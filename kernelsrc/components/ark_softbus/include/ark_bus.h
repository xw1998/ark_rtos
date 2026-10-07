/*
 * ark_bus.h - the ark_softbus core.
 *
 * A bus owns a topic registry, a device table and (optionally) a small set of
 * QoS1 sessions, and it sits on top of exactly one transport adapter.  There is
 * no hidden allocation and no libc: every table is a static array sized by
 * ark_bus_config.h, so a bus struct is a plain value the caller can put in
 * .bss on an MCU or on the stack in a test.
 *
 * Typical MCU use:
 *
 *     static ark_bus_t bus;
 *     ark_bus_init(&bus, uart_adapter);
 *     ark_bus_subscribe(&bus, "sensor/temp", on_temp, NULL);
 *     ark_bus_start(&bus, 0x1234, DEV_SENSOR, CAP_TEMP);
 *     for (;;) { uart_pump(); ark_bus_poll(&bus); }
 */
#ifndef ARK_BUS_H
#define ARK_BUS_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus_protocol.h"
#include "ark_bus_hal.h"
#include "ark_bus_transport.h"
#include "ark_bus_stream.h"
#include "ark_bus_coll.h"
#include "ark_bus_infer.h"
#include "ark_bus_tool.h"
#include "ark_bus_flow.h"
#include "ark_bus_route.h"
#include "ark_bus_raft.h"
#include "ark_bus_fwu.h"
#include "ark_bus_crypto.h"

/* In-flight QoS1 session: the encoded frame plus its retry bookkeeping. */
typedef struct {
    uint8_t  used;
    uint8_t  state;    /* 0 free, 1 awaiting ACK, 2 done            */
    uint32_t dst_id;
    uint16_t seq;
    uint32_t sent_at_ms;
    uint8_t  retries;
    uint16_t frame_len;
    uint8_t  frame[ARK_BUS_FRAME_MAX_SIZE];
} ark_bus_qos_session_t;

/*
 * Membership changes a node cares about:
 *   JOIN    a peer appeared (first seen, or back after being offline)
 *   LEAVE   a peer announced it is leaving (ark_bus_leave on its side)
 *   OFFLINE a peer missed ARK_BUS_DEVICE_TIMEOUT_MS of heartbeats
 *   REAPED  a peer was dropped from the table (see the reap window below)
 *
 * `event` is an ark_bus_member_event_t; `type`/`capabilities` describe the
 * peer (last known values for LEAVE/OFFLINE/REAPED).  The callback runs from
 * inside dispatch, so it may call back into the bus (the critical section is
 * reentrant); keep it short.
 */
typedef enum {
    ARK_BUS_MEMBER_JOIN    = 0,
    ARK_BUS_MEMBER_LEAVE   = 1,
    ARK_BUS_MEMBER_OFFLINE = 2,
    ARK_BUS_MEMBER_REAPED  = 3
} ark_bus_member_event_t;

typedef void (*ark_bus_member_cb_t)(uint32_t id, uint8_t type,
                                    uint32_t capabilities,
                                    int event, void *user);

#if ARK_BUS_REMOTE_SUB_ENABLED
/* One peer's remembered subscription set: the topic ids it asked us to send. */
typedef struct {
    uint32_t peer;
    uint8_t  n;
    uint16_t topics[ARK_BUS_REMOTE_SUB_TOPICS];
} ark_bus_rsub_entry_t;
#endif

typedef struct ark_bus {
    transport_adapter_t *transport;

    uint32_t self_id;
    uint8_t  device_type;
    uint32_t capabilities;
    uint16_t next_seq;
    uint8_t  started;

    uint8_t          topic_count;
    ark_bus_topic_t  topics[ARK_BUS_MAX_TOPICS];

    uint8_t           device_count;
    ark_bus_device_t  devices[ARK_BUS_MAX_DEVICES];

#if ARK_BUS_QOS_ENABLED
    ark_bus_qos_session_t qos[ARK_BUS_QOS_MAX_SESSIONS];
#endif

#if ARK_BUS_RPC_ENABLED
    ark_bus_rpc_state_t rpc;
    /* Optional app hook for the DESCRIBE system method. */
    ark_bus_describe_fn describe_fn;
    void               *describe_user;
#endif

#if ARK_BUS_TASK_ENABLED
    ark_bus_task_pool_t tasks;
#endif

#if ARK_BUS_STREAM_ENABLED
    ark_bus_stream_state_t stream;
#endif

#if ARK_BUS_COLL_ENABLED
    ark_bus_coll_state_t coll;
#endif

#if ARK_BUS_INFER_ENABLED
    ark_bus_infer_state_t infer;
#endif

#if ARK_BUS_TOOL_ENABLED
    ark_bus_tool_state_t tools;
#endif

#if ARK_BUS_FLOW_ENABLED
    ark_bus_flow_state_t flow;
#endif

#if ARK_BUS_ROUTE_ENABLED
    ark_bus_route_state_t route;
#endif

#if ARK_BUS_RAFT_ENABLED
    ark_bus_raft_state_t raft;
#endif

#if ARK_BUS_FWU_ENABLED
    ark_bus_fwu_state_t fwu;
#endif

#if ARK_BUS_REMOTE_SUB_ENABLED
    ark_bus_rsub_entry_t rsub[ARK_BUS_REMOTE_SUB_PEERS];
#endif

#if ARK_BUS_CRYPTO_ENABLED
    /* Pre-shared key for frame authentication (ark_bus_crypto.h).  All-zero
     * when no key is installed, in which case crypto_key_set is 0 and the bus
     * neither seals nor demands tags. */
    uint8_t crypto_key[ARK_BUS_CRYPTO_KEY_MAX];
    uint8_t crypto_key_len;
    uint8_t crypto_key_set;
#if ARK_BUS_ENCRYPT_ENABLED
    /* Subkeys derived from crypto_key by HMAC (key separation), so the same
     * secret never feeds the MAC (crypto_mac_key) and the cipher
     * (crypto_enc_key) directly.  crypto_enc_counter is the monotonic nonce
     * counter: it increments once per sealed frame and never repeats for the
     * life of the key. */
    uint8_t  crypto_mac_key[32];
    uint8_t  crypto_enc_key[32];
    uint64_t crypto_enc_counter;
#endif
#if ARK_BUS_REPLAY_ENABLED
    /* Per-peer sliding window of accepted sequence numbers (ark_bus_replay.c). */
    ark_bus_replay_entry_t replay[ARK_BUS_REPLAY_PEERS];
#endif
#if ARK_BUS_HANDSHAKE_ENABLED
    /* Per-peer session keys from the authenticated X25519 handshake
     * (ark_bus_session.h).  When a ready entry exists for a peer, frames to
     * and from it use that entry's keys instead of the group key.  The table
     * is a plain array: no allocation, no pointer that can dangle. */
    ark_bus_session_t sessions[ARK_BUS_SESSION_PEERS];
    /* Monotonic epoch source: each handshake we start takes the next value,
     * so a responder can tell a stale request from a fresh one. */
    uint16_t session_epoch;
#endif
#endif

    ark_bus_stats_t stats;
    uint32_t last_heartbeat_ms;
    uint32_t last_cleanup_ms;

    /* Dynamic membership (M4).  See ark_bus_set_member_cb() and
     * ark_bus_set_member_reap_ms().  0 reap window == keep every peer. */
    ark_bus_member_cb_t member_cb;
    void               *member_user;
    uint32_t            member_reap_ms;

    /* Re-entrancy depth for the HAL critical section, so a user callback that
     * publishes from inside another publish does not deadlock a plain mutex. */
    uint8_t lock_depth;
} ark_bus_t;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Bind a bus to a transport and clear every table.  `tport` is borrowed. */
void ark_bus_init(ark_bus_t *bus, transport_adapter_t *tport);

/*
 * Mark the bus live and announce itself.  Until start() is called the bus
 * answers nothing and never emits heartbeats; publish/subscribe still work for
 * a purely local test.
 *
 * Returns the number of bytes announced, or a negative ARK_BUS_E_* code.
 */
int ark_bus_start(ark_bus_t *bus, uint32_t self_id,
                  uint8_t device_type, uint32_t capabilities);

/* Stop announcing and heartbeating; pending frames stop being retransmitted. */
void ark_bus_stop(ark_bus_t *bus);

/*
 * Leave gracefully (M4): broadcast a LEAVE so every peer can drop us at once
 * instead of waiting out ARK_BUS_DEVICE_TIMEOUT_MS, then stop the bus.  Same
 * effect as ark_bus_stop() plus that one announcement; returns the bytes sent,
 * or a negative ARK_BUS_E_* code.
 */
int ark_bus_leave(ark_bus_t *bus);

/*
 * One step of the event loop: drain the transport, dispatch frames, run QoS
 * retransmission and heartbeat/liveness timers.  Call it as often as you can;
 * it never blocks.
 */
void ark_bus_poll(ark_bus_t *bus);

/* ------------------------------------------------------------------ */
/* Publish / subscribe                                                 */
/* ------------------------------------------------------------------ */

/* Broadcast on a topic.  The topic is registered on first use.
 * Returns the bytes handed to the transport, or a negative error. */
int ark_bus_publish(ark_bus_t *bus, const char *topic,
                    const void *payload, uint16_t len);

/* Address one peer.  Carries the QoS1 flag, so it is retried until ACKed.
 * Returns the bytes handed to the transport, or a negative error. */
int ark_bus_publish_to(ark_bus_t *bus, uint32_t target_id, const char *topic,
                       const void *payload, uint16_t len);

/* Broadcast with explicit frame flags (e.g. ARK_BUS_FLAG_COMPRESSED).
 * Returns the bytes handed to the transport, or a negative error. */
int ark_bus_publish_raw(ark_bus_t *bus, const char *topic,
                        const void *payload, uint16_t len, uint8_t flags);

/*
 * Directed with explicit frame flags.  Same as ark_bus_publish_to() except the
 * caller owns the flags: with ARK_BUS_FLAG_QOS1 it is retried like publish_to(),
 * with flags 0 it is fire-and-forget (one attempt, no ACK).  The best-effort
 * form is the traffic M4 flow control can hold off and queue; a QoS1 frame has
 * its own retransmission and is left alone.
 */
int ark_bus_publish_directed(ark_bus_t *bus, uint32_t target_id,
                             const char *topic, const void *payload,
                             uint16_t len, uint8_t flags);

/* Register a local callback for a topic.  Re-subscribing replaces the handler.
 * Returns the topic id (a positive number), or a negative error. */
int ark_bus_subscribe(ark_bus_t *bus, const char *topic,
                      ark_bus_msg_cb_t callback, void *user);

/* Drop the callback; the topic id stays registered.  Returns ARK_BUS_OK or a
 * negative error. */
int ark_bus_unsubscribe(ark_bus_t *bus, const char *topic);

/* ------------------------------------------------------------------ */
/* Topic registry                                                      */
/* ------------------------------------------------------------------ */

/* Return the id of an existing topic, or ARK_BUS_E_NOTFOUND. */
int ark_bus_topic_lookup(const ark_bus_t *bus, const char *name);

/* Return the id of `name`, registering it if new; ARK_BUS_E_FULL when full. */
int ark_bus_topic_register(ark_bus_t *bus, const char *name);

/* ------------------------------------------------------------------ */
/* Frame intake                                                        */
/* ------------------------------------------------------------------ */

/* Decode and dispatch one raw frame.  Returns ARK_BUS_OK or an error. */
int ark_bus_feed(ark_bus_t *bus, const uint8_t *data, uint16_t len);

/* Dispatch an already-decoded frame. */
int ark_bus_dispatch(ark_bus_t *bus, const ark_bus_frame_t *frame);

/* Announce ourselves (send a DISCOVER) so peers can reply.
 * Returns the bytes sent, or a negative error. */
int ark_bus_announce(ark_bus_t *bus);

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Dynamic membership (M4)                                             */
/* ------------------------------------------------------------------ */

/* Install (or clear, with NULL) the membership callback.  See
 * ark_bus_member_event_t / ark_bus_member_cb_t above. */
void ark_bus_set_member_cb(ark_bus_t *bus, ark_bus_member_cb_t cb, void *user);

/*
 * Reap window: remove an OFFLINE peer this many milliseconds after it was last
 * seen.  0 (the default, from ARK_BUS_MEMBER_REAP_MS) keeps every peer forever,
 * exactly as before M4.  Reaping frees a device-table slot for a live node; a
 * removal swaps the last entry into the hole, so the table stays contiguous
 * and ark_bus_device_count()/at() keep their meaning.
 */
void ark_bus_set_member_reap_ms(ark_bus_t *bus, uint32_t ms);

uint8_t ark_bus_device_count(const ark_bus_t *bus);
const ark_bus_device_t *ark_bus_device_at(const ark_bus_t *bus, uint8_t index);
const ark_bus_device_t *ark_bus_device_find(const ark_bus_t *bus, uint32_t id);
const ark_bus_stats_t  *ark_bus_get_stats(const ark_bus_t *bus);

#if ARK_BUS_REMOTE_SUB_ENABLED
/* ------------------------------------------------------------------ */
/* Remote subscription registry                                        */
/* ------------------------------------------------------------------ */
/*
 * With ARK_BUS_REMOTE_SUB_ENABLED, a node acts on SUBSCRIBE / UNSUBSCRIBE
 * frames (previously accepted and ignored) that are addressed to it or
 * broadcast, and remembers which peer wants which topic.  The consumer is a
 * hub or bridge: it can forward a topic only to the links that asked for it
 * instead of fanning every frame to everyone.  The core itself still
 * broadcasts -- this is an inventory, not a router.
 *
 * A peer is identified by its node id; the set is cleared when the peer leaves
 * or is reaped, and can be cleared by hand with ark_bus_rsub_clear().
 */

/* Number of topics remembered for `peer`, or ARK_BUS_E_NOTFOUND if untracked. */
int ark_bus_rsub_count(const ark_bus_t *bus, uint32_t peer);

/* 1 if `peer` asked for topic id `topic_id`, else 0. */
int ark_bus_rsub_has(const ark_bus_t *bus, uint32_t peer, uint16_t topic_id);

/* Copy the i-th remembered topic id for `peer`.  ARK_BUS_OK, or
 * ARK_BUS_E_NOTFOUND / ARK_BUS_E_INVAL. */
int ark_bus_rsub_at(const ark_bus_t *bus, uint32_t peer, uint8_t i,
                    uint16_t *topic_id_out);

/* Copy up to `max` tracked peer ids; returns how many were written. */
int ark_bus_rsub_peers(const ark_bus_t *bus, uint32_t *out, uint8_t max);

/* Drop every remembered topic for `peer`. */
void ark_bus_rsub_clear(struct ark_bus *bus, uint32_t peer);

/* Send one SUBSCRIBE (subscribe != 0) or UNSUBSCRIBE frame naming `topic_id`
 * to `target` (use ARK_BUS_ID_BROADCAST to tell every peer).  The receiving
 * node records it in its registry; a node that never enables the feature sees
 * the frame accepted and ignored, exactly as before.  Returns ARK_BUS_OK on
 * transmit, or ARK_BUS_E_INVAL for a null bus or topic id 0. */
int ark_bus_rsub_announce(ark_bus_t *bus, uint32_t target, uint16_t topic_id,
                          int subscribe);
#endif /* ARK_BUS_REMOTE_SUB_ENABLED */

#endif /* ARK_BUS_H */
