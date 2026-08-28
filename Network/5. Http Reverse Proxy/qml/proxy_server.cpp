#include "select_engine.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <ctime>
#include <cstdint>
#include <string>
#include <utility>
#include <map>
#include <vector>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

enum { DEFAULT_LISTEN = 8080, DEFAULT_ORIGIN_PORT = 8090 };

const size_t kMaxHead       = 8192;         // 下游请求头上限
const size_t kMaxBody       = 8 << 20;      // 下游 POST body 上限（8MB）
const size_t kCacheMaxEntry = 16 << 20;     // 单条缓存上限（16MB）
const size_t kCacheTotal    = 64 << 20;     // 缓存总容量（64MB，FIFO 淘汰）
const size_t kViewerCap     = 1 << 20;      // 直播 viewer 缓冲上限（1MB，超限踢掉慢观众）

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

// 从请求头文本解析：请求行 + Content-Length
struct HttpHead {
    std::string method, path, version;
    long long content_length = 0;
};
static bool ParseHead(const std::string& in, HttpHead* h) {
    size_t line_end = in.find("\r\n");
    if (line_end == std::string::npos) return false;
    std::string line = in.substr(0, line_end);
    size_t sp1 = line.find(' ');
    size_t sp2 = (sp1 == std::string::npos) ? std::string::npos : line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    h->method  = line.substr(0, sp1);
    h->path    = line.substr(sp1 + 1, sp2 - sp1 - 1);
    h->version = line.substr(sp2 + 1);
    if (h->path.empty() || h->path[0] != '/') return false;

    for (size_t pos = line_end + 2; pos + 1 < in.size(); ) {
        size_t e = in.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;   // 空行 = 头结束
        std::string kv = in.substr(pos, e - pos);
        size_t c = kv.find(':');
        if (c != std::string::npos) {
            std::string key = kv.substr(0, c), val = kv.substr(c + 1);
            for (size_t i = 0; i < key.size(); ++i)
                if (key[i] >= 'A' && key[i] <= 'Z') key[i] += 32;
            while (!val.empty() && val[0] == ' ') val.erase(0, 1);
            if (key == "content-length") h->content_length = atoll(val.c_str());
        }
        pos = e + 2;
    }
    return true;
}

// ---------- 前置声明 ----------
class Downstream;
class OriginFetch;
class LiveGroup;
class Proxy;

// ---------- 缓存：url -> 完整响应（头+体），容量上限 FIFO 淘汰 ----------
class Cache {
public:
    bool Get(const std::string& url, std::string* out) {
        auto it = entries_.find(url);
        if (it == entries_.end()) return false;
        it->second.ts = time(NULL);            // 命中即续期
        *out = it->second.resp;
        return true;
    }
    void Store(const std::string& url, const std::string& resp) {
        if (resp.size() > kCacheMaxEntry) return;
        auto it = entries_.find(url);
        if (it != entries_.end()) total_ -= it->second.resp.size();
        entries_[url] = Entry{resp, time(NULL)};
        total_ += resp.size();
        while (total_ > kCacheTotal) EvictOne();
    }
private:
    struct Entry { std::string resp; time_t ts; };
    void EvictOne() {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.ts < oldest->second.ts) oldest = it;
        if (oldest == entries_.end()) return;
        total_ -= oldest->second.resp.size();
        entries_.erase(oldest);
    }
    std::map<std::string, Entry> entries_;
    size_t total_ = 0;
};

// ---------- 全局上下文 ----------
class Proxy {
public:
    Proxy(SelectEngine& e) : engine(e) {}
    SelectEngine&   engine;
    Cache           cache;
    std::string     origin_host = "127.0.0.1";
    int             origin_port = DEFAULT_ORIGIN_PORT;
    std::map<std::string, OriginFetch*> fetches;   // url -> 在途回源（合并回源的挂靠点）
    std::map<std::string, LiveGroup*>   lives;     // url -> 直播拉流组
    long            next_id = 1;
};

// ---------- 下游会话：浏览器 / 播放器 ----------
class Downstream : public IIoHandler {
public:
    Downstream(Proxy& p, SelectEngine& eng, FD fd, long id, std::string peer);
    ~Downstream() override;

    void OnRead (FD fd, void* data, int32_t mask) override;
    void OnWrite(FD fd, void* data, int32_t mask) override;

    // ---- 供 OriginFetch / LiveGroup 调用 ----
    void Feed(const char* s, size_t n);            // 扇出数据进下游
    size_t OutSize() const { return out_.size(); }
    void UpstreamDone(const char* how);            // 上游正常结束：发完即关
    void Abort(const char* status);                // 上游异常：回错误响应后关
    void Close(const char* reason);                // 立即收尾（组踢人用）
    void SetFetch(OriginFetch* f) { fetch_ = f; }
    void SetLive (LiveGroup*  g) { live_  = g; }
    void ClearFetch() { fetch_ = nullptr; }
    void ClearLive()  { live_  = nullptr; }
    long Id() const { return id_; }
    const std::string& Peer() const { return peer_; }

private:
    void ClientRecv();     // 收下游请求（或发送期感知断开）
    void ClientWrite();    // 冲下游 out_
    void PassRecv();       // POST 直通：收上游响应
    void PassWrite();      // POST 直通：连上游/发请求
    void TryRoute();
    void HandleGet();      // 缓存 / 合并回源 / 新建回源
    void HandlePost();     // 直通
    void JoinLive();       // 直播
    void RespondError(const char* status, const char* why);
    void Finish(const char* how) { finished_ = true; done_how_ = how; if (out_.empty()) Close(how); }

    Proxy&        proxy_;
    SelectEngine& engine_;
    FD            fd_;
    long          id_;
    std::string   peer_;
    std::string   in_, out_;
    std::string   method_, path_;
    size_t        head_end_ = 0, need_body_ = 0;
    // 连接状态机：kHead 收请求头 / kBodyWait 等 POST body / kStream 推流 /
    // kPass POST 直通 / kRespond 错误响应待发送
    enum St { kHead, kBodyWait, kStream, kPass, kRespond } st_ = kHead;
    std::string   body_;
    bool          finished_ = false, closing_ = false;
    std::string   done_how_ = "done";

    OriginFetch* fetch_ = nullptr;   // 所属静态回源组（合并回源的挂靠点）
    LiveGroup*  live_  = nullptr;   // 所属直播组

    FD            pfd_ = -1;        // POST 直通的上游 fd
    bool          pconn_ = false;   // 上游 connect 是否已完成
    size_t        preq_sent_ = 0;   // 已发送的请求字节数（注意必须是 size_t，
                                    // 之前误写成 bool，导致 += 永远等于 1，无限重发）
    std::string   preq_;
    int           pass_retry_ = 0;  // POST 直通：连接被重置时的重试计数
};

// ---------- 静态回源组：1 条上游连接服务 N 个下游（合并回源 + 结果入缓存） ----------
class OriginFetch : public IIoHandler {
public:
    OriginFetch(Proxy& p, std::string url);
    ~OriginFetch() override;
    void OnRead (FD fd, void* data, int32_t mask) override;
    void OnWrite(FD fd, void* data, int32_t mask) override;
    void Attach(Downstream* d);    // 加入订阅：立刻补发已收到的前缀
    void Detach(Downstream* d);    // 退出订阅：没人了就中止回源
private:
    void Fail(const char* status);
    void Complete();
    void CloseSelf(const char* why);

    Proxy&        proxy_;
    std::string   url_;
    FD            ufd_ = -1;
    bool          connecting_ = true, dead_ = false, done_ = false, too_big_ = false;
    std::string   req_, resp_;
    size_t        req_sent_ = 0;
    std::vector<Downstream*> subs_;
};

// ---------- 直播组：1 条上游拉流 + N 个 viewer，来多少转多少，永不"完成" ----------
class LiveGroup : public IIoHandler {
public:
    LiveGroup(Proxy& p, std::string url);
    ~LiveGroup() override;
    void OnRead (FD fd, void* data, int32_t mask) override;
    void OnWrite(FD fd, void* data, int32_t mask) override;
    void Attach(Downstream* d);
    void Detach(Downstream* d);
    size_t ViewerCount() const { return subs_.size(); }
private:
    void FanOut(const char* s, size_t n);
    void CloseSelf(const char* why);

    Proxy&        proxy_;
    std::string   url_;
    FD            ufd_ = -1;
    bool          connecting_ = true, dead_ = false, hdr_done_ = false;
    std::string   req_, hdr_;                // req_: 发给源站的拉流请求
    size_t        req_sent_ = 0;
    std::vector<Downstream*> subs_;
};


// Downstream 实现

Downstream::Downstream(Proxy& p, SelectEngine& eng, FD fd, long id, std::string peer)
    : proxy_(p), engine_(eng), fd_(fd), id_(id), peer_(std::move(peer)) {
    LogLine(id_, peer_.c_str(), "connected");
}
Downstream::~Downstream() {
    if (fd_  >= 0) close(fd_);
    if (pfd_ >= 0) close(pfd_);
}

void Downstream::OnRead(FD fd, void* data, int32_t mask) {
    if (fd == pfd_) { PassRecv(); return; }
    ClientRecv();
}
void Downstream::OnWrite(FD fd, void* data, int32_t mask) {
    if (fd == pfd_) { PassWrite(); return; }
    ClientWrite();
}

// 收下游：kHead/kBodyWait 阶段攒请求；kStream/kPass 阶段只用来感知断开
void Downstream::ClientRecv() {
    char buf[4096];
    for (;;) {
        ssize_t n = recv(fd_, buf, sizeof buf, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            Close("recv error"); return;
        }
        if (n == 0) { Close("aborted by peer"); return; }
        if (st_ == kStream || st_ == kRespond || st_ == kPass) continue;   // 多余数据丢弃

        in_.append(buf, (size_t)n);
        if (in_.size() > kMaxHead) { RespondError("400 Bad Request", "header too large"); return; }

        size_t hend = in_.find("\r\n\r\n");
        if (hend == std::string::npos) continue;   // 头没收完，等下一段

        HttpHead h;
        if (!ParseHead(in_, &h)) { RespondError("400 Bad Request", "malformed request"); return; }
        method_ = h.method; path_ = h.path; head_end_ = hend + 4;

        if (method_ != "GET" && method_ != "POST") {
            RespondError("405 Method Not Allowed", method_.c_str()); return;
        }
        if (method_ == "POST") {                   // POST：还要等 body 收齐
            if (h.content_length < 0 || (size_t)h.content_length > kMaxBody) {
                RespondError("413 Payload Too Large", "body too large"); return;
            }
            need_body_ = (size_t)h.content_length;
            if (in_.size() - head_end_ >= need_body_) {
                body_ = in_.substr(head_end_, need_body_);
                HandlePost();
            } else {
                st_ = kBodyWait;                   // body 还没到齐，继续攒
            }
            return;
        }
        TryRoute();                                // GET：头齐即可路由
        return;
    }
}

// GET 路由：直播 / 缓存 / 合并回源
void Downstream::TryRoute() {
    if (path_.rfind("/live/", 0) == 0) { JoinLive(); return; }
    HandleGet();
}

void Downstream::HandleGet() {
    st_ = kStream;
    // 1) 缓存命中：直接回，不碰源站
    std::string resp;
    if (proxy_.cache.Get(path_, &resp)) {
        LogLine(id_, peer_.c_str(), "GET %s -> 200 (cache HIT, %zu bytes)",
                path_.c_str(), resp.size());
        finished_ = true; done_how_ = "cache hit served";
        Feed(resp.data(), resp.size());
        return;
    }
    // 2) 在途回源：同 URL 已有人回源，合并复用那条连接
    auto fit = proxy_.fetches.find(path_);
    if (fit != proxy_.fetches.end()) {
        LogLine(id_, peer_.c_str(), "GET %s -> 合并回源 (attach to in-flight fetch)",
                path_.c_str());
        fit->second->Attach(this);
        return;
    }
    // 3) 未命中：新建回源连接
    LogLine(id_, peer_.c_str(), "GET %s -> cache MISS, connect origin", path_.c_str());
    OriginFetch* f = new OriginFetch(proxy_, path_);
    proxy_.fetches[path_] = f;
    f->Attach(this);
}

// 直播：加入该 URL 的拉流组（首个人创建拉流连接，后来者直接挂靠扇出）
void Downstream::JoinLive() {
    st_ = kStream;
    auto it = proxy_.lives.find(path_);
    LiveGroup* g;
    if (it != proxy_.lives.end()) {
        g = it->second;
        LogLine(id_, peer_.c_str(), "LIVE %s -> join existing pull (viewer %zu)",
                path_.c_str(), g->ViewerCount() + 1);
    } else {
        g = new LiveGroup(proxy_, path_);
        proxy_.lives[path_] = g;
        LogLine(id_, peer_.c_str(), "LIVE %s -> cache MISS, new origin pull",
                path_.c_str());
    }
    g->Attach(this);
}

// POST：直通源站（单独一条上游连接，不合并不缓存；连接被重置自动重试一次）
void Downstream::HandlePost() {
    body_ = in_.substr(head_end_, need_body_);
    st_ = kPass;
    if (++pass_retry_ > 2) { Abort("502 Bad Gateway"); return; }   // 最多尝试 2 次
    pfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (pfd_ < 0) { Abort("502 Bad Gateway"); return; }
    set_nonblock(pfd_);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)proxy_.origin_port);
    if (inet_pton(AF_INET, proxy_.origin_host.c_str(), &a.sin_addr) != 1) {
        Abort("502 Bad Gateway"); return;
    }
    int r = connect(pfd_, (sockaddr*)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) {   // 立即失败：换新连接重试
        close(pfd_); pfd_ = -1;
        return HandlePost();
    }
    engine_.AddIoEvent(pfd_, kWriteEvent, this, NULL);   // 可写 = 连接完成（或失败）
    LogLine(id_, peer_.c_str(), "POST %s -> pass-through attempt %d (%zu bytes body)",
            path_.c_str(), pass_retry_, body_.size());
}

// POST 直通：上游可写 = 连接建立，发请求；发完转成只读等响应
void Downstream::PassWrite() {
    if (!pconn_) {
        int err = 0; socklen_t l = sizeof err;
        getsockopt(pfd_, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) {
            if (pass_retry_ <= 1) {   // 连接被重置/拒绝：换新连接重试一次
                LogLine(id_, peer_.c_str(), "[retry] connect failed errno=%d, reconnect", err);
                engine_.DeleteIoEvent(pfd_, kReadEvent | kWriteEvent);
                close(pfd_); pfd_ = -1; pconn_ = false; preq_sent_ = 0;
                return HandlePost();
            }
            LogLine(id_, peer_.c_str(), "[dbg] connect failed, errno=%d", err);
            Abort("502 Bad Gateway");
            return; }
        pconn_ = true;
        preq_ = "POST " + path_ + " HTTP/1.1\r\n"
                "Host: " + proxy_.origin_host + ":" +
                std::to_string(proxy_.origin_port) + "\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                "Connection: close\r\n\r\n" + body_;
        preq_sent_ = 0;
    }
    while (preq_sent_ < preq_.size()) {
        ssize_t n = send(pfd_, preq_.data() + preq_sent_, preq_.size() - preq_sent_, 0);
        if (n > 0) { preq_sent_ += (size_t)n; continue; }
        if (errno == EINTR) continue;
        if (errno == ECONNRESET && pass_retry_ <= 1) {   // 被源站重置：换新连接重发一次
            LogLine(id_, peer_.c_str(), "[retry] ECONNRESET, reconnect origin");
            engine_.DeleteIoEvent(pfd_, kReadEvent | kWriteEvent);
            close(pfd_); pfd_ = -1; pconn_ = false; preq_sent_ = 0;
            return HandlePost();
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        LogLine(id_, peer_.c_str(), "[dbg] send to origin failed, errno=%d", errno);
        Abort("502 Bad Gateway"); return;
    }
    // 请求发完：撤销可写，订阅可读等响应
    engine_.DeleteIoEvent(pfd_, kWriteEvent);
    engine_.AddIoEvent(pfd_, kReadEvent, this, NULL);
}

// POST 直通：收上游响应，原样喂给下游
void Downstream::PassRecv() {
    char buf[16384];
    for (;;) {
        ssize_t n = recv(pfd_, buf, sizeof buf, 0);
        if (n > 0) { Feed(buf, (size_t)n); continue; }
        if (n == 0) { Finish("upstream done"); return; }   // 响应结束
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        LogLine(id_, peer_.c_str(), "[dbg] recv from origin failed, errno=%d", errno);
        Abort("502 Bad Gateway"); return;
    }
}

// 冲下游 out_：分段发送核心逻辑（与文件服务器一致）
void Downstream::ClientWrite() {
    while (!out_.empty()) {
        ssize_t n = send(fd_, out_.data(), out_.size(), 0);
        if (n > 0) {
            LogLine(id_, peer_.c_str(), "send  %zd bytes (%zu bytes pending)",
                    n, out_.size() - (size_t)n);
            out_.erase(0, (size_t)n);
            continue;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        Close("send error"); return;
    }
    if (finished_) Close(done_how_.c_str());   // 数据发完且上游已结束 → 收尾
}

void Downstream::Feed(const char* s, size_t n) {
    if (closing_ || n == 0) return;
    out_.append(s, n);
    engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
}

void Downstream::UpstreamDone(const char* how) {
    Finish(how);
}

void Downstream::Abort(const char* status) {
    if (closing_) return;
    RespondError(status, "upstream error");
}

void Downstream::RespondError(const char* status, const char* why) {
    LogLine(id_, peer_.c_str(), "-> %s (%s)", status, why);
    std::string body = std::string(status) + "\n";
    char hdr[512];
    snprintf(hdr, sizeof hdr,
             "HTTP/1.1 %s\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %zu\r\n"
             "Connection: close\r\n"
             "Server: mini-proxy-select/1.0\r\n"
             "\r\n", status, body.size());
    out_ = std::string(hdr) + body;
    in_.clear();
    st_ = kRespond;
    finished_ = true; done_how_ = "error response sent";
    engine_.AddIoEvent(fd_, kReadEvent | kWriteEvent, this, NULL);
}

void Downstream::Close(const char* reason) {
    if (closing_) return;
    closing_ = true;
    LogLine(id_, peer_.c_str(), "closed (%s)", reason);
    if (fetch_) { OriginFetch* f = fetch_; fetch_ = nullptr; f->Detach(this); }
    if (live_)  { LiveGroup*  g = live_;  live_  = nullptr; g->Detach(this); }
    engine_.DeleteIoEvent(fd_, kReadEvent | kWriteEvent);
    if (fd_  >= 0) { close(fd_);  fd_  = -1; }
    if (pfd_ >= 0) {
        engine_.DeleteIoEvent(pfd_, kReadEvent | kWriteEvent);
        close(pfd_); pfd_ = -1;
    }
    delete this;
}

// ============================================================================
// OriginFetch 实现：一条回源连接 + N 个订阅下游
// ============================================================================
OriginFetch::OriginFetch(Proxy& p, std::string url)
    : proxy_(p), url_(std::move(url)) {
    req_ = "GET " + url_ + " HTTP/1.1\r\n"
           "Host: " + p.origin_host + ":" + std::to_string(p.origin_port) + "\r\n"
           "Connection: close\r\n"          // 用 EOF 界定响应结束，简化解析
           "\r\n";
    ufd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (ufd_ < 0) { dead_ = true; return; }
    set_nonblock(ufd_);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)p.origin_port);
    if (inet_pton(AF_INET, p.origin_host.c_str(), &a.sin_addr) != 1) { dead_ = true; return; }
    int r = connect(ufd_, (sockaddr*)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) { dead_ = true; return; }
    p.engine.AddIoEvent(ufd_, kWriteEvent, this, NULL);
}
OriginFetch::~OriginFetch() {
    if (ufd_ >= 0) close(ufd_);
}

void OriginFetch::Attach(Downstream* d) {
    if (done_ || dead_) {
        d->SetFetch(nullptr);
        d->Abort("502 Bad Gateway");
        return;
    }
    d->SetFetch(this);
    subs_.push_back(d);
    if (!resp_.empty()) d->Feed(resp_.data(), resp_.size());   // 补发已收到的前缀
}

void OriginFetch::Detach(Downstream* d) {
    auto it = std::find(subs_.begin(), subs_.end(), d);
    if (it != subs_.end()) subs_.erase(it);
    if (subs_.empty() && !done_) {        // 没人要了：中止回源，避免空转
        CloseSelf("all subscribers left, abort origin pull");
    }
}

void OriginFetch::OnWrite(FD, void*, int32_t) {
    if (connecting_) {                     // 非阻塞 connect 的完成判定
        int err = 0; socklen_t l = sizeof err;
        getsockopt(ufd_, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) { Fail("502 Bad Gateway"); return; }
        connecting_ = false;
    }
    while (req_sent_ < req_.size()) {
        ssize_t n = send(ufd_, req_.data() + req_sent_, req_.size() - req_sent_, 0);
        if (n > 0) { req_sent_ += (size_t)n; continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        Fail("502 Bad Gateway"); return;
    }
    proxy_.engine.DeleteIoEvent(ufd_, kWriteEvent);        // 请求发完
    proxy_.engine.AddIoEvent(ufd_, kReadEvent, this, NULL); // 转为只读
}

void OriginFetch::OnRead(FD, void*, int32_t) {
    char buf[65536];
    ssize_t n = recv(ufd_, buf, sizeof buf, 0);
    if (n == 0) { Complete(); return; }                    // Connection: close，EOF 即响应结束
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        Fail("502 Bad Gateway"); return;
    }
    if (resp_.size() > kCacheMaxEntry) too_big_ = true;    // 超大：照常转发，只是不缓存
    resp_.append(buf, (size_t)n);
    for (Downstream* d : subs_) d->Feed(buf, (size_t)n);   // 扇出给所有订阅者
}

void OriginFetch::Fail(const char* status) {
    for (Downstream* d : subs_) { d->ClearFetch(); d->Abort(status); }
    subs_.clear();
    CloseSelf("origin failed");
}

void OriginFetch::Complete() {
    done_ = true;
    if (!too_big_)
        proxy_.cache.Store(url_, resp_);                   // 回源结果入缓存，服务后来者
    proxy_.fetches.erase(url_);
    for (Downstream* d : subs_) { d->ClearFetch(); d->UpstreamDone("origin done"); }
    subs_.clear();
    CloseSelf("origin done, response cached");
}

void OriginFetch::CloseSelf(const char* why) {
    proxy_.engine.DeleteIoEvent(ufd_, kReadEvent | kWriteEvent);
    close(ufd_); ufd_ = -1;
    delete this;
}

LiveGroup::LiveGroup(Proxy& p, std::string url)
    : proxy_(p), url_(std::move(url)) {
    req_ = "GET " + url_ + " HTTP/1.1\r\n"
           "Host: " + p.origin_host + ":" + std::to_string(p.origin_port) + "\r\n"
           "Connection: close\r\n"
           "\r\n";
    ufd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (ufd_ < 0) { dead_ = true; return; }
    set_nonblock(ufd_);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port   = htons((unsigned short)p.origin_port);
    if (inet_pton(AF_INET, p.origin_host.c_str(), &a.sin_addr) != 1) { dead_ = true; return; }
    int r = connect(ufd_, (sockaddr*)&a, sizeof a);
    if (r < 0 && errno != EINPROGRESS) { dead_ = true; return; }
    p.engine.AddIoEvent(ufd_, kWriteEvent, this, NULL);
}
LiveGroup::~LiveGroup() {
    if (ufd_ >= 0) close(ufd_);
}

void LiveGroup::Attach(Downstream* d) {
    if (dead_) { d->SetLive(nullptr); d->Abort("502 Bad Gateway"); return; }
    d->SetLive(this);
    subs_.push_back(d);
    if (hdr_done_) d->Feed(hdr_.data(), hdr_.size());   // 中途加入：先补上游响应头
}

void LiveGroup::Detach(Downstream* d) {
    auto it = std::find(subs_.begin(), subs_.end(), d);
    if (it != subs_.end()) subs_.erase(it);
    if (subs_.empty()) CloseSelf("no viewers left, disconnect origin");   // 没观众就断源
}

void LiveGroup::OnWrite(FD, void*, int32_t) {
    if (connecting_) {   // 可写 = 非阻塞 connect 完成，检查是否成功
        int err = 0; socklen_t l = sizeof err;
        getsockopt(ufd_, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err) { CloseSelf("origin connect failed"); return; }
        connecting_ = false;
    }
    // 发送拉流请求（源站要先收到 GET 才会开始推流）
    while (req_sent_ < req_.size()) {
        ssize_t n = send(ufd_, req_.data() + req_sent_, req_.size() - req_sent_, 0);
        if (n > 0) { req_sent_ += (size_t)n; continue; }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        CloseSelf("origin send failed"); return;
    }
    // 请求发完：撤销可写，转只读持续收流
    proxy_.engine.DeleteIoEvent(ufd_, kWriteEvent);
    proxy_.engine.AddIoEvent(ufd_, kReadEvent, this, NULL);
}

void LiveGroup::OnRead(FD, void*, int32_t) {
    char buf[65536];
    ssize_t n = recv(ufd_, buf, sizeof buf, 0);
    if (n == 0) {   // 源停播：所有 viewer 结束
        for (Downstream* d : subs_) { d->ClearLive(); d->UpstreamDone("live stream ended"); }
        subs_.clear();
        CloseSelf("live source ended");
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        CloseSelf("origin read error"); return;
    }
    if (!hdr_done_) {   // 先截取上游响应头：中途加入的 viewer 需要它
        hdr_.append(buf, (size_t)n);
        size_t p = hdr_.find("\r\n\r\n");
        if (p == std::string::npos) return;            // 头还没齐，继续攒
        hdr_done_ = true;
        std::string head = hdr_.substr(0, p + 4);
        std::string tail = hdr_.substr(p + 4);
        for (Downstream* d : subs_) d->Feed(head.data(), head.size());
        FanOut(tail.data(), tail.size());
        return;
    }
    FanOut(buf, (size_t)n);
}

// 扇出 + 简单流控：积压超过上限的慢 viewer 直接踢掉（直播不等人）
void LiveGroup::FanOut(const char* s, size_t n) {
    std::vector<Downstream*> victims;
    for (Downstream* d : subs_) {
        d->Feed(s, n);
        if (d->OutSize() > kViewerCap) victims.push_back(d);
    }
    for (Downstream* d : victims) {
        d->ClearLive();
        auto it = std::find(subs_.begin(), subs_.end(), d);
        if (it != subs_.end()) subs_.erase(it);
        d->Close("viewer too slow, dropped");
    }
}

void LiveGroup::CloseSelf(const char* why) {
    proxy_.lives.erase(url_);
    proxy_.engine.DeleteIoEvent(ufd_, kReadEvent | kWriteEvent);
    close(ufd_); ufd_ = -1;
    delete this;
}

class ProxyServer : public IIoHandler {
public:
    ProxyServer(Proxy& p, FD listenfd) : proxy_(p), engine_(p.engine), listen_fd_(listenfd) {}

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
            Downstream* s = new Downstream(proxy_, engine_, cfd, proxy_.next_id++, peer);
            engine_.AddIoEvent(cfd, kReadEvent, s, NULL);
        }
    }
    void OnWrite(FD, void*, int32_t) override {}

private:
    Proxy&        proxy_;
    SelectEngine& engine_;
    FD            listen_fd_;
};

int main(int argc, char** argv) {
    // 用法：./proxy [源站IP] [源站端口] [监听端口]
    const char* ohost = (argc > 1) ? argv[1] : "127.0.0.1";
    int oport = (argc > 2) ? atoi(argv[2]) : DEFAULT_ORIGIN_PORT;
    int lport = (argc > 3) ? atoi(argv[3]) : DEFAULT_LISTEN;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    SelectEngine engine;
    Proxy proxy(engine);
    proxy.origin_host = ohost;
    proxy.origin_port = oport;

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); return EXIT_FAILURE; }
    int on = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    sockaddr_in servaddr{};
    servaddr.sin_family      = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port        = htons((unsigned short)lport);
    if (bind(listenfd, (sockaddr*)&servaddr, sizeof servaddr) < 0) { perror("bind"); return EXIT_FAILURE; }
    if (listen(listenfd, 16) < 0) { perror("listen"); return EXIT_FAILURE; }
    set_nonblock(listenfd);

    ProxyServer acceptor(proxy, listenfd);
    engine.AddIoEvent(listenfd, kReadEvent, &acceptor, NULL);

    char ts[32];
    time_t now = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    printf("[%s] [server] [-] reverse proxy listening on 0.0.0.0:%d -> origin %s:%d (select engine)\n",
           ts, lport, ohost, oport);

    return engine.Run();
}
