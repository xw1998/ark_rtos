/*
 * transport_loopback.c - two adapters wired back to back through a byte ring.
 *
 * It is the transport used by the host tests and by the Linux example, and it
 * doubles as a virtual link between two buses that live in the same process.
 * Frames are handed over whole: recv() only reports a frame once all of its
 * bytes have arrived, so the core never sees a half frame.
 */
#include "ark_bus_transport.h"

/* ------------------------------------------------------------------ */
/* Ring helpers                                                        */
/* ------------------------------------------------------------------ */

static uint16_t ring_count(const ark_bus_ring_t *r)
{
    if (r->head >= r->tail) {
        return (uint16_t)(r->head - r->tail);
    }
    return (uint16_t)(r->cap - r->tail + r->head);
}

static uint8_t ring_peek(const ark_bus_ring_t *r, uint16_t off)
{
    uint16_t idx = (uint16_t)((r->tail + off) % r->cap);
    return r->buf[idx];
}

static uint16_t ring_push(ark_bus_ring_t *r, const uint8_t *data, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len; i++) {
        uint16_t next = (uint16_t)((r->head + 1) % r->cap);
        if (next == r->tail) {
            break; /* full: drop the tail of this write */
        }
        r->buf[r->head] = data[i];
        r->head = next;
    }
    return i;
}

static void ring_pop(ark_bus_ring_t *r, uint8_t *out, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len && r->tail != r->head; i++) {
        out[i] = r->buf[r->tail];
        r->tail = (uint16_t)((r->tail + 1) % r->cap);
    }
}

static void ring_drop(ark_bus_ring_t *r, uint16_t len)
{
    uint16_t i;
    for (i = 0; i < len && r->tail != r->head; i++) {
        r->tail = (uint16_t)((r->tail + 1) % r->cap);
    }
}

/* ------------------------------------------------------------------ */
/* Adapter                                                             */
/* ------------------------------------------------------------------ */

static int loopback_send(void *ctx, const uint8_t *data, uint16_t len)
{
    ark_bus_loopback_ep_t *ep = (ark_bus_loopback_ep_t *)ctx;
    if (!ep || !ep->out) {
        return ARK_BUS_E_INVAL;
    }
    return (int)ring_push(ep->out, data, len);
}

static int loopback_recv(void *ctx, uint8_t *buf, uint16_t max_len)
{
    ark_bus_loopback_ep_t *ep = (ark_bus_loopback_ep_t *)ctx;
    ark_bus_ring_t        *r;
    uint16_t               avail;
    uint16_t               plen;
    uint16_t               total;

    if (!ep || !buf || !ep->in) {
        return ARK_BUS_E_INVAL;
    }
    r = ep->in;
    avail = ring_count(r);
    if (avail < 11) {
        return 0;
    }

    if (ring_peek(r, 0) != 0xAAu || ring_peek(r, 1) != 0x55u) {
        ring_drop(r, 1);   /* resynchronise one byte at a time */
        return 0;
    }

    plen = (uint16_t)(((uint16_t)ring_peek(r, 9) << 8) | ring_peek(r, 10));
    total = ark_bus_frame_size(plen);
    if (plen > ARK_BUS_MSG_MAX_SIZE || total > max_len) {
        ring_drop(r, 1);
        return 0;
    }
    if (avail < total) {
        return 0;          /* wait for the rest of the frame */
    }

    ring_pop(r, buf, total);
    return (int)total;
}

void ark_bus_loopback_init(ark_bus_loopback_t *lb, uint8_t *storage,
                           uint16_t half_cap)
{
    if (!lb || !storage || half_cap < 2u) {
        return;
    }

    ark_bus_ring_t *a2b = &lb->a2b;
    ark_bus_ring_t *b2a = &lb->b2a;

    a2b->buf = storage;
    a2b->cap = half_cap;
    a2b->head = 0;
    a2b->tail = 0;

    b2a->buf = storage + half_cap;
    b2a->cap = half_cap;
    b2a->head = 0;
    b2a->tail = 0;

    /* A writes to a2b, reads from b2a. */
    lb->ep_a.in  = b2a;
    lb->ep_a.out = a2b;
    /* B is the mirror image. */
    lb->ep_b.in  = a2b;
    lb->ep_b.out = b2a;

    lb->a.send = loopback_send;
    lb->a.recv = loopback_recv;
    lb->a.name = "LOOP-A";
    lb->a.ctx  = &lb->ep_a;

    lb->b.send = loopback_send;
    lb->b.recv = loopback_recv;
    lb->b.name = "LOOP-B";
    lb->b.ctx  = &lb->ep_b;
}

transport_adapter_t *ark_bus_loopback_a(ark_bus_loopback_t *lb)
{
    return lb ? &lb->a : NULL;
}

transport_adapter_t *ark_bus_loopback_b(ark_bus_loopback_t *lb)
{
    return lb ? &lb->b : NULL;
}
