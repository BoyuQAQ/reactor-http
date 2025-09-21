
#include "Server.h"
#include <stdio.h>
#include <sys/epoll.h>

//FSM状态设计
enum HttpState 
{
	STATE_REQUEST_LINE,//解析请求行
	STATE_HEADER,//解析请求头
	STATE_BODY,//解析请求体(post/put才有)
	STATE_DONE,//完成
	STATE_ERROR//出错
};

//HTTP解析上下文
struct HttpRequest
{
	enum HttpState state;	//当前解析状态
	char method[12];
	char url[1024];
	char version[16];
	char headers[4096];	//请求头缓冲区
	int content_length; 
	char body[8192];	//请求体缓冲区
	int body_received;
	int keep_alive;
};

struct FdInfo//将epoll的根结点和用于监听的文件描述符统合成一个结构体用于，给线程中的acceptClient函数作返回值
{
	int fd;
	int epfd;
	pthread_t tid;
	struct HttpRequest req;//HTTP解析状态机
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

	struct FdInfo* linfo = malloc(sizeof(struct FdInfo));
	linfo->fd = lfd;
	linfo->epfd = epfd;
	linfo->tid = 0;
	//2.lfd放红黑树中
	struct epoll_event ev = { 0 };
	ev.events = EPOLLIN; //可读事件
	ev.data.ptr = linfo; //监听套接字
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
		for (int i = 0; i < num; ++i) 
		{
			//struct FdInfo* info = (struct FdInfo*)malloc(sizeof(struct FdInfo));
			struct FdInfo* info = (struct FdInfo*)evs[i].data.ptr;
			/*int fd = evs[i].data.fd;
			info->epfd = epfd;
			info->fd = fd;*/
			if (info->fd == lfd)
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

int httpd_parse(struct FdInfo* info, const char* buf, int len)
{
	struct HttpRequest* req = &info->req;
	int i = 0;

	while (i < len)
	{
		switch (req->state)
		{
		case STATE_REQUEST_LINE:
		{
			char* line_end = strstr(buf + i, "\r\n");//在HTTP请求数据中查找行终止符\r\n
			if (!line_end)return 0;//请求行没收完，等下一次

			char line[1024];
			int line_len = line_end - (buf + i);//计算HTTP中某一行的实际长度(例如请求行或请求头)
			strncpy(line, buf + i, line_len);//从HTTP请求数据中安全复制一行内容到本地缓冲区
			line[line_len] = '\0';

			sscanf(line, "%s %s %s", req->method, req->url, req->version);
			printf("FSM解析请求行:%s %s %s\n", req->method, req->url, req->version);

			req->state = STATE_HEADER;
			i += line_len + 2;//跳过\r\n
			break;
		}
		case STATE_HEADER:
		{
			printf("进入HEADER头部\n");
			char* line_end = strstr(buf + i, "\r\n");//从当前缓冲区位置 buf + i开始查找 \r\n（HTTP 头部每行以 \r\n结尾）
			if (!line_end)return 0;

			if (line_end == buf + i)//检查空行，表示头部结束
			{
				printf("判断进入BODY或者DONE,content_length:%d\n",req->content_length);
				req->state = (req->content_length > 0) ? STATE_BODY : STATE_DONE;
				i += 2;	

				if (req->state == STATE_DONE)
				{
					printf("FSM解析完成，空行后直接完成\n");
					return 1;
				}
				break;
			}
			
			char header_line[1024];
			int line_len = line_end - (buf + i);
			strncpy(header_line, buf + i, line_len);
			header_line[line_len] = '\0';
			printf("Header: %s\n", header_line);
			printf("FSM解析请求头:%s\n", header_line);

			if (strncasecmp(header_line, "Content-Length:", 15) == 0)
			{
				req->content_length = atoi(header_line + 15);
			}
			else if(strncasecmp(header_line,"Connection:",11)==0)
			{
				if (strcasestr(header_line, "keep-alive"))
				{
					req->keep_alive = 1;
				}
				else
				{
					req->keep_alive = 0;
				}
			}

			strcat(req->headers, header_line);
			strcat(req->headers, "\n");

			i += line_len + 2;
			break;
		}
		case STATE_BODY:
		{
			printf("进入BODY\n");
			fflush(stdout);
			int remain = len - i;
			int need = req->content_length - req->body_received;
			int copy_len = (remain < need) ? remain : need;

			memcpy(req->body + req->body_received, buf + i, copy_len);
			req->body_received += copy_len;
			i += copy_len;

			if (req->body_received >= req->content_length)
			{
				req->state = STATE_DONE;
			}
			break;
		}
		case STATE_DONE:
		{
			printf("FSM解析完成\n");
			return 1;//解析完成
		}			
		case STATE_ERROR:					
			return -1;
			printf("FSM报错");
		}
	}
	
	if (req->state == STATE_DONE)
	{
		printf("FSM解析完成\n");
		return 1;//解析完成
	}
	printf("FSM 状态: %d, 本次读了: %d 字节\n", req->state,len);
	return 0;//数据还不够，继续等
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

	//初始化,为新客户分配的FdInfo
	struct FdInfo* cinfo = malloc(sizeof(struct FdInfo));
	if (!cinfo)
	{
		perror("malloc");
		close(cfd);
		return NULL;
	}
	memset(cinfo, 0, sizeof(struct FdInfo));
	cinfo->fd = cfd;
	cinfo->epfd = info->epfd;	//继承epoll fd
	cinfo->req.state = STATE_REQUEST_LINE;

	//3、将cfd放入红黑树中
	struct epoll_event ev;
	ev.data.ptr = cinfo;//将cfd放入到红黑树中
	ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;//把EPOLLET作为事件添加到events这个整形变量里边去 
	int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1)
	{
		perror("epoll_ctl");
		close(cfd);//如果添加失败，关闭连接
		free(cinfo);
		return NULL;
	}
	printf("acceptClient threadID:%ld\n", info->tid);
	return NULL;
}

void* recvHttpRequest(void* arg)
{
	struct FdInfo* info = (struct FdInfo*)arg;
	char buf[8192] = { 0 };
	int len;
	int parse_ret = 0; //0=未完成	1=完成	-1=错误

	while (1)
	{
		len = (recv(info->fd, buf, sizeof(buf), 0));
		if (len == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)	break;
			else {
				perror("recv error");
				parse_ret = -1;
				break;
			}		
		}
		else if (len == 0)
		{	//对端关闭连接
			parse_ret = -1;
			//关闭客户端连接
			epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
			close(info->fd);
			free(info);
			break;
		}
		else {
			int ret = httpd_parse(info, buf, len);
			if (ret == 1)
			{
				parse_ret = 1;
				break;
			}
			else if (ret == -1)
			{
				parse_ret = -1;
				break;
			}
			//ret==0 继续接收
			printf("[DEBUG] httpd_parse 返回 ret=%d, len=%d, state=%d\n", ret, len, info->req.state);
		}
	}
		printf("准备解析并调用业务逻辑[Thread %ld]\n",pthread_self());		
		fflush(stdout);
		if (parse_ret == 1)//解析完成，调用业务逻辑
		{
			printf("开始解析并调用业务逻辑[Thread %ld]\n", pthread_self());
			fflush(stdout);
			parseReuqestLine(info->req.url, info->fd);

			int keep_alive = 0;
			if (strcasestr(info->req.headers, "Connection:keep-alive"))
				keep_alive = 1;

			if (keep_alive)
			{//重置FSM，等待下一次请求
				memset(&info->req, 0, sizeof(info->req));
				info->req.state = STATE_REQUEST_LINE;

				//如果还需要保持连接，重新激活ONESHOT
				struct epoll_event ev;
				ev.data.ptr = info;
				ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
				epoll_ctl(info->epfd, EPOLL_CTL_MOD, info->fd, &ev);

				return NULL;//不free(info),保持连接
			}
			else
			{
				//客户端要求关闭
				epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
				close(info->fd);
				free(info);
				return NULL;
			}
		}
		
		else if (parse_ret == -1)
		{
			printf("FSM解析失败\n");
			sendHeadMsg(info->fd, 404, "Bad Request", "text/html", -1);
			sendFile("404.html", info->fd);
			epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
			close(info->fd);
			free(info);
		}
		else //parse_ret=0,表示需要更多数据，重新激活事件
		{
			struct epoll_event ev;
			ev.data.ptr = info;
			ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
			epoll_ctl(info->epfd, EPOLL_CTL_MOD, info->fd, &ev);
		}
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
	char decodepath[1024] = { 0 };//存储解码后的路径(如华为)
	
	//URL 解码
	urldecode(decodepath, line);	
	printf("解码后路径:%s\n", decodepath);

	//构建完整路径
	char fullpath[PATH_MAX] = { 0 };
	if (strcmp(decodepath, "/") == 0) {
		snprintf(fullpath, sizeof(fullpath), "%s", base_dir);
	}
	else {
		//直接拼接base_dir，和解码后的路径
		snprintf(fullpath, sizeof(fullpath), "%s%s", base_dir, decodepath);
	}

	//使用realpath规范化路径
	char resolved_path[PATH_MAX];
	if (realpath(fullpath, resolved_path) == NULL)
	{
		perror("realpath error");
		sendErrorResponse(cfd, 404, "Not Found");
		return -1;
	}

	//检查是否在允许的目录下
	size_t base_len = strlen(base_dir);
	if (strncmp(resolved_path, base_dir, strlen(base_dir)) != 0)
	{//检查是否resolved_path正好是base_dir without trailing slash
		if (base_len > 0 && 
			base_dir[base_len - 1] == '/' && 
			strlen(resolved_path) == base_len - 1 &&
			strncmp(resolved_path, base_dir, base_len - 1) == 0) {
			//允许访问
		}
		else
		{
			fprintf(stderr, "Path traversal attempt:%s (base_dir:%s)\n", resolved_path, base_dir);
			sendErrorResponse(cfd, 403, "Forbidden");
			return -1;
		}
		
	}

	//获取文件属性
	struct stat st;
	int ret = stat(fullpath, &st);
	if (ret == -1)
	{
		perror("stat failed");
		
		//尝试发送404页面
		char not_found_path[PATH_MAX];
		snprintf(not_found_path, sizeof(not_found_path), "%s/404.html", base_dir);
		
		if (access(not_found_path, R_OK) == 0)
		{
			sendHeadMsg(cfd, 404, "Not Found", getFileType(not_found_path), -1);
			sendFile(not_found_path, cfd);
		}
		else {
			sendErrorResponse(cfd, 404, "Not Found");
		}
		
		return 0;
	}
	//判断文件类型
	if (S_ISDIR(st.st_mode))//man文档中有关于该宏函数的使用说明
	{
		sendDir(resolved_path, decodepath,cfd);
	}
	else
	{
		// 把文件的内容发送给客户端
		sendHeadMsg(cfd, 200, "OK", getFileType(resolved_path),st.st_size);
		sendFile(resolved_path, cfd);
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
		//尝试发送404页面，但避免递归
		char not_found_path[PATH_MAX];
		snprintf(not_found_path, sizeof(not_found_path), "%s/404.html", base_dir);
		int fd_404 = open(not_found_path, O_RDONLY);
		if (fd_404 > 0)
		{
			struct stat st;
			if (fstat(fd_404, &st) == 0) {
				sendHeadMsg(cfd, 404, "Not Found", getFileType(not_found_path), st.st_size);
				off_t offset = 0;
				sendfile(cfd, fd_404, &offset, st.st_size);
			}
			close(fd_404);
		}
		else {
			//如果404页面也不存在，发送简单的错误响应
			sendErrorResponse(cfd, 404, "Not Found");
		}
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
		printf("[DEBUG] send file %ld/%ld bytes\n", sent, st.st_size);
		fflush(stdout);
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
	sprintf(buf + strlen(buf), "Connection:close\r\n\r\n");
	int n=send(cfd, buf, strlen(buf), 0);
	printf("[DEBUG] send head %d bytes: %s\n", n, buf);
	fflush(stdout);
	return 0;
}

void sendErrorResponse(int cfd, int status, const char* description) {
	char body[1024];
	sprintf(body, "<html><body><h1>%d %s</h1></body></html>", status, description);

	char head[512];
	sprintf(head, "HTTP/1.1 %d %s\r\n"
		"Content-Type: text/html\r\n"
		"Content-Length: %zu\r\n"
		"Connection: close\r\n\r\n",
		status, description, strlen(body));

	send(cfd, head, strlen(head), 0);
	send(cfd, body, strlen(body), 0);
}