/*
 * ark_bus_tool.h - a node's actions, described and bounded for a language model.
 *
 * M5 is the "LLM in the loop" milestone: the point of the whole bus is that a
 * model can reach out and *do* things on a cluster of heterogeneous nodes, and
 * that interface has to be (a) machine-readable, so a model can discover what
 * exists without being told, and (b) bounded, so a model cannot do arbitrary
 * damage on a node whose firmware it does not own.  This layer is both.
 *
 *   - A TOOL IS A NAMED, DESCRIBED ACTION.  `ark_bus_tool_register()` binds an
 *     id, a name, a one-line summary, a compact parameter signature and a
 *     handler.  The signature is a tiny grammar (see below), not a JSON
 *     document: an MCU has nowhere to put a kilobyte of JSON Schema, so the
 *     node ships the *fields* and the host expands them into the JSON Schema a
 *     model actually sees.  The split is deliberate - the schema shape a model
 *     needs is a host concern, the field list is a device concern.
 *   - THE POLICY IS DENY-BY-DEFAULT.  A node with no policy installed refuses
 *     every call with E_DENIED, and only tool ids whose bit is set in the
 *     allow-mask run.  So "expose this node to a model" is an explicit act,
 *     not the default state of a started bus.  The policy also caps the call
 *     rate in a fixed window and the encoded argument size.
 *   - HIGH-RISK TOOLS ARE HELD FOR A HUMAN.  A tool flagged
 *     ARK_BUS_TOOL_APPROVAL does not run on `TOOL_CALL`: the node caches the
 *     request and answers E_APPROVAL with a token.  The model (or its host)
 *     surfaces the pending request to a person, and only an explicit
 *     `TOOL_APPROVE` with that token runs it.  This is the "human in the loop"
 *     shape OpenAI's agents SDK calls an approval interruption: the run pauses
 *     and resumes on a decision, it is not silently allowed.
 *   - EVERY DECISION IS RECORDED.  Calls that ran, calls the policy refused and
 *     calls that failed land in a fixed audit ring with the caller and the
 *     result, so "what did the model do" is answerable after the fact.
 *
 * Wire (payload, big-endian).  Method ids live in ark_bus_protocol.h.
 *   TOOL_CALL    id u16 | args (see the value grammar below)
 *   TOOL_CALL R  astatus i8 | payload          payload = result, or the token
 *                When astatus == ARK_BUS_TOOL_STAGED the result did not fit
 *                the reply: payload is total u32 | handle u16, and the caller
 *                pulls it in chunks with TOOL_RESULT.
 *   TOOL_LIST    start u16                     entry index to begin at
 *   TOOL_LIST R  start u16 | next u16 | count u8 | entry[count]
 *                entry = id u16 | flags u16 | nlen u8 | name |
 *                        slen u8 | summary | plen u8 | params
 *                Every string is NUL-terminated on the wire; the length
 *                prefix counts the characters *before* that terminator, so
 *                ark_bus_tool_decode_desc() can point at them directly.
 *   TOOL_APPROVE token u16 | decision u8        decision 1 = run, 0 = deny
 *   TOOL_APPROVE R  astatus i8 | payload
 *   TOOL_AUDIT   max u8                         max entries to return
 *   TOOL_AUDIT R seq u32 | next u32 | count u8 | entry[count]
 *                entry = seq u32 | from u32 | tool u16 | astatus i8 | denied u8
 *   TOOL_RESULT  handle u16 | off u32 | max u8  pull a staged result chunk
 *   TOOL_RESULT R astatus i8 | total u32 | n u8 | bytes[n]
 *                `total` is the whole staged length, `n` the bytes in this
 *                chunk starting at the requested `off`; the caller is done
 *                when off + n == total.
 *
 * A result is not in the reply of a failed call: astatus carries the reason
 * (E_DENIED, E_APPROVAL, E_INVAL, ...) and the payload is empty, so a host
 * never has to guess whether a payload is a result or a leftover.
 *
 * Everything compiles out when ARK_BUS_TOOL_ENABLED is 0.  Nothing here
 * allocates: the registry, the pending table and the audit ring are static
 * arrays in the bus, sized by ark_bus_config.h.
 */
#ifndef ARK_BUS_TOOL_H
#define ARK_BUS_TOOL_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"

#if ARK_BUS_TOOL_ENABLED

struct ark_bus;

/* ------------------------------------------------------------------ */
/* Tool flags.  Bit meanings mirror the MCP tool annotations, so a host  */
/* can pass them through to a model without inventing its own.           */
/* ------------------------------------------------------------------ */
#define ARK_BUS_TOOL_READ_ONLY   0x0001u  /* no side effects (readOnlyHint)  */
#define ARK_BUS_TOOL_MUTATING    0x0002u  /* changes node state (destructive) */
#define ARK_BUS_TOOL_APPROVAL    0x0004u  /* hold for a human before running  */
#define ARK_BUS_TOOL_OPEN_WORLD  0x0008u  /* reaches outside the cluster      */
#define ARK_BUS_TOOL_IDEMPOTENT  0x0010u  /* repeating it is harmless         */

/*
 * The astatus byte of a TOOL_CALL reply.  A normal call answers 0 (result
 * inline) or a negative ARK_BUS_E_* code (no result); the one positive value
 * says "the result is larger than the reply frame, pull it with TOOL_RESULT".
 * A handler never produces this: the dispatch path sets it.
 */
#define ARK_BUS_TOOL_STAGED      1

/*
 * Parameter signature grammar (the `params` string a tool registers):
 *
 *     signature := "" | name ":" type ( "," name ":" type )*
 *     type      := "i32" | "u32" | "f32" | "bool" | "str" | "blob"
 *
 * Whitespace is not allowed.  Example: "channel:u32,value:f32".  A host turns
 * one `name:type` into one JSON Schema property; the ranking is fixed
 * (integer / number / boolean / string), so nothing here is a promise the
 * device cannot keep.
 */

/*
 * A tool handler.  `args` holds the encoded call arguments and `reply` the
 * encoded result; both use the value grammar of ark_bus_tool_w_* below.
 * Return ARK_BUS_OK for a normal result, or a negative ARK_BUS_E_* code, in
 * which case `reply` is ignored and the code is reported as the call status.
 *
 * `from` is the calling node's id, so a handler can keep per-caller state.
 */
typedef int (*ark_bus_tool_fn)(struct ark_bus *bus, uint32_t from,
                               const uint8_t *args, uint16_t alen,
                               uint8_t *reply, uint16_t rcap, uint16_t *rlen,
                               void *user);

/* ------------------------------------------------------------------ */
/* Value grammar (shared by call arguments and call results)             */
/* ------------------------------------------------------------------ */
/*
 * A value list is:  count u8, then `count` values of
 * { type u8 | len u16 (BE) | bytes }.  Scalars are 4 bytes little-endian
 * (host order on every target this runs on) so the wire is explicit and the
 * reader never type-puns.  Use the writer/reader below rather than hand-coding
 * offsets; they enforce the length and the count.
 */
#define ARK_BUS_TOOL_V_I32   0u
#define ARK_BUS_TOOL_V_U32   1u
#define ARK_BUS_TOOL_V_F32   2u
#define ARK_BUS_TOOL_V_BOOL  3u
#define ARK_BUS_TOOL_V_STR   4u
#define ARK_BUS_TOOL_V_BLOB  5u

typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint16_t len;      /* bytes written so far, including the count byte */
    uint8_t  count;
    uint8_t  err;      /* sticky: 1 once a put did not fit               */
} ark_bus_tool_writer_t;

typedef struct {
    const uint8_t *buf;
    uint16_t len;
    uint16_t off;
    uint8_t  count;
    uint8_t  idx;
} ark_bus_tool_reader_t;

/* Start a value list.  `cap` must be at least 1. */
void ark_bus_tool_w_init(ark_bus_tool_writer_t *w, uint8_t *buf, uint16_t cap);
/* Append a value.  A put that does not fit sets w->err and changes nothing
 * else; check it once with ark_bus_tool_w_end(). */
void ark_bus_tool_w_i32(ark_bus_tool_writer_t *w, int32_t v);
void ark_bus_tool_w_u32(ark_bus_tool_writer_t *w, uint32_t v);
void ark_bus_tool_w_f32(ark_bus_tool_writer_t *w, float v);
void ark_bus_tool_w_bool(ark_bus_tool_writer_t *w, int v);
void ark_bus_tool_w_str(ark_bus_tool_writer_t *w, const char *s);
void ark_bus_tool_w_blob(ark_bus_tool_writer_t *w, const void *p, uint16_t n);
/* Finish: returns the encoded length, or ARK_BUS_E_FULL if anything did not
 * fit (in which case `*out_len` is 0 and the buffer must be discarded). */
int ark_bus_tool_w_end(const ark_bus_tool_writer_t *w, uint16_t *out_len);

/* Start reading a value list.  Malformed input yields a reader whose count is
 * 0 and whose next call returns ARK_BUS_E_FORMAT. */
void ark_bus_tool_r_init(ark_bus_tool_reader_t *r, const uint8_t *buf,
                         uint16_t len);
/* Read the next value.  Returns ARK_BUS_OK, ARK_BUS_E_NOTFOUND at the end, or
 * ARK_BUS_E_FORMAT if the list is not well formed. */
int ark_bus_tool_r_next(ark_bus_tool_reader_t *r, uint8_t *type,
                        const uint8_t **val, uint16_t *vlen);
/* Typed conveniences over ark_bus_tool_r_next(); they check the type and the
 * width and return ARK_BUS_E_FORMAT when either is wrong. */
int ark_bus_tool_r_i32(ark_bus_tool_reader_t *r, int32_t *out);
int ark_bus_tool_r_u32(ark_bus_tool_reader_t *r, uint32_t *out);
int ark_bus_tool_r_f32(ark_bus_tool_reader_t *r, float *out);
int ark_bus_tool_r_bool(ark_bus_tool_reader_t *r, int *out);
/* Point `*s`/`*n` at a STR/BLOB payload inside the reader's buffer (no copy). */
int ark_bus_tool_r_raw(ark_bus_tool_reader_t *r, const uint8_t **p, uint16_t *n);

/* ------------------------------------------------------------------ */
/* Registry                                                             */
/* ------------------------------------------------------------------ */

/*
 * Register a tool under `id` (0..ARK_BUS_TOOL_MAX_TOOLS-1, which is also its
 * policy bit).  `name`/`summary`/`params` are borrowed and must stay valid;
 * `name` is at most ARK_BUS_TOOL_NAME_MAX bytes and `params` follows the
 * grammar above.  Registering an id again replaces the previous tool.
 *
 * Returns ARK_BUS_OK, ARK_BUS_E_INVAL (bad id/name/signature or a NULL
 * handler), or ARK_BUS_E_FULL when the registry has no free slot.
 */
int ark_bus_tool_register(struct ark_bus *bus, uint16_t id, const char *name,
                          const char *summary, const char *params,
                          uint16_t flags, ark_bus_tool_fn fn, void *user);

/* Tools currently registered. */
uint8_t ark_bus_tool_count(const struct ark_bus *bus);

/* Copy out the descriptor of the tool at registry index `i` (0..count-1).
 * Returns ARK_BUS_OK or ARK_BUS_E_NOTFOUND. */
typedef struct {
    uint16_t    id;
    uint16_t    flags;
    const char *name;
    const char *summary;
    const char *params;
} ark_bus_tool_desc_t;

int ark_bus_tool_desc_at(const struct ark_bus *bus, uint8_t i,
                         ark_bus_tool_desc_t *out);

/* ------------------------------------------------------------------ */
/* Policy: the safety boundary                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    /*
     * Bit `id` set means tool `id` may run.  Zero means "no tool may run",
     * which is also the state of a bus with no policy installed - the default
     * is closed, not open.
     */
    uint32_t allow_mask;
    /* Per-window call cap; 0 means "no rate limit".  The window is fixed and
     * starts at the first call after the previous window elapsed. */
    uint16_t max_calls;
    uint16_t window_ms;
    /* Calls that must be held for approval regardless of the tool's own flag.
     * A bit set here forces the approval path on. */
    uint32_t approval_mask;
} ark_bus_tool_policy_t;

/*
 * Install (or replace) the policy.  Passing NULL removes it, which also
 * restores deny-all.  Returns ARK_BUS_OK or ARK_BUS_E_INVAL.
 */
int ark_bus_tool_set_policy(struct ark_bus *bus,
                            const ark_bus_tool_policy_t *policy);
/* Copy back the installed policy.  Returns ARK_BUS_OK or ARK_BUS_E_NOTFOUND
 * when none is installed (so "no policy" is distinguishable from "policy that
 * allows nothing"). */
int ark_bus_tool_get_policy(const struct ark_bus *bus,
                            ark_bus_tool_policy_t *out);

/*
 * Per-caller override (M5.3).  `policy` replaces - it does not merge with -
 * the bus-wide policy for `caller`; a caller with neither its own entry nor a
 * bus-wide policy is still refused, so deny-by-default holds.  Each caller
 * entry keeps its own rate-limit window, so one caller cannot exhaust another's
 * budget.  Passing NULL removes the override.  Returns ARK_BUS_OK, or
 * ARK_BUS_E_INVAL / ARK_BUS_E_FULL (no free caller slot).
 */
int ark_bus_tool_set_caller_policy(struct ark_bus *bus, uint32_t caller,
                                   const ark_bus_tool_policy_t *policy);

/*
 * Copy back a caller's override.  Returns ARK_BUS_OK, or ARK_BUS_E_NOTFOUND
 * when this caller has none (it then falls back to the bus-wide policy).
 */
int ark_bus_tool_get_caller_policy(const struct ark_bus *bus, uint32_t caller,
                                   ark_bus_tool_policy_t *out);

/*
 * Restrict who may approve a held call (M5.3).  `count` 0 (or `ids` NULL)
 * clears the list and restores the default: any node may approve.  With a
 * non-empty list only those node ids may, and a TOOL_APPROVE from anyone else
 * is refused with E_DENIED.  The local ark_bus_tool_release() is not on the
 * bus and always runs.
 *
 * The bus identifies a caller by its node id and cannot cryptographically
 * verify it on a shared medium, so this narrows *which* ids may approve; it
 * does not make an id unforgeable.  Returns ARK_BUS_OK or ARK_BUS_E_INVAL
 * (count exceeds ARK_BUS_TOOL_MAX_APPROVERS, or ids is NULL with count > 0).
 */
int ark_bus_tool_set_approvers(struct ark_bus *bus, const uint32_t *ids,
                               uint8_t count);

/*
 * Install the four RPC methods (LIST / CALL / APPROVE / AUDIT) so peers can
 * reach this node's tools.  Registration is explicit on purpose: a node that
 * never calls this is not reachable as a tool provider at all.  Existing
 * handlers on those methods are replaced.  Returns ARK_BUS_OK, or what
 * ark_bus_rpc_register() returns.
 */
int ark_bus_tool_enable(struct ark_bus *bus);

/* ------------------------------------------------------------------ */
/* Audit and counters                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t seq;      /* monotonic call sequence (1-based)              */
    uint32_t from;     /* caller node id                                 */
    uint16_t tool;     /* tool id                                        */
    int8_t   status;   /* ARK_BUS_OK, or the negative code reported      */
    uint8_t  denied;   /* 1 when the policy refused it before running    */
} ark_bus_tool_audit_t;

typedef struct {
    uint32_t calls;        /* TOOL_CALL requests seen                      */
    uint32_t ok;           /* calls that produced a result                 */
    uint32_t denied;       /* refused by the policy                        */
    uint32_t held;         /* held for approval                            */
    uint32_t failed;       /* handler returned a negative code             */
    uint32_t approved;     /* held calls later released by TOOL_APPROVE    */
    uint32_t rejected;     /* held calls explicitly denied                 */
    uint32_t expired;      /* held calls whose token timed out             */
} ark_bus_tool_stats_t;

void ark_bus_tool_get_stats(const struct ark_bus *bus,
                            ark_bus_tool_stats_t *out);

/*
 * Read the audit ring newest-first: `newest = 0` is the most recent entry.
 * Returns ARK_BUS_OK, or ARK_BUS_E_NOTFOUND when `newest` is past the number
 * of entries recorded (or nothing has happened yet).
 */
int ark_bus_tool_audit(const struct ark_bus *bus, uint16_t newest,
                       ark_bus_tool_audit_t *out);

/*
 * Release (approve != 0) or deny (approve == 0) a held call, from any node.
 * This is the local equivalent of the TOOL_APPROVE method, for a host that
 * runs on the same node as the tool.  Returns ARK_BUS_OK, ARK_BUS_E_NOTFOUND
 * for an unknown or expired token, or ARK_BUS_E_STATE if the node has no such
 * pending entry.  Additional result bytes are not returned here; use the
 * remote wrapper when the result matters.
 */
int ark_bus_tool_release(struct ark_bus *bus, uint16_t token, int approve);

/* ------------------------------------------------------------------ */
/* Client side: drive a peer's tools                                    */
/* ------------------------------------------------------------------ */
/*
 * These are blocking convenience wrappers around ark_bus_rpc_call(); they must
 * run in the context that drives ark_bus_poll().  `reply` receives the raw
 * reply body (astatus in byte 0 for CALL/APPROVE) and `*rlen` its true length,
 * which may exceed `rcap` (the copy is truncated, never silently shortened).
 */

/* Enumerate from `start`; on return *next is the index to pass next time, or
 * 0xFFFF when the list is exhausted.  Entries are decoded by the caller with
 * the offsets documented above; helpers below do that decoding. */
int ark_bus_tool_remote_list(struct ark_bus *bus, uint32_t dst, uint16_t start,
                             uint8_t *reply, uint16_t rcap, uint16_t *rlen);
int ark_bus_tool_remote_call(struct ark_bus *bus, uint32_t dst, uint16_t id,
                             const uint8_t *args, uint16_t alen,
                             uint8_t *reply, uint16_t rcap, uint16_t *rlen);
int ark_bus_tool_remote_approve(struct ark_bus *bus, uint32_t dst,
                                uint16_t token, int approve,
                                uint8_t *reply, uint16_t rcap,
                                uint16_t *rlen);
int ark_bus_tool_remote_audit(struct ark_bus *bus, uint32_t dst, uint8_t max,
                              uint8_t *reply, uint16_t rcap, uint16_t *rlen);
#if ARK_BUS_TOOL_STAGE_MAX > 0
/*
 * Pull a staged result chunk.  Send `off` (the byte offset into the whole
 * result), ask for at most `max` bytes, and read the reply as declared above
 * (`total` is the whole length, the bytes follow the 6-byte header).  Only the
 * node that produced the result may pull it; a pull from anyone else is
 * E_DENIED.  Built only when ARK_BUS_TOOL_STAGE_MAX > 0.
 */
int ark_bus_tool_remote_result(struct ark_bus *bus, uint32_t dst,
                               uint16_t handle, uint32_t off, uint8_t max,
                               uint8_t *reply, uint16_t rcap, uint16_t *rlen);
#endif

/*
 * Decode one TOOL_LIST entry beginning at `off` in `buf`.  On success returns
 * ARK_BUS_OK, advances `*off`, and points the descriptor's strings into `buf`
 * (no copy).  Returns ARK_BUS_E_FORMAT when the entry runs past `len`.
 */
int ark_bus_tool_decode_desc(const uint8_t *buf, uint16_t len, uint16_t *off,
                             ark_bus_tool_desc_t *out);

/* The signature a host expands into JSON Schema, exposed so a host and a test
 * agree on the grammar.  Returns 1 when `sig` is a well-formed signature. */
int ark_bus_tool_sig_valid(const char *sig);

/* ------------------------------------------------------------------ */
/* Per-bus state (guarded by ARK_BUS_TOOL_ENABLED in ark_bus_t).         */
/* ------------------------------------------------------------------ */

/* One registry entry.  The strings are borrowed from the caller. */
typedef struct {
    uint16_t        id;
    uint16_t        flags;
    const char     *name;
    const char     *summary;
    const char     *params;
    ark_bus_tool_fn fn;
    void           *user;
} ark_bus_tool_entry_t;

/* One call held for approval, with a copy of its arguments. */
typedef struct {
    uint8_t  used;
    uint16_t token;
    uint16_t tool;
    uint16_t alen;
    uint32_t from;
    uint32_t expires_ms;
    uint8_t  args[ARK_BUS_TOOL_ARG_MAX];
} ark_bus_tool_pending_t;

/* One per-caller policy override (M5.3), with its own rate-limit window. */
typedef struct {
    uint8_t                used;
    uint32_t               caller;
    ark_bus_tool_policy_t  policy;
    uint32_t               window_start_ms;
    uint16_t               window_calls;
} ark_bus_tool_caller_t;

typedef struct {
    ark_bus_tool_entry_t      entries[ARK_BUS_TOOL_MAX_TOOLS];
    uint8_t                   count;
    uint8_t                   enabled;

    ark_bus_tool_policy_t     policy;
    uint8_t                   has_policy;
    uint32_t                  window_start_ms;
    uint16_t                  window_calls;

    ark_bus_tool_caller_t     callers[ARK_BUS_TOOL_MAX_CALLERS];

    uint32_t                  approvers[ARK_BUS_TOOL_MAX_APPROVERS];
    uint8_t                   approver_count;

#if ARK_BUS_TOOL_STAGE_MAX > 0
    uint8_t                   stage[ARK_BUS_TOOL_STAGE_MAX];
    uint16_t                  stage_len;
    uint16_t                  stage_handle;
    uint32_t                  stage_owner;
    uint8_t                   stage_used;
#endif

    ark_bus_tool_pending_t    pending[ARK_BUS_TOOL_PENDING_MAX];
    uint16_t                  next_token;

    ark_bus_tool_audit_t      audit[ARK_BUS_TOOL_AUDIT_LEN];
    uint16_t                  audit_head;   /* next slot to write        */
    uint32_t                  next_seq;     /* 1-based call sequence     */

    ark_bus_tool_stats_t      stats;
} ark_bus_tool_state_t;

#endif /* ARK_BUS_TOOL_ENABLED */

#endif /* ARK_BUS_TOOL_H */
