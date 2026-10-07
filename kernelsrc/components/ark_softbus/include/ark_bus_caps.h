/*
 * ark_bus_caps.h - what each node can do, and how to find one that can.
 *
 * Every node broadcasts its device type and capability bitmask in the
 * discovery payload, so the device table already knows what a peer is good for
 * without asking.  These helpers are the query side of that: pick a worker by
 * capability, and (over RPC) ask a node to describe itself in detail.
 *
 * The capability bits live in ark_bus_protocol.h (ARK_CAP_*).
 */
#ifndef ARK_BUS_CAPS_H
#define ARK_BUS_CAPS_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus.h"

/* Capabilities advertised by `id`, or 0 when the device is unknown.  Zero is
 * also a legal value (a node that advertises nothing), so pair it with
 * ark_bus_device_find() when that distinction matters. */
uint32_t ark_bus_device_caps(const ark_bus_t *bus, uint32_t id);

/*
 * Iterate the ONLINE devices that carry every bit in `require_mask` (pass 0 to
 * match any online device).  Start with *cursor == 0; each call returns the
 * next match and advances *cursor, or NULL once the table is exhausted.  A
 * caller that wants round-robin fairness simply resets *cursor to 0 after a
 * full sweep.
 */
const ark_bus_device_t *ark_bus_find_by_cap(const ark_bus_t *bus,
                                            uint32_t require_mask,
                                            uint8_t *cursor);

#if ARK_BUS_RPC_ENABLED
/*
 * Standard DESCRIBE reply body, before any application bytes:
 *
 *   offset  size  field
 *     0      1    device_type
 *     1      4    capabilities (BE)
 *     5      1    method_count
 *     6     2*N   method ids (BE), N = method_count
 *   6+2N     ...  application-specific bytes (see ark_bus_set_describe)
 */
#define ARK_BUS_DESCRIBE_HDR  6u

/*
 * Install a hook that appends application bytes to the DESCRIBE reply (for
 * example current load or a queue depth).  Passing NULL removes it.
 */
int ark_bus_set_describe(ark_bus_t *bus, ark_bus_describe_fn fn, void *user);

/*
 * Register the built-in system methods (PING and DESCRIBE).  ark_bus_start()
 * calls this; it is exposed so a test can exercise it directly.  Existing
 * handlers are left untouched.
 */
int ark_bus_register_system_methods(ark_bus_t *bus);
#endif /* ARK_BUS_RPC_ENABLED */

#endif /* ARK_BUS_CAPS_H */
