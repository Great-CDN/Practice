#pragma once
// ============================================================================
// net_util.h —— 通用小工具：时间 / 日志 / socket 设置（仅 Linux，头部直接内联）
// ============================================================================
#include "event_engine.h"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

inline long long NowMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// 单行日志（高频路径默认不打日志，见 VerboseLog）
inline void LogLine(const char* fmt, ...) {
    char ts[32], msg[1024];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", ts, msg);
    fflush(stdout);
}

// 每请求级日志开关（默认关：高频 printf + fflush 是吞吐杀手，压测时务必关闭）
inline bool VerboseLog() {
    static bool on = getenv("MINIHTTP_VERBOSE") != NULL;
    return on;
}

inline ErrCode SetNonblock(FD fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return kErr;
    return kOk;
}

// 禁用 Nagle：小包响应（如直播帧）不被攒包延迟，是低延迟的关键之一
inline void SetTcpNodelay(FD fd) {
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
}

// 创建监听 socket：SO_REUSEADDR + bind + listen + 非阻塞；失败返回 -1
inline FD CreateListenSocket(int port, int backlog) {
    FD fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons((unsigned short)port);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0) { close(fd); return -1; }
    if (listen(fd, backlog) < 0) { close(fd); return -1; }
    if (SetNonblock(fd) != kOk) { close(fd); return -1; }
    return fd;
}
