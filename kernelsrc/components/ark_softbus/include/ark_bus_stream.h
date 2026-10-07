/*
 * ark_bus_stream.h - the data plane: chunked bulk transfer between two peers.
 *
 * The control plane (RPC) carries one frame per message, so a parameter or a
 * result has to fit in ARK_BUS_MSG_MAX_SIZE bytes.  This layer removes that
 * ceiling: an object of up to ARK_BUS_STREAM_MAX_BYTES is cut into chunks that
 * each fit in one frame, sent in order, and reassembled on the far side.
 *
 * It is deliberately the smallest thing that is still correct on a constrained
 * link:
 *
 *   - the sender names each chunk's absolute offset, and the receiver
 *     acknowledges cumulatively ("every byte below `next` is stored").  A
 *     duplicated chunk is therefore idempotent, and a chunk that goes missing
 *     is recovered by the sender resending from the acknowledged offset;
 *   - one chunk is in flight at a time (stop-and-wait).  That keeps the state
 *     bounded and the code auditable, at the cost of throughput: an object of
 *     S bytes over a link with round-trip time R takes about S/chunk * R.  A
 *     sliding window is the follow-on, and this header is where it would live;
 *   - nothing is allocated.  Transmit slots borrow the caller's buffer;
 *     receive slots are static, sized by ark_bus_config.h.
 *
 * What it deliberately does NOT do: no flow control or backpressure across
 * nodes, no ordering guarantee other than "in order from one sender", no
 * retry past ARK_BUS_STREAM_RETRY_MAX, no encryption, no multicast.  The
 * receive side rejects an object larger than ARK_BUS_STREAM_RX_MAX with an
 * honest E_NOMEM rather than truncating it.
 */
#ifndef ARK_BUS_STREAM_H
#define ARK_BUS_STREAM_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus_protocol.h"

#if ARK_BUS_STREAM_ENABLED

struct ark_bus;

/*
 * Called once per outbound transfer when it finishes, from inside
 * ark_bus_poll().  `status` is ARK_BUS_OK, or ARK_BUS_E_TIMEOUT when the peer
 * stopped acknowledging, or a negative code the peer itself reported (for
 * example ARK_BUS_E_NOMEM when the object did not fit its receive buffer).
 */
typedef void (*ark_bus_stream_done_cb_t)(int status, uint16_t xid, void *user);

/*
 * Called once per inbound object, from inside ark_bus_poll(), after the last
 * chunk has arrived.  `data` points at the slot's static reassembly buffer and
 * is only valid for the duration of the call: copy what you need to keep.  The
 * slot stays intact afterwards (so a retransmitted final chunk still gets a
 * correct cumulative ACK) and is recycled when another transfer needs it.
 */
typedef void (*ark_bus_stream_recv_cb_t)(struct ark_bus *bus, uint32_t from,
                                         const uint8_t *data, uint16_t len,
                                         void *user);

/* State embedded in ark_bus_t (guarded by ARK_BUS_STREAM_ENABLED there).
 *
 * Transmit slots borrow the caller's buffer, so they are small; receive slots
 * own a static reassembly buffer, which is where the RAM goes. */
typedef struct {
    uint8_t  used;
    uint32_t dst;
    uint16_t xid;
    const uint8_t *buf;
    uint32_t total;        /* whole object length                */
    uint32_t acked;        /* every byte below this is confirmed */
    uint8_t  inflight;     /* a chunk is out, awaiting its ACK   */
    uint32_t sent_at_ms;
    uint8_t  retries;
    ark_bus_stream_done_cb_t cb;
    void    *user;
} ark_bus_stream_tx_t;

/* `have` is the contiguous byte count stored: a chunk that starts beyond it is
 * a gap and is refused, and the ACK tells the sender where to resume. */
typedef struct {
    uint8_t  used;
    uint8_t  complete;
    uint32_t src;
    uint16_t xid;
    uint32_t total;
    uint32_t have;
    uint8_t  buf[ARK_BUS_STREAM_RX_MAX];
} ark_bus_stream_rx_t;

typedef struct {
    ark_bus_stream_tx_t tx[ARK_BUS_STREAM_TX_SLOTS];
    ark_bus_stream_rx_t rx[ARK_BUS_STREAM_RX_SLOTS];
    uint16_t next_xid;
    ark_bus_stream_recv_cb_t recv_cb;
    void    *recv_user;
    uint32_t tx_objects, tx_chunks, rx_objects, rx_chunks;
    uint32_t retransmits, rejected;
} ark_bus_stream_state_t;

/*
 * Start sending `len` bytes to `dst`.  `buf` is borrowed, not copied, and must
 * stay alive and unchanged until the completion callback fires (or, when no
 * callback is given, until ark_bus_stream_pending() drops to zero).  Progress
 * is made by ark_bus_poll(), so the caller keeps polling while it waits.
 *
 * Returns ARK_BUS_OK and writes the transfer id to *xid_out, or
 * ARK_BUS_E_INVAL on bad arguments, ARK_BUS_E_NOTFOUND when `dst` is this
 * node, ARK_BUS_E_FULL when every transmit slot is busy, or
 * ARK_BUS_E_NOMEM when `len` exceeds ARK_BUS_STREAM_MAX_BYTES.
 */
int ark_bus_stream_send(struct ark_bus *bus, uint32_t dst,
                        const void *buf, uint32_t len,
                        ark_bus_stream_done_cb_t cb, void *user,
                        uint16_t *xid_out);

/* Number of outbound transfers still in progress. */
int ark_bus_stream_pending(const struct ark_bus *bus);

/* Register the inbound-object callback (replaces any previous one). */
void ark_bus_stream_on_recv(struct ark_bus *bus,
                            ark_bus_stream_recv_cb_t cb, void *user);

/*
 * Counters, for introspection and tests:
 *   objects/chunks in and out, retransmitted chunks, refused objects.
 * Any NULL pointer is ignored.
 */
void ark_bus_stream_get_stats(const struct ark_bus *bus,
                              uint32_t *tx_objects, uint32_t *tx_chunks,
                              uint32_t *rx_objects, uint32_t *rx_chunks,
                              uint32_t *retransmits, uint32_t *rejected);

#endif /* ARK_BUS_STREAM_ENABLED */

#endif /* ARK_BUS_STREAM_H */
