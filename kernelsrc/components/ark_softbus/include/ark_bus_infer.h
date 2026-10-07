/*
 * ark_bus_infer.h - the inference adapter (running a model across nodes).
 *
 * M2 gave the bus two ways to move numbers: a reliable point-to-point object
 * transfer and the group operations.  This layer turns those into what a
 * transformer actually needs - a forward pass whose weights and activations are
 * split across ranks, which is the same shape llama.cpp reaches for its RPC
 * backend and Megatron-LM reaches for tensor parallelism:
 *
 *   - a *compute backend* is a vtable of numeric kernels (GEMM, SiLU, RMSNorm,
 *     softmax, ...).  The caller picks one: the portable float reference here,
 *     a hand-tuned or fixed-point kernel set on a given MCU, a GPU, or - later -
 *     a *remote* backend that ships the op stream to a peer, which is exactly
 *     what ggml_backend_rpc does.  The layer above never learns which it got;
 *   - a *plan* is a straight list of steps to run on this rank, ending (where
 *     the split requires it) in an all-reduce.  The executor walks it and
 *     drives the collective layer for the steps that need the group.  This is
 *     the seam the remote backend will serialise (M3 in the roadmap): the steps
 *     are already a small, flat, pointer-light record;
 *   - the split itself is the Megatron recipe, one all-reduce per row-parallel
 *     projection: a column-parallel GEMM keeps its output sharded (no comm), a
 *     row-parallel GEMM sums its partial results with one all-reduce.  An MLP
 *     block is column-parallel up, activation, row-parallel down, all-reduce,
 *     residual - and that is the block builder provided here.
 *
 * What it deliberately does NOT do: no weights live in the layer (the caller
 * owns every buffer; a step borrows pointers and the plan is immutable while
 * it runs), no quantisation (the reference backend is float32; an integer
 * backend plugs in behind the same vtable), no sampling beyond a greedy global
 * argmax (a stochastic sampler is a CALL step), no KV cache (every call
 * recomputes the full causal window), no automatic device placement, no graph
 * optimisation, and no fault recovery past the collective layer's single-op
 * timeout.  Floating-point addition is not associative, so a distributed
 * result is compared to a serial reference with a tolerance, never bit-exact
 * (see ARK_BUS_INFER_TOL_* and the note on ark_bus_infer_mlp).
 *
 * One job runs at a time per bus.  The executor is asynchronous because the
 * all-reduce is: ark_bus_infer_run() executes every local step it can, starts
 * the collective it reaches, and returns; ark_bus_poll() (through
 * ark_bus_infer_tick) finishes the remaining steps and fires the done
 * callback.  Nothing blocks and nothing is allocated.
 */
#ifndef ARK_BUS_INFER_H
#define ARK_BUS_INFER_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"
#include "ark_bus_protocol.h"
#include "ark_bus_coll.h"

#if ARK_BUS_INFER_ENABLED

/* The optional M3 modules.  Only the quantised operand type is needed here (the
 * GEMM slot names it); the rest of each module is declared in its own header. */
#if ARK_BUS_INFER_QUANT_ENABLED
#include "ark_bus_infer_quant.h"
#endif

struct ark_bus;

/* ------------------------------------------------------------------ */
/* Compute backend: the numeric kernels behind the executor.           */
/* ------------------------------------------------------------------ */

/*
 * A backend is a set of function pointers plus a context.  Every kernel
 * returns ARK_BUS_OK or a negative ARK_BUS_E_* code; a NULL pointer means
 * "this backend cannot do that op" and the executor fails the job with
 * E_NOSYS rather than silently skipping a step.  All float buffers are
 * contiguous, row-major, float32; every pointer is caller-owned and borrowed
 * for the duration of the call only.
 */
typedef struct {
    const char *name;

    /* c[m*n] = a[m*k] * b[k*n], row-major, no leading-dimension padding. */
    int (*gemm)(void *ctx, int m, int n, int k,
                const float *a, const float *b, float *c);

    /* x[i] = x[i] / (1 + exp(-x[i])), in place. */
    int (*silu)(void *ctx, float *x, int n);

    /* y[r*dim + i] = x[r*dim + i] / sqrt(mean(x^2) + eps) * w[i]. */
    int (*rmsnorm)(void *ctx, const float *x, const float *w, float *y,
                   int rows, int dim, float eps);

    /* dst[i] += src[i]. */
    int (*add)(void *ctx, float *dst, const float *src, int n);

    /* Softmax over the last `dim` elements of each of `rows` rows. */
    int (*softmax)(void *ctx, float *x, int rows, int dim);

    /* Index of the maximum of x[0..n-1].  Returns the index (>= 0) or a
     * negative error. */
    int (*argmax)(void *ctx, const float *x, int n);

    /*
     * Multi-head causal self-attention over `heads` heads.  `qkv` is
     * [tokens, 3*heads*head_dim] laid out per row as the Q block, then the K
     * block, then the V block, each `heads*head_dim` wide; `out` is
     * [tokens, heads*head_dim].  For head h,
     *   scores[i][j] = (q_i . k_j) / sqrt(head_dim)
     * and, when `causal` is non-zero, position i attends only to j <= i;
     * otherwise it attends to every j.  A NULL pointer means the backend
     * cannot run attention and the executor fails the step with E_NOSYS.
     */
    int (*attn)(void *ctx, const float *qkv, float *out,
                int tokens, int heads, int head_dim, int causal);

#if ARK_BUS_INFER_KV_ENABLED
    /*
     * Decode-step attention with a caller-owned KV cache (M3 completion).
     *
     * `qkv` is the NEW tokens only, [nnew, 3*heads*head_dim], token-major
     * Q|K|V.  The kernel appends the new K and V rows to `k_cache`/`v_cache`
     * (each [max_len, heads*head_dim], row t = absolute position t), advances
     * `*plen` by `nnew`, and writes `out` [nnew, heads*head_dim] where new
     * token i (absolute position old_len+i) attends to every cached position
     * j <= old_len+i when `causal` is non-zero, or to every j in
     * [0, old_len+nnew) otherwise.  Refuses old_len+nnew > max_len with
     * E_INVAL.  A NULL pointer means the backend has no cache and a
     * KV-cache step fails with E_NOSYS, exactly like any other missing kernel.
     */
    int (*attn_cache)(void *ctx, const float *qkv,
                      float *k_cache, float *v_cache, int *plen, int max_len,
                      float *out, int nnew, int heads, int head_dim,
                      int causal);
#endif

#if ARK_BUS_INFER_QUANT_ENABLED
    /*
     * Integer-weight GEMM: c[m*n] = a[m*k] * dequant(b[k*n]).  The weight
     * operand is a descriptor, not a `const float *`, so the signature says
     * what is actually there.  A NULL pointer means a QGEMM step fails with
     * E_NOSYS.
     */
    int (*qgemm)(void *ctx, int m, int n, int k,
                 const float *a, const ark_bus_infer_q8_t *b, float *c);
#endif

    /*
     * Vocab-parallel token-embedding lookup.  `tokens` holds `ntokens` ids;
     * `w` is THIS rank's [vocab_local, dim] slice starting at row `vocab_lo`.
     * For each row r,
     *   out[r*dim + i] = scale * w[(tokens[r]-vocab_lo)*dim + i]
     * when vocab_lo <= tokens[r] < vocab_lo+vocab_local, and 0 otherwise.
     * A following all-reduce sum then reconstructs the full embedding exactly
     * once per token -- only the rank that owns a row fills it, the rest
     * contribute zero, so the sum is the lookup, not a multiple of it.  A NULL
     * pointer means the backend cannot embed and the step fails with E_NOSYS.
     */
    int (*embed)(void *ctx, const int32_t *tokens, int ntokens, int dim,
                 const float *w, int vocab_lo, int vocab_local,
                 float scale, float *out);

    void *ctx;
} ark_bus_infer_backend_t;

/*
 * The portable float32 reference backend: plain C, no libm (exp is a built-in
 * range-reduced approximation), no heap.  It exists so the executor is usable
 * and testable everywhere; it is not tuned.  Returns a pointer to a static,
 * const instance.
 */
const ark_bus_infer_backend_t *ark_bus_infer_ref_backend(void);

/* ------------------------------------------------------------------ */
/* Plan: the flat list of steps one rank runs.                         */
/* ------------------------------------------------------------------ */

/* Step kinds.  The numeric value is part of the record an M3 remote backend
 * would serialise, so it is fixed. */
typedef enum {
    ARK_BUS_INFER_OP_NONE              = 0,
    ARK_BUS_INFER_OP_GEMM              = 1,
    ARK_BUS_INFER_OP_SILU              = 2,
    ARK_BUS_INFER_OP_RMSNORM           = 3,
    ARK_BUS_INFER_OP_ADD               = 4,
    ARK_BUS_INFER_OP_ALLREDUCE_SUM_F32 = 5,
    ARK_BUS_INFER_OP_SOFTMAX           = 6,
    ARK_BUS_INFER_OP_ARGMAX            = 7,
    ARK_BUS_INFER_OP_CALL               = 8,
    ARK_BUS_INFER_OP_ATTN               = 9,
    ARK_BUS_INFER_OP_EMBED              = 10,  /* vocab-parallel lookup   */
    ARK_BUS_INFER_OP_TOPK1              = 11,  /* per-row local (v,i) max */
    ARK_BUS_INFER_OP_ALLGATHER          = 12,  /* async: gather blocks    */
    ARK_BUS_INFER_OP_ARGMAX_MERGE       = 13,  /* global argmax over ranks*/
    /*
     * M3 completion.  The two values are fixed for the same reason as the ones
     * above - they are what a remote backend would serialise - and are
     * reserved whether or not the matching feature is compiled in.  A build
     * without the feature cannot build such a step (its builder is not
     * declared) and refuses a hand-built one at pre-flight.
     */
    ARK_BUS_INFER_OP_ATTN_CACHE         = 14,  /* decode attn w/ KV cache */
    ARK_BUS_INFER_OP_QGEMM              = 15   /* integer-weight GEMM      */
} ark_bus_infer_op_t;

/*
 * A callback step, run in order like any other; lets a caller splice its own
 * work (sampling, logging, an op this layer does not model) into the plan.
 * Return ARK_BUS_OK to continue or a negative code to fail the job.
 */
typedef int (*ark_bus_infer_call_fn)(struct ark_bus *bus, void *user);

/*
 * One step.  `a`, `b`, `c` are the operands (which roles they play depends on
 * `op`, documented on each builder below); `p0..p3` are integer shape
 * parameters and `f0` the one float parameter (an RMSNorm epsilon).  Keeping
 * the record flat is deliberate: an M3 remote backend needs to move it.
 */
typedef struct {
    uint8_t     op;         /* ark_bus_infer_op_t                       */
    uint8_t     rsv8;
    uint16_t    rsv16;
    const void *a;
    const void *b;
    void       *c;
    int32_t     p0, p1, p2, p3;
    float       f0;
} ark_bus_infer_step_t;

/*
 * One top-1 candidate: a value and where it sat.  A vocab-parallel argmax
 * produces one per rank per row and the merge picks across ranks, so the
 * index is meaningful relative to the rank that owns the row range
 * (see ark_bus_infer_plan_argmax_merge).  8 bytes, so it travels as raw bytes.
 */
typedef struct {
    float   value;
    int32_t index;
} ark_bus_infer_candidate_t;

/* A plan is caller-owned and immutable while it runs.  Size it for the job:
 * ARK_BUS_INFER_MAX_STEPS is a static ceiling, not a requirement. */
typedef struct {
    uint8_t  nsteps;
    uint8_t  rsv8;
    uint16_t rsv16;
    const ark_bus_coll_group_t *group;   /* borrowed; required iff a step is a collective */
    ark_bus_infer_step_t steps[ARK_BUS_INFER_MAX_STEPS];
} ark_bus_infer_plan_t;

/* Finish callback: `status` is ARK_BUS_OK or a negative ARK_BUS_E_* code. */
typedef void (*ark_bus_infer_done_cb_t)(int status, void *user);

/* Per-bus executor state (guarded by ARK_BUS_INFER_ENABLED in ark_bus_t).
 *
 * The executor keeps a *view* of the running plan - its step list, its length
 * and the group for any collective step - rather than the plan object itself.
 * The record is plain data, so a receiver of a bounded sub-plan can pass a
 * step array sized for what it actually got instead of the full static
 * ceiling (see ark_bus_infer_run_steps). */
typedef struct {
    const ark_bus_infer_step_t     *steps;   /* the running plan's step list    */
    uint8_t                         nsteps;  /* number of steps in that list    */
    const ark_bus_coll_group_t     *group;   /* group for the collective steps   */
    const ark_bus_infer_backend_t  *be;
    ark_bus_infer_done_cb_t         cb;
    void                           *user;
    uint8_t                         cursor;   /* next step to run                */
    uint8_t                         active;   /* a job is running                */
    uint8_t                         waiting;  /* blocked on a collective         */
    uint8_t                         resume;   /* collective finished; tick will go on */
    int                             status;   /* result of the last finished job */
    int                             pending;  /* status handed back by the collective */
    uint32_t                        jobs_done;
    uint32_t                        jobs_failed;
} ark_bus_infer_state_t;

/* ------------------------------------------------------------------ */
/* Plan builders.  Each appends one step and returns ARK_BUS_OK, or    */
/* ARK_BUS_E_FULL when the plan's step array is exhausted.             */
/* ------------------------------------------------------------------ */

/* Reset a plan: no steps, group set to `grp` (may be NULL if no step needs it). */
void ark_bus_infer_plan_reset(ark_bus_infer_plan_t *plan,
                              const ark_bus_coll_group_t *grp);

/* c[m*n] = a[m*k] * b[k*n]. */
int ark_bus_infer_plan_gemm(ark_bus_infer_plan_t *plan,
                            const float *a, const float *b, float *c,
                            int m, int n, int k);

/* Element-wise SiLU over n floats, in place on `buf`. */
int ark_bus_infer_plan_silu(ark_bus_infer_plan_t *plan, float *buf, int n);

/* RMSNorm: x[rows*dim] with weight w[dim] into y[rows*dim]. */
int ark_bus_infer_plan_rmsnorm(ark_bus_infer_plan_t *plan,
                               const float *x, const float *w, float *y,
                               int rows, int dim, float eps);

/* dst[n] += src[n]. */
int ark_bus_infer_plan_add(ark_bus_infer_plan_t *plan,
                           float *dst, const float *src, int n);

/* Softmax of each of `rows` rows of `dim`. */
int ark_bus_infer_plan_softmax(ark_bus_infer_plan_t *plan,
                               float *buf, int rows, int dim);

/* Write the index of the maximum of buf[n] to *out. */
int ark_bus_infer_plan_argmax(ark_bus_infer_plan_t *plan,
                              const float *buf, int n, int *out);

/* One attention step: out = attn(qkv), with the layouts documented on the
 * backend op.  `causal` is 0 or 1. */
int ark_bus_infer_plan_attn(ark_bus_infer_plan_t *plan,
                            const float *qkv, float *out,
                            int tokens, int heads, int head_dim, int causal);

/*
 * Sum `nfloats` float32 across the plan's group, in place on `buf` (the step's
 * operand c aliases a).  nfloats must be a non-zero multiple of the group size.
 * This is the one asynchronous step.
 */
int ark_bus_infer_plan_allreduce_sum_f32(ark_bus_infer_plan_t *plan,
                                         float *buf, int nfloats);

/* Invoke fn(bus, user) in sequence. */
int ark_bus_infer_plan_call(ark_bus_infer_plan_t *plan,
                            ark_bus_infer_call_fn fn, void *user);

#if ARK_BUS_INFER_QUANT_ENABLED
/*
 * Append a weight-only int8 GEMM step: c[m*n] = a[m*k] * dequant(b[k*n]).
 * `b->rows` must equal `k` and `b->cols` must equal `n` (E_INVAL otherwise);
 * the step needs a backend whose `qgemm` slot is set.  `b` and its code array
 * are borrowed and must outlive the job.
 */
int ark_bus_infer_plan_qgemm(ark_bus_infer_plan_t *plan,
                             const float *a, const ark_bus_infer_q8_t *b,
                             float *c, int m, int n, int k);

/*
 * Fill `out` with a backend that is the portable float reference plus the
 * integer-weight GEMM: every kernel the reference backend has, and a `qgemm`
 * for the steps that carry a quantised weight.  `out` is caller-owned (a plain
 * struct, no allocation); its `ctx` is copied from the reference backend.
 */
void ark_bus_infer_quant_backend_init(ark_bus_infer_backend_t *out);
#endif

/*
 * Vocab-parallel embedding: out[ntokens, dim] gets this rank's rows scaled by
 * `scale`, zeros elsewhere (the backend's embed op).  Follow it with an
 * all-reduce over the same [ntokens, dim] buffer to turn the shard into the
 * replicated embedding.  `w` is [vocab_local, dim] starting at row `vocab_lo`.
 */
int ark_bus_infer_plan_embed(ark_bus_infer_plan_t *plan,
                             const int32_t *tokens, int ntokens, int dim,
                             const float *w, int vocab_lo, int vocab_local,
                             float scale, float *out);

/*
 * A local top-1 for a vocab-parallel argmax.  For each of `rows` rows of
 * `x[rows, dim]`, write the row's largest value and its index to cand[row].
 * `cand` must hold `rows` candidates; follow this with an all-gather of every
 * rank's candidate block and ark_bus_infer_plan_argmax_merge to get the
 * global index.  Uses the backend's argmax kernel.
 */
int ark_bus_infer_plan_topk1(ark_bus_infer_plan_t *plan,
                             const float *x, int rows, int dim,
                             ark_bus_infer_candidate_t *cand);

/*
 * Gather every rank's `blk`-byte block into `rbuf` (nranks*blk bytes), block r
 * at offset r*blk -- the collective layer's all-gather as an async plan step.
 * `sbuf` may alias `rbuf + rank*blk`.  Needs the plan's group.  This is the
 * second asynchronous step kind (the first is the all-reduce).
 */
int ark_bus_infer_plan_allgather(ark_bus_infer_plan_t *plan,
                                 const void *sbuf, void *rbuf, uint32_t blk);

/*
 * Reduce the `nranks * rows` candidates an all-gather produced to a global
 * argmax per row.  Candidates are laid out block-per-rank: rank r's row t is
 * cand[r*rows + t].  The winner is the largest value, ties going to the
 * lowest rank; the index written to out[row] is the local index plus
 * r*vocab_local, i.e. the position in the full vocabulary (pass 0 to keep the
 * raw index).  Symbol-free: it reads what the gather wrote, no kernel needed.
 */
int ark_bus_infer_plan_argmax_merge(ark_bus_infer_plan_t *plan,
                                    const ark_bus_infer_candidate_t *cand,
                                    int nranks, int rows, int vocab_local,
                                    int *out);

/* ------------------------------------------------------------------ */
/* The MLP block as a ready-made plan (Megatron tensor-parallel shape). */
/* ------------------------------------------------------------------ */

/*
 * A single MLP block, tensor-parallel:
 *
 *   z = silu(x * W_up)          W_up is [d_model, d_ff/nranks], column-parallel
 *   y = allreduce_sum(z * W_dn) W_dn is [d_ff/nranks, d_model], row-parallel
 *   y += residual               if residual != NULL
 *
 * x and y are [tokens, d_model] and are the SAME on every rank (the split is
 * inside the projections).  w_up and w_dn are THIS rank's shard only.  scratch
 * must hold at least tokens * (d_ff/nranks) floats.  The result y is a sum of
 * `nranks` partial products, so it is not bit-exact against a single-node
 * reference; compare with a relative tolerance (see ARK_BUS_INFER_TOL_REL).
 */
typedef struct {
    int          tokens;
    int          d_model;
    int          d_ff;
    int          nranks;
    int          rank;
    const float *x;         /* [tokens, d_model], replicated            */
    const float *w_up;      /* [d_model, d_ff/nranks], this rank's cols */
    const float *w_dn;      /* [d_ff/nranks, d_model], this rank's rows */
    const float *residual;  /* [tokens, d_model] or NULL                */
    float       *scratch;   /* >= tokens * (d_ff/nranks) floats         */
    float       *y;         /* [tokens, d_model] out                    */
    float        eps;       /* RMSNorm? unused by the plain MLP; kept   */
} ark_bus_infer_mlp_t;

/*
 * Fill `plan` with the MLP block described by `mlp`.  The plan keeps pointers
 * into `mlp`'s buffers, so both must outlive the job.  Returns ARK_BUS_OK or a
 * negative code (E_INVAL on a bad shape - d_ff not divisible by nranks, rank
 * out of range, a NULL buffer).  The all-reduce splits the activation block
 * evenly, so tokens*d_model must also be divisible by nranks; a shape that is
 * not is refused here rather than built into a plan that would fail later.
 */
int ark_bus_infer_plan_mlp(ark_bus_infer_plan_t *plan,
                           const ark_bus_infer_mlp_t *mlp);

/* ------------------------------------------------------------------ */
/* The attention sub-block, head-parallel (Megatron tensor-parallel).  */
/* ------------------------------------------------------------------ */

/*
 * A self-attention sub-block whose HEADS are split across ranks, the
 * complement of the MLP's channel split:
 *
 *   z = x * W_qkv              W_qkv is [d_model, 3*heads/nranks*head_dim],
 *                              column-parallel, no communication
 *   a = attn(z)                this rank's heads only, causal or not
 *   y = allreduce_sum(a * W_o) W_o is [heads/nranks*head_dim, d_model],
 *                              row-parallel, one all-reduce
 *   y += residual              if residual != NULL
 *
 * Attention itself needs no communication because a head never looks at
 * another head's keys: only the two projections do.  `x` and `y` are
 * [tokens, d_model] and are the same on every rank; `w_qkv` and `w_o` are THIS
 * rank's shard.  `scratch` must hold at least tokens * 4 * (heads/nranks) *
 * head_dim floats: the QKV block first, then room for the attention output.
 * heads must be divisible by nranks and heads*head_dim must equal d_model.
 */
typedef struct {
    int          tokens;
    int          heads;      /* total heads, divisible by nranks           */
    int          head_dim;
    int          d_model;    /* == heads * head_dim                        */
    int          nranks;
    int          rank;
    int          causal;     /* 0 or 1                                     */
    const float *x;          /* [tokens, d_model], replicated             */
    const float *w_qkv;      /* this rank's columns                       */
    const float *w_o;        /* this rank's rows                          */
    const float *residual;   /* [tokens, d_model] or NULL                 */
    float       *scratch;    /* >= tokens * 4 * (heads/nranks) * head_dim */
    float       *y;          /* [tokens, d_model] out                     */
} ark_bus_infer_attention_t;

/* Fill `plan` with the attention sub-block described by `at`.  Like the MLP
 * builder this keeps pointers into `at`'s buffers (both must outlive the job)
 * and refuses a bad shape (heads not divisible by nranks, heads*head_dim not
 * equal to d_model, tokens*d_model not divisible by nranks, a NULL buffer)
 * with E_INVAL. */
int ark_bus_infer_plan_attention(ark_bus_infer_plan_t *plan,
                                 const ark_bus_infer_attention_t *at);

/* ------------------------------------------------------------------ */
/* A whole decoder block: norm, attention, norm, MLP.                  */
/* ------------------------------------------------------------------ */

/*
 * The pre-norm decoder block of a decoder-only transformer, tensor-parallel:
 *
 *   x += attn( rmsnorm(x, w_norm1) )
 *   x += mlp ( rmsnorm(x, w_norm2) )
 *
 * `x` is the residual stream [tokens, d_model], read and written in place on
 * every rank.  All weights are this rank's shards (same shapes as the two
 * sub-blocks).  Every buffer is caller-owned; `normed` is [tokens, d_model],
 * `attn_scratch` is [tokens, 4*(heads/nranks)*head_dim], `attn_out` and
 * `mlp_out` are [tokens, d_model], and `mlp_scratch` is
 * [tokens, d_ff/nranks].  The block holds two all-reduces, so the executor
 * parks and resumes twice; nothing else about the driver changes.
 */
typedef struct {
    int          tokens;
    int          d_model;
    int          d_ff;
    int          heads;
    int          head_dim;
    int          nranks;
    int          rank;
    int          causal;
    float        eps;
    float       *x;          /* [tokens, d_model] in/out, replicated      */
    const float *w_norm1;    /* [d_model]                                  */
    const float *w_qkv;      /* this rank's columns                        */
    const float *w_o;        /* this rank's rows                           */
    const float *w_norm2;    /* [d_model]                                  */
    const float *w_up;       /* this rank's columns                        */
    const float *w_dn;       /* this rank's rows                           */
    float       *normed;     /* [tokens, d_model]                          */
    float       *attn_scratch; /* [tokens, 4*(heads/nranks)*head_dim]      */
    float       *attn_out;   /* [tokens, d_model]                          */
    float       *mlp_scratch;/* [tokens, d_ff/nranks]                      */
    float       *mlp_out;    /* [tokens, d_model]                          */
} ark_bus_infer_decoder_t;

/* Fill `plan` with the decoder block described by `dc`.  Needs the group in
 * the plan (set at reset) because of its two all-reduces.  Refuses a bad shape
 * with E_INVAL, same guards as the two sub-blocks, plus d_ff divisible by
 * nranks. */
int ark_bus_infer_plan_decoder_block(ark_bus_infer_plan_t *plan,
                                     const ark_bus_infer_decoder_t *dc);

/* ------------------------------------------------------------------ */
/* A whole decoder-only transformer: embedding to logits (M3.2b).      */
/* ------------------------------------------------------------------ */

/* One layer's shards, in the same layout the block builders use. */
typedef struct {
    const float *w_norm1;    /* [d_model]                                  */
    const float *w_qkv;      /* [d_model, 3*(heads/nranks)*head_dim]       */
    const float *w_o;        /* [(heads/nranks)*head_dim, d_model]         */
    const float *w_norm2;    /* [d_model]                                  */
    const float *w_up;       /* [d_model, d_ff/nranks]                     */
    const float *w_dn;       /* [d_ff/nranks, d_model]                     */
} ark_bus_infer_layer_weights_t;

/*
 * A full decoder-only model, tensor- and vocab-parallel across `nranks` ranks:
 *
 *   x = embed(token_ids) * scale            vocab-parallel, one all-reduce
 *   for each layer:                         12 steps, two all-reduces
 *       x += attn( rmsnorm(x, w_norm1) )     heads split across ranks
 *       x += mlp ( rmsnorm(x, w_norm2) )     channels split across ranks
 *   h = rmsnorm(x, w_final_norm)
 *   logits = h * w_vocab                     vocab-parallel, this rank's cols
 *   tok    = global argmax over the vocabulary
 *
 * Every weight is THIS rank's shard; the residual stream `x`, the normed
 * buffer and the block work buffers are replicated scratch owned by the
 * caller.  `vocab_local` must be vocab/nranks; the embedding table is split by
 * vocabulary row (w_embed is [vocab_local, d_model], rows [vocab_lo,...)) and
 * the output projection by vocabulary column (w_vocab is [d_model,
 * vocab_local], columns [vocab_lo,...)) -- the Megatron convention, so the two
 * ends of the model agree on who owns which token.  The plan is a long flat
 * list (7 + 12*nlayers steps), so ARK_BUS_INFER_MAX_STEPS must be large enough;
 * a shape that does not fit is refused with E_FULL rather than quietly
 * truncated.
 */
typedef struct {
    int          tokens;        /* sequence length T                        */
    int          d_model;
    int          d_ff;
    int          heads;         /* total heads, divisible by nranks         */
    int          head_dim;
    int          nranks;
    int          rank;
    int          causal;
    float        eps;
    const int32_t *token_ids;   /* [tokens]                                 */
    int          vocab;
    int          vocab_local;   /* == vocab / nranks, THIS rank's shard      */
    float        embed_scale;   /* e.g. sqrt(d_model); 1.0 for none          */
    const float *w_embed;       /* [vocab_local, d_model], this rank's rows  */
    const ark_bus_infer_layer_weights_t *layers;   /* [nlayers]             */
    int          nlayers;
    const float *w_final_norm;  /* [d_model]                                */
    const float *w_vocab;       /* [d_model, vocab_local], this rank's cols  */

    /* Scratch, all caller-owned and replicated where noted. */
    float       *x;             /* [tokens, d_model] residual stream         */
    float       *normed;        /* [tokens, d_model]                         */
    float       *attn_scratch;  /* [tokens, 4*(heads/nranks)*head_dim]       */
    float       *attn_out;      /* [tokens, d_model]                         */
    float       *mlp_scratch;   /* [tokens, d_ff/nranks]                     */
    float       *mlp_out;       /* [tokens, d_model]                         */
    float       *hidden;        /* [tokens, d_model] final-norm output       */
    float       *logits;        /* [tokens, vocab_local] this rank's cols    */
    ark_bus_infer_candidate_t *cand;  /* [nranks * tokens] gathered top-1s  */
    int         *tok_out;       /* [tokens] global argmax                    */
} ark_bus_infer_model_t;

/* Fill `plan` with the whole model described by `m`.  Needs the group in the
 * plan (set at reset).  Returns ARK_BUS_OK, E_INVAL on a bad shape or a NULL
 * buffer, or E_FULL when the model needs more steps than the plan can hold.
 * The plan keeps pointers into `m`'s buffers and weight arrays, so all of them
 * must outlive the job. */
int ark_bus_infer_plan_model(ark_bus_infer_plan_t *plan,
                             const ark_bus_infer_model_t *m);

/* ------------------------------------------------------------------ */
/* Driving a plan.                                                     */
/* ------------------------------------------------------------------ */

/*
 * Start `plan` on this bus with backend `be`.  Runs the leading local steps
 * immediately, starts the first collective it meets, and returns ARK_BUS_OK
 * once the job is underway (not necessarily done).  Returns E_BUSY if a job is
 * already running, E_INVAL on a NULL/empty plan, or E_NOSYS if the plan needs a
 * kernel the backend does not provide.
 */
int ark_bus_infer_run(struct ark_bus *bus, const ark_bus_infer_backend_t *be,
                      const ark_bus_infer_plan_t *plan,
                      ark_bus_infer_done_cb_t cb, void *user);

/*
 * Same as ark_bus_infer_run() but takes the plan as a bare step list, so the
 * caller need not own an ark_bus_infer_plan_t.  This is how a node that is
 * handed a bounded sub-plan over the wire keeps its scratch at the sub-plan's
 * length: a plan object carries the full ARK_BUS_INFER_MAX_STEPS ceiling, but
 * the executor only ever reads the first `nsteps` entries of `steps`.  `group`
 * is required only if the list has a collective step.  `steps` must stay alive
 * until the done callback fires.
 */
int ark_bus_infer_run_steps(struct ark_bus *bus,
                            const ark_bus_infer_backend_t *be,
                            const ark_bus_infer_step_t *steps, uint8_t nsteps,
                            const ark_bus_coll_group_t *group,
                            ark_bus_infer_done_cb_t cb, void *user);

/* Advance the running job.  Called from ark_bus_poll(); safe to call when idle
 * (it does nothing). */
void ark_bus_infer_tick(struct ark_bus *bus);

/* 1 while a job is running on this bus, else 0. */
int ark_bus_infer_pending(const struct ark_bus *bus);

/* Counters: finished and failed jobs.  Any NULL pointer is ignored. */
void ark_bus_infer_get_stats(const struct ark_bus *bus,
                             uint32_t *done, uint32_t *failed);

/*
 * Tolerance used when a distributed result is compared to a single-node
 * reference: |a-b| <= ATOL + RTOL*|b|.  Float addition is order-dependent, so
 * bit-exactness is not a goal and must not be asserted.
 */
#define ARK_BUS_INFER_TOL_ATOL  1e-4f
#define ARK_BUS_INFER_TOL_REL   1e-4f

/* Test helper: |a-b| <= ATOL + RTOL*|b| over n floats.  Returns 1 if close. */
int ark_bus_infer_close(const float *a, const float *b, int n,
                        float atol, float rtol);

#endif /* ARK_BUS_INFER_ENABLED */

#endif /* ARK_BUS_INFER_H */
