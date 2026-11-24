/**
* @file drvuart_line.c
* @brief QEMU `virt`（LoongArch64）板的第二路 ns16550a：软总线物理线的端口
* @details 板上的第一路 ns16550a 被内核控制台独占（ttyS0，drvuart.c）。软总线
*          要一根「真线」，于是用它后面的第二路：基址来自板级 Kconfig 的
*          ARKRTOS_QEMU_UART2_BASE（默认 0x1FE002E0）。
*
*          这个基址不是抄来的：QEMU loongarch virt 的机器设备树里直接列着
*          四路串口 —— 用 `-machine dumpdtb=D:/tmp/x.dtb` 导出后能看到
*          serial@1fe001e0 / serial@1fe002e0 / serial@1fe003e0 /
*          serial@1fe004e0 四个节点。本板只取第二路，其余两路不使用。
*
*          收发都是**轮询**，理由与控制台那一份相同：本移植没有接外部中断
*          控制器（见 drvuart.c 与 port 文件头），把轮询写成"看起来是中断"
*          只会给出假证据。软总线侧的 pump 每轮把 FIFO 抽干再喂给帧解析器，
*          QEMU 的 FIFO 深度（16 字节）小于一次 pump 的抽取量，正常流量不丢。
*
*          边界（必须一起读）：
*            1) 这条线只在 ARKRTOS_USE_BUS_UART=1 时参与编译；关档下本文件
*               是空翻译单元，镜像体积与行为逐位不变。
*            2) 发送上限是**迭代次数**，不是时间：发送器一直不 ready 就返回
*               负值并记 tx_timeout，绝不当成功。
*            3) QEMU 上波特率由 -chardev 后端决定，没有可写的分频器，所以
*               这里不碰 DLL/DLM —— 写了也是假动作。
* @author xw
*/

#include "arkrtos_features.h"
#include "arkrtos_types.h"

#if ((ARKRTOS_USE_BUS_UART == 1) && (ARKRTOS_USE_SOFTBUS == 1))

#include "arkrtos_config.h"
#include "ark_bus_line_port.h"
#include "ark_bus_port_arkrtos.h"   /* ark_bus_port_tick_ms */

/* ns16550a 寄存器（与控制台 drvuart.c 同一组定义口径，只是基址不同） */
#define U2_BASE             (ARKRTOS_QEMU_UART2_BASE)
#define U2_REG(i)           (*(volatile uint8 *)(U2_BASE + (uint32)(i)))

#define U2_RBR              0u
#define U2_THR              0u
#define U2_IER              1u
#define U2_FCR              2u
#define U2_LCR              3u
#define U2_MCR              4u
#define U2_LSR              5u

#define U2_LSR_DR           0x01u
#define U2_LSR_THRE         0x20u

/* 发送等待的两个上限，缺一不可：
 *
 *   1) ARK_BUS_LINE_TX_TIMEOUT_MS 是**主**判据，按整帧计时。理由是一次实测：
 *      QEMU 的 socket chardev 在 server 侧**没有对端连接**时，16550 的发送
 *      FIFO 排不空、THRE 一直是 0。若只按迭代次数上限，一帧 149 字节要在
 *      1e6 次 MMIO 读上磨十几秒，任务会被一根没接线的串口饿死 —— 这在
 *      工业 RTOS 里不可接受，所以必须按时间收手。取 250 ms 是为了将来真板：
 *      9600 bps 下 149 字节约 155 ms，留一倍余量。
 *   2) ARK_BUS_LINE_TX_SPINS 是**二级**保险，防「时钟自身不前进」（节拍没起
 *      来时 tick_ms 恒 0，只看时间就永远不会超时）。
 */
#ifndef ARK_BUS_LINE_TX_TIMEOUT_MS
#define ARK_BUS_LINE_TX_TIMEOUT_MS   250u
#endif

#ifndef ARK_BUS_LINE_TX_SPINS
#define ARK_BUS_LINE_TX_SPINS   1000000u
#endif

static uint32 s_tx_timeouts;

void ark_bus_line_uart_init(void)
{
    /* 顺序按 16550 的常规：先关中断、再设帧格式、最后开/清 FIFO。
     * 本驱动是轮询的，IER=0 是有意的上限：中断线没有接线，打开只会让
     * 「为什么没进中断」变成一个假问题。 */
    U2_REG(U2_IER) = 0x00u;
    U2_REG(U2_LCR) = 0x03u;   /* 8 数据位，无校验，1 停止位 */
    U2_REG(U2_FCR) = 0x07u;   /* 使能 FIFO + 清发送/接收队列 */
    U2_REG(U2_MCR) = 0x03u;   /* DTR / RTS 置位（对端是 socket，仅表意） */
    s_tx_timeouts = 0u;
}

int ark_bus_line_uart_write(const uint8 *data, uint16 len)
{
    uint16 i;
    uint32 t0;
    uint32 spins = 0u;

    if(data == 0)
    {
        return -1;
    }

    /* 计时从"整帧开始"算，不是每字节各给一份预算：否则 N 字节的预算会变成
     * N 倍，一根断线仍然是慢动作。 */
    t0 = ark_bus_port_tick_ms();

    for(i = 0u; i < len; i++)
    {
        while((U2_REG(U2_LSR) & U2_LSR_THRE) == 0u)
        {
            spins++;
            if((spins > ARK_BUS_LINE_TX_SPINS) ||
               ((ark_bus_port_tick_ms() - t0) >= ARK_BUS_LINE_TX_TIMEOUT_MS))
            {
                s_tx_timeouts++;
                /* 已经发出去的部分如实报出：调用方据此知道草稿到哪一段。 */
                return (i > 0u) ? (int)i : -1;
            }
        }
        U2_REG(U2_THR) = data[i];
    }

    return (int)len;
}

int ark_bus_line_uart_read(uint8 *buf, uint16 max_len)
{
    uint16 n = 0u;

    if(buf == 0)
    {
        return 0;
    }
    while((n < max_len) && ((U2_REG(U2_LSR) & U2_LSR_DR) != 0u))
    {
        buf[n] = U2_REG(U2_RBR);
        n++;
    }

    return (int)n;
}

uint32 ark_bus_line_uart_tx_timeouts(void)
{
    return s_tx_timeouts;
}

#endif /* ARKRTOS_USE_BUS_UART && ARKRTOS_USE_SOFTBUS */
