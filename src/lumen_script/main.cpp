// El binario de Lumen 2.0: lee ficheros .lum y sirve.
#include <lumen_script/project.hpp>
#include <lumen_script/vm.hpp>
#include <lumen_script/autotest.hpp>
#include <lumen_script/db.hpp>

#include <lumen/app.hpp>
#include <lumen/middleware.hpp>
#include <lumen/logger.hpp>
#include <lumen/openapi.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

namespace fs = std::filesystem;

namespace {

// El modulo vivo.  El dispatcher lo lee en cada peticion y el watcher lo
// reemplaza al recompilar: cambiar de version es publicar un shared_ptr, no
// tocar el router que se esta usando.
std::shared_ptr<lumen_script::Module> g_module;
std::mutex                    g_module_mutex;
std::atomic<bool>             g_stop{false};
lumen_script::AutotestOptions         g_autotest;

// La sonda habla por el socket, asi que no puede correr en el hilo que acaba de
// recargar: se lanza aparte y se le deja terminar sola.
void lanzar_autotest(std::shared_ptr<lumen_script::Module> mod) {
    if (!g_autotest.enabled || !mod) return;
    std::thread([mod] { lumen_script::run_autotest(*mod, g_autotest); }).detach();
}

std::shared_ptr<lumen_script::Module> current_module() {
    std::lock_guard<std::mutex> lk(g_module_mutex);
    return g_module;
}

void publish_module(std::shared_ptr<lumen_script::Module> m) {
    std::lock_guard<std::mutex> lk(g_module_mutex);
    g_module = std::move(m);
}

void usage() {
    std::cerr <<
        "uso: lumen [opciones] <fichero.lum | ficheros... | directorio>\n"
        "\n"
        "  Un fichero      compila solo ese fichero\n"
        "  Varios ficheros compila solo esos\n"
        "  Un directorio   compila todos los .lum que contenga, recursivamente\n"
        "\n"
        "opciones:\n"
        "  --check       compila y sale, sin arrancar el servidor\n"
        "  --port N      sobrescribe el puerto del bloque app:\n"
        "  --no-watch    no vigilar cambios en los ficheros\n"
        "  --verbose     registrar por consola cada peticion que llega\n"
        "  --autotest    al arrancar y en cada recarga, recorre los endpoints\n"
        "  --autotest=all  incluye tambien POST/PUT/PATCH/DELETE\n";
}

// Vigila los ficheros compilados y recompila al detectar un cambio.
// Si la nueva version no compila, se imprime el error y se sigue sirviendo la
// anterior: un typo nunca tumba el servidor.
void watch_loop(std::vector<fs::path> inputs) {
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (g_stop.load()) return;

        auto mod = current_module();
        if (!mod) continue;

        bool changed = false;
        std::error_code ec;
        for (const auto& [path, stamp] : mod->stamps) {
            auto now = fs::last_write_time(path, ec);
            if (!ec && now != stamp) { changed = true; break; }
        }
        if (!changed) continue;

        lumen::log().info("cambios detectados: recompilando");

        // Margen para que el editor termine de escribir el fichero.
        std::this_thread::sleep_for(std::chrono::milliseconds(120));

        lumen_script::DiagnosticBag diags;
        auto next = lumen_script::compile(inputs, diags);
        if (!diags.empty()) {
            // Los diagnosticos apuntan a los ficheros del intento fallido, no a
            // los del modulo vivo: hay que formatear con next->files, que sigue
            // vivo mientras `next` lo este.
            std::cerr << "\n" << lumen_script::format_errors(diags, next->files)
                      << "recarga cancelada: se sigue sirviendo la version anterior\n\n";
            // Se refrescan los mtimes del modulo vivo para no reintentar en
            // bucle sobre un fichero que sigue roto.
            for (auto& [path, stamp] : mod->stamps) {
                auto now = fs::last_write_time(path, ec);
                if (!ec) stamp = now;
            }
            continue;
        }

        publish_module(next);
        lumen::log().info("recargado: " + std::to_string(next->program.routes.size()) +
                           " ruta(s) — " + std::to_string(next->declarative_routes) +
                           " declarativa(s), " + std::to_string(next->vm_routes) +
                           " con logica");
        lanzar_autotest(next);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    bool check_only = false, watch = true, verbose = false;
    int  port_override = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--check")    check_only = true;
        else if (a == "--no-watch") watch = false;
        else if (a == "--verbose")  verbose = true;
        else if (a == "--autotest")     g_autotest.enabled = true;
        else if (a == "--autotest=all") { g_autotest.enabled = true; g_autotest.unsafe = true; }
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else if (a == "--port") {
            if (i + 1 >= argc) { std::cerr << "--port necesita un numero\n"; return 2; }
            port_override = std::atoi(argv[++i]);
        }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "opcion desconocida: " << a << "\n";
            usage();
            return 2;
        }
        else args.push_back(a);
    }

    if (args.empty()) { usage(); return 2; }

    std::vector<fs::path> inputs;
    std::string error;
    if (!lumen_script::resolve_inputs(args, inputs, error)) {
        std::cerr << "lumen: " << error << "\n";
        return 2;
    }

    lumen_script::DiagnosticBag diags;
    auto mod = lumen_script::compile(inputs, diags);
    if (!diags.empty()) {
        std::cerr << lumen_script::format_errors(diags, mod->files)
                  << "\n" << diags.size() << " error(es)\n";
        return 1;
    }

    std::cout << "lumen: " << inputs.size() << " fichero(s), "
              << mod->program.routes.size() << " ruta(s) — "
              << mod->declarative_routes << " declarativa(s), "
              << mod->vm_routes << " con logica\n";
    if (check_only) return 0;

    publish_module(mod);

    const lumen_script::AppDecl& cfg = mod->program.app;
    lumen::App app;
    // Una linea por peticion, con su flush, cuesta cerca de un 25% del
    // rendimiento y multiplica por 2,6 la latencia mediana: se pide a mano.
    // El arranque, las recargas y el autotest se siguen viendo siempre.
    if (verbose) app.use(lumen::logger());

    // Los pools de los modulos de datos tienen hilos propios que reanudan
    // handlers posteando al event loop.  Hay que pararlos y esperarlos mientras
    // los loops siguen vivos; si no, un worker que termine tarde —SQLite puede
    // quedarse hasta 5 s en su busy handler— postea a un loop ya destruido.
    app.on_before_stop([] { lumen_script::DbRegistry::instance().shutdown(); });
    app.set_templates(cfg.templates_dir);
    if (!cfg.name.empty()) app.api_info(cfg.name, cfg.version.empty() ? "0.1.0"
                                                                     : cfg.version);
    for (const auto& m : cfg.statics) app.serve_static(m.url_prefix, m.fs_root, m.spa);
    // /openapi.json y /docs se sirven desde el modulo vivo, no desde una copia
    // congelada al arrancar: asi la recarga en caliente actualiza tambien la
    // documentacion.
    if (cfg.docs) {
        app.get("/openapi.json", [](lumen::Response& res) {
            auto mod = current_module();
            if (!mod) {
                res.status(503).json_text(R"({"error":"sin modulo"})");
                return;
            }
            res.json_text(mod->openapi);
        });
        app.get("/docs", [](lumen::Response& res) {
            res.html(lumen::swagger_ui_html("/openapi.json"));
        });
    }
    if (cfg.health)  app.enable_health();
    if (cfg.metrics) app.enable_metrics();

    // El enrutado real lo hace el modulo, que es lo que se intercambia en
    // caliente; en el motor basta con una entrada que lo capture todo.  Los
    // montajes estaticos se resuelven antes de llegar aqui.
    //
    // Hacen falta dos patrones: el comodin del radix tree cubre uno o mas
    // segmentos, asi que la raiz "/" no entra en "/*".
    auto dispatch = [](lumen::Request& req, lumen::Response& res) -> lumen::Task<void> {
        auto mod = current_module();
        if (!mod) { res.status(503).json_text(R"({"error":"sin modulo cargado"})"); co_return; }

        auto match = mod->router.match(req.method, req.path);
        if (!match.found) {
            lumen_script::Value::Dict d;
            d["error"] = lumen_script::Value::str("Not Found");
            d["path"]  = lumen_script::Value::str(req.path);
            res.status(404).json_text(lumen_script::Value::dict(std::move(d)).to_json_text());
            co_return;
        }
        req.params = std::move(match.params);
        co_await match.handler(req, res);
    };
    app.any("/",  dispatch);
    app.any("/*", dispatch);

    // Manejadores de `on error`: se registra uno solo en el motor y el reparto
    // por codigo lo hace el modulo vivo, igual que con las rutas, para que la
    // recarga en caliente tambien los alcance.
    app.on_error([](int code, lumen::Request& req, lumen::Response& res) {
        auto mod = current_module();
        if (!mod) return;

        auto it = mod->error_handlers.find(code);
        if (it == mod->error_handlers.end()) it = mod->error_handlers.find(0);
        if (it == mod->error_handlers.end()) return;   // sin manejador: se deja lo que haya

        lumen_script::NativeCtx ctx{req, res};
        ctx.error_code     = code;
        ctx.error_message  = res.status_code() >= 500 ? "error interno" : "peticion no valida";
        ctx.error_messages = &lumen_script::last_validation_messages();

        lumen_script::VM  vm;
        auto result = vm.start(*it->second, {}, ctx, &mod->functions);
        if (result.status == lumen_script::VM::Status::Error) {
            lumen::log().error("on error " + std::to_string(code) + ": " + result.error);
            return;
        }
        if (result.status != lumen_script::VM::Status::Done) return;   // no puede suspenderse

        if (!ctx.response_written && !result.value.is_null())
            res.header("Content-Type", "application/json; charset=utf-8")
               .send(result.value.to_json_text());

        // El manejador describe el fallo; no puede convertirlo en un exito.
        res.status(code);
    });

    std::thread watcher;
    if (watch) watcher = std::thread(watch_loop, inputs);

    // La primera pasada espera a que el servidor este escuchando.
    if (g_autotest.enabled) {
        g_autotest.port = port_override ? static_cast<uint16_t>(port_override)
                                        : static_cast<uint16_t>(cfg.port);
        std::thread([mod] {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            lumen_script::run_autotest(*mod, g_autotest);
        }).detach();
    }

    app.run(port_override ? static_cast<uint16_t>(port_override)
                          : static_cast<uint16_t>(cfg.port));

    g_stop.store(true);
    if (watcher.joinable()) watcher.join();
    return 0;
}
