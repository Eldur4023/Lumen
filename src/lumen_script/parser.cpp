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

// ─── Navegacion ──────────────────────────────────────────────────────────────

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
    error_here(std::string("se esperaba '") + tok_name(k) + "' " + context +
               ", pero hay '" + tok_name(peek().kind) + "'");
    return false;
}

void Parser::skip_newlines() {
    while (check(Tok::Newline)) advance();
}

// Tras un error, avanza hasta algo que pueda empezar una declaracion, para no
// encadenar fallos derivados del primero.
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

// ─── Programa ────────────────────────────────────────────────────────────────

// Una expresion suelta, para las plantillas.  Se exige que consuma todo el
// flujo: `{{ a.titulo basura }}` tiene que ser un error, no leerse a medias.
ExprPtr Parser::parse_single_expression() {
    skip_newlines();
    if (check(Tok::EndOfFile)) {
        error_here("se esperaba una expresion");
        return nullptr;
    }
    ExprPtr e = parse_expr();
    skip_newlines();
    if (!check(Tok::EndOfFile)) {
        error_here("sobra algo detras de la expresion");
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
        if (i_ == before) advance();   // red de seguridad: nunca bucle infinito
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

        // Declaraciones que la gramatica define pero que todavia no se compilan.
        // Se reportan explicitamente en vez de fallar con un error de sintaxis
        // confuso.
        case Tok::KwImport: {
            SourceLoc loc = advance().loc;
            if (!check(Tok::Ident)) {
                error_here("se esperaba el nombre del modulo tras 'import'");
                synchronize();
                return;
            }
            std::string mod = advance().text;
            if (!out.imports.insert(mod).second)
                diags_.error(loc, "'" + mod + "' ya estaba importado");
            return;
        }

        default:
            error_here("se esperaba una declaracion (get/post/... endpoint, o app)");
            synchronize();
            return;
    }
}

// ─── Rutas ───────────────────────────────────────────────────────────────────

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

    if (!expect(Tok::KwEndpoint, "tras el metodo de la ruta")) { synchronize(); return; }
    if (!expect(Tok::LParen, "tras 'endpoint'"))               { synchronize(); return; }

    if (!check(Tok::String)) {
        error_here("el primer argumento de endpoint() es el patron de ruta, entre comillas");
        synchronize();
        return;
    }
    r.pattern_loc = peek().loc;
    r.pattern     = advance().text;

    // El prefijo del grupo se pega delante, evitando la doble barra de
    // group("/api") + endpoint("/users").
    if (!prefix.empty()) {
        std::string p = prefix;
        if (!p.empty() && p.back() == '/') p.pop_back();
        if (r.pattern == "/")             r.pattern = p.empty() ? "/" : p;
        else if (r.pattern.empty() || r.pattern[0] != '/') r.pattern = p + "/" + r.pattern;
        else                              r.pattern = p + r.pattern;
    }

    // Cada ruta lleva la lista completa de guardas que la envuelven, de fuera
    // hacia dentro.
    r.guards = guards;

    while (match(Tok::Comma)) {
        if (check(Tok::RParen)) break;          // coma final tolerada
        r.params.push_back(parse_param());
    }
    if (!expect(Tok::RParen, "al cerrar endpoint()")) { synchronize(); return; }

    // Modificador origins(...) — la gramatica lo admite en cualquier ruta y el
    // checker lo rechaza fuera de ws.
    if (match(Tok::KwOrigins)) {
        expect(Tok::LParen, "tras 'origins'");
        while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
            if (check(Tok::String)) r.origins.push_back(advance().text);
            else { error_here("origins() solo admite cadenas"); advance(); }
            if (!match(Tok::Comma)) break;
        }
        expect(Tok::RParen, "al cerrar origins()");
    }

    if (!expect(Tok::Colon, "al abrir el cuerpo de la ruta")) { synchronize(); return; }
    r.body = parse_block();
    out.routes.push_back(std::move(r));
}

Param Parser::parse_param() {
    Param p;
    p.loc  = peek().loc;
    p.type = parse_type();
    if (check(Tok::Ident)) p.name = advance().text;
    else                   error_here("se esperaba el nombre del parametro");
    if (match(Tok::Assign)) p.default_value = parse_expr();
    return p;
}

// ─── Funciones ───────────────────────────────────────────────────────────────

void Parser::parse_fn(Program& out) {
    FnDecl f;
    f.loc = advance().loc;                            // 'fn'

    f.return_type = parse_type();
    if (check(Tok::Ident)) f.name = advance().text;
    else { error_here("se esperaba el nombre de la funcion"); synchronize(); return; }

    for (const auto& prev_fn : out.functions) {
        if (prev_fn.name == f.name) {
            diags_.error(f.loc, "la funcion '" + f.name + "' ya esta declarada");
            break;
        }
    }

    if (!expect(Tok::LParen, "tras el nombre de la funcion")) { synchronize(); return; }
    while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
        f.params.push_back(parse_param());
        if (!match(Tok::Comma)) break;
    }
    if (!expect(Tok::RParen, "al cerrar los parametros")) { synchronize(); return; }
    if (!expect(Tok::Colon, "al abrir el cuerpo de la funcion")) { synchronize(); return; }

    f.body = parse_block();
    out.functions.push_back(std::move(f));
}

// ─── Manejadores de error ────────────────────────────────────────────────────

void Parser::parse_error(Program& out) {
    ErrorDecl e;
    e.loc = advance().loc;                          // 'on'

    if (!expect(Tok::KwError, "tras 'on'")) { synchronize(); return; }

    if (check(Tok::Int)) {
        long v = std::strtol(advance().text.c_str(), nullptr, 10);
        // Solo tiene sentido para lo que genera el motor: con un 2xx el handler
        // ya escribio el cuerpo, y sobrescribirlo seria un filtro de respuesta.
        if (v < 400 || v > 599) {
            diags_.error(prev().loc, "'on error' solo cubre codigos 400-599; con un "
                                     "2xx el handler ya ha escrito la respuesta");
            synchronize();
            return;
        }
        e.code = static_cast<int>(v);
    }

    for (const auto& prev_decl : out.errors) {
        if (prev_decl.code == e.code) {
            diags_.error(e.loc, e.code ? "ya hay un manejador para el codigo " +
                                         std::to_string(e.code)
                                       : std::string("ya hay un manejador global de error"));
            break;
        }
    }

    if (!expect(Tok::Colon, "al abrir el manejador de error")) { synchronize(); return; }
    e.body = parse_block();
    out.errors.push_back(std::move(e));
}

// ─── Grupos ──────────────────────────────────────────────────────────────────

void Parser::parse_group(Program& out, const std::string& prefix,
                         const std::vector<Guard>& outer_guards) {
    SourceLoc loc = advance().loc;                    // 'group'

    if (!expect(Tok::LParen, "tras 'group'")) { synchronize(); return; }
    std::string own_prefix;
    if (check(Tok::String)) own_prefix = advance().text;
    else { error_here("group() espera el prefijo de URL entre comillas"); }
    if (!expect(Tok::RParen, "al cerrar group()")) { synchronize(); return; }
    if (!expect(Tok::Colon, "al abrir el cuerpo del grupo")) { synchronize(); return; }

    std::string combined = prefix;
    if (!combined.empty() && combined.back() == '/') combined.pop_back();
    if (!own_prefix.empty() && own_prefix != "/") {
        if (own_prefix[0] != '/') combined += "/";
        combined += own_prefix;
        if (!combined.empty() && combined.back() == '/') combined.pop_back();
    }

    skip_newlines();
    if (!expect(Tok::Indent, "al abrir el bloque del grupo")) { synchronize(); return; }

    // Las guardas se acumulan: una ruta anidada tiene que pasar las del padre
    // y luego las propias, en ese orden.
    std::vector<Guard> guards = outer_guards;

    bool seen_route = false;
    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
        size_t before = i_;

        if (check(Tok::KwRequire)) {
            SourceLoc gloc = advance().loc;
            if (seen_route)
                diags_.error(gloc, "las guardas del grupo van antes que sus rutas");
            Guard g;
            g.loc       = gloc;
            g.condition = std::shared_ptr<Expr>(parse_expr());
            if (expect(Tok::KwElse, "en 'require ... else ...'"))
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
                    error_here("dentro de un grupo solo caben 'require', rutas y "
                               "otros grupos");
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

// ─── Clases ──────────────────────────────────────────────────────────────────

void Parser::parse_class(Program& out) {
    ClassDecl c;
    c.loc = advance().loc;                        // 'class'

    if (check(Tok::Ident)) c.name = advance().text;
    else { error_here("se esperaba el nombre de la clase"); synchronize(); return; }

    if (!expect(Tok::Colon, "tras el nombre de la clase")) { synchronize(); return; }
    skip_newlines();
    if (!expect(Tok::Indent, "al abrir el cuerpo de la clase")) { synchronize(); return; }

    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
        size_t before = i_;

        // validate:
        //     <expresion>   "mensaje"
        if (check(Tok::KwValidate)) {
            advance();
            expect(Tok::Colon, "tras 'validate'");
            skip_newlines();
            if (!expect(Tok::Indent, "al abrir el bloque validate")) break;

            while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
                skip_newlines();
                if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;

                ValidateRule rule;
                rule.loc       = peek().loc;
                rule.condition = parse_expr();
                if (check(Tok::String)) rule.message = advance().text;
                else error_here("cada regla de validate lleva su mensaje entre comillas");
                c.rules.push_back(std::move(rule));
                skip_newlines();
            }
            match(Tok::Dedent);
        }
        // Metodo: fn <tipo> <nombre>(...)
        else if (check(Tok::KwFn)) {
            FnDecl m;
            m.loc = advance().loc;
            m.return_type = parse_type();
            if (check(Tok::Ident)) m.name = advance().text;
            else { error_here("se esperaba el nombre del metodo"); break; }

            for (const auto& prev_m : c.methods)
                if (prev_m.name == m.name)
                    diags_.error(m.loc, "el metodo '" + m.name + "' ya esta declarado "
                                        "en la clase '" + c.name + "'");

            expect(Tok::LParen, "tras el nombre del metodo");
            while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
                m.params.push_back(parse_param());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "al cerrar los parametros del metodo");
            expect(Tok::Colon, "al abrir el cuerpo del metodo");
            m.body = parse_block();
            c.methods.push_back(std::move(m));
        }
        // Constructor: NombreDeLaClase(...), con cuerpo o sin el.
        else if (check(Tok::Ident) && peek().text == c.name && peek(1).is(Tok::LParen)) {
            CtorDecl ct;
            ct.loc = advance().loc;
            advance();                                   // '('
            while (!check(Tok::RParen) && !check(Tok::EndOfFile)) {
                ct.params.push_back(parse_param());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "al cerrar los parametros del constructor");

            for (const auto& prev_ct : c.ctors)
                if (prev_ct.params.size() == ct.params.size())
                    diags_.error(ct.loc, "ya hay un constructor de '" + c.name +
                                         "' con " + std::to_string(ct.params.size()) +
                                         " parametro(s); se distinguen por su numero");

            // Sin dos puntos, es el constructor por mapeo automatico.
            if (match(Tok::Colon)) {
                ct.has_body = true;
                ct.body     = parse_block();
            }
            c.ctors.push_back(std::move(ct));
        }
        // Campo: <tipo> <nombre>
        else {
            Field f;
            f.loc  = peek().loc;
            f.type = parse_type();
            if (check(Tok::Ident)) f.name = advance().text;
            else error_here("se esperaba el nombre del campo");

            for (const auto& prev_field : c.fields) {
                if (prev_field.name == f.name) {
                    diags_.error(f.loc, "el campo '" + f.name + "' esta duplicado");
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
        diags_.error(c.loc, "la clase '" + c.name + "' no declara ningun campo");

    out.classes.push_back(std::move(c));
}

// ─── Bloque app: ─────────────────────────────────────────────────────────────

// kind: 0 cadena, 1 numero, 2 booleano.  env("VAR") se resuelve aqui mismo y
// cuenta como cadena; si la variable no existe, queda vacia y el llamante
// decide si eso es un error.
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
        if (!check(Tok::String)) { error_here("env() espera el nombre entre comillas"); return false; }
        Token name_tok = advance();
        const std::string& name = name_tok.text;
        expect(Tok::RParen, "al cerrar env()");
        const char* v = std::getenv(name.c_str());
        // Sin esto, una variable de entorno olvidada se convierte en "" sin
        // que nadie se entere hasta produccion: para session/jwt el secreto
        // vacio falla cerrado (se trata igual que "sin configurar"), pero el
        // error real queda escondido detras de un mensaje que no lo menciona.
        // No es un error de compilacion — .lum no tiene por que conocer el
        // entorno de despliegue final, y --check debe poder correr sin el.
        if (!v) {
            std::cerr << "lumen: aviso: " << (name_tok.loc.file ? *name_tok.loc.file : "?")
                      << ":" << name_tok.loc.line << ":" << name_tok.loc.col
                      << ": la variable de entorno '" << name
                      << "' no esta definida; se usa \"\" en su lugar\n";
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
        diags_.error(loc, "el bloque 'app:' solo puede aparecer una vez en el proyecto");
    }
    out.app.present = true;
    out.app.loc     = loc;

    if (!expect(Tok::Colon, "tras 'app'")) { synchronize(); return; }
    skip_newlines();
    if (!expect(Tok::Indent, "al abrir el bloque app")) { synchronize(); return; }

    while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
        skip_newlines();
        if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;

        const Token& key = peek();

        if (key.is(Tok::KwStatic)) {
            advance();
            StaticMount m;
            m.loc = key.loc;
            if (check(Tok::String)) m.url_prefix = advance().text;
            else error_here("se esperaba el prefijo de URL entre comillas");
            expect(Tok::Arrow, "entre el prefijo y el directorio");
            if (check(Tok::String)) m.fs_root = advance().text;
            else error_here("se esperaba el directorio entre comillas");
            if (match(Tok::KwSpa)) m.spa = true;
            out.app.statics.push_back(std::move(m));
        }
        else if (key.is(Tok::Ident)) {
            std::string k = advance().text;
            if (k == "name" || k == "version" || k == "templates") {
                if (!check(Tok::String)) { error_here("se esperaba una cadena"); }
                else {
                    std::string v = advance().text;
                    if      (k == "name")      out.app.name = v;
                    else if (k == "version")   out.app.version = v;
                    else                       out.app.templates_dir = v;
                }
            } else if (k == "port") {
                if (!check(Tok::Int)) error_here("se esperaba un numero de puerto");
                else {
                    long v = std::strtol(advance().text.c_str(), nullptr, 10);
                    if (v < 1 || v > 65535)
                        diags_.error(prev().loc, "puerto fuera de rango (1-65535)");
                    else out.app.port = static_cast<int>(v);
                }
            } else if (k == "docs")    { out.app.docs    = true; }
            else if   (k == "health")  { out.app.health  = true; }
            else if   (k == "metrics") { out.app.metrics = true; }
            // Bloque de configuracion de un modulo importado.
            else if (out.imports.count(k)) {
                expect(Tok::Colon, "tras el nombre del modulo");
                skip_newlines();
                if (!expect(Tok::Indent, "al abrir la configuracion del modulo")) break;

                auto& opts = out.app.modules[k];
                while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
                    skip_newlines();
                    if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
                    if (!check(Tok::Ident)) { error_here("se esperaba una clave"); advance(); continue; }

                    std::string mk = advance().text;
                    std::string text; long long number = 0; bool flag = false; int kind = -1;
                    if (!config_value(text, number, flag, kind)) {
                        error_here("valor invalido: se esperaba una cadena, un numero, "
                                   "true/false o env(\"VAR\")");
                        while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                               !check(Tok::EndOfFile)) advance();
                        continue;
                    }
                    // Todo se guarda como texto: cada driver interpreta lo suyo.
                    opts[mk] = (kind == 1) ? std::to_string(number)
                             : (kind == 2) ? (flag ? "true" : "false")
                                           : text;
                    skip_newlines();
                }
                match(Tok::Dedent);
            }
            // session: y jwt: son sub-bloques con sus propias claves.
            else if (k == "session" || k == "jwt") {
                bool is_session = (k == "session");
                expect(Tok::Colon, "tras la clave del sub-bloque");
                skip_newlines();
                if (!expect(Tok::Indent, "al abrir el sub-bloque")) break;

                while (!check(Tok::Dedent) && !check(Tok::EndOfFile)) {
                    skip_newlines();
                    if (check(Tok::Dedent) || check(Tok::EndOfFile)) break;
                    if (!check(Tok::Ident)) { error_here("se esperaba una clave"); advance(); continue; }

                    const Token& sub = peek();
                    std::string  sk  = advance().text;
                    std::string  text; long long number = 0; bool flag = false; int kind = -1;
                    if (!config_value(text, number, flag, kind)) {
                        error_here("valor invalido: se esperaba una cadena, un numero, "
                                   "true/false o env(\"VAR\")");
                        while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                               !check(Tok::EndOfFile)) advance();
                        continue;
                    }

                    if (is_session) {
                        if      (sk == "secret"  && kind == 0) out.app.session_secret  = text;
                        else if (sk == "max_age" && kind == 1) out.app.session_max_age = (int)number;
                        else if (sk == "secure"  && kind == 2) out.app.session_secure  = flag;
                        else diags_.error(sub.loc, "clave desconocida o de tipo equivocado "
                                                   "en session: '" + sk + "'");
                    } else {
                        if      (sk == "secret" && kind == 0) out.app.jwt_secret = text;
                        else if (sk == "issuer" && kind == 0) out.app.jwt_issuer = text;
                        else diags_.error(sub.loc, "clave desconocida o de tipo equivocado "
                                                   "en jwt: '" + sk + "'");
                    }
                    skip_newlines();
                }
                match(Tok::Dedent);
            }
            else {
                diags_.error(key.loc, "clave desconocida en el bloque app: '" + k + "'");
                while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                       !check(Tok::EndOfFile)) advance();
            }
        }
        else {
            error_here("se esperaba una clave de configuracion");
            while (!check(Tok::Newline) && !check(Tok::Dedent) &&
                   !check(Tok::EndOfFile)) advance();
        }

        skip_newlines();
    }
    match(Tok::Dedent);
}

// ─── Bloques y sentencias ────────────────────────────────────────────────────

Block Parser::parse_block() {
    Block body;
    skip_newlines();
    if (!expect(Tok::Indent, "al abrir el bloque")) return body;

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

    // Declaracion de variable: <tipo> <ident> [= expr]
    if (looks_like_type()) {
        auto s  = std::make_unique<Stmt>();
        s->kind = StmtKind::VarDecl;
        s->loc  = peek().loc;
        s->type = parse_type();
        if (check(Tok::Ident)) s->name = advance().text;
        else                   error_here("se esperaba el nombre de la variable");
        if (match(Tok::Assign)) s->value = parse_expr();
        return s;
    }

    // Asignacion o expresion suelta.  `x++` es una expresion como cualquier
    // otra: como sentencia, su valor simplemente se descarta.
    SourceLoc loc = peek().loc;
    ExprPtr   e   = parse_expr();

    // x += e  →  x = x + e, y asi con los cinco.
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
            diags_.error(loc, "solo se puede asignar a una variable o a un campo");
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

// Solo una variable o un campo pueden estar a la izquierda de una asignacion.
bool Parser::assignable(const Expr& e) {
    return e.kind == ExprKind::Ident || e.kind == ExprKind::Member ||
           e.kind == ExprKind::Index;
}

// Copia el lado izquierdo para poder leerlo y escribirlo en la misma sentencia.
// Con Ident y Member basta una copia superficial del receptor.
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
    expect(Tok::Colon, "tras la condicion del if");
    s->body = parse_block();

    skip_newlines();
    if (check(Tok::KwElif)) {
        // `elif` encadena: el else contiene un If completo.  `else if` hace lo
        // mismo, y se admiten los dos.
        s->orelse.push_back(parse_if_from_elif());
    }
    else if (check(Tok::KwElse)) {
        advance();
        if (check(Tok::KwIf)) {
            s->orelse.push_back(parse_if());
        } else {
            expect(Tok::Colon, "tras 'else'");
            s->orelse = parse_block();
        }
    }
    return s;
}

// Un `elif` es un `if` cuyo encabezado ya se consumio.
StmtPtr Parser::parse_if_from_elif() {
    auto s   = std::make_unique<Stmt>();
    s->kind  = StmtKind::If;
    s->loc   = advance().loc;                 // 'elif'
    s->value = parse_expr();
    expect(Tok::Colon, "tras la condicion del elif");
    s->body = parse_block();

    skip_newlines();
    if (check(Tok::KwElif)) {
        s->orelse.push_back(parse_if_from_elif());
    } else if (check(Tok::KwElse)) {
        advance();
        if (check(Tok::KwIf)) s->orelse.push_back(parse_if());
        else {
            expect(Tok::Colon, "tras 'else'");
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
    expect(Tok::Colon, "tras la condicion del while");
    s->body = parse_block();
    return s;
}

StmtPtr Parser::parse_for() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::For;
    s->loc  = advance().loc;
    s->type = parse_type();
    if (check(Tok::Ident)) s->name = advance().text;
    else                   error_here("se esperaba el nombre de la variable del bucle");
    expect(Tok::KwIn, "en el bucle for");
    s->target = parse_expr();
    expect(Tok::Colon, "tras el iterable del for");
    s->body = parse_block();
    return s;
}

StmtPtr Parser::parse_require() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::Require;
    s->loc  = advance().loc;
    s->value = parse_expr();
    if (!expect(Tok::KwElse, "en 'require ... else ...'")) return s;
    s->target = parse_expr();
    return s;
}

StmtPtr Parser::parse_try() {
    auto s  = std::make_unique<Stmt>();
    s->kind = StmtKind::Try;
    s->loc  = advance().loc;
    expect(Tok::Colon, "tras 'try'");
    s->body = parse_block();
    skip_newlines();
    if (!expect(Tok::KwCatch, "tras el bloque try")) return s;
    if (check(Tok::Ident)) s->name = advance().text;
    expect(Tok::Colon, "tras 'catch'");
    s->orelse = parse_block();
    return s;
}

// ─── Tipos ───────────────────────────────────────────────────────────────────

// Distingue `List<string> xs = ...` (declaracion) de `a < b` (expresion).
// Un tipo es: un identificador o palabra de tipo, opcionalmente con <...> y
// '?', seguido de un identificador.
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
        error_here("se esperaba un tipo");
        return t;
    }
    t.name = advance().text;

    if (match(Tok::Lt)) {
        do {
            t.args.push_back(parse_type());
        } while (match(Tok::Comma));
        // El lexer nunca fusiona '>>', asi que cada nivel cierra con su propio Gt.
        expect(Tok::Gt, "al cerrar los parametros del tipo");
    }
    if (match(Tok::Question)) t.optional = true;
    return t;
}

// ─── Expresiones ─────────────────────────────────────────────────────────────

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
    expect(Tok::Colon, "en el operador ternario");
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
    // ++x / --x : incrementa y da el valor YA incrementado.
    if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
        bool      up  = peek().is(Tok::PlusPlus);
        SourceLoc loc = advance().loc;
        auto e  = make(ExprKind::PreStep, loc);
        e->text = up ? "+" : "-";
        e->lhs  = parse_unary();
        if (e->lhs && !assignable(*e->lhs))
            diags_.error(loc, "'++' y '--' solo se aplican a una variable o a un campo");
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
        // x++ / x-- : incrementa y da el valor ANTERIOR.
        if (check(Tok::PlusPlus) || check(Tok::MinusMinus)) {
            bool      up  = peek().is(Tok::PlusPlus);
            SourceLoc loc = advance().loc;
            if (e && !assignable(*e))
                diags_.error(loc, "'++' y '--' solo se aplican a una variable o a un campo");
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
            // Tras un punto, una palabra reservada es solo un nombre: `state.get`
            // y `log.error` no tienen por que chocar con `get` y `error`.
            if (check(Tok::Ident) || !peek().text.empty()) m->text = advance().text;
            else error_here("se esperaba un nombre tras '.'");
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
                // Argumento con nombre: IDENT '=' expr
                if (check(Tok::Ident) && peek(1).is(Tok::Assign)) {
                    a.name = advance().text;
                    advance();
                    seen_named = true;
                } else if (seen_named) {
                    error_here("los argumentos posicionales van antes que los nombrados");
                }
                a.value = parse_expr();
                c->args.push_back(std::move(a));
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RParen, "al cerrar la llamada");
            e = std::move(c);
        }
        else if (check(Tok::LBracket)) {
            SourceLoc loc = advance().loc;
            auto ix = make(ExprKind::Index, loc);
            ix->object = std::move(e);
            ix->lhs    = parse_expr();
            expect(Tok::RBracket, "al cerrar el indice");
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

        // Palabras que son a la vez clave y objeto reservado dentro de un
        // handler: `sse.send(...)`, `ws.recv()`, `error.message`.
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
            expect(Tok::RParen, "al cerrar el parentesis");
            return inner;
        }

        case Tok::LBracket: {
            auto e = make(ExprKind::ListLit, advance().loc);
            while (!check(Tok::RBracket) && !check(Tok::EndOfFile)) {
                e->items.push_back(parse_expr());
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RBracket, "al cerrar la lista");
            return e;
        }

        case Tok::LBrace: {
            auto e = make(ExprKind::DictLit, advance().loc);
            while (!check(Tok::RBrace) && !check(Tok::EndOfFile)) {
                DictEntry entry;
                entry.key = parse_expr();
                expect(Tok::Colon, "entre la clave y el valor del diccionario");
                entry.value = parse_expr();
                e->entries.push_back(std::move(entry));
                if (!match(Tok::Comma)) break;
            }
            expect(Tok::RBrace, "al cerrar el diccionario");
            return e;
        }

        default: {
            error_here(std::string("se esperaba una expresion, pero hay '") +
                       tok_name(t.kind) + "'");
            auto e = make(ExprKind::NullLit, t.loc);
            advance();
            return e;
        }
    }
}

} // namespace lumen_script
