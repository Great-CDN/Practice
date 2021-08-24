
#ifndef __SOCKET_H__
#define __SOCKET_H__

#include <string>

#include "sdk_type.h"

namespace net
{
struct InetAddr
{
    std::string ip;
    uint16_t    port;

    InetAddr() : port(0)
    {}
};

class Socket
{
public:
    Socket(int fd);

    FD Fd() const
    {
        return m_sockfd;
    }

    static int NewTcpSocket(uint16_t af);
    static int NewUdpSocket(uint16_t af);

    ErrCode SetNonBlock();
    ErrCode SetReuseAddr(Bool on);
    ErrCode SetReusePort(Bool on);
    ErrCode GetErr();
    ErrCode SetOption(int32_t level, int32_t optname, const int *optval, int32_t optlen);

    ErrCode GetLocalAddr(InetAddr &local_addr);
    ErrCode GetRemoteAddr(InetAddr &remote_addr);

    int32_t SetRecvBuff(int32_t size);
    int32_t SetSendBuff(int32_t size);
    ErrCode Connect(const std::string &ip, uint16_t port);
    ErrCode Bind(const std::string &ip, uint16_t port);
    ErrCode Listen(int32_t backlog);
    FD      Accept(std::string &clientIp, uint16_t &clientPort);
    ErrCode Close();

private:
    int m_sockfd;
};
} // namespace net

#endif
