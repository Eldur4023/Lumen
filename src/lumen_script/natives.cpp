#include <lumen_script/natives.hpp>
#include <lumen_script/template.hpp>

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

// Renders an already compiled template.  The first argument is its index in
// the module table, placed there by the emitter; the second, the variables.
//
// The values are placed in the same order they were compiled in: the template
// keeps its names, so nothing is looked up by string on the hot path.
// render() never actually runs: the emitter ALWAYS rewrites it to __render_tpl
// with the template already compiled.  It stays in the table because the
// emitter looks there to know it exists and how many arguments it takes.
Value fn_render(NativeCtx&, std::vector<Value>&, std::string& error) {
    error = "render(): the template was not compiled at startup";
    return Value::null();
}

Value fn_render_tpl(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.templates || !args[0].is_int()) {
        error = "render(): template not compiled";
        return Value::null();
    }
    const size_t idx = static_cast<size_t>(args[0].as_int());
    if (idx >= ctx.templates->size()) {
        error = "render(): template index out of range";
        return Value::null();
    }
    const Template& p = (*ctx.templates)[idx];

    std::vector<Value> values;
    values.reserve(p.names.size());
    const bool present = args.size() > 1 && args[1].is_dict();
    for (const auto& n : p.names) {
        if (!present) { values.push_back(Value::null()); continue; }
        auto it = args[1].as_dict().find(n.name);
        values.push_back(it == args[1].as_dict().end() ? Value::null() : it->second);
    }

    std::string out;
    if (!render_plantilla(p, std::move(values), ctx, ctx.functions, out, error))
        return Value::null();

    ctx.res.header("Content-Type", "text/html; charset=utf-8").send(std::move(out));
    ctx.response_written = true;
    return Value::null();
}

Value fn_status(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_int()) {
        error = "status() expects an integer status code";
        return Value::null();
    }
    ctx.res.status(static_cast<int>(args[0].as_int())).send("");
    ctx.response_written = true;
    return Value::null();
}

// WARNING: redirect() sends its argument verbatim as the Location header —
// including a full external URL, which is intentional (an OAuth callback, a
// payment gateway return, a link to another site are all legitimate uses a
// framework primitive cannot tell apart from an attacker's ?next=). It is
// safe with a literal or with a value validated against an allowlist; it is
// an open redirect if the target comes straight from request input
// (query()/param()/a form field) with nothing checked — do that validation
// in the handler, the same way the grammar guide already flags for
// send_file(path) taking unsanitised input.
Value fn_redirect(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) {
        error = "redirect() expects the target as a string";
        return Value::null();
    }
    int code = 302;
    if (args.size() > 1) {
        if (!args[1].is_int()) {
            error = "the second argument of redirect() is the status code";
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
        error = "send_file() expects a path as a string";
        return Value::null();
    }
    if (args.size() == 2) {
        // Two-argument form: send_file(path, root) confines `path` inside
        // `root` — symlinks and ".." are resolved and checked, so a path
        // built from request input (query(), a path param, a form field)
        // cannot escape it.  This is the ONLY safe way to pass user input
        // to send_file(): the one-argument form trusts the caller the same
        // way a hardcoded literal does, and must never see unsanitised
        // input (see the LFI note on Response::send_file in response.hpp).
        if (!args[1].is_str()) {
            error = "the second argument of send_file() is the root directory";
            return Value::null();
        }
        ctx.res.serve_file_from(args[1].as_str(), args[0].as_str());
    } else {
        ctx.res.send_file(args[0].as_str());
    }
    ctx.response_written = true;
    return Value::null();
}

Value fn_len(NativeCtx&, std::vector<Value>& args, std::string& error) {
    const Value& v = args[0];
    if (v.is_str())  return Value::integer((long long)v.as_str().size());
    if (v.is_list()) return Value::integer((long long)v.as_list().size());
    if (v.is_dict()) return Value::integer((long long)v.as_dict().size());
    error = std::string("len() does not apply to ") + v.type_name();
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
        catch (...) { error = "int(): '" + v.as_str() + "' is not a number"; }
        return Value::null();
    }
    error = std::string("int() does not apply to ") + v.type_name();
    return Value::null();
}

Value fn_header(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "header() expects the name as a string"; return Value::null(); }
    auto h = ctx.req.header(args[0].as_str());
    if (!h) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(*h);
}

Value fn_query(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "query() expects the name as a string"; return Value::null(); }
    auto it = ctx.req.query.find(args[0].as_str());
    if (it == ctx.req.query.end()) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(it->second);
}

// ─── sse.* ───────────────────────────────────────────────────────────────────
// The `sse` object only exists inside an sse route; outside, ctx.sse is null
// and the builtin says so instead of blowing up.

bool need_sse(NativeCtx& ctx, std::string& error, const char* what) {
    if (ctx.sse) return true;
    error = std::string("'sse.") + what + "' only exists inside an sse route";
    return false;
}

// sse.send(data)
// sse.send(event, data)
// sse.send(event, data, id)     ← the id lets the browser reconnect
Value fn_sse_send(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!need_sse(ctx, error, "send")) return Value::null();

    if (args.size() == 1) return Value::boolean(ctx.sse->send(args[0].to_string()));

    if (!args[0].is_str()) {
        error = "the event name must be a string";
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
    error = std::string("'ws.") + what + "' only exists inside a ws route";
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
        error = "the session is not configured: missing 'session: secret ...' in app:";
        return Value::null();
    }
    auto it = ctx.session->data.find(args[0].as_str());
    return it == ctx.session->data.end() ? Value::null() : it->second;
}

Value fn_session_set(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "the session is not configured: missing 'session: secret ...' in app:";
        return Value::null();
    }
    ctx.session->data[args[0].as_str()] = args[1];
    ctx.session->dirty = true;
    return args[1];
}

Value fn_session_clear(NativeCtx& ctx, std::vector<Value>&, std::string& error) {
    if (!ctx.session || ctx.session->secret.empty()) {
        error = "the session is not configured: missing 'session: secret ...' in app:";
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
    if (!args[0].is_str()) { error = "state.incr() expects the key as a string"; return Value::null(); }
    long long by = 1;
    if (args.size() > 1) {
        if (!args[1].is_int()) { error = "state.incr() expects an integer"; return Value::null(); }
        by = args[1].as_int();
    }
    return Value::integer(SharedState::instance().incr(args[0].as_str(), by));
}

Value fn_state_decr(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.decr() expects the key as a string"; return Value::null(); }
    long long by = args.size() > 1 && args[1].is_int() ? args[1].as_int() : 1;
    return Value::integer(SharedState::instance().incr(args[0].as_str(), -by));
}

Value fn_state_get(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.get() expects the key as a string"; return Value::null(); }
    Value v = SharedState::instance().get(args[0].as_str());
    if (v.is_null() && args.size() > 1) return args[1];
    return v;
}

Value fn_state_set(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.set() expects the key as a string"; return Value::null(); }
    SharedState::instance().set(args[0].as_str(), args[1]);
    return args[1];
}

Value fn_state_remove(NativeCtx&, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "state.remove() expects the key as a string"; return Value::null(); }
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
    if (!args[0].is_str()) { error = "cookie() expects the name as a string"; return Value::null(); }
    auto c = ctx.req.cookie(args[0].as_str());
    if (!c) return args.size() > 1 ? args[1] : Value::null();
    return Value::str(*c);
}

Value fn_form(NativeCtx& ctx, std::vector<Value>& args, std::string& error) {
    if (!args[0].is_str()) { error = "form() expects the name as a string"; return Value::null(); }
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

// An empty list and not null when the error does not come from validation: so
// that walking it with `for` always works.
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
    // Response
    {"text",      1, 1,  fn_text},
    {"html",      1, 1,  fn_html},
    {"json",      1, 1,  fn_json},
    {"render",    1, 2,  fn_render},
    {"__render_tpl", 2, 2, fn_render_tpl},
    {"status",    1, 1,  fn_status},
    {"redirect",  1, 2,  fn_redirect},
    {"send_file", 1, 2,  fn_send_file},
    // Utilities
    {"len",       1, 1,  fn_len},
    {"str",       1, 1,  fn_str},
    {"int",       1, 1,  fn_int},
    // Request
    {"header",    1, 2,  fn_header},
    {"query",     1, 2,  fn_query},
    // sse.* — not written like this in Lumen Script: reached via member_native_id().
    {"__sse_send", 1, 3,  fn_sse_send},
    {"__sse_ping", 0, 1,  fn_sse_ping},
    {"__sse_open", 0, 0,  fn_sse_open},
    // ws.* — same as sse.*, reached via member_native_id().
    {"__ws_send",  1, 1,  fn_ws_send},
    {"__ws_open",  0, 0,  fn_ws_open},
    {"__ws_close", 0, 0,  fn_ws_close},
    // session.* / jwt.* — reached via member_native_id() or, for session, by
    // accessing any field.
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
    // Asynchronous: with no fn, the handler's driver resolves them.
    {"sleep",     1, 1,  nullptr, true},
    {"__ws_recv", 0, 0,  nullptr, true},
    // Database: the first argument is the module name, which the emitter
    // pushes; that way a single builtin serves all three.
    {"__db_query", 2, -1, nullptr, true},
    {"__db_exec",  2, -1, nullptr, true},
    {"__db_begin",    1, 1, nullptr, true},
    {"__db_commit",   1, 1, nullptr, true},
    {"__db_rollback", 1, 1, nullptr, true},
    {"__db_last_id",  1, 1, nullptr, true},
    // Final marker so native_count() does not depend on the order.
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

// ─── Methods on values ───────────────────────────────────────────────────────

namespace {

bool want(size_t got, size_t min, size_t max, const std::string& name,
          std::string& error) {
    if (got >= min && got <= max) return true;
    error = "'" + name + "()' received a number of arguments it does not accept";
    return false;
}

// Saves an uploaded part keeping only the file name, with no path: that way a
// filename with ".." or an absolute one cannot escape the directory.
std::string safe_name(const std::string& raw) {
    size_t slash = raw.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? raw : raw.substr(slash + 1);
    if (base.empty() || base == "." || base == "..") base = "subida";
    return base;
}

} // namespace

Value call_method(NativeCtx& ctx, Value& recv, const std::string& name,
                  std::vector<Value>& args, std::string& error) {
    // ── Response modifiers ───────────────────────────────────────────────────
    // They chain onto what is returned —`return {...}.status(201)`— and let the
    // value through, so as not to reintroduce a mutable `response` object.
    if (name == "status") {
        if (args.size() != 1 || !args[0].is_int()) {
            error = "status() expects an integer status code";
            return Value::null();
        }
        ctx.res.status(static_cast<int>(args[0].as_int()));
        return recv;
    }
    if (name == "header") {
        if (args.size() != 2 || !args[0].is_str()) {
            error = "header() expects name y value";
            return Value::null();
        }
        ctx.res.header(args[0].as_str(), args[1].to_string());
        return recv;
    }
    if (name == "cookie") {
        if (args.size() < 2 || !args[0].is_str()) {
            error = "cookie() expects al menos name y value";
            return Value::null();
        }
        lumen::CookieOptions opts;
        opts.path      = "/";
        opts.http_only = true;
        opts.same_site = lumen::SameSite::Lax;

        // Third slot: the named options, grouped by the emitter.
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
            if (!args[0].is_str()) { error = "'" + name + "()' expects un string"; return Value::null(); }
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
        error = "strings have no method '" + name + "'";
        return Value::null();
    }

    // ── List ─────────────────────────────────────────────────────────────────
    if (recv.is_list()) {
        if (name == "add") {
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            recv.as_list().push_back(args[0]);
            return recv;
        }
        error = "Lists have no method '" + name + "'";
        return Value::null();
    }

    // ── Dict, File included ──────────────────────────────────────────────────
    if (recv.is_dict()) {
        auto& d = recv.as_dict();

        if (name == "save") {
            auto idx = d.find("__idx");
            if (idx == d.end() || !ctx.parts) {
                error = "save() only exists on an uploaded File";
                return Value::null();
            }
            if (!want(args.size(), 1, 1, name, error)) return Value::null();
            if (!args[0].is_str()) { error = "save() expects the directory as a string"; return Value::null(); }

            size_t i = static_cast<size_t>(idx->second.as_int());
            if (!ctx.parts || i >= ctx.parts->size()) {
                error = "the uploaded file is no longer available";
                return Value::null();
            }

            auto  fn   = d.find("filename");
            std::string base = safe_name(fn == d.end() ? "" : fn->second.to_string());
            std::string dir  = args[0].as_str();
            if (!dir.empty() && dir.back() != '/') dir += "/";

            std::error_code ec;
            std::filesystem::create_directories(dir, ec);

            std::ofstream out(dir + base, std::ios::binary);
            if (!out) { error = "cannot write to " + dir + base; return Value::null(); }
            const std::string& bytes = (*ctx.parts)[i].body;
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!out) { error = "fallo al write " + dir + base; return Value::null(); }
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
        error = "Dicts have no method '" + name + "'";
        return Value::null();
    }

    error = std::string("values of type ") + recv.type_name() +
            " have no methods";
    return Value::null();
}

// The compile-time face of call_method().
//
// What the VM would reject at run time can be rejected earlier if the type of
// the receiver is known: `name.mayusculas()` on a string is a typo, and a typo
// should not wait for someone to load the page to show up.
//
// If a method is added above it has to be added here: they are the same list
// seen from both sides.
const std::vector<BuiltinMethod>* methods_of(const std::string& type) {
    // The three response modifiers chain onto ANY value —`return
    // {...}.status(201)`— so they appear in every list.
    static const std::vector<BuiltinMethod> kComunes = {
        {"status", 1, 1, nullptr}, {"header", 2, 2, nullptr}, {"cookie", 2, 3, nullptr},
    };
    static const auto with_own = [](std::initializer_list<BuiltinMethod> own) {
        std::vector<BuiltinMethod> v = kComunes;
        v.insert(v.end(), own);
        return v;
    };

    static const std::vector<BuiltinMethod> kString = with_own({
        {"starts_with", 1, 1, "bool"}, {"ends_with", 1, 1, "bool"},
        {"contains", 1, 1, "bool"},    {"upper", 0, 0, "string"},
        {"lower", 0, 0, "string"},     {"trim", 0, 0, "string"},
    });
    static const std::vector<BuiltinMethod> kList = with_own({{"add", 1, 1, nullptr}});
    static const std::vector<BuiltinMethod> kDict = with_own({
        {"has", 1, 1, "bool"}, {"keys", 0, 0, "List"}, {"save", 1, 1, "string"},
    });

    if (type == "string") return &kString;
    if (type == "List")   return &kList;
    if (type == "Dict")   return &kDict;
    // Numbers and bools have nothing of their own: only the common part.
    if (type == "int" || type == "long" || type == "float" ||
        type == "double" || type == "bool")
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
