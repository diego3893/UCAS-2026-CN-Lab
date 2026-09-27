/*
 * 实验二：基于 Socket 和 OpenSSL 的 HTTP/HTTPS 文件服务器。
 *
 * 程序同时监听 80 和 443 端口。80 端口将 HTTP 请求重定向到 HTTPS；
 * 443 端口完成 TLS 握手后，根据请求路径返回文件，并支持普通传输、
 * Range 分段传输、404、持久连接及多客户端并发。
 *
 * 并发模型分为两层：两个长期线程分别监听 HTTP 和 HTTPS；每次 accept
 * 成功后再创建独立工作线程处理该客户端。这样慢连接不会阻塞其他请求。
 */

/* 网络地址、Socket、线程、文件和基础 C 库接口。 */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

/* OpenSSL 的错误处理和 TLS 接口。 */
#include "openssl/err.h"
#include "openssl/ssl.h"

/* 限制请求头最大为 64 KiB，防止异常客户端无限占用内存。 */
#define REQUEST_LIMIT (64 * 1024)
/* 文件按 16 KiB 分块读取，避免把整个文件一次性装入内存。 */
#define IO_BUFFER_SIZE 16384
/* 内核为监听 Socket 保存的待 accept 连接队列长度。 */
#define LISTEN_BACKLOG 128

/* 传给 HTTP/HTTPS 监听线程的参数。SSL_CTX 仅由 HTTPS 线程实际使用。 */
struct listener_args {
    int port;
    int use_tls;
    SSL_CTX *ssl_ctx;
};

/* 每个客户端工作线程独占一份参数，线程退出前负责 free。 */
struct client_args {
    int client_fd;
    int use_tls;
    SSL_CTX *ssl_ctx;
};

/*
 * 统一描述普通 TCP 和 TLS 连接。
 * ssl 为 NULL 时使用 recv/send，否则使用 SSL_read/SSL_write。
 */
struct connection {
    int fd;
    SSL *ssl;
};

/* HTTP Range 对应的闭区间，start 和 end 两端的字节都需要发送。 */
struct byte_range {
    off_t start;
    off_t end;
};

/*
 * 保存同一 TCP 连接中尚未处理的数据。
 * TCP 没有消息边界，一次读取可能只有半个请求，也可能包含多个请求。
 */
struct request_buffer {
    char data[REQUEST_LIMIT + 1];
    size_t used;
};

/* 从普通连接或 TLS 连接读取数据，并把“对端正常关闭”统一表示为 0。 */
static ssize_t connection_read(struct connection *conn, void *buffer, size_t length)
{
    if (conn->ssl != NULL) {
        int chunk = length > INT_MAX ? INT_MAX : (int)length;
        int result = SSL_read(conn->ssl, buffer, chunk);

        if (result > 0) {
            return result;
        }

        /* SSL_read 的负返回值必须结合 SSL_get_error 判断具体状态。 */
        switch (SSL_get_error(conn->ssl, result)) {
        case SSL_ERROR_ZERO_RETURN:
            return 0;
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            errno = EINTR;
            return -1;
        default:
            return -1;
        }
    }

    return recv(conn->fd, buffer, length, 0);
}

/*
 * 完整发送 length 字节。
 * send 和 SSL_write 都可能只发送一部分，因此不能只调用一次。
 */
static int connection_write_all(struct connection *conn, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t result;

        if (conn->ssl != NULL) {
            size_t remaining = length - sent;
            int chunk = remaining > INT_MAX ? INT_MAX : (int)remaining;
            int ssl_result = SSL_write(conn->ssl, bytes + sent, chunk);

            if (ssl_result <= 0) {
                int ssl_error = SSL_get_error(conn->ssl, ssl_result);
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                    continue;
                }
                return -1;
            }
            result = ssl_result;
        } else {
#ifdef MSG_NOSIGNAL
            /* 客户端提前断开时不要让 SIGPIPE 终止整个服务器进程。 */
            result = send(conn->fd, bytes + sent, length - sent, MSG_NOSIGNAL);
#else
            result = send(conn->fd, bytes + sent, length - sent, 0);
#endif
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result <= 0) {
                return -1;
            }
        }

        sent += (size_t)result;
    }

    return 0;
}

/*
 * 从连接缓冲区中提取一条完整 HTTP 请求头。
 * 返回 0 表示成功，1 表示请求头过大，2 表示对端正常关闭，-1 表示错误。
 * 若缓冲区中还存在下一条请求，则通过 memmove 保留到下一轮处理。
 */
static int receive_request_headers(struct connection *conn,
                                   struct request_buffer *buffer,
                                   char **request_out)
{
    for (;;) {
        char *header_end;

        buffer->data[buffer->used] = '\0';
        /* HTTP 头部以空行结束，即连续的两个 CRLF。 */
        header_end = strstr(buffer->data, "\r\n\r\n");
        if (header_end != NULL) {
            size_t request_length = (size_t)(header_end - buffer->data) + 4;
            size_t remaining = buffer->used - request_length;
            char *request = malloc(request_length + 1);

            if (request == NULL) {
                return -1;
            }
            memcpy(request, buffer->data, request_length);
            request[request_length] = '\0';
            /* 只取第一条请求，其余字节可能已经包含后续请求。 */
            memmove(buffer->data, buffer->data + request_length, remaining);
            buffer->used = remaining;
            *request_out = request;
            return 0;
        }

        if (buffer->used == REQUEST_LIMIT) {
            return 1;
        }

        ssize_t received = connection_read(conn, buffer->data + buffer->used,
                                           REQUEST_LIMIT - buffer->used);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (received == 0) {
            return buffer->used == 0 ? 2 : -1;
        }

        buffer->used += (size_t)received;
    }
}

/*
 * 在 HTTP 请求头中查找指定字段。
 * 字段名不区分大小写，返回值会去除冒号后的前后空白。
 */
static int get_header_value(const char *request, const char *name,
                            char *value, size_t value_size)
{
    const char *line = strstr(request, "\r\n");
    size_t name_length = strlen(name);

    if (line == NULL || value_size == 0) {
        return 0;
    }
    line += 2;

    while (*line != '\0' && strncmp(line, "\r\n", 2) != 0) {
        const char *line_end = strstr(line, "\r\n");
        const char *colon;
        const char *begin;
        const char *end;
        size_t copy_length;

        if (line_end == NULL) {
            break;
        }

        /* 只在当前行内寻找冒号，避免误读下一行。 */
        colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon != NULL && (size_t)(colon - line) == name_length &&
            strncasecmp(line, name, name_length) == 0) {
            begin = colon + 1;
            while (begin < line_end && isspace((unsigned char)*begin)) {
                begin++;
            }

            end = line_end;
            while (end > begin && isspace((unsigned char)end[-1])) {
                end--;
            }

            copy_length = (size_t)(end - begin);
            if (copy_length >= value_size) {
                copy_length = value_size - 1;
            }
            memcpy(value, begin, copy_length);
            value[copy_length] = '\0';
            return 1;
        }

        line = line_end + 2;
    }

    return 0;
}

/* 检查 Connection 等逗号分隔字段中是否包含指定标记。 */
static int header_has_token(const char *value, const char *token)
{
    size_t token_length = strlen(token);

    while (*value != '\0') {
        const char *begin;
        const char *end;

        while (*value == ',' || isspace((unsigned char)*value)) {
            value++;
        }
        begin = value;
        while (*value != '\0' && *value != ',') {
            value++;
        }
        end = value;
        while (end > begin && isspace((unsigned char)end[-1])) {
            end--;
        }

        if ((size_t)(end - begin) == token_length &&
            strncasecmp(begin, token, token_length) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * 判断处理完当前请求后是否保留连接。
 * HTTP/1.1 默认保持连接；HTTP/1.0 只有显式 keep-alive 才保持连接。
 */
static int request_should_keep_alive(const char *request, const char *version)
{
    char connection_value[256];
    int has_connection = get_header_value(request, "Connection",
                                          connection_value,
                                          sizeof(connection_value));

    if (has_connection && header_has_token(connection_value, "close")) {
        return 0;
    }
    if (strcmp(version, "HTTP/1.1") == 0) {
        return 1;
    }
    return has_connection && header_has_token(connection_value, "keep-alive");
}

/* 将 URL 百分号编码中的单个十六进制字符转换为数值。 */
static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

/*
 * 将请求目标转换为当前目录下的文件路径。
 * 处理 %xx 解码、查询字符串、根路径和目录路径，并拒绝控制字符、
 * 反斜杠和 ".." 路径段，防止读取服务器工作目录以外的文件。
 */
static int target_to_file_path(const char *target, char *path, size_t path_size)
{
    char decoded[PATH_MAX];
    size_t source = 0;
    size_t destination = 0;
    const char *segment;

    if (target == NULL || target[0] != '/' || path_size < 4) {
        return -1;
    }

    /* 查询参数和片段标识不属于本地文件名。 */
    while (target[source] != '\0' && target[source] != '?' && target[source] != '#') {
        unsigned char ch = (unsigned char)target[source];

        if (ch == '%') {
            int high;
            int low;

            if (target[source + 1] == '\0' || target[source + 2] == '\0') {
                return -1;
            }
            high = hex_value(target[source + 1]);
            low = hex_value(target[source + 2]);
            if (high < 0 || low < 0) {
                return -1;
            }
            ch = (unsigned char)((high << 4) | low);
            source += 3;
        } else {
            source++;
        }

        if (ch == '\0' || ch == '\\' || ch < 0x20 || destination + 1 >= sizeof(decoded)) {
            return -1;
        }
        decoded[destination++] = (char)ch;
    }
    decoded[destination] = '\0';

    /* 百分号解码后再次逐段检查，防止使用 %2e%2e 绕过检查。 */
    segment = decoded + 1;
    while (*segment != '\0') {
        const char *slash = strchr(segment, '/');
        size_t segment_length = slash == NULL ? strlen(segment) : (size_t)(slash - segment);

        if (segment_length == 2 && segment[0] == '.' && segment[1] == '.') {
            return -1;
        }
        if (slash == NULL) {
            break;
        }
        segment = slash + 1;
    }

    /* 根目录和以斜杠结尾的目录默认返回其中的 index.html。 */
    if (strcmp(decoded, "/") == 0) {
        if (snprintf(path, path_size, "./index.html") >= (int)path_size) {
            return -1;
        }
    } else if (destination > 0 && decoded[destination - 1] == '/') {
        if (snprintf(path, path_size, ".%sindex.html", decoded) >= (int)path_size) {
            return -1;
        }
    } else {
        if (snprintf(path, path_size, ".%s", decoded) >= (int)path_size) {
            return -1;
        }
    }

    return 0;
}

/* 根据文件扩展名生成常用 Content-Type，未知类型按二进制文件处理。 */
static const char *content_type_for_path(const char *path)
{
    const char *extension = strrchr(path, '.');

    if (extension == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(extension, ".html") == 0 || strcasecmp(extension, ".htm") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcasecmp(extension, ".txt") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(extension, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcasecmp(extension, ".js") == 0) {
        return "application/javascript";
    }
    if (strcasecmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(extension, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(extension, ".svg") == 0) {
        return "image/svg+xml";
    }

    return "application/octet-stream";
}

/* 构造并发送带纯文本正文的短响应，主要用于 4xx 错误。 */
static int send_text_response(struct connection *conn, int status_code,
                              const char *reason, const char *body,
                              int keep_alive)
{
    char header[1024];
    size_t body_length = strlen(body);
    int header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 %d %s\r\n"
                                 "Content-Type: text/plain; charset=utf-8\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: %s\r\n"
                                 "\r\n",
                                 status_code, reason, body_length,
                                 keep_alive ? "keep-alive" : "close");

    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return -1;
    }
    /* 响应头必须先于响应体完整发出。 */
    if (connection_write_all(conn, header, (size_t)header_length) < 0) {
        return -1;
    }
    return connection_write_all(conn, body, body_length);
}

/*
 * 解析单个 Range 字段。
 * 支持 bytes=a-b、bytes=a- 和 bytes=-n；返回 0 表示没有 Range，
 * 1 表示解析成功，-1 表示格式或范围非法。
 */
static int parse_range_header(const char *range_value, off_t file_size,
                              struct byte_range *range)
{
    const char *value;
    const char *dash;
    char *end_pointer;
    unsigned long long first;
    unsigned long long last;

    if (range_value == NULL || range_value[0] == '\0') {
        return 0;
    }
    if (file_size <= 0 || strncasecmp(range_value, "bytes=", 6) != 0) {
        return -1;
    }

    value = range_value + 6;
    while (isspace((unsigned char)*value)) {
        value++;
    }
    if (strchr(value, ',') != NULL) {
        return -1;
    }

    dash = strchr(value, '-');
    if (dash == NULL) {
        return -1;
    }

    if (dash == value) {
        /* bytes=-n 表示文件末尾的 n 个字节。 */
        errno = 0;
        last = strtoull(dash + 1, &end_pointer, 10);
        while (isspace((unsigned char)*end_pointer)) {
            end_pointer++;
        }
        if (errno != 0 || end_pointer == dash + 1 || *end_pointer != '\0' || last == 0) {
            return -1;
        }
        if (last >= (unsigned long long)file_size) {
            range->start = 0;
        } else {
            range->start = file_size - (off_t)last;
        }
        range->end = file_size - 1;
        return 1;
    }

    errno = 0;
    first = strtoull(value, &end_pointer, 10);
    if (errno != 0 || end_pointer != dash || first >= (unsigned long long)file_size) {
        return -1;
    }

    if (dash[1] == '\0') {
        /* bytes=a- 的结束位置为文件最后一个字节。 */
        last = (unsigned long long)file_size - 1;
    } else {
        errno = 0;
        last = strtoull(dash + 1, &end_pointer, 10);
        while (isspace((unsigned char)*end_pointer)) {
            end_pointer++;
        }
        if (errno != 0 || end_pointer == dash + 1 || *end_pointer != '\0' || last < first) {
            return -1;
        }
        if (last >= (unsigned long long)file_size) {
            last = (unsigned long long)file_size - 1;
        }
    }

    range->start = (off_t)first;
    range->end = (off_t)last;
    return 1;
}

/*
 * 返回完整文件或指定字节范围。
 * 普通请求返回 200；合法 Range 返回 206；非法 Range 返回 416；
 * 文件不存在或不是普通文件时返回 404。
 */
static int send_file_response(struct connection *conn, const char *path,
                              const char *range_value, int keep_alive)
{
    int file_fd;
    struct stat file_stat;
    struct byte_range range;
    int range_result;
    off_t start;
    off_t end;
    off_t remaining;
    char header[2048];
    int header_length;
    unsigned char buffer[IO_BUFFER_SIZE];

    file_fd = open(path, O_RDONLY);
    if (file_fd < 0) {
        return send_text_response(conn, 404, "Not Found", "404 Not Found\n",
                                  keep_alive);
    }

    if (fstat(file_fd, &file_stat) < 0 || !S_ISREG(file_stat.st_mode)) {
        close(file_fd);
        return send_text_response(conn, 404, "Not Found", "404 Not Found\n",
                                  keep_alive);
    }

    range_result = parse_range_header(range_value, file_stat.st_size, &range);
    if (range_result < 0) {
        const char *body = "416 Range Not Satisfiable\n";
        size_t body_length = strlen(body);

        header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 416 Range Not Satisfiable\r\n"
                                 "Content-Range: bytes */%lld\r\n"
                                 "Content-Type: text/plain; charset=utf-8\r\n"
                                 "Content-Length: %zu\r\n"
                                 "Connection: %s\r\n"
                                 "\r\n",
                                 (long long)file_stat.st_size, body_length,
                                 keep_alive ? "keep-alive" : "close");
        close(file_fd);
        if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
            return -1;
        }
        if (connection_write_all(conn, header, (size_t)header_length) < 0) {
            return -1;
        }
        return connection_write_all(conn, body, body_length);
    }

    if (range_result == 1) {
        /* Range 的结束下标是包含式，因此长度需要加 1。 */
        start = range.start;
        end = range.end;
        header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 206 Partial Content\r\n"
                                 "Content-Type: %s\r\n"
                                 "Content-Length: %lld\r\n"
                                 "Content-Range: bytes %lld-%lld/%lld\r\n"
                                 "Accept-Ranges: bytes\r\n"
                                 "Connection: %s\r\n"
                                 "\r\n",
                                 content_type_for_path(path),
                                 (long long)(end - start + 1),
                                 (long long)start, (long long)end,
                                 (long long)file_stat.st_size,
                                 keep_alive ? "keep-alive" : "close");
    } else {
        start = 0;
        end = file_stat.st_size - 1;
        header_length = snprintf(header, sizeof(header),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: %s\r\n"
                                 "Content-Length: %lld\r\n"
                                 "Accept-Ranges: bytes\r\n"
                                 "Connection: %s\r\n"
                                 "\r\n",
                                 content_type_for_path(path),
                                 (long long)file_stat.st_size,
                                 keep_alive ? "keep-alive" : "close");
    }

    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        close(file_fd);
        return -1;
    }
    if (connection_write_all(conn, header, (size_t)header_length) < 0) {
        close(file_fd);
        return -1;
    }

    /* 定位到范围起点后，按块读取并通过统一接口发送。 */
    if (lseek(file_fd, start, SEEK_SET) < 0) {
        close(file_fd);
        return -1;
    }

    remaining = end >= start ? end - start + 1 : 0;
    while (remaining > 0) {
        size_t wanted = remaining > (off_t)sizeof(buffer) ? sizeof(buffer) : (size_t)remaining;
        ssize_t read_length = read(file_fd, buffer, wanted);

        if (read_length < 0 && errno == EINTR) {
            continue;
        }
        if (read_length <= 0) {
            close(file_fd);
            return -1;
        }
        if (connection_write_all(conn, buffer, (size_t)read_length) < 0) {
            close(file_fd);
            return -1;
        }
        remaining -= read_length;
    }

    close(file_fd);
    return 0;
}

/*
 * 为 80 端口构造 301 响应。
 * Location 使用请求中的 Host 和原始 target，保证路径与查询参数不丢失。
 */
static int send_https_redirect(struct connection *conn, const char *request,
                               const char *target, int keep_alive)
{
    char host[512] = "10.0.0.1";
    char header[2048];
    char *colon;
    int header_length;

    get_header_value(request, "Host", host, sizeof(host));

    /* 默认 HTTP 端口不应出现在跳转后的 HTTPS URL 中。 */
    colon = strrchr(host, ':');
    if (colon != NULL && strchr(host, ']') == NULL && strcmp(colon, ":80") == 0) {
        *colon = '\0';
    }

    header_length = snprintf(header, sizeof(header),
                             "HTTP/1.1 301 Moved Permanently\r\n"
                             "Location: https://%s%s\r\n"
                             "Content-Length: 0\r\n"
                             "Connection: %s\r\n"
                             "\r\n",
                             host, target,
                             keep_alive ? "keep-alive" : "close");

    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return -1;
    }
    return connection_write_all(conn, header, (size_t)header_length);
}

/*
 * 解析请求行并分派处理逻辑。
 * HTTP 连接只负责 301；HTTPS 连接将 URL 映射为文件，再决定 200/206/404。
 */
static int process_http_request(struct connection *conn, int use_tls,
                                const char *request, int *keep_alive_out)
{
    char method[16];
    char target[PATH_MAX];
    char version[32];
    char path[PATH_MAX];
    char range_value[256];
    const char *range = NULL;
    int keep_alive;

    /* 请求行格式为：方法 空格 请求目标 空格 HTTP版本。 */
    if (sscanf(request, "%15s %4095s %31s", method, target, version) != 3) {
        *keep_alive_out = 0;
        return send_text_response(conn, 400, "Bad Request", "400 Bad Request\n", 0);
    }

    if (strcmp(method, "GET") != 0) {
        *keep_alive_out = 0;
        return send_text_response(conn, 405, "Method Not Allowed",
                                  "405 Method Not Allowed\n", 0);
    }

    if (strncmp(version, "HTTP/", 5) != 0) {
        *keep_alive_out = 0;
        return send_text_response(conn, 400, "Bad Request", "400 Bad Request\n", 0);
    }

    keep_alive = request_should_keep_alive(request, version);
    *keep_alive_out = keep_alive;

    if (!use_tls) {
        return send_https_redirect(conn, request, target, keep_alive);
    }

    if (target_to_file_path(target, path, sizeof(path)) < 0) {
        return send_text_response(conn, 404, "Not Found", "404 Not Found\n",
                                  keep_alive);
    }

    if (get_header_value(request, "Range", range_value, sizeof(range_value))) {
        range = range_value;
    }
    return send_file_response(conn, path, range, keep_alive);
}

/*
 * 单个客户端的工作线程。
 * HTTPS 客户端先完成 TLS 握手，再循环处理同一连接上的多个请求；
 * 所有请求按读取顺序响应，因此不会发生同一连接内响应串线。
 */
static void *client_worker(void *argument)
{
    struct client_args *args = argument;
    struct connection conn;
    struct request_buffer request_buffer;

    conn.fd = args->client_fd;
    conn.ssl = NULL;
    request_buffer.used = 0;

    if (args->use_tls) {
        /* SSL_CTX 可在线程间共享，但每条连接必须创建独立 SSL 对象。 */
        conn.ssl = SSL_new(args->ssl_ctx);
        if (conn.ssl == NULL) {
            ERR_print_errors_fp(stderr);
            close(conn.fd);
            free(args);
            return NULL;
        }

        SSL_set_fd(conn.ssl, conn.fd);
        if (SSL_accept(conn.ssl) <= 0) {
            ERR_print_errors_fp(stderr);
            SSL_free(conn.ssl);
            close(conn.fd);
            free(args);
            return NULL;
        }
    }

    /* 持久连接循环；遇到 Connection: close、超时或读写错误时退出。 */
    for (;;) {
        char *request = NULL;
        int receive_result = receive_request_headers(&conn, &request_buffer, &request);

        if (receive_result == 0) {
            int keep_alive = 0;
            int process_result = process_http_request(&conn, args->use_tls,
                                                      request, &keep_alive);
            free(request);
            if (process_result < 0 || !keep_alive) {
                break;
            }
            continue;
        }

        if (receive_result == 1) {
            send_text_response(&conn, 431, "Request Header Fields Too Large",
                               "431 Request Header Fields Too Large\n", 0);
        }
        break;
    }

    /* 资源释放顺序：TLS 会话、Socket、线程参数。 */
    if (conn.ssl != NULL) {
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
    }
    shutdown(conn.fd, SHUT_RDWR);
    close(conn.fd);
    free(args);
    return NULL;
}

/* 为客户端 Socket 设置收发超时，避免异常连接永久占用工作线程。 */
static void set_client_timeouts(int client_fd)
{
    struct timeval timeout;

    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/*
 * HTTP 或 HTTPS 的监听线程。
 * 负责 socket、SO_REUSEADDR、bind、listen 和 accept；每个客户端交给
 * detached 工作线程，工作线程结束后无需主线程再次 pthread_join。
 */
static void *listener_worker(void *argument)
{
    struct listener_args *args = argument;
    int listen_fd;
    int enable = 1;
    struct sockaddr_in address;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return NULL;
    }

    /* 允许服务器重启后立即重新绑定仍处于 TIME_WAIT 相关状态的端口。 */
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(listen_fd);
        return NULL;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    /* 端口字段需要从主机字节序转换为网络字节序。 */
    address.sin_port = htons((uint16_t)args->port);

    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind");
        close(listen_fd);
        return NULL;
    }
    if (listen(listen_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return NULL;
    }

    for (;;) {
        struct sockaddr_in client_address;
        socklen_t client_length = sizeof(client_address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_address, &client_length);
        struct client_args *client;
        pthread_t thread;

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            continue;
        }

        set_client_timeouts(client_fd);
        client = malloc(sizeof(*client));
        if (client == NULL) {
            close(client_fd);
            continue;
        }
        client->client_fd = client_fd;
        client->use_tls = args->use_tls;
        client->ssl_ctx = args->ssl_ctx;

        if (pthread_create(&thread, NULL, client_worker, client) != 0) {
            perror("pthread_create");
            close(client_fd);
            free(client);
            continue;
        }
        /* detached 线程结束时自动回收线程控制块。 */
        pthread_detach(thread);
    }

    close(listen_fd);
    return NULL;
}

/* 创建 TLS 服务端上下文，加载证书和私钥，并验证二者是否匹配。 */
static SSL_CTX *create_ssl_context(void)
{
    const SSL_METHOD *method;
    SSL_CTX *context;

    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    /* TLS_server_method 由 OpenSSL 自动协商当前支持的服务端 TLS 版本。 */
    method = TLS_server_method();
    context = SSL_CTX_new(method);
    if (context == NULL) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(context, "./keys/cnlab.cert", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(context);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(context, "./keys/cnlab.prikey", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(context);
        return NULL;
    }
    if (SSL_CTX_check_private_key(context) != 1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(context);
        return NULL;
    }

    return context;
}

/* 初始化全局资源，启动 80/443 两个监听线程，并等待服务器运行。 */
int main(void)
{
    SSL_CTX *ssl_ctx;
    struct listener_args http_args;
    struct listener_args https_args;
    pthread_t http_thread;
    pthread_t https_thread;
    int http_created = 0;
    int https_created = 0;

    /* 客户端提前关闭连接时，写失败应由返回值处理，不能杀死整个进程。 */
    signal(SIGPIPE, SIG_IGN);

    ssl_ctx = create_ssl_context();
    if (ssl_ctx == NULL) {
        fprintf(stderr, "failed to initialize TLS context\n");
        return EXIT_FAILURE;
    }

    /* 两个监听线程共享只读 SSL_CTX；HTTP 线程不会实际使用 TLS。 */
    http_args.port = 80;
    http_args.use_tls = 0;
    http_args.ssl_ctx = ssl_ctx;

    https_args.port = 443;
    https_args.use_tls = 1;
    https_args.ssl_ctx = ssl_ctx;

    if (pthread_create(&http_thread, NULL, listener_worker, &http_args) == 0) {
        http_created = 1;
    } else {
        perror("pthread_create(http)");
    }

    if (pthread_create(&https_thread, NULL, listener_worker, &https_args) == 0) {
        https_created = 1;
    } else {
        perror("pthread_create(https)");
    }

    if (!http_created || !https_created) {
        SSL_CTX_free(ssl_ctx);
        return EXIT_FAILURE;
    }

    /* 监听线程正常情况下无限运行，join 使 main 始终保持存活。 */
    pthread_join(http_thread, NULL);
    pthread_join(https_thread, NULL);
    SSL_CTX_free(ssl_ctx);
    return EXIT_SUCCESS;
}
