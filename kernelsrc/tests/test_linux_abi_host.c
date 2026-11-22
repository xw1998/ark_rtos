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
#define T_NR_EXIT           (93u)
#define T_NR_EXIT_GROUP     (94u)
#define T_NR_SET_TID        (96u)
#define T_NR_CLOCK_GETTIME  (113u)
#define T_NR_KILL           (129u)
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

#define T_EPERM             (1)
#define T_ENOENT            (2)
#define T_ESRCH             (3)
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

#define T_O_NONBLOCK        (0x800u)
#define T_O_WRONLY          (0x1u)
#define T_O_CREAT           (0x40u)
#define T_O_TRUNC           (0x200u)
#define T_MAP_PRIVATE       (0x02u)
#define T_MAP_FIXED         (0x10u)
#define T_MAP_ANONYMOUS     (0x20u)
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

/* ---------------- 端口桩 ---------------- */

typedef struct
{
    unsigned long arg[4];
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

    return (idx < 4u) ? c->arg[idx] : 0UL;
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

int32 arkrtos_current_task_id = 42;

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
    c->ret = 0xDEADBEEFUL;
    c->ret_written = 0;
    return (int)arkrtos_linux_abi_try(c);
}

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
    ck_ret(c, 42, "getpid");
    (void)call(c, T_NR_GETUID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "getuid");
    (void)call(c, T_NR_GETEUID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "geteuid");

    u_put32(T_DATA_OFF, 0xA5A5A5A5u);
    (void)call(c, T_NR_SET_TID, u_ptr(T_DATA_OFF), 0UL, 0UL, 0UL);
    ck_ret(c, 42, "set_tid_address 回报 tid");
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
    ck_ret(c, -T_ENOSYS, "mmap 缺 MAP_ANONYMOUS 回 -ENOSYS");
    (void)call(c, T_NR_MMAP, 0UL, 0x1000UL, 0UL,
               (unsigned long)(T_MAP_PRIVATE | T_MAP_ANONYMOUS | T_MAP_FIXED));
    ck_ret(c, -T_ENOSYS, "mmap 带 MAP_FIXED 回 -ENOSYS");

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
}

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
    (void)call(c, T_NR_KILL, 42UL, 0UL, 0UL, 0UL);
    ck_ret(c, 0, "kill(sig 0) 是存在性探测");
    (void)call(c, T_NR_KILL, 42UL, 32UL, 0UL, 0UL);
    ck_ret(c, -T_EINVAL, "kill 实时信号号回 -EINVAL");
    (void)call(c, T_NR_TKILL, 43UL, 10UL, 0UL, 0UL);
    ck_ret(c, -T_ESRCH, "tkill 不是自己回 -ESRCH");
    (void)call(c, T_NR_TGKILL, 1UL, 2UL, 10UL, 0UL);
    ck_ret(c, -T_ESRCH, "tgkill tgid != tid 回 -ESRCH");
    (void)call(c, T_NR_TGKILL, 42UL, 42UL, 10UL, 0UL);
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
    (void)call(c, T_NR_KILL, 42UL, 12UL, 0UL, 0UL);

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
    (void)call(c, T_NR_KILL, 42UL, 17UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 42, "默认忽略的信号不改变返回值");
    ck_u(c->reg[T_UREG_ERA], 0u, "默认忽略的信号不进处理函数");

    fresh(c);
    (void)call(c, T_NR_KILL, 42UL, 19UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 42, "默认停止（SIGSTOP）按忽略处理");

    fresh(c);
    g_exit_calls = 0;
    g_exit_code = -1;
    (void)call(c, T_NR_KILL, 42UL, 2UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck(g_exit_calls == 1, "默认终止的信号让任务退出");
    ck(g_exit_code == 130, "退出码 = 128 + 信号号");

    fresh(c);
    ck(sig_install_act(c, 10u, 1UL, 0UL, 0UL) == 0, "SIG_IGN 不需要 restorer");
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_ret(c, 42, "SIG_IGN 的信号被吞掉");
    ck_u(c->reg[T_UREG_ERA], 0u, "SIG_IGN 不改上下文");

    /* 端口报不出帧长 / handler 不可读 / 栈不够：都不投递，pending 留着 */
    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    g_frame_bytes = 0u;      /* 投递点就在 kill 这次调用的返回处，条件必须提前设 */
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "帧长未知时不投递");
    g_frame_bytes = T_FRAME_BYTES;
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], T_HANDLER, "帧长恢复后 pending 仍能投递");

    fresh(c);
    ck(sig_install_act(c, 10u, 0x02000801UL, T_RESTORER, 0UL) == 0,
       "装一个未对齐的 handler（内核不拦，投递时拦）");
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "handler 未对齐时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, 0x200UL, T_RESTORER, 0UL) == 0,
       "装一个窗口外的 handler");
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "handler 读不出指令时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    c->reserve = 0UL;
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    ck_u(c->reg[T_UREG_ERA], 0u, "栈不够时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    g_to_budget = 0;
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
    (void)call(c, T_NR_GETPID, 0UL, 0UL, 0UL, 0UL);
    g_to_budget = -1;
    ck_u(c->reg[T_UREG_ERA], 0u, "帧铺不进用户栈时不投递");

    fresh(c);
    ck(sig_install_act(c, 10u, T_HANDLER, T_RESTORER, 0UL) == 0, "安装处理函数");
    c->set_reg_fail = T_UREG_SP;
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
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
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
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
    (void)call(c, T_NR_KILL, 42UL, 10UL, 0UL, 0UL);
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

    if(g_fails != 0)
    {
        printf("LINUX-ABI HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("LINUX-ABI HOST OK %d checks, 0 failed\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_LINUX_ABI */
