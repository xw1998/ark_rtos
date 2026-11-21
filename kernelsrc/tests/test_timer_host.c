/**
* @file test_timer_host.c
* @brief 软定时器（arkrtos_timer.c）的宿主单元测试
* @details 软定时器的状态分成三块，三块之间靠"标记"传递，而标记本身不带
*          归属与计数，于是它的破坏方式都很安静：

*            - tick 只倒计时与置 expired，真正干活的是服务任务；若服务任务
*              被唤醒的条件写错（比如只看 task_id 不看在不在 WAIT），回调
*              要么永远不跑，要么在一个正在运行的任务上硬改状态；
*            - 服务任务必须**先快照 cb/arg、清 expired、再开中断调回调**，
*              否则回调里 stop/delete 自己就会与快照打架；ONES HOT 还要求
*              在调回调之前就把 active 清掉，否则回调里再 start 会被覆盖；
*            - 句柄闸门若顺序写错（先查 used 再查标志位、或 delete 少查一
*              道），一个别的模块的句柄就可能删到定时器表里；
*            - period_ms 到 tick 的换算若溢出成 0 而没抬成 1，定时器就永远
*              不到期（remain 恒 0 在 tickless 下被当成"本拍到期"，普通档
*              下则彻底静默）。

*          被测的是真实的 arkrtos_timer.c。任务表、阻塞、就绪队列、
*          任务注册全部由本文件桩住；服务任务那个 while(1) 被
*          ARKRTOS_TIMER_HOST_TEST 档的预算改成一圈一圈可数的有限运行
*          （默认档不编这段，符号与行为零变化）。

*          覆盖范围（如实说明）：
*            已验证：module_init 清表、创建（最低空槽/句柄编码/名字 7 字符
*                    截断/表满/槽位复用）、start 的五道闸门与 period_ms 到
*                    tick 的换算（含 32 位乘法溢出被抬成 1 拍）、tick 倒计时
*                    与到期（oneshot 保 active、periodic 回填）、task_id<=0
*                    不唤醒、服务任务在 WAIT 时交接唤醒而在 READY 时不动、
*                    install 登记的参数、服务任务一圈预算：空闲阻塞、回调
*                    快照、oneshot 自动停、periodic 保持、回调内 stop 自己、
*                    回调内 delete 自己、cb==0 跳过、多个到期按槽位顺序处理、
*                    stop 两道闸门、delete 闸门与"删未用槽返回 0"、tickless
*                    next_ticks 的三条分支、以及关档全部拒绝。
*            未验证：真实节拍与真实调度器下的时序（谁先跑、回调被打断的窗
*                    口）、回调真的在任务上下文而不是中断上下文、以及只有
*                    上板才有的路径。
* @note 用法：由 tools/coverage_gate.py 的三条 case 编译并运行，输出
*       TIMER-HOST OK（默认档）、TIMER-HOST TL OK（TICKLESS=1 档）或
*       TIMER-HOST OFF OK（USE_TIMER=0 档）。
*/

#include "arkrtos_timer.h"
#include "arkrtos_config.h"
#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_tickless.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count = 0;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }

/* 服务任务"睡下去"：这里只数次数。真实现会把当前任务挂起并触发切换。 */
static int g_block_calls = 0;
void arkrtos_task_block_internal(void) { g_block_calls++; }

/* 就绪队列：记「谁被放回就绪队列」与次数。 */
static int g_ready_calls = 0;
static int g_ready_last  = -1;
void arkrtos_ready_add(int32 task_idx)
{
    g_ready_calls++;
    g_ready_last = (int)task_idx;
}

/* 任务注册桩：把参数记下来供断言，返回值可编程（模拟注册成功/失败）。 */
static int    g_reg_calls  = 0;
static int32  g_reg_ret    = -1;
static void (*g_reg_entry)(void) = 0;
static uint32 *g_reg_stack = 0;
static uint32 g_reg_size   = 0u;
static uint8  g_reg_prio   = 0u;
static uint32 g_reg_period = 0u;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                            uint8 priority, uint32 period_ms)
{
    g_reg_calls++;
    g_reg_entry  = entry;
    g_reg_stack  = stack_bottom;
    g_reg_size   = stack_size;
    g_reg_prio   = priority;
    g_reg_period = period_ms;
    return g_reg_ret;
}

/* ------------------------------------------------------------------
 * 断言
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

#define CK_EQ(a, b, msg)                                                     \
    do {                                                                     \
        long va = (long)(a);                                                 \
        long vb = (long)(b);                                                 \
        g_checks++;                                                          \
        if(va != vb)                                                         \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s (got %ld, want %ld)\n",                    \
                   __FILE__, __LINE__, msg, va, vb);                         \
        }                                                                    \
    } while(0)

static void cb_dummy(void *arg)
{
    (void)arg;
}

#if (ARKRTOS_USE_TIMER == 1)
extern int32 arkrtos_timer_daemon_budget;
#endif

/* 每个用例从"全清"开始。服务任务的循环预算只有宿主档有这个符号，
 * 关档时连表都不存在，所以预算那一行要跟着档位走。 */
static void reset_all(void)
{
    int32 i;

    arkrtos_timer_module_init();
    for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
    {
        memset(&arkrtos_task_table[i], 0, sizeof(arkrtos_task_table[i]));
        arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
    }
    g_block_calls = 0;
    g_ready_calls = 0;
    g_ready_last  = -1;
    g_reg_calls   = 0;
    g_reg_ret     = -1;
    g_reg_entry   = 0;
    g_reg_stack   = 0;
    g_reg_size    = 0u;
    g_reg_prio    = 0u;
    g_reg_period  = 0u;
#if (ARKRTOS_USE_TIMER == 1)
    arkrtos_timer_daemon_budget = 0;
#endif
}

#if (ARKRTOS_USE_TIMER == 1)

/* ------------------------------------------------------------------
 * 被测对象的内部符号
 * @details 只在 ARKRTOS_TIMER_HOST_TEST 档由 arkrtos_timer.c 导出：
 *          默认档里它们是 static，这里也不需要看。
 * ------------------------------------------------------------------ */
extern arkrtos_timer_obj_t arkrtos_timers[ARKRTOS_TIMER_NUM];
extern int32 arkrtos_timer_daemon_budget;
extern void arkrtos_timer_daemon(void);

/* 一个可复用的回调参数地址：只是"存进去再取出来"的凭证。 */
static int g_token = 0;

#define TH(i)        ((int32)(i) | ARKRTOS_TIMER_HANDLE_FLAG)
#define WRONG_TYPE   (0x00100000)

static void tick_n(int n)
{
    int i;
    for(i = 0; i < n; i++)
        arkrtos_timer_tick_handler();
}

/* ------------------------------------------------------------------
 * module_init：把整张表清干净，并把服务任务号退回哨兵
 * ------------------------------------------------------------------ */
static void test_module_init(void)
{
    int32 i;

    reset_all();
    CK_EQ(arkrtos_timer_create_internal("a"), TH(0), "a slot exists to dirty");
    arkrtos_timers[0].active  = 1;      /* 人为弄脏，看 init 是否清到 */
    arkrtos_timers[0].expired = 1;
    arkrtos_timers[0].cb      = cb_dummy;
    arkrtos_timers[0].arg     = &g_token;
    arkrtos_timer_module_init();

    for(i = 0; i < ARKRTOS_TIMER_NUM; i++)
    {
        CK_EQ(arkrtos_timers[i].used,         0, "init clears used");
        CK_EQ(arkrtos_timers[i].active,       0, "init clears active");
        CK_EQ(arkrtos_timers[i].expired,      0, "init clears expired");
        CK_EQ(arkrtos_timers[i].period_ticks, 0, "init clears period");
        CK_EQ(arkrtos_timers[i].remain_ticks, 0, "init clears remain");
        ck(arkrtos_timers[i].name[0] == 0, "init clears the name");
        ck(arkrtos_timers[i].cb == 0, "init clears the callback");
        ck(arkrtos_timers[i].arg == 0, "init clears the argument");
        CK_EQ(arkrtos_timers[i].mode, ARKRTOS_TIMER_MODE_PERIODIC,
              "init leaves the documented default mode");
    }
}

/* ------------------------------------------------------------------
 * create：最低空槽、句柄编码、名字截断、表满、槽位复用
 * ------------------------------------------------------------------ */
static void test_create(void)
{
    int32 i;
    int32 h;

    reset_all();

    h = arkrtos_timer_create_internal("t0");
    CK_EQ(h, TH(0), "the first create takes the lowest slot");
    CK_EQ(arkrtos_timers[0].used, 1, "the slot is marked used");
    ck(strcmp(arkrtos_timers[0].name, "t0") == 0, "the name is copied");
    CK_EQ(arkrtos_timers[0].active, 0, "a new timer is not armed");
    ck(arkrtos_timers[0].cb == 0, "a new timer has no callback");
    ck(arkrtos_timers[0].arg == 0, "a new timer has no argument");

    CK_EQ(arkrtos_timer_create_internal("t1"), TH(1),
          "the next create takes the next slot");

    /* name[8]，copy_name 写 7 个字符 + 结尾 0：第 8 个字符必须丢掉。 */
    CK_EQ(arkrtos_timer_create_internal("abcdefghij"), TH(2),
          "a long name still creates");
    ck(strcmp(arkrtos_timers[2].name, "abcdefg") == 0,
       "the name is truncated to 7 characters");
    ck(arkrtos_timers[2].name[7] == 0, "the truncated name is NUL-terminated");

    for(i = 3; i < ARKRTOS_TIMER_NUM; i++)
    {
        CK_EQ(arkrtos_timer_create_internal("x"), TH(i),
              "slots fill in ascending order");
    }
    CK_EQ(arkrtos_timer_create_internal("x"), -1,
          "a full table is refused (NOSLOT)");

    /* 槽位复用：删掉 1 号之后，下一次创建必须回到 1 号。 */
    CK_EQ(arkrtos_timer_delete_internal(TH(1)), 0, "delete a middle slot");
    CK_EQ(arkrtos_timer_create_internal("reuse"), TH(1),
          "a freed slot is reused, not appended");
    ck(strcmp(arkrtos_timers[1].name, "reuse") == 0,
       "the reused slot carries the new name");
}

/* ------------------------------------------------------------------
 * start：五道闸门 + period_ms 到 tick 的换算
 * ------------------------------------------------------------------ */
static void test_start_guards(void)
{
    int32 h;

    reset_all();
    h = arkrtos_timer_create_internal("g");
    CK_EQ(h, TH(0), "one timer to aim at");

    CK_EQ(arkrtos_timer_start_internal(h | WRONG_TYPE, 100u,
                                       ARKRTOS_TIMER_MODE_ONESHOT, cb_dummy, 0),
          -1, "a handle with the wrong type flag is rejected");
    CK_EQ(arkrtos_timer_start_internal(0, 100u,
                                       ARKRTOS_TIMER_MODE_ONESHOT, cb_dummy, 0),
          -1, "a bare index is not a handle");
    CK_EQ(arkrtos_timer_start_internal(TH(ARKRTOS_TIMER_NUM), 100u,
                                       ARKRTOS_TIMER_MODE_ONESHOT, cb_dummy, 0),
          -1, "an out-of-range index is rejected");
    CK_EQ(arkrtos_timer_start_internal(TH(1), 100u,
                                       ARKRTOS_TIMER_MODE_ONESHOT, cb_dummy, 0),
          -1, "an unused slot is rejected (STATE)");
    CK_EQ(arkrtos_timer_start_internal(h, 100u,
                                       ARKRTOS_TIMER_MODE_ONESHOT, 0, 0),
          -1, "a null callback is rejected (PARAM)");
    CK_EQ(arkrtos_timer_start_internal(h, 0u,
                                       ARKRTOS_TIMER_MODE_ONESHOT, cb_dummy, 0),
          -1, "a zero period is rejected (PARAM)");
    CK_EQ(arkrtos_timer_start_internal(h, 100u, 2u, cb_dummy, 0),
          -1, "an unknown mode is rejected (PARAM)");

    /* 合法启动：字段必须逐个对上。 */
    CK_EQ(arkrtos_timer_start_internal(h, 100u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_dummy, &g_token),
          0, "a valid oneshot start succeeds");
    CK_EQ(arkrtos_timers[0].mode, ARKRTOS_TIMER_MODE_ONESHOT, "mode stored");
    CK_EQ(arkrtos_timers[0].period_ticks, ARKRTOS_MS_TO_TICK(100u),
          "period_ms converted to ticks");
    CK_EQ(arkrtos_timers[0].remain_ticks, ARKRTOS_MS_TO_TICK(100u),
          "the countdown starts at the full period");
    CK_EQ(arkrtos_timers[0].active,  1, "armed");
    CK_EQ(arkrtos_timers[0].expired, 0, "not expired on the way in");
    ck(arkrtos_timers[0].cb == cb_dummy, "callback stored");
    ck(arkrtos_timers[0].arg == &g_token, "argument stored");

    /* rest 一次要能把 expired 清掉：先伪造一个到期标记再启动。 */
    arkrtos_timers[0].expired = 1;
    CK_EQ(arkrtos_timer_start_internal(h, 100u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_dummy, 0),
          0, "restart succeeds");
    CK_EQ(arkrtos_timers[0].expired, 0, "a restart clears the stale expiry");

    /* 换算里的 0 拍：0x80000000 ms 让 32 位乘法溢出成 0，必须抬成 1 拍，
     * 否则这个定时器永远不到期（普通档静默，tickless 档每拍都判到期）。 */
    CK_EQ(arkrtos_timer_start_internal(h, 0x80000000u,
                                       ARKRTOS_TIMER_MODE_PERIODIC, cb_dummy, 0),
          0, "an overflowing period is accepted");
    CK_EQ(arkrtos_timers[0].period_ticks, 1,
          "a zero tick count is floored to 1");
    CK_EQ(arkrtos_timers[0].remain_ticks, 1, "and so is the countdown");
}

/* ------------------------------------------------------------------
 * tick：倒计时、到期标记、oneshot 与 periodic 的区别
 * ------------------------------------------------------------------ */
static void test_tick_countdown(void)
{
    int32 h;
    uint32 ticks;

    reset_all();
    h = arkrtos_timer_create_internal("c");

    /* 2 ms = 4 拍：边界要一个一个踩。 */
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_dummy, 0),
          0, "arm a 4-tick oneshot");

    tick_n(3);
    CK_EQ(arkrtos_timers[0].remain_ticks, 1u,
          "three ticks leave one tick to go");
    CK_EQ(arkrtos_timers[0].expired, 0, "not expired one tick early");

    tick_n(1);
    CK_EQ(arkrtos_timers[0].remain_ticks, 0u,
          "the last tick reaches zero");
    CK_EQ(arkrtos_timers[0].expired, 1, "and marks the timer expired");
    CK_EQ(arkrtos_timers[0].active, 1,
          "a oneshot stays active until the service task consumes it");

    /* 已经到期、服务任务还没跑：再来的拍子不能把标记抹掉。 */
    tick_n(2);
    CK_EQ(arkrtos_timers[0].expired, 1,
          "extra ticks keep the pending expiry");

    /* periodic：到期即回填，active 保持。 */
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_dummy, 0),
          0, "arm a 4-tick periodic");
    tick_n(4);
    ticks = ARKRTOS_MS_TO_TICK(2u);
    CK_EQ(arkrtos_timers[0].expired, 1, "the periodic fires");
    CK_EQ(arkrtos_timers[0].remain_ticks, ticks,
          "the periodic reloads the full period");
    CK_EQ(arkrtos_timers[0].active, 1, "the periodic stays armed");

    /* stop 之后 tick 不该再动它。 */
    CK_EQ(arkrtos_timer_stop_internal(h), 0, "stop it");
    arkrtos_timers[0].expired = 0;
    tick_n(10);
    CK_EQ(arkrtos_timers[0].expired, 0, "a stopped timer never expires");

    /* 建了但从没 start：used==1 但 active==0，tick 必须跳过它。 */
    CK_EQ(arkrtos_timer_create_internal("idle"), TH(1),
          "a created-but-never-armed timer");
    arkrtos_timers[1].remain_ticks = 1;
    tick_n(5);
    CK_EQ(arkrtos_timers[1].expired, 0,
          "an inactive timer is not counted down");
    CK_EQ(arkrtos_timers[1].remain_ticks, 1u,
          "and its countdown is left alone");
}

/* ------------------------------------------------------------------
 * install 与 tick 的唤醒交接
 * ------------------------------------------------------------------ */
static void test_install_and_wake(void)
{
    int32 h;

    reset_all();
    g_reg_ret = 3;                       /* 模拟注册成 3 号任务 */
    arkrtos_timer_task_install();
    CK_EQ(g_reg_calls, 1, "install registers exactly one task");
    ck(g_reg_entry == arkrtos_timer_daemon,
       "the registered entry is the timer service loop");
    ck(g_reg_stack != 0, "a static stack is handed over");
    CK_EQ(g_reg_size, ARKRTOS_TIMER_TASK_STACK_WORDS * 4u,
          "the stack size matches the configured word count");
    CK_EQ(g_reg_prio, ARKRTOS_TIMER_TASK_PRI, "the configured priority");
    CK_EQ(g_reg_period, 0u, "the service task is not periodic");

    h = arkrtos_timer_create_internal("w");

    /* 服务任务在 WAIT：到期必须把它交接回就绪队列。 */
    arkrtos_task_table[2].status    = ARKRTOS_TASK_WAIT;
    arkrtos_task_table[2].wait_time = 77;
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_dummy, 0),
          0, "arm a 4-tick oneshot");
    tick_n(4);
    CK_EQ(g_ready_calls, 1, "an expiry wakes the service task exactly once");
    CK_EQ(g_ready_last, 2, "the awakened index is task_id - 1");
    CK_EQ(arkrtos_task_table[2].status, ARKRTOS_TASK_READY,
          "the service task is moved to READY");
    CK_EQ(arkrtos_task_table[2].wake_reason, ARKRTOS_WAKE_HANDOFF,
          "the wake carries the handoff bit");
    CK_EQ(arkrtos_task_table[2].wait_time, 0, "the wait timeout is cleared");

    /* 服务任务不在 WAIT（正在跑 / 还没跑）：不许动它的状态。 */
    arkrtos_task_table[2].status    = ARKRTOS_TASK_READY;
    arkrtos_task_table[2].wait_time = 55;
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_dummy, 0),
          0, "arm again");
    tick_n(4);
    CK_EQ(g_ready_calls, 1, "a READY service task is not re-queued");
    CK_EQ(arkrtos_task_table[2].wait_time, 55,
          "and its wait timeout is left untouched");
    CK_EQ(arkrtos_timers[0].expired, 1,
          "the expiry marker is still set for the next service pass");

    /* 注册失败（返回 -1）：只标到期，不碰任务表。 */
    reset_all();
    g_reg_ret = -1;
    arkrtos_timer_task_install();
    h = arkrtos_timer_create_internal("no-task");
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_dummy, 0),
          0, "arm with no service task");
    arkrtos_task_table[0].status = ARKRTOS_TASK_WAIT;
    tick_n(4);
    CK_EQ(arkrtos_timers[0].expired, 1, "the timer still expires");
    CK_EQ(g_ready_calls, 0, "but nobody is woken without a service task");

    /* 返回 0 也是"没有服务任务"：task_id > 0 才是有效下标，0 会算成 -1。 */
    reset_all();
    g_reg_ret = 0;
    arkrtos_timer_task_install();
    h = arkrtos_timer_create_internal("id0");
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_dummy, 0),
          0, "arm with task id 0");
    tick_n(4);
    CK_EQ(g_ready_calls, 0, "task id 0 is not a usable table index");
}

/* ------------------------------------------------------------------
 * 服务任务：一圈预算里的全部分支
 * ------------------------------------------------------------------ */
static int g_cb_calls = 0;
static void *g_cb_arg = 0;

static void cb_count(void *arg)
{
    g_cb_calls++;
    g_cb_arg = arg;
}

static int32 g_cb_handle = -1;

static void cb_stop_self(void *arg)
{
    (void)arg;
    (void)arkrtos_timer_stop_internal(g_cb_handle);
}

static void cb_delete_self(void *arg)
{
    (void)arg;
    (void)arkrtos_timer_delete_internal(g_cb_handle);
}

static int g_order[4];
static int g_order_n = 0;
static int g_tag[4] = { 0, 1, 2, 3 };

static void cb_order(void *arg)
{
    g_order[g_order_n] = *(int *)arg;
    g_order_n++;
}

static void run_daemon(int budget)
{
    arkrtos_timer_daemon_budget = budget;
    arkrtos_timer_daemon();
}

static void test_daemon(void)
{
    int32 h;

    /* 无工作：睡下去，然后把预算用完退出。 */
    reset_all();
    h = arkrtos_timer_create_internal("d");
    run_daemon(1);
    CK_EQ(g_block_calls, 1, "an idle service pass blocks once");
    CK_EQ(arkrtos_timer_daemon_budget, 0, "one pass consumes the budget");

    /* oneshot 到期：回调拿到快照的 cb/arg，oneshot 自动停。 */
    g_cb_calls = 0;
    g_cb_arg   = 0;
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_count, &g_token),
          0, "arm a oneshot");
    run_daemon(1);
    CK_EQ(g_cb_calls, 0, "nothing fires before the deadline");
    tick_n(4);
    g_block_calls = 0;
    run_daemon(1);
    CK_EQ(g_cb_calls, 1, "the expiry runs the callback once");
    ck(g_cb_arg == &g_token, "the callback sees the stored argument");
    CK_EQ(arkrtos_timers[0].expired, 0, "the expiry marker is consumed");
    CK_EQ(arkrtos_timers[0].active, 0, "a oneshot is disarmed by the service");
    ck(arkrtos_timers[0].cb != 0, "the callback pointer is kept for a restart");
    CK_EQ(g_block_calls, 0, "a pass with work does not block");

    /* periodic：服务一圈之后仍然 armed，下一轮到期还能再跑。 */
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_count, 0),
          0, "arm a periodic");
    tick_n(4);
    run_daemon(1);
    CK_EQ(g_cb_calls, 2, "the periodic fires once per period");
    CK_EQ(arkrtos_timers[0].active, 1, "a periodic stays armed");
    tick_n(4);
    run_daemon(1);
    CK_EQ(g_cb_calls, 3, "and fires again on the next period");

    /* 回调里 stop 自己：快照之后清 active/expired，不得再被 tick 采到。 */
    reset_all();
    h = arkrtos_timer_create_internal("s");
    g_cb_handle = h;
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_stop_self, 0),
          0, "arm a self-stopping timer");
    tick_n(4);
    run_daemon(1);
    CK_EQ(arkrtos_timers[0].active, 0, "the callback's stop disarms it");
    CK_EQ(arkrtos_timers[0].expired, 0, "and leaves no pending expiry");
    tick_n(10);
    CK_EQ(arkrtos_timers[0].expired, 0, "a self-stopped timer stays quiet");

    /* 回调里 delete 自己：槽位必须真的腾出来。 */
    reset_all();
    h = arkrtos_timer_create_internal("k");
    g_cb_handle = h;
    CK_EQ(arkrtos_timer_start_internal(h, 2u, ARKRTOS_TIMER_MODE_ONESHOT,
                                       cb_delete_self, 0),
          0, "arm a self-deleting timer");
    tick_n(4);
    run_daemon(1);
    CK_EQ(arkrtos_timers[0].used, 0, "the callback's delete frees the slot");
    ck(arkrtos_timers[0].cb == 0, "and clears the callback pointer");
    CK_EQ(arkrtos_timer_create_internal("k2"), TH(0),
          "the freed slot is reusable right away");

    /* cb==0（只可能是内部伪造的到期标记）：跳过回调，但标记要清掉。 */
    reset_all();
    CK_EQ(arkrtos_timer_create_internal("n"), TH(0), "one slot");
    arkrtos_timers[0].expired = 1;
    arkrtos_timers[0].cb      = 0;
    run_daemon(1);
    CK_EQ(arkrtos_timers[0].expired, 0,
          "an expiry with no callback is still consumed");
    CK_EQ(arkrtos_timers[0].used, 1, "the timer object itself survives");

    /* 多个到期 + 一个未到期：一圈里按槽位顺序处理，未到期的跳过。 */
    reset_all();
    g_order_n = 0;
    CK_EQ(arkrtos_timer_create_internal("o0"), TH(0), "slot 0");
    CK_EQ(arkrtos_timer_create_internal("o1"), TH(1), "slot 1");
    CK_EQ(arkrtos_timer_create_internal("o2"), TH(2), "slot 2");
    CK_EQ(arkrtos_timer_create_internal("o3"), TH(3), "slot 3");
    arkrtos_timers[0].expired = 1;
    arkrtos_timers[0].cb      = cb_order;
    arkrtos_timers[0].arg     = &g_tag[0];
    arkrtos_timers[2].expired = 1;
    arkrtos_timers[2].cb      = cb_order;
    arkrtos_timers[2].arg     = &g_tag[2];
    arkrtos_timers[1].expired = 0;
    arkrtos_timers[3].expired = 0;
    run_daemon(1);
    CK_EQ(g_order_n, 2, "both pending expiries are served in one pass");
    CK_EQ(g_order[0], 0, "the lowest slot runs first");
    CK_EQ(g_order[1], 2, "then the next expired slot");
    CK_EQ(arkrtos_timers[1].expired, 0, "the unexpired slot stays unexpired");
    CK_EQ(arkrtos_timers[3].expired, 0, "and so does the last one");

    /* 预算用尽后不再往下跑：证明记账的是"圈"而不是"时间"。 */
    reset_all();
    CK_EQ(arkrtos_timer_create_internal("b"), TH(0), "one slot");
    arkrtos_timers[0].expired = 1;
    arkrtos_timers[0].cb      = cb_order;
    arkrtos_timers[0].arg     = &g_tag[0];
    g_order_n = 0;
    run_daemon(0);
    CK_EQ(g_order_n, 0, "a zero budget runs no pass at all");
    CK_EQ(arkrtos_timers[0].expired, 1, "and nothing is consumed");
}

/* ------------------------------------------------------------------
 * stop / delete 的闸门与读数
 * ------------------------------------------------------------------ */
static void test_stop_delete(void)
{
    int32 h;

    reset_all();
    CK_EQ(arkrtos_timer_stop_internal(TH(0)), -1,
          "stopping an unused slot is refused (STATE)");
    h = arkrtos_timer_create_internal("z");
    CK_EQ(arkrtos_timer_stop_internal(h | WRONG_TYPE), -1,
          "stopping a foreign handle is refused (HANDLE)");
    CK_EQ(arkrtos_timer_stop_internal(TH(ARKRTOS_TIMER_NUM)), -1,
          "stopping an out-of-range index is refused");

    CK_EQ(arkrtos_timer_start_internal(h, 10u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_dummy, 0),
          0, "arm a periodic");
    CK_EQ(arkrtos_timer_stop_internal(h), 0, "a valid stop succeeds");
    CK_EQ(arkrtos_timers[0].active, 0, "stop clears active");
    CK_EQ(arkrtos_timers[0].expired, 0, "stop clears expired");
    CK_EQ(arkrtos_timers[0].used, 1, "stop keeps the object");

    CK_EQ(arkrtos_timer_delete_internal(h | WRONG_TYPE), -1,
          "deleting a foreign handle is refused (HANDLE)");
    CK_EQ(arkrtos_timer_delete_internal(TH(ARKRTOS_TIMER_NUM)), -1,
          "deleting an out-of-range index is refused");
    CK_EQ(arkrtos_timer_delete_internal(h), 0, "a valid delete succeeds");
    CK_EQ(arkrtos_timers[0].used, 0, "delete clears used");
    CK_EQ(arkrtos_timers[0].active, 0, "delete clears active");
    ck(arkrtos_timers[0].name[0] == 0, "delete clears the name");
    ck(arkrtos_timers[0].cb == 0, "delete clears the callback");
    ck(arkrtos_timers[0].arg == 0, "delete clears the argument");
    CK_EQ(arkrtos_timer_start_internal(h, 10u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_dummy, 0),
          -1, "a deleted handle cannot be restarted (STATE)");

    /* 读数（未修，已在文档登记）：delete 只查标志位与下标，不查 used，
     * 所以删一个从未用过的槽也返回 0，且再删一次仍然返回 0。 */
    CK_EQ(arkrtos_timer_delete_internal(h), 0,
          "read-out: deleting an unused slot still reports success");
    CK_EQ(arkrtos_timer_delete_internal(TH(3)), 0,
          "read-out: a never-used slot can be 'deleted' as well");
}

#if (ARKRTOS_USE_TICKLESS == 1)
/* ------------------------------------------------------------------
 * tickless：next_ticks 的三条分支
 * ------------------------------------------------------------------ */
static void test_next_ticks(void)
{
    int32 h0;
    int32 h1;

    reset_all();
    CK_EQ(arkrtos_timer_next_ticks(), ARKRTOS_TICKLESS_NO_EVENT,
          "no armed timer means no event");

    h0 = arkrtos_timer_create_internal("n0");
    h1 = arkrtos_timer_create_internal("n1");
    CK_EQ(arkrtos_timer_start_internal(h0, 20u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_dummy, 0),
          0, "arm 40 ticks");
    CK_EQ(arkrtos_timer_start_internal(h1, 100u, ARKRTOS_TIMER_MODE_PERIODIC,
                                       cb_dummy, 0),
          0, "arm 200 ticks");
    CK_EQ(arkrtos_timer_next_ticks(), 40u, "the nearest deadline wins");

    arkrtos_timers[0].remain_ticks = 300u;
    CK_EQ(arkrtos_timer_next_ticks(), 200u,
          "the other timer becomes the nearest one");

    arkrtos_timers[1].remain_ticks = 0u;
    CK_EQ(arkrtos_timer_next_ticks(), 1u,
          "a timer that is due this tick forces a 1-tick sleep");

    CK_EQ(arkrtos_timer_stop_internal(h0), 0, "disarm the first");
    CK_EQ(arkrtos_timer_stop_internal(h1), 0, "disarm the second");
    CK_EQ(arkrtos_timer_next_ticks(), ARKRTOS_TICKLESS_NO_EVENT,
          "disarmed timers are not events");

    /* 建了不 arm 的槽不计入。 */
    CK_EQ(arkrtos_timer_create_internal("idle"), TH(2),
          "an unarmed timer exists");
    arkrtos_timers[2].remain_ticks = 0u;
    CK_EQ(arkrtos_timer_next_ticks(), ARKRTOS_TICKLESS_NO_EVENT,
          "an inactive timer with a zero countdown is still not an event");
}
#endif /* ARKRTOS_USE_TICKLESS == 1 */

#endif /* ARKRTOS_USE_TIMER == 1 */

int main(void)
{
#if (ARKRTOS_USE_TIMER == 1)
    reset_all();

    test_module_init();
    test_create();
    test_start_guards();
    test_tick_countdown();
    test_install_and_wake();
    test_daemon();
    test_stop_delete();
#if (ARKRTOS_USE_TICKLESS == 1)
    test_next_ticks();
#endif

    if(g_fails != 0)
    {
        printf("TIMER-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
#if (ARKRTOS_USE_TICKLESS == 1)
    printf("TIMER-HOST TL OK %d checks\n", g_checks);
#else
    printf("TIMER-HOST OK %d checks\n", g_checks);
#endif
    return 0;
#else
    /* 关档：module_init / task_install / tick_handler 是空函数，其余全部拒绝。
     * 这里证明"不编定时器"不会变成静默成功，也不会碰到任务表。 */
    reset_all();
    arkrtos_timer_module_init();
    arkrtos_timer_task_install();
    arkrtos_timer_tick_handler();
    CK_EQ(g_block_calls, 0, "the disabled tick handler does nothing");
    ck(g_ready_calls == 0, "and wakes nobody");
    ck(arkrtos_task_table[0].status == ARKRTOS_TASK_INVALID,
       "the disabled module leaves the task table alone");
    CK_EQ(g_reg_calls, 0, "the disabled install registers no task");
    CK_EQ(arkrtos_timer_create_internal("x"), -1, "create is refused");
    CK_EQ(arkrtos_timer_start_internal(0, 10u, 0, cb_dummy, 0), -1,
          "start is refused");
    CK_EQ(arkrtos_timer_stop_internal(0), -1, "stop is refused");
    CK_EQ(arkrtos_timer_delete_internal(0), -1, "delete is refused");

    if(g_fails != 0)
    {
        printf("TIMER-HOST OFF FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("TIMER-HOST OFF OK %d checks\n", g_checks);
    return 0;
#endif
}
