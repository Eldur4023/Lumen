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
    : por_indice(std::move(o.por_indice)), handle_(o.handle_) {
    o.handle_ = nullptr;
}

NativeModule& NativeModule::operator=(NativeModule&& o) noexcept {
    if (this != &o) {
        if (handle_) dlclose(handle_);
        por_indice = std::move(o.por_indice);
        handle_    = o.handle_;
        o.handle_  = nullptr;
    }
    return *this;
}

size_t NativeModule::compiladas() const {
    size_t n = 0;
    for (auto* f : por_indice) if (f) ++n;
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

} // namespace

std::unique_ptr<NativeModule> compilar_nativo(const Program& prog, const FunctionSigs& sigs,
                                              const std::filesystem::path& cache_dir,
                                              std::string& aviso) {
    aviso.clear();

    std::vector<std::string> nombre_por_indice(sigs.size());
    for (const auto& [nombre, sig] : sigs)
        if (sig.index < nombre_por_indice.size()) nombre_por_indice[sig.index] = nombre;

    struct Generada {
        size_t      indice;
        std::string simbolo_abi;
    };
    std::vector<Generada> generadas;
    // Prototipos y cuerpos por separado: sigs (un std::map) itera en orden
    // alfabetico de nombre, no en el orden en que unas funciones llaman a
    // otras -- sin un prototipo adelantado, una funcion que llama a otra que
    // el mapa visita despues (p.ej. "usa" llamando a "valida") no compilaria
    // porque C++ exige ver la declaracion antes del uso.
    std::string prototipos;
    std::string cuerpos;

    for (const auto& [nombre, sig] : sigs) {
        const FnDecl* fn = buscar_fn(prog, nombre);
        if (!fn) continue; // no deberia pasar: sigs viene de este mismo prog

        DiagnosticBag diags_ir; // descartable: si esta funcion ya compilo a
                                // bytecode, su cuerpo tipa limpio tambien aqui.
        Chunk         descartable;
        Emitter       emitter(diags_ir, &sigs, nullptr, &prog.imports);
        IrBlock       body;
        if (!emitter.check_function(*fn, descartable, diags_ir, &body)) continue;

        auto generada = generar_funcion_nativa(*fn, body, nombre_por_indice);
        if (!generada) continue;

        prototipos += generada->firma_cpp + ";\n";
        cuerpos += generada->firma_cpp + " " + generada->cuerpo_cpp + "\n\n";
        // Sin simbolo_abi: la funcion usa `string` en su frontera y todavia
        // no cruza la ABI fija (ver tipo_abi_soportado en native_gen.cpp).
        // Su cuerpo ya quedo arriba, asi que otra funcion nativa que la
        // llame directamente se sigue beneficiando -- solo se queda fuera
        // del despacho desde la VM (por_indice no tendra entrada para ella).
        if (!generada->simbolo_abi.empty()) {
            cuerpos += generada->wrapper_cpp + "\n\n";
            generadas.push_back({sig.index, generada->simbolo_abi});
        }
    }

    if (generadas.empty()) return nullptr; // nada que ofrecer nativo: no es un error

    std::string codigo = "#include <cctype>\n#include <cstdint>\n#include <string>\n\n" +
                         abi_prelude() + "\n" + string_runtime_prelude() + "\n" +
                         prototipos + "\n" + cuerpos;

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
    cmd << "g++ -O2 -shared -fPIC -std=c++20 "
        << std::quoted(src_path.string()) << " -o " << std::quoted(so_path.string())
        << " 2> " << std::quoted(err_path.string());
    if (std::system(cmd.str().c_str()) != 0) {
        std::ifstream errf(err_path);
        std::ostringstream errs;
        errs << errf.rdbuf();
        aviso = "--native: " + std::to_string(generadas.size()) +
                " funcion(es) no se pudieron compilar (g++ fallo): " + errs.str();
        return nullptr;
    }

    void* handle = dlopen(so_path.c_str(), RTLD_NOW);
    if (!handle) {
        aviso = std::string("--native: no se pudo cargar la biblioteca generada: ") +
                (dlerror() ? dlerror() : "motivo desconocido");
        return nullptr;
    }

    auto out = std::make_unique<NativeModule>();
    out->handle_ = handle;
    out->por_indice.assign(nombre_por_indice.size(), nullptr);

    std::string simbolos_sin_resolver;
    for (const auto& g : generadas) {
        void* sym = dlsym(handle, g.simbolo_abi.c_str());
        if (!sym) { simbolos_sin_resolver += " " + g.simbolo_abi; continue; }
        out->por_indice[g.indice] = reinterpret_cast<CompiledFn>(sym);
    }
    if (!simbolos_sin_resolver.empty())
        aviso = "--native: simbolo(s) no resueltos tras compilar (se sirven con bytecode):" +
                simbolos_sin_resolver;

    return out;
}

} // namespace lumen_script
