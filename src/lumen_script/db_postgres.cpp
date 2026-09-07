#include <lumen_script/db.hpp>

#include <libpq-fe.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace lumen_script {

namespace {

// ─── Marcadores de parametro ─────────────────────────────────────────────────
//
// En Lumen Script el marcador es `?`, el de sqlite y mysql, para que la MISMA consulta
// valga en los tres motores.  Postgres es el unico que los numera, asi que la
// traduccion vive aqui: el driver es quien conoce su dialecto, y asi cambiar de
// motor no obliga a reescribir todas las consultas de la aplicacion.
//
// Reglas, y cada una tiene su motivo:
//
//   • Solo se traduce si la consulta lleva parametros.  Sin ellos no hay nada
//     que sustituir, y un `?` suelto en postgres es el operador de JSONB
//     —`datos ? 'clave'`—, que hay que dejar en paz.
//   • Una consulta escrita con $1 no tiene ningun `?`, asi que sale intacta.
//     El codigo anterior a esto sigue funcionando sin tocarlo.
//   • Lo que hay dentro de una cadena, de un identificador entrecomillado, de
//     un comentario o de un bloque $etiqueta$ no es un marcador.
//   • `??` es un `?` literal, para poder usar el operador de JSONB en una
//     consulta que ademas lleva parametros.
//
// Devuelve false —y deja el motivo en `error`— si la consulta mezcla los dos
// estilos o si los marcadores no cuadran con los argumentos.
bool traducir_marcadores(const std::string& sql, size_t nargs,
                         std::string& out, std::string& error) {
    // Sin parametros no hay nada que sustituir, y cualquier `?` de la consulta
    // es el operador de JSONB.  Se sale antes de mirar siquiera.
    if (nargs == 0) { out = sql; return true; }

    auto ident = [](unsigned char c) { return std::isalnum(c) || c == '_'; };

    std::string res;
    res.reserve(sql.size() + 8);
    size_t i = 0;
    int    n = 0;              // marcadores `?` encontrados
    bool   numerado = false;   // se vio un $1, $2...

    while (i < sql.size()) {
        const char c = sql[i];

        // Comentario de linea.
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') res += sql[i++];
            continue;
        }

        // Comentario de bloque.  En postgres anidan, asi que se cuenta.
        if (c == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            int prof = 0;
            while (i < sql.size()) {
                if (sql[i] == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
                    ++prof; res += sql[i++]; res += sql[i++]; continue;
                }
                if (sql[i] == '*' && i + 1 < sql.size() && sql[i + 1] == '/') {
                    --prof; res += sql[i++]; res += sql[i++];
                    if (prof == 0) break;
                    continue;
                }
                res += sql[i++];
            }
            continue;
        }

        // Cadena.  Dentro, '' es una comilla escapada; si la cadena viene
        // precedida de E, la barra tambien escapa.
        if (c == '\'') {
            const bool con_barra = !res.empty() && (res.back() == 'E' || res.back() == 'e');
            res += sql[i++];
            while (i < sql.size()) {
                if (con_barra && sql[i] == '\\' && i + 1 < sql.size()) {
                    res += sql[i++]; res += sql[i++]; continue;
                }
                if (sql[i] == '\'') {
                    if (i + 1 < sql.size() && sql[i + 1] == '\'') {
                        res += sql[i++]; res += sql[i++]; continue;
                    }
                    res += sql[i++];
                    break;
                }
                res += sql[i++];
            }
            continue;
        }

        // Identificador entrecomillado.
        if (c == '"') {
            res += sql[i++];
            while (i < sql.size()) {
                if (sql[i] == '"') {
                    if (i + 1 < sql.size() && sql[i + 1] == '"') {
                        res += sql[i++]; res += sql[i++]; continue;
                    }
                    res += sql[i++];
                    break;
                }
                res += sql[i++];
            }
            continue;
        }

        if (c == '$') {
            // $1, $2...: la consulta ya viene numerada.
            if (i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
                numerado = true;
                res += sql[i++];
                continue;
            }
            // $etiqueta$ ... $etiqueta$: nada de dentro es un marcador.
            size_t j = i + 1;
            while (j < sql.size() && ident(static_cast<unsigned char>(sql[j]))) ++j;
            if (j < sql.size() && sql[j] == '$') {
                const std::string tag = sql.substr(i, j - i + 1);
                const size_t fin = sql.find(tag, j + 1);
                const size_t hasta = (fin == std::string::npos) ? sql.size() : fin + tag.size();
                res.append(sql, i, hasta - i);
                i = hasta;
                continue;
            }
            res += sql[i++];
            continue;
        }

        if (c == '?') {
            if (i + 1 < sql.size() && sql[i + 1] == '?') {   // `??` -> `?`
                res += '?';
                i += 2;
                continue;
            }
            res += '$';
            res += std::to_string(++n);
            ++i;
            continue;
        }

        res += sql[i++];
    }

    // Ni un solo `?`: la consulta esta escrita al estilo de postgres y se manda
    // tal cual, sin tocar siquiera los `??` que pudiera llevar.
    if (n == 0) { out = sql; return true; }

    if (numerado) {
        error = "postgres: la consulta mezcla marcadores '?' y '$1'; usa solo uno "
                "de los dos estilos";
        return false;
    }
    if (static_cast<size_t>(n) != nargs) {
        error = "postgres: la consulta tiene " + std::to_string(n) +
                " marcador(es) '?' y se pasaron " + std::to_string(nargs) +
                " argumento(s)";
        return false;
    }
    out = std::move(res);
    return true;
}

// Driver de PostgreSQL sobre libpq.
//
// libpq tiene API asincrona, pero su modelo de espera no encaja con un event
// loop ajeno sin reimplementar el bucle de conexion.  Se usa la bloqueante
// dentro del pool: es mas simple y el efecto para quien escribe Lumen Script es el
// mismo, porque el handler se suspende igual.
class PostgresDriver : public DbDriver {
public:
    const char* name() const override { return "postgres"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        auto url = options.find("url");
        if (url != options.end() && !url->second.empty()) {
            conninfo_ = url->second;
        } else {
            // Sin url, se compone a partir de las piezas sueltas.
            auto get = [&](const char* k, const char* def) {
                auto it = options.find(k);
                return it == options.end() ? std::string(def) : it->second;
            };
            std::string host = get("host", "localhost");
            std::string port = get("port", "5432");
            std::string db   = get("database", "");
            std::string user = get("user", "");
            std::string pass = get("password", "");
            if (db.empty()) {
                error = "postgres: falta 'url' o 'database' en el bloque de configuracion";
                return false;
            }
            conninfo_ = "host=" + host + " port=" + port + " dbname=" + db;
            if (!user.empty()) conninfo_ += " user=" + user;
            if (!pass.empty()) conninfo_ += " password=" + pass;
        }

        auto p = options.find("pool");
        if (p != options.end()) {
            long n = std::strtol(p->second.c_str(), nullptr, 10);
            if (n < 1 || n > 64) {
                error = "postgres: 'pool' tiene que estar entre 1 y 64";
                return false;
            }
            set_pool_size(static_cast<size_t>(n));
        }
        conns_.assign(pool_size(), nullptr);
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "postgres: worker fuera de rango"; return false; }

        // Una conexion caida se reabre sola en la siguiente consulta, sin que
        // el .lum tenga que enterarse.
        if (conns_[worker] && PQstatus(conns_[worker]) != CONNECTION_OK) {
            PQfinish(conns_[worker]);
            conns_[worker] = nullptr;
        }
        if (conns_[worker]) return true;

        PGconn* c = PQconnectdb(conninfo_.c_str());
        if (!c || PQstatus(c) != CONNECTION_OK) {
            error = std::string("postgres: no se puede conectar: ") +
                    (c ? PQerrorMessage(c) : "sin memoria");
            if (c) PQfinish(c);
            return false;
        }
        conns_[worker] = c;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        PGresult* res = run(worker, sql, args, error);
        if (!res) return false;

        int rows = PQntuples(res), cols = PQnfields(res);
        Value::List list;
        list.reserve(static_cast<size_t>(rows));

        for (int r = 0; r < rows; ++r) {
            Value::Dict row;
            for (int c = 0; c < cols; ++c) {
                const char* col = PQfname(res, c);
                row[col ? col : std::to_string(c)] =
                    PQgetisnull(res, r, c) ? Value::null()
                                           : typed(PQftype(res, c), PQgetvalue(res, r, c));
            }
            list.push_back(Value::dict(std::move(row)));
        }
        PQclear(res);
        out = Value::list(std::move(list));
        return true;
    }

    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        PGresult* res = run(worker, sql, args, error);
        if (!res) return false;
        const char* n = PQcmdTuples(res);
        affected = (n && *n) ? std::strtoll(n, nullptr, 10) : 0;
        PQclear(res);
        return true;
    }

    ~PostgresDriver() override {
        for (auto* c : conns_) if (c) PQfinish(c);
    }

private:
    std::string          conninfo_;
    std::vector<PGconn*> conns_;

    // Los parametros van por PQexecParams, nunca concatenados: es lo que hace
    // imposible la inyeccion de SQL desde Lumen Script.  Se mandan como texto y el
    // servidor los convierte al tipo de la columna.
    PGresult* run(size_t worker, const std::string& sql, const std::vector<Value>& args,
                  std::string& error) {
        PGconn* c = conns_[worker];

        std::string sql_pg;
        if (!traducir_marcadores(sql, args.size(), sql_pg, error)) return nullptr;

        std::vector<std::string> store;
        std::vector<const char*> ptrs;
        store.reserve(args.size());
        ptrs.reserve(args.size());
        for (const auto& v : args) {
            if (v.is_null()) { store.emplace_back(); ptrs.push_back(nullptr); continue; }
            store.push_back(v.is_bool() ? (v.as_bool() ? "true" : "false") : v.to_string());
            ptrs.push_back(store.back().c_str());
        }
        // store puede haber realojado: se rehacen los punteros.
        for (size_t i = 0, j = 0; i < args.size(); ++i)
            if (!args[i].is_null()) { ptrs[i] = store[i].c_str(); ++j; }

        PGresult* res = PQexecParams(c, sql_pg.c_str(), static_cast<int>(args.size()),
                                     nullptr, ptrs.data(), nullptr, nullptr, 0);
        auto status = res ? PQresultStatus(res) : PGRES_FATAL_ERROR;
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
            error = std::string("postgres: ") +
                    (res ? PQresultErrorMessage(res) : PQerrorMessage(c));
            if (res) PQclear(res);
            return nullptr;
        }
        return res;
    }

    // OIDs de los tipos que merecen no llegar como cadena.
    static Value typed(unsigned oid, const char* text) {
        switch (oid) {
            case 16:   return Value::boolean(text && (*text == 't' || *text == 'T'));
            case 20: case 21: case 23:            // int8, int2, int4
                return Value::integer(std::strtoll(text, nullptr, 10));
            case 700: case 701: case 1700:        // float4, float8, numeric
                return Value::real(std::strtod(text, nullptr));
            default:
                return Value::str(text ? text : "");
        }
    }
};

} // namespace

std::unique_ptr<DbDriver> make_postgres_driver() {
    return std::make_unique<PostgresDriver>();
}

} // namespace lumen_script
