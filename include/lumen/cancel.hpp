#pragma once
#include <atomic>
#include <functional>
#include <memory>

namespace lumen {

// ─── CancellationToken ────────────────────────────────────────────────────────
//
// Shared between an HttpConnection and every Request it spawns.
// When the connection closes, cancel() is called from the event loop thread.
//
// If a coroutine is currently suspended inside co_await sleep(), a wake
// function is registered via set_wake().  cancel() fires it immediately,
// cancelling the timerfd and resuming the coroutine without waiting for
// the full sleep duration (no zombie coroutines).
//
// Because the event loop is single-threaded, the timer callback and
// cancel() are mutually exclusive — no atomics needed for wake_fn_.
//
//   app.get("/poll", [](Request& req) -> Task<json> {
//       while (true) {
//           co_await sleep(1000);
//           if (req.is_cancelled()) co_return {};   // connection gone
//           // ... do work ...
//       }
//   });

struct CancellationToken {
    std::atomic<bool> cancelled{false};
    std::function<void()> wake_fn_;   // set while a sleep() is active

    void cancel() noexcept {
        if (cancelled.exchange(true, std::memory_order_acq_rel)) return;
        if (wake_fn_) {
            auto fn = std::move(wake_fn_);
            fn();
        }
    }

    bool is_cancelled() const noexcept {
        return cancelled.load(std::memory_order_acquire);
    }

    // Called by SleepAwaitable on suspend.  Fires immediately if already cancelled.
    void set_wake(std::function<void()> fn) {
        if (cancelled.load(std::memory_order_acquire)) { fn(); return; }
        wake_fn_ = std::move(fn);
    }

    // Called by SleepAwaitable when the timer fires naturally (not via cancel).
    void clear_wake() noexcept { wake_fn_ = nullptr; }
};

} // namespace lumen
