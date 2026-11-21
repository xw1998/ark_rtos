/**
* @file linux_signal.c
* @brief 信号夹具（QEMU LoongArch64，Linux ABI）
* @details 和 linux_pipe.c 一样，这个文件没有一行 Ark SDK 调用：全部走
*          `syscall 0` + `$a7` 的 LoongArch Linux 约定。它要证的是本内核
*          信号支持**最小一档**的每一处行为，每一条都必须是可证伪的读数：
*
*            1) rt_sigaction 装得上处理函数，旧的动作为 SIG_DFL；
*            2) kill 自己后，处理函数在**系统调用返回前**被调用一次，
*               第一个参数是信号号；
*            3) 处理函数返回后 sigreturn 把 a0 装回被中断的系统调用的返回值
*               （kill 的返回值 0，而不是信号号 10）；
*            4) 处理函数里再投递另一个信号可以嵌套（深度 2）；
*            5) rt_sigprocmask 屏蔽期间不投递，查得回来、解除后立即投递；
*            6) 非法参数逐条拒绝：sigsetsize!=8、SIGKILL、缺 restorer、
*               别的 pid、越界信号号、tgkill 的 tgid!=tid；
*            7) SIG_IGN 真的不投递（计数一点都不涨）；
*            8) 还原成 SIG_DFL 之后 kill 自己，默认动作结束任务，退出码
*               128 + 10 = 138。
*
* @note 不链 libc、不链启动代码，入口仍是 Linux 的 [argc][argv..][NULL]。
* @author xw
*/

#define LX_KILL             129
#define LX_TGKILL           131
#define LX_RT_SIGACTION     134
#define LX_RT_SIGPROCMASK   135
#define LX_RT_SIGRETURN     139
#define LX_WRITE            64
#define LX_EXIT             93
#define LX_GETPID           172

#define LX_EINVAL           22
#define LX_ESRCH            3

#define LX_SIG_BLOCK        0
#define LX_SIG_UNBLOCK      1

#define SIGKILL             9
#define SIGUSR1             10
#define SIGUSR2             12
#define SIGTERM             15

#define SIG_DFL             0ul
#define SIG_IGN             1ul

#define SIGSET_BYTES        8

typedef long lx_long;

/* ------------------------------------------------------------------
 * Linux 系统调用（LoongArch ABI：syscall 0 + $a7）
 * ------------------------------------------------------------------ */
static lx_long lx2(lx_long n, lx_long a0v, lx_long a1v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a1 __asm__("a1") = a1v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a7)
                     : "memory");
    return r_a0;
}

static lx_long lx3(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a1 __asm__("a1") = a1v;
    register lx_long r_a2 __asm__("a2") = a2v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a7)
                     : "memory");
    return r_a0;
}

static lx_long lx4(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v, lx_long a3v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a1 __asm__("a1") = a1v;
    register lx_long r_a2 __asm__("a2") = a2v;
    register lx_long r_a3 __asm__("a3") = a3v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a7)
                     : "memory");
    return r_a0;
}

/* ------------------------------------------------------------------
 * 极小运行时（没有 libc）
 * ------------------------------------------------------------------ */
static int s_fail;

static unsigned long s_len(const char *s)
{
    unsigned long n = 0ul;

    while (s[n] != '\0')
    {
        n++;
    }
    return n;
}

static void out(const char *s)
{
    unsigned long n = s_len(s);

    if (n != 0ul)
    {
        (void)lx3(LX_WRITE, 1, (lx_long)(unsigned long)s, (lx_long)n);
    }
}

static void out_num(lx_long v)
{
    char buf[24];
    int  i = 0;
    unsigned long u;

    if (v < 0)
    {
        out("-");
        u = (unsigned long)(-v);
    }
    else
    {
        u = (unsigned long)v;
    }
    if (u == 0ul)
    {
        out("0");
        return;
    }
    while ((u != 0ul) && (i < (int)sizeof(buf)))
    {
        buf[i] = (char)('0' + (int)(u % 10ul));
        u /= 10ul;
        i++;
    }
    while (i > 0)
    {
        char c[2];

        i--;
        c[0] = buf[i];
        c[1] = '\0';
        out(c);
    }
}

static void check(const char *name, lx_long got, lx_long want)
{
    out("[sig] ");
    out(name);
    out(" = ");
    out_num(got);
    if (got != want)
    {
        out(" FAIL (want ");
        out_num(want);
        out(")\n");
        s_fail++;
    }
    else
    {
        out(" ok\n");
    }
}

/* ------------------------------------------------------------------
 * 处理函数与 restorer
 * ------------------------------------------------------------------ */

/* Linux 的 rt_sigaction 结构（loongarch64 UAPI）：32 字节
 * { handler, flags, restorer, mask(8 B) }。内核按 4 个 8 字节字读。 */
typedef struct
{
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
} act_t;

static int      s_hits1;
static int      s_hits2;
static int      s_depth;
static int      s_max_depth;
static lx_long  s_last1;
static lx_long  s_last2;
static lx_long  s_nested_rc;
static lx_long  s_self;

extern void sig_restorer(void);

/* 处理函数不能返回调用点：内核把 ra 指向 restorer，restorer 里发
 * rt_sigreturn（139）把现场装回去。handler 进入时 sp 已经被指到帧下面，
 * 它压栈只会继续往下长，不会踩到内核铺好的信号帧（头 32 B + 整份
 * trap 帧，帧长由端口报，不是固定的 64 字节）。 */
static void on_signal(unsigned long signo)
{
    s_hits1++;
    s_last1 = (lx_long)signo;
    s_depth++;
    if (s_depth > s_max_depth)
    {
        s_max_depth = s_depth;
    }

    if ((signo == SIGUSR1) && (s_depth == 1))
    {
        /* 嵌套：在这次 kill 的系统调用返回前会进第二个处理函数。 */
        s_nested_rc = lx2(LX_KILL, s_self, SIGUSR2);
    }
    s_depth--;
}

static void on_signal2(unsigned long signo)
{
    s_hits2++;
    s_last2 = (lx_long)signo;
    s_depth++;
    if (s_depth > s_max_depth)
    {
        s_max_depth = s_depth;
    }
    s_depth--;
}

__asm__(
    ".section .text.sig_restorer,\"ax\"\n"
    ".globl sig_restorer\n"
    ".type  sig_restorer, @function\n"
    "sig_restorer:\n"
    "  li.w  $a7, 139\n"
    "  syscall 0\n"
    "1:\n"
    "  b     1b\n"
    ".size  sig_restorer, .-sig_restorer\n"
    ".previous\n"
);

/* ------------------------------------------------------------------
 * 入口
 * ------------------------------------------------------------------ */
void linux_main(unsigned long sp);

__asm__(
    ".section .text._start,\"ax\"\n"
    ".globl _start\n"
    ".type  _start, @function\n"
    "_start:\n"
    "  move $a0, $sp\n"
    "  bl   linux_main\n"
    "1:\n"
    "  b    1b\n"
    ".size  _start, .-_start\n"
    ".previous\n"
);

void linux_main(unsigned long sp)
{
    act_t   act;
    act_t   old;
    unsigned long mask;
    lx_long rc;

    (void)sp;

    s_self = lx2(LX_GETPID, 0, 0);
    check("getpid", s_self > 0 ? 1 : 0, 1);

    /* ---------------- 1：装处理函数 + 旧动作 ---------------- */
    act.handler  = (unsigned long)(unsigned long)&on_signal;
    act.flags    = 0ul;
    act.restorer = (unsigned long)(unsigned long)&sig_restorer;
    act.mask     = 0ul;
    old.handler  = 0xDEADul;    /* 不先清零：内核必须自己写满 4 个字 */
    old.flags    = 0xDEADul;
    old.restorer = 0xDEADul;
    old.mask     = 0xDEADul;
    rc = lx4(LX_RT_SIGACTION, SIGUSR1, (lx_long)(unsigned long)&act,
             (lx_long)(unsigned long)&old, SIGSET_BYTES);
    check("rt_sigaction rc", rc, 0);
    check("old handler was SIG_DFL", (lx_long)old.handler, (lx_long)SIG_DFL);

    act.handler = (unsigned long)(unsigned long)&on_signal2;
    rc = lx4(LX_RT_SIGACTION, SIGUSR2, (lx_long)(unsigned long)&act, 0,
             SIGSET_BYTES);
    check("rt_sigaction SIGUSR2 rc", rc, 0);

    /* ---------------- 2/3：投递 + 返回值跨处理函数保留 ---------------- */
    rc = lx2(LX_KILL, s_self, SIGUSR1);
    check("kill rc survives handler", rc, 0);
    check("handler ran once", (lx_long)s_hits1, 1);
    check("handler signo", s_last1, SIGUSR1);

    /* ---------------- 4：嵌套 ---------------- */
    check("nested handler ran", (lx_long)s_hits2, 1);
    check("nested signo", (lx_long)s_last2, SIGUSR2);
    check("max handler depth", (lx_long)s_max_depth, 2);
    check("nested kill rc", s_nested_rc, 0);

    /* ---------------- 5：屏蔽 / 查询 / 解除 ---------------- */
    mask = 1ul << (SIGUSR2 - 1);
    rc = lx4(LX_RT_SIGPROCMASK, LX_SIG_BLOCK, (lx_long)(unsigned long)&mask,
             (lx_long)(unsigned long)&old.mask, SIGSET_BYTES);
    check("rt_sigprocmask block rc", rc, 0);

    rc = lx2(LX_KILL, s_self, SIGUSR2);
    check("kill while blocked rc", rc, 0);
    check("blocked signal not delivered", (lx_long)s_hits2, 1);

    old.mask = 0ul;
    rc = lx4(LX_RT_SIGPROCMASK, LX_SIG_BLOCK, 0,
             (lx_long)(unsigned long)&old.mask, SIGSET_BYTES);
    check("rt_sigprocmask query rc", rc, 0);
    check("query sees blocked bit", (lx_long)((old.mask >> (SIGUSR2 - 1)) & 1ul), 1);

    rc = lx4(LX_RT_SIGPROCMASK, LX_SIG_UNBLOCK, (lx_long)(unsigned long)&mask,
             0, SIGSET_BYTES);
    check("rt_sigprocmask unblock rc", rc, 0);
    check("delivered on unblock", (lx_long)s_hits2, 2);

    /* ---------------- 6：非法参数 ---------------- */
    rc = lx4(LX_RT_SIGACTION, SIGUSR1, 0, 0, 4);
    check("sigsetsize != 8 -> -EINVAL", rc, -LX_EINVAL);

    rc = lx4(LX_RT_SIGACTION, SIGKILL, (lx_long)(unsigned long)&act, 0,
             SIGSET_BYTES);
    check("SIGKILL -> -EINVAL", rc, -LX_EINVAL);

    act.handler  = (unsigned long)(unsigned long)&on_signal;
    act.restorer = 0ul;         /* 没有 restorer */
    rc = lx4(LX_RT_SIGACTION, SIGTERM, (lx_long)(unsigned long)&act, 0,
             SIGSET_BYTES);
    check("no restorer -> -EINVAL", rc, -LX_EINVAL);

    old.handler = 0xDEADul;
    rc = lx4(LX_RT_SIGACTION, SIGTERM, 0, (lx_long)(unsigned long)&old,
             SIGSET_BYTES);
    check("rejected action not installed", (lx_long)old.handler, (lx_long)SIG_DFL);

    rc = lx2(LX_KILL, s_self + 1, SIGUSR1);
    check("kill other pid -> -ESRCH", rc, -LX_ESRCH);

    rc = lx2(LX_KILL, s_self, 99);
    check("signal 99 -> -EINVAL", rc, -LX_EINVAL);

    rc = lx3(LX_TGKILL, s_self, s_self + 1, SIGUSR1);
    check("tgkill tgid!=tid -> -ESRCH", rc, -LX_ESRCH);

    /* ---------------- 7：SIG_IGN 真的不投递 ---------------- */
    act.handler  = (unsigned long)SIG_IGN;
    act.restorer = 0ul;
    rc = lx4(LX_RT_SIGACTION, SIGTERM, (lx_long)(unsigned long)&act, 0,
             SIGSET_BYTES);
    check("install SIG_IGN rc", rc, 0);
    rc = lx2(LX_KILL, s_self, SIGTERM);
    check("kill ignored signal rc", rc, 0);
    check("ignored signal not delivered", (lx_long)(s_hits1 + s_hits2), 3);

    /* ---------------- 8：SIG_DFL 的默认动作 ---------------- */
    act.handler  = (unsigned long)SIG_DFL;
    act.restorer = 0ul;
    rc = lx4(LX_RT_SIGACTION, SIGUSR1, (lx_long)(unsigned long)&act, 0,
             SIGSET_BYTES);
    check("restore SIG_DFL rc", rc, 0);

    if (s_fail == 0)
    {
        out("[sig] PASS: sigaction installed, handler ran before syscall return, "
            "a0 restored from sigreturn, nesting depth 2, block/query/unblock, "
            "EINVAL/ESRCH rejects, SIG_IGN silent, SIG_DFL exits with 128+signo\n");
        (void)lx2(LX_KILL, s_self, SIGUSR1);   /* 默认动作：结束任务，退出码 138 */
    }
    else
    {
        out("[sig] FAIL: ");
        out_num((lx_long)s_fail);
        out(" checks failed\n");
        (void)lx2(LX_EXIT, 1, 0);
    }
    for (;;)
    {
    }
}