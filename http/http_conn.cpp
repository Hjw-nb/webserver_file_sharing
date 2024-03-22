#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";


const char *file_save_path="./Update_File";
const char *output_zip_file = "./all_files.zip";
const char *file_all_download_path="/Update_File/all_files.zip";
locker m_lock;
map<string, string> users;

void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核时间表删除描述符
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;

    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_content_type=0;
    m_boundary=0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;
    m_json_response=0;
    download_flag=0;
    //json_response="0";
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
http_conn::LINE_STATUS http_conn::parse_line()
{
        // 增加一个状态，用于处理请求体
    // if (m_state == CHECK_STATE_CONTENT) {
    //     LOG_INFO("m_state == CHECK_STATE_CONTENT\r\n");
    //     // 直接根据Content-Length读取请求体，不再逐字节解析
    //     if (m_read_idx > m_content_length + m_checked_idx) {
    //         // 请求体已完全读取
    //         m_string = m_read_buf + m_checked_idx; // 假设m_string用于存储请求体数据
    //         m_checked_idx += m_content_length; // 更新已检查的索引
    //         return LINE_OK; // 表示请求体已完整读取
    //     } else {
    //         return LINE_OPEN; // 请求体尚未完全读取
    //     }
    // }

    // if (m_state == CHECK_STATE_CONTENT&&m_content_length>0) {
    //     LOG_INFO("m_state == CHECK_STATE_CONTENT\r\n");
    //     // 直接根据Content-Length读取请求体，不再逐字节解析
    //     if (m_read_idx < m_content_length + m_checked_idx-5) {
    //         return LINE_OPEN; // 请求体尚未完全读取
    //     } 
    // }

    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if ((m_checked_idx + 1) == m_read_idx)
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    m_url = strpbrk(text, " \t");
    if (!m_url)
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';
    char *method = text;
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if (!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }

    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        // m_url += 0;
        m_url = strchr(m_url, '/');
    }

    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "index.html");
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if (text[0] == '\0')
    {
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else if (strncasecmp(text, "Content-Type:", 13) == 0){
        text +=13;
        text += strspn(text, " \t");
        char* end =strchr(text,';');
        if(end!=nullptr){
            //return BAD_REQUEST;
            *end='\0';
            m_content_type=text;
            // *end=';';
            LOG_INFO("m_content_type:%s", m_content_type);

            text=end+1;
            text += strspn(text, " \t");
            if(strncasecmp(text, "boundary=", 9) == 0){
                text+=9;
                m_boundary=text;
                LOG_INFO("m_boundary:%s", m_boundary);

            }
        }
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}




// 用于查找文件名的辅助函数  
const char* http_conn::findFilename(const char *requestBody) {  
    const char *filenameStart = "filename=\"";  
    const char *filenameEnd = "\"";  
    const char *pos = strstr(requestBody, filenameStart);  
    if (!pos) return NULL;  
    pos += strlen(filenameStart);  
    const char *end = strchr(pos, '"');  
    if (!end) return NULL;  
    size_t filenameLength = end - pos;  
    char *filename = (char *)malloc(filenameLength + 1);  
    if (!filename) return NULL; // 内存分配失败  
    strncpy(filename, pos, filenameLength);  
    filename[filenameLength] = '\0';  
    return filename;  
}  
  
// 保存文件内容到指定路径的辅助函数  
void http_conn::saveFileContent(const char *filePath, const char *contentStart, const char *contentEnd) {  
    FILE *file = fopen(filePath, "wb"); // 以二进制模式写入文件  
    if (!file) {  
        LOG_INFO("----------------------------Failed to open the output file for writing\r\n");  
        return;  
    }  
    size_t contentLength = contentEnd - contentStart;  
    fwrite(contentStart, sizeof(char), contentLength, file); // 写入文件内容  
    fclose(file); // 关闭文件  
    LOG_INFO("-----------------------------File saved successfully to %s\n", filePath);  
}  
  
//处理上传的文件内容的函数  
// void http_conn::update_file_save(const char *requestBody) {  
//     const char *filename = findFilename(requestBody); // 提取文件名  
//     if (!filename) {  
//         LOG_INFO("Failed to find filename in the request body.\n");  
//         return;  
//     }  

//     // requestBody += strspn(requestBody, "Content-Type: text/plain");



//     const char *contentStart= strstr(requestBody, "\r\n\r\n")+4;//去除两个换行符组合
//     LOG_INFO("-----------------contentStart:%s", contentStart);
  
//     // const char *contentStart = requestBody;  

    
//     // const char *contentEnd = requestBody - strlen(m_boundary); 
//     const char *contentEnd=strstr(contentStart, m_boundary)-4;//去除两个横杆和一个换行符组合
//     // 构建完整的文件保存路径  
//     size_t pathLength = strlen(file_save_path) + strlen(filename) + 2; // +2 为了斜杠和空字符  
//     char *filePath = (char *)malloc(pathLength);

    
//     if (!filePath) {  
//         free((void *)filename); // 不要忘记释放之前分配的内存！  
//         fprintf(stderr, "Failed to allocate memory for file path.\n");  
//         return;  
//     }  
//     snprintf(filePath, pathLength, "%s/%s", file_save_path, filename); // 创建完整的文件路径  
//     free((void *)filename); // 不再需要文件名，释放内存。  
//     LOG_INFO("-----------------filePath:%s", filePath);
//     // 保存文件内容到指定路径的文件中。注意：这里的 contentStart 和 contentEnd 只是示例，并不真实。  
//     saveFileContent(filePath, contentStart, contentEnd); // 这将不会按预期工作，因为 contentStart 和 contentEnd 是硬编码的。  
//     free(filePath); // 不再需要文件路径，释放内存。  
// }  
//处理上传的文件内容的函数
void http_conn::update_file_save(const char *requestBody) {
    const char *filename = findFilename(requestBody);
    if (!filename) {
        LOG_INFO("Failed to find filename in the request body.\n");
        return;
    }

    const char *contentStart = strstr(requestBody, "\r\n\r\n") + 4;
    if (!contentStart) {
        LOG_INFO("Failed to locate the start of file content.\n");
        free((void *)filename);
        return;
    }

    // 构建结束边界标记，通常是 "--boundary--"
    std::string endBoundary = "\r\n--";
    endBoundary += m_boundary;
    endBoundary += "--";
    
    const char *contentEnd = strstr(contentStart, endBoundary.c_str());
    if (!contentEnd) {
        // 如果没有找到结束边界，直接根据Content-Length来定位结束位置
        LOG_INFO("End boundary not found, using Content-Length to locate the end of file content.\n");
        // 这里假设你有办法获取请求体的总长度m_content_length
        size_t headerLength = contentStart - requestBody; // 请求头长度
        contentEnd = requestBody + m_content_length - (strlen(m_boundary) + 6); // 减去边界和额外字符的长度
    } else {
        contentEnd -= 2; // 对于结束边界，回退到边界前的\r\n处
    }

    // 计算文件内容长度并校验
    size_t contentLength = contentEnd - contentStart;
    if (contentLength <= 0) {
        LOG_INFO("Invalid file content length.\n");
        free((void *)filename);
        return;
    }

    // 构建文件保存路径
    char filePath[FILENAME_LEN] = {0};
    snprintf(filePath, FILENAME_LEN, "%s/%s", file_save_path, filename);
    free((void *)filename);

    // 保存文件内容到指定路径
    saveFileContent(filePath, contentStart, contentEnd);
}





//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        //if(strncasecmp(m_content_type, "multipart/form-data", 19) == 0)
        if(m_content_type){
            LOG_INFO("-----------------解析上传文件\r\n");
            LOG_INFO("%s", text);
            // LOG_INFO("-----------------解析上传文件\r\n");
            update_file_save(text);
            // return FILE_REQUEST;
        }else{
            text[m_content_length] = '\0';
            //POST请求中最后为输入的用户名和密码
            m_string = text;
            //LOG_INFO("-----------------解析用户账户密码登录信息\r\n");
        }
        LOG_INFO("-----------------请求被完整读入了\r\n");
        return GET_REQUEST;
    }
    LOG_INFO("-----------------请求没有被完全读入\r\n");
    return NO_REQUEST;
}





http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();
        m_start_line = m_checked_idx;
        //LOG_INFO("主状态机数据读取：%s", text);
        if(m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK){
            // 循环打印每个字节的数据
            for (int i = 0; i < m_content_length; ++i) {
                // 打印当前字节，使用%02x来以十六进制形式打印，并且保证打印两位数（如果需要的话，前面补零）
                printf("%02x ", (unsigned char)text[i]);

                // 每16个字节换一行，以便查看
                if ((i + 1) % 16 == 0) {
                    printf("\n");
                }
            }
            printf("\n"); // 最后再换行，以便隔开后续的输出
        }
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);
            if (ret == GET_REQUEST)
                return do_request();
            line_status = LINE_OPEN;
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
void http_conn::file_response_to_json(string dir_name){
        DIR* dir;  
        struct dirent* ent;  
        struct stat file_stat;  
        std::vector<std::pair<std::string, std::string>> files_info;  
  
        if ((dir = opendir(dir_name.c_str())) != NULL) {  
            while ((ent = readdir(dir)) != NULL) {  
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {  
                    continue;  
                }  
  
                std::string full_path = dir_name + "/" + std::string(ent->d_name);
                LOG_INFO("full_path:   %s",full_path.c_str());
                if (stat(full_path.c_str(), &file_stat) == 0) {  
                    std::string file_name = ent->d_name;  
                    std::string last_modified_time = ctime(&file_stat.st_mtime);  
                    last_modified_time.pop_back();
                    files_info.push_back(std::make_pair(file_name, last_modified_time)); 
                    LOG_INFO("%s,%s",file_name.c_str(),last_modified_time.c_str()); 
                }  
            }  
            closedir(dir);  
        } else {  
            // 无法打开目录  
            LOG_INFO("--------------------------------------------无法打开目录");
            return ;
        }  


                // 构建JSON响应  
        Json::Value root;  
        for (const auto& file_info : files_info) {  
            Json::Value file_data;  
            file_data["name"] = file_info.first;  
            file_data["lastModified"] = file_info.second;  
            root.append(file_data);  
        }  
  
        Json::StreamWriterBuilder writer;  
        //m_json_response = Json::writeString(writer, root).c_str(); 
        json_temporary_response = Json::writeString(writer, root); 
        LOG_INFO("--------------------------------------------json构建完成");
        //LOG_INFO("%s",json_response.c_str());
        m_json_response=json_temporary_response.c_str();
        LOG_INFO("%s",m_json_response);
        LOG_INFO("--------------------------------------------json构建完成");
        return ; 
}
void http_conn::all_file_zip(const char* dir_name,const char* zip_name){
     // 构建完整的zip文件路径
    std::string zipPath = std::string(dir_name) + "/" + zip_name;

    std::string cmd = "rm -f ";
    cmd += zipPath;  // 删除的zip文件的完整路径
    int result = system(cmd.c_str());
        if (result != 0) {
        LOG_INFO("Failed to delete zip file.");
        return ;
    }

     cmd = "zip -r ";
    cmd += zipPath;  // 输出的zip文件的完整路径
    cmd += " ";
    cmd += dir_name; // 要打包的文件夹路径
    cmd += " -x ";
    cmd += zipPath; // 排除zip文件本身

    // 执行命令
    result = system(cmd.c_str());

    if (result != 0) {
        LOG_INFO("Failed to create zip file.");
        return ;
    }

    return ;
}
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');

    //处理cgi
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            if (users.find(name) == users.end())
            {
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if (!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/file_management.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }

    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/update_ready.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }else if (strncasecmp(p, "/Update_File", 12)==0){
        LOG_INFO("--------------------------------------------接收到了访问文件夹请求");
        // char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // strcpy(m_url_real, "/video.html");
        // strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        // free(m_url_real);
        file_response_to_json("./Update_File");
        //LOG_INFO("--------------------------------------------m_json_response");
        //LOG_INFO("%s",m_json_response);
        if(m_json_response){
            return FILE_REQUEST;
        }
        
    }else if (strncasecmp(p, "/upload", 7)==0){
        return FILE_REQUEST;
    }else if (strncasecmp(p, "/download_all_File", 18)==0){
        all_file_zip(file_save_path,output_zip_file);
        // char *m_url_real = (char *)malloc(sizeof(char) * 200);
        // strcpy(m_url_real, file_all_download_path);
        // strncpy(m_real_file + strlen(m_url_real), m_url_real, strlen(m_url_real));

        // free(m_url_real);
        download_flag=1;//发出了下载请求
        char server_path[200];
        getcwd(server_path, 200);
        strcpy(m_real_file, server_path);
        strcat(m_real_file, file_all_download_path);
        LOG_INFO("----------------download_flag=1-------------:%s",m_real_file);
    }
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    LOG_INFO("----------------m_real_file-------------:%s",m_real_file);
    // LOG_INFO("----------------m_real_file_m_file_stat-------------:%s",stat(m_real_file, &m_file_stat));
    if (stat(m_real_file, &m_file_stat) < 0){
        LOG_INFO("----------------download_flag=1-------------:%d",NO_RESOURCE);
        return NO_RESOURCE;
    }
        
    if (!(m_file_stat.st_mode & S_IROTH)){
        LOG_INFO("----------------download_flag=1-------------:%d",FORBIDDEN_REQUEST);
         return FORBIDDEN_REQUEST;
    }
       
    if (S_ISDIR(m_file_stat.st_mode)){
        LOG_INFO("----------------download_flag=1-------------:%d",BAD_REQUEST);
        return BAD_REQUEST;
    }
        

    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    LOG_INFO("----------------download_flag=1-------------:%d",FILE_REQUEST);
    return FILE_REQUEST;
}
void http_conn::unmap()
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;
    LOG_INFO("--------------------------------现在的m_linger状态是：%d\r\n",m_linger);
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }

    while (1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp < 0)
        {
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);

            if (m_linger)
            {
                init();
                LOG_INFO("--------------------------------接收到了keep-alive要求，保持连接\r\n");
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);

    LOG_INFO("response:\n%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()
{
    if(download_flag){
        LOG_INFO("----------------download_flag触发添加头-------------");
        return add_response("Content-Type:%s\r\nContent-Disposition: attachment; filename=\"all_files.zip\"\r\n", "application/octet-stream");
    }else{
        //return add_response("Content-Type:%s\r\n", "text/html");
    }
    // return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_content_type_json()
{
    return add_response("Content-Type:%s\r\n", "application/json");
}

bool http_conn::add_linger()
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        LOG_INFO("进入了200报文构造");
        add_status_line(200, ok_200_title);
        if (!m_json_response&&m_file_stat.st_size != 0)
        //if (m_file_stat.st_size != 0)
        {
            LOG_INFO("---------------------你猜错了----------------");
            add_content_type();
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            LOG_INFO("---------------------你没猜错----------------");
            if(m_json_response){
                    add_content_type_json();
                    //LOG_INFO("strlen(m_json_response):%d",strlen(m_json_response));
                     add_headers(strlen(m_json_response));
                    //LOG_INFO("%s",m_json_response);
                    m_iv[0].iov_base = m_write_buf;
                    m_iv[0].iov_len = m_write_idx;
                    m_iv[1].iov_base = const_cast<char*>(m_json_response);
                    m_iv[1].iov_len = strlen(m_json_response);
                    m_iv_count = 2;
                    bytes_to_send = m_write_idx + strlen(m_json_response);
            }else{
                char *ok_string = "<html><body>hello everyone</body></html>";
                add_headers(strlen(ok_string));
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = ok_string;
                m_iv[1].iov_len = strlen(ok_string);
                m_iv_count = 2;
                bytes_to_send = m_write_idx + strlen(ok_string);
            }



            return true;

        }
    }
    default:
        return false;
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}
