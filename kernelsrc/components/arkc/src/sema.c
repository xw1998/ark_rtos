/* Ark C Compiler - sema
 * Semantic analysis: undefined / redefinition checks and basic type inference.
 * Relaxed vs the original: > 4 parameters, variadic declarations, typedefs ignored, full node walk.
 */
#include "sema.h"
#include "symtab.h"
#include "memory.h"
#include "platform.h"
#include "common.h"
#include "diag.h"

static int g_errors;

/* Forward declarations */
static void sema_decl(AstNode *node);
static void sema_stmt(AstNode *node);
static Type *sema_expr(AstNode *node);
static AstNode *sema_implicit_cast(AstNode *node, Type *target);

/* Diagnostics raised here carry an armcc code when one has been measured for
 * the condition (see src/diag_msg_table.c), and arkc's own wording otherwise.
 * Columns are not tracked on AST nodes yet, so IDE-style output for these
 * omits the column (registered limitation, docs/DIAGNOSTICS.md). */
static DiagLoc loc_of(const AstNode *node) {
    DiagLoc loc;
    loc.file = 0;
    loc.line = node ? node->line : 0;
    loc.col  = node ? node->col : 0;
    return loc;
}

/* Report a semantic error using an armcc code, with `a0` filling its
 * placeholder (usually the offending identifier). */
/* armcc's %n is a *kind-qualified* name: it expands to e.g. `function "g" `
 * (trailing space included, which is why the printed text has two spaces
 * before "has"). Composing that here keeps the generated template word-for-word
 * armcc's instead of inventing a parallel wording. Single-threaded, used
 * immediately, so one buffer is enough. */
static const char *qual_name(const char *kind, const char *name) {
    static char buf[192];
    int n = 0, i = 0;
    for (i = 0; kind[i] && n < (int)sizeof(buf) - 4; i++) buf[n++] = kind[i];
    if (n < (int)sizeof(buf) - 4) buf[n++] = ' ';
    if (n < (int)sizeof(buf) - 4) buf[n++] = '"';
    for (i = 0; name[i] && n < (int)sizeof(buf) - 4; i++) buf[n++] = name[i];
    if (n < (int)sizeof(buf) - 4) buf[n++] = '"';
    if (n < (int)sizeof(buf) - 4) buf[n++] = ' ';
    buf[n] = 0;
    return buf;
}

static void err_code(AstNode *node, int code, const char *a0) {
    DiagLoc loc = loc_of(node);
    diag_report(code, DIAG_SEV_ERROR, &loc, a0, 0, 0);
    g_errors++;
}

/* Integer promotion (C99 6.3.1.1): char/short/_Bool/enum become int.
 * int can represent every value of those types (including uchar/ushort), so the
 * result is always signed int; int/long/llong are left unchanged. */
static Type *promote_integer(Type *t) {
    if (!t) return type_int();
    switch (t->kind) {
        case TYPE_CHAR:
        case TYPE_SHORT:
        case TYPE_BOOL:
        case TYPE_ENUM:
            return type_int();
        default:
            return t;
    }
}

/* Integer rank (C99 6.3.1.1), used by the usual arithmetic conversions to compare
 * operand widths. Assumes sizeof(long)==sizeof(int)==4 (ARM and Windows LLP64):
 *   int/unsigned int       -> 1
 *   long/unsigned long     -> 2
 *   long long/unsigned ll  -> 3
 * char/short/bool/enum are promoted to int by promote_integer first, so rank = 1. */
static int type_rank(Type *t) {
    if (!t) return 1;
    switch (t->kind) {
        case TYPE_LLONG: return 3;
        case TYPE_LONG:  return 2;
        default:         return 1;  /* int, or a narrow type after promotion */
    }
}

/* usual arithmetic conversions (C99 6.3.1.8):
 *   1. Floating point wins: double > float. Either side double -> double,
 *      otherwise either side float -> float.
 *   2. Otherwise both operands get integer promotion (char/short/_Bool/enum -> int).
 *   3. Same kind and same signedness: return it unchanged.
 *   4. Same rank: the unsigned type wins (unsigned int + int -> unsigned int).
 *   5. Different rank:
 *      - if the higher-rank type can represent every value of the lower one
 *        (higher unsigned, or higher signed but strictly wider), take it;
 *      - otherwise take the unsigned version of the higher-rank type. */
Type *usual_arith_conv(Type *lt, Type *rt) {
    /* complex wins over every real type; _Complex maps to _Complex double */
    if (lt && lt->kind == TYPE_COMPLEX) return type_complex(type_double());
    if (rt && rt->kind == TYPE_COMPLEX) return type_complex(type_double());
    /* Step 1: floating point first */
    if (lt && lt->kind == TYPE_DOUBLE) return type_double();
    if (rt && rt->kind == TYPE_DOUBLE) return type_double();
    if (lt && lt->kind == TYPE_FLOAT)  return type_float();
    if (rt && rt->kind == TYPE_FLOAT)  return type_float();

    /* Step 2: integer promotion */
    lt = promote_integer(lt);
    rt = promote_integer(rt);
    if (!lt) return rt ? rt : type_int();
    if (!rt) return lt;

    /* Step 3: identical types */
    if (lt->kind == rt->kind && lt->is_unsigned == rt->is_unsigned) return lt;

    {
        int rl = type_rank(lt);
        int rr = type_rank(rt);
        Type *hi, *lo;   /* higher rank / lower rank */
        int hi_uns, lo_uns;

        if (rl >= rr) { hi = lt; lo = rt; }
        else          { hi = rt; lo = lt; }
        hi_uns = hi->is_unsigned;
        lo_uns = lo->is_unsigned;

        /* Step 4: same rank -- same signedness already returned in step 3, so the
         * pair is one signed and one unsigned; the unsigned type wins (C99 6.3.1.8).
         * Note hi may be the signed one, hence the hi_uns || lo_uns test. */
        if (rl == rr) {
            int take_uns = (hi_uns || lo_uns);
            switch (hi->kind) {
                case TYPE_LLONG: return take_uns ? type_ullong() : type_llong();
                case TYPE_LONG:  return take_uns ? type_ulong()  : type_long();
                default:         return take_uns ? type_uint()   : type_int();
            }
        }

        /* Step 5: different rank -- can the higher rank represent all lower values?
         * Judge by byte width, not by rank: int=long=4B and llong=8B here, so
         * long (rank 2) and int (rank 1) are the same width; a higher rank is not
         * necessarily wider.
         *   - higher unsigned: always represents every lower value -> take it;
         *   - higher signed + lower signed: take the higher rank;
         *   - higher signed + lower unsigned: only when the higher rank is strictly wider (llong 8B vs uint 4B), else unsigned higher. */
        if (hi_uns || !lo_uns) {
            /* Higher unsigned always wins; or lower signed (fits) -> take the higher type */
            return hi;
        }
        {
            int hi_bytes = (hi->kind == TYPE_LLONG) ? 8 : 4;
            int lo_bytes = (lo->kind == TYPE_LLONG) ? 8 : 4;
            if (hi_bytes > lo_bytes) {
                /* Higher rank is wider (llong 8B vs uint 4B): signed can hold all unsigned lower values */
                return hi;
            }
            /* Same width: higher signed cannot hold the lower unsigned range -> unsigned higher */
            switch (hi->kind) {
                case TYPE_LLONG: return type_ullong();
                case TYPE_LONG:  return type_ulong();
                default:         return type_uint();
            }
        }
    }
}

/* Bug-I1: Determine whether a cast from src to dst would change the IR
 * representation (i.e., is not a no-op on the target machine).
 * Returns 1 if an explicit AST_CAST node must be inserted, 0 otherwise.
 * - float <-> int:   always needs a cast (IR_I2F / IR_F2I)
 * - int(32b) <-> ll(64b): needs a cast (widen / narrow stack slots)
 * - same-size int <-> int: no-op (signedness change is represented in type only)
 * - float <-> double:  not handled yet (irgen lacks float<->double promotion);
 *   skip cast so current float arithmetic keeps working.
 * - non-arithmetic types: no-op (pointer/struct/etc. casts are address passes) */
static int cast_changes_repr(Type *src, Type *dst) {
    int src_float, dst_float;
    if (!src || !dst) return 0;
    if (!type_is_arith(src) || !type_is_arith(dst)) return 0;
    src_float = (src->kind == TYPE_FLOAT || src->kind == TYPE_DOUBLE);
    dst_float = (dst->kind == TYPE_FLOAT || dst->kind == TYPE_DOUBLE);
    if (src_float && !dst_float) return 1;        /* float -> int */
    if (!src_float && dst_float) return 1;        /* int -> float */
    if (src_float && dst_float) return 0;          /* float <-> double: skip */
    /* both are integer types: cast only if size differs */
    return (type_size(src) != type_size(dst));
}

/* Bug-I1: Insert an implicit AST_CAST wrapping `node` to convert it to
 * `target` type, following C99 6.3 (assignment / usual arithmetic
 * conversions). Returns `node` unchanged when no actual conversion is
 * needed so that downstream IR is not polluted by redundant cast nodes. */
static AstNode *sema_implicit_cast(AstNode *node, Type *target) {
    if (!node || !target) return node;
    if (!node->type) return node;
    if (!cast_changes_repr(node->type, target)) return node;
    return ast_cast(target, node, node->line);
}

int eval_const_expr(AstNode *node, long long *out_value) {
    long long lv, rv;
    if (!node || !out_value) return 0;
    switch (node->kind) {
        case AST_NUM:
            /* Floating point constants are not folded here. */
            if (node->type && (node->type->kind == TYPE_FLOAT || node->type->kind == TYPE_DOUBLE))
                return 0;
            *out_value = node->u.num_val;
            return 1;
        case AST_SIZEOF:
            *out_value = node->u.num_val;
            return 1;
        case AST_CAST:
            /* cast: the value is passed through (no truncation here) */
            return eval_const_expr(node->u.unary.operand, out_value);
        case AST_VAR: {
            Symbol *s = sym_find(node->u.name);
            if (s && s->kind == SYM_ENUM_CONST) {
                *out_value = s->enum_value;
                return 1;
            }
            return 0;
        }
        case AST_UNARY:
            if (!eval_const_expr(node->u.unary.operand, &lv)) return 0;
            switch (node->u.unary.op) {
                case (int)TOK_MINUS: *out_value = -lv; return 1;
                case (int)TOK_PLUS:  *out_value =  lv; return 1;
                case (int)TOK_BNOT:  *out_value = ~lv; return 1;
                case (int)TOK_NOT:   *out_value = !lv; return 1;
                default: return 0;
            }
        case AST_BINARY:
            if (!eval_const_expr(node->u.binary.left,  &lv)) return 0;
            if (!eval_const_expr(node->u.binary.right, &rv)) return 0;
            switch (node->u.binary.op) {
                case (int)TOK_PLUS:  *out_value = lv +  rv; return 1;
                case (int)TOK_MINUS: *out_value = lv -  rv; return 1;
                case (int)TOK_MUL:   *out_value = lv *  rv; return 1;
                case (int)TOK_DIV:   if (rv == 0) return 0; *out_value = lv / rv; return 1;
                case (int)TOK_MOD:   if (rv == 0) return 0; *out_value = lv % rv; return 1;
                case (int)TOK_BAND:  *out_value = lv &  rv; return 1;
                case (int)TOK_BOR:   *out_value = lv |  rv; return 1;
                case (int)TOK_BXOR:  *out_value = lv ^  rv; return 1;
                case (int)TOK_SHL:   *out_value = lv << rv; return 1;
                case (int)TOK_SHR:   *out_value = lv >> rv; return 1;
                case (int)TOK_EQ:    *out_value = lv == rv; return 1;
                case (int)TOK_NE:    *out_value = lv != rv; return 1;
                case (int)TOK_LT:    *out_value = lv <  rv; return 1;
                case (int)TOK_GT:    *out_value = lv >  rv; return 1;
                case (int)TOK_LE:    *out_value = lv <= rv; return 1;
                case (int)TOK_GE:    *out_value = lv >= rv; return 1;
                case (int)TOK_AND:   *out_value = lv && rv; return 1;
                case (int)TOK_OR:    *out_value = lv || rv; return 1;
                default: return 0;
            }
        case AST_TERNARY: {
            long long cv;
            if (!eval_const_expr(node->u.ternary.cond, &cv)) return 0;
            if (cv) return eval_const_expr(node->u.ternary.true_expr,  out_value);
            else    return eval_const_expr(node->u.ternary.false_expr, out_value);
        }
        default:
            return 0;
    }
}

int sema_check(AstNode *prog) {
    int i;
    g_errors = 0;
    sym_init();
    if (!prog || prog->kind != AST_PROGRAM) return 0;
    /* Builtins: pre-register the simple host libc functions so they are not reported as undefined */
    {
        const char *builtins[] = {
            "printf","putchar","puts","strlen","strcmp","strcpy",
            "memset","memcpy","abort","malloc","free",0
        };
        int k;
        for (k = 0; builtins[k]; k++) {
            Symbol *s = sym_add(SYM_FUNC, builtins[k], type_int(), 0);
            s->param_count = -1; /* unknown */
            s->is_variadic = 1;
        }
    }
    for (i = 0; i < prog->u.program.decl_count; i++) {
        sema_decl(prog->u.program.decls[i]);
    }
    return g_errors;
}

static void sema_func(AstNode *node) {
    char *name = node->u.func.name;
    Symbol *existing = sym_find_current(name);
    Symbol *s;
    int i;
    if (existing) {
        if (existing->kind != SYM_FUNC) {
            err_code(node, 101, name);
            return;
        }
        if (existing->is_defined && node->u.func.body) {
            /* armcc __weak redefinition rule: a weak definition can be
             * overridden by a later non-weak definition of the same name.
             * Multiple weak definitions are allowed (first wins). A weak
             * redefinition of an already-defined non-weak function is
             * silently ignored. Two non-weak definitions remain an error. */
            if (!existing->is_weak && !node->u.func.is_weak) {
                DiagLoc loc = loc_of(node);
                diag_report(247, DIAG_SEV_ERROR, &loc,
                            qual_name("function", name), 0, 0);
                g_errors++;
                return;
            }
            if (!existing->is_weak && node->u.func.is_weak) {
                /* Existing non-weak, new weak: ignore new weak definition. */
                return;
            }
            if (existing->is_weak && !node->u.func.is_weak) {
                /* Override weak definition: clear is_defined/is_weak so the
                 * new non-weak body will be installed below as the canonical
                 * definition. irgen skips the discarded weak AST node by
                 * detecting symbol.is_weak != ast.is_weak. */
                existing->is_defined = 0;
                existing->is_weak = 0;
            } else {
                /* Both weak: ignore the new definition (first wins). */
                return;
            }
        }
        s = existing;
    } else {
        s = sym_add(SYM_FUNC, name, node->u.func.ret_type, 0);
    }
    s->param_count = node->u.func.param_count;
    s->is_variadic = node->u.func.is_variadic;
    /* Store parameter types so call sites can do 64-bit arg promotion
     * (int -> long long) based on the PARAMETER type, not the arg type.
     * Each params[i] is an AST_VAR_DECL whose u.var.var_type is the
     * parameter's declared type. */
    if (node->u.func.param_count > 0 && node->u.func.params) {
        s->param_types = (Type **)mem_pool_alloc(sizeof(Type *) * node->u.func.param_count);
        for (i = 0; i < node->u.func.param_count; i++) {
            AstNode *pp = node->u.func.params[i];
            s->param_types[i] = (pp && pp->u.var.var_type) ? pp->u.var.var_type : 0;
        }
    }
    s->svc_num = node->u.func.svc_num;
    /* Propagate the ARM Compiler 5 function attributes into the symbol table so irgen
     * can copy them onto the IrFunc. OR is used so a declaration that carries the
     * attributes keeps them when the definition itself carries none. */
    if (node->u.func.is_irq)   s->is_irq   = 1;
    if (node->u.func.is_weak)  s->is_weak  = 1;
    if (node->u.func.is_naked) s->is_naked = 1;
    if (node->u.func.is_used)  s->is_used  = 1;
    if (node->u.func.is_unused) s->is_unused = 1;
    if (node->u.func.is_asm_func) s->is_asm_func = 1;
    if (node->u.func.forceinline) s->forceinline = 1;
    if (node->u.func.alias_target) s->alias_target = node->u.func.alias_target;
    /* Write back: the AST node also gets the merged attributes so irgen can use d->u.func.* */
    node->u.func.is_irq   = s->is_irq;
    node->u.func.is_weak  = s->is_weak;
    node->u.func.is_naked = s->is_naked;
    node->u.func.is_used  = s->is_used;
    node->u.func.is_unused = s->is_unused;
    node->u.func.forceinline = s->forceinline;
    if (!node->u.func.alias_target) node->u.func.alias_target = s->alias_target;
    if (node->u.func.body || node->u.func.is_asm_func ||
        node->u.func.alias_target) {
        s->is_defined = 1;
    }
    if (node->u.func.is_asm_func) {
        /* __asm function body: no body-wide semantic checks */
        return;
    }

    if (node->u.func.body) {
        sym_enter_scope();
        for (i = 0; i < node->u.func.param_count; i++) {
            AstNode *pp = node->u.func.params[i];
            if (sym_find_current(pp->u.var.name)) {
                err_code(pp, 100, pp->u.var.name);
            } else {
                Symbol *psym = sym_add(SYM_PARAM, pp->u.var.name, pp->u.var.var_type, 0);
                (void)psym;
            }
        }
        sema_stmt(node->u.func.body);
        sym_leave_scope();
    }
}

static void sema_decl(AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case AST_FUNC_DEF:
        case AST_FUNC_DECL:
        case AST_ASM_FUNC:
            sema_func(node);
            break;
        case AST_VAR_DECL: {
            char *name = node->u.var.name;
            Symbol *existing;
            if (!name) return;
            existing = sym_find_current(name);
            if (existing) {
                /* Parser pre-registers globals as SYM_VAR at parse time so
                 * sizeof(var)/sizeof(var[i]) can be resolved. Upgrade it to
                 * SYM_GLOBAL here (the canonical kind for downstream irgen). */
                if (existing->kind == SYM_VAR) {
                    existing->kind = SYM_GLOBAL;
                    existing->is_extern = node->u.var.is_extern;
                    node->u.var.is_global = 1;
                    if (node->u.var.init) {
                        sema_expr(node->u.var.init);
                        /* Bug-I1: Apply initialization conversion (C99 6.7.8):
                         * cast the initializer to the variable's type. */
                        node->u.var.init =
                            sema_implicit_cast(node->u.var.init,
                                               node->u.var.var_type);
                    }
                    break;
                }
                /* Allow extern declaration after any prior declaration (redundant but legal).
                 * Allow definition after extern declaration (standard header/source pattern).
                 * Only error on actual redefinition (two non-extern definitions). */
                if (node->u.var.is_extern) return;
                if (existing->is_extern) return;
                err_code(node, 101, name);
                return;
            }
            {
                Symbol *sym = sym_add(SYM_GLOBAL, name, node->u.var.var_type, 0);
                sym->is_extern = node->u.var.is_extern;
            }
            node->u.var.is_global = 1;
            if (node->u.var.init) {
                sema_expr(node->u.var.init);
                /* Bug-I1: Apply initialization conversion (C99 6.7.8):
                 * cast the initializer to the variable's type. */
                node->u.var.init =
                    sema_implicit_cast(node->u.var.init,
                                       node->u.var.var_type);
            }
            break;
        }
        case AST_TYPEDEF:
            if (node->u.var.name)
                sym_add(SYM_TYPEDEF, node->u.var.name, node->u.var.var_type, 0);
            break;
        case AST_BLOCK: {
            int i;
            for (i = 0; i < node->u.block.stmt_count; i++)
                sema_decl(node->u.block.stmts[i]);
            break;
        }
        default:
            /* Nothing to do for this node kind. */
            break;
    }
}

static void sema_stmt(AstNode *node) {
    if (!node) return;
    switch (node->kind) {
        case AST_BLOCK: {
            int i;
            /* Declarations inside the block live in their own scope. */
            if (!node->u.block.is_decl_list) sym_enter_scope();
            for (i = 0; i < node->u.block.stmt_count; i++) {
                sema_stmt(node->u.block.stmts[i]);
            }
            if (!node->u.block.is_decl_list) sym_leave_scope();
            break;
        }
        case AST_VAR_DECL: {
            char *name = node->u.var.name;
            if (!name) break;
            if (sym_find_current(name)) {
                err_code(node, 101, name);
            } else {
                sym_add(SYM_VAR, name, node->u.var.var_type, 0);
            }
            if (node->u.var.init) {
                sema_expr(node->u.var.init);
                /* Bug-I1: Apply initialization conversion (C99 6.7.8):
                 * cast the initializer to the variable's type. */
                node->u.var.init =
                    sema_implicit_cast(node->u.var.init,
                                       node->u.var.var_type);
            }
            break;
        }
        case AST_TYPEDEF:
            if (node->u.var.name)
                sym_add(SYM_TYPEDEF, node->u.var.name, node->u.var.var_type, 0);
            break;
        case AST_IF:
            sema_expr(node->u.if_stmt.cond);
            sema_stmt(node->u.if_stmt.then_stmt);
            if (node->u.if_stmt.else_stmt) sema_stmt(node->u.if_stmt.else_stmt);
            break;
        case AST_WHILE:
            sema_expr(node->u.while_stmt.cond);
            sema_stmt(node->u.while_stmt.body);
            break;
        case AST_FOR:
            sym_enter_scope();
            if (node->u.for_stmt.init) {
                if (node->u.for_stmt.init->kind == AST_VAR_DECL ||
                    node->u.for_stmt.init->kind == AST_BLOCK)
                    sema_stmt(node->u.for_stmt.init);
                else
                    sema_expr(node->u.for_stmt.init);
            }
            if (node->u.for_stmt.cond) sema_expr(node->u.for_stmt.cond);
            if (node->u.for_stmt.step) sema_expr(node->u.for_stmt.step);
            sema_stmt(node->u.for_stmt.body);
            sym_leave_scope();
            break;
        case AST_DO_WHILE:
            sema_stmt(node->u.while_stmt.body);
            sema_expr(node->u.while_stmt.cond);
            break;
        case AST_SWITCH:
            sema_expr(node->u.switch_stmt.expr);
            sema_stmt(node->u.switch_stmt.body);
            break;
        case AST_CASE:
        case AST_DEFAULT:
            sema_stmt(node->u.case_stmt.stmt);
            break;
        case AST_LABEL:
            sema_stmt(node->u.label_stmt.stmt);
            break;
        case AST_GOTO:
            /* goto: targets are resolved by the backend, nothing to check here */
            break;
        case AST_RETURN:
            if (node->u.unary.operand) sema_expr(node->u.unary.operand);
            break;
        case AST_BREAK:
        case AST_CONTINUE:
            break;
        case AST_EXPR_STMT:
            if (node->u.unary.operand) sema_expr(node->u.unary.operand);
            break;
        case AST_ASM:
            /* asm statements need no semantic analysis */
            break;
        default:
            sema_expr(node);
            break;
    }
}

static Type *sema_expr(AstNode *node) {
    if (!node) return type_int();
    switch (node->kind) {
        case AST_NUM:
            if (!node->type) node->type = type_int();
            return node->type;
        case AST_STR:
            if (!node->type) node->type = type_pointer(type_char());
            return node->type;
        case AST_SIZEOF:
            node->type = type_int();
            return node->type;
        case AST_INIT_LIST: {
            int i;
            for (i = 0; i < node->u.init_list.item_count; i++) {
                AstNode *it = node->u.init_list.items[i];
                if (it && it->u.init_field.value) sema_expr(it->u.init_field.value);
            }
            node->type = type_int();
            return node->type;
        }
        case AST_ASM:
            node->type = type_void();
            return node->type;
        case AST_VAR: {
            Symbol *s = sym_find(node->u.name);
            if (!s) {
                err_code(node, 20, node->u.name);
                node->type = type_int();
                return node->type;
            }
            node->type = s->type;
            return node->type;
        }
        case AST_BINARY:
        case AST_COMMA: {
            Type *t = type_int();
            sema_expr(node->u.binary.left);
            sema_expr(node->u.binary.right);
            if (node->kind == AST_COMMA) {
                if (node->u.binary.right) t = node->u.binary.right->type ? node->u.binary.right->type : type_int();
            } else {
                int op = node->u.binary.op;
                int is_cmp = (op == (int)TOK_EQ || op == (int)TOK_NE ||
                              op == (int)TOK_LT || op == (int)TOK_GT ||
                              op == (int)TOK_LE || op == (int)TOK_GE ||
                              op == (int)TOK_AND || op == (int)TOK_OR);
                Type *lt = node->u.binary.left  ? node->u.binary.left->type  : 0;
                Type *rt = node->u.binary.right ? node->u.binary.right->type : 0;
                Type *common = usual_arith_conv(lt, rt);
                if (!is_cmp) {
                    t = common;
                    /* C99 6.5.6: the result of pointer arithmetic is a pointer
                     * (`ptr +/- int`, `int + ptr`) or ptrdiff_t (`ptr - ptr`),
                     * not the usual-arithmetic-conversion type. Without this a
                     * chained expression such as `(p + 3) - p` loses the
                     * pointer type and the element-size scaling never runs. */
                    {
                        int lp = lt && (lt->kind == TYPE_PTR || lt->kind == TYPE_ARRAY);
                        int rp = rt && (rt->kind == TYPE_PTR || rt->kind == TYPE_ARRAY);
                        if (op == (int)TOK_PLUS || op == (int)TOK_MINUS) {
                            if (lp && rp) t = type_int();
                            else if (lp)
                                t = (lt->kind == TYPE_ARRAY) ? type_pointer(lt->base) : lt;
                        }
                    }
                }
                /* Bug-I1: Cast both operands to the common type so irgen sees
                 * consistent operand types and emits proper IR_I2F / IR_F2I /
                 * 64-bit widen instructions. Only for arithmetic operands;
                 * pointer arithmetic (ptr + int) is left untouched. */
                if (common && lt && rt &&
                    type_is_arith(lt) && type_is_arith(rt)) {
                    node->u.binary.left  = sema_implicit_cast(node->u.binary.left,  common);
                    node->u.binary.right = sema_implicit_cast(node->u.binary.right, common);
                }
            }
            node->type = t;
            return node->type;
        }
        case AST_UNARY: {
            Type *t = sema_expr(node->u.unary.operand);
            if (node->u.unary.op == (int)TOK_BAND) {
                node->type = type_pointer(t);
            } else if (node->u.unary.op == (int)TOK_MUL) {
                if (t && t->kind == TYPE_PTR && t->is_func_ptr) {
                    /* *fp where fp points to a function: the designator decays
                     * straight back to the same pointer type (C99 6.5.3.2p4). */
                    node->type = t;
                } else if (t && (t->kind == TYPE_PTR || t->kind == TYPE_ARRAY))
                    node->type = t->base;
                else
                    node->type = type_int();
            } else {
                node->type = t ? t : type_int();
            }
            return node->type;
        }
        case AST_CAST:
            sema_expr(node->u.unary.operand);
            return node->type;
        case AST_ASSIGN:
            sema_expr(node->u.binary.left);
            sema_expr(node->u.binary.right);
            /* Bug-I1: Apply assignment conversion (C99 6.5.16.1): cast RHS
             * to LHS type when the two differ meaningfully (e.g. int -> double,
             * int -> long long). Same-size int<->int is a no-op cast. */
            if (node->u.binary.left && node->u.binary.right) {
                Type *lt = node->u.binary.left->type;
                node->u.binary.right = sema_implicit_cast(node->u.binary.right, lt);
            }
            node->type = node->u.binary.left->type;
            return node->type;
        case AST_FUNC_CALL: {
            Symbol *s = sym_find(node->u.call.name);
            int i;
            /* The parser only turns syntactic non-identifier callees into
             * indirect calls (name "?indirect"). `fp(x)` where fp is a
             * function pointer *variable* still arrives as a direct call
             * named "fp", and sym_find returns the variable, not a function.
             * Rewrite it to the indirect form here (the same shape the parser
             * builds for `(*fp)(x)`), otherwise irgen's __smc/__svc matcher
             * sees a symbol and mis-emits IR_SMC for the call. */
            if (s && s->kind != SYM_FUNC && !node->u.call.callee) {
                AstNode *callee = ast_var(node->u.call.name, node->line);
                callee->col = node->col;
                node->u.call.callee = callee;
                node->u.call.name = mem_strdup("?indirect");
                s = 0;
            }
            if (!s || s->kind != SYM_FUNC) {
                /* Indirect call (callee is a function pointer expression):
                 * must sema the callee so member offsets/types are resolved.
                 * Direct calls to unknown functions: just sema the args. */
                if (node->u.call.callee) {
                    Type *ct = sema_expr(node->u.call.callee);
                    /* Use callee's return type if it's a function pointer */
                    if (ct && ct->kind == TYPE_PTR && ct->base &&
                        ct->base->kind == TYPE_FUNC) {
                        node->type = ct->base->base;
                    } else if (ct && ct->kind == TYPE_PTR && ct->is_func_ptr) {
                        /* (*fp)(...): return type of the pointed-to function is
                         * not tracked for function-pointer declarators, so the
                         * call result defaults to int (as for any unprototyped
                         * call). */
                        node->type = ct->base ? ct->base : type_int();
                    } else if (ct && ct->kind == TYPE_FUNC) {
                        /* callee written as (*fp): the designator decays to
                         * the function type itself; the return type is base. */
                        node->type = ct->base;
                    } else {
                        node->type = type_int();
                    }
                } else {
                    node->type = type_int();
                }
                for (i = 0; i < node->u.call.arg_count; i++)
                    sema_expr(node->u.call.args[i]);
                return node->type;
            }
            for (i = 0; i < node->u.call.arg_count; i++) {
                sema_expr(node->u.call.args[i]);
                /* Bug-I1: Apply argument conversion (C99 6.5.2.2): cast each
                 * argument to the corresponding parameter type when the
                 * function has a prototype and the arg index is within the
                 * named parameter list. Variadic args beyond the named list
                 * get default argument promotion instead. */
                if (s->param_types && i < s->param_count) {
                    node->u.call.args[i] =
                        sema_implicit_cast(node->u.call.args[i],
                                           s->param_types[i]);
                }
            }
            if (s->param_count >= 0 && !s->is_variadic &&
                s->param_count != node->u.call.arg_count) {
                /* armcc splits this by direction: #165 for too few, #140 for
                 * too many (both measured, see docs/DIAGNOSTICS.md). */
                DiagLoc wloc = loc_of(node);
                if (node->u.call.arg_count < s->param_count) {
                    diag_report(165, DIAG_SEV_ERROR, &wloc, 0, 0, 0);
                } else {
                    /* armcc points #140 at the first argument that has no
                     * parameter, not at the call. */
                    const AstNode *extra = node->u.call.args[s->param_count];
                    if (extra && extra->col > 0) wloc = loc_of(extra);
                    diag_report(140, DIAG_SEV_ERROR, &wloc, 0, 0, 0);
                }
            }
            node->type = s->type;
            return node->type;
        }
        case AST_ARRAY_ACCESS: {
            Type *t = sema_expr(node->u.array_access.array);
            sema_expr(node->u.array_access.index);
            if (!t || (t->kind != TYPE_ARRAY && t->kind != TYPE_PTR)) {
                /* Not an array or pointer: fall back to int so later passes can continue. */
                node->type = type_int();
                return node->type;
            }
            node->type = t->base;
            return node->type;
        }
        case AST_MEMBER: {
            Type *t = sema_expr(node->u.member.obj);
            Type *st = t;
            StructField *f;
            if (node->u.member.is_arrow && t && t->kind == TYPE_PTR) st = t->base;
            f = type_find_field(st, node->u.member.member);
            if (f) {
                node->u.member.offset = f->offset;
                node->type = f->type;
            } else {
                node->type = type_int();
            }
            return node->type;
        }
        case AST_TERNARY: {
            Type *tt, *ft, *common;
            sema_expr(node->u.ternary.cond);
            tt = sema_expr(node->u.ternary.true_expr);
            ft = sema_expr(node->u.ternary.false_expr);
            /* Bug-I1 (B6): C99 6.5.15 - the result type is the common type
             * of the two branches after usual arithmetic conversions. Cast
             * both branches to the common type so irgen produces consistent
             * code. */
            common = usual_arith_conv(tt, ft);
            if (common && tt && ft &&
                type_is_arith(tt) && type_is_arith(ft)) {
                node->u.ternary.true_expr  = sema_implicit_cast(node->u.ternary.true_expr,  common);
                node->u.ternary.false_expr = sema_implicit_cast(node->u.ternary.false_expr, common);
                tt = common;
            }
            node->type = tt;
            return node->type;
        }
        default:
            node->type = type_int();
            return node->type;
    }
}
