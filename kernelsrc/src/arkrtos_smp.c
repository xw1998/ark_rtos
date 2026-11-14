/**
* @file arkrtos_smp.c
* @brief SMP 骨架实现：每核状态、次核启动、核间中断（默认关）
* @details 这个文件只做四件事，不多做一件：
*            1) 维护每核状态表与在线掩码；
*            2) 引导次核（CPU0 直接上线，其余交给 port 层）；
*            3) 次核入口：标记上线 → 开中断 → 空转（不参与调度）；
*            4) 核间中断的发起与默认处理。
*
*          **不在这里做**：多核调度、每核运行队列、核间任务迁移、
*          全局时钟同步、对象表的每核分区。这些都需要把调度器拆成每核
*          一份，属于另一件事；本文件不假装完成，也不给"看起来能并行"
*          的接口。
*
* @note 开关默认 0：关档下本文件是一个空翻译单元，不产生任何符号与代码。
*/

#include "arkrtos_smp.h"

#if (ARKRTOS_USE_SMP == 1)

#include "arkrtos_hal.h"

/* 弱符号：armclang / armcc 认 __weak，GCC 认 __attribute__((weak))。
 * 宿主（MinGW）上弱定义会被老 ld 当成 .eh_frame 的坏重定位而链接失败，
 * 而宿主用例本来也不需要覆盖这个处理函数，所以在 Windows 主机上让它变
 * 成普通强符号。目标侧（Keil / arm-none-eabi-gcc）仍然是弱符号，应用可以
 * 用同名强函数覆盖。 */
#if defined(__CC_ARM) || defined(__ARMCC_VERSION) || defined(__CC_ARM_VERSION)
#define ARKRTOS_SMP_WEAK        __weak
#elif defined(__GNUC__) && !defined(_WIN32)
#define ARKRTOS_SMP_WEAK        __attribute__((weak))
#else
#define ARKRTOS_SMP_WEAK
#endif

/* 每核状态表：下标就是硬件核号，CPU0 永远存在 */
arkrtos_smp_cpu_t arkrtos_smp_cpu[ARKRTOS_CPU_NUM];

/* 在线核掩码：bit n = 核 n 已上线 */
volatile uint32 arkrtos_smp_online_mask = 0u;

/* ============================================================
 * 内部工具
 * ============================================================ */

static void smp_cpu_clear(arkrtos_smp_cpu_t *p_cpu)
{
    p_cpu->online          = 0u;
    p_cpu->started         = 0u;
    p_cpu->current_task_id = 0u;
    p_cpu->tick            = 0u;
    p_cpu->ipi_count       = 0u;
    p_cpu->idle_count      = 0u;
}

/**
* @brief 原子置位在线掩码的某一位
* @note 用 CAS 重试而不是读-改-写：两个次核同时上线时，读-改-写会丢掉一个。
*/
static void smp_mask_set(uint32 cpu)
{
    uint32 old_mask;
    uint32 new_mask;

    do
    {
        old_mask = arkrtos_smp_online_mask;
        new_mask = old_mask | ((uint32)1u << cpu);
        if(new_mask == old_mask)
        {
            return;                       /* 已经是 1，不必再 CAS */
        }
    } while(arkrtos_port_atomic_cas((volatile uint32 *)&arkrtos_smp_online_mask,
                                    old_mask, new_mask) == 0u);
}

/* ============================================================
 * 对外接口
 * ============================================================ */

int32 arkrtos_smp_init(void)
{
    uint32 i;
    int32  accepted;

    for(i = 0u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        smp_cpu_clear(&arkrtos_smp_cpu[i]);
    }

    /* 调用者就是 CPU0，它已经在跑，不需要"启动" */
    arkrtos_smp_cpu[0].started = 1u;
    arkrtos_smp_cpu[0].online  = 1u;
    arkrtos_smp_online_mask   = 0u;      /* 重复初始化时旧掩码必须清掉 */
    smp_mask_set(0u);
    accepted = 1;

    for(i = 1u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        if(arkrtos_port_cpu_start(i) == 0)
        {
            /* 只记"请求被接受"：核是否真的跑起来由它自己置 online，
             * 中间可能失败（时钟没配好、固件缺失），不能在这里代填。 */
            arkrtos_smp_cpu[i].started = 1u;
            accepted++;
        }
    }

    return accepted;
}

void arkrtos_smp_cpu_online(uint32 cpu)
{
    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return;                           /* 核号越界：宁可不动，也不要写越界 */
    }

    arkrtos_smp_cpu[cpu].online = 1u;
    smp_mask_set(cpu);
}

void arkrtos_smp_secondary_main(uint32 cpu)
{
    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        /* 不知道自己几号核，就不要改任何状态。空转等外部复位。 */
        for(;;)
        {
            arkrtos_port_wfi();
        }
    }

    arkrtos_smp_cpu_online(cpu);
    arkrtos_port_enable_irq();

    /* 次核目前只空转：调度器还是单核的，这里没有可跑的任务队列。 */
    for(;;)
    {
        arkrtos_smp_idle_loop();
    }
}

arkrtos_smp_cpu_t *arkrtos_smp_cpu_self(void)
{
    uint32 cpu = arkrtos_port_cpu_id();

    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        cpu = 0u;                         /* 核号读错时收敛到核 0，不越界 */
    }

    return &arkrtos_smp_cpu[cpu];
}

uint32 arkrtos_smp_online_count(void)
{
    uint32 mask = arkrtos_smp_online_mask;
    uint32 n    = 0u;

    while(mask != 0u)
    {
        n += (mask & 1u);
        mask >>= 1;
    }

    return n;
}

uint32 arkrtos_smp_online_mask_get(void)
{
    return arkrtos_smp_online_mask;
}

void arkrtos_smp_tick(void)
{
    arkrtos_smp_cpu_self()->tick++;
}

int32 arkrtos_smp_send_ipi(uint32 cpu)
{
    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return -1;
    }

    if((arkrtos_smp_online_mask & ((uint32)1u << cpu)) == 0u)
    {
        return -1;                        /* 没上线的核没有中断入口可投 */
    }

    return arkrtos_port_send_ipi(cpu);
}

ARKRTOS_SMP_WEAK void arkrtos_smp_ipi_handler(void)
{
    arkrtos_smp_cpu_self()->ipi_count++;
}

void arkrtos_smp_idle_loop(void)
{
    arkrtos_smp_cpu_self()->idle_count++;
    arkrtos_port_wfi();
}

#endif /* ARKRTOS_USE_SMP == 1 */
