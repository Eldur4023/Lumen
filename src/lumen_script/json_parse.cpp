#include <lumen_script/value.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace lumen_script {

// ─── Parseo de JSON ──────────────────────────────────────────────────────────
//
// Descenso recursivo pequeno y estricto que produce un Value directamente, sin
// arbol intermedio.  No admite comentarios, ni comas colgando, ni NaN, ni
// basura detras del documento: solo JSON.
//
// Lo que entra por aqui viene de la red, asi que hay tope de anidamiento y no
// se da nada por bueno.

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    bool documento(Value& out) {
        espacios();
        if (!valor(out, 0)) return false;
        espacios();
        return i_ == t_.size();
    }

private:
    // Sin tope, un documento con miles de corchetes anidados se lleva por
    // delante la pila del hilo antes de que nadie pueda quejarse.
    static constexpr int kMaxProfundidad = 200;

    std::string_view t_;
    size_t           i_ = 0;

    static bool digito(char c) { return c >= '0' && c <= '9'; }

    void espacios() {
        while (i_ < t_.size()) {
            const char c = t_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }

    bool literal(std::string_view lit) {
        if (t_.size() - i_ < lit.size()) return false;
        if (t_.compare(i_, lit.size(), lit) != 0) return false;
        i_ += lit.size();
        return true;
    }

    bool valor(Value& out, int prof) {
        if (prof > kMaxProfundidad || i_ >= t_.size()) return false;
        switch (t_[i_]) {
            case 'n':
                if (!literal("null")) return false;
                out = Value::null();
                return true;
            case 't':
                if (!literal("true")) return false;
                out = Value::boolean(true);
                return true;
            case 'f':
                if (!literal("false")) return false;
                out = Value::boolean(false);
                return true;
            case '"': {
                std::string s;
                if (!cadena(s)) return false;
                out = Value::str(std::move(s));
                return true;
            }
            case '[': return lista(out, prof);
            case '{': return objeto(out, prof);
            default:  return numero(out);
        }
    }

    bool lista(Value& out, int prof) {
        ++i_;                                   // '['
        Value::List l;
        espacios();
        if (i_ < t_.size() && t_[i_] == ']') {
            ++i_;
            out = Value::list(std::move(l));
            return true;
        }
        for (;;) {
            espacios();
            Value v;
            if (!valor(v, prof + 1)) return false;
            l.push_back(std::move(v));
            espacios();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == ']') { ++i_; break; }
            return false;
        }
        out = Value::list(std::move(l));
        return true;
    }

    bool objeto(Value& out, int prof) {
        ++i_;                                   // '{'
        Value::Dict d;
        espacios();
        if (i_ < t_.size() && t_[i_] == '}') {
            ++i_;
            out = Value::dict(std::move(d));
            return true;
        }
        for (;;) {
            espacios();
            if (i_ >= t_.size() || t_[i_] != '"') return false;
            std::string k;
            if (!cadena(k)) return false;
            espacios();
            if (i_ >= t_.size() || t_[i_] != ':') return false;
            ++i_;
            espacios();
            Value v;
            if (!valor(v, prof + 1)) return false;
            d[k] = std::move(v);
            espacios();
            if (i_ >= t_.size()) return false;
            if (t_[i_] == ',') { ++i_; continue; }
            if (t_[i_] == '}') { ++i_; break; }
            return false;
        }
        out = Value::dict(std::move(d));
        return true;
    }

    // Un punto de codigo a UTF-8.
    static void utf8(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool hex4(uint32_t& cp) {
        if (i_ + 4 > t_.size()) return false;
        cp = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t_[i_ + k];
            cp <<= 4;
            if      (c >= '0' && c <= '9') cp |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') cp |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        i_ += 4;
        return true;
    }

    bool cadena(std::string& out) {
        ++i_;                                   // comilla de apertura

        // Camino rapido: lo que va hasta la primera barra o comilla se copia de
        // una vez.  La inmensa mayoria de las cadenas acaban aqui.
        const size_t inicio = i_;
        while (i_ < t_.size()) {
            const char c = t_[i_];
            if (c == '"' || c == '\\') break;
            if (static_cast<unsigned char>(c) < 0x20) return false;  // control sin escapar
            ++i_;
        }
        out.append(t_.data() + inicio, i_ - inicio);
        if (i_ >= t_.size()) return false;
        if (t_[i_] == '"') { ++i_; return true; }

        for (;;) {
            if (i_ >= t_.size()) return false;
            const char c = t_[i_];
            if (c == '"') { ++i_; return true; }
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out.push_back(c); ++i_; continue; }

            ++i_;
            if (i_ >= t_.size()) return false;
            const char e = t_[i_++];
            switch (e) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!hex4(cp)) return false;
                    // Un subrogado alto tiene que venir seguido de su bajo; uno
                    // suelto no representa nada y se rechaza.
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i_ + 1 >= t_.size() || t_[i_] != '\\' || t_[i_ + 1] != 'u')
                            return false;
                        i_ += 2;
                        uint32_t bajo = 0;
                        if (!hex4(bajo)) return false;
                        if (bajo < 0xDC00 || bajo > 0xDFFF) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (bajo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return false;
                    }
                    utf8(cp, out);
                    break;
                }
                default: return false;
            }
        }
    }

    bool numero(Value& out) {
        const size_t inicio = i_;
        if (i_ < t_.size() && t_[i_] == '-') ++i_;
        if (i_ >= t_.size()) return false;

        if (t_[i_] == '0') {
            ++i_;
        } else if (t_[i_] >= '1' && t_[i_] <= '9') {
            while (i_ < t_.size() && digito(t_[i_])) ++i_;
        } else {
            return false;
        }

        bool real = false;
        if (i_ < t_.size() && t_[i_] == '.') {
            real = true;
            ++i_;
            if (i_ >= t_.size() || !digito(t_[i_])) return false;
            while (i_ < t_.size() && digito(t_[i_])) ++i_;
        }
        if (i_ < t_.size() && (t_[i_] == 'e' || t_[i_] == 'E')) {
            real = true;
            ++i_;
            if (i_ < t_.size() && (t_[i_] == '+' || t_[i_] == '-')) ++i_;
            if (i_ >= t_.size() || !digito(t_[i_])) return false;
            while (i_ < t_.size() && digito(t_[i_])) ++i_;
        }

        const char* p = t_.data() + inicio;
        const char* f = t_.data() + i_;
        if (!real) {
            long long n = 0;
            const auto r = std::from_chars(p, f, n);
            if (r.ec == std::errc{} && r.ptr == f) {
                out = Value::integer(n);
                return true;
            }
            // Se sale del rango de long long: cae a double, como hace todo el mundo.
        }
        double d = 0;
        const auto r = std::from_chars(p, f, d);
        if (r.ec != std::errc{} || r.ptr != f) return false;
        out = Value::real(d);
        return true;
    }
};

} // namespace

bool Value::parse_json(std::string_view texto, Value& out) {
    Parser p(texto);
    return p.documento(out);
}

} // namespace lumen_script
