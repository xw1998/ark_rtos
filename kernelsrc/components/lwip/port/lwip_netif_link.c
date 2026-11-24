/**
* @file lwip_netif_link.c
* @brief 把板级网卡接成 lwIP 的一张真 netif（默认关）
* @details 在此之前 lwIP 只有一张回环网卡（见 lwip_port.c 顶部）：socket
*          语义都在，包不出芯片。本文件补上另一半——一张由真实驱动供包的
*          netif，于是 ARP、ICMP、TCP 走的都是真以太网帧。
*
*          四个决定：
*            1) **netif_add 走 tcpip_callback**。lwIP 的 netif API 不是线程
*               安全的（本工程 LWIP_TCPIP_CORE_LOCKING=0，靠 tcpip 线程串行化），
*               而本函数是在 lwIP 服务任务里调的。注意 tcpip_callback() 在
*               这个 lwIP 版本（STABLE-2_1_3）是**异步**的：它只把消息投进
*               tcpip 邮箱就返回，不等回调执行。所以"start 返回 0"只说明
*               "请求已投递"，装配成没成由回调自己打日志——不要在这里假装
*               读得到结果（这里读到的 rc 永远是投递前的初值）。
*            2) **linkoutput 用静态缓冲，不用栈**。TCPIP_THREAD_STACKSIZE 对
*               应 2 KB 栈（sys_arch.c 的 LWIP_THREAD_STACK_W），一帧 1536 字节
*               的栈数组会直接把 tcpip 线程的栈打穿 —— 那种崩溃看起来像
*               "随机地址访问错误"，与网卡毫无关系。linkoutput 只在 tcpip
*               线程里被调用，静态缓冲没有并发问题。
*            3) **收包投给 tcpip 线程，不在轮询任务里直接跑协议栈**。
*               本工程 LWIP_TCPIP_CORE_LOCKING=0，即"协议栈只在 tcpip 线程
*               里执行"是全部线程安全的前提。早期版本在轮询任务里直接调
*               netif.input（ethernet_input），于是 ARP 表 / netif 状态被
*               两个线程同时改：实测表现为跑几百毫秒后整机停住（高优先级
*               任务在一条被改坏的链表上死循环），而且 raw pcb 收不到回包。
*               现在轮询任务只做 pbuf_alloc + pbuf_take + tcpip_input()，
*               由 tcpip_input 把包投进 tcpip 邮箱，协议栈仍在 tcpip 线程里
*               串行执行——这正是 lwIP 对有 OS 的移植给出的标准用法。
*            4) **每轮把接收环抽干**（有上限），不是每轮一帧：1 ms 节拍内的
*               突发会让环溢出，而"溢出"在读数上长得像"对端没发"。
*            5) **静态地址，不开 DHCP**。这一版的目的是证明"真帧能进出"，
*               DHCP 会引入第二套需要单独验证的状态机。QEMU 的用户态网络
*               固定是 10.0.2.0/24、网关/宿主 10.0.2.2，所以地址是确定的。
*
*          只在 ARKRTOS_USE_NET_VIRTIO=1 时参与编译。
*
* @author xw
*/

#include "arkrtos_features.h"
#include "arkrtos_types.h"

#if (ARKRTOS_USE_NET_VIRTIO == 1)

#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "netif/ethernet.h"

#include "arkrtos.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_log.h"
#include "arkrtos_net_dev.h"

#include "lwip_netif_link.h"

/* QEMU 用户态网络的固定布局（slirp）：guest 10.0.2.15/24、网关 10.0.2.2。
 * 换真板时这三个数从板级配置来，不要在这里改。 */
#ifndef ARKRTOS_NETIF_IP0
#define ARKRTOS_NETIF_IP0       10u
#define ARKRTOS_NETIF_IP1       0u
#define ARKRTOS_NETIF_IP2       2u
#define ARKRTOS_NETIF_IP3       15u
#define ARKRTOS_NETIF_GW3       2u
#define ARKRTOS_NETIF_MASK      24u
#endif

/* 一轮最多抽几帧。16 帧 = 抽满一个接收环；再大也只是让同一轮更久，
 * 节拍反而变粗。 */
#ifndef ARKRTOS_NETIF_POLL_BUDGET
#define ARKRTOS_NETIF_POLL_BUDGET   16u
#endif

/** @brief 收包任务的周期（ms）。1 ms 对 10 Mbit/s 级别的链路足够，
 *         也远大于一次轮询的成本（十几次 MMIO 读）。 */
#ifndef ARKRTOS_NETIF_POLL_PERIOD_MS
#define ARKRTOS_NETIF_POLL_PERIOD_MS    1u
#endif

#define ARKRTOS_NETIF_POLL_PRIO     8u

static struct netif g_netif;
static ip4_addr_t    g_ip;
static ip4_addr_t    g_mask;
static ip4_addr_t    g_gw;

static uint32 s_stack[512];

/* 收包缓冲：只在轮询任务里用 */
static uint8 s_rx_frame[ARKRTOS_NET_DEV_FRAME_MAX];
/* 发送缓冲：只在 tcpip 线程里用（见文件头第 2 条） */
static uint8 s_tx_frame[ARKRTOS_NET_DEV_FRAME_MAX];

static uint32 s_netif_drop;
static int32  s_started;

static err_t netif_linkoutput(struct netif *n, struct pbuf *p)
{
    u16_t total;

    (void)n;

    if(p == NULL)
    {
        return ERR_ARG;
    }

    total = p->tot_len;
    if(total > ARKRTOS_NET_DEV_FRAME_MAX)
    {
        s_netif_drop++;
        return ERR_BUF;                 /* 超长帧：如实报错，不切片 */
    }

    /* pbuf_copy_partial 处理链式 pbuf，所以这里不能用 p->payload 直接读。 */
    (void)pbuf_copy_partial(p, s_tx_frame, total, 0);

    if(arkrtos_net_dev_tx(s_tx_frame, total) != 0)
    {
        s_netif_drop++;
        return ERR_IF;
    }

    return ERR_OK;
}

/* netif_add 之后的状态：-1 失败、0 成功。只在 tcpip 线程里写。 */
static int32 s_netif_state = -1;

static err_t netif_init_cb(struct netif *n)
{
    const uint8 *mac;
    uint32 i;

    n->mtu = 1500u;
    n->hwaddr_len = 6u;

    mac = arkrtos_net_dev_mac();
    if(mac == 0)
    {
        return ERR_IF;                  /* 没有 MAC 就没有网卡 */
    }

    for(i = 0u; i < 6u; i++)
    {
        n->hwaddr[i] = mac[i];
    }

    /* NETIF_FLAG_ETHERNET 不是装饰：tcpip_input() 只看它（与 ETHARP 一起）
     * 决定投递后在 tcpip 线程里走 ethernet_input 还是 ip_input。少了它，
     * 以太网头会被当成 IP 头解析。 */
    n->flags = (u8_t)(NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                      NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP);
    n->output = etharp_output;
    n->linkoutput = netif_linkoutput;

    return ERR_OK;
}

uint32 arkrtos_netif_link_poll(void)
{
    uint32 n;
    uint32 done = 0u;

    for(n = 0u; n < ARKRTOS_NETIF_POLL_BUDGET; n++)
    {
        uint16 len = 0u;
        int32 rc = arkrtos_net_dev_rx(s_rx_frame,
                                      (uint16)ARKRTOS_NET_DEV_FRAME_MAX, &len);
        struct pbuf *p;

        if(rc <= 0)
        {
            break;
        }

        p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        if(p == NULL)
        {
            s_netif_drop++;
            continue;
        }

        if(pbuf_take(p, s_rx_frame, len) != ERR_OK)
        {
            s_netif_drop++;
            pbuf_free(p);
            continue;
        }

        /* tcpip_input() 只把包投进 tcpip 邮箱，协议栈仍在 tcpip 线程里跑。
         * 它内部走 tcpip_inpkt()：邮箱满 / memp 用完时**不**释放 pbuf（只
         * 释放自己那条消息），所有权还在调用者手上 —— 这里必须自己放，
         * 否则每次拥塞都静默漏一帧。 */
        if(tcpip_input(p, &g_netif) != ERR_OK)
        {
            s_netif_drop++;
            pbuf_free(p);
        }
        done++;
    }

    return done;
}

void arkrtos_netif_link_stats(uint32 *tx_ok, uint32 *tx_drop,
                              uint32 *rx_ok, uint32 *rx_drop)
{
    uint32 rt = 0u;
    uint32 rd = 0u;
    uint32 rr = 0u;
    uint32 rxdr = 0u;

    arkrtos_net_dev_stats(&rt, &rd, &rr, &rxdr);

    if(tx_ok != 0)
    {
        *tx_ok = rt;
    }
    if(tx_drop != 0)
    {
        *tx_drop = rd + s_netif_drop;
    }
    if(rx_ok != 0)
    {
        *rx_ok = rr;
    }
    if(rx_drop != 0)
    {
        /* 驱动丢的 + netif 层丢的（pbuf 分配失败 / 投递失败）。只报驱动那一半
         * 会让"驱动收了 4 帧、协议栈一帧没见"这种情形在读数上完全消失。 */
        *rx_drop = rxdr + s_netif_drop;
    }
}

static void netif_poll_task(void)
{
    for(;;)
    {
        (void)arkrtos_netif_link_poll();
        arkrtos_task_wait_period_internal();
    }
}

/* 在 tcpip 线程里执行的实际装配。netif_add / netif_set_up 都不是线程安全的，
 * 所以它们必须跑在这里，而不是调用者那个任务里。 */
static void netif_add_cb(void *arg)
{
    (void)arg;

    IP4_ADDR(&g_ip,   ARKRTOS_NETIF_IP0, ARKRTOS_NETIF_IP1,
             ARKRTOS_NETIF_IP2, ARKRTOS_NETIF_IP3);
    IP4_ADDR(&g_gw,   ARKRTOS_NETIF_IP0, ARKRTOS_NETIF_IP1,
             ARKRTOS_NETIF_IP2, ARKRTOS_NETIF_GW3);
    IP4_ADDR(&g_mask, 255u, 255u, 255u,
             (255u << (32u - ARKRTOS_NETIF_MASK)) & 255u);

    if(netif_add(&g_netif, &g_ip, &g_mask, &g_gw, NULL,
                 netif_init_cb, tcpip_input) == NULL)
    {
        s_netif_state = -1;
        ARKRTOS_LOGE("NET", "netif_add failed, no real netif");
        return;
    }

    netif_set_default(&g_netif);
    netif_set_up(&g_netif);
    s_netif_state = 0;
    ARKRTOS_LOGI("NET", "netif up: %s ip 10.0.2.15/24 gw 10.0.2.2",
                 arkrtos_net_dev_name());
}

int32 arkrtos_netif_link_start(void)
{
    int32 rc;
    int32 id;

    if(s_started != 0)
    {
        return s_started;
    }

    rc = arkrtos_net_dev_init();
    if(rc != 0)
    {
        /* 没网卡就不建 netif：一个"I/O 永远失败"的网卡比"没有网卡"
         * 更难查，因为它看起来是存在的。 */
        s_started = -1;
        return -1;
    }

    /* 异步投递：这里只能判断"消息进没进邮箱"，装配结果看回调的日志。 */
    if(tcpip_callback(netif_add_cb, NULL) != ERR_OK)
    {
        s_started = -2;
        return -2;
    }

    id = arkrtos_task_register(netif_poll_task, s_stack,
                              (uint32)sizeof(s_stack),
                              (uint8)ARKRTOS_NETIF_POLL_PRIO,
                              (uint32)ARKRTOS_NETIF_POLL_PERIOD_MS);
    if(id < 0)
    {
        s_started = -4;
        return -4;
    }

    s_started = 0;
    return 0;
}

#endif /* ARKRTOS_USE_NET_VIRTIO */
