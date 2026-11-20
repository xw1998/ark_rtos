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
#include "arkrtos_trace.h"  /* arkrtos_trace_init()：追踪随板起来 */

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

#if (ARKRTOS_USE_SMP == 1)
    /* 开 SMP 时这行不能再写 "1 hart"\uff1a镜像里 ARKRTOS_CPU_NUM 个核都会起。
     * 关档仍走原字符串，保证默认镜像逐字节不变。 */
    arkrtos_board_puts("\n[Ark RTOS] board: qemu_riscv32 (virt, rv32imac, smp)\n");
#else
    arkrtos_board_puts("\n[Ark RTOS] board: qemu_riscv32 (virt, rv32imac, 1 hart)\n");
#endif
    arkrtos_board_puts("[Ark RTOS] arch=");
    arkrtos_board_puts(ARKRTOS_ARCH_NAME);
    arkrtos_board_puts(" clint=");
    arkrtos_board_puthex(ARKRTOS_PORT_CLINT_BASE);
    arkrtos_board_puts(" timebase=");
    arkrtos_board_putu(ARKRTOS_MACHINE_TIMER_HZ);
    arkrtos_board_puts("Hz\n");

    /* 追踪随板起来：环是静态的，不需要堆；档位由 MDK_TRACE_* 决定。
     * ARKRTOS_USE_MDK_TRACE=0 时 arkrtos_trace_init() 是空函数，这行代价为零。 */
    arkrtos_trace_init();
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
#if (ARKRTOS_DEMO_HWSAFE == 1)
static void hw_demo_dev_register(void);
#endif

void arkrtos_dev_board_init(void)
{
    if(arkrtos_board_uart_dev_register() != 0)
    {
        arkrtos_board_puts("[board] dev register ttyS0 FAILED\n");
    }
#if (ARKRTOS_DEMO_HWSAFE == 1)
    /* A5 读数装置：三台板内虚拟设备 + 驱动安全态回调（实现见文件末尾）。 */
    hw_demo_dev_register();
#endif
}
/* ============================================================
 * A5：遗言机制在「非 ARM」上的读数装置（ARKRTOS_DEMO_HWSAFE，默认关）
 * @details 遗言机制（ARKRTOS_HW_SAFE）与架构无关，但在此之前这块 QEMU 板上
 *          没有任何驱动登记安全态回调，整机 devs=0 —— 「机制在非 ARM 上也能在
 *          有设备的情况下工作」只有 UTEST 的合成回调作证，没有一次「真实设备
 *          + 真实任务死亡」的读数。
 *
 *          这里放三台**板内虚拟设备**（led0/led1/led2，纯 RAM 状态，不碰真
 *          外设），由驱动声明安全态：属主死亡时把 level 强制归 0 并计一次
 *          safe_hits。示例 main.c 的三个任务分别走三条通道：
 *            led0 —— 留遗言（will 通道）
 *            led1 —— 不留遗言（驱动回调通道）
 *            led2 —— 一直活着（台账里始终有一条所有者）
 *
 *          默认关：关着时本段不进二进制，板级行为与加入本装置之前逐位一致。
 * ============================================================ */
#if (ARKRTOS_DEMO_HWSAFE == 1)
#include "arkrtos_dev.h"
#include "arkrtos_hwsafe.h"

#define HW_DEMO_DEV_NUM   3u

typedef struct
{
    arkrtos_dev_hdr_t hdr;
    uint8             opened;
    uint32            level;   /* 0 = 安全态，非 0 = 通电（纯 RAM 状态） */
} hw_demo_dev_t;

static hw_demo_dev_t s_hw_demo[HW_DEMO_DEV_NUM];
static volatile uint32 s_hw_demo_safe_hits;

static arkrtos_dev_hdr_t *hw_demo_open(uint32 dev_id, uint32 param)
{
    (void)param;
    if(dev_id >= HW_DEMO_DEV_NUM)
    {
        return 0;
    }
    s_hw_demo[dev_id].hdr.block_size = sizeof(hw_demo_dev_t);
    s_hw_demo[dev_id].opened = 1u;
    s_hw_demo[dev_id].level  = 1u;   /* 打开即通电，与 STM32 的 led 演示同口径 */
    return &s_hw_demo[dev_id].hdr;
}

static int32 hw_demo_close(arkrtos_dev_hdr_t *obj)
{
    hw_demo_dev_t *p_dev = (hw_demo_dev_t *)obj;

    if((obj == 0) || (obj->block_size != sizeof(hw_demo_dev_t)))
    {
        return -1;
    }
    p_dev->opened = 0u;
    return 0;
}

static int32 hw_demo_ctrl(arkrtos_dev_hdr_t *obj, uint32 code, uint32 value)
{
    hw_demo_dev_t *p_dev = (hw_demo_dev_t *)obj;

    (void)value;
    if((obj == 0) || (obj->block_size != sizeof(hw_demo_dev_t)))
    {
        return -1;
    }
    /* 只认 0 = 关（安全态）、1 = 开；其余命令一律拒绝 —— 免得「遗言执行了
     * 但驱动其实什么都没做」看起来像成功。 */
    if(code > 1u)
    {
        return -1;
    }
    p_dev->level = code;
    return 0;
}

/* 安全态回调：属主死亡时把电平打到 0。非阻塞、不调 SVC、可重入（幂等）。 */
static int32 hw_demo_safe(arkrtos_dev_hdr_t *obj, uint32 owner)
{
    hw_demo_dev_t *p_dev = (hw_demo_dev_t *)obj;

    (void)owner;
    if((obj == 0) || (obj->block_size != sizeof(hw_demo_dev_t)))
    {
        return -1;
    }
    p_dev->level = 0u;
    s_hw_demo_safe_hits++;
    return 0;
}

static arkrtos_dev_drv_t s_hw_demo_drv =
{
    hw_demo_open,
    hw_demo_close,
    0,          /* read  */
    0,          /* write */
    hw_demo_ctrl
};

/* 把三台设备挂上设备表并声明安全态（由 arkrtos_dev_board_init() 调用）。 */
static void hw_demo_dev_register(void)
{
    (void)arkrtos_dev_register("led0", &s_hw_demo_drv, 0u);
    (void)arkrtos_dev_register("led1", &s_hw_demo_drv, 1u);
    (void)arkrtos_dev_register("led2", &s_hw_demo_drv, 2u);
    (void)arkrtos_hwsafe_register("led0", hw_demo_safe);
    (void)arkrtos_hwsafe_register("led1", hw_demo_safe);
    (void)arkrtos_hwsafe_register("led2", hw_demo_safe);
}

/* 示例读数用：驱动安全态回调真正被执行了多少次。内核的 neutralized 只记账，
 * 不证明驱动那段代码跑过，所以这里单独数一个。 */
uint32 arkrtos_board_hw_demo_safe_hits(void)
{
    return s_hw_demo_safe_hits;
}

/* 示例读数用：第 idx 台设备当前电平；idx 越界返回 0xFFFFFFFF。 */
uint32 arkrtos_board_hw_demo_level(uint32 idx)
{
    if(idx >= HW_DEMO_DEV_NUM)
    {
        return 0xFFFFFFFFu;
    }
    return s_hw_demo[idx].level;
}
#endif /* ARKRTOS_DEMO_HWSAFE */


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

    /* 故障现场实参。trap_entry 把 sp 存进 tp 当帧指针，故障派发只换 sp
     * （rv_fault_enter），tp 仍指向中断帧，所以帧里的 a0-a3/s3/sp0/ra0
     * 就是出事那条指令真正拿到的参数。只有 mepc 时只能知道"在哪个函数里"，
     * 有了这一行才知道"用哪个指针、哪个长度进来的"——例如
     * s3=0 又 mtval=0x4，就是 size=0 却仍解引用了 buffer。
     * 只在故障路径执行，不影响正常运行的非故障时序。 */
    {
        uint32 fp;
        __asm__ volatile("mv %0, tp" : "=r"(fp) : : "memory");
        if ((fp >= 0x80100000u) && (fp < 0x80200000u))
        {
            arkrtos_board_puts("[FRAME] a0=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[9]);
            arkrtos_board_puts(" a1=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[10]);
            arkrtos_board_puts(" a2=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[11]);
            arkrtos_board_puts(" a3=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[12]);
            arkrtos_board_puts(" s3=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[18]);
            arkrtos_board_puts(" sp0=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[1]);
            arkrtos_board_puts(" ra0=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[0]);
            arkrtos_board_puts(" s0=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[7]);
            arkrtos_board_puts(" s1=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[8]);
            arkrtos_board_puts(" s4=");
            arkrtos_board_puthex(((volatile uint32 *)fp)[19]);
            arkrtos_board_puts("\n");
        }
    }
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
