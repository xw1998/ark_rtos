/**
* @file arkrtos_sec.h
* @brief 安全基线自检（Security baseline self-check）。开关 ARKRTOS_USE_SECCHECK。
* @details 这个模块回答一个问题：**这块板子现在这套构建，按安全基线该有的东西
*          有哪些真的在、有哪些不在了**。它做三件事，且只做这三件：
*            1. 读编译期开关，报「基线要求的那几项是不是都编进来了」；
*            2. 读运行期结构（分区表 / 槽位 / 审计 / 崩溃台账 / 硬件安全态 / 实时度量），
*               报它们现在满不满足各自的不变式；
*            3. 对**这套构建里根本没有对应通路**的项，明确报 SKIP 并写清原因，
*               而不是猜一个好看的结果。
*
*          输出分四档，缺一不可：
*            PASS    基线要求如此，实测如此；
*            FAIL    基线要求如此，实测不如此（这一条挡现场，不是建议）；
*            FINDING 已知缺口：功能没做 / 没编进来。它不是 FAIL（设备没坏），
*                    但认证清单要按缺口登记，所以单独一档，不会被算进 PASS；
*            SKIP    没有可走的路去证实（例如内核没有读取 Flash 选项字节的接口）。
*
*          这个模块**只读**：除了审计 pass 会写它自己的统计、以及各查询接口
*          已有的副作用以外，不写任何被检查的结构，也不"顺手修好"——
*          修好会让下一次自检失去证据。
*
* @note 默认关：关掉时源码不参与编译，外壳命令表逐字节不变。
* @author xw
*/
#ifndef __ARKRTOS_SEC_H__
#define __ARKRTOS_SEC_H__

#include "arkrtos_types.h"

/* 与 arkrtos_features.h 保持一致，这里只给独立包含时的兜底 */
#ifndef ARKRTOS_USE_SECCHECK
#define ARKRTOS_USE_SECCHECK        0
#endif

/** @brief 自检项上限（超过的检查不会进报告，见 arkrtos_sec_add 的返回值） */
#define ARKRTOS_SEC_ITEM_MAX        (16u)

/** @brief 单项结论 */
#define ARKRTOS_SEC_PASS            (0u)
#define ARKRTOS_SEC_FAIL            (1u)
#define ARKRTOS_SEC_FINDING         (2u)
#define ARKRTOS_SEC_SKIP            (3u)

/** @brief 板级弱符号的返回值：本板没有实现这条自查通路（不是"检查通过"）。
 *         与上面的 PASS/FAIL/FINDING/SKIP 是两套语义：那是检查结果，这是
 *         「根本没有读数」。值不得单独修改。 */
#define ARKRTOS_SEC_ERR_NO_PORT     (-1)

/**
* @brief 一条自检项
* @details 说明文字不做字符串拼接：模块只给出 printf 风格的格式串与最多 4 个
*          整数，由**外壳**去格式化。理由是内核里另起一套 snprintf 会把代码
*          体积翻着涨，而外壳本来就有格式化能力。
*/
typedef struct {
    const char *name;         /* 稳定的机器可读名，例如 "mpu.slot_windows" */
    uint32      state;        /* ARKRTOS_SEC_PASS / FAIL / FINDING / SKIP */
    const char *fmt;          /* 说明格式串；可以为 "" */
    uint32      v[4];         /* 喂给 fmt 的参数 */
} arkrtos_sec_item_t;

/** @brief 一次自检的完整报告 */
typedef struct {
    uint32 count;             /* 有效项数 */
    uint32 pass;
    uint32 fail;
    uint32 finding;
    uint32 skip;
    arkrtos_sec_item_t item[ARKRTOS_SEC_ITEM_MAX];
} arkrtos_sec_report_t;

#if (ARKRTOS_USE_SECCHECK == 1)

/**
* @brief 跑一遍安全基线自检
* @param p_out 报告输出（必须非空）
* @return FAIL 的条数（0 = 没有硬失败；FINDING 不计入）
*/
uint32 arkrtos_sec_run(arkrtos_sec_report_t *p_out);

/** @brief 报告里某一档的条数（外壳打印用；state 越界返回 0） */
uint32 arkrtos_sec_count(const arkrtos_sec_report_t *p_rep, uint32 state);

/** @brief 结论名：pass / fail / finding / skip */
const char *arkrtos_sec_state_name(uint32 state);

#else

#define arkrtos_sec_state_name(state)   "disabled"

#endif /* ARKRTOS_USE_SECCHECK */

#endif /* __ARKRTOS_SEC_H__ */
