/**
* @file arkrtos_smp_shootdown.h
* @brief 跨核 TLB 失效 + 回执（R4，默认关）
* @details 这里做的**不是**「发个 IPI 然后假定目标核已经失效」。协议是：
*
*            发起核：给每个在线核写一条请求（空间 / VA / 页数 / 序号），
*                    发 IPI，然后**等那个核把 ack 写回序号**；
*            目标核：在 IPI 处理里看到 seq != ack，就按请求做失效（撤旧
*                    条目再按空间账本重铺，装完读回自证），失败也照样把
*                    ack 写回 —— 但把返回码记进 sd_last_rc / sd_fail。
*
*          ack 是**目标核写、发起核读**的计数，不是发起核自己数的迭代
*          次数：假实现（发完就返回 0）会让 `sd_ack` 一直是 0，负控制
*          （drop 档）也让发起核只能拿到 -2。这两条都由门禁钉住。
*
*          **边界（必须一起读）**：
*            1) 目标核只有在「它当前装着的窗口 == 请求的空间」时才真去撤
*               （别的空间在该核上没有条目，撤了也没有意义），这种情况记
*               sd_skip++，并且同样回执 —— 回执证明「它看过了」；
*            2) 失效动作是「撤旧 → 按账本重铺」，因为本载体的窗口条目不是
*               硬件缺页填的（页级窗口是自己 tlbwr 写下去的），只撤不铺会
*               让目标核下一次访问直接 refill 致命。这等于「带新 PTE 的
*               shootdown」，与 Linux 的做法同形；
*            3) 等待是**迭代次数上限**，不是时间上限：超时如实返回 -2 并
*               记 sd_timeout，绝不当成功；
*            4) 真机未验证（R5）：竞态与一致性协议不能只在 QEMU 上判死。
*
* @note 开关 ARKRTOS_USE_SMP_TLB_SD 默认 0：关档下本文件不产生任何符号，
*       镜像体积与行为逐位不变。打开要求 ARKRTOS_USE_SMP=1 且 CPU_NUM>=2
*       （在 arkrtos_config.h 里以 #error 拦）。
*/

#ifndef __ARKRTOS_SMP_SHOOTDOWN_H__
#define __ARKRTOS_SMP_SHOOTDOWN_H__

#include "arkrtos_types.h"
#include "arkrtos_config.h"

#if ((ARKRTOS_USE_SMP == 1) && (ARKRTOS_USE_SMP_TLB_SD == 1))

/** @brief 等回执的迭代上限（不是时间上限，见文件头第 3 条） */
#ifndef ARKRTOS_SMP_SD_SPINS
#define ARKRTOS_SMP_SD_SPINS        2000000u
#endif

/** @brief 诊断打印开关（默认 0）：每次处理请求时打出本核账本空间与请求空间
 *  @details 只在排查「目标核为什么没撤」时打开。R4 定位 refill 停机用过它
 *           （实测 win=0 == space=0，问题不在账本而在跑在窗口内的栈上）。
 *           开档会往控制台写东西，属于诊断档，不是正常配置。 */
#ifndef ARKRTOS_SMP_SD_DEBUG
#define ARKRTOS_SMP_SD_DEBUG         0
#endif

/**
* @brief 每个目标核一条请求槽：seq 是发布点（最后写），ack 是回执
* @details arkrtos_smp_ipi_handler() 在打开 ARKRTOS_USE_SMP_TLB_SD 后于尾部调用
*          arkrtos_smp_sd_target_poll()，它按本核下标取槽，先读 seq 再读参数：
*          发起侧先写 space/va/pages/last_rc 再写 seq（两条 __sync_synchronize
*          夹住），所以目标核看到新 seq 时参数一定已经是这条请求的。
*          目标核处理完把 ack 写成同一个 seq —— 回执由目标核写、发起核只读。
*/
typedef struct
{
    volatile uint32 seq;      /* 发起核写：本槽的请求序号（0 = 空）        */
    volatile uint32 ack;      /* 目标核写：已处理到哪个序号                */
    volatile uint32 space;    /* 要失效的空间号                            */
    volatile uint32 va;       /* 起始 VA（页对齐）                         */
    volatile uint32 pages;    /* 页数（>= 1）                              */
    volatile uint32 last_rc;  /* 目标核记：最近一次失效的返回码            */
} arkrtos_smp_sd_slot_t;

/** @brief 每核一个请求槽，下标即目标核号 */
extern arkrtos_smp_sd_slot_t arkrtos_smp_sd_slot[ARKRTOS_CPU_NUM];

extern volatile uint32 arkrtos_smp_sd_seq;      /* 发起核：序列号             */
extern volatile uint32 arkrtos_smp_sd_sent;     /* 发起过多少条请求（按核计） */
extern volatile uint32 arkrtos_smp_sd_ack;      /* 收到多少条回执（目标核记） */
extern volatile uint32 arkrtos_smp_sd_inval;    /* 目标核真做了失效的次数     */
extern volatile uint32 arkrtos_smp_sd_skip;     /* 目标核判定无需失效的次数   */
extern volatile uint32 arkrtos_smp_sd_fail;     /* 失效动作自己失败（rc<0）   */
extern volatile uint32 arkrtos_smp_sd_timeout;  /* 等不到回执的次数           */
extern volatile uint32 arkrtos_smp_sd_drop;     /* 测试用：1 = 目标核故意不回执 */

/**
* @brief 让所有在线核失效某个空间的一段 VA（R4）
* @param space 空间号
* @param va 起始 VA（按 4 KB 对齐）
* @param pages 页数（>= 1）
* @return 0 = 全部目标核都回了执
* @return -1 = 参数/环境不允许（页数为 0、VA 未对齐、没有第二个在线核）
* @return -2 = 有目标核在迭代上限内没回执（如实失败，绝不当成功）
* @return -3 = IPI 发不出去
* @details 发起核自己**不**在这里撤条目：它的窗口由调用方按自己的上下文
*          管（activate / install_space_window 都会在改完账本后自行撤铺），
*          在这里再撤一次只会把调用方的栈搞掉。
* @note 只能在任务上下文调用（这里会自旋等回执，占着 CPU 不放）；
*       中断上下文调用会把目标核饿死。
*/
int32 arkrtos_smp_tlb_shootdown(uint32 space, unsigned long va, uint32 pages);

/**
* @brief 目标侧：在 IPI 处理里处理本核的请求槽（R4）
* @details arkrtos_smp_ipi_handler() 在打开本开关后会调用它。幂等：
*          同一个 seq 处理过一次就不再处理（ack 已经是它了）。
*/
void arkrtos_smp_sd_target_poll(void);

#else

#define arkrtos_smp_tlb_shootdown(space, va, pages) \
    ((void)(space), (void)(va), (void)(pages), -1)
#define arkrtos_smp_sd_target_poll()                do { } while(0)

#endif /* ARKRTOS_USE_SMP && ARKRTOS_USE_SMP_TLB_SD */

#endif /* __ARKRTOS_SMP_SHOOTDOWN_H__ */
