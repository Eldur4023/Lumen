#pragma once
#include <string>
#include <string_view>

namespace lumen_script {

// Posicion en el fuente.  `file` apunta a una cadena que vive en el SourceFile,
// asi que un Token es barato de copiar.
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

    // Metodos de ruta
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
    std::string text;    // lexema; para String ya viene con los escapes resueltos
    SourceLoc   loc;

    bool is(Tok t) const { return kind == t; }
};

// Devuelve el Tok de palabra reservada para `s`, o Tok::Ident si no lo es.
Tok keyword_or_ident(std::string_view s);

} // namespace lumen_script
