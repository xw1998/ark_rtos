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
            d = &bus->devices[i];
            d->state              = ARK_BUS_DEV_ONLINE;
            d->last_seen_ms       = ark_bus_tick_ms();
            d->missed_heartbeats  = 0;
            if (type != 0u) {
                d->type = type;
            }
            if (capabilities != 0u) {
                d->capabilities = capabilities;
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
        uint32_t          idle;

        if (d->state != ARK_BUS_DEV_ONLINE) {
            continue;
        }
        idle = (uint32_t)(now - d->last_seen_ms);
        d->missed_heartbeats =
            (uint16_t)(idle / (uint32_t)ARK_BUS_HEARTBEAT_INTERVAL_MS);
        if (idle >= (uint32_t)ARK_BUS_DEVICE_TIMEOUT_MS) {
            d->state = ARK_BUS_DEV_OFFLINE;
#if ARK_BUS_TASK_ENABLED
            /* The worker is gone; anything it was running must be re-homed. */
            ark_bus_task_worker_offline(bus, d->id);
#endif
        }
    }
    return ARK_BUS_OK;
}
