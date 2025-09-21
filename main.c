#include "Server.h"

const char* base_dir;
int main(int argc,char* argv[]) //两个命令行参数，一个是服务器的端口，一个是服务器访问的资源目录
{
	setlocale(LC_ALL, "en_US.UTF-8");//设置程序的区域信息，解决中文乱码问题
	printf("准备开始运行服务器...\n");
	const char* errorPage = "/path/to/your/404.html";//在main函数或服务器初始化时设置默认错误页面路径
	if (argc < 3)
	{
		printf("./a.out port path\n");
		return -1;
	}	
	unsigned short port = atoi(argv[1]); //获取端口号（把port转换成无符号短整型)

	
	//使用realpath规范化路径
	char resolved_base_dir[PATH_MAX];
	if (realpath(argv[2], resolved_base_dir) == NULL) {
		perror("realpath error");
		return -1;
	}

	//确保路径以斜杠结尾
	size_t len = strlen(resolved_base_dir);
	if(resolved_base_dir[len-1]!='/')
	{
		if (len + 1 < PATH_MAX) 
		{
			resolved_base_dir[len] = '/';
			resolved_base_dir[len + 1] = '\0';
		}
		else
		{
			fprintf(stderr, "Path too long\n");
			return -1;
		}
	}
	base_dir = strdup(resolved_base_dir);//动态分配内存

	//切换工作目录
	if (chdir(base_dir) != 0)
	{
		perror("chdir failed");
		return -1;
	}

	//确保404页面存在
	char not_found_path[PATH_MAX];
	snprintf(not_found_path, sizeof(not_found_path),"%s/404.html", base_dir);
	if (access(not_found_path, R_OK) != 0)
	{
		FILE* f = fopen(not_found_path, "w");
		if (f) {
			fprintf(f, "<html><body><h1>404 Not Found</h1><p>The requested resouce was not found on this server.</p></body></html>");
			fclose(f);
		}
	}
	//初始化用于监听的套接字
	int lfd = initListenFd(port);
	//启动服务器程序
	epollRun(lfd);
	
	//释放内存
	free((void*)base_dir);
	return 0;
}

