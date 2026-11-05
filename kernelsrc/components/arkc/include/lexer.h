/* Ark C Compiler - lexer.h
 * Lexical analysis: turns the source text into a Token stream.
 */
#ifndef ARK_LEXER_H
#define ARK_LEXER_H

#include "common.h"

typedef struct {
    TokenKind kind;
    long long       value;
    double    fvalue;
    char     *str;
    int       line;
    int       col;          /* 1-based display column of the token start, for
                             * IDE-style diagnostics and the caret. 0 = unknown
                             * (tokens not produced by lexer_next). */
    const char *src_start;  /* start of the token in the source; used for __asm bodies */
    int       is_imag;      /* C99 imaginary literal (i/I/j/J suffix) */
    int       is_float;     /* C99 float suffix (f/F): 1 = float literal, 0 = double */
    /* Integer literal suffix flags (C99 6.4.4.1). Dropping them made
     * `-1 < 1u` compare as signed and gave 0xFFFFFFFFu / 1u<<31 the wrong
     * type, so the lexer now records them and the parser types the literal. */
    int       lit_unsigned;
    int       lit_long;
    int       lit_llong;
} Token;

typedef struct {
    const char *input;
    const char *p;
    int         line;
    const char *line_start; /* first byte of the current source line; the
                             * column of a token is start - line_start + 1 */
    Token       current;
    Token       ahead;
    int         last_line;  /* line of the last non-EOF token; an end-of-source
                             * diagnostic is anchored there (armcc does the
                             * same, the EOF token itself sits one line later
                             * whenever the file ends with a newline) */
    const char *col_p;      /* incremental column cursor (see lex_col) */
    int         col_cells;
} Lexer;

void  lexer_init(Lexer *lex, const char *input);
Token lexer_next(Lexer *lex);
Token lexer_peek(Lexer *lex);
Token lexer_peek2(Lexer *lex);
void  lexer_advance(Lexer *lex);
int   lexer_expect(Lexer *lex, TokenKind kind);

const char *token_kind_name(TokenKind k);

#endif /* ARK_LEXER_H */
