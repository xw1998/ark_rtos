/**
* @file arkrtos_port.c
* @brief Ark RTOS RISC-V RV32IMAC 移植层实现
* @details 实现 arkrtos_hal.h 声明的全部硬件抽象接口。目标平台是 QEMU 的
*          `virt` 机器（单 hart、M 模式、CLINT + 16550 UART），但本文件里
*          只有「内核移交点」与 CLINT 地址是平台相关的，其余都是 RISC-V
*          特权架构本身的约定，换到真实的 RV32 芯片时只需要改板级头。
*
*          ## 与 Cortex-M 移植的对应关系
*
*          | Cortex-M              | 本移植                                  |
*          |-----------------------|-----------------------------------------|
*          | SVC 异常              | `ecall`（mcause = 11），号经 a7 传递    |
*          | PendSV 切换           | CLINT 软件中断（mcause = 0x80000003）   |
*          | SysTick 节拍          | CLINT 机器定时器（0x80000007）          |
*          | 硬件自动压栈异常帧    | trap_entry 手写 144 字节帧（见 .S 文件）|
*          | PRIMASK               | mstatus.MIE                             |
*          | IPSR != 0 判在中断里  | 软件计数 s_in_trap                      |
*
*          ## 任务隔离（U 模式 + PMP）
*
*          `ARKRTOS_USE_MPU=1` 且 `ARKRTOS_USE_PRIV=1` 时，App 任务跑在
*          **U 模式**，由 PMP 给出「代码窗口 RX / 数据窗口 RW / 共享区 R /
*          外设窗口」，越权访问触发 access fault 后走内核的
*          arkrtos_cpu_fault_handler()（记录 MEMFAULT 并重启/禁用任务）。
*          M 模式不受 PMP 约束（entry 的 L 位保持 0），所以内核照常访问一切。
*
*          特权级的传递方式是**帧内的 mstatus.MPP**：硬件进 trap 时会把
*          来源特权写进 MPP，本层在每次 mret 前用 g_rv_next_mpp 覆盖它。
*          g_rv_next_mpp 默认跟随「本次 trap 的来源」，只有
*          arkrtos_port_set_thread_priv() 在任务切换时改它 —— 这样一次
*          不切换的 tick 中断不会把 U 模式任务意外抬回 M 模式。
*
*          ## 已经知道的边界（不得当成「已有隔离」对外表述）
*
*          1) 异常处理程序与被中断的任务共用同一个栈（没有独立 handler 栈）；
*          2) trap 不可嵌套：进 trap 时 mstatus.MIE 已被硬件清零，处理程序
*             全程关中断。内核里有大量 ARKRTOS_ENABLE_IRQ() 位于可能从 trap
*             里被调到的函数，所以 arkrtos_port_enable_irq() 在 s_in_trap
*             非零时直接忽略请求；riscv_trap_c() 另有一道嵌套保护。
*             这一条不是“风格”，是实测踩过的缺陷：放开它 U 模式任务会在
*             一次故障重启后静默跑成 M 模式（详见 enable_irq 的注释）；
*          3) `ecall` 之后 mepc 由本层加 4（硬件不会自己前进，不加上就是死循环）；
*          4) PMP 只有 R/W/X 三个权限位，**没有可缓存性/共享性属性**：
*             内存类型（Normal / Device）在该目标上无法按任务区分。
*
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"
#include "arkrtos_fault.h"   /* ARKRTOS_FAULT_MEMFAULT */

#if (ARKRTOS_USE_SMP == 1)
#include "arkrtos_smp.h"     /* arkrtos_smp_ipi_handler() */
#endif

#if (ARKRTOS_USE_RTSTAT == 1)
#include "arkrtos_rt.h"
#endif

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG
#endif

/* ============================================================
 * CLINT（核心本地中断器）
 * @brief QEMU virt 的 CLINT 固定落在 0x02000000；mtimecmp/mtime 的偏移由
 *        RISC-V 特权规范固定，所有 CLINT 实现都一样，只有基址需要板级给。
 *        hart0：msip @ +0x0000，mtimecmp @ +0x4000，mtime @ +0xBFF8。
 * ============================================================ */
#ifndef ARKRTOS_PORT_CLINT_BASE
#define ARKRTOS_PORT_CLINT_BASE     0x02000000u
#endif

#define RV_CLINT_MSIP        (*(volatile uint32 *)(ARKRTOS_PORT_CLINT_BASE + 0x0000u))
#define RV_CLINT_MTIMECMP_LO (*(volatile uint32 *)(ARKRTOS_PORT_CLINT_BASE + 0x4000u))
#define RV_CLINT_MTIMECMP_HI (*(volatile uint32 *)(ARKRTOS_PORT_CLINT_BASE + 0x4004u))

/* 每 hart 的 msip / mtimecmp 取址（ACLINT = 经典 CLINT 的偏移）：
 *   msip[cpu]       = base + 4  * cpu
 *   mtimecmp[cpu]   = base + 0x4000 + 8 * cpu
 * cpu 就是 mhartid。QEMU 9.2 的 virt 用 aclint 设备，偏移与 CLINT 一致
 * （hw/intc/riscv_aclint.c，实测 -smp 2 下 msip[1] 能拉起次核）。
 * 单 hart 档 hart0 的两个地址与上面的 RV_CLINT_MSIP / RV_CLINT_MTIMECMP_LO
 * 逐字节相同，所以默认镜像不受影响。 */
#define RV_CLINT_MSIP_ADDR(cpu)         (ARKRTOS_PORT_CLINT_BASE + 4u * (uint32)(cpu))
#define RV_CLINT_MTIMECMP_LO_ADDR(cpu)  (ARKRTOS_PORT_CLINT_BASE + 0x4000u + 8u * (uint32)(cpu))

#if (ARKRTOS_USE_SMP == 1)
/* 写某个 hart 的 msip：1 = 给它挂一个机器软件中断，0 = 清掉。
 * 这是 RISC-V 里唯一一条跨 hart 的「敲门」通路（IPI 与 PendSV 共用）。
 * 关档下不编进来：单 hart 的 msip 由 RV_CLINT_MSIP 宏直接读写，
 * 留着它会多一条 -Wunused-function。 */
static void rv_clint_msip_write(uint32 cpu, uint32 val)
{
    *(volatile uint32 *)RV_CLINT_MSIP_ADDR(cpu) = val;
}
#endif /* ARKRTOS_USE_SMP == 1 */
#define RV_CLINT_MTIME_LO    (*(volatile uint32 *)(ARKRTOS_PORT_CLINT_BASE + 0xBFF8u))
#define RV_CLINT_MTIME_HI    (*(volatile uint32 *)(ARKRTOS_PORT_CLINT_BASE + 0xBFFCu))

/* 节拍时基（mtime 的计数频率）。QEMU virt 的 timebase-frequency 是 10 MHz，
 * 与 16550 的波特率生成无关；换频率只需板级改这一个宏。 */
#ifndef ARKRTOS_MACHINE_TIMER_HZ
#define ARKRTOS_MACHINE_TIMER_HZ    10000000u
#endif

/* ============================================================
 * trap 帧索引（与 arkrtos_context.S 的 144 字节布局一一对应）
 * ============================================================ */
#define RV_FR_RA            (0u)
#define RV_FR_SP            (1u)
#define RV_FR_GP            (2u)
#define RV_FR_TP            (3u)
#define RV_FR_T0            (4u)
#define RV_FR_A0            (9u)     /* 偏移 36 */
#define RV_FR_A7            (16u)    /* 偏移 64 */
#define RV_FR_MEPC          (31u)    /* 偏移 124 */
#define RV_FR_MSTATUS       (32u)    /* 偏移 128 */
#define RV_FR_WORDS         (36u)    /* 144 字节 */
#define RV_FR_SIZE          (144u)

/* mstatus / mie 相关位 */
#define RV_MSTATUS_MIE      (1u << 3)
#define RV_MSTATUS_MPIE     (1u << 7)
#define RV_MSTATUS_MPP_M    (3u << 11)
/* 任务首次运行时 mret 前的 mstatus：MPIE=1（mret 后 MIE=1）、MPP=3（M 模式） */
#define RV_MSTATUS_INIT     (RV_MSTATUS_MPIE | RV_MSTATUS_MPP_M)

/* mstatus.MPP 的位偏移（bit12:11）：mret 就是回到这个特权级 */
#define RV_MSTATUS_MPP_SHIFT (11u)
#define RV_PRIV_U           (0u)
#define RV_PRIV_M           (3u)

/* mie 位：MSIE = bit3（软件中断），MTIE = bit7（机器定时器） */
#define RV_MIE_MSIE         (1u << 3)
#define RV_MIE_MTIE         (1u << 7)
#define RV_MIE_MEIE         (1u << 11)  /* machine external：PLIC */

/* mcause 编码 */
#define RV_MCAUSE_IRQ_FLAG  (0x80000000u)
#define RV_MCAUSE_ILLEGAL     (2u)   /* 非法指令：U 模式执行 CSR 特杈指令走这里 */
#define RV_MCAUSE_INSN_FAULT  (1u)   /* 取指访问异常：PMP 拒绝执行 */
#define RV_MCAUSE_LOAD_FAULT  (5u)   /* 取数访问异常：PMP 拒绝读 */
#define RV_MCAUSE_STORE_FAULT (7u)   /* 存数访问异常：PMP 拒绝写 */
#define RV_MCAUSE_ECALL_U   (8u)
#define RV_MCAUSE_ECALL_M   (11u)
#define RV_MCAUSE_IRQ_TMR   (7u)
#define RV_MCAUSE_IRQ_SFT   (3u)
#define RV_MCAUSE_IRQ_EXT   (11u)  /* machine external：走 PLIC */

/* ============================================================
 * 内部状态
 * ============================================================ */

/* 节拍定时器的计数周期。供 riscv_trap_c() 重装 mtimecmp 用：
 * mtimecmp 是「比到就触发」的比较值，不像 SysTick 会自动重装。 */
static uint32 s_tick_counts = 1u;

/* 是否已经在异常处理程序内部。RISC-V 进 trap 后没地方读「我在哪个模式」，
 * 内核（arkrtos_sync_in_handler 的调用方）需要这个判定决定走注册还是让出。 */
static volatile uint32 s_in_trap = 0u;

/* 任务切换门闩：enter_idle 之前不切（与 M4 移植同一语义，理由见那边注释）。 */
static uint8 s_switch_ready = 0u;

/* 下一次 mret 要回到的特权级（0=U，3=M）。**必须是非 static 的全局**：
 * trap_entry / arkrtos_port_resume_task 的恢复半段要在汇编里读它。 */
volatile uint32 g_rv_next_mpp = RV_PRIV_M;

/* ecall 来源统计：U 模式的 ecall 是 mcause=8，M 模式是 11。
 * 两个计数分开记，是为了让「这个任务真的跑在 U 模式」变成一个可读的数字，
 * 而不是靠"没有崩"来推断。SVC 号本身不区分来源，所以只能在这里分。 */
volatile uint32 g_rv_ecall_u = 0u;
volatile uint32 g_rv_ecall_m = 0u;

/* 外部符号：异常入口与两个汇编入口 */
extern void trap_entry(void);
extern void arkrtos_port_resume_task(uint32 stack_ptr);
extern void arkrtos_port_enter_idle(uint32 stack_ptr, uint32 use_priv);

/* 板级可覆盖的故障报告（默认什么都不打，见文件末尾） */
void arkrtos_port_fatal_report(uint32 mcause);
void arkrtos_port_fault_detail(uint32 mcause, uint32 mtval, uint32 from_priv);
/* 故障路径的独立栈：asm 层的 rv_fault_enter 会把它装进 sp。
 * 大小可在板级配置里覆盖（默认 1 KB，故障路径只做「报告 + 重建 + 切走」）。 */
#ifndef ARKRTOS_RV32_FAULT_STACK_WORDS
#define ARKRTOS_RV32_FAULT_STACK_WORDS   256u
#endif

/* 故障栈本体 + 它的栈顶指针。栈顶以「常量指针」导出，asm 和 C 两端
 * 用的是同一个表达式，不会出现两边各自写一份大小而漂移的情况。 */
uint32 rv_fault_stack[ARKRTOS_RV32_FAULT_STACK_WORDS] __attribute__((aligned(16)));
uint32 * const rv_fault_stack_top_addr = &rv_fault_stack[ARKRTOS_RV32_FAULT_STACK_WORDS];

void rv_fault_enter(uint32 mcause, uint32 from_priv);
void arkrtos_port_fault_dispatch(uint32 mcause, uint32 from_priv);
uint32 arkrtos_cpu_fault_handler(uint32 fault_type);

/* ============================================================
 * CSR 小工具
 * ============================================================ */
static uint32 rv_mcause_read(void)
{
    uint32 v;

    __asm__ volatile("csrr %0, mcause" : "=r"(v) : : "memory");
    return v;
}

/* mtval：访问异常发生时给出错地址（QEMU 会填）。 */
static uint32 rv_mtval_read(void)
{
    uint32 v;

    __asm__ volatile("csrr %0, mtval" : "=r"(v) : : "memory");
    return v;
}

/* mtime 是 64 位；32 位 hart 用 hi-lo-hi 重读法拿一个一致的低 32 位。
 * 只有低 32 位就够：单次比较窗口最长也就几个 tick（s_tick_counts 远小于 2^32）。 */
static uint32 rv_mtime_read(void)
{
    uint32 hi;
    uint32 lo;
    uint32 hi2;

    do
    {
        hi  = RV_CLINT_MTIME_HI;
        lo  = RV_CLINT_MTIME_LO;
        hi2 = RV_CLINT_MTIME_HI;
    } while (hi != hi2);

    return lo;
}

/* 写 mtimecmp（64 位）：规范要求先写 hi=0xFFFFFFFF 让比较永不命中，
 * 再写 lo，最后写 hi，避免写入过程中 mtime 恰好穿过新比较值造成丢中断。 */
static void rv_mtimecmp_write(uint32 lo)
{
    RV_CLINT_MTIMECMP_HI = 0xFFFFFFFFu;
    RV_CLINT_MTIMECMP_LO = lo;
    RV_CLINT_MTIMECMP_HI = 0u;
}

/* ============================================================
 * PLIC 外部中断（machine external interrupt，mcause = 0x8000000B）
 * @details 定时器与软件中断都在 CLINT 里，外设（UART、虚拟磁盘等）走 PLIC。
 *          单 hart 单 context（hart0 的 M 模式 = context 0），所以只需要
 *          一套 enable / threshold / claim 寄存器。
 *
 *          布局（PLIC 规范，virt 机器一致）：
 *            +0x000000 + 4*id   优先级（id 1..1023，0 = 永不触发）
 *            +0x002000           context 0 的使能位图（每 id 1 bit）
 *            +0x200000           context 0 的抢占阈值
 *            +0x200004           context 0 的 claim/complete（读=claim，写=complete）
 * ============================================================ */
#ifndef ARKRTOS_PORT_PLIC_BASE
#define ARKRTOS_PORT_PLIC_BASE   0x0C000000u
#endif

#define RV_PLIC_PRIORITY(id)  (*(volatile uint32 *)(ARKRTOS_PORT_PLIC_BASE + 4u * (uint32)(id)))
#define RV_PLIC_ENABLE0       (*(volatile uint32 *)(ARKRTOS_PORT_PLIC_BASE + 0x2000u))
#define RV_PLIC_THRESHOLD0    (*(volatile uint32 *)(ARKRTOS_PORT_PLIC_BASE + 0x200000u))
#define RV_PLIC_CLAIM0        (*(volatile uint32 *)(ARKRTOS_PORT_PLIC_BASE + 0x200004u))

/* 一次 trap 里最多处理几个外部中断。UART 是电平触发：如果处理程序
 * 没把源摘干净，claim 会一直返回同一个 id，无上限的循环会把节拍饿死。
 * 上限存在的意义是「宁可晚一拍，不要丢掉 tick」。 */
#ifndef ARKRTOS_RV32_PLIC_CLAIM_MAX
#define ARKRTOS_RV32_PLIC_CLAIM_MAX   8u
#endif

/* 板级实现；没实现时只是计数丢掉的 irq，不假处理。 */
void arkrtos_port_external_isr(uint32 irq);
volatile uint32 g_rv_ext_irq_count;    /* 进过几次外部中断 trap */
volatile uint32 g_rv_ext_irq_unhandled;/* claim 到 0 的次数（正常应为 0） */

/**
* @brief 打开一个 PLIC 中断源（优先级 > 0 + 使能位 + 阈值放行）
* @param irq 中断号（virt 机器：UART0 = 10，UART1 = 11，VIRTIO = 1..8）
* @param prio 优先级 1..7，0 表示永久关闭
* @note 阈值固定为 0（= 只要优先级 > 0 就放行）。做优先级分组需要它，
*       但这套内核的中断优先级在 ARM 侧是 NVIC 的寄存器，语义不通用，
*       所以这里不假装支持抢占级。
*/
void arkrtos_port_plic_enable(uint32 irq, uint32 prio)
{
    if((irq == 0u) || (irq >= 1024u))
    {
        return;
    }

    RV_PLIC_PRIORITY(irq) = prio;
    RV_PLIC_ENABLE0 |= (1u << irq);
    RV_PLIC_THRESHOLD0 = 0u;
}

/**
* @brief 关闭一个 PLIC 中断源
*/
void arkrtos_port_plic_disable(uint32 irq)
{
    if((irq == 0u) || (irq >= 1024u))
    {
        return;
    }

    RV_PLIC_ENABLE0 &= ~(1u << irq);
    RV_PLIC_PRIORITY(irq) = 0u;
}

/**
* @brief 外部中断分发：claim -> 板级处理 -> complete
* @details 必须先 claim 拿到 id、处理完再 complete，否则同一源会一直
*          挂起（电平触发），trap 退不出去。
*/
static void rv_plic_dispatch(void)
{
    uint32 n;

    g_rv_ext_irq_count++;

    for(n = 0u; n < ARKRTOS_RV32_PLIC_CLAIM_MAX; n++)
    {
        uint32 irq = RV_PLIC_CLAIM0;

        if((irq == 0u) || (irq > 1023u))
        {
            g_rv_ext_irq_unhandled++;
            break;
        }

        arkrtos_port_external_isr(irq);

        /* complete：写回同一个 id 才算摘掉这一轮挂起 */
        RV_PLIC_CLAIM0 = irq;
    }
}

/*
* 微秒 -> 计数。
* @note 这里刻意不用 64 位乘除：目标工具链没有 rv32imac/ilp32 的 libgcc
*       多库（只有 rv32imafc / rv64），一旦生成 __udivdi3 / __muldi3 调用
*       就会在链接期找不到符号。时基一定是 MHz 整数倍，所以按「先约分
*       再相乘」的整数算式拆开即可，结果与 64 位算式逐位相同。
*/
static uint32 rv_us_to_counts(uint32 us)
{
    uint32 counts;

#if ((ARKRTOS_MACHINE_TIMER_HZ % 1000000u) == 0u)
    counts = us * (ARKRTOS_MACHINE_TIMER_HZ / 1000000u);
#elif ((ARKRTOS_MACHINE_TIMER_HZ % 1000u) == 0u)
    counts = ((us / 1000u) * (ARKRTOS_MACHINE_TIMER_HZ / 1000u))
           + (((us % 1000u) * (ARKRTOS_MACHINE_TIMER_HZ / 1000u)) / 1000u);
#else
    /* 时基既不是 MHz 也不是 kHz 整数倍：只能近似，误差不超过 1 个计数。 */
    counts = (us / 1000u) * (ARKRTOS_MACHINE_TIMER_HZ / 1000u);
#endif

    if(counts == 0u)
    {
        counts = 1u;
    }

    return counts;
}

/* ============================================================
 * CPU 控制
 * ============================================================ */
void arkrtos_port_wfi(void)  { __asm__ volatile("wfi"  ::: "memory"); }
void arkrtos_port_wfe(void)  { __asm__ volatile("wfi"  ::: "memory"); }   /* RISC-V 无 WFE，等价于等中断 */
void arkrtos_port_nop(void)  { __asm__ volatile("nop"  ::: "memory"); }
void arkrtos_port_isb(void)  { __asm__ volatile("fence.i" ::: "memory"); }
void arkrtos_port_dsb(void)  { __asm__ volatile("fence" ::: "memory"); }
void arkrtos_port_dmb(void)  { __asm__ volatile("fence" ::: "memory"); }

/* ============================================================
 * 原子操作与多核支持
 * ============================================================ */

/**
* @brief 比较并交换（LR/SC 实现）
* @details QEMU 的 rv32 带 A 扩展，`lr.w` / `sc.w` 是真原子指令，不需要
*          退化成「关中断近似」。单核下关中断也算正确，但这套写法在 SMP
*          上同样成立，将来换多 hart 时不用重写。
*/
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
{
    uint32 old;
    uint32 fail;

    do
    {
        __asm__ volatile("lr.w %0, (%1)"
                         : "=&r"(old)
                         : "r"(p_addr)
                         : "memory");

        if(old != expect)
        {
            /* 撤销保留：随便写一次 sc.w 并让它的 rd 丢弃结果即可。 */
            __asm__ volatile("sc.w zero, zero, (%0)" : : "r"(p_addr) : "memory");
            return 0u;
        }

        __asm__ volatile("sc.w %0, %2, (%1)"
                         : "=&r"(fail)
                         : "r"(p_addr), "r"(new_value)
                         : "memory");
    } while(fail != 0u);

    return 1u;
}

uint32 arkrtos_port_cpu_id(void)
{
#if (ARKRTOS_USE_SMP == 1)
    /* mhartid 是只读 CSR，不需要任何内存，次核一进来就能读。
     * 关档（默认）仍返回常量 0：默认镜像的指令不许因为这一条变化。 */
    uint32 hart;

    __asm__ volatile("csrr %0, mhartid" : "=r"(hart) : : "memory");
    return hart;
#else
    return 0u;
#endif
}

void arkrtos_port_spin_hint(void)
{
    __asm__ volatile("nop" ::: "memory");
}

#if (ARKRTOS_USE_SMP == 1)
/* ------------------------------------------------------------
 * SMP：次核启动与核间中断（RISC-V ACLINT / CLINT msip）
 *
 * @details 与 LA64 的 IOCSR mailbox 完全不同的一套协议，别混着读：
 *          QEMU `virt` 的 `-kernel` 直启会把**所有 hart 的复位 PC 都设成
 *          ELF 入口**（不同于 LoongArch，那里次核跑的是 pflash 固件）。
 *          所以次核会真的执行我们的 start.S，靠 mhartid 在入口处分叉：
 *          非 0 号 hart 进 start.S 的 park 循环，只用 CSR 轮询
 *          mip.MSIP（那一刻 .bss 可能还没被 CPU0 清完，碰内存就是竞态），
 *          等 CPU0 写 msip[cpu]=1 才换自己的栈、装 mtvec、开 MSIE 进 C。
 *
 *          因此「启动次核」= 一次 CLINT 写，不涉及 mailbox，也不涉及
 *          任何共享内存标志。
 * ------------------------------------------------------------ */
int32 arkrtos_port_cpu_start(uint32 cpu)
{
    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        return -1;
    }

    rv_clint_msip_write(cpu, 1u);
    return 0;
}

int32 arkrtos_port_send_ipi(uint32 cpu)
{
    if((cpu == 0u) || (cpu >= (uint32)ARKRTOS_CPU_NUM))
    {
        /* 核 0 的 msip 是 PendSV 的挂起位（arkrtos_port_switch_task 用它），
         * 拿它当 IPI 会把「切换」和「IPI」两个语义搅在一起，所以拒绝自投。 */
        return -1;
    }

    rv_clint_msip_write(cpu, 1u);
    return 0;
}

/**
* @brief 次核异常入口的 C 部分（由 arkrtos_context.S 的 rv_smp_trap_entry 调用）
* @details 次核只认一种来源：机器软件中断（msip = IPI）。其余原因一律按
*          致命异常处理并停机——次核没有可用的内核状态（共享的 s_in_trap、
*          g_rv_next_mpp、中断栈都是主核一份），把它放进完整 trap 分派
*          只会在别人的状态上乱写。
* @note 次核不开机器定时器中断（mie.MTIE 保持 0），所以不会有「次核 tick」。
*/
void rv_smp_trap_c(uint32 mcause)
{
    uint32 cpu = arkrtos_port_cpu_id();

    if(((mcause & RV_MCAUSE_IRQ_FLAG) != 0u) &&
       ((mcause & ~RV_MCAUSE_IRQ_FLAG) == RV_MCAUSE_IRQ_SFT))
    {
        rv_clint_msip_write(cpu, 0u);      /* 先清挂起，再计数 */
        arkrtos_smp_ipi_handler();
        return;
    }

    arkrtos_port_fatal_report(mcause);
    for(;;)
    {
        __asm__ volatile("wfi" ::: "memory");
    }
}
#endif /* ARKRTOS_USE_SMP == 1 */

/* ============================================================
 * 中断开关与异常上下文
 * ============================================================ */
uint8 arkrtos_port_in_isr(void)
{
    return (s_in_trap != 0u) ? 1u : 0u;
}

/**
* @brief 从 trap 帧里取系统调用号
* @details RISC-V 的 `ecall` 没有立即数字段（ARKRTOS_ARCH_SVC_NUM_BITS = -1），
*          号由约定寄存器携带：本移植用 a7，与 Linux/RISC-V 的习惯一致。
*/
uint32 arkrtos_port_syscall_num(void *p_exc_ctx)
{
    return ((uint32 *)p_exc_ctx)[RV_FR_A7];
}

uint32 arkrtos_port_svc_get_arg(void *p_exc_ctx, uint32 idx)
{
    if(idx >= ARKRTOS_ARCH_SVC_ARG_MAX)
    {
        return 0u;
    }

    return ((uint32 *)p_exc_ctx)[RV_FR_A0 + idx];
}

void arkrtos_port_svc_set_ret(void *p_exc_ctx, uint32 value)
{
    ((uint32 *)p_exc_ctx)[RV_FR_A0] = value;
}

#if (ARKRTOS_USE_LINUX_ABI == 1)
/**
* @brief Linux/RISC-V ABI 的系统调用号（a7）
* @details 本移植的 Ark 服务号本来就走 a7。两个 ABI 共用同一寄存器与同一条
*          `ecall`，只能按号段让位：0x10..0x22 是 Ark 自己的服务号
*          （ARKRTOS_SVC_LOG .. ARKRTOS_SVC_WAITMULTI_CTRL），永远归 Ark；
*          其余号交给 Linux 表，不认识的由兼容层回 -ENOSYS，不会静默当成功。
*/
uint32 arkrtos_port_linux_syscall_num(void *p_exc_ctx)
{
    uint32 num = ((uint32 *)p_exc_ctx)[RV_FR_A7];

    if((num >= 0x10u) && (num <= 0x22u))
    {
        return 0xFFFFFFFFu;
    }

    return num;
}

void arkrtos_port_svc_set_ret_signed(void *p_exc_ctx, int32 value)
{
    ((uint32 *)p_exc_ctx)[RV_FR_A0] = (uint32)value;
}
#endif

void arkrtos_port_disable_irq(void)
{
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
}

void arkrtos_port_enable_irq(void)
{
    /* 边界 2「trap 不可嵌套」的强制点：内核里有大量 ARKRTOS_ENABLE_IRQ()
     * 位于可能从 trap 里被调用到的函数（tick 处理、故障处理、loader 记账等）。
     * 在 M 模式里执行 csrsi mstatus,8 会真的把 MIE 打开，于是定时器立刻在
     * **处理程序内部**再进一次 trap：
     *   - 嵌套的 riscv_trap_c() 把 g_rv_next_mpp 改写成来源特权（M=3），
     *     外层恢复时任务就被静默抬回 M 模式；
     *   - 外层帧还在同一个栈上，嵌套帧压在其下方，恢复顺序也跟着错。
     * QEMU 上实测过这条：U 任务第 1 轮越权读 CLINT 正常 trap，重启后
     * ecall 变成 machine_ecall、CLINT 也能读了 —— 特权级就是这样丢的。
     *
     * 所以在这里吞掉 trap 内的开中断请求：返回用户上下文时 mstatus 从帧里
     * 恢复（MIE = 帧里的 MPIE = trap 前的值），中断照样是开着的，只是
     * 不会在处理程序中途被自己打断。 */
    if(s_in_trap != 0u)
    {
        return;
    }

    __asm__ volatile("csrsi mstatus, 8" ::: "memory");
}

/**
* @brief 进入临界区，返回进入前的 mstatus（只关心 MIE 位）
* @details 用 csrrc 一次完成「读旧值 + 清 MIE」，比先读后写少一条指令，
*          也不会被两条指令之间的中断打断。
*/
uint32 arkrtos_port_enter_critical(void)
{
    uint32 old;

    __asm__ volatile("csrrc %0, mstatus, %1"
                     : "=r"(old)
                     : "r"(RV_MSTATUS_MIE)
                     : "memory");

#if (ARKRTOS_USE_RTSTAT == 1)
    arkrtos_rt_crit_enter();
#endif

    return old;
}

void arkrtos_port_exit_critical(uint32 state)
{
#if (ARKRTOS_USE_RTSTAT == 1)
    arkrtos_rt_crit_exit();
#endif

    if((state & RV_MSTATUS_MIE) != 0u)
    {
        /* 走同一个门控：trap 内部的「退出临界区」同样不许真的开 MIE，
         * 否则又会把处理程序交给定时器打断（见 enable_irq 的注释）。 */
        arkrtos_port_enable_irq();
    }
}

/**
* @brief 触发一次任务切换
* @details 置 CLINT 软件中断挂起位。这是 RISC-V 里 PendSV 的对应物：
*          - 在中断上下文里调用：此刻 mstatus.MIE = 0，中断被挂起，
*            等 mret 之后立刻进 trap → 切换，不会在 tick 处理中途换栈；
*          - 在线程上下文里调用：立刻进 trap 完成切换。
*          两种情形都与内核「置挂起、稍后切」的假设一致。
*/
void arkrtos_port_switch_task(void)
{
    if(s_switch_ready != 0u)
    {
        RV_CLINT_MSIP = 1u;
    }
}

void arkrtos_port_switch_enable(void)
{
    s_switch_ready = 1u;
    RV_CLINT_MSIP = 1u;
}

/* ============================================================
 * 上下文层
 * ============================================================ */

/* RISC-V 移植没有 PSP/CONTROL 这组概念，也没有线程级特权切换
 * （不使用 U 模式）。这几个接口保留是为了满足 HAL 的完整声明。 */
void arkrtos_port_set_psp(uint32 val)
{
    (void)val;
}

#if (ARKRTOS_USE_TASK_SWAP == 1)
/* ---- 任务热切换的端口层（RV32IMAC） ---------------------------------
 * 帧格式见本文件的 RV_FR_*（144 字节 / 36 字），trap 入口把 tp 当帧指针。
 * 恢复路径是「riscv_trap_c 的返回值」——没有 PSP 可挪，所以 activate 把新帧
 * 记在 s_rv32_swap_frame 里，由 riscv_trap_c 在 SVC_Server 返回之后取走。
 *
 * 帧里必须改的四个位置：
 *   mepc    <- 新入口
 *   sp      <- 新块里的栈顶（不能留旧值：整块拷贝后 sp 槽里还是旧块地址）
 *   ra      <- 0（新 main 返回即跳 0，与新建任务同一口径）
 *   mstatus <- 只保留 MPP，置 MPIE（mret 用 MPIE 恢复 MIE）
 * a0..a7 清零；gp 保留（同一镜像内 gp 有效），其它 callee-saved 保留原值。
 * mstatus.MPP 真正生效还要过 g_rv_next_mpp（汇编恢复时覆盖），这里保留原值
 * 即「本次 trap 的来源特权」，切换与否由内核决定。
 * ------------------------------------------------------------------- */
static uint32 *s_rv32_swap_frame = 0;

int32 arkrtos_port_swap_frame_ok(const void *p_exc_ctx)
{
    const uint32 *p_frame = (const uint32 *)p_exc_ctx;
    uint32 mepc;
    uint32 mpp;

    if(p_frame == 0)
    {
        return 0;
    }
    mepc = p_frame[RV_FR_MEPC];
    mpp  = (p_frame[RV_FR_MSTATUS] >> RV_MSTATUS_MPP_SHIFT) & 3u;
    /* 对齐只要求 2 字节：本 port 编的是 RV32**IMAC**，C 扩展让指令可以
     * 落在 2 mod 4 的地址上——ecall 本身是 32 位，但它前面可以是一条
     * 16 位压缩指令。按 4 字节判会把这种建法的 ecall 帧判成"坏帧"，
     * 表现为「swap 命令随机 FAIL（v1 一直涨、v2 一直是 0）」，且只与
     * 链接后的指令落点有关（实测 mepc=0x8002B80E）。 */
    if((mepc == 0u) || ((mepc & 1u) != 0u))
    {
        return 0;
    }
    if((mpp != RV_PRIV_U) && (mpp != RV_PRIV_M))
    {
        return 0;
    }
    return 1;
}

uint32 arkrtos_port_swap_frame_bytes(void)
{
    return RV_FR_SIZE;
}

void arkrtos_port_swap_rewrite(void *p_exc_ctx, uint32 entry)
{
    uint32 *p_frame = (uint32 *)p_exc_ctx;
    uint32 i;

    for(i = RV_FR_A0; i <= RV_FR_A7; i++)
    {
        p_frame[i] = 0u;
    }

    p_frame[RV_FR_RA]  = 0u;
    p_frame[RV_FR_SP]  = (uint32)p_frame + RV_FR_SIZE;
    p_frame[RV_FR_MEPC] = entry & ~1u;   /* 同上：C 扩展下入口只需 2 字节对齐 */
    p_frame[RV_FR_MSTATUS] = (p_frame[RV_FR_MSTATUS] & RV_MSTATUS_MPP_M) |
                             RV_MSTATUS_MPIE;
}

void arkrtos_port_swap_activate(void *p_old_ctx, void *p_new_ctx)
{
    (void)p_old_ctx;
    s_rv32_swap_frame = (uint32 *)p_new_ctx;
}
#endif /* ARKRTOS_USE_TASK_SWAP == 1 */

uint32 arkrtos_port_get_control(void)
{
    return 0u;
}

void arkrtos_port_set_control(uint32 val)
{
    (void)val;
}

/**
* @brief 设定「下一次 mret 回到哪个特权级」
* @param is_priv 非 0 = 内核任务（M 模式），0 = App 任务（U 模式）
* @details 本函数在 trap 处理程序里被调用（切换路径），此刻 CPU 已在 M 模式，
*          所以改的是「即将恢复的任务」而不是自己。真正的落笔位置是汇编恢复
*          半段的 mstatus.MPP —— 见 arkrtos_context.S 的注释。
*          不在 C 里直接改帧的原因：arkrtos_sched_activate() 返回的帧指针
*          要到它返回之后才拿到，而本函数是在它内部被调用的。
*/
void arkrtos_port_set_thread_priv(uint32 is_priv)
{
    g_rv_next_mpp = (is_priv != 0u) ? RV_PRIV_M : RV_PRIV_U;
}

/**
* @brief 构造任务初始 trap 帧
* @param stack_top 栈顶（高地址）
* @param entry     任务入口
* @return 帧指针（存入 task->stack_ptr）
* @details 帧格式与 arkrtos_context.S 完全一致。任务第一次被恢复时会走
*          「trap_entry 的恢复半段」：mepc←entry、mstatus←0x1880、sp←stack_top，
*          然后 mret 跳进任务，此时 MIE 已经打开。
*          ra 置 0：任务函数返回就等于跳到 0 取指，会立刻进致命异常，
*          与 Cortex-M 版（LR=0）同一处理口径。
*          gp 取当前内核上下文里的值（start.S 设好了 __global_pointer$），
*          否则链接器的 gp 相对松弛在小任务里会读到错误地址。
*/
uint32 arkrtos_port_stack_init(uint32 stack_top, void (*entry)(void))
{
    uint32 *frame;
    uint32 gp;
    uint32 i;
    uint32 top;

    top   = stack_top & ~0xFu;
    frame = (uint32 *)(top - RV_FR_SIZE);

    for(i = 0u; i < RV_FR_WORDS; i++)
    {
        frame[i] = 0u;
    }

    __asm__ volatile("mv %0, gp" : "=r"(gp));

    frame[RV_FR_RA]      = 0u;
    frame[RV_FR_SP]      = top;
    frame[RV_FR_GP]      = gp;
    frame[RV_FR_TP]      = 0u;
    frame[RV_FR_MEPC]    = (uint32)entry;
    frame[RV_FR_MSTATUS] = RV_MSTATUS_INIT;

    return (uint32)frame;
}

/* ============================================================
 * 定时器层
 * ============================================================ */
uint32 arkrtos_port_get_system_clock(void)
{
    return ARKRTOS_MACHINE_TIMER_HZ;
}

uint32 arkrtos_port_get_timer_counter(void)
{
    return rv_mtime_read();
}

uint32 arkrtos_port_get_timer_reload(void)
{
    return 0xFFFFFFFFu;
}

/* ============================================================
 * 周期级时间基准（mcycle / mcycleh）
 * ------------------------------------------------------------
 * RISC-V 的 mcycle（CSR 0xB00）是自由运行的周期计数器，与 ARM 的 DWT
 * CYCCNT 是同一类东西。过去这里写着「没有周期级计数器」—— 那句话是错的：
 * 真缺的不是计数器，而是「计数器不等于校准过的时间基准」。mcycle 按核频
 * 走，mtime 按 10 MHz 走，两者不同源，所以频率只能实测。
 *
 * 做法：开机时用 mtime 当尺子，量 10 ms 里 mcycle 走了多少，得出每秒
 * 多少个周期。量不出来（mtime 不走 / mcycle 不走 / 结果荒唐）就返回 -1，
 * 让上层如实报「不可用」，不拿 10 MHz 冒充核频。
 *
 * RTSTAT 关闭时这一段不产生任何代码，与引入之前逐字节一致。
 * ============================================================ */
#if (ARKRTOS_USE_RTSTAT == 1)

static uint32 s_cycle_hz;              /* 实测的 mcycle 频率；0 = 未校准 */

static uint32 rv_mcycle_read(void)
{
    uint32 v;

    /* RV32 上 mcycle 是一条 32 位只读 CSR，高位在 mcycleh（0xB80）。
     * 单次校准窗口只有 10 ms，1 秒内不会回绕（> 4 GHz 才会），32 位够用。 */
    __asm__ volatile("csrr %0, 0xb00" : "=r"(v) : : "memory");
    return v;
}

int32 arkrtos_port_cyccnt_init(void)
{
    uint32 t0;
    uint32 c0;
    uint32 t1;
    uint32 span;
    uint32 elapsed;
    uint32 guard;
    uint32 dc;

    /* 频率在运行期不变：量到一次就够。重复校准只会把好数字换成第二次的
     * 噪声，而且后面每一拍都拿它折算，一旦变了读数就前后不可比。 */
    if(s_cycle_hz != 0u)
    {
        return 0;
    }

    /* 取 100 ms 的 mtime 计数当窗口，于是"每秒周期数 = 窗口内周期数 * 10"，
     * 整段不需要除法：RV32 工具链没有 libgcc 多库，64 位除法（__udivdi3）
     * 链接不过，而这个比值只要窗口准确就够了。窗口内的相对误差来自
     * "轮询越界"的那几条指令，实测 < 0.01%。 */
    span = ARKRTOS_MACHINE_TIMER_HZ / 10u;
    if(span == 0u)
    {
        span = 1u;
    }

    t0 = rv_mtime_read();
    c0 = rv_mcycle_read();
    guard = 0u;
    for(;;)
    {
        t1 = rv_mtime_read();
        elapsed = (uint32)(t1 - t0);
        if(elapsed >= span)
        {
            break;
        }
        /* mtime 不走 = 这个核没有可用时基：报错，不在这里死等。 */
        if(++guard > 20000000u)
        {
            return -1;
        }
    }

    dc = (uint32)(rv_mcycle_read() - c0);

    /* 0 = mcycle 没动（被 mcountinhibit 停了 / 核不实现）；超过 4e8 说明
     * 窗口内就走了 8 个参考时钟，对应 8 GHz 以上，不是本档的核。 */
    if((dc == 0u) || (dc > 400000000u))
    {
        return -1;
    }

    s_cycle_hz = dc * 10u;
    return 0;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    return rv_mcycle_read();
}

uint32 arkrtos_port_get_cycle_clock(void)
{
    return s_cycle_hz;
}

int32 arkrtos_port_cycle_clock_measured(void)
{
    return 1;
}

#else  /* RTSTAT == 0：保留原来的「不可用」桩，零体积 */

int32 arkrtos_port_cyccnt_init(void)
{
    return -1;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    return 0u;
}

#endif /* ARKRTOS_USE_RTSTAT */

#if (ARKRTOS_USE_TICKLESS == 1)
/* mtimecmp 是「比较值」，没有 COUNTFLAG：编程时留一个 mtime 快照，
 * 用 32 位差值算实际经过了多少计数（回绕也被减法吃掉）。 */
static uint32 s_sleep_start;

uint32 arkrtos_port_timer_sleep_max(void)
{
    /* mtime 是 64 位、这里是 32 位视图：取一半，避免"编程时还没睡完就已经
     * 跨过 2^31"这种分不清前后的情况。 */
    return 0x7FFFFFFFu;
}

uint32 arkrtos_port_timer_elapsed_counts(uint32 programmed)
{
    uint32 d = (uint32)(rv_mtime_read() - s_sleep_start);

    (void)programmed;
    return (d == 0u) ? 1u : d;
}
#endif /* ARKRTOS_USE_TICKLESS */

/**
* @brief 启动节拍定时器
* @param tick_period_us 节拍周期（微秒）
* @details 用 CLINT 的 mtimecmp 做「下一次比较」：每进一次定时器中断就
*          重装 mtime + s_tick_counts，硬件负不了这个责，由 riscv_trap_c 补。
*/
void arkrtos_port_start_timer(uint32 tick_period_us)
{
    s_tick_counts = rv_us_to_counts(tick_period_us);
    rv_mtimecmp_write(rv_mtime_read() + s_tick_counts);
    /* 节拍中断在这里才真正放行：irq_init() 只开全局中断位，不碰 MTIE。
     * 原因见 irq_init() 的注释（mtimecmp 复位值是 0，提前放行会立刻进 trap）。 */
    __asm__ volatile("csrs mie, %0" : : "r"((uint32)RV_MIE_MTIE) : "memory");
}

/* tickless 用：直接给计数，不做微秒换算（避免 n * TICK_PERIOD_US 丢相位）。 */
void arkrtos_port_start_timer_counts(uint32 counts)
{
    if(counts == 0u)
    {
        counts = 1u;
    }

    s_tick_counts = counts;
    {
        uint32 base = rv_mtime_read();

#if (ARKRTOS_USE_TICKLESS == 1)
        /* tickless 结账用的快照：必须与写进 mtimecmp 的是同一次读数。 */
        s_sleep_start = base;
#endif
        rv_mtimecmp_write(base + counts);
    }
    __asm__ volatile("csrs mie, %0" : : "r"((uint32)RV_MIE_MTIE) : "memory");
}

/* mtimecmp 没有「计到过 0」这样的粘滞标志：比较命中就触发中断，此位无从谈起。
 * 返回 0 让上层按「提前唤醒」处理（结果仍正确，只是少省一次睡）。 */
uint32 arkrtos_port_timer_countflag_take(void)
{
    return 0u;
}

void arkrtos_port_delay_us(uint32 us)
{
    uint32 start = rv_mtime_read();
    uint32 wait  = rv_us_to_counts(us);

    while((uint32)(rv_mtime_read() - start) < wait)
    {
        /* 忙等：这段时间刻意不睡，语义与 ARM 版一致 */
    }
}

/* ============================================================
 * 异常分发
 * ============================================================ */

/**
* @brief trap 帧的统一 C 入口
* @param frame 当前上下文保存的帧指针（trap_entry 里由 tp 传入）
* @return 要恢复的帧指针：发生任务切换时换成另一个任务的帧，否则原样返回
* @details 三条路径：
*          - 机器定时器中断（0x80000007）：重装 mtimecmp 后交给内核节拍处理；
*          - 机器软件中断（0x80000003）：清挂起位，做一次调度决策；
*          - 环境调用（11 = M 模式 ecall）：mepc + 4 后进 SVC_Server。
*          其余原因一律按致命异常处理（打印后停机，不做假恢复）。
*/
uint32 *riscv_trap_c(uint32 *frame)
{
    uint32 mcause;
    uint32 prev_mpp = g_rv_next_mpp;
    uint32 nested   = (s_in_trap != 0u) ? 1u : 0u;

    s_in_trap++;

    #if (ARKRTOS_USE_STACK_GUARD == 1)
    /* 本 port 的 trap 帧直接压在被中断任务的栈上：帧写下去之后立刻查一次
     * 栈底保护字，把「帧把栈吃穿」变成一条带任务号的记录，而不是稍后
     * 某个随机指针崩掉。这里只记录，不改调度状态（还在异常里）。 */
    arkrtos_stack_guard_check_current();
    #endif

    /* 默认回到「本次 trap 的来源特权级」：不切换时这就是任务自己的特权，
     * 切换时 arkrtos_port_set_thread_priv() 会把它改成新任务的值。
     * 少了这一句，一次不切换的 tick 中断会把 U 模式任务抬回 M 模式。 */
    g_rv_next_mpp = (frame[RV_FR_MSTATUS] >> RV_MSTATUS_MPP_SHIFT) & 3u;

    mcause = rv_mcause_read();

    if((mcause & RV_MCAUSE_IRQ_FLAG) != 0u)
    {
        uint32 cause = (mcause & ~RV_MCAUSE_IRQ_FLAG);

        if(cause == RV_MCAUSE_IRQ_TMR)
        {
            rv_mtimecmp_write(rv_mtime_read() + s_tick_counts);
            arkrtos_kernel_tick_handler();
        }
        else if(cause == RV_MCAUSE_IRQ_EXT)
        {
            /* 外部中断：PLIC claim -> 板级处理 -> complete。
             * 中断里不做任务切换判定：真正的切换由处理程序里调用的
             * *_from_isr() 置的软件中断在 mret 之后完成（与 Cortex-M
             * 的 PendSV 同一套时序），不在 trap 里换栈。 */
            rv_plic_dispatch();
        }
        else if(cause == RV_MCAUSE_IRQ_SFT)
        {
            int32 next;
            uint32 next_sp;

            RV_CLINT_MSIP = 0u;

            /* 与 PendSV_Handler 完全同一套判定：>= 0 才切（0 = 切到 idle）。
             * 嵌套时绝不切：外层帧还压在同一个栈上，换栈会把两层栈帧
             * 一起丢掉（第二道保险，主保险在 arkrtos_port_enable_irq）。 */
            next = (nested != 0u) ? ARKRTOS_TASK_NONE : arkrtos_sched_is_switching();
            if(next >= 0)
            {
                next_sp = arkrtos_sched_activate(next, (uint32)frame);
                if(next_sp != 0u)
                {
                    frame = (uint32 *)next_sp;
                }
            }
        }
        else
        {
            arkrtos_port_fatal_report(mcause);
            for(;;)
            {
                __asm__ volatile("wfi" ::: "memory");
            }
        }
    }
    else
    {
        if((mcause == RV_MCAUSE_ECALL_M) || (mcause == RV_MCAUSE_ECALL_U))
        {
            /* 硬件不会让 mepc 前进，ecall 指令占 4 字节（RV32 无压缩形式）。
             * M 模式与 U 模式的 ecall 走同一个内核入口：越权检查靠的是
             * PMP（U 模式根本碰不到内核数据），不是 SVC 号本身。 */
            if(mcause == RV_MCAUSE_ECALL_U)
            {
                g_rv_ecall_u++;
            }
            else
            {
                g_rv_ecall_m++;
            }
            frame[RV_FR_MEPC] += 4u;
            SVC_Server((void *)frame);
            #if (ARKRTOS_USE_TASK_SWAP == 1)
            /* 热切换：端口层把改写好的新帧记在 s_rv32_swap_frame 里。
             * 恢复帧是本函数的返回值，所以这里必须把它换掉，否则返回的
             * 还是旧块里那一帧（旧块马上就要被还回去了）。 */
            if(s_rv32_swap_frame != 0)
            {
                frame = s_rv32_swap_frame;
                s_rv32_swap_frame = 0;
            }
            #endif
        }
        else if((mcause == RV_MCAUSE_STORE_FAULT) ||
                (mcause == RV_MCAUSE_LOAD_FAULT)  ||
                (mcause == RV_MCAUSE_INSN_FAULT)  ||
                /* 非法指令只当「U 模式的越权」处理：U 模式执行 CSR 指令
                 * （arkrtos_port_disable_irq 的 csrrc 就算）会得到 mcause=2，
                 * 而不是 5/7 —— 特权级隔离本来就包含「不能改 CSR」这一条，
                 * 把它当成致命错误停机就把保护机制自己搞死了（实测：U 任务
                 * 一调内核内部函数就 FATAL: mcause=0x2）。M 模式碰到非法指令
                 * 仍然是真 bug，保持下面 else 的停机路径。 */
                (mcause == RV_MCAUSE_ILLEGAL))
        {
            /* PMP 拒绝访问 = 保护机制真的生效了。交给内核：App 任务会被记录
             * MEMFAULT 并重启/禁用，内核上下文则停机等调试器。
             *
             * 这里**必须先切到独立故障栈**再往下走，不能继续用在被中断任务
             * 的栈上（本移植没有独立 handler 栈，见文件头边界 1）：
             * 内核的故障恢复（arkrtos_task_recover_pending）会调用
             * arkrtos_task_stack_init() 把那个任务的整段栈重新填成
             * 0xcdcdcdcd 再重建入口帧 —— 而处理程序自己的活动栈帧正在
             * 那段区间里。填完一返回，栈上取回的 ra 就是 0xcdcdcdcd，
             * PC 直接跳进填充图案（QEMU 上实测：每轮 8 层
             * `fault_fetch epc=0xcdcdcdcc` 递归，并且叠到栈外去）。
             * 任务栈的顶部 144 字节在活动栈帧之上，重建入口帧本身是安全的，
             * 所以只需要把「执行处理程序的那段栈」搬走。 */
            {
                uint32 from_priv = (frame[RV_FR_MSTATUS] >> RV_MSTATUS_MPP_SHIFT) & 3u;

                /* 非法指令只在 U 模式算越权；M 模式自己吃到 mcause=2 是
                 * 移植层/内核的真 bug，不能拿“重启任务”掩掉。 */
                if((mcause == RV_MCAUSE_ILLEGAL) && (from_priv != RV_PRIV_U))
                {
                    arkrtos_port_fatal_report(mcause);
                    for(;;)
                    {
                        __asm__ volatile("wfi" ::: "memory");
                    }
                }

                rv_fault_enter(mcause, from_priv);
            }
            /* 不返回：rv_fault_enter 换栈后 tail 进 arkrtos_port_fault_dispatch */
        }
        else
        {
            arkrtos_port_fatal_report(mcause);
            for(;;)
            {
                __asm__ volatile("wfi" ::: "memory");
            }
        }
    }

    s_in_trap--;

    /* 真要发生嵌套（不该发生，见 enable_irq 的门控），外层恢复时必须用
     * 自己那一轮判定的特权级，不能被内层改写成来源特权。 */
    if(nested != 0u)
    {
        g_rv_next_mpp = prev_mpp;
    }

    return frame;
}

/* ============================================================
 * 板级初始化默认实现（board 层可覆盖）
 * ============================================================ */
ARKRTOS_WEAK void arkrtos_port_board_init(void)
{
    /* 无 FPU 需要使能，无时钟树需要配置：QEMU virt 起来就是这个状态。 */
}

/**
* @brief 安装异常入口并打开 M 模式中断
* @details 三件事：
*          1) mtvec = trap_entry（直接模式，基址 4 字节对齐）；
*          2) mie 打开 MSIE（软件中断，任务切换用）与 MTIE（定时器节拍）；
*          3) mstatus.MIE = 1 —— 相当于 Cortex-M 复位后 PRIMASK 为 0。
*              不在这里打开的话，switch_enable 置的软件中断永远不会被取走，
*              第一个任务也就永远启动不了。
*/
ARKRTOS_WEAK void arkrtos_port_irq_init(void)
{
    /* 向量表 */
    __asm__ volatile("csrw mtvec, %0" : : "r"((uint32)&trap_entry) : "memory");

    /* 清掉可能已经挂起的节拍比较：CLINT 的 mtimecmp 复位值是 0，而 mtime
     * 早已是几十亿，若此时 MTIE 已开就会在打开 mstatus.MIE 的那一瞬间
     * 立刻进 trap（实测症状：板级横幅之后一步都不打印，看起来像死机）。
     * 把比较值推到最大值 = "永不到期"，等 start_timer() 再装真值。 */
    rv_mtimecmp_write(0xFFFFFFFFu);

    /* 只放行软件中断（任务切换）。节拍中断由 start_timer() 放行 —— 那时
     * 内核数据结构已初始化，在 trap 里改调度状态才是安全的。
     * 外部中断（MEIE）也在这里放行：PLIC 的每个源本身默认是关的
     * （优先级 0 + 使能位 0），没源就不会进 trap；真正接上外设的是
     * 板级调的 arkrtos_port_plic_enable()。 */
    __asm__ volatile("csrs mie, %0" : : "r"((uint32)(RV_MIE_MSIE | RV_MIE_MEIE)) : "memory");

    /* 相当于 Cortex-M 复位后 PRIMASK = 0：不放这里，switch_enable() 置的
     * 软件中断永远取不走，第一个任务起不来。 */
    __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    __asm__ volatile("fence.i" ::: "memory");
}

ARKRTOS_WEAK void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    (void)task_func;
    (void)stack_addr;
    (void)stack_size;
}

/* ============================================================
 * PMP 内存保护（U 模式的隔离由它提供）
 * @details RISC-V 的 PMP 与本内核的 MPU 抽象一一对应：
 *          `arkrtos_arch_mpu_t.region_base[]` 存 pmpaddr，
 *          `region_attr[]` 存 pmpcfg 的一个字节。区域列表由 arkrtos_mpu.c
 *          按任务算好（代码 / 数据 / 共享区 / 两个外设窗 / 外部 RAM），
 *          本层只负责编码与写 CSR。
*
 *          **为什么 M 模式不受影响**：PMP entry 的 L（lock）位保持 0 时，
 *          M 模式在「没有匹配 entry」的情况下默认全部允许；U 模式没有匹配
 *          就是全部拒绝。所以给 U 模式编排权限不会把自己锁在外面，内核
 *          也仍然能访问所有内存与外设。
 *
 *          **为什么用 NAPOT**：区域大小在本内核里已经是 2 的幂
 *          （arkrtos_mpu.c 的 round_up），NAPOT 正好表达「对齐的 2^n 区间」；
 *          4 字节区域退回 NA4（NAPOT 只覆盖 >= 8 字节）。
 *
 *          **没有的属性**：PMP 只有 R/W/X 三个权限位，没有 ARM MPU 的
 *          XN/Device/可缓存性。所以「外设窗口」在本目标上只表达读写权限，
 *          不能阻止 U 模式做非对齐访问或写合并。
 * ============================================================ */

/* PMP 条目数：pmpcfg0/pmpcfg1 各管 4 条，共 8 条，与 ARKRTOS_MPU_REGION_MAX 对齐 */
#define RV_PMP_ENTRIES       (8u)

/* pmpcfg 字节位 */
#define RV_PMP_R             (1u << 0)
#define RV_PMP_W             (1u << 1)
#define RV_PMP_X             (1u << 2)
#define RV_PMP_A_MASK        (3u << 3)
#define RV_PMP_A_NA4         (2u << 3)
#define RV_PMP_A_NAPOT       (3u << 3)

static void rv_pmpaddr_write(uint32 idx, uint32 value)
{
    switch(idx)
    {
        case 0u: __asm__ volatile("csrw pmpaddr0, %0" : : "r"(value) : "memory"); break;
        case 1u: __asm__ volatile("csrw pmpaddr1, %0" : : "r"(value) : "memory"); break;
        case 2u: __asm__ volatile("csrw pmpaddr2, %0" : : "r"(value) : "memory"); break;
        case 3u: __asm__ volatile("csrw pmpaddr3, %0" : : "r"(value) : "memory"); break;
        case 4u: __asm__ volatile("csrw pmpaddr4, %0" : : "r"(value) : "memory"); break;
        case 5u: __asm__ volatile("csrw pmpaddr5, %0" : : "r"(value) : "memory"); break;
        case 6u: __asm__ volatile("csrw pmpaddr6, %0" : : "r"(value) : "memory"); break;
        case 7u: __asm__ volatile("csrw pmpaddr7, %0" : : "r"(value) : "memory"); break;
        default: break;
    }
}

/* 关掉全部 entry：U 模式随后对任何地址都没有权限（M 模式不受限）。 */
static void rv_pmp_disable_all(void)
{
    __asm__ volatile("csrw pmpcfg0, zero" ::: "memory");
    __asm__ volatile("csrw pmpcfg1, zero" ::: "memory");
}

/**
* @brief 把一个区域编码进任务上下文（不碰硬件）
* @details 只算 pmpaddr / pmpcfg 两个数。写 CSR 在 set_app() 里做，
*          因为切换时必须「先关后写」，中间态不能让 U 模式看到。
*/
int32 arkrtos_port_mpu_encode(arkrtos_arch_mpu_t *p_mpu, uint32 idx, uint32 base, uint32 size,
                            arkrtos_mpu_mem_t mem)
{
    uint32 attr;
    uint32 addr;
    uint32 pow2;

    if((p_mpu == 0) || (idx >= RV_PMP_ENTRIES) || (size == 0u))
    {
        return -1;
    }

    /* 区域大小必须是 2 的幂（arkrtos_mpu.c 已经 round up，这里只做保险），
     * 且至少 4 字节 —— PMP 的最小粒度就是 4 字节（NA4）。 */
    pow2 = 4u;
    while((pow2 < size) && (pow2 < 0x80000000u))
    {
        pow2 <<= 1u;
    }

    /* 基址向下对齐到区域大小：PMP 的区间必须自然对齐。 */
    base &= ~(pow2 - 1u);

    switch(mem)
    {
        case ARKRTOS_MPU_MEM_ROM:       attr = RV_PMP_R | RV_PMP_X; break;
        case ARKRTOS_MPU_MEM_RAM:       attr = RV_PMP_R | RV_PMP_W; break;
        case ARKRTOS_MPU_MEM_RAMX:      attr = RV_PMP_R | RV_PMP_W | RV_PMP_X; break;
        case ARKRTOS_MPU_MEM_PERIPH_RO: attr = RV_PMP_R; break;
        case ARKRTOS_MPU_MEM_PERIPH_RW: attr = RV_PMP_R | RV_PMP_W; break;
        case ARKRTOS_MPU_MEM_EXTRAM:    attr = RV_PMP_R | RV_PMP_W; break;
        default:                        return -1;
    }

    if(pow2 == 4u)
    {
        attr |= RV_PMP_A_NA4;
        addr  = base >> 2u;
    }
    else
    {
        /* NAPOT：pmpaddr 低位连续 1 的个数编码区间大小，size = 2^(k+3)。 */
        attr |= RV_PMP_A_NAPOT;
        addr  = (base >> 2u) | ((pow2 >> 3u) - 1u);
    }

    p_mpu->region_base[idx] = addr;
    p_mpu->region_attr[idx] = attr;

    return 0;
}

/**
* @brief 把一份任务上下文写进 PMP
* @details 顺序很关键：先把所有 entry 关掉，再写 pmpaddr，最后一次性写
*          pmpcfg。反过来的话，U 模式可能在「新地址 + 旧属性」的窗口里
*          访问到一块本来不该给它的内存。
*          传 0 表示「没有可给 U 模式的东西」——idle 任务与未配置的任务用它。
*/
void arkrtos_port_mpu_set_app(const arkrtos_arch_mpu_t *p_mpu)
{
    uint32 cfg0 = 0u;
    uint32 cfg1 = 0u;
    uint32 i;

    rv_pmp_disable_all();

    if(p_mpu == 0)
    {
        return;
    }

    for(i = 0u; i < RV_PMP_ENTRIES; i++)
    {
        uint32 a = p_mpu->region_attr[i];

        if((a & RV_PMP_A_MASK) == 0u)
        {
            continue;                       /* A=OFF：区域未启用 */
        }

        rv_pmpaddr_write(i, p_mpu->region_base[i]);

        if(i < 4u)
        {
            cfg0 |= (a & 0xFFu) << (8u * i);
        }
        else
        {
            cfg1 |= (a & 0xFFu) << (8u * (i - 4u));
        }
    }

    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(cfg0) : "memory");
    __asm__ volatile("csrw pmpcfg1, %0" : : "r"(cfg1) : "memory");
}

/* 启动时 PMP 是空的（U 模式什么都碰不到），不需要额外初始化。 */
void arkrtos_port_mpu_init(void)
{
    rv_pmp_disable_all();
}

/* 复位 = 退回「只有 M 模式」的状态：U 模式权限全部收走。 */
void arkrtos_port_mpu_reset(void)
{
    rv_pmp_disable_all();
}

/* 空闲任务是内核代码，跑在 M 模式：U 模式权限全部收走即可。 */
void arkrtos_port_mpu_set_idle(void)
{
    rv_pmp_disable_all();
}

/**
* @brief 捕获空闲任务的保护上下文（与 M4 移植同名接口的语义对齐）
* @details 本目标上空闲任务跑 M 模式，不需要任何 U 模式权限，所以这里
*          只接收参数不建 region。
*/
void arkrtos_port_mpu_set_region(uint32 rom_addr, uint32 rom_size, uint32 ram_addr, uint32 ram_size)
{
    (void)rom_addr;
    (void)rom_size;
    (void)ram_addr;
    (void)ram_size;
}

/* ============================================================
 * Flash 接口（QEMU 无片内 Flash，Load、App 装载均未启用）
 * ============================================================ */
ARKRTOS_WEAK int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    (void)addr;
    (void)size;
    return -1;
}

ARKRTOS_WEAK int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    (void)addr;
    (void)data;
    (void)len;
    return -1;
}

ARKRTOS_WEAK uint32 arkrtos_port_flash_sector_size(uint32 addr)
{
    (void)addr;
    return 0u;
}

ARKRTOS_WEAK int32 arkrtos_port_flash_protection(arkrtos_flash_prot_t *p_out)
{
    (void)p_out;
    return -1;
}

/* ============================================================
 * 看门狗（QEMU virt 没有 IWDG，明确报「没有」而不是假装喂了）
 * ============================================================ */
ARKRTOS_WEAK int32 arkrtos_port_wdg_init(uint32 timeout_ms)
{
    (void)timeout_ms;
    return -1;
}

ARKRTOS_WEAK void arkrtos_port_wdg_feed(void)
{
}

ARKRTOS_WEAK uint32 arkrtos_port_wdg_timeout_ms(void)
{
    return 0u;
}

/* ============================================================
 * 致命异常报告：默认静默停机，板级覆盖后打到串口
 * ============================================================ */
ARKRTOS_WEAK void arkrtos_port_fatal_report(uint32 mcause)
{
    (void)mcause;
}

/**
* @brief PMP 违规现场（mcause / mtval / 出错时的特权级）
* @details 默认什么都不做；板级覆盖后打到串口，就是「保护真的挡下来了」的
*          现场证据。单独一个钩子而不是复用 fatal_report，是因为访问异常是
*          可恢复的（App 任务会被记录并重启），fatal_report 的语义是停机。
*/
/*
* 外部中断的默认实现：什么都不做。
* @details 故意不在这里打印/不停机：PLIC 会继续挂起同一个源，但如果换成
*          「停机」就会把一个配错的中断号变成整个系统的死机。板级覆盖它，
*          并且必须把源摘干净（UART 要读干净 RX FIFO、清掉 LSR 错误位）。
*          没被覆盖时应该在第一次启用 PLIC 源之前就发现 —— plic_enable()
*          只在板级代码里调。
*/
ARKRTOS_WEAK void arkrtos_port_external_isr(uint32 irq)
{
    (void)irq;
}

ARKRTOS_WEAK void arkrtos_port_fault_detail(uint32 mcause, uint32 mtval, uint32 from_priv)
{
    (void)mcause;
    (void)mtval;
    (void)from_priv;
}

/* ============================================================
 * 故障派发（跑在独立故障栈上，不返回）
 * ============================================================ */
/**
* @brief 访问异常的实际处理体
* @param mcause    硬件原因号（1/5/7）
* @param from_priv 出事时的特权级（0=U，3=M）
* @details 入口是汇编的 rv_fault_enter：它先把 sp 换成 rv_fault_stack_top_addr，
*          再 tail 进来。换栈的理由只有一个但很硬：内核的故障恢复会重建
*          肇事任务的栈（整段填 0xcdcdcdcd），如果处理程序还在那段栈上跑，
*          重建一完成，处理程序自己取回的 ra 就是填充图案，返回就飞了。
*
*          本函数永不返回：要么装上下一个任务的上下文（resume_task），
*          要么停机等调试器。
*/
void arkrtos_port_fault_dispatch(uint32 mcause, uint32 from_priv)
{
    uint32 resume_sp;

    arkrtos_port_fault_detail(mcause, rv_mtval_read(), from_priv);

    resume_sp = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_MEMFAULT);

    if(resume_sp != 0u)
    {
        /* 不经过 trap_entry 的返回半段，直接把状态收拾干净再换上下文。 */
        s_in_trap = 0u;
        arkrtos_port_resume_task(resume_sp);   /* 不返回 */
    }

    arkrtos_port_fatal_report(mcause);
    for(;;)
    {
        __asm__ volatile("wfi" ::: "memory");
    }
}
