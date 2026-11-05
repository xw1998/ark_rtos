/* Ark C Compiler - codegen common utilities
 * AsmBuffer: a lightweight dynamic byte buffer used by all backends
 * to accumulate emitted assembly or generated C source.
 */
#include "codegen.h"
#include "memory.h"

#define ASM_INIT_CAP 4096

void asmbuf_init(AsmBuffer *b) {
    b->cap = ASM_INIT_CAP;
    b->len = 0;
    b->buf = (char *)mem_pool_alloc(b->cap);
    b->buf[0] = 0;
}

static void asmbuf_grow(AsmBuffer *b, int extra) {
    if (b->len + extra + 1 > b->cap) {
        int nc = b->cap;
        while (b->len + extra + 1 > nc) nc *= 2;
        b->buf = (char *)mem_realloc(b->buf, b->cap, nc);
        b->cap = nc;
    }
}

void asmbuf_putc(AsmBuffer *b, char c) {
    asmbuf_grow(b, 1);
    b->buf[b->len++] = c;
    b->buf[b->len] = 0;
}

void asmbuf_puts(AsmBuffer *b, const char *s) {
    int n = ark_strlen(s);
    asmbuf_grow(b, n);
    {
        int i;
        for (i = 0; i < n; i++) b->buf[b->len++] = s[i];
    }
    b->buf[b->len] = 0;
}

void asmbuf_int(AsmBuffer *b, long long v) {
    char tmp[24];
    int neg = 0;
    int n = 0;
    int i;
    unsigned long long x;
    if (v < 0) { neg = 1; x = (unsigned long long)(-v); }
    else x = (unsigned long long)v;
    if (x == 0) tmp[n++] = '0';
    while (x) {
        tmp[n++] = (char)('0' + (int)(x % 10));
        x /= 10;
    }
    if (neg) asmbuf_putc(b, '-');
    for (i = n - 1; i >= 0; i--) asmbuf_putc(b, tmp[i]);
}

void asmbuf_printf_int(AsmBuffer *b, const char *prefix, int v, const char *suffix) {
    if (prefix) asmbuf_puts(b, prefix);
    asmbuf_int(b, v);
    if (suffix) asmbuf_puts(b, suffix);
}
