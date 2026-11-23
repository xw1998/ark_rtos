/**
* @file arkrtos_smp_shootdown.c
* @brief 跨核 TLB 失效 + 回执（R4，默认关）
* @details 协议与边界写在 arkrtos_smp_shootdown.h 的文件头里，这里只重复
*          两条最容易读错的：
*
*          1) 发起核不撤自己的条目。它改账本之后走的是 activate /
*             install_space_window 那条自己撤自己铺的路，再在这里撤一次
*             会把调用方正在用的栈映射打掉；
*          2) ack 由目标核写。sd_ack 只加在目标侧，所以「不回执」的假实现
*             会让发起核停在 -2 —— 负控制就是这么造的（sd_drop）。
*
* @note 关档（ARKRTOS_USE_SMP_TLB_SD=0）下本文件是一个空翻译单元。
*/

#include "arkrtos_smp.h"

#if (ARKRTOS_USE_SMP == 1)

#include "arkrtos_smp_shootdown.h"

#if (ARKRTOS_USE_SMP_TLB_SD == 1)

#include "arkrtos_hal.h"
#if (ARKRTOS_USE_SMP_SCHED == 1)
#include "arkrtos_smp_sched.h"
#endif

arkrtos_smp_sd_slot_t arkrtos_smp_sd_slot[ARKRTOS_CPU_NUM];

volatile uint32 arkrtos_smp_sd_seq     = 0u;
volatile uint32 arkrtos_smp_sd_sent    = 0u;
volatile uint32 arkrtos_smp_sd_ack     = 0u;
volatile uint32 arkrtos_smp_sd_inval   = 0u;
volatile uint32 arkrtos_smp_sd_skip    = 0u;
volatile uint32 arkrtos_smp_sd_fail    = 0u;
volatile uint32 arkrtos_smp_sd_timeout = 0u;
volatile uint32 arkrtos_smp_sd_drop    = 0u;

/* 本核当前装着的窗口空间号；本档没有每核窗口（没开 SMP_SCHED 或没开
 * 页级窗口）时回 0xFFFFFFFF —— 「不知道」不许当成 0（空间 0 是真实空间，
 * 混起来会让目标核去撤一个它根本没有的条目）。 */
static uint32 sd_win_space(uint32 cpu)
{
#if ((ARKRTOS_USE_SMP_SCHED == 1) && (ARKRTOS_USE_PAGING == 1) && \
     (ARKRTOS_USE_PRIV == 1))
    return arkrtos_smp_sched_win_space(cpu);
#else
    (void)cpu;
    return 0xFFFFFFFFu;
#endif
}

int32 arkrtos_smp_tlb_shootdown(uint32 space, unsigned long va, uint32 pages)
{
    uint32 self;
    uint32 mask;
    uint32 seq;
    uint32 sent = 0u;
    uint32 i;
    uint32 spins;

    if((pages == 0u) || ((va & 0xFFFu) != 0u))
    {
        return -1;                    /* 页数/对齐不对：拒绝，不猜 */
    }

    self = arkrtos_port_cpu_id();
    if(self >= (uint32)ARKRTOS_CPU_NUM)
    {
        self = 0u;                    /* 核号读错时与 arkrtos_smp_cpu_self 同口径 */
    }

    if(arkrtos_smp_online_count() < 2u)
    {
        return -1;                    /* 没有第二个核：没有可证的对象 */
    }

    mask = arkrtos_smp_online_mask_get();
    seq  = ++arkrtos_smp_sd_seq;

    for(i = 0u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        if((i == self) || ((mask & (1u << i)) == 0u))
        {
            continue;
        }

        arkrtos_smp_sd_slot[i].space   = space;
        arkrtos_smp_sd_slot[i].va      = (uint32)va;
        arkrtos_smp_sd_slot[i].pages   = pages;
        arkrtos_smp_sd_slot[i].last_rc = 0xFFFFFFFFu;
        __sync_synchronize();         /* 字段先落地 */
        arkrtos_smp_sd_slot[i].seq     = seq;   /* 再发布序号 */
        __sync_synchronize();

        if(arkrtos_smp_send_ipi(i) != 0)
        {
            return -3;                /* 发不出去就如实失败，不假装发过 */
        }
        sent++;
    }

    if(sent == 0u)
    {
        /* 在线掩码与在线计数不一致（例如掩码里只剩本核）时会走到这里。
         * 一条都没发出去却回 0 等于「什么都没做也叫成功」—— 如实拒绝。 */
        return -1;
    }

    arkrtos_smp_sd_sent += sent;

    for(spins = 0u; spins < ARKRTOS_SMP_SD_SPINS; spins++)
    {
        uint32 done = 0u;

        for(i = 0u; i < (uint32)ARKRTOS_CPU_NUM; i++)
        {
            if((i == self) || ((mask & (1u << i)) == 0u))
            {
                continue;
            }
            if((arkrtos_smp_sd_slot[i].seq == seq) &&
               (arkrtos_smp_sd_slot[i].ack == seq))
            {
                done++;
            }
        }

        if(done == sent)
        {
            return 0;                 /* 每个目标核都自己写了回执 */
        }
        __sync_synchronize();
    }

    arkrtos_smp_sd_timeout++;         /* 等不到：如实失败，绝不当成功 */
    return -2;
}

void arkrtos_smp_sd_target_poll(void)
{
    arkrtos_smp_sd_slot_t *s;
    uint32 cpu;
    uint32 seq;
    uint32 space;
    int32  rc;

    cpu = arkrtos_port_cpu_id();
    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return;
    }

    s = &arkrtos_smp_sd_slot[cpu];
    seq = s->seq;
    if((seq == 0u) || (seq == s->ack))
    {
        return;                       /* 没有新请求，或这条已经处理过 */
    }

    if(arkrtos_smp_sd_drop != 0u)
    {
        return;                       /* 假绿自检：看了，但故意不回执 */
    }

    space = s->space;
    rc    = 1;                        /* 1 = 本核没有这个空间的条目 */

#if (ARKRTOS_SMP_SD_DEBUG == 1)
    /* 诊断档：把「本核账本空间」与「请求空间」打出来。R4 定位 refill 停机
     * 就是靠这一行 —— 实测 win=0 与 space=0 相等，说明问题不在账本，而是
     * 处理代码跑在位于窗口内的任务栈上（已在 la_smp_trap_entry 修掉：
     *  进 C 之前先换到本核 Trap 栈）。默认关，零代码零输出。 */
    arkrtos_board_puts("[sd] poll cpu=");
    arkrtos_board_putu(cpu);
    arkrtos_board_puts(" seq=");
    arkrtos_board_putu(seq);
    arkrtos_board_puts(" space=");
    arkrtos_board_putu(space);
    arkrtos_board_puts(" win=");
    arkrtos_board_putu(sd_win_space(cpu));
    arkrtos_board_puts("\n");
#endif

    if(sd_win_space(cpu) == space)
    {
        /* 撤旧 + 按账本重铺：本载体的窗口条目是自己 tlbwr 写下去的，
         * 只撤不铺会让本核下一次访问直接 refill 致命（见头文件第 2 条）。 */
        rc = arkrtos_port_paging_install_space_window(space);
        if(rc == 0)
        {
            arkrtos_smp_sd_inval++;
        }
        else
        {
            arkrtos_smp_sd_fail++;    /* 装不回去也要回执：让发起核知道结果 */
        }
    }
    else
    {
        arkrtos_smp_sd_skip++;
    }

    s->last_rc = (uint32)rc;
    __sync_synchronize();
    s->ack = seq;                     /* 回执：目标核写、发起核读 */
    arkrtos_smp_sd_ack++;
}

#endif /* ARKRTOS_USE_SMP_TLB_SD == 1 */

#endif /* ARKRTOS_USE_SMP == 1 */
