#include "epoll_engine.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

#include <sys/epoll.h>
#include <unistd.h>

EpollEngine::EpollEngine() : epfd_(epoll_create1(EPOLL_CLOEXEC)), running_(true) {
    if (epfd_ == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
}

EpollEngine::~EpollEngine() {
    close(epfd_);
}

// 把公共事件掩码转换成 Linux epoll 事件，并同步到内核。
ErrCode EpollEngine::UpdateKernel(FD fd, std::int32_t events, int operation) {
    epoll_event event{};
    if ((events & kReadEvent) != 0) event.events |= EPOLLIN | EPOLLRDHUP;
    if ((events & kWriteEvent) != 0) event.events |= EPOLLOUT;
    event.data.fd = fd;

    if (epoll_ctl(epfd_, operation, fd, &event) == -1) {
        perror("epoll_ctl");
        return kErr;
    }
    return kOk;
}

ErrCode EpollEngine::AddIoEvent(FD fd, std::int32_t events, IIoHandler* handler,
                                void* user_data) {
    constexpr std::int32_t kAllEvents = kReadEvent | kWriteEvent;
    if (fd < 0 || handler == nullptr || events == 0 || (events & ~kAllEvents) != 0) {
        return kErr;
    }

    const auto it = entries_.find(fd);
    const bool exists = it != entries_.end();
    const std::int32_t combined = events | (exists ? it->second.events : 0);
    const int operation = exists ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;

    // 先更新内核；失败时不污染用户态注册表。
    if (UpdateKernel(fd, combined, operation) != kOk) return kErr;

    const Entry entry{handler, user_data, combined};
    if (exists) {
        it->second = entry;
    } else {
        entries_.emplace(fd, entry);
    }
    return kOk;
}

void EpollEngine::DeleteIoEvent(FD fd, std::int32_t events) {
    const auto it = entries_.find(fd);
    if (it == entries_.end()) return;

    constexpr std::int32_t kAllEvents = kReadEvent | kWriteEvent;
    const std::int32_t remaining = it->second.events & ~(events & kAllEvents);
    if (remaining == 0) {
        epoll_event unused{};
        if (epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, &unused) == -1 && errno != ENOENT) {
            perror("epoll_ctl DEL");
        }
        // 即使内核删除失败，也必须清掉回调，避免以后调用悬空指针。
        entries_.erase(it);
        return;
    }

    // MOD 极少失败；保留用户态期望的新掩码，使过期内核事件会被 Run 过滤。
    UpdateKernel(fd, remaining, EPOLL_CTL_MOD);
    it->second.events = remaining;
}

ErrCode EpollEngine::Run() {
    running_ = true;
    epoll_event events[kMaxEvents];
    while (running_) {
        if (entries_.empty()) return kOk;

        const int ready = epoll_wait(epfd_, events, kMaxEvents, -1);
        if (ready == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            return kErr;
        }

        // epoll_wait 已经返回就绪列表，只遍历 events[0..ready)。
        for (int index = 0; index < ready; ++index) {
            const FD fd = events[index].data.fd;
            const std::uint32_t occurred = events[index].events;
            auto it = entries_.find(fd);
            if (it == entries_.end()) continue;

            std::int32_t fired = 0;
            if ((occurred & (EPOLLIN | EPOLLRDHUP)) != 0) fired |= kReadEvent;
            if ((occurred & EPOLLOUT) != 0) fired |= kWriteEvent;
            // EPOLLERR/EPOLLHUP 无需订阅。把当前关注项交给业务层，让 recv/send
            // 返回具体错误或 EOF，并沿用统一的关闭流程。
            if ((occurred & (EPOLLERR | EPOLLHUP)) != 0) fired |= it->second.events;
            fired &= it->second.events;
            if (fired == 0) continue;

            if ((fired & kReadEvent) != 0) {
                IIoHandler* const handler = it->second.handler;
                handler->OnRead(fd, it->second.user_data, fired);
            }

            // OnRead 可能删除注册项和处理器，必须重新查询。
            it = entries_.find(fd);
            if (it == entries_.end()) continue;
            if ((fired & kWriteEvent) != 0 && (it->second.events & kWriteEvent) != 0) {
                it->second.handler->OnWrite(fd, it->second.user_data, fired);
            }
        }
    }
    return kOk;
}
