/**
* @file linux_pipe.c
* @brief 管道夹具（QEMU LoongArch64，Linux ABI）
* @details 和 linux_hello.c / linux_execve.c 一样，这个文件没有一行 Ark SDK
*          调用：全部走 `syscall 0` + `$a7` 的 LoongArch Linux 约定。它要证的
*          是 pipe2（59）这一档的行为，每一条都必须是可证伪的读数。管道
*          的默认语义是**阻塞**（与 Linux 一致，O_NONBLOCK 才是显式档），
*          所以夹具分成两组：
*
*          A. O_NONBLOCK 档（能读/写多少算多少 + -EAGAIN 显式失败）：
*            1) pipe2 成功并回两个 fd（读端在先）；
*            2) 写进去的字节原样读回来（内容逐字节比对，不看长度了事）；
*            3) 空管道读回 -EAGAIN（-11），不许回 0 冒充 EOF；
*            4) 写端关掉之后读回 0（EOF），与 Linux 一致；
*            5) 带 O_CLOEXEC 的 pipe2 被接受（本内核已在 execve 换帧后
*               按 FD_CLOEXEC 还槽，这个位是真承诺而不是记账）；
*            6) 缓冲满时是**短写**（返回实际写进去的字节数），不是假装写全；
*               读端全关后写回 -EPIPE（-32）。
*
*          B. 默认阻塞档（flags=0，本档是单执行流能证的集合）：
*            7) 有数据时阻塞读立刻回（不因为「阻塞」就多等一个时间片）；
*            8) 写端关掉之后阻塞读立刻回 0（EOF 是返回条件，不是等待条件）；
*            9) 读端全关后阻塞写回 -EPIPE，并置 SIGPIPE——SIG_IGN 时只记一次
*               忽略，装处理函数时在系统调用返回前进处理函数（a0 == 13），
*               sigreturn 之后 a0 仍是 -EPIPE。
*
* @note 单执行流无法证「阻塞读等到别人写」这类跨执行流行为，那一组读数在
*       linux_fork.c 里（fork 档才构建）。这里只证返回条件与 SIGPIPE。
*
* @note 不链 libc、不链启动代码，入口仍是 Linux 的 [argc][argv..][NULL]。
* @author xw
*/

#define LX_CLOSE            57
#define LX_PIPE2            59
#define LX_READ             63
#define LX_WRITE            64
#define LX_EXIT             93
#define LX_RT_SIGACTION     134
#define LX_RT_SIGRETURN     139

#define LX_EAGAIN           11
#define LX_EINVAL           22
#define LX_EPIPE            32

#define LX_O_CLOEXEC        0x80000
#define LX_O_NONBLOCK       0x800

#define SIGPIPE             13
#define SIG_IGN             1ul
#define SIGSET_BYTES        8

#define PIPE_BUF_SIZE       256

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
    out("[pipe] ");
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
 * 静态数据
 * ------------------------------------------------------------------ */
static char s_buf[512];
static char s_big[400];

/* ------------------------------------------------------------------
 * SIGPIPE：处理函数与 restorer（写法与 linux_signal.c 同一套）
 * ------------------------------------------------------------------ */

/* Linux 的 rt_sigaction 结构（loongarch64 UAPI）：32 字节
 * { handler, flags, restorer, mask(8 B) }。 */
typedef struct
{
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
} act_t;

static int     s_pipe_hits;
static lx_long s_pipe_last;

extern void sig_restorer(void);

/* handler 不能返回调用点：内核把 ra 指向 restorer，restorer 发
 * rt_sigreturn（139）把被打断的系统调用现场装回去（a0 回到 -EPIPE）。 */
static void on_sigpipe(unsigned long signo)
{
    s_pipe_hits++;
    s_pipe_last = (lx_long)signo;
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
    int     fds[2];        /* Linux pipe2 的 out 参数是 int[2]，不是 long[2] */
    lx_long rc;
    lx_long i;
    int     same;

    (void)sp;

    /* ---------------- 第 1 条：pipe2 成功 + 两个 fd ---------------- */
    s_buf[0] = '\0';
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, LX_O_NONBLOCK);
    check("pipe2 rc", rc, 0);
    out("[pipe] fds = ");
    out_num(fds[0]);
    out(" ");
    out_num(fds[1]);
    out("\n");
    check("fds are two increasing numbers", ((fds[1] > fds[0]) && (fds[0] >= 3)) ? 1 : 0, 1);

    /* ---------------- 第 2 条：写进去的原样读回来 ---------------- */
    {
        const char *msg = "hello";
        unsigned long n = 5ul;

        rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)msg, (lx_long)n);
        check("write 5 bytes", rc, 5);

        for (i = 0; i < (lx_long)sizeof(s_buf); i++)
        {
            s_buf[i] = 0;
        }
        rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 5);
        check("read 5 bytes", rc, 5);

        same = 1;
        for (i = 0; i < 5; i++)
        {
            if (s_buf[i] != msg[i])
            {
                same = 0;
            }
        }
        check("content matches", same, 1);
    }

    /* ---------------- 第 3 条：空管道读回 -EAGAIN ---------------- */
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("read empty -> -EAGAIN", rc, -LX_EAGAIN);

    /* ---------------- 第 4 条：写端关掉后读回 EOF ---------------- */
    rc = lx1(LX_CLOSE, fds[1]);
    check("close write end", rc, 0);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("read after writer closed -> EOF", rc, 0);
    (void)lx1(LX_CLOSE, fds[0]);

    /* ---------------- 第 5 条：O_CLOEXEC 被接受 ---------------- */
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds,
             LX_O_CLOEXEC | LX_O_NONBLOCK);
    check("pipe2(O_CLOEXEC) rc", rc, 0);
    check("pipe2(O_CLOEXEC) hands out two fds",
          ((fds[0] >= 3) && (fds[1] >= 3)) ? 1 : 0, 1);
    (void)lx1(LX_CLOSE, fds[0]);
    (void)lx1(LX_CLOSE, fds[1]);

    /* ---------------- 第 6 条：满时短写 + 读端关掉后 -EPIPE ------------- */
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, LX_O_NONBLOCK);
    check("second pipe2 rc", rc, 0);

    for (i = 0; i < (lx_long)sizeof(s_big); i++)
    {
        s_big[i] = 'a';
    }
    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)s_big, 400);
    check("write 400 into 256 buffer -> short write", rc, PIPE_BUF_SIZE);

    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 400);
    check("read back what fits", rc, PIPE_BUF_SIZE);

    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)s_big, 400);
    check("write again after drain", rc, PIPE_BUF_SIZE);

    (void)lx1(LX_CLOSE, fds[0]);

    /* 读端全关的写现在会置 SIGPIPE（U-26）：默认动作是结束任务（退出码
     * 141），所以先把它忽略掉，才能读到 -EPIPE 这个返回值。真投递那一路
     * 在第 9 条里装处理函数证。 */
    {
        act_t act;

        act.handler  = SIG_IGN;
        act.flags    = 0ul;
        act.restorer = 0ul;
        act.mask     = 0ul;
        rc = lx4(LX_RT_SIGACTION, SIGPIPE, (lx_long)(unsigned long)&act, 0,
                 SIGSET_BYTES);
        check("sigaction(SIGPIPE, SIG_IGN) before EPIPE", rc, 0);
    }

    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)s_big, 4);
    check("write with reader closed -> -EPIPE", rc, -LX_EPIPE);
    (void)lx1(LX_CLOSE, fds[1]);

    /* ---------------- 第 7 条：默认阻塞档（flags=0） ----------------
     * 单执行流只能证「有返回条件」的那几条：有数据就回、写端全关回 EOF、
     * 读端全关回 -EPIPE + SIGPIPE。「无事可做就挂起、等到对侧才回」需要
     * 第二个执行流，那一组读数在 linux_fork.c 里。 */
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, 0);
    check("blocking pipe2 rc", rc, 0);

    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)"block", 5);
    check("blocking write with data present", rc, 5);

    for (i = 0; i < (lx_long)sizeof(s_buf); i++)
    {
        s_buf[i] = 0;
    }
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 5);
    check("blocking read with data present", rc, 5);

    same = 1;
    for (i = 0; i < 5; i++)
    {
        if (s_buf[i] != "block"[i])
        {
            same = 0;
        }
    }
    check("blocking read content matches", same, 1);

    rc = lx1(LX_CLOSE, fds[1]);
    check("blocking close write end", rc, 0);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("blocking read after writer closed -> EOF", rc, 0);
    (void)lx1(LX_CLOSE, fds[0]);

    /* ---------------- 第 8 条：SIG_IGN 时 -EPIPE 不挂死 ---------------- */
    {
        act_t act;

        act.handler  = SIG_IGN;
        act.flags    = 0ul;
        act.restorer = 0ul;
        act.mask     = 0ul;
        rc = lx4(LX_RT_SIGACTION, SIGPIPE, (lx_long)(unsigned long)&act, 0,
                 SIGSET_BYTES);
        check("sigaction(SIGPIPE, SIG_IGN)", rc, 0);
    }

    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, 0);
    check("blocking pipe2 (EPIPE) rc", rc, 0);
    (void)lx1(LX_CLOSE, fds[0]);
    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)"x", 1);
    check("blocking write, reader closed, SIG_IGN -> -EPIPE", rc, -LX_EPIPE);

    /* ---------------- 第 9 条：SIGPIPE 真投递 ----------------
     * 投递点在「系统调用返回用户态之前」：handler 先跑一次（a0 == 13），
     * 它 sigreturn 之后 a0 装回这次 write 的返回值 -EPIPE。两条一起证，
     * 才排除「handler 跑了但返回值被覆盖」和「返回值对了但没投递」。 */
    {
        act_t act;

        act.handler  = (unsigned long)(unsigned long)&on_sigpipe;
        act.flags    = 0ul;
        act.restorer = (unsigned long)(unsigned long)&sig_restorer;
        act.mask     = 0ul;
        rc = lx4(LX_RT_SIGACTION, SIGPIPE, (lx_long)(unsigned long)&act, 0,
                 SIGSET_BYTES);
        check("sigaction(SIGPIPE, handler)", rc, 0);
    }

    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)"xyzw", 4);
    check("blocking write, reader closed, handler -> -EPIPE", rc, -LX_EPIPE);
    check("SIGPIPE handler ran once", (lx_long)s_pipe_hits, 1);
    check("SIGPIPE handler got signo", s_pipe_last, SIGPIPE);
    rc = lx1(LX_CLOSE, fds[1]);
    check("blocking close write end (EPIPE pipe)", rc, 0);

    if (s_fail == 0)
    {
        out("[pipe] PASS: pipe2 two fds, data round trip, EAGAIN on empty, EOF on writer close, O_CLOEXEC accepted, short write and EPIPE, blocking defaults and SIGPIPE delivery\n");
        (void)lx1(LX_EXIT, 55);
    }
    else
    {
        out("[pipe] FAIL: ");
        out_num((lx_long)s_fail);
        out(" checks failed\n");
        (void)lx1(LX_EXIT, 1);
    }
    for (;;)
    {
    }
}
