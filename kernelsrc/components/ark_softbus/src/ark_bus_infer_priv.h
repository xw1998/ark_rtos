/*
 * ark_bus_infer_priv.h - the one internal helper the inference translation
 * units share: the plan-step append.
 *
 * The executor and the plan builders live in ark_bus_infer.c, and the optional
 * M3 modules (quantisation in ark_bus_infer_quant.c, the KV cache in
 * ark_bus_infer_kv.c) live in their own files so that a build which does not
 * enable them never sees a byte of them.  Those files still append a step, so
 * the append is a `static` function here rather than a `static` function in one
 * .c file: each translation unit gets its own copy and none of them becomes a
 * link-time symbol.
 *
 * Included by ark_bus_infer.c and the optional modules only.
 */
#ifndef ARK_BUS_INFER_PRIV_H
#define ARK_BUS_INFER_PRIV_H

#include "ark_bus_infer.h"

#if ARK_BUS_INFER_ENABLED

/* Append a zeroed step of kind `op`, or NULL when the plan is full/NULL.
 * `static inline` so a module that does not use it (a feature compiled out)
 * defines no symbol and raises no unused-function warning. */
static inline ark_bus_infer_step_t *plan_push(ark_bus_infer_plan_t *plan,
                                              uint8_t op)
{
    ark_bus_infer_step_t *s;

    if (!plan || plan->nsteps >= (uint8_t)ARK_BUS_INFER_MAX_STEPS) {
        return (ark_bus_infer_step_t *)0;
    }
    s = &plan->steps[plan->nsteps++];
    s->op    = op;
    s->rsv8  = 0u;
    s->rsv16 = 0u;
    s->a = (const void *)0;
    s->b = (const void *)0;
    s->c = (void *)0;
    s->p0 = s->p1 = s->p2 = s->p3 = 0;
    s->f0 = 0.0f;
    return s;
}

#endif /* ARK_BUS_INFER_ENABLED */

#endif /* ARK_BUS_INFER_PRIV_H */
