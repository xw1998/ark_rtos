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

    ark_bus_stats_t stats;
    uint32_t last_heartbeat_ms;
    uint32_t last_cleanup_ms;

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

uint8_t ark_bus_device_count(const ark_bus_t *bus);
const ark_bus_device_t *ark_bus_device_at(const ark_bus_t *bus, uint8_t index);
const ark_bus_device_t *ark_bus_device_find(const ark_bus_t *bus, uint32_t id);
const ark_bus_stats_t  *ark_bus_get_stats(const ark_bus_t *bus);

#endif /* ARK_BUS_H */
