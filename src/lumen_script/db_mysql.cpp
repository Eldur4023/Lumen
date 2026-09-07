#include <lumen_script/db.hpp>
#include <lumen_script/crypto.hpp>

#include <mysql.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

namespace lumen_script {

// MySQL 8 dropped `my_bool` in favour of `bool`; MariaDB and MySQL 5 keep it.
#if !defined(MARIADB_VERSION_ID) && MYSQL_VERSION_ID >= 80000
using lumen_script_my_bool = bool;
#else
using lumen_script_my_bool = my_bool;
#endif

namespace {

// MySQL/MariaDB driver on top of libmysqlclient.
//
// Everything carrying parameters goes through a prepared statement: they
// travel by bind and never concatenated, which is what makes SQL injection
// impossible from Lumen Script.  What does NOT carry parameters is sent
// directly, because MySQL's prepared protocol cannot open a transaction.
class MysqlDriver : public DbDriver {
public:
    const char* name() const override { return "mysql"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        // libmysqlclient has to be initialized ONCE and from a single thread,
        // before anyone else touches it.  Without this, the implicit
        // initialization of mysql_init() was triggered by the first pool worker
        // to receive work, while another was already inside the library:
        // ThreadSanitizer catches it as a race on the mutex mysql_server_init
        // sets up.  configure() runs before pool->start(), so there are no
        // workers yet at this point.
        static std::once_flag once;
        static bool started = false;
        std::call_once(once, [] { started = mysql_library_init(0, nullptr, nullptr) == 0; });
        if (!started) {
            error = "mysql: cannot initialize libmysqlclient";
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
            error = "mysql: missing 'database' in the configuration block";
            return false;
        }

        auto p = options.find("pool");
        if (p != options.end()) {
            long n = std::strtol(p->second.c_str(), nullptr, 10);
            if (n < 1 || n > 64) {
                error = "mysql: 'pool' must be between 1 and 64";
                return false;
            }
            set_pool_size(static_cast<size_t>(n));
        }
        conns_.assign(pool_size(), nullptr);
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "mysql: worker out of range"; return false; }
        if (conns_[worker] && mysql_ping(conns_[worker]) != 0) {
            mysql_close(conns_[worker]);
            conns_[worker] = nullptr;
        }
        if (conns_[worker]) return true;

        MYSQL* c = mysql_init(nullptr);
        if (!c) { error = "mysql: out of memory"; return false; }

        // Automatic reconnection disabled: silently reopening halfway through a
        // transaction would lose it without warning.  open() already reopens
        // between queries.  MySQL 8.0.34 dropped the option and that is now
        // the default behaviour.
#if defined(MYSQL_OPT_RECONNECT)
        lumen_script_my_bool reconnect = 0;
        mysql_options(c, MYSQL_OPT_RECONNECT, &reconnect);
#endif

        if (!mysql_real_connect(c, host_.c_str(), user_.c_str(), pass_.c_str(),
                                db_.c_str(), port_, nullptr, 0)) {
            error = std::string("mysql: cannot connect: ") + mysql_error(c);
            mysql_close(c);
            return false;
        }
        mysql_set_character_set(c, "utf8mb4");
        conns_[worker] = c;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        // All of this is local to the call, and has to be: there is a single
        // driver shared by the N pool workers.  With the bind buffers kept in
        // the object, two simultaneous queries trod on the pointers
        // libmysqlclient was still using.
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
        if (!meta) {                       // it returned no rows
            mysql_stmt_close(stmt);
            out = Value::list();
            return true;
        }

        // `max_length` is ZERO unless it is asked for explicitly and the result
        // is brought to the client.  Without these two calls, every buffer
        // stayed at the reserved size and any longer column was truncated
        // SILENTLY: a 4 KB TEXT arrived with 1024 bytes, cut mid-character on
        // top of that if it was UTF-8.
        lumen_script_my_bool yes = 1;
        mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &yes);
        if (mysql_stmt_store_result(stmt) != 0) {
            error = std::string("mysql: ") + mysql_stmt_error(stmt);
            mysql_free_result(meta);
            mysql_stmt_close(stmt);
            return false;
        }

        unsigned cols = mysql_num_fields(meta);
        MYSQL_FIELD* fields = mysql_fetch_fields(meta);

        // Output buffers: everything is asked for as text and converted
        // afterwards, which avoids a type table per MySQL integer variant.
        std::vector<std::vector<char>> bufs(cols);
        std::vector<unsigned long>     lens(cols, 0);
        // Arrays and not vector<bool>: that specialization packs bits and does
        // not allow taking the address of an element, which is what the API needs.
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
            // MYSQL_DATA_TRUNCATED used to slip through here as if it were a
            // good row.  With the buffers properly sized it should not happen;
            // if it does, it is a failure and not half a row of data.
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
        // With no parameters the statement goes straight through, without the
        // prepared statement protocol.
        //
        // It is not an optimization, it is NEEDED: MySQL accepts neither BEGIN
        // nor START TRANSACTION there —error 1295— so down the prepared path the
        // transaction never actually opened.  The UPDATE autocommitted and the
        // ROLLBACK returned success with nothing to undo.
        //
        // And it opens no hole: what makes injection impossible is that the
        // PARAMETERS travel by bind, and here there are none to bind.
        if (args.empty()) {
            MYSQL* c = conns_[worker];
            if (mysql_real_query(c, sql.c_str(),
                                 static_cast<unsigned long>(sql.size())) != 0) {
                error = std::string("mysql: ") + mysql_error(c);
                return false;
            }
            // An exec() on something returning rows would leave the connection
            // half-used and the next query would fail for no apparent reason.
            if (MYSQL_RES* r = mysql_store_result(c)) mysql_free_result(r);
            affected = static_cast<long long>(mysql_affected_rows(c));
            return true;
        }

        // All of this is local to the call, and has to be: there is a single
        // driver shared by the N pool workers.  With the bind buffers kept in
        // the object, two simultaneous queries trod on the pointers
        // libmysqlclient was still using.
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
            error = "mysql: no connection";
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
        if (!*out) { error = "mysql: out of memory"; return false; }

        if (mysql_stmt_prepare(*out, sql.c_str(),
                               static_cast<unsigned long>(sql.size())) != 0) {
            error = std::string("mysql: ") + mysql_stmt_error(*out);
            mysql_stmt_close(*out);
            *out = nullptr;
            return false;
        }

        unsigned expected = mysql_stmt_param_count(*out);
        if (expected != args.size()) {
            error = "mysql: the query has " + std::to_string(expected) +
                    " parameter(s) but " + std::to_string(args.size()) + " were passed";
            mysql_stmt_close(*out);
            *out = nullptr;
            return false;
        }
        if (args.empty()) return true;

        // Everything is sent as text: MySQL converts it to the column type, and
        // that way no branch per numeric type is needed.
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

    // `charsetnr == 63` is the binary charset: that is what tells a BLOB from a
    // TEXT, which in MySQL share a type and differ only in the encoding.
    static constexpr unsigned kBinario = 63;

    static Value typed(enum_field_types t, unsigned flags, unsigned charset,
                       const std::string& text) {
        // A BLOB is not text: it is arbitrary bytes.  Returning them as a string
        // left the response not valid UTF-8, and then the failure is not the
        // request's but the client's that receives it.  Base64 is how a binary
        // goes into a JSON.
        if (charset == kBinario &&
            (t == MYSQL_TYPE_BLOB      || t == MYSQL_TYPE_TINY_BLOB ||
             t == MYSQL_TYPE_MEDIUM_BLOB || t == MYSQL_TYPE_LONG_BLOB ||
             t == MYSQL_TYPE_STRING    || t == MYSQL_TYPE_VAR_STRING))
            return Value::str(crypto::base64_encode(text));

        switch (t) {
            case MYSQL_TYPE_TINY:  case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_LONG:  case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_INT24: case MYSQL_TYPE_YEAR: {
                // A BIGINT UNSIGNED reaches 18446744073709551615, which does not
                // fit in the Lumen Script integer.  strtoll would pin it at
                // INT64_MAX without warning, so above that cap it falls back to
                // a decimal, which is what the JSON parser does and what
                // JavaScript does.  Below that it stays exact.
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
