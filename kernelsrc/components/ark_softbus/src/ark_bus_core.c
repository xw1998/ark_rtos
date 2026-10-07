/*
 * ark_bus_core.c - the bus: topic registry, publishing, frame intake and the
 * event loop.  Platform independent; the only outside world it touches is the
 * transport adapter and the HAL.
 */
#include "ark_bus_priv.h"
#include "ark_bus_compress.h"

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
    bus->member_reap_ms = (uint32_t)ARK_BUS_MEMBER_REAP_MS;
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
#if ARK_BUS_TOOL_ENABLED
    ark_bus_tool_init(bus);
#endif
#if ARK_BUS_FLOW_ENABLED
    ark_bus_flow_init(bus);
#endif
#if ARK_BUS_ROUTE_ENABLED
    ark_bus_route_init(bus);
#endif
#if ARK_BUS_RAFT_ENABLED
    ark_bus_raft_init(bus);
#endif
#if ARK_BUS_FWU_ENABLED
    /* After stream_init: FWU takes over the data plane's inbound callback. */
    ark_bus_fwu_init(bus);
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

uint16_t ark_bus_next_seq(ark_bus_t *bus)
{
    uint16_t seq;

    if (!bus) {
        return 0u;
    }
    seq = bus->next_seq++;
    if (bus->next_seq == 0) {
        bus->next_seq = 1;   /* keep 0 unused as a sequence value */
    }
    return seq;
}

int ark_bus_build_frame(ark_bus_t *bus, uint32_t dst, uint8_t msg_type,
                        uint16_t topic_id, const void *payload, uint16_t len,
                        uint8_t flags, uint16_t seq, uint8_t *out,
                        uint16_t cap)
{
    ark_bus_frame_t f;
    int             total;

    if (!bus || !out) {
        return ARK_BUS_E_INVAL;
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
#if ARK_BUS_COMPRESS_ENABLED
    /* Held for the whole call: build_frame() below reads through `payload`,
     * which may point at this buffer, so it must outlive the compression
     * branch.  Only touched when the caller asks for compression. */
    uint8_t  comp[ARK_BUS_MSG_MAX_SIZE];
#endif
#if ARK_BUS_CRYPTO_ENABLED
    /* Holds [nonce ||] ciphertext || tag while the frame is built; lives only
     * when a key is installed, but is declared unconditionally so the seal
     * branch below needs no nested scope. */
    uint8_t  sec[ARK_BUS_WIRE_PAYLOAD_MAX];
#endif
    uint16_t seq;
    int      total;
    int      n;

    if (!bus || !bus->transport) {
        return ARK_BUS_E_INVAL;
    }

    bus_lock(bus);

    /* Reserve the sequence number before sealing: when the replay window is
     * enabled the tag covers it, so it has to exist first.  With the window off
     * the tag ignores it, and this is the same counter the old code bumped
     * inside build_frame. */
    seq = ark_bus_next_seq(bus);

#if ARK_BUS_COMPRESS_ENABLED
    /* If the caller asked for compression, try it here so every send path
     * benefits; "not worth it" or "no room" simply clears the flag and sends
     * the bytes as they are -- never a frame that claims to be smaller. */
    if ((flags & ARK_BUS_FLAG_COMPRESSED) != 0u && len > 0u) {
        int clen = ark_bus_compress((const uint8_t *)payload, len, comp,
                                    (uint16_t)sizeof(comp));
        if (clen > 0) {
            payload = comp;
            len     = (uint16_t)clen;
        } else {
            flags &= (uint8_t)~ARK_BUS_FLAG_COMPRESSED;
        }
    }
#endif

#if ARK_BUS_CRYPTO_ENABLED
    /* Authenticate last, on the bytes that actually go out (so a compressed
     * payload is tagged as compressed), and before build_frame() assigns `seq`
     * -- the tag covers the header fields an attacker would rewrite, not the
     * sequence number.  A bus with no key skips this entirely and behaves as
     * before.  With a key the send is fail-closed: if the tagged payload cannot
     * be assembled we return the error rather than emit it unauthenticated. */
    if (bus->crypto_key_set) {
        uint16_t seal_len = 0;
        int      src;

        flags |= ARK_BUS_FLAG_AUTHENTICATED;
        src = ark_bus_crypto_seal(bus, msg_type, flags, topic_id, bus->self_id,
                                  dst, seq, (const uint8_t *)payload, len,
                                  sec, (uint16_t)sizeof(sec), &seal_len);
        if (src != ARK_BUS_OK) {
            bus_unlock(bus);
            return src;
        }
        payload = sec;
        len     = seal_len;
    }
#endif

    total = ark_bus_build_frame(bus, dst, msg_type, topic_id, payload, len,
                                flags, seq, tx, (uint16_t)sizeof(tx));
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

#if ARK_BUS_FLOW_ENABLED
    /* Flow control owns the plain (non-QoS1) send path: it either hands the
     * frame to the link whole or queues it, and pushes back with E_AGAIN when
     * even the queue is full.  With depth 0 it is a straight passthrough. */
    n = ark_bus_flow_emit(bus, tx, (uint16_t)total, dst);
#else
    n = bus->transport->send(bus->transport->ctx, tx, (uint16_t)total);
    ark_bus_stats_tx(bus, n);
#endif
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

int ark_bus_publish_directed(ark_bus_t *bus, uint32_t target_id,
                             const char *topic, const void *payload,
                             uint16_t len, uint8_t flags)
{
    int id = ark_bus_topic_register(bus, topic);
    if (id < 0) {
        return id;
    }
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
    case ARK_BUS_MSG_LEAVE:
        ark_bus_discovery_handle(bus, f);
        break;

#if ARK_BUS_FLOW_ENABLED
    case ARK_BUS_MSG_FLOW_BUSY:
    case ARK_BUS_MSG_FLOW_READY:
        ark_bus_flow_handle(bus, f);
        break;
#endif

    case ARK_BUS_MSG_ACK:
        /* An ACK is directed back at whoever sent the QoS1 frame; a bystander
         * on a shared medium must not let it retire one of its own sessions. */
        if (f->dst_id == bus->self_id) {
            ark_bus_qos_handle_ack(bus, f);
        }
        break;

#if ARK_BUS_ROUTE_ENABLED
    case ARK_BUS_MSG_ROUTE:
    case ARK_BUS_MSG_ROUTE_ADV:
        /* Forwarding is not point-to-point: a ROUTE envelope is addressed to
         * its next hop but must also be acted on by that hop, and a ROUTE_ADV
         * is a broadcast anyone may learn from. */
        ark_bus_route_handle(bus, f);
        break;
#endif

#if ARK_BUS_RAFT_ENABLED
    case ARK_BUS_MSG_RAFT_VOTE_REQ:
    case ARK_BUS_MSG_RAFT_VOTE_REP:
    case ARK_BUS_MSG_RAFT_APPEND:
    case ARK_BUS_MSG_RAFT_APPEND_REP:
        /* Raft traffic is always directed at one peer (never broadcast), so a
         * node only acts on a message addressed to it.  The same gate the RPC
         * layer uses keeps a bystander on a shared medium from counting a vote
         * or appending an entry that was never sent to it. */
        if (f->dst_id == bus->self_id) {
            ark_bus_raft_handle(bus, f);
        }
        break;
#endif

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

#if ARK_BUS_HANDSHAKE_ENABLED
    case ARK_BUS_MSG_HANDSHAKE_REQ:
    case ARK_BUS_MSG_HANDSHAKE_REP:
        /* Handshakes are point-to-point; a node only answers one addressed to
         * it (or broadcast).  The frame has already been authenticated with
         * the pre-shared key by the crypto layer above. */
        if (f->dst_id == bus->self_id ||
            f->dst_id == ARK_BUS_ID_BROADCAST) {
            ark_bus_session_handle(bus, f);
        }
        break;
#endif

    case ARK_BUS_MSG_SUBSCRIBE:
    case ARK_BUS_MSG_UNSUBSCRIBE:
        /* Without the feature these frames are accepted and ignored (the local
         * subscription table is driven by ark_bus_subscribe()).  With it, a
         * node records which peer wants which topic for a hub/bridge to use. */
#if ARK_BUS_REMOTE_SUB_ENABLED
        ark_bus_subreg_handle(bus, f);
#endif
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
#if ARK_BUS_ENCRYPT_ENABLED
    /* Plaintext lands here when the frame is encrypted; open() repoints
     * f.payload at it, so it must outlive the dispatch below. */
    uint8_t         dec[ARK_BUS_MSG_MAX_SIZE];
#endif

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

#if ARK_BUS_CRYPTO_ENABLED
#if ARK_BUS_HANDSHAKE_ENABLED
    /* A revoked peer is refused outright until it handshakes again; the
     * handshake frame is the one exception, so that it can.  This runs before
     * the tag check on purpose: revoke() wipes the peer's session keys, so a
     * frame it sealed with them could never open anyway, and refusing it here
     * turns an indistinguishable E_AUTH into a diagnosable denial.  The src_id
     * is not yet authenticated at this point, but a forged one only earns the
     * same refusal, so no trust is placed in it. */
    if (ARK_BUS_IS_HANDSHAKE_TYPE(f.msg_type) == 0 &&
        ark_bus_session_denied(bus, f.src_id)) {
        bus->stats.rx_denied++;
        bus_unlock(bus);
        return ARK_BUS_E_DENIED;
    }
#endif
    /* Verify before anything downstream trusts the frame.  A keyed node refuses
     * frames with a bad tag and frames with no tag at all; an unkeyed node
     * refuses tagged frames it cannot verify.  Either way the frame is dropped
     * here, never decompressed and never dispatched. */
#if ARK_BUS_ENCRYPT_ENABLED
    rc = ark_bus_crypto_open(bus, &f, dec, (uint16_t)sizeof(dec));
#else
    rc = ark_bus_crypto_open(bus, &f, (uint8_t *)0, 0u);
#endif
    if (rc != ARK_BUS_OK) {
        bus->stats.rx_auth_fail++;
        bus_unlock(bus);
        return rc;
    }

#if ARK_BUS_REPLAY_ENABLED
    /* Only now, once the tag has vouched for the sequence number, is a
     * duplicate meaningful -- and only on a keyed bus: with no key the frames
     * are unauthenticated, so remembering their numbers would protect against
     * an attacker who can forge anything anyway.  QoS1 is exempt: a
     * retransmission reuses its sequence number by design, so the window would
     * flag every honest retry (see ark_bus_config.h). */
    if (bus->crypto_key_set && (f.flags & ARK_BUS_FLAG_QOS1) == 0u) {
        rc = ark_bus_replay_check(bus, &f);
        if (rc != ARK_BUS_OK) {
            bus->stats.rx_replay++;
            bus_unlock(bus);
            return rc;
        }
    }
#endif
#endif

#if ARK_BUS_COMPRESS_ENABLED
    /* Inflate before dispatch so subscribers, RPC handlers and the tool layer
     * all see plaintext; a stream we cannot decode stops here instead of being
     * handed on as if it were valid. */
    if ((f.flags & ARK_BUS_FLAG_COMPRESSED) != 0u) {
        uint8_t plain[ARK_BUS_MSG_MAX_SIZE];
        int     plen = ark_bus_decompress(f.payload, f.payload_len, plain,
                                          (uint16_t)sizeof(plain));
        if (plen < 0) {
            bus->stats.rx_bad_frames++;
            bus_unlock(bus);
            return ARK_BUS_E_FORMAT;
        }
        f.payload     = plain;
        f.payload_len = (uint16_t)plen;
        f.flags      &= (uint8_t)~ARK_BUS_FLAG_COMPRESSED;
    }
#endif

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
#if ARK_BUS_FLOW_ENABLED
    ark_bus_flow_tick(bus);
#endif
#if ARK_BUS_HANDSHAKE_ENABLED
    ark_bus_session_tick(bus);
#endif
    ark_bus_discovery_tick(bus);
#if ARK_BUS_ROUTE_ENABLED
    /* After discovery: it reads the device table discovery just refreshed. */
    ark_bus_route_tick(bus);
#endif
#if ARK_BUS_RAFT_ENABLED
    ark_bus_raft_tick(bus);
#endif
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
