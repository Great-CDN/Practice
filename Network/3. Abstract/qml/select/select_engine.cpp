#include "select_engine.h"

#include <cerrno>
#include <cstdio>
#include <vector>

SelectEngine::SelectEngine() : maxfd_(-1), running_(true) {
    FD_ZERO(&readfds_);
    FD_ZERO(&writefds_);
}

SelectEngine::~SelectEngine() = default;

ErrCode SelectEngine::AddIoEvent(FD fd, std::int32_t events, IIoHandler* handler,
                                 void* user_data) {
    constexpr std::int32_t kAllEvents = kReadEvent | kWriteEvent;
    if (fd < 0 || fd >= FD_SETSIZE || handler == nullptr || events == 0 ||
        (events & ~kAllEvents) != 0) {
        return kErr;
    }

    auto it = entries_.find(fd);
    if (it == entries_.end()) {
        // 显式初始化 events，避免读取未初始化的掩码。
        const Entry entry{handler, user_data, events};
        it = entries_.emplace(fd, entry).first;
    } else {
        it->second.handler = handler;
        it->second.user_data = user_data;
        it->second.events |= events;
    }

    if ((it->second.events & kReadEvent) != 0) FD_SET(fd, &readfds_);
    if ((it->second.events & kWriteEvent) != 0) FD_SET(fd, &writefds_);
    if (fd > maxfd_) maxfd_ = fd;
    return kOk;
}

void SelectEngine::DeleteIoEvent(FD fd, std::int32_t events) {
    const auto it = entries_.find(fd);
    if (it == entries_.end()) return;

    constexpr std::int32_t kAllEvents = kReadEvent | kWriteEvent;
    it->second.events &= ~(events & kAllEvents);
    if ((it->second.events & kReadEvent) == 0) FD_CLR(fd, &readfds_);
    if ((it->second.events & kWriteEvent) == 0) FD_CLR(fd, &writefds_);

    if (it->second.events == 0) {
        entries_.erase(it);
        if (fd == maxfd_) UpdateMaxFd();
    }
}

void SelectEngine::UpdateMaxFd() {
    maxfd_ = entries_.empty() ? -1 : entries_.rbegin()->first;
}

ErrCode SelectEngine::Run() {
    running_ = true;
    while (running_) {
        if (entries_.empty()) return kOk;

        // select 会覆盖传入集合，只能把主集合复制给本轮系统调用。
        fd_set readable = readfds_;
        fd_set writable = writefds_;
        const int ready = select(maxfd_ + 1, &readable, &writable, nullptr, nullptr);
        if (ready == -1) {
            if (errno == EINTR) continue;
            perror("select");
            return kErr;
        }

        // 回调可以删除 fd，所以先保存本轮注册项的描述符快照。
        std::vector<FD> fds;
        fds.reserve(entries_.size());
        for (const auto& item : entries_) fds.push_back(item.first);

        // select 不会给出就绪列表，仍需逐项执行 FD_ISSET。
        for (const FD fd : fds) {
            std::int32_t fired = 0;
            if (FD_ISSET(fd, &readable)) fired |= kReadEvent;
            if (FD_ISSET(fd, &writable)) fired |= kWriteEvent;
            if (fired == 0) continue;

            auto it = entries_.find(fd);
            if (it == entries_.end()) continue;

            // OnRead 可能关闭连接并销毁处理器。调用 OnWrite 前必须重新查表。
            if ((fired & kReadEvent) != 0) {
                IIoHandler* const handler = it->second.handler;
                handler->OnRead(fd, it->second.user_data, fired);
            }

            it = entries_.find(fd);
            if (it == entries_.end()) continue;
            if ((fired & kWriteEvent) != 0 && (it->second.events & kWriteEvent) != 0) {
                it->second.handler->OnWrite(fd, it->second.user_data, fired);
            }
        }
    }
    return kOk;
}
