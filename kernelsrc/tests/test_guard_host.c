/**
* @file test_guard_host.c
* @brief 看门狗门控与心跳契约（arkrtos_guard.c）的宿主用例
* @details 被测对象只做一件事：判断"系统还在按承诺推进吗，如果不，该让硬件
*          结束这次运行吗"。它不碰硬件寄存器——喂狗与装狗都走 port，所以宿主
*          用例把 port 与几个邻居（分区表、自检、崩溃记账、故障环、日志、
*          系统节拍）桩掉，钉住决策本身：
*
*            1. 契约只在**服务拍**（now % SERVICE_TICKS == 0）判定，其它拍
*               提前返回，既不判也不喂；
*            2. 违约只看"距上次报到的节拍龄"，不看任务此刻在做什么——阻塞
*               超过声明周期同样是破约，否则从不醒来的镜像永远抓不到；
*            3. 违约第一次发生时记一次账（每开机一次），之后只累加 starves，
*               不再写崩溃记录——否则一次破约会被记成几百条；
*            4. 只有 verdict 干净才喂狗；enabled / 装狗失败两种情况下不喂，
*               但仍然**照常判定**并在状态里如实报告；
*            5. 契约跟着任务走：任务没了，契约立刻作废，且不能算到接手的
*               下一个任务头上。
*
*          三个档：USE_KERNEL_GUARD=1 + WDG_ENABLE=1（完整）、
*          USE_KERNEL_GUARD=1 + WDG_ENABLE=0（板子没要求看门狗：仍判定、
*          不喂）、USE_KERNEL_GUARD=0（模块整段编掉：明确回答"没有监督"，
*          绝不给一个看着像成功的 0）。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_guard.h"
#include "arkrtos_task.h"
#include "arkrtos_cfg.h"
#include "arkrtos_ptable.h"
#include "arkrtos_audit.h"
#include "arkrtos_crash.h"
#include "arkrtos_fault.h"
#include "arkrtos_log.h"
#include "arkrtos_hal.h"
#include "arkrtos_features.h"

#ifndef ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           (0)
#endif
/* 与 arkrtos_guard.c 相同的 #ifndef 默认值：-D 覆盖时两边一起变。 */
#ifndef ARKRTOS_WDG_TIMEOUT_MS
#define ARKRTOS_WDG_TIMEOUT_MS       (4000u)
#endif

/* 与 arkrtos_guard.c 内部一致的派生量（本文件只做算术，不改语义）：
 *   TICKS_PER_MS  = 1000 / TICK_PERIOD_US
 *   SERVICE_TICKS = GUARD_MIN_PERIOD_MS * TICKS_PER_MS
 * 审计细分 AUDIT_DIV 是 .c 内部的实现常量（10），这里按契约钉住它的节拍。 */
#define T_TICKS_PER_MS   (1000u / ARKRTOS_TICK_PERIOD_US)
#define T_SERVICE_TICKS  (ARKRTOS_GUARD_MIN_PERIOD_MS * T_TICKS_PER_MS)
#define T_AUDIT_DIV      (10u)
#define T_AGE_NONE       (0xFFFFFFFFu)

#if (ARKRTOS_USE_KERNEL_GUARD == 1)

#if (ARKRTOS_WDG_ENABLE == 1)

/* ---------------- 断言 ---------------- */

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

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %lu want %lu (line %d)\n", what, got, want, __LINE__);
    }
}

/* ---------------- 桩：被守卫调用的邻居 ---------------- */

static uint32 g_tick;

static int    g_wdg_init_calls;
static int    g_wdg_init_ret;
static uint32 g_wdg_init_arg;
static uint32 g_wdg_timeout;
static int    g_wdg_feed_calls;

static int    g_audit_calls;
static uint32 g_audit_mask;

static int32  g_find_ret;
static int    g_find_calls;

static uint32 g_crash_fault_ret;
static int    g_crash_fault_calls;
static uint32 g_crash_fault_reason;

/* arkrtos_crash_disabled() 在记账前后各被问一次：第一次"还没被扣留"，
 * 第二次可以"刚被这次记账扣留"，从而走到"slot held"那条日志。 */
static uint32 g_disabled_vals[2];
static int    g_disabled_calls;

static int    g_fault_record_calls;
static uint32 g_fault_record_type;

static int    g_log_lines;

uint32 arkrtos_kernel_get_tick(void)
{
    return g_tick;
}

int32 arkrtos_port_wdg_init(uint32 timeout_ms)
{
    g_wdg_init_calls++;
    g_wdg_init_arg = timeout_ms;
    return g_wdg_init_ret;
}

uint32 arkrtos_port_wdg_timeout_ms(void)
{
    return g_wdg_timeout;
}

void arkrtos_port_wdg_feed(void)
{
    g_wdg_feed_calls++;
}

void arkrtos_audit_run(arkrtos_audit_result_t *p_out)
{
    g_audit_calls++;
    p_out->mask   = g_audit_mask;
    p_out->index  = ARKRTOS_AUDIT_INDEX_NONE;
    p_out->checks = 1u;
}

int32 arkrtos_ptable_find_task(uint32 task_id)
{
    (void)task_id;
    g_find_calls++;
    return g_find_ret;
}

uint32 arkrtos_crash_fault(uint32 slot, uint32 reason)
{
    (void)slot;
    g_crash_fault_calls++;
    g_crash_fault_reason = reason;
    return g_crash_fault_ret;
}

uint32 arkrtos_crash_disabled(uint32 slot)
{
    uint32 v;

    (void)slot;
    v = (g_disabled_calls < 1) ? g_disabled_vals[0] : g_disabled_vals[1];
    g_disabled_calls++;
    return v;
}

const char *arkrtos_crash_reason_name(uint32 reason)
{
    (void)reason;
    return "heartbeat";
}

void arkrtos_fault_record(uint32 type, int32 task_id)
{
    (void)task_id;
    g_fault_record_calls++;
    g_fault_record_type = type;
}

uint32 arkrtos_log_get_level(void)
{
    return (uint32)ARKRTOS_LOG_INFO;    /* 让 LOGE/LOGI 真的走一遍格式化与出口 */
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level;
    (void)tag;
    (void)line;
    (void)msg;
    g_log_lines++;
}

/* ---------------- 任务表（守卫按 id 直接索引） ---------------- */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count;
int32          arkrtos_current_task_id;

/* ---------------- 夹具 ---------------- */

static void reset_all(void)
{
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));  /* status 0 = INVALID */
    arkrtos_task_count      = 0;
    arkrtos_current_task_id = 0;

    g_tick                = 0u;
    g_wdg_init_calls      = 0;
    g_wdg_init_ret        = 0;      /* 默认：装狗成功 */
    g_wdg_init_arg        = 0u;
    g_wdg_timeout         = 4000u;
    g_wdg_feed_calls      = 0;
    g_audit_calls         = 0;
    g_audit_mask          = 0u;
    g_find_ret            = -1;
    g_find_calls          = 0;
    g_crash_fault_ret     = 1u;
    g_crash_fault_calls   = 0;
    g_crash_fault_reason  = 0u;
    g_disabled_vals[0]    = 0u;
    g_disabled_vals[1]    = 0u;
    g_disabled_calls      = 0;
    g_fault_record_calls  = 0;
    g_fault_record_type   = 0u;
    g_log_lines           = 0;

    arkrtos_guard_init();
}

static arkrtos_guard_status_t status_now(void)
{
    arkrtos_guard_status_t s;

    memset(&s, 0x5A, sizeof(s));
    arkrtos_guard_status(&s);
    return s;
}

static arkrtos_guard_task_t task_at(uint32 idx)
{
    arkrtos_guard_task_t e;

    memset(&e, 0x5A, sizeof(e));
    (void)arkrtos_guard_task_at(idx, &e);
    return e;
}

/* 让一个任务存在（id 是 1 起）。 */
static void make_task(int32 id, arkrtos_task_status_t st)
{
    if(id > arkrtos_task_count)
    {
        arkrtos_task_count = id;
    }
    arkrtos_task_table[id - 1].status = st;
}

/* ================= 用例 ================= */

/* 初始化必须把表、状态、审计细分全部归零；装狗成功时 timeout 来自 port。 */
static void test_init_clears(void)
{
    arkrtos_guard_status_t s;
    arkrtos_guard_task_t e;

    reset_all();

    ck_u((unsigned long)g_wdg_init_calls, 1u, "init calls wdg_init once");
    ck_u((unsigned long)g_wdg_init_arg, (unsigned long)ARKRTOS_WDG_TIMEOUT_MS,
         "init asks the port for the configured timeout");

    s = status_now();
    ck_u(s.enabled, 1u, "enabled mirrors the board switch");
    ck_u(s.timeout_ms, 4000u, "armed timeout is read back from the port");
    ck_u(s.feeds, 0u, "no feed at init");
    ck_u(s.starves, 0u, "no starvation at init");
    ck_u(s.verdict, ARKRTOS_GUARD_OK, "verdict starts clean");
    ck_u(s.audit_mask, 0u, "audit mask starts zero");
    ck_u(s.audit_runs, 0u, "audit runs starts zero");
    ck_u(s.violating, 0u, "no violator at init");
    ck_u(s.declared, 0u, "no declared contract at init");

    e = task_at(0u);
    ck_u((unsigned long)e.task_id, 0u, "cleared entry reports no task");
    ck_u((unsigned long)e.slot, (unsigned long)-1, "cleared entry has no slot");
    ck_u(e.declared, 0u, "cleared entry declares nothing");
    ck_u(e.period_ms, 0u, "cleared entry has no period");
    ck_u(e.state, 0xFFFFFFFFu, "cleared entry has no TCB state");
    ck_u(e.beat_age_ms, T_AGE_NONE, "cleared beat age is 'never'");
    ck_u(e.svc_age_ms, T_AGE_NONE, "cleared svc age is 'never'");
    ck_u(e.cpu_age_ms, T_AGE_NONE, "cleared cpu age is 'never'");
    ck_u(e.misses, 0u, "cleared entry has no misses");
}

/* 装狗被 port 拒绝：enabled 仍是 1，但 timeout 必须是 0，且永远不喂。 */
static void test_init_refused_still_judges(void)
{
    arkrtos_guard_status_t s;
    int log_before;

    reset_all();
    g_wdg_init_ret = -1;
    log_before = g_log_lines;

    arkrtos_guard_init();

    s = status_now();
    ck_u(s.enabled, 1u, "refused init keeps the board switch visible");
    ck_u(s.timeout_ms, 0u, "refused init reports timeout 0");
    ck(g_log_lines > log_before, "refused init says so in the log");

    g_tick = T_SERVICE_TICKS;
    arkrtos_guard_tick();

    ck_u((unsigned long)g_wdg_feed_calls, 0u, "nothing to feed when not armed");
    ck_u(status_now().feeds, 0u, "feed counter stays 0 when not armed");
    ck_u(status_now().starves, 0u, "not armed is not starvation");
}

/* 免费证据：svc 记到对应条目；越界 id 一律忽略。 */
static void test_svc_evidence(void)
{
    arkrtos_guard_task_t e;

    reset_all();
    make_task(3, ARKRTOS_TASK_READY);

    g_tick = 10u;
    arkrtos_guard_svc(0);
    arkrtos_guard_svc(-5);
    arkrtos_guard_svc((int32)(ARKRTOS_TASK_MAX_NUM + 1));

    ck_u(task_at(0u).svc_age_ms, T_AGE_NONE, "idle/kernel svc records nothing");
    ck_u(task_at(2u).svc_age_ms, T_AGE_NONE, "out-of-range svc records nothing");

    arkrtos_guard_svc(3);

    e = task_at(2u);
    ck_u(e.svc_age_ms, 0u, "svc age is 0 on the same tick");
    ck_u(e.declared, 0u, "svc evidence is not a contract");

    g_tick = 10u + (7u * T_TICKS_PER_MS);
    ck_u(task_at(2u).svc_age_ms, 7u, "svc age converts ticks to ms");
}

/* 报到的参数闸门与声明/撤销的记账。 */
static void test_beat_contract(void)
{
    arkrtos_guard_task_t e;

    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    g_tick = 100u;

    ck_u((unsigned long)arkrtos_guard_beat(0, 100u),
         (unsigned long)ARKRTOS_GUARD_ERR_PARAM, "id 0 refused");
    ck_u((unsigned long)arkrtos_guard_beat(-2, 100u),
         (unsigned long)ARKRTOS_GUARD_ERR_PARAM, "negative id refused");
    ck_u((unsigned long)arkrtos_guard_beat((int32)(ARKRTOS_TASK_MAX_NUM + 1), 100u),
         (unsigned long)ARKRTOS_GUARD_ERR_PARAM, "id past the table refused");
    ck_u((unsigned long)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS - 1u),
         (unsigned long)ARKRTOS_GUARD_ERR_PARAM, "period below the service window refused");
    ck_u(status_now().declared, 0u, "refusals declare nothing");
    ck_u(task_at(0u).period_ms, 0u, "refusals leave the entry untouched");

    ck_u((unsigned long)arkrtos_guard_beat(1, 0u), 0u,
         "withdrawing an undeclared contract is not an error");
    ck_u(status_now().declared, 0u, "withdraw on undeclared does not go negative");

    ck_u((unsigned long)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS), 0u,
         "period at the window is accepted");
    ck_u(status_now().declared, 1u, "accepting declares exactly one");

    e = task_at(0u);
    ck_u(e.declared, 1u, "entry reports declared");
    ck_u(e.period_ms, ARKRTOS_GUARD_MIN_PERIOD_MS, "entry keeps the period");
    ck_u(e.beat_age_ms, 0u, "declaring also counts as a beat");
    ck_u((unsigned long)(uint32)e.task_id, 1u, "entry mirrors the task id");

    ck_u((unsigned long)arkrtos_guard_beat(1, 2u * ARKRTOS_GUARD_MIN_PERIOD_MS), 0u,
         "re-declaring a new period is accepted");
    ck_u(status_now().declared, 1u, "re-declaring does not double count");

    ck_u((unsigned long)arkrtos_guard_beat(1, 0u), 0u, "withdraw accepted");
    ck_u(status_now().declared, 0u, "withdraw drops the count");

    e = task_at(0u);
    ck_u(e.declared, 0u, "entry no longer declared");
    ck_u(e.period_ms, 0u, "period cleared by withdraw");
    ck_u(e.beat_age_ms, 0u, "withdraw keeps the last beat stamp (age still readable)");

    /* 一个任务只能算一次；两个任务算两次。 */
    g_tick = 200u;
    (void)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS);
    make_task(2, ARKRTOS_TASK_READY);
    (void)arkrtos_guard_beat(2, ARKRTOS_GUARD_MIN_PERIOD_MS);
    ck_u(status_now().declared, 2u, "two tasks are two contracts");
}

/* 服务拍闸门：非服务拍什么都不做；服务拍判契约、按 10 拍细分跑审计、干净才喂。 */
static void test_tick_gate_and_feed(void)
{
    arkrtos_guard_status_t s;
    uint32 i;

    reset_all();
    make_task(1, ARKRTOS_TASK_READY);

    g_tick = T_SERVICE_TICKS - 1u;
    arkrtos_guard_tick();
    ck_u((unsigned long)g_audit_calls, 0u, "off-service tick does not audit");
    ck_u((unsigned long)g_wdg_feed_calls, 0u, "off-service tick does not feed");
    ck_u(status_now().violating, 0u, "off-service tick records no violator");

    g_tick = T_SERVICE_TICKS;
    arkrtos_guard_tick();
    s = status_now();
    ck_u((unsigned long)g_audit_calls, 0u, "audit waits for its slower divider");
    ck_u((unsigned long)g_wdg_feed_calls, 1u, "clean verdict feeds the watchdog");
    ck_u(s.feeds, 1u, "feed counter follows");
    ck_u(s.verdict, ARKRTOS_GUARD_OK, "verdict clean with nothing declared");
    ck_u(s.audit_runs, 0u, "audit has not run yet");

    /* 再走到第 T_AUDIT_DIV 个服务拍，审计才第一次跑。 */
    for(i = 2u; i <= T_AUDIT_DIV; i++)
    {
        g_tick = T_SERVICE_TICKS * i;
        arkrtos_guard_tick();
    }

    s = status_now();
    ck_u((unsigned long)g_audit_calls, 1u, "audit runs once per AUDIT_DIV service periods");
    ck_u(s.audit_runs, 1u, "audit run count follows");
    ck_u(s.audit_mask, 0u, "clean audit leaves the mask zero");
    ck_u(s.feeds, (unsigned long)T_AUDIT_DIV, "every clean service period feeds");
}

/* 谁在 CPU 上跑过，只是一个只读证据列。 */
static void test_cpu_age_evidence(void)
{
    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    make_task(2, ARKRTOS_TASK_READY);
    arkrtos_current_task_id = 2;

    g_tick = T_SERVICE_TICKS;
    arkrtos_guard_tick();

    ck_u(task_at(1u).cpu_age_ms, 0u, "running task gets cpu age 0");
    ck_u(task_at(0u).cpu_age_ms, T_AGE_NONE, "idle never appears as a contract holder");
    ck_u(task_at(1u).declared, 0u, "cpu evidence is not a contract");

    arkrtos_current_task_id = 0;
    g_tick = T_SERVICE_TICKS * 2u;
    arkrtos_guard_tick();
    ck_u(task_at(1u).cpu_age_ms, ARKRTOS_GUARD_MIN_PERIOD_MS,
         "cpu age converts ticks to ms");
}

/* 违约：契约只看节拍龄；第一个坏拍记账一次，之后只累加 starves。 */
static void test_violation_and_starve_latch(void)
{
    arkrtos_guard_status_t s;

    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    g_find_ret           = 0;      /* 任务 1 属于槽 0 */
    g_crash_fault_ret    = 4u;

    /* 在节拍 0 声明：beat_tick == 0 在"年龄"列上等于"从未报到"，
     * 但违约判定用的是 (now - 0)，所以契约照样从 0 起算。 */
    (void)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS);
    ck_u(task_at(0u).beat_age_ms, T_AGE_NONE,
         "beat at tick 0 reads as 'never' in the age column");

    /* 恰好等于窗口：还没破约。 */
    g_tick = ARKRTOS_GUARD_MIN_PERIOD_MS * T_TICKS_PER_MS;
    arkrtos_guard_tick();
    s = status_now();
    ck_u(s.violating, 0u, "age equal to the window is on time");
    ck_u((unsigned long)g_wdg_feed_calls, 1u, "on-time contract still feeds");

    /* 超出窗口：破约。 */
    g_tick = 2u * T_SERVICE_TICKS;      /* 400 ticks，服务拍；> 200 窗口 */
    arkrtos_guard_tick();
    s = status_now();
    ck_u(s.violating, 1u, "one task in violation");
    ck_u(s.verdict, ARKRTOS_GUARD_BAD_BEAT, "verdict carries BAD_BEAT");
    ck_u(s.starves, 1u, "starvation is counted");
    ck_u((unsigned long)g_wdg_feed_calls, 1u, "a bad verdict never feeds");
    ck_u((unsigned long)g_crash_fault_calls, 1u, "first violation charges the slot once");
    ck_u(g_crash_fault_reason, ARKRTOS_CRASH_REASON_HEARTBEAT,
         "the charge names the heartbeat reason");
    ck_u((unsigned long)g_fault_record_calls, 1u, "GUARDHB fault recorded once");
    ck_u(g_fault_record_type, (uint32)ARKRTOS_FAULT_GUARDHB,
         "the fault ring gets GUARDHB");
    ck(task_at(0u).misses == 1u, "entry counts the missed window");

    /* 下一个坏拍：starves 继续涨，但不再记账（每开机一次）。 */
    g_tick = 3u * T_SERVICE_TICKS;
    arkrtos_guard_tick();
    s = status_now();
    ck_u(s.starves, 2u, "starvation counter keeps climbing");
    ck_u((unsigned long)g_crash_fault_calls, 1u, "no second charge in the same boot");
    ck_u((unsigned long)g_fault_record_calls, 1u, "no second fault record in the same boot");
    ck(task_at(0u).misses == 2u, "every judged bad window adds a miss");

    /* 撤销契约后恢复判定为干净，喂狗恢复，但 starves 不回退（本开机事实）。 */
    (void)arkrtos_guard_beat(1, 0u);
    g_tick = 4u * T_SERVICE_TICKS;
    arkrtos_guard_tick();
    s = status_now();
    ck_u(s.violating, 0u, "withdrawn contract is judged clean again");
    ck_u((unsigned long)g_wdg_feed_calls, 2u, "feeding resumes");
    ck_u(s.starves, 2u, "starvation count does not roll back");
}

/* 任务没了，契约跟着走：不能被记到接手这个 id 的下一个任务头上。 */
static void test_contract_follows_task(void)
{
    arkrtos_guard_status_t s;

    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    make_task(2, ARKRTOS_TASK_READY);
    g_find_ret = 0;

    (void)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS);
    (void)arkrtos_guard_beat(2, ARKRTOS_GUARD_MIN_PERIOD_MS);
    ck_u(status_now().declared, 2u, "two contracts declared");

    /* 任务 2 结束：task_count 缩回 1。 */
    arkrtos_task_count = 1;
    g_tick = 2u * T_SERVICE_TICKS;
    arkrtos_guard_tick();

    s = status_now();
    ck_u(s.declared, 1u, "the dead task's contract is dropped");
    ck_u(s.violating, 1u, "only the surviving task can violate");
    ck_u(task_at(1u).period_ms, 0u, "dead task entry no longer declares");
    ck_u(task_at(0u).misses, 1u, "survivor still misses");

    /* 另一种消亡形态：槽还在，但任务被标成 INVALID。 */
    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    make_task(2, ARKRTOS_TASK_INVALID);
    (void)arkrtos_guard_beat(2, ARKRTOS_GUARD_MIN_PERIOD_MS);

    g_tick = T_SERVICE_TICKS;
    arkrtos_guard_tick();

    s = status_now();
    ck_u(s.declared, 0u, "INVALID task loses its contract immediately");
    ck_u(s.violating, 0u, "an INVALID task is not a violator");
    ck_u(task_at(1u).period_ms, 0u, "INVALID task entry cleared");
}

/* 记账只在有槽、且槽没被扣留时才发生；但 GUARDHB 故障记录与槽无关。 */
static void test_starve_charge_gates(void)
{
    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    (void)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS);

    /* 没有槽（不是镜像任务）：不记崩溃，但仍然记一条 GUARDHB 故障。 */
    g_find_ret = -1;
    g_tick = 2u * T_SERVICE_TICKS;
    arkrtos_guard_tick();

    ck_u((unsigned long)g_crash_fault_calls, 0u, "no slot means no crash charge");
    ck_u((unsigned long)g_fault_record_calls, 1u, "heartbeat fault still recorded");
    ck_u(g_fault_record_type, (uint32)ARKRTOS_FAULT_GUARDHB, "record type is GUARDHB");

    /* 有槽但已被扣留：同样不再加数。 */
    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    (void)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS);
    g_find_ret        = 0;
    g_disabled_vals[0] = 1u;
    g_disabled_vals[1] = 1u;
    g_tick = 2u * T_SERVICE_TICKS;
    arkrtos_guard_tick();

    ck_u((unsigned long)g_crash_fault_calls, 0u, "an already held slot is not charged again");
    ck_u((unsigned long)g_fault_record_calls, 1u, "fault ring still sees the heartbeat");

    /* 第三种形态：记账之前没被扣留，记账把这一笔数推过上限，于是记账之后
     * 读到"已扣留"——这时必须把槽号、原因、连续次数、声明窗口与实际静默
     * 时长一起写进日志，光说"坏了"没法追。 */
    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    (void)arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS);
    g_find_ret         = 0;
    g_disabled_vals[0] = 0u;
    g_disabled_vals[1] = 1u;
    g_crash_fault_ret  = 3u;
    g_tick = 2u * T_SERVICE_TICKS;
    arkrtos_guard_tick();

    ck_u((unsigned long)g_crash_fault_calls, 1u, "the charge is issued once");
    ck_u((unsigned long)g_fault_record_calls, 1u, "heartbeat fault recorded");
    ck(g_log_lines >= 2, "the newly held slot is reported, not silently dropped");
}

/* 结构自检坏了：判 BAD_AUDIT、不喂、记一次 AUDITFAIL；与心跳违约分开记。 */
static void test_bad_audit(void)
{
    arkrtos_guard_status_t s;
    uint32 i;

    reset_all();
    g_audit_mask = 0x1u;

    for(i = 1u; i < T_AUDIT_DIV; i++)
    {
        g_tick = T_SERVICE_TICKS * i;
        arkrtos_guard_tick();
    }

    s = status_now();
    ck_u(s.audit_runs, 0u, "audit has not run before its divider");
    ck_u(s.verdict, ARKRTOS_GUARD_OK, "verdict cannot know about an audit not yet run");
    ck_u((unsigned long)g_wdg_feed_calls, (unsigned long)(T_AUDIT_DIV - 1u),
         "clean periods keep feeding until the audit fires");

    g_tick = T_SERVICE_TICKS * T_AUDIT_DIV;
    arkrtos_guard_tick();

    s = status_now();
    ck_u(s.audit_runs, 1u, "audit fires on the section period");
    ck_u(s.audit_mask, 0x1u, "mask is copied from the audit result");
    ck_u(s.verdict, ARKRTOS_GUARD_BAD_AUDIT, "verdict carries BAD_AUDIT");
    ck_u(s.violating, 0u, "no heartbeat contract is broken");
    ck_u(s.starves, 1u, "a bad verdict starves the watchdog");
    ck_u((unsigned long)g_wdg_feed_calls, (unsigned long)(T_AUDIT_DIV - 1u),
         "the bad verdict itself never feeds");
    ck_u((unsigned long)g_fault_record_calls, 1u, "AUDITFAIL recorded once");
    ck_u(g_fault_record_type, (uint32)ARKRTOS_FAULT_AUDITFAIL, "record type is AUDITFAIL");

    g_tick = T_SERVICE_TICKS * (T_AUDIT_DIV * 2u);
    arkrtos_guard_tick();

    ck_u(status_now().starves, 2u, "starvation keeps climbing");
    ck_u((unsigned long)g_fault_record_calls, 1u, "but the record is still once per boot");
}

/* 状态与表遍历的边界。 */
static void test_report_boundaries(void)
{
    arkrtos_guard_status_t s;
    arkrtos_guard_task_t e;

    reset_all();
    make_task(1, ARKRTOS_TASK_READY);
    g_find_ret = 2;

    arkrtos_guard_status(0);
    ck(1, "null status output is tolerated");

    ck_u((unsigned long)arkrtos_guard_task_at(0u, 0),
         (unsigned long)ARKRTOS_GUARD_ERR_PARAM, "null entry output refused");
    ck_u((unsigned long)arkrtos_guard_task_at((uint32)ARKRTOS_TASK_MAX_NUM, &e),
         (unsigned long)ARKRTOS_GUARD_ERR_PARAM, "index at the table end refused");

    ck_u((unsigned long)arkrtos_guard_task_slots(), (unsigned long)ARKRTOS_TASK_MAX_NUM,
         "slot count is the table capacity");

    ck_u((unsigned long)arkrtos_guard_task_at(0u, &e), 0u, "valid index accepted");
    ck_u((unsigned long)e.slot, 2u, "owning slot comes from the partition table");
    ck_u(e.state, (uint32)ARKRTOS_TASK_READY, "state mirrors the TCB");

    /* 表里没有这个任务：task_id 报 0、state 报未知、不查分区表。 */
    g_find_calls = 0;
    ck_u((unsigned long)arkrtos_guard_task_at(5u, &e), 0u, "empty slot still readable");
    ck_u((unsigned long)e.task_id, 0u, "empty slot reports no task");
    ck_u(e.state, 0xFFFFFFFFu, "empty slot reports unknown state");
    ck_u((unsigned long)e.slot, (unsigned long)-1, "empty slot has no slot");
    ck_u((unsigned long)g_find_calls, 0u, "no partition lookup for a non-task");

    s = status_now();
    ck_u(s.enabled, 1u, "status reflects the board switch");
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);

    test_init_clears();
    test_init_refused_still_judges();
    test_svc_evidence();
    test_beat_contract();
    test_tick_gate_and_feed();
    test_cpu_age_evidence();
    test_violation_and_starve_latch();
    test_contract_follows_task();
    test_starve_charge_gates();
    test_bad_audit();
    test_report_boundaries();

    if(g_fails != 0)
    {
        printf("GUARD-HOST FAILED  %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("GUARD-HOST OK  %d checks\n", g_checks);
    return 0;
}

#else   /* ARKRTOS_USE_KERNEL_GUARD == 1 and ARKRTOS_WDG_ENABLE == 0 */

/* 板子没要求看门狗：契约与自检照常判定、状态如实报告，但一次也不喂。
 * 这一段存在的意义就是把"enabled == 0 就什么都不做"这个常见误读钉死。 */

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

static uint32 g_tick;
static int    g_wdg_init_calls;
static int    g_wdg_feed_calls;
static int    g_audit_calls;
static int32  g_find_ret;

uint32 arkrtos_kernel_get_tick(void) { return g_tick; }
int32  arkrtos_port_wdg_init(uint32 ms) { (void)ms; g_wdg_init_calls++; return 0; }
uint32 arkrtos_port_wdg_timeout_ms(void) { return 4000u; }
void   arkrtos_port_wdg_feed(void) { g_wdg_feed_calls++; }
void   arkrtos_audit_run(arkrtos_audit_result_t *p_out)
{
    g_audit_calls++;
    p_out->mask = 0u;
    p_out->index = ARKRTOS_AUDIT_INDEX_NONE;
    p_out->checks = 1u;
}
int32  arkrtos_ptable_find_task(uint32 id) { (void)id; return g_find_ret; }
uint32 arkrtos_crash_fault(uint32 slot, uint32 reason)
{
    (void)slot; (void)reason; return 1u;
}
uint32 arkrtos_crash_disabled(uint32 slot) { (void)slot; return 0u; }
const char *arkrtos_crash_reason_name(uint32 reason) { (void)reason; return "heartbeat"; }
void   arkrtos_fault_record(uint32 type, int32 task_id) { (void)type; (void)task_id; }
uint32 arkrtos_log_get_level(void) { return (uint32)ARKRTOS_LOG_NONE; }
void   arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level; (void)tag; (void)line; (void)msg;
}

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count;
int32          arkrtos_current_task_id;

int main(void)
{
    arkrtos_guard_status_t s;
    arkrtos_guard_task_t e;

    setvbuf(stdout, 0, _IONBF, 0);

    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_count = 1;
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;

    arkrtos_guard_init();
    ck(g_wdg_init_calls == 0, "no watchdog is armed when the board asked for none");

    arkrtos_guard_status(&s);
    ck(s.enabled == 0u, "status reports the board switch as off");
    ck(s.timeout_ms == 0u, "no hardware timeout is claimed");

    ck(arkrtos_guard_beat(1, ARKRTOS_GUARD_MIN_PERIOD_MS) == 0, "contract still accepted");

    /* 违约一拍：判定必须照常发生，只是不喂。 */
    g_tick = 2u * T_SERVICE_TICKS;
    arkrtos_guard_tick();

    arkrtos_guard_status(&s);
    ck(s.violating == 1u, "violation is still judged without a watchdog");
    ck((s.verdict & ARKRTOS_GUARD_BAD_BEAT) != 0u, "verdict still carries BAD_BEAT");
    ck(s.starves == 0u, "not feeding is not counted as starvation");
    ck(g_wdg_feed_calls == 0, "never feeds when disabled");
    ck(g_audit_calls == 0, "audit cadence is untouched by the watchdog switch");

    (void)arkrtos_guard_task_at(0u, &e);
    ck(e.declared == 1u, "the contract is still visible in the table");

    if(g_fails != 0)
    {
        printf("GUARD-HOST NOWDG FAILED  %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("GUARD-HOST NOWDG OK  %d checks\n", g_checks);
    return 0;
}

#endif  /* ARKRTOS_WDG_ENABLE == 1 */

#else   /* ARKRTOS_USE_KERNEL_GUARD == 0 */

/* 模块整段编掉：没有监督就必须说"没有"，不能给一个看着像成功的 0。 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

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

int main(void)
{
    arkrtos_guard_status_t s;
    arkrtos_guard_task_t e;

    setvbuf(stdout, 0, _IONBF, 0);

    arkrtos_guard_init();
    arkrtos_guard_tick();
    arkrtos_guard_svc(1);
    arkrtos_guard_svc(0);

    ck(arkrtos_guard_beat(1, 100u) == ARKRTOS_GUARD_ERR_DISABLED,
       "an explicit beat is refused, not silently accepted");
    ck(arkrtos_guard_beat(1, 0u) == ARKRTOS_GUARD_ERR_DISABLED,
       "withdraw is refused too");

    memset(&s, 0x5A, sizeof(s));
    arkrtos_guard_status(&s);
    ck(s.enabled == 0u, "status says disabled");
    ck(s.feeds == 0u, "status says nothing was fed");
    ck(s.starves == 0u, "status says nothing starved");
    ck(s.verdict == ARKRTOS_GUARD_OK, "status reports no known problem");

    arkrtos_guard_status(0);
    ck(1, "null status output tolerated");

    memset(&e, 0x5A, sizeof(e));
    ck(arkrtos_guard_task_at(0u, &e) == ARKRTOS_GUARD_ERR_DISABLED,
       "table walk says disabled rather than pretending an empty table");
    ck(arkrtos_guard_task_at(0u, 0) == ARKRTOS_GUARD_ERR_DISABLED,
       "null output refused before the index is examined");

    ck(arkrtos_guard_task_slots() == 0u, "no slots when the module is compiled out");

    if(g_fails != 0)
    {
        printf("GUARD-HOST OFF FAILED  %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("GUARD-HOST OFF OK  %d checks\n", g_checks);
    return 0;
}

#endif
