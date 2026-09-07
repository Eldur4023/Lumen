#include <lumen_script/db.hpp>

#include <libpq-fe.h>

#include <cctype>
#include <cstdlib>
#include <string>

namespace lumen_script {

namespace {

// ─── Parameter placeholders ──────────────────────────────────────────────────
//
// In Lumen Script the placeholder is `?`, sqlite's and mysql's, so the SAME
// query works on all three engines.  Postgres is the only one that numbers
// them, so the translation lives here: the driver is the one that knows its
// dialect, and that way switching engines does not force rewriting every query.
//
// The rules, each with its reason:
//
//   • It only translates if the query carries parameters.  Without them there
//     is nothing to substitute, and a bare `?` in postgres is the JSONB
//     operator —`data ? 'key'`— which must be left alone.
//   • A query written with $1 has no `?` at all, so it comes out untouched.
//     Code written before this keeps working without changes.
//   • What is inside a string, a quoted identifier, a comment or a $tag$ block
//     is not a placeholder.
//   • `??` is a literal `?`, so the JSONB operator can be used in a query that
//     also carries parameters.
//
// Returns false —leaving the reason in `error`— if the query mixes both styles
// or if the placeholders do not match the arguments.
bool traducir_marcadores(const std::string& sql, size_t nargs,
                         std::string& out, std::string& error) {
    // With no parameters there is nothing to substitute, and any `?` in the
    // query is the JSONB operator.  It leaves before even looking.
    if (nargs == 0) { out = sql; return true; }

    auto ident = [](unsigned char c) { return std::isalnum(c) || c == '_'; };

    std::string res;
    res.reserve(sql.size() + 8);
    size_t i = 0;
    int    n = 0;              // marcadores `?` encontrados
    bool   numerado = false;   // a $1, $2... was seen

    while (i < sql.size()) {
        const char c = sql[i];

        // Line comment.
        if (c == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            while (i < sql.size() && sql[i] != '\n') res += sql[i++];
            continue;
        }

        // Block comment.  In postgres they nest, so they are counted.
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

        // String.  Inside, '' is an escaped quote; if the string is preceded by
        // an E, the backslash escapes too.
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

        // Quoted identifier.
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
            // $1, $2...: the query already comes numbered.
            if (i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1]))) {
                numerado = true;
                res += sql[i++];
                continue;
            }
            // $tag$ ... $tag$: nothing inside is a placeholder.
            size_t j = i + 1;
            while (j < sql.size() && ident(static_cast<unsigned char>(sql[j]))) ++j;
            if (j < sql.size() && sql[j] == '$') {
                const std::string tag = sql.substr(i, j - i + 1);
                const size_t fin = sql.find(tag, j + 1);
                const size_t up_to = (fin == std::string::npos) ? sql.size() : fin + tag.size();
                res.append(sql, i, up_to - i);
                i = up_to;
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

    // Not a single `?`: the query is written postgres style and is sent as is,
    // without even touching any `??` it might carry.
    if (n == 0) { out = sql; return true; }

    if (numerado) {
        error = "postgres: the query mixes '?' and '$1' placeholders; use only one "
                "of the two styles";
        return false;
    }
    if (static_cast<size_t>(n) != nargs) {
        error = "postgres: the query has " + std::to_string(n) +
                " '?' placeholder(s) but " + std::to_string(nargs) +
                " argument(s) were passed";
        return false;
    }
    out = std::move(res);
    return true;
}

// PostgreSQL driver on top of libpq.
//
// libpq has an asynchronous API, but its waiting model does not fit someone
// else's event loop without reimplementing the connection loop.  The blocking
// one is used inside the pool: it is simpler and the effect for whoever writes
// Lumen Script is the same, because the handler suspends either way.
class PostgresDriver : public DbDriver {
public:
    const char* name() const override { return "postgres"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        auto url = options.find("url");
        if (url != options.end() && !url->second.empty()) {
            conninfo_ = url->second;
        } else {
            // Without a url, it is composed from the separate pieces.
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
                error = "postgres: missing 'url' or 'database' in the configuration block";
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
                error = "postgres: 'pool' must be between 1 and 64";
                return false;
            }
            set_pool_size(static_cast<size_t>(n));
        }
        conns_.assign(pool_size(), nullptr);
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "postgres: worker out of range"; return false; }

        // A dropped connection reopens itself on the next query, without the
        // .lum having to know.
        if (conns_[worker] && PQstatus(conns_[worker]) != CONNECTION_OK) {
            PQfinish(conns_[worker]);
            conns_[worker] = nullptr;
        }
        if (conns_[worker]) return true;

        PGconn* c = PQconnectdb(conninfo_.c_str());
        if (!c || PQstatus(c) != CONNECTION_OK) {
            error = std::string("postgres: cannot connect: ") +
                    (c ? PQerrorMessage(c) : "out of memory");
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

    // The parameters go through PQexecParams, never concatenated: that is what
    // makes SQL injection impossible from Lumen Script.  They are sent as text
    // and the server converts them to the column type.
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
        // store may have reallocated: the pointers are rebuilt.
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

    // OIDs of the types that deserve not to arrive as a string.
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
