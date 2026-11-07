/* Ark C Compiler - opt.h
 * IR-level optimization passes: constant folding, dead code elimination.
 *
 * Runs after irgen and before backend code generation.
 * Guards:
 *   - Compile-time: #if ARKCC_FEATURE_OPT_CONSTFOLD / ARKCC_FEATURE_OPT_DCE
 *   - Runtime: g_config.enable_opt_constfold / g_config.enable_opt_dce / opt_level
 */
#ifndef ARK_OPT_H
#define ARK_OPT_H

#include "ir.h"

/* Run optimizations over the entire IR program.
 * level: 0=none, 1=basic (constfold + DCE), 2=aggressive (currently same as 1).
 * Each pass additionally guards on g_config.enable_opt_xxx. */
void opt_run(IrProgram *prog, int level);

/* Per-function constant folding: folds PUSH_INT + PUSH_INT + BINARY patterns. */
void opt_constfold_func(IrFunc *f);

/* Per-function dead code elimination: removes unreachable instructions
 * after RETURN/JMP up to the next label. */
void opt_dce_func(IrFunc *f);
/* 基本块级可达性 DCE（--dce-cfg）。语义与 opt_dce_func 相同：只删不可达指令。 */
void opt_dce_cfg_func(IrFunc *f);

/* Auto inlining: inlines small function bodies into call sites.
 * Threshold: IR instruction count <= ARK_INLINE_THRESHOLD (default 12).
 * Excludes: recursion, variadic, irq, naked, asm_func, >4 params. */
void opt_inline(IrProgram *prog);

/* Auto vectorization: recognizes simple counted loops (a[i]=b[i] +/- c[i])
 * and parallelizes them with SIMD instructions.
 * Only truly accelerates on the ARM backend; C backend falls back to scalar. */
void opt_vectorize(IrProgram *prog);

#endif /* ARK_OPT_H */
