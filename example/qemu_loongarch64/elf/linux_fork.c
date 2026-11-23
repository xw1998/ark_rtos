/**
* @file linux_fork.c
* @brief 按 Linux 系统调用 ABI 写的 fork/clone 夹具（QEMU LoongArch64）
* @details 与 linux_hello.c / linux_execve.c 一样，这个文件没有一行 Ark SDK
*          调用：所有请求走 `syscall 0` + $a7 的 LoongArch Linux 约定。它要
*          证的是 clone(220) / wait4(260) 这一对在**已降权用户任务**上真正
*          把一个进程分成两个：
*
*            1) 返回值分流：父拿到正的子进程号，子拿到 0；两者继续执行在
*               同一条指令上（这就是 clone 的语义，不靠"看起来像"）；
*            2) 子进程号 != 父进程号（两种子里各自 getpid，由门禁跨行比对）；
*            3) 地址空间是**真复制**：子改自己的 .data 全局，父读回原值；
*            4) 退出码经 wait4 传回来：子 exit(42)，父必须读到
*               WIFEXITED 且 WEXITSTATUS == 42（即 status == 42 << 8）；
*            5) 记账是消耗品：子被 reap 之后再 wait4 同一个号回 -ECHILD；
*            6) 越界参数当场拒绝，而不是做一个"看起来像成功"的线程：
*               child_stack != 0 回 -EINVAL，flags 带 CLONE_VM 回 -EINVAL，
*               wait4 认不出的号回 -ECHILD；
*            7) WNOHANG 在子进程还活着时回 0（可能正好被跳过：子进程若在父
*               第一次轮询前就跑完了，这一格只打印不断言，避免假 FAIL）。
*            8) 阻塞 wait4（options = 0）真的会等：子进程先活够 60 ms，
*               父进程在这一行上等，读数里量得到 >= 40 ms 的等待；
*            9) fd 表是**每个进程一份**、管道对象才是共享的：fork 之后子进程
*               关掉两个 fd 不影响父进程的读端，子进程写进管道的数据父进程
*               读得到（引用计数正确，不是「谁来 close 都把管道拆了」）；
*           10) 子进程退出会把 SIGCHLD 投给父进程：父进程装的处理函数在
*               它下一次系统调用返回前被调用，信号号必须是 17。
*
*          另外两条反假绿：
*            - 子进程在退出前写一段自己的串，父不会被它的写"带跑"；
*            - 父进程最后再 wait4 一次，确认记账真的被消耗掉。
*
* @note 不链 libc、不链启动代码，入口仍是 Linux 的 [argc][argv..][NULL]。
* @author xw
*/

#define LX_WRITE            64
#define LX_EXIT             93
#define LX_CLOCK_GETTIME    113
#define LX_GETPID           172
#define LX_CLONE            220
#define LX_WAIT4            260

#define LX_EINVAL           22
#define LX_ECHILD           10
#define LX_ENOSYS           38

#define LX_CLOSE            57
#define LX_PIPE2            59
#define LX_MMAP             222
#define LX_READ             63
#define LX_KILL             129
#define LX_RT_SIGACTION     134
#define LX_RT_SIGRETURN     139

#define SIG_DFL             0ul

#define LX_CLOCK_MONOTONIC  1
#define LX_SIGCHLD          17          /* clone 只接受 0 / SIGCHLD */
#define LX_CLONE_VM         0x00000100ul
#define LX_WNOHANG          1

#define LX_MAP_SHARED       0x01ul
#define LX_MAP_PRIVATE      0x02ul
#define LX_MAP_ANONYMOUS    0x20ul

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

static lx_long lx6(lx_long n, lx_long a0v, lx_long a1v, lx_long a2v,
                  lx_long a3v, lx_long a4v, lx_long a5v)
{
    register lx_long r_a0 __asm__("a0") = a0v;
    register lx_long r_a1 __asm__("a1") = a1v;
    register lx_long r_a2 __asm__("a2") = a2v;
    register lx_long r_a3 __asm__("a3") = a3v;
    register lx_long r_a4 __asm__("a4") = a4v;
    register lx_long r_a5 __asm__("a5") = a5v;
    register lx_long r_a7 __asm__("a7") = n;

    __asm__ volatile("syscall 0"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a5),
                       "r"(r_a7)
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
    while ((u != 0ul) && (i < (int)(sizeof(buf) - 1)))
    {
        buf[i] = (char)('0' + (int)(u % 10ul));
        u /= 10ul;
        i++;
    }
    while (i > 0)
    {
        char one[2];

        i--;
        one[0] = buf[i];
        one[1] = '\0';
        out(one);
    }
}

static int g_fail = 0;

/* 把一条判据先拼成整行再一次写出。
 * 否则 "[fork] " 前缀与正文会被内核日志插在中间，
 * 门禁按行计数就会少一条。 */
static char *chk_str(char *p, const char *s)
{
    while (*s != '\0')
    {
        *p = *s;
        p++;
        s++;
    }
    return p;
}

static char *chk_num(char *p, lx_long v)
{
    char          tmp[24];
    int           i = 0;
    unsigned long u;

    if (v < 0)
    {
        *p = '-';
        p++;
        u = (unsigned long)(-v);
    }
    else
    {
        u = (unsigned long)v;
    }
    if (u == 0ul)
    {
        *p = '0';
        p++;
        return p;
    }
    while ((u != 0ul) && (i < 23))
    {
        tmp[i] = (char)('0' + (int)(u % 10ul));
        u /= 10ul;
        i++;
    }
    while (i > 0)
    {
        i--;
        *p = tmp[i];
        p++;
    }
    return p;
}

static void check(const char *what, lx_long got, lx_long want)
{
    char  buf[200];
    char *p = buf;

    p = chk_str(p, "[fork] ");
    p = chk_str(p, what);
    p = chk_str(p, " -> ");
    p = chk_num(p, got);
    if (got != want)
    {
        p = chk_str(p, " FAIL (want ");
        p = chk_num(p, want);
        p = chk_str(p, ")\n");
        *p = '\0';
        out(buf);
        g_fail++;
        return;
    }
    p = chk_str(p, " ok\n");
    *p = '\0';
    out(buf);
}

/* fork 之后父子各有一份：子进程改它，父进程必须读回 1。
 * 共享同一份地址空间（或只改 trap 帧不复制窗口）的实现会在这里露馅。 */
volatile unsigned long g_fork_marker = 1ul;

#if defined(ARKRTOS_USE_PAGING_COW) && (ARKRTOS_USE_PAGING_COW == 1)
/* 写时复制段用的一对全局：fork 之前两边都是 100，fork 之后父进程把它们
 * 改成 0xBEEF；子进程必须仍读到 100（它那一份是 COW 复制出来的私有页）。
 * 两个变量放在同一个 8 KB 对里的概率很高，所以只要有任何一个被子看到
 * 父的新值，位掩码就丢一位 —— 这正是要抓的「其实共享同一份物理页」。 */
volatile unsigned long g_cow_a = 0ul;
volatile unsigned long g_cow_b = 0ul;
#endif

/* ------------------------------------------------------------------
 * 第二批读取：阻塞 wait4 / 每进程 fd 表 / 跨任务 SIGCHLD
 *
 * 这三条共用一套东西，所以放在一起：父进程 fork 出子进程、子进程在**它
 * 自己那一份** fd 表上 close 并把数据写进共享管道对象，然后死掉；父进程
 * 用阻塞 wait4 等它，再由内核把这个退出翻译成 SIGCHLD 投回父进程。
 * ------------------------------------------------------------------ */

/* Linux 的 rt_sigaction 结构（loongarch64 UAPI）：{handler, flags, restorer, mask}。 */
typedef struct
{
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
} act_t;

static int     s_chld_hits;
static lx_long s_chld_signo;

extern void fork_sig_restorer(void);

static void on_sigchld(unsigned long signo)
{
    s_chld_hits++;
    s_chld_signo = (lx_long)signo;
}

/* 处理函数不能返回调用点：内核把 ra 指向 restorer，restorer 发 rt_sigreturn。 */
__asm__(
    ".section .text.sig_restorer,\"ax\"\n"
    ".globl fork_sig_restorer\n"
    ".type  fork_sig_restorer, @function\n"
    "fork_sig_restorer:\n"
    "  li.w  $a7, 139\n"
    "  syscall 0\n"
    "1:\n"
    "  b     1b\n"
    ".size  fork_sig_restorer, .-fork_sig_restorer\n"
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

static void child_path(void)
{
    lx_long pid = lx1(LX_GETPID, 0);

    out("[fork] child pid=");
    out_num(pid);
    out("\n");

    /* 子进程改自己那份 .data，然后如实报告退出码 42 */
    g_fork_marker = 0x5A5A5A5Aul;
    out("[fork] child wrote its own marker\n");

    (void)lx1(LX_EXIT, 42);
    for (;;)
    {
    }
}

void linux_main(unsigned long sp)
{
    (void)sp;

    out("[fork] parent pid=");
    out_num(lx1(LX_GETPID, 0));
    out("\n");

    /* 反假绿一：两条越界参数必须当场被拒 */
    check("clone(child_stack!=0) == -EINVAL",
          lx3(LX_CLONE, LX_SIGCHLD, 4096, 0), -LX_EINVAL);
    check("clone(flags|CLONE_VM) == -EINVAL",
          lx3(LX_CLONE, LX_SIGCHLD | (lx_long)LX_CLONE_VM, 0, 0), -LX_EINVAL);

    {
        lx_long child = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        lx_long status = -1;
        lx_long rc;
        int     saw_alive = 0;

        if (child == 0)
        {
            child_path();       /* 不返回 */
        }

        out("[fork] parent child=");
        out_num(child);
        out("\n");
        if (child <= 0)
        {
            out("[fork] FAIL: clone returned ");
            out_num(child);
            out(" in the parent\n");
            (void)lx1(LX_EXIT, 1);
            for (;;)
            {
            }
        }
        check("child pid > 0", (child > 0) ? 1 : 0, 1);
        check("child pid != parent pid",
              (child != lx1(LX_GETPID, 0)) ? 1 : 0, 1);

        /* WNOHANG 轮询：子进程还活着时回 0，跑完了就回它的号。
         * 轮询挂一个**墙钟上限**（400 ms），而不是「转 N 圈」：后者在宿主
         * 上量不出意义，在 QEMU 上又可能一整圈都在时间片里转完 —— 于是
         * 子进程一次都没被调度到，夹具却报「wait4 没等到」。上限到了还没
         * 等到就如实判 FAIL（这是读数失败，不是子进程没跑）。 */
        {
            long ts[2];
            long t0 = -1;
            long now = 0;

            if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                    (lx_long)(unsigned long)ts, 0) == 0)
            {
                t0 = (ts[0] * 1000) + (ts[1] / 1000000);
                now = t0;
            }

            for (;;)
            {
                rc = lx4(LX_WAIT4, child, (lx_long)(unsigned long)&status,
                         LX_WNOHANG, 0);
                if (rc == child)
                {
                    break;
                }
                if (rc != 0)
                {
                    break;          /* 出错（-ECHILD/-EIO/...）：交给下面的 check */
                }
                saw_alive = 1;
                if (t0 < 0)
                {
                    break;          /* 时钟拿不到就不死等 */
                }
                if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                        (lx_long)(unsigned long)ts, 0) != 0)
                {
                    break;
                }
                now = (ts[0] * 1000) + (ts[1] / 1000000);
                if ((now - t0) >= 400)
                {
                    break;
                }
            }
            out("[fork] WNOHANG waited ");
            out_num((t0 < 0) ? -1 : (now - t0));
            out(" ms, saw_alive=");
            out_num(saw_alive);
            out("\n");
        }
        check("wait4 returned child pid", rc, child);
        out("[fork] status=");
        out_num(status);
        out("\n");
        check("WEXITSTATUS == 42", (status >> 8) & 0xFF, 42);
        check("WIFEXITED (low 7 bits == 0)", status & 0x7F, 0);

        /* 3) 地址空间是真复制：父的 marker 没被子改掉 */
        check("parent marker intact", (lx_long)g_fork_marker, 1);

        /* 5) 记账是消耗品：同一个号再 wait4 一次必须回 -ECHILD */
        check("wait4(child) again == -ECHILD",
              lx4(LX_WAIT4, child, (lx_long)(unsigned long)&status,
                  LX_WNOHANG, 0), -LX_ECHILD);

        /* 认不出的号必须回 -ECHILD，而不是编一个结果 */
        check("wait4(999) == -ECHILD",
              lx4(LX_WAIT4, 999, (lx_long)(unsigned long)&status,
                  LX_WNOHANG, 0), -LX_ECHILD);
    }

    /* ---------------- 阻塞 wait4 / fd 继承 / 跨任务信号 ---------------- */
    {
        int     pfds[2];        /* Linux pipe2 的 out 参数是 int[2] */
        int     god[2];         /* 放行管道：父先读起点、再放子进程开跑 */
        int     gopipe = 0;
        lx_long child2;
        lx_long rc2;
        lx_long status2;
        char    rbuf[8];
        char    goch;
        long    ts2[2];
        long    b0;
        long    b1;
        int     i;

        check("pipe2 before fork == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)pfds, 0), 0);
        if (lx2(LX_PIPE2, (lx_long)(unsigned long)god, 0) != 0)
        {
            gopipe = 0;
        }
        else
        {
            gopipe = 1;
        }

        child2 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (child2 == 0)
        {
            /* 子进程：先活够 60 ms（让父的阻塞 wait4 真的阻塞），再往管道里
             * 写 3 字节、关掉自己那两个 fd、退出 7。关的必须是**它自己**
             * 那一份 fd 表 —— 旧实现是全局单份，父的读端会被一起关掉。
             *
             * 「活够 60 ms」不能靠调度时序赌：先前子进程一 clone 就自己
             * 开跑，父的起点读数就变成「子已经跑掉一部分」之后的时刻，
             * 偶尔量出 2 ms 这种读数（看起来像 wait4 没阻塞）。
             *
             * 现在改成**轮询放行管道**：本内核的管道是非阻塞的（空管道
             * read 当场 -EAGAIN，见文档 §6），所以「子阻塞在 read 上」这条
             * 前提不成立 —— 单次 read 会立刻返回，子照样先跑掉。必须一直
             * read 到父进程写进来的那个字节真的被取到为止。这样 60 ms 的
             * 计时起点严格在「父读完 b0、写完放行字节」之后，读数是被构造
             * 出来的，不是碰出来的。轮询有上限，取不到就照旧往下走（不会
             * 挂死；真挂死也会在下面的读数里暴露出来）。 */
            long c0 = -1;
            long cnow;
            long gpoll = 0;
            lx_long wrc;

            if (gopipe != 0)
            {
                while (gpoll < 4000000)
                {
                    if (lx3(LX_READ, (lx_long)god[0],
                            (lx_long)(unsigned long)&goch, 1) == 1)
                    {
                        break;
                    }
                    gpoll++;
                }
            }
            if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                    (lx_long)(unsigned long)ts2, 0) == 0)
            {
                c0 = (ts2[0] * 1000) + (ts2[1] / 1000000);
            }
            for (;;)
            {
                if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                        (lx_long)(unsigned long)ts2, 0) != 0)
                {
                    break;
                }
                cnow = (ts2[0] * 1000) + (ts2[1] / 1000000);
                if ((c0 >= 0) && ((cnow - c0) >= 60))
                {
                    break;
                }
            }
            wrc = lx3(LX_WRITE, (lx_long)pfds[1], (lx_long)(unsigned long)"LK!", 3);
            if (wrc == 3)
            {
                out("[fork] child wrote 3 bytes to inherited pipe\n");
            }
            (void)lx1(LX_CLOSE, (lx_long)pfds[0]);
            (void)lx1(LX_CLOSE, (lx_long)pfds[1]);
            (void)lx1(LX_EXIT, 7);
            for (;;)
            {
            }
        }

        check("clone for pipe child > 0", (child2 > 0) ? 1 : 0, 1);

        b0 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts2, 0) == 0)
        {
            b0 = (ts2[0] * 1000) + (ts2[1] / 1000000);
        }

        /* 起点已经读到了，现在才放子进程去睡那 60 ms。 */
        if (gopipe != 0)
        {
            goch = 'g';
            (void)lx3(LX_WRITE, (lx_long)god[1],
                      (lx_long)(unsigned long)&goch, 1);
            (void)lx1(LX_CLOSE, (lx_long)god[0]);
            (void)lx1(LX_CLOSE, (lx_long)god[1]);
        }

        /* 阻塞 wait4：options = 0，没有 WNOHANG —— 父进程必须在这里等。 */
        rc2 = lx4(LX_WAIT4, child2, (lx_long)(unsigned long)&status2, 0, 0);

        b1 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts2, 0) == 0)
        {
            b1 = (ts2[0] * 1000) + (ts2[1] / 1000000);
        }
        out("[fork] blocking wait4 waited ");
        out_num(((b0 < 0) || (b1 < 0)) ? -1 : (b1 - b0));
        out(" ms\n");

        check("blocking wait4 returned child pid", rc2, child2);
        check("child exit code 7", (status2 >> 8) & 0xFF, 7);
        check("blocking wait4 blocked >= 40 ms",
              ((b0 >= 0) && (b1 >= 0) && ((b1 - b0) >= 40)) ? 1 : 0, 1);

        /* 父进程的读端还在（子进程关的是它自己那份），而且子进程写进管道的
         * 数据读得到 —— 同一管道对象、引用计数正确。 */
        for (i = 0; i < 8; i++)
        {
            rbuf[i] = 0;
        }
        rc2 = lx3(LX_READ, (lx_long)pfds[0], (lx_long)(unsigned long)rbuf, 8);
        check("parent read end survived child close", rc2, 3);
        check("child-written pipe data arrived",
              ((rbuf[0] == 'L') && (rbuf[1] == 'K') && (rbuf[2] == '!')) ? 1 : 0, 1);
        (void)lx1(LX_CLOSE, (lx_long)pfds[0]);
        (void)lx1(LX_CLOSE, (lx_long)pfds[1]);
    }

    /* ---------------- 跨任务信号：子进程退出 -> 父进程 SIGCHLD ---------------- */
    {
        act_t   act;
        act_t   old;
        lx_long child3;
        lx_long status3;

        act.handler = (unsigned long)(unsigned long)&on_sigchld;
        act.flags = 0ul;
        act.restorer = (unsigned long)(unsigned long)&fork_sig_restorer;
        act.mask = 0ul;
        old.handler = SIG_DFL;
        old.flags = 0ul;
        old.restorer = 0ul;
        old.mask = 0ul;

        check("rt_sigaction(SIGCHLD) rc",
              lx4(LX_RT_SIGACTION, LX_SIGCHLD,
                  (lx_long)(unsigned long)&act,
                  (lx_long)(unsigned long)&old, 8), 0);

        child3 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (child3 == 0)
        {
            (void)lx1(LX_EXIT, 9);
            for (;;)
            {
            }
        }

        /* 子进程退出时内核给它爹置 SIGCHLD pending：父进程下一次系统调用
         * 返回前必须进处理函数（这里用两次，确保至少落到一次返回上）。 */
        (void)lx1(LX_GETPID, 0);
        status3 = -1;
        (void)lx4(LX_WAIT4, child3, (lx_long)(unsigned long)&status3, 0, 0);
        (void)lx1(LX_GETPID, 0);

        check("SIGCHLD handler ran", (s_chld_hits > 0) ? 1 : 0, 1);
        check("SIGCHLD signo", s_chld_signo, LX_SIGCHLD);
        check("wait4 reaped exit code 9", (status3 >> 8) & 0xFF, 9);
    }


#if defined(ARKRTOS_USE_PAGING_SHM) && (ARKRTOS_USE_PAGING_SHM == 1)
    /* ---------------- MAP_SHARED 匿名页：fork 之后两边看到同一份 ----------------
     * 这是 COW 的镜像问题：私有页「各写各的」，共享页「写的是同一帧」。判据
     * 必须是**双向**的，只测一个方向会被「子进程自己那份」骗过：
     *   1) 父在 fork 之后写的 200，子必须看得到（若共享对被 COW 复制了，
     *      子永远停在 100/0，超时后退出码 bit0=0）；
     *   2) 子看到 200 之后写 300，父必须看得到（证明写真的落回同一帧）。
     * 另加两条反假绿：非匿名 MAP_SHARED 与 PRIVATE|SHARED 同时给，都必须当场
     * -ENOSYS —— 「把 MAP_SHARED 悄悄当私有」是最难查的那种假绿。 */
    {
        volatile unsigned long *shm;
        lx_long shmaddr;
        int     sfds[2];
        int     spipe = 0;

        check("mmap(shared without anon) == -ENOSYS",
              lx6(LX_MMAP, 0, 4096, 3, LX_MAP_SHARED, -1, 0), -LX_ENOSYS);
        check("mmap(private|shared) == -ENOSYS",
              lx6(LX_MMAP, 0, 4096, 3,
                  LX_MAP_PRIVATE | LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0),
              -LX_ENOSYS);

        shmaddr = lx6(LX_MMAP, 0, 4096, 3,
                      LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
        check("mmap(MAP_SHARED|ANON) > 0", (shmaddr > 0) ? 1 : 0, 1);

        if (shmaddr > 0)
        {
            lx_long child5;
            lx_long status5 = -1;
            lx_long rc5 = -1;

            shm = (volatile unsigned long *)(unsigned long)shmaddr;
            check("shared page starts zeroed", (lx_long)(*shm), 0);
            *shm = 100ul;

            /* 用管道把「父先写、子后读」排成确定顺序。原先子进程纯用户自旋
             * 等 4M 次，赌的是「父进程能在子自旋期间拿到 CPU」——在用户态
             * 不抢占的配置下这把赌注会输，读数是 child bits=0，看起来像
             * 「共享失效」，其实是夹具在赌调度。
             *
             * 本内核的管道是非阻塞的（空管道 read 当场 -EAGAIN），所以子进程
             * 不能靠「阻塞在 read 上」定序 —— 要**轮询**到父进程写进来的
             * 那个字节真的读到，再去读共享页。上限只是防挂死。 */
            if (lx2(LX_PIPE2, (lx_long)(unsigned long)sfds, 0) == 0)
            {
                spipe = 1;
            }

            child5 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
            if (child5 == 0)
            {
                int  bits = 0;
                char go = 0;

                if (spipe != 0)
                {
                    long spoll = 0;

                    while (spoll < 4000000)
                    {
                        if (lx3(LX_READ, (lx_long)sfds[0],
                                (lx_long)(unsigned long)&go, 1) == 1)
                        {
                            break;
                        }
                        spoll++;
                    }
                }
                if (*shm == 200ul)
                {
                    bits = 1;
                    *shm = 300ul;
                }
                (void)lx1(LX_EXIT, bits);
                for (;;)
                {
                }
            }

            check("clone for shm child > 0", (child5 > 0) ? 1 : 0, 1);

            /* 父进程 fork 之后写：这一笔必须落到共享帧里。 */
            *shm = 200ul;
            if (spipe != 0)
            {
                char go = 'x';
                (void)lx3(LX_WRITE, (lx_long)sfds[1],
                          (lx_long)(unsigned long)&go, 1);
            }

            rc5 = lx4(LX_WAIT4, child5, (lx_long)(unsigned long)&status5, 0, 0);
            check("shm child exit reaped", rc5, child5);
            check("shm child saw parent post-fork write (bits=1)",
                  (status5 >> 8) & 0xFF, 1);
            /* 子看到 200 之后写的 300 必须回落到同一帧：wait4 返回时子已经
             * 退出，这一读没有竞态。 */
            check("parent saw child shared write", (lx_long)(*shm), 300);
        }
    }
#endif

#if defined(ARKRTOS_USE_PAGING_COW) && (ARKRTOS_USE_PAGING_COW == 1)
    /* ---------------- 写时复制：fork 之后父的写不许被子看见 ---------------- */
    {
        int     cfds[2];
        lx_long child4;
        lx_long rc4;
        lx_long status4;
        char    cch;
        int     spins;

        g_cow_a = 100ul;
        g_cow_b = 100ul;

        check("pipe2 for cow == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)cfds, 0), 0);

        child4 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (child4 == 0)
        {
            /* 子进程：轮询父进程的「已写完」字节（本内核管道空读回 -EAGAIN，
             * 不阻塞），拿到字节后才看两个全局。用位掩码当退出码：
             * bit0 = g_cow_a 仍是 100，bit1 = g_cow_b 仍是 100。 */
            int seen = 0;

            cch = 0;
            for (spins = 0; spins < 200000; spins++)
            {
                rc4 = lx3(LX_READ, (lx_long)cfds[0],
                          (lx_long)(unsigned long)&cch, 1);
                if (rc4 == 1)
                {
                    break;
                }
            }
            if (g_cow_a == 100ul)
            {
                seen |= 1;
            }
            if (g_cow_b == 100ul)
            {
                seen |= 2;
            }
            (void)lx1(LX_CLOSE, (lx_long)cfds[0]);
            (void)lx1(LX_CLOSE, (lx_long)cfds[1]);
            (void)lx1(LX_EXIT, seen);
            for (;;)
            {
            }
        }

        check("clone for cow child > 0", (child4 > 0) ? 1 : 0, 1);

        /* 父进程在 fork 之后写这两个全局，再通知子进程去读。COW 没生效时
         * （父子共享同一份物理页）子会读到 0xBEEF，退出码就丢位。 */
        g_cow_a = 0xBEEFul;
        g_cow_b = 0xBEEFul;
        (void)lx3(LX_WRITE, (lx_long)cfds[1], (lx_long)(unsigned long)"C", 1);

        status4 = -1;
        rc4 = lx4(LX_WAIT4, child4, (lx_long)(unsigned long)&status4, 0, 0);
        check("cow child exit reaped", rc4, child4);
        check("cow child saw pre-fork values (bits=3)", (status4 >> 8) & 0xFF, 3);
        check("parent cow write kept", (lx_long)g_cow_a, 0xBEEF);

        (void)lx1(LX_CLOSE, (lx_long)cfds[0]);
        (void)lx1(LX_CLOSE, (lx_long)cfds[1]);
    }
#endif

    if (g_fail == 0)
    {
        out("[fork] PASS: child ran in its own space, exit 42 reaped\n");
        (void)lx1(LX_EXIT, 0);
    }
    else
    {
        out("[fork] FAIL: ");
        out_num(g_fail);
        out(" checks failed\n");
        (void)lx1(LX_EXIT, 1);
    }
    for (;;)
    {
    }
}
