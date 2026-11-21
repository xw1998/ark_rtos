/**
* @file test_export_host.c
* @brief 内核符号导出表（arkrtos_exports.c）的宿主单元测试
* @details 这个文件回答的是「可下载模块能按名字调用内核的哪些函数」。它只有
*          两件事会错，而两件都不会报错：

*            - **查表匹配写成前缀匹配或忽略大小写**。模块里的外部引用由装载器
*              按名字解析，多认一个名字等于多给一个入口：写 "arkrtos_mod_print"
*              就能拿到 arkrtos_mod_printf 的地址，而这种偏差在正常路径上
*              完全看不出来（模块拼对名字时照样跑）；
*            - **模块打印的格式化与容量**。它是模块唯一的输出通道：%s 传空指针、
*              末尾一个孤立的 %、超长输出都必须有确定的答案，不能越界也不能
*              把缓冲区里的旧字节当成输出。

*          被测的是真实的 arkrtos_exports.c。控制台、当前任务、以及表里登记的
*          那几个内核函数入口全部由本文件桩住；表项地址因此是桩的地址，这正是
*          用例要断言的东西（名字 → 哪一个函数的地址）。

*          覆盖范围（如实说明）：
*            已验证：fmt 为空返回 -1 且不写控制台、纯字面量、%s/%c/%d/%i/%u/%x/%p/%%
*                    八种占位符、未知占位符原样输出、末尾孤立 % 的终止、%s 传空指针
*                    输出 (null)、超长输出按缓冲上限截断且返回值与写入一致、按当前
*                    任务下标选缓冲（四个槽取模）、查表的三道参数闸门、六个导出名字
*                    逐条命中且地址就是对应函数、前缀/后缀/大小写不同的名字被拒、
*                    未命中时不写 p_addr、以及整张表的名字非空、互不相同、都能被
*                    lookup 命中、p_count 为空指针时不崩。
*            未验证：装载器真的拿这些地址去重定位 ELF（由 loader 与 modld 用例负责）、
*                    以及关档（ARKRTOS_USE_MODLOAD=0）时「零体积零符号」——宿主只能
*                    证明这个 TU 编成空目标、链接不引入任何未定义引用。
* @note 用法：由 tools/coverage_gate.py 的两条 case 编译并运行，输出
*       EXPORT-HOST OK（MODLOAD=1 档）或 EXPORT-HOST OFF OK（MODLOAD=0 档）。
*/

#include "arkrtos_config.h"
#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_log.h"
#include "arkrtos_export.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

#define CK_EQ(a, b, msg)                                                     \
    do {                                                                     \
        long va = (long)(a);                                                 \
        long vb = (long)(b);                                                 \
        g_checks++;                                                          \
        if(va != vb)                                                         \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s (got %ld, want %ld)\n",                    \
                   __FILE__, __LINE__, msg, va, vb);                         \
        }                                                                    \
    } while(0)

#if (ARKRTOS_USE_MODLOAD == 1)

/* exports.c 在定义点之前自带原型，头文件里没有；这里照抄同签名。 */
int32 arkrtos_mod_printf(const char *fmt, ...);

/* 模块打印缓冲上限。exports.c 里是私有宏（带 #ifndef 允许板级覆盖），头文件
 * 没有导出它；这里钉住的是**默认值**。改动那边必须让这条断言先红一次，
 * 这正是它的用途。 */
#define EXPORT_TEST_BUF      (192u)

/* ------------------------------------------------------------------
 * 内核桩
 * ------------------------------------------------------------------ */
static uint8 g_cap[512];
static uint32 g_cap_len   = 0u;
static int    g_cap_calls = 0;

int32 arkrtos_console_write(const uint8 *p_data, uint32 len)
{
    uint32 i;

    g_cap_calls++;
    g_cap_len = 0u;
    for(i = 0u; (i < len) && (i < (uint32)sizeof(g_cap)); i++)
    {
        g_cap[i] = p_data[i];
    }
    g_cap_len = (len < (uint32)sizeof(g_cap)) ? len : (uint32)sizeof(g_cap);
    return (int32)len;
}

/* 任务表：exports.c 用它算「当前 TCB 的下标」。这里给的是真数组（不是本地
 * 小数组），因为指针相减只有落在同一数组里才是有定义的行为。 */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_task_count = 0;

/* 当前任务：可编程，用来验证「按 TCB 下标取模选缓冲」。 */
static arkrtos_task_t *g_cur = 0;

arkrtos_task_t *arkrtos_task_get_current(void) { return g_cur; }

/* 表里登记的其余四个入口：只关心地址，不关心实现。 */
uint32 arkrtos_kernel_get_tick(void) { return 0u; }
void   arkrtos_task_delay_internal(uint32 us) { (void)us; }
void   arkrtos_thread_exit_internal(void) { }

static void reset_all(void)
{
    memset(g_cap, 0, sizeof(g_cap));
    g_cap_len   = 0u;
    g_cap_calls = 0;
    g_cur       = 0;
}

/* 把控制台截获的内容按字符串比较。缓冲区一定被本文件清零过，且每次写入
 * 都从下标 0 开始重新拷，所以不会读到上一轮的残留。 */
static int cap_is(const char *want)
{
    uint32 n = (uint32)strlen(want);

    if(g_cap_len != n)
    {
        return 0;
    }
    return (memcmp(g_cap, want, n) == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * 用例：模块打印
 * ------------------------------------------------------------------ */
static void test_printf_null(void)
{
    reset_all();
    CK_EQ(arkrtos_mod_printf(0), -1, "a null format is refused");
    CK_EQ(g_cap_calls, 0, "and nothing is written to the console");
}

static void test_printf_literals(void)
{
    reset_all();
    CK_EQ(arkrtos_mod_printf("abc"), 3, "plain text returns its length");
    CK_EQ(g_cap_calls, 1, "exactly one console write");
    ck(cap_is("abc"), "and the bytes are the format itself");

    reset_all();
    CK_EQ(arkrtos_mod_printf(""), 0, "an empty format writes nothing but succeeds");
    CK_EQ(g_cap_calls, 1, "still exactly one (possibly empty) write");
}

static void test_printf_conversions(void)
{
    static const char want[] = "S|Z|42|-7|300|beef|0x2a|%";

    reset_all();
    CK_EQ(arkrtos_mod_printf("%s|%c|%d|%i|%u|%x|%p|%%",
                             "S", 'Z', (long)42, (long)-7,
                             300ul, 0xbeeful, 0x2aul),
          (long)strlen(want), "every supported conversion contributes its own bytes");
    ck(cap_is(want),
       "conversions render in decimal / hex with a 0x prefix for %p");

    /* %s 传空指针：不能解引用，也不该打印空。 */
    reset_all();
    CK_EQ(arkrtos_mod_printf("[%s]", (const char *)0), (long)strlen("[(null)]"),
          "a null %s still prints");
    ck(cap_is("[(null)]"), "a null string renders as (null)");
}

static void test_printf_edge_formats(void)
{
    /* 未知占位符原样输出：%q 不认识，就把 %q 打出来。 */
    reset_all();
    CK_EQ(arkrtos_mod_printf("%q"), 2, "an unknown conversion is echoed");
    ck(cap_is("%q"), "unknown conversions come out verbatim");

    /* 末尾一个孤立的 %：不能吃掉后面的内存，也不能死循环。 */
    reset_all();
    CK_EQ(arkrtos_mod_printf("abc%"), 3, "a trailing percent ends the scan");
    ck(cap_is("abc"), "and only the literal part is written");

    /* 数字 0：mod_putu 的零分支。 */
    reset_all();
    CK_EQ(arkrtos_mod_printf("%u,%x", 0ul, 0ul), 3, "zero is not empty");
    ck(cap_is("0,0"), "zero renders as a single 0 in both bases");
}

static void test_printf_truncation(void)
{
    char big[301];
    uint32 i;

    for(i = 0u; i < 300u; i++) { big[i] = 'a'; }
    big[300] = '\0';

    reset_all();
    CK_EQ(arkrtos_mod_printf("%s", big), (long)(EXPORT_TEST_BUF - 1u),
          "oversized output stops one byte short of the buffer");
    CK_EQ(g_cap_len, EXPORT_TEST_BUF - 1u,
          "and the reported length matches what was written");
    ck(g_cap[0] == 'a', "the head of the oversized string survives");
    ck(g_cap[EXPORT_TEST_BUF - 1u] == 0u,
       "the byte just past the written region is untouched");
}

static void test_printf_slot_per_task(void)
{
    /* 没有当前任务：槽号保持 0，照样能打。 */
    reset_all();
    CK_EQ(arkrtos_mod_printf("%d", (long)1), 1, "no current task still prints");
    ck(cap_is("1"), "and the output is right");

    /* 当前任务是 2 号下标：取模后仍是合法槽，输出必须完全正确。 */
    reset_all();
    g_cur = &arkrtos_task_table[2];
    CK_EQ(arkrtos_mod_printf("t%d", (long)2), 2, "index 2 prints");
    ck(cap_is("t2"), "and does not interleave with another slot");

    /* 下标 5 落在槽 1（5 & 3）。 */
    reset_all();
    g_cur = &arkrtos_task_table[5];
    CK_EQ(arkrtos_mod_printf("x%s", "y"), 2, "index 5 prints");
    ck(cap_is("xy"), "slot is the TCB index modulo four");
}

/* ------------------------------------------------------------------
 * 用例：导出表
 * ------------------------------------------------------------------ */
static void test_lookup_guards(void)
{
    unsigned long addr = 0xDEADul;

    CK_EQ(arkrtos_export_lookup(0, &addr), -1, "a null name is refused");
    CK_EQ(arkrtos_export_lookup("arkrtos_mod_printf", 0), -1,
          "a null out pointer is refused");
    CK_EQ(arkrtos_export_lookup("", &addr), -1, "an empty name is refused");
    CK_EQ(addr, 0xDEADul, "a refused lookup never writes the out pointer");
}

static void test_lookup_hits(void)
{
    unsigned long addr = 0ul;

    CK_EQ(arkrtos_export_lookup("arkrtos_mod_printf", &addr), 0,
          "arkrtos_mod_printf is exported");
    CK_EQ(addr, (unsigned long)(void *)arkrtos_mod_printf,
          "and resolves to this very function");
    CK_EQ(arkrtos_export_lookup("arkrtos_console_write", &addr), 0,
          "arkrtos_console_write is exported");
    CK_EQ(addr, (unsigned long)(void *)arkrtos_console_write,
          "and resolves to the console stub");
    CK_EQ(arkrtos_export_lookup("arkrtos_kernel_get_tick", &addr), 0,
          "arkrtos_kernel_get_tick is exported");
    CK_EQ(arkrtos_export_lookup("arkrtos_task_delay_internal", &addr), 0,
          "arkrtos_task_delay_internal is exported");
    CK_EQ(arkrtos_export_lookup("arkrtos_task_get_current", &addr), 0,
          "arkrtos_task_get_current is exported");
    CK_EQ(arkrtos_export_lookup("arkrtos_thread_exit_internal", &addr), 0,
          "arkrtos_thread_exit_internal is exported");
}

static void test_lookup_misses(void)
{
    unsigned long addr = 0ul;

    /* 前缀必须不被接受：装载器按名字精确匹配，模糊匹配等于多开一个入口。 */
    CK_EQ(arkrtos_export_lookup("arkrtos_mod_print", &addr), -1,
          "a prefix is not a match");
    CK_EQ(arkrtos_export_lookup("arkrtos_mod_printfX", &addr), -1,
          "a longer name is not a match");
    CK_EQ(arkrtos_export_lookup("ARKRTOS_MOD_PRINTF", &addr), -1,
          "lookup is case sensitive");
    CK_EQ(arkrtos_export_lookup("snprintf", &addr), -1,
          "platform symbols are not exported");
    CK_EQ(addr, 0ul, "a miss leaves the out pointer alone");
}

static void test_export_table(void)
{
    const arkrtos_export_t *t;
    uint32 count = 0u;
    uint32 i;
    uint32 j;

    t = arkrtos_export_table(&count);
    ck(t != 0, "the table is not null when the feature is on");
    CK_EQ(count, 6u, "six symbols are exported");
    /* 这里不能写成 CK_EQ(arkrtos_export_table(0), ...)：把函数调用直接
     * 转成整数会触发 -Wbad-function-cast（指针返回不是那种整数），门禁会红。
     * 指针与指针比，本来就是更准确的写法。 */
    ck(arkrtos_export_table(0) == t,
       "asking without a count returns the same table");

    for(i = 0u; i < count; i++)
    {
        unsigned long addr = 0ul;

        ck(t[i].name != 0, "every entry has a name");
        ck(strncmp(t[i].name, "arkrtos_", 8) == 0,
           "every exported name is a kernel symbol");
        ck(t[i].addr != 0ul, "every entry has a non-zero address");
        CK_EQ(arkrtos_export_lookup(t[i].name, &addr), 0,
              "every table name is resolvable through lookup");
        CK_EQ(addr, t[i].addr, "and resolves to the same address");
        for(j = i + 1u; j < count; j++)
        {
            ck(strcmp(t[i].name, t[j].name) != 0, "names are unique");
        }
    }
}

#endif /* ARKRTOS_USE_MODLOAD == 1 */

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void)
{
#if (ARKRTOS_USE_MODLOAD == 1)
    test_printf_null();
    test_printf_literals();
    test_printf_conversions();
    test_printf_edge_formats();
    test_printf_truncation();
    test_printf_slot_per_task();
    test_lookup_guards();
    test_lookup_hits();
    test_lookup_misses();
    test_export_table();

    if(g_fails != 0)
    {
        printf("EXPORT-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("EXPORT-HOST OK %d checks\n", g_checks);
    return 0;
#else
    /* 关档：exports.c 是空目标（零体积零符号），调用点由头文件里的宏回答
     * 「没有这件事」。这里证明两件事：开关真是 0，以及头文件的宏确实拒绝。 */
    unsigned long addr = 0xDEADul;

    CK_EQ(ARKRTOS_USE_MODLOAD, 0, "the switch really is off");
    ck(arkrtos_export_lookup("arkrtos_mod_printf", &addr) == -1,
       "the off-build refuses every lookup");
    CK_EQ(addr, 0xDEADul, "and writes nothing");
    /* arkrtos_export_table 在关档展开成 ((const arkrtos_export_t *)0)，而这个
     * typedef 只在开档存在——所以这里连「表是空的」都无法写成表达式，这本身
     * 就是关档零暴露的证据（头文件里没有可用的类型与函数）。 */

    if(g_fails != 0)
    {
        printf("EXPORT-HOST OFF FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("EXPORT-HOST OFF OK %d checks\n", g_checks);
    return 0;
#endif
}
