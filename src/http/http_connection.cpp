#include "http_connection.hpp"
#include "../../include/lumen/request.hpp"
#include "../../include/lumen/response.hpp"
#include "../../include/lumen/task.hpp"
#include "../../include/lumen/metrics.hpp"
#include "../../include/lumen/logger.hpp"

#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <fstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <sstream>

namespace lumen::http {

// ── URL helpers ───────────────────────────────────────────────────────────────

static std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char buf[3] = {s[i+1], s[i+2], '\0'};
            char* endptr;
            unsigned long v = std::strtoul(buf, &endptr, 16);
            if (endptr == buf + 2) {
                // Reject %00 (null byte): it can bypass string comparisons
                // used for authorization checks (e.g. "\0admin" != "admin").
                if (v != 0) out += static_cast<char>(v);
                i += 2;
            } else {
                out += s[i];  // keep literal '%' for invalid hex sequences
            }
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static void parse_query(const std::string& qs,
                        std::unordered_map<std::string, std::string>& out) {
    if (qs.empty()) return;
    std::istringstream ss(qs);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos)
            out[url_decode(pair.substr(0, eq))] = url_decode(pair.substr(eq + 1));
        else if (!pair.empty())
            out[url_decode(pair)] = "";
    }
}

// ── HttpConnection ────────────────────────────────────────────────────────────

HttpConnection::HttpConnection(int fd, core::EventLoop& loop,
                               lumen::DispatchFn dispatch,
                               std::shared_ptr<std::atomic<int>> conn_count)
    : fd_(fd)
    , loop_(loop)
    , dispatch_(std::move(dispatch))
    , conn_count_(std::move(conn_count))
    , parser_([this](ParsedRequest req) { this->dispatch(std::move(req)); })
{}

void HttpConnection::start() {
    // Arm the header timeout — Slowloris defence.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    header_tfd_ = loop_.schedule_timer(kHeaderTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->header_tfd_ = -1;  // event loop already closed this tfd
                self->send_error(408, "Request Header Timeout");
                self->close();
            }
        }
    });
}

HttpConnection::~HttpConnection() {
    if (!closed_) {
        // Safe closure: we skip loop_.remove() to avoid re-entrant erase if
        // this destructor is called during EventLoop callbacks_ teardown.
        if (header_tfd_  >= 0) ::close(header_tfd_);
        if (timeout_tfd_ >= 0) ::close(timeout_tfd_);
        if (file_fd_     >= 0) ::close(file_fd_);
        ::close(fd_);
    }
}

// ── Event dispatch ────────────────────────────────────────────────────────────

void HttpConnection::on_event(uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) { close(); return; }

    // While write_buf_ has data, only EPOLLOUT is armed.
    // Once the buffer is drained, EPOLLIN is re-armed (see on_write_complete).
    if (events & EPOLLOUT) do_write();
    if (events & EPOLLIN)  do_read();
}

// ── Read path ─────────────────────────────────────────────────────────────────

void HttpConnection::do_read() {
    char buf[16384];
    while (!closed_) {
        ssize_t n;
        {
            n = ::read(fd_, buf, sizeof(buf));
            if (n == 0) { close(); return; }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR) continue;
                close(); return;
            }
        }

        // WebSocket mode: forward decrypted bytes to the WS frame parser.
        if (auto req = current_req_.lock(); req && req->_ws_on_data) {
            req->_ws_on_data(buf, static_cast<size_t>(n));
            continue;
        }

        // Pipelining: while a previous request is in flight the parser is
        // paused.  Buffer the bytes; on_write_complete replays them once the
        // current response has been written.
        if (in_flight_ || parser_.is_paused()) {
            if (pending_buf_.size() + static_cast<size_t>(n) > kMaxPendingBuf) {
                send_error(400, "Pipelined request too large");
                close();
                return;
            }
            pending_buf_.append(buf, static_cast<size_t>(n));
            continue;
        }

        in_parser_ = true;
        bool ok    = parser_.feed(buf, static_cast<size_t>(n));
        in_parser_ = false;
        if (!ok) {
            send_error(parser_.body_too_large() ? 413 : 400,
                       parser_.body_too_large() ? "Content Too Large" : "Bad Request");
            close();
            return;
        }

        // Parser pauses after each completed message — save any trailing
        // bytes that belong to the next pipelined request.
        if (parser_.is_paused()) {
            size_t un = parser_.unconsumed();
            if (un > 0) {
                if (un > kMaxPendingBuf) {
                    send_error(400, "Pipelined request too large");
                    close();
                    return;
                }
                pending_buf_.append(buf + static_cast<size_t>(n) - un, un);
            }
        }

        // El handler era sincrono y ya respondio dentro de feed(): ahora que la
        // pausa esta puesta, se puede cerrar el ciclo de verdad.
        if (cycle_pending_) {
            cycle_pending_ = false;
            finish_cycle();
            if (closed_) return;
        }
    }
}

// ── Dispatch ──────────────────────────────────────────────────────────────────

void HttpConnection::dispatch(ParsedRequest req_parsed) {
    // Mark the connection as busy until on_write_complete fires.  do_read()
    // will buffer further bytes rather than starting a second dispatch that
    // would race for write_buf_ / timeout_tfd_.
    in_flight_ = true;

    // Fresh cancellation token for this request.
    cancel_token_ = std::make_shared<lumen::CancellationToken>();

    // Set thread-locals so handlers can call sleep(ms) without explicit args.
    lumen::detail::current_loop  = &loop_;
    lumen::detail::current_token = cancel_token_;

    auto req_ptr = std::make_shared<lumen::Request>();
    auto res_ptr = std::make_shared<lumen::Response>();

    req_ptr->method       = req_parsed.method;
    req_ptr->path         = req_parsed.path;
    req_ptr->version      = req_parsed.version;
    req_ptr->headers      = std::move(req_parsed.headers);
    req_ptr->body         = std::move(req_parsed.body);
    req_ptr->loop         = &loop_;
    req_ptr->cancel_token = cancel_token_;
    req_ptr->_conn_fd     = fd_;

    // TLS-aware writer for SSE / WebSocket / any path that needs direct socket
    // I/O.  Captures `this` via shared_from_this so the fd stays valid
    // for the lifetime of the lambda.  After close(), fd_ is
    // -1, so calls degrade to write(-1) which returns EBADF cleanly.
    {
        auto self = shared_from_this();
        req_ptr->_raw_write = [self](const char* data, size_t len) -> ssize_t {
            if (self->closed_) { errno = EBADF; return -1; }
            return ::write(self->fd_, data, len);
        };
    }

    parse_query(req_parsed.query, req_ptr->query);

    // Keep a weak ref for WebSocket mode — do_read() routes through it.
    current_req_ = req_ptr;

    // Resolve remote IP from the socket
    sockaddr_storage ss{};
    socklen_t sslen = sizeof(ss);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&ss), &sslen) == 0) {
        char ipbuf[INET6_ADDRSTRLEN]{};
        if (ss.ss_family == AF_INET) {
            inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(&ss)->sin_addr,
                      ipbuf, sizeof(ipbuf));
        } else if (ss.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(&ss)->sin6_addr,
                      ipbuf, sizeof(ipbuf));
        }
        req_ptr->remote_ip = ipbuf;
    }

    // Headers fully received — cancel the Slowloris timer.
    loop_.cancel_timer(header_tfd_);
    header_tfd_ = -1;

    // ── Arm request timeout ───────────────────────────────────────────────────
    // If the handler + write don't complete within kRequestTimeoutMs, send 408.
    // cancel_timer() is called in on_write_complete() when everything succeeds.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    timeout_tfd_ = loop_.schedule_timer(kRequestTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->timeout_tfd_ = -1;  // event loop already closed this tfd
                self->send_error(408, "Request Timeout");
                self->close();
            }
        }
    });

    auto wrapper_task = [](std::shared_ptr<lumen::Request> req_ptr,
                           std::shared_ptr<lumen::Response> res_ptr,
                           lumen::DispatchFn disp) -> lumen::Task<void> {
        try {
            co_await disp(*req_ptr, *res_ptr);
        } catch (const std::exception& e) {
            // Log internally but do not expose e.what() to clients — it may
            // contain connection strings, file paths, or other internal detail.
            lumen::log().error("unhandled exception: ", e.what());
            res_ptr->status(500).json_text(R"({"error":"Internal Server Error"})");
        } catch (...) {
            res_ptr->status(500).json_text(R"({"error":"Internal Server Error"})");
        }
    }(req_ptr, res_ptr, dispatch_);

    auto h = wrapper_task.detach();
    h.promise().loop = &loop_;

    auto self = shared_from_this();
    h.promise().on_complete = [self, req_ptr, res_ptr]() {
        self->finish_dispatch(*req_ptr, *res_ptr);
    };

    h.resume();
}

void HttpConnection::finish_dispatch(lumen::Request& request,
                                     lumen::Response& response) {
    // Record the request in the global metrics counter.
    lumen::Metrics::instance().record(response.status_code());

    // Determine keep-alive before building the response
    keep_alive_ = (request.version == "HTTP/1.1");
    if (auto conn = request.header("connection")) {
        std::string val = *conn;
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        keep_alive_ = (val != "close");
    }
    response.header("Connection", keep_alive_ ? "keep-alive" : "close");

    // SSE / WebSocket mode: headers were already written directly.
    // Just close the connection — do not send a second response.
    if (response.sse_started() || response.ws_started()) {
        keep_alive_ = false;
        close();
        return;
    }

    // sendfile path — send headers with correct Content-Length, but no body for HEAD
    if (!response.sendfile_path().empty() && request.method == "HEAD") {
        send_response(response.build());
        return;
    }
    if (!response.sendfile_path().empty()) {
        // Non-TLS: open the file; do_sendfile() will stream it via sendfile(2).
        int fd = ::open(response.sendfile_path().c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            lumen::Response err;
            err.status(500).json_text(R"({"error":"Cannot open file"})");
            err.header("Connection", "close");
            keep_alive_ = false;
            send_response(err.build());
            return;
        }
        file_fd_        = fd;
        file_offset_    = 0;
        file_remaining_ = static_cast<size_t>(response.sendfile_size());
    }

    send_response(response.build());
}

// ── Write path ────────────────────────────────────────────────────────────────
//
// Strategy:
//   1. Try an immediate write(2).  If all bytes go through → done.
//   2. If EAGAIN or partial write → buffer the remainder, arm EPOLLOUT.
//      EPOLLIN is removed while writing so we don't try to parse a new
//      request before the current response is fully sent.
//   3. EPOLLOUT fires → do_write() drains the buffer.
//   4. Buffer empty → on_write_complete(): cancel timeout, handle keep-alive.

void HttpConnection::send_response(std::string data) {
    if (closed_) return;

    // Hard cap: if a single response exceeds kMaxResponseBytes, the client is
    // reading so slowly that buffering would exhaust RAM.  Close cleanly.
    if (data.size() > kMaxResponseBytes) {
        keep_alive_ = false;
        close();
        return;
    }

    // Try immediate write
    ssize_t n;
    {
        n = ::write(fd_, data.data(), data.size());
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(); return;
            }
            write_buf_    = std::move(data);
            write_offset_ = 0;
            loop_.modify(fd_, EPOLLOUT);
            return;
        }
    }

    {
        size_t written = static_cast<size_t>(n);
        if (written == data.size()) {
            // Headers sent in one shot — still need to stream the file body if pending.
            if (file_fd_ >= 0) { do_sendfile(); return; }
            on_write_complete();
            return;
        }
        // Partial write — keep the full string and advance the offset.
        write_buf_    = std::move(data);
        write_offset_ = written;
    }

    // Arm EPOLLOUT only; drop EPOLLIN until write completes
    loop_.modify(fd_, EPOLLOUT);
}

void HttpConnection::do_write() {
    while (write_offset_ < write_buf_.size()) {
        ssize_t n;
        {
            n = ::write(fd_,
                        write_buf_.data()  + write_offset_,
                        write_buf_.size()  - write_offset_);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    loop_.modify(fd_, EPOLLOUT);
                    return;
                }
                if (errno == EINTR) continue;
                close(); return;
            }
        }
        write_offset_ += static_cast<size_t>(n);
    }

    // All header/body bytes sent — reset buffer
    write_buf_.clear();
    write_offset_ = 0;

    // If a file is pending, stream it via sendfile(2)
    if (file_fd_ >= 0) { do_sendfile(); return; }

    on_write_complete();
}

void HttpConnection::do_sendfile() {
    // Zero-copy kernel transfer, plaintext only.
    while (file_remaining_ > 0) {
        ssize_t n = ::sendfile(fd_, file_fd_, &file_offset_,
                               std::min(file_remaining_, size_t{256 * 1024}));
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                loop_.modify(fd_, EPOLLOUT); // socket buffer full — resume when drains
                return;
            }
            if (errno == EINTR) continue;
            close(); return;
        }
        if (n == 0) break; // EOF
        file_remaining_ -= static_cast<size_t>(n);
    }

    ::close(file_fd_);
    file_fd_        = -1;
    file_offset_    = 0;
    file_remaining_ = 0;
    on_write_complete();
}

void HttpConnection::on_write_complete() {
    // Cancel the request timeout — response delivered successfully
    loop_.cancel_timer(timeout_tfd_);
    timeout_tfd_ = -1;
    in_flight_ = false;

    if (!keep_alive_) {
        close();
        return;
    }

    // Dentro de feed() la pausa todavia no existe: aplazar el cierre de ciclo.
    if (in_parser_) { cycle_pending_ = true; return; }
    finish_cycle();
}

void HttpConnection::finish_cycle() {
    // Pipelining: drain any buffered bytes that belong to the next request.
    // Resume the parser, feed the saved bytes, and let the resulting dispatch
    // start a fresh response cycle.  We deliberately avoid re-arming EPOLLIN
    // here when a new dispatch fires — the next on_write_complete will do it.
    if (parser_.is_paused()) parser_.resume();
    if (!pending_buf_.empty()) {
        std::string buf;
        buf.swap(pending_buf_);
        if (!parser_.feed(buf.data(), buf.size())) {
            send_error(parser_.body_too_large() ? 413 : 400,
                       parser_.body_too_large() ? "Content Too Large" : "Bad Request");
            close();
            return;
        }
        if (parser_.is_paused()) {
            size_t un = parser_.unconsumed();
            if (un > 0) {
                if (un > kMaxPendingBuf) {
                    send_error(400, "Pipelined request too large");
                    close();
                    return;
                }
                pending_buf_.assign(buf.data() + buf.size() - un, un);
            }
        }
        // dispatch() inside feed() flipped in_flight_ back on; defer the
        // EPOLLIN re-arm and the Slowloris timer until that request's write
        // completes.
        if (in_flight_) return;
    }

    // Re-arm the header timeout for the next pipelined/keep-alive request.
    // Without this, a client that sends headers slowly on the second request
    // (Slowloris) would go unchecked — the 5s timer only ran for the first one.
    auto self_weak = std::weak_ptr<HttpConnection>(shared_from_this());
    header_tfd_ = loop_.schedule_timer(kHeaderTimeoutMs, [self_weak]() {
        if (auto self = self_weak.lock()) {
            if (!self->closed_) {
                self->header_tfd_ = -1;  // event loop already closed this tfd
                self->send_error(408, "Request Header Timeout");
                self->close();
            }
        }
    });

    // Ready for the next request
    loop_.modify(fd_, EPOLLIN);
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void HttpConnection::send_error(int code, const char* msg) {
    lumen::Response r;
    // El mensaje viene de una lista fija del motor, sin comillas ni barras.
    r.status(code).json_text(std::string(R"({"error":")") + msg + R"("})");
    r.header("Connection", "close");
    // send_error is only called for protocol-level errors; ignore keep-alive
    keep_alive_ = false;
    send_response(r.build());
}

void HttpConnection::close() {
    if (closed_) return;
    closed_ = true;

    // Signal any suspended coroutines (sleep, ws.recv()) to wake up and exit.
    if (cancel_token_) cancel_token_->cancel();

    // Drop the data callback so no further bytes get dispatched.  The
    // shared_ptr<WSState> captured inside the closure stays alive via the
    // running handler coroutine until that coroutine observes the cancellation
    // and unwinds.
    if (auto req = current_req_.lock(); req && req->_ws_on_data) {
        req->_ws_on_data = nullptr;
    }
    current_req_.reset();

    loop_.cancel_timer(header_tfd_);
    header_tfd_ = -1;
    loop_.cancel_timer(timeout_tfd_);
    timeout_tfd_ = -1;
    loop_.remove(fd_);

    // TLS shutdown: best-effort (non-blocking); we close the fd regardless.

    ::close(fd_);
    if (file_fd_ >= 0) { ::close(file_fd_); file_fd_ = -1; }
    if (conn_count_) conn_count_->fetch_sub(1, std::memory_order_relaxed);
}

} // namespace lumen::http
