/*
 * ark_bus_discovery.c - "who is out there", and "are you still there".
 *
 * DISCOVER is a broadcast; every peer answers with DISCOVER_REPLY and then
 * keeps itself known with periodic HEARTBEATs.  A peer that goes quiet for
 * ARK_BUS_DEVICE_TIMEOUT_MS is marked offline rather than removed, so the
 * table keeps its shape and a returning device simply flips back to ONLINE.
 *
 * The discovery payload is 5 bytes: device type (1) + capabilities (4, BE).
 */
#include "ark_bus_priv.h"

#define DISCOVERY_PAYLOAD_LEN 5

static uint16_t discovery_payload(const ark_bus_t *bus, uint8_t *out)
{
    out[0] = bus->device_type;
    ark_wr32(out + 1, bus->capabilities);
    return DISCOVERY_PAYLOAD_LEN;
}

/* Tell the application a peer's membership changed (M4).  The callback runs
 * with the bus lock held but reentrant, so it may publish. */
static void member_fire(ark_bus_t *bus, const ark_bus_device_t *d, int event)
{
    if (bus->member_cb) {
        bus->member_cb(d->id, d->type, d->capabilities, event,
                       bus->member_user);
    }
}

static uint8_t device_index(const ark_bus_t *bus, uint32_t id)
{
    uint8_t i;
    for (i = 0; i < bus->device_count; i++) {
        if (bus->devices[i].id == id) {
            return i;
        }
    }
    return bus->device_count;
}

/*
 * Drop one entry, keeping the table contiguous: the last entry is swapped into
 * the hole and the count shrinks, so ark_bus_device_count()/at() stay a plain
 * 0..count-1 walk.  Per-task "already tried" bits are swapped along with it so
 * a retry still avoids the peers it already used (a removal must not silently
 * repoint those bits at a different node).
 */
static void device_remove(ark_bus_t *bus, uint8_t idx, int event)
{
    ark_bus_device_t gone;
    uint8_t          last;

    if (idx >= bus->device_count) {
        return;
    }
    gone = bus->devices[idx];
    last = (uint8_t)(bus->device_count - 1u);

#if ARK_BUS_TASK_ENABLED
    ark_bus_task_note_device_swap(bus, idx, last);
#endif

#if ARK_BUS_REMOTE_SUB_ENABLED
    /* A departed peer must not keep a hub forwarding its old topics. */
    ark_bus_subreg_forget(bus, gone.id);
#endif

    if (idx != last) {
        bus->devices[idx] = bus->devices[last];
    }
    ark_memset(&bus->devices[last], 0, sizeof(bus->devices[last]));
    bus->device_count--;

    if (bus->member_cb) {
        bus->member_cb(gone.id, gone.type, gone.capabilities, event,
                       bus->member_user);
    }
}

static ark_bus_device_t *peer_touch(ark_bus_t *bus, uint32_t id,
                                    uint8_t type, uint32_t capabilities)
{
    uint8_t          i;
    ark_bus_device_t *d;

    if (id == ARK_BUS_ID_BROADCAST || id == bus->self_id) {
        return NULL;
    }

    for (i = 0; i < bus->device_count; i++) {
        if (bus->devices[i].id == id) {
            uint8_t was_online;
            d = &bus->devices[i];
            was_online            = (uint8_t)(d->state == ARK_BUS_DEV_ONLINE);
            d->state              = ARK_BUS_DEV_ONLINE;
            d->last_seen_ms       = ark_bus_tick_ms();
            d->missed_heartbeats  = 0;
            if (type != 0u) {
                d->type = type;
            }
            if (capabilities != 0u) {
                d->capabilities = capabilities;
            }
            if (!was_online) {
                /* Back after being offline: report it as a (re)join. */
                member_fire(bus, d, ARK_BUS_MEMBER_JOIN);
            }
            return d;
        }
    }

    if (bus->device_count >= ARK_BUS_MAX_DEVICES) {
        return NULL;
    }

    d = &bus->devices[bus->device_count++];
    d->id                = id;
    d->type              = type;
    d->capabilities      = capabilities;
    d->state             = ARK_BUS_DEV_ONLINE;
    d->last_seen_ms      = ark_bus_tick_ms();
    d->missed_heartbeats = 0;
    member_fire(bus, d, ARK_BUS_MEMBER_JOIN);
    return d;
}

int ark_bus_announce(ark_bus_t *bus)
{
    uint8_t  p[DISCOVERY_PAYLOAD_LEN];
    uint16_t n;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    n = discovery_payload(bus, p);
    return ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_DISCOVER,
                        ARK_BUS_TOPIC_NONE, p, n, 0);
}

void ark_bus_discovery_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    uint8_t  type = 0;
    uint32_t caps = 0;

    if (f->payload_len >= DISCOVERY_PAYLOAD_LEN) {
        type = f->payload[0];
        caps = ark_rd32(f->payload + 1);
    }

    if (f->msg_type == ARK_BUS_MSG_LEAVE) {
        /* A peer is leaving on purpose: drop it now and do not re-touch it
         * (a LEAVE is the one discovery frame that means "not here"). */
        uint8_t idx = device_index(bus, f->src_id);
        if (idx < bus->device_count) {
            device_remove(bus, idx, ARK_BUS_MEMBER_LEAVE);
        }
        return;
    }

    (void)peer_touch(bus, f->src_id, type, caps);

    if (f->msg_type == ARK_BUS_MSG_DISCOVER) {
        uint8_t  p[DISCOVERY_PAYLOAD_LEN];
        uint16_t n = discovery_payload(bus, p);
        (void)ark_bus_emit(bus, f->src_id, ARK_BUS_MSG_DISCOVER_REPLY,
                           ARK_BUS_TOPIC_NONE, p, n, 0);
    }
}

int ark_bus_discovery_tick(ark_bus_t *bus)
{
    uint32_t now;
    uint8_t  i;

    if (!bus || !bus->started) {
        return ARK_BUS_OK;
    }

    now = ark_bus_tick_ms();

    if ((uint32_t)(now - bus->last_heartbeat_ms) >=
        (uint32_t)ARK_BUS_HEARTBEAT_INTERVAL_MS) {
        uint8_t  p[DISCOVERY_PAYLOAD_LEN];
        uint16_t n;
        bus->last_heartbeat_ms = now;
        n = discovery_payload(bus, p);
        (void)ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_HEARTBEAT,
                           ARK_BUS_TOPIC_NONE, p, n, 0);
    }

    for (i = 0; i < bus->device_count; i++) {
        ark_bus_device_t *d = &bus->devices[i];
        uint32_t          idle = (uint32_t)(now - d->last_seen_ms);

        if (d->state == ARK_BUS_DEV_ONLINE) {
            d->missed_heartbeats =
                (uint16_t)(idle / (uint32_t)ARK_BUS_HEARTBEAT_INTERVAL_MS);
            if (idle >= (uint32_t)ARK_BUS_DEVICE_TIMEOUT_MS) {
                d->state = ARK_BUS_DEV_OFFLINE;
                member_fire(bus, d, ARK_BUS_MEMBER_OFFLINE);
#if ARK_BUS_TASK_ENABLED
                /* The worker is gone; anything it was running must be re-homed. */
                ark_bus_task_worker_offline(bus, d->id);
#endif
            }
        }

        if (bus->member_reap_ms != 0u &&
            d->state == ARK_BUS_DEV_OFFLINE && idle >= bus->member_reap_ms) {
            device_remove(bus, i, ARK_BUS_MEMBER_REAPED);
            i--;  /* re-examine the entry swapped into this slot */
        }
    }
    return ARK_BUS_OK;
}

/* ------------------------------------------------------------------ */
/* Dynamic membership API (M4)                                         */
/* ------------------------------------------------------------------ */

void ark_bus_set_member_cb(ark_bus_t *bus, ark_bus_member_cb_t cb, void *user)
{
    if (!bus) {
        return;
    }
    bus->member_cb   = cb;
    bus->member_user = user;
}

void ark_bus_set_member_reap_ms(ark_bus_t *bus, uint32_t ms)
{
    if (!bus) {
        return;
    }
    bus->member_reap_ms = ms;
}

int ark_bus_leave(ark_bus_t *bus)
{
    uint8_t  p[DISCOVERY_PAYLOAD_LEN];
    uint16_t n;
    int      rc;

    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }
    n  = discovery_payload(bus, p);
    rc = ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_LEAVE,
                      ARK_BUS_TOPIC_NONE, p, n, 0);
    ark_bus_stop(bus);
    return rc;
}
