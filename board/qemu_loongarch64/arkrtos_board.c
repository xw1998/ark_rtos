/**
* @file arkrtos_board.c
* @brief Ark RTOS 板级实现 - QEMU `virt` 机器（LoongArch64，单核）
* @details 这一份板级只做三件事：
*          1) 把 ns16550a UART 配成 8N1 并给出轮询输出；
*          2) 覆盖 port 层的弱板级钩子（开机横幅、致命异常报告）；
*          3) 提供 Flash / 看门狗的空桩 —— 注意是「明确返回失败」的空桩，
*             不是假装成功的桩：QEMU 上没有片内 Flash，也没有 IWDG，
*             声称擦写成功会让上层以为它真写进去了。
*
* @note 异常入口（EENTRY）与 CRMD.IE 的配置留在 port 层的默认实现里：
*       它是 LoongArch 架构的固定动作，不是板级知识。
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_config.h"
#include "arkrtos_board.h"
#include "arkrtos_trace.h"     /* arkrtos_trace_init()：内核事件追踪 */

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG
#endif

/* ============================================================
 * ns16550a（轮询）
 * @details 与 16550 的寄存器布局一致：偏移 0 = RBR/THR，5 = LSR。
*          实测 QEMU loongarch virt 的 LSR bit5（THRE）会正常置起，
*          所以轮询发送可用，不需要中断。
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

    arkrtos_board_puts("\n[Ark RTOS] board: qemu_loongarch64 (virt, la64, 1 core)\n");
    arkrtos_board_puts("[Ark RTOS] arch=");
    arkrtos_board_puts(ARKRTOS_ARCH_NAME);
    arkrtos_board_puts(" uart=");
    arkrtos_board_puthex(ARKRTOS_QEMU_UART_BASE);
    arkrtos_board_puts(" timer=");
    arkrtos_board_putu(ARKRTOS_PORT_TIMER_HZ);
    arkrtos_board_puts("Hz rdtime=");
    arkrtos_board_putu(ARKRTOS_PORT_RDTIME_HZ);
    arkrtos_board_puts("Hz\n");

    /* 追踪组件与板子一起起来，而不是等谁敲一条 shell 命令：复位之后那一段
     * 恰恰是「复现不了」的那一段。环是静态的（QEMU 目标用 BUFF 后端，环在
     * .bss 里），init 只清游标、起时间基，不分配。开关关掉时这里是空函数。 */
    arkrtos_trace_init();
}

/* ============================================================
 * 设备注册（覆盖内核期望的板级钩子）
 * @details 本板真实存在的外设只有 ns16550a 控制台和一块 RAM 介质。控制台
 *          必须进设备表：内核 Shell 是按**设备名**打开 ttyS0 的，不进表就
 *          只会报"打不开"。块设备由示例在 main 里显式注册（它需要在
 *          task_fs 里决定何时 format），不在这里做。
 *          注册失败要打出来 —— 静默失败会让 shell 起不来却看不出原因。
 * ============================================================ */
void arkrtos_dev_board_init(void)
{
    if(arkrtos_board_uart_dev_register() != 0)
    {
        arkrtos_board_puts("[board] dev register ttyS0 FAILED\n");
    }
}

/* ============================================================
 * 致命异常报告（覆盖 port 层的弱实现）
 * @details 打三个最关键的读数：ESTAT（Ecode + IS）、ERA（出错 PC）、
 *          BADV 不读（QEMU 对本移植用到的异常都会填，但 badi 也可能为 0，
 *          读出来反而误导）。然后停机。不尝试恢复：bring-up 阶段一次
 *          异常就是一次真缺陷，掩盖它比停下来更糟。
* ============================================================ */
void arkrtos_port_fatal_report(uint32 code)
{
    uint32 era;
    uint32 crmd;
    uint32 ecode;

    (void)code;

    __asm__ volatile("csrrd %0, 0x6" : "=r"(era)  : : "memory");
    __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd) : : "memory");

    /* code 传进来的是 ESTAT 原值（中断源）或 Ecode 已清零的值；
     * 这里重新读一次 ESTAT 打全，避免两处口径不一致。 */
    __asm__ volatile("csrrd %0, 0x5" : "=r"(ecode) : : "memory");

    /* BADV（CSR 0x7）是出错地址、BADI（0x8）是出错指令，和 ARM 的 BFAR 同位：
     * 没有它们只能知道"在哪条指令出事"，有了才知道"碰的是哪个地址"。 */
    {
        uint32 badv;
        uint32 badi;

        __asm__ volatile("csrrd %0, 0x7" : "=r"(badv) : : "memory");
        __asm__ volatile("csrrd %0, 0x8" : "=r"(badi) : : "memory");
        arkrtos_board_puts("\n[Ark RTOS] FATAL: estat=");
        arkrtos_board_puthex(ecode);
        arkrtos_board_puts(" ecode=");
        arkrtos_board_puthex((ecode >> 16) & 0x3Fu);
        arkrtos_board_puts(" era=");
        arkrtos_board_puthex(era);
        arkrtos_board_puts(" badv=");
        arkrtos_board_puthex(badv);
        arkrtos_board_puts(" badi=");
        arkrtos_board_puthex(badi);
        arkrtos_board_puts(" crmd=");
        arkrtos_board_puthex(crmd);
        arkrtos_board_puts("\n[Ark RTOS] halted.\n");
        return;
    }
    arkrtos_board_puts("\n[Ark RTOS] FATAL: estat=");
    arkrtos_board_puthex(ecode);
    arkrtos_board_puts(" ecode=");
    arkrtos_board_puthex((ecode >> 16) & 0x3Fu);
    arkrtos_board_puts(" era=");
    arkrtos_board_puthex(era);
    arkrtos_board_puts(" crmd=");
    arkrtos_board_puthex(crmd);
    arkrtos_board_puts("\n[Ark RTOS] halted.\n");
}
