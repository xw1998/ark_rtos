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
#include "arkrtos_task.h"   /* arkrtos_current_task_id（故障现场的任务号） */

/* 外部中断（PLIC）通路由 port 层提供；头文件在 arkrtos_hal.h 里没开这一类
 * 通用接口（各架构的接线方式差得太远），所以这里就地声明。 */
void arkrtos_port_plic_enable(uint32 irq, uint32 prio);

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
#define UART_IIR            2u   /* 读：中断标识（与 FCR 同地址，读写不同） */
#define UART_LCR            3u   /* 线路控制（位宽/校验/停止位）           */
#define UART_MCR            4u   /* MODEM 控制                             */
#define UART_LSR            5u   /* 线路状态：bit0=DR 有数据，bit5=THRE    */

#define UART_LCR_8N1        0x03u
#define UART_FCR_ENABLE     0x01u
#define UART_LSR_DR         0x01u  /* bit0：接收数据就绪 */
#define UART_LSR_THRE       0x20u  /* bit5：发送保持寄存器空 */

#define UART_IER_RDA        0x01u  /* 接收数据可用中断 */
#define UART_IER_RLS        0x04u  /* 线路状态中断（溢出/帧错） */

/* IIR：bit0 = 0 表示有中断挂起，bit3:1 = 类型 */
#define UART_IIR_NO_PEND    0x01u
#define UART_IIR_ID_MASK    0x0Eu
#define UART_IIR_ID_RDA     0x04u  /* 接收数据可用 */
#define UART_IIR_ID_RTO     0x0Cu  /* 字符超时（FIFO 里还有残字节） */
#define UART_IIR_ID_LSR     0x06u  /* 线路状态（错误位） */

/* 接收环形缓冲：256 字节。输入是人手敲的，但要能接住一次粘贴（上百字节）而不丢。 */
#define UART_RX_RING_SIZE   256u

static uint8  s_rx_ring[UART_RX_RING_SIZE];
static volatile uint32 s_rx_head;
static volatile uint32 s_rx_tail;
static volatile uint32 s_rx_dropped;
static volatile uint32 s_rx_bytes;
static volatile uint32 s_isr_count;
static volatile uint32 s_unknown_irq;

/**
* @brief 每收到一个字节在中断里调一次（弱钩子，应用可覆盖）
* @details 存在的意义是让"中断 -> 内核"这一段变成应用可见的事件：应用在
*          钩子里调 arkrtos_sem_post_from_isr()，就能拿到"在中断里唤醒任务"
*          的完整证据链（而不是靠轮询"看起来切过去了"）。
*          ！！只能做中断安全的事：不能阻塞、不能长循环。
*/
void arkrtos_board_uart_rx_hook(uint8 byte);

__attribute__((weak)) void arkrtos_board_uart_rx_hook(uint8 byte)
{
    (void)byte;
}

/**
* @brief 中断接收：把 16550 的"接收数据可用"接到 PLIC，并打开该源
* @details 三条一起才成立：
*          1) 16550 自己开 IER 的 RDA 位（否则它不会拉 INTx）；
*          2) PLIC 给 UART0 的 IRQ（virt 机器固定 = 10）一个非零优先级；
*          3) PLIC 的使能位与抢占阈值放行。
*          少了 2 或 3，串口会在 host 端收到字节时把数据静静丢掉 ——
*          表现为"能打不能收"，很容易误判成驱动写错。
*
*          发送仍然是轮询：QEMU 的 16550 发送永远就绪，用中断只会多一层
*          状态机而没有任何收益；真实板子上发送中断的意义是"别忙等"，
*          那是驱动层的取舍，与本移植要验证的"中断入口到得了内核"无关。
*/
void arkrtos_board_uart_irq_init(void)
{
    s_rx_head     = 0u;
    s_rx_tail     = 0u;
    s_rx_dropped  = 0u;
    s_isr_count   = 0u;
    s_rx_bytes    = 0u;
    s_unknown_irq = 0u;

    /* 先把 FIFO 里的残留读干净，再开中断，避免一开就进一次"假"中断 */
    while((UART_REG(UART_LSR) & UART_LSR_DR) != 0u)
    {
        (void)UART_REG(UART_RBR);
    }

    UART_REG(UART_IER) = (uint8)(UART_IER_RDA | UART_IER_RLS);

    arkrtos_port_plic_enable(ARKRTOS_QEMU_UART_IRQ, 1u);
}

/**
* @brief 从接收环形缓冲取数据（任务上下文）
* @param buf 目标缓冲
* @param max 最多取多少字节
* @return 实际取到的字节数（0 = 当前没有数据）
*/
uint32 arkrtos_board_uart_rx_read(uint8 *buf, uint32 max)
{
    uint32 n = 0u;

    if(buf == 0)
    {
        return 0u;
    }

    while((n < max) && (s_rx_tail != s_rx_head))
    {
        buf[n] = s_rx_ring[s_rx_tail];
        s_rx_tail = (s_rx_tail + 1u) % UART_RX_RING_SIZE;
        n++;
    }

    return n;
}

uint32 arkrtos_board_uart_isr_count(void)  { return s_isr_count; }
uint32 arkrtos_board_uart_rx_bytes(void)   { return s_rx_bytes; }
uint32 arkrtos_board_uart_rx_dropped(void) { return s_rx_dropped; }
uint32 arkrtos_board_uart_unknown_irq(void){ return s_unknown_irq; }

/**
* @brief 16550 中断服务：把源摘干净再返回
* @details 电平触发断不能"看一眼就走"——IIR 的 bit0 还在就说明没摘净，
*          PLIC 会立刻再投一次，表现为中断风暴把节拍饿死。所以这里是
*          "按 IIR 的类型把对应寄存器读到空"的循环。
*/
static void uart_isr(void)
{
    uint32 guard = 0u;

    s_isr_count++;

    /* 最多转 64 圈：一个 FIFO 最多 16 字节，留足余量；真正兜底的是
     * "读干净"本身，这个上限只防硬件异常导致的死循环。 */
    while(((UART_REG(UART_IIR) & UART_IIR_NO_PEND) == 0u) && (guard < 64u))
    {
        uint32 id = UART_REG(UART_IIR) & UART_IIR_ID_MASK;

        guard++;

        if((id == UART_IIR_ID_RDA) || (id == UART_IIR_ID_RTO) || (id == UART_IIR_ID_LSR))
        {
            /* 读到 DR 空为止（顺带把 LSR 的错误位读掉） */
            while((UART_REG(UART_LSR) & UART_LSR_DR) != 0u)
            {
                uint8 byte = (uint8)(UART_REG(UART_RBR) & 0xFFu);
                uint32 next = (s_rx_head + 1u) % UART_RX_RING_SIZE;

                s_rx_bytes++;

                if(next == s_rx_tail)
                {
                    s_rx_dropped++;          /* 满了：丢新的，保住已收的 */
                }
                else
                {
                    s_rx_ring[s_rx_head] = byte;
                    s_rx_head = next;
                }

                arkrtos_board_uart_rx_hook(byte);
            }
        }
        else
        {
            /* 发送保持寄存器空之类的：本板发送走轮询，这里只要读一下
             * IIR 就已经把它摘掉了（对 16550 而言 THRE 中断靠写 THR 或读 IIR 清）。 */
            break;
        }
    }
}

/**
* @brief 外部中断分发（覆盖 port 层弱实现）
* @details 只认 UART0。其它中断号只计数不处理 —— 不假装处理，也不停机：
*          停机会把一个配错的 plic_enable() 变成整机死机。
*/
void arkrtos_port_external_isr(uint32 irq)
{
    if(irq == ARKRTOS_QEMU_UART_IRQ)
    {
        uart_isr();
    }
    else
    {
        s_unknown_irq++;
    }
}

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
 * @details QEMU virt 这台机器上没有 LED、没有按键、没有 Flash 控制器，
 *          唯一真正存在且需要登记到设备框架里的是控制台 16550。
 *          它必须在这里注册（而不是在 main 里）：Shell / 内核日志都按
 *          SHELL_DEV_NAME 打开控制台，而本函数由 arkrtos_kernel_module_init()
 *          在设备框架初始化之后、调度启动之前调用，正是注册的时点。
 *          注册失败不再沉默：返回值不为 0 就打到控制台，否则 shell 起来
 *          只会报"打不开串口"，看不出根因在注册这一步。
 * ============================================================ */
void arkrtos_dev_board_init(void)
{
    if(arkrtos_board_uart_dev_register() != 0)
    {
        arkrtos_board_puts("[board] dev register ttyS0 FAILED\n");
    }
}

/* ============================================================
 * PMP 违规现场（覆盖 port 层的弱实现）
 * @details 保护机制生效时，系统最想知道的是三件事：什么原因、碰了哪个
 *          地址、从哪个特权级碰的。三个数都来自硬件（mcause / mtval /
 *          进入 trap 前的 mstatus.MPP），不是内核推断出来的。
 *          注意：这里**不做善于处理**——违规就是违规，先把它打出来，
 *          恢复策略由上层的 arkrtos_cpu_fault_handler 决定。
 * ============================================================ */
void arkrtos_port_fault_detail(uint32 mcause, uint32 mtval, uint32 from_priv)
{
    uint32 mepc;
    uint32 ra;
    uint32 task;

    __asm__ volatile("csrr %0, mepc" : "=r"(mepc) : : "memory");
    /* ra 是「谁调进来的」：故障常常落在库函数内部（memcpy/memset），
     * 只有 ra 才指得出肇事的那一行调用。缺了它，mepc 只会告诉我们在
     * memcpy 里，定位还得再烧一次。 */
    __asm__ volatile("mv %0, ra" : "=r"(ra) : : "memory");

    /* 任务号取内核的当前任务：故障发生在任务上下文时它就是肇事者，
     * 发生在内核/中断上下文时是 0。这两个数与 mcause/mtval 一起才够定位。 */
    task = (arkrtos_current_task_id > 0) ? (uint32)arkrtos_current_task_id : 0u;

    arkrtos_board_puts("\n[PMP] fault mcause=");
    arkrtos_board_putu(mcause);
    arkrtos_board_puts(" mtval=");
    arkrtos_board_puthex(mtval);
    arkrtos_board_puts(" mepc=");
    arkrtos_board_puthex(mepc);
    arkrtos_board_puts(" ra=");
    arkrtos_board_puthex(ra);
    arkrtos_board_puts(" from=");
    arkrtos_board_puts((from_priv == 0u) ? "U" : "M");
    arkrtos_board_puts(" task=");
    arkrtos_board_putu(task);
    arkrtos_board_puts("\n");
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
