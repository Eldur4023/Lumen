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
#include <functional>
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
// (check_condition), y compara ambas listas de mensajes. Ademas comprueba la
// otra mitad de lo que hace check_expr/check_call desde que devuelven
// IrExprPtr: que el nodo construido sea no-nulo exactamente cuando la
// compilacion real tuvo exito, y nulo exactamente cuando fallo -- un IrExpr
// a medio construir no deberia sobrevivir a un error en ningun caso.
// `inspeccionar`, si se da, recibe el IrExpr construido (solo se llama si no
// es nulo) para comprobar su forma (call_shape, slot, etc.).
static void caso(const char* nombre, const std::string& src,
                 const std::vector<NombreTipado>& names,
                 const std::string& esperado_substr,
                 const FunctionSigs* fns = nullptr, const ClassSigs* classes = nullptr,
                 const std::set<std::string>* imports = nullptr,
                 const std::function<bool(const IrExpr&, std::string&)>& inspeccionar = {}) {
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
    IrExprPtr ir = em.check_condition(*e, names, chunk, diags_shadow);

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
        if (!ir) {
            ++fallos;
            std::printf("  FALLA %s (compilo limpio pero check_expr/check_call devolvio "
                        "nullptr)\n", nombre);
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
        if (ir) {
            ++fallos;
            std::printf("  FALLA %s (fallo la compilacion pero check_expr/check_call "
                        "devolvio un IrExpr no nulo)\n", nombre);
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

    if (ir && inspeccionar) {
        std::string motivo;
        if (!inspeccionar(*ir, motivo)) {
            ++fallos;
            std::printf("  FALLA %s (shape del IrExpr: %s)\n", nombre, motivo.c_str());
            return;
        }
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

    // ── Camino feliz: ninguna de las dos vias debe quejarse, y ademas se
    //    inspecciona la forma del IrExpr construido (call_shape, slot...)
    //    contra las 8 formas de COMPILACION-NATIVA.md §1.2 que sean
    //    alcanzables desde una expresion suelta (check_condition fija
    //    route_method_ a "", asi que ReservedMemberCall solo se puede
    //    ejercitar en su rama de error, ya cubierta mas arriba) ──────────
    caso("metodo builtin valido sobre string", "quien.upper()", {{"quien", "string"}}, "",
         nullptr, nullptr, nullptr,
         [](const IrExpr& ir, std::string& why) {
             if (ir.kind != IrExprKind::Call) { why = "no es Call"; return false; }
             if (ir.call_shape != IrCallShape::BuiltinMethodCall) {
                 why = "call_shape no es BuiltinMethodCall"; return false;
             }
             if (ir.call_name != "upper") { why = "call_name != 'upper'"; return false; }
             if (!ir.object || ir.object->kind != IrExprKind::Ident) {
                 why = "el receptor no es el Ident esperado"; return false;
             }
             return true;
         });

    {
        ClassSigs classes;
        classes["P"].fields = {"x"};
        caso("campo valido de una clase", "p.x", {{"p", "P"}}, "", nullptr, &classes);
    }

    {
        FunctionSigs fns;
        fns["saluda"].required = 1;
        fns["saluda"].defaults.resize(1);
        caso("llamada valida a funcion de usuario", "saluda(1)", {}, "", &fns,
             nullptr, nullptr,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.call_shape != IrCallShape::UserFunctionCall) {
                     why = "call_shape no es UserFunctionCall"; return false;
                 }
                 if (ir.call_index != 0) { why = "call_index no es el indice esperado"; return false; }
                 if (ir.args.size() != 1) { why = "no lleva el unico argumento dado"; return false; }
                 return true;
             });
    }

    {
        ClassSigs classes;
        classes["Usuario"].ctors[2] = 3;
        caso("llamada valida a constructor", "Usuario(1, 2)", {}, "", nullptr, &classes,
             nullptr,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.call_shape != IrCallShape::ConstructorCall) {
                     why = "call_shape no es ConstructorCall"; return false;
                 }
                 if (ir.call_index != 3) { why = "call_index no es el indice del ctor"; return false; }
                 if (ir.args.size() != 2) { why = "no lleva los 2 argumentos dados"; return false; }
                 return true;
             });
    }

    {
        ClassSigs classes;
        classes["Punto"].fields = {"x", "y"};
        classes["Punto"].methods["cuadrado"] = {5, 0, {}};
        caso("llamada valida a metodo de clase", "p.cuadrado()", {{"p", "Punto"}}, "",
             nullptr, &classes, nullptr,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.call_shape != IrCallShape::ClassMethodCall) {
                     why = "call_shape no es ClassMethodCall"; return false;
                 }
                 if (ir.call_index != 5) { why = "call_index no es el indice del metodo"; return false; }
                 if (!ir.object || ir.object->kind != IrExprKind::Ident || ir.object->slot != 0) {
                     why = "el receptor no lleva la ranura resuelta de 'p'"; return false;
                 }
                 return true;
             });
    }

    {
        std::set<std::string> imports = {"sqlite"};
        caso("llamada valida a builtin async con await",
             "await sqlite.query(\"select 1\")", {}, "", nullptr, nullptr, &imports,
             [](const IrExpr& ir, std::string& why) {
                 if (ir.kind != IrExprKind::Await) { why = "no es Await"; return false; }
                 if (!ir.lhs || ir.lhs->call_shape != IrCallShape::DbModuleCall) {
                     why = "la llamada envuelta no es DbModuleCall"; return false;
                 }
                 if (ir.lhs->call_name != "sqlite") { why = "call_name no es 'sqlite'"; return false; }
                 if (!ir.lhs->awaited) { why = "awaited no quedo en true"; return false; }
                 if (ir.lhs->args.size() != 1) { why = "no lleva el argumento de la consulta"; return false; }
                 return true;
             });
    }

    caso("identificador con ranura resuelta", "equis", {{"equis", "int"}}, "",
         nullptr, nullptr, nullptr,
         [](const IrExpr& ir, std::string& why) {
             if (ir.kind != IrExprKind::Ident) { why = "no es Ident"; return false; }
             if (ir.slot != 0) { why = "slot no es 0"; return false; }
             if (ir.type != Type::primitive(Type::Kind::Int)) { why = "type no es int"; return false; }
             return true;
         });

    if (fallos == 0) {
        std::printf("check_expr_shadow: todo reproducido, %d fallos\n", fallos);
        return 0;
    }
    std::printf("check_expr_shadow: %d fallo(s)\n", fallos);
    return 1;
}
