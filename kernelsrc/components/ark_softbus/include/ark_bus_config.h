/*
 * ark_bus_config.h - compile-time configuration for the ark_softbus core.
 *
 * Every knob can be overridden from the command line (e.g. -DARK_BUS_MAX_TOPICS=16)
 * or from a board-level header that is force-included before this one.  Nothing
 * here allocates memory: all limits are static array sizes, sized for an MCU
 * first and simply left generous on a PC.
 */
#ifndef ARK_BUS_CONFIG_H
#define ARK_BUS_CONFIG_H

/* Maximum number of distinct topics the local registry can hold. */
#ifndef ARK_BUS_MAX_TOPICS
#define ARK_BUS_MAX_TOPICS              32
#endif

/* Maximum number of remote devices tracked in the device table. */
#ifndef ARK_BUS_MAX_DEVICES
#define ARK_BUS_MAX_DEVICES             16
#endif

/* Maximum payload bytes carried inside one frame. */
#ifndef ARK_BUS_MSG_MAX_SIZE
#define ARK_BUS_MSG_MAX_SIZE            256
#endif

/* Maximum length of a topic name, including the terminating NUL. */
#ifndef ARK_BUS_TOPIC_NAME_MAX
#define ARK_BUS_TOPIC_NAME_MAX          32
#endif

/* Set to 0 to compile out QoS1 retransmission entirely. */
#ifndef ARK_BUS_QOS_ENABLED
#define ARK_BUS_QOS_ENABLED             1
#endif

/* Maximum number of in-flight QoS1 sessions. */
#ifndef ARK_BUS_QOS_MAX_SESSIONS
#define ARK_BUS_QOS_MAX_SESSIONS        4
#endif

/* Retransmit a QoS1 frame after this many milliseconds without an ACK. */
#ifndef ARK_BUS_QOS_RETRY_INTERVAL_MS
#define ARK_BUS_QOS_RETRY_INTERVAL_MS   200
#endif

/* Give up on a QoS1 frame after this many retransmissions. */
#ifndef ARK_BUS_QOS_RETRY_MAX
#define ARK_BUS_QOS_RETRY_MAX           3
#endif

/*
 * RPC layer (see ark_bus_rpc.h).  Set to 0 to compile it out entirely; the
 * RPC symbols then vanish from the map, which is how the footprint check
 * proves "off = free".
 */
#ifndef ARK_BUS_RPC_ENABLED
#define ARK_BUS_RPC_ENABLED             1
#endif

/* Maximum number of outstanding RPC requests awaiting a reply. */
#ifndef ARK_BUS_RPC_MAX_SLOTS
#define ARK_BUS_RPC_MAX_SLOTS           8
#endif

/* Maximum number of locally serviced RPC methods. */
#ifndef ARK_BUS_RPC_MAX_METHODS
#define ARK_BUS_RPC_MAX_METHODS         8
#endif

/* Give up on an RPC request after this many milliseconds without a reply. */
#ifndef ARK_BUS_RPC_TIMEOUT_MS
#define ARK_BUS_RPC_TIMEOUT_MS          2000
#endif

/*
 * Task engine (see ark_bus_task.h).  It rides on RPC, so it needs
 * ARK_BUS_RPC_ENABLED.  Set to 0 to compile it out.
 */
#ifndef ARK_BUS_TASK_ENABLED
#define ARK_BUS_TASK_ENABLED           1
#endif

#if ARK_BUS_TASK_ENABLED && !ARK_BUS_RPC_ENABLED
#error "ARK_BUS_TASK_ENABLED needs ARK_BUS_RPC_ENABLED"
#endif

/* Maximum number of tasks tracked at the coordinator. */
#ifndef ARK_BUS_TASK_MAX
#define ARK_BUS_TASK_MAX               8
#endif

/* Largest task parameter/argument block kept for (re)dispatch.  The task's
 * arguments must be stored so a failed attempt can be sent to another worker;
 * a task that needs more than this belongs on the M2 data plane. */
#ifndef ARK_BUS_TASK_PARAM_MAX
#define ARK_BUS_TASK_PARAM_MAX         32
#endif

/* Number of finished results kept for retrieval (round-robin eviction). */
#ifndef ARK_BUS_TASK_RESULT_SLOTS
#define ARK_BUS_TASK_RESULT_SLOTS      4
#endif

/* Largest task result kept on the control plane.  Anything bigger needs the
 * M2 data plane (result handles). */
#ifndef ARK_BUS_TASK_RESULT_MAX
#define ARK_BUS_TASK_RESULT_MAX        32
#endif

/* Worker-side idempotence cache: a redelivered TASK_RUN with a task id seen
 * before returns the cached result instead of running the handler again. */
#ifndef ARK_BUS_TASK_CACHE
#define ARK_BUS_TASK_CACHE             4
#endif

/* Re-dispatch a task up to this many times before declaring it LOST. */
#ifndef ARK_BUS_TASK_MAX_RETRY
#define ARK_BUS_TASK_MAX_RETRY         2
#endif

/*
 * Data plane: chunked bulk transfer (see ark_bus_stream.h).  A control-plane
 * payload must fit in one frame; an object that does not is moved in chunks by
 * this layer instead, which is what lifts the "one frame" ceiling without
 * touching the wire format.  Set to 0 to compile it out entirely.
 *
 * Budget, stated before the code (host gcc -Os, MCU-first sizing):
 *   .text  ~1.5 KB when enabled, 0 B and no symbols when disabled.
 *   .bss   TX_SLOTS * 32 B  +  RX_SLOTS * (RX_MAX + 32 B)
 *          = 2*32 + 2*(512+32) ~= 1.1 KB with the defaults below.
 * No heap, no libc, one frame per chunk like everything else.
 */
#ifndef ARK_BUS_STREAM_ENABLED
#define ARK_BUS_STREAM_ENABLED          1
#endif

/* Concurrent outbound transfers this node can drive.  Each holds a pointer to
 * caller-owned bytes, not a copy. */
#ifndef ARK_BUS_STREAM_TX_SLOTS
#define ARK_BUS_STREAM_TX_SLOTS         2
#endif

/* Inbound transfer slots and the largest object each can reassemble.  A
 * completed slot is kept (so a retransmitted last chunk still gets a correct
 * cumulative ACK) and is recycled only when another transfer needs the room. */
#ifndef ARK_BUS_STREAM_RX_SLOTS
#define ARK_BUS_STREAM_RX_SLOTS         2
#endif
#ifndef ARK_BUS_STREAM_RX_MAX
#define ARK_BUS_STREAM_RX_MAX           512
#endif

/* Largest object ark_bus_stream_send() accepts.  Bounded by the caller's
 * buffer, not by the receive side, so it may differ from RX_MAX. */
#ifndef ARK_BUS_STREAM_MAX_BYTES
#define ARK_BUS_STREAM_MAX_BYTES        512
#endif

/* Resend a chunk that has not been acknowledged after this long, giving up
 * after ARK_BUS_STREAM_RETRY_MAX resends. */
#ifndef ARK_BUS_STREAM_RETRY_INTERVAL_MS
#define ARK_BUS_STREAM_RETRY_INTERVAL_MS 300
#endif
#ifndef ARK_BUS_STREAM_RETRY_MAX
#define ARK_BUS_STREAM_RETRY_MAX        4
#endif

/*
 * Collective communication (see ark_bus_coll.h).  It rides on the data plane,
 * so it needs ARK_BUS_STREAM_ENABLED and follows it by default: on whenever
 * the data plane is, off whenever it is not.  Force 0 (or 1) to override.
 *
 * Budget, measured, not guessed (host gcc -Os, x86; the target numbers differ
 * but the shape does not):
 *   .text  5052 B when enabled (ark_bus_coll.o, size -t), 0 B and no symbols
 *          when disabled -- see tools/footprint.sh for the gate.
 *   .bss   628 B per bus = a 92 B op record (which already contains the 36 B
 *          group) + the 512 B staging buffer + a few pointers/counters.  The
 *          staging buffer is the bulk of it: it holds one assembled collective
 *          object, because the stream layer borrows the caller's bytes rather
 *          than copying them.
 * No heap, no libc.
 */
#ifndef ARK_BUS_COLL_ENABLED
#define ARK_BUS_COLL_ENABLED           ARK_BUS_STREAM_ENABLED
#endif

/* A collective cannot exist without the layer it rides on, so the default above
 * simply follows the data plane: disabling the stream layer disables the
 * collectives with it, and a firmware that never wanted either never has to
 * remember to switch off two macros.  Asking for collectives on a build with
 * no data plane is still a hard error rather than a silent no-op. */
#if ARK_BUS_COLL_ENABLED && !ARK_BUS_STREAM_ENABLED
#error "ARK_BUS_COLL_ENABLED needs ARK_BUS_STREAM_ENABLED"
#endif

/* Largest group this node can join.  A rank is one byte on the wire, so keep
 * it within the 32-bit receive bitmask the ring uses. */
#ifndef ARK_BUS_COLL_MAX_RANKS
#define ARK_BUS_COLL_MAX_RANKS         8
#endif

#if ARK_BUS_COLL_MAX_RANKS > 32
#error "ARK_BUS_COLL_MAX_RANKS must fit ARK_BUS_COLL's 32-bit receive mask"
#endif

/* Staging buffer for one assembled collective object.  A broadcast carries a
 * whole buffer, so it must hold ARK_BUS_COLL_HDR + the largest object sent. */
#ifndef ARK_BUS_COLL_STAGE_MAX
#define ARK_BUS_COLL_STAGE_MAX         ARK_BUS_STREAM_MAX_BYTES
#endif

/* Fail an operation that has made no progress for this long (a dead peer). */
#ifndef ARK_BUS_COLL_TIMEOUT_MS
#define ARK_BUS_COLL_TIMEOUT_MS        5000
#endif

/*
 * Inference adapter (see ark_bus_infer.h).  It drives the collective layer, so
 * it needs ARK_BUS_COLL_ENABLED and follows it by default: on whenever
 * collectives are, off whenever they are not.  Force 0 (or 1) to override.
 *
 * Budget, measured, not guessed (host gcc -Os, x86; the target numbers differ
 * but the shape does not):
 *   .text  4604 B when enabled (ark_bus_infer.o 2952 B + ark_bus_infer_ref.o
 *          1652 B, size -t), 0 B and no symbols when disabled -- see
 *          tools/footprint.sh for the gate.
 *   .bss   ~40 B per bus: the executor state is one plan/backend pointer, a
 *          handful of flags and two counters.  Every buffer a step touches is
 *          the caller's; the layer owns no weights, no activations and no
 *          heap.
 * No heap, no libc, and the reference backend needs no libm (see
 * ark_bus_infer_ref.c).
 */
#ifndef ARK_BUS_INFER_ENABLED
#define ARK_BUS_INFER_ENABLED          ARK_BUS_COLL_ENABLED
#endif

/* Asking for the inference layer on a build with no collectives is a hard
 * error rather than a silent no-op, same as the layer below it. */
#if ARK_BUS_INFER_ENABLED && !ARK_BUS_COLL_ENABLED
#error "ARK_BUS_INFER_ENABLED needs ARK_BUS_COLL_ENABLED"
#endif

/* Maximum steps in one plan.  A transformer block is a handful of steps; the
 * ceiling is here so the plan can be a caller-owned static array. */
#ifndef ARK_BUS_INFER_MAX_STEPS
#define ARK_BUS_INFER_MAX_STEPS        32
#endif

/* Emit a heartbeat every this many milliseconds once the bus is started. */
#ifndef ARK_BUS_HEARTBEAT_INTERVAL_MS
#define ARK_BUS_HEARTBEAT_INTERVAL_MS   3000
#endif

/* Declare a peer offline after this many milliseconds without a heartbeat. */
#ifndef ARK_BUS_DEVICE_TIMEOUT_MS
#define ARK_BUS_DEVICE_TIMEOUT_MS       9000
#endif

/*
 * Largest complete frame the wire can carry: header + payload + CRC.  The
 * streaming reader allocates this much per instance, so keep it in mind when
 * sizing buffers on an MCU.
 */
#define ARK_BUS_FRAME_MAX_SIZE \
    (ARK_BUS_HDR_LEN_ + ARK_BUS_MSG_MAX_SIZE + ARK_BUS_CRC_LEN_)

/*
 * Internal constants duplicated here as macros so that the config header owns
 * every size that shows up in a static array.  ark_bus_protocol.h checks that
 * its enumerated values agree with these.
 */
#define ARK_BUS_HDR_LEN_   19
#define ARK_BUS_CRC_LEN_    2

#endif /* ARK_BUS_CONFIG_H */
