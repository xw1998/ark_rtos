/**
* @file arkrtos_rt.c
* @brief 实时性度量实现（ARKRTOS_USE_RTSTAT，默认关）
* @details 所有时间戳都来自 arkrtos_port_get_cyccnt()（DWT 自由运行周期计数器）。
*          这里只做四件事：在固定位置取时间戳、算差值、按项记 min/avg/max、
*          折算成微秒。不猜测、不插值：某一项没有样本时 n = 0，读数行如实报 0，
*          而不是拿别的样本顶上去。
*
*          为什么用"最近一次节拍入口"当基准而不是"该唤醒的时刻"：
*          节拍是软件能看到的最早时间点，真正该唤醒的时刻由内核算出后就在
*          同一个中断里结账，两者之间没有可观测的空隙。所以
*          delta = now - tick_entry 就是"从节拍中断入口到任务继续执行"的全部
*          代价，既包含内核结账，也包含切换。
*
* @author xw
*/
#include "arkrtos_rt.h"

#if (ARKRTOS_USE_RTSTAT == 1)

#include "arkrtos_hal.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_sync.h"

#define RT_ROW_TICK_ISR     0u
#define RT_ROW_WAKE_LO      1u
#define RT_ROW_WAKE_HI      2u
#define RT_ROW_CRIT         3u

/* 辅助任务（场景二）的静态栈：只调两次 SVC 内部入口，256 字够用且留了余量。 */
#define RT_HELPER_STACK_WORDS   256u
#define RT_HELPER_PRIORITY      1u

typedef struct {
    uint32 n;
    uint32 min_c;
    uint32 max_c;
    uint32 sum_c;
} rt_acc_t;

typedef struct {
    int32  min_c;
    int32  max_c;
    uint32 n;
} rt_jit_t;

static struct {
    uint8  ready;               /* 周期计数器可用 */
    uint8  crit_depth;
    uint32 clk;                 /* CPU 主频（Hz） */
    uint32 per_tick;            /* 一拍多少周期 */

    uint32 last_tick_cyc;       /* 最近一次节拍入口的时间戳 */
    uint8  have_tick;
    uint32 tick_t0;             /* 本次节拍入口 */
    uint8  in_tick;
    uint32 crit_t0;

    rt_acc_t  acc[ARKRTOS_RT_ROWS];
    rt_jit_t  jit;
    uint32    late_n;           /* 唤醒场景里"超过一拍"的样本数 */

    uint8  helper_up;
    int32  sem_hi;              /* 辅助任务等它 */
    int32  sem_ack;             /* 调用者等回执 */
    volatile uint32 post_cyc;   /* 调用者 post 之前的时刻 */
    volatile uint32 hi_done;    /* 辅助任务已经采到的样本数 */
    volatile uint32 hi_target;  /* 本次要采多少 */
    volatile uint8  hi_run;     /* 1 = 场景进行中 */
    int32  last_err;            /* 辅助任务起不来时记下是哪一步失败 */
} g_rt;

static uint32 rt_helper_stack[RT_HELPER_STACK_WORDS];

static void rt_acc_add(rt_acc_t *p_acc, uint32 c)
{
    if(p_acc->n == 0u)
    {
        p_acc->min_c = c;
        p_acc->max_c = c;
        p_acc->sum_c = c;
        p_acc->n = 1u;
        return;
    }
    if(c < p_acc->min_c) { p_acc->min_c = c; }
    if(c > p_acc->max_c) { p_acc->max_c = c; }
    p_acc->sum_c += c;
    p_acc->n++;
}

static void rt_acc_clear(rt_acc_t *p_acc)
{
    p_acc->n = 0u;
    p_acc->min_c = 0u;
    p_acc->max_c = 0u;
    p_acc->sum_c = 0u;
}

static uint32 rt_to_us(uint32 c)
{
    unsigned long long v;
    if(g_rt.clk == 0u) { return 0u; }

    /* c * 1e6 / clk，用 64 位中间值避免 c 稍大就溢出。 */
    v = ((unsigned long long)c * 1000000ull) / (unsigned long long)g_rt.clk;
    return (uint32)v;
}

int32 arkrtos_rt_counter_alive(void)
{
    uint32 t0;
    uint32 t1;
    uint32 i;
    volatile uint32 acc = 0u;

    if(arkrtos_port_cyccnt_init() != 0) { return 0; }

    t0 = arkrtos_port_get_cyccnt();
    for(i = 0u; i < 200u; i++) { acc = acc + i; }
    t1 = arkrtos_port_get_cyccnt();
    (void)acc;

    return (t1 != t0) ? 1 : 0;
}

void arkrtos_rt_init(void)
{
    unsigned long long pt;

    g_rt.ready = 0u;
    g_rt.crit_depth = 0u;
    g_rt.have_tick = 0u;
    g_rt.in_tick = 0u;
    g_rt.late_n = 0u;
    g_rt.helper_up = 0u;
    g_rt.sem_hi = -1;
    g_rt.sem_ack = -1;
    g_rt.hi_run = 0u;
    g_rt.hi_done = 0u;
    g_rt.hi_target = 0u;
    g_rt.post_cyc = 0u;
    g_rt.last_err = 0;

    /* 先武装计数器、再清统计：cyccnt_init() 会把 CYCCNT 清零，如果先清统计
     * 再清零计数器，中间已经有一拍把时间戳记进了 last_tick_cyc，清零后下
     * 一拍就会算出一个巨大的负数抖动。ready = 0 期间 tick 钩子不记账。 */
    g_rt.ready = 0u;
    g_rt.clk = arkrtos_port_get_system_clock();
    if(arkrtos_rt_counter_alive() == 0)
    {
        /* 没有周期计数器（或者它转不动）：所有测量项保持 n = 0，shell 如实
         * 报"不可用"，不退回节拍计数去凑一个分辨率差三个数量级的数。
         *
         * 这个分支在调试器连着的时候会真的走到：刷完固件后调试器断开时会
         * 清掉 DEMCR.TRCENA，那一刻之后 CYCCNT 就停住了。所以 `rt run`
         * 在开测前会再调一次本函数把计数器重新武装起来，而不是假定开机时
         * 打开的就一直有效。 */
        g_rt.per_tick = 0u;
        g_rt.ready = 0u;
        return;
    }

    arkrtos_rt_reset();

    pt = ((unsigned long long)g_rt.clk * (unsigned long long)ARKRTOS_TICK_PERIOD_US) / 1000000ull;
    g_rt.per_tick = (uint32)pt;
    g_rt.ready = 1u;
}

void arkrtos_rt_reset(void)
{
    uint32 i;

    for(i = 0u; i < ARKRTOS_RT_ROWS; i++)
    {
        rt_acc_clear(&g_rt.acc[i]);
    }
    g_rt.jit.n = 0u;
    g_rt.jit.min_c = 0;
    g_rt.jit.max_c = 0;
    g_rt.late_n = 0u;
    g_rt.have_tick = 0u;
}

void arkrtos_rt_tick_enter(void)
{
    uint32 now;

    if(g_rt.ready == 0u) { return; }

    now = arkrtos_port_get_cyccnt();
    g_rt.tick_t0 = now;
    g_rt.in_tick = 1u;

    if(g_rt.have_tick != 0u)
    {
        /* 有符号差值：早到为负、晚到为正。用一个 32 位减法再解释成 int32，
         * 计数器回绕也被这条减法吃掉（两次采样间隔远小于 2^31 周期）。 */
        int32 d = (int32)(now - g_rt.last_tick_cyc) - (int32)g_rt.per_tick;

        if(g_rt.jit.n == 0u)
        {
            g_rt.jit.min_c = d;
            g_rt.jit.max_c = d;
        }
        else
        {
            if(d < g_rt.jit.min_c) { g_rt.jit.min_c = d; }
            if(d > g_rt.jit.max_c) { g_rt.jit.max_c = d; }
        }
        g_rt.jit.n++;
    }

    g_rt.last_tick_cyc = now;
    g_rt.have_tick = 1u;
}

void arkrtos_rt_tick_exit(void)
{
    if((g_rt.ready == 0u) || (g_rt.in_tick == 0u)) { return; }

    g_rt.in_tick = 0u;
    rt_acc_add(&g_rt.acc[RT_ROW_TICK_ISR],
               arkrtos_port_get_cyccnt() - g_rt.tick_t0);
}

void arkrtos_rt_crit_enter(void)
{
    if(g_rt.ready == 0u) { return; }

    /* 只记最外层：嵌套的内层不会让关中断的时间变长。 */
    if(g_rt.crit_depth == 0u)
    {
        g_rt.crit_t0 = arkrtos_port_get_cyccnt();
    }
    g_rt.crit_depth++;
}

void arkrtos_rt_crit_exit(void)
{
    uint32 c;

    if(g_rt.ready == 0u) { return; }
    if(g_rt.crit_depth == 0u) { return; }     /* 不成对：不算，也不下溢 */

    g_rt.crit_depth--;
    if(g_rt.crit_depth != 0u) { return; }

    c = arkrtos_port_get_cyccnt() - g_rt.crit_t0;

    /* 临界区本身很短，取时间戳的开销占比不低，所以这一项是"含测量的
     * 上界"——文档里单列了关掉本开关时的对照值。 */
    rt_acc_add(&g_rt.acc[RT_ROW_CRIT], c);
}

/* ---------------------------------------------------------------- 场景一 */

int32 arkrtos_rt_measure_wake(uint32 n)
{
    uint32 i;
    uint32 now;
    uint32 d;

    if(g_rt.ready == 0u) { return -1; }

    for(i = 0u; i < n; i++)
    {
        /* 1 ms 延时（在本配置下正好两拍）。醒来时最近一次节拍入口就是
         * 把本任务置 READY 的那一次，除非中途又被更高优先级挡了整拍。 */
        arkrtos_task_wait_internal(1u);
        now = arkrtos_port_get_cyccnt();

        if(g_rt.have_tick == 0u) { continue; }

        d = now - g_rt.last_tick_cyc;
        if(d > g_rt.per_tick)
        {
            g_rt.late_n++;
            continue;
        }
        rt_acc_add(&g_rt.acc[RT_ROW_WAKE_LO], d);
    }
    return 0;
}

/* ---------------------------------------------------------------- 场景二 */

static void rt_helper_task(void)
{
    for(;;)
    {
        if(arkrtos_sem_wait_internal(g_rt.sem_hi, -1) != 0) { continue; }

        if(g_rt.hi_run != 0u)
        {
            uint32 t0 = g_rt.post_cyc;
            uint32 t1 = arkrtos_port_get_cyccnt();     /* 醒来第一行 */

            if(g_rt.hi_done < g_rt.hi_target)
            {
                rt_acc_add(&g_rt.acc[RT_ROW_WAKE_HI], t1 - t0);
                g_rt.hi_done++;
            }
        }
        (void)arkrtos_sem_post_internal(g_rt.sem_ack);
    }
}

static int32 rt_helper_start(void)
{
    if(g_rt.helper_up != 0u) { return 0; }

    g_rt.last_err = 0;

    g_rt.sem_hi = arkrtos_sem_create_internal("rt_hi", 0);
    if(g_rt.sem_hi < 0) { g_rt.last_err = 1; return -1; }

    g_rt.sem_ack = arkrtos_sem_create_internal("rt_ack", 0);
    if(g_rt.sem_ack < 0) { g_rt.last_err = 2; return -1; }

    /* 用内核内部的 arkrtos_task_register()：辅助任务属于内核自己，不走
     * arkrtos_thread_create_internal() 的"入口必须在调用者自己的固件区内"
     * 校验（内核任务没有独立固件窗口，那条校验会直接拒掉内核自己的代码）。 */
    {
        int32 tid = arkrtos_task_register(rt_helper_task, rt_helper_stack,
                                          sizeof(rt_helper_stack),
                                          RT_HELPER_PRIORITY, 0u);
        if(tid <= 0) { g_rt.last_err = 4 - tid; return -1; }
    }
    g_rt.helper_up = 1u;
    return 0;
}

int32 arkrtos_rt_measure_preempt(uint32 n)
{
    uint32 i;

    if(g_rt.ready == 0u) { return -1; }
    if(rt_helper_start() != 0) { return -1; }

    g_rt.hi_done = 0u;
    g_rt.hi_target = n;
    g_rt.hi_run = 1u;

    for(i = 0u; i < n; i++)
    {
        /* 先记时间戳再 post：这个差值就是"一次同步唤醒 + 抢占"的全部代价，
         * 其中包含调用 post 的几条指令本身（量级见文档，不隐藏）。 */
        g_rt.post_cyc = arkrtos_port_get_cyccnt();
        (void)arkrtos_sem_post_internal(g_rt.sem_hi);
        (void)arkrtos_sem_wait_internal(g_rt.sem_ack, 100);
    }

    g_rt.hi_run = 0u;

    /* 辅助任务没跟上（超时）时样本数会少于 n，如实报出来。 */
    return (g_rt.hi_done == n) ? 0 : -1;
}

/* ---------------------------------------------------------------- 读数 */

static void rt_fill_row(arkrtos_rt_row_t *p_row, const char *name, uint32 idx)
{
    const rt_acc_t *p_acc = &g_rt.acc[idx];

    p_row->name = name;
    p_row->late_n = 0u;
    p_row->n = p_acc->n;
    if(p_acc->n == 0u)
    {
        p_row->min_c = 0u;
        p_row->avg_c = 0u;
        p_row->max_c = 0u;
        p_row->min_us = 0u;
        p_row->avg_us = 0u;
        p_row->max_us = 0u;
        return;
    }

    p_row->min_c = p_acc->min_c;
    p_row->max_c = p_acc->max_c;
    p_row->avg_c = p_acc->sum_c / p_acc->n;
    p_row->min_us = rt_to_us(p_acc->min_c);
    p_row->max_us = rt_to_us(p_acc->max_c);
    p_row->avg_us = rt_to_us(p_row->avg_c);
}

uint32 arkrtos_rt_snapshot(arkrtos_rt_row_t *rows, uint32 max_rows)
{
    if((rows == 0) || (max_rows == 0u)) { return 0u; }

    if(max_rows > 0u) { rt_fill_row(&rows[0], "tick.isr", RT_ROW_TICK_ISR); }
    if(max_rows > 1u) { rt_fill_row(&rows[1], "wake.lo", RT_ROW_WAKE_LO); }
    if(max_rows > 2u) { rt_fill_row(&rows[2], "wake.hi", RT_ROW_WAKE_HI); }
    if(max_rows > 3u) { rt_fill_row(&rows[3], "crit.max", RT_ROW_CRIT); }

    if(max_rows > 1u) { rows[1].late_n = g_rt.late_n; }

    return (max_rows < ARKRTOS_RT_ROWS) ? max_rows : ARKRTOS_RT_ROWS;
}

uint32 arkrtos_rt_jitter(int32 *min_c, int32 *max_c, uint32 *n)
{
    if(min_c != 0) { *min_c = g_rt.jit.min_c; }
    if(max_c != 0) { *max_c = g_rt.jit.max_c; }
    if(n != 0) { *n = g_rt.jit.n; }
    return g_rt.jit.n;
}

uint32 arkrtos_rt_cycles_per_tick(void)
{
    return g_rt.per_tick;
}

int32 arkrtos_rt_last_error(void)
{
    return g_rt.last_err;
}

#endif /* ARKRTOS_USE_RTSTAT */
