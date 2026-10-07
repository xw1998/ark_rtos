/*
 * transport_uart.c - a byte-fed transport for UART (and any other link that
 * hands you one byte at a time).
 *
 * The RX path is interrupt safe by construction: feed_byte() only appends to a
 * frame assembler and, on the last byte, copies the finished frame aside.
 * recv() (called from the poll loop) hands that frame to the core.  No malloc,
 * no blocking, and a bad byte never wedges the assembler.
 */
#include "ark_bus_transport.h"
#include "ark_bus_priv.h"

static ark_bus_transport_uart_t *uart_of(const transport_adapter_t *tp)
{
    return (ark_bus_transport_uart_t *)tp->ctx;
}

static int uart_send(void *ctx, const uint8_t *data, uint16_t len)
{
    ark_bus_transport_uart_t *u = (ark_bus_transport_uart_t *)ctx;
    if (!u || !u->write) {
        return ARK_BUS_E_NOSYS;
    }
    return u->write(u->io, data, len);
}

static int uart_recv(void *ctx, uint8_t *buf, uint16_t max_len)
{
    ark_bus_transport_uart_t *u = (ark_bus_transport_uart_t *)ctx;
    uint16_t                  n;

    if (!u || !buf) {
        return ARK_BUS_E_INVAL;
    }
    if (u->frame_len == 0) {
        return 0;
    }
    n = u->frame_len < max_len ? u->frame_len : max_len;
    ark_memcpy(buf, u->frame, n);
    u->frame_len = 0;
    return (int)n;
}

void ark_bus_transport_uart_init(ark_bus_transport_uart_t *u,
                                 transport_adapter_t *tp,
                                 void *io,
                                 ark_bus_uart_write_fn write)
{
    if (!u || !tp) {
        return;
    }
    ark_memset(u, 0, sizeof(*u));
    u->io    = io;
    u->write = write;
    ark_bus_frame_reader_init(&u->reader);

    tp->send = uart_send;
    tp->recv = uart_recv;
    tp->name = "UART";
    tp->ctx  = u;
}

void ark_bus_transport_uart_feed_byte(transport_adapter_t *tp, uint8_t byte)
{
    ark_bus_transport_uart_t *u;
    int                       rc;

    if (!tp) {
        return;
    }
    u = uart_of(tp);
    if (!u) {
        return;
    }

    rc = ark_bus_frame_reader_push(&u->reader, byte);
    if (rc < 0) {
        u->dropped++;
        return;
    }
    if (rc == 1) {
        if (u->frame_len == 0) {
            ark_memcpy(u->frame, u->reader.buf, u->reader.len);
            u->frame_len = u->reader.len;
        } else {
            /* The core has not picked up the previous frame yet.  Dropping the
             * new one and counting it is more honest than overwriting. */
            u->dropped++;
        }
        ark_bus_frame_reader_reset(&u->reader);
    }
}

void ark_bus_transport_uart_feed(transport_adapter_t *tp,
                                 const uint8_t *data, uint16_t len)
{
    uint16_t i;
    if (!tp || !data) {
        return;
    }
    for (i = 0; i < len; i++) {
        ark_bus_transport_uart_feed_byte(tp, data[i]);
    }
}

uint32_t ark_bus_transport_uart_dropped(const transport_adapter_t *tp)
{
    ark_bus_transport_uart_t *u;
    if (!tp) {
        return 0;
    }
    u = uart_of(tp);
    return u ? u->dropped : 0;
}
