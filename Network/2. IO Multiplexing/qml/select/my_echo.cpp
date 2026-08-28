#include <cstdio>     
#include <cstdlib>    
#include <cstdarg>    
#include <cstring>    
#include <ctime>     
#include <string>     
#include <vector>     
#include <map>        
#define WIN32_LEAN_AND_MEAN   
#include <winsock2.h>         
#include <ws2tcpip.h>         
#pragma comment(lib, "ws2_32.lib")   


enum {
    SERV_PORT = 9877,   // 服务器监听端口（UNP 惯例端口）
    LISTENQ   = 16,     // listen backlog：内核中已完成握手等待 accept 的连接队列长度
    MAXLINE   = 4096    // 单次 recv 的最大字节数
};


typedef int socklen_t;


// 每个客户端连接对应一个 Conn，记录该连接的全部上下文
struct Conn {
    long          id;      // 连接编号：全局从 1 递增，日志里用来区分不同客户端
    std::string   peer;    // 对端地址 "ip:port"，日志展示用
    unsigned long msgs;    // 本连接第几条数据：从 1 递增，区分同一连接的不同请求
    std::string   out;     // 待发送缓冲：send 没发完（内核缓冲满）的字节暂存在这里，
                           // 等 select 报告"可写"后再继续发 —— 这就是对分段发送的支持
};

static std::map<SOCKET, Conn> g_conns;   // 全局连接表：fd -> 连接状态
static long g_next_id = 1;               // 下一个新连接分配到的编号




static void logmsg(const Conn* c, const char* fmt, ...) {
    char ts[32], msg[1024];

    time_t now = time(NULL);                    // 当前日历时间（秒）
    struct tm tm_buf;                           // MSVC 安全版 localtime：结果写入调用者的缓冲，
    localtime_s(&tm_buf, &now);                 // 注意参数顺序与标准 localtime 相反（缓冲在前）
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm_buf);   // 格式化成 "2026-08-28 11:41:07"

    va_list ap;                     // 可变参数：把 fmt 后的参数格式化进 msg
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (c)
        printf("[%s] [conn %ld] [%s] %s\n", ts, c->id, c->peer.c_str(), msg);
    else
        printf("[%s] [server] [-] %s\n", ts, msg);
    fflush(stdout);                 // 立即刷出，日志实时可见（配合下面的无缓冲设置双保险）
}


static void die(const char* what) {
    fprintf(stderr, "[FATAL] %s failed, WSA error %d\n", what, WSAGetLastError());
    exit(EXIT_FAILURE);
}

// 把 socket 设为非阻塞模式：之后 recv/send/accept 不会卡住，

static void set_nonblock(SOCKET fd) {
    u_long mode = 1;                       // 1 = 非阻塞，0 = 阻塞
    if (ioctlsocket(fd, FIONBIO, &mode) != 0) die("ioctlsocket");
}

// 尽力把 out 缓冲里的数据全部发出去
// 返回值： 1 = 已清空；0 = 内核发送缓冲满（WSAEWOULDBLOCK），剩余字节留在 out 里等下轮；
//         -1 = 连接出错（对端 reset 等），调用方应关闭该连接
static int flush_out(SOCKET fd, Conn& c) {
    while (!c.out.empty()) {
        // 尝试把 out 里全部字节一次性发出
        int n = send(fd, c.out.data(), (int)c.out.size(), 0);
        if (n > 0) {
            // 发出了 n 字节：记日志，从缓冲头部删掉已发出的部分，继续尝试发剩余的
            logmsg(&c, "send  %d bytes (%d bytes pending)", n, (int)c.out.size() - n);
            c.out.erase(0, (size_t)n);
            continue;
        }
        // n < 0（理论上 send 不返回 0）：查看具体错误
        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS)
            return 0;              // 内核缓冲满，正常现象：留着慢慢发
        return -1;                 // 真正的错误
    }
    return 1;                      // 循环正常结束 = 全部发完
}

// 关闭一条连接：打日志 -> 关 socket -> 从连接表删除
static void close_conn(SOCKET fd, const char* reason) {
    auto it = g_conns.find(fd);
    if (it == g_conns.end()) return;             // 已被删过，防止重复关闭
    logmsg(&it->second, "closed (%s)", reason);  // 记录关闭原因（EOF/错误）
    closesocket(fd);                             
    g_conns.erase(it);
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);   // 关闭 stdout 缓冲：printf 直接输出，日志实时可见

    //1. 初始化 Winsock
    // Windows 特有：进程第一次使用 socket 前必须先启动 Winsock 库（加载 ws2_32.dll）
    WSADATA wsa;                                  // 接收库初始化信息的结构体
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)    
        die("WSAStartup");                        

    //2. 创建监听 socket
    // AF_INET = IPv4 协议族，SOCK_STREAM = 字节流套接字，第三个参数 0 = 默认协议（即 TCP）
    SOCKET listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == INVALID_SOCKET) die("socket");

    // 设置 SO_REUSEADDR：服务器重启时端口若处于 TIME_WAIT 状态也能立刻重新绑定，
    // 否则要等几分钟才能再次 bind 同一端口
    BOOL on = TRUE;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof on);

    //3. 绑定地址和端口
    sockaddr_in servaddr{};                       // IPv4 地址结构，{} 零初始化
    servaddr.sin_family      = AF_INET;           // 协议族：IPv4
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听本机所有网卡；htonl：主机序 -> 网络序
    servaddr.sin_port        = htons(SERV_PORT);  // 端口 9877；htons：主机序 -> 网络序
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof servaddr) == SOCKET_ERROR) die("bind");

    //4. 进入监听状态
    // LISTENQ 是 backlog：已完成三次握手、等待 accept 的连接排队上限
    if (listen(listenfd, LISTENQ) == SOCKET_ERROR) die("listen");
    set_nonblock(listenfd);    // 监听 socket 也设非阻塞：accept 取完队列里的连接立即返回
    logmsg(NULL, "listening on 0.0.0.0:%d (select model, FD_SETSIZE=%d)", SERV_PORT, FD_SETSIZE);

    //5. 事件循环
    //每轮：收集所有关心的 fd -> select 阻塞等待 -> 逐个处理就绪的 fd
    for (;;) {
        fd_set rs, ws;            // rs = 关心"可读"的集合，ws = 关心"可写"的集合
        FD_ZERO(&rs);             // 清空可读集合
        FD_ZERO(&ws);             // 清空可写集合
        FD_SET(listenfd, &rs);    // 监听 socket"可读" = 有新连接到来
        for (auto& kv : g_conns) {
            FD_SET(kv.first, &rs);            // 每个连接都关心"可读"（对方发数据来/关连接）
            if (!kv.second.out.empty())       // 只有 out 缓冲还有存货，才需要关心"可写"
                FD_SET(kv.first, &ws);        // （缓冲空时订阅可写会永远立即触发，空转 CPU）
        }

        // select 阻塞直到任一 fd 就绪；返回值 = 就绪 fd 的总个数
       
        // 最后一个 NULL = 不设超时，永久等待
        int ready = select(0, &rs, &ws, NULL, NULL);
        if (ready == SOCKET_ERROR) die("select");

        //监听 socket 可读 
        if (FD_ISSET(listenfd, &rs)) {
            for (;;) {   // 循环 accept 把队列取空（一次 select 可能来了多个连接）
                sockaddr_in cliaddr{};                    // 出参：接收对方的 IP 和端口
                socklen_t clilen = sizeof cliaddr;        // 必须先初始化为结构体大小
                SOCKET fd = accept(listenfd, (sockaddr*)&cliaddr, &clilen);
                if (fd == INVALID_SOCKET) break;          // 非阻塞：队列取空即返回 WSAEWOULDBLOCK
                set_nonblock(fd);                         // 新连接也设非阻塞

                // 把对方地址转成可读字符串 "ip:port"
                char ip[INET_ADDRSTRLEN];                
                inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof ip);
                char peer[64];
                snprintf(peer, sizeof peer, "%s:%u", ip, (unsigned)ntohs(cliaddr.sin_port));

                // 登记到连接表，分配连接编号
                Conn c;
                c.id   = g_next_id++;   // 编号从 1 递增：日志里区分不同客户端
                c.msgs = 0;             // 请求计数清零
                c.peer = peer;
                g_conns[fd] = c;
                logmsg(&g_conns[fd], "connected");
            }
        }

        //谁就绪服务谁
        // 先对连接表的 key 做一份快照：下面处理过程中可能 close 并修改 map，
        
        std::vector<SOCKET> fds;
        fds.reserve(g_conns.size());
        for (auto& kv : g_conns) fds.push_back(kv.first);

        for (SOCKET fd : fds) {
            auto it = g_conns.find(fd);
            if (it == g_conns.end()) continue;   // 该连接可能已被前面的分支关闭，跳过
            Conn& c = it->second;

            // (a) 可读事件：对方发来数据，或对方关闭了连接
            if (FD_ISSET(fd, &rs)) {
                char buf[MAXLINE];
                int n = recv(fd, buf, sizeof buf, 0);   // 非阻塞：立即返回
                if (n == 0) {                           // 返回 0 = 对端正常关闭（发了 FIN）
                    close_conn(fd, "EOF by peer");
                    continue;
                }
                if (n < 0) {                            // 返回 -1 = 出错或暂时无数据
                    int e = WSAGetLastError();
                    if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS)
                        continue;                       // 暂时无数据，正常，下一轮再说
                    close_conn(fd, "recv error");       // 其他错误：连接已坏，关闭
                    continue;
                }
                // n > 0：收到 n 字节。
                // 注意：TCP 是字节流没有消息边界，这里读到的可能只是半截消息，
                // 回射服务器无需关心边界，读到什么就原样回什么
                logmsg(&c, "recv  #%lu %d bytes: \"%.*s\"", ++c.msgs, n, n, buf);
                c.out.append(buf, (size_t)n);           // 先追加到待发送缓冲，下面统一发
            }

            // (b) 发送：刚收到数据，或 select 报告"可写"（内核缓冲有空位），都尝试发
            if (!c.out.empty() && flush_out(fd, c) < 0) {
                close_conn(fd, "send error");           // 发送出错：关闭该连接
            }
        }
       
    }
}
