/*
 * ark_bus_infer_remote.h - run a plan on a peer (the M3.3 remote backend).
 *
 * The plan is already "a straight list of steps to run on this rank" with a
 * flat, pointer-light record, which is exactly what a remote executor needs to
 * ship.  This layer forwards that step list to a peer over the bus RPC, the
 * same move ggml_backend_rpc makes for llama.cpp: the small node orchestrates,
 * the big node runs the kernels.
 *
 *   - TENANTS ARE PEER-RESIDENT.  A plan's operands are raw pointers into the
 *     caller's buffers, which mean nothing on another node.  So both sides
 *     agree on a small table of {handle, buffer, length}: the client maps its
 *     pointer to a handle before sending, the server maps the handle back to
 *     the buffer IT owns.  The handle, not the pointer, goes on the wire - so
 *     weights never move and the request stays inside one RPC frame.
 *   - ONE REQUEST, ONE SUB-PLAN.  The shipped sub-plan must be free of
 *     collectives (a peer cannot join this rank's group) and of CALL steps
 *     (a function pointer is not portable).  Both are refused up front with
 *     E_NOSYS rather than shipped and failing on the far side.
 *   - THE ANSWER IS EXPLICIT.  The client names the handles it wants back; the
 *     server returns them by value in the reply.  A handle that is unknown, or
 *     a length that disagrees with the client's table, is an error - never a
 *     silent truncation.  The reply shares one RPC frame with the request, so
 *     it is small on purpose.
 *   - A BIG ANSWER IS STAGED, THEN PULLED.  With
 *     ARK_BUS_INFER_REMOTE_STAGE_ENABLED the server keeps a reply that would
 *     not fit one frame in a staging buffer, answers with a token instead, and
 *     the client collects it in chunks with ARK_BUS_M_INFER_FETCH.  Off by
 *     default, in which case an oversized reply is still refused with E_NOMEM.
 *
 * Nothing here allocates and nothing blocks: the request is asynchronous (a
 * completion callback), and the server runs the sub-plan synchronously inside
 * the handler because a collective-free plan never parks.
 *
 * Wire layout (payload, big-endian).  Magic 0xB3C1, version 1.
 *   RUN:   magic u16 | ver u8 | nsteps u8 | nret u8 | rsv u8
 *          step[nsteps]: op u8 | a u16 | b u16 | c u16 | p0 i16 | p1 i16 |
 *                        p2 i16 | p3 i16 | f0 f32                    (19 B)
 *          ret[nret]: handle u16
 *   REPLY: status i8 | nret u8
 *          buf[nret]: handle u16 | nfloats u32 | data f32[nfloats]
 *          When the reply does not fit one frame and staging is on, status is
 *          ARK_BUS_INFER_REMOTE_STAGED and the body is instead
 *          total u32 | handle u16 (total = size of the REPLY body above).
 *   FETCH: handle u16 | off u32 | max u8        pull a staged reply chunk
 *   FETCH R: status i8 | total u32 | n u8 | bytes[n]
 * Handle 0xFFFF means "no operand".
 */
#ifndef ARK_BUS_INFER_REMOTE_H
#define ARK_BUS_INFER_REMOTE_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus_infer.h"
#include "ark_bus_rpc.h"

#if ARK_BUS_INFER_REMOTE_ENABLED

struct ark_bus;

/* A shipped sub-plan is bounded by one RPC frame on both sides. */
#if ARK_BUS_INFER_REMOTE_MAX_STEPS > ARK_BUS_INFER_MAX_STEPS
#error "ARK_BUS_INFER_REMOTE_MAX_STEPS cannot exceed ARK_BUS_INFER_MAX_STEPS"
#endif

/* Handle of a tensor in the remote namespace.  A handle is a 16-bit id. */
typedef uint16_t ark_bus_infer_handle_t;

#define ARK_BUS_INFER_HANDLE_NONE  0xFFFFu

/* Status a RUN reply carries, in place of ARK_BUS_OK, when the result was
 * staged because it did not fit one frame.  The client then pulls it in
 * chunks before the buffers are usable; req->stage_total / req->stage_handle
 * carry the size and token. */
#define ARK_BUS_INFER_REMOTE_STAGED     1

/* Fixed wire sizes, so a caller can size a buffer without guessing. */
#define ARK_BUS_INFER_REMOTE_HDR_SIZE   6u
#define ARK_BUS_INFER_REMOTE_STEP_SIZE  19u
#define ARK_BUS_INFER_FETCH_REQ_SIZE    7u   /* handle u16 | off u32 | max u8 */
#define ARK_BUS_INFER_FETCH_HDR_SIZE    6u   /* status i8 | total u32 | n u8  */

/* One tensor both sides agree on: the client sends the id, the server runs
 * against its own buffer.  `ptr` is a float*; `nfloats` its element count. */
typedef struct {
    ark_bus_infer_handle_t id;
    uint16_t               rsv;
    void                  *ptr;
    uint32_t               nfloats;
} ark_bus_infer_remote_tensor_t;

/* ------------------------------------------------------------------ */
/* Server: answer remote plan requests on this bus.                    */
/* ------------------------------------------------------------------ */

/*
 * A server is a paper-thin wrapper: the backend that runs the kernels plus the
 * tensor table that resolves handles to its own buffers.  It is caller-owned
 * and must outlive the registration.  One server per bus; registering again
 * with the same method replaces the handler.
 */
typedef struct {
    struct ark_bus                *bus;
    const ark_bus_infer_backend_t *be;
    uint16_t                       method;
    uint8_t                        attached;
    uint8_t                        ntensors;
    ark_bus_infer_remote_tensor_t  tensors[ARK_BUS_INFER_REMOTE_MAX_TENSORS];
    uint32_t                       jobs_served;
    uint32_t                       jobs_failed;
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
    /* One staged reply at a time (as with the tool layer): the body that would
     * not fit one frame, the token that names it, and its owner's node id - a
     * peer other than the caller may not read it. */
    uint8_t                        stage[ARK_BUS_INFER_REMOTE_STAGE_MAX];
    uint16_t                       stage_len;
    uint16_t                       stage_handle;
    uint16_t                       stage_token;   /* last token handed out    */
    uint32_t                       stage_owner;
    uint8_t                        stage_used;
#endif
} ark_bus_infer_server_t;

/*
 * Register `srv` to serve `method` on `bus`, running every shipped plan with
 * `be`.  Returns ARK_BUS_OK, ARK_BUS_E_INVAL, or what ark_bus_rpc_register()
 * returns.  `srv` and `be` must outlive the registration.
 */
int ark_bus_infer_server_init(ark_bus_infer_server_t *srv, struct ark_bus *bus,
                              uint16_t method,
                              const ark_bus_infer_backend_t *be);

/*
 * Declare that handle `id` names `ptr` (a float* of `nfloats` elements) on this
 * node.  Re-using an id replaces the previous binding.  Returns ARK_BUS_OK,
 * ARK_BUS_E_INVAL, or ARK_BUS_E_FULL when the table is exhausted.
 */
int ark_bus_infer_server_bind(ark_bus_infer_server_t *srv,
                              ark_bus_infer_handle_t id,
                              void *ptr, uint32_t nfloats);

/* Stop serving.  The RPC layer has no unregister, so the method is left
 * answering ARK_BUS_E_NOSYS.  The tensor table is left as-is. */
void ark_bus_infer_server_fini(ark_bus_infer_server_t *srv);

/* ------------------------------------------------------------------ */
/* Client: submit a sub-plan to a peer.                                */
/* ------------------------------------------------------------------ */

/* Called exactly once per submit: with the sub-plan's status (ARK_BUS_OK or a
 * negative ARK_BUS_E_* code from the far side), or ARK_BUS_E_TIMEOUT when no
 * reply arrives.  The requested buffers have already been written back. */
typedef void (*ark_bus_infer_remote_done_cb_t)(int status, void *user);

/*
 * One submission.  `tensors` is THIS node's table (the pointers the plan uses,
 * mapped to the handles the peer knows); `ret_ids` names the handles whose
 * contents should come back.  The struct must stay alive and unchanged until
 * the completion callback fires - the reply is written back through it.
 */
typedef struct {
    const ark_bus_infer_remote_tensor_t *tensors;
    uint8_t                              ntensors;
    const ark_bus_infer_handle_t        *ret_ids;
    uint8_t                              nret;
    ark_bus_infer_remote_done_cb_t       cb;
    void                                *user;
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
    /* FILLED BY THE CLIENT, not the caller: when the reply is too big for one
     * frame the completion callback fires with ARK_BUS_INFER_REMOTE_STAGED and
     * records here how many bytes are staged and with which token.  The caller
     * then pulls them with ark_bus_infer_remote_fetch() and writes the buffers
     * back with ark_bus_infer_remote_apply_reply(). */
    uint32_t                             stage_total;
    uint16_t                             stage_handle;
#endif
} ark_bus_infer_remote_req_t;

/*
 * Encode `plan` (with `req`'s table and return list) and send it to `dst` as an
 * RPC request for `method`.  Returns the number of bytes handed to the
 * transport (as ark_bus_rpc_request_id does), or a negative code: E_INVAL for a
 * bad argument or an operand pointer not in the table, E_NOSYS for a step that
 * cannot be shipped (a collective or a CALL), E_NOMEM when the frame does not
 * fit, or ARK_BUS_E_FULL when every RPC slot is busy.  `req_id_out`, when not
 * NULL, receives the request id for ark_bus_rpc_cancel().
 */
int ark_bus_infer_remote_submit(struct ark_bus *bus, uint32_t dst,
                                uint16_t method,
                                const ark_bus_infer_plan_t *plan,
                                const ark_bus_infer_remote_req_t *req,
                                uint16_t *req_id_out);

/*
 * The wire codec on its own: build the RUN payload for `plan`.  Exposed so a
 * caller can drive a transport other than RPC, and so the format is testable
 * without a bus.  Writes at most `cap` bytes and stores the true length in
 * *outlen.  Returns ARK_BUS_OK, or E_NOMEM with *outlen set to the needed size,
 * or the same E_INVAL / E_NOSYS / E_FULL codes as submit.
 */
int ark_bus_infer_remote_encode(const ark_bus_infer_plan_t *plan,
                                const ark_bus_infer_remote_tensor_t *tensors,
                                uint8_t ntensors,
                                const ark_bus_infer_handle_t *ret_ids,
                                uint8_t nret,
                                uint8_t *out, uint16_t cap, uint16_t *outlen);

/*
 * Write a (possibly staged) REPLY body - the bytes returned by a live RUN
 * reply or by ark_bus_infer_remote_fetch() - back into `req`'s buffers.  Same
 * checks as the inline path: a handle or length that disagrees with the
 * client's table is an error, never a truncation.  Returns ARK_BUS_OK or a
 * negative ARK_BUS_E_* code.
 */
int ark_bus_infer_remote_apply_reply(const uint8_t *body, uint16_t len,
                                     const ark_bus_infer_remote_req_t *req);

#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
/*
 * Pull a staged reply body into `out` (at most `out_cap` bytes), reading it in
 * chunks from `dst`.  `total_out` receives the whole staged size.  Returns
 * ARK_BUS_OK when the whole body was transferred, ARK_BUS_E_NOMEM when
 * out_cap is smaller than the staged size (total_out still set), or a
 * transport/RPC error.  Call it after a submit whose callback reported
 * ARK_BUS_INFER_REMOTE_STAGED, then hand the bytes to
 * ark_bus_infer_remote_apply_reply().
 */
int ark_bus_infer_remote_fetch(struct ark_bus *bus, uint32_t dst,
                               uint16_t handle,
                               uint8_t *out, uint32_t out_cap,
                               uint32_t *total_out);
#endif /* ARK_BUS_INFER_REMOTE_STAGE_ENABLED */

#endif /* ARK_BUS_INFER_REMOTE_ENABLED */

#endif /* ARK_BUS_INFER_REMOTE_H */
