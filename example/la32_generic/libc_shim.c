/**
* @file libc_shim.c
* @brief 裸机 LoongArch64 的最小 C 运行库替代
* @details 目标工具链只有一个 64 位 multilib（lp64d），可以链 newlib，
*          但那样会拖进 crt0 与一堆 syscall 桩（_sbrk/_write/...），而
*          QEMU bring-up 既没有堆也没有文件描述符。所以这里保持"自足实现"
*          这条路线，只是把长度参数写成 size_t 的宽度（unsigned long）：
*          内核源码里 include <string.h>，newlib 的原型按 size_t 声明，
*          参数宽度不一致会被判 conflicting types 而编译不过。
*
*          只实现「被真正引用到的」这一小撮，函数语义按 C 标准写，
*          不做任何取巧（不做 SIMD、不假设对齐）——这几个函数每秒被调用
*          的次数以百计，正确性比多省几条指令重要。
*
* @note 一旦将来换到带 rv32imac 多库的工具链，可以把本文件从构建里去掉，
*       改成正常链接 newlib；在此之前这是唯一选项，不是权宜写法。
* @author xw
*/

#include "arkrtos_types.h"

void *memcpy(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while(n > 0u)
    {
        *d = *s;
        d++;
        s++;
        n--;
    }

    return dst;
}

void *memmove(void *dst, const void *src, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if((d == s) || (n == 0u))
    {
        return dst;
    }

    if(d < s)
    {
        while(n > 0u)
        {
            *d = *s;
            d++;
            s++;
            n--;
        }
    }
    else
    {
        d += n;
        s += n;
        while(n > 0u)
        {
            d--;
            s--;
            *d = *s;
            n--;
        }
    }

    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)dst;

    while(n > 0u)
    {
        *d = (unsigned char)c;
        d++;
        n--;
    }

    return dst;
}

int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    while(n > 0u)
    {
        if(*pa != *pb)
        {
            return (int)(*pa) - (int)(*pb);
        }
        pa++;
        pb++;
        n--;
    }

    return 0;
}

unsigned long strlen(const char *s)
{
    unsigned long n = 0u;

    while(s[n] != '\0')
    {
        n++;
    }

    return n;
}

int strcmp(const char *a, const char *b)
{
    while((*a != '\0') && (*a == *b))
    {
        a++;
        b++;
    }

    return (int)(unsigned char)(*a) - (int)(unsigned char)(*b);
}

int strncmp(const char *a, const char *b, unsigned long n)
{
    while((n > 0u) && (*a != '\0') && (*a == *b))
    {
        a++;
        b++;
        n--;
    }

    if(n == 0u)
    {
        return 0;
    }

    return (int)(unsigned char)(*a) - (int)(unsigned char)(*b);
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;

    while(*src != '\0')
    {
        *d = *src;
        d++;
        src++;
    }
    *d = '\0';

    return dst;
}

char *strncpy(char *dst, const char *src, unsigned long n)
{
    char *d = dst;

    while((n > 0u) && (*src != '\0'))
    {
        *d = *src;
        d++;
        src++;
        n--;
    }

    while(n > 0u)
    {
        *d = '\0';
        d++;
        n--;
    }

    return dst;
}

char *strchr(const char *s, int c)
{
    char ch = (char)c;

    for(;;)
    {
        if(*s == ch)
        {
            return (char *)s;
        }
        if(*s == '\0')
        {
            return 0;
        }
        s++;
    }
}

/* ============================================================
 * 格式化输出：vsnprintf / snprintf
 * @details 内核里有两处用到（日志头、softbus 的名字拼装），没有 libc 就
 *          必须自带一份。只实现整数/字符串/字符/指针这几类，浮点一律
 *          返回 -1 并把输出截断为空 —— 不做"看起来像成功"的近似：
 *          这颗芯片没有 FPU，也没有任何地方打印浮点。
 *          返回值遵守 C99：返回"若缓冲区足够大本应写入的字符数"（不含
 *          结尾 0），因此可以先用小缓冲试探长度。
 * ============================================================ */

typedef __builtin_va_list rv_va_list;

typedef struct
{
    char  *buf;
    unsigned long cap;      /* 缓冲区容量（含结尾 0） */
    unsigned int len;      /* 已写入字符数（可能超过 cap，用于返回值） */
} rv_fmt_out_t;

static void rv_out_ch(rv_fmt_out_t *o, char c)
{
    if(o->len + 1u < o->cap)
    {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void rv_out_pad(rv_fmt_out_t *o, char c, int n)
{
    while(n > 0)
    {
        rv_out_ch(o, c);
        n--;
    }
}

/* 无符号整数按进制输出，返回数字串长度；buf 至少 34 字节 */
static int rv_utoa(unsigned long v, unsigned int base, int upper, char *buf)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[34];
    int n = 0;
    int i;

    if(v == 0ul)
    {
        buf[0] = '0';
        return 1;
    }

    while(v != 0ul)
    {
        tmp[n] = digits[(unsigned int)(v % (unsigned long)base)];
        v /= (unsigned long)base;
        n++;
    }

    for(i = 0; i < n; i++)
    {
        buf[i] = tmp[n - 1 - i];
    }

    return n;
}

int vsnprintf(char *buf, unsigned long cap, const char *fmt, rv_va_list ap)
{
    rv_fmt_out_t o;

    o.buf = buf;
    o.cap = (cap == 0u) ? 0u : cap;
    o.len = 0u;

    while(*fmt != '\0')
    {
        int left = 0;            /* '-': 左对齐 */
        int zero = 0;            /* '0': 补零 */
        int width = 0;
        int prec = -1;
        int is_long = 0;
        char conv;

        if(*fmt != '%')
        {
            rv_out_ch(&o, *fmt);
            fmt++;
            continue;
        }
        fmt++;

        for(;;)
        {
            if(*fmt == '-')      { left = 1; fmt++; }
            else if(*fmt == '0') { zero = 1; fmt++; }
            else if(*fmt == '+') { fmt++; }   /* 无符号实现：忽略 */
            else if(*fmt == ' ') { fmt++; }
            else if(*fmt == '#') { fmt++; }
            else { break; }
        }

        while((*fmt >= '0') && (*fmt <= '9'))
        {
            width = width * 10 + (int)(*fmt - '0');
            fmt++;
        }

        if(*fmt == '.')
        {
            prec = 0;
            fmt++;
            while((*fmt >= '0') && (*fmt <= '9'))
            {
                prec = prec * 10 + (int)(*fmt - '0');
                fmt++;
            }
        }

        /* 长度修饰：本实现把 long / long long / size_t 归一成 unsigned long
         * （ilp32 下都是 32 位），h/hh 也按 32 位处理 —— 由调用方自行截断。 */
        while((*fmt == 'l') || (*fmt == 'h') || (*fmt == 'z') || (*fmt == 't'))
        {
            if(*fmt == 'l') { is_long = 1; }
            fmt++;
        }
        (void)is_long;

        conv = *fmt;
        if(conv == '\0')
        {
            break;
        }
        fmt++;

        if(conv == '%')
        {
            rv_out_ch(&o, '%');
            continue;
        }

        if((conv == 'f') || (conv == 'F') || (conv == 'g') || (conv == 'G') ||
           (conv == 'e') || (conv == 'E'))
        {
            /* 没有浮点支持：明确失败，不打印假数字 */
            if(o.cap > 0u) { o.buf[0] = '\0'; }
            return -1;
        }

        if((conv == 's') || (conv == 'c'))
        {
            char tmp[2];
            const char *s;
            int n;

            if(conv == 'c')
            {
                tmp[0] = (char)__builtin_va_arg(ap, int);
                tmp[1] = '\0';
                s = tmp;
            }
            else
            {
                s = __builtin_va_arg(ap, const char *);
                if(s == 0)
                {
                    s = "(null)";
                }
            }

            n = 0;
            while((s[n] != '\0') && ((prec < 0) || (n < prec)))
            {
                n++;
            }
            if(!left) { rv_out_pad(&o, ' ', width - n); }
            for(int i = 0; i < n; i++)
            {
                rv_out_ch(&o, s[i]);
            }
            if(left) { rv_out_pad(&o, ' ', width - n); }
            continue;
        }

        if((conv == 'd') || (conv == 'i') || (conv == 'u') ||
           (conv == 'x') || (conv == 'X') || (conv == 'o') || (conv == 'p'))
        {
            char num[40];
            unsigned long v;
            int n;
            int neg = 0;
            unsigned int base = 10u;
            int upper = 0;
            int zeros;

            if((conv == 'd') || (conv == 'i'))
            {
                long sv = (long)__builtin_va_arg(ap, int);
                if(sv < 0)
                {
                    neg = 1;
                    v = (unsigned long)(-sv);
                }
                else
                {
                    v = (unsigned long)sv;
                }
            }
            else if(conv == 'p')
            {
                v = (unsigned long)__builtin_va_arg(ap, void *);
                base = 16u;
            }
            else
            {
                v = __builtin_va_arg(ap, unsigned int);
                if(conv == 'x') { base = 16u; }
                else if(conv == 'X') { base = 16u; upper = 1; }
                else if(conv == 'o') { base = 8u; }
            }

            n = rv_utoa(v, base, upper, num);

            /* 精度给的是"最少位数"，不足补零（与 width 的补零不同） */
            zeros = 0;
            if((prec >= 0) && (prec > n))
            {
                zeros = prec - n;
            }

            if(!left && !zero)
            {
                rv_out_pad(&o, ' ', width - n - zeros - neg);
            }
            if(neg)
            {
                rv_out_ch(&o, '-');
            }
            if(!left && zero)
            {
                rv_out_pad(&o, '0', width - n - zeros - neg);
            }
            rv_out_pad(&o, '0', zeros);
            for(int i = 0; i < n; i++)
            {
                rv_out_ch(&o, num[i]);
            }
            if(left)
            {
                rv_out_pad(&o, ' ', width - n - zeros - neg);
            }
            continue;
        }

        /* 不认识的转换：原样吐出，便于暴露而不是静默吞掉 */
        rv_out_ch(&o, '%');
        rv_out_ch(&o, conv);
    }

    if(o.cap > 0u)
    {
        unsigned int end = (o.len < o.cap - 1u) ? o.len : (o.cap - 1u);
        o.buf[end] = '\0';
    }

    return (int)o.len;
}

int snprintf(char *buf, unsigned long cap, const char *fmt, ...)
{
    rv_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = vsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);

    return n;
}

/* ============================================================
 * 字符串函数补件：littlefs 用到 strspn / strcspn / strrchr / strpbrk
 * @details lfs_dir_find() 用 strspn/strcspn 把路径按 "/" 切成段；VFS 与
 *          内核 Shell 的路径处理用 strrchr（找最后一个 '/'）与 strpbrk。
 *          语义按 C 标准；返回值是 size_t（本目标 ilp32 下 32 位）。
 * ============================================================ */

unsigned long strspn(const char *s, const char *accept)
{
    unsigned long n = 0u;

    while(s[n] != '\0')
    {
        const char *a = accept;

        while((*a != '\0') && (*a != s[n]))
        {
            a++;
        }

        if(*a == '\0')
        {
            break;
        }
        n++;
    }

    return n;
}

unsigned long strcspn(const char *s, const char *reject)
{
    unsigned long n = 0u;

    while(s[n] != '\0')
    {
        const char *r = reject;

        while((*r != '\0') && (*r != s[n]))
        {
            r++;
        }

        if(*r != '\0')
        {
            break;
        }
        n++;
    }

    return n;
}

char *strrchr(const char *s, int c)
{
    char ch = (char)c;
    const char *last = 0;

    for(;;)
    {
        if(*s == ch)
        {
            last = s;
        }
        if(*s == '\0')
        {
            break;
        }
        s++;
    }

    return (char *)last;
}

char *strpbrk(const char *s, const char *accept)
{
    while(*s != '\0')
    {
        const char *a = accept;

        while(*a != '\0')
        {
            if(*a == *s)
            {
                return (char *)s;
            }
            a++;
        }
        s++;
    }

    return 0;
}

/* ============================================================
 * libgcc 内置函数补件（32/64 位）
 * @details 全程 -nostdlib，没有 libgcc；GCC 会把 __builtin_bswap* /
 *          __builtin_popcount* / __builtin_clz* / __builtin_ctz* 打桩成
 *          对 libgcc 的调用。littlefs 在 ilp32 下按 32 位字做 popcount，
 *          实测缺的是 __popcountsi2 与 __bswapsi2；64 位那三个照样给出，
 *          因为 GCC 对 unsigned long long 的同类内建在 ilp32 下也会落到
 *          __popcountdi2 / __bswapdi2。语义与 libgcc 一致：
 *          零输入的 clz/ctz 返回位宽（libgcc 的实际取值）。
 *          名字是 ABI 名字，不能改。
 * ============================================================ */

unsigned int __bswapsi2(unsigned int v)
{
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8)  |
           ((v & 0x00FF0000u) >> 8)  |
           ((v & 0xFF000000u) >> 24);
}

unsigned long long __bswapdi2(unsigned long long v)
{
    return ((v & 0x00000000000000FFULL) << 56) |
           ((v & 0x000000000000FF00ULL) << 40) |
           ((v & 0x0000000000FF0000ULL) << 24) |
           ((v & 0x00000000FF000000ULL) << 8)  |
           ((v & 0x000000FF00000000ULL) >> 8)  |
           ((v & 0x0000FF0000000000ULL) >> 24) |
           ((v & 0x00FF000000000000ULL) >> 40) |
           ((v & 0xFF00000000000000ULL) >> 56);
}

int __popcountsi2(unsigned int v)
{
    int n = 0;

    while(v != 0u)
    {
        v &= (v - 1u);
        n++;
    }

    return n;
}

int __popcountdi2(unsigned long long v)
{
    int n = 0;

    while(v != 0ull)
    {
        v &= (v - 1ull);
        n++;
    }

    return n;
}

int __clzsi2(unsigned int v)
{
    int n = 0;
    unsigned int m = 0x80000000u;

    while((n < 32) && ((v & m) == 0u))
    {
        m >>= 1;
        n++;
    }

    return n;
}

int __ctzsi2(unsigned int v)
{
    int n = 0;
    unsigned int m = 0x00000001u;

    while((n < 32) && ((v & m) == 0u))
    {
        m <<= 1;
        n++;
    }

    return n;
}
