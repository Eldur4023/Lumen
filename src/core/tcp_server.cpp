#include "tcp_server.hpp"
#include "../http/http_connection.hpp"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <memory>
#include <iostream>

namespace lumen::core {

TcpServer::TcpServer(const std::string& host, uint16_t port,
                     EventLoop& loop, lumen::DispatchFn dispatch,
                     int max_connections,
                     std::shared_ptr<std::atomic<int>> conn_count)
    : loop_(loop)
    , dispatch_(std::move(dispatch))
    , max_connections_(max_connections)
    , conn_count_(conn_count ? std::move(conn_count)
                             : std::make_shared<std::atomic<int>>(0))
{
    // Usamos IPv4 cuando el host lo indica (0.0.0.0 o IP v4)
    // y IPv6 solo cuando se pide explícitamente (::)
    bool use_ipv6 = (host == "::" || host.find(':') != std::string::npos);
    int family    = use_ipv6 ? AF_INET6 : AF_INET;

    listen_fd_ = socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0)
        throw std::runtime_error(std::string("socket: ") + strerror(errno));

    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    setsockopt(listen_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (use_ipv6) {
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        const char* h = (host == "::") ? "::" : host.c_str();
        if (inet_pton(AF_INET6, h, &addr.sin6_addr) != 1)
            throw std::runtime_error("invalid IPv6 host: " + host);
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind: ") + strerror(errno));
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (host == "0.0.0.0") {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else {
            // inet_pton returns 0 for malformed input; inet_addr would silently
            // accept some malformed strings or return INADDR_NONE, causing a
            // confusing bind on broadcast/all-ones.
            if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
                throw std::runtime_error("invalid IPv4 host: " + host);
        }
        if (bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind: ") + strerror(errno));
    }

    if (listen(listen_fd_, SOMAXCONN) < 0)
        throw std::runtime_error(std::string("listen: ") + strerror(errno));

    loop_.add(listen_fd_, EPOLLIN, [this](uint32_t) { on_accept(); });
}

TcpServer::~TcpServer() {
    stop_accepting();
}

void TcpServer::pedir_parada() {
    loop_.post([this] { stop_accepting(); });
}

void TcpServer::stop_accepting() {
    if (listen_fd_ >= 0) {
        loop_.remove(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void TcpServer::on_accept() {
    while (true) {
        sockaddr_storage addr{};
        socklen_t addrlen = sizeof(addr);

        int client_fd = accept4(listen_fd_, (sockaddr*)&addr, &addrlen,
                                SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            std::cerr << "accept4: " << strerror(errno) << '\n';
            break;
        }

        // Reject if at connection limit — send 503 and close immediately.
        int current = conn_count_->fetch_add(1, std::memory_order_relaxed);
        if (current >= max_connections_) {
            conn_count_->fetch_sub(1, std::memory_order_relaxed);
            // Write a minimal 503 without allocating an HttpConnection.
            // Body is exactly 32 bytes — keep Content-Length in sync.
            const char resp[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: 32\r\n"
                "Connection: close\r\n\r\n"
                "{\"error\":\"Too many connections\"}";
            (void)::write(client_fd, resp, sizeof(resp) - 1);
            ::close(client_fd);
            continue;
        }

        auto conn = std::make_shared<http::HttpConnection>(
            client_fd, loop_, dispatch_, conn_count_);

        conn->start();

        loop_.add(client_fd, EPOLLIN, [conn](uint32_t events) {
            conn->on_event(events);
        });
    }
}

} // namespace lumen::core
