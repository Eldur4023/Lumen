#include <lumen_script/db.hpp>
#include <lumen_script/crypto.hpp>

#include <mysql.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace lumen_script {

// MySQL 8 retiro `my_bool` en favor de `bool`; MariaDB y MySQL 5 lo mantienen.
#if !defined(MARIADB_VERSION_ID) && MYSQL_VERSION_ID >= 80000
using lumen_script_my_bool = bool;
#else
using lumen_script_my_bool = my_bool;
#endif

namespace {

// Driver de MySQL/MariaDB sobre libmysqlclient.
//
// Todo lo que lleva parametros va por sentencia preparada: viajan por bind y
// nunca concatenados, que es lo que hace imposible la inyeccion de SQL desde
// Lumen Script.  Lo que NO lleva parametros se manda directo, porque el protocolo
// preparado de MySQL no admite abrir una transaccion.
class MysqlDriver : public DbDriver {
public:
    const char* name() const override { return "mysql"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        // libmysqlclient hay que inicializarla UNA vez y desde un solo hilo,
        // antes de que ningun otro la toque.  Sin esto, la inicializacion
        // implicita de mysql_init() la disparaba el primer worker del pool que
        // recibiera trabajo, mientras otro ya estaba dentro de la biblioteca:
        // ThreadSanitizer lo caza como carrera sobre el mutex que monta
        // mysql_server_init.  configure() corre antes de pool->start(), asi que
        // aqui todavia no hay workers.
        static std::once_flag una_vez;
        static bool arrancada = false;
        std::call_once(una_vez, [] { arrancada = mysql_library_init(0, nullptr, nullptr) == 0; });
        if (!arrancada) {
            error = "mysql: no se puede inicializar libmysqlclient";
            return false;
        }

        auto get = [&](const char* k, const char* def) {
            auto it = options.find(k);
            return it == options.end() ? std::string(def) : it->second;
        };
        host_ = get("host", "localhost");
        port_ = static_cast<unsigned>(std::strtoul(get("port", "3306").c_str(), nullptr, 10));
        user_ = get("user", "");
        pass_ = get("password", "");
        db_   = get("database", "");

        if (db_.empty()) {
            error = "mysql: falta 'database' en el bloque de configuracion";
            return false;
        }

        auto p = options.find("pool");
        if (p != options.end()) {
            long n = std::strtol(p->second.c_str(), nullptr, 10);
            if (n < 1 || n > 64) {
                error = "mysql: 'pool' tiene que estar entre 1 y 64";
                return false;
            }
            set_pool_size(static_cast<size_t>(n));
        }
        conns_.assign(pool_size(), nullptr);
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "mysql: worker fuera de rango"; return false; }
        if (conns_[worker] && mysql_ping(conns_[worker]) != 0) {
            mysql_close(conns_[worker]);
            conns_[worker] = nullptr;
        }
        if (conns_[worker]) return true;

        MYSQL* c = mysql_init(nullptr);
        if (!c) { error = "mysql: sin memoria"; return false; }

        // Reconexion automatica desactivada: reabrir en silencio a mitad de una
        // transaccion la perderia sin avisar.  open() ya reabre entre consultas.
        // MySQL 8.0.34 retiro la opcion y ese es ya el comportamiento.
#if defined(MYSQL_OPT_RECONNECT)
        lumen_script_my_bool reconnect = 0;
        mysql_options(c, MYSQL_OPT_RECONNECT, &reconnect);
#endif

        if (!mysql_real_connect(c, host_.c_str(), user_.c_str(), pass_.c_str(),
                                db_.c_str(), port_, nullptr, 0)) {
            error = std::string("mysql: no se puede conectar: ") + mysql_error(c);
            mysql_close(c);
            return false;
        }
        mysql_set_character_set(c, "utf8mb4");
        conns_[worker] = c;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        // Todo esto es local a la llamada, y tiene que serlo: el driver es uno
        // solo y lo comparten los N workers del pool.  Con los buffers de bind
        // guardados en el objeto, dos consultas simultaneas se pisaban los
        // punteros que libmysqlclient todavia estaba usando.
        MYSQL_STMT* stmt = nullptr;
        std::vector<MYSQL_BIND>  binds;
        std::vector<std::string> store;
        std::unique_ptr<lumen_script_my_bool[]> nulls_in;
        std::vector<unsigned long>      lens_in;
        if (!prepare(worker, sql, args, &stmt, binds, store, nulls_in, lens_in, error))
            return false;

        if (mysql_stmt_execute(stmt) != 0) {
            error = std::string("mysql: ") + mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return false;
        }

        MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
        if (!meta) {                       // no devolvia filas
            mysql_stmt_close(stmt);
            out = Value::list();
            return true;
        }

        // `max_length` vale CERO mientras no se pida expresamente y no se traiga
        // el resultado al cliente.  Sin estas dos llamadas, todos los buffers se
        // quedaban en el tamano de reserva y cualquier columna mas larga se
        // truncaba EN SILENCIO: un TEXT de 4 KB llegaba con 1024 bytes, cortado
        // ademas a mitad de caracter si era UTF-8.
        lumen_script_my_bool si = 1;
        mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &si);
        if (mysql_stmt_store_result(stmt) != 0) {
            error = std::string("mysql: ") + mysql_stmt_error(stmt);
            mysql_free_result(meta);
            mysql_stmt_close(stmt);
            return false;
        }

        unsigned cols = mysql_num_fields(meta);
        MYSQL_FIELD* fields = mysql_fetch_fields(meta);

        // Buffers de salida: se pide todo como texto y se convierte despues,
        // que evita una tabla de tipos por cada variante de entero de MySQL.
        std::vector<std::vector<char>> bufs(cols);
        std::vector<unsigned long>     lens(cols, 0);
        // Arrays y no vector<bool>: esa especializacion empaqueta bits y no
        // permite tomar la direccion de un elemento, que es lo que pide la API.
        auto nulls = std::make_unique<lumen_script_my_bool[]>(cols);
        auto errs  = std::make_unique<lumen_script_my_bool[]>(cols);
        std::vector<MYSQL_BIND>        obind(cols);
        std::memset(obind.data(), 0, sizeof(MYSQL_BIND) * cols);

        for (unsigned i = 0; i < cols; ++i) {
            size_t cap = fields[i].max_length ? fields[i].max_length + 1 : 1024;
            bufs[i].assign(cap, 0);
            obind[i].buffer_type   = MYSQL_TYPE_STRING;
            obind[i].buffer        = bufs[i].data();
            obind[i].buffer_length = static_cast<unsigned long>(cap);
            obind[i].length        = &lens[i];
            obind[i].is_null       = &nulls[i];
            obind[i].error         = &errs[i];
        }
        mysql_stmt_bind_result(stmt, obind.data());

        Value::List rows;
        for (;;) {
            int rc = mysql_stmt_fetch(stmt);
            if (rc == MYSQL_NO_DATA) break;
            // MYSQL_DATA_TRUNCATED se colaba por aqui como si fuera una fila
            // buena.  Con los buffers ya bien dimensionados no deberia ocurrir;
            // si ocurre, es un fallo y no un dato a medias.
            if (rc == 1 || rc == MYSQL_DATA_TRUNCATED) {
                error = (rc == MYSQL_DATA_TRUNCATED)
                      ? std::string("mysql: fila truncada al leerla")
                      : std::string("mysql: ") + mysql_stmt_error(stmt);
                mysql_free_result(meta);
                mysql_stmt_close(stmt);
                return false;
            }
            Value::Dict row;
            for (unsigned i = 0; i < cols; ++i) {
                if (nulls[i]) { row[fields[i].name] = Value::null(); continue; }
                std::string text(bufs[i].data(), std::min<size_t>(lens[i], bufs[i].size()));
                row[fields[i].name] = typed(fields[i].type, fields[i].flags,
                                            fields[i].charsetnr, text);
            }
            rows.push_back(Value::dict(std::move(row)));
        }

        mysql_free_result(meta);
        mysql_stmt_close(stmt);
        out = Value::list(std::move(rows));
        return true;
    }

    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        // Sin parametros la sentencia va directa, sin pasar por el protocolo de
        // sentencias preparadas.
        //
        // No es una optimizacion, es que HACE FALTA: MySQL no admite ahi ni
        // BEGIN ni START TRANSACTION —error 1295—, asi que por el camino
        // preparado la transaccion no llegaba a abrirse nunca.  El UPDATE se
        // autocommiteaba y el ROLLBACK devolvia exito sin nada que deshacer.
        //
        // Y no abre ningun agujero: lo que hace imposible la inyeccion es que
        // los PARAMETROS viajen por bind, y aqui no hay ninguno que enlazar.
        if (args.empty()) {
            MYSQL* c = conns_[worker];
            if (mysql_real_query(c, sql.c_str(),
                                 static_cast<unsigned long>(sql.size())) != 0) {
                error = std::string("mysql: ") + mysql_error(c);
                return false;
            }
            // Un exec() sobre algo que devuelve filas dejaria la conexion a
            // medias y la siguiente consulta fallaria sin motivo aparente.
            if (MYSQL_RES* r = mysql_store_result(c)) mysql_free_result(r);
            affected = static_cast<long long>(mysql_affected_rows(c));
            return true;
        }

        // Todo esto es local a la llamada, y tiene que serlo: el driver es uno
        // solo y lo comparten los N workers del pool.  Con los buffers de bind
        // guardados en el objeto, dos consultas simultaneas se pisaban los
        // punteros que libmysqlclient todavia estaba usando.
        MYSQL_STMT* stmt = nullptr;
        std::vector<MYSQL_BIND>  binds;
        std::vector<std::string> store;
        std::unique_ptr<lumen_script_my_bool[]> nulls_in;
        std::vector<unsigned long>      lens_in;
        if (!prepare(worker, sql, args, &stmt, binds, store, nulls_in, lens_in, error))
            return false;

        if (mysql_stmt_execute(stmt) != 0) {
            error = std::string("mysql: ") + mysql_stmt_error(stmt);
            mysql_stmt_close(stmt);
            return false;
        }
        affected = static_cast<long long>(mysql_stmt_affected_rows(stmt));
        mysql_stmt_close(stmt);
        return true;
    }

    bool last_insert_id(size_t worker, long long& id, std::string& error) override {
        if (worker >= conns_.size() || !conns_[worker]) {
            error = "mysql: sin conexion";
            return false;
        }
        id = static_cast<long long>(mysql_insert_id(conns_[worker]));
        return true;
    }

    ~MysqlDriver() override {
        for (auto* c : conns_) if (c) mysql_close(c);
    }

private:
    std::string          host_, user_, pass_, db_;
    unsigned             port_ = 3306;
    std::vector<MYSQL*>  conns_;

    bool prepare(size_t worker, const std::string& sql,
                 const std::vector<Value>& args,
                 MYSQL_STMT** out, std::vector<MYSQL_BIND>& binds,
                 std::vector<std::string>& store,
                 std::unique_ptr<lumen_script_my_bool[]>& nulls,
                 std::vector<unsigned long>& lens, std::string& error) {
        MYSQL* c = conns_[worker];
        *out = mysql_stmt_init(c);
        if (!*out) { error = "mysql: sin memoria"; return false; }

        if (mysql_stmt_prepare(*out, sql.c_str(),
                               static_cast<unsigned long>(sql.size())) != 0) {
            error = std::string("mysql: ") + mysql_stmt_error(*out);
            mysql_stmt_close(*out);
            *out = nullptr;
            return false;
        }

        unsigned expected = mysql_stmt_param_count(*out);
        if (expected != args.size()) {
            error = "mysql: la consulta tiene " + std::to_string(expected) +
                    " parametro(s) y se pasaron " + std::to_string(args.size());
            mysql_stmt_close(*out);
            *out = nullptr;
            return false;
        }
        if (args.empty()) return true;

        // Todo se manda como texto: MySQL lo convierte al tipo de la columna, y
        // asi no hace falta una rama por cada tipo numerico.
        store.reserve(args.size());
        for (const auto& v : args)
            store.push_back(v.is_null() ? std::string()
                          : v.is_bool() ? (v.as_bool() ? "1" : "0")
                                        : v.to_string());

        binds.assign(args.size(), MYSQL_BIND{});
        std::memset(binds.data(), 0, sizeof(MYSQL_BIND) * binds.size());
        nulls = std::make_unique<lumen_script_my_bool[]>(args.size());
        lens.assign(args.size(), 0);

        for (size_t i = 0; i < args.size(); ++i) {
            nulls[i] = args[i].is_null() ? 1 : 0;
            lens[i]  = static_cast<unsigned long>(store[i].size());
            binds[i].buffer_type   = MYSQL_TYPE_STRING;
            binds[i].buffer        = store[i].data();
            binds[i].buffer_length = lens[i];
            binds[i].length        = &lens[i];
            binds[i].is_null       = &nulls[i];
        }
        if (mysql_stmt_bind_param(*out, binds.data()) != 0) {
            error = std::string("mysql: al enlazar parametros: ") + mysql_stmt_error(*out);
            mysql_stmt_close(*out);
            *out = nullptr;
            return false;
        }
        return true;
    }

    // `charsetnr == 63` es el juego binary: eso distingue un BLOB de un TEXT,
    // que en MySQL comparten tipo y solo se diferencian en la codificacion.
    static constexpr unsigned kBinario = 63;

    static Value typed(enum_field_types t, unsigned flags, unsigned charset,
                       const std::string& text) {
        // Un BLOB no es texto: son bytes cualesquiera.  Devolverlos como cadena
        // dejaba la respuesta sin ser UTF-8 valido, y entonces el fallo no es de
        // la peticion sino del cliente que la recibe.  Base64 es como se mete un
        // binario en un JSON.
        if (charset == kBinario &&
            (t == MYSQL_TYPE_BLOB      || t == MYSQL_TYPE_TINY_BLOB ||
             t == MYSQL_TYPE_MEDIUM_BLOB || t == MYSQL_TYPE_LONG_BLOB ||
             t == MYSQL_TYPE_STRING    || t == MYSQL_TYPE_VAR_STRING))
            return Value::str(crypto::base64_encode(text));

        switch (t) {
            case MYSQL_TYPE_TINY:  case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:  case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24: case MYSQL_TYPE_YEAR: {
                // Un BIGINT UNSIGNED llega hasta 18446744073709551615, que no
                // cabe en el entero de Lumen Script.  strtoll lo dejaria clavado en
                // INT64_MAX sin avisar, asi que por encima de ese tope se cae a
                // decimal, que es lo mismo que hace el parser de JSON y lo que
                // hace JavaScript.  Por debajo sigue siendo exacto.
                if (flags & UNSIGNED_FLAG) {
                    unsigned long long u = std::strtoull(text.c_str(), nullptr, 10);
                    if (u > static_cast<unsigned long long>(INT64_MAX))
                        return Value::real(static_cast<double>(u));
                    return Value::integer(static_cast<long long>(u));
                }
                return Value::integer(std::strtoll(text.c_str(), nullptr, 10));
            }
            case MYSQL_TYPE_FLOAT: case MYSQL_TYPE_DOUBLE:
            case MYSQL_TYPE_DECIMAL: case MYSQL_TYPE_NEWDECIMAL:
                return Value::real(std::strtod(text.c_str(), nullptr));
            default:
                return Value::str(text);
        }
    }
};

} // namespace

std::unique_ptr<DbDriver> make_mysql_driver() {
    return std::make_unique<MysqlDriver>();
}

} // namespace lumen_script
