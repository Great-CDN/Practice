#pragma once
#pragma once
#include "engine.h"
#include <string.h>
struct HttpRequest {
	char method[20];
	char uri[20];
	char body[4096];
	char api[20];
	int content_length;
};

struct WriteCtx {
	char* write_buf;
	int* write_len;
	char* write_head;
	char* write_tail;
};

struct ReadCtx {
	char* read_buf;
	int* read_len;
};

struct Buf {
	WriteCtx* write_ctx;
	ReadCtx* read_ctx;
};

//server part
class Server :public IIOHandler {
public:
	virtual int OnRead(FD fd, void* date, int mask);
	virtual int OnWrite(FD fd, void* date, int mask);
	Server(char* listen_ip, char* listen_port, int buf_len, EventEngine* epoll, const char* path);
	ErrCode Listen();
	ErrCode ProcessRequest(FD fd, char* read_buf, HttpRequest* req);
	ErrCode ProcessApi(FD fd, WriteCtx* write_ctx, char* uri, char* api);
	ErrCode SendHead(FD fd, int status_code, int length = 0);
	ErrCode SendFile(FD fd, WriteCtx* write_ctx);
	ErrCode IsExist(char* file_name, FD fd);
	void Release(FD fd);
	int Stop();
private:
	char* m_listen_ip;
	char* m_listen_port;
	FD m_server_fd;
	int m_buf_len;
	EventEngine* m_epoll;
	const char* m_path;
	std::map<FD, HttpRequest*>m_reqs;
	FD m_file_fd;
};

