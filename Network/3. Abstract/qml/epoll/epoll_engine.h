#pragma once

#include "../event_engine.h"

#include <map>

// 使用 Linux epoll 水平触发模式实现公共事件引擎接口。
class EpollEngine : public IEventEngine {
public:
    EpollEngine();
    ~EpollEngine() override;

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

    ErrCode UpdateKernel(FD fd, std::int32_t events, int operation);

    static constexpr int kMaxEvents = 64;

    int epfd_;                    // epoll 实例本身也是 Linux 文件描述符
    std::map<FD, Entry> entries_; // fd -> 回调和当前关注项
    bool running_;
};
