/**
* @file test_printf_host.c
* @brief 小程序 SDK 的格式化器宿主门禁：用 gcc 直接跑 arkrtos_mini_printf_words()
* @details 为什么要有它：printf 的错法基本都是「打出来是另一句话」——宽度差一格、
*          负数少个号、宽度算错把数字吃掉。这类错在板子上只能靠眼睛看，而眼睛
*          看不出来。所以把格式化主体拿到宿主上，用一批固定用例逐字比对。
*
*          【这里测什么、不测什么】
*          测：格式化主体（转换、宽度、补零、左对齐、负号、越界、未知转换、
*              超长行分段 flush）。
*          不测：那条 __asm 取参桩——它是 ARM 汇编，宿主上跑不了，只能上板。
*          所以「参数真的进到槽里」这件事由板上的演示程序覆盖，两者合起来才算
*          这条链路完整。
*
*          必须用 -m32 编：槽位是 32 位的，64 位宿主上指针会被截断。
*          跑法见 tools/mini_sdk_printf_gate.py。
*/

#include <stdio.h>
#include <string.h>

/* ---- 控制台捕获：替掉 SVC，把 write() 送出来的字节收进缓冲 ---- */
static char g_con[8192];
static int  g_con_n;

int arkrtos_call_shell_svc(unsigned int *p)
{
    const char *s = (const char *)p[1];
    while ((s != 0) && (*s != 0))
    {
        if (g_con_n < (int)sizeof(g_con) - 1)
        {
            g_con[g_con_n] = *s;
            g_con_n++;
            g_con[g_con_n] = 0;
        }
        s++;
    }
    return 0;
}

int arkrtos_call_thread_ctrl(unsigned int *p)
{
    (void)p;
    return 0;
}

/* 被测文件：这里就是「SDK 的 rt.c 原样」，不是副本。 */
#include "../arkc/arkrtos_mini_rt_arkc.c"

/* ---- 断言 ---- */
static int g_fail;
static int g_case;

static void expect_n(const char *label, const char *fmt,
                     const unsigned int *args, int nargs,
                     const char *want)
{
    unsigned int w[MINI_PF_SLOTS];
    int i;
    int got;
    int want_len = (int)strlen(want);

    for (i = 0; i < MINI_PF_SLOTS; i++)
    {
        w[i] = 0u;
    }
    w[0] = (unsigned int)fmt;
    for (i = 0; i < nargs && (i + 1) < MINI_PF_SLOTS; i++)
    {
        w[i + 1] = args[i];
    }

    g_con_n = 0;
    g_con[0] = 0;
    got = arkrtos_mini_printf_words(fmt, w);
    g_case++;

    if ((got != want_len) || (strcmp(g_con, want) != 0))
    {
        g_fail++;
        printf("FAIL %-22s fmt=%-28s got=%d want=%d\n  got  = \"%s\"\n  want = \"%s\"\n",
               label, fmt, got, want_len, g_con, want);
    }
    else
    {
        printf("PASS %-22s \"%s\"\n", label, g_con);
    }
}

#define EXPECT(label, fmt, want, ...)                                        \
    do {                                                                     \
        static const unsigned int a_[] = { 0, ##__VA_ARGS__ };               \
        expect_n(label, fmt, a_ + 1, (int)(sizeof(a_) / sizeof(a_[0])) - 1,  \
                 want);                                                      \
    } while (0)

int main(void)
{
    static char big[201];

    EXPECT("plain", "Hello, World!\n", "Hello, World!\n");
    EXPECT("empty", "", "");
    EXPECT("percent", "100%%", "100%");
    EXPECT("dec", "%d", "-1", (unsigned int)-1);
    EXPECT("dec-min", "%d", "-2147483648", 0x80000000u);
    EXPECT("dec-zero", "%d", "0", 0);
    EXPECT("undec", "%u", "4294967295", 0xFFFFFFFFu);
    EXPECT("hex", "%x", "deadbeef", 0xDEADBEEFu);
    EXPECT("hex-upper", "%X", "DEADBEEF", 0xDEADBEEFu);
    EXPECT("hex-pad0", "%08x", "00000123", 0x123u);
    EXPECT("dec-width", "%5d|", "   42|", 42);
    EXPECT("dec-left", "%-5d|", "42   |", 42);
    EXPECT("str", "%s", "ab", (unsigned int)"ab");
    EXPECT("str-width", "%5s|", "   ab|", (unsigned int)"ab");
    EXPECT("str-left", "%-5s|", "ab   |", (unsigned int)"ab");
    EXPECT("str-null", "%s", "(null)", 0);
    EXPECT("chr", "%c%c", "ab", 'a', 'b');
    EXPECT("ptr", "%p", "0x2000c000", 0x2000C000u);
    EXPECT("mixed", "[%d|%u|%x|%s|%c]", "[-7|7|7|s|z]", -7, 7u, 0x7u, (unsigned int)"s", 'z');
    EXPECT("no-arg", "%d", "0");
    EXPECT("unknown", "%q", "%q");
    EXPECT("trailing-pct", "abc%", "abc");

    /* 12 个槽：格式串 + 11 个参数，全部要取到 */
    EXPECT("11-args", "%d %d %d %d %d %d %d %d %d %d %d",
           "1 2 3 4 5 6 7 8 9 10 11",
           1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11);

    /* 超过缓冲（96 字节）必须分段 flush，不能丢字 */
    memset(big, 'x', 200);
    big[200] = 0;
    EXPECT("long-200", "%s", big, (unsigned int)big);

    printf("---- %d case(s), %d failure(s) ----\n", g_case, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
