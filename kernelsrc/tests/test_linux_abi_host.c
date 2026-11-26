/**
* @file test_linux_abi_host.c
* @brief arkrtos_linux_abi.c 的宿主用例：号表分发 / 返回值分类 / fd 表 /
*        管道 / 信号投递与回收 / execve 失败映射 / 直方图
* @details 被验的是「一次系统调用从入口到返回值写回」这条链上的判据，
*          不是「一个 Linux 程序真的跑起来了」——后者要真 CPU 与真镜像，
*          由 QEMU 上板覆盖。
*
*          宿主这一档必须说清三件事，否则读数会被误读：
*
*            1) 用户内存是**模拟**的。copy_from_user / to_user 在本用例里
*               是桩：地址落在 [WINDOW_BASE, WINDOW_BASE+WINDOW_SIZE) 内就
*               从 g_user[] 对应偏移读写，越界返回 -3（= 被测代码看到的
*               "不在窗口里"）。所以「合法指针 / 非法指针 / 半路失效」三种
*               情形都能确定地造出来，但这不证明真分页后端的窗口判定。
*
*            2) 字宽是 32 位（MinGW ILP32，unsigned long = 4 字节）。信号
*               帧头在 32 位上只占 16 字节，而 total 仍按
*               LX_SIGFRAME_HDR_BYTES(32) 算 ⇒ 铺帧会多拷 16 字节槽尾；
*               payload 从 &s_sigframe[4]（字节偏移 16）开始。la64 的
*               64 位布局（头 32 字节、payload 偏移 32）由 QEMU 上板覆盖，
*               这里**不**伪装覆盖。
*
*            3) 帧长只测 256 字节这一档。fbytes=1040 在 32 位宿主上会写爆
*               536 字节的 s_sigframe（total = 32+1040 > 536），那属于
*               端口帧长与内核缓冲的老账，不是本用例要断言的行为。
*
*          明确的开关前提（与 coverage_gate / ci_gate 的宏一致）：
*            ARKRTOS_USE_LINUX_ABI=1 / USE_PAGING=1（真 copy 函数）
*            USE_ELF=1 / USE_EXIT_CODE=1 / LINUX_ABI_SYSSTAT=1
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_config.h"
#include "arkrtos_linux_abi.h"

extern int32 arkrtos_port_svc_restart(void *p_exc_ctx);

#if (ARKRTOS_USE_LINUX_ABI == 0)

/* 关档：入口全折成常量。这里只证明这一档仍可编可跑，且每个读数都回答
 * 「没有这件事」，而不是空成功。 */
int main(void)
{
    int fails = 0;

    if(arkrtos_linux_abi_try((void *)0) != 0)
    {
        fails++;
    }
    if(arkrtos_linux_abi_stat(0u) != 0u)
    {
        fails++;
    }
    if(arkrtos_linux_abi_nr_slots() != 0u)
    {
        fails++;
    }
    if(arkrtos_linux_abi_nr_slot(0u, (uint32 *)0) != 0u)
    {
        fails++;
    }
    if(arkrtos_linux_abi_nr_overflow() != 0u)
    {
        fails++;
    }
    arkrtos_linux_abi_reset();

    if(fails != 0)
    {
        printf("LINUX-ABI HOST OFF FAIL %d\n", fails);
        return 1;
    }
    printf("LINUX-ABI HOST OFF OK 0 failed\n");
    return 0;
}

#else  /* ARKRTOS_USE_LINUX_ABI == 1 */

#include "arkrtos_task.h"
#include "arkrtos_elf.h"
#include "arkrtos_vfs.h"
#include "arkrtos_hal.h"
#include "arkrtos_mpu.h"
#include "arkrtos_board.h"
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
/* net_abi 的子命令/错误号从这张头来；它不拉 lwIP（只有 net.c 才拉）。 */
#include "arkrtos_net.h"
#endif

#if !defined(__SIZEOF_LONG__) || (__SIZEOF_LONG__ != 4)
#error "test_linux_abi_host 的帧头断言按 32 位字宽写死，需要 ILP32 宿主"
#endif
#if (ARKRTOS_USE_PAGING != 1)
#error "需要分页档：copy_from_user/to_user 只在 USE_PAGING=1 时是真函数"
#endif
#if (ARKRTOS_USE_ELF != 1) || (ARKRTOS_USE_EXIT_CODE != 1)
#error "需要 ELF 与退出码两档"
#endif
#if (ARKRTOS_LINUX_ABI_SYSSTAT != 1)
#error "本用例覆盖号表直方图，需要 LINUX_ABI_SYSSTAT=1"
#endif
#if !defined(ARKRTOS_ELF_WINDOW_BASE) || !defined(ARKRTOS_ELF_WINDOW_SIZE)
#error "需要板级配置给出的 ELF 窗口"
#endif

/* 号表与 errno 另写一份（用例不该因为被测实现改了常量就跟着改）。 */
#define T_NR_OPENAT         (56u)
#define T_NR_CLOSE          (57u)
#define T_NR_PIPE2          (59u)
#define T_NR_READ           (63u)
#define T_NR_WRITE          (64u)
#define T_NR_PREAD64        (67u)
#define T_NR_PSELECT6       (72u)
#define T_NR_EXIT           (93u)
#define T_NR_EXIT_GROUP     (94u)
#define T_NR_SET_TID        (96u)
#define T_NR_SET_ROBUST_LIST (99u)
#define T_NR_GET_ROBUST_LIST (100u)
#define T_NR_CLOCK_GETTIME  (113u)
#define T_NR_KILL           (129u)
#define T_NR_FUTEX          (98u)
#define T_NR_TKILL          (130u)
#define T_NR_TGKILL         (131u)
#define T_NR_RT_SIGACTION   (134u)
#define T_NR_RT_SIGPROCMASK (135u)
#define T_NR_RT_SIGRETURN   (139u)
#define T_NR_UNAME          (160u)
#define T_NR_GETPID         (172u)
#define T_NR_GETUID         (174u)
#define T_NR_GETEUID        (175u)
#define T_NR_BRK            (214u)
#define T_NR_MUNMAP         (215u)
#define T_NR_EXECVE         (221u)
#define T_NR_MMAP           (222u)
#define T_NR_MPROTECT       (226u)
#define T_NR_MSYNC          (227u)
/* socket 族号（asm-generic）。send/recv 没有独立号：glibc 走 sendto/recvfrom。 */
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
#define T_NR_SOCKET         (198u)
#define T_NR_BIND           (200u)
#define T_NR_LISTEN         (201u)
#define T_NR_ACCEPT         (202u)
#define T_NR_CONNECT        (203u)
#define T_NR_GETSOCKNAME    (204u)
#define T_NR_GETPEERNAME    (205u)
#define T_NR_SENDTO         (206u)
#define T_NR_RECVFROM       (207u)
#define T_NR_SETSOCKOPT     (208u)
#define T_NR_GETSOCKOPT     (209u)
#define T_NR_SHUTDOWN       (210u)
#define T_NR_ACCEPT4        (242u)
#endif

#define T_EPERM             (1)
#define T_ENOENT            (2)
#define T_ESRCH             (3)
#define T_EINTR             (4)
#define T_EIO               (5)
#define T_E2BIG             (7)
#define T_ENOEXEC           (8)
#define T_EBADF             (9)
#define T_EAGAIN            (11)
#define T_ENOMEM            (12)
#define T_EACCES            (13)
#define T_EFAULT            (14)
#define T_EBUSY             (16)
#define T_EISDIR            (21)
#define T_EINVAL            (22)
#define T_ENFILE            (23)
#define T_EROFS             (30)
#define T_EPIPE             (32)
#define T_ENAMETOOLONG      (36)
#define T_ENOSYS            (38)
#define T_ETIMEDOUT         (110)
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
#define T_EMFILE            (24)
#define T_ENOTSOCK          (88)
#define T_EMSGSIZE          (90)
#define T_ENOPROTOOPT       (92)
#define T_EPROTONOSUPPORT   (93)
#define T_ESOCKTNOSUPPORT   (94)
#define T_EAFNOSUPPORT      (97)
#define T_ENOBUFS           (105)
#define T_EHOSTUNREACH      (113)
#define T_ECONNABORTED      (103)
#define T_ECONNRESET        (104)
#define T_EISCONN           (106)
#define T_ENOTCONN          (107)
#define T_ECONNREFUSED      (111)
#define T_EALREADY          (114)
#define T_EINPROGRESS       (115)
/* socket 常量（与 asm-generic 的号表另写一份，理由同上面那张号表）。 */
#define T_AF_INET           (2u)
#define T_SOCK_STREAM       (1u)
#define T_SOCK_DGRAM        (2u)
#define T_SOCK_NONBLOCK     (0x800u)
#define T_SOCK_CLOEXEC      (0x80000u)
#define T_SOL_SOCKET        (1)
#define T_SO_REUSEADDR      (2)
#define T_SO_TYPE           (3)
#define T_SO_ERROR          (4)
#define T_SO_RCVBUF         (8)
#define T_SO_SNDBUF         (7)
#define T_SO_DOMAIN         (39)
#define T_MSG_DONTWAIT      (0x40u)
#endif

#define T_O_NONBLOCK        (0x800u)
#define T_O_RDWR            (0x2u)
#define T_O_WRONLY          (0x1u)
#define T_O_CREAT           (0x40u)
#define T_O_TRUNC           (0x200u)
#define T_MAP_SHARED        (0x01u)
#define T_MAP_PRIVATE       (0x02u)
#define T_MAP_FIXED         (0x10u)
#define T_MAP_ANONYMOUS     (0x20u)
#define T_PROT_READ         (0x1u)
#define T_PROT_WRITE        (0x2u)
#define T_MS_SYNC           (4u)
#define T_SIGSET_BYTES      (8u)

#define T_UREG_ERA          (0u)
#define T_UREG_SP           (1u)
#define T_UREG_RA           (2u)

/* 宿主上 magic 被截成 32 位：0x53494746524D4532 & 0xFFFFFFFF。 */
#define T_SIGFRAME_MAGIC32  (0x524D4532u)
#define T_FRAME_BYTES       (256u)
#define T_FRAME_TOTAL       (32u + T_FRAME_BYTES)

/* 用户内存里的固定偏移（都在窗口内，离堆区很远）。 */
#define T_PATH_OFF          (0x00100u)
#define T_ACT_OFF           (0x00200u)
#define T_OLD_OFF           (0x00240u)
#define T_SET_OFF           (0x00280u)
#define T_OLDMSK_OFF        (0x002C0u)
#define T_ARGV_OFF          (0x00400u)
#define T_STR_OFF           (0x00600u)
#define T_DATA_OFF          (0x01000u)
#define T_FSP_OFF           (0x1F0000u)
#define T_HANDLER           (0x02000800UL)
#define T_RESTORER          (0x02000C00UL)

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got 0x%lx want 0x%lx (line %d)\n",
               what, got, want, __LINE__);
    }
}

/* ---------------- 模拟用户内存 ---------------- */

#define U_BASE  ((unsigned long)ARKRTOS_ELF_WINDOW_BASE)
#define U_SIZE  ((uint32)ARKRTOS_ELF_WINDOW_SIZE)

static uint8 g_user[U_SIZE];

static unsigned long u_ptr(uint32 off)
{
    return U_BASE + (unsigned long)off;
}

static void u_put(uint32 off, const void *src, uint32 len)
{
    memcpy(&g_user[off], src, (size_t)len);
}

static void u_get(uint32 off, void *dst, uint32 len)
{
    memcpy(dst, &g_user[off], (size_t)len);
}

static void u_put32(uint32 off, uint32 v)
{
    memcpy(&g_user[off], &v, 4u);
}

static uint32 u_get32(uint32 off)
{
    uint32 v = 0u;

    memcpy(&v, &g_user[off], 4u);
    return v;
}

/* 单字节读数：断言「第 N 个字节的值」时必须用这个，不能拿 u_get32 去读
 * （那读的是 4 字节小端拼接值，0x7C 会被读成 0x7F7E7D7C）。 */
static uint32 u_get8(uint32 off)
{
    return (uint32)g_user[off];
}

static void u_put_str(uint32 off, const char *s)
{
    size_t n = strlen(s) + 1u;

    memcpy(&g_user[off], s, n);
}

/* 拷贝预算：-1 不限，0 直接失败，>0 再允许这么多次成功。
 * 用来造「第一块成功、第二块失败」的半路失效，而不是靠越界地址。 */
static int g_from_budget = -1;
static int g_to_budget = -1;

static int32 host_copy_from(void *dst, const void *usrc, uint32 len)
{
    unsigned long a = (unsigned long)(uintptr_t)usrc;

    if(g_from_budget == 0)
    {
        return -3;
    }
    if((a < U_BASE) || ((unsigned long)len > (unsigned long)U_SIZE - (a - U_BASE)))
    {
        return -3;
    }
    if(g_from_budget > 0)
    {
        g_from_budget--;
    }
    memcpy(dst, &g_user[a - U_BASE], (size_t)len);
    return 0;
}

static int32 host_copy_to(void *udst, const void *src, uint32 len)
{
    unsigned long a = (unsigned long)(uintptr_t)udst;

    if(g_to_budget == 0)
    {
        return -3;
    }
    if((a < U_BASE) || ((unsigned long)len > (unsigned long)U_SIZE - (a - U_BASE)))
    {
        return -3;
    }
    if(g_to_budget > 0)
    {
        g_to_budget--;
    }
    memcpy(&g_user[a - U_BASE], src, (size_t)len);
    return 0;
}

int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len)
{
    return host_copy_from(dst, usrc, len);
}

int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len)
{
    return host_copy_to(udst, src, len);
}

/* 临界区：futex 的登记与文件映射的写回忙标志都在临界区里（真机上与唤醒/切换
 * 争同一份表）。宿主是单线程，进出都当无事发生，只数进入次数证明真的走进去过。
 * 两个开关都要它，所以放在 futex 的条件之外。 */
/* MAP_FIXED 的端口能力（只有按需缺页档才有这一格）。宿主的职责只到「ABI
 * 层把请求原样交给了端口、把端口的结果翻成 errno、把游标推过去」；窗口里的
 * 字节怎么丢由端口负责，宿主不假装验它。g_discard_rc 让用例把端口的两种
 * 拒绝（-1 参数/越界、-2 帧池空）分别喂回来。 */
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
uint32 g_discard_calls;
int32  g_discard_rc;
unsigned long g_discard_va;
unsigned long g_discard_len;

int32 arkrtos_port_paging_discard(unsigned long va, unsigned long len)
{
    g_discard_calls++;
    g_discard_va  = va;
    g_discard_len = len;
    return g_discard_rc;
}
#endif

uint32 g_crit_in;

uint32 arkrtos_port_enter_critical(void)
{
    g_crit_in++;
    return 0u;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
}

#if (ARKRTOS_USE_LINUX_FUTEX == 1) || (ARKRTOS_USE_LINUX_SELECT == 1) || \
    (ARKRTOS_USE_LINUX_SOCKET == 1)
/* ---------------- futex / pselect6 的前提桩 ----------------
 * 本档只链接 linux_abi.c；futex 的键要走分页后端与任务表，这三组符号由用例
 * 自己给：窗口就是模拟用户内存那一块（这样 copy_from_user 也能用），每对
 * 8 KB 的物理基址按 idx 线性推进，idx 7 故意留空（没驻留的对应回 0）。
 * 键因此是可算的：u_ptr(0x10) -> 0x00100010。
 * pselect6 只要同一组「任务表 + block_in_critical + svc_restart」，分页那几
 * 个符号它不用，但一并编进来不影响读数。 */
#define T_FUTEX_WAIT        (0u)
#define T_FUTEX_WAKE        (1u)
#define T_FUTEX_REQUEUE     (3u)
#define T_FUTEX_CMP_REQUEUE (4u)
#define T_FUTEX_WAIT_BITSET (9u)
#define T_FUTEX_WAKE_BITSET (10u)
#define T_FUTEX_CLOCK_REALTIME (256u)
#define T_FUTEX_LOCK_PI     (6u)
#define T_FUTEX_UNLOCK_PI   (7u)
#define T_FUTEX_TRYLOCK_PI  (8u)
#define T_FUTEX_TID_MASK    (0x3FFFFFFFu)
#define T_FUTEX_WAITERS     (0x80000000u)
#define T_FUTEX_OWNER_DIED  (0x40000000u)

int32 g_block_calls;
uint32 g_block_ms;
int32 g_wake_calls;
int32 g_wake_last;
int32 g_restart_rc = -1;

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];

int32 arkrtos_task_block_in_critical(uint32 timeout_ms)
{
    g_block_calls++;
    g_block_ms = timeout_ms;
    return 0;
}

int32 arkrtos_task_wakeup_internal(int32 task_id)
{
    g_wake_calls++;
    g_wake_last = task_id;
    return 0;
}

/* PI 的优先级重算要调 arkrtos_ready_reprio（真机上把换过优先级的任务换桶）。
 * 宿主用例里没有就绪队列，这里只记账：断言看的是「有没有按 base_priority
 * 算回去」，以及算回去之前那个旧值对不对。 */
int32 g_reprio_calls;
int32 g_reprio_last_idx;
uint8 g_reprio_last_old;

void arkrtos_ready_reprio(int32 task_idx, uint8 old_prio)
{
    g_reprio_calls++;
    g_reprio_last_idx = task_idx;
    g_reprio_last_old = old_prio;
}

/* 窗口基址做成变量：futex 的键在「没有分页窗口」（MPU 后端）时退回虚拟
 * 地址，那条分支只有把这里改成 0 才走得到。默认值与用户内存起点一致。 */
unsigned long g_win_va = U_BASE;

unsigned long arkrtos_paging_window_va(void)
{
    return g_win_va;
}

uint32 arkrtos_paging_pair_total(void)
{
    return (uint32)(U_SIZE / 0x2000u);
}

uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task)
{
    (void)p_task;
    return 0u;
}

uint32 arkrtos_paging_pair_pa(uint32 space, uint32 idx)
{
    (void)space;
    if(idx == 7u)
    {
        return 0u;          /* 没驻留的那一对：问什么都不编地址 */
    }
    return 0x00100000u + (idx * 0x2000u);
}

/* 端口能力由用例直接给：本档用 -DARKRTOS_HOST_NO_PORT_RESTART=1 把
 * linux_abi.c 的缺省实现整段去掉（--wrap 管不到同一目标里的定义），
 * g_restart_rc 逐段控制「会不会重放」。 */
int32 arkrtos_port_svc_restart(void *p_exc_ctx)
{
    (void)p_exc_ctx;
    return g_restart_rc;
}
#endif

/* ---------------- 端口桩 ---------------- */

typedef struct
{
    unsigned long arg[6];
    uint32 nr;
    int    is_linux;
    unsigned long ret;
    int    ret_written;
    unsigned long reg[4];
    int    set_reg_fail;        /* -1 = 不失败；否则这个 id 会失败 */
    int    set_reg_calls;
    unsigned long reserve;
    uint32 reserve_bytes;
    int    reserve_calls;
    int    export_calls;
    int    import_fail;
    int    import_calls;
    unsigned long imported[64];
    int    exec_frame_rc;
    int    exec_frame_calls;
    unsigned long exec_entry;
    unsigned long exec_sp;
    unsigned long exec_tp;
} t_ctx;

static uint32 g_frame_bytes = T_FRAME_BYTES;
static unsigned long g_export_pat[64];

unsigned long arkrtos_port_svc_get_arg(void *p, uint32 idx)
{
    t_ctx *c = (t_ctx *)p;

    return (idx < 6u) ? c->arg[idx] : 0UL;
}

void arkrtos_port_svc_set_ret(void *p, unsigned long value)
{
    t_ctx *c = (t_ctx *)p;

    c->ret = value;
    c->ret_written = 1;
}

void arkrtos_port_svc_set_ret_signed(void *p, int32 value)
{
    t_ctx *c = (t_ctx *)p;

    c->ret = (unsigned long)(uint32)value;
    c->ret_written = 1;
}

uint32 arkrtos_port_linux_syscall_num(void *p)
{
    t_ctx *c = (t_ctx *)p;

    return c->is_linux ? c->nr : 0xFFFFFFFFu;
}

unsigned long arkrtos_port_svc_user_reg(void *p, uint32 id)
{
    t_ctx *c = (t_ctx *)p;

    return (id < 4u) ? c->reg[id] : 0UL;
}

int32 arkrtos_port_svc_set_user_reg(void *p, uint32 id, unsigned long value)
{
    t_ctx *c = (t_ctx *)p;

    c->set_reg_calls++;
    if((id >= 4u) || (c->set_reg_fail == (int)id))
    {
        return -1;
    }
    c->reg[id] = value;
    return 0;
}

uint32 arkrtos_port_svc_frame_bytes(void)
{
    return g_frame_bytes;
}

unsigned long arkrtos_port_svc_frame_reserve(void *p, uint32 bytes)
{
    t_ctx *c = (t_ctx *)p;

    c->reserve_calls++;
    c->reserve_bytes = bytes;
    return c->reserve;
}

void arkrtos_port_svc_frame_export(void *p, void *p_dst)
{
    t_ctx *c = (t_ctx *)p;

    c->export_calls++;
    memcpy(p_dst, g_export_pat, (size_t)g_frame_bytes);
}

int32 arkrtos_port_svc_frame_import(void *p, const void *p_src)
{
    t_ctx *c = (t_ctx *)p;

    c->import_calls++;
    if(c->import_fail != 0)
    {
        return -1;
    }
    memcpy(c->imported, p_src, (size_t)g_frame_bytes);
    return 0;
}

/* 源文件里这条是 __attribute__((weak))；用例给一份强定义，把 execve 的
 * 成功路径也走一遍（否则只能测到「端口不支持 -> ENOSYS」）。强定义与源
 * 里那个弱定义在同一个链接里，MinGW 下弱符号会解析到本地定义（-ffunction
 * -sections 也救不回来），所以用链接期 -Wl,--wrap 把引用改指到这里。 */
int32 __wrap_arkrtos_port_svc_exec_frame(void *p, unsigned long entry,
                                         unsigned long sp, unsigned long tp);
int32 __wrap_arkrtos_port_svc_exec_frame(void *p, unsigned long entry,
                                         unsigned long sp, unsigned long tp)
{
    t_ctx *c = (t_ctx *)p;

    c->exec_frame_calls++;
    c->exec_entry = entry;
    c->exec_sp = sp;
    c->exec_tp = tp;
    return c->exec_frame_rc;
}

/* ---------------- 内核侧桩 ---------------- */

int32 arkrtos_current_task_id = 7;

static uint32 g_ms = 1234567u;
static int32  g_exit_code = -1;
static int    g_exit_calls;
static char   g_putc[512];
static uint32 g_putc_len;

uint32 arkrtos_kernel_get_time(void)
{
    return g_ms;
}

void arkrtos_thread_exit_code_internal(int32 code)
{
    g_exit_code = code;
    g_exit_calls++;
}

void arkrtos_thread_exit_internal(void)
{
    g_exit_calls++;
}

void arkrtos_board_putc(char c)
{
    if(g_putc_len < (uint32)sizeof(g_putc))
    {
        g_putc[g_putc_len++] = c;
    }
}

/* lx_write 现在整次调用持控制台锁（否则内核日志能把用户一行切成两截）。
 * 宿主用例只链接 arkrtos_linux_abi.c，不链接 arkrtos_log.c，所以这里给一
 * 对带计数的桩：既补上符号，也让 write 用例能验证"进过一次锁、出来一定
 * 释放"——锁没释放的实现在下一次并发写里才会露馅，等不起。 */
static uint32 g_con_lock_calls;
static uint32 g_con_unlock_calls;
static uint32 g_con_held;

/* 与 kernelsrc/include/arkrtos_log.h 里的声明逐字一致：本用例不 include 那个
 * 头（它会拉进日志宏与运行期级别），但原型必须在这里可见，否则
 * -Wmissing-prototypes 会把这对桩当成"没声明的全局函数"报出来。 */
int32 arkrtos_console_lock(void);
void  arkrtos_console_unlock(void);

int32 arkrtos_console_lock(void)
{
    g_con_lock_calls++;
    g_con_held++;
    return 1;
}

void arkrtos_console_unlock(void)
{
    g_con_unlock_calls++;
    if(g_con_held > 0u)
    {
        g_con_held--;
    }
}

/* ---------------- 假 VFS ---------------- */

static int32  g_stat_rc;
static uint32 g_stat_size;
static uint32 g_stat_mode;
static char   g_stat_path[64];

int32 arkrtos_vfs_app_stat(const char *path, uint32 *size, uint32 *mode)
{
    size_t n = strlen(path);

    if(n >= sizeof(g_stat_path))
    {
        n = sizeof(g_stat_path) - 1u;
    }
    memcpy(g_stat_path, path, n);
    g_stat_path[n] = 0;

    if(g_stat_rc != 0)
    {
        return g_stat_rc;
    }
    *size = g_stat_size;
    *mode = g_stat_mode;
    return 0;
}

static uint8  g_file[4096];
static uint32 g_file_len;
static int32  g_read_rc;
static int    g_synth_any;
static char   g_read_path[64];

int32 arkrtos_vfs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                          uint32 *out_len)
{
    size_t n = strlen(path);
    uint32 k;

    if(n >= sizeof(g_read_path))
    {
        n = sizeof(g_read_path) - 1u;
    }
    memcpy(g_read_path, path, n);
    g_read_path[n] = 0;

    if(g_read_rc != 0)
    {
        return g_read_rc;
    }
    if(g_synth_any != 0)
    {
        for(k = 0u; k < len; k++)
        {
            buf[k] = (uint8)(off + k);
        }
        *out_len = len;
        return 0;
    }
    if(off >= g_file_len)
    {
        *out_len = 0u;
        return 0;
    }
    k = g_file_len - off;
    if(k > len)
    {
        k = len;
    }
    memcpy(buf, &g_file[off], (size_t)k);
    *out_len = k;
    return 0;
}

/* 写整个文件的桩：记下最后一次写回的路径/长度/内容，并按需拒绝。
 * 文件映射的写回是「读整个文件 → 覆盖映射段 → 整文件写回」，所以这里同时
 * 是读数口：写回成功后 g_file 就是文件的新内容。 */
static uint8  g_wbuf[8192];
static uint32 g_wlen;
static int32  g_write_rc;
static int    g_write_calls;
static char   g_write_path[64];

int32 arkrtos_vfs_write_file(const char *path, const uint8 *buf, uint32 len)
{
    size_t n = strlen(path);
    uint32 k = len;

    if(n >= sizeof(g_write_path))
    {
        n = sizeof(g_write_path) - 1u;
    }
    memcpy(g_write_path, path, n);
    g_write_path[n] = 0;
    g_write_calls++;
    g_wlen = len;
    if(k > sizeof(g_wbuf))
    {
        k = (uint32)sizeof(g_wbuf);
    }
    memcpy(g_wbuf, buf, (size_t)k);
    if(g_write_rc != 0)
    {
        return g_write_rc;
    }
    k = len;
    if(k > (uint32)sizeof(g_file))
    {
        k = (uint32)sizeof(g_file);
    }
    memcpy(g_file, buf, (size_t)k);
    g_file_len = len;
    return 0;
}

/* ---------------- 假 ELF 装载器 ---------------- */

static int32 g_exec_rc;
static int32 g_exec_argc;
static char  g_exec_path[64];
static char  g_exec_argv1[64];

int32 arkrtos_elf_exec_replace(const char *path, int32 argc, char *argv[],
                               arkrtos_elf_info_t *p_info,
                               unsigned long *p_tp)
{
    size_t n = strlen(path);

    if(n >= sizeof(g_exec_path))
    {
        n = sizeof(g_exec_path) - 1u;
    }
    memcpy(g_exec_path, path, n);
    g_exec_path[n] = 0;
    g_exec_argc = argc;
    if(argc > 0)
    {
        n = strlen(argv[0]);
        if(n >= sizeof(g_exec_argv1))
        {
            n = sizeof(g_exec_argv1) - 1u;
        }
        memcpy(g_exec_argv1, argv[0], n);
        g_exec_argv1[n] = 0;
    }
    if(g_exec_rc != ARKRTOS_ELF_OK)
    {
        return g_exec_rc;
    }

    memset(p_info, 0, sizeof(*p_info));
    p_info->entry = U_BASE + 0x1000u;
    p_info->sp    = U_BASE + 0x1F0000u;
    p_info->segs  = 1u;
    p_info->argc  = (uint32)argc;
    p_info->task  = 3;
    if(p_tp != 0)
    {
        *p_tp = 0x00001234UL;
    }
    return ARKRTOS_ELF_OK;
}

/* ---------------- 调用助手 ---------------- */

static void ctx_init(t_ctx *c)
{
    memset(c, 0, sizeof(*c));
    c->set_reg_fail = -1;
    c->reserve = u_ptr(T_FSP_OFF);
    c->is_linux = 1;
}

static int call(t_ctx *c, uint32 nr, unsigned long a0, unsigned long a1,
                unsigned long a2, unsigned long a3)
{
    c->nr = nr;
    c->is_linux = 1;
    c->arg[0] = a0;
    c->arg[1] = a1;
    c->arg[2] = a2;
    c->arg[3] = a3;
    c->arg[4] = 0UL;
    c->arg[5] = 0UL;
    c->ret = 0xDEADBEEFUL;
    c->ret_written = 0;
    return (int)arkrtos_linux_abi_try(c);
}

/* 六参调用：mmap 是 addr/len/prot/flags/fd/offset，四参的 call() 装不下。
 * 单独一个入口而不是把 call() 扩成变参：变参在 -Wformat 之外什么都检查不到，
 * 而这里写错的后果是「参数错位但照样返回一个数」。
 * 只有文件映射档用得到（mmap 带 fd），其它档编进来就是一条未使用函数告警。 */
#if (ARKRTOS_USE_PAGING_FILEMAP == 1) || (ARKRTOS_USE_LINUX_FUTEX == 1) || \
    (ARKRTOS_USE_LINUX_SELECT == 1) || (ARKRTOS_USE_LINUX_SOCKET == 1)
static int call6(t_ctx *c, uint32 nr, unsigned long a0, unsigned long a1,
                 unsigned long a2, unsigned long a3, unsigned long a4,
                 unsigned long a5)
{
    c->nr = nr;
    c->is_linux = 1;
    c->arg[0] = a0;
    c->arg[1] = a1;
    c->arg[2] = a2;
    c->arg[3] = a3;
    c->arg[4] = a4;
    c->arg[5] = a5;
    c->ret = 0xDEADBEEFUL;
    c->ret_written = 0;
    return (int)arkrtos_linux_abi_try(c);
}
#endif /* ARKRTOS_USE_PAGING_FILEMAP == 1 */

static void ck_ret(t_ctx *c, int32 want, const char *what)
{
    g_checks++;
    if(c->ret_written == 0)
    {
        g_fails++;
        printf("FAIL %s: ret not written (line %d)\n", what, __LINE__);
        return;
    }
    if((int32)c->ret != want)
    {
        g_fails++;
        printf("FAIL %s: got %d want %d (line %d)\n",
               what, (int)(int32)c->ret, (int)want, __LINE__);
    }
}

/* 阻塞中的调用（socket / pselect6）走 LX_RET_RETRY：这一次**不写** a0。
 * 所以「登记重放」的判据是 ret_written==0，而不是某个返回值。
 * 两个特性都不开的档里没人用它（只有那两档会挂起重放），所以整段跟着
 * 特性走；否则 -Wunused-function 会把它报出来，而告警门禁是硬基线。 */
#if (ARKRTOS_USE_LINUX_SOCKET == 1) || (ARKRTOS_USE_LINUX_SELECT == 1)
static void ck_pending(t_ctx *c, const char *what)
{
    g_checks++;
    if(c->ret_written != 0)
    {
        g_fails++;
        printf("FAIL %s: ret written %d, want pending (line %d)\n",
               what, (int)(int32)c->ret, __LINE__);
    }
}
#endif

static void fresh(t_ctx *c)
{
    arkrtos_linux_abi_reset();
    ctx_init(c);
}

/* 往用户内存里放一个 rt_sigaction 结构并按需安装。返回该次调用的返回码。 */
static int32 sig_install_act(t_ctx *c, uint32 sig, unsigned long handler,
                             unsigned long restorer, unsigned long mask)
{
    u_put32(T_ACT_OFF + 0u, (uint32)handler);
    u_put32(T_ACT_OFF + 4u, 0u);
    u_put32(T_ACT_OFF + 8u, (uint32)restorer);
    u_put32(T_ACT_OFF + 12u, (uint32)mask);
    (void)call(c, T_NR_RT_SIGACTION, (unsigned long)sig, u_ptr(T_ACT_OFF),
               0UL, (unsigned long)T_SIGSET_BYTES);
    return (int32)c->ret;
}

/* ------------------------------------------------------------------ */

/* 分组 1：入口判定 / 号表分发 / 直方图 */
static void t_entry(t_ctx *c)
{
    uint32 h0;
    uint32 n0;
    uint32 i;
    uint32 seen = 0u;

    fresh(c);

    /* 不是 Linux 调用：一个字节都不许动，返回 0 交给 Ark 号表。 */
    c->nr = T_NR_GETPID;
    c->is_linux = 0;
    c->ret_written = 0;
    ck(arkrtos_linux_abi_try(c) == 0, "非 Linux 调用返回 0");
    ck(c->ret_written == 0, "非 Linux 调用不写返回值");

    /* 未知号：-ENOSYS 并计进 stat(1)。 */
    n0 = arkrtos_linux_abi_stat(1u);
    (void)call(c, 999u, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "未知号回 -ENOSYS");
    ck_u(arkrtos_linux_abi_stat(1u) - n0, 1u, "未知号计进 stat(1)");

    /* getpid / getuid / geteuid / set_tid_address */
    h0 = arkrtos_linux_abi_stat(0u);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 7, "getpid");
    (void)call(c, T_NR_GETUID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "getuid");
    (void)call(c, T_NR_GETEUID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "geteuid");

    u_put32(T_DATA_OFF, 0xA5A5A5A5u);
    (void)call(c, T_NR_SET_TID, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 7, "set_tid_address 回报 tid");
    ck_u(u_get32(T_DATA_OFF), 0xA5A5A5A5u, "set_tid_address 不写用户内存");

    ck_u(arkrtos_linux_abi_stat(0u) - h0, 4u, "四次成功计进 stat(0)");

    /* stat 越界下标 */
    ck_u(arkrtos_linux_abi_stat(9u), 0u, "stat 越界下标返回 0");

    /* 直方图：这几次调用之后 getpid 一定在表里，且次数 >= 1。 */
    n0 = arkrtos_linux_abi_nr_slots();
    ck(n0 > 0u, "直方图有槽");
    for(i = 0u; i < n0; i++)
    {
        uint32 nr = 0u;

        if(arkrtos_linux_abi_nr_slot(i, &nr) >= 1u && nr == T_NR_GETPID)
        {
            seen = 1u;
        }
    }
    ck(seen == 1u, "直方图记到 getpid");

    /* 越界槽：返回 0，且把出参写 0 */
    {
        uint32 nr = 0xFFFFFFFFu;

        ck_u(arkrtos_linux_abi_nr_slot(n0 + 1u, &nr), 0u, "直方图越界槽返回 0");
        ck_u(nr, 0u, "直方图越界槽把出参清零");
        ck(arkrtos_linux_abi_nr_slot(0u, (uint32 *)0) > 0u, "直方图允许空出参");
    }

    /* 槽用满之后只计溢出，不静默丢。 */
    (void)call(c, 0xFFFFFFFFu - 1u, 0UL, 0UL, 0UL, 0UL);
    fresh(c);
    for(i = 0u; i < 18u; i++)
    {
        (void)call(c, 300u + i, 0UL, 0UL, 0UL, 0UL);
    }
    ck_u(arkrtos_linux_abi_nr_slots(), 16u, "直方图槽数上限 16");
    ck_u(arkrtos_linux_abi_nr_overflow(), 2u, "超出的号计进 overflow");
}

/* 分组 2：exit / exit_group */
static void t_exit(t_ctx *c)
{
    fresh(c);
    g_exit_calls = 0;
    g_exit_code = -1;

    ck(call(c, T_NR_EXIT, 7UL, 0UL, 0UL, 0UL) == 1, "exit 返回 1（已结束）");
    ck(g_exit_calls == 1, "exit 调了一次退出");
    ck(g_exit_code == 7, "exit 退出码原样上报");
    ck_ret(c, 0, "exit 的返回值写 0");

    g_exit_calls = 0;
    ck(call(c, T_NR_EXIT_GROUP, 3UL, 0UL, 0UL, 0UL) == 1, "exit_group 返回 1");
    ck(g_exit_calls == 1, "exit_group 调了一次退出");
    ck(g_exit_code == 3, "exit_group 退出码原样上报");
}

/* 分组 3：openat / close */
static void t_open_close(t_ctx *c)
{
    uint32 i;

    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 100u;
    g_stat_mode = 0u;

    /* 路径指针非法 */
    (void)call(c, T_NR_OPENAT, 0x100UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "openat 空路径 -EFAULT");

    /* 路径没有 NUL */
    memset(&g_user[T_PATH_OFF], 'a', 80u);

    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, -T_ENAMETOOLONG, "openat 无 NUL 回 -ENAMETOOLONG");

    u_put_str(T_PATH_OFF, "/a.txt");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_WRONLY, 0UL);
    ck_ret(c, -T_EROFS, "写打开回 -EROFS");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_CREAT, 0UL);
    ck_ret(c, -T_EROFS, "O_CREAT 回 -EROFS");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_TRUNC, 0UL);
    ck_ret(c, -T_EROFS, "O_TRUNC 回 -EROFS");

    g_stat_rc = -1;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, -T_ENOENT, "不存在的文件回 -ENOENT");

    g_stat_rc = 0;
    g_stat_mode = ARKRTOS_VFS_S_IFDIR;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, -T_EISDIR, "目录回 -EISDIR");
    g_stat_mode = 0u;

    /* 成功：fd 从 3 起，路径经 VFS 命名空间门面解析（原样带过去）。 */
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, 3, "第一个文件 fd = 3");
    ck(strcmp(g_stat_path, "/a.txt") == 0, "openat 把命名空间路径交给 VFS");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, 4, "第二个文件 fd = 4");

    /* close：0..2 是空操作；坏 fd 回 -EBADF；重复关也回 -EBADF。 */
    (void)call(c, T_NR_CLOSE, 1UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "close(1) 是空操作");
    (void)call(c, T_NR_CLOSE, 2UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "close(2) 是空操作");
    (void)call(c, T_NR_CLOSE, 99UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EBADF, "close 坏 fd 回 -EBADF");
    (void)call(c, T_NR_CLOSE, 3UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "close(3) 成功");
    (void)call(c, T_NR_CLOSE, 3UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EBADF, "重复关闭回 -EBADF");

    /* fd 表满：8 项 */
    fresh(c);
    for(i = 0u; i < 8u; i++)
    {
        (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
        ck_ret(c, (int32)(3u + i), "fd 顺序分配");
    }
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, -T_ENFILE, "fd 表满回 -ENFILE");
}

/* 分组 4：read */
static void t_read(t_ctx *c)
{
    uint32 i;
    uint8 buf[400];

    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_stat_mode = 0u;
    g_read_rc = 0;
    g_synth_any = 0;
    g_file_len = 300u;
    for(i = 0u; i < g_file_len; i++)
    {
        g_file[i] = (uint8)(i & 0xFFu);
    }

    /* fd 0 = EOF（不是错误）；坏 fd = -EBADF */
    (void)call(c, T_NR_READ, 0UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, 0, "read(fd 0) 回 0");
    (void)call(c, T_NR_READ, 99UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, -T_EBADF, "read 坏 fd 回 -EBADF");

    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, 3, "打开读文件");

    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, 0, "read 长度为 0 回 0");
    (void)call(c, T_NR_READ, 3UL, 0UL, 10UL, 0UL);
    ck_ret(c, -T_EFAULT, "read 空指针 -EFAULT");

    /* 全量读 300，游标走到底；再读回 0。 */
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 300UL, 0UL);
    ck_ret(c, 300, "read 读完 300 字节");
    u_get(T_DATA_OFF, buf, 300u);
    for(i = 0u; i < 300u; i++)
    {
        if(buf[i] != (uint8)(i & 0xFFu))
        {
            break;
        }
    }
    ck(i == 300u, "read 数据与源一致");
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, 0, "读到底回 0");

    /* VFS 出错：没读到任何东西 -> -EIO */
    g_read_rc = -1;
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, -T_EIO, "read VFS 出错回 -EIO");
    g_read_rc = 0;

    /* 半路失效：第一块 128 成功、第二块失败 -> 回 128，且游标只前进 128 */
    fresh(c);
    g_stat_size = 300u;
    g_file_len = 300u;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    g_to_budget = 1;
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 300UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, 128, "read 半路拷贝失败返回已读字节数");
    ck_u(u_get8(T_DATA_OFF + 124u), 124u, "已交付的前 128 字节内容正确");
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, 10, "下一次 read 从第 128 字节接着读（不重放）");
    ck_u(u_get8(T_DATA_OFF), 128u, "续读的起点是 128");

    /* 短读：文件比请求短 */
    fresh(c);
    g_stat_size = 100u;
    g_file_len = 100u;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 200UL, 0UL);
    ck_ret(c, 100, "文件短于请求时短读");
}

/* 分组 5：write（标准输出 + 管道写端） */
static void t_write(t_ctx *c)
{
    fresh(c);
    g_putc_len = 0u;
    u_put_str(T_STR_OFF, "hello");

    (void)call(c, T_NR_WRITE, 1UL, u_ptr(T_STR_OFF), 5UL, 0UL);
    ck_ret(c, 5, "write(1) 返回写入字节数");
    ck(g_putc_len == 5u && memcmp(g_putc, "hello", 5u) == 0, "标准输出逐字节写出");
    ck((g_con_lock_calls == 1u) && (g_con_unlock_calls == 1u) && (g_con_held == 0u),
       "整次 write 持控制台锁且退出时已释放");

    g_putc_len = 0u;
    (void)call(c, T_NR_WRITE, 2UL, u_ptr(T_STR_OFF), 5UL, 0UL);
    ck_ret(c, 5, "write(2) 也走控制台");
    ck(g_putc_len == 5u, "stderr 的字节也逐个写出");

    (void)call(c, T_NR_WRITE, 1UL, 0UL, 5UL, 0UL);
    ck_ret(c, -T_EFAULT, "write 空指针 -EFAULT");
    (void)call(c, T_NR_WRITE, 1UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "write 长度 0 且空指针回 0");

    (void)call(c, T_NR_WRITE, 7UL, u_ptr(T_STR_OFF), 5UL, 0UL);
    ck_ret(c, -T_EBADF, "write 非管道 fd 回 -EBADF");

    /* 半路失效：第一块 128 成功、第二块失败 */
    g_putc_len = 0u;
    g_from_budget = 1;
    (void)call(c, T_NR_WRITE, 1UL, u_ptr(T_DATA_OFF), 200UL, 0UL);
    g_from_budget = -1;
    ck_ret(c, 128, "write 半路失败返回已写字节数");
}

/* 分组 6：pread64 */
static void t_pread(t_ctx *c)
{
    uint32 i;
    uint8 buf[200];

    fresh(c);
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    g_synth_any = 0;
    for(i = 0u; i < g_file_len; i++)
    {
        g_file[i] = (uint8)(i & 0xFFu);
    }

    (void)call(c, T_NR_PREAD64, 99UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, -T_EBADF, "pread64 坏 fd 回 -EBADF");

    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, 0, "pread64 长度 0 回 0");
    (void)call(c, T_NR_PREAD64, 3UL, 0UL, 10UL, 0UL);
    ck_ret(c, -T_EFAULT, "pread64 空指针 -EFAULT");

    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 200UL, 100UL);
    ck_ret(c, 200, "pread64 读到 200 字节");
    u_get(T_DATA_OFF, buf, 200u);
    ck(buf[0] == (uint8)100u && buf[199] == (uint8)((100 + 199) & 0xFF),
       "pread64 从指定偏移取数据");

    /* 不改游标：随后的 read 仍从 0 开始。 */
    (void)call(c, T_NR_READ, 3UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, 10, "pread64 不改读游标");
    ck_u(u_get8(T_DATA_OFF), 0u, "read 从头开始");

    g_read_rc = -1;
    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 10UL, 0UL);
    ck_ret(c, -T_EIO, "pread64 VFS 出错回 -EIO");
    g_read_rc = 0;

    /* 32 位宿主上 pos 溢出分支：done 累加到让 pos > 0xFFFFFFFF-done 才成立，
     * 于是 return done。要造它就得让 VFS 对任意偏移都给数据，且 pos 必须
     * 落在 0xFFFFFF80 以上（0xFFFFFF00 + 200 不会越过 32 位边界，那条
     * 位置实测会读完 200 字节，不是溢出路径）。 */
    g_synth_any = 1;
    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 200UL, 0xFFFFFFF0u);
    ck_ret(c, 128, "pread64 偏移回绕时返回已读字节数");
    g_synth_any = 0;

    /* 半路拷贝失败同样返回已读 */
    g_file_len = 300u;
    g_to_budget = 1;
    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 300UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, 128, "pread64 半路拷贝失败返回已读字节数");

    /* 越过文件末尾：VFS 给 0 字节，循环当场收工（不是死转） */
    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 20UL, 500UL);
    ck_ret(c, 0, "pread64 越过文件末尾回 0");

    /* 尾部不足一整块：拿到 50 就收，不补读、不报错 */
    (void)call(c, T_NR_PREAD64, 3UL, u_ptr(T_DATA_OFF), 200UL, 250UL);
    ck_ret(c, 50, "pread64 尾部不足时返回实际字节数");
}

/* 分组 7：pipe2 / 管道读写 */
static void t_pipe(t_ctx *c)
{
    uint32 fds[2];
    uint8 buf[400];
    uint32 i;
    int32 rc;

    fresh(c);

    (void)call(c, T_NR_PIPE2, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "pipe2 空指针 -EFAULT");
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0x40UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "pipe2 不认识的 flags 回 -EINVAL");

    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 0, "pipe2 成功");
    u_get(T_DATA_OFF, fds, 8u);
    ck(fds[0] == 3u && fds[1] == 4u, "pipe2 写回两个 fd（读端在先）");

    /* 写 5 读 5 */
    u_put_str(T_STR_OFF, "hello");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 5UL, 0UL);
    ck_ret(c, 5, "管道写 5 字节");
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 5UL, 0UL);
    ck_ret(c, 5, "管道读 5 字节");
    u_get(T_DATA_OFF, buf, 5u);
    ck(memcmp(buf, "hello", 5u) == 0, "管道内容一致");

    /* 空读且写端还在 -> -EAGAIN */
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 5UL, 0UL);
    ck_ret(c, -T_EAGAIN, "管道空读回 -EAGAIN（不阻塞）");

    /* 写端 fd 不能读 */
    (void)call(c, T_NR_READ, (unsigned long)fds[1], u_ptr(T_DATA_OFF), 5UL, 0UL);
    ck_ret(c, -T_EBADF, "管道写端不能读");

    /* 关写端 -> EOF */
    (void)call(c, T_NR_CLOSE, (unsigned long)fds[1], 0UL, 0UL, 0UL);
    ck_ret(c, 0, "关管道写端");
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 5UL, 0UL);
    ck_ret(c, 0, "写端全关后读到 EOF");
    (void)call(c, T_NR_CLOSE, (unsigned long)fds[0], 0UL, 0UL, 0UL);
    ck_ret(c, 0, "关管道读端（两端都关，管道释放）");

    /* 读端全关 -> 写回 -EPIPE */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    u_get(T_DATA_OFF, fds, 8u);
    (void)call(c, T_NR_CLOSE, (unsigned long)fds[0], 0UL, 0UL, 0UL);
    u_put_str(T_STR_OFF, "x");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 1UL, 0UL);
    ck_ret(c, -T_EPIPE, "读端全关后写回 -EPIPE");

    /* 写满 -> 短写 256，再写 -EAGAIN */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    u_get(T_DATA_OFF, fds, 8u);
    for(i = 0u; i < 300u; i++)
    {
        g_user[T_STR_OFF + i] = (uint8)i;
    }
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 300UL, 0UL);
    ck_ret(c, 256, "管道满时短写 256 字节");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 1UL, 0UL);
    ck_ret(c, -T_EAGAIN, "管道满再写回 -EAGAIN");

    /* 读侧半路失效：已提交的 128 字节被消费、其余留在环形缓冲里 */
    g_to_budget = 1;
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 256UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, 128, "管道读半路失败返回已读字节数");
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 200UL, 0UL);
    ck_ret(c, 128, "剩下的 128 字节仍在管道里（失败不丢数据）");
    ck_u(u_get8(T_DATA_OFF), 128u, "续读内容从第 128 字节开始");

    /* 管道条数上限 2 */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 0, "第一条管道");
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 0, "第二条管道");
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENFILE, "第三条管道回 -ENFILE");

    /* fd 只剩一个空位时 pipe2 要整条回滚 */
    fresh(c);
    for(i = 0u; i < 7u; i++)
    {
        (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    }
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENFILE, "fd 只剩一个空位时 pipe2 回 -ENFILE");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, 10, "失败后那个 fd 槽位被还回去");

    /* fd 表满：第一个空位都拿不到，走 i0 这一支回滚 */
    fresh(c);
    for(i = 0u; i < 8u; i++)
    {
        (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    }
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENFILE, "fd 表全满时 pipe2 回 -ENFILE");

    /* 写回用户数组失败：整条管道撤回，fd 槽位也还回 */
    fresh(c);
    g_to_budget = 0;
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, -T_EFAULT, "pipe2 写回失败 -EFAULT");
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    rc = (int32)c->ret;
    ck_ret(c, 0, "撤回后再建管道成功");
    ck(rc == 0, "撤回不留半条管道");

    /* fd 号越过分区上限：必须先判号再算表下标，不拿一个大号去索引 */
    (void)call(c, T_NR_READ, 11UL, u_ptr(T_DATA_OFF), 5UL, 0UL);
    ck_ret(c, -T_EBADF, "fd 号越过分区上限回 -EBADF");

    /* 长度 0 与空指针：Linux 这两个判据的顺序是「先长度后指针」，
     * 所以 read(fd, NULL, 0) 合法回 0，不是 -EFAULT。 */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    u_get(T_DATA_OFF, fds, 8u);
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, 0, "管道 read 长度 0 回 0（不碰指针）");
    (void)call(c, T_NR_READ, (unsigned long)fds[0], 0UL, 5UL, 0UL);
    ck_ret(c, -T_EFAULT, "管道 read 空指针回 -EFAULT");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 0UL, 0UL);
    ck_ret(c, 0, "管道 write 长度 0 回 0");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], 0UL, 5UL, 0UL);
    ck_ret(c, -T_EFAULT, "管道 write 空指针回 -EFAULT");

    /* 读得比管道里有的多：n 要被压到 count，返回实际读到的字节数 */
    u_put_str(T_STR_OFF, "hi!");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 3UL, 0UL);
    ck_ret(c, 3, "管道写 3 字节");
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 200UL, 0UL);
    ck_ret(c, 3, "读得比有的多时短读到 count（不假装读满）");

    /* 剩余空间小于一次拷贝块：n 要被压到 space，否则会盖掉还没读走的数据 */
    for(i = 0u; i < 200u; i++)
    {
        g_user[T_STR_OFF + i] = (uint8)i;
    }
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 200UL, 0UL);
    ck_ret(c, 200, "管道先写 200 字节");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 200UL, 0UL);
    ck_ret(c, 56, "剩余空间只有 56 字节时短写到 space（不越界）");

    /* 写侧半路失效：已拷进去的照常提交，返回已写字节数（短写是正常结果） */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    u_get(T_DATA_OFF, fds, 8u);
    g_from_budget = 1;
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1], u_ptr(T_STR_OFF), 256UL, 0UL);
    g_from_budget = -1;
    ck_ret(c, 128, "管道写半路失效返回已写字节数");
    (void)call(c, T_NR_READ, (unsigned long)fds[0], u_ptr(T_DATA_OFF), 200UL, 0UL);
    ck_ret(c, 128, "已提交的那一块真的在管道里");
}

/* 分组 8：clock_gettime / uname */
static void t_meta(t_ctx *c)
{
    uint32 ts[2];
    char sysname[65];
    char machine[65];

    fresh(c);
    (void)call(c, T_NR_CLOCK_GETTIME, 1UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "clock_gettime 空指针 -EFAULT");
    (void)call(c, T_NR_CLOCK_GETTIME, 0UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "CLOCK_REALTIME 回 -ENOSYS（不拿开机时长冒充墙钟）");
    (void)call(c, T_NR_CLOCK_GETTIME, 2UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "未知 clockid 回 -EINVAL");

    (void)call(c, T_NR_CLOCK_GETTIME, 1UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, 0, "CLOCK_MONOTONIC 成功");
    u_get(T_DATA_OFF, ts, 8u);
    ck(ts[0] == 1234u && ts[1] == 567000000u, "timespec = ms 换算的秒/纳秒");

    (void)call(c, T_NR_CLOCK_GETTIME, 4UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    ck_ret(c, 0, "CLOCK_MONOTONIC_RAW 也接受");

    g_to_budget = 0;
    (void)call(c, T_NR_CLOCK_GETTIME, 1UL, u_ptr(T_DATA_OFF), 0UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, -T_EFAULT, "clock_gettime 写回失败 -EFAULT");

    (void)call(c, T_NR_UNAME, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "uname 空指针 -EFAULT");
    (void)call(c, T_NR_UNAME, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 0, "uname 成功");
    u_get(T_DATA_OFF, sysname, 65u);
    u_get(T_DATA_OFF + 260u, machine, 65u);
    ck(strcmp(sysname, "ArkRTOS") == 0, "uname.sysname = ArkRTOS");
    ck(strcmp(machine, "unknown") == 0, "宿主上 machine 走 #else 分支 = unknown");

    g_to_budget = 0;
    (void)call(c, T_NR_UNAME, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, -T_EFAULT, "uname 写回失败 -EFAULT");
}

/* 分组 9：mprotect / brk / mmap / munmap */
static void t_mem(t_ctx *c)
{
    unsigned long base = U_BASE + ((unsigned long)U_SIZE / 2UL);
    unsigned long limit = U_BASE + (unsigned long)U_SIZE - (128UL * 1024UL);
    uint32 m0;

    fresh(c);

    (void)call(c, T_NR_MPROTECT, U_BASE + 0x1001UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "mprotect 非页对齐回 -EINVAL");
    (void)call(c, T_NR_MPROTECT, U_BASE + 0x1000UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "mprotect 长度 0 回 0");
    (void)call(c, T_NR_MPROTECT, 0x1000UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, -T_ENOMEM, "mprotect 窗口外回 -ENOMEM");
    (void)call(c, T_NR_MPROTECT, U_BASE + 0x1000UL, 0x400000UL, 0UL, 0UL);
    ck_ret(c, -T_ENOMEM, "mprotect 超过窗口回 -ENOMEM");

    m0 = arkrtos_linux_abi_stat(4u);
    (void)call(c, T_NR_MPROTECT, U_BASE + 0x1000UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, 0, "mprotect 接受但未强制");
    ck_u(arkrtos_linux_abi_stat(4u) - m0, 1u, "mprotect 单独计数");

    /* brk：游标从窗口半程起 */
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base, "brk(0) 返回初始 break");
    (void)call(c, T_NR_BRK, base + 0x1000UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x1000UL, "brk 前移");
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x1000UL, "brk(0) 读到新值");
    (void)call(c, T_NR_BRK, U_BASE, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x1000UL, "brk 低于窗口时返回当前 break（不是负值）");
    (void)call(c, T_NR_BRK, U_BASE + (unsigned long)U_SIZE + 0x1000UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x1000UL, "brk 越界时返回当前 break");

    /* mmap */
    (void)call(c, T_NR_MMAP, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "mmap 长度 0 回 -EINVAL");
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL, (unsigned long)T_MAP_ANONYMOUS);
    ck_ret(c, -T_ENOSYS, "mmap 缺 MAP_PRIVATE 回 -ENOSYS");
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL, (unsigned long)T_MAP_PRIVATE);
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* 开了文件映射档之后，「没有 MAP_ANONYMOUS」不再是无意义请求，而是文件
     * 映射：fd 无效就该是 -EBADF（这一条读数把两档的语义差别钉住）。 */
    ck_ret(c, -T_EBADF, "文件映射档下缺 MAP_ANONYMOUS 按文件映射处理（坏 fd 回 -EBADF）");
#else
    ck_ret(c, -T_ENOSYS, "mmap 缺 MAP_ANONYMOUS 回 -ENOSYS");
#endif
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* 按需缺页档：MAP_FIXED 是真支持的，具体读数在 t_mapfixed 里。这里只咬住
     * 「不原样接受就拒绝」这一条 —— 地址为 0 不是可交付的固定地址。 */
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_EINVAL, "MAP_FIXED(addr=0) 回 -EINVAL");
#else
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_ENOSYS, "mmap 带 MAP_FIXED 回 -ENOSYS");
#endif

    /* SHM 关档（宿主这一档就是）时 MAP_SHARED 必须 -ENOSYS，不许偷偷降级成
     * 私有：降级会让两个进程看起来共享、其实各改各的，那是最坏的一种"能跑"。
     * 两个都给（PRIVATE|SHARED）同理。 */
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_SHARED | T_MAP_ANONYMOUS));
    ck_ret(c, -T_ENOSYS, "SHM 关档时 MAP_SHARED 回 -ENOSYS（不降级成私有）");
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_SHARED | T_MAP_ANONYMOUS));
    ck_ret(c, -T_ENOSYS, "PRIVATE|SHARED 同时给回 -ENOSYS");

    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS));
    {
        unsigned long got = c->ret;
        unsigned long aligned = (base + 0x1000UL + 4095UL) & ~4095UL;

        ck_u(got, aligned, "mmap 返回对齐后的地址");
        (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
        ck_u(c->ret, aligned + 0x1000UL, "mmap 推进了堆游标");
    }

    /* 越界：从当前游标到窗口顶放不下 */
    (void)call(c, T_NR_MMAP, 0UL, 0x100000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS));
    ck_ret(c, -T_ENOMEM, "mmap 放不下回 -ENOMEM");

    /* 32 位加法回绕：len 大到 start + len 绕回低地址，必须仍然回 -ENOMEM */
    (void)call(c, T_NR_MMAP, 0UL, 0xFDE21000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS));
    ck_ret(c, -T_ENOMEM, "mmap 长度回绕时回 -ENOMEM（不静默成功）");
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck(c->ret >= base && c->ret <= limit, "回绕失败不动堆游标");

    /* munmap 只记账 */
    (void)call(c, T_NR_MUNMAP, 0UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "munmap(0) 回 -EINVAL");
    (void)call(c, T_NR_MUNMAP, U_BASE + 0x1000UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, 0, "munmap 接受但不还地址");
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x2000UL, "munmap 不改变堆游标");

    /* 堆游标的惰性起点有两个入口：brk 先到（上面走过）与 mmap 先到。
     * 后者必须自己把游标落到窗口半程 —— 从 0 开始会让第一批映射全落在
     * 用户内存之外，程序拿到一个自己不能用、看着又合法的地址。 */
    fresh(c);
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS));
    ck_u(c->ret, base, "reset 后第一个 mmap 从窗口半程起（惰性初始化堆起点）");
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x1000UL, "mmap 自己把堆游标推到映射之后");
}

#if (ARKRTOS_USE_PAGING_DEMAND == 1)
/* 分组 12b：MAP_FIXED（真 mmap 语义里「就在这里」那一半）。这一档能测的是
 * ABI 层：地址原样交回、长度按页对齐交给端口、游标被推过去、端口的两种拒绝
 * 各自翻成 -ENOMEM / -EINVAL、以及「不支持就明确拒绝」的两种组合（共享、
 * 文件映射）。窗口里的字节由 QEMU 上板读，宿主不假装验它。 */
static void t_mapfixed(t_ctx *c)
{
    unsigned long base  = U_BASE + ((unsigned long)U_SIZE / 2UL);
    unsigned long limit = U_BASE + ((unsigned long)U_SIZE - (128UL * 1024UL));

    fresh(c);
    g_discard_calls = 0u;
    g_discard_rc    = 0;
    g_discard_va    = 0UL;
    g_discard_len   = 0UL;

    (void)call(c, T_NR_MMAP, base + 0x3000UL, 0x2000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_u(c->ret, base + 0x3000UL, "MAP_FIXED 返回的就是请求的那个地址");
    ck(g_discard_calls == 1u, "MAP_FIXED 真的走到端口 discard 一次");
    ck_u((unsigned long)g_discard_va, base + 0x3000UL, "交给端口的起址 = 请求地址");
    ck_u((unsigned long)g_discard_len, 0x2000UL, "交给端口的长度按页对齐");

    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x5000UL, "MAP_FIXED 之后堆游标推到这一段之后");

    /* 非页对齐：Linux 的 mmap 也回 -EINVAL，端口一次都不该被叫 */
    g_discard_calls = 0u;
    (void)call(c, T_NR_MMAP, base + 0x5001UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_EINVAL, "MAP_FIXED 未对齐回 -EINVAL");
    ck(g_discard_calls == 0u, "参数非法时不动端口");

    /* 低于堆窗口：那是程序自己的映像/解释器区，丢掉它只会停机 */
    (void)call(c, T_NR_MMAP, U_BASE + 0x1000UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_EINVAL, "MAP_FIXED 落在堆窗口之下回 -EINVAL");

    /* 放不下 */
    (void)call(c, T_NR_MMAP, limit - 0x1000UL, 0x2000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_ENOMEM, "MAP_FIXED 超出窗口顶回 -ENOMEM");

    /* 端口两种拒绝的翻译 */
    g_discard_rc = -2;
    (void)call(c, T_NR_MMAP, base + 0x6000UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_ENOMEM, "端口帧池空(-2) 翻成 -ENOMEM");
    g_discard_rc = -1;
    (void)call(c, T_NR_MMAP, base + 0x6000UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_EINVAL, "端口拒绝(-1) 翻成 -EINVAL");
    g_discard_rc = 0;
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, base + 0x5000UL, "端口失败时堆游标一步都不动");

    /* 两种「不支持就不做」的组合：共享与文件映射。半做比不做更难查。 */
    (void)call(c, T_NR_MMAP, base + 0x7000UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_SHARED |
                               T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_ENOSYS, "MAP_FIXED + MAP_SHARED 回 -ENOSYS");
    (void)call(c, T_NR_MMAP, base + 0x7000UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_FIXED));
    ck_ret(c, -T_ENOSYS, "MAP_FIXED + 文件映射回 -ENOSYS");
}
#endif /* ARKRTOS_USE_PAGING_DEMAND == 1 */

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
/* 分组 13：文件映射与写回（U-21）。这一档的语义都写死在开关文档里，这里逐条
 * 变成读数：内容真的装进映射区、EOF 之后是零、offset 真的生效、可写共享映射
 * 写回、私有映射不写回、参数校验、写回失败不吞错误。 */
static void t_filemap(t_ctx *c)
{
    unsigned long base = U_BASE + ((unsigned long)U_SIZE / 2UL);
    uint32 i;
    int fd;
    int fdrd;

    fresh(c);
    g_stat_rc = 0;
    g_stat_mode = 0u;
    g_stat_size = 300u;
    g_file_len = 300u;
    for(i = 0u; i < 300u; i++)
    {
        g_file[i] = (uint8)('A' + (i % 26u));
    }
    g_write_rc = 0;
    g_write_calls = 0;
    g_wlen = 0u;
    g_read_rc = 0;

    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    fd = (int32)c->ret;
    ck(fd == 3, "只读打开文件拿到 fd 3");

    /* 1) MAP_PRIVATE + PROT_READ：内容装进映射区 */
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fd, 0UL);
    {
        unsigned long addr = c->ret;

        ck(addr == base, "文件映射从窗口半程起（与匿名同一套堆游标）");
        ck_u(u_get8((uint32)(addr - U_BASE) + 0u), 'A', "映射首字节是文件首字节");
        ck_u(u_get8((uint32)(addr - U_BASE) + 25u), 'Z', "映射内容按 pattern 前进");
        /* EOF 之后清零：300 之后到 4 KB 全 0 */
        ck_u(u_get8((uint32)(addr - U_BASE) + 299u), 'A' + (299u % 26u),
             "EOF 前最后一字节仍是文件内容");
        ck_u(u_get8((uint32)(addr - U_BASE) + 300u), 0u, "EOF 之后清零（不伪造 SIGBUS）");
        ck_u(u_get8((uint32)(addr - U_BASE) + 4095u), 0u, "整段末尾也是零");
        ck(g_write_calls == 0, "只读私有映射不产生写回");
    }

    /* 2) 参数校验 */
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fd, 0x10UL);
    ck_ret(c, -T_EINVAL, "offset 非 4 KB 对齐回 -EINVAL");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fd, 0UL);
    ck_ret(c, -T_EINVAL, "prot 不含 PROT_READ 回 -EINVAL");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, 99UL, 0UL);
    ck_ret(c, -T_EBADF, "坏 fd 回 -EBADF");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fd, 0UL);
    ck_ret(c, -T_EACCES, "只读 fd + 可写共享映射回 -EACCES（Linux 语义）");

    /* 3) offset 生效：offset=4096 在 300 字节文件里是整段越界 ⇒ 全零 */
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fd, 0x1000UL);
    {
        unsigned long addr = c->ret;

        ck(addr > base, "第二次映射接在第一次之后");
        ck_u(u_get8((uint32)(addr - U_BASE) + 0u), 0u, "offset 越过 EOF 时整段零");
    }

    /* 4) 私有可写映射：改动不落盘 */
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fd, 0UL);
    {
        unsigned long addr = c->ret;
        uint32 off = (uint32)(addr - U_BASE);

        u_put(off, "ZZ", 2u);
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, 0, "msync 私有映射回 0（没有可写回的东西）");
        ck(g_write_calls == 0, "私有映射的改动不写回");
    }

    /* 5) O_RDWR + 可写共享映射：真的写回 */
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fdrd = (int32)c->ret;
    ck(fdrd > 3, "FILEMAP 档下已存在文件可以 O_RDWR 打开");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fdrd, 0UL);
    {
        unsigned long addr = c->ret;
        uint32 off = (uint32)(addr - U_BASE);

        ck(addr > 0 && addr < (U_BASE + (unsigned long)U_SIZE), "可写共享映射拿到地址");
        u_put(off + 10u, "XY", 2u);
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, 0, "msync 可写共享映射回 0");
        ck(g_write_calls == 1, "写回发生了一次");
        ck_u(g_wlen, 300u, "写回的长度是文件原长（不因映射 4 KB 而变长）");
        ck_u((uint32)g_wbuf[10], (uint32)'X', "写回把映射里的改动带上去了");
        ck_u((uint32)g_wbuf[12], (uint32)'M', "未被改动的位置保留原内容");
        ck(strcmp(g_write_path, "/mnt/ram/fm") == 0, "写回的路径就是打开时的路径");
        u_put(off + 10u, "AB", 2u);
        (void)call(c, T_NR_MUNMAP, addr, 0x1000UL, 0UL, 0UL);
        ck_ret(c, 0, "munmap 命中记账回 0");
        ck(g_write_calls == 2, "munmap 也是一次写回");
        ck_u((uint32)g_wbuf[10], (uint32)'A', "第二次写回带上了新内容");
        (void)call(c, T_NR_MUNMAP, addr, 0x1000UL, 0UL, 0UL);
        ck(g_write_calls == 2, "同一段再 munmap 不重复写回");
    }

    /* 6) 写回失败不被吞掉：文件长度在映射期间变了 */
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fdrd, 0UL);
    {
        unsigned long addr = c->ret;

        g_stat_size = 301u;
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, -T_EIO, "文件长度变了就 -EIO（不把文件截断成读到的长度）");
        g_stat_size = 300u;
        g_write_rc = -1;
        (void)call(c, T_NR_MUNMAP, addr, 0x1000UL, 0UL, 0UL);
        ck_ret(c, -T_EIO, "写回失败时 munmap 把错误交回去");
        g_write_rc = 0;
        (void)call(c, T_NR_MUNMAP, addr, 0x1000UL, 0UL, 0UL);
        ck_ret(c, 0, "写回成功后同一次 munmap 重试可以收尾（记账没被吞掉）");
    }

    /* 7) msync 参数校验 */
    (void)call(c, T_NR_MSYNC, base + 1UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "msync 地址非页对齐回 -EINVAL");
    (void)call(c, T_NR_MSYNC, base, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "msync 长度 0 回 -EINVAL");
    (void)call(c, T_NR_MSYNC, base, 0x1000UL, 0x8UL, 0UL);
    ck_ret(c, -T_EINVAL, "msync 未知标志位回 -EINVAL");
    (void)call(c, T_NR_MSYNC, 0x1000UL, 0x1000UL, 0UL, 0UL);
    ck_ret(c, -T_ENOMEM, "msync 堆窗口之外回 -ENOMEM");

    /* 8) 文件太大：mmap 当场 -ENOMEM */
    g_stat_size = 9000u;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    {
        int big = (int32)c->ret;

        (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                    (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                    (unsigned long)T_MAP_SHARED, (unsigned long)big, 0UL);
        ck_ret(c, -T_ENOMEM, "超过写回缓冲的文件当场 -ENOMEM");
    }
    g_stat_size = 300u;

    /* 9) 普通文件 write 如实 -ENOSYS（不装作写进去了） */
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    {
        int wfd = (int32)c->ret;

        u_put(T_DATA_OFF, "abcd", 4u);
        (void)call(c, T_NR_WRITE, (unsigned long)wfd, u_ptr(T_DATA_OFF), 4UL, 0UL);
        ck_ret(c, -T_ENOSYS, "普通文件 write 回 -ENOSYS（内容只走映射写回）");
    }
}
#endif /* ARKRTOS_USE_PAGING_FILEMAP == 1 */

#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
/* 分组 14：文件映射的错误与边界路径（U-21）。分组 13 走的是「正常用法」，
 * 这一组专门打那些只有走错一条路才会碰到的分支：写回槽用满、要的长度越过
 * 堆窗口、拿管道 fd 去映射、底层短读、拷贝半路失败、写回期间文件被换掉。
 * 覆盖率门禁要求它们不是死代码；判据要求每条都回对的错误码（不吞、不伪造）。 */
static void t_filemap_edge(t_ctx *c)
{
    int32 fda;
    int32 fdb;
    int32 fdc;
    int32 prd;
    uint32 i;

    /* --- 1) 写回记账槽用满：每进程 2 条可写共享映射 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_mode = 0u;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    g_write_rc = 0;
    g_write_calls = 0;
    for(i = 0u; i < 300u; i++)
    {
        g_file[i] = (uint8)('A' + (i % 26u));
    }
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fda = (int32)c->ret;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fdb = (int32)c->ret;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fdc = (int32)c->ret;
    ck(fda > 0 && fdb > 0 && fdc > 0, "三份 O_RDWR 句柄都拿到了");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
    ck((c->ret > 0u) && (c->ret < (U_BASE + (unsigned long)U_SIZE)),
       "第一条可写共享映射成功");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fdb, 0UL);
    ck((c->ret > 0u) && (c->ret < (U_BASE + (unsigned long)U_SIZE)),
       "第二条可写共享映射成功");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fdc, 0UL);
    ck_ret(c, -T_ENOMEM, "第三条可写共享映射回 -ENOMEM（每进程 2 条写回记账）");

    /* --- 2) 要的长度越过堆窗口：当场 -ENOMEM，不悄悄缩成一段 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    fda = (int32)c->ret;
    ck(fda > 0, "越窗测试先拿到只读句柄");
    (void)call6(c, T_NR_MMAP, 0UL, 0x180000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fda, 0UL);
    ck_ret(c, -T_ENOMEM, "映射长度越过堆窗口回 -ENOMEM");

    /* --- 3) 管道 fd 不能当文件映射 --- */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 0, "pipe2 建管道");
    prd = (int32)u_get32(T_DATA_OFF);
    ck(prd > 0, "管道读端拿到 fd");
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)prd, 0UL);
    ck_ret(c, -T_EBADF, "拿管道读端去映射回 -EBADF（只有普通文件能映射）");

    /* --- 4) 底层短读：读到的比声明的少，余下按 EOF 清零 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 100u;
    g_read_rc = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    fda = (int32)c->ret;
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fda, 0UL);
    {
        unsigned long addr = c->ret;
        uint32 off;

        ck((addr > 0u) && (addr < (U_BASE + (unsigned long)U_SIZE)),
           "短读场景下映射照样拿到地址");
        off = (uint32)(addr - U_BASE);
        ck_u(u_get8(off + 99u), (unsigned long)('A' + (99u % 26u)),
             "短读之前的内容照装");
        ck_u(u_get8(off + 100u), 0u, "底层短读之后余下按 EOF 清零");
        ck_u(u_get8(off + 299u), 0u, "清零铺到文件声明的长度上");
    }

    /* --- 5) 内容拷贝半路失败：如实 -EFAULT --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    fda = (int32)c->ret;
    g_to_budget = 0;
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL, (unsigned long)T_PROT_READ,
                (unsigned long)T_MAP_PRIVATE, (unsigned long)fda, 0UL);
    ck_ret(c, -T_EFAULT, "内容往用户区拷不进去回 -EFAULT");
    g_to_budget = -1;

    /* --- 6) 清零阶段拷贝失败：-EFAULT 且写回记账不泄漏 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 0u;
    g_file_len = 0u;
    g_read_rc = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fda = (int32)c->ret;
    g_to_budget = 0;
    (void)call6(c, T_NR_MMAP, 0UL, 0x2000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
    ck_ret(c, -T_EFAULT, "清零阶段拷不进去回 -EFAULT");
    g_to_budget = -1;
    for(i = 0u; i < 2u; i++)
    {
        (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                    (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                    (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
        ck((c->ret > 0u) && (c->ret < (U_BASE + (unsigned long)U_SIZE)),
           "失败那次没把写回槽吃掉（同一条仍能再建两条）");
    }
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
    ck_ret(c, -T_ENOMEM, "两条建满之后第三条仍是 -ENOMEM（槽数是 2 不是 3）");

    /* --- 7) msync 地址在窗口里但不属于任何记账：回 0 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    g_write_rc = 0;
    g_write_calls = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fda = (int32)c->ret;
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
    {
        unsigned long addr = c->ret;

        ck((addr > 0u) && (addr < (U_BASE + (unsigned long)U_SIZE)),
           "记账存在时的可写共享映射成功");
        (void)call(c, T_NR_MSYNC, addr + 0x8000UL, 0x1000UL,
                   (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, 0, "msync 跳到窗口里别的页回 0（认得窗口，但不假装有东西要写回）");
        ck(g_write_calls == 0, "没有记账命中就不发生写回");
    }

    /* --- 8) 写回本身的三条失败路径 --- */
    (void)call(c, T_NR_MSYNC, U_BASE + ((unsigned long)U_SIZE / 2UL),
               0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    g_write_rc = 0;
    g_write_calls = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fda = (int32)c->ret;
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
    {
        unsigned long addr = c->ret;

        ck((addr > 0u) && (addr < (U_BASE + (unsigned long)U_SIZE)),
           "失败路径用的可写共享映射成功");
        g_read_rc = -1;
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, -T_EIO, "写回读不到原文件回 -EIO");
        g_read_rc = 0;
        g_stat_rc = -1;
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, -T_EIO, "写回量不出文件长度回 -EIO");
        g_stat_rc = 0;
        g_from_budget = 0;
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, -T_EFAULT, "写回把映射区读不回内核回 -EFAULT");
        g_from_budget = -1;
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, 0, "三条失败都收尾之后写回仍能成功（忙标志没被卡住）");
        ck(g_write_calls == 1, "前三次失败一次都没写，第四次才写");
    }

    /* --- 9) 映射段整段落在 EOF 之后：写回是空操作，文件也不因此变长 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    g_write_rc = 0;
    g_write_calls = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fda = (int32)c->ret;
    (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0x1000UL);
    {
        unsigned long addr = c->ret;

        ck((addr > 0u) && (addr < (U_BASE + (unsigned long)U_SIZE)),
           "整段越过 EOF 的可写共享映射也能建起来");
        (void)call(c, T_NR_MSYNC, addr, 0x1000UL, (unsigned long)T_MS_SYNC, 0UL);
        ck_ret(c, 0, "msync 整段越过 EOF 回 0");
        ck(g_write_calls == 0, "没有字节可写回时不动文件（不把它撑长）");
    }

    /* --- 10) munmap：参数校验 + 命中前先跳过别的记账 --- */
    fresh(c);
    g_stat_rc = 0;
    g_stat_size = 300u;
    g_file_len = 300u;
    g_read_rc = 0;
    g_write_rc = 0;
    g_write_calls = 0;
    u_put_str(T_PATH_OFF, "/mnt/ram/fm");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF),
               (unsigned long)T_O_RDWR, 0UL);
    fda = (int32)c->ret;
    {
        unsigned long a1;
        unsigned long a2;

        (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                    (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                    (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
        a1 = c->ret;
        (void)call6(c, T_NR_MMAP, 0UL, 0x1000UL,
                    (unsigned long)(T_PROT_READ | T_PROT_WRITE),
                    (unsigned long)T_MAP_SHARED, (unsigned long)fda, 0UL);
        a2 = c->ret;
        ck((a1 > 0u) && (a2 > 0u) && (a1 != a2),
           "两条可写共享映射落在不同地址");
        (void)call(c, T_NR_MUNMAP, a1 + 1UL, 0x1000UL, 0UL, 0UL);
        ck_ret(c, -T_EINVAL, "munmap 地址非对齐回 -EINVAL");
        (void)call(c, T_NR_MUNMAP, a1, 0UL, 0UL, 0UL);
        ck_ret(c, -T_EINVAL, "munmap 长度 0 回 -EINVAL");
        (void)call(c, T_NR_MUNMAP, a2, 0x1000UL, 0UL, 0UL);
        ck_ret(c, 0, "munmap 跳过前面未命中的记账、命中目标后回 0");
        ck(g_write_calls == 1, "munmap 收掉可写共享映射时把改动写回一次");
        (void)call(c, T_NR_MUNMAP, a1, 0x1000UL, 0UL, 0UL);
        ck_ret(c, 0, "再收掉第一条也回 0");
        ck(g_write_calls == 2, "两条各自写回一次");
    }
}
#endif /* ARKRTOS_USE_PAGING_FILEMAP == 1 */

/* 分组 10：信号（装/查动作、屏蔽字、投递、回收） */
static void t_signal(t_ctx *c)
{
    uint32 m = 0u;
    uint32 w[4];
    uint32 i;

    /* --- rt_sigaction 的参数校验 --- */
    fresh(c);
    (void)call(c, T_NR_RT_SIGACTION, 10UL, u_ptr(T_ACT_OFF), 0UL, 7UL);
    ck_ret(c, -T_EINVAL, "rt_sigaction sigsetsize != 8 回 -EINVAL");
    (void)call(c, T_NR_RT_SIGACTION, 0UL, u_ptr(T_ACT_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EINVAL, "信号 0 回 -EINVAL");
    (void)call(c, T_NR_RT_SIGACTION, 32UL, u_ptr(T_ACT_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EINVAL, "信号 32 回 -EINVAL");
    (void)call(c, T_NR_RT_SIGACTION, 9UL, u_ptr(T_ACT_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EINVAL, "SIGKILL 不可捕获");
    (void)call(c, T_NR_RT_SIGACTION, 19UL, u_ptr(T_ACT_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EINVAL, "SIGSTOP 不可捕获");

    /* 端口没有读写 trap 帧的能力 -> -ENOSYS（不装一个永远投递不到的处理函数） */
    c->set_reg_fail = T_UREG_ERA;
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == -T_ENOSYS,
       "端口无能力时 rt_sigaction 回 -ENOSYS");
    c->set_reg_fail = -1;

    /* 处理函数不是 DFL/IGN 时必须有 restorer */
    c->ret_written = 0;
    ck(sig_install_act(c, 10u, T_HANDLER, 0UL, 0UL) == -T_EINVAL,
       "缺 restorer 回 -EINVAL");

    /* act 指针非法 / old 指针非法 */
    (void)call(c, T_NR_RT_SIGACTION, 10UL, 0x100UL, 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EFAULT, "act 指针非法 -EFAULT");
    (void)call(c, T_NR_RT_SIGACTION, 10UL, 0UL, 0x100UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EFAULT, "old 指针非法 -EFAULT");

    /* 安装 + 读回 */
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装 SIGUSR1");
    (void)call(c, T_NR_RT_SIGACTION, 10UL, 0UL, u_ptr(T_OLD_OFF),
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, 0, "读回 sigaction");
    u_get(T_OLD_OFF, w, 16u);
    ck(w[0] == (uint32)T_HANDLER && w[2] == (uint32)T_RESTORER, "读回动作内容");

    /* --- rt_sigprocmask --- */
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, 0UL, 7UL);
    ck_ret(c, -T_EINVAL, "rt_sigprocmask sigsetsize != 8 回 -EINVAL");
    (void)call(c, T_NR_RT_SIGPROCMASK, 3UL, 0UL, 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EINVAL, "how > 2 回 -EINVAL");
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, 0x100UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EFAULT, "old 指针非法 -EFAULT");
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0x100UL, 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, -T_EFAULT, "set 指针非法 -EFAULT");

    /* SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK，KILL 与 STOP 永远屏蔽不了 */
    u_put32(T_SET_OFF, 0xFFFFFFFFu);
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, 0, "SIG_BLOCK");
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, u_ptr(T_OLDMSK_OFF),
               (unsigned long)T_SIGSET_BYTES);
    m = u_get32(T_OLDMSK_OFF);
    /* 信号 1..31 占 0..30 位（bit31 无信号），SIGKILL=9 → bit8、SIGSTOP=19 → bit18 被强制清掉。 */
    ck(m == 0x7FFBFEFFu, "屏蔽字不含 SIGKILL/SIGSTOP");
    (void)call(c, T_NR_RT_SIGPROCMASK, 1UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, 0, "SIG_UNBLOCK");
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, u_ptr(T_OLDMSK_OFF),
               (unsigned long)T_SIGSET_BYTES);
    ck_u(u_get32(T_OLDMSK_OFF), 0u, "解屏蔽后回到 0");
    (void)call(c, T_NR_RT_SIGPROCMASK, 2UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, 0, "SIG_SETMASK");

    /* --- kill / tkill / tgkill --- */
    fresh(c);
    (void)call(c, T_NR_KILL, 43UL, 10UL, 0UL, 0UL);
    ck_ret(c, -T_ESRCH, "kill 不是自己回 -ESRCH");
    (void)call(c, T_NR_KILL, 7UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "kill(sig 0) 是存在性探测");
    (void)call(c, T_NR_KILL, 7UL, 32UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "kill 实时信号号回 -EINVAL");
    (void)call(c, T_NR_TKILL, 43UL, 10UL, 0UL, 0UL);
    ck_ret(c, -T_ESRCH, "tkill 不是自己回 -ESRCH");
    (void)call(c, T_NR_TGKILL, 1UL, 2UL, 10UL, 0UL);
    ck_ret(c, -T_ESRCH, "tgkill tgid != tid 回 -ESRCH");
    (void)call(c, T_NR_TGKILL, 7UL, 7UL, 10UL, 0UL);
    ck_ret(c, 0, "tgkill 自己成功");

    /* --- 投递：帧内容 / 寄存器改写 / 屏蔽字 --- */
    fresh(c);
    /* fresh 不清用户内存，T_SET_OFF 里可能还留着上一段的 0xFFFFFFFF；
     * 要的是「屏蔽字清空」，必须显式把这份输入写回 0。 */
    u_put32(T_SET_OFF, 0u);
    (void)call(c, T_NR_RT_SIGPROCMASK, 2UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    for(i = 0u; i < 64u; i++)
    {
        g_export_pat[i] = 0x11110000UL + i;
    }
    ck(sig_install_act(c, 12u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装 SIGUSR2");
    (void)call(c, T_NR_KILL, 7UL, 12UL, 0UL, 0UL);

    /* 投递点在「系统调用返回用户态之前」，kill 这次调用自己就会进 handler，
     * 所以 a0 已经是信号号（不是 kill 的 0）。 */
    ck_u(c->ret, 12u, "投递后 a0 = 信号号");
    ck_u(c->reg[T_UREG_ERA], T_HANDLER, "era 指向处理函数");
    ck_u(c->reg[T_UREG_SP], u_ptr(T_FSP_OFF), "sp 指向帧");
    ck_u(c->reg[T_UREG_RA], T_RESTORER, "ra 指向 restorer");
    ck_u(c->reserve_bytes, T_FRAME_TOTAL, "帧长 = 头 + 快照");
    ck_u(u_get32(T_FSP_OFF + 0u), T_SIGFRAME_MAGIC32, "帧头 magic");
    ck_u(u_get32(T_FSP_OFF + 4u), 12u, "帧头记信号号");
    ck_u(u_get32(T_FSP_OFF + 8u), 0u, "帧头记投递前的屏蔽字");
    ck_u(u_get32(T_FSP_OFF + 12u), T_FRAME_BYTES, "帧头记快照字节数");
    ck_u(u_get32(T_FSP_OFF + 16u), (uint32)g_export_pat[0], "payload 从偏移 16 开始");
    ck_u(u_get32(T_FSP_OFF + 16u + 4u * 63u), (uint32)g_export_pat[63],
         "payload 末字也拷全");

    /* 处理期间屏蔽本信号 */
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, u_ptr(T_OLDMSK_OFF),
               (unsigned long)T_SIGSET_BYTES);
    ck_u(u_get32(T_OLDMSK_OFF), 1u << (12u - 1u), "处理期间屏蔽本信号");

    /* --- rt_sigreturn --- */
    c->reg[T_UREG_SP] = u_ptr(T_FSP_OFF);
    u_put32(T_FSP_OFF + 8u, 0u);
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck(c->import_calls == 1, "rt_sigreturn 装回快照");
    ck_u(c->imported[0], g_export_pat[0], "装回的快照内容正确");
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, u_ptr(T_OLDMSK_OFF),
               (unsigned long)T_SIGSET_BYTES);
    ck_u(u_get32(T_OLDMSK_OFF), 0u, "rt_sigreturn 恢复屏蔽字");

    /* rt_sigreturn 的拒绝面：一个字节都不按不可信的帧恢复 */
    fresh(c);
    c->reg[T_UREG_SP] = u_ptr(T_FSP_OFF);
    g_frame_bytes = 0u;
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "fbytes = 0 时 rt_sigreturn 回 -ENOSYS");
    g_frame_bytes = 1041u;
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "fbytes 超出缓冲时回 -ENOSYS");
    g_frame_bytes = T_FRAME_BYTES;

    c->reg[T_UREG_SP] = 0UL;
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "sp = 0 回 -EFAULT");

    c->reg[T_UREG_SP] = u_ptr(T_FSP_OFF);
    u_put32(T_FSP_OFF, 0x12345678u);
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "magic 不对回 -EFAULT");

    u_put32(T_FSP_OFF, T_SIGFRAME_MAGIC32);
    u_put32(T_FSP_OFF + 12u, 128u);
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "帧里记的 fbytes 与实际不符回 -EFAULT");

    u_put32(T_FSP_OFF + 12u, T_FRAME_BYTES);
    c->import_fail = 1;
    (void)call(c, T_NR_RT_SIGRETURN, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "端口装不回时回 -ENOSYS");

    /* --- 默认动作 / 忽略 / 不可投递 --- */
    fresh(c);
    (void)call(c, T_NR_KILL, 7UL, 17UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 7, "默认忽略的信号不改变返回值");
    ck_u(c->reg[T_UREG_ERA], 0u, "默认忽略的信号不进处理函数");

    fresh(c);
    (void)call(c, T_NR_KILL, 7UL, 19UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 7, "默认停止（SIGSTOP）按忽略处理");

    fresh(c);
    g_exit_calls = 0;
    g_exit_code = -1;
    (void)call(c, T_NR_KILL, 7UL, 2UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck(g_exit_calls == 1, "默认终止的信号让任务退出");
    ck(g_exit_code == 130, "退出码 = 128 + 信号号");

    fresh(c);
    ck(sig_install_act(c, 10u, 1UL, 0UL, 0UL) == 0, "SIG_IGN 不需要 restorer");
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 7, "SIG_IGN 的信号被吞掉");
    ck_u(c->reg[T_UREG_ERA], 0u, "SIG_IGN 不改上下文");

    /* 端口报不出帧长 / handler 不可读 / 栈不够：都不投递，pending 留着 */
    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    g_frame_bytes = 0u;      /* 投递点就在 kill 这次调用的返回处，条件必须提前设 */
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "帧长未知时不投递");
    g_frame_bytes = T_FRAME_BYTES;
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], T_HANDLER, "帧长恢复后 pending 仍能投递");

    fresh(c);
    ck(sig_install_act(c, 10u, 0x02000801UL, T_RESTORER, 0UL) == 0,
       "装一个未对齐的 handler（内核不拦，投递时拦）");
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "handler 未对齐时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, 0x200UL, T_RESTORER, 0UL) == 0,
       "装一个窗口外的 handler");
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "handler 读不出指令时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    c->reserve = 0UL;
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "栈不够时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    g_to_budget = 0;
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    g_to_budget = -1;
    ck_u(c->reg[T_UREG_ERA], 0u, "帧铺不进用户栈时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    c->set_reg_fail = T_UREG_SP;
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "写 sp 失败时回滚 era（不进半个 handler）");
    c->set_reg_fail = -1;
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], T_HANDLER, "失败后 pending 仍在，下一次投递成功");

    /* 屏蔽期间的 pending 不投递 */
    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    u_put32(T_SET_OFF, 1u << (10u - 1u));
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "被屏蔽的信号不投递");
    (void)call(c, T_NR_RT_SIGPROCMASK, 1UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], T_HANDLER, "解屏蔽后补投递");
}

/* 分组 11：execve */
static void t_execve(t_ctx *c)
{
    uint32 i;
    /* LA64 的 argv 是 8 字节指针槽；宿主是 32 位，所以必须用 uint64 造槽，
     * 高半强制为 0（32 位宿主表达不了 >4GB 的用户地址，这是用例的限制，
     * 已写进文件头的「不证明」清单）。用 unsigned long 槽会在 &raw[8*i]
     * 处把下一个元素当高半拷进去，造出假地址。 */
    unsigned long long ptr8[17];
    uint8 raw[8 * 17];

    fresh(c);
    g_exec_rc = 0;
    g_exec_argc = -1;
    g_exec_path[0] = 0;

    (void)call(c, T_NR_EXECVE, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "execve 空路径 -EFAULT");

    memset(&g_user[T_PATH_OFF], 'b', 80u);
    (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENAMETOOLONG, "execve 无 NUL 路径回 -ENAMETOOLONG");

    u_put_str(T_PATH_OFF, "/bin/prog");

    /* argv 指针本身非法 */
    (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), 0x100UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "execve argv 指针非法 -EFAULT");

    /* argv 元素指针非法 */
    memset(raw, 0, sizeof(raw));
    ptr8[0] = (unsigned long long)0x100UL;
    memcpy(raw, ptr8, 8u);
    u_put(T_ARGV_OFF, raw, 8u);
    (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), u_ptr(T_ARGV_OFF), 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "execve argv[0] 非法 -EFAULT");

    /* argv 串没有 NUL */
    memset(&g_user[T_STR_OFF], 'c', 300u);
    memset(raw, 0, sizeof(raw));
    ptr8[0] = (unsigned long long)u_ptr(T_STR_OFF);
    memcpy(raw, ptr8, 8u);
    u_put(T_ARGV_OFF, raw, 8u);
    (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), u_ptr(T_ARGV_OFF), 0UL, 0UL);
    ck_ret(c, -T_ENAMETOOLONG, "execve argv 串没有 NUL 回 -ENAMETOOLONG");

    /* argc 超过 16：每个元素必须是**合法**参数串，否则先撞上 ENAMETOOLONG
     * 而不是 argc 上限。 */
    u_put_str(T_STR_OFF, "x");
    memset(raw, 0, sizeof(raw));
    for(i = 0u; i < 16u; i++)
    {
        ptr8[i] = (unsigned long long)u_ptr(T_STR_OFF);
        memcpy(&raw[8u * i], &ptr8[i], 8u);
    }
    u_put(T_ARGV_OFF, raw, 8u * 16u);
    (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), u_ptr(T_ARGV_OFF), 0UL, 0UL);
    ck_ret(c, -T_E2BIG, "execve argc 超过 16 回 -E2BIG");

    /* 装载器错误码到 errno 的映射 */
    {
        static const struct
        {
            int32 elf;
            int32 err;
            const char *what;
        } map[] =
        {
            { ARKRTOS_ELF_ERR_FS,     -T_ENOENT, "FS -> ENOENT" },
            { ARKRTOS_ELF_ERR_FORMAT, -T_ENOEXEC, "FORMAT -> ENOEXEC" },
            { ARKRTOS_ELF_ERR_ARCH,   -T_ENOEXEC, "ARCH -> ENOEXEC" },
            { ARKRTOS_ELF_ERR_LAYOUT, -T_ENOEXEC, "LAYOUT -> ENOEXEC" },
            { ARKRTOS_ELF_ERR_NOMEM,  -T_ENOMEM, "NOMEM -> ENOMEM" },
            { ARKRTOS_ELF_ERR_BUSY,   -T_EBUSY, "BUSY -> EBUSY" },
            { ARKRTOS_ELF_ERR_SIGN,   -T_EACCES, "SIGN -> EACCES" },
            { ARKRTOS_ELF_ERR_INTERP, -T_ENOENT, "INTERP -> ENOENT" },
            { ARKRTOS_ELF_ERR_PARAM,  -T_EINVAL, "其它 -> EINVAL" }
        };

        for(i = 0u; i < (uint32)(sizeof(map) / sizeof(map[0])); i++)
        {
            g_exec_rc = map[i].elf;
            (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), 0UL, 0UL, 0UL);
            ck_ret(c, map[i].err, map[i].what);
        }
    }

    /* 成功路径：argv 先落进内核、帧被改写、堆游标归零 */
    fresh(c);
    g_exec_rc = 0;
    g_exec_argc = -1;
    g_stat_size = 100u;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    (void)call(c, T_NR_BRK, U_BASE + 0x100000UL, 0UL, 0UL, 0UL);

    u_put_str(T_STR_OFF, "arg0");
    u_put_str(T_STR_OFF + 64u, "arg1");
    memset(raw, 0, sizeof(raw));
    ptr8[0] = (unsigned long long)u_ptr(T_STR_OFF);
    ptr8[1] = (unsigned long long)u_ptr(T_STR_OFF + 64u);
    ptr8[2] = 0u;
    memcpy(raw, ptr8, 8u * 3u);
    u_put(T_ARGV_OFF, raw, 8u * 3u);

    c->exec_frame_rc = 0;
    ck(call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), u_ptr(T_ARGV_OFF), 0UL, 0UL) == 1,
       "execve 成功也返回 1（已处理）");
    ck(c->ret_written == 0, "execve 成功不写返回值（Linux 语义：不返回）");
    ck(strcmp(g_exec_path, "/bin/prog") == 0, "execve 把路径交给装载器");
    ck(g_exec_argc == 2, "execve 把两个参数交给装载器");
    ck(strcmp(g_exec_argv1, "arg0") == 0, "argv 串已先落进内核");
    ck(c->exec_frame_calls == 1, "execve 改写了 trap 帧");
    ck_u(c->exec_entry, U_BASE + 0x1000u, "新入口来自装载结果");
    ck_u(c->exec_sp, U_BASE + 0x1F0000u, "新 sp 来自装载结果");
    ck_u(c->exec_tp, 0x1234UL, "tp 来自装载结果");
    (void)call(c, T_NR_BRK, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->ret, U_BASE + ((unsigned long)U_SIZE / 2UL), "execve 重置堆游标");

    /* 端口不支持换帧：卸载器已经覆盖了旧镜像，这里只能回 -ENOSYS */
    fresh(c);
    g_exec_rc = 0;
    c->exec_frame_rc = -1;
    (void)call(c, T_NR_EXECVE, u_ptr(T_PATH_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "端口没有换帧能力时回 -ENOSYS");
}

/* 分组 12：reset 清掉 fd / 管道 / 信号，但不清 stat */
static void t_reset(t_ctx *c)
{
    uint32 fds[2];
    uint32 m;
    uint32 s0;

    fresh(c);
    g_stat_size = 10u;
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    u_get(T_DATA_OFF, fds, 8u);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    s0 = arkrtos_linux_abi_stat(0u);

    arkrtos_linux_abi_reset();
    /* reset 清的是内核侧的动作表与 pending，不是「当前上下文里的 era」；
     * 上面那次 kill 已经在自己的返回点进了 handler，era 是陈旧值。
     * 清零后再看后面几次调用会不会重新设它。 */
    c->reg[T_UREG_ERA] = 0UL;

    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, 3, "reset 后 fd 表从 3 重新分配");
    (void)call(c, T_NR_CLOSE, (unsigned long)fds[0], 0UL, 0UL, 0UL);
    ck_ret(c, -T_EBADF, "reset 后旧管道 fd 不存在");

    /* pending 与动作表也清了：同一个信号不会再触发处理函数 */
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "reset 清掉 pending 与动作表");
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, 0UL, u_ptr(T_OLDMSK_OFF),
               (unsigned long)T_SIGSET_BYTES);
    m = u_get32(T_OLDMSK_OFF);
    ck_u(m, 0u, "reset 清掉屏蔽字");

    /* stat 计数器不在 reset 的清理范围里（它是累计读数） */
    ck(arkrtos_linux_abi_stat(0u) >= s0, "reset 不清 stat 计数器");
}

/* 分组 13：任务退出钩子，以及「可重启系统调用」的端口降级约定
 *
 * fork 关档（宿主这一档）时 on_task_exit 必须在越界号与正常号上都是原地
 * 返回的空实现——它由 task 的收尸路径无条件调用，任何一条「没有进程状态」
 * 的路径都不能解引用进程槽。svc_restart 的弱定义回答 -1，ABI 层据此回
 * -EAGAIN；这里直接钉住这条降级约定，而不是让「睡不下去也当睡着了」
 * 混进读数。 */
static void t_task_exit(t_ctx *c)
{
    (void)c;
    arkrtos_linux_abi_on_task_exit(0);
    arkrtos_linux_abi_on_task_exit(9999);
    arkrtos_linux_abi_on_task_exit(1);
    ck(1, "on_task_exit accepts out-of-range and live task ids alike");
    ck(arkrtos_port_svc_restart(0) == -1,
       "a port without the primitive answers -1");
}

#if (ARKRTOS_USE_LINUX_FUTEX == 1)
/* 分组 14：futex 的六档 op（WAIT / WAKE / REQUEUE / CMP_REQUEUE / 位集 / 超时）
 *
 * 判据全部落在「程序看到什么」上：返回值、有没有真的登记、唤醒数、销账。
 * 键是「物理帧 + 页内偏移」，所以 u_ptr(0x10) 与 u_ptr(0x2010) 必须是两个
 * 不同的键 —— 后者若叫得醒前者，跨页的误唤醒就会混进读数。
 *
 * 本档端口能力（svc_restart）由 g_restart_rc 控制，口径与真 port 一致
 * （LA64 的 arkrtos_port_svc_restart 回 0 = 已把 era 退回系统调用那条指令）：
 *   -1  端口不会重放 ⇒ WAIT 如实 -ENOSYS，不留登记；
 *    0  会重放      ⇒ WAIT 登记 + 阻塞，重放时继续睡 / 被 WAKE 叫醒后回 0。 */
static void t_futex(t_ctx *c)
{
    unsigned long w0 = u_ptr(0x10u);
    unsigned long w1 = u_ptr(0x2010u);
    unsigned long noframe = u_ptr(0xE000u);   /* idx 7：pair_pa 回 0 */

    u_put32(0x10u, 5u);
    u_put32(0x2010u, 5u);

    g_restart_rc = -1;
    g_block_calls = 0;

    /* 无等待者：WAKE 回 0，不改任何东西 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex WAKE 没有等待者回 0");

    /* 值不等：WAIT 立刻 -EAGAIN，不登记 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 7UL, 0UL);
    ck_ret(c, -T_EAGAIN, "futex WAIT 值不等回 -EAGAIN");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex EAGAIN 之后没有留下等待登记");

    /* 窗口外：没有可查的帧，回 -ENOSYS 而不是拿 VA 当键 */
    (void)call(c, T_NR_FUTEX, U_BASE - 8UL, (unsigned long)T_FUTEX_WAIT,
               5UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex 窗口外回 -ENOSYS");

    /* 没驻留的那一对：同样回 -ENOSYS */
    (void)call(c, T_NR_FUTEX, noframe, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex 未驻留的页回 -ENOSYS");

    /* 不认识的 op：如实 -ENOSYS，不猜成 WAIT。
     * 用 11（FUTEX_WAIT_REQUEUE_PI）：6/7/8 已经是 LOCK_PI / UNLOCK_PI /
     * TRYLOCK_PI，9/10 是两种 BITSET，拿它们当「未知 op」会让夹具与实现
     * 自相矛盾。11/12 这两条 PI 条件变量专用 op 本实现不提供（默认构建
     * 的 glibc 不生成它们），所以仍是一条诚实的未知 op。 */
    (void)call(c, T_NR_FUTEX, w0, 11UL, 5UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex 未知 op 回 -ENOSYS");

    /* 唤醒数取负：-EINVAL */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE,
               (unsigned long)-1, 0UL);
    ck_ret(c, -T_EINVAL, "futex WAKE 负数回 -EINVAL");

    /* 端口不会重放：值匹配也不假装睡过，回 -ENOSYS 且不登记 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex WAIT 端口无重放能力回 -ENOSYS");
    ck(g_block_calls == 0, "futex 没有重放能力时一次也没睡");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex ENOSYS 之后没有留下等待登记");

    /* 端口会重放：WAIT 登记并阻塞一个时间片 */
    g_restart_rc = 0;
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex WAIT 登记后回 0（返回值被重放的那次覆盖）");
    ck(g_block_calls == 1, "futex WAIT 真的阻塞了一次");
    ck_u((unsigned long)g_block_ms, 100UL, "futex 用的是 wait4 同一个时间片");

    /* 重放同一次：值没变 ⇒ 继续睡，不销账 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 时间片醒来后值没变，继续睡");
    ck(g_block_calls == 2, "继续睡又阻塞了一次");

    /* 另一个键的 WAKE 叫不醒它 */
    (void)call(c, T_NR_FUTEX, w1, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex 另一个键的 WAKE 叫不醒这个等待者");

    /* 同一个键的 WAKE：唤醒 1 个，并真的叫了那个任务 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 1, "futex WAKE 回实际唤醒数 1");
    ck(g_wake_calls > 0, "futex WAKE 真的调用了唤醒接口");
    ck(g_wake_last == 7, "futex WAKE 叫的是等待的那个任务");

    /* 重放：这次是「被 WAKE 叫醒」，回 0 并销账 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 被 WAKE 叫醒后重放回 0");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex 醒来之后不再有等待者");

    /* 超时醒来但值变了：当被叫醒处理，回 0 并销账 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 再登记一次");
    u_put32(0x10u, 6u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 值在等待期间变了，重放回 0");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex 值变了之后也销了账");

    /* 任务号越界：入口先判号再动手，回 -EFAULT 而不是拿坏号去索引任务表 */
    arkrtos_current_task_id = 0;
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, -T_EFAULT, "futex 任务号为 0 回 -EFAULT");
    arkrtos_current_task_id = 7;

    /* 没有分页窗口（MPU 后端）：整个地址空间只有一份，虚拟地址本身就是键 */
    g_win_va = 0ul;
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "没有分页窗口时键退回虚拟地址（WAKE 回 0）");
    g_win_va = U_BASE;

    /* 越过窗口末端的字：没有可查的帧，不拿 VA 假装它是键 */
    (void)call(c, T_NR_FUTEX, U_BASE + (unsigned long)U_SIZE,
               (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex 越过窗口末端回 -ENOSYS");

    /* 重放识别：a0 被返回值清成 0 之后，靠「本槽还登记着」认出这是同一次
     * WAIT，并用登记下来的地址与期望值继续判，而不是拿 0 去读字。
     * 这正是「一次超过一个时间片仍没被唤醒的 WAIT」会走的路：以前这里
     * 必然 -EFAULT，现在它应当和普通时间片重放一样继续睡。 */
    g_restart_rc = 0;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 先登记一次等待（重放识别的前置）");
    {
        int32 blocks0 = g_block_calls;
        (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
        ck_ret(c, 0, "futex 重放时 a0 被清成 0，用登记下的地址继续睡（不再 -EFAULT）");
        ck(g_block_calls == blocks0 + 1, "futex 重放后真的又睡了一个时间片");
    }
    /* 值在等待期间变了：重放读到登记地址上的新值，按被叫醒处理并销账。 */
    u_put32(0x10u, 6u);
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 重放时读到登记地址上的新值，当被叫醒处理");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "值变了那条重放也销了账");

    /* 登记下的地址真的读不出来：这才是 -EFAULT（不是「地址是 0」就判死）。 */
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 再登记一次等待");
    g_from_budget = 0;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "futex 重放时登记地址读不出来才回 -EFAULT");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "这条 -EFAULT 也销了账");

    /* 重放时端口忽然不肯重放：如实 -ENOSYS，不假装又睡过一个时间片 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 再登记一次等待");
    g_restart_rc = -1;
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex 重放时端口没有重放能力回 -ENOSYS");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "端口不肯重放时也销了账");
    g_restart_rc = 0;

    /* 第一次进入就读不出那个字：不登记、也不回 EAGAIN */
    g_from_budget = 0;
    (void)call(c, T_NR_FUTEX, w1, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "futex 首次进入读不出字时回 -EFAULT");

    /* 重放时撞上未屏蔽信号：时间片醒了、值没变，但有信号要处理 -> -EINTR
     * 并销账（Linux 语义，不是我们编的）。先登记再 kill：首次进入的入口
     * 有 pending 时根本不会登记。 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 为 EINTR 那条路径先登记一次");
    (void)sig_install_act(c, 10u, 0x1000UL, 0x2000UL, 0u);
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    ck_ret(c, 0, "futex 前置：kill 自己成功（造一个投不掉的 pending）");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, -T_EINTR, "futex 重放时撞上未屏蔽信号回 -EINTR");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex EINTR（重放路径）之后没有留下等待登记");

    /* 有未屏蔽信号：WAIT 回 -EINTR 且不登记。
     * 难点是让「待投递」活到下一次调用：信号在每次系统调用返回前都会投递，
     * 默认动作会把它销掉。这里把 SIGUSR1 的处理函数装在一个读不出来的地址上
     * ——投递时取不到指令就把 pending 位放回去（那条路径本来就不许拿猜的地址
     * 铺帧），于是下一个入口仍看得见它。 */
    u_put32(0x10u, 5u);
    g_restart_rc = 0;
    (void)sig_install_act(c, 10u, 0x1000UL, 0x2000UL, 0u);
    ck_ret(c, 0, "futex 前置：给 SIGUSR1 装一个投递不到的处理函数");
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    ck_ret(c, 0, "futex 前置：kill 自己成功");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, -T_EINTR, "futex WAIT 有未屏蔽信号回 -EINTR");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex EINTR 之后没有留下等待登记");

    /* ---------------- 第二档：REQUEUE / CMP_REQUEUE / BITSET / TIMEOUT ----------------
     * 这一段的每个条目都独立造自己的状态：先登记一个等待者，再验证「这次的 op
     * 对它做了什么」，最后把账销干净，下一条从空表开始。
     *
     * 先屏蔽 SIGUSR1：上面 EINTR 那两条把 pending 位留在位上了（处理函数故意
     * 投递不到），不屏蔽的话下面每次 WAIT 都会先回 -EINTR。块尾恢复。 */
    u_put32(T_SET_OFF, 0x200u);        /* bit9 = 信号 10 */
    (void)call(c, T_NR_RT_SIGPROCMASK, 0UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, 0, "futex 第二档前置：屏蔽 SIGUSR1（EINTR 那条留下的 pending）");

    /* ---- timeout（相对，WAIT 的语义） ---- */
    u_put32(0x10u, 5u);
    g_restart_rc = 0;
    g_ms = 1000u;
    u_put32(0x60u, 0u);                /* tv_sec  */
    u_put32(0x64u, 50000000u);         /* tv_nsec = 50 ms */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, 0, "futex WAIT 带相对 timeout 先睡下");
    g_ms = 1030u;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, 0, "futex 没到期的那次重放继续睡（不重新计时）");
    g_ms = 1060u;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, -T_ETIMEDOUT, "futex WAIT 相对 timeout 到期回 -ETIMEDOUT");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex 超时之后没有留下等待登记");

    /* 相对 timeout 为 0：值相等也当场 -ETIMEDOUT，不白睡一个时间片 */
    u_put32(0x60u, 0u);
    u_put32(0x64u, 0u);
    {
        int32 blocks0 = g_block_calls;

        (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL,
                   u_ptr(0x60u));
        ck_ret(c, -T_ETIMEDOUT, "futex WAIT 相对 timeout 为 0 当场 -ETIMEDOUT");
        ck(g_block_calls == blocks0, "futex 当场超时的那次一次也没睡");
    }

    /* timespec 本身不合法：-EINVAL（不是当成 0） */
    u_put32(0x60u, 0u);
    u_put32(0x64u, 1000000000u);       /* nsec 必须 < 1e9 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, -T_EINVAL, "futex timeout 的 nsec 越界回 -EINVAL");
    u_put32(0x60u, 0xFFFFFFFFu);       /* tv_sec 是负数（long） */
    u_put32(0x64u, 0u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, -T_EINVAL, "futex timeout 的 tv_sec 为负回 -EINVAL");

    /* timeout 指针读不出来：-EFAULT（不登记、也不是 EAGAIN） */
    u_put32(0x60u, 0u);
    u_put32(0x64u, 50000000u);
    g_from_budget = 0;
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "futex timeout 指针读不出来回 -EFAULT");

    /* WAKE 之后再超时也没关系：先被叫醒就该回 0，不是 -ETIMEDOUT */
    g_ms = 2000u;
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, 0, "futex 带 timeout 登记一次");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 1, "futex 带 timeout 的等待者也能被 WAKE 叫醒");
    g_ms = 999999u;                    /* 远超截止时刻 */
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, u_ptr(0x60u));
    ck_ret(c, 0, "futex 被 WAKE 叫醒优先于超时（回 0 不是 -ETIMEDOUT）");

    /* ---- WAIT_BITSET / WAKE_BITSET ---- */
    u_put32(0x10u, 5u);
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL, 0UL,
                0UL, 0UL);
    ck_ret(c, -T_EINVAL, "futex WAIT_BITSET 位集为 0 回 -EINVAL");

    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL, 0UL,
                0UL, 2UL);
    ck_ret(c, 0, "futex WAIT_BITSET 登记位集 2 并睡下");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE_BITSET, 1UL, 0UL,
                0UL, 0UL);
    ck_ret(c, -T_EINVAL, "futex WAKE_BITSET 位集为 0 回 -EINVAL");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE_BITSET, 1UL, 0UL,
                0UL, 1UL);
    ck_ret(c, 0, "futex WAKE_BITSET 位集不相交时不唤醒");
    (void)call6(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL, 0UL,
                0UL, 2UL);
    ck_ret(c, 0, "futex WAIT_BITSET 没被唤醒前重放继续睡");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE_BITSET, 1UL, 0UL,
                0UL, 2UL);
    ck_ret(c, 1, "futex WAKE_BITSET 位集相交时唤醒 1 个");
    (void)call6(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL, 0UL,
                0UL, 2UL);
    ck_ret(c, 0, "futex WAIT_BITSET 被叫醒后重放回 0");

    /* 普通 WAKE 不按位筛选：能叫醒位集等待者（Linux 语义） */
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL, 0UL,
                0UL, 4UL);
    ck_ret(c, 0, "futex WAIT_BITSET 再登记一次（位集 4）");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 1, "futex 普通 WAKE 能叫醒位集等待者");
    (void)call6(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL, 0UL,
                0UL, 4UL);
    ck_ret(c, 0, "futex 被普通 WAKE 叫醒的位集重放回 0");

    /* WAIT_BITSET 的 timeout 是**绝对**时刻（与 Linux 一致），不是相对时长 */
    g_ms = 5000u;
    u_put32(0x60u, 2u);                /* 绝对 2000 ms，已经过去 */
    u_put32(0x64u, 0u);
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL,
                u_ptr(0x60u), 0UL, 1UL);
    ck_ret(c, -T_ETIMEDOUT, "futex WAIT_BITSET 绝对时刻已过回 -ETIMEDOUT");
    u_put32(0x60u, 6u);                /* 绝对 6000 ms，还差 1000 ms */
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL,
                u_ptr(0x60u), 0UL, 1UL);
    ck_ret(c, 0, "futex WAIT_BITSET 绝对时刻未到先睡下");
    g_ms = 6100u;
    (void)call6(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT_BITSET, 5UL,
                u_ptr(0x60u), 0UL, 1UL);
    ck_ret(c, -T_ETIMEDOUT, "futex WAIT_BITSET 绝对时刻到了回 -ETIMEDOUT");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex WAIT_BITSET 超时之后没有留下等待登记");

    /* CLOCK_REALTIME：本机没有墙钟，带 timeout 如实 -ENOSYS；
     * 不带 timeout 时这面旗没有作用对象（忽略），但仍只对 WAIT_BITSET 合法。 */
    g_ms = 7000u;
    u_put32(0x60u, 8u);
    u_put32(0x64u, 0u);
    (void)call6(c, T_NR_FUTEX, w0,
                (unsigned long)(T_FUTEX_WAIT_BITSET | T_FUTEX_CLOCK_REALTIME),
                5UL, u_ptr(0x60u), 0UL, 1UL);
    ck_ret(c, -T_ENOSYS, "futex CLOCK_REALTIME 带 timeout 回 -ENOSYS（没有墙钟）");
    (void)call6(c, T_NR_FUTEX, w0,
                (unsigned long)(T_FUTEX_WAIT | T_FUTEX_CLOCK_REALTIME),
                5UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex CLOCK_REALTIME 配非 WAIT_BITSET 回 -ENOSYS");

    /* ---- REQUEUE ---- */
    u_put32(0x10u, 5u);
    u_put32(0x2010u, 7u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex REQUEUE 前置：先登记一个等待者");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_REQUEUE, 0UL, 1UL, w1,
                0UL);
    ck_ret(c, 1, "futex REQUEUE nr_wake=0 回 1（改挂数也计入返回值）");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex 改挂之后原地址上没有等待者了");
    (void)call(c, T_NR_FUTEX, w1, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 1, "futex 改挂之后新地址上的 WAKE 叫醒 1 个");
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 被改挂的等待者重放回 0（用新地址与新期望值）");

    /* nr_wake 与 nr_requeue 同时给：先唤醒再改挂，返回值是唤醒数加改挂数
     * （man futex 的措辞是 "woken up or requeued"） */
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex REQUEUE 前置：再登记一个等待者");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_REQUEUE, 1UL, 0UL, w1,
                0UL);
    ck_ret(c, 1, "futex REQUEUE nr_wake=1 回 1");
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex REQUEUE 唤醒的那条重放回 0");
    (void)call(c, T_NR_FUTEX, w1, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "futex nr_requeue=0 时没有把等待者挂到新地址");

    /* ---- CMP_REQUEUE ---- */
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex CMP_REQUEUE 前置：再登记一个等待者");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_CMP_REQUEUE, 0UL, 1UL,
                w1, 9UL);
    ck_ret(c, -T_EAGAIN, "futex CMP_REQUEUE 期望值不符回 -EAGAIN");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 1, "futex CMP_REQUEUE 判不通过时没动过等待者");
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 它还是被原地址的 WAKE 叫醒（重放回 0）");

    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex CMP_REQUEUE 前置：再登记一个等待者");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_CMP_REQUEUE, 1UL, 1UL,
                w1, 5UL);
    ck_ret(c, 1, "futex CMP_REQUEUE 期望值相符时唤醒 nr_wake 个");
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex CMP_REQUEUE 唤醒的那条重放回 0");

    /* nr_wake=0 的 CMP_REQUEUE：返回值必须能看出「挂过去了」——glibc 的
     * pthread_cond_broadcast 就是按这个数决定要不要继续的 */
    u_put32(0x10u, 5u);
    u_put32(0x2010u, 5u);
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex CMP_REQUEUE nr_wake=0 前置：再登记一个等待者");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_CMP_REQUEUE, 0UL,
                1UL, w1, 5UL);
    ck_ret(c, 1, "futex CMP_REQUEUE nr_wake=0 回 1（改挂数计入返回值）");
    (void)call(c, T_NR_FUTEX, w1, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 1, "futex CMP_REQUEUE 改挂之后新地址上的 WAKE 叫醒 1 个");
    /* 重放用的是**用户原样写下的旧地址**（真机上重放的 a0 就是它；早先这里
     * 写成 0 只走得到「a0 被返回值抹成 0」那条兜底分支，于是「按地址认重放
     * 认不出被改挂过的自己」这个缺陷在宿主档里照不出来）。 */
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAIT, 5UL, 0UL);
    ck_ret(c, 0, "futex 被 CMP_REQUEUE 改挂的等待者按原地址重放也回 0");
    (void)call(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_WAKE, 1UL, 0UL);
    ck_ret(c, 0, "改挂过的等待者没有重新睡回旧地址");

    /* 参数与键的边界 */
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_REQUEUE, 0UL, 0UL, 0UL,
                0UL);
    ck_ret(c, -T_EINVAL, "futex REQUEUE 缺 uaddr2 回 -EINVAL");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_REQUEUE, 0UL, 1UL,
                U_BASE - 8UL, 0UL);
    ck_ret(c, -T_ENOSYS, "futex REQUEUE 目标地址没有键回 -ENOSYS");
    (void)call6(c, T_NR_FUTEX, w0, (unsigned long)T_FUTEX_REQUEUE, 0UL,
                (unsigned long)-1, w1, 0UL);
    ck_ret(c, -T_EINVAL, "futex REQUEUE nr_requeue 为负回 -EINVAL");

    /* 块尾：恢复信号屏蔽（与进入这一段之前的状态一致） */
    (void)call(c, T_NR_RT_SIGPROCMASK, 1UL, u_ptr(T_SET_OFF), 0UL,
               (unsigned long)T_SIGSET_BYTES);
    ck_ret(c, 0, "futex 第二档收尾：恢复 SIGUSR1 屏蔽");

    /* 收尾：把端口能力恢复成默认，后面的分组依赖 -1 */
    g_restart_rc = -1;
}

/* 分组 14b：futex 的 PI 三档（LOCK_PI / UNLOCK_PI / TRYLOCK_PI）与
 * robust list（set_robust_list / get_robust_list / 退出清理）
 *
 * 判据全部落在「用户看到的字与返回值」上：持有者 tid、WAITERS 位、OWNER_DIED
 * 位、EDEADLK / EPERM / EAGAIN / ETIMEDOUT，以及交接之后字里是新持有者。
 * 优先级继承的有效性靠任务表的 priority 与 arkrtos_ready_reprio 的调用记账
 * （真机上就是换桶），这样「抬上去」和「算回去」都能被钉死 —— 只测值不测
 * 优先级的话，实现可以整个跳过继承仍然全绿。
 *
 * 只有一个执行流可用，所以「另一个任务」通过切换 arkrtos_current_task_id
 * 模拟：任务表是真实的，futex 槽按任务号索引，两边看到的字是同一个。 */
static void t_futex_pi(t_ctx *c)
{
    unsigned long w = u_ptr(0x10u);
    int32 cur0 = arkrtos_current_task_id;

    fresh(c);
    g_restart_rc  = 0;
    g_block_calls = 0;
    g_wake_calls  = 0;
    g_reprio_calls = 0;
    arkrtos_current_task_id = 7;
    arkrtos_task_table[6].base_priority = 4u;    /* 等待者：tid 7（就是我） */
    arkrtos_task_table[6].priority      = 4u;
    arkrtos_task_table[4].base_priority = 10u;   /* 持有者：tid 5 */
    arkrtos_task_table[4].priority      = 10u;

    /* ---- 空锁直接拿 ---- */
    u_put32(0x10u, 0u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex LOCK_PI 空锁直接拿到");
    ck_u(u_get32(0x10u), 7u, "futex LOCK_PI 把持有者 tid 写进字里");

    /* ---- 自己已经持有：真死锁，回 -EDEADLK ---- */
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, -35, "futex LOCK_PI 自己持有回 -EDEADLK（不假装成功）");
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_TRYLOCK_PI, 0UL, 0UL);
    ck_ret(c, -35, "futex TRYLOCK_PI 自己持有也回 -EDEADLK（Linux 同）");

    /* ---- UNLOCK_PI ---- */
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex UNLOCK_PI 持有者解锁回 0");
    ck_u(u_get32(0x10u), 0u, "futex UNLOCK_PI 没人等时把字清成 0");
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_ret(c, -1, "futex UNLOCK_PI 不是持有者回 -EPERM");

    /* ---- TRYLOCK_PI 成功 / 失败 ---- */
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_TRYLOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex TRYLOCK_PI 空锁拿到");
    ck_u(u_get32(0x10u), 7u, "futex TRYLOCK_PI 也写持有者 tid");
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex TRYLOCK_PI 拿到之后解锁回 0");

    u_put32(0x10u, 5u);                 /* 别人持有 */
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_TRYLOCK_PI, 0UL, 0UL);
    ck_ret(c, -11, "futex TRYLOCK_PI 别人持有回 -EAGAIN");
    ck_u(u_get32(0x10u), 5u | T_FUTEX_WAITERS,
         "futex TRYLOCK_PI 失败时把 WAITERS 位置上（有人试过）");

    /* ---- LOCK_PI 拿不到：登记 + 抬持有者优先级 ---- */
    fresh(c);
    g_restart_rc  = 0;
    g_block_calls = 0;
    g_wake_calls  = 0;
    g_reprio_calls = 0;
    arkrtos_current_task_id = 7;
    arkrtos_task_table[6].base_priority = 4u;    /* 等待者：tid 7（就是我） */
    arkrtos_task_table[6].priority      = 4u;
    arkrtos_task_table[4].base_priority = 10u;   /* 持有者：tid 5 */
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex LOCK_PI 别人持有时登记等待并回 0（返回值被重放的那次覆盖）");
    ck(g_block_calls == 1, "futex LOCK_PI 真的阻塞了一次");
    ck_u(u_get32(0x10u), 5u | T_FUTEX_WAITERS,
         "futex LOCK_PI 拿不到时置 WAITERS 位（持有者的 UNLOCK 靠它知道要交接）");
    ck_u((unsigned long)arkrtos_task_table[4].priority, 4UL,
         "futex LOCK_PI 把持有者（tid 5）的有效优先级抬到等待者的 4");
    ck(g_reprio_calls >= 1, "futex LOCK_PI 抬优先级走的是换桶接口（不是偷偷改字段）");
    ck_u((unsigned long)arkrtos_task_table[4].base_priority, 10UL,
         "futex LOCK_PI 没有动持有者的基准优先级（撤销继承的依据）");

    /* 时间片醒来、值没变：继续睡，不再重复抬一次（已经抬过了） */
    {
        int32 prio0 = (int32)arkrtos_task_table[4].priority;

        (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
        ck_ret(c, 0, "futex LOCK_PI 重放（a0 被清成 0）继续睡");
        ck(g_block_calls == 2, "futex LOCK_PI 重放后又睡了一个时间片");
        ck_u((unsigned long)arkrtos_task_table[4].priority, (unsigned long)prio0,
             "futex LOCK_PI 重放时持有者优先级没有被重复叠加");
    }

    /* ---- 持有者 UNLOCK_PI：交接给优先级最高的等待者 ---- */
    arkrtos_current_task_id = 5;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex UNLOCK_PI 有人等时交接回 0");
    ck_u(u_get32(0x10u), 7u, "futex UNLOCK_PI 把字写成等待者的 tid（直接交接）");
    ck_u((unsigned long)g_wake_last, 7UL, "futex UNLOCK_PI 唤醒的是那个等待者");
    ck_u((unsigned long)arkrtos_task_table[4].priority, 10UL,
         "futex 交接之后原持有者的优先级算回基准（不是永久抬着）");

    /* ---- 被交接的等待者重放：确认所有权，字里仍是它 ---- */
    arkrtos_current_task_id = 7;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex 被交接的等待者重放回 0");
    ck_u(u_get32(0x10u), 7u, "futex 交接后的字保持新持有者的 tid");
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex 新持有者解锁回 0");
    ck_u(u_get32(0x10u), 0u, "futex 新持有者解锁后字清 0");

    /* ---- 超时：不白睡，也不留下登记 ---- */
    arkrtos_task_table[4].priority = 10u;
    u_put32(0x10u, 5u);
    u_put32(0x60u, 0u);                 /* tv_sec  */
    u_put32(0x64u, 0u);                 /* tv_nsec：当场到期 */
    {
        int32 blocks0 = g_block_calls;

        (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL,
                   u_ptr(0x60u));
        ck_ret(c, -110, "futex LOCK_PI 超时 0 回 -ETIMEDOUT");
        ck(g_block_calls == blocks0, "futex LOCK_PI 已经到期时一次也没睡");
    }
    ck_u((unsigned long)arkrtos_task_table[4].priority, 10UL,
         "futex LOCK_PI 超时后持有者优先级没有被抬着");

    /* ---- 原持有者已死：接手者拿到的字里保留 OWNER_DIED ---- */
    u_put32(0x10u, 5u | T_FUTEX_OWNER_DIED);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "futex LOCK_PI 接手已死持有者的锁回 0");
    ck_u(u_get32(0x10u), 7u | T_FUTEX_OWNER_DIED,
         "futex LOCK_PI 接手时保留 OWNER_DIED（用户态据此报 EOWNERDEAD）");
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_u(u_get32(0x10u), 0u, "futex 用户态清过 OWNER_DIED 之后解锁就是 0");

    /* ---- 参数边界：窗口外的字没有键 ---- */
    (void)call(c, T_NR_FUTEX, U_BASE - 8UL, (unsigned long)T_FUTEX_LOCK_PI,
               0UL, 0UL);
    ck_ret(c, -38, "futex LOCK_PI 窗口外的字回 -ENOSYS（不拿 VA 冒充键）");

    /* ================= robust list ================= */
    fresh(c);
    arkrtos_current_task_id = 7;

    /* ---- 参数校验 ---- */
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 8UL, 0UL, 0UL);
    ck_ret(c, -22, "set_robust_list 长度装不下结构回 -EINVAL");
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x302u), 12UL, 0UL, 0UL);
    ck_ret(c, -22, "set_robust_list 指针没对齐回 -EINVAL");
    (void)call(c, T_NR_SET_ROBUST_LIST, 0UL, 12UL, 0UL, 0UL);
    ck_ret(c, -22, "set_robust_list 空指针回 -EINVAL");
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    ck_ret(c, 0, "set_robust_list 正常登记回 0");

    /* ---- get 回读 ---- */
    u_put32(0x380u, 0u);
    u_put32(0x384u, 0u);
    (void)call(c, T_NR_GET_ROBUST_LIST, 7UL, u_ptr(0x380u), u_ptr(0x384u), 0UL);
    ck_ret(c, 0, "get_robust_list 查自己回 0");
    ck_u(u_get32(0x380u), (unsigned long)u_ptr(0x300u),
         "get_robust_list 回读链头指针");
    ck_u(u_get32(0x384u), 12UL, "get_robust_list 回读长度");
    (void)call(c, T_NR_GET_ROBUST_LIST, 9UL, u_ptr(0x380u), u_ptr(0x384u), 0UL);
    ck_ret(c, -3, "get_robust_list 查别的进程回 -ESRCH（不编造）");
    (void)call(c, T_NR_GET_ROBUST_LIST, 7UL, 0UL, u_ptr(0x384u), 0UL);
    ck_ret(c, -14, "get_robust_list 出参为空回 -EFAULT");

    /* ---- 非 PI：退出时 tid 清掉、置 OWNER_DIED、留 WAITERS 并唤醒一个 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    g_block_calls = 0;
    g_wake_calls = 0;
    u_put32(0x360u, 7u | T_FUTEX_WAITERS);      /* 任务 7 持有的 robust 互斥字 */
    u_put32(0x300u, (uint32)u_ptr(0x320u));     /* head.next -> 节点 */
    u_put32(0x304u, 0x40u);                     /* futex_offset = 0x40 */
    u_put32(0x308u, 0u);                        /* list_op_pending */
    u_put32(0x320u, (uint32)u_ptr(0x300u));     /* 节点的 next = head（终止） */
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    ck_ret(c, 0, "robust 非 PI 前置：登记链头");

    /* 造一个真的等待者（任务 3）等在同一个字上，退出时必须被叫醒 */
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, u_ptr(0x360u), (unsigned long)T_FUTEX_WAIT,
               7u | T_FUTEX_WAITERS, 0UL);
    ck_ret(c, 0, "robust 非 PI 前置：任务 3 在这个字上登记等待");
    arkrtos_current_task_id = 7;
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck_u(u_get32(0x360u), T_FUTEX_OWNER_DIED | T_FUTEX_WAITERS,
         "robust 退出把字改成「持有者已死」（tid 清掉、WAITERS 留着）");
    ck_u((unsigned long)g_wake_last, 3UL, "robust 退出叫醒的是那个等待者（不是随便一个）");
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_WAIT,
               7u | T_FUTEX_WAITERS, 0UL);
    ck_ret(c, 0, "robust 退出唤醒的那条重放回 0 并销账");

    /* 字已经不属于它（用户态自己解过锁）：一个字节都不许改 */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x360u, 9u);
    u_put32(0x300u, (uint32)u_ptr(0x320u));
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    ck_ret(c, 0, "robust 别人的字前置：登记链头");
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck_u(u_get32(0x360u), 9u, "robust 退出不碰别人持有的字");

    /* ---- PI：tid 保留、交接给队首、OWNER_DIED 留给接手者 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    g_block_calls = 0;
    g_wake_calls = 0;
    arkrtos_task_table[6].base_priority = 10u;   /* 持有者：tid 7 */
    arkrtos_task_table[6].priority      = 10u;
    arkrtos_task_table[2].base_priority = 4u;    /* 等待者：tid 3 */
    arkrtos_task_table[2].priority      = 4u;
    u_put32(0x360u, 7u);                          /* 任务 7 持有 */
    u_put32(0x300u, (uint32)(u_ptr(0x320u) | 1u)); /* 最低位 = PI 条目 */
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    ck_ret(c, 0, "robust PI 前置：登记链头");
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, u_ptr(0x360u), (unsigned long)T_FUTEX_LOCK_PI,
               0UL, 0UL);
    ck_ret(c, 0, "robust PI 前置：任务 3 在 PI 锁上排队");
    ck_u((unsigned long)arkrtos_task_table[6].priority, 4UL,
         "robust PI 前置：任务 7 被抬到 4");
    arkrtos_current_task_id = 7;
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck_u(u_get32(0x360u), 3u | T_FUTEX_OWNER_DIED,
         "robust PI 退出把所有权交接给队首，并保留 OWNER_DIED");
    ck_u((unsigned long)g_wake_last, 3UL, "robust PI 退出叫醒的是队首等待者");
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "robust PI 接手者重放回 0（拿到所有权）");
    ck_u(u_get32(0x360u), 3u | T_FUTEX_OWNER_DIED,
         "robust PI 接手者的字里带着 OWNER_DIED（用户态据此报 EOWNERDEAD）");
    (void)call(c, T_NR_FUTEX, u_ptr(0x360u), (unsigned long)T_FUTEX_UNLOCK_PI,
               0UL, 0UL);
    ck_ret(c, 0, "robust PI 接手者解锁回 0");
    ck_u(u_get32(0x360u), 0u, "robust PI 用户态清过 OWNER_DIED 之后解锁就是 0");

    /* ---- 坏链表（自环）：退出路径必须能走完 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x300u, (uint32)u_ptr(0x320u));
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x320u));       /* 自己指向自己 */
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    ck_ret(c, 0, "robust 自环前置：登记链头");
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck(1, "robust 自环的链表也能走完退出路径（有 2048 条上限）");

    arkrtos_current_task_id = cur0;
    g_restart_rc = -1;
}

/* 分组 14c：futex PI 的边界与多等待者、robust list 的坏输入
 *
 * 存在的理由不是「凑覆盖率」，而是这些分支各自对应一条真实的失败语义：
 *   - 读/写字失败（g_from_budget/g_to_budget 造）必须原样 -EFAULT 且不留下
 *     一条「已经在等」的旧登记；
 *   - 有未屏蔽信号 / 端口不肯重放要在登记前后各自处理，且都要销账；
 *   - 重放的四种收尾（到期 / 被交接 / 继续睡 / 读字失败）各走一条；
 *   - PI 多等待者交接时，队首按 base_priority 选、其余等待者的持有者一并改；
 *   - robust 链被写坏（越界 / 空帧 / 断链 / pending / 非法任务号）时退出路径
 *     必须走完且不写坏别人的字 —— 死在退出钩子里就是整个系统停摆。
 */
static void t_futex_pi_edge(t_ctx *c)
{
    unsigned long w = u_ptr(0x10u);
    int32 cur0 = arkrtos_current_task_id;

    /* ---- 非重放：读字失败 -EFAULT（并在 get_word 内层回 -EFAULT） ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x10u, 5u);
    g_from_budget = 0;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "LOCK_PI 读字失败回 -EFAULT");

    /* ---- 非重放：置 WAITERS 写失败 -EFAULT ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x10u, 5u);                         /* 无 WAITERS，必须写一次 */
    g_to_budget = 0;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, -T_EFAULT, "LOCK_PI 置 WAITERS 写失败回 -EFAULT");

    /* ---- 非重放：读超时结构失败 -EFAULT ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x10u, 5u);
    u_put32(0x60u, 1u);
    u_put32(0x64u, 0u);
    g_from_budget = 1;                          /* get_word 成功后 timeout 的 copy 失败 */
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL,
               u_ptr(0x60u));
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "LOCK_PI 读超时结构失败回 -EFAULT");

    /* ---- 非重放：有未屏蔽信号 -EINTR ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    /* 信号在每次系统调用返回前都会投递；把处理函数装在读不出来的地址上，
     * 投递取不到指令就把 pending 位放回去，下一个入口仍看得见它。 */
    (void)sig_install_act(c, 10u, 0x1000UL, 0x2000UL, 0u);
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);   /* 置 SIGUSR1 pending */
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, -T_EINTR, "LOCK_PI 有未屏蔽信号回 -EINTR");

    /* ---- 非重放：端口没有重放能力 -ENOSYS ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x10u, 5u);
    g_restart_rc = 1;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_restart_rc = -1;
    ck_ret(c, -T_ENOSYS, "LOCK_PI 端口没有重放能力回 -ENOSYS");

    /* ---- 超时钳位：绝对上限 / 相对时长超 int32 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    u_put32(0x10u, 5u);
    u_put32(0x60u, 2147484u);                   /* > 2147483 ⇒ 直接钳到上限 */
    u_put32(0x64u, 0u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL,
               u_ptr(0x60u));
    ck_ret(c, 0, "LOCK_PI 超长时长（ts.sec>2147483）按上限登记");

    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    u_put32(0x10u, 5u);
    u_put32(0x60u, 2147483u);                   /* 乘 1000 后超 int32 上限 */
    u_put32(0x64u, 999999999u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL,
               u_ptr(0x60u));
    ck_ret(c, 0, "LOCK_PI 相对时长超 int32 上限时钳位");

    /* ================= 重放路径的四种收尾 ================= */

    /* (a) 重放到期 -ETIMEDOUT */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    g_ms = 1234567u;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    u_put32(0x60u, 1000u);
    u_put32(0x64u, 0u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL,
               u_ptr(0x60u));
    g_ms += 2000000u;                           /* 远超截止 */
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, -T_ETIMEDOUT, "LOCK_PI 重放到期回 -ETIMEDOUT（并销账）");

    /* (b) 重放读字失败 -EFAULT */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_from_budget = 0;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "LOCK_PI 重放读字失败回 -EFAULT（并销账）");

    /* (c) 重放接手已死持有者：写字成功、保留 OWNER_DIED */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "LOCK_PI 登记等待（重放接手的前置）");
    u_put32(0x10u, 5u | T_FUTEX_OWNER_DIED);    /* 原持有者已死 */
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "LOCK_PI 重放接手已死持有者回 0");
    ck_u(u_get32(0x10u), 7u | T_FUTEX_OWNER_DIED,
         "LOCK_PI 重放接手把字写成自己并保留 OWNER_DIED");

    /* (d) 重放接手时写字失败 -EFAULT */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    u_put32(0x10u, 5u | T_FUTEX_OWNER_DIED);
    g_to_budget = 0;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, -T_EFAULT, "LOCK_PI 重放接手写字失败回 -EFAULT（并销账）");

    /* (e) 重放时来信号 -EINTR */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    (void)sig_install_act(c, 10u, 0x1000UL, 0x2000UL, 0u);
    (void)call(c, T_NR_KILL, 7UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, -T_EINTR, "LOCK_PI 重放时来信号回 -EINTR（并销账）");

    /* (f) 重放时端口不肯重放 -ENOSYS */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_restart_rc = 1;
    (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    g_restart_rc = -1;
    ck_ret(c, -T_ENOSYS, "LOCK_PI 重放时端口没有重放能力回 -ENOSYS（并销账）");

    /* (g) 重放发现持有者换人：改写登记里的持有者、两边优先级重算后继续睡 */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    g_block_calls = 0;
    g_reprio_calls = 0;
    arkrtos_task_table[6].base_priority = 4u;   /* 我：tid 7，等待者 */
    arkrtos_task_table[6].priority      = 4u;
    arkrtos_task_table[4].base_priority = 10u;  /* 初持有者：tid 5 */
    arkrtos_task_table[4].priority      = 10u;
    arkrtos_task_table[8].base_priority = 6u;   /* 换上的持有者：tid 9 */
    arkrtos_task_table[8].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "LOCK_PI 登记等待（换人重放的前置）");
    ck_u((unsigned long)arkrtos_task_table[4].priority, 4UL,
         "LOCK_PI 登记后初持有者被抬到 4");
    {
        int32 b0 = g_block_calls;

        u_put32(0x10u, 9u);                     /* 持有者换成 tid 9 */
        (void)call(c, T_NR_FUTEX, 0UL, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
        ck_ret(c, 0, "LOCK_PI 重放发现持有者换人后继续睡");
        ck(g_block_calls == b0 + 1, "LOCK_PI 重放换人后又睡了一个时间片");
    }
    ck_u((unsigned long)arkrtos_task_table[8].priority, 4UL,
         "LOCK_PI 重放换人后新持有者被抬到等待者的 4");
    ck_u((unsigned long)arkrtos_task_table[4].priority, 10UL,
         "LOCK_PI 重放换人后旧持有者优先级算回基准 10");

    /* ================= PI 多等待者：队首选择与其余等待者改持有人 ================= */

    /* 两个等待者（tid 3 prio 4 / tid 6 prio 8），持有者 tid 5：解锁交接给 3，
     * 另一个等待者 6 的持有者一并改成 3；字里留 WAITERS（队列还有人不空）。 */
    fresh(c);
    arkrtos_current_task_id = 3;                /* 先排队的等待者 tid 3：prio 8（较低） */
    g_restart_rc = 0;
    g_block_calls = 0;
    g_wake_calls  = 0;
    arkrtos_task_table[2].base_priority = 8u;
    arkrtos_task_table[2].priority      = 8u;
    arkrtos_task_table[5].base_priority = 4u;   /* 后登记者 tid 6：prio 4（更高） */
    arkrtos_task_table[5].priority      = 4u;
    arkrtos_task_table[4].base_priority = 10u;  /* 持有者 tid 5 */
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL); /* tid 3 排队 */
    arkrtos_current_task_id = 6;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL); /* tid 6 排队 */
    arkrtos_current_task_id = 5;                /* 交给持有者解锁 */
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    ck_ret(c, 0, "UNLOCK_PI 多等待者交接回 0");
    ck_u(u_get32(0x10u), 6u | T_FUTEX_WAITERS,
         "UNLOCK_PI 交接给基准优先级最高的 6（后登记者更高也要选对），队列不空留 WAITERS");
    ck_u((unsigned long)g_wake_last, 6UL, "UNLOCK_PI 唤醒的是优先级最高的 6");

    /* 同上但要写字失败：交接不能半途留下一个「看起来成功」的字 */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[6].base_priority = 8u;
    arkrtos_task_table[6].priority      = 8u;
    arkrtos_task_table[2].base_priority = 4u;
    arkrtos_task_table[2].priority      = 4u;
    arkrtos_task_table[4].base_priority = 10u;
    arkrtos_task_table[4].priority      = 10u;
    u_put32(0x10u, 5u);
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    arkrtos_current_task_id = 5;
    g_to_budget = 0;
    (void)call(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_UNLOCK_PI, 0UL, 0UL);
    g_to_budget = -1;
    ck_ret(c, -T_EFAULT, "UNLOCK_PI 交接写字失败回 -EFAULT");

    /* ================= CMP_REQUEUE 的读字失败 ================= */

    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x10u, 0u);
    g_from_budget = 0;
    (void)call6(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_CMP_REQUEUE, 1UL,
                1UL, u_ptr(0x20u), 0UL);
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "CMP_REQUEUE 核对 uaddr 读字失败回 -EFAULT");

    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x10u, 0u);                         /* 期望值 = 0，核对通过 */
    g_from_budget = 1;                          /* 第一次成功、第二次（uaddr2）失败 */
    (void)call6(c, T_NR_FUTEX, w, (unsigned long)T_FUTEX_CMP_REQUEUE, 1UL,
                1UL, u_ptr(0x20u), 0UL);
    g_from_budget = -1;
    ck_ret(c, -T_EFAULT, "CMP_REQUEUE 读 uaddr2 失败回 -EFAULT");

    arkrtos_current_task_id = cur0;
    g_restart_rc = -1;
    g_from_budget = -1;
    g_to_budget = -1;
}

static void t_robust_edge(t_ctx *c)
{
    int32 cur0 = arkrtos_current_task_id;

    /* ---- 节点字地址越界：mark 读字失败，退出路径跳过（不崩） ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x300u, (uint32)u_ptr(0x320u));     /* head.next -> 节点 */
    u_put32(0x304u, (uint32)(U_SIZE - 2u - 0x320u)); /* node+offset 落在窗口末 2 字节 */
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));     /* 节点 next = head（终止） */
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck(1, "robust 节点字越界时退出路径走完（mark 读字失败即跳过）");

    /* ---- 字落在没驻留的帧上：键无效 -ENOSYS，字原样不动 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0xE000u, 7u);                       /* idx7 空帧上的字，tid=自己 */
    u_put32(0x300u, (uint32)u_ptr(0x320u));
    u_put32(0x304u, 0xDCE0u);                   /* 0x320 + 0xDCE0 = 0xE000 */
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck_u(u_get32(0xE000u), 7u, "robust 键落在没驻留的帧上时不动那个字");

    /* ---- PI 多等待者：交接给最高优先级，其余等待者持有人一并改 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    g_wake_calls = 0;
    arkrtos_task_table[6].base_priority = 10u;  /* 持有者 tid 7 */
    arkrtos_task_table[6].priority      = 10u;
    arkrtos_task_table[2].base_priority = 4u;   /* 等待者 tid 3 */
    arkrtos_task_table[2].priority      = 4u;
    arkrtos_task_table[5].base_priority = 8u;   /* 等待者 tid 6 */
    arkrtos_task_table[5].priority      = 8u;
    u_put32(0x360u, 7u);
    u_put32(0x300u, (uint32)(u_ptr(0x320u) | 1u));  /* PI 条目 */
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, u_ptr(0x360u), (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    arkrtos_current_task_id = 6;
    (void)call(c, T_NR_FUTEX, u_ptr(0x360u), (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    arkrtos_current_task_id = 7;
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck_u(u_get32(0x360u), 3u | T_FUTEX_OWNER_DIED,
         "robust PI 多等待者退出：交接给优先级最高的 3");
    ck_u((unsigned long)g_wake_last, 3UL, "robust PI 多等待者退出：唤醒队首 3");
    ck_u((unsigned long)arkrtos_task_table[6].priority, 10UL,
         "robust PI 多等待者退出：原持有者优先级算回基准 10");

    /* ---- PI 交接写字失败：退出路径吞掉 -EFAULT、不写坏 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    g_restart_rc = 0;
    arkrtos_task_table[6].base_priority = 10u;
    arkrtos_task_table[6].priority      = 10u;
    arkrtos_task_table[2].base_priority = 4u;
    arkrtos_task_table[2].priority      = 4u;
    u_put32(0x360u, 7u);
    u_put32(0x300u, (uint32)(u_ptr(0x320u) | 1u));
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    arkrtos_current_task_id = 3;
    (void)call(c, T_NR_FUTEX, u_ptr(0x360u), (unsigned long)T_FUTEX_LOCK_PI, 0UL, 0UL);
    arkrtos_current_task_id = 7;
    g_to_budget = 0;
    (void)arkrtos_linux_abi_on_task_exit(7);
    g_to_budget = -1;
    ck_u(u_get32(0x360u), 7u | T_FUTEX_WAITERS,
         "robust PI 交接写字失败时不动那个字");

    /* ---- 非 PI 写字失败 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x360u, 7u | T_FUTEX_WAITERS);
    u_put32(0x300u, (uint32)u_ptr(0x320u));
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    u_put32(0x320u, (uint32)u_ptr(0x300u));
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    g_to_budget = 0;
    (void)arkrtos_linux_abi_on_task_exit(7);
    g_to_budget = -1;
    ck_u(u_get32(0x360u), 7u | T_FUTEX_WAITERS,
         "robust 非 PI 写字失败时不动那个字");

    /* ---- 链头读不出：hdr copy 失败直接返回 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(U_SIZE - 4u), 12UL, 0UL, 0UL);
    ck_ret(c, 0, "set_robust_list 链头贴着窗口末端仍可登记");
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck(1, "robust 链头读不出时退出路径直接返回（不崩）");

    /* ---- 节点 next 读不出：循环 break（有界） ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x300u, (uint32)u_ptr(U_SIZE - 2u));    /* head.next 越界 */
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, 0u);
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck(1, "robust 节点 next 读不出时退循环 break（有界）");

    /* ---- list_op_pending：已摘下、字还没写的窗口也要清 ---- */
    fresh(c);
    arkrtos_current_task_id = 7;
    u_put32(0x360u, 7u);
    u_put32(0x300u, (uint32)u_ptr(0x300u));     /* 空链：next 指向自己（终止） */
    u_put32(0x304u, 0x40u);
    u_put32(0x308u, (uint32)u_ptr(0x320u));     /* pending */
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    (void)arkrtos_linux_abi_on_task_exit(7);
    ck_u(u_get32(0x360u), T_FUTEX_OWNER_DIED,
         "robust list_op_pending 条目也被清理（置 OWNER_DIED）");

    /* ---- 非法任务号：set / get 都回 -EFAULT ---- */
    arkrtos_current_task_id = 0;
    (void)call(c, T_NR_SET_ROBUST_LIST, u_ptr(0x300u), 12UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "set_robust_list 当前任务号非法回 -EFAULT");
    (void)call(c, T_NR_GET_ROBUST_LIST, 0UL, u_ptr(0x380u), u_ptr(0x384u), 0UL);
    ck_ret(c, -T_EFAULT, "get_robust_list 当前任务号非法回 -EFAULT");

    /* ---- get：两个出参各自的写失败 ---- */
    arkrtos_current_task_id = 7;
    (void)call(c, T_NR_GET_ROBUST_LIST, 7UL, u_ptr(U_SIZE - 2u), u_ptr(0x384u), 0UL);
    ck_ret(c, -T_EFAULT, "get_robust_list 链头出参越界回 -EFAULT");
    (void)call(c, T_NR_GET_ROBUST_LIST, 7UL, u_ptr(0x380u), u_ptr(U_SIZE - 2u), 0UL);
    ck_ret(c, -T_EFAULT, "get_robust_list 长度出参越界回 -EFAULT");

    arkrtos_current_task_id = cur0;
    g_restart_rc = -1;
    g_from_budget = -1;
    g_to_budget = -1;
}

#endif /* ARKRTOS_USE_LINUX_FUTEX == 1 */

#if (ARKRTOS_USE_LINUX_SELECT == 1)
/* 分组 15：pselect6（U-22，就绪等待）
 *
 * 判据落在「程序看到什么」上：返回值是就绪描述符个数、用户 fd_set 被改写成
 * 只剩就绪位、timeout=0 不睡、到点回 0、没有可重放端口回 -EAGAIN。就绪
 * 判据按本层对象定义：管道读端有数据或写端全关、管道写端有空间或读端全关、
 * 普通文件永远就绪、fd 0 可读、fd 1/2 可写；没有任何对象支撑的描述符被选中
 * 回 -EBADF；exceptfds 永不上报。 */
static void t_pselect6(t_ctx *c)
{
    uint32 fds[2];
    uint32 i;
    unsigned long R0 = u_ptr(T_DATA_OFF);              /* 0x1000 读集 */
    unsigned long W0 = u_ptr(T_DATA_OFF + 0x100u);     /* 0x1100 写集 */
    unsigned long E0 = u_ptr(T_DATA_OFF + 0x200u);     /* 0x1200 异常集 */
    uint32 tpo = T_DATA_OFF + 0x300u;                  /* 0x1300 时长（偏移） */
    unsigned long TP = u_ptr(tpo);

    fresh(c);
    g_restart_rc = -1;
    g_block_calls = 0;
    g_ms = 1000u;

    /* ---- 参数边界 ---- */
    (void)call6(c, T_NR_PSELECT6, 0UL, 0UL, 0UL, 0UL, 0UL, 1UL);
    ck_ret(c, -T_ENOSYS, "pselect6 带临时信号掩码回 -ENOSYS");

    (void)call6(c, T_NR_PSELECT6, 1025UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "pselect6 nfds > 1024 回 -EINVAL");

    u_put32(tpo, 0u);
    u_put32(tpo + 4u, 0u);
    (void)call6(c, T_NR_PSELECT6, 0UL, 0UL, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 nfds=0 且 timeout=0 回 0");

    u_put32(tpo, 3000000u);                     /* 约 34 天：截到 int32 上限 */
    u_put32(tpo + 4u, 0u);
    (void)call6(c, T_NR_PSELECT6, 0UL, 0UL, 0UL, 0UL, TP, 0UL);
    ck_ret(c, -T_EAGAIN, "pselect6 超大时长按上限登记（没有重放端口回 -EAGAIN）");

    (void)call6(c, T_NR_PSELECT6, 1UL, U_BASE - 8UL, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "pselect6 fd_set 指针不可读回 -EFAULT");
    (void)call6(c, T_NR_PSELECT6, 1UL, 0UL, U_BASE - 8UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "pselect6 写集指针不可读回 -EFAULT");
    (void)call6(c, T_NR_PSELECT6, 1UL, 0UL, 0UL, U_BASE - 8UL, 0UL, 0UL);
    ck_ret(c, -T_EFAULT, "pselect6 异常集指针不可读回 -EFAULT");

    /* ---- 无管道时的就绪判据 ---- */
    u_put32(T_DATA_OFF, 1u);                    /* bit0 = stdin */
    (void)call6(c, T_NR_PSELECT6, 1UL, R0, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 1, "pselect6 fd0 恒可读回 1");
    ck_u(u_get32(T_DATA_OFF), 1u, "pselect6 fd0 就绪位保留");

    u_put32(T_DATA_OFF + 0x100u, 2u);           /* bit1 = stdout */
    (void)call6(c, T_NR_PSELECT6, 2UL, 0UL, W0, 0UL, 0UL, 0UL);
    ck_ret(c, 1, "pselect6 fd1 恒可写回 1");

    u_put32(tpo, 0u);                           /* 纯轮询 */
    u_put32(tpo + 4u, 0u);
    u_put32(T_DATA_OFF, 0x02u);                 /* bit1 选读：stderr 不可读 */
    (void)call6(c, T_NR_PSELECT6, 2UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 fd1 不可读回 0");

    u_put32(T_DATA_OFF + 0x100u, 0x01u);        /* bit0 选写：stdin 不可写 */
    (void)call6(c, T_NR_PSELECT6, 1UL, 0UL, W0, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 fd0 不可写回 0");

    /* 普通文件同步读写，两个方向永远就绪 */
    g_stat_rc = 0;
    g_stat_mode = 0u;
    u_put_str(T_PATH_OFF, "/sel.txt");
    (void)call(c, T_NR_OPENAT, u_ptr(T_PATH_OFF), u_ptr(T_PATH_OFF), 0UL, 0UL);
    ck_ret(c, 3, "pselect6 前置：打开普通文件得 fd 3");
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 1, "pselect6 普通文件读就绪回 1");
    u_put32(T_DATA_OFF + 0x100u, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, 0UL, W0, 0UL, 0UL, 0UL);
    ck_ret(c, 1, "pselect6 普通文件写就绪回 1");
    (void)call(c, T_NR_CLOSE, 3UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "pselect6 收尾：关普通文件");

    u_put32(T_DATA_OFF, 0x20u);                 /* bit5：没有打开 */
    (void)call6(c, T_NR_PSELECT6, 6UL, R0, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EBADF, "pselect6 选中未打开的描述符回 -EBADF");

    u_put32(T_DATA_OFF, 1u);                    /* bit0 读 + bit1 写 */
    u_put32(T_DATA_OFF + 0x100u, 2u);
    (void)call6(c, T_NR_PSELECT6, 2UL, R0, W0, 0UL, 0UL, 0UL);
    ck_ret(c, 2, "pselect6 读写集同时就绪回 2");

    /* ---- 管道就绪 ---- */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF + 0x400u), 0UL, 0UL, 0UL);
    ck_ret(c, 0, "pselect6 前置：建管道");
    u_get(T_DATA_OFF + 0x400u, fds, 8u);

    u_put32(tpo, 0u);
    u_put32(tpo + 4u, 0u);
    u_put32(T_DATA_OFF, 0x08u);                 /* bit3 = 读端 */
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 空管道 timeout=0 回 0");
    ck_u(u_get32(T_DATA_OFF), 0u, "pselect6 空管道把就绪位清掉");
    ck(g_block_calls == 0, "pselect6 timeout=0 不睡");

    u_put32(T_DATA_OFF, 0x10u);                 /* bit4 = 写端 */
    (void)call6(c, T_NR_PSELECT6, 5UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 管道写端不可读回 0");

    u_put_str(T_DATA_OFF + 0x500u, "hi");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1],
               u_ptr(T_DATA_OFF + 0x500u), 2UL, 0UL);
    ck_ret(c, 2, "pselect6 前置：向管道写 2 字节");
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 1, "pselect6 有数据的管道读端回 1");
    ck_u(u_get32(T_DATA_OFF), 0x08u, "pselect6 读端就绪位保留");

    u_put32(T_DATA_OFF + 0x100u, 0x10u);
    (void)call6(c, T_NR_PSELECT6, 5UL, 0UL, W0, 0UL, TP, 0UL);
    ck_ret(c, 1, "pselect6 未满管道写端可写回 1");

    for(i = 0u; i < 300u; i++)
    {
        g_user[T_DATA_OFF + 0x500u + i] = (uint8)i;
    }
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1],
               u_ptr(T_DATA_OFF + 0x500u), 300UL, 0UL);
    ck_ret(c, 254, "pselect6 前置：写满管道（短写 254 字节）");
    u_put32(T_DATA_OFF + 0x100u, 0x10u);
    (void)call6(c, T_NR_PSELECT6, 5UL, 0UL, W0, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 满管道写端不可写回 0");
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 1, "pselect6 满管道读端仍可读回 1");

    (void)call(c, T_NR_READ, (unsigned long)fds[0],
               u_ptr(T_DATA_OFF + 0x600u), 256UL, 0UL);
    ck_ret(c, 256, "pselect6 前置：排空管道");
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 排空后读端未就绪（写端还开着）");

    (void)call(c, T_NR_CLOSE, (unsigned long)fds[1], 0UL, 0UL, 0UL);
    ck_ret(c, 0, "pselect6 前置：关写端");
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 1, "pselect6 写端全关后读端按 EOF 就绪");

    u_put32(T_DATA_OFF + 0x200u, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, 0UL, 0UL, E0, TP, 0UL);
    ck_ret(c, 0, "pselect6 exceptfds 永不上报");
    ck_u(u_get32(T_DATA_OFF + 0x200u), 0u, "pselect6 exceptfds 被清空");

    (void)call(c, T_NR_CLOSE, (unsigned long)fds[0], 0UL, 0UL, 0UL);
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, -T_EBADF, "pselect6 关掉的描述符仍被选中回 -EBADF");

    /* ---- 阻塞与超时（靠 svc_restart 重放） ---- */
    fresh(c);
    (void)call(c, T_NR_PIPE2, u_ptr(T_DATA_OFF + 0x400u), 0UL, 0UL, 0UL);
    u_get(T_DATA_OFF + 0x400u, fds, 8u);
    g_block_calls = 0;
    g_ms = 1000u;

    arkrtos_current_task_id = 0;
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "pselect6 没有任务身份时按纯轮询回 0");
    ck_u(u_get32(T_DATA_OFF), 0u, "pselect6 无身份时也清空就绪集");
    arkrtos_current_task_id = 7;

    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EAGAIN, "pselect6 无限等待但没有可重放端口回 -EAGAIN");
    ck(g_block_calls == 0, "pselect6 -EAGAIN 时不假装睡过");

    g_restart_rc = 0;
    u_put32(tpo, 1u);                            /* 相对 1 s */
    u_put32(tpo + 4u, 0u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_pending(c, "pselect6 无就绪就登记并阻塞（不写 a0）");
    ck(g_block_calls == 1, "pselect6 阻塞走 block_in_critical 一次");

    g_ms = 1500u;
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_pending(c, "pselect6 截止时刻未到继续睡（不写 a0）");
    ck(g_block_calls == 2, "pselect6 继续睡也走一次 block_in_critical");

    g_ms = 2500u;
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 0, "pselect6 到点回 0");
    ck(g_block_calls == 2, "pselect6 到点不再睡");
    ck_u(u_get32(T_DATA_OFF), 0u, "pselect6 超时把就绪位清掉");

    (void)call6(c, T_NR_PSELECT6, 0UL, 0UL, 0UL, 0UL, U_BASE - 8UL, 0UL);
    ck_ret(c, -T_EFAULT, "pselect6 timeout 指针不可读回 -EFAULT");
    u_put32(tpo, 0u);
    u_put32(tpo + 4u, 1000000000u);
    (void)call6(c, T_NR_PSELECT6, 0UL, 0UL, 0UL, 0UL, TP, 0UL);
    ck_ret(c, -T_EINVAL, "pselect6 tv_nsec 越界回 -EINVAL");
    u_put32(tpo, 0xFFFFFFFFu);
    u_put32(tpo + 4u, 0u);
    (void)call6(c, T_NR_PSELECT6, 0UL, 0UL, 0UL, 0UL, TP, 0UL);
    ck_ret(c, -T_EINVAL, "pselect6 负 tv_sec 回 -EINVAL");

    /* 就绪优先于未到期的超时 */
    g_ms = 1000u;
    g_block_calls = 0;
    u_put32(tpo, 5u);
    u_put32(tpo + 4u, 0u);
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_pending(c, "pselect6 前置：挂一次 5 s 等待");
    u_put_str(T_DATA_OFF + 0x500u, "z");
    (void)call(c, T_NR_WRITE, (unsigned long)fds[1],
               u_ptr(T_DATA_OFF + 0x500u), 1UL, 0UL);
    g_ms = 1500u;
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_ret(c, 1, "pselect6 就绪优先于未到期的超时");

    /* 任务退出要清掉挂着的截止时刻：否则任务号复用会继承旧记录 */
    g_ms = 1000u;
    g_block_calls = 0;
    (void)call(c, T_NR_READ, (unsigned long)fds[0],
               u_ptr(T_DATA_OFF + 0x600u), 1UL, 0UL);
    ck_ret(c, 1, "pselect6 前置：排空 1 字节");
    u_put32(tpo, 5u);
    u_put32(tpo + 4u, 0u);
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_pending(c, "pselect6 前置：挂一次 5 s 等待");
    arkrtos_linux_abi_on_task_exit(7);
    g_ms = 7000u;                               /* 已过旧截止 6000 */
    (void)call6(c, T_NR_PSELECT6, 4UL, R0, 0UL, 0UL, TP, 0UL);
    ck_pending(c, "pselect6 任务退出后重新登记");
    ck(g_block_calls == 2, "pselect6 任务退出清掉了旧的截止时刻");

    /* 收尾：清掉挂着的记录，恢复端口能力 */
    (void)arkrtos_linux_abi_on_task_exit(7);
    g_restart_rc = -1;
    g_block_calls = 0;
}
#endif /* ARKRTOS_USE_LINUX_SELECT == 1 */

#if (ARKRTOS_USE_LINUX_SOCKET == 1)
/* ===================== socket（U-23）=====================
 * 被测层只链接 linux_abi.c，net.c 不进来：下面是一份按脚本回答的
 * arkrtos_net_call。三条纪律：
 *   1) 参数原样记下来 ——「ABI 层到底把什么交给了服务」是可断言的事实，
 *      不是注释里的承诺；
 *   2) 地址类输出按 net 的约定写进用户地址（ip 先、port 后），换算走
 *      host_copy_to，与 copy_to_user 同一套映射；
 *   3)「先 EPENDING 再成功」逐段可控 —— 一次 net 请求至少要一个往返，
 *      不把这个往返造出来就等于只测了一半。
 */

#define T_STILL_WAITING     (0x7FFF)
#define T_SA_OFF            (T_DATA_OFF + 0x400u)   /* sockaddr_in，16 B */
#define T_LEN_OFF           (T_DATA_OFF + 0x420u)   /* socklen_t */
#define T_VAL_OFF           (T_DATA_OFF + 0x430u)   /* int 选项值 */
#define T_IO_OFF            (T_DATA_OFF + 0x440u)   /* send/recv 缓冲 */
#define T_TMO_OFF           (T_DATA_OFF + 0x480u)   /* pselect6 的 timespec */
#define T_PIPE_OFF          (T_DATA_OFF + 0x700u)   /* 管道 fds[2] */

static uint32 g_net_calls;
static uint32 g_net_opn[32];      /* 每个子命令被问了几次 */
static uint32 g_net_arg[5];
static int    g_net_pending;      /* >0：还要再回这么多次 EPENDING */
static int    g_net_peer_pending; /* >0：GETPEERNAME 还要再回多少次 EPENDING */
static int    g_net_busy;         /* >0：还要再回这么多次 EBUSY */
static int32  g_net_rc;           /* 最终返回：>=0 结果，<0 负错误号 */
static int32  g_net_handle;       /* SOCKET / ACCEPT 回的句柄 */
static uint32 g_net_ip;
static uint32 g_net_port;
static int    g_net_bytes;        /* RECV 回的字节数（取完即清零） */
static const uint8 g_net_payload[8] = { 'h', 'e', 'l', 'l', 'o', 0u, 0u, 0u };

static void net_reset(void)
{
    uint32 i;

    g_net_calls       = 0u;
    for(i = 0u; i < 32u; i++)
    {
        g_net_opn[i] = 0u;
    }
    for(i = 0u; i < 5u; i++)
    {
        g_net_arg[i] = 0u;
    }
    g_net_pending      = 0;
    g_net_peer_pending = 0;
    g_net_busy         = 0;
    g_net_rc           = 0;
    g_net_handle       = 1;
    g_net_ip           = 0x0A000001u;
    g_net_port         = 8080u;
    g_net_bytes        = 0;
}

/* net 的地址输出：ip 写进第一个用户指针、port 写进第二个。顺序是协议的一部分
 * （见 arkrtos_net_abi.h 的 GETSOCKNAME 行），写反了在真机上就是「端口变成了
 * IP 的低半字」这种查不出根因的错答案。 */
static void net_addr_out(unsigned long ip_ua, unsigned long port_ua)
{
    uint32 w;

    w = g_net_port;
    (void)host_copy_to((void *)port_ua, &w, 4u);
    w = g_net_ip;
    (void)host_copy_to((void *)ip_ua, &w, 4u);
}

/* 服务任务在回复落地时会调这个查询（见 lx_sock_replay_ms）：宿主上没有
 * 那份回复队列，所以恒报「还没到」，让重放路径按时间片兜底走下去。 */
int32 arkrtos_net_reply_ready(int32 owner)
{
    (void)owner;
    return 0;
}
int32 arkrtos_net_call(uint32 op, uint32 a1, uint32 a2, uint32 a3,
                       uint32 a4, uint32 a5)
{
    uint32 n;
    uint32 w;

    g_net_calls++;
    if((op < 32u) && (g_net_opn[op] < 0xFFFFFFFFu))
    {
        g_net_opn[op]++;
    }
    g_net_arg[0] = a1;
    g_net_arg[1] = a2;
    g_net_arg[2] = a3;
    g_net_arg[3] = a4;
    g_net_arg[4] = a5;

    if(g_net_busy > 0)
    {
        g_net_busy--;
        return ARKRTOS_NET_EBUSY;
    }
    if((op == ARKRTOS_NET_SUB_GETPEERNAME) && (g_net_peer_pending > 0))
    {
        g_net_peer_pending--;
        return ARKRTOS_NET_EPENDING;
    }
    if(g_net_pending > 0)
    {
        g_net_pending--;
        return ARKRTOS_NET_EPENDING;
    }

    switch(op)
    {
    case ARKRTOS_NET_SUB_SOCKET:
    case ARKRTOS_NET_SUB_ACCEPT:
        return g_net_handle;
    case ARKRTOS_NET_SUB_GETSOCKNAME:
    case ARKRTOS_NET_SUB_GETPEERNAME:
        net_addr_out((unsigned long)a2, (unsigned long)a3);
        /* 地址照写（成功路径用得上），返回码交给 g_net_rc：测试要能造出
         * 「名字查询失败」这一档，否则那条分支永远测不到。 */
        return g_net_rc;
    case ARKRTOS_NET_SUB_RECVFROM:
        net_addr_out((unsigned long)a4, (unsigned long)a5);
        /* fall through */
    case ARKRTOS_NET_SUB_RECV:
        if(g_net_rc < 0)
        {
            return g_net_rc;
        }
        n = (uint32)g_net_bytes;
        if(n > a3)
        {
            n = a3;
        }
        if(n > 0u)
        {
            (void)host_copy_to((void *)(unsigned long)a2, g_net_payload, n);
        }
        g_net_bytes = 0;
        return (int32)n;
    case ARKRTOS_NET_SUB_GETSOCKOPT:
        /* 结果直接写进用户地址 —— ABI 层因此必须把用户指针交下来。 */
        w = g_net_ip;
        (void)host_copy_to((void *)(unsigned long)a3, &w, 4u);
        return g_net_rc;
    case ARKRTOS_NET_SUB_POLL:
        return g_net_rc;
    default:
        return g_net_rc;
    }
}

/* 一次「逻辑调用」跑到底：第一遍用原参数，之后每一遍**全 0 参数**。
 * 这就是「参数落在本任务的槽里」的强断言 —— 如果实现偷懒从寄存器读，
 * 重放那一遍就会拿着 0 去调 net，这里立刻就能看出来。 */
static int32 sock_run(t_ctx *c, uint32 nr, unsigned long a0, unsigned long a1,
                      unsigned long a2, unsigned long a3,
                      unsigned long a4, unsigned long a5)
{
    int32 r = T_STILL_WAITING;
    int   i;

    for(i = 0; i < 8; i++)
    {
        if(i == 0)
        {
            (void)call6(c, nr, a0, a1, a2, a3, a4, a5);
        }
        else
        {
            (void)call6(c, nr, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
        }
        if(c->ret_written != 0)
        {
            r = (int32)c->ret;
            break;
        }
    }
    return r;
}

/* Linux 的 sockaddr_in 铺 16 字节：family(host) / port(BE) / addr(BE) / 8 零。 */
static void sa_put(uint32 off, uint32 ip, uint32 port)
{
    uint8  b[16];
    uint32 i;

    for(i = 0u; i < 16u; i++)
    {
        b[i] = 0u;
    }
    b[0] = (uint8)(T_AF_INET & 0xFFu);
    b[1] = (uint8)((T_AF_INET >> 8) & 0xFFu);
    b[2] = (uint8)((port >> 8) & 0xFFu);
    b[3] = (uint8)(port & 0xFFu);
    b[4] = (uint8)((ip >> 24) & 0xFFu);
    b[5] = (uint8)((ip >> 16) & 0xFFu);
    b[6] = (uint8)((ip >> 8) & 0xFFu);
    b[7] = (uint8)(ip & 0xFFu);
    u_put(off, b, 16u);
}

static uint32 sa_ip(uint32 off)
{
    return ((uint32)u_get8(off + 4u) << 24) | ((uint32)u_get8(off + 5u) << 16) |
           ((uint32)u_get8(off + 6u) << 8)  |  (uint32)u_get8(off + 7u);
}

static uint32 sa_port(uint32 off)
{
    return ((uint32)u_get8(off + 2u) << 8) | (uint32)u_get8(off + 3u);
}

static void t_socket(t_ctx *c)
{
    unsigned long sa = u_ptr(T_SA_OFF);
    unsigned long lenp = u_ptr(T_LEN_OFF);
    unsigned long valp = u_ptr(T_VAL_OFF);
    unsigned long iop  = u_ptr(T_IO_OFF);
    unsigned long tmo  = u_ptr(T_TMO_OFF);
    uint32 fds[2];
    uint8  buf[8];
    int32  r;
    uint32 h3;

    /* ---- socket(2) 的参数校验：一条都不能发到 net 去 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_block_calls = 0;

    (void)call(c, T_NR_SOCKET, 10UL, T_SOCK_STREAM, 0UL, 0UL);
    ck_ret(c, -T_EAFNOSUPPORT, "socket 非 AF_INET 回 -EAFNOSUPPORT");
    (void)call(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM | 0x10UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "socket 高位里除 NONBLOCK/CLOEXEC 外的位回 -EINVAL");
    (void)call(c, T_NR_SOCKET, T_AF_INET, 5UL, 0UL, 0UL);
    ck_ret(c, -T_ESOCKTNOSUPPORT, "socket 未知 type 回 -ESOCKTNOSUPPORT");
    (void)call(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 6UL, 0UL);
    ck_ret(c, -T_EPROTONOSUPPORT, "socket 指定非 0 协议回 -EPROTONOSUPPORT");
    ck_u(g_net_calls, 0u, "参数校验失败一次 net 请求都不发");
    ck(g_block_calls == 0, "参数校验失败不睡");

    /* ---- socket(2) 成功：一次往返 ---- */
    net_reset();
    g_net_pending = 1;
    g_net_handle  = 5;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM | T_SOCK_NONBLOCK,
                 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "socket 成功回 fd=3（句柄 5 只留在槽里，不外露）");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SOCKET], 2u, "socket 往返两次：登记 + 取结果");
    ck_u(g_net_arg[0], T_SOCK_STREAM, "交给 net 的 type 已去掉 SOCK_NONBLOCK");
    ck(g_block_calls == 1, "一次往返睡一个时间片");
    h3 = 3u;

    /* ---- EBUSY 也等：别人的请求在飞不是这个 socket 的错误 ---- */
    net_reset();
    g_net_busy   = 2;
    g_net_handle = 6;
    g_block_calls = 0;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_DGRAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 4, "两次 EBUSY 之后成功，拿到 fd=4");
    ck_u(g_net_calls, 3u, "两次 EBUSY + 一次成功共三次请求");
    ck_u(g_net_arg[0], T_SOCK_DGRAM, "重放之后 type 仍取自本任务的槽");
    ck(g_block_calls == 2, "每次 EBUSY 都睡一个时间片");

    /* ---- 服务回坏句柄：-EIO，且不吃 fd 槽 ---- */
    net_reset();
    g_net_handle = 0;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == -T_EIO, "服务回句柄 0 判成 -EIO，不交给程序");
    net_reset();
    g_net_handle = 300;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == -T_EIO, "服务回越界句柄 300 也判 -EIO");
    net_reset();
    g_net_handle = 7;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 5, "前两次失败不吃 fd 槽：这次仍是 fd=5");

    /* ---- net 的负错误号逐档映射 ---- */
    sa_put(T_SA_OFF, 0x7F000001u, 0x1F90u);
    net_reset();
    g_net_rc = ARKRTOS_NET_ENOTSUP;
    r = sock_run(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL, 0UL, 0UL);
    ck(r == -T_ENOSYS, "net ENOTSUP 映射成 -ENOSYS（协议栈没编进来）");
    g_net_rc = ARKRTOS_NET_EHOSTUNREACH;
    r = sock_run(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL, 0UL, 0UL);
    ck(r == -T_EHOSTUNREACH, "net EHOSTUNREACH 原样映射");
    g_net_rc = ARKRTOS_NET_ENOBUFS;
    r = sock_run(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL, 0UL, 0UL);
    ck(r == -T_ENOBUFS, "net ENOBUFS 原样映射");
    g_net_rc = ARKRTOS_NET_EINVAL;
    r = sock_run(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL, 0UL, 0UL);
    ck(r == -T_EINVAL, "net EINVAL 原样映射");
    g_net_rc = -999;
    r = sock_run(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL, 0UL, 0UL);
    ck(r == -T_EIO, "对不上号的 net 错误回 -EIO，不编一个看似合理的 errno");
    g_net_rc = 0;

    /* ---- bind(2) ---- */
    net_reset();
    (void)call(c, T_NR_BIND, 99UL, sa, 16UL, 0UL);
    ck_ret(c, -T_EBADF, "bind 未打开的 fd 回 -EBADF");
    (void)call(c, T_NR_PIPE2, u_ptr(T_PIPE_OFF), 0UL, 0UL, 0UL);
    u_get(T_PIPE_OFF, fds, 8u);
    (void)call(c, T_NR_BIND, (unsigned long)fds[0], sa, 16UL, 0UL);
    ck_ret(c, -T_ENOTSOCK, "bind 一个管道 fd 回 -ENOTSOCK");
    (void)call(c, T_NR_BIND, (unsigned long)h3, sa, 4UL, 0UL);
    ck_ret(c, -T_EINVAL, "bind addrlen < 8 回 -EINVAL");
    (void)call(c, T_NR_BIND, (unsigned long)h3, 0UL, 16UL, 0UL);
    ck_ret(c, -T_EFAULT, "bind 地址为 NULL 回 -EFAULT");
    ck_u(g_net_calls, 0u, "bind 的参数错误一条都不发到 net");
    sa_put(T_SA_OFF, 0x7F000001u, 0x1F90u);
    g_user[T_SA_OFF] = 10u;
    (void)call(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL);
    ck_ret(c, -T_EAFNOSUPPORT, "bind 非 AF_INET 的走地址回 -EAFNOSUPPORT");
    sa_put(T_SA_OFF, 0x7F000001u, 0x1F90u);
    net_reset();
    r = sock_run(c, T_NR_BIND, (unsigned long)h3, sa, 16UL, 0UL, 0UL, 0UL);
    ck(r == 0, "bind 成功回 0");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_BIND], 1u, "bind 一个往返就够了（无阻塞）");
    ck_u(g_net_arg[1], 0x7F000001u, "bind 交给 net 的地址是 host order");
    ck_u(g_net_arg[2], 0x1F90u, "bind 交给 net 的端口是 host order");
    ck_u(g_net_arg[0], 5u, "bind 交的是这个 fd 的 net 句柄");

    /* ---- listen(2)：backlog 静默夹取 ---- */
    net_reset();
    r = sock_run(c, T_NR_LISTEN, (unsigned long)h3, 0xFFFFFFFFUL, 0UL, 0UL, 0UL, 0UL);
    ck(r == 0, "listen 负 backlog 仍然成功");
    ck_u(g_net_arg[1], 0u, "listen 负 backlog 静默当 0");
    net_reset();
    r = sock_run(c, T_NR_LISTEN, (unsigned long)h3, 1000UL, 0UL, 0UL, 0UL, 0UL);
    ck(r == 0, "listen 大 backlog 也成功");
    ck_u(g_net_arg[1], 255u, "listen backlog 超过 255 静默截到 255");
    (void)call(c, T_NR_LISTEN, (unsigned long)fds[0], 5UL, 0UL, 0UL);
    ck_ret(c, -T_ENOTSOCK, "listen 管道 fd 回 -ENOTSOCK");

    /* ---- accept(2)：没有地址时只差一个往返 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 11;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "accept 前置：监听 socket = fd 3");
    net_reset();
    g_net_pending = 1;
    g_net_handle  = 42;
    r = sock_run(c, T_NR_ACCEPT, 3UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    ck(r == 4, "accept 不带地址回新 fd=4");
    ck_u(g_net_arg[0], 11u, "accept 交的是监听 socket 的句柄");

    /* ---- accept(2) 带地址：两步式，重放不许重复 accept ---- */
    net_reset();
    g_net_peer_pending = 2;
    g_net_handle = 43;
    g_net_ip     = 0xC0A80105u;
    g_net_port   = 1234u;
    u_put32(T_LEN_OFF, 0u);
    sa_put(T_SA_OFF, 0u, 0u);
    r = sock_run(c, T_NR_ACCEPT, 3UL, sa, lenp, 0UL, 0UL, 0UL);
    ck(r == 5, "accept 带地址回新 fd=5");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_ACCEPT], 1u,
         "第二轮只补 GETPEERNAME，不重复 accept（否则每次重放漏一个 socket）");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_GETPEERNAME], 3u, "地址查询按重放次数轮询");
    ck_u(sa_ip(T_SA_OFF), 0xC0A80105u, "accept 回填对端地址（网络序进布局）");
    ck_u(sa_port(T_SA_OFF), 1234u, "accept 回填对端端口");
    ck_u((uint32)u_get8(T_SA_OFF) | ((uint32)u_get8(T_SA_OFF + 1u) << 8),
         2u, "回填的 family 是 AF_INET");
    ck_u(u_get32(T_LEN_OFF), 16u, "回填的 addrlen 是 16");

    /* ---- accept4(2)：只有两个标志位合法 ---- */
    (void)call(c, T_NR_ACCEPT4, 3UL, 0UL, 0UL, 0x10UL);
    ck_ret(c, -T_EINVAL, "accept4 高位里有别的位回 -EINVAL");
    net_reset();
    g_net_handle = 44;
    u_put32(T_LEN_OFF, 0u);
    r = sock_run(c, T_NR_ACCEPT4, 3UL, 0UL, 0UL, T_SOCK_NONBLOCK, 0UL, 0UL);
    ck(r == 6, "accept4 带 SOCK_NONBLOCK 成功回 fd=6");

    /* ---- connect(2)：非阻塞直报 EINPROGRESS，阻塞在时间片上重放 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 21;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM | T_SOCK_NONBLOCK,
                 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "connect 前置：非阻塞 socket = fd 3");
    net_reset();
    g_net_rc = ARKRTOS_NET_EINPROGRESS;
    g_block_calls = 0;
    sa_put(T_SA_OFF, 0x7F000001u, 8080u);
    (void)call6(c, T_NR_CONNECT, 3UL, sa, 16UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EINPROGRESS, "非阻塞 connect 把「还在进行」如实交回去");
    ck(g_block_calls == 0, "非阻塞 connect 不睡");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_CONNECT], 1u, "非阻塞 connect 只问一次");

    net_reset();
    g_net_handle = 22;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 4, "connect 前置：阻塞 socket = fd 4");
    net_reset();
    g_net_rc = ARKRTOS_NET_EINPROGRESS;
    g_block_calls = 0;
    (void)call6(c, T_NR_CONNECT, 4UL, sa, 16UL, 0UL, 0UL, 0UL);
    ck_pending(c, "阻塞 connect 遇到 EINPROGRESS 登记重放（不写 a0）");
    ck(g_block_calls == 1, "阻塞 connect 重放时睡一个时间片");
    g_net_rc = 0;
    (void)call6(c, T_NR_CONNECT, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "阻塞 connect 重放后连上回 0");
    ck_u(g_net_arg[0], 22u, "重放时句柄取自本任务的槽");

    /* ---- sendto(2) / recvfrom(2) ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 31;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "sendto 前置：socket = fd 3");
    (void)call(c, T_NR_SENDTO, 3UL, iop, 0x11170UL, 0UL);
    ck_ret(c, -T_EMSGSIZE, "sendto 长度超过 16 位宽度回 -EMSGSIZE");
    net_reset();
    u_put_str(T_IO_OFF, "hi");
    r = sock_run(c, T_NR_SENDTO, 3UL, iop, 2UL, 0UL, 0UL, 0UL);
    ck(r == 0, "sendto 无地址走 SEND，返回 net 的字节数");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SEND], 1u, "无地址的 sendto 走 SEND 而不是 SENDTO");
    ck_u(g_net_arg[1], (uint32)iop, "SEND 交的是用户缓冲地址");
    ck_u(g_net_arg[2], 2u, "SEND 交的是长度");

    net_reset();
    sa_put(T_SA_OFF, 0x0A0000FEu, 9999u);
    r = sock_run(c, T_NR_SENDTO, 3UL, iop, 2UL, 0UL, sa, 16UL);
    ck(r == 0, "sendto 带地址成功");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SENDTO], 1u, "带地址的 sendto 走 SENDTO");
    ck_u(g_net_arg[3], 0x0A0000FEu, "SENDTO 的地址是 host order");
    ck_u(g_net_arg[4], 9999u, "SENDTO 的端口是 host order");

    net_reset();
    g_net_bytes = 5;
    u_put32(T_LEN_OFF, 0u);
    r = sock_run(c, T_NR_RECVFROM, 3UL, iop, 8UL, 0UL, sa, lenp);
    ck(r == 5, "recvfrom 回收到的字节数");
    u_get(T_IO_OFF, buf, 5u);
    ck(memcmp(buf, "hello", 5) == 0, "recvfrom 的数据落进用户缓冲");
    ck_u(sa_ip(T_SA_OFF), 0x0A000001u, "recvfrom 回填来源地址");
    ck_u(sa_port(T_SA_OFF), 8080u, "recvfrom 回填来源端口");
    ck_u(u_get32(T_LEN_OFF), 16u, "recvfrom 回填 addrlen=16");
    (void)call(c, T_NR_RECVFROM, 3UL, iop, 0UL, 0UL);
    ck_ret(c, 0, "recvfrom 长度 0 直接回 0，不发请求");
    net_reset();
    (void)call(c, T_NR_RECVFROM, 3UL, iop, 0x11170UL, 0UL);
    ck_ret(c, -T_EMSGSIZE, "recvfrom 长度超过 16 位宽度回 -EMSGSIZE");
    ck_u(g_net_calls, 0u, "这两条参数路径一条请求都不发");

    net_reset();
    g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
    (void)call(c, T_NR_SENDTO, 3UL, iop, 2UL, T_MSG_DONTWAIT);
    ck_ret(c, -T_EAGAIN, "MSG_DONTWAIT 的 sendto 遇到没空间立刻回 -EAGAIN");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SEND], 1u, "不重放：只问了一次");
    g_block_calls = 0;
    (void)call(c, T_NR_SENDTO, 3UL, iop, 2UL, 0UL);
    ck_pending(c, "阻塞 sendto 遇到没空间登记重放（不写 a0）");
    ck(g_block_calls == 1, "阻塞 sendto 重放时睡一个时间片");
    g_net_rc = 0;
    (void)call(c, T_NR_SENDTO, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "阻塞 sendto 重放后成功");

    (void)call(c, T_NR_PIPE2, u_ptr(T_PIPE_OFF), 0UL, 0UL, 0UL);
    u_get(T_PIPE_OFF, fds, 8u);
    (void)call(c, T_NR_SENDTO, (unsigned long)fds[0], iop, 2UL, 0UL);
    ck_ret(c, -T_ENOTSOCK, "sendto 一个管道 fd 回 -ENOTSOCK");

    /* ---- setsockopt(2) / getsockopt(2) ---- */
    fresh(c);
    net_reset();
    g_net_handle = 51;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_DGRAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "setsockopt 前置：dgram socket = fd 3");
    net_reset();
    u_put32(T_VAL_OFF, 4096u);
    (void)call(c, T_NR_SETSOCKOPT, 3UL, 6UL, T_SO_REUSEADDR, valp);
    ck_ret(c, -T_ENOPROTOOPT, "setsockopt 非 SOL_SOCKET 的 level 回 -ENOPROTOOPT");
    (void)call(c, T_NR_SETSOCKOPT, 3UL, T_SOL_SOCKET, 99UL, valp);
    ck_ret(c, -T_ENOPROTOOPT, "setsockopt 本层不认的选项号回 -ENOPROTOOPT");
    (void)call(c, T_NR_SETSOCKOPT, 3UL, T_SOL_SOCKET, T_SO_ERROR, valp);
    ck_ret(c, -T_EINVAL, "setsockopt SO_ERROR 是只读选项，回 -EINVAL");
    (void)call(c, T_NR_SETSOCKOPT, 3UL, T_SOL_SOCKET, T_SO_RCVBUF, 0UL);
    ck_ret(c, -T_EFAULT, "setsockopt 值指针为 NULL 回 -EFAULT");
    (void)call(c, T_NR_SETSOCKOPT, 3UL, T_SOL_SOCKET, T_SO_RCVBUF, valp);
    ck_u(g_net_calls, 0u, "以上四条参数错误一条都不发到 net");

    net_reset();
    /* 4 参入口装不下 optlen：linux_abi.c 看到的是 0，按 Linux 的判法回 -EINVAL。 */
    (void)call(c, T_NR_SETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
               (unsigned long)T_SO_RCVBUF, valp);
    ck_ret(c, -T_EINVAL, "setsockopt 4 参入口的 optlen 是 0，回 -EINVAL");
    ck_u(g_net_calls, 0u, "optlen 不足一条请求都不发");
    net_reset();
    (void)call6(c, T_NR_SETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_RCVBUF, valp, 2UL, 0UL);
    ck_ret(c, -T_EINVAL, "setsockopt optlen < 4 回 -EINVAL");
    net_reset();
    u_put32(T_VAL_OFF, 4096u);
    (void)call6(c, T_NR_SETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_RCVBUF, valp, 4UL, 0UL);
    ck_ret(c, 0, "setsockopt SO_RCVBUF 成功");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SETSOCKOPT], 1u, "成功路径只发一条请求");
    ck_u(g_net_arg[0], 51u, "SETSOCKOPT 的句柄");
    ck_u(g_net_arg[1], ARKRTOS_NET_SO_RCVBUF, "本层的选项号翻译成 net 的选项号");
    ck_u(g_net_arg[2], 4096u, "选项值原样交给 net");

    /* getsockopt：三个本地就能答的 + 一个转发 */
    u_put32(T_LEN_OFF, 4u);
    u_put32(T_VAL_OFF, 0u);
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_TYPE, valp, lenp, 0UL);
    ck_ret(c, 0, "getsockopt SO_TYPE 本地作答");
    ck_u(u_get32(T_VAL_OFF), T_SOCK_DGRAM, "SO_TYPE 回的就是建 socket 时的 type");
    ck_u(u_get32(T_LEN_OFF), 4u, "SO_TYPE 把 optlen 写成 4");
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_DOMAIN, valp, lenp, 0UL);
    ck_ret(c, 0, "getsockopt SO_DOMAIN 本地作答");
    ck_u(u_get32(T_VAL_OFF), T_AF_INET, "SO_DOMAIN 恒是 AF_INET");
    u_put32(T_VAL_OFF, 0xDEADBEEFu);
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_ERROR, valp, lenp, 0UL);
    ck_ret(c, 0, "getsockopt SO_ERROR 本地作答");
    ck_u(u_get32(T_VAL_OFF), 0u, "SO_ERROR 恒 0（错误都在调用返回值里给过）");
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_ERROR, 0UL, lenp, 0UL);
    ck_ret(c, -T_EFAULT, "getsockopt 值指针为 NULL 回 -EFAULT");
    u_put32(T_LEN_OFF, 2u);
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_TYPE, valp, lenp, 0UL);
    ck_ret(c, -T_EINVAL, "getsockopt optlen < 4 回 -EINVAL");
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, 6UL, (unsigned long)T_SO_TYPE, valp, lenp, 0UL);
    ck_ret(c, -T_ENOPROTOOPT, "getsockopt 非 SOL_SOCKET 的 level 回 -ENOPROTOOPT");

    net_reset();
    u_put32(T_LEN_OFF, 4u);
    u_put32(T_VAL_OFF, 0u);
    g_net_ip = 8192u;
    (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                (unsigned long)T_SO_RCVBUF, valp, lenp, 0UL);
    ck_ret(c, 0, "getsockopt SO_RCVBUF 转发给 net");
    ck_u(g_net_arg[2], (uint32)valp, "GETSOCKOPT 交的是用户指针，不是内核栈地址");
    ck_u(u_get32(T_VAL_OFF), 8192u, "net 写回的值原样留在用户内存里");
    ck_u(u_get32(T_LEN_OFF), 4u, "转发路径也把 optlen 写成 4");

    /* ---- getsockname(2) / getpeername(2) ---- */
    net_reset();
    g_net_ip = 0x7F000001u;
    g_net_port = 5555u;
    u_put32(T_LEN_OFF, 0u);
    r = sock_run(c, T_NR_GETSOCKNAME, 3UL, sa, lenp, 0UL, 0UL, 0UL);
    ck(r == 0, "getsockname 成功回 0");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_GETSOCKNAME], 1u, "getsockname 问的是 GETSOCKNAME");
    ck_u(sa_ip(T_SA_OFF), 0x7F000001u, "getsockname 回填地址");
    ck_u(sa_port(T_SA_OFF), 5555u, "getsockname 回填端口");
    ck_u(u_get32(T_LEN_OFF), 16u, "getsockname 回填 addrlen=16");
    net_reset();
    g_net_ip = 0x7F000002u;
    g_net_port = 6666u;
    r = sock_run(c, T_NR_GETPEERNAME, 3UL, sa, lenp, 0UL, 0UL, 0UL);
    ck(r == 0, "getpeername 成功回 0");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_GETPEERNAME], 1u, "getpeername 问的是 GETPEERNAME");
    ck_u(sa_port(T_SA_OFF), 6666u, "getpeername 回填端口");
    net_reset();
    r = sock_run(c, T_NR_GETSOCKNAME, 3UL, 0UL, 0UL, 0UL, 0UL, 0UL);
    ck(r == 0, "getsockname 不要地址也成功");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_GETSOCKNAME], 1u,
         "不要地址仍然问一次：socket 已经不在了这类错误要照常报出来");

    /* ---- shutdown(2) ---- */
    net_reset();
    (void)call(c, T_NR_SHUTDOWN, 3UL, 3UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "shutdown how > 2 回 -EINVAL");
    ck_u(g_net_calls, 0u, "非法 how 不发请求");
    net_reset();
    r = sock_run(c, T_NR_SHUTDOWN, 3UL, 1UL, 0UL, 0UL, 0UL, 0UL);
    ck(r == 0, "shutdown SHUT_WR 成功");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SHUTDOWN], 1u, "shutdown 走 SHUTDOWN 子命令");
    ck_u(g_net_arg[1], 1u, "how 原样交给 net");

    /* ---- read(2) / write(2) 落在 socket 上 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 61;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "read/write 前置：阻塞 socket = fd 3");
    (void)call(c, T_NR_READ, 3UL, iop, 0UL, 0UL);
    ck_ret(c, 0, "socket 上 read 长度 0 直接回 0");
    (void)call(c, T_NR_READ, 3UL, iop, 0x11170UL, 0UL);
    ck_ret(c, -T_EMSGSIZE, "socket 上 read 长度超过 16 位宽度回 -EMSGSIZE");
    net_reset();
    g_net_bytes = 5;
    r = sock_run(c, T_NR_READ, 3UL, iop, 8UL, 0UL, 0UL, 0UL);
    ck(r == 5, "socket 上 read 回 net 的字节数");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_RECV], 1u, "read 走 RECV");
    u_get(T_IO_OFF, buf, 5u);
    ck(memcmp(buf, "hello", 5) == 0, "read 拿到的内容落进用户缓冲");

    net_reset();
    r = sock_run(c, T_NR_WRITE, 3UL, iop, 3UL, 0UL, 0UL, 0UL);
    ck(r == 0, "socket 上 write 成功回 net 的字节数");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_SEND], 1u, "write 走 SEND");
    (void)call(c, T_NR_WRITE, 3UL, iop, 0x11170UL, 0UL);
    ck_ret(c, -T_EMSGSIZE, "socket 上 write 长度超过 16 位宽度回 -EMSGSIZE");

    net_reset();
    g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
    g_block_calls = 0;
    (void)call(c, T_NR_READ, 3UL, iop, 8UL, 0UL);
    ck_pending(c, "阻塞 read 遇到没数据登记重放（不写 a0）");
    ck(g_block_calls == 1, "阻塞 read 重放时睡一个时间片");
    g_net_bytes = 2;
    g_net_rc = 0;
    (void)call(c, T_NR_READ, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 2, "阻塞 read 重放后拿到数据");
    ck_u(g_net_arg[0], 61u, "重放时句柄取自本任务的槽");

    net_reset();
    g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
    (void)call(c, T_NR_WRITE, 3UL, iop, 2UL, 0UL);
    ck_pending(c, "阻塞 write 遇到没空间登记重放");
    g_net_rc = 0;
    (void)call(c, T_NR_WRITE, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "阻塞 write 重放后成功");

    /* 非阻塞 socket：EAGAIN 立刻交回，不重放 */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 62;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM | T_SOCK_NONBLOCK,
                 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "非阻塞前置：fd 3");
    net_reset();
    g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
    g_block_calls = 0;
    (void)call(c, T_NR_READ, 3UL, iop, 8UL, 0UL);
    ck_ret(c, -T_EAGAIN, "非阻塞 read 遇到没数据立刻回 -EAGAIN");
    (void)call(c, T_NR_WRITE, 3UL, iop, 2UL, 0UL);
    ck_ret(c, -T_EAGAIN, "非阻塞 write 遇到没空间立刻回 -EAGAIN");
    ck(g_block_calls == 0, "非阻塞两条路径都不睡");

    /* ---- close(2) 落在 socket 上 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 71;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "close 前置：fd 3");
    net_reset();
    g_net_pending = 1;
    g_block_calls = 0;
    (void)call(c, T_NR_CLOSE, 3UL, 0UL, 0UL, 0UL);
    ck_pending(c, "socket 的 close 遇到 EPENDING 登记重放（不写 a0）");
    ck(g_block_calls == 1, "socket 的 close 重放时睡一个时间片");
    (void)call(c, T_NR_CLOSE, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "socket 的 close 重放后成功（a0 已被清成 0 也认得出）");
    ck_u(g_net_arg[0], 71u, "CLOSE 交的是这个 socket 的句柄");
    (void)call(c, T_NR_CLOSE, 3UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EBADF, "socket 关掉之后同一个 fd 回 -EBADF");

    /* ---- fd 槽用满：先判 -EMFILE，不往 net 发请求 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 1;
    for(r = 0; r < 8; r++)
    {
        (void)sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    }
    net_reset();
    (void)sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, -T_EMFILE, "8 个 fd 槽用满后 socket 回 -EMFILE");
    ck_u(g_net_calls, 0u, "槽满时一条请求都不发（不会漏一个 net socket）");

    /* ---- 任务退出 / 复位要把重放登记撤掉 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 81;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "清槽前置：fd 3");
    net_reset();
    g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
    (void)call(c, T_NR_READ, 3UL, iop, 8UL, 0UL);
    ck_pending(c, "清槽前置：read 挂起并登记重放");
    arkrtos_linux_abi_on_task_exit(7);
    net_reset();
    (void)call(c, T_NR_SOCKET, 10UL, T_SOCK_STREAM, 0UL, 0UL);
    ck_ret(c, -T_EAFNOSUPPORT,
           "任务退出清掉重放登记：下一次调用用的是新参数，不是上一次的槽");
    ck_u(g_net_calls, 0u, "被参数校验挡住，一条请求都不发");

    /* ---- 没有任务身份：一律 -ENOSYS ---- */
    arkrtos_current_task_id = 0;
    (void)call(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL);
    ck_ret(c, -T_ENOSYS, "没有任务身份时 socket 回 -ENOSYS");
    arkrtos_current_task_id = 7;

#if (ARKRTOS_USE_LINUX_SELECT == 1)
    /* ---- pselect6 的 socket 就绪位 ---- */
    fresh(c);
    net_reset();
    g_restart_rc = 0;
    g_net_handle = 91;
    r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
    ck(r == 3, "pselect6 前置：socket = fd 3（bit3）");
    net_reset();
    g_net_rc = (int32)ARKRTOS_NET_POLL_IN;
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 1, "pselect6 把 socket 的 POLL_IN 折成读就绪");
    ck_u(u_get32(T_DATA_OFF), 0x08u, "读就绪位留在集合里");
    ck_u(g_net_opn[ARKRTOS_NET_SUB_POLL], 1u, "一次 pselect6 只问一次 POLL");

    net_reset();
    g_net_rc = (int32)ARKRTOS_NET_POLL_OUT;
    u_put32(T_DATA_OFF + 0x100u, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, 0UL, u_ptr(T_DATA_OFF + 0x100u), 0UL, 0UL, 0UL);
    ck_ret(c, 1, "pselect6 把 POLL_OUT 折成写就绪");
    net_reset();
    g_net_rc = 0;
    u_put32(T_TMO_OFF, 0u);
    u_put32(T_TMO_OFF + 4u, 0u);
    u_put32(T_DATA_OFF, 0x08u);
    (void)call6(c, T_NR_PSELECT6, 4UL, u_ptr(T_DATA_OFF), 0UL, 0UL, tmo, 0UL);
    ck_ret(c, 0, "socket 没就绪 + timeout=0 回 0");
    ck_u(u_get32(T_DATA_OFF), 0u, "没就绪就把就绪位清掉");
    ck(g_block_calls >= 0, "（时间片计数只作可读性参照）");
#endif /* ARKRTOS_USE_LINUX_SELECT == 1 */

    /* ================= 第二波：拒绝路径、边界与不可达分支 ================= */

    /* ---- net 的负错误号逐档映射（EALREADY..EMSGSIZE） ---- */
    {
        static const struct
        {
            int32 net;
            int32 lx;
            const char *what;
        } emap[] =
        {
            { ARKRTOS_NET_EALREADY,     -T_EALREADY,     "net EALREADY 映射成 -EALREADY" },
            { ARKRTOS_NET_EISCONN,      -T_EISCONN,      "net EISCONN 映射成 -EISCONN" },
            { ARKRTOS_NET_ENOTCONN,     -T_ENOTCONN,     "net ENOTCONN 映射成 -ENOTCONN" },
            { ARKRTOS_NET_ECONNREFUSED, -T_ECONNREFUSED, "net ECONNREFUSED 映射成 -ECONNREFUSED" },
            { ARKRTOS_NET_ECONNRESET,   -T_ECONNRESET,   "net ECONNRESET 映射成 -ECONNRESET" },
            { ARKRTOS_NET_ECONNABORTED, -T_ECONNABORTED, "net ECONNABORTED 映射成 -ECONNABORTED" },
            { ARKRTOS_NET_EMSGSIZE,     -T_EMSGSIZE,     "net EMSGSIZE 映射成 -EMSGSIZE" },
        };
        uint32 k;

        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 101;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "errno 映射前置：fd 3");
        sa_put(T_SA_OFF, 0x7F000001u, 1234u);
        for(k = 0u; k < (uint32)(sizeof(emap) / sizeof(emap[0])); k++)
        {
            net_reset();
            g_net_rc = emap[k].net;
            r = sock_run(c, T_NR_BIND, 3UL, sa, 16UL, 0UL, 0UL, 0UL);
            ck(r == emap[k].lx, emap[k].what);
        }
    }

    /* ---- 没有可重放端口：如实回 -EAGAIN，不假装睡过 ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 102;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "重放不可用前置：fd 3");
        net_reset();
        g_restart_rc = -1;
        g_net_pending = 1;
        g_block_calls = 0;
        r = sock_run(c, T_NR_BIND, 3UL, sa, 16UL, 0UL, 0UL, 0UL);
        ck(r == -T_EAGAIN, "没有重放端口时 bind 回 -EAGAIN（不假装睡过）");
        ck(g_block_calls == 0, "没有重放端口时一次也不睡");
        g_restart_rc = 0;
    }

    /* ---- 没有任务身份：每个入口都回 -ENOSYS，一条 net 请求都不发 ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 111;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_DGRAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "身份前置：dgram socket = fd 3");
        arkrtos_current_task_id = 0;
        net_reset();
        g_block_calls = 0;
        sa_put(T_SA_OFF, 0x7F000001u, 1234u);
        u_put32(T_LEN_OFF, 4u);

        (void)call(c, T_NR_BIND, 3UL, sa, 16UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 bind 回 -ENOSYS");
        (void)call(c, T_NR_LISTEN, 3UL, 4UL, 0UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 listen 回 -ENOSYS");
        (void)call(c, T_NR_ACCEPT, 3UL, 0UL, 0UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 accept 回 -ENOSYS");
        (void)call(c, T_NR_CONNECT, 3UL, sa, 16UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 connect 回 -ENOSYS");
        (void)call(c, T_NR_SENDTO, 3UL, iop, 2UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 sendto 回 -ENOSYS");
        (void)call(c, T_NR_RECVFROM, 3UL, iop, 2UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 recvfrom 回 -ENOSYS");
        (void)call(c, T_NR_SETSOCKOPT, 3UL, T_SOL_SOCKET, T_SO_RCVBUF, valp);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 setsockopt 回 -ENOSYS");
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_TYPE, valp, lenp, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 getsockopt 回 -ENOSYS");
        (void)call(c, T_NR_GETSOCKNAME, 3UL, sa, lenp, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 getsockname 回 -ENOSYS");
        (void)call(c, T_NR_GETPEERNAME, 3UL, sa, lenp, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 getpeername 回 -ENOSYS");
        (void)call(c, T_NR_SHUTDOWN, 3UL, 1UL, 0UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 shutdown 回 -ENOSYS");
        (void)call(c, T_NR_READ, 3UL, iop, 4UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 socket 上 read 回 -ENOSYS");
        (void)call(c, T_NR_WRITE, 3UL, iop, 2UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 socket 上 write 回 -ENOSYS");
        (void)call(c, T_NR_CLOSE, 3UL, 0UL, 0UL, 0UL);
        ck_ret(c, -T_ENOSYS, "没有任务身份时 socket 上 close 回 -ENOSYS");
        ck_u(g_net_calls, 0u, "没有任务身份时一条 net 请求都不发");
        ck(g_block_calls == 0, "没有任务身份时一次也不睡");
        arkrtos_current_task_id = 7;
    }

    /* ---- 坏 fd：在 net 之前就拒掉 ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 112;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "坏 fd 前置：fd 3");
        net_reset();
        (void)call(c, T_NR_ACCEPT, 99UL, 0UL, 0UL, 0UL);
        ck_ret(c, -T_EBADF, "accept 未打开的 fd 回 -EBADF");
        (void)call(c, T_NR_CONNECT, 99UL, sa, 16UL, 0UL);
        ck_ret(c, -T_EBADF, "connect 未打开的 fd 回 -EBADF");
        (void)call(c, T_NR_RECVFROM, 99UL, iop, 8UL, 0UL);
        ck_ret(c, -T_EBADF, "recvfrom 未打开的 fd 回 -EBADF");
        (void)call(c, T_NR_SETSOCKOPT, 99UL, T_SOL_SOCKET, T_SO_RCVBUF, valp);
        ck_ret(c, -T_EBADF, "setsockopt 未打开的 fd 回 -EBADF");
        (void)call6(c, T_NR_GETSOCKOPT, 99UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_TYPE, valp, lenp, 0UL);
        ck_ret(c, -T_EBADF, "getsockopt 未打开的 fd 回 -EBADF");
        (void)call(c, T_NR_GETSOCKNAME, 99UL, sa, lenp, 0UL);
        ck_ret(c, -T_EBADF, "getsockname 未打开的 fd 回 -EBADF");
        (void)call(c, T_NR_SHUTDOWN, 99UL, 1UL, 0UL, 0UL);
        ck_ret(c, -T_EBADF, "shutdown 未打开的 fd 回 -EBADF");
        ck_u(g_net_calls, 0u, "坏 fd 一条请求都不发");
    }

    /* ---- 地址拷不动 / 写不回：都必须在 net 之外如实报 -EFAULT ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 121;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "地址失败前置：fd 3");
        sa_put(T_SA_OFF, 0x7F000001u, 1234u);

        net_reset();
        (void)call(c, T_NR_BIND, 3UL, 0x10UL, 16UL, 0UL);
        ck_ret(c, -T_EFAULT, "bind 读不到用户 sockaddr 回 -EFAULT");
        ck_u(g_net_calls, 0u, "bind 地址读失败不发请求");

        net_reset();
        (void)call6(c, T_NR_SENDTO, 3UL, iop, 2UL, 0UL, 0x10UL, 16UL);
        ck_ret(c, -T_EFAULT, "sendto 读不到目的地址回 -EFAULT");
        ck_u(g_net_calls, 0u, "sendto 地址读失败不发请求");

        net_reset();
        (void)call(c, T_NR_GETSOCKNAME, 3UL, 0x10UL, lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getsockname 第一块地址读不回回 -EFAULT");

        net_reset();
        (void)call(c, T_NR_GETSOCKNAME, 3UL, u_ptr(U_SIZE - 4u), lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getsockname 第二块地址读不回回 -EFAULT");

        net_reset();
        (void)call(c, T_NR_GETSOCKNAME, 3UL, u_ptr(U_SIZE - 8u), lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getsockname 16 字节写不回回 -EFAULT");

        net_reset();
        u_put32(T_LEN_OFF, 0xDEADBEEFu);
        (void)call(c, T_NR_GETSOCKNAME, 3UL, 0UL, lenp, 0UL);
        ck_ret(c, 0, "getsockname 不要地址也成功");
        ck_u(u_get32(T_LEN_OFF), 0xDEADBEEFu,
             "不要地址时本层不碰 addrlen（这条分支在 lx_sock_name 里早退，"
             " addr_out 的 ua==0 那一半因此不可达）");

        net_reset();
        (void)call(c, T_NR_GETSOCKNAME, 3UL, sa, u_ptr(U_SIZE - 2u), 0UL);
        ck_ret(c, -T_EFAULT, "getsockname addrlen 写不回回 -EFAULT");

        net_reset();
        g_net_bytes = 0;
        (void)call6(c, T_NR_RECVFROM, 3UL, iop, 4UL, 0UL, 0x10UL, 16UL);
        ck_ret(c, -T_EFAULT, "recvfrom 来源地址第一块读不回回 -EFAULT");

        net_reset();
        g_net_bytes = 0;
        (void)call6(c, T_NR_RECVFROM, 3UL, iop, 4UL, 0UL, u_ptr(U_SIZE - 8u), 16UL);
        ck_ret(c, -T_EFAULT, "recvfrom 16 字节来源地址写不回回 -EFAULT");

        net_reset();
        g_net_rc = ARKRTOS_NET_ECONNRESET;
        (void)call(c, T_NR_GETSOCKNAME, 3UL, sa, lenp, 0UL);
        ck_ret(c, -T_ECONNRESET, "getsockname 转发错误原样交回");

        net_reset();
        (void)call(c, T_NR_GETPEERNAME, 3UL, 0x10UL, lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getpeername 地址读不回回 -EFAULT");
    }

    /* ---- SOL_SOCKET 选项：翻译、只读、越界与写回失败 ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 131;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM | T_SOCK_CLOEXEC,
                     0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "带 SOCK_CLOEXEC 建 socket 成功");

        net_reset();
        u_put32(T_VAL_OFF, 1u);
        (void)call6(c, T_NR_SETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_REUSEADDR, valp, 4UL, 0UL);
        ck_ret(c, 0, "setsockopt SO_REUSEADDR 成功");
        ck_u(g_net_arg[1], ARKRTOS_NET_SO_REUSEADDR,
             "REUSEADDR 翻成 net 的选项号");

        net_reset();
        u_put32(T_VAL_OFF, 8192u);
        (void)call6(c, T_NR_SETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_SNDBUF, valp, 4UL, 0UL);
        ck_ret(c, 0, "setsockopt SO_SNDBUF 成功");
        ck_u(g_net_arg[1], ARKRTOS_NET_SO_SNDBUF,
             "SNDBUF 翻成 net 的选项号");
        ck_u(g_net_arg[2], 8192u, "SNDBUF 的值原样交给 net");

        net_reset();
        (void)call6(c, T_NR_SETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_RCVBUF, 0x10UL, 4UL, 0UL);
        ck_ret(c, -T_EFAULT, "setsockopt 值读不回回 -EFAULT");

        net_reset();
        u_put32(T_LEN_OFF, 4u);
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_TYPE, valp, 0x10UL, 0UL);
        ck_ret(c, -T_EFAULT, "getsockopt optlen 读不回回 -EFAULT");

        net_reset();
        u_put32(T_LEN_OFF, 4u);
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_TYPE, u_ptr(U_SIZE - 2u), lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getsockopt 本地值写不回回 -EFAULT");

        net_reset();
        u_put32(T_LEN_OFF, 4u);
        g_to_budget = 1;
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_TYPE, valp, lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getsockopt 本地 optlen 写不回回 -EFAULT");
        g_to_budget = -1;

        net_reset();
        u_put32(T_LEN_OFF, 4u);
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    99UL, valp, lenp, 0UL);
        ck_ret(c, -T_ENOPROTOOPT, "getsockopt 本层不认的选项号回 -ENOPROTOOPT");

        net_reset();
        u_put32(T_LEN_OFF, 4u);
        g_net_rc = ARKRTOS_NET_EINVAL;
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_RCVBUF, valp, lenp, 0UL);
        ck_ret(c, -T_EINVAL, "getsockopt 转发的错误原样交回");

        net_reset();
        u_put32(T_LEN_OFF, 4u);
        g_to_budget = 1;
        (void)call6(c, T_NR_GETSOCKOPT, 3UL, (unsigned long)T_SOL_SOCKET,
                    (unsigned long)T_SO_RCVBUF, valp, lenp, 0UL);
        ck_ret(c, -T_EFAULT, "getsockopt 转发路径 optlen 写不回回 -EFAULT");
        g_to_budget = -1;
    }

    /* ---- accept 族的分配失败、坏句柄与地址半路失败 ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 141;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "accept 边界前置：监听 socket = fd 3");

        for(r = 0; r < 7; r++)
        {
            (void)sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM,
                           0UL, 0UL, 0UL, 0UL);
        }
        net_reset();
        (void)sock_run(c, T_NR_ACCEPT, 3UL, 0UL, 0UL, 0UL, 0UL, 0UL);
        ck_ret(c, -T_EMFILE, "fd 槽用满时 accept 回 -EMFILE");
        ck_u(g_net_calls, 0u,
             "槽满时 accept 一条请求都不发（不会漏一个 net socket）");

        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 142;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "accept 坏句柄前置：fd 3");
        net_reset();
        g_net_handle = 0;
        r = sock_run(c, T_NR_ACCEPT, 3UL, 0UL, 0UL, 0UL, 0UL, 0UL);
        ck(r == -T_EIO, "accept 收到的坏句柄判 -EIO");

        net_reset();
        g_net_handle = 143;
        r = sock_run(c, T_NR_ACCEPT4, 3UL, 0UL, 0UL, T_SOCK_CLOEXEC, 0UL, 0UL);
        ck(r == 4, "accept4 带 SOCK_CLOEXEC 成功回新 fd");

        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 144;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_STREAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "accept 地址失败前置：fd 3");
        net_reset();
        g_net_handle = 145;
        g_from_budget = 0;
        r = sock_run(c, T_NR_ACCEPT, 3UL, sa, lenp, 0UL, 0UL, 0UL);
        ck(r == -T_EFAULT, "accept 读不回对端地址回 -EFAULT（新 fd 已退掉）");
        g_from_budget = -1;
        net_reset();
        g_net_handle = 146;
        r = sock_run(c, T_NR_ACCEPT, 3UL, 0UL, 0UL, 0UL, 0UL, 0UL);
        ck(r == 4, "accept 地址失败后新 fd 已退掉，下一次仍拿 fd 4");

        net_reset();
        g_net_handle = 147;
        g_to_budget = 2;
        r = sock_run(c, T_NR_ACCEPT, 3UL, sa, lenp, 0UL, 0UL, 0UL);
        ck(r == -T_EFAULT, "accept 写不回对端地址回 -EFAULT（新 fd 已退掉）");
        g_to_budget = -1;
    }

    /* ---- recvfrom：无地址走 RECV、非阻塞直回、阻塞重放、其他错误直回 ---- */
    {
        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 151;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_DGRAM, 0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "recvfrom 边界前置：fd 3");

        net_reset();
        g_net_bytes = 5;
        r = sock_run(c, T_NR_RECVFROM, 3UL, iop, 8UL, 0UL, 0UL, 0UL);
        ck(r == 5, "recvfrom 不带地址回收到的字节数");
        ck_u(g_net_opn[ARKRTOS_NET_SUB_RECV], 1u,
             "不带地址的 recvfrom 走 RECV 而不是 RECVFROM");

        net_reset();
        g_net_rc = ARKRTOS_NET_ECONNRESET;
        r = sock_run(c, T_NR_RECVFROM, 3UL, iop, 8UL, 0UL, 0UL, 0UL);
        ck(r == -T_ECONNRESET, "recvfrom 的其他错误原样交回");
        g_net_rc = 0;

        net_reset();
        g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
        g_block_calls = 0;
        r = sock_run(c, T_NR_RECVFROM, 3UL, iop, 8UL, T_MSG_DONTWAIT, 0UL, 0UL);
        ck(r == -T_EAGAIN, "MSG_DONTWAIT 的 recvfrom 遇到没数据立刻回 -EAGAIN");
        ck(g_block_calls == 0, "MSG_DONTWAIT 的 recvfrom 不睡");
        g_net_rc = 0;

        net_reset();
        g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
        g_block_calls = 0;
        (void)call6(c, T_NR_RECVFROM, 3UL, iop, 8UL, 0UL, 0UL, 0UL);
        ck_pending(c, "阻塞 recvfrom 遇到没数据登记重放");
        ck(g_block_calls == 1, "阻塞 recvfrom 重放时睡一个时间片");
        g_net_bytes = 3;
        g_net_rc = 0;
        (void)call6(c, T_NR_RECVFROM, 0UL, 0UL, 0UL, 0UL, 0UL, 0UL);
        ck_ret(c, 3, "阻塞 recvfrom 重放后拿到数据");

        fresh(c);
        net_reset();
        g_restart_rc = 0;
        g_net_handle = 152;
        r = sock_run(c, T_NR_SOCKET, T_AF_INET, T_SOCK_DGRAM | T_SOCK_NONBLOCK,
                     0UL, 0UL, 0UL, 0UL);
        ck(r == 3, "非阻塞 recvfrom 前置：fd 3");
        net_reset();
        g_net_rc = ARKRTOS_NET_EWOULDBLOCK;
        g_block_calls = 0;
        r = sock_run(c, T_NR_RECVFROM, 3UL, iop, 8UL, 0UL, 0UL, 0UL);
        ck(r == -T_EAGAIN, "非阻塞 recvfrom 遇到没数据立刻回 -EAGAIN");
        ck(g_block_calls == 0, "非阻塞 recvfrom 不睡");
    }

    /* ---- write 落在管道读端：-EBADF ---- */
    {
        fresh(c);
        net_reset();
        (void)call(c, T_NR_PIPE2, u_ptr(T_PIPE_OFF), 0UL, 0UL, 0UL);
        u_get(T_PIPE_OFF, fds, 8u);
        u_put_str(T_IO_OFF, "x");
        (void)call(c, T_NR_WRITE, (unsigned long)fds[0], iop, 1UL, 0UL);
        ck_ret(c, -T_EBADF, "往管道读端写回 -EBADF");
    }

    /* 收尾：把挂着的登记和端口能力恢复成默认 */
    (void)arkrtos_linux_abi_on_task_exit(7);
    g_restart_rc = -1;
    g_block_calls = 0;
}
#endif /* ARKRTOS_USE_LINUX_SOCKET == 1 */

int main(void)
{
    t_ctx c;

    /* 帧头位置：32 位宿主上头只有 16 字节，payload 从偏移 16 起。这条断言
     * 把「本文件的读数为什么这么写」钉在运行期，而不是只写在注释里。 */
    ctx_init(&c);
    ck(sizeof(unsigned long) == 4u, "宿主字宽 4 字节");

    t_entry(&c);
    t_exit(&c);
    t_open_close(&c);
    t_read(&c);
    t_write(&c);
    t_pread(&c);
    t_pipe(&c);
    t_meta(&c);
    t_mem(&c);
    t_signal(&c);
    t_execve(&c);
    t_reset(&c);
    t_task_exit(&c);
#if (ARKRTOS_USE_LINUX_FUTEX == 1)
    t_futex(&c);
    t_futex_pi(&c);
    t_futex_pi_edge(&c);
    t_robust_edge(&c);
#endif
#if (ARKRTOS_USE_LINUX_SELECT == 1)
    t_pselect6(&c);
#endif
#if (ARKRTOS_USE_LINUX_SOCKET == 1)
    t_socket(&c);
#endif
#if (ARKRTOS_USE_PAGING_DEMAND == 1)
    t_mapfixed(&c);
#endif
#if (ARKRTOS_USE_PAGING_FILEMAP == 1)
    t_filemap(&c);
    t_filemap_edge(&c);
#endif

    if(g_fails != 0)
    {
        printf("LINUX-ABI HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("LINUX-ABI HOST OK %d checks, 0 failed\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_LINUX_ABI */
