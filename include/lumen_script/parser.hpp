#pragma once
#include <vector>
#include "ast.hpp"
#include "diagnostic.hpp"
#include "token.hpp"

namespace lumen_script {

// Parser descendente recursivo sobre el flujo de tokens del lexer.
//
// Ante un error no aborta: lo registra y sincroniza hasta el siguiente inicio
// de declaracion, para poder reportar varios fallos en una sola pasada.
class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticBag& diags)
        : toks_(std::move(tokens)), diags_(diags) {}

    // Parsea en `out`.  Varias llamadas con distintos ficheros acumulan sobre
    // el mismo Program: es lo que permite repartir la app en varios .lum.
    void parse_into(Program& out);

    // Parsea UNA expresion y nada mas.  Lo usan las plantillas: lo que va
    // dentro de {{ }} o de {% if %} es una expresion de Lumen Script, no un lenguaje
    // aparte.  Devuelve nulo si no hay expresion o si sobra algo detras.
    ExprPtr parse_single_expression();

private:
    std::vector<Token> toks_;
    DiagnosticBag&     diags_;
    size_t             i_ = 0;

    // ── Navegacion ───────────────────────────────────────────────────────────
    const Token& peek(size_t ahead = 0) const;
    const Token& prev() const;
    bool  check(Tok k) const { return peek().is(k); }
    bool  match(Tok k);
    const Token& advance();
    bool  expect(Tok k, const char* context);
    void  error_here(std::string msg);
    void  synchronize();
    void  skip_newlines();

    // ── Declaraciones ────────────────────────────────────────────────────────
    void parse_declaration(Program& out);
    void parse_route(Program& out, const Token& method_tok,
                     const std::string& prefix, const std::vector<Guard>& guards);
    void parse_group(Program& out, const std::string& prefix,
                     const std::vector<Guard>& guards);
    void parse_app(Program& out);
    // Resuelve un valor de configuracion: cadena, numero, booleano o env("VAR").
    bool config_value(std::string& text, long long& number, bool& flag, int& kind);
    void parse_class(Program& out);
    void parse_error(Program& out);
    void parse_fn(Program& out);

    // ── Sentencias ───────────────────────────────────────────────────────────
    Block   parse_block();
    StmtPtr parse_statement();
    StmtPtr parse_if();
    StmtPtr parse_if_from_elif();
    StmtPtr parse_while();
    StmtPtr parse_for();
    StmtPtr parse_return();
    static bool assignable(const Expr& e);
    ExprPtr clone_target(const Expr& e);
    StmtPtr parse_require();
    StmtPtr parse_try();

    // ── Tipos y parametros ───────────────────────────────────────────────────
    bool    looks_like_type() const;
    TypeRef parse_type();
    Param   parse_param();

    // ── Expresiones, de menor a mayor precedencia ────────────────────────────
    ExprPtr parse_expr();
    ExprPtr parse_ternary();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_equality();
    ExprPtr parse_comparison();
    ExprPtr parse_sum();
    ExprPtr parse_product();
    ExprPtr parse_unary();
    ExprPtr parse_postfix();
    ExprPtr parse_primary();

    ExprPtr make(ExprKind k, SourceLoc loc);
};

} // namespace lumen_script
