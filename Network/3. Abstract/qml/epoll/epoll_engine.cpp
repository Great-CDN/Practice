#include "epoll_engine.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <unistd.h>   // close：epoll 实例的 fd 也用完要关
#include <cstring>

EpollEngine::EpollEngine() : epfd_(-1), running_(true) {
    epfd_ = epoll_create1(0);
    if (epfd_ < 0) {
        fprintf(stderr, "[FATAL] epoll_create1 failed: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

EpollEngine::~EpollEngine() {
    if (epfd_ >= 0) close(epfd_);
}

// 把业务层的事件掩码翻译成内核的 epoll 事件位
ErrCode EpollEngine::SyncToKernel(FD fd, int32_t mask, bool exists) {
    epoll_event ev{};
    if (mask & kReadEvent)  ev.events |= EPOLLIN;
    if (mask & kWriteEvent) ev.events |= EPOLLOUT;
    ev.data.fd = fd;

    int op = exists ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;   // 新注册 vs 修改关注
    if (epoll_ctl(epfd_, op, fd, &ev) < 0) return kErr;
    return kOk;
}

ErrCode EpollEngine::AddIoEvent(FD fd, int32_t mask, IIoHandler* handler, void* user_data) {
    if (fd < 0 || handler == NULL || (mask & ~(kReadEvent | kWriteEvent)) != 0)
        return kErr;

    bool exists = entries_.find(fd) != entries_.end();   // 必须在插入前判断
    Entry& e = entries_[fd];
    e.handler   = handler;
    e.user_data = user_data;
    e.mask     |= mask;               // 叠加语义：mask 取并集
    return SyncToKernel(fd, e.mask, exists);
}

void EpollEngine::DeleteIoEvent(FD fd, int32_t mask) {
    auto it = entries_.find(fd);
    if (it == entries_.end()) return;

    Entry& e = it->second;
    e.mask &= ~mask;                  // 只清掉指定的事件位

    if (e.mask != 0) {
        SyncToKernel(fd, e.mask, true);      // 还剩部分关注：MOD 更新
    } else {
        // 读写都不关注了：彻底移出引擎
        // 注：close(fd) 时内核也会自动摘除，这里显式 DEL 只是保持表干净
        epoll_event ev{};
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &ev);
        entries_.erase(it);
    }
}

ErrCode EpollEngine::Run() {
    epoll_event evs[kMaxEvents];
    while (running_) {
        // 返回值 n = 就绪 fd 个数，evs[0..n) 直接就是就绪列表——
        // 没有 select 的集合重建，也没有 O(n) 的 FD_ISSET 扫描
        int n = epoll_wait(epfd_, evs, kMaxEvents, -1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return kErr;
        }

        for (int i = 0; i < n; ++i) {
            FD fd = (FD)evs[i].data.fd;

            auto it = entries_.find(fd);
            if (it == entries_.end()) continue;   // 事件已过期（连接刚被处理掉）

            int32_t fired = 0;
            if (evs[i].events & EPOLLIN)  fired |= kReadEvent;
            if (evs[i].events & EPOLLOUT) fired |= kWriteEvent;
            // 出错/挂断：把该 fd 当前关注的全部事件都报给业务，
            // 让它在 recv/send 里感知到错误并自行收尾关闭
            if (evs[i].events & (EPOLLERR | EPOLLHUP)) fired |= it->second.mask;
            if (fired == 0) continue;

            IIoHandler* handler   = it->second.handler;
            void*       user_data = it->second.user_data;

            // 注意：OnRead 回调里可能关闭连接（DeleteIoEvent + delete this），
            // 所以调 OnWrite 前必须重新确认 entry 还在，否则悬空指针
            if (fired & kReadEvent)
                handler->OnRead(fd, user_data, fired);

            it = entries_.find(fd);
            if (it == entries_.end()) continue;
            if (fired & kWriteEvent)
                it->second.handler->OnWrite(fd, it->second.user_data, fired);
        }
    }
    return kOk;
}
