/**
* @file arkrtos_mini_rt_arkc.c
* @brief 小程序 SDK 的 arkc 运行时：POSIX 控制台写出 + 线程退出
* @details 这个小程序为什么需要它，而 Keil / GCC 的小程序不需要：
*
*          Keil / GCC 的小程序把 `sdk/app_sdk/arkrtos_oslib.c` 链接进来，
*          那份门面把内核的每一个服务都封成 SVC 调用。arkc 编得动它（实测
*          17 KB 对象），但小程序是**整份负载搬进 RAM 执行**的，把一个用不到
*          的服务门面整块塞进 8 KB 代码块里，剩下的空间就不够写程序了。
*
*          所以这里只保留小程序真正需要的两个服务，SVC 号与 oslib 逐位相同
*          （不是新 ABI，只是同一张表上的两条）：
*
*            0x1A 子命令 3  arkrtos_shell_print(const char *msg)   控制台
*            0x1B 子命令 3  arkrtos_thread_exit(void)              线程退出
*
*          对照点：`kernelsrc/sdk/app_sdk/arkrtos_oslib.c` 是这两条的唯一
*          权威实现，本文件只是它们在 arkc + 小程序场景下的最小副本。两边
*          的 SVC 号若有一天不一致，两个文件都会在同一处被改到（这里和那里
*          都引用了同样的服务号）。
*
*          【为什么 write 要拷进缓冲】
*          SVC 0x1A 的控制台接口收的是 NUL 结尾的字符串，而 POSIX 的
*          write(fd, buf, len) 允许任意缓冲、任意长度。这里按需分块拷贝并
*          补 '\0'，所以调用方可以传非字符串的内存。已知限制写在下面。
*
* @note 只给 arkc 用。Keil / GCC 的小程序请用 app_sdk 的 oslib。
*/

/* 与 oslib 相同的两个服务，写成 arkc 认得的 __svc 形式。
 *
 * 宿主门禁（tests/test_printf_host.c）用 gcc 直接把本文件编进去跑格式化器：
 * gcc 不认 __svc，所以非 arkc 档只留两条普通声明，由那里提供桩。 */
typedef unsigned int u32;

#if defined(__ARK_CC__)
int __svc(0x1A) arkrtos_call_shell_svc(u32 *p);
int __svc(0x1B) arkrtos_call_thread_ctrl(u32 *p);
#else
int arkrtos_call_shell_svc(u32 *p);
int arkrtos_call_thread_ctrl(u32 *p);
#endif

/* 一次拷多少字节进缓冲。控制台是给人看的，96 字节够一行；大块输出会被
 * 拆成多次 SVC，每次都是完整的提示输出，不会与别的任务交错到半个字。 */
#define MINI_RT_CHUNK   96

static char g_chunk[MINI_RT_CHUNK];

/**
* @brief POSIX write：fd 1/2 写控制台，其余返回 -1
* @return 实际交给内核的字节数（= len），失败 -1
* @note 已知限制：控制台接口按 NUL 结尾字符串输出，所以负载里出现 0 字节时
*       只有它之前的部分会出来。往控制台打二进制本来就没有意义，这里不假装
*       支持（不转义、不拆分），如实记在文档里。
*/
int write(int fd, const void *buf, unsigned int len)
{
    const char  *p    = (const char *)buf;
    unsigned int done = 0u;
    u32          q[3];

    if ((fd != 1) && (fd != 2))
    {
        return -1;
    }
    if (buf == (const void *)0)
    {
        return -1;
    }

    while (done < len)
    {
        unsigned int n = len - done;
        unsigned int i;

        if (n > (MINI_RT_CHUNK - 1u))
        {
            n = MINI_RT_CHUNK - 1u;
        }
        for (i = 0u; i < n; i++)
        {
            g_chunk[i] = p[done + i];
        }
        g_chunk[n] = 0;

        q[0] = 3u; q[1] = (u32)g_chunk; q[2] = 0u;
        (void)arkrtos_call_shell_svc(q);

        done += n;
    }

    return (int)len;
}

/**
* @brief POSIX _exit：小程序没有进程，交还自己占的两块 RAM
* @note 与 app_sdk 的 arkrtos_thread_exit() 是同一条 SVC。main() 正常返回时
*       由启动桩调用它，程序里显式调用也合法（提前结束）。
*/
void _exit(int code)
{
    u32 q[3];

    (void)code;
    q[0] = 3u; q[1] = 0u; q[2] = 0u;
    (void)arkrtos_call_thread_ctrl(q);

    /* 内核不会再调度这个任务；真回来了就停住，不往前跑到别人的代码上。 */
    for (;;)
    {
    }
}

/**
* @brief App SDK 的退出接口，供 sdk/mini_sdk/arkrtos_mini_main.c 使用
* @note 那个文件按 SDK 约定调 arkrtos_thread_exit()。Keil / GCC 的小程序由
*       oslib 提供它；arkc 的小程序由本文件提供，语义完全相同。
*/
void arkrtos_thread_exit(void)
{
    _exit(0);
}

/* ============================================================
 * stdio 子集：printf / puts / putchar
 *
 * 【为什么 printf 是一个 __asm 函数】
 * arkc 没有 va_list：源码里写不出 va_start/va_arg，所以格式化器不能像桌面
 * 上那样顺着参数指针走。但调用约定是确定的（AAPCS）：前 4 个参数在 r0-r3，
 * 第 5 个起由调用方压在自己栈上、与 r0-r3 一起构成连续的一串字。于是这里用
 * 一条 __asm 函数当一个“取参桩”：序言把 r0-r3 与调用方压栈的字落到自己栈上
 * 的 12 个槽里，再把这块内存交给下面那个普通 C 函数按格式串取值。
 *
 * 对调用方来说 printf 就是普通变参函数，一个宏都不用。
 *
 * 【硬上限 12】
 * 桩只搬 12 个字：格式串 + 11 个参数。第 13 个起取不到（会读到 0）。编译器
 * 本身允许更多参数，所以这条上限是 SDK 的，写在 stdio.h 与演示文档里。
 *
 * 【栈对齐】
 * 调用方在 BL 之前把 sp 对齐到 8 字节；桩 PUSH {r4, lr}（8 字节）再 SUB (48)
 * 之后仍然对齐，槽 0..11 占 [sp+0..47]，调用方的第 5 个参数（即槽 4）起在
 * [sp+56]，于是最后一个可用槽 11 在 [sp+84]。
 * ============================================================ */

#define MINI_PF_SLOTS   12          /* 格式串 + 11 个参数 */
#define MINI_PF_LINE    96

static char g_pf_line[MINI_PF_LINE];
static int  g_pf_len;
static int  g_pf_total;

static void pf_flush(void)
{
    if (g_pf_len > 0)
    {
        g_pf_line[g_pf_len] = 0;
        (void)write(1, g_pf_line, (unsigned int)g_pf_len);
        g_pf_total += g_pf_len;
        g_pf_len = 0;
    }
}

static void pf_ch(char c)
{
    if (g_pf_len >= (MINI_PF_LINE - 1))
    {
        pf_flush();
    }
    g_pf_line[g_pf_len] = c;
    g_pf_len++;
}

static void pf_pad(int n, char c)
{
    while (n > 0)
    {
        pf_ch(c);
        n--;
    }
}

/* 取一个参数；越界返回 0 并照样推进游标，保证后面的转换不会串位 */
static u32 pf_arg(const u32 *w, int *ai)
{
    u32 v = 0u;

    if (*ai < MINI_PF_SLOTS)
    {
        v = w[*ai];
    }
    (*ai)++;
    return v;
}

/* 无符号数转字符串，返回长度（不含 NUL）。base 只支持 10 / 16。 */
static int pf_utoa(u32 v, u32 base, int upper, char *out)
{
    char tmp[12];
    int  n = 0;
    int  i;

    do
    {
        u32 d = v % base;
        if (d < 10u)
        {
            tmp[n] = (char)('0' + d);
        }
        else
        {
            tmp[n] = (char)((upper ? 'A' : 'a') + (d - 10u));
        }
        n++;
        v = v / base;
    } while (v != 0u);

    for (i = 0; i < n; i++)
    {
        out[i] = tmp[n - 1 - i];
    }
    out[n] = 0;
    return n;
}

/**
* @brief printf 的格式化主体：按格式串从 w[] 里取参数
* @param fmt 格式串
* @param w   12 个字：w[0] 是格式串本身（桩一起落进槽里），参数从 w[1] 起
* @return 写出的字符数
* @note 只给本文件的 printf 桩调用，不要直接call
*/
int arkrtos_mini_printf_words(const char *fmt, const u32 *w)
{
    int ai = 1;

    g_pf_len   = 0;
    g_pf_total = 0;
    if (fmt == (const char *)0)
    {
        return 0;
    }

    while (*fmt != 0)
    {
        char c = *fmt;
        int  left = 0;
        int  zero = 0;
        int  width = 0;
        int  neg = 0;
        int  len;
        int  i;
        u32  mag;
        u32  base = 10u;
        int  upper = 0;
        char conv;
        char tmp[16];
        const char *str;

        fmt++;
        if (c != '%')
        {
            pf_ch(c);
            continue;
        }

        for (;;)
        {
            if (*fmt == '-')
            {
                left = 1;
                fmt++;
            }
            else if (*fmt == '0')
            {
                zero = 1;
                fmt++;
            }
            else
            {
                break;
            }
        }
        while ((*fmt >= '0') && (*fmt <= '9'))
        {
            width = width * 10 + (int)(*fmt - '0');
            if (width > 40)
            {
                width = 40;
            }
            fmt++;
        }

        conv = *fmt;
        if (conv == 0)
        {
            break;                 /* 末尾孤零零一个 '%'：不输出 */
        }
        fmt++;

        if (conv == '%')
        {
            pf_ch('%');
            continue;
        }
        if (conv == 'c')
        {
            char ch = (char)(pf_arg(w, &ai) & 0xFFu);
            if (left == 0)
            {
                pf_pad(width - 1, ' ');
            }
            pf_ch(ch);
            if (left != 0)
            {
                pf_pad(width - 1, ' ');
            }
            continue;
        }
        if (conv == 's')
        {
            str = (const char *)pf_arg(w, &ai);
            if (str == (const char *)0)
            {
                str = "(null)";
            }
            len = 0;
            while (str[len] != 0)
            {
                len++;
            }
            if (left == 0)
            {
                pf_pad(width - len, ' ');
            }
            for (i = 0; i < len; i++)
            {
                pf_ch(str[i]);
            }
            if (left != 0)
            {
                pf_pad(width - len, ' ');
            }
            continue;
        }

        if (conv == 'd' || conv == 'i')
        {
            int sv = (int)pf_arg(w, &ai);
            if (sv < 0)
            {
                neg = 1;
                /* 取负走无符号：-2147483648 反过来会溢出 */
                mag = 0u - (u32)sv;
            }
            else
            {
                mag = (u32)sv;
            }
        }
        else if (conv == 'u')
        {
            mag = pf_arg(w, &ai);
        }
        else if (conv == 'x' || conv == 'X')
        {
            mag   = pf_arg(w, &ai);
            base  = 16u;
            upper = (conv == 'X') ? 1 : 0;
        }
        else if (conv == 'p')
        {
            mag    = pf_arg(w, &ai);
            base   = 16u;
            zero   = 1;
            width  = 8;
            pf_ch('0');
            pf_ch('x');
        }
        else
        {
            /* 不认识的转换：原样打出来，不吞掉（静默吞掉最难查） */
            pf_ch('%');
            pf_ch(conv);
            continue;
        }

        {
            int digits = pf_utoa(mag, base, upper, tmp);
            int total  = digits + ((neg != 0) ? 1 : 0);

            if (left == 0 && zero == 0)
            {
                pf_pad(width - total, ' ');
            }
            if (neg != 0)
            {
                pf_ch('-');
            }
            if (left == 0 && zero != 0)
            {
                pf_pad(width - total, '0');
            }
            for (i = 0; i < digits; i++)
            {
                pf_ch(tmp[i]);
            }
            if (left != 0)
            {
                pf_pad(width - total, ' ');
            }
        }
    }

    pf_flush();
    return g_pf_total;
}

int puts(const char *s)
{
    int n = 0;

    if (s == (const char *)0)
    {
        return -1;
    }
    while (s[n] != 0)
    {
        n++;
    }
    (void)write(1, s, (unsigned int)n);
    (void)write(1, "\n", 1u);
    return n + 1;
}

int putchar(int c)
{
    char ch = (char)(c & 0xFF);

    (void)write(1, &ch, 1u);
    return (int)ch;
}

/*
 * printf 本体：取参桩。
 *
 * 进函数时（调用方已把 sp 对齐到 8 字节）：
 *   r0 = 格式串，r1..r3 = 参数 1..3，[sp+0..] = 参数 4..
 * 本函数把 12 个字落到自己的栈上（槽 0 = 格式串），再把 &slot[0] 交给
 * arkrtos_mini_printf_words()。r0 里剩的就是它的返回值。
 *
 * 只搬 8 个栈参数（槽 4..11）：这是 SDK 的硬上限，见 stdio.h。
 */
#if defined(__ARK_CC__)
__asm int printf(const char *fmt, ...)
{
    PUSH {r4, lr}
    SUB  sp, sp, #48
    STR  r0, [sp, #0]
    STR  r1, [sp, #4]
    STR  r2, [sp, #8]
    STR  r3, [sp, #12]
    LDR  r0, [sp, #56]
    STR  r0, [sp, #16]
    LDR  r0, [sp, #60]
    STR  r0, [sp, #20]
    LDR  r0, [sp, #64]
    STR  r0, [sp, #24]
    LDR  r0, [sp, #68]
    STR  r0, [sp, #28]
    LDR  r0, [sp, #72]
    STR  r0, [sp, #32]
    LDR  r0, [sp, #76]
    STR  r0, [sp, #36]
    LDR  r0, [sp, #80]
    STR  r0, [sp, #40]
    LDR  r0, [sp, #84]
    STR  r0, [sp, #44]
    MOV  r1, sp
    LDR  r0, [sp, #0]
    BL   arkrtos_mini_printf_words
    ADD  sp, sp, #48
    POP  {r4, pc}
}
#endif /* __ARK_CC__ —— 取参桩是 ARM 汇编：宿主门禁直接调
          arkrtos_mini_printf_words()，不需要它，gcc 也编不了 __asm。 */
