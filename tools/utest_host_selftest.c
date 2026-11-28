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

/* 失败记账的三种形态：负值（PUT_L 的负号分支）、无 detail、无条件失败 */
static void case_report_paths(void)
{
    UTEST_CHECK(ark_utest_ctx_failed() == 0);
    UTEST_CHECK(ark_utest_ctx_skipped() == 0);
    UTEST_CHECK_EQ(-5, 3);                /* 负值走 put_l 的负号分支 */
    UTEST_FAIL("unconditional failure");  /* detail 非 NULL */
    ark_utest_fail("x.c", 7, "detail.is.null", 0);  /* detail == NULL */
    ark_utest_fail(0, 8, "file.is.null", 0);        /* file == NULL：base_name + put_s 兜底 */
    ark_utest_fail("dir\\sub\\x.c", 9, "backslash", "d"); /* '\\' 分隔符分支 */
    ark_utest_fail("/unix/style/y.c", 10, "forwardslash", 0); /* '/' 分隔符分支 */
}

/* 一条断言都不跑却报 PASS 的用例：框架必须打出 [no assertions!] */
static void case_no_assertions(void)
{
}

/* 超长名字用例的实体：必须与其它用例函数地址不同，否则会被去重挡掉 */
static void case_longname_body(void)
{
    UTEST_CHECK(1 == 1);
}

/* ---- 注册表耗尽用：64 个互不相同的空函数 ----------------------------
 * 注册表按函数指针去重，所以要造溢出只能拿 64 个不同的地址。
 * 数量取 ARKRTOS_UTEST_MAX_CASES 的默认值；下面的 EXPECT 会钉住这个前提。 */
#define OV_ID_LIST(X) \
    X(00) X(01) X(02) X(03) X(04) X(05) X(06) X(07) \
    X(08) X(09) X(10) X(11) X(12) X(13) X(14) X(15) \
    X(16) X(17) X(18) X(19) X(20) X(21) X(22) X(23) \
    X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) \
    X(32) X(33) X(34) X(35) X(36) X(37) X(38) X(39) \
    X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) \
    X(48) X(49) X(50) X(51) X(52) X(53) X(54) X(55) \
    X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63)

#define OV_DEFINE(id) static void ov_fn_##id(void) { }
OV_ID_LIST(OV_DEFINE)
#undef OV_DEFINE

#define OV_ADDR(id) ov_fn_##id,
static const ark_utest_fn_t g_ov_fns[64] = { OV_ID_LIST(OV_ADDR) };
#undef OV_ADDR

#define OV_COUNT 64u

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
    uint32_t            registered;

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
    registered = ark_utest_list();
    EXPECT(registered == 5u);
    EXPECT(strstr(g_out, "manual.pass") != NULL);

    /* 空模式等价于"全部"（pattern == "" 分支）；out == NULL 时不回填汇总 */
    (void)ark_utest_run("", &s);
    EXPECT(s.matched == registered);
    (void)ark_utest_run("no_such_case_name", 0);

    /* 用例之外 skip：不计账、不崩（s_cur == NULL 的早退） */
    ark_utest_skip("outside.case");

    /* 超长用例名：行缓冲装满后丢弃后面的字符，不许越界 */
    {
        static ark_utest_case_t longname;
        static char             longbuf[300];
        uint32_t                i;
        const char             *p;
        size_t                  seg;

        for(i = 0u; i < (uint32_t)(sizeof(longbuf) - 1u); i++)
        {
            longbuf[i] = 'L';
        }
        longbuf[sizeof(longbuf) - 1u] = '\0';
        longname.name = longbuf;
        longname.fn   = case_longname_body;
        ark_utest_register(&longname);

        g_out_len = 0; g_out[0] = '\0';
        (void)ark_utest_run("LLLL", &s);
        EXPECT(s.passed == 1u);
        p = strchr(g_out, '\n');
        seg = (p != 0) ? (size_t)(p - g_out) : g_out_len;
        EXPECT(seg <= 159u);              /* UTEST_LINE_MAX - 1 */
    }

    /* ---- 失败与边角路径（第 2 段）---------------------------------
     * 这一段会往注册表里加用例、最后再把表塞满，所以必须放在上面那些
     * "5 条用例"的断言之后。 */

    /* 用例之外调用断言 API：一律不计账、不崩（s_cur == NULL 的早退） */
    ark_utest_bump();
    ark_utest_fail("x.c", 1, "outside.case", 0);
    ark_utest_fail_num("x.c", 1, "outside.case", 1, 2);
    EXPECT(ark_utest_ctx_failed() == 0);
    EXPECT(ark_utest_ctx_skipped() == 0);

    /* 非法登记：NULL 结构 / 缺名 / 缺函数，都不该进表 */
    {
        static const ark_utest_case_t no_name = { 0, case_all_pass };
        static const ark_utest_case_t no_fn   = { "manual.no_fn", 0 };
        ark_utest_register(0);
        ark_utest_register(&no_name);
        ark_utest_register(&no_fn);
    }
    g_out_len = 0; g_out[0] = '\0';
    EXPECT(ark_utest_list() == (registered + 1u));   /* 只多了超长名字那一条 */

    {
        static const ark_utest_case_t report = { "manual.report", case_report_paths };
        static const ark_utest_case_t empty  = { "manual.empty",  case_no_assertions };
        ark_utest_register(&report);
        ark_utest_register(&empty);
    }
    g_out_len = 0; g_out[0] = '\0';
    (void)ark_utest_run("manual.report", &s);
    EXPECT(s.matched == 1u);
    EXPECT(s.failed == 1u);
    EXPECT(s.failed_checks == 6u);
    EXPECT(strstr(g_out, "(got -5") != NULL);
    EXPECT(strstr(g_out, "unconditional failure") != NULL);
    EXPECT(strstr(g_out, "FAIL  manual.report   (6/4 assertions failed)") != NULL);
    EXPECT(strstr(g_out, "(null):8: file.is.null") != NULL);
    EXPECT(strstr(g_out, "x.c:9: backslash") != NULL);
    EXPECT(strstr(g_out, "y.c:10: forwardslash") != NULL);

    /* 零断言却 PASS：必须显式提示 */
    g_out_len = 0; g_out[0] = '\0';
    (void)ark_utest_run("manual.empty", &s);
    EXPECT(s.passed == 1u);
    EXPECT(s.checks == 0u);
    EXPECT(strstr(g_out, "[no assertions!]") != NULL);

    /* 注册表耗尽：装不下的用例计入 overflow，并在列表与汇总里报出来 */
    {
        static ark_utest_case_t fill[OV_COUNT];
        uint32_t i;

        for(i = 0u; i < OV_COUNT; i++)
        {
            fill[i].name = "overflow.filler";
            fill[i].fn   = g_ov_fns[i];
        }
        for(i = 0u; i < OV_COUNT; i++)
        {
            ark_utest_register(&fill[i]);
        }
    }
    g_out_len = 0; g_out[0] = '\0';
    EXPECT(ark_utest_list() == (uint32_t)ARKRTOS_UTEST_MAX_CASES);
    EXPECT(strstr(g_out, "DROPPED (table full)") != NULL);

    g_out_len = 0; g_out[0] = '\0';
    (void)ark_utest_run("no_such_case_name", &s);
    EXPECT(s.overflow > 0u);
    EXPECT(strstr(g_out, "utest: WARNING table full") != NULL);

    /* 没有输出汇（默认状态）：行照样排好、只是被丢弃，不许崩 */
    ark_utest_set_output(0);
    (void)ark_utest_list();
    (void)ark_utest_run("no_such_case_name", &s);
    ark_utest_set_output(sink);

    printf("HOST-SELFTEST OK: registry=%u, filter/accounting/skip/autoreg all consistent\n",
           (unsigned)registered);
    return 0;
}
