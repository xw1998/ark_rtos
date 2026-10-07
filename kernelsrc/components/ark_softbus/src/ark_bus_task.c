/*
 * ark_bus_task.c - the task engine.
 *
 * Coordinator side: submit tasks, pick a worker by capability, follow the RPC
 * completion, retry on a different worker, keep the result.
 * Worker side: serve TASK_RUN with a small idempotence cache.
 *
 * Everything is bounded and static; see ark_bus_task.h for the contract.
 */
#include "ark_bus_priv.h"
#include "ark_bus_task.h"
#include "ark_bus_caps.h"
#include "ark_bus_rpc.h"

#if ARK_BUS_TASK_ENABLED

/* TASK_RUN payload: task id (2, BE) followed by the user params. */
#define TASK_RUN_ID_HDR 2u

static int task_is_terminal(uint8_t state)
{
    return state == (uint8_t)ARK_BUS_TASK_DONE ||
           state == (uint8_t)ARK_BUS_TASK_FAILED ||
           state == (uint8_t)ARK_BUS_TASK_LOST ||
           state == (uint8_t)ARK_BUS_TASK_CANCELLED;
}

/* ------------------------------------------------------------------ */
/* Table helpers                                                       */
/* ------------------------------------------------------------------ */

static ark_bus_task_entry_t *entry_find(ark_bus_t *bus, uint16_t id)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (bus->tasks.entries[i].used && bus->tasks.entries[i].task_id == id) {
            return &bus->tasks.entries[i];
        }
    }
    return NULL;
}

/* A free record, or the first finished one, which is safe to reuse. */
static ark_bus_task_entry_t *entry_free(ark_bus_t *bus)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (!bus->tasks.entries[i].used) {
            return &bus->tasks.entries[i];
        }
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (task_is_terminal(bus->tasks.entries[i].state)) {
            return &bus->tasks.entries[i];
        }
    }
    return NULL;
}

static uint16_t task_id_alloc(ark_bus_t *bus)
{
    uint16_t id = bus->tasks.next_task_id;
    do {
        id++;
        if (id == 0u) {
            id = 1u;
        }
    } while (entry_find(bus, id) != NULL);
    bus->tasks.next_task_id = id;
    return id;
}

static int result_find(const ark_bus_t *bus, uint16_t task_id)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_TASK_RESULT_SLOTS; i++) {
        if (bus->tasks.results[i].owner_task == task_id) {
            return (int)i;
        }
    }
    return -1;
}

/* Reserve a result slot for `task_id`, evicting round-robin if need be. */
static int result_alloc(ark_bus_t *bus, uint16_t task_id)
{
    ark_bus_task_pool_t *ts = &bus->tasks;
    uint8_t i;

    for (i = 0; i < ARK_BUS_TASK_RESULT_SLOTS; i++) {
        if (ts->results[i].owner_task == task_id) {
            return (int)i;
        }
    }
    for (i = 0; i < ARK_BUS_TASK_RESULT_SLOTS; i++) {
        if (ts->results[i].owner_task == 0u) {
            ts->results[i].owner_task = task_id;
            ts->results[i].len        = 0u;
            return (int)i;
        }
    }
    i = (uint8_t)(ts->result_next % ARK_BUS_TASK_RESULT_SLOTS);
    ts->result_next = (uint8_t)((i + 1u) % ARK_BUS_TASK_RESULT_SLOTS);
    ts->results[i].owner_task = task_id;
    ts->results[i].len        = 0u;
    return (int)i;
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                            */
/* ------------------------------------------------------------------ */

static void task_rpc_cb(int status, const uint8_t *result,
                        uint16_t len, void *user);

#if ARK_BUS_MAX_DEVICES > 32
#error "task retry bitmap holds 32 devices; ARK_BUS_MAX_DEVICES is too large"
#endif

/* Device-table index of `id`, or -1 when it is not a known peer. */
static int device_index(const ark_bus_t *bus, uint32_t id)
{
    uint8_t i;
    for (i = 0; i < bus->device_count; i++) {
        if (bus->devices[i].id == id) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * Choose the worker for an automatic dispatch.  `tried` is the set of device
 * indices this task has already been sent to, so a retry never bounces back to
 * a worker that just failed -- that is what makes a redelivery actually move
 * on when the peer is still listed ONLINE (its liveness timeout has not
 * elapsed yet).
 */
static const ark_bus_device_t *pick_worker(ark_bus_t *bus, uint32_t require_caps,
                                           uint32_t tried)
{
    uint8_t                 cursor = 0;
    const ark_bus_device_t *d;

    while ((d = ark_bus_find_by_cap(bus, require_caps, &cursor)) != NULL) {
        int idx = device_index(bus, d->id);
        if (idx < 0 || idx >= 32 || (tried & (1u << (unsigned)idx)) == 0u) {
            return d;
        }
    }
    return NULL;
}

/*
 * Send the task to a worker.  Returns the bytes sent, or a negative error:
 * ARK_BUS_E_NOTFOUND when no worker matches (the task stays QUEUED).
 */
static int task_dispatch(ark_bus_t *bus, ark_bus_task_entry_t *e)
{
    uint8_t  payload[ARK_BUS_MSG_MAX_SIZE];
    uint32_t worker = e->worker_id;
    int      n, idx;

    if (worker == 0u) {
        const ark_bus_device_t *d = pick_worker(bus, e->require_caps,
                                                e->tried_mask);
        if (!d && e->tried_mask != 0u) {
            /* Every candidate was tried once; allow a second lap rather than
             * starving the task forever on a single-worker deployment. */
            e->tried_mask = 0u;
            d = pick_worker(bus, e->require_caps, 0u);
        }
        if (!d) {
            return ARK_BUS_E_NOTFOUND;
        }
        worker = d->id;
    }

    /* Remember this attempt so a later retry moves on to a fresh worker. */
    idx = device_index(bus, worker);
    if (idx >= 0 && idx < 32) {
        e->tried_mask |= (uint32_t)(1u << (unsigned)idx);
    }

    ark_wr16(payload, e->task_id);
    if (e->params_len) {
        ark_memcpy(payload + TASK_RUN_ID_HDR, e->params, e->params_len);
    }

    n = ark_bus_rpc_request_id(bus, worker, e->method, payload,
                               (uint16_t)(TASK_RUN_ID_HDR + e->params_len),
                               task_rpc_cb, e, &e->req_id);
    if (n < 0) {
        e->req_id = 0u;
        return n;
    }
    e->worker_id = worker;
    e->state     = (uint8_t)ARK_BUS_TASK_RUNNING;
    e->submit_ms = ark_bus_tick_ms();
    return n;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

void ark_bus_task_init(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    bus->tasks.bus         = bus;
    bus->tasks.cache_next  = 0u;
    bus->tasks.result_next = 0u;
    bus->tasks.next_task_id = 0u;
}

int ark_bus_task_submit(ark_bus_t *bus, uint16_t method,
                        const void *params, uint16_t len,
                        uint32_t require_caps, uint32_t worker_id,
                        ark_bus_task_cb_t cb, void *user)
{
    ark_bus_task_entry_t *e;

    if (!bus || method == 0u) {
        return ARK_BUS_E_INVAL;
    }
    if (len > (uint16_t)ARK_BUS_TASK_PARAM_MAX) {
        return ARK_BUS_E_INVAL;
    }

    e = entry_free(bus);
    if (!e) {
        return ARK_BUS_E_FULL;
    }

    /* Clear a reused record, keeping only its id slot free for now. */
    ark_memset(e, 0, sizeof(*e));
    e->owner        = bus;
    e->method       = method;
    e->require_caps = require_caps;
    e->worker_id    = worker_id;
    e->params_len   = len;
    e->cb           = cb;
    e->user         = user;
    e->result_slot  = 0xFFu;
    e->state        = (uint8_t)ARK_BUS_TASK_QUEUED;
    if (len && params) {
        ark_memcpy(e->params, params, len);
    }
    e->task_id = task_id_alloc(bus);
    e->used    = 1u;

    /* Try once now; if there is no worker it stays QUEUED and ark_bus_task_tick
     * picks it up as soon as one appears. */
    if (task_dispatch(bus, e) < 0) {
        e->state = (uint8_t)ARK_BUS_TASK_QUEUED;
    }
    return (int)e->task_id;
}

int ark_bus_task_status(const ark_bus_t *bus, uint16_t task_id)
{
    uint8_t i;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (bus->tasks.entries[i].used &&
            bus->tasks.entries[i].task_id == task_id) {
            return (int)bus->tasks.entries[i].state;
        }
    }
    return ARK_BUS_E_NOTFOUND;
}

int ark_bus_task_result(const ark_bus_t *bus, uint16_t task_id,
                        uint8_t *buf, uint16_t cap, uint16_t *len)
{
    const ark_bus_task_entry_t *e = NULL;
    int      slot;
    uint8_t  i;

    if (!bus || (!buf && cap)) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (bus->tasks.entries[i].used &&
            bus->tasks.entries[i].task_id == task_id) {
            e = &bus->tasks.entries[i];
            break;
        }
    }
    if (!e) {
        return ARK_BUS_E_NOTFOUND;
    }
    if (e->state != (uint8_t)ARK_BUS_TASK_DONE) {
        return ARK_BUS_E_AGAIN;
    }
    slot = result_find(bus, task_id);
    if (slot < 0) {
        return ARK_BUS_E_NOTFOUND;   /* result already evicted */
    }
    {
        const ark_bus_task_result_t *r = &bus->tasks.results[slot];
        uint16_t n = r->len < cap ? r->len : cap;
        if (n && buf) {
            ark_memcpy(buf, r->bytes, n);
        }
        if (len) {
            *len = r->len;
        }
    }
    return ARK_BUS_OK;
}

int ark_bus_task_cancel(ark_bus_t *bus, uint16_t task_id)
{
    ark_bus_task_entry_t *e;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    e = entry_find(bus, task_id);
    if (!e) {
        return ARK_BUS_E_NOTFOUND;
    }
    if (e->state == (uint8_t)ARK_BUS_TASK_RUNNING && e->req_id) {
        (void)ark_bus_rpc_cancel(bus, e->req_id);
    }
    e->req_id = 0u;
    e->state  = (uint8_t)ARK_BUS_TASK_CANCELLED;
    if (e->cb) {
        e->cb(e->task_id, ARK_BUS_TASK_CANCELLED, ARK_BUS_OK, e->user);
    }
    return ARK_BUS_OK;
}

uint8_t ark_bus_task_count(const ark_bus_t *bus)
{
    uint8_t i, n = 0;
    if (!bus) {
        return 0;
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        if (bus->tasks.entries[i].used) {
            n++;
        }
    }
    return n;
}

const ark_bus_task_entry_t *ark_bus_task_at(const ark_bus_t *bus, uint8_t index)
{
    if (!bus || index >= ARK_BUS_TASK_MAX) {
        return NULL;
    }
    return &bus->tasks.entries[index];
}

/* ------------------------------------------------------------------ */
/* Completion and retry                                                */
/* ------------------------------------------------------------------ */

static void task_finish(ark_bus_t *bus, ark_bus_task_entry_t *e,
                        int state, int status)
{
    (void)bus;
    e->state = (uint8_t)state;
    if (e->cb) {
        e->cb(e->task_id, state, status, e->user);
    }
}

static void task_rpc_cb(int status, const uint8_t *result,
                        uint16_t len, void *user)
{
    ark_bus_task_entry_t *e = (ark_bus_task_entry_t *)user;
    ark_bus_t            *bus;

    if (!e) {
        return;
    }
    bus = e->owner;
    e->req_id = 0u;

    if (e->state != (uint8_t)ARK_BUS_TASK_RUNNING) {
        return;   /* cancelled while in flight */
    }

    if (status == 0) {
        int slot = result_alloc(bus, e->task_id);
        if (slot >= 0) {
            ark_bus_task_result_t *r = &bus->tasks.results[slot];
            uint16_t n = len;
            if (n > (uint16_t)ARK_BUS_TASK_RESULT_MAX) {
                n = (uint16_t)ARK_BUS_TASK_RESULT_MAX;
            }
            if (n && result) {
                ark_memcpy(r->bytes, result, n);
            }
            r->len         = n;
            e->result_slot = (uint8_t)slot;
        }
        task_finish(bus, e, ARK_BUS_TASK_DONE, 0);
        return;
    }

    /* Timeout with retries left: re-dispatch, possibly to another worker. */
    if (status == ARK_BUS_E_TIMEOUT &&
        e->retries < ARK_BUS_TASK_MAX_RETRY) {
        e->retries++;
        e->worker_id = 0u;             /* tried_mask already holds it */
        if (task_dispatch(bus, e) >= 0) {
            return;
        }
        e->state = (uint8_t)ARK_BUS_TASK_QUEUED;   /* wait for a worker */
        return;
    }

    task_finish(bus, e,
                status == ARK_BUS_E_TIMEOUT ? ARK_BUS_TASK_LOST
                                            : ARK_BUS_TASK_FAILED,
                status);
}

void ark_bus_task_tick(ark_bus_t *bus)
{
    uint8_t i;

    if (!bus) {
        return;
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        ark_bus_task_entry_t *e = &bus->tasks.entries[i];
        if (e->used && e->state == (uint8_t)ARK_BUS_TASK_QUEUED) {
            (void)task_dispatch(bus, e);
        }
    }
}

/*
 * A worker fell off the bus.  Anything it was running cannot be completing
 * normally, so stop waiting for the reply and either re-queue the task (more
 * retries left) or give up on it explicitly as LOST.
 */
void ark_bus_task_worker_offline(ark_bus_t *bus, uint32_t worker_id)
{
    uint8_t i;

    if (!bus) {
        return;
    }
    for (i = 0; i < ARK_BUS_TASK_MAX; i++) {
        ark_bus_task_entry_t *e = &bus->tasks.entries[i];
        if (!e->used || e->state != (uint8_t)ARK_BUS_TASK_RUNNING ||
            e->worker_id != worker_id) {
            continue;
        }
        if (e->req_id) {
            (void)ark_bus_rpc_cancel(bus, e->req_id);
            e->req_id = 0u;
        }
        if (e->retries >= ARK_BUS_TASK_MAX_RETRY) {
            task_finish(bus, e, ARK_BUS_TASK_LOST, ARK_BUS_E_TIMEOUT);
        } else {
            e->retries++;
            e->worker_id = 0u;         /* tried_mask already holds it */
            e->state     = (uint8_t)ARK_BUS_TASK_QUEUED;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Worker side: serve TASK_RUN with an idempotence cache               */
/* ------------------------------------------------------------------ */

static int cache_find(const ark_bus_task_pool_t *ts, uint16_t task_id)
{
    uint8_t i;
    for (i = 0; i < ARK_BUS_TASK_CACHE; i++) {
        if (ts->cache[i].task_id == task_id) {
            return (int)i;
        }
    }
    return -1;
}

static void cache_store(ark_bus_task_pool_t *ts, uint16_t task_id,
                        int status, const uint8_t *result, uint16_t len)
{
    int     slot = cache_find(ts, task_id);
    uint8_t i;

    if (slot < 0) {
        slot = ts->cache_next % ARK_BUS_TASK_CACHE;
        ts->cache_next = (uint8_t)((slot + 1) % ARK_BUS_TASK_CACHE);
    }
    i = (uint8_t)slot;
    ts->cache[i].task_id = task_id;
    ts->cache[i].status  = (int8_t)status;
    ts->cache[i].len     = len > (uint16_t)ARK_BUS_TASK_RESULT_MAX
                               ? (uint16_t)ARK_BUS_TASK_RESULT_MAX : len;
    if (ts->cache[i].len && result) {
        ark_memcpy(ts->cache[i].bytes, result, ts->cache[i].len);
    }
}

static int task_run_trampoline(struct ark_bus *bus, uint32_t from,
                               const uint8_t *params, uint16_t plen,
                               uint8_t *reply, uint16_t rcap,
                               uint16_t *rlen, void *user)
{
    ark_bus_task_pool_t *ts = &bus->tasks;
    uint16_t              task_id;
    const uint8_t        *body;
    uint16_t              blen;
    int                   hit, st;

    (void)from; (void)user;

    if (plen < TASK_RUN_ID_HDR) {
        return ARK_BUS_E_INVAL;
    }
    task_id = ark_rd16(params);
    body    = params + TASK_RUN_ID_HDR;
    blen    = (uint16_t)(plen - TASK_RUN_ID_HDR);

    /* Redelivery: answer from the cache without running the body again. */
    hit = cache_find(ts, task_id);
    if (hit >= 0) {
        uint16_t n = ts->cache[hit].len;
        if (n > rcap) {
            n = rcap;
        }
        if (n) {
            ark_memcpy(reply, ts->cache[hit].bytes, n);
        }
        *rlen = n;
        return (int)ts->cache[hit].status;
    }

    if (!ts->run_handler) {
        return ARK_BUS_E_NOSYS;
    }
    st = ts->run_handler(task_id, body, blen, reply, rcap, rlen, ts->run_user);
    cache_store(ts, task_id, st, reply, *rlen);
    return st;
}

int ark_bus_task_on_run(ark_bus_t *bus, ark_bus_task_fn fn, void *user)
{
    if (!bus || !fn) {
        return ARK_BUS_E_INVAL;
    }
    bus->tasks.run_handler = fn;
    bus->tasks.run_user    = user;
    return ark_bus_rpc_register(bus, ARK_BUS_M_TASK_RUN,
                                task_run_trampoline, NULL);
}

int ark_bus_task_has_run_handler(const ark_bus_t *bus)
{
    return bus && bus->tasks.run_handler != NULL;
}

#endif /* ARK_BUS_TASK_ENABLED */
