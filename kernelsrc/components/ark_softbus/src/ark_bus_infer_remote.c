/*
 * ark_bus_infer_remote.c - run a plan on a peer (the M3.3 remote backend).
 *
 * See include/ark_bus_infer_remote.h for the model and the wire format.  The
 * short version: the client maps its operand pointers to 16-bit handles, ships
 * the step list in one RPC request, and the server maps the handles back to
 * its own buffers and runs the sub-plan.  No allocation, no blocking, and no
 * silent truncation anywhere: anything that does not fit or does not resolve
 * is a hard error.
 */
#include <string.h>
#include "ark_bus.h"
#include "ark_bus_infer.h"
#include "ark_bus_infer_remote.h"

#if ARK_BUS_INFER_REMOTE_ENABLED

#define ARK_BUS_INFER_REMOTE_MAGIC    0xB3C1u
#define ARK_BUS_INFER_REMOTE_VERSION  1u

/* ------------------------------------------------------------------ */
/* Little endian helpers - the wire is big-endian, like the RPC layer.  */
/* ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static int32_t rd_i16(const uint8_t *p)
{
    return (int32_t)(int16_t)rd16(p);
}

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v & 0xFFu);
}

static int put_i16(uint8_t *p, int32_t v)
{
    if (v < -32768 || v > 32767) {
        return ARK_BUS_E_INVAL;      /* a shape the wire cannot carry */
    }
    wr16(p, (uint16_t)(int16_t)v);
    return ARK_BUS_OK;
}

static void put_f32(uint8_t *p, float v)
{
    uint32_t u;
    memcpy(&u, &v, sizeof u);
    wr32(p, u);
}

static float get_f32(const uint8_t *p)
{
    uint32_t u = rd32(p);
    float    v;
    memcpy(&v, &u, sizeof v);
    return v;
}

/* ------------------------------------------------------------------ */
/* Handle resolution.                                                   */
/* ------------------------------------------------------------------ */

/* Local pointer -> the handle the peer knows, or E_INVAL when the buffer was
 * never declared: shipping an unnamed pointer would mean sending nothing. */
static int handle_of(const ark_bus_infer_remote_tensor_t *t, uint8_t n,
                     const void *ptr, uint16_t *out)
{
    uint8_t i;

    if (!ptr) {
        *out = ARK_BUS_INFER_HANDLE_NONE;
        return ARK_BUS_OK;
    }
    for (i = 0u; i < n; i++) {
        if (t[i].ptr == ptr) {
            *out = (uint16_t)t[i].id;
            return ARK_BUS_OK;
        }
    }
    return ARK_BUS_E_INVAL;
}

/* Handle -> the local buffer it names, or NULL. */
static void *ptr_of(const ark_bus_infer_server_t *srv, uint16_t handle)
{
    uint8_t i;

    if (handle == ARK_BUS_INFER_HANDLE_NONE) {
        return (void *)0;
    }
    for (i = 0u; i < srv->ntensors; i++) {
        if ((uint16_t)srv->tensors[i].id == handle) {
            return srv->tensors[i].ptr;
        }
    }
    return (void *)0;
}

/* ------------------------------------------------------------------ */
/* Codec.                                                               */
/* ------------------------------------------------------------------ */

/* A step this layer can carry: every kernel with no cross-rank dependency.
 * A collective would need the peer in this rank's group and a CALL carries a
 * function pointer, so both are refused here rather than on the far side. */
static int op_shippable(uint8_t op)
{
    switch (op) {
    case ARK_BUS_INFER_OP_GEMM:
    case ARK_BUS_INFER_OP_SILU:
    case ARK_BUS_INFER_OP_RMSNORM:
    case ARK_BUS_INFER_OP_ADD:
    case ARK_BUS_INFER_OP_SOFTMAX:
    case ARK_BUS_INFER_OP_ARGMAX:
    case ARK_BUS_INFER_OP_ATTN:
        return 1;
    default:
        return 0;
    }
}

int ark_bus_infer_remote_encode(const ark_bus_infer_plan_t *plan,
                                const ark_bus_infer_remote_tensor_t *tensors,
                                uint8_t ntensors,
                                const ark_bus_infer_handle_t *ret_ids,
                                uint8_t nret,
                                uint8_t *out, uint16_t cap, uint16_t *outlen)
{
    uint16_t need;
    uint8_t  i;
    uint8_t *p;

    if (!plan || !out || !outlen) {
        return ARK_BUS_E_INVAL;
    }
    if (plan->nsteps == 0u) {
        return ARK_BUS_E_INVAL;
    }
    if (plan->nsteps > ARK_BUS_INFER_REMOTE_MAX_STEPS ||
        nret > ARK_BUS_INFER_REMOTE_MAX_RET) {
        return ARK_BUS_E_FULL;
    }
    if (nret && !ret_ids) {
        return ARK_BUS_E_INVAL;
    }
    if (ntensors && !tensors) {
        return ARK_BUS_E_INVAL;
    }

    need = (uint16_t)(ARK_BUS_INFER_REMOTE_HDR_SIZE +
                      (uint16_t)plan->nsteps * ARK_BUS_INFER_REMOTE_STEP_SIZE +
                      (uint16_t)nret * 2u);
    if (cap < need) {
        *outlen = need;
        return ARK_BUS_E_NOMEM;
    }

    p = out;
    wr16(p, ARK_BUS_INFER_REMOTE_MAGIC);
    p[2] = (uint8_t)ARK_BUS_INFER_REMOTE_VERSION;
    p[3] = plan->nsteps;
    p[4] = nret;
    p[5] = 0u;
    p += ARK_BUS_INFER_REMOTE_HDR_SIZE;

    for (i = 0u; i < plan->nsteps; i++) {
        const ark_bus_infer_step_t *s = &plan->steps[i];
        uint16_t ha, hb, hc;
        int      rc;

        if (!op_shippable(s->op)) {
            /* A collective step (all-reduce, all-gather) would need the peer in
             * this rank's group; a CALL carries a function pointer.  Both are
             * unsupported by construction, so say NOSYS rather than INVAL. */
            return (s->op == ARK_BUS_INFER_OP_ALLREDUCE_SUM_F32 ||
                    s->op == ARK_BUS_INFER_OP_ALLGATHER ||
                    s->op == ARK_BUS_INFER_OP_CALL) ? ARK_BUS_E_NOSYS
                                                    : ARK_BUS_E_INVAL;
        }
        if ((rc = handle_of(tensors, ntensors, s->a, &ha)) != ARK_BUS_OK ||
            (rc = handle_of(tensors, ntensors, s->b, &hb)) != ARK_BUS_OK ||
            (rc = handle_of(tensors, ntensors, s->c, &hc)) != ARK_BUS_OK) {
            return rc;
        }

        p[0] = s->op;
        wr16(p + 1, ha);
        wr16(p + 3, hb);
        wr16(p + 5, hc);
        if ((rc = put_i16(p + 7, s->p0)) != ARK_BUS_OK ||
            (rc = put_i16(p + 9, s->p1)) != ARK_BUS_OK ||
            (rc = put_i16(p + 11, s->p2)) != ARK_BUS_OK ||
            (rc = put_i16(p + 13, s->p3)) != ARK_BUS_OK) {
            return rc;
        }
        put_f32(p + 15, s->f0);
        p += ARK_BUS_INFER_REMOTE_STEP_SIZE;
    }

    for (i = 0u; i < nret; i++) {
        uint8_t j;
        int     found = 0;

        /* The client must own the buffer it asks back, or the reply has
         * nowhere to land - refuse instead of quietly dropping it. */
        for (j = 0u; j < ntensors; j++) {
            if ((uint16_t)tensors[j].id == ret_ids[i]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return ARK_BUS_E_INVAL;
        }
        wr16(p, ret_ids[i]);
        p += 2;
    }

    *outlen = (uint16_t)(p - out);
    return ARK_BUS_OK;
}

/* ------------------------------------------------------------------ */
/* Server.                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int status;
} remote_run_ctx_t;

static void remote_run_done(int status, void *user)
{
    ((remote_run_ctx_t *)user)->status = status;
}

/* Write the reply body (status | nret | handle|nfloats|data ...) into `dst`,
 * which the caller has already sized - see the length pass in the handler.
 * Split out so the same bytes can go to the wire or to the staging buffer. */
static uint16_t build_reply_body(const ark_bus_infer_server_t *srv,
                                 const uint8_t *params, uint16_t need,
                                 uint8_t nret, uint8_t *dst)
{
    uint16_t off = 2u;
    uint8_t  i;

    dst[0] = (uint8_t)(int8_t)ARK_BUS_OK;
    dst[1] = nret;
    for (i = 0u; i < nret; i++) {
        uint16_t id = rd16(params + need - (uint16_t)nret * 2u +
                           (uint16_t)i * 2u);
        void    *tp = ptr_of(srv, id);
        uint32_t nf = 0u;
        uint8_t  j;

        for (j = 0u; j < srv->ntensors; j++) {
            if ((uint16_t)srv->tensors[j].id == id) {
                nf = srv->tensors[j].nfloats;
                break;
            }
        }
        wr16(dst + off, id);
        wr32(dst + off + 2u, nf);
        off = (uint16_t)(off + 6u);
        if (tp && nf) {
            memcpy(dst + off, tp, nf * 4u);
        }
        off = (uint16_t)(off + nf * 4u);
    }
    return off;
}

/* Write the status-only reply (no buffers).  Always the first thing on the
 * wire so the client sees a job status even when nothing can be returned. */
static int reply_status_only(uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                             int status)
{
    if (rcap < 2u) {
        return ARK_BUS_E_NOMEM;
    }
    reply[0] = (uint8_t)(int8_t)status;
    reply[1] = 0u;
    *rlen = 2u;
    return ARK_BUS_OK;
}

static int infer_remote_handle(struct ark_bus *bus, uint32_t from,
                               const uint8_t *params, uint16_t plen,
                               uint8_t *reply, uint16_t rcap,
                               uint16_t *rlen, void *user)
{
    ark_bus_infer_server_t *srv = (ark_bus_infer_server_t *)user;
    ark_bus_infer_step_t    steps[ARK_BUS_INFER_REMOTE_MAX_STEPS];
    remote_run_ctx_t        ctx;
    const uint8_t          *p;
    uint16_t                need, off;
    uint8_t                 nsteps, nret, i;
    int                     rc;

    (void)from;

    if (!srv || !params || !rlen) {
        return ARK_BUS_E_INVAL;
    }
    if (plen < ARK_BUS_INFER_REMOTE_HDR_SIZE ||
        rd16(params) != ARK_BUS_INFER_REMOTE_MAGIC ||
        params[2] != (uint8_t)ARK_BUS_INFER_REMOTE_VERSION) {
        return ARK_BUS_E_FORMAT;
    }
    nsteps = params[3];
    nret   = params[4];
    if (nsteps == 0u || nsteps > ARK_BUS_INFER_REMOTE_MAX_STEPS ||
        nret > ARK_BUS_INFER_REMOTE_MAX_RET) {
        return ARK_BUS_E_FORMAT;
    }
    need = (uint16_t)(ARK_BUS_INFER_REMOTE_HDR_SIZE +
                      (uint16_t)nsteps * ARK_BUS_INFER_REMOTE_STEP_SIZE +
                      (uint16_t)nret * 2u);
    if (plen < need) {
        return ARK_BUS_E_FORMAT;
    }

    /* Rebuild the sub-plan against THIS node's buffers.  The scratch is a
     * stack local bounded by ARK_BUS_INFER_REMOTE_MAX_STEPS - a shipped plan
     * carries no collective, so it also has no group. */
    p = params + ARK_BUS_INFER_REMOTE_HDR_SIZE;
    for (i = 0u; i < nsteps; i++) {
        ark_bus_infer_step_t *s = &steps[i];
        uint16_t ha = rd16(p + 1);
        uint16_t hb = rd16(p + 3);
        uint16_t hc = rd16(p + 5);

        if (!op_shippable(p[0])) {
            srv->jobs_failed++;   /* a job we declined to serve*/
            return ARK_BUS_E_FORMAT;
        }
        s->op  = p[0];
        s->rsv8 = 0u;
        s->rsv16 = 0u;
        s->a   = ptr_of(srv, ha);
        s->b   = ptr_of(srv, hb);
        s->c   = ptr_of(srv, hc);
        s->p0  = rd_i16(p + 7);
        s->p1  = rd_i16(p + 9);
        s->p2  = rd_i16(p + 11);
        s->p3  = rd_i16(p + 13);
        s->f0  = get_f32(p + 15);

        /* A named operand that this node does not hold is a protocol error,
         * not an empty buffer: say so rather than run against NULL. */
        if ((ha != ARK_BUS_INFER_HANDLE_NONE && !s->a) ||
            (hb != ARK_BUS_INFER_HANDLE_NONE && !s->b) ||
            (hc != ARK_BUS_INFER_HANDLE_NONE && !s->c)) {
            srv->jobs_failed++;   /* a job we declined to serve*/
            return ARK_BUS_E_FORMAT;
        }
        p += ARK_BUS_INFER_REMOTE_STEP_SIZE;
    }

    /* Run it here.  A shipped plan has no collective, so it finishes inside
     * run(); if it ever parked we could not answer within the handler. */
    ctx.status = ARK_BUS_OK;
    rc = ark_bus_infer_run_steps(bus, srv->be, steps, nsteps,
                                 (const ark_bus_coll_group_t *)0,
                                 remote_run_done, &ctx);
    if (rc == ARK_BUS_OK && ark_bus_infer_pending(bus)) {
        rc = ARK_BUS_E_STATE;
    }
    if (rc != ARK_BUS_OK) {
        srv->jobs_failed++;
        return reply_status_only(reply, rcap, rlen, rc);
    }
    if (ctx.status != ARK_BUS_OK) {
        srv->jobs_failed++;
        return reply_status_only(reply, rcap, rlen, ctx.status);
    }

    /* Size the reply: status | nret | (handle | nfloats | data)... .  If the
     * buffers do not fit one frame we stage them (when enabled) or say
     * E_NOMEM - never a partial copy. */
    off = 2u;
    for (i = 0u; i < nret; i++) {
        uint16_t id = rd16(params + need - (uint16_t)nret * 2u + (uint16_t)i * 2u);
        void    *tp = ptr_of(srv, id);
        uint32_t nf;
        uint8_t  j;

        if (!tp) {
            srv->jobs_failed++;
            return reply_status_only(reply, rcap, rlen, ARK_BUS_E_INVAL);
        }
        nf = 0u;
        for (j = 0u; j < srv->ntensors; j++) {
            if ((uint16_t)srv->tensors[j].id == id) {
                nf = srv->tensors[j].nfloats;
                break;
            }
        }
        off = (uint16_t)(off + 6u + nf * 4u);
    }
    if (off > rcap) {
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
        /* Too big for one reply: stage the whole body and hand back a token
         * the client pulls in chunks with INFER_FETCH. */
        if (off <= (uint16_t)ARK_BUS_INFER_REMOTE_STAGE_MAX && rcap >= 7u) {
            (void)build_reply_body(srv, params, need, nret, srv->stage);
            srv->stage_len    = off;
            srv->stage_token++;
            if (srv->stage_token == 0u ||
                srv->stage_token == ARK_BUS_INFER_HANDLE_NONE) {
                srv->stage_token++;
            }
            srv->stage_handle = srv->stage_token;
            srv->stage_owner  = from;
            srv->stage_used   = 1u;
            reply[0] = (uint8_t)(int8_t)ARK_BUS_INFER_REMOTE_STAGED;
            wr32(reply + 1u, (uint32_t)off);
            wr16(reply + 5u, srv->stage_handle);
            srv->jobs_served++;
            *rlen = 7u;
            return ARK_BUS_OK;
        }
#endif
        srv->jobs_failed++;
        return reply_status_only(reply, rcap, rlen, ARK_BUS_E_NOMEM);
    }

    (void)build_reply_body(srv, params, need, nret, reply);
    srv->jobs_served++;
    *rlen = off;
    return ARK_BUS_OK;
}

#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
/* INFER_FETCH: handle u16 | off u32 | max u8 -> status i8 | total u32 | n u8 | bytes */
static int infer_fetch_handle(struct ark_bus *bus, uint32_t from,
                              const uint8_t *params, uint16_t plen,
                              uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                              void *user)
{
    ark_bus_infer_server_t *srv = (ark_bus_infer_server_t *)user;
    uint16_t handle;
    uint32_t off;
    uint8_t  max;
    uint32_t avail;
    uint32_t n;

    (void)bus;

    if (!srv || !reply || !rlen || rcap < 1u) {
        return ARK_BUS_E_INVAL;
    }
    if (!params || plen < ARK_BUS_INFER_FETCH_REQ_SIZE) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    handle = rd16(params);
    off    = rd32(params + 2u);
    max    = params[6];

    /* A token is good only while its body is staged, and only its owner (the
     * caller the reply belongs to) may read it. */
    if (!srv->stage_used || handle != srv->stage_handle) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_NOTFOUND;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    if (from != srv->stage_owner) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_DENIED;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    if (rcap < ARK_BUS_INFER_FETCH_HDR_SIZE) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_FULL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    if (off > (uint32_t)srv->stage_len) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    avail = (uint32_t)srv->stage_len - off;
    n = ((uint32_t)max < avail) ? (uint32_t)max : avail;
    if (n > (uint32_t)(rcap - ARK_BUS_INFER_FETCH_HDR_SIZE)) {
        n = (uint32_t)(rcap - ARK_BUS_INFER_FETCH_HDR_SIZE);
    }
    reply[0] = 0u;
    wr32(reply + 1u, (uint32_t)srv->stage_len);
    reply[5] = (uint8_t)n;
    if (n) {
        memcpy(reply + ARK_BUS_INFER_FETCH_HDR_SIZE, srv->stage + off,
               (uint16_t)n);
    }
    if (off + n >= (uint32_t)srv->stage_len) {
        srv->stage_used = 0u;   /* the whole body has been delivered */
    }
    *rlen = (uint16_t)(ARK_BUS_INFER_FETCH_HDR_SIZE + n);
    return ARK_BUS_OK;
}
#endif /* ARK_BUS_INFER_REMOTE_STAGE_ENABLED */

int ark_bus_infer_server_init(ark_bus_infer_server_t *srv, struct ark_bus *bus,
                              uint16_t method,
                              const ark_bus_infer_backend_t *be)
{
    int rc;

    if (!srv || !bus || !be) {
        return ARK_BUS_E_INVAL;
    }
    srv->bus      = bus;
    srv->be       = be;
    srv->method   = method;
    srv->ntensors = 0u;
    srv->jobs_served = 0u;
    srv->jobs_failed = 0u;
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
    srv->stage_len    = 0u;
    srv->stage_handle = 0u;
    srv->stage_token  = 0u;
    srv->stage_owner  = 0u;
    srv->stage_used   = 0u;
#endif

    rc = ark_bus_rpc_register(bus, method, infer_remote_handle, srv);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
    /* FETCH is a fixed system id, so a client knows where to pull from
     * without being told; the RUN method stays caller-chosen. */
    rc = ark_bus_rpc_register(bus, ARK_BUS_M_INFER_FETCH, infer_fetch_handle,
                              srv);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
#endif
    srv->attached = 1u;
    return ARK_BUS_OK;
}

int ark_bus_infer_server_bind(ark_bus_infer_server_t *srv,
                              ark_bus_infer_handle_t id,
                              void *ptr, uint32_t nfloats)
{
    uint8_t i;

    if (!srv || !ptr || nfloats == 0u || id == ARK_BUS_INFER_HANDLE_NONE) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0u; i < srv->ntensors; i++) {
        if (srv->tensors[i].id == id) {
            srv->tensors[i].ptr     = ptr;
            srv->tensors[i].nfloats = nfloats;
            return ARK_BUS_OK;
        }
    }
    if (srv->ntensors >= (uint8_t)ARK_BUS_INFER_REMOTE_MAX_TENSORS) {
        return ARK_BUS_E_FULL;
    }
    srv->tensors[srv->ntensors].id      = id;
    srv->tensors[srv->ntensors].rsv     = 0u;
    srv->tensors[srv->ntensors].ptr     = ptr;
    srv->tensors[srv->ntensors].nfloats = nfloats;
    srv->ntensors++;
    return ARK_BUS_OK;
}

/* The RPC layer has no unregister, so tearing a server down leaves a handler
 * that refuses: the method must stop serving either way. */
static int infer_remote_refuse(struct ark_bus *bus, uint32_t from,
                               const uint8_t *params, uint16_t plen,
                               uint8_t *reply, uint16_t rcap,
                               uint16_t *rlen, void *user)
{
    (void)bus; (void)from; (void)params; (void)plen;
    (void)reply; (void)rcap; (void)rlen; (void)user;
    return ARK_BUS_E_NOSYS;
}

void ark_bus_infer_server_fini(ark_bus_infer_server_t *srv)
{
    if (!srv || !srv->attached) {
        return;
    }
    (void)ark_bus_rpc_register(srv->bus, srv->method, infer_remote_refuse,
                               (void *)0);
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
    (void)ark_bus_rpc_register(srv->bus, ARK_BUS_M_INFER_FETCH,
                               infer_remote_refuse, (void *)0);
#endif
    srv->attached = 0u;
}

/* ------------------------------------------------------------------ */
/* Client.                                                              */
/* ------------------------------------------------------------------ */

/* Unpack the reply into the client's own buffers, keyed by handle. */
int ark_bus_infer_remote_apply_reply(const uint8_t *result, uint16_t len,
                                     const ark_bus_infer_remote_req_t *req)
{
    uint16_t off;
    uint8_t  nret, i;

    if (!result || len < 2u) {
        return ARK_BUS_E_FORMAT;
    }
    if ((int)(int8_t)result[0] != ARK_BUS_OK) {
        return (int)(int8_t)result[0];
    }
    nret = result[1];
    /* We asked for req->nret buffers; a short answer means they did not fit
     * on the wire, which must surface as an error, not a silent pass. */
    if (nret != req->nret) {
        return ARK_BUS_E_NOMEM;
    }

    off = 2u;
    for (i = 0u; i < nret; i++) {
        uint16_t id;
        uint32_t nf;
        uint8_t  j;
        int      found = 0;

        if ((uint32_t)off + 6u > (uint32_t)len) {
            return ARK_BUS_E_FORMAT;
        }
        id = rd16(result + off);
        nf = rd32(result + off + 2u);
        off = (uint16_t)(off + 6u);
        if ((uint32_t)off + nf * 4u > (uint32_t)len) {
            return ARK_BUS_E_FORMAT;
        }
        for (j = 0u; j < req->ntensors; j++) {
            if ((uint16_t)req->tensors[j].id == id) {
                if (req->tensors[j].nfloats != nf) {
                    return ARK_BUS_E_FORMAT;
                }
                memcpy(req->tensors[j].ptr, result + off, nf * 4u);
                found = 1;
                break;
            }
        }
        if (!found) {
            return ARK_BUS_E_INVAL;
        }
        off = (uint16_t)(off + nf * 4u);
    }
    return ARK_BUS_OK;
}

static void remote_rpc_done(int status, const uint8_t *result, uint16_t len,
                            void *user)
{
    ark_bus_infer_remote_req_t *req = (ark_bus_infer_remote_req_t *)user;
    int job = status;

    if (status == ARK_BUS_OK) {
#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
        /* A staged reply carries a size and a token, not the buffers; surface
         * that to the caller rather than pretend the tensors are ready. */
        if (result && len >= 7u &&
            (int)(int8_t)result[0] == ARK_BUS_INFER_REMOTE_STAGED) {
            req->stage_total  = rd32(result + 1u);
            req->stage_handle = rd16(result + 5u);
            job = ARK_BUS_INFER_REMOTE_STAGED;
        } else
#endif
        {
            job = ark_bus_infer_remote_apply_reply(result, len, req);
        }
    }
    if (req->cb) {
        req->cb(job, req->user);
    }
}

int ark_bus_infer_remote_submit(struct ark_bus *bus, uint32_t dst,
                                uint16_t method,
                                const ark_bus_infer_plan_t *plan,
                                const ark_bus_infer_remote_req_t *req,
                                uint16_t *req_id_out)
{
    uint8_t  buf[ARK_BUS_MSG_MAX_SIZE];
    uint16_t len = 0;
    int      rc;

    if (!bus || !plan || !req) {
        return ARK_BUS_E_INVAL;
    }
    rc = ark_bus_infer_remote_encode(plan, req->tensors, req->ntensors,
                                     req->ret_ids, req->nret,
                                     buf, (uint16_t)sizeof buf, &len);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    /* `req` is the completion context, so it must outlive the request. */
    return ark_bus_rpc_request_id(bus, dst, method, buf, len,
                                  remote_rpc_done, (void *)req, req_id_out);
}

#if ARK_BUS_INFER_REMOTE_STAGE_ENABLED
int ark_bus_infer_remote_fetch(struct ark_bus *bus, uint32_t dst,
                               uint16_t handle,
                               uint8_t *out, uint32_t out_cap,
                               uint32_t *total_out)
{
    uint32_t off = 0u;
    uint32_t total = 0u;

    if (!bus || !out || out_cap == 0u) {
        return ARK_BUS_E_INVAL;
    }

    for (;;) {
        uint8_t  p[ARK_BUS_INFER_FETCH_REQ_SIZE];
        uint8_t  reply[ARK_BUS_MSG_MAX_SIZE];
        uint16_t rlen = 0u;
        uint32_t room = out_cap - off;
        uint32_t n;
        int      rc;

        wr16(p, handle);
        wr32(p + 2u, off);
        p[6] = (uint8_t)((room > 255u) ? 255u : room);

        rc = ark_bus_rpc_call(bus, dst, ARK_BUS_M_INFER_FETCH, p,
                              (uint16_t)sizeof p, reply, (uint16_t)sizeof reply,
                              &rlen);
        if (rc != ARK_BUS_OK) {
            return rc;
        }
        /* An error reply is a lone status byte; only a success carries the
         * full header.  Read the status first, then demand the rest. */
        if (rlen < 1u) {
            return ARK_BUS_E_FORMAT;
        }
        if ((int)(int8_t)reply[0] != ARK_BUS_OK) {
            return (int)(int8_t)reply[0];
        }
        if (rlen < ARK_BUS_INFER_FETCH_HDR_SIZE) {
            return ARK_BUS_E_FORMAT;
        }
        total = rd32(reply + 1u);
        n = reply[5];
        if (n > (uint32_t)(rlen - ARK_BUS_INFER_FETCH_HDR_SIZE)) {
            return ARK_BUS_E_FORMAT;
        }
        /* The first chunk already names the whole size, so a buffer that can
         * never hold it is refused up front with the true size reported -
         * not discovered by spinning until the window closes. */
        if (total > out_cap) {
            if (total_out) {
                *total_out = total;
            }
            return ARK_BUS_E_NOMEM;   /* caller's buffer is too small */
        }
        if (off + n > out_cap) {
            return ARK_BUS_E_FORMAT;  /* server overran the window we asked for */
        }
        if (n) {
            memcpy(out + off, reply + ARK_BUS_INFER_FETCH_HDR_SIZE,
                   (size_t)n);
        }
        off += n;
        if (off >= total) {
            break;
        }
        if (n == 0u) {
            return ARK_BUS_E_FORMAT;  /* no progress: refuse to spin */
        }
    }

    if (total_out) {
        *total_out = total;
    }
    return ARK_BUS_OK;
}
#endif /* ARK_BUS_INFER_REMOTE_STAGE_ENABLED */

#endif /* ARK_BUS_INFER_REMOTE_ENABLED */
