/* Ark C Compiler - codegen.h
 * Common back-end interface shared by all code generators.
 * Wraps AsmBuffer so codegen_program and the backends stay independent.
 */
#ifndef ARK_CODEGEN_H
#define ARK_CODEGEN_H

#include "ir.h"

typedef struct {
    char *buf;
    int   len;
    int   cap;
} AsmBuffer;

void asmbuf_init(AsmBuffer *b);
void asmbuf_putc(AsmBuffer *b, char c);
void asmbuf_puts(AsmBuffer *b, const char *s);
void asmbuf_int(AsmBuffer *b, long long v);
void asmbuf_printf_int(AsmBuffer *b, const char *prefix, int v, const char *suffix);

#endif /* ARK_CODEGEN_H */
