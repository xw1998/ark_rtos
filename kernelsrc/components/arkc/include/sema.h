/* Ark C Compiler - sema.h
 * Semantic analysis: redeclaration / undefined checks and basic type inference.
 */
#ifndef ARK_SEMA_H
#define ARK_SEMA_H

#include "ast.h"

int sema_check(AstNode *prog);

/* Fold a constant expression: returns 1 and writes the value through
 * *out_value on success, 0 otherwise. Handles unary -/~/!, binary arithmetic/
 * shift/compare/bitwise, ?:, enum constants and result types on AST_CAST. */
int eval_const_expr(AstNode *node, long long *out_value);

/* usual arithmetic conversions (C99 6.3.1.8):
 * Integer promotion first, then the common-type rules, applied to binary ops.
 * Promotion turns char/short/_Bool/enum into int, after which rank decides.
 * Here sizeof(long)==sizeof(int)==4, so long and int share a rank
 * (ARM and Windows LLP64); the result follows signedness then rank. */
Type *usual_arith_conv(Type *lt, Type *rt);

/* 浮点常量表达式求值（C99 6.6）。eval_const_expr 的值域是 long long，装不下
 * double，因此浮点初始化器一直求不出值。返回 1 表示 node 是可在编译期算出的
 * 浮点（或整数）常量表达式，值写入 *out。除零不折叠（UB，不得发明结果）。 */
int eval_const_fexpr(AstNode *node, double *out);

/* _Complex 常量表达式求值（C99 6.6）：实部写 *re、虚部写 *im。返回 1 表示
 * node 可在编译期算出。整数/浮点/虚数字面量、+ - * /、一元正负、以及到
 * _Complex float / _Complex double 的显式转换都在范围内；除零不折叠。 */
int eval_const_cexpr(AstNode *node, double *re, double *im);

/* 浮点常量 -> 整数类型，按 C99 6.3.1.4p1 向零截断；越界/NaN/Inf 返回 0
 * （UB 不折叠，不发明值）。只在 ARKCC_FEATURE_FCONST_FOLD 打开时才有定义，
 * 调用点同样在 #if 里。 */
int fconst_to_int(double d, Type *t, long long *out);

#endif /* ARK_SEMA_H */
