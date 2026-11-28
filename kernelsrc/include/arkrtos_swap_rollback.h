/**
* @file arkrtos_swap_rollback.h
* @brief Failure judge and automatic rollback for a hot swap (A/B seam).
* @details `ARKRTOS_USE_TASK_SWAP` moves a running task onto another image at
*          its next SVC and keeps the old slot LOADED - that is the raw material
*          for a rollback. This module answers the question the swap itself
*          deliberately leaves open: **who decides the new image failed**, and
*          what is done about it.
*
*          The answer is the heartbeat contract, not a timer of our own. A task
*          under `arkrtos_guard_beat(task_id, period_ms)` has promised to report
*          in every period. The window is three periods counted from the moment
*          the new entry started:
*            - a violation counted inside that window means the new image did not
*              keep a promise the old one was keeping -> roll back;
*            - no violation means it did -> keep it, and say so in the log.
*          A task with no contract is not armable: there is nothing to judge it
*          by, and pretending otherwise would roll back healthy images.
*
*          Execution is split in two on purpose:
*            - `arkrtos_swap_rollback_tick()` runs inside the tick handler. It
*              only reads and sets flags - no scheduler surgery, no I/O.
*            - `arkrtos_swap_rollback_service()` runs in task context (the SVC
*              entry path) and does the actual stop + restart. Stopping an image
*              closes its VFS handles, which must not happen in an ISR.
*
*          Off by default: when `ARKRTOS_USE_SWAP_ROLLBACK` is 0 every entry
*          point here is compiled out and the swap behaves exactly as before.
*/
#ifndef __ARKRTOS_SWAP_ROLLBACK_H__
#define __ARKRTOS_SWAP_ROLLBACK_H__

#include "arkrtos_types.h"

#if (defined(__cplusplus))
extern "C" {
#endif

/* Return codes. -1 is shared on purpose between "bad argument" and "wrong
 * state": the caller's recovery is the same (do not arm), and the name states
 * which rule was broken. */
#define ARKRTOS_ROLLBACK_ERR_PARAM       (-1)   /* bad task id / slot / null output */
#define ARKRTOS_ROLLBACK_ERR_STATE       (-1)   /* one judgement is already armed */
#define ARKRTOS_ROLLBACK_ERR_NOCONTRACT  (-2)   /* the task has no heartbeat period: not judgeable */
#define ARKRTOS_ROLLBACK_ERR_DISABLED    (-1)   /* compiled out */

/* State of the judge. ARMING is not a state: arm() either accepts (ARMED) or
 * returns a reason. */
#define ARKRTOS_ROLLBACK_IDLE            (0u)
#define ARKRTOS_ROLLBACK_ARMED           (1u)   /* counting down, watching misses */
#define ARKRTOS_ROLLBACK_CONFIRMED       (2u)   /* window passed without a violation */
#define ARKRTOS_ROLLBACK_FAILED          (3u)   /* judged failed; waiting for service() */
#define ARKRTOS_ROLLBACK_ROLLED_BACK     (4u)   /* old slot started again */
#define ARKRTOS_ROLLBACK_GAVE_UP         (5u)   /* rollback attempted, the old slot would not start */

/* Why the judge fired (or did not). */
#define ARKRTOS_ROLLBACK_REASON_NONE     (0u)
#define ARKRTOS_ROLLBACK_REASON_CONTRACT (1u)   /* the new image broke its heartbeat contract */
#define ARKRTOS_ROLLBACK_REASON_DEAD     (2u)   /* the new image's task is gone */
#define ARKRTOS_ROLLBACK_REASON_WITHDRAWN (3u)  /* the contract was withdrawn: nothing left to judge on */

/** @brief One readable snapshot of the judge, for the shell and for reports. */
typedef struct {
    uint32 state;           /* ARKRTOS_ROLLBACK_* state */
    uint32 reason;          /* ARKRTOS_ROLLBACK_REASON_* of the last verdict */
    int32  task_id;         /* the task that was swapped (0 when idle) */
    uint32 old_slot;        /* the slot kept for rollback */
    uint32 new_slot;        /* the slot the task was moved to */
    uint32 window_ticks;    /* length of the judgement window */
    uint32 ticks_left;      /* ticks left before the window closes */
    uint32 misses_at_arm;   /* contract violations already on the clock when armed */
    uint32 misses_now;      /* the same counter at the last look */
    int32  last_rc;         /* result of the last stop/start attempt (0 = ok) */
    uint32 arms;            /* times a judgement was armed since boot */
    uint32 refusals;        /* times arming was refused (no contract / busy) */
    uint32 rollbacks;       /* times an old slot was actually restarted */
} arkrtos_swap_rollback_status_t;

/** @brief Clear the judge. Called from kernel init. */
void arkrtos_swap_rollback_init(void);

/**
* @brief Arm the judge after a swap has landed.
* @details Reads the task's heartbeat contract to size the window (three
*          periods, at least two ticks). Refuses when the task has no contract:
*          an unjudgeable image is left alone rather than rolled back on a
*          guess.
* @param task_id   1-based task id that was just moved.
* @param old_slot  Slot that still holds the previous image (must stay LOADED).
* @param new_slot  Slot the task was moved into (stopped on rollback).
* @return 0 when armed; ARKRTOS_ROLLBACK_ERR_x otherwise.
*/
int32 arkrtos_swap_rollback_arm(int32 task_id, uint32 old_slot, uint32 new_slot);

/**
* @brief Drop a judgement without acting on it.
* @details Used when the operator stops/uninstalls either slot underneath us,
*          or when a second swap supersedes this one.
* @param task_id  The judged task; 0 cancels whatever is armed.
* @return 0 when cancelled (or nothing was armed), -1 when another task is armed.
*/
int32 arkrtos_swap_rollback_cancel(int32 task_id);

/**
* @brief One tick of judgement. Runs inside the tick handler.
* @note Reads the guard table and writes this module's state; never touches the
*       scheduler, the file system or the slot table.
*/
void arkrtos_swap_rollback_tick(void);

/**
* @brief Carry out a failed verdict. Runs in task context (SVC entry path).
* @details Stops the new slot (it goes back to LOADED, its tasks are taken out
*          of the scheduler) and starts the old slot again. Runs at most once
*          per verdict; a failed restart is reported, not retried, so a broken
*          image cannot cause a restart storm.
*/
void arkrtos_swap_rollback_service(void);

/**
* @brief Fill in the status snapshot.
*/
int32 arkrtos_swap_rollback_status(arkrtos_swap_rollback_status_t *p_out);

/** @brief 1 when a verdict is waiting for service(), else 0 (cheap check). */
int32 arkrtos_swap_rollback_pending(void);

#if (defined(__cplusplus))
}
#endif

#endif /* __ARKRTOS_SWAP_ROLLBACK_H__ */
