/*
 * ark_bus_core.c - the bus: topic registry, publishing, frame intake and the
 * event loop.  Platform independent; the only outside world it touches is the
 * transport adapter and the HAL.
 */
#include "ark_bus_priv.h"

/* ------------------------------------------------------------------ */
/* Critical section                                                    */
/*                                                                     */
/* The HAL may hand us a plain (non-recursive) mutex, yet a subscriber  */
/* callback can publish from inside a dispatch.  A depth counter makes  */
/* the section reentrant, so the same mutex is taken exactly once.     */
/* ------------------------------------------------------------------ */
static void bus_lock(ark_bus_t *bus)
{
    if (bus->lock_depth == 0) {
        ark_bus_lock();
    }
    bus->lock_depth++;
}

static void bus_unlock(ark_bus_t *bus)
{
    if (bus->lock_depth == 0) {
        return;
    }
    bus->lock_depth--;
    if (bus->lock_depth == 0) {
        ark_bus_unlock();
    }
}

/* ------------------------------------------------------------------ */
/* Topic registry                                                      */
/* ------------------------------------------------------------------ */

uint16_t ark_bus_topic_hash(const char *name)
{
    uint32_t h = 2166136261u; /* FNV-1a offset basis */

    if (!name) {
        return 0;
    }
    while (*name) {
        h ^= (uint8_t)*name++;
        h *= 16777619u;
    }
    h &= 0x7FFFu;             /* 15 bits, so the id always fits a positive int */
    if (h == 0) {
        h = 1u;               /* 0 is reserved for "no topic" */
    }
    return (uint16_t)h;
}

static ark_bus_topic_t *topic_find_by_id(ark_bus_t *bus, uint16_t id)
{
    uint8_t i;
    for (i = 0; i < bus->topic_count; i++) {
        if (bus->topics[i].id == id) {
            return &bus->topics[i];
        }
    }
    return NULL;
}

int ark_bus_topic_register(ark_bus_t *bus, const char *name)
{
    uint16_t        id;
    ark_bus_topic_t *t;

    if (!bus || !name || !*name) {
        return ARK_BUS_E_INVAL;
    }

    id = ark_bus_topic_hash(name);
    t = topic_find_by_id(bus, id);
    if (t) {
        return (int)t->id;
    }
    if (bus->topic_count >= ARK_BUS_MAX_TOPICS) {
        return ARK_BUS_E_FULL;
    }

    t = &bus->topics[bus->topic_count];
    ark_strncpy(t->name, name, ARK_BUS_TOPIC_NAME_MAX);
    t->id       = id;
    t->used     = 1;
    t->callback = NULL;
    t->user     = NULL;
    bus->topic_count++;
    return (int)id;
}

int ark_bus_topic_lookup(const ark_bus_t *bus, const char *name)
{
    ark_bus_topic_t *t;

    if (!bus || !name || !*name) {
        return ARK_BUS_E_INVAL;
    }
    t = topic_find_by_id((ark_bus_t *)bus, ark_bus_topic_hash(name));
    return t ? (int)t->id : ARK_BUS_E_NOTFOUND;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void ark_bus_init(ark_bus_t *bus, transport_adapter_t *tport)
{
    if (!bus) {
        return;
    }
    ark_memset(bus, 0, sizeof(*bus));
    bus->transport = tport;
    bus->next_seq  = 1;
#if ARK_BUS_TASK_ENABLED
    ark_bus_task_init(bus);
#endif
#if ARK_BUS_STREAM_ENABLED
    ark_bus_stream_init(bus);
#endif
#if ARK_BUS_COLL_ENABLED
    ark_bus_coll_init(bus);
#endif
#if ARK_BUS_INFER_ENABLED
    ark_bus_infer_init(bus);
#endif
}

int ark_bus_start(ark_bus_t *bus, uint32_t self_id,
                  uint8_t device_type, uint32_t capabilities)
{
    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }

    bus->self_id          = self_id;
    bus->device_type      = device_type;
    bus->capabilities     = capabilities;
    bus->next_seq         = 1;
    bus->started          = 1;
    bus->last_heartbeat_ms = ark_bus_tick_ms();
    bus->last_cleanup_ms   = bus->last_heartbeat_ms;

#if ARK_BUS_RPC_ENABLED
    /* Make PING and DESCRIBE answerable out of the box; an application that
     * registered its own handler first keeps it. */
    (void)ark_bus_register_system_methods(bus);
#endif

    return ark_bus_announce(bus);
}

void ark_bus_stop(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    bus->started = 0;
    ark_bus_qos_reset(bus);
}

/* ------------------------------------------------------------------ */
/* Frame building / sending                                            */
/* ------------------------------------------------------------------ */

int ark_bus_build_frame(ark_bus_t *bus, uint32_t dst, uint8_t msg_type,
                        uint16_t topic_id, const void *payload, uint16_t len,
                        uint8_t flags, uint8_t *out, uint16_t cap,
                        uint16_t *seq_out)
{
    ark_bus_frame_t f;
    uint16_t        seq;
    int             total;

    if (!bus || !out) {
        return ARK_BUS_E_INVAL;
    }

    seq = bus->next_seq++;
    if (bus->next_seq == 0) {
        bus->next_seq = 1;   /* keep 0 unused as a sequence value */
    }

    ark_memset(&f, 0, sizeof(f));
    f.version     = ARK_BUS_VERSION;
    f.msg_type    = msg_type;
    f.flags       = flags;
    f.topic_id    = topic_id;
    f.seq         = seq;
    f.src_id      = bus->self_id;
    f.dst_id      = dst;
    f.payload     = (const uint8_t *)payload;
    f.payload_len = len;

    total = ark_bus_frame_encode(&f, out, cap);
    if (total >= 0 && seq_out) {
        *seq_out = seq;
    }
    return total;
}

void ark_bus_stats_tx(ark_bus_t *bus, int n)
{
    if (!bus || n <= 0) {
        return;
    }
    bus->stats.tx_frames++;
    bus->stats.tx_bytes += (uint32_t)n;
}

int ark_bus_emit(ark_bus_t *bus, uint32_t dst, uint8_t msg_type,
                 uint16_t topic_id, const void *payload, uint16_t len,
                 uint8_t flags)
{
    uint8_t  tx[ARK_BUS_FRAME_MAX_SIZE];
    uint16_t seq = 0;
    int      total;
    int      n;

    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }

    bus_lock(bus);

    total = ark_bus_build_frame(bus, dst, msg_type, topic_id, payload, len,
                                flags, tx, (uint16_t)sizeof(tx), &seq);
    if (total < 0) {
        bus_unlock(bus);
        return total;
    }

#if ARK_BUS_QOS_ENABLED
    if ((flags & ARK_BUS_FLAG_QOS1) != 0u && dst != ARK_BUS_ID_BROADCAST) {
        if (ark_bus_qos_submit(bus, dst, seq, tx, (uint16_t)total) == ARK_BUS_OK) {
            bus_unlock(bus);
            return total;
        }
        /* No free session: fall through and send once, untracked. */
    }
#endif

    n = bus->transport->send(bus->transport->ctx, tx, (uint16_t)total);
    ark_bus_stats_tx(bus, n);
    bus_unlock(bus);
    return n;
}

/* ------------------------------------------------------------------ */
/* Publish / subscribe                                                 */
/* ------------------------------------------------------------------ */

int ark_bus_publish(ark_bus_t *bus, const char *topic,
                    const void *payload, uint16_t len)
{
    int id = ark_bus_topic_register(bus, topic);
    if (id < 0) {
        return id;
    }
    return ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_PUBLISH,
                        (uint16_t)id, payload, len, 0);
}

int ark_bus_publish_to(ark_bus_t *bus, uint32_t target_id, const char *topic,
                       const void *payload, uint16_t len)
{
    int     id = ark_bus_topic_register(bus, topic);
    uint8_t flags = 0;

    if (id < 0) {
        return id;
    }
#if ARK_BUS_QOS_ENABLED
    flags = ARK_BUS_FLAG_QOS1;
#endif
    return ark_bus_emit(bus, target_id, ARK_BUS_MSG_DIRECT,
                        (uint16_t)id, payload, len, flags);
}

int ark_bus_publish_raw(ark_bus_t *bus, const char *topic,
                        const void *payload, uint16_t len, uint8_t flags)
{
    int id = ark_bus_topic_register(bus, topic);
    if (id < 0) {
        return id;
    }
    /* A raw broadcast never carries QoS1: there is no single peer to ACK. */
    return ark_bus_emit(bus, ARK_BUS_ID_BROADCAST, ARK_BUS_MSG_PUBLISH,
                        (uint16_t)id, payload, len,
                        (uint8_t)(flags & ~ARK_BUS_FLAG_QOS1));
}

int ark_bus_subscribe(ark_bus_t *bus, const char *topic,
                      ark_bus_msg_cb_t callback, void *user)
{
    int             id = ark_bus_topic_register(bus, topic);
    ark_bus_topic_t *t;

    if (id < 0) {
        return id;
    }
    t = topic_find_by_id(bus, (uint16_t)id);
    if (!t) {
        return ARK_BUS_E_NOTFOUND;
    }
    t->callback = callback;
    t->user     = user;
    return id;
}

int ark_bus_unsubscribe(ark_bus_t *bus, const char *topic)
{
    ark_bus_topic_t *t;

    if (!bus || !topic) {
        return ARK_BUS_E_INVAL;
    }
    t = topic_find_by_id(bus, ark_bus_topic_hash(topic));
    if (!t) {
        return ARK_BUS_E_NOTFOUND;
    }
    t->callback = NULL;
    t->user     = NULL;
    return ARK_BUS_OK;
}

/* ------------------------------------------------------------------ */
/* Frame intake                                                        */
/* ------------------------------------------------------------------ */

static void deliver_topic(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_topic_t *t;

    if (f->dst_id != ARK_BUS_ID_BROADCAST && f->dst_id != bus->self_id) {
        return; /* addressed to somebody else */
    }
    if (f->topic_id == ARK_BUS_TOPIC_NONE) {
        return;
    }

    t = topic_find_by_id(bus, f->topic_id);
    if (!t) {
        bus->stats.dispatch_errors++;
        return;
    }
    if (t->callback) {
        t->callback(t->name, f->payload, f->payload_len, t->user);
    } else {
        bus->stats.dispatch_errors++;
    }
}

/*
 * A QoS1 frame addressed to us is acknowledged so the sender can retire its
 * retransmit session.  This applies to every addressed message type, not just
 * DIRECT: an RPC request or reply that were never ACKed would be re-sent, and
 * the receiver would run the same method twice.
 */
static void ack_if_qos1(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    uint8_t p[2];

    if (f->dst_id != bus->self_id ||
        (f->flags & ARK_BUS_FLAG_QOS1) == 0u) {
        return;
    }
    ark_wr16(p, f->seq);
    (void)ark_bus_emit(bus, f->src_id, ARK_BUS_MSG_ACK,
                       ARK_BUS_TOPIC_NONE, p, 2u, 0);
}

int ark_bus_dispatch(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    int rc = ARK_BUS_OK;

    if (!bus || !f) {
        return ARK_BUS_E_INVAL;
    }

    bus_lock(bus);

    /* Ignore our own frames echoed back by a loopback link. */
    if (bus->self_id != 0 && f->src_id == bus->self_id) {
        bus_unlock(bus);
        return ARK_BUS_OK;
    }

    switch (f->msg_type) {
    case ARK_BUS_MSG_PUBLISH:
        deliver_topic(bus, f);
        break;

    case ARK_BUS_MSG_DIRECT:
        deliver_topic(bus, f);
        ack_if_qos1(bus, f);
        break;

    case ARK_BUS_MSG_DISCOVER:
    case ARK_BUS_MSG_DISCOVER_REPLY:
    case ARK_BUS_MSG_HEARTBEAT:
        ark_bus_discovery_handle(bus, f);
        break;

    case ARK_BUS_MSG_ACK:
        /* An ACK is directed back at whoever sent the QoS1 frame; a bystander
         * on a shared medium must not let it retire one of its own sessions. */
        if (f->dst_id == bus->self_id) {
            ark_bus_qos_handle_ack(bus, f);
        }
        break;

#if ARK_BUS_STREAM_ENABLED
    case ARK_BUS_MSG_STREAM:
    case ARK_BUS_MSG_STREAM_ACK:
        /* Bulk transfer is point-to-point, so (like RPC) both ends must
         * confirm the frame is for them; a hub fanning frames to every client
         * would otherwise make every worker assemble the same object. */
        if (f->dst_id == bus->self_id) {
            ark_bus_stream_handle(bus, f);
        }
        break;
#endif

    case ARK_BUS_MSG_SUBSCRIBE:
    case ARK_BUS_MSG_UNSUBSCRIBE:
        /* Remote subscription intent is host policy; the local registry is
         * driven by ark_bus_subscribe().  Acknowledge silently. */
        break;

#if ARK_BUS_RPC_ENABLED
    case ARK_BUS_MSG_RPC_REQ:
    case ARK_BUS_MSG_RPC_REP:
        /* RPC is always directed (never broadcast), so a node only acts on a
         * request or reply addressed to it.  Without this gate every peer on a
         * shared medium -- a bus, or a star hub that fans frames out to all
         * clients -- would run the same method, and every worker would answer. */
        if (f->dst_id == bus->self_id) {
            ark_bus_rpc_handle(bus, f);
        }
        ack_if_qos1(bus, f);
        break;
#endif

    default:
        rc = ARK_BUS_E_NOSYS;
        break;
    }

    bus_unlock(bus);
    return rc;
}

int ark_bus_feed(ark_bus_t *bus, const uint8_t *data, uint16_t len)
{
    ark_bus_frame_t f;
    int             rc;

    if (!bus || !data) {
        return ARK_BUS_E_INVAL;
    }

    bus_lock(bus);

    rc = ark_bus_frame_decode(data, len, &f);
    if (rc == ARK_BUS_E_CRC) {
        bus->stats.rx_crc_errors++;
        bus_unlock(bus);
        return rc;
    }
    if (rc != ARK_BUS_OK) {
        bus->stats.rx_bad_frames++;
        bus_unlock(bus);
        return rc;
    }

    bus->stats.rx_frames++;
    rc = ark_bus_dispatch(bus, &f);
    bus_unlock(bus);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Event loop                                                          */
/* ------------------------------------------------------------------ */

void ark_bus_poll(ark_bus_t *bus)
{
    uint8_t rx[ARK_BUS_FRAME_MAX_SIZE];
    int     i;
    int     n;

    if (!bus || !bus->transport) {
        return;
    }

    bus_lock(bus);

    /* Bound the work per call so a chatty link cannot starve the caller. */
    for (i = 0; i < 8; i++) {
        n = bus->transport->recv(bus->transport->ctx, rx, (uint16_t)sizeof(rx));
        if (n <= 0) {
            break;
        }
        bus->stats.rx_bytes += (uint32_t)n;
        (void)ark_bus_feed(bus, rx, (uint16_t)n);
    }

    ark_bus_qos_tick(bus);
    ark_bus_discovery_tick(bus);
#if ARK_BUS_RPC_ENABLED
    ark_bus_rpc_tick(bus);
#endif
#if ARK_BUS_TASK_ENABLED
    ark_bus_task_tick(bus);
#endif
#if ARK_BUS_STREAM_ENABLED
    ark_bus_stream_tick(bus);
#endif
#if ARK_BUS_COLL_ENABLED
    ark_bus_coll_tick(bus);
#endif
#if ARK_BUS_INFER_ENABLED
    /* After the collective layer: a finished all-reduce is picked up here, so
     * the executor never starts a new collective from inside a callback. */
    ark_bus_infer_tick(bus);
#endif

    bus_unlock(bus);
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

uint8_t ark_bus_device_count(const ark_bus_t *bus)
{
    return bus ? bus->device_count : 0;
}

const ark_bus_device_t *ark_bus_device_at(const ark_bus_t *bus, uint8_t index)
{
    if (!bus || index >= bus->device_count) {
        return NULL;
    }
    return &bus->devices[index];
}

const ark_bus_device_t *ark_bus_device_find(const ark_bus_t *bus, uint32_t id)
{
    uint8_t i;
    if (!bus) {
        return NULL;
    }
    for (i = 0; i < bus->device_count; i++) {
        if (bus->devices[i].id == id) {
            return &bus->devices[i];
        }
    }
    return NULL;
}

const ark_bus_stats_t *ark_bus_get_stats(const ark_bus_t *bus)
{
    return bus ? &bus->stats : NULL;
}
