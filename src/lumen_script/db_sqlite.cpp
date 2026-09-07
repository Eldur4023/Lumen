#include <string_view>
#include <unordered_map>
#include <lumen_script/db.hpp>
#include <lumen_script/crypto.hpp>

#include <sqlite3.h>

#include <cstring>

namespace lumen_script {

namespace {

// SQLite driver.
//
// Each worker opens its own connection to the same file.  SQLite serializes
// writes internally, so several concurrent connections are safe; what is
// enabled is WAL, which allows reading while another writes instead of
// blocking everyone.
class SqliteDriver : public DbDriver {
public:
    const char* name() const override { return "sqlite"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        auto it = options.find("file");
        if (it == options.end() || it->second.empty()) {
            error = "sqlite: missing 'file' in the configuration block";
            return false;
        }
        file_ = it->second;

        auto p = options.find("pool");
        if (p != options.end()) {
            long n = std::strtol(p->second.c_str(), nullptr, 10);
            if (n < 1 || n > 64) {
                error = "sqlite: 'pool' must be between 1 and 64";
                return false;
            }
            set_pool_size(static_cast<size_t>(n));
        }

        auto t = options.find("timeout_ms");
        if (t != options.end()) busy_timeout_ = std::atoi(t->second.c_str());

        conns_.assign(pool_size(), nullptr);
        cache_.assign(pool_size(), {});
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "sqlite: worker out of range"; return false; }
        if (conns_[worker]) return true;

        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(file_.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK) {
            error = std::string("sqlite: cannot open '") + file_ + "': " +
                    (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
            if (db) sqlite3_close(db);
            return false;
        }

        // WAL: concurrent reads with a write in progress.  Without this, with
        // several workers any write would block every read.
        char* msg = nullptr;
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);
        sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);

        // Waits instead of failing when another connection holds the file.
        sqlite3_busy_timeout(db, busy_timeout_);

        conns_[worker] = db;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        bool          cacheada = false;
        if (!prepare(worker, sql, args, &stmt, &cacheada, error)) return false;

        Value::List rows;
        int cols = sqlite3_column_count(stmt);

        for (;;) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
                release(stmt, cacheada);
                return false;
            }
            Value::Dict row;
            // The column count is known: without this the dictionary grew in
            // steps and it was five reallocations per row.
            row.reserve(static_cast<size_t>(cols));
            for (int i = 0; i < cols; ++i) {
                const char* col = sqlite3_column_name(stmt, i);
                // No ternary: mixing it with std::to_string forced building a
                // temporary std::string on EVERY column of EVERY row just to
                // read it back as a string_view.
                if (col) row[std::string_view(col)]  = column_value(stmt, i);
                else     row[std::to_string(i)]      = column_value(stmt, i);
            }
            rows.push_back(Value::dict(std::move(row)));
        }

        release(stmt, cacheada);
        out = Value::list(std::move(rows));
        return true;
    }

    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        bool          cacheada = false;
        if (!prepare(worker, sql, args, &stmt, &cacheada, error)) return false;

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
            release(stmt, cacheada);
            return false;
        }
        release(stmt, cacheada);
        affected = sqlite3_changes(conns_[worker]);
        return true;
    }

    bool last_insert_id(size_t worker, long long& id, std::string& error) override {
        if (worker >= conns_.size() || !conns_[worker]) {
            error = "sqlite: no connection";
            return false;
        }
        id = sqlite3_last_insert_rowid(conns_[worker]);
        return true;
    }

    ~SqliteDriver() override {
        // The statements first: sqlite3_close fails if any are still alive.
        for (auto& table : cache_)
            for (auto& [_, stmt] : table) sqlite3_finalize(stmt);
        for (auto* db : conns_) if (db) sqlite3_close(db);
    }

    // Returns the statement to where it came from.  A cached one is reset —it
    // has to be: in WAL mode a half-walked statement keeps its read snapshot
    // open— and a one-off one is destroyed.
    static void release(sqlite3_stmt* stmt, bool cacheada) {
        if (!stmt) return;
        if (cacheada) { sqlite3_reset(stmt); sqlite3_clear_bindings(stmt); }
        else          sqlite3_finalize(stmt);
    }

private:
    // Prepared statements, per connection.
    //
    // The queries of a .lum are source literals, so the set is closed and
    // small.  Without this SQLite parsed and planned the same SELECT tens of
    // thousands of times per second.
    //
    // With the cap full, a new query is prepared and destroyed as before: one
    // already inside is never evicted.  That way, whoever builds SQL by hand
    // cannot blow up the memory or evict the good ones.
    static constexpr size_t kMaxSentencias = 128;

    std::string           file_;
    int                   busy_timeout_ = 5000;
    std::vector<sqlite3*> conns_;
    std::vector<std::unordered_map<std::string, sqlite3_stmt*>> cache_;

    // Parameters ALWAYS go through bind, never concatenated: that is what makes
    // SQL injection impossible from Lumen Script.
    bool prepare(size_t worker, const std::string& sql, const std::vector<Value>& args,
                 sqlite3_stmt** out, bool* cacheada, std::string& error) {
        sqlite3* db = conns_[worker];
        auto&    table = cache_[worker];

        if (auto it = table.find(sql); it != table.end()) {
            *out      = it->second;
            *cacheada = true;
            sqlite3_reset(*out);
            sqlite3_clear_bindings(*out);
        } else {
            if (sqlite3_prepare_v2(db, sql.c_str(), -1, out, nullptr) != SQLITE_OK) {
                error = std::string("sqlite: ") + sqlite3_errmsg(db);
                return false;
            }
            *cacheada = table.size() < kMaxSentencias;
            if (*cacheada) table.emplace(sql, *out);
        }

        int expected = sqlite3_bind_parameter_count(*out);
        if (expected != static_cast<int>(args.size())) {
            error = "sqlite: the query has " + std::to_string(expected) +
                    " parameter(s) but " + std::to_string(args.size()) + " were passed";
            release(*out, *cacheada);
            *out = nullptr;
            return false;
        }

        for (size_t i = 0; i < args.size(); ++i) {
            const Value& v = args[i];
            int idx = static_cast<int>(i) + 1;
            int rc;
            if      (v.is_null())  rc = sqlite3_bind_null(*out, idx);
            else if (v.is_bool())  rc = sqlite3_bind_int(*out, idx, v.as_bool() ? 1 : 0);
            else if (v.is_int())   rc = sqlite3_bind_int64(*out, idx, v.as_int());
            else if (v.is_float()) rc = sqlite3_bind_double(*out, idx, v.as_float());
            else {
                std::string s = v.to_string();
                rc = sqlite3_bind_text(*out, idx, s.c_str(),
                                       static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) {
                error = std::string("sqlite: while binding parameter ") +
                        std::to_string(idx) + ": " + sqlite3_errmsg(db);
                release(*out, *cacheada);
                *out = nullptr;
                return false;
            }
        }
        return true;
    }

    static Value column_value(sqlite3_stmt* stmt, int i) {
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:    return Value::null();
            case SQLITE_INTEGER: return Value::integer(sqlite3_column_int64(stmt, i));
            case SQLITE_FLOAT:   return Value::real(sqlite3_column_double(stmt, i));

            // A BLOB is not text: it is arbitrary bytes.  Returning them as a
            // string left the response not valid UTF-8 —an x'FF' slipped through
            // raw— and then the failure is not the request's but the client's
            // that receives it, which is worse because it shows up far away.
            // Base64 is how a binary goes into a JSON.
            case SQLITE_BLOB: {
                const void* p = sqlite3_column_blob(stmt, i);
                int         n = sqlite3_column_bytes(stmt, i);
                if (!p || n <= 0) return Value::str("");
                return Value::str(crypto::base64_encode(
                    std::string_view(static_cast<const char*>(p), static_cast<size_t>(n))));
            }

            default: {
                const auto* txt = sqlite3_column_text(stmt, i);
                int         len = sqlite3_column_bytes(stmt, i);
                return Value::str(txt ? std::string(reinterpret_cast<const char*>(txt),
                                                    static_cast<size_t>(len))
                                      : std::string());
            }
        }
    }
};

} // namespace

std::unique_ptr<DbDriver> make_sqlite_driver() {
    return std::make_unique<SqliteDriver>();
}

} // namespace lumen_script
