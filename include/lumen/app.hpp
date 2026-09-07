#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <unistd.h>
#include "types.hpp"
#include "router.hpp"
#include "openapi.hpp"
#include "websocket.hpp"
#include "metrics.hpp"

namespace lumen {

class App {
public:
    App()  = default;
    ~App() = default;

    // Route registration — support both :param and {param} styles.
    // Each registration also captures compile-time type info for OpenAPI generation.
    template<typename F> App& get   (std::string path, F&& h) { router_.add("GET",    std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& post  (std::string path, F&& h) { router_.add("POST",   std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& put   (std::string path, F&& h) { router_.add("PUT",    std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& patch (std::string path, F&& h) { router_.add("PATCH",  std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& del   (std::string path, F&& h) { router_.add("DELETE", std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& any   (std::string path, F&& h) {                                     router_.add("*",      std::move(path), std::forward<F>(h)); return *this; }

    // Middleware (applied in order for every request)
    App& use(Middleware m) { middlewares_.push_back(std::move(m)); return *this; }

    // ── Cierre ───────────────────────────────────────────────────────────────
    //
    // Invoked once, on the main event loop thread, when the connections are
    // already drained but BEFORE stopping the loops.
    //
    // It is the only safe point to shut down anything that has its own threads
    // posting to the loop —a database module's pool, for instance—: while it
    // runs, the loops are still alive, so a late post() still finds its
    // destination.  When it returns, nobody can be posting any more and
    // stopping the loops is safe.
    //
    // It blocks the shutdown for as long as it lasts, so it must finish.
    App& on_before_stop(std::function<void()> fn) {
        before_stop_ = std::move(fn);
        return *this;
    }

    // Serve a directory of static files under a URL prefix.
    //   app.serve_static("/static", "./public")
    //   GET /static/app.js  →  ./public/app.js
    //
    // SPA fallback (spa = true): any path that doesn't match a real file is
    // served as ./public/index.html with status 200.  Enables client-side
    // routing for React/Vue/Svelte apps.
    //   app.serve_static("/", "./dist", true)
    App& serve_static(std::string url_prefix, std::string fs_root, bool spa = false) {
        static_mounts_.push_back({std::move(url_prefix), std::move(fs_root), spa});
        return *this;
    }

    // Register an error handler for a specific HTTP status code.
    //   app.on_error(404, [](int code, Request& req, Response& res) { ... });
    // Or a catch-all for any error (4xx/5xx):
    //   app.on_error([](int code, Request& req, Response& res) { ... });
    App& on_error(int code, ErrorHandler h) {
        error_handlers_[code] = std::move(h);
        return *this;
    }
    App& on_error(ErrorHandler h) {
        catchall_error_handler_ = std::move(h);
        return *this;
    }

    // Async variants — use when error handling needs to co_await (e.g. DB logging).
    App& on_async_error(int code, AsyncErrorHandler h) {
        async_error_handlers_[code] = std::move(h);
        return *this;
    }
    App& on_async_error(AsyncErrorHandler h) {
        catchall_async_error_handler_ = std::move(h);
        return *this;
    }

    // Directory where the templates are looked up (default: "./templates")
    App& set_templates(std::string dir) { templates_dir_ = std::move(dir); return *this; }

    // Override the title and version shown in /docs and /openapi.json.
    App& api_info(std::string title, std::string version = "0.1.0") {
        api_title_   = std::move(title);
        api_version_ = std::move(version);
        return *this;
    }


    // ── WebSocket ────────────────────────────────────────────────────────────
    //
    // Register a WebSocket handler.  The framework performs the RFC 6455
    // handshake and hands a WSConnection to the handler.
    //
    //   app.ws("/chat", [](WSConnection ws) -> Task<void> {
    //       while (ws.is_open()) {
    //           auto msg = co_await ws.recv();
    //           if (!msg) break;
    //           ws.send("echo: " + msg->data);
    //       }
    //   });
    //
    // ── Cross-Site WebSocket Hijacking (CSWH) protection ─────────────────────
    // Browsers do NOT enforce same-origin policy on WebSocket handshakes.
    // Any site can open a WS connection that inherits the user's cookies, so
    // an authenticated WS endpoint without an Origin check is hijackable.
    //
    //   app.ws("/chat", handler, {.allowed_origins = {"https://app.example.com"}});
    //
    // If allowed_origins is empty (default) the check is skipped — only safe
    // for unauthenticated public endpoints.
    struct WSOptions {
        std::vector<std::string> allowed_origins;
    };

    template<typename F>
    App& ws(std::string path, F&& fn) {
        // No allowed_origins configured: any website can open a WebSocket
        // to this endpoint and inherit the user's cookies (CSWSH).
        // Safe only for unauthenticated / public endpoints.
        // Suppress this warning with: app.ws(path, fn, {.allowed_origins = {"https://your-app.com"}})
        std::cerr << "[lumen] ws(\"" << path << "\"): no allowed_origins set — "
                     "cross-site WebSocket hijacking possible on cookie-authenticated endpoints. "
                     "Use ws(path, fn, {.allowed_origins = {\"https://your-app.com\"}}) to restrict.\n";
        return ws(std::move(path), std::forward<F>(fn), WSOptions{});
    }

    // Builds the upgrade handler without registering it.  Used by ws() and also
    // by the Lumen Script frontend, which has its own router and needs the same
    // handshake logic without going through the engine's router.
    //
    // `fn` receives the established connection along with the Request and the
    // Response of the originating request; after the upgrade, the Response
    template<typename F>
    static Handler make_ws_handler(F&& fn, WSOptions opts) {
        auto wrapper = [fn = std::forward<F>(fn), opts = std::move(opts)]
                       (Request& req, Response& res) mutable -> Task<void> {

            // Origin check — applied to both HTTP/1.1 and RFC 8441 (HTTP/2) paths.
            if (!opts.allowed_origins.empty()) {
                auto origin = req.header("origin");
                bool ok = false;
                if (origin) {
                    for (const auto& o : opts.allowed_origins) {
                        if (o == *origin) { ok = true; break; }
                    }
                }
                if (!ok) {
                    res.status(403).json_text(R"({"error":"Origin not allowed"})");
                    co_return;
                }
            }

            // ── RFC 6455 — 101 Switching Protocols ───────────────────────────
            auto upgrade = req.header("upgrade");
            auto key     = req.header("sec-websocket-key");

            // Token-level match for Upgrade (RFC 7230 §3.2.6): split on ',',
            // trim, compare case-insensitively.  A substring search would
            // accept "notwebsocket" and reject "WebSocket".
            auto has_websocket_token = [](const std::string& h) {
                size_t pos = 0;
                while (pos < h.size()) {
                    size_t comma = h.find(',', pos);
                    size_t end = (comma == std::string::npos) ? h.size() : comma;
                    size_t a = pos, b = end;
                    while (a < b && (h[a] == ' ' || h[a] == '\t')) ++a;
                    while (b > a && (h[b-1] == ' ' || h[b-1] == '\t')) --b;
                    if (b - a == 9) {
                        bool eq = true;
                        static const char kWs[] = "websocket";
                        for (size_t i = 0; i < 9; ++i) {
                            char c = h[a + i];
                            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
                            if (c != kWs[i]) { eq = false; break; }
                        }
                        if (eq) return true;
                    }
                    if (comma == std::string::npos) break;
                    pos = comma + 1;
                }
                return false;
            };

            if (!upgrade || !has_websocket_token(*upgrade) || !key) {
                res.status(426)
                   .header("Sec-WebSocket-Version", "13")
                   .json_text(R"({"error":"WebSocket upgrade required"})");
                co_return;
            }

            // RFC 6455 §4.1: Sec-WebSocket-Version MUST be 13.  Reject other
            // drafts with 426 + the version header so the client can retry.
            auto ws_ver = req.header("sec-websocket-version");
            if (!ws_ver || *ws_ver != "13") {
                res.status(426)
                   .header("Sec-WebSocket-Version", "13")
                   .json_text(R"({"error":"Unsupported WebSocket version"})");
                co_return;
            }

            if (!req._raw_write) {
                res.status(500).json_text(R"({"error":"no raw writer for WS upgrade"})");
                co_return;
            }

            // Write the handshake through the TLS-aware writer so HTTPS works
            // identically to plain HTTP.
            std::string hs =
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Accept: " + detail::ws_accept(*key) + "\r\n\r\n";
            size_t sent = 0;
            while (sent < hs.size()) {
                ssize_t n = req._raw_write(hs.data() + sent, hs.size() - sent);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    co_return;
                }
                if (n == 0) co_return;
                sent += static_cast<size_t>(n);
            }

            auto ws_state = std::make_shared<detail::WSState>();
            ws_state->token = req.cancel_token;
            ws_state->loop  = req.loop;
            // Outbound frames: best-effort lossy write through the TLS-aware
            // writer.  EAGAIN/partial writes drop the frame to keep the loop
            // responsive (matches the prior plaintext behaviour).
            auto writer = req._raw_write;
            ws_state->send_fn = [writer](std::string frame) {
                size_t w = 0;
                while (w < frame.size()) {
                    ssize_t n = writer(frame.data() + w, frame.size() - w);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        return;   // EAGAIN / fatal → drop remainder
                    }
                    if (n == 0) return;
                    w += static_cast<size_t>(n);
                }
            };

            // Incoming bytes (decrypted by HttpConnection::do_read) flow here.
            auto state_weak = std::weak_ptr<detail::WSState>(ws_state);
            req._ws_on_data = [state_weak](const char* data, size_t len) {
                if (auto s = state_weak.lock())
                    s->feed(reinterpret_cast<const uint8_t*>(data), len);
            };
            res.mark_ws_started();

            WSConnection ws_conn(ws_state);
            co_await fn(std::move(ws_conn), req, res);
        };
        return Handler(std::move(wrapper));
    }

    template<typename F>
    App& ws(std::string path, F&& fn, WSOptions opts) {
        router_.add_internal("GET", std::move(path),
            make_ws_handler(
                [fn = std::forward<F>(fn)]
                (WSConnection c, Request&, Response&) mutable -> Task<void> {
                    co_await fn(std::move(c));
                },
                std::move(opts)));
        return *this;
    }

    // ── Operational endpoints ────────────────────────────────────────────────
    //
    //   app.enable_health();    // GET /health  → JSON {status, uptime, …}
    //   app.enable_metrics();   // GET /metrics → Prometheus text
    //
    App& enable_health(std::string path = "/health") {
        router_.add("GET", std::move(path), [](Request&, Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8")
               .send(Metrics::instance().to_health_text());
        });
        return *this;
    }

    App& enable_metrics(std::string path = "/metrics") {
        router_.add("GET", std::move(path), [](Request&, Response& res) {
            res.header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
               .send(Metrics::instance().to_prometheus());
        });
        return *this;
    }

    // Maximum simultaneous open connections (default 10 000).
    // Excess connections receive 503 immediately.
    App& max_connections(int n) { max_connections_ = n; return *this; }

    // Start listening — Flask style:
    //   app.run()               → 0.0.0.0:5000
    //   app.run(8080)           → 0.0.0.0:8080
    //   app.run("127.0.0.1", 8080)
    void run(const std::string& host = "0.0.0.0", uint16_t port = 5000);
    void run(uint16_t port) { run("0.0.0.0", port); }

    // ── Testing ──────────────────────────────────────────────────────────────
    //
    // Registers docs routes (idempotent). Called automatically by run() and
    // by TestClient on construction — safe to call more than once.
    void prepare();

    // Execute one request through the full middleware + router pipeline without
    // a network connection. Used by TestClient for in-process testing.
    Task<void> handle_request(Request& req, Response& res);

    struct StaticMount { std::string prefix; std::string root; bool spa = false; };

private:
    Router                                    router_;
    std::vector<Middleware>                   middlewares_;
    std::vector<StaticMount>                  static_mounts_;
    std::unordered_map<int, ErrorHandler>     error_handlers_;
    ErrorHandler                              catchall_error_handler_;
    std::unordered_map<int, AsyncErrorHandler> async_error_handlers_;
    AsyncErrorHandler                          catchall_async_error_handler_;
    std::string                               templates_dir_ = "./templates";
    std::function<void()>                     before_stop_;

    std::string                               api_title_       = "Lumen API";
    std::string                               api_version_     = "0.1.0";
    int                                       max_connections_ = 10'000;


    // Set to true by prepare() so docs routes are only registered once.
    bool                                      prepared_ = false;
};

} // namespace lumen
