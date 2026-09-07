#include <lumen_script/parser.hpp>
#include <cstdlib>
#include <iostream>

namespace lumen_script {

std::string TypeRef::str() const {
    std::string s = name;
    if (!args.empty()) {
        s += "<";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i) s += ", ";
            s += args[i].str();
        }
        s += ">";
    }
    if (optional) s += "?";
    return s;
}

// ─── Navigation ──────────────────────────────────────────────────────────────

const Token& Parser::peek(size_t ahead) const {
    size_t j = i_ + ahead;
    return j < toks_.size() ? toks_[j] : toks_.back();
}

const Token& Parser::prev() const {
    return i_ > 0 ? toks_[i_ - 1] : toks_.front();
}

const Token& Parser::advance() {
    if (i_ + 1 < toks_.size()) ++i_;
    return toks_[i_ - 1];
}

bool Parser::match(Tok k) {
    if (!check(k)) return false;
    advance();
    return true;
}

void Parser::error_here(std::string msg) {
    diags_.error(peek().loc, std::move(msg));
}

bool Parser::expect(Tok k, const char* context) {
    if (match(k)) return true;
    error_here(std::string("expected '") + tok_name(k) + "' " + context +
               ", but there is '" + tok_name(peek().kind) + "'");
    return false;
}

void Parser::skip_newlines() {
    while (check(Tok::Newline)) advance();
}

// After an error, it advances to something that can start a declaration, so as
// not to chain failures derived from the first one.
void Parser::synchronize() {
    int depth = 0;
    while (!check(Tok::EndOfFile)) {
        switch (peek().kind) {
            case Tok::Indent: ++depth; break;
            case Tok::Dedent: if (depth > 0) --depth; break;
            case Tok::KwGet: case Tok::KwPost: case Tok::KwPut:
            case Tok::KwPatch: case Tok::KwDelete: case Tok::KwAny:
            case Tok::KwSse: case Tok::KwWs: case Tok::KwApp:
            case Tok::KwClass: case Tok::KwFn: case Tok::KwGroup:
            case Tok::KwImport: case Tok::KwOn:
                if (depth == 0) return;
                break;
            default: break;
        }
        advance();
    }
}

// ─── Program ─────────────────────────────────────────────────────────────────

// A standalone expression, for the templates.  It is required to consume the
// whole stream: `{{ a.title garbage }}` has to be an error, not a partial read.
ExprPtr Parser::parse_single_expression() {
    skip_newlines();
    if (check(Tok::EndOfFile)) {
        error_here("expected an expression");
        return nullptr;
    }
    ExprPtr e = parse_expr();
    skip_newlines();
    if (!check(Tok::EndOfFile)) {
        error_here("trailing input after the expression");
        return nullptr;
    }
    return e;
}

void Parser::parse_into(Program& out) {
    skip_newlines();
    while (!check(Tok::EndOfFile)) {
        size_t before = i_;
        parse_declaration(out);
        skip_newlines();
        if (i_ == before) advance();   // safety net: never an infinite loop
    }
}

void Parser::parse_declaration(Program& out) {
    switch (peek().kind) {
        case Tok::KwGet: case Tok::KwPost: case Tok::KwPut:
        case Tok::KwPatch: case Tok::KwDelete: case Tok::KwAny:
        case Tok::KwSse: case Tok::KwWs: {
            const Token& m = advance();
            parse_route(out, m, "", {});
            return;
        }
        case Tok::KwGroup:
            parse_group(out, "", {});
            return;
        case Tok::KwApp:
            parse_app(out);
            return;
        case Tok::KwClass:
            parse_class(out);
            return;
        case Tok::KwOn:
            parse_error(out);
            return;
        case Tok::KwFn:
            parse_fn(out);
            return;

        // Declarations the grammar defines but that are not compiled yet.  They
        // are reported explicitly instead of failing with a confusing syntax
        // error.
        case Tok::KwImport: {
            SourceLoc loc = advance().loc;
            if (!check(Tok::Ident)) {
                error_here("expected the module name after 'import'");
                synchronize();
                return;
            }
            std::string mod = advance().text;
            if (!out.imports.insert(mod).second)
                diags_.error(loc, "'" + mod + "' was already imported");
            return;
        }

        default:
            error_here("expected a declaration (get/post/... endpoint, or app)");
            synchronize();
            return;
    }
}

// ─── Routes ──────────────────────────────────────────────────────────────────

void Parser::parse_route(Program& out, const Token& method_tok,
                         const std::string& prefix,
                         const std::vector<Guard>& guards) {
    RouteDecl r;
    r.loc = method_tok.loc;
    switch (method_tok.kind) {
        case Tok::KwGet:    r.method = "GET";    break;
        case Tok::KwPost:   r.method = "POST";   break;
        case Tok::KwPut:    r.method = "PUT";    break;
        case Tok::KwPatch:  r.method = "PATCH";  break;
        case Tok::KwDelete: r.method = "DELETE"; break;
        case Tok::KwAny:    r.method = "*";      break;
        case Tok::KwSse:    r.method = "SSE";    break;
        case Tok::KwWs:     r.method = "WS";     break;
        default:            r.method = "GET";    break;
    }

    if (!expect(Tok::KwEndpoint, "after the route method")) { synchronize(); return; }
    if (!expect(Tok::LParen, "after 'endpoint'"))               { synchronize(); return; }

    if (!check(Tok::String)) {
        error_here("the first argument of endpoint() is the route pattern, in quotes");
        synchronize();
        return;
    }
    r.pattern_loc = peek().loc;
    r.pattern     = advance().text;

    // The group prefix is glued in front, avoiding the double slash of
    // group("/api") + endpoint("/users").
    if (!prefix.empty()) {
        std::string p = prefix;
        if (!p.empty() && p.back() == '/') p.pop_back();
        if (r.pattern == "/")             r.pattern = p.empty() ? "/" : p;
        else if (r.pattern.empty() || r.pattern[0] != '/') r.pattern = p + "/" + r.pattern;
        else                              r.pattern = p + r.pattern;
    }

    // Every route carries the complete list of guards wrapping it, from the
    // outside in.
    r.guards = guards;

    while (match(Tok::Comma)) {
        if (check(Tok::RParen)) break;          // coma final tolerada
        r.params.push_back(parse_param());
    }
    if (!expect(Tok::RParen, "closing endpoint()")) { synchronize(); return; }

    // origins(...) modifier — the grammar accepts it on any route and the
    // checker rejects it outside ws.
    if (match(Tok::KwOrigins)) {
        expect(Tok::LParen, "after 'origins'");
        while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
            if (check(Tok::String)) r.origins.push_back(advance().text);
            else { error_here("origins() only accepts strings"); advance(); }
            if (!match(Tok::Comma)) break;
        }
        expect(Tok::RParen, "closing origins()");
    }

    if (!expect(Tok::Colon, "opening the route body")) { synchronize(); return; }
    r.body = parse_block();
    out.routes.push_back(std::move(r));
}

Param Parser::parse_param() {
    Param p;
    p.loc  = peek().loc;
    p.type = parse_type();
    if (check(Tok::Ident)) p.name = advance().text;
    else                   error_here("expected the parameter name");
    if (match(Tok::Assign)) p.default_value = parse_expr();
    return p;
}

// ─── Functions ───────────────────────────────────────────────────────────────

void Parser::parse_fn(Program& out) {
    FnDecl f;
    f.loc = advance().loc;                            // 'fn'

    f.return_type = parse_type();
    if (check(Tok::Ident)) f.name = advance().text;
    else { error_here("expected the function name"); synchronize(); return; }

    for (const auto& prev_fn : out.functions) {
        if (prev_fn.name == f.name) {
            diags_.error(f.loc, "function '" + f.name + "' is already declared");
            break;
        }
    }

    if (!expect(Tok::LParen, "after the function name")) { synchronize(); return; }
    while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
        f.params.push_back(parse_param());
        if (!match(Tok::Comma)) break;
    }
    if (!expect(Tok::RParen, "closing the parameters")) { synchronize(); return; }
    if (!expect(Tok::Colon, "opening the function body")) { synchronize(); return; }

    f.body = parse_block();
    out.functions.push_back(std::move(f));
}

// ─── Error handlers ──────────────────────────────────────────────────────────

void Parser::parse_error(Program& out) {
    ErrorDecl e;
    e.loc = advance().loc;                          // 'on'

    if (!expect(Tok::KwError, "after 'on'")) { synchronize(); return; }

    if (check(Tok::Int)) {
        long v = std::strtol(advance().text.c_str(), nullptr, 10);
        // It only makes sense for what the engine generates: with a 2xx the
        // handler already wrote the body, and overwriting it would be a filter.
        if (v < 400 || v > 599) {
            diags_.error(prev().loc, "'on error' only covers codes 400-599; with a "
                                     "2xx the handler has already written the response");
            synchronize();
            return;
        }
        e.code = static_cast<int>(v);
    }

    for (const auto& prev_decl : out.errors) {
        if (prev_decl.code == e.code) {
            diags_.error(e.loc, e.code ? "there is already a handler for code " +
                                         std::to_string(e.code)
                                       : std::string("there is already a global error handler"));
            break;
        }
    }

    if (!expect(Tok::Colon, "opening the error handler")) { synchronize(); return; }
    e.body = parse_block();
    out.errors.push_back(std::move(e));
}

// ─── Groups ──────────────────────────────────────────────────────────────────

void Parser::parse_group(Program& out, const std::string& prefix,
                         const std::vector<Guard>& outer_guards) {
    SourceLoc loc = advance().loc;                    // 'group'

    if (!expect(Tok::LParen, "after 'group'")) { synchronize(); return; }
    std::string own_prefix;
    if (check(Tok::String)) own_prefix = advance().text;
    else { error_here("group() expects the URL prefix in quotes"); }
    if (!expect(Tok::RParen, "closing group()")) { synchronize(); return; }
    if (!expect(Tok::Colon, "opening the group body")) { synchronize(); return; }

    std::string combined = prefix;
    if (!combined.empty() && combined.back() == '/') combined.pop_back();
    if (!own_prefix.empty() && own_prefix != "/") {
        if (own_prefix[0] != '/') combined += "/";
        combined += own_prefix;
        if (!combined.empty() && combined.back() == '/') combined.pop_back();
    }

    skip_newlines();
    if (!expect(Tok::Indent, "opening the group block")) { synchronize(); return; }

    // Guards accumulate: a nested route has to pass the parent's and then its
    // own, in that order.
    std::vector<Guard> guards = outer_guards;

    bool seen_route = false;
    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
        size_t before = i_;

        if (check(Tok::KwRequire)) {
            SourceLoc gloc = advance().loc;
            if (seen_route)
                diags_.error(gloc, "group guards come before its routes");
            Guard g;
            g.loc       = gloc;
            g.condition = std::shared_ptr<Expr>(parse_expr());
            if (expect(Tok::KwElse, "in 'require ... else ...'"))
                g.otherwise = std::shared_ptr<Expr>(parse_expr());
            guards.push_back(std::move(g));
        }
        else if (check(Tok::KwGroup)) {
            seen_route = true;
            parse_group(out, combined, guards);
        }
        else {
            switch (peek().kind) {
                case Tok::KwGet: case Tok::KwPost: case Tok::KwPut:
                case Tok::KwPatch: case Tok::KwDelete: case Tok::KwAny:
                case Tok::KwSse: case Tok::KwWs: {
                    seen_route = true;
                    const Token& m = advance();
                    parse_route(out, m, combined, guards);
                    break;
                }
                default:
                    error_here("a group only accepts 'require', routes and "
                               "other groups");
                    while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                           !check(Tok::EndOfFile)) advance();
            }
        }

        skip_newlines();
        if (i_ == before) advance();
    }
    match(Tok::Dedent);
    (void)loc;
}

// ─── Classes ─────────────────────────────────────────────────────────────────

void Parser::parse_class(Program& out) {
    ClassDecl c;
    c.loc = advance().loc;                        // 'class'

    if (check(Tok::Ident)) c.name = advance().text;
    else { error_here("expected the class name"); synchronize(); return; }

    if (!expect(Tok::Colon, "after the class name")) { synchronize(); return; }
    skip_newlines();
    if (!expect(Tok::Indent, "opening the class body")) { synchronize(); return; }

    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
        size_t before = i_;

        // validate:
        //     <expression>   "message"
        if (check(Tok::KwValidate)) {
            advance();
            expect(Tok::Colon, "after 'validate'");
            skip_newlines();
            if (!expect(Tok::Indent, "opening the validate block")) break;

            while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
                skip_newlines();
                if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;

                ValidateRule rule;
                rule.loc       = peek().loc;
                rule.condition = parse_expr();
                if (check(Tok::String)) rule.message = advance().text;
                else error_here("every validate rule needs its message in quotes");
                c.rules.push_back(std::move(rule));
                skip_newlines();
            }
            match(Tok::Dedent);
        }
        // Method: fn <type> <name>(...)
        else if (check(Tok::KwFn)) {
            FnDecl m;
            m.loc = advance().loc;
            m.return_type = parse_type();
            if (check(Tok::Ident)) m.name = advance().text;
            else { error_here("expected the method name"); break; }

            for (const auto& prev_m : c.methods)
                if (prev_m.name == m.name)
                    diags_.error(m.loc, "method '" + m.name + "' is already declared "
                                        "in class '" + c.name + "'");

            expect(Tok::LParen, "after the method name");
            while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
                m.params.push_back(parse_param());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "closing the method parameters");
            expect(Tok::Colon, "opening the method body");
            m.body = parse_block();
            c.methods.push_back(std::move(m));
        }
        // Constructor: ClassName(...), with or without a body.
        else if (check(Tok::Ident) && peek().text == c.name && peek(1).is(Tok::LParen)) {
            CtorDecl ct;
            ct.loc = advance().loc;
            advance();                                   // '('
            while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
                ct.params.push_back(parse_param());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "closing the constructor parameters");

            for (const auto& prev_ct : c.ctors)
                if (prev_ct.params.size() == ct.params.size())
                    diags_.error(ct.loc, "there is already a constructor for '" + c.name +
                                         "' with " + std::to_string(ct.params.size()) +
                                         " parameter(s); they are told apart by their count");

        // Without a colon, it is the automatic mapping constructor.
            if (match(Tok::Colon)) {
                ct.has_body = true;
                ct.body     = parse_block();
            }
            c.ctors.push_back(std::move(ct));
        }
        // Field: <type> <name>
        else {
            Field f;
            f.loc  = peek().loc;
            f.type = parse_type();
            if (check(Tok::Ident)) f.name = advance().text;
            else error_here("expected the field name");

            for (const auto& prev_field : c.fields) {
                if (prev_field.name == f.name) {
                    diags_.error(f.loc, "field '" + f.name + "' is duplicated");
                    break;
                }
            }
            c.fields.push_back(std::move(f));
        }

        skip_newlines();
        if (i_ == before) advance();
    }
    match(Tok::Dedent);

    if (c.fields.empty())
        diags_.error(c.loc, "class '" + c.name + "' declares no fields");

    out.classes.push_back(std::move(c));
}

// ─── app: block ──────────────────────────────────────────────────────────────

// kind: 0 string, 1 number, 2 boolean.  env("VAR") is resolved right here and
// counts as a string; if the variable does not exist it stays empty and the
// caller decides whether that is an error.
bool Parser::config_value(std::string& text, long long& number, bool& flag, int& kind) {
    if (check(Tok::String)) { text = advance().text; kind = 0; return true; }
    if (check(Tok::Int))    { number = std::strtoll(advance().text.c_str(), nullptr, 10); kind = 1; return true; }
    if (check(Tok::KwTrue) || check(Tok::KwFalse)) {
        flag = advance().kind == Tok::KwTrue;
        kind = 2;
        return true;
    }
    if (check(Tok::Ident) && peek().text == "env" && peek(1).is(Tok::LParen)) {
        advance(); advance();
        if (!check(Tok::String)) { error_here("env() expects the name in quotes"); return false; }
        Token name_tok = advance();
        const std::string& name = name_tok.text;
        expect(Tok::RParen, "closing env()");
        const char* v = std::getenv(name.c_str());
        // Without this, a forgotten environment variable turns into "" with
        // nobody noticing until production: for session/jwt an empty secret
        // fails closed (it is treated the same as "not configured"), but the
        // real error stays hidden behind a message that does not mention it.
        // It is not a compile error — a .lum has no business knowing the final
        // deployment environment, and --check must be able to run without it.
        if (!v) {
            std::cerr << "lumen: warning: " << (name_tok.loc.file ? *name_tok.loc.file : "?")
                      << ":" << name_tok.loc.line << ":" << name_tok.loc.col
                      << ": the environment variable '" << name
                      << "' is not defined; using \"\" instead\n";
        }
        text = v ? v : "";
        kind = 0;
        return true;
    }
    return false;
}

void Parser::parse_app(Program& out) {
    SourceLoc loc = advance().loc;                    // 'app'
    if (out.app.present) {
        diags_.error(loc, "the 'app:' block can only appear once in the project");
    }
    out.app.present = true;
    out.app.loc     = loc;

    if (!expect(Tok::Colon, "after 'app'")) { synchronize(); return; }
    skip_newlines();
    if (!expect(Tok::Indent, "opening the app block")) { synchronize(); return; }

    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;

        const Token& key = peek();

        if (key.is(Tok::KwStatic)) {
            advance();
            StaticMount m;
            m.loc = key.loc;
            if (check(Tok::String)) m.url_prefix = advance().text;
            else error_here("expected the URL prefix in quotes");
            expect(Tok::Arrow, "between the prefix and the directory");
            if (check(Tok::String)) m.fs_root = advance().text;
            else error_here("expected the directory in quotes");
            if (match(Tok::KwSpa)) m.spa = true;
            out.app.statics.push_back(std::move(m));
        }
        else if (key.is(Tok::Ident)) {
            std::string k = advance().text;
            if (k == "name" || k == "version" || k == "templates") {
                if (!check(Tok::String)) { error_here("expected a string"); }
                else {
                    std::string v = advance().text;
                    if      (k == "name")      out.app.name = v;
                    else if (k == "version")   out.app.version = v;
                    else                       out.app.templates_dir = v;
                }
            } else if (k == "port") {
                if (!check(Tok::Int)) error_here("expected a port number");
                else {
                    long v = std::strtol(advance().text.c_str(), nullptr, 10);
                    if (v < 1 || v > 65535)
                        diags_.error(prev().loc, "port out of range (1-65535)");
                    else out.app.port = static_cast<int>(v);
                }
            } else if (k == "docs")    { out.app.docs    = true; }
            else if   (k == "health")  { out.app.health  = true; }
            else if   (k == "metrics") { out.app.metrics = true; }
            // Configuration block of an imported module.
            else if (out.imports.count(k)) {
                expect(Tok::Colon, "after the module name");
                skip_newlines();
                if (!expect(Tok::Indent, "opening the module configuration")) break;

                auto& opts = out.app.modules[k];
                while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
                    skip_newlines();
                    if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
                    if (!check(Tok::Ident)) { error_here("expected a key"); advance(); continue; }

                    std::string mk = advance().text;
                    std::string text; long long number = 0; bool flag = false; int kind = -1;
                    if (!config_value(text, number, flag, kind)) {
                        error_here("invalid value: expected a string, a number, "
                                   "true/false o env(\"VAR\")");
                        while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                               !check(Tok::EndOfFile)) advance();
                        continue;
                    }
                    // Everything is kept as text: each driver reads its own.
                    opts[mk] = (kind == 1) ? std::to_string(number)
                             : (kind == 2) ? (flag ? "true" : "false")
                                           : text;
                    skip_newlines();
                }
                match(Tok::Dedent);
            }
            // session: and jwt: are sub-blocks with their own keys.
            else if (k == "session" || k == "jwt") {
                bool is_session = (k == "session");
                expect(Tok::Colon, "after the sub-block key");
                skip_newlines();
                if (!expect(Tok::Indent, "opening the sub-block")) break;

                while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
                    skip_newlines();
                    if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
                    if (!check(Tok::Ident)) { error_here("expected a key"); advance(); continue; }

                    const Token& sub = peek();
                    std::string  sk  = advance().text;
                    std::string  text; long long number = 0; bool flag = false; int kind = -1;
                    if (!config_value(text, number, flag, kind)) {
                        error_here("invalid value: expected a string, a number, "
                                   "true/false o env(\"VAR\")");
                        while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                               !check(Tok::EndOfFile)) advance();
                        continue;
                    }

                    if (is_session) {
                        if      (sk == "secret"  && kind == 0) out.app.session_secret  = text;
                        else if (sk == "max_age" && kind == 1) out.app.session_max_age = (int)number;
                        else if (sk == "secure"  && kind == 2) out.app.session_secure  = flag;
                        else diags_.error(sub.loc, "unknown key or wrong type "
                                                   "in session: '" + sk + "'");
                    } else {
                        if      (sk == "secret" && kind == 0) out.app.jwt_secret = text;
                        else if (sk == "issuer" && kind == 0) out.app.jwt_issuer = text;
                        else diags_.error(sub.loc, "unknown key or wrong type "
                                                   "in jwt: '" + sk + "'");
                    }
                    skip_newlines();
                }
                match(Tok::Dedent);
            }
            else {
                diags_.error(key.loc, "unknown key in the app block: '" + k + "'");
                while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                       !check(Tok::EndOfFile)) advance();
            }
        }
        else {
            error_here("expected a configuration key");
            while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                   !check(Tok::EndOfFile)) advance();
        }

        skip_newlines();
    }
    match(Tok::Dedent);
}

// ─── Blocks and statements ───────────────────────────────────────────────────

Block Parser::parse_block() {
    Block body;
    skip_newlines();
    if (!expect(Tok::Indent, "opening the block")) return body;

    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
        size_t before = i_;
        if (auto s = parse_statement()) body.push_back(std::move(s));
        skip_newlines();
        if (i_ == before) advance();
    }
    match(Tok::Dedent);
    return body;
}

StmtPtr Parser::parse_statement() {
    switch (peek().kind) {
        case Tok::KwReturn:   return parse_return();
        case Tok::KwIf:       return parse_if();
        case Tok::KwWhile:    return parse_while();
        case Tok::KwFor:      return parse_for();
        case Tok::KwRequire:  return parse_require();
        case Tok::KwTry:      return parse_try();
        case Tok::KwBreak: {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Break; s->loc = advance().loc;
            return s;
        }
        case Tok::KwContinue: {
            auto s = std::make_unique<Stmt>();
            s->kind = StmtKind::Continue; s->loc = advance().loc;
            return s;
        }
        default: break;
    }

    // Variable declaration: <type> <ident> [= expr]
    if (looks_like_type()) {
        auto s  = std::make_unique<Stmt>();
        s->kind = StmtKind::VarDecl;
        s->loc  = peek().loc;
        s->type = parse_type();
        if (check(Tok::Ident)) s->name = advance().text;
        else                   error_here("expected the variable name");
        if (match(Tok::Assign)) s->value = parse_expr();
        return s;
    }

    // Assignment or standalone expression.  `x++` is an expression like any
    // other: as a statement, its value is simply discarded.
    SourceLoc loc = peek().loc;
    ExprPtr   e   = parse_expr();

    // x += e  →  x = x + e, and so on for all five.
    if (check(Tok::PlusEq) || check(Tok::MinusEq) || check(Tok::StarEq) ||
        check(Tok::SlashEq) || check(Tok::PercentEq)) {
        const char* op = nullptr;
        switch (peek().kind) {
            case Tok::PlusEq:    op = "+"; break;
            case Tok::MinusEq:   op = "-"; break;
            case Tok::StarEq:    op = "*"; break;
            case Tok::SlashEq:   op = "/"; break;
            default:             op = "%"; break;
        }
        SourceLoc oploc = advance().loc;
        if (!assignable(*e)) {
            diags_.error(loc, "can only assign to a variable or a field");
            return nullptr;
        }
        auto bin  = make(ExprKind::Binary, oploc);
        bin->text = op;
        bin->lhs  = clone_target(*e);
        bin->rhs  = parse_expr();

        auto s    = std::make_unique<Stmt>();
        s->kind   = StmtKind::Assign;
        s->loc    = loc;
        s->target = std::move(e);
        s->value  = std::move(bin);
        return s;
    }

    if (match(Tok::Assign)) {
        auto s    = std::make_unique<Stmt>();
        s->kind   = StmtKind::Assign;
        s->loc    = loc;
        s->target = std::move(e);
        s->value  = parse_expr();
        return s;
    }
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::ExprStmt;
    s->loc  = loc;
    s->value = std::move(e);
    return s;
}

// Only a variable or a field can sit on the left of an assignment.
bool Parser::assignable(const Expr& e) {
    return e.kind == ExprKind::Ident || e.kind == ExprKind::Member ||
           e.kind == ExprKind::Index;
}

// Copies the left-hand side so it can be read and written in the same
// statement.  For Ident and Member a shallow copy of the receiver is enough.
ExprPtr Parser::clone_target(const Expr& e) {
    auto out  = make(e.kind, e.loc);
    out->text = e.text;
    if (e.object) out->object = clone_target(*e.object);
    if (e.kind == ExprKind::Index && e.lhs) out->lhs = clone_target(*e.lhs);
    out->int_value   = e.int_value;
    out->float_value = e.float_value;
    out->bool_value  = e.bool_value;
    return out;
}

StmtPtr Parser::parse_return() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::Return;
    s->loc  = advance().loc;
    if (!check(Tok::Newline) && !check(Tok::Dedent) && !check(Tok::EndOfFile))
        s->value = parse_expr();
    return s;
}

StmtPtr Parser::parse_if() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::If;
    s->loc  = advance().loc;
    s->value = parse_expr();
    expect(Tok::Colon, "after the if condition");
    s->body = parse_block();

    skip_newlines();
    if (check(Tok::KwElif)) {
        // `elif` chains: the else holds a complete If.  `else if` does the same,
        // and both are accepted.
        s->orelse.push_back(parse_if_from_elif());
    }
    else if (check(Tok::KwElse)) {
        advance();
        if (check(Tok::KwIf)) {
            s->orelse.push_back(parse_if());
        } else {
            expect(Tok::Colon, "after 'else'");
            s->orelse = parse_block();
        }
    }
    return s;
}

// An `elif` is an `if` whose header has already been consumed.
StmtPtr Parser::parse_if_from_elif() {
    auto s   = std::make_unique<Stmt>();
    s->kind  = StmtKind::If;
    s->loc   = advance().loc;                 // 'elif'
    s->value = parse_expr();
    expect(Tok::Colon, "after the elif condition");
    s->body = parse_block();

    skip_newlines();
    if (check(Tok::KwElif)) {
        s->orelse.push_back(parse_if_from_elif());
    } else if (check(Tok::KwElse)) {
        advance();
        if (check(Tok::KwIf)) s->orelse.push_back(parse_if());
        else {
            expect(Tok::Colon, "after 'else'");
            s->orelse = parse_block();
        }
    }
    return s;
}

StmtPtr Parser::parse_while() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::While;
    s->loc  = advance().loc;
    s->value = parse_expr();
    expect(Tok::Colon, "after the while condition");
    s->body = parse_block();
    return s;
}

StmtPtr Parser::parse_for() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::For;
    s->loc  = advance().loc;
    s->type = parse_type();
    if (check(Tok::Ident)) s->name = advance().text;
    else                   error_here("expected the loop variable name");
    expect(Tok::KwIn, "in the for loop");
    s->target = parse_expr();
    expect(Tok::Colon, "after the for iterable");
    s->body = parse_block();
    return s;
}

StmtPtr Parser::parse_require() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::Require;
    s->loc  = advance().loc;
    s->value = parse_expr();
    if (!expect(Tok::KwElse, "in 'require ... else ...'")) return s;
    s->target = parse_expr();
    return s;
}

StmtPtr Parser::parse_try() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::Try;
    s->loc  = advance().loc;
    expect(Tok::Colon, "after 'try'");
    s->body = parse_block();
    skip_newlines();
    if (!expect(Tok::KwCatch, "after the try block")) return s;
    if (check(Tok::Ident)) s->name = advance().text;
    expect(Tok::Colon, "after 'catch'");
    s->orelse = parse_block();
    return s;
}

// ─── Types ───────────────────────────────────────────────────────────────────

// Tells `List<string> xs = ...` (a declaration) from `a < b` (an expression).
// A type is: an identifier or type keyword, optionally with <...> and '?',
// followed by an identifier.
bool Parser::looks_like_type() const {
    if (!check(Tok::Ident) && !check(Tok::KwVoid)) return false;

    size_t j = 1;
    if (peek(j).is(Tok::Lt)) {
        int depth = 0;
        while (j < toks_.size() - i_) {
            Tok k = peek(j).kind;
            if      (k == Tok::Lt) ++depth;
            else if (k == Tok::Gt) { if (--depth == 0) { ++j; break; } }
            else if (k == Tok::Newline || k == Tok::EndOfFile) return false;
            ++j;
        }
        if (depth != 0) return false;
    }
    if (peek(j).is(Tok::Question)) ++j;
    return peek(j).is(Tok::Ident);
}

TypeRef Parser::parse_type() {
    TypeRef t;
    t.loc = peek().loc;
    if (check(Tok::KwVoid)) { advance(); t.name = "void"; return t; }

    if (!check(Tok::Ident)) {
        error_here("expected a type");
        return t;
    }
    t.name = advance().text;

    if (match(Tok::Lt)) {
        do {
            t.args.push_back(parse_type());
        } while (match(Tok::Comma));
        // The lexer never merges '>>', so every level closes with its own Gt.
        expect(Tok::Gt, "closing the type parameters");
    }
    if (match(Tok::Question)) t.optional = true;
    return t;
}

// ─── Expressions ─────────────────────────────────────────────────────────────

ExprPtr Parser::make(ExprKind k, SourceLoc loc) {
    auto e  = std::make_unique<Expr>();
    e->kind = k;
    e->loc  = loc;
    return e;
}

ExprPtr Parser::parse_expr() { return parse_ternary(); }

ExprPtr Parser::parse_ternary() {
    ExprPtr cond = parse_or();
    if (!check(Tok::Question)) return cond;

    SourceLoc loc = advance().loc;
    auto e = make(ExprKind::Ternary, loc);
    e->object = std::move(cond);
    e->lhs    = parse_expr();
    expect(Tok::Colon, "in the ternary operator");
    e->rhs    = parse_expr();
    return e;
}

ExprPtr Parser::parse_or() {
    ExprPtr l = parse_and();
    while (check(Tok::KwOr)) {
        SourceLoc loc = advance().loc;
        auto e = make(ExprKind::Binary, loc);
        e->text = "or"; e->lhs = std::move(l); e->rhs = parse_and();
        l = std::move(e);
    }
    return l;
}

ExprPtr Parser::parse_and() {
    ExprPtr l = parse_not();
    while (check(Tok::KwAnd)) {
        SourceLoc loc = advance().loc;
        auto e = make(ExprKind::Binary, loc);
        e->text = "and"; e->lhs = std::move(l); e->rhs = parse_not();
        l = std::move(e);
    }
    return l;
}

ExprPtr Parser::parse_not() {
    if (check(Tok::KwNot)) {
        SourceLoc loc = advance().loc;
        auto e = make(ExprKind::Unary, loc);
        e->text = "not"; e->lhs = parse_not();
        return e;
    }
    return parse_equality();
}

ExprPtr Parser::parse_equality() {
    ExprPtr l = parse_comparison();
    while (check(Tok::Eq) || check(Tok::NotEq)) {
        const Token& op = advance();
        auto e = make(ExprKind::Binary, op.loc);
        e->text = tok_name(op.kind);
        e->lhs = std::move(l); e->rhs = parse_comparison();
        l = std::move(e);
    }
    return l;
}

ExprPtr Parser::parse_comparison() {
    ExprPtr l = parse_sum();
    while (check(Tok::Lt) || check(Tok::LtEq) || check(Tok::Gt) || check(Tok::GtEq)) {
        const Token& op = advance();
        auto e = make(ExprKind::Binary, op.loc);
        e->text = tok_name(op.kind);
        e->lhs = std::move(l); e->rhs = parse_sum();
        l = std::move(e);
    }
    return l;
}

ExprPtr Parser::parse_sum() {
    ExprPtr l = parse_product();
    while (check(Tok::Plus) || check(Tok::Minus)) {
        const Token& op = advance();
        auto e = make(ExprKind::Binary, op.loc);
        e->text = tok_name(op.kind);
        e->lhs = std::move(l); e->rhs = parse_product();
        l = std::move(e);
    }
    return l;
}

ExprPtr Parser::parse_product() {
    ExprPtr l = parse_unary();
    while (check(Tok::Star) || check(Tok::Slash) || check(Tok::Percent)) {
        const Token& op = advance();
        auto e = make(ExprKind::Binary, op.loc);
        e->text = tok_name(op.kind);
        e->lhs = std::move(l); e->rhs = parse_unary();
        l = std::move(e);
    }
    return l;
}

ExprPtr Parser::parse_unary() {
    // ++x / --x : increments and yields the ALREADY incremented value.
    if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
        bool      up  = peek().is(Tok::PlusPlus);
        SourceLoc loc = advance().loc;
        auto e  = make(ExprKind::PreStep, loc);
        e->text = up ? "+" : "-";
        e->lhs  = parse_unary();
        if (e->lhs && !assignable(*e->lhs))
            diags_.error(loc, "'++' and '--' only apply to a variable or a field");
        return e;
    }
    if (check(Tok::Minus)) {
        SourceLoc loc = advance().loc;
        auto e = make(ExprKind::Unary, loc);
        e->text = "-"; e->lhs = parse_unary();
        return e;
    }
    return parse_postfix();
}

ExprPtr Parser::parse_postfix() {
    ExprPtr e = parse_primary();

    while (true) {
        // x++ / x-- : increments and yields the PREVIOUS value.
        if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
            bool      up  = peek().is(Tok::PlusPlus);
            SourceLoc loc = advance().loc;
            if (e && !assignable(*e))
                diags_.error(loc, "'++' and '--' only apply to a variable or a field");
            auto step  = make(ExprKind::PostStep, loc);
            step->text = up ? "+" : "-";
            step->lhs  = std::move(e);
            e = std::move(step);
            continue;
        }
        if (check(Tok::Dot)) {
            SourceLoc loc = advance().loc;
            auto m = make(ExprKind::Member, loc);
            m->object = std::move(e);
            // After a dot, a reserved word is just a name: `state.get` and
            // `log.error` need not clash with `get` and `error`.
            if (check(Tok::Ident) || !peek().text.empty()) m->text = advance().text;
            else error_here("expected a name after '.'");
            e = std::move(m);
        }
        else if (check(Tok::LParen)) {
            SourceLoc loc = advance().loc;
            auto c = make(ExprKind::Call, loc);
            c->object = std::move(e);
            bool seen_named = false;
            while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
                Arg a;
                a.loc = peek().loc;
                // Named argument: IDENT '=' expr
                if (check(Tok::Ident) && peek(1).is(Tok::Assign)) {
                    a.name = advance().text;
                    advance();
                    seen_named = true;
                } else if (seen_named) {
                    error_here("positional arguments come before named ones");
                }
                a.value = parse_expr();
                c->args.push_back(std::move(a));
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "closing the call");
            e = std::move(c);
        }
        else if (check(Tok::LBracket)) {
            SourceLoc loc = advance().loc;
            auto ix = make(ExprKind::Index, loc);
            ix->object = std::move(e);
            ix->lhs    = parse_expr();
            expect(Tok::RBracket, "closing the index");
            e = std::move(ix);
        }
        else break;
    }
    return e;
}

ExprPtr Parser::parse_primary() {
    const Token& t = peek();

    switch (t.kind) {
        case Tok::String: {
            auto e = make(ExprKind::StringLit, advance().loc);
            e->text = prev().text;
            return e;
        }
        case Tok::Int: {
            auto e = make(ExprKind::IntLit, advance().loc);
            e->int_value = std::strtoll(prev().text.c_str(), nullptr, 10);
            return e;
        }
        case Tok::Float: {
            auto e = make(ExprKind::FloatLit, advance().loc);
            e->float_value = std::strtod(prev().text.c_str(), nullptr);
            return e;
        }
        case Tok::KwTrue: case Tok::KwFalse: {
            auto e = make(ExprKind::BoolLit, advance().loc);
            e->bool_value = prev().is(Tok::KwTrue);
            return e;
        }
        case Tok::KwNull:
            return make(ExprKind::NullLit, advance().loc);
        case Tok::KwThis:
            return make(ExprKind::This, advance().loc);

        case Tok::KwAwait: {
            auto e = make(ExprKind::Await, advance().loc);
            e->lhs = parse_unary();
            return e;
        }

        // Words that are both a keyword and a reserved object inside a handler:
        // `sse.send(...)`, `ws.recv()`, `error.message`.
        case Tok::KwSse: case Tok::KwWs: case Tok::KwError: {
            auto e = make(ExprKind::Ident, advance().loc);
            e->text = prev().text;
            return e;
        }

        case Tok::Ident: {
            auto e = make(ExprKind::Ident, advance().loc);
            e->text = prev().text;
            return e;
        }

        case Tok::LParen: {
            advance();
            ExprPtr inner = parse_expr();
            expect(Tok::RParen, "closing the parenthesis");
            return inner;
        }

        case Tok::LBracket: {
            auto e = make(ExprKind::ListLit, advance().loc);
            while (!check(Tok::RBracket) && !check(Tok::EndOfFile)) {
                e->items.push_back(parse_expr());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RBracket, "closing the list");
            return e;
        }

        case Tok::LBrace: {
            auto e = make(ExprKind::DictLit, advance().loc);
            while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
                DictEntry entry;
                entry.key = parse_expr();
                expect(Tok::Colon, "between the dictionary key and value");
                entry.value = parse_expr();
                e->entries.push_back(std::move(entry));
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RBrace, "closing the dictionary");
            return e;
        }

        default: {
            error_here(std::string("expected an expression, but there is '") +
                       tok_name(t.kind) + "'");
            auto e = make(ExprKind::NullLit, t.loc);
            advance();
            return e;
        }
    }
}

} // namespace lumen_script
