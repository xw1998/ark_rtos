/*
 * ark_bus_infer_quant.c - weight-only int8 quantisation.
 *
 * One GEMM is all a weight-only quantiser has to change: the weights are read
 * from int8 codes and scaled, the activation stays float32.  The kernel is a
 * straight triple loop - dequantising `b` on the fly rather than expanding it
 * into a float scratch buffer, which is the point (the matrix never has to
 * exist in float anywhere).
 *
 * The backend helper composes the reference float kernels with this one integer
 * kernel, so a plan that used `gemm` for the projections and switches to
 * `qgemm` runs unchanged in every other step.
 */
#include "ark_bus_priv.h"
#include "ark_bus_infer.h"
#include "ark_bus_infer_priv.h"

#if ARK_BUS_INFER_QUANT_ENABLED

int ark_bus_infer_q8_quantize(const float *w, int n, int8_t *q,
                              float *scale_out)
{
    float amax = 0.0f;
    float scale;
    int   i;

    if (!w || !q || !scale_out || n <= 0) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < n; i++) {
        float v = w[i] < 0.0f ? -w[i] : w[i];
        if (v > amax) {
            amax = v;
        }
    }
    if (amax == 0.0f) {
        *scale_out = 0.0f;
        for (i = 0; i < n; i++) {
            q[i] = 0;
        }
        return ARK_BUS_OK;
    }
    /* 127 is the largest magnitude we store, so the biggest weight lands
     * exactly on +-127 (zero error) and every other element rounds to within
     * half a step. */
    scale = amax / 127.0f;
    for (i = 0; i < n; i++) {
        float t = w[i] / scale;
        int   r = (int)(t + (t >= 0.0f ? 0.5f : -0.5f));   /* round half away */
        if (r > 127) {
            r = 127;
        } else if (r < -127) {
            r = -127;
        }
        q[i] = (int8_t)r;
    }
    *scale_out = scale;
    return ARK_BUS_OK;
}

int ark_bus_infer_q8_dequantize(const ark_bus_infer_q8_t *wq, float *w)
{
    int n, i;

    if (!wq || !wq->q || !w || wq->rows <= 0 || wq->cols <= 0) {
        return ARK_BUS_E_INVAL;
    }
    n = wq->rows * wq->cols;
    for (i = 0; i < n; i++) {
        w[i] = (float)wq->q[i] * wq->scale;
    }
    return ARK_BUS_OK;
}

int ark_bus_infer_quant_gemm(void *ctx, int m, int n, int k,
                             const float *a, const ark_bus_infer_q8_t *b,
                             float *c)
{
    int i, j, p;

    (void)ctx;
    if (m <= 0 || n <= 0 || k <= 0 || !a || !b || !b->q || !c) {
        return ARK_BUS_E_INVAL;
    }
    if (b->rows != k || b->cols != n) {
        /* A shape that does not match the step is refused rather than read
         * past: a mismatched b is a bug in the plan, not data to guess at. */
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < m; i++) {
        for (j = 0; j < n; j++) {
            float acc = 0.0f;
            for (p = 0; p < k; p++) {
                acc += a[(size_t)i * (size_t)k + (size_t)p] *
                       ((float)b->q[(size_t)p * (size_t)n + (size_t)j] * b->scale);
            }
            c[(size_t)i * (size_t)n + (size_t)j] = acc;
        }
    }
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_qgemm(ark_bus_infer_plan_t *plan,
                             const float *a, const ark_bus_infer_q8_t *b,
                             float *c, int m, int n, int k)
{
    ark_bus_infer_step_t *s;

    if (!plan || !a || !b || !c || !b->q || m <= 0 || n <= 0 || k <= 0) {
        return ARK_BUS_E_INVAL;
    }
    if (b->rows != k || b->cols != n) {
        return ARK_BUS_E_INVAL;
    }
    s = plan_push(plan, ARK_BUS_INFER_OP_QGEMM);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a  = a;
    s->b  = b;
    s->c  = c;
    s->p0 = m;
    s->p1 = n;
    s->p2 = k;
    return ARK_BUS_OK;
}

void ark_bus_infer_quant_backend_init(ark_bus_infer_backend_t *out)
{
    if (!out) {
        return;
    }
    /* Everything the reference backend does, plus the one integer kernel. */
    *out = *ark_bus_infer_ref_backend();
    out->name  = "ref-f32+q8";
    out->qgemm = ark_bus_infer_quant_gemm;
}

#endif /* ARK_BUS_INFER_QUANT_ENABLED */
