/**
* @brief Ark RTOS tickless idle 实现
* @details 设计取舍见 arkrtos_tickless.h。这里只补四件实现上的事：
*
*          1) 相位（phase）必须由软件记住，不能只看硬件。
*             被别的中断提前叫醒时，走过的计数往往不是整拍，余数如果丢掉，
*             每次 idle 都会少算最多一拍——单次看不出来，跑一天就是分钟级
*             的慢。所以这里维护 s_tl_phase = "已经朝下一拍走了多少个计数"，
*             每次结账按 total = phase + elapsed 折算整拍，剩下的继续留着。
*
*          2) 相位不为 0 时，硬件必须只跑"补满这一拍"的短周期，然后由那一次
*             中断自己恢复成整拍（s_tl_short）。如果图省事把 LOAD 直接写成
*             短值不管，之后每一拍都会跟着变短，节拍就永久性地快了。
*
*          3) 补齐节拍发生在中断里（睡满那条路）。中断一退出就可能切到别的
*             任务，那时再补就会把"过去"算到"现在"上。补齐走
*             arkrtos_kernel_tick_ticks()（直接的节拍体），不走
*             arkrtos_kernel_tick_handler()——后者会重新结账，把还没到的
*             短周期当成到了，凭空多补一拍。
*
*          4) 只认单次回卷。上限（默认 8 拍）远小于计数器能表达的跨度，
*             加上 tick 中断本身就是唤醒源，醒来必然在回卷后不久，所以
*             "回卷了两次以上"这种情形在这个设计里到不了。
*/
#include "arkrtos_tickless.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_timer.h"

#if (ARKRTOS_USE_TICKLESS == 1)

/* SysTick 是 24 位递减计数器：超过这个值的编程值会被截断。 */
#define ARKRTOS_TL_COUNT_MAX    0x00FFFFFFu

/* 长节拍状态。只在 idle 与 tick 中断两个上下文之间传递，都在内核态，
 * 所以不需要临界区；用 volatile 是因为编译器看不见中断里的写入。 */
static volatile uint8  s_tl_armed;      /* 1 = 已编程为长节拍，等待结账     */
static volatile uint8  s_tl_short;      /* 1 = 当前周期是"补满一拍"的短周期 */
static volatile uint32 s_tl_counts;     /* 这次编程下去的计数               */
static volatile uint32 s_tl_cpt;        /* 一个正常节拍的计数               */
static volatile uint32 s_tl_phase;      /* 已朝下一拍走过的计数（< cpt）    */

/* 板端可见的累计读数（见 arkrtos_tickless_stats）：只增不清，复位归零。
 * 计数在"真的把硬件编程成短周期并 WFI"那一刻加一，所以它同时能证明
 * 开关生效与省下了中断。 */
static volatile uint32 s_tl_isrs;       /* 进 SysTick 中断的次数             */
static volatile uint32 s_tl_sleeps;     /* 进过长睡眠的次数                 */


/* 一个正常节拍有多少个时钟计数。与 arkrtos_port_start_timer() 同一算式，
 * 但那一个入口只收微秒，tickless 要的是计数本身（见 arkrtos_hal.h）。 */
static uint32 arkrtos_tl_counts_per_tick(void)
{
    uint32 clk = arkrtos_port_get_system_clock();
    uint32 cpt = (uint32)(((unsigned long long)clk * ARKRTOS_TICK_PERIOD_US) / 1000000u);

    return (cpt == 0u) ? 1u : cpt;
}

uint32 arkrtos_tickless_plan(uint32 delay_ticks, uint32 timer_ticks, uint32 cap)
{
    uint32 next = (delay_ticks < timer_ticks) ? delay_ticks : timer_ticks;

    if(next == 0u)
    {
        next = 1u;
    }
    if(cap < 2u)
    {
        cap = 2u;                       /* 跳一拍没有意义：一样要进一次中断 */
    }
    if(next > cap)
    {
        next = cap;
    }
    return next;
}

void arkrtos_tickless_stats(uint32 *isrs, uint32 *sleeps)
{
    if(isrs != 0)
    {
        *isrs = s_tl_isrs;
    }
    if(sleeps != 0)
    {
        *sleeps = s_tl_sleeps;
    }
}

void arkrtos_tickless_split(uint32 elapsed_counts, uint32 counts_per_tick,
                            uint32 *ticks, uint32 *remain_counts)
{
    uint32 n;

    if((ticks == 0) || (remain_counts == 0))
    {
        return;                         /* 调用点写错：不猜，什么都不写 */
    }
    if(counts_per_tick == 0u)
    {
        *ticks = 0u;
        *remain_counts = 0u;
        return;
    }
    n = elapsed_counts / counts_per_tick;
    *ticks = n;
    *remain_counts = elapsed_counts - (n * counts_per_tick);
}

/* 把硬件编程成"再过 counts 个计数进一拍"。is_short 标记这个周期是不是
 * 为了对齐相位而缩短的：是的话，那一拍结束后必须恢复整拍，否则节拍会
 * 永久性地偏快。 */
static void arkrtos_tl_program(uint32 counts, uint8 is_short)
{
    if(counts == 0u)
    {
        counts = 1u;
    }
    if(counts > ARKRTOS_TL_COUNT_MAX)
    {
        counts = ARKRTOS_TL_COUNT_MAX;
    }
    s_tl_short = is_short;
    arkrtos_port_start_timer_counts(counts);
}

/* 结账之后把硬件摆到"下一拍还有 remain 个计数"的位置。 */
static void arkrtos_tl_rearm_normal(uint32 cpt, uint32 remain)
{
    if(remain == 0u)
    {
        arkrtos_tl_program(cpt, 0u);
    }
    else
    {
        arkrtos_tl_program((cpt > remain) ? (cpt - remain) : 1u, 1u);
    }
}

/**
* @brief 结算这一次中断：算出走过几个整拍，并把硬件摆回正确相位
* @return 整拍数（可以为 0：这次中断本身就是那一拍，由调用方补 1 次节拍体）
*/
static uint32 arkrtos_tl_settle(void)
{
    uint32 flag;
    uint32 val;
    uint32 counts;
    uint32 elapsed;
    uint32 cpt;
    uint32 total;
    uint32 ticks;

    if(s_tl_armed == 0u)
    {
        if(s_tl_short == 0u)
        {
            return 0u;                  /* 普通一整拍，没有账要结 */
        }
        /* 补相位的那一拍到了：它本身就代表了这一拍，恢复整拍周期。 */
        s_tl_short = 0u;
        s_tl_phase = 0u;
        arkrtos_port_start_timer_counts((s_tl_cpt != 0u) ? s_tl_cpt
                                                         : arkrtos_tl_counts_per_tick());
        return 0u;
    }

    /* 读序固定：读 CTRL 会清 COUNTFLAG，所以先取标志再取计数值。 */
    flag   = arkrtos_port_timer_countflag_take();
    val    = arkrtos_port_get_timer_counter() & ARKRTOS_TL_COUNT_MAX;
    counts = s_tl_counts;
    s_tl_armed = 0u;

    if(flag != 0u)
    {
        /* 已经回卷：整段 + 回卷后走过的部分（LOAD = counts - 1）。 */
        uint32 since_wrap = (counts > 0u) ? (counts - 1u - val) : 0u;

        elapsed = counts + since_wrap;
    }
    else
    {
        /* 提前唤醒：计数器还在第一段里，剩下的就是还没走的部分。 */
        elapsed = (counts > val) ? (counts - val) : 1u;
    }

    cpt   = s_tl_cpt;
    total = s_tl_phase + elapsed;
    ticks = total / cpt;
    s_tl_phase = total - (ticks * cpt);
    arkrtos_tl_rearm_normal(cpt, s_tl_phase);
    return ticks;
}

uint32 arkrtos_tickless_wake_ticks(void)
{
    uint32 n = arkrtos_tl_settle();

    /* 一个 SysTick 中断算一次，不管它结账出几拍（stats 的 isrs 列）。
     * 只在这一处加：这是中断路径上唯一的入口。同一处把结账出的拍数
     * 累加进 comp：`节拍总数 == isrs + comp` 因此恒成立。 */
    s_tl_isrs++;
    return n;
}

uint32 arkrtos_tickless_idle(void)
{
    uint32 delay_ticks;
    uint32 timer_ticks;
    uint32 n;
    uint32 cpt;
    uint32 counts;
    uint32 i;

    if(s_tl_armed != 0u)
    {
        /* 上一段还没结账就又被叫来睡：不重入，按普通 WFI 处理。 */
        ARKRTOS_WFI();
        return 0u;
    }

    delay_ticks = arkrtos_task_next_delay_ticks();
    timer_ticks = arkrtos_timer_next_ticks();
    n = arkrtos_tickless_plan(delay_ticks, timer_ticks,
                              (uint32)ARKRTOS_TICKLESS_MAX_TICKS);

    cpt = arkrtos_tl_counts_per_tick();

    /* 已经走掉的那点相位要从这一段里扣掉：睡 n 拍 = 再走 (n*cpt - phase) 个计数。 */
    counts = n * cpt;
    if(counts > s_tl_phase)
    {
        counts -= s_tl_phase;
    }
    else
    {
        counts = 1u;
    }

    /* 24 位计数器装不下就不睡那么久：截到装得下的上限。
     * 截短只会让我们早醒（早醒按提前唤醒结账），不会让节拍变快。 */
    if(counts > ARKRTOS_TL_COUNT_MAX)
    {
        counts = ARKRTOS_TL_COUNT_MAX;
    }

    /* 省不下一次中断就别跳：不如老老实实 WFI 到下一拍。 */
    if((counts <= cpt) || (counts < 2u))
    {
        ARKRTOS_WFI();
        return 0u;
    }

    s_tl_cpt    = cpt;
    s_tl_counts = counts;
    s_tl_short  = 0u;
    s_tl_armed  = 1u;
    s_tl_sleeps++;
    arkrtos_port_start_timer_counts(counts);
    ARKRTOS_WFI();

    if(s_tl_armed == 0u)
    {
        /* 睡满整段：tick 中断已经结账并补发（见 tick handler 的包装），
         * 补发量在中断路径上计入 s_tl_comp，这里不重复记。 */
        return n;
    }

    /* 提前唤醒：节拍还挂在长周期上，这里自己结账，恢复正常周期后把整拍
     * 用节拍体补出去。补发走 arkrtos_kernel_tick_ticks() 而不是
     * arkrtos_kernel_tick_handler()：后者会重新结账，把刚摆好的短周期
     * 当成已经到点，凭空多补一拍。 */
    i = arkrtos_tl_settle();
    arkrtos_kernel_tick_ticks(i);
    return i;
}

#endif /* ARKRTOS_USE_TICKLESS */
