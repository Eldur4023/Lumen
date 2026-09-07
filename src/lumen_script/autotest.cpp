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

// ─── Cliente HTTP minimo ─────────────────────────────────────────────────────
//
// Se habla por el socket de verdad, no por dentro: asi la sonda recorre el
// mismo camino que una peticion real —parser, router, middleware— y no solo el
// handler.

struct Respuesta {
    bool        conectado = false;
    int         codigo    = 0;
    long long   ms        = 0;
    size_t      bytes     = 0;
};

Respuesta pedir(uint16_t port, const std::string& metodo, const std::string& ruta,
                const std::string& cuerpo, int timeout_ms, bool leer_flujo,
                int stream_ms) {
    Respuesta r;
    auto t0 = std::chrono::steady_clock::now();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return r;

    timeval tv{};
    int espera = leer_flujo ? stream_ms : timeout_ms;
    tv.tv_sec  = espera / 1000;
    tv.tv_usec = (espera % 1000) * 1000;
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
    r.conectado = true;

    std::string pet = metodo + " " + ruta + " HTTP/1.1\r\n"
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

    std::string recibido;
    char        buf[4096];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        recibido.append(buf, static_cast<size_t>(n));
        // De un flujo basta con la cabecera y algo de cuerpo: no termina nunca.
        if (leer_flujo && recibido.size() > 64) break;
        if (recibido.size() > 1u << 20) break;
    }
    ::close(fd);

    if (recibido.rfind("HTTP/1.1 ", 0) == 0)
        r.codigo = std::atoi(recibido.c_str() + 9);
    r.bytes = recibido.size();
    r.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count();
    return r;
}

// ─── Sintesis de valores ─────────────────────────────────────────────────────

// Un valor plausible para un parametro, por tipo.  No pretende pasar las reglas
// de `validate`: un 422 tambien es una respuesta valida y se informa como tal.
std::string valor_para(const std::string& tipo) {
    if (tipo == "int" || tipo == "long")     return "1";
    if (tipo == "float" || tipo == "double") return "1.5";
    if (tipo == "bool")                      return "true";
    return "prueba";
}

std::string json_para(const std::string& tipo) {
    if (tipo == "int" || tipo == "long")     return "1";
    if (tipo == "float" || tipo == "double") return "1.5";
    if (tipo == "bool")                      return "true";
    return "\"prueba\"";
}

// Rellena :id / {id} y anade las query con valor.
std::string ruta_concreta(const RouteDecl& r) {
    std::string out;
    size_t i = 0;
    while (i < r.pattern.size()) {
        char c = r.pattern[i];
        if (c == ':' || c == '{') {
            char cierre = (c == '{') ? '}' : '/';
            size_t j = i + 1;
            while (j < r.pattern.size() && r.pattern[j] != cierre) ++j;
            std::string nombre = r.pattern.substr(i + 1, j - i - 1);

            std::string tipo = "string";
            for (const auto& p : r.params) if (p.name == nombre) tipo = p.type.name;
            out += valor_para(tipo);
            i = (c == '{') ? j + 1 : j;
        } else if (c == '*') {
            out += "prueba";
            ++i;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// Cuerpo JSON a partir de la clase que la ruta espera, si espera alguna.
std::string cuerpo_para(const RouteDecl& r, const Program& programa) {
    for (const auto& p : r.params) {
        for (const auto& c : programa.classes) {
            if (c.name != p.type.name) continue;
            std::string j = "{";
            bool primero = true;
            for (const auto& f : c.fields) {
                if (f.type.optional) continue;          // los opcionales se omiten
                if (!primero) j += ",";
                j += "\"" + f.name + "\":" + json_para(f.type.name);
                primero = false;
            }
            return j + "}";
        }
    }
    return {};
}

const char* veredicto(const Respuesta& r, bool flujo) {
    if (!r.conectado)                 return "SIN CONEXION";
    if (flujo && r.codigo == 200)     return "flujo";
    if (r.codigo >= 500)              return "ERROR";
    if (r.codigo >= 400)              return "rechazada";
    if (r.codigo >= 200)              return "ok";
    return "?";
}

bool metodo_seguro(const std::string& m) {
    return m == "GET" || m == "HEAD" || m == "SSE";
}

} // namespace

void run_autotest(const Module& mod, const AutotestOptions& opts) {
    if (!opts.enabled) return;

    const auto& rutas = mod.program.routes;
    if (rutas.empty()) return;

    std::ostringstream cab;
    cab << "autotest: probando " << rutas.size() << " ruta(s)";
    if (!opts.unsafe) cab << " (solo metodos seguros; --autotest=all incluye el resto)";
    lumen::log().info(cab.str());

    int ok = 0, rechazadas = 0, errores = 0, omitidas = 0;

    for (const auto& r : rutas) {
        std::string metodo = r.method;

        if (metodo == "WS") {
            lumen::log().info("  omitida   WS   " + r.pattern +
                               "   (necesita handshake de WebSocket)");
            ++omitidas;
            continue;
        }
        if (!opts.unsafe && !metodo_seguro(metodo)) {
            lumen::log().info("  omitida   " + metodo + "  " + r.pattern +
                               "   (metodo con efectos)");
            ++omitidas;
            continue;
        }

        bool flujo = (metodo == "SSE");
        if (flujo || metodo == "*") metodo = "GET";

        std::string ruta   = ruta_concreta(r);
        std::string cuerpo = metodo_seguro(r.method) ? std::string()
                                                     : cuerpo_para(r, mod.program);

        Respuesta res = pedir(opts.port, metodo, ruta, cuerpo,
                              opts.timeout_ms, flujo, opts.stream_ms);

        const char* v = veredicto(res, flujo);
        if      (std::strcmp(v, "ERROR") == 0 ||
                 std::strcmp(v, "SIN CONEXION") == 0) ++errores;
        else if (std::strcmp(v, "rechazada") == 0)    ++rechazadas;
        else                                          ++ok;

        std::ostringstream linea;
        linea << "  " << std::left << std::setw(10) << v
              << std::setw(7) << metodo << std::setw(34) << ruta;
        if (res.conectado) linea << res.codigo << "  " << res.ms << "ms";
        linea << "";
        lumen::log().info(linea.str());
    }

    std::ostringstream fin;
    fin << "autotest: " << ok << " ok, " << rechazadas << " rechazadas, "
        << errores << " con error";
    if (omitidas) fin << ", " << omitidas << " omitidas";

    // Un 5xx es lo unico que significa "esto se ha roto": un 4xx puede ser el
    // comportamiento correcto de una guarda o de una validacion.
    if (errores) lumen::log().error(fin.str());
    else         lumen::log().info(fin.str());
}

} // namespace lumen_script
