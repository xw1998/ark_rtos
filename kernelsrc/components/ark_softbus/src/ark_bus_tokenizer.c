/*
 * ark_bus_tokenizer.c - greedy-longest-match text <-> token ids.
 *
 * The contract lives in ark_bus_tokenizer.h; the interesting decisions are
 * here, and they are all about not inventing an answer:
 *
 *   - init() proves the table is invertible before it is used.  An empty text,
 *     a duplicate id or a duplicate text would each make decode() ambiguous,
 *     so all three are refused up front rather than resolved by a tie-break
 *     nobody can see from the outside.
 *   - encode() is total: unmatched bytes become one unk id each.  That costs
 *     tokens, never bytes, so a caller can always round-trip the structure of
 *     the input even when the vocabulary has never seen its content.
 *   - Both directions report the size they needed when they refuse
 *     (E_FULL), and they write nothing in that case -- a half-filled output
 *     buffer is exactly the kind of "looks authoritative, is wrong" result the
 *     rest of this component refuses to hand out.
 *
 * No libc, no heap, no float: byte compares and index arithmetic only.
 */
#include "ark_bus_tokenizer.h"

#if ARK_BUS_TOKENIZER_ENABLED

static uint16_t tok_strlen(const char *s)
{
    uint16_t n = 0;
    if (!s) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

/* 1 when the first `n` bytes of `a` equal `b` (which has at least n bytes). */
static int tok_prefix_eq(const char *a, const char *b, uint16_t n)
{
    uint16_t i;
    for (i = 0; i < n; i++) {
        if ((uint8_t)a[i] != (uint8_t)b[i]) {
            return 0;
        }
    }
    return 1;
}

int ark_bus_tokenizer_init(ark_bus_tokenizer_t *tok,
                           const ark_bus_token_t *vocab, uint16_t nvocab,
                           int32_t unk_id)
{
    uint16_t i;
    uint16_t j;

    if (!tok) {
        return ARK_BUS_E_INVAL;
    }
    tok->vocab  = (const ark_bus_token_t *)0;
    tok->nvocab = 0;
    tok->unk_id = 0;

    if (!vocab || nvocab == 0u || unk_id < 0) {
        return ARK_BUS_E_INVAL;
    }

    for (i = 0; i < nvocab; i++) {
        uint16_t len = tok_strlen(vocab[i].text);
        if (len == 0u || len > (uint16_t)(ARK_BUS_TOKENIZER_TOKEN_MAX - 1) ||
            vocab[i].id < 0) {
            return ARK_BUS_E_INVAL;
        }
        for (j = 0; j < i; j++) {
            if (vocab[j].id == vocab[i].id) {
                return ARK_BUS_E_INVAL;   /* ambiguous id */
            }
            if (tok_strlen(vocab[j].text) == len &&
                tok_prefix_eq(vocab[i].text, vocab[j].text, len)) {
                return ARK_BUS_E_INVAL;   /* ambiguous text */
            }
        }
    }

    tok->vocab  = vocab;
    tok->nvocab = nvocab;
    tok->unk_id = unk_id;
    return ARK_BUS_OK;
}

/* Longest entry that matches at `text[pos]`, or 0 when none does. */
static uint16_t tok_longest(const ark_bus_tokenizer_t *tok,
                            const char *text, uint16_t len, uint16_t pos,
                            int32_t *id_out)
{
    uint16_t best_len = 0u;
    int32_t  best_id  = 0;
    uint16_t i;

    for (i = 0; i < tok->nvocab; i++) {
        uint16_t mlen = tok_strlen(tok->vocab[i].text);
        if (mlen > best_len && (uint16_t)(len - pos) >= mlen &&
            tok_prefix_eq(text + pos, tok->vocab[i].text, mlen)) {
            best_len = mlen;
            best_id  = tok->vocab[i].id;
        }
    }
    if (best_len == 0u) {
        return 0u;
    }
    *id_out = best_id;
    return best_len;
}

int ark_bus_tokenizer_encode(const ark_bus_tokenizer_t *tok,
                             const char *text, uint16_t len,
                             int32_t *out, uint16_t cap, uint16_t *nout)
{
    uint16_t pos;
    uint16_t n = 0;

    if (!tok || !tok->vocab || !text || !out || !nout) {
        return ARK_BUS_E_INVAL;
    }
    *nout = 0;

    /* Count first, then write: refusing with E_FULL has to leave the output
     * untouched, so the size check cannot be discovered half-way through. */
    for (pos = 0; pos < len; ) {
        int32_t  id  = tok->unk_id;
        uint16_t mlen = tok_longest(tok, text, len, pos, &id);
        pos = (uint16_t)(pos + (mlen != 0u ? mlen : 1u));
        n++;
    }

    if (n > cap) {
        *nout = n;              /* how much room the caller actually needs */
        return ARK_BUS_E_FULL;
    }

    n = 0;
    for (pos = 0; pos < len; ) {
        int32_t  id  = tok->unk_id;
        uint16_t mlen = tok_longest(tok, text, len, pos, &id);
        out[n++] = id;
        pos = (uint16_t)(pos + (mlen != 0u ? mlen : 1u));
    }

    *nout = n;
    return ARK_BUS_OK;
}

int ark_bus_tokenizer_decode(const ark_bus_tokenizer_t *tok,
                             const int32_t *ids, uint16_t n,
                             char *out, uint16_t cap, uint16_t *nout)
{
    uint16_t need = 0;
    uint16_t i;
    uint16_t w = 0;

    if (!tok || !tok->vocab || !ids || !out || !nout) {
        return ARK_BUS_E_INVAL;
    }
    *nout = 0;

    for (i = 0; i < n; i++) {
        const char *t = (const char *)0;
        if (ark_bus_tokenizer_id_text(tok, ids[i], &t) != ARK_BUS_OK) {
            return ARK_BUS_E_NOTFOUND;   /* never skip a token silently */
        }
        need = (uint16_t)(need + tok_strlen(t));
    }

    if ((uint16_t)(need + 1u) > cap) {
        *nout = need;
        return ARK_BUS_E_FULL;
    }

    for (i = 0; i < n; i++) {
        const char *t = (const char *)0;
        uint16_t    tl;
        (void)ark_bus_tokenizer_id_text(tok, ids[i], &t);
        tl = tok_strlen(t);
        {
            uint16_t k;
            for (k = 0; k < tl; k++) {
                out[w++] = t[k];
            }
        }
    }
    out[w] = '\0';
    *nout = w;
    return ARK_BUS_OK;
}

int ark_bus_tokenizer_id_text(const ark_bus_tokenizer_t *tok, int32_t id,
                              const char **text)
{
    uint16_t i;

    if (text) {
        *text = (const char *)0;
    }
    if (!tok || !tok->vocab || !text) {
        return ARK_BUS_E_INVAL;
    }

    for (i = 0; i < tok->nvocab; i++) {
        if (tok->vocab[i].id == id) {
            *text = tok->vocab[i].text;
            return ARK_BUS_OK;
        }
    }
    return ARK_BUS_E_NOTFOUND;
}

#endif /* ARK_BUS_TOKENIZER_ENABLED */
