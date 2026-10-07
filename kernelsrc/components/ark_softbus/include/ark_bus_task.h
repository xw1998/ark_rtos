/*
 * ark_bus_task.h - the task engine: submit work, watch it run, collect results.
 *
 * A task is one RPC call to a worker's TASK_RUN method.  The wire payload is
 *
 *     task_id u16 | user params ...
 *
 * so a worker can tell a redelivery (same task_id) from new work and stay
 * idempotent.  The engine tracks each task through a small state machine:
 *
 *     QUEUED -> RUNNING -> DONE | FAILED | LOST
 *                |  ^
 *                v  |
 *              (retry: another worker, same task id)
 *
 * Delivery is at-least-once: a worker that loses a reply will simply see the
 * same task id again.  ark_bus_task_on_run() installs a body that also keeps a
 * small result cache, so a redelivered task returns the earlier answer instead
 * of running again.
 *
 * A retry (after a reply timeout, or after the worker is seen going offline)
 * is re-dispatched to a *different* worker whenever one is available; the
 * worker that just failed is only reused when it is the sole candidate.
 *
 * The whole engine compiles out when ARK_BUS_TASK_ENABLED is 0 (which in turn
 * requires ARK_BUS_RPC_ENABLED).
 */
#ifndef ARK_BUS_TASK_H
#define ARK_BUS_TASK_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus.h"

#if ARK_BUS_TASK_ENABLED

typedef enum {
    ARK_BUS_TASK_QUEUED = 0,   /* waiting for a worker                 */
    ARK_BUS_TASK_RUNNING,      /* dispatched, awaiting the reply       */
    ARK_BUS_TASK_DONE,         /* finished, result retained            */
    ARK_BUS_TASK_FAILED,       /* worker answered with an error        */
    ARK_BUS_TASK_LOST,         /* no worker could run it after retries */
    ARK_BUS_TASK_CANCELLED     /* withdrawn by the coordinator         */
} ark_bus_task_state_t;

/*
 * Submit a task.  If `worker_id` is nonzero the task goes straight to that
 * worker; otherwise the first ONLINE device whose capabilities include every
 * bit of `require_caps` is chosen (pass 0 for "any worker").  If no worker is
 * available right now the task stays QUEUED until one appears.
 *
 * `params` (<= ARK_BUS_TASK_PARAM_MAX bytes) is copied into the task record so
 * a failed attempt can be re-sent.  `cb` (optional) is called once, with the
 * terminal state.
 *
 * Returns the task id (> 0), or ARK_BUS_E_FULL / ARK_BUS_E_INVAL.
 */
int ark_bus_task_submit(ark_bus_t *bus, uint16_t method,
                        const void *params, uint16_t len,
                        uint32_t require_caps, uint32_t worker_id,
                        ark_bus_task_cb_t cb, void *user);

/*
 * State of `task_id` (an ark_bus_task_state_t), or ARK_BUS_E_NOTFOUND.
 */
int ark_bus_task_status(const ark_bus_t *bus, uint16_t task_id);

/*
 * Copy the result of a finished task.  Up to `cap` bytes are written and the
 * true length is stored in *len.  Returns ARK_BUS_OK, ARK_BUS_E_AGAIN when the
 * task has not finished, or ARK_BUS_E_NOTFOUND when the task or its (evicted)
 * result is unknown.
 */
int ark_bus_task_result(const ark_bus_t *bus, uint16_t task_id,
                        uint8_t *buf, uint16_t cap, uint16_t *len);

/*
 * Withdraw a task.  A queued task is dropped; a running task is abandoned (the
 * worker is not interrupted in M1) and its late reply ignored.  Returns
 * ARK_BUS_OK or ARK_BUS_E_NOTFOUND.
 */
int ark_bus_task_cancel(ark_bus_t *bus, uint16_t task_id);

/* Number of task records currently held. */
uint8_t ark_bus_task_count(const ark_bus_t *bus);

/* Read-only access to a record (NULL when the index is out of range). */
const ark_bus_task_entry_t *ark_bus_task_at(const ark_bus_t *bus,
                                            uint8_t index);

/*
 * Worker side: install the body that serves TASK_RUN.  The wrapper strips the
 * task id, consults an idempotence cache, and calls `fn` only on first sight
 * of a task id.  It also registers the TASK_RUN RPC method.
 */
int ark_bus_task_on_run(ark_bus_t *bus, ark_bus_task_fn fn, void *user);

/* Whether a worker body has been installed (used by capability reporting). */
int ark_bus_task_has_run_handler(const ark_bus_t *bus);

#endif /* ARK_BUS_TASK_ENABLED */

#endif /* ARK_BUS_TASK_H */
