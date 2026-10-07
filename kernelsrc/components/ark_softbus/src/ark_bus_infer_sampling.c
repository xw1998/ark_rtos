/*
 * ark_bus_infer_sampling.c - the stochastic sampler that runs as a CALL step.
 *
 * Everything here is scalar, allocation-free and libm-free.  The candidate set
 * is built with a bounded sorted insert (the k highest logits, ties going to
 * the lower index), the weights come from the same range-reduced exp the
 * reference backend uses, and the draw is one 24-bit xorshift number scaled to
 * [0, 1).  No full-vocabulary sort, no heap, and no hidden stack array: the
 * candidate buffer lives in the caller-owned sampler struct.
 */
#include "ark_bus_priv.h"
#include "ark_bus_infer.h"
#include "ark_bus_infer_sampling.h"
#include "ark_bus_infer_math.h"

#if ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_SAMPLING_ENABLED

/* xorshift32: one word of state, period 2^32-1, zero is a fixed point. */
static uint32_t sample_rng_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/*
 * Offer one (value, index) to a descending top-k list of capacity k.  Keeps the
 * list sorted, drops the old smallest when full and the newcomer is larger, and
 * uses strict `>` so equal logits keep their original order (a tie is decided
 * by index, deterministically).  Returns the new count.
 */
static int sample_cand_offer(ark_bus_sample_cand_t *cand, int k, int cnt,
                             float value, int32_t index)
{
    int pos, i, last;

    if (cnt >= k && !(value > cand[k - 1].value)) {
        return cnt;
    }
    last = (cnt < k) ? cnt : k - 1;
    pos  = last;
    while (pos > 0 && value > cand[pos - 1].value) {
        pos--;
    }
    for (i = last; i > pos; i--) {
        cand[i] = cand[i - 1];
    }
    cand[pos].value = value;
    cand[pos].index = index;
    return (cnt < k) ? cnt + 1 : cnt;
}

/* Weight of one candidate: uniform for TOPK, exp((v-vmax)/T) otherwise. */
static float sample_weight(const ark_bus_sampler_t *s, int i)
{
    if (s->mode == ARK_BUS_SAMPLE_TOPK) {
        return 1.0f;
    }
    return ark_infer_expf((s->cand[i].value - s->cand[0].value) / s->temperature);
}

void ark_bus_sampler_init(ark_bus_sampler_t *s, int mode, float temperature,
                          int top_k, float top_p, uint32_t seed)
{
    int i;

    if (!s) {
        return;
    }
    s->mode        = mode;
    s->temperature = temperature;
    s->top_k       = top_k;
    s->top_p       = top_p;
    s->rng         = (seed != 0u) ? seed : 0x9e3779b9u;
    for (i = 0; i < ARK_BUS_SAMPLER_MAX_K; i++) {
        s->cand[i].value = 0.0f;
        s->cand[i].index = 0;
    }
}

int ark_bus_sampler_effective_k(const ark_bus_sampler_t *s, int n)
{
    int k;

    if (!s || n <= 0) {
        return 0;
    }
    k = s->top_k;
    if (k > n) {
        k = n;
    }
    if (k > ARK_BUS_SAMPLER_MAX_K) {
        k = ARK_BUS_SAMPLER_MAX_K;
    }
    if (k < 1) {
        return 0;   /* nothing to draw from; the caller sees E_INVAL */
    }
    return k;
}

int ark_bus_sampler_pick(ark_bus_sampler_t *s, const float *logits, int n,
                         int32_t *out)
{
    int   k, m, i, q;
    float total, draw, acc;

    if (!s || !logits || !out || n <= 0) {
        return ARK_BUS_E_INVAL;
    }

    /* Greedy is exact and consumes no randomness. */
    if (s->mode == ARK_BUS_SAMPLE_GREEDY) {
        int best = 0;
        for (i = 1; i < n; i++) {
            if (logits[i] > logits[best]) {
                best = i;
            }
        }
        *out = best;
        return ARK_BUS_OK;
    }

    if (s->mode != ARK_BUS_SAMPLE_TOPK &&
        s->mode != ARK_BUS_SAMPLE_TEMPERATURE &&
        s->mode != ARK_BUS_SAMPLE_TOPP) {
        return ARK_BUS_E_INVAL;
    }
    if (!(s->temperature > 0.0f)) {
        return ARK_BUS_E_INVAL;
    }
    if (s->mode == ARK_BUS_SAMPLE_TOPP &&
        !(s->top_p > 0.0f && s->top_p <= 1.0f)) {
        return ARK_BUS_E_INVAL;
    }
    k = ark_bus_sampler_effective_k(s, n);
    if (k <= 0) {
        return ARK_BUS_E_INVAL;
    }

    /* The candidate set: the k highest logits, descending. */
    m = 0;
    for (i = 0; i < n; i++) {
        m = sample_cand_offer(s->cand, k, m, logits[i], (int32_t)i);
    }

    total = 0.0f;
    for (i = 0; i < m; i++) {
        total += sample_weight(s, i);
    }
    if (!(total > 0.0f)) {
        /* Unreachable: the max candidate weighs exp(0) = 1.  Refuse rather
         * than divide by zero if it ever is. */
        return ARK_BUS_E_STATE;
    }

    /* Nucleus: the shortest descending prefix that carries `top_p` of the
     * mass.  Every other mode draws from the whole candidate set. */
    q = m;
    if (s->mode == ARK_BUS_SAMPLE_TOPP) {
        float cum  = 0.0f;
        float need = s->top_p * total;
        q = 0;
        while (q < m) {
            cum += sample_weight(s, q);
            q++;
            if (cum >= need) {
                break;
            }
        }
        if (q < 1) {
            q = 1;
        }
        if (q != m) {
            float sub = 0.0f;
            for (i = 0; i < q; i++) {
                sub += sample_weight(s, i);
            }
            total = sub;
        }
    }

    /* One draw in [0, 1): 24 bits, scaled by 2^24 (exact in float). */
    draw = ((float)(sample_rng_next(&s->rng) >> 8) / 16777216.0f) * total;
    acc  = 0.0f;
    for (i = 0; i < q; i++) {
        acc += sample_weight(s, i);
        if (draw < acc) {
            *out = s->cand[i].index;
            return ARK_BUS_OK;
        }
    }
    *out = s->cand[q - 1].index;
    return ARK_BUS_OK;
}

int ark_bus_sample_call(struct ark_bus *bus, void *user)
{
    ark_bus_sample_request_t *req = (ark_bus_sample_request_t *)user;

    (void)bus;
    if (!req) {
        return ARK_BUS_E_INVAL;
    }
    return ark_bus_sampler_pick(req->sampler, req->logits, req->n, req->out);
}

int ark_bus_infer_plan_sample(ark_bus_infer_plan_t *plan,
                              ark_bus_sample_request_t *req)
{
    if (!plan || !req || !req->sampler || !req->logits || !req->out ||
        req->n <= 0) {
        return ARK_BUS_E_INVAL;
    }
    return ark_bus_infer_plan_call(plan, ark_bus_sample_call, req);
}

#endif /* ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_SAMPLING_ENABLED */
