
#include <cstring>
#include <stdio.h>
#include <time.h>

#include "be_engine.h"

BeEngine::BeEngine(int32_t capacity) : m_event_loop(NULL), m_io_event(NULL), m_capacity(capacity)
{}

BeEngine::~BeEngine()
{}

ErrCode BeEngine::Initialize()
{
    Assert(!m_event_loop);
    Assert(!m_io_event);

    ErrCode err = EC_OK;
    do
    {
        m_event_loop = aeCreateEventLoop(m_capacity);
        Assert(m_event_loop);
        if (!m_event_loop)
        {
            err = EC_ERR;
            break;
        }
        m_io_event = new IoEvent[m_capacity];
        memset(m_io_event, 0, sizeof(IoEvent) * m_capacity);
    } while (0);

    return err;
}

ErrCode BeEngine::Main()
{
    aeMain(m_event_loop);
    return EC_OK;
}

ErrCode BeEngine::AddIoEvent(FD fd, int mask, IIoHandler *handler, void *user_data)
{
    Assert(handler);
    Assert(fd < m_capacity);

    IoEvent *event = &m_io_event[fd];
    // Assert(!(event->mask & mask));

    ErrCode err = EC_OK;
    do
    {
        if (mask & AE_READABLE)
        {
            event->read_handler = handler;
        }
        if (mask & AE_WRITABLE)
        {
            event->write_handler = handler;
        }
        event->user_data = user_data;
        err = aeCreateFileEvent(m_event_loop, fd, mask, be_io_callback, this);
        if (err)
        {
            err = EC_ERR;
            break;
        }
        event->mask |= mask;

    } while (0);

    return err;
}

void be_io_callback(struct aeEventLoop *event_loop, FD fd, void *data, int mask)
{
    BeEngine *engine = (BeEngine *)data;
    engine->OnIoEvent(fd, mask);
}

void BeEngine::OnIoEvent(FD fd, int mask)
{
    Assert(mask & (AE_READABLE | AE_WRITABLE));

    IoEvent *event = &m_io_event[fd];
    if (mask & event->mask & AE_READABLE)
    {
        event->read_handler->OnRead(fd, event->user_data, mask);
    }
    if (mask & event->mask & AE_WRITABLE)
    {
        event->write_handler->OnWrite(fd, event->user_data, mask);
    }
}

void BeEngine::DeleteIoEvent(FD fd, int mask)
{
    Assert(fd < m_capacity);

    IoEvent *event = &m_io_event[fd];
    // Assert(event->mask & mask);

    aeDeleteFileEvent(m_event_loop, fd, mask);
    event->mask = event->mask & (~mask);
}

void BeEngine::StopEngine()
{
    Assert(m_event_loop);
    Assert(m_io_event);

    aeStop(m_event_loop);
}

void BeEngine::Release()
{
    aeDeleteEventLoop(m_event_loop);
    delete[] m_io_event;
}
