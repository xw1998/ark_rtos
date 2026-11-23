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

#define LX_CLOCK_MONOTONIC  1
#define LX_SIGCHLD          17          /* clone 只接受 0 / SIGCHLD */
#define LX_CLONE_VM         0x00000100ul
#define LX_WNOHANG          1

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

static void check(const char *what, lx_long got, lx_long want)
{
    out("[fork] ");
    out(what);
    out(" -> ");
    out_num(got);
    if (got != want)
    {
        out(" FAIL (want ");
        out_num(want);
        out(")\n");
        g_fail++;
    }
    else
    {
        out(" ok\n");
    }
}

/* fork 之后父子各有一份：子进程改它，父进程必须读回 1。
 * 共享同一份地址空间（或只改 trap 帧不复制窗口）的实现会在这里露馅。 */
volatile unsigned long g_fork_marker = 1ul;

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