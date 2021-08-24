
#ifndef __BE_ENGINE_H__
#define __BE_ENGINE_H__

#include <time.h>

#include "ae.h"
#include "sdk_type.h"

class IIoHandler
{
public:
    virtual void OnRead(FD fd, void *data, int32_t mask) = 0;
    virtual void OnWrite(FD fd, void *data, int32_t mask) = 0;
};

struct IoEvent
{
    uint8_t     mask;
    IIoHandler *write_handler;
    IIoHandler *read_handler;
    void *      user_data;
};

class IEventEngine
{
public:
    virtual ErrCode AddIoEvent(FD fd, int32_t mask, IIoHandler *handler, void *user_data) = 0;
    virtual void    DeleteIoEvent(FD fd, int32_t mask) = 0;
};

class BeEngine : public IEventEngine
{
public:
    BeEngine(int32_t capacity);
    virtual ~BeEngine();

public:
    ErrCode Initialize();
    void    StopEngine();
    void    Release();

    ErrCode AddIoEvent(FD fd, int mask, IIoHandler *handler, void *user_data);
    void    DeleteIoEvent(FD fd, int mask);

public:
    ErrCode Main();
    void    OnIoEvent(FD fd, int mask);

private:
    aeEventLoop *m_event_loop;
    IoEvent *    m_io_event;
    int32_t      m_capacity;
};

void be_io_callback(struct aeEventLoop *eventLoop, FD fd, void *data, int mask);

#endif
