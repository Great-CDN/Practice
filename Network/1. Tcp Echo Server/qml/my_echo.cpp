// Linux TCP 回射服务端：收到多少字节，就把这些字节原样发回。
// 使用 IPv4、阻塞式 I/O；同一时刻只处理一个客户端。
// 调用顺序：socket -> setsockopt -> bind -> listen -> accept -> recv/send -> close。
#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <arpa/inet.h>   // inet_ntop：把二进制 IP 地址转换为可读字符串
#include <netinet/in.h> // sockaddr_in、htons、htonl、ntohs
#include <sys/socket.h> // socket、bind、listen、accept、recv、send
#include <unistd.h>     // close：关闭 Linux 文件描述符

enum { SERV_PORT = 9877, LISTENQ = 8, MAXLINE = 4096 };

// 启动阶段遇到错误时，打印 errno 对应的原因并退出。
// 函数放在 main 前面，保证编译器在使用它之前已经看到定义。
static void err(const char* message) {
    perror(message);
    exit(EXIT_FAILURE);
}

// send 成功也可能只发送一部分，因此必须循环，直到 count 个字节全部交给内核。
// 返回 false 表示当前连接无法继续发送；由调用者关闭这个连接。
static bool Writen(int fd, const char* buffer, size_t count) {
    while (count > 0) {
        // Linux 下 MSG_NOSIGNAL 避免对端断开时，SIGPIPE 信号直接终止服务端。
        // 连接错误仍会通过 send 的返回值和 errno 报告。
        const ssize_t written = send(fd, buffer, count, MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR) continue; // 被信号中断，本次未发送，重新尝试
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

int main() {
    // 关闭 stdout 缓冲，让 Visual Studio 的 Linux 控制台及时显示日志。
    setvbuf(stdout, nullptr, _IONBF, 0);

    // 1. 创建 IPv4 TCP socket。Linux 用 int 文件描述符表示 socket，失败返回 -1。
    const int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1) err("socket");

    // 允许重用处于 TIME_WAIT 等状态的本地地址，方便停止调试后重新启动。
    // 这不允许两个服务端同时监听相同的地址和端口。
    const int reuse = 1;
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) {
        err("setsockopt");
    }

    // 2. 设置监听地址并绑定端口。{} 将整个结构体初始化为零。
    // INADDR_ANY 表示监听本机所有 IPv4 网卡地址；客户端应连接实际 IP。
    // 端口和 IPv4 地址字段需要使用网络字节序（大端）。
    sockaddr_in servaddr{};
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(SERV_PORT);
    if (bind(listenfd, reinterpret_cast<const sockaddr*>(&servaddr), sizeof(servaddr)) == -1) {
        err("bind");
    }

    // 3. 将 socket 变为监听 socket。
    // LISTENQ 是等待 accept 的连接队列长度请求值，不是同时处理客户端的数量。
    if (listen(listenfd, LISTENQ) == -1) err("listen");
    printf("[server] listening on 0.0.0.0:%d ...\n", SERV_PORT);

    for (;;) {
        // 4. accept 阻塞等待连接，并返回专门与这个客户端通信的新描述符 connfd。
        // listenfd 继续用于监听。clilen 是输入/输出参数，每次 accept 前都要初始化。
        sockaddr_in cliaddr{};
        socklen_t clilen = sizeof(cliaddr);
        const int connfd = accept(listenfd, reinterpret_cast<sockaddr*>(&cliaddr), &clilen);
        if (connfd == -1) {
            if (errno == EINTR) continue;
            err("accept");
        }

        char ip[INET_ADDRSTRLEN] = "unknown";
        if (inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof(ip)) == nullptr) {
            perror("inet_ntop");
        }
        const unsigned int port = ntohs(cliaddr.sin_port);
        printf("[accept] %s:%u connected\n", ip, port);

        // 5. TCP 是字节流：一次 recv 得到的字节数不一定等于客户端一次 send 的长度。
        // 回射服务端只需要将每次收到的字节完整发回，不需要识别文本行边界。
        char buffer[MAXLINE];
        for (;;) {
            const ssize_t received = recv(connfd, buffer, sizeof(buffer), 0);
            if (received == 0) break; // 对端正常关闭了发送方向，已经没有更多数据
            if (received < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break; // 当前客户端出错，关闭它后仍可接受后续客户端
            }

            // recv 不会自动添加字符串结束符；%.*s 限制日志最多读取 received 字节。
            // 日志按文本显示，但发送时仍按实际字节数处理，包括数据中的 '\0'。
            printf("[recv  ] %s:%u : \"%.*s\"\n", ip, port, static_cast<int>(received), buffer);
            if (!Writen(connfd, buffer, static_cast<size_t>(received))) break;
            printf("[echo  ] %s:%u : \"%.*s\"\n", ip, port, static_cast<int>(received), buffer);
        }

        // 6. 关闭当前连接，再回到 accept。
        // 本示例是串行服务：当前客户端保持连接时，其他客户端需要等待它结束。
        close(connfd);
        printf("[close ] %s:%u disconnected\n", ip, port);
    }
}
