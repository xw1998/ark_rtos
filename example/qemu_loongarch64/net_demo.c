/**
* @file net_demo.c
* @brief 真实以太网收发演示：对网关 10.0.2.2 发三次 ICMP echo（默认不编）
* @details 这段代码只做一件事，但它是这条欠账的判据：**证明帧真的出了芯片
*          又回来了**。判据不是"我调用了发送函数"，而是三件可以核对的事：
*
*            1) 串口上有 `[net] ping seq=N`（发了第 N 个 echo request）；
*            2) 串口上有 `[net] reply from 10.0.2.2 seq=N len=..`（对端回了）；
*            3) QEMU 的 filter-dump 抓到的 pcap 里有 ARP request/reply 与
*               ICMP echo request/echo reply 四类帧，且 MAC/IP 与我们设的
*               一致。
*
*          第 3 条单独由 tools/net_virtio_gate.py 解析 pcap 核对。少了它，
*          "ping 通了"这句话可能只是因为回环网卡把包又喂回了自己。
*
*          三个实现约束：
*            - raw API 的 pcb 创建与收包回调都跑在 tcpip 线程里：本工程
*              LWIP_TCPIP_CORE_LOCKING=0，协议栈靠 tcpip 线程串行化，别的
*              线程直接碰 pcb 是数据竞争。所以创建与发送都包在
*              tcpip_callback() 里。注意这个 lwIP 版本（STABLE-2_1_3）的
*              tcpip_callback() 是**异步**的：投完消息就返回，不等回调执行。
*              于是"pcb 建好了没有"只能看回调自己写的标志，本任务按周期
*              轮询那个标志，而不是读 tcpip_callback 的返回值。
*            - raw 回调拿到的 pbuf **仍带着 IP 头**：本版本 ip4_input 在摘
*              头之前就调 raw_input，所以回调里 d[0] 是 0x45 而不是 ICMP
*              type。要按 IHL 自己跳过 IP 头（见 net_demo_recv 的注释）。
*              这一点最贵：照"回调里就是 ICMP 报文"写，代码不报错、抓包也
*              有回包，只是 raw pcb 一次都不吃。
*            - ICMP 校验和自己算：lwIP 不对 raw ICMP 算校验和（那是
*              apps/ping 的事，而 src/apps/ 没有 vendored 进来）。算错的话
*              对端直接丢掉，症状与"网卡没通"一模一样，所以这里必须自己
*              逐一字节算。
*            - 目标地址写死 10.0.2.2：QEMU 的用户态网络（slirp）固定网段
*              10.0.2.0/24，网关与宿主都是 10.0.2.2，它会应答 ICMP echo。
*
*          开关：ARKRTOS_NET_DEMO=1（未定义 = 不编，镜像里一个字节都不多）。
*          依赖 ARKRTOS_USE_NET_VIRTIO=1，否则链接期就会缺网卡符号。
*
* @author xw
*/

#include "arkrtos_features.h"
#include "arkrtos_types.h"

#if defined(ARKRTOS_NET_DEMO) && (ARKRTOS_NET_DEMO == 1)

#if (ARKRTOS_USE_NET_VIRTIO != 1)
#error "ARKRTOS_NET_DEMO=1 requires ARKRTOS_USE_NET_VIRTIO=1 (no NIC, nothing to ping over)"
#endif

#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/prot/icmp.h"

#include "arkrtos.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_board.h"
#include "arkrtos_net_dev.h"

/* QEMU 用户态网络的网关（也是宿主）：slirp 固定 10.0.2.2，会应答 echo。 */
#define NET_DEMO_TGT_0      10u
#define NET_DEMO_TGT_1      0u
#define NET_DEMO_TGT_2      2u
#define NET_DEMO_TGT_3      2u

#define NET_DEMO_COUNT      3u          /* 发几次 echo request */
#define NET_DEMO_PAYLOAD    32u         /* 每次的负载字节数（对齐用 4 的倍数） */
#define NET_DEMO_STACK      512u        /* 2 KB 栈：只打日志与等回调 */
#define NET_DEMO_PRIO       13u
/* 周期 250 ms，每次发完等两个周期（500 ms）。QEMU 上往返在毫秒级，
 * 留 500 ms 是给 ARP（第一发之前要先解析 10.0.2.2 的 MAC）。 */
#define NET_DEMO_PERIOD_MS  250u
#define NET_DEMO_REPLY_WAIT 6u          /* 收尾再等 6 个周期（1.5 s）看迟到回包 */
#define NET_DEMO_SETUP_WAIT 8u          /* 等 tcpip 线程把 pcb 建起来（8 个周期 = 2 s） */

static uint32 s_stack[NET_DEMO_STACK];

static struct raw_pcb *s_pcb;
static ip_addr_t       s_dst;

static uint32 s_sent;                   /* 已发出的请求数 */
static uint32 s_replies;                /* 收到的回包数 */
static uint16 s_last_len;               /* 最后一个回包的 ICMP 长度 */
static uint32 s_registered;             /* pcb 建好且目标地址已填 */
/* setup 的完成状态：0 = 还没跑、1 = 成功、-1 = raw_new 失败。
 * 只在 tcpip 线程里写、在演示任务里读——两个任务之间的一个 int，读写的
 * 先后由调度器切换保证（不需要原子）。 */
static volatile int32 s_setup_state;

/* ------------------------------------------------------------------
 * ICMP 校验和：16 位反码和的反码。
 * 逐字节拼而不是直接读 u16：本 port 编了 -mstrict-align，且 ICMP 头里的
 * 字段在报文里不一定按 2 字节对齐的写法取，拼字节在任何架构上都对。
 * ------------------------------------------------------------------ */
static uint16 net_demo_cksum(const uint8 *p, uint16 len)
{
    uint32 sum = 0u;
    uint16 i;

    for(i = 0u; (uint16)(i + 1u) < len; i = (uint16)(i + 2u))
    {
        sum += (uint32)((uint32)p[i] << 8) | (uint32)p[i + 1u];
    }

    if((len & 1u) != 0u)
    {
        sum += (uint32)((uint32)p[len - 1u] << 8);
    }

    while((sum >> 16) != 0u)
    {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }

    return (uint16)(~sum & 0xFFFFu);
}

/* 收包回调：跑在 tcpip 线程里。
 *
 * **p->payload 指向的是 IP 头，不是 ICMP 头**。本 lwIP 版本（STABLE-2_1_3）
 * 的 ip4_input 在 `pbuf_remove_header()` 之前就调 raw_input()，也就是"raw
 * 回调拿到的是整条 IP 报文"。实测第一字节是 0x45（版本 4 + IHL 5），不是
 * ICMP type —— 照"回调里 d[0] 就是 ICMP type"写会永远判成"不是回包"，
 * 而症状与"网卡没通"完全一样（raw pcb 一次都不吃包、ping 0/3）。所以这里
 * 自己按 IHL 跳过 IP 头。 */
static u8_t net_demo_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p,
                          const ip_addr_t *addr)
{
    uint8 *d;
    uint16 hlen;
    uint16 seq;
    uint32 i;
    uint32 off;

    (void)arg;
    (void)pcb;

    if((p == NULL) || (p->tot_len < 20u))
    {
        return 0u;                      /* 不吃：让别的 raw pcb 试 */
    }

    d = (uint8 *)p->payload;
    hlen = (uint16)((uint16)(d[0] & 0x0Fu) * 4u);
    if((hlen < 20u) || (((uint32)hlen + 8u) > (uint32)p->tot_len))
    {
        return 0u;
    }

    /* type 0 = echo reply。不是回包就不吃（ICMP 别的类型这里不关心）。 */
    if(d[hlen] != 0u)
    {
        return 0u;
    }

    /* bswap：ICMP 的 id/seq 是网络序，这里按字节拼。 */
    seq = (uint16)(((uint16)d[hlen + 6u] << 8) | (uint16)d[hlen + 7u]);

    s_replies++;
    s_last_len = (uint16)p->tot_len;

    arkrtos_board_puts("[net] reply from ");
    arkrtos_board_putu((uint32)ip4_addr1(ip_2_ip4(addr)));
    arkrtos_board_putc('.');
    arkrtos_board_putu((uint32)ip4_addr2(ip_2_ip4(addr)));
    arkrtos_board_putc('.');
    arkrtos_board_putu((uint32)ip4_addr3(ip_2_ip4(addr)));
    arkrtos_board_putc('.');
    arkrtos_board_putu((uint32)ip4_addr4(ip_2_ip4(addr)));
    arkrtos_board_puts(" seq=");
    arkrtos_board_putu((uint32)seq);
    arkrtos_board_puts(" len=");
    arkrtos_board_putu((uint32)p->tot_len);
    arkrtos_board_puts(" data=\"");

    /* 回显内容按"字节值 = 0x40 + i"填的，这里读回来核对：能对上说明
     * 负载真的穿过了两趟网络，而不是某个缓冲里的残值。 */
    off = (uint32)hlen + 8u;
    for(i = 0u; (off < (uint32)p->tot_len) && (i < NET_DEMO_PAYLOAD); i++, off++)
    {
        arkrtos_board_putc((char)d[off]);
    }
    arkrtos_board_puts("\"\n");

    pbuf_free(p);
    return 1u;                          /* 吃掉了 */
}

/* 建 pcb（tcpip 线程）。结果写进 s_setup_state，由任务轮询。 */
static void net_demo_setup(void *arg)
{
    (void)arg;

    s_pcb = raw_new(IP_PROTO_ICMP);
    if(s_pcb == NULL)
    {
        s_setup_state = -1;
        arkrtos_board_puts("[net] raw_new(ICMP) failed\n");
        return;
    }

    raw_recv(s_pcb, net_demo_recv, NULL);

    IP_ADDR4(&s_dst, NET_DEMO_TGT_0, NET_DEMO_TGT_1,
             NET_DEMO_TGT_2, NET_DEMO_TGT_3);

    s_registered  = 1u;
    s_setup_state = 1;
}

/* 发一个 echo request（tcpip 线程）。arg 就是 seq。 */
static void net_demo_send(void *arg)
{
    uint32 seq = (uint32)(unsigned long)arg;
    struct pbuf *p;
    uint8 *d;
    uint16 ck;
    uint32 i;

    if(s_pcb == NULL)
    {
        return;
    }

    p = pbuf_alloc(PBUF_IP, (u16_t)(8u + NET_DEMO_PAYLOAD), PBUF_RAM);
    if(p == NULL)
    {
        arkrtos_board_puts("[net] send: pbuf_alloc failed\n");
        return;
    }

    d = (uint8 *)p->payload;

    d[0] = 8u;                          /* echo request */
    d[1] = 0u;                          /* code */
    d[2] = 0u;                          /* checksum，先填 0 再算 */
    d[3] = 0u;
    d[4] = 0x41u;                       /* id = "AR" */
    d[5] = 0x52u;
    d[6] = (uint8)((seq >> 8) & 0xFFu);
    d[7] = (uint8)(seq & 0xFFu);

    for(i = 0u; i < NET_DEMO_PAYLOAD; i++)
    {
        d[8u + i] = (uint8)(0x40u + i);  /* 'A','B','C'... 便于肉眼核对 */
    }

    ck = net_demo_cksum(d, (uint16)(8u + NET_DEMO_PAYLOAD));
    d[2] = (uint8)((ck >> 8) & 0xFFu);
    d[3] = (uint8)(ck & 0xFFu);

    if(raw_sendto(s_pcb, p, &s_dst) != ERR_OK)
    {
        arkrtos_board_puts("[net] send failed seq=");
        arkrtos_board_putu(seq);
        arkrtos_board_puts("\n");
        pbuf_free(p);
        return;
    }

    /* raw_sendto 不吃 pbuf 的所有权，成功路径也要自己释放。 */
    pbuf_free(p);

    s_sent++;
    arkrtos_board_puts("[net] ping seq=");
    arkrtos_board_putu(seq);
    arkrtos_board_puts(" -> 10.0.2.2\n");
}

static void net_demo_task(void)
{
    uint32 i;
    uint32 tx_ok = 0u;
    uint32 tx_drop = 0u;
    uint32 rx_ok = 0u;
    uint32 rx_drop = 0u;

    /* 投递 pcb 创建请求。tcpip_callback 是异步的（见文件头），所以这里
     * 只可能知道"请求进没进邮箱"，成功与否看 s_setup_state。 */
    if(tcpip_callback(net_demo_setup, NULL) != ERR_OK)
    {
        s_setup_state = -1;
    }

    for(i = 0u; (i < NET_DEMO_SETUP_WAIT) && (s_setup_state == 0); i++)
    {
        arkrtos_task_wait_period_internal();
    }

    if(s_setup_state != 1)
    {
        arkrtos_board_puts("[net] demo: raw pcb setup failed state=");
        arkrtos_board_putu((uint32)s_setup_state);
        arkrtos_board_puts("\n");
        for(;;)
        {
            arkrtos_task_wait_period_internal();
        }
    }

    for(i = 0u; i < NET_DEMO_COUNT; i++)
    {
        (void)tcpip_callback(net_demo_send, (void *)(unsigned long)(i + 1u));
        /* 两个周期 = 500 ms：第一发要等 ARP 解析完才有回包。 */
        arkrtos_task_wait_period_internal();
        arkrtos_task_wait_period_internal();
    }

    for(i = 0u; i < NET_DEMO_REPLY_WAIT; i++)
    {
        if(s_replies >= NET_DEMO_COUNT)
        {
            break;
        }
        arkrtos_task_wait_period_internal();
    }

    arkrtos_net_dev_stats(&tx_ok, &tx_drop, &rx_ok, &rx_drop);

    arkrtos_board_puts("[net] ping: ");
    arkrtos_board_putu(s_replies);
    arkrtos_board_puts("/");
    arkrtos_board_putu(NET_DEMO_COUNT);
    arkrtos_board_puts((s_replies >= NET_DEMO_COUNT) ? " ok\n" : " FAIL\n");

    arkrtos_board_puts("[net] stats tx=");
    arkrtos_board_putu(tx_ok);
    arkrtos_board_puts(" txdrop=");
    arkrtos_board_putu(tx_drop);
    arkrtos_board_puts(" rx=");
    arkrtos_board_putu(rx_ok);
    arkrtos_board_puts(" rxdrop=");
    arkrtos_board_putu(rx_drop);
    arkrtos_board_puts(" last_len=");
    arkrtos_board_putu((uint32)s_last_len);
    arkrtos_board_puts("\n");
    arkrtos_board_puts("[net] demo: done\n");

    for(;;)
    {
        arkrtos_task_wait_period_internal();
    }
}

int32 arkrtos_net_demo_register(void)
{
    return arkrtos_task_register(net_demo_task, s_stack,
                                 (uint32)sizeof(s_stack),
                                 (uint8)NET_DEMO_PRIO,
                                 (uint32)NET_DEMO_PERIOD_MS);
}

#endif /* ARKRTOS_NET_DEMO */
