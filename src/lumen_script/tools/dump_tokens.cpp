// Herramienta de desarrollo: vuelca los tokens de un .lum.
// Sirve para verificar el lexer sin depender del parser.
#include <lumen_script/lexer.hpp>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: dump_tokens <fichero.lum> [--quiet]\n";
        return 2;
    }
    bool quiet = (argc > 2 && std::string(argv[2]) == "--quiet");

    lumen_script::SourceFile src;
    src.path = argv[1];
    {
        std::ifstream f(src.path, std::ios::binary);
        if (!f) { std::cerr << "no se puede abrir: " << src.path << "\n"; return 2; }
        std::ostringstream ss; ss << f.rdbuf();
        src.text = ss.str();
    }

    lumen_script::DiagnosticBag diags;
    lumen_script::Lexer lexer(src, diags);
    auto tokens = lexer.tokenize();

    if (!quiet) {
        int depth = 0;
        for (const auto& t : tokens) {
            if (t.is(lumen_script::Tok::Dedent)) --depth;
            std::cout << std::string(depth < 0 ? 0 : depth * 2, ' ')
                      << lumen_script::tok_name(t.kind);
            if (!t.text.empty()) std::cout << " \"" << t.text << "\"";
            std::cout << "  @" << t.loc.line << ":" << t.loc.col << "\n";
            if (t.is(lumen_script::Tok::Indent)) ++depth;
        }
    }

    std::cout << "\ntokens: " << tokens.size()
              << "  errores: " << diags.size() << "\n";
    if (!diags.empty()) std::cout << "\n" << diags.format({&src});
    return diags.empty() ? 0 : 1;
}
