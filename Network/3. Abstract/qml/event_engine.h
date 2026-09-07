#pragma once

#include <cstdint>

// Linux 使用 int 表示 socket 文件描述符。别名让业务代码不依赖具体多路复用实现。
using FD = int;
using ErrCode = int;

constexpr std::int32_t kReadEvent = 0x01;  // 可读、对端关闭或监听 fd 有新连接
constexpr std::int32_t kWriteEvent = 0x02; // 内核发送缓冲区有可用空间
constexpr ErrCode kOk = 0;
constexpr ErrCode kErr = -1;

// 业务层实现事件回调；data 是注册 fd 时传入的上下文指针。
class IIoHandler {
public:
    virtual ~IIoHandler() = default;

    virtual void OnRead(FD fd, void* data, std::int32_t events) = 0;
    virtual void OnWrite(FD fd, void* data, std::int32_t events) = 0;
};

// select 和 epoll 对业务层提供完全相同的接口。
class IEventEngine {
public:
    virtual ~IEventEngine() = default;

    // 为 fd 增加关注事件。重复调用会把 events 与原有关注项合并。
    virtual ErrCode AddIoEvent(FD fd, std::int32_t events, IIoHandler* handler,
                               void* user_data) = 0;

    // 删除指定关注项；读写事件都删除后，fd 会从引擎中移除。
    virtual void DeleteIoEvent(FD fd, std::int32_t events) = 0;

    // Run 阻塞并分发事件。Stop 适合在回调中调用，使 Run 在本轮结束后退出。
    virtual ErrCode Run() = 0;
    virtual void Stop() = 0;
};
