#ifndef __EG_H__
#define __EG_H__

#include <errno.h>
#include <iostream>
#include <map>
#include <stdio.h>

class IoHandler
{
public:
    virtual void OnRead(int fd, void *data, int mask) = 0;
    virtual void OnWrite(int fd, void *data, int mask) = 0;
};

class IoEngine
{
public:
    virtual int  AddIoEvent(int fd, int mask, IoHandler *handler, void *user_data) = 0;
    virtual void DeleteIoEvent(int fd, int mask) = 0;
};

struct IoHandlerCtx
{
    int        mask;
    IoHandler *handler;
    void *     user_data;
};

class Engine : public IoEngine
{
public:
    Engine(int size) : m_size(size)
    {}
    int Initialize();

    virtual int  AddIoEvent(int fd, int mask, IoHandler *handler, void *user_data);
    virtual void DeleteIoEvent(int fd, int mask);

    void Start();
    void Release();

public:
    int                           m_size;
    int                           m_efd;
    std::map<int, IoHandlerCtx *> m_fds;
};

#endif
