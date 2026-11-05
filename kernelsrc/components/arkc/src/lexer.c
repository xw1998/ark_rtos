/* Ark C Compiler - lexer
 * Lexical analyzer for the C99 source language.
 *
 * Features:
 *   - C99 keywords including _Bool/inline/restrict.
 *   - Numeric literals: octal 0755, hex 0xFF, decimal with suffixes u/U/l/L.
 *   - Floating literals: 1.5, 1.5e-3, .5, 1.5f.
 *   - Escape sequences in char/string literals: \n \t \r \\ \' \" \0 \xNN \NNN \a \b \f \v.
 *   - Wide char/string literals with the L prefix.
 *   - Line comments // and block comments.
 *   - Operators: . / -> / ...
 */
#include "lexer.h"
#include "diag.h"
#include "memory.h"
#include "platform.h"
#include "config.h"
#include "arkcc_config.h"
#include "type.h"

/* Display column of `p` inside the current line: one cell per ASCII byte, one
 * for a 2-byte UTF-8 sequence, two for a 3- or 4-byte one. That is exactly the
 * width diag.c's caret replay uses, so the caret lands on the character the
 * message names instead of walking past it on CJK lines. A tab counts as one
 * cell, because the caret line replays it as a tab.
 *
 * lexing only moves forward, so the walk is continued from the previous
 * position instead of restarting at the line start: a token costs the few
 * characters after its predecessor, not the whole line. */
static int lex_col(Lexer *lex, const char *p) {
    if (!lex->line_start || p < lex->line_start) return 0;
    if (!lex->col_p || lex->col_p < lex->line_start || lex->col_p > p) {
        lex->col_p = lex->line_start;
        lex->col_cells = 1;
    }
    while (lex->col_p < p) {
        unsigned char c = (unsigned char)*lex->col_p;
        if (c < 0x80)                { lex->col_cells += 1; lex->col_p += 1; }
        else if ((c & 0xE0) == 0xC0) { lex->col_cells += 1; lex->col_p += 2; }
        else if ((c & 0xF0) == 0xE0) { lex->col_cells += 2; lex->col_p += 3; }
        else if ((c & 0xF8) == 0xF0) { lex->col_cells += 2; lex->col_p += 4; }
        else                         { lex->col_cells += 1; lex->col_p += 1; }
    }
    return lex->col_cells;
}

static Token lexer_read_token(Lexer *lex);

/* Encode Unicode codepoint cp as UTF-8 into buf; returns byte length.
 * Caller must ensure buf has at least 4 bytes available. */
static int utf8_encode(unsigned int cp, char *buf) {
    if (cp < 0x80u) {
        buf[0] = (char)cp;
        return 1;
    } else if (cp < 0x800u) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000u) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

/* Wrapper around lexer_read_token: ensures every returned token has a
 * src_start so the parser can report errors correctly (especially __asm). */
static Token lexer_read_token_with_pos(Lexer *lex) {
    const char *p_before;
    Token t;
    /* Skip whitespace and comments before scanning the next token. */
    /* We do not let lexer_read_token do the skipping itself, because
     * lexer_read_token only sets src_start when it actually consumes bytes.
     * If lexer_read_token hits EOF right after skipping, the returned token's
     * src_start would be stale. By skipping here and capturing p_before, the
     * next token's src_start always points at the first byte of the token,
     * which is essential for inline-asm source-range tracking. */
    {
        /* Inline skip_ws_and_comments to advance p here. */
        while (*lex->p) {
            char c = *lex->p;
            if (c == ' ' || c == '\t' || c == '\r') lex->p++;
            else if (c == '\n') { lex->line++; lex->p++; lex->line_start = lex->p; }
            else if (c == '/' && lex->p[1] == '/') {
                while (*lex->p && *lex->p != '\n') lex->p++;
            } else if (c == '/' && lex->p[1] == '*') {
                lex->p += 2;
                while (*lex->p && !(lex->p[0]=='*' && lex->p[1]=='/')) {
                    if (*lex->p == '\n') { lex->line++; lex->line_start = lex->p + 1; }
                    lex->p++;
                }
                if (*lex->p) lex->p += 2;
            } else break;
        }
    }
    p_before = lex->p;
    t = lexer_read_token(lex);
    if (!t.src_start) t.src_start = p_before;
    /* Column of the token start, in display cells (see lex_col). */
    t.col = lex_col(lex, p_before);
    return t;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int is_oct(char c) { return c >= '0' && c <= '7'; }

/* Identifier rules: $ is allowed when --dollar is set; otherwise standard.
 * This matches ARMCC behavior where $ is a valid identifier character
 * (e.g. $count is a common symbol name). */
static int is_ident_start(char c) {
    if (is_alpha(c)) return 1;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    if (c == '$' && g_config.dollar_in_identifiers) return 1;
#else
    (void)c;
#endif
    return 0;
}
static int is_ident_char(char c) {
    if (is_alnum(c)) return 1;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    if (c == '$' && g_config.dollar_in_identifiers) return 1;
#else
    (void)c;
#endif
    return 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static Token make_token(TokenKind kind, int line) {
    Token t;
    t.kind = kind; t.value = 0; t.fvalue = 0.0; t.str = 0; t.line = line; t.col = 0;
    t.src_start = 0; t.is_imag = 0; t.is_float = 0;
    t.lit_unsigned = 0; t.lit_long = 0; t.lit_llong = 0;
    return t;
}

/* Consume an integer literal suffix (u/U, l/L, ll/LL, ul, llu, ...) and
 * report what was seen. C99 6.4.4.1: the suffix decides the literal's type. */
static void scan_int_suffix(Lexer *lex, int *uns, int *lng, int *llng) {
    int u = 0, l = 0, ll = 0;
    for (;;) {
        char ch = *lex->p;
        if (ch == 'u' || ch == 'U') { u = 1; lex->p++; }
        else if (ch == 'l' || ch == 'L') {
            if (lex->p[1] == 'l' || lex->p[1] == 'L') { ll = 1; lex->p += 2; }
            else { l = 1; lex->p++; }
        } else break;
    }
    if (uns)  *uns  = u;
    if (lng)  *lng  = l;
    if (llng) *llng = ll;
}

static Token make_number(long long val, int line) {
    Token t = make_token(TOK_NUM, line);
    t.value = val;
    return t;
}

static Token make_ident(const char *start, int len, int line) {
    Token t = make_token(TOK_IDENT, line);
    t.str = mem_strndup(start, len);
    return t;
}

/* Keyword table */
typedef struct { const char *s; TokenKind k; } Keyword;

static Keyword g_keywords[] = {
    /* C89 */
    {"int",      TOK_INT},
    {"char",     TOK_CHAR},
    {"void",     TOK_VOID},
    {"return",   TOK_RETURN},
    {"if",       TOK_IF},
    {"else",     TOK_ELSE},
    {"while",    TOK_WHILE},
    {"for",      TOK_FOR},
    {"break",    TOK_BREAK},
    {"continue", TOK_CONTINUE},
    {"do",       TOK_DO},
    /* C99 stdint keywords */
    {"short",    TOK_SHORT},
    {"long",     TOK_LONG},
    {"signed",   TOK_SIGNED},
    {"unsigned", TOK_UNSIGNED},
    {"float",    TOK_FLOAT},
    {"double",   TOK_DOUBLE},
    {"_Bool",    TOK_BOOL},
    {"const",    TOK_CONST},
    {"volatile", TOK_VOLATILE},
    {"restrict", TOK_RESTRICT},
    {"inline",   TOK_INLINE},
    {"static",   TOK_STATIC},
    {"extern",   TOK_EXTERN},
    {"auto",     TOK_AUTO},
    {"register", TOK_REGISTER},
    {"struct",   TOK_STRUCT},
    {"union",    TOK_UNION},
    {"enum",     TOK_ENUM},
    {"typedef",  TOK_TYPEDEF},
    {"sizeof",   TOK_SIZEOF},
    {"switch",   TOK_SWITCH},
    {"case",     TOK_CASE},
    {"default",  TOK_DEFAULT},
    {"goto",     TOK_GOTO},
    /* 0.3.0 GNU extended keywords */
    {"__attribute__", TOK_ATTRIBUTE},
    {"asm",           TOK_ASM},
    {"__asm",         TOK_ASM},
    {"__asm__",       TOK_ASM},
    {"__inline",      TOK_INLINE},
    {"__inline__",    TOK_INLINE},
    {"__volatile__",  TOK_VOLATILE},
    {"__const",       TOK_CONST},
    {"__restrict",    TOK_RESTRICT},
    {"__svc",         TOK_SVC},     /* ARM Compiler 5 SVC instruction keyword */
    {"__irq",         TOK_IRQ},     /* ARM Compiler 5 interrupt handler keyword */
    {"__weak",        TOK_WEAK},    /* ARM Compiler 5 weak symbol keyword */
    {"__naked",       TOK_NAKED},   /* ARM Compiler 5 naked function keyword */
    {"__packed",      TOK_PACKED_KW},  /* ARM Compiler 5 packed attribute keyword */
    {"__align",       TOK_ALIGN_KW},   /* ARM Compiler 5 alignment keyword */
    {"__at",          TOK_AT},         /* ARM Compiler 5 absolute address placement */
    {"__softfp",      TOK_SOFTFP},     /* ARM Compiler 5 software floating-point ABI */
    {"__forceinline", TOK_FORCEINLINE},/* ARM Compiler 5 force-inline keyword */
    /* B4: _Complex support (C99) */
    {"_Complex",      TOK_COMPLEX},
    {"__complex__",   TOK_COMPLEX},
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    /* 0.6.x: armcc 5 specific keywords; map to internal token kinds. */
    {"__value_in_regs", TOK_VALUE_IN_REGS},
    {"__thread",        TOK_THREAD},
    {"__global_reg",    TOK_GLOBAL_REG},
    {"__pure",          TOK_PURE},
    {"__interwork",     TOK_INTERWORK},
    {"__smc",           TOK_SMC},
    {"__int64",         TOK_INT64},
    {"__int32",         TOK_INT32},
    {"__int16",         TOK_INT16},
    {"__int8",          TOK_INT8},
    {"__typedef",       TOK_TYPEDEF_KW},
    {"__args",          TOK_ARGS},
    {"__return",        TOK_RETURN_KW},
    {"__tp_soft",       TOK_TP_SOFT},
    {"__nothrow",       TOK_NOTHROW},
    {"__deprecated",    TOK_DEPRECATED_KW},
    {"__may_alias",     TOK_MAY_ALIAS},
    {"__declspec",      TOK_DECLSPEC},   /* MSVC-style attribute keyword */
#endif
    /* __asm is treated as TOK_ASM; the parser handles the operand syntax for
     * both basic and extended asm forms, including armcc-style
     * __asm void fn(){...} naked function bodies. */
    {0, TOK_EOF}
};

static int kw_eq(const char *s, int len, const char *kw) {
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] != kw[i]) return 0;
        if (kw[i] == 0) return 0;
    }
    return kw[len] == 0;
}

static int check_keyword(const char *s, int len) {
    int i;
    for (i = 0; g_keywords[i].s; i++) {
        if (kw_eq(s, len, g_keywords[i].s)) return (int)g_keywords[i].k;
    }
    return -1;
}

static void skip_ws_and_comments(Lexer *lex) {
    while (*lex->p) {
        char c = *lex->p;
        if (c == ' ' || c == '\t' || c == '\r') {
            lex->p++;
        } else if (c == '\n') {
            lex->line++;
            lex->p++;
            lex->line_start = lex->p;
        } else if (c == '/' && lex->p[1] == '/') {
            while (*lex->p && *lex->p != '\n') lex->p++;
        } else if (c == '/' && lex->p[1] == '*') {
            lex->p += 2;
            while (*lex->p && !(lex->p[0]=='*' && lex->p[1]=='/')) {
                if (*lex->p == '\n') { lex->line++; lex->line_start = lex->p + 1; }
                lex->p++;
            }
            if (*lex->p) lex->p += 2;
        } else {
            break;
        }
    }
}

/* Parse escape sequences: \xNN, \NNN, \n, \t, etc. (also simple \uXXXX and
 * \UXXXXXXXX). We return the byte value to insert; the caller handles
 * multi-byte UTF-8 encoding if the source uses \u/\U UCN escapes.
 */
static int read_escape(const char **pp) {
    char e = **pp;
    int v;
    if (e == 'x') {
        (*pp)++;
        v = 0;
        while (is_hex(**pp)) { v = v * 16 + hex_val(**pp); (*pp)++; }
        return v & 0xFF;
    }
    if (e >= '0' && e <= '7') {
        int n = 0;
        v = 0;
        while (n < 3 && is_oct(**pp)) {
            v = v * 8 + (**pp - '0');
            (*pp)++;
            n++;
        }
        return v & 0xFF;
    }
    (*pp)++;
    switch (e) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case '\\': return '\\';
        case '\'': return '\'';
        case '"':  return '"';
        case '?':  return '?';
        case 'a':  return 7;
        case 'b':  return 8;
        case 'f':  return 12;
        case 'v':  return 11;
        case '0':  return 0;
        default:   return e;
    }
}

static Token lexer_read_token(Lexer *lex) {
    const char *start;
    int line;
    char c;
    Token tk;

    skip_ws_and_comments(lex);
    start = lex->p;
    line = lex->line;
    c = *lex->p;

    if (c == '\0') { tk = make_token(TOK_EOF, line); tk.src_start = start; return tk; }

#if ARKCC_FEATURE_WCHAR
    /* B2: Wide string literal L"..." or wide char literal L'...'.
     * The prefix L is a single identifier char, so we look ahead: if L is
     * followed by " or ', treat it as a wide literal and consume both.
     * Otherwise fall through to identifier scanning.
     */
    if (g_config.enable_wchar && c == 'L' &&
        (lex->p[1] == '"' || lex->p[1] == '\'')) {
        if (lex->p[1] == '"') {
            /* Wide string literal; emit TOK_STR with value=1 (wide flag). */
            Token t = make_token(TOK_STR, line);
            int cap = 32, len = 0;
            char *buf = (char *)mem_pool_alloc(cap);
            lex->p += 2; /* consume L" */
            while (*lex->p && *lex->p != '"') {
                int ch;
                if (*lex->p == '\\' && lex->p[1]) {
                    lex->p++;
                    ch = read_escape(&lex->p);
                } else {
                    ch = (unsigned char)*lex->p;
                    lex->p++;
                }
                if (len + 1 >= cap) {
                    int nc = cap * 2;
                    buf = (char *)mem_realloc(buf, cap, nc);
                    cap = nc;
                }
                buf[len++] = (char)ch;
            }
            if (*lex->p == '"') lex->p++;
            buf[len] = 0;
            t.str = buf;
            t.value = 1;  /* wide-string flag */
            return t;
        } else {
            /* Wide char literal L'...': C does not have a distinct token kind,
             * so we emit TOK_NUM with the codepoint value. */
            int val = 0;
            lex->p += 2; /* consume L' */
            if (*lex->p == '\\') {
                lex->p++;
                val = read_escape(&lex->p);
            } else if (*lex->p) {
                val = (unsigned char)*lex->p;
                lex->p++;
            }
            if (*lex->p == '\'') lex->p++;
            return make_number(val, line);
        }
    }
#endif

    /* Identifier / keyword scan, also handles C99 UCN escapes: \uXXXX \UXXXXXXXX. */
    if (is_ident_start(c) || (c == '\\' && (lex->p[1] == 'u' || lex->p[1] == 'U'))) {
        char *buf;
        int cap = 32, blen = 0;
        int kw;
        buf = (char *)mem_pool_alloc(cap);
        /* First char: is_ident_start already validated plain chars; only UCN
         * needs special handling for the first character. */
        if (c == '\\') {
            /* UCN will be processed in the loop below; nothing to add here. */
        } else {
            buf[blen++] = c;
            lex->p++;  /* consume first identifier character */
        }
        while (1) {
            char ch = *lex->p;
            if (is_ident_char(ch)) {
                if (blen + 1 >= cap) { int nc = cap * 2; buf = (char *)mem_realloc(buf, cap, nc); cap = nc; }
                buf[blen++] = ch;
                lex->p++;
            } else if (ch == '\\' && (lex->p[1] == 'u' || lex->p[1] == 'U')) {
                unsigned int cp = 0;
                int ndigits, i;
                char kind = lex->p[1];
                lex->p += 2;  /* consume \u or \U */
                ndigits = (kind == 'u') ? 4 : 8;
                for (i = 0; i < ndigits; i++) {
                    if (!is_hex(lex->p[i])) break;
                    cp = cp * 16 + (unsigned int)hex_val(lex->p[i]);
                }
                if (i == ndigits) {
                    lex->p += ndigits;
                    /* Encode the codepoint as UTF-8 into the buffer. */
                    if (cp < 0x80u) {
                        if (blen + 1 >= cap) { int nc = cap * 2; buf = (char *)mem_realloc(buf, cap, nc); cap = nc; }
                        buf[blen++] = (char)cp;
                    } else if (cp < 0x800u) {
                        if (blen + 2 >= cap) { int nc = cap * 2; buf = (char *)mem_realloc(buf, cap, nc); cap = nc; }
                        buf[blen++] = (char)(0xC0 | (cp >> 6));
                        buf[blen++] = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000u) {
                        if (blen + 3 >= cap) { int nc = cap * 2; buf = (char *)mem_realloc(buf, cap, nc); cap = nc; }
                        buf[blen++] = (char)(0xE0 | (cp >> 12));
                        buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[blen++] = (char)(0x80 | (cp & 0x3F));
                    } else {
                        if (blen + 4 >= cap) { int nc = cap * 2; buf = (char *)mem_realloc(buf, cap, nc); cap = nc; }
                        buf[blen++] = (char)(0xF0 | (cp >> 18));
                        buf[blen++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                        buf[blen++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[blen++] = (char)(0x80 | (cp & 0x3F));
                    }
                } else {
                    /* Malformed UCN: preserve the literal escape text. */
                    if (blen + 2 + i >= cap) { int nc = cap * 2 + 2 + i; buf = (char *)mem_realloc(buf, cap, nc); cap = nc; }
                    buf[blen++] = '\\';
                    buf[blen++] = kind;
                    { int j; for (j = 0; j < i; j++) buf[blen++] = lex->p[j]; }
                    lex->p += i;
                }
            } else {
                break;
            }
        }
        buf[blen] = 0;
        kw = check_keyword(buf, blen);
        if (kw >= 0) {
            Token t = make_token((TokenKind)kw, line);
            t.src_start = start;
            return t;
        }
        {
            Token t = make_token(TOK_IDENT, line);
            t.str = buf;
            t.src_start = start;
            return t;
        }
    }

    /* Numeric literal scan. */
    if (is_digit(c) || (c == '.' && is_digit(lex->p[1]))) {
        int is_float = 0;
        long long val = 0;
        double fval = 0.0;

        if (c == '0' && (lex->p[1] == 'x' || lex->p[1] == 'X')) {
            /* Hex literal: may be integer or hex float (C99 6.4.4.2). */
            const char *q;
            int is_hfloat = 0;
            long long ipart = 0;
            double fpart = 0.0, fscale = 1.0;
            int exp_neg = 0, exp_v = 0, has_exp = 0;
            double fval;
            lex->p += 2;
            while (is_hex(*lex->p)) {
                ipart = ipart * 16 + hex_val(*lex->p);
                lex->p++;
            }
            if (*lex->p == '.') {
                is_hfloat = 1;
                lex->p++;
                while (is_hex(*lex->p)) {
                    fpart = fpart * 16.0 + (double)hex_val(*lex->p);
                    fscale *= 16.0;
                    lex->p++;
                }
            }
            if (*lex->p == 'p' || *lex->p == 'P') {
                is_hfloat = 1;
                lex->p++;
                if (*lex->p == '+') lex->p++;
                else if (*lex->p == '-') { exp_neg = 1; lex->p++; }
                while (is_digit(*lex->p)) {
                    exp_v = exp_v * 10 + (*lex->p - '0');
                    lex->p++;
                }
                has_exp = 1;
            }
            if (is_hfloat) {
                double e = 1.0;
                int i;
                fval = (double)ipart + fpart / fscale;
                for (i = 0; i < exp_v; i++) e *= 2.0;
                if (has_exp) {
                    if (exp_neg) fval /= e; else fval *= e;
                }
                {
                    int is_float_suffix = 0;
                    while (*lex->p == 'f' || *lex->p == 'F' ||
                           *lex->p == 'l' || *lex->p == 'L') {
                        if (*lex->p == 'f' || *lex->p == 'F') is_float_suffix = 1;
                        lex->p++;
                    }
                    if (*lex->p == 'i' || *lex->p == 'I' ||
                        *lex->p == 'j' || *lex->p == 'J') {
                        lex->p++;
                        {
                            Token t = make_token(TOK_FCONST, line);
                            t.fvalue = fval;
                            t.value = (int)fval;
                            t.is_imag = 1;
                            t.is_float = is_float_suffix;
                            return t;
                        }
                    }
                    {
                        Token t = make_token(TOK_FCONST, line);
                        t.fvalue = fval;
                        t.value = (int)fval;
                        t.is_float = is_float_suffix;
                        return t;
                    }
                }
            }
            val = ipart;
            /* Consume integer suffix u/U/l/L/ll (recorded on the token). */
            {
                int u = 0, l = 0, ll = 0;
                Token t;
                scan_int_suffix(lex, &u, &l, &ll);
                t = make_number(val, line);
                t.lit_unsigned = u; t.lit_long = l; t.lit_llong = ll;
                return t;
            }
        }

        if (c == '0' && is_oct(lex->p[1])) {
            /* Octal literal. */
            lex->p++;
            while (is_oct(*lex->p)) {
                val = val * 8 + (*lex->p - '0');
                lex->p++;
            }
            {
                int u = 0, l = 0, ll = 0;
                Token t;
                scan_int_suffix(lex, &u, &l, &ll);
                t = make_number(val, line);
                t.lit_unsigned = u; t.lit_long = l; t.lit_llong = ll;
                return t;
            }
        }

        /* Decimal integer literal. */
        while (is_digit(*lex->p)) {
            val = val * 10 + (*lex->p - '0');
            lex->p++;
        }
        if (*lex->p == '.') {
            is_float = 1;
            lex->p++;
            while (is_digit(*lex->p)) lex->p++;
        }
        if (*lex->p == 'e' || *lex->p == 'E') {
            is_float = 1;
            lex->p++;
            if (*lex->p == '+' || *lex->p == '-') lex->p++;
            while (is_digit(*lex->p)) lex->p++;
        }
        if (is_float) {
            /* Parse decimal float by re-scanning from start. */
            const char *q = start;
            double sign = 1.0;
            double intp = 0.0, fracp = 0.0, frac_scale = 1.0;
            int has_exp = 0, exp_neg = 0, exp_v = 0;
            int i;
            if (*q == '-') { sign = -1.0; q++; }
            while (is_digit(*q)) { intp = intp * 10.0 + (*q - '0'); q++; }
            if (*q == '.') {
                q++;
                while (is_digit(*q)) {
                    fracp = fracp * 10.0 + (*q - '0');
                    frac_scale *= 10.0;
                    q++;
                }
            }
            if (*q == 'e' || *q == 'E') {
                has_exp = 1;
                q++;
                if (*q == '-') { exp_neg = 1; q++; }
                else if (*q == '+') q++;
                while (is_digit(*q)) { exp_v = exp_v * 10 + (*q - '0'); q++; }
            }
            fval = sign * (intp + fracp / frac_scale);
            if (has_exp) {
                /* C99 requires correct rounding for subnormal range values
                 * (e.g. 1e-320). Direct integer pow of 10^exp is unreliable
                 * at extreme magnitudes:
                 *   10^320 overflows to +inf, so fval/inf = 0;
                 *   pow(10,-320) may return a subnormal value.
                 * Use host pow() to inherit its subnormal handling.
                 *
                 * 宿主环境使用 strtod 进行正确舍入的浮点解析。
                 * 手动计算 mantissa * pow(10, exp) 在 DBL_MAX
                 * (1.7976931348623157e308) 等极端值时会因中间舍入溢出为 inf。
                 * strtod 由宿主 libc 提供正确舍入，能精确解析所有 double 字面量。 */
#if !defined(ARKCC_STM32)
                {
                    char lit[64];
                    int lit_len = (int)(lex->p - start);
                    extern double strtod(const char *, char **);
                    if (lit_len >= (int)sizeof(lit)) lit_len = (int)sizeof(lit) - 1;
                    { int ii; for (ii = 0; ii < lit_len; ii++) lit[ii] = start[ii]; }
                    lit[lit_len] = 0;
                    fval = strtod(lit, 0);
                }
#else
                /* STM32 freestanding 环境：手动计算（对极端大数可能不够精确） */
                {
                    double e;
                    extern double pow(double, double);
                    e = pow(10.0, (double)(exp_neg ? -exp_v : exp_v));
                    fval *= e;
                }
#endif
            }
            {
                int is_float_suffix = 0;
                while (*lex->p == 'f' || *lex->p == 'F' ||
                       *lex->p == 'l' || *lex->p == 'L') {
                    if (*lex->p == 'f' || *lex->p == 'F') is_float_suffix = 1;
                    lex->p++;
                }
                /* C99 imaginary suffix: the i/I/j/J suffix sets is_imag=1.
                 * For example, 1.0i becomes the pure imaginary 0+1i literal,
                 * which the parser converts into a TYPE_COMPLEX AST_NUM node. */
                if (*lex->p == 'i' || *lex->p == 'I' ||
                    *lex->p == 'j' || *lex->p == 'J') {
                    lex->p++;
                    {
                        Token t = make_token(TOK_FCONST, line);
                        t.fvalue = fval;
                        t.value = (int)fval;
                        t.is_imag = 1;
                        t.is_float = is_float_suffix;
                        return t;
                    }
                }
                {
                    Token t = make_token(TOK_FCONST, line);
                    t.fvalue = fval;
                    t.value = (int)fval;
                    t.is_float = is_float_suffix;
                    return t;
                }
            }
        }
        /* Consume integer suffix u/U/l/L/ll (recorded on the token). */
        {
            int u = 0, l = 0, ll = 0;
            Token t;
            scan_int_suffix(lex, &u, &l, &ll);
            t = make_number(val, line);
            t.lit_unsigned = u; t.lit_long = l; t.lit_llong = ll;
            return t;
        }
    }

    /* Character literal. */
    if (c == '\'') {
        int val = 0;
        lex->p++;
        if (*lex->p == '\\') {
            lex->p++;
            val = read_escape(&lex->p);
        } else if (*lex->p) {
            val = (unsigned char)*lex->p;
            lex->p++;
        }
        if (*lex->p == '\'') lex->p++;
        return make_number(val, line);
    }

    /* String literal; adjacent "..." segments are concatenated per C99. */
    if (c == '"') {
        Token t = make_token(TOK_STR, line);
        int cap = 32, len = 0;
        char *buf = (char *)mem_pool_alloc(cap);
        for (;;) {
            lex->p++; /* consume opening quote " */
            while (*lex->p && *lex->p != '"') {
                int ch;
                if (*lex->p == '\\' && lex->p[1]) {
                    /* C99 UCN: \uXXXX \UXXXXXXXX is encoded as UTF-8 bytes. */
                    if (lex->p[1] == 'u' || lex->p[1] == 'U') {
                        unsigned int cp = 0;
                        int ndigits, i;
                        char kind = lex->p[1];
                        char ub[4];
                        int ulen;
                        lex->p += 2;
                        ndigits = (kind == 'u') ? 4 : 8;
                        for (i = 0; i < ndigits; i++) {
                            if (!is_hex(lex->p[i])) break;
                            cp = cp * 16 + (unsigned int)hex_val(lex->p[i]);
                        }
                        if (i == ndigits) {
                            lex->p += ndigits;
                            ulen = utf8_encode(cp, ub);
                            if (len + ulen + 1 >= cap) {
                                int nc = cap * 2;
                                while (len + ulen + 1 >= nc) nc *= 2;
                                buf = (char *)mem_realloc(buf, cap, nc);
                                cap = nc;
                            }
                            for (i = 0; i < ulen; i++) buf[len++] = ub[i];
                            continue;
                        }
                        /* Malformed UCN: emit the literal 'u' or 'U' byte. */
                        ch = (int)kind;
                        lex->p++;  /* consume the 'u' or 'U' */
                    } else {
                        lex->p++;
                        ch = read_escape(&lex->p);
                    }
                } else {
                    ch = (unsigned char)*lex->p;
                    lex->p++;
                }
                if (len + 1 >= cap) {
                    int nc = cap * 2;
                    buf = (char *)mem_realloc(buf, cap, nc);
                    cap = nc;
                }
                buf[len++] = (char)ch;
            }
            if (*lex->p == '"') lex->p++;
            /* Skip whitespace between adjacent string literals (concatenation). */
            {
                const char *q = lex->p;
                while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') {
                    if (*q == '\n') { lex->line++; lex->line_start = q + 1; }
                    q++;
                }
                if (*q == '"') {
                    lex->p = q;
                    continue;
                }
            }
            break;
        }
        buf[len] = 0;
        t.str = buf;
        return t;
    }

    lex->p++;
    switch (c) {
        case '(': return make_token(TOK_LPAREN, line);
        case ')': return make_token(TOK_RPAREN, line);
        case '{': return make_token(TOK_LBRACE, line);
        case '}': return make_token(TOK_RBRACE, line);
        case '[': return make_token(TOK_LBRACKET, line);
        case ']': return make_token(TOK_RBRACKET, line);
        case ';': return make_token(TOK_SEMICOLON, line);
        case ',': return make_token(TOK_COMMA, line);
        case '?': return make_token(TOK_QUESTION, line);
        case ':': return make_token(TOK_COLON, line);
        case '#': return make_token(TOK_HASH, line);
        case '~': return make_token(TOK_BNOT, line);
        case '.':
            if (lex->p[0] == '.' && lex->p[1] == '.') {
                lex->p += 2;
                return make_token(TOK_ELLIPSIS, line);
            }
            return make_token(TOK_DOT, line);
        case '^':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_XOR_ASSIGN, line); }
            return make_token(TOK_BXOR, line);
        case '+':
            if (*lex->p == '+') { lex->p++; return make_token(TOK_INC, line); }
            if (*lex->p == '=') { lex->p++; return make_token(TOK_ADD_ASSIGN, line); }
            return make_token(TOK_PLUS, line);
        case '-':
            if (*lex->p == '-') { lex->p++; return make_token(TOK_DEC, line); }
            if (*lex->p == '=') { lex->p++; return make_token(TOK_SUB_ASSIGN, line); }
            if (*lex->p == '>') { lex->p++; return make_token(TOK_ARROW, line); }
            return make_token(TOK_MINUS, line);
        case '*':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_MUL_ASSIGN, line); }
            return make_token(TOK_MUL, line);
        case '/':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_DIV_ASSIGN, line); }
            return make_token(TOK_DIV, line);
        case '%':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_MOD_ASSIGN, line); }
            return make_token(TOK_MOD, line);
        case '=':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_EQ, line); }
            return make_token(TOK_ASSIGN, line);
        case '!':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_NE, line); }
            return make_token(TOK_NOT, line);
        case '<':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_LE, line); }
            if (*lex->p == '<') {
                lex->p++;
                if (*lex->p == '=') { lex->p++; return make_token(TOK_SHL_ASSIGN, line); }
                return make_token(TOK_SHL, line);
            }
            return make_token(TOK_LT, line);
        case '>':
            if (*lex->p == '=') { lex->p++; return make_token(TOK_GE, line); }
            if (*lex->p == '>') {
                lex->p++;
                if (*lex->p == '=') { lex->p++; return make_token(TOK_SHR_ASSIGN, line); }
                return make_token(TOK_SHR, line);
            }
            return make_token(TOK_GT, line);
        case '&':
            if (*lex->p == '&') { lex->p++; return make_token(TOK_AND, line); }
            if (*lex->p == '=') { lex->p++; return make_token(TOK_AND_ASSIGN, line); }
            return make_token(TOK_BAND, line);
        case '|':
            if (*lex->p == '|') { lex->p++; return make_token(TOK_OR, line); }
            if (*lex->p == '=') { lex->p++; return make_token(TOK_OR_ASSIGN, line); }
            return make_token(TOK_BOR, line);
        default:
            /* armcc reports this as #7 "unrecognized token" with the caret on
             * the offending character and no extra detail, so we do the same
             * instead of leaking a hex code. */
            {
                DiagLoc loc;
                loc.file = 0;
                loc.line = line;
                loc.col = lex_col(lex, lex->p);
                diag_report(7, DIAG_SEV_ERROR, &loc, 0, 0, 0);
            }
            return make_token(TOK_EOF, line);
    }
}

void lexer_init(Lexer *lex, const char *input) {
    lex->input = input;
    lex->p = input;
    lex->line = 1;
    lex->line_start = input;
    lex->last_line = 0;
    lex->col_p = 0;
    lex->col_cells = 1;
    lex->current = lexer_read_token_with_pos(lex);
    lex->ahead = lexer_read_token_with_pos(lex);
}

Token lexer_peek(Lexer *lex)  { return lex->current; }
Token lexer_peek2(Lexer *lex) { return lex->ahead; }

Token lexer_next(Lexer *lex) {
    Token t = lex->current;
    lex->current = lex->ahead;
    lex->ahead = lexer_read_token_with_pos(lex);
    if (t.kind != TOK_EOF && t.line > 0) lex->last_line = t.line;
    return t;
}

void lexer_advance(Lexer *lex) {
    (void)lexer_next(lex);
}

/* Spelling of the token a caller expected, so the message reads like
 * "expected a \";\"" instead of exposing the TokenKind enum. */
static const char *expect_name(TokenKind k) {
    switch (k) {
        case TOK_SEMICOLON: return "a \";\"";
        case TOK_RPAREN:    return "a \")\"";
        case TOK_RBRACE:    return "a \"}\"";
        case TOK_RBRACKET:  return "a \"]\"";
        case TOK_LBRACE:    return "a \"{\"";
        case TOK_LPAREN:    return "a \"(\"";
        case TOK_IDENT:     return "an identifier";
        default:            return "a token";
    }
}

/* armcc codes for the "expected a X" family, measured one by one:
 *   int a[2;        -> #17 expected a "]"
 *   f(1, 2;         -> #18 expected a ")"
 *   int a = ;       -> #29 expected an expression
 *   missing ;       -> #65 expected a ";"
 *   unclosed {      -> #67 expected a "}"  (positioned at the EOF token)
 * `{`/`(` and identifiers have no measured code, so they keep the arkc
 * wording. */
static int expect_code(TokenKind k) {
    switch (k) {
        case TOK_SEMICOLON: return 65;
        case TOK_RPAREN:    return 18;
        case TOK_RBRACE:    return 67;
        case TOK_RBRACKET:  return 17;
        default:            return DIAG_NO_CODE;
    }
}

int lexer_expect(Lexer *lex, TokenKind kind) {
    if (lex->current.kind != kind) {
        DiagLoc loc;
        int code;
        loc.file = 0;
        loc.line = lex->current.line;
        loc.col  = lex->current.col;
        code = expect_code(kind);
        if (lex->current.kind == TOK_EOF) {
            /* Nothing left to point at: armcc anchors these at the line of the
             * last real token and adds "At end of source" instead of echoing
             * the line. */
            /* armcc anchors the message on the line the final newline
             * terminates, which is one before the EOF token (measured on
             * 1-, 3- and 5-line files). */
            if (loc.line > 1) {
                loc.line--;
            } else if (lex->last_line > 0) {
                loc.line = lex->last_line;
            }
            loc.col = 0;
            if (code >= 0) diag_report_at_eof(code, DIAG_SEV_ERROR, &loc);
            else diag_report_msg(DIAG_SEV_ERROR, &loc, "expected %s",
                                 expect_name(kind), 0, 0);
            return 0;
        }
        if (code >= 0) {
            diag_report(code, DIAG_SEV_ERROR, &loc, 0, 0, 0);
        } else {
            diag_report_msg(DIAG_SEV_ERROR, &loc, "expected %s",
                            expect_name(kind), 0, 0);
        }
        return 0;
    }
    lexer_advance(lex);
    return 1;
}

const char *token_kind_name(TokenKind k) {
    switch (k) {
        case TOK_EOF: return "EOF";
        case TOK_IDENT: return "ident";
        case TOK_NUM: return "num";
        case TOK_STR: return "str";
        case TOK_FCONST: return "fconst";
        default: return "tok";
    }
}
