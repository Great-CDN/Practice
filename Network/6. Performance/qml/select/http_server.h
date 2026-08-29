#pragma once
#include "event_engine.h"
#include <string>
#include <vector>

class HttpConn;


class HttpServer : public IIoHandler {
public:
    HttpServer(IEventEngine& eng, FD listen_fd)
        : engine_(eng), listen_fd_(listen_fd) {}

    // IIoHandler：监听 fd 可读 = 有新连接；监听 fd 不会可写
    void OnRead (FD fd, void* data, int32_t mask) override;
    void OnWrite(FD, void*, int32_t) override {}

    // 引擎每轮回调（由 OnLoop 钩子转发到这里）
    void Tick();

    // ---- 供 HttpConn 调用 ----
    void AddTick(HttpConn* c);        // 加入定时驱动（直播/慢响应）
    void RemoveTick(HttpConn* c);     // 移出定时驱动
    void Recycle(HttpConn* c);        // 连接已关闭：登记延迟删除
    void AddRequest() { ++requests_; }

private:
    void Stats(long long now);

    IEventEngine&          engine_;
    FD                     listen_fd_;
    std::vector<HttpConn*> ticking_;       // 需要定时驱动的连接
    std::vector<HttpConn*> dead_;          // 已关闭、待删除的连接
    long long              conns_ = 0;     // 累计连接数
    long long              requests_ = 0;  // 累计请求数
    long long              last_stats_ms_ = 0;
};
