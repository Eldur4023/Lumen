#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <lumen/core/event_loop.hpp>
#include "cancel.hpp"
#include "cookies.hpp"

namespace lumen {

class Request {
public:
    std::string method;
    std::string path;
    std::string version;
    std::string body;
    std::string remote_ip;  // IPv4/IPv6 of the connected peer

    // Headers stored with lowercase keys
    std::unordered_map<std::string, std::string> headers;

    // Path params extracted by the router (e.g. /users/:id → params["id"])
    std::unordered_map<std::string, std::string> params;

    // Query string params (e.g. ?page=1&limit=20 → query["page"] = "1")
    std::unordered_map<std::string, std::string> query;

    // Pointer to the event loop for scheduling tasks
    core::EventLoop* loop = nullptr;

    // Raw socket fd — used by res.sse(req) and WebSocket upgrade.
    int _conn_fd = -1;

    // Single write on the connection socket.  Returns bytes written, or -1
    // with errno set (EAGAIN when the kernel buffer is full, EBADF after
    // close).  Set by HttpConnection::dispatch().  Used by SSE, the WebSocket
    // handshake, and WS frame writers — these paths bypass the normal response
    // pipeline, so they write through here rather than the buffered path.
    std::function<ssize_t(const char*, size_t)> _raw_write;

    // WebSocket mode: called by HttpConnection::do_read() with the bytes it
    // just decrypted/read from the socket.  Set by the ws() upgrade wrapper to
    // forward to WSState::feed().  Replacing the older _ws_on_readable (which
    // did its own ::read) so reads work over TLS too.
    std::function<void(const char*, size_t)> _ws_on_data;

    // Cancellation token — shared with the HttpConnection.
    // Cancelled when the connection closes (timeout, disconnect, write error).
    // Check in long-running handlers to exit early.
    std::shared_ptr<CancellationToken> cancel_token;

    // Convenience: true if the underlying connection has been closed.
    bool is_cancelled() const noexcept {
        return cancel_token && cancel_token->is_cancelled();
    }

    // Convenience: get a header by name (case-insensitive)
    std::optional<std::string> header(std::string name) const {
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        auto it = headers.find(name);
        if (it == headers.end()) return std::nullopt;
        return it->second;
    }

    // Convenience: get a query param with a default
    std::string query_param(const std::string& key, const std::string& def = "") const {
        auto it = query.find(key);
        return (it != query.end()) ? it->second : def;
    }

    // Convenience: get a cookie value by name. Parses the Cookie header lazily;
    // the result is cached on the first call so repeated lookups are O(1).
    std::optional<std::string> cookie(const std::string& name) const {
        if (!cookies_parsed_) {
            cookies_parsed_ = true;
            auto h = header("cookie");
            if (h) cookies_cache_ = parse_cookie_header(*h);
        }
        auto it = cookies_cache_.find(name);
        if (it == cookies_cache_.end()) return std::nullopt;
        return it->second;
    }

    // Parse an application/x-www-form-urlencoded body.
    // Returns an empty map if Content-Type doesn't match or body is empty.
    std::unordered_map<std::string, std::string> form() const {
        auto ct = header("content-type");
        if (!ct || ct->find("application/x-www-form-urlencoded") == std::string::npos)
            return {};
        return parse_form_encoded(body);
    }

    // Mutable cache for parsed Cookie header — only populated on first cookie().
    mutable std::unordered_map<std::string, std::string> cookies_cache_;
    mutable bool                                          cookies_parsed_ = false;

private:
    static std::string form_url_decode(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') {
                out += ' ';
            } else if (s[i] == '%' && i + 2 < s.size()) {
                char buf[3] = {s[i+1], s[i+2], '\0'};
                char* end;
                unsigned long v = std::strtoul(buf, &end, 16);
                if (end == buf + 2) {
                    if (v != 0) out += static_cast<char>(v);  // reject %00 null bytes
                    i += 2;
                } else { out += '%'; }
            } else {
                out += s[i];
            }
        }
        return out;
    }

    static std::unordered_map<std::string, std::string>
    parse_form_encoded(const std::string& src) {
        std::unordered_map<std::string, std::string> result;
        size_t pos = 0;
        while (pos <= src.size()) {
            size_t amp = src.find('&', pos);
            if (amp == std::string::npos) amp = src.size();
            size_t eq = src.find('=', pos);
            if (eq != std::string::npos && eq < amp) {
                result[form_url_decode(src.substr(pos, eq - pos))] =
                    form_url_decode(src.substr(eq + 1, amp - eq - 1));
            } else if (amp > pos) {
                result[form_url_decode(src.substr(pos, amp - pos))] = "";
            }
            pos = amp + 1;
        }
        return result;
    }
};

} // namespace lumen
