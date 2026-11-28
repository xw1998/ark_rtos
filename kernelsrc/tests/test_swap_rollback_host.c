/**
* @file test_swap_rollback_host.c
* @brief 热切换失败自动回滚（arkrtos_swap_rollback.c）的宿主用例
* @details 被测对象只做一件事：一次热切换落地后，判断新镜像是否守住了它
*          自己承诺的心跳周期；没守住就把旧槽重新拉起来。它不碰调度器、
*          不碰文件系统——判定在 tick 里只置标志，动作在 SVC 上下文里做，
*          所以宿主用例只需三样邻居：心跳契约读数（guard_probe/guard_rebase）、
*          槽位停/启（loader_stop/loader_start）、日志出口。
*
*          覆盖范围（如实说明）：
*            已验证：无契约不布防（probe 失败 / declared==0 / period==0 三种
*                    都拒绝并记账）、参数拒绝（task<=0、新旧槽同号）、正在
*                    布防时再布防被拒、窗口 = 三倍周期（含最小拍下限）、
*                    ARMED 到期转 CONFIRMED、ARMED 里 misses 增长转 FAILED、
*                    任务消失（probe 失败 / INVALID / 0xFFFFFFFF 三路）转
*                    FAILED(DEAD)、契约中途撤销转 CONFIRMED(WITHDRAWN)、
*                    service() 停新槽再启旧槽（成功 / 停失败仍继续 / 启失败
*                    转 GAVE_UP 且不重试）、cancel 的三种入口、status 快照与
*                    空指针拒绝、pending() 只在 FAILED 时为 1。
*            未验证：真在 tick 里被调用、真在 SVC 里被调用、真槽位停/启
*                    产生的调度效果——这些只有上板或 QEMU 实跑能证明，桩只
*                    回答"给定这个契约读数与槽位结果，代码走哪条路"。
* @note 用法：由 tools/coverage_gate.py 的 swaprb / swaprb-off 两档编译并
*          运行；开档输出 SWAPRB-HOST OK，关档输出 SWAPRB-HOST OFF OK。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_swap_rollback.h"
#include "arkrtos_guard.h"
#include "arkrtos_task.h"
#include "arkrtos_loader.h"
#include "arkrtos_log.h"
#include "arkrtos_config.h"
#include "arkrtos_features.h"

/* ==================================================================
 * 内核桩
 * ================================================================== */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count      = 0;
int32          arkrtos_current_task_id = 0;

uint32 arkrtos_log_get_level(void)
{
    return (uint32)ARKRTOS_LOG_INFO;    /* 让 LOGE/LOGI 真的走一遍格式化与出口 */
}

static int g_log_lines;

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level;
    (void)tag;
    (void)line;
    (void)msg;
    g_log_lines++;
}

#if (ARKRTOS_USE_SWAP_ROLLBACK == 1)

/* ---------------- 可编程的契约桩 ---------------- */

static arkrtos_guard_probe_t g_probe;
static int32                 g_probe_rc;      /* 非 0 表示任务查不到 */
static int                   g_rebase_calls;
static int32                 g_rebase_last_id;

int32 arkrtos_guard_probe(int32 task_id, arkrtos_guard_probe_t *p_out)
{
    (void)task_id;
    if(g_probe_rc != 0)
    {
        return g_probe_rc;
    }
    *p_out = g_probe;
    return 0;
}

void arkrtos_guard_rebase(int32 task_id)
{
    g_rebase_calls++;
    g_rebase_last_id = task_id;
}

/* ---------------- 可编程的槽位桩 ---------------- */

static int32  g_stop_rc;
static int32  g_start_rc;
static uint32 g_stop_slot;
static uint32 g_start_slot;
static int    g_stop_calls;
static int    g_start_calls;

int32 arkrtos_loader_stop(uint32 slot)
{
    g_stop_calls++;
    g_stop_slot = slot;
    return g_stop_rc;
}

int32 arkrtos_loader_start(uint32 slot)
{
    g_start_calls++;
    g_start_slot = slot;
    return g_start_rc;
}

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what, int line)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, line);
    }
}

#define CK(cond)                    ck((cond), #cond, __LINE__)
#define CK_EQ(a, b)                 ck(((a) == (b)), #a " == " #b, __LINE__)

/* 一份健康的契约读数：任务 5、周期 10ms、0 违约、READY。 */
static void probe_reset(void)
{
    memset(&g_probe, 0, sizeof(g_probe));
    g_probe.declared  = 1u;
    g_probe.period_ms = 10u;
    g_probe.misses    = 0u;
    g_probe.state     = (uint32)ARKRTOS_TASK_READY;
    g_probe_rc        = 0;
}

#define T_TPM       (1000u / ARKRTOS_TICK_PERIOD_US)
#define T_WINDOW_10 (10u * 3u * T_TPM)   /* period_ms=10 的三周期窗口 */
/* 当 tick 周期 > 1000us 时 T_TPM 归零，三周期窗口算出 0 拍——这正是
 * RB_MIN_WINDOW_TICKS 下限要兜的场景。宿主的默认 tick 周期让下限够不着，
 * 于是用一份只验证下限的最小主函数（-DARKRTOS_TICK_PERIOD_US=2000）单独量它。 */
#if ((1000u / ARKRTOS_TICK_PERIOD_US) == 0u)

int main(void)
{
    arkrtos_swap_rollback_status_t st;

    probe_reset();
    arkrtos_swap_rollback_init();

    /* 周期 10ms，但 T_TPM=0 -> 窗口=0，必须被夹到 2 拍 */
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.window_ticks, 2u);
    CK_EQ(st.ticks_left, 2u);

    /* 两拍后到期转 CONFIRMED */
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_CONFIRMED);
    CK_EQ(st.ticks_left, 0u);

    if(g_fails == 0)
    {
        printf("SWAPRB-CLAMP OK (%d checks)\n", g_checks);
        return 0;
    }
    printf("SWAPRB-CLAMP FAILED: %d/%d\n", g_fails, g_checks);
    return 1;
}

#else   /* 正常 tick 周期：跑完整用例 */


int main(void)
{
    arkrtos_swap_rollback_status_t st;

    probe_reset();
    arkrtos_swap_rollback_init();

    /* 1. init 之后是 IDLE，pending 为 0 */
    memset(&st, 0xFF, sizeof(st));
    CK_EQ(arkrtos_swap_rollback_status(&st), 0);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_IDLE);
    CK_EQ(arkrtos_swap_rollback_pending(), 0);

    /* 2. status(NULL) 被拒 */
    CK_EQ(arkrtos_swap_rollback_status(0), (int32)ARKRTOS_ROLLBACK_ERR_PARAM);

    /* 3. 参数拒绝：task<=0、新旧槽同号（都不记账） */
    CK_EQ(arkrtos_swap_rollback_arm(0, 1u, 2u), (int32)ARKRTOS_ROLLBACK_ERR_PARAM);
    CK_EQ(arkrtos_swap_rollback_arm(-3, 1u, 2u), (int32)ARKRTOS_ROLLBACK_ERR_PARAM);
    CK_EQ(arkrtos_swap_rollback_arm(5, 2u, 2u), (int32)ARKRTOS_ROLLBACK_ERR_PARAM);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.refusals, 0u);
    CK_EQ(st.arms, 0u);

    /* 4. 无契约不布防：probe 失败、declared==0、period==0 三种都拒绝并记账 */
    g_probe_rc = -1;
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), (int32)ARKRTOS_ROLLBACK_ERR_NOCONTRACT);
    g_probe_rc = 0;
    g_probe.declared = 0u;
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), (int32)ARKRTOS_ROLLBACK_ERR_NOCONTRACT);
    g_probe.declared = 1u;
    g_probe.period_ms = 0u;
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), (int32)ARKRTOS_ROLLBACK_ERR_NOCONTRACT);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.refusals, 3u);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_IDLE);

    /* 5. 正常布防：窗口 = 三倍周期，拍数照搬契约 */
    probe_reset();
    g_probe.misses = 4u;                 /* 布防前的历史违约要记下来，不当成新违约 */
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_ARMED);
    CK_EQ(st.window_ticks, (uint32)T_WINDOW_10);
    CK_EQ(st.ticks_left, (uint32)T_WINDOW_10);
    CK_EQ(st.misses_at_arm, 4u);
    CK_EQ(st.misses_now, 4u);
    CK_EQ(st.arms, 1u);
    CK_EQ(st.old_slot, 1u);
    CK_EQ(st.new_slot, 2u);
    CK_EQ(st.task_id, 5);

    /* 6. 正在布防时再布防被拒（只允许一个在飞） */
    CK_EQ(arkrtos_swap_rollback_arm(6, 3u, 4u), (int32)ARKRTOS_ROLLBACK_ERR_STATE);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.refusals, 4u);
    CK_EQ(st.task_id, 5);                /* 原判没被顶掉 */

    /* 7. cancel 指定别的任务被拒，指定本任务成功，之后 IDLE */
    CK_EQ(arkrtos_swap_rollback_cancel(6), (int32)ARKRTOS_ROLLBACK_ERR_STATE);
    CK_EQ(arkrtos_swap_rollback_cancel(0), 0);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_IDLE);
    CK_EQ(st.task_id, 0);

    /* 8. IDLE 时 cancel 是空操作 */
    CK_EQ(arkrtos_swap_rollback_cancel(0), 0);

    /* 9. 窗口内守约：逐拍倒计时，到零转 CONFIRMED(NONE) */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    {
        uint32 i;
        for(i = 0; i < (uint32)T_WINDOW_10; i++)
        {
            CK_EQ(arkrtos_swap_rollback_pending(), 0);
            arkrtos_swap_rollback_tick();
        }
    }
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_CONFIRMED);
    CK_EQ(st.reason, (uint32)ARKRTOS_ROLLBACK_REASON_NONE);
    CK_EQ(st.ticks_left, 0u);

    /* 10. 已 CONFIRMED 后再 tick 是空操作（不会再变状态） */
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_CONFIRMED);

    /* 11. 窗口内违约：misses 增长转 FAILED(CONTRACT)，pending 置 1 */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.misses = 1u;
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_FAILED);
    CK_EQ(st.reason, (uint32)ARKRTOS_ROLLBACK_REASON_CONTRACT);
    CK_EQ(st.misses_now, 1u);
    CK_EQ(arkrtos_swap_rollback_pending(), 1);

    /* 12. service()：先停新槽(2)再启旧槽(1)，成功转 ROLLED_BACK 并记一次回滚 */
    g_stop_rc = 0;
    g_start_rc = 42;
    g_stop_calls = g_start_calls = 0;
    arkrtos_swap_rollback_service();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(g_stop_calls, 1);
    CK_EQ(g_stop_slot, 2u);
    CK_EQ(g_start_calls, 1);
    CK_EQ(g_start_slot, 1u);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_ROLLED_BACK);
    CK_EQ(st.rollbacks, 1u);
    CK_EQ(st.last_rc, 0);
    CK_EQ(arkrtos_swap_rollback_pending(), 0);

    /* 13. 已出队后再 service 是空操作（不会重复停/启） */
    arkrtos_swap_rollback_service();
    CK_EQ(g_stop_calls, 1);
    CK_EQ(g_start_calls, 1);

    /* 14. 任务消失三路：probe 失败 / state==INVALID / state==0xFFFFFFFF */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe_rc = -1;
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_FAILED);
    CK_EQ(st.reason, (uint32)ARKRTOS_ROLLBACK_REASON_DEAD);

    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.state = (uint32)ARKRTOS_TASK_INVALID;
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.reason, (uint32)ARKRTOS_ROLLBACK_REASON_DEAD);

    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.state = 0xFFFFFFFFu;         /* probe 成功但表里没有这台任务 */
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.reason, (uint32)ARKRTOS_ROLLBACK_REASON_DEAD);

    /* 15. 契约中途撤销：declared 变 0 转 CONFIRMED(WITHDRAWN)，保留新槽 */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.declared = 0u;
    arkrtos_swap_rollback_tick();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_CONFIRMED);
    CK_EQ(st.reason, (uint32)ARKRTOS_ROLLBACK_REASON_WITHDRAWN);

    /* 16. service 时停新槽失败也要继续启旧槽，仍然算回滚成功 */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.misses = 2u;
    arkrtos_swap_rollback_tick();        /* FAILED */
    g_stop_rc  = -7;                     /* 停失败 */
    g_start_rc = 9;
    g_stop_calls = g_start_calls = 0;
    arkrtos_swap_rollback_service();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(g_stop_calls, 1);
    CK_EQ(g_start_calls, 1);             /* 照常启旧槽 */
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_ROLLED_BACK);
    CK_EQ(st.last_rc, 0);                /* 最终成功，last_rc 被清 0 */

    /* 17. 旧槽启不来：转 GAVE_UP，不重试（再 service 不再动槽） */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.misses = 1u;
    arkrtos_swap_rollback_tick();        /* FAILED */
    g_stop_rc = 0;
    g_start_rc = -5;
    g_stop_calls = g_start_calls = 0;
    arkrtos_swap_rollback_service();
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_GAVE_UP);
    CK_EQ(st.last_rc, -5);
    arkrtos_swap_rollback_service();     /* GAVE_UP 不在服务门内 */
    CK_EQ(g_start_calls, 1);             /* 没有重试 */

    /* 18. cancel 对 FAILED 也有效（等待 service 期间被运维撤掉） */
    probe_reset();
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    g_probe.misses = 1u;
    arkrtos_swap_rollback_tick();        /* FAILED */
    CK_EQ(arkrtos_swap_rollback_cancel(0), 0);
    arkrtos_swap_rollback_status(&st);
    CK_EQ(st.state, (uint32)ARKRTOS_ROLLBACK_IDLE);
    CK_EQ(arkrtos_swap_rollback_pending(), 0);

    /* 19. 极短周期：窗口 = 三倍周期，且绝不少于 2 拍下限 */
    probe_reset();
    g_probe.period_ms = 1u;
    CK_EQ(arkrtos_swap_rollback_arm(5, 1u, 2u), 0);
    arkrtos_swap_rollback_status(&st);
    CK(st.window_ticks >= 2u);
    CK_EQ(st.window_ticks, (uint32)(1u * 3u * T_TPM));
    (void)arkrtos_swap_rollback_cancel(0);

    /* 20. 日志出口真的被走过（布防/判定/回滚各打一条以上） */
    CK(g_log_lines > 0);

    if(g_fails == 0)
    {
        printf("SWAPRB-HOST OK (%d checks)\n", g_checks);
        return 0;
    }
    printf("SWAPRB-HOST FAILED: %d/%d\n", g_fails, g_checks);
    return 1;
}

#endif  /* T_TPM != 0 */

#else   /* ARKRTOS_USE_SWAP_ROLLBACK == 0 */

/* 关档：整个模块被编掉（零符号），这里只证明关档能编过且不留接口。 */
int main(void)
{
    printf("SWAPRB-HOST OFF OK\n");
    return 0;
}

#endif
