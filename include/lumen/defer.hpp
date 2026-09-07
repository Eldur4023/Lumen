#pragma once
#include <utility>

namespace lumen {

// RAII wrapper that runs a callable when it goes out of scope.
//
//   auto cleanup = defer([&]{ db.rollback(); });
//   // ... do work ...
//   cleanup.cancel();  // skip if you want to commit instead
//
template<typename F>
class ScopeExit {
    F    fn_;
    bool active_ = true;
public:
    explicit ScopeExit(F&& fn) : fn_(std::forward<F>(fn)) {}
    ~ScopeExit() { if (active_) fn_(); }

    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ScopeExit(ScopeExit&&)                 = delete;

    void cancel() noexcept { active_ = false; }
};

template<typename F>
ScopeExit<F> defer(F&& fn) { return ScopeExit<F>(std::forward<F>(fn)); }

} // namespace lumen
