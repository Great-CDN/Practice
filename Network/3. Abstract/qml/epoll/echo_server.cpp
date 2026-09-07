// Linux TCP 回射服务端：业务层只依赖 IEventEngine，不直接调用 epoll。
#include "epoll_engine.h"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

enum { SERV_PORT = 9877, LISTENQ = 16, MAXLINE = 4096 };

// id 为 0 表示服务器日志，否则输出连接编号和对端地址。
static void LogLine(long id, const char* peer, const char* format, ...) {
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

    if (id == 0) {
        printf("[%s] [server] [-] %s\n", timestamp, message);
    } else {
        printf("[%s] [conn %ld] [%s] %s\n", timestamp, id, peer, message);
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
    snprintf(peer, sizeof(peer), "%s:%u", ip,
             static_cast<unsigned int>(ntohs(address.sin_port)));
    return peer;
}

// 每条连接拥有一个 EchoSession。它只通过公共接口增删读写关注项。
class EchoSession : public IIoHandler {
public:
    EchoSession(IEventEngine& engine, FD fd, long id, const std::string& peer)
        : engine_(engine), fd_(fd), id_(id), peer_(peer), reads_(0),
          read_closed_(false) {}

    void LogConnected() const {
        LogLine(id_, peer_.c_str(), "connected");
    }

    void OnRead(FD, void*, std::int32_t) override {
        char buffer[MAXLINE];

        // 非阻塞 recv 要持续到 EAGAIN，取完本次已经到达的所有数据。
        for (;;) {
            const ssize_t received = recv(fd_, buffer, sizeof(buffer), 0);
            if (received > 0) {
                ++reads_;
                out_.append(buffer, static_cast<size_t>(received));
                LogLine(id_, peer_.c_str(), "recv #%lu %zd bytes: \"%.*s\"",
                        reads_, received, static_cast<int>(received), buffer);
                continue;
            }
            if (received == 0) {
                read_closed_ = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;

            perror("recv");
            Close("recv error");
            return;
        }

        // 先增加写关注，再在半关闭时移除读关注，避免中间把 fd 完全移出引擎。
        if (!out_.empty() &&
            engine_.AddIoEvent(fd_, kWriteEvent, this, nullptr) != kOk) {
            Close("add write event failed");
            return;
        }

        if (read_closed_) {
            engine_.DeleteIoEvent(fd_, kReadEvent);
            // 对端关闭发送方向后，仍要把已经收到的数据完整回射。
            if (out_.empty()) Close("EOF by peer");
        }
    }

    void OnWrite(FD, void*, std::int32_t) override {
        while (!out_.empty()) {
            const ssize_t written =
                send(fd_, out_.data(), out_.size(), MSG_NOSIGNAL);
            if (written > 0) {
                out_.erase(0, static_cast<size_t>(written));
                LogLine(id_, peer_.c_str(), "send %zd bytes (%zu bytes pending)",
                        written, out_.size());
                continue;
            }
            if (written == -1 && errno == EINTR) continue;
            if (written == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return;
            }

            if (written == -1) perror("send");
            Close("send error");
            return;
        }

        // LT 模式下空缓冲仍关注可写会导致事件循环持续空转。
        engine_.DeleteIoEvent(fd_, kWriteEvent);
        if (read_closed_) Close("EOF by peer");
    }

private:
    void Close(const char* reason) {
        LogLine(id_, peer_.c_str(), "closed (%s)", reason);
        engine_.DeleteIoEvent(fd_, kReadEvent | kWriteEvent);
        close(fd_);

        // 会话由自己管理生命周期。引擎在两个回调之间会重新查表，
        // 因而删除注册项后不会再次调用这个对象。
        delete this;
    }

    IEventEngine& engine_;
    FD fd_;
    long id_;
    std::string peer_;
    std::string out_;
    unsigned long reads_;
    bool read_closed_;
};

class EchoServer : public IIoHandler {
public:
    EchoServer(IEventEngine& engine, FD listenfd)
        : engine_(engine), listenfd_(listenfd), next_id_(1) {}

    void OnRead(FD, void*, std::int32_t) override {
        // 监听 socket 是非阻塞的，一次通知可能对应多个待接收连接。
        for (;;) {
            sockaddr_in client_address{};
            socklen_t address_length = sizeof(client_address);
            const int fd = accept(listenfd_,
                                  reinterpret_cast<sockaddr*>(&client_address),
                                  &address_length);
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

            EchoSession* const session =
                new EchoSession(engine_, fd, next_id_++, PeerName(client_address));
            if (engine_.AddIoEvent(fd, kReadEvent, session, nullptr) != kOk) {
                fprintf(stderr, "无法把客户端 fd %d 注册到事件引擎\n", fd);
                close(fd);
                delete session;
                continue;
            }
            session->LogConnected();
        }
    }

    // 监听 socket 只注册读事件，不会调用这个空实现。
    void OnWrite(FD, void*, std::int32_t) override {}

private:
    IEventEngine& engine_;
    FD listenfd_;
    long next_id_;
};

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. 创建并配置非阻塞 IPv4 TCP 监听 socket。
    const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    const int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) == -1) {
        perror("setsockopt");
        close(listenfd);
        return EXIT_FAILURE;
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERV_PORT);
    if (bind(listenfd, reinterpret_cast<const sockaddr*>(&server_address),
             sizeof(server_address)) == -1) {
        perror("bind");
        close(listenfd);
        return EXIT_FAILURE;
    }
    if (listen(listenfd, LISTENQ) == -1) {
        perror("listen");
        close(listenfd);
        return EXIT_FAILURE;
    }
    if (!SetNonblocking(listenfd)) {
        close(listenfd);
        return EXIT_FAILURE;
    }

    // 2. 业务层只负责注册监听 fd；等待和分发都由事件引擎完成。
    EpollEngine engine;
    EchoServer server(engine, listenfd);
    if (engine.AddIoEvent(listenfd, kReadEvent, &server, nullptr) != kOk) {
        fprintf(stderr, "无法把监听 fd 注册到 epoll 引擎\n");
        close(listenfd);
        return EXIT_FAILURE;
    }

    LogLine(0, nullptr, "listening on 0.0.0.0:%d (epoll engine)", SERV_PORT);
    const ErrCode result = engine.Run();

    engine.DeleteIoEvent(listenfd, kReadEvent);
    close(listenfd);
    return result == kOk ? EXIT_SUCCESS : EXIT_FAILURE;
}
