/*
 * ark_bus_route.c - distance-vector mesh routing (see ark_bus_route.h).
 *
 * Nothing here allocates: routes live in a fixed table inside ark_bus_t and the
 * envelopes are built in stack buffers.  The whole file compiles to nothing
 * when ARK_BUS_ROUTE_ENABLED is 0.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_ROUTE_ENABLED

#include "ark_bus_route.h"
#include "ark_bus_hal.h"

/* ROUTE payload: ttl(1) | final_dst(4 BE) | inner frame (...) */
#define ARK_ROUTE_HDR  5u

/* ROUTE_ADV payload: count(1) | count * ( dst(4 BE) | hops(1) ) */
#define ARK_ROUTE_ADV_ENTRY 5u

static const ark_bus_route_state_t *route_const(const ark_bus_t *bus)
{
    return &bus->route;
}

void ark_bus_route_init(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    ark_memset(&bus->route, 0, sizeof(bus->route));
}

static ark_bus_route_entry_t *route_find(ark_bus_t *bus, uint32_t dst)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_ROUTE_MAX_ENTRIES; i++) {
        if (bus->route.entries[i].used &&
            bus->route.entries[i].dst == dst) {
            return &bus->route.entries[i];
        }
    }
    return NULL;
}

int ark_bus_route_learn(ark_bus_t *bus, uint32_t dst, uint32_t next_hop,
                        uint8_t hops)
{
    ark_bus_route_entry_t *e;
    uint8_t                i;
    int                    free_slot = -1;

    if (!bus || dst == ARK_BUS_ID_BROADCAST) {
        return ARK_BUS_E_INVAL;
    }
    /* A route to ourselves, or one whose next hop is ourselves, is not a route
     * -- storing it would only invite a forwarding loop. */
    if (dst == bus->self_id) {
        return ARK_BUS_OK;
    }
    if (next_hop == bus->self_id) {
        return ARK_BUS_E_INVAL;
    }

    for (i = 0; i < ARK_BUS_ROUTE_MAX_ENTRIES; i++) {
        e = &bus->route.entries[i];
        if (!e->used) {
            if (free_slot < 0) {
                free_slot = (int)i;
            }
            continue;
        }
        if (e->dst == dst) {
            /* Heard again: refresh the freshness stamp.  Replace the path only
             * when the new one is strictly shorter, so a slower advertisement
             * never displaces a faster route (ties keep the incumbent). */
            e->learned_ms = ark_bus_tick_ms();
            if (hops < e->hops) {
                e->next_hop = next_hop;
                e->hops     = hops;
                e->direct   = (uint8_t)(next_hop == dst);
            }
            return ARK_BUS_OK;
        }
    }

    if (free_slot < 0) {
        return ARK_BUS_E_FULL;   /* fail-closed: keep what we have */
    }
    e = &bus->route.entries[free_slot];
    e->used       = 1;
    e->dst        = dst;
    e->next_hop   = next_hop;
    e->hops       = hops;
    e->direct     = (uint8_t)(next_hop == dst);
    e->learned_ms = ark_bus_tick_ms();
    return ARK_BUS_OK;
}

/* Direct neighbours (the discovery device table) are always one hop away. */
static void route_sync_neighbours(ark_bus_t *bus)
{
    uint8_t i;
    for (i = 0; i < bus->device_count; i++) {
        const ark_bus_device_t *d = &bus->devices[i];
        if (d->id == 0 || d->id == bus->self_id) {
            continue;
        }
        if (d->state == ARK_BUS_DEV_ONLINE) {
            (void)ark_bus_route_learn(bus, d->id, d->id, 1);
        }
    }
}

/* Drop learned (multi-hop) routes nobody has refreshed in a while.  Direct
 * routes are left to the device table, which owns their liveness. */
static void route_age(ark_bus_t *bus)
{
    uint32_t now = ark_bus_tick_ms();
    uint8_t  i;
    for (i = 0; i < ARK_BUS_ROUTE_MAX_ENTRIES; i++) {
        ark_bus_route_entry_t *e = &bus->route.entries[i];
        if (!e->used || e->direct) {
            continue;
        }
        if ((uint32_t)(now - e->learned_ms) > (uint32_t)ARK_BUS_ROUTE_TIMEOUT_MS) {
            e->used = 0;
        }
    }
}

static void route_advertise(ark_bus_t *bus)
{
    uint8_t buf[ARK_BUS_MSG_MAX_SIZE];
    uint8_t count = 0;
    uint8_t max_entries;
    uint8_t i;
    uint32_t now = ark_bus_tick_ms();

    /* count + count*5 must fit one frame. */
    max_entries = (uint8_t)((ARK_BUS_MSG_MAX_SIZE - 1u) / ARK_ROUTE_ADV_ENTRY);
    for (i = 0; i < ARK_BUS_ROUTE_MAX_ENTRIES && count < max_entries; i++) {
        const ark_bus_route_entry_t *e = &route_const(bus)->entries[i];
        if (!e->used || e->dst == bus->self_id) {
            continue;
        }
        ark_wr32(buf + 1u + (uint16_t)count * ARK_ROUTE_ADV_ENTRY, e->dst);
        buf[1u + (uint16_t)count * ARK_ROUTE_ADV_ENTRY + 4u] = e->hops;
        count++;
    }
    buf[0] = count;
    bus->route.last_adv_ms = now;
    (void)ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_ROUTE_ADV,
                       ARK_BUS_TOPIC_NONE, buf,
                       (uint16_t)(1u + (uint16_t)count * ARK_ROUTE_ADV_ENTRY), 0);
}

static void route_learn_adv(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    uint8_t count;
    uint8_t i;

    if (f->payload_len < 1u) {
        return;
    }
    count = f->payload[0];
    if (f->payload_len < (uint16_t)(1u + (uint16_t)count * ARK_ROUTE_ADV_ENTRY)) {
        return;
    }
    for (i = 0; i < count; i++) {
        const uint8_t *p    = f->payload + 1u + (uint16_t)i * ARK_ROUTE_ADV_ENTRY;
        uint32_t       dst  = ark_rd32(p);
        uint8_t        hops = p[4];
        if (hops >= (uint8_t)ARK_BUS_ROUTE_MAX_HOPS) {
            continue;   /* already at the ceiling: one more would be unreachable */
        }
        (void)ark_bus_route_learn(bus, dst, f->src_id, (uint8_t)(hops + 1u));
    }
}

static void route_forward_or_deliver(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    uint32_t       final_dst;
    uint32_t       next_hop;
    uint8_t        ttl;
    uint8_t        buf[ARK_BUS_MSG_MAX_SIZE];
    ark_bus_frame_t inner;
    int            rc;

    /* A ROUTE envelope is addressed to its next hop, never broadcast.  On a
     * shared medium a node hears frames meant for its neighbours too, so a
     * node that is not the addressee must stay out of the way: acting on it
     * would forward it again and spin the envelope between peers until the TTL
     * runs out. */
    if (f->dst_id != bus->self_id) {
        return;
    }
    if (f->payload_len < ARK_ROUTE_HDR) {
        bus->route.dropped++;
        return;
    }
    ttl       = f->payload[0];
    final_dst = ark_rd32(f->payload + 1);

    if (final_dst == bus->self_id) {
        /* It is for us: strip the envelope and hand the inner frame to the
         * dispatcher.  The envelope was already authenticated as a whole by
         * the crypto seam, so the inner frame is not re-checked here. */
        uint16_t inner_len = (uint16_t)(f->payload_len - ARK_ROUTE_HDR);
        rc = ark_bus_frame_decode(f->payload + ARK_ROUTE_HDR, inner_len, &inner);
        if (rc == ARK_BUS_OK) {
            bus->route.delivered++;
            (void)ark_bus_dispatch(bus, &inner);
        } else {
            bus->route.dropped++;
        }
        return;
    }

    if (ttl <= 1u || ark_bus_route_lookup(bus, final_dst, &next_hop, NULL) != ARK_BUS_OK) {
        bus->route.dropped++;
        return;
    }

    /* Re-wrap with a decremented TTL and hand it to the next hop.  The envelope
     * is forwarded verbatim apart from that one byte. */
    if (f->payload_len > sizeof(buf)) {
        bus->route.dropped++;
        return;
    }
    ark_memcpy(buf, f->payload, f->payload_len);
    buf[0] = (uint8_t)(ttl - 1u);
    (void)ark_bus_emit(bus, next_hop, ARK_BUS_MSG_ROUTE, ARK_BUS_TOPIC_NONE,
                       buf, f->payload_len, 0);
    bus->route.forwarded++;
}

void ark_bus_route_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    if (!bus || !f) {
        return;
    }
    switch (f->msg_type) {
    case ARK_BUS_MSG_ROUTE:
        route_forward_or_deliver(bus, f);
        break;
    case ARK_BUS_MSG_ROUTE_ADV:
        route_learn_adv(bus, f);
        break;
    default:
        break;
    }
}

void ark_bus_route_tick(ark_bus_t *bus)
{
    uint32_t now;

    if (!bus || bus->self_id == 0) {
        return;
    }
    /* Make sure we can name ourselves (hops 0). */
    {
        ark_bus_route_entry_t *self = route_find(bus, bus->self_id);
        if (!self) {
            uint8_t i;
            for (i = 0; i < ARK_BUS_ROUTE_MAX_ENTRIES; i++) {
                if (!bus->route.entries[i].used) {
                    self = &bus->route.entries[i];
                    self->used       = 1;
                    self->dst        = bus->self_id;
                    self->next_hop   = bus->self_id;
                    self->hops       = 0;
                    self->direct     = 1;
                    self->learned_ms = ark_bus_tick_ms();
                    break;
                }
            }
        } else {
            self->learned_ms = ark_bus_tick_ms();
        }
    }

    route_sync_neighbours(bus);
    route_age(bus);

    now = ark_bus_tick_ms();
    if ((uint32_t)(now - bus->route.last_adv_ms) >=
        (uint32_t)ARK_BUS_ROUTE_ADV_INTERVAL_MS) {
        route_advertise(bus);
    }
}

int ark_bus_route_lookup(const ark_bus_t *bus, uint32_t dst,
                         uint32_t *next_hop, uint8_t *hops)
{
    const ark_bus_route_entry_t *e;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    if (dst == bus->self_id && bus->self_id != 0) {
        if (next_hop) {
            *next_hop = dst;
        }
        if (hops) {
            *hops = 0;
        }
        return ARK_BUS_OK;
    }
    for (e = bus->route.entries; e < bus->route.entries + ARK_BUS_ROUTE_MAX_ENTRIES; e++) {
        if (e->used && e->dst == dst) {
            if (next_hop) {
                *next_hop = e->next_hop;
            }
            if (hops) {
                *hops = e->hops;
            }
            return ARK_BUS_OK;
        }
    }
    return ARK_BUS_E_NOTFOUND;
}

int ark_bus_route_send(ark_bus_t *bus, uint32_t dst, uint16_t topic_id,
                       const void *payload, uint16_t len)
{
    uint32_t next_hop;
    uint8_t  hops;
    uint8_t  inner[ARK_BUS_FRAME_MAX_SIZE];
    uint8_t  env[ARK_BUS_MSG_MAX_SIZE];
    int      inner_len;

    if (!bus || !payload) {
        return ARK_BUS_E_INVAL;
    }
    if (dst == bus->self_id) {
        return ARK_BUS_E_INVAL;
    }
    /* Fold in any neighbour we have discovered since the last tick, so a node
     * can reach a neighbour it just met without waiting for the next poll. */
    route_sync_neighbours(bus);
    if (ark_bus_route_lookup(bus, dst, &next_hop, &hops) != ARK_BUS_OK) {
        return ARK_BUS_E_NOTFOUND;   /* no route: say so rather than guess */
    }

    if (next_hop == dst) {
        /* A direct neighbour: send it exactly as a normal directed frame. */
        return ark_bus_emit(bus, dst, ARK_BUS_MSG_DIRECT, topic_id,
                            payload, len, 0);
    }

    /* Multi-hop: build the inner frame, then wrap it in a ROUTE envelope. */
    inner_len = ark_bus_build_frame(bus, dst, ARK_BUS_MSG_DIRECT, topic_id,
                                    payload, len, 0, ark_bus_next_seq(bus),
                                    inner, (uint16_t)sizeof(inner));
    if (inner_len < 0) {
        return inner_len;
    }
    if ((uint32_t)inner_len + ARK_ROUTE_HDR > (uint32_t)ARK_BUS_MSG_MAX_SIZE) {
        return ARK_BUS_E_FULL;   /* too big to ride inside one envelope */
    }
    env[0] = (uint8_t)ARK_BUS_ROUTE_MAX_HOPS;
    ark_wr32(env + 1, dst);
    ark_memcpy(env + ARK_ROUTE_HDR, inner, (size_t)inner_len);
    return ark_bus_emit(bus, next_hop, ARK_BUS_MSG_ROUTE, ARK_BUS_TOPIC_NONE,
                        env, (uint16_t)(ARK_ROUTE_HDR + (uint16_t)inner_len), 0);
}

uint8_t ark_bus_route_count(const ark_bus_t *bus)
{
    uint8_t i;
    uint8_t n = 0;
    if (!bus) {
        return 0;
    }
    for (i = 0; i < ARK_BUS_ROUTE_MAX_ENTRIES; i++) {
        if (bus->route.entries[i].used) {
            n++;
        }
    }
    return n;
}

const ark_bus_route_entry_t *ark_bus_route_at(const ark_bus_t *bus, uint8_t index)
{
    if (!bus || index >= ARK_BUS_ROUTE_MAX_ENTRIES ||
        !bus->route.entries[index].used) {
        return NULL;
    }
    return &bus->route.entries[index];
}

const ark_bus_route_state_t *ark_bus_route_state(const ark_bus_t *bus)
{
    return bus ? &bus->route : NULL;
}

#endif /* ARK_BUS_ROUTE_ENABLED */
