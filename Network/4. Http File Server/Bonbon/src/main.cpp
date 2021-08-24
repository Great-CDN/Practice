
#include <cstdio>
#include <cstdlib>
#include <iostream>

#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "be_engine.h"
#include "socket.h"
#include "str.h"

#define INFO(x)  std::cout << GetTime() << "\tINFO\t" << x << " [" << __FILE__ << ":" << __LINE__ << "]" << std::endl
#define ERROR(x) std::cout << GetTime() << "\tERROR\t" << x << " [" << __FILE__ << ":" << __LINE__ << "]" << std::endl

std::string GetTime()
{
    time_t    t = time(NULL);
    struct tm tm_now;
    localtime_r(&t, &tm_now);
    char str_time[20] = {0};
    strftime(str_time, 20, "%Y-%m-%d %H:%M:%S", &tm_now);
    return std::string(str_time, 19);
}

class Connection
{
public:
    Connection(int sock_fd) : m_socket(sock_fd)
    {}

    int Fd() const
    {
        return m_socket.Fd();
    }
    void SetRemote(const std::string &ip, uint16_t port)
    {
        m_remote.ip = ip;
        m_remote.port = port;
    }

    std::string Remote() const
    {
        return m_remote.ip + ":" + STR::UInt64ToStr(m_remote.port);
    }

protected:
    net::Socket   m_socket;
    net::InetAddr m_local;
    net::InetAddr m_remote;
    void *        m_user_data;
};

struct HttpRequest
{
    std::string method;
    std::string request_uri;
    Connection *conn;
    int         file_fd;
    int64_t     file_size;
    char        send_buf[8192];
    char        recv_buf[8192];
    int64_t     recv_bytes;
    int64_t     send_bytes;

    HttpRequest() : conn(NULL), file_fd(-1), file_size(-1), recv_bytes(0), send_bytes(0)
    {}
};

class HttpServer : public IIoHandler
{
public:
    HttpServer(BeEngine *engine) : m_engine(engine), m_listen_sock(NULL)
    {}
    ErrCode Initialize(const std::string &ip, uint16_t port, const std::string &root_path);

    void Destroy(HttpRequest *req);

    virtual void OnRead(FD fd, void *data, int32_t mask);
    virtual void OnWrite(FD fd, void *data, int32_t mask);

private:
    BeEngine *   m_engine;
    net::Socket *m_listen_sock;
    std::string  m_root_path;
};

ErrCode HttpServer::Initialize(const std::string &ip, uint16_t port, const std::string &root_path)
{
    ErrCode err = EC_OK;
    do
    {
        Assert(!m_listen_sock);

        ErrCode err = EC_OK;
        do
        {
            int fd = net::Socket::NewTcpSocket(AF_INET);
            if (fd == -1)
            {
                err = errno;
                break;
            }

            m_listen_sock = new net::Socket(fd);
            err = m_listen_sock->SetNonBlock();
            if (err)
            {
                break;
            }

            m_listen_sock->SetReuseAddr(TRUE);
            err = m_listen_sock->Bind(ip, port);
            if (err)
            {
                break;
            }
            m_listen_sock->Listen(1024);
            m_engine->AddIoEvent(m_listen_sock->Fd(), AE_READABLE, this, NULL);
            m_root_path = root_path;
        } while (0);

        return err;
    } while (0);
    return err;
}

void HttpServer::OnRead(FD fd, void *data, int32_t mask)
{
    if (fd == m_listen_sock->Fd())
    {
        std::string remote_ip;
        uint16_t    remote_port;
        FD          conn_fd = m_listen_sock->Accept(remote_ip, remote_port);
        if (conn_fd == -1)
        {
            return;
        }
        HttpRequest *req = new HttpRequest;
        Connection * conn = new Connection(conn_fd);
        conn->SetRemote(remote_ip, remote_port);
        req->conn = conn;
        m_engine->AddIoEvent(conn_fd, AE_READABLE, this, req);
    }
    else
    {
        Assert(data);
        HttpRequest *req = (HttpRequest *)data;
        ErrCode      err = EC_OK;
        int          n = read(fd, req->recv_buf + req->recv_bytes, sizeof(req->recv_buf) - req->recv_bytes);
        if (n == -1)
        {
            if (errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
            {
                err = errno;
            }
        }
        else if (n == 0)
        {
            err = EC_EOF;
        }
        else
        {
            const char *s = strstr(req->recv_buf, "\r\n\r\n");
            if (s)
            {
                req->method = STR::GetKey(req->recv_buf, " ");
                req->request_uri = STR::GetValue(req->recv_buf, " ", " ");
                std::string file = m_root_path + req->request_uri;

                struct stat st;
                int         result = stat(file.c_str(), &st);
                req->file_size = (result == -1) ? -1 : st.st_size;
                req->file_fd = open(file.c_str(), O_RDWR | O_LARGEFILE);
                m_engine->DeleteIoEvent(fd, AE_READABLE);
                m_engine->AddIoEvent(fd, AE_WRITABLE, this, req);
                INFO("[req:" << (void *)req << "] receive req " << req->request_uri
                             << ", client: " << req->conn->Remote());
            }
        }
        if (err)
        {
            ERROR("[req:" << (void *)req << "] failed to read req, err: " << err
                          << ", client: " << req->conn->Remote());
            m_engine->DeleteIoEvent(fd, AE_READABLE);
            Destroy(req);
        }
    }
}

void HttpServer::OnWrite(FD fd, void *data, int32_t mask)
{
    HttpRequest *req = (HttpRequest *)data;
    ErrCode      err = EC_OK;

    if (req->file_fd == -1)
    {
        int l = snprintf(req->send_buf, sizeof(req->send_buf), "HTTP/1.1 404 NOT FOUND\r\nContent-Length: 0\r\n\r\n");
        int N = write(fd, req->send_buf, l);
        ERROR("[req:" << (void *)req << "] there is no file, root: " << m_root_path << ", uri: " << req->request_uri);
        m_engine->DeleteIoEvent(fd, AE_WRITABLE);
        Destroy(req);
    }
    else if (req->send_bytes == 0)
    {
        int l = snprintf(req->send_buf, sizeof(req->send_buf), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n",
                         req->file_size);
        int n = read(req->file_fd, req->send_buf + l, sizeof(req->send_buf) - l);
        int N = write(fd, req->send_buf, n + l);
        if (N == -1 && errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
        {
            m_engine->DeleteIoEvent(fd, AE_WRITABLE);
            Destroy(req);
        }
        req->send_bytes += N;
    }
    else if (req->send_bytes < req->file_size)
    {
        int n = read(req->file_fd, req->send_buf, sizeof(req->send_buf));
        int N = write(fd, req->send_buf, n);
        if (N == -1 && errno != EINTR && errno != EWOULDBLOCK && errno != EAGAIN)
        {
            m_engine->DeleteIoEvent(fd, AE_WRITABLE);
            Destroy(req);
        }
        req->send_bytes += N;
    }
    else
    {
        m_engine->DeleteIoEvent(fd, AE_WRITABLE);
        Destroy(req);
    }
}

void HttpServer::Destroy(HttpRequest *req)
{
    if (req->file_fd != -1)
    {
        close(req->file_fd);
    }
    close(req->conn->Fd());
    delete req->conn;
    delete req;
}

int main(int argc, char **argv)
{
    uint16_t    port = 0;
    std::string root;
    if (argc != 3)
    {
        ERROR("usage: http_file_server [port] [root_path]");
        return 0;
    }
    if (argc == 3)
    {
        port = atoll(argv[1]);
        root = argv[2];
    }

    ErrCode err = EC_OK;
    do
    {
        BeEngine *engine = new BeEngine(65535);
        err = engine->Initialize();
        if (err)
        {
            ERROR("failed to initialize engine, err: " << err);
            break;
        }

        HttpServer server(engine);
        err = server.Initialize("0.0.0.0", port, root);
        if (err)
        {
            break;
        }

        engine->Main();

    } while (0);
}
