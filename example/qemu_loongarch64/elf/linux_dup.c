/**
* @file linux_dup.c
* @brief dup / dup3 夹具（QEMU LoongArch64，Linux ABI）
* @details 和 linux_pipe.c 一样，这个文件没有一行 Ark SDK 调用：全部走
*          `syscall 0` + `$a7` 的 LoongArch Linux 约定。它要证的是 dup(23) 与
*          dup3(24) 这一档的行为，每一条都是可证伪的读数：
*
*            1) dup 成功，回一个**新的** fd 号（>=3 且不等于源号）；
*            2) 写进副本的字节能从原读端读到（管道对象是同一个）；
*            3) 原写端关掉之后副本仍然可写（引用是按 fd 记的，不是按名字）；
*            4) 所有写端都关掉之后读端读到 EOF；
*            5) dup3(pw, 1, 0) 把 stdout 指到管道，write(1, ...) 真的落进去；
*            6) close(1) 把重定向撤掉（之后 fd 1 回到控制台）；
*            7) dup3 覆盖一个已经在用的 fd（Linux 语义：先关掉再指过去），
*               被覆盖的写端不再能写；
*            8) dup(0) 造出一个控制台引用：读它回 EOF；
*            9) 失败面：dup(不存在的号) / dup3(old==new) /
*               dup3(..., O_CLOEXEC) / dup3(不存在的号, 1) 各回什么。
*
* @note 本夹具的输出走 **fd 2**：第 5 组用例会把 fd 1 重定向到管道，用 fd 1
*       打印的话读数会跑进那条管道里，被自己吃掉。fd 2 这一档与 fd 1 走同一
*       条控制台路径，读数照样落在串口上。
* @note 不链 libc、不链启动代码，入口仍是 Linux 的 [argc][argv..][NULL]。
* @author xw
*/

#define LX_DUP              23
#define LX_DUP3             24
#define LX_CLOSE            57
#define LX_PIPE2            59
#define LX_READ             63
#define LX_WRITE            64
#define LX_EXIT             93

#define LX_EBADF            9
#define LX_EINVAL           22

#define LX_O_CLOEXEC        0x80000

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

/* ------------------------------------------------------------------
 * 极小运行时（没有 libc）；输出走 fd 2，理由见文件头
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
    out("[dup] ");
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

static lx_long match(const char *a, const char *b, lx_long n)
{
    lx_long i;

    for (i = 0; i < n; i++)
    {
        if (a[i] != b[i])
        {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------
 * 静态数据
 * ------------------------------------------------------------------ */
static char s_buf[64];

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
    int     fds[2];
    lx_long d;
    lx_long rc;
    lx_long i;

    (void)sp;

    /* ---------------- 第 1 组：dup 复制一个管道写端 ---------------- */
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, 0);
    check("pipe2 rc", rc, 0);

    d = lx1(LX_DUP, fds[1]);
    check("dup(writer) is a new fd", ((d >= 3) && (d != (lx_long)fds[1])) ? 1 : 0, 1);

    rc = lx3(LX_WRITE, d, (lx_long)(unsigned long)"abcde", 5);
    check("write through the copy", rc, 5);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 5);
    check("read from the original reader", rc, 5);
    check("content matches", match(s_buf, "abcde", 5), 1);

    /* 原写端关掉：副本是独立的 fd，管子里的计数只减一份 */
    rc = lx1(LX_CLOSE, fds[1]);
    check("close the original writer", rc, 0);
    rc = lx3(LX_WRITE, d, (lx_long)(unsigned long)"xyz", 3);
    check("copy still writable after close", rc, 3);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("read the second write", rc, 3);
    check("second content matches", match(s_buf, "xyz", 3), 1);

    /* 最后一个写端关掉 -> 读回 EOF */
    rc = lx1(LX_CLOSE, d);
    check("close the copy", rc, 0);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("read after all writers closed -> EOF", rc, 0);
    rc = lx1(LX_CLOSE, fds[0]);
    check("close the reader", rc, 0);

    /* ---------------- 第 2 组：失败面 ---------------- */
    rc = lx1(LX_DUP, 999);
    check("dup(999) -> -EBADF", rc, -LX_EBADF);
    rc = lx3(LX_DUP3, 1, 1, 0);
    check("dup3(1, 1, 0) -> -EINVAL", rc, -LX_EINVAL);
    rc = lx3(LX_DUP3, 1, 6, LX_O_CLOEXEC);
    check("dup3(..., O_CLOEXEC) -> -EINVAL", rc, -LX_EINVAL);
    rc = lx3(LX_DUP3, 999, 1, 0);
    check("dup3(999, 1, 0) -> -EBADF", rc, -LX_EBADF);

    /* ---------------- 第 3 组：dup3(pw, 1) 重定向 stdout ------------- */
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, 0);
    check("second pipe2 rc", rc, 0);
    rc = lx3(LX_DUP3, fds[1], 1, 0);
    check("dup3(writer, 1) returns 1", rc, 1);
    rc = lx3(LX_WRITE, 1, (lx_long)(unsigned long)"ZZZ", 3);
    check("write(1) lands in the pipe", rc, 3);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("read the redirected write", rc, 3);
    check("redirected content matches", match(s_buf, "ZZZ", 3), 1);

    /* 撤掉重定向：fd 1 回到控制台，写端计数只剩原来那一个 */
    rc = lx1(LX_CLOSE, 1);
    check("close(1) drops the alias", rc, 0);
    rc = lx1(LX_CLOSE, fds[1]);
    check("close the original writer too", rc, 0);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("read after both writers gone -> EOF", rc, 0);
    rc = lx1(LX_CLOSE, fds[0]);
    check("close the reader", rc, 0);

    /* ---------------- 第 4 组：dup3 覆盖一个在用的 fd ---------------- */
    rc = lx2(LX_PIPE2, (lx_long)(unsigned long)fds, 0);
    check("third pipe2 rc", rc, 0);
    rc = lx3(LX_DUP3, fds[0], fds[1], 0);
    check("dup3(reader, writer) overwrites", rc, (lx_long)fds[1]);
    rc = lx3(LX_WRITE, fds[1], (lx_long)(unsigned long)"q", 1);
    check("overwritten fd is no longer a writer", rc, -LX_EBADF);
    rc = lx3(LX_READ, fds[0], (lx_long)(unsigned long)s_buf, 8);
    check("overwrite dropped the only writer -> EOF", rc, 0);
    rc = lx1(LX_CLOSE, fds[1]);
    check("close the overwritten fd", rc, 0);
    rc = lx1(LX_CLOSE, fds[0]);
    check("close the reader", rc, 0);

    /* ---------------- 第 5 组：dup(0) 造一个控制台引用 ---------------- */
    d = lx1(LX_DUP, 0);
    check("dup(0) is a console reference", (d >= 3) ? 1 : 0, 1);
    for (i = 0; i < (lx_long)sizeof(s_buf); i++)
    {
        s_buf[i] = 0;
    }
    rc = lx3(LX_READ, d, (lx_long)(unsigned long)s_buf, 4);
    check("read the console reference -> EOF", rc, 0);
    rc = lx1(LX_CLOSE, d);
    check("close the console reference", rc, 0);

    if (s_fail == 0)
    {
        out("[dup] PASS: dup new fd, data through the copy, alias survives close, "
            "dup3 redirects stdout, dup3 overwrites, console reference, "
            "EBADF/EINVAL on the four failure surfaces\n");
        (void)lx1(LX_EXIT, 56);
    }
    else
    {
        out("[dup] FAIL: ");
        out_num((lx_long)s_fail);
        out(" checks failed\n");
        (void)lx1(LX_EXIT, 1);
    }
    for (;;)
    {
    }
}
