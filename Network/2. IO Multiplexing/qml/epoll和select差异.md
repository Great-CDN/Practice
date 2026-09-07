# Linux 下 epoll 与 select 的差异

本章的两个服务端功能相同：都使用一个线程和非阻塞 socket 提供 TCP 回射服务。
它们的主要差别不是 `recv` 或 `send`，而是内核如何记录关注项、如何返回就绪事件。

## 1. 关注项的管理

`select` 会修改传入的 `fd_set`。因此服务端每轮都要重新清空并填充读写集合，再把
`maxfd + 1` 和这些集合传给内核：

```cpp
FD_ZERO(&readfds);
FD_ZERO(&writefds);
FD_SET(listenfd, &readfds);
for (const auto& item : g_conns) {
    if (!item.second.read_closed) FD_SET(item.first, &readfds);
    if (!item.second.out.empty()) FD_SET(item.first, &writefds);
}
select(maxfd + 1, &readfds, &writefds, nullptr, nullptr);
```

`epoll` 用 `epoll_ctl` 把描述符登记到内核中的 epoll 实例。连接建立时执行 `ADD`，
关注项改变时才执行 `MOD`：

```cpp
epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);

event.events = EPOLLIN | EPOLLRDHUP;
if (!conn.out.empty()) event.events |= EPOLLOUT;
epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event);
```

两种实现都只在确有待发送数据时关注“可写”。否则 socket 通常一直可写，会让事件
循环反复醒来并占用 CPU。

## 2. 就绪事件的获取

`select` 返回后，程序仍要遍历本轮登记的全部连接，并用 `FD_ISSET` 判断哪些连接就绪。
所以即使只有少量连接活跃，也需要扫描整个集合。

`epoll_wait` 直接填充就绪事件数组，程序只遍历 `events[0..ready)`。当连接数量很多、
但每轮只有少量连接活跃时，这通常比扫描全部连接更合适。

## 3. 描述符数量

`select` 的描述符必须小于 `FD_SETSIZE`；本示例在接收连接时显式检查这个边界。
Linux 上该常量通常为 1024，但应以当前编译环境中的实际值为准。

`epoll` 没有 `FD_SETSIZE` 这一固定集合边界，实际连接数仍受进程文件描述符上限、
内存和系统配置约束。

## 4. 本例共同采用的规则

- 监听 socket 和客户端 socket 都设置为非阻塞。
- `accept` 和 `recv` 循环执行到 `EAGAIN/EWOULDBLOCK`，避免遗漏当前已到达的数据。
- `send` 的短写数据保存在 `Conn::out`，等待后续可写事件继续发送。
- 对端半关闭后，服务端先发完已收到的数据，再关闭连接。
- 服务端使用 `MSG_NOSIGNAL`，避免单个断开的客户端通过 `SIGPIPE` 终止整个进程。

本例的 epoll 使用水平触发（LT），没有启用 `EPOLLET`。即便如此，排空非阻塞 socket
仍能减少重复唤醒，也为理解边缘触发的正确写法打下基础。
