// TCP 回射客户端 —— 配合 my_echo.cpp / echo_select.cpp 的服务器使用
// 用法：client.exe [服务器IP] [--split]
//   --split：把一行输入按 3 字节一段、间隔 200ms 分多次 send 发出，
//            模拟"同一连接分段发送"，验证服务器的非阻塞回射能力
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
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
#  include <time.h>
   typedef int SOCKET;
#  define INVALID_SOCKET (-1)
#  define SOCKET_ERROR (-1)
#  define CLOSE(s) close(s)
#endif

enum { SERV_PORT = 9877, MAXLINE = 4096 };

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

static void err(const char* msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

// UNP 的 Writen：send 可能只发出一部分，循环直到 n 字节全部发出（阻塞 socket）
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
    SetConsoleOutputCP(CP_UTF8);        // 控制台切到 UTF-8，中文显示不乱码
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) err("WSAStartup");
#endif

    // 解析命令行：IP 之外的参数里出现 --split 则启用分段发送
    const char* servip = "127.0.0.1";
    int split = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--split") == 0) split = 1;
        else servip = argv[i];
    }

    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCKET) err("socket");

    sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof servaddr);
    servaddr.sin_family = AF_INET;
    servaddr.sin_port   = htons(SERV_PORT);
    if (inet_pton(AF_INET, servip, &servaddr.sin_addr) != 1) err("inet_pton");

    // 客户端不需要 bind/listen/accept，直接发起连接（端口由内核自动分配）
    if (connect(sockfd, (sockaddr*)&servaddr, sizeof servaddr) == SOCKET_ERROR) err("connect");

    printf("已连接到服务器 %s:%d%s，输入内容回车发送，输入 exit 退出\n",
           servip, SERV_PORT, split ? "（分段发送模式）" : "");

    char sendline[MAXLINE], recvline[MAXLINE];
    for (;;) {
        printf("[你输入] ");
        fflush(stdout);
        if (fgets(sendline, MAXLINE, stdin) == NULL) break;
        sendline[strcspn(sendline, "\r\n")] = '\0';     // 去掉行尾换行符
        if (sendline[0] == '\0') continue;
        if (strcmp(sendline, "exit") == 0) break;

        int len = (int)strlen(sendline);

        // 分段发送：每段 3 字节，间隔 200ms，验证服务器对半截消息的处理
        if (split) {
            for (int off = 0; off < len; off += 3) {
                int k = (len - off < 3) ? (len - off) : 3;
                Writen(sockfd, sendline + off, k);
                printf("[分段发送] %d 字节: \"%.*s\"\n", k, k, sendline + off);
                sleep_ms(200);
            }
        } else {
            Writen(sockfd, sendline, len);
        }

        // 收满 len 字节为止：服务器可能分多段回射，也可能合并成一段
        int got = 0;
        while (got < len) {
            int n = recv(sockfd, recvline, MAXLINE, 0);
            if (n == SOCKET_ERROR) err("recv");
            if (n == 0) { printf("服务器已关闭连接\n"); goto done; }
            printf("[服务器回射] %.*s\n", n, recvline);
            got += n;
        }
    }
done:
    CLOSE(sockfd);
    printf("已断开连接\n");
    return 0;
}
