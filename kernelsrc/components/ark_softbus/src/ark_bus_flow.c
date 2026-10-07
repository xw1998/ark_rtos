/*
 * ark_bus_flow.c - flow control and backpressure (M4).  See ark_bus_flow.h for
 * why this layer exists and exactly what it does not do.
 *
 * Everything here is a fixed table inside ark_bus_t: no heap, no libc, and no
 * blocking.  The one hard rule is that a frame is either sent whole or queued
 * whole -- never half-written to a link that then cannot take the rest.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_FLOW_ENABLED

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static ark_bus_flow_state_t *flow_of(struct ark_bus *bus)
{
    return &((ark_bus_t *)bus)->flow;
}

static int suspended_index(const ark_bus_flow_state_t *fl, uint32_t id)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_FLOW_MAX_PAUSED; i++) {
        if (fl->suspended[i].used && fl->suspended[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

static void suspend_peer(ark_bus_flow_state_t *fl, uint32_t id, uint32_t until_ms)
{
    int idx = suspended_index(fl, id);

    if (idx >= 0) {
        fl->suspended[idx].until_ms = until_ms;
        return;
    }
    for (idx = 0; idx < (int)ARK_BUS_FLOW_MAX_PAUSED; idx++) {
        if (!fl->suspended[idx].used) {
            fl->suspended[idx].used     = 1;
            fl->suspended[idx].id       = id;
            fl->suspended[idx].until_ms = until_ms;
            return;
        }
    }
    /* No room: drop the hold.  Losing it only means we keep sending, which is
     * the pre-M4 behaviour, so it is safe to ignore rather than to evict an
     * entry that is still doing its job. */
}

static void resume_peer(ark_bus_flow_state_t *fl, uint32_t id)
{
    int idx = suspended_index(fl, id);
    if (idx >= 0) {
        fl->suspended[idx].used = 0;
    }
}

static ark_bus_flow_entry_t *queue_free(ark_bus_flow_state_t *fl)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_FLOW_QUEUE_MAX; i++) {
        if (!fl->q[i].used) {
            return &fl->q[i];
        }
    }
    return NULL;
}

static void queue_put(ark_bus_flow_state_t *fl, const uint8_t *frame,
                      uint16_t len, uint32_t dst)
{
    ark_bus_flow_entry_t *e = queue_free(fl);

    if (!e) {
        return;
    }
    e->used      = 1;
    e->retries   = 0;
    e->dst       = dst;
    e->queued_ms = ark_bus_tick_ms();
    e->len       = len;
    ark_memcpy(e->frame, frame, len);
    fl->count++;
}

/* Try to hand one frame to the link.  Returns >0 on a whole send, otherwise the
 * link refused it (a negative error, or a short write). */
static int link_try(struct ark_bus *bus, const uint8_t *frame, uint16_t len)
{
    int n = bus->transport->send(bus->transport->ctx, frame, len);
    if (n == (int)len) {
        ark_bus_stats_tx(bus, n);
        return n;
    }
    /* A short write still pushed some bytes out; count them so the link's byte
     * counters stay honest, but treat the frame as not delivered (the receiver
     * will resync past the fragment). */
    if (n > 0) {
        ark_bus_stats_tx(bus, n);
    }
    return ARK_BUS_E_AGAIN;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ark_bus_flow_init(struct ark_bus *bus)
{
    ark_bus_flow_state_t *fl;

    if (!bus) {
        return;
    }
    fl = flow_of(bus);
    ark_memset(fl, 0, sizeof(*fl));
    fl->depth = 0;   /* off until asked for */
}

uint8_t ark_bus_flow_set_depth(struct ark_bus *bus, uint8_t depth)
{
    ark_bus_flow_state_t *fl;

    if (!bus) {
        return 0;
    }
    fl = flow_of(bus);
    if (depth > (uint8_t)ARK_BUS_FLOW_QUEUE_MAX) {
        depth = (uint8_t)ARK_BUS_FLOW_QUEUE_MAX;
    }
    if (depth == 0) {
        ark_memset(fl->q, 0, sizeof(fl->q));
        fl->count = 0;
    }
    fl->depth = depth;
    return depth;
}

uint8_t ark_bus_flow_depth(const struct ark_bus *bus)
{
    return bus ? ((const ark_bus_t *)bus)->flow.depth : 0;
}

const ark_bus_flow_state_t *ark_bus_flow_state(const struct ark_bus *bus)
{
    if (!bus) {
        return NULL;
    }
    return &((const ark_bus_t *)bus)->flow;
}

int ark_bus_flow_is_suspended(const struct ark_bus *bus, uint32_t peer_id)
{
    const ark_bus_flow_state_t *fl;

    if (!bus) {
        return 0;
    }
    fl = &((const ark_bus_t *)bus)->flow;
    return suspended_index(fl, peer_id) >= 0;
}

/* ------------------------------------------------------------------ */
/* The core path                                                       */
/* ------------------------------------------------------------------ */

int ark_bus_flow_emit(struct ark_bus *bus, const uint8_t *frame, uint16_t len,
                      uint32_t dst)
{
    ark_bus_flow_state_t *fl;
    int                   n;

    if (!bus || !bus->transport || !frame) {
        return ARK_BUS_E_INVAL;
    }
    fl = flow_of(bus);

    /* Flow control off: behave exactly like the plain send path. */
    if (fl->depth == 0u) {
        n = bus->transport->send(bus->transport->ctx, frame, len);
        ark_bus_stats_tx(bus, n);
        return n;
    }

    /* A paused unicast peer: defer without touching the link. */
    if (dst != ARK_BUS_ID_BROADCAST && ark_bus_flow_is_suspended(bus, dst)) {
        if (fl->count >= fl->depth) {
            fl->refused++;
            return ARK_BUS_E_AGAIN;
        }
        queue_put(fl, frame, len, dst);
        fl->deferred++;
        return len;
    }

    n = link_try(bus, frame, len);
    if (n > 0) {
        fl->sent++;
        return n;
    }

    /* The link would not take it whole: queue it, or push back. */
    if (fl->count >= fl->depth) {
        fl->refused++;
        return ARK_BUS_E_AGAIN;
    }
    queue_put(fl, frame, len, dst);
    fl->queued++;
    return len;
}

void ark_bus_flow_tick(struct ark_bus *bus)
{
    ark_bus_flow_state_t *fl;
    uint32_t              now;
    uint8_t               i;

    if (!bus || !bus->transport) {
        return;
    }
    fl = flow_of(bus);
    if (fl->depth == 0u || fl->count == 0u) {
        return;
    }
    now = ark_bus_tick_ms();

    for (i = 0; i < ARK_BUS_FLOW_QUEUE_MAX; i++) {
        ark_bus_flow_entry_t *e = &fl->q[i];
        int                   n;

        if (!e->used) {
            continue;
        }

        if ((uint32_t)(now - e->queued_ms) >= (uint32_t)ARK_BUS_FLOW_TTL_MS) {
            e->used = 0;
            fl->count--;
            fl->dropped_ttl++;
            continue;
        }

        if (e->dst != ARK_BUS_ID_BROADCAST && ark_bus_flow_is_suspended(bus, e->dst)) {
            continue;   /* still held off; try again next tick */
        }

        n = link_try(bus, e->frame, e->len);
        if (n > 0) {
            e->used = 0;
            fl->count--;
            fl->sent++;
            continue;
        }
        e->retries++;
    }
}

/* ------------------------------------------------------------------ */
/* Ingress pause: FLOW_BUSY / FLOW_READY                               */
/* ------------------------------------------------------------------ */

int ark_bus_flow_busy(struct ark_bus *bus, uint32_t ms)
{
    uint8_t p[4];

    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }
    flow_of(bus)->our_busy_until_ms = ark_bus_tick_ms() + ms;
    ark_wr32(p, ms);
    return ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_FLOW_BUSY,
                        ARK_BUS_TOPIC_NONE, p, 4u, 0);
}

int ark_bus_flow_ready(struct ark_bus *bus)
{
    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }
    flow_of(bus)->our_busy_until_ms = 0u;
    return ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_FLOW_READY,
                        ARK_BUS_TOPIC_NONE, NULL, 0u, 0);
}

void ark_bus_flow_handle(struct ark_bus *bus, const ark_bus_frame_t *f)
{
    ark_bus_flow_state_t *fl;

    if (!bus) {
        return;
    }
    fl = flow_of(bus);

    if (f->msg_type == ARK_BUS_MSG_FLOW_BUSY) {
        uint32_t dur = (f->payload_len >= 4u) ? ark_rd32(f->payload) : 0u;
        if (dur > 0u) {
            suspend_peer(fl, f->src_id, ark_bus_tick_ms() + dur);
        }
    } else if (f->msg_type == ARK_BUS_MSG_FLOW_READY) {
        resume_peer(fl, f->src_id);
    }
}

#endif /* ARK_BUS_FLOW_ENABLED */
