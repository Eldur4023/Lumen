#include <charconv>
#include <cstdint>
#include <cstdio>
#include <lumen_script/value.hpp>
#include <lumen_script/bytecode.hpp>

#include <cmath>
#include <sstream>

namespace lumen_script {

const char* Value::type_name() const {
    switch (type_) {
        case Type::Null:  return "null";
        case Type::Bool:  return "bool";
        case Type::Int:   return "int";
        case Type::Float: return "float";
        case Type::Str:   return "string";
        case Type::List:  return "List";
        case Type::Dict:  return "Dict";
    }
    return "?";
}

std::string Value::to_string() const {
    switch (type_) {
        case Type::Null:  return "null";
        case Type::Bool:  return b_ ? "true" : "false";
        case Type::Int:   return std::to_string(i_);
        case Type::Float: {
            // Sin ceros de relleno: 2.5 y no 2.500000.
            std::ostringstream ss;
            ss << d_;
            return ss.str();
        }
        case Type::Str:   return as_str();
        case Type::List:
        case Type::Dict:  return to_json_text();
    }
    return {};
}

// ─── Serializacion directa a texto ───────────────────────────────────────────

namespace {

// Cierto si alguno de los ocho bytes necesita escape.  Las tres pruebas son
// SWAR: en vez de mirar byte a byte, se opera sobre la palabra entera.
inline bool bloque_sucio(uint64_t w) {
    constexpr uint64_t UNOS  = 0x0101010101010101ULL;
    constexpr uint64_t ALTOS = 0x8080808080808080ULL;
    const uint64_t menor_20 = (w - UNOS * 0x20) & ~w & ALTOS;
    const uint64_t c1 = w ^ (UNOS * 0x22);            // comillas
    const uint64_t c2 = w ^ (UNOS * 0x5C);            // barra invertida
    return (menor_20 | ((c1 - UNOS) & ~c1 & ALTOS)
                     | ((c2 - UNOS) & ~c2 & ALTOS)) != 0;
}

// ─── UTF-8 ───────────────────────────────────────────────────────────────────
//
// JSON tiene que ser UTF-8 (RFC 8259).  Un byte suelto que no forme una
// secuencia valida deja el documento ilegible para cualquier cliente, y eso es
// peor que un error: no falla la peticion, falla quien la recibe, y el fallo
// aparece lejos del origen.
//
// Llega de fuera por tres vias comprobadas —cuerpo JSON, query y cabeceras— y
// tambien de una base de datos que no valide la codificacion, como sqlite.  Por
// eso se ataja aqui, en la salida, y no en cada puerta de entrada: es un sitio
// en vez de cinco, y cubre tambien lo que ya estaba guardado.
//
// La validacion va en su propia pasada para NO tocar el bucle de escapado, que
// es lo mas caliente del sistema.  Tiene el mismo atajo: mientras los ocho
// bytes sean ASCII se salta el bloque entero, asi que el texto normal no paga
// casi nada.

// Longitud de la secuencia que empieza en `i`, o 0 si no es valida.  Rechaza lo
// mismo que el estandar: continuaciones sueltas, sobrelargos, subrogados y todo
// lo que pase de U+10FFFF.
inline size_t largo_utf8(const unsigned char* p, size_t n, size_t i) {
    const unsigned char c = p[i];
    auto cont = [&](size_t k) { return i + k < n && (p[i + k] & 0xC0) == 0x80; };

    if (c < 0x80) return 1;
    if (c < 0xC2) return 0;                       // continuacion suelta o sobrelargo
    if (c < 0xE0) return cont(1) ? 2 : 0;
    if (c < 0xF0) {
        if (!cont(1) || !cont(2)) return 0;
        if (c == 0xE0 && p[i + 1] < 0xA0) return 0;               // sobrelargo
        if (c == 0xED && p[i + 1] >= 0xA0) return 0;              // subrogado
        return 3;
    }
    if (c < 0xF5) {
        if (!cont(1) || !cont(2) || !cont(3)) return 0;
        if (c == 0xF0 && p[i + 1] < 0x90) return 0;               // sobrelargo
        if (c == 0xF4 && p[i + 1] >= 0x90) return 0;              // > U+10FFFF
        return 4;
    }
    return 0;
}

bool utf8_valido(const char* p, size_t n) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    size_t i = 0;
    while (i < n) {
        // Mientras los ocho bytes sean ASCII no hay nada que validar.
        while (i + 8 <= n) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            if (w & 0x8080808080808080ULL) break;
            i += 8;
        }
        if (i >= n) break;
        if (u[i] < 0x80) { ++i; continue; }
        const size_t l = largo_utf8(u, n, i);
        if (l == 0) return false;
        i += l;
    }
    return true;
}

// Copia sustituyendo por U+FFFD cada byte que rompe la codificacion, que es lo
// que hace tambien el encoding/json de Go.  Solo se recorre si la validacion ya
// dijo que hay algo malo, asi que este camino no lo pisa el texto normal.
void sanear_utf8(const std::string& in, std::string& out) {
    const auto* u = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    out.clear();
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const size_t l = largo_utf8(u, n, i);
        if (l == 0) {
            out += "\xEF\xBF\xBD";                // U+FFFD, un byte a un rombo
            ++i;
        } else {
            out.append(in, i, l);
            i += l;
        }
    }
}

void escapar_valido(const std::string& in, std::string& out) {
    // El texto real —el cuerpo de un articulo, un resumen— no lleva casi nada
    // que escapar, asi que se avanza de ocho en ocho bytes mientras el bloque
    // este limpio y solo se baja a mirar byte a byte cuando hay algo.  Con las
    // respuestas grandes esta funcion era el 24% del perfil.
    out.push_back('"');
    const char*  p = in.data();
    const size_t n = in.size();
    size_t i = 0, limpio = 0;

    while (i < n) {
        while (i + 8 <= n) {
            uint64_t w;
            std::memcpy(&w, p + i, 8);
            if (bloque_sucio(w)) break;
            i += 8;
        }
        // O quedan menos de ocho bytes, o el bloque de aqui trae algo: en
        // cualquier caso esto recorre ocho como mucho.
        unsigned char c = 0;
        bool hay = false;
        for (; i < n; ++i) {
            c = static_cast<unsigned char>(p[i]);
            if (c < 0x20 || c == '"' || c == '\\') { hay = true; break; }
        }
        if (!hay) break;

        out.append(in, limpio, i - limpio);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case 0x08: out += "\\b"; break;
            case 0x0C: out += "\\f"; break;
            case 0x0A: out += "\\n"; break;
            case 0x0D: out += "\\r"; break;
            case 0x09: out += "\\t"; break;
            default: {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
        }
        limpio = ++i;
    }
    out.append(in, limpio, n - limpio);
    out.push_back('"');
}

void escapar(const std::string& in, std::string& out) {
    if (utf8_valido(in.data(), in.size())) { escapar_valido(in, out); return; }
    std::string limpio;
    sanear_utf8(in, limpio);
    escapar_valido(limpio, out);
}

void escribir_double(double d, std::string& out) {
    // JSON no sabe escribir NaN ni infinito: no son tokens del formato.  Se
    // escriben como null, que es lo que hace JSON.stringify y lo unico que
    // todos los clientes saben leer.
    //
    // No es teorico: postgres admite 'NaN' e 'Infinity' en un double precision,
    // y sin esto una fila con uno de esos valores producia un documento que
    // ningun parser acepta —`{"d":inf}`— en vez de un error visible.
    if (!std::isfinite(d)) { out += "null"; return; }

    char buf[32];
    auto r = std::to_chars(buf, buf + sizeof(buf), d);
    if (r.ec != std::errc{}) { out += "0"; return; }
    std::string t(buf, r.ptr);
    // Un double entero sale como "3"; JSON lo leeria como entero, asi que se le
    // pone el ".0" igual que hacia nlohmann.
    if (t.find_first_of(".eE") == std::string::npos) t += ".0";
    out += t;
}

} // namespace

void Value::write_json(std::string& out) const {
    switch (type_) {
        case Type::Null:  out += "null";                   return;
        case Type::Bool:  out += b_ ? "true" : "false";    return;
        case Type::Int:   out += std::to_string(i_);       return;
        case Type::Float: escribir_double(d_, out);        return;
        case Type::Str:   escapar(as_str(), out);          return;
        case Type::List: {
            out.push_back('[');
            bool primero = true;
            for (const auto& v : as_list()) {
                if (!primero) out.push_back(',');
                primero = false;
                v.write_json(out);
            }
            out.push_back(']');
            return;
        }
        case Type::Dict: {
            // Las claves que empiezan por "__" son internas y no salen nunca.
            out.push_back('{');
            bool primero = true;
            for (const auto& [k, v] : as_dict()) {
                if (k.rfind("__", 0) == 0) continue;
                if (!primero) out.push_back(',');
                primero = false;
                escapar(k, out);
                out.push_back(':');
                v.write_json(out);
            }
            out.push_back('}');
            return;
        }
    }
    out += "null";
}


bool Value::equals(const Value& o) const {
    // int y float se comparan por valor numerico; el resto exige mismo tipo.
    if (is_num() && o.is_num()) {
        if (is_int() && o.is_int()) return i_ == o.i_;
        return as_float() == o.as_float();
    }
    if (type_ != o.type_) return false;

    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return b_ == o.b_;
        case Type::Str:  return as_str() == o.as_str();
        case Type::List: {
            if (as_list().size() != o.as_list().size()) return false;
            for (size_t i = 0; i < as_list().size(); ++i)
                if (!as_list()[i].equals(o.as_list()[i])) return false;
            return true;
        }
        case Type::Dict: {
            if (as_dict().size() != o.as_dict().size()) return false;
            for (const auto& [k, v] : as_dict()) {
                auto it = o.as_dict().find(k);
                if (it == o.as_dict().end() || !v.equals(it->second)) return false;
            }
            return true;
        }
        default: return false;
    }
}

const char* op_name(Op op) {
    switch (op) {
        case Op::Const:            return "CONST";
        case Op::LoadLocal:        return "LOAD_LOCAL";
        case Op::StoreLocal:       return "STORE_LOCAL";
        case Op::Pop:              return "POP";
        case Op::Add:              return "ADD";
        case Op::Sub:              return "SUB";
        case Op::Mul:              return "MUL";
        case Op::Div:              return "DIV";
        case Op::Mod:              return "MOD";
        case Op::Neg:              return "NEG";
        case Op::Eq:               return "EQ";
        case Op::Ne:               return "NE";
        case Op::Lt:               return "LT";
        case Op::Le:               return "LE";
        case Op::Gt:               return "GT";
        case Op::Ge:               return "GE";
        case Op::Not:              return "NOT";
        case Op::AddInt:           return "ADD_INT";
        case Op::SubInt:           return "SUB_INT";
        case Op::MulInt:           return "MUL_INT";
        case Op::LtInt:            return "LT_INT";
        case Op::LeInt:            return "LE_INT";
        case Op::GtInt:            return "GT_INT";
        case Op::GeInt:            return "GE_INT";
        case Op::Jump:             return "JUMP";
        case Op::JumpIfFalse:      return "JUMP_IF_FALSE";
        case Op::JumpIfFalsePeek:  return "JUMP_IF_FALSE_PEEK";
        case Op::JumpIfTruePeek:   return "JUMP_IF_TRUE_PEEK";
        case Op::MakeList:         return "MAKE_LIST";
        case Op::MakeDict:         return "MAKE_DICT";
        case Op::GetIndex:         return "GET_INDEX";
        case Op::SetIndex:         return "SET_INDEX";
        case Op::IterList:         return "ITER_LIST";
        case Op::GetMember:        return "GET_MEMBER";
        case Op::SetMember:        return "SET_MEMBER";
        case Op::CallFunction:     return "CALL_FUNCTION";
        case Op::CallMethod:       return "CALL_METHOD";
        case Op::CallNative:       return "CALL_NATIVE";
        case Op::CallAsync:        return "CALL_ASYNC";
        case Op::Return:           return "RETURN";
        case Op::ReturnNull:       return "RETURN_NULL";
    }
    return "?";
}

} // namespace lumen_script
