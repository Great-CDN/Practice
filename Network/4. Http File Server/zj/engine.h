#pragma once
#pragma once
#include <sys/epoll.h>
#include <map>

typedef int FD;
typedef int ErrCode;
#define MAX_EVENTS_NUM 100
#define R EPOLLIN
#define W EPOLLOUT
#define R_W EPOLLIN | EPOLLOUT

//epoll part
class IIOHandler
{
public:
	virtual ErrCode OnRead(FD fd, void* date, int mask) = 0;
	virtual ErrCode OnWrite(FD fd, void* date, int mask) = 0;
};

struct Processor
{
	FD fd;
	IIOHandler* handler;
	void* user_data;
	int mask;
};

class IEventEngine
{
public:
	virtual ErrCode AddIoEvent(FD fd, int mask, IIOHandler* handler, void* user_data) = 0;
	virtual ErrCode DeleteIoEvent(FD fd, int mask) = 0;
};

class EventEngine : public IEventEngine
{
public:
	EventEngine();
	ErrCode Init();
	ErrCode Run();
	ErrCode AddIoEvent(FD fd, int mask, IIOHandler* handler, void* user_data);
	ErrCode DeleteIoEvent(FD fd, int mask);
	void Stop();
	Processor* GetFd(FD fd);
	void DelFd(FD fd);
private:
	FD m_epoll_fd;
	epoll_event* m_events;
	std::map<FD, Processor*> m_fds; // ²»¹«¿ª
};
