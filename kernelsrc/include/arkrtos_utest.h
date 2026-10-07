/**
 * @file arkrtos_utest.h
 * @brief 板端单元测试框架（utest）——用例自动注册 + 断言记账 + 汇总报告
 * @details 形态对齐 RT-Thread 的 utest / Zephyr 的 ztest：
 *          - 用例用 UTEST_CASE(id) 定义，由静态初始化期构造函数自动登记，
 *            不需要维护任何中心清单；
 *          - 断言宏（UTEST_CHECK / UTEST_REQUIRE / UTEST_CHECK_EQ ...）
 *            只记账不中断（REQUIRE 例外），一条用例里失败多少条都数得清；
 *          - 运行器遍历注册表，打印每条用例的 PASS/FAIL 与总账。
 *
 *          设计约束（与内核其它模块一致）：
 *          - 框架自身**不依赖** shell / printf：输出走可注入的汇
 *            （ark_utest_set_output），因此同一份核心也能在宿主机上编译并
 *            自测（见 tools/utest_host_selftest.c）；板端由 shell 的
 *            `utest` 命令把汇接到控制台。
 *          - 用例表的容量固定且显式（ARKRTOS_UTEST_MAX_CASES），溢出时
 *            计数并报出来，**绝不静默丢弃**用例。
 *          - 自动注册依赖 `__attribute__((constructor))`：armclang / GCC 原生支持；
 *            ARMCC 5 也会把函数指针放进 `.init_array`，其 C 库在 __main 里经
 *            __cpp_initialize__aeabi_ 调用（已用 armlink 出的镜像反汇编核实），
 *            因此 AC5 同样能自动登记。
 *
 *          门控：ARKRTOS_USE_UTEST（arkrtos_features.h，默认 1）。
 *          为 0 时本头文件与实现都不参与编译，体积与行为零变化。
 *
 * @note 本模块属于内核特权态代码。
 */
#ifndef ARKRTOS_UTEST_H
#define ARKRTOS_UTEST_H

#include "arkrtos_features.h"

#if (ARKRTOS_USE_UTEST == 1)

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 自动注册依赖构造函数属性：armclang / GCC 原生支持；ARMCC 5 会把它落到
 * .init_array，并在 __main -> __cpp_initialize__aeabi_ 里被调用（armlink 镜像
 * 反汇编已核实）。故三种工具链都无需额外清单。若某个工具链既不认该属性、又
 * 不跑 .init_array，退化的表现是"0 case(s) registered"，是显式的空账，而不是
 * 造假通过。 */

/** 用例函数：无参无返回，失败通过断言宏记账 */
typedef void (*ark_utest_fn_t)(void);

/** 一条用例：展示名（建议 "模块.场景"）+ 函数 */
typedef struct
{
    const char *name;
    ark_utest_fn_t fn;
} ark_utest_case_t;

/** 一次运行的汇总账 */
typedef struct
{
    uint32_t cases;         /**< 实际执行的用例数 */
    uint32_t passed;        /**< 全部断言通过的用例数 */
    uint32_t failed;        /**< 至少一条断言失败的用例数 */
    uint32_t skipped;       /**< 主动跳过的用例数 */
    uint32_t checks;        /**< 断言总条数 */
    uint32_t failed_checks; /**< 失败断言条数 */
    uint32_t overflow;      /**< 注册表溢出（未能登上的用例）次数 */
    uint32_t matched;       /**< 模式匹配到的用例数（含被跳过的） */
} ark_utest_summary_t;

/** 注册表容量：固定且显式；超出会计入 overflow，不静默丢弃 */
#ifndef ARKRTOS_UTEST_MAX_CASES
#define ARKRTOS_UTEST_MAX_CASES 64
#endif

/** 输出汇：框架把每一行交给它；默认 NULL 表示丢弃输出 */
typedef void (*ark_utest_out_t)(const char *s);
void ark_utest_set_output(ark_utest_out_t out);

/** 用例自注册。通常由 UTEST_CASE 宏自动调用；重复注册同一指针会被忽略 */
void ark_utest_register(const ark_utest_case_t *c);

/** 列出全部已注册用例（名字 + 序号）；返回用例数 */
uint32_t ark_utest_list(void);

/**
 * @brief 运行用例
 * @param pattern 子串过滤；NULL 或空串表示全部
 * @param out     若非 NULL，填汇总账
 * @return 失败用例数（0 表示全过）
 */
uint32_t ark_utest_run(const char *pattern, ark_utest_summary_t *out);

/* ---- 断言记账（仅可在用例函数内调用）---- */

/** 记一条断言（无论成败都计数）；仅可在用例函数内调用 */
void ark_utest_bump(void);
/** 记一条失败；detail 可为 NULL */
void ark_utest_fail(const char *file, int line, const char *expr, const char *detail);
/** 记一条数值不等；以有符号十进制与十六进制两种口径打印 */
void ark_utest_fail_num(const char *file, int line, const char *expr,
                        long a, long b);
/** 当前用例是否已出现失败（供 UTEST_REQUIRE 提前收手） */
int  ark_utest_ctx_failed(void);
/** 记一条跳过并置位；调用方随后 return */
void ark_utest_skip(const char *reason);
/** 当前用例是否已被跳过 */
int  ark_utest_ctx_skipped(void);

/** 注册一条用例（文件作用域辅助）：供 UTEST_CASE 宏展开使用 */
#define ARK_UTEST_CASE_DEF(_ident)                                        \
    static void utest_body_##_ident(void);                                \
    static const ark_utest_case_t utest_case_##_ident = {                 \
        #_ident, utest_body_##_ident };                                   \
    static void __attribute__((constructor)) utest_autoreg_##_ident(void) \
    {                                                                     \
        ark_utest_register(&utest_case_##_ident);                         \
    }                                                                     \
    static void utest_body_##_ident(void)

/**
 * @brief 定义一条用例
 * @param _ident 唯一标识符（同一翻译单元内不得重名）；同时作为展示名
 *
 * 用法：
 *   UTEST_CASE(mempool_alloc_free)
 *   {
 *       ...
 *       UTEST_CHECK_EQ(a, b);
 *   }
 */
#define UTEST_CASE(_ident) ARK_UTEST_CASE_DEF(_ident)

/** 真值断言：失败只记账，继续跑后面的断言 */
#define UTEST_CHECK(_expr)                                        \
    do {                                                          \
        ark_utest_bump();                                         \
        if(!(_expr))                                              \
        {                                                         \
            ark_utest_fail(__FILE__, __LINE__, #_expr, (const char *)0); \
        }                                                         \
    } while(0)

/**
 * 前置断言：失败记一条账后**立即返回**（后续断言不再执行）。
 * 用于"不满足就没法继续"的前置条件，避免拿坏状态继续跑出假绿。
 */
#define UTEST_REQUIRE(_expr)                                      \
    do {                                                          \
        UTEST_CHECK(_expr);                                       \
        if(ark_utest_ctx_failed())                                \
        {                                                         \
            return;                                               \
        }                                                         \
    } while(0)

/** 数值相等（long 口径）*/
#define UTEST_CHECK_EQ(_a, _b)                                    \
    do {                                                          \
        long utest_a_ = (long)(_a);                               \
        long utest_b_ = (long)(_b);                               \
        ark_utest_bump();                                         \
        if(utest_a_ != utest_b_)                                  \
        {                                                         \
            ark_utest_fail_num(__FILE__, __LINE__,                \
                               #_a " == " #_b, utest_a_, utest_b_); \
        }                                                         \
    } while(0)

/** 数值不等（long 口径）*/
#define UTEST_CHECK_NE(_a, _b)                                    \
    do {                                                          \
        long utest_a_ = (long)(_a);                               \
        long utest_b_ = (long)(_b);                               \
        ark_utest_bump();                                         \
        if(utest_a_ == utest_b_)                                  \
        {                                                         \
            ark_utest_fail_num(__FILE__, __LINE__,                \
                               #_a " != " #_b, utest_a_, utest_b_); \
        }                                                         \
    } while(0)

/** 区间断言：lo <= x && x <= hi */
#define UTEST_CHECK_RANGE(_lo, _x, _hi)                           \
    do {                                                          \
        long utest_lo_ = (long)(_lo);                             \
        long utest_x_  = (long)(_x);                              \
        long utest_hi_ = (long)(_hi);                             \
        ark_utest_bump();                                         \
        if((utest_x_ < utest_lo_) || (utest_x_ > utest_hi_))      \
        {                                                         \
            ark_utest_fail_num(__FILE__, __LINE__,                \
                               #_lo " <= " #_x " <= " #_hi, utest_x_, utest_lo_); \
        }                                                         \
    } while(0)

/** 无条件失败（用于"走到这里本不该发生"的分支）*/
#define UTEST_FAIL(_msg)                                          \
    do {                                                          \
        ark_utest_bump();                                         \
        ark_utest_fail(__FILE__, __LINE__, "FAIL", (_msg));       \
    } while(0)

/** 主动跳过当前用例（其后内容不再执行）*/
#define UTEST_SKIP(_reason)                                       \
    do {                                                          \
        ark_utest_skip(_reason);                                  \
        return;                                                   \
    } while(0)

#ifdef __cplusplus
}
#endif

#endif /* ARKRTOS_USE_UTEST */

#endif /* ARKRTOS_UTEST_H */
