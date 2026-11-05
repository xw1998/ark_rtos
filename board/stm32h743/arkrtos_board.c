/**
* @brief Ark RTOS 板级移植层 - STM32H743
* @details 实现 port 层要求板级提供的基础配置：
*          - FPU 使能（CPACR + FPCCR）
*          - NVIC 优先级分组与内核异常优先级
*          - 板载设备注册（ttyS0 / led0）
*          - CPU 异常向量与系统节拍入口
*          其余 CPU 相关实现全部在 port/arm/cortex-m7/，
*          board/ 只承担芯片相关的差异。
*
*          本文件由 F427 版本派生，改动只有四处，逐条列出以便核对：
*            1. 头文件换成 stm32h7xx.h / stm32h7xx_hal.h；
*            2. 灯只剩一颗（PC13，低电平点亮），注册名仍是 "led0"；
*            3. 不再注册 led1/led2；
*            4. 块设备只有内部的 int0（QSPI NOR 驱动是 M2 的事）。
*          CPACR/FPCCR 的位、NVIC 优先级数值、SysTick 补 HAL 时基的算法
*          与 F427 完全一致：这些是 ARMv7-M 架构与内核节拍约定，
*          不是芯片差异，改了就是引入新的行为。
* @author xw
* @date 2026.11.04
*/

#include "stm32h7xx.h"
#include "stm32h7xx_hal.h"   /* HAL_IncTick(): 板级补 HAL 毫秒时基 */
#include "arkrtos_hal.h"
#include "arkrtos_dev.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"
#include "drvled.h"
#include "drvuart.h"
#include "drvblk.h"
#include "drvfmc.h"
#if (ARKRTOS_USE_SDRAM == 1)
#include "arkrtos_shell.h"        /* arkrtos_board_shell_register 的返回约定 */
#include "ark_shell_commands.h"   /* g_cmd_table / ARK_SHELL_CMD / 容量上限 */
#endif
#include "arkrtos_log.h"
#include "arkrtos_fault.h"
#include "arkrtos_trace.h"
#if ARKRTOS_HW_SAFE
#include "arkrtos_hwsafe.h"
#endif

/* ============================================================
 * 板级初始化 - 实现 port 层板级接口
 * ============================================================ */
void arkrtos_port_board_init(void)
{
#if (ARKRTOS_USE_SDRAM == 1)
    /* 外置 SDRAM 必须在任何人访问 0xC0000000 之前起来：arkc 的 .ark_buf
     * 与 ramfs 的文件缓冲都落在那里。失败只报错不终止 —— 板子还能起来、
     * 串口还能用，只有编译器与文件缓冲不可用；把错误说清楚，比带着一块
     * 没上电的内存继续跑更接近事实。 */
    {
        int32 sdram_rc = arkrtos_sdram_init();

        if(sdram_rc != 0)
        {
            ARKRTOS_LOGE("SDRAM", "init failed (%d): 0x%08X is unusable (arkc arena and ramfs buffers live there)",
                         (int)sdram_rc, (unsigned)CHIP_SDRAM_BASE);
        }
    }
#endif

    /* CP10/CP11 全权限，否则任务里的浮点指令会升级成 UsageFault（NOCP）。 */
    SCB->CPACR |= (3UL << 20) | (3UL << 22);
    FPU->FPCCR = FPU_FPCCR_ASPEN_Msk;

    /* H743 首版没有编入 mdk_trace（Makefile 里 ARKRTOS_USE_MDK_TRACE=0），
     * 这里的调用因此是 trace.c 的空实现。保留调用点是为了让「板子起来就
     * 带上仪表」这条约定在换回 SWD trace 时不需要再改板级代码。 */
    arkrtos_trace_init();
}

/* ============================================================
 * 中断优先级初始化 - 实现 port 层板级接口
 * ============================================================ */
void arkrtos_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);

    /* 使能 MemManage/BusFault/UsageFault：
     * Cortex-M 复位后这三个异常默认关闭，不使能的话 MPU 越权、非法访问、
     * 未定义指令一律升级成 HardFault，故障类型无法区分，
     * stm32h7xx_it.c 里的对应处理函数也永远不会被调用。 */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
}

/* ============================================================
 * 空闲任务 MPU 配置 - 实现 port 层板级接口
 * ============================================================ */
void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    #if (ARKRTOS_USE_MPU == 1)
    arkrtos_port_mpu_set_region(task_func, 0x1000, stack_addr, stack_size);
    #else
    (void)task_func;
    (void)stack_addr;
    (void)stack_size;
    #endif
}

/* ============================================================
 * 板载设备注册
 * ============================================================ */
extern arkrtos_dev_drv_t usart_drv;
extern arkrtos_dev_drv_t led_drv;

void arkrtos_dev_board_init(void)
{
    /* 板上只有一颗用户灯（PC13），对外仍叫 "led0"：
     * 名字是 App 看到的契约，换板不该让 App 改代码。 */
    arkrtos_dev_register("led0",  &led_drv, LED_ID_STATUS);
    arkrtos_dev_register("ttyS0", &usart_drv, 0);

#if ARKRTOS_HW_SAFE
    /* 声明灯的安全态：属主死亡时强制熄灭。 */
    (void)arkrtos_hwsafe_register("led0", led_drv_safe);
#endif

    /* 块设备在这里登记但不在这里上电：描述符是静态的，
     * arkrtos_blk_init() 在首次使用时才做 bring-up。 */
    if(arkrtos_board_blk_init() != 0)
    {
        ARKRTOS_LOGE("BOARD", "block device registration failed");
    }
}

#if (ARKRTOS_USE_SDRAM == 1)
/* ============================================================
 * 板级 Shell 命令登记 - 内核扩展点实现
 * @brief 把 "sdram" 追加进 ark_shell 的命令表
 * @param idx 内核给的第一个空闲槽位
 * @return 追加后的表长（没有追加就原样返回 idx）
 * @details 这个函数只有内核在 register_kernel_commands() 末尾才会调用，
 *          也就是说命令表已经被 ark_shell_init() 重建过、内容已定型。
 *          以前在 arkrtos_dev_board_init() 里用 arkrtos_shell_ext_register()
 *          登记，写的还是 g_cmd_table[0] —— 随后 ark_shell_commands_init()
 *          从内置表重建前 5 槽，"sdram" 被 "help" 顶掉，板上表现就是
 *          `sdram: command not found`（驱动本身没问题）。
 *          返回表长的约定与 arkvim_shell_register()/arkc_shell_register() 相同。
 * ============================================================ */
int32 arkrtos_board_shell_register(int32 idx)
{
    if(idx < 0)
    {
        return idx;
    }

    /* 表满时保持原样返回：内核接着把哨兵写到 idx，命令表依然自洽。
     * 少一条板级诊断命令可以接受，写坏命令表不行。 */
    if((idx + 1) > ARK_SHELL_MAX_COMMANDS)
    {
        ARKRTOS_LOGE("SDRAM", "shell command table full (%d), \'sdram\' not registered",
                     (int)ARK_SHELL_MAX_COMMANDS);
        return idx;
    }

    g_cmd_table[idx] = ARK_SHELL_CMD("sdram", arkrtos_sdram_shell_cmd,
        "External SDRAM: sdram [info | init | verify | probe | test | fsm | pins | clk <n> | regs]",
        3);
    return idx + 1;
}
#endif /* ARKRTOS_USE_SDRAM */

/* ============================================================
 * 内核异常与节拍向量
 * @brief Cortex-M 核内异常向量的板级接线
 *          硬故障/内存管理/总线/用法异常统一交内核故障处理，
 *          节拍中断交内核 tick 处理，并同步推进 HAL 毫秒基准。
 * ============================================================ */
void SysTick_Handler(void)
{
    /* HAL 的毫秒基准必须跟着内核节拍一起走。
     * CubeMX 生成的那个 SysTick_Handler（内含 HAL_IncTick()）已在
     * stm32h7xx_it.c 里用 #if 0 整体屏蔽，全工程再无第二处调用 ——
     * HAL_GetTick() 永远返回 0，任何用 HAL_Delay() 的驱动都会死等。
     * 内核节拍周期是 ARKRTOS_TICK_PERIOD_US（本板 500us，即 2kHz），
     * 而 HAL 时基是 1ms（1kHz），所以每 2 个内核节拍补一次 HAL_IncTick()。 */
    #if ((1000 % ARKRTOS_TICK_PERIOD_US) != 0)
    #error "ARKRTOS_TICK_PERIOD_US 必须能整除 1000us，否则无法按整毫秒为 HAL 时基补 tick"
    #endif

    if((arkrtos_kernel_get_tick() % (1000u / ARKRTOS_TICK_PERIOD_US)) == 0u)
    {
        HAL_IncTick();
    }

    arkrtos_kernel_tick_handler();
}

/* ============================================================
 * 异常向量入口
 * @brief 进 C 处理体之前，先把异常现场交给 trace（关闭时为零语句）
 * @details GCC/armclang 走 ARKRTOS_TRACE_FAULT_CAPTURE() 宏；该宏在
 *          ARKRTOS_USE_MDK_TRACE==0 时展开为 do { } while(0)，
 *          因此 H743 首版这里只是普通的 C 入口。
 * ============================================================ */
void arkrtos_board_hardfault_body(void);
void arkrtos_board_memmanage_body(void);
void arkrtos_board_busfault_body(void);
void arkrtos_board_usgfault_body(void);

/* ============================================================
 * Raw panic report - no locks, no console, no printf, no malloc
 * @brief One line of truth on USART1 (polled register writes).
 * @details The regular fault path ends in the kernel handler, which logs
 *          through the console, and the console takes an exclusive-access
 *          lock. A fault that breaks exclusive access (or that happens while
 *          the console lock is held) therefore dies before printing anything:
 *          the board just sits there in lockup with a silent wire, and the
 *          fault stays invisible. This reporter runs FIRST and touches only
 *          USART1 registers, so every fault leaves its frame, its fault
 *          status registers and the live MPU regions on the wire.
 *
 *          Build switch ARKRTOS_BOARD_PANIC_RAW: 1 (default on this board)
 *          keeps the reporter, 0 removes the whole block and restores the
 *          one-line handler bodies.
 * ============================================================ */
#ifndef ARKRTOS_BOARD_PANIC_RAW
#define ARKRTOS_BOARD_PANIC_RAW 1
#endif

#if (ARKRTOS_BOARD_PANIC_RAW == 1)

#define PANIC_UART USART1

static void panic_putc(char c)
{
    uint32 guard = 0u;

    while(((PANIC_UART->ISR & USART_ISR_TXE_TXFNF) == 0u) && (guard < 200000u))
    {
        guard++;
    }

    PANIC_UART->TDR = (uint32)(uint8)c;
}

static void panic_puts(const char *s)
{
    while(*s != '\0')
    {
        panic_putc(*s);
        s++;
    }
}

static void panic_hex(uint32 v)
{
    static const char hex_digit[] = "0123456789abcdef";
    int32 i;

    panic_puts("0x");
    for(i = 7; i >= 0; i--)
    {
        panic_putc(hex_digit[(v >> (uint32)(i * 4)) & 0xFu]);
    }
}

static void panic_field(const char *name, uint32 v)
{
    panic_puts(name);
    panic_putc('=');
    panic_hex(v);
    panic_putc(' ');
}

void arkrtos_board_panic_dump(uint32 kind, uint32 exc_return, uint32 sp)
{
    const char *what = "HARD";
    uint32 *p_frame;
    uint32 i;

    /* UART never came up (fault before the console driver ran): stay silent
     * rather than spin on a peripheral that will never set TXE. */
    if((PANIC_UART->CR1 & USART_CR1_UE) == 0u)
    {
        return;
    }

    /* Stop the interrupt-driven console from interleaving with this dump. */
    PANIC_UART->CR1 &= ~(USART_CR1_TXEIE_TXFNFIE | USART_CR1_TCIE);

    if(kind == 1u)
    {
        what = "MEM ";
    }
    else if(kind == 2u)
    {
        what = "BUS ";
    }
    else if(kind == 3u)
    {
        what = "USG ";
    }

    p_frame = (uint32 *)sp;

    panic_puts("\r\n*** PANIC " );
    panic_puts(what);
    panic_puts("***\r\n");

    panic_field("EXC_RET", exc_return);
    panic_field("SP", sp);
    panic_putc('\n');

    /* Stacked frame: R0-R3, R12, LR, PC, xPSR. With an FP context the core
     * words come first and S0-S15/FPSCR follow, so [0..7] is the same. */
    panic_field("R0", p_frame[0]);
    panic_field("R1", p_frame[1]);
    panic_field("R2", p_frame[2]);
    panic_field("R3", p_frame[3]);
    panic_putc('\n');
    panic_field("R12", p_frame[4]);
    panic_field("LR", p_frame[5]);
    panic_field("PC", p_frame[6]);
    panic_field("XPSR", p_frame[7]);
    panic_putc('\n');

    panic_field("CFSR", SCB->CFSR);
    panic_field("HFSR", SCB->HFSR);
    panic_field("MMFAR", SCB->MMFAR);
    panic_field("BFAR", SCB->BFAR);
    panic_putc('\n');
    panic_field("SHCSR", SCB->SHCSR);
    panic_field("CPACR", SCB->CPACR);
    panic_field("MPUCTRL", MPU->CTRL);
    panic_putc('\n');

    for(i = 0u; (i < 4u) && (i < ARKRTOS_MPU_REGION_MAX); i++)
    {
        MPU->RNR = i;
        panic_puts("R");
        panic_putc((char)('0' + (char)i));
        panic_putc(' ');
        panic_field("BAR", MPU->RBAR);
        panic_field("ASR", MPU->RASR);
        panic_putc('\n');
    }
}

#endif /* ARKRTOS_BOARD_PANIC_RAW */

/* Shared entry from the naked vectors:
 *   r0 = EXC_RETURN, r1 = stacked frame pointer, r2 = fault kind. */
void arkrtos_board_fault_entry(uint32 exc_return, uint32 sp, uint32 kind);

void __attribute__((naked)) HardFault_Handler(void)
{
    __asm volatile(
        "movs r2, #0                  \n"
        "mov  r0, lr                  \n"
        "tst  r0, #4                  \n"
        "ite  eq                      \n"
        "mrseq r1, msp                \n"
        "mrsne r1, psp                \n"
        "b    arkrtos_board_fault_entry \n");
}

void __attribute__((naked)) MemManage_Handler(void)
{
    __asm volatile(
        "movs r2, #1                  \n"
        "mov  r0, lr                  \n"
        "tst  r0, #4                  \n"
        "ite  eq                      \n"
        "mrseq r1, msp                \n"
        "mrsne r1, psp                \n"
        "b    arkrtos_board_fault_entry \n");
}

void __attribute__((naked)) BusFault_Handler(void)
{
    __asm volatile(
        "movs r2, #2                  \n"
        "mov  r0, lr                  \n"
        "tst  r0, #4                  \n"
        "ite  eq                      \n"
        "mrseq r1, msp                \n"
        "mrsne r1, psp                \n"
        "b    arkrtos_board_fault_entry \n");
}

void __attribute__((naked)) UsageFault_Handler(void)
{
    __asm volatile(
        "movs r2, #3                  \n"
        "mov  r0, lr                  \n"
        "tst  r0, #4                  \n"
        "ite  eq                      \n"
        "mrseq r1, msp                \n"
        "mrsne r1, psp                \n"
        "b    arkrtos_board_fault_entry \n");
}

void arkrtos_board_fault_entry(uint32 exc_return, uint32 sp, uint32 kind)
{
    #if (ARKRTOS_BOARD_PANIC_RAW == 1)
    arkrtos_board_panic_dump(kind, exc_return, sp);
    #else
    (void)exc_return;
    (void)sp;
    #endif

    ARKRTOS_TRACE_FAULT_CAPTURE();

    if(kind == 1u)
    {
        arkrtos_board_memmanage_body();
    }
    else if(kind == 2u)
    {
        arkrtos_board_busfault_body();
    }
    else if(kind == 3u)
    {
        arkrtos_board_usgfault_body();
    }
    else
    {
        arkrtos_board_hardfault_body();
    }
}

void arkrtos_board_hardfault_body(void)
{
    volatile uint32 hfsr  = SCB->HFSR;
    volatile uint32 cfsr  = SCB->CFSR;
    volatile uint32 mmfar = SCB->MMFAR;
    volatile uint32 bfar  = SCB->BFAR;
    uint32 sp;

    (void)hfsr; (void)cfsr; (void)mmfar; (void)bfar;

    /* 必须交给内核故障处理：任务级恢复（重建栈帧重启该任务），
     * 连续故障达到 APP_CRASH_RESTART_MAX 时由 arkrtos_loader_on_fault() 禁用该 App。 */
    sp = arkrtos_hardfault_handler();

    if(sp != 0u)
    {
        /* 内核已选出可运行任务并返回其栈指针：直接恢复并异常返回（本函数不返回） */
        arkrtos_port_resume_task(sp);
    }

    while(1) { }
}

/* ============================================================
 * 其余 CPU 异常向量
 * @brief 统一交给内核故障处理（见 arkrtos_cpu_fault_handler）
 * @details stm32h7xx_it.c 中 CubeMX 生成的同名函数已在源码内用 #if 0 屏蔽。
 *          内核返回可恢复的任务栈指针时，直接恢复该任务上下文并异常返回；
 *          返回 0（内核/中断上下文故障）时落回 while(1)，停机等调试器接管。
 * ============================================================ */
void arkrtos_board_memmanage_body(void)
{
    uint32 sp = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_MEMFAULT);

    if(sp != 0u)
    {
        arkrtos_port_resume_task(sp);
    }

    while(1) { }
}

void arkrtos_board_busfault_body(void)
{
    uint32 sp = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_BUSFAULT);

    if(sp != 0u)
    {
        arkrtos_port_resume_task(sp);
    }

    while(1) { }
}

void arkrtos_board_usgfault_body(void)
{
    uint32 sp = arkrtos_cpu_fault_handler(ARKRTOS_FAULT_USGFAULT);

    if(sp != 0u)
    {
        arkrtos_port_resume_task(sp);
    }

    while(1) { }
}
