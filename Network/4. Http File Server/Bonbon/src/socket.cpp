
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#include "socket.h"

using namespace net;

Socket::Socket(int sock_fd) : m_sockfd(sock_fd)
{}

int Socket::NewTcpSocket(uint16_t af)
{
    return socket(af, SOCK_STREAM, 0);
}

int Socket::NewUdpSocket(uint16_t af)
{
    return socket(af, SOCK_DGRAM, 0);
}

ErrCode Socket::SetNonBlock()
{
    int32_t cflags = fcntl(m_sockfd, F_GETFL, 0);
    return fcntl(m_sockfd, F_SETFL, cflags | O_NONBLOCK) ? errno : EC_OK;
}

ErrCode Socket::SetReuseAddr(Bool on)
{
    const int flag = on;
    return SetOption(SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
}

ErrCode Socket::SetReusePort(Bool on)
{
    const int flag = on;
    ErrCode   err = 0;
#ifdef SO_REUSEPORT
    err = SetOption(SOL_SOCKET, SO_REUSEPORT, &flag, sizeof(flag));
#endif
    return err;
}

int32_t Socket::SetRecvBuff(int32_t size)
{
    int optVal = size;
    int optLen = sizeof(int);
    return setsockopt(m_sockfd, SOL_SOCKET, SO_RCVBUF, (char*)&optVal, optLen);
}

int32_t Socket::SetSendBuff(int32_t size)
{
    int optVal = size;
    int optLen = sizeof(int);
    return setsockopt(m_sockfd, SOL_SOCKET, SO_SNDBUF, (char*)&optVal, optLen);
}

ErrCode Socket::SetOption(int32_t level, int32_t optname, const int* optval, int32_t optlen)
{
    return setsockopt(m_sockfd, level, optname, optval, optlen) ? errno : EC_OK;
}

ErrCode Socket::GetLocalAddr(InetAddr& local_addr)
{
    sockaddr_in local;
    socklen_t   len = sizeof(local);
    getsockname(m_sockfd, (sockaddr*)&local, &len);
    char addr[INET6_ADDRSTRLEN];
    inet_ntop(local.sin_family, (void*)&local.sin_addr, addr, sizeof(addr));
    local_addr.ip = addr;
    local_addr.port = ntohs(local.sin_port);
    return EC_OK;
}

ErrCode Socket::GetRemoteAddr(InetAddr& remote_addr)
{
    sockaddr_in remote;
    socklen_t   len = sizeof(remote);
    getpeername(m_sockfd, (sockaddr*)&remote, &len);
    char addr[INET6_ADDRSTRLEN];
    inet_ntop(remote.sin_family, (void*)&remote.sin_addr, addr, sizeof(addr));
    remote_addr.ip = addr;
    remote_addr.port = ntohs(remote.sin_port);
    return EC_OK;
}

ErrCode Socket::GetErr()
{
    int       error;
    socklen_t len = sizeof(error);
    if (getsockopt(m_sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
    {
        return EC_ERR;
    }
    return error;
}

ErrCode Socket::Connect(const std::string& ip, uint16_t port)
{
    ErrCode err = EC_OK;
    do
    {
        uint16_t af = AF_INET;
        sockaddr_storage addr_storage;
        sockaddr* addr = (sockaddr*)&addr_storage;
        socklen_t        addr_len = 0;
        if (af == AF_INET)
        {
            sockaddr_in* addr_in = (sockaddr_in*)addr;
            addr_in->sin_family = af;
            addr_in->sin_port = htons(port);
            inet_pton(af, ip.c_str(), &addr_in->sin_addr);
            addr_len = sizeof(sockaddr_in);
        }
        else if (af == AF_INET6)
        {
            sockaddr_in6* addr_in6 = (sockaddr_in6*)addr;
            addr_in6->sin6_family = af;
            addr_in6->sin6_port = htons(port);
            inet_pton(af, ip.c_str(), &addr_in6->sin6_addr);
            addr_len = sizeof(sockaddr_in6);
        }

        int ret = connect(m_sockfd, addr, addr_len);
        if (ret == -1 && errno == EINPROGRESS)
        {
            ret = 0;
        }
        err = ret == 0 ? EC_OK : errno;
    } while (0);
    return err;
}

ErrCode Socket::Bind(const std::string& ip, uint16_t port)
{
    ErrCode err = EC_OK;
    do
    {
        uint16_t af = AF_INET;
        sockaddr_storage addr_storage;
        sockaddr* addr = (sockaddr*)&addr_storage;
        socklen_t        addr_len = 0;
        if (af == AF_INET)
        {
            sockaddr_in* addr_in = (sockaddr_in*)addr;
            addr_in->sin_family = af;
            addr_in->sin_port = htons(port);
            inet_pton(af, ip.c_str(), &addr_in->sin_addr);
            addr_len = sizeof(sockaddr_in);
        }
        else if (af == AF_INET6)
        {
            sockaddr_in6* addr_in6 = (sockaddr_in6*)addr;
            addr_in6->sin6_family = af;
            addr_in6->sin6_port = htons(port);
            inet_pton(af, ip.c_str(), &addr_in6->sin6_addr);
            addr_len = sizeof(sockaddr_in6);
        }

        err = bind(m_sockfd, addr, addr_len);
        if (err)
        {
            err = errno;
            break;
        }
    } while (0);
    return err;
}

ErrCode Socket::Listen(int32_t backlog)
{
    return listen(m_sockfd, backlog) ? errno : EC_OK;
}

FD Socket::Accept(std::string& ip, uint16_t& port)
{
    sockaddr_storage addr_storage;
    socklen_t        len = sizeof(addr_storage);
    FD               fd = accept(m_sockfd, (sockaddr*)&addr_storage, &len);
    char             addr[INET6_ADDRSTRLEN];
    if (addr_storage.ss_family == AF_INET)
    {
        sockaddr_in* addr_in = (sockaddr_in*)&addr_storage;
        inet_ntop(addr_storage.ss_family, (void*)&addr_in->sin_addr, addr, sizeof(addr));
        ip = addr;
        port = ntohs(addr_in->sin_port);
    }
    else if (addr_storage.ss_family == AF_INET6)
    {
        sockaddr_in6* addr_in6 = (sockaddr_in6*)&addr_storage;
        inet_ntop(addr_storage.ss_family, (void*)&addr_in6->sin6_addr, addr, sizeof(addr));
        ip = addr;
        port = ntohs(addr_in6->sin6_port);
    }
    return fd;
}

ErrCode Socket::Close()
{
    Assert(m_sockfd >= 0);
    ErrCode err = EC_OK;
    if (m_sockfd >= 0)
    {
        err = shutdown(m_sockfd, SHUT_RDWR);
        err = close(m_sockfd);
        m_sockfd = -1;
    }
    return err;
}
