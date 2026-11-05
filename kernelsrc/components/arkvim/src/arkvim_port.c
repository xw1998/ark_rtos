/* ============================================================
 * arkvim_port.c - Port Dispatch
 *
 * Holds the currently-installed port pointer and provides the
 * thin wrappers the editor calls. Keeping the dispatch in one
 * file means editor sources never name a concrete transport.
 * ============================================================ */
#include "arkvim_port.h"

static const arkvim_port_t *s_port = 0;

void arkvim_port_init(const arkvim_port_t *port)
{
    s_port = port;
}

const arkvim_port_t *arkvim_port_get(void)
{
    return s_port;
}

void arkvim_port_send_byte(uint8_t b)
{
    if (s_port && s_port->send_byte) {
        s_port->send_byte(b);
    }
}

void arkvim_port_send_string(const char *s)
{
    if (s_port && s_port->send_string && s) {
        s_port->send_string(s);
    }
}

void arkvim_port_send_buf(const uint8_t *buf, uint16_t len)
{
    if (s_port && s_port->send_buf && buf && len > 0) {
        s_port->send_buf(buf, len);
    }
}

int arkvim_port_recv_byte(void)
{
    if (s_port && s_port->recv_byte) {
        return s_port->recv_byte();
    }
    return -1;
}

uint32_t arkvim_port_tick_ms(void)
{
    if (s_port && s_port->tick_ms) {
        return s_port->tick_ms();
    }
    return 0;
}
