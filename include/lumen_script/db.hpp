#pragma once
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <coroutine>

#include <lumen/core/event_loop.hpp>
#include "value.hpp"

namespace lumen_script {

// ─── Pool de hilos para las llamadas a base de datos ─────────────────────────
//
// Los tres clientes (SQLite, libpq, libmysqlclient) tienen API bloqueante.
// Ejecutarlos en el hilo del event loop clavaria ese core entero: todas las
// conexiones que le tocaron por SO_REUSEPORT dejarian de responder mientras
// dura la consulta.
//
// Por eso una consulta suspende el handler —igual que `await sleep`— y el
// trabajo real ocurre aqui.  Cada worker es dueno de UNA conexion, asi que no
// hay que repartir conexiones entre hilos ni sincronizar su uso.
class DbPool {
public:
    ~DbPool();

    void start(size_t workers);

    // Reparto libre: lo coge el primer worker disponible.
    void submit(std::function<void(size_t worker)> job);

    // Fijado a un worker concreto.  Lo necesita una transaccion: BEGIN, las
    // consultas y COMMIT tienen que ir por la MISMA conexion, y cada worker es
    // dueno de una.
    void submit_to(size_t worker, std::function<void(size_t worker)> job);

    void stop();

    size_t size() const { return workers_.size(); }

private:
    std::vector<std::thread>                        threads_;
    std::vector<int>                                workers_;   // solo para el tamano
    std::queue<std::function<void(size_t)>>              jobs_;
    std::vector<std::queue<std::function<void(size_t)>>> pinned_;
    std::mutex                                      mutex_;
    std::condition_variable                         cv_;
    bool                                            stopping_ = false;
};

// ─── Driver ──────────────────────────────────────────────────────────────────
//
// Un driver es lo minimo que hace falta para hablar con un motor: abrir la
// conexion de un worker, consultar y ejecutar.  Todo se llama desde el pool,
// nunca desde el hilo del event loop.
class DbDriver {
public:
    virtual ~DbDriver() = default;

    // Nombre con el que se importa y se invoca desde Lumen Script: `sqlite`, `postgres`,
    // `mysql`.
    virtual const char* name() const = 0;

    // Configuracion tomada del bloque `<modulo>:` de `app:`.
    virtual bool configure(const std::map<std::string, std::string>& options,
                           std::string& error) = 0;

    // Abre la conexion del worker indicado.  Se llama una vez por worker, la
    // primera vez que le toca trabajo.
    virtual bool open(size_t worker, std::string& error) = 0;

    // SELECT: devuelve una List de Dict, una entrada por fila.
    virtual bool query(size_t worker, const std::string& sql,
                       const std::vector<Value>& args,
                       Value& out, std::string& error) = 0;

    // INSERT/UPDATE/DELETE/DDL: devuelve el numero de filas afectadas.
    virtual bool exec(size_t worker, const std::string& sql,
                      const std::vector<Value>& args,
                      long long& affected, std::string& error) = 0;

    // Identificador generado por el ultimo INSERT en esa conexion.  No todos
    // los motores lo ofrecen de forma fiable; el que no, lo dice.
    virtual bool last_insert_id(size_t worker, long long& id, std::string& error) {
        (void)worker; (void)id;
        error = std::string(name()) + ": last_id() no esta disponible; usa "
                "'insert ... returning id' con query()";
        return false;
    }

    size_t pool_size() const { return pool_size_; }
    void   set_pool_size(size_t n) { pool_size_ = n; }

protected:
    size_t pool_size_ = 4;
};

// ─── Registro de modulos ─────────────────────────────────────────────────────
//
// Los drivers compilados se registran aqui.  Que un modulo exista depende de
// las opciones de cmake, asi que `import` de uno no compilado tiene que dar un
// error claro y no un fallo raro mas adelante.
class DbRegistry {
public:
    static DbRegistry& instance();

    // Nombres de los drivers disponibles en este binario.
    std::vector<std::string> available() const;
    bool                     has(const std::string& name) const;

    // Activa un modulo con su configuracion y arranca su pool.
    bool activate(const std::string& name,
                  const std::map<std::string, std::string>& options,
                  std::string& error);

    // Driver activo, o nullptr si ese modulo no se importo.
    DbDriver* active(const std::string& name) const;
    DbPool*   pool(const std::string& name) const;

    void shutdown();

private:
    DbRegistry();

    struct Slot {
        std::unique_ptr<DbDriver> driver;
        std::unique_ptr<DbPool>   pool;
        bool                      activated = false;
    };
    std::map<std::string, Slot> slots_;
};

// ─── Puente con las corrutinas del motor ─────────────────────────────────────
//
// Suspende el handler, hace el trabajo en el pool y lo reanuda EN EL HILO DE SU
// EVENT LOOP.  Reanudar desde el hilo del pool tocaria estructuras del loop
// desde fuera, que no son seguras para eso.
struct DbAwaitable {
    DbPool*                        pool;
    lumen::core::EventLoop*       loop;
    std::function<void(size_t)>    work;   // recibe el worker: elige la conexion
    int                            pinned = -1;   // >= 0 dentro de una transaccion

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        // `this` vive hasta que el co_await termina, y el handle se reanuda una
        // sola vez, asi que capturarlos por valor es seguro.
        auto* p = pool; auto* l = loop; auto w = work; int pin = pinned;
        auto job = [l, h, w](size_t worker) {
            w(worker);
            l->post([h]() mutable { h.resume(); });
        };
        if (pin >= 0) p->submit_to(static_cast<size_t>(pin), std::move(job));
        else          p->submit(std::move(job));
    }

    void await_resume() const noexcept {}
};

} // namespace lumen_script
