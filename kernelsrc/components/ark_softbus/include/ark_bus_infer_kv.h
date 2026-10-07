/*
 * ark_bus_infer_kv.h - decode-step attention with a caller-owned KV cache
 * (M3 completion).
 *
 * The inference layer's second deliberate gap was "no KV cache - every
 * attention call recomputes the full causal window".  A cached decode step is
 * the fix: a rank keeps the keys and values it has already computed, so
 * generating the next token is one query against the history rather than a
 * re-projection of the whole window.
 *
 * The cache is plain caller-owned memory - two float arrays, no heap - and the
 * step that uses it is an ordinary local kernel, so the executor needs no new
 * machinery: it is one more case in the same switch, driven by the same plan.
 *
 * The whole file is inside `#if ARK_BUS_INFER_KV_ENABLED`, 0 by default.
 */
#ifndef ARK_BUS_INFER_KV_H
#define ARK_BUS_INFER_KV_H

#include "ark_bus_infer.h"

#if ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_KV_ENABLED

/*
 * A KV cache for one rank's heads.  `k`/`v` are [max_len, heads*head_dim]
 * row-major, position t on row t; `heads` is THIS rank's local head count
 * (attention is head-parallel, so a rank caches only its own heads).  `len` is
 * the number of positions filled so far.  All fields are caller-owned; the
 * cache has no allocation and no lifetime of its own.
 */
typedef struct {
    float *k;         /* [max_len, heads*head_dim]                       */
    float *v;         /* [max_len, heads*head_dim]                       */
    int    max_len;   /* capacity in positions                           */
    int    heads;     /* this rank's heads (total heads / nranks)        */
    int    head_dim;
    int    len;       /* positions cached so far                         */
} ark_bus_infer_kv_t;

/* The operands of one cached-attention step, in the form the plan stores. */
typedef struct {
    const float *qkv;      /* [nnew, 3*heads*head_dim], new tokens       */
    float       *k_cache;  /* [max_len, heads*head_dim]                  */
    float       *v_cache;  /* [max_len, heads*head_dim]                  */
    int         *len;      /* in/out position count                      */
    int          max_len;
    float       *out;      /* [nnew, heads*head_dim]                     */
    int          nnew;     /* new tokens this step (1 for a decode step) */
    int          heads;
    int          head_dim;
    int          causal;
} ark_bus_infer_kv_step_t;

/*
 * The portable KV-cache attention kernel, matching the backend's `attn_cache`
 * slot.  Appends the new tokens' K/V to the cache, advances `*plen`, and writes
 * `out`.  Uses no libm and no scratch buffer (the softmax is two extra passes
 * over the cached keys, like the reference attention).  Returns ARK_BUS_OK, or
 * E_INVAL on a NULL/invalid argument or when the cache would overflow.
 */
int ark_bus_infer_kv_attn_cache(void *ctx, const float *qkv,
                                float *k_cache, float *v_cache,
                                int *plen, int max_len, float *out,
                                int nnew, int heads, int head_dim, int causal);

/*
 * Append a cached-attention step to `plan`.  `st` is borrowed and must outlive
 * the job.  The plan's group is irrelevant (the step is local).  Returns
 * ARK_BUS_OK, E_INVAL on a bad argument/shape, or E_FULL when the plan is full.
 */
int ark_bus_infer_plan_kv_attn(ark_bus_infer_plan_t *plan,
                               const ark_bus_infer_kv_step_t *st);

/*
 * Fill `out` with a backend that is the portable float reference plus the
 * KV-cache kernel: every reference kernel, and a real `attn_cache`.
 */
void ark_bus_infer_kv_backend_init(ark_bus_infer_backend_t *out);

#endif /* ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_KV_ENABLED */

#endif /* ARK_BUS_INFER_KV_H */
