#include <lumen/core/event_loop.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <iostream>

namespace lumen::core {

EpollLoop::EpollLoop() {
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error(std::string("epoll_create1: ") + strerror(errno));

    // eventfd used to wake up epoll_wait from stop()
    wakeup_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd_ < 0)
        throw std::runtime_error(std::string("eventfd: ") + strerror(errno));

    epoll_event ev{};
    ev.events   = EPOLLIN;
    ev.data.fd  = wakeup_fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wakeup_fd_, &ev);
}

EpollLoop::~EpollLoop() {
    if (wakeup_fd_ >= 0) ::close(wakeup_fd_);
    if (epoll_fd_  >= 0) ::close(epoll_fd_);
}

void EpollLoop::add(int fd, uint32_t events, Callback cb) {
    callbacks_[fd] = std::move(cb);

    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
        callbacks_.erase(fd);
        std::cerr << "epoll_ctl ADD fd=" << fd << ": " << strerror(errno) << '\n';
    }
}

void EpollLoop::modify(int fd, uint32_t events) {
    epoll_event ev{};
    ev.events  = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EpollLoop::remove(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    callbacks_.erase(fd);
}

void EpollLoop::post(std::function<void()> cb) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        task_queue_.push_back(std::move(cb));
    }
    uint64_t val = 1;
    (void)write(wakeup_fd_, &val, sizeof(val));
}

void EpollLoop::process_tasks() {
    std::vector<std::function<void()>> current_tasks;
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        current_tasks.swap(task_queue_);
    }
    for (auto& task : current_tasks) {
        if (task) task();
    }
}

void EpollLoop::run() {
    running_ = true;
    constexpr int kMaxEvents = 64;
    epoll_event events[kMaxEvents];

    while (running_) {
        // We process tasks before waiting to handle anything posted before the loop
        process_tasks();
        
        int n = epoll_wait(epoll_fd_, events, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            std::cerr << "epoll_wait: " << strerror(errno) << '\n';
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;

            // Wakeup fd → this will trigger next iteration to process_tasks()
            if (fd == wakeup_fd_) {
                uint64_t val;
                (void)read(wakeup_fd_, &val, sizeof(val));
                continue;
            }

            auto it = callbacks_.find(fd);
            if (it == callbacks_.end()) continue;

            // Make a local copy so the callback isn't destroyed if it
            // calls remove() on itself (e.g. connection close).
            Callback cb = it->second;
            cb(events[i].events);
        }
    }
}

int EpollLoop::schedule_timer(int ms, std::function<void()> cb) {
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

void EpollLoop::cancel_timer(int tfd) {
    if (tfd < 0) return;
    remove(tfd);
    ::close(tfd);
}

void EpollLoop::stop() {
    running_ = false;
    uint64_t val = 1;
    (void)write(wakeup_fd_, &val, sizeof(val));
}

} // namespace lumen::core
