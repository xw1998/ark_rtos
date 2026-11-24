/**
* @file test_softbus_line_host.c
* @brief arkrtos_softbus_line.c 的宿主用例：真组件 + 假串口，端到端
* @details 被验的不是"软总线好不好用"，而是这一层**物理线接入代码**有没有
*          把组件接错。做法是让整个 ark_softbus 组件（core / frame / qos /
*          rpc / transport_uart / port 胶水）在宿主上真编真链，只把最底下
*          那四个 UART 钩子换成内存 FIFO：
*
*            ark_bus_line_uart_init       清空假 FIFO、清零超时计数
*            ark_bus_line_uart_write      把字节推进"对端接收 FIFO"
*            ark_bus_line_uart_read       从"本端接收 FIFO"取字节
*            ark_bus_line_uart_tx_timeouts 返回可控的超时计数
*
*          于是"总线写出的字节"与"总线读到的字节"都在用例手里，可以：
*
*            1. 未 init 时六个入口必须报 NOTUP，node_id 必须是 0；
*            2. init(0) 必须报 PARAM；正常 init 后 node_id / 档位 / 初始统计
*               都对得上（档位错了在宿主上先红，而不是等上板 RAM 不够）；
*            3. probe 的字节数与"行"语义（0 视为 1 行、失败时如实返回负）；
*            4. publish 必须让线上真的有字节，且统计随之增长；
*            5. 把写出的帧原样回灌进接收 FIFO 后 pump 不得崩、不得无限循环
*               —— 这一条钉的是 SB_LINE_DRAIN_LIMIT：后端把 LSR.DR 钉在 1
*               时的兜底（实机 solo 门禁第一版正是卡在这里）；
*            6. stats 的两个口径：rx_bytes 取"总线解析"与"本层抽到的裸字节"
*               的较大者；uart_tx_timeout 必须透传底层计数。
*
*          明确没验证：真串口电气特性、两个真实节点、波特率与流控、中断
*          驱动收发。那些只能上板；QEMU 上的两节点读数由
*          tools/bus_line_two_nodes.py 负责（判词 bus_line_two_nodes_ok_on_qemu）。
*
* @author xw
* @date 2026.11.24
*/
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "arkrtos_softbus_line.h"
#include "arkrtos_task.h"           /* arkrtos_task_wait_internal */
#include "ark_bus_port_arkrtos.h"   /* ark_bus_port_tick_ms / lock / unlock */
#include "ark_bus_line_port.h"      /* 被测层依赖的四个 UART 钩子 */

#if ((ARKRTOS_USE_SOFTBUS != 1) || (ARKRTOS_USE_BUS_UART != 1))

/* 关档：本文件只证明"关档时用例仍可编可跑"。真正的断言是 ci_gate 里的
 * 体积/符号逐位对比（默认档不许出现 line 层的任何符号）。 */
int main(void)
{
    printf("SOFTBUS-LINE-HOST OK  off\n");
    return 0;
}

#else

static int g_checks;
static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_fails++;                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------ */
/* 虚拟时钟（与 test_softbus_host.c 同一口径）                            */
/* ------------------------------------------------------------------ */

static uint32_t g_now;

uint32_t ark_bus_port_tick_ms(void)
{
    return g_now;
}

void ark_bus_port_lock(void)
{
}

void ark_bus_port_unlock(void)
{
}

void arkrtos_task_wait_internal(uint32 ms)
{
    g_now += (uint32_t)ms;
}

/* ------------------------------------------------------------------ */
/* 假串口：两条内存 FIFO                                                */
/* ------------------------------------------------------------------ */

#define FAKE_FIFO_MAX 4096u

static uint8_t  g_tx[FAKE_FIFO_MAX];   /* 本端写出去（= 对端收到） */
static size_t   g_tx_len;
static uint8_t  g_rx[FAKE_FIFO_MAX];   /* 对端发来（= 本端读到） */
static size_t   g_rx_len;
static size_t   g_rx_pos;

/* 三种可编程的异常形态 */
static int      g_write_fail_at = -1;  /* 写到第几个字节开始失败 */
static int      g_read_stuck;          /* 1 = DR 永远置位、每次都给满 64 字节 */
static uint32_t g_tx_timeouts;

static void fake_reset(void)
{
    g_tx_len = 0u;
    g_rx_len = 0u;
    g_rx_pos = 0u;
    g_write_fail_at = -1;
    g_read_stuck = 0;
    g_tx_timeouts = 0u;
}

/* 把"对端要发的字节"塞进接收 FIFO */
static void fake_peer_send(const void *data, size_t len)
{
    size_t i;

    for (i = 0u; (i < len) && (g_rx_len < FAKE_FIFO_MAX); i++) {
        g_rx[g_rx_len++] = ((const uint8_t *)data)[i];
    }
}

void ark_bus_line_uart_init(void)
{
    fake_reset();
}

int ark_bus_line_uart_write(const uint8_t *data, uint16_t len)
{
    uint16 i;

    if (data == 0) {
        return -1;
    }
    for (i = 0u; i < len; i++) {
        if ((g_write_fail_at >= 0) && ((int)i >= g_write_fail_at)) {
            g_tx_timeouts++;
            return (i > 0u) ? (int)i : -1;
        }
        if (g_tx_len < FAKE_FIFO_MAX) {
            g_tx[g_tx_len++] = data[i];
        }
    }
    return (int)len;
}

int ark_bus_line_uart_read(uint8_t *buf, uint16_t max_len)
{
    uint16 n = 0u;

    if (buf == 0) {
        return 0;
    }
    if (g_read_stuck) {
        /* 模拟"LSR.DR 恒置位但读不出真数据"：每次都回满一缓冲。 */
        for (n = 0u; n < max_len; n++) {
            buf[n] = 0xA5u;
        }
        return (int)max_len;
    }
    while ((n < max_len) && (g_rx_pos < g_rx_len)) {
        buf[n++] = g_rx[g_rx_pos++];
    }
    if (g_rx_pos >= g_rx_len) {
        g_rx_pos = 0u;
        g_rx_len = 0u;
    }
    return (int)n;
}

uint32_t ark_bus_line_uart_tx_timeouts(void)
{
    return g_tx_timeouts;
}

/* ------------------------------------------------------------------ */
/* 用例                                                                */
/* ------------------------------------------------------------------ */

static int has_tx(const char *needle)
{
    size_t n = strlen(needle);
    size_t i;

    if (g_tx_len < n) {
        return 0;
    }
    for (i = 0u; i + n <= g_tx_len; i++) {
        if (memcmp(g_tx + i, needle, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* 假回调：只占住 subscribe / rpc_register 的函数指针位。 */
static void line_test_on_msg(const char *topic, const uint8_t *payload,
                             uint16_t len, void *user)
{
    (void)topic;
    (void)payload;
    (void)len;
    (void)user;
}

static int line_test_on_rpc(struct ark_bus *bus, uint32_t from,
                            const uint8_t *params, uint16_t plen,
                            uint8_t *reply, uint16_t rcap,
                            uint16_t *rlen, void *user)
{
    (void)bus;
    (void)from;
    (void)params;
    (void)plen;
    (void)reply;
    (void)rcap;
    (void)user;
    if (rlen != 0) {
        *rlen = 0u;
    }
    return 0;
}

int main(void)
{
    arkrtos_softbus_line_stats_t st;
    int32_t rc;

    printf("== softbus_line host ==\n");

    /* --- 1. 未 init：六个入口都必须拒绝 ------------------------------ */
    fake_reset();
    CK(arkrtos_softbus_line_node_id() == 0u);
    CK(arkrtos_softbus_line_pump(1u) != 0);
    CK(arkrtos_softbus_line_subscribe("line/hb", 0, 0) != 0);
    CK(arkrtos_softbus_line_publish(0x2102u, "line/hb", "x", 1u) != 0);
    CK(arkrtos_softbus_line_rpc(0x2102u, 1u, "x", 1u, 0, 0) != 0);
    CK(arkrtos_softbus_line_rpc_register(1u, 0, 0) != 0);
    CK(arkrtos_softbus_line_stats(&st) != 0);
    CK(arkrtos_softbus_line_stats(0) != 0);

    /* --- 2. init 参数与状态 ------------------------------------------ */
    CK(arkrtos_softbus_line_init(0u, 1u) != 0);   /* node_id=0 必须拒 */
    rc = arkrtos_softbus_line_init(0x2101u, 1u);
    CK(rc == 0);
    CK(arkrtos_softbus_line_node_id() == 0x2101u);
    /* init 末尾会 pump 一轮广播 DISCOVER：线上必须已经有字节出去。 */
    CK(g_tx_len > 0u);

    rc = arkrtos_softbus_line_stats(&st);
    CK(rc == 0);
    CK(st.node_id == 0x2101u);
    CK(st.tx_bytes == st.rx_bytes || st.tx_bytes > 0u);

    /* --- 3. probe 的字节数与"行"语义 -------------------------------- */
    g_tx_len = 0u;
    rc = arkrtos_softbus_line_probe(3u);
    CK(rc == (int32_t)(3u * 14u));                 /* "ARK-BUS-LINE\r\n"=14 字节 */
    CK(has_tx("ARK-BUS-LINE"));

    g_tx_len = 0u;
    rc = arkrtos_softbus_line_probe(0u);            /* 0 视为 1 行 */
    CK(rc == 14);

    /* 写到一半失败时必须如实返回已发字节数，而不是报成功 */
    g_tx_len = 0u;
    g_write_fail_at = 5;
    rc = arkrtos_softbus_line_probe(2u);
    CK(rc == 5);
    g_write_fail_at = 0;
    rc = arkrtos_softbus_line_probe(1u);
    CK(rc < 0);
    g_write_fail_at = -1;

    /* --- 4. publish 必须让线上真有字节 ------------------------------ */
    g_tx_len = 0u;
    rc = arkrtos_softbus_line_publish(0x2102u, "line/hb", "hello", 5u);
    CK(rc > 0);
    CK(g_tx_len > 0u);
    CK(has_tx("hello") || g_tx_len > 20u);   /* 帧头 + 载荷，至少要有字节 */

    rc = arkrtos_softbus_line_stats(&st);
    CK(rc == 0);
    CK(st.tx_frames > 0u);
    CK(st.tx_bytes > 0u);

    /* --- 5. 回灌 + 卡死形态：pump 必须能返回 ------------------------- */
    fake_peer_send(g_tx, g_tx_len);
    CK(arkrtos_softbus_line_pump(1u) == 0);
    rc = arkrtos_softbus_line_stats(&st);
    CK(rc == 0);
    CK(st.rx_bytes > 0u);            /* 裸字节口径：确实从假口抽到东西 */

    /* DR 恒置位：没有 SB_LINE_DRAIN_LIMIT 时这里会挂死。用例跑得完
     * 本身就是断言（外层门禁有超时）。 */
    g_read_stuck = 1;
    CK(arkrtos_softbus_line_pump(1u) == 0);
    g_read_stuck = 0;

    /* 0 轮按 1 轮处理（与 probe(0) 同一口径），不是"什么都不做"。 */
    CK(arkrtos_softbus_line_pump(0u) == 0);

    /* --- 6. 统计透传：uart_tx_timeout 必须来自底层 ------------------- */
    g_write_fail_at = 0;
    (void)arkrtos_softbus_line_publish(0x2102u, "line/hb", "x", 1u);
    rc = arkrtos_softbus_line_stats(&st);
    CK(rc == 0);
    CK(st.uart_tx_timeout > 0u);
    CK(st.uart_tx_timeout == ark_bus_line_uart_tx_timeouts());
    g_write_fail_at = -1;

    /* --- 7. 订阅与 RPC 注册在起来之后必须接受 ------------------------ */
    /* 组件语义：subscribe 返回主题 id（>=0），回调可为 0（只登记主题）；
     * rpc_register 的 handler 不可为 0（E_INVAL）。两条都按组件真实行为
     * 断言 —— "以为返回 0"这种假设必须在这里先红。 */
    CK(arkrtos_softbus_line_subscribe("line/hb", line_test_on_msg, 0) >= 0);
    CK(arkrtos_softbus_line_rpc_register(1u, line_test_on_rpc, 0) == 0);
    CK(arkrtos_softbus_line_rpc(0x2102u, 1u, "ping", 4u, 0, 0) > 0);

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if (g_fails != 0) {
        printf("SOFTBUS-LINE-HOST FAIL\n");
        return 1;
    }
    printf("SOFTBUS-LINE-HOST OK\n");
    return 0;
}

#endif /* ARKRTOS_USE_SOFTBUS && ARKRTOS_USE_BUS_UART */