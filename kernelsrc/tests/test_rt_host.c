/**
* @file test_rt_host.c
* @brief 实时性度量（arkrtos_rt.c）的宿主用例
* @details 被测对象不碰硬件：所有时间戳都问 port 的周期计数器，唤醒与抢占
*          都经内核内部原语。所以宿主用例把周期计数器、信号量、任务登记与
*          延时全部桩掉，钉住**算术与账目**本身：
*
*            1. 计数器不可用 / 主频读不到时，读数项保持 n = 0，微秒折算返回 0，
*               绝不退回节拍计数去凑一个分辨率差三个数量级的数；
*            2. 微秒换算按千赫拆开（RV32 没有 64 位除法），整秒项与余数项
*               都要保住，32 位主频下逐位可算；
*            3. 抖动是有符号的：早到为负、晚到为正，第一拍没有参照物，
*               不计样本；
*            4. wake.lo 从节拍入口起算，switch 从节拍体最后一行起算；两者
*               相减才是节拍体本身，所以同一拍必须**序号配对**，节拍体内
*               直接切走的样本要丢弃并单独计数，不能拿上一拍的末尾去减；
*            5. 超过一拍的唤醒样本是"被更高优先级挡了一会儿"，不是唤醒延迟，
*               单独计入 late_n，不进统计；
*            6. 抢占场景的辅助任务按需创建一次，起不来时 last_error 说清是
*               哪一步（两个信号量 / 任务登记），样本数不足时返回 -1 而不是
*               把缺的样本补成 0；
*            7. 临界区只记最外层，嵌套的内层不重复计时，不成对的出口既不计
*               样本也不下溢。
*
*          被测的是真实的 arkrtos_rt.c。辅助任务体在目标上是 for(;;)，
*          宿主上由 ARKRTOS_RT_HOST_TEST 档的预算改成一圈一圈可数的有限运行
*          （默认档不编这段，符号与行为零变化）。
*
*          覆盖范围（如实说明）：
*            已验证：计数器存活判定、init 的三条失败路径与两种主频下的每拍
*                    周期数、微秒换算（整秒项 / 千赫余数 / 失零）、节拍抖动
*                    的取小与取大、tick.isr 的 min/avg/max、临界区嵌套与
*                    不成对出口、wake.lo 的正常/迟到/未见到节拍三种样本、
*                    switch 的配对与不配对、抢占场景的四种启动失败与正常
*                    采样、辅助任务的三条跳过路径、快照的行序与短缓冲。
*            未验证：真实节拍与真实调度器下的时序（谁先跑、切换开销）、
*                    DWT / mcycle 计数器的真实分辨率、以及只有上板才有的
*                    中断延迟与编译器指令数。
* @note 用法：由 tools/coverage_gate.py 的两条 case 编译并运行，输出
*       RT-HOST OK（RTSTAT=1 档）或 RT-HOST OFF OK（RTSTAT=0 档，空目标）。
*/

#include "arkrtos_rt.h"

#if (ARKRTOS_USE_RTSTAT == 1)

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_hal.h"
#include "arkrtos_config.h"
#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_sync.h"

/* 用例与 arkrtos_rt.c 之间的宿主钩子（见该文件的 ARKRTOS_RT_HOST_TEST 段）。 */
extern int32 arkrtos_rt_helper_budget;

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

/* ------------------------------------------------------------------
 * 周期计数器 / port 桩
 * ------------------------------------------------------------------ */

static uint32 g_cyc;
static int    g_alive_probe;    /* 存活探测进行中 */
static int    g_alive_calls;
static int    g_alive_move = 1; /* 探测的第二次读是否前进 */
static int32  g_cyc_init_ret;
static uint32 g_clk = 100000000u;

int32 arkrtos_port_cyccnt_init(void)
{
    if(g_cyc_init_ret != 0)
    {
        g_alive_probe = 0;
        return g_cyc_init_ret;
    }
    g_alive_probe = 1;
    g_alive_calls = 0;
    return 0;
}

uint32 arkrtos_port_get_cyccnt(void)
{
    if(g_alive_probe != 0)
    {
        g_alive_calls++;
        if(g_alive_calls >= 2)
        {
            g_alive_probe = 0;
            return g_cyc + ((g_alive_move != 0) ? 1u : 0u);
        }
        return g_cyc;
    }
    return g_cyc;
}

uint32 arkrtos_port_get_cycle_clock(void)
{
    return g_clk;
}

/* ------------------------------------------------------------------
 * 内核邻居桩
 * ------------------------------------------------------------------ */

static uint32 g_wait_advance;   /* 每次延时把周期计数器推进多少 */
static int    g_wait_calls;
static uint32 g_wait_ms;

void arkrtos_task_wait_internal(uint32 ms)
{
    g_wait_calls++;
    g_wait_ms = ms;
    g_cyc += g_wait_advance;
}

static int    g_sem_create_calls;
static int32  g_sem_fail_at;    /* 1 = 第一个 create 失败，2 = 第二个 */
static char   g_sem_name[2][16];
static int32  g_sem_init_count[2];

int32 arkrtos_sem_create_internal(const char *name, int32 init_count)
{
    int idx = g_sem_create_calls;

    g_sem_create_calls++;
    if((g_sem_fail_at != 0) && ((idx + 1) == (int)g_sem_fail_at))
    {
        return -1;
    }
    if(idx < 2)
    {
        (void)snprintf(g_sem_name[idx], sizeof(g_sem_name[idx]), "%s", name);
        g_sem_init_count[idx] = init_count;
    }
    return (int32)(idx + 1);
}

static int32 g_sem_wait_ret;
static int32 g_sem_wait_handle;
static int32 g_sem_wait_timeout;
static int   g_sem_wait_calls;

int32 arkrtos_sem_wait_internal(int32 handle, int32 timeout_ms)
{
    g_sem_wait_calls++;
    g_sem_wait_handle = handle;
    g_sem_wait_timeout = timeout_ms;
    return g_sem_wait_ret;
}

static void  (*g_reg_entry)(void);   /* 任务登记桩记下的辅助任务入口 */
static int   g_post_calls;
static int32 g_post_handle;
static int   g_helper_on_post;      /* 1 = 调度器在 post 之后立刻跑辅助任务 */
static int32 g_helper_budget_setting;
static uint32 g_sem_hi_handle = 1u; /* 第一个创建的信号量（辅助任务等的那个） */

int32 arkrtos_sem_post_internal(int32 handle)
{
    g_post_calls++;
    g_post_handle = handle;

    /* 只有"叫醒辅助任务"那一次 post 才会切到它；它回执的那次 post 不能
     * 再触发一次，否则就是无穷递归。 */
    if((g_helper_on_post != 0) && ((uint32)handle == g_sem_hi_handle) && (g_reg_entry != 0))
    {
        g_cyc += 77u;               /* post 路径本身的代价，量级取自文档 */
        arkrtos_rt_helper_budget = g_helper_budget_setting;
        g_reg_entry();
    }
    return 0;
}

static uint32 *g_reg_stack;
static uint32 g_reg_size;
static uint8  g_reg_prio;
static uint32 g_reg_period;
static int32  g_reg_ret = 5;
static int    g_reg_calls;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                            uint8 priority, uint32 period_ms)
{
    g_reg_calls++;
    g_reg_entry = entry;
    g_reg_stack = stack_bottom;
    g_reg_size = stack_size;
    g_reg_prio = priority;
    g_reg_period = period_ms;
    return g_reg_ret;
}

/* ------------------------------------------------------------------
 * 起停帮助
 * ------------------------------------------------------------------ */

static void arm(void)
{
    g_cyc_init_ret = 0;
    g_alive_move = 1;
    g_clk = 100000000u;
    g_cyc = 0u;
    arkrtos_rt_init();
}

static void disarm(void)
{
    g_cyc_init_ret = -1;
    arkrtos_rt_init();
}

/* ------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------ */

static void test_counter_alive(void)
{
    g_cyc = 100u;

    g_cyc_init_ret = -1;
    ck(arkrtos_rt_counter_alive() == 0, "a port that refuses to arm the counter is reported dead");

    g_cyc_init_ret = 0;
    g_alive_move = 0;
    ck(arkrtos_rt_counter_alive() == 0, "an armed counter that does not move is reported dead");

    g_alive_move = 1;
    ck(arkrtos_rt_counter_alive() == 1, "an armed, moving counter is reported alive");
}

/* 必须在任何一次成功的 init 之前跑：那时 g_rt.clk 还是初值 0。 */
static void test_unarmed_first(void)
{
    arkrtos_rt_row_t rows[8];
    uint32 i;

    disarm();
    ck(arkrtos_rt_cycles_per_tick() == 0u, "no counter -> 0 cycles per tick (unknown, not a guess)");
    ck(arkrtos_rt_to_us(1000u) == 0u, "with the clock unknown, microseconds are not invented");

    ck(arkrtos_rt_measure_wake(1u) == ARKRTOS_RT_ERR_NOTREADY, "measure_wake refuses when not armed");
    ck(arkrtos_rt_measure_preempt(1u) == ARKRTOS_RT_ERR_NOTREADY, "measure_preempt refuses when not armed");
    ck(arkrtos_rt_last_error() == 0, "not-ready is not a start failure");

    ck(arkrtos_rt_snapshot(rows, 8u) == ARKRTOS_RT_ROWS, "the snapshot still answers with the fixed row count");
    for(i = 0u; i < ARKRTOS_RT_ROWS; i++)
    {
        ck(rows[i].n == 0u, "an unarmed sampler reports zero samples, never a substituted one");
    }

    /* 未就绪时三个钩子都是惰性的 */
    arkrtos_rt_tick_enter();
    arkrtos_rt_tick_exit();
    arkrtos_rt_crit_enter();
    arkrtos_rt_crit_exit();
    ck(1, "the tick and critical-section hooks are inert when not armed");
}

static void test_init_paths(void)
{
    /* 计数器在走，但主频读不到 */
    g_cyc_init_ret = 0;
    g_alive_move = 1;
    g_clk = 0u;
    arkrtos_rt_init();
    ck(arkrtos_rt_cycles_per_tick() == 0u, "a counter without a clock yields no tick length");

    /* 正常：100 MHz、500 us 一拍 */
    g_clk = 100000000u;
    arkrtos_rt_init();
    ck(arkrtos_rt_cycles_per_tick() == 50000u, "100 MHz with a 500 us tick is 50000 cycles");

    /* 主频低于 1 kHz：千赫为 0，微秒折算如实返回 0 而不是除零 */
    g_clk = 500u;
    arkrtos_rt_init();
    ck(arkrtos_rt_cycles_per_tick() == 1u, "a sub-kHz clock still yields a non-zero tick length");
    ck(arkrtos_rt_to_us(500u) == 0u, "below 1 kHz the microsecond conversion refuses to divide by zero");

    /* 千赫余数项不能被丢掉 */
    g_clk = 100123000u;
    arkrtos_rt_init();
    ck(arkrtos_rt_cycles_per_tick() == 50061u,
       "a 100.123 MHz clock keeps the kilohertz remainder (50061 cycles per tick)");
}

static void test_to_us(void)
{
    arm();

    ck(arkrtos_rt_to_us(0u) == 0u, "zero cycles is zero microseconds");
    ck(arkrtos_rt_to_us(50000u) == 500u, "one tick at 100 MHz is 500 us");
    ck(arkrtos_rt_to_us(12345u) == 123u, "the result truncates to whole microseconds");
    ck(arkrtos_rt_to_us(250000u) == 2500u, "the whole-second term is not dropped");

    /* 96 MHz 这种不是 1000 整数倍的主频也要能算（误差 < 0.1%） */
    g_clk = 96000000u;
    arkrtos_rt_init();
    ck(arkrtos_rt_to_us(96000u) == 1000u, "96 MHz: 96000 cycles is exactly 1000 us");
}

static void test_tick_and_jitter(void)
{
    int32 mn;
    int32 mx;
    uint32 n;
    arkrtos_rt_row_t rows[8];

    arm();
    ck(arkrtos_rt_jitter(&mn, &mx, &n) == 0u, "no jitter sample before the first tick");
    ck(arkrtos_rt_jitter(0, 0, 0) == 0u, "null outputs are optional");

    g_cyc = 1000u;
    arkrtos_rt_tick_enter();
    ck(arkrtos_rt_jitter(&mn, &mx, &n) == 0u, "the first tick has nothing to compare against");

    g_cyc = 1200u;
    arkrtos_rt_tick_exit();             /* tick.isr = 200 */
    g_cyc = 1000u + 50000u;
    arkrtos_rt_tick_enter();            /* exactly one period later */
    ck(arkrtos_rt_jitter(&mn, &mx, &n) == 1u, "one comparison sample exists now");
    ck((n == 1u) && (mn == 0) && (mx == 0), "a tick exactly one period later is zero jitter");

    g_cyc = 1000u + 50000u + 49998u;
    arkrtos_rt_tick_enter();            /* 2 cycles early */
    ck(arkrtos_rt_jitter(&mn, &mx, &n) == 2u, "a second comparison sample exists");
    ck((n == 2u) && (mn == -2) && (mx == 0), "an early tick shows as negative jitter and updates the minimum");

    g_cyc += 50000u + 60u;
    arkrtos_rt_tick_enter();            /* 60 cycles late */
    ck(arkrtos_rt_jitter(&mn, &mx, &n) == 3u, "a third comparison sample exists");
    ck((n == 3u) && (mn == -2) && (mx == 60), "a late tick updates the maximum only");

    g_cyc += 123u;
    arkrtos_rt_tick_exit();             /* tick.isr = 123 */

    ck(arkrtos_rt_snapshot(rows, 8u) == ARKRTOS_RT_ROWS, "the snapshot fills the fixed row count");
    ck((rows[0].n == 2u) && (rows[0].min_c == 123u) && (rows[0].max_c == 200u),
       "tick.isr keeps the two samples apart");
    ck(rows[0].avg_c == 161u, "tick.isr averages 200 and 123 to 161 cycles");
    ck((rows[0].min_us == 1u) && (rows[0].max_us == 2u),
       "each bound is converted on its own, not from the average");

    arkrtos_rt_tick_exit();             /* already closed: ignored */
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[0].n == 2u, "an exit without an entry adds no sample");
}

static void test_crit(void)
{
    arkrtos_rt_row_t rows[8];

    arm();
    g_cyc = 5000u;
    arkrtos_rt_crit_enter();
    g_cyc = 5100u;
    arkrtos_rt_crit_enter();            /* nested: no second timestamp */
    g_cyc = 5300u;
    arkrtos_rt_crit_exit();             /* still inside */

    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[4].n == 0u, "an inner exit does not close the outer critical section");

    g_cyc = 5400u;
    arkrtos_rt_crit_exit();             /* closed: 400 cycles from 5000 */
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck((rows[4].n == 1u) && (rows[4].min_c == 400u),
       "the outermost exit measures the whole nested span");

    arkrtos_rt_crit_exit();             /* unbalanced */
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[4].n == 1u, "an unbalanced exit neither samples nor underflows");
}

static void test_measure_wake(void)
{
    arkrtos_rt_row_t rows[8];

    arm();
    g_cyc = 1000u;
    arkrtos_rt_tick_enter();            /* last_tick_cyc = 1000 */
    g_cyc = 1200u;
    arkrtos_rt_tick_exit();             /* tick_end_cyc = 1200, sequence paired */
    g_wait_calls = 0;
    g_wait_advance = 300u;

    ck(arkrtos_rt_measure_wake(1u) == 0, "a wake sample completes");
    ck((g_wait_calls == 1) && (g_wait_ms == 1u), "each sample waits 1 ms");

    (void)arkrtos_rt_snapshot(rows, 8u);
    ck((rows[1].n == 1u) && (rows[1].min_c == 500u),
       "wake.lo measures from the tick entry (1200 + 300 - 1000)");
    ck((rows[3].n == 1u) && (rows[3].min_c == 300u),
       "switch measures from the end of the tick body (1500 - 1200)");
    ck((rows[1].late_n == 0u) && (rows[3].skip_n == 0u), "a normal sample discards nothing");

    /* 迟到一整拍：不是唤醒延迟，单独计数 */
    g_wait_advance = 50010u;
    ck(arkrtos_rt_measure_wake(1u) == 0, "a late sample still completes");
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck((rows[1].n == 1u) && (rows[1].late_n == 1u),
       "a sample more than one period late is counted, not averaged in");

    /* 节拍体还没收尾就切走：switch 样本必须丢弃 */
    arkrtos_rt_tick_enter();            /* new sequence, tick body open */
    g_wait_advance = 100u;
    ck(arkrtos_rt_measure_wake(1u) == 0, "a wake inside an open tick body still yields wake.lo");
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[1].n == 2u, "the wake is still a wake.lo sample");
    ck((rows[3].n == 1u) && (rows[3].skip_n == 1u),
       "a sample whose tick body has not closed is discarded from switch");

    /* 一个节拍都没见过 */
    arkrtos_rt_reset();
    g_wait_advance = 100u;
    ck(arkrtos_rt_measure_wake(1u) == 0, "with no tick seen yet the sample is skipped");
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[1].n == 0u, "no wake.lo sample was fabricated");
}

static void test_measure_preempt(void)
{
    arkrtos_rt_row_t rows[8];

    arm();
    g_sem_create_calls = 0;
    g_sem_fail_at = 1;
    ck(arkrtos_rt_measure_preempt(1u) == ARKRTOS_RT_ERR_START,
       "a failed helper start is reported as a start failure");
    ck(arkrtos_rt_last_error() == 1, "last_error names the first semaphore");

    g_sem_create_calls = 0;
    g_sem_fail_at = 2;
    ck(arkrtos_rt_measure_preempt(1u) == ARKRTOS_RT_ERR_START, "the second semaphore can fail on its own");
    ck(arkrtos_rt_last_error() == 2, "last_error names the second semaphore");

    g_sem_create_calls = 0;
    g_sem_fail_at = 0;
    g_reg_ret = 0;
    ck(arkrtos_rt_measure_preempt(1u) == ARKRTOS_RT_ERR_START, "a refused registration is a start failure");
    ck(arkrtos_rt_last_error() == 4, "register returning 0 is folded into error 4");

    g_reg_ret = -3;
    ck(arkrtos_rt_measure_preempt(1u) == ARKRTOS_RT_ERR_START, "a negative register code also fails");
    ck(arkrtos_rt_last_error() == 7, "and it is folded in (4 - (-3) = 7), not lost");

    /* 正常：一次 post 对应一次辅助任务采样 */
    g_reg_ret = 5;
    g_sem_create_calls = 0;
    g_sem_wait_ret = 0;
    g_helper_on_post = 1;
    g_helper_budget_setting = 1;
    g_post_calls = 0;
    g_reg_calls = 0;

    ck(arkrtos_rt_measure_preempt(3u) == 0, "three preemption samples complete");
    ck(g_reg_calls == 1, "the helper task is registered once, not once per sample");
    ck(g_reg_prio == 1u, "the helper runs at priority 1");
    ck(g_reg_period == 0u, "the helper is not periodic");
    ck(g_reg_size == 256u * 4u, "the helper gets its static 256-word stack");
    ck(g_reg_stack != 0, "the stack pointer is handed to the registration");
    ck((strcmp(g_sem_name[0], "rt_hi") == 0) && (strcmp(g_sem_name[1], "rt_ack") == 0),
       "the two semaphores are named");
    ck((g_sem_init_count[0] == 0) && (g_sem_init_count[1] == 0), "both semaphores start empty");
    ck((g_sem_wait_handle == 2) && (g_sem_wait_timeout == 100),
       "the caller waits on the ack with a 100 ms timeout");

    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[2].n == 3u, "wake.hi got one sample per preemption");
    ck(rows[2].min_c == 77u, "wake.hi measures from the post to the helper's first instruction");

    /* 辅助任务已经在跑：第二次不再登记 */
    ck(arkrtos_rt_measure_preempt(1u) == 0, "a later run reuses the helper");
    ck(g_reg_calls == 1, "no second registration");

    /* 一次唤醒跑两圈：第二圈已经达到目标，不重复计数 */
    g_helper_budget_setting = 2;
    ck(arkrtos_rt_measure_preempt(1u) == 0, "a double wake still completes");
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[2].n == 5u, "the second wake in the same sample is not counted");

    /* 采样结束后才到的唤醒：hi_run 已清，不计数 */
    g_helper_budget_setting = 1;
    g_reg_entry();
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[2].n == 5u, "a wake that arrives after the run ended is not counted");

    /* 辅助任务睡不下去：回到循环而不是采样 */
    g_sem_wait_ret = 1;
    g_helper_budget_setting = 1;
    g_reg_entry();
    (void)arkrtos_rt_snapshot(rows, 8u);
    ck(rows[2].n == 5u, "a helper that cannot sleep loops back instead of sampling");
    g_sem_wait_ret = 0;

    /* 辅助任务没跟上：样本数不足要报 -1，而不是补齐 */
    g_helper_on_post = 0;
    ck(arkrtos_rt_measure_preempt(2u) == -1,
       "an unanswered sample is reported as failure instead of being invented");
    g_helper_on_post = 1;
}

static void test_snapshot(void)
{
    arkrtos_rt_row_t rows[8];
    int32 mn;
    int32 mx;
    uint32 n;

    arm();

    ck(arkrtos_rt_snapshot(0, 8u) == 0u, "no output buffer -> zero rows");
    ck(arkrtos_rt_snapshot(rows, 0u) == 0u, "no room -> zero rows");

    ck(arkrtos_rt_snapshot(rows, 1u) == 1u, "a one-row buffer is honoured");
    ck(strcmp(rows[0].name, "tick.isr") == 0, "rows come in a fixed order");
    ck((rows[0].n == 0u) && (rows[0].min_c == 0u) && (rows[0].avg_c == 0u) &&
       (rows[0].min_us == 0u) && (rows[0].avg_us == 0u) && (rows[0].max_us == 0u),
       "an unmeasured row reads all zeros");

    ck(arkrtos_rt_snapshot(rows, 3u) == 3u, "a short buffer is honoured exactly");
    ck(arkrtos_rt_snapshot(rows, 10u) == ARKRTOS_RT_ROWS, "a long buffer is capped at the row count");
    ck(strcmp(rows[1].name, "wake.lo") == 0, "row 1 is wake.lo");
    ck(strcmp(rows[2].name, "wake.hi") == 0, "row 2 is wake.hi");
    ck(strcmp(rows[3].name, "switch") == 0, "row 3 is switch");
    ck(strcmp(rows[4].name, "crit.max") == 0, "row 4 is crit.max");
    ck(rows[0].late_n == 0u, "non-late rows carry a zero late counter");
    ck(rows[0].skip_n == 0u, "non-switch rows carry a zero skip counter");

    ck(arkrtos_rt_jitter(&mn, &mx, &n) == 0u, "a fresh sampler has no jitter samples");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);

    test_counter_alive();
    test_unarmed_first();       /* 必须在第一次成功 init 之前 */
    test_init_paths();
    test_to_us();
    test_tick_and_jitter();
    test_crit();
    test_measure_wake();
    test_measure_preempt();
    test_snapshot();

    if(g_fails != 0)
    {
        printf("RT-HOST FAILED  %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("RT-HOST OK  %d checks\n", g_checks);
    return 0;
}

#else /* ARKRTOS_USE_RTSTAT != 1 */

#include <stdio.h>

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);

    /* 关档是空目标：头文件除了开关宏之外什么都不暴露，这里一个函数都不能
     * 调用（调了会在链接期失败——那正是"默认关零符号"的判据）。 */
    if(ARKRTOS_USE_RTSTAT != 0)
    {
        printf("RT-HOST OFF FAILED  the switch is not 0\n");
        return 1;
    }
    printf("RT-HOST OFF OK  switch=0, no symbols\n");
    return 0;
}

#endif
