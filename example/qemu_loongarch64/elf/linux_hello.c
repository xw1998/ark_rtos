/**
* @file linux_hello.c
* @brief 按 Linux 系统调用 ABI 写的静态程序（QEMU LoongArch64）
* @details 这个文件里**没有一行 Ark SDK 的调用**：所有请求都走 Linux 的
*          LoongArch 约定（`syscall 0` + $a7 放系统调用号，参数在 a0..a5，
*          返回值在 a0，负值 = -errno），号是 asm-generic 的
*          56/64/93/113/160/172/214/222，和 Linux 上编出来的静态程序一致。
*
*          它验证的是兼容层的每一格：
*            1) write(1, ...)        -> 控制台真的收到字节；
*            2) uname(&buf)          -> 内核能往用户缓冲区写结构化数据；
*            3) clock_gettime       -> CLOCK_MONOTONIC 有读数、
*                                      CLOCK_REALTIME 明确回 -ENOSYS；
*             3b) getrandom 278 -------> 未实现的号必须明确回 -ENOSYS，
*                                      不认识的号不许看起来成功；
*            4) getpid              -> 非 0；
*            5) brk(0) / brk(+4096) -> 窗口内堆可增长；
*            6) mmap(匿名 4096)     -> 返回窗口内地址、可读写；
*            7) openat + read       -> 用 Linux fd 读 ramfs 里的文件
*                                      （读到的应该是本程序的 ELF 头 0x7F 'E' 'L' 'F'）；
*            8) exit(7)             -> 退出码 7 回到 shell 的 exec。
*          任何一格失败都打印 FAIL 并带着非 0 退出码结束——不把"没做"说成"成功"。
*
* @note 不链 libc、不链启动代码。入口仍是 Linux 的 [argc][argv..][NULL]
*       布局，_start 汇编垫片把入口 sp 交给 C（理由见 hello.c）。
* @author xw
*/

/* asm-generic 号表（Linux UAPI） */
#define LX_OPENAT           56
#define LX_CLOSE            57
#define LX_READ             63
#define LX_WRITE            64
#define LX_EXIT             93
#define LX_CLOCK_GETTIME    113
#define LX_UNAME            160
#define LX_GETPID           172
#define LX_BRK              214
#define LX_MMAP             222
#define LX_GETRANDOM        278     /* 本内核没实现：用来验「未知号」这一格 */

#define LX_EINVAL           22
#define LX_ENOSYS           38

#define LX_PROT_READ        0x1
#define LX_PROT_WRITE       0x2
#define LX_MAP_PRIVATE      0x02
#define LX_MAP_ANONYMOUS    0x20

#define LX_CLOCK_MONOTONIC  1

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
 * 极小的运行时（没有 libc）
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
    out("[linux] ");
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
    struct utsname_local
    {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    } uts;
    long ts[2];
    char path[] = "/mnt/ram/linux_hello";
    unsigned char magic[4];
    lx_long fd;
    lx_long got;
    lx_long heap0;
    lx_long heap1;
    lx_long p;
    lx_long pid;
    int i;

    (void)sp;   /* 本程序不读 argv：兼容层的入口证据在 hello.c 里 */

    out("[linux] hello from the Linux syscall ABI (a7=64 write)\n");

    /* 2) uname */
    for (i = 0; i < (int)sizeof(uts); i++)
    {
        ((char *)&uts)[i] = 0;
    }
    got = lx1(LX_UNAME, (lx_long)(unsigned long)&uts);
    check("uname", got, 0);
    out("[linux] sysname=");
    out(uts.sysname);
    out(" machine=");
    out(uts.machine);
    out("\n");

    /* 3) clock_gettime：单调钟有读数，墙钟明确不支持 */
    got = lx3(LX_CLOCK_GETTIME, LX_CLOCK_MONOTONIC, (lx_long)(unsigned long)ts, 0);
    check("clock_gettime(MONOTONIC)", got, 0);
    if (got == 0)
    {
        out("[linux] monotonic sec=");
        out_num(ts[0]);
        out(" nsec=");
        out_num(ts[1]);
        out("\n");
    }
    got = lx3(LX_CLOCK_GETTIME, 0, (lx_long)(unsigned long)ts, 0);
    check("clock_gettime(REALTIME)", got, -LX_ENOSYS);

    /* 3b) 一个**根本没实现**的号（getrandom 278）。这一格验的是「未知号
     * 必须明确回 -ENOSYS」，而它也是按号直方图（lxstat）存在的理由：
     * 真实发行版二进制卡住的地方就在这类号上，要先把号拿到才谈得上实现。 */
    check("getrandom(unknown nr)", lx3(LX_GETRANDOM, 0, 8, 0), -LX_ENOSYS);

    /* 4) getpid */
    pid = lx1(LX_GETPID, 0);
    check("getpid==task id>0", (pid > 0) ? 1 : 0, 1);

    /* 5) brk */
    heap0 = lx1(LX_BRK, 0);
    check("brk(0)>0", (heap0 > 0) ? 1 : 0, 1);
    heap1 = lx1(LX_BRK, heap0 + 4096);
    check("brk(+4096)", heap1, heap0 + 4096);

    /* 6) mmap：匿名私有 4 KB，写进去再读回来 */
    p = lx4(LX_MMAP, 0, 4096, LX_PROT_READ | LX_PROT_WRITE,
            LX_MAP_PRIVATE | LX_MAP_ANONYMOUS);
    check("mmap>0", (p > 0) ? 1 : 0, 1);
    if (p > 0)
    {
        volatile unsigned char *q = (volatile unsigned char *)(unsigned long)p;

        q[0] = 0xA5u;
        q[4095] = 0x5Au;
        check("mmap write/read", (q[0] == 0xA5u && q[4095] == 0x5Au) ? 1 : 0, 1);
    }

    /* 7) openat + read：读自己的 ELF 头（0x7F 'E' 'L' 'F'） */
    fd = lx4(LX_OPENAT, -100, (lx_long)(unsigned long)path, 0, 0);
    check("openat>0", (fd > 0) ? 1 : 0, 1);
    if (fd > 0)
    {
        got = lx3(LX_READ, fd, (lx_long)(unsigned long)magic, 4);
        check("read 4 bytes", got, 4);
        check("ELF magic", (magic[0] == 0x7Fu && magic[1] == 'E' &&
                            magic[2] == 'L' && magic[3] == 'F') ? 1 : 0, 1);
        check("close", lx1(LX_CLOSE, fd), 0);

        /* 负例：读一个不存在的文件必须回 -ENOENT，而不是给个假句柄 */
        check("openat(missing)", lx4(LX_OPENAT, -100,
                                     (lx_long)(unsigned long)"/mnt/ram/nope", 0, 0),
              -2);
    }

    if (g_fail == 0)
    {
        out("[linux] PASS: every syscall answered as specified\n");
    }
    else
    {
        out("[linux] FAIL: ");
        out_num(g_fail);
        out(" checks failed\n");
    }

    /* 8) exit(7)：退出码要一路回到 shell 的 exec */
    (void)lx1(LX_EXIT, (g_fail == 0) ? 7 : 1);

    for (;;)
    {
    }
}
