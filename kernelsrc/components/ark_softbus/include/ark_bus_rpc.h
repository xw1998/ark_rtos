/*
 * ark_bus_rpc.h - request/reply on top of the bus.
 *
 * RPC is a thin layer over the existing directed-send path: a request is a
 * DIRECT frame carrying msg_type RPC_REQ, and the answer travels back as
 * RPC_REP.  Delivery reuses the QoS1 retransmit machinery, so a reply that is
 * re-sent arrives at most a few times; the RPC layer is idempotent about that
 * (a reply for an already-completed request is simply dropped).
 *
 * There is no hidden allocation and no blocking inside the core.  Requests
 * are tracked in a small static slot table; a caller either takes a completion
 * callback (asynchronous) or uses ark_bus_rpc_call(), which drives
 * ark_bus_poll() in a loop and never sleeps.
 *
 * Wire layout (payload, big-endian):
 *   RPC_REQ:  req_id u16 | method u16 | params ...
 *   RPC_REP:  req_id u16 | method u16 | status i8 | result ...
 *
 * The whole layer compiles out when ARK_BUS_RPC_ENABLED is 0.
 */
#ifndef ARK_BUS_RPC_H
#define ARK_BUS_RPC_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus.h"

#if ARK_BUS_RPC_ENABLED

/*
 * Register the handler that serves `method` locally.  Registering the same
 * method again replaces the previous handler.  System methods
 * (ARK_BUS_M_* below ARK_BUS_M_USER_BASE) may be overridden by an application
 * that wants to answer them itself.
 *
 * Returns ARK_BUS_OK, ARK_BUS_E_INVAL, or ARK_BUS_E_FULL.
 */
int ark_bus_rpc_register(ark_bus_t *bus, uint16_t method,
                         ark_bus_rpc_handler_t handler, void *user);

/*
 * Send an RPC request to `dst`.  `cb` (optionally NULL) is invoked once, from
 * inside ark_bus_poll(), either with the reply or with ARK_BUS_E_TIMEOUT.
 *
 * Returns the number of bytes handed to the transport, ARK_BUS_E_FULL when
 * every slot is busy, ARK_BUS_E_INVAL on a bad argument, or ARK_BUS_E_NOSYS
 * when the bus has no transport.
 */
int ark_bus_rpc_request(ark_bus_t *bus, uint32_t dst, uint16_t method,
                        const void *params, uint16_t len,
                        ark_bus_rpc_cb_t cb, void *user);

/*
 * Like ark_bus_rpc_request(), but also reports the request id, so a caller can
 * withdraw the request later with ark_bus_rpc_cancel().  The id is stable for
 * the life of the request, across any QoS1 retransmission.
 */
int ark_bus_rpc_request_id(ark_bus_t *bus, uint32_t dst, uint16_t method,
                           const void *params, uint16_t len,
                           ark_bus_rpc_cb_t cb, void *user,
                           uint16_t *req_id_out);

/*
 * Withdraw an outstanding request.  Its callback is NOT invoked; the slot is
 * simply freed.  Returns ARK_BUS_OK, or ARK_BUS_E_NOTFOUND when no request with
 * that id is still pending (for example, it already completed).
 */
int ark_bus_rpc_cancel(ark_bus_t *bus, uint16_t req_id);

/*
 * Blocking wrapper around ark_bus_rpc_request(): send, then keep calling
 * ark_bus_poll() until the reply arrives or ARK_BUS_RPC_TIMEOUT_MS elapses.
 * Up to `rcap` result bytes are copied into `reply` and the true length is
 * stored in *rlen (which may exceed rcap; the copy is truncated).
 *
 * Returns ARK_BUS_OK, ARK_BUS_E_TIMEOUT, or another ARK_BUS_E_* code.  It must
 * be called from the same context that normally drives ark_bus_poll(); nothing
 * else may pump the bus while it is waiting.
 */
int ark_bus_rpc_call(ark_bus_t *bus, uint32_t dst, uint16_t method,
                     const void *params, uint16_t len,
                     uint8_t *reply, uint16_t rcap, uint16_t *rlen);

/* Number of slots currently awaiting a reply (introspection / tests). */
uint8_t ark_bus_rpc_pending(const ark_bus_t *bus);

#endif /* ARK_BUS_RPC_ENABLED */

#endif /* ARK_BUS_RPC_H */
