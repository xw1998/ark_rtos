/**
* @file test_net_host.c
* @brief arkrtos_net.c 的宿主用例：请求槽 + socket 表，假 lwIP 驱动
* @details 被验的是内核这一层自己的语义，不是"协议栈能不能连通"：
*            1. 未起来 / 懒建立 / 幂等 start / 队列被删后重建 / 建队列或建
*               任务失败的四条生命周期返回码；
*            2. SVC 永不阻塞：第一次请求回 EPENDING，同一个调用再来一次
*               取回结果；请求与结果按"整个参数块"配对，别人（或另一组
*               参数）来问一律 EBUSY，绝不把别人的答案递出去；
*            3. 参数块校验：子命令范围、socket 类型、handle 归属、listen
*               上限、send/recv 长度、读写窗口（读窗口与写窗口分开）、
*               recvfrom/getsockopt/getsockname 的输出指针、shutdown how、
*               poll 标志位；
*            4. 16 个子命令的成功路径：地址与端口确实按网络序进出、
*               accept 不继承非阻塞、accept 复用已释放的行、表满回
*               ENOBUFS 并把刚建的 fd 关掉、EISCONN 视为成功、关闭后
*               handle 立刻失效；
*            5. 归属回收：任务卸载把它的 socket 标成孤儿、把请求槽解除
*               归属，服务任务那一拍关 fd；请求槽在"主人已死"时丢弃结果
*               而不是递给下一个调用者。
*
*          lwIP 换成本文件的假实现（kernelsrc/tests/net_stub）：每个入口都能
*          被强制失败、最后一次参数会被记下来供断言。这样测的是"内核把什么
*          交给 lwIP、又把 lwIP 的什么交给 App"，而不是协议栈行为。
*
*          明确没验证：真 lwIP 的 socket 生命周期、真网卡与真时序、两块
*          板之间的连接、非阻塞 connect 的 EINPROGRESS 演进。那些只能上板。
*
*          $a0 语义：SVC 的 a0 装的是「参数块指针」，不是第一个参数。
*          宿主用 /d/mingw 的 32 位 MinGW（ILP32，指针 = long = 4 字节），
*          所以参数块指针可以原样塞进窄字，不会截断。
*          ARKRTOS_SVC_WIDE_ARGS 要求 __SIZEOF_LONG__ > 4，在宿主编译期
*          恒为 0：宽档分支（含 wide_ok 标记检查）在这里根本不会被编译，
*          由 64 位目标上板覆盖，此处不伪装覆盖。
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_features.h"
#include "arkrtos_net.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_kernel_check.h"
#include "arkrtos_hal.h"
#include "arkrtos_mq.h"

#if (ARKRTOS_USE_LWIP == 0)

/* 关档：四个入口都退化成"未编入"。这里只证明这一档仍可编可跑，且
 * 每个入口都给出诚实的"不支持"，而不是空成功。 */
int main(void)
{
    int fails = 0;

    if (arkrtos_net_start() != 0) {
        fails++;
        printf("FAIL line %d\n", __LINE__);
    }
    if (arkrtos_net_ready() != 0u) {
        fails++;
        printf("FAIL line %d\n", __LINE__);
    }
    if (arkrtos_net_svc(0) != ARKRTOS_NET_ENOTSUP) {
        fails++;
        printf("FAIL line %d\n", __LINE__);
    }
    arkrtos_net_release_task(1);
    printf("NET-HOST OFF OK  %d failed\n", fails);
    return (fails == 0) ? 0 : 1;
}

#else

#include "lwip/sockets.h"
#include "lwip/def.h"

/* ------------------------------------------------------------------ */
/* 内核桩：只提供链接所需的符号，行为尽量"什么都不做"                     */
/* ------------------------------------------------------------------ */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count      = 0;
int32  arkrtos_current_task_id = 1;

static int   g_lock_depth;
static int   g_unlock_calls;
static int   g_period_calls;

void arkrtos_sched_lock_internal(void) { g_lock_depth++; }
uint32 arkrtos_sched_unlock_internal(void) { g_unlock_calls++; return 0u; }
void arkrtos_port_dmb(void) { }
void arkrtos_task_wait_period_internal(void) { g_period_calls++; }

unsigned long arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
{
    return (unsigned long)(((arkrtos_svc_arg_t *)p_exc_ctx)[idx]);
}

void arkrtos_port_svc_set_ret(void *p_exc_ctx, unsigned long value)
{
    (void)p_exc_ctx;
    (void)value;
}

/* 任务注册：成功时把入口记进任务表，start() 的"服务任务已在"分支才有
 * 可观察的判据（它比的是 entry == arkrtos_net_task）。 */
static int32 g_reg_result = 2;
static void  (*g_reg_entry)(void);

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    (void)stack_bottom;
    (void)stack_size;
    (void)priority;
    (void)period_ms;
    g_reg_entry = entry;
    if (g_reg_result < 0) {
        return g_reg_result;
    }
    arkrtos_task_table[0].entry  = entry;
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;
    arkrtos_task_count = 1;
    return g_reg_result;
}

/* 消息队列：只当邮箱用。net.c 只用它投递两个单字消息。 */
static int32 g_mq_handle       = 7;
static int32 g_mq_create_result = 7;
static int   g_mq_alive        = 0;
static int   g_mq_send_fail    = 0;
static int   g_mq_deleted      = 0;

/* 必须是真队列，不是"最后一个消息槽"：release_task 会在一个请求还在飞时
 * 再投一条 REAP，而真板上的 mq 有深度，两条消息必须都留着、按序处理。
 * 用单槽会把 REQUEST 挤掉，测试就会测成"请求永远没人执行"——那不是内核
 * 的行为，而是桩的行为。 */
static uint32 g_mq_q[8];
static int    g_mq_head;
static int    g_mq_tail;
static uint32 g_mq_msg;                 /* 最后一条入队的消息，供断言 */

static void mq_clear(void)
{
    g_mq_head = 0;
    g_mq_tail = 0;
    g_mq_msg  = 0u;
}

static void mq_push(uint32 v)
{
    g_mq_msg = v;
    if (g_mq_tail < (int)(sizeof(g_mq_q) / sizeof(g_mq_q[0]))) {
        g_mq_q[g_mq_tail++] = v;
    }
}

static int mq_pop(uint32 *v)
{
    if (g_mq_head == g_mq_tail) {
        return -1;
    }
    *v = g_mq_q[g_mq_head++];
    if (g_mq_head == g_mq_tail) {
        g_mq_head = 0;
        g_mq_tail = 0;
    }
    return 0;
}

int32 arkrtos_mq_create_kernel_internal(const char *name)
{
    (void)name;
    if (g_mq_create_result < 0) {
        return g_mq_create_result;
    }
    g_mq_alive = 1;
    g_mq_deleted = 0;
    return g_mq_create_result;
}

int32 arkrtos_mq_is_alive(int32 handle)
{
    return (g_mq_alive != 0 && handle == g_mq_handle) ? 1 : 0;
}

int32 arkrtos_mq_send_internal(int32 handle, uint32 *buf, int32 len_words,
                               int32 timeout_ms)
{
    (void)handle;
    (void)len_words;
    (void)timeout_ms;
    if (g_mq_send_fail != 0) {
        return -1;
    }
    mq_push(*buf);
    return 0;
}

int32 arkrtos_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words,
                               int32 timeout_ms)
{
    (void)handle;
    (void)len_words;
    (void)timeout_ms;
    return mq_pop(buf);
}

int32 arkrtos_mq_delete_internal(int32 handle)
{
    (void)handle;
    g_mq_alive = 0;
    g_mq_deleted = 1;
    return 0;
}

/* 窗口检查：把"哪一个调用该被拒"做成计数器，而不是整体开关——recvfrom
 * 的第一个窗口是载荷（写）、后两个是输出地址，需要分别拒。 */
static int g_args_ok  = 1;
#if (ARKRTOS_SVC_WIDE_ARGS == 1)
static int g_wide_ok  = 1;
#endif
static int g_ro_ok    = 1;
static int g_dev_calls = 0;
static int g_dev_fail_at = 0;
static uint32 g_args_len = 0u;

uint8 arkrtos_kernel_svc_args_ok(const void *p, uint32 len)
{
    (void)p;
    g_args_len = len;
    return (g_args_ok != 0) ? 1u : 0u;
}

#if (ARKRTOS_SVC_WIDE_ARGS == 1)
uint8 arkrtos_kernel_svc_wide_ok(const void *p)
{
    (void)p;
    return (g_wide_ok != 0) ? 1u : 0u;
}
#endif

uint8 arkrtos_kernel_user_ro_ok(const void *p, uint32 len)
{
    (void)p;
    (void)len;
    return (g_ro_ok != 0) ? 1u : 0u;
}

uint8 arkrtos_kernel_dev_buf_ok(const void *p, int32 len)
{
    (void)p;
    (void)len;
    g_dev_calls++;
    if (g_dev_fail_at != 0 && g_dev_calls == g_dev_fail_at) {
        return 0u;
    }
    return 1u;
}

/* ------------------------------------------------------------------ */
/* 假 lwIP 的实现                                                       */
/* ------------------------------------------------------------------ */

int   ark_fake_fail[ARK_FAKE_OP_COUNT];
int   ark_fake_errno;
int   ark_fake_so_error;
int   ark_fake_next_fd;
int   ark_fake_last_fd;
int   ark_fake_last_domain;
int   ark_fake_last_type;
int   ark_fake_last_proto;
int   ark_fake_last_level;
int   ark_fake_last_optname;
int   ark_fake_last_option;
int   ark_fake_last_flags;
u32_t ark_fake_last_ip;
u32_t ark_fake_last_port;
u32_t ark_fake_peer_ip;
u32_t ark_fake_peer_port;
int   ark_fake_ready_in;
int   ark_fake_ready_out;
int   ark_fake_ready_err;
int   ark_fake_select_maxfd;
int   ark_fake_close_count;
int   ark_fake_fcntl_count;
const void *ark_fake_last_buf;
u32_t ark_fake_last_len;
int   ark_fake_rx_len;
char  ark_fake_rx_payload[64];

static int fake_fail(int op)
{
    if (ark_fake_fail[op] != 0) {
        errno = ark_fake_errno;
        return -1;
    }
    return 0;
}

static void fake_addr_out(struct sockaddr *sa, socklen_t *out_len)
{
    struct sockaddr_in sin;

    memset(&sin, 0, sizeof(sin));
    sin.sin_len    = (u8_t)sizeof(sin);
    sin.sin_family = (sa_family_t)AF_INET;
    sin.sin_port   = lwip_htons((u16_t)ark_fake_peer_port);
    sin.sin_addr.s_addr = lwip_htonl(ark_fake_peer_ip);
    memcpy(sa, &sin, sizeof(sin));
    *out_len = (socklen_t)sizeof(sin);
}

void ark_fake_reset(void)
{
    memset(ark_fake_fail, 0, sizeof(ark_fake_fail));
    ark_fake_errno = 0;
    ark_fake_so_error = 0;
    ark_fake_next_fd = 3;
    ark_fake_last_fd = -1;
    ark_fake_last_domain = 0;
    ark_fake_last_type = 0;
    ark_fake_last_proto = 0;
    ark_fake_last_level = 0;
    ark_fake_last_optname = 0;
    ark_fake_last_option = 0;
    ark_fake_last_flags = 0;
    ark_fake_last_ip = 0u;
    ark_fake_last_port = 0u;
    ark_fake_peer_ip = 0u;
    ark_fake_peer_port = 0u;
    ark_fake_ready_in = 0;
    ark_fake_ready_out = 0;
    ark_fake_ready_err = 0;
    ark_fake_select_maxfd = 0;
    ark_fake_close_count = 0;
    ark_fake_fcntl_count = 0;
    ark_fake_last_buf = 0;
    ark_fake_last_len = 0u;
    ark_fake_rx_len = 0;
    memset(ark_fake_rx_payload, 0, sizeof(ark_fake_rx_payload));
}

int lwip_socket(int domain, int type, int protocol)
{
    ark_fake_last_domain = domain;
    ark_fake_last_type = type;
    ark_fake_last_proto = protocol;
    if (fake_fail(ARK_FAKE_SOCKET) != 0) {
        return -1;
    }
    ark_fake_last_fd = ark_fake_next_fd;
    ark_fake_next_fd++;
    return ark_fake_last_fd;
}

int lwip_fcntl(int s, int cmd, int val)
{
    (void)s;
    (void)cmd;
    (void)val;
    ark_fake_fcntl_count++;
    return fake_fail(ARK_FAKE_FCNTL);
}

int lwip_bind(int s, const struct sockaddr *name, socklen_t namelen)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)name;

    (void)namelen;
    ark_fake_last_fd = s;
    ark_fake_last_ip = lwip_ntohl(sin->sin_addr.s_addr);
    ark_fake_last_port = (u32_t)lwip_ntohs(sin->sin_port);
    return fake_fail(ARK_FAKE_BIND);
}

int lwip_listen(int s, int backlog)
{
    (void)s;
    ark_fake_last_option = backlog;
    return fake_fail(ARK_FAKE_LISTEN);
}

int lwip_accept(int s, struct sockaddr *addr, socklen_t *addrlen)
{
    (void)s;
    (void)addr;
    (void)addrlen;
    if (fake_fail(ARK_FAKE_ACCEPT) != 0) {
        return -1;
    }
    ark_fake_last_fd = ark_fake_next_fd;
    ark_fake_next_fd++;
    return ark_fake_last_fd;
}

int lwip_connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)name;

    (void)namelen;
    ark_fake_last_fd = s;
    ark_fake_last_ip = lwip_ntohl(sin->sin_addr.s_addr);
    ark_fake_last_port = (u32_t)lwip_ntohs(sin->sin_port);
    return fake_fail(ARK_FAKE_CONNECT);
}

int lwip_send(int s, const void *dataptr, size_t size, int flags)
{
    ark_fake_last_fd = s;
    ark_fake_last_buf = dataptr;
    ark_fake_last_len = (u32_t)size;
    ark_fake_last_flags = flags;
    if (fake_fail(ARK_FAKE_SEND) != 0) {
        return -1;
    }
    return (int)size;
}

int lwip_recv(int s, void *mem, size_t len, int flags)
{
    int n = ark_fake_rx_len;

    ark_fake_last_fd = s;
    ark_fake_last_buf = mem;
    ark_fake_last_len = (u32_t)len;
    ark_fake_last_flags = flags;
    if (fake_fail(ARK_FAKE_RECV) != 0) {
        return -1;
    }
    if (n > (int)len) {
        n = (int)len;
    }
    if (n > 0) {
        memcpy(mem, ark_fake_rx_payload, (size_t)n);
    }
    return n;
}

int lwip_sendto(int s, const void *dataptr, size_t size, int flags,
                const struct sockaddr *to, socklen_t tolen)
{
    const struct sockaddr_in *sin = (const struct sockaddr_in *)to;

    (void)tolen;
    ark_fake_last_fd = s;
    ark_fake_last_buf = dataptr;
    ark_fake_last_len = (u32_t)size;
    ark_fake_last_flags = flags;
    ark_fake_last_ip = lwip_ntohl(sin->sin_addr.s_addr);
    ark_fake_last_port = (u32_t)lwip_ntohs(sin->sin_port);
    if (fake_fail(ARK_FAKE_SENDTO) != 0) {
        return -1;
    }
    return (int)size;
}

int lwip_recvfrom(int s, void *mem, size_t len, int flags,
                  struct sockaddr *from, socklen_t *fromlen)
{
    int n = ark_fake_rx_len;

    ark_fake_last_fd = s;
    ark_fake_last_buf = mem;
    ark_fake_last_len = (u32_t)len;
    ark_fake_last_flags = flags;
    if (fake_fail(ARK_FAKE_RECVFROM) != 0) {
        return -1;
    }
    if (n > (int)len) {
        n = (int)len;
    }
    if (n > 0) {
        memcpy(mem, ark_fake_rx_payload, (size_t)n);
    }
    fake_addr_out(from, fromlen);
    return n;
}

int lwip_close(int s)
{
    ark_fake_last_fd = s;
    ark_fake_close_count++;
    return fake_fail(ARK_FAKE_CLOSE);
}

int lwip_setsockopt(int s, int level, int optname, const void *optval,
                    socklen_t optlen)
{
    ark_fake_last_fd = s;
    ark_fake_last_level = level;
    ark_fake_last_optname = optname;
    if (optval != 0 && optlen == (socklen_t)sizeof(int)) {
        ark_fake_last_option = *(const int *)optval;
    }
    return fake_fail(ARK_FAKE_SETSOCKOPT);
}

int lwip_getsockopt(int s, int level, int optname, void *optval,
                    socklen_t *optlen)
{
    ark_fake_last_fd = s;
    ark_fake_last_level = level;
    ark_fake_last_optname = optname;
    if (fake_fail(ARK_FAKE_GETSOCKOPT) != 0) {
        return -1;
    }
    if (optval != 0 && *optlen >= (socklen_t)sizeof(int)) {
        *(int *)optval = ark_fake_so_error;
        *optlen = (socklen_t)sizeof(int);
    }
    return 0;
}

int lwip_getpeername(int s, struct sockaddr *name, socklen_t *namelen)
{
    ark_fake_last_fd = s;
    if (fake_fail(ARK_FAKE_GETPEERNAME) != 0) {
        return -1;
    }
    fake_addr_out(name, namelen);
    return 0;
}

int lwip_getsockname(int s, struct sockaddr *name, socklen_t *namelen)
{
    ark_fake_last_fd = s;
    if (fake_fail(ARK_FAKE_GETSOCKNAME) != 0) {
        return -1;
    }
    fake_addr_out(name, namelen);
    return 0;
}

int lwip_shutdown(int s, int how)
{
    ark_fake_last_fd = s;
    ark_fake_last_option = how;
    return fake_fail(ARK_FAKE_SHUTDOWN);
}

int lwip_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                fd_set *exceptset, struct timeval *timeout)
{
    int i;
    int n = 0;

    (void)timeout;
    ark_fake_select_maxfd = maxfdp1;
    if (fake_fail(ARK_FAKE_SELECT) != 0) {
        return -1;
    }
    for (i = 0; i < maxfdp1 && i < ARK_FAKE_FD_MAX; i++) {
        if (readset != 0 && FD_ISSET(i, readset)) {
            if (ark_fake_ready_in != 0) {
                n++;
            } else {
                FD_CLR(i, readset);
            }
        }
        if (writeset != 0 && FD_ISSET(i, writeset)) {
            if (ark_fake_ready_out != 0) {
                n++;
            } else {
                FD_CLR(i, writeset);
            }
        }
        if (exceptset != 0 && FD_ISSET(i, exceptset)) {
            if (ark_fake_ready_err != 0) {
                n++;
            } else {
                FD_CLR(i, exceptset);
            }
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* 断言与 SVC 驱动                                                       */
/* ------------------------------------------------------------------ */

static int g_checks;
static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

struct host_ctx {
    arkrtos_svc_arg_t arg[ARKRTOS_NET_ARG_WORDS];
    unsigned long     ret;
};

static struct host_ctx g_ctx;

/* SVC 的真实契约：a0 是「参数块指针」，内核把它当 arkrtos_svc_arg_t* 解引用
 * （见 arkrtos_task.c 的每个入口）。所以驱动分两层——g_block 是被指的块，
 * g_ctx.arg[0] 装它的地址，其余寄存器不使用。 */
static arkrtos_svc_arg_t g_block[ARKRTOS_NET_ARG_WORDS];

static void set_req(uint32 sub, arkrtos_svc_arg_t a1, arkrtos_svc_arg_t a2,
                    arkrtos_svc_arg_t a3, arkrtos_svc_arg_t a4,
                    arkrtos_svc_arg_t a5)
{
    memset(g_block, 0, sizeof(g_block));
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_block[0] = ARKRTOS_SVC_SUB(sub);
    g_block[1] = a1;
    g_block[2] = a2;
    g_block[3] = a3;
    g_block[4] = a4;
    g_block[5] = a5;
    g_ctx.arg[0] = (arkrtos_svc_arg_t)(uintptr_t)&g_block[0];
}

static int32 net_call(void)
{
    g_ctx.ret = 0u;
    return arkrtos_net_svc(&g_ctx);
}

/* 等一个已经发出去的请求落地：按 App 的真实做法轮询，同时让服务任务
 * 每次跑一拍。必须轮询而不是"一拍定生死"——队列里可能还压着别的消息
 * （比如 release_task 投的 REAP），服务任务按序处理，请求不一定是第一条。 */
static int32 net_wait(void)
{
    int   guard = 0;
    int32 r     = net_call();

    while ((r == ARKRTOS_NET_EPENDING) && (guard++ < 16)) {
        arkrtos_net_host_step();
        r = net_call();
    }
    return r;
}

/* 一次完整请求：发出 → 服务任务跑到有结果 → 同一个调用取回结果。 */
static int32 net_req(uint32 sub, arkrtos_svc_arg_t a1, arkrtos_svc_arg_t a2,
                     arkrtos_svc_arg_t a3, arkrtos_svc_arg_t a4,
                     arkrtos_svc_arg_t a5)
{
    set_req(sub, a1, a2, a3, a4, a5);
    return net_wait();
}

/* 复位到"服务已起来、表是空的、没有在飞的请求" */
static void reset_all(void)
{
    int i;

    arkrtos_current_task_id = 1;
    g_ctx.ret = 0u;
    g_args_ok = 1;
#if (ARKRTOS_SVC_WIDE_ARGS == 1)
    g_wide_ok = 1;
#endif
    g_ro_ok = 1;
    g_dev_calls = 0;
    g_dev_fail_at = 0;
    g_mq_send_fail = 0;
    mq_clear();
    g_reg_result = 2;
    g_mq_create_result = g_mq_handle;
    g_mq_alive = 0;
    g_lock_depth = 0;
    g_unlock_calls = 0;
    arkrtos_net_host_reset();
    arkrtos_task_count = 0;
    for (i = 0; i < ARKRTOS_TASK_MAX_NUM; i++) {
        arkrtos_task_table[i].entry = 0;
        arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
    }
    ark_fake_reset();
}

static void free_all_rows(void)
{
    uint32 i;

    for (i = 1u; i <= (uint32)ARKRTOS_NET_MAX_SOCKETS; i++) {
        if (arkrtos_net_host_sock_fd(i - 1u) >= 0) {
            (void)net_req(ARKRTOS_NET_SUB_CLOSE, i, 0u, 0u, 0u, 0u);
        }
    }
    arkrtos_net_host_step();            /* 把孤儿行也关掉 */
}

/* ------------------------------------------------------------------ */
/* 用例                                                                  */
/* ------------------------------------------------------------------ */

static void test_lifecycle(void)
{
    reset_all();

    CK(arkrtos_net_ready() == 0u);
    set_req(ARKRTOS_NET_SUB_QUERY, 0u, 0u, 0u, 0u, 0u);
    CK(net_call() == 0);                        /* 询问时还没起来 */

    CK(arkrtos_net_start() == 0);
    CK(arkrtos_net_ready() == 1u);
    CK(g_reg_entry != 0);
    CK(arkrtos_net_start() == 0);               /* 幂等 */
    CK(arkrtos_net_ready() == 1u);

    set_req(ARKRTOS_NET_SUB_QUERY, 0u, 0u, 0u, 0u, 0u);
    CK(net_call() == 1);
    CK(arkrtos_net_host_req_state() == 0u);     /* 查询不占请求槽 */

    /* 队列对象没了：start 重建队列，但服务任务还在，不重复登记。 */
    g_mq_alive = 0;
    arkrtos_task_count = 1;
    CK(arkrtos_net_start() == 0);
    CK(arkrtos_net_ready() == 1u);

    /* 建队列失败：返回负值、状态不变。 */
    g_mq_alive = 0;
    g_mq_create_result = -3;
    CK(arkrtos_net_start() == -3);
    CK(arkrtos_net_ready() == 0u);
    g_mq_create_result = g_mq_handle;

    /* 建任务失败：队列被删掉，服务保持"没起来"。 */
    g_mq_alive = 0;
    arkrtos_task_count = 0;
    arkrtos_task_table[0].entry = 0;
    arkrtos_task_table[0].status = ARKRTOS_TASK_INVALID;
    g_reg_result = -1;
    CK(arkrtos_net_start() == -1);
    CK(arkrtos_net_ready() == 0u);
    CK(g_mq_deleted == 1);
    g_reg_result = 2;

    CK(arkrtos_net_start() == 0);
    CK(arkrtos_net_ready() == 1u);

    /* 起来之前未就绪的拒答码。 */
    g_mq_alive = 0;
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM, 0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_ENOTSUP);
    g_mq_alive = 1;
    CK(arkrtos_net_host_req_state() == 0u);
}

static void test_svc_gates(void)
{
    reset_all();
    (void)arkrtos_net_start();

    /* 参数块本身：长度必须是 8 字（宽档 64 字节）。 */
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM, 0u, 0u, 0u, 0u);
    g_args_ok = 0;
    CK(net_call() == ARKRTOS_NET_EINVAL);
    CK(g_args_len == (uint32)ARKRTOS_SVC_ARG_BYTES(ARKRTOS_NET_ARG_WORDS * 4u));
    g_args_ok = 1;

    /* 宽档标记位检查只在 64 位目标上存在（ARKRTOS_SVC_WIDE_ARGS 由
     * __SIZEOF_LONG__ 推导）。宿主 MinGW 是 ILP32，长整型 4 字节，这一档
     * 编译期就是 0——所以这里按条件编，而不是假装能测。 */
#if (ARKRTOS_SVC_WIDE_ARGS == 1)
    g_wide_ok = 0;
    CK(net_call() == ARKRTOS_NET_EINVAL);
    g_wide_ok = 1;
#endif

    /* 子命令范围。 */
    set_req(17u, 0u, 0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EINVAL);

    /* socket 类型。 */
    set_req(ARKRTOS_NET_SUB_SOCKET, 3u, 0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EINVAL);

    /* handle 0 / 越界 / 不归自己。 */
    set_req(ARKRTOS_NET_SUB_BIND, 0u, 0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EINVAL);
    set_req(ARKRTOS_NET_SUB_BIND, 5u, 0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EINVAL);

    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 1);
    arkrtos_current_task_id = 2;
    CK(net_req(ARKRTOS_NET_SUB_BIND, 1u, 0u, 0u, 0u, 0u) == ARKRTOS_NET_EINVAL);
    arkrtos_current_task_id = 1;

    /* listen backlog 上限。 */
    CK(net_req(ARKRTOS_NET_SUB_LISTEN, 1u, 256u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);

    /* send 长度上限与读窗口。 */
    CK(net_req(ARKRTOS_NET_SUB_SEND, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 0x10000u,
               0u, 0u) == ARKRTOS_NET_EINVAL);
    g_ro_ok = 0;
    CK(net_req(ARKRTOS_NET_SUB_SEND, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 2u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    g_ro_ok = 1;

    /* recv 的 0 长度与写窗口。 */
    CK(net_req(ARKRTOS_NET_SUB_RECV, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    g_dev_fail_at = 1;
    g_dev_calls = 0;
    CK(net_req(ARKRTOS_NET_SUB_RECV, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 2u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);

    /* recvfrom 的两个输出指针分别拒（载荷那个放行）。 */
    {
        uint32 ip = 0u;
        uint32 port = 0u;

        g_dev_fail_at = 2;
        g_dev_calls = 0;
        CK(net_req(ARKRTOS_NET_SUB_RECVFROM, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 2u,
                   (arkrtos_svc_arg_t)(uintptr_t)&ip, (arkrtos_svc_arg_t)(uintptr_t)&port) ==
           ARKRTOS_NET_EINVAL);
        g_dev_fail_at = 3;
        g_dev_calls = 0;
        CK(net_req(ARKRTOS_NET_SUB_RECVFROM, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 2u,
                   (arkrtos_svc_arg_t)(uintptr_t)&ip, (arkrtos_svc_arg_t)(uintptr_t)&port) ==
           ARKRTOS_NET_EINVAL);
        g_dev_fail_at = 0;
    }

    /* getsockopt / name 的输出指针。 */
    {
        uint32 v = 0u;
        uint32 ip = 0u;
        uint32 port = 0u;

        g_dev_fail_at = 1;
        g_dev_calls = 0;
        CK(net_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u,
                   (uint32)ARKRTOS_NET_SO_ERROR, (arkrtos_svc_arg_t)(uintptr_t)&v,
                   0u, 0u) == ARKRTOS_NET_EINVAL);
        g_dev_fail_at = 2;
        g_dev_calls = 0;
        CK(net_req(ARKRTOS_NET_SUB_GETSOCKNAME, 1u,
                   (arkrtos_svc_arg_t)(uintptr_t)&ip, (arkrtos_svc_arg_t)(uintptr_t)&port,
                   0u, 0u) == ARKRTOS_NET_EINVAL);
        g_dev_fail_at = 0;
    }

    /* shutdown how 与 poll 标志。 */
    CK(net_req(ARKRTOS_NET_SUB_SHUTDOWN, 1u, 3u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    CK(net_req(ARKRTOS_NET_SUB_POLL, 1u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    CK(net_req(ARKRTOS_NET_SUB_POLL, 1u, 8u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);

    CK(g_unlock_calls > 0);                     /* 每条拒答都放了锁 */
    CK(g_lock_depth == g_unlock_calls);
    free_all_rows();
}

static void test_socket_table(void)
{
    reset_all();
    (void)arkrtos_net_start();

    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 1);
    CK(ark_fake_last_domain == AF_INET);
    CK(ark_fake_last_type == SOCK_STREAM);
    CK(ark_fake_last_proto == 0);
    CK(ark_fake_fcntl_count == 1);              /* 建好即非阻塞 */
    CK(arkrtos_net_host_sock_fd(0u) == 3);
    CK(arkrtos_net_host_sock_owner(0u) == 1);

    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
               0u, 0u, 0u, 0u) == 2);
    CK(ark_fake_last_type == SOCK_DGRAM);

    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 3);
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 4);

    /* 第五个：表满，刚建的 fd 必须被关掉，不能泄漏。 */
    {
        int before = ark_fake_close_count;

        CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
                   0u, 0u, 0u, 0u) == ARKRTOS_NET_ENOBUFS);
        CK(ark_fake_close_count == before + 1);
    }

    /* 关一个，再 accept 一个：accept 出来的 socket 也要非阻塞，并复用行。 */
    CK(net_req(ARKRTOS_NET_SUB_CLOSE, 2u, 0u, 0u, 0u, 0u) == 0);
    CK(arkrtos_net_host_sock_fd(1u) == -1);
    CK(arkrtos_net_host_sock_owner(1u) == 0);

    CK(net_req(ARKRTOS_NET_SUB_LISTEN, 1u, 5u, 0u, 0u, 0u) == 0);
    CK(ark_fake_last_option == 5);
    CK(net_req(ARKRTOS_NET_SUB_ACCEPT, 1u, 0u, 0u, 0u, 0u) == 2);
    CK(ark_fake_fcntl_count == 6);              /* 4 个 socket + 1 accept */
    CK(arkrtos_net_host_sock_fd(1u) >= 0);

    /* 关闭后再用这个 handle：立刻失效。 */
    CK(net_req(ARKRTOS_NET_SUB_CLOSE, 2u, 0u, 0u, 0u, 0u) == 0);
    CK(net_req(ARKRTOS_NET_SUB_BIND, 2u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);

    /* 行号越界读数：-2（不是 -1 那样"空行"）。 */
    CK(arkrtos_net_host_sock_fd((uint32)ARKRTOS_NET_MAX_SOCKETS) == -2);
    CK(arkrtos_net_host_sock_owner((uint32)ARKRTOS_NET_MAX_SOCKETS) == -2);

    free_all_rows();
}

static const char PING[] = "PING";

static void test_operations(void)
{
    uint32 ip = ARKRTOS_NET_IP(10, 0, 0, 7);
    uint32 port = 8080u;

    reset_all();
    (void)arkrtos_net_start();
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 1);

    /* bind：出门时确实换成了网络序（假实现按 ntohl 解回来比对）。 */
    CK(net_req(ARKRTOS_NET_SUB_BIND, 1u, ip, port, 0u, 0u) == 0);
    CK(ark_fake_last_ip == ip);
    CK(ark_fake_last_port == port);

    /* connect：EISCONN 是"已经到了"，不是失败。 */
    CK(net_req(ARKRTOS_NET_SUB_CONNECT, 1u, ip, port, 0u, 0u) == 0);
    CK(ark_fake_last_ip == ip);
    CK(ark_fake_last_port == port);

    ark_fake_fail[ARK_FAKE_CONNECT] = 1;
    ark_fake_errno = EISCONN;
    CK(net_req(ARKRTOS_NET_SUB_CONNECT, 1u, ip, port, 0u, 0u) == 0);

    ark_fake_errno = ECONNREFUSED;
    CK(net_req(ARKRTOS_NET_SUB_CONNECT, 1u, ip, port, 0u, 0u) ==
       ARKRTOS_NET_ECONNREFUSED);
    ark_fake_fail[ARK_FAKE_CONNECT] = 0;

    /* send：载荷指针、长度、flags 原样交给 lwIP，返回值是字节数。 */
    CK(net_req(ARKRTOS_NET_SUB_SEND, 1u, (arkrtos_svc_arg_t)(uintptr_t)PING, 4u,
               0x40u, 0u) == 4);
    CK(ark_fake_last_buf == (const void *)PING);
    CK(ark_fake_last_len == 4u);
    CK(ark_fake_last_flags == 0x40);

    /* 0 长度允许（不需要可读窗口）。 */
    CK(net_req(ARKRTOS_NET_SUB_SEND, 1u, 0u, 0u, 0u, 0u) == 0);

    /* recv：写进调用方缓冲。 */
    {
        char buf[8];

        memcpy(ark_fake_rx_payload, "PONG", 4u);
        ark_fake_rx_len = 4;
        memset(buf, 0, sizeof(buf));
        CK(net_req(ARKRTOS_NET_SUB_RECV, 1u, (arkrtos_svc_arg_t)(uintptr_t)buf, sizeof(buf),
                   0u, 0u) == 4);
        CK(memcmp(buf, "PONG", 4u) == 0);
        CK(ark_fake_last_buf == (const void *)buf);
    }

    /* sendto / recvfrom：地址与端口双向。 */
    CK(net_req(ARKRTOS_NET_SUB_SENDTO, 1u, (arkrtos_svc_arg_t)(uintptr_t)"AB", 2u,
               ip, port) == 2);
    CK(ark_fake_last_ip == ip);
    CK(ark_fake_last_port == port);

    {
        char buf[8];
        uint32 out_ip = 0u;
        uint32 out_port = 0u;

        ark_fake_peer_ip = ARKRTOS_NET_IP(192, 168, 1, 9);
        ark_fake_peer_port = 1234u;
        memcpy(ark_fake_rx_payload, "HI", 2u);
        ark_fake_rx_len = 2;
        memset(buf, 0, sizeof(buf));
        CK(net_req(ARKRTOS_NET_SUB_RECVFROM, 1u, (arkrtos_svc_arg_t)(uintptr_t)buf, sizeof(buf),
                   (arkrtos_svc_arg_t)(uintptr_t)&out_ip,
                   (arkrtos_svc_arg_t)(uintptr_t)&out_port) == 2);
        CK(memcmp(buf, "HI", 2u) == 0);
        CK(out_ip == ark_fake_peer_ip);
        CK(out_port == ark_fake_peer_port);
    }

    /* close：句柄失效。 */
    CK(net_req(ARKRTOS_NET_SUB_CLOSE, 1u, 0u, 0u, 0u, 0u) == 0);
    CK(net_req(ARKRTOS_NET_SUB_CLOSE, 1u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);

    free_all_rows();
}

static void test_options_and_poll(void)
{
    reset_all();
    (void)arkrtos_net_start();
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
               0u, 0u, 0u, 0u) == 1);

    /* setsockopt：三个支持项 + 一个诚实拒绝。 */
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_REUSEADDR, 1u, 0u, 0u) == 0);
    CK(ark_fake_last_optname == SO_REUSEADDR);
    CK(ark_fake_last_option == 1);
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_RCVBUF, 2048u, 0u, 0u) == 0);
    CK(ark_fake_last_optname == SO_RCVBUF);
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_SNDBUF, 1024u, 0u, 0u) == 0);
    CK(ark_fake_last_optname == SO_SNDBUF);
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u, 9u, 0u, 0u, 0u) ==
       ARKRTOS_NET_ENOTSUP);

    ark_fake_fail[ARK_FAKE_SETSOCKOPT] = 1;
    ark_fake_errno = ECONNABORTED;
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_REUSEADDR, 1u, 0u, 0u) ==
       ARKRTOS_NET_ECONNABORTED);
    ark_fake_fail[ARK_FAKE_SETSOCKOPT] = 0;

    /* getsockopt：SO_ERROR 的 0 / 非 0 都翻成 ABI 错误码。 */
    {
        uint32 v = 0xDEADBEEFu;

        CK(net_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u,
                   (uint32)ARKRTOS_NET_SO_ERROR, (arkrtos_svc_arg_t)(uintptr_t)&v,
                   0u, 0u) == 0);
        CK(v == 0u);
        CK(ark_fake_last_optname == SO_ERROR);

        ark_fake_so_error = ECONNRESET;
        v = 0u;
        CK(net_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u,
                   (uint32)ARKRTOS_NET_SO_ERROR, (arkrtos_svc_arg_t)(uintptr_t)&v,
                   0u, 0u) == 0);
        CK(v == (uint32)ARKRTOS_NET_ECONNRESET);

        /* 输出可以为空：只要问得出来，就回答 0。 */
        CK(net_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u,
                   (uint32)ARKRTOS_NET_SO_ERROR, 0u, 0u, 0u) == 0);
    }
    CK(net_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u, 7u, 0u, 0u, 0u) ==
       ARKRTOS_NET_ENOTSUP);

    ark_fake_fail[ARK_FAKE_GETSOCKOPT] = 1;
    ark_fake_errno = EINVAL;
    CK(net_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_ERROR, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    ark_fake_fail[ARK_FAKE_GETSOCKOPT] = 0;

    /* getsockname / getpeername：都回地址与端口，也允许两个输出都为空。 */
    {
        uint32 ip = 0u;
        uint32 port = 0u;

        ark_fake_peer_ip = ARKRTOS_NET_IP(127, 0, 0, 1);
        ark_fake_peer_port = 53u;
        CK(net_req(ARKRTOS_NET_SUB_GETSOCKNAME, 1u,
                   (arkrtos_svc_arg_t)(uintptr_t)&ip, (arkrtos_svc_arg_t)(uintptr_t)&port,
                   0u, 0u) == 0);
        CK(ip == ark_fake_peer_ip);
        CK(port == ark_fake_peer_port);

        ip = 0u;
        port = 0u;
        CK(net_req(ARKRTOS_NET_SUB_GETPEERNAME, 1u,
                   (arkrtos_svc_arg_t)(uintptr_t)&ip, (arkrtos_svc_arg_t)(uintptr_t)&port,
                   0u, 0u) == 0);
        CK(ip == ark_fake_peer_ip);
        CK(port == ark_fake_peer_port);

        CK(net_req(ARKRTOS_NET_SUB_GETSOCKNAME, 1u, 0u, 0u, 0u, 0u) == 0);
        CK(net_req(ARKRTOS_NET_SUB_GETPEERNAME, 1u, 0u, 0u, 0u, 0u) == 0);
    }

    ark_fake_fail[ARK_FAKE_GETPEERNAME] = 1;
    ark_fake_errno = ENOTCONN;
    CK(net_req(ARKRTOS_NET_SUB_GETPEERNAME, 1u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_ENOTCONN);
    ark_fake_fail[ARK_FAKE_GETPEERNAME] = 0;

    ark_fake_fail[ARK_FAKE_GETSOCKNAME] = 1;
    ark_fake_errno = ENOTCONN;
    CK(net_req(ARKRTOS_NET_SUB_GETSOCKNAME, 1u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_ENOTCONN);
    ark_fake_fail[ARK_FAKE_GETSOCKNAME] = 0;

    /* shutdown：合法 how 与 lwIP 错误。 */
    CK(net_req(ARKRTOS_NET_SUB_SHUTDOWN, 1u, (uint32)ARKRTOS_NET_SHUT_RDWR,
               0u, 0u, 0u) == 0);
    CK(ark_fake_last_option == ARKRTOS_NET_SHUT_RDWR);
    ark_fake_fail[ARK_FAKE_SHUTDOWN] = 1;
    ark_fake_errno = ENOTCONN;
    CK(net_req(ARKRTOS_NET_SUB_SHUTDOWN, 1u, (uint32)ARKRTOS_NET_SHUT_WR,
               0u, 0u, 0u) == ARKRTOS_NET_ENOTCONN);
    ark_fake_fail[ARK_FAKE_SHUTDOWN] = 0;

    /* poll：就绪位按请求的位回，没就绪回 0，select 失败翻成错误码。 */
    CK(net_req(ARKRTOS_NET_SUB_POLL, 1u, (uint32)ARKRTOS_NET_POLL_IN,
               0u, 0u, 0u) == 0);
    ark_fake_ready_in = 1;
    CK(net_req(ARKRTOS_NET_SUB_POLL, 1u, (uint32)ARKRTOS_NET_POLL_IN,
               0u, 0u, 0u) == (int32)ARKRTOS_NET_POLL_IN);
    ark_fake_ready_out = 1;
    ark_fake_ready_err = 1;
    CK(net_req(ARKRTOS_NET_SUB_POLL, 1u,
               (uint32)(ARKRTOS_NET_POLL_IN | ARKRTOS_NET_POLL_OUT |
                        ARKRTOS_NET_POLL_ERR),
               0u, 0u, 0u) ==
       (int32)(ARKRTOS_NET_POLL_IN | ARKRTOS_NET_POLL_OUT | ARKRTOS_NET_POLL_ERR));
    CK(ark_fake_select_maxfd == 4);             /* fd 3 + 1 */

    ark_fake_fail[ARK_FAKE_SELECT] = 1;
    ark_fake_errno = EINVAL;
    CK(net_req(ARKRTOS_NET_SUB_POLL, 1u, (uint32)ARKRTOS_NET_POLL_IN,
               0u, 0u, 0u) == ARKRTOS_NET_EINVAL);
    ark_fake_fail[ARK_FAKE_SELECT] = 0;

    /* 没映射过的 errno 一律 EIO：不许把 lwIP 的号透给 App。 */
    ark_fake_fail[ARK_FAKE_SEND] = 1;
    ark_fake_errno = 999;
    CK(net_req(ARKRTOS_NET_SUB_SEND, 1u, (arkrtos_svc_arg_t)(uintptr_t)"A", 1u, 0u, 0u) ==
       ARKRTOS_NET_EIO);
    ark_fake_fail[ARK_FAKE_SEND] = 0;

    free_all_rows();
}

static void test_request_slot(void)
{
    reset_all();
    (void)arkrtos_net_start();

    /* 在飞时：别人 EBUSY、自己不同参数也 EBUSY、自己同参数 EPENDING。 */
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EPENDING);
    CK(arkrtos_net_host_req_state() == 1u);

    arkrtos_current_task_id = 2;
    CK(net_call() == ARKRTOS_NET_EBUSY);
    arkrtos_current_task_id = 1;

    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EBUSY);

    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EPENDING);

    arkrtos_net_host_step();
    CK(arkrtos_net_host_req_state() == 2u);

    /* 完成态：别人来取 EBUSY，主人来取才拿到。 */
    arkrtos_current_task_id = 2;
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EBUSY);
    CK(arkrtos_net_host_req_state() == 2u);
    arkrtos_current_task_id = 1;
    CK(net_call() == 1);
    CK(arkrtos_net_host_req_state() == 0u);

    /* 主人放弃（换了参数）：旧答案被丢弃，不是递给新调用。 */
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EPENDING);
    arkrtos_net_host_step();
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EPENDING);     /* 丢弃后立刻接了新请求 */
    CK(net_wait() == 3);                        /* 新请求自己的结果（第三行） */
    CK(arkrtos_net_host_req_state() == 0u);

    /* 队列发送失败：状态回 IDLE，不留下"永远在飞"的槽。 */
    g_mq_send_fail = 1;
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EBUSY);
    CK(arkrtos_net_host_req_state() == 0u);
    g_mq_send_fail = 0;

    free_all_rows();
}

static void test_release_task(void)
{
    reset_all();
    (void)arkrtos_net_start();

    /* 任务 3 建一个 socket，然后被卸载：行变孤儿，服务任务一拍后关 fd。 */
    arkrtos_current_task_id = 3;
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 1);
    CK(arkrtos_net_host_sock_owner(0u) == 3);

    arkrtos_net_release_task(3);
    CK(arkrtos_net_host_sock_owner(0u) == -1);
    CK(g_mq_msg == 2u);                         /* REAP 消息真的投了 */

    arkrtos_net_host_step();
    CK(arkrtos_net_host_sock_fd(0u) == -1);

    /* 行回到可用：任务 1 再建一个，拿到的还是同一行。 */
    arkrtos_current_task_id = 1;
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
               0u, 0u, 0u, 0u) == 1);
    CK(arkrtos_net_host_sock_owner(0u) == 1);

    /* 在飞的请求跟着主人一起作废：结果没人收，下一个调用丢弃它再接新请求。 */
    arkrtos_current_task_id = 3;
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EPENDING);
    arkrtos_net_release_task(3);
    arkrtos_net_host_step();                    /* 执行时 owner 已是 -1 */
    CK(arkrtos_net_host_req_state() == 2u);

    arkrtos_current_task_id = 1;
    set_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
            0u, 0u, 0u, 0u);
    CK(net_call() == ARKRTOS_NET_EPENDING);
    CK(net_wait() == 2);                        /* 任务 1 自己的 socket */

    /* 非法任务号：什么都不做。 */
    {
        int before = arkrtos_net_host_sock_owner(0u);

        arkrtos_net_release_task(0);
        arkrtos_net_release_task(-5);
        CK(arkrtos_net_host_sock_owner(0u) == before);
    }

    free_all_rows();
}

/* 服务任务那一拍里，"收到的是 REAP 而不是 REQUEST"也要走通。 */
static void test_reap_only(void)
{
    int before;

    reset_all();
    (void)arkrtos_net_start();
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 1);

    before = ark_fake_close_count;
    mq_push(2u);                                /* 与 release_task 投的同值 */
    arkrtos_net_host_step();
    CK(arkrtos_net_host_req_state() == 0u);     /* REAP 不碰请求槽 */
    CK(ark_fake_close_count == before);         /* 没有孤儿，就不关任何 fd */

    free_all_rows();
}

/* 拒绝路径：这些分支在正常用例里不会走到，但它们才是"内核怎么对 App 说不"
 * 的实质——映射表把 lwIP 的号翻成自己的号（绝不透传），每个操作在句柄已经
 * 不可用时必须拒绝而不是碰表。 */
static void test_failure_paths(void)
{
    static const char D[] = "ABCD";
    static const uint32 errs[] = {
        EWOULDBLOCK, EINPROGRESS, EALREADY, ENOTCONN, ECONNABORTED,
        EHOSTUNREACH, EMSGSIZE, ENOBUFS, EINVAL, 4242u
    };
    static const int32 codes[] = {
        ARKRTOS_NET_EWOULDBLOCK, ARKRTOS_NET_EINPROGRESS, ARKRTOS_NET_EALREADY,
        ARKRTOS_NET_ENOTCONN, ARKRTOS_NET_ECONNABORTED,
        ARKRTOS_NET_EHOSTUNREACH, ARKRTOS_NET_EMSGSIZE, ARKRTOS_NET_ENOBUFS,
        ARKRTOS_NET_EINVAL, ARKRTOS_NET_EIO
    };
    uint32 i;

    reset_all();
    (void)arkrtos_net_start();
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
               0u, 0u, 0u, 0u) == 1);

    /* 1) errno 映射表逐项：App 只许看见自己的号，没映射的一律 EIO。 */
    for (i = 0u; i < (uint32)(sizeof(errs) / sizeof(errs[0])); i++) {
        ark_fake_fail[ARK_FAKE_SEND] = 1;
        ark_fake_errno = (int)errs[i];
        CK(net_req(ARKRTOS_NET_SUB_SEND, 1u,
                   (arkrtos_svc_arg_t)(uintptr_t)D, 4u, 0u, 0u) == codes[i]);
        ark_fake_fail[ARK_FAKE_SEND] = 0;
    }

    /* 2) 操作里的 lwIP 失败映射：前面用例没覆盖到的几条。 */
    {
        char buf[8];

        ark_fake_fail[ARK_FAKE_RECV] = 1;
        ark_fake_errno = ECONNRESET;
        CK(net_req(ARKRTOS_NET_SUB_RECV, 1u,
                   (arkrtos_svc_arg_t)(uintptr_t)buf, 4u, 0u, 0u) ==
           ARKRTOS_NET_ECONNRESET);
        ark_fake_fail[ARK_FAKE_RECV] = 0;
    }

    ark_fake_fail[ARK_FAKE_SETSOCKOPT] = 1;
    ark_fake_errno = ENOBUFS;
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_RCVBUF, 512u, 0u, 0u) ==
       ARKRTOS_NET_ENOBUFS);
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_SNDBUF, 512u, 0u, 0u) ==
       ARKRTOS_NET_ENOBUFS);
    ark_fake_fail[ARK_FAKE_SETSOCKOPT] = 0;

    ark_fake_fail[ARK_FAKE_BIND] = 1;
    ark_fake_errno = EINVAL;
    CK(net_req(ARKRTOS_NET_SUB_BIND, 1u, 0u, 80u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    ark_fake_fail[ARK_FAKE_BIND] = 0;

    ark_fake_fail[ARK_FAKE_LISTEN] = 1;
    ark_fake_errno = ENOBUFS;
    CK(net_req(ARKRTOS_NET_SUB_LISTEN, 1u, 4u, 0u, 0u, 0u) ==
       ARKRTOS_NET_ENOBUFS);
    ark_fake_fail[ARK_FAKE_LISTEN] = 0;

    ark_fake_fail[ARK_FAKE_ACCEPT] = 1;
    ark_fake_errno = ECONNABORTED;
    CK(net_req(ARKRTOS_NET_SUB_ACCEPT, 1u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_ECONNABORTED);
    ark_fake_fail[ARK_FAKE_ACCEPT] = 0;

    ark_fake_fail[ARK_FAKE_SENDTO] = 1;
    ark_fake_errno = EMSGSIZE;
    CK(net_req(ARKRTOS_NET_SUB_SENDTO, 1u, (arkrtos_svc_arg_t)(uintptr_t)D,
               4u, ARKRTOS_NET_IP(10, 0, 0, 1), 9u) == ARKRTOS_NET_EMSGSIZE);
    ark_fake_fail[ARK_FAKE_SENDTO] = 0;

    {
        char buf[8];

        ark_fake_fail[ARK_FAKE_RECVFROM] = 1;
        ark_fake_errno = ECONNRESET;
        CK(net_req(ARKRTOS_NET_SUB_RECVFROM, 1u,
                   (arkrtos_svc_arg_t)(uintptr_t)buf, 4u, 0u, 0u) ==
           ARKRTOS_NET_ECONNRESET);
        ark_fake_fail[ARK_FAKE_RECVFROM] = 0;
    }

    ark_fake_fail[ARK_FAKE_SETSOCKOPT] = 1;
    ark_fake_errno = EINVAL;
    CK(net_req(ARKRTOS_NET_SUB_SETSOCKOPT, 1u,
               (uint32)ARKRTOS_NET_SO_REUSEADDR, 1u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    ark_fake_fail[ARK_FAKE_SETSOCKOPT] = 0;

    ark_fake_fail[ARK_FAKE_SHUTDOWN] = 1;
    ark_fake_errno = ENOTCONN;
    CK(net_req(ARKRTOS_NET_SUB_SHUTDOWN, 1u,
               (uint32)ARKRTOS_NET_SHUT_RDWR, 0u, 0u, 0u) ==
       ARKRTOS_NET_ENOTCONN);
    ark_fake_fail[ARK_FAKE_SHUTDOWN] = 0;

    /* close 失败也要真的把行还回去，否则下一次调用拿到一个死 fd。 */
    ark_fake_fail[ARK_FAKE_CLOSE] = 1;
    ark_fake_errno = EINVAL;
    CK(net_req(ARKRTOS_NET_SUB_CLOSE, 1u, 0u, 0u, 0u, 0u) ==
       ARKRTOS_NET_EINVAL);
    ark_fake_fail[ARK_FAKE_CLOSE] = 0;
    CK(arkrtos_net_host_sock_fd(0u) == -1);

    /* 2b) 句柄不归自己：每个带"归属"检查的命令都要在调用者上下文里拒，
     *     而不是把请求投进槽让服务任务去发现。 */
    {
        char   buf[8];
        uint32 v = 0u;
        uint32 ip = 0u;
        uint32 port = 0u;

        arkrtos_current_task_id = 3;            /* 套接字是任务 1 的 */
        set_req(ARKRTOS_NET_SUB_LISTEN, 1u, 4u, 0u, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        set_req(ARKRTOS_NET_SUB_RECVFROM, 1u,
                (arkrtos_svc_arg_t)(uintptr_t)buf, 4u, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        set_req(ARKRTOS_NET_SUB_GETSOCKOPT, 1u, (uint32)ARKRTOS_NET_SO_ERROR,
                (arkrtos_svc_arg_t)(uintptr_t)&v, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        set_req(ARKRTOS_NET_SUB_GETSOCKNAME, 1u,
                (arkrtos_svc_arg_t)(uintptr_t)&ip,
                (arkrtos_svc_arg_t)(uintptr_t)&port, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        set_req(ARKRTOS_NET_SUB_GETPEERNAME, 1u,
                (arkrtos_svc_arg_t)(uintptr_t)&ip,
                (arkrtos_svc_arg_t)(uintptr_t)&port, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        set_req(ARKRTOS_NET_SUB_SHUTDOWN, 1u,
                (uint32)ARKRTOS_NET_SHUT_RDWR, 0u, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        set_req(ARKRTOS_NET_SUB_POLL, 1u, (uint32)ARKRTOS_NET_POLL_IN,
                0u, 0u, 0u);
        CK(net_call() == ARKRTOS_NET_EINVAL);
        arkrtos_current_task_id = 1;
    }

    /* 3) socket() 本身失败：没有列，必须把 lwIP 的号翻过来。 */
    ark_fake_fail[ARK_FAKE_SOCKET] = 1;
    ark_fake_errno = ENOBUFS;
    CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_DGRAM,
               0u, 0u, 0u, 0u) == ARKRTOS_NET_ENOBUFS);
    ark_fake_fail[ARK_FAKE_SOCKET] = 0;

    /* 4) accept 拿到 fd 但表已满：新 fd 必须被关掉，不能漏。 */
    {
        uint32 before = (uint32)ark_fake_close_count;

        CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
                   0u, 0u, 0u, 0u) == 1);
        CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
                   0u, 0u, 0u, 0u) == 2);
        CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
                   0u, 0u, 0u, 0u) == 3);
        CK(net_req(ARKRTOS_NET_SUB_SOCKET, (uint32)ARKRTOS_NET_SOCK_STREAM,
                   0u, 0u, 0u, 0u) == 4);
        CK(net_req(ARKRTOS_NET_SUB_ACCEPT, 1u, 0u, 0u, 0u, 0u) ==
           ARKRTOS_NET_ENOBUFS);
        CK((uint32)ark_fake_close_count == before + 1u);
    }
    free_all_rows();

    /* 5) 在飞时主人没了：请求已经进了槽，执行时句柄不可达。每个操作都必须
     *    返回 EINVAL，而不是照着旧参数去碰表（更不是把结果递给下一个人）。
     *    请求是在主人还活着时发的（校验通过），所以每一轮都要给任务 3 重建
     *    一个套接字——上一轮的会被 reap 关掉，这正是本用例要验证的一半。 */
    {
        static const uint32 ops[] = {
            ARKRTOS_NET_SUB_BIND,      ARKRTOS_NET_SUB_LISTEN,
            ARKRTOS_NET_SUB_ACCEPT,    ARKRTOS_NET_SUB_CONNECT,
            ARKRTOS_NET_SUB_SEND,      ARKRTOS_NET_SUB_SENDTO,
            ARKRTOS_NET_SUB_RECV,      ARKRTOS_NET_SUB_RECVFROM,
            ARKRTOS_NET_SUB_CLOSE,     ARKRTOS_NET_SUB_SETSOCKOPT,
            ARKRTOS_NET_SUB_GETSOCKOPT, ARKRTOS_NET_SUB_GETSOCKNAME,
            ARKRTOS_NET_SUB_GETPEERNAME, ARKRTOS_NET_SUB_SHUTDOWN,
            ARKRTOS_NET_SUB_POLL
        };
        char   buf[8];
        uint32 v = 0u;
        uint32 ip = 0u;
        uint32 port = 0u;
        int    guard;

        for (i = 0u; i < (uint32)(sizeof(ops) / sizeof(ops[0])); i++) {
            arkrtos_current_task_id = 3;
            CK(net_req(ARKRTOS_NET_SUB_SOCKET,
                       (uint32)ARKRTOS_NET_SOCK_STREAM, 0u, 0u, 0u, 0u) == 1);

            switch (ops[i]) {
            case ARKRTOS_NET_SUB_BIND:
                set_req(ops[i], 1u, 0u, 80u, 0u, 0u); break;
            case ARKRTOS_NET_SUB_LISTEN:
                set_req(ops[i], 1u, 4u, 0u, 0u, 0u); break;
            case ARKRTOS_NET_SUB_ACCEPT:
                set_req(ops[i], 1u, 0u, 0u, 0u, 0u); break;
            case ARKRTOS_NET_SUB_CONNECT:
                set_req(ops[i], 1u, 0u, 0u, 0u, 0u); break;
            case ARKRTOS_NET_SUB_SEND:
            case ARKRTOS_NET_SUB_SENDTO:
                set_req(ops[i], 1u, (arkrtos_svc_arg_t)(uintptr_t)buf, 4u,
                        0u, 0u); break;
            case ARKRTOS_NET_SUB_RECV:
            case ARKRTOS_NET_SUB_RECVFROM:
                set_req(ops[i], 1u, (arkrtos_svc_arg_t)(uintptr_t)buf, 4u,
                        0u, 0u); break;
            case ARKRTOS_NET_SUB_CLOSE:
                set_req(ops[i], 1u, 0u, 0u, 0u, 0u); break;
            case ARKRTOS_NET_SUB_SETSOCKOPT:
                set_req(ops[i], 1u, (uint32)ARKRTOS_NET_SO_REUSEADDR, 1u,
                        0u, 0u); break;
            case ARKRTOS_NET_SUB_GETSOCKOPT:
                set_req(ops[i], 1u, (uint32)ARKRTOS_NET_SO_ERROR,
                        (arkrtos_svc_arg_t)(uintptr_t)&v, 0u, 0u); break;
            case ARKRTOS_NET_SUB_GETSOCKNAME:
            case ARKRTOS_NET_SUB_GETPEERNAME:
                set_req(ops[i], 1u, (arkrtos_svc_arg_t)(uintptr_t)&ip,
                        (arkrtos_svc_arg_t)(uintptr_t)&port, 0u, 0u); break;
            case ARKRTOS_NET_SUB_SHUTDOWN:
                set_req(ops[i], 1u, (uint32)ARKRTOS_NET_SHUT_RDWR,
                        0u, 0u, 0u); break;
            default:                            /* POLL */
                set_req(ops[i], 1u, (uint32)ARKRTOS_NET_POLL_IN,
                        0u, 0u, 0u); break;
            }
            CK(net_call() == ARKRTOS_NET_EPENDING);
            arkrtos_net_release_task(3);        /* 主人走了，句柄变孤儿 */

            guard = 0;
            while ((arkrtos_net_host_req_state() != 2u) && (guard++ < 16)) {
                arkrtos_net_host_step();        /* 队列里可能先有 REAP */
            }
            CK(arkrtos_net_host_req_state() == 2u);
            CK(arkrtos_net_host_req_result() == ARKRTOS_NET_EINVAL);
            arkrtos_net_host_reset();
            arkrtos_current_task_id = 1;
            free_all_rows();
        }
    }

    free_all_rows();
}

int main(void)
{
    g_checks = 0;
    g_fails = 0;

    test_lifecycle();
    test_svc_gates();
    test_socket_table();
    test_operations();
    test_options_and_poll();
    test_request_slot();
    test_release_task();
    test_reap_only();
    test_failure_paths();

    printf("NET-HOST OK  %d checks, %d failed\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}

#endif /* ARKRTOS_USE_LWIP */
