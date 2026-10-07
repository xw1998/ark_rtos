/*
 * ark_bus_infer_sampling.h - stochastic token sampling as a CALL step
 * (M3 completion).
 *
 * The model plan (M3.2b) ends in a greedy global argmax.  Sampling beyond that
 * is, in the inference layer's own idiom, "a CALL step": a self-contained
 * function the plan splices in.  This file is that function, plus the tiny
 * sampler state it needs.
 *
 * Scope, stated plainly (a sampler that hides its candidate set is a sampler
 * that lies): the distribution is drawn over the `top_k` highest logits, not
 * the whole vocabulary.  There is no heap, so the candidate set is a bounded
 * caller-owned array; a mode that wants more than ARK_BUS_SAMPLER_MAX_K is
 * clamped, and the effective k is what `ark_bus_sampler_pick` reports it used.
 * Set `top_k` to the width you want; for nucleus sampling make it wider than
 * the nucleus.  This is the same trade an embedded sampler makes, and it is
 * the reason the knob exists rather than being hidden.
 *
 * Determinism: the RNG is a 32-bit xorshift whose whole state is one word in
 * the struct, so a seeded sampler replays exactly - which is what the tests
 * assert and what makes a "did the sampler change?" question answerable.
 *
 * No libm: the temperature weights use the same range-reduced exp the reference
 * backend uses.  The whole file is inside `#if ARK_BUS_INFER_SAMPLING_ENABLED`,
 * 0 by default.
 */
#ifndef ARK_BUS_INFER_SAMPLING_H
#define ARK_BUS_INFER_SAMPLING_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_infer.h"

#if ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_SAMPLING_ENABLED

struct ark_bus;

/* How the next token is chosen from the candidate set. */
typedef enum {
    ARK_BUS_SAMPLE_GREEDY      = 0,  /* argmax over all n logits           */
    ARK_BUS_SAMPLE_TOPK        = 1,  /* uniform over the top-k             */
    ARK_BUS_SAMPLE_TEMPERATURE = 2,  /* exp(v/T) over the top-k            */
    ARK_BUS_SAMPLE_TOPP        = 3   /* nucleus (top-p) over the top-k     */
} ark_bus_sample_mode_t;

/* One candidate: a logit and where it sat. */
typedef struct {
    float   value;
    int32_t index;
} ark_bus_sample_cand_t;

/*
 * Caller-owned sampler state.  `cand` is the scratch the pick sort uses, sized
 * by ARK_BUS_SAMPLER_MAX_K; `rng` is the whole RNG state.  Zero-initialise, then
 * call ark_bus_sampler_init().
 */
typedef struct {
    int      mode;        /* ark_bus_sample_mode_t                        */
    float    temperature; /* > 0 for every non-greedy mode                */
    int      top_k;       /* candidate-set size, 1..ARK_BUS_SAMPLER_MAX_K */
    float    top_p;       /* nucleus mass in (0, 1] (TOPP only)           */
    uint32_t rng;         /* xorshift state; never left at 0 after init   */
    ark_bus_sample_cand_t cand[ARK_BUS_SAMPLER_MAX_K];
} ark_bus_sampler_t;

/*
 * Initialise a sampler.  `seed` 0 is replaced by a fixed non-zero constant (a
 * zero xorshift state is a fixed point).  The other arguments are stored as
 * given; ark_bus_sampler_pick() validates them, so a bad temperature or top_k
 * is an error at draw time, not a silent default.
 */
void ark_bus_sampler_init(ark_bus_sampler_t *s, int mode, float temperature,
                          int top_k, float top_p, uint32_t seed);

/* The candidate width this sampler will actually use for `n` logits: the
 * clamped top_k.  Exposed so a caller can report what it got. */
int ark_bus_sampler_effective_k(const ark_bus_sampler_t *s, int n);

/*
 * Draw one token from `logits[0..n-1]` into `*out`.  Advances the RNG for every
 * mode except GREEDY (which is a pure argmax and consumes nothing).  Returns
 * ARK_BUS_OK, or E_INVAL on a NULL/bad argument, a non-positive n, a
 * non-positive temperature in a non-greedy mode, or a nucleus mass outside
 * (0, 1].  A greedy draw is the exact argmax over all n logits.
 */
int ark_bus_sampler_pick(ark_bus_sampler_t *s, const float *logits, int n,
                         int32_t *out);

/*
 * The CALL-step wrapper: `user` points at a request, so a plan can carry the
 * sampler's inputs.  This is the function ark_bus_infer_plan_sample() installs.
 */
typedef struct {
    ark_bus_sampler_t *sampler;  /* in/out: RNG advances                */
    const float       *logits;   /* [n]                                 */
    int                n;
    int32_t           *out;      /* [1]                                 */
} ark_bus_sample_request_t;

/* Signature matches ark_bus_infer_call_fn. */
int ark_bus_sample_call(struct ark_bus *bus, void *user);

/*
 * Append a sampling CALL step to `plan`.  `req` is borrowed and must outlive
 * the job (the step stores a pointer to it).  Validates the request shape so a
 * missing pointer fails here rather than inside the running plan.
 */
int ark_bus_infer_plan_sample(ark_bus_infer_plan_t *plan,
                              ark_bus_sample_request_t *req);

#endif /* ARK_BUS_INFER_ENABLED && ARK_BUS_INFER_SAMPLING_ENABLED */

#endif /* ARK_BUS_INFER_SAMPLING_H */
