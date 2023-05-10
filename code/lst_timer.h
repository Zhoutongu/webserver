#ifndef LST_TIMER
#define LST_TIMER

#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 64
class util_timer; // 前向声明

// 用户数据结构
struct client_data
{
    sockaddr_in address; // 客户端socket地址
    int sockfd;          // socket文件描述符
    util_timer timer;    // 定时器
};

// 定时器类
class util_timer
{
public:
    util_timer() : pre(nullptr), next(nullptr) {}
    // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    void (*call_back)(client_data *); 

public:
    time_t expire; // 任务超时时间，这里使用绝对时间
    client_data *user_data;
    util_timer *pre;  // 指向前一个定时器
    util_timer *next; // 指向后一个定时器
};

// 定时器链表，它是一个升序、双向链表，且带有头节点和尾节点
class sort_timer_lst
{
public:
    sort_timer_lst() : head(nullptr), tail(nullptr) {}
    // 链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst()
    {
        util_timer *temp = head;
        while (temp)
        {
            head = temp->next;
            delete temp;
            temp = head;
        }
    }

public:
    void add_timer(util_timer *timer);    // 将目标定时器timer添加到链表中
    void adjust_timer(util_timer *timer); // 调整对应的定时器在链表中的位置
    void del_timer(util_timer *timer);    // 将目标定时器 timer 从链表中删除
    void tick();                          // 处理链表上到期任务

private:
    void add_timer(util_timer *timer, util_timer *lst_head);

private:
    util_timer *head; // 头结点
    util_timer *tail; // 尾结点
}

#endif