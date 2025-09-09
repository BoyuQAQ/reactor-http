#include "Server.h"




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
	//切换服务器的工作路径
	chdir(argv[2]);
	//初始化用于监听的套接字
	int lfd = initListenFd(port);
	//启动服务器程序
	epollRun(lfd);
	
	return 0;
}

