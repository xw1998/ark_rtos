/*
 * ark_bus_stream.c - chunked bulk transfer (the data plane).
 *
 * See ark_bus_stream.h for the contract and the reasons behind the design.
 * In one sentence: a bulk object is cut into frame-sized chunks that each
 * carry their absolute offset, the receiver stores them and answers with a
 * cumulative "next expected offset", and one chunk is in flight at a time.
 *
 * Two consequences worth spelling out, because they are the whole point:
 *
 *   - a chunk is self-describing (offset + total), so a duplicate is
 *     idempotent and a missing chunk is recoverable by resending from the
 *     acknowledged offset -- no window bitmap, no per-byte bookkeeping;
 *   - the transfer is driven by ark_bus_poll(), so it costs no stack in the
 *     application and works the same in a test (pump both buses) and across
 *     processes.
 */
#include "ark_bus_priv.h"
#include "ark_bus_stream.h"

#if ARK_BUS_STREAM_ENABLED

/* ------------------------------------------------------------------ */
/* Slot bookkeeping                                                    */
/* ------------------------------------------------------------------ */

static ark_bus_stream_tx_t *tx_free(ark_bus_stream_state_t *st)
{
    int i;
    for (i = 0; i < ARK_BUS_STREAM_TX_SLOTS; i++) {
        if (st->tx[i].used == 0u) {
            return &st->tx[i];
        }
    }
    return 0;
}

static ark_bus_stream_tx_t *tx_find(ark_bus_stream_state_t *st,
                                    uint32_t dst, uint16_t xid)
{
    int i;
    for (i = 0; i < ARK_BUS_STREAM_TX_SLOTS; i++) {
        if (st->tx[i].used != 0u && st->tx[i].dst == dst &&
            st->tx[i].xid == xid) {
            return &st->tx[i];
        }
    }
    return 0;
}

static ark_bus_stream_rx_t *rx_find(ark_bus_stream_state_t *st,
                                    uint32_t src, uint16_t xid)
{
    int i;
    for (i = 0; i < ARK_BUS_STREAM_RX_SLOTS; i++) {
        if (st->rx[i].used != 0u && st->rx[i].src == src &&
            st->rx[i].xid == xid) {
            return &st->rx[i];
        }
    }
    return 0;
}

/*
 * Take a slot for a new inbound object.  A completed slot is recycled first:
 * it is only being kept so that a retransmitted final chunk still earns a
 * correct cumulative ACK, and a fresh object is the better use of the room.
 * Nothing is ever evicted while it is still incomplete, so a partial object is
 * never silently repaired with a hole in it.
 */
static ark_bus_stream_rx_t *rx_alloc(ark_bus_stream_state_t *st)
{
    int i;
    for (i = 0; i < ARK_BUS_STREAM_RX_SLOTS; i++) {
        if (st->rx[i].used == 0u) {
            return &st->rx[i];
        }
    }
    for (i = 0; i < ARK_BUS_STREAM_RX_SLOTS; i++) {
        if (st->rx[i].complete != 0u) {
            return &st->rx[i];
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Outbound                                                            */
/* ------------------------------------------------------------------ */

static void tx_finish(ark_bus_t *bus, ark_bus_stream_tx_t *s, int status)
{
    ark_bus_stream_done_cb_t cb = s->cb;
    void    *user = s->user;
    uint16_t xid  = s->xid;

    (void)bus;
    ark_memset(s, 0, sizeof(*s));

    if (cb) {
        cb(status, xid, user);
    }
}

/* Send the chunk at the acknowledged offset.  Returns the transmit result
 * (> 0 bytes, or a negative error) exactly as ark_bus_emit() reports it. */
static int tx_send_chunk(ark_bus_t *bus, ark_bus_stream_tx_t *s)
{
    uint8_t  payload[ARK_BUS_STREAM_HDR + ARK_BUS_STREAM_CHUNK_MAX];
    uint32_t remain = s->total - s->acked;
    uint16_t n      = (uint16_t)(remain > (uint32_t)ARK_BUS_STREAM_CHUNK_MAX
                                     ? (uint32_t)ARK_BUS_STREAM_CHUNK_MAX
                                     : remain);

    if (n == 0u) {
        return ARK_BUS_E_INVAL;
    }

    ark_wr16(payload, s->xid);
    ark_wr32(payload + 2, s->acked);
    ark_wr32(payload + 6, s->total);
    ark_wr16(payload + 10, n);
    ark_memcpy(payload + ARK_BUS_STREAM_HDR, s->buf + s->acked, n);

    return ark_bus_emit(bus, s->dst, ARK_BUS_MSG_STREAM, ARK_BUS_TOPIC_NONE,
                        payload, (uint16_t)(ARK_BUS_STREAM_HDR + n), 0);
}

static void stream_send_ack(ark_bus_t *bus, uint32_t dst, uint16_t xid,
                            uint32_t next, int8_t status)
{
    uint8_t p[ARK_BUS_STREAM_ACK_LEN];

    ark_wr16(p, xid);
    ark_wr32(p + 2, next);
    p[6] = (uint8_t)status;

    (void)ark_bus_emit(bus, dst, ARK_BUS_MSG_STREAM_ACK, ARK_BUS_TOPIC_NONE,
                       p, ARK_BUS_STREAM_ACK_LEN, 0);
}

/* ------------------------------------------------------------------ */
/* Inbound                                                             */
/* ------------------------------------------------------------------ */

static void stream_rx(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_stream_state_t *st = &bus->stream;
    const uint8_t *p = f->payload;
    ark_bus_stream_rx_t *s;
    uint16_t xid;
    uint32_t off;
    uint32_t total;
    uint16_t n;

    /* Too short to even name the transfer: drop it silently rather than
     * inventing an ACK for a transfer we cannot identify. */
    if (f->payload_len < (uint16_t)ARK_BUS_STREAM_HDR) {
        return;
    }

    xid   = ark_rd16(p);
    off   = ark_rd32(p + 2);
    total = ark_rd32(p + 6);
    n     = ark_rd16(p + 10);

    if (n == 0u || total == 0u ||
        n > (uint16_t)(f->payload_len - ARK_BUS_STREAM_HDR)) {
        return;
    }

    s = rx_find(st, f->src_id, xid);
    if (!s) {
        if (total > (uint32_t)ARK_BUS_STREAM_RX_MAX) {
            /* Honest refusal: better than storing a truncated object. */
            st->rejected++;
            stream_send_ack(bus, f->src_id, xid, 0u, (int8_t)ARK_BUS_E_NOMEM);
            return;
        }
        s = rx_alloc(st);
        if (!s) {
            st->rejected++;
            stream_send_ack(bus, f->src_id, xid, 0u, (int8_t)ARK_BUS_E_FULL);
            return;
        }
        s->used     = 1;
        s->complete = 0;
        s->src      = f->src_id;
        s->xid      = xid;
        s->total    = total;
        s->have     = 0;
    }

    /* The same xid naming a different object means the peer reused an id
     * before we finished; refusing is safer than mixing two objects. */
    if (total != s->total) {
        stream_send_ack(bus, f->src_id, xid, s->have, (int8_t)ARK_BUS_E_INVAL);
        return;
    }

    /* The gap test comes first and the range test must not be reached with an
     * untrusted `off`: testing `off > have` before adding keeps the sum inside
     * a u32 that cannot wrap. */
    if (off > s->have || off + (uint32_t)n > s->total) {
        stream_send_ack(bus, f->src_id, xid, s->have, (int8_t)ARK_BUS_OK);
        return;
    }

    ark_memcpy(s->buf + off, p + ARK_BUS_STREAM_HDR, n);
    if (off + (uint32_t)n > s->have) {
        s->have = off + (uint32_t)n;
    }
    st->rx_chunks++;

    /* Acknowledge before delivering, so the sender learns its last chunk
     * landed even if the application callback misbehaves. */
    stream_send_ack(bus, f->src_id, xid, s->have, (int8_t)ARK_BUS_OK);

    if (s->complete == 0u && s->have == s->total) {
        s->complete = 1;
        st->rx_objects++;
        if (st->recv_cb) {
            st->recv_cb(bus, s->src, s->buf, (uint16_t)s->total, st->recv_user);
        }
    }
}

static void stream_ack(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_stream_state_t *st = &bus->stream;
    ark_bus_stream_tx_t *s;
    uint32_t next;
    int8_t   status;

    if (f->payload_len < (uint16_t)ARK_BUS_STREAM_ACK_LEN) {
        return;
    }

    s = tx_find(st, f->src_id, ark_rd16(f->payload));
    if (!s) {
        return;                 /* stale ACK for a finished or unknown transfer */
    }

    next   = ark_rd32(f->payload + 2);
    status = (int8_t)f->payload[6];

    if (status < 0) {
        tx_finish(bus, s, (int)status);
        return;
    }
    if (next > s->total) {
        return;                 /* cannot be true; ignore rather than trust it */
    }

    /* Any change in the acknowledged offset makes progress: it may advance, or
     * it may go backwards when the peer lost its partial state (the slot was
     * recycled) and needs the object from an earlier offset.  Doing nothing on
     * an unchanged offset is what lets the retry timer still fire. */
    if (next != s->acked) {
        s->acked      = next;
        s->inflight   = 0;
        s->retries    = 0;
        s->sent_at_ms = ark_bus_tick_ms();
    }

    if (s->acked >= s->total) {
        tx_finish(bus, s, ARK_BUS_OK);
    }
}

/* ------------------------------------------------------------------ */
/* Wiring                                                              */
/* ------------------------------------------------------------------ */

void ark_bus_stream_init(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    /* ark_bus_init() already cleared the storage; only the id cursor needs a
     * non-zero start so transfer id 0 stays meaningless. */
    bus->stream.next_xid = 1u;
}

void ark_bus_stream_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    if (bus == 0 || f == 0) {
        return;
    }
    if (f->msg_type == ARK_BUS_MSG_STREAM) {
        stream_rx(bus, f);
    } else if (f->msg_type == ARK_BUS_MSG_STREAM_ACK) {
        stream_ack(bus, f);
    }
}

void ark_bus_stream_tick(ark_bus_t *bus)
{
    ark_bus_stream_state_t *st;
    uint32_t now;
    int      i;

    if (!bus) {
        return;
    }
    st  = &bus->stream;
    now = ark_bus_tick_ms();

    for (i = 0; i < ARK_BUS_STREAM_TX_SLOTS; i++) {
        ark_bus_stream_tx_t *s = &st->tx[i];

        if (s->used == 0u) {
            continue;
        }

        if (s->inflight == 0u) {
            if (tx_send_chunk(bus, s) > 0) {
                s->inflight   = 1;
                s->sent_at_ms = now;
                st->tx_chunks++;
            }
            continue;           /* a failed send is retried on the next tick */
        }

        if ((uint32_t)(now - s->sent_at_ms) <
            (uint32_t)ARK_BUS_STREAM_RETRY_INTERVAL_MS) {
            continue;
        }

        if (s->retries >= (uint8_t)ARK_BUS_STREAM_RETRY_MAX) {
            tx_finish(bus, s, ARK_BUS_E_TIMEOUT);
            continue;
        }

        s->retries++;
        st->retransmits++;
        if (tx_send_chunk(bus, s) > 0) {
            s->sent_at_ms = now;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int ark_bus_stream_send(ark_bus_t *bus, uint32_t dst,
                        const void *buf, uint32_t len,
                        ark_bus_stream_done_cb_t cb, void *user,
                        uint16_t *xid_out)
{
    ark_bus_stream_state_t *st;
    ark_bus_stream_tx_t    *s;

    if (!bus || !buf || len == 0u) {
        return ARK_BUS_E_INVAL;
    }
    if (len > (uint32_t)ARK_BUS_STREAM_MAX_BYTES) {
        return ARK_BUS_E_NOMEM;
    }
    if (dst == ARK_BUS_ID_BROADCAST) {
        /* One-way unicast only: a broadcast object would need one transfer id
         * per receiver, which is a different (and larger) piece of machinery. */
        return ARK_BUS_E_INVAL;
    }
    if (bus->self_id != 0u && dst == bus->self_id) {
        return ARK_BUS_E_NOTFOUND;
    }

    st = &bus->stream;
    s  = tx_free(st);
    if (!s) {
        return ARK_BUS_E_FULL;
    }

    st->next_xid++;
    if (st->next_xid == 0u) {
        st->next_xid = 1u;
    }

    ark_memset(s, 0, sizeof(*s));
    s->used   = 1;
    s->dst    = dst;
    s->xid    = st->next_xid;
    s->buf    = (const uint8_t *)buf;
    s->total  = len;
    s->acked  = 0;
    s->cb     = cb;
    s->user   = user;
    st->tx_objects++;

    if (xid_out) {
        *xid_out = s->xid;
    }

    /* Kick the first chunk now so a caller that polls immediately advances
     * without waiting for a whole retry interval to elapse first. */
    if (tx_send_chunk(bus, s) > 0) {
        s->inflight   = 1;
        s->sent_at_ms = ark_bus_tick_ms();
        st->tx_chunks++;
    }

    return ARK_BUS_OK;
}

int ark_bus_stream_pending(const ark_bus_t *bus)
{
    int i;
    int n = 0;

    if (!bus) {
        return 0;
    }
    for (i = 0; i < ARK_BUS_STREAM_TX_SLOTS; i++) {
        if (bus->stream.tx[i].used != 0u) {
            n++;
        }
    }
    return n;
}

void ark_bus_stream_on_recv(ark_bus_t *bus, ark_bus_stream_recv_cb_t cb,
                            void *user)
{
    if (!bus) {
        return;
    }
#if ARK_BUS_COLL_ENABLED
    /* Once the collective layer has taken the receive path it keeps it for
     * good: it forwards every object that is not a collective message to the
     * callback registered here.  Swapping it out at the end of each operation
     * would drop a peer's next-operation message that arrived in the same
     * receive batch, because the batch is drained before the callback can be
     * re-installed. */
    if (bus->coll.owns_recv) {
        bus->coll.fallback_cb   = cb;
        bus->coll.fallback_user = user;
        bus->coll.fallback_set  = 1u;
        return;
    }
#endif
    bus->stream.recv_cb   = cb;
    bus->stream.recv_user = user;
}

void ark_bus_stream_get_stats(const ark_bus_t *bus,
                              uint32_t *tx_objects, uint32_t *tx_chunks,
                              uint32_t *rx_objects, uint32_t *rx_chunks,
                              uint32_t *retransmits, uint32_t *rejected)
{
    if (!bus) {
        return;
    }
    if (tx_objects)  { *tx_objects  = bus->stream.tx_objects;  }
    if (tx_chunks)   { *tx_chunks   = bus->stream.tx_chunks;   }
    if (rx_objects)  { *rx_objects  = bus->stream.rx_objects;  }
    if (rx_chunks)   { *rx_chunks   = bus->stream.rx_chunks;   }
    if (retransmits) { *retransmits = bus->stream.retransmits; }
    if (rejected)    { *rejected    = bus->stream.rejected;    }
}

#endif /* ARK_BUS_STREAM_ENABLED */
