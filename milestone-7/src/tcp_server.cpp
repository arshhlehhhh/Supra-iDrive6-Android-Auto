// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#include "nbt_env.hpp"
#include "tcp_server.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/tcp.h>

namespace nbt {

// Low-latency + dead-peer detection for AA sockets. Small frames (media ACKs, input)
// must not wait for Nagle/delayed-ACK; a vanished wireless phone must eventually
// produce an error instead of an eternal recv().
static void tuneSocket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    int idle = 10;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
    int intvl = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
    int cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

TcpServer::TcpServer(uint16_t port)
    : port_(port),
      server_fd_(-1),
      accept_thread_(0),
      connector_thread_(0),
      is_running_(false),
      has_active_client_(false),
      client_callback_(NULL),
      client_user_data_(NULL) {
}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::notifySessionEnded() {
    has_active_client_.store(false);
}

int TcpServer::tryConnect(const char* ip, uint16_t port, int timeout_ms) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    // Set non-blocking for quick timeout probe
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    int res = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res < 0) {
        if (errno == EINPROGRESS) {
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            timeval tv;
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;

            res = select(sock + 1, NULL, &write_fds, NULL, &tv);
            if (res > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) {
                    // Connected successfully! Restore blocking mode.
                    fcntl(sock, F_SETFL, flags);
                    tuneSocket(sock);
                    return sock;
                }
            }
        }
        close(sock);
        return -1;
    }

    fcntl(sock, F_SETFL, flags);
    tuneSocket(sock);
    return sock;
}

bool TcpServer::start() {
    if (is_running_) return true;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        perror("[TcpServer] socket creation failed");
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Bind to 0.0.0.0
    address.sin_port        = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("[TcpServer] bind failed");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 5) < 0) {
        perror("[TcpServer] listen failed");
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    is_running_ = true;
    has_active_client_.store(false);

    pthread_create(&accept_thread_, NULL, &TcpServer::acceptThreadStub, this);
    pthread_create(&connector_thread_, NULL, &TcpServer::connectorThreadStub, this);

    printf("[TcpServer] Bi-directional Android Auto engine active on port %u (Listener + Auto-Connector)\n", port_);
    printf("[TcpServer] Notice: If phone does not connect, verify firewall is disabled via 'pfctl -d'.\n");
    return true;
}

void TcpServer::stop() {
    if (!is_running_) return;
    is_running_ = false;

    if (accept_thread_) {
        pthread_join(accept_thread_, NULL);   // accept loop polls is_running_ every second
        accept_thread_ = 0;
    }

    if (server_fd_ >= 0) {
        close(server_fd_);
        server_fd_ = -1;
    }

    if (accept_thread_) {
        pthread_join(accept_thread_, NULL);
        accept_thread_ = 0;
    }

    if (connector_thread_) {
        pthread_join(connector_thread_, NULL);
        connector_thread_ = 0;
    }

    printf("[TcpServer] Listener and auto-connector stopped.\n");
}

void TcpServer::setConnectionCallback(ClientConnectedCallback callback, void* user_data) {
    client_callback_ = callback;
    client_user_data_ = user_data;
}

void* TcpServer::acceptThreadStub(void* arg) {
    reinterpret_cast<TcpServer*>(arg)->acceptLoop();
    return NULL;
}

void* TcpServer::connectorThreadStub(void* arg) {
    reinterpret_cast<TcpServer*>(arg)->connectorLoop();
    return NULL;
}

void TcpServer::acceptLoop() {
    while (is_running_) {
        sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Poll with a timeout instead of blocking in accept(): closing a listening
        // socket does not reliably wake a thread blocked in accept() on QNX, which
        // made stop() (and therefore Ctrl+C) hang in pthread_join.
        int lfd = server_fd_;
        if (lfd < 0) break;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(lfd, &rf);
        timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int sr = select(lfd + 1, &rf, NULL, NULL, &tv);
        if (sr <= 0) {
            if (sr < 0 && errno != EINTR && is_running_) sleep(1);
            continue;
        }

        int client_fd = accept(lfd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (!is_running_) break;
            if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN) {
                perror("[TcpServer] accept failed");
                sleep(1);   // do not spin at 100% CPU on persistent errors (EMFILE...)
            }
            continue;
        }
        tuneSocket(client_fd);

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        printf("[TcpServer] Incoming Android Auto connection from %s:%u (fd: %d)!\n",
               client_ip, ntohs(client_addr.sin_port), client_fd);

        // Claim the session slot atomically. A second connection must never
        // displace a live session: if a session is already active, close the
        // new fd and keep listening. The callback (onClientConnected) clears
        // the flag via notifySessionEnded() if starting the session fails,
        // so a failed start does not wedge the server in "active" state.
        bool expected = false;
        if (!has_active_client_.compare_exchange_strong(expected, true)) {
            printf("[TcpServer] Rejecting connection from %s:%u (fd: %d): a session is already active.\n",
                   client_ip, ntohs(client_addr.sin_port), client_fd);
            close(client_fd);
            continue;
        }
        if (client_callback_) {
            client_callback_(client_fd, client_user_data_);
        } else {
            // No consumer: do not leave the flag set, or the connector loop
            // (which waits on has_active_client_) would stall forever.
            has_active_client_.store(false);
            close(client_fd);
        }
    }
}

void TcpServer::connectorLoop() {
    // Known priority IPs where the phone has connected in past runs
    static const int priority_last_octets[] = {
        110, 111, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
        112, 113, 114, 115, 116, 117, 118, 119, 120, 125, 130, 140, 150,
        2, 3, 4, 5, 10, 20
    };
    static const size_t num_priorities = sizeof(priority_last_octets) / sizeof(priority_last_octets[0]);

    // Native wireless mode: the phone connects to us after the Bluetooth handshake,
    // so the outbound scanner (dev "head unit server" workflow) is disabled.
    const bool scanner_disabled = (nbt::wirelessEnabled());
    if (scanner_disabled) {
        printf("[TcpServer] Wireless mode (default): outbound phone scanner disabled, accept-only.\n");
    }

    while (is_running_) {
        if (has_active_client_.load() || scanner_disabled) {
            sleep(1);
            continue;
        }

        // 1. Probe priority targets on port 5277
        char ip_buf[32];
        for (size_t i = 0; i < num_priorities && is_running_ && !has_active_client_.load(); ++i) {
            snprintf(ip_buf, sizeof(ip_buf), "172.16.224.%d", priority_last_octets[i]);
            int sock = tryConnect(ip_buf, port_, 80);
            if (sock >= 0) {
                printf("[TcpServer] >>> FOUND ANDROID AUTO PHONE at %s:%u (fd: %d)! Connected!\n",
                       ip_buf, port_, sock);
                // Claim atomically: an accept may have started a session
                // between the probe and here. On a lost claim, close the
                // socket instead of displacing the live session.
                bool expected = false;
                if (!has_active_client_.compare_exchange_strong(expected, true)) {
                    printf("[TcpServer] Rejecting connector socket (fd: %d): a session is already active.\n", sock);
                    close(sock);
                    break;
                }
                if (client_callback_) {
                    client_callback_(sock, client_user_data_);
                } else {
                    has_active_client_.store(false);
                    close(sock);
                }
                while (is_running_ && has_active_client_.load()) {
                    sleep(1);
                }
                break;
            }
        }

        // 2. Full sweep across entire 172.16.224.100 - 172.16.224.150 range
        for (int octet = 100; octet <= 150 && is_running_ && !has_active_client_.load(); ++octet) {
            snprintf(ip_buf, sizeof(ip_buf), "172.16.224.%d", octet);
            int sock = tryConnect(ip_buf, port_, 60);
            if (sock >= 0) {
                printf("[TcpServer] >>> FOUND ANDROID AUTO PHONE at %s:%u (fd: %d)! Connected!\n",
                       ip_buf, port_, sock);
                bool expected = false;
                if (!has_active_client_.compare_exchange_strong(expected, true)) {
                    printf("[TcpServer] Rejecting connector socket (fd: %d): a session is already active.\n", sock);
                    close(sock);
                    break;
                }
                if (client_callback_) {
                    client_callback_(sock, client_user_data_);
                } else {
                    has_active_client_.store(false);
                    close(sock);
                }
                while (is_running_ && has_active_client_.load()) {
                    sleep(1);
                }
                break;
            }
        }

        // Rest before next scanning cycle
        for (int i = 0; i < 15 && is_running_ && !has_active_client_.load(); ++i) {
            usleep(100000); // 100ms
        }
    }
}

} // namespace nbt
