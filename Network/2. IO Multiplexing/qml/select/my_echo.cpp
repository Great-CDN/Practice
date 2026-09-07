// Linux TCP 回射服务端（select 版）：使用一个线程同时服务多个客户端。
// 所有 socket 都是非阻塞的；select 负责等待连接、读和写事件。
// 主循环：重建 fd 集合 -> select -> accept/recv/send -> 下一轮。
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <arpa/inet.h>   // inet_ntop
#include <fcntl.h>      // fcntl、O_NONBLOCK
#include <netinet/in.h> // sockaddr_in、htons、htonl、ntohs
#include <sys/select.h> // select、fd_set、FD_* 宏
#include <sys/socket.h> // socket、bind、listen、accept、recv、send
#include <unistd.h>     // close

enum { SERV_PORT = 9877, LISTENQ = 16, MAXLINE = 4096 };

// 一个 Conn 保存一条客户端连接跨越多轮 select 所需的状态。
struct Conn {
    long id;               // 日志中的连接编号
    std::string peer;      // 对端地址，格式为 "ip:port"
    unsigned long reads;   // 本连接成功 recv 的次数
    std::string out;       // 尚未发送完的回射数据
    bool read_closed;      // 对端是否已关闭发送方向
};

static std::map<int, Conn> g_conns; // Linux 文件描述符 -> 连接状态
static long g_next_id = 1;

static void err(const char* message) {
    perror(message);
    exit(EXIT_FAILURE);
}

// 输出统一格式的日志；conn == nullptr 表示服务器自身的事件。
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

// 非阻塞 socket 在暂时不能继续 accept/recv/send 时返回 -1，并把 errno
// 设为 EAGAIN 或 EWOULDBLOCK，使事件循环可以继续服务其他连接。
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

// 尽可能发送 out 中的所有数据。
// 返回 true 表示连接仍可用；若内核发送缓冲区已满，未发送部分保留到下一轮。
static bool FlushOutput(int fd, Conn& conn) {
    while (!conn.out.empty()) {
        // MSG_NOSIGNAL 防止对端断开时 SIGPIPE 直接终止整个服务端。
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
    close(fd);
    g_conns.erase(it);
}

// 监听 socket 可读时，循环 accept，直到已完成握手的连接队列被取空。
static void AcceptConnections(int listenfd) {
    for (;;) {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        const int fd = accept(listenfd, reinterpret_cast<sockaddr*>(&client_address), &address_length);
        if (fd == -1) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            perror("accept");
            return; // 单次 accept 失败不应终止已有连接
        }

        // select 的 fd_set 只能表示 [0, FD_SETSIZE) 内的描述符。
        if (fd >= FD_SETSIZE) {
            fprintf(stderr, "[reject] fd %d exceeds FD_SETSIZE=%d\n", fd, FD_SETSIZE);
            close(fd);
            continue;
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
        logmsg(&inserted.first->second, "connected");
    }
}

// 非阻塞 recv 要一直读到 EAGAIN，确保一次可读通知中的数据被取完。
// 返回 false 表示连接发生了不可恢复的读取错误。
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
            // 保留已经收到的待发送数据；全部回射后再关闭连接。
            conn.read_closed = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;

        perror("recv");
        return false;
    }
}

int main() {
    // 关闭 stdout 缓冲，让 Linux 控制台立即显示事件循环日志。
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. 创建并配置非阻塞 IPv4 TCP 监听 socket。
    const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) err("socket");
    if (listenfd >= FD_SETSIZE) {
        fprintf(stderr, "listen fd %d exceeds FD_SETSIZE=%d\n", listenfd, FD_SETSIZE);
        close(listenfd);
        return EXIT_FAILURE;
    }

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

    logmsg(nullptr, "listening on 0.0.0.0:%d (select, FD_SETSIZE=%d)", SERV_PORT, FD_SETSIZE);

    // 2. 事件循环。select 会修改传入的集合，所以每轮都必须重新构造。
    for (;;) {
        fd_set readfds;
        fd_set writefds;
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(listenfd, &readfds);
        int maxfd = listenfd;

        // 快照既用于构造集合，也避免处理事件时删除 map 元素导致迭代器失效。
        std::vector<int> fds;
        fds.reserve(g_conns.size());
        for (const auto& item : g_conns) {
            const int fd = item.first;
            const Conn& conn = item.second;
            fds.push_back(fd);
            if (!conn.read_closed) FD_SET(fd, &readfds);
            if (!conn.out.empty()) FD_SET(fd, &writefds);
            if (fd > maxfd) maxfd = fd;
        }

        // Linux 要求第一个参数是“最大描述符 + 1”，否则内核不会检查这些描述符。
        const int ready = select(maxfd + 1, &readfds, &writefds, nullptr, nullptr);
        if (ready == -1) {
            if (errno == EINTR) continue;
            err("select");
        }

        if (FD_ISSET(listenfd, &readfds)) AcceptConnections(listenfd);

        // 3. 只处理本轮 select 前已经登记的客户端。
        for (const int fd : fds) {
            auto it = g_conns.find(fd);
            if (it == g_conns.end()) continue;

            const bool readable = FD_ISSET(fd, &readfds);
            const bool writable = FD_ISSET(fd, &writefds);
            if (readable && !ReadAvailable(fd, it->second)) {
                CloseConnection(fd, "recv error");
                continue;
            }

            // 刚读到的数据可以立即尝试发送；不必等下一轮 select 报告可写。
            if (!it->second.out.empty() && (readable || writable) && !FlushOutput(fd, it->second)) {
                CloseConnection(fd, "send error");
                continue;
            }

            // TCP 半关闭后，先回射最后收到的数据，再释放连接。
            if (it->second.read_closed && it->second.out.empty()) {
                CloseConnection(fd, "EOF by peer");
            }
        }
    }
}
