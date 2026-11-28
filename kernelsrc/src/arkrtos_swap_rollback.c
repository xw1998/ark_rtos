/**
* @file arkrtos_swap_rollback.c
* @brief Failure judge and automatic rollback for a hot swap - see the header.
* @details Two rules drive everything here:
*            1. only a task with a heartbeat contract can be judged, because
*               the contract is the only promise the new image can break that
*               the old image was keeping;
*            2. the judgement is counted from the instant the new entry began
*               running (the guard window is rebased at swap time), so a
*               healthy image that reports in on schedule is never punished for
*               the few ticks the swap itself took.
*/

#include "arkrtos_swap_rollback.h"
#include "arkrtos_config.h"     /* feature gates */
#include "arkrtos_features.h"

#if (ARKRTOS_USE_SWAP_ROLLBACK == 1)

#include "arkrtos_guard.h"
#include "arkrtos_task.h"
#include "arkrtos_loader.h"
#include "arkrtos_ptable.h"
#include "arkrtos_log.h"

/* The judgement window is this many contract periods. Three is the smallest
 * count that survives one delayed report (a period long enough to include a
 * blocking call) plus one service-period rounding, without letting a broken
 * image stay in charge for long enough to matter. */
#define RB_WINDOW_PERIODS       (3u)

/* Ticks in one millisecond, same arithmetic the guard uses. */
#define RB_TICKS_PER_MS         (1000u / ARKRTOS_TICK_PERIOD_US)

/* A window shorter than this would be judged before the guard had a chance to
 * count anything, which turns "no evidence yet" into "failed". */
#define RB_MIN_WINDOW_TICKS     (2u)

typedef struct {
    uint32 state;
    uint32 reason;
    int32  task_id;
    uint32 old_slot;
    uint32 new_slot;
    uint32 window_ticks;
    uint32 ticks_left;
    uint32 misses_at_arm;
    uint32 misses_now;
    int32  last_rc;
    uint32 arms;
    uint32 refusals;
    uint32 rollbacks;
} rb_state_t;

static rb_state_t s_rb;

void arkrtos_swap_rollback_init(void)
{
    s_rb.state         = ARKRTOS_ROLLBACK_IDLE;
    s_rb.reason        = ARKRTOS_ROLLBACK_REASON_NONE;
    s_rb.task_id       = 0;
    s_rb.old_slot      = 0u;
    s_rb.new_slot      = 0u;
    s_rb.window_ticks  = 0u;
    s_rb.ticks_left    = 0u;
    s_rb.misses_at_arm = 0u;
    s_rb.misses_now    = 0u;
    s_rb.last_rc       = 0;
    s_rb.arms          = 0u;
    s_rb.refusals      = 0u;
    s_rb.rollbacks     = 0u;
}

int32 arkrtos_swap_rollback_arm(int32 task_id, uint32 old_slot, uint32 new_slot)
{
    arkrtos_guard_probe_t g;
    uint32 window;

    if((task_id <= 0) || (old_slot == new_slot))
    {
        return ARKRTOS_ROLLBACK_ERR_PARAM;
    }

    if(s_rb.state == ARKRTOS_ROLLBACK_ARMED)
    {
        /* One in flight, like the swap itself: two verdicts that can each stop
         * a slot would take turns rolling each other back. */
        s_rb.refusals++;
        return ARKRTOS_ROLLBACK_ERR_STATE;
    }

    if((arkrtos_guard_probe(task_id, &g) != 0) || (g.declared == 0u) ||
       (g.period_ms == 0u))
    {
        /* No promise, no judge. Registering a contract is the image's own
         * decision (it knows its loop time); inventing one here would turn a
         * busy image into a false failure and a healthy one into a quiet pass. */
        s_rb.refusals++;
        ARKRTOS_LOGI("SWAP", "rollback not armed for task %d: no heartbeat contract",
                   (int)task_id);
        return ARKRTOS_ROLLBACK_ERR_NOCONTRACT;
    }

    window = g.period_ms * RB_WINDOW_PERIODS * RB_TICKS_PER_MS;
    if(window < RB_MIN_WINDOW_TICKS)
    {
        window = RB_MIN_WINDOW_TICKS;
    }

    s_rb.state         = ARKRTOS_ROLLBACK_ARMED;
    s_rb.reason        = ARKRTOS_ROLLBACK_REASON_NONE;
    s_rb.task_id       = task_id;
    s_rb.old_slot      = old_slot;
    s_rb.new_slot      = new_slot;
    s_rb.window_ticks  = window;
    s_rb.ticks_left    = window;
    s_rb.misses_at_arm = g.misses;
    s_rb.misses_now    = g.misses;
    s_rb.arms++;

    ARKRTOS_LOGI("SWAP", "rollback armed: task %d in slot %u, %u tick(s) (%u ms x %u)"
               " to keep its heartbeat or slot %u comes back",
               (int)task_id, (unsigned)new_slot, (unsigned)window,
               (unsigned)g.period_ms, (unsigned)RB_WINDOW_PERIODS,
               (unsigned)old_slot);
    return 0;
}

int32 arkrtos_swap_rollback_cancel(int32 task_id)
{
    if((s_rb.state != ARKRTOS_ROLLBACK_ARMED) &&
       (s_rb.state != ARKRTOS_ROLLBACK_FAILED))
    {
        return 0;           /* nothing armed: cancelling an idle judge is a no-op */
    }

    if((task_id != 0) && (task_id != s_rb.task_id))
    {
        return ARKRTOS_ROLLBACK_ERR_STATE;
    }

    ARKRTOS_LOGI("SWAP", "rollback cancelled for task %d", (int)s_rb.task_id);
    s_rb.state  = ARKRTOS_ROLLBACK_IDLE;
    s_rb.task_id = 0;
    return 0;
}

/* Mark the verdict. The action itself is left to service(). */
static void rb_fail(uint32 reason)
{
    s_rb.state  = ARKRTOS_ROLLBACK_FAILED;
    s_rb.reason = reason;

    if(reason == ARKRTOS_ROLLBACK_REASON_CONTRACT)
    {
        ARKRTOS_LOGE("SWAP", "rollback: task %d broke its heartbeat contract in slot %u"
                   " (%u -> %u misses); slot %u will be restarted",
                   (int)s_rb.task_id, (unsigned)s_rb.new_slot,
                   (unsigned)s_rb.misses_at_arm, (unsigned)s_rb.misses_now,
                   (unsigned)s_rb.old_slot);
    }
    else
    {
        /* Only CONTRACT and DEAD reach here; a withdrawn contract is not a
         * failure and is handled inline in tick() as CONFIRMED, never FAILED.
         * A task that disappears stops accruing misses, so it must be judged
         * dead before the miss count is consulted.
         */
        ARKRTOS_LOGE("SWAP", "rollback: task %d is gone from slot %u;"
                   " slot %u will be restarted",
                   (int)s_rb.task_id, (unsigned)s_rb.new_slot,
                   (unsigned)s_rb.old_slot);
    }
}

void arkrtos_swap_rollback_tick(void)
{
    arkrtos_guard_probe_t g;

    if(s_rb.state != ARKRTOS_ROLLBACK_ARMED)
    {
        return;
    }

    if(arkrtos_guard_probe(s_rb.task_id, &g) != 0)
    {
        rb_fail(ARKRTOS_ROLLBACK_REASON_DEAD);
        return;
    }

    s_rb.misses_now = g.misses;

    /* The task is gone: there is no image to keep, so the verdict cannot be
     * "it worked". Judged before the miss count, because a dead task stops
     * accruing misses and would otherwise look healthy forever. */
    if((g.state == (uint32)ARKRTOS_TASK_INVALID) ||
       (g.state == 0xFFFFFFFFu))
    {
        rb_fail(ARKRTOS_ROLLBACK_REASON_DEAD);
        return;
    }

    if(g.declared == 0u)
    {
        /* Withdrawn mid-window: the promise we were judging no longer exists.
         * Say so and keep the image; it is not the judge's job to re-impose a
         * contract the image itself dropped. */
        s_rb.state  = ARKRTOS_ROLLBACK_CONFIRMED;
        s_rb.reason = ARKRTOS_ROLLBACK_REASON_WITHDRAWN;
        ARKRTOS_LOGI("SWAP", "rollback: task %d withdrew its contract; slot %u kept",
                   (int)s_rb.task_id, (unsigned)s_rb.new_slot);
        return;
    }

    if(g.misses > s_rb.misses_at_arm)
    {
        rb_fail(ARKRTOS_ROLLBACK_REASON_CONTRACT);
        return;
    }

    if(s_rb.ticks_left > 0u)
    {
        s_rb.ticks_left--;
        if(s_rb.ticks_left == 0u)
        {
            s_rb.state  = ARKRTOS_ROLLBACK_CONFIRMED;
            s_rb.reason = ARKRTOS_ROLLBACK_REASON_NONE;
            ARKRTOS_LOGI("SWAP", "rollback: task %d kept its contract through the window;"
                       " slot %u stands",
                       (int)s_rb.task_id, (unsigned)s_rb.new_slot);
        }
    }
}

void arkrtos_swap_rollback_service(void)
{
    int32 rc;
    int32 tid;

    if(s_rb.state != ARKRTOS_ROLLBACK_FAILED)
    {
        return;
    }

    /* Take the verdict out of the queue before doing anything that can be
     * preempted: a second pass must not repeat the stop/start. */
    s_rb.state = ARKRTOS_ROLLBACK_ROLLED_BACK;

    /* Stop the new image first. Its tasks are taken out of the scheduler and
     * the slot goes back to LOADED, which is the state the old slot must be in
     * for start() to accept it. A refusal here is not fatal: the slot may
     * already have been stopped by its own crash path, and the rollback still
     * has to try. */
    rc = arkrtos_loader_stop(s_rb.new_slot);
    s_rb.last_rc = rc;
    if(rc != 0)
    {
        ARKRTOS_LOGE("SWAP", "rollback: stopping slot %u reported %d; starting slot %u anyway",
                   (unsigned)s_rb.new_slot, (int)rc, (unsigned)s_rb.old_slot);
    }

    tid = arkrtos_loader_start(s_rb.old_slot);
    if(tid > 0)
    {
        s_rb.rollbacks++;
        s_rb.last_rc = 0;
        ARKRTOS_LOGE("SWAP", "rollback done: slot %u is running again as task %d"
                   " (slot %u stopped)",
                   (unsigned)s_rb.old_slot, (int)tid, (unsigned)s_rb.new_slot);
    }
    else
    {
        /* No retry loop: a slot that cannot start (crashed too often, image
         * gone, RAM window taken) will not start on the next tick either, and
         * a retry storm would drown the log that explains why. */
        s_rb.state   = ARKRTOS_ROLLBACK_GAVE_UP;
        s_rb.last_rc = tid;
        ARKRTOS_LOGE("SWAP", "rollback failed: slot %u would not start (%d);"
                   " no image is running this task",
                   (unsigned)s_rb.old_slot, (int)tid);
    }
}

int32 arkrtos_swap_rollback_status(arkrtos_swap_rollback_status_t *p_out)
{
    if(p_out == 0)
    {
        return ARKRTOS_ROLLBACK_ERR_PARAM;
    }

    p_out->state         = s_rb.state;
    p_out->reason        = s_rb.reason;
    p_out->task_id       = s_rb.task_id;
    p_out->old_slot      = s_rb.old_slot;
    p_out->new_slot      = s_rb.new_slot;
    p_out->window_ticks  = s_rb.window_ticks;
    p_out->ticks_left    = s_rb.ticks_left;
    p_out->misses_at_arm = s_rb.misses_at_arm;
    p_out->misses_now    = s_rb.misses_now;
    p_out->last_rc       = s_rb.last_rc;
    p_out->arms          = s_rb.arms;
    p_out->refusals      = s_rb.refusals;
    p_out->rollbacks     = s_rb.rollbacks;
    return 0;
}

int32 arkrtos_swap_rollback_pending(void)
{
    return (s_rb.state == ARKRTOS_ROLLBACK_FAILED) ? 1 : 0;
}

#else   /* ARKRTOS_USE_SWAP_ROLLBACK == 0 */

/* Compiled out: the object is empty rather than a pile of no-op calls, so an
 * OFF build shows no rollback symbol at all (same rule as every other switch
 * in this tree). The call sites are guarded with the same #if. */
typedef int arkrtos_swap_rollback_compiled_out_t;

#endif /* ARKRTOS_USE_SWAP_ROLLBACK */
