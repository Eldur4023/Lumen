#pragma once
#include <string>
#include <string_view>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <iostream>
#include <algorithm>

namespace lumen {

// ─── SameSite ────────────────────────────────────────────────────────────────
//
// Controls the SameSite cookie attribute (RFC 6265bis §4.1.2.7).
//
//   Strict — cookie sent only on same-site requests.  Strongest CSRF defence.
//   Lax    — cookie sent on top-level navigations and same-site requests.
//            Default for modern browsers.
//   None   — cookie sent on all cross-site requests.  REQUIRES Secure=true.
//
enum class SameSite { Strict, Lax, None };

// ─── CookieOptions ───────────────────────────────────────────────────────────
struct CookieOptions {
    std::string                path     = "/";
    std::string                domain;                 // empty → no Domain attr
    std::optional<int>         max_age;                // seconds; nullopt → session cookie
    bool                       secure    = false;      // Secure attribute
    bool                       http_only = true;       // HttpOnly attribute (JS cannot read)
    SameSite                   same_site = SameSite::Lax;
};

// ─── build_set_cookie() ─────────────────────────────────────────────────────
//
// Serialises a Set-Cookie header value following RFC 6265.
// CR/LF in name or value is stripped to prevent header injection.
// SameSite=None without Secure is auto-promoted to Secure (browsers reject it
// otherwise) and a warning is emitted to stderr.
//
inline std::string build_set_cookie(std::string name, std::string value,
                                     CookieOptions opts = {}) {
    auto strip = [](std::string& s) {
        for (auto it = s.begin(); it != s.end(); ) {
            unsigned char c = *it;
            // RFC 6265: cookie-name and cookie-value forbid CTLs and separators.
            // We strip the dangerous ones aggressively to prevent header injection.
            if (c == '\r' || c == '\n' || c == ';' || c == ',' || c == ' ' || c == '\t')
                it = s.erase(it);
            else
                ++it;
        }
    };
    // Path/Domain are concatenated into the Set-Cookie header below.  Without
    // sanitising them, a handler that passes user input (e.g. tenant-specific
    // path) could inject arbitrary headers via CR/LF.
    auto strip_crlf = [](std::string& s) {
        s.erase(std::remove_if(s.begin(), s.end(),
            [](char c){ return c == '\r' || c == '\n'; }), s.end());
    };
    strip(name);
    strip(value);
    strip_crlf(opts.path);
    strip_crlf(opts.domain);

    // Browsers silently drop SameSite=None without Secure.  Auto-correct so the
    // cookie actually reaches the client.
    if (opts.same_site == SameSite::None && !opts.secure) {
        std::cerr << "[lumen] cookie '" << name
                  << "': SameSite=None requires Secure — auto-enabling Secure.\n";
        opts.secure = true;
    }

    std::string out;
    out.reserve(name.size() + value.size() + 64);
    out += name;
    out += '=';
    out += value;

    if (!opts.path.empty()) { out += "; Path="; out += opts.path; }
    if (!opts.domain.empty()) { out += "; Domain="; out += opts.domain; }
    if (opts.max_age) {
        out += "; Max-Age=";
        out += std::to_string(*opts.max_age);
    }
    if (opts.http_only) out += "; HttpOnly";
    if (opts.secure)    out += "; Secure";
    switch (opts.same_site) {
        case SameSite::Strict: out += "; SameSite=Strict"; break;
        case SameSite::Lax:    out += "; SameSite=Lax";    break;
        case SameSite::None:   out += "; SameSite=None";   break;
    }
    return out;
}

// ─── parse_cookie_header() ──────────────────────────────────────────────────
//
// Parses an HTTP `Cookie:` request header into a name→value map.
// Tolerates leading whitespace and missing values.  Quoted values keep their
// quotes (Servlet/RFC ambiguity — the caller can strip them if needed).
//
inline std::unordered_map<std::string, std::string>
parse_cookie_header(std::string_view header) {
    std::unordered_map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < header.size()) {
        // Skip leading whitespace
        while (pos < header.size() && (header[pos] == ' ' || header[pos] == '\t'))
            ++pos;
        size_t end = header.find(';', pos);
        if (end == std::string_view::npos) end = header.size();
        auto pair = header.substr(pos, end - pos);
        auto eq = pair.find('=');
        if (eq != std::string_view::npos) {
            std::string k(pair.substr(0, eq));
            std::string v(pair.substr(eq + 1));
            if (!k.empty()) out.emplace(std::move(k), std::move(v));
        }
        pos = end + 1;
    }
    return out;
}

} // namespace lumen
