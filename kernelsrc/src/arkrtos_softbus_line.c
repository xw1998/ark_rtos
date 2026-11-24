/**
* @file arkrtos_softbus_line.c
* @brief 软总线物理线：把总线节点接到第二路 UART（默认关）
* @details 本文件只有三件事：
*
*            1) 把组件的 UART 传输层（transport/transport_uart.c）接到板上
*               的线端口（port/ark_bus_line_port.h，QEMU 上是 drvuart_line.c）；
*            2) 每轮 pump 先**抽干接收 FIFO** 再喂帧解析器，最后 poll 一轮
*               总线 —— 顺序如此是因为 ark_bus_poll 只在调用时刻取一帧，
*               先喂后 poll 才能在同一轮里把「刚到的字节」变成「刚处理的帧」；
*            3) 给用例/控制台一层薄封装（订阅 / 定向发布 / RPC / 统计）。
*
*          为什么抽干而不是每轮读一字节：QEMU 的 16550 接收 FIFO 深 16 字节，
*          一帧最长 149 字节，若每轮只取一个字节，poll 频率必须比线上字节率
*          高一个数量级才不丢。抽干（read 返回 0 为止）把这条约束消掉了，
*          解析器自己按帧同步，多喂的字节不会越界。
*
*          **不在这里做的事**（写清楚是为了防止后人把策略埋进内核）：
*            - 不定心跳周期、不决定什么时候发布：那是用例的事；
*            - 不做重连/复位序列：QEMU 的线是内存 FIFO，没有"拔插"事件；
*              真板接入时要在这里补（欠账，见 docs/软总线接入.md）。
*
* @note 只在 ARKRTOS_USE_SOFTBUS=1 且 ARKRTOS_USE_BUS_UART=1 时参与编译。
* @author xw
*/

#include "arkrtos_softbus_line.h"

#if ((ARKRTOS_USE_SOFTBUS == 1) && (ARKRTOS_USE_BUS_UART == 1))

#include <string.h>

#include "ark_bus_transport.h"
#include "ark_bus_line_port.h"
#include "ark_bus_port_arkrtos.h"

/* 一次从 FIFO 抽多少。取 64 而不是 frame 最大长度：rounds 之间会反复抽，
 * 小缓冲能把「抽干」这件事做得更平滑，也不占静态 RAM。 */
#define SB_LINE_RX_CHUNK     64u

/* 单轮最多抽几次。正常情况下 read 返回 0 就退出了，这个上限只为一种异常
 * 兜底：后端把 LSR.DR 钉在 1 却读不出新字节（例如串口线悬空时的某些实现）。
 * 没有它，「抽干」就会变成"永远抽不完"，任务被饥饿到不再检查任何判据 ——
 * 这不是假设，solo 门禁第一版就卡在这里。32 * 64 = 2048 字节/轮，远大于
 * 一帧最长 149 字节，不改变正常路径的行为。 */
#define SB_LINE_DRAIN_LIMIT  32u

static ark_bus_t                g_line;
static ark_bus_transport_uart_t g_lu;
static transport_adapter_t      g_ltp;

static uint32 g_up;
static uint32 g_node_id;
static uint32 g_rx_bytes;   /* 从 UART 收到的裸字节（含被解析器丢掉的） */

/* 组件的写钩子签名带 ctx 参数，板上端口不关心它，这里抹平。 */
static int sb_line_uart_write(void *io, const uint8_t *data, uint16_t len)
{
    (void)io;
    return ark_bus_line_uart_write(data, len);
}

int32 arkrtos_softbus_line_init(uint32 node_id, uint32 node_type)
{
    if(node_id == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_PARAM;
    }

    /* 板级三钩子（毫秒 / 临界区）与线端口分开装：前三个是内核本体，
     * 后三个是板上 UART，混在一起会让「换板」变成改两处。 */
    ark_bus_port_arkrtos_install();
    ark_bus_line_uart_init();

    ark_bus_transport_uart_init(&g_lu, &g_ltp, NULL, sb_line_uart_write);
    ark_bus_init(&g_line, &g_ltp);

    g_up = 0u;
    g_node_id = node_id;
    g_rx_bytes = 0u;

    if(ark_bus_start(&g_line, node_id, (uint8_t)node_type,
                     ARK_CAP_MCU_EDGE) < 0)
    {
        return ARKRTOS_SOFTBUS_ERR_INIT;
    }

    g_up = 1u;
    /* 启动即广播一次 DISCOVER：对端在起来之前发的字节已经烂在 FIFO 里，
     * 这一发让「先起的一侧」不必等下一次心跳（默认 3 s）才被看见。 */
    (void)arkrtos_softbus_line_pump(1u);
    return 0;
}

int32 arkrtos_softbus_line_pump(uint32 rounds)
{
    uint32 r;
    int    n;

    if(g_up == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_NOTUP;
    }
    if(rounds == 0u)
    {
        rounds = 1u;
    }

    for(r = 0u; r < rounds; r++)
    {
        uint8_t chunk[SB_LINE_RX_CHUNK];
        uint32  drains = 0u;

        /* 抽干：read 返回 0 就说明 FIFO 空了，没有任何"再等一会儿"。
         * drains 上限见 SB_LINE_DRAIN_LIMIT 的注释。 */
        for(;;)
        {
            int i;

            n = ark_bus_line_uart_read(chunk, (uint16_t)sizeof(chunk));
            if(n <= 0)
            {
                break;
            }
            g_rx_bytes += (uint32)n;
            for(i = 0; i < n; i++)
            {
                ark_bus_transport_uart_feed_byte(&g_ltp, chunk[i]);
            }
            if(++drains >= SB_LINE_DRAIN_LIMIT)
            {
                break;
            }
        }
        ark_bus_poll(&g_line);
    }

    return 0;
}

int32 arkrtos_softbus_line_subscribe(const char *topic, ark_bus_msg_cb_t cb,
                                     void *user)
{
    if(g_up == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_NOTUP;
    }
    return ark_bus_subscribe(&g_line, topic, cb, user);
}

int32 arkrtos_softbus_line_publish(uint32 target_id, const char *topic,
                                   const void *payload, uint16 len)
{
    if(g_up == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_NOTUP;
    }
    return ark_bus_publish_to(&g_line, target_id, topic, payload, len);
}

int32 arkrtos_softbus_line_rpc(uint32 target_id, uint16 method,
                               const void *params, uint16 len,
                               ark_bus_rpc_cb_t cb, void *user)
{
    if(g_up == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_NOTUP;
    }
    return ark_bus_rpc_request(&g_line, target_id, method, params, len, cb, user);
}

int32 arkrtos_softbus_line_rpc_register(uint16 method,
                                        ark_bus_rpc_handler_t handler,
                                        void *user)
{
    if(g_up == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_NOTUP;
    }
    return ark_bus_rpc_register(&g_line, method, handler, user);
}

int32 arkrtos_softbus_line_probe(uint32 lines)
{
    static const char banner[] = "ARK-BUS-LINE\r\n";
    uint32 i;
    int    total = 0;

    if(lines == 0u)
    {
        lines = 1u;
    }
    for(i = 0u; i < lines; i++)
    {
        int n = ark_bus_line_uart_write((const uint8_t *)banner,
                                        (uint16_t)(sizeof(banner) - 1u));

        /* 一行没写完就是没写完：短写不许当作"这一行发完了"接着数下一行，
         * 否则同一个半截行会被重复计入总数（fail-at-5 两行会报 10 字节）。 */
        if(n < (int)(sizeof(banner) - 1u))
        {
            if(n > 0)
            {
                total += n;
            }
            return (total > 0) ? (int32)total : (int32)((n < 0) ? n : -1);
        }
        total += n;
    }
    return (int32)total;
}

uint32 arkrtos_softbus_line_node_id(void)
{
    return g_node_id;
}

int32 arkrtos_softbus_line_stats(arkrtos_softbus_line_stats_t *out)
{
    const ark_bus_stats_t *st;

    if(out == NULL)
    {
        return ARKRTOS_SOFTBUS_ERR_PARAM;
    }
    if(g_up == 0u)
    {
        return ARKRTOS_SOFTBUS_ERR_NOTUP;
    }

    (void)memset(out, 0, sizeof(*out));
    st = ark_bus_get_stats(&g_line);
    if(st != NULL)
    {
        out->tx_frames  = st->tx_frames;
        out->tx_bytes   = st->tx_bytes;
        out->rx_frames  = st->rx_frames;
        out->rx_bytes   = st->rx_bytes;
        out->rx_crc_err = st->rx_crc_errors;
        out->rx_bad     = st->rx_bad_frames;
        out->retrans    = st->qos_retransmits;
        out->timeouts   = st->qos_timeouts;
    }
    /* rx_dropped 取帧解析器自己的账（重同步丢掉的字节），rx_bytes 用本层
     * 从 UART 抽到的裸字节 —— 两个口径都在控制台上，谁大谁小能直接看出是
     * 「线上有噪声」还是「对端根本没发」。 */
    out->rx_dropped      = g_lu.dropped;
    out->node_id         = g_node_id;
    out->uart_tx_timeout = ark_bus_line_uart_tx_timeouts();
    if(g_rx_bytes > out->rx_bytes)
    {
        out->rx_bytes = g_rx_bytes;
    }
    return 0;
}

#endif /* ARKRTOS_USE_SOFTBUS && ARKRTOS_USE_BUS_UART */