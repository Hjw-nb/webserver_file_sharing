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
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>
#include <cstring>  
#include <cstdio>  
#include <cstdlib>  
#include <dirent.h>  
#include <vector>  
#include "../jsoncpp/include/json/json.h" // 需要安装jsoncpp库来处理JSON  
  
#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;  //设置读取文件的名称m_real_file大小
    static const int READ_BUFFER_SIZE = 65535;  //设置读缓冲区m_read_buf大小
    static const int WRITE_BUFFER_SIZE = 65535;  //设置写缓冲区m_write_buf大小


    //报文的请求方法，本项目只用到GET和POST
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };


    //主状态机的状态
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0, //当前正在分析请求行
        CHECK_STATE_HEADER,          //当前正在分析头部字段
        CHECK_STATE_CONTENT          //当前正在解析请求体
    };


     //报文解析的结果
    enum HTTP_CODE
    {
        NO_REQUEST,                  //请求不完整，需要继续读取客户数据
        GET_REQUEST,                 //表示获得了一个完成的客户请求
        BAD_REQUEST,                 //表示客户请求语法错误
        NO_RESOURCE,                 //表示服务器没有资源
        FORBIDDEN_REQUEST,           //表示客户对资源没有足够的访问权限
        FILE_REQUEST,                //文件请求,获取文件成功
        INTERNAL_ERROR,              //表示服务器内部错误
        CLOSED_CONNECTION            //表示客户端已经关闭连接了
    };


    //从状态机的状态
    enum LINE_STATUS
    {
        LINE_OK = 0,                 //读取到一个完整的行
        LINE_BAD,                    //行出错
        LINE_OPEN                    //行数据尚且不完整
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    void close_conn(bool real_close = true);
    void process();
    bool read_once();//读取浏览器端发来的全部数据
    bool write();//响应报文写入函数
    sockaddr_in *get_address()
    {
        return &m_address;
    }


    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;


private:
    void init();
    HTTP_CODE process_read();
    bool process_write(HTTP_CODE ret);
    HTTP_CODE parse_request_line(char *text);
    HTTP_CODE parse_headers(char *text);
    HTTP_CODE parse_content(char *text);
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_type_json();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    void update_file_save(const char *requestBody);
    void saveFileContent(const char *filePath, const char *contentStart, const char *contentEnd);
    const char* findFilename(const char *requestBody);
    void file_response_to_json(string dir_name);
    void saveFile(const char *filename, const char *data, size_t length);
    void all_file_zip(const char *dir_name,const char *zip_name);
public:
    static int m_epollfd;
    static int m_user_count;
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx; //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    long m_checked_idx;
    int m_start_line;
    char m_write_buf[WRITE_BUFFER_SIZE];
    int m_write_idx;
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];
    char *m_url;
    char *m_version;
    char *m_host;
    long m_content_length;
    char *m_content_type;
    char *m_boundary;
    bool m_linger;
    char *m_file_address;
    struct stat m_file_stat;
    struct iovec m_iv[2];
    int m_iv_count;
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    string json_temporary_response;
    const char *m_json_response; //用来存储要传输的jsonshuju 
    int bytes_to_send;
    int bytes_have_send;
    char *doc_root;
    bool download_flag;//当浏览器发出下载请求的时候，这个flag为真

    
    map<string, string> m_users;
    int m_TRIGMode;
    int m_close_log;

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif
