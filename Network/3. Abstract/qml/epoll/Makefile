# Linux TCP 回射 —— 抽象事件引擎的 epoll 实现
# make          构建服务端和客户端
# make server   只构建 echo_server
# make client   只构建 client
# make clean    清理构建产物

CXX      := g++
CXXFLAGS := -Wall -Wextra -Wpedantic -O2 -std=c++11

BINS := echo_server client

.PHONY: all server clean

all: $(BINS)

server: echo_server

echo_server: echo_server.cpp epoll_engine.cpp epoll_engine.h ../event_engine.h
	$(CXX) $(CXXFLAGS) echo_server.cpp epoll_engine.cpp -o $@

client: client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(BINS)
