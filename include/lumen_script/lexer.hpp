#pragma once
#include <vector>
#include "token.hpp"
#include "diagnostic.hpp"

namespace lumen_script {

// Lexer con bloques por indentacion al estilo Python.
//
// Reglas de linea logica:
//   • Las lineas en blanco y las que solo tienen comentario no generan Newline
//     ni afectan a la indentacion.
//   • Dentro de (), [] o {} se suprimen Newline/Indent/Dedent: una llamada
//     puede repartirse en varias lineas.
//   • Una linea cuyo primer token es '.' continua la anterior, para permitir
//     encadenar sobre el valor devuelto:
//         return render("x.html")
//                  .status(201)
//
// Regla lexica del grammar: '>' '>' NUNCA se fusionan en un token de
// desplazamiento, para que List<Dict<string,int>> cierre sin ambiguedad.
class Lexer {
public:
    Lexer(const SourceFile& file, DiagnosticBag& diags)
        : file_(file), diags_(diags) {}

    // Tokeniza el fichero entero.  Siempre termina en EndOfFile, con los Dedent
    // pendientes emitidos antes.  Los errores van al DiagnosticBag; el vector
    // devuelto sigue siendo utilizable para que el parser avance y reporte mas.
    std::vector<Token> tokenize();

private:
    const SourceFile& file_;
    DiagnosticBag&    diags_;

    size_t pos_  = 0;
    int    line_ = 1;
    int    col_  = 1;

    std::vector<int> indents_{0};
    int  bracket_depth_ = 0;
    bool at_line_start_ = true;

    std::vector<Token> out_;

    char peek(size_t ahead = 0) const;
    bool eof() const { return pos_ >= file_.text.size(); }
    char advance();

    SourceLoc here() const { return {&file_.path, line_, col_}; }
    void push(Tok kind, SourceLoc loc, std::string text = {});

    void handle_line_start();
    void lex_token();
    void lex_number(SourceLoc loc);
    void lex_string(SourceLoc loc);
    void lex_string_multi(SourceLoc loc);
    void lex_ident(SourceLoc loc);
    void skip_line_comment();
};

} // namespace lumen_script
