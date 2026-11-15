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
*          ## 已经知道的边界（不得当成「已有隔离」对外表述）
*
*          1) 只跑 M 模式，没有 U 模式 / PMP，任务之间没有内存隔离；
*          2) 异常处理程序与被中断的任务共用同一个栈（没有独立 handler 栈）；
*          3) trap 不可嵌套：进 trap 时 mstatus.MIE 已被硬件清零，处理程序
*             全程关中断；
*          4) `ecall` 之后 mepc 由本层加 4（硬件不会自己前进，不加上就是死循环）。
*
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

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

/* mie 位：MSIE = bit3（软件中断），MTIE = bit7（机器定时器） */
#define RV_MIE_MSIE         (1u << 3)
#define RV_MIE_MTIE         (1u << 7)

/* mcause 编码 */
#define RV_MCAUSE_IRQ_FLAG  (0x80000000u)
#define RV_MCAUSE_ECALL_M   (11u)
#define RV_MCAUSE_IRQ_TMR   (7u)
#define RV_MCAUSE_IRQ_SFT   (3u)

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

/* 外部符号：异常入口与两个汇编入口 */
extern void trap_entry(void);
extern void arkrtos_port_resume_task(uint32 stack_ptr);
extern void arkrtos_port_enter_idle(uint32 stack_ptr, uint32 use_priv);

/* 板级可覆盖的故障报告（默认什么都不打，见文件末尾） */
void arkrtos_port_fatal_report(uint32 mcause);

/* ============================================================
 * CSR 小工具
 * ============================================================ */
static uint32 rv_mstatus_read(void)
{
    uint32 v;

    __asm__ volatile("csrr %0, mstatus" : "=r"(v) : : "memory");
    return v;
}

static uint32 rv_mcause_read(void)
{
    uint32 v;

    __asm__ volatile("csrr %0, mcause" : "=r"(v) : : "memory");
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
    return 0u;
}

void arkrtos_port_spin_hint(void)
{
    __asm__ volatile("nop" ::: "memory");
}

#if (ARKRTOS_USE_SMP == 1)
/* 单 hart 移植：没有第二核可启动，也没有跨核 IPI 通路。 */
int32 arkrtos_port_cpu_start(uint32 cpu)
{
    (void)cpu;
    return -1;
}

int32 arkrtos_port_send_ipi(uint32 cpu)
{
    (void)cpu;
    return -1;
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

void arkrtos_port_disable_irq(void)
{
    __asm__ volatile("csrci mstatus, 8" ::: "memory");
}

void arkrtos_port_enable_irq(void)
{
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
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
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

uint32 arkrtos_port_get_control(void)
{
    return 0u;
}

void arkrtos_port_set_control(uint32 val)
{
    (void)val;
}

void arkrtos_port_set_thread_priv(uint32 is_priv)
{
    (void)is_priv;
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

/* 没有周期级计数器（DWT 的对应物）。返回 -1 是「这个核给不了这个读数」，
 * 调用方必须按「不可用」上报，不得编一个粗三个数量级的分辨率出来。 */
int32 arkrtos_port_cyccnt_init(void)
{
    return -1;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    return 0u;
}

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
    rv_mtimecmp_write(rv_mtime_read() + counts);
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

    s_in_trap++;

    mcause = rv_mcause_read();

    if((mcause & RV_MCAUSE_IRQ_FLAG) != 0u)
    {
        uint32 cause = (mcause & ~RV_MCAUSE_IRQ_FLAG);

        if(cause == RV_MCAUSE_IRQ_TMR)
        {
            rv_mtimecmp_write(rv_mtime_read() + s_tick_counts);
            arkrtos_kernel_tick_handler();
        }
        else if(cause == RV_MCAUSE_IRQ_SFT)
        {
            int32 next;
            uint32 next_sp;

            RV_CLINT_MSIP = 0u;

            /* 与 PendSV_Handler 完全同一套判定：>= 0 才切（0 = 切到 idle）。 */
            next = arkrtos_sched_is_switching();
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
        if(mcause == RV_MCAUSE_ECALL_M)
        {
            /* 硬件不会让 mepc 前进，ecall 指令占 4 字节（RV32 无压缩形式）。 */
            frame[RV_FR_MEPC] += 4u;
            SVC_Server((void *)frame);
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
     * 内核数据结构已初始化，在 trap 里改调度状态才是安全的。 */
    __asm__ volatile("csrs mie, %0" : : "r"((uint32)RV_MIE_MSIE) : "memory");

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
