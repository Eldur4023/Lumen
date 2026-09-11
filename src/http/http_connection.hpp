#pragma once
#include <string>
#include <memory>
#include <atomic>
#include <cstdint>
#include "http_parser.hpp"
#include <lumen/core/event_loop.hpp>
#include "../../include/lumen/types.hpp"
#include "../../include/lumen/cancel.hpp"

namespace lumen::http {

class HttpConnection : public std::enable_shared_from_this<HttpConnection> {
public:
    HttpConnection(int fd, core::EventLoop& loop, lumen::DispatchFn dispatch,
                   std::shared_ptr<std::atomic<int>> conn_count = nullptr);
    ~HttpConnection();

    void start();
    void on_event(uint32_t events);

private:
    int                fd_;
    core::EventLoop&   loop_;
    lumen::DispatchFn dispatch_;
    std::shared_ptr<std::atomic<int>>          conn_count_;   // decremented on close()
    std::shared_ptr<lumen::CancellationToken> cancel_token_; // one per request
    HttpParser         parser_;
    bool               closed_         = false;

    // Weak reference to the current request — used in WebSocket mode to route
    // do_read() bytes into the WS frame parser instead of the HTTP parser.
    std::weak_ptr<lumen::Request> current_req_;

    // ── Response buffer limit ─────────────────────────────────────────────────
    // Hard cap on the size of a single response.  Connections that exceed this
    // are closed to prevent unbounded RAM growth from slow-reading clients.
    static constexpr size_t kMaxResponseBytes = 16 * 1024 * 1024; // 16 MB

    // ── Write buffer ─────────────────────────────────────────────────────────
    // Non-blocking writes: if send buffer is full (EAGAIN), data is queued here
    // and flushed when EPOLLOUT fires.  Using an offset avoids O(n) erases.
    std::string write_buf_;
    size_t      write_offset_ = 0;
    bool        keep_alive_   = false;  // stored here so on_write_complete can act

    // ── Pipelining serialisation ─────────────────────────────────────────────
    // While a request's response is being produced and written, no second
    // dispatch may run on the same connection (write_buf_ is shared, timers
    // are per-connection).  The parser is paused on each message_complete;
    // any trailing bytes that arrived in the same TCP segment are saved here
    // and replayed in on_write_complete once the previous response is out.
    // pending_buf_ is capped to bound memory under a buggy/abusive pipeliner.
    std::string                pending_buf_;
    bool                       in_flight_ = false;
    static constexpr size_t    kMaxPendingBuf = 64 * 1024;  // 64 KB pipelined

    // Un handler sincrono responde DENTRO de parser_.feed(): llhttp llama a
    // on_message_complete, which dispatches, and the response is written in
    // full before the callback returns HPE_PAUSED.  If the cycle close ran
    // there, it would resume() a pause that does not exist yet and the
    // connection would stay paused forever.  So it is deferred until feed()
    // returns and the pause is in place.
    bool in_parser_     = false;
    bool cycle_pending_ = false;

    // ── Timeouts ──────────────────────────────────────────────────────────────
    // kHeaderTimeoutMs: armed at construction; fires 408 if complete headers are
    //   not received within this window (Slowloris defence).
    //   Cancelled in dispatch() once headers are fully parsed.
    // kRequestTimeoutMs: armed in dispatch(); fires 408 if handler + write take
    //   too long.  Cancelled in on_write_complete().
    static constexpr int kHeaderTimeoutMs  = 5'000;
    static constexpr int kRequestTimeoutMs = 30'000;
    int header_tfd_  = -1;
    int timeout_tfd_ = -1;

    // ── sendfile state ────────────────────────────────────────────────────────
    // When serving static files, we skip the read-into-buffer step and stream
    // directly from the file descriptor to the socket.  The connection sends
    // the HTTP headers via the normal write_buf_ path, then transitions to
    // do_sendfile() once the headers are fully flushed.
    int    file_fd_        = -1;
    off_t  file_offset_    = 0;
    size_t file_remaining_ = 0;

    // ── WebSocket outbound queue ────────────────────────────────────────────
    // Once the handshake completes, individual frames are written straight to
    // the socket (queue_ws_write()) instead of going through write_buf_ / the
    // request-response cycle — a WS session has no "one response" shape and
    // is full-duplex, so on_write_complete()'s keep-alive/timeout bookkeeping
    // does not apply to it.
    //
    // A write that would block is buffered here and drained by do_ws_write()
    // on EPOLLOUT, exactly like write_buf_ is for HTTP responses — a frame
    // either reaches the wire whole or the connection closes, it is never
    // torn in the middle.  Capped so a peer that stops reading (deliberately
    // or not) cannot grow this without bound; the cap sits at kMaxResponseBytes
    // so one legitimate large ws.send() still fits without fragmenting.
    std::string ws_write_buf_;
    size_t      ws_write_offset_ = 0;

    void do_read();
    void do_write();
    void do_ws_write();
    void queue_ws_write(std::string frame);
    void do_sendfile();
    void on_write_complete();

    // Response cycle close: resumes the parser, replays anything that arrived
    // by pipelining, rearms the header timer and EPOLLIN.
    void finish_cycle();

    // Begin writing `data`; buffers any unsent remainder and arms EPOLLOUT.
    void send_response(std::string data);
    void send_error(int code, const char* msg);
    void close();

    void dispatch(ParsedRequest req);
    void finish_dispatch(lumen::Request& request, lumen::Response& response);
};

} // namespace lumen::http
