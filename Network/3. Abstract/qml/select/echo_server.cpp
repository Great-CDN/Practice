#include "select_engine.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

enum { SERV_PORT = 9877, LISTENQ = 16, MAXLINE = 4096 };

// 标准化日志：[时间] [conn 编号] [对端地址] 内容
static void LogLine(long id, const char* peer, const char* fmt, ...) {
    char ts[32], msg[1024];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%s] [conn %ld] [%s] %s\n", ts, id, peer, msg);
    fflush(stdout);
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        fprintf(stderr, "[FATAL] fcntl failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

class EchoSession : public IIoHandler {
public:
    EchoSession(SelectEngine& engine, FD fd, long id, const std::string& peer)
        : engine_(engine), fd_(fd), id_(id), peer_(peer) {
        LogLine(id_, peer_.c_str(), "connected");
    }

    // 可读：对端发来数据（n>0）或关闭连接（n=0）
    void OnRead(FD fd, void* data, int32_t mask) override {
        (void)data; (void)mask;   // 本例用不上，引擎保证 OnRead 只在可读时被调
        char buf[MAXLINE];
        ssize_t n = recv(fd_, buf, sizeof buf, 0);
        if (n > 0) {
            // TCP 是字节流：读到的可能只是半截消息，回射服务器照单转发即可
            LogLine(id_, peer_.c_str(), "recv  #%lu %zd bytes: \"%.*s\"",
                    ++msgs_, n, (int)n, buf);
            out_.append(buf, (size_t)n);
            // 有待发数据了：向引擎叠加"可写"关注（旧版：FD_SET 到 ws 集合）
            engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
            return;
        }
        if (n == 0) { Close("EOF by peer"); return; }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            Close("recv error");          // 非阻塞下 EAGAIN 只是暂时没数据
    }

    // 可写：把 out 缓冲尽力发完，发完了就撤销可写关注
    void OnWrite(FD fd, void* data, int32_t mask) override {
        (void)data; (void)mask;
        while (!out_.empty()) {
            ssize_t n = send(fd_, out_.data(), out_.size(), 0);
            if (n > 0) {
                LogLine(id_, peer_.c_str(), "send  %zd bytes (%zu bytes pending)",
                        n, out_.size() - (size_t)n);
                out_.erase(0, (size_t)n);
                continue;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;   // 缓冲满，下轮继续
            Close("send error");
            return;
        }
        // 发完了：撤销可写关注（旧版：下一轮不再 FD_SET 到 ws）。
        // 不撤销的话内核缓冲只要有空位就会一直触发可写，空转烧 CPU
        engine_.DeleteIoEvent(fd_, kWriteEvent);
    }

private:
    void Close(const char* reason) {
        LogLine(id_, peer_.c_str(), "closed (%s)", reason);
        engine_.DeleteIoEvent(fd_, kReadEvent | kWriteEvent);
        close(fd_);
        delete this;    // 连接对象随连接一起销毁；引擎回调前会重查 entry，不会悬空
    }

    SelectEngine& engine_;
    FD            fd_;
    long          id_;
    std::string   peer_;
    std::string   out_;      // 待发送缓冲：send 没发完的字节暂存（支持分段发送）
    unsigned long msgs_ = 0; // 请求编号：区分同一连接的不同请求
};


class EchoServer : public IIoHandler {
public:
    EchoServer(SelectEngine& engine, FD listenfd)
        : engine_(engine), listen_fd_(listenfd) {}

    void OnRead(FD fd, void* data, int32_t mask) override {
        (void)fd; (void)data; (void)mask;
        for (;;) {   // 非阻塞 accept 循环取空队列，取完返回 EAGAIN
            sockaddr_in cliaddr{};
            socklen_t clilen = sizeof cliaddr;
            int cfd = accept(listen_fd_, (sockaddr*)&cliaddr, &clilen);
            if (cfd < 0) break;

            set_nonblock(cfd);

            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof ip);
            char peer[64];
            snprintf(peer, sizeof peer, "%s:%u", ip, (unsigned)ntohs(cliaddr.sin_port));

            EchoSession* session = new EchoSession(engine_, cfd, next_id_++, peer);
            engine_.AddIoEvent(cfd, kReadEvent, session, NULL);
        }
    }

    // 监听 socket 永远不会有可写事件，空实现即可
    void OnWrite(FD, void*, int32_t) override {}

private:
    SelectEngine& engine_;
    FD            listen_fd_;
    long          next_id_ = 1;
};

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);   // 向已关闭的连接 send 默认会触发它杀死进程

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return EXIT_FAILURE; }

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in servaddr{};
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(SERV_PORT);
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof servaddr) < 0) { perror("bind"); return EXIT_FAILURE; }
    if (listen(listenfd, LISTENQ) < 0) { perror("listen"); return EXIT_FAILURE; }
    set_nonblock(listenfd);

    // ---- 业务方代码到此为止只有这些：注册监听 fd，然后跑引擎 ----
    SelectEngine engine;
    EchoServer   acceptor(engine, listenfd);
    engine.AddIoEvent(listenfd, kReadEvent, &acceptor, NULL);

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] [server] [-] listening on 0.0.0.0:%d (select engine)\n", ts, SERV_PORT);

    return engine.Run();   // 一切事件都在引擎循环里分发，业务代码再无 while/select
}
