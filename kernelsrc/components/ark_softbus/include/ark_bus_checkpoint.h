/*
 * ark_bus_checkpoint.h - task-state checkpoint and restore (M4).
 *
 * The task engine keeps work-in-flight at a coordinator: a QUEUED task waiting
 * for a worker, a RUNNING task with a reply on the way.  If that coordinator
 * restarts, the work is gone and every worker sits idle.  This layer writes the
 * in-flight table into a caller-owned buffer (a file, a flash sector, a
 * checkpoint record) and re-adopts it on the next boot, so the job continues
 * instead of being resubmitted by hand.
 *
 * What is saved, exactly: for every non-terminal task, its state, retries,
 * task id, method, the worker it was on, the capability mask it needs and its
 * parameter block.  Nothing else -- the format is stated in the .c file and
 * every field is big-endian like the rest of the wire.
 *
 * What cannot be saved, stated plainly: the completion callback and its `user`
 * pointer (a function address and a caller closure do not survive a reboot),
 * the outstanding RPC request id, and the "already tried" worker mask (it
 * indexes the live device table).  A restored task therefore comes back as
 * QUEUED with cb == NULL, to be re-dispatched to any eligible worker; the
 * application re-installs its handlers (ark_bus_task_on_run and any RPC
 * methods) and calls ark_bus_poll() to drive it on.  A restored task will not
 * call a completion callback, because there is none to call.
 *
 * Finished tasks are deliberately not saved: DONE/FAILED/LOST are terminal and
 * their results live in the local result store, which is meant to be read
 * before the restart, not to be a durable log.
 *
 * The layer is compile-gated (ARK_BUS_CHECKPOINT_ENABLED) and needs the task
 * engine; it adds no state to ark_bus_t and no symbol at all when disabled.
 */
#ifndef ARK_BUS_CHECKPOINT_H
#define ARK_BUS_CHECKPOINT_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"

#if ARK_BUS_CHECKPOINT_ENABLED

struct ark_bus;

/*
 * Bytes a checkpoint of the current in-flight table would take.  Stable across
 * a save/restore of the same table, so a caller can size its buffer once.
 */
uint16_t ark_bus_checkpoint_size(const struct ark_bus *bus);

/*
 * Encode the in-flight task table into `buf`.  Up to `cap` bytes are written
 * and the true length is stored in *len.  Returns ARK_BUS_OK, or ARK_BUS_E_NOMEM
 * when `cap` is too small (nothing is written in that case), or E_INVAL.
 */
int ark_bus_checkpoint_save(const struct ark_bus *bus, uint8_t *buf,
                            uint16_t cap, uint16_t *len);

/*
 * Number of tasks a checkpoint blob describes, or a negative ARK_BUS_E_* code
 * when the blob is malformed (E_FORMAT) or truncated (E_NOMEM).
 */
int ark_bus_checkpoint_count(const uint8_t *buf, uint16_t len);

/*
 * Re-adopt the tasks in `buf` into `bus`.  Each task is inserted as QUEUED so
 * the next ark_bus_poll() dispatches it.  Returns the number of tasks adopted,
 * ARK_BUS_E_FULL when the table ran out of room (the ones that fit are
 * adopted), or a negative ARK_BUS_E_* code for a bad blob.
 */
int ark_bus_task_restore(struct ark_bus *bus, const uint8_t *buf, uint16_t len);

#endif /* ARK_BUS_CHECKPOINT_ENABLED */

#endif /* ARK_BUS_CHECKPOINT_H */
