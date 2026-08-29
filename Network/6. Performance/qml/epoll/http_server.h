#pragma once
#include "event_engine.h"
#include <string>
#include <vector>

class HttpConn;

// ============================================================================
// HttpServer —— HTTP 服务器主体（单线程、事件驱动）
//
// 职责：
//   1. accept 新连接，创建 HttpConn（连接内部逻辑见 http_server.cpp）
//   2. 引擎每轮末尾被调 Tick()：驱动直播定帧 / 慢响应到期 / 回收已关闭连接
//
// 生命周期与资源管理（无泄漏的关键设计）：
//   * HttpConn::Close() 是连接的唯一收尾出口：立即注销事件、关闭 fd、
//     移出定时列表，但对象本身进入 dead_ 延迟删除，由下一轮 Tick() 末尾
//     统一 delete —— 回调栈里绝不 delete this，从根上避免"回调中悬空指针"
// ============================================================================
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
