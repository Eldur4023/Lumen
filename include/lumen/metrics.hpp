#pragma once
#include <atomic>
#include <chrono>
#include <string>
#include <sstream>
#include <iomanip>

// ── Metrics ────────────────────────────────────────────────────────────────────
//
// Process-wide singleton that tracks basic server counters.
// Written from HttpConnection::finish_dispatch (one call per request),
// read from the /metrics route. All fields are atomics — lock-free.
//
// Wired up by App::run():
//   Metrics::instance().active_connections_ = shared_conn_count.get();
//
// Usage:
//   app.enable_health();    // GET /health  → {"status":"ok","uptime":…}
//   app.enable_metrics();   // GET /metrics → Prometheus text format

namespace lumen {

class Metrics {
public:
    static Metrics& instance() noexcept {
        static Metrics m;
        return m;
    }

    // Called from finish_dispatch() for every completed HTTP request.
    void record(int status) noexcept {
        requests_total_.fetch_add(1, std::memory_order_relaxed);
        if      (status >= 500) requests_5xx_.fetch_add(1, std::memory_order_relaxed);
        else if (status >= 400) requests_4xx_.fetch_add(1, std::memory_order_relaxed);
        else if (status >= 200) requests_2xx_.fetch_add(1, std::memory_order_relaxed);
    }

    // Prometheus exposition format (text/plain; version=0.0.4).
    std::string to_prometheus() const {
        using Clock = std::chrono::steady_clock;
        double uptime = std::chrono::duration<double>(Clock::now() - started_at_).count();
        int    conns  = active_connections_
                        ? active_connections_->load(std::memory_order_relaxed)
                        : 0;

        uint64_t total = requests_total_.load(std::memory_order_relaxed);
        uint64_t r2xx  = requests_2xx_.load(std::memory_order_relaxed);
        uint64_t r4xx  = requests_4xx_.load(std::memory_order_relaxed);
        uint64_t r5xx  = requests_5xx_.load(std::memory_order_relaxed);

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(3);

        ss << "# HELP lumen_requests_total Total HTTP requests handled\n"
              "# TYPE lumen_requests_total counter\n"
           << "lumen_requests_total " << total << "\n\n"

              "# HELP lumen_requests_by_class HTTP requests grouped by status class\n"
              "# TYPE lumen_requests_by_class counter\n"
           << "lumen_requests_by_class{class=\"2xx\"} " << r2xx << "\n"
           << "lumen_requests_by_class{class=\"4xx\"} " << r4xx << "\n"
           << "lumen_requests_by_class{class=\"5xx\"} " << r5xx << "\n\n"

              "# HELP lumen_active_connections Currently open TCP connections\n"
              "# TYPE lumen_active_connections gauge\n"
           << "lumen_active_connections " << conns << "\n\n"

              "# HELP lumen_uptime_seconds Seconds since server start\n"
              "# TYPE lumen_uptime_seconds gauge\n"
           << "lumen_uptime_seconds " << uptime << "\n";

        return ss.str();
    }

    // Resumen de /health, ya en texto.  Son cuatro campos de tipos conocidos:
    // montar un arbol JSON para esto era pasar por un intermediario que no
    // aportaba nada.
    std::string to_health_text() const {
        using Clock = std::chrono::steady_clock;
        double uptime = std::chrono::duration<double>(Clock::now() - started_at_).count();
        int    conns  = active_connections_
                        ? active_connections_->load(std::memory_order_relaxed)
                        : 0;
        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"uptime_seconds\":" << uptime
           << ",\"active_connections\":" << conns
           << ",\"requests_total\":" << requests_total_.load(std::memory_order_relaxed)
           << "}";
        return ss.str();
    }

    // ── Framework-internal ──────────────────────────────────────────────────────
    // Pointer to the shared conn counter owned by App::run().
    // Written once before threads start; never written again until run() returns.
    std::atomic<int>* active_connections_ = nullptr;

private:
    Metrics() = default;

    std::atomic<uint64_t>                  requests_total_{0};
    std::atomic<uint64_t>                  requests_2xx_{0};
    std::atomic<uint64_t>                  requests_4xx_{0};
    std::atomic<uint64_t>                  requests_5xx_{0};
    std::chrono::steady_clock::time_point  started_at_ = std::chrono::steady_clock::now();
};

} // namespace lumen
