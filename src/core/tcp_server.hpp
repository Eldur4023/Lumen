#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include <lumen/core/event_loop.hpp>
#include "../../include/lumen/types.hpp"

namespace lumen::core {

class TcpServer {
public:
    // max_connections: maximum simultaneous open connections (default 10 000).
    // Excess connections are immediately closed with 503.
    //
    // conn_count: optional shared counter — pass the same one to every
    // TcpServer instance so the limit is enforced globally across all threads.
    // If nullptr, a per-instance counter is created (single-thread behaviour).
    TcpServer(const std::string& host, uint16_t port,
              EventLoop& loop, lumen::DispatchFn dispatch,
              int max_connections = 10'000,
              std::shared_ptr<std::atomic<int>> conn_count = nullptr);
    ~TcpServer();

    // Stop accepting new connections without shutting down the event loop.
    // In-flight connections continue until they finish or are timed out.
    //
    // ONLY from its own loop's thread: it touches the handler map, which the
    // loop is reading in its epoll_wait.  From outside, use request_stop().
    void stop_accepting();

    // The same, requested from another thread.
    //
    // The orderly shutdown is triggered by the main loop's thread, but every
    // worker has its own loop and its own server: calling stop_accepting() on
    // them from outside was an erase concurrent with a read on the loop's
    // unordered_map, which is not reading a stale value but corrupting the
    // container, and on top of that while the connections were still draining.
    // It is queued on the right loop, which is the mechanism the rest of the
    // code already used.
    void pedir_parada();

private:
    int                listen_fd_ = -1;
    EventLoop&         loop_;
    lumen::DispatchFn dispatch_;
    int                max_connections_;

    // Shared between TcpServer and every HttpConnection so they can decrement
    // the counter on close without holding a pointer back to TcpServer.
    std::shared_ptr<std::atomic<int>> conn_count_;

    void on_accept();
};

} // namespace lumen::core
