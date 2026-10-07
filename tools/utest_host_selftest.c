/**
 * @file utest_host_selftest.c
 * @brief arkrtos_utest 框架的宿主自测（不依赖任何板子 / HAL）
 * @details 目的只有一个：证明框架自身的记账是可信的——
 *          「通过 / 失败 / 跳过 / 断言条数 / 注册表溢出 / 子串过滤 / 自动注册」
 *          七件事都按预期计数。用宿主 gcc 编译运行，退出码 0 表示框架正确。
 *
 *          特意**不用** UTEST_CASE 宏来构造失败用例，而是直接调
 *          ark_utest_register()，这样"框架该报几条失败"是程序自己掌控的常量，
 *          不会因为将来往仓库里加用例而把这份自测染红。
 *
 * 构建 & 运行（见 tools/ci_gate.py 的 utest-host 检查）：
 *   gcc -std=gnu11 -I kernelsrc/include -o utest_host_selftest \
 *       tools/utest_host_selftest.c kernelsrc/src/arkrtos_utest.c
 *   ./utest_host_selftest
 */
#include "arkrtos_utest.h"

#include <stdio.h>
#include <string.h>

/* 捕获框架输出：只用于在自测里逐行检查文本，不打印到 stdout 以免噪声 */
static char  g_out[8192];
static size_t g_out_len;

static void sink(const char *s)
{
    size_t n = strlen(s);
    if((g_out_len + n + 2) < sizeof(g_out))
    {
        memcpy(g_out + g_out_len, s, n);
        g_out_len += n;
        g_out[g_out_len++] = '\n';
        g_out[g_out_len] = '\0';
    }
}

static void case_all_pass(void)
{
    UTEST_CHECK(1 == 1);
    UTEST_CHECK_EQ(2 + 2, 4);
    UTEST_CHECK_RANGE(1, 5, 9);
}

static void case_one_fail(void)
{
    UTEST_CHECK(1 == 1);
    UTEST_CHECK_EQ(3, 4);            /* 唯一失败点 */
}

static void case_skip(void)
{
    UTEST_SKIP("deliberate skip");
    UTEST_CHECK(0);                  /* 不该被计到 */
}

/* 同时验证 UTEST_CASE 宏的自动注册确实生效 */
UTEST_CASE(auto_registered_case)
{
    UTEST_CHECK_EQ(1 + 1, 2);
}

/* 第二个自动注册用例：用子串过滤时应当被 pattern "auto_" 同时选中 */
UTEST_CASE(auto_second_case)
{
    UTEST_CHECK_NE("x", "y");
}

#define EXPECT(cond) do {                                                  \
        if(!(cond)) {                                                      \
            printf("HOST-SELFTEST FAILED at line %d: %s\n", __LINE__, #cond); \
            return 1;                                                      \
        }                                                                  \
    } while(0)

int main(void)
{
    ark_utest_summary_t s;

    ark_utest_set_output(sink);

    /* 手动注册三条"可控"用例；自动注册的两条由构造函数登记 */
    {
        static const ark_utest_case_t manual1 = { "manual.pass", case_all_pass };
        static const ark_utest_case_t manual2 = { "manual.fail", case_one_fail };
        static const ark_utest_case_t manual3 = { "manual.skip", case_skip };
        ark_utest_register(&manual1);
        ark_utest_register(&manual2);
        ark_utest_register(&manual3);
        ark_utest_register(&manual1);   /* 重复登记：应被忽略 */
    }

    /* 全部用例：手动 3 + 自动 2 = 5；失败 1、跳过 1、通过 3 */
    g_out_len = 0; g_out[0] = '\0';
    (void)ark_utest_run(NULL, &s);
    EXPECT(s.matched == 5u);
    EXPECT(s.passed == 3u);
    EXPECT(s.failed == 1u);
    EXPECT(s.skipped == 1u);
    EXPECT(s.checks == 7u);         /* 3 + 2 + (skip 里 0) + 1 + 1 */
    EXPECT(s.failed_checks == 1u);
    EXPECT(s.overflow == 0u);
    EXPECT(strstr(g_out, "FAIL  manual.fail") != NULL);
    EXPECT(strstr(g_out, "SKIP  manual.skip") != NULL);
    EXPECT(strstr(g_out, "PASS  auto_registered_case") != NULL);

    /* 子串过滤：只跑 auto_ 前缀的两条，且都通过 */
    g_out_len = 0; g_out[0] = '\0';
    (void)ark_utest_run("auto_", &s);
    EXPECT(s.matched == 2u);
    EXPECT(s.passed == 2u);
    EXPECT(s.failed == 0u);
    EXPECT(strstr(g_out, "manual.") == NULL);

    /* 过滤到一个都不匹配：不该报成功，也不该报失败 */
    (void)ark_utest_run("no_such_case_name", &s);
    EXPECT(s.matched == 0u);
    EXPECT(s.failed == 0u);

    /* 列表 */
    g_out_len = 0; g_out[0] = '\0';
    EXPECT(ark_utest_list() == 5u);
    EXPECT(strstr(g_out, "manual.pass") != NULL);

    printf("HOST-SELFTEST OK: registry=%u, filter/accounting/skip/autoreg all consistent\n",
           (unsigned)ark_utest_list());
    return 0;
}
