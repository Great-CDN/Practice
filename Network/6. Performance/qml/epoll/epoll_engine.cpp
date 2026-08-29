#include "epoll_engine.h"
#include <cerrno>
#include <unistd.h>

EpollEngine::EpollEngine() : epfd_(-1), running_(true) {
    epfd_ = epoll_create1(0);
    // 失败时 epfd_ 保持 -1，由 AddIoEvent/Run 统一返回 kErr 暴露
}

EpollEngine::~EpollEngine() {
    if (epfd_ >= 0) close(epfd_);
}

// 把业务事件掩码翻译成 epoll 掩码并执行操作（ADD/MOD）
ErrCode EpollEngine::Ctl(FD fd, int op, int32_t mask) {
    epoll_event ev{};
    if (mask & kReadEvent)  ev.events |= EPOLLIN;
    if (mask & kWriteEvent) ev.events |= EPOLLOUT;
    ev.data.fd = fd;
    return epoll_ctl(epfd_, op, fd, &ev) == 0 ? kOk : kErr;
}

ErrCode EpollEngine::AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) {
    // 参数合法性：非法 fd、空回调、非法 mask 位都拒绝（epoll 没有 select 的
    // FD_SETSIZE 上限，因此不再需要越界检查）
    if (epfd_ < 0 || fd < 0 || handler == NULL ||
        (mask & ~(kReadEvent | kWriteEvent)) != 0)
        return kErr;

    auto it = entries_.find(fd);
    if (it == entries_.end()) {                       // 新注册
        if (Ctl(fd, EPOLL_CTL_ADD, mask) != kOk) return kErr;
        entries_[fd] = Entry{handler, user_data, mask};
        return kOk;
    }

    Entry& e = it->second;
    int32_t new_mask = e.mask | mask;                 // 叠加语义：mask 取并集
    if (new_mask != e.mask &&
        Ctl(fd, EPOLL_CTL_MOD, new_mask) != kOk)
        return kErr;
    e.mask      = new_mask;
    e.handler   = handler;
    e.user_data = user_data;
    return kOk;
}

void EpollEngine::DeleteIoEvent(FD fd, int32_t mask) {
    auto it = entries_.find(fd);
    if (it == entries_.end()) return;

    Entry& e = it->second;
    int32_t new_mask = e.mask & ~mask;                // 只清掉指定的事件位
    if (new_mask == e.mask) return;                   // 没有任何位被注销

    if (new_mask == 0) {                              // 读写都不关注：彻底移出引擎
        // fd 可能已被业务 close（DEL 返回 EBADF），静默忽略即可
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, NULL);
        entries_.erase(it);
        return;
    }
    if (Ctl(fd, EPOLL_CTL_MOD, new_mask) == kOk)
        e.mask = new_mask;
}

ErrCode EpollEngine::Run() {
    return Run(-1);   // 默认无超时
}

// 带超时的主循环：timeout_ms < 0 表示永久等待；每轮结束调用 OnLoop() 钩子。
// 与 select 版的关键差异：epoll_wait 直接返回"就绪的 fd 列表"，
// 不需要扫描 0..maxfd，连接数再多也只处理真正就绪的连接。
ErrCode EpollEngine::Run(int timeout_ms) {
    if (epfd_ < 0) return kErr;

    epoll_event evs[kMaxEvents];
    while (running_) {
        int n = epoll_wait(epfd_, evs, kMaxEvents,
                           timeout_ms < 0 ? -1 : timeout_ms);
        if (n < 0) {
            if (errno == EINTR) continue;             // 被信号打断，重来
            return kErr;
        }

        for (int i = 0; i < n; ++i) {
            FD fd = evs[i].data.fd;

            // EPOLLERR/EPOLLHUP 按可读上报：业务 recv 一次即可感知断开
            int32_t fired = 0;
            if (evs[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) fired |= kReadEvent;
            if (evs[i].events & EPOLLOUT)                        fired |= kWriteEvent;
            if (fired == 0) continue;

            auto it = entries_.find(fd);
            if (it == entries_.end()) continue;       // 快照生成后 fd 已被注销
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
