/**
 * @file arkrtos_utest.c
 * @brief 板端单元测试框架的实现：注册表 + 运行器 + 输出汇
 * @details 不依赖 shell / printf / libc：数字格式化成字节写进一行缓冲再整体
 *          交给输出汇，因此同一份源码既能在目标上跑，也能在宿主机上编译自测。
 *          行缓冲是 static（非重入）——用例本来就串行执行，且运行器不在中断里。
 *
 * @note 本模块属于内核特权态代码。
 */
#include "arkrtos_utest.h"

#if (ARKRTOS_USE_UTEST == 1)

#define UTEST_LINE_MAX 160

/* ---- 注册表 ---- */
static ark_utest_case_t s_cases[ARKRTOS_UTEST_MAX_CASES];
static uint32_t         s_count;
static uint32_t         s_overflow;   /* 装不下的用例数：报出来，不静默丢弃 */

/* ---- 输出 ---- */
static ark_utest_out_t  s_out;

/* ---- 当前用例上下文 ---- */
typedef struct
{
    const char *name;
    uint32_t    checks;
    uint32_t    failed;
    int         skipped;
} utest_ctx_t;

static utest_ctx_t *s_cur;

/* ============================================================
 * 行缓冲：极小的整数/字符串格式化，避免引入 libc
 * ============================================================ */
static char s_line[UTEST_LINE_MAX];
static int  s_len;

static void line_reset(void)
{
    s_len = 0;
}

static void put_c(char c)
{
    if(s_len < (UTEST_LINE_MAX - 1))
    {
        s_line[s_len++] = c;
    }
}

static void put_s(const char *s)
{
    if(s == 0)
    {
        put_s("(null)");
        return;
    }
    while(*s != '\0')
    {
        put_c(*s++);
    }
}

static void put_u(unsigned long v, unsigned base, int upper)
{
    char tmp[24];
    int  n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if(v == 0ul)
    {
        put_c('0');
        return;
    }
    while((v != 0ul) && (n < (int)sizeof(tmp)))
    {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    while(n > 0)
    {
        put_c(tmp[--n]);
    }
}

static void put_l(long v)
{
    if(v < 0)
    {
        put_c('-');
        put_u((unsigned long)(-(v + 1)) + 1ul, 10u, 0);
    }
    else
    {
        put_u((unsigned long)v, 10u, 0);
    }
}

static void put_hex(unsigned long v)
{
    put_s("0x");
    put_u(v, 16u, 1);
}

static void line_flush(void)
{
    s_line[s_len] = '\0';
    if(s_out != 0)
    {
        s_out(s_line);
    }
    s_len = 0;
}

/* 把 "/long/path/to/file.c" 缩成 "file.c"，让失败行读起来是给人看的 */
static const char *base_name(const char *path)
{
    const char *b = path;
    const char *p;

    for(p = path; (p != 0) && (*p != '\0'); p++)
    {
        if((*p == '/') || (*p == '\\'))
        {
            b = p + 1;
        }
    }
    return b;
}

/* ============================================================
 * 注册
 * ============================================================ */
void ark_utest_set_output(ark_utest_out_t out)
{
    s_out = out;
}

void ark_utest_register(const ark_utest_case_t *c)
{
    uint32_t i;

    if((c == 0) || (c->fn == 0))
    {
        return;
    }
    for(i = 0u; i < s_count; i++)
    {
        if(s_cases[i].fn == c->fn)
        {
            return;   /* 同一用例重复登记：忽略，不制造重复账 */
        }
    }
    if(s_count >= (uint32_t)ARKRTOS_UTEST_MAX_CASES)
    {
        s_overflow++;
        return;
    }
    s_cases[s_count].name = c->name;
    s_cases[s_count].fn = c->fn;
    s_count++;
}

/* ============================================================
 * 断言记账
 * ============================================================ */
void ark_utest_bump(void)
{
    if(s_cur != 0)
    {
        s_cur->checks++;
    }
}

void ark_utest_fail(const char *file, int line, const char *expr, const char *detail)
{
    if(s_cur == 0)
    {
        return;   /* 用例外调用：不计账，避免污染汇总 */
    }
    s_cur->failed++;
    line_reset();
    put_s("        ! ");
    put_s(base_name(file));
    put_c(':');
    put_l((long)line);
    put_s(": ");
    put_s(expr);
    if(detail != 0)
    {
        put_s("  -- ");
        put_s(detail);
    }
    line_flush();
}

void ark_utest_fail_num(const char *file, int line, const char *expr, long a, long b)
{
    if(s_cur == 0)
    {
        return;
    }
    s_cur->failed++;
    line_reset();
    put_s("        ! ");
    put_s(base_name(file));
    put_c(':');
    put_l((long)line);
    put_s(": ");
    put_s(expr);
    put_s("  (got ");
    put_l(a);
    put_s(" / ");
    put_hex((unsigned long)a);
    put_s(", want ");
    put_l(b);
    put_s(" / ");
    put_hex((unsigned long)b);
    put_s(")");
    line_flush();
}

int ark_utest_ctx_failed(void)
{
    return (s_cur != 0) ? (int)s_cur->failed : 0;
}

void ark_utest_skip(const char *reason)
{
    if(s_cur != 0)
    {
        s_cur->skipped = 1;
        line_reset();
        put_s("        - skipped: ");
        put_s(reason);
        line_flush();
    }
}

int ark_utest_ctx_skipped(void)
{
    return (s_cur != 0) ? s_cur->skipped : 0;
}

/* ============================================================
 * 运行
 * ============================================================ */
static int name_matches(const char *name, const char *pattern)
{
    const char *h;
    const char *n;

    if((pattern == 0) || (*pattern == '\0'))
    {
        return 1;
    }
    for(h = name; *h != '\0'; h++)
    {
        const char *a = h;
        const char *b = pattern;
        while((*a != '\0') && (*b != '\0') && (*a == *b))
        {
            a++;
            b++;
        }
        if(*b == '\0')
        {
            return 1;
        }
    }
    (void)n;
    return 0;
}

uint32_t ark_utest_list(void)
{
    uint32_t i;

    line_reset();
    put_s("utest: ");
    put_u((unsigned long)s_count, 10u, 0);
    put_s(" case(s) registered");
    if(s_overflow != 0u)
    {
        put_s(", ");
        put_u((unsigned long)s_overflow, 10u, 0);
        put_s(" DROPPED (table full)");
    }
    line_flush();

    for(i = 0u; i < s_count; i++)
    {
        line_reset();
        put_c(' ');
        put_u((unsigned long)(i + 1u), 10u, 0);
        put_s(". ");
        put_s(s_cases[i].name);
        line_flush();
    }
    return s_count;
}

uint32_t ark_utest_run(const char *pattern, ark_utest_summary_t *out)
{
    ark_utest_summary_t s = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    uint32_t i;

    s.overflow = s_overflow;

    for(i = 0u; i < s_count; i++)
    {
        utest_ctx_t c;

        if(!name_matches(s_cases[i].name, pattern))
        {
            continue;
        }
        s.matched++;

        c.name    = s_cases[i].name;
        c.checks  = 0u;
        c.failed  = 0u;
        c.skipped = 0;
        s_cur = &c;

        line_reset();
        put_s("RUN   ");
        put_s(c.name);
        line_flush();

        s_cases[i].fn();

        s.checks        += c.checks;
        s.failed_checks += c.failed;
        s.cases++;

        line_reset();
        if(c.skipped)
        {
            s.skipped++;
            put_s("SKIP  ");
            put_s(c.name);
        }
        else if(c.failed == 0u)
        {
            s.passed++;
            put_s("PASS  ");
            put_s(c.name);
            if(c.checks == 0u)
            {
                /* 一条断言都没跑却报 PASS，本身就该被看见 */
                put_s("   [no assertions!]");
            }
        }
        else
        {
            s.failed++;
            put_s("FAIL  ");
            put_s(c.name);
            put_s("   (");
            put_u((unsigned long)c.failed, 10u, 0);
            put_c('/');
            put_u((unsigned long)c.checks, 10u, 0);
            put_s(" assertions failed)");
        }
        line_flush();
    }

    s_cur = 0;

    line_reset();
    put_s("utest: ");
    put_u((unsigned long)s.matched, 10u, 0);
    put_s(" matched, ");
    put_u((unsigned long)s.passed, 10u, 0);
    put_s(" passed, ");
    put_u((unsigned long)s.failed, 10u, 0);
    put_s(" failed, ");
    put_u((unsigned long)s.skipped, 10u, 0);
    put_s(" skipped; assertions ");
    put_u((unsigned long)s.checks, 10u, 0);
    put_s(" run / ");
    put_u((unsigned long)s.failed_checks, 10u, 0);
    put_s(" failed");
    line_flush();

    if(s_overflow != 0u)
    {
        line_reset();
        put_s("utest: WARNING table full, ");
        put_u((unsigned long)s_overflow, 10u, 0);
        put_s(" case(s) never registered");
        line_flush();
    }

    if(out != 0)
    {
        *out = s;
    }
    return s.failed;
}

#endif /* ARKRTOS_USE_UTEST */
