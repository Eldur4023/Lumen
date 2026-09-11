#include <lumen_script/native_gen.hpp>
#include <lumen_script/natives.hpp>

#include <algorithm>
#include <cstdio>
#include <map>
#include <sstream>

namespace lumen_script {
namespace {

// Los mismos native_id() que resuelve member_native_id("sqlite"/"postgres"/
// "mysql", "query"/"exec"/"last_id") en el checker real (natives.cpp) --
// las tres modulos comparten el mismo id para cada operacion (una sola
// entrada "__db_query"/"__db_exec"/"__db_last_id" en la tabla de nativos),
// asi que basta con el nombre de la operacion, sin mirar de que modulo
// vino la llamada.
int db_query_id()    { static const int id = native_id("__db_query");    return id; }
int db_exec_id()     { static const int id = native_id("__db_exec");     return id; }
int db_last_id_id()  { static const int id = native_id("__db_last_id");  return id; }
int db_begin_id()    { static const int id = native_id("__db_begin");    return id; }
int db_commit_id()   { static const int id = native_id("__db_commit");   return id; }
int db_rollback_id() { static const int id = native_id("__db_rollback"); return id; }

// Fase 5.9: state.incr/decr/get/set/remove (natives.cpp: SharedState, ya
// expuesta en natives.hpp -- incluida sin condiciones desde la Fase 5.7
// para last_validation_messages()) son ReservedMemberCall, no
// DbModuleCall: nunca asincronos (no hay ningun `await state....` en el
// lenguaje), y su native_id es uno solo por operacion, igual que
// db_query_id() -- da igual si el llamante escribio `state.incr(...)`,
// solo hay un modulo "state" posible.
int state_incr_id()   { static const int id = native_id("__state_incr");   return id; }
int state_decr_id()   { static const int id = native_id("__state_decr");   return id; }
int state_get_id()    { static const int id = native_id("__state_get");    return id; }
int state_set_id()    { static const int id = native_id("__state_set");    return id; }
int state_remove_id() { static const int id = native_id("__state_remove"); return id; }

// Los unicos elementos de List o valores de Dict que esta fase sabe
// representar -- un nivel, sin anidar (List<List<int>>, Dict<string,List<int>>
// quedan fuera: su elemento/valor no es ninguno de estos cuatro).
// Type::Kind::Json entra tambien -- Fase 5.5 (COMPILACION-NATIVA.md): un
// List<Json>/Dict<string,Json> (la forma real de una fila de base de
// datos) se representa igual que un Json suelto, ver tipo_cpp() mas abajo.
bool tipo_elemento_contenedor_soportado(const Type& elem) {
    if (elem.is_optional()) return false;
    switch (elem.kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::String:
        case Type::Kind::Json:
            return true;
        default:
            return false;
    }
}

// Los unicos Type que esta fase sabe representar de forma nativa -- ver la
// tabla de §7. `?` (optional) haria falta empaquetarlo (std::optional<T> o
// un centinela) y esta fase no lo cubre todavia: una funcion con un
// parametro/campo/retorno opcional se queda en bytecode por ahora.
//
// `string` entra aqui como std::string por VALOR, no con el refcount
// intrusivo que describe §8 para listas/diccionarios/instancias -- porque en
// Lumen Script una cadena es inmutable (concatenar produce una cadena nueva,
// nunca muta la existente), asi que compartirla o copiarla es exactamente lo
// mismo desde fuera: no hay manera de observar la diferencia. `List`/`Dict`,
// en cambio, SI son mutables (`.add()`/asignar por indice) y SI necesitan
// semantica de referencia real -- ver LList/LDict en list_runtime_prelude()/
// dict_runtime_prelude() y el comentario de §8.
//
// Dict NO admite lectura por indice en esta fase (`d[k]` con `k` ausente):
// el VM devuelve `null` en ese caso (vm.cpp::GetIndex), un tipo distinto del
// valor declarado que esta fase no puede representar en una ranura de tipo
// fijo -- la misma clase de ambiguedad que `a / b` entre dos int (ver la
// correccion critica de mas arriba), asi que se trata igual: no demostrable,
// se queda fuera (Comprobador::tipo_provable, caso Index). Si SE admite
// escribir (`d[k] = v`, siempre valido en el VM) y los dos metodos que
// reconoce metodos_de() para Dict: `has`/`keys`.
// `clases`, cuando se pasa, permite ademas Type::Kind::Class -- solo si esa
// clase concreta tiene entrada en la tabla (construir_clases() solo mete
// las que son representables: todos los campos escalares, ninguno
// opcional). Sin `clases` (el valor por defecto), cualquier Class se
// rechaza -- es lo que ya quiere tipo_elemento_contenedor_soportado() (una
// clase nunca es valida como elemento de List/Dict; el lenguaje tampoco lo
// permite como campo de otra clase) y tipo_abi_soportado() (una clase
// nunca cruza la ABI, sea representable o no).
bool tipo_soportado(const Type& t, const TablaClases* clases = nullptr) {
    if (t.is_optional()) return false;
    switch (t.kind()) {
        case Type::Kind::Int:
        case Type::Kind::Float:
        case Type::Kind::Bool:
        case Type::Kind::Void:
        case Type::Kind::String:
        // Fase 5.5: el valor dinamico que devuelve una consulta de base de
        // datos (Value en tiempo de generacion, ver tipo_cpp()) -- nunca
        // demostrable con un Type nativo fijo (el driver puede fallar en
        // tiempo de ejecucion y devolver una forma distinta, ver el
        // comentario grande sobre esto en COMPILACION-NATIVA.md), asi que
        // se representa como lo que de verdad es: dinamico.
        case Type::Kind::Json:
            return true;
        case Type::Kind::List:
        case Type::Kind::Dict:
            return tipo_elemento_contenedor_soportado(t.element());
        case Type::Kind::Class:
            return clases && clases->count(t.class_name()) > 0;
        default:
            return false;
    }
}

bool es_numerico(Type::Kind k) { return k == Type::Kind::Int || k == Type::Kind::Float; }

// Subconjunto de tipo_soportado() que puede cruzar la ABI fija de
// native_abi.hpp (NativeValue solo tiene un int64_t/double/bool en su
// union): una funcion cuyos parametros y retorno caen todos aqui puede
// recibir un wrapper `extern "C"` y ser invocada desde la VM; una que use
// `string`/`List`/`Dict` en su frontera todavia no -- pero SI se genera su
// cuerpo C++ (ver generar_funcion_nativa), asi que otra funcion nativa que
// la llame directamente (sin pasar por la ABI) se beneficia igual. Extender
// la ABI para que tambien lleve esos tipos queda para cuando una funcion
// con uno de ellos en la frontera sea, ella misma, el objetivo de una
// llamada desde bytecode.
bool tipo_abi_soportado(const Type& t) {
    return tipo_soportado(t) && t.kind() != Type::Kind::String && t.kind() != Type::Kind::List &&
           t.kind() != Type::Kind::Dict && t.kind() != Type::Kind::Json;
}

std::string tipo_cpp(const Type& t) {
    switch (t.kind()) {
        case Type::Kind::Int:    return "int64_t";
        case Type::Kind::Float:  return "double";
        case Type::Kind::Bool:   return "bool";
        case Type::Kind::Void:   return "void";
        case Type::Kind::String: return "std::string";
        // Json es dinamico -- ya ES el Value que usa el VM, ver el
        // comentario de tipo_soportado(). Un List<Json>/Dict<string,Json>
        // (una fila/tabla de base de datos) se representa IGUAL, no como
        // LList<Value>/LDict<Value>: esas dos plantillas existen para
        // contenedores HOMOGENEOS de tipo fijo (§8), y aqui el propio
        // Value ya sabe ser una lista o un diccionario por su cuenta --
        // envolverlo en otra caja no anadiria nada, solo una indireccion
        // de mas.
        case Type::Kind::Json:   return "Value";
        case Type::Kind::List:
            return t.element().kind() == Type::Kind::Json ? "Value"
                                                            : "LList<" + tipo_cpp(t.element()) + ">";
        case Type::Kind::Dict:
            return t.element().kind() == Type::Kind::Json ? "Value"
                                                            : "LDict<" + tipo_cpp(t.element()) + ">";
        case Type::Kind::Class:  return "L" + t.class_name();
        default: return ""; // inalcanzable si tipo_soportado() dio el visto bueno
    }
}

// Escapa una cadena Lumen para que quepa, literal, en un fichero .cpp.
std::string literal_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += "\"";
    return out;
}

// Prefijo fijo para todo identificador generado (variables, parametros,
// funciones): evita por construccion cualquier choque con una palabra
// reservada de C++ que no lo sea de Lumen Script (`new`, `class`,
// `template`...), sin necesitar una lista de palabras reservadas que
// mantener sincronizada con el estandar.
std::string nombre_cpp(const std::string& lumen) { return "l_" + lumen; }

// Los unicos Type::Kind que se pueden empaquetar en un Value escalar
// directamente (Value::integer/real/boolean/str) -- ver Comprobador::
// es_valor_json() y Generador::valor_json() mas abajo, para el valor de
// retorno de una ruta (Fase 4).
bool es_escalar_json(Type::Kind k) {
    return k == Type::Kind::Int || k == Type::Kind::Float || k == Type::Kind::Bool ||
           k == Type::Kind::String;
}

// ¿Es este Type, en la practica, un Value dinamico (Fase 5.5)? Bare Json
// (el resultado directo de `await <modulo>.query/exec/last_id(...)`) O un
// List<Json>/Dict<string,Json> (element() == Json) -- las dos formas
// comparten el MISMO tipo_cpp() ("Value", ver mas arriba): un
// List<Json> no se envuelve en LList<Value>, porque Value ya sabe ser una
// lista por su cuenta. Usado en vez de comparar `kind() == Json` a secas en
// cualquier sitio que necesite tratar las dos formas igual (Index/Binary/
// len()/int()/str()/valor_json()).
bool es_json_dinamico(const Type& t) {
    if (t.kind() == Type::Kind::Json) return true;
    if ((t.kind() == Type::Kind::List || t.kind() == Type::Kind::Dict) &&
        t.element().kind() == Type::Kind::Json)
        return true;
    return false;
}

// Ver native_abi.hpp: campo de la union de NativeValue que corresponde a
// cada Type::Kind soportado, y la etiqueta que hay que ponerle.
std::string campo_abi(Type::Kind k) {
    switch (k) {
        case Type::Kind::Int:   return "i";
        case Type::Kind::Float: return "d";
        case Type::Kind::Bool:  return "b";
        default: return ""; // inalcanzable: tipo_abi_soportado() ya lo descarto
    }
}

std::string etiqueta_abi(Type::Kind k) {
    switch (k) {
        case Type::Kind::Int:   return "NativeValue::Tag::Int";
        case Type::Kind::Float: return "NativeValue::Tag::Float";
        case Type::Kind::Bool:  return "NativeValue::Tag::Bool";
        default: return "";
    }
}

// Los 6 metodos de string que reconoce metodos_de()/call_method()
// (natives.cpp) -- misma lista, vista desde este lado. Cada uno tiene su
// funcion equivalente en string_runtime_prelude(), con la misma semantica
// exacta, y un tipo de retorno FIJO (nunca ambiguo): bool para los tres
// primeros, string para los otros tres.
bool metodo_string_soportado(const std::string& nombre) {
    return nombre == "starts_with" || nombre == "ends_with" || nombre == "contains" ||
           nombre == "upper" || nombre == "lower" || nombre == "trim";
}

bool metodo_string_devuelve_bool(const std::string& nombre) {
    return nombre == "starts_with" || nombre == "ends_with" || nombre == "contains";
}

// ── Comprobacion de tipos y compilabilidad ──────────────────────────────────
//
// Lumen Script no comprueba en ningun sitio -- ni el checker (check_expr/
// check_stmt), ni el VM en tiempo de ejecucion -- que una variable, un
// argumento o un valor de retorno mantengan el tipo con el que se
// declararon: es un lenguaje dinamicamente tipado por debajo de la
// anotacion. `int x = 1; x = "otro tipo"` compila y corre sin aviso; una
// funcion declarada `fn int f(): return "hola"` tambien. Y `a / b` entre
// dos `int` da Int si la division es exacta y Float si no -- una decision
// que solo se puede tomar en tiempo de ejecucion.
//
// La primera version de esta fase confiaba en el tipo DECLARADO (el de
// IrExpr::type / IrStmt::decl_type) para elegir la representacion C++, sin
// verificar que fuera a coincidir con el tipo REAL en tiempo de ejecucion.
// El resultado: programas legales, ya validados por el corpus, que --native
// compilaba sin error y ejecutaba dando una respuesta HTTP DISTINTA a la
// del bytecode -- una violacion directa del invariante de la seccion 3
// ("mismo fuente, mismo comportamiento"), descubierta manualmente al probar
// una reasignacion de tipo y un `and`/`or` con operandos no booleanos.
//
// tipo_provable() reemplaza esa confianza ciega: para cada forma de
// expresion, aplica LAS MISMAS reglas dinamicas que usa el VM (vm.cpp) para
// decidir si el tipo del resultado esta garantizado, y con cual. Cuando no
// puede demostrarlo -- una division entre dos int, un `and`/`or` con
// operandos no booleanos, una variable cuyo tipo no se pudo demostrar mas
// arriba -- devuelve nullopt, y la expresion (o la funcion entera) se queda
// sin compilar a nativo. No es una traduccion de "es compilable" a
// "type_of() en emitter.cpp": type_of() es deliberadamente debil (Unknown
// para casi todo) porque solo necesita servir a un puñado de comprobaciones
// puntuales del checker; esta funcion necesita ser SOLIDA, asi que es un
// analisis propio, mas estricto y mas completo, solo para esta fase.
//
// Devuelve un Type COMPLETO, no solo un Kind: List<int> y List<string>
// tienen el mismo Kind pero son tipos distintos, y Type::operator== ya sabe
// comparar eso (incluido el elemento, recursivamente).
class Comprobador {
public:
    Comprobador(const std::vector<std::string>& nombre_por_indice, const TablaFirmas& firmas,
               const TablaClases& clases, const TablaRoles& roles)
        : nombre_por_indice_(nombre_por_indice), firmas_(firmas), clases_(clases), roles_(roles) {}

    // Expuesto para que Generador pueda traducir un ConstructorCall/
    // ClassMethodCall a la clase (y, para un metodo, el nombre) que
    // corresponde a su call_index -- ver esos casos en Generador::expr.
    const TablaRoles& roles() const { return roles_; }

    // Fase 5: ¿demostro tipo_provable() algun `await` en lo que llevamos
    // comprobado? Puesto a verdad, nunca a falso, dentro del caso
    // IrExprKind::Await -- por eso una unica pasada de block_compilable()
    // basta para saberlo con certeza al final: si el cuerpo entero fue
    // demostrable Y contenia un await en algun punto, esto ya lo vio.
    // Quien llama (generar_funcion_nativa/generar_metodo_nativo/
    // generate_native_route) decide que hacer con el -- una funcion/metodo lo
    // rechaza (fuera de alcance: nadie mas espera a que termine), una ruta
    // lo usa para generar una corrutina de verdad en vez de una funcion
    // plana.
    bool usa_await() const { return usa_await_; }

    // Fase 5.6: ¿demostro tipo_provable() algun `await <modulo>.begin()` en
    // lo que llevamos comprobado? Igual que usa_await_: puesto a verdad,
    // nunca a falso. generate_native_route() lo usa para decidir si la ruta
    // necesita cerrar, al final, cualquier transaccion que el handler haya
    // dejado abierta (rollback_pendientes_db) -- una ruta que nunca llama a
    // begin() no paga ese co_await de mas.
    bool usa_transaccion() const { return usa_transaccion_; }

    // Ranura -> tipo declarado, en el orden en que VarDecl/parametros/`for`
    // los van presentando -- igual que Generador::ranura_a_nombre_, pero de
    // tipo en vez de nombre. Una vez registrada, una ranura mantiene ESE
    // tipo durante toda la funcion: es la CONSECUENCIA (no la causa) de que
    // stmt_compilable() exija que cualquier Assign(Local) sobre esa ranura
    // demuestre el mismo tipo -- por induccion, un Ident que resuelve aqui
    // tiene garantizado que su valor real coincide siempre.
    void registrar(int slot, Type t) { ranura_tipos_.insert_or_assign(slot, std::move(t)); }

    // ¿Se puede construir esta expresion como un Value (el tipo dinamico
    // que ya usa el VM, con to_json_text()) para el valor de retorno de una
    // ruta (Fase 4)? A diferencia de tipo_provable(), NO exige que un
    // DictLit/ListLit tenga un tipo homogeneo -- un cuerpo JSON real casi
    // nunca lo es (bench/lumen/app.lum: un dict con int/string/double/bool
    // mezclados). Alcance: escalares, DictLit/ListLit anidados
    // (recursivamente) de eso mismo, y una List<T>/Dict<V> YA construida
    // (un Ident, por ejemplo) -- convertidas iterando LList<T>/LDict<V> con
    // lumen_valor_de() (route_runtime_prelude), nunca mas de un nivel
    // porque tipo_elemento_contenedor_soportado ya prohibe List<List<..>>/
    // Dict<string,List<..>>. Iterar TODOS los pares de un LDict es una
    // operacion bien definida (a diferencia de "leer una clave que puede
    // faltar", que sigue fuera: vease el comentario de tipo_soportado), asi
    // que no reabre esa ambiguedad.
    bool es_valor_json(const IrExpr& e) const {
        if (e.kind == IrExprKind::DictLit) {
            if (e.entries.empty()) return false;
            for (const auto& entry : e.entries) {
                if (!entry.key || !entry.value) return false;
                auto tk = tipo_provable(*entry.key);
                if (!tk || tk->kind() != Type::Kind::String) return false;
                if (!es_valor_json(*entry.value)) return false;
            }
            return true;
        }
        if (e.kind == IrExprKind::ListLit) {
            if (e.items.empty()) return false;
            for (const auto& item : e.items)
                if (!item || !es_valor_json(*item)) return false;
            return true;
        }
        // Fase 5.8: `<valor>.status(codigo)`, el "modificador de respuesta"
        // que natives.cpp (call_method(), grupo "Modificadores de
        // respuesta") deja encadenar sobre CUALQUIER valor -- fija
        // res.status(codigo) como efecto lateral y devuelve el receptor
        // SIN TOCARLO (`return recv;`), para no reintroducir un objeto
        // `response` mutable en el lenguaje. El unico que aparece en el
        // banco de pruebas (`return {...}.status(409)`, POST /orders) --
        // header()/cookie() son la misma idea pero no los usa nada que
        // esta fase necesite compilar todavia, asi que quedan fuera a
        // proposito (nunca generacion parcial: si hiciera falta, se anade
        // igual que este).
        if (e.kind == IrExprKind::Call && e.call_shape == IrCallShape::BuiltinMethodCall &&
            e.call_name == "status" && e.object) {
            if (e.args.size() != 1 || !e.args[0].value) return false;
            auto tc = tipo_provable(*e.args[0].value);
            if (!tc || tc->kind() != Type::Kind::Int) return false;
            return es_valor_json(*e.object);
        }
        auto t = tipo_provable(e);
        return t && (es_escalar_json(t->kind()) || t->kind() == Type::Kind::List ||
                    t->kind() == Type::Kind::Dict || t->kind() == Type::Kind::Json);
    }

    // `require cond else status(N)` (el patron de guarda mas comun, ver el
    // banco de pruebas) y sus hermanos -- las seis funciones globales de
    // natives.cpp que escriben la respuesta ELLAS MISMAS y devuelven `null`
    // (`ctx.response_written = true`, nunca un valor que comparar contra el
    // tipo de retorno): reconocidas aqui a mano, porque tipo_provable()
    // todavia no sabe nada de BuiltinGlobalCall en general. Cada argumento
    // se exige demostrable con la MISMA regla que usaria su lugar en la
    // ABI/en un valor de retorno -- `text`/`html` solo escalares (son
    // texto/numero, no una estructura), `json` cualquier cosa que
    // es_valor_json() ya sepa serializar.
    bool es_llamada_respuesta(const IrExpr& e) const {
        if (e.kind != IrExprKind::Call || e.call_shape != IrCallShape::BuiltinGlobalCall)
            return false;
        const auto& a = e.args;
        if (e.call_name == "status") {
            if (a.size() != 1 || !a[0].value) return false;
            auto t = tipo_provable(*a[0].value);
            return t && t->kind() == Type::Kind::Int;
        }
        if (e.call_name == "text" || e.call_name == "html") {
            if (a.size() != 1 || !a[0].value) return false;
            auto t = tipo_provable(*a[0].value);
            return t && es_escalar_json(t->kind());
        }
        if (e.call_name == "json") return a.size() == 1 && a[0].value && es_valor_json(*a[0].value);
        if (e.call_name == "redirect") {
            if (a.empty() || a.size() > 2 || !a[0].value) return false;
            auto destino = tipo_provable(*a[0].value);
            if (!destino || destino->kind() != Type::Kind::String) return false;
            if (a.size() == 2) {
                if (!a[1].value) return false;
                auto codigo = tipo_provable(*a[1].value);
                if (!codigo || codigo->kind() != Type::Kind::Int) return false;
            }
            return true;
        }
        if (e.call_name == "send_file") {
            if (a.size() != 1 || !a[0].value) return false;
            auto t = tipo_provable(*a[0].value);
            return t && t->kind() == Type::Kind::String;
        }
        return false;
    }

    // Nullopt si no se puede demostrar; si no, el Type exacto que el VM
    // SIEMPRE produciria para esta expresion, con los mismos valores.
    std::optional<Type> tipo_provable(const IrExpr& e) const {
        switch (e.kind) {
            case IrExprKind::IntLit:    return Type::primitive(Type::Kind::Int);
            case IrExprKind::FloatLit:  return Type::primitive(Type::Kind::Float);
            case IrExprKind::BoolLit:   return Type::primitive(Type::Kind::Bool);
            case IrExprKind::StringLit: return Type::primitive(Type::Kind::String);

            // Sin representacion en esta fase, o sin sentido fuera de una
            // ruta.
            case IrExprKind::NullLit:
                return std::nullopt;

            // Fase 5/5.5/5.6: los awaits que esta fase sabe representar --
            // `await sleep(ms)` (traducido a `co_await lumen::sleep(...)`
            // de verdad) y `await <modulo>.query/exec/last_id/begin/commit/
            // rollback(...)` (traducido a `co_await lumen_script::
            // await_db(...)`, el mismo camino que ya usa bytecode -- ver el
            // comentario de esa funcion en db.hpp). `begin()` marca ademas
            // usa_transaccion_ (ver su comentario) para que
            // generate_native_route() cierre, al final de la ruta, cualquier
            // transaccion que el handler haya dejado abierta; cualquier
            // otro await (ws.recv()...) sigue sin representacion. No hay
            // caso aparte para
            // IrCallShape aqui: e.lhs siempre es IrExprKind::Call
            // (Emitter::check_expr lo garantiza para Await), asi que basta
            // con mirar su call_name/call_shape/call_index directamente.
            case IrExprKind::Await: {
                if (!e.lhs || e.lhs->kind != IrExprKind::Call) return std::nullopt;
                const IrExpr& call = *e.lhs;

                if (call.call_shape == IrCallShape::BuiltinGlobalCall &&
                    call.call_name == "sleep") {
                    if (call.args.size() != 1 || !call.args[0].value) return std::nullopt;
                    auto t = tipo_provable(*call.args[0].value);
                    if (!t || t->kind() != Type::Kind::Int) return std::nullopt;
                    usa_await_ = true;
                    return Type::void_();
                }

                if (call.call_shape == IrCallShape::DbModuleCall) {
                    // query/exec: el primer argumento es la SQL (string), el
                    // resto son parametros -- cualquier escalar o un Json ya
                    // construido (una fila de otra consulta, reusada como
                    // parametro; el driver ya acepta un Value cualquiera).
                    if (call.call_index == db_query_id() || call.call_index == db_exec_id()) {
                        if (call.args.empty() || !call.args[0].value) return std::nullopt;
                        auto tsql = tipo_provable(*call.args[0].value);
                        if (!tsql || tsql->kind() != Type::Kind::String) return std::nullopt;
                        for (size_t i = 1; i < call.args.size(); ++i) {
                            if (!call.args[i].value) return std::nullopt;
                            auto ta = tipo_provable(*call.args[i].value);
                            if (!ta || !(es_escalar_json(ta->kind()) || ta->kind() == Type::Kind::Json))
                                return std::nullopt;
                        }
                        usa_await_ = true;
                        return Type::json();
                    }
                    // last_id()/begin()/commit()/rollback(): sin argumentos.
                    // begin()/commit()/rollback() devuelven Value::boolean(true)
                    // en exito o un Value::Dict {"error":...} en fallo (ver
                    // await_db() en db.cpp) -- el mismo patron dinamico que
                    // query/exec/last_id, asi que Type::json() tambien les
                    // sirve. usa_transaccion_ se marca aparte: una ruta que
                    // llama a begin() necesita el cierre de la transaccion al
                    // final (rollback_pendientes_db), aunque nunca llegue a
                    // llamar a commit()/rollback() (return anticipado, error).
                    if (call.call_index == db_last_id_id() ||
                        call.call_index == db_begin_id() ||
                        call.call_index == db_commit_id() ||
                        call.call_index == db_rollback_id()) {
                        if (!call.args.empty()) return std::nullopt;
                        usa_await_ = true;
                        if (call.call_index == db_begin_id()) usa_transaccion_ = true;
                        return Type::json();
                    }
                }
                return std::nullopt;
            }

            // El unico nodo del IR que YA lleva el nombre de la clase
            // directamente en su tipo (e.type = Type::class_ref(cls),
            // puesto por type_of() -- ver check_method/check_ctor: "this"
            // se declara con exactamente ese tipo). No hace falta pasar
            // por ranura_tipos_: "this" no es reasignable (no existe
            // "this = x" en la gramatica), asi que su tipo es solido sin
            // necesitar la induccion que protege a un Ident normal.
            case IrExprKind::This:
                if (e.type.kind() != Type::Kind::Class || !clases_.count(e.type.class_name()))
                    return std::nullopt;
                return e.type;

            // o.campo (incluido this.campo): demostrable solo si `o` es
            // demostrablemente una instancia de una clase representable
            // (TablaClases) que de verdad tiene ese campo -- resuelto por
            // NOMBRE aqui, en tiempo de generacion, no en tiempo de
            // ejecucion (una clase tipica tiene unos pocos campos; no hay
            // ninguna busqueda que ahorrar en runtime, a diferencia de
            // GetMember en el VM).
            case IrExprKind::Member: {
                if (!e.object) return std::nullopt;
                auto tobj = tipo_provable(*e.object);
                if (!tobj || tobj->kind() != Type::Kind::Class) return std::nullopt;
                auto cit = clases_.find(tobj->class_name());
                if (cit == clases_.end()) return std::nullopt;
                for (const auto& c : cit->second.campos)
                    if (c.nombre == e.text) return c.tipo;
                return std::nullopt;
            }

            case IrExprKind::Ident: {
                auto it = ranura_tipos_.find(e.slot);
                return it == ranura_tipos_.end() ? std::nullopt : std::optional<Type>(it->second);
            }

            // [a, b, c]: demostrable solo si TODOS los elementos demuestran
            // el MISMO tipo (una lista vacia no tiene de donde inferir el
            // elemento -- se queda fuera).
            case IrExprKind::ListLit: {
                if (e.items.empty() || !e.items[0]) return std::nullopt;
                auto t0 = tipo_provable(*e.items[0]);
                if (!t0 || !tipo_elemento_contenedor_soportado(*t0)) return std::nullopt;
                for (size_t i = 1; i < e.items.size(); ++i) {
                    if (!e.items[i]) return std::nullopt;
                    auto ti = tipo_provable(*e.items[i]);
                    if (!ti || *ti != *t0) return std::nullopt;
                }
                return Type::list_of(*t0);
            }

            // {k: v, ...}: cada clave demostrablemente string, cada valor
            // demostrablemente el MISMO tipo (igual criterio que ListLit;
            // un diccionario vacio tambien se queda fuera, no hay de donde
            // inferir el tipo del valor).
            case IrExprKind::DictLit: {
                if (e.entries.empty()) return std::nullopt;
                std::optional<Type> tv;
                for (const auto& entry : e.entries) {
                    if (!entry.key || !entry.value) return std::nullopt;
                    auto tk = tipo_provable(*entry.key);
                    if (!tk || tk->kind() != Type::Kind::String) return std::nullopt;
                    auto tval = tipo_provable(*entry.value);
                    if (!tval || !tipo_elemento_contenedor_soportado(*tval)) return std::nullopt;
                    if (!tv) tv = tval;
                    else if (*tv != *tval) return std::nullopt;
                }
                return Type::dict_of(*tv);
            }

            // xs[i]: object es el receptor, lhs el indice (ver el
            // comentario de IrExpr en ir.hpp). Solo List: leer un Dict por
            // indice puede dar `null` si la clave no existe (vm.cpp::
            // GetIndex) -- un tipo distinto del valor declarado, la misma
            // ambiguedad que "a / b" entre dos int (ver la correccion
            // critica). No demostrable, se queda sin compilar a proposito;
            // Dict SI admite escribir por indice (ver Assign mas abajo, sin
            // esa ambiguedad: el VM siempre acepta la escritura).
            //
            // Json (Fase 5.5): el objeto YA es dinamico -- una fila de base
            // de datos puede ser una List (`filas[0]`) o, dentro de ella,
            // un Dict-por-nombre-de-columna (`f["nombre"]`). Aqui NO hay
            // ambiguedad que evitar (a diferencia de Dict<V> arriba): el
            // Value entero, con su ausencia-da-null incluida, viaja intacto
            // -- es EXACTAMENTE lo que vm.cpp::GetIndex ya hace, solo que
            // resuelto en tiempo de ejecucion por lumen_json_index_int/_str
            // (route_runtime_prelude) en vez de por un opcode. El indice
            // decide la forma (Int -> acceso de List, String -> acceso de
            // Dict); Comprobador no sabe -- ni le hace falta saber -- si el
            // Value sera realmente una List o un Dict en tiempo de
            // ejecucion, igual que el VM tampoco lo sabe hasta ese momento.
            case IrExprKind::Index: {
                if (!e.object || !e.lhs) return std::nullopt;
                auto tobj = tipo_provable(*e.object);
                auto tidx = tipo_provable(*e.lhs);
                if (!tobj || !tidx) return std::nullopt;
                if (es_json_dinamico(*tobj)) {
                    if (tidx->kind() == Type::Kind::Int || tidx->kind() == Type::Kind::String)
                        return Type::json();
                    return std::nullopt;
                }
                if (tobj->kind() != Type::Kind::List) return std::nullopt;
                if (tidx->kind() != Type::Kind::Int) return std::nullopt;
                return tobj->element();
            }

            case IrExprKind::Unary: {
                if (!e.lhs) return std::nullopt;
                auto t = tipo_provable(*e.lhs);
                if (!t) return std::nullopt;
                if (e.text == "not") return Type::primitive(Type::Kind::Bool); // Op::Not: siempre bool
                return es_numerico(t->kind()) ? t : std::nullopt; // '-': solo sobre numeros
            }

            case IrExprKind::Binary: {
                if (!e.lhs || !e.rhs) return std::nullopt;

                // Fase 5.7: `x == null`/`x != null`. Solo tiene sentido
                // contra un valor dinamico (Json, o un campo `?` de clase
                // -- ver CampoNativo, se representa igual): un escalar
                // nativo (int/float/bool/string) nunca es null en tiempo
                // de ejecucion, asi que la comparacion, aunque compilase,
                // seria siempre el mismo booleano constante -- no vale la
                // pena representarla, mejor que se quede sin demostrar.
                // NullLit en si mismo NUNCA es demostrable (tipo_provable,
                // caso NullLit) fuera de este contexto, asi que se mira
                // ANTES de pedir el tipo de los dos lados: pedirselo a un
                // NullLit directamente siempre daria nullopt y tumbaria
                // esta rama entera.
                bool lhs_null = e.lhs->kind == IrExprKind::NullLit;
                bool rhs_null = e.rhs->kind == IrExprKind::NullLit;
                if (lhs_null || rhs_null) {
                    if (e.text != "==" && e.text != "!=") return std::nullopt;
                    const IrExpr& otro = lhs_null ? *e.rhs : *e.lhs;
                    auto to = tipo_provable(otro);
                    if (!to || !es_json_dinamico(*to)) return std::nullopt;
                    return Type::primitive(Type::Kind::Bool);
                }

                auto tl = tipo_provable(*e.lhs);
                auto tr = tipo_provable(*e.rhs);
                if (!tl || !tr) return std::nullopt;

                // Json (Fase 5.5) en cualquiera de los dos lados: se
                // resuelve en tiempo de ejecucion con la MISMA logica que
                // vm.cpp -- numeric_pair()/compare()/Op::Add/Sub/Mul/Div/
                // Mod/Eq/Ne, ver los lumen_json_* de route_runtime_prelude.
                // and/or quedan fuera: la semantica "el operando que gana"
                // (ver el comentario de mas abajo) tampoco se generaba para
                // dos operandos YA tipados que no fueran bool, y un Json es
                // menos demostrable que eso todavia.
                if (es_json_dinamico(*tl) || es_json_dinamico(*tr)) {
                    // El lado que NO es dinamico tiene que ser algo que
                    // Generador::valor_json() sepa convertir a Value -- si
                    // fuera, por ejemplo, una instancia de clase, no habria
                    // ninguna llamada de C++ que generar (valor_json() no
                    // la cubre) y esto quedaria en un cuerpo roto en vez de
                    // caer a bytecode a tiempo.
                    auto compatible = [](const Type& t) {
                        return es_escalar_json(t.kind()) || t.kind() == Type::Kind::List ||
                               t.kind() == Type::Kind::Dict || t.kind() == Type::Kind::Json;
                    };
                    if (!compatible(*tl) || !compatible(*tr)) return std::nullopt;
                    if (e.text == "and" || e.text == "or") return std::nullopt;
                    if (e.text == "==" || e.text == "!=" || e.text == "<" || e.text == "<=" ||
                        e.text == ">" || e.text == ">=")
                        return Type::primitive(Type::Kind::Bool);
                    if (e.text == "+" || e.text == "-" || e.text == "*" || e.text == "/" ||
                        e.text == "%")
                        return Type::json();
                    return std::nullopt;
                }

                // and/or (vm.cpp: JumpIfFalsePeek/JumpIfTruePeek) devuelven
                // el VALOR del operando que gana, al estilo Python -- NO un
                // booleano forzado. Traducirlo a &&/|| (lo que hace
                // Generador::expr) solo coincide, observablemente, cuando
                // los dos lados YA son bool: alli "el operando que gana" y
                // "el resultado de &&/||" son el mismo valor. Para
                // cualquier otro tipo (`5 and 10` -> 10, no `true`) no
                // coinciden, y esta fase no genera la logica de verdad
                // (evaluar una vez, devolver el operando) -- se queda sin
                // compilar.
                if (e.text == "and" || e.text == "or")
                    return (tl->kind() == Type::Kind::Bool && tr->kind() == Type::Kind::Bool)
                               ? std::optional<Type>(Type::primitive(Type::Kind::Bool))
                               : std::nullopt;

                if (e.text == "==" || e.text == "!=" || e.text == "<" || e.text == "<=" ||
                    e.text == ">" || e.text == ">=") {
                    bool numericos = es_numerico(tl->kind()) && es_numerico(tr->kind());
                    bool strings   = tl->kind() == Type::Kind::String &&
                                    tr->kind() == Type::Kind::String;
                    return (numericos || strings) ? std::optional<Type>(Type::primitive(Type::Kind::Bool))
                                                  : std::nullopt;
                }

                if (e.text == "+" && tl->kind() == Type::Kind::String &&
                    tr->kind() == Type::Kind::String)
                    return Type::primitive(Type::Kind::String);

                if (!es_numerico(tl->kind()) || !es_numerico(tr->kind())) return std::nullopt;

                if (e.text == "%") // vm.cpp: '%' exige enteros a los dos lados
                    return (tl->kind() == Type::Kind::Int && tr->kind() == Type::Kind::Int)
                               ? std::optional<Type>(Type::primitive(Type::Kind::Int)) : std::nullopt;

                if (e.text == "/")
                    // vm.cpp: entre dos int, Int si la division es EXACTA y
                    // Float si no -- una rama que solo el valor en tiempo de
                    // ejecucion decide. No demostrable estaticamente.
                    return (tl->kind() == Type::Kind::Int && tr->kind() == Type::Kind::Int)
                               ? std::nullopt : std::optional<Type>(Type::primitive(Type::Kind::Float));

                // +, -, *: Int si los dos son Int, Float en cualquier otra
                // combinacion numerica (vm.cpp: `ints ? integer : real`).
                return (tl->kind() == Type::Kind::Int && tr->kind() == Type::Kind::Int)
                           ? std::optional<Type>(Type::primitive(Type::Kind::Int))
                           : std::optional<Type>(Type::primitive(Type::Kind::Float));
            }

            case IrExprKind::Ternary: {
                // La condicion solo necesita ser demostrable en algun tipo
                // (Int/Float/Bool convierten a bool en C++ identico a
                // truthy(); string no convierte -- g++ lo rechaza solo).
                if (!e.object || !tipo_provable(*e.object)) return std::nullopt;
                if (!e.lhs || !e.rhs) return std::nullopt;
                auto ts = tipo_provable(*e.lhs);
                auto tn = tipo_provable(*e.rhs);
                if (ts && tn && *ts == *tn) return ts;
                return std::nullopt;
            }

            case IrExprKind::PreStep:
            case IrExprKind::PostStep: {
                if (!e.lhs || e.lhs->kind != IrExprKind::Ident) return std::nullopt;
                auto t = tipo_provable(*e.lhs);
                return (t && es_numerico(t->kind())) ? t : std::nullopt;
            }

            case IrExprKind::Call: {
                if (e.call_shape == IrCallShape::BuiltinMethodCall) {
                    if (!e.object) return std::nullopt;
                    auto tobj = tipo_provable(*e.object);
                    if (!tobj) return std::nullopt;

                    if (tobj->kind() == Type::Kind::String) {
                        if (!metodo_string_soportado(e.call_name)) return std::nullopt;
                        for (const auto& a : e.args)
                            if (!a.value || !tipo_provable(*a.value)) return std::nullopt;
                        return metodo_string_devuelve_bool(e.call_name)
                                   ? Type::primitive(Type::Kind::Bool)
                                   : Type::primitive(Type::Kind::String);
                    }
                    // El unico metodo de List que reconoce metodos_de()
                    // (natives.cpp: kList) es "add" -- muta la lista en
                    // sitio y devuelve la MISMA lista (recv), igual que
                    // call_method(). El argumento tiene que ser exactamente
                    // el tipo del elemento -- SALVO sobre List<Json>
                    // (Fase 5.10): el patron mas comun para construirla a
                    // mano es un DictLit HETEROGENEO (`{"id": i, "name":
                    // ..., "active": bool}`, cada valor de un tipo
                    // distinto), y tipo_provable(DictLit) exige valores
                    // homogeneos -- nunca demuestra nada de un literal asi
                    // (esa es la via de "Dict<string,V> ya tipado", una
                    // cosa distinta). Le basta con ser cualquier cosa
                    // construible como Value (es_valor_json(), la MISMA
                    // regla que ya usa el valor de retorno de una ruta o
                    // un parametro de sqlite.exec()) -- List<Json>.add(x)
                    // es, en tiempo de ejecucion, exactamente
                    // items.push_back(x) sobre un LList<Value>.
                    if (tobj->kind() == Type::Kind::List && e.call_name == "add") {
                        if (e.args.size() != 1 || !e.args[0].value) return std::nullopt;
                        if (tobj->element().kind() == Type::Kind::Json)
                            return es_valor_json(*e.args[0].value) ? tobj : std::nullopt;
                        auto targ = tipo_provable(*e.args[0].value);
                        if (!targ || *targ != tobj->element()) return std::nullopt;
                        return tobj;
                    }
                    // Los dos metodos de Dict que reconoce metodos_de()
                    // (natives.cpp: kDict) sin depender de un contexto de
                    // ruta (el tercero, "save", solo existe sobre un File
                    // subido): "has" comprueba una clave, "keys" devuelve
                    // List<string> con todas -- ninguno de los dos tiene la
                    // ambiguedad de leer un valor por indice.
                    if (tobj->kind() == Type::Kind::Dict && e.call_name == "has") {
                        if (e.args.size() != 1 || !e.args[0].value) return std::nullopt;
                        auto tk = tipo_provable(*e.args[0].value);
                        return (tk && tk->kind() == Type::Kind::String)
                                   ? std::optional<Type>(Type::primitive(Type::Kind::Bool))
                                   : std::nullopt;
                    }
                    if (tobj->kind() == Type::Kind::Dict && e.call_name == "keys") {
                        if (!e.args.empty()) return std::nullopt;
                        return Type::list_of(Type::primitive(Type::Kind::String));
                    }
                    return std::nullopt;
                }
                if (e.call_shape == IrCallShape::UserFunctionCall) {
                    if (e.call_index < 0 ||
                        static_cast<size_t>(e.call_index) >= nombre_por_indice_.size())
                        return std::nullopt;
                    auto fit = firmas_.find(nombre_por_indice_[static_cast<size_t>(e.call_index)]);
                    if (fit == firmas_.end() || fit->second.params.size() != e.args.size())
                        return std::nullopt;
                    for (size_t i = 0; i < e.args.size(); ++i) {
                        if (!e.args[i].value) return std::nullopt;
                        auto ta = tipo_provable(*e.args[i].value);
                        if (!ta || *ta != fit->second.params[i]) return std::nullopt;
                    }
                    return fit->second.retorno;
                }
                // ClassName(args...): solo el constructor SIN cuerpo
                // (automapeo) -- ver RolFuncion::tiene_cuerpo. El automapeo
                // exige un argumento POR CADA campo, en el mismo orden de
                // declaracion (generar_clase_runtime genera el unico
                // constructor de la clase C++ con esa misma firma
                // posicional): si el numero no coincide, no es este
                // constructor (uno con menos parametros que campos, que
                // dejaria alguno en null, no se genera nunca -- ver
                // construir_clases).
                if (e.call_shape == IrCallShape::ConstructorCall) {
                    auto rit = roles_.find(e.call_index);
                    if (rit == roles_.end() || !rit->second.metodo.empty() ||
                        rit->second.tiene_cuerpo)
                        return std::nullopt;
                    auto cit = clases_.find(rit->second.clase);
                    if (cit == clases_.end()) return std::nullopt;
                    const auto& campos = cit->second.campos;
                    if (campos.size() != e.args.size()) return std::nullopt;
                    for (size_t i = 0; i < e.args.size(); ++i) {
                        if (!e.args[i].value) return std::nullopt;
                        auto ta = tipo_provable(*e.args[i].value);
                        if (!ta || *ta != campos[i].tipo) return std::nullopt;
                    }
                    return Type::class_ref(rit->second.clase);
                }
                // receptor.metodo(args...): el receptor tiene que ser
                // demostrablemente una instancia de la MISMA clase que
                // check_call ya resolvio para este call_index -- una
                // comprobacion redundante en la practica (los dos vienen
                // del mismo check_call, sobre el mismo `e.object`), pero
                // barata, y consistente con no confiar en nada que no se
                // pueda demostrar aqui mismo (ver el comentario de la
                // clase).
                if (e.call_shape == IrCallShape::ClassMethodCall) {
                    if (!e.object) return std::nullopt;
                    auto trec = tipo_provable(*e.object);
                    if (!trec || trec->kind() != Type::Kind::Class) return std::nullopt;
                    auto rit = roles_.find(e.call_index);
                    if (rit == roles_.end() || rit->second.metodo.empty() ||
                        rit->second.clase != trec->class_name())
                        return std::nullopt;
                    auto cit = clases_.find(rit->second.clase);
                    if (cit == clases_.end()) return std::nullopt;
                    auto mit = cit->second.metodos.find(rit->second.metodo);
                    if (mit == cit->second.metodos.end() ||
                        mit->second.params.size() != e.args.size())
                        return std::nullopt;
                    for (size_t i = 0; i < e.args.size(); ++i) {
                        if (!e.args[i].value) return std::nullopt;
                        auto ta = tipo_provable(*e.args[i].value);
                        if (!ta || *ta != mit->second.params[i]) return std::nullopt;
                    }
                    return mit->second.retorno;
                }
                // Fase 5.9: state.incr/decr/get/set/remove (natives.cpp:
                // SharedState) -- el unico ReservedMemberCall que esta fase
                // sabe demostrar; sse/ws/error/request/log quedan fuera
                // (necesitan contexto -- una conexion, un error en curso --
                // que una expresion "pura" no tiene aqui). incr/decr
                // SIEMPRE dan Value::integer (fn_state_incr/_decr, ver
                // natives.cpp): Int provable sin ambiguedad, a diferencia
                // de get(), que depende de lo que haya guardado. remove()
                // siempre Bool. set()/get() son dinamicos -- Json, igual
                // que un resultado de BD: lo que hay guardado en la clave
                // puede ser cualquier cosa, o nada.
                if (e.call_shape == IrCallShape::ReservedMemberCall) {
                    if (e.call_index == state_incr_id() || e.call_index == state_decr_id()) {
                        if (e.args.empty() || e.args.size() > 2 || !e.args[0].value)
                            return std::nullopt;
                        auto tk = tipo_provable(*e.args[0].value);
                        if (!tk || tk->kind() != Type::Kind::String) return std::nullopt;
                        if (e.args.size() == 2) {
                            if (!e.args[1].value) return std::nullopt;
                            auto tb = tipo_provable(*e.args[1].value);
                            if (!tb || tb->kind() != Type::Kind::Int) return std::nullopt;
                        }
                        return Type::primitive(Type::Kind::Int);
                    }
                    if (e.call_index == state_remove_id()) {
                        if (e.args.size() != 1 || !e.args[0].value) return std::nullopt;
                        auto tk = tipo_provable(*e.args[0].value);
                        if (!tk || tk->kind() != Type::Kind::String) return std::nullopt;
                        return Type::primitive(Type::Kind::Bool);
                    }
                    if (e.call_index == state_get_id()) {
                        if (e.args.empty() || e.args.size() > 2 || !e.args[0].value)
                            return std::nullopt;
                        auto tk = tipo_provable(*e.args[0].value);
                        if (!tk || tk->kind() != Type::Kind::String) return std::nullopt;
                        if (e.args.size() == 2) {
                            if (!e.args[1].value) return std::nullopt;
                            auto td = tipo_provable(*e.args[1].value);
                            if (!td || !(es_escalar_json(td->kind()) ||
                                        td->kind() == Type::Kind::List ||
                                        td->kind() == Type::Kind::Dict ||
                                        td->kind() == Type::Kind::Json))
                                return std::nullopt;
                        }
                        return Type::json();
                    }
                    if (e.call_index == state_set_id()) {
                        if (e.args.size() != 2 || !e.args[0].value || !e.args[1].value)
                            return std::nullopt;
                        auto tk = tipo_provable(*e.args[0].value);
                        if (!tk || tk->kind() != Type::Kind::String) return std::nullopt;
                        // fn_state_set devuelve el MISMO valor que recibio
                        // (identidad) -- el tipo de la llamada es el del
                        // segundo argumento, tal cual.
                        auto tv = tipo_provable(*e.args[1].value);
                        if (!tv || !(es_escalar_json(tv->kind()) ||
                                    tv->kind() == Type::Kind::List ||
                                    tv->kind() == Type::Kind::Dict ||
                                    tv->kind() == Type::Kind::Json))
                            return std::nullopt;
                        return tv;
                    }
                    return std::nullopt;
                }
                // Tres builtins globales puros (natives.cpp: fn_str/fn_len/
                // fn_int), el resto (sleep/render/status/text/...) o tienen
                // efecto (escriben la respuesta, leen la peticion) o son
                // asincronos -- fuera de alcance de tipo_provable, que solo
                // demuestra tipos de expresiones sin efectos. `str(x)` reusa
                // el mismo puente a Value que el valor de retorno de una
                // ruta (ver Generador::valor_json): solo escalares, para no
                // reimplementar Value::to_string() aqui. `len(x)` solo
                // String/List<T> -- Dict queda fuera porque LDict no expone
                // ningun metodo de tamaño (dict_runtime_prelude solo trae
                // has/set/keys). `int(x)` acepta los cuatro escalares --
                // sobre string puede fallar en tiempo de ejecucion
                // (lumen_native_fail, mismo canal que division/modulo por
                // cero: atrapado por el wrapper de una funcion o por el
                // try/catch de una ruta, nunca sin red).
                if (e.call_shape == IrCallShape::BuiltinGlobalCall) {
                    // Fase 5.5: los tres aceptan tambien un Json dinamico --
                    // str()/int() lo resuelven en tiempo de ejecucion
                    // (Value::to_string()/lumen_json_as_int(), mismo canal
                    // de error que ya usan sobre un tipo fijo); len() sobre
                    // un Json puede ser string/List/Dict, igual que fn_len.
                    if (e.call_name == "str" && e.args.size() == 1 && e.args[0].value) {
                        auto t = tipo_provable(*e.args[0].value);
                        if (t && (es_escalar_json(t->kind()) || es_json_dinamico(*t)))
                            return Type::primitive(Type::Kind::String);
                    }
                    if (e.call_name == "len" && e.args.size() == 1 && e.args[0].value) {
                        auto t = tipo_provable(*e.args[0].value);
                        if (t && (t->kind() == Type::Kind::String || t->kind() == Type::Kind::List ||
                                 es_json_dinamico(*t)))
                            return Type::primitive(Type::Kind::Int);
                    }
                    if (e.call_name == "int" && e.args.size() == 1 && e.args[0].value) {
                        auto t = tipo_provable(*e.args[0].value);
                        if (t && (es_escalar_json(t->kind()) || es_json_dinamico(*t)))
                            return Type::primitive(Type::Kind::Int);
                    }
                }
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    bool block_compilable(const IrBlock& b, const Type& retorno_fn) {
        for (const auto& s : b)
            if (!s || !stmt_compilable(*s, retorno_fn)) return false;
        return true;
    }

    bool stmt_compilable(const IrStmt& s, const Type& retorno_fn) {
        switch (s.kind) {
            case IrStmtKind::Return: {
                // Type::Kind::Json es el centinela de "esto es una ruta,
                // no una funcion" (ver generate_native_route): el valor de
                // retorno se serializa a JSON, asi que no tiene que
                // demostrar un Type nativo concreto -- basta con ser
                // construible como Value (es_valor_json()) o ser una de las
                // llamadas que escriben la respuesta ellas mismas
                // (es_llamada_respuesta(), p.ej. "return status(404)").
                if (retorno_fn.kind() == Type::Kind::Json)
                    return !s.value || es_llamada_respuesta(*s.value) || es_valor_json(*s.value);
                if (!s.value) return retorno_fn.kind() == Type::Kind::Void;
                auto t = tipo_provable(*s.value);
                return t && *t == retorno_fn;
            }

            case IrStmtKind::ExprStmt:
                return s.value && tipo_provable(*s.value).has_value();

            case IrStmtKind::VarDecl: {
                if (s.value) {
                    auto t = tipo_provable(*s.value);
                    if (!t) {
                        // Fase 5.10: `List<Json> items = []` (o `Dict<string,V>
                        // d = {}`) -- un literal VACIO no tiene de donde
                        // inferir el tipo de sus elementos (tipo_provable,
                        // casos ListLit/DictLit, lo rechaza SIEMPRE, tenga o
                        // no tipo declarado), pero eso no es lo mismo que "no
                        // se sabe que tipo es": el tipo DECLARADO ya lo dice
                        // sin ninguna ambiguedad, y estar vacio no lo
                        // contradice -- a diferencia de cualquier otro valor
                        // no demostrable, un [] / {} vacio es compatible con
                        // CUALQUIER List<T>/Dict<string,V> soportado. Sin
                        // esto, "acumular en una lista vacia dentro de un
                        // bucle" (el patron mas comun de construir un
                        // List<Json> a mano, ver bench/lumen/app.lum:
                        // /payload/:n) nunca compilaba: la ruta entera caia
                        // en el primerisimo statement.
                        bool vacio_compatible =
                            (s.value->kind == IrExprKind::ListLit && s.value->items.empty() &&
                             s.decl_type.kind() == Type::Kind::List) ||
                            (s.value->kind == IrExprKind::DictLit && s.value->entries.empty() &&
                             s.decl_type.kind() == Type::Kind::Dict);
                        if (!vacio_compatible || !tipo_soportado(s.decl_type, &clases_))
                            return false;
                        registrar(s.slot, s.decl_type);
                        return true;
                    }
                    if (*t == s.decl_type) {
                        registrar(s.slot, s.decl_type);
                        return true;
                    }
                    // Fase 5.5: el valor real es Json (dinamico) aunque el
                    // tipo declarado sea otra cosa -- `int stock =
                    // filas[0]["stock"]` es valido Lumen (el tipo declarado
                    // es decorativo, ver el comentario grande sobre esto en
                    // COMPILACION-NATIVA.md) y el VM no le exige nada en la
                    // asignacion, solo cuando el valor se USA de verdad. Se
                    // registra el tipo REAL (Json), no el que puso el
                    // programador: las operaciones posteriores (aritmetica,
                    // comparacion, indexado...) ya saben resolverlo en
                    // tiempo de ejecucion, exactamente como el VM. Cualquier
                    // reasignacion futura de esta ranura tiene que demostrar
                    // Json tambien (Assign(Local) exige el mismo tipo que
                    // registro() dejo aqui).
                    if (es_json_dinamico(*t)) {
                        registrar(s.slot, Type::json());
                        return true;
                    }
                    return false;
                }
                if (!tipo_soportado(s.decl_type, &clases_)) return false;
                registrar(s.slot, s.decl_type);
                return true;
            }

            case IrStmtKind::Assign: {
                if (!s.value) return false;
                if (s.assign_target == IrAssignTarget::Local) {
                    // El nuevo valor tiene que demostrar EXACTAMENTE el
                    // tipo con el que esa ranura se declaro -- es la regla
                    // que mantiene solido tipo_provable(Ident) durante el
                    // resto de la funcion (ver el comentario de
                    // registrar()).
                    auto original = ranura_tipos_.find(s.assign_slot);
                    if (original == ranura_tipos_.end()) return false;
                    auto t = tipo_provable(*s.value);
                    return t && *t == original->second;
                }
                if (s.assign_target == IrAssignTarget::Index) {
                    // xs[i] = v: solo sobre una variable (una expresion
                    // temporal -- p.ej. el resultado de una llamada -- no
                    // tiene sentido mutarla in place). List exige indice
                    // Int; Dict exige indice string (y, a diferencia de
                    // leer, escribir SIEMPRE es valido en el VM -- sin la
                    // ambiguedad de una clave ausente, ver el caso Index en
                    // tipo_provable()). En los dos casos, el valor tiene que
                    // ser exactamente el tipo del elemento.
                    if (!s.assign_object || s.assign_object->kind != IrExprKind::Ident ||
                        !s.assign_index)
                        return false;
                    auto tobj = tipo_provable(*s.assign_object);
                    auto tidx = tipo_provable(*s.assign_index);
                    auto tval = tipo_provable(*s.value);
                    if (!tobj || !tidx || !tval) return false;
                    if (tobj->kind() == Type::Kind::List)
                        return tidx->kind() == Type::Kind::Int && *tval == tobj->element();
                    if (tobj->kind() == Type::Kind::Dict)
                        return tidx->kind() == Type::Kind::String && *tval == tobj->element();
                    return false;
                }
                if (s.assign_target == IrAssignTarget::Member) {
                    // o.campo = v (incluido this.campo = v): igual que
                    // Index, solo sobre una variable (This o Ident) -- una
                    // instancia demostrablemente de una clase representable
                    // que de verdad tiene ese campo, con el valor exacto de
                    // su tipo declarado.
                    if (!s.assign_object ||
                        (s.assign_object->kind != IrExprKind::Ident &&
                         s.assign_object->kind != IrExprKind::This))
                        return false;
                    auto tobj = tipo_provable(*s.assign_object);
                    if (!tobj || tobj->kind() != Type::Kind::Class) return false;
                    auto cit = clases_.find(tobj->class_name());
                    if (cit == clases_.end()) return false;
                    const Type* campo_tipo = nullptr;
                    for (const auto& c : cit->second.campos)
                        if (c.nombre == s.assign_field) { campo_tipo = &c.tipo; break; }
                    if (!campo_tipo) return false;
                    auto tval = tipo_provable(*s.value);
                    return tval && *tval == *campo_tipo;
                }
                return false; // Session: fuera de esta fase (requiere una ruta)
            }

            case IrStmtKind::If:
                return s.value && tipo_provable(*s.value).has_value() &&
                       block_compilable(s.body, retorno_fn) &&
                       block_compilable(s.orelse, retorno_fn);

            case IrStmtKind::While:
                return s.value && tipo_provable(*s.value).has_value() &&
                       block_compilable(s.body, retorno_fn);

            case IrStmtKind::Break:
            case IrStmtKind::Continue:
                return true;

            // `for x in xs:`: s.target es el iterable, s.name/s.slot la
            // variable del bucle. El tipo de esa variable no viene de una
            // anotacion en el .lum (Lumen no la exige) -- se DERIVA aqui
            // del elemento de `xs`; Generador vuelve a preguntar
            // tipo_provable(*s.target) para declarar la variable C++
            // correspondiente (ver Generador::stmt, mismo caso).
            //
            case IrStmtKind::For: {
                if (!s.target) return false;
                auto titer = tipo_provable(*s.target);
                if (!titer || titer->kind() != Type::Kind::List) return false;
                registrar(s.slot, titer->element());
                return block_compilable(s.body, retorno_fn);
            }

            // `require cond else otherwise`: "si no cond, devuelve
            // otherwise" -- el mismo IrStmtKind que las guardas de grupo de
            // una ruta (Emitter::check_require_like, compartido). La
            // condicion solo necesita ser demostrable en algun tipo (igual
            // que If/While); `otherwise` tiene que demostrar EXACTAMENTE el
            // tipo de retorno de la funcion/ruta, igual que un Return.
            case IrStmtKind::Require: {
                if (!s.value || !tipo_provable(*s.value).has_value()) return false;
                if (!s.target) return false;
                // "else status(N)"/"else text(...)"/... escribe la
                // respuesta el mismo -- no produce ningun valor que
                // comparar, ver es_llamada_respuesta().
                if (es_llamada_respuesta(*s.target)) return true;
                if (retorno_fn.kind() == Type::Kind::Json) return es_valor_json(*s.target);
                auto t = tipo_provable(*s.target);
                return t && *t == retorno_fn;
            }

            // Try no es "primitivos y control de flujo" en el sentido
            // estrecho de esta fase todavia -- necesita decidir como se
            // representa un error nativo, que es una decision de la fase 5
            // (asincronia y errores).
            case IrStmtKind::Try:
                return false;
        }
        return false;
    }

private:
    const std::vector<std::string>& nombre_por_indice_;
    const TablaFirmas&               firmas_;
    const TablaClases&               clases_;
    const TablaRoles&                roles_;
    std::map<int, Type>              ranura_tipos_;
    mutable bool                     usa_await_ = false;
    mutable bool                     usa_transaccion_ = false;
};

// ── Generacion ────────────────────────────────────────────────────────────

// Formato inequivoco: siempre con punto decimal, para que "1.0" no se
// genere como el entero "1" (que en C++ convierte implicitamente pero deja
// de ser, a simple vista, el literal de coma flotante que era en el .lum).
std::string literal_float(double d) {
    std::ostringstream o;
    o.precision(17);
    o << d;
    std::string s = o.str();
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

const std::map<std::string, std::string>& operadores_binarios() {
    static const std::map<std::string, std::string> ops = {
        {"+", "+"}, {"-", "-"}, {"*", "*"},
        {"==", "=="}, {"!=", "!="}, {"<", "<"}, {"<=", "<="}, {">", ">"}, {">=", ">="},
        {"and", "&&"}, {"or", "||"},
    };
    return ops;
}

class Generador {
public:
    // `comprobador` es el mismo (ya usado, ya con exito) que decidio que
    // esta funcion se puede generar -- Generador lo reusa para volver a
    // preguntar el tipo de una expresion cuando el C++ que emite lo
    // necesita explicito (DictLit, la variable de un `for`): no vuelve a
    // decidir nada, solo consulta lo que tipo_provable() ya demostro.
    // `ruta`: si es verdad, un Return/Require con destino serializa su
    // valor como respuesta HTTP en vez de generar un `return <valor>;` de
    // funcion -- ver esos dos casos en stmt() y generate_native_route().
    // `asincrona` (solo tiene sentido si `ruta` tambien lo es, ver
    // Comprobador::usa_await()): la ruta se genera como `lumen::Task<void>`
    // -- toda salida temprana tiene que ser `co_return;`, no `return;` (una
    // corrutina no admite un `return` a secas), y `await sleep(ms)` se
    // traduce a un `co_await` de verdad, no a una llamada plana.
    Generador(const std::vector<std::string>& nombre_por_indice, const Comprobador& comprobador,
             bool ruta = false, bool asincrona = false)
        : nombre_por_indice_(nombre_por_indice), comprobador_(comprobador), ruta_(ruta),
          asincrona_(asincrona) {}

    // Ranura -> nombre C++ ya calculado, para poder generar `nombre = ...`
    // en un Assign(Local): el IrStmt solo trae `assign_slot` (lo unico que
    // necesita el bytecode), no un nombre, asi que el generador lleva su
    // propia cuenta segun va viendo parametros y VarDecl. Los parametros se
    // registran en generar_funcion_nativa (ranura i-esima = parametro
    // i-esimo, por como los declara check_function antes que nada mas).
    void registrar(int slot, const std::string& nombre) { ranura_a_nombre_[slot] = nombre; }

    std::string expr(const IrExpr& e) const {
        switch (e.kind) {
            // static_cast, no solo el sufijo LL: en glibc/x86-64, int64_t es
            // `long` pero un literal `LL` es `long long` -- dos tipos
            // DISTINTOS para el compilador (aunque los dos midan 64 bits),
            // que la deduccion de tipo de LList{...} (ver ListLit) no
            // confunde entre si. El cast deja el valor exactamente en el
            // tipo que tipo_cpp(Int) promete en cualquier plataforma.
            case IrExprKind::IntLit:
                return "static_cast<int64_t>(" + std::to_string(e.int_value) + "LL)";
            case IrExprKind::FloatLit:  return literal_float(e.float_value);
            case IrExprKind::BoolLit:   return e.bool_value ? "true" : "false";
            case IrExprKind::StringLit: return "std::string(" + literal_string(e.text) + ")";
            case IrExprKind::Ident:     return nombre_cpp(e.text);

            // CTAD (una guia de deduccion en list_runtime_prelude) deduce T
            // solo con los elementos, sin que Generador tenga que saber el
            // tipo aqui.
            case IrExprKind::ListLit: {
                std::string s = "LList{";
                for (size_t i = 0; i < e.items.size(); ++i) {
                    if (i) s += ", ";
                    s += expr(*e.items[i]);
                }
                s += "}";
                return s;
            }

            // A diferencia de ListLit, aqui NO basta con CTAD: deducir V a
            // traves de una lista de std::pair anidados esta fuera de lo
            // que el estandar deja deducir (cada elemento es el resultado
            // de list-init de std::pair<string,V>, y eso no participa en la
            // deduccion de argumentos de plantilla de LDict) -- se
            // comprobo directamente contra g++, que lo rechaza. Con V
            // explicito (tipo_provable() ya lo demostro) no hace falta
            // deducir nada.
            case IrExprKind::DictLit: {
                const Type tipo = *comprobador_.tipo_provable(e);
                std::string s = "LDict<" + tipo_cpp(tipo.element()) + ">{";
                for (size_t i = 0; i < e.entries.size(); ++i) {
                    if (i) s += ", ";
                    s += "{" + expr(*e.entries[i].key) + ", " + expr(*e.entries[i].value) + "}";
                }
                s += "}";
                return s;
            }

            case IrExprKind::Index: {
                // Json (Fase 5.5): el objeto es dinamico -- el indice
                // (Int -> lectura de List, String -> lectura de Dict, ver
                // el comentario de Comprobador::tipo_provable) decide que
                // funcion llamar; cual de las dos es ya se sabe en tiempo
                // de generacion (el indice demostro Int o String, nunca los
                // dos), aunque el objeto en si solo se sepa en tiempo de
                // ejecucion.
                auto tobj = comprobador_.tipo_provable(*e.object);
                if (es_json_dinamico(*tobj)) {
                    auto tidx = comprobador_.tipo_provable(*e.lhs);
                    const char* fn = tidx->kind() == Type::Kind::Int ? "lumen_json_index_int"
                                                                      : "lumen_json_index_str";
                    return std::string(fn) + "(" + expr(*e.object) + ", " + expr(*e.lhs) + ")";
                }
                return expr(*e.object) + ".lumen_get(" + expr(*e.lhs) + ")";
            }

            // "l_this" es el nombre fijo del receptor en un metodo generado
            // (ver generar_metodo_nativo) -- This no lleva `text` en el IR
            // (solo `slot`), a diferencia de un Ident normal.
            case IrExprKind::This:
                return "l_this";

            // o.campo: el nombre del accesor lo pone generar_clase_runtime
            // ("campo_" + nombre de campo), resuelto en tiempo de
            // generacion -- Comprobador::tipo_provable() ya demostro que
            // `o` es de una clase que de verdad tiene ese campo.
            case IrExprKind::Member:
                return expr(*e.object) + ".campo_" + e.text + "()";

            case IrExprKind::Unary:
                return std::string("(") + (e.text == "not" ? "!" : "-") + expr(*e.lhs) + ")";

            // '/' y '%' con un ayudante que comprueba el divisor antes de
            // dividir (ver error_runtime_prelude): el resto de operadores
            // no tiene ningun caso de fallo en tiempo de ejecucion que el
            // VM trate como error controlado, asi que van directos al
            // operador de C++ equivalente.
            case IrExprKind::Binary: {
                // Fase 5.7: `x == null`/`x != null` -- ver el comentario
                // de Comprobador::tipo_provable, mismo caso. `Value` ya
                // sabe responder si es null; no hace falta pasar por
                // valor_json() (el lado null no tiene NADA que convertir).
                if (e.lhs->kind == IrExprKind::NullLit || e.rhs->kind == IrExprKind::NullLit) {
                    const IrExpr& otro = e.lhs->kind == IrExprKind::NullLit ? *e.rhs : *e.lhs;
                    std::string chequeo = expr(otro) + ".is_null()";
                    return e.text == "==" ? chequeo : ("!" + chequeo);
                }
                auto tl = comprobador_.tipo_provable(*e.lhs);
                auto tr = comprobador_.tipo_provable(*e.rhs);
                if (es_json_dinamico(*tl) || es_json_dinamico(*tr)) {
                    // Cada lado se lleva a Value con valor_json() -- si YA
                    // es Json, es la identidad; si es un escalar/List/Dict
                    // nativo, lo envuelve (Value::integer/real/boolean/str
                    // o lumen_valor_de()) -- la MISMA conversion que ya usa
                    // el valor de retorno de una ruta.
                    std::string a = valor_json(*e.lhs);
                    std::string b = valor_json(*e.rhs);
                    if (e.text == "+")  return "lumen_json_add("  + a + ", " + b + ")";
                    if (e.text == "-")  return "lumen_json_arit(" + a + ", " + b + ", '-')";
                    if (e.text == "*")  return "lumen_json_arit(" + a + ", " + b + ", '*')";
                    if (e.text == "/")  return "lumen_json_arit(" + a + ", " + b + ", '/')";
                    if (e.text == "%")  return "lumen_json_arit(" + a + ", " + b + ", '%')";
                    if (e.text == "<")  return "lumen_json_lt("   + a + ", " + b + ")";
                    if (e.text == "<=") return "lumen_json_le("   + a + ", " + b + ")";
                    if (e.text == ">")  return "lumen_json_gt("   + a + ", " + b + ")";
                    if (e.text == ">=") return "lumen_json_ge("   + a + ", " + b + ")";
                    if (e.text == "==") return "lumen_json_eq("   + a + ", " + b + ")";
                    return "lumen_json_ne(" + a + ", " + b + ")"; // "!="
                }
                if (e.text == "/")
                    return "lumen_div_check(" + expr(*e.lhs) + ", " + expr(*e.rhs) + ")";
                if (e.text == "%")
                    return "lumen_mod_check(" + expr(*e.lhs) + ", " + expr(*e.rhs) + ")";
                return "(" + expr(*e.lhs) + " " + operadores_binarios().at(e.text) + " " +
                       expr(*e.rhs) + ")";
            }

            case IrExprKind::Ternary:
                return "(" + expr(*e.object) + " ? " + expr(*e.lhs) + " : " + expr(*e.rhs) + ")";

            case IrExprKind::PreStep:
            case IrExprKind::PostStep: {
                const std::string op = (e.text == "+") ? "++" : "--";
                const std::string v  = nombre_cpp(e.lhs->text);
                return e.kind == IrExprKind::PreStep ? ("(" + op + v + ")") : ("(" + v + op + ")");
            }

            case IrExprKind::Call: {
                // ClassName(args...): el UNICO constructor de la clase C++
                // generada (generar_clase_runtime) es, a proposito, el
                // automapeo -- un valor por campo, en orden -- asi que
                // llamarlo directamente ES la logica del constructor sin
                // cuerpo que tipo_provable() ya demostro. No hace falta
                // ningun simbolo `l_new_X` aparte.
                if (e.call_shape == IrCallShape::ConstructorCall) {
                    const auto& rol = comprobador_.roles().at(e.call_index);
                    std::string s = "L" + rol.clase + "(";
                    for (size_t i = 0; i < e.args.size(); ++i) {
                        if (i) s += ", ";
                        s += expr(*e.args[i].value);
                    }
                    s += ")";
                    return s;
                }
                // receptor.metodo(args...): funcion libre nombrada
                // "l_<Clase>_<metodo>" (ver generar_metodo_nativo) -- no
                // "l_<metodo>" solo, porque dos clases distintas pueden
                // compartir el nombre de un metodo. El receptor va primero,
                // como cualquier llamada a metodo en el IR (ver
                // Emitter::emit_call, IrCallShape::ClassMethodCall).
                if (e.call_shape == IrCallShape::ClassMethodCall) {
                    const auto& rol = comprobador_.roles().at(e.call_index);
                    std::string s = "l_" + rol.clase + "_" + rol.metodo + "(" + expr(*e.object);
                    for (const auto& a : e.args) s += ", " + expr(*a.value);
                    s += ")";
                    return s;
                }

                // "add" sobre una List: sintaxis de metodo nativo
                // (LList::lumen_add), no funcion libre como los de string
                // -- es el unico metodo que Comprobador acepta sobre un
                // receptor List (ver metodos_de()/kList en natives.cpp).
                // Fase 5.10: sobre List<Json> (representada como Value, no
                // LList<Value>, ver tipo_cpp()) no existe .lumen_add() --
                // lumen_json_list_add() (route_runtime_prelude) es su
                // equivalente sobre Value. El argumento pasa por
                // valor_json() (identidad si YA es Json -- p.ej. una fila
                // de sqlite.query() reusada -- o construido desde un
                // DictLit heterogeneo, ver el comentario de Comprobador,
                // mismo caso) en vez de expr(): expr() en un DictLit
                // asume Dict<string,V> homogeneo, exactamente lo que este
                // literal NO es.
                if (e.call_shape == IrCallShape::BuiltinMethodCall &&
                    e.object->type.kind() == Type::Kind::List) {
                    auto tobj = comprobador_.tipo_provable(*e.object);
                    if (tobj && tobj->element().kind() == Type::Kind::Json)
                        return "lumen_json_list_add(" + expr(*e.object) + ", " +
                               valor_json(*e.args[0].value) + ")";
                    return expr(*e.object) + ".lumen_add(" + expr(*e.args[0].value) + ")";
                }

                // "has"/"keys" sobre un Dict: mismo criterio, sintaxis de
                // metodo nativo (LDict::lumen_has/lumen_keys).
                if (e.call_shape == IrCallShape::BuiltinMethodCall &&
                    e.object->type.kind() == Type::Kind::Dict) {
                    if (e.call_name == "has")
                        return expr(*e.object) + ".lumen_has(" + expr(*e.args[0].value) + ")";
                    return expr(*e.object) + ".lumen_keys()"; // "keys": sin argumentos
                }

                // str(x)/len(x)/int(x) (natives.cpp: fn_str/fn_len/fn_int):
                // los unicos BuiltinGlobalCall que tipo_provable() sabe
                // demostrar (ver ese caso, mas arriba). str() pasa por el
                // mismo puente a Value que un valor de retorno de ruta
                // (Generador::valor_json) y llama a Value::to_string(), la
                // MISMA funcion que fn_str. len() traduce a lo que cada
                // tipo expone (string: std::string::size(); List<T>: LList
                // no tiene .size(), su metodo es lumen_len() -- ver
                // list_runtime_prelude), no a una sola llamada generica.
                if (e.call_shape == IrCallShape::BuiltinGlobalCall && e.call_name == "str")
                    return valor_json(*e.args[0].value) + ".to_string()";
                if (e.call_shape == IrCallShape::BuiltinGlobalCall && e.call_name == "len") {
                    auto t = comprobador_.tipo_provable(*e.args[0].value);
                    // Json (Fase 5.5): puede ser string/List/Dict en tiempo
                    // de ejecucion -- lumen_json_len() decide, igual que
                    // fn_len (natives.cpp).
                    if (es_json_dinamico(*t)) return "lumen_json_len(" + expr(*e.args[0].value) + ")";
                    return t->kind() == Type::Kind::String
                               ? "static_cast<int64_t>(" + expr(*e.args[0].value) + ".size())"
                               : expr(*e.args[0].value) + ".lumen_len()";
                }
                // int(x): identidad sobre Int, truncar hacia cero sobre
                // Float/Bool (igual que el cast de C++ que ya usa fn_int
                // sobre Value::as_float()/as_bool()), y sobre String, un
                // fallo real de verdad (lumen_str_to_int, mismo mensaje EXACTO
                // que fn_int -- error_runtime_prelude) -- el unico de los
                // tres casos de int(x) que puede tomar el canal de error.
                // Json (Fase 5.5): lumen_json_as_int() hace la MISMA
                // comprobacion dinamica que fn_int, en tiempo de ejecucion.
                if (e.call_shape == IrCallShape::BuiltinGlobalCall && e.call_name == "int") {
                    auto t = comprobador_.tipo_provable(*e.args[0].value);
                    if (es_json_dinamico(*t)) return "lumen_json_as_int(" + expr(*e.args[0].value) + ")";
                    if (t->kind() == Type::Kind::Int) return expr(*e.args[0].value);
                    if (t->kind() == Type::Kind::String)
                        return "lumen_str_to_int(" + expr(*e.args[0].value) + ")";
                    return "static_cast<int64_t>(" + expr(*e.args[0].value) + ")";
                }

                // Fase 5.9: state.incr/decr/get/set/remove -- llaman
                // directamente a lumen_script::SharedState::instance(), la
                // MISMA clase que fn_state_*() (natives.cpp) ya usa, sin
                // ningun ayudante intermedio: sus metodos ya devuelven
                // exactamente el tipo C++ que necesita cada caso
                // (incr()/decr() -> int64_t, get() -> Value).
                if (e.call_shape == IrCallShape::ReservedMemberCall) {
                    const std::string clave = expr(*e.args[0].value);
                    if (e.call_index == state_incr_id()) {
                        const std::string cuanto = e.args.size() > 1 ? expr(*e.args[1].value) : "1";
                        return "lumen_script::SharedState::instance().incr(" + clave + ", " +
                               cuanto + ")";
                    }
                    if (e.call_index == state_decr_id()) {
                        // fn_state_decr: incr(clave, -cuanto) -- MISMA
                        // funcion, cuanto en negativo.
                        const std::string cuanto = e.args.size() > 1 ? expr(*e.args[1].value) : "1";
                        return "lumen_script::SharedState::instance().incr(" + clave + ", -(" +
                               cuanto + "))";
                    }
                    if (e.call_index == state_remove_id())
                        return "lumen_script::SharedState::instance().remove(" + clave + ")";
                    if (e.call_index == state_get_id()) {
                        // Sin defecto: identidad -- get() ya devuelve
                        // Value::null() si la clave no existe, igual que
                        // fn_state_get sin segundo argumento.
                        if (e.args.size() < 2)
                            return "lumen_script::SharedState::instance().get(" + clave + ")";
                        // Con defecto: fn_state_get devuelve args[1] SOLO
                        // si lo guardado es null (ausente o guardado como
                        // null explicito no se distinguen, ver
                        // SharedState::get) -- misma regla aqui, con una
                        // IIFE para no evaluar get() dos veces.
                        return "([&]{ Value __v = lumen_script::SharedState::instance().get(" +
                               clave + "); return __v.is_null() ? " + valor_json(*e.args[1].value) +
                               " : __v; }())";
                    }
                    if (e.call_index == state_set_id()) {
                        // fn_state_set: guarda args[1] y lo devuelve tal
                        // cual (identidad) -- una IIFE para no construir el
                        // Value dos veces (una para guardar, otra para
                        // devolver).
                        return "([&]{ Value __v = " + valor_json(*e.args[1].value) +
                               "; lumen_script::SharedState::instance().set(" + clave +
                               ", __v); return __v; }())";
                    }
                    return ""; // inalcanzable: Comprobador ya lo descarto
                }

                // BuiltinMethodCall (metodos de string, ver
                // metodo_string_soportado): funcion libre de
                // string_runtime_prelude(), receptor primero, luego los
                // argumentos -- mismo orden que call_method(recv, args) en
                // natives.cpp, solo que en tiempo de compilacion en vez de
                // por nombre en tiempo de ejecucion.
                std::string s = e.call_shape == IrCallShape::BuiltinMethodCall
                                   ? "lumen_str_" + e.call_name
                                   : nombre_cpp(nombre_por_indice_.at(static_cast<size_t>(e.call_index)));
                s += "(";
                if (e.call_shape == IrCallShape::BuiltinMethodCall) s += expr(*e.object);
                for (size_t i = 0; i < e.args.size(); ++i) {
                    if (i || e.call_shape == IrCallShape::BuiltinMethodCall) s += ", ";
                    s += expr(*e.args[i].value);
                }
                s += ")";
                return s;
            }

            // `await sleep(ms)` (Fase 5, el unico await que Comprobador::
            // tipo_provable acepta): co_await de verdad sobre el mismo
            // lumen::sleep() que usa build_routes() para una ruta VM, con
            // el mismo piso de 1ms (lumen_clamp_sleep_ms,
            // route_runtime_prelude) que aplica clamp_sleep_ms() alli --
            // sin el, un `sleep(0)` en un bucle podria fijar un hilo entero
            // reprogramando un temporizador de 0ms sin parar (ver el
            // comentario de clamp_sleep_ms en project.cpp).
            case IrExprKind::Await:
                if (e.lhs->call_shape == IrCallShape::BuiltinGlobalCall)
                    return "co_await lumen::sleep(lumen_clamp_sleep_ms(" +
                           expr(*e.lhs->args[0].value) + "))";
                // await <modulo>.query/exec/last_id(...) (Fase 5.5): mismo
                // camino que bytecode (lumen_script::await_db(), ver
                // db.hpp) -- l_pinned_workers/l_last_exec_workers son las
                // dos variables locales que generate_native_route() declara
                // al principio de cualquier ruta asincrona, equivalentes a
                // los mapas que NativeCtx lleva para una peticion bytecode.
                // El vector de parametros NUNCA se construye con
                // `std::vector<Value>{...}` inline aqui -- GCC 13.3 da un
                // ICE real (internal compiler error en
                // build_special_member_call) al ver un braced-init-list de
                // Value como argumento directo de una llamada que se hace
                // co_await, encontrado compilando la primera ruta con `await
                // <modulo>.query(...)` de verdad (aislado con un
                // reproductor minimo antes de descartarlo como error
                // propio). lumen_db_params(...) (route_runtime_prelude) es
                // el mismo vector, construido por una llamada de funcion
                // normal en su lugar -- eso SI compila limpio, confirmado
                // con el mismo reproductor.
                {
                    const IrExpr& call = *e.lhs;
                    const std::string dbop = call.call_index == db_query_id()    ? "Query"
                                            : call.call_index == db_exec_id()    ? "Exec"
                                            : call.call_index == db_last_id_id() ? "LastId"
                                            : call.call_index == db_begin_id()   ? "Begin"
                                            : call.call_index == db_commit_id()  ? "Commit"
                                                                                 : "Rollback";
                    std::string sql    = "std::string()";
                    std::string params = "lumen_db_params()";
                    if (dbop == "Query" || dbop == "Exec") {
                        sql = expr(*call.args[0].value);
                        params = "lumen_db_params(";
                        for (size_t i = 1; i < call.args.size(); ++i) {
                            if (i > 1) params += ", ";
                            params += valor_json(*call.args[i].value);
                        }
                        params += ")";
                    }
                    return "co_await lumen_script::await_db(lumen_script::DbOp::" + dbop + ", " +
                           literal_string(call.call_name) + ", req.loop, " + sql + ", " + params +
                           ", l_pinned_workers, l_last_exec_workers)";
                }

            default:
                return ""; // inalcanzable: Comprobador ya lo descarto antes de llegar aqui
        }
    }

    std::string block(const IrBlock& b, int indent) {
        std::string s;
        const std::string p(static_cast<size_t>(indent) * 4, ' ');
        for (const auto& st : b) s += p + stmt(*st, indent) + "\n";
        return s;
    }

    std::string stmt(const IrStmt& s, int indent) {
        switch (s.kind) {
            case IrStmtKind::Return:
                if (ruta_ && s.value && comprobador_.es_llamada_respuesta(*s.value))
                    return codigo_llamada_respuesta(*s.value) + "; " + ret_vacio();
                if (ruta_) return respuesta_de_retorno(s.value.get());
                return s.value ? ("return " + expr(*s.value) + ";") : "return;";

            case IrStmtKind::ExprStmt:
                return expr(*s.value) + ";";

            case IrStmtKind::VarDecl: {
                registrar(s.slot, s.name);
                // El tipo REAL (el que Comprobador registro, ver su caso
                // VarDecl) no siempre es el declarado -- Fase 5.5: `int
                // stock = filas[0]["stock"]` genera un `Value stock = ...`,
                // no un `int64_t`, porque el tipo declarado es decorativo y
                // Comprobador ya resolvio que el valor real es dinamico.
                // Cuando coinciden (el caso de siempre) esto no cambia nada.
                //
                // Fase 5.10: un `[]`/`{}` VACIO no tiene tipo propio
                // (Comprobador::tipo_provable, casos ListLit/DictLit, lo
                // rechaza siempre -- `*comprobador_.tipo_provable(*s.value)`
                // sin comprobar seria un `*nullopt`, UB real) -- si
                // stmt_compilable() aceptó esta declaración de todas formas
                // fue precisamente por eso: el tipo declarado ya lo resuelve
                // sin ambigüedad (ver el comentario de Comprobador, mismo
                // caso), así que el tipo real ES el declarado. El valor usa
                // el mismo `{}` que valor_por_defecto() ya da para un
                // List/Dict SIN parametro -- el tipo completo (con su
                // elemento) ya está a la izquierda del `=` en la propia
                // declaración, así que `{}` invoca el constructor por
                // defecto de ESE tipo exacto sin que haga falta deducir
                // nada de un literal sin elementos.
                auto t_valor    = s.value ? comprobador_.tipo_provable(*s.value) : std::nullopt;
                Type tipo_real  = t_valor ? *t_valor : s.decl_type;
                std::string val = (!s.value || !t_valor) ? valor_por_defecto(s.decl_type)
                                                          : expr(*s.value);
                return tipo_cpp(tipo_real) + " " + nombre_cpp(s.name) + " = " + val + ";";
            }

            case IrStmtKind::Assign: {
                if (s.assign_target == IrAssignTarget::Index)
                    return expr(*s.assign_object) + ".lumen_set(" + expr(*s.assign_index) +
                           ", " + expr(*s.value) + ");";
                if (s.assign_target == IrAssignTarget::Member)
                    return expr(*s.assign_object) + ".campo_" + s.assign_field + "() = " +
                           expr(*s.value) + ";";
                // Local: stmt_compilable() ya garantizo esto. El nombre C++
                // es el que se registro cuando esa ranura se declaro (un
                // parametro o un VarDecl anterior).
                return nombre_cpp(ranura_a_nombre_.at(s.assign_slot)) + " = " +
                       expr(*s.value) + ";";
            }

            case IrStmtKind::If: {
                std::string r = "if (" + expr(*s.value) + ") {\n" + block(s.body, indent + 1) +
                                pad(indent) + "}";
                if (!s.orelse.empty())
                    r += " else {\n" + block(s.orelse, indent + 1) + pad(indent) + "}";
                return r;
            }

            case IrStmtKind::While:
                return "while (" + expr(*s.value) + ") {\n" + block(s.body, indent + 1) +
                       pad(indent) + "}";

            case IrStmtKind::Break:    return "break;";
            case IrStmtKind::Continue: return "continue;";

            // Snapshot del tamaño UNA vez (igual que el bytecode: ver el
            // comentario de IrStmtKind::For en emitter.cpp -- `count` se
            // calcula antes del bucle, no en cada vuelta), asi que si el
            // cuerpo hace `.add()` sobre la misma lista que se recorre, el
            // bucle sigue iterando solo sobre los elementos que ya habia al
            // empezar -- el mismo comportamiento que el VM. `break`/
            // `continue` son los de C++ de siempre: al generar un `for` de
            // verdad, no hace falta ningun parcheo de saltos como en el
            // bytecode.
            case IrStmtKind::For: {
                const std::string tipo_var = tipo_cpp(comprobador_.tipo_provable(*s.target)->element());
                std::string r = "{\n";
                r += pad(indent + 1) + "const auto& l__for_items = " + expr(*s.target) + ";\n";
                r += pad(indent + 1) + "const int64_t l__for_count = l__for_items.lumen_len();\n";
                r += pad(indent + 1) +
                     "for (int64_t l__for_i = 0; l__for_i < l__for_count; ++l__for_i) {\n";
                r += pad(indent + 2) + tipo_var + " " + nombre_cpp(s.name) +
                     " = l__for_items.lumen_get(l__for_i);\n";
                r += block(s.body, indent + 2);
                r += pad(indent + 1) + "}\n";
                r += pad(indent) + "}";
                return r;
            }

            case IrStmtKind::Require:
                if (ruta_ && comprobador_.es_llamada_respuesta(*s.target))
                    // "else status(N)"/"else text(...)"/... escribe la
                    // respuesta el mismo (mismo texto que su fn_* en
                    // natives.cpp) -- no hay ningun valor que serializar.
                    return "if (!(" + expr(*s.value) + ")) { " +
                           codigo_llamada_respuesta(*s.target) + "; " + ret_vacio() + " }";
                if (ruta_)
                    return "if (!(" + expr(*s.value) + ")) { " +
                           respuesta_de_retorno(s.target.get()) + " }";
                return "if (!(" + expr(*s.value) + ")) { return " + expr(*s.target) + "; }";

            default:
                return ""; // inalcanzable: Comprobador ya lo descarto antes de llegar aqui
        }
    }

    // El equivalente, en modo ruta, de "return <e>;": serializa el valor a
    // JSON y escribe la respuesta, igual que la cola de build_routes
    // (project.cpp) hace con el `Value` que devuelve el VM -- `nullptr`
    // (un `return` sin valor) es el 204 vacio de esa misma cola.
    std::string respuesta_de_retorno(const IrExpr* e) const {
        if (!e) return "res.status(204).send(\"\"); " + ret_vacio();
        return "res.header(\"Content-Type\", \"application/json; charset=utf-8\").send(" +
               valor_json(*e) + ".to_json_text()); " + ret_vacio();
    }

    // Construye un lumen_script::Value equivalente a `e` -- el puente entre
    // la representacion nativa tipada (rapida, dentro del cuerpo de una
    // ruta) y el Value dinamico que necesita el cuerpo JSON final
    // (Comprobador::es_valor_json ya demostro que esto es valido). A
    // diferencia de expr(), un DictLit/ListLit aqui NO tiene que ser
    // homogeneo: cada entrada se convierte por su cuenta, recursivamente.
    std::string valor_json(const IrExpr& e) const {
        if (e.kind == IrExprKind::DictLit) {
            std::string s = "([&]{ Value::Dict d; ";
            for (const auto& entry : e.entries)
                s += "d[" + expr(*entry.key) + "] = " + valor_json(*entry.value) + "; ";
            s += "return Value::dict(std::move(d)); }())";
            return s;
        }
        if (e.kind == IrExprKind::ListLit) {
            std::string s = "([&]{ Value::List l; ";
            for (const auto& item : e.items) s += "l.push_back(" + valor_json(*item) + "); ";
            s += "return Value::list(std::move(l)); }())";
            return s;
        }
        // Fase 5.8: `<valor>.status(codigo)` -- ver el comentario de
        // Comprobador::es_valor_json, mismo caso. El operador coma
        // secuencia los dos lados (garantizado desde C++17: el efecto
        // lateral de fijar el codigo pasa ANTES de que el valor completo
        // de la expresion -- el del receptor -- se evalue), asi que esto
        // es valido tanto en la posicion mas comun (`return X.status(N)`,
        // valor de retorno directo) como anidado dentro de un
        // DictLit/ListLit mas grande.
        if (e.kind == IrExprKind::Call && e.call_shape == IrCallShape::BuiltinMethodCall &&
            e.call_name == "status")
            return "(res.status(" + expr(*e.args[0].value) + "), " + valor_json(*e.object) + ")";
        auto t = comprobador_.tipo_provable(e);
        // Json (Fase 5.5), o un List<Json>/Dict<string,Json> (mismo
        // tipo_cpp() que Json, ver native_gen.cpp): la expresion YA es un
        // Value -- identidad, sin envolver nada.
        if (es_json_dinamico(*t)) return expr(e);
        switch (t->kind()) {
            case Type::Kind::Int:    return "Value::integer(" + expr(e) + ")";
            case Type::Kind::Float:  return "Value::real(" + expr(e) + ")";
            case Type::Kind::Bool:   return "Value::boolean(" + expr(e) + ")";
            case Type::Kind::String: return "Value::str(" + expr(e) + ")";
            case Type::Kind::List:
            case Type::Kind::Dict:   return "lumen_valor_de(" + expr(e) + ")";
            default: return ""; // inalcanzable: es_valor_json() ya lo descarto
        }
    }

    // El texto C++ (sin `return;` -- lo antepone quien llama) de una de las
    // llamadas que Comprobador::es_llamada_respuesta() ya valido: cada una
    // escribe la respuesta directamente sobre `res`, igual que su fn_* en
    // natives.cpp. `text`/`html`/`json` pasan por valor_json() + el mismo
    // to_string()/to_json_text() de Value que usa la VM -- para un escalar
    // (el unico caso que es_llamada_respuesta acepta para text/html) es la
    // misma llamada, byte a byte, que haria fn_text/fn_html sobre el
    // Value equivalente.
    std::string codigo_llamada_respuesta(const IrExpr& e) const {
        const auto& a = e.args;
        if (e.call_name == "status")
            return "res.status(" + expr(*a[0].value) + ").send(\"\")";
        if (e.call_name == "text")
            return "res.text(" + valor_json(*a[0].value) + ".to_string())";
        if (e.call_name == "html")
            return "res.html(" + valor_json(*a[0].value) + ".to_string())";
        if (e.call_name == "json")
            return "res.header(\"Content-Type\", \"application/json; charset=utf-8\").send(" +
                   valor_json(*a[0].value) + ".to_json_text())";
        if (e.call_name == "redirect") {
            const std::string codigo = a.size() > 1 ? expr(*a[1].value) : "302";
            return "res.status(" + codigo + ").header(\"Location\", " + expr(*a[0].value) +
                   ").send(\"\")";
        }
        if (e.call_name == "send_file") return "res.send_file(" + expr(*a[0].value) + ")";
        return ""; // inalcanzable: es_llamada_respuesta() ya lo descarto
    }

    // La salida temprana de una ruta: `return;` en una funcion `void`
    // normal, `co_return;` cuando el cuerpo es una corrutina (`asincrona_`
    // -- una corrutina no admite un `return` a secas, ver el comentario del
    // constructor). Usado en TODO punto de salida que no sea la caida
    // natural al final del cuerpo (esa se cubre aparte, ver el comentario
    // grande en generate_native_route sobre el 204 implicito).
    //
    // Fase 5.6: si el cuerpo demostro un `await <modulo>.begin()`
    // (Comprobador::usa_transaccion()), CUALQUIER salida -- un `return`
    // explicito, un `status(...)` como ultima expresion, un 400 de
    // parametro invalido -- tiene que cerrar antes la transaccion que
    // pudiera seguir abierta, o la conexion que la abrio quedaria pinned
    // para siempre (ver rollback_pendientes_db, db.hpp). C++ no tiene
    // `finally`; como ret_vacio() ya es el punto de paso obligado de TODO
    // punto de salida temprano (ver el comentario de arriba), basta con
    // anteponer la limpieza aqui una sola vez en vez de repetirla en cada
    // llamante. Igual que bytecode (rollback_pendientes en project.cpp), no
    // se hace desde el catch(...) de la ruta: una excepcion sin atrapar dentro
    // de una transaccion abierta ya es un fallo grave del motor, y este
    // documento evita a proposito que bytecode y --native diverjan en que
    // limpian y que no.
    std::string ret_vacio() const {
        if (!asincrona_) return "return;";
        if (comprobador_.usa_transaccion())
            return "co_await lumen_script::rollback_pendientes_db(l_pinned_workers, req.loop); co_return;";
        return "co_return;";
    }

private:
    const std::vector<std::string>& nombre_por_indice_;
    const Comprobador&               comprobador_;
    bool                             ruta_ = false;
    bool                             asincrona_ = false;
    std::map<int, std::string>      ranura_a_nombre_;

    static std::string pad(int indent) { return std::string(static_cast<size_t>(indent) * 4, ' '); }

    static std::string valor_por_defecto(const Type& t) {
        switch (t.kind()) {
            case Type::Kind::Int:   return "0";
            case Type::Kind::Float: return "0.0";
            case Type::Kind::Bool:  return "false";
            // Fase 5.10: List<Json>/Dict<string,Json> son un Value (no
            // LList<T>/LDict<V>, ver tipo_cpp()) -- "{}" sobre un Value
            // default-construye Value::null(), NO una lista/diccionario
            // vacios (Value::as_list()/as_dict() sobre un null es
            // comportamiento indefinido: lee la union por la rama
            // equivocada). Value::list()/Value::dict() sin argumentos SI
            // dan la lista/diccionario vacios que hace falta aqui.
            case Type::Kind::List:
                return t.element().kind() == Type::Kind::Json ? "Value::list()" : "{}";
            case Type::Kind::Dict:
                return t.element().kind() == Type::Kind::Json ? "Value::dict()" : "{}";
            default: return "{}"; // string: ""
        }
    }
};

// ── ¿Termina el cuerpo SIEMPRE con un return? ───────────────────────────────
//
// Bug real, encontrado probando esto a proposito (no una precaucion
// especulativa): "fn int f(int n): if n > 5: return n" -- sin `else`, sin
// nada despues del `if` -- compila hoy sin ningun aviso (el checker real lo
// acepta: el VM, si el cuerpo se acaba sin `return`, simplemente devuelve
// `null`). Confirmado contra el binario real: bytecode da `{"r":null}` para
// n=3; --native, ANTES de esta comprobacion, daba `{"r":3}` -- basura de la
// pila de C++, porque "llegar al final de una funcion no-void sin return"
// es comportamiento indefinido, no "null" ni ningun otro valor concreto.
//
// La causa de fondo es la misma familia que la correccion critica de más
// arriba: cualquier camino que "cae al final" devuelve `null` en Lumen, un
// tipo distinto del declarado salvo que la funcion sea `void` -- asi que,
// para cualquier retorno no-void, "el cuerpo no demuestra que SIEMPRE
// retorna" es exactamente tan inseguro como "el tipo no coincide". La
// funcion entera se rechaza (bloque_siempre_retorna() se comprueba, para
// retorno no-void, justo despues de block_compilable() en
// generar_funcion_nativa()/generar_metodo_nativo()) en vez de intentar
// generar un `return` sintetico: inventar un valor por defecto seria
// funcionalidad nueva que el VM no tiene (el VM da null, no un "0"/"" que
// esta fase no puede representar de todas formas).
bool bloque_siempre_retorna(const IrBlock& b);

bool stmt_siempre_retorna(const IrStmt& s) {
    switch (s.kind) {
        case IrStmtKind::Return:
            return true;
        // Solo si TIENE `else` y las dos ramas garantizan un return --
        // igual que exige el propio compilador de C++ para el mismo
        // patron. Un `if` sin `else` nunca garantiza nada (el camino
        // "condicion falsa" sigue cayendo al resto del bloque).
        case IrStmtKind::If:
            return !s.orelse.empty() && bloque_siempre_retorna(s.body) &&
                   bloque_siempre_retorna(s.orelse);
        // Require: solo cubre el camino "condicion falsa" (ahi si vuelve,
        // con `otherwise`) -- el camino "condicion verdadera" sigue
        // cayendo al resto del bloque, asi que Require por si sola nunca
        // garantiza nada. While/For: el cuerpo puede ejecutarse cero
        // veces, tampoco garantizan nada por si solos.
        default:
            return false;
    }
}

bool bloque_siempre_retorna(const IrBlock& b) {
    for (const auto& s : b)
        if (s && stmt_siempre_retorna(*s)) return true; // el resto del bloque queda inalcanzable
    return false;
}

// Duplicado deliberado de la funcion del mismo nombre en project.cpp (linkage
// interno en las dos, por el anonimo que las envuelve -- sin choque de
// simbolos): extrae los nombres :param / {param} del patron de ruta, para
// saber si un parametro va a req.params (Path) o a req.query (Query) --
// misma regla exacta que bind_params(). No vale con importarla: vive en un
// binario distinto (`lumen` enlaza project.cpp, esta libreria no).
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

} // namespace

std::optional<FuncionNativa> generar_funcion_nativa(const FnDecl& fn, const IrBlock& body,
                                                     const std::vector<std::string>& nombre_por_indice,
                                                     const TablaFirmas& firmas,
                                                     const TablaClases& clases,
                                                     const TablaRoles& roles) {
    const Type retorno_decl = Type::from_declared(fn.return_type);
    if (!tipo_soportado(retorno_decl, &clases)) return std::nullopt;
    for (const auto& p : fn.params)
        if (!tipo_soportado(Type::from_declared(p.type), &clases)) return std::nullopt;

    Comprobador comprobador(nombre_por_indice, firmas, clases, roles);
    // check_function declara los parametros, en orden, antes que nada mas
    // (ver Emitter::check_function): la ranura i-esima es siempre el
    // parametro i-esimo -- mismo orden que Generador::registrar() mas abajo.
    for (size_t i = 0; i < fn.params.size(); ++i)
        comprobador.registrar(static_cast<int>(i), Type::from_declared(fn.params[i].type));
    if (!comprobador.block_compilable(body, retorno_decl)) return std::nullopt;
    // Fase 5: `await` solo se representa dentro de una ruta (build_routes()
    // es el UNICO punto de entrada nativo que se invoca ya como corrutina;
    // una funcion/metodo suelto se llama directamente en C++ desde otra
    // funcion nativa, nunca con `co_await`, asi que suspenderse a mitad no
    // tiene a quien avisar). Rechazar aqui dejala en bytecode entera, igual
    // que cualquier otra pieza fuera de alcance.
    if (comprobador.usa_await()) return std::nullopt;
    // Ver el comentario de bloque_siempre_retorna(): sin esto, un cuerpo
    // que "cae al final" en algun camino (el VM da null con naturalidad)
    // generaria una funcion C++ no-void que puede llegar al final sin
    // return -- comportamiento indefinido, no "null".
    if (retorno_decl.kind() != Type::Kind::Void && !bloque_siempre_retorna(body))
        return std::nullopt;

    FuncionNativa out;
    out.nombre_lumen = fn.name;

    std::string params;
    for (size_t i = 0; i < fn.params.size(); ++i) {
        if (i) params += ", ";
        params += tipo_cpp(Type::from_declared(fn.params[i].type)) + " " +
                  nombre_cpp(fn.params[i].name);
    }
    out.firma_cpp = tipo_cpp(retorno_decl) + " " + nombre_cpp(fn.name) + "(" + params + ")";

    // `comprobador` ya demostro el tipo de cada expresion del cuerpo --
    // Generador la reusa (Comprobador::tipo_provable) en vez de volver a
    // decidir nada, para el tipo C++ de la variable de un `for` y el valor
    // de un DictLit (ver esos casos en Generador::expr/stmt).
    Generador gen(nombre_por_indice, comprobador);
    for (size_t i = 0; i < fn.params.size(); ++i)
        gen.registrar(static_cast<int>(i), fn.params[i].name);
    out.cuerpo_cpp = "{\n" + gen.block(body, 1) + "}";

    // El wrapper de ABI fija (ver native_abi.hpp): descomprime args[i] al
    // tipo real de cada parametro, llama a la funcion de arriba, y empaqueta
    // el resultado -- o, si la funcion es void, un NativeValue::Tag::Int a 0
    // que nadie mira (el llamante conoce el tipo de retorno declarado, igual
    // que ya conoce la aridad, asi que un valor void nunca se desempaqueta).
    // El try/catch alrededor de todo es el otro lado del canal de error
    // (ver error_runtime_prelude): una funcion nativa que se ejecuta hasta
    // el final sin invocar lumen_native_fail() nunca lo atraviesa, asi que
    // no cuesta nada en el camino normal.
    //
    // Si la frontera de la funcion usa `string`/`List`, la ABI fija de hoy
    // no los representa (ver tipo_abi_soportado): el cuerpo de arriba se
    // genera igual -- otra funcion nativa que la llame directamente se
    // beneficia -- pero sin wrapper, se queda fuera del despacho desde la
    // VM.
    bool frontera_cruza_abi = tipo_abi_soportado(retorno_decl);
    for (const auto& p : fn.params)
        frontera_cruza_abi = frontera_cruza_abi && tipo_abi_soportado(Type::from_declared(p.type));
    if (!frontera_cruza_abi) return out;

    out.simbolo_abi = "lumen_native_" + fn.name;
    std::string cuerpo_wrapper = "    (void)argc;\n    try {\n";
    for (size_t i = 0; i < fn.params.size(); ++i) {
        const Type t = Type::from_declared(fn.params[i].type);
        cuerpo_wrapper += "        " + tipo_cpp(t) + " " + nombre_cpp(fn.params[i].name) +
                          " = args[" + std::to_string(i) + "]." + campo_abi(t.kind()) + ";\n";
    }
    const std::string llamada = nombre_cpp(fn.name) + "(" + [&] {
        std::string s;
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i) s += ", ";
            s += nombre_cpp(fn.params[i].name);
        }
        return s;
    }() + ")";
    if (retorno_decl.kind() == Type::Kind::Void) {
        cuerpo_wrapper += "        " + llamada + ";\n"
                          "        NativeValue salida; salida.tag = NativeValue::Tag::Int; "
                          "salida.i = 0;\n"
                          "        return salida;\n";
    } else {
        cuerpo_wrapper += "        NativeValue salida; salida.tag = " +
                          etiqueta_abi(retorno_decl.kind()) + "; salida." +
                          campo_abi(retorno_decl.kind()) + " = " + llamada + ";\n"
                          "        return salida;\n";
    }
    cuerpo_wrapper += "    } catch (const LumenNativeError&) {\n"
                      "        NativeValue error; error.tag = NativeValue::Tag::Error; "
                      "error.i = 0;\n"
                      "        return error;\n"
                      "    }\n}";
    out.wrapper_cpp = "extern \"C\" NativeValue " + out.simbolo_abi +
                      "(const NativeValue* args, int32_t argc) {\n" +
                      cuerpo_wrapper;
    return out;
}

std::optional<FuncionNativa> generar_metodo_nativo(const std::string& clase, const FnDecl& fn,
                                                    const IrBlock& body,
                                                    const std::vector<std::string>& nombre_por_indice,
                                                    const TablaFirmas& firmas,
                                                    const TablaClases& clases,
                                                    const TablaRoles& roles) {
    auto cit = clases.find(clase);
    if (cit == clases.end()) return std::nullopt; // la propia clase no es representable

    const Type retorno_decl = Type::from_declared(fn.return_type);
    if (!tipo_soportado(retorno_decl, &clases)) return std::nullopt;
    for (const auto& p : fn.params)
        if (!tipo_soportado(Type::from_declared(p.type), &clases)) return std::nullopt;

    Comprobador comprobador(nombre_por_indice, firmas, clases, roles);
    // check_method declara "this" ANTES que los parametros (ranura 0), al
    // reves que check_function -- ver el comentario de Emitter::check_method.
    comprobador.registrar(0, Type::class_ref(clase));
    for (size_t i = 0; i < fn.params.size(); ++i)
        comprobador.registrar(static_cast<int>(i + 1), Type::from_declared(fn.params[i].type));
    if (!comprobador.block_compilable(body, retorno_decl)) return std::nullopt;
    // Fase 5: `await` solo se representa dentro de una ruta (build_routes()
    // es el UNICO punto de entrada nativo que se invoca ya como corrutina;
    // una funcion/metodo suelto se llama directamente en C++ desde otra
    // funcion nativa, nunca con `co_await`, asi que suspenderse a mitad no
    // tiene a quien avisar). Rechazar aqui dejala en bytecode entera, igual
    // que cualquier otra pieza fuera de alcance.
    if (comprobador.usa_await()) return std::nullopt;
    // Ver el comentario de bloque_siempre_retorna(): sin esto, un cuerpo
    // que "cae al final" en algun camino (el VM da null con naturalidad)
    // generaria una funcion C++ no-void que puede llegar al final sin
    // return -- comportamiento indefinido, no "null".
    if (retorno_decl.kind() != Type::Kind::Void && !bloque_siempre_retorna(body))
        return std::nullopt;

    FuncionNativa out;
    out.nombre_lumen = fn.name;

    std::string params = "L" + clase + " l_this";
    for (size_t i = 0; i < fn.params.size(); ++i)
        params += ", " + tipo_cpp(Type::from_declared(fn.params[i].type)) + " " +
                  nombre_cpp(fn.params[i].name);
    out.firma_cpp = tipo_cpp(retorno_decl) + " l_" + clase + "_" + fn.name + "(" + params + ")";

    Generador gen(nombre_por_indice, comprobador);
    // "this" no necesita registrarse por nombre (This tiene su propio caso
    // en Generador::expr, "l_this" fijo); los parametros si, para poder
    // resolver un Assign(Local) por su nombre C++.
    for (size_t i = 0; i < fn.params.size(); ++i)
        gen.registrar(static_cast<int>(i + 1), fn.params[i].name);
    out.cuerpo_cpp = "{\n" + gen.block(body, 1) + "}";

    // Nunca cruza la ABI -- el receptor es siempre de tipo clase, y una
    // clase nunca es tipo_abi_soportado() (igual que string/List/Dict).
    // simbolo_abi/wrapper_cpp quedan vacios: solo invocable directamente en
    // C++ desde otra funcion nativa (otro metodo, o una funcion suelta cuyo
    // cuerpo pasa una instancia sin construirla -- ver el comentario sobre
    // classes_ == nullptr en compile_native).
    return out;
}

// Fase 5.7/5.8: enlaza el cuerpo JSON de la peticion a una instancia de
// `clase`, reproduciendo EXACTAMENTE bind_body() (project.cpp) -- mismos
// mensajes, mismo orden de comprobacion, mismo formato de error -- pero
// como C++ generado en vez de una funcion compartida: a diferencia de
// await_db()/rollback_pendientes_db() (Fase 5.5/5.6), bind_body() depende
// de tipos (ClassInfo, el `Chunk` de una regla validate:, VM) que viven
// dentro de project.cpp y no se exponen. Las reglas validate: de `clase`
// (si tiene, ver ClaseNativa::reglas/reglas_ok) SI se evaluan aqui, pero
// como C++ generado a partir de su IR (construir_clases()), nunca
// invocando una VM: el binding entero -- parseo JSON, campo por campo,
// obligatorio/opcional, tipo, reglas -- es codigo C++ directo sobre
// `Value`, nada que reproduzca una tabla en tiempo de ejecucion.
//
// Un campo `?` ausente o `null` no es error: la variable que lo recibe
// (Value, ver CampoNativo) se queda en Value::null(), y el cuerpo de la
// ruta lo comprueba con `campo != null` (Comprobador::tipo_provable/
// Generador::expr, caso Binary contra NullLit). Un campo NO opcional
// ausente/null, o de tipo equivocado, se acumula en `__msgs`; solo cuando
// TODOS los campos comprobaron sin mensajes se construye la instancia --
// `L<Clase>` no tiene constructor por defecto (generar_clase_runtime), asi
// que no hay forma de "construirla primero y corregirla despues": los
// valores de cada campo viven en variables locales C++ propias hasta que
// se sabe que no hara falta el 422.
std::string codigo_bind_cuerpo(const std::string& nombre_param, const std::string& nombre_clase,
                               const ClaseNativa& clase, const Generador& gen) {
    const std::string cuerpo_var = "__cuerpo_" + nombre_param;
    const std::string msgs_var   = "__msgs_" + nombre_param;
    std::string s;

    s += "    Value " + cuerpo_var + ";\n";
    s += "    if (!Value::parse_json(req.body, " + cuerpo_var + ")) {\n";
    s += "        Value::Dict __d;\n";
    s += "        __d[\"error\"] = Value::str(\"invalid JSON\");\n";
    s += "        res.status(400).header(\"Content-Type\", \"application/json; charset=utf-8\")"
         ".send(Value::dict(std::move(__d)).to_json_text());\n";
    s += "        " + gen.ret_vacio() + "\n";
    s += "    }\n";
    s += "    if (!" + cuerpo_var + ".is_dict()) {\n";
    s += "        Value::Dict __d;\n";
    s += "        __d[\"error\"] = Value::str(\"Validacion fallida\");\n";
    s += "        Value::List __l;\n";
    s += "        __l.push_back(Value::str(\"the body must be a JSON object\"));\n";
    s += "        __d[\"messages\"] = Value::list(std::move(__l));\n";
    s += "        res.status(422).header(\"Content-Type\", \"application/json; charset=utf-8\")"
         ".send(Value::dict(std::move(__d)).to_json_text());\n";
    s += "        " + gen.ret_vacio() + "\n";
    s += "    }\n";
    s += "    std::vector<std::string> " + msgs_var + ";\n";

    // Nombres de las variables C++ de cada campo, en el orden EXACTO de
    // clase.campos -- el mismo orden que espera el (unico) constructor de
    // L<Clase> (generar_clase_runtime).
    std::vector<std::string> campo_vars;
    for (const auto& c : clase.campos) {
        const std::string var = "__c_" + nombre_param + "_" + c.nombre;
        campo_vars.push_back(var);

        const std::string chequeo = c.kind_escalar == Type::Kind::Int    ? "is_int"
                                   : c.kind_escalar == Type::Kind::Float ? "is_num"
                                   : c.kind_escalar == Type::Kind::Bool  ? "is_bool"
                                                                          : "is_str";
        s += "    " + tipo_cpp(c.tipo) + " " + var +
             (c.opcional ? std::string() :
              " = " + std::string(c.kind_escalar == Type::Kind::String ? "std::string()"
                                  : c.kind_escalar == Type::Kind::Bool   ? "false"
                                  : c.kind_escalar == Type::Kind::Float  ? "0.0" : "0")) +
             ";\n";
        s += "    {\n";
        s += "        auto it = " + cuerpo_var + ".as_dict().find(" + literal_string(c.nombre) + ");\n";
        s += "        if (it == " + cuerpo_var + ".as_dict().end() || it->second.is_null()) {\n";
        if (!c.opcional)
            s += "            " + msgs_var + ".push_back(" +
                 literal_string(c.nombre + ": required") + ");\n";
        s += "        } else if (!it->second." + chequeo + "()) {\n";
        s += "            " + msgs_var + ".push_back(" +
             literal_string(c.nombre + ": expected " + c.ortografia) + ");\n";
        s += "        } else {\n";
        if (c.opcional) {
            // valor_encaja() (project.cpp): float/double SIEMPRE se
            // normaliza con Value::real(as_float()) -- un entero JSON en
            // un campo double? tiene que guardarse como Value::Float, no
            // como el Value::Int que trajo el body.
            s += "            " + var + " = " +
                 (c.kind_escalar == Type::Kind::Float ? "Value::real(it->second.as_float());\n"
                                                       : "it->second;\n");
        } else {
            const std::string accesor = c.kind_escalar == Type::Kind::Int    ? "as_int"
                                       : c.kind_escalar == Type::Kind::Float ? "as_float"
                                       : c.kind_escalar == Type::Kind::Bool  ? "as_bool"
                                                                              : "as_str";
            s += "            " + var + " = it->second." + accesor + "();\n";
        }
        s += "        }\n";
        s += "    }\n";
    }

    // Fase 5.8: las reglas de validate: (si las hay -- ver ClaseNativa::
    // reglas) solo se evaluan si NINGUN campo dio mensaje, exactamente
    // igual que bind_body() ("evaluarlas sobre valores ausentes daria
    // errores de tipo en vez del mensaje util"). Cada condicion, generada
    // por construir_clases() con Generador::expr(), espera encontrar una
    // variable "l_<campo>" -- los alias de abajo son EXACTAMENTE eso,
    // referencias a las variables de campo ya rellenas (__c_<param>_<campo>),
    // en un bloque propio para no filtrar esos nombres fuera (una regla
    // podria compartir nombre con un parametro de ruta distinto, p.ej.
    // ":price" en el patron Y un campo "price" en la clase del cuerpo --
    // el alias, con su propio scope, resuelve al campo aqui sin pisar
    // nada de fuera).
    if (!clase.reglas.empty()) {
        s += "    if (" + msgs_var + ".empty()) {\n";
        s += "        {\n";
        for (const auto& c : clase.campos)
            s += "            auto& " + nombre_cpp(c.nombre) + " = __c_" + nombre_param + "_" +
                 c.nombre + ";\n";
        for (const auto& r : clase.reglas) {
            s += "            if (!(" + r.condicion_cpp + ")) " + msgs_var + ".push_back(" +
                 literal_string(r.mensaje) + ");\n";
        }
        s += "        }\n";
        s += "    }\n";
    }

    s += "    if (!" + msgs_var + ".empty()) {\n";
    // bind_body() (project.cpp) hace exactamente esto antes de responder:
    // deja los mensajes en el thread_local que lee `on error 422:` (ver
    // app.on_error() en main.cpp) -- sin esto, un `on error 422:` que la
    // aplicacion declare veria `error.messages` vacio para una ruta
    // nativa, aunque el cuerpo de ESTA respuesta (por si la app no declara
    // ese manejador y deja pasar la de aqui) ya los lleve bien.
    s += "        lumen_script::last_validation_messages() = " + msgs_var + ";\n";
    s += "        Value::Dict __d;\n";
    s += "        __d[\"error\"] = Value::str(\"Validacion fallida\");\n";
    s += "        Value::List __l;\n";
    s += "        for (const auto& __m : " + msgs_var + ") __l.push_back(Value::str(__m));\n";
    s += "        __d[\"messages\"] = Value::list(std::move(__l));\n";
    s += "        res.status(422).header(\"Content-Type\", \"application/json; charset=utf-8\")"
         ".send(Value::dict(std::move(__d)).to_json_text());\n";
    s += "        " + gen.ret_vacio() + "\n";
    s += "    }\n";

    // Construido SOLO llegados aqui: L<Clase> no tiene constructor por
    // defecto, asi que hasta este punto ningun campo se ha podido asignar
    // a una instancia real -- cada uno vivio en su propia variable local.
    s += "    L" + nombre_clase + " " + nombre_cpp(nombre_param) + "(";
    for (size_t i = 0; i < campo_vars.size(); ++i) {
        if (i) s += ", ";
        s += campo_vars[i];
    }
    s += ");\n";
    return s;
}

std::optional<RutaNativa> generate_native_route(const RouteDecl& route, const IrBlock& body,
                                              int indice,
                                              const std::vector<std::string>& nombre_por_indice,
                                              const TablaFirmas& firmas,
                                              const TablaClases& clases,
                                              const TablaRoles& roles) {
    // ws/sse no producen un IrBlock comparable (su bucle vive fuera del
    // cuerpo, en build_routes) -- ni falta que hace: nunca llegan aqui con
    // logica que valga la pena compilar de esta forma.
    if (route.method == "WS" || route.method == "SSE") return std::nullopt;

    struct ParamRuta {
        std::string nombre;
        Type        tipo = Type::unknown();
        bool        en_path = false;
        bool        con_defecto = false;
        std::string texto_defecto; // solo si con_defecto
        bool        es_cuerpo = false;         // Fase 5.7: parametro de tipo clase
        const ClaseNativa* clase = nullptr;     // solo si es_cuerpo
    };
    const auto en_patron = pattern_params(route.pattern);
    std::vector<ParamRuta> params;
    bool cuerpo_visto = false;
    for (const auto& p : route.params) {
        // Fase 5.7: un parametro cuyo tipo es una clase representable
        // (TablaClases) y SIN validate: se enlaza al cuerpo de la
        // peticion -- misma idea que bind_params() (project.cpp), pero
        // reproducida a mano aqui por el mismo motivo que el resto de esta
        // funcion: bind_params todavia no ha corrido cuando
        // compile_native() llama a esto (ver el comentario grande sobre
        // el orden en compile(), project.cpp), asi que las reglas
        // estructurales ("un unico parametro de cuerpo", "GET/DELETE no
        // llevan cuerpo", "no puede estar en el patron") se repiten aqui.
        // Si algo no encaja, esta ruta cae a bytecode y bind_params dara
        // el error real (o la aceptara, si el problema era solo que esta
        // fase no llega) -- nunca un handler nativo silenciando un caso
        // que bind_params habria rechazado.
        auto cit = clases.find(p.type.name);
        if (cit != clases.end()) {
            if (p.type.optional) return std::nullopt; // "Clase?" como cuerpo: fuera de alcance
            bool en_path_cuerpo = std::find(en_patron.begin(), en_patron.end(), p.name) !=
                                  en_patron.end();
            if (en_path_cuerpo) return std::nullopt;
            if (route.method == "GET" || route.method == "DELETE") return std::nullopt;
            if (cuerpo_visto) return std::nullopt;
            if (!cit->second.reglas_ok) return std::nullopt; // ver el comentario de ClaseNativa
            cuerpo_visto = true;
            ParamRuta pr;
            pr.nombre    = p.name;
            pr.tipo      = Type::class_ref(p.type.name);
            pr.en_path   = false;
            pr.es_cuerpo = true;
            pr.clase     = &cit->second;
            params.push_back(std::move(pr));
            continue;
        }

        // Alcance de este primer corte (ver el comentario de RutaNativa en
        // el header): sin `?`, y solo los cuatro escalares -- un
        // File/List<File> nunca produce ninguno de esos Type::Kind, asi
        // que ya queda excluido por la misma comprobacion.
        if (p.type.optional) return std::nullopt;
        Type t = Type::from_declared(p.type);
        if (t.kind() != Type::Kind::Int && t.kind() != Type::Kind::Float &&
            t.kind() != Type::Kind::Bool && t.kind() != Type::Kind::String)
            return std::nullopt;
        bool en_path = std::find(en_patron.begin(), en_patron.end(), p.name) != en_patron.end();

        bool        con_defecto = false;
        std::string texto_defecto;
        if (p.default_value) {
            // "un parametro de ruta no puede tener valor por defecto" -- la
            // misma regla que bind_params() (project.cpp): si esta ruta
            // llega a compilar de todas formas (no deberia, bind_params la
            // rechazara en build_routes), mejor que se quede en bytecode a
            // que un handler nativo silencie el error.
            if (en_path) return std::nullopt;
            // Mismo extractor EXACTO que bind_params(): solo constantes
            // literales, resueltas aqui, en tiempo de compilacion -- un
            // valor por defecto que no sea uno de estos tres tipos de
            // literal ya es un error de compilacion en bind_params, asi
            // que esta ruta tampoco necesita intentarlo.
            const Expr& d = *p.default_value;
            if (d.kind == ExprKind::StringLit) texto_defecto = d.text;
            else if (d.kind == ExprKind::IntLit) texto_defecto = std::to_string(d.int_value);
            else if (d.kind == ExprKind::BoolLit) texto_defecto = d.bool_value ? "true" : "false";
            else return std::nullopt;
            con_defecto = true;
        }
        params.push_back({p.name, std::move(t), en_path, con_defecto, std::move(texto_defecto)});
    }

    Comprobador comprobador(nombre_por_indice, firmas, clases, roles);
    // check_route declara los parametros de la ruta, en orden, antes que
    // nada mas (ver Emitter::check_route) -- mismo orden que se registra
    // aqui y en Generador::registrar() mas abajo.
    for (size_t i = 0; i < params.size(); ++i)
        comprobador.registrar(static_cast<int>(i), params[i].tipo);
    // Type::json() es el centinela de "esto es una ruta": Return/Require
    // solo tienen que demostrar que su valor es construible como Value
    // (Comprobador::es_valor_json), no un Type nativo exacto -- ver esos dos
    // casos en Comprobador::stmt_compilable.
    if (!comprobador.block_compilable(body, Type::json())) return std::nullopt;
    // Fase 5: si el cuerpo demostro algun `await` (hoy: solo `await
    // sleep(ms)`, ver Comprobador::tipo_provable), esta ruta se genera
    // como una corrutina de verdad -- ver el comentario del constructor de
    // Generador y RutaNativa::asincrona.
    const bool asincrona = comprobador.usa_await();

    RutaNativa out;
    out.simbolo    = "lumen_native_route_" + std::to_string(indice);
    out.asincrona  = asincrona;

    // El binding de parametros: mismo criterio EXACTO que prepare_args()
    // (project.cpp) -- Path va a req.params, Query a req.query; ausente da
    // el "cero" del tipo en silencio; presente pero sin parsear es un 400
    // con el mismo cuerpo {"error":"parametro invalido","param":...,
    // "esperado":...,"recibido":...}. No hay manera de llamar a prepare_args
    // desde aqui (vive en otro binario, y trabaja sobre Value/ParamBind, no
    // sobre los tipos nativos que declara esta ruta) -- se reproduce en vez
    // de reusarse, con las mismas funciones de conversion que
    // route_runtime_prelude() antepone una sola vez.
    // Todo el cuerpo va dentro de un try/catch: a diferencia de una funcion
    // (donde el canal de error -- lumen_native_fail()/LumenNativeError, ver
    // error_runtime_prelude -- lo atrapa el wrapper de la ABI, generado solo
    // para funciones que la cruzan), una ruta no tiene ningun wrapper --
    // nadie la llama a traves de la ABI, la invoca build_routes()
    // directamente. Sin este catch, una division/modulo por cero o un
    // indice de List fuera de rango DENTRO de una ruta (p.ej. `a % b` con
    // `b` dinamico, ya demostrado seguro por Comprobador pero no exento de
    // fallar en tiempo de ejecucion) lanzaria una excepcion sin nadie que la
    // atrape -- confirmado contra el binario real: sin este catch, bytecode
    // daba 500 con {"error":"modulo por cero","en":"archivo:linea:col"} y
    // --native daba 500 con {"error":"Internal Server Error"} (el generico
    // del motor para una excepcion sin atrapar, ver dispatch en
    // http_connection.cpp) -- no un crash, pero si una respuesta distinta,
    // exactamente la clase de divergencia silenciosa que este documento
    // entero existe para no permitir. El mensaje coincide EXACTO con
    // bytecode (mismo lumen_native_error_message()); "en" no puede ser
    // igual de preciso -- el codigo nativo no lleva ninguna nocion de
    // linea/columna en tiempo de ejecucion -- asi que usa "METODO patron",
    // el mismo respaldo que ya usa build_routes cuando el VM tampoco tiene
    // una ubicacion precisa.
    const std::string donde = literal_string(route.method + " " + route.pattern);
    // Construido ya aqui (antes del binding de parametros) solo para poder
    // usar gen.ret_vacio() en el 400 de un parametro invalido -- registrar()
    // (que si depende de `params`) se llama despues, mas abajo.
    Generador gen(nombre_por_indice, comprobador, /*ruta=*/true, asincrona);
    std::string cuerpo = "{\n    try {\n";
    // Igual que prepare_args() (project.cpp) al principio de CUALQUIER
    // ruta bytecode, no solo una con parametro de cuerpo: si esta peticion
    // termina en un codigo con `on error <code>:` declarado, ese manejador
    // lee `error.messages` de este MISMO hilo (ver app.on_error() en
    // main.cpp, ctx.error_messages = &last_validation_messages()) -- sin
    // limpiarlo aqui, una ruta que de un error SIN pasar por
    // codigo_bind_cuerpo() (p.ej. `return status(422)` a mano) heredaria
    // los mensajes de una validacion de OTRA peticion anterior en este
    // mismo hilo. Barato: un vector vacio no reasigna memoria al
    // limpiarse.
    cuerpo += "    lumen_script::last_validation_messages().clear();\n";
    // Fase 5.5/5.6: equivalentes locales, para toda la duracion de esta
    // peticion, de NativeCtx::pinned_workers/last_exec_workers --
    // lumen_script::await_db() (db.hpp) los toma por referencia para fijar
    // una consulta a la misma conexion que abrio una transaccion (begin(),
    // ver Comprobador::usa_transaccion()) o que hizo el ultimo exec() (para
    // que last_id() lea la conexion correcta); rollback_pendientes_db()
    // (llamada desde ret_vacio()/el 204 implicito, ver esos comentarios)
    // los consulta al final para cerrar lo que el handler haya dejado
    // abierto. Declarados siempre que la ruta es asincrona, se usen o no:
    // mas simple que detectar de antemano si el cuerpo de verdad toca una
    // base de datos, y el coste de un std::map vacio es insignificante.
    if (asincrona)
        cuerpo += "    std::map<std::string, int> l_pinned_workers, l_last_exec_workers;\n";
    for (const auto& p : params) {
        if (p.es_cuerpo) {
            cuerpo += codigo_bind_cuerpo(p.nombre, p.tipo.class_name(), *p.clase, gen);
            continue;
        }
        const std::string mapa = p.en_path ? "req.params" : "req.query";
        const std::string nombre = nombre_cpp(p.nombre);
        cuerpo += "    " + tipo_cpp(p.tipo) + " " + nombre + ";\n";
        cuerpo += "    {\n";
        cuerpo += "        auto it = " + mapa + ".find(" + literal_string(p.nombre) + ");\n";
        cuerpo += "        bool presente = it != " + mapa + ".end();\n";
        cuerpo += "        std::string raw = presente ? it->second : std::string();\n";
        // Ausente pero con valor por defecto: se trata como SI hubiera
        // llegado ese texto -- exactamente lo que hace prepare_args()
        // (project.cpp) antes de llamar a coerce(), asi que un defecto mal
        // tipado (p.ej. `bool activo = "20"`) da el mismo 400 que un valor
        // real mal tipado, con "recibido" mostrando el propio defecto.
        if (p.con_defecto)
            cuerpo += "        if (!presente) { raw = " + literal_string(p.texto_defecto) +
                      "; presente = true; }\n";
        if (p.tipo.kind() == Type::Kind::String) {
            cuerpo += "        " + nombre + " = raw;\n";
        } else {
            const std::string cero = p.tipo.kind() == Type::Kind::Bool   ? "false"
                                    : p.tipo.kind() == Type::Kind::Float ? "0.0"
                                                                          : "0";
            const std::string fn = p.tipo.kind() == Type::Kind::Bool   ? "lumen_route_coerce_bool"
                                  : p.tipo.kind() == Type::Kind::Float ? "lumen_route_coerce_float"
                                                                        : "lumen_route_coerce_int";
            cuerpo += "        if (!presente) {\n";
            cuerpo += "            " + nombre + " = " + cero + ";\n";
            cuerpo += "        } else if (!" + fn + "(raw, " + nombre + ")) {\n";
            cuerpo += "            Value::Dict __d;\n";
            cuerpo += "            __d[\"error\"] = Value::str(\"invalid parameter\");\n";
            cuerpo += "            __d[\"param\"] = Value::str(" + literal_string(p.nombre) + ");\n";
            cuerpo += "            __d[\"expected\"] = Value::str(" + literal_string(p.tipo.to_string()) +
                      ");\n";
            cuerpo += "            __d[\"received\"] = Value::str(raw);\n";
            cuerpo += "            res.status(400).header(\"Content-Type\", "
                      "\"application/json; charset=utf-8\")"
                      ".send(Value::dict(std::move(__d)).to_json_text());\n";
            cuerpo += "            " + gen.ret_vacio() + "\n";
            cuerpo += "        }\n";
        }
        cuerpo += "    }\n";
    }

    for (size_t i = 0; i < params.size(); ++i) gen.registrar(static_cast<int>(i), params[i].nombre);
    cuerpo += gen.block(body, 1);
    // A diferencia de una funcion, aqui NO hace falta bloque_siempre_retorna:
    // la funcion generada es `void`/`Task<void>`, asi que "caer al final"
    // es C++ perfectamente valido en los dos casos (nunca comportamiento
    // indefinido, y una corrutina `Task<void>` que cae al final hace
    // return_void() con la misma naturalidad) -- y es, ademas, EXACTAMENTE
    // lo mismo que hace el VM cuando el cuerpo de una ruta termina sin
    // `return` explicito (null -> 204, ver build_routes).
    //
    // Este es el unico punto de salida que ret_vacio() no cubre (ver su
    // comentario): la caida natural al final del cuerpo, sin ningun
    // `return`. Si la ruta demostro un begin() en algun punto, necesita el
    // mismo cierre de transaccion que cualquier otra salida.
    if (asincrona && comprobador.usa_transaccion())
        cuerpo += "    co_await lumen_script::rollback_pendientes_db(l_pinned_workers, req.loop);\n";
    cuerpo += "    res.status(204).send(\"\");\n";
    cuerpo += "    } catch (const LumenNativeError&) {\n";
    cuerpo += "        Value::Dict __e;\n";
    cuerpo += "        __e[\"error\"] = Value::str(lumen_native_error_message());\n";
    cuerpo += "        __e[\"en\"] = Value::str(" + donde + ");\n";
    cuerpo += "        res.status(500).header(\"Content-Type\", \"application/json; charset=utf-8\")"
              ".send(Value::dict(std::move(__e)).to_json_text());\n";
    cuerpo += "    }\n";
    cuerpo += "}";

    out.cuerpo_cpp = "extern \"C\" " + std::string(asincrona ? "lumen::Task<void>" : "void") +
                     " " + out.simbolo + "(lumen::Request& req, lumen::Response& res) " + cuerpo;
    return out;
}

std::string generar_clase_runtime(const std::string& nombre_clase, const ClaseNativa& clase) {
    const std::string tipo = "L" + nombre_clase;
    const std::string caja = tipo + "_Box";

    // Parametros del UNICO constructor -- el de la clase C++ y el de su
    // caja -- en el orden de `clase.campos`: es, a la vez, el layout del
    // struct y el automapeo del unico constructor Lumen que esta fase
    // compila (ver RolFuncion::tiene_cuerpo en construir_clases()).
    std::string params_tipados, params_nombres;
    for (size_t i = 0; i < clase.campos.size(); ++i) {
        if (i) { params_tipados += ", "; params_nombres += ", "; }
        params_tipados += tipo_cpp(clase.campos[i].tipo) + " " + nombre_cpp(clase.campos[i].nombre);
        params_nombres += nombre_cpp(clase.campos[i].nombre);
    }

    std::string s = "struct " + caja + " {\n    long rc;\n";
    for (const auto& c : clase.campos) s += "    " + tipo_cpp(c.tipo) + " f_" + c.nombre + ";\n";
    s += "    " + caja + "(" + params_tipados + ") : rc(1)";
    for (const auto& c : clase.campos) s += ", f_" + c.nombre + "(" + nombre_cpp(c.nombre) + ")";
    s += " {}\n};\n";

    // Semantica de referencia real (§8), mismo diseño que LList/LDict:
    // copiar una instancia copia el puntero a la caja, no los campos -- dos
    // variables sobre la misma instancia ven las mutaciones la una de la
    // otra, igual que Value::Dict en el VM (que es, hoy, la representacion
    // de CUALQUIER instancia -- ver el comentario de §7/§8).
    s += "class " + tipo + " {\npublic:\n";
    s += "    " + tipo + "(" + params_tipados + ") : b_(new " + caja + "(" + params_nombres + ")) {}\n";
    s += "    " + tipo + "(const " + tipo + "& o) : b_(o.b_) { ++b_->rc; }\n";
    s += "    " + tipo + "(" + tipo + "&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }\n";
    s += "    " + tipo + "& operator=(const " + tipo + "& o) {\n"
         "        if (b_ != o.b_) { rel(); b_ = o.b_; ++b_->rc; }\n"
         "        return *this;\n"
         "    }\n";
    s += "    " + tipo + "& operator=(" + tipo + "&& o) noexcept {\n"
         "        if (this != &o) { rel(); b_ = o.b_; o.b_ = nullptr; }\n"
         "        return *this;\n"
         "    }\n";
    s += "    ~" + tipo + "() { rel(); }\n";
    // Un accesor por campo, "campo_<nombre>()", que devuelve una REFERENCIA
    // -- sirve para leer (Member) y para escribir (Assign Member: "x.campo_f()
    // = v") con el mismo metodo, sin necesitar un getter y un setter por
    // separado (ver Generador::expr/stmt, casos Member).
    for (const auto& c : clase.campos)
        s += "    " + tipo_cpp(c.tipo) + "& campo_" + c.nombre + "() const { return b_->f_" +
             c.nombre + "; }\n";
    s += "private:\n    void rel() { if (b_ && --b_->rc == 0) delete b_; }\n    " + caja +
         "* b_;\n};\n";
    return s;
}

void construir_clases(const Program& prog, const ClassSigs& clases_sig,
                      const FunctionSigs& fns, const std::set<std::string>* imports,
                      TablaClases& clases, TablaRoles& roles) {
    for (const auto& c : prog.classes) {
        // Solo entra si TODOS los campos son representables -- el lenguaje ya
        // restringe los campos de clase a los cuatro escalares
        // (project.cpp), asi que el unico motivo real para quedar fuera,
        // hoy, es un campo de un tipo que ni siquiera el lenguaje permite
        // (no debería pasar nunca: build_classes ya lo rechazaria antes).
        // Fase 5.7: un campo `?` SI entra -- ver el comentario de
        // CampoNativo sobre por que se almacena como Json.
        ClaseNativa cn;
        bool todos_soportados = true;
        for (const auto& f : c.fields) {
            Type t = Type::from_declared(f.type);
            Type::Kind k = t.kind();
            if (k != Type::Kind::Int && k != Type::Kind::Float &&
                k != Type::Kind::Bool && k != Type::Kind::String) {
                todos_soportados = false;
                break;
            }
            CampoNativo cf;
            cf.nombre       = f.name;
            cf.opcional     = t.is_optional();
            cf.kind_escalar = k;
            cf.ortografia   = t.base_name();
            cf.tipo         = cf.opcional ? Type::json() : t;
            cn.campos.push_back(std::move(cf));
        }
        if (!todos_soportados) continue;

        // Fase 5.8: cada `validate:` se compila a IR (Emitter::
        // check_condition, el mismo canario que build_classes() -- project.cpp
        // -- ya usaba para comparar bytecode/IR y descartaba) y se reprueba
        // como Type::Kind::Bool contra los campos, con un Comprobador propio
        // (registrado con clase.campos[i].tipo -- consciente de Json para un
        // campo opcional, a diferencia del tipo que ve el emisor de
        // bytecode, que no distingue "puede ser null"). Sin funciones ni
        // otras clases visibles (una regla de validate: nunca las necesita
        // en la practica: comparaciones, aritmetica, metodos de string) --
        // si alguna algun dia lo intenta, faltara en firmas_vacias/
        // clases_vacias y esta rama la rechazara limpio, no con un fallo a
        // medias.
        if (!c.rules.empty()) {
            std::vector<TypedName> field_names;
            for (const auto& f : c.fields) field_names.push_back({f.name, f.type.name});

            static const std::vector<std::string> nombre_por_indice_vacio;
            static const TablaFirmas               firmas_vacias;
            static const TablaClases               clases_vacias;
            static const TablaRoles                roles_vacios;
            Comprobador comprobador_regla(nombre_por_indice_vacio, firmas_vacias, clases_vacias,
                                          roles_vacios);
            for (size_t i = 0; i < cn.campos.size(); ++i)
                comprobador_regla.registrar(static_cast<int>(i), cn.campos[i].tipo);
            // Generador::expr() traduce Ident por NOMBRE (nombre_cpp(e.text),
            // no por slot -- ver el comentario de Generador::expr, caso
            // Ident): el C++ generado aqui espera encontrar una variable
            // "l_<campo>" en el momento en que se evalue -- codigo_bind_cuerpo()
            // las provee como alias con ESE nombre exacto, en un bloque
            // propio, justo antes de evaluar las reglas.
            Generador gen_regla(nombre_por_indice_vacio, comprobador_regla);

            bool todas_ok = true;
            std::vector<ReglaNativa> reglas;
            for (const auto& r : c.rules) {
                DiagnosticBag diags_desechados, shadow_desechado;
                Chunk         chunk_desechado;
                Emitter       emitter(diags_desechados, &fns, &clases_sig, imports);
                IrExprPtr     ir = emitter.check_condition(*r.condition, field_names,
                                                           chunk_desechado, shadow_desechado);
                if (!ir || !shadow_desechado.empty()) { todas_ok = false; break; }
                auto tipo = comprobador_regla.tipo_provable(*ir);
                if (!tipo || tipo->kind() != Type::Kind::Bool) { todas_ok = false; break; }
                reglas.push_back({gen_regla.expr(*ir), r.message});
            }
            if (todas_ok) cn.reglas = std::move(reglas);
            else          cn.reglas_ok = false;
        }

        auto sig_it = clases_sig.find(c.name);
        if (sig_it == clases_sig.end()) continue; // no deberia pasar: viene del mismo prog

        for (const auto& m : c.methods) {
            auto fsig_it = sig_it->second.methods.find(m.name);
            if (fsig_it == sig_it->second.methods.end()) continue;
            FirmaNativa firma;
            firma.retorno = Type::from_declared(m.return_type);
            for (const auto& p : m.params) firma.params.push_back(Type::from_declared(p.type));
            cn.metodos[m.name] = std::move(firma);
            roles[static_cast<int>(fsig_it->second.index)] = RolFuncion{c.name, m.name, true};
        }

        for (const auto& ct : c.ctors) {
            auto idx_it = sig_it->second.ctors.find(ct.params.size());
            if (idx_it == sig_it->second.ctors.end()) continue;
            roles[static_cast<int>(idx_it->second)] = RolFuncion{c.name, "", ct.has_body};
        }
        // El constructor implicito que build_class_signatures() sintetiza
        // cuando la clase no declara ninguno (project.cpp): "un parametro
        // por campo, en orden", sin cuerpo. No vive en Program::classes
        // (se sintetiza aparte, dentro de compile()), asi que se repite la
        // misma regla aqui.
        if (c.ctors.empty() && !c.fields.empty()) {
            auto idx_it = sig_it->second.ctors.find(c.fields.size());
            if (idx_it != sig_it->second.ctors.end())
                roles[static_cast<int>(idx_it->second)] = RolFuncion{c.name, "", false};
        }

        clases[c.name] = std::move(cn);
    }
}

std::string abi_prelude() {
    // Identico, campo a campo, a la definicion de include/lumen_script/native_abi.hpp.
    return
        "struct NativeValue {\n"
        "    enum class Tag : int32_t { Int, Float, Bool, Error } tag = Tag::Int;\n"
        "    union { int64_t i; double d; bool b; };\n"
        "};\n"
        "using CompiledFn = NativeValue (*)(const NativeValue*, int32_t);\n";
}

std::string string_runtime_prelude() {
    // Cada una calca, a proposito, la rama `string` de call_method() en
    // natives.cpp -- misma logica, tipos nativos en vez de Value.
    return
        "static bool lumen_str_starts_with(const std::string& s, const std::string& n) {\n"
        "    return s.rfind(n, 0) == 0;\n"
        "}\n"
        "static bool lumen_str_ends_with(const std::string& s, const std::string& n) {\n"
        "    return s.size() >= n.size() && s.compare(s.size() - n.size(), n.size(), n) == 0;\n"
        "}\n"
        "static bool lumen_str_contains(const std::string& s, const std::string& n) {\n"
        "    return s.find(n) != std::string::npos;\n"
        "}\n"
        "static std::string lumen_str_upper(std::string s) {\n"
        "    for (char& c : s) c = static_cast<char>(::toupper((unsigned char)c));\n"
        "    return s;\n"
        "}\n"
        "static std::string lumen_str_lower(std::string s) {\n"
        "    for (char& c : s) c = static_cast<char>(::tolower((unsigned char)c));\n"
        "    return s;\n"
        "}\n"
        "static std::string lumen_str_trim(const std::string& s) {\n"
        "    size_t a = s.find_first_not_of(\" \\t\\r\\n\");\n"
        "    if (a == std::string::npos) return \"\";\n"
        "    size_t b = s.find_last_not_of(\" \\t\\r\\n\");\n"
        "    return s.substr(a, b - a + 1);\n"
        "}\n";
}

std::string error_runtime_prelude() {
    // g_lumen_native_error: por hilo, porque varias peticiones concurrentes
    // pueden estar cada una a mitad de una llamada nativa a la vez. El
    // mensaje es valido hasta la SIGUIENTE llamada nativa en el mismo hilo
    // -- quien lo lee (vm.cpp) lo copia a su propio std::string antes de
    // hacer cualquier otra cosa.
    //
    // lumen_div_check/lumen_mod_check son los puntos de fallo de la
    // aritmetica (division y modulo por cero, vm.cpp: "division por cero" /
    // "modulo por cero") -- plantillas porque el generador los usa tanto
    // para int64_t/int64_t (modulo) como para cualquier mezcla de
    // int64_t/double (division; ver Comprobador::tipo_provable(), que ya
    // garantiza que una division entre dos int nunca llega aqui). LList
    // (list_runtime_prelude) reusa lumen_native_fail() para "indice fuera
    // de rango".
    return
        "static thread_local std::string g_lumen_native_error;\n"
        "struct LumenNativeError {};\n"
        "[[noreturn]] static void lumen_native_fail(std::string msg) {\n"
        "    g_lumen_native_error = std::move(msg);\n"
        "    throw LumenNativeError{};\n"
        "}\n"
        "extern \"C\" const char* lumen_native_error_message() {\n"
        "    return g_lumen_native_error.c_str();\n"
        "}\n"
        "template <class T, class U>\n"
        "static auto lumen_div_check(T a, U b) {\n"
        "    if (b == 0) lumen_native_fail(\"division by zero\");\n"
        "    return a / b;\n"
        "}\n"
        "template <class T, class U>\n"
        "static auto lumen_mod_check(T a, U b) {\n"
        "    if (b == 0) lumen_native_fail(\"modulo by zero\");\n"
        "    return a % b;\n"
        "}\n"
        // int(x) sobre string (natives.cpp: fn_int) -- mismo std::stoll SIN
        // comprobar cuanto consumio (fn_int tampoco lo hace: "12abc" da 12,
        // no un error) y mismo mensaje EXACTO cuando ni eso analiza.
        "static int64_t lumen_str_to_int(const std::string& s) {\n"
        "    try { return std::stoll(s); }\n"
        "    catch (...) { lumen_native_fail(\"int(): '\" + s + \"' no es un numero\"); }\n"
        "}\n";
}

std::string list_runtime_prelude() {
    // Caja con refcount NO atomico -- a diferencia del Caja de value.hpp,
    // una LList nunca cruza la ABI (tipo_abi_soportado la excluye, igual
    // que string) asi que nunca viaja entre hilos: nace y muere dentro de
    // una sola llamada nativa (la de la VM que la desencadeno) en un solo
    // hilo. Semantica de referencia real (§8): copiar una LList copia el
    // puntero a la caja, no los datos -- dos variables que apuntan a la
    // misma lista ven las mutaciones la una de la otra, igual que
    // Value::List en el VM.
    //
    // lumen_get/lumen_set comprueban el indice con lumen_native_fail() en
    // vez de comportamiento indefinido -- el mismo canal de error que ya
    // usan division/modulo, con el mismo formato de mensaje que GetIndex/
    // SetIndex en vm.cpp ("indice fuera de rango: N (tamano M)").
    //
    // La guia de deduccion permite escribir `LList{1LL, 2LL, 3LL}` sin
    // template argument explicito (Generador::expr, caso ListLit): el
    // compilador deduce T de los elementos, asi que el generador no
    // necesita saber el tipo para construir el literal.
    return
        "template <class T>\n"
        "struct LListBox { long rc; std::vector<T> v; LListBox() : rc(1) {} };\n"
        "template <class T>\n"
        "class LList {\n"
        "public:\n"
        "    LList() : b_(new LListBox<T>()) {}\n"
        "    LList(std::initializer_list<T> init) : b_(new LListBox<T>()) {\n"
        "        b_->v.assign(init.begin(), init.end());\n"
        "    }\n"
        // Para LDict::lumen_keys(): un vector ya calculado en tiempo de
        // ejecucion (las claves de un diccionario), no una lista literal
        // conocida al generar -- initializer_list no sirve aqui.
        "    explicit LList(std::vector<T> v) : b_(new LListBox<T>()) { b_->v = std::move(v); }\n"
        "    LList(const LList& o) : b_(o.b_) { ++b_->rc; }\n"
        "    LList(LList&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }\n"
        "    LList& operator=(const LList& o) {\n"
        "        if (b_ != o.b_) { rel(); b_ = o.b_; ++b_->rc; }\n"
        "        return *this;\n"
        "    }\n"
        "    LList& operator=(LList&& o) noexcept {\n"
        "        if (this != &o) { rel(); b_ = o.b_; o.b_ = nullptr; }\n"
        "        return *this;\n"
        "    }\n"
        "    ~LList() { rel(); }\n"
        "    int64_t lumen_len() const { return (int64_t)b_->v.size(); }\n"
        "    T lumen_get(int64_t i) const {\n"
        "        if (i < 0 || i >= (int64_t)b_->v.size())\n"
        "            lumen_native_fail(\"indice fuera de rango: \" + std::to_string(i) +\n"
        "                              \" (tamano \" + std::to_string(b_->v.size()) + \")\");\n"
        "        return b_->v[(size_t)i];\n"
        "    }\n"
        "    void lumen_set(int64_t i, T x) const {\n"
        "        if (i < 0 || i >= (int64_t)b_->v.size())\n"
        "            lumen_native_fail(\"indice fuera de rango: \" + std::to_string(i) +\n"
        "                              \" (tamano \" + std::to_string(b_->v.size()) + \")\");\n"
        "        b_->v[(size_t)i] = std::move(x);\n"
        "    }\n"
        "    LList lumen_add(T x) const { b_->v.push_back(std::move(x)); return *this; }\n"
        "private:\n"
        "    void rel() { if (b_ && --b_->rc == 0) delete b_; }\n"
        "    LListBox<T>* b_;\n"
        "};\n"
        "template <class T>\n"
        "LList(std::initializer_list<T>) -> LList<T>;\n";
}

std::string dict_runtime_prelude() {
    // Mismo diseño que LList (caja con refcount no atomico, semantica de
    // referencia real): un vector de pares en vez de una tabla hash de
    // verdad -- Value::Dict en value.hpp hace lo mismo por la misma razon
    // (los diccionarios tipicos de una respuesta HTTP tienen entre 3 y 10
    // claves; un recorrido lineal les gana al arbol/tabla hasta bastante
    // mas que eso). Sin `lumen_get`: leer por indice queda fuera de esta
    // fase a proposito (ver el comentario de tipo_soportado() sobre por
    // que), asi que no hace falta ni decidir que devolver en una clave
    // ausente.
    return
        "template <class V>\n"
        "struct LDictBox { long rc; std::vector<std::pair<std::string, V>> v; LDictBox() : rc(1) {} };\n"
        "template <class V>\n"
        "class LDict {\n"
        "public:\n"
        "    LDict() : b_(new LDictBox<V>()) {}\n"
        "    LDict(std::initializer_list<std::pair<std::string, V>> init) : b_(new LDictBox<V>()) {\n"
        "        for (const auto& kv : init) lumen_set(kv.first, kv.second);\n"
        "    }\n"
        "    LDict(const LDict& o) : b_(o.b_) { ++b_->rc; }\n"
        "    LDict(LDict&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }\n"
        "    LDict& operator=(const LDict& o) {\n"
        "        if (b_ != o.b_) { rel(); b_ = o.b_; ++b_->rc; }\n"
        "        return *this;\n"
        "    }\n"
        "    LDict& operator=(LDict&& o) noexcept {\n"
        "        if (this != &o) { rel(); b_ = o.b_; o.b_ = nullptr; }\n"
        "        return *this;\n"
        "    }\n"
        "    ~LDict() { rel(); }\n"
        "    bool lumen_has(const std::string& k) const {\n"
        "        for (const auto& kv : b_->v) if (kv.first == k) return true;\n"
        "        return false;\n"
        "    }\n"
        "    void lumen_set(const std::string& k, V val) const {\n"
        "        for (auto& kv : b_->v) if (kv.first == k) { kv.second = std::move(val); return; }\n"
        "        b_->v.emplace_back(k, std::move(val));\n"
        "    }\n"
        "    LList<std::string> lumen_keys() const {\n"
        "        std::vector<std::string> ks;\n"
        "        ks.reserve(b_->v.size());\n"
        "        for (const auto& kv : b_->v) ks.push_back(kv.first);\n"
        "        return LList<std::string>(std::move(ks));\n"
        "    }\n"
        // Sin equivalente Lumen (ningun `d[k]`/metodo del lenguaje resuelve
        // aqui, ver el comentario de arriba sobre por que leer por indice
        // sigue fuera): SOLO para lumen_valor_de() (route_runtime_prelude),
        // el puente a Value del valor de retorno de una ruta -- recorrer
        // TODOS los pares es una operacion bien definida (a diferencia de
        // "leer una clave que puede faltar"), asi que no reabre la
        // ambiguedad que motivo dejar fuera la lectura por clave.
        "    int64_t lumen_len() const { return (int64_t)b_->v.size(); }\n"
        "    const std::string& lumen_key_at(int64_t i) const { return b_->v[(size_t)i].first; }\n"
        "    const V& lumen_val_at(int64_t i) const { return b_->v[(size_t)i].second; }\n"
        "private:\n"
        "    void rel() { if (b_ && --b_->rc == 0) delete b_; }\n"
        "    LDictBox<V>* b_;\n"
        "};\n"
        "template <class V>\n"
        "LDict(std::initializer_list<std::pair<std::string, V>>) -> LDict<V>;\n";
}

std::string route_runtime_prelude() {
    // Mismas tres reglas que coerce() en project.cpp, reproducidas a mano
    // (ver el comentario de generate_native_route): std::stoll/std::stod
    // aceptan basura al final ("12abc" -> 12) sin comprobar cuanto
    // consumieron -- eso NO es un descuido de aqui, es replicar el mismo
    // comportamiento del interprete bit a bit, para que --native nunca
    // acepte (o rechace) un valor que bytecode habria tratado distinto.
    return
        "inline bool lumen_route_coerce_int(const std::string& t, int64_t& out) {\n"
        "    try { out = std::stoll(t); return true; } catch (...) { return false; }\n"
        "}\n"
        "inline bool lumen_route_coerce_float(const std::string& t, double& out) {\n"
        "    try { out = std::stod(t); return true; } catch (...) { return false; }\n"
        "}\n"
        "inline bool lumen_route_coerce_bool(const std::string& t, bool& out) {\n"
        "    if (t == \"true\" || t == \"1\")  { out = true;  return true; }\n"
        "    if (t == \"false\" || t == \"0\") { out = false; return true; }\n"
        "    return false;\n"
        "}\n"
        // El puente a Value para el valor de retorno de una ruta cuando ya
        // es una List<T> construida (un Ident, tipicamente) -- ver
        // Generador::valor_json(). Solo un nivel: T es siempre un escalar
        // (tipo_elemento_contenedor_soportado prohibe List<List<..>>), asi
        // que la sobrecarga de LList<T> nunca necesita recursion real, solo
        // reusar las de escalar sobre cada elemento.
        "inline Value lumen_valor_de(int64_t v)            { return Value::integer(v); }\n"
        "inline Value lumen_valor_de(double v)              { return Value::real(v); }\n"
        "inline Value lumen_valor_de(bool v)                { return Value::boolean(v); }\n"
        "inline Value lumen_valor_de(const std::string& v)  { return Value::str(v); }\n"
        "template <class T>\n"
        "Value lumen_valor_de(const LList<T>& l) {\n"
        "    Value::List out;\n"
        "    for (int64_t i = 0; i < l.lumen_len(); ++i) out.push_back(lumen_valor_de(l.lumen_get(i)));\n"
        "    return Value::list(std::move(out));\n"
        "}\n"
        "template <class V>\n"
        "Value lumen_valor_de(const LDict<V>& d) {\n"
        "    Value::Dict out;\n"
        "    for (int64_t i = 0; i < d.lumen_len(); ++i)\n"
        "        out[d.lumen_key_at(i)] = lumen_valor_de(d.lumen_val_at(i));\n"
        "    return Value::dict(std::move(out));\n"
        "}\n"
        // Fase 5: mismo piso de 1ms que clamp_sleep_ms() en project.cpp --
        // sin el, un `await sleep(0)` en un bucle podria fijar un hilo
        // entero reprogramando un temporizador de 0ms sin parar (ver el
        // comentario de clamp_sleep_ms alli).
        "inline int lumen_clamp_sleep_ms(int64_t ms) {\n"
        "    return ms < 1 ? 1 : static_cast<int>(ms);\n"
        "}\n"
        // Fase 5.5: operaciones dinamicas sobre un Json (el Value que
        // devuelve `await <modulo>.query/exec/last_id(...)`) -- misma
        // semantica EXACTA que vm.cpp (numeric_pair()/compare()/
        // Op::Add/Sub/Mul/Div/Mod/Eq/Ne/GetIndex), mismos mensajes de
        // error, mismo canal (lumen_native_fail -- atrapado por el
        // try/catch de la ruta, ver generate_native_route). Un valor de base
        // de datos no tiene un tipo fijo demostrable en tiempo de
        // compilacion (el driver puede fallar y devolver una forma
        // distinta), asi que estas decisiones se resuelven aqui, en tiempo
        // de ejecucion, exactamente como lo haria el interprete sobre el
        // mismo Value -- no una version mas permisiva ni mas estricta.
        "inline bool lumen_json_numeric_pair(const Value& a, const Value& b) {\n"
        "    return a.is_num() && b.is_num();\n"
        "}\n"
        "inline Value lumen_json_add(const Value& a, const Value& b) {\n"
        "    if (a.is_str() && b.is_str()) return Value::str(a.as_str() + b.as_str());\n"
        "    if (a.is_str() || b.is_str())\n"
        "        lumen_native_fail(std::string(\"no se puede sumar \") + a.type_name() + \" y \" +\n"
        "                          b.type_name() +\n"
        "                          \"; para concatenar usa str(): \\\"...\\\" + str(x)\");\n"
        "    if (lumen_json_numeric_pair(a, b)) {\n"
        "        if (a.is_int() && b.is_int()) return Value::integer(a.as_int() + b.as_int());\n"
        "        return Value::real(a.as_float() + b.as_float());\n"
        "    }\n"
        "    if (a.is_list() && b.is_list()) {\n"
        "        Value::List out = a.as_list();\n"
        "        for (const auto& v : b.as_list()) out.push_back(v);\n"
        "        return Value::list(std::move(out));\n"
        "    }\n"
        "    lumen_native_fail(std::string(\"no se puede sumar \") + a.type_name() + \" y \" +\n"
        "                      b.type_name());\n"
        "}\n"
        "inline Value lumen_json_arit(const Value& a, const Value& b, char op) {\n"
        "    if (!lumen_json_numeric_pair(a, b))\n"
        "        lumen_native_fail(std::string(\"operacion aritmetica entre \") + a.type_name() +\n"
        "                          \" y \" + b.type_name());\n"
        "    bool ints = a.is_int() && b.is_int();\n"
        "    if (op == '%') {\n"
        "        if (!ints) lumen_native_fail(\"'%' solo aplica a enteros\");\n"
        "        if (b.as_int() == 0) lumen_native_fail(\"modulo by zero\");\n"
        "        return Value::integer(a.as_int() % b.as_int());\n"
        "    }\n"
        "    if (op == '/') {\n"
        "        if (b.as_float() == 0) lumen_native_fail(\"division by zero\");\n"
        "        if (ints && a.as_int() % b.as_int() == 0) return Value::integer(a.as_int() / b.as_int());\n"
        "        return Value::real(a.as_float() / b.as_float());\n"
        "    }\n"
        "    if (ints) {\n"
        "        long long x = a.as_int(), y = b.as_int();\n"
        "        return Value::integer(op == '-' ? x - y : x * y);\n"
        "    }\n"
        "    double x = a.as_float(), y = b.as_float();\n"
        "    return Value::real(op == '-' ? x - y : x * y);\n"
        "}\n"
        "inline int lumen_json_compare(const Value& a, const Value& b) {\n"
        "    if (lumen_json_numeric_pair(a, b)) {\n"
        "        if (a.is_int() && b.is_int()) {\n"
        "            long long x = a.as_int(), y = b.as_int();\n"
        "            return x < y ? -1 : (x > y ? 1 : 0);\n"
        "        }\n"
        "        double x = a.as_float(), y = b.as_float();\n"
        "        return x < y ? -1 : (x > y ? 1 : 0);\n"
        "    }\n"
        "    if (a.is_str() && b.is_str()) {\n"
        "        int c = a.as_str().compare(b.as_str());\n"
        "        return c < 0 ? -1 : (c > 0 ? 1 : 0);\n"
        "    }\n"
        "    lumen_native_fail(std::string(\"no se pueden comparar \") + a.type_name() + \" y \" +\n"
        "                      b.type_name());\n"
        "}\n"
        "inline bool lumen_json_lt(const Value& a, const Value& b) { return lumen_json_compare(a, b) < 0; }\n"
        "inline bool lumen_json_le(const Value& a, const Value& b) { return lumen_json_compare(a, b) <= 0; }\n"
        "inline bool lumen_json_gt(const Value& a, const Value& b) { return lumen_json_compare(a, b) > 0; }\n"
        "inline bool lumen_json_ge(const Value& a, const Value& b) { return lumen_json_compare(a, b) >= 0; }\n"
        "inline bool lumen_json_eq(const Value& a, const Value& b) { return a.equals(b); }\n"
        "inline bool lumen_json_ne(const Value& a, const Value& b) { return !a.equals(b); }\n"
        // GetIndex (vm.cpp): cual de las dos formas (List/Dict) decide el
        // OBJETO en tiempo de ejecucion, no el indice -- el indice solo
        // dice si esta ruta espera un acceso de List (int) o de Dict
        // (string), ver Comprobador::tipo_provable caso Index.
        "inline Value lumen_json_index_int(const Value& obj, int64_t idx) {\n"
        "    if (obj.is_list()) {\n"
        "        auto& l = obj.as_list();\n"
        "        if (idx < 0 || idx >= (int64_t)l.size())\n"
        "            lumen_native_fail(\"indice fuera de rango: \" + std::to_string(idx) +\n"
        "                              \" (tamano \" + std::to_string(l.size()) + \")\");\n"
        "        return l[(size_t)idx];\n"
        "    }\n"
        "    if (obj.is_dict()) lumen_native_fail(\"la clave de un Dict tiene que ser string\");\n"
        "    lumen_native_fail(std::string(\"no se puede indexar \") + obj.type_name());\n"
        "}\n"
        "inline Value lumen_json_index_str(const Value& obj, const std::string& key) {\n"
        "    if (obj.is_dict()) {\n"
        "        auto& d = obj.as_dict();\n"
        "        auto it = d.find(key);\n"
        "        return it == d.end() ? Value::null() : it->second;\n"
        "    }\n"
        "    if (obj.is_list()) lumen_native_fail(\"el indice de una List tiene que ser int\");\n"
        "    lumen_native_fail(std::string(\"no se puede indexar \") + obj.type_name());\n"
        "}\n"
        // len()/int() sobre un Json -- mismas reglas que fn_len/fn_int
        // (natives.cpp).
        "inline int64_t lumen_json_len(const Value& v) {\n"
        "    if (v.is_str())  return (int64_t)v.as_str().size();\n"
        "    if (v.is_list()) return (int64_t)v.as_list().size();\n"
        "    if (v.is_dict()) return (int64_t)v.as_dict().size();\n"
        "    lumen_native_fail(std::string(\"len() no aplica a \") + v.type_name());\n"
        "}\n"
        "inline int64_t lumen_json_as_int(const Value& v) {\n"
        "    if (v.is_int())   return v.as_int();\n"
        "    if (v.is_float()) return (int64_t)v.as_float();\n"
        "    if (v.is_bool())  return v.as_bool() ? 1 : 0;\n"
        "    if (v.is_str())   return lumen_str_to_int(v.as_str());\n"
        "    lumen_native_fail(std::string(\"int() no aplica a \") + v.type_name());\n"
        "}\n"
        // Fase 5.10: List<Json>.add(x) -- call_method() (natives.cpp,
        // rama recv.is_list()) hace exactamente esto: push_back en sitio,
        // devuelve el receptor. List<Json> se representa como Value (no
        // LList<Value>, ver tipo_cpp()), asi que .lumen_add() (el metodo
        // de LList<T>) no existe sobre ella -- este es su equivalente.
        "inline Value& lumen_json_list_add(Value& lista, Value item) {\n"
        "    lista.as_list().push_back(std::move(item));\n"
        "    return lista;\n"
        "}\n"
        // Los parametros de `await <modulo>.query/exec(sql, ...)` -- NUNCA
        // se construyen con `std::vector<Value>{...}` directo en la
        // llamada a await_db() que se hace co_await: GCC 13.3 da un
        // internal compiler error real ahi (build_special_member_call) con
        // un braced-init-list de Value como argumento inline de una
        // llamada co_await-eada -- confirmado con un reproductor minimo,
        // no un error de este generador. Una funcion normal que devuelve
        // el mismo vector SI compila limpio (ver Generador::expr, caso
        // Await).
        "template <class... Args>\n"
        "inline std::vector<Value> lumen_db_params(Args&&... args) {\n"
        "    std::vector<Value> v;\n"
        "    v.reserve(sizeof...(args));\n"
        "    (v.push_back(std::forward<Args>(args)), ...);\n"
        "    return v;\n"
        "}\n";
}

} // namespace lumen_script
