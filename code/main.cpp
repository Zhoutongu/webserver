#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "threadpool.h"
#include "locker.h"
#include "http_conn.h"
#include "lst_timer.h"

#define MAX_FD 65536           // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 // 同时监听的最大事件数量
#define TIME_SLOT 5            // 定时器触发时间片

static int pipefd[2];
static int epollfd = 0;
static sort_timer_lst timer_lst;

// 添加信号捕捉
void addsig(int sig, void(handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

void sig_handler(int sig)
{
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char *)msg, 1, 0)
        errno = save_errno;
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭
void call_back(client_data *user_data)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    close(user_data->sockfd);
    http_conn::m_user_count--;
    printf("close fd %d\n", user_data->sockfd);
}

void timer_handler()
{
    // 定时处理任务，实际就是调用tick()函数
    timer_lst.tick();
    // 一次arlarm调用只会引起一次SIGALARM信号，所以重新设闹钟
    alarm(TIME_SLOT);
}

// 添加文件描述符到epoll当中 http_conn.cpp里实现
extern void addfd(int epollfd, int fd, bool one_shot);

// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);

// 修改文件描述符，充值socket EPOLLONESHOT 和 EPOLLRDHUP 事件，确保下一次可读时 EPOLLIN 事件被触发
extern void modfd(int epollfd, int fd, int ev);

extern void setnonblocking(int fd);

int main(int argc, char *argv[])
{
    if (argc <= 1)
    {
        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));
        return 1;
    }

    // 获取端口号
    int port = atoi(argv[1]);

    // 对SIGPIE信号做处理,SIG_IGN忽略信号
    addsig(SIGPIPE, SIG_IGN);

    // 对SIGALRM、SIGTERM设置信号处理函数
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);

    // 创建线程池，初始化线程池
    threadpool<http_conn> *pool = nullptr;
    try
    {
        pool = new threadpool<http_conn>;
    }
    catch (...)
    {
        printf("error\n");
        exit(-1);
    }

    // 创建一个数组用于保存所有的用户客户端信息
    http_conn *users = new http_conn[MAX_FD];

    // 创建监听套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);

    // 设置端口复用，需要在绑定前设置
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_port = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr *)&address, sizeof(address));

    // 监听
    listen(listenfd, 5);

    // 创建epoll对象，事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    epollfd = epoll_create(5); // 参数会被忽略，>0即可

    // 将监听的文件描述符放到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    // 创建管道
    int ret = sockerpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], EPOLLIN | EPOLLET);

    bool stop_server = false;
    bool timeout = false;
    client_data *client_users = new client_data[MAX_FD];
    alarm(TIME_SLOT); // 定时,5秒后产生SIGALARM信号

    while (!stop_server)
    {
        // 如果成功，返回请求的I/O准备就绪的文件描述符的数目
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((num < 0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; i++)
        {
            int sockfd = events[i].data.fd;
            if (sockfd == listenfd)
            {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);

                if (http_conn::m_user_count >= MAX_FD)
                {
                    // 目前的连接数满了
                    // 这里可以告诉客户端服务器内部正忙
                    close(connfd);
                    continue;
                }

                // 将新的客户端的数据初始化放到数组中
                users[connfd].init(connfd, client_address);

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer *timer = new util_timer;
                timer->call_back = call_back;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIME_SLOT;

                client_users[connfd].sockfd = connfd;
                client_users[connfd].timer = timer;
                client_users[connfd].address = client_address;

                timer->user_data = &client_users[connfd];
                timer_lst.add_timer(timer);
            }
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN))
            {
                // 处理信号
                int sig;
                char signals[1024];
                int ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1)
                    continue;
                else if (ret == 0)
                    continue;
                else
                {
                    for (int i = 0; i < ret; i++)
                    {
                        switch (signals[i])
                        {
                        case SIGALRM:
                        {
                            // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                            // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                            timeout = true;
                            break;
                        }
                        case SIGTERM:
                        {
                            stop_server = true;
                        }
                        }
                    }
                }
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方异常断开，关闭链接
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                util_timer *timer = client_users[sockfd].timer;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIME_SLOT;
                timer_lst.add_timer(timer);

                // 有读事件发生
                if (users[sockfd].read())
                {
                    // 一次性把数据全部读完
                    pool->append(users + sockfd);
                }
                else
                {
                    // 没读到数据或者关闭了
                    timer_lst.del_timer(timer);
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                // 有写事件发生
                if (!users[sockfd].write())
                {
                    // 写失败了
                    users[sockfd].close_conn();
                }
            }
        }

        // 最后处理定时事件，因为I/O事件有更高的优先级。
        // 当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if (timeout)
        {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;

    return 0;
}