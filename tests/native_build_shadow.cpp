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

static VM::Result ejecutar_result(const Chunk& chunk, std::vector<Value> args,
                                  const FunctionTable* fns, const NativeModule* nativo) {
    lumen::Request  req;
    lumen::Response res;
    NativeCtx       ctx{req, res};
    VM              vm;
    NativeDispatch  nd = nativo ? nativo->dispatch() : NativeDispatch{};
    return vm.start(chunk, std::move(args), ctx, fns, &nd);
}

static long long ejecutar(const Chunk& chunk, long long arg, const FunctionTable* fns,
                          const NativeModule* nativo) {
    VM::Result r = ejecutar_result(chunk, {Value::integer(arg)}, fns, nativo);
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

// Los tres casos que encontraron el bug critico documentado en
// COMPILACION-NATIVA.md (Fase 3, "Correccion critica"): Lumen Script no
// comprueba en ningun sitio que una reasignacion, un `and`/`or`, o una
// division/modulo conserven el tipo o eviten el divisor cero -- confiar en
// el tipo DECLARADO sin demostrarlo daba C++ que compilaba y respondia
// distinto al bytecode (o, para division/modulo, tumbaba el proceso
// entero). Estas pruebas fijan la correccion (Comprobador::tipo_provable en
// native_gen.cpp) para que no se pueda perder sin que ctest lo note.
static bool prueba_tipos_dinamicos() {
    bool ok = true;

    // 1) Reasignar `x` (declarado int) a un float: no demostrable -> la
    // funcion entera se queda sin compilar a nativo. Si SI compilase,
    // truncaria 3.5 a 3 en silencio (el bug de verdad, ya reproducido y
    // corregido).
    {
        const std::string src =
            "fn int riesgo(int a):\n"
            "    int x = a\n"
            "    x = 3.5\n"
            "    return x\n";
        SourceFile file; DiagnosticBag diag; Program prog;
        if (!parse_program(src, file, diag, prog)) { std::printf("FALLA (reasignacion): no parsea\n"); return false; }
        FunctionSigs sigs = firmar(prog);
        std::string aviso;
        std::error_code ec;
        auto cache = std::filesystem::temp_directory_path() / "lumen_native_build_check_reasig";
        std::filesystem::remove_all(cache, ec);
        auto nativo = compilar_nativo(prog, sigs, cache, aviso);
        std::filesystem::remove_all(cache, ec);
        if (nativo) {
            std::printf("  FALLA reasignacion de tipo: se compilo a nativo (deberia caer a "
                        "bytecode)\n");
            ok = false;
        } else {
            std::printf("  ok    reasignacion int->float: se queda en bytecode, como debe\n");
        }
    }

    // 2) `and`/`or` con operandos no booleanos: el resultado de Lumen es el
    // VALOR del operando que gana (estilo Python), no un booleano forzado
    // -- no demostrable con la traduccion a &&/|| de hoy, tiene que caer a
    // bytecode. Con operandos YA booleanos si es sano (unico caso en el que
    // &&/|| coincide observablemente), y eso se comprueba tambien: no debe
    // dejar de compilar por el cambio.
    {
        const std::string src =
            "fn int no_booleano(int a, int b):\n"
            "    return a and b\n"
            "\n"
            "fn bool booleano(bool a, bool b):\n"
            "    return a and b\n";
        SourceFile file; DiagnosticBag diag; Program prog;
        if (!parse_program(src, file, diag, prog)) { std::printf("FALLA (and/or): no parsea\n"); return false; }
        FunctionSigs sigs = firmar(prog);
        FunctionTable tabla_vm;
        std::unique_ptr<NativeModule> nativo;
        auto cache = std::filesystem::temp_directory_path() / "lumen_native_build_check_andor";
        if (!compilar_las_dos_vias(prog, sigs, tabla_vm, nativo, cache)) return false;
        std::error_code ec;
        std::filesystem::remove_all(cache, ec);

        if (!nativo || nativo->compiladas() != 1) {
            std::printf("  FALLA and/or: se esperaba exactamente 1 funcion nativa (booleano), "
                        "hay %zu\n", nativo ? nativo->compiladas() : 0);
            ok = false;
        } else {
            std::printf("  ok    and/or: 'no_booleano' (int) se queda en bytecode, 'booleano' "
                        "(bool) si compila\n");
            // Ademas de compilarse, "booleano" tiene que dar el mismo
            // resultado por las dos vias -- para bool, &&  SI coincide con
            // "el operando que gana".
            const Chunk& chunk = *tabla_vm[sigs.at("booleano").index];
            VM::Result sin_n = ejecutar_result(chunk, {Value::boolean(true), Value::boolean(false)},
                                              &tabla_vm, nullptr);
            VM::Result con_n = ejecutar_result(chunk, {Value::boolean(true), Value::boolean(false)},
                                              &tabla_vm, nativo.get());
            if (sin_n.value.as_bool() != con_n.value.as_bool()) {
                std::printf("  FALLA booleano(true,false): bytecode=%d nativo=%d\n",
                            sin_n.value.as_bool(), con_n.value.as_bool());
                ok = false;
            }
        }

        // 'no_booleano' en si -- confirma que Lumen realmente da el "operando
        // que gana" (5 -> truthy, gana "b"=10), no un booleano: es la
        // semantica real que --native tendria que reproducir si algun dia
        // aprende a compilar and/or fuera del caso bool-bool.
        const Chunk& chunk = *tabla_vm[sigs.at("no_booleano").index];
        VM::Result r = ejecutar_result(chunk, {Value::integer(5), Value::integer(10)}, &tabla_vm,
                                      nullptr);
        if (r.status != VM::Status::Done || r.value.as_int() != 10) {
            std::printf("  FALLA no_booleano(5,10): se esperaba 10 (estilo Python), dio %lld "
                        "(status=%d)\n", r.value.as_int(), static_cast<int>(r.status));
            ok = false;
        }
    }

    // 3) Division y modulo por cero: error controlado en el VM, UB (en la
    // practica SIGFPE) en C++ puro. La funcion SI debe compilar a nativo
    // (el tipo esta garantizado, solo el divisor es dinamico) y el canal de
    // error (NativeValue::Tag::Error / lumen_native_fail) debe convertir el
    // fallo en el mismo VM::Status::Error que daria el bytecode -- nunca en
    // un proceso muerto.
    {
        const std::string src = "fn int f(int a, int b):\n    return a % b\n";
        SourceFile file; DiagnosticBag diag; Program prog;
        if (!parse_program(src, file, diag, prog)) { std::printf("FALLA (modulo cero): no parsea\n"); return false; }
        FunctionSigs sigs = firmar(prog);
        FunctionTable tabla_vm;
        std::unique_ptr<NativeModule> nativo;
        auto cache = std::filesystem::temp_directory_path() / "lumen_native_build_check_modcero";
        if (!compilar_las_dos_vias(prog, sigs, tabla_vm, nativo, cache)) return false;
        std::error_code ec;
        std::filesystem::remove_all(cache, ec);

        if (!nativo || nativo->compiladas() != 1) {
            std::printf("  FALLA modulo cero: se esperaba que 'f' compilase a nativo (es "
                        "segura), hay %zu\n", nativo ? nativo->compiladas() : 0);
            ok = false;
        } else {
            const Chunk& chunk = *tabla_vm[sigs.at("f").index];
            VM::Result sin_n = ejecutar_result(chunk, {Value::integer(10), Value::integer(0)},
                                              &tabla_vm, nullptr);
            VM::Result con_n = ejecutar_result(chunk, {Value::integer(10), Value::integer(0)},
                                              &tabla_vm, nativo.get());
            if (sin_n.status != VM::Status::Error || con_n.status != VM::Status::Error) {
                std::printf("  FALLA modulo cero: se esperaba Status::Error en las dos vias "
                            "(bytecode=%d nativo=%d)\n",
                            static_cast<int>(sin_n.status), static_cast<int>(con_n.status));
                ok = false;
            } else if (sin_n.error != con_n.error) {
                std::printf("  FALLA modulo cero: mensajes distintos -- bytecode='%s' "
                            "nativo='%s'\n", sin_n.error.c_str(), con_n.error.c_str());
                ok = false;
            } else {
                std::printf("  ok    modulo por cero: las dos vias dan Status::Error con el "
                            "mismo mensaje ('%s'), el proceso sigue vivo\n", con_n.error.c_str());
            }
        }
    }

    return ok;
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
    if (!prueba_tipos_dinamicos()) ++fallos;

    if (fallos == 0) {
        std::printf("native_build_shadow: la VM con --native conectado coincide en todos los "
                    "casos\n");
        return 0;
    }
    std::printf("native_build_shadow: %d fallo(s)\n", fallos);
    return 1;
}
