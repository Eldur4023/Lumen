#include <lumen_script/autotest.hpp>

#include <lumen/logger.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace lumen_script {

namespace {

// ─── Minimal HTTP client ─────────────────────────────────────────────────────
//
// It talks over the real socket, not internally: that way the probe walks the
// same path as a real request —parser, router, middleware— and not just the
// handler.

struct Response {
    bool        connected = false;
    int         code    = 0;
    long long   ms        = 0;
    size_t      bytes     = 0;
};

Response pedir(uint16_t port, const std::string& method, const std::string& path,
                const std::string& cuerpo, int timeout_ms, bool read_stream,
                int stream_ms) {
    Response r;
    auto t0 = std::chrono::steady_clock::now();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    timeval tv{};
    int expects = read_stream ? stream_ms : timeout_ms;
    tv.tv_sec  = expects / 1000;
    tv.tv_usec = (expects % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_in dir{};
    dir.sin_family = AF_INET;
    dir.sin_port   = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &dir.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&dir), sizeof(dir)) < 0) {
        ::close(fd);
        return r;
    }
    r.connected = true;

    std::string pet = method + " " + path + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Connection: close\r\n"
                      "X-Lumen-Autotest: 1\r\n";
    if (!cuerpo.empty()) {
        pet += "Content-Type: application/json\r\n";
        pet += "Content-Length: " + std::to_string(cuerpo.size()) + "\r\n";
    }
    pet += "\r\n" + cuerpo;

    size_t enviado = 0;
    while (enviado < pet.size()) {
        ssize_t n = ::send(fd, pet.data() + enviado, pet.size() - enviado, MSG_NOSIGNAL);
        if (n <= 0) { ::close(fd); return r; }
        enviado += static_cast<size_t>(n);
    }

    std::string received;
    char        buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        received.append(buf, static_cast<size_t>(n));
        // For a stream the header and some body are enough: it never ends.
        if (read_stream && received.size() > 64) break;
        if (received.size() > 1u << 20) break;
    }
    ::close(fd);

    if (received.rfind("HTTP/1.1 ", 0) == 0)
        r.code = std::atoi(received.c_str() + 9);
    r.bytes = received.size();
    r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
    return r;
}

// ─── Value synthesis ─────────────────────────────────────────────────────────

// A plausible value for a parameter, by type.  It does not aim to pass the
// `validate` rules: a 422 is also a valid response and is reported as such.
std::string value_for(const std::string& type) {
    if (type == "int" || type == "long")     return "1";
    if (type == "float" || type == "double") return "1.5";
    if (type == "bool")                      return "true";
    return "test";
}

std::string json_para(const std::string& type) {
    if (type == "int" || type == "long")     return "1";
    if (type == "float" || type == "double") return "1.5";
    if (type == "bool")                      return "true";
    return "\"test\"";
}

// Fills in :id / {id} and appends the query parameters with a value.
std::string concrete_route(const RouteDecl& r) {
    std::string out;
    size_t i = 0;
    while (i < r.pattern.size()) {
        char c = r.pattern[i];
        if (c == ':' || c == '{') {
            char cierre = (c == '{') ? '}' : '/';
            size_t j = i + 1;
            while (j < r.pattern.size() && r.pattern[j] != cierre) ++j;
            std::string name = r.pattern.substr(i + 1, j - i - 1);

            std::string type = "string";
            for (const auto& p : r.params) if (p.name == name) type = p.type.name;
            out += value_for(type);
            i = (c == '{') ? j + 1 : j;
        } else if (c == '*') {
            out += "test";
            ++i;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// JSON body from the class the route expects, if it expects one.
std::string body_for(const RouteDecl& r, const Program& programa) {
    for (const auto& p : r.params) {
        for (const auto& c : programa.classes) {
            if (c.name != p.type.name) continue;
            std::string j = "{";
            bool first = true;
            for (const auto& f : c.fields) {
                if (f.type.optional) continue;          // the optional ones are skipped
                if (!first) j += ",";
                j += "\"" + f.name + "\":" + json_para(f.type.name);
                first = false;
            }
            return j + "}";
        }
    }
    return {};
}

const char* veredicto(const Response& r, bool stream) {
    if (!r.connected)                 return "SIN CONEXION";
    if (stream && r.code == 200)     return "stream";
    if (r.code >= 500)              return "ERROR";
    if (r.code >= 400)              return "rejected";
    if (r.code >= 200)              return "ok";
    return "?";
}

bool safe_method(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "SSE";
}

} // namespace

void run_autotest(const Module& mod, const AutotestOptions& opts) {
    if (!opts.enabled) return;

    const auto& routes = mod.program.routes;
    if (routes.empty()) return;

    std::ostringstream cab;
    cab << "autotest: probando " << routes.size() << " path(s)";
    if (!opts.unsafe) cab << " (safe methods only; --autotest=all includes the rest)";
    lumen::log().info(cab.str());

    int ok = 0, rechazadas = 0, errores = 0, omitidas = 0;

    for (const auto& r : routes) {
        std::string method = r.method;

        if (method == "WS") {
            lumen::log().info("  omitida   WS   " + r.pattern +
                               "   (needs a WebSocket handshake)");
            ++omitidas;
            continue;
        }
        if (!opts.unsafe && !safe_method(method)) {
            lumen::log().info("  omitida   " + method + "  " + r.pattern +
                               "   (method with side effects)");
            ++omitidas;
            continue;
        }

        bool stream = (method == "SSE");
        if (stream || method == "*") method = "GET";

        std::string path   = concrete_route(r);
        std::string cuerpo = safe_method(r.method) ? std::string()
                                                     : body_for(r, mod.program);

        Response res = pedir(opts.port, method, path, cuerpo,
                              opts.timeout_ms, stream, opts.stream_ms);

        const char* v = veredicto(res, stream);
        if      (std::strcmp(v, "ERROR") == 0 ||
                 std::strcmp(v, "SIN CONEXION") == 0) ++errores;
        else if (std::strcmp(v, "rejected") == 0)    ++rechazadas;
        else                                          ++ok;

        std::ostringstream line;
        line << "  " << std::left << std::setw(10) << v
              << std::setw(7) << method << std::setw(34) << path;
        if (res.connected) line << res.code << "  " << res.ms << "ms";
        line << "";
        lumen::log().info(line.str());
    }

    std::ostringstream fin;
    fin << "autotest: " << ok << " ok, " << rechazadas << " rechazadas, "
        << errores << " with an error";
    if (omitidas) fin << ", " << omitidas << " omitidas";

    // A 5xx is the only thing that means "this broke": a 4xx can be the correct
    // behaviour of a guard or a validation.
    if (errores) lumen::log().error(fin.str());
    else         lumen::log().info(fin.str());
}

} // namespace lumen_script
