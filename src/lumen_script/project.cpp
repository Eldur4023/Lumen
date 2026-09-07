#include <lumen_script/project.hpp>
#include <lumen_script/plantilla.hpp>
#include <lumen_script/lexer.hpp>
#include <lumen_script/parser.hpp>
#include <lumen_script/emitter.hpp>
#include <lumen_script/vm.hpp>
#include <lumen_script/crypto.hpp>
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
#include <ctime>
#include <map>
#include <set>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lumen_script {

// ─── Entrada ─────────────────────────────────────────────────────────────────

bool resolve_inputs(const std::vector<std::string>& args,
                    std::vector<fs::path>& out,
                    std::string& error) {
    std::error_code ec;

    for (const auto& a : args) {
        fs::path p(a);
        if (!fs::exists(p, ec)) {
            error = "no existe: " + a;
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
                error = "el directorio no contiene ningun .lum: " + a;
                return false;
            }
        } else {
            out.push_back(p);
        }
    }

    // Orden estable para que los diagnosticos salgan siempre igual.  El orden
    // no afecta al significado: la resolucion de nombres es en dos pasadas.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    if (out.empty()) { error = "no hay ficheros que compilar"; return false; }
    return true;
}

std::string format_errors(const DiagnosticBag& diags,
                          const std::vector<std::unique_ptr<SourceFile>>& files) {
    std::vector<const SourceFile*> raw;
    raw.reserve(files.size());
    for (const auto& f : files) raw.push_back(f.get());
    return diags.format(raw);
}

// ─── Evaluacion de literales ─────────────────────────────────────────────────
//
// Una ruta declarativa solo puede contener valores constantes: si aparece algo
// que hay que calcular, la ruta necesita el VM y eso es otro hito.

namespace {

// Nombre de tipo de Lumen Script para un valor ya construido.  Se usa donde los datos
// son constantes y por tanto su tipo es exacto.
std::string tipo_lumen_script_de(const Value& v) {
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



// Igual, pero produciendo un Value en vez de un arbol nlohmann.
//
// La diferencia importa para el orden de las claves: nlohmann guarda sus
// objetos en un std::map y las ordena alfabeticamente, mientras que el VM las
// devuelve en el orden en que estan escritas.  Si las rutas declarativas usaran
// nlohmann, dos rutas del mismo .lum ordenarian distinto.
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

// Nombre de la funcion llamada, si es una llamada directa a un identificador.
const std::string* callee_name(const Expr& e) {
    if (e.kind != ExprKind::Call || !e.object) return nullptr;
    if (e.object->kind != ExprKind::Ident)     return nullptr;
    return &e.object->text;
}

// Responde con un cuerpo JSON construido a mano.
void responder(lumen::Response& res, int code, Value v) {
    res.status(code).json_text(v.to_json_text());
}

// Los errores del motor son siempre {"error": "..."} y a veces con una lista de
// mensajes: dos formas fijas que no necesitan nada mas.
void responder_error(lumen::Response& res, int code, const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    responder(res, code, Value::dict(std::move(d)));
}

void responder_error(lumen::Response& res, int code, const std::string& msg,
                     const std::vector<std::string>& mensajes) {
    Value::List l;
    l.reserve(mensajes.size());
    for (const auto& m : mensajes) l.push_back(Value::str(m));
    Value::Dict d;
    d["error"]    = Value::str(msg);
    d["mensajes"] = Value::list(std::move(l));
    responder(res, code, Value::dict(std::move(d)));
}

using Action = std::function<void(lumen::Request&, lumen::Response&)>;

// Traduce el `return <expr>` de una ruta declarativa a una accion nativa.
// Devuelve un Action vacio y anota el diagnostico si la expresion necesita
// evaluacion en runtime.
Action compile_return(const Expr& e, DiagnosticBag& diags,
                      const std::string& tpl_dir) {
    // return { ... }  /  return "literal"  → cuerpo JSON
    Value literal;
    if (const_eval_valor(e, literal)) {
        // El cuerpo se serializa AQUI, una vez.  Antes se guardaba el arbol
        // nlohmann y se hacia dump() en cada peticion: una ruta que se resuelve
        // entera al compilar no deberia serializar nada en caliente.
        std::string cuerpo = literal.to_json_text();
        return [cuerpo](lumen::Request&, lumen::Response& res) {
            res.header("Content-Type", "application/json; charset=utf-8").send(cuerpo);
        };
    }

    const std::string* fn = callee_name(e);
    if (!fn) {
        diags.error(e.loc, "esta expresion necesita el VM, que todavia no esta "
                           "implementado; en el hito 1 una ruta solo puede "
                           "devolver un valor constante o una llamada nativa "
                           "(render, text, html, json, status, redirect, send_file)");
        return {};
    }

    auto need_string = [&](size_t idx, const char* what) -> const std::string* {
        if (e.args.size() <= idx || e.args[idx].value->kind != ExprKind::StringLit) {
            diags.error(e.loc, std::string(*fn) + "() espera " + what +
                               " como cadena literal");
            return nullptr;
        }
        return &e.args[idx].value->text;
    };

    if (*fn == "render") {
        const std::string* tpl = need_string(0, "el nombre de la plantilla");
        if (!tpl) return {};

        // Los argumentos con nombre son las variables Jinja2 de la plantilla.
        // Se convierten a valores de Jinja2 AQUI, una vez: la ruta es
        // declarativa, asi que en caliente solo queda renderizar.
        Value::Dict data;
        for (size_t i = 1; i < e.args.size(); ++i) {
            const Arg& a = e.args[i];
            if (a.name.empty()) {
                diags.error(a.loc, "tras el nombre de la plantilla, los argumentos "
                                   "de render() van con nombre: clave=valor");
                return {};
            }
            Value v;
            if (!const_eval_valor(*a.value, v)) {
                diags.error(a.loc, "valor no constante en render(): requiere el VM");
                return {};
            }
            data[a.name] = std::move(v);
        }
        // Los datos son constantes, asi que la pagina se puede renderizar
        // ENTERA aqui: la ruta se queda en mandar unos bytes fijos.  Y de paso
        // los errores de la plantilla salen al compilar, como en las demas.
        const std::string name = *tpl;
        if (name.find("..") != std::string::npos ||
            std::filesystem::path(name).is_absolute()) {
            diags.error(e.loc, "nombre de plantilla no valido: '" + name + "'");
            return {};
        }
        std::ifstream f(std::filesystem::path(tpl_dir) / name, std::ios::binary);
        if (!f) {
            diags.error(e.loc, "no se encuentra la plantilla '" + name + "' en " + tpl_dir);
            return {};
        }
        const std::string fuente((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());

        // Aqui los datos son CONSTANTES, asi que el tipo de cada clave se sabe
        // exacto: la plantilla se comprueba contra los valores de verdad.
        std::vector<NombreTipado> claves;
        std::vector<Value>        valores;
        for (const auto& [k, v] : data) {
            claves.push_back({k, tipo_lumen_script_de(v)});
            valores.push_back(v);
        }

        Plantilla tpl_c;
        if (!compilar_plantilla(fuente, name, tpl_dir, claves, diags, tpl_c)) return {};

        lumen::Request  req_falsa;
        lumen::Response res_falsa;
        NativeCtx        ctx{req_falsa, res_falsa};
        std::string      html, err;
        if (!render_plantilla(tpl_c, std::move(valores), ctx, nullptr, html, err)) {
            diags.error(e.loc, "al renderizar '" + name + "': " + err);
            return {};
        }
        return [html](lumen::Request&, lumen::Response& res) {
            res.header("Content-Type", "text/html; charset=utf-8").send(html);
        };
    }

    if (*fn == "text" || *fn == "html") {
        const std::string* s = need_string(0, "el contenido");
        if (!s) return {};
        std::string body = *s;
        bool is_html = (*fn == "html");
        return [body, is_html](lumen::Request&, lumen::Response& res) {
            if (is_html) res.html(body); else res.text(body);
        };
    }

    if (*fn == "send_file") {
        const std::string* p = need_string(0, "la ruta del fichero");
        if (!p) return {};
        std::string path = *p;
        return [path](lumen::Request&, lumen::Response& res) {
            res.send_file(path);
        };
    }

    if (*fn == "status") {
        if (e.args.empty() || e.args[0].value->kind != ExprKind::IntLit) {
            diags.error(e.loc, "status() espera un codigo numerico");
            return {};
        }
        int code = static_cast<int>(e.args[0].value->int_value);
        return [code](lumen::Request&, lumen::Response& res) {
            res.status(code).send("");
        };
    }

    if (*fn == "redirect") {
        const std::string* target = need_string(0, "el destino");
        if (!target) return {};
        int code = 302;
        if (e.args.size() > 1) {
            if (e.args[1].value->kind != ExprKind::IntLit) {
                diags.error(e.loc, "el segundo argumento de redirect() es el codigo");
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
            diags.error(e.loc, "json() espera un unico argumento");
            return {};
        }
        Value v;
        if (!const_eval_valor(*e.args[0].value, v)) {
            diags.error(e.loc, "json() con valor no constante: requiere el VM");
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

// Extrae los nombres :param / {param} del patron de ruta.
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

// El identificador de un builtin asincrono es su indice en la tabla; se
// consulta una vez para no depender del orden.
int async_sleep_id() {
    static const int id = native_id("sleep");
    return id;
}

int async_ws_recv_id() {
    static const int id = native_id("__ws_recv");
    return id;
}

// El tope de pasos del VM se reinicia en cada suspension a proposito, para que
// un bucle de sse/ws legitimo pueda vivir horas (ver kStepLimit en vm.hpp).
// Pero eso deja un hueco: `while true: await sleep(0)` se suspende y reanuda
// sin avanzar apenas nada, asi que nunca acumula pasos entre dos suspensiones
// y el tope no lo ve nunca — un solo cliente podria fijar un hilo entero
// reprogramando un temporizador de 0 ms sin parar.  Un piso de 1 ms no lo
// impide del todo, pero lo acota a ~1000 reanudaciones/s por conexion en vez
// de tantas como el planificador quiera dar: ninguna carga legitima necesita
// menos de eso.
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

// Resuelve una suspension de base de datos.
//
// El primer argumento es siempre el nombre del modulo, que apila el emisor.
// Un fallo del motor no revienta el handler: llega como un valor con `error`,
// que el .lum puede mirar o dejar pasar.
lumen::Task<Value> run_db(const VM::Result& r, int op, lumen::Request& req,
                           NativeCtx& ctx) {
    if (r.await_args.empty() || !r.await_args[0].is_str())
        co_return db_error("consulta mal formada");

    const std::string mod = r.await_args[0].as_str();

    auto& reg    = DbRegistry::instance();
    auto* driver = reg.active(mod);
    auto* pool   = reg.pool(mod);
    if (!driver || !pool)
        co_return db_error("el modulo '" + mod + "' no esta configurado: "
                           "falta su bloque en app:");

    bool needs_sql = (op == async_db_query_id() || op == async_db_exec_id());
    std::string sql;
    std::vector<Value> params;
    if (needs_sql) {
        if (r.await_args.size() < 2 || !r.await_args[1].is_str())
            co_return db_error("falta la consulta SQL");
        sql    = r.await_args[1].as_str();
        params = std::vector<Value>(r.await_args.begin() + 2, r.await_args.end());
    }

    // Dentro de una transaccion, todo va por la conexion que la abrio.
    int  pin   = -1;
    auto pinit = ctx.pinned_workers.find(mod);
    if (pinit != ctx.pinned_workers.end()) pin = pinit->second;

    // last_id() se encamina a la conexion del ultimo exec: el identificador
    // generado no existe en las demas.
    if (pin < 0 && op == async_db_last_id()) {
        auto le = ctx.last_exec_workers.find(mod);
        if (le != ctx.last_exec_workers.end()) pin = le->second;
    }

    auto result   = std::make_shared<Value>(Value::null());
    auto errmsg   = std::make_shared<std::string>();
    auto used     = std::make_shared<int>(-1);

    co_await DbAwaitable{pool, req.loop,
        [driver, sql, params, op, result, errmsg, used](size_t worker) {
            *used = static_cast<int>(worker);
            std::string err;
            if (!driver->open(worker, err)) { *errmsg = err; return; }

            long long n = 0;
            if (op == async_db_query_id()) {
                Value rows;
                if (!driver->query(worker, sql, params, rows, err)) { *errmsg = err; return; }
                *result = std::move(rows);
            } else if (op == async_db_exec_id()) {
                if (!driver->exec(worker, sql, params, n, err)) { *errmsg = err; return; }
                *result = Value::integer(n);
            } else if (op == async_db_last_id()) {
                if (!driver->last_insert_id(worker, n, err)) { *errmsg = err; return; }
                *result = Value::integer(n);
            } else {
                const char* stmt = (op == async_db_begin_id())    ? "BEGIN"
                                 : (op == async_db_commit_id())   ? "COMMIT"
                                                                  : "ROLLBACK";
                if (!driver->exec(worker, stmt, {}, n, err)) { *errmsg = err; return; }
                *result = Value::boolean(true);
            }
        },
        pin};

    if (!errmsg->empty()) co_return db_error(*errmsg);

    if (op == async_db_exec_id()) ctx.last_exec_workers[mod] = *used;

    // La transaccion fija su conexion al abrirse y la suelta al cerrarse.
    if (op == async_db_begin_id())       ctx.pinned_workers[mod] = *used;
    else if (op == async_db_commit_id() ||
             op == async_db_rollback_id()) ctx.pinned_workers.erase(mod);

    co_return std::move(*result);
}

// Cierra las transacciones que el handler dejo abiertas.
//
// Sin esto, un `return` a mitad o un error dejarian la conexion dentro de una
// transaccion para siempre, y el siguiente que la cogiera del pool heredaria
// ese estado.
lumen::Task<void> rollback_pendientes(NativeCtx& ctx, lumen::Request& req) {
    if (ctx.pinned_workers.empty()) co_return;

    auto pendientes = ctx.pinned_workers;
    for (const auto& [mod, worker] : pendientes) {
        auto& reg    = DbRegistry::instance();
        auto* driver = reg.active(mod);
        auto* pool   = reg.pool(mod);
        if (!driver || !pool) continue;

        lumen::log().warn("transaccion de '" + mod + "' sin commit ni rollback: "
                           "se deshace");
        co_await DbAwaitable{pool, req.loop,
            [driver](size_t w) {
                long long n = 0;
                std::string err;
                driver->exec(w, "ROLLBACK", {}, n, err);
            },
            worker};
    }
    ctx.pinned_workers.clear();
}

// ─── Clases ──────────────────────────────────────────────────────────────────

struct ClassField {
    std::string name;
    std::string type;
    bool        optional = false;
};

// Una regla de `validate:` ya compilada: recibe los campos como locales, en el
// orden de declaracion, y devuelve un booleano.
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
            diags.error(c.loc, "la clase '" + c.name + "' ya esta declarada");
            continue;
        }

        auto info  = std::make_shared<ClassInfo>();
        info->name = c.name;

        std::vector<NombreTipado> field_names;
        bool ok = true;

        for (const auto& f : c.fields) {
            if (!is_scalar(f.type.name)) {
                diags.error(f.loc, "el tipo '" + f.type.str() + "' como campo todavia "
                                   "no esta implementado; de momento solo "
                                   "int, long, float, double, bool y string");
                ok = false;
                continue;
            }
            info->fields.push_back({f.name, f.type.name, f.type.optional});
            field_names.push_back({f.name, f.type.name});
        }
        if (!ok) continue;

        // Cada regla se compila contra los campos de la clase: si menciona un
        // nombre que no existe, el error sale aqui y no en produccion.
        for (const auto& r : c.rules) {
            auto    chunk = std::make_shared<Chunk>();
            Emitter emitter(diags, &fns, &sigs, imports);
            if (!emitter.emit_condition(*r.condition, field_names, *chunk)) continue;
            info->rules.push_back({chunk, r.message});
        }

        out[c.name] = std::move(info);
    }
}

// ─── Sesion firmada y JWT ────────────────────────────────────────────────────
//
// La sesion es una cookie firmada, como en Flask: sin estado en servidor, lo
// que encaja con un VM por peticion y N event loops sin nada que sincronizar.
//
// Formato:  base64url(json) "." base64url(hmac_sha256(secreto, base64url(json)))
//
// El contenido va firmado pero NO cifrado: el usuario puede leerlo, solo no
// puede falsificarlo.  No se guarda ahi nada que no pueda ver.

constexpr const char* kSessionCookie = "lumen_session";

std::string sign_session(const Value::Dict& data, const std::string& secret) {
    std::string payload = crypto::base64url_encode(
        Value::dict(data).to_json_text());
    std::string mac = crypto::base64url_encode(
        crypto::hmac_sha256(secret, payload));
    return payload + "." + mac;
}

// Devuelve false si la cookie falta, esta mal formada o la firma no cuadra.
// En cualquiera de esos casos la sesion arranca vacia, nunca a medias.
bool load_session(const std::string& cookie, const std::string& secret,
                  Value::Dict& out) {
    size_t dot = cookie.rfind('.');
    if (dot == std::string::npos) return false;

    std::string payload = cookie.substr(0, dot);
    std::string given   = cookie.substr(dot + 1);

    std::string expected = crypto::base64url_encode(
        crypto::hmac_sha256(secret, payload));
    if (!crypto::constant_time_equal(given, expected)) return false;

    std::string json_text;
    if (!crypto::base64url_decode(payload, json_text)) return false;

    Value v;
    if (!Value::parse_json(json_text, v) || !v.is_dict()) return false;
    out = v.as_dict();
    return true;
}

// Verifica un JWT HS256 y devuelve los claims.
//
// Comprueba alg, firma y expiracion.  Un token con alg "none", o con RS256
// cuando esperamos HS256, se rechaza: aceptar el alg que diga el token es la
// vulnerabilidad clasica de las librerias de JWT.
bool verify_jwt(const std::string& token, const std::string& secret,
                const std::string& issuer, Value& claims_out) {
    size_t p1 = token.find('.');
    if (p1 == std::string::npos) return false;
    size_t p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos) return false;

    std::string signing_input = token.substr(0, p2);
    std::string given_sig     = token.substr(p2 + 1);

    std::string expected = crypto::base64url_encode(
        crypto::hmac_sha256(secret, signing_input));
    if (!crypto::constant_time_equal(given_sig, expected)) return false;

    std::string header_text, payload_text;
    if (!crypto::base64url_decode(token.substr(0, p1), header_text)) return false;
    if (!crypto::base64url_decode(token.substr(p1 + 1, p2 - p1 - 1), payload_text))
        return false;

    Value header;
    if (!Value::parse_json(header_text, header) || !header.is_dict()) return false;
    {
        auto it = header.as_dict().find("alg");
        if (it == header.as_dict().end() || !it->second.is_str() ||
            it->second.as_str() != "HS256")
            return false;
    }

    Value payload;
    if (!Value::parse_json(payload_text, payload) || !payload.is_dict()) return false;

    if (auto it = payload.as_dict().find("exp");
        it != payload.as_dict().end() && it->second.is_num()) {
        const long long now = static_cast<long long>(std::time(nullptr));
        if (static_cast<long long>(it->second.as_float()) < now) return false;
    }
    if (!issuer.empty()) {
        auto it = payload.as_dict().find("iss");
        if (it != payload.as_dict().end() &&
            (!it->second.is_str() || it->second.as_str() != issuer))
            return false;
    }

    claims_out = std::move(payload);
    return true;
}

// Configuracion de autenticacion que cada handler necesita en runtime.
struct AuthConfig {
    std::string session_secret;
    int         session_max_age = 86400;
    bool        session_secure  = true;
    std::string jwt_secret;
    std::string jwt_issuer;
};

// Prepara sesion y claims antes de ejecutar el handler.
void begin_auth(const AuthConfig& cfg, lumen::Request& req,
                SessionState& session, Value& claims, NativeCtx& ctx) {
    session.secret = cfg.session_secret;
    if (!cfg.session_secret.empty()) {
        auto cookie = req.cookie(kSessionCookie);
        if (cookie) load_session(*cookie, cfg.session_secret, session.data);
        session.loaded = true;
    }
    ctx.session = &session;

    if (!cfg.jwt_secret.empty()) {
        auto auth = req.header("authorization");
        if (auth && auth->rfind("Bearer ", 0) == 0) {
            ctx.jwt_ok = verify_jwt(auth->substr(7), cfg.jwt_secret,
                                    cfg.jwt_issuer, claims);
        }
    }
    ctx.jwt_claims = &claims;
}

// Reescribe la cookie solo si el handler toco la sesion.
void end_auth(const AuthConfig& cfg, const SessionState& session,
              lumen::Response& res) {
    if (!session.dirty || cfg.session_secret.empty()) return;

    lumen::CookieOptions opts;
    opts.path      = "/";
    opts.http_only = true;                 // JS no la puede leer
    opts.secure    = cfg.session_secure;
    opts.same_site = lumen::SameSite::Lax;

    if (session.data.empty()) {
        res.clear_cookie(kSessionCookie, opts);
        return;
    }
    opts.max_age = cfg.session_max_age;
    res.cookie(kSessionCookie, sign_session(session.data, cfg.session_secret), opts);
}

// ─── Enlace de parametros ────────────────────────────────────────────────────

enum class BindKind { Path, Query, Body, File, FileList };

struct ParamBind {
    BindKind    kind = BindKind::Path;
    std::string name;
    std::string type;          // escalar, o el nombre de la clase si es Body
    bool        has_default = false;
    std::string default_text;
    std::shared_ptr<ClassInfo> cls;   // solo Body
};

// Convierte el texto crudo de la URL al tipo declarado.  Un valor mal formado
// es un 400: lo mando mal el cliente, no es un fallo del servidor.
bool coerce(const std::string& text, const std::string& type, Value& out) {
    try {
        if (type == "string")                    { out = Value::str(text); return true; }
        if (type == "int" || type == "long")     { out = Value::integer(std::stoll(text)); return true; }
        if (type == "float" || type == "double") { out = Value::real(std::stod(text)); return true; }
        if (type == "bool") {
            if (text == "true"  || text == "1") { out = Value::boolean(true);  return true; }
            if (text == "false" || text == "0") { out = Value::boolean(false); return true; }
            return false;
        }
    } catch (...) { return false; }
    return false;
}

// Comprueba que un valor JSON encaja con el tipo declarado del campo.
// No hay conversion entre familias: un string en un campo int es un error, no
// un intento de parseo.
// Comprueba que el valor recibido encaja con el tipo declarado en la clase.
//
// Es deliberadamente estricto: un "30" no vale donde se declaro un int.  Que el
// cuerpo diga una cosa y la clase otra es justo lo que la validacion existe
// para atrapar.
bool valor_encaja(const Value& v, const std::string& type, Value& out) {
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

// Valida los parametros contra el patron y produce el plan de enlace.
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
                        "el patron declara ':" + seg + "' pero ningun parametro lo recoge");
            ok = false;
        }
    }

    for (const auto& p : r.params) {
        bool in_path = std::find(in_pattern.begin(), in_pattern.end(), p.name)
                       != in_pattern.end();

        // File / List<File> se enlazan a las partes multipart con ese nombre.
        bool is_file      = (p.type.name == "File");
        bool is_file_list = (p.type.name == "List" && p.type.args.size() == 1 &&
                             p.type.args[0].name == "File");
        if (is_file || is_file_list) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' esta en el patron de ruta, "
                                   "asi que no puede ser un fichero subido");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "una ruta " + r.method + " no lleva cuerpo");
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

        // Un parametro cuyo tipo es una clase se enlaza al cuerpo de la
        // peticion: es la idea de FastAPI, el body es un parametro tipado mas.
        auto it = classes.find(p.type.name);
        if (it != classes.end()) {
            if (in_path) {
                diags.error(p.loc, "'" + p.name + "' esta en el patron de ruta, "
                                   "asi que no puede ser del tipo '" + p.type.name + "'");
                ok = false;
                continue;
            }
            if (seen_body) {
                diags.error(p.loc, "solo puede haber un parametro de cuerpo por ruta");
                ok = false;
                continue;
            }
            if (r.method == "GET" || r.method == "DELETE") {
                diags.error(p.loc, "una ruta " + r.method + " no lleva cuerpo");
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
            diags.error(p.loc, "tipo desconocido: '" + p.type.str() + "'");
            ok = false;
            continue;
        }
        if (p.type.optional) {
            diags.error(p.loc, "los parametros opcionales todavia no estan implementados");
            ok = false;
            continue;
        }

        b.kind = in_path ? BindKind::Path : BindKind::Query;

        if (in_path && p.default_value) {
            diags.error(p.loc, "un parametro de ruta no puede tener valor por defecto");
            ok = false;
            continue;
        }
        if (!in_path && p.default_value) {
            const Expr& d = *p.default_value;
            if      (d.kind == ExprKind::StringLit) b.default_text = d.text;
            else if (d.kind == ExprKind::IntLit)    b.default_text = std::to_string(d.int_value);
            else if (d.kind == ExprKind::BoolLit)   b.default_text = d.bool_value ? "true" : "false";
            else {
                diags.error(p.loc, "el valor por defecto tiene que ser una constante");
                ok = false;
                continue;
            }
            b.has_default = true;
        }

        out.push_back(std::move(b));
    }
    return ok;
}

// Reconoce la forma puramente declarativa: un unico `return` que se resuelve
// entero en compilacion.  Estas rutas no ejecutan ni un paso de bytecode.
Action try_declarative(const RouteDecl& r, const std::string& tpl_dir) {
    // Una ruta con guardas NUNCA puede tomar la via declarativa: la accion
    // nativa no las ejecuta, asi que se saltaria la proteccion del grupo.
    if (!r.guards.empty())                                       return {};
    if (!r.params.empty())                                       return {};
    if (r.body.size() != 1)                                      return {};
    if (r.body[0]->kind != StmtKind::Return || !r.body[0]->value) return {};

    DiagnosticBag scratch;
    Action a = compile_return(*r.body[0]->value, scratch, tpl_dir);
    return scratch.empty() ? a : Action{};
}

// Construye la instancia a partir del cuerpo JSON.
//
// Faltar un campo obligatorio, o traerlo con el tipo equivocado, o incumplir
// una regla de validate, es 422 con la lista completa de motivos: se reportan
// todos de una vez, no el primero.  El handler no llega a ejecutarse.
bool bind_body(const ClassInfo& ci, const FunctionTable* fns,
               lumen::Request& req, lumen::Response& res,
               NativeCtx& ctx, Value& out) {
    Value body;
    if (!Value::parse_json(req.body, body)) {
        responder_error(res, 400, "JSON invalido");
        return false;
    }
    if (!body.is_dict()) {
        responder_error(res, 422, "Validacion fallida",
                        {"el cuerpo tiene que ser un objeto JSON"});
        return false;
    }

    std::vector<std::string> messages;
    Value::Dict              fields;
    std::vector<Value>       ordered;

    for (const auto& f : ci.fields) {
        auto it = body.as_dict().find(f.name);
        if (it == body.as_dict().end() || it->second.is_null()) {
            if (!f.optional) messages.push_back(f.name + ": obligatorio");
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        Value v;
        if (!valor_encaja(it->second, f.type, v)) {
            messages.push_back(f.name + ": se esperaba " + f.type);
            fields[f.name] = Value::null();
            ordered.push_back(Value::null());
            continue;
        }
        fields[f.name] = v;
        ordered.push_back(std::move(v));
    }

    // Las reglas solo se ejecutan si los campos estan bien: evaluarlas sobre
    // valores ausentes daria errores de tipo en vez del mensaje util.
    if (messages.empty()) {
        thread_local VM rule_vm;
        for (const auto& rule : ci.rules) {
            VM::Result r = rule_vm.start(*rule.chunk, ordered, ctx, fns);
            if (r.status != VM::Status::Done) {
                lumen::log().error("validate de " + ci.name + ": " + r.error);
                responder_error(res, 500, r.error);
                return false;
            }
            if (!r.value.truthy()) messages.push_back(rule.message);
        }
    }

    if (!messages.empty()) {
        // Se dejan a mano del manejador `on error 422` de esta misma peticion.
        last_validation_messages() = messages;
        responder_error(res, 422, "Validacion fallida", messages);
        return false;
    }

    out = Value::dict(std::move(fields));
    return true;
}

// Rellena las ranuras de los parametros a partir de la peticion.
// Devuelve false, con la respuesta ya escrita, si algun valor no encaja.
// Un File del lenguaje son los metadatos mas el indice de su parte en
// ctx.uploads; los bytes no viajan dentro del Value.
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
    // Se limpian al empezar: un 422 que el handler escriba a mano no debe
    // heredar los mensajes de una validacion anterior en este hilo.
    last_validation_messages().clear();
    out.reserve(binds.size());
    for (const auto& b : binds) {
        if (b.kind == BindKind::File || b.kind == BindKind::FileList) {
            if (!ctx.uploads) {
                responder_error(res, 400, "se esperaba multipart/form-data");
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
                                {b.name + ": falta el fichero"});
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
                // Un campo de texto de un formulario multipart es una parte mas,
                // igual que un File, solo que sin filename.  Antes esta rama no
                // existia: un parametro escalar en una ruta con File siempre
                // salia vacio, sin error, porque solo se miraba la query string.
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
            d["error"]    = Value::str("parametro invalido");
            d["param"]    = Value::str(b.name);
            d["esperado"] = Value::str(b.type);
            d["recibido"] = Value::str(raw);
            responder(res, 400, Value::dict(std::move(d)));
            return false;
        }
        out.push_back(std::move(v));
    }
    return true;
}

// ─── OpenAPI desde el AST ────────────────────────────────────────────────────
//
// La version C++ de esto eran 347 lineas de metaprogramacion que deducian el
// esquema construyendo un T{} por defecto e inspeccionando el JSON resultante.
// Con un AST delante, los nombres y los tipos ya estan ahi: es recorrer
// declaraciones.

std::string openapi_type(const std::string& t) {
    if (t == "int" || t == "long")        return "integer";
    if (t == "float" || t == "double")    return "number";
    if (t == "bool")                      return "boolean";
    return "string";
}

// El patron de Lumen Script usa :nombre; OpenAPI usa {nombre}.
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

// Atajos para montar el documento sin ahogarse en Value::Dict.
Value jstr(std::string v) { return Value::str(std::move(v)); }

Value jobj(std::initializer_list<std::pair<const char*, Value>> campos) {
    Value::Dict d;
    d.reservar(campos.size());
    for (const auto& [k, v] : campos) d[k] = v;
    return Value::dict(std::move(d));
}

// El documento se construye y se serializa UNA vez, al compilar el .lum.
// Antes se guardaba el arbol y se hacia dump() en cada peticion a
// /openapi.json, que es trabajo en caliente para algo que no cambia.
std::string build_openapi(const Program& program, const ClassTable& classes) {
    Value::Dict doc;
    doc["openapi"] = jstr("3.0.3");
    doc["info"]    = jobj({
        {"title",   jstr(program.app.name.empty() ? "Lumen API" : program.app.name)},
        {"version", jstr(program.app.version.empty() ? "0.1.0" : program.app.version)},
    });

    // Las clases se publican como esquemas reutilizables.  Los mensajes de
    // `validate:` se adjuntan como descripcion: son las reglas reales que
    // aplica el servidor, asi que documentan mejor que cualquier texto aparte.
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
            std::string desc = "Reglas de validacion:";
            for (const auto& r : info->rules) desc += "\n- " + r.message;
            schema["description"] = jstr(std::move(desc));
        }
        schemas[name] = Value::dict(std::move(schema));
    }
    if (!schemas.empty())
        doc["components"] = jobj({{"schemas", Value::dict(std::move(schemas))}});

    Value::Dict paths;

    for (const auto& r : program.routes) {
        // Las rutas de flujo no encajan en OpenAPI 3.0: se anuncian como GET
        // con la respuesta que realmente devuelven, sin fingir un esquema.
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
                {"description", jstr("Flujo de eventos")},
                {"content",     jobj({{"text/event-stream", Value::dict()}})},
            });
            op["summary"] = jstr("Server-Sent Events");
        } else if (r.method == "WS") {
            responses["101"] = jobj({{"description", jstr("Cambio a WebSocket")}});
            op["summary"]    = jstr("WebSocket");
        } else {
            responses["200"] = jobj({{"description", jstr("OK")}});
        }
        // Solo se declaran los codigos que el servidor produce de verdad.
        if (!body_class.empty() && body_class != "__multipart")
            responses["422"] = jobj({{"description", jstr("Validacion fallida")}});
        if (!r.guards.empty())
            responses["403"] = jobj({{"description", jstr("Guarda del grupo no superada")}});

        // Cada codigo de `on error` declarado se anuncia en todas las rutas.
        for (const auto& e : program.errors)
            if (e.code >= 400)
                responses[std::to_string(e.code)] =
                    jobj({{"description", jstr("Manejador propio")}});

        op["responses"] = Value::dict(std::move(responses));

        // Varios metodos pueden compartir ruta, asi que se acumula sobre la que
        // ya hubiera en lugar de sobrescribirla.
        Value& entrada = paths[openapi_path(r.pattern)];
        if (!entrada.is_dict()) entrada = Value::dict();
        entrada.as_dict()[method] = Value::dict(std::move(op));
    }

    doc["paths"] = Value::dict(std::move(paths));
    return Value::dict(std::move(doc)).to_json_text();
}

// Las funciones se compilan antes que rutas y manejadores, y todas ven la
// tabla completa: asi pueden llamarse entre si sin importar el orden en que se
// declararon ni el fichero en que estan.
// Rellena FnSig a partir de una lista de parametros, comprobando que ningun
// obligatorio va detras de uno con valor por defecto.
FnSig make_sig(size_t index, const std::vector<Param>& params, DiagnosticBag& diags) {
    FnSig sig;
    sig.index = index;
    bool seen_default = false;
    for (const auto& p : params) {
        sig.defaults.push_back(p.default_value.get());
        if (p.default_value) seen_default = true;
        else {
            if (seen_default)
                diags.error(p.loc, "un parametro sin valor por defecto no puede ir "
                                   "despues de uno que lo tiene");
            ++sig.required;
        }
    }
    return sig;
}

// Metodos y constructores se compilan como funciones con `this` de primer
// parametro, asi que van a la misma tabla que las funciones sueltas.
ClassSigs build_class_signatures(Module& mod, DiagnosticBag& diags) {
    ClassSigs out;

    for (const auto& c : mod.program.classes) {
        if (out.count(c.name)) continue;      // duplicado ya reportado
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

        // Sin constructor declarado, se ofrece el de mapeo completo: todos los
        // campos en orden.
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
            emitter.emit_method(c.name, m, *mod.functions[ms->second.index]);
        }
        for (const auto& ct : c.ctors) {
            auto cs = sig.ctors.find(ct.params.size());
            if (cs == sig.ctors.end()) continue;
            Emitter emitter(diags, &fns, &classes, imports);
            emitter.emit_ctor(c.name, sig.fields, ct, *mod.functions[cs->second]);
        }

        // El constructor implicito: un CtorDecl sintetico con un parametro por
        // campo, en orden de declaracion.
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
                emitter.emit_ctor(c.name, sig.fields, implicito,
                                  *mod.functions[cs->second]);
            }
        }
    }
}

FunctionSigs build_functions(Module& mod, DiagnosticBag& diags) {
    FunctionSigs index;

    for (const auto& f : mod.program.functions) {
        if (native_id(f.name) >= 0) {
            diags.error(f.loc, "'" + f.name + "' es un builtin: elige otro nombre");
            continue;
        }
        if (index.count(f.name)) continue;   // el parser ya reporto el duplicado

        index[f.name] = make_sig(mod.functions.size(), f.params, diags);
        mod.functions.push_back(std::make_shared<Chunk>());
    }

    for (const auto& f : mod.program.functions) {
        auto it = index.find(f.name);
        if (it == index.end()) continue;
        Emitter emitter(diags, &index, nullptr, &mod.program.imports);
        emitter.emit_function(f, *mod.functions[it->second.index]);
    }
    return index;
}

void build_error_handlers(Module& mod, const FunctionSigs& fns,
                          const ClassSigs& sigs, DiagnosticBag& diags) {
    // Un manejador de error tambien puede renderizar una pagina.
    PlantillaCtx pctx{mod.program.app.templates_dir, &mod.plantillas};
    for (const auto& e : mod.program.errors) {
        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
        if (!emitter.emit_error_handler(e, *chunk)) continue;
        mod.error_handlers[e.code] = std::move(chunk);
    }
}

void build_routes(Module& mod, const ClassTable& classes, const AuthConfig& auth,
                  const FunctionSigs& fns, const ClassSigs& sigs,
                  DiagnosticBag& diags) {
    // El Module posee la tabla y sobrevive a cualquier peticion en vuelo: el
    // dispatcher mantiene vivo su shared_ptr mientras el handler se ejecuta.
    const FunctionTable* fn_table = &mod.functions;
    // Las plantillas viven en el modulo, como las funciones: el puntero es
    // estable mientras el modulo lo este, y el swap de recarga cambia los dos
    // a la vez.
    const std::vector<Plantilla>* tpl_table = &mod.plantillas;

    // Contexto que necesitan los emisores para compilar las plantillas que
    // encuentren en un render().
    PlantillaCtx pctx{mod.program.app.templates_dir, &mod.plantillas};

    for (const auto& r : mod.program.routes) {
        if (!r.origins.empty() && r.method != "WS")
            diags.error(r.loc, "origins() solo es valido en rutas ws");

        // ── Rutas ws ─────────────────────────────────────────────────────────
        // El handshake RFC 6455 lo hace el motor; aqui solo se conduce el VM
        // con la conexion ya establecida.
        if (r.method == "WS") {
            if (r.origins.empty()) {
                diags.error(r.loc, "una ruta ws necesita origins(...): sin lista "
                                   "blanca, cualquier web puede abrir la conexion "
                                   "desde el navegador de tu usuario");
                continue;
            }

            std::vector<ParamBind> ws_binds;
            if (!bind_params(r, classes, ws_binds, diags)) continue;
            bool body_param = false;
            for (const auto& b : ws_binds)
                if (b.kind == BindKind::Body) body_param = true;
            if (body_param) {
                diags.error(r.loc, "una ruta ws no lleva cuerpo");
                continue;
            }

            auto    ws_chunk = std::make_shared<Chunk>();
            Emitter ws_emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
            if (!ws_emitter.emit_route(r, *ws_chunk)) continue;

            ++mod.vm_routes;
            std::string ws_where = "WS " + r.pattern;
            lumen::App::WSOptions opts;
            opts.allowed_origins = r.origins;

            mod.router.add_internal("GET", r.pattern,
                lumen::App::make_ws_handler(
                    [ws_chunk, ws_binds, ws_where, auth, fn_table, tpl_table]
                    (lumen::WSConnection conn, lumen::Request& req,
                     lumen::Response& res) -> lumen::Task<void> {
                        NativeCtx    ctx{req, res};
                ctx.plantillas = tpl_table;
                ctx.funciones  = fn_table;
                        SessionState session;
                        Value        claims = Value::dict();
                        begin_auth(auth, req, session, claims, ctx);
                        ctx.ws              = &conn;
                        ctx.response_written = true;   // el upgrade ya respondio

                        std::vector<Value> args;
                        if (!prepare_args(ws_binds, fn_table, req, res, ctx, args)) co_return;

                        VM         vm;
                        VM::Result result = vm.start(*ws_chunk, std::move(args), ctx, fn_table);

                        while (result.status == VM::Status::Suspended) {
                            Value produced = Value::null();

                            if (result.await_id == async_ws_recv_id()) {
                                // Primera suspension que devuelve valor: el
                                // mensaje entra en el VM como resultado del
                                // `await`.  null significa conexion cerrada.
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

        // ── Rutas sse ────────────────────────────────────────────────────────
        // El flujo se abre antes de arrancar el VM y se cierra al terminar el
        // handler; no hay respuesta final que escribir.
        if (r.method == "SSE") {
            std::vector<ParamBind> sse_binds;
            if (!bind_params(r, classes, sse_binds, diags)) continue;
            for (const auto& b : sse_binds) {
                if (b.kind == BindKind::Body) {
                    diags.error(r.loc, "una ruta sse no lleva cuerpo");
                    break;
                }
            }

            auto    sse_chunk = std::make_shared<Chunk>();
            Emitter sse_emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
            if (!sse_emitter.emit_route(r, *sse_chunk)) continue;

            ++mod.vm_routes;
            std::string sse_where = "SSE " + r.pattern;
            mod.router.add_internal("GET", r.pattern,
                [sse_chunk, sse_binds, sse_where, auth, fn_table, tpl_table](lumen::Request& req,
                                                        lumen::Response& res)
                    -> lumen::Task<void> {
                    NativeCtx    ctx{req, res};
                ctx.plantillas = tpl_table;
                ctx.funciones  = fn_table;
                    SessionState session;
                    Value        claims = Value::dict();
                    begin_auth(auth, req, session, claims, ctx);

                    std::vector<Value> args;
                    if (!prepare_args(sse_binds, fn_table, req, res, ctx, args)) co_return;

                    // make_sse escribe ya las cabeceras del flujo, asi que la
                    // respuesta cuenta como emitida desde este momento.
                    auto writer = lumen::make_sse(res, req);
                    ctx.sse             = &writer;
                    ctx.response_written = true;

                    VM         vm;
                    VM::Result result = vm.start(*sse_chunk, std::move(args), ctx, fn_table);

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

        // El patron se comprueba SIEMPRE, antes del atajo declarativo: una ruta
        // sin logica tambien puede declarar ':id' y no recogerlo, y esa promesa
        // del compilador no puede depender de por que camino vaya la ruta.
        for (const auto& seg : pattern_params(r.pattern)) {
            bool recogido = false;
            for (const auto& p : r.params) if (p.name == seg) recogido = true;
            if (!recogido)
                diags.error(r.pattern_loc, "el patron declara ':" + seg +
                                           "' pero ningun parametro lo recoge");
        }

        // Nivel 1: ruta declarativa → accion nativa, cero bytecode.
        if (Action a = try_declarative(r, mod.program.app.templates_dir)) {
            ++mod.declarative_routes;
            mod.router.add_internal(r.method, r.pattern,
                [a](lumen::Request& req, lumen::Response& res) -> lumen::Task<void> {
                    a(req, res);
                    co_return;
                });
            continue;
        }

        // Nivel 2: ruta con logica → bytecode sobre el VM.
        std::vector<ParamBind> binds;
        if (!bind_params(r, classes, binds, diags)) continue;

        auto    chunk = std::make_shared<Chunk>();
        Emitter emitter(diags, &fns, &sigs, &mod.program.imports, &pctx);
        if (!emitter.emit_route(r, *chunk)) continue;

        ++mod.vm_routes;
        bool needs_upload = false;
        for (const auto& b : binds)
            if (b.kind == BindKind::File || b.kind == BindKind::FileList)
                needs_upload = true;

        std::string where = r.method + " " + r.pattern;
        mod.router.add_internal(r.method, r.pattern,
            [chunk, binds, where, auth, needs_upload, fn_table, tpl_table](lumen::Request& req, lumen::Response& res)
                -> lumen::Task<void> {
                NativeCtx    ctx{req, res};
                ctx.plantillas = tpl_table;
                ctx.funciones  = fn_table;
                SessionState session;
                Value        claims = Value::dict();
                begin_auth(auth, req, session, claims, ctx);

                // Solo se parsea el cuerpo multipart si alguna ranura lo pide.
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

                // El VM vive en el marco de esta corrutina, no en el hilo: dos
                // handlers suspendidos a la vez sobre el mismo core tienen cada
                // uno su pila y sus locales.  Los que no pueden suspenderse
                // reutilizan uno por hilo y se ahorran las dos reservas.
                thread_local VM shared_vm;
                VM  own_vm;
                VM& vm = chunk->has_await ? own_vm : shared_vm;

                VM::Result result = vm.start(*chunk, std::move(args), ctx, fn_table);

                // El VM no sabe esperar: cada vez que se detiene, el co_await
                // de verdad ocurre aqui, sobre el motor, y se le devuelve el
                // resultado.
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
                        // sleep() despierta antes si el cliente se desconecta;
                        // en ese caso no tiene sentido seguir ejecutando.
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

// ─── Compilacion ─────────────────────────────────────────────────────────────

std::shared_ptr<Module> compile(const std::vector<fs::path>& inputs,
                                DiagnosticBag& diags) {
    auto mod = std::make_shared<Module>();
    std::error_code ec;

    for (const auto& path : inputs) {
        auto src  = std::make_unique<SourceFile>();
        src->path = path.string();

        std::ifstream f(path, std::ios::binary);
        if (!f) {
            diags.error({}, "no se puede leer: " + src->path);
            continue;
        }
        std::ostringstream ss; ss << f.rdbuf();
        src->text = ss.str();

        auto stamp = fs::last_write_time(path, ec);
        if (!ec) mod->stamps.emplace_back(path, stamp);

        mod->files.push_back(std::move(src));
    }

    // Pasada 1: lexar y parsear todos los ficheros sobre el mismo Program.
    for (const auto& src : mod->files) {
        Lexer  lexer(*src, diags);
        Parser parser(lexer.tokenize(), diags);
        parser.parse_into(mod->program);
    }

    // Los modulos se comprueban antes que nada: importar uno que este binario
    // no trae, o usarlo sin configurar, tiene que decirse claro.
    if (diags.empty()) {
        auto& reg = DbRegistry::instance();
        for (const auto& m : mod->program.imports) {
            if (!reg.has(m)) {
                auto disponibles = reg.available();
                std::string lista;
                for (const auto& d : disponibles) lista += (lista.empty() ? "" : ", ") + d;
                diags.error({}, "el modulo '" + m + "' no esta compilado en este binario" +
                                (lista.empty() ? "" : "; disponibles: " + lista));
                continue;
            }
            auto it = mod->program.app.modules.find(m);
            if (it == mod->program.app.modules.end()) {
                diags.error(mod->program.app.loc,
                            "'import " + m + "' sin su bloque '" + m + ":' en app:");
                continue;
            }
            std::string err;
            if (!reg.activate(m, it->second, err)) diags.error(mod->program.app.loc, err);
        }
    }

    // Pasada 2: resolver clases y construir la tabla de rutas.  Las clases van
    // primero porque las rutas se enlazan contra ellas; dentro de cada pasada el
    // orden de los ficheros es indiferente.
    if (diags.empty()) {
        // Orden: primero las firmas de todo lo llamable —funciones sueltas,
        // metodos y constructores—, y solo despues los cuerpos.  Asi cualquiera
        // puede llamar a cualquiera sin importar el orden de declaracion.
        auto fns  = build_functions(*mod, diags);
        auto sigs = build_class_signatures(*mod, diags);
        emit_class_bodies(*mod, sigs, fns, diags);

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

    // Se devuelve siempre: el llamante mira diags.empty() para saber si
    // publicarlo.  Ver la nota en project.hpp sobre la vida de los SourceLoc.
    return mod;
}

} // namespace lumen_script
