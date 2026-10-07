/**
* @file arkrtos_guard.c
* @brief Watchdog gating and the per-slot heartbeat contract - see arkrtos_guard.h
*/

#include "arkrtos_guard.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_ptable.h"
#include "arkrtos_audit.h"
#include "arkrtos_crash.h"
#include "arkrtos_fault.h"
#include "arkrtos_log.h"
#include "arkrtos_hal.h"
#include "arkrtos_config.h"    /* feature gates: ARKRTOS_USE_KERNEL_GUARD */
#if ARKRTOS_USE_KERNEL_GUARD

#ifndef ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           (0)
#endif

#ifndef ARKRTOS_WDG_TIMEOUT_MS
#define ARKRTOS_WDG_TIMEOUT_MS       (4000u)
#endif

/* Service period: the shortest window the contract can be judged against, and
 * the granularity of every age this module reports. */
#define ARKRTOS_GUARD_SERVICE_MS     (ARKRTOS_GUARD_MIN_PERIOD_MS)

/* Self-audit cadence. The audit is a few hundred loads, so running it once a
 * second is noise; running it every service period would buy nothing, because
 * a corrupted structure stays corrupted. */
#define ARKRTOS_GUARD_AUDIT_DIV      (10u)

/* For the tick arithmetic. */
#define ARKRTOS_GUARD_TICKS_PER_MS   (1000u / ARKRTOS_TICK_PERIOD_US)
#define ARKRTOS_GUARD_SERVICE_TICKS  (ARKRTOS_GUARD_SERVICE_MS * ARKRTOS_GUARD_TICKS_PER_MS)

/** @brief "no evidence recorded yet" */
#define ARKRTOS_GUARD_AGE_NONE       (0xFFFFFFFFu)

typedef struct {
    uint32 period_ms;       /* 0 = no contract declared for this task */
    uint32 beat_tick;       /* last explicit heartbeat  (0 = never) */
    uint32 svc_tick;        /* last SVC call from this task (0 = never) */
    uint32 cpu_tick;        /* last time it was the running task (0 = never) */
    uint32 misses;          /* service periods spent in violation */
    uint32 task_id;         /* mirrored so a report is readable without the TCB */
} arkrtos_guard_entry_t;

/* Indexed by task id (1-based) so the hot path is one array store. Index 0 is
 * unused: task id 0 is the idle task, which every SVC path excludes anyway. */
static arkrtos_guard_entry_t g_guard[ARKRTOS_TASK_MAX_NUM + 1];

static arkrtos_guard_status_t g_status;
static uint32 g_audit_div = 0u;

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static uint32 guard_age_ms(uint32 now_tick, uint32 stamp_tick)
{
    if(stamp_tick == 0u)
    {
        return ARKRTOS_GUARD_AGE_NONE;
    }
    return (uint32)((now_tick - stamp_tick) / ARKRTOS_GUARD_TICKS_PER_MS);
}

static uint32 guard_is_violating(const arkrtos_guard_entry_t *p_e, uint32 now_tick)
{
    uint32 window_ticks;

    if((p_e->period_ms == 0u) || (p_e->task_id == 0u))
    {
        return 0u;
    }

    window_ticks = p_e->period_ms * ARKRTOS_GUARD_TICKS_PER_MS;
    if((now_tick - p_e->beat_tick) <= window_ticks)
    {
        return 0u;      /* the image reported in on time */
    }

    return 1u;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                 */
/* ------------------------------------------------------------------ */

void arkrtos_guard_init(void)
{
    uint32 i;

    for(i = 0u; i <= ARKRTOS_TASK_MAX_NUM; i++)
    {
        g_guard[i].period_ms = 0u;
        g_guard[i].beat_tick = 0u;
        g_guard[i].svc_tick  = 0u;
        g_guard[i].cpu_tick  = 0u;
        g_guard[i].misses    = 0u;
        g_guard[i].task_id   = 0u;
    }

    g_status.enabled    = (uint32)ARKRTOS_WDG_ENABLE;
    g_status.timeout_ms = 0u;
    g_status.feeds      = 0u;
    g_status.starves    = 0u;
    g_status.verdict    = ARKRTOS_GUARD_OK;
    g_status.audit_mask = 0u;
    g_status.audit_runs = 0u;
    g_status.violating  = 0u;
    g_status.declared   = 0u;
    g_audit_div         = 0u;

    #if (ARKRTOS_WDG_ENABLE == 1)
    if(arkrtos_port_wdg_init((uint32)ARKRTOS_WDG_TIMEOUT_MS) != 0)
    {
        /* Requested but not running: say so instead of leaving the reader to
         * conclude from "enabled = 1" that a watchdog is protecting the board. */
        ARKRTOS_LOGE("GUARD", "watchdog requested but the port refused it (%u ms)",
                   (uint32)ARKRTOS_WDG_TIMEOUT_MS);
    }
    else
    {
        g_status.timeout_ms = arkrtos_port_wdg_timeout_ms();
        ARKRTOS_LOGI("GUARD", "watchdog armed, hardware timeout %u ms",
                   (uint32)g_status.timeout_ms);
    }
    #endif
}

void arkrtos_guard_svc(int32 task_id)
{
    uint32 now;

    if((task_id <= 0) || (task_id > ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }

    now = arkrtos_kernel_get_tick();
    g_guard[task_id].svc_tick = now;
    g_guard[task_id].task_id  = (uint32)task_id;
}

int32 arkrtos_guard_beat(int32 task_id, uint32 period_ms)
{
    uint32 now;

    if((task_id <= 0) || (task_id > ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }

    if((period_ms != 0u) && (period_ms < ARKRTOS_GUARD_MIN_PERIOD_MS))
    {
        /* Refuse rather than accept a contract that cannot be checked: a
         * window shorter than the service period would be judged by sampling,
         * which produces verdicts that are not evidence. */
        return -1;
    }

    now = arkrtos_kernel_get_tick();

    if(period_ms == 0u)
    {
        if(g_guard[task_id].period_ms != 0u)
        {
            g_status.declared--;
        }
        g_guard[task_id].period_ms = 0u;
        return 0;
    }

    if(g_guard[task_id].period_ms == 0u)
    {
        g_status.declared++;
    }
    g_guard[task_id].period_ms = period_ms;
    g_guard[task_id].beat_tick = now;
    g_guard[task_id].task_id   = (uint32)task_id;

    return 0;
}

void arkrtos_guard_tick(void)
{
    uint32 now;
    uint32 violating = 0u;
    int32  id;

    now = arkrtos_kernel_get_tick();

    /* Free evidence: whatever was on the CPU at this tick did run. Deliberately
     * not used to satisfy the contract (see the header). */
    if((arkrtos_current_task_id > 0) && (arkrtos_current_task_id <= ARKRTOS_TASK_MAX_NUM))
    {
        g_guard[arkrtos_current_task_id].cpu_tick  = now;
        g_guard[arkrtos_current_task_id].task_id   = (uint32)arkrtos_current_task_id;
    }

    if((now % ARKRTOS_GUARD_SERVICE_TICKS) != 0u)
    {
        return;
    }

    /* ---- contract ---- */
    for(id = 1; id <= ARKRTOS_TASK_MAX_NUM; id++)
    {
        arkrtos_guard_entry_t *p_e = &g_guard[id];

        if(p_e->period_ms == 0u)
        {
            continue;
        }

        /* The task is gone, so its contract goes with it. Without this the
         * entry would outlive the task and be charged to whoever is handed
         * the id next. */
        if((id > arkrtos_task_count) ||
           ((uint32)arkrtos_task_table[id - 1].status == ARKRTOS_TASK_INVALID))
        {
            p_e->period_ms = 0u;
            g_status.declared--;
            continue;
        }

        /* Judged on the beat age alone, whatever the task happens to be doing
         * at this instant: the period is the image's own promise, so a sleep
         * longer than the period is a broken promise, not an excuse. Excusing
         * blocked tasks would make the contract unenforceable for the ordinary
         * shape of an app loop, and an image that never wakes up would never
         * be caught. */
        if(guard_is_violating(p_e, now) != 0u)
        {
            p_e->misses++;
            violating++;
        }
    }

    g_status.violating = violating;

    /* ---- structure audit, at a slower cadence ---- */
    g_audit_div++;
    if((g_audit_div % ARKRTOS_GUARD_AUDIT_DIV) == 0u)
    {
        arkrtos_audit_result_t r;

        arkrtos_audit_run(&r);
        g_status.audit_mask = r.mask;
        g_status.audit_runs++;
    }

    g_status.verdict = ARKRTOS_GUARD_OK;
    if(g_status.audit_mask != 0u)
    {
        g_status.verdict |= ARKRTOS_GUARD_BAD_AUDIT;
    }
    if(violating != 0u)
    {
        g_status.verdict |= ARKRTOS_GUARD_BAD_BEAT;
    }

    /* ---- feed or starve ---- */
    if(g_status.enabled == 0u)
    {
        return;                 /* board asked for no watchdog */
    }
    if(g_status.timeout_ms == 0u)
    {
        return;                 /* requested but not armed; nothing to feed */
    }

    if(g_status.verdict == ARKRTOS_GUARD_OK)
    {
        arkrtos_port_wdg_feed();
        g_status.feeds++;
    }
    else
    {
        /* Record the reason once per verdict change, not every period: a
         * full fault ring would otherwise bury the first occurrence. */
        if(g_status.starves == 0u)
        {
            /* Charge this reset to the images that broke their contract,
             * before the watchdog ends the run.
             *
             * Once per boot, not once per tick. The latch is g_status.starves,
             * which this function never clears after the first starvation: the
             * tick re-evaluates the same violation every period, so charging per
             * evaluation would turn one broken image into hundreds of "faults"
             * and hold a slot for the single condition it was already in. That
             * rate limit also sets the convergence speed - a slot that keeps
             * breaking its promise gains exactly one fault per boot, so it is
             * held on the boot after its count reaches the limit. Deliberately
             * not per episode: an image whose health flickers would otherwise
             * be accused faster than a reset can happen.
             *
             * Without the charge at all, the count dies with the RAM it lives in,
             * the image autostarts again and the board reboots forever instead
             * of holding one slot. */
            for(id = 1; id <= ARKRTOS_TASK_MAX_NUM; id++)
            {
                arkrtos_guard_entry_t *p_v = &g_guard[id];
                int32 slot;
                uint32 n;

                if((p_v->period_ms == 0u) || (p_v->task_id == 0u) ||
                   (guard_is_violating(p_v, now) == 0u))
                {
                    continue;
                }

                slot = arkrtos_ptable_find_task((uint32)id);

                /* No slot to tag (not an image task), or already held: the
                 * number would only grow without changing any decision. */
                if((slot < 0) || (arkrtos_crash_disabled((uint32)slot) != 0u))
                {
                    continue;
                }

                n = arkrtos_crash_fault((uint32)slot, ARKRTOS_CRASH_REASON_HEARTBEAT);

                if(arkrtos_crash_disabled((uint32)slot) != 0u)
                {
                    ARKRTOS_LOGE("CRASH", "slot %d held: %s, %u consecutive faults"
                               " (declared %u ms, silent for %u ticks)",
                               (int)slot,
                               arkrtos_crash_reason_name(arkrtos_crash_disabled((uint32)slot)),
                               (unsigned)n,
                               (unsigned)p_v->period_ms,
                               (unsigned)(now - p_v->beat_tick));
                }
            }

            if((g_status.verdict & ARKRTOS_GUARD_BAD_AUDIT) != 0u)
            {
                arkrtos_fault_record(ARKRTOS_FAULT_AUDITFAIL, 0);
            }
            if((g_status.verdict & ARKRTOS_GUARD_BAD_BEAT) != 0u)
            {
                arkrtos_fault_record(ARKRTOS_FAULT_GUARDHB, 0);
            }
            ARKRTOS_LOGE("GUARD", "not feeding: verdict 0x%X (audit 0x%X, violating %u)",
                       (uint32)g_status.verdict, (uint32)g_status.audit_mask,
                       (uint32)violating);
        }
        g_status.starves++;
    }
}

void arkrtos_guard_status(arkrtos_guard_status_t *p_out)
{
    if(p_out == 0)
    {
        return;
    }
    *p_out = g_status;
}

uint32 arkrtos_guard_task_slots(void)
{
    return (uint32)ARKRTOS_TASK_MAX_NUM;
}

int32 arkrtos_guard_task_at(uint32 idx, arkrtos_guard_task_t *p_out)
{
    const arkrtos_guard_entry_t *p_e;
    int32 slot;
    int32 id;
    uint32 now;

    if((p_out == 0) || (idx >= (uint32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }

    id = (int32)(idx + 1u);
    p_e = &g_guard[id];
    now = arkrtos_kernel_get_tick();

    p_out->task_id     = (id <= arkrtos_task_count) ? id : 0;
    p_out->slot        = -1;
    p_out->declared    = (p_e->period_ms != 0u) ? 1u : 0u;
    p_out->period_ms   = p_e->period_ms;
    p_out->state       = 0xFFFFFFFFu;
    p_out->beat_age_ms = guard_age_ms(now, p_e->beat_tick);
    p_out->svc_age_ms  = guard_age_ms(now, p_e->svc_tick);
    p_out->cpu_age_ms  = guard_age_ms(now, p_e->cpu_tick);
    p_out->misses      = p_e->misses;

    if(p_out->task_id != 0)
    {
        p_out->state = (uint32)arkrtos_task_table[id - 1].status;
        slot = arkrtos_ptable_find_task((uint32)id);
        if(slot >= 0)
        {
            p_out->slot = slot;
        }
    }

    return 0;
}

#else   /* ARKRTOS_USE_KERNEL_GUARD == 0 */
/* Heartbeat contract compiled out.  Recording is a no-op (there is no table
 * to record into) and an explicit beat is refused, so an application that
 * asks for supervision is told "no", not "yes" - a caller that trusts a
 * silent success would ship a watchdog contract nobody is keeping. */

#include <string.h>

void arkrtos_guard_init(void)
{
}

void arkrtos_guard_tick(void)
{
}

void arkrtos_guard_svc(int32 task_id)
{
    (void)task_id;
}

int32 arkrtos_guard_beat(int32 task_id, uint32 period_ms)
{
    (void)task_id;
    (void)period_ms;
    return -1;
}

void arkrtos_guard_status(arkrtos_guard_status_t *p_out)
{
    if(p_out != 0)
    {
        memset(p_out, 0, sizeof(*p_out));
    }
}

int32 arkrtos_guard_task_at(uint32 idx, arkrtos_guard_task_t *p_out)
{
    (void)idx;
    (void)p_out;
    return -1;
}

uint32 arkrtos_guard_task_slots(void)
{
    return 0u;
}
#endif /* ARKRTOS_USE_KERNEL_GUARD */
