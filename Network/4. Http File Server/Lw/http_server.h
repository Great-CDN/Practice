#ifndef __TS_H__
#define __TS_H__

#include "engine.h"

enum HttpProgress
{
    REQUEST,
    RESPONSE,
};

struct HttpReqCtx
{
    int          fd;
    uint8_t      buffer[4096];
    int          len;
    HttpProgress progress;

    HttpReqCtx() : len(0), progress(REQUEST)
    {
        buffer[4095] = '\0';
    }
};

struct HttpRespCtx
{
    int         fd;
    long long   sendn;
    char        rest[8 * 1024];
    int         rest_len;

    HttpRespCtx() : fd(-1), sendn(0), rest_len(0)
    {}
};

struct HttpResp
{
    int          status;
    int          header_len;
    long long    content_length;
    HttpRespCtx* ctx;

    HttpResp() : ctx(NULL)
    {}
};

struct HttpReq
{
    std::string method;
    std::string path;
    void* user_data;
    HttpResp* resp;
    HttpReqCtx* ctx;

    HttpReq() : resp(NULL), ctx(NULL)
    {}
};

class TestHttpServer : public IoHandler
{
public:
    TestHttpServer(Engine* engine) : m_engine(engine)
    {}

    int  Initialize();
    int  Listen(std::string ip, unsigned short int port);
    void Release();

    virtual void OnRead(int fd, void* data, int mask);
    virtual void OnWrite(int fd, void* data, int mask);

    virtual int  ParseReq(HttpReq* req);
    virtual void OnReq(HttpReq* req);
    virtual void CloseReq(HttpReq* req);

public:
    Engine* m_engine;
    int                      m_fd;
    std::map<int, HttpReq*> m_reqs;
};

#endif
