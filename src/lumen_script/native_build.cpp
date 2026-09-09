#include <lumen_script/native_build.hpp>
#include <lumen_script/native_gen.hpp>

#include <dlfcn.h>

#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace lumen_script {

NativeModule::~NativeModule() {
    if (handle_) dlclose(handle_);
}

NativeModule::NativeModule(NativeModule&& o) noexcept
    : por_indice(std::move(o.por_indice)), error_message(o.error_message),
      rutas_por_indice(std::move(o.rutas_por_indice)),
      rutas_async_por_indice(std::move(o.rutas_async_por_indice)), handle_(o.handle_) {
    o.handle_ = nullptr;
}

NativeModule& NativeModule::operator=(NativeModule&& o) noexcept {
    if (this != &o) {
        if (handle_) dlclose(handle_);
        por_indice             = std::move(o.por_indice);
        error_message          = o.error_message;
        rutas_por_indice       = std::move(o.rutas_por_indice);
        rutas_async_por_indice = std::move(o.rutas_async_por_indice);
        handle_                = o.handle_;
        o.handle_              = nullptr;
    }
    return *this;
}

size_t NativeModule::compiladas() const {
    size_t n = 0;
    for (auto* f : por_indice) if (f) ++n;
    return n;
}

size_t NativeModule::rutas_compiladas() const {
    size_t n = 0;
    for (auto* f : rutas_por_indice) if (f) ++n;
    for (auto* f : rutas_async_por_indice) if (f) ++n;
    return n;
}

namespace {

// Una funcion de usuario por su nombre, para poder pasarle a check_function
// el FnDecl que corresponde a cada entrada de `sigs` -- FunctionSigs solo
// guarda el indice, no un puntero al AST.
const FnDecl* buscar_fn(const Program& prog, const std::string& nombre) {
    for (const auto& f : prog.functions)
        if (f.name == nombre) return &f;
    return nullptr;
}

// La firma (tipos de parametros y retorno) de cada funcion de `prog`, para
// que tipo_provable() (native_gen.cpp) pueda comprobar una llamada contra
// el destino sin volver a mirar el AST. Se construye para TODAS las
// funciones, no solo las que van a terminar compilando: una funcion nativa
// puede llamar a otra que el mapa (alfabetico, por FunctionSigs) todavia no
// proceso.
TablaFirmas construir_firmas(const Program& prog) {
    TablaFirmas firmas;
    for (const auto& f : prog.functions) {
        FirmaNativa firma;
        firma.retorno = Type::from_declared(f.return_type);
        for (const auto& p : f.params) firma.params.push_back(Type::from_declared(p.type));
        firmas[f.name] = std::move(firma);
    }
    return firmas;
}

} // namespace

std::unique_ptr<NativeModule> compilar_nativo(const Program& prog, const FunctionSigs& sigs,
                                              const ClassSigs& clases_sig,
                                              const std::filesystem::path& cache_dir,
                                              std::string& aviso) {
    aviso.clear();

    std::vector<std::string> nombre_por_indice(sigs.size());
    for (const auto& [nombre, sig] : sigs)
        if (sig.index < nombre_por_indice.size()) nombre_por_indice[sig.index] = nombre;

    const TablaFirmas firmas = construir_firmas(prog);

    TablaClases clases;
    TablaRoles  roles;
    construir_clases(prog, clases_sig, sigs, &prog.imports, clases, roles);

    // El texto de cada clase representable va ANTES que ningun prototipo/
    // cuerpo: un LPunto usado como parametro/retorno necesita el tipo
    // completo, no basta una declaracion adelantada. Sin dependencias entre
    // clases que ordenar -- el lenguaje no admite una clase como campo de
    // otra (project.cpp), asi que el orden entre ellas es indiferente.
    std::string clases_texto;
    for (const auto& [nombre, cn] : clases) clases_texto += generar_clase_runtime(nombre, cn) + "\n";

    struct Generada {
        size_t      indice;
        std::string simbolo_abi;
    };
    std::vector<Generada> generadas;
    // Prototipos y cuerpos por separado: sigs (un std::map) itera en orden
    // alfabetico de nombre, no en el orden en que unas funciones llaman a
    // otras -- sin un prototipo adelantado, una funcion que llama a otra que
    // el mapa visita despues (p.ej. "usa" llamando a "valida") no compilaria
    // porque C++ exige ver la declaracion antes del uso. Los metodos
    // comparten el mismo bloque -- una funcion suelta puede llamar a un
    // metodo (recibiendo la instancia ya construida) y viceversa.
    std::string prototipos;
    std::string cuerpos;

    for (const auto& [nombre, sig] : sigs) {
        const FnDecl* fn = buscar_fn(prog, nombre);
        if (!fn) continue; // no deberia pasar: sigs viene de este mismo prog

        DiagnosticBag diags_ir; // descartable: si esta funcion ya compilo a
                                // bytecode, su cuerpo tipa limpio tambien aqui.
        Chunk         descartable;
        // classes_ = nullptr, igual que build_functions() en project.cpp:
        // una funcion SUELTA no puede construir instancias ni llamar a un
        // metodo (el checker real solo resuelve eso dentro de una ruta/
        // metodo, que si reciben ClassSigs) -- pasarlo aqui haria a este
        // check_function() mas permisivo que el compilador real, la misma
        // clase de divergencia que motivo la correccion critica de mas
        // arriba. Una funcion suelta SI puede recibir/devolver una
        // instancia ya construida (un parametro/retorno de tipo clase, sin
        // tocar sus campos ni metodos) -- eso no necesita classes_ en
        // absoluto, solo el tipo declarado del parametro.
        Emitter emitter(diags_ir, &sigs, nullptr, &prog.imports);
        IrBlock body;
        if (!emitter.check_function(*fn, descartable, diags_ir, &body)) continue;

        auto generada = generar_funcion_nativa(*fn, body, nombre_por_indice, firmas, clases, roles);
        if (!generada) continue;

        prototipos += generada->firma_cpp + ";\n";
        cuerpos += generada->firma_cpp + " " + generada->cuerpo_cpp + "\n\n";
        // Sin simbolo_abi: la funcion usa string/List/Dict/clase en su
        // frontera y todavia no cruza la ABI fija (ver tipo_abi_soportado
        // en native_gen.cpp). Su cuerpo ya quedo arriba, asi que otra
        // funcion nativa que la llame directamente se sigue beneficiando --
        // solo se queda fuera del despacho desde la VM (por_indice no
        // tendra entrada para ella).
        if (!generada->simbolo_abi.empty()) {
            cuerpos += generada->wrapper_cpp + "\n\n";
            generadas.push_back({sig.index, generada->simbolo_abi});
        }
    }

    // Metodos de las clases representables (los constructores no necesitan
    // generacion aparte: el UNICO constructor de la clase C++ generada, en
    // generar_clase_runtime(), YA es el automapeo -- ver el comentario de
    // ConstructorCall en Generador::expr). Nunca cruzan la ABI (el receptor
    // es de tipo clase), asi que no aportan nada a `generadas`/por_indice --
    // solo invocables desde otra funcion nativa, directamente en C++.
    for (const auto& c : prog.classes) {
        if (!clases.count(c.name)) continue; // la clase no es representable
        for (const auto& m : c.methods) {
            DiagnosticBag diags_ir;
            Chunk         descartable;
            // classes_ = &clases_sig aqui SI, igual que emit_class_bodies()
            // en project.cpp: un metodo si puede construir instancias y
            // llamar a otros metodos.
            Emitter emitter(diags_ir, &sigs, &clases_sig, &prog.imports);
            IrBlock body;
            if (!emitter.check_method(c.name, m, descartable, diags_ir, &body)) continue;

            auto generada = generar_metodo_nativo(c.name, m, body, nombre_por_indice, firmas,
                                                  clases, roles);
            if (!generada) continue;
            prototipos += generada->firma_cpp + ";\n";
            cuerpos += generada->firma_cpp + " " + generada->cuerpo_cpp + "\n\n";
        }
    }

    // Rutas (Fase 4): a diferencia de funciones/metodos, una ruta nunca
    // aporta prototipo (nadie mas la llama en C++ generado) -- su texto
    // completo (extern "C" incluido) se acumula aparte y va DESPUES de
    // cuerpos, sin que le afecte el orden alfabetico de `sigs`.
    struct RutaGenerada {
        size_t      indice;
        std::string simbolo;
    };
    std::vector<RutaGenerada> rutas_generadas;       // void(Request&, Response&)
    std::vector<RutaGenerada> rutas_async_generadas; // Task<void>(Request&, Response&)
    std::string               rutas_cuerpos;
    for (size_t i = 0; i < prog.routes.size(); ++i) {
        const RouteDecl& r = prog.routes[i];
        if (r.method == "WS" || r.method == "SSE") continue;

        DiagnosticBag diags_ir;
        Chunk         descartable;
        // classes_ = &clases_sig, igual que build_routes() en project.cpp:
        // una ruta si puede construir instancias y llamar a metodos (aunque
        // esta primera fase de rutas no llegue a generar ninguno de esos
        // casos -- ver el comentario de RutaNativa).
        Emitter  emitter(diags_ir, &sigs, &clases_sig, &prog.imports);
        IrBlock  body;
        if (!emitter.check_route(r, descartable, diags_ir, &body)) continue;

        auto generada = generar_ruta_nativa(r, body, static_cast<int>(i), nombre_por_indice,
                                            firmas, clases, roles);
        if (!generada) continue;

        rutas_cuerpos += generada->cuerpo_cpp + "\n\n";
        (generada->asincrona ? rutas_async_generadas : rutas_generadas)
            .push_back({i, generada->simbolo});
    }

    if (generadas.empty() && rutas_generadas.empty() && rutas_async_generadas.empty())
        return nullptr; // nada que ofrecer nativo: no es un error

    const bool con_rutas = !rutas_generadas.empty() || !rutas_async_generadas.empty();

    std::string codigo =
        "#include <cctype>\n#include <cstdint>\n#include <initializer_list>\n#include <map>\n"
        "#include <string>\n#include <utility>\n#include <vector>\n\n" +
        abi_prelude() + "\n" + error_runtime_prelude() + "\n" + list_runtime_prelude() + "\n" +
        dict_runtime_prelude() + "\n" + string_runtime_prelude() + "\n";
    // lumen_script::Value hace falta SIEMPRE que algo pueda usar str() --
    // Generador::expr() lo traduce a valor_json(...).to_string(), el mismo
    // puente que usa el valor de retorno de una ruta -- no solo cuando hay
    // rutas: una funcion suelta o un metodo puede llamar a str() igual (ver
    // tipo_provable(), caso BuiltinGlobalCall). Descubierto escribiendo
    // tests/native_build_shadow.cpp::prueba_str_len(): antes de esto, el
    // include/using solo se anteponia "si con_rutas", y una funcion con
    // str() en el cuerpo (sin ninguna ruta en el modulo) fallaba a compilar
    // con "'Value' has not been declared" -- el mismo tipo de descuido que
    // ya corrigieron las dos comprobaciones criticas anteriores: no asumir
    // que un caso nuevo hereda las condiciones de guarda de una fase
    // anterior sin volver a mirarlas.
    codigo += "#include <lumen_script/value.hpp>\nusing lumen_script::Value;\n\n";
    // Cabeceras de lumen::Request/Response y el binding de parametros SOLO
    // si hay al menos una ruta: eso si es exclusivo de rutas (ninguna
    // funcion suelta ve jamas un Request/Response).
    // <lumen/task.hpp>: Task<void>/sleep() -- Fase 5, `await sleep(ms)` --
    // hace falta siempre que haya rutas, no solo las que de verdad usan
    // `await`: es mas simple incluirla siempre que decidir aqui cual de
    // ellas la necesita de verdad (route_runtime_prelude() la usa igual).
    // <lumen_script/db.hpp>: DbOp/await_db() -- Fase 5.5, `await
    // <modulo>.query/exec/last_id(...)` -- mismo criterio que Task/sleep:
    // siempre que haya rutas, no solo las que de verdad usan una base de
    // datos.
    // <lumen_script/natives.hpp>: last_validation_messages() -- Fase 5.7,
    // limpiada al principio de CUALQUIER ruta (no solo una con parametro
    // de cuerpo, ver el comentario en generar_ruta_nativa) para que un
    // `on error <code>:` que la app declare no herede mensajes de una
    // peticion anterior en el mismo hilo.
    if (con_rutas)
        codigo += "#include <lumen/request.hpp>\n#include <lumen/response.hpp>\n"
                  "#include <lumen/task.hpp>\n#include <lumen_script/db.hpp>\n"
                  "#include <lumen_script/natives.hpp>\n\n" +
                  route_runtime_prelude() + "\n";
    codigo += clases_texto + "\n" + prototipos + "\n" + cuerpos;
    if (con_rutas) codigo += rutas_cuerpos;

    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);
    if (ec) {
        aviso = "--native: no se pudo crear " + cache_dir.string() + ": " + ec.message();
        return nullptr;
    }

    const std::filesystem::path src_path = cache_dir / "native.cpp";
    const std::filesystem::path so_path  = cache_dir / "native.so";
    const std::filesystem::path err_path = cache_dir / "native.err";
    {
        std::ofstream out(src_path, std::ios::trunc);
        out << codigo;
    }

    std::ostringstream cmd;
    cmd << "g++ -O2 -shared -fPIC -std=c++20 ";
    // LUMEN_NATIVE_INCLUDE_DIR/LUMEN_NATIVE_SCRIPT_LIB: horneadas por CMake
    // (ver CMakeLists.txt) -- el binario `lumen` no tiene otra forma de
    // saber, en tiempo de ejecucion, donde viven las cabeceras del proyecto
    // ni donde quedo liblumen_script.a ya compilada. Necesarias siempre
    // (Value ya no es opcional, ver arriba), no solo "si con_rutas".
    cmd << "-I" << std::quoted(std::string(LUMEN_NATIVE_INCLUDE_DIR)) << " ";
    cmd << std::quoted(src_path.string()) << " -o " << std::quoted(so_path.string());
    cmd << " " << std::quoted(std::string(LUMEN_NATIVE_SCRIPT_LIB));
    // liblumen.a: SOLO si hay rutas -- una funcion suelta nunca usa
    // lumen::Task/lumen::Response, asi que nunca deja un simbolo de lumen
    // sin resolver. Orden importante para un enlazado estatico: DESPUES de
    // liblumen_script.a (que ya la necesita transitivamente) y del propio
    // .cpp (que la necesita directamente para Task<void>::promise_type::
    // FinalAwaitable::await_suspend -- EpollLoop::post -- en cualquier ruta
    // con `await`, ver el comentario de LUMEN_NATIVE_LIB en CMakeLists.txt).
    if (con_rutas) cmd << " " << std::quoted(std::string(LUMEN_NATIVE_LIB));
    cmd << " 2> " << std::quoted(err_path.string());
    if (std::system(cmd.str().c_str()) != 0) {
        std::ifstream errf(err_path);
        std::ostringstream errs;
        errs << errf.rdbuf();
        aviso = "--native: " + std::to_string(generadas.size() + rutas_generadas.size() +
                                              rutas_async_generadas.size()) +
                " funcion(es)/ruta(s) no se pudieron compilar (g++ fallo): " + errs.str();
        return nullptr;
    }

    void* handle = dlopen(so_path.c_str(), RTLD_NOW);
    if (!handle) {
        // dlerror() tiene semantica "de un solo uso": una segunda llamada
        // (la que hacia el ternario de antes, `dlerror() ? dlerror() : ...`)
        // ya devuelve nullptr porque la primera consumio el mensaje --
        // std::string + nullptr es UB (aqui, un SEGV real, encontrado
        // probando esto contra un dlopen que de verdad fallaba). Una sola
        // llamada, guardada.
        const char* motivo = dlerror();
        aviso = std::string("--native: no se pudo cargar la biblioteca generada: ") +
                (motivo ? motivo : "motivo desconocido");
        return nullptr;
    }

    auto out = std::make_unique<NativeModule>();
    out->handle_ = handle;
    out->por_indice.assign(nombre_por_indice.size(), nullptr);
    out->rutas_por_indice.assign(prog.routes.size(), nullptr);
    out->rutas_async_por_indice.assign(prog.routes.size(), nullptr);

    out->error_message =
        reinterpret_cast<ErrorMessageFn>(dlsym(handle, "lumen_native_error_message"));

    std::string simbolos_sin_resolver;
    for (const auto& g : generadas) {
        void* sym = dlsym(handle, g.simbolo_abi.c_str());
        if (!sym) { simbolos_sin_resolver += " " + g.simbolo_abi; continue; }
        out->por_indice[g.indice] = reinterpret_cast<CompiledFn>(sym);
    }
    for (const auto& g : rutas_generadas) {
        void* sym = dlsym(handle, g.simbolo.c_str());
        if (!sym) { simbolos_sin_resolver += " " + g.simbolo; continue; }
        out->rutas_por_indice[g.indice] = reinterpret_cast<NativeModule::RouteFn>(sym);
    }
    for (const auto& g : rutas_async_generadas) {
        void* sym = dlsym(handle, g.simbolo.c_str());
        if (!sym) { simbolos_sin_resolver += " " + g.simbolo; continue; }
        out->rutas_async_por_indice[g.indice] = reinterpret_cast<NativeModule::RouteFnAsync>(sym);
    }
    if (!out->error_message) simbolos_sin_resolver += " lumen_native_error_message";
    if (!simbolos_sin_resolver.empty())
        aviso = "--native: simbolo(s) no resueltos tras compilar (se sirven con bytecode):" +
                simbolos_sin_resolver;

    return out;
}

} // namespace lumen_script
