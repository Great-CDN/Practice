#include <assert.h>
#include <arpa/inet.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include<iostream> 
#include "server.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

Server::Server(char* listen_ip, char* listen_port, int buf_len, EventEngine* epoll, const char* path) {
	m_listen_ip = listen_ip;
	m_listen_port = listen_port;
	m_buf_len = buf_len;
	m_epoll = epoll;
	m_path = path;
}


int Server::Listen() {
	ErrCode err = 0;
	do {
		m_server_fd = socket(AF_INET, SOCK_STREAM, 0);
		int reuse = 1;
		err = setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, (const void*)&reuse, sizeof(int));
		if (err < 0)
		{
			break;
		}
		err = setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEPORT, (const void*)&reuse, sizeof(int));
		if (err < 0)
		{
			break;
		}
		//server socket address
		struct sockaddr_in  hostaddr;
		memset(&hostaddr, 0, sizeof(hostaddr));
		hostaddr.sin_family = AF_INET;
		hostaddr.sin_addr.s_addr = inet_addr(m_listen_ip);
		hostaddr.sin_port = htons(atoi(m_listen_port));
		err = bind(m_server_fd, (struct sockaddr*)&hostaddr, sizeof(hostaddr));
		if (err) {
			break;
		}
		err = listen(m_server_fd, 1024);
		if (err) {
			break;
		}
		Buf* buf = new Buf();
		err = m_epoll->AddIoEvent(m_server_fd, R, this, buf);
		if (err == -1) {
			break;
		}
	} while (0);
	return err;
}


ErrCode Server::OnRead(FD fd, void* date, int mask) {
	ErrCode err = 0;
	do {
		if (fd == m_server_fd) {
			socklen_t len = 0;
			struct sockaddr_in  client_addr;
			int client_sd = accept(m_server_fd, (sockaddr*)&client_addr, &len);
			if (client_sd == -1) {
				err = -1;
				break;
			}
			Buf* buf = new Buf();
			buf->write_ctx = new WriteCtx();
			buf->read_ctx = new ReadCtx();

			buf->read_ctx->read_buf = new char[m_buf_len];
			buf->read_ctx->read_len = new int(0);
			buf->write_ctx->write_buf = new char[m_buf_len];
			buf->write_ctx->write_len = new int(0);
			buf->write_ctx->write_head = buf->write_ctx->write_buf;
			buf->write_ctx->write_tail = buf->write_ctx->write_buf + m_buf_len;
			m_epoll->AddIoEvent(client_sd, R, this, buf);
		}
		else {
			Buf* buf = (Buf*)(m_epoll->GetFd(fd)->user_data);
			char* read_buf = buf->read_ctx->read_buf;
			int* read_len = buf->read_ctx->read_len;
			int n = read(fd, read_buf + *read_len, m_buf_len - *read_len);
			if (n < 0) {
				err = -1;
				break;
			}
			if (n == 0) {
				Release(fd);
				break;
			}
			*read_len += n;
			if (*read_len > m_buf_len) {
				Release(fd);
				break;
			}
			if (strstr(read_buf, "\r\n\r\n")) {
				m_epoll->DeleteIoEvent(fd, R);
				m_epoll->AddIoEvent(fd, W, this, buf);
			}
			else {
				err = -1;
			}
		}
	} while (0);
	return err;
}


ErrCode Server::OnWrite(FD fd, void* data, int mask) {
	ErrCode err = 0;
	do {
		Buf* buf = ((Buf*)(m_epoll->GetFd(fd)->user_data));
		WriteCtx* write_ctx = buf->write_ctx;
		ReadCtx* read_ctx = buf->read_ctx;

		char* read_buf = read_ctx->read_buf;
		int* read_len = read_ctx->read_len;

		char* write_buf = write_ctx->write_buf;
		int* write_len = write_ctx->write_len;
		char* write_head = write_ctx->write_head;
		char* write_tail = write_ctx->write_tail;
		if (!m_reqs.count(fd)) {
			m_reqs[fd] = new HttpRequest();
			err = ProcessRequest(fd, read_buf, m_reqs[fd]);
			if (err) {
				Release(fd);
				break;
			}
		}
		err = ProcessApi(fd, write_ctx, m_reqs[fd]->uri, m_reqs[fd]->api);
		if (err) {
			Release(fd);
			break;
		}
		if (*write_len == m_reqs[fd]->content_length) {
			m_epoll->DeleteIoEvent(fd, W);
			m_epoll->AddIoEvent(fd, R, this, buf);
			*write_len = 0;
		}
	} while (0);
	return err;
}


ErrCode Server::SendHead(FD fd, int status_code, int length) {
	ErrCode err = 0;
	do {
		char head[100];
		const char* return_msg;
		if (status_code == 200) {
			return_msg = "OK";
		}
		else if (status_code == 400) {
			return_msg = "BAD REQUEST";
		}
		else if (status_code == 404)
		{
			return_msg = "NOT FOUND";
		}
		int head_n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\nContent-Length:%d\r\n\r\n", status_code, return_msg, length);
		if (head_n < 0) {
			err = -1;
			break;
		}
		int n = write(fd, head, head_n);
		if (n <= 0) {
			err = -1;
		}
	} while (0);
	return err;
}


ErrCode Server::ProcessRequest(FD fd, char* read_buf, HttpRequest* req) {
	ErrCode err = 0;
	do {
		char* saveptr = NULL;
		char row[100];
		char* row_end = strstr(read_buf, "\r\n");
		if (!row_end) {
			err = -1;
			break;
		}
		memcpy(row, read_buf, row_end - read_buf);
		char protocol[20];
		err = sscanf(row, "%[^ ] %[^ ] %[^ ]", req->method, req->uri, protocol);
		if (err <= 0) {
			err = -1;
			break;
		}
		else {
			err = 0;
		}
		char* uri = req->uri;
		if ((uri[0] != '/') || (uri[strlen(uri) - 1] == '/') || (strrchr(uri, '/') == strchr(uri, '/'))) {
			err = SendHead(fd, 400);
			break;
		}
		err = sscanf(uri, "/%[^/]", req->api);
		if (err <= 0) {
			err = -1;
			break;
		}
		else {
			err = 0;
		}
	} while (0);
	return err;
}


ErrCode Server::ProcessApi(FD fd, WriteCtx* write_ctx, char* uri, char* api) {
	ErrCode err = 0;
	int* write_len = write_ctx->write_len;
	do {
		if (strncmp(api, "file", sizeof(*api)) == 0) {
			if (*write_len == 0) {
				char file_name[1024];
				sscanf(uri, "/%*[^/]/%s", file_name);
				err = IsExist(file_name, fd);
				if (err) {
					err = SendHead(fd, 404);
					break;
				}
				else {
					err = SendHead(fd, 200, m_reqs[fd]->content_length);
					if (err) {
						break;
					}
				}
				m_file_fd = open(file_name, 'r');
				if (m_file_fd < 0) {
					err = -1;
					break;
				}
			}
			err = SendFile(fd, write_ctx); // 这里死循环无法实现并发，即同一时刻只能给一个人服务
		}
		else {
			err = SendHead(fd, 400);
			break;
		}
	} while (0);
	return err;
}


ErrCode Server::IsExist(char* file_name, FD fd) {
	ErrCode err = 0;
	do {
		char path_abs[1024];
		strcpy(path_abs, m_path);
		strcat(path_abs, file_name);
		strcpy(file_name, path_abs);
		if (access(file_name, 4)) {
			err = -1;
			break;
		}
		struct stat st;
		stat(file_name, &st);
		m_reqs[fd]->content_length = st.st_size;
	} while (0);
	return err;
}


ErrCode Server::SendFile(FD fd, WriteCtx* write_ctx) {
	ErrCode err = 0;
	char* write_buf = write_ctx->write_buf;
	int* write_len = write_ctx->write_len;
	char* write_head = write_ctx->write_head;
	char* write_tail = write_ctx->write_tail;
	do {
		int read_n;
		int write_n;
		read_n = read(m_file_fd, write_buf, m_buf_len);
		if (read_n < 0) {
			err = -1;
			break;
		}
		if ((read_n == 0) & (*write_len == m_reqs[fd]->content_length)) {
			break;
		}
		write_n = write(fd, write_buf, read_n);

		if (write_n <= 0) {
			err = -1;
			break;
		}
		*write_len += write_n;
		if (write_n == read_n) {
			memset(write_buf, '\0', write_n);
		}
		else {
			memcpy(write_buf, write_buf + write_n, read_n - write_n);
			memset(write_buf + write_n, '\0', read_n);
		}
	} while (0);
	return err;
}


void Server::Release(FD fd) {
	delete[]((Buf*)m_epoll->GetFd(fd)->user_data)->read_ctx->read_buf;
	delete[]((Buf*)m_epoll->GetFd(fd)->user_data)->write_ctx->write_buf;
	delete ((Buf*)m_epoll->GetFd(fd)->user_data)->write_ctx->write_len;
	delete ((Buf*)m_epoll->GetFd(fd)->user_data)->read_ctx->read_len;
	delete (Buf*)m_epoll->GetFd(fd)->user_data;
	delete m_epoll->GetFd(fd);
	delete m_reqs[fd];
	m_reqs.erase(fd);
	m_epoll->DelFd(fd);
	close(m_file_fd);
	close(fd);
}


int Server::Stop() {
	delete (Buf*)m_epoll->GetFd(m_server_fd)->user_data;
	delete m_epoll->GetFd(m_server_fd);
	delete m_reqs[m_server_fd];
	m_reqs.erase(m_server_fd);
	m_epoll->DelFd(m_server_fd);
	close(m_server_fd);
}


int main(int argc, char* argv[]) {
	char* ip;
	char* port;
	if (argc < 3) {
		ip = (char*)"192.168.1.143";
		port = (char*)"8888";
	}
	else {
		ip = argv[1];
		port = argv[2];
	}
	EventEngine* engine = new EventEngine();
	Server* server = new Server(ip, port, 100, engine, "/zj_files/");
	ErrCode err = engine->Init();
	if (err == -1) {
		return err;
	}
	err = server->Listen();
	if (err == -1) {
		return err;
	}
	err = engine->Run();
	if (err == -1) {
		return err;
	}
	server->Stop();
	delete(server);
	engine->Stop();
	delete(engine);
	return 0;
}



