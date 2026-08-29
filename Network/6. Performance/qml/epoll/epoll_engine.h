#pragma once
#include "event_engine.h"
#include <sys/epoll.h>
#include <map>

// ============================================================================
// Epoll 版事件引擎：单线程、电平触发（LT）
//
// 为什么选 LT 而不是 ET（EPOLLET）：
//   * LT 的"条件成立即报告"语义与 Select 版完全一致 —— 业务层 http_server
//     零改动复用，两版行为可逐字节对照，复杂度最低
//   * 相对 select 的关键收益在本引擎内部已经拿到：fd 数量不再受
//     FD_SETSIZE(1024) 硬限制；事件分发 O(就绪数)，而不是 O(maxfd) 全表扫描
//   * ET 在本项目目标规模（2 万 rps 级）收益微小，却要求所有读写路径
//     "循环读到 EAGAIN"并仔细处理漏事件，复杂度明显上升，不值得
//
// 语义说明（与 select 版一致的部分）：
//   * 可写关注必须"发完即撤"，否则 LT 每轮都会报告可写导致空转
//   * 重复 AddIoEvent = 叠加关注（mask 取并集）；读写都注销后彻底移出引擎
// 引擎内部约定：
//   * EPOLLERR/EPOLLHUP 统一按"可读"上报：业务 recv 一次即可感知对端关闭
//     或连接出错，避免在未订阅可写的状态下凭空收到写回调
// ============================================================================
class EpollEngine : public IEventEngine
{
public:
    EpollEngine();
    ~EpollEngine() override;

    ErrCode AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) override;
    void    DeleteIoEvent(FD fd, int32_t mask) override;

    // 事件主循环：epoll_wait 等待 -> 批量分发回调，业务方调一次即可
    ErrCode Run();

    // 带超时的主循环：每轮最多等 timeout_ms 毫秒，每轮结束回调 OnLoop()
    // （定时器场景用：直播定帧、慢响应到期、周期统计）
    ErrCode Run(int timeout_ms);
    virtual void OnLoop() {}

    void Stop() { running_ = false; }

private:
    ErrCode Ctl(FD fd, int op, int32_t mask);

    struct Entry {
        IIoHandler* handler;
        void*       user_data;
        int32_t     mask;    // 当前关注的事件（读/写可独立增删）
    };

    static const int kMaxEvents = 1024;   // 单次 epoll_wait 批量取走的事件上限

    int                 epfd_;
    std::map<FD, Entry> entries_;         // fd -> 注册信息
    bool                running_;
};
