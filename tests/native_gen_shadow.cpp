// Fase 2 de --native (COMPILACION-NATIVA.md): "esta fase es la que valida o
// tumba la tesis entera del documento". El criterio de aceptacion es
// concreto -- fib y cuenta_primos (los mismos del banco de pruebas) dan
// resultados identicos a la VM -- y esta prueba lo comprueba de verdad:
// genera C++ real a partir del IR, lo compila con el compilador del
// sistema, lo ejecuta, y compara su salida contra la misma funcion
// corriendo en el VM.
#include <lumen_script/diagnostic.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/native_gen.hpp>
#include <lumen_script/natives.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/vm.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
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

static long long ejecutar_vm(const Chunk& chunk, long long arg, const FunctionTable* fns) {
    lumen::Request  req;
    lumen::Response res;
    NativeCtx       ctx{req, res};
    VM              vm;
    VM::Result      r = vm.start(chunk, {Value::integer(arg)}, ctx, fns);
    return r.status == VM::Status::Done ? r.value.as_int() : -1;
}

int main() {
    // Mismas fib()/cuenta_primos() de bench/lumen/app.lum, letra por letra.
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
    std::vector<std::string> por_indice(prog.functions.size());
    for (const auto& [name, sig] : sigs) por_indice[sig.index] = name;

    TablaFirmas firmas;
    for (const auto& f : prog.functions) {
        FirmaNativa firma;
        firma.retorno = Type::from_declared(f.return_type);
        for (const auto& p : f.params) firma.params.push_back(Type::from_declared(p.type));
        firmas[f.name] = std::move(firma);
    }

    // 1) VM real: emit_function tal cual lo llama project.cpp.
    FunctionTable tabla_vm(prog.functions.size());
    DiagnosticBag diags_vm;
    // 1b) El IR de cada funcion, para generar C++ a partir de el. Un
    // DiagnosticBag aparte: no debe interferir con la compilacion real de
    // arriba, solo hace falta el arbol.
    std::vector<IrBlock> cuerpos(prog.functions.size());
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter em(diags_vm, &sigs, nullptr, &prog.imports);
        em.emit_function(prog.functions[i], *chunk);
        tabla_vm[i] = chunk;

        DiagnosticBag diags_ir;
        Chunk         descartable;
        Emitter       em2(diags_ir, &sigs, nullptr, &prog.imports);
        em2.check_function(prog.functions[i], descartable, diags_ir, &cuerpos[i]);
    }
    if (!diags_vm.empty()) {
        std::printf("FALLA: la VM no compilo: %s\n", diags_vm.items().front().message.c_str());
        return 1;
    }

    // 2) Genera C++ para cada funcion. El criterio de aceptacion de esta
    // fase exige que fib/cuenta_primos SI se puedan generar -- si alguna no
    // se pudiera, seria una regresion del generador, no un resultado
    // aceptable que silenciar.
    std::string codigo = "#include <cstdint>\n#include <string>\n\n" + error_runtime_prelude() + "\n";
    for (size_t i = 0; i < prog.functions.size(); ++i) {
        auto f = generar_funcion_nativa(prog.functions[i], cuerpos[i], por_indice, firmas);
        if (!f) {
            std::printf("FALLA: '%s' no se pudo generar a C++ (deberia, para el criterio de "
                        "esta fase)\n", prog.functions[i].name.c_str());
            return 1;
        }
        codigo += f->firma_cpp + " " + f->cuerpo_cpp + "\n\n";
    }

    // Driver minimo: imprime el resultado de llamar a la funcion pedida, sin
    // enlazar una biblioteca dinamica -- basta con un ejecutable y popen().
    codigo +=
        "#include <cstdio>\n"
        "#include <cstdlib>\n"
        "#include <cstring>\n"
        "int main(int argc, char** argv) {\n"
        "    if (argc < 3) return 1;\n"
        "    long long n = atoll(argv[2]);\n"
        "    long long r = 0;\n"
        "    if (!strcmp(argv[1], \"fib\")) r = (long long)l_fib(n);\n"
        "    else if (!strcmp(argv[1], \"cuenta_primos\")) r = (long long)l_cuenta_primos(n);\n"
        "    else return 2;\n"
        "    printf(\"%lld\\n\", r);\n"
        "    return 0;\n"
        "}\n";

    const char* src_path = "/tmp/lumen_native_gen_check.cpp";
    const char* bin_path = "/tmp/lumen_native_gen_check.bin";
    const char* err_path = "/tmp/lumen_native_gen_check.err";
    {
        std::ofstream out(src_path, std::ios::trunc);
        out << codigo;
    }

    std::string cmd_compila = std::string("g++ -O2 -std=c++20 ") + src_path + " -o " + bin_path +
                              " 2>" + err_path;
    if (std::system(cmd_compila.c_str()) != 0) {
        std::printf("FALLA: el C++ generado no compilo -- ver %s\n", err_path);
        std::printf("---- codigo generado ----\n%s\n", codigo.c_str());
        return 1;
    }

    auto comparar = [&](const char* nombre, long long arg) {
        long long vm_val = ejecutar_vm(*tabla_vm[sigs.at(nombre).index], arg, &tabla_vm);

        std::string cmd = std::string(bin_path) + " " + nombre + " " + std::to_string(arg);
        FILE* p = popen(cmd.c_str(), "r");
        char  buf[64] = {0};
        if (p) {
            if (!std::fgets(buf, sizeof(buf), p)) buf[0] = 0;
            pclose(p);
        }
        long long nativo = std::atoll(buf);

        if (nativo != vm_val) {
            ++fallos;
            std::printf("  FALLA %s(%lld): vm=%lld nativo=%lld\n", nombre, arg, vm_val, nativo);
        } else {
            std::printf("  ok    %s(%lld) = %lld (vm y nativo coinciden)\n", nombre, arg, vm_val);
        }
    };

    comparar("fib", 10);
    comparar("fib", 25);
    comparar("fib", 30);
    comparar("cuenta_primos", 1000);
    comparar("cuenta_primos", 100000);

    if (fallos == 0) {
        std::printf("native_gen_shadow: vm y nativo coinciden en todos los casos\n");
        return 0;
    }
    std::printf("native_gen_shadow: %d fallo(s)\n", fallos);
    return 1;
}
