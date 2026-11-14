/**
* @file arkrtos_rt.h
* @brief 实时性度量：中断入口、唤醒、抢占、临界区的最坏值（内核内部头文件）
* @details 打开 ARKRTOS_USE_RTSTAT 后，内核在几个固定位置打时间戳，用 DWT
*          周期计数器（1 个 CPU 周期分辨率）累计一组"最坏值"读数，供 shell 的
*          `rt` 命令打印。它回答的是认证/验收会问的那几个问题：
*
*            - 节拍中断自己占多少时间（tick.isr）；
*            - 节拍间隔抖多少（jitter，有符号，单位周期）；
*            - 一个任务从"该被唤醒"到"真的在跑"要多久（wake.lo / wake.hi）；
*            - 内核最长的一次关中断有多久（crit.max）。
*
*          三条边界，写在这里以免被读成"实时性已被证明"：
*          1) 这些是**本机在本配置下测到的最大值**，不是形式化 WCET。测不到
*             更坏的情况，只说明这段时间没发生更坏的情况。
*          2) 测量本身有开销：crit.max 在临界区内多读一次周期计数器，
*             开着这个开关时测到的最长关中断比关掉时**略大**（见文档里的
*             开销实测）。默认关时这些点全是死代码，体积与行为零变化。
*          3) DWT 周期计数器是调试单元资源，与 mdk_trace 共用同一来源；
*             两者都只是"打开使能位"，互不影响计数值。
*
* @author xw
*/
#ifndef __ARKRTOS_RT_H__
#define __ARKRTOS_RT_H__

#include "arkrtos_def.h"
#include "arkrtos_config.h"

/* 返回码：值不得单独修改。last_err 里另有细分原因。 */
#define ARKRTOS_RT_ERR_NOTREADY (-1)   /* 测量没起来 / 还没有读数 */
#define ARKRTOS_RT_ERR_START    (-1)   /* 辅助任务起不来（细节见 arkrtos_rt_last_error） */


#ifndef ARKRTOS_USE_RTSTAT
#define ARKRTOS_USE_RTSTAT              0
#endif

#if (ARKRTOS_USE_RTSTAT == 1)

/** @brief 一行读数（周期 + 微秒两种单位，微秒按当前主频折算） */
typedef struct {
    const char *name;       /* 固定字符串，调用方不得释放 */
    uint32 n;               /* 样本数；0 表示这一项还没有测量 */
    uint32 min_c, avg_c, max_c;   /* CPU 周期 */
    uint32 min_us, avg_us, max_us;/* 微秒（整数，四舍五入到 1 us） */
    uint32 late_n;          /* 被丢弃的"迟到"样本（见 arkrtos_rt_measure_wake） */
} arkrtos_rt_row_t;

/** @brief 读数行数上限（由 arkrtos_rt_snapshot 填） */
#define ARKRTOS_RT_ROWS                 4u

/** @brief 模块初始化：打开周期计数器、算出每拍周期数、清空统计 */
void   arkrtos_rt_init(void);

/**
* @brief 重新武装周期计数器并确认它真的在走（1 = 在走，0 = 转不动）
* @details 不是纯读：会写 DEMCR.TRCENA / DWT.CYCCNTENA。调试器断开时
*          会把这两位清掉，所以每次开测前都调一次，而不是假定开机时
*          打开的就一直有效。不碰统计量。
*/
int32  arkrtos_rt_counter_alive(void);

/** @brief 节拍中断入口/出口时间戳（由 arkrtos_kernel_tick_body 调用） */
void   arkrtos_rt_tick_enter(void);
void   arkrtos_rt_tick_exit(void);

/** @brief 临界区入口/出口（由移植层 enter/exit_critical 调用，带嵌套计数） */
void   arkrtos_rt_crit_enter(void);
void   arkrtos_rt_crit_exit(void);

/**
* @brief 场景一：当前任务自己按 1 ms 延时循环 n 次，量"节拍 → 本任务继续跑"
* @param n 采样次数
* @return 0 = 完成；-1 = 采样器未就绪（周期计数器不可用）
* @details 每醒来一次记一个 delta = 现在 - 最近一次节拍入口的时间戳。
*          一个正常样本必然小于一拍；超过一拍的样本不参与统计，计入
*          late_n 单独报出来——那是"低优先级任务被更高优先级挡了一会儿"，
*          不是唤醒延迟。
*/
int32  arkrtos_rt_measure_wake(uint32 n);

/**
* @brief 场景二：最高优先级任务被 post 唤醒的延迟
* @param n 采样次数
* @return 0 = 完成；-1 = 采样器未就绪 / 辅助任务建不起来
* @details 第一次调用时按需创建一个优先级 1 的辅助任务（静态栈 1 KB），
*          本任务（调用者）自己当"发信号的一方"：记时间戳 → post → 等回执。
*          辅助任务在醒来后的第一行取时间戳，所以这个数包含 post 路径、
*          抢占判定、两层上下文切换。
*/
int32  arkrtos_rt_measure_preempt(uint32 n);

/**
* @brief 取一份读数快照（按名称固定顺序，未测量的行 n = 0）
* @return 行数
*/
uint32 arkrtos_rt_snapshot(arkrtos_rt_row_t *rows, uint32 max_rows);

/** @brief 节拍抖动（有符号周期，正 = 比标称晚）：返回 0 表示还没有样本 */
uint32 arkrtos_rt_jitter(int32 *min_c, int32 *max_c, uint32 *n);

/** @brief 清空所有统计（不影响已经建好的辅助任务） */
void   arkrtos_rt_reset(void);

/** @brief 一个节拍标称多少 CPU 周期（0 = 周期计数器不可用） */
uint32 arkrtos_rt_cycles_per_tick(void);

/**
* @brief 上一次准备失败的原因码（0 = 没失败）
* @details 1/2 = 两个信号量创建失败（多半是信号量池满）；
*          5 = 辅助任务登记失败（返回值 -1，窗口/优先级校验不过）；
*          其它非 0 只说明失败，具体码由实现给出。
*/
int32  arkrtos_rt_last_error(void);

#endif /* ARKRTOS_USE_RTSTAT */

#endif /* __ARKRTOS_RT_H__ */
