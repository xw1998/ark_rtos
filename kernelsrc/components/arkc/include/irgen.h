/* Ark C Compiler - irgen.h
 * Generate IR from AST.
 */
#ifndef ARK_IRGEN_H
#define ARK_IRGEN_H

#include "ast.h"
#include "ir.h"

IrProgram *irgen_program(AstNode *prog);

/* Number of AST constructs IR generation could not lower. Non-zero means the
 * emitted IR is incomplete, so the driver must fail the compilation instead of
 * writing an image that silently computes the wrong answer. Reset on entry to
 * irgen_program(). */
extern int g_irgen_errors;

/* 设置 IR 生成是否使用 8 字节槽模型（C 后端）。
 * 启用后 irgen_widen_to_64 和 irgen_cond_to_bool 成为 no-op，
 * 因为 8 字节 long long 槽无需符号扩展或高低字合并。 */
void irgen_set_64bit_slots(int enabled);

/* Number of stack words a _Complex double occupies on the selected backend.
 * The C backend keeps a complex value in one 8-byte union slot (1); the
 * machine backends (ARM / RISC-V / x86-64) carry four 32-bit words (4).
 * Set before irgen_program(). Used to size discard/reload/dup of complex
 * values and to reject complex arguments/returns where the calling
 * convention cannot carry them. */
void irgen_set_complex_words(int words);

#endif /* ARK_IRGEN_H */
