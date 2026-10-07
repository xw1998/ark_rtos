/*
 * ark_bus_infer.c - the plan executor.
 *
 * Walks a rank's step list, calling the chosen backend's kernels for the local
 * steps and the collective layer for the one step kind that needs the group
 * (all-reduce).  Two rules shape the code:
 *
 *   1. A step is only allowed to advance the cursor once it has actually
 *      happened.  The collective step is asynchronous, so the cursor stops on
 *      it and only the completion callback moves it on - the same discipline
 *      the collective layer needed to stop reusing a buffer mid-flight.
 *   2. Nothing that starts a new collective runs from inside a collective's
 *      own callback.  The callback records the result and sets a flag; the
 *      work resumes from ark_bus_infer_tick() (called by ark_bus_poll after the
 *      collective layer has run), so the two state machines never nest.
 */
#include "ark_bus_priv.h"
#include "ark_bus_infer.h"
#include "ark_bus_infer_priv.h"
#if ARK_BUS_INFER_KV_ENABLED
#include "ark_bus_infer_kv.h"
#endif

#if ARK_BUS_INFER_ENABLED

/* ------------------------------------------------------------------ */
/* Plan builders                                                       */
/* ------------------------------------------------------------------ */

void ark_bus_infer_plan_reset(ark_bus_infer_plan_t *plan,
                              const ark_bus_coll_group_t *grp)
{
    if (!plan) {
        return;
    }
    plan->nsteps = 0u;
    plan->rsv8   = 0u;
    plan->rsv16  = 0u;
    plan->group  = grp;
}

int ark_bus_infer_plan_gemm(ark_bus_infer_plan_t *plan,
                            const float *a, const float *b, float *c,
                            int m, int n, int k)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_GEMM);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = a; s->b = b; s->c = c;
    s->p0 = m; s->p1 = n; s->p2 = k;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_silu(ark_bus_infer_plan_t *plan, float *buf, int n)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_SILU);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->c = buf;
    s->p0 = n;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_rmsnorm(ark_bus_infer_plan_t *plan,
                               const float *x, const float *w, float *y,
                               int rows, int dim, float eps)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_RMSNORM);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = x; s->b = w; s->c = y;
    s->p0 = rows; s->p1 = dim;
    s->f0 = eps;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_add(ark_bus_infer_plan_t *plan,
                           float *dst, const float *src, int n)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_ADD);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->c = dst; s->a = src;
    s->p0 = n;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_softmax(ark_bus_infer_plan_t *plan,
                               float *buf, int rows, int dim)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_SOFTMAX);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->c = buf;
    s->p0 = rows; s->p1 = dim;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_argmax(ark_bus_infer_plan_t *plan,
                              const float *buf, int n, int *out)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_ARGMAX);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = buf; s->c = out;
    s->p0 = n;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_attn(ark_bus_infer_plan_t *plan,
                            const float *qkv, float *out,
                            int tokens, int heads, int head_dim, int causal)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_ATTN);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = qkv; s->c = out;
    s->p0 = tokens; s->p1 = heads; s->p2 = head_dim; s->p3 = causal;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_allreduce_sum_f32(ark_bus_infer_plan_t *plan,
                                         float *buf, int nfloats)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_ALLREDUCE_SUM_F32);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = buf; s->c = buf;   /* in place */
    s->p0 = nfloats;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_call(ark_bus_infer_plan_t *plan,
                            ark_bus_infer_call_fn fn, void *user)
{
    ark_bus_infer_step_t *s = plan_push(plan, ARK_BUS_INFER_OP_CALL);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a = (const void *)fn;
    s->b = (const void *)user;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_embed(ark_bus_infer_plan_t *plan,
                             const int32_t *tokens, int ntokens, int dim,
                             const float *w, int vocab_lo, int vocab_local,
                             float scale, float *out)
{
    ark_bus_infer_step_t *s;

    if (!plan || !tokens || !w || !out || ntokens <= 0 || dim <= 0 ||
        vocab_lo < 0 || vocab_local < 0) {
        return ARK_BUS_E_INVAL;
    }
    s = plan_push(plan, ARK_BUS_INFER_OP_EMBED);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a  = tokens;
    s->b  = w;
    s->c  = out;
    s->p0 = ntokens;
    s->p1 = dim;
    s->p2 = vocab_lo;
    s->p3 = vocab_local;
    s->f0 = scale;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_topk1(ark_bus_infer_plan_t *plan,
                             const float *x, int rows, int dim,
                             ark_bus_infer_candidate_t *cand)
{
    ark_bus_infer_step_t *s;

    if (!plan || !x || !cand || rows <= 0 || dim <= 0) {
        return ARK_BUS_E_INVAL;
    }
    s = plan_push(plan, ARK_BUS_INFER_OP_TOPK1);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a  = x;
    s->c  = cand;
    s->p0 = dim;
    s->p1 = rows;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_allgather(ark_bus_infer_plan_t *plan,
                                 const void *sbuf, void *rbuf, uint32_t blk)
{
    ark_bus_infer_step_t *s;

    if (!plan || !sbuf || !rbuf || blk == 0u || blk > 0x7fffffffu) {
        return ARK_BUS_E_INVAL;
    }
    s = plan_push(plan, ARK_BUS_INFER_OP_ALLGATHER);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a  = sbuf;
    s->c  = rbuf;
    s->p0 = (int32_t)blk;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_argmax_merge(ark_bus_infer_plan_t *plan,
                                    const ark_bus_infer_candidate_t *cand,
                                    int nranks, int rows, int vocab_local,
                                    int *out)
{
    ark_bus_infer_step_t *s;

    if (!plan || !cand || !out || nranks <= 0 || rows <= 0 || vocab_local < 0) {
        return ARK_BUS_E_INVAL;
    }
    s = plan_push(plan, ARK_BUS_INFER_OP_ARGMAX_MERGE);
    if (!s) {
        return ARK_BUS_E_FULL;
    }
    s->a  = cand;
    s->c  = out;
    s->p0 = nranks;
    s->p1 = rows;
    s->p2 = vocab_local;
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_mlp(ark_bus_infer_plan_t *plan,
                           const ark_bus_infer_mlp_t *mlp)
{
    int flocal, rc;

    if (!plan || !mlp) {
        return ARK_BUS_E_INVAL;
    }
    if (mlp->nranks <= 0 || mlp->rank < 0 || mlp->rank >= mlp->nranks ||
        mlp->tokens <= 0 || mlp->d_model <= 0 || mlp->d_ff <= 0 ||
        (mlp->d_ff % mlp->nranks) != 0) {
        return ARK_BUS_E_INVAL;
    }
    if (!mlp->x || !mlp->w_up || !mlp->w_dn || !mlp->scratch || !mlp->y) {
        return ARK_BUS_E_INVAL;
    }
    /* The all-reduce splits the activation block evenly across ranks (the
     * collective layer rejects a byte count that is not a multiple of the
     * group size), so refuse a shape the group cannot hold rather than build
     * a plan that would fail at run time. */
    if (((long)mlp->tokens * (long)mlp->d_model) % mlp->nranks != 0) {
        return ARK_BUS_E_INVAL;
    }
    flocal = mlp->d_ff / mlp->nranks;

    ark_bus_infer_plan_reset(plan, plan->group);

    /* z = x * W_up[this rank's columns] : out sharded, no comm. */
    rc = ark_bus_infer_plan_gemm(plan, mlp->x, mlp->w_up, mlp->scratch,
                                 mlp->tokens, flocal, mlp->d_model);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_infer_plan_silu(plan, mlp->scratch, mlp->tokens * flocal);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    /* y = z * W_dn[this rank's rows] : partial, then one all-reduce. */
    rc = ark_bus_infer_plan_gemm(plan, mlp->scratch, mlp->w_dn, mlp->y,
                                 mlp->tokens, mlp->d_model, flocal);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_infer_plan_allreduce_sum_f32(plan, mlp->y,
                                              mlp->tokens * mlp->d_model);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    if (mlp->residual) {
        rc = ark_bus_infer_plan_add(plan, mlp->y, mlp->residual,
                                    mlp->tokens * mlp->d_model);
        if (rc != ARK_BUS_OK) {
            return rc;
        }
    }
    return ARK_BUS_OK;
}

int ark_bus_infer_plan_attention(ark_bus_infer_plan_t *plan,
                                 const ark_bus_infer_attention_t *at)
{
    int   hl, cdim, cqkv, rc;
    float *qkv, *a;

    if (!plan || !at) {
        return ARK_BUS_E_INVAL;
    }
    if (at->nranks <= 0 || at->rank < 0 || at->rank >= at->nranks ||
        at->tokens <= 0 || at->d_model <= 0 ||
        at->heads <= 0 || at->head_dim <= 0 ||
        (at->heads % at->nranks) != 0) {
        return ARK_BUS_E_INVAL;
    }
    if ((long)at->heads * (long)at->head_dim != (long)at->d_model) {
        return ARK_BUS_E_INVAL;
    }
    if (!at->x || !at->w_qkv || !at->w_o || !at->scratch || !at->y) {
        return ARK_BUS_E_INVAL;
    }
    if (((long)at->tokens * (long)at->d_model) % at->nranks != 0) {
        return ARK_BUS_E_INVAL;
    }
    hl   = at->heads / at->nranks;
    cdim = hl * at->head_dim;
    cqkv = 3 * cdim;
    if (cdim <= 0 || cqkv <= 0) {
        return ARK_BUS_E_INVAL;
    }
    qkv = at->scratch;
    a   = at->scratch + (size_t)at->tokens * (size_t)cqkv;

    ark_bus_infer_plan_reset(plan, plan->group);

    /* z = x * W_qkv : this rank's heads' Q/K/V, column-parallel, no comm. */
    rc = ark_bus_infer_plan_gemm(plan, at->x, at->w_qkv, qkv,
                                 at->tokens, cqkv, at->d_model);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_infer_plan_attn(plan, qkv, a, at->tokens, hl,
                                 at->head_dim, at->causal);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    /* y = a * W_o : rows summed across ranks, then one all-reduce. */
    rc = ark_bus_infer_plan_gemm(plan, a, at->w_o, at->y,
                                 at->tokens, at->d_model, cdim);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    rc = ark_bus_infer_plan_allreduce_sum_f32(plan, at->y,
                                              at->tokens * at->d_model);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    if (at->residual) {
        rc = ark_bus_infer_plan_add(plan, at->y, at->residual,
                                    at->tokens * at->d_model);
        if (rc != ARK_BUS_OK) {
            return rc;
        }
    }
    return ARK_BUS_OK;
}

/* Validate a decoder shape and append its twelve steps, without resetting the
 * plan -- the model builder stacks many of these into one plan. */
static int append_decoder_block(ark_bus_infer_plan_t *plan,
                                const ark_bus_infer_decoder_t *dc)
{
    int   hl, cdim, cqkv, flocal, rc;
    float *qkv, *a;

    if (!plan || !dc) {
        return ARK_BUS_E_INVAL;
    }
    if (dc->nranks <= 0 || dc->rank < 0 || dc->rank >= dc->nranks ||
        dc->tokens <= 0 || dc->d_model <= 0 || dc->d_ff <= 0 ||
        dc->heads <= 0 || dc->head_dim <= 0) {
        return ARK_BUS_E_INVAL;
    }
    if ((dc->heads % dc->nranks) != 0 || (dc->d_ff % dc->nranks) != 0 ||
        (long)dc->heads * (long)dc->head_dim != (long)dc->d_model) {
        return ARK_BUS_E_INVAL;
    }
    if (!dc->x || !dc->w_norm1 || !dc->w_qkv || !dc->w_o || !dc->w_norm2 ||
        !dc->w_up || !dc->w_dn || !dc->normed || !dc->attn_scratch ||
        !dc->attn_out || !dc->mlp_scratch || !dc->mlp_out) {
        return ARK_BUS_E_INVAL;
    }
    if (((long)dc->tokens * (long)dc->d_model) % dc->nranks != 0) {
        return ARK_BUS_E_INVAL;
    }
    hl     = dc->heads / dc->nranks;
    cdim   = hl * dc->head_dim;
    cqkv   = 3 * cdim;
    flocal = dc->d_ff / dc->nranks;
    if (cdim <= 0 || cqkv <= 0 || flocal <= 0) {
        return ARK_BUS_E_INVAL;
    }
    qkv = dc->attn_scratch;
    a   = dc->attn_scratch + (size_t)dc->tokens * (size_t)cqkv;

    /* attention half: x += attn( rmsnorm(x) ) */
    rc = ark_bus_infer_plan_rmsnorm(plan, dc->x, dc->w_norm1, dc->normed,
                                    dc->tokens, dc->d_model, dc->eps);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_gemm(plan, dc->normed, dc->w_qkv, qkv,
                                 dc->tokens, cqkv, dc->d_model);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_attn(plan, qkv, a, dc->tokens, hl,
                                 dc->head_dim, dc->causal);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_gemm(plan, a, dc->w_o, dc->attn_out,
                                 dc->tokens, dc->d_model, cdim);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_allreduce_sum_f32(plan, dc->attn_out,
                                              dc->tokens * dc->d_model);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_add(plan, dc->x, dc->attn_out,
                                dc->tokens * dc->d_model);
    if (rc != ARK_BUS_OK) return rc;

    /* MLP half: x += mlp( rmsnorm(x) ) */
    rc = ark_bus_infer_plan_rmsnorm(plan, dc->x, dc->w_norm2, dc->normed,
                                    dc->tokens, dc->d_model, dc->eps);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_gemm(plan, dc->normed, dc->w_up, dc->mlp_scratch,
                                 dc->tokens, flocal, dc->d_model);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_silu(plan, dc->mlp_scratch, dc->tokens * flocal);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_gemm(plan, dc->mlp_scratch, dc->w_dn, dc->mlp_out,
                                 dc->tokens, dc->d_model, flocal);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_allreduce_sum_f32(plan, dc->mlp_out,
                                              dc->tokens * dc->d_model);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_add(plan, dc->x, dc->mlp_out,
                                dc->tokens * dc->d_model);
    if (rc != ARK_BUS_OK) return rc;

    return ARK_BUS_OK;
}

int ark_bus_infer_plan_decoder_block(ark_bus_infer_plan_t *plan,
                                     const ark_bus_infer_decoder_t *dc)
{
    if (!plan || !dc) {
        return ARK_BUS_E_INVAL;
    }
    ark_bus_infer_plan_reset(plan, plan->group);
    return append_decoder_block(plan, dc);
}

int ark_bus_infer_plan_model(ark_bus_infer_plan_t *plan,
                             const ark_bus_infer_model_t *m)
{
    ark_bus_infer_decoder_t dc;
    int i, rc;

    if (!plan || !m) {
        return ARK_BUS_E_INVAL;
    }
    if (m->nranks <= 0 || m->rank < 0 || m->rank >= m->nranks ||
        m->tokens <= 0 || m->d_model <= 0 || m->d_ff <= 0 ||
        m->heads <= 0 || m->head_dim <= 0 || m->vocab <= 0 ||
        m->vocab_local <= 0 || m->nlayers < 0) {
        return ARK_BUS_E_INVAL;
    }
    if ((m->heads % m->nranks) != 0 || (m->d_ff % m->nranks) != 0 ||
        (m->vocab % m->nranks) != 0 ||
        m->vocab_local != m->vocab / m->nranks ||
        (long)m->heads * (long)m->head_dim != (long)m->d_model) {
        return ARK_BUS_E_INVAL;
    }
    if (((long)m->tokens * (long)m->d_model) % m->nranks != 0) {
        return ARK_BUS_E_INVAL;
    }
    if (!m->token_ids || !m->w_embed || !m->w_final_norm || !m->w_vocab ||
        !m->x || !m->normed || !m->attn_scratch || !m->attn_out ||
        !m->mlp_scratch || !m->mlp_out || !m->hidden || !m->logits ||
        !m->cand || !m->tok_out) {
        return ARK_BUS_E_INVAL;
    }
    if (m->nlayers > 0 && !m->layers) {
        return ARK_BUS_E_INVAL;
    }
    for (i = 0; i < m->nlayers; i++) {
        if (!m->layers[i].w_norm1 || !m->layers[i].w_qkv ||
            !m->layers[i].w_o || !m->layers[i].w_norm2 ||
            !m->layers[i].w_up || !m->layers[i].w_dn) {
            return ARK_BUS_E_INVAL;
        }
    }

    ark_bus_infer_plan_reset(plan, plan->group);

    /* x = embed(token_ids) * scale, summed across the vocabulary shards. */
    rc = ark_bus_infer_plan_embed(plan, m->token_ids, m->tokens, m->d_model,
                                  m->w_embed, m->rank * m->vocab_local,
                                  m->vocab_local, m->embed_scale, m->x);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_allreduce_sum_f32(plan, m->x,
                                              m->tokens * m->d_model);
    if (rc != ARK_BUS_OK) return rc;

    /* The stacked blocks, each twelve steps with two all-reduces. */
    for (i = 0; i < m->nlayers; i++) {
        dc.tokens       = m->tokens;
        dc.d_model      = m->d_model;
        dc.d_ff         = m->d_ff;
        dc.heads        = m->heads;
        dc.head_dim     = m->head_dim;
        dc.nranks       = m->nranks;
        dc.rank         = m->rank;
        dc.causal       = m->causal;
        dc.eps          = m->eps;
        dc.x            = m->x;
        dc.w_norm1      = m->layers[i].w_norm1;
        dc.w_qkv        = m->layers[i].w_qkv;
        dc.w_o          = m->layers[i].w_o;
        dc.w_norm2      = m->layers[i].w_norm2;
        dc.w_up         = m->layers[i].w_up;
        dc.w_dn         = m->layers[i].w_dn;
        dc.normed       = m->normed;
        dc.attn_scratch = m->attn_scratch;
        dc.attn_out     = m->attn_out;
        dc.mlp_scratch  = m->mlp_scratch;
        dc.mlp_out      = m->mlp_out;
        rc = append_decoder_block(plan, &dc);
        if (rc != ARK_BUS_OK) return rc;
    }

    /* Final norm, the vocab-parallel projection, and the global argmax. */
    rc = ark_bus_infer_plan_rmsnorm(plan, m->x, m->w_final_norm, m->hidden,
                                    m->tokens, m->d_model, m->eps);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_gemm(plan, m->hidden, m->w_vocab, m->logits,
                                 m->tokens, m->vocab_local, m->d_model);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_topk1(plan, m->logits, m->tokens, m->vocab_local,
                                  m->cand + (size_t)m->rank * (size_t)m->tokens);
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_allgather(plan,
                                      m->cand + (size_t)m->rank * (size_t)m->tokens,
                                      m->cand,
                                      (uint32_t)((size_t)m->tokens * sizeof(ark_bus_infer_candidate_t)));
    if (rc != ARK_BUS_OK) return rc;
    rc = ark_bus_infer_plan_argmax_merge(plan, m->cand, m->nranks, m->tokens,
                                         m->vocab_local, m->tok_out);
    if (rc != ARK_BUS_OK) return rc;

    return ARK_BUS_OK;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static int infer_run_step(ark_bus_t *bus, const ark_bus_infer_backend_t *be,
                          const ark_bus_infer_step_t *s)
{
    switch (s->op) {
    case ARK_BUS_INFER_OP_GEMM:
        if (!be->gemm) return ARK_BUS_E_NOSYS;
        return be->gemm(be->ctx, s->p0, s->p1, s->p2,
                        (const float *)s->a, (const float *)s->b, (float *)s->c);
    case ARK_BUS_INFER_OP_SILU:
        if (!be->silu) return ARK_BUS_E_NOSYS;
        return be->silu(be->ctx, (float *)s->c, s->p0);
    case ARK_BUS_INFER_OP_RMSNORM:
        if (!be->rmsnorm) return ARK_BUS_E_NOSYS;
        return be->rmsnorm(be->ctx, (const float *)s->a, (const float *)s->b,
                           (float *)s->c, s->p0, s->p1, s->f0);
    case ARK_BUS_INFER_OP_ADD:
        if (!be->add) return ARK_BUS_E_NOSYS;
        return be->add(be->ctx, (float *)s->c, (const float *)s->a, s->p0);
    case ARK_BUS_INFER_OP_SOFTMAX:
        if (!be->softmax) return ARK_BUS_E_NOSYS;
        return be->softmax(be->ctx, (float *)s->c, s->p0, s->p1);
    case ARK_BUS_INFER_OP_ARGMAX: {
        int r;
        if (!be->argmax) return ARK_BUS_E_NOSYS;
        r = be->argmax(be->ctx, (const float *)s->a, s->p0);
        if (r < 0) return r;
        *(int *)s->c = r;
        return ARK_BUS_OK;
    }
    case ARK_BUS_INFER_OP_ATTN:
        if (!be->attn) return ARK_BUS_E_NOSYS;
        return be->attn(be->ctx, (const float *)s->a, (float *)s->c,
                        s->p0, s->p1, s->p2, s->p3);
#if ARK_BUS_INFER_KV_ENABLED
    case ARK_BUS_INFER_OP_ATTN_CACHE: {
        const ark_bus_infer_kv_step_t *kv =
            (const ark_bus_infer_kv_step_t *)s->a;
        if (!be->attn_cache) return ARK_BUS_E_NOSYS;
        if (!kv) return ARK_BUS_E_INVAL;
        return be->attn_cache(be->ctx, kv->qkv, kv->k_cache, kv->v_cache,
                              kv->len, kv->max_len, kv->out,
                              kv->nnew, kv->heads, kv->head_dim, kv->causal);
    }
#endif
#if ARK_BUS_INFER_QUANT_ENABLED
    case ARK_BUS_INFER_OP_QGEMM:
        if (!be->qgemm) return ARK_BUS_E_NOSYS;
        return be->qgemm(be->ctx, s->p0, s->p1, s->p2,
                         (const float *)s->a,
                         (const ark_bus_infer_q8_t *)s->b, (float *)s->c);
#endif
    case ARK_BUS_INFER_OP_CALL: {
        ark_bus_infer_call_fn fn = (ark_bus_infer_call_fn)s->a;
        if (!fn) return ARK_BUS_E_INVAL;
        return fn(bus, (void *)s->b);
    }
    case ARK_BUS_INFER_OP_EMBED:
        if (!be->embed) return ARK_BUS_E_NOSYS;
        return be->embed(be->ctx, (const int32_t *)s->a, s->p0, s->p1,
                         (const float *)s->b, s->p2, s->p3, s->f0,
                         (float *)s->c);
    case ARK_BUS_INFER_OP_TOPK1: {
        const float *x = (const float *)s->a;
        ark_bus_infer_candidate_t *cand = (ark_bus_infer_candidate_t *)s->c;
        int rows = s->p1;
        int dim  = s->p0;
        int t;
        if (!be->argmax) return ARK_BUS_E_NOSYS;
        for (t = 0; t < rows; t++) {
            const float *row = x + (size_t)t * (size_t)dim;
            int idx = be->argmax(be->ctx, row, dim);
            if (idx < 0) return idx;
            cand[t].index = idx;
            cand[t].value = row[idx];
        }
        return ARK_BUS_OK;
    }
    case ARK_BUS_INFER_OP_ARGMAX_MERGE: {
        const ark_bus_infer_candidate_t *cand =
            (const ark_bus_infer_candidate_t *)s->a;
        int *out = (int *)s->c;
        int nranks = s->p0;
        int rows    = s->p1;
        int vloc    = s->p2;
        int t, r;
        for (t = 0; t < rows; t++) {
            int best = 0;
            for (r = 1; r < nranks; r++) {
                if (cand[(size_t)r * (size_t)rows + (size_t)t].value >
                    cand[(size_t)best * (size_t)rows + (size_t)t].value) {
                    best = r;
                }
            }
            out[t] = cand[(size_t)best * (size_t)rows + (size_t)t].index +
                     best * vloc;
        }
        return ARK_BUS_OK;
    }
    default:
        return ARK_BUS_E_INVAL;
    }
}

static void infer_finish(ark_bus_t *bus, int status)
{
    ark_bus_infer_state_t *st = &bus->infer;
    ark_bus_infer_done_cb_t cb = st->cb;
    void *user = st->user;

    st->active  = 0u;
    st->waiting = 0u;
    st->resume  = 0u;
    st->steps   = (const ark_bus_infer_step_t *)0;
    st->nsteps  = 0u;
    st->group   = (const ark_bus_coll_group_t *)0;
    st->cb      = (ark_bus_infer_done_cb_t)0;
    st->user    = (void *)0;
    st->status  = status;
    if (status == ARK_BUS_OK) {
        st->jobs_done++;
    } else {
        st->jobs_failed++;
    }
    if (cb) {
        cb(status, user);
    }
}

/* Called from the collective layer's completion, i.e. from inside
 * ark_bus_poll.  It must not start another collective here - only record. */
static void infer_coll_done(int status, uint8_t op, void *user)
{
    ark_bus_t *bus = (ark_bus_t *)user;
    ark_bus_infer_state_t *st = &bus->infer;

    (void)op;
    st->waiting = 0u;
    st->pending = status;
    st->resume  = 1u;
}

static void infer_advance(ark_bus_t *bus)
{
    ark_bus_infer_state_t *st = &bus->infer;
    const ark_bus_infer_step_t *steps = st->steps;

    if (!steps) {
        return;
    }
    while (st->cursor < st->nsteps) {
        const ark_bus_infer_step_t *s = &steps[st->cursor];
        int rc;

        if (s->op == ARK_BUS_INFER_OP_ALLREDUCE_SUM_F32 ||
            s->op == ARK_BUS_INFER_OP_ALLGATHER) {
            if (st->waiting) {
                return;                     /* still in flight */
            }
            if (!st->group) {
                infer_finish(bus, ARK_BUS_E_INVAL);
                return;
            }
            if (s->op == ARK_BUS_INFER_OP_ALLREDUCE_SUM_F32) {
                rc = ark_bus_coll_allreduce(bus, st->group,
                                            ARK_BUS_COLL_SUM_F32,
                                            (uint8_t)sizeof(float), s->c, s->c,
                                            (uint32_t)s->p0 *
                                                (uint32_t)sizeof(float),
                                            infer_coll_done, bus);
            } else {
                /* All-gather: each rank contributes its p0-byte block, every
                 * rank keeps the concatenation (block r at r*p0). */
                rc = ark_bus_coll_allgather(bus, st->group, (uint32_t)s->p0,
                                            s->a, s->c, infer_coll_done, bus);
            }
            if (rc != ARK_BUS_OK) {
                infer_finish(bus, rc);
                return;
            }
            st->waiting = 1u;
            return;                         /* cursor stays on this step */
        }

        rc = infer_run_step(bus, st->be, s);
        if (rc != ARK_BUS_OK) {
            infer_finish(bus, rc);
            return;
        }
        st->cursor++;
    }
    infer_finish(bus, ARK_BUS_OK);
}

/* Pre-flight: a backend that lacks a needed kernel, or a collective step with
 * no group, fails the call rather than a job that only dies mid-flight. */
static int infer_validate(const ark_bus_infer_step_t *steps, uint8_t nsteps,
                          const ark_bus_coll_group_t *group,
                          const ark_bus_infer_backend_t *be)
{
    uint8_t i;

    for (i = 0; i < nsteps; i++) {
        const ark_bus_infer_step_t *s = &steps[i];
        switch (s->op) {
        case ARK_BUS_INFER_OP_GEMM:    if (!be->gemm)    return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_SILU:    if (!be->silu)    return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_RMSNORM: if (!be->rmsnorm) return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_ADD:     if (!be->add)     return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_SOFTMAX: if (!be->softmax) return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_ARGMAX:  if (!be->argmax)  return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_ATTN:    if (!be->attn)    return ARK_BUS_E_NOSYS; break;
#if ARK_BUS_INFER_KV_ENABLED
        case ARK_BUS_INFER_OP_ATTN_CACHE:
            if (!be->attn_cache) return ARK_BUS_E_NOSYS;
            if (!s->a) return ARK_BUS_E_INVAL;
            break;
#endif
#if ARK_BUS_INFER_QUANT_ENABLED
        case ARK_BUS_INFER_OP_QGEMM:
            if (!be->qgemm) return ARK_BUS_E_NOSYS;
            if (!s->a || !s->b) return ARK_BUS_E_INVAL;
            break;
#endif
        case ARK_BUS_INFER_OP_EMBED:   if (!be->embed)   return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_TOPK1:   if (!be->argmax)  return ARK_BUS_E_NOSYS; break;
        case ARK_BUS_INFER_OP_ARGMAX_MERGE:
            break;
        case ARK_BUS_INFER_OP_ALLREDUCE_SUM_F32:
        case ARK_BUS_INFER_OP_ALLGATHER:
            if (!group) {
                return ARK_BUS_E_INVAL;
            }
            break;
        case ARK_BUS_INFER_OP_CALL:
            if (!s->a) return ARK_BUS_E_INVAL;
            break;
        default:
            return ARK_BUS_E_INVAL;
        }
    }
    return ARK_BUS_OK;
}

int ark_bus_infer_run(ark_bus_t *bus, const ark_bus_infer_backend_t *be,
                      const ark_bus_infer_plan_t *plan,
                      ark_bus_infer_done_cb_t cb, void *user)
{
    if (!plan) {
        return ARK_BUS_E_INVAL;
    }
    return ark_bus_infer_run_steps(bus, be, plan->steps, plan->nsteps,
                                   plan->group, cb, user);
}

int ark_bus_infer_run_steps(ark_bus_t *bus, const ark_bus_infer_backend_t *be,
                            const ark_bus_infer_step_t *steps, uint8_t nsteps,
                            const ark_bus_coll_group_t *group,
                            ark_bus_infer_done_cb_t cb, void *user)
{
    ark_bus_infer_state_t *st;
    int rc;

    if (!bus || !be || !steps) {
        return ARK_BUS_E_INVAL;
    }
    if (nsteps == 0u) {
        return ARK_BUS_E_INVAL;
    }
    rc = infer_validate(steps, nsteps, group, be);
    if (rc != ARK_BUS_OK) {
        return rc;
    }
    st = &bus->infer;
    if (st->active) {
        return ARK_BUS_E_BUSY;
    }
    st->steps   = steps;
    st->nsteps  = nsteps;
    st->group   = group;
    st->be      = be;
    st->cb      = cb;
    st->user    = user;
    st->cursor  = 0u;
    st->waiting = 0u;
    st->resume  = 0u;
    st->pending = ARK_BUS_OK;
    st->active  = 1u;

    /* If the plan has no collective step this finishes here and fires cb from
     * within run(); otherwise it parks on the first all-reduce. */
    infer_advance(bus);
    return ARK_BUS_OK;
}

void ark_bus_infer_tick(ark_bus_t *bus)
{
    ark_bus_infer_state_t *st;

    if (!bus) {
        return;
    }
    st = &bus->infer;
    if (!st->active || st->waiting || !st->resume) {
        return;
    }
    st->resume = 0u;
    if (st->pending != ARK_BUS_OK) {
        infer_finish(bus, st->pending);
        return;
    }
    st->cursor++;      /* consume the collective step that just finished */
    infer_advance(bus);
}

int ark_bus_infer_pending(const struct ark_bus *bus)
{
    if (!bus) {
        return 0;
    }
    return bus->infer.active ? 1 : 0;
}

void ark_bus_infer_get_stats(const struct ark_bus *bus,
                             uint32_t *done, uint32_t *failed)
{
    if (!bus) {
        return;
    }
    if (done) {
        *done = bus->infer.jobs_done;
    }
    if (failed) {
        *failed = bus->infer.jobs_failed;
    }
}

int ark_bus_infer_close(const float *a, const float *b, int n,
                        float atol, float rtol)
{
    int i;

    if (n < 0 || !a || !b) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        float d = a[i] - b[i];
        float m = d < 0.0f ? -d : d;
        float ref = b[i] < 0.0f ? -b[i] : b[i];
        if (m > atol + rtol * ref) {
            return 0;
        }
    }
    return 1;
}

/* Called once from ark_bus_init(). */
void ark_bus_infer_init(ark_bus_t *bus)
{
    ark_bus_infer_state_t *st;

    if (!bus) {
        return;
    }
    st = &bus->infer;
    st->steps   = (const ark_bus_infer_step_t *)0;
    st->nsteps  = 0u;
    st->group   = (const ark_bus_coll_group_t *)0;
    st->be      = (const ark_bus_infer_backend_t *)0;
    st->cb      = (ark_bus_infer_done_cb_t)0;
    st->user    = (void *)0;
    st->cursor  = 0u;
    st->active  = 0u;
    st->waiting = 0u;
    st->resume  = 0u;
    st->status  = ARK_BUS_OK;
    st->pending = ARK_BUS_OK;
    st->jobs_done   = 0u;
    st->jobs_failed = 0u;
}

#endif /* ARK_BUS_INFER_ENABLED */
