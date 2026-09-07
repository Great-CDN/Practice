// Linux TCP 回射客户端，可与本章的 select 或 epoll 服务端配合使用。
// 用法：./client [服务器IPv4地址] [--split]
// --split 会把输入按 3 字节分段发送，用于观察 TCP 字节流和多路复用行为。
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>   // inet_pton
#include <netinet/in.h> // sockaddr_in、htons
#include <sys/socket.h> // socket、connect、send、recv
#include <time.h>       // nanosleep
#include <unistd.h>     // close

enum { SERV_PORT = 9877, MAXLINE = 4096, SPLIT_SIZE = 3, SPLIT_DELAY_MS = 200 };

static void err(const char* message) {
    perror(message);
    exit(EXIT_FAILURE);
}

static bool SleepMilliseconds(long milliseconds) {
    timespec remaining{};
    remaining.tv_sec = milliseconds / 1000;
    remaining.tv_nsec = (milliseconds % 1000) * 1000000L;

    while (nanosleep(&remaining, &remaining) == -1) {
        if (errno == EINTR) continue;
        perror("nanosleep");
        return false;
    }
    return true;
}

// send 成功也可能只写入一部分，因此循环到 count 个字节全部交给内核。
static bool Writen(int fd, const char* buffer, size_t count) {
    while (count > 0) {
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

// TCP 没有消息边界。一次 send 的数据可能被服务端分多次回射，因此累计收满 count 字节。
static bool ReadEcho(int fd, size_t count) {
    char buffer[MAXLINE];
    size_t total = 0;
    while (total < count) {
        const size_t remaining = count - total;
        const size_t capacity = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
        const ssize_t received = recv(fd, buffer, capacity, 0);
        if (received == 0) {
            fprintf(stderr, "服务器已关闭连接：本次回射收到 %zu/%zu 字节\n", total, count);
            return false;
        }
        if (received < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            return false;
        }

        printf("[服务器回射] %.*s\n", static_cast<int>(received), buffer);
        total += static_cast<size_t>(received);
    }
    return true;
}

static void PrintUsage(const char* program) {
    fprintf(stderr, "用法：%s [服务器IPv4地址] [--split]\n", program);
}

int main(int argc, char** argv) {
    const char* server_ip = "127.0.0.1";
    bool address_set = false;
    bool split = false;

    // 参数顺序不限，但最多接受一个 IPv4 地址。
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--split") == 0) {
            split = true;
        } else if (!address_set) {
            server_ip = argv[index];
            address_set = true;
        } else {
            PrintUsage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERV_PORT);
    const int parsed = inet_pton(AF_INET, server_ip, &server_address.sin_addr);
    if (parsed == 0) {
        fprintf(stderr, "无效的 IPv4 地址：%s\n", server_ip);
        return EXIT_FAILURE;
    }
    if (parsed == -1) err("inet_pton");

    // 客户端无需 bind；connect 时内核会自动选择本地地址和临时端口。
    const int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) err("socket");
    if (connect(sockfd, reinterpret_cast<const sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        err("connect");
    }

    printf("已连接到服务器 %s:%d%s，输入内容回车发送，输入 exit 退出\n",
           server_ip, SERV_PORT, split ? "（分段发送模式）" : "");

    char sendline[MAXLINE];
    int result = EXIT_SUCCESS;
    for (;;) {
        printf("[你输入] ");
        fflush(stdout);
        if (fgets(sendline, sizeof(sendline), stdin) == nullptr) {
            if (ferror(stdin)) {
                perror("fgets");
                result = EXIT_FAILURE;
            }
            break;
        }

        sendline[strcspn(sendline, "\r\n")] = '\0';
        if (sendline[0] == '\0') continue;
        if (strcmp(sendline, "exit") == 0) break;

        const size_t length = strlen(sendline);
        if (split) {
            for (size_t offset = 0; offset < length; offset += SPLIT_SIZE) {
                const size_t remaining = length - offset;
                const size_t split_size = static_cast<size_t>(SPLIT_SIZE);
                const size_t part = (remaining < split_size) ? remaining : split_size;
                if (!Writen(sockfd, sendline + offset, part)) {
                    result = EXIT_FAILURE;
                    break;
                }
                printf("[分段发送] %zu 字节: \"%.*s\"\n", part, static_cast<int>(part),
                       sendline + offset);
                if (offset + part < length && !SleepMilliseconds(SPLIT_DELAY_MS)) {
                    result = EXIT_FAILURE;
                    break;
                }
            }
            if (result != EXIT_SUCCESS) break;
        } else if (!Writen(sockfd, sendline, length)) {
            result = EXIT_FAILURE;
            break;
        }

        if (!ReadEcho(sockfd, length)) {
            result = EXIT_FAILURE;
            break;
        }
    }

    close(sockfd);
    printf("已断开连接\n");
    return result;
}
