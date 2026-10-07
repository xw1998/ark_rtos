/*
 * ark_bus_subreg.c - remote subscription registry (see ark_bus.h).
 *
 * A SUBSCRIBE / UNSUBSCRIBE frame names one topic id and, by its dst_id, one
 * node (or the broadcast id).  Without this file the core accepts those frames
 * and throws them away; with ARK_BUS_REMOTE_SUB_ENABLED a node writes down
 * "peer P wants topic T" so a hub or bridge can later forward T only to the
 * links that asked for it, instead of fanning every frame to everyone.
 *
 * This is an inventory, not a router: the core still broadcasts, exactly as
 * before.  Nothing here blocks, allocates, or calls libc -- it is a fixed
 * table inside ark_bus_t.  When the feature is compiled out this file is empty
 * and the tables cost neither .text nor .bss.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_REMOTE_SUB_ENABLED

/* ------------------------------------------------------------------ */
/* Fixed-table helpers                                                 */
/* ------------------------------------------------------------------ */

static ark_bus_rsub_entry_t *entry_find(ark_bus_t *bus, uint32_t peer)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_REMOTE_SUB_PEERS; i++) {
        if (bus->rsub[i].n != 0 && bus->rsub[i].peer == peer) {
            return &bus->rsub[i];
        }
    }
    return NULL;
}

/* Returns the peer's entry, allocating a free slot if it has none yet, or
 * NULL when the table is full. */
static ark_bus_rsub_entry_t *entry_obtain(ark_bus_t *bus, uint32_t peer)
{
    uint8_t i;
    ark_bus_rsub_entry_t *e = entry_find(bus, peer);

    if (e) {
        return e;
    }
    for (i = 0; i < ARK_BUS_REMOTE_SUB_PEERS; i++) {
        if (bus->rsub[i].n == 0) {
            bus->rsub[i].peer = peer;
            return &bus->rsub[i];
        }
    }
    return NULL;
}

static int topic_index(const ark_bus_rsub_entry_t *e, uint16_t topic_id)
{
    uint8_t i;
    for (i = 0; i < e->n; i++) {
        if (e->topics[i] == topic_id) {
            return (int)i;
        }
    }
    return -1;
}

/* Append `topic_id` to `e`, ignoring duplicates.  Returns ARK_BUS_OK, or
 * ARK_BUS_E_FULL when the per-peer budget is exhausted (the caller counts the
 * drop so the state is never silently wrong). */
static int topic_add(ark_bus_rsub_entry_t *e, uint16_t topic_id)
{
    if (topic_index(e, topic_id) >= 0) {
        return ARK_BUS_OK;
    }
    if (e->n >= ARK_BUS_REMOTE_SUB_TOPICS) {
        return ARK_BUS_E_FULL;
    }
    e->topics[e->n++] = topic_id;
    return ARK_BUS_OK;
}

/* Remove `topic_id` from `e`; free the entry when it was the last one. */
static void topic_del(ark_bus_rsub_entry_t *e, uint16_t topic_id)
{
    int idx = topic_index(e, topic_id);
    uint8_t last;

    if (idx < 0) {
        return;
    }
    last = (uint8_t)(e->n - 1u);
    if ((uint8_t)idx != last) {
        e->topics[idx] = e->topics[last];
    }
    e->topics[last] = 0;
    e->n--;
    if (e->n == 0) {
        e->peer = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Frame intake (called by ark_bus_dispatch)                           */
/* ------------------------------------------------------------------ */

void ark_bus_subreg_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_rsub_entry_t *e;

    /* Only record intent that is aimed at us or broadcast.  A frame addressed
     * to some other node on a shared medium is not ours to remember. */
    if (f->dst_id != ARK_BUS_ID_BROADCAST && f->dst_id != bus->self_id) {
        return;
    }
    /* A subscription with no topic cannot be honoured; refuse rather than
     * store a topic id that will never match. */
    if (f->topic_id == ARK_BUS_TOPIC_NONE) {
        bus->stats.rsub_dropped++;
        return;
    }

    e = entry_obtain(bus, f->src_id);
    if (!e) {
        bus->stats.rsub_dropped++;
        return;
    }

    if (f->msg_type == ARK_BUS_MSG_SUBSCRIBE) {
        if (topic_add(e, f->topic_id) != ARK_BUS_OK) {
            bus->stats.rsub_dropped++;
        }
    } else { /* ARK_BUS_MSG_UNSUBSCRIBE */
        topic_del(e, f->topic_id);
    }
}

/* Drop every remembered topic for `peer` (a peer that left or was reaped). */
void ark_bus_subreg_forget(ark_bus_t *bus, uint32_t peer)
{
    ark_bus_rsub_entry_t *e = entry_find(bus, peer);
    if (e) {
        ark_memset(e, 0, sizeof(*e));
    }
}

/* ------------------------------------------------------------------ */
/* Public query API (declared under ARK_BUS_REMOTE_SUB_ENABLED)        */
/* ------------------------------------------------------------------ */

int ark_bus_rsub_count(const ark_bus_t *bus, uint32_t peer)
{
    ark_bus_rsub_entry_t *e;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    e = entry_find((ark_bus_t *)bus, peer);
    return e ? (int)e->n : ARK_BUS_E_NOTFOUND;
}

int ark_bus_rsub_has(const ark_bus_t *bus, uint32_t peer, uint16_t topic_id)
{
    ark_bus_rsub_entry_t *e;

    if (!bus) {
        return 0;
    }
    e = entry_find((ark_bus_t *)bus, peer);
    return (e && topic_index(e, topic_id) >= 0) ? 1 : 0;
}

int ark_bus_rsub_at(const ark_bus_t *bus, uint32_t peer, uint8_t i,
                    uint16_t *topic_id_out)
{
    ark_bus_rsub_entry_t *e;

    if (!bus || !topic_id_out) {
        return ARK_BUS_E_INVAL;
    }
    e = entry_find((ark_bus_t *)bus, peer);
    if (!e) {
        return ARK_BUS_E_NOTFOUND;
    }
    if (i >= e->n) {
        return ARK_BUS_E_INVAL;
    }
    *topic_id_out = e->topics[i];
    return ARK_BUS_OK;
}

int ark_bus_rsub_peers(const ark_bus_t *bus, uint32_t *out, uint8_t max)
{
    uint8_t i;
    uint8_t n = 0;

    if (!bus || !out) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < ARK_BUS_REMOTE_SUB_PEERS && n < max; i++) {
        if (bus->rsub[i].n != 0) {
            out[n++] = bus->rsub[i].peer;
        }
    }
    return (int)n;
}

void ark_bus_rsub_clear(struct ark_bus *bus, uint32_t peer)
{
    ark_bus_rsub_entry_t *e;

    if (!bus) {
        return;
    }
    e = entry_find(bus, peer);
    if (e) {
        ark_memset(e, 0, sizeof(*e));
    }
}

int ark_bus_rsub_announce(ark_bus_t *bus, uint32_t target, uint16_t topic_id,
                          int subscribe)
{
    int rc;

    if (!bus || topic_id == ARK_BUS_TOPIC_NONE) {
        return ARK_BUS_E_INVAL;
    }
    /* ark_bus_emit() reports the transmitted byte count on success; this API
     * has nothing to say about size, so collapse any non-negative result to
     * ARK_BUS_OK and pass errors through untouched. */
    rc = ark_bus_emit(bus, target,
                      subscribe ? ARK_BUS_MSG_SUBSCRIBE
                                : ARK_BUS_MSG_UNSUBSCRIBE,
                      topic_id, NULL, 0, 0);
    return rc < 0 ? rc : ARK_BUS_OK;
}

#endif /* ARK_BUS_REMOTE_SUB_ENABLED */
