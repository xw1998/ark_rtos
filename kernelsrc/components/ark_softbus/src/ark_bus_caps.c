/*
 * ark_bus_caps.c - capability queries and the built-in self-description.
 *
 * The device table is filled by discovery; everything here reads it or serves
 * the two system RPC methods that any started node answers without extra
 * wiring (PING and DESCRIBE).
 */
#include "ark_bus_priv.h"
#include "ark_bus_caps.h"
#include "ark_bus_rpc.h"

uint32_t ark_bus_device_caps(const ark_bus_t *bus, uint32_t id)
{
    const ark_bus_device_t *d = ark_bus_device_find(bus, id);
    return d ? d->capabilities : 0u;
}

const ark_bus_device_t *ark_bus_find_by_cap(const ark_bus_t *bus,
                                            uint32_t require_mask,
                                            uint8_t *cursor)
{
    uint8_t i;

    if (!bus || !cursor) {
        return NULL;
    }
    for (i = *cursor; i < bus->device_count; i++) {
        const ark_bus_device_t *d = &bus->devices[i];
        if (d->state != ARK_BUS_DEV_ONLINE) {
            continue;
        }
        if (require_mask != 0u &&
            (d->capabilities & require_mask) != require_mask) {
            continue;
        }
        *cursor = (uint8_t)(i + 1u);
        return d;
    }
    *cursor = bus->device_count;
    return NULL;
}

#if ARK_BUS_RPC_ENABLED

int ark_bus_set_describe(ark_bus_t *bus, ark_bus_describe_fn fn, void *user)
{
    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    bus->describe_fn   = fn;
    bus->describe_user = user;
    return ARK_BUS_OK;
}

/* PING: echo the request parameters straight back, so a probe learns both
 * reachability and round-trip integrity in one hop. */
static int sys_ping(struct ark_bus *bus, uint32_t from,
                    const uint8_t *params, uint16_t plen,
                    uint8_t *reply, uint16_t rcap,
                    uint16_t *rlen, void *user)
{
    uint16_t n = plen;
    (void)bus; (void)from; (void)user;
    if (n > rcap) {
        n = rcap;
    }
    if (n && params) {
        ark_memcpy(reply, params, n);
    }
    *rlen = n;
    return ARK_BUS_OK;
}

/* DESCRIBE: the standard header plus whatever the app hook appends. */
static int sys_describe(struct ark_bus *bus, uint32_t from,
                        const uint8_t *params, uint16_t plen,
                        uint8_t *reply, uint16_t rcap,
                        uint16_t *rlen, void *user)
{
    uint16_t n = ARK_BUS_DESCRIBE_HDR;
    uint8_t  mc = 0;
    uint8_t  i;
    (void)from; (void)params; (void)plen; (void)user;

    if (!bus || rcap < ARK_BUS_DESCRIBE_HDR) {
        return ARK_BUS_E_FULL;
    }

    reply[0] = bus->device_type;
    ark_wr32(reply + 1, bus->capabilities);

    for (i = 0; i < bus->rpc.method_count; i++) {
        if ((uint16_t)(n + 2u) > rcap) {
            break;
        }
        ark_wr16(reply + n, bus->rpc.methods[i].method);
        n  += 2u;
        mc++;
    }
    reply[5] = mc;

    if (bus->describe_fn) {
        uint16_t extra = bus->describe_fn(bus, reply + n,
                                          (uint16_t)(rcap - n),
                                          bus->describe_user);
        if ((uint16_t)(n + extra) > rcap) {
            extra = (uint16_t)(rcap - n);
        }
        n = (uint16_t)(n + extra);
    }

    *rlen = n;
    return ARK_BUS_OK;
}

int ark_bus_register_system_methods(ark_bus_t *bus)
{
    int rc;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    if (!ark_bus_rpc_has_method(bus, ARK_BUS_M_PING)) {
        rc = ark_bus_rpc_register(bus, ARK_BUS_M_PING, sys_ping, NULL);
        if (rc != ARK_BUS_OK) {
            return rc;
        }
    }
    if (!ark_bus_rpc_has_method(bus, ARK_BUS_M_DESCRIBE)) {
        rc = ark_bus_rpc_register(bus, ARK_BUS_M_DESCRIBE, sys_describe, NULL);
        if (rc != ARK_BUS_OK) {
            return rc;
        }
    }
    return ARK_BUS_OK;
}

#endif /* ARK_BUS_RPC_ENABLED */
