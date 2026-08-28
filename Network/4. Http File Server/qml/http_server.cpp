
#include "select_engine.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdlib>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>      // open 系列之外的 close/read
#include <fcntl.h>       // open / O_RDONLY
#include <sys/stat.h>    // fstat / S_ISREG

enum { LISTENQ = 16 };

const size_t kMaxHeader  = 8192;    // 请求头上限：超过即 400（防异常/恶意报文撑爆内存）
const size_t kFileChunk  = 65536;   // 每次从文件读入 out_ 的块大小

// 连接状态机
enum State {
    kRecvRequest,   // 正在收请求头（只订阅可读）
    kRespond,       // 纯错误响应在 out_ 中，发完即关
    kSendFile       // 文件流发送中
};

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

// 按扩展名给 Content-Type（找不到就当二进制流）
static const char* MimeType(const std::string& path) {
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = path.substr(dot + 1);
    for (size_t i = 0; i < ext.size(); ++i)
        if (ext[i] >= 'A' && ext[i] <= 'Z') ext[i] += 32;
    if (ext == "html" || ext == "htm") return "text/html";
    if (ext == "css")  return "text/css";
    if (ext == "js")   return "application/javascript";
    if (ext == "json") return "application/json";
    if (ext == "txt")  return "text/plain";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif")  return "image/gif";
    if (ext == "ico")  return "image/x-icon";
    if (ext == "pdf")  return "application/pdf";
    if (ext == "mp4")  return "video/mp4";
    return "application/octet-stream";
}

class HttpSession : public IIoHandler {
public:
    HttpSession(SelectEngine& engine, FD fd, long id,
                const std::string& peer, const std::string& docroot)
        : engine_(engine), fd_(fd), id_(id), peer_(peer), docroot_(docroot) {
        LogLine(id_, peer_.c_str(), "connected");
    }

    ~HttpSession() {
        if (file_fd_ >= 0) close(file_fd_);   // 兜底：任何路径漏关文件都有析构保险
    }

    // 可读：收请求 / 感知对端断开
    void OnRead(FD, void*, int32_t) override {
        char buf[4096];
        for (;;) {   // 非阻塞：一次把可读数据收干净
            ssize_t n = recv(fd_, buf, sizeof buf, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
                Close("recv error"); return;
            }
            if (n == 0) {   // 对端断开：请求中或发送中被断都从这里收尾
                Close(state_ == kRecvRequest ? "EOF before request" : "aborted by peer");
                return;
            }
            // 发送/响应阶段的额外数据：Connection: close 语义下直接丢弃
            if (state_ != kRecvRequest) continue;

            in_.append(buf, (size_t)n);
            if (in_.size() > kMaxHeader) {
                RespondError("400 Bad Request", "header too large"); return;
            }
            if (in_.find("\r\n\r\n") != std::string::npos) {   // 请求头收完
                HandleRequest();
                return;
            }
        }
    }

    //可写：把 out_ 冲干净；空了就补文件块
    void OnWrite(FD, void*, int32_t) override {
        for (;;) {
            while (!out_.empty()) {
                ssize_t n = send(fd_, out_.data(), out_.size(), 0);
                if (n > 0) {
                    LogLine(id_, peer_.c_str(), "send  %zd bytes (%zu bytes pending)",
                            n, out_.size() - (size_t)n);
                    out_.erase(0, (size_t)n); continue;   // 分段发送核心：
                }
                if (errno == EINTR) continue;                        // send 发多少算多少，
                if (errno == EAGAIN || errno == EWOULDBLOCK)         // 剩余留到下轮可写
                    return;
                Close("send error"); return;
            }
            // out_ 已空
            if (state_ == kRespond) { Close("response sent"); return; }
            if (state_ == kSendFile) {
                if (file_left_ > 0) {
                    if (!FillFromFile()) return;   // 内部已 Close
                    continue;                      // 补了新块，回头继续发
                }
                LogLine(id_, peer_.c_str(), "GET done (%lld bytes sent)",
                        (long long)file_size_);
                Close("done");
                return;
            }
            return;   // kRecvRequest 状态不会订阅可写，防御性兜底
        }
    }

private:
    //解析请求行并打开文件
    void HandleRequest() {
        size_t line_end = in_.find("\r\n");
        std::string line = in_.substr(0, line_end);

        // 请求行三段：METHOD SP PATH SP VERSION
        size_t sp1 = line.find(' ');
        size_t sp2 = (sp1 == std::string::npos) ? std::string::npos
                                                : line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) {
            RespondError("400 Bad Request", "malformed request line"); return;
        }
        std::string method = line.substr(0, sp1);
        std::string path   = line.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string ver    = line.substr(sp2 + 1);
        if (ver.compare(0, 5, "HTTP/") != 0) {
            RespondError("400 Bad Request", "bad version"); return;
        }
        if (method != "GET") {
            RespondError("405 Method Not Allowed", method.c_str()); return;
        }

        size_t q = path.find('?');                  // 丢弃查询串
        if (q != std::string::npos) path.erase(q);
        if (path.empty() || path[0] != '/') {
            RespondError("400 Bad Request", "bad path"); return;
        }
        if (path[path.size() - 1] == '/') path += "index.html";
        if (path.find("..") != std::string::npos) { // 目录穿越防护
            RespondError("403 Forbidden", "path traversal"); return;
        }

        std::string full = docroot_ + path;
        file_fd_ = open(full.c_str(), O_RDONLY);    // 通用文件 IO：open
        if (file_fd_ < 0) {
            RespondError("404 Not Found", full.c_str()); return;
        }
        struct stat st;
        if (fstat(file_fd_, &st) < 0 || !S_ISREG(st.st_mode)) {
            close(file_fd_); file_fd_ = -1;         // 目录等非常规文件按 404 处理
            RespondError("404 Not Found", "not a regular file"); return;
        }

        file_size_ = (long long)st.st_size;
        file_left_ = st.st_size;

        char hdr[1024];
        snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: %s\r\n"
                 "Content-Length: %lld\r\n"
                 "Connection: close\r\n"
                 "Server: mini-http-select/1.0\r\n"
                 "\r\n",
                 MimeType(path), (long long)file_size_);
        out_ += hdr;
        in_.clear();
        state_ = kSendFile;

        LogLine(id_, peer_.c_str(), "GET %s -> 200 (%s, %lld bytes)",
                path.c_str(), MimeType(path), (long long)file_size_);

        if (!FillFromFile()) return;               // 预读第一块
        // 发送期间保持可读订阅：才能感知"发到一半客户端断了"
        engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
    }

    // 从文件补一块进 out_。返回 false 表示已 Close，调用方必须立即 return
    bool FillFromFile() {
        char chunk[kFileChunk];
        size_t want = (file_left_ < (off_t)kFileChunk) ? (size_t)file_left_
                                                       : kFileChunk;
        ssize_t n = read(file_fd_, chunk, want);   // 通用文件 IO：read
        if (n <= 0) {
            Close(n < 0 ? "file read error" : "file truncated unexpectedly");
            return false;
        }
        out_.append(chunk, (size_t)n);
        file_left_ -= n;
        return true;
    }

    // 构造错误响应（无文件）：发完自动关闭
    void RespondError(const char* status, const char* why) {
        LogLine(id_, peer_.c_str(), "-> %s (%s)", status, why);
        std::string body = std::string(status) + "\n";
        char hdr[512];
        snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 %s\r\n"
                 "Content-Type: text/plain\r\n"
                 "Content-Length: %zu\r\n"
                 "Connection: close\r\n"
                 "Server: mini-http-select/1.0\r\n"
                 "\r\n",
                 status, body.size());
        out_ = std::string(hdr) + body;
        in_.clear();
        state_ = kRespond;
        engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
    }

    // 所有错误/完成路径的唯一出口：资源成对释放，顺序不可乱
    void Close(const char* reason) {
        LogLine(id_, peer_.c_str(), "closed (%s)", reason);
        engine_.DeleteIoEvent(fd_, kReadEvent | kWriteEvent);
        close(fd_); fd_ = -1;
        if (file_fd_ >= 0) { close(file_fd_); file_fd_ = -1; }
        delete this;   // 引擎回调前会重查 entry，删除后不会被再次调用
    }

    SelectEngine& engine_;
    FD            fd_;
    long          id_;
    std::string   peer_;
    std::string   docroot_;

    State         state_ = kRecvRequest;
    std::string   in_;          // 请求头积累缓冲（TCP 分段到达）
    std::string   out_;         // 待发送缓冲（响应头 + 当前文件块）
    int           file_fd_ = -1;// 已打开文件的 fd
    long long     file_size_ = 0;
    off_t         file_left_ = 0;
};


// HttpServer：监听 socket 回调，accept 新连接并创建会话

class HttpServer : public IIoHandler {
public:
    HttpServer(SelectEngine& engine, FD listenfd, const std::string& docroot)
        : engine_(engine), listen_fd_(listenfd), docroot_(docroot) {}

    void OnRead(FD, void*, int32_t) override {
        for (;;) {   // 非阻塞 accept 循环取空队列
            sockaddr_in cliaddr{};
            socklen_t clilen = sizeof cliaddr;
            int cfd = accept(listen_fd_, (sockaddr*)&cliaddr, &clilen);
            if (cfd < 0) break;   // EAGAIN：队列空了

            set_nonblock(cfd);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof ip);
            char peer[64];
            snprintf(peer, sizeof peer, "%s:%u", ip, (unsigned)ntohs(cliaddr.sin_port));

            HttpSession* session = new HttpSession(engine_, cfd, next_id_++, peer, docroot_);
            engine_.AddIoEvent(cfd, kReadEvent, session, NULL);
        }
    }

    void OnWrite(FD, void*, int32_t) override {}   // 监听 socket 无可写事件

private:
    SelectEngine& engine_;
    FD            listen_fd_;
    std::string   docroot_;
    long          next_id_ = 1;
};

int main(int argc, char** argv) {
    // 用法：./http_server [文档根目录] [端口]
    const char* docroot = (argc > 1) ? argv[1] : ".";
    int port = (argc > 2) ? atoi(argv[2]) : 8080;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);   // 客户端中途断开时 send 不能杀死进程

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return EXIT_FAILURE; }

    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);

    sockaddr_in servaddr{};
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons((unsigned short)port);
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof servaddr) < 0) {
        perror("bind"); return EXIT_FAILURE;
    }
    if (listen(listenfd, LISTENQ) < 0) { perror("listen"); return EXIT_FAILURE; }
    set_nonblock(listenfd);

    SelectEngine engine;
    HttpServer   server(engine, listenfd, docroot);
    engine.AddIoEvent(listenfd, kReadEvent, &server, NULL);

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] [server] [-] http listening on 0.0.0.0:%d docroot=%s (select engine)\n",
           ts, port, docroot);

    return engine.Run();
}
