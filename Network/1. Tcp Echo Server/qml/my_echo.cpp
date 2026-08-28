// TCP 回射服务器 —— 《UNIX网络编程》卷一 5.3 节练习
// 同步阻塞 IO，单线程串行服务多个客户端
// 流程：socket -> bind -> listen -> accept -> recv/send -> close
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   typedef int socklen_t;
#  define CLOSE(s) closesocket(s)
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define CLOSE(s) close(s)
#endif

enum { SERV_PORT = 9877, LISTENQ = 8, MAXLINE = 4096 };

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);   // 实时打印日志

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) err("WSAStartup");
#endif

    // 1. 创建监听 socket：IPv4 + 字节流 = TCP
    SOCKET listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == INVALID_SOCKET) err("socket");

    // 2. 绑定地址和端口（htonl/htons 转网络字节序）
    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERV_PORT);
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) err("bind");

    // 3. 进入监听状态，LISTENQ 是排队长度
    if (listen(listenfd, LISTENQ) == SOCKET_ERROR) err("listen");
    printf("[server] listening on 0.0.0.0:%d ...\n", SERV_PORT);

    for (;;) {
        // 4. 阻塞等待客户端连接，clilen 必须先初始化
        sockaddr_in cliaddr{};
        socklen_t clilen = sizeof(cliaddr);
        SOCKET connfd = accept(listenfd, (sockaddr*)&cliaddr, &clilen);
        if (connfd == INVALID_SOCKET) err("accept");

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof(ip));
        unsigned short port = ntohs(cliaddr.sin_port);
        printf("[accept] %s:%u connected\n", ip, port);

        // 5. 回射循环：recv > 0 收到数据原样发回；== 0 客户端关闭
        char buf[MAXLINE];
        int n;
        while ((n = recv(connfd, buf, MAXLINE, 0)) > 0) {
            printf("[recv  ] %s:%u : \"%.*s\"\n", ip, port, n, buf);
            Writen(connfd, buf, n);
            printf("[echo  ] %s:%u : \"%.*s\"\n", ip, port, n, buf);
        }
        if (n == SOCKET_ERROR) err("recv");

        // 6. 客户端断开，关闭连接，回到 accept 等待下一个客户
        printf("[close ] %s:%u disconnected\n", ip, port);
        CLOSE(connfd);
    }
}
static void err(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// UNP 的 Writen：send 可能只发出一部分，循环直到 n 字节全部发出
static void Writen(SOCKET fd, const char* buf, int n) {
    while (n > 0) {
        int w = send(fd, buf, n, 0);
        if (w == SOCKET_ERROR || w == 0) err("send");
        buf += w;
        n -= w;
    }
}