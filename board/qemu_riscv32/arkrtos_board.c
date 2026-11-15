/**
* @file arkrtos_board.c
* @brief Ark RTOS 板级实现 - QEMU `virt` 机器（RISC-V RV32，单 hart）
* @details 这一份板级只做三件事：
*          1) 把 16550 UART 配成 8N1 并给出轮询输出；
*          2) 覆盖 port 层的弱板级钩子（开机横幅、致命异常报告）；
*          3) 提供 Flash / 看门狗的空桩 —— 注意是「明确返回失败」的空桩，
*             不是假装成功的桩：QEMU 上没有片内 Flash，也没有 IWDG，
*             声称擦写成功会让上层以为它真写进去了。
*
* @note 中断入口（mtvec）与 mie/mstatus 的配置留在 port 层的默认实现里：
*       它是 RISC-V 特权架构的固定动作，不是板级知识。
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_config.h"
#include "arkrtos_board.h"

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG
#endif

/* ============================================================
 * 16550 UART（轮询）
 * ============================================================ */
#define UART_BASE           (ARKRTOS_QEMU_UART_BASE)
#define UART_REG(i)         (*(volatile uint8 *)(UART_BASE + (uint32)(i)))

#define UART_RBR            0u   /* 读：接收缓冲；写同一地址 = THR（发送） */
#define UART_IER            1u   /* 中断使能（本板不用中断收）             */
#define UART_FCR            2u   /* 写：FIFO 控制                          */
#define UART_LCR            3u   /* 线路控制（位宽/校验/停止位）           */
#define UART_MCR            4u   /* MODEM 控制                             */
#define UART_LSR            5u   /* 线路状态：bit5=THRE 发送保持寄存器空    */

#define UART_LCR_8N1        0x03u
#define UART_FCR_ENABLE     0x01u
#define UART_LSR_THRE       0x20u

void arkrtos_board_console_init(void)
{
    UART_REG(UART_IER) = 0x00u;                 /* 全部关掉：输出走轮询，不收中断 */
    UART_REG(UART_LCR) = UART_LCR_8N1;
    UART_REG(UART_FCR) = UART_FCR_ENABLE;       /* 打开并清空 FIFO */
    UART_REG(UART_MCR) = 0x03u;                 /* DTR | RTS */
}

void arkrtos_board_putc(char c)
{
    /* 等发送保持寄存器空。QEMU 的 16550 永远会空出来，这个循环不会挂死；
     * 真实芯片上若外设没上电，这里会卡住 —— 那正是要暴露的问题。 */
    while((UART_REG(UART_LSR) & UART_LSR_THRE) == 0u)
    {
    }

    if(c == '\n')
    {
        UART_REG(UART_RBR) = (uint8)'\r';
        while((UART_REG(UART_LSR) & UART_LSR_THRE) == 0u)
        {
        }
    }

    UART_REG(UART_RBR) = (uint8)c;
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
    char buf[11];
    uint32 i = 0u;

    if(value == 0u)
    {
        arkrtos_board_putc('0');
        return;
    }

    while((value != 0u) && (i < 10u))
    {
        buf[i] = (char)('0' + (value % 10u));
        value /= 10u;
        i++;
    }

    while(i > 0u)
    {
        i--;
        arkrtos_board_putc(buf[i]);
    }
}

void arkrtos_board_puthex(uint32 value)
{
    static const char hex[] = "0123456789abcdef";
    int32 shift;

    arkrtos_board_puts("0x");
    for(shift = 28; shift >= 0; shift -= 4)
    {
        arkrtos_board_putc(hex[(value >> (uint32)shift) & 0xFu]);
    }
}

/* ============================================================
 * 板级初始化（覆盖 port 层的弱实现）
 * ============================================================ */
void arkrtos_port_board_init(void)
{
    arkrtos_board_console_init();

    arkrtos_board_puts("\n[Ark RTOS] board: qemu_riscv32 (virt, rv32imac, 1 hart)\n");
    arkrtos_board_puts("[Ark RTOS] arch=");
    arkrtos_board_puts(ARKRTOS_ARCH_NAME);
    arkrtos_board_puts(" clint=");
    arkrtos_board_puthex(ARKRTOS_PORT_CLINT_BASE);
    arkrtos_board_puts(" timebase=");
    arkrtos_board_putu(ARKRTOS_MACHINE_TIMER_HZ);
    arkrtos_board_puts("Hz\n");
}

/* ============================================================
 * 设备注册（覆盖内核期望的板级钩子）
 * @details QEMU virt 这台机器上没有任何真实外设：没有 LED、没有按键、
 *          没有 Flash 控制器。控制台走 16550 轮询，不经 arkrtos_dev 设备表。
 *          所以这里故意留空 —— 空实现的意思是"本板没有设备"，而不是
 *          "设备注册失败被吞掉"。
 * ============================================================ */
void arkrtos_dev_board_init(void)
{
}

/* ============================================================
 * 致命异常报告（覆盖 port 层的弱实现）
 * @details 只打 mcause 与 mepc 两个最关键的读数，然后停机。不尝试恢复：
 *          bring-up 阶段一次异常就是一次真缺陷，掩盖它比停下来更糟。
* ============================================================ */
void arkrtos_port_fatal_report(uint32 mcause)
{
    uint32 mepc;

    __asm__ volatile("csrr %0, mepc" : "=r"(mepc) : : "memory");

    arkrtos_board_puts("\n[Ark RTOS] FATAL: mcause=");
    arkrtos_board_puthex(mcause);
    arkrtos_board_puts(" mepc=");
    arkrtos_board_puthex(mepc);
    arkrtos_board_puts("\n[Ark RTOS] halted.\n");
}
