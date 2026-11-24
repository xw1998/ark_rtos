/**
* @file lwip_port.c
* @brief 把 lwIP 起起来。
* @details 这块板上没有以太网 PHY（仓库里只有 CubeMX 留下的 ETH/PHY 宏，没有
*          任何 ETH 初始化），所以现在的网络面是**回环**：lwIP 自己带一张 lo
*          网卡，由 lwip_init() 里的 netif_init() 建好（127.0.0.1/8，input 接
*          tcpip_input）。这不影响 socket/select 的语义验证——连接、收发、
*          关闭、超时走的是同一条 TCP 状态机，只是包不出芯片。
*
*          这里只做一件事：**在一个内核任务里调一次 tcpip_init()**。
*          为什么不能放在 main()：tcpip_init() 会等 tcpip 线程把初始化做完
*          （内部一个信号量），而 main() 跑在调度器启动之前，等不到任何任务。
*
*          为什么不需要周期调 netif_poll()：回环是
*          LWIP_NETIF_LOOPBACK_MULTITHREADING=1（见 lwipopts.h 里的说明），
*          netif_loop_output() 会用 tcpip_try_callback() 把 netif_poll 排给
*          tcpip 线程，包由它自己喂回协议栈。只有多线程 =0 的轮询模式下，
*          "没人调 netif_poll_all()" 才会表现为"连接永远建不起来且不报错"。
*
* @author xw
* @date 2026.09.23
*/
#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"

#include "arkrtos.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"      /* wait_period_internal */
#include "arkrtos_log.h"

#include "lwip_port.h"
#include "arkrtos_net.h"      /* socket service: starts once tcpip is alive */

#if (ARKRTOS_USE_NET_VIRTIO == 1)
#include "lwip_netif_link.h"  /* real NIC -> lwIP netif (off by default) */
#endif

#if (ARKRTOS_USE_LWIP == 1)

/* 服务任务的栈。默认档只需要 1 KB（只调一次 tcpip_init），但接真实网卡
 * 之后这一轮还要跑 tcpip_callback(netif_add) 与一行带格式化的日志，
 * 实测 1 KB 会打穿（STK 守卫当场报 "stack bottom flag overwritten"，
 * 而被打穿的是相邻的 .bss —— 症状看起来像协议栈内部断言，其实是栈溢出）。
 * 所以只在开网卡时把栈加到 4 KB，默认档的 RAM 占用一个字节都不多。 */
/* 开 socket 翻译层（U-23）时也要 4 KB：这一档靠 lwIP 的回环网卡跑 TCP/UDP，
 * tcpip_init 这一轮除了建 tcpip 线程，还要装 lo 网卡并挂上 loopif 的
 * tcpip_try_callback 通路。实测 1 KB 会打穿 —— 症状是相邻 .bss 被踩，
 * 先看到 lwIP 自己报 "memp_free: mem properly aligned" 断言，随后 lwIP
 * 任务在 era=0x23 上 INE 停机（栈底守卫字从 0x0000ed01 变成 0x0000000d）。
 * 关档时仍然只占 1 KB。 */
#if (ARKRTOS_USE_NET_VIRTIO == 1) || (defined(ARKRTOS_USE_LINUX_SOCKET) && \
                                      (ARKRTOS_USE_LINUX_SOCKET == 1))
static uint32 g_lwip_task_stack[1024];   /* 4 KB：tcpip_init + 回环/网卡装配 */
#else
static uint32 g_lwip_task_stack[256];    /* 1 KB：只调一次 tcpip_init */
#endif

static void arkrtos_lwip_task(void)
{
    /* 第一轮：把 lwIP 与 tcpip 线程起起来。tcpip_init() 会等线程把初始化
     * 做完（内部一个信号量），所以这一轮比其他轮久——本任务由内核按周期
     * 调度，第一轮超时不影响后续节拍。 */
    tcpip_init(NULL, NULL);
    ARKRTOS_LOGI("LWIP", "tcpip up, loopif 127.0.0.1");

    /* Start the socket service task here and nowhere else: it must not
     * accept a request before the tcpip thread exists, and this is the only
     * point that knows that happened. */
    (void)arkrtos_net_start();

#if (ARKRTOS_USE_NET_VIRTIO == 1)
    /* Real Ethernet NIC: bring it up and attach a netif.  It must happen
     * here and not earlier: netif_add() runs on the tcpip thread, and this
     * is the first point that knows the tcpip thread exists.  A failure is
     * logged by the link layer and leaves the loopback netif working, so it
     * is reported but not fatal. */
    if(arkrtos_netif_link_start() != 0)
    {
        ARKRTOS_LOGE("LWIP", "netif link start failed, loopback only");
    }
#endif

    /* 之后没有周期工作。任务不能退出（内核没有"结束自己"这个语义），所以
     * 按周期躺着；将来接真实以太网驱动时，收包搬运就落在这个循环里。 */
    for(;;)
    {
        arkrtos_task_wait_period_internal();
    }
}

int32 arkrtos_lwip_start(void)
{
    return arkrtos_task_register(arkrtos_lwip_task,
                               g_lwip_task_stack,
                               (uint32)sizeof(g_lwip_task_stack),
                               (uint8)ARKRTOS_LWIP_TASK_PRIO,
                               (uint32)ARKRTOS_LWIP_TASK_PERIOD_MS);
}

#else   /* ARKRTOS_USE_LWIP != 1 */

int32 arkrtos_lwip_start(void)
{
    return 0;       /* 关掉时不注册任务，也不碰 lwIP 的符号 */
}

#endif  /* ARKRTOS_USE_LWIP */
