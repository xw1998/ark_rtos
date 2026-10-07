/*
 * ark_bus_transport.h - the transport adapter: the one interface that turns
 * bytes into frames for the core.  A UART, an in-memory loopback link, a TCP
 * socket and a UDP datagram socket all present the same two calls.
 *
 * send() returns the number of bytes handed to the link, or a negative
 * ARK_BUS_E_* code.  recv() returns the length of one complete frame, 0 when
 * nothing is ready, or a negative error; it never blocks.
 */
#ifndef ARK_BUS_TRANSPORT_H
#define ARK_BUS_TRANSPORT_H

#include <stdint.h>
#include "ark_bus_protocol.h"

typedef struct transport_adapter {
    int (*send)(void *ctx, const uint8_t *data, uint16_t len);
    int (*recv)(void *ctx, uint8_t *buf, uint16_t max_len);
    const char *name;
    void *ctx;
} transport_adapter_t;

/* ------------------------------------------------------------------ */
/* UART: byte-fed, interrupt friendly.                                 */
/* ------------------------------------------------------------------ */
typedef int (*ark_bus_uart_write_fn)(void *io, const uint8_t *data, uint16_t len);

typedef struct {
    void                   *io;
    ark_bus_uart_write_fn   write;
    ark_bus_frame_reader_t  reader;
    uint8_t                 frame[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t                frame_len;
    uint32_t                dropped;
} ark_bus_transport_uart_t;

/*
 * Initialise a UART transport.  `u` and `tp` are caller-owned; `write` is the
 * platform's blocking/non-blocking "push these bytes out of the UART" hook.
 * Feed received bytes from the UART ISR with ark_bus_transport_uart_feed_*().
 */
void ark_bus_transport_uart_init(ark_bus_transport_uart_t *u,
                                 transport_adapter_t *tp,
                                 void *io,
                                 ark_bus_uart_write_fn write);

/* Feed one byte (call from the RX interrupt). */
void ark_bus_transport_uart_feed_byte(transport_adapter_t *tp, uint8_t byte);

/* Feed a block of bytes (call from a DMA-complete handler). */
void ark_bus_transport_uart_feed(transport_adapter_t *tp,
                                 const uint8_t *data, uint16_t len);

/* Bytes discarded because they did not belong to a valid frame. */
uint32_t ark_bus_transport_uart_dropped(const transport_adapter_t *tp);

/* ------------------------------------------------------------------ */
/* Loopback: two adapters wired back to back through a byte ring.      */
/* Used by the host tests and by MCU-to-MCU virtual links.             */
/* ------------------------------------------------------------------ */
typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t head;
    uint16_t tail;
} ark_bus_ring_t;

typedef struct {
    ark_bus_ring_t *in;   /* ring this endpoint reads from  */
    ark_bus_ring_t *out;  /* ring this endpoint writes into */
} ark_bus_loopback_ep_t;

typedef struct {
    ark_bus_ring_t       a2b;
    ark_bus_ring_t       b2a;
    ark_bus_loopback_ep_t ep_a;
    ark_bus_loopback_ep_t ep_b;
    transport_adapter_t  a;
    transport_adapter_t  b;
} ark_bus_loopback_t;

/*
 * `storage` must be at least 2 * half_cap bytes and stay alive for the life of
 * the link; the first half_cap bytes become the A->B ring, the rest the B->A
 * ring.  half_cap should be comfortably larger than one frame, otherwise a
 * burst of traffic drops frames.
 */
void ark_bus_loopback_init(ark_bus_loopback_t *lb, uint8_t *storage,
                           uint16_t half_cap);

transport_adapter_t *ark_bus_loopback_a(ark_bus_loopback_t *lb);
transport_adapter_t *ark_bus_loopback_b(ark_bus_loopback_t *lb);

/* ------------------------------------------------------------------ */
/* TCP / UDP: POSIX stream and datagram sockets.                       */
/*                                                                     */
/* These are compiled only where sockets exist; on a bare-metal target */
/* the declarations are present but the definitions are not, so a link */
/* error is the honest answer if someone tries to use them.            */
/* ------------------------------------------------------------------ */
#define ARK_BUS_SOCK_RXBUF  (ARK_BUS_FRAME_MAX_SIZE)

typedef struct {
    int      fd;         /* connected / data socket               */
    int      listen_fd;  /* listening socket, or -1 when unused   */
    int      is_listen;  /* 1 when accept() must run before recv  */
    uint16_t rx_len;
    uint8_t  rx[ARK_BUS_SOCK_RXBUF];
} ark_bus_transport_sock_t;

/* Client: connect to host:port. */
int ark_bus_transport_tcp_connect(ark_bus_transport_sock_t *s,
                                  transport_adapter_t *tp,
                                  const char *host, uint16_t port);

/* Server: bind and listen on bind_ip:port; accept one client on first recv. */
int ark_bus_transport_tcp_listen(ark_bus_transport_sock_t *s,
                                 transport_adapter_t *tp,
                                 const char *bind_ip, uint16_t port);

/* UDP: send to host:port (host NULL => reply to last sender) and receive. */
int ark_bus_transport_udp_init(ark_bus_transport_sock_t *s,
                               transport_adapter_t *tp,
                               const char *host, uint16_t port);

void ark_bus_transport_sock_close(ark_bus_transport_sock_t *s);

#endif /* ARK_BUS_TRANSPORT_H */
