#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

// ── Logger ────────────────────────────────────────────────────────────────────
//
// Process-wide logger: levels, console output, and optional rotating file
// output. Unconfigured it logs to the console only (Info and above) — calling
// configure() turns on file output and, optionally, a periodic performance
// report.
//
//   lumen::log().configure({
//       .dir           = "./logs",
//       .filename      = "app.log",
//       .max_file_size = 5 * 1024 * 1024,   // rotate at 5 MB
//       .performance   = true,              // report every 60 s
//   });
//
//   lumen::log().info("cache warmed, ", n, " entries");
//   lumen::log().error("payment provider unreachable: ", err);
//
// The performance report needs request data — add the logger() middleware:
//
//   app.use(lumen::logger());
//
// Every interval it logs throughput, latency and an estimated load percentage
// (Little's law: req/s × avg latency ÷ worker threads) with a one-line
// diagnosis of how saturated the server is.

namespace lumen {

enum class LogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3, Off = 4 };

struct LoggerOptions {
    // File output. The directory is created if missing. Set file=false for
    // console-only logging.
    bool        file          = true;
    std::string dir           = "./logs";
    std::string filename      = "lumen.log";

    // Rotate when the current file reaches this size: lumen.log is renamed
    // to lumen.1.log (then .2, .3, …) and a fresh lumen.log is started.
    std::size_t max_file_size = 10 * 1024 * 1024;

    // Rotated files to keep — the oldest is deleted on rotation. 0 = keep all.
    int         max_files     = 0;

    bool        console       = true;
    LogLevel    level         = LogLevel::Info;

    // Periodic performance report (off by default).
    bool        performance            = false;
    int         performance_interval_s = 60;
    // Worker threads used for the load estimate. 0 = hardware_concurrency(),
    // which matches what App::run() spawns.
    unsigned    workers                = 0;
};

class Logger {
public:
    static Logger& instance() {
        static Logger l;
        return l;
    }

    void configure(LoggerOptions opts) {
        stop_performance_thread();
        {
            std::lock_guard<std::mutex> lk(mutex_);
            opts_ = std::move(opts);
            if (opts_.workers == 0)
                opts_.workers = std::max(1u, std::thread::hardware_concurrency());
            file_.close();
            file_size_ = 0;
            if (opts_.file) open_file_locked();
        }
        if (opts_.file)
            info("logger: writing to ",
                 (std::filesystem::path(opts_.dir) / opts_.filename).string(),
                 " (rotate at ", fmt1(opts_.max_file_size / 1024.0), " KiB)");
        if (opts_.performance) start_performance_thread();
    }

    template<typename... Args> void debug(Args&&... a) { write(LogLevel::Debug, concat(std::forward<Args>(a)...)); }
    template<typename... Args> void info (Args&&... a) { write(LogLevel::Info,  concat(std::forward<Args>(a)...)); }
    template<typename... Args> void warn (Args&&... a) { write(LogLevel::Warn,  concat(std::forward<Args>(a)...)); }
    template<typename... Args> void error(Args&&... a) { write(LogLevel::Error, concat(std::forward<Args>(a)...)); }

    // ── Framework-internal ────────────────────────────────────────────────────
    // Request instrumentation, called by the logger() middleware. Feeds the
    // performance report; harmless no-ops in terms of output.

    void request_started() noexcept {
        int cur = in_flight_.fetch_add(1, std::memory_order_relaxed) + 1;
        atomic_max(peak_in_flight_, cur);
    }

    void request_finished(int status, std::int64_t us) noexcept {
        in_flight_.fetch_sub(1, std::memory_order_relaxed);
        req_count_.fetch_add(1, std::memory_order_relaxed);
        total_us_.fetch_add(us, std::memory_order_relaxed);
        atomic_max(max_us_, us);
        if (status >= 500) err_5xx_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Logger() { stop_performance_thread(); }

private:
    Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    template<typename... Args>
    static std::string concat(Args&&... a) {
        std::ostringstream os;
        (os << ... << std::forward<Args>(a));
        return os.str();
    }

    template<typename T>
    static void atomic_max(std::atomic<T>& a, T v) noexcept {
        T cur = a.load(std::memory_order_relaxed);
        while (v > cur &&
               !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    static const char* tag(LogLevel l) noexcept {
        switch (l) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info:  return "INFO ";
            case LogLevel::Warn:  return "WARN ";
            default:              return "ERROR";
        }
    }

    static std::string timestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto ms  = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
        std::time_t t = system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&t, &tm);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms));
        return buf;
    }

    // One decimal place, e.g. 3.2 — enough precision for log lines.
    static std::string fmt1(double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", v);
        return buf;
    }

    void write(LogLevel level, const std::string& msg) {
        std::lock_guard<std::mutex> lk(mutex_);
        if (level < opts_.level) return;
        std::string line = timestamp() + ' ' + tag(level) + ' ' + msg + '\n';
        if (opts_.console)
            ((level >= LogLevel::Warn) ? std::cerr : std::cout) << line << std::flush;
        if (file_.is_open()) {
            file_ << line << std::flush;
            file_size_ += line.size();
            if (file_size_ >= opts_.max_file_size) rotate_locked();
        }
    }

    // ── File handling (all *_locked methods require mutex_ held) ─────────────

    std::filesystem::path current_path() const {
        return std::filesystem::path(opts_.dir) / opts_.filename;
    }

    // lumen.log → lumen.<n>.log
    std::filesystem::path rotated_path(int n) const {
        auto dot  = opts_.filename.rfind('.');
        auto stem = (dot == std::string::npos) ? opts_.filename
                                               : opts_.filename.substr(0, dot);
        auto ext  = (dot == std::string::npos) ? std::string{}
                                               : opts_.filename.substr(dot);
        return std::filesystem::path(opts_.dir) /
               (stem + '.' + std::to_string(n) + ext);
    }

    void open_file_locked() {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::create_directories(opts_.dir, ec);
        file_.open(current_path(), std::ios::app);
        if (!file_) {
            std::cerr << "[lumen] logger: cannot open "
                      << current_path().string() << " — file logging disabled\n";
            return;
        }
        auto sz = fs::file_size(current_path(), ec);
        file_size_ = ec ? 0 : static_cast<std::size_t>(sz);

        // Resume numbering after the highest existing rotated file.
        next_index_ = 1;
        for (int n = 1; fs::exists(rotated_path(n), ec); ++n)
            next_index_ = n + 1;
    }

    void rotate_locked() {
        namespace fs = std::filesystem;
        std::error_code ec;
        file_.close();
        fs::rename(current_path(), rotated_path(next_index_), ec);
        if (opts_.max_files > 0) {
            int oldest = next_index_ - opts_.max_files;
            if (oldest >= 1) fs::remove(rotated_path(oldest), ec);
        }
        ++next_index_;
        file_.open(current_path(), std::ios::trunc);
        file_size_ = 0;
    }

    // ── Efficiency report ─────────────────────────────────────────────────────

    void start_performance_thread() {
        std::lock_guard<std::mutex> lk(thread_mx_);
        stop_ = false;
        reporter_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(thread_mx_);
            auto interval = std::chrono::seconds(
                std::max(1, opts_.performance_interval_s));
            while (!cv_.wait_for(lk, interval, [this] { return stop_; }))
                report_performance();
        });
    }

    void stop_performance_thread() {
        {
            std::lock_guard<std::mutex> lk(thread_mx_);
            stop_ = true;
        }
        cv_.notify_all();
        if (reporter_.joinable()) reporter_.join();
    }

    void report_performance() {
        auto count    = req_count_.exchange(0, std::memory_order_relaxed);
        auto total_us = total_us_.exchange(0, std::memory_order_relaxed);
        auto max_us   = max_us_.exchange(0, std::memory_order_relaxed);
        auto e5xx     = err_5xx_.exchange(0, std::memory_order_relaxed);
        int  in_fl    = in_flight_.load(std::memory_order_relaxed);
        int  peak     = peak_in_flight_.exchange(in_fl, std::memory_order_relaxed);
        int  secs     = std::max(1, opts_.performance_interval_s);

        if (count == 0) {
            info("[performance] 0 req/", secs, "s — idle, no traffic");
            return;
        }

        double rps    = static_cast<double>(count) / secs;
        double avg_us = static_cast<double>(total_us) / count;
        // Little's law: average busy workers = arrival rate × avg latency.
        // Against the worker count this estimates how full the server is.
        double load   = 100.0 * rps * (avg_us / 1e6) / opts_.workers;

        const char* diagnosis =
            (load < 30.0) ? "healthy, plenty of headroom" :
            (load < 60.0) ? "moderate load, comfortable margin" :
            (load < 85.0) ? "high load, approaching capacity" :
                            "saturated: requests are queuing, handlers can't keep up";

        std::string msg = concat(
            "[performance] ", count, " req/", secs, "s (", fmt1(rps), " req/s)",
            " | avg ", fmt1(avg_us / 1000.0), " ms",
            " | max ", fmt1(max_us / 1000.0), " ms",
            " | peak in-flight ", peak,
            " | 5xx ", e5xx,
            " | load ~", fmt1(load), "% — ", diagnosis);
        write(e5xx > 0 ? LogLevel::Warn : LogLevel::Info, msg);
    }

    // ── State ─────────────────────────────────────────────────────────────────

    std::mutex              mutex_;          // guards opts_, file_, file_size_
    LoggerOptions           opts_{.file = false};   // console-only until configure()
    std::ofstream           file_;
    std::size_t             file_size_ = 0;
    int                     next_index_ = 1;

    // Request counters (window since last report)
    std::atomic<std::uint64_t> req_count_{0};
    std::atomic<std::int64_t>  total_us_{0};
    std::atomic<std::int64_t>  max_us_{0};
    std::atomic<std::uint64_t> err_5xx_{0};
    std::atomic<int>           in_flight_{0};
    std::atomic<int>           peak_in_flight_{0};

    // Efficiency reporter thread
    std::thread             reporter_;
    std::mutex              thread_mx_;
    std::condition_variable cv_;
    bool                    stop_ = false;
};

// Global accessor — lumen::log().info("…");
inline Logger& log() { return Logger::instance(); }

} // namespace lumen
