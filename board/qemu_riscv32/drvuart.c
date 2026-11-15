/**
* @file drvuart.c
* @brief QEMU `virt`（RISC-V RV32）板级控制台设备：把 16550 注册成 ttyS0
* @details 为什么需要这一层：内核 Shell（ark_shell）不直接用板级打印函数，
*          它按「设备」打开控制台 —— 名字取 `SHELL_DEV_NAME`（默认 ttyS0），
*          param 是波特率，收发都走 arkrtos_dev_read/write_internal。所以
*          板级必须把 UART 注册进设备框架，否则 shell 起来只会报「打不开」。
*
*          收发两条路径刻意不同：
*            - **发送**用轮询（等 THRE 再写），因为 shell 是行模式、一次几十
*              字节，轮询最省事且不会与接收中断抢同一份状态；
*            - **接收**用中断（PLIC + 16550 IER.RDA），字节落进 256 B 环形缓冲，
*              由 arkrtos_board_uart_rx_read() 取走 —— 同一份缓冲同时供示例的
*              task_rx 与 shell 用，谁先取谁得，没有第二份影子状态。
*
*          另带一个板级复位实现（arkrtos_board_reboot）：QEMU virt 提供
*          sifive_test 设备，写 0x7777 即整机复位。Cortex-M 那边的 AIRCR
*          写法在这里没有任何意义（0xE000ED0C 在 RV32 上没有映射，写它会
*          store fault），所以复位必须由板级接管。
*
* @author xw
*/

#include "arkrtos_features.h"
#include "arkrtos_board.h"

/* QEMU virt 的 sifive_test 设备：写 TEST_PASS(0x5555) 关机，
 * 写 TEST_RESET(0x7777) 复位。地址是 virt 机器的固定映射。
 * 复位不属于 shell（见文件末尾），所以这两句放在守卫外面。 */
#define ARKRTOS_QEMU_TEST_BASE    0x00100000u
#define ARKRTOS_QEMU_TEST_RESET   0x7777u

#if (SHELL_ENABLE == 1)

#include "arkrtos_config.h"
#include "arkrtos_dev.h"
#include "arkrtos_partition.h"      /* SHELL_DEV_NAME */

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

    /* 打开的语义是「可以用」，不是「重新初始化硬件」：控制台从复位起就在
     * 跑，重复初始化会把已经收到的字节和 PLIC 使能位一起抹掉。 */
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

static int32 uart_drv_read(arkrtos_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    if((p == 0) || (p->block_size != sizeof(qemu_uart_dev_t)))
    {
        return ARKRTOS_DEV_ERR_HANDLE;
    }
    if((pdata == 0) || (len <= 0))
    {
        return 0;
    }

    return (int32)arkrtos_board_uart_rx_read(pdata, (uint32)len);
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

    /* 波特率在 QEMU 上由 -serial 后端决定，写它没有任何作用。返回 -1 而不是 0：
     * "改成功了" 是假答案，这个平台根本没有可改的波特率分频器。 */
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

#else  /* SHELL_ENABLE == 0：没有 shell 就不注册控制台设备。但复位仍然要有 ——
        * 复位是板级能力，不是 shell 的附属品。 */

int32 arkrtos_board_uart_dev_register(void)
{
    return 0;   /* 不占用 ttyS0 这个名字：注册「成功」但没有任何设备被挂上 */
}

#endif /* SHELL_ENABLE */

void arkrtos_board_reboot(void)
{
    *(volatile uint32 *)ARKRTOS_QEMU_TEST_BASE = ARKRTOS_QEMU_TEST_RESET;

    for(;;)
    {
    }
}
