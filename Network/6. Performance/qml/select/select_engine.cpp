#include "select_engine.h"
#include <cerrno>
#include <cstddef>

SelectEngine::SelectEngine() : maxfd_(-1), running_(true) {
    FD_ZERO(&master_r_);
    FD_ZERO(&master_w_);
}

SelectEngine::~SelectEngine() {}

ErrCode SelectEngine::AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) {
    // 参数合法性：fd 越界（select 硬限制 FD_SETSIZE）、空回调、非法 mask 位都拒绝
    if (fd < 0 || fd >= FD_SETSIZE || handler == NULL ||
        (mask & ~(kReadEvent | kWriteEvent)) != 0)
        return kErr;

    Entry& e = entries_[fd];          // 不存在则新建，存在则复用
    e.handler   = handler;
    e.user_data = user_data;
    e.mask     |= mask;               // 叠加语义：mask 取并集
    if (e.mask & kReadEvent)  FD_SET(fd, &master_r_);
    if (e.mask & kWriteEvent) FD_SET(fd, &master_w_);
    if (fd > maxfd_) maxfd_ = fd;
    return kOk;
}

void SelectEngine::DeleteIoEvent(FD fd, int32_t mask) {
    auto it = entries_.find(fd);
    if (it == entries_.end()) return;

    Entry& e = it->second;
    e.mask &= ~mask;                  // 只清掉指定的事件位
    if (!(e.mask & kReadEvent))  FD_CLR(fd, &master_r_);
    if (!(e.mask & kWriteEvent)) FD_CLR(fd, &master_w_);

    if (e.mask == 0) {                // 读写都不关注了：彻底移出引擎
        entries_.erase(it);
        if (fd == maxfd_) UpdateMaxFd();
    }
}

void SelectEngine::UpdateMaxFd() {
    maxfd_ = -1;
    for (auto& kv : entries_)
        if (kv.first > maxfd_) maxfd_ = kv.first;
}

ErrCode SelectEngine::Run() {
    return Run(-1);   // 默认无超时
}

// 带超时的主循环：timeout_ms < 0 表示永久等待；每轮结束调用 OnLoop() 钩子
ErrCode SelectEngine::Run(int timeout_ms) {
    while (running_) {
        // 关键：select 会改写传入的集合，每轮必须从主集合拷贝工作副本
        fd_set rs = master_r_;
        fd_set ws = master_w_;

        timeval tv{};
        timeval* ptv = NULL;
        if (timeout_ms >= 0) {
            tv.tv_sec  = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            ptv = &tv;
        }

        int n = select(maxfd_ + 1, &rs, &ws, NULL, ptv);
        if (n < 0) {
            if (errno == EINTR) continue;   // 被信号打断，重来
            return kErr;
        }

        // select 的固有代价：只知道"n 个就绪"，不知道是谁，
        // 必须扫描 0..maxfd 逐个 FD_ISSET —— O(maxfd)
        for (FD fd = 0; fd <= maxfd_ && n > 0; ++fd) {
            int32_t fired = 0;
            if (FD_ISSET(fd, &rs)) fired |= kReadEvent;
            if (FD_ISSET(fd, &ws)) fired |= kWriteEvent;
            if (fired == 0) continue;
            --n;

            auto it = entries_.find(fd);
            if (it == entries_.end()) continue;
            IIoHandler* handler   = it->second.handler;
            void*       user_data = it->second.user_data;

            // 注意：OnRead 回调里可能 DeleteIoEvent 本 fd（连接关闭），
            // 所以调 OnWrite 前必须重新确认 entry 还在，否则悬空指针
            if (fired & kReadEvent)
                handler->OnRead(fd, user_data, fired);

            it = entries_.find(fd);
            if (it == entries_.end()) continue;
            if (fired & kWriteEvent)
                it->second.handler->OnWrite(fd, it->second.user_data, fired);
        }

        OnLoop();   // 每轮末尾的定时钩子（超时或处理完事件都会走这里）
    }
    return kOk;
}
