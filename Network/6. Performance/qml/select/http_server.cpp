
#include "http_server.h"
#include "net_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <map>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

// ---- 行为参数 ----
static const size_t    kMaxHead        = 8192;      // 请求头上限
static const size_t    kMaxBody        = 8192;      // POST body 上限（与原版一致）
static const size_t    kMaxInBuf       = 64 * 1024; // 攒包缓冲上限（防恶意管道化）
static const int       kMaxKb          = 16 * 1024; // /static|/slow 单响应上限（16MB）
static const size_t    kBodyCacheBytes = 64 << 20;  // 静态体缓存总预算
static const long long kFrameMs        = 100;       // 直播帧间隔
static const long long kSlowMs         = 2000;      // /slow/ 延时时长


// HTTP 解析


// 一个请求里服务器关心的字段
struct HttpReq {
    std::string method;
    std::string path;
    bool        keep_alive = true;
    long long   content_length = 0;
};

// 解析请求行 + 头部（in[0..head_end) 为完整头部，以 "\r\n\r\n" 结尾）
static bool ParseHead(const std::string& in, size_t head_end, HttpReq* req) {
    size_t line_end = in.find("\r\n");
    if (line_end == std::string::npos || line_end > head_end) return false;

    // 请求行：METHOD SP PATH SP VERSION
    std::string line = in.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req->method = line.substr(0, sp1);
    req->path   = line.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string ver = line.substr(sp2 + 1);
    if (req->path.empty() || req->path[0] != '/') return false;
    if (ver.compare(0, 7, "HTTP/1.") != 0) return false;
    req->keep_alive = (ver.size() >= 8 && ver[7] >= '1');   // HTTP/1.1 默认 keep-alive

    // 头部：只关心 content-length / connection
    for (size_t pos = line_end + 2; pos < head_end; ) {
        size_t e = in.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;
        if (e > head_end) e = head_end;
        size_t c = in.find(':', pos);
        if (c != std::string::npos && c < e) {
            std::string key = in.substr(pos, c - pos);
            for (size_t i = 0; i < key.size(); ++i)
                if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 32;
            size_t v = c + 1;
            while (v < e && in[v] == ' ') ++v;             // 跳过冒号后的空格
            std::string val = in.substr(v, e - v);
            if (key == "content-length") {
                req->content_length = atoll(val.c_str());
            } else if (key == "connection") {
                if (val.find("close") != std::string::npos)           req->keep_alive = false;
                else if (val.find("keep-alive") != std::string::npos) req->keep_alive = true;
            }
        }
        pos = e + 2;
    }
    return true;
}

// 解析 "/static/N"、"/slow/N" 里的 N：
//   非法/缺省 -> 0（调用方回落 4KB，与原版一致）；超过 kMaxKb -> -1（拒绝）
static int ParseKb(size_t prefix_len, const std::string& path) {
    if (path.size() <= prefix_len) return 0;
    long v = 0;
    for (size_t i = prefix_len; i < path.size(); ++i) {
        char ch = path[i];
        if (ch < '0' || ch > '9') return 0;
        v = v * 10 + (ch - '0');
        if (v > kMaxKb) return -1;                        // 提前止损，防溢出
    }
    return (int)v;
}

// 响应头（Content-Length 明确标注，客户端才能放心复用连接）
static void AppendHead(std::string* out, const char* status, const char* ctype,
                       size_t content_len, bool keep_alive) {
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: %s\r\n"
                     "\r\n",
                     status, ctype, content_len, keep_alive ? "keep-alive" : "close");
    out->append(hdr, (size_t)n);
}

// N KB 确定性内容（A-Z 循环），按大小缓存复用（原版每请求重新生成）
// 预算制：缓存总量超过 kBodyCacheBytes 就整体清空，占用有上界
static const std::string& StaticBody(int kb) {
    static std::map<int, std::string> cache;
    static size_t total = 0;
    auto it = cache.find(kb);
    if (it != cache.end()) return it->second;

    std::string b;
    b.reserve((size_t)kb * 1024);
    for (int i = 0; i < kb * 1024; ++i) b.push_back((char)('A' + i % 26));
    if (total + b.size() > kBodyCacheBytes) { cache.clear(); total = 0; }
    total += b.size();
    return cache.insert(std::make_pair(kb, std::move(b))).first->second;
}

// 组装 N KB 静态响应（/static/ 立即回、/slow/ 到期回，共用）
static void AppendStaticResponse(std::string* out, int kb, bool keep_alive) {
    AppendHead(out, "200 OK", "application/octet-stream", (size_t)kb * 1024, keep_alive);
    out->append(StaticBody(kb));
}

// HttpConn —— 一条客户端连接（一个完整的请求/响应状态机）

class HttpConn : public IIoHandler {
public:
    HttpConn(HttpServer& srv, IEventEngine& eng, FD fd, std::string peer);
    ~HttpConn() override;                 // 兜底关 fd（正常路径在 Close 里已关）

    void OnRead (FD fd, void* data, int32_t mask) override;
    void OnWrite(FD fd, void* data, int32_t mask) override;

    void OnTick(long long now);           // 直播定帧 / 慢响应到期
    void Close(const char* reason);       // 唯一收尾出口（可安全重入）

private:
    enum St { kHead, kWrite, kLive, kDelay };

    // 状态机主泵：只要还有完整请求且连接空闲就一直处理（keep-alive/管道化）
    void TryProcess();
    // 一个完整请求的路由与响应
    void HandleRequest(size_t body_len);
    void RespondStatic();
    void RespondError(const char* status, const char* why);
    void StartLive();
    void StartDelay();
    // 冲刷 out_；发完撤可写关注。失败（EPIPE/ECONNRESET 等）会走 Close
    void TryFlush();
    // 响应追加完毕后调用：决定连接去向（回 kHead / 等 kWrite / 关闭）
    void FlushSettle();

    HttpServer&   srv_;
    IEventEngine& eng_;
    FD            fd_;
    std::string   peer_;

    St          st_ = kHead;
    bool        closing_ = false;
    std::string in_, out_;                // 收包缓冲 / 待发缓冲

    HttpReq     req_;                     // 当前请求（head_done_ 后有效）
    bool        head_done_ = false;       // 头是否已解析（POST 攒 body 期间为 true）
    size_t      head_len_ = 0;            // 头部总长（含 \r\n\r\n）
    size_t      need_body_ = 0;           // POST 还需等待的 body 字节数

    int         kb_ = 4;                  // /slow/ 的响应大小
    long long   due_ms_ = 0;              // /slow/ 到期时刻
    long long   last_frame_ms_ = 0;       // 直播上一帧时刻
};


HttpConn::HttpConn(HttpServer& srv, IEventEngine& eng, FD fd, std::string peer)
    : srv_(srv), eng_(eng), fd_(fd), peer_(std::move(peer)) {}

HttpConn::~HttpConn() {
    if (fd_ >= 0) close(fd_);             // Close() 已把 fd_ 置 -1，这里只是兜底
}

void HttpConn::Close(const char* reason) {
    if (closing_) return;
    closing_ = true;
    if (VerboseLog()) LogLine("conn %s closed (%s)", peer_.c_str(), reason);
    eng_.DeleteIoEvent(fd_, kReadEvent | kWriteEvent);
    if (fd_ >= 0) { close(fd_); fd_ = -1; }   // fd 立即归还系统；对象延后删除
    srv_.RemoveTick(this);                    // 移出定时列表（不在列表则无操作）
    srv_.Recycle(this);                       // 下一轮 Tick() 末尾统一 delete
}


void HttpConn::OnRead(FD, void*, int32_t) {
    if (closing_) return;
    char buf[16384];
    for (;;) {
        ssize_t n = recv(fd_, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            LogLine("conn %s recv error: %s", peer_.c_str(), strerror(errno));
            Close("recv error");
            return;
        }
        if (n == 0) {                         // 对端关闭：keep-alive 断开也走这里
            Close("peer closed");
            return;
        }

        // 推流/等待期不再收请求：读只为感知断开，数据一律丢弃
        if (st_ == kLive || st_ == kDelay) continue;

        in_.append(buf, (size_t)n);
        TryProcess();
        if (closing_) return;
        if (st_ == kWrite) return;            // 响应没发完：暂停收包（内核缓冲保留，
    }                                         // LT 电平触发稍后会再次通知）
}

// 状态机主泵：循环"冲响应 / 解析下一请求"，直到无事可做
void HttpConn::TryProcess() {
    while (!closing_) {
        if (st_ == kWrite) {                  // 上一响应还在发：先冲
            FlushSettle();
            if (closing_ || st_ == kWrite) return;
        }
        if (st_ != kHead) return;             // kLive/kDelay：不接受新请求
        if (in_.size() > kMaxInBuf) {
            RespondError("400 Bad Request", "request too large");
            return;
        }

        if (!head_done_) {                    // 阶段一：攒齐并解析请求头
            size_t hend = in_.find("\r\n\r\n");
            if (hend == std::string::npos) {
                // 没收齐不会无限攒：总量受上面 kMaxInBuf 约束
                if (in_.size() > kMaxHead) {
                    RespondError("400 Bad Request", "header too large");
                    return;
                }
                return;                       // 头没收齐，等下一批数据
            }
            // 完整头部也要查长度：一次性到达的超大头不能绕过检查
            if (hend + 4 > kMaxHead) {
                RespondError("400 Bad Request", "header too large");
                return;
            }
            if (!ParseHead(in_, hend, &req_)) {
                RespondError("400 Bad Request", "malformed request");
                return;
            }
            head_done_ = true;
            head_len_  = hend + 4;
            need_body_ = 0;
            if (req_.method == "POST") {
                if (req_.content_length < 0 || (size_t)req_.content_length > kMaxBody) {
                    RespondError("413 Payload Too Large", "body too large");
                    return;
                }
                need_body_ = (size_t)req_.content_length;
            }
        }

        // 阶段二：POST 等 body 收齐
        if (in_.size() - head_len_ < need_body_) return;

        // 请求完整：从缓冲消费掉，然后路由处理
        size_t body_len = need_body_;
        in_.erase(0, head_len_ + body_len);
        head_done_ = false; head_len_ = 0; need_body_ = 0;
        HandleRequest(body_len);
    }
}

void HttpConn::HandleRequest(size_t body_len) {
    srv_.AddRequest();
    if (VerboseLog())
        LogLine("conn %s %s %s", peer_.c_str(), req_.method.c_str(), req_.path.c_str());

    if (req_.method == "GET") {
        if (req_.path.rfind("/live/", 0)   == 0) { StartLive();     return; }
        if (req_.path.rfind("/slow/", 0)   == 0) { StartDelay();    return; }
        if (req_.path.rfind("/static/", 0) == 0) { RespondStatic(); return; }
        RespondError("404 Not Found", "no such path");
        return;
    }
    if (req_.method == "POST") {
        // 与原版一致只回显字节数，body 内容不需要拷贝
        char text[128];
        snprintf(text, sizeof text, "POST OK, server received %zu bytes\n", body_len);
        AppendHead(&out_, "200 OK", "text/plain", strlen(text), req_.keep_alive);
        out_.append(text);
        FlushSettle();
        return;
    }
    RespondError("405 Method Not Allowed", "method not supported");
}


void HttpConn::RespondStatic() {
    int kb = ParseKb(8, req_.path);           // "/static/" 长度 8
    if (kb < 0) { RespondError("413 Payload Too Large", "static size too large"); return; }
    if (kb == 0) kb = 4;                      // 与原版一致：非法大小回落 4KB
    AppendStaticResponse(&out_, kb, req_.keep_alive);
    FlushSettle();
}

void HttpConn::StartLive() {
    if (VerboseLog()) LogLine("conn %s live start %s", peer_.c_str(), req_.path.c_str());
    st_ = kLive;
    out_ += "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    last_frame_ms_ = 0;                       // 下一轮 Tick 立即推第一帧
    srv_.AddTick(this);
    eng_.AddIoEvent(fd_, kWriteEvent, this, NULL);
    TryFlush();                               // 响应头先冲出去；没发完等 OnWrite
}

void HttpConn::StartDelay() {
    int kb = ParseKb(6, req_.path);           // "/slow/" 长度 6
    if (kb < 0) { RespondError("413 Payload Too Large", "static size too large"); return; }
    if (kb == 0) kb = 4;
    kb_ = kb;
    st_ = kDelay;
    due_ms_ = NowMs() + kSlowMs;
    srv_.AddTick(this);
}

void HttpConn::RespondError(const char* status, const char* why) {
    LogLine("conn %s -> %s (%s)", peer_.c_str(), status, why);
    req_.keep_alive = false;                  // 出错一律发完响应就断开
    char text[128];
    snprintf(text, sizeof text, "%s: %s\n", status, why);
    out_.clear();
    AppendHead(&out_, status, "text/plain", strlen(text), false);
    out_.append(text);
    FlushSettle();
}


void HttpConn::OnTick(long long now) {
    if (closing_) return;

    if (st_ == kDelay) {                      // /slow/ 到期：回静态响应（短连接）
        if (now < due_ms_) return;
        srv_.RemoveTick(this);
        AppendStaticResponse(&out_, kb_, false);
        st_ = kWrite;
        eng_.AddIoEvent(fd_, kWriteEvent, this, NULL);
        TryFlush();
        return;
    }

    if (st_ == kLive) {                       // 直播：100ms 定帧
        if (last_frame_ms_ != 0 && now - last_frame_ms_ < kFrameMs) return;
        last_frame_ms_ = now;
        char frame[64];
        snprintf(frame, sizeof frame, "frame ts=%lld live-data\n", now);
        out_.append(frame);
        eng_.AddIoEvent(fd_, kWriteEvent, this, NULL);
        TryFlush();
    }
}


void HttpConn::OnWrite(FD, void*, int32_t) {
    if (closing_) return;
    if (st_ == kLive) { TryFlush(); return; } // 定时器追加的数据照常冲刷
    if (st_ != kWrite) return;                // kHead/kDelay：残留关注，无数据可发
    FlushSettle();                            // -> kHead（发完）/ 仍 kWrite / Close
    if (!closing_ && st_ == kHead && !in_.empty())
        TryProcess();                         // 处理写响应期间攒下的下一个请求
}

void HttpConn::TryFlush() {
    while (!out_.empty()) {
        ssize_t n = send(fd_, out_.data(), out_.size(), 0);
        if (n > 0) { out_.erase(0, (size_t)n); continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        LogLine("conn %s send error: %s", peer_.c_str(), strerror(errno));
        Close("send error");
        return;
    }
    eng_.DeleteIoEvent(fd_, kWriteEvent);     // 发完：撤可写，防止 select 空转
}

void HttpConn::FlushSettle() {
    TryFlush();
    if (closing_) return;
    if (out_.empty()) {
        if (req_.keep_alive) { st_ = kHead; return; }   // 复用连接，回去收下一请求
        Close("done");
        return;
    }
    st_ = kWrite;                             // 没发完：等可写继续
    eng_.AddIoEvent(fd_, kWriteEvent, this, NULL);
}


// 监听 fd 可读：循环 accept 直到 EAGAIN
void HttpServer::OnRead(FD, void*, int32_t) {
    for (;;) {
        sockaddr_in cli{};
        socklen_t clen = sizeof cli;
        FD cfd = accept(listen_fd_, (sockaddr*)&cli, &clen);
        if (cfd < 0) return;                  // EAGAIN/EMFILE 等：本轮结束

        if (SetNonblock(cfd) != kOk) { close(cfd); continue; }
        SetTcpNodelay(cfd);                   // 低延迟关键项之一

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, ip, sizeof ip);
        char peer[64];
        snprintf(peer, sizeof peer, "%s:%u", ip, (unsigned)ntohs(cli.sin_port));

        HttpConn* c = new HttpConn(*this, engine_, cfd, peer);
        if (engine_.AddIoEvent(cfd, kReadEvent, c, NULL) != kOk) {
            delete c;                         // 未注册任何事件（多为超出 select fd
            continue;                         // 上限），析构兜底关 fd，直接删安全
        }
        ++conns_;
    }
}

// 引擎每轮末尾：定时驱动 + 统一回收 + 周期统计
void HttpServer::Tick() {
    long long now = NowMs();
    // 倒序遍历：OnTick 内部可能把自己从列表摘除（慢响应到期/连接关闭）
    for (int i = (int)ticking_.size() - 1; i >= 0; --i)
        ticking_[i]->OnTick(now);

    // 延迟删除统一收口：回调栈里从不 delete this，这里才是对象的真正终点
    for (size_t i = 0; i < dead_.size(); ++i) delete dead_[i];
    dead_.clear();

    Stats(now);
}

void HttpServer::AddTick(HttpConn* c)    { ticking_.push_back(c); }
void HttpServer::RemoveTick(HttpConn* c) {
    for (size_t i = 0; i < ticking_.size(); ++i)
        if (ticking_[i] == c) { ticking_.erase(ticking_.begin() + i); return; }
}
void HttpServer::Recycle(HttpConn* c)    { dead_.push_back(c); }

void HttpServer::Stats(long long now) {
    if (last_stats_ms_ == 0) { last_stats_ms_ = now; return; }
    if (now - last_stats_ms_ < 5000) return;
    last_stats_ms_ = now;
    LogLine("stats: conns=%lld reqs=%lld ticking=%zu",
            conns_, requests_, ticking_.size());
}
