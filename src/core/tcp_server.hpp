#pragma once
#include <string>
#include <cstdint>
#include <atomic>
#include <memory>
#include <lumen/core/event_loop.hpp>
#include "../../include/lumen/types.hpp"

namespace lumen::core {

class TcpServer {
public:
    // max_connections: maximum simultaneous open connections (default 10 000).
    // Excess connections are immediately closed with 503.
    //
    // conn_count: optional shared counter — pass the same one to every
    // TcpServer instance so the limit is enforced globally across all threads.
    // If nullptr, a per-instance counter is created (single-thread behaviour).
    TcpServer(const std::string& host, uint16_t port,
              EventLoop& loop, lumen::DispatchFn dispatch,
              int max_connections = 10'000,
              std::shared_ptr<std::atomic<int>> conn_count = nullptr);
    ~TcpServer();

    // Stop accepting new connections without shutting down the event loop.
    // In-flight connections continue until they finish or are timed out.
    //
    // SOLO desde el hilo de su propio loop: toca el mapa de manejadores, que el
    // loop esta leyendo en su epoll_wait.  Desde fuera, usa pedir_parada().
    void stop_accepting();

    // Lo mismo, pedido desde otro hilo.
    //
    // El apagado ordenado lo dispara el hilo del loop principal, pero cada
    // worker tiene su propio loop y su propio servidor: llamar a
    // stop_accepting() sobre ellos desde fuera era un erase concurrente con
    // lectura sobre el unordered_map del loop, que no es leer un valor viejo
    // sino corromper el contenedor, y encima mientras las conexiones todavia
    // drenan.  Se encola en el loop que toca, que es el mecanismo que el resto
    // del codigo ya usaba.
    void pedir_parada();

private:
    int                listen_fd_ = -1;
    EventLoop&         loop_;
    lumen::DispatchFn dispatch_;
    int                max_connections_;

    // Shared between TcpServer and every HttpConnection so they can decrement
    // the counter on close without holding a pointer back to TcpServer.
    std::shared_ptr<std::atomic<int>> conn_count_;

    void on_accept();
};

} // namespace lumen::core
