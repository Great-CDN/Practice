// ============================================================================
// origin_server.cpp —— 测试用源站（SelectEngine 单进程版，替代 fork 模型）
//
// 端点（配合反向代理验收）：
//   GET  /live/<name>   直播流：每 100ms 推一帧文本（引擎 OnLoop 钩子定时）
//   GET  /static/<N>    静态内容：N KB 确定性数据（带 Content-Length，可缓存）
//   GET  /slow/<N>      慢响应：延时 2 秒后同 /static/<N>（观察合并回源）
//   POST 任意            读取 body 后回显长度（POST 直通测试）
//
// 为什么弃用 fork 模型：fork 子进程在"accept 完成但尚未 read"的窗口退出时，
// 内核会对未读数据回 RST，导致代理的非阻塞连接偶发 ECONNRESET；
// 单进程 + 引擎后所有连接都在同一事件循环里，彻底消灭这类时序问题
// ============================================================================
#include "select_engine.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

const size_t kMaxHead = 8192;

static long long NowMs() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static void LogLine(const char* fmt, ...) {
    char ts[32], msg[1024];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("[%s] [origin] %s\n", ts, msg);
    fflush(stdout);
}

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
        fprintf(stderr, "[FATAL] fcntl failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

// N KB 确定性内容（ABCDEFGHIJKLMNOPQRSTUVWXYZ... 循环）
static std::string MakeBody(int kb) {
    std::string b;
    b.reserve((size_t)kb * 1024);
    for (int i = 0; i < kb * 1024; ++i) b.push_back((char)('A' + i % 26));
    return b;
}

static bool ParseHead(const std::string& in, std::string* method,
                      std::string* path, long long* content_length) {
    size_t line_end = in.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::string line = in.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    *method = line.substr(0, sp1);
    *path   = line.substr(sp1 + 1, sp2 - sp1 - 1);

    *content_length = 0;
    for (size_t pos = line_end + 2; pos + 1 < in.size(); ) {
        size_t e = in.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;
        std::string kv = in.substr(pos, e - pos);
        size_t c = kv.find(':');
        if (c != std::string::npos) {
            std::string key = kv.substr(0, c), val = kv.substr(c + 1);
            for (size_t i = 0; i < key.size(); ++i)
                if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 32;
            while (!val.empty() && val[0] == ' ') val.erase(0, 1);
            if (key == "content-length") *content_length = atoll(val.c_str());
        }
        pos = e + 2;
    }
    return true;
}

// ---------- 前置声明 ----------
class OriginServer;

// ---------- 一条到客户端的连接 ----------
class OriginConn : public IIoHandler {
public:
    OriginConn(OriginServer& srv, SelectEngine& eng, FD fd, std::string peer);
    ~OriginConn() override;

    void OnRead (FD fd, void* data, int32_t mask) override;
    void OnWrite(FD fd, void* data, int32_t mask) override;
    void OnTick(long long now);        // 引擎每轮回调：直播定帧 / 慢响应到期

    void Close(const char* reason);

private:
    void Route();                      // 请求头收齐后的分发
    void RespondNow(const std::string& resp, const char* done_how);
    void TryFinish() { if (out_.empty() && st_ != kLive && st_ != kDelay) Close("done"); }

    enum St { kHead, kBodyWait, kFlush, kDelay, kLive } st_ = kHead;

    OriginServer& srv_;
    SelectEngine& engine_;
    FD            fd_;
    std::string   peer_;

    std::string   in_, out_;
    std::string   path_;
    size_t        head_end_ = 0;
    size_t        need_body_ = 0;
    int           kb_ = 4;
    long long     due_ms_ = 0;         // /slow/ 的到期时刻
    long long     last_frame_ms_ = 0;  // 直播上一帧时刻
};

// ---------- 监听器 + 连接登记表 ----------
class OriginServer : public IIoHandler {
public:
    OriginServer(SelectEngine& eng, FD lfd) : engine_(eng), listen_fd_(lfd) {}

    void OnRead(FD, void*, int32_t) override {
        for (;;) {
            sockaddr_in cliaddr{};
            socklen_t clilen = sizeof cliaddr;
            FD cfd = accept(listen_fd_, (sockaddr*)&cliaddr, &clilen);
            if (cfd < 0) break;
            set_nonblock(cfd);
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cliaddr.sin_addr, ip, sizeof ip);
            char peer[64];
            snprintf(peer, sizeof peer, "%s:%u", ip, (unsigned)ntohs(cliaddr.sin_port));
            OriginConn* c = new OriginConn(*this, engine_, cfd, peer);
            engine_.AddIoEvent(cfd, kReadEvent, c, NULL);
        }
    }
    void OnWrite(FD, void*, int32_t) override {}

    void AddLive(OriginConn* c)  { live_.push_back(c); }
    void AddDelay(OriginConn* c) { delay_.push_back(c); }
    void Remove(OriginConn* c) {
        auto e = std::find(live_.begin(), live_.end(), c);   if (e != live_.end())  live_.erase(e);
        auto d = std::find(delay_.begin(), delay_.end(), c); if (d != delay_.end()) delay_.erase(d);
    }
    void Tick() {                     // 引擎每轮回调：驱动定时逻辑
        long long now = NowMs();
        for (OriginConn* c : live_)  c->OnTick(now);
        for (OriginConn* c : delay_) c->OnTick(now);
    }

private:
    SelectEngine& engine_;
    FD            listen_fd_;
    std::vector<OriginConn*> live_;    // 正在收看的直播连接
    std::vector<OriginConn*> delay_;   // 正在等待 /slow/ 到期的连接
};

// 引擎子类：把每轮回调转发给源站业务
class OriginEngine : public SelectEngine {
public:
    void SetServer(OriginServer* s) { srv_ = s; }   // 打破"互相引用"的构造顺序问题
    void OnLoop() override { if (srv_) srv_->Tick(); }
private:
    OriginServer* srv_ = nullptr;
};

// ============================================================================
// OriginConn 实现
// ============================================================================
OriginConn::OriginConn(OriginServer& srv, SelectEngine& eng, FD fd, std::string peer)
    : srv_(srv), engine_(eng), fd_(fd), peer_(std::move(peer)) {
    LogLine("conn %s established", peer_.c_str());
}
OriginConn::~OriginConn() {
    if (fd_ >= 0) close(fd_);
}

void OriginConn::OnRead(FD, void*, int32_t) {
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd_, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            Close("recv error"); return;
        }
        if (n == 0) { Close("client disconnected"); return; }
        if (st_ == kLive || st_ == kFlush) continue;    // 推送/响应阶段：数据一律丢弃

        in_.append(buf, (size_t)n);
        if (in_.size() > kMaxHead) { Close("request too large"); return; }

        size_t hend = in_.find("\r\n\r\n");
        if (hend == std::string::npos) continue;        // 请求头没收齐

        std::string method, path;
        long long cl = 0;
        if (!ParseHead(in_, &method, &path, &cl)) { Close("bad request"); return; }
        path_ = path; head_end_ = hend + 4;
        LogLine("%s %s", method.c_str(), path_.c_str());

        if (method == "POST") {                          // 读齐 body 再回显
            if (cl < 0 || (size_t)cl > kMaxHead) { Close("body too large"); return; }
            need_body_ = (size_t)cl;
            if (in_.size() - head_end_ >= need_body_) {
                LogLine("POST: received %zu bytes", in_.size() - head_end_);
                char tail[128];
                snprintf(tail, sizeof tail, "POST OK, origin received %zu bytes\n",
                         in_.size() - head_end_);
                std::string resp = std::string(tail);
                char hdr[256];
                snprintf(hdr, sizeof hdr,
                         "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n", resp.size());
                out_ = std::string(hdr) + resp;
                st_ = kFlush;
                engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
            } else {
                st_ = kBodyWait;
            }
            return;
        }

        if (method != "GET") { Close("method not supported"); return; }

        if (path_.rfind("/live/", 0) == 0) {             // 直播：先发响应头，之后 OnTick 定帧
            LogLine("live start %s", path_.c_str());
            out_ += "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                    "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
            st_ = kLive;
            last_frame_ms_ = 0;                          // 下一轮 Tick 立刻推第一帧
            srv_.AddLive(this);
            engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
            return;
        }
        if (path_.rfind("/slow/", 0) == 0) {             // 慢响应：延时 2 秒（观察合并回源）
            kb_ = atoi(path_.c_str() + 6);
            if (kb_ <= 0) kb_ = 4;
            LogLine("slow %s: delay 2s then %d KB", path_.c_str(), kb_);
            st_ = kDelay;
            due_ms_ = NowMs() + 2000;
            srv_.AddDelay(this);
            return;                                      // 延迟期间只保留可读（感知断开）
        }
        if (path_.rfind("/static/", 0) == 0) {           // 静态：立即回 N KB
            kb_ = atoi(path_.c_str() + 8);
            if (kb_ <= 0) kb_ = 4;
            char hdr[256];
            snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                     "Content-Length: %d\r\nConnection: close\r\n\r\n", kb_ * 1024);
            out_ = std::string(hdr) + MakeBody(kb_);
            st_ = kFlush;
            engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
            return;
        }
        // 未匹配路径：404
        out_ = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
               "Content-Length: 0\r\nConnection: close\r\n\r\n";
        st_ = kFlush;
        engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
        return;
    }
}

void OriginConn::OnWrite(FD, void*, int32_t) {
    while (!out_.empty()) {
        ssize_t n = send(fd_, out_.data(), out_.size(), 0);
        if (n > 0) { out_.erase(0, (size_t)n); continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        Close("send error"); return;
    }
    if (st_ == kFlush) Close("done");                    // 非直播：发完即关
}

// 引擎每轮回调：直播定帧 + 慢响应到期
void OriginConn::OnTick(long long now) {
    if (st_ == kLive) {
        if (last_frame_ms_ != 0 && now - last_frame_ms_ < 100) return;
        last_frame_ms_ = now;
        char frame[256];
        snprintf(frame, sizeof frame, "frame ts=%lld live-data\n", now);
        out_.append(frame);
        engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
        return;
    }
    if (st_ == kDelay && now >= due_ms_) {
        char hdr[256];
        snprintf(hdr, sizeof hdr,
                 "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n", kb_ * 1024);
        out_ = std::string(hdr) + MakeBody(kb_);
        st_ = kFlush;
        engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
    }
}

void OriginConn::Close(const char* reason) {
    LogLine("conn %s closed (%s)", peer_.c_str(), reason);
    engine_.DeleteIoEvent(fd_, kReadEvent | kWriteEvent);
    srv_.Remove(this);                 // 从直播/延时列表摘除（Close 后列表不再持有指针）
    if (fd_ >= 0) { close(fd_); fd_ = -1; }
    delete this;
}

// ============================================================================
// main
// ============================================================================
int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 8090;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return EXIT_FAILURE; }
    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(listenfd, (sockaddr*)&a, sizeof a) < 0) { perror("bind"); return EXIT_FAILURE; }
    if (listen(listenfd, 16) < 0) { perror("listen"); return EXIT_FAILURE; }
    set_nonblock(listenfd);

    OriginEngine engine;                    // srv_ 先为空
    OriginServer server(engine, listenfd);  // 源站业务对象
    engine.SetServer(&server);              // 回填指针，之后 OnLoop 才开始转发
    engine.AddIoEvent(listenfd, kReadEvent, &server, NULL);

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] [origin] listening on 0.0.0.0:%d (select engine)\n", ts, port);

    return engine.Run(50);   // 50ms 循环粒度：驱动直播 100ms 定帧与 /slow/ 2s 到期
}
