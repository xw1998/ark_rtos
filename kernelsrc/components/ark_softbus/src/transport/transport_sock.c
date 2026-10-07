/*
 * transport_sock.c - TCP and UDP transports over sockets.
 *
 * The body is written against the BSD socket API and built on both POSIX and
 * Winsock; a small shim below maps the few differences (close, non-blocking
 * mode, would-block errors).  Where sockets do not exist (a bare-metal MCU) the
 * three entry points are still defined, but they answer ARK_BUS_E_NOSYS instead
 * of pretending a link is up.  The declarations in ark_bus_transport.h are
 * always present so the header can be included unconditionally.
 */
#include "ark_bus_transport.h"

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__) || \
    defined(_WIN32) || defined(ARK_BUS_HAVE_SOCKETS)

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string.h>
#define ARK_SOCK_CLOSE(fd)      closesocket(fd)
#define ARK_SOCK_SHUTDOWN(fd)   shutdown(fd, SD_BOTH)
static int ark_sock_would_block(void)
{
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS;
}
static int ark_sock_set_nonblock(int fd)
{
    u_long one = 1;
    return ioctlsocket(fd, FIONBIO, &one);
}
static void ark_sock_startup(void)
{
    static int done = 0;
    WSADATA wsa;
    if (!done) {
        (void)WSAStartup(MAKEWORD(2, 2), &wsa);
        done = 1;
    }
}
/* MinGW.org's ws2tcpip.h does not always expose inet_pton; use inet_addr
 * (the address space we accept here is IPv4 dotted quad only). */
static int ark_sock_inet_pton(const char *src, struct in_addr *dst)
{
    unsigned long a = inet_addr(src);
    if (a == INADDR_NONE && strcmp(src, "255.255.255.255") != 0) {
        return 0;
    }
    dst->s_addr = a;
    return 1;
}
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#define ARK_SOCK_CLOSE(fd)      close(fd)
#define ARK_SOCK_SHUTDOWN(fd)   shutdown(fd, SHUT_RDWR)
static int ark_sock_would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
static int ark_sock_set_nonblock(int fd)
{
    return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
static void ark_sock_startup(void) { }
static int ark_sock_inet_pton(const char *src, struct in_addr *dst)
{
    return inet_pton(AF_INET, src, dst) == 1;
}
#endif

/* recvfrom() takes int* on Winsock and socklen_t* on POSIX. */
#if defined(_WIN32)
typedef int ark_socklen_t;
#else
typedef socklen_t ark_socklen_t;
#endif

#define SOCK_CTX(tp)  ((ark_bus_transport_sock_t *)(tp)->ctx)

/* Try to lift one complete frame out of s->rx.  Returns the frame length, 0
 * when more bytes are needed, and drops a stray byte when the stream is out of
 * sync.  `max_len` is the caller's buffer size. */
static int sock_extract(ark_bus_transport_sock_t *s, uint8_t *buf, uint16_t max_len)
{
    uint16_t plen;
    uint16_t total;

    if (s->rx_len < 11u) {
        return 0;
    }
    if (s->rx[0] != 0xAAu || s->rx[1] != 0x55u) {
        memmove(s->rx, s->rx + 1, (size_t)(s->rx_len - 1));
        s->rx_len--;
        return 0;
    }
    plen = (uint16_t)(((uint16_t)s->rx[9] << 8) | s->rx[10]);
    total = ark_bus_frame_size(plen);
    if (plen > ARK_BUS_MSG_MAX_SIZE || total > max_len) {
        memmove(s->rx, s->rx + 1, (size_t)(s->rx_len - 1));
        s->rx_len--;
        return 0;
    }
    if (s->rx_len < total) {
        return 0;
    }
    memcpy(buf, s->rx, total);
    if (s->rx_len > total) {
        memmove(s->rx, s->rx + total, (size_t)(s->rx_len - total));
    }
    s->rx_len = (uint16_t)(s->rx_len - total);
    return (int)total;
}

static int sock_send(void *ctx, const uint8_t *data, uint16_t len)
{
    ark_bus_transport_sock_t *s = (ark_bus_transport_sock_t *)ctx;
    int n;
    if (!s || s->fd < 0) {
        return ARK_BUS_E_STATE;
    }
#ifdef MSG_NOSIGNAL
    n = (int)send(s->fd, (const char *)data, len, MSG_NOSIGNAL);
#else
    n = (int)send(s->fd, (const char *)data, len, 0);
#endif
    return n > 0 ? n : ARK_BUS_E_AGAIN;
}

static int sock_recv(void *ctx, uint8_t *buf, uint16_t max_len)
{
    ark_bus_transport_sock_t *s = (ark_bus_transport_sock_t *)ctx;
    int rc;

    if (!s) {
        return ARK_BUS_E_INVAL;
    }

    if (s->is_listen && s->fd < 0 && s->listen_fd >= 0) {
        int c = (int)accept(s->listen_fd, NULL, NULL);
        if (c < 0) {
            return 0;              /* no client yet */
        }
        ark_sock_set_nonblock(c);
        s->fd = c;
    }
    if (s->fd < 0) {
        return 0;
    }

    rc = sock_extract(s, buf, max_len);
    if (rc != 0) {
        return rc;
    }

    for (;;) {
        int n;
        int room = (int)(sizeof(s->rx) - s->rx_len);
        if (room <= 0) {
            return 0;
        }
        n = (int)recv(s->fd, (char *)(s->rx + s->rx_len), room, 0);
        if (n > 0) {
            s->rx_len = (uint16_t)(s->rx_len + n);
            rc = sock_extract(s, buf, max_len);
            if (rc != 0) {
                return rc;
            }
            if (n < room) {
                return 0;          /* drained for now */
            }
            continue;
        }
        if (n == 0) {
            return 0;              /* peer closed */
        }
        if (ark_sock_would_block()) {
            return 0;
        }
        return ARK_BUS_E_AGAIN;
    }
}

static void sock_setup(ark_bus_transport_sock_t *s, transport_adapter_t *tp,
                       const char *name)
{
    tp->send = sock_send;
    tp->recv = sock_recv;
    tp->name = name;
    tp->ctx  = s;
}

int ark_bus_transport_tcp_connect(ark_bus_transport_sock_t *s,
                                  transport_adapter_t *tp,
                                  const char *host, uint16_t port)
{
    struct sockaddr_in addr;

    if (!s || !tp || !host) {
        return ARK_BUS_E_INVAL;
    }
    ark_sock_startup();
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->listen_fd = -1;

    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0) {
        return ARK_BUS_E_AGAIN;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (!ark_sock_inet_pton(host, &addr.sin_addr)) {
        ARK_SOCK_CLOSE(s->fd);
        s->fd = -1;
        return ARK_BUS_E_INVAL;
    }
    if (connect(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ARK_SOCK_CLOSE(s->fd);
        s->fd = -1;
        return ARK_BUS_E_AGAIN;
    }
    ark_sock_set_nonblock(s->fd);
    sock_setup(s, tp, "TCP");
    return ARK_BUS_OK;
}

int ark_bus_transport_tcp_listen(ark_bus_transport_sock_t *s,
                                 transport_adapter_t *tp,
                                 const char *bind_ip, uint16_t port)
{
    struct sockaddr_in addr;
    int                on = 1;

    if (!s || !tp) {
        return ARK_BUS_E_INVAL;
    }
    ark_sock_startup();
    memset(s, 0, sizeof(*s));
    s->fd = -1;

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) {
        return ARK_BUS_E_AGAIN;
    }
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&on, (int)sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    addr.sin_addr.s_addr = bind_ip ? inet_addr(bind_ip) : htonl(INADDR_ANY);
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(s->listen_fd, 1) < 0) {
        ARK_SOCK_CLOSE(s->listen_fd);
        s->listen_fd = -1;
        return ARK_BUS_E_AGAIN;
    }
    ark_sock_set_nonblock(s->listen_fd);
    s->is_listen = 1;
    sock_setup(s, tp, "TCP-LISTEN");
    return ARK_BUS_OK;
}

static int sock_send_udp(void *ctx, const uint8_t *data, uint16_t len)
{
    ark_bus_transport_sock_t *s = (ark_bus_transport_sock_t *)ctx;
    int n;
    if (!s || s->fd < 0) {
        return ARK_BUS_E_STATE;
    }
    if (s->connected) {
        n = (int)send(s->fd, (const char *)data, len, 0);
    } else if (s->peer_len > 0) {
        n = (int)sendto(s->fd, (const char *)data, len, 0,
                        (const struct sockaddr *)s->peer, (int)s->peer_len);
    } else {
        return ARK_BUS_E_STATE;   /* no peer learned yet (host == NULL) */
    }
    return n > 0 ? n : ARK_BUS_E_AGAIN;
}

/* One datagram == one frame.  Read one datagram, then validate it; a datagram
 * that is not a whole frame is discarded, never left half-parsed (a UDP peer
 * cannot send a continuation of it). */
static int sock_recv_udp(void *ctx, uint8_t *buf, uint16_t max_len)
{
    ark_bus_transport_sock_t *s = (ark_bus_transport_sock_t *)ctx;
    int rc;

    if (!s || s->fd < 0) {
        return 0;
    }

    if (s->rx_len == 0) {
        int n;
        if (s->connected) {
            n = (int)recv(s->fd, (char *)s->rx, (int)sizeof(s->rx), 0);
        } else {
            ark_socklen_t plen = (ark_socklen_t)sizeof(s->peer);
            n = (int)recvfrom(s->fd, (char *)s->rx, (int)sizeof(s->rx), 0,
                              (struct sockaddr *)s->peer, &plen);
            if (n > 0) {
                s->peer_len = (uint8_t)plen;
            }
        }
        if (n > 0) {
            s->rx_len = (uint16_t)n;
        } else if (n == 0) {
            return 0;              /* empty datagram */
        } else if (ark_sock_would_block()) {
            return 0;
        } else {
            return ARK_BUS_E_AGAIN;
        }
    }

    rc = sock_extract(s, buf, max_len);
    if (rc == 0) {
        s->rx_len = 0;             /* malformed / oversized datagram: drop it */
        return 0;
    }
    return rc;
}

int ark_bus_transport_udp_init(ark_bus_transport_sock_t *s,
                               transport_adapter_t *tp,
                               const char *host, uint16_t port)
{
    struct sockaddr_in addr;

    if (!s || !tp) {
        return ARK_BUS_E_INVAL;
    }
    ark_sock_startup();
    memset(s, 0, sizeof(*s));
    s->fd = -1;
    s->listen_fd = -1;

    s->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (s->fd < 0) {
        return ARK_BUS_E_AGAIN;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (host && host[0]) {
        /* Client: fix the peer by connect()ing the datagram socket. */
        if (!ark_sock_inet_pton(host, &addr.sin_addr)) {
            ARK_SOCK_CLOSE(s->fd);
            s->fd = -1;
            return ARK_BUS_E_INVAL;
        }
        if (connect(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ARK_SOCK_CLOSE(s->fd);
            s->fd = -1;
            return ARK_BUS_E_AGAIN;
        }
        memcpy(s->peer, &addr, sizeof(addr));
        s->peer_len  = (uint8_t)sizeof(addr);
        s->connected = 1;
    } else {
        /* Server / peer-less: bind and learn the peer from the first datagram. */
        int on = 1;
        setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR,
                   (const char *)&on, (int)sizeof(on));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ARK_SOCK_CLOSE(s->fd);
            s->fd = -1;
            return ARK_BUS_E_AGAIN;
        }
        s->connected = 0;
        s->peer_len  = 0;
    }

    ark_sock_set_nonblock(s->fd);
    tp->send = sock_send_udp;
    tp->recv = sock_recv_udp;
    tp->name = "UDP";
    tp->ctx  = s;
    return ARK_BUS_OK;
}

void ark_bus_transport_sock_close(ark_bus_transport_sock_t *s)
{
    if (!s) {
        return;
    }
    if (s->fd >= 0) {
        ARK_SOCK_CLOSE(s->fd);
        s->fd = -1;
    }
    if (s->listen_fd >= 0) {
        ARK_SOCK_CLOSE(s->listen_fd);
        s->listen_fd = -1;
    }
    s->rx_len = 0;
}

#else /* no sockets on this platform */

int ark_bus_transport_tcp_connect(ark_bus_transport_sock_t *s,
                                  transport_adapter_t *tp,
                                  const char *host, uint16_t port)
{
    (void)s; (void)tp; (void)host; (void)port;
    return ARK_BUS_E_NOSYS;
}

int ark_bus_transport_tcp_listen(ark_bus_transport_sock_t *s,
                                 transport_adapter_t *tp,
                                 const char *bind_ip, uint16_t port)
{
    (void)s; (void)tp; (void)bind_ip; (void)port;
    return ARK_BUS_E_NOSYS;
}

int ark_bus_transport_udp_init(ark_bus_transport_sock_t *s,
                               transport_adapter_t *tp,
                               const char *host, uint16_t port)
{
    (void)s; (void)tp; (void)host; (void)port;
    return ARK_BUS_E_NOSYS;
}

void ark_bus_transport_sock_close(ark_bus_transport_sock_t *s)
{
    (void)s;
}

#endif /* socket support */
