/*
 * ark_bus_infer_math.h - the no-libm scalar math shared by the optional M3
 * modules (the KV cache kernel and the sampler).
 *
 * These are the same exp / rsqrt / dot the reference backend uses, expressed as
 * `static inline` so each optional module carries its own copy and nothing
 * becomes a link-time symbol.  They are DELIBERATELY not factored out of
 * ark_bus_infer_ref.c: that file is always compiled, and moving its math into a
 * header would change its code generation.  The contract this repo keeps is
 * "a feature disabled costs (almost) nothing", so the always-on file is left
 * exactly as it was and the few dozen bytes of scalar math are duplicated into
 * the modules that are off by default.  See ark_bus_infer_ref.c for the
 * derivation of the approximations.
 *
 * Included by optional modules only.
 */
#ifndef ARK_BUS_INFER_MATH_H
#define ARK_BUS_INFER_MATH_H

#include <stdint.h>

/* 2^f for f in [-0.5, 0.5], a degree-5 minimax polynomial (~1e-7 relative). */
static inline float ark_infer_exp2_poly(float f)
{
    return 1.0f + f * (0.6931471805599453f + f * (0.2402265069591007f +
           f * (0.05550410866482158f + f * (0.009618129107628477f +
           f * 0.0013333558146428443f))));
}

/* exp(x), range-clamped so the exponent field trick stays normal. */
static inline float ark_infer_expf(float x)
{
    float   t, f;
    int     n;
    uint32_t bits;
    float   v;
    union { uint32_t u; float fv; } cv;

    if (x > 88.722839f)  return 3.4028235e38f;
    if (x < -87.336548f) return 0.0f;

    t = x * 1.4426950408889634f;                 /* x / ln2 */
    n = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));
    f = t - (float)n;

    bits = (uint32_t)(n + 127) << 23;
    cv.u = bits;
    v    = cv.fv;
    return v * ark_infer_exp2_poly(f);
}

/* 1/sqrt(x) for x > 0: exponent-halving bit trick + three Newton steps. */
static inline float ark_infer_rsqrtf(float x)
{
    union { uint32_t u; float f; } cv;
    float y;

    cv.f = x;
    cv.u = 0x5f3759dfu - (cv.u >> 1);
    y = cv.f;
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    y = y * (1.5f - 0.5f * x * y * y);
    return y;
}

/* Dot product of two n-vectors. */
static inline float ark_infer_dot(const float *a, const float *b, int n)
{
    float s = 0.0f;
    int   i;
    for (i = 0; i < n; i++) {
        s += a[i] * b[i];
    }
    return s;
}

#endif /* ARK_BUS_INFER_MATH_H */
