#pragma once
#include <string>
#include <string_view>
#include <memory>
#include <unistd.h>
#include <cerrno>
#include "cancel.hpp"
#include "request.hpp"
#include "response.hpp"

namespace lumen {

// ─── SSEWriter ────────────────────────────────────────────────────────────────
//
// Keeps an HTTP connection open and pushes text/event-stream events to the
// client.  Obtained via res.sse(req) — see Response::sse().
//
// Lifetime: headers are sent immediately by sse(); the writer remains valid
// until the client disconnects or the handler returns.  When the client
// disconnects, epoll fires EPOLLHUP → HttpConnection::close() → the shared
// CancellationToken is cancelled → is_open() returns false → the handler
// loop should exit naturally.
//
// Usage:
//   app.get("/events", [](Request& req, Response& res) -> Task<void> {
//       auto sse = res.sse(req);
//       int n = 0;
//       while (sse.is_open()) {
//           sse.send(std::to_string(n++));
//           co_await lumen::sleep(1000);
//       }
//   });
//
class SSEWriter {
public:
    // Writes through req._raw_write, bypassing the buffered response pipeline
    // so each event goes out as soon as it is produced.
    SSEWriter(std::function<ssize_t(const char*, size_t)> writer,
              std::shared_ptr<CancellationToken> token)
        : writer_(std::move(writer)), token_(std::move(token)) {}

    SSEWriter(const SSEWriter&)            = delete;
    SSEWriter& operator=(const SSEWriter&) = delete;
    SSEWriter(SSEWriter&& o) noexcept
        : writer_(std::move(o.writer_)), token_(std::move(o.token_))
    {}
    SSEWriter& operator=(SSEWriter&&) = delete;

    // Send a "data: <text>\n\n" event.
    // Returns false if the connection is gone.
    bool send(std::string_view data) {
        return write_frame("", data, "");
    }

    // Send a named event: "event: <type>\ndata: <text>\n\n"
    // Optionally attach a last-event ID for browser auto-reconnect.
    bool send_event(std::string_view event_type, std::string_view data,
                    std::string_view id = "") {
        return write_frame(event_type, data, id);
    }

    // Send a comment (": <text>\n\n") — browsers ignore it; useful as a keepalive.
    bool ping(std::string_view comment = "") {
        std::string frame = ": ";
        frame += comment;
        frame += "\n\n";
        return raw_write(frame);
    }

    // True while the underlying connection is alive.
    bool is_open() const { return token_ && !token_->is_cancelled(); }

private:
    std::function<ssize_t(const char*, size_t)> writer_;
    std::shared_ptr<CancellationToken>          token_;

    // Strip CR/LF from a string_view to prevent SSE field injection.
    static std::string sanitize_field(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) if (c != '\r' && c != '\n') out += c;
        return out;
    }

    bool write_frame(std::string_view event, std::string_view data, std::string_view id) {
        if (!is_open()) return false;
        std::string frame;
        frame.reserve(data.size() + 48);
        if (!id.empty())    { frame += "id: ";    frame += sanitize_field(id);    frame += "\n"; }
        if (!event.empty()) { frame += "event: "; frame += sanitize_field(event); frame += "\n"; }
        // RFC 8895 §3.2: each line of multiline data needs its own "data:" prefix.
        std::string_view rem = data;
        while (true) {
            auto nl = rem.find('\n');
            frame += "data: ";
            frame += rem.substr(0, nl);
            frame += "\n";
            if (nl == std::string_view::npos) break;
            rem = rem.substr(nl + 1);
        }
        frame += "\n";  // blank line ends the event
        return raw_write(frame);
    }

    // Best-effort write.  EAGAIN = socket buffer full → drops this event (lossy
    // but non-fatal: the client will receive the next one).  Any other error
    // means the connection is gone; the epoll EPOLLHUP will fire shortly and
    // cancel the token — is_open() will return false on the next loop check.
    bool raw_write(const std::string& frame) {
        if (!is_open()) return false;

        // EAGAIN on the first byte → drop this event (no bytes sent yet, stream intact).
        // EAGAIN after a partial write → stream is corrupted; treat as fatal.
        if (!writer_) return false;
        size_t written = 0;
        while (written < frame.size()) {
            ssize_t n = writer_(frame.data() + written, frame.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return written == 0;  // true=clean drop; false=partial write, close
                return false;
            }
            if (n == 0) return false;
            written += static_cast<size_t>(n);
        }
        return true;
    }
};

// ─── res.sse(req) — convenience free function ─────────────────────────────────
//
// Writes SSE headers to the socket immediately (bypassing the normal response
// pipeline) and marks the Response so finish_dispatch knows not to send again.
//
// Must be the first I/O operation on the response for this request.

inline SSEWriter make_sse(Response& res, const Request& req) {
    // Set SSE-specific headers (user may override Content-Type before calling)
    res.status(200)
       .header("Content-Type",     "text/event-stream")
       .header("Cache-Control",    "no-cache")
       .header("X-Accel-Buffering","no");  // disable nginx / proxy buffering

    res.header("Connection", "keep-alive");

    // Write the headers straight to the socket, bypassing write_buf_, so they
    // go out before the first event is produced.
    std::string headers = res.build_sse_headers();
    if (req._raw_write) {
        size_t written = 0;
        while (written < headers.size()) {
            ssize_t n = req._raw_write(headers.data() + written,
                                       headers.size() - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;  // connection gone; SSEWriter::is_open() → false
            }
            if (n == 0) break;
            written += static_cast<size_t>(n);
        }
    }

    // Tell finish_dispatch that headers are already out
    res.mark_sse_started();

    return SSEWriter(req._raw_write, req.cancel_token);
}

} // namespace lumen
