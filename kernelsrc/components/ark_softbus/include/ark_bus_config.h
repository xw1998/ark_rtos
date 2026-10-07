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

/* Maximum steps in one plan.  The ceiling is here so the plan can be a
 * caller-owned static array; it is not a library allocation (the plan is the
 * caller's), so raising it costs nothing in the core.  One decoder block is 12
 * steps, so the default builds a spine of `(128 - 7) / 12` = 10 stacked layers
 * (see ark_bus_infer_plan_model).  A board that only ever runs a block or two
 * can lower it: the plan record is `ARK_BUS_INFER_MAX_STEPS * sizeof(step)`
 * bytes, which is the one place a big value shows up.  (The M3.3 remote server
 * is unaffected: it keeps only as many steps as one shipped sub-plan, see
 * ark_bus_infer_run_steps.) */
#ifndef ARK_BUS_INFER_MAX_STEPS
#define ARK_BUS_INFER_MAX_STEPS        128
#endif

/*
 * Remote inference (M3.3): ship a plan's step list to a peer and let it run
 * the kernels there, the ggml_backend_rpc shape.  It rides on the RPC layer
 * (a request/reply per sub-plan), so it needs ARK_BUS_RPC_ENABLED as well as
 * the inference layer, and follows both on by default.
 */
#ifndef ARK_BUS_INFER_REMOTE_ENABLED
#define ARK_BUS_INFER_REMOTE_ENABLED   \
    (ARK_BUS_INFER_ENABLED && ARK_BUS_RPC_ENABLED)
#endif

#if ARK_BUS_INFER_REMOTE_ENABLED && !ARK_BUS_INFER_ENABLED
#error "ARK_BUS_INFER_REMOTE_ENABLED needs ARK_BUS_INFER_ENABLED"
#endif
#if ARK_BUS_INFER_REMOTE_ENABLED && !ARK_BUS_RPC_ENABLED
#error "ARK_BUS_INFER_REMOTE_ENABLED needs ARK_BUS_RPC_ENABLED"
#endif

/* Tensors one node can expose to (or receive back from) the remote service.
 * A handle is a 16-bit id, so the table stays small on purpose: it holds the
 * weights and activations of the split, not a whole model. */
#ifndef ARK_BUS_INFER_REMOTE_MAX_TENSORS
#define ARK_BUS_INFER_REMOTE_MAX_TENSORS 16
#endif

/* Steps in one shipped sub-plan.  Bounded by the RPC frame, not by taste:
 * a step is 19 wire bytes, so eight steps plus the header fit a 256-byte MTU. */
#ifndef ARK_BUS_INFER_REMOTE_MAX_STEPS
#define ARK_BUS_INFER_REMOTE_MAX_STEPS  8
#endif

/* Buffers a single request may ask the peer to return. */
#ifndef ARK_BUS_INFER_REMOTE_MAX_RET
#define ARK_BUS_INFER_REMOTE_MAX_RET    4
#endif

/* RPC method a server answers with ark_bus_infer_server_init() by default. */
#ifndef ARK_BUS_M_INFER_REMOTE
#define ARK_BUS_M_INFER_REMOTE         0x8001u
#endif

/*
 * Chunked result fetch for the remote inference backend (see
 * ark_bus_infer_remote.h).  Off by default.  A RUN reply lives in one RPC
 * frame, so a return buffer bigger than that is refused with E_NOMEM -- unless
 * this is on, in which case the server stages the reply and the client pulls
 * it in chunks with ARK_BUS_M_INFER_FETCH.  When off nothing here is linked
 * and the single-frame behaviour is exactly as before.
 *
 * Budget when enabled (host gcc -Os): .text ~400 B; .bss = STAGE_MAX bytes in
 * the caller-owned server, plus a few scalars.  No heap, no libc.
 */
#ifndef ARK_BUS_INFER_REMOTE_STAGE_ENABLED
#define ARK_BUS_INFER_REMOTE_STAGE_ENABLED   0
#endif

/* Bytes a server can hold for one staged reply.  0 compiles staging out. */
#ifndef ARK_BUS_INFER_REMOTE_STAGE_MAX
#define ARK_BUS_INFER_REMOTE_STAGE_MAX       2048
#endif

#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED && ARK_BUS_INFER_REMOTE_STAGE_MAX == 0
#error "ARK_BUS_INFER_REMOTE_STAGE_ENABLED needs ARK_BUS_INFER_REMOTE_STAGE_MAX > 0"
#endif
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED && !ARK_BUS_INFER_REMOTE_ENABLED
#error "ARK_BUS_INFER_REMOTE_STAGE_ENABLED needs ARK_BUS_INFER_REMOTE_ENABLED"
#endif

/*
 * M3 completion: three optional capabilities on top of the inference adapter.
 * Each is off by default, so a build that turns none of them on has exactly
 * the code it had before (same `.text`, same symbols, same behaviour).  Turn
 * one on and its symbols appear; the footprint gate proves both directions.
 *
 * They are mutually independent - enable any subset.
 */

/*
 * KV cache (decode-step attention).  Without it every attention call
 * recomputes the full causal window; with it a rank keeps the keys and values
 * it has already computed, so generating token n+1 costs one query against the
 * cached history instead of a full re-projection.  The cache itself is
 * caller-owned memory (a plain struct of two float arrays); this knob only
 * compiles in the kernel and the plan step that use it.
 */
#ifndef ARK_BUS_INFER_KV_ENABLED
#define ARK_BUS_INFER_KV_ENABLED       0
#endif
#if ARK_BUS_INFER_KV_ENABLED && !ARK_BUS_INFER_ENABLED
#error "ARK_BUS_INFER_KV_ENABLED needs ARK_BUS_INFER_ENABLED"
#endif

/*
 * Weight-only int8 quantisation.  The reference backend is float32; with this
 * on, a node can store a weight matrix as int8 codes plus one scale and run a
 * quantised GEMM step, which is the "an integer backend plugs in behind the
 * same vtable" claim made real.  Activations stay float32.
 */
#ifndef ARK_BUS_INFER_QUANT_ENABLED
#define ARK_BUS_INFER_QUANT_ENABLED    0
#endif
#if ARK_BUS_INFER_QUANT_ENABLED && !ARK_BUS_INFER_ENABLED
#error "ARK_BUS_INFER_QUANT_ENABLED needs ARK_BUS_INFER_ENABLED"
#endif

/*
 * Stochastic sampling.  A greedy global argmax is what the model plan ends in;
 * temperature / top-k / nucleus sampling is a self-contained function that runs
 * as a CALL step, exactly the seam the header describes.  Needs no collective
 * and no backend kernel - it is plain scalar code with no libm and no heap.
 */
#ifndef ARK_BUS_INFER_SAMPLING_ENABLED
#define ARK_BUS_INFER_SAMPLING_ENABLED 0
#endif
#if ARK_BUS_INFER_SAMPLING_ENABLED && !ARK_BUS_INFER_ENABLED
#error "ARK_BUS_INFER_SAMPLING_ENABLED needs ARK_BUS_INFER_ENABLED"
#endif

/*
 * Candidate-set ceiling for the sampler.  The sampler keeps a bounded array of
 * the `top_k` highest logits (no heap, no full-vocabulary sort), so this is the
 * largest `top_k` it can honour and also the widest nucleus it can carry.  A
 * mode that asks for more is clamped to this and the caller is told the
 * effective k.  64 candidates is 512 bytes of caller-owned state; raise it for
 * a wider nucleus at the cost of that one array.
 */
#ifndef ARK_BUS_SAMPLER_MAX_K
#define ARK_BUS_SAMPLER_MAX_K          64
#endif
#if ARK_BUS_SAMPLER_MAX_K < 1
#error "ARK_BUS_SAMPLER_MAX_K must be at least 1"
#endif

/*
 * Tool interface (M5): expose a node's actions as model-callable tools with a
 * machine-readable description, behind a deny-by-default policy.  It rides on
 * the RPC layer, so it follows ARK_BUS_RPC_ENABLED by default.
 *
 * Registration is explicit: nothing is answerable until an application calls
 * ark_bus_tool_register() and ark_bus_tool_enable().  With no policy installed
 * every call is refused (E_DENIED), so an un-configured node cannot be driven
 * by a model at all.
 */
#ifndef ARK_BUS_TOOL_ENABLED
#define ARK_BUS_TOOL_ENABLED           ARK_BUS_RPC_ENABLED
#endif
#if ARK_BUS_TOOL_ENABLED && !ARK_BUS_RPC_ENABLED
#error "ARK_BUS_TOOL_ENABLED needs ARK_BUS_RPC_ENABLED"
#endif

/* Tools one node may register.  The policy allow-mask is 32 bits, so this is
 * also the ceiling on tool ids (0..31). */
#ifndef ARK_BUS_TOOL_MAX_TOOLS
#define ARK_BUS_TOOL_MAX_TOOLS         8
#endif
#if ARK_BUS_TOOL_MAX_TOOLS > 32
#error "ARK_BUS_TOOL_MAX_TOOLS cannot exceed 32 (the allow-mask is 32 bits)"
#endif

/* Longest tool name / summary / params signature (ASCII, not NUL-counted on
 * the wire; the registry points at caller-owned strings). */
#ifndef ARK_BUS_TOOL_NAME_MAX
#define ARK_BUS_TOOL_NAME_MAX          24
#endif

/* Encoded arguments of one call, and encoded result of one call, in bytes.
 * One 256-byte frame carries the RPC header, the tool id and this payload, so
 * they are deliberately small: a tool is a control operation, not a data pipe
 * (bulk data belongs on the data plane). */
#ifndef ARK_BUS_TOOL_ARG_MAX
#define ARK_BUS_TOOL_ARG_MAX           64
#endif
#ifndef ARK_BUS_TOOL_RESULT_MAX
#define ARK_BUS_TOOL_RESULT_MAX        64
#endif

/* Per-caller policy overrides (M5.3).  Each entry binds one caller node id to
 * its own policy and its own rate-limit window, so the bus-wide policy is only
 * the fallback.  A node that never installs an override pays only this table
 * (a few tens of bytes); the default is 4 slots. */
#ifndef ARK_BUS_TOOL_MAX_CALLERS
#define ARK_BUS_TOOL_MAX_CALLERS       4
#endif

/* Nodes allowed to approve a held call (M5.3).  An empty list (0, the default)
 * keeps the old behaviour: any node may approve.  A non-empty list restricts
 * approvals to those ids. */
#ifndef ARK_BUS_TOOL_MAX_APPROVERS
#define ARK_BUS_TOOL_MAX_APPROVERS     4
#endif

/* Staging buffer for a result too large for the inline reply (M5.3).  A tool
 * whose encoded result exceeds the reply frame is staged here and pulled in
 * chunks over TOOL_RESULT.  0 compiles the staged-result path out entirely:
 * such a result is E_FULL, exactly as before. */
#ifndef ARK_BUS_TOOL_STAGE_MAX
#define ARK_BUS_TOOL_STAGE_MAX         512
#endif

/* Calls held awaiting approval (tools flagged ARK_BUS_TOOL_APPROVAL).  Each
 * slot copies the arguments, so its size is 4 * (bookkeeping + ARG_MAX). */
#ifndef ARK_BUS_TOOL_PENDING_MAX
#define ARK_BUS_TOOL_PENDING_MAX       4
#endif
#ifndef ARK_BUS_TOOL_APPROVAL_TTL_MS
#define ARK_BUS_TOOL_APPROVAL_TTL_MS   30000
#endif

/* Audit ring: the most recent calls, newest overwriting the oldest. */
#ifndef ARK_BUS_TOOL_AUDIT_LEN
#define ARK_BUS_TOOL_AUDIT_LEN         8
#endif

/* Default rate-limit window for the policy. */
#ifndef ARK_BUS_TOOL_WINDOW_MS
#define ARK_BUS_TOOL_WINDOW_MS         1000
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
 * Dynamic membership (M4): remove a peer from the device table this long after
 * it went OFFLINE, freeing the slot for a node that is actually present.  The
 * default 0 disables reaping, so a node that never asks for it keeps the
 * append-only table exactly as before -- this is an initial value for
 * ark_bus_set_member_reap_ms(), which can turn it on at runtime.
 *
 * A hub that sees churn (nodes come and go) needs this: ARK_BUS_MAX_DEVICES
 * slots would otherwise slowly fill with dead entries and hide every new node.
 * It must be comfortably larger than ARK_BUS_DEVICE_TIMEOUT_MS, otherwise a
 * brief packet loss removes a peer instead of just marking it offline.
 */
#ifndef ARK_BUS_MEMBER_REAP_MS
#define ARK_BUS_MEMBER_REAP_MS          0
#endif

/*
 * Task-state checkpoint / restore (M4, see ark_bus_checkpoint.h).  Off by
 * default: it is a coordinator-side recovery aid, not something every node
 * wants linked in.  It rides on the task engine and adds no state to ark_bus_t,
 * so enabling it costs only the .text of the two entry points.
 */
#ifndef ARK_BUS_CHECKPOINT_ENABLED
#define ARK_BUS_CHECKPOINT_ENABLED      0
#endif
#if ARK_BUS_CHECKPOINT_ENABLED && !ARK_BUS_TASK_ENABLED
#error "ARK_BUS_CHECKPOINT_ENABLED needs ARK_BUS_TASK_ENABLED"
#endif

/*
 * Flow control / backpressure (M4, see ark_bus_flow.h).  Off by default and
 * inert until ark_bus_flow_set_depth() switches it on, so a firmware that
 * never asks for it keeps the pre-M4 hot path exactly (the frames this header
 * would hold are the only cost, and they are not linked in at all when the
 * whole feature is compiled out).
 *
 * Budget when enabled (host gcc -Os): .text ~900 B, .bss = QUEUE_MAX *
 * (a frame + 12 B) + MAX_PAUSED * 12 B.  With the defaults below that is
 * 4 * ~289 + 4 * 12 ~= 1.2 KB per bus; a board with a 64-byte MTU and a
 * 2-deep queue pays a quarter of that.  No heap, no libc.
 */
#ifndef ARK_BUS_FLOW_ENABLED
#define ARK_BUS_FLOW_ENABLED            0
#endif

/* Complete frames the egress queue can hold.  0 compiles the queue out. */
#ifndef ARK_BUS_FLOW_QUEUE_MAX
#define ARK_BUS_FLOW_QUEUE_MAX          4
#endif

/* Peers we can hold off sending to at once (FLOW_BUSY senders). */
#ifndef ARK_BUS_FLOW_MAX_PAUSED
#define ARK_BUS_FLOW_MAX_PAUSED         4
#endif

/* Drop a queued frame that has waited this long; the link is not draining. */
#ifndef ARK_BUS_FLOW_TTL_MS
#define ARK_BUS_FLOW_TTL_MS             5000
#endif

/*
 * Remote subscription registry (see ark_bus.h).  Off by default.  When off,
 * SUBSCRIBE / UNSUBSCRIBE frames are accepted and ignored exactly as before
 * and the registry costs neither .text nor .bss.  When on, a node remembers
 * which peer asked for which topic so a hub or bridge can forward selectively.
 * The tables are per-bus and live in ark_bus_t; size them to the peer and
 * topic fan-out the node actually serves.  No heap, no libc.
 *
 * Budget when enabled (host gcc -Os): .text ~500 B; .bss = PEERS * (8 B +
 * 2 B * TOPICS).  With the defaults below that is 4 * (8 + 16) = ~96 B.
 */
#ifndef ARK_BUS_REMOTE_SUB_ENABLED
#define ARK_BUS_REMOTE_SUB_ENABLED      0
#endif

/* Distinct peers whose subscription sets we track at once.  0 compiles out. */
#ifndef ARK_BUS_REMOTE_SUB_PEERS
#define ARK_BUS_REMOTE_SUB_PEERS        4
#endif

/* Topic ids remembered per tracked peer.  0 compiles the registry out. */
#ifndef ARK_BUS_REMOTE_SUB_TOPICS
#define ARK_BUS_REMOTE_SUB_TOPICS       8
#endif

#if ARK_BUS_REMOTE_SUB_ENABLED && ARK_BUS_REMOTE_SUB_PEERS == 0
#error "ARK_BUS_REMOTE_SUB_ENABLED needs ARK_BUS_REMOTE_SUB_PEERS > 0"
#endif
#if ARK_BUS_REMOTE_SUB_ENABLED && ARK_BUS_REMOTE_SUB_TOPICS == 0
#error "ARK_BUS_REMOTE_SUB_ENABLED needs ARK_BUS_REMOTE_SUB_TOPICS > 0"
#endif

/*
 * Payload compression (see ark_bus_compress.h).  Off by default.  When off,
 * ARK_BUS_FLAG_COMPRESSED is carried and ignored exactly as before and the
 * codec costs neither .text nor .bss.  When on, a payload whose COMPRESSED
 * flag is set is compressed on send and inflated on receive with a small LZSS
 * codec that needs no heap and no libc.  Both ends must agree, so enable it
 * only on links whose peers also do.
 *
 * Budget when enabled (host gcc -Os): .text ~600 B, no static .bss (the
 * scratch buffers live on the caller's stack and are only touched while a
 * compressed frame is being processed).
 */
#ifndef ARK_BUS_COMPRESS_ENABLED
#define ARK_BUS_COMPRESS_ENABLED        0
#endif

/*
 * Frame authentication (see ark_bus_crypto.h).  Off by default.  When off, no
 * tag is added, no frame carries ARK_BUS_FLAG_AUTHENTICATED, and the codec
 * costs neither .text nor .bss.  When on, every frame this node sends carries
 * a truncated HMAC-SHA256 over its header fields and payload, keyed by a
 * pre-shared key installed with ark_bus_crypto_set_key(); a frame whose tag
 * does not verify (or a node with no key) is dropped before dispatch and
 * counted in stats.rx_auth_fail.  Both ends must share the key, so enable it
 * only on links whose peers also do.
 *
 * The tag authenticates the header too, so src_id can no longer be spoofed on
 * a shared medium -- the strongest boundary the tool layer's trust-based node
 * id was missing.  Confidentiality (AES-CCM / ChaCha20) is deliberately not
 * here: this is an integrity+authenticity seam, not a cipher.
 *
 * Budget when enabled (measured, host gcc -Os): ~2.7 KB of .text + .rodata
 * (SHA-256 + HMAC + the seam; thumb-2 targets are usually smaller), nowhere
 * near 1 KB, so size it before enabling it on a tight part.  No static .bss:
 * the hashing state lives on the caller's stack.  The wire frame grows by
 * ARK_BUS_CRYPTO_TAG_LEN bytes, and the bus struct by KEY_MAX + 2.
 */
#ifndef ARK_BUS_CRYPTO_ENABLED
#define ARK_BUS_CRYPTO_ENABLED          0
#endif

/* Tag bytes kept from the 32-byte HMAC-SHA256 digest.  8 gives a 64-bit tag;
 * raise it (up to 32) for a stronger boundary at a few bytes more per frame. */
#ifndef ARK_BUS_CRYPTO_TAG_LEN
#define ARK_BUS_CRYPTO_TAG_LEN          8
#endif

/* Longest pre-shared key accepted by ark_bus_crypto_set_key(). */
#ifndef ARK_BUS_CRYPTO_KEY_MAX
#define ARK_BUS_CRYPTO_KEY_MAX          32
#endif

#if ARK_BUS_CRYPTO_ENABLED && (ARK_BUS_CRYPTO_TAG_LEN < 4 || ARK_BUS_CRYPTO_TAG_LEN > 32)
#error "ARK_BUS_CRYPTO_TAG_LEN must be 4..32 (HMAC-SHA256 is 32 bytes)"
#endif
#if ARK_BUS_CRYPTO_ENABLED && ARK_BUS_CRYPTO_KEY_MAX < 1
#error "ARK_BUS_CRYPTO_ENABLED needs ARK_BUS_CRYPTO_KEY_MAX >= 1"
#endif
#if ARK_BUS_CRYPTO_ENABLED && \
    (ARK_BUS_MSG_MAX_SIZE < ARK_BUS_CRYPTO_TAG_LEN + 1)
#error "ARK_BUS_CRYPTO_TAG_LEN leaves no room for a payload"
#endif

/*
 * Payload confidentiality (see ark_bus_chacha20.h).  Off by default and
 * requires ARK_BUS_CRYPTO_ENABLED.  Authentication alone makes a payload
 * un-tamperable but still readable; this adds ChaCha20 encryption under the
 * same tag (encrypt-then-MAC), so a passive listener on the medium sees
 * ciphertext.  A keyed bus encrypts every frame it sends once a key is
 * installed; an unkeyed bus behaves exactly as before.
 *
 * Both ends must agree, and because the wire format changes they must both be
 * compiled with this on -- there is no in-band negotiation.  The two subkeys
 * are derived from the pre-shared key by HMAC (key separation), so the same
 * pre-shared key never feeds both the cipher and the MAC directly.
 *
 * Budget: ChaCha20 is ~300 B of .text on a host -Os build (no tables, no
 * .rodata).  The wire frame grows by ARK_BUS_CRYPTO_NONCE_LEN more bytes (the
 * per-frame nonce counter) on top of the tag.
 */
#ifndef ARK_BUS_ENCRYPT_ENABLED
#define ARK_BUS_ENCRYPT_ENABLED         0
#endif

#if ARK_BUS_ENCRYPT_ENABLED && !ARK_BUS_CRYPTO_ENABLED
#error "ARK_BUS_ENCRYPT_ENABLED requires ARK_BUS_CRYPTO_ENABLED"
#endif

/* Bytes of nonce counter carried ahead of the ciphertext.  The full ChaCha20
 * nonce is src_id (4, already in the header and authenticated) || this 8-byte
 * per-sender counter; only the counter has to travel. */
#define ARK_BUS_CRYPTO_NONCE_LEN        8

/*
 * Replay window (see ark_bus_replay.c).  Off by default and requires
 * ARK_BUS_CRYPTO_ENABLED: without a tag a replayed frame is indistinguishable
 * from a fresh one, so the window only makes sense once frames are
 * authenticated.  When on, the sequence number joins the MAC (so an attacker
 * cannot renumber a captured frame to slip past the window) and every peer's
 * non-QoS1 frames are checked against a sliding window of the last
 * ARK_BUS_REPLAY_WINDOW sequence numbers.  A duplicate is dropped before
 * dispatch and counted in stats.rx_replay.
 *
 * QoS1 frames are exempt: a retransmission reuses the sequence number by
 * design, so a window would report every honest retry as a replay.  A
 * deployment that needs both must make its QoS1 handlers idempotent (which is
 * the QoS1 contract anyway).
 */
#ifndef ARK_BUS_REPLAY_ENABLED
#define ARK_BUS_REPLAY_ENABLED          0
#endif

#if ARK_BUS_REPLAY_ENABLED && !ARK_BUS_CRYPTO_ENABLED
#error "ARK_BUS_REPLAY_ENABLED requires ARK_BUS_CRYPTO_ENABLED"
#endif

/* Per-peer window state kept live (one entry per distinct src_id).  A frame
 * from a peer beyond this many concurrent senders is dropped (fail closed),
 * so size it for the senders this node actually listens to. */
#ifndef ARK_BUS_REPLAY_PEERS
#define ARK_BUS_REPLAY_PEERS            8
#endif

/* Bits of history per peer.  64 covers a 16-bit sequence space amply for a
 * lossy link and costs 8 bytes of bitmap per peer. */
#ifndef ARK_BUS_REPLAY_WINDOW
#define ARK_BUS_REPLAY_WINDOW           64
#endif

#if ARK_BUS_REPLAY_ENABLED && ARK_BUS_REPLAY_WINDOW > 64
#error "ARK_BUS_REPLAY_WINDOW is capped at 64 (one uint64_t bitmap)"
#endif
#if ARK_BUS_REPLAY_ENABLED && ARK_BUS_REPLAY_PEERS < 1
#error "ARK_BUS_REPLAY_ENABLED needs ARK_BUS_REPLAY_PEERS >= 1"
#endif

/*
 * Session handshake (M5 security, see ark_bus_session.h).  Off by default and
 * requires ARK_BUS_CRYPTO_ENABLED.
 *
 * Without it the pre-shared key is the only secret and it is used for every
 * frame and every peer: a shared group key.  With it, two nodes run a
 * pre-shared-key-authenticated ephemeral X25519 exchange and derive a per-peer
 * session key (forward secrecy: the ephemeral secrets are discarded, so a key
 * leaked later cannot decrypt earlier traffic).  Each pair gets its own key,
 * so one compromised session no longer exposes the others, and the rotation /
 * revocation calls can retire a single peer's key.  The initial pre-shared key
 * still has to be provisioned out of band -- that is the trust anchor, and
 * there is deliberately no PKI here.
 *
 * Budget when enabled (measured, host gcc -Os; see tools/footprint.sh):
 *   .text  X25519 + HKDF + the handshake/session state machine, a few KB.
 *   .bss   ARK_BUS_SESSION_PEERS * sizeof(ark_bus_session_t) per bus (the
 *          session's two subkeys, the previous pair for key rollover, and the
 *          in-flight ephemeral material).  Size PEERS to the peers this node
 *          actually negotiates with.
 * No heap, no libc.  A node that never calls the handshake API is unaffected
 * even with the feature compiled in (no session means no session key).
 */
#ifndef ARK_BUS_HANDSHAKE_ENABLED
#define ARK_BUS_HANDSHAKE_ENABLED       0
#endif

#if ARK_BUS_HANDSHAKE_ENABLED && !ARK_BUS_CRYPTO_ENABLED
#error "ARK_BUS_HANDSHAKE_ENABLED requires ARK_BUS_CRYPTO_ENABLED"
#endif

/* Concurrent session records this node keeps live (one per peer).  A peer
 * beyond this many cannot establish a session until one is freed or revoked,
 * which is fail-closed: the bus then falls back to the pre-shared group key
 * only for peers with no session at all. */
#ifndef ARK_BUS_SESSION_PEERS
#define ARK_BUS_SESSION_PEERS           4
#endif

#if ARK_BUS_HANDSHAKE_ENABLED && ARK_BUS_SESSION_PEERS < 1
#error "ARK_BUS_HANDSHAKE_ENABLED needs ARK_BUS_SESSION_PEERS >= 1"
#endif

/* Retransmit an unanswered handshake request after this long, giving up (and
 * reporting ARK_BUS_E_TIMEOUT to the caller of ark_bus_handshake_start, via
 * the completion callback) after ARK_BUS_HANDSHAKE_RETRY_MAX resends. */
#ifndef ARK_BUS_HANDSHAKE_RETRY_MS
#define ARK_BUS_HANDSHAKE_RETRY_MS      500
#endif
#ifndef ARK_BUS_HANDSHAKE_RETRY_MAX
#define ARK_BUS_HANDSHAKE_RETRY_MAX     4
#endif

/*
 * Allow the handshake to run with no random source (the HAL supplies none and
 * this is set to 1).  Off by default, and off deliberately: minting ephemeral
 * keys from a predictable seed (the tick, say) would let an attacker who knows
 * it recompute every session key, so the handshake refuses (E_NOSYS) instead
 * of handing back a false sense of security.  Set this only where the seed is
 * genuinely unpredictable, or where the link is already physically protected
 * and the handshake is wanted purely for key separation.
 */
#ifndef ARK_BUS_CRYPTO_ALLOW_WEAK_RNG
#define ARK_BUS_CRYPTO_ALLOW_WEAK_RNG   0
#endif

/*
 * Text tokenizer (see ark_bus_tokenizer.h).  Off by default: a node that only
 * moves tensors never needs it, and the vocabulary belongs to whichever model
 * is being served, so its size is the application's budget and not a limit
 * here.  Enabling it adds one allocation-free greedy-longest-match encoder and
 * its inverse -- no libc, no heap, no float.
 */
#ifndef ARK_BUS_TOKENIZER_ENABLED
#define ARK_BUS_TOKENIZER_ENABLED       0
#endif

/* Longest vocabulary entry init() accepts, NUL included.  This is a sanity
 * bound on the caller's table (a runaway entry is caught at init instead of
 * half-way through a token stream), not a buffer: nothing here allocates it. */
#ifndef ARK_BUS_TOKENIZER_TOKEN_MAX
#define ARK_BUS_TOKENIZER_TOKEN_MAX     24
#endif

#if ARK_BUS_TOKENIZER_ENABLED && ARK_BUS_TOKENIZER_TOKEN_MAX < 2
#error "ARK_BUS_TOKENIZER_TOKEN_MAX must leave room for a 1-byte token plus NUL"
#endif

/*
 * Mesh routing (see ark_bus_route.h).  Off by default.  The base bus is
 * hub-and-spoke: a node reaches only the peers it can hear directly.  This
 * layer adds distance-vector routing on top, so a node can reach a destination
 * several hops away by having an intermediate node forward the frame -- the
 * "mesh beyond a single hub" the architecture notes call out as not-yet-done.
 *
 * It is a compile-time gate plus a runtime no-op: with the feature off nothing
 * here is linked and the hot path is byte-for-byte the old one.
 *
 * Budget, stated before the code (host gcc -Os): .text ~1 KB; .bss =
 * ARK_BUS_ROUTE_MAX_ENTRIES * sizeof(entry) (a few tens of bytes with the
 * defaults below).
 */
#ifndef ARK_BUS_ROUTE_ENABLED
#define ARK_BUS_ROUTE_ENABLED           0
#endif

/* Routes the table can hold (destinations, including direct neighbours). */
#ifndef ARK_BUS_ROUTE_MAX_ENTRIES
#define ARK_BUS_ROUTE_MAX_ENTRIES       8
#endif

/* Broadcast our route table this often (ms). */
#ifndef ARK_BUS_ROUTE_ADV_INTERVAL_MS
#define ARK_BUS_ROUTE_ADV_INTERVAL_MS   2000
#endif

/* Forget a learned (multi-hop) route after this long without a refresh (ms).
 * Direct neighbours are kept in step with the discovery device table instead,
 * so they are not aged out by this timer. */
#ifndef ARK_BUS_ROUTE_TIMEOUT_MS
#define ARK_BUS_ROUTE_TIMEOUT_MS        8000
#endif

/* Hop ceiling: a routed frame carries a TTL starting here and is dropped at 0,
 * so a transient routing loop cannot circulate a frame forever. */
#ifndef ARK_BUS_ROUTE_MAX_HOPS
#define ARK_BUS_ROUTE_MAX_HOPS          8
#endif

#if ARK_BUS_ROUTE_ENABLED && ARK_BUS_ROUTE_MAX_ENTRIES == 0
#error "ARK_BUS_ROUTE_ENABLED needs ARK_BUS_ROUTE_MAX_ENTRIES > 0"
#endif

/*
 * Raft consensus (see ark_bus_raft.h).  Off by default.  The core can already
 * select a peer, replicate an object and run a collective, but it has no way to
 * get a group of nodes to agree on one value.  This layer adds a small,
 * crash-fault-tolerant Raft: leader election, log replication and a replicated
 * state machine, so a cluster of gateways can agree on a value (the design
 * guide's part 9 / appendix B).
 *
 * It is deliberately small and bounded so it fits the same MCU-first budget:
 * no heap, no libc, fixed tables, one in-flight entry per follower.  With the
 * feature off nothing here is linked and the hot path is byte-for-byte the old
 * one.
 *
 * Budget, stated before the code (host gcc -Os): .text ~2-3 KB; .bss =
 * the log (ARK_BUS_RAFT_LOG_MAX * entry) plus per-member bookkeeping.
 */
#ifndef ARK_BUS_RAFT_ENABLED
#define ARK_BUS_RAFT_ENABLED            0
#endif

/* Largest cluster this build supports (including ourselves). */
#ifndef ARK_BUS_RAFT_MAX_MEMBERS
#define ARK_BUS_RAFT_MAX_MEMBERS        5
#endif

/* Log entries kept in RAM.  A full log refuses a proposal (E_FULL); this is a
 * demonstration-grade Raft, not a durable one -- entries live in the bus, not
 * in flash. */
#ifndef ARK_BUS_RAFT_LOG_MAX
#define ARK_BUS_RAFT_LOG_MAX            16
#endif

/* Largest payload one replicated entry carries. */
#ifndef ARK_BUS_RAFT_ENTRY_MAX
#define ARK_BUS_RAFT_ENTRY_MAX          64
#endif

/* A follower starts an election after this long without a leader heartbeat.
 * The timeout is spread by the node id so two nodes do not always campaign at
 * once: timeout = MIN + (self_id % 4) * SPREAD. */
#ifndef ARK_BUS_RAFT_ELECTION_MIN_MS
#define ARK_BUS_RAFT_ELECTION_MIN_MS    1500
#endif
#ifndef ARK_BUS_RAFT_ELECTION_SPREAD_MS
#define ARK_BUS_RAFT_ELECTION_SPREAD_MS 250
#endif

/* A leader re-asserts itself this often; must be well under the election
 * minimum or followers will keep timing out. */
#ifndef ARK_BUS_RAFT_HEARTBEAT_MS
#define ARK_BUS_RAFT_HEARTBEAT_MS       500
#endif

#if ARK_BUS_RAFT_ENABLED && ARK_BUS_RAFT_MAX_MEMBERS < 1
#error "ARK_BUS_RAFT_ENABLED needs ARK_BUS_RAFT_MAX_MEMBERS >= 1"
#endif
#if ARK_BUS_RAFT_ENABLED && ARK_BUS_RAFT_LOG_MAX < 1
#error "ARK_BUS_RAFT_ENABLED needs ARK_BUS_RAFT_LOG_MAX >= 1"
#endif
#if ARK_BUS_RAFT_ENABLED && ARK_BUS_RAFT_HEARTBEAT_MS >= ARK_BUS_RAFT_ELECTION_MIN_MS
#error "ARK_BUS_RAFT_HEARTBEAT_MS must be under ARK_BUS_RAFT_ELECTION_MIN_MS"
#endif

/* ------------------------------------------------------------------ */
/* Fleet management: signed firmware images (ARK_BUS_FWU_ENABLED).      */
/*                                                                     */
/* Off by default.  A controller signs an image (HMAC-SHA256 over the   */
/* image digest) and streams {manifest || image} to a target over the   */
/* data plane; the target checks the signature before it hands the      */
/* image to its apply hook.  It cannot move bytes without the data      */
/* plane, nor sign without the SHA-256/HMAC in the crypto layer, so it  */
/* is only meaningful with both enabled.                                */
/* ------------------------------------------------------------------ */
#ifndef ARK_BUS_FWU_ENABLED
#define ARK_BUS_FWU_ENABLED             0
#endif

/* Signing-key ceiling. */
#ifndef ARK_BUS_FWU_KEY_MAX
#define ARK_BUS_FWU_KEY_MAX             32
#endif

/* Firmware name ceiling (no NUL on the wire). */
#ifndef ARK_BUS_FWU_NAME_MAX
#define ARK_BUS_FWU_NAME_MAX            32
#endif

/* Largest {manifest || image} object FWU will build or accept.  It must fit
 * what the data plane can both send and reassemble, so it is derived from the
 * stream ceiling by default rather than picked independently. */
#ifndef ARK_BUS_FWU_OBJ_MAX
#define ARK_BUS_FWU_OBJ_MAX             ARK_BUS_STREAM_RX_MAX
#endif

#if ARK_BUS_FWU_ENABLED && !ARK_BUS_STREAM_ENABLED
#error "ARK_BUS_FWU_ENABLED needs ARK_BUS_STREAM_ENABLED (it moves the image)"
#endif
#if ARK_BUS_FWU_ENABLED && !ARK_BUS_CRYPTO_ENABLED
#error "ARK_BUS_FWU_ENABLED needs ARK_BUS_CRYPTO_ENABLED (it signs the image)"
#endif
#if ARK_BUS_FWU_ENABLED && ARK_BUS_FWU_OBJ_MAX > ARK_BUS_STREAM_MAX_BYTES
#error "ARK_BUS_FWU_OBJ_MAX must not exceed ARK_BUS_STREAM_MAX_BYTES"
#endif

/*
 * Largest payload the wire can carry.  Normally that is ARK_BUS_MSG_MAX_SIZE,
 * the largest message a caller may hand to ark_bus_send()/publish().  With
 * authentication compiled in the sender appends ARK_BUS_CRYPTO_TAG_LEN bytes
 * after the payload; with encryption it also prepends ARK_BUS_CRYPTO_NONCE_LEN
 * bytes of nonce counter.  The wire payload (and the frame that holds it)
 * grows by exactly that overhead; callers still see ARK_BUS_MSG_MAX_SIZE as
 * their limit.
 */
#if ARK_BUS_ENCRYPT_ENABLED
#define ARK_BUS_CRYPTO_WIRE_OVERHEAD \
    (ARK_BUS_CRYPTO_NONCE_LEN + ARK_BUS_CRYPTO_TAG_LEN)
#elif ARK_BUS_CRYPTO_ENABLED
#define ARK_BUS_CRYPTO_WIRE_OVERHEAD ARK_BUS_CRYPTO_TAG_LEN
#else
#define ARK_BUS_CRYPTO_WIRE_OVERHEAD 0
#endif

#define ARK_BUS_WIRE_PAYLOAD_MAX \
    (ARK_BUS_MSG_MAX_SIZE + ARK_BUS_CRYPTO_WIRE_OVERHEAD)

/*
 * Largest complete frame the wire can carry: header + payload + CRC.  The
 * streaming reader allocates this much per instance, so keep it in mind when
 * sizing buffers on an MCU.
 */
#define ARK_BUS_FRAME_MAX_SIZE \
    (ARK_BUS_HDR_LEN_ + ARK_BUS_WIRE_PAYLOAD_MAX + ARK_BUS_CRC_LEN_)

/*
 * Internal constants duplicated here as macros so that the config header owns
 * every size that shows up in a static array.  ark_bus_protocol.h checks that
 * its enumerated values agree with these.
 */
#define ARK_BUS_HDR_LEN_   19
#define ARK_BUS_CRC_LEN_    2

#endif /* ARK_BUS_CONFIG_H */
