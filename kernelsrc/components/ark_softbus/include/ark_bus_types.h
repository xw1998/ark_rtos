/*
 * ark_bus_types.h - shared types: status codes, callback signature, and the
 * entries kept in the local topic and device tables.
 */
#ifndef ARK_BUS_TYPES_H
#define ARK_BUS_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "ark_bus_config.h"

/* ------------------------------------------------------------------ */
/* Status codes.  Any negative return is an ARK_BUS_E_* error.  A     */
/* function that has a value to report on success returns it as a      */
/* positive number (a topic id or a byte count); the rest return       */
/* ARK_BUS_OK (0).  Nothing returns a bare -1.                         */
/* ------------------------------------------------------------------ */
#define ARK_BUS_OK          0
#define ARK_BUS_E_INVAL     (-1)   /* bad argument                     */
#define ARK_BUS_E_NOMEM     (-2)   /* no storage left for a new entry  */
#define ARK_BUS_E_FULL      (-3)   /* a table or queue is full         */
#define ARK_BUS_E_NOTFOUND  (-4)   /* unknown topic / device           */
#define ARK_BUS_E_NOSYS     (-5)   /* operation not supported          */
#define ARK_BUS_E_AGAIN     (-6)   /* try again later                  */
#define ARK_BUS_E_FORMAT    (-7)   /* malformed frame                  */
#define ARK_BUS_E_CRC       (-8)   /* checksum mismatch                */
#define ARK_BUS_E_STATE     (-9)   /* wrong state for this call        */
#define ARK_BUS_E_TIMEOUT   (-10)  /* an RPC or task reply never came  */
#define ARK_BUS_E_BUSY      (-11)  /* a worker refused the work        */

/*
 * Subscriber callback.  `topic` is the registered name, `payload` points into
 * the receive buffer and is only valid for the duration of the call, `user` is
 * whatever the subscriber passed to ark_bus_subscribe().
 */
typedef void (*ark_bus_msg_cb_t)(const char *topic,
                                 const uint8_t *payload,
                                 uint16_t len,
                                 void *user);

/* Local device liveness, as tracked for every peer in the device table. */
typedef enum {
    ARK_BUS_DEV_OFFLINE = 0,
    ARK_BUS_DEV_ONLINE  = 1
} ark_bus_dev_state_t;

/* One entry of the topic registry: a name mapped to a 16-bit id. */
typedef struct {
    char             name[ARK_BUS_TOPIC_NAME_MAX];
    uint16_t         id;
    uint8_t          used;
    ark_bus_msg_cb_t callback;
    void            *user;
} ark_bus_topic_t;

/* One peer seen on the bus. */
typedef struct {
    uint32_t id;
    uint8_t  type;
    uint32_t capabilities;
    uint8_t  state;              /* ark_bus_dev_state_t            */
    uint32_t last_seen_ms;       /* tick of the last heartbeat     */
    uint16_t missed_heartbeats;  /* consecutive intervals missed   */
} ark_bus_device_t;

/* Counters, all monotonic, useful for a status line or a scope view. */
typedef struct {
    uint32_t tx_frames;
    uint32_t tx_bytes;
    uint32_t rx_frames;
    uint32_t rx_bytes;
    uint32_t rx_crc_errors;
    uint32_t rx_bad_frames;
    uint32_t rx_resyncs;         /* bytes dropped while resyncing  */
    uint32_t qos_retransmits;
    uint32_t qos_timeouts;
    uint32_t dispatch_errors;    /* frames with no subscriber      */
} ark_bus_stats_t;

#if ARK_BUS_RPC_ENABLED
/* ark_bus_t is defined in ark_bus.h; the RPC handler only sees it in transit. */
struct ark_bus;

/*
 * Completion callback for one RPC request.  Called exactly once, either when
 * the reply arrives (status == 0, `result`/`len` describe the reply) or when
 * the request times out (status == ARK_BUS_E_TIMEOUT, result NULL, len 0).
 * `result` points into a transient buffer and is valid only for the call.
 */
typedef void (*ark_bus_rpc_cb_t)(int status, const uint8_t *result,
                                 uint16_t len, void *user);

/*
 * Server-side handler for one method.  Copy up to `rcap` result bytes into
 * `reply`, set *rlen, and return ARK_BUS_OK.  Return a negative ARK_BUS_E_*
 * code to answer with that status and an empty result.
 */
typedef int (*ark_bus_rpc_handler_t)(struct ark_bus *bus, uint32_t from,
                                     const uint8_t *params, uint16_t plen,
                                     uint8_t *reply, uint16_t rcap,
                                     uint16_t *rlen, void *user);

/*
 * Optional provider that appends application-specific bytes to a DESCRIBE
 * reply, after the standard header.  Returns the number of bytes written.
 */
typedef uint16_t (*ark_bus_describe_fn)(struct ark_bus *bus, uint8_t *out,
                                        uint16_t cap, void *user);

/*
 * One outstanding RPC request.  Bookkeeping only: request and reply payloads
 * are not stored here, so a slot stays comfortably under 32 bytes.
 */
typedef struct {
    uint8_t          used;
    uint8_t          state;      /* 1 = awaiting reply              */
    uint16_t         req_id;
    uint16_t         method;
    uint32_t         peer_id;
    uint32_t         sent_at_ms;
    ark_bus_rpc_cb_t cb;
    void            *user;
} ark_bus_rpc_slot_t;

/* A locally serviced method: id -> handler. */
typedef struct {
    uint16_t              method;
    ark_bus_rpc_handler_t handler;
    void                 *user;
} ark_bus_rpc_method_t;

/* All RPC state, embedded in ark_bus_t. */
typedef struct {
    ark_bus_rpc_slot_t   slots[ARK_BUS_RPC_MAX_SLOTS];
    ark_bus_rpc_method_t methods[ARK_BUS_RPC_MAX_METHODS];
    uint8_t              method_count;
    uint16_t             next_req_id;
} ark_bus_rpc_state_t;
#endif /* ARK_BUS_RPC_ENABLED */

#if ARK_BUS_TASK_ENABLED
/*
 * Completion notification for a task.  `state` is an ark_bus_task_state_t value (see ark_bus_task.h);
 * `status` is 0 for DONE or a negative ARK_BUS_E_* code otherwise.
 */
typedef void (*ark_bus_task_cb_t)(uint16_t task_id, int state, int status,
                                  void *user);

/*
 * Worker-side task body: run the work described by `params` and fill `reply`.
 * `task_id` lets a handler stay idempotent across a redelivery.
 */
typedef int (*ark_bus_task_fn)(uint16_t task_id, const uint8_t *params,
                               uint16_t plen, uint8_t *reply, uint16_t rcap,
                               uint16_t *rlen, void *user);

/*
 * Coordinator-side task record.  Metadata plus a result-slot index; the result
 * bytes live in the shared store below, so an entry keeps only its own bounded
 * parameter block (ARK_BUS_TASK_PARAM_MAX bytes) besides the metadata.
 */
typedef struct {
    uint8_t           used;
    uint8_t           state;        /* ark_bus_task_state_t          */
    uint8_t           retries;
    uint8_t           result_slot;  /* 0xFF = none                   */
    uint16_t          params_len;
    uint16_t          task_id;
    uint16_t          method;
    uint16_t          req_id;       /* outstanding RPC request, 0 none */
    uint32_t          worker_id;    /* 0 = not yet dispatched        */
    uint32_t          tried_mask;   /* bit i = device[i] already tried    */
    uint32_t          submit_ms;
    uint32_t          require_caps; /* worker selection mask         */
    struct ark_bus   *owner;        /* bus this task lives on        */
    ark_bus_task_cb_t cb;
    void             *user;
    uint8_t           params[ARK_BUS_TASK_PARAM_MAX];
} ark_bus_task_entry_t;

/* One retained result; evicted round-robin when all slots are taken. */
typedef struct {
    uint16_t owner_task;            /* 0 = free                      */
    uint16_t len;
    uint8_t  bytes[ARK_BUS_TASK_RESULT_MAX];
} ark_bus_task_result_t;

/* Worker-side idempotence cache entry. */
typedef struct {
    uint16_t task_id;               /* 0 = empty                     */
    int8_t   status;
    uint16_t len;
    uint8_t  bytes[ARK_BUS_TASK_RESULT_MAX];
} ark_bus_task_cache_t;

typedef struct {
    struct ark_bus       *bus;      /* back-pointer for RPC callbacks */
    ark_bus_task_entry_t  entries[ARK_BUS_TASK_MAX];
    ark_bus_task_result_t results[ARK_BUS_TASK_RESULT_SLOTS];
    ark_bus_task_cache_t  cache[ARK_BUS_TASK_CACHE];
    uint8_t               cache_next;
    uint8_t               result_next;
    uint16_t              next_task_id;
    ark_bus_task_fn       run_handler;
    void                 *run_user;
} ark_bus_task_pool_t;
#endif /* ARK_BUS_TASK_ENABLED */

#endif /* ARK_BUS_TYPES_H */
