#pragma once
#include "event_engine.h"
#include <sys/epoll.h>
#include <map>

class EpollEngine : public IEventEngine
{
public:
    EpollEngine();
    ~EpollEngine() override;

    ErrCode AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) override;
    void    DeleteIoEvent(FD fd, int32_t mask) override;

    // 事件主循环：epoll_wait 等待 -> 分发回调
    ErrCode Run();

    void Stop() { running_ = false; }

private:
    struct Entry {
        IIoHandler* handler;
        void*       user_data;
        int32_t     mask;    // 当前关注的事件（读/写可独立增删）
    };

    // 把 entries_[fd] 里的 mask 同步到内核：新 fd 用 ADD，老 fd 用 MOD
    ErrCode SyncToKernel(FD fd, int32_t mask, bool exists);

    int                epfd_;      // epoll 实例（内核事件表）的句柄
    std::map<FD, Entry> entries_;  // fd -> 注册信息
    bool               running_;

    static const int kMaxEvents = 64;   // epoll_wait 单次最多取回的就绪事件数
};
