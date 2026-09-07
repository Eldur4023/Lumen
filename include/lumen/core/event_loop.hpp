#pragma once
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <atomic>
#include <mutex>

namespace lumen::core {

// ── EpollLoop ─────────────────────────────────────────────────────────────────
//
// Default event loop backend — uses epoll + timerfd + eventfd.
// All existing code refers to this as "EventLoop" via the alias below.

class EpollLoop {
public:
    EpollLoop();
    ~EpollLoop();

    using Callback = std::function<void(uint32_t events)>;

    void add   (int fd, uint32_t events, Callback cb);
    void modify(int fd, uint32_t events);
    void remove(int fd);

    void post(std::function<void()> cb);

    int  schedule_timer(int ms, std::function<void()> cb);
    void cancel_timer  (int tfd);

    void run();
    void stop();

private:
    void process_tasks();

    int  epoll_fd_  = -1;
    int  wakeup_fd_ = -1;
    // Atomico porque stop() lo escribe desde OTRO hilo —el del apagado— y el
    // loop lo lee en cada vuelta.  Como bool corriente era una carrera, aunque
    // en la practica el compilador no llegara a morderla.
    std::atomic<bool> running_{false};

    std::unordered_map<int, Callback> callbacks_;
    std::vector<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
};

} // namespace lumen::core

// ── Backend alias ─────────────────────────────────────────────────────────────
//
// When LUMEN_IO_URING is defined, EventLoop resolves to IoUringLoop.
// All connection and server code uses core::EventLoop without any changes.

#ifdef LUMEN_IO_URING
#  include "io_uring_loop.hpp"
   namespace lumen::core { using EventLoop = IoUringLoop; }
#else
   namespace lumen::core { using EventLoop = EpollLoop; }
#endif
