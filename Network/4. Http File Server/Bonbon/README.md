# HttpFileServer

Simple Http File Server

实现了简单的Http文件服务器

## 构建
执行make命令即可

## 使用
http_file_server [port] [root_path]
port为服务器监听端口，root_path为文件服务器根目录对应的服务器物理目录

示例
```
[root@localhost HttpFileServer]# ./http_file_server 8000 /tmp
2021-08-22 11:19:25	INFO	[req:0x1a7c180] receive req /xx.txt, client: 127.0.0.1:40382 [src/main.cpp:181]
2021-08-22 11:19:28	INFO	[req:0x1a7c180] receive req /xx2.txt, client: 127.0.0.1:40384 [src/main.cpp:181]
2021-08-22 11:19:28	ERROR	[req:0x1a7c180] there is no file, root: /tmp, uri: /xx2.txt [src/main.cpp:203]
```

## 说明
1. 事件库使用ae_event的封装，但有了接口，后续可平滑替换为自己的实现
2. 编码、日志符合开发规范要求
3. 为求简单，使用req的指针作为ID，容易出现重复
4. 集成了clangformat配置进行代码格式化
5. 对HTTP协议做了最小支持，支持对接curl
6. 无内存泄漏，FD泄漏
