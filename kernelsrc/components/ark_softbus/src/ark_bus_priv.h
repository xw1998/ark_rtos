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

/* Encode one frame into `out`; on success returns the byte count and writes the
 * sequence number it consumed into `*seq_out` (may be NULL). */
int ark_bus_build_frame(ark_bus_t *bus, uint32_t dst, uint8_t msg_type,
                        uint16_t topic_id, const void *payload, uint16_t len,
                        uint8_t flags, uint8_t *out, uint16_t cap,
                        uint16_t *seq_out);

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

/* qos.c */
int  ark_bus_qos_submit(ark_bus_t *bus, uint32_t dst, uint16_t seq,
                        const uint8_t *frame, uint16_t len);
void ark_bus_qos_tick(ark_bus_t *bus);
void ark_bus_qos_handle_ack(ark_bus_t *bus, const ark_bus_frame_t *f);
void ark_bus_qos_reset(ark_bus_t *bus);

#endif /* ARK_BUS_PRIV_H */
