// Verificacion de equivalencia de EJECUCION (fase 1, COMPILACION-NATIVA.md):
// compara, corriendo de verdad en el VM, el bytecode que emite el emisor
// viejo (emit_function sobre Expr/Stmt) contra el que emite el nuevo emisor
// que consume el IR (check_function construye IrExpr/IrStmt con diags_ real,
// emit_block/emit_stmt/emit_expr los consumen sin volver a comprobar nada).
//
// tests/check_expr_shadow.cpp y tests/check_stmt_shadow.cpp ya prueban que
// los DIAGNOSTICOS coinciden. Esta prueba es la otra mitad: que el bytecode
// resultante se COMPORTA igual ejecutado de verdad, no solo que compila
// limpio -- necesaria antes de considerar conectar el emisor nuevo a ningun
// punto de entrada real.
#include <lumen_script/diagnostic.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/natives.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/vm.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
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

static std::string ejecutar(const Chunk& chunk, std::vector<Value> args,
                            const FunctionTable* fns, std::string& resumen) {
    lumen::Request  req;
    lumen::Response res;
    NativeCtx       ctx{req, res};
    VM              vm;
    VM::Result      r = vm.start(chunk, std::move(args), ctx, fns);
    switch (r.status) {
        case VM::Status::Done:
            resumen = "Done";
            return r.value.to_json_text();
        case VM::Status::Error:
            resumen = "Error";
            return r.error;
        case VM::Status::Suspended:
            resumen = "Suspended";
            return "(await_id=" + std::to_string(r.await_id) + ")";
    }
    return {};
}

// Compila TODAS las funciones de `src` (para que la recursion/llamadas entre
// ellas resuelvan) con la via vieja y la nueva, ejecuta `fn_objetivo` con
// `args` en las dos, y compara resultado y estado palabra por palabra.
static void caso(const char* nombre, const std::string& src, const std::string& fn_objetivo,
                 const std::vector<Value>& args) {
    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog)) {
        ++fallos;
        std::printf("  FALLA %s (no parsea: %s)\n", nombre,
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return;
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

    auto idx = sigs.find(fn_objetivo);
    if (idx == sigs.end()) {
        ++fallos;
        std::printf("  FALLA %s (no existe la funcion '%s')\n", nombre, fn_objetivo.c_str());
        return;
    }
    size_t objetivo = idx->second.index;

    // Via vieja: emit_function real, tal cual hace project.cpp hoy.
    FunctionTable tabla_vieja(prog.functions.size());
    DiagnosticBag diags_vieja;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_vieja, &sigs, nullptr, &prog.imports);
        em.emit_function(prog.functions[i], *chunk);
        tabla_vieja[i] = chunk;
    }
    if (!diags_vieja.empty()) {
        ++fallos;
        std::printf("  FALLA %s (la via vieja no compilo: %s)\n", nombre,
                    diags_vieja.items().front().message.c_str());
        return;
    }

    // Via nueva: check_function construye el IR con diags_ REAL (no shadow:
    // aqui un error de verdad tiene que parar la prueba, no compararse
    // contra si mismo) y emit_block lo consume sin volver a comprobar nada.
    FunctionTable tabla_nueva(prog.functions.size());
    DiagnosticBag diags_nueva;
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_nueva, &sigs, nullptr, &prog.imports);
        IrBlock body;
        bool ok = em.check_function(prog.functions[i], *chunk, diags_nueva, &body);
        if (ok) {
            em.emit_block(body);
            chunk->emit(Op::ReturnNull, prog.functions[i].loc);
        }
        tabla_nueva[i] = chunk;
    }
    if (!diags_nueva.empty()) {
        ++fallos;
        std::printf("  FALLA %s (la via nueva no compilo: %s)\n", nombre,
                    diags_nueva.items().front().message.c_str());
        return;
    }

    std::string resumen_vieja, resumen_nueva;
    std::string valor_vieja = ejecutar(*tabla_vieja[objetivo], args, &tabla_vieja, resumen_vieja);
    std::string valor_nueva = ejecutar(*tabla_nueva[objetivo], args, &tabla_nueva, resumen_nueva);

    if (resumen_vieja != resumen_nueva || valor_vieja != valor_nueva) {
        ++fallos;
        std::printf("  FALLA %s (ejecucion distinta)\n", nombre);
        std::printf("           vieja: %s %s\n", resumen_vieja.c_str(), valor_vieja.c_str());
        std::printf("           nueva: %s %s\n", resumen_nueva.c_str(), valor_nueva.c_str());
        return;
    }

    std::printf("  ok    %s (%s: %s)\n", nombre, resumen_vieja.c_str(), valor_vieja.c_str());
}

int main() {
    caso("aritmetica y recursion (factorial)",
        "fn int factorial(int n):\n"
        "    if n <= 1:\n"
        "        return 1\n"
        "    return n * factorial(n - 1)\n",
        "factorial", {Value::integer(6)});

    caso("for con break/continue sobre una List",
        "fn int suma_pares(List<int> xs):\n"
        "    int total = 0\n"
        "    for int x in xs:\n"
        "        if x < 0:\n"
        "            break\n"
        "        if x % 2 != 0:\n"
        "            continue\n"
        "        total = total + x\n"
        "    return total\n",
        "suma_pares",
        {Value::list({Value::integer(1), Value::integer(2), Value::integer(3),
                     Value::integer(4), Value::integer(-1), Value::integer(8)})});

    caso("while con asignacion compuesta y ++/--",
        "fn int cuenta(int n):\n"
        "    int i = 0\n"
        "    int total = 0\n"
        "    while i < n:\n"
        "        total = total + i\n"
        "        i++\n"
        "    return total\n",
        "cuenta", {Value::integer(10)});

    caso("indices, listas y metodos builtin",
        "fn List<int> duplica(List<int> xs):\n"
        "    List<int> out = []\n"
        "    for int x in xs:\n"
        "        out.add(x * 2)\n"
        "    return out\n",
        "duplica",
        {Value::list({Value::integer(1), Value::integer(2), Value::integer(3)})});

    caso("metodos de string",
        "fn string grita(string s):\n"
        "    return s.upper() + \"!\"\n",
        "grita", {Value::str("hola")});

    caso("try/catch con division por cero",
        "fn int seguro(int a, int b):\n"
        "    try:\n"
        "        return a / b\n"
        "    catch e:\n"
        "        return -1\n",
        "seguro", {Value::integer(10), Value::integer(0)});

    caso("try/catch sin disparar el error",
        "fn int seguro2(int a, int b):\n"
        "    try:\n"
        "        return a / b\n"
        "    catch e:\n"
        "        return -1\n",
        "seguro2", {Value::integer(10), Value::integer(2)});

    caso("dict y ternario",
        "fn Json etiqueta(int edad):\n"
        "    return { \"rol\": edad >= 18 ? \"adulto\" : \"menor\" }\n",
        "etiqueta", {Value::integer(20)});

    caso("llamada mutua entre funciones",
        "fn bool es_par(int n):\n"
        "    if n == 0:\n"
        "        return true\n"
        "    return es_impar(n - 1)\n"
        "\n"
        "fn bool es_impar(int n):\n"
        "    if n == 0:\n"
        "        return false\n"
        "    return es_par(n - 1)\n",
        "es_par", {Value::integer(11)});

    if (fallos == 0) {
        std::printf("emit_ir_shadow: todo equivalente, %d fallos\n", fallos);
        return 0;
    }
    std::printf("emit_ir_shadow: %d fallo(s)\n", fallos);
    return 1;
}
