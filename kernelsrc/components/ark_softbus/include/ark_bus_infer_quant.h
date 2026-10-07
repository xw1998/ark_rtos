/*
 * ark_bus_infer_quant.h - weight-only int8 quantisation (M3 completion).
 *
 * The inference layer's deliberate gap was "no quantisation - the reference
 * backend is float32; an integer backend plugs in behind the same vtable".
 * This file makes that claim concrete, in the smallest honest form:
 *
 *   - a weight MATRIX is stored as int8 codes with ONE scale for the tensor
 *     (`ark_bus_infer_q8_t`).  Activations stay float32, so the rest of the
 *     plan is untouched: only the GEMM that consumes a quantised weight
 *     changes, into an `ark_bus_infer_plan_qgemm` step;
 *   - `ark_bus_infer_quant_backend_init()` layers that integer GEMM over the
 *     portable float kernels, so a node runs the same plan it did before with
 *     the weight representation swapped;
 *   - the error is a stated number, not a hope: with a symmetric round-to-
 *     nearest quantiser, |w - dequant(quant(w))| <= scale/2 elementwise, and
 *     scale is max|w| / 127.  The tests assert against that bound, not against
 *     "close enough".
 *
 * Everything is caller-owned: no heap, no libc, no math library (rounding is a
 * sign-aware add, not lround).
 *
 * The whole file is inside `#if ARK_BUS_INFER_QUANT_ENABLED`, which is 0 by
 * default, so a build that does not ask for it carries none of it.
 */
#ifndef ARK_BUS_INFER_QUANT_H
#define ARK_BUS_INFER_QUANT_H

#include <stdint.h>
#include "ark_bus_config.h"

#if ARK_BUS_INFER_QUANT_ENABLED

/*
 * A symmetric, per-tensor quantised matrix: `w[i] ~= q[i] * scale`, with the
 * codes in [-127, 127] (both ends used, no bias).  `rows`/`cols` describe the
 * logical float matrix so the GEMM can refuse a shape that does not match the
 * step it is asked to run - a mismatch is an error, never a reinterpretation.
 */
typedef struct {
    const int8_t *q;      /* [rows*cols], row-major, codes in [-127,127]      */
    float         scale;  /* w = q * scale; 0.0 iff every weight is zero      */
    int           rows;
    int           cols;
} ark_bus_infer_q8_t;

/*
 * Quantise `n` floats to int8 with one scale.  Writes `*scale_out` and fills
 * `q[0..n-1]`.  `scale_out` is 0 and every code is 0 when all inputs are zero
 * (so the dequantised matrix is exactly zero, not NaN).  Returns ARK_BUS_OK or
 * E_INVAL on a NULL / non-positive count.
 */
int ark_bus_infer_q8_quantize(const float *w, int n, int8_t *q,
                              float *scale_out);

/*
 * Expand a quantised matrix back to float: `w[i] = q[i] * scale`.  Refuses a
 * NULL descriptor, a NULL output, a non-positive shape or a NULL code array
 * with E_INVAL.
 */
int ark_bus_infer_q8_dequantize(const ark_bus_infer_q8_t *wq, float *w);

/*
 * Integer-weight GEMM: c[m*n] = a[m*k] * dequant(b[k*n]).
 *
 * The kernel signature is the one the planner's `QGEMM` step calls; it takes
 * the quantised operand by descriptor (not by `const float *`), so the types
 * say what is actually there.  `b->rows` must equal `k` and `b->cols` must
 * equal `n`, or the call fails with E_INVAL rather than reading past the
 * matrix.
 */
int ark_bus_infer_quant_gemm(void *ctx, int m, int n, int k,
                             const float *a, const ark_bus_infer_q8_t *b,
                             float *c);

#endif /* ARK_BUS_INFER_QUANT_ENABLED */

#endif /* ARK_BUS_INFER_QUANT_H */
