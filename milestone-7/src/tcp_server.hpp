// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
#ifndef TCP_SERVER_HPP
#define TCP_SERVER_HPP

#include <stdint.h>
#include <atomic>
#include <pthread.h>
#include <signal.h>

namespace nbt {

typedef void (*ClientConnectedCallback)(int client_socket_fd, void* user_data);

class TcpServer {
public:
    TcpServer(uint16_t port = 5277);
    ~TcpServer();

    bool start();
    void stop();

    void setConnectionCallback(ClientConnectedCallback callback, void* user_data = NULL);
    void notifySessionEnded();
    void markSessionActive() { has_active_client_.store(true); }   // re-assert after AASession::start() returned

private:
    static void* acceptThreadStub(void* arg);
    void acceptLoop();

    static void* connectorThreadStub(void* arg);
    void connectorLoop();

    int tryConnect(const char* ip, uint16_t port, int timeout_ms);

    uint16_t port_;
    int server_fd_;
    pthread_t accept_thread_;
    pthread_t connector_thread_;
    volatile bool is_running_;
    std::atomic<bool> has_active_client_;

    ClientConnectedCallback client_callback_;
    void* client_user_data_;
};

} // namespace nbt

#endif // TCP_SERVER_HPP
