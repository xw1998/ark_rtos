/*
 * ark_bus_infer_kv.c - the decode-step attention kernel and its plan step.
 *
 * The math is the reference attention's, restricted to the new tokens: a query
 * for new token i attends over cached positions 0..old_len+i (causal) or
 * 0..total-1 (non-causal), reading K/V from the cache the rank has been filling
 * since the prefill.  The prefill itself is just a cached step with nnew =
 * the prompt length, so one code path covers both; the win is that a later
 * one-token step no longer recomputes the whole window.
 *
 * No scratch buffer: the softmax is the same max / sum-exp / weighted-sum three
 * passes the reference attention uses, so a VLA or heap would buy nothing.
 */
#include "ark_bus_priv.h"
#include "ark_bus_infer.h"
#include "ark_bus_infer_priv.h"
#include "ark_bus_infer_kv.h"
#include "ark_bus_infer_math.h"

#if ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_KV_ENABLED

int ark_bus_infer_kv_attn_cache(void *ctx, const float *qkv,
                                float *k_cache, float *v_cache,
                                int *plen, int max_len, float *out,
                                int nnew, int heads, int head_dim, int causal)
{
    int   cdim, old_len, total, i, h, j, d;
    float scale;

    (void)ctx;
    if (!qkv || !k_cache || !v_cache || !plen || !out) {
        return ARK_BUS_E_INVAL;
    }
    if (nnew <= 0 || heads <= 0 || head_dim <= 0 || max_len <= 0) {
        return ARK_BUS_E_INVAL;
    }
    old_len = *plen;
    if (old_len < 0 || old_len + nnew > max_len) {
        /* Overrunning the cache is refused, not silently truncated: a shorter
         * cache than the sequence is a caller sizing bug. */
        return ARK_BUS_E_INVAL;
    }
    cdim  = heads * head_dim;
    scale = ark_infer_rsqrtf((float)head_dim);

    /* Append this step's K and V, one row per new token, at the tail. */
    for (i = 0; i < nnew; i++) {
        const float *row = qkv + (size_t)i * (size_t)(3 * cdim);
        float       *kc  = k_cache + (size_t)(old_len + i) * (size_t)cdim;
        float       *vc  = v_cache + (size_t)(old_len + i) * (size_t)cdim;
        for (j = 0; j < cdim; j++) {
            kc[j] = row[cdim + j];
            vc[j] = row[2 * cdim + j];
        }
    }
    total = old_len + nnew;

    for (i = 0; i < nnew; i++) {
        int          p    = old_len + i;              /* absolute position   */
        int          last = causal ? p : total - 1;   /* highest key allowed */
        const float *qi   = qkv + (size_t)i * (size_t)(3 * cdim);
        float       *oi   = out + (size_t)i * (size_t)cdim;

        for (h = 0; h < heads; h++) {
            const float *qh = qi + (size_t)h * (size_t)head_dim;
            float       *oh = oi + (size_t)h * (size_t)head_dim;
            float        mx  = -3.4028235e38f;
            float        sum = 0.0f;

            for (j = 0; j <= last; j++) {
                const float *kj = k_cache + (size_t)j * (size_t)cdim +
                                  (size_t)h * (size_t)head_dim;
                float s = ark_infer_dot(qh, kj, head_dim) * scale;
                if (s > mx) {
                    mx = s;
                }
            }
            for (j = 0; j <= last; j++) {
                const float *kj = k_cache + (size_t)j * (size_t)cdim +
                                  (size_t)h * (size_t)head_dim;
                sum += ark_infer_expf(ark_infer_dot(qh, kj, head_dim) * scale - mx);
            }
            for (d = 0; d < head_dim; d++) {
                oh[d] = 0.0f;
            }
            if (sum <= 0.0f) {
                continue;
            }
            for (j = 0; j <= last; j++) {
                const float *kj = k_cache + (size_t)j * (size_t)cdim +
                                  (size_t)h * (size_t)head_dim;
                const float *vj = v_cache + (size_t)j * (size_t)cdim +
                                  (size_t)h * (size_t)head_dim;
                float pj = ark_infer_expf(ark_infer_dot(qh, kj, head_dim)
                                          * scale - mx) / sum;
                for (d = 0; d < head_dim; d++) {
                    oh[d] += pj * vj[d];
                }
            }
        }
    }
    *plen = total;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_kv_attn(ark_bus_infer_plan_t *plan,
                               const ark_bus_infer_kv_step_t *st)
{
    ark_bus_infer_step_t *s;

    if (!plan || !st) {
        return ARK_BUS_E_INVAL;
    }
    if (!st->qkv || !st->k_cache || !st->v_cache || !st->len || !st->out ||
        st->nnew <= 0 || st->heads <= 0 || st->head_dim <= 0 ||
        st->max_len <= 0) {
        return ARK_BUS_E_INVAL;
    }
    if (*st->len < 0 || *st->len + st->nnew > st->max_len) {
        return ARK_BUS_E_INVAL;
    }
    s = plan_push(plan, ARK_BUS_INFER_OP_ATTN_CACHE);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = st;
    return ARK_BUS_OK;
}

void ark_bus_infer_kv_backend_init(ark_bus_infer_backend_t *out)
{
    if (!out) {
        return;
    }
    *out = *ark_bus_infer_ref_backend();
    out->name       = "ref-f32+kvcache";
    out->attn_cache = ark_bus_infer_kv_attn_cache;
}

#endif /* ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_KV_ENABLED */
