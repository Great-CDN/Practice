
#include <assert.h>
#include <sys/epoll.h>

#include "engine.h"

int Engine::Initialize()
{
    m_efd = epoll_create(m_size);
    return m_efd;
}

int Engine::AddIoEvent(int fd, int mask, IoHandler* handler, void* user_data)
{
    struct epoll_event ev;
    ev.data.fd = fd;

    if (m_fds.count(fd))
    {
        m_fds[fd]->mask |= mask;
        ev.events = m_fds[fd]->mask;
        epoll_ctl(m_efd, EPOLL_CTL_MOD, fd, &ev);
    }
    else
    {
        IoHandlerCtx* ctx = new IoHandlerCtx();
        m_fds[fd] = ctx;

        m_fds[fd]->mask = mask;
        ev.events = mask;
        epoll_ctl(m_efd, EPOLL_CTL_ADD, fd, &ev);
    }

    m_fds[fd]->handler = handler;
    m_fds[fd]->user_data = user_data;

    return 0;
}

void Engine::DeleteIoEvent(int fd, int mask)
{
    assert(m_fds.count(fd));
    m_fds[fd]->mask &= ~mask;

    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = m_fds[fd]->mask;

    if (m_fds[fd]->mask)
    {
        epoll_ctl(m_efd, EPOLL_CTL_MOD, fd, &ev);
    }
    else
    {
        epoll_ctl(m_efd, EPOLL_CTL_DEL, fd, &ev);
        delete m_fds[fd];
        m_fds.erase(fd);
    }
}

void Engine::Start()
{
    int                 fd;
    int                 readies;
    struct epoll_event* evs = new struct epoll_event[m_size]();
    while (1)
    {
        if ((readies = epoll_wait(m_efd, evs, m_fds.size(), -1)) == -1)
        {
            printf("Epoll Wait Error : %d\n", errno);
            break;
        }

        for (int i = 0; i < readies; i++)
        {
            fd = evs[i].data.fd;
            assert(m_fds.count(fd));

            if (m_fds[fd]->mask & evs[i].events & EPOLLIN)
            {
                m_fds[fd]->handler->OnRead(fd, m_fds[fd]->user_data, m_fds[fd]->mask);
            }

            if (m_fds[fd]->mask & evs[i].events & EPOLLOUT)
            {
                m_fds[fd]->handler->OnWrite(fd, m_fds[fd]->user_data, m_fds[fd]->mask);
            }
        }
    }
    delete[] evs;
}

void Engine::Release()
{
    for (std::map<int, IoHandlerCtx*>::iterator it = m_fds.begin(); it != m_fds.end(); it++)
    {
        delete it->second;
    }
    m_fds.clear();
    close(m_efd);
    m_efd = 0;
}
