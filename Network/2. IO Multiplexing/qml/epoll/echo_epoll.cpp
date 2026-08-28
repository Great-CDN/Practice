
// epoll 相比 select 的三个本质改进：
//   1. fd 经 epoll_ctl 一次性注册进内核，不用每轮重建 fd 集合并整体拷入内核
//   2. epoll_wait 返回的直接是就绪列表，免去 O(n) 逐个 FD_ISSET 扫描
//   3. 无 FD_SETSIZE 上限
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdint>
#include <string>
#include <map>

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

enum { SERV_PORT = 9877, LISTENQ = 16, MAXLINE = 4096, MAX_EVENTS = 64 };

struct Conn {
    long          id;      // 连接编号：区分不同客户端
    std::string   peer;    // 对端 "ip:port"
    unsigned long msgs;    // 请求编号：区分同一连接的不同请求
    std::string   out;     // 待发送缓冲：send 没发完的字节暂存，写就绪后续发（支持分段发送）
};

static std::map<int, Conn> g_conns;
static long g_next_id = 1;
static int  g_epfd    = -1;   // epoll 实例（内核事件表）的句柄

// 标准化日志：[时间] [conn 编号] [对端地址] 内容；c == NULL 表示服务器自身事件
static void logmsg(const Conn* c, const char* fmt, ...) {
    char ts[32], msg[1024];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (c)
        printf("[%s] [conn %ld] [%s] %s\n", ts, c->id, c->peer.c_str(), msg);
    else
        printf("[%s] [server] [-] %s\n", ts, msg);
    fflush(stdout);
}

static void die(const char* what) {
    fprintf(stderr, "[FATAL] %s failed: %s\n", what, strerror(errno));
    exit(EXIT_FAILURE);
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) die("fcntl");
}

// 按当前 out 状态刷新该 fd 关注的事件：始终关注 EPOLLIN，out 有存货才加 EPOLLOUT
// 关键坑：out 为空时绝不能订阅 EPOLLOUT——LT 模式下内核缓冲只要有空位就一直报
// "可写"，事件循环会被空转打死循环烧 CPU
static void update_interest(int fd, const Conn& c) {
    epoll_event ev{};
    ev.events   = EPOLLIN | (c.out.empty() ? 0 : EPOLLOUT);
    ev.data.fd  = fd;
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &ev);
}

// 尽力把 out 缓冲发完。返回 1=已清空，0=内核缓冲满待续发，-1=连接出错
static int flush_out(int fd, Conn& c) {
    while (!c.out.empty()) {
        ssize_t n = send(fd, c.out.data(), c.out.size(), 0);
        if (n > 0) {
            logmsg(&c, "send  %zd bytes (%zu bytes pending)", n, c.out.size() - (size_t)n);
            c.out.erase(0, (size_t)n);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return 1;
}

// close(fd) 时内核会自动把 fd 从 epoll 摘除，无需显式 EPOLL_CTL_DEL
static void close_conn(int fd, const char* reason) {
    auto it = g_conns.find(fd);
    if (it == g_conns.end()) return;
    logmsg(&it->second, "closed (%s)", reason);
    close(fd);
    g_conns.erase(it);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) die("socket");

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in servaddr{};
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons(SERV_PORT);
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof servaddr) < 0) die("bind");
    if (listen(listenfd, LISTENQ) < 0) die("listen");
    set_nonblock(listenfd);

    g_epfd = epoll_create1(0);
    if (g_epfd < 0) die("epoll_create1");

    // 监听 fd 只需注册这一次（select 每轮都要重建集合，这是两者的结构差异）
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listenfd;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, listenfd, &ev) < 0) die("epoll_ctl ADD listenfd");

    // 忽略 SIGPIPE：向已关闭的连接 send 默认会触发它杀死进程，
    // 忽略后 send 返回 -1/EPIPE，走正常错误处理
    signal(SIGPIPE, SIG_IGN);

    logmsg(NULL, "listening on 0.0.0.0:%d (epoll model, level-triggered)", SERV_PORT);

    epoll_event evs[MAX_EVENTS];
    for (;;) {
        // 返回值 n = 就绪 fd 个数，evs[0..n) 就是就绪列表，只遍历它们即可
        int n = epoll_wait(g_epfd, evs, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("epoll_wait");
        }

        for (int i = 0; i < n; ++i) {
            int      fd = evs[i].data.fd;
            uint32_t re = evs[i].events;

            // (1) 监听 fd 就绪 = 有新连接
            if (fd == listenfd) {
                for (;;) {   // 非阻塞 accept 循环取空队列，取完返回 EAGAIN
                    sockaddr_in cliaddr{};
                    socklen_t clilen = sizeof cliaddr;
                    int cfd = accept(listenfd, (sockaddr*)&cliaddr, &clilen);
                    if (cfd < 0) break;

                    set_nonblock(cfd);

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof ip);
                    char peer[64];
                    snprintf(peer, sizeof peer, "%s:%u", ip, (unsigned)ntohs(cliaddr.sin_port));

                    Conn c;
                    c.id   = g_next_id++;
                    c.msgs = 0;
                    c.peer = peer;
                    g_conns[cfd] = c;

                    epoll_event cev{};
                    cev.events  = EPOLLIN;   // 新连接先只关注可读
                    cev.data.fd = cfd;
                    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &cev) < 0) die("epoll_ctl ADD conn");
                    logmsg(&g_conns[cfd], "connected");
                }
                continue;
            }

            // (2) 已连接 fd 的事件
            auto it = g_conns.find(fd);
            if (it == g_conns.end()) continue;   // 连接刚被处理掉，事件已过期
            Conn& c = it->second;

            if (re & (EPOLLERR | EPOLLHUP)) { close_conn(fd, "error/hup by kernel"); continue; }

            if (re & EPOLLIN) {
                char buf[MAXLINE];
                ssize_t n = recv(fd, buf, sizeof buf, 0);
                if (n > 0) {
                    // TCP 是字节流：读到的可能只是半截消息，回射服务器照单转发即可
                    logmsg(&c, "recv  #%lu %zu bytes: \"%.*s\"", ++c.msgs, (size_t)n, (int)n, buf);
                    c.out.append(buf, (size_t)n);
                } else if (n == 0) {
                    close_conn(fd, "EOF by peer");
                    continue;
                } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    close_conn(fd, "recv error");   // 非阻塞下 EAGAIN 只是暂时没数据，不算错误
                    continue;
                }
            }

            // 刚收到数据或 EPOLLOUT 就绪，都尝试清空 out
            if (!c.out.empty() && flush_out(fd, c) < 0) {
                close_conn(fd, "send error");
                continue;
            }

            // out 状态可能变了（清空/新增），同步刷新关注事件
            update_interest(fd, c);
        }
    }
}
