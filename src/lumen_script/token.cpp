#include <lumen_script/token.hpp>
#include <unordered_map>

namespace lumen_script {

const char* tok_name(Tok t) {
    switch (t) {
        case Tok::EndOfFile: return "fin de file";
        case Tok::Newline:   return "fin de line";
        case Tok::Indent:    return "indentacion";
        case Tok::Dedent:    return "des-indentacion";
        case Tok::Ident:     return "identificador";
        case Tok::Int:       return "entero";
        case Tok::Float:     return "decimal";
        case Tok::String:    return "string_value";

        case Tok::KwImport:   return "import";
        case Tok::KwClass:    return "class";
        case Tok::KwFn:       return "fn";
        case Tok::KwApp:      return "app";
        case Tok::KwGroup:    return "group";
        case Tok::KwEndpoint: return "endpoint";
        case Tok::KwOn:       return "on";
        case Tok::KwError:    return "error";
        case Tok::KwOrigins:  return "origins";
        case Tok::KwValidate: return "validate";
        case Tok::KwStatic:   return "static";
        case Tok::KwSpa:      return "spa";

        case Tok::KwGet:    return "get";
        case Tok::KwPost:   return "post";
        case Tok::KwPut:    return "put";
        case Tok::KwPatch:  return "patch";
        case Tok::KwDelete: return "delete";
        case Tok::KwAny:    return "any";
        case Tok::KwSse:    return "sse";
        case Tok::KwWs:     return "ws";

        case Tok::KwIf:       return "if";
        case Tok::KwElse:     return "else";
        case Tok::KwElif:     return "elif";
        case Tok::KwWhile:    return "while";
        case Tok::KwFor:      return "for";
        case Tok::KwIn:       return "in";
        case Tok::KwReturn:   return "return";
        case Tok::KwRequire:  return "require";
        case Tok::KwTry:      return "try";
        case Tok::KwCatch:    return "catch";
        case Tok::KwBreak:    return "break";
        case Tok::KwContinue: return "continue";

        case Tok::KwAnd:   return "and";
        case Tok::KwOr:    return "or";
        case Tok::KwNot:   return "not";
        case Tok::KwTrue:  return "true";
        case Tok::KwFalse: return "false";
        case Tok::KwNull:  return "null";
        case Tok::KwThis:  return "this";
        case Tok::KwAwait: return "await";
        case Tok::KwVoid:  return "void";

        case Tok::LParen:   return "(";
        case Tok::RParen:   return ")";
        case Tok::LBracket: return "[";
        case Tok::RBracket: return "]";
        case Tok::LBrace:   return "{";
        case Tok::RBrace:   return "}";
        case Tok::Comma:    return ",";
        case Tok::Colon:    return ":";
        case Tok::Dot:      return ".";
        case Tok::Question: return "?";
        case Tok::Arrow:    return "->";
        case Tok::Assign:   return "=";
        case Tok::Eq:       return "==";
        case Tok::NotEq:    return "!=";
        case Tok::Lt:       return "<";
        case Tok::LtEq:     return "<=";
        case Tok::Gt:       return ">";
        case Tok::GtEq:     return ">=";
        case Tok::Plus:     return "+";
        case Tok::Minus:    return "-";
        case Tok::Star:     return "*";
        case Tok::Slash:    return "/";
        case Tok::Percent:  return "%";
        case Tok::PlusPlus:   return "++";
        case Tok::MinusMinus: return "--";
        case Tok::PlusEq:     return "+=";
        case Tok::MinusEq:    return "-=";
        case Tok::StarEq:     return "*=";
        case Tok::SlashEq:    return "/=";
        case Tok::PercentEq:  return "%=";
    }
    return "?";
}

Tok keyword_or_ident(std::string_view s) {
    static const std::unordered_map<std::string_view, Tok> kw = {
        {"import", Tok::KwImport}, {"class", Tok::KwClass}, {"fn", Tok::KwFn},
        {"app", Tok::KwApp}, {"group", Tok::KwGroup}, {"endpoint", Tok::KwEndpoint},
        {"on", Tok::KwOn}, {"error", Tok::KwError}, {"origins", Tok::KwOrigins},
        {"validate", Tok::KwValidate}, {"static", Tok::KwStatic}, {"spa", Tok::KwSpa},

        {"get", Tok::KwGet}, {"post", Tok::KwPost}, {"put", Tok::KwPut},
        {"patch", Tok::KwPatch}, {"delete", Tok::KwDelete}, {"any", Tok::KwAny},
        {"sse", Tok::KwSse}, {"ws", Tok::KwWs},

        {"if", Tok::KwIf}, {"else", Tok::KwElse}, {"elif", Tok::KwElif}, {"while", Tok::KwWhile},
        {"for", Tok::KwFor}, {"in", Tok::KwIn}, {"return", Tok::KwReturn},
        {"require", Tok::KwRequire}, {"try", Tok::KwTry}, {"catch", Tok::KwCatch},
        {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},

        {"and", Tok::KwAnd}, {"or", Tok::KwOr}, {"not", Tok::KwNot},
        {"true", Tok::KwTrue}, {"false", Tok::KwFalse}, {"null", Tok::KwNull},
        {"this", Tok::KwThis}, {"await", Tok::KwAwait}, {"void", Tok::KwVoid},
    };
    auto it = kw.find(s);
    return it == kw.end() ? Tok::Ident : it->second;
}

} // namespace lumen_script
