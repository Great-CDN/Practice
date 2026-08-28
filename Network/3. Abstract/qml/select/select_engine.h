
#pragma once
#include "event_engine.h"
#include <sys/select.h>
#include <map>

class SelectEngine : public IEventEngine
{
public:
    SelectEngine();
    ~SelectEngine() override;

    ErrCode AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) override;
    void    DeleteIoEvent(FD fd, int32_t mask) override;

    // 事件主循环：select 等待 -> 分发回调，业务方调一次即可
    ErrCode Run();

    void Stop() { running_ = false; }   // 需要退出循环时调用

private:
    struct Entry {
        IIoHandler* handler;
        void*       user_data;
        int32_t     mask;    // 当前关注的事件（读/写可独立增删）
    };
    void UpdateMaxFd();

    std::map<FD, Entry> entries_;   // fd -> 注册信息
    fd_set  master_r_;              // 主可读集合（select 会改写传入集合，每轮需拷贝）
    fd_set  master_w_;              // 主可写集合
    FD      maxfd_;
    bool    running_;
};
