#pragma once
#include <functional>
#include "task.hpp"   // Task<T>

namespace lumen {

class Request;
class Response;

// A NextFn calls the next middleware/handler in the chain and returns a Task
// that completes when the rest of the chain (and its async work) finishes.
using NextFn     = std::function<Task<void>()>;

// Middleware signature — must co_await next() to continue the chain.
using Middleware = std::function<Task<void>(Request&, Response&, NextFn)>;

// Top-level dispatch function produced by App and consumed by HttpConnection.
using DispatchFn = std::function<Task<void>(Request&, Response&)>;

// Handler stored in the router (always returns Task<void>; sync handlers are wrapped).
using Handler    = std::function<Task<void>(Request&, Response&)>;

// Error handler (sync; called after the async chain completes).
using ErrorHandler = std::function<void(int code, Request&, Response&)>;

// Async error handler — use when error handling needs to co_await (e.g. DB logging).
using AsyncErrorHandler = std::function<Task<void>(int code, Request&, Response&)>;

} // namespace lumen
