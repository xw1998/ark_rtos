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
*               它下一次系统调用返回前被调用，信号号必须是 17；
*           11) futex(98) 的 WAIT / WAKE 跨进程真的能配对：父进程在 WAIT 里
*               睡着（这是本内核唯一的真阻塞点之一），子进程改共享页上的那
*               个字再 WAKE，父进程必须带着 0 醒来并看到新值；futex 的键是
*               「物理帧 + 页内偏移」，所以子进程必须叫醒的是**同一个字**，
*               不是同一个虚拟地址上的另一份拷贝。两边都用「重试到收敛」的
*               写法（用户态可被时间片抢占），不拿调度顺序当判据。
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
#define LX_EXIT_GROUP       94
#define LX_GETTID           178     /* asm-generic 表：LA64 的 gettid 是 178 */
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
#define LX_CLONE_FS         0x00000200ul
#define LX_CLONE_FILES      0x00000400ul
#define LX_CLONE_SIGHAND    0x00000800ul
#define LX_CLONE_VFORK      0x00004000ul
#define LX_CLONE_THREAD     0x00010000ul
#define LX_CLONE_SETTLS     0x00080000ul
#define LX_CLONE_PARENT_SETTID  0x00100000ul
#define LX_CLONE_CHILD_CLEARTID 0x00200000ul
#define LX_CLONE_CHILD_SETTID   0x01000000ul
#define LX_WNOHANG          1

#define LX_MAP_SHARED       0x01ul
#define LX_MAP_PRIVATE      0x02ul
#define LX_MAP_ANONYMOUS    0x20ul
#define LX_MAP_FIXED        0x10ul

#define LX_FUTEX            98
#define LX_FUTEX_WAIT       0
#define LX_FUTEX_WAKE       1
#define LX_FUTEX_REQUEUE    3
#define LX_FUTEX_CMP_REQUEUE 4
#define LX_FUTEX_WAIT_BITSET 9
#define LX_FUTEX_WAKE_BITSET 10
#define LX_EAGAIN           11
#define LX_EINTR            4
#define LX_ETIMEDOUT        110

/* 文件映射（U-21）这一档要用的号：openat / mmap 已经有了 mmap 222，
 * 另外两个是 munmap 215 与 msync 227。 */
#define LX_MPROTECT         226
#define LX_OPENAT           56
#define LX_MUNMAP           215
#define LX_MSYNC            227

/* pselect6：跨 fd 就绪等待（U-22） */
#define LX_PSELECT6         72

/* socket 家族（U-23，第八档） */
#define LX_SOCKET           198
#define LX_BIND             200
#define LX_LISTEN           201
#define LX_ACCEPT           202
#define LX_CONNECT          203
#define LX_GETSOCKNAME      204
#define LX_GETPEERNAME      205
#define LX_SENDTO           206
#define LX_RECVFROM         207
#define LX_SETSOCKOPT       208
#define LX_GETSOCKOPT       209
#define LX_SHUTDOWN         210

#define LX_AF_INET          2
#define LX_SOCK_STREAM      1
#define LX_SOCK_DGRAM       2
#define LX_SOL_SOCKET       1
#define LX_SO_REUSEADDR     2
#define LX_SO_TYPE          3
#define LX_SO_ERROR         4
#define LX_SHUT_WR          1

#define LX_ENOPROTOOPT      92
#define LX_EAFNOSUPPORT     97
#define LX_ESOCKTNOSUPPORT  94

#define SK_PORT             8081
#define SK_UPORT            8082

#define LX_O_RDONLY         0
#define LX_O_RDWR           2
#define LX_PROT_NONE        0
#define LX_PROT_READ        1
#define LX_PROT_WRITE       2
#define LX_MS_SYNC          4
#define LX_EBADF            9
#define LX_EACCES           13
#define LX_ENOMEM           12
#define LX_EIO              5

#define LX_AT_FDCWD         (-100)

#define FM_PATH             "/mnt/ram/filemap"
#define FM_SIZE             5000

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

static int g_fail = 0;

/* 读一次单调时钟（毫秒）。QEMU 上实测「连续读时钟」这条路径偶发让某一次
 * clock_gettime 回一个来路不明的正值（详见 docs/已知限制.md），所以这里
 * 重试几次；全失败才回 -1 —— 判据据此判红，不拿 0 冒充成功。 */
static long mono_ms(void)
{
    long ts[2];
    int  i;

    for (i = 0; i < 8; i++)
    {
        ts[0] = 0;
        ts[1] = 0;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts, 0) == 0)
        {
            return (ts[0] * 1000) + (ts[1] / 1000000);
        }
    }
    return -1;
}

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


/* 一行里带数字时也必须**一次 write 写完**。
 * lx_write 持控制台锁只保证「一次调用不被打断」；本内核的 write 每次是一次
 * 系统调用，所以 "parent child=" / 数字 / 换行 三次 write 之间仍然会让位，
 * 另一个进程的整行会插在中间（实测：[fork] parent child=[fork] child pid=12）。
 * 门禁按行读，这一行就变成「缺了 child pid」。 */
static void out_line3(const char *a, lx_long v, const char *b)
{
    char  buf[200];
    char *p = buf;

    p = chk_str(p, a);
    p = chk_num(p, v);
    p = chk_str(p, b);
    *p = '\0';
    out(buf);
}

static void out_line5(const char *a, lx_long v1, const char *b, lx_long v2,
                      const char *c)
{
    char  buf[200];
    char *p = buf;

    p = chk_str(p, a);
    p = chk_num(p, v1);
    p = chk_str(p, b);
    p = chk_num(p, v2);
    p = chk_str(p, c);
    *p = '\0';
    out(buf);
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

#if defined(ARKRTOS_USE_LINUX_THREAD) && (ARKRTOS_USE_LINUX_THREAD == 1)
/* ------------------------------------------------------------------
 * 第三批读取：CLONE_THREAD（线程共享同一份地址空间）
 *
 * 下面这些全局全部落在同一份 .bss 里。线程与父线程共享它 —— 这正是要
 * 验的东西：如果实现给线程另开了一份地址空间，子线程写的字父线程读不
 * 到，这里的 check 立刻红。
 * ------------------------------------------------------------------ */
static volatile lx_long g_thr_ran;
static volatile lx_long g_thr_tid;
static volatile lx_long g_thr_pid;
static volatile lx_long g_thr_tp;
static volatile lx_long g_thr_ctid_self;
static volatile lx_long g_thr_local_addr;
static volatile lx_long g_thr_ctid;
static volatile lx_long g_thr2_ctid;
#define THR_TLS_MAGIC   ((lx_long)0x5a5a0000)

/* 旧式 clone(2) 的子线程从**系统调用后面那条指令**继续跑，返回值 0。
 * 也就是说「进线程体」这件事必须挤在 syscall 与分支之间、不许编译器插入
 * 任何一条读栈的指令 —— 子线程此时已经换到自己的栈上了，父线程的栈内容
 * 它一点都读不到。这里照 musl __clone 的写法，把 syscall / 分支 / 跳转
 * 写进同一个 asm 块：子线程那条路只有 bnez 与 jirl 两条指令。 */
static lx_long lx_clone_thread_call(lx_long flags, lx_long sp, lx_long ptid,
                                    lx_long ctid, lx_long tls,
                                    void (*body)(void))
{
    register lx_long r_a0 __asm__("a0") = flags;
    register lx_long r_a1 __asm__("a1") = sp;
    register lx_long r_a2 __asm__("a2") = ptid;
    register lx_long r_a3 __asm__("a3") = ctid;
    register lx_long r_a4 __asm__("a4") = tls;
    register lx_long r_a7 __asm__("a7") = LX_CLONE;

    __asm__ volatile("syscall 0\n\t"
                     "bnez $a0, 1f\n\t"
                     "jirl $ra, %[body], 0\n\t"
                     "1:\n"
                     : "+r"(r_a0)
                     : "r"(r_a1), "r"(r_a2), "r"(r_a3), "r"(r_a4), "r"(r_a7),
                       [body] "r"((lx_long)(unsigned long)body)
                     : "memory");
    return r_a0;
}

static void thread_body(void)
{
    lx_long local = 0x51;
    lx_long tpv = 0;

    __asm__ volatile("move %0, $tp" : "=r"(tpv));
    g_thr_tp        = tpv;
    g_thr_tid       = lx1(LX_GETTID, 0);
    g_thr_pid       = lx1(LX_GETPID, 0);
    g_thr_local_addr = (lx_long)(unsigned long)&local;
    /* CHILD_SETTID 应当已经在跑起来之前就写好了：读回来必须是自己的 tid。 */
    g_thr_ctid_self = g_thr_ctid;
    g_thr_ran       = 1;
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

/* 第二个线程只用来验「同组里能有两个」，退出码与第一次无关。 */
static void thread_body2(void)
{
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}
#if defined(ARKRTOS_USE_APP_QUOTA) && (ARKRTOS_USE_APP_QUOTA == 1)
/* 配额段专用的线程脚手架。
 *
 * 为什么要三块栈：配额段会**同时**有两个填充线程活着（它们就是「名额
 * 已满」的证据），另外还有几次「应该被拒」的 clone 需要一个不会被真用到
 * 的栈。共用一块栈会让两个线程踩同一片内存 —— 那种失败看起来像内核问题。
 */
static volatile lx_long g_q_go;       /* 填充线程 1 的放行字 */
static volatile lx_long g_q_seen;
static volatile lx_long g_q_ctid;     /* 填充线程 1 的 CLEARTID 字 */
static volatile lx_long g_q2_go;      /* 填充线程 2 的放行字 */
static volatile lx_long g_q2_ctid;    /* 填充线程 2 的 CLEARTID 字 */
static volatile lx_long g_q_park;    /* 配管子进程的定时休眠字（无人会改它） */
static unsigned char s_q_stk[3][16384] __attribute__((aligned(16)));

static void quota_thread_body(void)
{
    g_q_seen = 1;
    while (g_q_go == 0)
    {
        (void)lx4(LX_FUTEX, (lx_long)(unsigned long)&g_q_go,
                  LX_FUTEX_WAIT, 0, 0);
    }
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

static void quota_thread_body2(void)
{
    while (g_q2_go == 0)
    {
        (void)lx4(LX_FUTEX, (lx_long)(unsigned long)&g_q2_go,
                  LX_FUTEX_WAIT, 0, 0);
    }
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

/* 「应该被拒」的那几次用它：万一内核真的放行了，子线程立刻退出，
 * 不许它接着跑父线程的判据代码（那会把后面的读数全部搅乱）。 */
static void quota_nop_body(void)
{
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

/* slot 选栈（0/1 给填充线程，2 给误放行的 nop）；ctid_addr = 0 表示不要
 * CLEARTID（内核把 0 当「没给」，退出时什么也不写）。 */
static lx_long quota_thread_spawn(void (*body)(void), lx_long ctid_addr, int slot)
{
    unsigned int ptid = 0xFFFFFFFFu;

    return lx_clone_thread_call(
               (lx_long)(LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES |
                         LX_CLONE_SIGHAND | LX_CLONE_THREAD |
                         LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID),
               (lx_long)(unsigned long)(s_q_stk[slot] + sizeof(s_q_stk[slot])),
               (lx_long)(unsigned long)&ptid,
               ctid_addr, 0, body);
}

static lx_long quota_thread_start(lx_long ctid_addr)
{
    return quota_thread_spawn(quota_thread_body, ctid_addr, 0);
}

static lx_long quota_thread_start2(lx_long ctid_addr)
{
    return quota_thread_spawn(quota_thread_body2, ctid_addr, 1);
}

static lx_long quota_thread_nop(void)
{
    return quota_thread_spawn(quota_nop_body, 0, 2);
}

/* 放行 + 按 pthread_join 的真实路径等 CLEARTID 被清成 0。 */
static void quota_thread_join(volatile lx_long *p_go, volatile lx_long *p_ctid)
{
    long     ts[2];
    lx_long  dur[2];
    lx_long  wrc = 0;
    int      spun;

    dur[0] = 0;
    dur[1] = 0;
    if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
            (lx_long)(unsigned long)ts, 0) == 0)
    {
        dur[0] = ts[0] + 2;
        dur[1] = ts[1];
    }
    *p_go = 1;
    (void)lx3(LX_FUTEX, (lx_long)(unsigned long)p_go, LX_FUTEX_WAKE, 1);
    for (spun = 0; spun < 64; spun++)
    {
        if (*p_ctid == 0)
        {
            break;
        }
        wrc = lx4(LX_FUTEX, (lx_long)(unsigned long)p_ctid, LX_FUTEX_WAIT,
                  *p_ctid, (lx_long)(unsigned long)dur);
        if ((wrc == 0) || (wrc != -LX_EAGAIN))
        {
            break;
        }
    }
}
#endif
#endif

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

    out_line3("[fork] child pid=", pid, "\n");

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

    out_line3("[fork] parent pid=", lx1(LX_GETPID, 0), "\n");

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

        out_line3("[fork] parent child=", child, "\n");
        if (child <= 0)
        {
            out_line3("[fork] FAIL: clone returned ", child,
                      " in the parent\n");
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
            out_line5("[fork] WNOHANG waited ", (t0 < 0) ? -1 : (now - t0),
                      " ms, saw_alive=", saw_alive, "\n");
        }
        check("wait4 returned child pid", rc, child);
        out_line3("[fork] status=", status, "\n");
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
        out_line3("[fork] blocking wait4 waited ",
                  ((b0 < 0) || (b1 < 0)) ? -1 : (b1 - b0), " ms\n");

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

#if defined(ARKRTOS_USE_PAGING_DEMAND) && (ARKRTOS_USE_PAGING_DEMAND == 1)
    /* ---------------- 真 mmap 语义：MAP_FIXED 丢掉同一地址上的旧内容 ----------------
     * 这一档只在按需分页（DEMAND）构建里存在：没有页级账本就无法「丢掉」
     * 一段已映射的东西，ABI 层在那里如实回 -ENOSYS。
     *
     * 判据是**内容**而不是返回值：先写两个标记字节，再对同一地址做一次
     * MAP_FIXED，之后必须是 0。只判「返回值等于请求地址」的实现能过，
     * 但它其实是把旧页原地留着 —— 那不是 mmap 语义。 */
    {
        lx_long fp;
        volatile unsigned char *fq;
        lx_long frc;

        fp = lx6(LX_MMAP, 0, 4096, LX_PROT_READ | LX_PROT_WRITE,
                 LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
        check("mmap for MAP_FIXED > 0", (fp > 0) ? 1 : 0, 1);
        fq = (volatile unsigned char *)(unsigned long)fp;
        fq[0] = 0x11u;
        fq[4095] = 0x22u;
        frc = lx6(LX_MMAP, fp, 4096, LX_PROT_READ | LX_PROT_WRITE,
                  LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0);
        check("MAP_FIXED returned the same address", (frc == fp) ? 1 : 0, 1);
        check("MAP_FIXED discarded the old bytes",
              ((fq[0] == 0u) && (fq[4095] == 0u)) ? 1 : 0, 1);
        fq[0] = 0x33u;
        check("MAP_FIXED page writable afterwards", (fq[0] == 0x33u) ? 1 : 0, 1);
        check("MAP_FIXED misaligned -> -EINVAL",
              lx6(LX_MMAP, fp + 1, 4096, LX_PROT_READ | LX_PROT_WRITE,
                  LX_MAP_PRIVATE | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0),
              -LX_EINVAL);
        check("MAP_FIXED shared -> -ENOSYS",
              lx6(LX_MMAP, fp, 4096, LX_PROT_READ | LX_PROT_WRITE,
                  LX_MAP_SHARED | LX_MAP_ANONYMOUS | LX_MAP_FIXED, -1, 0),
              -LX_ENOSYS);
        (void)lx2(LX_MUNMAP, fp, 4096);
    }
#endif

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

#if defined(ARKRTOS_USE_PAGING_FILEMAP) && (ARKRTOS_USE_PAGING_FILEMAP == 1)
        /* 开了文件映射档之后，「没带 MAP_ANONYMOUS」不再是非法形态，而是
         * 一次文件映射：fd = -1 必须回 -EBADF。这条读数把两档的语义差别
         * 钉住，也顺手证明它没有被当成匿名映射放过。 */
        check("mmap(shared without anon) == -EBADF",
              lx6(LX_MMAP, 0, 4096, 3, LX_MAP_SHARED, -1, 0), -LX_EBADF);
#else
        check("mmap(shared without anon) == -ENOSYS",
              lx6(LX_MMAP, 0, 4096, 3, LX_MAP_SHARED, -1, 0), -LX_ENOSYS);
#endif
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

            /* fd 表只有 8 格：定序管道用完就还回去，否则后面 socket 段
             * 会因为 fd 槽不够拿不到 socket。 */
            if (spipe != 0)
            {
                (void)lx1(LX_CLOSE, (lx_long)sfds[0]);
                (void)lx1(LX_CLOSE, (lx_long)sfds[1]);
            }
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

#if defined(ARKRTOS_USE_LINUX_FUTEX) && (ARKRTOS_USE_LINUX_FUTEX == 1)
    /* ---------------- futex：WAIT / WAKE 跨进程配对 ----------------
     * 顺序是构造出来的，不是碰出来的：
     *   1) 父进程先把共享页上的字置 1，再 fork；
     *   2) 父进程立刻进 futex(WAIT, 1) —— 这一次调用在本内核里是真阻塞
     *      （时间片重放），父进程睡着之后调度器只剩子进程可跑；
     *   3) 子进程看到字还是 1（证明父进程等的就是它），把它改成 2 再
     *      futex(WAKE, 1)，这一次必须回 1（真的叫醒了一个等待者）；
     *   4) 父进程带着 0 醒来，读到的字必须是 2。
     * 反假绿：如果 WAKE 没叫醒（回 0），子进程用 SIGCHLD 把父进程从等待里
     * 打断（跨进程信号会让 WAIT 回 -EINTR），这样失败是一次可读的读数，
     * 不是把 QEMU 挂死。
     *
     * 另外三条单进程判据不依赖调度：没有等待者时 WAKE 回 0、值不等时 WAIT
     * 回 -EAGAIN、不认识的 op 回 -ENOSYS。 */
    {
        volatile unsigned long *fw;
        lx_long faddr;
        lx_long child6;
        lx_long status6 = -1;
        lx_long rc6 = -1;
        lx_long ppid;

        faddr = lx6(LX_MMAP, 0, 4096, 3,
                    LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
        check("futex mmap shared page > 0", (faddr > 0) ? 1 : 0, 1);

        if (faddr > 0)
        {
            fw = (volatile unsigned long *)(unsigned long)faddr;

            check("futex(WAKE) with no waiter == 0",
                  lx3(LX_FUTEX, (lx_long)(unsigned long)fw, LX_FUTEX_WAKE, 1),
                  0);
            check("futex(WAIT) on a different value == -EAGAIN",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)fw, LX_FUTEX_WAIT,
                      7, 0), -LX_EAGAIN);
            /* 6 = FUTEX_LOCK_PI：PI 类 op 一律 -ENOSYS。这一条不能再拿 9
             * 来测——9 已经是 WAIT_BITSET，是实现了的合法 op。 */
            check("futex PI op (LOCK_PI) == -ENOSYS",
                  lx3(LX_FUTEX, (lx_long)(unsigned long)fw, 6, 0), -LX_ENOSYS);

            *fw = 1ul;
            ppid = lx1(LX_GETPID, 0);

            child6 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
            if (child6 == 0)
            {
                int     code = 0;
                lx_long wrc  = 0;
                int     tries;

                if (*fw == 1ul)
                {
                    code |= 1;      /* bit0: 父进程等的还是 1 */
                }
                /* 「写值 + 叫醒」重复到真的叫醒一个等待者为止。
                 *
                 * 写成循环而不是一次，是因为「父进程恰好在 clone 返回之后
                 * 被时间片切走」是合法调度：用户态可以被抢占，一次就判红
                 * 等于拿调度当判据。实测确实踩到过（WAIT 回 -EAGAIN、
                 * WAKE 回 0，看起来像 futex 不配对，其实是子进程先跑完了）。
                 * 值一直写 2，父进程那边用「当前值」当期望值重睡，所以不管
                 * 谁先被调度，两边都会收敛；futex 真的不配对时这个循环永远
                 * 叫不醒人，仍是红的。 */
                for (tries = 0; tries < 200000; tries++)
                {
                    *fw = 2ul;
                    wrc = lx3(LX_FUTEX, (lx_long)(unsigned long)fw,
                              LX_FUTEX_WAKE, 1);
                    if (wrc == 1)
                    {
                        code |= 2;  /* bit1: 真的叫醒了 1 个等待者 */
                        break;
                    }
                }
                if (wrc != 1)
                {
                    /* 兜底：一直没叫醒就把 SIGCHLD 投给父进程，让那次 WAIT
                     * 回 -EINTR（读数是失败），而不是让镜像挂在这里等超时。 */
                    (void)lx2(LX_KILL, ppid, LX_SIGCHLD);
                }
                (void)lx1(LX_EXIT, code);
                for (;;)
                {
                }
            }

            check("clone for futex child > 0", (child6 > 0) ? 1 : 0, 1);

            /* 父进程：睡着等子进程把字改成 2。
             *
             * 第一次进入时期望值还是 1。若子进程抢在父进程睡下之前就把值改
             * 了（合法调度），这一次 WAIT 按 Linux 语义当场回 -EAGAIN ——
             * 那不是缺陷，拿当前值再睡一次即可；子进程那边一直在重试
             * WAKE，所以「真睡下 + 真被叫醒」这一条仍是可证的。 */
            {
                int round;

                rc6 = -LX_EAGAIN;
                for (round = 0; round < 4; round++)
                {
                    lx_long v6 = (lx_long)(*fw);

                    rc6 = lx4(LX_FUTEX, (lx_long)(unsigned long)fw,
                              LX_FUTEX_WAIT, v6, 0);
                    if (rc6 != -LX_EAGAIN)
                    {
                        break;
                    }
                }
            }
            check("futex(WAIT) returned 0 after being woken", rc6, 0);
            check("parent saw the value the waker wrote", (lx_long)(*fw), 2);

            status6 = -1;
            rc6 = lx4(LX_WAIT4, child6, (lx_long)(unsigned long)&status6, 0, 0);
            check("futex child reaped", rc6, child6);
            /* bit0 必须置：子进程看到的就是父进程等的那个 1（不是「父还没
             * 睡下就叫醒了」那种空匹配）；bit1 必须置：WAKE 真的叫醒了它。 */
            check("futex waker saw 1 and woke 1 (bits=3)",
                  (status6 >> 8) & 0xFF, 3);
        }
    }

    /* 多时间片等待：父进程的 WAIT 若跨过一个 LX_WAIT_SLICE_MS（100 ms）
     * 时间片仍没被叫醒，重放的那一次 a0 已被清成 0。修复前它拿 0 去读字
     * ⇒ -EFAULT 并销账（一次正常的长等待被打死）；修复后用登记下来的地址
     * 继续睡。这条只有 QEMU 上能证明：宿主用例把「重放」单独造出来了，但
     * 真正的调度器才是「时间片到期」这件事的来源。
     *
     * 构造：子进程先忙等 250 ms（> 两个时间片）再写值 + WAKE，父进程这期间
     * 至少被时间片叫醒两次、每次都该继续睡。最终 rc 必须是 0，且从进入
     * WAIT 到返回的墙钟必须 >= 150 ms（证明真的跨了时间片，不是一次命中）。 */
    {
        volatile unsigned long *fw2;
        lx_long faddr2;
        lx_long child8;
        lx_long status8 = -1;
        lx_long rc8 = -1;
        lx_long ppid8;
        long    elapsed8 = -1;

        faddr2 = lx6(LX_MMAP, 0, 4096, 3,
                     LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
        check("futex multislice mmap shared page > 0",
              (faddr2 > 0) ? 1 : 0, 1);

        if (faddr2 > 0)
        {
            fw2 = (volatile unsigned long *)(unsigned long)faddr2;
            *fw2 = 1ul;
            fw2[1] = 0ul;       /* 子进程在它上面等内核超时，没人会改它 */
            ppid8 = lx1(LX_GETPID, 0);

            child8 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
            if (child8 == 0)
            {
                int     code = 0;
                lx_long wrc  = 0;
                long    dur[2];
                int     k;
                int     spins;

                if (*fw2 == 1ul)
                {
                    code |= 1;      /* bit0: 父进程等的还是 1 */
                }
                /* 等满 300 ms（三个 100 ms 时间片）——**不用用户态读时钟**。
                 * 原来这里是「反复 clock_gettime 忙等 250 ms」，QEMU 实测这条
                 * 路径偶发让某一次 clock_gettime 回一个来路不明的正值，忙等被
                 * 提前打断、跨时间片那条判据就假红。改成内核自己记账的超时：
                 * 连睡三个时间片，一个字都不读时钟。 */
                dur[0] = 0;
                dur[1] = 100000000L;        /* 100 ms */
                for (k = 0; k < 3; k++)
                {
                    (void)lx4(LX_FUTEX, (lx_long)(unsigned long)(fw2 + 1),
                              LX_FUTEX_WAIT, 0, (lx_long)(unsigned long)dur);
                }
                for (spins = 0; spins < 200000; spins++)
                {
                    *fw2 = 2ul;
                    wrc = lx3(LX_FUTEX, (lx_long)(unsigned long)fw2,
                              LX_FUTEX_WAKE, 1);
                    if (wrc == 1)
                    {
                        code |= 2;  /* bit1: 真的叫醒了 1 个等待者 */
                        break;
                    }
                }
                if (wrc != 1)
                {
                    (void)lx2(LX_KILL, ppid8, LX_SIGCHLD);
                }
                (void)lx1(LX_EXIT, code);
                for (;;)
                {
                }
            }

            check("clone for futex multislice child > 0",
                  (child8 > 0) ? 1 : 0, 1);

            {
                long t0 = mono_ms();
                int  round;

                rc8 = -LX_EAGAIN;
                for (round = 0; round < 8; round++)
                {
                    lx_long v8 = (lx_long)(*fw2);

                    rc8 = lx4(LX_FUTEX, (lx_long)(unsigned long)fw2,
                              LX_FUTEX_WAIT, v8, 0);
                    if (rc8 != -LX_EAGAIN)
                    {
                        break;
                    }
                }
                if (t0 >= 0)
                {
                    long t1 = mono_ms();

                    if (t1 >= 0)
                    {
                        elapsed8 = t1 - t0;
                    }
                }
            }
            check("futex multislice WAIT returned 0 after being woken", rc8, 0);
            check("futex multislice parent saw the value the waker wrote",
                  (lx_long)(*fw2), 2);
            check("futex multislice WAIT spanned >= 150 ms (crossed a slice)",
                  (elapsed8 >= 150) ? 1 : 0, 1);
            status8 = -1;
            rc8 = lx4(LX_WAIT4, child8, (lx_long)(unsigned long)&status8, 0, 0);
            check("futex multislice child reaped", rc8, child8);
            check("futex multislice waker saw 1 and woke 1 (bits=3)",
                  (status8 >> 8) & 0xFF, 3);
        }
    }

    /* ---------------- futex 第二档：位集 / 超时 / 改挂 ----------------
     * 单进程那几条判「参数与语义」：位集 0、绝对时刻已过、相对 0、
     * REQUEUE 缺 uaddr2 / 期望值不符。
     * 跨进程那两条判「真的按位集筛选」「真的把等待者挂到新地址」：父进程
     * 先睡下（单核上父睡了才轮到子），子进程再动手，所以「掩码不相交时
     * 回 0」确实是在有一个等待者的前提下读到的。
     * 反假绿都落在读数上：位集被忽略时 mask=2 的 WAKE 永远回 0；requeue
     * 没生效时新地址的 WAKE 也永远回 0。 */
    {
        volatile unsigned long *q;
        lx_long qa;
        long    zero[2];
        long    past[2];
        long    dur[2];
        long    t0;
        long    t1;
        lx_long rq;

        zero[0] = 0; zero[1] = 0;
        past[0] = 0; past[1] = 0;      /* 单调时钟 0 ms 一定已经过去 */
        dur[0]  = 0; dur[1] = 50000000; /* 相对 50 ms */

        qa = lx6(LX_MMAP, 0, 4096, 3,
                 LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
        check("futex2 mmap shared page > 0", (qa > 0) ? 1 : 0, 1);

        if (qa > 0)
        {
            q = (volatile unsigned long *)(unsigned long)qa;
            *q = 1ul;

            check("futex(WAIT_BITSET) with bitset 0 == -EINVAL",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_WAIT_BITSET, 1, 0, 0, 0), -LX_EINVAL);
            check("futex(WAKE_BITSET) with bitset 0 == -EINVAL",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_WAKE_BITSET, 1, 0, 0, 0), -LX_EINVAL);
            check("futex(WAKE_BITSET) with no waiter == 0",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_WAKE_BITSET, 1, 0, 0, 1), 0);
            check("futex(WAIT) with zero relative timeout == -ETIMEDOUT",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)q, LX_FUTEX_WAIT, 1,
                      (lx_long)(unsigned long)zero), -LX_ETIMEDOUT);
            check("futex(WAIT_BITSET) with absolute past == -ETIMEDOUT",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_WAIT_BITSET, 1,
                      (lx_long)(unsigned long)past, 0, 1), -LX_ETIMEDOUT);
            check("futex(REQUEUE) with no waiter == 0",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_REQUEUE, 0, 0,
                      (lx_long)(unsigned long)(q + 8), 0), 0);
            check("futex(CMP_REQUEUE) with mismatched value == -EAGAIN",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_CMP_REQUEUE, 0, 0,
                      (lx_long)(unsigned long)(q + 8), 99), -LX_EAGAIN);
            check("futex(REQUEUE) without uaddr2 == -EINVAL",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                      LX_FUTEX_REQUEUE, 0, 0, 0, 0), -LX_EINVAL);

            /* 相对 50 ms 超时：真的跨过一个时间片后回 -ETIMEDOUT。
             * 这条不依赖别的进程：没有人改这个字，到期就是到期。 */
            t0 = -1;
            t1 = -1;
            if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                    (lx_long)(unsigned long)zero, 0) == 0)
            {
                t0 = zero[0] * 1000 + zero[1] / 1000000;
            }
            rq = lx4(LX_FUTEX, (lx_long)(unsigned long)q, LX_FUTEX_WAIT, 1,
                     (lx_long)(unsigned long)dur);
            if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                    (lx_long)(unsigned long)zero, 0) == 0)
            {
                t1 = zero[0] * 1000 + zero[1] / 1000000;
            }
            check("futex(WAIT) with 50 ms timeout == -ETIMEDOUT",
                  rq, -LX_ETIMEDOUT);
            check("futex(WAIT) timeout spanned >= 50 ms",
                  ((t0 >= 0) && (t1 >= 0) && ((t1 - t0) >= 50)) ? 1 : 0, 1);

            /* 父进程在 q 上按位集 2 睡下；子进程先试不相交的位集（必须
             * 叫不醒），再试相交的位集（必须叫醒）。 */
            *q = 1ul;
            {
                lx_long ppid2 = lx1(LX_GETPID, 0);
                lx_long child9 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
                lx_long status9 = -1;
                lx_long rc9 = -1;

                if (child9 == 0)
                {
                    int     code = 0;
                    lx_long w   = 0;
                    int     tries;

                    if (*q == 1ul)
                    {
                        code |= 1;      /* bit0: 父进程等的还是 1 */
                    }
                    /* 父进程已经睡下（单核上父睡了才轮到子），所以这一次
                     * 确实有一个等待者：位集不相交就必须回 0。 */
                    w = lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                            LX_FUTEX_WAKE_BITSET, 1, 0, 0, 1);
                    if (w == 0)
                    {
                        code |= 2;      /* bit1: 不相交的位集没有唤醒它 */
                    }
                    for (tries = 0; tries < 200000; tries++)
                    {
                        w = lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                                LX_FUTEX_WAKE_BITSET, 1, 0, 0, 2);
                        if (w == 1)
                        {
                            code |= 4;  /* bit2: 相交的位集叫醒了 1 个 */
                            break;
                        }
                    }
                    if (w != 1)
                    {
                        (void)lx2(LX_KILL, ppid2, LX_SIGCHLD);
                    }
                    (void)lx1(LX_EXIT, code);
                    for (;;)
                    {
                    }
                }

                check("clone for futex2 bitset child > 0",
                      (child9 > 0) ? 1 : 0, 1);
                {
                    int round;

                    rc9 = -LX_EAGAIN;
                    for (round = 0; round < 4; round++)
                    {
                        lx_long v9 = (lx_long)(*q);

                        rc9 = lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                                  LX_FUTEX_WAIT_BITSET, v9, 0, 0, 2);
                        if (rc9 != -LX_EAGAIN)
                        {
                            break;
                        }
                    }
                }
                check("futex(WAIT_BITSET) returned 0 after being woken",
                      rc9, 0);
                rc9 = lx4(LX_WAIT4, child9, (lx_long)(unsigned long)&status9,
                          0, 0);
                check("futex2 bitset child reaped", rc9, child9);
                check("futex2 bitset waker saw 1, woke only matching (bits=7)",
                      (status9 >> 8) & 0xFF, 7);
            }

            /* 父进程在 q 上睡下；子进程用 CMP_REQUEUE 把它改挂到 q+8，
             * 然后在新地址上 WAKE（旧地址上 WAKE 必须叫不醒）。 */
            *q = 1ul;
            q[8] = 1ul;                 /* 新地址上的字，requeue 后就是期望值 */
            {
                lx_long ppid3 = lx1(LX_GETPID, 0);
                lx_long child10 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
                lx_long status10 = -1;
                lx_long rc10 = -1;
                volatile unsigned long *q2 =
                    (volatile unsigned long *)(unsigned long)(q + 8);

                if (child10 == 0)
                {
                    int     code = 0;
                    lx_long cq   = 0;
                    lx_long w    = 0;
                    int     tries;

                    if (*q == 1ul)
                    {
                        code |= 1;      /* bit0: 父进程等的还是 1 */
                    }
                    /* 子进程不能忙等：它不让步，父进程根本轮不到去 sleep，
                     * 结果 CMP_REQUEUE 永远看不到等待者，子进程把预算烧完
                     * 退出、父进程才去睡 —— 一次必定超时的假红（实测偶发）。
                     * 0.5 ms 一次的重试既让步又给了 60 轮 100 ms 的父进程
                     * 足够窗口（共 5 s，父进程那边总共 6 s）。 */
                    for (tries = 0; tries < 10000; tries++)
                    {
                        lx_long yld[2];

                        cq = lx6(LX_FUTEX, (lx_long)(unsigned long)q,
                                 LX_FUTEX_CMP_REQUEUE, 0, 1,
                                 (lx_long)(unsigned long)q2, 1);
                        /* 返回值 = 唤醒数 + 改挂数：val=0 时它就是「挂过去
                         * 了几个」，所以 >0 才算这次真的动了手。 */
                        if (cq > 0)
                        {
                            w = lx3(LX_FUTEX, (lx_long)(unsigned long)q2,
                                    LX_FUTEX_WAKE, 1);
                            if (w == 1)
                            {
                                code |= 2;  /* bit1: 新地址上的 WAKE 叫醒 1 个 */
                            }
                            break;
                        }
                        if ((cq < 0) && (cq != -LX_EAGAIN))
                        {
                            break;          /* 真错误：不再重试 */
                        }
                        /* 让步：在这一页里另挑一个字（与 q / q2 不同键，
                         * 没人等它）睡 0.5 ms。本机没有 sched_yield 也没有
                         * nanosleep，FUTEX_WAIT 带 timeout 是唯一能主动交出
                         * CPU 的路；栈上的字没有键（回 -ENOSYS），所以必须
                         * 用这张映射页里的字。 */
                        yld[0] = 0;
                        yld[1] = 500000L;
                        (void)lx4(LX_FUTEX,
                                  (lx_long)(unsigned long)(q + 64),
                                  LX_FUTEX_WAIT, 0,
                                  (lx_long)(unsigned long)yld);
                    }
                    if ((cq <= 0) || (w != 1))
                    {
                        code &= ~2;
                        (void)lx2(LX_KILL, ppid3, LX_SIGCHLD);
                    }
                    (void)lx1(LX_EXIT, code);
                    for (;;)
                    {
                    }
                }

                check("clone for futex2 requeue child > 0",
                      (child10 > 0) ? 1 : 0, 1);
                {
                    int     round;
                    lx_long tdur[2];

                    tdur[0] = 0;
                    tdur[1] = 100000000L;       /* 100 ms：只用来给重试封顶 */
                    rc10 = -LX_EAGAIN;
                    for (round = 0; round < 60; round++)
                    {
                        lx_long v10 = (lx_long)(*q);

                        rc10 = lx4(LX_FUTEX, (lx_long)(unsigned long)q,
                                   LX_FUTEX_WAIT, v10,
                                   (lx_long)(unsigned long)tdur);
                        if (rc10 == 0)
                        {
                            break;
                        }
                        /* 三种「这次没等到」都要重试，真实调用方也是这么写的：
                         *   -EAGAIN  子线程还没来得及把字改掉；
                         *   -EINTR   被本进程里子进程退出的 SIGCHLD 打断
                         *            （Linux 允许 FUTEX_WAIT 被信号打断）；
                         *   -ETIMEDOUT 这一轮 100 ms 内子线程还没轮到。
                         * 第一版只认 -EAGAIN，第九档里偶发把一次合法打断读成
                         * 假红（rc10 = -4）。 */
                        if ((rc10 != -LX_EAGAIN) && (rc10 != -LX_EINTR) &&
                            (rc10 != -LX_ETIMEDOUT))
                        {
                            break;
                        }
                    }
                }
                check("futex(WAIT) on requeued word returned 0", rc10, 0);
                check("futex2 requeue: old word has no waiter left",
                      lx3(LX_FUTEX, (lx_long)(unsigned long)q,
                          LX_FUTEX_WAKE, 1), 0);
                rc10 = lx4(LX_WAIT4, child10,
                           (lx_long)(unsigned long)&status10, 0, 0);
                check("futex2 requeue child reaped", rc10, child10);
                check("futex2 requeue child saw 1 and new-word WAKE woke 1 "
                      "(bits=3)", (status10 >> 8) & 0xFF, 3);
            }
        }
    }

#endif

#if defined(ARKRTOS_USE_PAGING_FILEMAP) && (ARKRTOS_USE_PAGING_FILEMAP == 1)
    /* ---------------- 文件映射与写回（U-21） ----------------
     * 探测文件 /mnt/ram/filemap 是内核侧先写进 ramfs 的 5000 字节，
     * 第 i 字节 = 'A' + (i % 26)。夹具自己不写这个文件（它一启动就要读），
     * 所以这里只读、只核对。四组判据：
     *   1) 只读私有映射：内容真的装进映射区（首字节 / 第 25 / 第 4095）；
     *   2) 参数校验：offset 非 4 KB、坏 fd、只读 fd 上的可写共享分别回
     *      -EINVAL / -EBADF / -EACCES；
     *   3) offset=4096：映射从第二页起（5000 字节文件的第二页只剩 904
     *      字节内容），EOF 之后必须是零而不是垃圾；
     *   4) 写回：O_RDWR + MAP_SHARED 改两个字节 → msync → 重新映射读回，
     *      必须看到新值；同一次里再做一份私有可写映射写另外两个字节，
     *      重新映射时那两个字节必须还是原值。
     * 反假绿：第 4 组把「共享写落盘」与「私有写不落盘」放在同一次重新
     * 映射里读 —— 只判前者的话，"把映射区直接整段写回文件"也能过。 */
    {
        lx_long fd;
        lx_long fdw;
        lx_long addr;
        lx_long addr2;
        lx_long addr5;
        lx_long maddr = 0;
        lx_long paddr = 0;
        lx_long bits;

        fd = lx3(LX_OPENAT, LX_AT_FDCWD, (lx_long)(unsigned long)FM_PATH,
                 LX_O_RDONLY);
        check("filemap open read-only fd > 2", (fd > 2) ? 1 : 0, 1);

        addr = lx6(LX_MMAP, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, fd, 0);
        check("filemap private map > 0", (addr > 0) ? 1 : 0, 1);
        if (addr > 0)
        {
            volatile unsigned char *m =
                (volatile unsigned char *)(unsigned long)addr;

            /* 'A' + 25 = 'Z'；4095 % 26 = 13 -> 'N' */
            check("filemap first byte is file first byte", (lx_long)m[0], 'A');
            check("filemap byte 25 is Z", (lx_long)m[25], 'Z');
            check("filemap last byte of the page", (lx_long)m[4095], 'N');
        }

        check("filemap misaligned offset == -EINVAL",
              lx6(LX_MMAP, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, fd, 0x10),
              -LX_EINVAL);
        check("filemap bad fd == -EBADF",
              lx6(LX_MMAP, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, 99, 0),
              -LX_EBADF);
        check("filemap writable shared on read-only fd == -EACCES",
              lx6(LX_MMAP, 0, 4096, LX_PROT_READ | LX_PROT_WRITE, LX_MAP_SHARED,
                  fd, 0), -LX_EACCES);

        /* offset=4096：4096 % 26 = 14 -> 'O'；5000 - 4096 = 904 字节之后是 EOF */
        addr2 = lx6(LX_MMAP, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, fd, 4096);
        check("filemap offset=4096 map > 0", (addr2 > 0) ? 1 : 0, 1);
        if (addr2 > 0)
        {
            volatile unsigned char *m2 =
                (volatile unsigned char *)(unsigned long)addr2;

            check("filemap offset content starts at 4096", (lx_long)m2[0], 'O');
            check("filemap past EOF reads zero", (lx_long)m2[904], 0);
        }

        fdw = lx3(LX_OPENAT, LX_AT_FDCWD, (lx_long)(unsigned long)FM_PATH,
                  LX_O_RDWR);
        check("filemap O_RDWR open on existing file > 0",
              (fdw > 2) ? 1 : 0, 1);

        if (fdw > 2)
        {
            maddr = lx6(LX_MMAP, 0, 4096, LX_PROT_READ | LX_PROT_WRITE,
                        LX_MAP_SHARED, fdw, 0);
            check("filemap writable shared map > 0", (maddr > 0) ? 1 : 0, 1);
            if (maddr > 0)
            {
                volatile unsigned char *mw =
                    (volatile unsigned char *)(unsigned long)maddr;

                mw[10] = 'X';
                mw[11] = 'Y';
                check("filemap msync rc",
                      lx3(LX_MSYNC, maddr, 4096, LX_MS_SYNC), 0);
                check("filemap munmap rc", lx2(LX_MUNMAP, maddr, 4096), 0);
            }

            paddr = lx6(LX_MMAP, 0, 4096, LX_PROT_READ | LX_PROT_WRITE,
                        LX_MAP_PRIVATE, fdw, 0);
            check("filemap private writable map > 0", (paddr > 0) ? 1 : 0, 1);
            if (paddr > 0)
            {
                volatile unsigned char *mp =
                    (volatile unsigned char *)(unsigned long)paddr;

                /* 20 % 26 = 20 -> 'U'，21 -> 'V'；改成 Q 之后必须不落盘 */
                mp[20] = 'Q';
                mp[21] = 'Q';
                check("filemap private msync rc (no writeback)",
                      lx3(LX_MSYNC, paddr, 4096, LX_MS_SYNC), 0);
                check("filemap private munmap rc",
                      lx2(LX_MUNMAP, paddr, 4096), 0);
            }
        }

        /* 重新映射读回：共享写的 10/11 变了（'X'/'Y'），12 还是 'M'，
         * 私有的 20/21 还是 'U'/'V'。 */
        addr5 = lx6(LX_MMAP, 0, 4096, LX_PROT_READ, LX_MAP_PRIVATE, fd, 0);
        check("filemap remap after writeback > 0", (addr5 > 0) ? 1 : 0, 1);
        if (addr5 > 0)
        {
            volatile unsigned char *m5 =
                (volatile unsigned char *)(unsigned long)addr5;

            bits = 0;
            if (m5[10] == 'X') { bits |= 1; }
            if (m5[11] == 'Y') { bits |= 2; }
            if (m5[12] == 'M') { bits |= 4; }
            if (m5[20] == 'U') { bits |= 8; }
            if (m5[21] == 'V') { bits |= 16; }
            check("filemap shared write persisted, private write not (bits=31)",
                  bits, 31);
        }
    }
#endif

#if defined(ARKRTOS_USE_PAGING_MPROTECT) && (ARKRTOS_USE_PAGING_MPROTECT == 1)
    /* ---------------- 页级写保护 mprotect（U-22） ----------------
     * 要证的是「mprotect 真的改了硬件条目」，不是「返回 0」：
     *   1) 一页设只读后，读仍然成功（只读不是不可访问）；
     *   2) **同一对里的另一半页照旧可写** —— 这是 4 KB 粒度与 8 KB 粒度
     *      的分界线，按对记台账的实现在这里必红；
     *   3) mprotect(RW) 之后写回得去（保护是可撤的，不是单向的）；
     *   4) 违规写由子进程执行：它必须被**摘掉**（不是重启 —— 重启会回到
     *      同一条指令、无限重启），父进程 wait4 收得到它，且父进程那一页
     *      一个字节都没变。串口侧另有 `[LA64U] mprotect violation` 一行，
     *      由门禁逐字断言。
     * 反假绿：第 2 条咬粒度、第 4 条咬「真的拦住了」。只有第 1、3 条的话，
     * 「mprotect 什么都不做」也能全绿。 */
    {
        volatile unsigned long *pg;
        lx_long maddr;
        lx_long child7;
        lx_long status7 = -1;
        lx_long rc7;

        maddr = lx6(LX_MMAP, 0, 8192, 3,
                    LX_MAP_PRIVATE | LX_MAP_ANONYMOUS, -1, 0);
        check("mprotect mmap 8 KB > 0", (maddr > 0) ? 1 : 0, 1);

        if (maddr > 0)
        {
            pg = (volatile unsigned long *)(unsigned long)maddr;
            pg[0] = 0x1111ul;           /* 第一页 */
            pg[1024] = 0x2222ul;        /* 第二页（4096 / 8 = 512 个 long） */

            check("mprotect RO == 0",
                  lx3(LX_MPROTECT, maddr, 4096, LX_PROT_READ), 0);
            check("read of a read-only page still works",
                  (lx_long)pg[0], 0x1111);

            pg[1024] = 0x3333ul;
            check("sibling 4 KB half of the same pair stays writable",
                  (lx_long)pg[1024], 0x3333);

            check("mprotect RW == 0",
                  lx3(LX_MPROTECT, maddr, 4096, LX_PROT_READ | LX_PROT_WRITE), 0);
            pg[0] = 0x4444ul;
            check("write after mprotect(RW) took effect",
                  (lx_long)pg[0], 0x4444);

            check("mprotect RO again == 0",
                  lx3(LX_MPROTECT, maddr, 4096, LX_PROT_READ), 0);

            child7 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
            if (child7 == 0)
            {
                volatile unsigned long *c =
                    (volatile unsigned long *)(unsigned long)maddr;

                c[0] = 0xDEADul;        /* 这一条必须杀死本任务 */
                (void)lx1(LX_EXIT, 0);  /* 走到这里 = 写没被拦住 */
                for (;;)
                {
                }
            }

            check("clone for mprotect child > 0", (child7 > 0) ? 1 : 0, 1);

            rc7 = lx4(LX_WAIT4, child7, (lx_long)(unsigned long)&status7, 0, 0);
            /* 用位掩码比较而不是直接比 rc7：门禁要逐字断言这一行，
             * 值随 pid 变就没法当读数用。 */
            check("mprotect child reaped (killed, not restarted)",
                  (rc7 == child7) ? 1 : 0, 1);
            /* 状态字要按 Linux 的「被信号终止」编码（低 7 位 = 信号号），
             * 不是 exit(11) 的高字节。两件事在 Linux 里 WIFEXITED/WIFSIGNALED
             * 完全不同，混着报等于把一次越界写说成程序正常退出。 */
            check("mprotect violation reports SIGSEGV status",
                  status7 & 0x7F, 11);
            check("parent page unchanged after the violation",
                  (lx_long)pg[0], 0x4444);
        }
    }
#endif

#if defined(ARKRTOS_USE_LINUX_SELECT) && (ARKRTOS_USE_LINUX_SELECT == 1)
    /* ---------------- pselect6：跨 fd 就绪等待（U-22） ----------------
     * 要证的是「就绪判据真的按对象状态算」，不是「返回了个数」：
     *   1) 空管道的读端**不**就绪：timeout=0 即刻回 0，且用户集被清位
     *      （写回只留就绪位，不是原样退回来）；
     *   2) 50 ms 定时等待必须真的等够：墙钟读数，不拿调度顺序当判据；
     *   3) 写进 3 字节后读端就绪，且用户集里那一位还在；
     *   4) 写端按「管道还有空间」判就绪，stdout 恒可写；
     *   5) 管道写满后写端**不再**就绪 —— 这一条咬的是「容量」不是「能写」；
     *   6) 写端全关后读端按 EOF 就绪 —— 这一条咬的是「写端没了」不是
     *      「有数据」（Linux 的 select 在 EOF 上必须报可读）；
     *   7) exceptfds 接受但永不上报，且同样被清位（本层没有带外数据）；
     *   8) 没有对象支撑的 fd 被选中回 -EBADF，不是静默忽略。
     * 反假绿：第 5 条咬容量、第 6 条咬 EOF、第 8 条咬 fd 有效性。只留
     * 第 3/4/7 条的话，「直接回 nfds 个」也能全绿。 */
    {
        int          sel[2];
        unsigned int rs[32];
        unsigned int ws[32];
        unsigned int es[32];
        long         to[2];
        long         cts[2];
        lx_long      r;
        lx_long      wrc;
        long         t0;
        long         t1;
        int          i;
        int          selr;
        int          selw;
        int          rword;
        int          wword;
        unsigned int rbit;
        unsigned int wbit;

        check("pselect6 pipe2 == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)sel, 0), 0);

        selr  = sel[0];
        selw  = sel[1];
        rword = selr >> 5;
        wword = selw >> 5;
        rbit  = 1u << (unsigned int)(selr & 31);
        wbit  = 1u << (unsigned int)(selw & 31);

        /* (1) 空管道读端不就绪：timeout=0 是纯轮询，不睡。 */
        for (i = 0; i < 32; i++)
        {
            rs[i] = 0u;
            ws[i] = 0u;
            es[i] = 0u;
        }
        rs[rword] = rbit;
        to[0] = 0;
        to[1] = 0;
        r = lx6(LX_PSELECT6, (lx_long)(selr + 1),
                (lx_long)(unsigned long)rs, 0, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 empty pipe read end not ready", r, 0);
        check("pselect6 clears the user set when nothing is ready",
              (lx_long)rs[rword], 0);

        /* (2) 50 ms 定时等待：真的等够，而不是立刻回 0。 */
        t0 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)cts, 0) == 0)
        {
            t0 = (cts[0] * 1000) + (cts[1] / 1000000);
        }
        rs[rword] = rbit;
        to[0] = 0;
        to[1] = 50 * 1000000;
        r = lx6(LX_PSELECT6, (lx_long)(selr + 1),
                (lx_long)(unsigned long)rs, 0, 0,
                (lx_long)(unsigned long)to, 0);
        t1 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)cts, 0) == 0)
        {
            t1 = (cts[0] * 1000) + (cts[1] / 1000000);
        }
        out_line3("[fork] pselect6 timed wait ",
                  ((t0 < 0) || (t1 < 0)) ? -1 : (t1 - t0), " ms\n");
        check("pselect6 timed wait returns 0 at the deadline", r, 0);
        check("pselect6 timed wait really waited >= 40 ms",
              ((t0 >= 0) && (t1 >= 0) && ((t1 - t0) >= 40)) ? 1 : 0, 1);

        /* (3) 有数据之后读端就绪，用户集里那一位必须保留。 */
        wrc = lx3(LX_WRITE, (lx_long)selw,
                  (lx_long)(unsigned long)"abc", 3);
        check("pselect6 pipe wrote 3 bytes", wrc, 3);
        rs[rword] = rbit;
        to[0] = 0;
        to[1] = 0;
        r = lx6(LX_PSELECT6, (lx_long)(selr + 1),
                (lx_long)(unsigned long)rs, 0, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 read end ready after 3 bytes", r, 1);
        check("pselect6 keeps the ready bit in the user set",
              (((int)rs[rword] & (int)rbit) != 0) ? 1 : 0, 1);

        /* (4) 写端按「还有空间」就绪；stdout 恒可写。 */
        ws[wword] = wbit;
        r = lx6(LX_PSELECT6, (lx_long)(selw + 1),
                0, (lx_long)(unsigned long)ws, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 write end ready while the pipe has space", r, 1);

        for (i = 0; i < 32; i++)
        {
            ws[i] = 0u;
        }
        ws[0] = 2u;                     /* fd 1 的位（第 0 个 32 位字） */
        r = lx6(LX_PSELECT6, 2, 0, (lx_long)(unsigned long)ws, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 stdout is always writable", r, 1);

        /* (5) 写满之后写端不再就绪（容量 256 字节） */
        i = 0;
        while (i < 8192)
        {
            wrc = lx3(LX_WRITE, (lx_long)selw,
                      (lx_long)(unsigned long)"x", 1);
            if (wrc != 1)
            {
                break;
            }
            i++;
        }
        out_line3("[fork] pselect6 filled the pipe with ", i, " bytes\n");
        ws[wword] = wbit;
        r = lx6(LX_PSELECT6, (lx_long)(selw + 1),
                0, (lx_long)(unsigned long)ws, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 write end not ready when the pipe is full", r, 0);

        /* (6) 写端全关之后读端按 EOF 就绪 */
        (void)lx1(LX_CLOSE, (lx_long)selw);
        rs[rword] = rbit;
        r = lx6(LX_PSELECT6, (lx_long)(selr + 1),
                (lx_long)(unsigned long)rs, 0, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 read end ready on EOF after the writer closed",
              r, 1);

        /* (7) exceptfds 永不上报，且被清位 */
        es[rword] = rbit;
        r = lx6(LX_PSELECT6, (lx_long)(selr + 1), 0, 0,
                (lx_long)(unsigned long)es,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 never reports exceptfds", r, 0);
        check("pselect6 clears exceptfds", (lx_long)es[rword], 0);

        /* (8) 没有对象支撑的 fd 被选中必须回 -EBADF */
        rs[3] = 8u;                     /* fd 99 的位 */
        r = lx6(LX_PSELECT6, 100, (lx_long)(unsigned long)rs, 0, 0,
                (lx_long)(unsigned long)to, 0);
        check("pselect6 bad fd == -EBADF", r, -LX_EBADF);

        (void)lx1(LX_CLOSE, (lx_long)selr);
    }
#endif


#if defined(ARKRTOS_USE_LINUX_SOCKET) && (ARKRTOS_USE_LINUX_SOCKET == 1) && \
    defined(ARKRTOS_USE_LWIP) && (ARKRTOS_USE_LWIP == 1)
    /* ---------------- socket：翻译层 + lwIP 回环（U-23） ----------------
     * 要证的是「13 个号真的把一个 TCP 连接跑完」，不是「返回了个数」：
     *   1) 建 stream / dgram 两个 socket，坏 domain / 坏 type 各自的 errno；
     *   2) bind 127.0.0.1:8081 + listen + getsockname 读回端口（端口写回
     *      必须按 sockaddr_in 的网络序，不是 host 序）；
     *   3) connect 回环、accept 拿到新 fd、accept 写回的对端 family；
     *   4) send 5 字节 / recv 5 字节，载荷逐字节比对 —— 只比长度的话
     *      「返回 5 但内容是旧的」也能过；
     *   5) getpeername 读回 127.0.0.1（证明地址不是 0.0.0.0）；
     *   6) SO_REUSEADDR 设置成功、SO_TYPE 本地答 1、SO_ERROR 本地答 0；
     *      未知选项回 -ENOPROTOOPT、写 SO_ERROR 回 -EINVAL（翻译层自己
     *      的拒法，不能转给协议栈去猜）；
     *   7) UDP 走 sendto / recvfrom，且 recvfrom 对端端口必须等于客户端
     *      getsockname 读到的本地端口（同一根连接，不是「随便找了个地址」）；
     *   8) shutdown(SHUT_WR) 之后对端 recv 回 0（EOF）—— 这一条咬的是
     *      「半关真的发了 FIN」，不是「关了个标志」；
     *   9) 关掉之后 read / write 同一个 fd 回 -EBADF（反假绿：fd 槽必须
     *      真的被释放）。
     *
     * 协议栈由 lwIP 的回环网卡提供（127.0.0.1/8，包不出芯片）。第一个
     * socket() 可能早于 tcpip 线程起来，此时 net 服务回 ENOTSUP，翻译层
     * 如实回 -ENOSYS；这里用 pselect6 的空超时当睡眠重试，最多 3 秒。
     * 不重试的话，这一档的成败会取决于调度顺序。 */
    {
        typedef struct
        {
            unsigned char b[16];
        } sk_sa_t;

        sk_sa_t  sa;
        sk_sa_t  pa;
        unsigned int lenp;
        char     buf[16];
        lx_long  srv;
        lx_long  cli;
        lx_long  acc;
        lx_long  dgs;
        lx_long  dgc;
        lx_long  r;
        lx_long  r2;
        lx_long  pack;
        unsigned int pack32;
        long     ts[2];
        int      i;
        int      tries;
        long     tm0;
        long     tm2;

        /* 地址按 Linux sockaddr_in 写：family 小端 2 字节、端口网络序、
         * 地址 4 字节网络序。 */
        for (i = 0; i < 16; i++)
        {
            sa.b[i] = 0u;
            pa.b[i] = 0u;
        }
        sa.b[0] = (unsigned char)(LX_AF_INET & 0xFF);
        sa.b[1] = 0u;
        sa.b[2] = (unsigned char)((SK_PORT >> 8) & 0xFF);
        sa.b[3] = (unsigned char)(SK_PORT & 0xFF);
        sa.b[4] = 127u;
        sa.b[7] = 1u;

        tm0 = mono_ms();
        /* 等协议栈：socket() 早于 tcpip 线程时如实回 -ENOSYS，重试。 */
        srv = -1;
        for (tries = 0; tries < 300; tries++)
        {
            srv = lx3(LX_SOCKET, LX_AF_INET, LX_SOCK_STREAM, 0);
            if (srv >= 0)
            {
                break;
            }
            ts[0] = 0;
            ts[1] = 10 * 1000000;
            (void)lx6(LX_PSELECT6, 0, 0, 0, 0,
                      (lx_long)(unsigned long)ts, 0);
        }
        check("socket stream fd >= 3", (srv >= 3) ? 1 : 0, 1);

        check("socket bad domain == -EAFNOSUPPORT",
              lx3(LX_SOCKET, 99, LX_SOCK_STREAM, 0), -LX_EAFNOSUPPORT);
        check("socket type >= SOCK_MAX == -EINVAL",
              lx3(LX_SOCKET, LX_AF_INET, 15, 0), -LX_EINVAL);
        check("socket unsupported type == -ESOCKTNOSUPPORT",
              lx3(LX_SOCKET, LX_AF_INET, 7, 0), -LX_ESOCKTNOSUPPORT);

        dgs = lx3(LX_SOCKET, LX_AF_INET, LX_SOCK_DGRAM, 0);
        check("socket dgram fd >= 3", (dgs >= 3) ? 1 : 0, 1);

        check("bind loopback:8081 == 0",
              lx3(LX_BIND, srv, (lx_long)(unsigned long)&sa, 16), 0);
        check("listen == 0", lx2(LX_LISTEN, srv, 2), 0);

        /* getsockname：端口要按网络序读回 8081。 */
        lenp = 0u;
        r = lx3(LX_GETSOCKNAME, srv, (lx_long)(unsigned long)&pa,
                (lx_long)(unsigned long)&lenp);
        check("getsockname == 0", r, 0);
        check("getsockname addrlen == 16", (lx_long)lenp, 16);
        check("getsockname family == AF_INET", (lx_long)pa.b[0], LX_AF_INET);
        check("getsockname port == 8081",
              ((lx_long)pa.b[2] << 8) | (lx_long)pa.b[3], SK_PORT);

        cli = lx3(LX_SOCKET, LX_AF_INET, LX_SOCK_STREAM, 0);
        check("client socket fd >= 3", (cli >= 3) ? 1 : 0, 1);
        check("connect loopback:8081 == 0",
              lx3(LX_CONNECT, cli, (lx_long)(unsigned long)&sa, 16), 0);

        for (i = 0; i < 16; i++)
        {
            pa.b[i] = 0u;
        }
        lenp = 0u;
        acc = lx3(LX_ACCEPT, srv, (lx_long)(unsigned long)&pa,
                  (lx_long)(unsigned long)&lenp);
        check("accept fd >= 3", (acc >= 3) ? 1 : 0, 1);
        check("accept wrote peer family == AF_INET", (lx_long)pa.b[0],
              LX_AF_INET);

        r = lx6(LX_SENDTO, cli, (lx_long)(unsigned long)"hiyou", 5,
                0, 0, 0);
        check("tcp send 5 == 5", r, 5);
        for (i = 0; i < 16; i++)
        {
            buf[i] = 0;
        }
        r = lx6(LX_RECVFROM, acc, (lx_long)(unsigned long)buf, 5,
                0, 0, 0);
        check("tcp recv 5 == 5", r, 5);
        pack = ((lx_long)(unsigned char)buf[0] << 24) |
               ((lx_long)(unsigned char)buf[1] << 16) |
               ((lx_long)(unsigned char)buf[2] << 8) |
               (lx_long)(unsigned char)buf[3];
        check("tcp payload hiyo", pack, 0x6869796FL);
        check("tcp payload last byte", (lx_long)(unsigned char)buf[4], 0x75);

        for (i = 0; i < 16; i++)
        {
            pa.b[i] = 0u;
        }
        lenp = 0u;
        r = lx3(LX_GETPEERNAME, acc, (lx_long)(unsigned long)&pa,
                (lx_long)(unsigned long)&lenp);
        check("getpeername == 0", r, 0);
        check("getpeername ip == 127.0.0.1",
              ((lx_long)pa.b[4] << 24) | ((lx_long)pa.b[5] << 16) |
              ((lx_long)pa.b[6] << 8) | (lx_long)pa.b[7], 0x7F000001L);

        pack32 = 1u;
        r = lx6(LX_SETSOCKOPT, cli, LX_SOL_SOCKET, LX_SO_REUSEADDR,
                (lx_long)(unsigned long)&pack32, 4, 0);
        check("setsockopt REUSEADDR == 0", r, 0);

        {
            unsigned int optlen = 4u;

            /* 内核按 int 写 4 字节：缓冲就要是 4 字节，用 long 接的话高 32
             * 位保留旧值，读回来是一个「看起来很深」的假数字。 */
            pack32 = 0xFFFFFFFFu;
            r = lx6(LX_GETSOCKOPT, cli, LX_SOL_SOCKET, LX_SO_TYPE,
                    (lx_long)(unsigned long)&pack32,
                    (lx_long)(unsigned long)&optlen, 0);
            check("getsockopt SO_TYPE == 0", r, 0);
            check("getsockopt SO_TYPE locally answered 1",
                  (lx_long)pack32, 1);

            pack32 = 0xFFFFFFFFu;
            optlen = 4u;
            r = lx6(LX_GETSOCKOPT, cli, LX_SOL_SOCKET, LX_SO_ERROR,
                    (lx_long)(unsigned long)&pack32,
                    (lx_long)(unsigned long)&optlen, 0);
            check("getsockopt SO_ERROR == 0", r, 0);
            check("getsockopt SO_ERROR locally answered 0",
                  (lx_long)pack32, 0);

            pack32 = 0u;
            optlen = 4u;
            r = lx6(LX_GETSOCKOPT, cli, LX_SOL_SOCKET, 55,
                    (lx_long)(unsigned long)&pack32,
                    (lx_long)(unsigned long)&optlen, 0);
            check("getsockopt unknown opt == -ENOPROTOOPT", r,
                  -LX_ENOPROTOOPT);

            pack32 = 0u;
            r = lx6(LX_SETSOCKOPT, cli, LX_SOL_SOCKET, LX_SO_ERROR,
                    (lx_long)(unsigned long)&pack32, 4, 0);
            check("setsockopt SO_ERROR == -EINVAL", r, -LX_EINVAL);
        }

        /* 半关：对端必须看到 EOF（recv 回 0），不是「什么都没有」。 */
        check("shutdown SHUT_WR == 0", lx2(LX_SHUTDOWN, acc, LX_SHUT_WR), 0);
        r = lx6(LX_RECVFROM, cli, (lx_long)(unsigned long)buf, 8,
                0, 0, 0);
        check("recv after peer shutdown == 0 (EOF)", r, 0);

        /* TCP 这三只先关掉：lwIP 的 netconn 池（MEMP_NUM_NETCONN=4）与
         * 内核的 socket 行数（ARKRTOS_NET_MAX_SOCKETS=4）一样大，同时占满
         * 之后 UDP 那一对建不出来。真实程序也是「用完就关」，这里照做比
         * 悄悄把池子调大更能暴露「池子到底够不够」。 */
        (void)lx1(LX_CLOSE, acc);
        (void)lx1(LX_CLOSE, srv);
        (void)lx1(LX_CLOSE, cli);

        /* 反假绿：fd 槽必须真的释放，关掉之后不能再读写。 */
        check("write on closed socket == -EBADF",
              lx3(LX_WRITE, cli, (lx_long)(unsigned long)"x", 1), -LX_EBADF);
        check("read on closed socket == -EBADF",
              lx3(LX_READ, cli, (lx_long)(unsigned long)buf, 1), -LX_EBADF);

        /* UDP：sendto 之后 recvfrom 必须同时读回数据与对端端口。 */
        sa.b[2] = (unsigned char)((SK_UPORT >> 8) & 0xFF);
        sa.b[3] = (unsigned char)(SK_UPORT & 0xFF);
        check("udp bind loopback:8082 == 0",
              lx3(LX_BIND, dgs, (lx_long)(unsigned long)&sa, 16), 0);

        dgc = lx3(LX_SOCKET, LX_AF_INET, LX_SOCK_DGRAM, 0);
        check("udp client socket fd >= 3", (dgc >= 3) ? 1 : dgc, 1);
        r = lx6(LX_SENDTO, dgc, (lx_long)(unsigned long)"udp", 3u, 0,
                (lx_long)(unsigned long)&sa, 16);
        check("udp sendto 3 == 3", r, 3);

        for (i = 0; i < 16; i++)
        {
            pa.b[i] = 0u;
            buf[i] = 0;
        }
        lenp = 0u;
        r = lx6(LX_RECVFROM, dgs, (lx_long)(unsigned long)buf, 3u, 0,
                (lx_long)(unsigned long)&pa,
                (lx_long)(unsigned long)&lenp);
        check("udp recvfrom 3 == 3", r, 3);
        pack = ((lx_long)(unsigned char)buf[0] << 16) |
               ((lx_long)(unsigned char)buf[1] << 8) |
               (lx_long)(unsigned char)buf[2];
        check("udp payload udp", pack, 0x756470L);

        {
            sk_sa_t cli_sa;
            unsigned int cli_len = 0u;
            lx_long      cli_port;

            for (i = 0; i < 16; i++)
            {
                cli_sa.b[i] = 0u;
            }
            r2 = lx3(LX_GETSOCKNAME, dgc, (lx_long)(unsigned long)&cli_sa,
                     (lx_long)(unsigned long)&cli_len);
            cli_port = ((lx_long)cli_sa.b[2] << 8) | (lx_long)cli_sa.b[3];
            check("udp recvfrom peer port == client port",
                  (r2 == 0) ? ((((lx_long)pa.b[2] << 8) |
                                (lx_long)pa.b[3]) - cli_port) : -1, 0);
        }

        (void)lx1(LX_CLOSE, dgc);
        (void)lx1(LX_CLOSE, dgs);
        /* 一次 socket 往返不该按「一个等待时间片」结算（LX_WAIT_SLICE_MS =
         * 100 ms）：回复落地时服务任务会直接叫醒等待者（见 arkrtos_net.c 的
         * g_net_req_wake）。这条判据就是那个机制的回归门 —— 唤醒丢了的话，
         * 这一段十几次往返会回到 ~2 s。 */
        tm2 = mono_ms();
        check("socket round trips do not cost a wait slice (< 800 ms)",
              ((tm0 >= 0) && (tm2 >= 0) && ((tm2 - tm0) < 800)) ? 1 : 0, 1);
    }
#elif defined(ARKRTOS_USE_LINUX_SELECT) && (ARKRTOS_USE_LINUX_SELECT == 1)
    /* 反假绿：socket 开关没开时，198 号必须如实回 -ENOSYS，而不是落到
     * 一个「看起来能用」的桩上。这一条只在 select 档（socket 的下一档）
     * 里存在，所以不改变下面各档的条数。 */
    check("socket with the switch off == -ENOSYS",
          lx3(LX_SOCKET, LX_AF_INET, LX_SOCK_STREAM, 0), -LX_ENOSYS);
#endif

#if defined(ARKRTOS_USE_LINUX_THREAD) && (ARKRTOS_USE_LINUX_THREAD == 1)
    /* ------------------------------------------------------------------
     * 第三批：CLONE_THREAD
     * ------------------------------------------------------------------ */
    {
        static unsigned char s_thr_stack[16384] __attribute__((aligned(16)));
        static unsigned char s_thr2_stack[16384] __attribute__((aligned(16)));
        unsigned int ptid = 0xFFFFFFFFu;   /* 内核按 int 写 4 字节，别拿 64 位读 */
        lx_long  thr;
        lx_long  self_local = 0x1234;
        lx_long  parent_tid = lx1(LX_GETTID, 0);
        lx_long  parent_pid = lx1(LX_GETPID, 0);
        lx_long  flags;

        flags = (lx_long)(LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES |
                          LX_CLONE_SIGHAND | LX_CLONE_THREAD |
                          LX_CLONE_SETTLS | LX_CLONE_PARENT_SETTID |
                          LX_CLONE_CHILD_SETTID | LX_CLONE_CHILD_CLEARTID);

        g_thr_ctid = 0x7eadbeef;      /* 非零，好观察 CLEARTID 有没有真的清 */
        g_thr_ran  = 0;
        g_thr_tid  = 0;
        g_thr_pid  = 0;
        g_thr_tp   = 0;

        thr = lx_clone_thread_call(flags,
                  (lx_long)(unsigned long)(s_thr_stack + sizeof(s_thr_stack)),
                  (lx_long)(unsigned long)&ptid,
                  (lx_long)(unsigned long)&g_thr_ctid,
                  THR_TLS_MAGIC, thread_body);
        check("clone(CLONE_THREAD) > 0", (thr > 0) ? 1 : 0, 1);
        check("thread PARENT_SETTID wrote the tid", (lx_long)ptid, thr);

        if (thr > 0)
        {
            /* pthread_join 的真实路径：CHILD_CLEARTID 把字清成 0 并 futex 唤醒。
             * 上限 2 s，避免唤醒丢了就把整台机器挂死（那样连 FAIL 都印不出来）。 */
            {
                long     ts[2];
                lx_long  dur[2];
                lx_long  wrc = 0;
                int      spun;

                dur[0] = 0;
                dur[1] = 0;
                if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                        (lx_long)(unsigned long)ts, 0) == 0)
                {
                    dur[0] = ts[0] + 2;
                    dur[1] = ts[1];
                }
                for (spun = 0; spun < 64; spun++)
                {
                    if (g_thr_ctid == 0)
                    {
                        wrc = 0;
                        break;
                    }
                    wrc = lx4(LX_FUTEX,
                              (lx_long)(unsigned long)&g_thr_ctid, LX_FUTEX_WAIT,
                              g_thr_ctid, (lx_long)(unsigned long)dur);
                    if ((wrc == 0) || (wrc != -LX_EAGAIN))
                    {
                        break;
                    }
                }
                check("futex WAIT on CHILD_CLEARTID woke with 0", wrc, 0);
            }
            check("CHILD_CLEARTID cleared to 0", g_thr_ctid, 0);
            check("thread ran", g_thr_ran, 1);
            check("thread CHILD_SETTID saw its own tid", g_thr_ctid_self, thr);
            check("thread tid != parent tid", (g_thr_tid != parent_tid) ? 1 : 0, 1);
            check("thread getpid == tgid (parent pid)", g_thr_pid, parent_pid);
            check("parent getpid unchanged after thread", lx1(LX_GETPID, 0),
                  parent_pid);
            check("CLONE_SETTLS landed in $tp", g_thr_tp, THR_TLS_MAGIC);
            check("two stacks are distinct",
                  (g_thr_local_addr != (lx_long)(unsigned long)&self_local) ? 1 : 0, 1);
            check("parent local survived thread", self_local, 0x1234);
        }

        /* 第二个线程：同一组里再来一个，验证计数式退出（不是最后一个就别拆
         * 进程的 fd 表 / 槽）没有把组拆坏。 */
        g_thr2_ctid = 0x600df00d;
        {
            unsigned int ptid2 = 0xFFFFFFFFu;
            lx_long thr2;

            thr2 = lx_clone_thread_call(
                       (lx_long)(LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES |
                                 LX_CLONE_SIGHAND | LX_CLONE_THREAD |
                                 LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID),
                       (lx_long)(unsigned long)(s_thr2_stack + sizeof(s_thr2_stack)),
                       (lx_long)(unsigned long)&ptid2,
                       (lx_long)(unsigned long)&g_thr2_ctid,
                       0, thread_body2);
            check("second thread in group clone > 0", (thr2 > 0) ? 1 : 0, 1);

            if (thr2 > 0)
            {
                int n;

                for (n = 0; n < 400; n++)
                {
                    if (g_thr2_ctid == 0)
                    {
                        break;
                    }
                    (void)lx4(LX_FUTEX,
                              (lx_long)(unsigned long)&g_thr2_ctid, LX_FUTEX_WAIT,
                              g_thr2_ctid, 0);
                }
                check("second thread CHILD_CLEARTID cleared", g_thr2_ctid, 0);
            }
        }
        check("process alive after two thread exits", lx1(LX_GETPID, 0),
              parent_pid);

        /* 反假绿：参数门槛。这四条在开关打开时也必须逐条拒绝。 */
        check("CLONE_THREAD without CLONE_VM == -EINVAL",
              lx3(LX_CLONE,
                  (lx_long)(LX_CLONE_THREAD | LX_CLONE_SIGHAND | LX_CLONE_FS |
                            LX_CLONE_FILES),
                  (lx_long)(unsigned long)(s_thr_stack + sizeof(s_thr_stack)),
                  0), -LX_EINVAL);
        check("CLONE_THREAD child_stack=0 == -EINVAL",
              lx3(LX_CLONE,
                  (lx_long)(LX_CLONE_VM | LX_CLONE_THREAD), 0, 0), -LX_EINVAL);
        check("CLONE_THREAD misaligned stack == -EINVAL",
              lx3(LX_CLONE,
                  (lx_long)(LX_CLONE_VM | LX_CLONE_THREAD),
                  (lx_long)(unsigned long)(s_thr_stack + sizeof(s_thr_stack) - 8u),
                  0), -LX_EINVAL);
        check("CLONE_THREAD|CLONE_VFORK == -EINVAL",
              lx3(LX_CLONE,
                  (lx_long)(LX_CLONE_VM | LX_CLONE_THREAD | LX_CLONE_VFORK),
                  (lx_long)(unsigned long)(s_thr_stack + sizeof(s_thr_stack)),
                  0), -LX_EINVAL);

        /* fork 之后仍然能 fork：线程退出没有把 fd 表 / 进程槽提前拆掉。 */
        {
            lx_long p = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);

            if (p == 0)
            {
                (void)lx1(LX_EXIT, 5);
                for (;;)
                {
                }
            }
            if (p > 0)
            {
                lx_long st = -1;
                lx_long r = -1;

                for (;;)
                {
                    r = lx4(LX_WAIT4, p, (lx_long)(unsigned long)&st, 0, 0);
                    if ((r == p) || (r < 0))
                    {
                        break;
                    }
                }
                check("fork after threads reaped", r, p);
                check("fork-after-threads exit code 5", (st >> 8) & 0xFF, 5);
            }
        }
    }

    /* exit_group(94) 从线程发起：整组一起走，wait4 拿到的码是线程给的那个。 */
    {
        lx_long p = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);

        if (p == 0)
        {
            static unsigned char s_eg_stack[8192] __attribute__((aligned(16)));
            lx_long t = lx6(LX_CLONE,
                            (lx_long)(LX_CLONE_VM | LX_CLONE_FS |
                                      LX_CLONE_FILES | LX_CLONE_SIGHAND |
                                      LX_CLONE_THREAD),
                            (lx_long)(unsigned long)(s_eg_stack + sizeof(s_eg_stack)),
                            0, 0, 0, 0);

            if (t == 0)
            {
                (void)lx1(LX_EXIT_GROUP, 6);    /* 线程：整组退 */
            }
            if (t > 0)
            {
                for (;;)
                {
                    (void)lx1(LX_GETPID, 0);    /* 组长：等着被带走 */
                }
            }
            (void)lx1(LX_EXIT, 99);             /* 造线程失败 */
            for (;;)
            {
            }
        }
        if (p > 0)
        {
            lx_long st = -1;
            lx_long r = -1;

            for (;;)
            {
                r = lx4(LX_WAIT4, p, (lx_long)(unsigned long)&st, 0, 0);
                if ((r == p) || (r < 0))
                {
                    break;
                }
            }
            check("exit_group from a thread reaped the group", r, p);
            check("exit_group-from-thread code 6", (st >> 8) & 0xFF, 6);
        }
    }

    /* exit_group(94) 从组长发起：同样整组退出，码取组长的。 */
    {
        lx_long p = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);

        if (p == 0)
        {
            (void)lx1(LX_EXIT_GROUP, 7);
            for (;;)
            {
            }
        }
        if (p > 0)
        {
            lx_long st = -1;
            lx_long r = -1;

            for (;;)
            {
                r = lx4(LX_WAIT4, p, (lx_long)(unsigned long)&st, 0, 0);
                if ((r == p) || (r < 0))
                {
                    break;
                }
            }
            check("exit_group from the leader reaped", r, p);
            check("exit_group-from-leader code 7", (st >> 8) & 0xFF, 7);
        }
    }
#endif

#if defined(ARKRTOS_USE_APP_QUOTA) && (ARKRTOS_USE_APP_QUOTA == 1)
    /* ------------------------------------------------------------------
     * 第四批：SEC-G07 每 App 任务配额
     * ------------------------------------------------------------------
     * 判据只有一条：这个 App 的**活任务数**到上限之后，再 fork /
     * clone(CLONE_THREAD) 必须回 -EAGAIN，且回收之后立刻能再起。
     *
     * 口径（本板最要紧的两条限制）：
     *   1) qemu_loongarch64 的 ARKRTOS_PAGING_APPS_MAX 是 2，**并发进程最多
     *      两个**（父 + 1 子）。fork 被拒时 ERR_QUOTA 与 ERR_BUSY 都映射成
     *      -EAGAIN，所以「空间已满」时那次 fork 分不出是谁拦的。
     *   2) 线程**不占新地址空间**，所以线程是唯一能把「配额」与「空间用完」
     *      分开的杠杆。
     * 于是这一段用「进程 + 线程」混合把名额压满，再分别读三条：
     *   - 线程被拒（干净：不需要空间）；
     *   - fork 被拒而空间仍空闲（干净：不可能是 ERR_BUSY）；
     *   - 回收后立刻能再起（正控制）。
     * 门禁取 ARKRTOS_APP_MAX_TASKS = 3：父 + 1 子 + 1 线程刚好压满，而
     * 第三批线程段（父 + 1 线程 = 2）仍在上限内，两段互不干扰。配额计数
     * 逻辑与常量取值无关，默认档的 8 走同一条代码。
     */
#ifndef ARKRTOS_APP_MAX_TASKS
#define ARKRTOS_APP_MAX_TASKS 3
#endif
    {
        int      qp[2];
        lx_long  kid = -1;
        lx_long  thr1 = -1;
        lx_long  thr2 = -1;
        lx_long  rc;
        int      nreap = 0;
        int      i;

        check("quota pipe2 for the child barrier -> 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)qp, 0), 0);

        /* ① 进程先占一格：父 1 + 子 1 = 2。子进程关掉自己的写端后堵在管道
         *    读上 —— 配额数的是**活任务**，让它直接 exit 就测不到上限。 */
        kid = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (kid == 0)
        {
            char    b;
            long    park[2];
            lx_long rr;

            (void)lx1(LX_CLOSE, (lx_long)qp[1]);
            /* 本内核的管道读**不阻塞**：空且还有写端时回 -EAGAIN（见
             * LX_PIPE_BUF 上的注释）。所以「等数据」这一步得自己睡出来：
             * 每 100 ms 醒一次重试，读到 EOF(0) 才退出。睡的是内核自己
             * 记账的 futex 超时，不读用户态时钟。 */
            park[0] = 0;
            park[1] = 100000000L;       /* 100 ms */
            for (;;)
            {
                rr = lx3(LX_READ, (lx_long)qp[0],
                         (lx_long)(unsigned long)&b, 1);
                if (rr == 0)
                {
                    break;              /* 写端全关：EOF */
                }
                if ((rr < 0) && (rr != -LX_EAGAIN))
                {
                    break;              /* 其它错误：别在这里空转 */
                }
                (void)lx4(LX_FUTEX, (lx_long)(unsigned long)&g_q_park,
                          LX_FUTEX_WAIT, 0, (lx_long)(unsigned long)park);
            }
            (void)lx1(LX_EXIT, 4);
            for (;;)
            {
            }
        }
        check("quota fork under the limit > 0", (kid > 0) ? 1 : 0, 1);

        /* ② 再用一个线程把最后一格占掉：父 + 子 + 线程 = 3 = 上限。
         *   线程不需要新地址空间，所以下面几条的拒绝不可能来自空间耗尽。 */
        g_q_go   = 0;
        g_q_seen = 0;
        g_q_ctid = 0x0badf00d;
        thr1 = quota_thread_start((lx_long)(unsigned long)&g_q_ctid);
        check("quota thread fills the last slot > 0", (thr1 > 0) ? 1 : 0, 1);

        /* ③ 名额已满：fork 与线程都必须被拒。fork 这一条此刻空间 1 已被
         *    子进程占着（同时被配额与空间挡），线程这一条是干净的。 */
        check("quota fork over the limit == -EAGAIN -> -11",
              lx3(LX_CLONE, LX_SIGCHLD, 0, 0), -LX_EAGAIN);
        check("quota thread over the limit == -EAGAIN -> -11",
              quota_thread_nop(), -LX_EAGAIN);

        /* ④ 关写端 → 子进程读到 EOF 退出 → 活任务数掉回 2（父 + 线程1）。 */
        (void)lx1(LX_CLOSE, (lx_long)qp[1]);
        if (kid > 0)
        {
            lx_long st = -1;
            lx_long r;

            for (;;)
            {
                r = lx4(LX_WAIT4, kid, (lx_long)(unsigned long)&st, 0, 0);
                if ((r == kid) || (r < 0))
                {
                    break;
                }
            }
            if (r == kid)
            {
                nreap++;
            }
        }
        (void)lx1(LX_CLOSE, (lx_long)qp[0]);
        check("quota blocked child reaped -> 1", (lx_long)nreap, 1);

        /* ⑤ 再加一个线程压满（父 + 线程1 + 线程2 = 3），此时**空间 1 是空的**
         *    —— 这一次 fork 的拒绝只能是配额。这是全段最干净的一条读数。 */
        g_q2_go   = 0;
        g_q2_ctid = 0x0ddba11;
        thr2 = quota_thread_start2((lx_long)(unsigned long)&g_q2_ctid);
        check("quota second thread under the limit > 0", (thr2 > 0) ? 1 : 0, 1);
        check("quota fork blocked while a space is free == -EAGAIN",
              lx3(LX_CLONE, LX_SIGCHLD, 0, 0), -LX_EAGAIN);

        /* ⑥ 线程同时算额度：再要一个线程也必须被拒。 */
        check("quota third thread over the limit == -EAGAIN",
              quota_thread_nop(), -LX_EAGAIN);

        /* ⑦ 放行两个线程，按 pthread_join 的真实路径等 CHILD_CLEARTID 清零。
         *    上限 2 s：唤醒丢了就整台机器挂死，连 FAIL 都印不出来。 */
        quota_thread_join(&g_q_go, &g_q_ctid);
        quota_thread_join(&g_q2_go, &g_q2_ctid);
        check("quota threads joined (CHILD_CLEARTID cleared -> 0)",
              (g_q_ctid == 0 && g_q2_ctid == 0) ? 1 : 0, 1);

        /* ⑧ 额度与空间都回来了：立刻能再起一个进程，且退出码照传。 */
        rc = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (rc == 0)
        {
            (void)lx1(LX_EXIT, 3);
            for (;;)
            {
            }
        }
        check("quota fork after the quota was refilled > 0 -> 1",
              (rc > 0) ? 1 : 0, 1);
        if (rc > 0)
        {
            lx_long st = -1;
            lx_long r;

            for (;;)
            {
                r = lx4(LX_WAIT4, rc, (lx_long)(unsigned long)&st, 0, 0);
                if ((r == rc) || (r < 0))
                {
                    break;
                }
            }
            check("quota refilled child exit code 3 -> 3", (st >> 8) & 0xFF, 3);
        }
        (void)i;
    }
#endif

    if (g_fail == 0)
    {
        out("[fork] PASS: child ran in its own space, exit 42 reaped\n");
        (void)lx1(LX_EXIT, 0);
    }
    else
    {
        out_line3("[fork] FAIL: ", g_fail, " checks failed\n");
        (void)lx1(LX_EXIT, 1);
    }
    for (;;)
    {
    }
}
