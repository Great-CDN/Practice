#pragma once

#include "../event_engine.h"

#include <map>
#include <sys/select.h>

// 使用 Linux select 实现公共事件引擎接口。
class SelectEngine : public IEventEngine {
public:
    SelectEngine();
    ~SelectEngine() override;

    ErrCode AddIoEvent(FD fd, std::int32_t events, IIoHandler* handler,
                       void* user_data) override;
    void DeleteIoEvent(FD fd, std::int32_t events) override;
    ErrCode Run() override;
    void Stop() override { running_ = false; }

private:
    struct Entry {
        IIoHandler* handler;
        void* user_data;
        std::int32_t events;
    };

    void UpdateMaxFd();

    std::map<FD, Entry> entries_; // fd -> 回调和当前关注项
    fd_set readfds_;              // select 会修改工作副本，因此保留主集合
    fd_set writefds_;
    FD maxfd_;
    bool running_;
};
