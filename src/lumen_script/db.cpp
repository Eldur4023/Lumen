#include <lumen_script/db.hpp>

namespace lumen_script {

// ─── DbPool ──────────────────────────────────────────────────────────────────

DbPool::~DbPool() { stop(); }

void DbPool::start(size_t workers) {
    if (!threads_.empty()) return;
    workers_.assign(workers, 0);
    pinned_.resize(workers);

    for (size_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i] {
            for (;;) {
                std::function<void(size_t)> job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this, i] {
                        return stopping_ || !jobs_.empty() || !pinned_[i].empty();
                    });
                    if (stopping_ && jobs_.empty() && pinned_[i].empty()) return;

                    // Lo fijado a este worker va primero: es la continuacion de
                    // una transaccion que ya tiene su conexion abierta.
                    if (!pinned_[i].empty()) {
                        job = std::move(pinned_[i].front());
                        pinned_[i].pop();
                    } else if (!jobs_.empty()) {
                        job = std::move(jobs_.front());
                        jobs_.pop();
                    } else {
                        continue;
                    }
                }
                // Un trabajo que lanza no puede llevarse por delante el worker:
                // sin conexion viva, el modulo dejaria de responder para todos.
                try { job(i); } catch (...) {}
            }
        });
    }
}

void DbPool::submit(std::function<void(size_t)> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void DbPool::submit_to(size_t worker, std::function<void(size_t)> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || worker >= pinned_.size()) return;
        pinned_[worker].push(std::move(job));
    }
    // notify_all y no notify_one: el worker que debe atenderlo puede no ser el
    // que despierte, y los demas volveran a dormirse.
    cv_.notify_all();
}

void DbPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

// ─── DbRegistry ──────────────────────────────────────────────────────────────

// Cada driver compilado se declara aqui.  Las funciones existen solo si su
// opcion de cmake esta activada.
#ifdef LUMEN_SQLITE
std::unique_ptr<DbDriver> make_sqlite_driver();
#endif
#ifdef LUMEN_POSTGRES
std::unique_ptr<DbDriver> make_postgres_driver();
#endif
#ifdef LUMEN_MYSQL
std::unique_ptr<DbDriver> make_mysql_driver();
#endif

DbRegistry::DbRegistry() {
#ifdef LUMEN_SQLITE
    { Slot s; s.driver = make_sqlite_driver();   slots_["sqlite"]   = std::move(s); }
#endif
#ifdef LUMEN_POSTGRES
    { Slot s; s.driver = make_postgres_driver(); slots_["postgres"] = std::move(s); }
#endif
#ifdef LUMEN_MYSQL
    { Slot s; s.driver = make_mysql_driver();    slots_["mysql"]    = std::move(s); }
#endif
}

DbRegistry& DbRegistry::instance() {
    static DbRegistry r;
    return r;
}

std::vector<std::string> DbRegistry::available() const {
    std::vector<std::string> out;
    for (const auto& [name, _] : slots_) out.push_back(name);
    return out;
}

bool DbRegistry::has(const std::string& name) const {
    return slots_.count(name) > 0;
}

bool DbRegistry::activate(const std::string& name,
                          const std::map<std::string, std::string>& options,
                          std::string& error) {
    auto it = slots_.find(name);
    if (it == slots_.end()) {
        error = "el modulo '" + name + "' no esta compilado en este binario";
        return false;
    }
    Slot& slot = it->second;
    if (slot.activated) return true;

    if (!slot.driver->configure(options, error)) return false;

    slot.pool = std::make_unique<DbPool>();
    slot.pool->start(slot.driver->pool_size());
    slot.activated = true;
    return true;
}

DbDriver* DbRegistry::active(const std::string& name) const {
    auto it = slots_.find(name);
    if (it == slots_.end() || !it->second.activated) return nullptr;
    return it->second.driver.get();
}

DbPool* DbRegistry::pool(const std::string& name) const {
    auto it = slots_.find(name);
    if (it == slots_.end() || !it->second.activated) return nullptr;
    return it->second.pool.get();
}

void DbRegistry::shutdown() {
    for (auto& [_, slot] : slots_)
        if (slot.pool) slot.pool->stop();
}

// ─── Puente compartido bytecode/--native ─────────────────────────────────────

namespace {
Value db_error(const std::string& msg) {
    Value::Dict d;
    d["error"] = Value::str(msg);
    return Value::dict(std::move(d));
}
} // namespace

lumen::Task<Value> await_db(DbOp op, const std::string& module, lumen::core::EventLoop* loop,
                            const std::string& sql, std::vector<Value> params,
                            std::map<std::string, int>& pinned_workers,
                            std::map<std::string, int>& last_exec_workers) {
    auto& reg    = DbRegistry::instance();
    auto* driver = reg.active(module);
    auto* pool   = reg.pool(module);
    if (!driver || !pool)
        co_return db_error("el modulo '" + module + "' no esta configurado: "
                           "falta su bloque en app:");

    // Dentro de una transaccion, todo va por la conexion que la abrio.
    int  pin   = -1;
    auto pinit = pinned_workers.find(module);
    if (pinit != pinned_workers.end()) pin = pinit->second;

    // last_id() se encamina a la conexion del ultimo exec: el identificador
    // generado no existe en las demas.
    if (pin < 0 && op == DbOp::LastId) {
        auto le = last_exec_workers.find(module);
        if (le != last_exec_workers.end()) pin = le->second;
    }

    auto result = std::make_shared<Value>(Value::null());
    auto errmsg = std::make_shared<std::string>();
    auto used   = std::make_shared<int>(-1);

    co_await DbAwaitable{pool, loop,
        [driver, sql, params, op, result, errmsg, used](size_t worker) {
            *used = static_cast<int>(worker);
            std::string err;
            if (!driver->open(worker, err)) { *errmsg = err; return; }

            long long n = 0;
            if (op == DbOp::Query) {
                Value rows;
                if (!driver->query(worker, sql, params, rows, err)) { *errmsg = err; return; }
                *result = std::move(rows);
            } else if (op == DbOp::Exec) {
                if (!driver->exec(worker, sql, params, n, err)) { *errmsg = err; return; }
                *result = Value::integer(n);
            } else if (op == DbOp::LastId) {
                if (!driver->last_insert_id(worker, n, err)) { *errmsg = err; return; }
                *result = Value::integer(n);
            } else {
                const char* stmt = (op == DbOp::Begin)  ? "BEGIN"
                                  : (op == DbOp::Commit) ? "COMMIT" : "ROLLBACK";
                if (!driver->exec(worker, stmt, {}, n, err)) { *errmsg = err; return; }
                *result = Value::boolean(true);
            }
        },
        pin};

    if (!errmsg->empty()) co_return db_error(*errmsg);

    if (op == DbOp::Exec) last_exec_workers[module] = *used;

    // La transaccion fija su conexion al abrirse y la suelta al cerrarse.
    if (op == DbOp::Begin) pinned_workers[module] = *used;
    else if (op == DbOp::Commit || op == DbOp::Rollback) pinned_workers.erase(module);

    co_return std::move(*result);
}

} // namespace lumen_script
