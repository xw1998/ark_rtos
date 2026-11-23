/**
* @file arkrtos_smp.h
* @brief SMP 骨架：每核状态、次核启动入口、核间中断通路（默认关）
* @details 必须先把话说清楚：**本内核的调度器是单核设计**——当前任务号
*          是全局的（arkrtos_current_task_id）、就绪表是全局位图、延迟
*          队列也是全局一份。打开 ARKRTOS_USE_SMP 不会把这些变成每核一份，
*          也不会让两个核同时跑任务。
*
*          这个模块提供的是多核的地基，而不是多核本身：
*            1) 每核一块状态（arkrtos_smp_cpu_t）：在线标志、当前任务号、
*               节拍计数、IPI 计数、空转计数；
*            2) 次核启动：arkrtos_smp_init() 由内核模块初始化调用，CPU0 直接
*               上线，1..N-1 交给 port 层的 arkrtos_port_cpu_start()；
*            3) 核间中断：arkrtos_smp_send_ipi() → arkrtos_port_send_ipi()，
*               收端调 arkrtos_smp_ipi_handler()（默认只计数，可覆盖）；
*            4) 在线核掩码与计数，供将来做核间唤醒/负载均衡时判断目标核。
*
*          次核被拉起来后默认只跑 arkrtos_smp_idle_loop()（WFI 空转），不参与
*          调度、不碰对象表。要让次核跑任务，打开 ARKRTOS_USE_SMP_SCHED：
*          那是独立的 AMP 模块（arkrtos_smp_sched.h），给次核一份自己的运行
*          队列，任务必须由调用方显式迁移过去、再显式要回来。全局调度器
*          仍然是单核的那一份，本文件也不假装它是多核调度器。
*
* @note 开关默认 0：关档下本文件的类型与原型全部不存在，arkrtos_smp.c
*       不产生任何代码，镜像体积与行为逐位不变。
* @note port 层对不支持的核返回 -1（weak 桩），arkrtos_smp_init() 如实把
*       "启动请求被拒绝"与"已经上线"分开记：请求成功不等于核在跑。
*/

#ifndef __ARKRTOS_SMP_H__
#define __ARKRTOS_SMP_H__

#include "arkrtos_types.h"
#include "arkrtos_config.h"

#if (ARKRTOS_USE_SMP == 1)

/* 次核栈大小（每核一块，下标即硬件核号）。
 * 真正的取值在板级头 board/<板>/arkrtos_board_config.h：
 * 汇编侧 arkrtos_context.S 看不到内核配置头，只能认板级头那一处。
 * 这里的 #ifndef 是兜底：宿主用例（tools/smp_gate.py 编
 * kernelsrc/tests/test_smp_host.c）不带板级头，没有它就编不过。
 * 两边默认值必须一致：改一处就得改两处。 */
#ifndef ARKRTOS_SMP_SEC_STACK_SIZE
#define ARKRTOS_SMP_SEC_STACK_SIZE   8192
#endif

/**
* @brief 单核状态块
* @details online 由次核自己在启动入口里置位（CPU0 由 init 置位）；
*          started 只表示"port 层接受了启动请求"。
*/
typedef struct
{
    volatile uint32 online;          /* 1 = 该核已进 arkrtos_smp_cpu_online() */
    volatile uint32 started;         /* 1 = 启动请求已被 port 层接受          */
    volatile uint32 current_task_id; /* 该核当前任务号（0 = 无任务/空闲）      */
    volatile uint32 tick;            /* 该核自己的节拍计数                    */
    volatile uint32 ipi_count;       /* 该核收到的核间中断次数                */
    volatile uint32 idle_count;      /* 该核执行空转循环的次数                */
    volatile uint32 ipi_back;        /* 该核应要求回给 CPU0 的 IPI 次数        */
    volatile uint32 ipi_back_req;    /* 1 = 主核要求下一个 IPI 顺手回一个      */
} arkrtos_smp_cpu_t;

/** @brief 每核状态表，下标即硬件核号（0..ARKRTOS_CPU_NUM-1） */
extern arkrtos_smp_cpu_t arkrtos_smp_cpu[ARKRTOS_CPU_NUM];

/** @brief 在线核掩码，bit n = 核 n 已上线 */
extern volatile uint32 arkrtos_smp_online_mask;

/**
* @brief 引导次核
* @details CPU0（调用者）直接标记上线；1..ARKRTOS_CPU_NUM-1 逐个调用
*          arkrtos_port_cpu_start()。
* @return 启动请求被接受的核数（含 CPU0）；port 层不支持的核不计入
* @note 返回 N 只代表"N 个核的启动请求被接受"，上线与否要看掩码。
*/
int32 arkrtos_smp_init(void);

/**
* @brief 次核启动入口（由 port 层在次核上调用）
* @param cpu 硬件核号（必须落在 1..ARKRTOS_CPU_NUM-1）
* @note 会把该核标记上线、开中断，然后进入空转循环，永不返回。
*       核号越界时什么都不标记，直接空转——宁可停在那里，也不要让一个
*       不知道自己是几号核的 CPU 去改别人的状态。
*/
void arkrtos_smp_secondary_main(uint32 cpu);

/**
* @brief 把指定核标记为上线（幂等）
* @param cpu 硬件核号，越界直接忽略
* @note 次核启动入口内部调用它；移植层若自己完成了启动，也可以直接调。
*/
void arkrtos_smp_cpu_online(uint32 cpu);

/**
* @brief 当前核的状态块
* @note arkrtos_port_cpu_id() 返回值越界时收敛到核 0：核号读错不应该变成
*       越界写。
*/
arkrtos_smp_cpu_t *arkrtos_smp_cpu_self(void);

/** @brief 在线核数（按掩码统计） */
uint32 arkrtos_smp_online_count(void);

/** @brief 在线核掩码 */
uint32 arkrtos_smp_online_mask_get(void);

/** @brief 记一拍（由各核的 tick 中断调用），只累加当前核的计数 */
void arkrtos_smp_tick(void);

/**
* @brief 发核间中断
* @param cpu 目标核号
* @return 0 = 已交给 port 层发出；-1 = 核号越界 / 该核未上线 / port 不支持
*/
int32 arkrtos_smp_send_ipi(uint32 cpu);

/**
* @brief 请指定次核在下一个 IPI 上顺手回一个 IPI 给 CPU0（R1 的回执读数）
* @param cpu 目标次核号（1..ARKRTOS_CPU_NUM-1）
* @return 0 = 已登记并已发出；-1 = 核号越界 / 该核未上线 / port 不支持
* @details 这不是「失效回执」，只是「主核那条接收路径真的通」的读数：
*          次核在 arkrtos_smp_ipi_handler() 里看到 ipi_back_req 就把本核
*          ipi_back++ 并 arkrtos_smp_send_ipi(0)，CPU0 侧则读自己的
*          ipi_count。send_ipi(0) 两侧都必须被允许，否则这条路径不存在。
* @note 关档（ARKRTOS_USE_SMP=0）时不存在。
*/
int32 arkrtos_smp_ping_main(uint32 cpu);

/**
* @brief 核间中断服务例程（由 port 层的 IPI 中断调用）
* @note 默认实现累加当前核的 ipi_count；若该核的 ipi_back_req 被登记为 1，
*       顺手回一个 IPI 给 CPU0 并把 ipi_back++（R1 的双向读数）。需要真实
*       动作（例如踢调度）时，应用提供同名强符号即可覆盖（__weak）。
*/
void arkrtos_smp_ipi_handler(void);

/**
* @brief 次核空转一步：累加 idle_count 后 WFI
* @note 默认次核循环就是它；将来接每核调度器时，把这一步换成 idle 任务的
*       调度入口。
*/
void arkrtos_smp_idle_loop(void);

#endif /* ARKRTOS_USE_SMP == 1 */

#endif /* __ARKRTOS_SMP_H__ */
