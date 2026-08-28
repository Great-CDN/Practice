# TCP 回射 —— 多目标构建（Windows + MinGW g++）
# 用法：
#   make            构建全部目标（select 并发服务器 + 客户端）
#   make server     只构建服务器 my_echo.exe
#   make client     只构建客户端 client.exe
#   make clean      清理所有产物

CXX      := g++
CXXFLAGS := -Wall -Wextra -O2 -std=c++11
LDLIBS   := -lws2_32

BINS := my_echo.exe client.exe

.PHONY: all server client clean

all: $(BINS)

server: my_echo.exe
client: client.exe

my_echo.exe: my_echo.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

client.exe: client.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(BINS)
