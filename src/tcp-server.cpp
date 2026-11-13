#include "tcp-server.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <utility>

namespace {
// Keep only a short amount of data queued per client. If a client cannot keep
// up, it is disconnected instead of accumulating latency.
constexpr size_t MAX_CLIENT_QUEUE_BYTES = 512 * 1024;
constexpr int ACCEPT_POLL_TIMEOUT_MS = 200;
constexpr int CLIENT_SEND_TIMEOUT_MS = 200;
constexpr int CLIENT_SNDBUF_BYTES = 64 * 1024;
#ifdef TCP_NOTSENT_LOWAT
constexpr int CLIENT_NOTSENT_LOWAT_BYTES = 16 * 1024;
#endif
#ifdef TCP_USER_TIMEOUT
constexpr unsigned int CLIENT_USER_TIMEOUT_MS = 3000;
#endif
}

struct TcpServer::Client
{
    int fd = -1;
    bool is_tcp = false;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    // Encoded chunks are shared between all clients, so broadcasting does not
    // copy the same MPEG-TS bytes once per client.
    std::deque<std::shared_ptr<const std::vector<uint8_t>>> queue;
    size_t queued_bytes = 0;
    std::atomic<bool> alive{true};

    ~Client()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

TcpServer::TcpServer(std::string host, int port, std::string unix_path)
    : host_(std::move(host)), port_(port), unix_path_(std::move(unix_path))
{
}

TcpServer::~TcpServer()
{
    stop();
}

bool TcpServer::start()
{
    if (running_.load())
        return true;

    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const char* node = host_.empty() ? nullptr : host_.c_str();
    const std::string service = std::to_string(port_);

    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(node, service.c_str(), &hints, &result);
    if (rc != 0)
    {
        std::cerr << "getaddrinfo(" << host_ << "): "
            << gai_strerror(rc) << std::endl;
        return false;
    }

    int fd = -1;
    int last_error = 0;
    for (auto* ai = result; ai != nullptr; ai = ai->ai_next)
    {
        fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
        {
            last_error = errno;
            continue;
        }

        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

#ifdef IPV6_V6ONLY
        if (ai->ai_family == AF_INET6)
            ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#endif

        if (::bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            ::listen(fd, SOMAXCONN) == 0)
        {
            break;
        }

        last_error = errno;
        ::close(fd);
        fd = -1;
    }

    freeaddrinfo(result);

    if (fd < 0)
    {
        std::cerr << "Failed to listen on " << host_ << ":" << port_ << ": "
            << std::strerror(last_error) << std::endl;
        return false;
    }

    int unix_fd = -1;
    if (!unix_path_.empty())
    {
        unix_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (unix_fd < 0)
        {
            std::cerr << "Failed to create Unix socket " << unix_path_ << ": "
                << std::strerror(errno) << std::endl;
            ::close(fd);
            return false;
        }

        struct sockaddr_un addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        if (unix_path_.size() >= sizeof(addr.sun_path))
        {
            std::cerr << "Unix socket path is too long: " << unix_path_ << std::endl;
            ::close(unix_fd);
            ::close(fd);
            return false;
        }
        std::memcpy(addr.sun_path, unix_path_.c_str(), unix_path_.size() + 1);

        // Remove a stale socket from a previous run.
        ::unlink(unix_path_.c_str());

        if (::bind(unix_fd, reinterpret_cast<struct sockaddr*>(&addr),
                sizeof(addr)) != 0 ||
            ::listen(unix_fd, SOMAXCONN) != 0)
        {
            std::cerr << "Failed to listen on Unix socket " << unix_path_ << ": "
                << std::strerror(errno) << std::endl;
            ::close(unix_fd);
            ::close(fd);
            ::unlink(unix_path_.c_str());
            return false;
        }

        // Only the owner may connect to the local stream socket.
        ::chmod(unix_path_.c_str(), 0600);
    }

    listen_fd_.store(fd);
    unix_fd_.store(unix_fd);
    running_.store(true);
    accept_thread_ = std::thread(&TcpServer::accept_loop, this);
    return true;
}

void TcpServer::stop()
{
    if (!running_.exchange(false))
        return;

    int fd = listen_fd_.exchange(-1);
    if (fd >= 0)
    {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }

    int ufd = unix_fd_.exchange(-1);
    if (ufd >= 0)
    {
        ::shutdown(ufd, SHUT_RDWR);
        ::close(ufd);
    }

    if (!unix_path_.empty())
        ::unlink(unix_path_.c_str());

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& client : clients_)
        {
            {
                std::lock_guard<std::mutex> client_lock(client->mutex);
                client->alive.store(false);
            }
            ::shutdown(client->fd, SHUT_RDWR);
            client->cv.notify_all();
        }
    }

    if (accept_thread_.joinable())
        accept_thread_.join();

    reap_clients();
}

void TcpServer::accept_loop()
{
    while (running_.load())
    {
        struct pollfd pfds[2];
        int nfds = 0;

        const int tcp_fd = listen_fd_.load();
        if (tcp_fd >= 0)
        {
            pfds[nfds].fd = tcp_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        const int unix_fd = unix_fd_.load();
        if (unix_fd >= 0)
        {
            pfds[nfds].fd = unix_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }

        if (nfds == 0)
            break;

        int rc = ::poll(pfds, nfds, ACCEPT_POLL_TIMEOUT_MS);
        if (!running_.load())
            break;

        if (rc > 0)
        {
            for (int i = 0; i < nfds; ++i)
            {
                if (!(pfds[i].revents & POLLIN))
                    continue;

                const bool is_tcp = pfds[i].fd == tcp_fd;
                int fd = ::accept(pfds[i].fd, nullptr, nullptr);
                if (fd >= 0)
                    accept_client(fd, is_tcp);
            }
        }

        reap_clients();
    }

    reap_clients();
}

void TcpServer::accept_client(int fd, bool is_tcp)
{
    if (is_tcp)
    {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

#ifdef TCP_KEEPIDLE
        int keepidle = 10;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
#endif
#ifdef TCP_KEEPINTVL
        int keepintvl = 5;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
#endif
#ifdef TCP_KEEPCNT
        int keepcnt = 3;
        ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));
#endif
#ifdef TCP_NOTSENT_LOWAT
        int lowat = CLIENT_NOTSENT_LOWAT_BYTES;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NOTSENT_LOWAT, &lowat, sizeof(lowat));
#endif
#ifdef TCP_USER_TIMEOUT
        unsigned int user_timeout = CLIENT_USER_TIMEOUT_MS;
        ::setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout,
            sizeof(user_timeout));
#endif
    }

    // A small send buffer plus a short send timeout keeps kernel buffering
    // (and therefore latency) bounded. A slow client is dropped instead of
    // delaying the encoder.
    int sndbuf = CLIENT_SNDBUF_BYTES;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = CLIENT_SEND_TIMEOUT_MS * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    auto client = std::make_unique<Client>();
    client->fd = fd;
    client->is_tcp = is_tcp;
    Client* raw = client.get();

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        clients_.push_back(std::move(client));
    }

    raw->thread = std::thread(&TcpServer::client_loop, this, raw);

    // Make the next encoded frame an IDR so the new client can decode
    // immediately instead of waiting for the scheduled keyframe.
    request_keyframe();

    std::cerr << (is_tcp ? "TCP" : "Unix") << " client connected ("
        << client_count() << " total)" << std::endl;
}

void TcpServer::client_loop(Client* client)
{
    while (client->alive.load() && running_.load())
    {
        std::shared_ptr<const std::vector<uint8_t>> chunk;
        {
            std::unique_lock<std::mutex> lock(client->mutex);
            client->cv.wait(lock, [client] {
                return !client->alive.load() || !client->queue.empty();
            });

            if (!client->alive.load() || client->queue.empty())
                break;

            chunk = std::move(client->queue.front());
            client->queue.pop_front();
            client->queued_bytes -= chunk->size();
        }

        size_t offset = 0;
        while (offset < chunk->size())
        {
            ssize_t sent = ::send(client->fd, chunk->data() + offset,
                chunk->size() - offset, MSG_NOSIGNAL);

            if (sent > 0)
            {
                offset += static_cast<size_t>(sent);
                continue;
            }

            if (sent < 0 && errno == EINTR)
                continue;

            client->alive.store(false);
            break;
        }
    }

    client->alive.store(false);
    client->cv.notify_all();
}

void TcpServer::reap_clients()
{
    std::vector<std::unique_ptr<Client>> dead;

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto it = clients_.begin(); it != clients_.end();)
        {
            if (!(*it)->alive.load())
            {
                dead.push_back(std::move(*it));
                it = clients_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& client : dead)
    {
        if (client->thread.joinable())
            client->thread.join();
    }

    if (!dead.empty())
    {
        std::cerr << "Client disconnected (" << client_count()
            << " total)" << std::endl;
    }
}

bool TcpServer::write(const uint8_t* data, size_t size)
{
    if (size == 0)
        return true;

    // Avoid copying/allocating when nobody is listening.
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        if (clients_.empty())
            return true;
    }

    // One copy for the whole broadcast; every client shares the same buffer.
    auto chunk = std::make_shared<const std::vector<uint8_t>>(data, data + size);

    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (auto& client : clients_)
    {
        std::lock_guard<std::mutex> client_lock(client->mutex);

        if (!client->alive.load())
            continue;

        // Always accept a chunk when the queue is empty, so a single large
        // keyframe cannot disconnect an otherwise healthy client.
        if (!client->queue.empty() &&
            client->queued_bytes + size > MAX_CLIENT_QUEUE_BYTES)
        {
            client->alive.store(false);
            client->cv.notify_all();
            continue;
        }

        client->queue.push_back(chunk);
        client->queued_bytes += size;
        client->cv.notify_one();
    }

    return true;
}

void TcpServer::request_keyframe()
{
    keyframe_requested_.store(true);
}

bool TcpServer::consume_keyframe_request()
{
    return keyframe_requested_.exchange(false);
}

size_t TcpServer::client_count() const
{
    std::lock_guard<std::mutex> lock(clients_mutex_);
    size_t count = 0;
    for (const auto& client : clients_)
    {
        if (client->alive.load())
            ++count;
    }
    return count;
}
