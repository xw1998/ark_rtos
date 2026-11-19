/**
* @file arkrtos_smp_sched.h
* @brief 调度分核（AMP）：次核自己的运行队列 + 显式任务迁移（默认关）
* @details 先把边界说清楚，免得被读成"支持对称多核"：
*
*          **全局调度器没有改**。arkrtos_current_task_id、就绪位图、延时链
*          仍然是 CPU0 那一份，单核行为逐字节不变。本模块只做一件事：
*          在次核上跑一个**独立的小运行队列**，任务可以由调用方显式迁移
*          过去、再显式要回来。内核不做负载均衡，也不会自动迁移。
*
*          与 ARKRTOS_USE_SMP 的分层：
*            - USE_SMP（地基）：每核状态块、次核启动、核间中断；
*            - USE_SMP_SCHED（本模块）：次核真的跑任务。
*          打开后者必须同时打开前者，且 ARKRTOS_CPU_NUM >= 2（两条都由
*          arkrtos_config.h 的 #error 拦下）。
*
*          **次核任务的四条限制**（不是建议，是这套实现的边界；违反第 1
*          条时 port 层按致命异常停机，不静默跑错）：
*            1) 不得发 SVC、不得调用任何内核 API（设备表/文件系统/同步对象
*               都没有跨核保护，次核进 SVC 等于制造静默数据竞争）；
*            2) 不支持延时与阻塞：迁移只接受 READY 任务，次核没有延时链，
*               也没有 WAIT 唤醒路径；
*            3) 次核节拍只累加本核计数，不推进全局 arkrtos_kernel_tick，
*               因此全局时间/软定时器仍由 CPU0 单一驱动；
*            4) 迁移是"请求 + 轮询"而不是即时握手：迁出到次核在次核的下一个
*               节拍生效，迁回 CPU0 也由 CPU0 每拍 poll 收走。上限都是 1 拍。
*
*          **CPU0 侧的一致性**：迁到次核的任务 status 仍是 READY（status 的
*          语义是"可运行"，不是"在 CPU0 的队列里"），所以 CPU0 的就绪集
*          重建 / 一致性自检 / 线性优选都必须跳过它们——判据是本模块的
*          arkrtos_smp_sched_owned_by_other()。这是本模块唯一的"侵入点"。
*
* @note 开关默认 0：关档下本文件不声明任何符号，arkrtos_smp_sched.c 是空
*       翻译单元，镜像体积与行为逐位不变。
*/

#ifndef __ARKRTOS_SMP_SCHED_H__
#define __ARKRTOS_SMP_SCHED_H__

#include "arkrtos_types.h"
#include "arkrtos_config.h"

#if (ARKRTOS_USE_SMP_SCHED == 1)

/** @brief "本核空闲"的哨兵值（队列里/当前任务号都用它表示没有任务） */
#define ARKRTOS_SMP_SCHED_NIL           (-1)

/**
* @brief 次核进入调度域（不返回）
* @param cpu 硬件核号（必须 1..ARKRTOS_CPU_NUM-1）
* @details 由 arkrtos_smp_secondary_main() 调用：本核节拍定时器起来后，
*          在 WFI 循环里等第一拍——第一拍把空闲帧存进本核的 idle 槽，
*          之后的每一拍都从这里换到（或被换回）任务帧。
* @note 第一次节拍之前本核不跑任何任务。本核栈由汇编入口
*       (arkrtos_smp_secondary_entry) 设好，这里不再动 sp。
*/
void arkrtos_smp_sched_enter(uint32 cpu);

/**
* @brief 把任务迁到指定次核
* @param task_id 任务号（1..task_count）
* @param cpu     目标核号（1..ARKRTOS_CPU_NUM-1；0 不走这里）
* @return 0=已入次核运行队列；-1=任务号/核号非法；-2=目标核是 0（应调
*         arkrtos_smp_sched_task_back）；-3=任务不是 READY（RUNNING/WAIT/
*         DELAY/INVALID 一律拒绝）；-4=任务已归别的核；-5=目标队列满
* @note 只在本核（CPU0）调用：它会摘 CPU0 的就绪链，那是全局结构。
*/
int32 arkrtos_smp_sched_task_to(int32 task_id, uint32 cpu);

/**
* @brief 把迁出去的任务要回 CPU0
* @param task_id 任务号
* @return 0=请求已登记（下一拍生效）；-1=任务号非法；-4=该任务本来就在 CPU0
* @details 只是置请求位，不搬任何链表：次核在下一次节拍里把任务摘出来、
*          置回 CPU0 归属并登记归还；CPU0 每拍 arkrtos_smp_sched_poll()
*          把它重新挂回就绪链。请求登记后重复调用是幂等的。
*/
int32 arkrtos_smp_sched_task_back(int32 task_id);

/**
* @brief 次核节拍上下文里的调度决策
* @param cpu   本核号
* @param frame 本次异常压在被中断上下文栈上的帧指针
* @return 要 ertn 回去的帧指针（可能已换成另一个任务，或换回本核空闲帧）
* @note 只在次核的异常入口调用，且调用时中断已关（在异常里）。它自己
*       用本核自旋锁与 CPU0 的迁移/回收动作互斥。
*/
uint32 arkrtos_smp_sched_switch(uint32 cpu, uint32 frame);

/** @brief 次核记一拍（由次核的定时器中断调用） */
void arkrtos_smp_sched_tick(uint32 cpu);

/**
* @brief CPU0 每拍回收"要回来"的任务
* @note 由 arkrtos_kernel_tick_handler 的节拍体调用（仅在开关打开时编译）。
*       没有待回收任务时是一次常数级判断。
*/
void arkrtos_smp_sched_poll(void);

/**
* @brief 当前在指定核上运行的任务下标
* @return -1=该核空闲（或核号非法）
*/
int32 arkrtos_smp_sched_cur(uint32 cpu);

/**
* @brief 该任务是否归别的核
* @param task_idx 任务下标（0 起）
* @return 1=归次核（CPU0 的就绪集/自检必须跳过它），0=归 CPU0
* @note 开关关着时不存在这个符号；CPU0 侧的调用点都在同一开关内。
*/
uint8 arkrtos_smp_sched_owned_by_other(int32 task_idx);

/**
* @brief 读数快照（给自测/串口打印用）
* @param out 至少 8 个字的数组，依次写：
*            [0] 次核当前任务下标（-1=idle 转成 0xFFFFFFFF 报）
*            [1] 次核运行队列长度
*            [2] 次核切换次数
*            [3] 迁入次数
*            [4] 迁出次数
*            [5] 次核节拍计数
*            [6] 待回收队列非空标志
*            [7] 次核归属任务数（全表统计）
* @return 0=成功；-1=out 为空或没有次核
*/
int32 arkrtos_smp_sched_selftest(uint32 *out);

#endif /* ARKRTOS_USE_SMP_SCHED == 1 */

#endif /* __ARKRTOS_SMP_SCHED_H__ */
