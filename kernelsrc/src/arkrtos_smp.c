/**
* @file arkrtos_smp.c
* @brief SMP 骨架实现：每核状态、次核启动、核间中断（默认关）
* @details 这个文件只做四件事，不多做一件：
*            1) 维护每核状态表与在线掩码；
*            2) 引导次核（CPU0 直接上线，其余交给 port 层）；
*            3) 次核入口：标记上线 → 开中断 → 空转（不参与调度）；
*            4) 核间中断的发起与默认处理。
*
*          **2026-11-19 现状**：② 次核启动 与 ③ IPI 通路在 QEMU LA64 上
*          已经真跑通（port 层写 IOCSR mailbox 拉起次核、IPI 双向可达，
*          串口有实测读数，见 docs/QEMU模拟器移植与验证.md 的 SMP 一节）。
*          ④ 调度分核由独立模块 arkrtos_smp_sched.c 承担（开关
*          ARKRTOS_USE_SMP_SCHED，默认关）：次核有自己的运行队列，任务
*          可显式迁移过去再迁回。**全局调度器仍是单核的**——本文件不参与
*          调度，只负责把次核拉起来并交棒给那个模块。
*
*          **不在这里做**：对称调度（自动负载均衡）、全局时钟同步、
*          对象表的每核分区。这些都需要把全局调度器拆成每核一份，属于
*          另一件事；本文件不假装完成，也不给"看起来能并行"的接口。
*
* @note 开关默认 0：关档下本文件是一个空翻译单元，不产生任何符号与代码。
*/

#include "arkrtos_smp.h"

#if (ARKRTOS_USE_SMP == 1)

#include "arkrtos_hal.h"
#if (ARKRTOS_USE_SMP_SCHED == 1)
#include "arkrtos_smp_sched.h"
#endif

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

/* 每核栈：x 号核用第 x 块（0 号块不用，留给引导核的既有栈）。
 * arkrtos_context.S 的 arkrtos_smp_secondary_entry 按 CSR.TID 算栈顶，
 * 所以大小必须与那边用的是同一个宏（板级配置里的
 * ARKRTOS_SMP_SEC_STACK_SIZE）。放 .bss：不占镜像体积。 */
uint8 arkrtos_smp_stacks[ARKRTOS_CPU_NUM][ARKRTOS_SMP_SEC_STACK_SIZE]
    __attribute__((aligned(16)));

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
    p_cpu->ipi_back        = 0u;
    p_cpu->ipi_back_req    = 0u;
    p_cpu->pg_on           = 0u;
    p_cpu->pg_rc           = 0xFFFFFFFFu;   /* 还没报：不是「成功」也不是「失败」 */
    p_cpu->pg_look         = 0u;            /* 还没做读回 ⇒ 不是「通过」        */
    p_cpu->pg_hit          = 0u;
    p_cpu->pg_ppn          = 0u;
    p_cpu->pg_ps           = 0u;
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
    uint32 self;
    int32  accepted;

    for(i = 0u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        smp_cpu_clear(&arkrtos_smp_cpu[i]);
    }

    /* 调用者就是"本核"，它已经在跑，不需要"启动"。
     * 核号必须问 port：LA64 上 CPU0 是 CSR.TID==0，但真硬件上启动核不一定
     * 是 0；写死 0 会让后面的掩码/状态表整体错位一格。 */
    self = arkrtos_port_cpu_id();
    if(self >= (uint32)ARKRTOS_CPU_NUM)
    {
        self = 0u;                        /* 读错时收敛到 0，不越界 */
    }
    arkrtos_smp_cpu[self].started = 1u;
    arkrtos_smp_cpu[self].online  = 1u;
    arkrtos_smp_online_mask       = 0u;  /* 重复初始化时旧掩码必须清掉 */
    smp_mask_set(self);
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

#if ((ARKRTOS_USE_PAGING == 1) && (ARKRTOS_USE_PRIV == 1))
    /* R2：打开本核的分页。顺序是硬要求 —— 必须在 arkrtos_port_enable_irq()
     * **之前**：写 CSR 与按快照重放条目之间不能被节拍打断，否则节拍的
     * 取指/访存会落在一半有映射一半没有的窗口里。
     *
     * 返回码如实记账，不美化：pg_on 只有「真的打开了」才置 1。
     * 打开失败不在这里停机 —— 次核此时还在直译模式（DA=1）下跑，空转
     * 循环不需要分页，停机只会把「次核没开分页」伪装成「次核没起来」。
     * 失败由主核在自检里读 pg_rc 报 FAIL。 */
    {
        int32 pg_rc = arkrtos_port_paging_enable_secondary();

        arkrtos_smp_cpu[cpu].pg_rc = (uint32)pg_rc;
        arkrtos_smp_cpu[cpu].pg_on = (pg_rc == 0) ? 1u : 0u;

        /* 调用方自己再核一遍：port 层读回的是它自己的快照口径，这里用一个
         * **属于内核 .bss 的地址**独立验一次恒等映射。两层都过才算通过。
         *
         * 不论打开成功与否都读回一次，原始读数照抄进状态块：失败时这组数
         * 就是判因依据 —— hit=0 说明条目根本没进 TLB；hit=1 而 ppn/ps 对
         * 不上说明条目进去了但翻译不是恒等。读数本身不掺判词，判词在
         * pg_look 里。 */
        {
            unsigned long va = (unsigned long)&arkrtos_smp_cpu[cpu];
            unsigned long mask = ~0xFFFUL;
            uint32 index = 0u;
            uint32 ppn = 0u;
            uint32 ps = 0u;
            uint32 hit;

            hit = arkrtos_port_paging_lookup(va, &index, &ppn, &ps);
            arkrtos_smp_cpu[cpu].pg_hit = hit;
            arkrtos_smp_cpu[cpu].pg_ppn = ppn;
            arkrtos_smp_cpu[cpu].pg_ps = ps;

            if((hit != 0u) && (ps <= 30u))
            {
                /* 与 port 层 la_pg_identity_ok() 同一个口径，理由见那里：
                 *   1) PPN 是页基址，不是 va；
                 *   2) lookup 只读 TLBELO0（偶页），va 落在奇页时要给
                 *      PPN 加一个页大小 —— 内核 .bss 有 4.9 MB，跨过了
                 *      4 MB 边界，这条地址正好落在奇页里。 */
                mask = ~((1UL << ps) - 1UL);
                arkrtos_smp_cpu[cpu].pg_look =
                    (((unsigned long)ppn + (va & (1UL << ps))) == (va & mask))
                        ? 1u : 0u;
            }
            else
            {
                arkrtos_smp_cpu[cpu].pg_look = 0u;
            }
        }
    }
#endif

    /* LA64 的 arkrtos_smp_secondary_entry 进 C 之前已经开过 CRMD.IE
     * （那里不能过 arkrtos_port_enable_irq()：它的 s_in_trap 是全局变量，
     * 主核正在 trap 时会被静默跳过）。这里再调一次是幂等的，留给将来
     * 别的 port 复用。 */
    arkrtos_port_enable_irq();

    #if (ARKRTOS_USE_SMP_SCHED == 1)
    /* 调度分核：进本核的调度域（起本核节拍，然后在 WFI 里等任务被迁过来）。
     * 全局调度器仍然只归 CPU0；次核跑的是 arkrtos_smp_sched 的每核队列。 */
    arkrtos_smp_sched_enter(cpu);
    #else
    /* 次核目前只空转：调度器还是单核的，这里没有可跑的任务队列。 */
    for(;;)
    {
        arkrtos_smp_idle_loop();
    }
    #endif
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

int32 arkrtos_smp_ping_main(uint32 cpu)
{
    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        return -1;                    /* 自己不需要回 ping；越界不落地 */
    }

    /* 先登记再发：反过来的话次核可能在中断里先到，看到 req==0 就不回，
     * 主核白等一个上限。ip_back_req 是普通 uint32 store，次核在中断里
     * 读它——这里是「先写后发中断」，中间那条 IPI 写自带同步语义。 */
    arkrtos_smp_cpu[cpu].ipi_back_req = 1u;

    if(arkrtos_smp_send_ipi(cpu) != 0)
    {
        arkrtos_smp_cpu[cpu].ipi_back_req = 0u;
        return -1;                    /* 没上线：把登记的请求撤掉，不留哑弹 */
    }

    return 0;
}

ARKRTOS_SMP_WEAK void arkrtos_smp_ipi_handler(void)
{
    uint32 cpu = arkrtos_port_cpu_id();

    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        cpu = 0u;                     /* 核号读错时收敛到 0，与 cpu_self 同口径 */
    }

    arkrtos_smp_cpu[cpu].ipi_count++;

    /* R1 的双向读数：主核登记了「下一个 IPI 顺手回一个」时，次核在这里
     * 回一个给 CPU0。只在次核上做（cpu != 0），并且回完就清登记——
     * 一次登记换一次回执，不做周期性心跳（那会把计数变成时间函数，
     * 读数就不再是「这条路径通不通」的证据）。 */
    if((cpu != 0u) && (arkrtos_smp_cpu[cpu].ipi_back_req != 0u))
    {
        arkrtos_smp_cpu[cpu].ipi_back_req = 0u;
        if(arkrtos_smp_send_ipi(0u) == 0)
        {
            arkrtos_smp_cpu[cpu].ipi_back++;
        }
    }
}

void arkrtos_smp_idle_loop(void)
{
    arkrtos_smp_cpu_self()->idle_count++;
    arkrtos_port_wfi();
}

#endif /* ARKRTOS_USE_SMP == 1 */
