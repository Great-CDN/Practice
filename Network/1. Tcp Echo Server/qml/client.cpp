// TCP 回射客户端 —— 配合 my_echo.cpp 的服务器使用
// 流程：socket -> connect -> fgets/recv/send -> close
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

enum { SERV_PORT = 9877, MAXLINE = 4096 };

static void err(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void Writen(SOCKET fd, const char* buf, int n) {
    while (n > 0) {
        int w = send(fd, buf, n, 0);
        if (w == SOCKET_ERROR || w == 0) err("send");
        buf += w;
        n -= w;
    }
}

int main(int argc, char** argv) {
#ifdef _WIN32
    system("chcp 65001 >nul");          // 控制台切到 UTF-8，中文显示不乱码
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) err("WSAStartup");
#endif

    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCKET) err("socket");

    // 服务器地址
    const char* servip = (argc > 1) ? argv[1] : "127.0.0.1";
    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERV_PORT);
    if (inet_pton(AF_INET, servip, &servaddr.sin_addr) != 1) err("inet_pton");

    // 客户端不需要 bind/listen/accept，直接发起连接（端口由内核自动分配）
    if (connect(sockfd, (sockaddr*)&servaddr, sizeof(servaddr)) == SOCKET_ERROR) err("connect");

    printf("已连接到服务器 %s:%d，输入内容回车发送，输入 exit 退出\n", servip, SERV_PORT);

    char sendline[MAXLINE], recvline[MAXLINE];
    for (;;) {
        printf("[你输入] ");
        fflush(stdout);
        if (fgets(sendline, MAXLINE, stdin) == NULL) break;
        sendline[strcspn(sendline, "\r\n")] = '\0';     // 去掉行尾换行符
        if (sendline[0] == '\0') continue;
        if (strcmp(sendline, "exit") == 0) break;

        int len = (int)strlen(sendline);
        Writen(sockfd, sendline, len);

        // 阻塞等待服务器回射
        int n = recv(sockfd, recvline, MAXLINE, 0);
        if (n == SOCKET_ERROR) err("recv");
        if (n == 0) { printf("服务器已关闭连接\n"); break; }

        printf("[服务器回射] %.*s\n", n, recvline);
    }

    CLOSE(sockfd);
    printf("已断开连接\n");
    return 0;
}
