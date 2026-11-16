/**
* @file drvuart.c
* @brief QEMU `virt`（LoongArch64）板级控制台设备：把 ns16550a 注册成 ttyS0
* @details 为什么需要这一层：内核 Shell（ark_shell）不直接用板级打印函数，
*          它按「设备」打开控制台 —— 名字取 `SHELL_DEV_NAME`（默认 ttyS0），
*          收发都走 arkrtos_dev_read/write_internal。板级因此必须把 UART
*          注册进设备框架，否则 shell 起来只会报「打不开」。
*
*          收发都是**轮询**，与 RISC-V 那边的「中断收」刻意不同：
*          LA64 这个移植还没有把 PLIC/外部中断接起来（见 port 文件头，
*          本版只用了定时器这一个内部中断源）。信号量驱动的「中断收」
*          在没有外部中断控制器的前提下无从谈起，硬写成"看起来是中断"
*          只会给出假证据。轮询的代价是 shell 任务每 SHELL_TASK_PERIOD_MS
*          问一次 FIFO，收益是这条链路每一段都真的被执行过。
*
* @note 复位：QEMU 的 loongarch `virt` 没有 sifive_test 那种"写一个魔数
*       就整机复位"的设备（那是 RISC-V virt 的约定）。所以 shell 的
*       `reboot` 在这里**做不到**，本实现明确打印一句说明后停机 —— 假装
*       复位成功比拒绝更糟。
* @author xw
*/

#include "arkrtos_features.h"
#include "arkrtos_board.h"

#if (SHELL_ENABLE == 1)

#include "arkrtos_config.h"
#include "arkrtos_dev.h"
#include "arkrtos_partition.h"      /* SHELL_DEV_NAME */

/* ns16550a 寄存器（与 arkrtos_board.c 的 console 用同一组定义口径） */
#define UART_BASE           (ARKRTOS_QEMU_UART_BASE)
#define UART_REG(i)         (*(volatile uint8 *)(UART_BASE + (uint32)(i)))

#define UART_RBR            0u   /* 读：接收缓冲 */
#define UART_LSR            5u   /* 线路状态：bit0=DR 收到字节、bit5=THRE 可发 */
#define UART_LSR_DR         0x01u

typedef struct
{
    arkrtos_dev_hdr_t hdr;
    uint8             opened;
    uint32            baud;
} qemu_uart_dev_t;

static qemu_uart_dev_t s_uart_dev;

static arkrtos_dev_hdr_t *uart_drv_open(uint32 dev_id, uint32 param)
{
    (void)dev_id;

    s_uart_dev.hdr.block_size = sizeof(qemu_uart_dev_t);
    s_uart_dev.baud           = param;
    s_uart_dev.opened         = 1u;

    /* 打开的语义是「可以用」，不是「重新初始化硬件」：控制台从复位起
     * 就在跑，重复初始化会把已经收到的字节连同 FIFO 一起清掉。 */
    return &s_uart_dev.hdr;
}

static int32 uart_drv_close(arkrtos_dev_hdr_t *p)
{
    if((p == 0) || (p->block_size != sizeof(qemu_uart_dev_t)))
    {
        return ARKRTOS_DEV_ERR_HANDLE;
    }

    s_uart_dev.opened = 0u;

    return 0;
}

/* 非阻塞：FIFO 里有字节就取（最多 len 个），空就返回 0。
 * platform_uart_recv() 依赖这个契约（返回 0 = 这次没有，不是错误）。 */
static int32 uart_drv_read(arkrtos_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    int32 n = 0;

    if((p == 0) || (p->block_size != sizeof(qemu_uart_dev_t)))
    {
        return ARKRTOS_DEV_ERR_HANDLE;
    }
    if((pdata == 0) || (len <= 0))
    {
        return 0;
    }

    while((n < len) && ((UART_REG(UART_LSR) & UART_LSR_DR) != 0u))
    {
        pdata[n] = UART_REG(UART_RBR);
        n++;
    }

    return n;
}

static int32 uart_drv_write(arkrtos_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    int32 i;

    if((p == 0) || (p->block_size != sizeof(qemu_uart_dev_t)))
    {
        return ARKRTOS_DEV_ERR_HANDLE;
    }
    if((pdata == 0) || (len <= 0))
    {
        return 0;
    }

    for(i = 0; i < len; i++)
    {
        arkrtos_board_putc((char)pdata[i]);
    }

    return len;
}

static int32 uart_drv_ctrl(arkrtos_dev_hdr_t *p, uint32 code, uint32 value)
{
    (void)code;
    (void)value;

    if((p == 0) || (p->block_size != sizeof(qemu_uart_dev_t)))
    {
        return ARKRTOS_DEV_ERR_HANDLE;
    }

    /* QEMU 上波特率由 -serial 后端决定，没有可写的分频器：返回 -1 而不是 0，
     * 因为 "改成功了" 是假答案。 */
    return ARKRTOS_DEV_ERR_NOIMPL;
}

static arkrtos_dev_drv_t s_uart_drv =
{
    uart_drv_open,
    uart_drv_close,
    uart_drv_read,
    uart_drv_write,
    uart_drv_ctrl
};

int32 arkrtos_board_uart_dev_register(void)
{
    return arkrtos_dev_register(SHELL_DEV_NAME, &s_uart_drv, 1u);
}

#else  /* SHELL_ENABLE == 0：没有 shell 就不注册控制台设备 */

int32 arkrtos_board_uart_dev_register(void)
{
    return 0;   /* 不占用 ttyS0 这个名字 */
}

#endif /* SHELL_ENABLE */

void arkrtos_board_reboot(void)
{
    arkrtos_board_puts("[board] reboot: qemu-loongarch64 virt has no reset device, "
                       "stop here instead of pretending\n");

    for(;;)
    {
    }
}
