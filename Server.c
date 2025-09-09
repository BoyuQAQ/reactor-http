#include "Server.h"
#include <stdio.h>
#include <sys/epoll.h>

struct FdInfo//将epoll的根结点和用于监听的文件描述符统合成一个结构体用于，给线程中的acceptClient函数作返回值
{
	int fd;
	int epfd;
	pthread_t tid;
};

int initListenFd(unsigned short port)
{
	//1、创建监听的套接字 
	int lfd = socket(AF_INET, SOCK_STREAM, 0);
	if (lfd == -1) {
		perror("socket");
		return -1;
	}
	//2、设置端口复用
	int opt = 1;
	int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	if (ret == -1) {
		perror("socket");
		return -1;
	}
	//3、绑定
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	ret = bind(lfd, (struct sockaddr*)&addr, sizeof addr);
	if (ret == -1) {
		perror("socket");
		return -1;
	}
	//4、监听
	ret = listen(lfd, 128);
	if (ret == -1)
	{
		perror("listen");
		return -1;
	}
	//5、返回监听的套接字
	return lfd;
	return 0;
}

int epollRun(int lfd)
{
	printf("服务器开始运行...(准备epoll实例函数运行)\n");
	// 在服务器启动时打印当前目录
	char cwd[1024];
	getcwd(cwd, sizeof(cwd));
	printf("Server working directory: %s\n", cwd);
	//1、创建epoll实例
	int epfd = epoll_create(1);
	if (epfd == -1) {
		perror("epoll_create");
		return -1;
	}
	//2.lfd放红黑树中
	struct epoll_event ev = { 0 };
	ev.events = EPOLLIN; //可读事件
	ev.data.fd = lfd; //监听套接字
	int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
	if (ret == -1) {
		perror("epoll_ctl");
		return -1;
	}
	//3.检测
	struct epoll_event evs[1024];
	int size = sizeof(evs) / sizeof(evs[0]);
	while (1) {
		int num = epoll_wait(epfd, evs, size, -1);
		for (int i = 0; i < num; ++i) {
			struct FdInfo* info = (struct FdInfo*)malloc(sizeof(struct FdInfo));
			int fd = evs[i].data.fd;
			info->epfd = epfd;
			info->fd = fd;
			if (fd == lfd)
			{
				//建立新连接
				//acceptClient(epfd, lfd);
				pthread_create(&info->tid, NULL, acceptClient, info);
			}
			else {
				//主要接收对端的数据
				//recvHttpRequest(fd, epfd);
				pthread_create(&info->tid, NULL, recvHttpRequest, info);
			}
		} 
	}
	return 0;
}

void* acceptClient(void* arg)
{
	struct FdInfo* info = (struct FdInfo*)arg;
	//1、建立连接
	int cfd = accept(info->fd, NULL, NULL);
	if (cfd == -1) {
		perror("accept");
		return NULL;
	}

	//2、设置非阻塞IO
	int flag = fcntl(cfd, F_GETFL);//设置标志位，并进行获取文件描述符的标志位操作
	flag |= O_NONBLOCK;	//这是标志位，意味着通过按位或操作将对应标志位从0变成1，那么属性就存在
	fcntl(cfd, F_SETFL, flag);//将属性设置给io

	//3、将cfd放入红黑树中
	struct epoll_event ev;
	ev.data.fd = cfd;//将cfd放入到红黑树中
	ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;//把EPOLLET作为事件添加到events这个整形变量里边去 
	int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1)
	{
		perror("epoll_ctl");
		close(cfd);//如果添加失败，关闭连接
		return NULL;
	}
	printf("acceptClient threadID:%ld\n", info->tid);
	free(info);
	return NULL;
}

void* recvHttpRequest(void* arg)
{
	struct FdInfo* info = (struct FdInfo*)arg;
	printf("准备接收数据...\n");
	fflush(stdout);  // 立即刷新
	int len = 0, total = 0;
	char tmp[1024] = { 0 };//防止下一次recv接收的数据覆盖掉上一次的数据，
	char buf[8192] = { 0 };
	memset(buf, 0, sizeof(buf));
	while ((len = recv(info->fd, tmp, sizeof(tmp) - 1, 0)) > 0)
	{
		if (total + len < sizeof(buf))
		{
			memcpy(buf + total, tmp, len);
		}
		total += len;
	}
	//判断数据是否被接收完毕
	if (len == -1 && errno == EAGAIN)
	{
		//解析请求行
		char* pt = strstr(buf, "\r\n");//查找请求行中的结尾\r\n字符串 
		int reqLen = pt - buf;
		buf[reqLen] = '\0';
		parseReuqestLine(buf, info->fd);
		char method[12];
		char path[1024];
		/*char line[1024] = { 0 };*/
		sscanf(buf, "%[^ ] %[^ ]", method, path);
	}
	else if (len == 0)
	{
		//客户端断开了连接
		epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
		close(info->fd);
		return 0;
	}
	else
	{
		perror("recv");
	}
	printf("recvMsg threadID:%ld\n", info->tid);
	free(info);
	return NULL;
}

void urldecode(char* dst, const char* src)
{
	printf("准备开始解码\n");
	char a, b;
	while (*src)
	{
		if (*src == '%' && (a = src[1]) && (b = src[2]) &&
			isxdigit(a) && isxdigit(b))//处理%xx格式
		{
			if (a >= 'a')a -= 'a' - 'A';//小写转大写
			if (a >= 'A')a -= ('A' - 10);//A-F→10-15
			else a -= '0';//0-9 →0-9
			if (b >= 'a')b -= 'a' - 'A';
			if (b >= 'A')b -= ('A' - 10);
			else b -= '0';
			*dst++ = 16 * a + b;//16进制转字符
			src += 3;//跳过%xx
		}
		else if (*src == '+')//+转换为空格
		{
			*dst++ = ' ';
			src++;
		}
		else
		{//其他字符直接复制
			*dst++ = *src++;
		}
	}
	*dst++ = '\0';//字符串结束
}//URL解码函数

int parseReuqestLine(const char* line, int cfd)
{
	printf("解析请求行: %s\n", line);
	//解析请求行
	char method[12] = { 0 };
	char encodepath[1024] = { 0 };//存储原始URL编码的路径(如%E5%8D%8E...)
	char decodepath[1024] = { 0 };//存储解码后的路径(如华为)
	// 直接从 line 解析（跳过第一行的 "GET / HTTP/1.1" 等）
	if (sscanf(line, "%s %s ", method, encodepath) != 2)
	{
		return -1;
	}
	
	//URL 解码
	urldecode(decodepath, encodepath);

	// 安全检查：防止路径穿越（如 /../）
	if (strstr(decodepath, "..") != NULL)
	{
		// 在parseReuqestLine中添加路径验证
		printf("Requested path: %s\n", decodepath);
		if (access(decodepath, R_OK) != 0) {
			perror("Path access error");
		}
		return -1;
	}

	//处理客户端请求的静态资源(目录或文件)
	char* file = NULL;
	if (strcmp(decodepath, "/") == 0)
	{
		file = "./";
	}
	else
	{
		file = decodepath + 1;
	}	

	//获取文件属性
	struct stat st;
	int ret = stat(file, &st);
	if (ret == -1)
	{
		perror("stat failed");
		//文件不存在--回复404
		sendHeadMsg(cfd,404,"Not Found",getFileType(".html"),-1);
		sendFile("404.html", cfd);
		return 0;
	}
	//判断文件类型
	if (S_ISDIR(st.st_mode))//man文档中有关于该宏函数的使用说明
	{
		// 把这个目录中的内容发送给客户端
		//sendHeadMsg(cfd, 200, "OK", getFileType(".html"),-1);
		sendDir(file, decodepath,cfd);
	}
	else
	{
		// 把文件的内容发送给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(file),st.st_size);
		sendFile(file, cfd);
	}
	return 0;
}

const char* getFileType(const char* fileName)
{
	// a.jpg a.mp4 a.html
	//自左向右查找 '.'字符，如不存在返回NULL
	const char* dot = strrchr(fileName, '.');
	if (dot == NULL)
		return "text/plain; charset=utf-8";//纯文本
	if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
		return "text/html; charset=utf-8";
	if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(dot, ".gif") == 0)
		return "image/gif";
	if (strcmp(dot, ".png") == 0)
		return "image/png";
	if (strcmp(dot, ".css") == 0)
		return "text/css";
	if (strcmp(dot, ".au") == 0)
		return "audio/basic";
	if (strcmp(dot, ".wav") == 0)
		return "audio/wav";
	if (strcmp(dot, ".avi") == 0)
		return "video/x-msvideo";
	if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
		return "video/quicktime";
	if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
		return "video/mpeg";
	if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
		return "model/vrml";
	if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
		return "audio/midi";
	if (strcmp(dot, ".mp3") == 0)
		return "audio/mpeg";
	if (strcmp(dot, ".ogg") == 0)
		return "application/ogg";
	if (strcmp(dot, ".pac") == 0)
		return "application/x-ns-proxy-autoconfig";
	if (strcmp(dot, ".xmind") == 0)
		return "application/vnd.xmind.workbook";
	if (strcmp(dot, ".zip") == 0)
		return "application/zip";
	if (strcmp(dot, ".pdf") == 0)
		return "application/pdf";

	return "text/plain;charset=utf-8"; //默认返回纯文本类型
}

int sendDir(const char* dirName,const char* urlPath, int cfd)
{
	char *buf=malloc(16384);
	sprintf(buf, "<html><head><title>reactor高并发服务器</title></head><body><table>");
	struct dirent** namelist;
	int num=scandir(dirName, &namelist, NULL, alphasort);
	for (int i = 0; i < num; ++i)
	{
		//取出文件名
		char* name = namelist[i]->d_name;
		printf("文件名为name=%s\n", name);

		//跳过"."和".."
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
			free(namelist[i]);
			continue;
		}

		struct stat st;
		char subPath[1024] = { 0 };
		sprintf(subPath, "%s/%s", dirName, name);
		if (stat(subPath, &st) == -1)
		{
			perror("stat filed");
			free(namelist[i]);
			continue;
		}
		if (S_ISDIR(st.st_mode)) 
		{
			//a标签<a href="">name</a>
			sprintf(buf + strlen(buf), 
				"<tr><td><a href=\"%s%s%s/\">%s</a></td><td>%ld</td></tr>",
				urlPath,
				(urlPath[strlen(urlPath) - 1] == '/' ? "" : "/"),//确保只有一个"/"
				name,name,st.st_size);
		}
		else 
		{
			sprintf(buf + strlen(buf),
				"<tr><td><a href=\"%s%s%s\">%s</a></td><td>%ld</td></tr>",
				urlPath,
				(urlPath[strlen(urlPath) - 1] == '/' ? "" : "/"),//确保只有一个"/"
				name, name, st.st_size);
		}
		free(namelist[i]);
	}
	free(namelist);
	strcat(buf, "</table></body></html>");

	//计算长度并先发响应头
	int contentLen = strlen(buf);
	sendHeadMsg(cfd, 200, "OK", getFileType(".html"), contentLen);

	send(cfd, buf, contentLen, 0);
	free(buf);
	
	return 0;
}

int sendFile(const char* fileName, int cfd)
{
	//1、打开文件
	int fd = open(fileName, O_RDONLY);
	if (fd <= 0)
	{
		perror("open failed");//输出具体错误
		sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);
		sendFile("404.html", cfd);//确保存在404.html
		return -1;//返回错误码
	}
	struct stat st;
	if (fstat(fd, &st) == -1)
	{
		perror("fstat error");
		close(fd);
		return -1;
	}
	off_t offset = 0;
	ssize_t sent = 0;
	while (offset < st.st_size) 
	{
		sent = sendfile(cfd, fd, &offset, st.st_size - offset);
		if (sent <= 0) {
			if (errno == EAGAIN || errno == EINTR) 
			{
				continue;  // 重试
			}
			perror("sendfile error");
			break;
		}
	}
	
	close(fd);
	return 0;
}

int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int len)
{
	//状态行
	char buf[4096] = { 0 }; 
	sprintf(buf, "HTTP/1.1 %d %s\r\n", status, descr);
	//响应头(根据文件类型动态设置)
	sprintf(buf + strlen(buf), "Content-Type:%s\r\n", type);

	// //在sendHeadMsg中强制设置HTML类型
	//sprintf(buf + strlen(buf), "Content-Type: text/html; charset=utf-8\r\n");
	if (len >= 0)
	{
		sprintf(buf + strlen(buf), "Content-Length:%d\r\n", len);
	}
	sprintf(buf + strlen(buf), "Connection: close\r\n\r\n");  // 确保有结束标记
	send(cfd, buf, strlen(buf), 0);
	return 0;
}

