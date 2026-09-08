// Fase 4 de --native (COMPILACION-NATIVA.md): la primera ruta HTTP
// compilada a codigo nativo. A diferencia de las pruebas anteriores (que
// arman un Program/Emitter/VM a mano), esta pasa por compile() de verdad --
// el mismo camino que toma `lumen --native app.lum` -- y despacha peticiones
// contra mod->router tal como lo haria HttpConnection, comparando el
// resultado de compilar el MISMO fuente con native=false y con native=true.
//
// El objetivo es que una ruta nativa sea indistinguible, desde fuera
// (status + cabeceras + cuerpo), de la misma ruta servida por bytecode --
// guarda de grupo (`require ... else status(400)`), un parametro de patron
// mal tipado (el mismo 400 exacto que prepare_args), un parametro de query
// con valor por defecto, el valor de retorno heterogeneo (`{"n": n,
// "result": r}`, generado con el puente Value de Generador::valor_json en
// vez de Dict<V> homogeneo), y las otras funciones globales que escriben la
// respuesta ellas mismas (text/html/status/redirect como "else" de una
// guarda o como el propio `return`, ver Comprobador::es_llamada_respuesta).
#include <lumen_script/project.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace lumen_script;

static int fallos = 0;

// Ejecuta el Task<void> hasta el final. Ninguna ruta de esta prueba usa
// `await`: el handler (nativo o bytecode) nunca suspende de verdad, asi que
// un solo resume() basta -- no hace falta event loop.
static void ejecutar_sincrono(lumen::Task<void> t) {
    if (!t.handle.done()) t.handle.resume();
}

struct Resultado {
    bool        encontrada = false;
    int         status     = 0;
    std::string body;
    std::string location; // header "Location" (redirect()), vacio si no hay
};

static Resultado pedir(Module& mod, const std::string& metodo, const std::string& path) {
    // Query string: match() (igual que HttpConnection, que la separa ANTES
    // de llamar a match()) solo entiende el patron -- un '?' colado en el
    // path que se le pasa nunca encontraria la ruta.
    auto        qpos     = path.find('?');
    std::string solo_ruta = (qpos == std::string::npos) ? path : path.substr(0, qpos);

    lumen::RouteMatch m = mod.router.match(metodo, solo_ruta);
    if (!m.found) return {};

    lumen::Request  req;
    lumen::Response res;
    req.method = metodo;
    req.path   = solo_ruta;
    req.params = m.params;

    if (qpos != std::string::npos) {
        std::string qs = path.substr(qpos + 1);
        size_t i = 0;
        while (i < qs.size()) {
            size_t amp = qs.find('&', i);
            if (amp == std::string::npos) amp = qs.size();
            size_t eq = qs.find('=', i);
            if (eq != std::string::npos && eq < amp)
                req.query[qs.substr(i, eq - i)] = qs.substr(eq + 1, amp - eq - 1);
            i = amp + 1;
        }
    }

    ejecutar_sincrono(m.handler(req, res));
    const auto&      hdrs = res.headers_map();
    auto              it  = hdrs.find("Location");
    return {true, res.status_code(), res.body(), it != hdrs.end() ? it->second : std::string()};
}

int main() {
    const std::string src =
        "fn int fib(int n):\n"
        "    if n < 2:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "\n"
        "get endpoint(\"/compute/fib/:n\", int n):\n"
        "    require n >= 1 and n <= 32 else status(400)\n"
        "    int r = fib(n)\n"
        "    return { \"n\": n, \"result\": r }\n"
        "\n"
        "get endpoint(\"/compute/fibq\", int n = 5):\n"
        "    require n >= 0 and n <= 32 else status(400)\n"
        "    return { \"n\": n, \"result\": fib(n) }\n"
        "\n"
        "get endpoint(\"/saluda/:n\", int n):\n"
        "    require n >= 1 and n <= 100 else text(\"fuera de rango\")\n"
        "    if n > 50:\n"
        "        return html(\"<b>grande</b>\")\n"
        "    return status(204)\n"
        "\n"
        "get endpoint(\"/ir/:n\", int n):\n"
        "    require n >= 1 else redirect(\"/saluda/1\", 301)\n"
        "    return redirect(\"/saluda/1\")\n";

    const auto dir  = std::filesystem::temp_directory_path() / "lumen_native_route_check";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto archivo = dir / "app.lum";
    {
        std::ofstream out(archivo, std::ios::trunc);
        out << src;
    }

    // El cache de --native (compile() lo hornea como ".lumen-native",
    // relativo al directorio de trabajo del proceso -- no configurable
    // desde aqui) se limpia antes de cada compilacion para que un fallo
    // previo no deje un .so obsoleto que dlopen() cargue por error.
    std::filesystem::remove_all(".lumen-native", ec);
    DiagnosticBag diags_bc;
    auto mod_bc = compile({archivo}, diags_bc, /*native=*/false);
    if (!diags_bc.empty()) {
        std::printf("FALLA: compilacion bytecode con errores: %s\n",
                    diags_bc.items().front().message.c_str());
        return 1;
    }

    std::filesystem::remove_all(".lumen-native", ec);
    DiagnosticBag diags_nat;
    auto mod_nat = compile({archivo}, diags_nat, /*native=*/true);
    std::filesystem::remove_all(".lumen-native", ec);
    if (!diags_nat.empty()) {
        std::printf("FALLA: compilacion --native con errores: %s\n",
                    diags_nat.items().front().message.c_str());
        return 1;
    }
    if (!mod_nat->native_aviso.empty())
        std::printf("aviso de --native: %s\n", mod_nat->native_aviso.c_str());

    // Si esto no compilo a nativo, el resto de la prueba compararia
    // bytecode contra bytecode -- no probaria nada nuevo. rutas_compiladas()
    // no es publico (solo el conteo total via compiladas()), asi que basta
    // con pedir una respuesta y comprobar mas abajo que las dos vias
    // coinciden; el fallo real que esto quiere atrapar (no compilar nada)
    // ya lo señala native_aviso arriba con detalle. Aun asi, se deja
    // constancia explicita: sin mod_nat->native, la ruta cayo entera a
    // bytecode y esta prueba no vale para nada.
    if (!mod_nat->native) {
        std::printf("FALLA: --native no compilo nada (se esperaba que /compute/fib/:n "
                    "compilase entera)\n");
        return 1;
    }

    bool ok = true;
    auto comparar = [&](const char* etiqueta, const std::string& path) {
        Resultado bc  = pedir(*mod_bc, "GET", path);
        Resultado nat = pedir(*mod_nat, "GET", path);
        if (!bc.encontrada || !nat.encontrada) {
            std::printf("  FALLA %s: la ruta no aparece en el router (bytecode=%d nativo=%d)\n",
                        etiqueta, bc.encontrada, nat.encontrada);
            ok = false;
            return;
        }
        if (bc.status != nat.status || bc.body != nat.body || bc.location != nat.location) {
            std::printf("  FALLA %s: bytecode(%d, '%s', Location='%s') != nativo(%d, '%s', "
                        "Location='%s')\n", etiqueta, bc.status, bc.body.c_str(),
                        bc.location.c_str(), nat.status, nat.body.c_str(), nat.location.c_str());
            ok = false;
        } else {
            std::printf("  ok    %s: bytecode y --native dan (%d, '%s'%s%s) en las dos vias\n",
                        etiqueta, bc.status, bc.body.c_str(),
                        bc.location.empty() ? "" : ", Location=", bc.location.c_str());
        }
    };

    // Camino feliz: la guarda pasa, fib(10) se calcula nativamente, y el
    // dict de retorno (int + int, pero generado con Value, no Dict<V>) se
    // serializa igual que el que produce el VM.
    comparar("fib(10)", "/compute/fib/10");

    // La guarda (`require ... else status(400)`): n=50 esta fuera de rango.
    // El handler nativo escribe res.status(400).send("") el mismo, sin
    // pasar por prepare_args ni por el VM -- tiene que dar el 204/400 exacto
    // que da build_routes con status().
    comparar("guarda rechaza n=50", "/compute/fib/50");

    // El parametro de patron no es un entero: el binding que genera
    // generar_ruta_nativa reproduce coerce()/prepare_args a mano -- este es
    // el caso que prueba que el 400 {"error":"parametro invalido",...} sale
    // BYTE A BYTE igual, JSON-escapado incluido.
    comparar("parametro invalido n='abc'", "/compute/fib/abc");

    // Query con valor por defecto (`int n = 5`): ausente usa el defecto (el
    // mismo texto que bind_params() extrae del AST, corriendo por el MISMO
    // coerce() que un valor real), presente lo sustituye, y mal tipada da
    // el mismo 400 -- las tres vias que prepare_args() distingue.
    comparar("query con defecto, ausente", "/compute/fibq");
    comparar("query con defecto, presente", "/compute/fibq?n=8");
    comparar("query con defecto, mal tipada", "/compute/fibq?n=xyz");

    // es_llamada_respuesta(): las otras cinco funciones globales que
    // escriben la respuesta ellas mismas (text/html/status como valor de
    // Return, no solo como "else" de una guarda) -- cada una debe dar el
    // mismo status+cuerpo+cabecera que su fn_* en natives.cpp.
    comparar("guarda con text()", "/saluda/200");           // fuera de rango
    comparar("return html()", "/saluda/80");                 // n > 50
    comparar("return status(204)", "/saluda/10");             // n <= 50
    comparar("guarda con redirect(url, codigo)", "/ir/0");    // n < 1
    comparar("return redirect(url)", "/ir/5");

    if (ok) {
        std::printf("native_route_shadow: las rutas nativas coinciden con bytecode en todos "
                    "los casos\n");
    } else {
        std::printf("native_route_shadow: %d fallo(s)\n", ++fallos);
    }

    std::filesystem::remove_all(dir, ec);
    return ok ? 0 : 1;
}
