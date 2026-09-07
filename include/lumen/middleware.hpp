#pragma once
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include "types.hpp"
#include "request.hpp"
#include "response.hpp"
#include "task.hpp"
#include "logger.hpp"

namespace lumen {

// ─── logger() ─────────────────────────────────────────────────────────────────
//
// Logs every request with method, path, status code, and duration through the
// global Logger (see logger.hpp — configure file output and the performance
// report there). Because it co_awaits next(), it measures the FULL handler +
// middleware time (including async handlers). It also feeds the request
// counters behind the performance report.
//
//   app.use(lumen::logger());
//
inline Middleware logger() {
    return [](Request& req, Response& res, NextFn next) -> Task<void> {
        using Clock = std::chrono::steady_clock;
        auto& lg = Logger::instance();
        lg.request_started();
        auto t0 = Clock::now();

        auto elapsed_us = [&t0] {
            return std::chrono::duration_cast<std::chrono::microseconds>(
                       Clock::now() - t0).count();
        };
        try {
            co_await next();
        } catch (...) {
            // Keep the in-flight gauge balanced; the framework's error
            // handling upstream produces the actual 500 response.
            auto us = elapsed_us();
            lg.request_finished(500, us);
            lg.error(req.method, ' ', req.path, " threw after ",
                     us / 1000, " ms");
            throw;
        }
        auto us = elapsed_us();
        lg.request_finished(res.status_code(), us);
        lg.info(req.method, ' ', req.path, " -> ", res.status_code(),
                " (", us / 1000, " ms)");
    };
}

// CORS, compresion, cabeceras de seguridad y limitacion de ritmo vivian aqui.
// En Lumen 2.0 los pone el proxy inverso que hay delante —nginx, Caddy,
// Traefik— que ya lo hace mejor y sin gastar el hilo del event loop.

} // namespace lumen
