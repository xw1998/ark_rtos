/**
* @file test_smp_sched_host.c
* @brief arkrtos_smp_sched.c 的宿主用例：每核运行队列 + 显式迁移
* @details 被验的是次核那套纯状态机，不是"能不能上板"：
*            1. 迁移只接受 READY 任务，参数/核号/归属三道闸门的返回值；
*            2. 迁出会先摘 CPU0 就绪链（用桩记账），队列满时任务必须被
*               挂回就绪链而不是凭空消失（该分支结构性不可达，见下）；
*            3. 次核切换的四段：交接登记 / 存帧 / 选下一个 / 落地。四种
*               结局各有读数——原地不动（空闲↔空闲、时间片未尽、轮转后
*               仍是自己）、换任务、换回本核空闲帧、栈指针为 0 时留在原帧；
*            4. 交还分两条路：正在跑（先存帧再交还）与还在队列里（直接
*               摘出来），以及 CPU0 每拍 poll 只把 READY 的任务挂回就绪链；
*            5. 读数接口（cur / owned_by_other / selftest）的边界。
*
*          enter() 是 for(;;) WFI，宿主上不可能返回：这里把 arkrtos_port_wfi
*          做成"记一次、然后 longjmp 出来"，于是两条分支（非法核号空转 /
*          合法核号起节拍后等第一拍）都能被观测，而不是跳过不测。
*
*          明确没验证：真核间中断、真次核异常入口的压栈/ertn、次核上跑
*          真实任务、跨核内存序。那些只能上板。
*
* @author xw
* @date 2026.11.22
*/
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

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

#if ARKRTOS_USE_SMP_SCHED

/* ------------------------------------------------------------
 * 任务表与就绪链桩
 * ------------------------------------------------------------ */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count = 0;
int32          arkrtos_current_task_id = 0;

static int32 g_ready_add[ARKRTOS_TASK_MAX_NUM];
static int32 g_ready_del[ARKRTOS_TASK_MAX_NUM];
static int32 g_disarm[ARKRTOS_TASK_MAX_NUM];
static uint8 g_on_ready[ARKRTOS_TASK_MAX_NUM];

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
static uint32 g_cas;

static jmp_buf g_jmp;
static int     g_armed;
static int     g_wfi_budget;   /* >0 时先让循环走一次回边，再 longjmp */

void arkrtos_port_wfi(void)
{
    g_wfi++;
    if (g_armed != 0)
    {
        if (g_wfi_budget > 0)
        {
            g_wfi_budget--;
            return;
        }
        longjmp(g_jmp, 1);
    }
}

uint32 arkrtos_port_cpu_id(void)
{
    return 0u;
}

uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect,
                               uint32 new_value)
{
    uint32 old = *p_addr;

    g_cas++;
    if (old == expect)
    {
        *p_addr = new_value;
        return 1u;
    }
    return 0u;
}

void arkrtos_port_spin_hint(void) { }
void arkrtos_port_dmb(void)       { }

void arkrtos_port_start_timer(uint32 tick_period_us)
{
    (void)tick_period_us;
    g_start_timer++;
}

void arkrtos_port_enable_irq(void)
{
    g_enable_irq++;
}

/* ------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------ */

#define NTASK  6

static void fixture(void)
{
    int32 i;

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    memset(g_ready_add, 0, sizeof(g_ready_add));
    memset(g_ready_del, 0, sizeof(g_ready_del));
    memset(g_disarm, 0, sizeof(g_disarm));
    memset(g_on_ready, 0, sizeof(g_on_ready));

    arkrtos_task_count = NTASK;
    for (i = 0; i < NTASK; i++)
    {
        arkrtos_task_table[i].status        = ARKRTOS_TASK_READY;
        arkrtos_task_table[i].priority      = 5u;
        arkrtos_task_table[i].stack_ptr     = (unsigned long)(0x1000u + (uint32)i * 0x100u);
        arkrtos_task_table[i].stack_peak_low = 0xFFFFFFFFu;
        arkrtos_task_table[i].ready_next    = ARKRTOS_TASK_NIL;
        arkrtos_task_table[i].ready_prev    = ARKRTOS_TASK_NIL;
    }
    /* idx2 比 idx3 低优先级，用来验"选队列里数值最小者" */
    arkrtos_task_table[2].priority = 7u;
    arkrtos_task_table[3].priority = 2u;
    arkrtos_task_table[5].priority = 1u;
}

static void snap(uint32 *out)
{
    memset(out, 0, 8u * sizeof(uint32));
    CK_I(arkrtos_smp_sched_selftest(out), 0);
}

/* ------------------------------------------------------------
 * 0. 未初始化时的 poll 是空操作
 * ------------------------------------------------------------ */

static void test_poll_before_init(void)
{
    arkrtos_smp_sched_poll();          /* s_inited == 0：直接返回 */
    CK_I(g_ready_add[0], 0);
    CK_I(g_ready_add[1], 0);
}

/* ------------------------------------------------------------
 * 1. 查询接口边界（初始化前也必须是良定义回答）
 * ------------------------------------------------------------ */

static void test_queries(void)
{
    CK_I(arkrtos_smp_sched_selftest(NULL), -1);
    CK_I(arkrtos_smp_sched_cur(99u), ARKRTOS_SMP_SCHED_NIL);
    CK_I(arkrtos_smp_sched_owned_by_other(-1), 0);
    CK_I(arkrtos_smp_sched_owned_by_other((int32)ARKRTOS_TASK_MAX_NUM), 0);
    CK_I(arkrtos_smp_sched_owned_by_other(0), 0);
}

/* ------------------------------------------------------------
 * 2. enter()：非法核号空转；合法核号起本核节拍后等第一拍
 * ------------------------------------------------------------ */

static void test_enter(void)
{
    uint32 timer0 = g_start_timer;
    uint32 irq0   = g_enable_irq;
    uint32 wfi0   = g_wfi;

    g_armed = 1;
    g_wfi_budget = 1;                  /* 先走一次循环回边，再脱出 */
    if (setjmp(g_jmp) == 0)
    {
        arkrtos_smp_sched_enter(0u);   /* 核号非法：只 WFI，不碰定时器 */
        CK(0);                         /* 不该返回 */
    }
    CK_I(g_start_timer, timer0);
    CK_I(g_enable_irq, irq0);

    g_wfi_budget = 1;
    if (setjmp(g_jmp) == 0)
    {
        arkrtos_smp_sched_enter(2u);   /* 合法次核 */
        CK(0);
    }
    g_armed = 0;
    CK_I(g_start_timer, timer0 + 1u);
    CK_I(g_enable_irq, irq0 + 1u);
    CK_I(g_wfi, wfi0 + 4u);
    CK_I(arkrtos_smp_sched_cur(2u), ARKRTOS_SMP_SCHED_NIL);
}

/* ------------------------------------------------------------
 * 3. 空闲↔空闲原地返回，不计假切换
 * ------------------------------------------------------------ */

static void test_idle_stay(void)
{
    uint32 out[8];

    g_armed = 0;
    CK_I(arkrtos_smp_sched_switch(1u, 0xA000u), 0xA000u);
    snap(out);
    CK_I(out[0], 0xFFFFFFFFu);
    CK_I(out[1], 0);
    CK_I(out[2], 0);
}

/* ------------------------------------------------------------
 * 4. 迁移、上 CPU、时间片、轮转、交还、回收
 * ------------------------------------------------------------ */

static void test_migrate_and_switch(void)
{
    uint32 out[8];

    fixture();

    /* --- 迁移闸门 --- */
    CK_I(arkrtos_smp_sched_task_to(1, 0u), -2);                        /* CPU0 走 back */
    CK_I(arkrtos_smp_sched_task_to(1, 99u), -1);                       /* 核号越界 */
    CK_I(arkrtos_smp_sched_task_to(0, 1u), -1);                        /* 任务号 0 非法 */
    CK_I(arkrtos_smp_sched_task_to(-1, 1u), -1);
    CK_I(arkrtos_smp_sched_task_to((int32)ARKRTOS_TASK_MAX_NUM + 1, 1u), -1);
    CK_I(arkrtos_smp_sched_task_to(NTASK + 1, 1u), -1);                /* idx >= task_count */
    arkrtos_task_table[4].status = ARKRTOS_TASK_WAIT;
    CK_I(arkrtos_smp_sched_task_to(5, 1u), -3);                        /* 非 READY */
    arkrtos_task_table[4].status = ARKRTOS_TASK_READY;

    /* 正在 CPU0 上跑的那一个：READY 且还挂在就绪链上，但迁走就是两核共用
     * 一份栈，必须挡住（-6）。放在真正迁出之前测，才不会被 -4 先命中。 */
    arkrtos_current_task_id = 1;
    CK_I(arkrtos_smp_sched_task_to(1, 1u), -6);                        /* 正在跑 */
    arkrtos_current_task_id = 0;

    /* --- 迁入 task1（idx0）：摘就绪链、摘延时链（幂等）--- */
    CK_I(arkrtos_smp_sched_task_to(1, 1u), 0);
    CK_I(g_ready_del[0], 1);
    CK_I(g_disarm[0], 1);
    CK_I(arkrtos_smp_sched_owned_by_other(0), 1);
    CK_I(arkrtos_smp_sched_task_to(1, 1u), -4);                        /* 已归次核 */
    snap(out);
    CK_I(out[1], 1);                                                   /* 队列长度 */
    CK_I(out[3], 1);                                                   /* 迁入 */
    CK_I(out[7], 1);                                                   /* 归属任务数 */

    /* --- 空闲 → task1：取空闲帧、上任务帧 --- */
    CK_I(arkrtos_smp_sched_switch(1u, 0xA100u), 0x1000u);
    CK_I(arkrtos_smp_sched_cur(1u), 0);
    /* 帧属于被切下的空闲上下文，存进 idle 槽；任务自己的 sp 不动 */
    CK_I(arkrtos_task_table[0].stack_ptr, 0x1000u);
    CK_I((long)arkrtos_task_table[0].stack_peak_low, 0xFFFFFFFFL);
    snap(out);
    CK_I(out[0], 0);
    CK_I(out[1], 0);
    CK_I(out[2], 1);
    CK_I(out[3], 1);

    /* --- 时间片未尽：原地返回，帧不变 --- */
    CK_I(arkrtos_smp_sched_switch(1u, 0xA200u), 0xA200u);
    CK_I(arkrtos_smp_sched_cur(1u), 0);
    CK_I(arkrtos_task_table[0].stack_ptr, 0xA200u);
    snap(out);
    CK_I(out[1], 1);                                                   /* 放回队尾 */
    CK_I(out[2], 1);                                                   /* 不算切换 */

    /* --- 同优先级轮转：时间片尽后换到队首的另一个任务 --- */
    CK_I(arkrtos_smp_sched_task_to(2, 1u), 0);                         /* idx1，同优先级 */
    snap(out);
    CK_I(out[1], 2);
    CK_I(g_on_ready[1], 0);
    CK_I(arkrtos_smp_sched_switch(1u, 0xA300u), 0x1100u);
    CK_I(arkrtos_smp_sched_cur(1u), 1);
    snap(out);
    CK_I(out[1], 1);
    CK_I(out[2], 2);

    /* --- 交还"正在跑"的任务：先存帧，再换回本核空闲帧 --- */
    CK_I(arkrtos_smp_sched_task_back(2), 0);
    CK_I(arkrtos_smp_sched_task_back(2), 0);                           /* 幂等 */
    snap(out);
    CK_I(out[1], 1);                                                   /* 队列里 idx0 还没上 */
    CK_I(arkrtos_smp_sched_switch(1u, 0xA400u), 0xA300u);              /* idx0 在 D 步存的帧 */
    CK_I(arkrtos_task_table[1].stack_ptr, 0xA400u);
    CK_I(arkrtos_smp_sched_cur(1u), 0);                                /* 队列里 idx0 接上 */
    CK_I(arkrtos_smp_sched_owned_by_other(1), 0);
    snap(out);
    CK_I(out[4], 1);                                                   /* 迁出 */
    CK_I(out[6], 1);                                                   /* 等待 poll */
    CK_I(out[2], 3);

    /* --- poll 只把 READY 的任务挂回 CPU0 就绪链 --- */
    CK_I(g_on_ready[1], 0);
    arkrtos_smp_sched_poll();
    CK_I(g_on_ready[1], 1);
    CK_I(g_ready_add[1], 1);
    snap(out);
    CK_I(out[6], 0);
    CK_I(out[7], 1);                                                   /* idx0 仍归次核 */

    /* --- 交还"还在队列里"的任务：直接摘出，不经过运行 --- */
    CK_I(arkrtos_smp_sched_task_to(3, 1u), 0);                         /* idx2 */
    CK_I(arkrtos_smp_sched_task_to(4, 1u), 0);                         /* idx3 */
    CK_I(arkrtos_smp_sched_task_back(3), 0);
    snap(out);
    CK_I(out[1], 2);
    CK_I(arkrtos_smp_sched_switch(1u, 0xA500u), 0x1300u);              /* idx3 优先级更低 */
    CK_I(arkrtos_smp_sched_cur(1u), 3);
    CK_I(arkrtos_smp_sched_owned_by_other(2), 0);
    snap(out);
    CK_I(out[1], 1);                                                   /* 队列里剩 idx0 */
    CK_I(out[4], 2);

    arkrtos_smp_sched_poll();
    CK_I(g_on_ready[2], 1);
    CK_I(g_ready_add[2], 1);

    /* --- 栈指针为 0：状态照常落地，但返回原帧而不是 0 --- */
    arkrtos_task_table[5].stack_ptr = 0u;
    CK_I(arkrtos_smp_sched_task_to(6, 1u), 0);                         /* idx5，最高优先级 */
    CK_I(arkrtos_smp_sched_switch(1u, 0xA600u), 0xA600u);
    CK_I(arkrtos_smp_sched_cur(1u), 5);                                /* 状态已改 */
    snap(out);
    CK_I(out[2], 5);

    /* --- 轮转一圈还是自己：第二次选择仍等于 cur 就原地返回 --- */
    arkrtos_task_table[5].stack_ptr = 0x1500u;
    CK_I(arkrtos_smp_sched_switch(1u, 0xA700u), 0xA700u);              /* slice 1 -> 0 */
    CK_I(arkrtos_smp_sched_switch(1u, 0xA800u), 0xA800u);              /* slice 0：轮转后仍自己 */
    CK_I(arkrtos_smp_sched_cur(1u), 5);
    snap(out);
    CK_I(out[2], 5);
}

/* ------------------------------------------------------------
 * 5. 节拍计数与非法核号
 * ------------------------------------------------------------ */

static void test_tick_and_bad_cpu(void)
{
    uint32 out[8];
    uint32 before;

    snap(out);
    before = out[5];
    arkrtos_smp_sched_tick(1u);
    arkrtos_smp_sched_tick(1u);
    arkrtos_smp_sched_tick(1u);
    arkrtos_smp_sched_tick(0u);        /* 本核也累加，只是不看这一槽 */
    arkrtos_smp_sched_tick(99u);       /* 越界：不累加 */
    snap(out);
    CK_I(out[5], before + 3u);

    /* 非法核号的 switch 原样返回，状态一点不动 */
    snap(out);
    before = out[2];
    CK_I(arkrtos_smp_sched_switch(0u, 0xB000u), 0xB000u);
    CK_I(arkrtos_smp_sched_switch(99u, 0xB001u), 0xB001u);
    CK_I(arkrtos_smp_sched_cur(1u), 5);
    snap(out);
    CK_I(out[2], before);
}

/* ------------------------------------------------------------
 * 6. task_back 的闸门与交还登记
 * ------------------------------------------------------------ */

static void test_task_back(void)
{
    CK_I(arkrtos_smp_sched_task_back(0), -1);
    CK_I(arkrtos_smp_sched_task_back(-1), -1);
    CK_I(arkrtos_smp_sched_task_back((int32)ARKRTOS_TASK_MAX_NUM + 1), -1);
    CK_I(arkrtos_smp_sched_task_back(NTASK + 1), -1);
    CK_I(arkrtos_smp_sched_task_back(3), -4);                          /* idx2 已还回 CPU0 */

    CK_I(arkrtos_smp_sched_task_back(4), 0);                           /* idx3 归次核 */
    CK_I(arkrtos_smp_sched_task_back(4), 0);

    /* 处理掉这条交接（idx3 此刻在队列里，不在跑），再 poll 干净 */
    (void)arkrtos_smp_sched_switch(1u, 0xA900u);
    arkrtos_smp_sched_poll();
    CK_I(g_on_ready[3], 1);

    /* 交还正在跑的任务、队列也空：换回本核空闲帧（不是原地返回） */
    CK_I(arkrtos_smp_sched_task_back(6), 0);                           /* idx5 正在跑 */
    (void)arkrtos_smp_sched_switch(1u, 0xB100u);                       /* 切到队列里的 idx0 */
    CK_I(arkrtos_smp_sched_cur(1u), 0);
    CK_I(arkrtos_smp_sched_task_back(1), 0);                           /* 现在 cur 是 idx0 */
    CK_I(arkrtos_smp_sched_switch(1u, 0xB200u), 0xA100u);              /* 队列空 → 空闲帧 */
    CK_I(arkrtos_smp_sched_cur(1u), ARKRTOS_SMP_SCHED_NIL);
    arkrtos_smp_sched_poll();
    CK_I(g_on_ready[0], 1);
    CK_I(arkrtos_smp_sched_owned_by_other(0), 0);
    CK_I(arkrtos_smp_sched_owned_by_other(5), 0);
}

#else   /* ARKRTOS_USE_SMP_SCHED == 0 */

/* 关档：头文件不声明任何入口，翻译单元为空。这里只钉住"没编进来"
 * 这件事本身；"空目标里没有全局符号"由 ci_gate 用 nm 复核。 */
static void test_compiled_out(void)
{
    CK_I(ARKRTOS_USE_SMP_SCHED, 0);
}

#endif  /* ARKRTOS_USE_SMP_SCHED */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

#if ARKRTOS_USE_SMP_SCHED
    test_poll_before_init();
    test_queries();
    test_enter();
    test_idle_stay();
    test_migrate_and_switch();
    test_tick_and_bad_cpu();
    test_task_back();
#else
    test_compiled_out();
#endif

    if (g_fails != 0)
    {
        printf("SMP-SCHED-HOST FAIL  %d check(s), %d failure(s)\n",
               g_checks, g_fails);
        return 1;
    }
    printf("SMP-SCHED-HOST OK  %d checks\n", g_checks);
    return 0;
}
