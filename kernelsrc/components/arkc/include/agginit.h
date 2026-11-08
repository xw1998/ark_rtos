/* Aggregate initialiser -> bytes.
 *
 * The IR carries an aggregate initialiser as C source text ("{1, 2, {3, 4}}"),
 * and turning that into data has to be done from the declared type: the text
 * alone cannot say where a nested object starts.  Both machine backends need
 * exactly that walk, so it lives here rather than in one of them -- two copies
 * would drift, and a drift means one target silently writes different bytes.
 *
 * Refusing is part of the contract: a shape that cannot be resolved into bytes
 * must fail, because writing the storage without its initialiser invents
 * zeroes, which is a wrong answer rather than a missing feature.
 */
#ifndef ARK_AGGINIT_H
#define ARK_AGGINIT_H

#include "type.h"

/* Fill `buf` (bufsz bytes, pre-zeroed by the caller: C zero-fills the tail of
 * an aggregate) with the object of type `t` described by `text`.  Returns 0
 * when the whole text was consumed, -1 when it holds something that is not
 * data this build can emit (a float, an address, a bitfield, a union, a
 * designator it does not understand, more initialisers than storage). */
int ark_agg_fill(Type *t, const char *text, unsigned char *buf, int bufsz);

/* Same walk, with a way to keep an address element as a symbol reference.
 * `addr_hook` is asked about every element that is not an integer constant and
 * whose type is a pointer: returning 1 means "the caller has recorded this
 * element and will place the address itself", and the filler leaves four zero
 * bytes there (the caller overwrites them).  Returning 0, or passing NULL,
 * keeps the old behaviour: the whole fill fails, because writing zeroes is a
 * wrong answer rather than a missing feature.
 *
 * The hook also cross-checks the two walks: the caller records offsets by
 * walking the AST against the same type, the filler reports them by walking
 * the text against that type.  An offset the caller did not record is refused
 * here instead of being stored somewhere else. */
int ark_agg_fill_ex(Type *t, const char *text, unsigned char *buf, int bufsz,
                    int (*addr_hook)(void *ud, int off, Type *t), void *ud);

/* A char array initialised by a string literal (irgen hands over the decoded
 * text, no quotes).  The rest of the array keeps the zero fill. */
void ark_agg_fill_cstr(const char *s, unsigned char *buf, int bufsz);

#endif /* ARK_AGGINIT_H */
