/*
 * ark_bus_route.h - mesh routing: let a node reach a peer several hops away.
 *
 * The base bus is hub-and-spoke: a node only reaches the peers whose frames it
 * can hear directly.  That covers a shared bus and a star hub, and it is why
 * the architecture notes list "mesh beyond a single hub" as not-done.  This
 * layer adds a small distance-vector router on top:
 *
 *   - every node learns its direct neighbours for free (they are the entries in
 *     the discovery device table, one hop away);
 *   - each node periodically broadcasts ROUTE_ADV listing the destinations it
 *     can reach and how many hops away they are.  A peer that hears it installs
 *     a route to each of those destinations via the sender, one hop further;
 *   - a frame for a destination that is not a direct neighbour is wrapped in a
 *     ROUTE envelope addressed to the next hop, which forwards it.  The
 *     envelope carries a TTL (ARK_BUS_ROUTE_MAX_HOPS), so a transient loop
 *     cannot circulate a frame forever.
 *
 * Scope, stated so it is not overread: this is best-effort routing of ordinary
 * (non-QoS1) frames.  It does not do source routing, per-link cost metrics,
 * multicast trees, or split-horizon; a route is chosen by hop count and ties
 * go to the entry already installed.  It is deliberately small and bounded so
 * it fits the same MCU-first budget as everything else: no heap, no libc, a
 * fixed table.
 *
 * It is compile-gated (ARK_BUS_ROUTE_ENABLED) and off by default; a build that
 * does not ask for it links none of this and its behaviour is unchanged.
 */
#ifndef ARK_BUS_ROUTE_H
#define ARK_BUS_ROUTE_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_protocol.h"

#if ARK_BUS_ROUTE_ENABLED

struct ark_bus;

/* One destination and how to get there. */
typedef struct {
    uint8_t  used;
    uint8_t  hops;        /* 0 = ourselves, 1 = a direct neighbour */
    uint8_t  direct;      /* 1 when next_hop == dst (no forwarding) */
    uint32_t dst;         /* final destination id                   */
    uint32_t next_hop;    /* the id we hand the frame to            */
    uint32_t learned_ms;  /* last time this route was refreshed     */
} ark_bus_route_entry_t;

typedef struct {
    ark_bus_route_entry_t entries[ARK_BUS_ROUTE_MAX_ENTRIES];
    uint32_t last_adv_ms;   /* when we last broadcast a ROUTE_ADV   */
    uint32_t forwarded;     /* frames we passed on for someone else */
    uint32_t delivered;     /* frames that arrived here via a route */
    uint32_t dropped;       /* frames dropped (no route, or TTL 0)  */
} ark_bus_route_state_t;

/* Called by ark_bus_init(). */
void ark_bus_route_init(struct ark_bus *bus);

/* Refresh direct-neighbour routes, age out stale ones and broadcast ROUTE_ADV;
 * called from ark_bus_poll(). */
void ark_bus_route_tick(struct ark_bus *bus);

/* Handle an incoming ROUTE (forward / deliver) or ROUTE_ADV (learn); called
 * from dispatch. */
void ark_bus_route_handle(struct ark_bus *bus, const ark_bus_frame_t *f);

/*
 * Install a route by hand (dst reachable via next_hop in `hops` hops).  A
 * candidate is only taken when it is new or strictly shorter than the current
 * one, so a slower path never displaces a faster one.  Returns ARK_BUS_OK, or
 * ARK_BUS_E_FULL when the table has no room and the destination is new.
 */
int ark_bus_route_learn(struct ark_bus *bus, uint32_t dst, uint32_t next_hop,
                        uint8_t hops);

/* Look up the next hop toward `dst`.  Returns ARK_BUS_OK and writes *next_hop
 * (and *hops, if given) or ARK_BUS_E_NOTFOUND.  `dst == self` yields next_hop
 * == dst and hops 0 once the node has been started. */
int ark_bus_route_lookup(const struct ark_bus *bus, uint32_t dst,
                         uint32_t *next_hop, uint8_t *hops);

/*
 * Send a directed payload to `dst`, routing it through a next hop when `dst`
 * is not a direct neighbour.  `topic_id` is carried in the inner frame, so a
 * subscriber on the far node receives it exactly as a direct frame.  Best
 * effort: no QoS1.  Returns the bytes handed to the link, or a negative
 * ARK_BUS_E_* code (E_NOTFOUND when there is no route at all).
 */
int ark_bus_route_send(struct ark_bus *bus, uint32_t dst, uint16_t topic_id,
                       const void *payload, uint16_t len);

/* Introspection: number of live routes, one entry by index, and counters. */
uint8_t ark_bus_route_count(const struct ark_bus *bus);
const ark_bus_route_entry_t *ark_bus_route_at(const struct ark_bus *bus,
                                              uint8_t index);
const ark_bus_route_state_t *ark_bus_route_state(const struct ark_bus *bus);

#endif /* ARK_BUS_ROUTE_ENABLED */

#endif /* ARK_BUS_ROUTE_H */
