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

#endif /* ARK_SEMA_H */
