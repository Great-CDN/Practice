# Linux TCP 回射 —— select 并发版
# 用法：
#   make            构建全部目标（select 并发服务器 + 客户端）
#   make server     只构建服务器 my_echo
#   make client     只构建客户端 client
#   make clean      清理所有产物

CXX      := g++
CXXFLAGS := -Wall -Wextra -Wpedantic -O2 -std=c++11

BINS := my_echo client

.PHONY: all server clean

all: $(BINS)

server: my_echo

my_echo: my_echo.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

client: client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f $(BINS)
