/**
* @file test_smp_shootdown_host.c
* @brief 跨核 TLB 失效 + 回执（R4）的宿主单元测试
* @details 被测的是真实的 arkrtos_smp_shootdown.c；port / sched 层用本文件的
*          可编程桩代替（核号、在线核数与掩码、IPI 投递、窗口账本、装窗口）。
*
*          「IPI 真的投递到另一个核」这件事在宿主上不存在，所以桩把
*          arkrtos_smp_send_ipi() 做成**同步调用目标核的处理函数**：
*          g_cpu_id 临时切到目标核 → arkrtos_smp_sd_target_poll() → 切回。
*          这正好把协议里最要命的一条摆到台面上 —— 回执是**目标核**写的，
*          发起核只是读；发完就返回 0 的假实现过不了本文件。
*
*          已验证：
*            参数拒绝（页数 0 / VA 未 4 KB 对齐）、没有第二个在线核、
*            正常回执（撤 + 重铺各一次、ack/inval 计数、last_rc=0）、
*            目标核空间不符（skip：不回执也要记账、不装窗口）、
*            装窗口失败（fail：照样回执，last_rc 如实为负）、
*            drop 负控制（看了不回执 ⇒ 发起核只能拿到 -2）、
*            IPI 发不出去（-3）、同一条请求不重复处理（幂等）、
*            发起核绝不碰自己的槽、只发给在线核。
*          未验证：真实 IPI 投递延迟、真实 TLB 条目是否消失（只有真机能证）、
*            并发跨核竞争（宿主是单线程模拟）。
* @note 用法：由 tools/coverage_gate.py 的 smpshootdown 档编译并运行，
*       输出 SMP-SD-HOST OK。
*/

#include "arkrtos_smp.h"
#include "arkrtos_smp_shootdown.h"
#include "arkrtos_smp_sched.h"
#include "arkrtos_hal.h"

#include <stdio.h>

#if (ARKRTOS_USE_SMP != 1)
#error "must be compiled with -DARKRTOS_USE_SMP=1"
#endif
#if (ARKRTOS_USE_SMP_TLB_SD != 1)
#error "must be compiled with -DARKRTOS_USE_SMP_TLB_SD=1"
#endif

static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

/* ---------------------------------------------------------------- 桩 */
static uint32 g_cpu_id      = 0u;
static uint32 g_online      = 2u;
static uint32 g_mask        = 0x3u;
static int    g_ipi_fail    = 0;
static int    g_ipi_calls   = 0;
static uint32 g_ipi_last    = 0xFFFFFFFFu;
static uint32 g_ipi_seen[ARKRTOS_CPU_NUM];

static uint32 g_win[ARKRTOS_CPU_NUM];
static uint32 g_win_default = 0xFFFFFFFFu;

static int    g_install_calls = 0;
static int    g_install_rc    = 0;
static uint32 g_install_space = 0xFFFFFFFFu;

uint32 arkrtos_port_cpu_id(void)
{
    return g_cpu_id;
}

uint32 arkrtos_smp_online_count(void)
{
    return g_online;
}

uint32 arkrtos_smp_online_mask_get(void)
{
    return g_mask;
}

/* 同步投递：把「目标核处理请求」搬进发起核的调用栈里。 */
int32 arkrtos_smp_send_ipi(uint32 cpu)
{
    uint32 save = g_cpu_id;

    g_ipi_calls++;
    g_ipi_last = cpu;
    if(cpu < (uint32)ARKRTOS_CPU_NUM)
    {
        g_ipi_seen[cpu]++;
    }
    if(g_ipi_fail != 0)
    {
        return -1;
    }
    if(cpu < (uint32)ARKRTOS_CPU_NUM)
    {
        g_cpu_id = cpu;
        arkrtos_smp_sd_target_poll();
        g_cpu_id = save;
    }
    return 0;
}

uint32 arkrtos_smp_sched_win_space(uint32 cpu)
{
    if(cpu >= (uint32)ARKRTOS_CPU_NUM)
    {
        return 0xFFFFFFFFu;
    }
    return g_win[cpu];
}

int32 arkrtos_port_paging_install_space_window(uint32 space)
{
    g_install_calls++;
    g_install_space = space;
    return g_install_rc;
}

/* ---------------------------------------------------------------- 工具 */
static void reset_all(void)
{
    uint32 i;

    g_cpu_id        = 0u;
    g_online        = 2u;
    g_mask          = 0x3u;
    g_ipi_fail      = 0;
    g_ipi_calls     = 0;
    g_ipi_last      = 0xFFFFFFFFu;
    g_install_calls = 0;
    g_install_rc    = 0;
    g_install_space = 0xFFFFFFFFu;

    for(i = 0u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        g_ipi_seen[i] = 0u;
        g_win[i]      = g_win_default;
    }

    arkrtos_smp_sd_drop = 0u;
    arkrtos_smp_sd_slot[0].seq = 0u;
    arkrtos_smp_sd_slot[0].ack = 0u;
}

int main(void)
{
    uint32 ack0;
    uint32 inval0;
    uint32 skip0;
    uint32 fail0;
    uint32 to0;
    int32  rc;
    uint32 i;

    /* --- 1) 参数拒绝：页数 0 / 未页对齐，都不许发 IPI --- */
    reset_all();
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 0u);
    ck(rc == -1, "pages=0 must be rejected with -1");
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000800ul, 1u);
    ck(rc == -1, "unaligned va must be rejected with -1");
    ck(g_ipi_calls == 0, "a rejected request must not send any IPI");

    /* --- 2) 没有第二个在线核：拒绝，不假装成功 --- */
    reset_all();
    g_online = 1u;
    g_mask   = 0x1u;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == -1, "a single-core system must answer -1");
    ck(g_ipi_calls == 0, "no IPI when there is no target core");

    /* --- 3) 正常路径：目标核同空间 ⇒ 真撤 + 回执 --- */
    reset_all();
    g_win[1] = 5u;
    ack0   = arkrtos_smp_sd_ack;
    inval0 = arkrtos_smp_sd_inval;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == 0, "a handled request must return 0");
    ck(arkrtos_smp_sd_ack == ack0 + 1u, "the target core wrote one ack");
    ck(arkrtos_smp_sd_inval == inval0 + 1u, "the target core really invalidated");
    ck(g_install_calls == 1, "install_space_window called exactly once");
    ck(g_install_space == 5u, "install_space_window got the requested space");
    ck(arkrtos_smp_sd_slot[1].last_rc == 0u, "last_rc records the success");
    ck(arkrtos_smp_sd_slot[0].seq == 0u, "the initiator must not use its own slot");
    ck(g_ipi_last == 1u, "the IPI went to the other core");

    /* --- 4) 目标核空间不符：skip —— 记账、不装窗口、照样回执 --- */
    reset_all();
    g_win[1] = 7u;
    ack0  = arkrtos_smp_sd_ack;
    skip0 = arkrtos_smp_sd_skip;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == 0, "a skipped request is still acknowledged");
    ck(arkrtos_smp_sd_skip == skip0 + 1u, "skip counter bumped");
    ck(arkrtos_smp_sd_ack == ack0 + 1u, "skip still writes the ack");
    ck(g_install_calls == 0, "nothing to install when the space does not match");
    ck(arkrtos_smp_sd_slot[1].last_rc == 1u, "last_rc=1 means nothing to drop");

    /* --- 5) 账本说「不知道」（0xFFFFFFFF）：也不许当成空间 0 --- */
    reset_all();
    g_win[1] = 0xFFFFFFFFu;
    skip0 = arkrtos_smp_sd_skip;
    rc = arkrtos_smp_tlb_shootdown(0u, 0x2000000ul, 1u);
    ck(rc == 0, "unknown window is acknowledged, not silently dropped");
    ck(arkrtos_smp_sd_skip == skip0 + 1u, "unknown window counts as a skip");
    ck(g_install_calls == 0, "unknown window must not install space 0");

    /* --- 6) 装窗口失败：照样回执，但记账为 fail、last_rc 为负 --- */
    reset_all();
    g_win[1]      = 5u;
    g_install_rc  = -5;
    fail0 = arkrtos_smp_sd_fail;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == 0, "a failing install still acknowledges");
    ck(arkrtos_smp_sd_fail == fail0 + 1u, "install failure is recorded as fail");
    ck((int32)arkrtos_smp_sd_slot[1].last_rc == -5,
       "last_rc carries the negative return code");

    /* --- 7) drop 负控制：看了不回执 ⇒ 发起核只能拿到 -2 --- */
    reset_all();
    g_win[1] = 5u;
    arkrtos_smp_sd_drop = 1u;
    to0   = arkrtos_smp_sd_timeout;
    ack0  = arkrtos_smp_sd_ack;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == -2, "no ack must be reported as -2, never as success");
    ck(arkrtos_smp_sd_timeout == to0 + 1u, "timeout counter bumped");
    ck(arkrtos_smp_sd_ack == ack0, "drop must not write any ack");
    ck(g_install_calls == 0, "drop stops before the invalidation work");

    /* --- 8) IPI 发不出去：-3，不许等一个永远不来的回执 --- */
    reset_all();
    g_win[1]    = 5u;
    g_ipi_fail  = 1;
    to0 = arkrtos_smp_sd_timeout;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == -3, "a failed IPI must be reported as -3");
    ck(arkrtos_smp_sd_timeout == to0, "no timeout is charged for a failed send");

    /* --- 9) 幂等：同一条请求处理过就不再处理 --- */
    reset_all();
    g_win[1] = 5u;
    ack0 = arkrtos_smp_sd_ack;
    g_cpu_id = 1u;
    arkrtos_smp_sd_slot[1].space = 5u;
    arkrtos_smp_sd_slot[1].seq   = 77u;
    arkrtos_smp_sd_target_poll();
    ck(arkrtos_smp_sd_ack == ack0 + 1u, "first poll handles the request");
    ck(g_install_calls == 1, "first poll does the invalidation");
    arkrtos_smp_sd_target_poll();
    ck(arkrtos_smp_sd_ack == ack0 + 1u, "the same seq is not acknowledged twice");
    ck(g_install_calls == 1, "the same seq is not invalidated twice");
    g_cpu_id = 0u;

    /* --- 10) 空闲的槽（seq==0）不产生任何副作用 --- */
    reset_all();
    g_cpu_id = 1u;
    ack0 = arkrtos_smp_sd_ack;
    arkrtos_smp_sd_target_poll();
    ck(arkrtos_smp_sd_ack == ack0, "an empty slot is not acknowledged");
    g_cpu_id = 0u;

    /* --- 11) 只发给在线核 --- */
    reset_all();
    g_online = 2u;
    g_mask   = 0x1u;             /* 核 1 不在线 */
    g_win[1] = 5u;
    rc = arkrtos_smp_tlb_shootdown(5u, 0x2000000ul, 1u);
    ck(rc == -1, "no online target means -1 even if CPU_NUM says 2");
    ck(g_ipi_calls == 0, "no IPI to an offline core");

    /* --- 12) 两核都在线时每核一条请求（CPU_NUM 档内的遍历正确） --- */
    reset_all();
    for(i = 1u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        g_win[i] = 3u;
    }
    g_mask = (1u << ARKRTOS_CPU_NUM) - 1u;
    g_online = (uint32)ARKRTOS_CPU_NUM;
    rc = arkrtos_smp_tlb_shootdown(3u, 0x2000000ul, 2u);
    ck(rc == 0, "all online cores acknowledged");
    for(i = 1u; i < (uint32)ARKRTOS_CPU_NUM; i++)
    {
        ck(g_ipi_seen[i] == 1u, "each online core got exactly one IPI");
        ck(arkrtos_smp_sd_slot[i].pages == 2u, "pages reached every slot");
    }
    ck(g_ipi_seen[0] == 0u, "the initiator never sends to itself");

    if(g_fails != 0)
    {
        printf("SMP-SD-HOST FAIL: %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("SMP-SD-HOST OK (%d checks)\n", g_checks);
    return 0;
}
