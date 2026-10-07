/**
* @file arkrtos_softbus.c
* @brief 软总线（ark_softbus）内核接入：常驻节点 + 端到端自检 + 控制台命令
* @details 这块只有一块 F427 板、且唯一一路 USART1 已被内核控制台占用，所以
*          「怎么证明总线在板上真的能跑」这个问题用一个诚实的办法回答：
*
*            - 板内 loopback 链路（组件自带的传输层，非模拟器，走的是
*              真实的帧编解码 + 环形缓冲 + 发送/接收路径），把**本节点**
*              （id 0x1001）和**常驻对端节点**（id 0x1002，同一固件里另一
*              个 ark_bus_t 实例）接起来；
*            - 两个节点都是真实的总线实例，时间来自内核节拍、临界区来自
*              调度器锁，所以端口（board/…/arkrtos_bus_port.c）真的被走到；
*            - `bus selftest` 用这条链路跑完整的端到端检查，控制台上逐项
*              打印 PASS/FAIL。
*
*          **未覆盖的部分（如实声明）**：物理串口线上的两个节点（两块板
*          或板+PC）没有验证——板上没有空闲 USART。组件里的 UART 传输层
*          已同步进来，接法见 port/README.md 与 docs/软总线接入.md。
*
* @note 只在 ARKRTOS_USE_SOFTBUS=1 时参与编译。
* @author xw
* @date 2026.10.07
*/

#include "arkrtos_softbus.h"

#if (ARKRTOS_USE_SOFTBUS == 1)

#include <string.h>
#include <stdio.h>

#include "arkrtos_task.h"
#include "ark_bus_port_arkrtos.h"
#include "ark_bus_rpc.h"
#include "ark_shell.h"
#include "arkrtos_softbus_sync.h"

/* ------------------------------------------------------------------ */
/* 常驻节点与对端                                                       */
/* ------------------------------------------------------------------ */

#define SB_NODE_ID      (0x00001001u)   /* 本节点 */
#define SB_PEER_ID      (0x00001002u)   /* 常驻对端 */
#define SB_NODE_TYPE    (1u)
#define SB_PEER_TYPE    (2u)

/* loopback 环容量：一环 1024B，远大于一帧（128B 载荷的帧最大 149B），
 * 一帧都不会因为环小而被丢。两个环共 2048B。 */
#define SB_LB_HALF      (1024u)

static uint8_t              g_lb_store[2u * SB_LB_HALF];
static ark_bus_loopback_t   g_lb;
static ark_bus_t            g_node;          /* 本节点 */
static ark_bus_t            g_peer;          /* 常驻对端（同固件内） */
static uint32               g_up;

/* 对端观察到的内容（自检与 `bus rx` 都读它） */
static uint32               g_peer_msgs;
static char                 g_peer_last_topic[ARK_BUS_TOPIC_NAME_MAX];
static uint8_t              g_peer_last_payload[ARK_BUS_MSG_MAX_SIZE];
static uint16               g_peer_last_len;

static uint32               g_peer_rpc_calls;
static uint32               g_peer_stream_calls;
static uint32               g_peer_stream_len;
static int                  g_peer_stream_ok;

/* 本节点观察到的应答 */
static uint32               g_node_rpc_calls;
static int                  g_node_rpc_status;
static uint8_t              g_node_rpc_result[32];
static uint16               g_node_rpc_result_len;
static uint32               g_node_stream_done;
static int                  g_node_stream_status;

static void sb_state_reset(void)
{
    g_peer_msgs = 0u;
    g_peer_last_topic[0] = '\0';
    g_peer_last_len = 0u;
    g_peer_rpc_calls = 0u;
    g_peer_stream_calls = 0u;
    g_peer_stream_len = 0u;
    g_peer_stream_ok = 0;
    g_node_rpc_calls = 0u;
    g_node_rpc_status = 0;
    g_node_rpc_result_len = 0u;
    g_node_stream_done = 0u;
    g_node_stream_status = 0;
}

/* ---- 对端侧回调 ---- */

static void sb_peer_on_msg(const char *topic, const uint8_t *payload,
                           uint16_t len, void *user)
{
    (void)user;
    g_peer_msgs++;
    if(topic == NULL)
    {
        return;
    }
    (void)strncpy(g_peer_last_topic, topic, sizeof(g_peer_last_topic) - 1u);
    g_peer_last_topic[sizeof(g_peer_last_topic) - 1u] = '\0';
    g_peer_last_len = (len > (uint16_t)sizeof(g_peer_last_payload))
                      ? (uint16_t)sizeof(g_peer_last_payload) : len;
    if(payload != NULL)
    {
        (void)memcpy(g_peer_last_payload, payload, (uint32)g_peer_last_len);
    }
}

static int sb_peer_rpc_ping(struct ark_bus *bus, uint32_t from,
                            const uint8_t *params, uint16_t plen,
                            uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                            void *user)
{
    uint16_t i;
    uint16_t n = plen;

    (void)bus;
    (void)from;
    (void)user;
    g_peer_rpc_calls++;
    if(n > rcap)
    {
        n = rcap;
    }
    for(i = 0u; i < n; i++)
    {
        reply[i] = (uint8_t)(params[i] + 1u);
    }
    *rlen = n;
    return ARK_BUS_OK;
}

static void sb_peer_on_stream(struct ark_bus *bus, uint32_t from,
                              const uint8_t *data, uint16_t len, void *user)
{
    uint16_t i;

    (void)bus;
    (void)user;
    g_peer_stream_calls++;
    g_peer_stream_len = len;
    g_peer_stream_ok = (from == SB_NODE_ID) ? 1 : 0;
    /* 内容按「第 i 字节 = i & 0xFF」发出来，对端就按同一规则验。 */
    for(i = 0u; i < len; i++)
    {
        if(data[i] != (uint8_t)(i & 0xFFu))
        {
            g_peer_stream_ok = 0;
            break;
        }
    }
}

/* ---- 本节点侧回调 ---- */

static void sb_node_on_rpc(int status, const uint8_t *result, uint16_t len,
                           void *user)
{
    (void)user;
    g_node_rpc_calls++;
    g_node_rpc_status = status;
    g_node_rpc_result_len = (len > (uint16_t)sizeof(g_node_rpc_result))
                            ? (uint16_t)sizeof(g_node_rpc_result) : len;
    if((result != NULL) && (g_node_rpc_result_len > 0u))
    {
        (void)memcpy(g_node_rpc_result, result, (uint32)g_node_rpc_result_len);
    }
}

static void sb_node_on_stream_done(int status, uint16_t xid, void *user)
{
    (void)xid;
    (void)user;
    g_node_stream_done++;
    g_node_stream_status = status;
}

/* ------------------------------------------------------------------ */
/* 建立与推进                                                           */
/* ------------------------------------------------------------------ */

static int32 sb_build(void)
{
    sb_state_reset();

    ark_bus_loopback_init(&g_lb, g_lb_store, (uint16_t)SB_LB_HALF);

    ark_bus_init(&g_node, ark_bus_loopback_a(&g_lb));
    ark_bus_init(&g_peer, ark_bus_loopback_b(&g_lb));

    /* 对端提供的服务：一条订阅、一个 RPC 方法、一个数据平面接收口。
     * 订阅 "cmd/led" 是内核 LED 的语义位（与 hwsafe 的 LED3 一致），
     * 便于和别的演示对照。 */
    (void)ark_bus_subscribe(&g_peer, "cmd/led", sb_peer_on_msg, NULL);
    (void)ark_bus_rpc_register(&g_peer, ARK_BUS_M_PING, sb_peer_rpc_ping, NULL);
    ark_bus_stream_on_recv(&g_peer, sb_peer_on_stream, NULL);

    if(ark_bus_start(&g_node, SB_NODE_ID, SB_NODE_TYPE, ARK_CAP_MCU_EDGE) < 0)
    {
        return -1;
    }
    if(ark_bus_start(&g_peer, SB_PEER_ID, SB_PEER_TYPE, ARK_CAP_ACTUATOR) < 0)
    {
        return -1;
    }

    g_up = 1u;
    (void)arkrtos_softbus_pump(4u);   /* 广播 DISCOVER + 应答，互相入表 */
    return 0;
}

int32 arkrtos_softbus_init(void)
{
    /* 板级三钩子 -> 总线 HAL。装的是真实实现：毫秒来自内核节拍，
     * 临界区来自调度器锁。 */
    ark_bus_port_arkrtos_install();
    g_up = 0u;
    return sb_build();
}

int32 arkrtos_softbus_pump(uint32 rounds)
{
    uint32 i;

    if(g_up == 0u)
    {
        return -1;
    }
    if(rounds == 0u)
    {
        rounds = 1u;
    }
    for(i = 0u; i < rounds; i++)
    {
        ark_bus_poll(&g_node);
        ark_bus_poll(&g_peer);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* info                                                                 */
/* ------------------------------------------------------------------ */

int32 arkrtos_softbus_info(void)
{
    const ark_bus_device_t *peer;
    const ark_bus_stats_t  *st;

    if(g_up == 0u)
    {
        if(arkrtos_softbus_init() != 0)
        {
            ark_shell_printf("softbus: init failed\r\n");
            return -1;
        }
    }

    st = ark_bus_get_stats(&g_node);
    peer = ark_bus_device_find(&g_node, SB_PEER_ID);

    /* commit 由 tools/sync_ark_softbus.py 写进 arkrtos_softbus_sync.h：
     * 手工写死会在下次同步后变成假的「权威版本号」。 */
    ark_shell_printf("\r\nsoftbus (ark_softbus %s%s)\r\n",
                     ARKRTOS_SOFTBUS_COMMIT,
                     ARKRTOS_SOFTBUS_DIRTY ? "+dirty" : "");
    ark_shell_printf("  node id   : 0x%08X type %u cap 0x%08X\r\n",
                     (unsigned)g_node.self_id, (unsigned)g_node.device_type,
                     (unsigned)g_node.capabilities);
    ark_shell_printf("  link      : loopback (in-board, not a physical UART)\r\n");
    ark_shell_printf("  profile   : msg<=%u topic<=%u dev<=%u qos=%u rpc=%u stream=%u\r\n",
                     (unsigned)ARK_BUS_MSG_MAX_SIZE, (unsigned)ARK_BUS_MAX_TOPICS,
                     (unsigned)ARK_BUS_MAX_DEVICES,
                     (unsigned)ARK_BUS_QOS_MAX_SESSIONS,
                     (unsigned)ARK_BUS_RPC_MAX_SLOTS,
                     (unsigned)ARK_BUS_STREAM_RX_SLOTS);
    ark_shell_printf("  peers     : %u (peer 0x%08X %s)\r\n",
                     (unsigned)ark_bus_device_count(&g_node),
                     (unsigned)SB_PEER_ID,
                     (peer != NULL && peer->state == ARK_BUS_DEV_ONLINE)
                     ? "online" : "offline/absent");
    ark_shell_printf("  node tx   : frames=%u bytes=%u retrans=%u timeout=%u\r\n",
                     (unsigned)st->tx_frames, (unsigned)st->tx_bytes,
                     (unsigned)st->qos_retransmits, (unsigned)st->qos_timeouts);
    ark_shell_printf("  node rx   : frames=%u bytes=%u crc_err=%u bad=%u resync=%u\r\n",
                     (unsigned)st->rx_frames, (unsigned)st->rx_bytes,
                     (unsigned)st->rx_crc_errors, (unsigned)st->rx_bad_frames,
                     (unsigned)st->rx_resyncs);
    ark_shell_printf("  peer saw  : msgs=%u last_topic=%s last_len=%u rpc=%u stream=%u\r\n",
                     (unsigned)g_peer_msgs,
                     (g_peer_last_topic[0] != '\0') ? g_peer_last_topic : "-",
                     (unsigned)g_peer_last_len, (unsigned)g_peer_rpc_calls,
                     (unsigned)g_peer_stream_calls);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 自检                                                                 */
/* ------------------------------------------------------------------ */

static uint32 g_ck;
static uint32 g_bad;

static void sb_ck(int ok, const char *what)
{
    g_ck++;
    if(ok == 0)
    {
        g_bad++;
        ark_shell_printf("    FAIL: %s\r\n", what);
    }
}

static void sb_stage(const char *name, void (*fn)(void))
{
    uint32 before = g_bad;

    fn();
    ark_shell_printf("  %-4s %-18s\r\n", (g_bad == before) ? "PASS" : "FAIL", name);
}

/* 1. 帧编解码：与本仓库主机侧测试同一套向量（CRC-16/CCITT-FALSE）。 */
static void sb_st_codec(void)
{
    static const uint8_t vec[] = "123456789";
    ark_bus_frame_t      f;
    ark_bus_frame_t      out;
    uint8_t              buf[64];
    int                  n;

    sb_ck(ark_bus_crc16(vec, 9u) == 0x29B1u, "crc16 vector");

    (void)memset(&f, 0, sizeof(f));
    f.version = ARK_BUS_VERSION;
    f.msg_type = ARK_BUS_MSG_PUBLISH;
    f.flags = ARK_BUS_FLAG_QOS1;
    f.topic_id = 0x1234u;
    f.seq = 7u;
    f.src_id = 0xAABBCCDDu;
    f.payload = (const uint8_t *)"hi";
    f.payload_len = 2u;

    n = ark_bus_frame_encode(&f, buf, (uint16_t)sizeof(buf));
    sb_ck(n == (int)ark_bus_frame_size(2u), "encode size");
    sb_ck((buf[0] == 0xAAu) && (buf[1] == 0x55u), "magic");
    sb_ck((buf[5] == 0x12u) && (buf[6] == 0x34u), "big-endian topic id");

    sb_ck(ark_bus_frame_decode(buf, (uint16_t)n, &out) == ARK_BUS_OK, "decode ok");
    sb_ck(out.topic_id == 0x1234u, "decode topic");
    sb_ck(out.src_id == 0xAABBCCDDu, "decode src");
    sb_ck((out.payload_len == 2u) && (out.payload[0] == 'h'), "decode payload");

    buf[n - 1] ^= 0xFFu;
    sb_ck(ark_bus_frame_decode(buf, (uint16_t)n, &out) == ARK_BUS_E_CRC, "crc error");
    buf[n - 1] ^= 0xFFu;
    buf[0] = 0x00u;
    sb_ck(ark_bus_frame_decode(buf, (uint16_t)n, &out) == ARK_BUS_E_FORMAT, "bad magic");
    buf[0] = 0xAAu;
    sb_ck(ark_bus_frame_decode(buf, 4u, &out) == ARK_BUS_E_FORMAT, "truncated");
}

/* 2. 逐字节重组：前导噪声里能切出完整帧。 */
static void sb_st_reader(void)
{
    ark_bus_frame_t        f;
    ark_bus_frame_reader_t r;
    uint8_t                buf[32];
    int                    n;
    int                    i;
    int                    rc = 0;

    (void)memset(&f, 0, sizeof(f));
    f.version = ARK_BUS_VERSION;
    f.msg_type = ARK_BUS_MSG_PUBLISH;
    f.payload = (const uint8_t *)"abc";
    f.payload_len = 3u;
    n = ark_bus_frame_encode(&f, buf, (uint16_t)sizeof(buf));

    ark_bus_frame_reader_init(&r);
    sb_ck(ark_bus_frame_reader_push(&r, 0x00u) == -1, "reader rejects noise");
    sb_ck(ark_bus_frame_reader_push(&r, 0xAAu) == 0, "reader accepts first magic");
    sb_ck(ark_bus_frame_reader_push(&r, 0x11u) == -1, "reader rejects bad second magic");
    for(i = 0; i < n; i++)
    {
        rc = ark_bus_frame_reader_push(&r, buf[i]);
    }
    sb_ck(rc == 1, "reader completes a frame");
    sb_ck(r.len == (uint16_t)n, "reader frame length");
}

/* 3. 发现：start() 的 DISCOVER 广播 + 应答，双向入表。 */
static void sb_st_discovery(void)
{
    const ark_bus_device_t *p;

    (void)arkrtos_softbus_pump(4u);
    sb_ck(ark_bus_device_count(&g_node) == 1u, "node knows one peer");
    sb_ck(ark_bus_device_count(&g_peer) == 1u, "peer knows one node");
    p = ark_bus_device_find(&g_node, SB_PEER_ID);
    sb_ck(p != NULL, "node finds peer by id");
    sb_ck((p != NULL) && (p->state == ARK_BUS_DEV_ONLINE), "peer online");
    sb_ck((p != NULL) && (p->capabilities == ARK_CAP_ACTUATOR), "peer caps carried");
}

/* 4. 广播发布/订阅。 */
static void sb_st_pubsub(void)
{
    /* 订阅是接收侧的本地主题表：对端没注册这个名字，topic_find_by_id 就查不到，
     * 帧只会计进 dispatch_errors 而不会进回调。所以先让对端订阅，再发。 */
    sb_ck(ark_bus_subscribe(&g_peer, "sensor/temp", sb_peer_on_msg, NULL) > 0,
          "peer subscribes topic");
    g_peer_msgs = 0u;
    sb_ck(ark_bus_publish(&g_node, "sensor/temp", "25", 2u) > 0, "publish returns bytes");
    (void)arkrtos_softbus_pump(3u);
    sb_ck(g_peer_msgs == 1u, "peer callback fired once");
    sb_ck(strcmp(g_peer_last_topic, "sensor/temp") == 0, "topic carried");
    sb_ck((g_peer_last_len == 2u) && (g_peer_last_payload[0] == '2'), "payload carried");

    /* 没有订阅者的主题仍会到达对端，计为「已分发但无人处理」。 */
    (void)ark_bus_publish(&g_node, "nobody/home", "x", 1u);
    (void)arkrtos_softbus_pump(3u);
    sb_ck(g_peer.stats.dispatch_errors >= 1u, "unsubscribed topic counted");
}

/* 5. 定向发送 + QoS1 ACK。 */
static void sb_st_directed(void)
{
    uint32 rt_before = g_node.stats.qos_retransmits;

    (void)ark_bus_subscribe(&g_peer, "cmd/led", sb_peer_on_msg, NULL);
    g_peer_msgs = 0u;
    sb_ck(ark_bus_publish_to(&g_node, SB_PEER_ID, "cmd/led", "ON", 2u) > 0,
          "publish_to returns bytes");
    (void)arkrtos_softbus_pump(4u);
    sb_ck(g_peer_msgs == 1u, "peer got directed frame");
    sb_ck(g_node.stats.qos_retransmits == rt_before, "no retransmit after ACK");
}

/* 6. RPC 请求/应答（走定向路径，双向都是 QoS1）。 */
static void sb_st_rpc(void)
{
    uint8_t params[2];

    params[0] = 0x10u;
    params[1] = 0x20u;
    g_node_rpc_calls = 0u;
    g_node_rpc_status = 99;
    g_node_rpc_result_len = 0u;

    sb_ck(ark_bus_rpc_request(&g_node, SB_PEER_ID, ARK_BUS_M_PING,
                              params, 2u, sb_node_on_rpc, NULL) > 0,
          "rpc_request returns bytes");
    sb_ck(ark_bus_rpc_pending(&g_node) == 1u, "rpc pending before reply");
    sb_ck(g_node_rpc_calls == 0u, "rpc is async");
    (void)arkrtos_softbus_pump(3u);
    sb_ck(g_node_rpc_calls == 1u, "rpc callback fired once");
    sb_ck(g_node_rpc_status == ARK_BUS_OK, "rpc status ok");
    sb_ck(g_peer_rpc_calls == 1u, "peer handler ran");
    sb_ck((g_node_rpc_result_len == 2u) && (g_node_rpc_result[0] == 0x11u) &&
          (g_node_rpc_result[1] == 0x21u), "rpc result bytes");
    sb_ck(ark_bus_rpc_pending(&g_node) == 0u, "rpc slot released");
}

/* 7. 数据平面：200B 对象 > 一帧载荷(128B)，必然会分块。 */
static void sb_st_stream(void)
{
    uint8_t obj[200];
    uint16_t i;
    uint16_t xid = 0u;
    uint32 guard;

    for(i = 0u; i < (uint16_t)sizeof(obj); i++)
    {
        obj[i] = (uint8_t)(i & 0xFFu);
    }

    g_peer_stream_calls = 0u;
    g_peer_stream_ok = 0;
    g_node_stream_done = 0u;
    g_node_stream_status = 0;

    sb_ck(ark_bus_stream_send(&g_node, SB_PEER_ID, obj, sizeof(obj),
                              sb_node_on_stream_done, NULL, &xid) == ARK_BUS_OK,
          "stream_send accepted");

    for(guard = 0u; (guard < 200u) && (g_node_stream_done == 0u); guard++)
    {
        (void)arkrtos_softbus_pump(2u);
    }

    sb_ck(g_peer_stream_calls == 1u, "peer reassembled one object");
    sb_ck(g_peer_stream_len == (uint32)sizeof(obj), "object length preserved");
    sb_ck(g_peer_stream_ok == 1, "object bytes preserved");
    sb_ck((g_node_stream_done == 1u) && (g_node_stream_status == ARK_BUS_OK),
          "sender completed");
}

/* 8. QoS 重传与超时：目标不存在，重传 N 次后判超时。
 *    这一项要真实时间（重传间隔 200ms），所以用任务等待推进节拍，
 *    期间照常 poll——不是忙等，别的任务照跑。 */
static void sb_st_qos(void)
{
    uint32 rt_before = g_node.stats.qos_retransmits;
    uint32 to_before = g_node.stats.qos_timeouts;
    uint32 i;

    sb_ck(ark_bus_publish_to(&g_node, 0x0000ABCDu, "cmd/x", "Y", 1u) > 0,
          "publish_to unknown peer accepted");

    for(i = 0u; i < 60u; i++)
    {
        arkrtos_task_wait_internal(50u);
        ark_bus_poll(&g_node);
        if(g_node.stats.qos_timeouts > to_before)
        {
            break;
        }
    }

    sb_ck(g_node.stats.qos_retransmits > rt_before, "retransmitted without ACK");
    sb_ck(g_node.stats.qos_timeouts == (to_before + 1u), "gave up exactly once");
}

/* 9. 错误路径：超长载荷、坏帧、自发帧回环。 */
static void sb_st_errors(void)
{
    static uint8_t big[ARK_BUS_MSG_MAX_SIZE + 8u];
    ark_bus_frame_t f;
    uint8_t         buf[64];
    int             n;
    uint32          crc_before = g_node.stats.rx_crc_errors;
    uint32          msgs_before;

    (void)memset(big, 0x5A, sizeof(big));
    sb_ck(ark_bus_publish(&g_node, "cmd/led", big, ARK_BUS_MSG_MAX_SIZE + 1u) ==
          ARK_BUS_E_INVAL, "oversize payload refused");

    /* 自己发出去的帧回到自己：必须被丢弃，不得触发本地订阅。 */
    (void)ark_bus_subscribe(&g_node, "cmd/led", NULL, NULL);
    (void)memset(&f, 0, sizeof(f));
    f.version = ARK_BUS_VERSION;
    f.msg_type = ARK_BUS_MSG_PUBLISH;
    f.topic_id = (uint16_t)ark_bus_topic_lookup(&g_node, "cmd/led");
    f.src_id = g_node.self_id;
    f.payload = (const uint8_t *)"z";
    f.payload_len = 1u;
    n = ark_bus_frame_encode(&f, buf, (uint16_t)sizeof(buf));

    msgs_before = g_peer_msgs;
    sb_ck(ark_bus_feed(&g_node, buf, (uint16_t)n) == ARK_BUS_OK, "self frame accepted");
    sb_ck(g_peer_msgs == msgs_before, "self frame not echoed onward");

    buf[n - 1] ^= 0xFFu;
    sb_ck(ark_bus_feed(&g_node, buf, (uint16_t)n) == ARK_BUS_E_CRC, "bad crc rejected");
    sb_ck(g_node.stats.rx_crc_errors > crc_before, "crc error counted");
}

/* 10. 主题表：注册/查重/容量上限（放在最后，会把表填满）。 */
static void sb_st_topics(void)
{
    char name[16];
    int  k;
    int  full_seen = 0;
    int  dup_ok = 1;

    sb_ck(ark_bus_topic_lookup(&g_node, "no/such") == ARK_BUS_E_NOTFOUND,
          "unknown topic not found");
    sb_ck(ark_bus_topic_register(&g_node, "local/a") > 0, "register new topic");
    sb_ck(ark_bus_topic_register(&g_node, "local/a") ==
          ark_bus_topic_lookup(&g_node, "local/a"), "re-register returns same id");

    for(k = 0; k < (int)ARK_BUS_MAX_TOPICS + 4; k++)
    {
        int r;

        (void)snprintf(name, sizeof(name), "t/%d", k);
        r = ark_bus_topic_register(&g_node, name);
        if(r == ARK_BUS_E_FULL)
        {
            full_seen = 1;
        }
        else if(r < 0)
        {
            dup_ok = 0;
        }
    }
    sb_ck(full_seen == 1, "table reports FULL");
    sb_ck(dup_ok == 1, "no unexpected error while filling");
}

int32 arkrtos_softbus_selftest(void)
{
    ark_shell_printf("\r\nsoftbus selftest (loopback, two in-board nodes)\r\n");

    if(arkrtos_softbus_init() != 0)
    {
        ark_shell_printf("  init failed\r\n");
        return 99;
    }

    g_ck = 0u;
    g_bad = 0u;

    sb_stage("codec", sb_st_codec);
    sb_stage("reader", sb_st_reader);
    sb_stage("discovery", sb_st_discovery);
    sb_stage("pubsub", sb_st_pubsub);
    sb_stage("directed+ack", sb_st_directed);
    sb_stage("rpc", sb_st_rpc);
    sb_stage("stream", sb_st_stream);
    sb_stage("qos-timeout", sb_st_qos);
    sb_stage("errors", sb_st_errors);
    sb_stage("topics", sb_st_topics);

    ark_shell_printf("  checks %u, failed %u\r\n", (unsigned)g_ck, (unsigned)g_bad);

    /* 自检把主题表填满了，跑完复位一次，让 `bus pub/rpc` 还能用。 */
    (void)arkrtos_softbus_init();
    return (int32)g_bad;
}

/* ------------------------------------------------------------------ */
/* 控制台命令                                                           */
/* ------------------------------------------------------------------ */

/* arkrtos_shell.c 的 parse_u32 是 static 的，内核模块链接不到；这里自带
 * 一个等价实现：跳前导空格、纯十进制、无符号、溢出即失败。 */
static int sb_parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;
    int    any = 0;

    if((s == NULL) || (out == NULL))
    {
        return -1;
    }
    while((*s == ' ') || (*s == '\t'))
    {
        s++;
    }
    while((*s >= '0') && (*s <= '9'))
    {
        uint32 d = (uint32)(*s - '0');

        if(v > ((0xFFFFFFFFu - d) / 10u))
        {
            return -1;
        }
        v = v * 10u + d;
        any = 1;
        s++;
    }
    if(any == 0)
    {
        return -1;
    }
    *out = v;
    return 0;
}

static void sb_usage(void)
{
    ark_shell_printf("usage: bus [info | selftest | pump [n] | peers | rx |"
                     " pub <topic> <text> | rpc <text> | stream <n>]\r\n");
}

static int sb_cmd_pub(int argc, char *argv[])
{
    int rc;

    if(argc < 4)
    {
        sb_usage();
        return -1;
    }

    /* 让对端先订阅这个主题，任何主题名都能被观察到（没有通配订阅）。 */
    if(ark_bus_subscribe(&g_peer, argv[2], sb_peer_on_msg, NULL) < 0)
    {
        ark_shell_printf("bus pub: peer subscribe failed (topic table full?)\r\n");
        return -1;
    }

    g_peer_msgs = 0u;
    rc = ark_bus_publish(&g_node, argv[2], argv[3], (uint16_t)strlen(argv[3]));
    if(rc <= 0)
    {
        ark_shell_printf("bus pub: publish failed (%d)\r\n", rc);
        return -1;
    }

    (void)arkrtos_softbus_pump(4u);
    ark_shell_printf("bus pub: sent %d B on %s, peer got %u msg (%s len=%u)\r\n",
                     rc, argv[2], (unsigned)g_peer_msgs,
                     (g_peer_last_topic[0] != '\0') ? g_peer_last_topic : "-",
                     (unsigned)g_peer_last_len);
    return (g_peer_msgs == 1u) ? 0 : -1;
}

static int sb_cmd_rpc(int argc, char *argv[])
{
    uint32 guard;

    if(argc < 3)
    {
        sb_usage();
        return -1;
    }

    g_node_rpc_calls = 0u;
    g_node_rpc_status = 99;
    g_node_rpc_result_len = 0u;

    if(ark_bus_rpc_request(&g_node, SB_PEER_ID, ARK_BUS_M_PING,
                           (const uint8_t *)argv[2], (uint16_t)strlen(argv[2]),
                           sb_node_on_rpc, NULL) <= 0)
    {
        ark_shell_printf("bus rpc: request failed\r\n");
        return -1;
    }

    for(guard = 0u; (guard < 20u) && (g_node_rpc_calls == 0u); guard++)
    {
        (void)arkrtos_softbus_pump(3u);
    }

    if(g_node_rpc_calls == 0u)
    {
        ark_shell_printf("bus rpc: no reply (peer handler=%u)\r\n",
                         (unsigned)g_peer_rpc_calls);
        return -1;
    }

    ark_shell_printf("bus rpc: status=%d result_len=%u result=\"%s\"\r\n",
                     g_node_rpc_status, (unsigned)g_node_rpc_result_len,
                     g_node_rpc_result);
    return (g_node_rpc_status == ARK_BUS_OK) ? 0 : -1;
}

static int sb_cmd_stream(int argc, char *argv[])
{
    static uint8_t obj[ARK_BUS_STREAM_MAX_BYTES];
    uint32 n = 200u;
    uint32 guard;
    uint16_t i;
    uint16_t xid = 0u;

    if(argc >= 3)
    {
        if(sb_parse_u32(argv[2], &n) != 0)
        {
            n = 200u;
        }
    }
    if(n == 0u)
    {
        n = 1u;
    }
    if(n > (uint32)sizeof(obj))
    {
        n = (uint32)sizeof(obj);
    }
    for(i = 0u; i < (uint16_t)n; i++)
    {
        obj[i] = (uint8_t)(i & 0xFFu);
    }

    g_peer_stream_calls = 0u;
    g_peer_stream_ok = 0;
    g_node_stream_done = 0u;
    g_node_stream_status = 0;

    if(ark_bus_stream_send(&g_node, SB_PEER_ID, obj, n,
                           sb_node_on_stream_done, NULL, &xid) != ARK_BUS_OK)
    {
        ark_shell_printf("bus stream: send refused (%u B > %u?)\r\n",
                         (unsigned)n, (unsigned)ARK_BUS_STREAM_MAX_BYTES);
        return -1;
    }

    for(guard = 0u; (guard < 200u) && (g_node_stream_done == 0u); guard++)
    {
        (void)arkrtos_softbus_pump(2u);
    }

    ark_shell_printf("bus stream: xid=%u sent=%u B, peer rx_calls=%u len=%u bytes_ok=%d,"
                     " done=%u status=%d\r\n",
                     (unsigned)xid, (unsigned)n, (unsigned)g_peer_stream_calls,
                     (unsigned)g_peer_stream_len, g_peer_stream_ok,
                     (unsigned)g_node_stream_done, g_node_stream_status);

    return ((g_peer_stream_calls == 1u) && (g_peer_stream_ok == 1) &&
            (g_node_stream_done == 1u) && (g_node_stream_status == ARK_BUS_OK))
           ? 0 : -1;
}

int arkrtos_softbus_shell_cmd(int argc, char **argv)
{
    const char *sub;

    if(g_up == 0u)
    {
        if(arkrtos_softbus_init() != 0)
        {
            ark_shell_printf("softbus: init failed\r\n");
            return -1;
        }
    }

    if(argc < 2)
    {
        return (int)arkrtos_softbus_info();
    }

    sub = argv[1];

    if(strcmp(sub, "info") == 0)
    {
        return (int)arkrtos_softbus_info();
    }
    if(strcmp(sub, "selftest") == 0)
    {
        int32 bad = arkrtos_softbus_selftest();

        ark_shell_printf("bus selftest: %s\r\n", (bad == 0) ? "ALL PASS" : "FAILED");
        return (int)bad;
    }
    if(strcmp(sub, "pump") == 0)
    {
        uint32 rounds = 1u;

        if(argc >= 3)
        {
            (void)sb_parse_u32(argv[2], &rounds);
        }
        (void)arkrtos_softbus_pump(rounds);
        ark_shell_printf("bus pump: %u round(s)\r\n", (unsigned)rounds);
        return 0;
    }
    if(strcmp(sub, "peers") == 0)
    {
        uint8 i;

        for(i = 0u; i < ark_bus_device_count(&g_node); i++)
        {
            const ark_bus_device_t *d = ark_bus_device_at(&g_node, i);

            ark_shell_printf("  peer %u: id=0x%08X type=%u cap=0x%08X %s\r\n",
                             (unsigned)i, (unsigned)d->id, (unsigned)d->type,
                             (unsigned)d->capabilities,
                             (d->state == ARK_BUS_DEV_ONLINE) ? "online" : "offline");
        }
        if(ark_bus_device_count(&g_node) == 0u)
        {
            ark_shell_printf("  (no peers yet)\r\n");
        }
        return 0;
    }
    if(strcmp(sub, "rx") == 0)
    {
        ark_shell_printf("bus rx: msgs=%u topic=%s len=%u\r\n",
                         (unsigned)g_peer_msgs,
                         (g_peer_last_topic[0] != '\0') ? g_peer_last_topic : "-",
                         (unsigned)g_peer_last_len);
        return 0;
    }
    if(strcmp(sub, "pub") == 0)
    {
        return sb_cmd_pub(argc, argv);
    }
    if(strcmp(sub, "rpc") == 0)
    {
        return sb_cmd_rpc(argc, argv);
    }
    if(strcmp(sub, "stream") == 0)
    {
        return sb_cmd_stream(argc, argv);
    }

    sb_usage();
    return -1;
}

#endif /* ARKRTOS_USE_SOFTBUS */
