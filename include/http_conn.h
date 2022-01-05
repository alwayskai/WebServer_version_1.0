#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>
#include <time.h>
using std::exception;

class heap_timer;// 前向声明

class http_conn
{
public:
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区的大小
    
    // HTTP请求方法，这里只支持GET。enum是计算机编程语言中的一种数据类型。枚举类型：在实际问题中，有些变量的取值被限定在一个有限的范围内。
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    ///这玩意；也就相当于定义了HTTP_CODE类型

    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
public:
    http_conn(){}
    ~http_conn(){}
public:
    void init(int sockfd, const sockaddr_in& addr); // 初始化新接受的连接
    void close_conn();                              // 关闭连接
    void process();                                 // 处理客户端请求
    bool read();                                    // 非阻塞读
    bool write();                                   // 非阻塞写
private:
    void init();                                    // 初始化连接
    HTTP_CODE process_read();                       // 解析HTTP请求
    bool process_write( HTTP_CODE ret );            // 填充HTTP应答

    // 下面这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_request_line( char* text );
    HTTP_CODE parse_headers( char* text );
    HTTP_CODE parse_content( char* text );
    HTTP_CODE do_request();
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();

    // 这一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;    // 统计用户的数量
    int m_sockfd;               // 该HTTP连接的socket和对方的socket地址
    heap_timer* timer;          // 定时器

private:
    
    sockaddr_in m_address;
    
    char m_read_buf[ READ_BUFFER_SIZE ];    // 读缓冲区
    int m_read_idx;                         // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    int m_checked_idx;                      // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                       // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;              // 主状态机当前所处的状态
    METHOD m_method;                        // 请求方法

    char m_real_file[ FILENAME_LEN ];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char* m_url;                            // 客户请求的目标文件的文件名
    char* m_version;                        // HTTP协议版本号，我们仅支持HTTP1.1
    char* m_host;                           // 主机名
    int m_content_length;                   // HTTP请求的消息总长度
    bool m_linger;                          // HTTP请求是否要求保持连接

    char m_write_buf[ WRITE_BUFFER_SIZE ];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    int bytes_to_send;                      // 将要发送的数据的字节数
    int bytes_have_send;                    // 已经发送的字节数
};


// 定时器类
class heap_timer {
public:
    heap_timer( int delay ) {
        expire = time( NULL ) + delay;
    }

public:
   time_t expire;// 定时器生效的绝对时间，任务超时时间
   void (*cb_func)( http_conn* );
   http_conn* user_data;
};

// 时间堆类
class time_heap {
public:
    // 构造函数之一，初始化为一个大小为cap的空堆
    time_heap( int cap ) throw ( std::exception ) : capacity( cap ), cur_size( 0 ) {
	    array = new heap_timer* [capacity];// 创建堆数组
	    if ( ! array ) {
            throw std::exception();
	    }
        for( int i = 0; i < capacity; ++i ) {
            array[i] = NULL;
        }
    }
    // 构造函数之二，用已有的数组来初始化堆
    time_heap( heap_timer** init_array, int size, int capacity ) throw ( std::exception )
        : cur_size( size ), capacity( capacity ) {
        if ( capacity < size ) {
            throw std::exception();
        }
        array = new heap_timer* [capacity];// 创建堆数组
        if ( ! array ) {
            throw std::exception();
        }
        for( int i = 0; i < capacity; ++i ) {
            array[i] = NULL;
        }
        if ( size != 0 ) {
            // 初始化堆数组
            for ( int i =  0; i < size; ++i ) {
                array[ i ] = init_array[ i ];
            }
            for ( int i = (cur_size-1)/2; i >=0; --i ) {
                // 对数组中的第[(cur_size-1)/2]~0个元素执行下虑操作
                percolate_down( i );
            }
        }
    }
    ~time_heap() {
        for ( int i =  0; i < cur_size; ++i ) {
            delete array[i];
        }
        delete [] array; 
    }

public:
    // 添加目标定时器timer
    void add_timer( heap_timer* timer ) throw ( std::exception ) {
        if( !timer ) {
            return;
        }
        if( cur_size >= capacity ) {
            resize();// 如果当前堆数组容量不够，则将其扩大1倍
        }
        // 新插入了一个元素，当前堆大小加1，hole是新建空穴的位置
        int hole = cur_size++;
        int parent = 0;
        // 对从空穴到根节点路径上的所有节点执行上虑操作
        for( ; hole > 0; hole=parent ) {
            parent = (hole-1)/2;
            if ( array[parent]->expire <= timer->expire ) {
                break;
            }
            array[hole] = array[parent];
        }
        array[hole] = timer;
    }
    // 删除目标定时器tmer
    void del_timer( heap_timer* timer ) {
        if( !timer ) {
            return;
        }
        /*lazy delelte:仅仅将目标定时器的回调函数设置为空，
        即所谓的延迟销毁。这将节省真正删除该定时器造成的开销，
        但这样做容易使堆数组膨胀*/
        timer->cb_func = NULL;
    }
    /*获得堆顶部的定时器*/
    heap_timer* top() const {
        if ( empty() ) {
            return NULL;
        }
        return array[0];
    }
    /*删除堆顶部的定时器*/
    void pop_timer() {
        if( empty() ) {
            return;
        }
        if( array[0] ) {
            delete array[0];
            /*将原来的堆顶元素替换为堆数组中最后一个元素*/
            array[0] = array[--cur_size];
            percolate_down( 0 );/*对新的堆顶元素执行下虑操作*/
        }
    }
    /*调整目标定时器*/
	void adjust_timer(heap_timer* timer, time_t timeout) {
        if(timer == NULL)
            return;
        timer->expire = timeout;
        percolate_down( 0 );
	}

    /*心搏函数*/
    void tick() {
        heap_timer* tmp = array[0];
        time_t cur = time( NULL );/*循环处理堆中到期的定时器*/
        while( !empty() ) {
            if( !tmp ) {
                break;
            }
            // 如果堆顶定时器没到期，则退出循环
            if( tmp->expire > cur ) {
                break;
            }
            // 否则就执行堆顶定时器中的任务
            if( array[0]->cb_func ) {
                array[0]->cb_func( array[0]->user_data );
            }
            // 将堆顶元素删除，同时生成新的堆顶定时器(array[0])
            pop_timer();
            tmp = array[0];
        }
    }
    /*查看堆是否为空*/
    bool empty() const { 
        return cur_size == 0;
    }

private:
    /*最小堆的下虑操作，
    它确保堆数组中以第hole个节点作为根的子树拥有最小堆性质*/
    void percolate_down( int hole ) {
        heap_timer* temp = array[hole];
        int child = 0;
        for ( ; ((hole*2+1) <= (cur_size-1)); hole=child ) {
            child = hole*2+1;
            if ( (child < (cur_size-1)) && (array[child+1]->expire < array[child]->expire ) ) {
                ++child;
            }
            if ( array[child]->expire < temp->expire ) {
                array[hole] = array[child];
            }
            else {
                break;
            }
        }
        array[hole] = temp;
    }
    /*将堆数组容量扩大1倍*/
    void resize() throw ( std::exception ) {
        heap_timer** temp = new heap_timer* [2*capacity];
        for( int i = 0; i < 2*capacity; ++i ) {
            temp[i] = NULL;
        }
        if ( ! temp ) {
            throw std::exception();
        }
        capacity = 2*capacity;
        for ( int i = 0; i < cur_size; ++i ) {
            temp[i] = array[i];
        }
        delete [] array;
        array = temp;
    }

private:
    heap_timer** array;/*堆数组*/
    int capacity;/*堆数组的容量*/
    int cur_size;/*堆数组当前包含元素的个数*/
};

#endif
