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
#define LX_EXECVE           221     /* U-32：非组长线程发起应当被拒 */

#define LX_EINVAL           22
#define LX_ECHILD           10
#define LX_ENOSYS           38
#define LX_ENOENT           2

#define LX_CLOSE            57
#define LX_DUP              23
#define LX_PIPE2            59
#define LX_O_NONBLOCK       0x800       /* asm-generic 的 O_NONBLOCK 位 */
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
#define LX_FUTEX_LOCK_PI    6
#define LX_FUTEX_UNLOCK_PI  7
#define LX_FUTEX_TRYLOCK_PI 8
#define LX_FUTEX_WAIT_REQUEUE_PI 11
#define LX_FUTEX_CMP_REQUEUE_PI  12
#define LX_FUTEX_TID_MASK   0x3FFFFFFFul
#define LX_FUTEX_WAITERS    0x80000000ul
#define LX_FUTEX_OWNER_DIED 0x40000000ul
#define LX_SET_ROBUST_LIST  99
#define LX_GET_ROBUST_LIST  100
#define LX_EAGAIN           11
#define LX_EPERM            1
#define LX_ESRCH            3
#define LX_EDEADLK          35
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

/* 记录锁（fcntl 25，U-27）。asm-generic 的 F_* 与 l_type 取值，与内核
 * lx_fcntl_lock 认的那一组一致。 */
#define LX_FCNTL            25
#define LX_F_GETLK          5
#define LX_F_SETLK          6
#define LX_F_SETLKW         7
#define LX_RDLCK            0
#define LX_WRLCK            1
#define LX_UNLCK            2
#define LX_SEEK_SET         0
#define LX_SEEK_CUR         1
#define LX_SEEK_END         2
#define LK_PATH             "/mnt/ram/lockprobe"
#define LK_SIZE             4096

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

/* 跨进程阻塞管道那一段的缓冲（放静态区：那段要 fork，子进程的信号帧与
 * 内核链都压在同一根用户栈上，几百字节的局部数组没有必要去挤它）。 */
static char g_pb_fill[256];
static char g_pb_out[300];
static char g_pb_rd[128];

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

/* U-32：从**非组长线程**发起 execve。Linux 会收掉整组、调用者的 tid 变成
 * 进程号；本内核的 tid 就是任务号，exec 换不掉，所以明确回 -EINVAL。
 * 路径故意用不存在的 /no/such/prog：万一闸门被删掉，内核会走到装载器回
 * -ENOENT（而不是把镜像换掉），判据因此能干净地区分「有闸门」与「没闸门」。 */
static volatile lx_long g_ex_rc;
static volatile lx_long g_ex_ctid;

static void exec_thread_body(void)
{
    g_ex_rc = lx2(LX_EXECVE, (lx_long)(unsigned long)"/no/such/prog", 0);
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

/* ------------------------------------------------------------------
 * RUNNING 状态下的节拍抢占不得碰线程（回归夹具）
 *
 * 旧缺陷：内核给 clone(2) 线程的 TCB 记了 stack_bottom = 栈顶、
 * stack_size = 0（栈界未知），而调度的「psp < stack_bottom 即栈溢出」
 * 一条判据不看 stack_size。于是线程只要在 RUNNING 状态下被节拍切走一次，
 * 就被当成栈溢出**静默摘除**。
 *
 * 单靠一个忙等线程不足以**稳定**复现：节拍到期只有在同优先级就绪队列里
 * 还有别的任务时才会真的换人（否则 arkrtos_sched_next 选回自己，压根不进
 * arkrtos_sched_activate），于是命中率取决于忙等窗口是否恰好压到别的周期
 * 任务唤醒点 —— 实测只有 12%~37%。
 * 这里放**两个同优先级线程一起忙等**：时间片（10 拍 = 5 ms）一到必然互切，
 * 每次切换都是从 RUNNING 状态走 sched_activate，命中率 100%。
 * 被误杀时那个线程的 CHILD_CLEARTID 永远不会被清零，join 循环超时，判据红。
 * ------------------------------------------------------------------ */
static volatile lx_long g_pre_ran;
static volatile lx_long g_pre2_ran;
static volatile lx_long g_pre_ctid;
static volatile lx_long g_pre2_ctid;
static unsigned char s_pre_stk[16384] __attribute__((aligned(16)));
static unsigned char s_pre2_stk[16384] __attribute__((aligned(16)));

static lx_long preempt_spin(void)
{
    /* 纯用户态循环，一次系统调用都不发：线程在整个循环里都是 RUNNING。
     * volatile 累加是为了让 -O2 不能把求和折叠成一个常数（折叠了就没有
     * RUNNING 窗口）。4e6 轮在本板约几十毫秒，跨几十个时间片。 */
    volatile unsigned long spin = 0ul;
    lx_long i;

    for (i = 0; i < 4000000; i++)
    {
        spin = spin + (unsigned long)i;
    }
    return (lx_long)spin;
}

static void preempt_body(void)
{
    g_pre_ran = preempt_spin();
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

static void preempt_body2(void)
{
    g_pre2_ran = preempt_spin();
    (void)lx1(LX_EXIT, 0);
    for (;;)
    {
    }
}

/* 按 pthread_join 的真实路径等 CLEARTID 被清成 0（绝对时刻上限 2 s）。 */
static void pre_join(volatile lx_long *p_ctid)
{
    long    ts[2];
    lx_long dur[2];
    int     spun;

    dur[0] = 0;
    dur[1] = 0;
    if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
            (lx_long)(unsigned long)ts, 0) == 0)
    {
        /* 500 ms 足够（线程几十毫秒就跑完）；不给 2 s 是为了让「线程被误杀」
         * 这条失败分支也在夹具的 3000 ms 总预算里跑得完，能打出 FAIL 行，
         * 而不是把整段测试拖到超时、看起来像「少了几条 check」。 */
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

/* ------------------------------------------------------------------
 * 记录锁（fcntl F_GETLK / F_SETLK / F_SETLKW，U-27）
 *
 * asm-generic 的 64 位 struct flock：l_type@0 / l_whence@2 / l_start@8 /
 * l_len@16 / l_pid@24，整块 32 字节。LA64 上 long 就是 64 位，所以下面这个
 * 声明与内核读的布局一致；每次填之前先把整块清 0，不让 padding 影响读数。
 * ------------------------------------------------------------------ */
struct lk_flock
{
    short l_type;
    short l_whence;
    long  l_start;
    long  l_len;
    int   l_pid;
};

static void lk_set(struct lk_flock *f, short type, short whence,
                   long start, long len)
{
    unsigned char *p = (unsigned char *)f;
    unsigned int   i;

    for (i = 0u; i < (unsigned int)sizeof(*f); i++)
    {
        p[i] = 0u;
    }
    f->l_type   = type;
    f->l_whence = whence;
    f->l_start  = start;
    f->l_len    = len;
    f->l_pid    = -12345;       /* GETLK 无冲突时内核不许动它 */
}

static lx_long lk_call(lx_long fd, lx_long cmd, struct lk_flock *f)
{
    return lx3(LX_FCNTL, fd, cmd, (lx_long)(unsigned long)f);
}

/* 忙等一段时间：本内核的用户态没有 nanosleep，而这一段要的就是「父进程在
 * 一方把锁握着不放」的确定性，不依赖调度顺序。mono_ms 读到 -1（QEMU 上
 * clock_gettime 的已知偶发）时靠 guard 兜底，绝不死循环。 */
static void lk_busy_ms(long ms)
{
    long tp    = mono_ms();
    long guard = 0;

    while (((mono_ms() - tp) < ms) && (guard < 2000000))
    {
        guard++;
    }
}

/* 子进程侧：所有断言都在这里。g_fail 是 fork 之后各自的副本，子进程里的
 * FAIL 不会传回父进程 —— 但每条 check 都直接写一行到控制台，门禁按行数
 * 和 FAIL 行判，所以子进程的失败一样逃不掉。 */
static void lk_child(int *pc, int *cp, lx_long pfd)
{
    struct lk_flock fl;
    lx_long         me;
    lx_long         rc;
    long            t0;
    long            t1;
    char            b;

    me = lx1(LX_GETPID, 0);

    /* ① 父进程的写锁对子进程是真的冲突 */
    lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
    check("lock child SETLK on parent lock == -EAGAIN",
          lk_call(pfd, LX_F_SETLK, &fl), -LX_EAGAIN);

    /* ② 不相交的区间照常加得上（锁按区间，不按整个文件） */
    lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 200, 100);
    check("lock child disjoint SETLK", lk_call(pfd, LX_F_SETLK, &fl), 0);

    /* ③ SEEK_END 换算：锁到文件尾 [4086,4096)。父进程稍后用 SEEK_SET 问
     *    同一段，命中同一把锁 —— 换算对不对由**另一个进程**读出来。 */
    lk_set(&fl, LX_WRLCK, LX_SEEK_END, -10, 10);
    check("lock child SEEK_END SETLK", lk_call(pfd, LX_F_SETLK, &fl), 0);

    (void)lx3(LX_WRITE, (lx_long)cp[1], (lx_long)(unsigned long)"D", 1);
    (void)lx3(LX_READ, (lx_long)pc[0], (lx_long)(unsigned long)&b, 1);

    /* ④ F_GETLK 读别人的锁：类型 / 进程号 / 绝对区间 / 归一后的 whence */
    lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
    check("lock child GETLK sees parent lock", lk_call(pfd, LX_F_GETLK, &fl), 0);
    check("lock child GETLK type is WRLCK", (lx_long)fl.l_type, LX_WRLCK);
    check("lock child GETLK pid is not self", (fl.l_pid != (int)me) ? 1 : 0, 1);
    check("lock child GETLK start is absolute 0", fl.l_start, 0);
    check("lock child GETLK len is 100", fl.l_len, 100);
    check("lock child GETLK whence is SEEK_SET", (lx_long)fl.l_whence, LX_SEEK_SET);

    /* ⑤ F_SETLKW 真的挂起：先报「马上要睡」，父进程 100 ms 之后才解锁。
     *    这不是「加锁失败就立刻返回」——返回值必须是 0，而且墙钟要量得出来。 */
    lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
    (void)lx3(LX_WRITE, (lx_long)cp[1], (lx_long)(unsigned long)"P", 1);
    t0 = mono_ms();
    rc = lk_call(pfd, LX_F_SETLKW, &fl);
    t1 = mono_ms();
    check("lock child F_SETLKW rc", rc, 0);
    check("lock waiter blocked >= 40 ms",
          ((t0 > 0) && (t1 >= t0) && ((t1 - t0) >= 40)) ? 1 : 0, 1);

    (void)lx3(LX_WRITE, (lx_long)cp[1], (lx_long)(unsigned long)"A", 1);
    (void)lx3(LX_READ, (lx_long)pc[0], (lx_long)(unsigned long)&b, 1);

    /* ⑥ close 按文件释放：这个 fd 一关，本文件上的三把锁一起消失 */
    check("lock child close fd", lx1(LX_CLOSE, pfd), 0);
    (void)lx3(LX_WRITE, (lx_long)cp[1], (lx_long)(unsigned long)"Z", 1);
}

/* ------------------------------------------------------------------
 * U-28 调度策略（sched_* 十二个号）的 ABI 往返读数
 *
 * 这一段只证「号、参数、错误码真的过了内核又回来了」，不证选择次序：
 * EDF 谁先跑依赖墙钟与调度抖动，不是可证伪读数。选择次序、节流与补回
 * 由宿主用例 kernelsrc/tests/test_task_host.c 的 test_deadline_class
 * 用可控的 arkrtos_kernel_tick 断言。这里只证明翻译层没把 Linux 的
 * 语义翻错 —— 两处读数合起来才是完整的证据。
 * ------------------------------------------------------------------ */
#if defined(ARKRTOS_USE_LINUX_SCHED) && (ARKRTOS_USE_LINUX_SCHED == 1)

#define LX_SCHED_SETPARAM       118
#define LX_SCHED_SETSCHEDULER   119
#define LX_SCHED_GETSCHEDULER   120
#define LX_SCHED_GETPARAM       121
#define LX_SCHED_SETAFFINITY    122
#define LX_SCHED_GETAFFINITY    123
#define LX_SCHED_YIELD          124
#define LX_SCHED_PRIO_MAX       125
#define LX_SCHED_PRIO_MIN       126
#define LX_SCHED_RR_INTERVAL    127
#define LX_SCHED_SETATTR        274
#define LX_SCHED_GETATTR        275

#define LX_SCHED_OTHER          0
#define LX_SCHED_FIFO           1
#define LX_SCHED_RR             2
#define LX_SCHED_DEADLINE       6

#define LX_EBUSY                16
#define LX_EFAULT               14

/* struct sched_attr 的字段偏移（VER0 = 48 字节，VER1 = 56 字节）。
 * 内核逐字段按字节读写，这里也按字节填/取，不对用户指针做结构体强转。 */
#define SA_OFF_SIZE      0ul
#define SA_OFF_POLICY    4ul
#define SA_OFF_FLAGS     8ul
#define SA_OFF_NICE      16ul
#define SA_OFF_PRIO      20ul
#define SA_OFF_RUNTIME   24ul
#define SA_OFF_DEADLINE  32ul
#define SA_OFF_PERIOD    40ul
#define SA_OFF_UTIL0     48ul
#define SA_OFF_UTIL1     52ul

static void sched_check(const char *what, lx_long got, lx_long want)
{
    char  buf[200];
    char *p = buf;

    p = chk_str(p, "[sched] ");
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

static void sa_put32(unsigned char *b, unsigned long off, unsigned long v)
{
    b[off + 0ul] = (unsigned char)(v & 0xFFul);
    b[off + 1ul] = (unsigned char)((v >> 8) & 0xFFul);
    b[off + 2ul] = (unsigned char)((v >> 16) & 0xFFul);
    b[off + 3ul] = (unsigned char)((v >> 24) & 0xFFul);
}

static void sa_put64(unsigned char *b, unsigned long off, unsigned long long v)
{
    sa_put32(b, off, (unsigned long)(v & 0xFFFFFFFFull));
    sa_put32(b, off + 4ul, (unsigned long)(v >> 32));
}

static unsigned long sa_get32(const unsigned char *b, unsigned long off)
{
    return (unsigned long)b[off + 0ul] |
           ((unsigned long)b[off + 1ul] << 8) |
           ((unsigned long)b[off + 2ul] << 16) |
           ((unsigned long)b[off + 3ul] << 24);
}

static lx_long sa_get_s32(const unsigned char *b, unsigned long off)
{
    return (lx_long)(int)(unsigned int)sa_get32(b, off);
}

static unsigned long long sa_get64(const unsigned char *b, unsigned long off)
{
    return (unsigned long long)sa_get32(b, off) |
           ((unsigned long long)sa_get32(b, off + 4ul) << 32);
}

static void sched_probe(void)
{
    unsigned long long sa8[8];
    unsigned char     *sa = (unsigned char *)sa8;
    int                sp[2];          /* struct sched_param{int sched_priority;} */
    unsigned long      ts[2];          /* struct timespec{sec, nsec}（LP64） */
    unsigned long      mask = 0ul;
    int                i;

    for (i = 0; i < 8; i++)
    {
        sa8[i] = 0ull;
    }

    /* ---- 初值：ELF 任务是 OTHER(0)，priority 0 ---- */
    sched_check("getscheduler default",
                lx1(LX_SCHED_GETSCHEDULER, 0), LX_SCHED_OTHER);
    sp[0] = 0x7FFF;
    sched_check("getparam default",
                lx2(LX_SCHED_GETPARAM, 0, (lx_long)(unsigned long)sp), 0);
    sched_check("getparam default priority is 0", (lx_long)sp[0], 0);

    /* ---- 优先级上下界：FIFO/RR 是 99/1，其余策略 0/0 ---- */
    sched_check("prio_max FIFO", lx1(LX_SCHED_PRIO_MAX, LX_SCHED_FIFO), 99);
    sched_check("prio_min FIFO", lx1(LX_SCHED_PRIO_MIN, LX_SCHED_FIFO), 1);
    sched_check("prio_max RR", lx1(LX_SCHED_PRIO_MAX, LX_SCHED_RR), 99);
    sched_check("prio_max OTHER", lx1(LX_SCHED_PRIO_MAX, LX_SCHED_OTHER), 0);
    sched_check("prio_max unknown policy",
                lx1(LX_SCHED_PRIO_MAX, 9), -LX_EINVAL);

    /* ---- FIFO：88 -> 内核优先级 12（与 ELF 任务默认同格） ---- */
    sp[0] = 88;
    sched_check("setscheduler FIFO 88",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_FIFO,
                    (lx_long)(unsigned long)sp), 0);
    sched_check("getscheduler after FIFO",
                lx1(LX_SCHED_GETSCHEDULER, 0), LX_SCHED_FIFO);
    sp[0] = 0;
    sched_check("getparam FIFO",
                lx2(LX_SCHED_GETPARAM, 0, (lx_long)(unsigned long)sp), 0);
    sched_check("getparam FIFO priority is 88", (lx_long)sp[0], 88);

    /* setparam 只改优先级，策略保持 */
    sp[0] = 80;
    sched_check("setparam FIFO 80",
                lx2(LX_SCHED_SETPARAM, 0, (lx_long)(unsigned long)sp), 0);
    sp[0] = 0;
    (void)lx2(LX_SCHED_GETPARAM, 0, (lx_long)(unsigned long)sp);
    sched_check("setparam took effect", (lx_long)sp[0], 80);

    /* ---- RR：轮转量 = 时间片 10 拍 x 500 us = 5 ms ---- */
    sp[0] = 90;
    sched_check("setscheduler RR 90",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_RR,
                    (lx_long)(unsigned long)sp), 0);
    sched_check("getscheduler after RR",
                lx1(LX_SCHED_GETSCHEDULER, 0), LX_SCHED_RR);
    ts[0] = 0xFFFFul;
    ts[1] = 0xFFFFul;
    sched_check("rr_get_interval",
                lx2(LX_SCHED_RR_INTERVAL, 0, (lx_long)(unsigned long)ts), 0);
    sched_check("rr interval seconds is 0", (lx_long)ts[0], 0);
    sched_check("rr interval nsec is 5000000", (lx_long)ts[1], 5000000);

    /* ---- 策略面错误：越界优先级 / 未知策略 / deadline 走错号 ---- */
    sp[0] = 0;
    sched_check("setscheduler FIFO priority 0 is refused",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_FIFO,
                    (lx_long)(unsigned long)sp), -LX_EINVAL);
    sp[0] = 1;
    sched_check("setscheduler unknown policy is refused",
                lx3(LX_SCHED_SETSCHEDULER, 0, 9,
                    (lx_long)(unsigned long)sp), -LX_EINVAL);
    sched_check("setscheduler DEADLINE has no parameter form",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_DEADLINE,
                    (lx_long)(unsigned long)sp), -LX_EINVAL);
    sched_check("setscheduler null param -> EFAULT",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_FIFO, 0), -LX_EFAULT);
    sp[0] = 10;
    sched_check("setscheduler unknown pid -> ESRCH",
                lx3(LX_SCHED_SETSCHEDULER, 9999, LX_SCHED_FIFO,
                    (lx_long)(unsigned long)sp), -LX_ESRCH);

    /* ---- OTHER：priority 必须是 0，nice 走 setattr ---- */
    sp[0] = 0;
    sched_check("setscheduler OTHER",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_OTHER,
                    (lx_long)(unsigned long)sp), 0);
    sched_check("getscheduler after OTHER",
                lx1(LX_SCHED_GETSCHEDULER, 0), LX_SCHED_OTHER);
    sp[0] = 5;
    sched_check("setparam on OTHER with nonzero priority is refused",
                lx2(LX_SCHED_SETPARAM, 0, (lx_long)(unsigned long)sp), -LX_EINVAL);

    /* ---- setattr / getattr：struct sched_attr 的逐字段往返 ---- */
    sa_put32(sa, SA_OFF_SIZE, 48ul);
    sa_put32(sa, SA_OFF_POLICY, LX_SCHED_OTHER);
    sa_put32(sa, SA_OFF_NICE, (unsigned long)(unsigned int)(int)(-5));
    sched_check("setattr OTHER nice -5",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), 0);
    for (i = 0; i < 8; i++)
    {
        sa8[i] = 0ull;
    }
    sched_check("getattr OTHER",
                lx4(LX_SCHED_GETATTR, 0, (lx_long)(unsigned long)sa, 48, 0), 0);
    sched_check("getattr writes back the size", (lx_long)sa_get32(sa, SA_OFF_SIZE), 48);
    sched_check("getattr policy round-trips",
                (lx_long)sa_get32(sa, SA_OFF_POLICY), LX_SCHED_OTHER);
    sched_check("getattr nice round-trips", sa_get_s32(sa, SA_OFF_NICE), -5);
    sched_check("getattr leaves sched_flags 0",
                (lx_long)sa_get64(sa, SA_OFF_FLAGS), 0);

    /* VER1：多出来的两个 util 字段必须有确定值（本内核不做利用率跟踪 -> 0） */
    for (i = 0; i < 8; i++)
    {
        sa8[i] = 0xAAAAAAAAAAAAAAAAull;
    }
    sched_check("getattr VER1",
                lx4(LX_SCHED_GETATTR, 0, (lx_long)(unsigned long)sa, 56, 0), 0);
    sched_check("getattr VER1 reports size 56",
                (lx_long)sa_get32(sa, SA_OFF_SIZE), 56);
    sched_check("getattr VER1 util_lo is 0",
                (lx_long)sa_get32(sa, SA_OFF_UTIL0), 0);
    sched_check("getattr VER1 util_hi is 0",
                (lx_long)sa_get32(sa, SA_OFF_UTIL1), 0);

    /* 错误面：size 太小 / 系统调用 flags 非 0 / 结构里 sched_flags 非 0 */
    sa_put32(sa, SA_OFF_SIZE, 47ul);
    sched_check("setattr size 47 -> EINVAL",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
    sched_check("getattr size 47 -> EINVAL",
                lx4(LX_SCHED_GETATTR, 0, (lx_long)(unsigned long)sa, 47, 0), -LX_EINVAL);
    sched_check("getattr sysflags nonzero -> EINVAL",
                lx4(LX_SCHED_GETATTR, 0, (lx_long)(unsigned long)sa, 48, 1), -LX_EINVAL);
    sa_put32(sa, SA_OFF_SIZE, 48ul);
    sa_put32(sa, SA_OFF_POLICY, LX_SCHED_OTHER);
    sa_put64(sa, SA_OFF_FLAGS, 1ull);
    sched_check("setattr sched_flags nonzero -> EINVAL",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
    sa_put64(sa, SA_OFF_FLAGS, 0ull);
    sa_put64(sa, SA_OFF_RUNTIME, 1000000ull);
    sched_check("setattr OTHER with a runtime is refused",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
    sched_check("setattr null attr -> EFAULT",
                lx3(LX_SCHED_SETATTR, 0, 0, 0), -LX_EFAULT);

    /* ---- DEADLINE：只有 setattr 能设，且过准入 ---- */
    sa_put64(sa, SA_OFF_RUNTIME, 0ull);
    sa_put32(sa, SA_OFF_POLICY, LX_SCHED_DEADLINE);
    sa_put32(sa, SA_OFF_PRIO, 1ul);
    sa_put64(sa, SA_OFF_RUNTIME, 1000000ull);
    sa_put64(sa, SA_OFF_DEADLINE, 2000000ull);
    sa_put64(sa, SA_OFF_PERIOD, 100000000ull);
    sched_check("setattr DEADLINE with a priority is refused",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
    sa_put32(sa, SA_OFF_PRIO, 0ul);
    sa_put64(sa, SA_OFF_DEADLINE, 500000ull);       /* runtime > deadline */
    sched_check("setattr DEADLINE runtime > deadline is refused",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
    sa_put64(sa, SA_OFF_DEADLINE, 1000000ull);     /* 1 ms / 1 ms / 1 ms = 100% */
    sa_put64(sa, SA_OFF_PERIOD, 1000000ull);
#if defined(ARKRTOS_USE_SCHED_DEADLINE) && (ARKRTOS_USE_SCHED_DEADLINE == 1)
    sched_check("setattr DEADLINE 100% bandwidth -> EBUSY",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EBUSY);
#else
    /* 内核没有 deadline 类：这个号必须如实回 -EINVAL，不能假装设上了。 */
    sched_check("setattr DEADLINE 100% with no deadline class -> EINVAL",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
#endif
    sa_put64(sa, SA_OFF_DEADLINE, 2000000ull);     /* 1 ms / 2 ms / 100 ms = 1% */
    sa_put64(sa, SA_OFF_PERIOD, 100000000ull);
#if defined(ARKRTOS_USE_SCHED_DEADLINE) && (ARKRTOS_USE_SCHED_DEADLINE == 1)
    sched_check("setattr DEADLINE 1ms/2ms/100ms is admitted",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), 0);
#else
    sched_check("setattr DEADLINE 1ms/2ms/100ms with no class -> EINVAL",
                lx3(LX_SCHED_SETATTR, 0, (lx_long)(unsigned long)sa, 0), -LX_EINVAL);
#endif
    for (i = 0; i < 8; i++)
    {
        sa8[i] = 0ull;
    }
    sched_check("getattr DEADLINE",
                lx4(LX_SCHED_GETATTR, 0, (lx_long)(unsigned long)sa, 48, 0), 0);
#if defined(ARKRTOS_USE_SCHED_DEADLINE) && (ARKRTOS_USE_SCHED_DEADLINE == 1)
    sched_check("getattr policy is DEADLINE",
                (lx_long)sa_get32(sa, SA_OFF_POLICY), LX_SCHED_DEADLINE);
    sched_check("getattr DEADLINE runtime round-trips",
                (lx_long)sa_get64(sa, SA_OFF_RUNTIME), 1000000);
    sched_check("getattr DEADLINE deadline round-trips",
                (lx_long)sa_get64(sa, SA_OFF_DEADLINE), 2000000);
    sched_check("getattr DEADLINE period round-trips",
                (lx_long)sa_get64(sa, SA_OFF_PERIOD), 100000000);
#else
    /* 类没开：策略仍是刚才设的 OTHER，三个时间参数必须留 0（不是垃圾）。 */
    sched_check("getattr policy stays OTHER with no deadline class",
                (lx_long)sa_get32(sa, SA_OFF_POLICY), LX_SCHED_OTHER);
    sched_check("getattr runtime stays 0 with no deadline class",
                (lx_long)sa_get64(sa, SA_OFF_RUNTIME), 0);
    sched_check("getattr deadline stays 0 with no deadline class",
                (lx_long)sa_get64(sa, SA_OFF_DEADLINE), 0);
    sched_check("getattr period stays 0 with no deadline class",
                (lx_long)sa_get64(sa, SA_OFF_PERIOD), 0);
#endif

    /* 从 deadline 类转出：带宽必须释放，策略必须换掉 */
    sp[0] = 88;
    sched_check("setscheduler FIFO leaves the deadline class",
                lx3(LX_SCHED_SETSCHEDULER, 0, LX_SCHED_FIFO,
                    (lx_long)(unsigned long)sp), 0);
    sched_check("policy is FIFO after leaving DEADLINE",
                lx1(LX_SCHED_GETSCHEDULER, 0), LX_SCHED_FIFO);
    for (i = 0; i < 8; i++)
    {
        sa8[i] = 0ull;
    }
    (void)lx4(LX_SCHED_GETATTR, 0, (lx_long)(unsigned long)sa, 48, 0);
    sched_check("the deadline triple is gone",
                (lx_long)sa_get64(sa, SA_OFF_RUNTIME), 0);

    /* ---- 亲核性：单核，掩码只认 bit0 ---- */
    mask = 1ul;
    sched_check("setaffinity CPU0",
                lx3(LX_SCHED_SETAFFINITY, 0, 8, (lx_long)(unsigned long)&mask), 0);
    mask = 0ul;
    sched_check("getaffinity",
                lx3(LX_SCHED_GETAFFINITY, 0, 8, (lx_long)(unsigned long)&mask), 0);
    sched_check("getaffinity reports CPU0", (lx_long)mask, 1);
    mask = 0ul;
    sched_check("setaffinity empty mask -> EINVAL",
                lx3(LX_SCHED_SETAFFINITY, 0, 8, (lx_long)(unsigned long)&mask), -LX_EINVAL);
    mask = 1ul;
    sched_check("setaffinity short len -> EINVAL",
                lx3(LX_SCHED_SETAFFINITY, 0, 4, (lx_long)(unsigned long)&mask), -LX_EINVAL);
    sched_check("getaffinity short len -> EINVAL",
                lx3(LX_SCHED_GETAFFINITY, 0, 4, (lx_long)(unsigned long)&mask), -LX_EINVAL);
    sched_check("getaffinity unknown pid -> ESRCH",
                lx3(LX_SCHED_GETAFFINITY, 9999, 8, (lx_long)(unsigned long)&mask), -LX_ESRCH);

    /* ---- sched_yield ---- */
    sched_check("sched_yield", lx1(LX_SCHED_YIELD, 0), 0);
}
#endif /* ARKRTOS_USE_LINUX_SCHED */

/* ------------------------------------------------------------------
 * U-29 异步 I/O（fcntl F_SETOWN / F_GETOWN + O_ASYNC + SIGIO）
 *
 * 这一段证「登记 -> 开关 -> 投递」三步真的过了内核：
 *   1) 控制台本体没有可登记的主人：F_GETOWN 回 0，F_SETOWN 回 -EINVAL；
 *   2) 管道读端 F_SETOWN 0 是「认给自己」，F_GETOWN 原样读回任务号；
 *      进程组（负值）一律 -EINVAL，不假装支持；
 *   3) O_ASYNC 置位后写一个字节，SIGIO(29) 必须在这次 write 的返回路径
 *      上进 handler；rt_sigreturn 之后 write 自己的返回值仍是字节数 ——
 *      信号既不吞返回值也不吞数据；
 *   4) 清掉 O_ASYNC 后同一条管道不再投递（看命中数不变，不能只看 write
 *      返回 1）；
 *   5) dup 出来的是同一份 file description：主人与 O_ASYNC 共享；
 *   6) 写端全关（EOF）也算「变可读」，同样投一次。
 *
 * 默认动作（不装 handler = 结束任务）不在这里做：那会把本任务中途杀掉，
 * 后面的读数全丢；它由宿主用例 t_async_io 第 7 节断言。
 * ------------------------------------------------------------------ */
#if defined(ARKRTOS_USE_LINUX_SIGIO) && (ARKRTOS_USE_LINUX_SIGIO == 1)

#define LX_F_GETFL          3
#define LX_F_SETFL          4
#define LX_F_SETOWN         8
#define LX_F_GETOWN         9
#define LX_O_ASYNC          0x2000
#define LX_SIGIO            29
#define LX_SIGPIPE          13
#define LX_EPIPE            32
#define LX_SA_SIGINFO       4ul

static int     s_sio_hits;
static lx_long s_sio_signo;
static void   *s_sio_si;
static void   *s_sio_uc;
static int     s_pipe_hits;
static void   *s_pipe_si;

/* U-31：SA_SIGINFO 的 handler 是三参的。siginfo_t / ucontext_t 的字段偏移
 * 按 Linux loongarch64 UAPI 的布局读（si_signo 0、si_errno 4、si_code 8、
 * si_pid 16、si_uid 20、si_band 16、si_fd 24；uc_sigmask 40、
 * uc_mcontext.pc 168）。这里只读，不改。 */
static void on_sigio(unsigned long signo, void *si, void *uc)
{
    s_sio_hits++;
    s_sio_signo = (lx_long)signo;
    s_sio_si = si;
    s_sio_uc = uc;
}

static void on_sigpipe(unsigned long signo, void *si, void *uc)
{
    (void)uc;
    s_pipe_hits++;
    s_pipe_si = si;
}

static lx_long si_i32(const void *si, int off)
{
    return (lx_long)(*(const int *)((const char *)si + off));
}

static lx_long si_i64(const void *si, int off)
{
    return (lx_long)(*(const lx_long *)((const char *)si + off));
}

static void sio_check(const char *what, lx_long got, lx_long want)
{
    char  buf[200];
    char *p = buf;

    p = chk_str(p, "[sio] ");
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

static void sio_probe(void)
{
    act_t   act;
    act_t   old;
    int     fds[2];
    char    b[2];
    lx_long pid = lx1(LX_GETPID, 0);
    lx_long d;
    lx_long hits0;

    /* ---- 1) 控制台本体：没有可登记的主人 ---- */
    sio_check("getown on console is 0",
              lx3(LX_FCNTL, 1, LX_F_GETOWN, 0), 0);
    sio_check("setown on console -> EINVAL",
              lx3(LX_FCNTL, 1, LX_F_SETOWN, 0), -LX_EINVAL);

    /* ---- 2) 管道读端的登记语义 ---- */
    fds[0] = -1;
    fds[1] = -1;
    sio_check("pipe2",
              lx2(LX_PIPE2, (lx_long)(unsigned long)fds, LX_O_NONBLOCK), 0);
    sio_check("setown to self",
              lx3(LX_FCNTL, fds[0], LX_F_SETOWN, 0), 0);
    sio_check("getown reads back the task id",
              lx3(LX_FCNTL, fds[0], LX_F_GETOWN, 0), pid);
    sio_check("setown process group -> EINVAL",
              lx3(LX_FCNTL, fds[0], LX_F_SETOWN, -7), -LX_EINVAL);

    /* ---- 3) O_ASYNC 置位，dup 共享同一份 file description ---- */
    sio_check("setfl O_ASYNC",
              lx3(LX_FCNTL, fds[0], LX_F_SETFL,
                  LX_O_NONBLOCK | LX_O_ASYNC), 0);
    sio_check("getfl reports O_ASYNC",
              ((lx3(LX_FCNTL, fds[0], LX_F_GETFL, 0) & LX_O_ASYNC) != 0) ? 1 : 0,
              1);
    d = lx1(LX_DUP, fds[0]);
    sio_check("dup read end ok",
              ((d > 2) && (d != fds[0])) ? 1 : 0, 1);
    sio_check("dup shares the owner",
              lx3(LX_FCNTL, d, LX_F_GETOWN, 0), pid);
    sio_check("dup shares O_ASYNC",
              ((lx3(LX_FCNTL, d, LX_F_GETFL, 0) & LX_O_ASYNC) != 0) ? 1 : 0, 1);

    /* ---- 4) 投递：write 的返回路径上进 handler ---- */
    act.handler = (unsigned long)(unsigned long)&on_sigio;
    act.flags = LX_SA_SIGINFO;
    act.restorer = (unsigned long)(unsigned long)&fork_sig_restorer;
    act.mask = 0ul;
    old.handler = SIG_DFL;
    old.flags = 0ul;
    old.restorer = 0ul;
    old.mask = 0ul;
    sio_check("rt_sigaction(SIGIO) rc",
              lx4(LX_RT_SIGACTION, LX_SIGIO,
                  (lx_long)(unsigned long)&act,
                  (lx_long)(unsigned long)&old, 8), 0);

    hits0 = s_sio_hits;
    sio_check("write after O_ASYNC returns the byte",
              lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)"i", 1), 1);
    sio_check("SIGIO handler ran", s_sio_hits - hits0, 1);
    sio_check("SIGIO signo is 29", s_sio_signo, LX_SIGIO);
    sio_check("SIGIO siginfo pointer is set",
              (s_sio_si != (void *)0) ? 1 : 0, 1);
    sio_check("SIGIO si_signo", si_i32(s_sio_si, 0), LX_SIGIO);
    sio_check("SIGIO si_code is POLL_IN", si_i32(s_sio_si, 8), 1);
    sio_check("SIGIO si_band", si_i64(s_sio_si, 16), 0x41);
    sio_check("SIGIO si_fd", si_i32(s_sio_si, 24), (lx_long)fds[0]);
    sio_check("SIGIO ucontext pc is non-zero",
              ((s_sio_uc != (void *)0) &&
               (si_i64(s_sio_uc, 168) != 0)) ? 1 : 0, 1);
    b[0] = 0;
    b[1] = 0;
    sio_check("read still returns the byte",
              lx3(LX_READ, fds[0], (lx_long)(unsigned long)b, 1), 1);
    sio_check("the byte survived", (lx_long)(unsigned char)b[0], (lx_long)'i');

    /* ---- 5) 清掉 O_ASYNC 后不再投递 ---- */
    sio_check("clear O_ASYNC",
              lx3(LX_FCNTL, fds[0], LX_F_SETFL, LX_O_NONBLOCK), 0);
    sio_check("getfl no longer reports O_ASYNC",
              ((lx3(LX_FCNTL, fds[0], LX_F_GETFL, 0) & LX_O_ASYNC) == 0) ? 1 : 0,
              1);
    hits0 = s_sio_hits;
    sio_check("write after clearing O_ASYNC returns the byte",
              lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)"j", 1), 1);
    sio_check("clearing O_ASYNC stops delivery", s_sio_hits - hits0, 0);
    b[0] = 0;
    (void)lx3(LX_READ, fds[0], (lx_long)(unsigned long)b, 1);

    /* ---- 6) 写端全关（EOF）也投一次（先把 O_ASYNC 开回来） ---- */
    sio_check("re-open O_ASYNC",
              lx3(LX_FCNTL, fds[0], LX_F_SETFL,
                  LX_O_NONBLOCK | LX_O_ASYNC), 0);
    hits0 = s_sio_hits;
    sio_check("close writer returns 0", lx1(LX_CLOSE, fds[1]), 0);
    sio_check("close writer (EOF) sends SIGIO", s_sio_hits - hits0, 1);

    (void)lx1(LX_CLOSE, d);
    (void)lx1(LX_CLOSE, fds[0]);

    /* ---- 7) SIGPIPE 的 siginfo：内核代发走 SI_USER ---- */
    {
        int     pfds[2];
        act_t   pact;
        act_t   pold;
        lx_long phits0;
        lx_long wrc;

        pfds[0] = -1;
        pfds[1] = -1;
        sio_check("SIGPIPE pipe2",
                  lx2(LX_PIPE2, (lx_long)(unsigned long)pfds,
                      LX_O_NONBLOCK), 0);
        pact.handler = (unsigned long)(unsigned long)&on_sigpipe;
        pact.flags = LX_SA_SIGINFO;
        pact.restorer = (unsigned long)(unsigned long)&fork_sig_restorer;
        pact.mask = 0ul;
        pold.handler = SIG_DFL;
        pold.flags = 0ul;
        pold.restorer = 0ul;
        pold.mask = 0ul;
        sio_check("rt_sigaction(SIGPIPE, SA_SIGINFO)",
                  lx4(LX_RT_SIGACTION, LX_SIGPIPE,
                      (lx_long)(unsigned long)&pact,
                      (lx_long)(unsigned long)&pold, 8), 0);
        sio_check("close SIGPIPE pipe read end", lx1(LX_CLOSE, pfds[0]), 0);
        phits0 = s_pipe_hits;
        wrc = lx3(LX_WRITE, pfds[1], (lx_long)(unsigned long)"p", 1);
        sio_check("write with no reader returns -EPIPE", wrc, -LX_EPIPE);
        sio_check("SIGPIPE handler ran", s_pipe_hits - phits0, 1);
        sio_check("SIGPIPE si_signo",
                  (s_pipe_si != (void *)0) ? si_i32(s_pipe_si, 0) : -1,
                  LX_SIGPIPE);
        sio_check("SIGPIPE si_code is SI_USER",
                  (s_pipe_si != (void *)0) ? si_i32(s_pipe_si, 8) : -1, 0);
        sio_check("SIGPIPE si_pid is self",
                  (s_pipe_si != (void *)0) ? si_i32(s_pipe_si, 16) : -1,
                  lx1(LX_GETPID, 0));
        (void)lx1(LX_CLOSE, pfds[1]);
    }
}
#endif /* ARKRTOS_USE_LINUX_SIGIO */


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
             * 现在改成**放行管道**：管道默认是阻塞的（U-26，空读挂起等到
             * 有人写），所以子进程那一次 read 会一直等到父进程写下放行字节
             * 才返回 —— 60 ms 的计时起点严格在「父读完 b0、写完放行字节」
             * 之后，读数是被构造出来的，不是碰出来的。循环仍保留上限并接受
             * -EINTR：被打断就再读一次，不拿调度顺序当判据。 */
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

    /* ---------------- 阻塞管道的跨执行流语义（U-26） ----------------
     * 上面那一段证的是「数据已经在缓冲里」的读写；管道的默认语义是
     * **阻塞**：空读要等到有人写，满写要等到有人排空。这两条只有第二个
     * 执行流能证 —— 单执行流一阻塞就再也回不来，不是可证伪的读数。
     * 这里分两组：
     *   A. 子进程活够 60 ms 再写 3 字节 -> 父的阻塞读必须等满 40 ms 才回，
     *      读满之后写端全关（子进程已关并退出）-> 下一次阻塞读立刻回 0。
     *   B. 父把 256 字节缓冲填满后再写 300 字节（阻塞写）-> 子进程分两轮
     *      排空，父的写必须阻塞 >= 40 ms 后才回 300；子进程把「读到的总
     *      字节数」与「内容是否逐字节对图案」写回第二根管道，父读它 ——
     *      跨进程的读数不能只靠退出码，退出码只有 8 位。
     */
    {
        int     dfds[2];
        int     sfds[2];
        lx_long child6;
        lx_long child7;
        lx_long rc6;
        lx_long status6;
        long    ts6[2];
        long    c0;
        long    c1;
        long    b0;
        long    b1;
        char    rbuf6[8];
        int     i;

        /* ---------------- A：阻塞读等到别人写 ---------------- */
        check("cross-task blocking read pipe2 == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)dfds, 0), 0);

        child6 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (child6 == 0)
        {
            long t0 = -1;
            long tnow;

            (void)lx1(LX_CLOSE, (lx_long)dfds[0]);   /* 子进程不读 */
            if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                    (lx_long)(unsigned long)ts6, 0) == 0)
            {
                t0 = (ts6[0] * 1000) + (ts6[1] / 1000000);
            }
            for (;;)
            {
                if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                        (lx_long)(unsigned long)ts6, 0) != 0)
                {
                    break;
                }
                tnow = (ts6[0] * 1000) + (ts6[1] / 1000000);
                if ((t0 >= 0) && ((tnow - t0) >= 60))
                {
                    break;
                }
            }
            (void)lx3(LX_WRITE, (lx_long)dfds[1],
                      (lx_long)(unsigned long)"abc", 3);
            (void)lx1(LX_CLOSE, (lx_long)dfds[1]);  /* 关写端 -> 父下一次读 EOF */
            (void)lx1(LX_EXIT, 11);
            for (;;)
            {
            }
        }

        check("clone for blocking reader > 0", (child6 > 0) ? 1 : 0, 1);

        /* 父把自己那份写端关掉：此刻唯一的写者是子进程。父的这次读必须
         * 挂起，直到子进程 60 ms 后写出那 3 字节。 */
        (void)lx1(LX_CLOSE, (lx_long)dfds[1]);
        b0 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts6, 0) == 0)
        {
            b0 = (ts6[0] * 1000) + (ts6[1] / 1000000);
        }
        for (i = 0; i < 8; i++)
        {
            rbuf6[i] = 0;
        }
        rc6 = lx3(LX_READ, (lx_long)dfds[0], (lx_long)(unsigned long)rbuf6, 8);
        b1 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts6, 0) == 0)
        {
            b1 = (ts6[0] * 1000) + (ts6[1] / 1000000);
        }
        out_line3("[fork] blocking pipe read waited ",
                  ((b0 < 0) || (b1 < 0)) ? -1 : (b1 - b0), " ms\n");
        check("blocking pipe read returned 3", rc6, 3);
        check("blocking pipe read content",
              ((rbuf6[0] == 'a') && (rbuf6[1] == 'b') && (rbuf6[2] == 'c')) ? 1 : 0, 1);
        check("blocking pipe read waited >= 40 ms",
              ((b0 >= 0) && (b1 >= 0) && ((b1 - b0) >= 40)) ? 1 : 0, 1);
        /* 子进程已经关掉它那份写端并退出：这一次读不该再挂起，立刻回 0。 */
        c0 = -1;
        c1 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts6, 0) == 0)
        {
            c0 = (ts6[0] * 1000) + (ts6[1] / 1000000);
        }
        rc6 = lx3(LX_READ, (lx_long)dfds[0], (lx_long)(unsigned long)rbuf6, 8);
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts6, 0) == 0)
        {
            c1 = (ts6[0] * 1000) + (ts6[1] / 1000000);
        }
        check("blocking pipe read after writer exit -> EOF", rc6, 0);
        check("EOF read did not sleep a slice",
              ((c0 >= 0) && (c1 >= 0) && ((c1 - c0) < 40)) ? 1 : 0, 1);
        (void)lx1(LX_CLOSE, (lx_long)dfds[0]);

        rc6 = lx4(LX_WAIT4, child6, (lx_long)(unsigned long)&status6, 0, 0);
        check("blocking reader child reaped", rc6, child6);
        check("blocking reader child exit 11", (status6 >> 8) & 0xFF, 11);

        /* ---------------- B：阻塞写等到别人排空 ---------------- */
        check("cross-task blocking write pipe2 == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)dfds, 0), 0);
        check("status pipe2 == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)sfds, 0), 0);

        /* 先把 256 字节缓冲填满：父随后那 300 字节的第一个字节都写不进去。 */
        for (i = 0; i < 256; i++)
        {
            g_pb_fill[i] = 'A';
        }
        for (i = 0; i < 300; i++)
        {
            g_pb_out[i] = (char)(i & 0xFF);
        }
        check("fill the 256-byte pipe buffer",
              lx3(LX_WRITE, (lx_long)dfds[1],
                  (lx_long)(unsigned long)g_pb_fill, 256), 256);

        child7 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
        if (child7 == 0)
        {
            long t0 = -1;
            long tnow;
            long tot = 0;
            long rd;
            long bad = 0;
            char st[5];
            int  k;

            (void)lx1(LX_CLOSE, (lx_long)dfds[1]);   /* 子进程只读 */
            if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                    (lx_long)(unsigned long)ts6, 0) == 0)
            {
                t0 = (ts6[0] * 1000) + (ts6[1] / 1000000);
            }
            for (;;)
            {
                if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                        (lx_long)(unsigned long)ts6, 0) != 0)
                {
                    break;
                }
                tnow = (ts6[0] * 1000) + (ts6[1] / 1000000);
                if ((t0 >= 0) && ((tnow - t0) >= 60))
                {
                    break;
                }
            }
            /* 第一轮：排空父填进去的 256 个 'A' */
            rd = lx3(LX_READ, (lx_long)dfds[0],
                     (lx_long)(unsigned long)g_pb_fill, 256);
            if (rd == 256)
            {
                tot = 256;
                for (k = 0; k < 256; k++)
                {
                    if (g_pb_fill[k] != 'A')
                    {
                        bad = 1;
                    }
                }
            }
            else
            {
                bad = 1;
                if (rd > 0)
                {
                    tot += rd;
                }
            }
            /* 第二轮：父那 300 字节此刻才写得进来，逐字节对图案 */
            while (tot < 556)
            {
                rd = lx3(LX_READ, (lx_long)dfds[0],
                         (lx_long)(unsigned long)g_pb_rd, 128);
                if (rd <= 0)
                {
                    break;
                }
                for (k = 0; k < rd; k++)
                {
                    long want = (tot - 256) + k;

                    if ((unsigned char)g_pb_rd[k] != (unsigned char)(want & 0xFF))
                    {
                        bad = 1;
                    }
                }
                tot += rd;
            }
            st[0] = (char)((tot >> 24) & 0xFF);
            st[1] = (char)((tot >> 16) & 0xFF);
            st[2] = (char)((tot >> 8) & 0xFF);
            st[3] = (char)(tot & 0xFF);
            st[4] = (char)bad;
            (void)lx3(LX_WRITE, (lx_long)sfds[1], (lx_long)(unsigned long)st, 5);
            (void)lx1(LX_CLOSE, (lx_long)dfds[0]);
            (void)lx1(LX_CLOSE, (lx_long)sfds[1]);
            (void)lx1(LX_EXIT, 13);
            for (;;)
            {
            }
        }

        check("clone for blocking writer > 0", (child7 > 0) ? 1 : 0, 1);

        (void)lx1(LX_CLOSE, (lx_long)sfds[1]);   /* 父只从状态管道读 */

        /* 缓冲已满：这一次写必须挂起，直到子进程排空。 */
        b0 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts6, 0) == 0)
        {
            b0 = (ts6[0] * 1000) + (ts6[1] / 1000000);
        }
        rc6 = lx3(LX_WRITE, (lx_long)dfds[1],
                  (lx_long)(unsigned long)g_pb_out, 300);
        b1 = -1;
        if (lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                (lx_long)(unsigned long)ts6, 0) == 0)
        {
            b1 = (ts6[0] * 1000) + (ts6[1] / 1000000);
        }
        out_line3("[fork] blocking pipe write waited ",
                  ((b0 < 0) || (b1 < 0)) ? -1 : (b1 - b0), " ms\n");
        check("blocking pipe write wrote all 300", rc6, 300);
        check("blocking pipe write waited >= 40 ms",
              ((b0 >= 0) && (b1 >= 0) && ((b1 - b0) >= 40)) ? 1 : 0, 1);

        rc6 = lx4(LX_WAIT4, child7, (lx_long)(unsigned long)&status6, 0, 0);
        check("blocking writer child reaped", rc6, child7);
        check("blocking writer child exit 13", (status6 >> 8) & 0xFF, 13);

        for (i = 0; i < 5; i++)
        {
            rbuf6[i] = 0;
        }
        rc6 = lx3(LX_READ, (lx_long)sfds[0], (lx_long)(unsigned long)rbuf6, 5);
        check("child report came back", rc6, 5);
        check("child read 556 bytes in total",
              (((unsigned long)(unsigned char)rbuf6[0] << 24) |
               ((unsigned long)(unsigned char)rbuf6[1] << 16) |
               ((unsigned long)(unsigned char)rbuf6[2] << 8) |
               (unsigned long)(unsigned char)rbuf6[3]), 556);
        check("child saw the exact byte pattern", (lx_long)rbuf6[4], 0);

        (void)lx1(LX_CLOSE, (lx_long)dfds[0]);
        (void)lx1(LX_CLOSE, (lx_long)dfds[1]);
        (void)lx1(LX_CLOSE, (lx_long)sfds[0]);
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
            /* PI 类 op（6/7/8）已经实现，判据挪到下面「futex 第四档」；
             * 条件变量的 PI 变体（11/12）在「futex 第五档」。这一格只留一条
             * 参数面：缺 uaddr2（第 5 格为 0）必须当场回 -EINVAL，不能拿
             * 「反正是 PI 类」糊过去。 */
            check("futex(WAIT_REQUEUE_PI) without uaddr2 == -EINVAL",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)fw,
                      LX_FUTEX_WAIT_REQUEUE_PI, 0, 0, 0, 0), -LX_EINVAL);

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

                /* 压力断言：时钟忙等 250 ms，期间把 clock_gettime 的调用频率
                 * 拉到数十万次/秒。任何一次返回非 0（曾出现无来源的 3）都计为
                 * FAIL。该形态同时压住 syscall 快路径、用户态只读映射与
                 * 上下文切换三者的一致性。 */
                {
                    long c0 = mono_ms();
                    int  badc = 0;
                    int  sp2;

                    for (sp2 = 0; sp2 < 4000000; sp2++)
                    {
                        lx_long tss[2];
                        lx_long rcc;

                        tss[0] = 0;
                        tss[1] = 0;
                        rcc = lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC,
                                  (lx_long)(unsigned long)tss, 0);
                        if (rcc != 0)
                        {
                            /* 命中时把返回值与时序号一次打全行，便于复盘。 */
                            char  bb[200];
                            char *pp = bb;

                            pp = chk_str(pp, "[clkprobe] pid=");
                            pp = chk_num(pp, lx1(LX_GETPID, 0));
                            pp = chk_str(pp, " spin=");
                            pp = chk_num(pp, sp2);
                            pp = chk_str(pp, " rc=");
                            pp = chk_num(pp, rcc);
                            pp = chk_str(pp, " ts0=");
                            pp = chk_num(pp, tss[0]);
                            pp = chk_str(pp, " ts1=");
                            pp = chk_num(pp, tss[1]);
                            pp = chk_str(pp, "\n");
                            *pp = '\0';
                            out(bb);
                            badc = badc + 1;
                        }
                        if ((sp2 & 0x3Fu) == 0)
                        {
                            long cn = mono_ms();

                            if ((c0 >= 0) && (cn >= 0) && ((cn - c0) >= 250))
                            {
                                break;
                            }
                        }
                    }
                    if (badc != 0)
                    {
                        check("clock_gettime busy-wait rc", badc, 0);
                    }
                }

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

    /* ---------------- futex 第四档：PI 三档与 robust list ----------------
     * 三段判据，各判一件不同的事：
     *   1) 单进程：字布局与返回值（tid 写进字、-EDEADLK、-EPERM、-EAGAIN 且
     *      置 WAITERS、接手死掉的持有者时保留 OWNER_DIED）—— 不依赖调度；
     *   2) 跨进程 PI + robust PI 死亡交接：子进程用 LOCK_PI 持有并登记 robust
     *      链，父进程 LOCK_PI 排队；子进程**不解锁**直接退出，父进程必须拿到
     *      锁，且字里是「父进程 tid | OWNER_DIED」。这一条同时覆盖「真阻塞」
     *      与「持有者死亡后交接」两件事；
     *   3) 非 PI robust：子进程直接写字持有（glibc 的 robust 普通互斥就是这么
     *      写的）、登记链、退出后字必须变成 OWNER_DIED（tid 清掉）。
     * 反假绿：PI 若退化成「叫醒后大家再抢」或 robust 清理没跑，第 2 段的父
     * 进程会停在 -EAGAIN 或看到字里还是子进程 tid —— 是红的读数，不是挂死
     * （子进程等不到父进程排队时会先投 SIGCHLD 兜底）。 */
    {
        volatile unsigned long *pw;
        lx_long pa;
        lx_long mytid;
        lx_long ppid4;

        /* 长度按**一整页**申请：本机页大小是 8 KB（0x2000），只申请 4096
         * 也能拿到一页，但下面几组夹具按结构体排布会用到 0x1C00 附近，
         * 写清整页长度省得「后半页是不是有效的」变成隐含前提。 */
        pa = lx6(LX_MMAP, 0, 8192, 3,
                 LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
        check("futex4 mmap shared page > 0", (pa > 0) ? 1 : 0, 1);

        if (pa > 0)
        {
            pw = (volatile unsigned long *)(unsigned long)pa;
            mytid = lx1(LX_GETPID, 0);
            ppid4 = mytid;

            /* ---- 1) 单进程语义 ---- */
            pw[0] = 0ul;
            check("futex LOCK_PI on a free word == 0",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw, LX_FUTEX_LOCK_PI, 0, 0),
                  0);
            check("futex LOCK_PI wrote our tid into the word",
                  (lx_long)pw[0], mytid);
            check("futex LOCK_PI twice == -EDEADLK",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw, LX_FUTEX_LOCK_PI, 0, 0),
                  -LX_EDEADLK);
            check("futex TRYLOCK_PI while held == -EDEADLK",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw,
                      LX_FUTEX_TRYLOCK_PI, 0, 0), -LX_EDEADLK);
            check("futex UNLOCK_PI as owner == 0",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw,
                      LX_FUTEX_UNLOCK_PI, 0, 0), 0);
            check("futex UNLOCK_PI cleared the word", (lx_long)pw[0], 0);
            check("futex UNLOCK_PI when not the owner == -EPERM",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw,
                      LX_FUTEX_UNLOCK_PI, 0, 0), -LX_EPERM);

            pw[0] = 4242ul;             /* 一个不可能存在的持有者 */
            check("futex TRYLOCK_PI on an owned word == -EAGAIN",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw,
                      LX_FUTEX_TRYLOCK_PI, 0, 0), -LX_EAGAIN);
            check("futex TRYLOCK_PI set the WAITERS bit while failing",
                  (lx_long)(pw[0] & LX_FUTEX_WAITERS),
                  (lx_long)LX_FUTEX_WAITERS);
            check("futex TRYLOCK_PI left the owner tid alone",
                  (lx_long)(pw[0] & LX_FUTEX_TID_MASK), 4242);

            pw[0] = 4242ul | LX_FUTEX_OWNER_DIED;
            check("futex LOCK_PI on a dead owner == 0",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw, LX_FUTEX_LOCK_PI, 0, 0),
                  0);
            check("futex LOCK_PI took over from the dead owner",
                  (lx_long)(pw[0] & LX_FUTEX_TID_MASK), mytid);
            check("futex LOCK_PI kept OWNER_DIED for the taker",
                  (lx_long)(pw[0] & LX_FUTEX_OWNER_DIED),
                  (lx_long)LX_FUTEX_OWNER_DIED);
            pw[0] = (unsigned long)mytid;   /* 用户态 consistent 之后清掉 */
            check("futex UNLOCK_PI after consistent == 0",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)pw,
                      LX_FUTEX_UNLOCK_PI, 0, 0), 0);

            /* ---- robust list 的单进程参数面 ---- */
            check("set_robust_list with a short length == -EINVAL",
                  lx2(LX_SET_ROBUST_LIST,
                      (lx_long)(unsigned long)(pw + 0x20), 8), -LX_EINVAL);
            check("set_robust_list with an unaligned head == -EINVAL",
                  lx2(LX_SET_ROBUST_LIST,
                      (lx_long)(unsigned long)((char *)(pw + 0x20) + 1), 24),
                  -LX_EINVAL);
            check("set_robust_list with a null head == -EINVAL",
                  lx2(LX_SET_ROBUST_LIST, 0, 24), -LX_EINVAL);
            check("set_robust_list on a well formed head == 0",
                  lx2(LX_SET_ROBUST_LIST,
                      (lx_long)(unsigned long)(pw + 0x20), 24), 0);
            {
                unsigned long *out = (unsigned long *)(unsigned long)(pw + 0x60);
                unsigned long *olen = (unsigned long *)(unsigned long)(pw + 0x68);

                *out = 0ul;
                *olen = 0ul;
                check("get_robust_list(self) == 0",
                      lx3(LX_GET_ROBUST_LIST, mytid,
                          (lx_long)(unsigned long)out,
                          (lx_long)(unsigned long)olen), 0);
                check("get_robust_list reports the head we registered",
                      (lx_long)*out, (lx_long)(unsigned long)(pw + 0x20));
                check("get_robust_list reports the length we registered",
                      (lx_long)*olen, 24);
                check("get_robust_list on a foreign pid == -ESRCH",
                      lx3(LX_GET_ROBUST_LIST, 99999,
                          (lx_long)(unsigned long)out,
                          (lx_long)(unsigned long)olen), -LX_ESRCH);
            }

            /* ---- 2) 跨进程 PI + robust PI 死亡交接 ---- */
            {
                unsigned long *rh  = (unsigned long *)(unsigned long)(pw + 0x100);
                unsigned long *nod = (unsigned long *)(unsigned long)(pw + 0x120);
                volatile unsigned long *lock = pw + 0x160;   /* 0x120 + 0x40 */
                volatile unsigned long *go   = pw + 0x200;
                volatile unsigned long *yld  = pw + 0x280;   /* 只用来让 CPU */
                lx_long child;
                lx_long status = -1;
                lx_long rc = -1;
                int     round;

                rh[0] = (unsigned long)nod | 1ul;   /* 最低位 = PI 条目 */
                /* futex_offset 是按**字节**算的（Linux 的 struct robust_list_head
                 * 就这么定义），这里的变量都是 unsigned long *，写 0x40 会被
                 * 内核当成 0x40 个字节，指到别的字上去。 */
                rh[1] = (unsigned long)((const char *)lock - (const char *)nod);
                rh[2] = 0ul;                        /* list_op_pending */
                nod[0] = (unsigned long)rh;         /* 终止：回到链头 */
                *lock = 0ul;
                *go = 0ul;
                *yld = 0ul;

                child = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
                if (child == 0)
                {
                    int     code = 0;
                    int     spins;
                    lx_long me = lx1(LX_GETPID, 0);

                    if (lx2(LX_SET_ROBUST_LIST, (lx_long)(unsigned long)rh,
                            24) == 0)
                    {
                        code |= 1;
                    }
                    if (lx4(LX_FUTEX, (lx_long)(unsigned long)lock,
                            LX_FUTEX_LOCK_PI, 0, 0) == 0)
                    {
                        code |= 2;
                    }
                    if ((*lock & LX_FUTEX_TID_MASK) == (unsigned long)me)
                    {
                        code |= 4;
                    }
                    /* 放行父进程：它随后会 LOCK_PI 排在我后面 */
                    *go = 1ul;
                    (void)lx3(LX_FUTEX, (lx_long)(unsigned long)go,
                              LX_FUTEX_WAKE, 1);
                    /* 等父进程真的排上队：WAITERS 位是内核替它置的，比
                     * 「等几毫秒」可靠得多。这里必须**真的让出 CPU** —— 纯
                     * 自旋会把这个单核一直占住，父进程连一次 LOCK_PI 都跑
                     * 不到，WAITERS 永远不出现。本机没有 sched_yield，用
                     * FUTEX_WAIT 带超时是唯一能主动交棒的调用。等不到就先投
                     * SIGCHLD 兜底，让父进程那次 LOCK_PI 回 -EINTR（读数是
                     * 红的）。
                     *
                     * 超时必须是**整毫秒**：本内核 timespec 换算是
                     * ms = ts[0]*1000 + ts[1]/1000000（截断），0.5 ms 会被
                     * 取整成 0，FUTEX_WAIT 立刻回 -ETIMEDOUT，循环退化成
                     * 不交棒的空转（旧的 0.5 ms/2000 轮就是这么时红时绿的：
                     * 父进程还没排上队，子进程已经把 2000 轮跑完退出）。
                     * 5 ms × 400 轮 = 最坏 2 s，正常几毫秒内看到 WAITERS
                     * 就 break。 */
                    for (spins = 0; spins < 400; spins++)
                    {
                        if ((*lock & LX_FUTEX_WAITERS) != 0ul)
                        {
                            code |= 8;
                            break;
                        }
                        {
                            lx_long ts[2];

                            ts[0] = 0;
                            ts[1] = 5000000L;   /* 5 ms（见上：必须整毫秒） */
                            (void)lx4(LX_FUTEX, (lx_long)(unsigned long)yld,
                                      LX_FUTEX_WAIT, 0,
                                      (lx_long)(unsigned long)ts);
                        }
                    }
                    if ((code & 8) == 0)
                    {
                        (void)lx2(LX_KILL, ppid4, LX_SIGCHLD);
                    }
                    (void)lx1(LX_EXIT, code);   /* 持锁退出：交给 robust 清理 */
                    for (;;)
                    {
                    }
                }
                check("clone for PI robust child > 0", (child > 0) ? 1 : 0, 1);

                /* 先让出 CPU 让子进程把锁拿到（父进程一直占着 CPU 的话子进程
                 * 永远跑不起来）。用 go 这个字做握手，不读时钟。 */
                rc = -LX_ETIMEDOUT;
                for (round = 0; round < 16; round++)
                {
                    /* 期望值固定写 0（「还没放行」），不要读当前值再传进去：
                     * 子进程可能已经放行过了，那时传 1 会让内核认为「值没变、
                     * 该睡」，把已经发生的唤醒永远丢掉。0 这个口径下，字已经
                     * 变成 1 就直接 -EAGAIN，循环立刻收尾。 */
                    rc = lx4(LX_FUTEX, (lx_long)(unsigned long)go,
                             LX_FUTEX_WAIT, 0, 0);
                    if ((rc == 0) || (*go == 1ul))
                    {
                        break;
                    }
                }
                check("PI robust: parent observed the child holding the lock",
                      (lx_long)(*go), 1);

                /* 父进程排队：此刻子进程还持有，所以这一次必须真的睡下。
                 * 子进程退出时会投 SIGCHLD，而 robust 清理不一定已经跑完，
                 * 于是这一次可能先回 -EINTR；Linux 上这就是「自己决定重不重
                 * 试」的信号，重试即可（重试期间清理跑完，下一次就拿到锁）。 */
                rc = -LX_EAGAIN;
                for (round = 0; round < 16; round++)
                {
                    rc = lx4(LX_FUTEX, (lx_long)(unsigned long)lock,
                             LX_FUTEX_LOCK_PI, 0, 0);
                    if ((rc == 0) || ((rc != -LX_EINTR) && (rc != -LX_EAGAIN)))
                    {
                        break;
                    }
                }
                check("PI robust: parent acquired after the holder died", rc, 0);
                check("PI robust: word holds the parent tid",
                      (lx_long)(*lock & LX_FUTEX_TID_MASK), mytid);
                check("PI robust: word carries OWNER_DIED for the taker",
                      (lx_long)(*lock & LX_FUTEX_OWNER_DIED),
                      (lx_long)LX_FUTEX_OWNER_DIED);
                status = -1;
                check("PI robust child reaped",
                      lx4(LX_WAIT4, child, (lx_long)(unsigned long)&status, 0, 0),
                      child);
                /* bit0 登记成功 / bit1 拿到锁 / bit2 字里是它 / bit3 看到
                 * 父进程排队（WAITERS 位）。四条全置才算这一轮真的跑通。 */
                check("PI robust child saw register+lock+tid+waiter (bits=15)",
                      (status >> 8) & 0xFF, 15);
            }

            /* ---- 3) 非 PI robust：持锁退出后字必须变成 OWNER_DIED ---- */
            {
                unsigned long *rh2  = (unsigned long *)(unsigned long)(pw + 0x300);
                unsigned long *nod2 = (unsigned long *)(unsigned long)(pw + 0x320);
                volatile unsigned long *lock2 = pw + 0x360;
                volatile unsigned long *go2   = pw + 0x380;
                lx_long child2;
                lx_long status2 = -1;
                lx_long rc2 = -1;
                int     round2;

                rh2[0] = (unsigned long)nod2;   /* 没有 PI 位 */
                rh2[1] = (unsigned long)((const char *)lock2 -
                                         (const char *)nod2);
                rh2[2] = 0ul;
                nod2[0] = (unsigned long)rh2;
                *lock2 = 0ul;
                *go2 = 0ul;

                child2 = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
                if (child2 == 0)
                {
                    int     code = 0;
                    lx_long me = lx1(LX_GETPID, 0);

                    if (lx2(LX_SET_ROBUST_LIST, (lx_long)(unsigned long)rh2,
                            24) == 0)
                    {
                        code |= 1;
                    }
                    /* glibc 的 robust 普通互斥就是这么写的：字里放持有者 tid，
                     * 拿锁靠 cmpxchg，不经过内核。 */
                    *lock2 = (unsigned long)me;
                    if ((*lock2 & LX_FUTEX_TID_MASK) == (unsigned long)me)
                    {
                        code |= 2;
                    }
                    *go2 = 1ul;
                    (void)lx3(LX_FUTEX, (lx_long)(unsigned long)go2,
                              LX_FUTEX_WAKE, 1);
                    (void)lx1(LX_EXIT, code);       /* 持锁退出 */
                    for (;;)
                    {
                    }
                }
                check("clone for non-PI robust child > 0",
                      (child2 > 0) ? 1 : 0, 1);

                rc2 = -LX_ETIMEDOUT;
                for (round2 = 0; round2 < 16; round2++)
                {
                    /* 同上一段的注释：期望值固定 0，避免「已经放行」被当成
                     * 「值没变」而永久睡下。 */
                    rc2 = lx4(LX_FUTEX, (lx_long)(unsigned long)go2,
                              LX_FUTEX_WAIT, 0, 0);
                    if ((rc2 == 0) || (*go2 == 1ul))
                    {
                        break;
                    }
                }
                check("non-PI robust: parent observed the child owning the word",
                      (lx_long)(*go2), 1);

                /* 父进程按 PI 语义接手（OWNER_DIED 置位的字可以直接拿）：
                 * 这一条同时证明「清理跑过」（否则会一直排队，读到 -EINTR）
                 * 与「接手者看得见 OWNER_DIED」。 */
                rc2 = -LX_EAGAIN;
                for (round2 = 0; round2 < 16; round2++)
                {
                    rc2 = lx4(LX_FUTEX, (lx_long)(unsigned long)lock2,
                              LX_FUTEX_LOCK_PI, 0, 0);
                    if ((rc2 == 0) || ((rc2 != -LX_EINTR) && (rc2 != -LX_EAGAIN)))
                    {
                        break;
                    }
                }
                check("non-PI robust: parent took over the dead owner's word",
                      rc2, 0);
                check("non-PI robust: word carries OWNER_DIED after the death",
                      (lx_long)(*lock2 & LX_FUTEX_OWNER_DIED),
                      (lx_long)LX_FUTEX_OWNER_DIED);
                check("non-PI robust: word now holds the parent tid",
                      (lx_long)(*lock2 & LX_FUTEX_TID_MASK), mytid);
                status2 = -1;
                check("non-PI robust child reaped",
                      lx4(LX_WAIT4, child2, (lx_long)(unsigned long)&status2,
                          0, 0), child2);
                check("non-PI robust child registered and owned (bits=3)",
                      (status2 >> 8) & 0xFF, 3);
            }
        }
    }

    /* ---------------- futex 第五档：条件变量的 PI 变体 ----------------
     * glibc 的 `pthread_cond_wait` 配 PI 互斥走的是 WAIT_REQUEUE_PI(11) +
     * CMP_REQUEUE_PI(12)：等待者先睡在条件变量字上，被唤醒时内核把互斥
     * **直接交**给它——不是「叫醒再抢」。单进程只判参数面（值不符 / 目标锁
     * 空 / 缺 uaddr2），跨进程才判真的搬运与交接：
     *   父进程 LOCK_PI 持有互斥 → 子进程 WAIT_REQUEUE_PI(cond, 0, mtx) 睡下
     *   并置 WAITERS → 父进程 CMP_REQUEUE_PI(cond, 0, mtx, 1, 0) 把排头搬
     *   过去、字里写子进程 tid、叫醒它 → 子进程确认自己是新持有者，等父进程
     *   放行之后再解锁。
     * 反假绿：搬运没做时 CMP 回 0（不是 1）、字里还是父进程 tid（或 WAITERS
     * 残留），子进程的位掩码不会是 7；子进程等不到 WAITERS 时投 SIGCHLD 兜底。
     * 子进程接手后**不立刻解锁**：先等父进程把「tid/WAITERS」两条读数读完再
     * 放行，否则「叫醒即抢占」的调度会把字清成 0，读数就变成偶发红。 */
    {
        volatile unsigned long *rv;
        lx_long ra;
        lx_long rchild;
        lx_long rstatus = -1;
        lx_long rrc = -1;
        int     rround;

        ra = lx6(LX_MMAP, 0, 8192, 3,
                 LX_MAP_SHARED | LX_MAP_ANONYMOUS, -1, 0);
        check("futex5 mmap shared page > 0", (ra > 0) ? 1 : 0, 1);

        if (ra > 0)
        {
            volatile unsigned long *cv     = (volatile unsigned long *)(unsigned long)ra;
            volatile unsigned long *mx     = cv + 1;    /* PI 互斥 */
            volatile unsigned long *mxfree = cv + 2;    /* 空着的一把 */
            volatile unsigned long *ctidv  = cv + 3;    /* 子进程 tid */
            volatile unsigned long *gov    = cv + 4;    /* 父进程放行子进程 */
            volatile unsigned long *yldv   = cv + 5;    /* 只为让出 CPU */

            *cv = 0ul;
            *mx = 0ul;
            *mxfree = 0ul;
            *ctidv = 0ul;
            *gov = 0ul;
            *yldv = 0ul;

            /* ---- 1) 参数面（单进程，不依赖调度） ---- */
            check("futex(WAIT_REQUEUE_PI) on a different value == -EAGAIN",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)cv,
                      LX_FUTEX_WAIT_REQUEUE_PI, 1, 0,
                      (lx_long)(unsigned long)mx, 0), -LX_EAGAIN);
            check("futex(WAIT_REQUEUE_PI) with a free mutex == -EINVAL",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)cv,
                      LX_FUTEX_WAIT_REQUEUE_PI, 0, 0,
                      (lx_long)(unsigned long)mxfree, 0), -LX_EINVAL);
            check("futex LOCK_PI before the condvar handoff == 0",
                  lx4(LX_FUTEX, (lx_long)(unsigned long)mx,
                      LX_FUTEX_LOCK_PI, 0, 0), 0);
            check("futex(CMP_REQUEUE_PI) with a mismatched word == -EAGAIN",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)cv,
                      LX_FUTEX_CMP_REQUEUE_PI, 0, 1,
                      (lx_long)(unsigned long)mx, 1), -LX_EAGAIN);
            check("futex(CMP_REQUEUE_PI) without uaddr2 == -EINVAL",
                  lx6(LX_FUTEX, (lx_long)(unsigned long)cv,
                      LX_FUTEX_CMP_REQUEUE_PI, 0, 1, 0, 0), -LX_EINVAL);

            /* ---- 2) 跨进程：真的把排头搬进互斥并交接 ---- */
            rchild = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
            if (rchild == 0)
            {
                int     code = 0;
                lx_long me = lx1(LX_GETPID, 0);

                *ctidv = (unsigned long)me;
                /* 睡在条件变量字上；互斥此刻由父进程持有，所以这一次真的
                 * 会睡下（内核同时替父进程置 WAITERS）。 */
                if (lx6(LX_FUTEX, (lx_long)(unsigned long)cv,
                        LX_FUTEX_WAIT_REQUEUE_PI, 0, 0,
                        (lx_long)(unsigned long)mx, 0) == 0)
                {
                    code |= 1;  /* bit0: 是被搬运唤醒的，不是超时或错误 */
                }
                if ((*mx & LX_FUTEX_TID_MASK) == (unsigned long)me)
                {
                    code |= 2;  /* bit1: 互斥字已经写成本任务 tid */
                }
                /* 等父进程把读数读完再解锁。期望值固定写 0（「还没放行」）：
                 * 传当前值的话，已经放行过的字会让内核判「值没变、该睡」，
                 * 把已发生的唤醒丢掉。 */
                for (rround = 0; rround < 400; rround++)
                {
                    lx_long wrc;

                    if (*gov == 1ul)
                    {
                        break;
                    }
                    wrc = lx4(LX_FUTEX, (lx_long)(unsigned long)gov,
                              LX_FUTEX_WAIT, 0, 0);
                    if ((wrc == 0) || (*gov == 1ul))
                    {
                        break;
                    }
                }
                if (lx4(LX_FUTEX, (lx_long)(unsigned long)mx,
                        LX_FUTEX_UNLOCK_PI, 0, 0) == 0)
                {
                    code |= 4;  /* bit2: 接手来的锁能正常放掉 */
                }
                (void)lx1(LX_EXIT, code);
                for (;;)
                {
                }
            }
            check("clone for condvar rqpi child > 0",
                  (rchild > 0) ? 1 : 0, 1);

            /* 等子进程真的排上队：WAITERS 位是内核替它置的。让出 CPU 用带
             * 整毫秒超时的 FUTEX_WAIT（本机没有 sched_yield；0.5 ms 会被
             * timespec 换算截断成 0，循环退化成空转）。 */
            for (rround = 0; rround < 400; rround++)
            {
                lx_long ts[2];

                if ((*mx & LX_FUTEX_WAITERS) != 0ul)
                {
                    break;
                }
                ts[0] = 0;
                ts[1] = 5000000L;       /* 5 ms */
                (void)lx4(LX_FUTEX, (lx_long)(unsigned long)yldv,
                          LX_FUTEX_WAIT, 0, (lx_long)(unsigned long)ts);
            }
            check("futex rqpi: child queued on the mutex (WAITERS)",
                  ((*mx & LX_FUTEX_WAITERS) != 0ul) ? 1 : 0, 1);
            if ((*mx & LX_FUTEX_WAITERS) == 0ul)
            {
                (void)lx2(LX_KILL, rchild, LX_SIGCHLD);  /* 兜底，别挂死 */
            }

            rrc = lx6(LX_FUTEX, (lx_long)(unsigned long)cv,
                      LX_FUTEX_CMP_REQUEUE_PI, 0, 1,
                      (lx_long)(unsigned long)mx, 0);
            check("futex(CMP_REQUEUE_PI) handed the mutex over == 1", rrc, 1);
            check("futex(CMP_REQUEUE_PI) wrote the child tid into the mutex",
                  (lx_long)(*mx & LX_FUTEX_TID_MASK), (lx_long)(*ctidv));
            check("futex(CMP_REQUEUE_PI) left no stray WAITERS",
                  (lx_long)(*mx & LX_FUTEX_WAITERS), 0);

            /* 放行子进程：它会解锁并退出，再核对位掩码与最终的字。 */
            *gov = 1ul;
            (void)lx3(LX_FUTEX, (lx_long)(unsigned long)gov, LX_FUTEX_WAKE, 1);
            rstatus = -1;
            check("futex rqpi child reaped",
                  lx4(LX_WAIT4, rchild, (lx_long)(unsigned long)&rstatus, 0, 0),
                  rchild);
            check("futex rqpi child saw the handoff (bits=7)",
                  (rstatus >> 8) & 0xFF, 7);
            check("futex rqpi mutex cleared after the inherited unlock",
                  (lx_long)*mx, 0);
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
                     * 每次重试先让步 5 ms（同样必须整毫秒，0.5 ms 会被截断
                     * 成 0，等于空转不让步），1000 轮 = 最坏 5 s，与父进程
                     * 60 轮 × 100 ms = 6 s 的窗口对得上。 */
                    for (tries = 0; tries < 1000; tries++)
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
                         * 没人等它）睡 5 ms。本机没有 sched_yield 也没有
                         * nanosleep，FUTEX_WAIT 带 timeout 是唯一能主动交出
                         * CPU 的路；栈上的字没有键（回 -ENOSYS），所以必须
                         * 用这张映射页里的字。 */
                        yld[0] = 0;
                        yld[1] = 5000000L;
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

        /* 这条管道带 O_NONBLOCK 造：下面第 (5) 组要「一直写到位满」，
         * 只有非阻塞档才会在满的时候回 -EAGAIN。U-26 之后默认阻塞的
         * write 会挂起等读端腾空（本段没有读端在跑），会一直睡下去。
         * select 的就绪判据（有容量 / EOF）与 fd 的 O_NONBLOCK 位无关。 */
        check("pselect6 pipe2 == 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)sel, LX_O_NONBLOCK), 0);

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
     *      真的被释放）；
     *  10) socket 的 dup（U-30）：副本与源共享同一个 net 句柄 —— 关副本
     *      不影响源、关源之后副本接管句柄、最后一份关掉才真正关 socket。
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
        lx_long  dupc;
        lx_long  dupd;
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

        /* ---- U-30：socket 的 dup —— 副本与源共享同一个 net 句柄 ----
         * 判据分三层，缺一层都能被「假的复制」糊过去：
         *   a) dup 给回一个不同的新号；
         *   b) 关掉副本，源 fd 仍然能问出绑定端口（关副本时**不能**通知
         *      协议栈，否则源会被兄弟副本打哑）；
         *   c) 反过来关源 fd，副本必须接管那个句柄，仍然问得出 8081；
         *      最后一份关掉时才真正关 socket（由下面既有的 close 完成）。 */
        dupc = lx1(LX_DUP, srv);
        check("socket dup returns fd >= 3", (dupc >= 3) ? 1 : 0, 1);
        check("socket dup returns a different fd", (dupc != srv) ? 1 : 0, 1);
        check("close the dup copy == 0", lx1(LX_CLOSE, dupc), 0);
        lenp = 0u;
        r = lx3(LX_GETSOCKNAME, srv, (lx_long)(unsigned long)&pa,
                (lx_long)(unsigned long)&lenp);
        check("source fd still works after closing the dup copy", r, 0);
        check("source fd still bound to 8081",
              ((lx_long)pa.b[2] << 8) | (lx_long)pa.b[3], SK_PORT);

        dupd = lx1(LX_DUP, srv);
        check("second dup returns fd >= 3", (dupd >= 3) ? 1 : 0, 1);
        check("close the original fd == 0", lx1(LX_CLOSE, srv), 0);
        lenp = 0u;
        r = lx3(LX_GETSOCKNAME, dupd, (lx_long)(unsigned long)&pa,
                (lx_long)(unsigned long)&lenp);
        check("dup copy survives the original close", r, 0);
        check("dup copy still bound to 8081",
              ((lx_long)pa.b[2] << 8) | (lx_long)pa.b[3], SK_PORT);
        srv = dupd;         /* 后面那条 close 关的就是最后一份 */

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
        /* srv 这时是**最后一份**副本：这一关才真正通知协议栈。 */
        check("close the last copy == 0", lx1(LX_CLOSE, srv), 0);
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

        /* U-32：非组长线程发起 execve 必须被明确拒绝（Linux 会成功并收整组，
         * 本内核换不掉 tid，所以不假装——见已知限制）；组长自己发起时这道
         * 闸门不该拦，用一个不存在的路径验到 -ENOENT（装载器 ERR_FS）。 */
        {
            unsigned int eptid = 0xFFFFFFFFu;
            lx_long  et;

            g_ex_rc   = 0x7eadbeef;
            g_ex_ctid = 0x7eadbeef;
            et = lx_clone_thread_call(
                     (lx_long)(LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES |
                               LX_CLONE_SIGHAND | LX_CLONE_THREAD |
                               LX_CLONE_PARENT_SETTID | LX_CLONE_CHILD_CLEARTID),
                     (lx_long)(unsigned long)(s_thr2_stack + sizeof(s_thr2_stack)),
                     (lx_long)(unsigned long)&eptid,
                     (lx_long)(unsigned long)&g_ex_ctid, 0, exec_thread_body);
            check("exec thread clone > 0", (et > 0) ? 1 : 0, 1);
            if (et > 0)
            {
                int n;

                for (n = 0; n < 400; n++)
                {
                    if (g_ex_ctid == 0)
                    {
                        break;
                    }
                    (void)lx4(LX_FUTEX,
                              (lx_long)(unsigned long)&g_ex_ctid, LX_FUTEX_WAIT,
                              g_ex_ctid, 0);
                }
            }
            check("execve from non-leader thread == -EINVAL", g_ex_rc,
                  -LX_EINVAL);
        }
        check("execve from the leader passes the group gate (ENOENT)",
              lx2(LX_EXECVE, (lx_long)(unsigned long)"/no/such/prog", 0),
              -LX_ENOENT);

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

    /* 抢占即死回归：线程在 RUNNING 状态下被节拍抢占若干次后必须还活着。
     * 两个同优先级线程同时忙等，时间片一到必然互切（见 preempt_body 上方
     * 注释），bug 版每次跑都会把被切走的那个静默摘除。 */
    {
        unsigned int ptid = 0xFFFFFFFFu;
        unsigned int qtid = 0xFFFFFFFFu;
        lx_long  thr;
        lx_long  peer;
        lx_long  flags = (lx_long)(LX_CLONE_VM | LX_CLONE_FS | LX_CLONE_FILES |
                                   LX_CLONE_SIGHAND | LX_CLONE_THREAD |
                                   LX_CLONE_PARENT_SETTID |
                                   LX_CLONE_CHILD_CLEARTID);

        g_pre_ran   = 0;
        g_pre_ctid  = 0x5eed1234;    /* 非零：清零才能证明线程正常退出 */
        g_pre2_ran  = 0;
        g_pre2_ctid = 0x5eed5678;
        thr = lx_clone_thread_call(
                  flags,
                  (lx_long)(unsigned long)(s_pre_stk + sizeof(s_pre_stk)),
                  (lx_long)(unsigned long)&ptid,
                  (lx_long)(unsigned long)&g_pre_ctid, 0, preempt_body);
        check("preempting thread spawns > 0", (thr > 0) ? 1 : 0, 1);
        check("preempting thread PARENT_SETTID wrote the tid",
              (lx_long)ptid, thr);
        peer = lx_clone_thread_call(
                  flags,
                  (lx_long)(unsigned long)(s_pre2_stk + sizeof(s_pre2_stk)),
                  (lx_long)(unsigned long)&qtid,
                  (lx_long)(unsigned long)&g_pre2_ctid, 0, preempt_body2);
        check("preempting peer thread spawns > 0", (peer > 0) ? 1 : 0, 1);
        check("preempting peer PARENT_SETTID wrote the tid",
              (lx_long)qtid, peer);
        if (thr > 0)
        {
            pre_join(&g_pre_ctid);
        }
        if (peer > 0)
        {
            pre_join(&g_pre2_ctid);
        }
        check("preempted thread left its CLEARTID at 0", g_pre_ctid, 0);
        check("preempted thread did the work", (g_pre_ran != 0) ? 1 : 0, 1);
        check("preempted peer left its CLEARTID at 0", g_pre2_ctid, 0);
        check("preempted peer did the work", (g_pre2_ran != 0) ? 1 : 0, 1);
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
            /* U-26 之后管道默认阻塞：这一次 read 会直接挂起，等到父进程写
             * 放行字节、或写端全关时回 EOF(0)。下面的 futex 轮询是防御
             * 路径（只有 O_NONBLOCK 档才可能回 -EAGAIN），读到 EOF 才退出；
             * 睡的是内核自己记账的 futex 超时，不读用户态时钟。 */
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

/* ------------------------------------------------------------------
     * 记录锁（fcntl F_GETLK / F_SETLK / F_SETLKW，U-27）
     * ------------------------------------------------------------------
     * 宿主用例只有一个进程槽，只能读「锁表容量」；跨进程的那几条语义必须
     * 在这里真开两个进程槽才验得出来：
     *   1) 父进程握着的锁对子进程是真的冲突（F_SETLK 回 -EAGAIN），而
     *      自己的锁不与自己冲突；
     *   2) F_GETLK 把**别人的**锁原样报回来：l_type / l_pid 是持锁进程，
     *      l_whence 归一成 SEEK_SET，l_start 是绝对偏移；
     *   3) F_SETLKW 真的挂起：子进程睡在 SETLKW 上，父进程 100 ms 之后才
     *      解锁，子进程的墙钟读数必须 >= 40 ms（不是失败就立刻返回，也不是
     *      根本没挂起）；
     *   4) SEEK_END 的换算由**另一个进程**读出来：子进程用 SEEK_END(-10,10)
     *      锁文件尾，父进程用 SEEK_SET(4086,10) 问，命中同一把锁；
     *   5) close 按文件释放：子进程一关 fd，它在**这个文件**上的三把锁
     *      （冲突段 / 不相交段 / 文件尾段）全部消失，父进程逐个拿得回来。
     * 反假绿：第 5 条三把锁分三次判 —— 只判其中一把的话，「关 fd 时只放掉
     * 最后一把」或者「按进程一刀切、顺手把父的锁也放掉」都能混过去。
     */
    {
        int            pc[2];       /* parent -> child：放行 / 回执 */
        int            cp[2];       /* child  -> parent：报告 */
        lx_long        pfd;
        lx_long        kid;
        lx_long        st;
        lx_long        r;
        struct lk_flock fl;
        char           b;

        check("lock pipe2 barrier -> 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)pc, 0), 0);
        check("lock pipe2 report -> 0",
              lx2(LX_PIPE2, (lx_long)(unsigned long)cp, 0), 0);
        pfd = lx3(LX_OPENAT, LX_AT_FDCWD, (lx_long)(unsigned long)LK_PATH,
                  LX_O_RDONLY);
        check("lock probe file open > 2", (pfd > 2) ? 1 : 0, 1);

        if ((pfd > 2) && (pc[0] >= 0) && (cp[0] >= 0))
        {
            /* 父进程先握一把写锁，然后才 fork：锁在子进程出现之前就在表里。 */
            lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
            check("lock parent F_SETLK [0,100)", lk_call(pfd, LX_F_SETLK, &fl), 0);

            /* 自己的锁查出来不算冲突：只把 l_type 改成 F_UNLCK。 */
            lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
            check("lock parent F_GETLK on own lock",
                  lk_call(pfd, LX_F_GETLK, &fl), 0);
            check("lock own lock is not a conflict", (lx_long)fl.l_type, LX_UNLCK);

            kid = lx3(LX_CLONE, LX_SIGCHLD, 0, 0);
            if (kid == 0)
            {
                lk_child(pc, cp, pfd);
                (void)lx1(LX_EXIT, 0);
                for (;;)
                {
                }
            }
            check("lock fork for the waiter > 0", (kid > 0) ? 1 : 0, 1);

            if (kid > 0)
            {
                /* 子进程把两把不冲突的锁备好了才算数（'D' 之前别抢跑）。 */
                (void)lx3(LX_READ, (lx_long)cp[0],
                          (lx_long)(unsigned long)&b, 1);

                /* 不相交的那把：父进程用 SEEK_SET(200,100) 问，命中子进程 */
                lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 200, 100);
                check("lock parent GETLK child disjoint",
                      lk_call(pfd, LX_F_GETLK, &fl), 0);
                check("lock parent GETLK disjoint pid is child",
                      (lx_long)fl.l_pid, kid);
                check("lock parent GETLK disjoint start is 200", fl.l_start, 200);

                /* SEEK_END 换算的另一半：同一段用 SEEK_SET 问，还是它 */
                lk_set(&fl, LX_WRLCK, LX_SEEK_SET, LK_SIZE - 10, 10);
                check("lock parent GETLK eof region",
                      lk_call(pfd, LX_F_GETLK, &fl), 0);
                check("lock parent GETLK eof pid is child",
                      (lx_long)fl.l_pid, kid);

                /* 回执：子进程等这一字节才往下走到 SETLKW。没有它两边会
                 * 各自等各自（子等父的放行、父等子的「要睡了」）。 */
                (void)lx3(LX_WRITE, (lx_long)pc[1],
                          (lx_long)(unsigned long)"G", 1);

                /* 等子进程说「马上要睡」，再拖 100 ms 才解锁：读数据是可证伪的 */
                (void)lx3(LX_READ, (lx_long)cp[0],
                          (lx_long)(unsigned long)&b, 1);
                lk_busy_ms(100);
                lk_set(&fl, LX_UNLCK, LX_SEEK_SET, 0, 100);
                check("lock parent unlock [0,100)",
                      lk_call(pfd, LX_F_SETLK, &fl), 0);

                /* 子进程拿到锁之后再看一眼：现在这把锁的主人是它 */
                (void)lx3(LX_READ, (lx_long)cp[0],
                          (lx_long)(unsigned long)&b, 1);
                lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
                check("lock parent GETLK child blocking lock",
                      lk_call(pfd, LX_F_GETLK, &fl), 0);
                check("lock parent GETLK blocking pid is child",
                      (lx_long)fl.l_pid, kid);
                (void)lx3(LX_WRITE, (lx_long)pc[1],
                          (lx_long)(unsigned long)"X", 1);

                /* 子进程关掉 fd 之后，三把锁必须一把不剩 */
                (void)lx3(LX_READ, (lx_long)cp[0],
                          (lx_long)(unsigned long)&b, 1);
                lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 0, 100);
                check("lock free after child close (conflict range)",
                      lk_call(pfd, LX_F_SETLK, &fl), 0);
                lk_set(&fl, LX_WRLCK, LX_SEEK_SET, 200, 100);
                check("lock free after child close (disjoint range)",
                      lk_call(pfd, LX_F_SETLK, &fl), 0);
                lk_set(&fl, LX_WRLCK, LX_SEEK_SET, LK_SIZE - 10, 10);
                check("lock free after child close (eof range)",
                      lk_call(pfd, LX_F_SETLK, &fl), 0);

                st = -1;
                r  = -1;
                for (;;)
                {
                    r = lx4(LX_WAIT4, kid, (lx_long)(unsigned long)&st, 0, 0);
                    if ((r == kid) || (r < 0))
                    {
                        break;
                    }
                }
                check("lock child reaped", r, kid);
                check("lock child exit code 0", (st >> 8) & 0xFF, 0);
            }

            (void)lx1(LX_CLOSE, pfd);
        }

        (void)lx1(LX_CLOSE, (lx_long)pc[0]);
        (void)lx1(LX_CLOSE, (lx_long)pc[1]);
        (void)lx1(LX_CLOSE, (lx_long)cp[0]);
        (void)lx1(LX_CLOSE, (lx_long)cp[1]);
    }

#if defined(ARKRTOS_USE_LINUX_SCHED) && (ARKRTOS_USE_LINUX_SCHED == 1)
    /* U-28：调度策略十二个号的 ABI 往返读数（不在 fork 计数里） */
    sched_probe();
#endif

#if defined(ARKRTOS_USE_LINUX_SIGIO) && (ARKRTOS_USE_LINUX_SIGIO == 1)
    /* U-29：异步 I/O 的登记 / O_ASYNC / SIGIO 投递（不在 fork 计数里） */
    sio_probe();
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
