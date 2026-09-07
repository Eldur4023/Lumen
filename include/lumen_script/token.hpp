#pragma once
#include <string>
#include <string_view>

namespace lumen_script {

// Position in the source.  `file` points at a string that lives in the
// SourceFile, so a Token is cheap to copy.
struct SourceLoc {
    const std::string* file = nullptr;
    int line = 0;   // 1-indexado
    int col  = 0;   // 1-indexado, en bytes
};

enum class Tok {
    // Estructura
    EndOfFile, Newline, Indent, Dedent,

    // Literales e identificadores
    Ident, Int, Float, String,

    // Declaraciones
    KwImport, KwClass, KwFn, KwApp, KwGroup, KwEndpoint, KwOn, KwError,
    KwOrigins, KwValidate, KwStatic, KwSpa,

    // Metodos de path
    KwGet, KwPost, KwPut, KwPatch, KwDelete, KwAny, KwSse, KwWs,

    // Sentencias
    KwIf, KwElse, KwElif, KwWhile, KwFor, KwIn, KwReturn, KwRequire,
    KwTry, KwCatch, KwBreak, KwContinue,

    // Expresiones y tipos
    KwAnd, KwOr, KwNot, KwTrue, KwFalse, KwNull, KwThis, KwAwait, KwVoid,

    // Puntuacion
    LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Colon, Dot, Question, Arrow, Assign,
    Eq, NotEq, Lt, LtEq, Gt, GtEq,
    Plus, Minus, Star, Slash, Percent,
    PlusPlus, MinusMinus,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
};

const char* tok_name(Tok t);

struct Token {
    Tok         kind = Tok::EndOfFile;
    std::string text;    // lexeme; for String the escapes are already resolved
    SourceLoc   loc;

    bool is(Tok t) const { return kind == t; }
};

// Returns the reserved-word Tok for `s`, or Tok::Ident if it is not one.
Tok keyword_or_ident(std::string_view s);

} // namespace lumen_script
