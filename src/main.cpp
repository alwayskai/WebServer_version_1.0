#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // （一次）监听的最大的事件数量
#define TIMEOUT 5

static int pipefd[2];
static time_heap min_heap(1024);

extern int setnonblocking( int fd );

void sig_handler( int sig ) {
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void timer_handler() {
    min_heap.tick();
	/*获得堆顶的定时器，以它的定时时间设置alarm*/
	if(!min_heap.empty())
	{
		heap_timer* temp = min_heap.top();// 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
		alarm(temp->expire-time(NULL));//重新定时，以不断触发sigalarm信号
	}
}

// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭连接。
void cb_func( http_conn* user_data ) {
    user_data->close_conn();
}

extern void addfd( int epollfd, int fd, bool one_shot );

extern void removefd( int epollfd, int fd );

//专门做信号处理的函数
void addsig(int sig, void( handler )(int)) {
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] ) {
    
    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }

    int port = atoi( argv[1] );
    addsig( SIGPIPE, SIG_IGN );

    // 创建线程池，初始化线程池
    threadpool< http_conn >* pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    // 创建一个数组用于保存所有的客户端信息
    http_conn* users = new http_conn[ MAX_FD ];

    // 创建监听的套接字
    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );

    // 监听的套接字地址
    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    // 端口复用
    int reuse = 1;
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );

    // 创建epoll对象，和事件数组
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );

    // 将监听的文件描述符添加到epoll对象中
    addfd( epollfd, listenfd, false );
    http_conn::m_epollfd = epollfd;

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0], false );

    // 设置信号处理函数
    addsig( SIGALRM, sig_handler );
    addsig( SIGTERM, sig_handler );
 
    bool stop_server = false;
    bool timeout = false;

    while( !stop_server ) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        
        if ( ( number < 0 ) && ( errno != EINTR ) ) {
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            //有客户端连接进来了
            if( sockfd == listenfd ) {
                
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 

                if( http_conn::m_user_count >= MAX_FD ) {
                    //给客户端写一个信息，服务器内部正忙。
                    close(connfd);
                    continue;
                }
                
                users[connfd].init( connfd, client_address);

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到堆中
                heap_timer* timer = new heap_timer(2*TIMEOUT);
				timer->cb_func = cb_func;
				timer->user_data = &users[connfd];
				users[connfd].timer = timer;

				/*若该定时器为时间堆中第一个定时器，则设置alarm*/
				if(min_heap.empty()) {
					alarm(2*TIMEOUT);
				}

				min_heap.add_timer(timer);// 放到定时器堆中

            } else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {// 监听到管道里面有数据

                // 处理信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );

                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else {
                    for( int i = 0; i < ret; ++i ) {// 遍历信号
                        switch( signals[i] ) {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;// 先不处理了，先处理I/O的任务
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                                break;
                            }
                        }
                    }
                }
            } else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {

                heap_timer* timer = users[sockfd].timer;
                // 对方客户端异常断开或者错误等事件

                users[sockfd].close_conn();

                if( timer )
                {
                    min_heap.del_timer( timer );//移除其对应的定时器
                }

            } else if(events[i].events & EPOLLIN) {

                heap_timer* timer = users[sockfd].timer;

                if(users[sockfd].read()) {

                    // 如果某个客户端有数据可读,则调整该连接的定时器，以延迟该连接关闭的事件
					time_t timeout = time(NULL) + (2*TIMEOUT);
					printf("adjust timer once\n");
					min_heap.adjust_timer(users[sockfd].timer,timeout);

                    pool->append(users + sockfd);

                } else {

                    users[sockfd].close_conn();

                    if( timer )
                    {
                        min_heap.del_timer( timer );
                    }

                }

            }  else if( events[i].events & EPOLLOUT ) {

                heap_timer* timer = users[sockfd].timer;
                
                if( !users[sockfd].write() ) {

                    users[sockfd].close_conn();

                    if( timer )
                    {
                        min_heap.del_timer( timer );
                    }

                } else {

                    // 如果某个客户端上有数据可写，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
					time_t timeout = time(NULL) + (2*TIMEOUT);
					printf("adjust timer once\n");
					min_heap.adjust_timer(users[sockfd].timer,timeout);

                }
            }
        }

        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }
    
    close( epollfd );
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;
    return 0;
}