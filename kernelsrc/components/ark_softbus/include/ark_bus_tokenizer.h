/*
 * ark_bus_tokenizer.h - text <-> token ids for the model-facing edge.
 *
 * The inference layer speaks int32 token ids: embed() looks a token up in a
 * vocabulary row and the sampler produces one back.  Something has to cross
 * the gap between that and the text an LLM-in-the-loop actually exchanges over
 * the bus, and on an MCU it cannot be a Python regex engine.  This is the
 * small, honest piece that does it:
 *
 *   - The vocabulary is the caller's static table (text -> id).  Nothing here
 *     allocates, reads a file or knows what BPE is.
 *   - Encoding is greedy longest match over the raw bytes: at each position the
 *     longest entry that matches wins, so a vocabulary containing both "the"
 *     and "there" does the obvious thing.  No entry matches -> the caller's
 *     unknown id is emitted and the cursor advances exactly one byte, so a
 *     byte-level fallback is always possible and encode() never fails on
 *     unexpected input (it is total by construction -- a token stream that
 *     silently dropped bytes would be worse than one that admits ignorance).
 *   - Decoding is the inverse: ids joined back into their texts.  An id that
 *     is not in the table is refused with ARK_BUS_E_NOTFOUND rather than
 *     skipped, because a decoder that quietly drops a token tells the caller a
 *     different sentence than the model produced.
 *
 * Off by default (ARK_BUS_TOKENIZER_ENABLED = 0): a node that only moves
 * tensors never needs it and must not carry it.  Everything here is inside
 * `#if ARK_BUS_TOKENIZER_ENABLED`.
 *
 * Contract details that callers trip over, stated once:
 *   - init() rejects an empty vocabulary, an entry whose text is empty or
 *     longer than ARK_BUS_TOKENIZER_TOKEN_MAX - 1 bytes, a negative id, a
 *     duplicate id and a duplicate text.  A vocabulary that cannot be
 *     inverted unambiguously is a bug in the table, not something to paper
 *     over at lookup time.
 *   - encode()/decode() write nothing on E_FULL/E_NOTFOUND and still report
 *     the size that would have been needed, so a caller can size its buffer
 *     instead of guessing.
 *   - decode() NUL-terminates the result; the returned length excludes that
 *     NUL, and the buffer must hold length + 1 bytes.
 *   - Cost is O(text_len * nvocab): the table is walked per position.  A few
 *     hundred entries on a 100-byte prompt is fine on an MCU; a 50k-entry
 *     vocabulary is not what this header is for (put a prefix tree in the
 *     application, or keep the big tokenizer on the peer that can afford it).
 */
#ifndef ARK_BUS_TOKENIZER_H
#define ARK_BUS_TOKENIZER_H

#include <stdint.h>
#include "ark_bus_config.h"
#include "ark_bus_types.h"

#if ARK_BUS_TOKENIZER_ENABLED

/* One vocabulary entry.  `text` is a NUL-terminated byte string (UTF-8 is the
 * caller's business; this layer compares bytes); `id` is what the inference
 * layer will see. */
typedef struct {
    const char *text;
    int32_t     id;
} ark_bus_token_t;

/* A tokenizer over a caller-owned table.  Copying the struct is fine: it holds
 * no state of its own, so one table can back several tokenizers. */
typedef struct {
    const ark_bus_token_t *vocab;
    uint16_t               nvocab;
    int32_t                unk_id;   /* emitted for bytes no entry matches */
} ark_bus_tokenizer_t;

/*
 * Bind a tokenizer to a table.  Returns ARK_BUS_OK, or ARK_BUS_E_INVAL for a
 * NULL argument, an empty table, or any table entry that breaks the contract
 * above.  On failure the tokenizer is left zeroed and unusable on purpose.
 */
int ark_bus_tokenizer_init(ark_bus_tokenizer_t *tok,
                           const ark_bus_token_t *vocab, uint16_t nvocab,
                           int32_t unk_id);

/*
 * Encode `len` bytes of `text` into at most `cap` ids.  Returns ARK_BUS_OK and
 * the id count in *nout; ARK_BUS_E_FULL when more than `cap` ids would be
 * needed (nothing is written, *nout is the count that was needed); ARK_BUS_E_INVAL
 * for a NULL argument or a tokenizer that failed init.  An empty input encodes
 * to zero ids and is not an error.
 *
 * `unk_id` is emitted as-is when no entry matches; it does not have to exist in
 * the table (a caller may use a reserved id), but if it does, decoding that id
 * gives the entry's text back.
 */
int ark_bus_tokenizer_encode(const ark_bus_tokenizer_t *tok,
                             const char *text, uint16_t len,
                             int32_t *out, uint16_t cap, uint16_t *nout);

/*
 * Decode `n` ids back into text.  Returns ARK_BUS_OK with the byte count in
 * *nout (excluding the NUL, which is written when it fits); ARK_BUS_E_NOTFOUND
 * when an id is not in the table (nothing written, *nout = 0); ARK_BUS_E_FULL
 * when the joined text plus its NUL does not fit `cap` (nothing written,
 * *nout is the size needed, excluding the NUL); ARK_BUS_E_INVAL for a NULL
 * argument.  An empty id list decodes to the empty string.
 */
int ark_bus_tokenizer_decode(const ark_bus_tokenizer_t *tok,
                             const int32_t *ids, uint16_t n,
                             char *out, uint16_t cap, uint16_t *nout);

/*
 * The text behind one id, or ARK_BUS_E_NOTFOUND when the table has no such id
 * (which is what the sampler's argmax needs before it can hand the model a
 * word).  *text is set to the entry's NUL-terminated string on success and to
 * NULL otherwise.
 */
int ark_bus_tokenizer_id_text(const ark_bus_tokenizer_t *tok, int32_t id,
                              const char **text);

#endif /* ARK_BUS_TOKENIZER_ENABLED */

#endif /* ARK_BUS_TOKENIZER_H */
