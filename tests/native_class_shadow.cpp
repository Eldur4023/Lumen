// Fase 3 de --native (COMPILACION-NATIVA.md): clases de usuario. A
// diferencia de string/List/Dict, esta pieza no se puede probar a traves de
// compile_native() -- ni siquiera sirviendo HTTP de verdad -- porque hoy es
// estructuralmente inalcanzable desde cualquier programa Lumen en
// ejecucion: una funcion SUELTA no puede tocar una clase (el checker real
// solo resuelve ConstructorCall/ClassMethodCall dentro de una ruta o un
// metodo, que reciben ClassSigs; build_functions() en project.cpp se lo
// niega a proposito a una funcion suelta), y un metodo nunca cruza la ABI
// (el receptor es de tipo clase), asi que nunca entra en por_indice.
// compile_native() maneja esto con seguridad (generadas.empty() -> vuelve
// nullptr, nada se pierde ni se rompe) pero eso significa que, hasta que la
// Fase 4 compile rutas a nativo, el codigo de esta pieza no tiene ningun
// punto de entrada real.
//
// Esta prueba valida el generador de todas formas -- construyendo el C++ de
// la clase y sus metodos directamente (generar_clase_runtime()/
// generar_metodo_nativo(), sin pasar por compile_native()), y ensamblando
// un ejecutable con un main() propio que los llama, igual que
// native_gen_shadow.cpp hace para fib/cuenta_primos. Es la prueba de que el
// C++ generado es correcto; la de que es ALCANZABLE llegara con la Fase 4.
#include <lumen_script/diagnostic.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/native_build.hpp>
#include <lumen_script/native_gen.hpp>
#include <lumen_script/natives.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/vm.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace lumen_script;

static int fallos = 0;

// Firmas y cuerpos de clase, a mano -- el mismo protocolo que
// project.cpp::build_class_signatures()/emit_class_bodies(): metodos
// primero (reservando su hueco en `tabla`), constructores despues, y el
// implicito (automapeo) sintetizado si la clase no declara ninguno.
static bool compilar_clases_bytecode(const Program& prog, FunctionSigs& fns, ClassSigs& classes,
                                     FunctionTable& tabla, DiagnosticBag& diags) {
    for (const auto& c : prog.classes) {
        ClassSig sig;
        for (const auto& f : c.fields) sig.fields.push_back(f.name);
        for (const auto& m : c.methods) {
            size_t idx = tabla.size();
            tabla.push_back(std::make_shared<Chunk>());
            FnSig fs;
            fs.index = idx;
            for (const auto& p : m.params) {
                fs.defaults.push_back(p.default_value.get());
                if (!p.default_value) ++fs.required;
            }
            sig.methods[m.name] = fs;
        }
        for (const auto& ct : c.ctors) {
            size_t idx = tabla.size();
            tabla.push_back(std::make_shared<Chunk>());
            sig.ctors[ct.params.size()] = idx;
        }
        if (sig.ctors.empty() && !c.fields.empty()) {
            size_t idx = tabla.size();
            tabla.push_back(std::make_shared<Chunk>());
            sig.ctors[c.fields.size()] = idx;
        }
        classes[c.name] = std::move(sig);
    }

    for (const auto& c : prog.classes) {
        const ClassSig& sig = classes.at(c.name);
        for (const auto& m : c.methods) {
            Emitter em(diags, &fns, &classes, &prog.imports);
            em.emit_method(c.name, m, *tabla[sig.methods.at(m.name).index]);
        }
        for (const auto& ct : c.ctors) {
            Emitter em(diags, &fns, &classes, &prog.imports);
            em.emit_ctor(c.name, sig.fields, ct, *tabla[sig.ctors.at(ct.params.size())]);
        }
        if (c.ctors.empty() && !c.fields.empty()) {
            CtorDecl implicito;
            implicito.loc = c.loc;
            for (const auto& f : c.fields) {
                Param p;
                p.loc  = f.loc;
                p.name = f.name;
                p.type = f.type;
                implicito.params.push_back(std::move(p));
            }
            Emitter em(diags, &fns, &classes, &prog.imports);
            em.emit_ctor(c.name, sig.fields, implicito, *tabla[sig.ctors.at(c.fields.size())]);
        }
    }
    return diags.empty();
}

// Genera el C++ de todos los metodos de `prog` (asume una sola clase
// representable, para mantener la prueba simple), ensambla un ejecutable
// con `driver` como main(), lo compila y lo ejecuta -- devuelve su salida
// completa (stdout), o cadena vacia si algo fallo (con el motivo impreso).
static std::string compilar_y_correr(const Program& prog, const ClassSigs& classes,
                                     const FunctionSigs& fns, const std::string& driver) {
    TablaClases clases_n;
    TablaRoles  roles_n;
    construir_clases(prog, classes, fns, &prog.imports, clases_n, roles_n);
    if (prog.classes.empty() || !clases_n.count(prog.classes[0].name)) {
        std::printf("  FALLA: la clase no es representable\n");
        return "";
    }
    const std::string& nombre_clase = prog.classes[0].name;

    std::string codigo = "#include <cstdint>\n#include <cstdio>\n#include <string>\n\n" +
                         error_runtime_prelude() + "\n" +
                         generar_clase_runtime(nombre_clase, clases_n.at(nombre_clase)) + "\n";

    std::vector<std::string> nombre_por_indice; // vacio: esta prueba no usa funciones sueltas
    TablaFirmas               firmas_n;
    for (const auto& c : prog.classes) {
        for (const auto& m : c.methods) {
            DiagnosticBag diags_ir;
            Chunk         descartable;
            Emitter       em(diags_ir, nullptr, &classes, &prog.imports);
            IrBlock       body;
            if (!em.check_method(c.name, m, descartable, diags_ir, &body)) {
                std::printf("  FALLA: check_method(%s.%s) no compilo: %s\n", c.name.c_str(),
                           m.name.c_str(),
                           diags_ir.items().empty() ? "?" : diags_ir.items().front().message.c_str());
                return "";
            }
            auto gen = generar_metodo_nativo(c.name, m, body, nombre_por_indice, firmas_n,
                                             clases_n, roles_n);
            if (!gen) {
                std::printf("  FALLA: generar_metodo_nativo(%s.%s) dio nullopt (deberia "
                           "compilar)\n", c.name.c_str(), m.name.c_str());
                return "";
            }
            codigo += gen->firma_cpp + " " + gen->cuerpo_cpp + "\n\n";
        }
    }
    codigo += "int main() {\n" + driver + "    return 0;\n}\n";

    const std::filesystem::path src_path = std::filesystem::temp_directory_path() /
                                           "lumen_native_class_check.cpp";
    const std::filesystem::path bin_path = std::filesystem::temp_directory_path() /
                                           "lumen_native_class_check.bin";
    const std::filesystem::path err_path = std::filesystem::temp_directory_path() /
                                           "lumen_native_class_check.err";
    { std::ofstream out(src_path, std::ios::trunc); out << codigo; }

    std::ostringstream cmd;
    cmd << "g++ -O2 -std=c++20 " << std::quoted(src_path.string()) << " -o "
        << std::quoted(bin_path.string()) << " 2> " << std::quoted(err_path.string());
    if (std::system(cmd.str().c_str()) != 0) {
        std::printf("  FALLA: el C++ generado no compilo -- ver %s\n", err_path.string().c_str());
        std::printf("---- codigo generado ----\n%s\n", codigo.c_str());
        return "";
    }

    FILE* p = popen(bin_path.string().c_str(), "r");
    std::string salida;
    if (p) {
        char buf[256];
        while (std::fgets(buf, sizeof(buf), p)) salida += buf;
        pclose(p);
    }
    return salida;
}

// Punto.cuadrado(): campos, this.x/this.y, y el constructor automapeado
// (sin cuerpo) -- el unico que esta fase compila.
static bool prueba_campos_y_metodo() {
    const std::string src =
        "class Punto:\n"
        "    int x\n"
        "    int y\n"
        "\n"
        "    fn int cuadrado():\n"
        "        return this.x * this.x + this.y * this.y\n";

    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    file.path = "<prueba>";
    file.text = src;
    Lexer  lexer(file, diag_parse);
    Parser parser(lexer.tokenize(), diag_parse);
    parser.parse_into(prog);
    if (!diag_parse.empty()) {
        std::printf("FALLA (campos): no parsea (%s)\n",
                   diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return false;
    }

    FunctionSigs  fns;
    ClassSigs     classes;
    FunctionTable tabla;
    DiagnosticBag diags_vm;
    if (!compilar_clases_bytecode(prog, fns, classes, tabla, diags_vm)) {
        std::printf("FALLA (campos): la VM no compilo: %s\n",
                   diags_vm.items().front().message.c_str());
        return false;
    }

    // Referencia: bytecode real, constructor + metodo.
    lumen::Request req;
    lumen::Response res;
    NativeCtx ctx{req, res};
    VM        vm_ctor;
    auto      r_p = vm_ctor.start(*tabla[classes.at("Punto").ctors.at(2)],
                                 {Value::integer(3), Value::integer(4)}, ctx, &tabla, nullptr);
    VM        vm_m;
    auto      r_m = vm_m.start(*tabla[classes.at("Punto").methods.at("cuadrado").index],
                              {r_p.value}, ctx, &tabla, nullptr);
    if (r_p.status != VM::Status::Done || r_m.status != VM::Status::Done) {
        std::printf("FALLA (campos): la VM no termino\n");
        return false;
    }
    const long long esperado = r_m.value.as_int();

    std::string salida = compilar_y_correr(prog, classes, fns,
        "    LPunto p(3, 4);\n"
        "    printf(\"%lld\\n\", (long long)l_Punto_cuadrado(p));\n");
    long long nativo = salida.empty() ? -1 : std::atoll(salida.c_str());
    if (nativo != esperado) {
        std::printf("  FALLA cuadrado(Punto(3,4)): bytecode=%lld nativo=%lld\n", esperado, nativo);
        return false;
    }
    std::printf("  ok    cuadrado(Punto(3,4)) = %lld (bytecode y C++ generado coinciden)\n",
               esperado);
    return true;
}

// desplazado(): un ConstructorCall DENTRO de un metodo, devolviendo una
// instancia nueva -- y mueve(): this.campo = expr (Assign Member) mas
// semantica de referencia (dos variables sobre la misma instancia).
static bool prueba_ctor_y_referencia() {
    const std::string src =
        "class Punto:\n"
        "    int x\n"
        "    int y\n"
        "\n"
        "    fn Punto desplazado(int dx, int dy):\n"
        "        return Punto(this.x + dx, this.y + dy)\n"
        "\n"
        "    fn void mueve(int dx, int dy):\n"
        "        this.x = this.x + dx\n"
        "        this.y = this.y + dy\n";

    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    file.path = "<prueba>";
    file.text = src;
    Lexer  lexer(file, diag_parse);
    Parser parser(lexer.tokenize(), diag_parse);
    parser.parse_into(prog);
    if (!diag_parse.empty()) {
        std::printf("FALLA (ctor/referencia): no parsea (%s)\n",
                   diag_parse.items().empty() ? "?" : diag_parse.items().front().message.c_str());
        return false;
    }

    FunctionSigs  fns;
    ClassSigs     classes;
    FunctionTable tabla;
    DiagnosticBag diags_vm;
    if (!compilar_clases_bytecode(prog, fns, classes, tabla, diags_vm)) {
        std::printf("FALLA (ctor/referencia): la VM no compilo: %s\n",
                   diags_vm.items().front().message.c_str());
        return false;
    }

    std::string salida = compilar_y_correr(prog, classes, fns,
        "    LPunto p(3, 4);\n"
        "    LPunto q = l_Punto_desplazado(p, 1, 1);\n"
        "    printf(\"%lld %lld\\n\", (long long)q.campo_x(), (long long)q.campo_y());\n"
        "    LPunto a(0, 0);\n"
        "    LPunto b = a;\n"
        "    l_Punto_mueve(b, 5, 6);\n"
        "    printf(\"%lld %lld %lld %lld\\n\", (long long)a.campo_x(), (long long)a.campo_y(),\n"
        "           (long long)b.campo_x(), (long long)b.campo_y());\n");
    if (salida.empty()) return false;

    long long dx, dy, ax, ay, bx, by;
    if (std::sscanf(salida.c_str(), "%lld %lld", &dx, &dy) != 2 || dx != 4 || dy != 5) {
        std::printf("  FALLA desplazado(1,1) sobre Punto(3,4): se esperaba '4 5', salio '%s'\n",
                   salida.c_str());
        return false;
    }
    std::printf("  ok    desplazado(1,1) sobre Punto(3,4) = Punto(4,5): ConstructorCall dentro "
               "de un metodo funciona\n");

    size_t salto = salida.find('\n');
    if (salto == std::string::npos ||
        std::sscanf(salida.c_str() + salto + 1, "%lld %lld %lld %lld", &ax, &ay, &bx, &by) != 4 ||
        ax != 5 || ay != 6 || bx != 5 || by != 6) {
        std::printf("  FALLA alias: se esperaba a=b=(5,6), salio '%s'\n", salida.c_str());
        return false;
    }
    std::printf("  ok    alias: 'b = a' y mueve(b) mutan la MISMA instancia (a=(%lld,%lld) "
               "b=(%lld,%lld)) -- semantica de referencia real (§8)\n", ax, ay, bx, by);
    return true;
}

// El limite documentado arriba: un programa que solo tiene una clase (sin
// ninguna funcion suelta que cruce la ABI) no debe romper nada -- solo
// quedarse sin nada que ofrecer a la VM.
static bool prueba_inalcanzable_no_rompe() {
    const std::string src =
        "class Punto:\n"
        "    int x\n"
        "    int y\n"
        "\n"
        "    fn int cuadrado():\n"
        "        return this.x * this.x + this.y * this.y\n";

    SourceFile    file;
    DiagnosticBag diag_parse;
    Program       prog;
    file.path = "<prueba>";
    file.text = src;
    Lexer  lexer(file, diag_parse);
    Parser parser(lexer.tokenize(), diag_parse);
    parser.parse_into(prog);
    if (!diag_parse.empty()) return false;

    FunctionSigs  fns;
    ClassSigs     classes;
    FunctionTable tabla;
    DiagnosticBag diags_vm;
    if (!compilar_clases_bytecode(prog, fns, classes, tabla, diags_vm)) return false;

    std::string aviso;
    auto cache_dir = std::filesystem::temp_directory_path() / "lumen_native_class_inalcanzable";
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
    auto nativo = compile_native(prog, fns, classes, cache_dir, aviso);
    std::filesystem::remove_all(cache_dir, ec);
    if (nativo != nullptr) {
        std::printf("  FALLA: se esperaba nullptr (nada cruza la ABI todavia), hubo %zu "
                   "funcion(es)\n", nativo->compiled());
        return false;
    }
    std::printf("  ok    un programa solo-clases no rompe nada: compile_native() da nullptr "
               "sin aviso de error\n");
    return true;
}

int main() {
    if (!prueba_campos_y_metodo()) ++fallos;
    if (!prueba_ctor_y_referencia()) ++fallos;
    if (!prueba_inalcanzable_no_rompe()) ++fallos;

    std::error_code ec;
    std::filesystem::remove(std::filesystem::temp_directory_path() / "lumen_native_class_check.cpp", ec);
    std::filesystem::remove(std::filesystem::temp_directory_path() / "lumen_native_class_check.bin", ec);
    std::filesystem::remove(std::filesystem::temp_directory_path() / "lumen_native_class_check.err", ec);

    if (fallos == 0) {
        std::printf("native_class_shadow: el C++ generado para clases coincide con la VM en "
                   "todos los casos\n");
        return 0;
    }
    std::printf("native_class_shadow: %d fallo(s)\n", fallos);
    return 1;
}
