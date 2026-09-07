#pragma once
#include <vector>
#include "token.hpp"
#include "diagnostic.hpp"

namespace lumen_script {

// Lexer with indentation blocks, Python style.
//
// Logical line rules:
//   • Blank lines and comment-only lines generate no Newline and do not
//     affect the indentation.
//   • Inside (), [] or {}, Newline/Indent/Dedent are suppressed: a call can
//     be spread over several lines.
//   • A line whose first token is '.' continues the previous one, to allow
//     chaining on the returned value:
//         return render("x.html")
//                  .status(201)
//
// Lexical rule of the grammar: '>' '>' NEVER merge into a shift token, so that
// List<Dict<string,int>> closes without ambiguity.
class Lexer {
public:
    Lexer(const SourceFile& file, DiagnosticBag& diags)
        : file_(file), diags_(diags) {}

    // Tokenizes the whole file.  It always ends in EndOfFile, with the pending
    // Dedents emitted before it.  Errors go to the DiagnosticBag; the returned
    // vector stays usable so the parser can advance and report more.
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
