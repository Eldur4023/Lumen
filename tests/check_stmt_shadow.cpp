// Verificacion de Emitter::check_stmt/check_block (fase 1, COMPILACION-
// NATIVA.md, paso 3 de la seccion 1.1): compara, sentencia a sentencia, la
// salida de check_route/check_function/check_method/check_ctor contra la
// compilacion real (emit_route/emit_function/emit_method/emit_ctor) sobre
// funciones completas de verdad -- no expresiones sueltas, que es lo que ya
// verifico tests/check_expr_shadow.cpp.
#include <lumen_script/diagnostic.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/parser.hpp>

#include <cstdio>
#include <functional>
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

static std::vector<std::string> texts(const DiagnosticBag& d) {
    std::vector<std::string> v;
    for (const auto& it : d.items()) v.push_back(it.message);
    return v;
}

// Compila la primera `fn` de `src` como funcion de usuario, una vez con la
// via real y otra con el checker en paralelo, y compara ambas listas.
// `inspeccionar`, si se da, recibe el IrBlock construido (solo si el camino
// feliz aplico, i.e. `esperado_substr` vacio) para comprobar su forma.
static void caso_fn(const char* nombre, const std::string& src,
                    const std::string& esperado_substr,
                    const std::function<bool(const IrBlock&, std::string&)>& inspeccionar = {}) {
    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog) || prog.functions.empty()) {
        ++fallos;
        std::printf("  FALLA %s (no parsea: %s)\n", nombre,
                    diag_parse.items().empty() ? "sin fn" : diag_parse.items().front().message.c_str());
        return;
    }

    DiagnosticBag diags_real, diags_shadow;
    Emitter       em(diags_real, nullptr, nullptr, nullptr);
    Chunk         chunk;
    em.emit_function(prog.functions[0], chunk);
    IrBlock body;
    em.check_function(prog.functions[0], chunk, diags_shadow, &body);

    std::vector<std::string> real = texts(diags_real), shadow = texts(diags_shadow);

    if (esperado_substr.empty()) {
        if (!real.empty()) {
            ++fallos;
            std::printf("  FALLA %s (se esperaba que compilara limpio, dio %zu error(es))\n",
                        nombre, real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    } else {
        bool ok = false;
        for (const auto& m : real) if (m.find(esperado_substr) != std::string::npos) ok = true;
        if (!ok) {
            ++fallos;
            std::printf("  FALLA %s (la compilacion real no dio \"%s\"; dio %zu error(es))\n",
                        nombre, esperado_substr.c_str(), real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    }

    if (real != shadow) {
        ++fallos;
        std::printf("  FALLA %s (check_stmt no reproduce lo mismo)\n", nombre);
        std::printf("           real   (%zu): ", real.size());
        for (const auto& m : real) std::printf("[%s] ", m.c_str());
        std::printf("\n           shadow (%zu): ", shadow.size());
        for (const auto& m : shadow) std::printf("[%s] ", m.c_str());
        std::printf("\n");
        return;
    }

    if (esperado_substr.empty() && inspeccionar) {
        std::string motivo;
        if (!inspeccionar(body, motivo)) {
            ++fallos;
            std::printf("  FALLA %s (shape del IrBlock: %s)\n", nombre, motivo.c_str());
            return;
        }
    }

    std::printf("  ok    %s\n", nombre);
}

// Igual que caso_fn, pero para una clase completa: compila el primer metodo
// y el primer constructor (si los hay) de la primera clase.
static void caso_clase(const char* nombre, const std::string& src,
                       const std::string& esperado_substr,
                       const std::function<bool(const IrBlock&, std::string&)>& inspeccionar = {}) {
    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    if (!parse_program(src, file, diag_parse, prog) || prog.classes.empty()) {
        ++fallos;
        std::printf("  FALLA %s (no parsea: %s)\n", nombre,
                    diag_parse.items().empty() ? "sin class" : diag_parse.items().front().message.c_str());
        return;
    }
    const ClassDecl& cls = prog.classes[0];
    std::vector<std::string> fields;
    for (const auto& f : cls.fields) fields.push_back(f.name);

    // comprobar_campo/check_field solo miran classes_ si viene con la clase
    // ya registrada: sin esto, "P" no tiene tabla y el campo se da por bueno.
    ClassSigs classes;
    classes[cls.name].fields = fields;
    for (const auto& m : cls.methods) classes[cls.name].methods[m.name] = {};

    DiagnosticBag diags_real, diags_shadow;
    Emitter       em(diags_real, nullptr, &classes, nullptr);
    IrBlock       body;

    if (!cls.methods.empty()) {
        Chunk chunk;
        em.emit_method(cls.name, cls.methods[0], chunk);
        em.check_method(cls.name, cls.methods[0], chunk, diags_shadow, &body);
    }
    if (!cls.ctors.empty()) {
        Chunk chunk;
        em.emit_ctor(cls.name, fields, cls.ctors[0], chunk);
        em.check_ctor(cls.name, fields, cls.ctors[0], chunk, diags_shadow, &body);
    }

    std::vector<std::string> real = texts(diags_real), shadow = texts(diags_shadow);

    if (esperado_substr.empty()) {
        if (!real.empty()) {
            ++fallos;
            std::printf("  FALLA %s (se esperaba que compilara limpio, dio %zu error(es))\n",
                        nombre, real.size());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    } else {
        bool ok = false;
        for (const auto& m : real) if (m.find(esperado_substr) != std::string::npos) ok = true;
        if (!ok) {
            ++fallos;
            std::printf("  FALLA %s (la compilacion real no dio \"%s\")\n", nombre,
                        esperado_substr.c_str());
            for (const auto& m : real) std::printf("           real: %s\n", m.c_str());
            return;
        }
    }

    if (real != shadow) {
        ++fallos;
        std::printf("  FALLA %s (check_stmt no reproduce lo mismo)\n", nombre);
        std::printf("           real   (%zu): ", real.size());
        for (const auto& m : real) std::printf("[%s] ", m.c_str());
        std::printf("\n           shadow (%zu): ", shadow.size());
        for (const auto& m : shadow) std::printf("[%s] ", m.c_str());
        std::printf("\n");
        return;
    }

    if (esperado_substr.empty() && inspeccionar) {
        std::string motivo;
        if (!inspeccionar(body, motivo)) {
            ++fallos;
            std::printf("  FALLA %s (shape del IrBlock: %s)\n", nombre, motivo.c_str());
            return;
        }
    }

    std::printf("  ok    %s\n", nombre);
}

int main() {
    // ── Camino feliz: VarDecl, if/else, while, for con break/continue,
    //    try/catch, indices, ++/--, asignaciones compuestas ya desazucaradas.
    caso_fn("aritmetica y control de flujo basico",
        "fn int suma(int a, int b):\n"
        "    int c = a + b\n"
        "    if c > 10:\n"
        "        return c\n"
        "    else:\n"
        "        return 0\n",
        "",
        [](const IrBlock& body, std::string& why) {
            // a: ranura 0, b: ranura 1 (parametros) -> c: ranura 2 (VarDecl).
            if (body.size() != 2) { why = "no son 2 sentencias (VarDecl + If)"; return false; }
            const IrStmt& decl = *body[0];
            if (decl.kind != IrStmtKind::VarDecl) { why = "la 1a no es VarDecl"; return false; }
            if (decl.name != "c" || decl.slot != 2) { why = "'c' no quedo en la ranura 2"; return false; }
            if (decl.decl_type != Type::primitive(Type::Kind::Int)) {
                why = "decl_type de 'c' no es int"; return false;
            }
            const IrStmt& si = *body[1];
            if (si.kind != IrStmtKind::If) { why = "la 2a no es If"; return false; }
            if (si.body.size() != 1 || si.orelse.size() != 1) {
                why = "If no lleva un Return en cada rama"; return false;
            }
            return true;
        });

    caso_fn("while con asignacion",
        "fn int cuenta(int n):\n"
        "    int i = 0\n"
        "    int total = 0\n"
        "    while i < n:\n"
        "        total = total + i\n"
        "        i = i + 1\n"
        "    return total\n",
        "",
        [](const IrBlock& body, std::string& why) {
            // n: ranura 0, i: ranura 1, total: ranura 2.
            const IrStmt& bucle = *body[2];
            if (bucle.kind != IrStmtKind::While) { why = "la 3a no es While"; return false; }
            const IrStmt& asigna_total = *bucle.body[0];
            if (asigna_total.kind != IrStmtKind::Assign ||
                asigna_total.assign_target != IrAssignTarget::Local ||
                asigna_total.assign_slot != 2) {
                why = "'total = ...' no quedo como Assign Local a la ranura 2"; return false;
            }
            return true;
        });

    caso_fn("for con break y continue",
        "fn int recorre(List<int> xs):\n"
        "    int total = 0\n"
        "    for int x in xs:\n"
        "        if x == 0:\n"
        "            continue\n"
        "        if x < 0:\n"
        "            break\n"
        "        total = total + x\n"
        "    return total\n",
        "",
        [](const IrBlock& body, std::string& why) {
            // xs: ranura 0, total: ranura 1; dentro del For: items/count/index
            // (2, 3, 4) y x en la ranura 5.
            const IrStmt& bucle = *body[1];
            if (bucle.kind != IrStmtKind::For) { why = "la 2a no es For"; return false; }
            if (bucle.name != "x" || bucle.slot != 5) {
                why = "'x' no quedo en la ranura 5 tras los 3 auxiliares"; return false;
            }
            if (bucle.decl_type != Type::primitive(Type::Kind::Int)) {
                why = "decl_type de 'x' no es int"; return false;
            }
            if (!bucle.target || bucle.target->kind != IrExprKind::Ident ||
                bucle.target->slot != 0) {
                why = "el iterable no es el Ident 'xs' con su ranura resuelta"; return false;
            }
            return true;
        });

    caso_fn("try/catch",
        "fn int intenta():\n"
        "    try:\n"
        "        return 1\n"
        "    catch e:\n"
        "        return 0\n",
        "",
        [](const IrBlock& body, std::string& why) {
            const IrStmt& intento = *body[0];
            if (intento.kind != IrStmtKind::Try) { why = "no es Try"; return false; }
            if (intento.name != "e" || intento.slot != 0) {
                why = "'e' no quedo con nombre/ranura resueltos"; return false;
            }
            if (intento.body.size() != 1 || intento.orelse.size() != 1) {
                why = "Try no lleva un Return en el cuerpo y en el catch"; return false;
            }
            return true;
        });

    caso_fn("indices y ++/--",
        "fn int mezcla():\n"
        "    List<int> l = [1, 2, 3]\n"
        "    l[0] = 9\n"
        "    int i = 0\n"
        "    i++\n"
        "    return l[0] + i\n",
        "",
        [](const IrBlock& body, std::string& why) {
            const IrStmt& asigna = *body[1];
            if (asigna.kind != IrStmtKind::Assign ||
                asigna.assign_target != IrAssignTarget::Index) {
                why = "'l[0] = 9' no quedo como Assign Index"; return false;
            }
            if (!asigna.assign_object || asigna.assign_object->slot != 0) {
                why = "el objeto indexado no es 'l' con su ranura resuelta"; return false;
            }
            return true;
        });

    // ── Errores: uno por cada rama nueva que toca check_stmt ─────────────
    caso_fn("asignar a variable no declarada",
        "fn int malo():\n"
        "    equis = 1\n"
        "    return equis\n",
        "is not declared");

    caso_fn("break fuera de un bucle",
        "fn int malo2():\n"
        "    break\n"
        "    return 0\n",
        "'break' outside a loop");

    caso_fn("continue fuera de un bucle",
        "fn int malo3():\n"
        "    continue\n"
        "    return 0\n",
        "'continue' outside a loop");

    caso_fn("require con condicion que usa una variable no declarada",
        "fn int malo4():\n"
        "    require equis else 0\n"
        "    return 1\n",
        "is not declared");

    // ── Clases: campo valido/invalido en Assign a un Member, y constructor
    //    sin cuerpo con un parametro que no es campo ───────────────────────
    caso_clase("asignacion a campo valido en un metodo",
        "class P:\n"
        "    int x\n"
        "\n"
        "    fn void pon(int v):\n"
        "        this.x = v\n",
        "",
        [](const IrBlock& body, std::string& why) {
            if (body.empty()) { why = "cuerpo vacio"; return false; }
            const IrStmt& asigna = *body[0];
            if (asigna.kind != IrStmtKind::Assign ||
                asigna.assign_target != IrAssignTarget::Member) {
                why = "'this.x = v' no quedo como Assign Member"; return false;
            }
            if (asigna.assign_field != "x") { why = "assign_field no es 'x'"; return false; }
            // this: ranura 0.
            if (!asigna.assign_object || asigna.assign_object->kind != IrExprKind::This ||
                asigna.assign_object->slot != 0) {
                why = "el receptor no es el 'this' con su ranura resuelta"; return false;
            }
            return true;
        });

    caso_clase("asignacion a campo inexistente en un metodo",
        "class P:\n"
        "    int x\n"
        "\n"
        "    fn void pon(int v):\n"
        "        this.noexiste = v\n",
        "has no field");

    caso_clase("constructor implicito con parametro que no es campo",
        "class P:\n"
        "    int x\n"
        "\n"
        "    P(int x, int sobra)\n",
        "is not a field");

    if (fallos == 0) {
        std::printf("check_stmt_shadow: todo reproducido, %d fallos\n", fallos);
        return 0;
    }
    std::printf("check_stmt_shadow: %d fallo(s)\n", fallos);
    return 1;
}
