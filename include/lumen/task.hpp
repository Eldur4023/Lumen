#pragma once
#include <coroutine>
#include <exception>
#include <utility>
#include <functional>
#include <memory>
#include <iostream>
#include <lumen/core/event_loop.hpp>
#include "cancel.hpp"

namespace lumen {

// ─── Task<T> ─────────────────────────────────────────────────────────────────
//
// Coroutine return type. Supports:
//   - co_return value
//   - co_await sleep(ms, loop)
//   - co_await Task<U>          (chained tasks via symmetric transfer)
//
// Lifetime: the coroutine frame self-destructs via the event loop after
// completing, as long as loop is set on the promise before resuming.
// Use detach() to transfer ownership from the Task wrapper to the promise.

template<typename T>
struct Task {
    struct promise_type {
        using value_type = T;

        T               result;
        std::exception_ptr exception;
        std::function<void(T)> on_complete;   // called when the task finishes
        std::coroutine_handle<> continuation; // outer coroutine waiting on us
        core::EventLoop* loop = nullptr;      // for deferred self-destruction

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        // After the coroutine finishes:
        //   - if there is a continuation (co_await chain), resume it
        //   - otherwise schedule self-destruction via the event loop
        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto c = h.promise().continuation) return c;  // symmetric transfer

                // No continuation — schedule destruction on the event loop
                if (h.promise().loop) {
                    auto raw = std::coroutine_handle<>(h);
                    h.promise().loop->post([raw]() mutable { raw.destroy(); });
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_value(T value) {
            result = std::move(value);
            if (on_complete) on_complete(result);
        }
        void unhandled_exception() {
            exception = std::current_exception();
            if (!continuation) {
                try { std::rethrow_exception(exception); }
                catch (const std::exception& e) {
                    std::cerr << "[lumen] unhandled exception in detached task: " << e.what() << '\n';
                } catch (...) {
                    std::cerr << "[lumen] unknown exception in detached task\n";
                }
            }
        }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task(const Task&) = delete;
    ~Task() { if (handle) handle.destroy(); }

    // Transfer ownership to the promise (stops ~Task from destroying the frame)
    std::coroutine_handle<promise_type> detach() {
        return std::exchange(handle, nullptr);
    }

    bool done() const { return handle && handle.done(); }

    T get_result() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }

    // ── Awaiter interface (for co_await Task<T> inside another coroutine) ────
    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> outer) noexcept {
        // Store the outer coroutine as our continuation; FinalAwaitable resumes it
        handle.promise().continuation = outer;
        handle.resume(); // start the inner task
    }

    T await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
        return std::move(handle.promise().result);
    }
};

// ─── Task<void> ──────────────────────────────────────────────────────────────

template<>
struct Task<void> {
    struct promise_type {
        using value_type = void;

        std::exception_ptr exception;
        std::function<void()> on_complete;
        std::coroutine_handle<> continuation;
        core::EventLoop* loop = nullptr;

        Task get_return_object() {
            return Task(std::coroutine_handle<promise_type>::from_promise(*this));
        }
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaitable {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept
            {
                if (auto c = h.promise().continuation) return c;

                if (h.promise().loop) {
                    auto raw = std::coroutine_handle<>(h);
                    h.promise().loop->post([raw]() mutable { raw.destroy(); });
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaitable final_suspend() noexcept { return {}; }

        void return_void() {
            if (on_complete) on_complete();
        }
        void unhandled_exception() {
            exception = std::current_exception();
            if (!continuation) {
                try { std::rethrow_exception(exception); }
                catch (const std::exception& e) {
                    std::cerr << "[lumen] unhandled exception in detached task: " << e.what() << '\n';
                } catch (...) {
                    std::cerr << "[lumen] unknown exception in detached task\n";
                }
            }
        }
    };

    std::coroutine_handle<promise_type> handle;

    explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
    Task(Task&& o) noexcept : handle(std::exchange(o.handle, nullptr)) {}
    Task(const Task&) = delete;
    ~Task() { if (handle) handle.destroy(); }

    std::coroutine_handle<promise_type> detach() {
        return std::exchange(handle, nullptr);
    }

    bool done() const { return handle && handle.done(); }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> outer) noexcept {
        handle.promise().continuation = outer;
        handle.resume();
    }
    void await_resume() {
        if (handle.promise().exception) std::rethrow_exception(handle.promise().exception);
    }
};

// ─── SleepAwaitable ──────────────────────────────────────────────────────────
// Non-blocking sleep: suspends the coroutine and resumes it after `ms`
// milliseconds via the event loop's timerfd (one-shot, no extra threads).
//
// If a CancellationToken is supplied (via the thread-local or explicitly) and
// the connection is closed before the timer fires, the coroutine is resumed
// immediately so it can check is_cancelled() and exit rather than waiting out
// the full duration (avoids zombie coroutines on slow-path handlers).

struct SleepAwaitable {
    int ms;
    core::EventLoop*                    loop;
    std::weak_ptr<CancellationToken>    token;

    bool await_ready() const noexcept { return ms <= 0; }

    void await_suspend(std::coroutine_handle<> h) noexcept {
        if (!loop) { h.resume(); return; }

        // Schedule the timer.  The callback clears the wake slot first so
        // cancel() can't fire it again after the timer has already won.
        int tfd = loop->schedule_timer(ms, [h, tok = token]() mutable {
            if (auto t = tok.lock()) t->clear_wake();
            if (!h.done()) h.resume();
        });

        // Register the early-wake callback.  If the token is already
        // cancelled, set_wake() fires the callback immediately: we cancel
        // the timerfd we just armed and resume the coroutine.
        if (auto t = token.lock()) {
            t->set_wake([h, tfd, l = loop]() mutable {
                l->cancel_timer(tfd);
                if (!h.done()) h.resume();
            });
        }
    }

    void await_resume() const noexcept {}
};

// Thread-locals set by HttpConnection::dispatch() for the current request.
namespace detail {
    inline thread_local core::EventLoop*             current_loop  = nullptr;
    inline thread_local std::weak_ptr<CancellationToken> current_token = {};
}

// sleep(ms, loop) — explicit loop (backward-compat / advanced use)
inline SleepAwaitable sleep(int ms, core::EventLoop* loop) {
    return {ms, loop, detail::current_token};
}

// sleep(ms) — uses thread-locals set for the current request (no args needed)
inline SleepAwaitable sleep(int ms) {
    return {ms, detail::current_loop, detail::current_token};
}

} // namespace lumen
