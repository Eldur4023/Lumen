// Verificacion de Emitter::check_expr/check_call (fase 1, COMPILACION-NATIVA.md
// paso 2): comprueba que reproducen, letra por letra, los diagnosticos que ya
// produce la compilacion real (emit_expr/emit_call vistos a traves de
// emit_condition) para una expresion suelta. No sustituye a emit_expr en
// ningun sitio real todavia -- esto es solo la comparacion que el plan pide
// antes de dar ese paso.
#include <lumen_script/diagnostic.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/parser.hpp>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace lumen_script;

static int fallos = 0;

static ExprPtr parse(const std::string& src, SourceFile& file, DiagnosticBag& diags) {
    file.path = "<prueba>";
    file.text = src;
    Lexer lexer(file, diags);
    Parser parser(lexer.tokenize(), diags);
    return parser.parse_single_expression();
}

// Compila `src` como expresion suelta con `names` declarados, una vez con la
// via real (emit_condition) y otra con el checker en paralelo
// (check_condition), y compara ambas listas de mensajes.
static void caso(const char* nombre, const std::string& src,
                 const std::vector<NombreTipado>& names,
                 const std::string& esperado_substr,
                 const FunctionSigs* fns = nullptr, const ClassSigs* classes = nullptr,
                 const std::set<std::string>* imports = nullptr) {
    SourceFile   file;
    DiagnosticBag diag_parse;
    ExprPtr       e = parse(src, file, diag_parse);
    if (!e) {
        ++fallos;
        std::printf("  FALLA %s (no parsea: %s)\n", nombre,
                    diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return;
    }

    DiagnosticBag diags_real, diags_shadow;
    Emitter       em(diags_real, fns, classes, imports);
    Chunk         chunk;
    em.emit_condition(*e, names, chunk);
    em.check_condition(*e, names, diags_shadow);

    auto texts = [](const DiagnosticBag& d) {
        std::vector<std::string> v;
        for (const auto& it : d.items()) v.push_back(it.message);
        return v;
    };
    std::vector<std::string> real = texts(diags_real), shadow = texts(diags_shadow);

    // Cadena vacia = camino feliz: se espera que la compilacion real no de
    // NINGUN diagnostico (y por tanto tampoco el checker en paralelo).
    if (esperado_substr.empty()) {
        if (!real.empty()) {
            ++fallos;
            std::printf("  FALLA %s (se esperaba que compilara limpio, dio %zu error(es))\n",
                        nombre, real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    } else {
        bool real_tiene_esperado = false;
        for (const auto& m : real)
            if (m.find(esperado_substr) != std::string::npos) real_tiene_esperado = true;

        if (!real_tiene_esperado) {
            ++fallos;
            std::printf("  FALLA %s (la compilacion real no dio \"%s\"; dio %zu error(es))\n",
                        nombre, esperado_substr.c_str(), real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    }

    if (real != shadow) {
        ++fallos;
        std::printf("  FALLA %s (check_expr/check_call no reproduce lo mismo)\n", nombre);
        std::printf("           real   (%zu): ", real.size());
        for (const auto& m : real) std::printf("[%s] ", m.c_str());
        std::printf("\n           shadow (%zu): ", shadow.size());
        for (const auto& m : shadow) std::printf("[%s] ", m.c_str());
        std::printf("\n");
        return;
    }

    std::printf("  ok    %s\n", nombre);
}

int main() {
    // ── Las 6 ramas de emit_expr/emit_call que ya cubre el corpus real
    //    (tests/casos/malos/*.lum) ────────────────────────────────────────
    caso("builtin async sin await (malos/await.lum)", "sleep(10)", {}, "es asincrono");

    caso("objeto reservado fuera de sitio (malos/sse.lum)", "sse.send(\"x\")", {},
         "solo existe dentro de una ruta sse");

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        caso("campo inexistente en clase (malos/campo_tipo.lum)", "p.noexiste",
             {{"p", "P"}}, "no tiene un campo", nullptr, &classes);
    }

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        caso("metodo inexistente en clase (malos/metodo.lum)", "p.noexiste()",
             {{"p", "P"}}, "no tiene un metodo", nullptr, &classes);
    }

    caso("metodo builtin inexistente sobre string (malos/metodo_tipo.lum)",
         "quien.mayusculas()", {{"quien", "string"}}, "no tienen el metodo");

    caso("modulo de BD sin import (malos/import.lum)",
         "await sqlite.query(\"select 1\")", {}, "falta 'import sqlite'");

    // ── Ramas de los otros call-shapes (COMPILACION-NATIVA.md §1.2),
    //    sin corpus dedicado pero construidas contra la compilacion real ──
    {
        ClassSigs classes;
        classes["Usuario"].ctors[1] = 0;
        caso("aridad de constructor", "Usuario(1, 2, 3)", {}, "no tiene constructor de 3",
             nullptr, &classes);
    }

    {
        FunctionSigs fns;
        fns["saluda"].required = 1;
        fns["saluda"].defaults.resize(1);
        caso("aridad de funcion de usuario", "saluda()", {}, "espera 1 argumento", &fns);
    }

    caso("funcion desconocida", "no_existe_esta_funcion()", {}, "funcion desconocida");

    caso("identificador no declarado", "equis", {}, "no esta declarada");

    caso("builtin usado como valor, no llamado", "len", {}, "es un builtin");

    caso("await sobre algo que no es una llamada", "await 5", {},
         "solo se aplica a una llamada asincrona");

    caso("llamada a nada llamable (shape 8: invalid)", "(a + b)()",
         {{"a", "int"}, {"b", "int"}}, "solo se pueden llamar builtins o metodos");

    caso("ws fuera de una ruta ws", "ws.send(\"x\")", {}, "solo existe dentro de una ruta ws");

    // ── Camino feliz: ninguna de las dos vias debe quejarse ──────────────
    caso("metodo builtin valido sobre string", "quien.upper()", {{"quien", "string"}}, "");

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        caso("campo valido de una clase", "p.x", {{"p", "P"}}, "", nullptr, &classes);
    }

    {
        FunctionSigs fns;
        fns["saluda"].required = 1;
        fns["saluda"].defaults.resize(1);
        caso("llamada valida a funcion de usuario", "saluda(1)", {}, "", &fns);
    }

    {
        std::set<std::string> imports = {"sqlite"};
        caso("llamada valida a builtin async con await",
             "await sqlite.query(\"select 1\")", {}, "", nullptr, nullptr, &imports);
    }

    if (fallos == 0) {
        std::printf("check_expr_shadow: todo reproducido, %d fallos\n", fallos);
        return 0;
    }
    std::printf("check_expr_shadow: %d fallo(s)\n", fallos);
    return 1;
}
