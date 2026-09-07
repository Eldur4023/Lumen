#include <lumen/core/io_uring_loop.hpp>

// Only compiled when LUMEN_IO_URING is defined (see CMakeLists.txt).
#ifdef LUMEN_IO_URING

#include <linux/io_uring.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <atomic>

// ── Raw syscall wrappers ──────────────────────────────────────────────────────

static long io_uring_setup(unsigned entries, struct io_uring_params* params) {
    return syscall(__NR_io_uring_setup, entries, params);
}

static long io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                            unsigned flags, void* sig) {
    return syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, sig, 0);
}

// ── SQE / CQE layout helpers ─────────────────────────────────────────────────
//
// We address the io_uring_sqe array by index.  The struct is 64 bytes for
// normal rings (we don't set IORING_SETUP_SQE128).

static inline struct io_uring_sqe* sqe_at(void* sqes_base, uint32_t index) {
    return reinterpret_cast<struct io_uring_sqe*>(
        static_cast<uint8_t*>(sqes_base) + index * sizeof(struct io_uring_sqe));
}

static inline struct io_uring_cqe* cqe_at(void* cqes_base, uint32_t index) {
    return reinterpret_cast<struct io_uring_cqe*>(
        static_cast<uint8_t*>(cqes_base) + index * sizeof(struct io_uring_cqe));
}

// Memory barrier helpers
static inline void smp_store_release_u32(uint32_t* ptr, uint32_t val) {
    std::atomic_store_explicit(
        reinterpret_cast<std::atomic<uint32_t>*>(ptr), val,
        std::memory_order_release);
}
static inline uint32_t smp_load_acquire_u32(uint32_t* ptr) {
    return std::atomic_load_explicit(
        reinterpret_cast<std::atomic<uint32_t>*>(ptr),
        std::memory_order_acquire);
}

namespace lumen::core {

// ── Constructor / Destructor ──────────────────────────────────────────────────

IoUringLoop::IoUringLoop() {
    struct io_uring_params params{};
    // No extra flags — default (interrupt-driven) mode, normal SQE/CQE sizes.
    ring_fd_ = static_cast<int>(io_uring_setup(256, &params));
    if (ring_fd_ < 0)
        throw std::runtime_error(std::string("io_uring_setup: ") + strerror(errno));

    // ── mmap the three regions ─────────────────────────────────────────────
    // 1. SQ ring (may also host CQ ring when IORING_FEAT_SINGLE_MMAP is set)
    sq_ring_sz_ = params.sq_off.array +
                  params.sq_entries * sizeof(uint32_t);
    cq_ring_sz_ = params.cq_off.cqes +
                  params.cq_entries * sizeof(struct io_uring_cqe);

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        if (cq_ring_sz_ > sq_ring_sz_) sq_ring_sz_ = cq_ring_sz_;
        cq_ring_sz_ = sq_ring_sz_;
    }

    sq_ring_ = static_cast<uint8_t*>(mmap(nullptr, sq_ring_sz_,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        ring_fd_, IORING_OFF_SQ_RING));
    if (sq_ring_ == MAP_FAILED)
        throw std::runtime_error(std::string("mmap SQ ring: ") + strerror(errno));

    if (params.features & IORING_FEAT_SINGLE_MMAP) {
        cq_ring_ = sq_ring_;
    } else {
        cq_ring_ = static_cast<uint8_t*>(mmap(nullptr, cq_ring_sz_,
            PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
            ring_fd_, IORING_OFF_CQ_RING));
        if (cq_ring_ == MAP_FAILED)
            throw std::runtime_error(std::string("mmap CQ ring: ") + strerror(errno));
    }

    // 2. SQE array
    sqes_sz_ = params.sq_entries * sizeof(struct io_uring_sqe);
    sqes_map_ = mmap(nullptr, sqes_sz_,
        PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
        ring_fd_, IORING_OFF_SQES);
    if (sqes_map_ == MAP_FAILED)
        throw std::runtime_error(std::string("mmap SQEs: ") + strerror(errno));
    sqes_ = sqes_map_;

    // ── Wire up ring pointers ──────────────────────────────────────────────
    sq_head_          = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.head);
    sq_tail_          = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.tail);
    sq_ring_mask_     = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.ring_mask);
    sq_ring_entries_  = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.ring_entries);
    sq_flags_         = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.flags);
    sq_dropped_       = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.dropped);
    sq_array_         = reinterpret_cast<uint32_t*>(sq_ring_ + params.sq_off.array);

    cq_head_          = reinterpret_cast<uint32_t*>(cq_ring_ + params.cq_off.head);
    cq_tail_          = reinterpret_cast<uint32_t*>(cq_ring_ + params.cq_off.tail);
    cq_ring_mask_     = reinterpret_cast<uint32_t*>(cq_ring_ + params.cq_off.ring_mask);
    cq_ring_entries_  = reinterpret_cast<uint32_t*>(cq_ring_ + params.cq_off.ring_entries);
    cq_cqes_          = cq_ring_ + params.cq_off.cqes;

    // ── eventfd for wakeup (token 0) ───────────────────────────────────────
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw std::runtime_error(std::string("eventfd: ") + strerror(errno));

    // Register wakeup fd with multishot poll, token = 0
    submit_poll(wakeup_fd_, POLLIN, 0);
    flush_sqes();
}

IoUringLoop::~IoUringLoop() {
    if (sqes_map_ && sqes_map_ != MAP_FAILED) munmap(sqes_map_, sqes_sz_);
    if (cq_ring_ && cq_ring_ != MAP_FAILED && cq_ring_ != sq_ring_)
        munmap(cq_ring_, cq_ring_sz_);
    if (sq_ring_ && sq_ring_ != MAP_FAILED) munmap(sq_ring_, sq_ring_sz_);
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    if (ring_fd_   >= 0) ::close(ring_fd_);
}

// ── submit_poll ───────────────────────────────────────────────────────────────
//
// Enqueues one IORING_OP_POLL_ADD SQE into the SQ.
// IORING_POLL_ADD_MULTI keeps it alive for multiple events (like edge-triggered
// epoll with EPOLLONESHOT cleared).

bool IoUringLoop::submit_poll(int fd, uint32_t events, uint64_t token) {
    uint32_t tail   = *sq_tail_;
    uint32_t head   = smp_load_acquire_u32(sq_head_);
    uint32_t mask   = *sq_ring_mask_;
    uint32_t avail  = *sq_ring_entries_ - (tail - head);
    if (avail == 0) {
        std::cerr << "io_uring SQ full, dropping poll for fd=" << fd << '\n';
        return false;
    }

    uint32_t idx = tail & mask;
    struct io_uring_sqe* sqe = sqe_at(sqes_, idx);
    memset(sqe, 0, sizeof(*sqe));

    sqe->opcode       = IORING_OP_POLL_ADD;
    sqe->fd           = fd;
    sqe->addr         = 0;
    sqe->len          = 0;
    sqe->poll_events  = static_cast<uint16_t>(events);
    sqe->user_data    = token;
    // IORING_POLL_ADD_MULTI — persistent; produces CQEs with IORING_CQE_F_MORE
    sqe->len          = IORING_POLL_ADD_MULTI;

    sq_array_[idx] = idx;
    smp_store_release_u32(sq_tail_, tail + 1);
    return true;
}

// ── submit_poll_remove ────────────────────────────────────────────────────────

void IoUringLoop::submit_poll_remove(uint64_t token) {
    uint32_t tail   = *sq_tail_;
    uint32_t head   = smp_load_acquire_u32(sq_head_);
    uint32_t mask   = *sq_ring_mask_;
    uint32_t avail  = *sq_ring_entries_ - (tail - head);
    if (avail == 0) return;

    uint32_t idx = tail & mask;
    struct io_uring_sqe* sqe = sqe_at(sqes_, idx);
    memset(sqe, 0, sizeof(*sqe));

    sqe->opcode    = IORING_OP_POLL_REMOVE;
    sqe->fd        = 0;
    sqe->addr      = token;   // user_data of the SQE to remove
    sqe->user_data = ~token;  // distinct token so we can ignore its CQE

    sq_array_[idx] = idx;
    smp_store_release_u32(sq_tail_, tail + 1);
}

// ── flush_sqes ────────────────────────────────────────────────────────────────

void IoUringLoop::flush_sqes() {
    uint32_t tail = *sq_tail_;
    uint32_t head = smp_load_acquire_u32(sq_head_);
    uint32_t to_submit = tail - head;
    if (to_submit == 0) return;
    io_uring_enter(ring_fd_, to_submit, 0, 0, nullptr);
}

// ── Public interface ──────────────────────────────────────────────────────────

void IoUringLoop::add(int fd, uint32_t events, Callback cb) {
    uint64_t token = alloc_token();
    entries_[token] = PollEntry{fd, events, std::move(cb)};
    fd_token_[fd]   = token;
    submit_poll(fd, events, token);
    flush_sqes();
}

void IoUringLoop::modify(int fd, uint32_t events) {
    auto it = fd_token_.find(fd);
    if (it == fd_token_.end()) return;

    uint64_t old_token = it->second;
    auto entry_it = entries_.find(old_token);
    if (entry_it == entries_.end()) return;

    // Remove old poll
    submit_poll_remove(old_token);

    // Re-register with new events under a fresh token
    uint64_t new_token = alloc_token();
    entry_it->second.events = events;

    // Move entry to new token
    entries_[new_token] = std::move(entry_it->second);
    entries_.erase(old_token);
    fd_token_[fd] = new_token;

    submit_poll(fd, events, new_token);
    flush_sqes();
}

void IoUringLoop::remove(int fd) {
    auto it = fd_token_.find(fd);
    if (it == fd_token_.end()) return;
    submit_poll_remove(it->second);
    entries_.erase(it->second);
    fd_token_.erase(it);
    flush_sqes();
}

void IoUringLoop::post(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push_back(std::move(cb));
    }
    uint64_t val = 1;
    (void)::write(wakeup_fd_, &val, sizeof(val));
}

void IoUringLoop::process_tasks() {
    std::vector<std::function<void()>> current;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current.swap(task_queue_);
    }
    for (auto& t : current)
        if (t) t();
}

// ── run ───────────────────────────────────────────────────────────────────────

void IoUringLoop::run() {
    running_ = true;

    while (running_) {
        process_tasks();

        // Wait for at least one CQE
        long ret = io_uring_enter(ring_fd_, 0, 1, IORING_ENTER_GETEVENTS, nullptr);
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0) {
            std::cerr << "io_uring_enter: " << strerror(errno) << '\n';
            break;
        }

        // Drain CQ
        uint32_t head = *cq_head_;
        uint32_t tail = smp_load_acquire_u32(cq_tail_);
        uint32_t mask = *cq_ring_mask_;

        while (head != tail) {
            struct io_uring_cqe* cqe = cqe_at(cq_cqes_, head & mask);
            uint64_t token = cqe->user_data;
            int32_t  res   = cqe->res;
            uint32_t flags = cqe->flags;
            head++;

            // Token 0 = wakeup eventfd
            if (token == 0) {
                uint64_t val;
                (void)::read(wakeup_fd_, &val, sizeof(val));
                // Re-arm if IORING_CQE_F_MORE not set (ring may have removed it)
                if (!(flags & IORING_CQE_F_MORE)) {
                    submit_poll(wakeup_fd_, POLLIN, 0);
                    flush_sqes();
                }
                continue;
            }

            // POLL_REMOVE ack — token is ~original; ignore it
            // (We detect this because submit_poll_remove uses ~token as user_data)
            // All tokens from alloc_token() start at 1 and never wrap to
            // the complement of a valid token within a session, so a simple
            // lookup suffices: if not found → stale or remove-ack → skip.
            auto it = entries_.find(token);
            if (it == entries_.end()) continue;

            // Convert io_uring poll result bits to epoll-style events
            uint32_t ev = 0;
            if (res & POLLIN)  ev |= EPOLLIN;
            if (res & POLLOUT) ev |= EPOLLOUT;
            if (res & POLLHUP) ev |= EPOLLHUP;
            if (res & POLLERR) ev |= EPOLLERR;

            // Make a copy so callback can call remove() / modify() safely
            Callback cb = it->second.cb;

            // If the multishot was cancelled (no MORE flag) and the fd is
            // still registered, re-arm it.
            bool rearm = !(flags & IORING_CQE_F_MORE) && (fd_token_.count(it->second.fd));

            cb(ev);

            if (rearm) {
                auto it2 = fd_token_.find(it->second.fd);
                if (it2 != fd_token_.end() && it2->second == token) {
                    // Still the same token — re-arm
                    submit_poll(it->second.fd, it->second.events, token);
                    flush_sqes();
                }
            }
        }

        // Advance CQ head
        smp_store_release_u32(cq_head_, head);
    }
}

// ── Timers — identical to EpollLoop (timerfd) ─────────────────────────────────

int IoUringLoop::schedule_timer(int ms, std::function<void()> cb) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) {
        post(std::move(cb));
        return -1;
    }

    struct itimerspec ts{};
    if (ms > 0) {
        ts.it_value.tv_sec  = ms / 1000;
        ts.it_value.tv_nsec = static_cast<long>(ms % 1000) * 1'000'000L;
    } else {
        ts.it_value.tv_nsec = 1;
    }
    timerfd_settime(tfd, 0, &ts, nullptr);

    add(tfd, EPOLLIN, [this, tfd, cb = std::move(cb)](uint32_t) mutable {
        uint64_t val;
        (void)::read(tfd, &val, sizeof(val));
        remove(tfd);
        ::close(tfd);
        cb();
    });
    return tfd;
}

void IoUringLoop::cancel_timer(int tfd) {
    if (tfd < 0) return;
    remove(tfd);
    ::close(tfd);
}

void IoUringLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    (void)::write(wakeup_fd_, &val, sizeof(val));
}

} // namespace lumen::core

#endif // LUMEN_IO_URING
