#pragma once
#include <stdint.h>

typedef int FD;         // 文件描述符（Linux 下 socket 就是 int）
typedef int ErrCode;    // 错误码：0 = 成功，负数 = 失败

// 事件掩码：AddIoEvent 声明"关注什么"，回调参数里的 mask 表示"发生了什么"
static const int32_t kReadEvent  = 0x01;   // 可读（对端发数据/对端关闭/有新连接）
static const int32_t kWriteEvent = 0x02;   // 可写（内核发送缓冲有空位）

enum { kOk = 0, kErr = -1 };

// Engine 回调外部接口：业务方实现这两个纯虚函数，引擎在事件就绪时调用
class IIoHandler
{
public:
    virtual ~IIoHandler() {}
    // mask：本次触发的事件（kReadEvent/kWriteEvent 的位或，可能同时触发）
    // data：注册时传入的 user_data 原样带回，用于挂业务上下文
    virtual void OnRead (FD fd, void* data, int32_t mask) = 0;
    virtual void OnWrite(FD fd, void* data, int32_t mask) = 0;
};

// Engine 调用接口：业务方通过它注册/注销 fd 的事件关注
class IEventEngine
{
public:
    virtual ~IEventEngine() {}
    // 关注 fd 上的 mask 事件；就绪时回调 handler->OnRead/OnWrite 并带回 user_data
    // 对同一 fd 重复调用 = 叠加关注（mask 取并集）
    virtual ErrCode AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) = 0;
    // 取消 fd 对 mask 的关注；读写都取消后该 fd 彻底移出引擎
    virtual void    DeleteIoEvent(FD fd, int32_t mask) = 0;
};
