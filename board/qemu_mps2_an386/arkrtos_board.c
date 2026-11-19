/**
* @file arkrtos_board.c
* @brief Ark RTOS 板级实现 - QEMU `mps2-an386`（Cortex-M4）
* @details 这一份板级只做四件事：
*          1) 把 CMSDK APB UART0 配成 8N1 并给出轮询输出；
*          2) 提供 SystemCoreClock（Cortex-M4 移植层用它换算 SysTick 与 us 延时）；
*          3) 接线 Cortex-M 的核内异常：SysTick 交内核 tick、PendSV/SVC 由
*             port 层的汇编提供、四个 fault 打印现场后停机；
*          4) 满足 port 层要求的板级钩子（board_init / irq_init / set_idle_mpu）。
*
*          QEMU 上没有片子 Flash、没有看门狗、也没有第二块板子，本档的功能
*          开关在 arkrtos_board_config.h 里全部关掉，所以这里没有它们的桩。
*
* @note 本档 ARKRTOS_USE_MPU = 0，arkrtos_port_set_idle_mpu() 是明确空桩，
*       不是「配了但没生效」。
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_config.h"
#include "arkrtos_board.h"
#include "mps2_an386.h"

/* ============================================================
 * CMSIS 需要的系统时钟变量
 * @details Cortex-M 的 CMSIS 约定由 system_<device>.c 定义这个符号，
 *          本板没有厂商启动文件，所以在这里给出。取值来自
 *          QEMU hw/arm/mps2.c 的 SYSCLK_FRQ（25 MHz）。
 * ============================================================ */
uint32_t SystemCoreClock = MPS2_SYSCLK_HZ;

/* ============================================================
 * CMSDK APB UART0（轮询发送）
 * @details 寄存器按 32 位字偏移访问：
 *            DATA       0x00  写 = 发送、读 = 接收
 *            STATE      0x04  bit0 TXFULL（1 = 发送缓冲满）
 *                             bit1 RXFULL
 *            CTRL       0x08  bit0 TXEN、bit1 RXEN
 *            INTSTATUS  0x0C
 *            BAUDDIV    0x10
 *          QEMU 的实现里发送永远很快就绪，轮询不会卡住；但真实 MPS2 板上
 *          TXFULL 会真的置起，所以这里按状态位等，不假定「写就有」。
 * ============================================================ */
#define UART0_BASE          (MPS2_UART0_BASE)
#define UART0_REG(off)      (*(volatile uint32 *)(UART0_BASE + (uint32)(off)))

#define UART0_DATA          0x00u
#define UART0_STATE         0x04u
#define UART0_CTRL          0x08u
#define UART0_BAUDDIV       0x10u

#define UART0_STATE_TXFULL  0x1u
#define UART0_CTRL_TXEN     0x1u
#define UART0_CTRL_RXEN     0x2u

/* 波特率分频：CMSDK 的 BAUDDIV = pclk / 波特率（QEMU 不据此做限速，
 * 真实板上按 25 MHz / 115200 ≈ 217）。 */
#define UART0_BAUDDIV_115200  ((MPS2_SYSCLK_HZ / 115200u))

void arkrtos_board_console_init(void)
{
    UART0_REG(UART0_BAUDDIV) = UART0_BAUDDIV_115200;
    UART0_REG(UART0_CTRL)    = (UART0_CTRL_TXEN | UART0_CTRL_RXEN);
}

void arkrtos_board_putc(char c)
{
    /* 换行补 '\r'：QEMU 的串口按终端语义显示，只给 '\n' 会出现阶梯状输出。
     * 这与 RV32/LA64 两份板级同一条约定。 */
    if(c == '\n')
    {
        arkrtos_board_putc('\r');
    }

    while((UART0_REG(UART0_STATE) & UART0_STATE_TXFULL) != 0u)
    {
        /* 等发送缓冲空出来 */
    }

    UART0_REG(UART0_DATA) = (uint32)(uint8)c;
}

void arkrtos_board_puts(const char *s)
{
    if(s == 0)
    {
        return;
    }

    while(*s != '\0')
    {
        arkrtos_board_putc(*s);
        s++;
    }
}

void arkrtos_board_putu(uint32 value)
{
    char buf[10];
    uint32 n = 0u;

    if(value == 0u)
    {
        arkrtos_board_putc('0');
        return;
    }

    while((value > 0u) && (n < sizeof(buf)))
    {
        buf[n] = (char)('0' + (value % 10u));
        value /= 10u;
        n++;
    }

    while(n > 0u)
    {
        n--;
        arkrtos_board_putc(buf[n]);
    }
}

void arkrtos_board_puthex(uint32 value)
{
    /* 定宽 8 位（与 RV32 板级同一约定：不带 0x 前缀）。 */
    static const char hex[] = "0123456789ABCDEF";
    int32 shift;

    for(shift = 28; shift >= 0; shift -= 4)
    {
        arkrtos_board_putc(hex[(value >> (uint32)shift) & 0xFu]);
    }
}

/* ============================================================
 * 板级初始化 - 实现 port 层板级接口
 * ============================================================ */
void arkrtos_port_board_init(void)
{
    /* 本档 FPU 关（-mfloat-abi=soft），所以不碰 CPACR/FPCCR：
     * 打开 FPU 却不保存 S16-S31 的上下文切换会静默破坏任务帧。 */
    arkrtos_board_console_init();
}

/* ============================================================
 * 板载设备注册
 * @details 设备框架本身是内核必编的一部分（arkrtos_dev_module_init 无条件调用），
 *          但本档 ARKRTOS_USE_SHELL = 0、没有驱动需要命名，所以这里不登记
 *          任何设备。这是明确空桩，不是漏写：
 *          控制台输出走板级轮询函数，不经过设备框架。
 * ============================================================ */
void arkrtos_dev_board_init(void)
{
}

/* ============================================================
 * 中断优先级初始化 - 实现 port 层板级接口
 * @details 与 STM32 板级同一套策略：SysTick 最高、SVC 次之、PendSV 最低。
 *          PendSV 压到最低是任务切换的前提——它必须在所有其它中断都退完之后
 *          才被裁决，否则会切在半截中断里。
 * ============================================================ */
void arkrtos_port_irq_init(void)
{
    NVIC_SetPriorityGrouping(0);
    NVIC_SetPriority(PendSV_IRQn,  0xFF);
    NVIC_SetPriority(SysTick_IRQn, 0x00);
    NVIC_SetPriority(SVCall_IRQn,  0x01);

    /* MemManage/BusFault/UsageFault 复位后是关的，不使能就全部升级成
     * HardFault，故障类型分不开。本档 MPU 未开，但把这三条打开仍有意义：
     * 未定义指令、非对齐访问会走 UsageFault，读数比 HardFault 精确。 */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk |
                  SCB_SHCSR_BUSFAULTENA_Msk |
                  SCB_SHCSR_USGFAULTENA_Msk;
}

/* ============================================================
 * 空闲任务 MPU 配置 - 实现 port 层板级接口
 * @details 本档 ARKRTOS_USE_MPU = 0，明确空桩。
 * ============================================================ */
void arkrtos_port_set_idle_mpu(uint32 task_func, uint32 stack_addr, uint32 stack_size)
{
    (void)task_func;
    (void)stack_addr;
    (void)stack_size;
}

/* ============================================================
 * 节拍中断
 * ============================================================ */
void SysTick_Handler(void)
{
    arkrtos_kernel_tick_handler();
}

/* ============================================================
 * 异常处理
 * @details 本档没有故障恢复链路（CRASH_LOG / loader 全关），所以打印现场
 *          之后就地停机，等调试器或串口读数说明问题。停机是**故意**的：
 *          带着坏现场继续跑会把它变成更难查的二次故障。
 * ============================================================ */
void arkrtos_board_fault_report(const char *name, uint32 cfsr, uint32 hfsr,
                                uint32 mmfar, uint32 bfar)
{
    arkrtos_board_puts("\n[fault] ");
    arkrtos_board_puts(name);
    arkrtos_board_puts(" hfsr=");
    arkrtos_board_puthex(hfsr);
    arkrtos_board_puts(" cfsr=");
    arkrtos_board_puthex(cfsr);
    arkrtos_board_puts(" mmfar=");
    arkrtos_board_puthex(mmfar);
    arkrtos_board_puts(" bfar=");
    arkrtos_board_puthex(bfar);
    arkrtos_board_puts(" halted\n");
}

static void fault_halt(const char *name)
{
    arkrtos_board_fault_report(name, SCB->CFSR, SCB->HFSR, SCB->MMFAR, SCB->BFAR);

    for(;;)
    {
        /* 停机等调试器 */
    }
}

void HardFault_Handler(void)
{
    fault_halt("HardFault");
}

void MemManage_Handler(void)
{
    fault_halt("MemManage");
}

void BusFault_Handler(void)
{
    fault_halt("BusFault");
}

void UsageFault_Handler(void)
{
    fault_halt("UsageFault");
}
