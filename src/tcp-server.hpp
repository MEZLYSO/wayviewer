#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A small broadcast server. Every connected client (TCP or Unix stream)
// receives the same MPEG-TS byte stream. Slow clients are dropped instead of
// blocking the encoder, and clients can connect or disconnect at any time.
class TcpServer
{
public:
    TcpServer(std::string host, int port, std::string unix_path = {});
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // Bind, listen and start the accept thread.
    bool start();
    void stop();

    // Broadcast data to every connected client. Returns true if the data was
    // accepted, even when there are no clients connected.
    bool write(const uint8_t* data, size_t size);

    // Ask the encoder to emit an IDR frame as soon as possible. Called when a
    // client connects so it can start decoding without waiting for the next
    // scheduled keyframe.
    void request_keyframe();
    bool consume_keyframe_request();

    size_t client_count() const;

private:
    struct Client;

    void accept_loop();
    void accept_client(int fd, bool is_tcp);
    void client_loop(Client* client);
    void reap_clients();

    std::string host_;
    int port_;
    std::string unix_path_;
    std::atomic<int> listen_fd_{-1};
    std::atomic<int> unix_fd_{-1};
    std::atomic<bool> running_{false};
    std::atomic<bool> keyframe_requested_{false};
    std::thread accept_thread_;
    mutable std::mutex clients_mutex_;
    std::vector<std::unique_ptr<Client>> clients_;
};
