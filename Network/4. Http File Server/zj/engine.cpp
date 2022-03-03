#include <assert.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include<errno.h>
#include<fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include<iostream> 
#include "engine.h"


EventEngine::EventEngine() {
	m_events = new epoll_event[MAX_EVENTS_NUM];
}


ErrCode EventEngine::Init() {
	m_epoll_fd = epoll_create1(0);
	if (m_epoll_fd == -1) {
		return -1;
	}
	return 0;
}


ErrCode EventEngine::AddIoEvent(FD fd, int mask, IIOHandler* handler, void* user_data) {
	ErrCode err = 0;
	do {
		epoll_event ev;
		ev.data.fd = fd;
		ev.events = mask;
		if (m_fds.count(fd)) {
			if (m_fds[fd]->mask) {
				ev.events = mask | m_fds[fd]->mask;
				err = epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
			}
			else {
				err = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
			}
			if (err == -1) {
				break;
			}
		}
		else {
			Processor* processor = new Processor();
			processor->fd = fd;
			processor->handler = handler;
			processor->user_data = user_data;
			processor->mask = mask;
			m_fds[fd] = processor;
			if (fd > MAX_EVENTS_NUM) {
				return -1;
			}
			//ev.data.ptr = processor;
			ErrCode err = epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &ev);
			if (err == -1) {
				break;
			}
		}
	} while (0);
	return err;
}


ErrCode EventEngine::DeleteIoEvent(FD fd, int mask) {
	assert(m_fds.count(fd));
	m_fds[fd]->mask &= !mask;
	epoll_event ev;
	ErrCode err = 0;
	if (m_fds[fd]->mask) {
		ev.events = m_fds[fd]->mask;
		err = epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &ev);
	}
	else {
		err = epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, NULL);
	}
	if (err == -1) {
		return err;
	}
	return 0;
}


ErrCode EventEngine::Run() {
	while (1) {
		int num_events = epoll_wait(m_epoll_fd, m_events, sizeof(m_events), -1);
		if (num_events < 0) {
			return -1;
		}
		for (int i = 0; i < num_events; i++) {
			FD fd = m_events[i].data.fd;
			if (m_events[i].events & EPOLLIN) {
				ErrCode err = m_fds[fd]->handler->OnRead(fd, m_fds[fd]->user_data, m_events[i].events);
				if (err == -1) {
					return err;
				}
			}
			if (m_events[i].events & EPOLLOUT) {
				ErrCode err = m_fds[fd]->handler->OnWrite(fd, m_fds[fd]->user_data, m_events[i].events);
				if (err == -1) {
					return err;
				}
			}
		}
	}
	return 0;
}


Processor* EventEngine::GetFd(FD fd) {
	return m_fds[fd];
}


void EventEngine::DelFd(FD fd) {
	m_fds.erase(fd);
}


void EventEngine::Stop() {
	close(m_epoll_fd);
}
