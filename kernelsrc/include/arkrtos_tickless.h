/**
* @brief Ark RTOS tickless idle（内核内部头文件）
* @details 打开 ARKRTOS_USE_TICKLESS 后，idle 循环不再一拍一次 WFI，而是
*          先算"下一次真正要干活"离现在还有几拍，再把 SysTick 重新编程成
*          一段更长的节拍、睡完一次、醒来把这几十拍一次补齐。
*
*          三条边界，写在这里以免被读成"省电模式"：
*          1) 只处理 tick 驱动的唤醒源：延时链（任务超时、周期等待）与软件
*             定时器。idle 期间没有任何任务在跑，所以不会有别人来 post
*             信号量——真要有中断来，也是那一段睡眠被提前打断，按"提前
*             唤醒"结算（见 arkrtos_tickless_wake_ticks）。
*          2) 睡眠长度有上限（ARKRTOS_TICKLESS_MAX_TICKS），既保证唤醒后
*             补发的拍数少，也保证不会超出 SysTick 的 24 位计数。
*          3) 时间不会因此变慢：醒来时按"实际过去了多少个时钟"折算整拍，
*             余数进下一拍的相位，所以每次 idle 不会丢半拍。
*
*          纯计算的两个函数（plan / split）不碰硬件，宿主测试直接调用；
*          碰硬件的那两个在板上才有效，未上板验证前不得写成已验证。
*/
#ifndef __ARKRTOS_TICKLESS_H__
#define __ARKRTOS_TICKLESS_H__

#include "arkrtos_def.h"
#include "arkrtos_config.h"

#if (ARKRTOS_USE_TICKLESS == 1)

/** 无事件（延时链空 / 没有活动定时器）的哨兵值，给 plan 用 */
#define ARKRTOS_TICKLESS_NO_EVENT   0xFFFFFFFFu

/**
* @brief 纯计算：这一次最多能睡几拍
* @param delay_ticks 延时链最近到期还有几拍（无事件给 NO_EVENT）
* @param timer_ticks 软件定时器最近到期还有几拍（无事件给 NO_EVENT）
* @param cap         上限（ARKRTOS_TICKLESS_MAX_TICKS）
* @return >= 1 的拍数；< 2 表示"不值得跳"，调用方走普通 WFI
* @details 取两者最小，再夹到 [1, cap]。已经是 1 拍也返回 1：调用方据此
*          走普通 WFI，不做任何重新编程。
*/
uint32 arkrtos_tickless_plan(uint32 delay_ticks, uint32 timer_ticks, uint32 cap);

/**
* @brief 纯计算：实际过去的计数折成"整拍 + 余数"
* @param elapsed_counts  自编程以来过去的时钟计数
* @param counts_per_tick 一个正常节拍的计数
* @param ticks           输出：整拍数（可以为 0）
* @param remain_counts   输出：不足一拍的余数（相位补偿用）
* @details elapsed < cpt 时 ticks = 0，表示"被提前叫醒但没有走满一拍"。
*/
void arkrtos_tickless_split(uint32 elapsed_counts, uint32 counts_per_tick,
                            uint32 *ticks, uint32 *remain_counts);

/**
* @brief idle 循环里的"睡一觉"入口（替代 ARKRTOS_WFI）
* @return 本次跳过并在醒来后补发掉的整拍数（0 = 没有跳）
* @details 只有在跑到这里时确定"至少两拍内没人需要 CPU"才会重新编程；
*          否则退化为一次普通 WFI。返回时所有被跳过的节拍都已经补发，
*          调用方可以直接进入下一轮。
*/
uint32 arkrtos_tickless_idle(void);

/**
* @brief tick 冲突入口：本次 SysTick 中断该补几拍
* @return 0 = 没有处在长节拍里（按普通一拍处理）；n > 0 = 一共 n 拍
* @details 由 arkrtos_kernel_tick_handler 在中断入口调用一次。它顺带把
*          SysTick 恢复成正常周期（含相位余数），所以调用之后的中断间隔
*          立刻回到一拍。读序为"先读 COUNTFLAG 再读 VAL"：读 CTRL 会清
*          标志，两次读取必须来自同一次结账。
*
*          相位不为 0 时恢复出来的是"补满这一拍"的短周期，并在那一次
*          中断里自己恢复成整拍：短周期若不被收回，之后每一拍都会变短，
*          节拍就永久性地快了。
*/
uint32 arkrtos_tickless_wake_ticks(void);

/**
* @brief 累计读数：SysTick 中断次数与长睡眠次数
* @param isrs    输出：进入 SysTick 中断的次数（一个中断算一次，不管它补了几拍）
* @param sleeps  输出：成功编程为长节拍并进 WFI 的次数（0 = 没睡过）
* @details 板端唯一能证明"tickless 真的在睡"的读数：没有它，"节拍没变慢"
*          与"开关没生效"在串口上长得一模一样。两个指针都可以传空。
*
*          "跳过的拍数"不单独计数，直接用 `arkrtos_kernel_get_tick() - isrs`
*          算：每个中断至少贡献一拍，剩下的必然是补发路径走完的。
*          所以 `节拍总数 == isrs + skipped` 是定义而不是测得量。
*          真正有信息量的是 `isrs` 与节拍总数的比值（4 倍睡眠时约 1:4）。
*          计数只增不清，复位才归零。
*/
void arkrtos_tickless_stats(uint32 *isrs, uint32 *sleeps);

#endif /* ARKRTOS_USE_TICKLESS */

#endif /* __ARKRTOS_TICKLESS_H__ */
