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

static Resultado pedir(Module& mod, const std::string& metodo, const std::string& path,
                       const std::string& body = {}) {
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
    req.body   = body;

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
    const auto dir_db = std::filesystem::temp_directory_path() / "lumen_native_route_check";
    std::error_code ec_db;
    std::filesystem::create_directories(dir_db, ec_db);
    const auto archivo_db = dir_db / "sqlite_check.db";
    std::filesystem::remove(archivo_db, ec_db);

    const std::string src =
        "import sqlite\n"
        "\n"
        "app:\n"
        "    sqlite:\n"
        "        file \"" + archivo_db.string() + "\"\n"
        "\n"
        "class Ajuste:\n"
        "    string nombre\n"
        "    int?   extra\n"
        "\n"
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
        "    return redirect(\"/saluda/1\")\n"
        "\n"
        "get endpoint(\"/mod/:a/:b\", int a, int b):\n"
        "    int r = a % b\n"
        "    return { \"r\": r }\n"
        "\n"
        "get endpoint(\"/rango/:n\", int n):\n"
        "    require n >= 0 and n <= 10 else status(400)\n"
        "    List<int> xs = [n]\n"
        "    int i = 0\n"
        "    while i < n:\n"
        "        xs.add(i * i)\n"
        "        i = i + 1\n"
        "    return { \"n\": n, \"cuadrados\": xs }\n"
        "\n"
        "get endpoint(\"/contadores/:n\", int n):\n"
        "    require n >= 0 and n <= 10 else status(400)\n"
        "    Dict<string, int> d = {\"base\": n}\n"
        "    int i = 0\n"
        "    while i < n:\n"
        "        d[\"c\" + str(i)] = i * 2\n"
        "        i = i + 1\n"
        "    return { \"total\": d }\n"
        "\n"
        "get endpoint(\"/espera/:ms\", int ms):\n"
        "    require ms >= 0 and ms <= 3000 else status(400)\n"
        "    await sleep(ms)\n"
        "    return { \"waited_ms\": ms }\n"
        "\n"
        "get endpoint(\"/db/:id\", int id):\n"
        "    List<Json> filas = await sqlite.query(\"select id, nombre from cosas where id = ?\", id)\n"
        "    if len(filas) == 0:\n"
        "        return status(404)\n"
        "    Json fila = filas[0]\n"
        "    int fid = fila[\"id\"]\n"
        "    return { \"id\": fid, \"nombre\": fila[\"nombre\"] }\n"
        "\n"
        "post endpoint(\"/db/tx/:id\", int id):\n"
        "    await sqlite.begin()\n"
        "    await sqlite.exec(\"update cosas set nombre = ? where id = ?\", \"cambiado\", id)\n"
        "    if id == 0:\n"
        "        return status(400)\n"
        "    await sqlite.commit()\n"
        "    return status(204)\n"
        "\n"
        "put endpoint(\"/ajuste\", Ajuste datos):\n"
        "    if datos.extra != null:\n"
        "        return { \"nombre\": datos.nombre, \"extra\": datos.extra }\n"
        "    return { \"nombre\": datos.nombre, \"extra\": 0 }\n";

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

    // Fase 5.5: `/db/:id` (List<Json>/Json/indexado/len() sobre el
    // resultado de `await sqlite.query(...)`) tiene que compilar como ruta
    // asincrona -- SOLO se comprueba que compilo (rutas_informe, Fase 6),
    // no se ejecuta: un await sobre DbAwaitable de verdad suspende sobre un
    // DbPool y solo se reanuda cuando el EventLoop real hace
    // loop->post(...) -- esta prueba no tiene uno (a diferencia de `await
    // sleep()`, que se reanuda al acto sin loop, ver SleepAwaitable), asi
    // que ejecutarla aqui se quedaria colgada para siempre en vez de fallar
    // limpio. La ejecucion de verdad (con datos reales, contra el binario
    // real sirviendo HTTP) esta verificada a mano -- ver COMPILACION-NATIVA.md.
    {
        std::string via;
        for (const auto& r : mod_nat->rutas_informe)
            if (r.patron == "/db/:id") { via = r.via; break; }
        if (via != "nativa (async)") {
            std::printf("  FALLA /db/:id: se esperaba 'nativa (async)', rutas_informe dice "
                        "'%s'\n", via.empty() ? "(no aparece)" : via.c_str());
            ok = false;
        } else {
            std::printf("  ok    /db/:id compila como ruta asincrona (await sqlite.query + "
                        "Json + indexado + len())\n");
        }
    }

    // Fase 5.6: `/db/tx/:id` (await sqlite.begin()/exec()/commit(), con un
    // `return` de por medio ANTES del commit() -- a proposito, para que
    // generar_ruta_nativa tenga que generar el cierre de la transaccion
    // (rollback_pendientes_db, via ret_vacio()) en ese punto de salida
    // temprano, no solo al final del cuerpo) tiene que compilar como ruta
    // asincrona igual que /db/:id -- mismo motivo para no ejecutarla aqui
    // (DbAwaitable necesita un EventLoop real, ver el comentario de arriba).
    {
        std::string via;
        for (const auto& r : mod_nat->rutas_informe)
            if (r.patron == "/db/tx/:id") { via = r.via; break; }
        if (via != "nativa (async)") {
            std::printf("  FALLA /db/tx/:id: se esperaba 'nativa (async)', rutas_informe dice "
                        "'%s'\n", via.empty() ? "(no aparece)" : via.c_str());
            ok = false;
        } else {
            std::printf("  ok    /db/tx/:id compila como ruta asincrona (await sqlite.begin/"
                        "exec/commit(), con un return anticipado antes del commit)\n");
        }
    }

    // Fase 5.7: `/ajuste` (PUT endpoint("/ajuste", Ajuste datos), un
    // parametro de tipo clase enlazado al cuerpo JSON, con un campo
    // obligatorio (`string nombre`) y uno opcional (`int? extra`), SIN
    // `validate:`) -- a diferencia de /db/:id y /db/tx/:id, esta ruta no
    // usa `await`: compila SINCRONA, asi que se puede EJECUTAR de verdad
    // aqui (sin EventLoop) y comparar bytecode contra --native byte a
    // byte, no solo comprobar que compila.
    {
        std::string via;
        for (const auto& r : mod_nat->rutas_informe)
            if (r.patron == "/ajuste") { via = r.via; break; }
        if (via != "nativa") {
            std::printf("  FALLA /ajuste: se esperaba 'nativa', rutas_informe dice '%s'\n",
                        via.empty() ? "(no aparece)" : via.c_str());
            ok = false;
        } else {
            std::printf("  ok    /ajuste compila como ruta sincrona (parametro de cuerpo, "
                        "campo obligatorio + campo `?`, sin validate:)\n");
        }
    }
    auto comparar_cuerpo = [&](const char* etiqueta, const std::string& body) {
        Resultado bc  = pedir(*mod_bc, "PUT", "/ajuste", body);
        Resultado nat = pedir(*mod_nat, "PUT", "/ajuste", body);
        if (!bc.encontrada || !nat.encontrada) {
            std::printf("  FALLA %s: la ruta no aparece en el router (bytecode=%d nativo=%d)\n",
                        etiqueta, bc.encontrada, nat.encontrada);
            ok = false;
            return;
        }
        if (bc.status != nat.status || bc.body != nat.body) {
            std::printf("  FALLA %s: bytecode(%d, '%s') != nativo(%d, '%s')\n", etiqueta,
                        bc.status, bc.body.c_str(), nat.status, nat.body.c_str());
            ok = false;
        } else {
            std::printf("  ok    %s: bytecode y --native dan (%d, '%s') en las dos vias\n",
                        etiqueta, bc.status, bc.body.c_str());
        }
    };
    // Campo opcional presente.
    comparar_cuerpo("cuerpo completo", R"({"nombre":"a","extra":7})");
    // Campo opcional ausente: no es error, `datos.extra != null` da falso.
    comparar_cuerpo("campo opcional ausente", R"({"nombre":"a"})");
    // Campo opcional explicitamente null: mismo caso que ausente.
    comparar_cuerpo("campo opcional null", R"({"nombre":"a","extra":null})");
    // Campo obligatorio ausente: 422 con el mensaje "nombre: obligatorio".
    comparar_cuerpo("campo obligatorio ausente", R"({"extra":3})");
    // Campo obligatorio de tipo equivocado.
    comparar_cuerpo("campo obligatorio tipo invalido", R"({"nombre":5})");
    // Campo opcional de tipo equivocado (SI presente, tiene que ser el tipo
    // correcto -- opcional no es "cualquier cosa vale").
    comparar_cuerpo("campo opcional tipo invalido", R"({"nombre":"a","extra":"x"})");
    // JSON mal formado.
    comparar_cuerpo("JSON invalido", R"({"nombre":)");
    // Cuerpo bien formado pero no es un objeto.
    comparar_cuerpo("cuerpo no es objeto", R"([1,2,3])");

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

    // Una List<int> YA construida (no un ListLit) dentro del dict de
    // retorno: Generador::valor_json() la convierte con lumen_valor_de(),
    // iterando LList<T> con lumen_len()/lumen_get() -- a diferencia de un
    // DictLit/ListLit literal, esta rama no exige homogeneidad porque la
    // lista YA es homogenea por construccion (es un tipo nativo, no JSON
    // heterogeneo).
    comparar("List<int> ya construida en el retorno", "/rango/0");
    comparar("List<int> ya construida en el retorno", "/rango/5");

    // Igual que la List<T> de arriba, pero un Dict<string,int> ya
    // construido: lumen_valor_de() recorre TODOS los pares con
    // lumen_len()/lumen_key_at()/lumen_val_at() (nuevos en LDict, sin
    // equivalente en el lenguaje -- solo para este puente), preservando el
    // orden de insercion (LDict es un vector, igual que Value::Dict).
    comparar("Dict<string,int> ya construido en el retorno", "/contadores/0");
    comparar("Dict<string,int> ya construido en el retorno", "/contadores/4");

    // Fase 5: `await sleep(ms)` -- la ruta se genera como lumen::Task<void>
    // de verdad (RutaNativa::asincrona), no como una funcion void. Esta
    // prueba, sin un event loop real detras (current_loop queda a nullptr
    // en este binario, nunca lo pone HttpConnection::dispatch()), no
    // verifica el TIEMPO de espera -- SleepAwaitable::await_suspend()
    // reanuda al acto sin loop, asi que un solo handle.resume() basta --
    // solo que status+cuerpo coincidan, exactamente igual que cualquier
    // otra ruta. El tiempo de espera de verdad (~ms reales, no ~0) esta
    // verificado a mano contra el binario real sirviendo HTTP -- ver
    // COMPILACION-NATIVA.md.
    comparar("guarda rechaza ms=9999", "/espera/9999");
    comparar("await sleep(ms)", "/espera/50");

    // Bug real, encontrado probando esto a proposito (no una precaucion
    // especulativa): una ruta nativa no tiene NINGUN wrapper de la ABI (a
    // diferencia de una funcion) -- nadie la llama a traves de ella, la
    // invoca build_routes() directamente -- asi que, antes de que
    // generar_ruta_nativa() envolviera el cuerpo entero en un try/catch, un
    // modulo por cero (lumen_mod_check, el mismo canal de error que ya usan
    // las funciones) escapaba de la corrutina sin que nadie lo atrapara.
    // Confirmado contra el binario real: NO tumbaba el proceso (Task<void>
    // absorbe la excepcion en su unhandled_exception()), pero daba
    // {"error":"Internal Server Error"} -- el generico de http_connection.cpp
    // para una excepcion sin atrapar -- en vez de {"error":"modulo por
    // cero","en":"..."} que da bytecode: 500 en las dos vias, pero un cuerpo
    // distinto. El mensaje de "error" tiene que coincidir EXACTO (mismo
    // lumen_native_error_message()); "en" no -- el codigo nativo no lleva
    // ninguna nocion de linea/columna en tiempo de ejecucion, asi que usa
    // "METODO patron" en vez del "archivo:linea:col" que da el VM.
    {
        Resultado bc  = pedir(*mod_bc, "GET", "/mod/10/0");
        Resultado nat = pedir(*mod_nat, "GET", "/mod/10/0");
        Value     bc_v, nat_v;
        bool      parsea = Value::parse_json(bc.body, bc_v) && Value::parse_json(nat.body, nat_v);
        if (!bc.encontrada || !nat.encontrada || bc.status != 500 || nat.status != 500 ||
            !parsea || !bc_v.is_dict() || !nat_v.is_dict() ||
            !bc_v.as_dict().count("error") || !nat_v.as_dict().count("error") ||
            bc_v.as_dict().find("error")->second.as_str() !=
                nat_v.as_dict().find("error")->second.as_str()) {
            std::printf("  FALLA modulo por cero en ruta: bytecode(%d, '%s') vs nativo(%d, "
                        "'%s')\n", bc.status, bc.body.c_str(), nat.status, nat.body.c_str());
            ok = false;
        } else {
            std::printf("  ok    modulo por cero en ruta: las dos vias dan 500 con el mismo "
                        "mensaje de error ('%s'), el proceso sigue vivo\n",
                        nat_v.as_dict().find("error")->second.as_str().c_str());
        }
        // El proceso (bueno, el Module -- no hay proceso aparte en esta
        // prueba) tiene que seguir sirviendo despues del error.
        Resultado vivo = pedir(*mod_nat, "GET", "/mod/10/3");
        if (!vivo.encontrada || vivo.status != 200) {
            std::printf("  FALLA: la ruta nativa no responde tras el modulo por cero anterior\n");
            ok = false;
        }
    }

    if (ok) {
        std::printf("native_route_shadow: las rutas nativas coinciden con bytecode en todos "
                    "los casos\n");
    } else {
        std::printf("native_route_shadow: %d fallo(s)\n", ++fallos);
    }

    std::filesystem::remove_all(dir, ec);
    return ok ? 0 : 1;
}
