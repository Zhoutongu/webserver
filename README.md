# webserver
## 项目介绍

Linux下C++轻量级Web服务器

- 通过构建线程池完成多个逻辑处理任务的并⾏处理，利⽤同步I/O实现了模拟Proactor模型来处理客⼾端的请求；
- 利用I/O多路复⽤技术Epoll，提⾼服务器处理浏览器连接请求的效率，并采⽤了有限状态机解析HTTP请求报⽂；
- 利用升序的双向链表实现了定时器，清除不活跃的连接减少⾼并发场景下不必要的系统资源的占⽤；
- 使⽤WebBench对服务器进⾏了性能测试和压⼒测试，确保服务器的稳定性和可靠性。

主要参考了牛客的webserver项目视频和游双的《Linux高性能服务器编程》

同时参考了tinywebserver这个项目https://github.com/qinguoyi/TinyWebServer
