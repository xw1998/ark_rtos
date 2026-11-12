/**
* @file test_stdio_host.c
* @brief App / 小程序 SDK 的 `printf` / `puts` / `putchar` 宿主门禁
* @details 编 `kernelsrc/sdk/app_sdk/arkrtos_stdio.c`，用一份桩替换
*          `arkrtos_shell_print()`（板上是 SVC 0x1A），把送出去的字节收进缓冲，
*          再逐字节与期望值比对。
*
*          为什么要有它：`printf` 的错误形态几乎都是「打出来是另一句话」——
*          宽度差一格、负数少个号、多打一个换行。这类错在板上只能靠眼睛看，
*          而眼睛看不出来；在宿主上可以逐字节比对。
*
*          【测什么 / 不测什么】
*          测：`printf` 的格式化与截断、`puts` 的补换行、`putchar` 的 EOF。
*          不测：SVC 那条通道本身（宿主上没有 SVC），它由板上的例程覆盖。
*
*          跑法：`python tools/sdk_stdio_host_gate.py`（退出码 0 = 全过）。
*/

#include <stdio.h>
#include <string.h>

/* ---------------- 控制台捕获：替掉 SVC ---------------- */

static char g_con[4096];
static int  g_con_n;

int arkrtos_shell_print(const char *msg)
{
    if (msg == 0)
    {
        return 0;
    }
    while (*msg != '\0')
    {
        if (g_con_n < (int)sizeof(g_con) - 1)
        {
            g_con[g_con_n++] = *msg;
            g_con[g_con_n] = '\0';
        }
        msg++;
    }
    return g_con_n;
}

/** @brief 清空捕获缓冲，并把它当作"一个屏幕"的开始 */
static void cap_reset(void)
{
    g_con_n = 0;
    g_con[0] = '\0';
}

/** @brief 捕获内容里有多少个字节 */
static int cap_len(void)
{
    return g_con_n;
}

/* ---------------- 断言记账 ---------------- */

static int g_fail;
static int g_pass;

static void expect_bytes(const char *what, const char *want, int want_len, int got_len)
{
    if ((want_len == got_len) && (memcmp(want, g_con, (size_t)want_len) == 0))
    {
        g_pass++;
        return;
    }

    g_fail++;
    fputs("FAIL: ", stdout);
    fputs(what, stdout);
    fputs("\n  want: ", stdout);
    fwrite(want, 1, (size_t)want_len, stdout);
    fputs("\n  got : ", stdout);
    fwrite(g_con, 1, (size_t)got_len, stdout);
    fputs("\n", stdout);
}

/* ---------------- 用例 ---------------- */

int main(void)
{
    /* 1) 用户原文那一条 */
    cap_reset();
    printf("Hello, World!\n");
    {
        /* 串口终端按 ONLCR 处理：\n 补成 \r\n */
        static const char want[] = "Hello, World!\r\n";
        expect_bytes("printf hello", want, (int)sizeof(want) - 1, cap_len());
    }

    /* 2) 常用转换 */
    cap_reset();
    printf("n=%d neg=%d hex=%08X str=%s ch=%c pct=%%\n",
           42, -7, (unsigned)0xBEEFu, "abc", 'Z');
    {
        static const char want[] = "n=42 neg=-7 hex=0000BEEF str=abc ch=Z pct=%\r\n";
        expect_bytes("printf conversions", want, (int)sizeof(want) - 1, cap_len());
    }

    /* 3) 返回值 = 写入字符数 */
    cap_reset();
    {
        int r = printf("abc");
        expect_bytes("printf return", "abc", 3, cap_len());
        if (r != 3)
        {
            g_fail++;
            fputs("FAIL: printf return value != 3\n", stdout);
        }
        else
        {
            g_pass++;
        }
    }

    /* 4) puts 自动补一个换行 */
    cap_reset();
    puts("line");
    {
        static const char want[] = "line\r\n";
        expect_bytes("puts", want, (int)sizeof(want) - 1, cap_len());
    }

    /* 5) putchar */
    cap_reset();
    putchar('Q');
    putchar('\n');
    {
        static const char want[] = "Q\r\n";
        expect_bytes("putchar", want, (int)sizeof(want) - 1, cap_len());
    }

    /* 6) putchar(EOF) 不产生输出 */
    cap_reset();
    {
        int r = putchar(EOF);
        expect_bytes("putchar EOF", "", 0, cap_len());
        if (r != EOF)
        {
            g_fail++;
            fputs("FAIL: putchar(EOF) did not return EOF\n", stdout);
        }
        else
        {
            g_pass++;
        }
    }

    /* 7) 长行截断：单条上限 128 字节（含结尾 NUL），所以最多 127 个字符 */
    cap_reset();
    {
        char big[200];
        int  i;
        for (i = 0; i < 199; i++)
        {
            big[i] = 'x';
        }
        big[199] = '\0';
        (void)printf("%s", big);
        if (cap_len() != 127)
        {
            g_fail++;
            fputs("FAIL: truncation is not 127 bytes\n", stdout);
        }
        else
        {
            g_pass++;
        }
    }

    /* 8) 用户自己写 "\r\n" 时不能补成 "\r\r\n" */
    cap_reset();
    printf("a\r\nb\n");
    {
        static const char want[] = "a\r\nb\r\n";
        expect_bytes("explicit CRLF", want, (int)sizeof(want) - 1, cap_len());
    }

    /* 9) 分段：超过一次 SVC 上限的长串要被切成多段送出去，内容不许丢 */
    cap_reset();
    {
        char big[200];
        int  i;
        for (i = 0; i < 120; i++)
        {
            big[i] = 'y';
        }
        big[120] = '\0';
        (void)printf("%s", big);
        if (cap_len() != 120)
        {
            g_fail++;
            fputs("FAIL: long line was not chunked intact\n", stdout);
        }
        else
        {
            g_pass++;
        }
    }

    fputs(g_fail == 0 ? "STDIO-HOST OK\n" : "STDIO-HOST FAILED\n", stdout);

    return (g_fail == 0) ? 0 : 1;
}
