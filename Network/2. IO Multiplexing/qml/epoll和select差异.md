# epoll和select差异

## 1. 机制差异

### 1.1 fd集合的管理方式

select 版每轮循环都要**重建**集合并拷给内核：

```c++
// select 版：每轮都做
for (;;) {
    fd_set rs, ws;
    FD_ZERO(&rs); FD_ZERO(&ws);
    FD_SET(listenfd, &rs);
    for (auto& kv : g_conns) {          // 遍历所有连接重新登记
        FD_SET(kv.first, &rs);
        if (!kv.second.out.empty()) FD_SET(kv.first, &ws);
    }
    select(0, &rs, &ws, NULL, NULL);    // 整个集合拷入内核
```



epoll 版**注册一次**，以后只做增量修改：

```c++
// epoll 版：一次性注册（main 里只写一遍）
epoll_ctl(g_epfd, EPOLL_CTL_ADD, listenfd, &ev);   // 新连接时 ADD

// 想改关注的事件，才需要 MOD —— 这就是多出来的 update_interest() 函数
epoll_ctl(g_epfd, EPOLL_CTL_MOD, fd, &ev);
```





### 1.2 事件获取方式

连接数越多，这个差距越大：select 是“每轮服务所有连接的成本”，epoll 是“每轮只服务活跃连接的成本”。

```
// select：返回后不知道谁就绪，要拿快照 O(n) 扫全部连接
std::vector<SOCKET> fds;                  // 还得先做快照防遍历中删元素
for (SOCKET fd : fds) {
    if (FD_ISSET(fd, &rs)) { ... }        // 绝大多数连接其实没就绪，白查
}

// epoll：内核直接给就绪列表，只处理真正有事的
int n = epoll_wait(g_epfd, evs, MAX_EVENTS, -1);
for (int i = 0; i < n; ++i) { ... }       // n = 就绪个数，无浪费
```



### 1.3 **容量上限**

select 受 `FD_SETSIZE`（Linux 1024，Windows 64）硬限制；epoll 无此限制。





## 2.平台差异

select在windows

epoll在linux









