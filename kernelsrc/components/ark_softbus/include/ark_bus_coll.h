/*
 * ark_bus_coll.h - collective communication (the group-operations layer).
 *
 * The data plane (ark_bus_stream.h) moves one object between two peers.  This
 * layer composes those point-to-point moves into the operations a group of
 * ranks needs to act as one machine: all-reduce, broadcast, reduce, all-gather
 * and gather.  It is the same vocabulary MPI and NCCL give a distributed job,
 * cut down to what a constrained node can actually run:
 *
 *   - a group is an explicit, ordered list of node ids; a node's rank is its
 *     index in that list (0-based).  There is no communicator object to create
 *     and no context to negotiate -- every member must call the same operation
 *     with the same group, which is exactly the contract NCCL states ("called
 *     for each rank ... using the same count and the same datatype");
 *   - one operation runs at a time per bus, is driven by ark_bus_poll(), and
 *     reports completion through a callback.  Nothing blocks and nothing is
 *     allocated: the ranks, the work buffers and one fixed staging buffer all
 *     live in ark_bus_t;
 *   - the ring operations use the bandwidth-optimal schedule that Baidu
 *     popularised for distributed training (reduce-scatter then all-gather,
 *     each moving (P-1)/P of the buffer per rank); broadcast walks the ring,
 *     gather/reduce finish at the root in a star.
 *
 * The payloads ride inside data-plane objects (ARK_BUS_MSG_STREAM frames), so
 * the wire format is untouched: a collective message is simply a stream object
 * whose first ARK_BUS_COLL_HDR bytes are a collective header.  A node that has
 * its own use for the stream layer keeps it: an inbound object that is not a
 * collective message is handed to the stream receive callback that was in
 * place when the operation started (see ark_bus_coll_set_stream_fallback).
 *
 * What it deliberately does NOT do: no pipelining across steps (each ring step
 * is one object in flight, so latency dominates on a long link -- a sliding
 * window is the follow-on), no tree/hierarchical algorithms for large P, no
 * non-power-of-two hosts, no fault recovery past a single operation timeout
 * (a dead rank fails the operation with E_TIMEOUT; re-forming the group is the
 * caller's job), and no in-place aliasing where it is not spelled out.  An
 * operation on a group of one rank is rejected with E_INVAL rather than
 * silently no-op'd.
 *
 * Reduction, stated because it is easy to get wrong: the built-in operators
 * combine fixed-width little-endian-independent *numeric* values (1, 2 or 4
 * bytes, treated as unsigned, or IEEE-754 binary32 for SUM_F32).  Floating
 * point addition is not associative, so a SUM_F32 result depends on the ring
 * order; that is fine for gradient averaging but must not be read as bit-exact
 * against a reference computed in another order.
 */
#ifndef ARK_BUS_COLL_H
#define ARK_BUS_COLL_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus_protocol.h"
#include "ark_bus_stream.h"

#if ARK_BUS_COLL_ENABLED

struct ark_bus;

/* Size of the collective header that prefixes every collective object. */
#define ARK_BUS_COLL_HDR         8u

/* Magic that marks a stream object as a collective message.  A user stream
 * object whose first two bytes happen to equal this is the one collision the
 * layer cannot tell apart; the trailing op id makes it vanishingly unlikely. */
#define ARK_BUS_COLL_MAGIC       0xC011u

/* Which collective to run.  Carried inside the header so both ends agree. */
typedef enum {
    ARK_BUS_COLL_ALLREDUCE = 1,  /* reduce across ranks, everyone keeps it */
    ARK_BUS_COLL_BROADCAST = 2,  /* root's buffer copied to every rank      */
    ARK_BUS_COLL_REDUCE    = 3,  /* reduce across ranks, only the root keeps */
    ARK_BUS_COLL_ALLGATHER = 4,  /* every rank's block to everyone          */
    ARK_BUS_COLL_GATHER    = 5   /* every rank's block to the root          */
} ark_bus_coll_kind_t;

/* Element-wise reduction operators (see the header note on associativity). */
typedef enum {
    ARK_BUS_COLL_SUM     = 0,    /* unsigned integer sum                    */
    ARK_BUS_COLL_MAX     = 1,    /* unsigned integer maximum                */
    ARK_BUS_COLL_MIN     = 2,    /* unsigned integer minimum                */
    ARK_BUS_COLL_SUM_F32 = 3     /* IEEE-754 binary32 sum (elem_size must 4) */
} ark_bus_coll_rop_t;

/*
 * A group of ranks, in rank order.  `nranks` is the group size and `ids[r]` is
 * the node id of rank r; the calling node must appear exactly once.  The same
 * array must be passed by every member of the group.
 */
typedef struct {
    uint8_t  nranks;
    uint32_t ids[ARK_BUS_COLL_MAX_RANKS];
} ark_bus_coll_group_t;

/*
 * Called once per operation from inside ark_bus_poll().  `status` is
 * ARK_BUS_OK, or a negative ARK_BUS_E_* code: E_TIMEOUT when a peer stopped
 * responding, E_NOMEM/E_INVAL when a peer refused the message it received.
 * `op` is the ark_bus_coll_kind_t that finished.  The result is already in the
 * caller's buffer: `rbuf` for all-gather/gather/reduce/all-reduce, `buf` for
 * broadcast.
 */
typedef void (*ark_bus_coll_done_cb_t)(int status, uint8_t op, void *user);

/* State embedded in ark_bus_t (guarded by ARK_BUS_COLL_ENABLED there). */
typedef struct {
    uint8_t  used;
    uint8_t  kind;        /* ark_bus_coll_kind_t                     */
    uint8_t  phase;       /* ARK_BUS_COLL_PH_* below                 */
    uint8_t  nranks;
    uint8_t  rank;        /* this node's rank in the group           */
    uint8_t  root;        /* root rank, for broadcast/reduce/gather  */
    uint8_t  elem_size;   /* 1/2/4 for integer ops, 4 for SUM_F32    */
    uint8_t  rop;         /* ark_bus_coll_rop_t                      */

    uint8_t  step;        /* ring step, 0 .. nranks-2                */
    uint8_t  sync_next;   /* roll call: next peer rank to announce    */
    uint8_t  sync_round;  /* roll call round: 0 before data, 1 mid    */
    uint32_t sync_ms;     /* roll call: when we last announced        */
    uint8_t  rshift;      /* ring index offset: 0, or 1 for allreduce */
    uint8_t  send_busy;   /* an outgoing object is still in flight   */
    uint8_t  sent;        /* star/broadcast: our one send is done     */
    uint8_t  recv_count;  /* star root: how many blocks have arrived */
    uint16_t xid;         /* outstanding stream transfer id          */
    uint16_t op_id;       /* identifies this operation on the wire   */

    uint32_t total;       /* bytes in the whole buffer               */
    uint32_t blk;         /* bytes per rank block                    */
    uint32_t recv_mask;   /* reduce/one-shot: bit i = block i arrived */
    uint32_t gmask;       /* gather phase arrivals (all-gather / the
                           * star finish of reduce); kept apart from
                           * recv_mask so a block arriving before the
                           * mid-operation barrier is not lost when the
                           * phase's index space changes             */

    uint32_t started_ms;
    ark_bus_coll_group_t group;
    void    *sbuf;        /* borrowed: all-gather/gather input block */
    void    *rbuf;        /* borrowed: result / working buffer       */
    void    *bbuf;        /* borrowed: broadcast buffer (in place)   */

    ark_bus_coll_done_cb_t cb;
    void                  *user;
} ark_bus_coll_op_t;

typedef struct {
    ark_bus_coll_op_t op;
    uint8_t           stage[ARK_BUS_COLL_STAGE_MAX];
    uint16_t          next_op_id;

    /* Roll call that opens every operation: the running operation id and
     * the set of peer ranks that have announced it (see ark_bus_coll.c). */
    uint32_t sync_mask;
    uint16_t sync_opid;
    uint8_t  sync_used;
    uint8_t  sync_round;  /* which roll-call round sync_mask holds      */
    uint8_t  owns_recv;   /* we hold the stream receive path for good   */

    /* The stream callback that was installed when the operation began, so an
     * application's own object transfer keeps working alongside collectives. */
    ark_bus_stream_recv_cb_t fallback_cb;
    void                    *fallback_user;
    uint8_t                  fallback_set;

    uint32_t ops_completed;
    uint32_t ops_failed;
} ark_bus_coll_state_t;

/* ------------------------------------------------------------------ */
/* Operations.  All return ARK_BUS_OK when the operation was started,  */
/* or a negative ARK_BUS_E_* code when it could not be.  A started     */
/* operation is asynchronous: progress and completion come from        */
/* ark_bus_poll() and the callback.                                    */
/* ------------------------------------------------------------------ */

/*
 * out[i] = reduce over ranks of in[i], for every i, on every rank.
 * `nbytes` must be a non-zero multiple of the group size.
 */
int ark_bus_coll_allreduce(struct ark_bus *bus, const ark_bus_coll_group_t *grp,
                           ark_bus_coll_rop_t op, uint8_t elem_size,
                           const void *sbuf, void *rbuf, uint32_t nbytes,
                           ark_bus_coll_done_cb_t cb, void *user);

/* Copy the root's `buf` to every rank's `buf` (in place, `nbytes` bytes). */
int ark_bus_coll_broadcast(struct ark_bus *bus, const ark_bus_coll_group_t *grp,
                           uint8_t root, void *buf, uint32_t nbytes,
                           ark_bus_coll_done_cb_t cb, void *user);

/*
 * out[i] = reduce over ranks of in[i]; only the root's `rbuf` is meaningful.
 * `nbytes` must be a non-zero multiple of the group size.
 */
int ark_bus_coll_reduce(struct ark_bus *bus, const ark_bus_coll_group_t *grp,
                        uint8_t root, ark_bus_coll_rop_t op, uint8_t elem_size,
                        const void *sbuf, void *rbuf, uint32_t nbytes,
                        ark_bus_coll_done_cb_t cb, void *user);

/*
 * Gather every rank's `blk`-byte block into `rbuf` (nranks*blk bytes) on every
 * rank, block r at offset r*blk.  `sbuf` may alias `rbuf + rank*blk`.
 */
int ark_bus_coll_allgather(struct ark_bus *bus, const ark_bus_coll_group_t *grp,
                           uint32_t blk, const void *sbuf, void *rbuf,
                           ark_bus_coll_done_cb_t cb, void *user);

/*
 * Gather every rank's `blk`-byte block into the root's `rbuf` (nranks*blk
 * bytes), block r at offset r*blk.  `rbuf` is read only on the root.
 */
int ark_bus_coll_gather(struct ark_bus *bus, const ark_bus_coll_group_t *grp,
                        uint8_t root, uint32_t blk, const void *sbuf,
                        void *rbuf, ark_bus_coll_done_cb_t cb, void *user);

/* 1 while an operation is in progress on this bus, else 0. */
int ark_bus_coll_pending(const struct ark_bus *bus);

/*
 * Register this node's callback for stream objects that are not collective
 * messages, so an application's own object transfers keep working alongside
 * collectives.  While an operation is running the collective layer owns the
 * stream receive callback and chains unrecognised objects here; when idle the
 * callback is installed directly.  Passing NULL removes it.
 */
void ark_bus_coll_set_stream_fallback(struct ark_bus *bus,
                                      ark_bus_stream_recv_cb_t cb, void *user);

/* Counters: completed and failed operations.  Any NULL pointer is ignored. */
void ark_bus_coll_get_stats(const struct ark_bus *bus,
                            uint32_t *completed, uint32_t *failed);

#endif /* ARK_BUS_COLL_ENABLED */

#endif /* ARK_BUS_COLL_H */
