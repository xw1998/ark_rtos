/**
* @file sockets.h
* @brief 宿主用例用的假 lwIP socket 层（只给 test_net_host.c 用）
* @details 为什么不用真 lwIP：被测的是 arkrtos_net.c 自己的那一层——请求槽
*          的归属与轮询语义、参数块校验、handle 表、任务回收。真 lwIP 在宿主
*          上要 tcpip 线程、sys_arch 与一块网卡，跑起来之后出错仍要先分清
*          "是内核这一层还是协议栈"，而这一层恰恰是唯一被本仓库改写过的。
*          所以这里只提供与 lwIP 同名同签名的入口，行为完全由用例摆布：
*          每个操作都能被强制失败，最后一个参数（地址/端口/fd/载荷）会被
*          记下来供断言，select 的就绪位也可以直接摆。
*
*          这个文件只服务于宿主用例，**不参与任何板级构建**，也不代表
*          lwIP 的语义。用例里明确写了"没验证"的那几项（真协议栈、真网卡、
*          真时序）都在真 lwIP 那一侧。
*
* @note 只在 -Ikernelsrc/tests/net_stub 时可见；生产构建的搜索路径里没有它。
* @author xw
*/
#ifndef ARK_FAKE_LWIP_SOCKETS_H
#define ARK_FAKE_LWIP_SOCKETS_H

#include <errno.h>
#include <stddef.h>

/* lwIP 的基础类型（只到够编译 arkrtos_net.c 的程度）。 */
typedef unsigned char  u8_t;
typedef unsigned short u16_t;
typedef unsigned int   u32_t;
typedef unsigned int   socklen_t;
typedef unsigned short sa_family_t;

#ifndef AF_INET
#define AF_INET              2
#endif
#define SOCK_STREAM          1
#define SOCK_DGRAM           2

#define SOL_SOCKET           0x0fff
#define SO_REUSEADDR         0x0004
#define SO_SNDBUF            0x1001
#define SO_RCVBUF            0x1002
#define SO_ERROR             0x1007

#define F_SETFL              4
#define O_NONBLOCK           0x0004

struct in_addr {
    u32_t s_addr;
};

struct sockaddr {
    u8_t        sa_len;
    sa_family_t sa_family;
    char        sa_data[14];
};

struct sockaddr_in {
    u8_t             sin_len;
    sa_family_t      sin_family;
    u16_t            sin_port;
    struct in_addr   sin_addr;
    char             sin_zero[8];
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

/* 只做"位在不在"的集合：fake fd 从 3 起、最多 8 个，一位一字节足够。 */
#define ARK_FAKE_FD_MAX      8
typedef struct ark_fake_fd_set {
    unsigned char bit[ARK_FAKE_FD_MAX];
} fd_set;

#define FD_ZERO(set)        do { int i_; for (i_ = 0; i_ < ARK_FAKE_FD_MAX; i_++) { \
                                     (set)->bit[i_] = 0u; } } while (0)
#define FD_SET(fd, set)     do { (set)->bit[(fd) % ARK_FAKE_FD_MAX] = 1u; } while (0)
#define FD_ISSET(fd, set)   ((set)->bit[(fd) % ARK_FAKE_FD_MAX] != 0u)
#define FD_CLR(fd, set)     do { (set)->bit[(fd) % ARK_FAKE_FD_MAX] = 0u; } while (0)

/* lwIP 的错误码在宿主上可能缺失（MinGW 的 errno.h 只给一部分），
 * 定义成互不相同的值即可——arkrtos_net_from_errno() 是 switch，重复值编不过。 */
#ifndef EWOULDBLOCK
#define EWOULDBLOCK          140
#endif
#ifndef EINPROGRESS
#define EINPROGRESS          141
#endif
#ifndef EALREADY
#define EALREADY             142
#endif
#ifndef EISCONN
#define EISCONN              143
#endif
#ifndef ENOTCONN
#define ENOTCONN             144
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED         145
#endif
#ifndef ECONNRESET
#define ECONNRESET           146
#endif
#ifndef ECONNABORTED
#define ECONNABORTED         147
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH         148
#endif
#ifndef EMSGSIZE
#define EMSGSIZE             149
#endif
#ifndef ENOBUFS
#define ENOBUFS              150
#endif

/* ---------------------------------------------------------------- 假实现 */

/* 每个操作一个失败开关：置 1 时该入口返回 -1 并把 errno 设成 ark_fake_errno。 */
enum {
    ARK_FAKE_SOCKET = 0,
    ARK_FAKE_FCNTL,
    ARK_FAKE_BIND,
    ARK_FAKE_LISTEN,
    ARK_FAKE_ACCEPT,
    ARK_FAKE_CONNECT,
    ARK_FAKE_SEND,
    ARK_FAKE_RECV,
    ARK_FAKE_SENDTO,
    ARK_FAKE_RECVFROM,
    ARK_FAKE_CLOSE,
    ARK_FAKE_SETSOCKOPT,
    ARK_FAKE_GETSOCKOPT,
    ARK_FAKE_GETPEERNAME,
    ARK_FAKE_GETSOCKNAME,
    ARK_FAKE_SHUTDOWN,
    ARK_FAKE_SELECT,
    ARK_FAKE_OP_COUNT
};

/* 假实现的状态（定义在 test_net_host.c 里，用例直接读写）。 */
extern int   ark_fake_fail[ARK_FAKE_OP_COUNT];
extern int   ark_fake_errno;
extern int   ark_fake_so_error;
extern int   ark_fake_next_fd;
extern int   ark_fake_last_fd;
extern int   ark_fake_last_domain;
extern int   ark_fake_last_type;
extern int   ark_fake_last_proto;
extern int   ark_fake_last_level;
extern int   ark_fake_last_optname;
extern int   ark_fake_last_option;
extern int   ark_fake_last_flags;
extern u32_t ark_fake_last_ip;
extern u32_t ark_fake_last_port;
extern u32_t ark_fake_peer_ip;
extern u32_t ark_fake_peer_port;
extern int   ark_fake_ready_in;
extern int   ark_fake_ready_out;
extern int   ark_fake_ready_err;
extern int   ark_fake_select_maxfd;
extern int   ark_fake_close_count;
extern int   ark_fake_fcntl_count;
extern const void *ark_fake_last_buf;
extern u32_t ark_fake_last_len;
extern int   ark_fake_rx_len;
extern char  ark_fake_rx_payload[64];

void ark_fake_reset(void);

int  lwip_socket(int domain, int type, int protocol);
int  lwip_fcntl(int s, int cmd, int val);
int  lwip_bind(int s, const struct sockaddr *name, socklen_t namelen);
int  lwip_listen(int s, int backlog);
int  lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen);
int  lwip_connect(int s, const struct sockaddr *name, socklen_t namelen);
int  lwip_send(int s, const void *dataptr, size_t size, int flags);
int  lwip_recv(int s, void *mem, size_t len, int flags);
int  lwip_sendto(int s, const void *dataptr, size_t size, int flags,
                 const struct sockaddr *to, socklen_t tolen);
int  lwip_recvfrom(int s, void *mem, size_t len, int flags,
                   struct sockaddr *from, socklen_t *fromlen);
int  lwip_close(int s);
int  lwip_setsockopt(int s, int level, int optname, const void *optval,
                     socklen_t optlen);
int  lwip_getsockopt(int s, int level, int optname, void *optval,
                     socklen_t *optlen);
int  lwip_getpeername(int s, struct sockaddr *name, socklen_t *namelen);
int  lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen);
int  lwip_shutdown(int s, int how);
int  lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                 fd_set *exceptset, struct timeval *timeout);

#endif /* ARK_FAKE_LWIP_SOCKETS_H */
