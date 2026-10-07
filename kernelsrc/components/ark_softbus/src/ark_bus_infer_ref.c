/*
 * ark_bus_infer_ref.c - the portable float32 compute backend.
 *
 * Plain C, no heap, and - this is the part worth stating - no libm.  A
 * transformer's activation and normalisation need exp and a reciprocal square
 * root; pulling in <math.h> would drag a float library into an image that is
 * meant to link with nothing, so both are reimplemented here in a few dozen
 * bytes each:
 *
 *   exp(x)  = 2^n * poly(2^f),  with x/ln2 split into an integer n and a
 *             fractional f in [-0.5, 0.5]; 2^n is built straight into the
 *             exponent field and the fractional part uses a degree-5 minimax
 *             polynomial (~1e-7 relative).  Range is clamped first, so no
 *             overflow/denormal case has to be handled.
 *   rsqrt(x) starts from the exponent-halving bit trick and takes three
 *             Newton steps (~1e-7 relative); only used on a positive mean
 *             square, so the domain is safe.
 *
 * The kernels are not tuned - a straight triple loop for GEMM, one pass for the
 * rest.  They exist so the executor runs and can be tested everywhere; a board
 * that wants speed swaps in its own ark_bus_infer_backend_t (CMSIS-NN, a fixed
 * point set, a GPU) and the executor never knows.
 */
#include "ark_bus_infer.h"

#if ARK_BUS_INFER_ENABLED

/* ------------------------------------------------------------------ */
/* Scalar math, no libm                                                */
/* ------------------------------------------------------------------ */

static float ark_infer_exp2_poly(float f)
{
    return 1.0f + f * (0.6931471805599453f + f * (0.2402265069591007f +
           f * (0.05550410866482158f + f * (0.009618129107628477f +
           f * 0.0013333558146428443f))));
}

static float ark_infer_expf(float x)
{
    float   t, f;
    int     n;
    uint32_t bits;
    float   v;

    if (x > 88.722839f)  return 3.4028235e38f;   /* saturate, stay finite */
    if (x < -87.336548f) return 0.0f;

    t = x * 1.4426950408889634f;                 /* x / ln2 */
    n = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
    f = t - (float)n;

    bits = (uint32_t)(n + 127) << 23;            /* 2^n via the exponent field */
    /* n is in [-126,127] after the clamps, so the exponent is normal. */
    {
        union { uint32_t u; float fv; } cv;
        cv.u = bits;
        v = cv.fv;
    }
    return v * ark_infer_exp2_poly(f);
}

static float ark_infer_rsqrtf(float x)
{
    union { uint32_t u; float f; } cv;
    float y;

    cv.f = x;
    cv.u = 0x5f3759dfu - (cv.u >> 1);            /* initial guess */
    y = cv.f;
    y = y * (1.5f - 0.5f * x * y * y);           /* Newton, x3 */
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return y;
}

/* ------------------------------------------------------------------ */
/* Kernels                                                             */
/* ------------------------------------------------------------------ */

static int ref_gemm(void *ctx, int m, int n, int k,
                    const float *a, const float *b, float *c)
{
    int i, j, p;

    (void)ctx;
    if (m <= 0 || n <= 0 || k <= 0 || !a || !b || !c) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < m; i++) {
        for (j = 0; j < n; j++) {
            float acc = 0.0f;
            for (p = 0; p < k; p++) {
                acc += a[(size_t)i * (size_t)k + (size_t)p] *
                       b[(size_t)p * (size_t)n + (size_t)j];
            }
            c[(size_t)i * (size_t)n + (size_t)j] = acc;
        }
    }
    return ARK_BUS_OK;
}

static int ref_silu(void *ctx, float *x, int n)
{
    int i;

    (void)ctx;
    if (n < 0 || !x) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < n; i++) {
        float v = x[i];
        /* For v below the exp clamp the sigmoid underflows to 0 and the result
         * is 0; carry that through explicitly so no inf*0 ever forms. */
        x[i] = (v < -87.336548f) ? 0.0f : v / (1.0f + ark_infer_expf(-v));
    }
    return ARK_BUS_OK;
}

static int ref_rmsnorm(void *ctx, const float *x, const float *w, float *y,
                       int rows, int dim, float eps)
{
    int r, i;

    (void)ctx;
    if (rows <= 0 || dim <= 0 || !x || !w || !y) {
        return ARK_BUS_E_INVAL;
    }
    for (r = 0; r < rows; r++) {
        const float *xr = x + (size_t)r * (size_t)dim;
        float       *yr = y + (size_t)r * (size_t)dim;
        float        ss = 0.0f;
        float        inv;
        for (i = 0; i < dim; i++) {
            ss += xr[i] * xr[i];
        }
        inv = ark_infer_rsqrtf(ss / (float)dim + eps);
        for (i = 0; i < dim; i++) {
            yr[i] = xr[i] * inv * w[i];
        }
    }
    return ARK_BUS_OK;
}

static int ref_add(void *ctx, float *dst, const float *src, int n)
{
    int i;

    (void)ctx;
    if (n < 0 || !dst || !src) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < n; i++) {
        dst[i] += src[i];
    }
    return ARK_BUS_OK;
}

static int ref_softmax(void *ctx, float *x, int rows, int dim)
{
    int r, i;

    (void)ctx;
    if (rows <= 0 || dim <= 0 || !x) {
        return ARK_BUS_E_INVAL;
    }
    for (r = 0; r < rows; r++) {
        float *xr = x + (size_t)r * (size_t)dim;
        float  mx = xr[0];
        float  sum = 0.0f;
        for (i = 1; i < dim; i++) {
            if (xr[i] > mx) {
                mx = xr[i];
            }
        }
        for (i = 0; i < dim; i++) {
            float e = ark_infer_expf(xr[i] - mx);
            xr[i] = e;
            sum += e;
        }
        if (sum > 0.0f) {
            float inv = 1.0f / sum;
            for (i = 0; i < dim; i++) {
                xr[i] *= inv;
            }
        }
    }
    return ARK_BUS_OK;
}

static int ref_argmax(void *ctx, const float *x, int n)
{
    int i, best;

    (void)ctx;
    if (n <= 0 || !x) {
        return ARK_BUS_E_INVAL;
    }
    best = 0;
    for (i = 1; i < n; i++) {
        if (x[i] > x[best]) {
            best = i;
        }
    }
    return best;
}

static float ref_dot(const float *a, const float *b, int n)
{
    float s = 0.0f;
    int   i;
    for (i = 0; i < n; i++) {
        s += a[i] * b[i];
    }
    return s;
}

/*
 * Multi-head attention, one head at a time.  The softmax is computed in three
 * passes over the keys (max, sum of exp, weighted sum of V) so the kernel needs
 * no temporary row buffer and therefore no VLA or heap; a reference may spend
 * the extra dot products.
 */
static int ref_attn(void *ctx, const float *qkv, float *out,
                    int tokens, int heads, int head_dim, int causal)
{
    int   ti, h, j, d;
    int   cdim = heads * head_dim;
    float scale;

    (void)ctx;
    if (tokens <= 0 || heads <= 0 || head_dim <= 0 ||
        !qkv || !out) {
        return ARK_BUS_E_INVAL;
    }
    scale = ark_infer_rsqrtf((float)head_dim);

    for (ti = 0; ti < tokens; ti++) {
        int last = causal ? ti : tokens - 1;
        for (h = 0; h < heads; h++) {
            /* `qkv` is token-major: each row is [q | k | v], so key j lives
             * at row j, not at a fixed block plus an offset. */
            const float *qi = qkv + (size_t)ti * (size_t)(3 * cdim)
                                   + (size_t)h * (size_t)head_dim;
            float       *oi = out + (size_t)ti * (size_t)cdim
                                   + (size_t)h * (size_t)head_dim;
            size_t       hoff = (size_t)cdim + (size_t)h * (size_t)head_dim;
            float        mx = -3.4028235e38f;
            float        sum = 0.0f;

            for (j = 0; j <= last; j++) {
                const float *kj = qkv + (size_t)j * (size_t)(3 * cdim) + hoff;
                float s = ref_dot(qi, kj, head_dim) * scale;
                if (s > mx) {
                    mx = s;
                }
            }
            for (j = 0; j <= last; j++) {
                const float *kj = qkv + (size_t)j * (size_t)(3 * cdim) + hoff;
                sum += ark_infer_expf(ref_dot(qi, kj, head_dim) * scale - mx);
            }
            for (d = 0; d < head_dim; d++) {
                oi[d] = 0.0f;
            }
            if (sum <= 0.0f) {
                continue;
            }
            for (j = 0; j <= last; j++) {
                const float *kj = qkv + (size_t)j * (size_t)(3 * cdim) + hoff;
                const float *vj = qkv + (size_t)j * (size_t)(3 * cdim)
                                       + (size_t)(2 * cdim)
                                       + (size_t)h * (size_t)head_dim;
                float        p  = ark_infer_expf(ref_dot(qi, kj, head_dim)
                                                 * scale - mx) / sum;
                for (d = 0; d < head_dim; d++) {
                    oi[d] += p * vj[d];
                }
            }
        }
    }
    return ARK_BUS_OK;
}

/*
 * Vocab-parallel embedding: copy this rank's owned rows, scale, and zero the
 * rest.  The caller all-reduces afterwards, and a token owned by exactly one
 * rank is why the sum is the lookup and not a multiple of it.
 */
static int ref_embed(void *ctx, const int32_t *tokens, int ntokens, int dim,
                     const float *w, int vocab_lo, int vocab_local,
                     float scale, float *out)
{
    int t, i;

    (void)ctx;
    if (ntokens <= 0 || dim <= 0 || !tokens || !w || !out) {
        return ARK_BUS_E_INVAL;
    }
    for (t = 0; t < ntokens; t++) {
        float  *o  = out + (size_t)t * (size_t)dim;
        int32_t id = tokens[t];
        if (id >= vocab_lo && id < vocab_lo + vocab_local) {
            const float *row = w + (size_t)(id - vocab_lo) * (size_t)dim;
            for (i = 0; i < dim; i++) {
                o[i] = row[i] * scale;
            }
        } else {
            for (i = 0; i < dim; i++) {
                o[i] = 0.0f;
            }
        }
    }
    return ARK_BUS_OK;
}

const ark_bus_infer_backend_t *ark_bus_infer_ref_backend(void)
{
    static const ark_bus_infer_backend_t be = {
        "ref-f32",
        ref_gemm, ref_silu, ref_rmsnorm, ref_add, ref_softmax, ref_argmax,
        ref_attn,
#if ARK_BUS_INFER_KV_ENABLED
        (void *)0,   /* attn_cache: the float reference keeps no cache      */
#endif
#if ARK_BUS_INFER_QUANT_ENABLED
        (void *)0,   /* qgemm: see ark_bus_infer_quant_backend_init()      */
#endif
        ref_embed,
        (void *)0
    };
    return &be;
}

#endif /* ARK_BUS_INFER_ENABLED */
