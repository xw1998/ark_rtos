/**
* @file linux_execve.c
* @brief 按 Linux 系统调用 ABI 写的 execve 夹具（QEMU LoongArch64）
* @details 与 linux_hello.c 一样，这个文件没有一行 Ark SDK 调用：所有请求走
*          `syscall 0` + $a7 的 LoongArch Linux 约定。它要证的是 execve（221）
*          在**同一个任务**上换掉当前镜像时的四件事：
*
*            1) 任务号不变：stage2 打印的 pid 必须等于 stage1 传下去的 pid
*               （pid 以十进制串经 argv[2] 传过去，不靠"看起来像"）；
*            2) 镜像真的被重装：一个 .data 里的全局变量在 stage1 被改成
*               0xDEADBEEF，stage2 必须读回文件里的初值 1。只改 trap 帧、
*               不重写段的实现会在这里露馅；
*            3) fd 保留：stage1 用 openat 拿到 fd（应为 3）并**不关闭**，
*               stage2 直接从 fd 3 读 4 字节，必须是本程序的 ELF 魔数；
*            4) 堆游标归零：stage1 记下 brk(0) 的基点并把它推后 8 KB，
*               再由 argv[3] 把基点传下去；stage2 的 brk(0) 必须回到基点
*               （旧游标的地址语义属于旧镜像）。
*
*          另外两条反假绿：
*            - stage1 先 execve 一个不存在的路径，必须回 -ENOENT(2) 且程序
*              还能继续往下跑（失败是可返回的，旧镜像仍在）；
*            - execve 若"返回"了任何值，都按失败处理并打印 —— Linux 语义下
*              成功不返回。
*
* @note 不链 libc、不链启动代码，入口仍是 Linux 的 [argc][argv..][NULL]。
* @author xw
*/

#define LX_OPENAT           56
#define LX_READ             63
#define LX_WRITE            64
#define LX_EXIT             93
#define LX_GETPID           172
#define LX_BRK              214
#define LX_EXECVE           221

#define LX_ENOENT           2

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

/* 十进制无符号 -> 串（调用方保证 buf 至少 24 字节） */
static void u_to_str(unsigned long u, char *buf)
{
    char tmp[24];
    int  i = 0;
    int  j = 0;

    if (u == 0ul)
    {
        buf[0] = '0';
        buf[1] = '\0';
        return;
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
        buf[j] = tmp[i];
        j++;
    }
    buf[j] = '\0';
}

static unsigned long str_to_u(const char *s)
{
    unsigned long u = 0ul;

    while ((*s >= '0') && (*s <= '9'))
    {
        u = (u * 10ul) + (unsigned long)(*s - '0');
        s++;
    }
    return u;
}

static int str_eq(const char *a, const char *b)
{
    while ((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }
    return (*a == *b) ? 1 : 0;
}

static int g_fail = 0;

static void check(const char *what, lx_long got, lx_long want)
{
    out("[execve] ");
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

/* .data 里的标记：stage1 改成 0xDEADBEEF，stage2 必须读回 1（文件初值）。
 * 只改 trap 帧不重写 PT_LOAD 的实现会在这里被抓住。 */
volatile unsigned long g_image_marker = 1ul;

static void stage1(const char *self);

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
    unsigned long argc = *((unsigned long *)(unsigned long)sp);
    char **argv = (char **)(unsigned long)(sp + 8ul);
    const char *self;

    self = ((argc >= 1ul) && (argv[0] != 0)) ? argv[0] : "/mnt/ram/linux_execve";

    if ((argc >= 2ul) && (argv[1] != 0) && (str_eq(argv[1], "stage2") != 0))
    {
        /* ---------------- stage2：新镜像里的第一段代码 ---------------- */
        lx_long pid = lx1(LX_GETPID, 0);
        lx_long hb;
        lx_long want_pid = -1;
        lx_long want_hb = -1;
        unsigned char magic[4];

        out("[execve] stage2 pid=");
        out_num(pid);
        out("\n");

        if ((argc >= 4ul) && (argv[2] != 0) && (argv[3] != 0))
        {
            want_hb  = (lx_long)str_to_u(argv[2]);
            want_pid = (lx_long)str_to_u(argv[3]);
        }

        /* 1) 同一任务 */
        check("stage2 pid==stage1 pid", pid, want_pid);

        /* 2) 镜像被重装（.data 的初值回来了） */
        check("image reloaded (.data marker)", (lx_long)g_image_marker, 1);

        /* 3) fd 保留：fd 3 是 stage1 打开的同一份文件 */
        (void)lx3(LX_READ, 3, (lx_long)(unsigned long)magic, 4);
        check("fd 3 survived (ELF magic)",
              (lx_long)((magic[0] == 0x7Fu && magic[1] == 'E' &&
                         magic[2] == 'L' && magic[3] == 'F') ? 1 : 0), 1);

        /* 4) 堆游标归零：brk(0) 回到 stage1 记下的基点 */
        hb = lx1(LX_BRK, 0);
        out("[execve] stage2 brk(0)=");
        out_num(hb);
        out("\n");
        check("heap cursor reset", hb, want_hb);

        if (g_fail == 0)
        {
            out("[execve] PASS: same task, image reloaded, fd kept, heap reset\n");
            (void)lx1(LX_EXIT, 42);
        }
        else
        {
            out("[execve] FAIL: ");
            out_num(g_fail);
            out(" checks failed\n");
            (void)lx1(LX_EXIT, 1);
        }
        for (;;)
        {
        }
    }

    stage1(self);
}

static void stage1(const char *self)
{
    char heap_str[24];
    char pid_str[24];
    char *new_argv[5];
    lx_long pid;
    lx_long fd;
    lx_long hb;
    lx_long rc;

    out("[execve] stage1 pid=");
    pid = lx1(LX_GETPID, 0);
    out_num(pid);
    out("\n");

    /* fd：从 3 起分配，且 execve 之后必须还在 */
    fd = lx4(LX_OPENAT, -100, (lx_long)(unsigned long)self, 0, 0);
    out("[execve] stage1 fd=");
    out_num(fd);
    out("\n");
    check("stage1 fd==3", fd, 3);

    /* 留一个只能被"重装段"抹掉的痕迹 */
    g_image_marker = 0xDEADBEEFul;

    /* 把堆游标推后 8 KB，新镜像应回到基点 */
    hb = lx1(LX_BRK, 0);
    out("[execve] stage1 brk(0)=");
    out_num(hb);
    out("\n");
    check("stage1 brk(+8192)", lx1(LX_BRK, hb + 8192), hb + 8192);

    /* 反假绿一：不存在的路径必须回 -ENOENT，而且程序还能继续跑 */
    new_argv[0] = (char *)self;
    new_argv[1] = 0;
    rc = lx3(LX_EXECVE, (lx_long)(unsigned long)"/mnt/ram/nope",
             (lx_long)(unsigned long)&new_argv[0], 0);
    check("execve(missing) == -ENOENT", rc, -LX_ENOENT);

    /* 正式的 execve：同一个文件、argv[1]="stage2" + 两个十进制串 */
    u_to_str((unsigned long)hb, heap_str);
    u_to_str((unsigned long)pid, pid_str);
    new_argv[1] = (char *)"stage2";
    new_argv[2] = heap_str;
    new_argv[3] = pid_str;
    new_argv[4] = 0;

    out("[execve] stage1 calling execve(");
    out(self);
    out(")\n");
    rc = lx3(LX_EXECVE, (lx_long)(unsigned long)self,
             (lx_long)(unsigned long)&new_argv[0], 0);

    /* 反假绿二：Linux 的 execve 成功不返回，返回任何值都是失败 */
    out("[execve] FAIL: execve returned ");
    out_num(rc);
    out(" (success must not return)\n");
    (void)lx1(LX_EXIT, 1);
    for (;;)
    {
    }
}