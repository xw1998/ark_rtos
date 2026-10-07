/*
 * ark_bus_priv.h - internal helpers shared by the core translation units.
 * Not installed; application code includes ark_bus.h instead.
 */
#ifndef ARK_BUS_PRIV_H
#define ARK_BUS_PRIV_H

#include <stdint.h>
#include <stddef.h>
#include "ark_bus.h"

/* ------------------------------------------------------------------ */
/* Local string/memory helpers.  Defined in ark_bus_util.c so the core */
/* does not link libc by intent.  (A compiler is still free to turn a   */
/* byte loop into a call to its own memcpy; that is the toolchain's     */
/* runtime, not a dependency we introduced.)                            */
/* ------------------------------------------------------------------ */
void  *ark_memcpy(void *dst, const void *src, size_t n);
void  *ark_memset(void *dst, int c, size_t n);
int    ark_memcmp(const void *a, const void *b, size_t n);
size_t ark_strlen(const char *s);
int    ark_strncmp(const char *a, const char *b, size_t n);
/* Copies at most n-1 bytes and always NUL-terminates. */
void   ark_strncpy(char *dst, const char *src, size_t n);

/* Big-endian helpers, the only place wire order is encoded/decoded. */
uint16_t ark_rd16(const uint8_t *p);
uint32_t ark_rd32(const uint8_t *p);
void     ark_wr16(uint8_t *p, uint16_t v);
void     ark_wr32(uint8_t *p, uint32_t v);

/* ------------------------------------------------------------------ */
/* Shared internals                                                    */
/* ------------------------------------------------------------------ */

/* Stable 15-bit topic id derived from the name, so two devices that know the
 * same topic string agree on the id without any negotiation.  0 is reserved. */
uint16_t ark_bus_topic_hash(const char *name);

/* Encode one frame into `out` with a caller-supplied sequence number (see
 * ark_bus_next_seq()); returns the byte count, or a negative error. */
int ark_bus_build_frame(ark_bus_t *bus, uint32_t dst, uint8_t msg_type,
                        uint16_t topic_id, const void *payload, uint16_t len,
                        uint8_t flags, uint16_t seq, uint8_t *out,
                        uint16_t cap);

/* Build and send a frame, honouring the QoS1 flag, and count the transmit. */
int ark_bus_emit(ark_bus_t *bus, uint32_t dst, uint8_t msg_type,
                 uint16_t topic_id, const void *payload, uint16_t len,
                 uint8_t flags);

/* Record one transmit of `n` bytes (n <= 0 counts nothing). */
void ark_bus_stats_tx(ark_bus_t *bus, int n);

/* discovery.c */
int  ark_bus_discovery_tick(ark_bus_t *bus);
void ark_bus_discovery_handle(ark_bus_t *bus, const ark_bus_frame_t *f);

/* rpc.c */
#if ARK_BUS_RPC_ENABLED
void ark_bus_rpc_handle(ark_bus_t *bus, const ark_bus_frame_t *f);
void ark_bus_rpc_tick(ark_bus_t *bus);
int  ark_bus_rpc_has_method(const ark_bus_t *bus, uint16_t method);
#endif

/* caps.c: registers the built-in PING/DESCRIBE methods on start(). */
#if ARK_BUS_RPC_ENABLED
int  ark_bus_register_system_methods(ark_bus_t *bus);
#endif

/* task.c */
#if ARK_BUS_TASK_ENABLED
void ark_bus_task_init(ark_bus_t *bus);
void ark_bus_task_tick(ark_bus_t *bus);
void ark_bus_task_worker_offline(ark_bus_t *bus, uint32_t worker_id);
/* Keep per-task tried_mask bits aligned when discovery swaps device slots. */
void ark_bus_task_note_device_swap(ark_bus_t *bus, uint8_t from, uint8_t to);
#endif

/* stream.c (the data plane) */
#if ARK_BUS_STREAM_ENABLED
void ark_bus_stream_init(ark_bus_t *bus);
void ark_bus_stream_handle(ark_bus_t *bus, const ark_bus_frame_t *f);
void ark_bus_stream_tick(ark_bus_t *bus);
#endif

/* coll.c (collective communication) */
#if ARK_BUS_COLL_ENABLED
void ark_bus_coll_init(ark_bus_t *bus);
void ark_bus_coll_tick(ark_bus_t *bus);
#endif

/* infer.c (the tensor-parallel plan executor) */
#if ARK_BUS_INFER_ENABLED
void ark_bus_infer_init(ark_bus_t *bus);
#endif

/* tool.c (the model-facing tool registry and its policy) */
#if ARK_BUS_TOOL_ENABLED
void ark_bus_tool_init(ark_bus_t *bus);
#endif

/* route.c (mesh routing, see ark_bus_route.h).  Adds multi-hop forwarding on
 * top of the hub-and-spoke base, so a node can reach a peer it cannot hear
 * directly. */
#if ARK_BUS_ROUTE_ENABLED
void ark_bus_route_init(ark_bus_t *bus);
void ark_bus_route_tick(ark_bus_t *bus);
void ark_bus_route_handle(ark_bus_t *bus, const ark_bus_frame_t *f);
#endif

/* raft.c (consensus, see ark_bus_raft.h).  Elects one leader and replicates a
 * log so the cluster agrees on an order of entries; off by default. */
#if ARK_BUS_RAFT_ENABLED
void ark_bus_raft_init(ark_bus_t *bus);
void ark_bus_raft_tick(ark_bus_t *bus);
void ark_bus_raft_handle(ark_bus_t *bus, const ark_bus_frame_t *f);
#endif

/* fwu.c (fleet management, see ark_bus_fwu.h).  Signs a firmware image and
 * streams it over the data plane; the target verifies before applying. */
#if ARK_BUS_FWU_ENABLED
void ark_bus_fwu_init(ark_bus_t *bus);
#endif

/* subreg.c (remote subscription registry).  Forgets a peer's topics when it
 * leaves or is reaped, so a hub does not keep forwarding to a dead link. */
#if ARK_BUS_REMOTE_SUB_ENABLED
void ark_bus_subreg_handle(ark_bus_t *bus, const ark_bus_frame_t *f);
void ark_bus_subreg_forget(ark_bus_t *bus, uint32_t peer);
#endif

/* crypto.c: seal an outgoing payload (optionally encrypt it, then append a
 * tag) / verify and strip one from a decoded frame.  Only called by core.c
 * emit/feed under ARK_BUS_CRYPTO_ENABLED, and only when a key is installed.
 *
 * seal() takes the sequence number so it can be covered by the MAC when the
 * replay window is enabled (ark_bus_config.h); with the window off the number
 * is not hashed and the tag is byte-for-byte what it always was.
 *
 * open() decrypts into `scratch` (cap bytes) when ARK_BUS_ENCRYPT_ENABLED and
 * repoints f->payload at it; without encryption `scratch` is unused and
 * f->payload still points into the caller's receive buffer. */
#if ARK_BUS_CRYPTO_ENABLED
int ark_bus_crypto_seal(ark_bus_t *bus, uint8_t msg_type, uint8_t flags,
                        uint16_t topic_id, uint32_t src, uint32_t dst,
                        uint16_t seq, const uint8_t *payload, uint16_t len,
                        uint8_t *out, uint16_t cap, uint16_t *out_len);
int ark_bus_crypto_open(const ark_bus_t *bus, ark_bus_frame_t *f,
                        uint8_t *scratch, uint16_t cap);
#endif

/* Next sequence number, reserved and wrapped (0 stays unused).  emit() calls
 * this before sealing so the number can be authenticated. */
uint16_t ark_bus_next_seq(ark_bus_t *bus);

/* crypto.c: HMAC-SHA256 over up to three message segments, exposed to the
 * handshake's HKDF so it can hash (T(i-1) | info | counter) without a scratch
 * copy.  Only linked when the handshake is enabled.  `out` may alias `a`. */
#if ARK_BUS_CRYPTO_ENABLED && ARK_BUS_HANDSHAKE_ENABLED
void ark_hmac_sha256_3(const uint8_t *key, uint16_t klen,
                       const uint8_t *a, uint32_t alen,
                       const uint8_t *b, uint32_t blen,
                       const uint8_t *c3, uint32_t clen,
                       uint8_t out[32]);
#endif

/* session.c (M5 security): the authenticated X25519 handshake and the
 * per-peer session keys it deploys.  All of these are no-ops/zero when the
 * feature is off -- the whole translation unit compiles out. */
#if ARK_BUS_HANDSHAKE_ENABLED
/* A handshake frame is always sealed and opened with the pre-shared key, never
 * a session key: there may be no session yet, and a rotation must be readable
 * regardless of which keys the two ends currently hold. */
#define ARK_BUS_IS_HANDSHAKE_TYPE(t) \
    ((t) == ARK_BUS_MSG_HANDSHAKE_REQ || (t) == ARK_BUS_MSG_HANDSHAKE_REP)

/* Dispatch an incoming HANDSHAKE_REQ / HANDSHAKE_REP (called by core.c). */
void ark_bus_session_handle(ark_bus_t *bus, const ark_bus_frame_t *f);
/* Retransmit pending requests / time them out (called by core.c poll()). */
void ark_bus_session_tick(ark_bus_t *bus);
/* Current and previous subkeys for `peer`; 1 when they exist (crypto.c uses
 * them to pick a peer key over the group key). */
int  ark_bus_session_keys(const ark_bus_t *bus, uint32_t peer,
                          const uint8_t **mac_key, const uint8_t **enc_key);
int  ark_bus_session_prev_keys(const ark_bus_t *bus, uint32_t peer,
                               const uint8_t **mac_key, const uint8_t **enc_key);
/* 1 when `peer` was revoked and is owed a denial. */
int  ark_bus_session_denied(const ark_bus_t *bus, uint32_t peer);
#endif

/* replay.c: check an already-authenticated frame against the per-peer sliding
 * window.  Returns ARK_BUS_OK on first sight, ARK_BUS_E_STATE on a duplicate
 * or too-old sequence number.  Only called by core.c feed under
 * ARK_BUS_REPLAY_ENABLED. */
#if ARK_BUS_REPLAY_ENABLED
int ark_bus_replay_check(ark_bus_t *bus, const ark_bus_frame_t *f);
#endif

/* qos.c */
int  ark_bus_qos_submit(ark_bus_t *bus, uint32_t dst, uint16_t seq,
                        const uint8_t *frame, uint16_t len);
void ark_bus_qos_tick(ark_bus_t *bus);
void ark_bus_qos_handle_ack(ark_bus_t *bus, const ark_bus_frame_t *f);
void ark_bus_qos_reset(ark_bus_t *bus);

#endif /* ARK_BUS_PRIV_H */
