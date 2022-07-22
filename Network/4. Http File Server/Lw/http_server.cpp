
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

#include "http_server.h"

std::string status_2_str(int status)
{
    std::string res = "";
    switch (status)
    {
    case 200:
        res = "OK";
        break;
    case 404:
        res = "Not Found";
        break;
    default:
        res = "Internal Server Error";
        break;
    }

    return res;
}

int TestHttpServer::Initialize()
{
    int res = 0;
    do
    {
        m_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (m_fd < 0)
        {
            res = -1;
            break;
        }

        bool flag = true;
        res = setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        if (res < 0)
        {
            break;
        }

        res = setsockopt(m_fd, SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
        if (res < 0)
        {
            break;
        }

        res = fcntl(m_fd, F_SETFL, fcntl(m_fd, F_GETFD, 0) | O_NONBLOCK);
        if (res < 0)
        {
            break;
        }

    } while (0);

    return res;
}

int TestHttpServer::Listen(std::string ip, unsigned short int port)
{
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip.c_str());

    int res = bind(m_fd, (struct sockaddr*)&addr, sizeof(addr));
    if (res == -1)
    {
        return -1;
    }

    res = listen(m_fd, 1000);
    if (res == -1)
    {
        return -1;
    }

    m_engine->AddIoEvent(m_fd, EPOLLIN, this, NULL);
    return 0;
}

void TestHttpServer::Release()
{
    close(m_fd);
}

void TestHttpServer::OnRead(int fd, void* data, int mask)
{
    do
    {
        if (fd == m_fd)
        {
            struct sockaddr_in remote;
            socklen_t          len = sizeof(remote);
            int                client_fd = accept(fd, (struct sockaddr*)&remote, &len);
            if (client_fd == -1)
            {
                break;
            }

            m_engine->AddIoEvent(client_fd, EPOLLIN, this, NULL);
            HttpReq* req = new HttpReq();
            req->ctx = new HttpReqCtx();
            req->ctx->fd = client_fd;
            assert(!m_reqs.count(client_fd));
            m_reqs[client_fd] = req;

            printf("[fd:%d]new connection, remote: %s:%d\n", client_fd, (char*)inet_ntoa(remote.sin_addr),
                htons(remote.sin_port));
        }
        else
        {
            assert(m_reqs.count(fd));
            HttpReq* req = m_reqs[fd];
            int      buf_len = sizeof(req->ctx->buffer) - req->ctx->len - 1;
            if (!buf_len)
            {
                CloseReq(req);
                break;
            }

            int res = read(fd, req->ctx->buffer + req->ctx->len, buf_len);
            if (res == -1)
            {
                if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
                {
                    CloseReq(req);
                }
                break;
            }

            if (res == 0)
            {
                CloseReq(req);
                break;
            }

            req->ctx->len += res;

            res = ParseReq(req);
            if (res < 0)
            {
                CloseReq(req);
                break;
            }

            if (res > 0)
            {
                OnReq(req);
            }
        }
    } while (0);
}

void TestHttpServer::OnWrite(int fd, void* data, int mask)
{
    do
    {
        assert(m_reqs.count(fd));
        HttpReq* req = m_reqs[fd];

        if (!req->resp->ctx->sendn)
        {
            char resp[1024];
            int  n = sprintf(
                resp,
                "HTTP/1.1 %d %s \r\nContent-Length: %lld \r\nContent-Type: text/plain; charset=utf-8 \r\nConnection: close "
                "\r\n\r\n",
                req->resp->status, status_2_str(req->resp->status).c_str(), req->resp->content_length);

            req->resp->header_len = n;
            int res = write(fd, resp, req->resp->header_len);
            if (res == -1)
            {
                if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
                {
                    CloseReq(req);
                }
                break;
            }

            if (res == 0)
            {
                CloseReq(req);
                break;
            }

            assert(res <= req->resp->header_len);
            if (res < req->resp->header_len)
            {
                memcpy(req->resp->ctx->rest, resp + res, req->resp->header_len - res);
                req->resp->ctx->rest_len = req->resp->header_len - res;
            }
            req->resp->ctx->sendn += res;
        }
        else
        {
            if (req->resp->ctx->rest_len)
            {
                int res = write(fd, req->resp->ctx->rest, req->resp->ctx->rest_len);
                if (res == -1)
                {
                    if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
                    {
                        CloseReq(req);
                    }
                    break;
                }

                if (res == 0)
                {
                    CloseReq(req);
                    break;
                }

                assert(res <= req->resp->ctx->rest_len);
                if (res < req->resp->ctx->rest_len)
                {
                    memmove(req->resp->ctx->rest, req->resp->ctx->rest + res, req->resp->ctx->rest_len - res);
                }

                req->resp->ctx->sendn += res;
                req->resp->ctx->rest_len -= res;
            }
            else
            {
                uint8_t buf[8 * 1024];
                int     readn = read(req->resp->ctx->fd, buf, 8 * 1024);
                if (readn == -1)
                {
                    CloseReq(req);
                    break;
                }

                int res = write(fd, buf, readn);
                if (res == -1)
                {
                    if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
                    {
                        CloseReq(req);
                        break;
                    }
                }

                if (res == 0)
                {
                    CloseReq(req);
                    break;
                }

                assert(res <= readn);
                if (res < readn)
                {
                    memcpy(req->resp->ctx->rest, buf + res, readn - res);
                    req->resp->ctx->rest_len = readn - res;
                }

                req->resp->ctx->sendn += res;
            }
        }

        if (req->resp->status != 200 || req->resp->header_len + req->resp->content_length == req->resp->ctx->sendn)
        {
            CloseReq(req);
            break;
        }
    } while (0);
}

int TestHttpServer::ParseReq(HttpReq* req)
{
    int res = 0;
    do
    {
        char* r = strstr((char*)(req->ctx->buffer), "\r\n\r\n");
        if (r)
        {
            char* head = (char*)req->ctx->buffer;
            char* tail = strstr(head, " ");
            int   len = tail - head;
            req->method = std::string(head, len);
            if (req->method != "GET")
            {
                res = -1;
                break;
            }

            head = tail + 1;
            tail = strstr(head, "?");
            if (!tail)
            {
                tail = strstr(head, " ");
            }
            len = tail - head;
            req->path = std::string(head, len);
            res = 1;
        }

    } while (0);
    return res;
}

void TestHttpServer::OnReq(HttpReq* req)
{
    req->resp = new HttpResp();
    req->resp->ctx = new HttpRespCtx();
    if (access(req->path.c_str(), F_OK) == -1)
    {
        req->resp->status = 404;
        req->resp->content_length = 0;
    }
    else
    {
        req->resp->status = 200;

        struct stat info;
        stat(req->path.c_str(), &info);
        req->resp->content_length = info.st_size;
        req->resp->ctx->fd = open(req->path.c_str(), O_RDWR | O_LARGEFILE);
    }

    req->ctx->progress = RESPONSE;
    m_engine->DeleteIoEvent(req->ctx->fd, EPOLLIN);
    m_engine->AddIoEvent(req->ctx->fd, EPOLLOUT, this, NULL);
}

void TestHttpServer::CloseReq(HttpReq* req)
{
    assert(req->ctx);
    if (req->resp)
    {
        assert(req->resp->ctx);
        if (req->resp->ctx->fd != -1)
        {
            close(req->resp->ctx->fd);
        }
        delete req->resp->ctx;
        delete req->resp;
    }

    if (req->ctx->progress == REQUEST)
    {
        m_engine->DeleteIoEvent(req->ctx->fd, EPOLLIN);
    }
    else
    {
        m_engine->DeleteIoEvent(req->ctx->fd, EPOLLOUT);
    }

    m_reqs.erase(req->ctx->fd);
    close(req->ctx->fd);
    delete req->ctx;
    delete req;
}

int main(int argc, char** argv)
{
    Engine* engine = new Engine(65535);
    engine->Initialize();

    TestHttpServer* server = new TestHttpServer(engine);
    server->Initialize();
    server->Listen("0.0.0.0", 12345);

    engine->Start();
    engine->Release();
    server->Release();

    return 0;
}
