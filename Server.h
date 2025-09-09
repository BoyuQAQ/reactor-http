#include <arpa/inet.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <assert.h>
#include <unistd.h> 
#include <locale.h>
#include <ctype.h>
#include <pthread.h>

//初始化监听的套接字
int initListenFd(unsigned short port);
//启动epoll
int epollRun(int lfd);
//和客户端建立连接
void* acceptClient(void* arg);
//接收http请求
//int recvHttpRequest(int cfd, int epfd);
void* recvHttpRequest(void* arg);

void urldecode(char* dst, const char* src);
//解析请求行
int parseReuqestLine(const char* line, int cfd);
//发送文件
int sendFile(const char* fileNmae, int cfd);
//发送响应头(状态行+响应头)
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int len);
//文件后缀名匹配
const char* getFileType(const char* fileName);
//发送目录
int sendDir(const char* dirName,const char* urlPath, int cfd);