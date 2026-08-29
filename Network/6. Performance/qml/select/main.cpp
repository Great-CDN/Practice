
#include "select_engine.h"
#include "http_server.h"
#include "net_util.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// 引擎子类：把每轮回调转发给服务器
// （先建引擎后建服务器，用回填指针打破互相引用的构造顺序问题）
class ServerEngine : public SelectEngine {
public:
    void SetServer(HttpServer* s) { srv_ = s; }
    void OnLoop() override { if (srv_) srv_->Tick(); }
private:
    HttpServer* srv_ = nullptr;
};

int main(int argc, char** argv) {
    int port = (argc > 1) ? atoi(argv[1]) : 8090;

    signal(SIGPIPE, SIG_IGN);   // 对端断开后 send 返回 EPIPE 而不是杀进程
    setvbuf(stdout, NULL, _IONBF, 0);

    // backlog 1024：突发连接潮不丢握手（原版 16 在压测下会丢连接）
    FD listen_fd = CreateListenSocket(port, 1024);
    if (listen_fd < 0) {
        fprintf(stderr, "[FATAL] listen on port %d failed: %s\n", port, strerror(errno));
        return EXIT_FAILURE;
    }

    ServerEngine engine;
    HttpServer server(engine, listen_fd);
    engine.SetServer(&server);
    engine.AddIoEvent(listen_fd, kReadEvent, &server, NULL);

    LogLine("http-select listening on 0.0.0.0:%d (pid %d)", port, getpid());

    // 50ms 循环粒度：驱动直播 100ms 定帧、/slow/ 2s 到期与周期统计。
    // 有事件的轮次 select 会立即返回，请求延迟不受该粒度影响。
    return engine.Run(50) == kOk ? EXIT_SUCCESS : EXIT_FAILURE;
}
