#include <lumen_script/template.hpp>

#include <lumen_script/emitter.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/vm.hpp>

#include <map>
#include <memory>
#include <filesystem>
#include <fstream>

namespace lumen_script {

void escapar_html(const std::string& in, std::string& out) {
    // What must not be touched is copied in one go, as in the JSON escaping:
    // the text of a page almost never carries metacharacters.
    size_t limpio = 0;
    for (size_t i = 0; i < in.size(); ++i) {
        const char* rep = nullptr;
        switch (in[i]) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: continue;
        }
        out.append(in, limpio, i - limpio);
        out += rep;
        limpio = i + 1;
    }
    out.append(in, limpio, in.size() - limpio);
}

namespace {

// Trims spaces on the left/right according to the {%- and -%} markers.
void recortar_izq(std::string& t) {
    size_t n = t.size();
    while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t' ||
                     t[n - 1] == '\n' || t[n - 1] == '\r')) --n;
    t.resize(n);
}
void recortar_der(size_t& i, const std::string& src) {
    while (i < src.size() && (src[i] == ' ' || src[i] == '\t' ||
                              src[i] == '\n' || src[i] == '\r')) ++i;
}

std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}


// ─── Compiler ────────────────────────────────────────────────────────────────

class Compilador {
public:
    Compilador(const std::string& dir, DiagnosticBag& diags, Template& out)
        : dir_(dir), diags_(diags), out_(out) {}

    bool compilar(const std::string& fuente, const std::string& file,
                  const std::vector<TypedName>& data) {
        out_.names = data;

        // Inheritance: it climbs the {% extends %} chain collecting blocks.  The
        // child wins, so only the block that was not there yet is recorded.  At
        // the end the ROOT is compiled, with the blocks substituted.
        std::string raiz = fuente, raiz_fichero = file;
        for (int n = 0; n <= 16; ++n) {
            std::string parent;
            if (!padre_de(raiz, raiz_fichero, parent)) break;
            if (n == 16) {
                error(loc_inicio(raiz_fichero), raiz_fichero,
                      "too many chained {% extends %}: is there a cycle?");
                return false;
            }
            recoger_bloques(raiz, raiz_fichero);
            std::string parent_text;
            if (!read_template(parent, parent_text)) {
                error(loc_inicio(raiz_fichero), raiz_fichero,
                      "base template not found: '" + parent + "'");
                return false;
            }
            raiz         = std::move(parent_text);
            raiz_fichero = parent;
        }

        cuerpo(raiz, raiz_fichero, 0);
        if (!open_.empty()) {
            error(open_.back().loc, file,
                  "missing {% end" + open_.back().kind + " %}");
        }
        return !fallo_;
    }

private:
    struct Open {
        std::string          kind;      // "if" or "for"
        SourceLoc            loc;
        std::vector<size_t>  patches;  // jumps to the end of the block
        size_t               start = 0;
        // Names this loop shadows, to give them back when it closes.
        std::vector<std::pair<size_t, std::string>> tapados;
    };

    // Shadows an already visible name.  Slots cannot be moved —every compiled
    // chunk indexes by position— so the outer one is renamed to something that
    // is not a valid identifier: it stays unreachable for as long as the loop
    // lasts, which is exactly what shadowing means.
    void tapar(const std::string& name, std::vector<std::pair<size_t, std::string>>& tapados) {
        for (size_t i = out_.names.size(); i-- > 0;) {
            if (out_.names[i].name == name) {
                tapados.emplace_back(i, out_.names[i].name);
                // The marker carries the slot so two shadowed names cannot clash
                // with each other: the emitter does not allow that either.
                out_.names[i].name = " tapado" + std::to_string(i);
                break;
            }
        }
    }

    const std::string&   dir_;
    DiagnosticBag&       diags_;
    Template&           out_;
    bool                 fallo_ = false;
    std::vector<Open> open_;
    std::vector<std::string> file_stack_;   // to detect include cycles

    // The files are kept alive because SourceLoc points at their path.
    std::vector<std::unique_ptr<SourceFile>> files_;

    void error(SourceLoc loc, const std::string& file, std::string msg) {
        fallo_ = true;
        (void)file;
        diags_.error(loc, std::move(msg));
    }

    const std::string* guardar_nombre(const std::string& f) {
        for (const auto& sf : files_)
            if (sf->path == f && sf->text.empty()) return &sf->path;
        files_.push_back(std::make_unique<SourceFile>(SourceFile{f, {}}));
        return &files_.back()->path;
    }

    size_t text(std::string t) {
        if (t.empty()) return SIZE_MAX;
        out_.texts.push_back(std::move(t));
        out_.code.push_back({Template::Op::Text,
                             static_cast<uint32_t>(out_.texts.size() - 1), 0, 0, 0, {}});
        return out_.code.size() - 1;
    }

    // Compiles a Lumen Script expression with the names visible right now.
    // Returns its index in out_.exprs, or SIZE_MAX if it does not compile.
    size_t expresion(const std::string& expr_text, SourceLoc loc,
                     const std::string& file) {
        // The fragment is lexed as if it were a file of its own.  Its errors
        // carry the position INSIDE the fragment, which is no use to anyone: the
        // template location is reported and the reason attached.
        files_.push_back(std::make_unique<SourceFile>(
            SourceFile{file + " (expresion)", expr_text}));
        SourceFile&   sf = *files_.back();
        DiagnosticBag own_diags;

        Lexer   lx(sf, own_diags);
        Parser  ps(lx.tokenize(), own_diags);
        ExprPtr e = ps.parse_single_expression();
        if (!e || !own_diags.empty()) {
            error(loc, file, "in the template expression: " +
                  (own_diags.empty() ? std::string("invalid expression")
                                   : own_diags.items().front().message));
            return SIZE_MAX;
        }

        Chunk   ch;
        Emitter em(own_diags, fns_, classes_, imports_);
        if (!em.emit_condition(*e, out_.names, ch) || !own_diags.empty()) {
            error(loc, file, "in the template expression: " +
                  (own_diags.empty() ? std::string("cannot be compiled")
                                   : own_diags.items().front().message));
            return SIZE_MAX;
        }
        out_.exprs.push_back(std::move(ch));
        return out_.exprs.size() - 1;
    }

    void cuerpo(const std::string& src, const std::string& file, int depth);

    // Blocks the child (or the grandchild) defines, by name.  They also keep
    // which file they came from, so the error points at the right place.
    std::map<std::string, std::pair<std::string, std::string>> bloques_;

    SourceLoc loc_inicio(const std::string& file) {
        SourceLoc l;
        l.file = guardar_nombre(file);
        l.line = 1;
        l.col  = 1;
        return l;
    }

    bool read_template(const std::string& name, std::string& out) {
        if (name.empty() || name.find("..") != std::string::npos) return false;
        std::ifstream f(std::filesystem::path(dir_) / name, std::ios::binary);
        if (!f) return false;
        out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return true;
    }

    // If the template starts with {% extends "x" %}, returns x.  It has to be
    // first: a template that inherits contributes no text of its own, only blocks.
    bool padre_de(const std::string& src, const std::string& file,
                  std::string& parent) {
        size_t i = src.find_first_not_of(" \t\r\n");
        if (i == std::string::npos || src.compare(i, 2, "{%") != 0) return false;
        size_t fin = src.find("%}", i);
        if (fin == std::string::npos) return false;
        std::string cont = trim(src.substr(i + 2, fin - i - 2));
        if (cont.rfind("extends", 0) != 0) return false;
        parent = trim(cont.substr(7));
        if (parent.size() >= 2 && (parent.front() == 0x22 || parent.front() == 0x27))
            parent = parent.substr(1, parent.size() - 2);
        if (parent.empty()) {
            error(loc_inicio(file), file, "{% extends %} without a template name");
            return false;
        }
        return true;
    }

    // Index right after the {% endblock %} closing the one that starts at
    // `from`, counting the nested ones.
    static size_t end_of_block(const std::string& src, size_t from_, size_t& content_end) {
        int  level = 1;
        size_t i   = from_;
        while (i < src.size()) {
            size_t abre = src.find("{%", i);
            if (abre == std::string::npos) break;
            size_t fin = src.find("%}", abre);
            if (fin == std::string::npos) break;
            std::string cont = trim(src.substr(abre + 2, fin - abre - 2));
            if (cont.rfind("block", 0) == 0)      ++level;
            else if (cont.rfind("endblock", 0) == 0) {
                if (--level == 0) { content_end = abre; return fin + 2; }
            }
            i = fin + 2;
        }
        content_end = src.size();
        return std::string::npos;
    }

    // Records the blocks of a child template.  The first one seen wins: it
    // climbs from the most derived, so the lower one shadows the upper.
    void recoger_bloques(const std::string& src, const std::string& file) {
        size_t i = 0;
        while (i < src.size()) {
            size_t abre = src.find("{%", i);
            if (abre == std::string::npos) break;
            size_t fin = src.find("%}", abre);
            if (fin == std::string::npos) break;
            std::string cont = trim(src.substr(abre + 2, fin - abre - 2));
            if (cont.rfind("block", 0) != 0) { i = fin + 2; continue; }

            std::string name = trim(cont.substr(5));
            size_t      cfin   = 0;
            size_t      tras   = end_of_block(src, fin + 2, cfin);
            if (tras == std::string::npos) {
                error(loc_inicio(file), file,
                      "missing {% endblock %} for block '" + name + "'");
                return;
            }
            if (!name.empty() && !bloques_.count(name))
                bloques_[name] = {src.substr(fin + 2, cfin - fin - 2), file};
            i = tras;
        }
    }

public:
    // Module compilation context, so the expressions can call user functions
    // and build their classes.
    const FunctionSigs*          fns_     = nullptr;
    const ClassSigs*             classes_  = nullptr;
    const std::set<std::string>* imports_ = nullptr;
};

} // namespace

// The body is defined outside so {% include %} can recurse.
void Compilador::cuerpo(const std::string& src, const std::string& file,
                        int depth) {
    size_t i = 0, text_from = 0;
    int    line = 1;

    auto loc_en = [&](size_t pos) {
        SourceLoc l;
        l.file = guardar_nombre(file);
        int ln = 1, col = 1;
        for (size_t k = 0; k < pos && k < src.size(); ++k) {
            if (src[k] == '\n') { ++ln; col = 1; } else ++col;
        }
        l.line = ln;
        l.col = col;
        return l;
    };
    (void)line;

    while (i < src.size()) {
        size_t abre = src.find('{', i);
        if (abre == std::string::npos || abre + 1 >= src.size()) break;
        const char type = src[abre + 1];
        if (type != '{' && type != '%' && type != '#') { i = abre + 1; continue; }

        const bool recorta_antes = (abre + 2 < src.size() && src[abre + 2] == '-');
        const char* cierre_str = (type == '{') ? "}}" : (type == '%') ? "%}" : "#}";
        size_t cierre = src.find(cierre_str, abre + 2);
        if (cierre == std::string::npos) {
            error(loc_en(abre), file, std::string("missing the closing '") +
                  (type == '{' ? "{{" : type == '%' ? "{%" : "{#") + "'");
            return;
        }
        const bool recorta_despues = (cierre > abre + 2 && src[cierre - 1] == '-');

        std::string previo = src.substr(text_from, abre - text_from);
        if (recorta_antes) recortar_izq(previo);
        text(std::move(previo));

        size_t ini_cont = abre + 2 + (recorta_antes ? 1 : 0);
        size_t end_cont = cierre - (recorta_despues ? 1 : 0);
        std::string content = trim(src.substr(ini_cont, end_cont - ini_cont));
        const SourceLoc loc = loc_en(abre);

        i = cierre + 2;
        if (recorta_despues) recortar_der(i, src);
        text_from = i;

        if (type == '#') continue;                       // comentario

        if (type == '{') {                               // {{ expresion }}
            bool crudo = false;
            if (content.size() > 5 &&
                content.compare(content.size() - 5, 5, "|safe") == 0) {
                crudo = true;
                content = trim(content.substr(0, content.size() - 5));
            }
            if (content.find('|') != std::string::npos) {
                error(loc, file,
                      "Jinja2 filters do not exist here: they are Lumen Script methods. "
                      "Instead of {{ x|upper }}, write {{ x.upper() }}");
                continue;
            }
            size_t k = expresion(content, loc, file);
            if (k == SIZE_MAX) continue;
            out_.code.push_back({crudo ? Template::Op::WriteRaw
                                       : Template::Op::Write,
                                 static_cast<uint32_t>(k), 0, 0, 0, loc});
            continue;
        }

        // {% tag ... %}
        std::string tag = content.substr(0, content.find_first_of(" \t"));
        std::string resto    = trim(content.substr(tag.size()));

        if (tag == "if") {
            size_t k = expresion(resto, loc, file);
            if (k == SIZE_MAX) continue;
            out_.code.push_back({Template::Op::SaltarSiFalso,
                                 static_cast<uint32_t>(k), 0, 0, 0, loc});
            Open ab{"if", loc, {}, out_.code.size() - 1};
            open_.push_back(std::move(ab));

        } else if (tag == "elif" || tag == "else") {
            if (open_.empty() || open_.back().kind != "if") {
                error(loc, file, "{% " + tag + " %} without {% if %}");
                continue;
            }
            // The previous block jumps to the end of the whole chain.
            out_.code.push_back({Template::Op::Saltar, 0, 0, 0, 0, loc});
            open_.back().patches.push_back(out_.code.size() - 1);
            // And the pending JumpIfFalse lands here.
            out_.code[open_.back().start].b =
                static_cast<uint32_t>(out_.code.size());

            if (tag == "elif") {
                size_t k = expresion(resto, loc, file);
                if (k == SIZE_MAX) continue;
                out_.code.push_back({Template::Op::SaltarSiFalso,
                                     static_cast<uint32_t>(k), 0, 0, 0, loc});
                open_.back().start = out_.code.size() - 1;
            } else {
                open_.back().start = SIZE_MAX;   // no queda condicion pendiente
            }

        } else if (tag == "endif") {
            if (open_.empty() || open_.back().kind != "if") {
                error(loc, file, "{% endif %} without {% if %}");
                continue;
            }
            Open ab = std::move(open_.back());
            open_.pop_back();
            if (ab.start != SIZE_MAX)
                out_.code[ab.start].b = static_cast<uint32_t>(out_.code.size());
            for (size_t p : ab.patches)
                out_.code[p].b = static_cast<uint32_t>(out_.code.size());

        } else if (tag == "for") {
            // for <name> in <expression>
            size_t en = resto.find(" in ");
            if (en == std::string::npos) {
                error(loc, file, "expected {% for x in list %}");
                continue;
            }
            std::string var   = trim(resto.substr(0, en));
            std::string list_ = trim(resto.substr(en + 4));
            if (var.empty()) {
                error(loc, file, "missing the loop variable name");
                continue;
            }
            size_t k = expresion(list_, loc, file);
            if (k == SIZE_MAX) continue;

            // Slots only grow, and the names this loop shadows are recorded to
            // give them back at the {% endfor %}.
            std::vector<std::pair<size_t, std::string>> tapados;
            tapar(var, tapados);
            tapar("loop", tapados);

            const uint32_t slot = static_cast<uint32_t>(out_.names.size());
            // The loop variable carries no type: it depends on what is inside
            // the list, and that is not known here.
            out_.names.push_back({var, {}});
            const uint32_t slot_loop = static_cast<uint32_t>(out_.names.size());
            out_.names.push_back({"loop", {}});

            out_.code.push_back({Template::Op::BucleInicio,
                                 static_cast<uint32_t>(k), 0, slot, slot_loop, loc});
            Open ab{"for", loc, {}, out_.code.size() - 1, std::move(tapados)};
            open_.push_back(std::move(ab));

        } else if (tag == "endfor") {
            if (open_.empty() || open_.back().kind != "for") {
                error(loc, file, "{% endfor %} without {% for %}");
                continue;
            }
            Open ab = std::move(open_.back());
            open_.pop_back();
            for (const auto& [idx, nom] : ab.tapados) out_.names[idx].name = nom;
            const auto& ini = out_.code[ab.start];
            out_.code.push_back({Template::Op::BucleSiguiente, 0,
                                 static_cast<uint32_t>(ab.start + 1),
                                 ini.slot, ini.slot_loop, loc});
            out_.code[ab.start].b = static_cast<uint32_t>(out_.code.size());

        } else if (tag == "include") {
            if (depth > 16) {
                error(loc, file, "too many nested {% include %}: is there a cycle?");
                continue;
            }
            std::string name = trim(resto);
            if (name.size() >= 2 && (name.front() == '"' || name.front() == '\''))
                name = name.substr(1, name.size() - 2);
            std::filesystem::path path = std::filesystem::path(dir_) / name;
            if (name.empty() || name.find("..") != std::string::npos) {
                error(loc, file, "invalid template name in {% include %}");
                continue;
            }
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                error(loc, file, "template not found: '" + name + "'");
                continue;
            }
            std::string incluido((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            cuerpo(incluido, name, depth + 1);

        } else if (tag == "block") {
            // At the root: if the child defined this block, ITS text is compiled
            // and the one here is skipped; if not, the one here is compiled,
            // which is the default value.
            const std::string name = trim(resto);
            size_t content_end = 0;
            size_t tras = end_of_block(src, i, content_end);
            if (tras == std::string::npos) {
                error(loc, file, "missing {% endblock %} for block '" + name + "'");
                return;
            }
            auto it = bloques_.find(name);
            if (it != bloques_.end()) {
                cuerpo(it->second.first, it->second.second, depth + 1);
                i = tras;
                text_from = i;
            }
            // With no replacement, reading carries on normally: the default
            // content is compiled as is and the {% endblock %} does nothing.

        } else if (tag == "endblock") {
            // Closes a block whose default content has just been compiled.

        } else if (tag == "extends") {
            error(loc, file, "{% extends %} must be the first thing in the template");

        } else if (tag == "macro" || tag == "endmacro") {
            error(loc, file, "{% " + tag + " %} no existe en Lumen Script: "
                                "to reuse, use {% include %} or a function from the .lum");
        } else {
            error(loc, file, "unknown tag: {% " + tag + " %}");
        }
    }

    if (text_from < src.size()) text(src.substr(text_from));
}

bool compilar_plantilla(const std::string& fuente, const std::string& file,
                        const std::string& dir,
                        const std::vector<TypedName>& data,
                        DiagnosticBag& diags, Template& out) {
    Compilador c(dir, diags, out);
    return c.compilar(fuente, file, data);
}

// ─── Renderizado ─────────────────────────────────────────────────────────────

bool render_plantilla(const Template& p, std::vector<Value> values,
                      NativeCtx& ctx, const FunctionTable* fns,
                      std::string& out, std::string& error) {
    // One slot per name: the input data already arrives, the rest are loop
    // variables that get filled in.
    values.resize(p.names.size());

    // Each loop's list is kept alive for as long as it lasts: the item of every
    // pass comes out of it.
    struct Frame { std::shared_ptr<Value> list_; size_t i; };
    std::vector<Frame> loops;

    thread_local VM vm;

    auto evaluar = [&](uint32_t idx, Value& out) -> bool {
        VM::Result r = vm.start(p.exprs[idx], values, ctx, fns);
        if (r.status != VM::Status::Done) {
            error = r.error;
            return false;
        }
        out = std::move(r.value);
        return true;
    };

    auto poner_loop = [&](uint32_t slot_loop, size_t i, size_t n) {
        Value::Dict d;
        d.reserve(5);
        d["index"]  = Value::integer(static_cast<long long>(i + 1));
        d["index0"] = Value::integer(static_cast<long long>(i));
        d["first"]  = Value::boolean(i == 0);
        d["last"]   = Value::boolean(i + 1 == n);
        d["length"] = Value::integer(static_cast<long long>(n));
        values[slot_loop] = Value::dict(std::move(d));
    };

    size_t pc = 0;
    while (pc < p.code.size()) {
        const auto& in = p.code[pc];
        switch (in.op) {
            case Template::Op::Text:
                out += p.texts[in.a];
                ++pc;
                break;

            case Template::Op::Write: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                escapar_html(v.to_string(), out);
                ++pc;
                break;
            }
            case Template::Op::WriteRaw: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                out += v.to_string();
                ++pc;
                break;
            }
            case Template::Op::SaltarSiFalso: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                pc = v.truthy() ? pc + 1 : in.b;
                break;
            }
            case Template::Op::Saltar:
                pc = in.b;
                break;

            case Template::Op::BucleInicio: {
                Value v;
                if (!evaluar(in.a, v)) return false;
                if (!v.is_list()) {
                    error = std::string("{% for %} needs a list, not ") +
                            v.type_name();
                    return false;
                }
                auto list_ = std::make_shared<Value>(std::move(v));
                if (list_->as_list().empty()) { pc = in.b; break; }
                loops.push_back({list_, 0});
                values[in.slot] = list_->as_list()[0];
                poner_loop(in.slot_loop, 0, list_->as_list().size());
                ++pc;
                break;
            }
            case Template::Op::BucleSiguiente: {
                Frame& m = loops.back();
                const auto& l = m.list_->as_list();
                if (++m.i < l.size()) {
                    values[in.slot] = l[m.i];
                    poner_loop(in.slot_loop, m.i, l.size());
                    pc = in.b;
                } else {
                    loops.pop_back();
                    ++pc;
                }
                break;
            }
        }
    }
    return true;
}

} // namespace lumen_script
