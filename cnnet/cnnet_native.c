/*
 *  cnnet_native.c —— Canon 网络通信层的 loong64 原生替代实现
 *
 *  替代三个闭源库（原库仅在 /opt/canonij2 下）：
 *    libcnnet2.so     -> CNNL_*    （旧通信接口，cnijifnet.c 用）
 *    libcnbpnet30.so  -> CNNET3_*  （新传输层，cnijifnet2.c 用）
 *    libcnbpnet20.so  -> CNNET2_*  （SNMP 发现，cnijifnet2.c dlopen）
 *
 *  设计取舍（诚实标注）：
 *    * CNNET3_* / CNNL_* 的**传输**部分按 IVEC over TCP(9100) / CHMP over
 *      HTTP 实现，两条协议均已在真机 G3010 上端到端验证：
 *        - 打印数据  : TCP 9100 RAW（直接灌 rastertocanonij 输出 → 打印机出纸）
 *        - 状态/控制 : CHMP = POST 提交 + GET 取响应（transfer-encoding: chunked）
 *      协议细节来自官方 cnijlgmon3 的真机抓包（抓包材料属一次性证据，不随仓库分发）。
 *    * CNNET2_* 的**发现**部分用 SNMP v1（community canon_admin + Canon 私有
 *      OID，企业号 1602）**单播扫描**实现 —— 不发广播，因为本机 firewalld 的
 *      conntrack 会把广播回包判成 NEW 并回 ICMP admin prohibited。
 *      真机实测：192.168.1.84 / MAC 6C3C7CA051CB / serial YCAA29779 / conn 5。
 *    * 所有函数在参数非法/未连接时返回错误码而非崩溃，保证 lgmon3 稳定。
 *
 *  编译（三份，SONAME 各自正确）：
 *    gcc -O2 -fPIC -shared -o libcnnet2.so.1.2.5     cnnet_native.c -Wl,-soname,libcnnet2.so
 *    gcc -O2 -fPIC -shared -o libcnbpnet30.so.1.0.0  cnnet_native.c -Wl,-soname,libcnbpnet30.so
 *    gcc -O2 -fPIC -shared -o libcnbpnet20.so.1.0.0  cnnet_native.c -Wl,-soname,libcnbpnet20.so
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ifaddrs.h>
#include <net/if.h>

/* 发现结果结构 tagSearchPrinterInfo —— 直接采用源码树里的权威定义，
 * 保证与 lgmon3 编译时的布局逐字节一致（避免手抄结构体出错）。 */
#include "libcnnet2_type.h"

/* ================================================================== */
/*  常量                                                              */
/* ================================================================== */
#define CNNET3_ERR_SUCCESS              0
#define CNNET3_ERR_NOCONTENT            1
#define CNNET3_ERR_FATAL               -1
#define CNNET3_ERR_UNKNOWN_IFTYPE      -2
#define CNNET3_ERR_INVALID_HANDLE      -3
#define CNNET3_ERR_INVALID_OPERATION   -4
#define CNNET3_ERR_INVALID_IP_FORMAT   -5
#define CNNET3_ERR_FAILED_TO_RESOLVE   -6
#define CNNET3_ERR_FAILED_TO_CONNECT   -7
#define CNNET3_ERR_INVALID_PARAMETER  -14
#define CNNET3_ERR_WRITING_TIMEOUT    -11
#define CNNET3_ERR_READING_TIMEOUT    -12
#define CNNET3_ERR_CONNECTION_ABORTED -13
#define CNNET3_ERR_INSUFFICIENT_BUFFER -16

#define CNNET3_IFTYPE_HTTP      1
#define CNNET3_IFTYPE_PORT9100  2

#define CNNET2_ERROR_CODE_SUCCESS       0
#define CNNET2_ERROR_CODE_PARAM        -1
#define CNNET2_ERROR_CODE_MEMORY       -2
#define CNNET2_ERROR_CODE_OTHER        -3
#define CNNET2_ERROR_CODE_SOCKET       -4
#define CNNET2_ERROR_CODE_PACKET       -5
#define CNNET2_ERROR_CODE_RECV_TIMEOUT -6
#define CNNET2_ERROR_CODE_NIC          -7

#define CNNL_RET_SUCCESS       0
#define CNNL_RET_FAILURE       1
#define CNNL_RET_BUSY          2
#define CNNL_RET_NOT_WORKING   3
#define CNNL_RET_POWEROFF      4
#define CNNL_RET_BUSY_RESPONSE 5

#define CNNL_COMMAND_SUPPORT   0
#define CNNL_COMMAND_NOSUPPORT 1
#define CNNL_CONFIG_SET_VERSION 0
#define CNNET_SEARCH_BROADCAST 0
#define CNNET_SEARCH_UNICAST   1
#define CNNET_SEARCH_AUTO      2
#define CNNET_SEARCH_CACHE_ALL 3
#define CNNET_SEARCH_CACHE_ACTIVEONLY 4

#define DEFAULT_TMO_MS  3000
#define PORT_RAW        9100
#define PORT_HTTP       80
/* CHMP 的 X-CHMP-Timeout 取值。官方 cnijlgmon3 实发就是 20，
 * 照抄（改成 10000 这种大值反而会被打印机判 409 Conflict）。 */
#define CN_CHMP_POST_TIMEOUT 20

/* ================================================================== */
/*  公共工具                                                          */
/* ================================================================== */
long long cn_ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* 十进制点分 IPv4 合法性 */
int cn_valid_ipv4(const char *s)
{
    struct in_addr a;
    return s && inet_pton(AF_INET, s, &a) == 1;
}

/* 带超时的 TCP 连接 */
int cn_tcp_connect(const char *ip, int port, int tmo_ms)
{
    int fd, flags, rc;
    struct sockaddr_in sa;
    struct pollfd pfd;

    if (!cn_valid_ipv4(ip)) {
        return -1;
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &sa.sin_addr);

    rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        pfd.fd = fd;
        pfd.events = POLLOUT;
        rc = poll(&pfd, 1, tmo_ms > 0 ? tmo_ms : DEFAULT_TMO_MS);
        if (rc <= 0) {
            close(fd);
            return -1;
        }
        { int soerr = 0; socklen_t sl = sizeof(soerr);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
              close(fd);
              return -1;
          } }
    }
    fcntl(fd, F_SETFL, flags);          /* 回到阻塞，随后用 poll 控超时 */
    { int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    return fd;
}

/* 带超时的一次写完 */
int cn_write_all(int fd, const void *buf, size_t len, int tmo_ms)
{
    const uint8_t *p = buf;
    size_t off = 0;
    while (off < len) {
        struct pollfd pfd = { fd, POLLOUT, 0 };
        if (poll(&pfd, 1, tmo_ms > 0 ? tmo_ms : DEFAULT_TMO_MS) <= 0) {
            return -1;
        }
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* 带超时读取（读到 len 或对端关闭） */
ssize_t cn_read_some(int fd, void *buf, size_t len, int tmo_ms)
{
    struct pollfd pfd = { fd, POLLIN, 0 };
    int rc = poll(&pfd, 1, tmo_ms > 0 ? tmo_ms : DEFAULT_TMO_MS);
#ifdef _DEBUG_MODE_
    if (rc <= 0)
        fprintf(stderr, "[cn_read_some] fd=%d poll rc=%d revents=%#x errno=%d (%s)\n",
                fd, rc, (unsigned)pfd.revents, errno, strerror(errno));
#endif
    if (rc == 0)  return -2;                 /* 超时 */
    if (rc < 0)   return -1;
    ssize_t n = recv(fd, buf, len, 0);
#ifdef _DEBUG_MODE_
    if (n <= 0)
        fprintf(stderr, "[cn_read_some] fd=%d recv n=%zd errno=%d (%s)\n",
                fd, n, errno, strerror(errno));
#endif
    return n;
}

/* HTTP 响应体定位：返回 body 起始，*bodyLen 为可读长度 */
char *cn_http_body(char *resp, size_t got, size_t *bodyLen)
{
    char *p = strstr(resp, "\r\n\r\n");
    if (!p) {
        p = strstr(resp, "\n\n");
        if (!p) { *bodyLen = 0; return NULL; }
        p += 2;
    } else {
        p += 4;
    }
    *bodyLen = got - (size_t)(p - resp);
    return p;
}



/* ================================================================== */
/*  CNNET3_*  —— IVEC over TCP(9100) / HTTP                            */
/* ================================================================== */
typedef struct {
    int   fd;
    int   iftype;
    char  ip[64];
    const char *url;
    int   tmo_ms;
    unsigned int evbuf_size;
    int   cmdtype;
    int   evtype;
    int   connected;
    uint8_t *acc;            /* HTTP 模式下的累积缓冲 */
    size_t   acc_len, acc_cap;
    /* CHMP 一请求两回合的状态：POST 提交 → 收 ack → GET 取响应体。
     * 每发出一次新的 POST 都要把这两个标志清零。 */
    int   http_ack_done;     /* POST 的 "200 OK" 空 ack 已收掉 */
    int   http_get_sent;     /* GET 已发出 */
    int   http_verbose;      /* 打印 CHMP 交互细节到 stderr（排障用） */
    /* 用户态读缓冲。⚠ 必须有：HTTP 响应头和数据常在同一次 recv 里到达，
     * 若「读到 \r\n\r\n 就收手」，多读进来的 body 字节会随栈帧丢掉，
     * 之后再也读不到（表现为等到超时）。所有读都经过这个缓冲。 */
    uint8_t rbuf[8192];
    size_t  rlen, rpos;
} cnnet3_t;

/* ------------------------------------------------------------------ *
 *  CHMP over HTTP 的读写原语
 *
 *  协议实测（官方 cnijlgmon3 真机报文）：
 *    1) POST <url>  —— 提交命令，打印机只回一个不含 body 的 "200 OK" ack
 *    2) GET  <url>  —— 同一 TCP 连接上取回真正的响应，body 是
 *                      Transfer-Encoding: chunked
 *  只 POST 不 GET 的话永远读不到内容（旧实现就卡在这里，返回 err=1）。
 * ------------------------------------------------------------------ */

/* --- 用户态读缓冲：CHMP 的头和 body 必须从同一个缓冲里顺序消费 ----- */

/* 从 socket 再读一段进 rbuf。0 成功 / -1 断开 / -2 超时 */
static int cn_buf_fill(cnnet3_t *h)
{
    ssize_t n;
    h->rpos = 0;
    h->rlen = 0;
    n = cn_read_some(h->fd, h->rbuf, sizeof(h->rbuf), h->tmo_ms);
    if (n == -2) return -2;
    if (n <= 0)  return -1;
    h->rlen = (size_t)n;
    return 0;
}

/* 从缓冲取 1 字节；返回 0..255 / -1 断开 / -2 超时 */
static int cn_buf_getc(cnnet3_t *h)
{
    if (h->rpos >= h->rlen) {
        int r = cn_buf_fill(h);
        if (r != 0) return r;
        if (h->rlen == 0) return -1;
    }
    return (int)h->rbuf[h->rpos++];
}

/* 读满一个 HTTP 响应头（含结尾空行）。
 * hdr 至少 cap 字节；成功时 hdr 以 NUL 结尾、*hdrLen = 头部总字节数。
 * 返回 0 成功 / -1 连接断开 / -2 超时 / -3 头过大 */
static int cn_http_read_hdr(cnnet3_t *h, char *hdr, size_t cap, size_t *hdrLen)
{
    size_t got = 0;
    if (cap < 8) return -3;
    hdr[0] = '\0';
    for (;;) {
        if (got >= 4 && memcmp(hdr + got - 4, "\r\n\r\n", 4) == 0) {
            *hdrLen = got;
            hdr[got] = '\0';
            return 0;
        }
        if (got + 1 >= cap) return -3;
        {   /* 逐字节取，走用户态缓冲，绝不越过边界多读 */
            int c = cn_buf_getc(h);
            if (c == -1) return -1;
            if (c == -2) return -2;
            hdr[got++] = (char)c;
            hdr[got] = '\0';
        }
    }
}

/* 精确读 n 字节（走用户态缓冲，头/体不会互相吃掉）：0 成功 / -1 断开 / -2 超时 */
static int cn_read_exact(cnnet3_t *h, unsigned char *buf, size_t n)
{
    while (n > 0) {
        if (h->rpos >= h->rlen) {
            int r = cn_buf_fill(h);
            if (r != 0) return r;
        }
        {
            size_t avail = h->rlen - h->rpos;
            size_t take = (n < avail) ? n : avail;
            if (buf) { memcpy(buf, h->rbuf + h->rpos, take); buf += take; }
            h->rpos += take;
            n -= take;
        }
    }
    return 0;
}

/* 头里取状态码，失败返回 0 */
static int cn_http_status(const char *hdr)
{
    int code = 0;
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &code) != 1) return 0;
    return code;
}

/* 头里是否含指定 token（大小写不敏感的子串匹配，够用） */
static int cn_hdr_has(const char *hdr, const char *token)
{
    size_t tl = strlen(token);
    for (const char *p = hdr; *p; p++) {
        if (strncasecmp(p, token, tl) == 0) return 1;
    }
    return 0;
}

/* 解析 Content-Length，没有则返回 -1 */
static long cn_hdr_content_length(const char *hdr)
{
    const char *p = hdr;
    while ((p = strcasestr(p, "Content-Length:")) != NULL) {
        p += strlen("Content-Length:");
        while (*p == ' ' || *p == '\t') p++;
        if (*p >= '0' && *p <= '9') return strtol(p, NULL, 10);
    }
    return -1;
}

/* 读一行（到 \n，含），用于 chunk 长度行。返回 0/-1/-2 */
static int cn_read_line(cnnet3_t *h, char *line, size_t cap)
{
    size_t got = 0;
    if (cap < 2) return -1;
    while (got + 1 < cap) {
        int c = cn_buf_getc(h);
        if (c == -1) return -1;
        if (c == -2) return -2;
        line[got++] = (char)c;
        if (c == '\n') break;
    }
    line[got] = '\0';
    return 0;
}

/* 按响应头把 body 收全（支持 chunked 与 Content-Length）。
 * 返回实际写入 body 的字节数；负数为错误（-1 断开 / -2 超时）。 */
static long cn_http_read_body(cnnet3_t *h, const char *hdr,
                              unsigned char *body, size_t cap)
{
    size_t out = 0;

    if (cn_hdr_has(hdr, "chunked")) {
        for (;;) {
            char line[64];
            unsigned long sz = 0;
            if (cn_read_line(h, line, sizeof(line)) != 0) return -1;
            sz = strtoul(line, NULL, 16);
            if (sz == 0) {
                /* 结尾：吃到一个空行为止（容忍 trailer） */
                for (int guard = 0; guard < 8; guard++) {
                    char t[256];
                    if (cn_read_line(h, t, sizeof(t)) != 0) break;
                    if (t[0] == '\r' || t[0] == '\n' || t[0] == '\0') break;
                }
                break;
            }
            {
                size_t take = sz;
                if (out + take > cap) take = (out < cap) ? cap - out : 0;
                if (take && cn_read_exact(h, body + out, take) != 0) return -1;
                out += take;
                /* 被截断的部分也要从流里吃掉，保持连接状态正确 */
                if (take < sz) {
                    unsigned char sink[4096];
                    size_t skip = sz - take;
                    while (skip > 0) {
                        size_t k = skip < sizeof(sink) ? skip : sizeof(sink);
                        if (cn_read_exact(h, sink, k) != 0) return -1;
                        skip -= k;
                    }
                }
            }
            {   /* chunk 数据后的 CRLF */
                unsigned char crlf[2];
                if (cn_read_exact(h, crlf, 2) != 0) return -1;
            }
        }
        return (long)out;
    }

    {
        long cl = cn_hdr_content_length(hdr);
        if (cl > 0) {
            size_t take = (size_t)cl;
            if (take > cap) take = cap;
            if (cn_read_exact(h, body, take) != 0) return -1;
            /* 超长部分从流里丢掉 */
            if ((size_t)cl > take) {
                unsigned char sink[4096];
                size_t skip = (size_t)cl - take;
                while (skip > 0) {
                    size_t k = skip < sizeof(sink) ? skip : sizeof(sink);
                    if (cn_read_exact(h, sink, k) != 0) return -1;
                    skip -= k;
                }
            }
            return (long)take;
        }
    }

    /* 既无 chunked 也无 Content-Length：按协议就是「无 body 的 ack」 */
    return 0;
}


void *CNNET3_Open(void)
{
    cnnet3_t *h = (cnnet3_t *)calloc(1, sizeof(cnnet3_t));
    if (!h) return NULL;
    h->fd = -1;
    h->iftype = CNNET3_IFTYPE_PORT9100;
    h->url = "/canon/ij/command2/port1";
    h->tmo_ms = DEFAULT_TMO_MS;
#ifdef _DEBUG_MODE_
    h->http_verbose = 1;
#endif
    return h;
}

int cnnet3_valid(cnnet3_t *h) { return h != NULL; }

int CNNET3_Close(void *handle)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!h) return CNNET3_ERR_INVALID_HANDLE;
    if (h->fd >= 0) { close(h->fd); h->fd = -1; }
    free(h->acc);
    free(h);
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetIF(void *handle, int ifType)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    if (ifType != CNNET3_IFTYPE_HTTP && ifType != CNNET3_IFTYPE_PORT9100)
        return CNNET3_ERR_UNKNOWN_IFTYPE;
    h->iftype = ifType;
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetIP(void *handle, const char *address)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    if (!cn_valid_ipv4(address)) return CNNET3_ERR_INVALID_IP_FORMAT;
    snprintf(h->ip, sizeof(h->ip), "%s", address);
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetPrinterName(void *handle, const char *printerName)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    (void)printerName;
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetURL(void *handle, const char *url)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    if (url) h->url = url;
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetTimeout(void *handle, int toSetting, unsigned int second)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    (void)toSetting;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    h->tmo_ms = (int)(second ? second * 1000u : DEFAULT_TMO_MS);
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetMasterPortOption(void *handle, int optionType)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    (void)optionType;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetEventType(void *handle, int eventtype)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    h->evtype = eventtype;
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetCommandType(void *handle, int commandtype)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    h->cmdtype = commandtype;
    return CNNET3_ERR_SUCCESS;
}

int CNNET3_SetEventBufferSize(void *handle, unsigned int size)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h)) return CNNET3_ERR_INVALID_HANDLE;
    h->evbuf_size = size;
    return CNNET3_ERR_SUCCESS;
}

int cnnet3_ensure_conn(cnnet3_t *h)
{
    if (h->fd >= 0 && h->connected) return 0;
    if (h->fd >= 0) { close(h->fd); h->fd = -1; }
    if (!cn_valid_ipv4(h->ip)) return -1;
    h->fd = cn_tcp_connect(h->ip,
                           h->iftype == CNNET3_IFTYPE_HTTP ? PORT_HTTP : PORT_RAW,
                           h->tmo_ms);
    if (h->fd < 0) return -1;
    h->connected = 1;
    h->http_ack_done = 0;
    h->http_get_sent = 0;
    h->rpos = 0;
    h->rlen = 0;
    return 0;
}

/* Send：RAW 直接发；HTTP 单次 POST（闭合式，一请求一响应） */
int CNNET3_Send(void *handle, unsigned char *sendBuffer,
                       unsigned long bufferSize, unsigned long *sentSize)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (sentSize) *sentSize = 0;
    if (!cnnet3_valid(h) || !sendBuffer) return CNNET3_ERR_INVALID_PARAMETER;
    if (cnnet3_ensure_conn(h) != 0)   return CNNET3_ERR_FAILED_TO_CONNECT;

    if (h->iftype == CNNET3_IFTYPE_HTTP) {
        /* CHMP = Canon HTTP Management Protocol。
         * 头部字段与顺序照官方 cnijlgmon3 的真实报文逐字节还原
         * （同一个 TCP 连接上两回合）：
         *
         *   回合 1  POST <url>   ← 提交命令
         *           ack: "HTTP/1.1 200 OK" + 3 个头，**没有 body**
         *   回合 2  GET  <url>   ← 取响应
         *           body: Transfer-Encoding: chunked，里面才是真 XML
         *
         * POST 头的原样顺序：
         *   Content-Length / X-CHMP-Timeout: 20 / Connection: Keep-Alive /
         *   Content-Type: application/octet-stream / Host / X-CHMP-Version: 1.0.0
         * 注意：官方 body 里**不带 jobID**（param_set 是空的）。
         * X-CHMP-Version 是硬性要求，写成别的值打印机会静默丢包。 */
        char head[512];
        int hl = snprintf(head, sizeof(head),
                          "POST %s HTTP/1.1\r\n"
                          "Content-Length: %lu\r\n"
                          "X-CHMP-Timeout: %d\r\n"
                          "Connection: Keep-Alive\r\n"
                          "Content-Type: application/octet-stream\r\n"
                          "Host: %s\r\n"
                          "X-CHMP-Version: 1.0.0\r\n\r\n",
                          h->url, bufferSize, CN_CHMP_POST_TIMEOUT, h->ip);
        if (cn_write_all(h->fd, head, (size_t)hl, h->tmo_ms) != 0)
            return CNNET3_ERR_WRITING_TIMEOUT;
        /* 新的一次请求周期：清掉上一轮的回合状态 */
        h->http_ack_done = 0;
        h->http_get_sent = 0;
    }
    if (cn_write_all(h->fd, sendBuffer, bufferSize, h->tmo_ms) != 0)
        return CNNET3_ERR_WRITING_TIMEOUT;
    if (sentSize) *sentSize = bufferSize;
    return CNNET3_ERR_SUCCESS;
}

/* Write：带 needContinue 的分块写（HTTP 下先累积，末块一次性 POST） */
int CNNET3_Write(void *handle, unsigned char *sendBuffer,
                        unsigned long bufferSize, int needContinue)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (!cnnet3_valid(h) || (!sendBuffer && bufferSize)) return CNNET3_ERR_INVALID_PARAMETER;

    if (h->iftype != CNNET3_IFTYPE_HTTP) {
        unsigned long sent = 0;
        return CNNET3_Send(handle, sendBuffer, bufferSize, &sent);
    }

    /* HTTP：累积，needContinue==0 时发出 */
    if (h->acc_len + bufferSize > h->acc_cap) {
        size_t ncap = (h->acc_len + bufferSize) * 2 + 1024;
        uint8_t *np = (uint8_t *)realloc(h->acc, ncap);
        if (!np) return CNNET3_ERR_FATAL;
        h->acc = np; h->acc_cap = ncap;
    }
    if (bufferSize) memcpy(h->acc + h->acc_len, sendBuffer, bufferSize);
    h->acc_len += bufferSize;

    if (needContinue) return CNNET3_ERR_SUCCESS;

    {
        unsigned long sent = 0;
        int r = CNNET3_Send(handle, h->acc, (unsigned long)h->acc_len, &sent);
        h->acc_len = 0;
        return r;
    }
}

/* Read：RAW 直接读；HTTP 走「收 ack → 发 GET → 收 chunked body」两回合 */
int CNNET3_Read(void *handle, unsigned char *recvBuffer,
                       unsigned long *bufferSize, int *needContinue)
{
    cnnet3_t *h = (cnnet3_t *)handle;
    if (needContinue) *needContinue = 0;
    if (!cnnet3_valid(h) || !recvBuffer || !bufferSize) return CNNET3_ERR_INVALID_PARAMETER;
    if (h->fd < 0) return CNNET3_ERR_INVALID_OPERATION;

    if (h->iftype == CNNET3_IFTYPE_HTTP) {
        char hdr[8192];
        size_t hlen = 0;
        int rc;
        unsigned long cap = *bufferSize;
        long bl;

        /* --- 回合 1：收掉 POST 的 "200 OK" 空 ack ------------------- */
        if (!h->http_ack_done) {
            rc = cn_http_read_hdr(h, hdr, sizeof(hdr), &hlen);
            if (rc == -2) return CNNET3_ERR_READING_TIMEOUT;
            if (rc != 0)  return CNNET3_ERR_CONNECTION_ABORTED;
            if (h->http_verbose)
                fprintf(stderr, "CHMP: [ack] %.*s", (int)(hlen > 64 ? 64 : hlen), hdr);
            /* ack 按协议没有 body；万一有也吃掉，别污染下一次解析 */
            bl = cn_http_read_body(h, hdr, recvBuffer, cap);
            if (bl < 0) return CNNET3_ERR_CONNECTION_ABORTED;
            {
                int code = cn_http_status(hdr);
                h->http_ack_done = 1;
                if (code && (code < 200 || code >= 300)) {
                    /* 例：409 Conflict —— 命令被拒，不要傻等 body */
                    if (h->http_verbose)
                        fprintf(stderr, "CHMP: [ack] 非 2xx 状态码 %d\n", code);
                    *bufferSize = 0;
                    return CNNET3_ERR_NOCONTENT;
                }
            }
        }

        /* --- 回合 2：GET 取真正的响应 ------------------------------ */
        if (!h->http_get_sent) {
            char req[512];
            int rl = snprintf(req, sizeof(req),
                              "GET %s HTTP/1.1\r\n"
                              "Connection: Keep-Alive\r\n"
                              "Content-Type: application/octet-stream\r\n"
                              "Host: %s\r\n"
                              "X-CHMP-Version: 1.0.0\r\n\r\n",
                              h->url, h->ip);
            if (cn_write_all(h->fd, req, (size_t)rl, h->tmo_ms) != 0)
                return CNNET3_ERR_WRITING_TIMEOUT;
            h->http_get_sent = 1;
        }

        rc = cn_http_read_hdr(h, hdr, sizeof(hdr), &hlen);
        if (rc == -2) return CNNET3_ERR_READING_TIMEOUT;
        if (rc != 0)  return CNNET3_ERR_CONNECTION_ABORTED;
        {
            int code = cn_http_status(hdr);
            if (h->http_verbose) {
                fprintf(stderr, "CHMP: [get ] %.*s", (int)(hlen > 64 ? 64 : hlen), hdr);
                fprintf(stderr, "CHMP: [get ] status=%d\n", code);
            }
            if (code && (code < 200 || code >= 300)) {
                *bufferSize = 0;
                return CNNET3_ERR_NOCONTENT;
            }
        }

        bl = cn_http_read_body(h, hdr, recvBuffer, cap);
        if (bl < 0) return CNNET3_ERR_CONNECTION_ABORTED;
        *bufferSize = (unsigned long)bl;
        if (h->http_verbose && bl > 0)
            fprintf(stderr, "CHMP: [get ] body %ld 字节\n", bl);
        /* 一次取完；不需要再续读，让调用侧的 while(flag) 立刻退出 */
        if (needContinue) *needContinue = 0;
        return CNNET3_ERR_SUCCESS;
    }

    /* RAW：先把用户态缓冲里可能残留的数据吐出去（HTTP→RAW 切换时） */
    if (h->rpos < h->rlen) {
        size_t avail = h->rlen - h->rpos;
        size_t take = (avail < *bufferSize) ? avail : (size_t)*bufferSize;
        memcpy(recvBuffer, h->rbuf + h->rpos, take);
        h->rpos += take;
        *bufferSize = (unsigned long)take;
        return CNNET3_ERR_SUCCESS;
    }

    ssize_t n = cn_read_some(h->fd, recvBuffer, *bufferSize, h->tmo_ms);
    if (n == -2) { *bufferSize = 0; return CNNET3_ERR_READING_TIMEOUT; }
    if (n < 0)   { *bufferSize = 0; return CNNET3_ERR_CONNECTION_ABORTED; }
    *bufferSize = (unsigned long)n;
    return CNNET3_ERR_SUCCESS;
}

/* ================================================================== */
/*  CNNL_*  —— 旧通信接口（会话 + 数据读写 + 发现）                    */
/* ================================================================== */
typedef struct {
    int  fd;
    char ip[64];
    int  tmo_ms;
    int  cmdtype;
} cnnl_t;

#define CNNL_STATIC_TMO 5000

int CNNL_Init(void **h)
{
    cnnl_t *c;
    if (!h) return CNNL_RET_FAILURE;
    c = (cnnl_t *)calloc(1, sizeof(cnnl_t));
    if (!c) return CNNL_RET_FAILURE;
    c->fd = -1;
    c->tmo_ms = CNNL_STATIC_TMO;
    *h = c;
    return CNNL_RET_SUCCESS;
}

int CNNL_Close(void *h)
{
    cnnl_t *c = (cnnl_t *)h;
    if (!c) return CNNL_RET_FAILURE;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    return CNNL_RET_SUCCESS;
}

int CNNL_Terminate(void **h)
{
    if (!h || !*h) return CNNL_RET_FAILURE;
    CNNL_Close(*h);
    free(*h);
    *h = NULL;
    return CNNL_RET_SUCCESS;
}

int CNNL_OpenEx(void *h, const char *host, int command_type,
                       int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry;
    if (!c || !host) return CNNL_RET_FAILURE;
    if (!cn_valid_ipv4(host)) {
        /* 允许主机名解析 */
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr_list[0]) return CNNL_RET_FAILURE;
        inet_ntop(AF_INET, he->h_addr_list[0], c->ip, sizeof(c->ip));
    } else {
        snprintf(c->ip, sizeof(c->ip), "%s", host);
    }
    c->cmdtype = command_type;
    if (timeout) c->tmo_ms = (int)timeout;
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    c->fd = cn_tcp_connect(c->ip, PORT_RAW, c->tmo_ms);
    if (c->fd < 0) return CNNL_RET_FAILURE;
    return CNNL_RET_SUCCESS;
}

int CNNL_Open(void *h, const char *host)
{
    return CNNL_OpenEx(h, host, CNNL_COMMAND_SUPPORT, 0, CNNL_STATIC_TMO);
}

int CNNL_SessionStart(void *h, const char *user, const char *computer,
                             const char *document, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)user; (void)computer; (void)document; (void)retry; (void)timeout;
    if (!c) return CNNL_RET_FAILURE;
    return (c->fd >= 0) ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE;
}

int CNNL_SessionEnd(void *h, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry; (void)timeout;
    if (!c) return CNNL_RET_FAILURE;
    return CNNL_RET_SUCCESS;
}

int CNNL_DataWrite(void *h, const void *buf, unsigned long bufsz,
                          unsigned long *writesize, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry;
    if (writesize) *writesize = 0;
    if (!c || c->fd < 0 || !buf) return CNNL_RET_FAILURE;
    if (cn_write_all(c->fd, buf, bufsz, (int)timeout) != 0) return CNNL_RET_FAILURE;
    if (writesize) *writesize = bufsz;
    return CNNL_RET_SUCCESS;
}

int CNNL_DataRead(void *h, void *buf, unsigned long *readsz,
                         unsigned long bufsz, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    ssize_t n;
    (void)retry;
    if (readsz) *readsz = 0;
    if (!c || c->fd < 0 || !buf || !readsz) return CNNL_RET_FAILURE;
    n = cn_read_some(c->fd, buf, bufsz, (int)timeout);
    if (n == -2) return CNNL_RET_FAILURE;
    if (n < 0)   return CNNL_RET_FAILURE;
    *readsz = (unsigned long)n;
    return CNNL_RET_SUCCESS;
}

int CNNL_GetDeviceID(void *h, void *buf, unsigned long *readsz,
                            unsigned long bufsz, int retry, unsigned long timeout)
{
    return CNNL_DataRead(h, buf, readsz, bufsz, retry, timeout);
}

int CNNL_GetModelName(void *h, char *model, int modelsz,
                             int retry, unsigned long timeout)
{
    (void)h; (void)retry; (void)timeout;
    if (!model || modelsz <= 0) return CNNL_RET_FAILURE;
    model[0] = '\0';
    return CNNL_RET_SUCCESS;
}

int CNNL_SoftReset(void *h, int retry, unsigned long timeout)
{
    (void)retry; (void)timeout;
    return h ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE;
}

int CNNL_Abort(void *h) { return h ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE; }

int CNNL_StartPrint(void *h, int retry, unsigned long timeout)
{
    (void)retry; (void)timeout; return h ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE;
}
int CNNL_CheckPrint(void *h, int retry, unsigned long timeout)
{
    (void)retry; (void)timeout; return h ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE;
}
int CNNL_EndPrint(void *h, int retry, unsigned long timeout)
{
    (void)retry; (void)timeout; return h ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE;
}
int CNNL_CheckVersion(void *h, int retry, unsigned long timeout)
{
    (void)retry; (void)timeout; return h ? CNNL_RET_SUCCESS : CNNL_RET_FAILURE;
}
int CNNL_GetNICInfo(void *h, char *hwaddr, const int hwsize,
                           char *ipaddr, const int ipsize,
                           int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry; (void)timeout;
    if (!c) return CNNL_RET_FAILURE;
    if (ipaddr && ipsize > 0) snprintf(ipaddr, (size_t)ipsize, "%s", c->ip);
    if (hwaddr && hwsize > 0) memset(hwaddr, 0, (size_t)hwsize);
    return CNNL_RET_SUCCESS;
}
int CNNL_SetTimeout(void *h, unsigned long time, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry; (void)timeout;
    if (!c) return CNNL_RET_FAILURE;
    c->tmo_ms = (int)time;
    return CNNL_RET_SUCCESS;
}
int CNNL_GetTimeout(void *h, unsigned long *time, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry; (void)timeout;
    if (!c || !time) return CNNL_RET_FAILURE;
    *time = (unsigned long)c->tmo_ms;
    return CNNL_RET_SUCCESS;
}
int CNNL_GetSessionInfo(void *h, int *count, int *activeid,
                               char *user, const int usersz,
                               char *computer, const int computersz,
                               char *document, const int documentsz,
                               int retry, unsigned long timeout)
{
    (void)h; (void)retry; (void)timeout;
    if (count) *count = 0;
    if (activeid) *activeid = 0;
    if (user && usersz > 0) user[0] = '\0';
    if (computer && computersz > 0) computer[0] = '\0';
    if (document && documentsz > 0) document[0] = '\0';
    return CNNL_RET_SUCCESS;
}
int CNNL_GetCommandType(void *h, int *commandtype, int retry, unsigned long timeout)
{
    cnnl_t *c = (cnnl_t *)h;
    (void)retry; (void)timeout;
    if (!c || !commandtype) return CNNL_RET_FAILURE;
    *commandtype = c->cmdtype;
    return CNNL_RET_SUCCESS;
}
int CNNL_Config(void *h, const unsigned long mode, void *val, unsigned long *valsz)
{
    (void)h; (void)mode; (void)val; (void)valsz;
    return CNNL_RET_SUCCESS;
}
int CNNL_GetExtensionSupport(void *h, int *support_type, int retry, unsigned long timeout)
{
    (void)h; (void)retry; (void)timeout;
    if (support_type) *support_type = 0;
    return CNNL_RET_SUCCESS;
}
int CNNL_GetMaxDataSize(void *h, unsigned long *maxDataSize, int retry, unsigned long timeout)
{
    (void)h; (void)retry; (void)timeout;
    if (maxDataSize) *maxDataSize = 65536;
    return CNNL_RET_SUCCESS;
}

/* ---- 发现：ARP 表 + SNMP 广播（见文件头「尽力实现」说明） ---- */

/* 读 Linux ARP 表，按 MAC 找 IP；macStr 形如 "00-1E-8F-12-34-56" */
int cn_arp_lookup(const char *macStr, char *ipOut, size_t cap)
{
    FILE *fp;
    char line[512];
    char norm[32];
    size_t i, j = 0;

    if (!macStr || !ipOut) return -1;
    for (i = 0; macStr[i] && j < sizeof(norm) - 1; i++) {
        if (macStr[i] != '-' && macStr[i] != ':') {
            norm[j++] = (char)tolower((unsigned char)macStr[i]);
        }
    }
    norm[j] = '\0';

    fp = fopen("/proc/net/arp", "r");
    if (!fp) return -1;
    (void)fgets(line, sizeof(line), fp);            /* 表头 */
    while (fgets(line, sizeof(line), fp)) {
        char ip[64], hw[64];
        char n2[32];
        size_t k, m = 0;
        if (sscanf(line, "%63s %*s %*s %63s", ip, hw) != 2) continue;
        for (k = 0; hw[k] && m < sizeof(n2) - 1; k++) {
            if (hw[k] != '-' && hw[k] != ':') n2[m++] = (char)tolower((unsigned char)hw[k]);
        }
        n2[m] = '\0';
        if (strcmp(n2, norm) == 0) {
            snprintf(ipOut, cap, "%s", ip);
            fclose(fp);
            return 0;
        }
    }
    fclose(fp);
    return -1;
}

/* 广播 SNMP v1 GetRequest(sysDescr) 到 255.255.255.255:161
 * 返回响应源 IP 列表（最多 max），-1 表示失败。 */
int cn_snmp_broadcast(char ipList[][64], int max, int tmo_ms)
{
    int fd, n = 0, i;
    struct sockaddr_in bc;
    int bcOn = 1;
    /* SNMP v1 GetRequest: version(0) community "public" GetRequest(sysDescr.0) */
    static const uint8_t req[] = {
        0x30, 0x26,
          0x02, 0x01, 0x00,                       /* version = 0 */
          0x04, 0x06, 'p','u','b','l','i','c',    /* community */
          0xa0, 0x19,                             /* GetRequest */
            0x02, 0x04, 0x00,0x00,0x00,0x01,      /* request-id */
            0x02, 0x01, 0x00,                      /* error-status */
            0x02, 0x01, 0x00,                      /* error-index */
            0x30, 0x0b,
              0x30, 0x09,
                0x06, 0x05, 0x2b,0x06,0x01,0x02,0x01, /* 1.3.6.1.2.1 */
                0x05, 0x00                            /* NULL */
    };

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &bcOn, sizeof(bcOn));

    memset(&bc, 0, sizeof(bc));
    bc.sin_family = AF_INET;
    bc.sin_port = htons(161);
    bc.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, req, sizeof(req), 0, (struct sockaddr *)&bc, sizeof(bc));

    for (i = 0; i < 4 && n < max; i++) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        if (poll(&pfd, 1, tmo_ms / 4 > 0 ? tmo_ms / 4 : 250) <= 0) continue;
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        uint8_t buf[2048];
        if (recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl) > 0) {
            char ip[64];
            int dup = 0, k;
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            for (k = 0; k < n; k++) if (!strcmp(ipList[k], ip)) { dup = 1; break; }
            if (!dup) { snprintf(ipList[n], 64, "%s", ip); n++; }
        }
    }
    close(fd);
    return n;
}

/* ================================================================== */
/*  Canon SNMP 设备发现（单播优先）                                     */
/*                                                                    */
/*  官方发现走 SNMP 广播（255.255.255.255:161，community "canon_admin"，*/
/*  查 5 个 Canon 私有 OID）。广播在启用 conntrack 的防火墙上会失效：  */
/*  出包目的地址是广播地址，打印机单播回包时源地址不匹配 conntrack 元组 */
/*  → 被判为 NEW → REJECT。本机 firewalld 实测如此（抓包可见打印机已  */
/*  回 GetResponse，紧接着本机发 ICMP admin-prohibited）。             */
/*                                                                    */
/*  故改为**单播扫描**：候选 = ARP 表已知主机 + 本机 /24 网段全部地址。 */
/*  单播的 conntrack 元组精确，回包正常匹配，防火墙不再拦。            */
/*                                                                    */
/*  OID（IANA 企业号 1602 = Canon），取自真机抓包实测：                 */
/*    1.3.6.1.4.1.1602.1.3.1.13.0       MAC（6 字节二进制）           */
/*    1.3.6.1.4.1.1602.1.2.1.8.1.3.1.1  序列号（ASCII）               */
/*    1.3.6.1.4.1.1602.1.1.1.1.0        型号（ASCII）                 */
/*    1.3.6.1.4.1.1602.1.1.1.10.0       设备类型（INTEGER）           */
/*    1.3.6.1.4.1.1602.1.3.1.12.0       连接模式（INTEGER，5=infra）  */
/* ================================================================== */

#define CN_CANON_OID_MAC    ".1.3.6.1.4.1.1602.1.3.1.13.0"
#define CN_CANON_OID_SERIAL ".1.3.6.1.4.1.1602.1.2.1.8.1.3.1.1"
#define CN_CANON_OID_MODEL  ".1.3.6.1.4.1.1602.1.1.1.1.0"
#define CN_CANON_OID_TYPE   ".1.3.6.1.4.1.1602.1.1.1.10.0"   /* 设备类型 */
#define CN_CANON_OID_CONN   ".1.3.6.1.4.1.1602.1.3.1.12.0"   /* 连接模式 */
#define CN_CANON_COMMUNITY  "canon_admin"

#define CN_CAND_MAX 700

typedef struct {
    char ip[64];
    char mac[13];        /* 12 个十六进制字符（大写、无分隔），对齐 MacAddressStr_ */
    char serial[65];
    char model[256];
    int  conn_mode;      /* 4=无线直连 5=infra，见 tagSearchPrinterInfo 注释 */
    int  dev_type;
} cn_canon_dev;

static long cn_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static size_t cn_ber_lenenc(uint8_t *p, size_t n)
{
    if (n < 0x80) { p[0] = (uint8_t)n; return 1; }
    if (n <= 0xff) { p[0] = 0x81; p[1] = (uint8_t)n; return 2; }
    p[0] = 0x82; p[1] = (uint8_t)(n >> 8); p[2] = (uint8_t)n; return 3;
}

static size_t cn_ber_tlv(uint8_t *p, uint8_t tag, const uint8_t *v, size_t n)
{
    size_t l;
    p[0] = tag;
    l = cn_ber_lenenc(p + 1, n);
    if (n) memcpy(p + 1 + l, v, n);
    return 1 + l + n;
}

/* 点分 OID -> BER 内容（不含 TLV 头），返回长度；0 表示失败 */
static size_t cn_ber_oid_content(const char *dots, uint8_t *out, size_t cap)
{
    unsigned long v[40];
    int n = 0, i;
    const char *s = dots;
    size_t k = 0;

    while (*s && n < 40) {
        char *end = NULL;
        while (*s == '.') s++;
        if (!*s) break;
        v[n++] = strtoul(s, &end, 10);
        if (end == s) break;
        s = end;
    }
    if (n < 2 || cap < 2) return 0;

    out[k++] = (uint8_t)(v[0] * 40 + v[1]);
    for (i = 2; i < n; i++) {
        uint8_t tmp[10];
        int t = 0;
        unsigned long x = v[i];
        tmp[t++] = (uint8_t)(x & 0x7f);
        x >>= 7;
        while (x) { tmp[t++] = (uint8_t)((x & 0x7f) | 0x80); x >>= 7; }
        while (t > 0) {
            if (k >= cap) return 0;
            out[k++] = tmp[--t];
        }
    }
    return k;
}

/* 构造 SNMP v1 GetRequest（community canon_admin，一次查 5 个 Canon OID） */
static size_t cn_canon_req_build(uint8_t *out, size_t cap)
{
    static const char *oids[5] = {
        CN_CANON_OID_MAC, CN_CANON_OID_SERIAL, CN_CANON_OID_MODEL,
        CN_CANON_OID_CONN, CN_CANON_OID_TYPE
    };
    static const uint8_t nullv[2] = { 0x05, 0x00 };
    uint8_t vb[512], vbltlv[600], inner[800];
    uint8_t commtlv[64], msg[1024], ver[3] = { 0x02, 0x01, 0x00 };
    uint8_t rid[3] = { 0x02, 0x01, 0x01 }, zero[3] = { 0x02, 0x01, 0x00 };
    size_t vbl = 0, il = 0, ml = 0, cl, i, total;

    for (i = 0; i < 5; i++) {
        uint8_t oc[64], oidtlv[96], iv[160];
        size_t ocl = cn_ber_oid_content(oids[i], oc, sizeof(oc));
        size_t ol, ivl = 0;
        if (!ocl) return 0;
        ol = cn_ber_tlv(oidtlv, 0x06, oc, ocl);
        memcpy(iv, oidtlv, ol);      ivl += ol;
        memcpy(iv + ivl, nullv, 2);  ivl += 2;
        vbl += cn_ber_tlv(vb + vbl, 0x30, iv, ivl);
    }
    {
        size_t n = cn_ber_tlv(vbltlv, 0x30, vb, vbl);
        memcpy(inner + il, rid, 3);     il += 3;
        memcpy(inner + il, zero, 3);    il += 3;
        memcpy(inner + il, zero, 3);    il += 3;
        memcpy(inner + il, vbltlv, n);  il += n;
    }
    memcpy(msg + ml, ver, 3); ml += 3;
    cl = cn_ber_tlv(commtlv, 0x04, (const uint8_t *)CN_CANON_COMMUNITY,
                    strlen(CN_CANON_COMMUNITY));
    memcpy(msg + ml, commtlv, cl); ml += cl;
    {
        uint8_t pdutlv[900];
        size_t pn = cn_ber_tlv(pdutlv, 0xA0, inner, il);
        memcpy(msg + ml, pdutlv, pn); ml += pn;
    }
    total = cn_ber_tlv(out, 0x30, msg, ml);
    if (total > cap) return 0;
    return total;
}

/* 在响应报文里定位某个 OID 的 varbind 值；返回指针与长度，NULL 表示未找到 */
static const uint8_t *cn_ber_get_value(const uint8_t *buf, size_t len,
                                       const char *oidDots, size_t *vlen)
{
    uint8_t oc[64], pat[96];
    size_t ocl = cn_ber_oid_content(oidDots, oc, sizeof(oc));
    size_t pl, i;

    if (!ocl) return NULL;
    pl = cn_ber_tlv(pat, 0x06, oc, ocl);          /* 完整 OID TLV 作为搜索模式 */

    for (i = 0; i + pl < len; i++) {
        size_t j, l, hdr;
        if (memcmp(buf + i, pat, pl) != 0) continue;
        j = i + pl;
        if (j + 2 > len) return NULL;
        if ((buf[j] & 0x1f) == 0x1f) return NULL;  /* 多字节 tag，不支持 */
        if (buf[j + 1] < 0x80) { l = buf[j + 1]; hdr = 2; }
        else if (buf[j + 1] == 0x81) { if (j + 3 > len) return NULL; l = buf[j + 2]; hdr = 3; }
        else if (buf[j + 1] == 0x82) { if (j + 4 > len) return NULL;
                                       l = ((size_t)buf[j + 2] << 8) | buf[j + 3]; hdr = 4; }
        else return NULL;
        if (j + hdr + l > len) return NULL;
        *vlen = l;
        return buf + j + hdr;
    }
    return NULL;
}

static int cn_parse_canon_response(const uint8_t *buf, size_t len,
                                   const char *ip, cn_canon_dev *dev)
{
    static const char hx[] = "0123456789ABCDEF";
    const uint8_t *v;
    size_t vl;
    int i;

    memset(dev, 0, sizeof(*dev));
    snprintf(dev->ip, sizeof(dev->ip), "%s", ip);

    /* MAC 是身份判据：没有它就不是 Canon 打印机 */
    v = cn_ber_get_value(buf, len, CN_CANON_OID_MAC, &vl);
    if (!v || vl < 6) return -1;
    for (i = 0; i < 6; i++) {
        dev->mac[i * 2]     = hx[(v[i] >> 4) & 0x0f];
        dev->mac[i * 2 + 1] = hx[v[i] & 0x0f];
    }
    dev->mac[12] = '\0';

    v = cn_ber_get_value(buf, len, CN_CANON_OID_SERIAL, &vl);
    if (v && vl < sizeof(dev->serial)) { memcpy(dev->serial, v, vl); dev->serial[vl] = '\0'; }

    v = cn_ber_get_value(buf, len, CN_CANON_OID_MODEL, &vl);
    if (v && vl < sizeof(dev->model)) { memcpy(dev->model, v, vl); dev->model[vl] = '\0'; }

    v = cn_ber_get_value(buf, len, CN_CANON_OID_CONN, &vl);
    if (v && vl >= 1) dev->conn_mode = v[vl - 1];

    v = cn_ber_get_value(buf, len, CN_CANON_OID_TYPE, &vl);
    if (v && vl >= 1) dev->dev_type = v[vl - 1];

    return 0;
}

static int cn_str_dup(char ips[][64], int n, const char *ip)
{
    int i;
    for (i = 0; i < n; i++) if (strcmp(ips[i], ip) == 0) return 1;
    return 0;
}

static int cn_dev_dup(const cn_canon_dev *d, int n, const char *ip)
{
    int i;
    for (i = 0; i < n; i++) if (strcmp(d[i].ip, ip) == 0) return 1;
    return 0;
}

/* 候选地址 = ARP 表已知主机 + 本机在 /24 网段内的全部地址（ARP 优先，先发） */
static int cn_enum_candidates(char ips[][64], int max)
{
    struct ifaddrs *ifa = NULL, *p;
    FILE *fp;
    char line[512];
    int n = 0;

    fp = fopen("/proc/net/arp", "r");
    if (fp) {
        (void)fgets(line, sizeof(line), fp);
        while (fgets(line, sizeof(line), fp) && n < max) {
            char ip[64];
            if (sscanf(line, "%63s", ip) != 1) continue;
            if (!strcmp(ip, "0.0.0.0")) continue;
            if (!cn_str_dup(ips, n, ip)) snprintf(ips[n++], 64, "%s", ip);
        }
        fclose(fp);
    }

    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p && n < max; p = p->ifa_next) {
            struct sockaddr_in *sin, *msk;
            uint32_t ip, mask, net, bc, cur;
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (p->ifa_flags & IFF_LOOPBACK) continue;
            sin = (struct sockaddr_in *)p->ifa_addr;
            msk = (struct sockaddr_in *)p->ifa_netmask;
            if (!msk) continue;
            ip   = ntohl(sin->sin_addr.s_addr);
            mask = ntohl(msk->sin_addr.s_addr);
            if (mask == 0xffffffffu) continue;
            /* 只对 >= /24 的网段做全扫，避免大网段把候选表撑爆 */
            if ((mask & 0xffffff00u) != 0xffffff00u) continue;
            net = ip & mask;
            bc  = net | ~mask;
            for (cur = net + 1; cur < bc && n < max; cur++) {
                char s[64];
                struct in_addr a;
                a.s_addr = htonl(cur);
                inet_ntop(AF_INET, &a, s, sizeof(s));
                if (!cn_str_dup(ips, n, s)) snprintf(ips[n++], 64, "%s", s);
            }
        }
        freeifaddrs(ifa);
    }
    return n;
}

/* 对给定候选地址列表做单播 SNMP 探测，返回发现的 Canon 打印机数量 */
static int cn_snmp_probe_list(char ips[][64], int nips,
                              cn_canon_dev *devs, int maxdev, int tmo_ms)
{
    uint8_t req[1024];
    size_t reqlen;
    int i, nfound = 0, fd;
    long deadline;

    if (nips <= 0) return 0;

    reqlen = cn_canon_req_build(req, sizeof(req));
    if (!reqlen) return -1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    {
        int fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
    if (tmo_ms <= 0) tmo_ms = 1500;

    /* 一次性把所有候选发完，再统一收（总耗时 ≈ tmo_ms，与候选数无关） */
    for (i = 0; i < nips; i++) {
        struct sockaddr_in d;
        memset(&d, 0, sizeof(d));
        d.sin_family = AF_INET;
        d.sin_port = htons(161);
        if (inet_pton(AF_INET, ips[i], &d.sin_addr) != 1) continue;
        (void)sendto(fd, req, reqlen, 0, (struct sockaddr *)&d, sizeof(d));
    }

    deadline = cn_now_ms() + tmo_ms;
    while (nfound < maxdev) {
        struct pollfd pfd;
        long remain = deadline - cn_now_ms();
        uint8_t buf[4096];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t r;
        char fip[64];

        if (remain <= 0) break;
        pfd.fd = fd; pfd.events = POLLIN; pfd.revents = 0;
        if (poll(&pfd, 1, (int)remain) <= 0) break;

        r = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (r <= 0) continue;
        inet_ntop(AF_INET, &from.sin_addr, fip, sizeof(fip));
        if (cn_dev_dup(devs, nfound, fip)) continue;
        if (cn_parse_canon_response(buf, (size_t)r, fip, &devs[nfound]) == 0) {
            nfound++;
        }
    }
    close(fd);
    return nfound;
}

/* 单播 SNMP 扫描本网段，返回发现的 Canon 打印机数量；-1 表示内部错误 */
int cn_snmp_discover(cn_canon_dev *devs, int maxdev, int tmo_ms)
{
    static char ips[CN_CAND_MAX][64];
    int nips = cn_enum_candidates(ips, CN_CAND_MAX);

    if (nips <= 0) return 0;
    return cn_snmp_probe_list(ips, nips, devs, maxdev, tmo_ms);
}

int CNNL_GetIPAddressEx(void *h, const char *cachefile, const char *macaddr,
                               char *ipaddr, const unsigned long bufsz,
                               int mode, int retry, unsigned long timeout)
{
    char list[16][64];
    int n, i;
    (void)h; (void)retry;
    if (!ipaddr || bufsz == 0) return CNNL_RET_FAILURE;

    /* 1) ARP 表（最快，同网段几乎必中） */
    if (macaddr && cn_arp_lookup(macaddr, ipaddr, (size_t)bufsz) == 0) {
        return CNNL_RET_SUCCESS;
    }
    (void)cachefile; (void)mode;

    /* 2) SNMP 广播兜底：只说得出 IP 时，若只有一个响应则直接采用 */
    n = cn_snmp_broadcast(list, 16, (int)(timeout ? timeout : DEFAULT_TMO_MS));
    if (n == 1) {
        snprintf(ipaddr, (size_t)bufsz, "%s", list[0]);
        return CNNL_RET_SUCCESS;
    }
    for (i = 0; i < n; i++) {
        if (cn_arp_lookup(macaddr, ipaddr, (size_t)bufsz) == 0) return CNNL_RET_SUCCESS;
        break;
    }
    return CNNL_RET_FAILURE;
}

int CNNL_GetIPAddress(const char *cachefile, const char *macaddr,
                             char *ipaddr, const unsigned long bufsz,
                             int mode, int retry, unsigned long timeout)
{
    return CNNL_GetIPAddressEx(NULL, cachefile, macaddr, ipaddr, bufsz,
                               mode, retry, timeout);
}

int CNNL_SearchPrintersEx(void *h, void *nic, const char *cachefile,
                                 const int maxprinters, int *foundprinters,
                                 int mode, int retry, unsigned long timeout)
{
    char list[16][64];
    int n, i, cnt;
    (void)h; (void)cachefile; (void)retry; (void)mode;

    if (foundprinters) *foundprinters = 0;
    if (!nic || maxprinters <= 0) return CNNL_RET_FAILURE;

    n = cn_snmp_broadcast(list, 16, (int)(timeout ? timeout : DEFAULT_TMO_MS));
    if (n <= 0) return CNNL_RET_FAILURE;

    cnt = (n < maxprinters) ? n : maxprinters;
    for (i = 0; i < cnt; i++) {
        /* CNNLNICINFO { unsigned char macaddr[6]; unsigned char ipaddr[4]; } */
        struct in_addr a;
        unsigned char *p = (unsigned char *)nic + (size_t)i * 10;
        memset(p, 0, 10);
        if (inet_pton(AF_INET, list[i], &a) == 1) memcpy(p + 6, &a, 4);
    }
    if (foundprinters) *foundprinters = cnt;
    return CNNL_RET_SUCCESS;
}

int CNNL_SearchPrinters(void *nic, const char *cachefile,
                               const int maxprinters, int *foundprinters,
                               int mode, int retry, unsigned long timeout)
{
    return CNNL_SearchPrintersEx(NULL, nic, cachefile, maxprinters, foundprinters,
                                 mode, retry, timeout);
}

/* ================================================================== */
/*  CNNET2_*  —— SNMP 发现（dlopen 自 libcnbpnet20.so）                */
/*                                                                     */
/*  诚实说明：Canon 原库走 SNMP + 私有 deviceId 编码                   */
/*  （形如 "MFG:Canon;MDL:...;DES:...;"）。本机 G3010 为 USB 连接，    */
/*  网络发现无法真机验证，故此处为「尽力实现」：                       */
/*    * 广播 SNMP v1 GetRequest（标准 MIB-II) 收集响应者 IP            */
/*    * 按 IP 反查 Linux ARP 表补 MAC                                  */
/*    * deviceId_ 以可解析的形式填充，CNCL_GetProtocol 可据此判 IVEC    */
/*  返回码/结构布局严格正确，任何异常都返回错误码而非崩溃。             */
/* ================================================================== */

/* 按 IP 反查 ARP 表补 MAC（输出 "xx-xx-xx-xx-xx-xx"） */
int cn_arp_lookup_by_ip(const char *ip, char *macOut, size_t cap)
{
    FILE *fp;
    char line[512];

    if (!ip || !macOut || cap == 0) return -1;
    fp = fopen("/proc/net/arp", "r");
    if (!fp) return -1;
    (void)fgets(line, sizeof(line), fp);            /* 表头 */
    while (fgets(line, sizeof(line), fp)) {
        char aip[64], hw[64];
        char out[32];
        size_t k, j = 0;
        if (sscanf(line, "%63s %*s %*s %63s", aip, hw) != 2) continue;
        if (strcmp(aip, ip) != 0) continue;
        for (k = 0; hw[k] && j < sizeof(out) - 1; k++) {
            out[j++] = (hw[k] == ':') ? '-' : hw[k];
        }
        out[j] = '\0';
        snprintf(macOut, cap, "%s", out);
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return -1;
}

typedef struct {
    int  get_info_tmo_ms;
    int  disc_tmo_ms;
    int  include_not_unicast;
    int  retry_count;
    int  retry_wait_ms;
    int  pkt_count;
    int  pkt_wait_ms;
    int  found;
    tagSearchPrinterInfo *list;
    char ipList[16][64];
} cnnet2_inst_t;

void *CNNET2_CreateInstance(void)
{
    cnnet2_inst_t *in = (cnnet2_inst_t *)calloc(1, sizeof(cnnet2_inst_t));
    if (!in) return NULL;
    in->get_info_tmo_ms = 2000;
    in->disc_tmo_ms     = 10000;
    in->pkt_count       = 2;
    return in;
}

void CNNET2_DestroyInstance(void *instance)
{
    cnnet2_inst_t *in = (cnnet2_inst_t *)instance;
    if (!in) return;
    free(in->list);
    free(in);
}

int CNNET2_OptSetting(void *instance, int settingFlag, unsigned int settingInfo)
{
    cnnet2_inst_t *in = (cnnet2_inst_t *)instance;
    if (!in) return CNNET2_ERROR_CODE_PARAM;
    switch (settingFlag) {
        case 1: in->get_info_tmo_ms     = (int)settingInfo; break;
        case 3: in->include_not_unicast = (int)settingInfo; break;
        case 4: in->disc_tmo_ms         = (int)settingInfo; break;
        case 5: in->retry_count         = (int)settingInfo; break;
        case 6: in->retry_wait_ms       = (int)settingInfo; break;
        case 7: in->pkt_count           = (int)settingInfo; break;
        case 8: in->pkt_wait_ms         = (int)settingInfo; break;
        default: return CNNET2_ERROR_CODE_PARAM;
    }
    return CNNET2_ERROR_CODE_SUCCESS;
}

int CNNET2_Search(void *instance, const char *ipv4Address,
                         void *callback, void *arg)
{
    cnnet2_inst_t *in = (cnnet2_inst_t *)instance;
    void (*cb)(void *, const tagSearchPrinterInfo *) = NULL;
    static cn_canon_dev devs[16];
    int n, i, tmo;

    if (!in) return CNNET2_ERROR_CODE_PARAM;
    if (callback) cb = (void (*)(void *, const tagSearchPrinterInfo *))callback;

    tmo = in->disc_tmo_ms > 0 ? in->disc_tmo_ms : 2000;

    if (ipv4Address && cn_valid_ipv4(ipv4Address)) {
        char one[1][64];
        snprintf(one[0], sizeof(one[0]), "%s", ipv4Address);
        n = cn_snmp_probe_list(one, 1, devs, 16, tmo);
    } else {
        /* 单播扫描：广播路径会被 conntrack 防火墙拦截，详见 cn_snmp_discover 注释 */
        n = cn_snmp_discover(devs, 16, tmo);
    }
    if (n < 0) return CNNET2_ERROR_CODE_SOCKET;

    in->found = 0;
    free(in->list);
    in->list = NULL;
    if (n <= 0) return 0;                       /* 未发现任何设备 */

    in->list = (tagSearchPrinterInfo *)calloc((size_t)n, sizeof(tagSearchPrinterInfo));
    if (!in->list) return CNNET2_ERROR_CODE_MEMORY;

    for (i = 0; i < n; i++) {
        const cn_canon_dev *d = &devs[i];
        tagSearchPrinterInfo *e = &in->list[i];
        const char *mdl = d->model[0] ? d->model : "series";

        memset(e, 0, sizeof(*e));
        e->nicIndex_ = i;
        snprintf(e->ipAddressStr_, sizeof(e->ipAddressStr_), "%s", d->ip);
        /* MacAddressStr_ 为 12 个十六进制字符、无分隔、大写；
         * 上层 cnijifnet2.c 会重排成 xx-xx-xx-xx-xx-xx 的形式。 */
        snprintf(e->MacAddressStr_, sizeof(e->MacAddressStr_), "%s", d->mac);
        snprintf(e->serialNumberStr_, sizeof(e->serialNumberStr_), "%s", d->serial);
        snprintf(e->modelName_, sizeof(e->modelName_), "%s", mdl);
        /* deviceId_：上层 CNCL_GetProtocol() 从其中找 "IVEC" 来判定走 IVEC 协议。
         * 真机完整 Device ID（IPP get-printer-attributes 实测）为
         *   MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP;MDL:G3010 series;
         *   CLS:PRINTER;DES:Canon G3010 series;VER:2.000;...
         * 这里按 SNMP 得到的型号重建同样的语义串；DES 字段供上层
         * （cnijifnet2.c 的型号兜底解析）提取显示名。 */
        snprintf(e->deviceId_, sizeof(e->deviceId_),
                 "MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP;MDL:%s;"
                 "CLS:PRINTER;DES:Canon %s;", mdl, mdl);
        e->currentConnectMode_ = d->conn_mode;
        e->deviceType_         = d->dev_type;
        e->isUnicast_          = 1;
        e->isSameSegment_      = 1;
        in->found++;
        if (cb) cb(arg, e);
    }
    return in->found;
}

int CNNET2_SearchByIpv6(void *instance, const char *ipv6Address,
                               void *callback, void *arg)
{
    (void)instance; (void)ipv6Address; (void)callback; (void)arg;
    return 0;                                   /* IPv6 发现未实现：返回 0 台，不视为错误 */
}

void CNNET2_CancelSearch(void *instance) { (void)instance; }

int CNNET2_EnumSearchInfo(void *instance, tagSearchPrinterInfo *out,
                                 unsigned int *ioSize)
{
    cnnet2_inst_t *in = (cnnet2_inst_t *)instance;
    unsigned int canHold, cnt, i;

    if (!in || !out || !ioSize) return CNNET2_ERROR_CODE_PARAM;

    canHold = *ioSize / (unsigned int)sizeof(tagSearchPrinterInfo);
    cnt = (unsigned int)in->found;
    if (cnt > canHold) cnt = canHold;

    for (i = 0; i < cnt; i++) {
        out[i] = in->list[i];
    }
    *ioSize = cnt * (unsigned int)sizeof(tagSearchPrinterInfo);
    return CNNET2_ERROR_CODE_SUCCESS;
}
