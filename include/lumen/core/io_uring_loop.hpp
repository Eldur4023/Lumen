#pragma once
#include <functional>
#include <unordered_map>
#include <cstdint>
#include <vector>
#include <mutex>

namespace lumen::core {

// ── IoUringLoop ───────────────────────────────────────────────────────────────
//
// io_uring-based event loop backend.  Same public interface as EpollLoop.
// Enabled at compile time with -DLUMEN_IO_URING.
//
// Implementation notes:
//  • Uses IORING_POLL_ADD_MULTI (persistent multishot poll) — one SQE per fd,
//    multiple CQEs without re-submitting.
//  • Timers still use timerfd — simpler than IORING_OP_TIMEOUT and avoids the
//    one-shot vs multishot complexity.
//  • Wakeup (post/stop) uses eventfd, same as EpollLoop.
//  • SQE user_data carries a 64-bit token.  Token 0 is reserved for the wakeup
//    eventfd.  fd_token_ maps fd → current token so stale CQEs can be ignored.
//  • modify() / remove() issue a IORING_OP_POLL_REMOVE then re-submit if needed.
//  • Raw syscalls only — no liburing dependency.

class IoUringLoop {
public:
    IoUringLoop();
    ~IoUringLoop();

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
    // Ring file descriptor and mapped rings
    int      ring_fd_   = -1;
    int      wakeup_fd_ = -1;
    bool     running_   = false;

    // Mapped io_uring regions
    uint8_t* sq_ring_   = nullptr;   // mmap'd submission ring
    uint8_t* cq_ring_   = nullptr;   // mmap'd completion ring (may alias sq_ring_)
    void*    sqes_map_  = nullptr;   // mmap'd SQE array
    size_t   sq_ring_sz_  = 0;
    size_t   cq_ring_sz_  = 0;
    size_t   sqes_sz_     = 0;

    // SQ ring offsets (filled from io_sqring_offsets)
    uint32_t* sq_head_    = nullptr;
    uint32_t* sq_tail_    = nullptr;
    uint32_t* sq_ring_mask_ = nullptr;
    uint32_t* sq_ring_entries_ = nullptr;
    uint32_t* sq_flags_   = nullptr;
    uint32_t* sq_dropped_ = nullptr;
    uint32_t* sq_array_   = nullptr;

    // CQ ring offsets
    uint32_t* cq_head_    = nullptr;
    uint32_t* cq_tail_    = nullptr;
    uint32_t* cq_ring_mask_ = nullptr;
    uint32_t* cq_ring_entries_ = nullptr;
    void*     cq_cqes_    = nullptr;

    // SQE array pointer (64-byte structs)
    void*     sqes_       = nullptr;

    struct PollEntry {
        int      fd;
        uint32_t events;
        Callback cb;
    };

    uint64_t next_token_ = 1;   // token 0 = wakeup_fd_
    std::unordered_map<uint64_t, PollEntry> entries_;  // token → entry
    std::unordered_map<int, uint64_t>       fd_token_; // fd → active token

    std::vector<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;

    void     process_tasks();
    bool     submit_poll   (int fd, uint32_t events, uint64_t token);
    void     submit_poll_remove(uint64_t token);
    void     flush_sqes    ();
    uint64_t alloc_token   () { return next_token_++; }
};

} // namespace lumen::core
