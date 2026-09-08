// Fase 2 de --native, la mitad que native_gen_shadow.cpp no cubre: aquella
// prueba genera texto C++ y lo compila/ejecuta con un driver escrito a mano;
// esta usa compilar_nativo() de verdad -- el mismo camino que toma
// project.cpp::compile() cuando se pide --native -- y comprueba que la VM,
// con la tabla nativa resultante conectada via VM::start(), da exactamente
// el mismo resultado que sin ella. Es la prueba de que el despacho dentro
// del interprete (Op::CallFunction, ver vm.cpp) esta bien conectado, no solo
// que el generador produce C++ correcto.
#include <lumen_script/diagnostic.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/native_build.hpp>
#include <lumen_script/natives.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/vm.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace lumen_script;

static int fallos = 0;

static bool parse_program(const std::string& src, SourceFile& file, DiagnosticBag& diags,
                          Program& out) {
    file.path = "<prueba>";
    file.text = src;
    Lexer  lexer(file, diags);
    Parser parser(lexer.tokenize(), diags);
    parser.parse_into(out);
    return diags.empty();
}

static long long ejecutar(const Chunk& chunk, long long arg, const FunctionTable* fns,
                          const NativeModule* nativo) {
    lumen::Request  req;
    lumen::Response res;
    NativeCtx       ctx{req, res};
    VM              vm;
    NativeDispatch  nd = nativo ? nativo->dispatch() : NativeDispatch{};
    VM::Result      r  = vm.start(chunk, {Value::integer(arg)}, ctx, fns, &nd);
    if (r.status != VM::Status::Done) {
        std::printf("  FALLA: la VM no termino (status=%d, error=%s)\n",
                    static_cast<int>(r.status), r.error.c_str());
        return -1;
    }
    return r.value.as_int();
}

// Compila `prog` (con `sigs` ya calculadas) tanto a bytecode como a nativo, y
// devuelve la tabla de bytecode y el NativeModule resultante. Nulo en
// `nativo` si compilar_nativo() no genero nada (no es forzosamente un
// fallo: puede que ninguna funcion cruce la ABI, ver mas abajo).
static bool compilar_las_dos_vias(Program& prog, FunctionSigs& sigs, FunctionTable& tabla_vm,
                                  std::unique_ptr<NativeModule>& nativo,
                                  const std::filesystem::path& cache_dir) {
    tabla_vm.resize(prog.functions.size());
    DiagnosticBag diags_vm;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_vm, &sigs, nullptr, &prog.imports);
        em.emit_function(prog.functions[i], *chunk);
        tabla_vm[i] = chunk;
    }
    if (!diags_vm.empty()) {
        std::printf("FALLA: la VM no compilo: %s\n", diags_vm.items().front().message.c_str());
        return false;
    }

    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    std::string aviso;
    nativo = compilar_nativo(prog, sigs, cache_dir, aviso);
    if (!aviso.empty()) std::printf("aviso de compilar_nativo(): %s\n", aviso.c_str());
    return true;
}

static FunctionSigs firmar(const Program& prog) {
    FunctionSigs sigs;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        const FnDecl& f = prog.functions[i];
        FnSig sig;
        sig.index = i;
        sig.required = 0;
        for (const auto& p : f.params) {
            sig.defaults.push_back(p.default_value.get());
            if (!p.default_value) ++sig.required;
        }
        sigs[f.name] = std::move(sig);
    }
    return sigs;
}

// `string` en el cuerpo de una funcion cuya frontera SI cruza la ABI (int
// dentro, int fuera): literal, concatenacion, comparacion, y una llamada
// directa a otra funcion nativa cuya frontera usa `string` y por eso no
// tiene wrapper propio (ver tipo_abi_soportado en native_gen.cpp) -- la
// prueba de que esa funcion se compila igual y es invocable desde una que si
// tiene wrapper.
static bool prueba_strings() {
    const std::string src =
        "fn string saluda(string nombre):\n"
        "    return \"hola, \" + nombre\n"
        "\n"
        "fn int usa_saluda(int n):\n"
        "    string s = saluda(\"mundo\")\n"
        "    if s == \"hola, mundo\":\n"
        "        return n\n"
        "    return -1\n";

    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog)) {
        std::printf("FALLA (strings): no parsea (%s)\n",
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return false;
    }

    FunctionSigs                  sigs = firmar(prog);
    FunctionTable                 tabla_vm;
    std::unique_ptr<NativeModule> nativo;
    const auto cache_dir = std::filesystem::temp_directory_path() / "lumen_native_build_check_str";
    if (!compilar_las_dos_vias(prog, sigs, tabla_vm, nativo, cache_dir)) return false;

    if (!nativo) {
        std::printf("FALLA (strings): compilar_nativo() no compilo nada (usa_saluda deberia, "
                    "es int->int con string solo en el cuerpo)\n");
        return false;
    }
    // Solo usa_saluda cruza la ABI (int->int); saluda usa `string` en su
    // propia frontera y se queda sin wrapper -- se compila igual, pero no
    // aparece en por_indice.
    if (nativo->compiladas() != 1) {
        std::printf("FALLA (strings): se esperaba 1 funcion con wrapper (usa_saluda), hay %zu\n",
                    nativo->compiladas());
        return false;
    }

    const Chunk& chunk       = *tabla_vm[sigs.at("usa_saluda").index];
    long long    sin_nativo  = ejecutar(chunk, 7, &tabla_vm, nullptr);
    long long    con_nativo  = ejecutar(chunk, 7, &tabla_vm, nativo.get());
    if (sin_nativo != con_nativo || sin_nativo != 7) {
        std::printf("  FALLA usa_saluda(7): bytecode=%lld nativo=%lld (se esperaba 7)\n",
                    sin_nativo, con_nativo);
        return false;
    }
    std::printf("  ok    usa_saluda(7) = %lld (bytecode y VM+nativo coinciden, con string "
                "solo en el cuerpo)\n", sin_nativo);
    return true;
}

// Los 6 metodos de string (metodo_string_soportado en native_gen.cpp) y, de
// paso, el orden alfabetico de FunctionSigs (un std::map): "usa" llama a
// "valida", que el mapa visita DESPUES por nombre -- sin un prototipo
// adelantado en el .cpp ensamblado, esto no compilaria (bug real que este
// caso encontro al escribirlo, corregido en compilar_nativo() generando
// prototipos para todas las funciones antes que ningun cuerpo).
static bool prueba_metodos_string() {
    const std::string src =
        "fn bool valida(string email):\n"
        "    return email.contains(\"@\") and email.starts_with(\"a\")\n"
        "\n"
        "fn int usa(int n):\n"
        "    string s = \"  Hola Mundo  \"\n"
        "    string t = s.trim()\n"
        "    string u = t.upper()\n"
        "    if u == \"HOLA MUNDO\":\n"
        "        if valida(\"a@b.com\"):\n"
        "            return n\n"
        "    return -1\n";

    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog)) {
        std::printf("FALLA (metodos de string): no parsea (%s)\n",
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return false;
    }

    FunctionSigs                  sigs = firmar(prog);
    FunctionTable                 tabla_vm;
    std::unique_ptr<NativeModule> nativo;
    const auto cache_dir =
        std::filesystem::temp_directory_path() / "lumen_native_build_check_metodos_str";
    if (!compilar_las_dos_vias(prog, sigs, tabla_vm, nativo, cache_dir)) return false;

    if (!nativo || nativo->compiladas() != 1) {
        std::printf("FALLA (metodos de string): se esperaba 1 funcion con wrapper (usa), hay %zu\n",
                    nativo ? nativo->compiladas() : 0);
        return false;
    }

    const Chunk& chunk      = *tabla_vm[sigs.at("usa").index];
    long long    sin_nativo = ejecutar(chunk, 42, &tabla_vm, nullptr);
    long long    con_nativo = ejecutar(chunk, 42, &tabla_vm, nativo.get());
    if (sin_nativo != con_nativo || sin_nativo != 42) {
        std::printf("  FALLA usa(42): bytecode=%lld nativo=%lld (se esperaba 42)\n", sin_nativo,
                    con_nativo);
        return false;
    }
    std::printf("  ok    usa(42) = %lld (bytecode y VM+nativo coinciden: trim/upper/contains/"
                "starts_with, y una llamada a una funcion declarada despues en el .cpp)\n",
                sin_nativo);
    return true;
}

int main() {
    const std::string src =
        "fn int fib(int n):\n"
        "    if n < 2:\n"
        "        return n\n"
        "    return fib(n - 1) + fib(n - 2)\n"
        "\n"
        "fn int cuenta_primos(int limite):\n"
        "    int contador = 0\n"
        "    int i = 2\n"
        "    while i < limite:\n"
        "        bool es_primo = true\n"
        "        int d = 2\n"
        "        while d * d <= i:\n"
        "            if i % d == 0:\n"
        "                es_primo = false\n"
        "                break\n"
        "            d++\n"
        "        if es_primo:\n"
        "            contador++\n"
        "        i++\n"
        "    return contador\n";

    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog)) {
        std::printf("FALLA: no parsea (%s)\n",
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return 1;
    }

    FunctionSigs sigs;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        const FnDecl& f = prog.functions[i];
        FnSig sig;
        sig.index = i;
        sig.required = 0;
        for (const auto& p : f.params) {
            sig.defaults.push_back(p.default_value.get());
            if (!p.default_value) ++sig.required;
        }
        sigs[f.name] = std::move(sig);
    }

    FunctionTable tabla_vm(prog.functions.size());
    DiagnosticBag diags_vm;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_vm, &sigs, nullptr, &prog.imports);
        em.emit_function(prog.functions[i], *chunk);
        tabla_vm[i] = chunk;
    }
    if (!diags_vm.empty()) {
        std::printf("FALLA: la VM no compilo: %s\n", diags_vm.items().front().message.c_str());
        return 1;
    }

    const std::filesystem::path cache_dir =
        std::filesystem::temp_directory_path() / "lumen_native_build_check";
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);

    std::string aviso;
    auto        nativo = compilar_nativo(prog, sigs, cache_dir, aviso);
    if (!aviso.empty()) std::printf("aviso de compilar_nativo(): %s\n", aviso.c_str());
    if (!nativo) {
        std::printf("FALLA: compilar_nativo() no compilo nada (deberia, fib/cuenta_primos "
                    "son puras)\n");
        return 1;
    }
    if (nativo->compiladas() != prog.functions.size()) {
        std::printf("FALLA: se esperaban %zu funciones nativas, se compilaron %zu\n",
                    prog.functions.size(), nativo->compiladas());
        return 1;
    }

    auto comparar = [&](const char* nombre, long long arg) {
        const Chunk& chunk = *tabla_vm[sigs.at(nombre).index];
        long long sin_nativo = ejecutar(chunk, arg, &tabla_vm, nullptr);
        long long con_nativo = ejecutar(chunk, arg, &tabla_vm, nativo.get());

        if (sin_nativo != con_nativo) {
            ++fallos;
            std::printf("  FALLA %s(%lld): bytecode=%lld nativo=%lld\n", nombre, arg,
                        sin_nativo, con_nativo);
        } else {
            std::printf("  ok    %s(%lld) = %lld (bytecode y VM+nativo coinciden)\n", nombre,
                        arg, sin_nativo);
        }
    };

    comparar("fib", 10);
    comparar("fib", 25);
    comparar("fib", 30);
    comparar("cuenta_primos", 1000);
    comparar("cuenta_primos", 100000);

    std::filesystem::remove_all(cache_dir, ec);

    if (!prueba_strings()) ++fallos;
    if (!prueba_metodos_string()) ++fallos;

    if (fallos == 0) {
        std::printf("native_build_shadow: la VM con --native conectado coincide en todos los "
                    "casos\n");
        return 0;
    }
    std::printf("native_build_shadow: %d fallo(s)\n", fallos);
    return 1;
}
