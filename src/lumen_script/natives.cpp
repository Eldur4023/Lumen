#include <lumen_script/natives.hpp>
#include <lumen_script/plantilla.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>
#include <lumen/sse.hpp>
#include <lumen/websocket.hpp>
#include <lumen/logger.hpp>
#include <lumen/multipart.hpp>

#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace lumen_script {

namespace {

Value fn_text(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.res.text(args[0].to_string());
    ctx.response_written = true;
    return Value::null();
}

Value fn_html(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.res.html(args[0].to_string());
    ctx.response_written = true;
    return Value::null();
}

Value fn_json(NativeCtx& ctx, std::vector<Value>& args, std::string&) {
    ctx.res.header("Content-Type", "application/json; charset=utf-8")
           .send(args[0].to_json_text());
    ctx.response_written = true;
    return Value::null();
}

// Renderiza una plantilla ya compilada.  El primer argumento es su indice en
// la tabla del modulo, que puso el emisor; el segundo, las variables.
//
// Los valores se colocan en el mismo orden en que se compilaron: la plantilla
// guarda sus nombres, asi que aqui no se busca nada por cadena en caliente.
// render() nunca llega a ejecutarse: el emisor SIEMPRE lo reescribe a
// __render_tpl con la plantilla ya compilada.  Sigue en la tabla porque el
// emisor busca ahi para saber que existe y cuantos argumentos admite.
Value fn_render(NativeCtx&, std::vector<Value>&, std::string& error) {
    error = "render(): la plantilla no se compilo al arrancar";
    return Value::null();
}

Value fn_render_tpl(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.plantillas || !args[0].is_int()) {
        error = "render(): plantilla no compilada";
        return Value::null();
    }
    const size_t idx = static_cast<size_t>(args[0].as_int());
    if (idx >= ctx.plantillas->size()) {
        error = "render(): indice de plantilla fuera de rango";
        return Value::null();
    }
    const Plantilla& p = (*ctx.plantillas)[idx];

    std::vector<Value> valores;
    valores.reserve(p.nombres.size());
    const bool hay = args.size() > 1 && args[1].is_dict();
    for (const auto& n : p.nombres) {
        if (!hay) { valores.push_back(Value::null()); continue; }
        auto it = args[1].as_dict().find(n.nombre);
        valores.push_back(it == args[1].as_dict().end() ? Value::null() : it->second);
    }

    std::string salida;
    if (!render_plantilla(p, std::move(valores), ctx, ctx.funciones, salida, error))
        return Value::null();

    ctx.res.header("Content-Type", "text/html; charset=utf-8").send(std::move(salida));
    ctx.response_written = true;
    return Value::null();
}

Value fn_status(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) {
        error = "status() espera un codigo entero";
        return Value::null();
    }
    ctx.res.status(static_cast<int>(args[0].as_int())).send("");
    ctx.response_written = true;
    return Value::null();
}

Value fn_redirect(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) {
        error = "redirect() espera el destino como string";
        return Value::null();
    }
    int code = 302;
    if (args.size() > 1) {
        if (!args[1].is_int()) {
            error = "el segundo argumento de redirect() es el codigo";
            return Value::null();
        }
        code = static_cast<int>(args[1].as_int());
    }
    ctx.res.status(code).header("Location", args[0].as_str()).send("");
    ctx.response_written = true;
    return Value::null();
}

Value fn_send_file(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) {
        error = "send_file() espera una ruta como string";
        return Value::null();
    }
    ctx.res.send_file(args[0].as_str());
    ctx.response_written = true;
    return Value::null();
}

Value fn_len(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_str())  return Value::integer((long long)v.as_str().size());
    if (v.is_list()) return Value::integer((long long)v.as_list().size());
    if (v.is_dict()) return Value::integer((long long)v.as_dict().size());
    error = std::string("len() no aplica a ") + v.type_name();
    return Value::null();
}

Value fn_str(NativeCtx&, std::vector<Value>& args, std::string&) {
    return Value::str(args[0].to_string());
}

Value fn_int(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_int())   return v;
    if (v.is_float()) return Value::integer((long long)v.as_float());
    if (v.is_bool())  return Value::integer(v.as_bool() ? 1 : 0);
    if (v.is_str()) {
        try { return Value::integer(std::stoll(v.as_str())); }
        catch (...) { error = "int(): '" + v.as_str() + "' no es un numero"; }
        return Value::null();
    }
    error = std::string("int() no aplica a ") + v.type_name();
    return Value::null();
}

Value fn_header(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "header() espera el nombre como string"; return Value::null(); }
    auto h = ctx.req.header(args[0].as_str());
    if (!h) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(*h);
}

Value fn_query(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "query() espera el nombre como string"; return Value::null(); }
    auto it = ctx.req.query.find(args[0].as_str());
    if (it == ctx.req.query.end()) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(it->second);
}

// ─── sse.* ───────────────────────────────────────────────────────────────────
// El objeto `sse` solo existe dentro de una ruta sse; fuera, ctx.sse es nulo y
// el builtin lo dice en vez de reventar.

bool need_sse(NativeCtx& ctx, std::string& error, const char* what) {
    if (ctx.sse) return true;
    error = std::string("'sse.") + what + "' solo existe dentro de una ruta sse";
    return false;
}

// sse.send(datos)
// sse.send(evento, datos)
// sse.send(evento, datos, id)     ← el id permite al navegador reconectar
Value fn_sse_send(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_sse(ctx, error, "send")) return Value::null();

    if (args.size() == 1) return Value::boolean(ctx.sse->send(args[0].to_string()));

    if (!args[0].is_str()) {
        error = "el nombre del evento tiene que ser string";
        return Value::null();
    }
    std::string id = args.size() > 2 ? args[2].to_string() : std::string();
    return Value::boolean(ctx.sse->send_event(args[0].as_str(),
                                              args[1].to_string(), id));
}

Value fn_sse_ping(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_sse(ctx, error, "ping")) return Value::null();
    return Value::boolean(ctx.sse->ping(args.empty() ? "" : args[0].to_string()));
}

Value fn_sse_open(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!need_sse(ctx, error, "open")) return Value::null();
    return Value::boolean(ctx.sse->is_open());
}

// ─── ws.* ────────────────────────────────────────────────────────────────────

bool need_ws(NativeCtx& ctx, std::string& error, const char* what) {
    if (ctx.ws) return true;
    error = std::string("'ws.") + what + "' solo existe dentro de una ruta ws";
    return false;
}

Value fn_ws_send(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_ws(ctx, error, "send")) return Value::null();
    ctx.ws->send(args[0].to_string());
    return Value::null();
}

Value fn_ws_open(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!need_ws(ctx, error, "open")) return Value::null();
    return Value::boolean(ctx.ws->is_open());
}

Value fn_ws_close(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!need_ws(ctx, error, "close")) return Value::null();
    ctx.ws->close();
    return Value::null();
}

// ─── session.* / jwt.* ───────────────────────────────────────────────────────

Value fn_session_get(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "la sesion no esta configurada: falta 'session: secret ...' en app:";
        return Value::null();
    }
    auto it = ctx.session->data.find(args[0].as_str());
    return it == ctx.session->data.end() ? Value::null() : it->second;
}

Value fn_session_set(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "la sesion no esta configurada: falta 'session: secret ...' en app:";
        return Value::null();
    }
    ctx.session->data[args[0].as_str()] = args[1];
    ctx.session->dirty = true;
    return args[1];
}

Value fn_session_clear(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "la sesion no esta configurada: falta 'session: secret ...' en app:";
        return Value::null();
    }
    ctx.session->data.clear();
    ctx.session->dirty = true;
    return Value::null();
}

Value fn_jwt_valid(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::boolean(ctx.jwt_ok);
}

Value fn_jwt_claims(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    if (!ctx.jwt_ok || !ctx.jwt_claims) return Value::dict();
    return *ctx.jwt_claims;
}

// ─── state.* ─────────────────────────────────────────────────────────────────

Value fn_state_incr(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.incr() espera la clave como string"; return Value::null(); }
    long long by = 1;
    if (args.size() > 1) {
        if (!args[1].is_int()) { error = "state.incr() espera un entero"; return Value::null(); }
        by = args[1].as_int();
    }
    return Value::integer(SharedState::instance().incr(args[0].as_str(), by));
}

Value fn_state_decr(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.decr() espera la clave como string"; return Value::null(); }
    long long by = args.size() > 1 && args[1].is_int() ? args[1].as_int() : 1;
    return Value::integer(SharedState::instance().incr(args[0].as_str(), -by));
}

Value fn_state_get(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.get() espera la clave como string"; return Value::null(); }
    Value v = SharedState::instance().get(args[0].as_str());
    if (v.is_null() && args.size() > 1) return args[1];
    return v;
}

Value fn_state_set(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.set() espera la clave como string"; return Value::null(); }
    SharedState::instance().set(args[0].as_str(), args[1]);
    return args[1];
}

Value fn_state_remove(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.remove() espera la clave como string"; return Value::null(); }
    return Value::boolean(SharedState::instance().remove(args[0].as_str()));
}

// ─── log.* / cookie / form ───────────────────────────────────────────────────

Value fn_log_info(NativeCtx&, std::vector<Value>& args, std::string&) {
    lumen::log().info(args[0].to_string());
    return Value::null();
}
Value fn_log_warn(NativeCtx&, std::vector<Value>& args, std::string&) {
    lumen::log().warn(args[0].to_string());
    return Value::null();
}
Value fn_log_error(NativeCtx&, std::vector<Value>& args, std::string&) {
    lumen::log().error(args[0].to_string());
    return Value::null();
}

Value fn_cookie(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "cookie() espera el nombre como string"; return Value::null(); }
    auto c = ctx.req.cookie(args[0].as_str());
    if (!c) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(*c);
}

Value fn_form(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "form() espera el nombre como string"; return Value::null(); }
    auto f  = ctx.req.form();
    auto it = f.find(args[0].as_str());
    if (it == f.end()) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(it->second);
}

// ─── error.* / request.* ─────────────────────────────────────────────────────

Value fn_error_code(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::integer(ctx.error_code);
}

Value fn_error_message(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::str(ctx.error_message);
}

// Lista vacia y no null cuando el error no viene de validar: asi recorrerla con
// `for` siempre funciona.
Value fn_error_messages(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    Value::List out;
    if (ctx.error_messages)
        for (const auto& m : *ctx.error_messages) out.push_back(Value::str(m));
    return Value::list(std::move(out));
}

Value fn_req_path(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::str(ctx.req.path);
}

Value fn_req_method(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::str(ctx.req.method);
}

Value fn_req_ip(NativeCtx& ctx, std::vector<Value>&, std::string&) {
    return Value::str(ctx.req.remote_ip);
}

const std::array<NativeDef, 49> kNatives = {{
    // Respuesta
    {"text",      1, 1,  fn_text},
    {"html",      1, 1,  fn_html},
    {"json",      1, 1,  fn_json},
    {"render",    1, 2,  fn_render},
    {"__render_tpl", 2, 2, fn_render_tpl},
    {"status",    1, 1,  fn_status},
    {"redirect",  1, 2,  fn_redirect},
    {"send_file", 1, 1,  fn_send_file},
    // Utilidades
    {"len",       1, 1,  fn_len},
    {"str",       1, 1,  fn_str},
    {"int",       1, 1,  fn_int},
    // Peticion
    {"header",    1, 2,  fn_header},
    {"query",     1, 2,  fn_query},
    // sse.* — no se escriben asi en Lumen Script: se llega por member_native_id().
    {"__sse_send", 1, 3,  fn_sse_send},
    {"__sse_ping", 0, 1,  fn_sse_ping},
    {"__sse_open", 0, 0,  fn_sse_open},
    // ws.* — igual que sse.*, se llega por member_native_id().
    {"__ws_send",  1, 1,  fn_ws_send},
    {"__ws_open",  0, 0,  fn_ws_open},
    {"__ws_close", 0, 0,  fn_ws_close},
    // session.* / jwt.* — se llega por member_native_id() o, en el caso de
    // session, por el acceso a un campo cualquiera.
    {"__session_get",   1, 1,  fn_session_get},
    {"__session_set",   2, 2,  fn_session_set},
    {"__session_clear", 0, 0,  fn_session_clear},
    {"__jwt_valid",     0, 0,  fn_jwt_valid},
    {"__jwt_claims",    0, 0,  fn_jwt_claims},
    {"__error_code",    0, 0,  fn_error_code},
    {"__error_message",  0, 0, fn_error_message},
    {"__error_messages", 0, 0, fn_error_messages},
    {"__req_path",      0, 0,  fn_req_path},
    {"__req_method",    0, 0,  fn_req_method},
    {"__req_ip",        0, 0,  fn_req_ip},
    {"__state_incr",    1, 2,  fn_state_incr},
    {"__state_decr",    1, 2,  fn_state_decr},
    {"__state_get",     1, 2,  fn_state_get},
    {"__state_set",     2, 2,  fn_state_set},
    {"__state_remove",  1, 1,  fn_state_remove},
    {"__log_info",      1, 1,  fn_log_info},
    {"__log_warn",      1, 1,  fn_log_warn},
    {"__log_error",     1, 1,  fn_log_error},
    {"cookie",          1, 2,  fn_cookie},
    {"form",            1, 2,  fn_form},
    // Asincronos: sin fn, los resuelve el driver del handler.
    {"sleep",     1, 1,  nullptr, true},
    {"__ws_recv", 0, 0,  nullptr, true},
    // Base de datos: el primer argumento es el nombre del modulo, que el
    // emisor apila; asi un unico builtin sirve para los tres.
    {"__db_query", 2, -1, nullptr, true},
    {"__db_exec",  2, -1, nullptr, true},
    {"__db_begin",    1, 1, nullptr, true},
    {"__db_commit",   1, 1, nullptr, true},
    {"__db_rollback", 1, 1, nullptr, true},
    {"__db_last_id",  1, 1, nullptr, true},
    // Marcador final para que native_count() no dependa del orden.
    {nullptr,     0, 0,  nullptr},
}};

} // namespace

std::vector<std::string>& last_validation_messages() {
    thread_local std::vector<std::string> msgs;
    return msgs;
}

SharedState& SharedState::instance() {
    static SharedState s;
    return s;
}

long long SharedState::incr(const std::string& key, long long by) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto&     v   = data_[key];
    long long cur = v.is_int() ? v.as_int() : 0;
    long long out = cur + by;
    v = Value::integer(out);
    return out;
}

Value SharedState::get(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = data_.find(key);
    return it == data_.end() ? Value::null() : it->second;
}

void SharedState::set(const std::string& key, Value v) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_[key] = std::move(v);
}

bool SharedState::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.erase(key) > 0;
}

int native_id(const std::string& name) {
    for (size_t i = 0; i + 1 < kNatives.size(); ++i)
        if (name == kNatives[i].name) return static_cast<int>(i);
    return -1;
}

const NativeDef& native_at(int id) { return kNatives[static_cast<size_t>(id)]; }

int native_count() { return static_cast<int>(kNatives.size()) - 1; }

// ─── Metodos sobre valores ───────────────────────────────────────────────────

namespace {

bool want(size_t got, size_t min, size_t max, const std::string& name,
          std::string& error) {
    if (got >= min && got <= max) return true;
    error = "'" + name + "()' recibe un numero de argumentos que no admite";
    return false;
}

// Guarda una parte subida quedandose solo con el nombre de fichero, sin ruta:
// asi un filename con ".." o absoluto no puede escapar del directorio.
std::string safe_name(const std::string& raw) {
    size_t slash = raw.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
    if (base.empty() || base == "." || base == "..") base = "subida";
    return base;
}

} // namespace

Value call_method(NativeCtx& ctx, Value& recv, const std::string& name,
                  std::vector<Value>& args, std::string& error) {
    // ── Modificadores de respuesta ───────────────────────────────────────────
    // Se encadenan sobre lo que se devuelve —`return {...}.status(201)`— y
    // dejan pasar el valor, para no reintroducir un objeto `response` mutable.
    if (name == "status") {
        if (args.size() != 1 || !args[0].is_int()) {
            error = "status() espera un codigo entero";
            return Value::null();
        }
        ctx.res.status(static_cast<int>(args[0].as_int()));
        return recv;
    }
    if (name == "header") {
        if (args.size() != 2 || !args[0].is_str()) {
            error = "header() espera nombre y valor";
            return Value::null();
        }
        ctx.res.header(args[0].as_str(), args[1].to_string());
        return recv;
    }
    if (name == "cookie") {
        if (args.size() < 2 || !args[0].is_str()) {
            error = "cookie() espera al menos nombre y valor";
            return Value::null();
        }
        lumen::CookieOptions opts;
        opts.path      = "/";
        opts.http_only = true;
        opts.same_site = lumen::SameSite::Lax;

        // Tercer hueco: las opciones con nombre, agrupadas por el emisor.
        if (args.size() > 2 && args[2].is_dict()) {
            const auto& o = args[2].as_dict();
            auto pick = [&o](const char* k) -> const Value* {
                auto it = o.find(k);
                return it == o.end() ? nullptr : &it->second;
            };
            if (auto* v = pick("max_age");   v && v->is_int())  opts.max_age   = (int)v->as_int();
            if (auto* v = pick("path");      v && v->is_str())  opts.path      = v->as_str();
            if (auto* v = pick("domain");    v && v->is_str())  opts.domain    = v->as_str();
            if (auto* v = pick("secure");    v && v->is_bool()) opts.secure    = v->as_bool();
            if (auto* v = pick("http_only"); v && v->is_bool()) opts.http_only = v->as_bool();
            if (auto* v = pick("same_site"); v && v->is_str()) {
                const std::string& ss = v->as_str();
                if      (ss == "strict") opts.same_site = lumen::SameSite::Strict;
                else if (ss == "none")   opts.same_site = lumen::SameSite::None;
                else                     opts.same_site = lumen::SameSite::Lax;
            }
        }
        ctx.res.cookie(args[0].as_str(), args[1].to_string(), std::move(opts));
        return recv;
    }

    // ── string ───────────────────────────────────────────────────────────────
    if (recv.is_str()) {
        const std::string& s = recv.as_str();
        if (name == "starts_with" || name == "ends_with" || name == "contains") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "'" + name + "()' espera un string"; return Value::null(); }
            const std::string& n = args[0].as_str();
            if (name == "starts_with") return Value::boolean(s.rfind(n, 0) == 0);
            if (name == "ends_with")
                return Value::boolean(s.size() >= n.size() &&
                                      s.compare(s.size() - n.size(), n.size(), n) == 0);
            return Value::boolean(s.find(n) != std::string::npos);
        }
        if (name == "upper" || name == "lower") {
            std::string out = s;
            for (char& c : out) c = static_cast<char>(name == "upper" ? ::toupper((unsigned char)c)
                                                                     : ::tolower((unsigned char)c));
            return Value::str(std::move(out));
        }
        if (name == "trim") {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return Value::str("");
            size_t b = s.find_last_not_of(" \t\r\n");
            return Value::str(s.substr(a, b - a + 1));
        }
        error = "los string no tienen el metodo '" + name + "'";
        return Value::null();
    }

    // ── List ─────────────────────────────────────────────────────────────────
    if (recv.is_list()) {
        if (name == "add") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            recv.as_list().push_back(args[0]);
            return recv;
        }
        error = "las List no tienen el metodo '" + name + "'";
        return Value::null();
    }

    // ── Dict, incluido File ──────────────────────────────────────────────────
    if (recv.is_dict()) {
        auto& d = recv.as_dict();

        if (name == "save") {
            auto idx = d.find("__idx");
            if (idx == d.end() || !ctx.parts) {
                error = "save() solo existe sobre un File subido";
                return Value::null();
            }
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "save() espera el directorio como string"; return Value::null(); }

            size_t i = static_cast<size_t>(idx->second.as_int());
            if (!ctx.parts || i >= ctx.parts->size()) {
                error = "el fichero subido ya no esta disponible";
                return Value::null();
            }

            auto  fn   = d.find("filename");
            std::string base = safe_name(fn == d.end() ? "" : fn->second.to_string());
            std::string dir  = args[0].as_str();
            if (!dir.empty() && dir.back() != '/') dir += "/";

            std::error_code ec;
            std::filesystem::create_directories(dir, ec);

            std::ofstream out(dir + base, std::ios::binary);
            if (!out) { error = "no se puede escribir en " + dir + base; return Value::null(); }
            const std::string& bytes = (*ctx.parts)[i].body;
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) { error = "fallo al escribir " + dir + base; return Value::null(); }
            return Value::str(base);
        }

        if (name == "has") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            return Value::boolean(d.count(args[0].to_string()) > 0);
        }
        if (name == "keys") {
            Value::List ks;
            for (const auto& [k, _] : d) if (k.rfind("__", 0) != 0) ks.push_back(Value::str(k));
            return Value::list(std::move(ks));
        }
        error = "los Dict no tienen el metodo '" + name + "'";
        return Value::null();
    }

    error = std::string("los valores de tipo ") + recv.type_name() +
            " no tienen metodos";
    return Value::null();
}

// La cara de compilacion de call_method().
//
// Lo que el VM rechazaria al ejecutar se puede rechazar antes si el tipo del
// receptor se conoce: `nombre.mayusculas()` sobre un string es una errata, y
// una errata no deberia esperar a que alguien pida la pagina para salir.
//
// Si se anade un metodo arriba hay que anadirlo aqui: son la misma lista vista
// desde los dos lados.
const std::vector<MetodoBuiltin>* metodos_de(const std::string& tipo) {
    // Los tres modificadores de respuesta se encadenan sobre CUALQUIER valor
    // —`return {...}.status(201)`—, asi que aparecen en todas las listas.
    static const std::vector<MetodoBuiltin> kComunes = {
        {"status", 1, 1, nullptr}, {"header", 2, 2, nullptr}, {"cookie", 2, 3, nullptr},
    };
    static const auto con = [](std::initializer_list<MetodoBuiltin> propios) {
        std::vector<MetodoBuiltin> v = kComunes;
        v.insert(v.end(), propios);
        return v;
    };

    static const std::vector<MetodoBuiltin> kString = con({
        {"starts_with", 1, 1, "bool"}, {"ends_with", 1, 1, "bool"},
        {"contains", 1, 1, "bool"},    {"upper", 0, 0, "string"},
        {"lower", 0, 0, "string"},     {"trim", 0, 0, "string"},
    });
    static const std::vector<MetodoBuiltin> kList = con({{"add", 1, 1, nullptr}});
    static const std::vector<MetodoBuiltin> kDict = con({
        {"has", 1, 1, "bool"}, {"keys", 0, 0, "List"}, {"save", 1, 1, "string"},
    });

    if (tipo == "string") return &kString;
    if (tipo == "List")   return &kList;
    if (tipo == "Dict")   return &kDict;
    // Los numeros y los bool no tienen nada propio: solo lo comun.
    if (tipo == "int" || tipo == "long" || tipo == "float" ||
        tipo == "double" || tipo == "bool")
        return &kComunes;
    return nullptr;
}

namespace {
struct MemberMap { const char* object; const char* member; const char* native; };

const std::array<MemberMap, 42> kMembers = {{
    {"sse", "send",  "__sse_send"},
    {"sse", "ping",  "__sse_ping"},
    {"sse", "open",  "__sse_open"},
    {"ws",  "send",  "__ws_send"},
    {"ws",  "open",  "__ws_open"},
    {"ws",  "close", "__ws_close"},
    {"ws",  "recv",  "__ws_recv"},
    {"session", "clear",  "__session_clear"},
    {"jwt",     "valid",  "__jwt_valid"},
    {"jwt",     "claims", "__jwt_claims"},
    {"error",   "code",    "__error_code"},
    {"error",   "message",  "__error_message"},
    {"error",   "messages", "__error_messages"},
    {"sqlite",   "query", "__db_query"},
    {"sqlite",   "exec",  "__db_exec"},
    {"postgres", "query", "__db_query"},
    {"postgres", "exec",  "__db_exec"},
    {"mysql",    "query", "__db_query"},
    {"mysql",    "exec",  "__db_exec"},
    {"sqlite",   "begin",    "__db_begin"},
    {"sqlite",   "commit",   "__db_commit"},
    {"sqlite",   "rollback", "__db_rollback"},
    {"sqlite",   "last_id",  "__db_last_id"},
    {"postgres", "begin",    "__db_begin"},
    {"postgres", "commit",   "__db_commit"},
    {"postgres", "rollback", "__db_rollback"},
    {"postgres", "last_id",  "__db_last_id"},
    {"mysql",    "begin",    "__db_begin"},
    {"mysql",    "commit",   "__db_commit"},
    {"mysql",    "rollback", "__db_rollback"},
    {"mysql",    "last_id",  "__db_last_id"},
    {"request", "path",    "__req_path"},
    {"request", "method",  "__req_method"},
    {"request", "ip",      "__req_ip"},
    {"state",   "incr",    "__state_incr"},
    {"state",   "decr",    "__state_decr"},
    {"state",   "get",     "__state_get"},
    {"state",   "set",     "__state_set"},
    {"state",   "remove",  "__state_remove"},
    {"log",     "info",    "__log_info"},
    {"log",     "warn",    "__log_warn"},
    {"log",     "error",   "__log_error"},
}};
} // namespace

int member_native_id(const std::string& object, const std::string& member) {
    for (const auto& m : kMembers)
        if (object == m.object && member == m.member) return native_id(m.native);
    return -1;
}

bool is_reserved_object(const std::string& name) {
    for (const auto& m : kMembers) if (name == m.object) return true;
    return false;
}

bool is_db_module(const std::string& name) {
    return name == "sqlite" || name == "postgres" || name == "mysql";
}

} // namespace lumen_script
