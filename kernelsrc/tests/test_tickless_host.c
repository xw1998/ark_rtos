/**
* @file    test_tickless_host.c
* @brief   tickless idle 的宿主门禁：相位结算与"补发多少拍"必须逐位可复算
* @details 这里把 SysTick 与内核的两处查询（最近延时 / 最近定时器）换成
*          可控的假实现，然后让真实的 kernelsrc/src/arkrtos_tickless.c 跑
*          完整的一睡一醒循环。查的是三件事：
*
*          1) plan / split 的纯计算边界（0 事件、上限、0 计数、空指针）；
*          2) 睡满一条长节拍时，补发的拍数 == 睡掉的拍数，且硬件回到整拍；
*          3) 被提前叫醒时，余数不许丢：连续几十次"非整拍"唤醒之后，
*             累计补发的拍数仍等于"总走过的计数 / 每拍计数"——这就是
*             "跑一天不会慢"这句话的机器可验形式。
*
*          这里**不**验证真硬件：COUNTFLAG/VAL 的真实读序、SysTick 中断
*          优先级、WFI 的真实功耗收益都要上板。宿主全绿不等于板端已验证。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_tickless.h"
#include "arkrtos_task.h"
#include "arkrtos_timer.h"

/* ------------------------------------------------------------------
 * 假硬件 + 假内核查询
 * ------------------------------------------------------------------ */
static uint32 f_clk;            /* 系统主频              */
static uint32 f_cpt;            /* 一个正常节拍的计数    */
static uint32 f_load;           /* 编程下去的周期计数    */
static uint32 f_val;            /* 当前计数值            */
static uint32 f_flag;           /* COUNTFLAG 锁存        */
static uint32 f_delay_next;     /* arkrtos_task_next_delay_ticks() 的返回值 */
static uint32 f_timer_next;     /* arkrtos_timer_next_ticks() 的返回值      */
static uint32 f_programmed;     /* 最近一次编程下去的计数（门禁用）         */
static uint32 f_delivered;      /* 实际补发出的整拍数                       */
static uint32 f_early;          /* 0 = 睡满；>0 = 提前醒了这么多计数        */

void arkrtos_port_start_timer_counts(uint32 counts)
{
    if(counts == 0u)
    {
        counts = 1u;
    }
    f_load = counts;
    f_val  = counts - 1u;
    f_flag = 0u;
    f_programmed = counts;
}

uint32 arkrtos_port_timer_countflag_take(void)
{
    uint32 v = f_flag;

    f_flag = 0u;
    return v;
}

uint32 arkrtos_port_get_timer_counter(void)
{
    return f_val;
}

uint32 arkrtos_port_get_system_clock(void)
{
    return f_clk;
}

void arkrtos_port_wfi(void)
{
    if(f_early == 0u)
    {
        /* 睡满：计数器回卷、COUNTFLAG 置位，SysTick 中断抢在 WFI 退出前跑。 */
        f_val  = f_load - 1u;
        f_flag = 1u;
        arkrtos_kernel_tick_handler();
    }
    else
    {
        /* 被别的中断提前叫醒：计数器还停在第一段里。 */
        f_val  = f_load - f_early;
        f_flag = 0u;
    }
}

uint32 arkrtos_task_next_delay_ticks(void)
{
    return f_delay_next;
}

uint32 arkrtos_timer_next_ticks(void)
{
    return f_timer_next;
}

/* 节拍体：tickless.c 通过 arkrtos_kernel_tick_ticks 走到这里。 */
static void arkrtos_kernel_tick_body(void)
{
    f_delivered++;
}

/* 与 arkrtos_task.c 里的包装同构：先结账，再补发 n 拍（n=0 表示普通一拍）。 */
void arkrtos_kernel_tick_handler(void)
{
    uint32 n = arkrtos_tickless_wake_ticks();

    arkrtos_kernel_tick_ticks((n == 0u) ? 1u : n);
}

/* 供测试调用：把"补发 n 拍"接到上面的计数上（真实实现里这是内核函数）。 */
void arkrtos_kernel_tick_ticks(uint32 n)
{
    uint32 i;

    for(i = 0u; i < n; i++)
    {
        arkrtos_kernel_tick_body();
    }
}

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks;
static int g_fails;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        g_checks++;                                                          \
        if(!(cond))                                                          \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg);              \
        }                                                                    \
    } while(0)

#define CHECK_EQ(a, b, msg)                                                  \
    do {                                                                     \
        unsigned long va = (unsigned long)(a);                               \
        unsigned long vb = (unsigned long)(b);                               \
        g_checks++;                                                          \
        if(va != vb)                                                         \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s (got %lu, want %lu)\n",                    \
                   __FILE__, __LINE__, msg, va, vb);                         \
        }                                                                    \
    } while(0)

static void reset_sim(uint32 clk, uint32 cpt)
{
    f_clk = clk;
    f_cpt = cpt;
    f_load = 0u;
    f_val = 0u;
    f_flag = 0u;
    f_delay_next = ARKRTOS_TICKLESS_NO_EVENT;
    f_timer_next = ARKRTOS_TICKLESS_NO_EVENT;
    f_programmed = 0u;
    f_delivered = 0u;
    f_early = 0u;
}

/* ------------------------------------------------------------------
 * 1) plan / split
 * ------------------------------------------------------------------ */
static void test_plan(void)
{
    CHECK_EQ(arkrtos_tickless_plan(ARKRTOS_TICKLESS_NO_EVENT,
                                   ARKRTOS_TICKLESS_NO_EVENT, 8u), 8u,
             "no event -> cap");
    CHECK_EQ(arkrtos_tickless_plan(5u, ARKRTOS_TICKLESS_NO_EVENT, 8u), 5u,
             "delay decides");
    CHECK_EQ(arkrtos_tickless_plan(ARKRTOS_TICKLESS_NO_EVENT, 3u, 8u), 3u,
             "timer decides");
    CHECK_EQ(arkrtos_tickless_plan(20u, 30u, 8u), 8u,
             "cap clamps");
    CHECK_EQ(arkrtos_tickless_plan(5u, 3u, 8u), 3u,
             "min wins");
    CHECK_EQ(arkrtos_tickless_plan(0u, 7u, 8u), 1u,
             "zero delay -> 1 (never 0)");
    CHECK_EQ(arkrtos_tickless_plan(5u, 3u, 1u), 2u,
             "cap below 2 is raised to 2");
}

static void test_split(void)
{
    uint32 t;
    uint32 r;

    arkrtos_tickless_split(0u, 1000u, &t, &r);
    CHECK_EQ(t, 0u, "split 0 counts -> 0 ticks");
    CHECK_EQ(r, 0u, "split 0 counts -> 0 remain");

    arkrtos_tickless_split(999u, 1000u, &t, &r);
    CHECK_EQ(t, 0u, "sub-tick -> 0 ticks");
    CHECK_EQ(r, 999u, "sub-tick remain kept");

    arkrtos_tickless_split(1000u, 1000u, &t, &r);
    CHECK_EQ(t, 1u, "exact tick");
    CHECK_EQ(r, 0u, "exact tick no remain");

    arkrtos_tickless_split(2500u, 1000u, &t, &r);
    CHECK_EQ(t, 2u, "two ticks + half");
    CHECK_EQ(r, 500u, "half remainder");

    arkrtos_tickless_split(123u, 0u, &t, &r);
    CHECK_EQ(t, 0u, "zero cpt -> 0 ticks");
    CHECK_EQ(r, 0u, "zero cpt -> 0 remain");

    /* 空指针：不能崩，也不能写坏别的变量 */
    t = 7u;
    arkrtos_tickless_split(500u, 100u, NULL, &r);
    CHECK_EQ(t, 7u, "NULL ticks -> no write");
    arkrtos_tickless_split(500u, 100u, &t, NULL);
    CHECK_EQ(t, 7u, "NULL remain -> no write");
}

/* ------------------------------------------------------------------
 * 2) 睡满一条长节拍
 * ------------------------------------------------------------------ */
static void test_full_sleep(void)
{
    uint32 got;

    reset_sim(84000000u, 42000u);       /* 84 MHz, 500 us/tick */
    f_delay_next = 5u;                  /* 最近的延时事件在 5 拍后 */

    got = arkrtos_tickless_idle();

    CHECK_EQ(got, 5u, "full sleep reports 5 ticks");
    CHECK_EQ(f_delivered, 5u, "5 tick bodies delivered by the ISR");
    /* 睡满之后硬件已经被摆回整拍，f_programmed 记的是这一次（最后一次）编程 */
    CHECK_EQ(f_load, 42000u, "hardware left at a full tick");

    /* 下一次：没有事件时按上限睡 */
    f_delivered = 0u;
    f_delay_next = ARKRTOS_TICKLESS_NO_EVENT;
    got = arkrtos_tickless_idle();
    CHECK_EQ(got, (uint32)ARKRTOS_TICKLESS_MAX_TICKS, "no event -> cap ticks");
    CHECK_EQ(f_delivered, (uint32)ARKRTOS_TICKLESS_MAX_TICKS, "cap ticks delivered");
    CHECK_EQ(f_load, 42000u, "back to full tick after capped sleep");

    /* 只差一拍就不该跳：直接 WFI */
    f_delivered = 0u;
    f_delay_next = 1u;
    got = arkrtos_tickless_idle();
    CHECK_EQ(got, 0u, "one tick away -> plain WFI");
    CHECK_EQ(f_delivered, 1u, "plain WFI waits for the normal tick");
}

/* ------------------------------------------------------------------
 * 3) 提前唤醒：余数不许丢
 * ------------------------------------------------------------------ */
static void test_early_wake(void)
{
    uint32 got;
    uint32 i;
    uint32 total_elapsed = 0u;
    uint32 total_ticks = 0u;
    const uint32 cpt = 1000u;

    /* 让"一个节拍"正好是 1000 个计数：2 MHz 主频 + 500 us 节拍 */
    reset_sim(2000000u, cpt);

    /* 第一次：计划 6 拍，只走了 1500 个计数（= 1 拍半）就被叫醒 */
    f_delay_next = 6u;
    f_timer_next = ARKRTOS_TICKLESS_NO_EVENT;
    f_early = 1500u;
    got = arkrtos_tickless_idle();
    CHECK_EQ(got, 1u, "1500/1000 -> 1 tick");
    CHECK_EQ(f_delivered, 1u, "exactly one body delivered");
    CHECK_EQ(f_programmed, 500u, "phase 500 -> shorten next tick to 500");

    /* 接着：短周期到点，这一拍自身要补一次，并恢复整拍 */
    f_delivered = 0u;
    f_early = 0u;
    arkrtos_kernel_tick_handler();      /* 模拟短周期到点的中断 */
    CHECK_EQ(f_delivered, 1u, "short period ends -> one tick");
    CHECK_EQ(f_load, cpt, "short period restored to a full tick");

    /* 长跑：每次都只走非整拍，累计补发必须等于 总计数/每拍 */
    reset_sim(2000000u, cpt);
    for(i = 0u; i < 40u; i++)
    {
        static const uint32 steps[] = { 1500u, 370u, 999u, 2201u, 41u, 1000u, 4999u };
        uint32 step = steps[i % (sizeof(steps) / sizeof(steps[0]))];

        f_delivered = 0u;
        f_delay_next = 50u;             /* 事件很远，别让它限制睡眠长度 */
        f_early = step;
        got = arkrtos_tickless_idle();

        total_elapsed += step;
        total_ticks += got;                     /* got 就是本次补发的整拍数 */

        CHECK_EQ(got, f_delivered, "reported ticks == delivered bodies");
        CHECK(f_programmed <= cpt, "never programmed a longer-than-tick period");
        CHECK((total_ticks == total_elapsed / cpt),
              "cumulative ticks == elapsed/cpt (no drift)");
        if(total_ticks != total_elapsed / cpt)
        {
            printf("  drift at step %u: ticks=%u elapsed=%u\n",
                   (unsigned)i, (unsigned)total_ticks, (unsigned)total_elapsed);
            break;
        }
    }
}

int main(void)
{
    test_plan();
    test_split();
    test_full_sleep();
    test_early_wake();

    if(g_fails != 0)
    {
        printf("TICKLESS-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("TICKLESS-HOST OK %d checks\n", g_checks);
    return 0;
}
