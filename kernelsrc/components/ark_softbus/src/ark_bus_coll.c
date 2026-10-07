/*
 * ark_bus_coll.c - collective communication over the data plane.
 *
 * See ark_bus_coll.h for the contract.  This file is the scheduler: it turns
 * the group operations into sequences of point-to-point stream objects and
 * keeps one object in flight at a time.
 *
 * Two schedules, both from the distributed-training literature:
 *
 *   - ring (all-reduce, all-gather, and the reduce half of reduce):
 *     P ranks hold P equal blocks.  At step s every rank sends block
 *     (rank - s) mod P to its successor and receives block (rank - s - 1) mod P
 *     from its predecessor.  All-gather stores the block it receives;
 *     reduce-scatter adds it.  After P-1 steps all-gather has copied every
 *     block to every rank, and reduce-scatter has left rank r holding the fully
 *     reduced block (r+1) mod P.  All-reduce is reduce-scatter followed by
 *     all-gather, which is the bandwidth-optimal ring schedule Baidu's allreduce
 *     uses.  Source: A. Gibiansky, "Bringing HPC Techniques to Deep Learning"
 *     (2017), the write-up of Baidu's ring allreduce.
 *
 *   - star (gather, and the finish of reduce): each non-root sends one block
 *     to the root, which files it by the index the block carries.  No ordering
 *     is assumed between the arrivals.
 *
 * Robustness note, because it is the one thing that is easy to get subtly
 * wrong: a received block is applied the moment it arrives and recorded in a
 * bitmask, not matched against the current step.  A neighbour can be one step
 * ahead of us (it may send step s+1 as soon as it has received step s from its
 * own predecessor, which does not depend on us), so late blocks must be
 * tolerated rather than treated as errors.  The receive mask makes a duplicate
 * a no-op and lets the scheduler gate each send on the block it needs, without
 * a reordering buffer.
 *
 * Cross-operation note: the tolerance above only spans one *step* of one
 * operation.  It does not span two back-to-back operations: a rank that
 * finishes early would start the next collective and send objects a slower
 * peer -- still inside the previous one -- would drop as belonging to an
 * operation already over, stalling the ring for good.  Every operation
 * therefore opens with a roll call (PH_SYNC): each rank announces the new
 * operation id to all peers and sends no data until every peer has
 * announced the same id.  The roll call is matched by the id alone, so an
 * announcement that arrives while the peer is still finishing the previous
 * operation is filed and consumed when that operation ends.
 */
#include "ark_bus_priv.h"
#include "ark_bus_coll.h"
#include "ark_bus_stream.h"

#if ARK_BUS_COLL_ENABLED

/* Wire phase codes: they say how a received object must be combined. */
#define ARK_BUS_COLL_PH_RS      1u  /* ring reduce-scatter (add)     */
#define ARK_BUS_COLL_PH_AG      2u  /* ring all-gather (store)       */
#define ARK_BUS_COLL_PH_BCAST   3u  /* broadcast forward (store)     */
#define ARK_BUS_COLL_PH_STAR    4u  /* gather-to-root (store)        */
#define ARK_BUS_COLL_PH_SYNC    5u  /* roll call before the data phase */

/* ------------------------------------------------------------------ */
/* Ring index helpers                                                  */
/* ------------------------------------------------------------------ */

/*
 * Ring indices.  `rshift` is 0 for reduce-scatter and for a plain all-gather,
 * and 1 for the all-gather half of all-reduce: reduce-scatter leaves rank r
 * holding the fully reduced block (r+1) mod P, so that phase must start by
 * shipping block (r+1) rather than its own block r.  Everything else about the
 * schedule is identical, which is why one shift covers all three.
 */
static uint8_t ring_out(const ark_bus_coll_op_t *op, uint8_t step)
{
    int d = (int)op->rank + (int)op->rshift - (int)step;
    return (uint8_t)((d + (int)op->nranks * 2) % (int)op->nranks);
}

static uint8_t ring_in(const ark_bus_coll_op_t *op, uint8_t step)
{
    int d = (int)op->rank + (int)op->rshift - (int)step - 1;
    return (uint8_t)((d + (int)op->nranks * 2) % (int)op->nranks);
}

/* ------------------------------------------------------------------ */
/* Element-wise reduction                                              */
/* ------------------------------------------------------------------ */

static uint32_t rd_elem(const uint8_t *p, uint8_t sz)
{
    if (sz == 1u) {
        return p[0];
    }
    if (sz == 2u) {
        return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
    }
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void wr_elem(uint8_t *p, uint8_t sz, uint32_t v)
{
    if (sz == 1u) {
        p[0] = (uint8_t)v;
        return;
    }
    if (sz == 2u) {
        p[0] = (uint8_t)(v >> 8);
        p[1] = (uint8_t)v;
        return;
    }
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t combine_u32(uint8_t rop, uint32_t a, uint32_t b)
{
    if (rop == (uint8_t)ARK_BUS_COLL_MAX) {
        return a > b ? a : b;
    }
    if (rop == (uint8_t)ARK_BUS_COLL_MIN) {
        return a < b ? a : b;
    }
    return a + b;                 /* SUM */
}

/* acc op= in, element by element.  Byte-wise so no alignment is assumed. */
static void coll_reduce_into(const ark_bus_coll_op_t *op, uint8_t *acc,
                             const uint8_t *in, uint32_t n)
{
    uint32_t i;

    if (op->rop == (uint8_t)ARK_BUS_COLL_SUM_F32) {
        for (i = 0; i + 4u <= n; i += 4u) {
            float a, v;
            ark_memcpy(&a, acc + i, 4u);
            ark_memcpy(&v, in + i, 4u);
            a += v;
            ark_memcpy(acc + i, &a, 4u);
        }
        return;
    }

    for (i = 0; i + op->elem_size <= n; i += op->elem_size) {
        uint32_t a = rd_elem(acc + i, op->elem_size);
        uint32_t b = rd_elem(in + i, op->elem_size);
        wr_elem(acc + i, op->elem_size, combine_u32(op->rop, a, b));
    }
}

/* ------------------------------------------------------------------ */
/* Operation end                                                       */
/* ------------------------------------------------------------------ */

static void coll_end(ark_bus_t *bus, int status)
{
    ark_bus_coll_state_t *cs = &bus->coll;
    ark_bus_coll_op_t    *op = &cs->op;
    ark_bus_coll_done_cb_t cb = op->cb;
    void    *user = op->user;
    uint8_t  kind = op->kind;

    /* The stream receive path stays ours: ark_bus_stream_on_recv() routes an
     * application's callback to cs->fallback_cb while we hold it, and
     * coll_dispatch forwards non-collective objects there.  Restoring it here
     * would lose a peer's next-operation message already queued behind this
     * batch.  See ark_bus_stream_on_recv(). */
    ark_memset(op, 0, sizeof(*op));

    if (status == ARK_BUS_OK) {
        cs->ops_completed++;
    } else {
        cs->ops_failed++;
    }
    if (cb) {
        cb(status, kind, user);
    }
}

/* ------------------------------------------------------------------ */
/* Sending one collective object                                       */
/* ------------------------------------------------------------------ */

static void coll_pump(ark_bus_t *bus);

static void coll_send_done(int status, uint16_t xid, void *user)
{
    ark_bus_t            *bus = (ark_bus_t *)user;
    ark_bus_coll_op_t    *op  = &bus->coll.op;

    if (!op->used || op->xid != xid) {
        return;                    /* stale completion from an earlier step */
    }
    if (status != ARK_BUS_OK) {
        coll_end(bus, status);
        return;
    }
    op->send_busy = 0u;
    coll_pump(bus);
}

/* Assemble the header plus `len` payload bytes and hand them to the stream
 * layer.  On success marks the operation's send in flight and records the
 * transfer id.  `index` is the block this object carries. */
static int coll_send_object(ark_bus_t *bus, uint32_t dst, uint8_t index,
                            const void *data, uint32_t len)
{
    ark_bus_coll_state_t *cs = &bus->coll;
    ark_bus_coll_op_t    *op = &cs->op;
    uint8_t  *s = cs->stage;
    uint16_t  xid = 0;
    int       rc;

    if ((uint32_t)ARK_BUS_COLL_HDR + len > (uint32_t)ARK_BUS_COLL_STAGE_MAX) {
        return ARK_BUS_E_NOMEM;
    }

    ark_wr16(s, (uint16_t)ARK_BUS_COLL_MAGIC);
    s[2] = op->kind;
    s[3] = op->phase;
    s[4] = index;
    s[5] = op->nranks;
    ark_wr16(s + 6, op->op_id);
    if (len != 0u) {
        ark_memcpy(s + ARK_BUS_COLL_HDR, data, len);
    }

    rc = ark_bus_stream_send(bus, dst, s,
                             (uint32_t)ARK_BUS_COLL_HDR + len,
                             coll_send_done, bus, &xid);
    if (rc == ARK_BUS_OK) {
        op->xid      = xid;
        op->send_busy = 1u;
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/* Phase transitions                                                   */
/* ------------------------------------------------------------------ */

static void coll_phase_done(ark_bus_t *bus);

/* ------------------------------------------------------------------ */
/* Ring engine (reduce-scatter and all-gather)                         */
/* ------------------------------------------------------------------ */

static void coll_ring_pump(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    while (op->step < (uint8_t)(op->nranks - 1u)) {
        uint8_t  out;
        uint32_t dst;

        if (op->send_busy) {
            return;
        }

        /* The block we are about to send is the one we received on the
         * previous step and accumulated into; wait for it before sending. */
        if (op->step > 0u) {
            uint8_t need = ring_out(op, op->step);   /* == ring_in(step-1) */
            if ((op->recv_mask & (1u << need)) == 0u) {
                return;
            }
        }

        out = ring_out(op, op->step);
        dst = op->group.ids[(op->rank + 1) % op->nranks];
        if (coll_send_object(bus, dst, out,
                             (uint8_t *)op->rbuf + (uint32_t)out * op->blk,
                             op->blk) != ARK_BUS_OK) {
            coll_end(bus, ARK_BUS_E_NOMEM);
            return;
        }
        op->step++;
        return;                    /* wait for the send to complete */
    }

    /* Every send is out; the phase ends when the last block has arrived and the
     * last object has been acknowledged.  Waiting for the ACK matters: the
     * transfer still owns the shared `stage` buffer until it completes, and a
     * phase change refills that buffer -- an in-flight retransmit would then
     * deliver the *next* object's bytes under the *old* transfer id, so the peer
     * would never see this phase's last block.  The peer's stream layer answers
     * ACKs on its own poll, so waiting here cannot deadlock. */
    if (op->send_busy) {
        return;
    }
    {
        uint8_t last = ring_in(op, (uint8_t)(op->nranks - 2u));
        if ((op->recv_mask & (1u << last)) == 0u) {
            return;
        }
    }
    coll_phase_done(bus);
}

/* ------------------------------------------------------------------ */
/* Broadcast engine                                                    */
/* ------------------------------------------------------------------ */

static void coll_bcast_pump(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;
    uint8_t last = (uint8_t)((op->root + op->nranks - 1u) % op->nranks);

    if (op->rank == op->root) {
        if (!op->sent) {
            uint32_t dst = op->group.ids[(op->rank + 1) % op->nranks];
            if (coll_send_object(bus, dst, 0u, op->bbuf, op->total) !=
                ARK_BUS_OK) {
                coll_end(bus, ARK_BUS_E_NOMEM);
                return;
            }
            op->sent = 1u;
            return;
        }
    } else {
        if ((op->recv_mask & 1u) == 0u) {
            return;                /* wait for the object from our predecessor */
        }
        if (op->rank != last && !op->sent) {
            uint32_t dst = op->group.ids[(op->rank + 1) % op->nranks];
            if (coll_send_object(bus, dst, 0u, op->bbuf, op->total) !=
                ARK_BUS_OK) {
                coll_end(bus, ARK_BUS_E_NOMEM);
                return;
            }
            op->sent = 1u;
            return;
        }
    }

    if (op->send_busy) {
        return;
    }
    coll_phase_done(bus);
}

/* ------------------------------------------------------------------ */
/* Star engine (gather, and the finish of reduce)                      */
/* ------------------------------------------------------------------ */

static void coll_star_pump(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    if (op->rank == op->root) {
        if (op->recv_count >= (uint8_t)(op->nranks - 1u)) {
            coll_phase_done(bus);
        }
        return;                    /* otherwise keep waiting for blocks */
    }

    if (!op->sent) {
        uint8_t     index;
        const void *data;
        uint32_t    dst = op->group.ids[op->root];

        if (op->kind == (uint8_t)ARK_BUS_COLL_GATHER) {
            index = op->rank;
            data  = op->sbuf;
        } else {
            /* reduce: this rank holds the reduced block (rank+1) mod P */
            index = (uint8_t)((op->rank + 1) % op->nranks);
            data  = (uint8_t *)op->rbuf + (uint32_t)index * op->blk;
        }
        if (coll_send_object(bus, dst, index, data, op->blk) != ARK_BUS_OK) {
            coll_end(bus, ARK_BUS_E_NOMEM);
            return;
        }
        op->sent = 1u;
        return;
    }

    if (op->send_busy) {
        return;
    }
    coll_phase_done(bus);
}

/* ------------------------------------------------------------------ */
/* Scheduler                                                           */
/* ------------------------------------------------------------------ */

static void coll_phase_done(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    switch (op->kind) {
    case ARK_BUS_COLL_ALLREDUCE:
        if (op->phase == ARK_BUS_COLL_PH_RS) {
            op->phase     = ARK_BUS_COLL_PH_AG;
            op->step      = 0u;
            op->send_busy = 0u;
            op->recv_mask = 0u;
            op->rshift    = 1u;   /* reduced block sits at (r+1) mod P */
            coll_pump(bus);
        } else {
            coll_end(bus, ARK_BUS_OK);
        }
        break;

    case ARK_BUS_COLL_REDUCE:
        if (op->phase == ARK_BUS_COLL_PH_RS) {
            op->phase      = ARK_BUS_COLL_PH_STAR;
            op->send_busy  = 0u;
            op->sent       = 0u;
            op->recv_count = 0u;
            op->recv_mask  = 0u;
            coll_pump(bus);
        } else {
            coll_end(bus, ARK_BUS_OK);
        }
        break;

    default:
        coll_end(bus, ARK_BUS_OK);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Roll call (barrier) before the data phases                          */
/* ------------------------------------------------------------------ */

/* True once every peer has announced this operation's id.  Our own bit is not
 * expected: we never send a roll call to ourselves. */
static int coll_sync_ready(const ark_bus_t *bus)
{
    const ark_bus_coll_state_t *cs = &bus->coll;
    const ark_bus_coll_op_t    *op = &cs->op;
    uint32_t need = 0u;
    uint8_t  r;

    if (!cs->sync_used || cs->sync_opid != op->op_id) {
        return 0;
    }
    for (r = 0; r < op->nranks; r++) {
        if (r != op->rank) {
            need |= (uint32_t)1u << r;
        }
    }
    return (cs->sync_mask & need) == need;
}

/* The kind's first data phase.  recv_mask is left alone: all-gather and
 * gather have already seeded it with the block they hold themselves. */
static void coll_enter_data(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    op->send_busy = 0u;
    op->step      = 0u;
    switch (op->kind) {
    case ARK_BUS_COLL_ALLREDUCE:
    case ARK_BUS_COLL_REDUCE:
        op->phase = ARK_BUS_COLL_PH_RS;
        break;
    case ARK_BUS_COLL_BROADCAST:
        op->phase = ARK_BUS_COLL_PH_BCAST;
        break;
    case ARK_BUS_COLL_ALLGATHER:
        op->phase = ARK_BUS_COLL_PH_AG;
        break;
    case ARK_BUS_COLL_GATHER:
        op->phase = ARK_BUS_COLL_PH_STAR;
        break;
    default:
        coll_end(bus, ARK_BUS_E_STATE);
        return;
    }
    coll_pump(bus);
}

static void coll_sync_pump(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    while (op->sync_next < op->nranks) {
        uint8_t  peer;
        uint32_t dst;

        if (op->send_busy) {
            return;                /* one object in flight at a time */
        }
        peer = op->sync_next++;
        if (peer == op->rank) {
            continue;              /* no need to announce to ourselves */
        }
        dst = op->group.ids[peer];
        if (coll_send_object(bus, dst, op->rank, NULL, 0u) != ARK_BUS_OK) {
            coll_end(bus, ARK_BUS_E_NOMEM);
            return;
        }
        return;                    /* resumed from coll_send_done() */
    }

    if (!coll_sync_ready(bus)) {
        return;                    /* wait for the peers to check in */
    }
    coll_enter_data(bus);
}

static void coll_pump(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    if (!op->used) {
        return;
    }
    op->started_ms = ark_bus_tick_ms();   /* any call here is progress */

    switch (op->phase) {
    case ARK_BUS_COLL_PH_SYNC:
        coll_sync_pump(bus);
        break;
    case ARK_BUS_COLL_PH_RS:
    case ARK_BUS_COLL_PH_AG:
        coll_ring_pump(bus);
        break;
    case ARK_BUS_COLL_PH_BCAST:
        coll_bcast_pump(bus);
        break;
    case ARK_BUS_COLL_PH_STAR:
        coll_star_pump(bus);
        break;
    default:
        coll_end(bus, ARK_BUS_E_STATE);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Inbound object dispatch                                             */
/* ------------------------------------------------------------------ */

static void coll_dispatch(ark_bus_t *bus, uint32_t from, const uint8_t *data,
                          uint16_t len, void *user)
{
    ark_bus_coll_state_t *cs = &bus->coll;
    ark_bus_coll_op_t    *op = &cs->op;

    (void)user;


    if (len >= (uint16_t)ARK_BUS_COLL_HDR &&
        ark_rd16(data) == (uint16_t)ARK_BUS_COLL_MAGIC) {
        uint16_t op_id = ark_rd16(data + 6);
        uint8_t  phase = data[3];
        uint8_t  index = data[4];
        const uint8_t *p    = data + ARK_BUS_COLL_HDR;
        uint16_t       plen = (uint16_t)(len - ARK_BUS_COLL_HDR);

        /* The roll call is answered before the operation gate: a peer may
         * announce the operation we have not started yet (or must be able to
         * ignore one that belongs to an operation already over). */
        if (phase == ARK_BUS_COLL_PH_SYNC) {
            if (!cs->sync_used || cs->sync_opid != op_id) {
                if (cs->sync_used &&
                    (int16_t)(op_id - cs->sync_opid) < 0) {
                    return;        /* an earlier operation's roll call */
                }
                cs->sync_opid = op_id;
                cs->sync_mask = 0u;
                cs->sync_used = 1u;
            }
            if (index < 32u) {
                cs->sync_mask |= (uint32_t)1u << index;
            }
            coll_pump(bus);
            return;
        }

        if (!op->used || op_id != op->op_id) {
            return;                /* belongs to an operation already over */
        }

        switch (phase) {
        case ARK_BUS_COLL_PH_RS:
            if (index >= op->nranks || plen != op->blk) {
                coll_end(bus, ARK_BUS_E_FORMAT);
                return;
            }
            if ((op->recv_mask & (1u << index)) != 0u) {
                return;            /* duplicate: idempotent, ignore */
            }
            coll_reduce_into(op, (uint8_t *)op->rbuf +
                                   (uint32_t)index * op->blk, p, op->blk);
            op->recv_mask |= (1u << index);
            break;

        case ARK_BUS_COLL_PH_AG:
            if (index >= op->nranks || plen != op->blk) {
                coll_end(bus, ARK_BUS_E_FORMAT);
                return;
            }
            if ((op->recv_mask & (1u << index)) != 0u) {
                return;
            }
            ark_memcpy((uint8_t *)op->rbuf + (uint32_t)index * op->blk,
                       p, op->blk);
            op->recv_mask |= (1u << index);
            break;

        case ARK_BUS_COLL_PH_BCAST:
            if (!op->bbuf || plen != op->total || index != 0u) {
                coll_end(bus, ARK_BUS_E_FORMAT);
                return;
            }
            ark_memcpy(op->bbuf, p, op->total);
            op->recv_mask |= 1u;
            break;

        case ARK_BUS_COLL_PH_STAR:
            if (op->rank != op->root) {
                return;            /* only the root collects */
            }
            if (index >= op->nranks || plen != op->blk) {
                coll_end(bus, ARK_BUS_E_FORMAT);
                return;
            }
            if ((op->recv_mask & (1u << index)) != 0u) {
                return;
            }
            ark_memcpy((uint8_t *)op->rbuf + (uint32_t)index * op->blk,
                       p, op->blk);
            op->recv_mask |= (1u << index);
            op->recv_count++;
            break;

        default:
            return;
        }

        coll_pump(bus);
        return;
    }

    /* Not ours: an application's own stream object. */
    if (cs->fallback_cb) {
        cs->fallback_cb(bus, from, data, len, cs->fallback_user);
    }
}

/* ------------------------------------------------------------------ */
/* Wiring                                                              */
/* ------------------------------------------------------------------ */

void ark_bus_coll_init(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    bus->coll.next_op_id = 1u;
    /* Nothing is claimed until an operation starts, so a plain data-plane
     * user sees no change until it joins a group. */
    bus->coll.fallback_cb   = bus->stream.recv_cb;
    bus->coll.fallback_user = bus->stream.recv_user;
    bus->coll.fallback_set  = 1u;
}

void ark_bus_coll_tick(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op;

    if (!bus) {
        return;
    }
    op = &bus->coll.op;
    if (!op->used) {
        return;
    }
    if ((uint32_t)(ark_bus_tick_ms() - op->started_ms) >
        (uint32_t)ARK_BUS_COLL_TIMEOUT_MS) {
        coll_end(bus, ARK_BUS_E_TIMEOUT);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/* Resolve the calling node's rank, or 0xFF when it is not a member. */
static uint8_t group_rank(const ark_bus_t *bus,
                          const ark_bus_coll_group_t *grp)
{
    uint8_t i;
    uint8_t j;

    if (grp->nranks < 2u || grp->nranks > (uint8_t)ARK_BUS_COLL_MAX_RANKS) {
        return 0xFFu;
    }
    for (i = 0; i < grp->nranks; i++) {
        for (j = (uint8_t)(i + 1u); j < grp->nranks; j++) {
            if (grp->ids[i] == grp->ids[j]) {
                return 0xFFu;      /* ranks must be distinct */
            }
        }
        if (grp->ids[i] == ARK_BUS_ID_BROADCAST) {
            return 0xFFu;          /* 0 is the broadcast sentinel, not a node */
        }
    }
    for (i = 0; i < grp->nranks; i++) {
        if (grp->ids[i] == bus->self_id) {
            return i;
        }
    }
    return 0xFFu;
}

static int valid_reduce(ark_bus_coll_rop_t op, uint8_t elem_size)
{
    if (op == ARK_BUS_COLL_SUM_F32) {
        return elem_size == 4u;
    }
    if (op == ARK_BUS_COLL_SUM || op == ARK_BUS_COLL_MAX ||
        op == ARK_BUS_COLL_MIN) {
        return elem_size == 1u || elem_size == 2u || elem_size == 4u;
    }
    return 0;
}

/* Common setup: validate, claim the receiver, arm the state machine.  The
 * caller has already checked every buffer it needs. */
static int coll_begin(ark_bus_t *bus, uint8_t kind,
                      const ark_bus_coll_group_t *grp, uint8_t root,
                      ark_bus_coll_rop_t op, uint8_t elem_size,
                      void *sbuf, void *rbuf, void *bbuf,
                      uint32_t total, uint32_t blk,
                      ark_bus_coll_done_cb_t cb, void *user)
{
    ark_bus_coll_state_t *cs = &bus->coll;
    ark_bus_coll_op_t    *st = &cs->op;
    uint8_t rank = group_rank(bus, grp);

    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }
    if (rank == 0xFFu) {
        return ARK_BUS_E_INVAL;
    }
    if (st->used) {
        return ARK_BUS_E_BUSY;     /* one collective at a time per bus */
    }

    ark_memset(st, 0, sizeof(*st));
    st->used      = 1u;
    st->kind      = kind;
    st->nranks    = grp->nranks;
    st->rank      = rank;
    st->root      = root;
    st->rop       = (uint8_t)op;
    st->elem_size = elem_size;
    st->total     = total;
    st->blk       = blk;
    st->sbuf      = sbuf;
    st->rbuf      = rbuf;
    st->bbuf      = bbuf;
    st->cb        = cb;
    st->user      = user;
    st->group     = *grp;

    cs->next_op_id++;
    if (cs->next_op_id == 0u) {
        cs->next_op_id = 1u;
    }
    st->op_id = cs->next_op_id;

    if (!cs->fallback_set) {
        cs->fallback_cb   = bus->stream.recv_cb;
        cs->fallback_user = bus->stream.recv_user;
        cs->fallback_set  = 1u;
    }
    if (!cs->owns_recv) {
        ark_bus_stream_on_recv(bus, coll_dispatch, bus);
        cs->owns_recv = 1u;
    }

    st->started_ms = ark_bus_tick_ms();
    /* Keep a roll call already filed for this id; only discard one that
     * belongs to an earlier operation. */
    if (!cs->sync_used || cs->sync_opid != st->op_id) {
        cs->sync_opid = st->op_id;
        cs->sync_mask = 0u;
        cs->sync_used = 1u;
    }
    return ARK_BUS_OK;
}

/* Arm the roll call and let it run.  The public calls below all end here. */
static void coll_start(ark_bus_t *bus)
{
    ark_bus_coll_op_t *op = &bus->coll.op;

    op->sync_next = 0u;
    op->phase     = ARK_BUS_COLL_PH_SYNC;
    coll_pump(bus);
}

int ark_bus_coll_allreduce(ark_bus_t *bus, const ark_bus_coll_group_t *grp,
                           ark_bus_coll_rop_t op, uint8_t elem_size,
                           const void *sbuf, void *rbuf, uint32_t nbytes,
                           ark_bus_coll_done_cb_t cb, void *user)
{
    uint32_t blk;
    int      rc;

    if (!grp || !sbuf || !rbuf || nbytes == 0u ||
        !valid_reduce(op, elem_size)) {
        return ARK_BUS_E_INVAL;
    }
    if (nbytes % grp->nranks != 0u) {
        return ARK_BUS_E_INVAL;
    }
    blk = nbytes / grp->nranks;
    if ((uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_COLL_STAGE_MAX ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_MAX_BYTES ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_RX_MAX) {
        return ARK_BUS_E_NOMEM;
    }

    if (sbuf != rbuf) {
        ark_memcpy(rbuf, sbuf, nbytes);
    }
    rc = coll_begin(bus, (uint8_t)ARK_BUS_COLL_ALLREDUCE, grp, 0u, op,
                    elem_size, (void *)sbuf, rbuf, NULL, nbytes, blk, cb, user);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    coll_start(bus);
    return ARK_BUS_OK;
}

int ark_bus_coll_broadcast(ark_bus_t *bus, const ark_bus_coll_group_t *grp,
                           uint8_t root, void *buf, uint32_t nbytes,
                           ark_bus_coll_done_cb_t cb, void *user)
{
    int rc;

    if (!grp || !buf || nbytes == 0u || root >= grp->nranks) {
        return ARK_BUS_E_INVAL;
    }
    if ((uint32_t)ARK_BUS_COLL_HDR + nbytes >
            (uint32_t)ARK_BUS_COLL_STAGE_MAX ||
        (uint32_t)ARK_BUS_COLL_HDR + nbytes >
            (uint32_t)ARK_BUS_STREAM_RX_MAX) {
        return ARK_BUS_E_NOMEM;
    }

    rc = coll_begin(bus, (uint8_t)ARK_BUS_COLL_BROADCAST, grp, root,
                    ARK_BUS_COLL_SUM, 1u, NULL, NULL, buf, nbytes, nbytes,
                    cb, user);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    coll_start(bus);
    return ARK_BUS_OK;
}

int ark_bus_coll_reduce(ark_bus_t *bus, const ark_bus_coll_group_t *grp,
                        uint8_t root, ark_bus_coll_rop_t op, uint8_t elem_size,
                        const void *sbuf, void *rbuf, uint32_t nbytes,
                        ark_bus_coll_done_cb_t cb, void *user)
{
    uint32_t blk;
    int      rc;

    if (!grp || !rbuf || nbytes == 0u || root >= grp->nranks ||
        !valid_reduce(op, elem_size)) {
        return ARK_BUS_E_INVAL;
    }
    if (nbytes % grp->nranks != 0u) {
        return ARK_BUS_E_INVAL;
    }
    /* Non-root ranks still need a working buffer for reduce-scatter. */
    if (!sbuf) {
        sbuf = rbuf;
    }
    blk = nbytes / grp->nranks;
    if ((uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_COLL_STAGE_MAX ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_MAX_BYTES ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_RX_MAX) {
        return ARK_BUS_E_NOMEM;
    }

    if (sbuf != rbuf) {
        ark_memcpy(rbuf, sbuf, nbytes);
    }
    rc = coll_begin(bus, (uint8_t)ARK_BUS_COLL_REDUCE, grp, root, op,
                    elem_size, (void *)sbuf, rbuf, NULL, nbytes, blk, cb, user);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    coll_start(bus);
    return ARK_BUS_OK;
}

int ark_bus_coll_allgather(ark_bus_t *bus, const ark_bus_coll_group_t *grp,
                           uint32_t blk, const void *sbuf, void *rbuf,
                           ark_bus_coll_done_cb_t cb, void *user)
{
    uint8_t rank;
    int     rc;

    if (!grp || !sbuf || !rbuf || blk == 0u) {
        return ARK_BUS_E_INVAL;
    }
    if ((uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_COLL_STAGE_MAX ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_MAX_BYTES ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_RX_MAX) {
        return ARK_BUS_E_NOMEM;
    }
    rank = group_rank(bus, grp);
    if (rank == 0xFFu) {
        return ARK_BUS_E_INVAL;
    }

    /* Seed our own block; the ring distributes the rest. */
    ark_memcpy((uint8_t *)rbuf + (uint32_t)rank * blk, sbuf, blk);

    rc = coll_begin(bus, (uint8_t)ARK_BUS_COLL_ALLGATHER, grp, 0u,
                    ARK_BUS_COLL_SUM, 1u, (void *)sbuf, rbuf, NULL,
                    (uint32_t)grp->nranks * blk, blk, cb, user);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    bus->coll.op.recv_mask = (uint32_t)1u << rank;
    coll_start(bus);
    return ARK_BUS_OK;
}

int ark_bus_coll_gather(ark_bus_t *bus, const ark_bus_coll_group_t *grp,
                        uint8_t root, uint32_t blk, const void *sbuf,
                        void *rbuf, ark_bus_coll_done_cb_t cb, void *user)
{
    uint8_t rank;
    int     rc;

    if (!grp || !sbuf || blk == 0u || root >= grp->nranks) {
        return ARK_BUS_E_INVAL;
    }
    if ((uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_COLL_STAGE_MAX ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_MAX_BYTES ||
        (uint32_t)ARK_BUS_COLL_HDR + blk > (uint32_t)ARK_BUS_STREAM_RX_MAX) {
        return ARK_BUS_E_NOMEM;
    }
    rank = group_rank(bus, grp);
    if (rank == 0xFFu) {
        return ARK_BUS_E_INVAL;
    }
    if (rank == root) {
        if (!rbuf) {
            return ARK_BUS_E_INVAL;
        }
        ark_memcpy((uint8_t *)rbuf + (uint32_t)root * blk, sbuf, blk);
    }

    rc = coll_begin(bus, (uint8_t)ARK_BUS_COLL_GATHER, grp, root,
                    ARK_BUS_COLL_SUM, 1u, (void *)sbuf, rbuf, NULL,
                    (uint32_t)grp->nranks * blk, blk, cb, user);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    if (rank == root) {
        bus->coll.op.recv_mask = (uint32_t)1u << root;
    }
    coll_start(bus);
    return ARK_BUS_OK;
}

int ark_bus_coll_pending(const ark_bus_t *bus)
{
    return (bus && bus->coll.op.used) ? 1 : 0;
}

void ark_bus_coll_set_stream_fallback(ark_bus_t *bus,
                                      ark_bus_stream_recv_cb_t cb, void *user)
{
    if (!bus) {
        return;
    }
    bus->coll.fallback_cb   = cb;
    bus->coll.fallback_user = user;
    bus->coll.fallback_set  = 1u;
    /* On a bus that has never run a collective this installs the callback on
     * the stream; once we own the receive path it just updates the forward
     * target (ark_bus_stream_on_recv() does the right thing either way). */
    ark_bus_stream_on_recv(bus, cb, user);
}

void ark_bus_coll_get_stats(const ark_bus_t *bus,
                            uint32_t *completed, uint32_t *failed)
{
    if (!bus) {
        return;
    }
    if (completed) { *completed = bus->coll.ops_completed; }
    if (failed)    { *failed    = bus->coll.ops_failed;    }
}

#endif /* ARK_BUS_COLL_ENABLED */
