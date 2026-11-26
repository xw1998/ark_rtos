/**
* @file arkrtos_board.c
* @brief Ark RTOS 板级实现 - QEMU `virt` 机器（LoongArch64，单核）
* @details 这一份板级只做三件事：
*          1) 把 ns16550a UART 配成 8N1 并给出轮询输出；
*          2) 覆盖 port 层的弱板级钩子（开机横幅、致命异常报告）；
*          3) 提供 Flash / 看门狗的空桩 —— 注意是「明确返回失败」的空桩，
*             不是假装成功的桩：QEMU 上没有片内 Flash，也没有 IWDG，
*             声称擦写成功会让上层以为它真写进去了。
*             唯一的例外是**镜像池那一段**：它用 RAM 模拟 Flash（擦除写
*             0xFF、编程按字节覆盖，见下面的 arkrtos_qemu_pool_*），因为
*             池内安装这条路要真跑才能验证；池以外（内核代码区、配置区）
*             仍然一律明确失败。
*
* @note 异常入口（EENTRY）与 CRMD.IE 的配置留在 port 层的默认实现里：
*       它是 LoongArch 架构的固定动作，不是板级知识。
* @author xw
*/

#include "arkrtos_hal.h"
#include "arkrtos_config.h"
#include "arkrtos_board.h"
#include "arkrtos_trace.h"     /* arkrtos_trace_init()：内核事件追踪 */
#if (ARKRTOS_USE_DEVICE_ID == 1)
#include "arkrtos_devid.h"
#endif

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
 * 镜像池的 Flash 模拟（只覆盖池那一段）
 * @details 本板的 "Flash"（CHIP_FLASH_BASE 起 1 MB）与 DRAM 是同一片内存
 *          的两段，QEMU 按普通 RAM 映射。真 Flash 空白态是 0xFF，DRAM 上电
 *          是 0x00 —— 差这一条，装载器的 find_clean（只认 0xFF 连续段）在
 *          模拟器上永远找不到已擦除空间，池内安装恒为 ERR_NOSPACE，于是
 *          「安装 → 落盘形态 → 开机 CRC 复核」这条链路一次都没在 QEMU 上
 *          真跑过（只有 RAM 里的等价构造能测）。
 *
 *          语义边界（写清楚是为了别被读成「QEMU 有 Flash 了」）：
 *          ① 只有 [IMAGE_POOL_BASE, IMAGE_POOL_END) 可擦可写，池以外一律
 *             返回 -1 —— 内核正在代码区跑，允许写等于允许自己改坏自己。
 *          ② 擦除不做「必须先擦后写」的位检查：真 Flash 把 0 写回 1 会失败，
 *             这里不模拟这一点（上层自己按擦—写顺序调用）。
 *          ③ RAM 不保持：每次复位都会把整池重新擦成 0xFF，所以「复位后镜像
 *             还在不在」只有真机才验证得到。
 * ============================================================ */
#define QEMU_POOL_FLASH_BEGIN   (IMAGE_POOL_BASE)
#define QEMU_POOL_FLASH_END     (IMAGE_POOL_END)

/* 区间校验：0 = 落在池内且不越界/不回绕 */
static int32 arkrtos_qemu_pool_range(uint32 addr, uint32 size)
{
    if(size == 0u)
    {
        return -1;
    }
    if(addr < QEMU_POOL_FLASH_BEGIN)
    {
        return -1;
    }
    if((addr + size) < addr)                  /* 32 位回绕 */
    {
        return -1;
    }
    if((addr + size) > QEMU_POOL_FLASH_END)
    {
        return -1;
    }
    return 0;
}

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    volatile uint8 *p = (volatile uint8 *)(unsigned long)addr;
    uint32 i;

    if(arkrtos_qemu_pool_range(addr, size) != 0)
    {
        return -1;
    }

    for(i = 0u; i < size; i++)
    {
        p[i] = 0xFFu;
    }
    return 0;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    volatile uint8 *p = (volatile uint8 *)(unsigned long)addr;
    uint32 i;

    if(data == 0)
    {
        return -1;
    }
    if(arkrtos_qemu_pool_range(addr, len) != 0)
    {
        return -1;
    }

    for(i = 0u; i < len; i++)
    {
        p[i] = data[i];
    }
    return 0;
}

/* 链接脚本给出的镜像末尾（link.ld.in: __text_end = .）。 */
extern uint8 __text_end[];

/* 池的物理擦除单位（与分区头的 IMAGE_POOL_SECTOR 同源），池外报 0（无扇区） */
uint32 arkrtos_port_flash_sector_size(uint32 addr)
{
    if((addr >= QEMU_POOL_FLASH_BEGIN) && (addr < QEMU_POOL_FLASH_END))
    {
        return (uint32)IMAGE_POOL_SECTOR;
    }
    return 0u;
}

/* ============================================================
 * 元件自检钩子（SEC-G14）
 * @details 只读镜像区间取「分区首地址 .. __text_end」：链接脚本把 .text、
 *          .rodata 与需要常驻 Flash 的只读表都放在这一段，__text_end 之后
 *          是 RAM 与镜像池，不属于「出厂就该一直不变的字节」。
 *          第二路计数器用 rdtime（恒频 100 MHz，与节拍定时器是两个计数源）：
 *          它能发现「节拍停了 / 定时器时钟跑飞」，但两路同源时看不出晶振
 *          本身的偏差——这一条写在已知限制里。
 * ============================================================ */
#if (ARKRTOS_USE_SELFTEST == 1)
int32 arkrtos_port_selftest_flash_span(uint32 *start, uint32 *len)
{
    uint32 end = (uint32)(unsigned long)__text_end;

    if((start == (uint32 *)0) || (len == (uint32 *)0))
    {
        return -1;
    }
    if(end <= (uint32)CHIP_FLASH_BASE)
    {
        return -1;
    }
    *start = (uint32)CHIP_FLASH_BASE;
    *len   = end - (uint32)CHIP_FLASH_BASE;
    return 0;
}

int32 arkrtos_port_selftest_clock(uint32 *now)
{
    if(now == (uint32 *)0)
    {
        return -1;
    }
    *now = arkrtos_port_get_timer_counter();
    return 0;
}
#endif /* ARKRTOS_USE_SELFTEST */

/* ============================================================
 * 板级初始化（覆盖 port 层的弱实现）
 * ============================================================ */
void arkrtos_port_board_init(void)
{
    arkrtos_board_console_init();

#if (ARKRTOS_USE_SMP == 1) && (ARKRTOS_CPU_NUM > 1)
    /* SMP 镜像不许再自称 1 core：这条自述行是上板后第一个读数。 */
    arkrtos_board_puts("\n[Ark RTOS] board: qemu_loongarch64 (virt, la64, ");
    arkrtos_board_putu((uint32)ARKRTOS_CPU_NUM);
    arkrtos_board_puts(" cores)\n");
#else
    arkrtos_board_puts("\n[Ark RTOS] board: qemu_loongarch64 (virt, la64, 1 core)\n");
#endif
    arkrtos_board_puts("[Ark RTOS] arch=");
    arkrtos_board_puts(ARKRTOS_ARCH_NAME);
    arkrtos_board_puts(" uart=");
    arkrtos_board_puthex(ARKRTOS_QEMU_UART_BASE);
    arkrtos_board_puts(" timer=");
    arkrtos_board_putu(ARKRTOS_PORT_TIMER_HZ);
    arkrtos_board_puts("Hz rdtime=");
    arkrtos_board_putu(ARKRTOS_PORT_RDTIME_HZ);
    arkrtos_board_puts("Hz\n");

    /* 镜像池上电态：擦成 0xFF。真 Flash 空白就是 0xFF，而 QEMU 这片 DRAM
     * 上电是 0x00 —— 不抹一次，池内安装在模拟器上永远找不到「干净空间」。
     * 每次复位都重来（RAM 不保持），这是本板口径的一部分，不是缺陷。
     *
     * 但擦之前必须先核一次镜像有没有越过池基址：内核和池是同一片 DRAM，
     * 越界时这次擦除会把内核自己的尾巴抹成 0xFF，症状是串口从某个地址起
     * 狂刷 0xFF、PC 卡在打印循环里 —— 看上去像装载器坏了，实际是布局不够。
     * 链接脚本已经用 ASSERT 把这条线卡成编译期错误；这里是第二道闸：
     * 真越界就**不擦**、明确报 FATAL，把池判为不可用（宁可报错，也不给
     * 一个「能启动但其实已经自毁」的错答案）。 */
    if((uint32)(unsigned long)__text_end > (uint32)IMAGE_POOL_BASE)
    {
        arkrtos_board_puts("[Ark RTOS] FATAL: kernel image ends at 0x");
        arkrtos_board_puthex((uint32)(unsigned long)__text_end);
        arkrtos_board_puts(" > image pool base 0x");
        arkrtos_board_puthex((uint32)IMAGE_POOL_BASE);
        arkrtos_board_puts("\n           pool left untouched (unusable); raise KERNEL_SIZE\n");
    }
    else
    {
        (void)arkrtos_port_flash_erase((uint32)QEMU_POOL_FLASH_BEGIN,
                                       (uint32)(QEMU_POOL_FLASH_END -
                                                QEMU_POOL_FLASH_BEGIN));
    }

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


/* 64 位十六进制打印：arkrtos_board_puthex() 只吃 uint32，而 LA64 的
 * ERA/BADV/BADI 都是 64 位 CSR。以前这里直接用 uint32 接，高位被砍掉，
 * 曾经让人把「0x00000000_0025229b」读成内核镜像里的地址（宽 VA 档的 App
 * 窗口本来就可能落在高位）。高位为零时输出与旧口径逐字相同。 */
static void la_board_puthex64(unsigned long v)
{
    static const char hexd[] = "0123456789abcdef";
    char buf[17];
    uint32 i;
    uint32 n = 0u;
    int started = 0;

    for(i = 0u; i < 16u; i++)
    {
        uint32 nib = (uint32)((v >> ((15u - i) * 4u)) & 0xFu);

        if((started == 0) && (nib == 0u) && (i < 15u))
        {
            continue;
        }
        started = 1;
        buf[n] = hexd[nib];
        n++;
    }
    buf[n] = '\0';
    arkrtos_board_puts(buf);
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
    unsigned long era;
    unsigned long crmd;
    unsigned long ecode;

    (void)code;

    __asm__ volatile("csrrd %0, 0x6" : "=r"(era)  : : "memory");
    __asm__ volatile("csrrd %0, 0x0" : "=r"(crmd) : : "memory");

    /* code 传进来的是 ESTAT 原值（中断源）或 Ecode 已清零的值；
     * 这里重新读一次 ESTAT 打全，避免两处口径不一致。 */
    __asm__ volatile("csrrd %0, 0x5" : "=r"(ecode) : : "memory");

    /* BADV（CSR 0x7）是出错地址、BADI（0x8）是出错指令，和 ARM 的 BFAR 同位：
     * 没有它们只能知道"在哪条指令出事"，有了才知道"碰的是哪个地址"。 */
    {
        unsigned long badv;
        unsigned long badi;

        __asm__ volatile("csrrd %0, 0x7" : "=r"(badv) : : "memory");
        __asm__ volatile("csrrd %0, 0x8" : "=r"(badi) : : "memory");
        arkrtos_board_puts("\n[Ark RTOS] FATAL: estat=");
        la_board_puthex64(ecode);
        arkrtos_board_puts(" ecode=");
        arkrtos_board_puthex((uint32)((ecode >> 16) & 0x3Fu));
        arkrtos_board_puts(" era=");
        la_board_puthex64(era);
        arkrtos_board_puts(" badv=");
        la_board_puthex64(badv);
        arkrtos_board_puts(" badi=");
        la_board_puthex64(badi);
        arkrtos_board_puts(" crmd=");
        la_board_puthex64(crmd);
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

/* ------------------------------------------------------------------
 * 设备身份（SEC-G13）的板级钩子
 *
 * QEMU virt 没有熔丝阵列：这里没有唯一号可读，也没有 OTP 密钥槽。但"没有"
 * 本身值得在这台模拟器上演示一次——本函数给一个**固定的、标注为模拟的**
 * uid（"qemu-la64" 的 9 字节），于是：
 *   - 派生出来的 ID 每次开机都一样，`devid` 命令能打印出来，脚本可以比；
 *   - 没有密钥，所以 ready=0、`devid prove ...` 会明确回"拒绝"而不是吐全零 MAC。
 * 换句话说，L3 上跑通的是**失败关闭**那一条，不是"身份成立"。真实身份要
 * 板子把 arkrtos_port_devid_uid()/arkrtos_port_devid_secret() 接到 OTP 上。
 * ------------------------------------------------------------------ */
#if (ARKRTOS_USE_DEVICE_ID == 1)
int32 arkrtos_port_devid_uid(uint8 *p_out, uint32 cap, uint32 *p_len)
{
    static const uint8 s_qemu_uid[9] = { 'q', 'e', 'm', 'u', '-', 'l', 'a', '6', '4' };
    uint32 i;

    if((p_out == 0) || (p_len == 0))
    {
        return -1;
    }
    if(cap < (uint32)sizeof(s_qemu_uid))
    {
        *p_len = (uint32)sizeof(s_qemu_uid);
        return 0;
    }
    for(i = 0u; i < (uint32)sizeof(s_qemu_uid); i++)
    {
        p_out[i] = s_qemu_uid[i];
    }
    *p_len = (uint32)sizeof(s_qemu_uid);
    return 0;
}
#endif /* ARKRTOS_USE_DEVICE_ID */
