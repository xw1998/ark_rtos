/**
* @file test_smp_sched_balance_host.c
* @brief arkrtos_smp_sched.c 的自动均衡档（ARKRTOS_USE_SMP_BALANCE=1）宿主用例
* @details 被验的是策略层那几条纯状态转移，不是"能不能上板"：
*            1. 候选表三道闸门（任务号非法 / 已在表里 / 表满）与摘除的 -4；
*            2. 读数快照：空指针 -1，四个计数各自独立、节拍每拍 +1；
*            3. 规则 1（次核空闲 + CPU0 就绪 >= 2）真的迁出，且候选筛选四道
*               （不在就绪链 / 是当前任务 / 非 READY / 已归别核）各自挡住；
*            4. 规则 2（CPU0 就绪为 0 + 次核排队 >= 2）从队尾挑候选要回，
*               队列里全是非候选时不动。
*
*          模块状态是进程级静态量，没有"复位"接口（真实内核里也不需要），
*          所以这里所有计数都按**增量**判：先取快照，再做一步，再比差。
*          队列也按"用完就还回 CPU0"逐段清干净，每段都自证空闲。
*
*          与 test_smp_sched_host.c 分开的原因：那一档钉的是迁出/切换的状态机，
*          打开 BALANCE 会让 poll 多做一步、把既有断言全打乱；这里只开均衡档，
*          两条用例的覆盖并集才是这个模块的读数。
*
*          明确没验证：真核间中断、次核真的跑起来、跨核内存序。那些只能上板。
*
* @author xw
* @date 2026.11.23
*/
#include <stdio.h>
#include <string.h>

#include "arkrtos_smp_sched.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_hal.h"
#include "arkrtos_spin.h"

static int g_checks;
static int g_fails;

#define CK(cond)                                                          \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            g_fails++;                                                    \
        }                                                                 \
    } while (0)

#define CK_I(got, want)                                                   \
    do {                                                                  \
        long g_ = (long)(got);                                            \
        long w_ = (long)(want);                                           \
        g_checks++;                                                       \
        if (g_ != w_) {                                                   \
            printf("FAIL %s:%d  got %ld want %ld\n", __FILE__, __LINE__,  \
                   g_, w_);                                               \
            g_fails++;                                                    \
        }                                                                 \
    } while (0)

#if ARKRTOS_USE_SMP_BALANCE

/* ------------------------------------------------------------
 * 任务表 / 就绪链 / 调度器读数桩
 * ------------------------------------------------------------ */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count = 0;
int32          arkrtos_current_task_id = 0;

static int32  g_ready_add[ARKRTOS_TASK_MAX_NUM];
static int32  g_ready_del[ARKRTOS_TASK_MAX_NUM];
static int32  g_disarm[ARKRTOS_TASK_MAX_NUM];
static uint8  g_on_ready[ARKRTOS_TASK_MAX_NUM];
static uint32 g_ready_count;

void arkrtos_ready_add(int32 task_idx)
{
    if ((task_idx >= 0) && (task_idx < (int32)ARKRTOS_TASK_MAX_NUM))
    {
        g_ready_add[task_idx]++;
        g_on_ready[task_idx] = 1u;
    }
}

void arkrtos_ready_del(int32 task_idx)
{
    if ((task_idx >= 0) && (task_idx < (int32)ARKRTOS_TASK_MAX_NUM))
    {
        g_ready_del[task_idx]++;
        g_on_ready[task_idx] = 0u;
    }
}

uint8 arkrtos_ready_contains(int32 task_idx)
{
    if ((task_idx >= 0) && (task_idx < (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return g_on_ready[task_idx];
    }
    return 0u;
}

uint32 arkrtos_sched_ready_count(void)
{
    return g_ready_count;
}

void arkrtos_delay_disarm(int32 task_idx)
{
    if ((task_idx >= 0) && (task_idx < (int32)ARKRTOS_TASK_MAX_NUM))
    {
        g_disarm[task_idx]++;
    }
}

/* ------------------------------------------------------------
 * HAL / port 桩
 * ------------------------------------------------------------ */

static uint32 g_wfi;
static uint32 g_start_timer;
static uint32 g_enable_irq;

void arkrtos_port_wfi(void)                       { g_wfi++; }
uint32 arkrtos_port_cpu_id(void)                  { return 0u; }
void arkrtos_port_spin_hint(void)                 { }
void arkrtos_port_dmb(void)                       { }
void arkrtos_port_start_timer(uint32 t)           { (void)t; g_start_timer++; }
void arkrtos_port_enable_irq(void)                { g_enable_irq++; }

uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,
                               uint32 new_value)
{
    uint32 old = *p_addr;

    if (old == expect)
    {
        *p_addr = new_value;
        return 1u;
    }
    return 0u;
}

/* ------------------------------------------------------------
 * 夹具：任务表清零（模块状态不清，计数一律走增量）
 * ------------------------------------------------------------ */

#define NTASK  8

static void fixture(void)
{
    int32 i;

    memset((void *)arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    memset(g_ready_add, 0, sizeof(g_ready_add));
    memset(g_ready_del, 0, sizeof(g_ready_del));
    memset(g_disarm, 0, sizeof(g_disarm));
    memset(g_on_ready, 0, sizeof(g_on_ready));
    g_ready_count = 0u;
    arkrtos_current_task_id = 0;

    arkrtos_task_count = NTASK;
    for (i = 0; i < NTASK; i++)
    {
        arkrtos_task_table[i].status         = ARKRTOS_TASK_READY;
        arkrtos_task_table[i].priority       = 5u;
        arkrtos_task_table[i].stack_ptr      = (unsigned long)(0x2000u + (uint32)i * 0x100u);
        arkrtos_task_table[i].stack_peak_low = 0xFFFFFFFFu;
        arkrtos_task_table[i].ready_next     = ARKRTOS_TASK_NIL;
        arkrtos_task_table[i].ready_prev     = ARKRTOS_TASK_NIL;
        g_on_ready[i] = 1u;
    }
}

/* 快照 + 差值：out[0..3] = 迁出 / 要回 / 无候选 / 拍数 */
static void stat(uint32 *out)
{
    memset(out, 0, 4u * sizeof(uint32));
    CK_I(arkrtos_smp_sched_balance_stat(out), 0);
}

/* ------------------------------------------------------------
 * 0. 未初始化时 poll 是空操作（s_inited == 0）
 * ------------------------------------------------------------ */

static void test_poll_before_init(void)
{
    arkrtos_smp_sched_poll();
    CK_I(g_ready_add[0], 0);
    CK_I(g_ready_del[0], 0);
}

/* ------------------------------------------------------------
 * 1. 候选表：四道闸门 + 重复 -3 + 表满 -2 + 摘除 -4
 * ------------------------------------------------------------ */

static void test_candidate_table(void)
{
    fixture();

    CK_I(arkrtos_smp_sched_auto_count(), 0);

    CK_I(arkrtos_smp_sched_auto_add(0), -1);
    CK_I(arkrtos_smp_sched_auto_add(-1), -1);
    CK_I(arkrtos_smp_sched_auto_add((int32)ARKRTOS_TASK_MAX_NUM + 1), -1);
    CK_I(arkrtos_smp_sched_auto_add(NTASK + 1), -1);               /* idx >= count */

    CK_I(arkrtos_smp_sched_auto_add(1), 0);
    CK_I(arkrtos_smp_sched_auto_count(), 1);
    CK_I(arkrtos_smp_sched_auto_add(1), -3);                       /* 已在表里 */

    CK_I(arkrtos_smp_sched_auto_add(2), 0);
    CK_I(arkrtos_smp_sched_auto_add(3), 0);
    CK_I(arkrtos_smp_sched_auto_add(4), 0);
    CK_I(arkrtos_smp_sched_auto_count(), 4);
    CK_I(arkrtos_smp_sched_auto_add(5), -2);                       /* 表满 */

    CK_I(arkrtos_smp_sched_auto_del(0), -1);
    CK_I(arkrtos_smp_sched_auto_del(5), -4);                       /* 不在表里 */
    CK_I(arkrtos_smp_sched_auto_del(1), 0);
    CK_I(arkrtos_smp_sched_auto_count(), 3);
    CK_I(arkrtos_smp_sched_auto_del(1), -4);

    CK_I(arkrtos_smp_sched_auto_del(2), 0);
    CK_I(arkrtos_smp_sched_auto_del(3), 0);
    CK_I(arkrtos_smp_sched_auto_del(4), 0);
    CK_I(arkrtos_smp_sched_auto_count(), 0);
}

/* ------------------------------------------------------------
 * 2. 读数：空指针 -1；无候选无排队时每 poll 只加拍数
 * ------------------------------------------------------------ */

static void test_stat_snapshot(void)
{
    uint32 out[4];
    uint32 tick0;

    CK_I(arkrtos_smp_sched_balance_stat(NULL), -1);

    fixture();
    stat(out);
    tick0 = out[3];
    CK_I(out[0], 0);
    CK_I(out[1], 0);

    arkrtos_smp_sched_poll();
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[3], tick0 + 2u);
    CK_I(out[0], 0);
    CK_I(out[1], 0);
}

/* ------------------------------------------------------------
 * 3. 规则 1：次核空闲 + CPU0 就绪 >= 2 => 迁出第一个合格候选
 * ------------------------------------------------------------ */

static void test_rule_move_out(void)
{
    uint32 out[4];
    uint32 out0;
    uint32 idle0;

    /* 两个次核（ARKRTOS_CPU_NUM = 3）。桩里的"CPU0 就绪数"是个显式变量，
     * 真实内核里 task_to 会 ready_del 掉一个，所以这里手工同步，口径一致。 */
    fixture();
    CK_I(arkrtos_smp_sched_auto_count(), 0);
    g_ready_count = 2u;
    CK_I(arkrtos_smp_sched_auto_add(1), 0);              /* idx0 */
    stat(out);
    out0  = out[0];
    idle0 = out[2];

    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[0], out0 + 1u);                             /* 自动迁出一次 */
    CK_I(out[2], idle0);                                 /* 不是"无候选" */
    CK_I(arkrtos_smp_sched_owned_by_other(0), 1);
    CK_I(g_ready_del[0], 1);
    CK_I(g_disarm[0], 1);
    g_ready_count = 1u;                                  /* 迁走一个，CPU0 只剩一个 */

    /* 就绪只剩 1：另一个空闲次核也不该把 CPU0 最后那一件抢走 */
    CK_I(arkrtos_smp_sched_auto_add(2), 0);              /* idx1 */
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[0], out0 + 1u);
    CK_I(arkrtos_smp_sched_owned_by_other(1), 0);
    g_ready_count = 2u;

    /* 清场：先摘候选（否则下面的 poll 会立刻把它再迁走），再走
     * task_back -> 次核摘队 -> CPU0 收走挂回就绪链 这条完整路径。 */
    CK_I(arkrtos_smp_sched_auto_del(1), 0);
    CK_I(arkrtos_smp_sched_auto_del(2), 0);
    CK_I(arkrtos_smp_sched_task_back(1), 0);
    (void)arkrtos_smp_sched_switch(1u, 0xD000u);
    arkrtos_smp_sched_poll();
    CK_I(g_ready_add[0] >= 1, 1);
    CK_I(arkrtos_smp_sched_owned_by_other(0), 0);
}

/* ------------------------------------------------------------
 * 4. 规则 1 的候选筛选：四种不合格各自挡住；全挡只记"无候选"
 * ------------------------------------------------------------ */

static void test_rule_filter(void)
{
    uint32 out[4];
    uint32 out0;
    uint32 idle0;

    fixture();
    CK_I(arkrtos_smp_sched_auto_count(), 0);
    g_ready_count = 2u;

    /* idx3 先归 cpu1：cpu1 队列非空，本段就只剩 cpu2 一个空闲次核，
     * 这样"候选全不合格 ⇒ 记一次无候选"不会被另一个次核的迁出掩盖。 */
    CK_I(arkrtos_smp_sched_task_to(4, 1u), 0);
    g_ready_count = 2u;                                  /* 桩不联动，回到本段口径 */

    CK_I(arkrtos_smp_sched_auto_add(1), 0);              /* idx0：下面从就绪链摘掉 */
    CK_I(arkrtos_smp_sched_auto_add(2), 0);              /* idx1：当前任务 */
    CK_I(arkrtos_smp_sched_auto_add(3), 0);              /* idx2：非 READY */
    CK_I(arkrtos_smp_sched_auto_add(4), 0);              /* idx3：已归 cpu1 */

    g_on_ready[0] = 0u;
    arkrtos_current_task_id = 2;
    arkrtos_task_table[2].status = ARKRTOS_TASK_WAIT;

    stat(out);
    out0  = out[0];
    idle0 = out[2];
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[0], out0);                                  /* 一个都没迁 */
    CK_I(out[2], idle0 + 1u);                            /* cpu2 记了一次"无候选" */
    CK_I(arkrtos_smp_sched_owned_by_other(0), 0);
    CK_I(arkrtos_smp_sched_owned_by_other(1), 0);
    CK_I(arkrtos_smp_sched_owned_by_other(2), 0);

    /* idx0 回到就绪链、且不再是当前任务 => 下一拍迁它（cpu1 非空，落 cpu2） */
    g_on_ready[0] = 1u;
    arkrtos_current_task_id = 0;
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[0], out0 + 1u);
    CK_I(arkrtos_smp_sched_owned_by_other(0), 1);

    /* 清场：先摘候选，再把两个迁出去的任务分两条路要回 */
    CK_I(arkrtos_smp_sched_auto_del(1), 0);
    CK_I(arkrtos_smp_sched_auto_del(2), 0);
    CK_I(arkrtos_smp_sched_auto_del(3), 0);
    CK_I(arkrtos_smp_sched_auto_del(4), 0);

    CK_I(arkrtos_smp_sched_task_back(1), 0);             /* idx0 在 cpu2 */
    (void)arkrtos_smp_sched_switch(2u, 0xD100u);
    arkrtos_smp_sched_poll();

    CK_I(arkrtos_smp_sched_task_back(4), 0);             /* idx3 在 cpu1 */
    (void)arkrtos_smp_sched_switch(1u, 0xD200u);
    arkrtos_smp_sched_poll();

    CK_I(arkrtos_smp_sched_owned_by_other(0), 0);
    CK_I(arkrtos_smp_sched_owned_by_other(3), 0);
}

/* ------------------------------------------------------------
 * 5. 规则 2：CPU0 就绪为 0 + 次核排队 >= 2 => 从队尾挑候选要回
 * ------------------------------------------------------------ */

static void test_rule_pull_back(void)
{
    uint32 out[4];
    uint32 back0;

    /* --- 5a. 次核排两个、但都不是候选：不动 --- */
    fixture();
    CK_I(arkrtos_smp_sched_auto_count(), 0);
    g_ready_count = 0u;
    CK_I(arkrtos_smp_sched_task_to(6, 1u), 0);           /* idx5 */
    CK_I(arkrtos_smp_sched_task_to(7, 1u), 0);           /* idx6 */
    stat(out);
    back0 = out[1];
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[1], back0);
    CK_I(arkrtos_smp_sched_owned_by_other(5), 1);
    CK_I(arkrtos_smp_sched_owned_by_other(6), 1);

    /* 清场：两个都从次核要回 */
    CK_I(arkrtos_smp_sched_task_back(6), 0);
    (void)arkrtos_smp_sched_switch(1u, 0xC000u);
    arkrtos_smp_sched_poll();
    CK_I(arkrtos_smp_sched_task_back(7), 0);
    (void)arkrtos_smp_sched_switch(1u, 0xC100u);
    arkrtos_smp_sched_poll();
    CK_I(arkrtos_smp_sched_owned_by_other(5), 0);
    CK_I(arkrtos_smp_sched_owned_by_other(6), 0);

    /* --- 5b. 队尾是候选：登记要回它，队首那个留在次核 --- */
    fixture();
    CK_I(arkrtos_smp_sched_auto_count(), 0);
    g_ready_count = 0u;
    CK_I(arkrtos_smp_sched_auto_add(3), 0);              /* idx2 */
    CK_I(arkrtos_smp_sched_auto_add(4), 0);              /* idx3 */
    CK_I(arkrtos_smp_sched_task_to(3, 1u), 0);           /* idx2 先入队 */
    CK_I(arkrtos_smp_sched_task_to(4, 1u), 0);           /* idx3 后入队 = 队尾 */
    stat(out);
    back0 = out[1];
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[1], back0 + 1u);                            /* 登记了一次要回 */
    CK_I(arkrtos_smp_sched_owned_by_other(2), 1);        /* 队首的 idx2 没动 */

    /* 交还：次核摘队 -> CPU0 收走 -> 下一拍 poll 挂回就绪链 */
    (void)arkrtos_smp_sched_switch(1u, 0xC200u);
    g_ready_count = 1u;
    arkrtos_smp_sched_poll();
    CK_I(arkrtos_smp_sched_owned_by_other(3), 0);        /* 队尾的 idx3 真回来了 */
    CK_I(g_ready_add[3] >= 1, 1);

    /* --- 5c. 队列只剩一个：规则 2 门槛不满足 --- */
    stat(out);
    back0 = out[1];
    CK_I(arkrtos_smp_sched_task_back(3), 0);             /* 把剩下的 idx2 要回 */
    (void)arkrtos_smp_sched_switch(1u, 0xC300u);
    arkrtos_smp_sched_poll();
    CK_I(arkrtos_smp_sched_owned_by_other(2), 0);
    g_ready_count = 0u;
    CK_I(arkrtos_smp_sched_task_to(4, 1u), 0);           /* idx3 单独入队 */
    arkrtos_smp_sched_poll();
    stat(out);
    CK_I(out[1], back0);                                 /* 没再要回 */
    CK_I(arkrtos_smp_sched_owned_by_other(3), 1);

    /* 清场 */
    CK_I(arkrtos_smp_sched_auto_del(3), 0);
    CK_I(arkrtos_smp_sched_auto_del(4), 0);
    CK_I(arkrtos_smp_sched_task_back(4), 0);
    (void)arkrtos_smp_sched_switch(1u, 0xC400u);
    arkrtos_smp_sched_poll();
    CK_I(arkrtos_smp_sched_owned_by_other(3), 0);
}

#else   /* ARKRTOS_USE_SMP_BALANCE == 0 */

static void test_compiled_out(void)
{
    CK_I(ARKRTOS_USE_SMP_BALANCE, 0);
}

#endif  /* ARKRTOS_USE_SMP_BALANCE */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

#if ARKRTOS_USE_SMP_BALANCE
    test_poll_before_init();
    test_candidate_table();
    test_stat_snapshot();
    test_rule_move_out();
    test_rule_filter();
    test_rule_pull_back();
#else
    test_compiled_out();
#endif

    if (g_fails != 0)
    {
        printf("SMP-BALANCE-HOST FAIL  %d check(s), %d failure(s)\n",
               g_checks, g_fails);
        return 1;
    }
    printf("SMP-BALANCE-HOST OK  %d checks\n", g_checks);
    return 0;
}
