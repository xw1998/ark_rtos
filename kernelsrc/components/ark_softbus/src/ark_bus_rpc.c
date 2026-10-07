/*
 * ark_bus_rpc.c - request/reply over the directed-send path.
 *
 * See ark_bus_rpc.h for the contract.  The layer is deliberately small: a
 * fixed slot table of outstanding requests, a fixed method table of local
 * handlers, and a timeout sweep.  Nothing here allocates and nothing blocks.
 */
#include "ark_bus_priv.h"
#include "ark_bus_rpc.h"

#if ARK_BUS_RPC_ENABLED

/* ------------------------------------------------------------------ */
/* Table helpers                                                       */
/* ------------------------------------------------------------------ */

static ark_bus_rpc_slot_t *slot_free(ark_bus_rpc_state_t *r)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_RPC_MAX_SLOTS; i++) {
        if (!r->slots[i].used) {
            return &r->slots[i];
        }
    }
    return NULL;
}

static ark_bus_rpc_slot_t *slot_find(ark_bus_rpc_state_t *r,
                                     uint32_t peer, uint16_t req_id)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_RPC_MAX_SLOTS; i++) {
        ark_bus_rpc_slot_t *s = &r->slots[i];
        if (s->used && s->state == 1u &&
            s->peer_id == peer && s->req_id == req_id) {
            return s;
        }
    }
    return NULL;
}

static ark_bus_rpc_method_t *method_find(ark_bus_rpc_state_t *r, uint16_t method)
{
    uint8_t i;
    for (i = 0; i < r->method_count; i++) {
        if (r->methods[i].method == method) {
            return &r->methods[i];
        }
    }
    return NULL;
}

static int req_id_in_use(const ark_bus_rpc_state_t *r, uint16_t id)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_RPC_MAX_SLOTS; i++) {
        if (r->slots[i].used && r->slots[i].req_id == id) {
            return 1;
        }
    }
    return 0;
}

/* Allocate an id that is not currently in flight, so a late reply cannot be
 * mistaken for a newer request that happens to reuse the same id. */
static uint16_t req_id_alloc(ark_bus_rpc_state_t *r)
{
    uint16_t id = r->next_req_id;

    do {
        id++;
        if (id == 0u) {
            id = 1u;   /* 0 stays unused */
        }
    } while (req_id_in_use(r, id));

    r->next_req_id = id;
    return id;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int ark_bus_rpc_has_method(const ark_bus_t *bus, uint16_t method)
{
    if (!bus) {
        return 0;
    }
    return method_find((ark_bus_rpc_state_t *)&bus->rpc, method) != NULL;
}

int ark_bus_rpc_register(ark_bus_t *bus, uint16_t method,
                         ark_bus_rpc_handler_t handler, void *user)
{
    ark_bus_rpc_method_t *m;

    if (!bus || !handler || method == 0u) {
        return ARK_BUS_E_INVAL;
    }

    m = method_find(&bus->rpc, method);
    if (m) {
        m->handler = handler;
        m->user    = user;
        return ARK_BUS_OK;
    }
    if (bus->rpc.method_count >= ARK_BUS_RPC_MAX_METHODS) {
        return ARK_BUS_E_FULL;
    }
    m = &bus->rpc.methods[bus->rpc.method_count++];
    m->method  = method;
    m->handler = handler;
    m->user    = user;
    return ARK_BUS_OK;
}

int ark_bus_rpc_request_id(ark_bus_t *bus, uint32_t dst, uint16_t method,
                           const void *params, uint16_t len,
                           ark_bus_rpc_cb_t cb, void *user,
                           uint16_t *req_id_out)
{
    ark_bus_rpc_slot_t *s;
    uint8_t  p[ARK_BUS_MSG_MAX_SIZE];
    uint16_t req_id;
    int      n;

    if (!bus || dst == ARK_BUS_ID_BROADCAST || method == 0u) {
        return ARK_BUS_E_INVAL;
    }
    if (len > (uint16_t)(ARK_BUS_MSG_MAX_SIZE - ARK_BUS_RPC_REQ_HDR)) {
        return ARK_BUS_E_INVAL;
    }

    s = slot_free(&bus->rpc);
    if (!s) {
        return ARK_BUS_E_FULL;
    }

    req_id = req_id_alloc(&bus->rpc);
    ark_wr16(p, req_id);
    ark_wr16(p + 2, method);
    if (len && params) {
        ark_memcpy(p + ARK_BUS_RPC_REQ_HDR, params, len);
    }

    n = ark_bus_emit(bus, dst, ARK_BUS_MSG_RPC_REQ, ARK_BUS_TOPIC_NONE,
                     p, (uint16_t)(ARK_BUS_RPC_REQ_HDR + len),
                     ARK_BUS_FLAG_QOS1);
    if (n < 0) {
        return n;
    }

    s->used       = 1u;
    s->state      = 1u;
    s->req_id     = req_id;
    s->method     = method;
    s->peer_id    = dst;
    s->sent_at_ms = ark_bus_tick_ms();
    s->cb         = cb;
    s->user       = user;
    if (req_id_out) {
        *req_id_out = req_id;
    }
    return n;
}

int ark_bus_rpc_request(ark_bus_t *bus, uint32_t dst, uint16_t method,
                        const void *params, uint16_t len,
                        ark_bus_rpc_cb_t cb, void *user)
{
    return ark_bus_rpc_request_id(bus, dst, method, params, len, cb, user,
                                  NULL);
}

int ark_bus_rpc_cancel(ark_bus_t *bus, uint16_t req_id)
{
    uint8_t i;

    if (!bus || req_id == 0u) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < ARK_BUS_RPC_MAX_SLOTS; i++) {
        ark_bus_rpc_slot_t *s = &bus->rpc.slots[i];
        if (s->used && s->req_id == req_id) {
            s->used  = 0u;
            s->state = 0u;
            return ARK_BUS_OK;
        }
    }
    return ARK_BUS_E_NOTFOUND;
}

uint8_t ark_bus_rpc_pending(const ark_bus_t *bus)
{
    uint8_t i, n = 0;
    if (!bus) {
        return 0;
    }
    for (i = 0; i < ARK_BUS_RPC_MAX_SLOTS; i++) {
        if (bus->rpc.slots[i].used) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Incoming frames                                                     */
/* ------------------------------------------------------------------ */

/* A request arrived: run the handler and answer with RPC_REP. */
static void rpc_serve(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    uint8_t  rep[ARK_BUS_MSG_MAX_SIZE];
    uint16_t req_id, method, rlen = 0;
    int      st;
    ark_bus_rpc_method_t *m;

    if (f->payload_len < ARK_BUS_RPC_REQ_HDR) {
        return; /* malformed: too short to hold req_id + method */
    }

    req_id = ark_rd16(f->payload);
    method = ark_rd16(f->payload + 2);
    m = method_find(&bus->rpc, method);

    ark_wr16(rep, req_id);
    ark_wr16(rep + 2, method);

    if (!m) {
        st = ARK_BUS_E_NOSYS;
    } else {
        st = m->handler(bus, f->src_id,
                        f->payload + ARK_BUS_RPC_REQ_HDR,
                        (uint16_t)(f->payload_len - ARK_BUS_RPC_REQ_HDR),
                        rep + ARK_BUS_RPC_REP_HDR,
                        (uint16_t)(ARK_BUS_MSG_MAX_SIZE - ARK_BUS_RPC_REP_HDR),
                        &rlen, m->user);
        if (rlen > (uint16_t)(ARK_BUS_MSG_MAX_SIZE - ARK_BUS_RPC_REP_HDR)) {
            rlen = (uint16_t)(ARK_BUS_MSG_MAX_SIZE - ARK_BUS_RPC_REP_HDR);
        }
    }

    rep[4] = (uint8_t)(int8_t)st;   /* status: 0 ok, negative ARK_BUS_E_* */

    (void)ark_bus_emit(bus, f->src_id, ARK_BUS_MSG_RPC_REP, ARK_BUS_TOPIC_NONE,
                       rep, (uint16_t)(ARK_BUS_RPC_REP_HDR + rlen),
                       ARK_BUS_FLAG_QOS1);
}

/* A reply arrived: match it to an outstanding request and complete it. */
static void rpc_complete(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    ark_bus_rpc_slot_t *s;
    uint16_t req_id;
    int      status;
    const uint8_t *result = NULL;
    uint16_t rlen = 0;

    if (f->payload_len < ARK_BUS_RPC_REP_HDR) {
        return;
    }
    req_id = ark_rd16(f->payload);
    status = (int)(int8_t)f->payload[4];
    result = f->payload + ARK_BUS_RPC_REP_HDR;
    rlen   = (uint16_t)(f->payload_len - ARK_BUS_RPC_REP_HDR);

    s = slot_find(&bus->rpc, f->src_id, req_id);
    if (!s) {
        return; /* late or duplicate reply: nothing to do */
    }

    /* Free the slot before the callback, so the callback may start a new
     * request that reuses it. */
    s->used = 0u;
    if (s->cb) {
        s->cb(status, status == 0 ? result : NULL,
              status == 0 ? rlen : 0u, s->user);
    }
}

void ark_bus_rpc_handle(ark_bus_t *bus, const ark_bus_frame_t *f)
{
    if (!bus || !f) {
        return;
    }
    if (f->msg_type == ARK_BUS_MSG_RPC_REQ) {
        rpc_serve(bus, f);
    } else if (f->msg_type == ARK_BUS_MSG_RPC_REP) {
        rpc_complete(bus, f);
    }
}

/* ------------------------------------------------------------------ */
/* Timeout sweep                                                       */
/* ------------------------------------------------------------------ */

void ark_bus_rpc_tick(ark_bus_t *bus)
{
    uint32_t now;
    uint8_t  i;

    if (!bus) {
        return;
    }
    now = ark_bus_tick_ms();
    for (i = 0; i < ARK_BUS_RPC_MAX_SLOTS; i++) {
        ark_bus_rpc_slot_t *s = &bus->rpc.slots[i];
        if (!s->used) {
            continue;
        }
        if ((uint32_t)(now - s->sent_at_ms) >=
            (uint32_t)ARK_BUS_RPC_TIMEOUT_MS) {
            ark_bus_rpc_cb_t cb = s->cb;
            void            *u  = s->user;
            s->used = 0u;
            if (cb) {
                cb(ARK_BUS_E_TIMEOUT, NULL, 0u, u);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Blocking wrapper                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    int       done;
    int       status;
    uint8_t  *reply;
    uint16_t  rcap;
    uint16_t  rlen;
} rpc_call_ctx_t;

static void rpc_call_cb(int status, const uint8_t *result,
                        uint16_t len, void *user)
{
    rpc_call_ctx_t *ctx = (rpc_call_ctx_t *)user;
    uint16_t        n;

    ctx->status = status;
    ctx->rlen   = 0;
    if (status == 0 && result && ctx->reply && ctx->rcap) {
        n = len < ctx->rcap ? len : ctx->rcap;
        ark_memcpy(ctx->reply, result, n);
        ctx->rlen = len;   /* report the true length, may exceed rcap */
    }
    ctx->done = 1;
}

int ark_bus_rpc_call(ark_bus_t *bus, uint32_t dst, uint16_t method,
                     const void *params, uint16_t len,
                     uint8_t *reply, uint16_t rcap, uint16_t *rlen)
{
    rpc_call_ctx_t ctx;
    int            rc;

    if (!bus || !reply || !rlen) {
        return ARK_BUS_E_INVAL;
    }

    ctx.done   = 0;
    ctx.status = ARK_BUS_E_INVAL;
    ctx.reply  = reply;
    ctx.rcap   = rcap;
    ctx.rlen   = 0;

    rc = ark_bus_rpc_request(bus, dst, method, params, len,
                             rpc_call_cb, &ctx);
    if (rc < 0) {
        return rc;
    }

    while (!ctx.done) {
        ark_bus_poll(bus);
    }

    *rlen = ctx.rlen;
    return ctx.status;
}

#endif /* ARK_BUS_RPC_ENABLED */
