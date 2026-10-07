/*
 * ark_bus_qos.c - "at least once" for directed messages.
 *
 * A directed publish is stored in a session slot and retransmitted every
 * ARK_BUS_QOS_RETRY_INTERVAL_MS until the peer's ACK arrives or the retry
 * budget runs out.  Broadcasts deliberately do not go through here: there is
 * no single peer to acknowledge them.
 */
#include "ark_bus_priv.h"

#if ARK_BUS_QOS_ENABLED

static ark_bus_qos_session_t *qos_alloc(ark_bus_t *bus)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_QOS_MAX_SESSIONS; i++) {
        if (!bus->qos[i].used) {
            return &bus->qos[i];
        }
    }
    return NULL;
}

int ark_bus_qos_submit(ark_bus_t *bus, uint32_t dst, uint16_t seq,
                       const uint8_t *frame, uint16_t len)
{
    ark_bus_qos_session_t *s;
    int                    n;

    if (!bus || !bus->transport || !frame || len > ARK_BUS_FRAME_MAX_SIZE) {
        return ARK_BUS_E_INVAL;
    }

    s = qos_alloc(bus);
    if (!s) {
        return ARK_BUS_E_FULL;
    }

    s->used        = 1;
    s->state       = 1;              /* awaiting ACK */
    s->dst_id      = dst;
    s->seq         = seq;
    s->sent_at_ms  = ark_bus_tick_ms();
    s->retries     = 0;
    s->frame_len   = len;
    ark_memcpy(s->frame, frame, len);

    n = bus->transport->send(bus->transport->ctx, s->frame, len);
    ark_bus_stats_tx(bus, n);
    if (n <= 0) {
        s->used = 0;
        return ARK_BUS_E_AGAIN;
    }
    return ARK_BUS_OK;
}

void ark_bus_qos_tick(ark_bus_t *bus)
{
    uint32_t now;
    uint8_t  i;

    if (!bus || !bus->transport) {
        return;
    }
    now = ark_bus_tick_ms();

    for (i = 0; i < ARK_BUS_QOS_MAX_SESSIONS; i++) {
        ark_bus_qos_session_t *s = &bus->qos[i];
        int                    n;

        if (!s->used) {
            continue;
        }
        if ((uint32_t)(now - s->sent_at_ms) <
            (uint32_t)ARK_BUS_QOS_RETRY_INTERVAL_MS) {
            continue;
        }

        s->sent_at_ms = now;

        if (s->retries >= (uint8_t)ARK_BUS_QOS_RETRY_MAX) {
            s->used  = 0;
            s->state = 0;
            bus->stats.qos_timeouts++;
            continue;
        }

        s->retries++;
        n = bus->transport->send(bus->transport->ctx, s->frame, s->frame_len);
        ark_bus_stats_tx(bus, n);
        bus->stats.qos_retransmits++;
    }
}

void ark_bus_qos_handle_ack(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    uint16_t ack_seq;
    uint8_t  i;

    if (!bus || f->payload_len < 2) {
        return;
    }
    ack_seq = ark_rd16(f->payload);

    for (i = 0; i < ARK_BUS_QOS_MAX_SESSIONS; i++) {
        ark_bus_qos_session_t *s = &bus->qos[i];
        if (s->used && s->dst_id == f->src_id && s->seq == ack_seq) {
            s->used  = 0;
            s->state = 2;            /* done */
            return;
        }
    }
}

void ark_bus_qos_reset(ark_bus_t *bus)
{
    if (bus) {
        ark_memset(bus->qos, 0, sizeof(bus->qos));
    }
}

#else /* QoS compiled out: the entry points stay, the machinery does not. */

int ark_bus_qos_submit(ark_bus_t *bus, uint32_t dst, uint16_t seq,
                       const uint8_t *frame, uint16_t len)
{
    (void)bus; (void)dst; (void)seq; (void)frame; (void)len;
    return ARK_BUS_E_NOSYS;
}

void ark_bus_qos_tick(ark_bus_t *bus) { (void)bus; }
void ark_bus_qos_handle_ack(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    (void)bus; (void)f;
}
void ark_bus_qos_reset(ark_bus_t *bus) { (void)bus; }

#endif /* ARK_BUS_QOS_ENABLED */
