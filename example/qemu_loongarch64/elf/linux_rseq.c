/**
* @file linux_rseq.c
* @brief rseq(293) / getcpu(168) 与 rseq_cs 中止的夹具（QEMU LoongArch64，Linux ABI）
* @details 和 linux_dup.c / linux_signal.c 一样，这个文件没有一行 Ark SDK 调用：
*          全部走 `syscall 0` + `$a7` 的 LoongArch Linux 约定。它要证的是
*          「注册成功后，临界区真的会被打断」——而不是只证「注册号回 0」。
*
*            1) getcpu(168) 的两个出参都写成 0（单核），且不是没写；
*            2) rseq(293) 注册成功，cpu_id_start / cpu_id 被内核写成 0
*               （进之前先填 0xDEADBEEF，证明是内核真写了）；
*            3) **信号投递点中止**：临界区里就地发 tgkill，被中断的 PC 落在
*               [start_ip, start_ip + post_commit_offset) 里，于是信号帧里存
*               的是 abort_ip；handler 跑完之后 sigreturn 从 abort 路径继续，
*               提交段一次也不执行，rseq_cs 已被内核清零；
*            4) 同一套结构在**没有信号**时正常提交：提交段执行、不被误中止；
*            5) **tick 抢占点中止**：另造一个同优先级的兄弟线程一起忙等，
*               时间片一到必然互切；主线程切走时 PC 在 [start, start+post)
*               里，于是它的 PC 被改到 abort_ip、rseq_cs 被清零，而兄弟线程
*               自己没注册 rseq，切走时什么也不该发生（反向对照）；
*            6) 注册契约：len != 32 / flags != 0 各回 -EINVAL；NULL 注销成功。
*
* @note 临界区里的 tgkill 是**就地内联**发出的，不是经函数调用：经过 helper
*       的话，被打断的 PC 会落在 helper 的函数体里，数值上不在
*       [start_ip, start_ip + post) 区间里——那正好是「本夹具要验的东西被自己
*       绕过去」。内联之后，syscall 的下一条指令就在区间内。
* @note 输出走 fd 2；不链 libc、不链启动代码，入口仍是 Linux 的
*       [argc][argv..][NULL]。
* @author xw
*/

#define LX_GETPID           172
#define LX_GETCPU           168
#define LX_RSEQ             293
#define LX_RT_SIGACTION     134
#define LX_KILL             129
#define LX_TGKILL           131
#define LX_WRITE            64
#define LX_EXIT             93
#define LX_CLONE            220
#define LX_FUTEX            98
#define LX_CLOCK_GETTIME    113
#define LX_CLOCK_MONOTONIC  1
#define LX_FUTEX_WAIT       0
#define LX_FUTEX_WAKE       1
#define LX_EAGAIN           11
#define LX_CLONE_VM         0x00000100ul
#define LX_CLONE_FS         0x00000200ul
#define LX_CLONE_FILES      0x00000400ul
#define LX_CLONE_SIGHAND    0x00000800ul
#define LX_CLONE_THREAD     0x00010000ul
#define LX_CLONE_PARENT_SETTID  0x00100000ul
#define LX_CLONE_CHILD_CLEARTID 0x00200000ul
#define LX_CLONE_CHILD_SETTID   0x01000000ul

#define LX_SIGUSR1          10
#define LX_EINVAL           22
#define LX_SIGSET_BYTES     8

typedef long lx_long;

/* ------------------------------------------------------------------
 * Linux 系统调用（LoongArch ABI：syscall 0 + $a7）
 * ------------------------------------------------------------------ */
static lx_long lx1(lx_long n, lx_long a0v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0" : "+r"(r_a0) : "r"(r_a7) : "memory");
    return r_a0;
}

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

static lx_long lx4(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v,
                   lx_long a3v)
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
 * 极小运行时（没有 libc）；输出走 fd 2
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
        (void)lx3(LX_WRITE, 2, (lx_long)(unsigned long)s, (lx_long)n);
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
    out("[rseq] ");
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
 * 处理函数与 restorer（与 linux_signal.c 同一套 ABI）
 * ------------------------------------------------------------------ */
typedef struct
{
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
} act_t;

/* struct rseq（Linux UAPI，32 字节）：cpu_id_start / cpu_id / rseq_cs + 填充。
 * 必须 32 字节对齐（内核按契约拒绝对不齐的地址）。 */
struct rseq_area
{
    unsigned int       cpu_id_start;
    unsigned int       cpu_id;
    unsigned long long rseq_cs;
    unsigned char      pad[16];
} __attribute__((aligned(32)));

/* struct rseq_cs（Linux UAPI，32 字节）。 */
struct rseq_cs_blk
{
    unsigned int       version;
    unsigned int       flags;
    unsigned long long start_ip;
    unsigned long long post_commit_offset;
    unsigned long long abort_ip;
};

static struct rseq_area    s_rseq;
static struct rseq_cs_blk  s_cs;
/* 编译器眼里的「可达性入口」：本工具链（LoongIDE GCC 8.3，LoongArch）
 * 对 &&label 的处理不可靠——中止块在 CFG 里不可达时会被整块删掉，
 * 连带把 &&label 的立即数算成邻近块的首址。用一个恒 0 的 volatile
 * 分支给中止块一条真边，块留下来、标签地址也就对了。运行时这条分支
 * 一次也不走（值恒 0）。 */
static volatile int s_zero_guard;
static int                 s_hits;
static int                 s_aborted;
static int                 s_post;
static int                 s_normal;
static int                 s_pre_aborted;
static lx_long             s_self;

static void on_signal(unsigned long signo)
{
    (void)signo;
    s_hits++;
}

extern void sig_restorer(void);

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
 * tick 抢占的中止点用的兄弟线程
 *
 * 与 linux_fork.c 的 RUNNING 抢占回归夹具同一套机械：**两个同优先级任务
 * 一起忙等**，时间片（10 拍 = 5 ms）一到必然互切。只有主线程在临界区里，
 * 于是切走时它的用户 PC 落在 [start, start+post) 之间，内核必须在真正切栈
 * 之前把它改到 abort_ip 并清 rseq_cs。
 *
 * 兄弟线程自己不注册 rseq（表里它那一格是 0），切走它时中止逻辑什么也不做
 * ——这正好是「只对注册过的任务动手」的反向对照。
 * ------------------------------------------------------------------ */
#if (ARKRTOS_USE_LINUX_THREAD == 1)
static volatile lx_long   g_pre_ran;
static volatile lx_long   g_pre_ctid;
static unsigned char      s_pre_stk[16384] __attribute__((aligned(16)));

/* 两段忙等的轮数：兄弟线程必须活得比主线程那一节长，否则它先退出、
 * 只剩主线程一个人跑，时间片轮不起来，tick 永远不来切主线程 ——
 * 那就成了「夹具自己把被测路径饿死」。主线程那一节要跨过至少两个
 * 时间片（10 拍 = 5 ms），所以给到最保守的下面这组值。 */
#define PRE_HELPER_SPIN  20000000L
#define PRE_MAIN_SPIN     6000000L

static void preempt_body(void)
{
    volatile unsigned long spin = 0ul;
    volatile lx_long       i;

    /* 纯用户态循环，一次系统调用都不发：整段都在 RUNNING 状态。 */
    for (i = 0; i < PRE_HELPER_SPIN; i++)
    {
        spin = spin + (unsigned long)i;
    }
    g_pre_ran = (lx_long)spin;
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

/* 旧式 clone(2) 的子线程从系统调用后面那条指令继续跑：syscall / 分支 / 跳转
 * 必须挤在同一个 asm 块里（照 musl __clone 的写法）。 */
static lx_long preempt_spawn(void)
{
    unsigned int ptid = 0xFFFFFFFFu;
    register lx_long r_a0 __asm__("a0") =
        (lx_long)(LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES |
                  LX_CLONE_SIGHAND | LX_CLONE_THREAD |
                  LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_SETTID |
                  LX_CLONE_CHILD_CLEARTID);
    register lx_long r_a1 __asm__("a1") =
        (lx_long)(unsigned long)(s_pre_stk + sizeof(s_pre_stk));
    register lx_long r_a2 __asm__("a2") =
        (lx_long)(unsigned long)&ptid;
    register lx_long r_a3 __asm__("a3") =
        (lx_long)(unsigned long)&g_pre_ctid;
    register lx_long r_a4 __asm__("a4") = 0;
    register lx_long r_a7 __asm__("a7") = LX_CLONE;

    __asm__ volatile("syscall 0\n\t"
                     "bnez $a0, 1f\n\t"
                     "jirl $ra, %[body], 0\n\t"
                     "1:\n"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4),
                       "r"(r_a7),
                       [body] "r"((lx_long)(unsigned long)preempt_body)
                     : "memory");
    return r_a0;
}

/* 按 pthread_join 的真实路径等 CLEARTID 被清成 0（绝对时刻上限 500 ms）。 */
static void preempt_join(void)
{
    long    ts[2];
    lx_long dur[2];
    int     spun;

    dur[0] = 0;
    dur[1] = 0;
    if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
            (lx_long)(unsigned long)ts, 0) == 0)
    {
        dur[0] = ts[0];
        dur[1] = ts[1] + 500000000;
        if (dur[1] >= 1000000000)
        {
            dur[0] += 1;
            dur[1] -= 1000000000;
        }
    }
    for (spun = 0; spun < 64; spun++)
    {
        lx_long wrc;

        if (g_pre_ctid == 0)
        {
            break;
        }
        wrc = lx4(LX_FUTEX, (lx_long)(unsigned long)&g_pre_ctid, LX_FUTEX_WAIT,
                  g_pre_ctid, (lx_long)(unsigned long)dur);
        if ((wrc == 0) || (wrc != -LX_EAGAIN))
        {
            break;
        }
    }
}
#endif /* ARKRTOS_USE_LINUX_THREAD */

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
    unsigned int  cpu = 0xDEADBEEFu;
    unsigned int  node = 0xDEADBEEFu;
    lx_long rc;

    (void)sp;

    s_self = lx1(LX_GETPID, 0);

    /* ---------------- 1：getcpu 的两个出参 ---------------- */
    rc = lx3(LX_GETCPU, (lx_long)(unsigned long)&cpu,
             (lx_long)(unsigned long)&node, 0);
    check("getcpu rc", rc, 0);
    check("getcpu cpu", (lx_long)cpu, 0);
    check("getcpu node", (lx_long)node, 0);

    /* ---------------- 2：注册 rseq ---------------- */
    s_rseq.cpu_id_start = 0xDEADBEEFu;
    s_rseq.cpu_id       = 0xDEADBEEFu;
    s_rseq.rseq_cs      = 0ul;
    rc = lx4(LX_RSEQ, (lx_long)(unsigned long)&s_rseq, 32, 0, 0);
    check("rseq register rc", rc, 0);
    check("cpu_id_start written 0", (lx_long)s_rseq.cpu_id_start, 0);
    check("cpu_id readback 0", (lx_long)s_rseq.cpu_id, 0);

    /* ---------------- 3：装处理函数 ---------------- */
    act.handler  = (unsigned long)(unsigned long)&on_signal;
    act.flags    = 0ul;
    act.restorer = (unsigned long)(unsigned long)&sig_restorer;
    act.mask     = 0ul;
    (void)lx4(LX_RT_SIGACTION, LX_SIGUSR1, (lx_long)(unsigned long)&act, 0,
              LX_SIGSET_BYTES);

    /* ---------------- 4：临界区里发信号 -> 必须中止 ---------------- */
    s_aborted = 0;
    s_post    = 0;
    if (s_zero_guard) { goto cs_abort; }
    s_cs.version            = 0u;
    s_cs.flags              = 0u;
    s_cs.start_ip           = (unsigned long long)(unsigned long)&&cs_start;
    s_cs.post_commit_offset = (unsigned long long)
                              ((unsigned long)&&cs_end -
                               (unsigned long)&&cs_start);
    s_cs.abort_ip           = (unsigned long long)(unsigned long)&&cs_abort;
    s_rseq.rseq_cs = (unsigned long long)(unsigned long)&s_cs;
    __asm__ volatile("" ::: "memory");

cs_start:
    /* 就地发 tgkill：syscall 的下一条指令仍在本区间内，于是内核在信号
     * 投递点上看到的 PC 落在 [start, start+post) 里。 */
    {
        register lx_long a0 __asm__("a0") = s_self;
        register lx_long a1 __asm__("a1") = s_self;
        register lx_long a2 __asm__("a2") = LX_SIGUSR1;
        register lx_long a7 __asm__("a7") = LX_TGKILL;

        __asm__ volatile("syscall 0"
                         : "+r"(a0)
                         : "r"(a1), "r"(a2), "r"(a7)
                         : "memory");
    }
    /* 一条真指令把「syscall 的下一条」钉在区间内部。没有它的话，cs_end 可能
     * 正好落在 syscall 的返回地址上——区间右端是开区间，PC 就会落在外面，
     * 中止永远不会发生，而夹具还会误以为验过了。 */
    __asm__ volatile("nop");
cs_end:
    s_post = 1;
    s_rseq.rseq_cs = 0ul;
    __asm__ volatile("" ::: "memory");
    goto cs_after;
cs_abort:
    s_aborted = 1;
cs_after:
    check("signal aborted the critical section", s_aborted, 1);
    check("post-commit did not run", s_post, 0);
    check("rseq_cs cleared by the abort", (lx_long)s_rseq.rseq_cs, 0);
    check("handler ran once", s_hits, 1);

    /* ---------------- 5：没有信号的临界区必须正常提交 ---------------- */
    s_normal = 0;
    if (s_zero_guard) { goto ok_abort; }
    s_cs.version            = 0u;
    s_cs.flags              = 0u;
    s_cs.start_ip           = (unsigned long long)(unsigned long)&&ok_start;
    s_cs.post_commit_offset = (unsigned long long)
                              ((unsigned long)&&ok_end -
                               (unsigned long)&&ok_start);
    s_cs.abort_ip           = (unsigned long long)(unsigned long)&&ok_abort;
    s_rseq.rseq_cs = (unsigned long long)(unsigned long)&s_cs;
    __asm__ volatile("" ::: "memory");

ok_start:
    (void)lx1(LX_GETPID, 0);
    __asm__ volatile("nop");
ok_end:
    s_normal = 1;
    __asm__ volatile("" ::: "memory");
    s_rseq.rseq_cs = 0ul;
    __asm__ volatile("" ::: "memory");
    goto ok_after;
ok_abort:
    s_normal = 2;
ok_after:
    check("normal critical section completed", s_normal, 1);
    check("normal section not falsely aborted", s_aborted, 1);
    check("rseq_cs cleared by the commit", (lx_long)s_rseq.rseq_cs, 0);

    /* ---------------- 6：tick 抢占点上的中止 ----------------
     * 这一节需要 CLONE_THREAD（兄弟线程）。没开线程的构建（如 linux_abi
     * 门禁的默认档）整段不编，check 条数仍是 16；线程档（fork 门禁的
     * 第九/十档）编进来后是 20 条。 */
#if (ARKRTOS_USE_LINUX_THREAD == 1)
    s_pre_aborted = 0;
    g_pre_ran     = 0;
    /* 哨兵：CLEARTID 只在退出时把 *ctid 清 0，创建时不写；不带
     * CHILD_SETTID 的话它一直就是 0，join 第一眼就以为已经结束了。
     * 照 linux_fork.c 的 pre_join 写法：先放一个不可能的值，再等它变 0。 */
    g_pre_ctid    = 0x5eed1234l;
    { lx_long sc = preempt_spawn();
      check("clone for preempt helper rc", (sc > 0) ? 1 : sc, 1); }
    if (s_zero_guard) { goto pre_abort; }
    s_cs.version            = 0u;
    s_cs.flags              = 0u;
    s_cs.start_ip           = (unsigned long long)(unsigned long)&&pre_start;
    s_cs.post_commit_offset = (unsigned long long)
                              ((unsigned long)&&pre_end -
                               (unsigned long)&&pre_start);
    s_cs.abort_ip           = (unsigned long long)(unsigned long)&&pre_abort;
    s_rseq.rseq_cs = (unsigned long long)(unsigned long)&s_cs;
    __asm__ volatile("" ::: "memory");

pre_start:
    /* 纯用户态忙等：这一节里一次系统调用都不发。发了就把中止点换成
     * 「信号投递」或「SVC 返回」，那不再是 tick 的账。兄弟线程同优先级
     * 一起忙等，时间片一到必然互切，切走时 PC 就在这里。 */
    {
        volatile unsigned long spin = 0ul;
        volatile lx_long       i;

        for (i = 0; i < PRE_MAIN_SPIN; i++)
        {
            spin = spin + (unsigned long)i;
        }
        (void)spin;
    }
    __asm__ volatile("nop");
pre_end:
    s_rseq.rseq_cs = 0ul;
    __asm__ volatile("" ::: "memory");
    goto pre_after;
pre_abort:
    s_pre_aborted = 1;
pre_after:
    check("tick preemption aborted the critical section", s_pre_aborted, 1);
    check("tick abort cleared rseq_cs", (lx_long)s_rseq.rseq_cs, 0);
    preempt_join();
    check("preempt helper ran to completion", (g_pre_ran != 0) ? 1 : 0, 1);
#endif /* ARKRTOS_USE_LINUX_THREAD */

    /* ---------------- 7：注册契约的拒绝面与注销 ---------------- */
    rc = lx4(LX_RSEQ, (lx_long)(unsigned long)&s_rseq, 16, 0, 0);
    check("rseq len != 32 -> -EINVAL", rc, -LX_EINVAL);
    rc = lx4(LX_RSEQ, (lx_long)(unsigned long)&s_rseq, 32, 1, 0);
    check("rseq flags != 0 -> -EINVAL", rc, -LX_EINVAL);
    rc = lx4(LX_RSEQ, 0, 32, 0, 0);
    check("rseq(NULL) deregisters", rc, 0);

    if (s_fail == 0)
    {
        out("[rseq] PASS: getcpu both outputs zero, rseq registered with "
            "cpu_id written, signal delivery aborted the critical section at "
            "abort_ip with rseq_cs cleared, ");
#if (ARKRTOS_USE_LINUX_THREAD == 1)
        out("tick preemption aborted the critical section too, ");
#endif
        out("commit path completed, invalid contract refused, NULL "
            "deregisters\n");
        (void)lx1(LX_EXIT, 57);
    }
    else
    {
        out("[rseq] FAIL: ");
        out_num((lx_long)s_fail);
        out(" checks failed\n");
        (void)lx1(LX_EXIT, 1);
    }
    for (;;)
    {
    }
}
