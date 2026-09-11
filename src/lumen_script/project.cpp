#include <lumen_script/project.hpp>
#include <lumen_script/template.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/vm.hpp>
#include <lumen_script/auth.hpp>
#include <lumen_script/db.hpp>

#include <lumen/request.hpp>
#include <lumen/response.hpp>
#include <lumen/task.hpp>
#include <lumen/sse.hpp>
#include <lumen/websocket.hpp>
#include <lumen/multipart.hpp>
#include <lumen/app.hpp>
#include <lumen/logger.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lumen_script {

// ─── Input ───────────────────────────────────────────────────────────────────

bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<fs::path>& out,
                    std::string& error) {
    std::error_code ec;

    for (const auto& a : args) {
        fs::path p(a);
        if (!fs::exists(p, ec)) {
            error = "does not exist: " + a;
            return false;
        }

        if (fs::is_directory(p, ec)) {
            size_t before = out.size();
            for (auto it = fs::recursive_directory_iterator(p, ec);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (it->is_regular_file(ec) && it->path().extension() == ".lum")
                    out.push_back(it->path());
            }
            if (out.size() == before) {
                error = "the directory contains no .lum files: " + a;
                return false;
            }
        } else {
            out.push_back(p);
        }
    }

    // Stable order so the diagnostics always come out the same.  The order does
    // not affect meaning: name resolution runs in two passes.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    if (out.empty()) { error = "no files to compile"; return false; }
    return true;
}

std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files) {
    std::vector<const SourceFile*> raw;
    raw.reserve(files.size());
    for (const auto& f : files) raw.push_back(f.get());
    return diags.format(raw);
}

// ─── Literal evaluation ──────────────────────────────────────────────────────
//
// A declarative route can only contain constant values: if something shows up
// that has to be computed, the route needs the VM and that is another milestone.

namespace {

// --native phase 1 (COMPILACION-NATIVA.md): for ONE specific call to
// emit_route/emit_function/emit_method/emit_ctor/emit_error_handler/
// emit_condition, compares the diagnostics it added against what its check_*
// equivalent gives. Purely observational — never touches `diags`, never
// changes the result of a real compilation — so it only runs if
// LUMEN_SHADOW_CHECK is set in the environment: zero cost on the normal
// path. It is the canary that validates, before taking the step of turning
// emit_expr/emit_stmt/emit_call into IR consumers and stripping their own
// checks, that check_expr/check_stmt keep reproducing the real compilation
// on organic .lum programs too (not just the hand-written cases in
// tests/check_*_shadow.cpp).
bool shadow_check_enabled() {
    static const bool v = std::getenv("LUMEN_SHADOW_CHECK") != nullptr;
    return v;
}

void shadow_compare(const char* label, const DiagnosticBag& diags, size_t before,
                    const DiagnosticBag& shadow) {
    if (!shadow_check_enabled()) return;
    std::vector<std::string> real, shadow_msgs;
    for (size_t i = before; i < diags.items().size(); ++i) real.push_back(diags.items()[i].message);
    for (const auto& it : shadow.items()) shadow_msgs.push_back(it.message);
    if (real == shadow_msgs) return;
    std::fprintf(stderr, "[shadow-check] discrepancy in %s\n", label);
    for (const auto& m : real)       std::fprintf(stderr, "  real:   %s\n", m.c_str());
    for (const auto& m : shadow_msgs) std::fprintf(stderr, "  shadow: %s\n", m.c_str());
}

// Lumen Script type name for an already built value.  Used where the data is
// constant and therefore its type is exact.
std::string lumen_script_type_of(const Value& v) {
    switch (v.type()) {
        case Value::Type::Str:   return "string";
        case Value::Type::Int:   return "int";
        case Value::Type::Float: return "float";
        case Value::Type::Bool:  return "bool";
        case Value::Type::List:  return "List";
        case Value::Type::Dict:  return "Dict";
        default:                 return {};
    }
}



// The same, but producing a Value instead of an nlohmann tree.
//
// The difference matters for key order: nlohmann keeps its objects in a
// std::map and sorts them alphabetically, while the VM returns them in the
// order they are written.  If declarative routes used nlohmann, two routes from
// the same .lum would order differently.
bool const_eval_valor(const Expr& e, Value& out) {
    switch (e.kind) {
        case ExprKind::StringLit: out = Value::str(e.text);          return true;
        case ExprKind::IntLit:    out = Value::integer(e.int_value); return true;
        case ExprKind::FloatLit:  out = Value::real(e.float_value);  return true;
        case ExprKind::BoolLit:   out = Value::boolean(e.bool_value); return true;
        case ExprKind::NullLit:   out = Value::null();               return true;

        case ExprKind::ListLit: {
            Value::List l;
            l.reserve(e.items.size());
            for (const auto& item : e.items) {
                Value v;
                if (!const_eval_valor(*item, v)) return false;
                l.push_back(std::move(v));
            }
            out = Value::list(std::move(l));
            return true;
        }
        case ExprKind::DictLit: {
            Value::Dict d;
            for (const auto& entry : e.entries) {
                if (entry.key->kind != ExprKind::StringLit) return false;
                Value v;
                if (!const_eval_valor(*entry.value, v)) return false;
                d[entry.key->text] = std::move(v);
            }
            out = Value::dict(std::move(d));
            return true;
        }
        default:
            return false;
    }
}

// Name of the called function, if it is a direct call to an identifier.
const std::string* callee_name(const Expr& e) {
    if (e.kind != ExprKind::Call || !e.object) return nullptr;
    if (e.object->kind != ExprKind::Ident)     return nullptr;
    return &e.object->text;
}

// Replies with a hand-built JSON body.
void responder(lumen::Response& res, int code, Value v) {
    res.status(code).json_text(v.to_json_text());
}

// Engine errors are always {"error": "..."} and sometimes carry a list of
// messages: two fixed shapes that need nothing more.
void responder_error(lumen::Response& res, int code, const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    responder(res, code, Value::dict(std::move(d)));
}

void responder_error(lumen::Response& res, int code, const std::string& msg,
                     const std::vector<std::string>& messages) {
    Value::List l;
    l.reserve(messages.size());
    for (const auto& m : messages) l.push_back(Value::str(m));
    Value::Dict d;
    d["error"]    = Value::str(msg);
    d["messages"] = Value::list(std::move(l));
    responder(res, code, Value::dict(std::move(d)));
}

using Action = std::function<void(lumen::Request&, lumen::Response&)>;

// Translates the `return <expr>` of a declarative route into a native action.
// Returns an empty Action and records the diagnostic if the expression needs
// runtime evaluation.
Action compile_return(const Expr& e, DiagnosticBag& diags,
                      const std::string& tpl_dir) {
    // return { ... }  /  return "literal"  → JSON body
    Value literal;
    if (const_eval_valor(e, literal)) {
        // The body is serialized HERE, once.  It used to keep the nlohmann tree
        // and call dump() on every request: a route resolved entirely at compile
        // time should not serialize anything on the hot path.
        std::string cuerpo = literal.to_json_text();
        return [cuerpo](lumen::Request&, lumen::Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(cuerpo);
        };
    }

    const std::string* fn = callee_name(e);
    if (!fn) {
        diags.error(e.loc, "this expression needs the VM, which is not "
                           "implemented yet; in milestone 1 a route can only "
                           "return a constant value or a native call "
                           "(render, text, html, json, status, redirect, send_file)");
        return {};
    }

    auto need_string = [&](size_t idx, const char* what) -> const std::string* {
        if (e.args.size() <= idx || e.args[idx].value->kind != ExprKind::StringLit) {
            diags.error(e.loc, std::string(*fn) + "() expects " + what +
                               " as a string literal");
            return nullptr;
        }
        return &e.args[idx].value->text;
    };

    if (*fn == "render") {
        const std::string* tpl = need_string(0, "the template name");
        if (!tpl) return {};

        // The named arguments are the template variables.  They are converted
        // HERE, once: the route is declarative, so all that is left on the hot
        // path is rendering.
        Value::Dict data;
        for (size_t i = 1; i < e.args.size(); ++i) {
            const Arg& a = e.args[i];
            if (a.name.empty()) {
                diags.error(a.loc, "after the template name, the arguments "
                                   "of render() are named: key=value");
                return {};
            }
            Value v;
            if (!const_eval_valor(*a.value, v)) {
                diags.error(a.loc, "non-constant value in render(): needs the VM");
                return {};
            }
            data[a.name] = std::move(v);
        }
        // The data is constant, so the page can be rendered IN FULL here: the
        // route is reduced to sending fixed bytes.  And the template errors come
        // out at compile time, like all the others.
        const std::string name = *tpl;
        if (name.find("..") != std::string::npos ||
            std::filesystem::path(name).is_absolute()) {
            diags.error(e.loc, "invalid template name: '" + name + "'");
            return {};
        }
        std::ifstream f(std::filesystem::path(tpl_dir) / name, std::ios::binary);
        if (!f) {
            diags.error(e.loc, "template not found: '" + name + "' en " + tpl_dir);
            return {};
        }
        const std::string fuente((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());

        // Here the data is CONSTANT, so the type of each key is known exactly:
        // the template is checked against the real values.
        std::vector<TypedName> keys;
        std::vector<Value>        values;
        for (const auto& [k, v] : data) {
            keys.push_back({k, lumen_script_type_of(v)});
            values.push_back(v);
        }

        Template tpl_c;
        if (!compilar_plantilla(fuente, name, tpl_dir, keys, diags, tpl_c)) return {};

        lumen::Request  req_falsa;
        lumen::Response res_falsa;
        NativeCtx        ctx{req_falsa, res_falsa};
        std::string      html, err;
        if (!render_plantilla(tpl_c, std::move(values), ctx, nullptr, html, err)) {
            diags.error(e.loc, "al renderizar '" + name + "': " + err);
            return {};
        }
        return [html](lumen::Request&, lumen::Response& res) {
            res.header("Content-Type", "text/html; charset=utf-8").send(html);
        };
    }

    if (*fn == "text" || *fn == "html") {
        const std::string* s = need_string(0, "the content");
        if (!s) return {};
        std::string body = *s;
        bool is_html = (*fn == "html");
        return [body, is_html](lumen::Request&, lumen::Response& res) {
            if (is_html) res.html(body); else res.text(body);
        };
    }

    if (*fn == "send_file") {
        const std::string* p = need_string(0, "the file path");
        if (!p) return {};
        std::string path = *p;
        return [path](lumen::Request&, lumen::Response& res) {
            res.send_file(path);
        };
    }

    if (*fn == "status") {
        if (e.args.empty() || e.args[0].value->kind != ExprKind::IntLit) {
            diags.error(e.loc, "status() expects a numeric status code");
            return {};
        }
        int code = static_cast<int>(e.args[0].value->int_value);
        return [code](lumen::Request&, lumen::Response& res) {
            res.status(code).send("");
        };
    }

    if (*fn == "redirect") {
        const std::string* target = need_string(0, "the target");
        if (!target) return {};
        int code = 302;
        if (e.args.size() > 1) {
            if (e.args[1].value->kind != ExprKind::IntLit) {
                diags.error(e.loc, "the second argument of redirect() is the status code");
                return {};
            }
            code = static_cast<int>(e.args[1].value->int_value);
        }
        std::string to = *target;
        return [to, code](lumen::Request&, lumen::Response& res) {
            res.status(code).header("Location", to).send("");
        };
    }

    if (*fn == "json") {
        if (e.args.size() != 1) {
            diags.error(e.loc, "json() expects a single argument");
            return {};
        }
        Value v;
        if (!const_eval_valor(*e.args[0].value, v)) {
            diags.error(e.loc, "json() with a non-constant value: needs the VM");
            return {};
        }
        std::string cuerpo = v.to_json_text();
        return [cuerpo](lumen::Request&, lumen::Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(cuerpo);
        };
    }

    diags.error(e.loc, "funcion nativa desconocida: '" + *fn + "'");
    return {};
}

// Extracts the :param / {param} names from the route pattern.
std::vector<std::string> pattern_params(const std::string& pattern) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':' || pattern[i] == '{') {
            char close = (pattern[i] == '{') ? '}' : '/';
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != close) ++j;
            out.push_back(pattern.substr(i + 1, j - i - 1));
            i = j;
        } else ++i;
    }
    return out;
}

const std::vector<std::string>& scalar_types() {
    static const std::vector<std::string> v =
        {"int", "long", "float", "double", "bool", "string"};
    return v;
}

// The identifier of an asynchronous builtin is its index in the table; it is
// looked up once so as not to depend on the order.
int async_sleep_id() {
    static const int id = native_id("sleep");
    return id;
}

int async_ws_recv_id() {
    static const int id = native_id("__ws_recv");
    return id;
}

// The VM step cap resets on every suspension on purpose, so a legitimate
// sse/ws loop can live for hours (see kStepLimit in vm.hpp).  But that leaves a
// gap: `while true: await sleep(0)` suspends and resumes without advancing
// much, so it never accumulates steps between two suspensions and the cap never
// sees it — a single client could pin a whole thread by rescheduling a 0 ms
// timer over and over.  A 1 ms floor does not prevent it entirely, but it
// bounds it to ~1000 resumptions/s per connection instead of as many as the
// scheduler cares to give: no legitimate workload needs less than that.
//
long long clamp_sleep_ms(long long ms) {
    return ms < 1 ? 1 : ms;
}

bool is_scalar(const std::string& t) {
    const auto& v = scalar_types();
    return std::find(v.begin(), v.end(), t) != v.end();
}

int async_db_query_id() { static const int id = native_id("__db_query"); return id; }
int async_db_exec_id()  { static const int id = native_id("__db_exec");  return id; }
int async_db_begin_id()    { static const int id = native_id("__db_begin");    return id; }
int async_db_commit_id()   { static const int id = native_id("__db_commit");   return id; }
int async_db_rollback_id() { static const int id = native_id("__db_rollback"); return id; }
int async_db_last_id()     { static const int id = native_id("__db_last_id");  return id; }

bool is_db_await(int id) {
    return id == async_db_query_id()  || id == async_db_exec_id()  ||
           id == async_db_begin_id()  || id == async_db_commit_id() ||
           id == async_db_rollback_id() || id == async_db_last_id();
}

Value db_error(const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    return Value::dict(std::move(d));
}

// Resolves a database suspension.
//
// The first argument is always the module name, which the emitter pushes.
// An engine failure does not blow up the handler: it arrives as a value with
// `error`, which the .lum can inspect or ignore.
//
// Thin adapter over lumen_script::await_db() (db.hpp/db.cpp) — the real
// logic (resolving the driver/pool, deciding the pinned worker, invoking
// DbAwaitable, updating pinned_workers/last_exec_workers) lives there,
// shared with the code that generates a --native route for the same thing:
// both paths run EXACTLY the same code, not a separate reproduction that
// could silently diverge (the same class of bug that already caused two
// critical fixes in this phase).
lumen::Task<Value> run_db(const VM::Result& r, int op, lumen::Request& req,
                           NativeCtx& ctx) {
    if (r.await_args.empty() || !r.await_args[0].is_str())
        co_return db_error("malformed query");

    const std::string mod = r.await_args[0].as_str();

    DbOp dbop = DbOp::Rollback;
    if      (op == async_db_query_id())  dbop = DbOp::Query;
    else if (op == async_db_exec_id())   dbop = DbOp::Exec;
    else if (op == async_db_last_id())   dbop = DbOp::LastId;
    else if (op == async_db_begin_id())  dbop = DbOp::Begin;
    else if (op == async_db_commit_id()) dbop = DbOp::Commit;

    std::string sql;
    std::vector<Value> params;
    if (dbop == DbOp::Query || dbop == DbOp::Exec) {
        if (r.await_args.size() < 2 || !r.await_args[1].is_str())
            co_return db_error("missing the SQL query");
        sql    = r.await_args[1].as_str();
        params = std::vector<Value>(r.await_args.begin() + 2, r.await_args.end());
    }

    Value v = co_await await_db(dbop, mod, req.loop, sql, std::move(params),
                                ctx.pinned_workers, ctx.last_exec_workers);
    co_return v;
}

// Closes the transactions the handler left open.
//
// Without this, a `return` halfway through or an error would leave the
// connection inside a transaction forever, and whoever took it from the pool
// next would inherit that state.
lumen::Task<void> rollback_pendientes(NativeCtx& ctx, lumen::Request& req) {
    co_await rollback_pendientes_db(ctx.pinned_workers, req.loop);
}

// ─── Classes ─────────────────────────────────────────────────────────────────

struct ClassField {
    std::string name;
    std::string type;
    bool        optional = false;
};

// A `validate:` rule already compiled: it receives the fields as locals, in the
// declaration order, and returns a boolean.
struct ClassRule {
    std::shared_ptr<Chunk> chunk;
    std::string            message;
};

struct ClassInfo {
    std::string             name;
    std::vector<ClassField> fields;
    std::vector<ClassRule>  rules;
};

using ClassTable = std::map<std::string, std::shared_ptr<ClassInfo>>;

void build_classes(const Program& program, const FunctionSigs& fns,
                   const ClassSigs& sigs, const std::set<std::string>* imports,
                   ClassTable& out, DiagnosticBag& diags) {
    for (const auto& c : program.classes) {
        if (out.count(c.name)) {
            diags.error(c.loc, "class '" + c.name + "' is already declared");
            continue;
        }

        auto info  = std::make_shared<ClassInfo>();
        info->name = c.name;

        std::vector<TypedName> field_names;
        bool ok = true;

        for (const auto& f : c.fields) {
            if (!is_scalar(f.type.name)) {
                diags.error(f.loc, "type '" + f.type.str() + "' as a field is not yet "
                                   "is not implemented; for now only "
                                   "int, long, float, double, bool y string");
                ok = false;
                continue;
            }
            info->fields.push_back({f.name, f.type.name, f.type.optional});
            field_names.push_back({f.name, f.type.name});
        }
        if (!ok) continue;

        // Every rule is compiled against the class fields: if it mentions a name
        // that does not exist, the error comes out here and not in production.
        for (const auto& r : c.rules) {
            auto    chunk = std::make_shared<Chunk>();
            Emitter emitter(diags, &fns, &sigs, imports);
            size_t  antes = diags.size();
            if (!emitter.emit_condition(*r.condition, field_names, *chunk)) continue;
            if (shadow_check_activo()) {
                DiagnosticBag shadow;
                emitter.check_condition(*r.condition, field_names, *chunk, shadow);
                shadow_comparar("validate", diags, antes, shadow);
            }
            info->rules.push_back({chunk, r.message});
        }

        out[c.name] = std::move(info);
    }
}

// Session signing/verification and JWT (sign_session, load_session,
// verify_jwt, AuthConfig, begin_auth, end_auth) live in
// include/lumen_script/auth.hpp + src/lumen_script/auth.cpp: any backend
// that runs Lumen Script routes needs it, not just this VM (see
// COMPILACION-NATIVA.md, phase 0) — and it is also where the session-cookie
// expiry lives (`exp` signed alongside the payload, checked in
// load_session): see the comment there for why a session that never expires
// on its own was a real bug, not just a hardening nice-to-have.

// ─── Parameter binding ───────────────────────────────────────────────────────

enum class BindKind { Path, Query, Body, File, FileList };

struct ParamBind {
    BindKind    kind = BindKind::Path;
    std::string name;
    std::string type;          // scalar, or the class name if it is a Body
    bool        has_default = false;
    std::string default_text;
    std::shared_ptr<ClassInfo> cls;   // Body only
};

// Converts the raw URL text to the declared type.  A malformed value is a 400:
// the client sent it wrong, it is not a server failure.
bool coerce(const std::string& text, const std::string& type, Value& out) {
    try {
        if (type == "string") { out = Value::str(text); return true; }
        if (type == "int" || type == "long") {
            // std::stoll() only requires a valid prefix, not the whole
            // string: "42abc" silently becomes 42, and "0x10" becomes 0 (it
            // reads the "0", stops at 'x', and reports success) since it
            // parses base 10 unless told otherwise. Requiring the full text
            // be consumed rejects both instead of quietly truncating input
            // the client sent wrong — README promises no implicit coercion
            // anywhere in the language; a route param is not an exception.
            size_t pos = 0;
            long long v = std::stoll(text, &pos);
            if (pos != text.size()) return false;
            out = Value::integer(v);
            return true;
        }
        if (type == "float" || type == "double") {
            size_t pos = 0;
            double v = std::stod(text, &pos);
            if (pos != text.size()) return false;
            out = Value::real(v);
            return true;
        }
        if (type == "bool") {
            if (text == "true"  || text == "1") { out = Value::boolean(true);  return true; }
            if (text == "false" || text == "0") { out = Value::boolean(false); return true; }
            return false;
        }
    } catch (...) { return false; }
    return false;
}

// Checks that a JSON value fits the field's declared type.
// There is no conversion between families: a string in an int field is an
// error, not an attempt to parse.
// Checks that the received value fits the type declared in the class.
//
// It is deliberately strict: a "30" is not good enough where an int was
// declared.  The body saying one thing and the class another is exactly what
// validation exists to catch.
bool value_matches(const Value& v, const std::string& type, Value& out) {
    if (type == "string") {
        if (!v.is_str()) return false;
        out = v;
        return true;
    }
    if (type == "bool") {
        if (!v.is_bool()) return false;
        out = v;
        return true;
    }
    if (type == "int" || type == "long") {
        if (!v.is_int()) return false;
        out = v;
        return true;
    }
    if (type == "float" || type == "double") {
        if (!v.is_num()) return false;
        out = Value::real(v.as_float());
        return true;
    }
    return false;
}

// Validates the parameters against the pattern and produces the binding plan.
bool bind_params(const RouteDecl& r, const ClassTable& classes,
                 std::vector<ParamBind>& out, DiagnosticBag& diags) {
    auto in_pattern = pattern_params(r.pattern);
    bool ok         = true;
    bool seen_body  = false;

    for (const auto& seg : in_pattern) {
        bool found = false;
        for (const auto& p : r.params) if (p.name == seg) { found = true; break; }
        if (!found) {
            diags.error(r.pattern_loc,
                        "the pattern declares ':" + seg + "' but no parameter binds it");
            ok = false;
        }
    }

    for (const auto& p : r.params) {
        bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                       != in_pattern.end();

        // File / List<File> bind to the multipart parts with that name.
        bool is_file      = (p.type.name == "File");
        bool is_file_list = (p.type.name == "List" && p.type.args.size() == 1 &&
                             p.type.args[0].name == "File");
        if (is_file || is_file_list) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' is in the route pattern, "
                                   "so it cannot be an uploaded file");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "a route " + r.method + " takes no body");
                ok = false;
                continue;
            }
            ParamBind fb;
            fb.kind = is_file ? BindKind::File : BindKind::FileList;
            fb.name = p.name;
            fb.type = p.type.name;
            out.push_back(std::move(fb));
            continue;
        }

        // A parameter whose type is a class binds to the request body: that is
        // the FastAPI idea, the body is one more typed parameter.
        auto it = classes.find(p.type.name);
        if (it != classes.end()) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' is in the route pattern, "
                                   "so it cannot be of type '" + p.type.name + "'");
                ok = false;
                continue;
            }
            if (seen_body) {
                diags.error(p.loc, "there can only be one body parameter per route");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "a route " + r.method + " takes no body");
                ok = false;
                continue;
            }
            seen_body = true;
            out.push_back({BindKind::Body, p.name, p.type.name, false, {}, it->second});
            continue;
        }

        ParamBind b;
        b.name = p.name;
        b.type = p.type.name;

        if (!is_scalar(p.type.name)) {
            diags.error(p.loc, "unknown type: '" + p.type.str() + "'");
            ok = false;
            continue;
        }
        if (p.type.optional) {
            diags.error(p.loc, "optional parameters are not implemented yet");
            ok = false;
            continue;
        }

        b.kind = in_path ? BindKind::Path : BindKind::Query;

        if (in_path && p.default_value) {
            diags.error(p.loc, "a path parameter cannot have a default value");
            ok = false;
            continue;
        }
        if (!in_path && p.default_value) {
            const Expr& d = *p.default_value;
            if      (d.kind == ExprKind::StringLit) b.default_text = d.text;
            else if (d.kind == ExprKind::IntLit)    b.default_text = std::to_string(d.int_value);
            else if (d.kind == ExprKind::BoolLit)   b.default_text = d.bool_value ? "true" : "false";
            else {
                diags.error(p.loc, "the default value must be a constant");
                ok = false;
                continue;
            }
            b.has_default = true;
        }

        out.push_back(std::move(b));
    }
    return ok;
}

// Recognizes the purely declarative shape: a single `return` resolved entirely
// at compile time.  These routes do not run a single bytecode step.
Action try_declarative(const RouteDecl& r, const std::string& tpl_dir) {
    // A route with guards can NEVER take the declarative path: the native
    // action does not run them, so it would skip the group's protection.
    if (!r.guards.empty())                                       return {};
    if (!r.params.empty())                                       return {};
    if (r.body.size() != 1)                                      return {};
    if (r.body[0]->kind != StmtKind::Return || !r.body[0]->value) return {};

    DiagnosticBag scratch;
    Action a = compile_return(*r.body[0]->value, scratch, tpl_dir);
    return scratch.empty() ? a : Action{};
}

// Builds the instance from the JSON body.
//
// A missing required field, or one with the wrong type, or a broken validate
// rule, is a 422 with the complete list of reasons: they are all reported at
// once, not just the first.  The handler never runs.
bool bind_body(const ClassInfo& ci, const FunctionTable* fns,
               lumen::Request& req, lumen::Response& res,
               NativeCtx& ctx, Value& out) {
    Value body;
    if (!Value::parse_json(req.body, body)) {
        responder_error(res, 400, "invalid JSON");
        return false;
    }
    if (!body.is_dict()) {
        responder_error(res, 422, "Validacion fallida",
                        {"the body must be a JSON object"});
        return false;
    }

    std::vector<std::string> messages;
    Value::Dict              fields;
    std::vector<Value>       ordered;

    for (const auto& f : ci.fields) {
        auto it = body.as_dict().find(f.name);
        if (it == body.as_dict().end() || it->second.is_null()) {
            if (!f.optional) messages.push_back(f.name + ": required");
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        Value v;
        if (!value_matches(it->second, f.type, v)) {
            messages.push_back(f.name + ": expected " + f.type);
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        fields[f.name] = v;
        ordered.push_back(std::move(v));
    }

    // The rules only run if the fields are sound: evaluating them over missing
    // values would give type errors instead of the useful message.
    if (messages.empty()) {
        thread_local VM rule_vm;
        for (const auto& rule : ci.rules) {
            VM::Result r = rule_vm.start(*rule.chunk, ordered, ctx, fns);
            if (r.status != VM::Status::Done) {
                lumen::log().error("validate of " + ci.name + ": " + r.error);
                responder_error(res, 500, r.error);
                return false;
            }
            if (!r.value.truthy()) messages.push_back(rule.message);
        }
    }

    if (!messages.empty()) {
        // They are left within reach of this same request's `on error 422`.
        last_validation_messages() = messages;
        responder_error(res, 422, "Validacion fallida", messages);
        return false;
    }

    out = Value::dict(std::move(fields));
    return true;
}

// Fills the parameter slots from the request.
// Returns false, with the response already written, if some value does not fit.
// A File in the language is the metadata plus the index of its part in
// ctx.uploads; the bytes do not travel inside the Value.
Value make_file_value(const lumen::MultipartPart& part, size_t index) {
    Value::Dict d;
    d["name"]         = Value::str(part.name);
    d["filename"]     = Value::str(part.filename);
    d["content_type"] = Value::str(part.content_type);
    d["size"]         = Value::integer(static_cast<long long>(part.body.size()));
    d["__idx"]        = Value::integer(static_cast<long long>(index));
    return Value::dict(std::move(d));
}

bool prepare_args(const std::vector<ParamBind>& binds, const FunctionTable* fns,
                  lumen::Request& req, lumen::Response& res,
                  NativeCtx& ctx, std::vector<Value>& out) {
    // They are cleared on entry: a 422 the handler writes by hand must not
    // inherit the messages of an earlier validation on this thread.
    last_validation_messages().clear();
    out.reserve(binds.size());
    for (const auto& b : binds) {
        if (b.kind == BindKind::File || b.kind == BindKind::FileList) {
            if (!ctx.uploads) {
                responder_error(res, 400, "expected multipart/form-data");
                return false;
            }
            Value::List matches;
            for (size_t i = 0; i < ctx.parts->size(); ++i) {
                const auto& part = (*ctx.parts)[i];
                if (part.name == b.name && !part.filename.empty())
                    matches.push_back(make_file_value(part, i));
            }

            if (b.kind == BindKind::FileList) {
                out.push_back(Value::list(std::move(matches)));
                continue;
            }
            if (matches.empty()) {
                responder_error(res, 422, "Validacion fallida",
                                {b.name + ": the file is missing"});
                return false;
            }
            out.push_back(matches[0]);
            continue;
        }

        if (b.kind == BindKind::Body) {
            Value v;
            if (!bind_body(*b.cls, fns, req, res, ctx, v)) return false;
            out.push_back(std::move(v));
            continue;
        }

        std::string raw;
        bool present = false;

        if (b.kind == BindKind::Path) {
            auto it = req.params.find(b.name);
            if (it != req.params.end()) { raw = it->second; present = true; }
        } else {
            auto it = req.query.find(b.name);
            if (it != req.query.end()) { raw = it->second; present = true; }
            else if (ctx.uploads && ctx.parts) {
                // A text field of a multipart form is one more part, just like a
                // File, only without a filename.  This branch did not exist
                // before: a scalar parameter on a route with a File always came
                // out empty, with no error, because only the query string was read.
                for (const auto& part : *ctx.parts) {
                    if (part.name == b.name && part.filename.empty()) {
                        raw = part.body; present = true; break;
                    }
                }
            }
            if (!present && b.has_default) { raw = b.default_text; present = true; }
        }

        Value v;
        if (!present) {
            if      (b.type == "string") v = Value::str("");
            else if (b.type == "bool")   v = Value::boolean(false);
            else if (b.type == "float" || b.type == "double") v = Value::real(0);
            else                         v = Value::integer(0);
        } else if (!coerce(raw, b.type, v)) {
            Value::Dict d;
            d["error"]    = Value::str("invalid parameter");
            d["param"]    = Value::str(b.name);
            d["expected"] = Value::str(b.type);
            d["received"] = Value::str(raw);
            responder(res, 400, Value::dict(std::move(d)));
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

// ─── OpenAPI from the AST ────────────────────────────────────────────────────
//
// The C++ version of this was 347 lines of metaprogramming that deduced the
// schema by building a default T{} and inspecting the resulting JSON.  With an
// AST in front of you, the names and the types are already there: it is a walk
// over declarations.

std::string openapi_type(const std::string& t) {
    if (t == "int" || t == "long")        return "integer";
    if (t == "float" || t == "double")    return "number";
    if (t == "bool")                      return "boolean";
    return "string";
}

// The Lumen Script pattern uses :name; OpenAPI uses {name}.
std::string openapi_path(const std::string& pattern) {
    std::string out;
    size_t i = 0;
    while (i < pattern.size()) {
        if (pattern[i] == ':') {
            size_t j = i + 1;
            while (j < pattern.size() && pattern[j] != '/') ++j;
            out += "{" + pattern.substr(i + 1, j - i - 1) + "}";
            i = j;
        } else {
            out += pattern[i++];
        }
    }
    return out;
}

// Shortcuts to assemble the document without drowning in Value::Dict.
Value jstr(std::string v) { return Value::str(std::move(v)); }

Value jobj(std::initializer_list<std::pair<const char*, Value>> fields) {
    Value::Dict d;
    d.reserve(fields.size());
    for (const auto& [k, v] : fields) d[k] = v;
    return Value::dict(std::move(d));
}

// The document is built and serialized ONCE, when the .lum is compiled.  It
// used to keep the tree and call dump() on every request to /openapi.json,
// which is hot-path work for something that does not change.
std::string build_openapi(const Program& program, const ClassTable& classes) {
    Value::Dict doc;
    doc["openapi"] = jstr("3.0.3");
    doc["info"]    = jobj({
        {"title",   jstr(program.app.name.empty() ? "Lumen API" : program.app.name)},
        {"version", jstr(program.app.version.empty() ? "0.1.0" : program.app.version)},
    });

    // Classes are published as reusable schemas.  The `validate:` messages are
    // attached as the description: they are the real rules the server applies,
    // so they document better than any separate text.
    Value::Dict schemas;
    for (const auto& [name, info] : classes) {
        Value::Dict props;
        Value::List required;

        for (const auto& f : info->fields) {
            props[f.name] = jobj({{"type", jstr(openapi_type(f.type))}});
            if (!f.optional) required.push_back(jstr(f.name));
        }

        Value::Dict schema;
        schema["type"]       = jstr("object");
        schema["properties"] = Value::dict(std::move(props));
        if (!required.empty()) schema["required"] = Value::list(std::move(required));

        if (!info->rules.empty()) {
            std::string desc = "Validation rules:";
            for (const auto& r : info->rules) desc += "\n- " + r.message;
            schema["description"] = jstr(std::move(desc));
        }
        schemas[name] = Value::dict(std::move(schema));
    }
    if (!schemas.empty())
        doc["components"] = jobj({{"schemas", Value::dict(std::move(schemas))}});

    Value::Dict paths;

    for (const auto& r : program.routes) {
        // Streaming routes do not fit OpenAPI 3.0: they are advertised as GET
        // with the response they really return, without faking a schema.
        std::string method = r.method;
        if (method == "SSE" || method == "WS") method = "GET";
        if (method == "*")                     method = "get";
        for (auto& c : method) c = static_cast<char>(::tolower((unsigned char)c));

        auto        in_pattern = pattern_params(r.pattern);
        Value::List params;
        std::string body_class;

        for (const auto& p : r.params) {
            if (classes.count(p.type.name)) { body_class = p.type.name; continue; }
            if (p.type.name == "File" ||
                (p.type.name == "List" && !p.type.args.empty() &&
                 p.type.args[0].name == "File")) {
                body_class = "__multipart";
                continue;
            }

            const bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                                 != in_pattern.end();
            params.push_back(jobj({
                {"name",     jstr(p.name)},
                {"in",       jstr(in_path ? "path" : "query")},
                {"required", Value::boolean(in_path)},
                {"schema",   jobj({{"type", jstr(openapi_type(p.type.name))}})},
            }));
        }

        Value::Dict op;
        if (!params.empty()) op["parameters"] = Value::list(std::move(params));

        if (body_class == "__multipart") {
            op["requestBody"] = jobj({
                {"required", Value::boolean(true)},
                {"content",  jobj({{"multipart/form-data",
                                    jobj({{"schema", jobj({{"type", jstr("object")}})}})}})},
            });
        } else if (!body_class.empty()) {
            op["requestBody"] = jobj({
                {"required", Value::boolean(true)},
                {"content",  jobj({{"application/json",
                    jobj({{"schema", jobj({{"$ref",
                        jstr("#/components/schemas/" + body_class)}})}})}})},
            });
        }

        Value::Dict responses;
        if (r.method == "SSE") {
            responses["200"] = jobj({
                {"description", jstr("Event stream")},
                {"content",     jobj({{"text/event-stream", Value::dict()}})},
            });
            op["summary"] = jstr("Server-Sent Events");
        } else if (r.method == "WS") {
            responses["101"] = jobj({{"description", jstr("Cambio a WebSocket")}});
            op["summary"]    = jstr("WebSocket");
        } else {
            responses["200"] = jobj({{"description", jstr("OK")}});
        }
        // Only the codes the server really produces are declared.
        if (!body_class.empty() && body_class != "__multipart")
            responses["422"] = jobj({{"description", jstr("Validacion fallida")}});
        if (!r.guards.empty())
            responses["403"] = jobj({{"description", jstr("Group guard not passed")}});

        // Every declared `on error` code is advertised on all routes.
        for (const auto& e : program.errors)
            if (e.code >= 400)
                responses[std::to_string(e.code)] =
                    jobj({{"description", jstr("Manejador propio")}});

        op["responses"] = Value::dict(std::move(responses));

        // Several methods can share a path, so it accumulates onto whatever was
        // already there instead of overwriting it.
        Value& entrada = paths[openapi_path(r.pattern)];
        if (!entrada.is_dict()) entrada = Value::dict();
        entrada.as_dict()[method] = Value::dict(std::move(op));
    }

    doc["paths"] = Value::dict(std::move(paths));
    return Value::dict(std::move(doc)).to_json_text();
}

// Functions are compiled before routes and handlers, and they all see the
// complete table: that way they can call each other regardless of the order
// they were declared in or the file they are in.
// Fills FnSig from a parameter list, checking that no required one comes after
// one with a default value.
FnSig make_sig(size_t index, const std::vector<Param>& params, DiagnosticBag& diags) {
    FnSig sig;
    sig.index = index;
    bool seen_default = false;
    for (const auto& p : params) {
        sig.defaults.push_back(p.default_value.get());
        if (p.default_value) seen_default = true;
        else {
            if (seen_default)
                diags.error(p.loc, "a parameter without a default cannot come "
                                   "after one that has one");
            ++sig.required;
        }
    }
    return sig;
}

// Methods and constructors are compiled as functions with `this` as the first
// parameter, so they go into the same table as standalone functions.
ClassSigs build_class_signatures(Module& mod, DiagnosticBag& diags) {
    ClassSigs out;

    for (const auto& c : mod.program.classes) {
        if (out.count(c.name)) continue;      // duplicate already reported
        ClassSig sig;
        for (const auto& f : c.fields) sig.fields.push_back(f.name);

        for (const auto& m : c.methods) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.methods[m.name] = make_sig(idx, m.params, diags);
        }
        for (const auto& ct : c.ctors) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.ctors[ct.params.size()] = idx;
        }

        // With no declared constructor, the full mapping one is offered: every
        // field in order.
        if (sig.ctors.empty() && !c.fields.empty()) {
            size_t idx = mod.functions.size();
            mod.functions.push_back(std::make_shared<Chunk>());
            sig.ctors[c.fields.size()] = idx;
        }
        out[c.name] = std::move(sig);
    }
    return out;
}

void emit_class_bodies(Module& mod, const ClassSigs& classes, const FunctionSigs& fns,
                       DiagnosticBag& diags) {
    const std::set<std::string>* imports = &mod.program.imports;
    for (const auto& c : mod.program.classes) {
        auto it = classes.find(c.name);
        if (it == classes.end()) continue;
        const ClassSig& sig = it->second;

        for (const auto& m : c.methods) {
            auto ms = sig.methods.find(m.name);
            if (ms == sig.methods.end()) continue;
            Emitter emitter(diags, &fns, &classes, imports);
            size_t  antes = diags.size();
            emitter.emit_method(c.name, m, *mod.functions[ms->second.index]);
            if (shadow_check_activo()) {
                DiagnosticBag shadow;
                emitter.check_method(c.name, m, *mod.functions[ms->second.index], shadow);
                shadow_comparar(("metodo " + c.name + "." + m.name).c_str(), diags, antes, shadow);
            }
        }
        for (const auto& ct : c.ctors) {
            auto cs = sig.ctors.find(ct.params.size());
            if (cs == sig.ctors.end()) continue;
            Emitter emitter(diags, &fns, &classes, imports);
            size_t  antes = diags.size();
            emitter.emit_ctor(c.name, sig.fields, ct, *mod.functions[cs->second]);
            if (shadow_check_activo()) {
                DiagnosticBag shadow;
                emitter.check_ctor(c.name, sig.fields, ct, *mod.functions[cs->second], shadow);
                shadow_comparar(("constructor " + c.name).c_str(), diags, antes, shadow);
            }
        }

        // The implicit constructor: a synthetic CtorDecl with one parameter per
        // field, in declaration order.
        if (c.ctors.empty() && !c.fields.empty()) {
            CtorDecl implicito;
            implicito.loc = c.loc;
            for (const auto& f : c.fields) {
                Param p;
                p.loc  = f.loc;
                p.name = f.name;
                p.type = f.type;
                implicito.params.push_back(std::move(p));
            }
            auto cs = sig.ctors.find(c.fields.size());
            if (cs != sig.ctors.end()) {
                Emitter emitter(diags, &fns, &classes, imports);
                size_t  antes = diags.size();
                emitter.emit_ctor(c.name, sig.fields, implicito,
                                  *mod.functions[cs->second]);
                if (shadow_check_activo()) {
                    DiagnosticBag shadow;
                    emitter.check_ctor(c.name, sig.fields, implicito, *mod.functions[cs->second],
                                       shadow);
                    shadow_comparar(("constructor implicito " + c.name).c_str(), diags, antes,
                                    shadow);
                }
            }
        }
    }
}

FunctionSigs build_functions(Module& mod, DiagnosticBag& diags) {
    FunctionSigs index;

    for (const auto& f : mod.program.functions) {
        if (native_id(f.name) >= 0) {
            diags.error(f.loc, "'" + f.name + "' es un builtin: elige otro name");
            continue;
        }
        if (index.count(f.name)) continue;   // the parser already reported the duplicate

        index[f.name] = make_sig(mod.functions.size(), f.params, diags);
        mod.functions.push_back(std::make_shared<Chunk>());
    }

    for (const auto& f : mod.program.functions) {
        auto it = index.find(f.name);
        if (it == index.end()) continue;
        Emitter emitter(diags, &index, nullptr, &mod.program.imports);
        size_t  antes = diags.size();
        emitter.emit_function(f, *mod.functions[it->second.index]);
        if (shadow_check_activo()) {
            DiagnosticBag shadow;
            emitter.check_function(f, *mod.functions[it->second.index], shadow);
            shadow_comparar(("funcion " + f.name).c_str(), diags, antes, shadow);
        }
    }
    return index;
}

void build_error_handlers(Module& mod, const FunctionSigs& fns,
                          const ClassSigs& sigs, DiagnosticBag& diags) {
    // An error handler can render a page too.
    TemplateCtx pctx{mod.program.app.templates_dir, &mod.templates};
    for (const auto& e : mod.program.errors) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
        size_t  antes = diags.size();
        if (!emitter.emit_error_handler(e, *chunk)) continue;
        if (shadow_check_activo()) {
            DiagnosticBag shadow;
            emitter.check_error_handler(e, *chunk, shadow);
            shadow_comparar(("on error " + std::to_string(e.code)).c_str(), diags, antes, shadow);
        }
        mod.error_handlers[e.code] = std::move(chunk);
    }
}

void build_routes(Module& mod, const ClassTable& classes, const AuthConfig& auth,
                  const FunctionSigs& fns, const ClassSigs& sigs,
                  DiagnosticBag& diags) {
    // The Module owns the table and outlives any in-flight request: the
    // dispatcher keeps its shared_ptr alive while the handler runs.
    const FunctionTable* fn_table = &mod.functions;
    // --native (phase 2): resolved once, like fn_table -- valid because
    // compile() already finished compiling native code before this is
    // called (see the comment on Module::native, project.hpp).
    const NativeDispatch native_table = mod.native ? mod.native->dispatch() : NativeDispatch{};
    // Templates live in the module, like the functions: the pointer is stable
    // as long as the module is, and the reload swap changes both at the same
    // time.
    const std::vector<Template>* tpl_table = &mod.templates;

    // Context the emitters need to compile the templates they find in a
    // render().
    TemplateCtx pctx{mod.program.app.templates_dir, &mod.templates};

    for (size_t ridx = 0; ridx < mod.program.routes.size(); ++ridx) {
        const auto& r = mod.program.routes[ridx];
        if (!r.origins.empty() && r.method != "WS")
            diags.error(r.loc, "origins() is only valid on ws routes");

        // ── ws routes ────────────────────────────────────────────────────────
        // The RFC 6455 handshake is done by the engine; here the VM is only
        // driven with the connection already established.
        if (r.method == "WS") {
            if (r.origins.empty()) {
                diags.error(r.loc, "a ws route needs origins(...): without an allowlist "
                                   "any site can open the connection "
                                   "from your user's browser");
                continue;
            }

            std::vector<ParamBind> ws_binds;
            if (!bind_params(r, classes, ws_binds, diags)) continue;
            bool body_param = false;
            for (const auto& b : ws_binds)
                if (b.kind == BindKind::Body) body_param = true;
            if (body_param) {
                diags.error(r.loc, "a ws route takes no body");
                continue;
            }

            auto    ws_chunk = std::make_shared<Chunk>();
            Emitter ws_emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
            size_t  antes = diags.size();
            if (!ws_emitter.emit_route(r, *ws_chunk)) continue;
            if (shadow_check_activo()) {
                DiagnosticBag shadow;
                ws_emitter.check_route(r, *ws_chunk, shadow);
                shadow_comparar(("ws " + r.pattern).c_str(), diags, antes, shadow);
            }

            ++mod.vm_routes;
            std::string ws_where = "WS " + r.pattern;
            lumen::App::WSOptions opts;
            opts.allowed_origins = r.origins;

            mod.rutas_informe.push_back({r.method, r.pattern, "ws"});
            mod.router.add_internal("GET", r.pattern,
                lumen::App::make_ws_handler(
                    [ws_chunk, ws_binds, ws_where, auth, fn_table, native_table, tpl_table]
                    (lumen::WSConnection conn, lumen::Request& req,
                     lumen::Response& res) -> lumen::Task<void> {
                        NativeCtx    ctx{req, res};
                ctx.templates = tpl_table;
                ctx.functions  = fn_table;
                        SessionState session;
                        Value        claims = Value::dict();
                        begin_auth(auth, req, session, claims, ctx);
                        ctx.ws              = &conn;
                        ctx.response_written = true;   // the upgrade already replied

                        std::vector<Value> args;
                        if (!prepare_args(ws_binds, fn_table, req, res, ctx, args)) co_return;

                        VM         vm;
                        VM::Result result = vm.start(*ws_chunk, std::move(args), ctx, fn_table, &native_table);

                        while (result.status == VM::Status::Suspended) {
                            Value produced = Value::null();

                            if (result.await_id == async_ws_recv_id()) {
                                // First suspension that returns a value: the
                                // message enters the VM as the result of the
                                // `await`.  null means the connection closed.
                                auto msg = co_await conn.recv();
                                if (msg && !msg->is_close())
                                    produced = Value::str(msg->data);
                            }
                            else if (is_db_await(result.await_id)) {
                                produced = co_await run_db(result, result.await_id,
                                                           req, ctx);
                            }
                            else if (result.await_id == async_sleep_id()) {
                                long long ms = result.await_args.empty()
                                             ? 0 : result.await_args[0].as_int();
                                ms = clamp_sleep_ms(ms);
                                co_await lumen::sleep(static_cast<int>(ms));
                                if (req.is_cancelled()) co_return;
                            }

                            result = vm.resume(std::move(produced), ctx);
                        }

                        if (result.status == VM::Status::Error) {
                            std::string at = result.error_loc.file
                                ? *result.error_loc.file + ":" +
                                  std::to_string(result.error_loc.line) + ":" +
                                  std::to_string(result.error_loc.col)
                                : ws_where;
                            lumen::log().error(at + ": " + result.error);
                        }
                    },
                    std::move(opts)));
            continue;
        }

        // ── sse routes ───────────────────────────────────────────────────────
        // The stream is opened before starting the VM and closed when the
        // handler ends; there is no final response to write.
        if (r.method == "SSE") {
            std::vector<ParamBind> sse_binds;
            if (!bind_params(r, classes, sse_binds, diags)) continue;
            for (const auto& b : sse_binds) {
                if (b.kind == BindKind::Body) {
                    diags.error(r.loc, "an sse route takes no body");
                    break;
                }
            }

            auto    sse_chunk = std::make_shared<Chunk>();
            Emitter sse_emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
            size_t  antes = diags.size();
            if (!sse_emitter.emit_route(r, *sse_chunk)) continue;
            if (shadow_check_activo()) {
                DiagnosticBag shadow;
                sse_emitter.check_route(r, *sse_chunk, shadow);
                shadow_comparar(("sse " + r.pattern).c_str(), diags, antes, shadow);
            }

            ++mod.vm_routes;
            std::string sse_where = "SSE " + r.pattern;
            mod.rutas_informe.push_back({r.method, r.pattern, "sse"});
            mod.router.add_internal("GET", r.pattern,
                [sse_chunk, sse_binds, sse_where, auth, fn_table, native_table, tpl_table](lumen::Request& req,
                                                        lumen::Response& res)
                    -> lumen::Task<void> {
                    NativeCtx    ctx{req, res};
                ctx.templates = tpl_table;
                ctx.functions  = fn_table;
                    SessionState session;
                    Value        claims = Value::dict();
                    begin_auth(auth, req, session, claims, ctx);

                    std::vector<Value> args;
                    if (!prepare_args(sse_binds, fn_table, req, res, ctx, args)) co_return;

                    // make_sse already writes the stream headers, so the
                    // response counts as sent from this point on.
                    auto writer = lumen::make_sse(res, req);
                    ctx.sse             = &writer;
                    ctx.response_written = true;

                    VM         vm;
                    VM::Result result = vm.start(*sse_chunk, std::move(args), ctx, fn_table, &native_table);

                    while (result.status == VM::Status::Suspended) {
                        Value produced = Value::null();
                        if (is_db_await(result.await_id)) {
                            produced = co_await run_db(result, result.await_id, req, ctx);
                        }
                        else if (result.await_id == async_sleep_id()) {
                            long long ms = result.await_args.empty()
                                         ? 0 : result.await_args[0].as_int();
                            ms = clamp_sleep_ms(ms);
                            co_await lumen::sleep(static_cast<int>(ms));
                            if (req.is_cancelled()) co_return;
                        }
                        result = vm.resume(std::move(produced), ctx);
                    }

                    if (result.status == VM::Status::Error) {
                        std::string at = result.error_loc.file
                            ? *result.error_loc.file + ":" +
                              std::to_string(result.error_loc.line) + ":" +
                              std::to_string(result.error_loc.col)
                            : sse_where;
                        lumen::log().error(at + ": " + result.error);
                    }
                });
            continue;
        }

        // The pattern is ALWAYS checked, before the declarative shortcut: a
        // route without logic can declare ':id' and not bind it too, and that
        // compiler promise cannot depend on which path the route takes.
        for (const auto& seg : pattern_params(r.pattern)) {
            bool recogido = false;
            for (const auto& p : r.params) if (p.name == seg) recogido = true;
            if (!recogido)
                diags.error(r.pattern_loc, "the pattern declares ':" + seg +
                                           "' but no parameter binds it");
        }

        // Level 1: declarative route → native action, zero bytecode.
        if (Action a = try_declarative(r, mod.program.app.templates_dir)) {
            ++mod.declarative_routes;
            mod.rutas_informe.push_back({r.method, r.pattern, "declarativa"});
            mod.router.add_internal(r.method, r.pattern,
                [a](lumen::Request& req, lumen::Response& res) -> lumen::Task<void> {
                    a(req, res);
                    co_return;
                });
            continue;
        }

        // Level 2: route with logic → bytecode on the VM.
        //
        // bind_params validates the parameters (a default on a path one,
        // File out of place, more than one body...) ALWAYS, regardless of
        // which level ends up serving the route -- level 1.5 does not use
        // `binds` directly (the native handler does its own binding,
        // identical in the rules but over C++ types instead of Value), but
        // a program with an invalid pattern/parameter has to fail
        // compilation all the same, and a route generate_native_route()
        // accepted is, by construction, a STRICTER subset of what
        // bind_params allows (no default, no File, no body) -- it should
        // never fail here if it already passed there.
        std::vector<ParamBind> binds;
        if (!bind_params(r, classes, binds, diags)) continue;

        // Nivel 1.5: ruta compilada nativamente (Fase 4/5, --native). Mismo
        // criterio de "todo o nada" que una funcion: generar_ruta_nativa()
        // (native_gen.cpp), invocado durante compile() -> compilar_nativo(),
        // ya decidio si esta ruta entera es representable (parametros
        // escalares de patron/query sin valor por defecto, cuerpo sin
        // sesion/JWT/render/contenedores, `await` limitado a `sleep`) -- si
        // lo es, mod.native trae el puntero ya resuelto por dlsym() y aqui
        // solo hace falta invocarlo. La funcion generada hace su PROPIO
        // binding de parametros (lee req.params/req.query ella misma) y
        // escribe la respuesta directamente sobre `res`: no pasa por
        // bind_params/prepare_args/begin_auth ni por el VM.
        //
        // Una ruta con `await` (RutaNativa::asincrona) se genero como
        // lumen::Task<void> de verdad -- su firma YA coincide exactamente
        // con Handler (ver types.hpp), asi que se registra directa, sin
        // ningun envoltorio: envolverla en otra corrutina que la
        // co_await-ara solo anadiria un frame sin necesidad.
        if (mod.native && ridx < mod.native->rutas_async_por_indice.size() &&
            mod.native->rutas_async_por_indice[ridx]) {
            ++mod.vm_routes;
            mod.rutas_informe.push_back({r.method, r.pattern, "nativa (async)"});
            mod.router.add_internal(r.method, r.pattern, mod.native->rutas_async_por_indice[ridx]);
            continue;
        }
        if (mod.native && ridx < mod.native->rutas_por_indice.size() &&
            mod.native->rutas_por_indice[ridx]) {
            auto fn = mod.native->rutas_por_indice[ridx];
            ++mod.vm_routes; // cuenta como ruta con logica, aunque no pase por el VM
            mod.rutas_informe.push_back({r.method, r.pattern, "nativa"});
            mod.router.add_internal(r.method, r.pattern,
                [fn](lumen::Request& req, lumen::Response& res) -> lumen::Task<void> {
                    fn(req, res);
                    co_return;
                });
            continue;
        }

        // Nivel 2: ruta con logica → bytecode sobre el VM.
        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
        size_t  antes = diags.size();
        if (!emitter.emit_route(r, *chunk)) continue;
        if (shadow_check_activo()) {
            DiagnosticBag shadow;
            emitter.check_route(r, *chunk, shadow);
            shadow_comparar((r.method + " " + r.pattern).c_str(), diags, antes, shadow);
        }

        ++mod.vm_routes;
        bool needs_upload = false;
        for (const auto& b : binds)
            if (b.kind == BindKind::File || b.kind == BindKind::FileList)
                needs_upload = true;

        std::string where = r.method + " " + r.pattern;
        mod.rutas_informe.push_back({r.method, r.pattern, "bytecode"});
        mod.router.add_internal(r.method, r.pattern,
            [chunk, binds, where, auth, needs_upload, fn_table, native_table, tpl_table](lumen::Request& req, lumen::Response& res)
                -> lumen::Task<void> {
                NativeCtx    ctx{req, res};
                ctx.templates = tpl_table;
                ctx.functions  = fn_table;
                SessionState session;
                Value        claims = Value::dict();
                begin_auth(auth, req, session, claims, ctx);

                // The multipart body is only parsed if some slot asks for it.
                std::vector<lumen::MultipartPart> parts;
                if (needs_upload) {
                    if (auto p = lumen::parse_multipart(req)) {
                        parts       = std::move(*p);
                        ctx.parts   = &parts;
                        ctx.uploads = true;
                    }
                }

                std::vector<Value> args;
                if (!prepare_args(binds, fn_table, req, res, ctx, args)) co_return;

                // The VM lives in this coroutine's frame, not in the thread: two
                // handlers suspended at once on the same core each have their own
                // stack and locals.  The ones that cannot suspend reuse one per
                // thread and save the two allocations.
                thread_local VM shared_vm;
                VM  own_vm;
                VM& vm = chunk->has_await ? own_vm : shared_vm;

                VM::Result result = vm.start(*chunk, std::move(args), ctx, fn_table, &native_table);

                // The VM does not know how to wait: every time it stops, the real
                // co_await happens here, on the engine, and the result is handed
                // back to it.
                while (result.status == VM::Status::Suspended) {
                    Value produced = Value::null();

                    if (is_db_await(result.await_id)) {
                        produced = co_await run_db(result, result.await_id, req, ctx);
                    }
                    else if (result.await_id == async_sleep_id()) {
                        long long ms = result.await_args.empty()
                                     ? 0 : result.await_args[0].as_int();
                        ms = clamp_sleep_ms(ms);
                        co_await lumen::sleep(static_cast<int>(ms));
                        // sleep() wakes early if the client disconnects; in that
                        // case there is no point in carrying on.
                        if (req.is_cancelled()) co_return;
                    }

                    result = vm.resume(std::move(produced), ctx);
                }

                if (result.status == VM::Status::Error) {
                    std::string at = result.error_loc.file
                        ? *result.error_loc.file + ":" +
                          std::to_string(result.error_loc.line) + ":" +
                          std::to_string(result.error_loc.col)
                        : where;
                    lumen::log().error(at + ": " + result.error);
                    Value::Dict d;
                    d["error"] = Value::str(result.error);
                    d["en"]    = Value::str(at);
                    responder(res, 500, Value::dict(std::move(d)));
                    co_return;
                }

                co_await rollback_pendientes(ctx, req);
                end_auth(auth, session, res);

                if (ctx.response_written) co_return;
                if (result.value.is_null()) { res.status(204).send(""); co_return; }
                res.header("Content-Type", "application/json; charset=utf-8")
                   .send(result.value.to_json_text());
            });
    }
}

} // namespace

// ─── Compilation ─────────────────────────────────────────────────────────────

std::shared_ptr<Module> compile(const std::vector<fs::path>& inputs,
                                DiagnosticBag& diags, bool native) {
    auto mod = std::make_shared<Module>();
    std::error_code ec;

    for (const auto& path : inputs) {
        auto src  = std::make_unique<SourceFile>();
        src->path = path.string();

        std::ifstream f(path, std::ios::binary);
        if (!f) {
            diags.error({}, "cannot read: " + src->path);
            continue;
        }
        std::ostringstream ss; ss << f.rdbuf();
        src->text = ss.str();

        auto stamp = fs::last_write_time(path, ec);
        if (!ec) mod->stamps.emplace_back(path, stamp);

        mod->files.push_back(std::move(src));
    }

    // Pass 1: lex and parse every file onto the same Program.
    for (const auto& src : mod->files) {
        Lexer  lexer(*src, diags);
        Parser parser(lexer.tokenize(), diags);
        parser.parse_into(mod->program);
    }

    // Modules are checked before anything else: importing one this binary does
    // not carry, or using it unconfigured, has to be said plainly.
    if (diags.empty()) {
        auto& reg = DbRegistry::instance();
        for (const auto& m : mod->program.imports) {
            if (!reg.has(m)) {
                auto disponibles = reg.available();
                std::string list_;
                for (const auto& d : disponibles) list_ += (list_.empty() ? "" : ", ") + d;
                diags.error({}, "module '" + m + "' is not compiled into this binary" +
                                (list_.empty() ? "" : "; disponibles: " + list_));
                continue;
            }
            auto it = mod->program.app.modules.find(m);
            if (it == mod->program.app.modules.end()) {
                diags.error(mod->program.app.loc,
                            "'import " + m + "' without its '" + m + ":' en app:");
                continue;
            }
            std::string err;
            if (!reg.activate(m, it->second, err)) diags.error(mod->program.app.loc, err);
        }
    }

    // Pass 2: resolve classes and build the route table.  Classes go first
    // because routes bind against them; within each pass the file order does
    // not matter.
    if (diags.empty()) {
        // Order: first the signatures of everything callable —standalone
        // functions, methods and constructors— and only then the bodies.  That
        // way anyone can call anyone regardless of declaration order.
        auto fns  = build_functions(*mod, diags);
        mod->function_sigs = fns;

        auto sigs = build_class_signatures(*mod, diags);
        emit_class_bodies(*mod, sigs, fns, diags);

        // --native (Fase 3): despues de las firmas/cuerpos de clase (necesita
        // ClassSigs para constructores/metodos) y antes de construir rutas,
        // para que puedan capturar mod->native.get() ya resuelto -- ver el
        // comentario sobre NativeModule en project.hpp. Solo si lo demas
        // compilo limpio: no tiene sentido invocar g++ sobre un programa que
        // de todas formas no se va a publicar.
        if (native && diags.empty())
            mod->native = compilar_nativo(mod->program, fns, sigs, ".lumen-native",
                                          mod->native_aviso);

        ClassTable classes;
        build_classes(mod->program, fns, sigs, &mod->program.imports, classes, diags);

        AuthConfig auth;
        auth.session_secret  = mod->program.app.session_secret;
        auth.session_max_age = mod->program.app.session_max_age;
        auth.session_secure  = mod->program.app.session_secure;
        auth.jwt_secret      = mod->program.app.jwt_secret;
        auth.jwt_issuer      = mod->program.app.jwt_issuer;

        if (diags.empty()) build_routes(*mod, classes, auth, fns, sigs, diags);
        if (diags.empty()) build_error_handlers(*mod, fns, sigs, diags);
        if (diags.empty()) mod->openapi = build_openapi(mod->program, classes);
    }

    // It is always returned: the caller checks diags.empty() to decide whether
    // to publish it.  See the note in project.hpp about SourceLoc lifetimes.
    return mod;
}

} // namespace lumen_script
