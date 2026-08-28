# TCP 回射 —— epoll 并发版，多目标构建（Linux）
# 用法：
#   make            构建全部目标（epoll 服务器 + 客户端）
#   make server     只构建服务器 echo_epoll
#   make client     只构建客户端 client
#   make clean      清理所有产物

CXX      := g++
CXXFLAGS := -Wall -Wextra -O2 -std=c++11

BINS := echo_epoll client

.PHONY: all server client clean

all: $(BINS)

server: echo_epoll
client: client

echo_epoll: echo_epoll.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

client: client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(BINS)
