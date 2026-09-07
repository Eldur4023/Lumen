#include "../include/lumen/app.hpp"
#include "../include/lumen/logger.hpp"
#include "../include/lumen/metrics.hpp"
#include "../include/lumen/request.hpp"
#include "../include/lumen/response.hpp"
#include "../include/lumen/task.hpp"

#include <lumen/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <sys/epoll.h>

#include <csignal>
#include <sched.h>
#include <iostream>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <mutex>
#include <functional>
#include <sstream>
#include <iomanip>
#include <unistd.h>
#include <fcntl.h>

namespace lumen {

namespace {


static const char* mime_for_ext(const std::string& ext) {
    if (ext == ".html" || ext == ".htm")  return "text/html; charset=utf-8";
    if (ext == ".css")   return "text/css; charset=utf-8";
    if (ext == ".js")    return "application/javascript; charset=utf-8";
    if (ext == ".json")  return "application/json; charset=utf-8";
    if (ext == ".svg")   return "image/svg+xml";
    if (ext == ".png")   return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif")   return "image/gif";
    if (ext == ".webp")  return "image/webp";
    if (ext == ".ico")   return "image/x-icon";
    if (ext == ".woff")  return "font/woff";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".ttf")   return "font/ttf";
    if (ext == ".pdf")   return "application/pdf";
    if (ext == ".xml")   return "application/xml";
    if (ext == ".txt")   return "text/plain; charset=utf-8";
    if (ext == ".wasm")  return "application/wasm";
    if (ext == ".mjs")   return "application/javascript; charset=utf-8";
    if (ext == ".map")   return "application/json; charset=utf-8";
    if (ext == ".mp4")   return "video/mp4";
    if (ext == ".webm")  return "video/webm";
    if (ext == ".mp3")   return "audio/mpeg";
    if (ext == ".ogg")   return "audio/ogg";
    if (ext == ".avif")  return "image/avif";
    return "application/octet-stream";
}

// Weak ETag from mtime + size: "mtime-size" hex-encoded.
static std::string make_etag(const std::filesystem::file_time_type& mtime,
                              std::uintmax_t size) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                  mtime.time_since_epoch()).count();
    std::ostringstream ss;
    ss << '"' << std::hex << ns << '-' << size << '"';
    return ss.str();
}

static std::string url_decode_path(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            char buf[3] = {s[i+1], s[i+2], '\0'};
            char* end;
            unsigned long v = std::strtoul(buf, &end, 16);
            if (end == buf + 2) {
                // Drop %00: it truncates POSIX path operations after
                // canonicalisation, creating a mismatch between what auth
                // middlewares see (decoded path) and what the filesystem
                // resolves (truncated at NUL).  Matches the HTTP-level
                // url_decode() in http_connection.cpp.
                if (v != 0) out += static_cast<char>(v);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Returns true and fills res if a static mount covers this path.
// Sets ETag, Cache-Control, and honours If-None-Match for 304 responses.
static bool try_serve_static(
    const std::vector<App::StaticMount>& mounts,
    const Request& req,
    Response& res)
{
    namespace fs = std::filesystem;
    for (const auto& m : mounts) {
        if (req.path.rfind(m.prefix, 0) != 0) continue;
        const size_t plen = m.prefix.size();
        if (req.path.size() > plen && req.path[plen] != '/') continue;

        std::string rel = url_decode_path(req.path.substr(plen));
        if (rel.empty() || rel.front() != '/') rel = '/' + rel;

        // Block dotfiles: any path component starting with '.' (e.g. .env,
        // .git/config, .htaccess) — common misconfiguration in deployments.
        // We check the URL-decoded relative path so %2E bypasses are caught.
        for (size_t i = 0; i < rel.size(); ++i) {
            if (rel[i] == '/' && i + 1 < rel.size() && rel[i + 1] == '.') {
                res.status(404).json_text(R"({"error":"Not Found"})");
                return true;
            }
        }

        // canonical() for the root resolves any symlinks inside the serve root
        // itself (e.g. if m.root is itself a symlink to /var/www).  Required
        // so the mismatch check below compares fully-resolved paths.
        std::error_code root_ec;
        auto canonical_root = fs::canonical(m.root, root_ec);
        if (root_ec) {
            res.status(500).json_text(R"({"error":"Server misconfiguration"})");
            return true;
        }

        fs::path file = canonical_root / rel.substr(1);

        // First pass: weakly_canonical catches ".." traversal even when the
        // target file does not exist yet (needed for the 404 branch below).
        std::error_code ec;
        auto preliminary = fs::weakly_canonical(file);
        {
            auto [ri, fi] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                          preliminary.begin());
            if (ri != canonical_root.end()) {
                res.status(403).json_text(R"({"error":"Forbidden"})");
                return true;
            }
        }

        auto canonical_file = preliminary;

        auto status = fs::status(preliminary, ec);
        if (ec || !fs::is_regular_file(status)) {
            // SPA fallback: serve index.html for unknown paths so client-side
            // routers (React Router, Vue Router, etc.) can handle the URL.
            if (m.spa) {
                canonical_file = fs::canonical(canonical_root / "index.html", ec);
                if (ec || !fs::is_regular_file(fs::status(canonical_file))) {
                    res.status(404).json_text(R"({"error":"Not Found"})");
                    return true;
                }
                // index.html itself may be a symlink pointing outside the root.
                // Re-check that the resolved path still lives inside canonical_root
                // so a misconfigured/compromised dist directory cannot exfiltrate
                // arbitrary files via the SPA fallback.
                auto [ri3, fi3] = std::mismatch(canonical_root.begin(),
                                                 canonical_root.end(),
                                                 canonical_file.begin());
                if (ri3 != canonical_root.end()) {
                    res.status(403).json_text(R"({"error":"Forbidden"})");
                    return true;
                }
            } else {
                res.status(404).json_text(R"({"error":"Not Found"})");
                return true;
            }
        } else {
            // File exists: fully resolve symlinks and re-check traversal.
            // The first pass caught ".." sequences; this pass catches symlinks
            // that point outside the root (e.g. uploads/evil -> /etc/passwd).
            canonical_file = fs::canonical(preliminary, ec);
            if (ec) {
                res.status(404).json_text(R"({"error":"Not Found"})");
                return true;
            }
            auto [ri2, fi2] = std::mismatch(canonical_root.begin(), canonical_root.end(),
                                             canonical_file.begin());
            if (ri2 != canonical_root.end()) {
                res.status(403).json_text(R"({"error":"Forbidden"})");
                return true;
            }
        }

        // ── ETag ──────────────────────────────────────────────────────────────
        std::error_code mtime_ec, size_ec;
        auto mtime    = fs::last_write_time(canonical_file, mtime_ec);
        auto filesize = fs::file_size(canonical_file, size_ec);
        if (mtime_ec || size_ec) {
            res.status(500).json_text(R"({"error":"Cannot stat file"})");
            return true;
        }
        std::string etag = make_etag(mtime, filesize);

        // ── Cache-Control ─────────────────────────────────────────────────────
        // Hashed filenames (e.g. app.abc123ef.js) → immutable for 1 year.
        // Detects a hash segment: last component after '.' or '-' is ≥8 hex chars.
        // Everything else → must-revalidate with short max-age.
        const std::string& ext  = canonical_file.extension().string();
        const std::string  stem = canonical_file.stem().string();
        auto is_hex_hash = [](const std::string& s) -> bool {
            auto pos = s.find_last_of(".-");
            if (pos == std::string::npos) return false;
            const auto seg = s.substr(pos + 1);
            if (seg.size() < 8) return false;
            return std::all_of(seg.begin(), seg.end(),
                               [](unsigned char c){ return std::isxdigit(c); });
        };
        const char* cache_ctrl = is_hex_hash(stem)
            ? "public, max-age=31536000, immutable"
            : "public, max-age=3600, must-revalidate";

        const char* mime = mime_for_ext(ext);
        res.header("ETag",          etag);
        res.header("Cache-Control", cache_ctrl);
        res.header("Content-Type",  mime);

        // ── 304 Not Modified ──────────────────────────────────────────────────
        auto inm = req.header("if-none-match");
        if (inm && *inm == etag) {
            res.status(304).send("");
            return true;
        }

        // ── Serve via sendfile(2) — zero-copy ─────────────────────────────────
        res.send_file(canonical_file, filesize);
        return true;
    }
    return false;
}

// ── Graceful shutdown ─────────────────────────────────────────────────────────
// Signal handler writes one byte to a pipe; the event loop thread reads it
// and runs the actual drain logic.  This keeps the handler async-signal-safe:
// only write(2) and _Exit(2) are used — both appear in the POSIX safe list.
//
// g_signal_pipe[0] = read end (monitored by epoll on the main loop)
// g_signal_pipe[1] = write end (written by the signal handler)
static int                   g_signal_pipe[2] = {-1, -1};
static std::function<void()> g_initiate_drain;
static volatile sig_atomic_t g_signal_count   = 0;

static void signal_handler(int) {
    if (++g_signal_count >= 2) {
        static const char msg[] = "\nForced exit.\n";
        (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
        std::_Exit(1);
    }
    static const char msg[] = "\nShutting down gracefully... (CTRL+C again to force)\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1);
    // Wake the event loop — write is async-signal-safe.
    if (g_signal_pipe[1] >= 0) {
        char byte = 1;
        (void)write(g_signal_pipe[1], &byte, 1);
    }
}

} // anonymous namespace

// ── App::run ──────────────────────────────────────────────────────────────────

// ── App::prepare ─────────────────────────────────────────────────────────────
// Ordena los montajes estaticos una sola vez (idempotente).

void App::prepare() {
    if (prepared_) return;
    prepared_ = true;
    std::sort(static_mounts_.begin(), static_mounts_.end(),
              [](const StaticMount& a, const StaticMount& b) {
                  return a.prefix.size() > b.prefix.size();
              });
}

// ── App::handle_request ───────────────────────────────────────────────────────
// Full middleware + router pipeline as a coroutine.
// Used by run() (via the DispatchFn) and by TestClient for in-process testing.

Task<void> App::handle_request(Request& req, Response& res) {
    res.set_templates_dir(templates_dir_);

    // Static file mounts bypass the middleware chain.
    if (req.method == "GET" || req.method == "HEAD") {
        if (try_serve_static(static_mounts_, req, res)) co_return;
    }

    // ── Async middleware chain ─────────────────────────────────────────────
    // call_next lives in a shared_ptr so NextFn closures that outlive this
    // coroutine frame (e.g. during shutdown) don't dangle on the function
    // object. advanced is also heap-allocated for the same reason.
    using CallNext = std::function<Task<void>(size_t)>;
    auto call_next = std::make_shared<CallNext>();
    *call_next = [this, &req, &res, call_next](size_t i) -> Task<void> {
        if (i < middlewares_.size()) {
            auto advanced = std::make_shared<bool>(false);
            co_await middlewares_[i](req, res,
                [call_next, advanced, i]() -> Task<void> {
                    if (*advanced) co_return;
                    *advanced = true;
                    co_await (*call_next)(i + 1);
                });
        } else {
            auto match = router_.match(req.method, req.path);
            if (match.found) {
                req.params = std::move(match.params);
                co_await match.handler(req, res);
            } else {
                res.status(404).json_text(R"({"error":"Not Found"})");
            }
        }
    };
    co_await (*call_next)(0);

    // Error handlers run after the full chain, while we still own req/res.
    // Async handlers take precedence over sync handlers for the same code.
    if (res.status_code() >= 400) {
        int code = res.status_code();

        // El cuerpo por defecto ya esta escrito y marcado como comprometido.
        // Un manejador de error existe precisamente para sustituirlo, asi que
        // se retira antes de darle el control; sin esto, su res.json() o
        // res.render() se ignoraria en silencio.
        //
        // Si el manejador no escribe nada, se repone el cuerpo original: no
        // haberlo escrito no puede significar quedarse sin respuesta.
        auto guarded = [&res](auto&& fn) {
            std::string saved = res.take_body();
            fn();
            if (!res.is_committed()) res.restore_body(std::move(saved));
        };

        auto ait = async_error_handlers_.find(code);
        if (ait != async_error_handlers_.end()) {
            std::string saved = res.take_body();
            co_await ait->second(code, req, res);
            if (!res.is_committed()) res.restore_body(std::move(saved));
        } else if (catchall_async_error_handler_) {
            std::string saved = res.take_body();
            co_await catchall_async_error_handler_(code, req, res);
            if (!res.is_committed()) res.restore_body(std::move(saved));
        } else {
            auto it = error_handlers_.find(code);
            if (it != error_handlers_.end()) {
                guarded([&] { it->second(code, req, res); });
            } else if (catchall_error_handler_) {
                guarded([&] { catchall_error_handler_(code, req, res); });
            }
        }
    }
}

// ── App::run ──────────────────────────────────────────────────────────────────

void App::run(const std::string& host, uint16_t port) {
    std::signal(SIGPIPE, SIG_IGN);

    prepare();  // ordena los montajes estaticos

    // ── Build the async dispatch function ─────────────────────────────────────
    // Returns handle_request() directly — no extra coroutine frame.
    DispatchFn dispatch = [this](Request& req, Response& res) {
        return handle_request(req, res);
    };

    // ── Multi-core: one event loop per hardware thread ────────────────────────
    // hardware_concurrency() cuenta los cores de la maquina, no los que este
    // proceso puede usar: ignora la mascara de afinidad y el limite de cpu del
    // cgroup.  En un contenedor con 2 cores asignados de una maquina de 64,
    // levantaria 64 event loops sobre 2 cores.
    unsigned num_threads = 0;
    {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (::sched_getaffinity(0, sizeof(mask), &mask) == 0)
            num_threads = static_cast<unsigned>(CPU_COUNT(&mask));
    }
    if (num_threads == 0) num_threads = std::thread::hardware_concurrency();
    num_threads = std::max(1u, num_threads);

    // Shared connection counter — enforces max_connections_ across all threads.
    auto shared_conn_count = std::make_shared<std::atomic<int>>(0);
    Metrics::instance().active_connections_ = shared_conn_count.get();

    std::vector<core::EventLoop*>  all_loops;
    std::vector<core::TcpServer*>  all_servers;
    std::mutex                     all_mutex;

    // ── Graceful shutdown ─────────────────────────────────────────────────────
    // On first SIGINT/SIGTERM:
    //   1. Signal handler writes one byte to g_signal_pipe[1] (async-signal-safe).
    //   2. The main epoll loop detects it and runs g_initiate_drain on its thread:
    //      stop accepting + poll every 100 ms until connections drain or 30 s elapse.
    // On second signal: std::_Exit(1) — see signal_handler above.
    g_signal_count = 0;
    if (::pipe2(g_signal_pipe, O_CLOEXEC | O_NONBLOCK) < 0)
        throw std::runtime_error(std::string("pipe2: ") + strerror(errno));

    core::EventLoop main_loop;
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_loops.push_back(&main_loop);
    }

    g_initiate_drain = [this, &main_loop, shared_conn_count, &all_loops, &all_servers, &all_mutex]() {
        {
            // Cada servidor deja de aceptar EN SU PROPIO loop.  Hacerlo desde
            // aqui —que es el hilo del loop principal— tocaba el mapa de
            // manejadores de los otros N loops mientras ellos lo estaban
            // leyendo.  Cuatro lineas mas abajo ya se usa post() para lo suyo:
            // es el mismo mecanismo, aplicado a todos.
            //
            // El desfase es de una vuelta de loop, en la que un worker todavia
            // podria aceptar una conexion.  Da igual: el drenaje espera hasta
            // 30 segundos a que no quede ninguna.
            std::lock_guard<std::mutex> lk(all_mutex);
            for (auto* s : all_servers) s->pedir_parada();
        }

        main_loop.post([this, &main_loop, shared_conn_count, &all_loops, &all_mutex]() {
            using Clock = std::chrono::steady_clock;
            auto deadline = Clock::now() + std::chrono::seconds(30);

            auto fn = std::make_shared<std::function<void()>>();
            *fn = [this, fn, &main_loop, shared_conn_count, deadline,
                   &all_loops, &all_mutex]() mutable {
                bool timed_out = Clock::now() >= deadline;
                int  remaining = shared_conn_count->load(std::memory_order_acquire);

                if (remaining == 0 || timed_out) {
                    if (timed_out && remaining > 0)
                        log().warn("shutdown: grace period expired — ",
                                   remaining, " connection(s) dropped");
                    else
                        log().info("shutdown: all connections drained");
                    // Ultimo momento en que los loops siguen vivos: aqui se
                    // apaga lo que tenga hilos propios posteando a ellos.  Si
                    // se hiciera despues, esos hilos escribirian en un loop ya
                    // destruido.
                    if (before_stop_) before_stop_();

                    std::lock_guard<std::mutex> lk(all_mutex);
                    for (auto* l : all_loops) l->stop();
                    return;
                }
                main_loop.schedule_timer(100, *fn);
            };
            (*fn)();
        });
    };

    // Register the signal pipe with the main event loop.
    // When the signal handler fires it writes a byte here; the loop thread
    // calls g_initiate_drain safely without any async-signal-safe concerns.
    main_loop.add(g_signal_pipe[0], EPOLLIN,
                  [](uint32_t) {
                      char buf[16];
                      while (::read(g_signal_pipe[0], buf, sizeof(buf)) > 0) {}
                      if (g_initiate_drain) g_initiate_drain();
                  });

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── Worker threads (cores 1..N-1) ─────────────────────────────────────────
    // Each thread runs its own EventLoop + TcpServer.  SO_REUSEPORT lets the
    // kernel distribute incoming connections evenly across all workers.
    std::vector<std::thread> threads;
    threads.reserve(num_threads - 1);
    for (unsigned i = 1; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            core::EventLoop loop;
            core::TcpServer server(host, port, loop, dispatch,
                                   max_connections_, shared_conn_count);
            {
                std::lock_guard<std::mutex> lk(all_mutex);
                all_loops.push_back(&loop);
                all_servers.push_back(&server);
            }
            loop.run();
            {
                // Remove from tracking after the loop exits so stop_accepting()
                // is never called on a destroyed server.
                std::lock_guard<std::mutex> lk(all_mutex);
                all_loops.erase(std::remove(all_loops.begin(), all_loops.end(), &loop), all_loops.end());
                all_servers.erase(std::remove(all_servers.begin(), all_servers.end(), &server), all_servers.end());
            }
        });
    }

    // ── Main thread (core 0) ──────────────────────────────────────────────────
    core::TcpServer main_server(host, port, main_loop, dispatch,
                                max_connections_, shared_conn_count);
    {
        std::lock_guard<std::mutex> lk(all_mutex);
        all_servers.push_back(&main_server);
    }

    const char* scheme = "http";
    log().info("Lumen running on ", scheme, "://", host, ':', port,
               " (threads=", num_threads, ", press CTRL+C to quit)");

    main_loop.run();

    for (auto& t : threads) t.join();

    // Restore default signal disposition BEFORE closing the pipe.  Otherwise
    // a stray SIGINT/SIGTERM between the close and the SIG_DFL reset would
    // run signal_handler with g_signal_pipe[1] either invalid or already
    // reassigned to an unrelated fd opened in another thread.
    std::signal(SIGINT,  SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);

    Metrics::instance().active_connections_ = nullptr;
    g_initiate_drain = nullptr;
    g_signal_count   = 0;
    if (g_signal_pipe[0] >= 0) { ::close(g_signal_pipe[0]); g_signal_pipe[0] = -1; }
    if (g_signal_pipe[1] >= 0) { ::close(g_signal_pipe[1]); g_signal_pipe[1] = -1; }
}

} // namespace lumen
