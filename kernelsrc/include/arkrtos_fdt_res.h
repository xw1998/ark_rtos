/**
* @file arkrtos_fdt_res.h
* @brief 把设备树当**驱动配置源**用：定时器与计数时基的口径核对
* @details 这一层回答的问题是「内核按什么频率在跑定时器」，而这个数字有两个
*          可能的来源：
*            1) 板级常量（编译进 port 的 ARKRTOS_PORT_TIMER_HZ 等）；
*            2) 设备树里的 /cpus/timebase-frequency 与定时器节点的
*               reg / clock-frequency。
*
*          MMU 档（LA64）走的是「正常项目」的做法：树是权威描述，板级常量是
*          它的一种落地。两者**必须一致**；不一致时本模块返回**具体原因码**，
*          由调用方拒绝继续（而不是挑一个"看起来能用"的值）——按错的频率算
*          节拍，症状是时间走得快/慢，看上去像调度器坏了。
*
*          「树里没写」和「写了但与板级不一致」是两种不同的事实，对应不同
*          的原因码：前者说明这张树描述得不全（内部定时器本来也可以不写），
*          后者说明树与板级矛盾，是配置错误。
*
*          本模块只读：不重编程定时器、不改 clock-frequency、不碰硬件。
*
*          开关 ARKRTOS_USE_FDT_RES 默认 0，且依赖 ARKRTOS_USE_FDT。
*
* @author xw
*/

#ifndef __ARKRTOS_FDT_RES_H__
#define __ARKRTOS_FDT_RES_H__

#include "arkrtos_types.h"
#include "arkrtos_fdt.h"

/* 原因码：每个都指向"树里的哪一条事实不成立"。检查按固定顺序进行
 * （时基 → 定时器 reg → 定时器 clock-frequency），返回**第一个**失败的码；
 * 已读到的值仍全部写进 out，便于把整行读数打出来。 */
#define ARKRTOS_RES_OK              0
#define ARKRTOS_RES_NO_CPUS       (-1)   /* 没有 /cpus 节点 */
#define ARKRTOS_RES_NO_TIMEBASE   (-2)   /* /cpus 没写 timebase-frequency */
#define ARKRTOS_RES_TIMEBASE_DIFF (-3)   /* timebase 与板级常量不一致 */
#define ARKRTOS_RES_NO_TIMER      (-4)   /* 按 compatible 找不到定时器节点 */
#define ARKRTOS_RES_TIMER_NO_REG  (-5)   /* 定时器节点没写 reg */
#define ARKRTOS_RES_TIMER_REG_DIFF (-6)  /* 定时器 reg 首单元与板级基址不一致 */
#define ARKRTOS_RES_TIMER_NO_HZ   (-7)   /* 定时器节点没写 clock-frequency */
#define ARKRTOS_RES_TIMER_HZ_DIFF (-8)   /* 定时器 clock-frequency 与板级不一致 */

/** @brief 板级期望。字段为 0 表示"这一项不检查"。 */
typedef struct
{
    uint32      expect_timebase;    /* 例如 10000000（mtime）或 100000000（rdtime） */
    const char *timer_compat;       /* 例如 "riscv,clint0"；0 = 不找定时器节点 */
    uint32      expect_timer_reg;   /* 定时器窗口基址；0 = 不检查 */
    uint32      expect_timer_hz;    /* 定时器 clock-frequency；0 = 不检查 */
} arkrtos_fdt_res_expect_t;

/** @brief 树里读出来的资源。has_* 区分「没写」与「值为 0」。 */
typedef struct
{
    uint32 timebase;
    uint8  has_timebase;

    /* 节点名（如 "clint@2000000"），不是绝对路径：本模块按 compatible
     * 找到节点，不假设它在哪一层，因此也不编一条看起来完整的路径出来。 */
    char   timer_node[64];
    char   timer_compat[32];
    uint32 timer_reg;
    uint32 timer_hz;
    uint8  has_timer;
    uint8  has_timer_reg;
    uint8  has_timer_hz;
} arkrtos_fdt_res_t;

/**
* @brief 读取并核对定时器 / 时基资源。
* @param expect 期望（不可为 0；字段置 0 表示不检查那一项）
* @param out    读到的值（失败时也尽量填满，便于打印）
* @return ARKRTOS_RES_OK 或上面某个负码
* @note 只读。
*/
int arkrtos_fdt_res_check(const arkrtos_fdt_res_expect_t *expect,
                          arkrtos_fdt_res_t *out);

/** @brief 原因码的稳定短名（用于日志），未知码返回 "?"。 */
const char *arkrtos_fdt_res_reason(int rc);

#endif /* __ARKRTOS_FDT_RES_H__ */
