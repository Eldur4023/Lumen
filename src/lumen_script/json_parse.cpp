#include <lumen_script/value.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace lumen_script {

// ─── JSON parsing ────────────────────────────────────────────────────────────
//
// A small, strict recursive descent that produces a Value directly, with no
// intermediate tree.  It accepts no comments, no trailing commas, no NaN and
// no garbage after the document: only JSON.
//
// What comes in here arrives from the network, so there is a nesting cap and
// nothing is taken on trust.

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    bool documento(Value& out) {
        espacios();
        if (!value(out, 0)) return false;
        espacios();
        return i_ == t_.size();
    }

private:
    // Without a cap, a document with thousands of nested brackets takes the
    // thread stack down before anyone can complain.
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

    bool value(Value& out, int prof) {
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
                if (!string_value(s)) return false;
                out = Value::str(std::move(s));
                return true;
            }
            case '[': return list_(out, prof);
            case '{': return object(out, prof);
            default:  return number(out);
        }
    }

    bool list_(Value& out, int prof) {
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
            if (!value(v, prof + 1)) return false;
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

    bool object(Value& out, int prof) {
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
            if (!string_value(k)) return false;
            espacios();
            if (i_ >= t_.size() || t_[i_] != ':') return false;
            ++i_;
            espacios();
            Value v;
            if (!value(v, prof + 1)) return false;
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

    // A code point to UTF-8.
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

    bool string_value(std::string& out) {
        ++i_;                                   // opening quote

        // Fast path: what runs up to the first backslash or quote is copied in
        // one go.  The vast majority of strings end here.
        const size_t start = i_;
        while (i_ < t_.size()) {
            const char c = t_[i_];
            if (c == '"' || c == '\\') break;
            if (static_cast<unsigned char>(c) < 0x20) return false;  // unescaped control character
            ++i_;
        }
        out.append(t_.data() + start, i_ - start);
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
                    // A high surrogate has to be followed by its low one; a
                    // stray one represents nothing and is rejected.
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

    bool number(Value& out) {
        const size_t start = i_;
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

        const char* p = t_.data() + start;
        const char* f = t_.data() + i_;
        if (!real) {
            long long n = 0;
            const auto r = std::from_chars(p, f, n);
            if (r.ec == std::errc{} && r.ptr == f) {
                out = Value::integer(n);
                return true;
            }
            // Out of long long range: falls to double, as everyone does.
        }
        double d = 0;
        const auto r = std::from_chars(p, f, d);
        if (r.ec != std::errc{} || r.ptr != f) return false;
        out = Value::real(d);
        return true;
    }
};

} // namespace

bool Value::parse_json(std::string_view text, Value& out) {
    Parser p(text);
    return p.documento(out);
}

} // namespace lumen_script
