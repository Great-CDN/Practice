// Linux TCP 回射客户端，与 my_echo.cpp 配合使用。
// 同一台 Linux 上运行：./client；连接其他主机：./client 192.168.198.128。
// 调用顺序：socket -> connect -> 读取输入 -> send -> recv -> close。
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>   // inet_pton：把 IPv4 字符串转换为网络字节序地址
#include <netinet/in.h> // sockaddr_in、htons
#include <sys/socket.h> // socket、connect、send、recv
#include <unistd.h>     // close

enum { SERV_PORT = 9877, MAXLINE = 4096 };

static void err(const char* message) {
    perror(message); // 打印系统调用失败时 errno 对应的错误原因
    exit(EXIT_FAILURE);
}

// 循环发送所有字节，处理 send 只发送一部分数据的情况。
static bool Writen(int fd, const char* buffer, size_t count) {
    while (count > 0) {
        // 对端断开时返回错误，而不是让 SIGPIPE 信号直接终止进程。
        const ssize_t written = send(fd, buffer, count, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return false;
        }
        if (written == 0) {
            fprintf(stderr, "send: 未能继续发送数据\n");
            return false;
        }
        buffer += static_cast<size_t>(written);
        count -= static_cast<size_t>(written);
    }
    return true;
}

// 回射服务会原样返回数据，因此发送 count 字节后，也应等待收齐 count 字节。
// 不能假设一次 recv 就能收齐；循环累计，直到完整收到或者连接发生错误。
static bool Readn(int fd, char* buffer, size_t count) {
    size_t total = 0;
    while (total < count) {
        const ssize_t received = recv(fd, buffer + total, count - total, 0);
        if (received == 0) {
            fprintf(stderr, "服务器已关闭连接：本次回射收到 %zu/%zu 字节\n", total, count);
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return false;
        }
        total += static_cast<size_t>(received);
    }
    return true;
}

int main(int argc, char** argv) {
    if (argc > 2) {
        fprintf(stderr, "用法：%s [服务器IPv4地址]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // 不传参数时连接本机；这里的“本机”指运行这个客户端的 Linux 系统。
    // 本示例接收点分十进制 IPv4 地址，不解析域名。
    const char* servip = (argc == 2) ? argv[1] : "127.0.0.1";
    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(SERV_PORT); // 主机字节序 -> 网络字节序
    const int parsed = inet_pton(AF_INET, servip, &servaddr.sin_addr);
    if (parsed == 0) {
        // 地址格式错误时 inet_pton 返回 0，不会设置 errno，不能用 perror 解释。
        fprintf(stderr, "无效的 IPv4 地址：%s\n", servip);
        return EXIT_FAILURE;
    }
    if (parsed == -1) err("inet_pton");

    // 1. 创建 IPv4 TCP socket；Linux socket 是 int 文件描述符，失败返回 -1。
    const int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) err("socket");

    // 2. 发起连接。客户端不必调用 bind；内核自动选择本地地址和临时端口。
    // 阻塞式 connect 成功返回后，TCP 连接已经建立。
    if (connect(sockfd, reinterpret_cast<const sockaddr*>(&servaddr), sizeof(servaddr)) == -1) {
        err("connect");
    }
    printf("已连接到服务器 %s:%d，输入内容回车发送，输入 exit 退出\n", servip, SERV_PORT);

    char sendline[MAXLINE];
    char recvline[MAXLINE];
    int result = EXIT_SUCCESS;
    for (;;) {
        // 3. fgets 最多读取 MAXLINE-1 个字节，并补 '\0'；更长的输入会分多次处理。
        printf("[你输入] ");
        fflush(stdout); // 提示没有换行，需要主动刷新到控制台
        if (fgets(sendline, sizeof(sendline), stdin) == nullptr) {
            if (ferror(stdin)) {
                perror("fgets");
                result = EXIT_FAILURE;
            }
            break; // Ctrl+D / 输入结束，或读取失败
        }

        // 去掉输入末尾的换行符；空输入跳过，exit 只在客户端本地处理。
        sendline[strcspn(sendline, "\r\n")] = '\0';
        if (sendline[0] == '\0') continue;
        if (strcmp(sendline, "exit") == 0) break;

        // 4. 先完整发送，再等待同样长度的回射。长度不包含字符串结束符 '\0'。
        const size_t length = strlen(sendline);
        if (!Writen(sockfd, sendline, length) || !Readn(sockfd, recvline, length)) {
            result = EXIT_FAILURE;
            break;
        }

        // 接收缓冲区没有自动添加 '\0'，按本次实际接收的长度输出。
        printf("[服务器回射] %.*s\n", static_cast<int>(length), recvline);
    }

    // 5. 关闭描述符，释放连接。Linux 不需要 Winsock 初始化或清理步骤。
    close(sockfd);
    printf("已断开连接\n");
    return result;
}
