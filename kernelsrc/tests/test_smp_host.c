/**
* @file test_smp_host.c
* @brief SMP 骨架（arkrtos_smp.c）的宿主单元测试
* @details 骨架里能验证的部分都在账目上：谁上线了、掩码对不对、越界核号
*          有没有被挡住、启动请求被拒绝时有没有虚报上线、IPI 目标是不是
*          先查过在线、当前核读错时会不会越界写。这些在宿主机上逐条摆得
*          出来，而且它们恰好是"看起来能跑、其实记错账"的那类错误。
*
*          被测的是真实的 arkrtos_smp.c；port 层用本文件的可编程桩代替
*          （核号、启动接受位图、IPI 记录、WFI/开中断计数）。
*
*          覆盖范围（如实说明）：
*            已验证：初始化账目（接受/拒绝分开记）、掩码置位与幂等、
*                    越界核号不落地、IPI 只发在线核、当前核读数越界收敛、
*                    节拍/IPI/空转计数只落在当前核、重复初始化清零、
*                    部分核启动失败时返回值与 started 位。
*            未验证：次核真实启动（arkrtos_smp_secondary_main 永不返回，
*                    宿主上不能调）、核间中断的真实投递与延迟、多核同时
*                    置掩码的 CAS 竞争、次核上的节拍中断。这些只有多核板
*                    能证明，本仓库当前没有任何多核板。
* @note 用法：由 tools/smp_gate.py 编译并运行，输出 SMP-HOST OK。
*/

#include "arkrtos_smp.h"

#include <stdio.h>
#include <stdlib.h>

#if (ARKRTOS_USE_SMP != 1)
#error "test_smp_host.c must be compiled with -DARKRTOS_USE_SMP=1"
#endif

#if (ARKRTOS_CPU_NUM != 4)
#error "test_smp_host.c expects -DARKRTOS_CPU_NUM=4"
#endif

/* ------------------------------------------------------------------
 * 断言与内核/移植层桩
 * ------------------------------------------------------------------ */
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

static uint32 g_cpu_id       = 0u;
static uint32 g_start_accept = 0xfu;   /* bit n = 核 n 接受启动请求 */
static int    g_start_calls  = 0;
static uint32 g_start_last   = 0u;
static int    g_ipi_calls    = 0;
static uint32 g_ipi_last     = 0u;
static int    g_wfi_calls    = 0;
static int    g_irq_enables  = 0;

/* 这几个 port 原语在真实构建里由 arkrtos_hal.h 声明；本文件用桩替代，
 * 仍按同一组原型先声明一遍，好让 -Wmissing-prototypes 对它们是沉默的
 * （门禁口径：桩不许在暗处扩接口）。 */
uint32 arkrtos_port_cpu_id(void);
int32  arkrtos_port_cpu_start(uint32 cpu);
int32  arkrtos_port_send_ipi(uint32 cpu);
void   arkrtos_port_wfi(void);
void   arkrtos_port_enable_irq(void);
void   arkrtos_port_disable_irq(void);
uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value);

uint32 arkrtos_port_cpu_id(void)
{
    return g_cpu_id;
}

int32 arkrtos_port_cpu_start(uint32 cpu)
{
    g_start_calls++;
    g_start_last = cpu;
    return ((g_start_accept >> cpu) & 1u) ? 0 : -1;
}

int32 arkrtos_port_send_ipi(uint32 cpu)
{
    g_ipi_calls++;
    g_ipi_last = cpu;
    return 0;
}

void arkrtos_port_wfi(void)
{
    g_wfi_calls++;
}

void arkrtos_port_enable_irq(void)
{
    g_irq_enables++;
}

void arkrtos_port_disable_irq(void)
{
}

uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
{
    if(*p_addr != expect)
    {
        return 0u;
    }
    *p_addr = new_value;
    return 1u;
}

static void stub_reset(void)
{
    g_cpu_id       = 0u;
    g_start_accept = 0xfu;
    g_start_calls  = 0;
    g_start_last   = 0u;
    g_ipi_calls    = 0;
    g_ipi_last     = 0u;
    g_wfi_calls    = 0;
    g_irq_enables  = 0;
}

static void drain(void)
{
    if(g_fails)
    {
        printf("SMP-HOST FAIL: %d of %d checks failed\n", g_fails, g_checks);
        exit(1);
    }
    printf("SMP-HOST OK (%d checks)\n", g_checks);
}

int main(void)
{
    int32  n;
    uint32 i;

    /* --- 1. 全部核都接受启动请求 -------------------------------- */
    stub_reset();
    n = arkrtos_smp_init();
    ck(n == 4, "init returns 4 accepted cores when every start request is accepted");
    ck(g_start_calls == 3, "init calls the port start hook once per secondary core");
    ck(g_start_last == 3u, "the last start request goes to the last core");
    ck(arkrtos_smp_online_mask_get() == 0x1u, "only CPU0 is online right after init");
    ck(arkrtos_smp_online_count() == 1u, "online count is 1 right after init");
    ck(arkrtos_smp_cpu[0].online == 1u, "CPU0 marks itself online");
    ck(arkrtos_smp_cpu[0].started == 1u, "CPU0 marks itself started");
    for(i = 1u; i < 4u; i++)
    {
        ck(arkrtos_smp_cpu[i].started == 1u, "secondary core records its start request");
        ck(arkrtos_smp_cpu[i].online == 0u, "start request accepted is not the same as online");
    }

    /* --- 2. 上线置位与幂等 -------------------------------------- */
    arkrtos_smp_cpu_online(2u);
    ck(arkrtos_smp_cpu[2].online == 1u, "cpu_online sets the core online");
    ck(arkrtos_smp_online_mask_get() == 0x5u, "online mask sets the right bit");
    ck(arkrtos_smp_online_count() == 2u, "online count follows the mask");
    arkrtos_smp_cpu_online(2u);
    ck(arkrtos_smp_online_mask_get() == 0x5u, "cpu_online is idempotent");
    ck(arkrtos_smp_online_count() == 2u, "idempotent online does not double count");

    /* --- 3. 越界核号不落地 -------------------------------------- */
    arkrtos_smp_cpu_online(4u);
    ck(arkrtos_smp_online_mask_get() == 0x5u, "out-of-range core does not touch the mask");
    arkrtos_smp_cpu_online(0xffffffffu);
    ck(arkrtos_smp_online_mask_get() == 0x5u, "huge core id does not touch the mask");

    /* --- 4. IPI 只发在线核 -------------------------------------- */
    g_ipi_calls = 0;
    ck(arkrtos_smp_send_ipi(1u) == -1, "IPI to an offline core is refused");
    ck(g_ipi_calls == 0, "refused IPI never reaches the port layer");
    ck(arkrtos_smp_send_ipi(2u) == 0, "IPI to an online core is sent");
    ck(g_ipi_calls == 1 && g_ipi_last == 2u, "IPI reaches the port layer with the target core");
    ck(arkrtos_smp_send_ipi(9u) == -1, "IPI to an out-of-range core is refused");
    ck(arkrtos_smp_send_ipi(4u) == -1, "IPI to the first out-of-range id is refused");

    /* --- 5. 当前核读数：正常与越界收敛 -------------------------- */
    g_cpu_id = 2u;
    ck(arkrtos_smp_cpu_self() == &arkrtos_smp_cpu[2], "cpu_self follows the port core id");
    g_cpu_id = 7u;
    ck(arkrtos_smp_cpu_self() == &arkrtos_smp_cpu[0],
       "a misreported core id collapses to core 0 instead of writing out of bounds");
    g_cpu_id = 0xffffffffu;
    ck(arkrtos_smp_cpu_self() == &arkrtos_smp_cpu[0], "an all-ones core id also collapses to core 0");

    /* --- 6. 计数只落在当前核 ------------------------------------ */
    g_cpu_id = 2u;
    arkrtos_smp_cpu[2].tick = 0u;
    arkrtos_smp_cpu[0].tick = 0u;
    arkrtos_smp_tick();
    arkrtos_smp_tick();
    arkrtos_smp_tick();
    ck(arkrtos_smp_cpu[2].tick == 3u, "tick counts on the current core");
    ck(arkrtos_smp_cpu[0].tick == 0u, "tick does not leak onto core 0");

    arkrtos_smp_cpu[2].ipi_count = 0u;
    arkrtos_smp_ipi_handler();
    arkrtos_smp_ipi_handler();
    ck(arkrtos_smp_cpu[2].ipi_count == 2u, "default IPI handler counts on the current core");

    g_wfi_calls = 0;
    arkrtos_smp_cpu[2].idle_count = 0u;
    arkrtos_smp_idle_loop();
    ck(arkrtos_smp_cpu[2].idle_count == 1u, "idle loop counts on the current core");
    ck(g_wfi_calls == 1, "idle loop waits for an interrupt");

    /* --- 7. 重复初始化把账清零 ---------------------------------- */
    stub_reset();
    arkrtos_smp_cpu[2].tick      = 99u;
    arkrtos_smp_cpu[2].ipi_count = 99u;
    n = arkrtos_smp_init();
    ck(n == 4, "re-init returns the same accepted count");
    ck(arkrtos_smp_online_mask_get() == 0x1u, "re-init resets the online mask to CPU0 only");
    ck(arkrtos_smp_cpu[2].online == 0u, "re-init clears a previously online secondary core");
    ck(arkrtos_smp_cpu[2].tick == 0u, "re-init clears the tick counter");
    ck(arkrtos_smp_cpu[2].ipi_count == 0u, "re-init clears the IPI counter");
    ck(arkrtos_smp_cpu[0].current_task_id == 0u, "no core claims a current task at boot");

    /* --- 8. 部分核拒绝启动：返回值与 started 必须分开 ------------- */
    stub_reset();
    g_start_accept = 0x5u;               /* 只有核 0 与核 2 接受 */
    n = arkrtos_smp_init();
    ck(n == 2, "refused start requests are not counted as accepted cores");
    ck(arkrtos_smp_cpu[1].started == 0u, "a refused core does not record started");
    ck(arkrtos_smp_cpu[2].started == 1u, "an accepted core does record started");
    ck(arkrtos_smp_online_mask_get() == 0x1u, "accepted-but-not-running cores stay offline");

    /* --- 9. port 报一个越界核号：收敛到 0，不许写越界 ------------- *
     * 真硬件上「我在几号核」来自 port（LA64 是 CSR.TID）。读错的后果不是
     * 少记一笔账，而是往 arkrtos_smp_cpu[ARKRTOS_CPU_NUM] 上写——一个
     * 静态数组越界，症状要到很久以后才出现。 */
    stub_reset();
    g_cpu_id = (uint32)ARKRTOS_CPU_NUM + 3u;
    n = arkrtos_smp_init();
    ck(n == 4, "an out-of-range self id does not change the accepted count");
    ck(arkrtos_smp_cpu[0].started == 1u, "an out-of-range self id converges to CPU0");
    ck(arkrtos_smp_cpu[0].online == 1u, "the fallback core marks itself online");
    ck(arkrtos_smp_online_mask_get() == 0x1u, "the mask keeps only the fallback core");

    /* --- 10. 主核 IPI 回执（R1） ------------------------------- *
     * 这一段钉的是「主核那条接收路径通不通」的回执面：ping_main 只对在线的
     * 次核登记并发中断，handler 只在被登记的次核上回一个给 CPU0。
     * 真实投递仍只有多核载体能证明（宿主上没有第二个核）。 */
    stub_reset();
    arkrtos_smp_init();
    ck(arkrtos_smp_ping_main(0u) == -1, "ping_main refuses CPU0 itself");
    ck(arkrtos_smp_ping_main(9u) == -1, "ping_main refuses an out-of-range core");
    ck(arkrtos_smp_ping_main(4u) == -1, "ping_main refuses the first out-of-range id");

    g_ipi_calls = 0;
    ck(arkrtos_smp_ping_main(2u) == -1, "ping_main refuses an offline secondary core");
    ck(g_ipi_calls == 0, "a refused ping never reaches the port layer");
    ck(arkrtos_smp_cpu[2].ipi_back_req == 0u,
       "a refused ping leaves no dangling ping request behind");

    arkrtos_smp_cpu_online(2u);
    g_ipi_calls = 0;
    g_ipi_last  = 0u;
    ck(arkrtos_smp_ping_main(2u) == 0, "ping_main accepts an online secondary core");
    ck(arkrtos_smp_cpu[2].ipi_back_req == 1u, "ping_main registers the back-ping request");
    ck(g_ipi_calls == 1 && g_ipi_last == 2u,
       "ping_main sends exactly one IPI, to the target core");

    /* 次核侧：被登记的那一下要回一个给 CPU0，并且只回一次。 */
    g_cpu_id = 2u;
    arkrtos_smp_cpu[2].ipi_count = 0u;
    arkrtos_smp_cpu[2].ipi_back  = 0u;
    g_ipi_calls = 0;
    g_ipi_last  = 0xffffffffu;
    arkrtos_smp_ipi_handler();
    ck(arkrtos_smp_cpu[2].ipi_count == 1u, "the handler still counts on the current core");
    ck(arkrtos_smp_cpu[2].ipi_back == 1u, "a registered core pings CPU0 back once");
    ck(arkrtos_smp_cpu[2].ipi_back_req == 0u, "the back-ping request is consumed, not periodic");
    ck(g_ipi_calls == 1 && g_ipi_last == 0u, "the back-ping is addressed to CPU0");

    g_ipi_calls = 0;
    arkrtos_smp_ipi_handler();
    ck(arkrtos_smp_cpu[2].ipi_back == 1u, "a second IPI without a request sends nothing back");
    ck(g_ipi_calls == 0, "no unsolicited back-ping reaches the port layer");

    g_cpu_id = 0u;
    arkrtos_smp_cpu[0].ipi_back_req = 1u;
    arkrtos_smp_cpu[0].ipi_back     = 0u;
    g_ipi_calls = 0;
    arkrtos_smp_ipi_handler();
    ck(arkrtos_smp_cpu[0].ipi_back == 0u, "CPU0 never pings itself back");
    ck(arkrtos_smp_cpu[0].ipi_back_req == 1u,
       "CPU0 keeps the request untouched (it is only meaningful on a secondary)");
    ck(g_ipi_calls == 0, "no CPU0 self-ping loop");
    arkrtos_smp_cpu[0].ipi_back_req = 0u;

    drain();
    return 0;
}
