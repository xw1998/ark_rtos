/*
 * ark_bus_flow.h - flow control and backpressure (M4).
 *
 * Every other layer in this library assumes the link will take a frame when it
 * is handed one.  A real UART, a half-duplex radio or a bridge between two
 * segments is not like that: the driver's buffer fills, and the frame that does
 * not fit is simply lost.  This layer gives the bus two ways to say "not now"
 * instead of dropping:
 *
 *   - egress: a small, fixed queue of complete frames the link refused.  The
 *     core retries them from ark_bus_poll(); a frame older than
 *     ARK_BUS_FLOW_TTL_MS is dropped and counted.  When the queue itself is
 *     full the producer is pushed back with ARK_BUS_E_AGAIN -- the publish is
 *     refused rather than silently lost, so the caller can slow down;
 *   - ingress: a node that cannot keep up broadcasts FLOW_BUSY for a while.
 *     Peers put the sender on a "suspended" list and defer (do not send to it),
 *     then resume when the window expires or a FLOW_READY arrives.
 *
 * Scope, stated so it is not overread: this layer only buffers traffic that has
 * no retransmission of its own, i.e. broadcasts and fire-and-forget directs.
 * A QoS1 frame is already retried by ark_bus_qos.c, so it is left alone -- a
 * second retry layer on top would only double-send.  RPC and the data plane
 * ride on QoS1 and therefore keep their existing behaviour.
 *
 * It is compile-gated (ARK_BUS_FLOW_ENABLED) and inert until switched on at
 * runtime with ark_bus_flow_set_depth(): with depth 0 (the default) every call
 * path is byte-for-byte the pre-M4 one, so a firmware that does not ask for
 * flow control pays nothing for it.
 */
#ifndef ARK_BUS_FLOW_H
#define ARK_BUS_FLOW_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus_protocol.h"

#if ARK_BUS_FLOW_ENABLED

struct ark_bus;

/* One frame waiting for the link to drain. */
typedef struct {
    uint8_t  used;
    uint8_t  retries;
    uint32_t dst;
    uint32_t queued_ms;
    uint16_t len;
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
} ark_bus_flow_entry_t;

typedef struct {
    ark_bus_flow_entry_t q[ARK_BUS_FLOW_QUEUE_MAX];

    /* Peers that asked us (or that we decided) to hold off sending to them. */
    struct {
        uint8_t  used;
        uint32_t id;
        uint32_t until_ms;
    } suspended[ARK_BUS_FLOW_MAX_PAUSED];

    uint8_t  depth;      /* runtime queue depth; 0 == flow control off */
    uint8_t  count;      /* frames currently queued                    */
    uint32_t our_busy_until_ms;  /* > now while we have told peers to pause */

    uint32_t queued;     /* frames accepted into the queue             */
    uint32_t sent;       /* frames that finally went out (incl. first try) */
    uint32_t dropped_ttl;/* frames that aged out of the queue          */
    uint32_t refused;    /* publishes pushed back with E_AGAIN         */
    uint32_t deferred;   /* frames not sent now because a peer paused us */
} ark_bus_flow_state_t;

/* Called by ark_bus_init(). */
void ark_bus_flow_init(struct ark_bus *bus);

/*
 * Turn flow control on with this many queue slots (clamped to
 * ARK_BUS_FLOW_QUEUE_MAX) or off with 0.  Returns the depth actually in use.
 */
uint8_t ark_bus_flow_set_depth(struct ark_bus *bus, uint8_t depth);

/* Current queue depth (0 when flow control is off). */
uint8_t ark_bus_flow_depth(const struct ark_bus *bus);

/* Counters, for a status line or a test.  Never NULL for a valid bus. */
const ark_bus_flow_state_t *ark_bus_flow_state(const struct ark_bus *bus);

/*
 * Tell every peer to stop sending to us for `ms` milliseconds (we are
 * overloaded), or to resume early.  Both are best-effort broadcasts; a peer
 * that misses one falls back to the timeout window.  Returns the bytes sent or
 * a negative ARK_BUS_E_* code.
 */
int ark_bus_flow_busy(struct ark_bus *bus, uint32_t ms);
int ark_bus_flow_ready(struct ark_bus *bus);

/* Whether we are currently holding off sending to `peer_id`. */
int ark_bus_flow_is_suspended(const struct ark_bus *bus, uint32_t peer_id);

/* Core path: send one already-encoded non-QoS frame, queueing it when the link
 * will not take it whole.  Returns bytes handed to the link (>=0) or
 * ARK_BUS_E_AGAIN when the queue is full and the caller must retry later. */
int ark_bus_flow_emit(struct ark_bus *bus, const uint8_t *frame, uint16_t len,
                      uint32_t dst);

/* Retry queued frames; called from ark_bus_poll(). */
void ark_bus_flow_tick(struct ark_bus *bus);

/* Handle a FLOW_BUSY / FLOW_READY frame; called from dispatch. */
void ark_bus_flow_handle(struct ark_bus *bus, const ark_bus_frame_t *f);

#endif /* ARK_BUS_FLOW_ENABLED */

#endif /* ARK_BUS_FLOW_H */
