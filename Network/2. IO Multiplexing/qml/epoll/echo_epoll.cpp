// Linux TCP 回射服务端（epoll 版）：使用一个线程同时服务多个客户端。
// 本例采用水平触发（LT）；所有 socket 都是非阻塞的。
// 主循环：epoll_wait 取得就绪列表 -> accept/recv/send -> 更新关注事件。
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <utility>

#include <arpa/inet.h>  // inet_ntop
#include <fcntl.h>     // fcntl、O_NONBLOCK
#include <netinet/in.h>
#include <sys/epoll.h> // epoll_create1、epoll_ctl、epoll_wait
#include <sys/socket.h>
#include <unistd.h>

enum { SERV_PORT = 9877, LISTENQ = 16, MAXLINE = 4096, MAX_EVENTS = 64 };

struct Conn {
    long id;               // 日志中的连接编号
    std::string peer;      // 对端地址，格式为 "ip:port"
    unsigned long reads;   // 本连接成功 recv 的次数
    std::string out;       // 尚未发送完的回射数据
    bool read_closed;      // 对端是否已关闭发送方向
};

static std::map<int, Conn> g_conns;
static long g_next_id = 1;

static void err(const char* message) {
    perror(message);
    exit(EXIT_FAILURE);
}

static void logmsg(const Conn* conn, const char* format, ...) {
    char timestamp[32] = "unknown-time";
    const time_t now = time(nullptr);
    tm local{};
    if (localtime_r(&now, &local) != nullptr) {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &local);
    }

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (conn != nullptr) {
        printf("[%s] [conn %ld] [%s] %s\n", timestamp, conn->id, conn->peer.c_str(), message);
    } else {
        printf("[%s] [server] [-] %s\n", timestamp, message);
    }
}

static bool SetNonblocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        return false;
    }
    return true;
}

static std::string PeerName(const sockaddr_in& address) {
    char ip[INET_ADDRSTRLEN] = "unknown";
    if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr) {
        perror("inet_ntop");
    }

    char peer[64];
    snprintf(peer, sizeof(peer), "%s:%u", ip, static_cast<unsigned int>(ntohs(address.sin_port)));
    return peer;
}

// EPOLLIN 始终用于接收数据；EPOLLRDHUP 用于发现 TCP 半关闭。
// 仅在确有待发送数据时订阅 EPOLLOUT，否则“通常可写”的 socket 会令事件循环空转。
static bool UpdateInterest(int epfd, int fd, const Conn& conn) {
    epoll_event event{};
    if (!conn.read_closed) event.events |= EPOLLIN | EPOLLRDHUP;
    if (!conn.out.empty()) event.events |= EPOLLOUT;
    event.data.fd = fd;

    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event) == -1) {
        perror("epoll_ctl MOD connection");
        return false;
    }
    return true;
}

// 尽力把待发送缓冲区清空；EAGAIN 表示暂时写满，留给后续 EPOLLOUT 事件。
static bool FlushOutput(int fd, Conn& conn) {
    while (!conn.out.empty()) {
        const ssize_t written = send(fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);
        if (written > 0) {
            conn.out.erase(0, static_cast<size_t>(written));
            logmsg(&conn, "send %zd bytes (%zu bytes pending)", written, conn.out.size());
            continue;
        }
        if (written == -1 && errno == EINTR) continue;
        if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;

        if (written == 0) {
            fprintf(stderr, "send: 未能继续发送数据\n");
        } else {
            perror("send");
        }
        return false;
    }
    return true;
}

static void CloseConnection(int fd, const char* reason) {
    const auto it = g_conns.find(fd);
    if (it == g_conns.end()) return;

    logmsg(&it->second, "closed (%s)", reason);
    // Linux 会在 close 时自动把 fd 从 epoll 实例移除，无需再调用 EPOLL_CTL_DEL。
    close(fd);
    g_conns.erase(it);
}

static bool ReadAvailable(int fd, Conn& conn) {
    char buffer[MAXLINE];
    for (;;) {
        const ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received > 0) {
            ++conn.reads;
            conn.out.append(buffer, static_cast<size_t>(received));
            logmsg(&conn, "recv #%lu %zd bytes: \"%.*s\"", conn.reads, received,
                   static_cast<int>(received), buffer);
            continue;
        }
        if (received == 0) {
            conn.read_closed = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;

        perror("recv");
        return false;
    }
}

// 监听 fd 在 epoll 中就绪后，排空 accept 队列，并把每个客户端登记到 epoll。
static void AcceptConnections(int epfd, int listenfd) {
    for (;;) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        const int fd = accept(listenfd, reinterpret_cast<sockaddr*>(&client_address), &address_length);
        if (fd == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            perror("accept");
            return;
        }

        if (!SetNonblocking(fd)) {
            close(fd);
            continue;
        }

        Conn conn{};
        conn.id = g_next_id++;
        conn.peer = PeerName(client_address);
        conn.reads = 0;
        conn.read_closed = false;
        const auto inserted = g_conns.emplace(fd, std::move(conn));

        epoll_event event{};
        event.events = EPOLLIN | EPOLLRDHUP;
        event.data.fd = fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event) == -1) {
            perror("epoll_ctl ADD connection");
            close(fd);
            g_conns.erase(inserted.first);
            continue;
        }
        logmsg(&inserted.first->second, "connected");
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. 创建并配置非阻塞 IPv4 TCP 监听 socket。
    const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) err("socket");

    const int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        err("setsockopt");
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERV_PORT);
    if (bind(listenfd, reinterpret_cast<const sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        err("bind");
    }
    if (listen(listenfd, LISTENQ) == -1) err("listen");
    if (!SetNonblocking(listenfd)) {
        close(listenfd);
        return EXIT_FAILURE;
    }

    // EPOLL_CLOEXEC 防止未来 exec 其他程序时意外继承 epoll 描述符。
    const int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd == -1) err("epoll_create1");

    // 监听 fd 只登记一次；后续 epoll_wait 会直接返回就绪的描述符列表。
    epoll_event listen_event{};
    listen_event.events = EPOLLIN;
    listen_event.data.fd = listenfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &listen_event) == -1) {
        err("epoll_ctl ADD listenfd");
    }

    logmsg(nullptr, "listening on 0.0.0.0:%d (epoll, level-triggered)", SERV_PORT);

    // 2. epoll_wait 只返回真正就绪的项目，不需要像 select 那样扫描全部连接。
    epoll_event events[MAX_EVENTS];
    for (;;) {
        const int ready = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (ready == -1) {
            if (errno == EINTR) continue;
            err("epoll_wait");
        }

        for (int index = 0; index < ready; ++index) {
            const int fd = events[index].data.fd;
            const uint32_t occurred = events[index].events;

            if (fd == listenfd) {
                AcceptConnections(epfd, listenfd);
                continue;
            }

            auto it = g_conns.find(fd);
            if (it == g_conns.end()) continue; // 同一批中的过期事件

            if ((occurred & EPOLLERR) != 0) {
                CloseConnection(fd, "socket error");
                continue;
            }

            // HUP/RDHUP 时仍先 recv，避免丢掉 FIN 之前已经到达的数据。
            if ((occurred & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) {
                if (!ReadAvailable(fd, it->second)) {
                    CloseConnection(fd, "recv error");
                    continue;
                }
                if ((occurred & (EPOLLRDHUP | EPOLLHUP)) != 0) it->second.read_closed = true;
            }

            // 新收到数据或写事件到达时，都立即尝试发送。
            if (!it->second.out.empty() && !FlushOutput(fd, it->second)) {
                CloseConnection(fd, "send error");
                continue;
            }

            if (it->second.read_closed && it->second.out.empty()) {
                CloseConnection(fd, "EOF by peer");
                continue;
            }

            // out 是否为空可能刚发生变化，因此同步修改下一轮关注的事件。
            if (!UpdateInterest(epfd, fd, it->second)) {
                CloseConnection(fd, "epoll_ctl error");
            }
        }
    }
}
