/*
 * ark_bus_tool.c - the model-facing tool registry, its policy and the audit
 * trail (the M5 layer).
 *
 * See include/ark_bus_tool.h for the model and the wire format.  In one line:
 * a node publishes named actions with a compact description, a deny-by-default
 * policy decides which may run, high-risk tools are held for a human, and every
 * decision is recorded.  It rides on the RPC layer and adds no allocation.
 *
 * Two conventions worth stating once:
 *   - The RPC layer answers a negative handler return with that status and an
 *     EMPTY body, so a tool handler never returns a negative code: it returns
 *     ARK_BUS_OK and puts the real outcome in reply[0] (the "astatus" byte).
 *     That is how E_DENIED / E_APPROVAL survive the trip.
 *   - The value grammar is explicit on the wire (type, then a big-endian
 *     length, then the bytes), so the reader never type-puns a struct.
 */
#include "ark_bus_priv.h"
#include "ark_bus_protocol.h"
#include "ark_bus_rpc.h"
#include "ark_bus_tool.h"

#if ARK_BUS_TOOL_ENABLED

/* The audit entry's on-wire size (seq + from + tool + astatus + denied). */
#define TOOL_AUDIT_ENTRY_WIRE  12u
/* The audit reply header: seq u32 | next u32 | count u8. */
#define TOOL_AUDIT_HDR_WIRE    9u
/* The list reply header: start u16 | next u16 | count u8. */
#define TOOL_LIST_HDR_WIRE     5u
/* The result-pull request: handle u16 | off u32 | max u8. */
#define TOOL_RESULT_REQ_WIRE   7u
/* The result-pull reply header: astatus i8 | total u32 | n u8. */
#define TOOL_RESULT_HDR_WIRE   6u

/* Reader sentinel: set once malformed input was seen, so r_next is sticky. */
#define TOOL_R_BAD             0xFFu

/* ================================================================== */
/* Value grammar - writer                                             */
/* ================================================================== */

static void w_put(ark_bus_tool_writer_t *w, uint8_t type,
                  const uint8_t *bytes, uint16_t n)
{
    if (!w || w->err) {
        return;
    }
    if ((uint32_t)w->len + 3u + (uint32_t)n > (uint32_t)w->cap) {
        w->err = 1u;
        return;
    }
    w->buf[w->len++] = type;
    ark_wr16(w->buf + w->len, n);
    w->len = (uint16_t)(w->len + 2u);
    if (n != 0u && bytes != NULL) {
        ark_memcpy(w->buf + w->len, bytes, n);
        w->len = (uint16_t)(w->len + n);
    }
    w->count++;
    w->buf[0] = w->count;
}

static void w_scalar(ark_bus_tool_writer_t *w, uint8_t type, uint32_t bits)
{
    uint8_t b[4];

    b[0] = (uint8_t)(bits & 0xFFu);
    b[1] = (uint8_t)((bits >> 8) & 0xFFu);
    b[2] = (uint8_t)((bits >> 16) & 0xFFu);
    b[3] = (uint8_t)((bits >> 24) & 0xFFu);
    w_put(w, type, b, 4u);
}

void ark_bus_tool_w_init(ark_bus_tool_writer_t *w, uint8_t *buf, uint16_t cap)
{
    if (!w) {
        return;
    }
    w->buf   = buf;
    w->cap   = cap;
    w->len   = 0u;
    w->count = 0u;
    w->err   = 0u;
    if (buf == NULL || cap < 1u) {
        w->err = 1u;
        return;
    }
    buf[0] = 0u;   /* the value count, patched in by every put */
    w->len = 1u;
}

void ark_bus_tool_w_i32(ark_bus_tool_writer_t *w, int32_t v)
{
    w_scalar(w, ARK_BUS_TOOL_V_I32, (uint32_t)v);
}

void ark_bus_tool_w_u32(ark_bus_tool_writer_t *w, uint32_t v)
{
    w_scalar(w, ARK_BUS_TOOL_V_U32, v);
}

void ark_bus_tool_w_f32(ark_bus_tool_writer_t *w, float v)
{
    union { float f; uint32_t u; } x;

    x.f = v;
    w_scalar(w, ARK_BUS_TOOL_V_F32, x.u);
}

void ark_bus_tool_w_bool(ark_bus_tool_writer_t *w, int v)
{
    w_scalar(w, ARK_BUS_TOOL_V_BOOL, v ? 1u : 0u);
}

void ark_bus_tool_w_str(ark_bus_tool_writer_t *w, const char *s)
{
    size_t n;

    if (s == NULL) {
        s = "";
    }
    n = ark_strlen(s);
    if (n > 0xFFFFu) {
        if (w) {
            w->err = 1u;
        }
        return;
    }
    w_put(w, ARK_BUS_TOOL_V_STR, (const uint8_t *)s, (uint16_t)n);
}

void ark_bus_tool_w_blob(ark_bus_tool_writer_t *w, const void *p, uint16_t n)
{
    if (n != 0u && p == NULL) {
        if (w) {
            w->err = 1u;
        }
        return;
    }
    w_put(w, ARK_BUS_TOOL_V_BLOB, (const uint8_t *)p, n);
}

int ark_bus_tool_w_end(const ark_bus_tool_writer_t *w, uint16_t *out_len)
{
    if (out_len) {
        *out_len = 0u;
    }
    if (!w || w->err) {
        return ARK_BUS_E_FULL;
    }
    if (out_len) {
        *out_len = w->len;
    }
    return (int)w->len;
}

/* ================================================================== */
/* Value grammar - reader                                             */
/* ================================================================== */

void ark_bus_tool_r_init(ark_bus_tool_reader_t *r, const uint8_t *buf,
                         uint16_t len)
{
    if (!r) {
        return;
    }
    r->buf   = buf;
    r->len   = len;
    r->off   = 0u;
    r->count = 0u;
    r->idx   = TOOL_R_BAD;
    if (buf != NULL && len >= 1u) {
        r->count = buf[0];
        r->off   = 1u;
        r->idx   = 0u;
    }
}

int ark_bus_tool_r_next(ark_bus_tool_reader_t *r, uint8_t *type,
                        const uint8_t **val, uint16_t *vlen)
{
    uint16_t n;

    if (!r) {
        return ARK_BUS_E_INVAL;
    }
    if (r->idx == TOOL_R_BAD) {
        return ARK_BUS_E_FORMAT;
    }
    if (r->idx >= r->count) {
        return ARK_BUS_E_NOTFOUND;
    }
    if ((uint32_t)r->off + 3u > (uint32_t)r->len) {
        r->idx = TOOL_R_BAD;
        return ARK_BUS_E_FORMAT;
    }
    n = ark_rd16(r->buf + r->off + 1u);
    if ((uint32_t)r->off + 3u + (uint32_t)n > (uint32_t)r->len) {
        r->idx = TOOL_R_BAD;
        return ARK_BUS_E_FORMAT;
    }
    if (type) {
        *type = r->buf[r->off];
    }
    if (val) {
        *val = r->buf + r->off + 3u;
    }
    if (vlen) {
        *vlen = n;
    }
    r->off = (uint16_t)(r->off + 3u + n);
    r->idx++;
    return ARK_BUS_OK;
}

static int r_scalar(ark_bus_tool_reader_t *r, uint8_t want, uint32_t *out)
{
    uint8_t        t;
    const uint8_t *v;
    uint16_t       n;
    int            rc;

    if (!out) {
        return ARK_BUS_E_INVAL;
    }
    rc = ark_bus_tool_r_next(r, &t, &v, &n);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    if (t != want || n != 4u) {
        return ARK_BUS_E_FORMAT;
    }
    *out = (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
           ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
    return ARK_BUS_OK;
}

int ark_bus_tool_r_i32(ark_bus_tool_reader_t *r, int32_t *out)
{
    uint32_t u = 0u;
    int      rc;

    if (!out) {
        return ARK_BUS_E_INVAL;
    }
    rc = r_scalar(r, ARK_BUS_TOOL_V_I32, &u);
    if (rc == ARK_BUS_OK) {
        *out = (int32_t)u;
    }
    return rc;
}

int ark_bus_tool_r_u32(ark_bus_tool_reader_t *r, uint32_t *out)
{
    return r_scalar(r, ARK_BUS_TOOL_V_U32, out);
}

int ark_bus_tool_r_f32(ark_bus_tool_reader_t *r, float *out)
{
    union { float f; uint32_t u; } x;
    int      rc;
    uint32_t u = 0u;

    if (!out) {
        return ARK_BUS_E_INVAL;
    }
    rc = r_scalar(r, ARK_BUS_TOOL_V_F32, &u);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    x.u = u;
    *out = x.f;
    return ARK_BUS_OK;
}

int ark_bus_tool_r_bool(ark_bus_tool_reader_t *r, int *out)
{
    uint32_t u = 0u;
    int      rc;

    if (!out) {
        return ARK_BUS_E_INVAL;
    }
    rc = r_scalar(r, ARK_BUS_TOOL_V_BOOL, &u);
    if (rc == ARK_BUS_OK) {
        *out = (u != 0u) ? 1 : 0;
    }
    return rc;
}

int ark_bus_tool_r_raw(ark_bus_tool_reader_t *r, const uint8_t **p, uint16_t *n)
{
    uint8_t        t;
    const uint8_t *v;
    uint16_t       vn;
    int            rc;

    rc = ark_bus_tool_r_next(r, &t, &v, &vn);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    if (t != ARK_BUS_TOOL_V_STR && t != ARK_BUS_TOOL_V_BLOB) {
        return ARK_BUS_E_FORMAT;
    }
    if (p) {
        *p = v;
    }
    if (n) {
        *n = vn;
    }
    return ARK_BUS_OK;
}

/* ================================================================== */
/* Signature grammar                                                   */
/* ================================================================== */

static int sig_name_ok(const char *p, size_t n)
{
    size_t i;

    if (n == 0u) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) {
            return 0;
        }
    }
    return 1;
}

static int sig_type_ok(const char *p, size_t n)
{
    if (n == 3u && p[0] == 'i' && p[1] == '3' && p[2] == '2') return 1;
    if (n == 3u && p[0] == 'u' && p[1] == '3' && p[2] == '2') return 1;
    if (n == 3u && p[0] == 'f' && p[1] == '3' && p[2] == '2') return 1;
    if (n == 3u && p[0] == 's' && p[1] == 't' && p[2] == 'r') return 1;
    if (n == 4u && p[0] == 'b' && p[1] == 'o' && p[2] == 'o' && p[3] == 'l')
        return 1;
    if (n == 4u && p[0] == 'b' && p[1] == 'l' && p[2] == 'o' && p[3] == 'b')
        return 1;
    return 0;
}

int ark_bus_tool_sig_valid(const char *sig)
{
    if (!sig) {
        return 0;
    }
    if (*sig == '\0') {
        return 1;   /* no parameters */
    }
    for (;;) {
        const char *colon = sig;
        const char *typ;
        const char *end;

        while (*colon != '\0' && *colon != ':' && *colon != ',') {
            colon++;
        }
        if (*colon != ':') {
            return 0;   /* missing ':' before the type */
        }
        if (!sig_name_ok(sig, (size_t)(colon - sig))) {
            return 0;
        }
        typ = colon + 1;
        end = typ;
        while (*end != '\0' && *end != ',') {
            end++;
        }
        if (!sig_type_ok(typ, (size_t)(end - typ))) {
            return 0;
        }
        if (*end == '\0') {
            return 1;
        }
        sig = end + 1;          /* skip ',' */
        if (*sig == '\0') {
            return 0;           /* trailing comma */
        }
    }
}

/* ================================================================== */
/* Registry                                                            */
/* ================================================================== */

uint8_t ark_bus_tool_count(const struct ark_bus *bus)
{
    return bus ? bus->tools.count : 0u;
}

static ark_bus_tool_entry_t *entry_find(struct ark_bus *bus, uint16_t id)
{
    uint8_t i;

    for (i = 0; i < bus->tools.count; i++) {
        if (bus->tools.entries[i].id == id) {
            return &bus->tools.entries[i];
        }
    }
    return (ark_bus_tool_entry_t *)0;
}

int ark_bus_tool_register(struct ark_bus *bus, uint16_t id, const char *name,
                          const char *summary, const char *params,
                          uint16_t flags, ark_bus_tool_fn fn, void *user)
{
    ark_bus_tool_state_t *t;
    ark_bus_tool_entry_t *e;

    if (!bus || !name || !summary || !params || !fn) {
        return ARK_BUS_E_INVAL;
    }
    if (id >= ARK_BUS_TOOL_MAX_TOOLS) {
        return ARK_BUS_E_INVAL;
    }
    if (ark_strlen(name) == 0u || ark_strlen(name) > ARK_BUS_TOOL_NAME_MAX) {
        return ARK_BUS_E_INVAL;
    }
    /* The wire carries each string length in one byte, plus a NUL. */
    if (ark_strlen(summary) > 255u || ark_strlen(params) > 255u) {
        return ARK_BUS_E_INVAL;
    }
    if (!ark_bus_tool_sig_valid(params)) {
        return ARK_BUS_E_INVAL;
    }

    t = &bus->tools;
    e = entry_find(bus, id);
    if (e == (ark_bus_tool_entry_t *)0) {
        if (t->count >= ARK_BUS_TOOL_MAX_TOOLS) {
            return ARK_BUS_E_FULL;
        }
        e = &t->entries[t->count++];
    }
    e->id      = id;
    e->flags   = flags;
    e->name    = name;
    e->summary = summary;
    e->params  = params;
    e->fn      = fn;
    e->user    = user;
    return ARK_BUS_OK;
}

int ark_bus_tool_desc_at(const struct ark_bus *bus, uint8_t i,
                         ark_bus_tool_desc_t *out)
{
    const ark_bus_tool_entry_t *e;

    if (!bus || !out || i >= bus->tools.count) {
        return ARK_BUS_E_NOTFOUND;
    }
    e = &bus->tools.entries[i];
    out->id      = e->id;
    out->flags   = e->flags;
    out->name    = e->name;
    out->summary = e->summary;
    out->params  = e->params;
    return ARK_BUS_OK;
}

/* ================================================================== */
/* Policy                                                              */
/* ================================================================== */

int ark_bus_tool_set_policy(struct ark_bus *bus,
                            const ark_bus_tool_policy_t *policy)
{
    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    if (!policy) {
        ark_memset(&bus->tools.policy, 0, sizeof(bus->tools.policy));
        bus->tools.has_policy    = 0u;
        bus->tools.window_start_ms = 0u;
        bus->tools.window_calls    = 0u;
        return ARK_BUS_OK;
    }
    bus->tools.policy = *policy;
    bus->tools.has_policy = 1u;
    /* A new policy starts a fresh rate-limit window. */
    bus->tools.window_start_ms = 0u;
    bus->tools.window_calls    = 0u;
    return ARK_BUS_OK;
}

int ark_bus_tool_get_policy(const struct ark_bus *bus,
                            ark_bus_tool_policy_t *out)
{
    if (!bus || !out) {
        return ARK_BUS_E_INVAL;
    }
    if (!bus->tools.has_policy) {
        return ARK_BUS_E_NOTFOUND;
    }
    *out = bus->tools.policy;
    return ARK_BUS_OK;
}

/* One caller's override slot, or NULL. */
static ark_bus_tool_caller_t *caller_find(struct ark_bus *bus, uint32_t caller)
{
    uint8_t i;

    for (i = 0u; i < ARK_BUS_TOOL_MAX_CALLERS; i++) {
        if (bus->tools.callers[i].used && bus->tools.callers[i].caller == caller) {
            return &bus->tools.callers[i];
        }
    }
    return (ark_bus_tool_caller_t *)0;
}

int ark_bus_tool_set_caller_policy(struct ark_bus *bus, uint32_t caller,
                                   const ark_bus_tool_policy_t *policy)
{
    ark_bus_tool_caller_t *c;
    uint8_t i;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    c = caller_find(bus, caller);
    if (!policy) {
        if (c) {
            c->used = 0u;   /* an empty slot is the fallback again */
        }
        return ARK_BUS_OK;
    }
    if (!c) {
        for (i = 0u; i < ARK_BUS_TOOL_MAX_CALLERS; i++) {
            if (!bus->tools.callers[i].used) {
                c = &bus->tools.callers[i];
                break;
            }
        }
        if (!c) {
            return ARK_BUS_E_FULL;
        }
        c->used = 1u;
        c->caller = caller;
    }
    c->policy = *policy;
    c->window_start_ms = 0u;   /* a new policy starts a fresh window */
    c->window_calls    = 0u;
    return ARK_BUS_OK;
}

int ark_bus_tool_get_caller_policy(const struct ark_bus *bus, uint32_t caller,
                                   ark_bus_tool_policy_t *out)
{
    const ark_bus_tool_caller_t *c;
    uint8_t i;

    if (!bus || !out) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0u; i < ARK_BUS_TOOL_MAX_CALLERS; i++) {
        c = &bus->tools.callers[i];
        if (c->used && c->caller == caller) {
            *out = c->policy;
            return ARK_BUS_OK;
        }
    }
    return ARK_BUS_E_NOTFOUND;
}

/*
 * The effective policy for one caller: its own override if it has one, else
 * the bus-wide policy, else none (deny-by-default).  `*win_start`/`*win_calls`
 * point at the window this policy rates against, so a caller's budget is its
 * own.
 */
static const ark_bus_tool_policy_t *select_policy(struct ark_bus *bus,
                                                  uint32_t from,
                                                  uint32_t **win_start,
                                                  uint16_t **win_calls)
{
    ark_bus_tool_caller_t *c = caller_find(bus, from);

    if (c) {
        *win_start = &c->window_start_ms;
        *win_calls = &c->window_calls;
        return &c->policy;
    }
    if (bus->tools.has_policy) {
        *win_start = &bus->tools.window_start_ms;
        *win_calls = &bus->tools.window_calls;
        return &bus->tools.policy;
    }
    *win_start = (uint32_t *)0;
    *win_calls = (uint16_t *)0;
    return (const ark_bus_tool_policy_t *)0;
}

int ark_bus_tool_set_approvers(struct ark_bus *bus, const uint32_t *ids,
                               uint8_t count)
{
    uint8_t i;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    if (count > ARK_BUS_TOOL_MAX_APPROVERS) {
        return ARK_BUS_E_INVAL;
    }
    if (count > 0u && ids == (const uint32_t *)0) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0u; i < count; i++) {
        bus->tools.approvers[i] = ids[i];
    }
    bus->tools.approver_count = count;
    return ARK_BUS_OK;
}

/* Is `from` allowed to approve?  Empty list means anyone (the default). */
static int caller_may_approve(const struct ark_bus *bus, uint32_t from)
{
    uint8_t i;

    if (bus->tools.approver_count == 0u) {
        return 1;
    }
    for (i = 0u; i < bus->tools.approver_count; i++) {
        if (bus->tools.approvers[i] == from) {
            return 1;
        }
    }
    return 0;
}

/* ================================================================== */
/* Audit and counters                                                  */
/* ================================================================== */

/* Entries currently in the ring (capped by its capacity). */
static uint32_t audit_total(const struct ark_bus *bus)
{
    uint32_t n = bus->tools.next_seq - 1u;   /* calls recorded so far */

    if (n > (uint32_t)ARK_BUS_TOOL_AUDIT_LEN) {
        n = (uint32_t)ARK_BUS_TOOL_AUDIT_LEN;
    }
    return n;
}

static void audit_record(struct ark_bus *bus, uint32_t from, uint16_t tool,
                         int status, uint8_t denied)
{
    ark_bus_tool_state_t *t = &bus->tools;
    ark_bus_tool_audit_t *a = &t->audit[t->audit_head];

    a->seq    = t->next_seq++;
    a->from   = from;
    a->tool   = tool;
    a->status = (int8_t)status;
    a->denied = denied;
    t->audit_head = (uint16_t)((t->audit_head + 1u) % ARK_BUS_TOOL_AUDIT_LEN);
}

void ark_bus_tool_get_stats(const struct ark_bus *bus,
                            ark_bus_tool_stats_t *out)
{
    if (!bus || !out) {
        return;
    }
    *out = bus->tools.stats;
}

int ark_bus_tool_audit(const struct ark_bus *bus, uint16_t newest,
                       ark_bus_tool_audit_t *out)
{
    uint32_t total;
    uint32_t idx;

    if (!bus || !out) {
        return ARK_BUS_E_INVAL;
    }
    total = audit_total(bus);
    if ((uint32_t)newest >= total) {
        return ARK_BUS_E_NOTFOUND;
    }
    idx = ((uint32_t)bus->tools.audit_head + (uint32_t)ARK_BUS_TOOL_AUDIT_LEN
           - 1u - (uint32_t)newest) % (uint32_t)ARK_BUS_TOOL_AUDIT_LEN;
    *out = bus->tools.audit[idx];
    return ARK_BUS_OK;
}

/* ================================================================== */
/* Server handlers                                                     */
/* ================================================================== */

/* Find a live pending entry; expire stale ones as we scan. */
static ark_bus_tool_pending_t *pending_find(struct ark_bus *bus, uint16_t token)
{
    ark_bus_tool_state_t *t = &bus->tools;
    uint32_t now = ark_bus_tick_ms();
    int      i;

    for (i = 0; i < ARK_BUS_TOOL_PENDING_MAX; i++) {
        ark_bus_tool_pending_t *p = &t->pending[i];
        if (!p->used) {
            continue;
        }
        if ((int32_t)(now - p->expires_ms) >= 0) {
            p->used = 0u;
            t->stats.expired++;
            continue;
        }
        if (p->token == token) {
            return p;
        }
    }
    return (ark_bus_tool_pending_t *)0;
}

/* TOOL_LIST: start u16 -> start u16 | next u16 | count u8 | entry[count] */
/*
 * Where a handler should write its result.  With a staging buffer (M5.3) it
 * writes there, so a result larger than the reply frame can still be produced;
 * the dispatch path then either copies it inline or stages it.  Without one it
 * writes straight into the reply, exactly as before.
 */
static uint8_t *result_buf(struct ark_bus *bus, uint8_t *reply, uint16_t rcap,
                           uint16_t *cap)
{
#if ARK_BUS_TOOL_STAGE_MAX > 0
    (void)reply;
    (void)rcap;
    *cap = (uint16_t)ARK_BUS_TOOL_STAGE_MAX;
    return bus->tools.stage;
#else
    (void)bus;
    *cap = (uint16_t)(rcap - 1u);
    return reply + 1u;
#endif
}

/*
 * Turn a handler's outcome into the reply.  A result that fits the frame goes
 * inline; one that does not is staged (when staging is built in) and the reply
 * carries the total length and a handle instead of the bytes.  Returns
 * ARK_BUS_OK; the caller owns the stats and the audit record.
 */
static int result_finish(struct ark_bus *bus, uint32_t from, uint8_t *reply,
                         uint16_t rcap, uint16_t *rlen,
                         const uint8_t *out, uint16_t olen)
{
#if ARK_BUS_TOOL_STAGE_MAX > 0
    if (olen > (uint16_t)(rcap - 1u)) {
        ark_bus_tool_state_t *t = &bus->tools;

        t->stage_used  = 1u;
        t->stage_len   = olen;
        t->stage_owner = from;
        t->next_token++;
        if (t->next_token == 0u) {
            t->next_token = 1u;   /* 0 is never a live token */
        }
        t->stage_handle = t->next_token;
        reply[0] = (uint8_t)ARK_BUS_TOOL_STAGED;
        ark_wr32(reply + 1u, (uint32_t)olen);
        ark_wr16(reply + 5u, t->stage_handle);
        *rlen = 7u;
        return ARK_BUS_OK;
    }
#else
    (void)bus;
    (void)from;
    (void)rcap;   /* without staging the result always goes inline */
#endif
    if (olen) {
        ark_memcpy(reply + 1u, out, olen);
    }
    reply[0] = 0u;
    *rlen = (uint16_t)(1u + olen);
    return ARK_BUS_OK;
}

static int tool_list_handler(struct ark_bus *bus, uint32_t from,
                             const uint8_t *params, uint16_t plen,
                             uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                             void *user)
{
    uint16_t start;
    uint16_t off;
    uint8_t  count = 0u;
    uint8_t  i;
    uint8_t  total;

    (void)from;
    (void)user;

    if (!bus || !reply || !rlen || !params || plen < 2u) {
        return ARK_BUS_E_INVAL;
    }
    if (rcap < TOOL_LIST_HDR_WIRE) {
        return ARK_BUS_E_FULL;
    }
    start = ark_rd16(params);
    total = bus->tools.count;

    ark_wr16(reply, start);
    off = TOOL_LIST_HDR_WIRE;

    i = (start > (uint16_t)total) ? total : (uint8_t)start;
    while (i < total) {
        const ark_bus_tool_entry_t *e = &bus->tools.entries[i];
        uint16_t nl = (uint16_t)ark_strlen(e->name);
        uint16_t sl = (uint16_t)ark_strlen(e->summary);
        uint16_t pl = (uint16_t)ark_strlen(e->params);
        uint32_t need = 2u + 2u + 1u + (uint32_t)nl + 1u
                      + 1u + (uint32_t)sl + 1u
                      + 1u + (uint32_t)pl + 1u;

        if ((uint32_t)off + need > (uint32_t)rcap) {
            if (count == 0u) {
                return ARK_BUS_E_FULL;   /* cannot make progress */
            }
            break;
        }
        ark_wr16(reply + off, e->id);
        off = (uint16_t)(off + 2u);
        ark_wr16(reply + off, e->flags);
        off = (uint16_t)(off + 2u);
        reply[off++] = (uint8_t)nl;
        if (nl) {
            ark_memcpy(reply + off, e->name, nl);
            off = (uint16_t)(off + nl);
        }
        reply[off++] = 0u;
        reply[off++] = (uint8_t)sl;
        if (sl) {
            ark_memcpy(reply + off, e->summary, sl);
            off = (uint16_t)(off + sl);
        }
        reply[off++] = 0u;
        reply[off++] = (uint8_t)pl;
        if (pl) {
            ark_memcpy(reply + off, e->params, pl);
            off = (uint16_t)(off + pl);
        }
        reply[off++] = 0u;
        count++;
        i++;
    }

    ark_wr16(reply + 2u, (i >= total) ? 0xFFFFu : (uint16_t)i);
    reply[4] = count;
    *rlen = off;
    return ARK_BUS_OK;
}

/* Cache a call and answer E_APPROVAL with its token. */
static int tool_hold(struct ark_bus *bus, const ark_bus_tool_entry_t *e,
                     uint32_t from, const uint8_t *args, uint16_t alen,
                     uint8_t *reply, uint16_t rcap, uint16_t *rlen)
{
    ark_bus_tool_state_t *t = &bus->tools;
    ark_bus_tool_pending_t *p;
    int i;

    if (alen > ARK_BUS_TOOL_ARG_MAX || (alen != 0u && args == NULL)) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        t->stats.failed++;
        audit_record(bus, from, e->id, ARK_BUS_E_INVAL, 0u);
        return ARK_BUS_OK;
    }
    for (i = 0; i < ARK_BUS_TOOL_PENDING_MAX; i++) {
        if (!t->pending[i].used) {
            break;
        }
    }
    if (i >= ARK_BUS_TOOL_PENDING_MAX || rcap < 3u) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_FULL;
        *rlen = 1u;
        t->stats.failed++;
        audit_record(bus, from, e->id, ARK_BUS_E_FULL, 0u);
        return ARK_BUS_OK;
    }

    p = &t->pending[i];
    t->next_token++;
    if (t->next_token == 0u) {
        t->next_token = 1u;   /* 0 is never a live token */
    }
    p->used       = 1u;
    p->token      = t->next_token;
    p->tool       = e->id;
    p->alen       = alen;
    p->from       = from;
    p->expires_ms = ark_bus_tick_ms() + ARK_BUS_TOOL_APPROVAL_TTL_MS;
    if (alen) {
        ark_memcpy(p->args, args, alen);
    }

    t->stats.held++;
    audit_record(bus, from, e->id, ARK_BUS_E_APPROVAL, 0u);

    reply[0] = (uint8_t)(int8_t)ARK_BUS_E_APPROVAL;
    ark_wr16(reply + 1u, p->token);
    *rlen = 3u;
    return ARK_BUS_OK;
}

/* TOOL_CALL: id u16 | args -> astatus i8 | payload */
static int tool_call_handler(struct ark_bus *bus, uint32_t from,
                             const uint8_t *params, uint16_t plen,
                             uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                             void *user)
{
    ark_bus_tool_state_t *t;
    ark_bus_tool_entry_t  *e;
    const ark_bus_tool_policy_t *pol;
    const uint8_t *args;
    uint32_t      *win_start;
    uint16_t      *win_calls;
    uint8_t       *out;
    uint16_t       id;
    uint16_t       alen;
    uint16_t       olen = 0u;
    uint16_t       ocap = 0u;
    uint32_t       now;
    int            rc;

    (void)user;

    if (!bus || !reply || !rlen || rcap < 1u) {
        return ARK_BUS_E_INVAL;
    }
    t = &bus->tools;
    t->stats.calls++;

    if (!params || plen < 2u) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    id   = ark_rd16(params);
    args = params + 2u;
    alen = (uint16_t)(plen - 2u);

    /* An id no tool can have is not a policy question: it is not found.  This
     * also keeps the 1u << id below inside the mask's width. */
    if (id >= ARK_BUS_TOOL_MAX_TOOLS) {
        audit_record(bus, from, id, ARK_BUS_E_NOTFOUND, 0u);
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_NOTFOUND;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    /* --- the policy is deny-by-default, per caller when one is set --- */
    pol = select_policy(bus, from, &win_start, &win_calls);
    if (pol == (const ark_bus_tool_policy_t *)0 ||
        (pol->allow_mask & (1u << id)) == 0u) {
        t->stats.denied++;
        audit_record(bus, from, id, ARK_BUS_E_DENIED, 1u);
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_DENIED;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    e = entry_find(bus, id);
    if (e == (ark_bus_tool_entry_t *)0) {
        audit_record(bus, from, id, ARK_BUS_E_NOTFOUND, 0u);
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_NOTFOUND;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    /* --- rate limit over a fixed window (this caller's own) --- */
    now = ark_bus_tick_ms();
    if (pol->max_calls != 0u) {
        if (*win_calls == 0u ||
            (uint32_t)(now - *win_start) >= pol->window_ms) {
            *win_start = now;
            *win_calls = 0u;
        }
        if (*win_calls >= pol->max_calls) {
            t->stats.denied++;
            audit_record(bus, from, id, ARK_BUS_E_DENIED, 1u);
            reply[0] = (uint8_t)(int8_t)ARK_BUS_E_DENIED;
            *rlen = 1u;
            return ARK_BUS_OK;
        }
        (*win_calls)++;
    }

    /* --- high-risk tools wait for a human --- */
    if ((e->flags & ARK_BUS_TOOL_APPROVAL) != 0u ||
        (pol->approval_mask & (1u << id)) != 0u) {
        return tool_hold(bus, e, from, args, alen, reply, rcap, rlen);
    }

    out = result_buf(bus, reply, rcap, &ocap);
    rc = e->fn(bus, from, args, alen, out, ocap, &olen, e->user);
    if (rc == ARK_BUS_OK) {
        t->stats.ok++;
        rc = result_finish(bus, from, reply, rcap, rlen, out, olen);
    } else {
        reply[0] = (uint8_t)(int8_t)rc;
        *rlen = 1u;
        t->stats.failed++;
    }
    audit_record(bus, from, id, rc, 0u);
    return ARK_BUS_OK;
}

/* TOOL_APPROVE: token u16 | decision u8 -> astatus i8 | payload */
static int tool_approve_handler(struct ark_bus *bus, uint32_t from,
                                const uint8_t *params, uint16_t plen,
                                uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                                void *user)
{
    ark_bus_tool_state_t   *t;
    ark_bus_tool_pending_t *p;
    ark_bus_tool_entry_t   *e;
    uint16_t token;
    uint8_t  decision;
    uint8_t *out;
    uint16_t ocap = 0u;
    uint16_t olen = 0u;
    uint32_t held_from;
    uint16_t held_tool;
    int      rc;

    (void)user;

    if (!bus || !reply || !rlen || rcap < 1u) {
        return ARK_BUS_E_INVAL;
    }
    t = &bus->tools;
    if (!params || plen < 3u) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    token    = ark_rd16(params);
    decision = params[2];

    /* Who may approve (M5.3).  Checked before the token is looked up, so a
     * non-approver cannot probe which tokens exist. */
    if (!caller_may_approve(bus, from)) {
        t->stats.denied++;
        audit_record(bus, from, 0xFFFFu, ARK_BUS_E_DENIED, 1u);
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_DENIED;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    p = pending_find(bus, token);
    if (p == (ark_bus_tool_pending_t *)0) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_NOTFOUND;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    e = entry_find(bus, p->tool);
    held_from = p->from;
    held_tool = p->tool;
    if (e == (ark_bus_tool_entry_t *)0) {
        p->used = 0u;
        audit_record(bus, held_from, held_tool, ARK_BUS_E_NOTFOUND, 0u);
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_NOTFOUND;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    if (decision == 0u) {
        p->used = 0u;
        t->stats.rejected++;
        audit_record(bus, held_from, held_tool, ARK_BUS_E_DENIED, 1u);
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_DENIED;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    out = result_buf(bus, reply, rcap, &ocap);
    rc = e->fn(bus, held_from, p->args, p->alen, out, ocap, &olen, e->user);
    p->used = 0u;
    t->stats.approved++;
    if (rc == ARK_BUS_OK) {
        t->stats.ok++;
        rc = result_finish(bus, from, reply, rcap, rlen, out, olen);
    } else {
        reply[0] = (uint8_t)(int8_t)rc;
        *rlen = 1u;
        t->stats.failed++;
    }
    audit_record(bus, held_from, held_tool, rc, 0u);
    return ARK_BUS_OK;
}

/* TOOL_AUDIT: max u8 -> seq u32 | next u32 | count u8 | entry[count] */
static int tool_audit_handler(struct ark_bus *bus, uint32_t from,
                              const uint8_t *params, uint16_t plen,
                              uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                              void *user)
{
    uint32_t total;
    uint32_t want;
    uint32_t i;
    uint16_t off;
    uint8_t  count = 0u;
    uint8_t  max;

    (void)from;
    (void)user;

    if (!bus || !reply || !rlen || !params || plen < 1u) {
        return ARK_BUS_E_INVAL;
    }
    if (rcap < TOOL_AUDIT_HDR_WIRE) {
        return ARK_BUS_E_FULL;
    }
    max   = params[0];
    total = audit_total(bus);
    want  = ((uint32_t)max < total) ? (uint32_t)max : total;

    off = TOOL_AUDIT_HDR_WIRE;
    for (i = 0u; i < want; i++) {
        uint32_t idx;
        const ark_bus_tool_audit_t *a;

        if ((uint32_t)off + TOOL_AUDIT_ENTRY_WIRE > (uint32_t)rcap) {
            break;
        }
        idx = ((uint32_t)bus->tools.audit_head + (uint32_t)ARK_BUS_TOOL_AUDIT_LEN
               - 1u - i) % (uint32_t)ARK_BUS_TOOL_AUDIT_LEN;
        a = &bus->tools.audit[idx];
        ark_wr32(reply + off, a->seq);
        off = (uint16_t)(off + 4u);
        ark_wr32(reply + off, a->from);
        off = (uint16_t)(off + 4u);
        ark_wr16(reply + off, a->tool);
        off = (uint16_t)(off + 2u);
        reply[off++] = (uint8_t)a->status;
        reply[off++] = a->denied;
        count++;
    }

    /* seq = the latest recorded call (0 when none); next = older entries the
     * caller did not receive in this reply, so a host can see a backlog. */
    ark_wr32(reply, bus->tools.next_seq - 1u);
    ark_wr32(reply + 4u, total - count);
    reply[8] = count;
    *rlen = off;
    return ARK_BUS_OK;
}

#if ARK_BUS_TOOL_STAGE_MAX > 0
/* TOOL_RESULT: handle u16 | off u32 | max u8 -> astatus i8 | total u32 | n u8 | bytes */
static int tool_result_handler(struct ark_bus *bus, uint32_t from,
                               const uint8_t *params, uint16_t plen,
                               uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                               void *user)
{
    ark_bus_tool_state_t *t;
    uint16_t handle;
    uint32_t off;
    uint8_t  max;
    uint32_t avail;
    uint32_t n;

    (void)user;

    if (!bus || !reply || !rlen || rcap < 1u) {
        return ARK_BUS_E_INVAL;
    }
    t = &bus->tools;
    if (!params || plen < TOOL_RESULT_REQ_WIRE) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    if (rcap < TOOL_RESULT_HDR_WIRE) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_FULL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    handle = ark_rd16(params);
    off    = ark_rd32(params + 2u);
    max    = params[6];

    /* A handle is only good while its result is staged, and only its owner
     * (the caller the result belongs to) may read it. */
    if (!t->stage_used || handle != t->stage_handle) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_NOTFOUND;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    if (from != t->stage_owner) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_DENIED;
        *rlen = 1u;
        return ARK_BUS_OK;
    }
    if (off > (uint32_t)t->stage_len) {
        reply[0] = (uint8_t)(int8_t)ARK_BUS_E_INVAL;
        *rlen = 1u;
        return ARK_BUS_OK;
    }

    avail = (uint32_t)t->stage_len - off;
    n = ((uint32_t)max < avail) ? (uint32_t)max : avail;
    if (n > (uint32_t)(rcap - TOOL_RESULT_HDR_WIRE)) {
        n = (uint32_t)(rcap - TOOL_RESULT_HDR_WIRE);
    }
    reply[0] = 0u;
    ark_wr32(reply + 1u, (uint32_t)t->stage_len);
    reply[5] = (uint8_t)n;
    if (n) {
        ark_memcpy(reply + TOOL_RESULT_HDR_WIRE, t->stage + off, (uint16_t)n);
    }
    if (off + n >= (uint32_t)t->stage_len) {
        t->stage_used = 0u;   /* the whole result has been delivered */
    }
    *rlen = (uint16_t)(TOOL_RESULT_HDR_WIRE + n);
    return ARK_BUS_OK;
}
#endif /* ARK_BUS_TOOL_STAGE_MAX > 0 */

int ark_bus_tool_enable(struct ark_bus *bus)
{
    int rc;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    rc = ark_bus_rpc_register(bus, ARK_BUS_M_TOOL_LIST, tool_list_handler,
                              (void *)0);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_rpc_register(bus, ARK_BUS_M_TOOL_CALL, tool_call_handler,
                              (void *)0);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_rpc_register(bus, ARK_BUS_M_TOOL_APPROVE, tool_approve_handler,
                              (void *)0);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_rpc_register(bus, ARK_BUS_M_TOOL_AUDIT, tool_audit_handler,
                              (void *)0);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
#if ARK_BUS_TOOL_STAGE_MAX > 0
    rc = ark_bus_rpc_register(bus, ARK_BUS_M_TOOL_RESULT, tool_result_handler,
                              (void *)0);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
#endif
    bus->tools.enabled = 1u;
    return ARK_BUS_OK;
}

int ark_bus_tool_release(struct ark_bus *bus, uint16_t token, int approve)
{
    ark_bus_tool_state_t   *t;
    ark_bus_tool_pending_t *p;
    ark_bus_tool_entry_t   *e;
    uint8_t  result[ARK_BUS_TOOL_RESULT_MAX];
    uint16_t olen = 0u;
    uint32_t held_from;
    uint16_t held_tool;
    int      rc;

    if (!bus) {
        return ARK_BUS_E_INVAL;
    }
    t = &bus->tools;
    p = pending_find(bus, token);
    if (p == (ark_bus_tool_pending_t *)0) {
        return ARK_BUS_E_NOTFOUND;
    }
    e = entry_find(bus, p->tool);
    held_from = p->from;
    held_tool = p->tool;

    if (!approve) {
        p->used = 0u;
        t->stats.rejected++;
        audit_record(bus, held_from, held_tool, ARK_BUS_E_DENIED, 1u);
        return ARK_BUS_OK;
    }
    if (e == (ark_bus_tool_entry_t *)0) {
        p->used = 0u;
        audit_record(bus, held_from, held_tool, ARK_BUS_E_NOTFOUND, 0u);
        return ARK_BUS_E_NOTFOUND;
    }

    rc = e->fn(bus, held_from, p->args, p->alen, result,
               (uint16_t)sizeof(result), &olen, e->user);
    p->used = 0u;
    t->stats.approved++;
    if (rc == ARK_BUS_OK) {
        t->stats.ok++;
    } else {
        t->stats.failed++;
    }
    audit_record(bus, held_from, held_tool, rc, 0u);
    return (rc == ARK_BUS_OK) ? ARK_BUS_OK : rc;
}

/* ================================================================== */
/* Client side                                                         */
/* ================================================================== */

int ark_bus_tool_remote_list(struct ark_bus *bus, uint32_t dst, uint16_t start,
                             uint8_t *reply, uint16_t rcap, uint16_t *rlen)
{
    uint8_t p[2];

    if (!bus || !reply || !rlen) {
        return ARK_BUS_E_INVAL;
    }
    ark_wr16(p, start);
    return ark_bus_rpc_call(bus, dst, ARK_BUS_M_TOOL_LIST, p, 2u,
                            reply, rcap, rlen);
}

int ark_bus_tool_remote_call(struct ark_bus *bus, uint32_t dst, uint16_t id,
                             const uint8_t *args, uint16_t alen,
                             uint8_t *reply, uint16_t rcap, uint16_t *rlen)
{
    uint8_t p[2u + ARK_BUS_TOOL_ARG_MAX];

    if (!bus || !reply || !rlen) {
        return ARK_BUS_E_INVAL;
    }
    if (alen > ARK_BUS_TOOL_ARG_MAX || (alen != 0u && args == NULL)) {
        return ARK_BUS_E_INVAL;
    }
    ark_wr16(p, id);
    if (alen) {
        ark_memcpy(p + 2u, args, alen);
    }
    return ark_bus_rpc_call(bus, dst, ARK_BUS_M_TOOL_CALL, p,
                            (uint16_t)(2u + alen), reply, rcap, rlen);
}

int ark_bus_tool_remote_approve(struct ark_bus *bus, uint32_t dst,
                                uint16_t token, int approve,
                                uint8_t *reply, uint16_t rcap,
                                uint16_t *rlen)
{
    uint8_t p[3];

    if (!bus || !reply || !rlen) {
        return ARK_BUS_E_INVAL;
    }
    ark_wr16(p, token);
    p[2] = approve ? 1u : 0u;
    return ark_bus_rpc_call(bus, dst, ARK_BUS_M_TOOL_APPROVE, p, 3u,
                            reply, rcap, rlen);
}

int ark_bus_tool_remote_audit(struct ark_bus *bus, uint32_t dst, uint8_t max,
                              uint8_t *reply, uint16_t rcap, uint16_t *rlen)
{
    uint8_t p[1];

    if (!bus || !reply || !rlen) {
        return ARK_BUS_E_INVAL;
    }
    p[0] = max;
    return ark_bus_rpc_call(bus, dst, ARK_BUS_M_TOOL_AUDIT, p, 1u,
                            reply, rcap, rlen);
}

#if ARK_BUS_TOOL_STAGE_MAX > 0
int ark_bus_tool_remote_result(struct ark_bus *bus, uint32_t dst,
                               uint16_t handle, uint32_t off, uint8_t max,
                               uint8_t *reply, uint16_t rcap, uint16_t *rlen)
{
    uint8_t p[TOOL_RESULT_REQ_WIRE];

    if (!bus || !reply || !rlen) {
        return ARK_BUS_E_INVAL;
    }
    ark_wr16(p, handle);
    ark_wr32(p + 2u, off);
    p[6] = max;
    return ark_bus_rpc_call(bus, dst, ARK_BUS_M_TOOL_RESULT, p,
                            (uint16_t)TOOL_RESULT_REQ_WIRE, reply, rcap, rlen);
}
#endif /* ARK_BUS_TOOL_STAGE_MAX > 0 */

int ark_bus_tool_decode_desc(const uint8_t *buf, uint16_t len, uint16_t *off,
                             ark_bus_tool_desc_t *out)
{
    uint16_t o;
    uint16_t nl;
    uint16_t sl;
    uint16_t pl;

    if (!buf || !off || !out) {
        return ARK_BUS_E_INVAL;
    }
    o = *off;
    if ((uint32_t)o + 5u > (uint32_t)len) {
        return ARK_BUS_E_FORMAT;
    }
    out->id    = ark_rd16(buf + o);
    out->flags = ark_rd16(buf + o + 2u);
    o = (uint16_t)(o + 4u);

    nl = buf[o++];
    if ((uint32_t)o + (uint32_t)nl + 1u > (uint32_t)len) {
        return ARK_BUS_E_FORMAT;
    }
    out->name = (const char *)(buf + o);
    o = (uint16_t)(o + nl + 1u);

    sl = buf[o++];
    if ((uint32_t)o + (uint32_t)sl + 1u > (uint32_t)len) {
        return ARK_BUS_E_FORMAT;
    }
    out->summary = (const char *)(buf + o);
    o = (uint16_t)(o + sl + 1u);

    pl = buf[o++];
    if ((uint32_t)o + (uint32_t)pl + 1u > (uint32_t)len) {
        return ARK_BUS_E_FORMAT;
    }
    out->params = (const char *)(buf + o);
    o = (uint16_t)(o + pl + 1u);

    *off = o;
    return ARK_BUS_OK;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

void ark_bus_tool_init(ark_bus_t *bus)
{
    if (!bus) {
        return;
    }
    ark_memset(&bus->tools, 0, sizeof(bus->tools));
    bus->tools.next_token = 1u;   /* 0 is never a live token */
    bus->tools.next_seq   = 1u;   /* the first recorded call is seq 1 */
}

#endif /* ARK_BUS_TOOL_ENABLED */
