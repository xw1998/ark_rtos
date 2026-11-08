/* Ark C Compiler - irgen
 * AST -> stack-based IR.
 *
 * Calling convention (coordinated with the backends):
 *   - Arguments 0..N-1 are PUSHed onto the virtual stack before the call;
 *     IR_CALL n makes the backend take the top n values into the matching
 *     registers / stack arguments, in the order arg0..argN-1.
 *   - String literals are referenced through the string pool + IR_PUSH_STR.
 *   - Globals are referenced through IR_PUSH_GLOBAL / IR_STORE_GLOBAL.
 */
#include "irgen.h"
#include "symtab.h"
#include "memory.h"
#include "platform.h"
#include "common.h"
#include "sema.h"
#include "diag.h"
#include "config.h"
#include "arkcc_config.h"
/* Note: <stdio.h> intentionally NOT included to avoid pulling in semihosting-
 * dependent snprintf on STM32. sb_putf below uses a custom dtoa instead. */

/* 1 (default): a variadic function reserves the C backend's 8 x 8-byte
 * argument slot region at frame+8000.  The driver clears this for ARM Thumb-2
 * and RV32I, whose regions are built from the caller's own argument words
 * (see include/ir.h and <stdarg.h>). */
int g_ir_va_slot_region = 1;

/* See include/ir.h.  0 by default, so every target except ARM Thumb-2 rejects
 * a >8-byte composite through `...` instead of emitting code that reads the
 * wrong words; the driver raises it for ARM (src/compiler.c). */
int g_ir_vararg_words = 0;

/* IR_CALL argument sizes that the argmask cannot express live in the IR
 * program's own table (ir_program_add_argbytes) -- not in a module global: the
 * driver resets the memory pool between translation units, which on the board
 * left a global pointer reading another unit's AST ("passes 900035835 argument
 * words" from `cc --pack`, three sources in one process). */

/* Internal buffer used to serialize an AST initializer list into a C source
 * snippet. 64 KB is enough for ordinary examples. */
typedef struct StrBuf {
    char *data;
    int   len;
    int   cap;
} StrBuf;

static void sb_init(StrBuf *b) { b->data = (char *)mem_pool_alloc(256); b->len = 0; b->cap = 256; }
static void sb_putc(StrBuf *b, char c) {
    if (b->len + 1 >= b->cap) {
        int nc = b->cap * 2;
        b->data = (char *)mem_realloc(b->data, b->cap, nc);
        b->cap = nc;
    }
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}
static void sb_puts(StrBuf *b, const char *s) { while (*s) sb_putc(b, *s++); }
static void sb_putd(StrBuf *b, long long v) {
    char tmp[24]; int n = 0; unsigned long long u; int neg = 0; int i;
    if (v < 0) { neg = 1; u = -(unsigned long long)v; } else u = (unsigned long long)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + u % 10); u /= 10; }
    if (neg) sb_putc(b, '-');
    for (i = n - 1; i >= 0; i--) sb_putc(b, tmp[i]);
}
static void sb_putf(StrBuf *b, double v) {
    /* Convert a double to a C floating literal. Scientific notation is used for
     * very large or very small magnitudes, fixed point otherwise. */
    /* 自定义 double 转字符串，避免使用 snprintf（STM32 半主机依赖）。
     * 大数（v >= 1e16）和极小值（v < 1e-4）使用科学记数法，
     * 正常范围使用定点表示，保证输出为合法 C 浮点字面量。
     * 并非完全可往返，但二进制值在 init_float 中单独保存用于代码生成。 */
    char tmp[80];
    int n = 0;
    if (v != v) { sb_puts(b, "nan"); return; }
    /* 无穷大检测：inf - inf = NaN，NaN != 0.0。
     * 不能用 v > DBL_MAX 比较，因为 -O2 下 x87 FPU 的 80 位扩展精度
     * 可能导致误判（DBL_MAX 被误判为 >DBL_MAX）。 */
    if (v - v != 0.0) { sb_puts(b, "inf"); return; }
    if (v < 0) { sb_putc(b, '-'); v = -v; }
    if (v == 0.0) { sb_puts(b, "0.0"); return; }
    /* 大数或极小值使用科学记数法 */
    if (v >= 1e16 || v < 1e-4) {
        int exp = 0;
        int i, d;
        /* 规范化到 1 <= v < 10，记录指数。
         * 用 10 的幂表大步长归一化，减少迭代次数以降低累积舍入误差
         * （逐次乘除 10 会因 300+ 次迭代累积误差，导致 1e-300 往返失败） */
        {
            static const double p10[] = {1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256};
            static const int sh[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
            int bi;
            for (bi = 8; bi >= 0; bi--) {
                while (v >= p10[bi]) { v /= p10[bi]; exp += sh[bi]; }
            }
            while (v >= 10.0) { v /= 10.0; exp++; }
            for (bi = 8; bi >= 0; bi--) {
                while (v < 1.0 && v * p10[bi] < 10.0) { v *= p10[bi]; exp -= sh[bi]; }
            }
            while (v < 1.0) { v *= 10.0; exp--; }
        }
        /* 尾数整数部分（1 位） */
        d = (int)v;
        if (d > 9) d = 9;
        tmp[n++] = (char)('0' + d);
        v -= (double)d;
        /* 小数点 + 17 位小数 */
        tmp[n++] = '.';
        for (i = 0; i < 17; i++) {
            v *= 10.0;
            d = (int)v;
            if (d > 9) d = 9;
            if (d < 0) d = 0;
            tmp[n++] = (char)('0' + d);
            v -= (double)d;
        }
        /* 去掉末尾多余的 0，保证至少有一位小数 */
        while (n > 0 && tmp[n-1] == '0') n--;
        if (tmp[n-1] == '.') tmp[n++] = '0';
        /* 指数部分：e+NN 或 e-NN（至少 2 位，不足补 0） */
        tmp[n++] = 'e';
        if (exp >= 0) tmp[n++] = '+';
        else { tmp[n++] = '-'; exp = -exp; }
        if (exp < 10) { tmp[n++] = '0'; tmp[n++] = (char)('0' + exp); }
        else {
            char ebuf[8]; int k = 0;
            while (exp > 0) { ebuf[k++] = (char)('0' + exp % 10); exp /= 10; }
            while (k > 0) tmp[n++] = ebuf[--k];
        }
        tmp[n] = 0;
        sb_puts(b, tmp);
        return;
    }
    /* 定点表示：1e-4 <= v < 1e16，保持原有输出方式 */
    {
        long long ip = (long long)v;
        double frac = v - (double)ip;
        if (ip == 0) tmp[n++] = '0';
        else {
            char ibuf[24]; int k = 0;
            while (ip > 0) { ibuf[k++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
            while (k > 0) tmp[n++] = ibuf[--k];
        }
        if (frac > 0) {
            int i;
            tmp[n++] = '.';
            for (i = 0; i < 17 && frac > 0; i++) {
                int d;
                frac *= 10.0;
                d = (int)frac;
                if (d > 9) d = 9;
                tmp[n++] = (char)('0' + d);
                frac -= (double)d;
            }
            while (n > 0 && tmp[n-1] == '0') n--;
            if (tmp[n-1] == '.') tmp[n++] = '0';
        } else {
            tmp[n++] = '.';
            tmp[n++] = '0';
        }
    }
    tmp[n] = 0;
    sb_puts(b, tmp);
}
static void sb_putstr_lit(StrBuf *b, const char *s) {
    sb_putc(b, '"');
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        switch (c) {
            case '\n': sb_puts(b, "\\n"); break;
            case '\t': sb_puts(b, "\\t"); break;
            case '\r': sb_puts(b, "\\r"); break;
            case '"':  sb_puts(b, "\\\""); break;
            case '\\': sb_puts(b, "\\\\"); break;
            default:
                if (c < 0x20 || c >= 0x7F) {
                    char buf[8];
                    const char *hex = "0123456789abcdef";
                    buf[0] = '\\'; buf[1] = 'x';
                    buf[2] = hex[(c >> 4) & 0xF];
                    buf[3] = hex[c & 0xF];
                    buf[4] = 0;
                    sb_puts(b, buf);
                } else sb_putc(b, (char)c);
                break;
        }
    }
    sb_putc(b, '"');
}

static void serialize_const_expr(StrBuf *b, AstNode *n);

/* Set when serialize_const_expr meets a node it cannot write as C source.
 * The callers that would otherwise hand "0" to a backend as an initialiser
 * check it and report an error: an initialiser that is not a constant
 * expression must not be invented as zero. */
static int g_ser_unknown = 0;

/* ---- address constants (C11 6.6p9) --------------------------------------
 *
 * `int (*gp)(int,int) = add;`, `int *pg = &gv;` and `int *pe = &arr[2];` are
 * initialisers whose value is an *address*, not a byte pattern.  A machine
 * backend cannot bake one: the address of a function is only known after the
 * linker has placed it.  So the IR carries (symbol, byte offset) and each
 * backend emits a symbol reference instead of a number.
 *
 * The two walkers below decode an expression into that triple.  They accept
 * exactly the forms the standard allows for a static initialiser and return 0
 * for everything else, so the caller keeps its "not a constant" path.  Both
 * used to be missing entirely: the old code dropped the '&' on the way to the
 * C-source serialiser and wrote the operand as data, so `int *pe = &arr[2];`
 * became a null pointer with no diagnostic at all.
 *
 * `isf` says whether the expression denotes a *function*: arithmetic on a
 * function designator is a constraint violation (C11 6.5.6p2), and a function
 * has no stride, so the walkers have to be able to tell `add` from a pointer
 * variable holding add.
 */

/* Stride of pointer arithmetic on an address constant: an array strides by its
 * element, anything else by its own size.  `t` is the type of the *object* the
 * address points at, not the type of the address: `&gv` yields int, so
 * `&gv + 1` moves by sizeof(int). */
static long long addr_elem_size(Type *t) {
    long long n;
    if (!t) return 1;
    if (t->kind == TYPE_FUNC) return 0;
    if (t->kind == TYPE_ARRAY) t = t->base ? t->base : t;
    n = type_size(t);
    return n > 0 ? n : 1;
}

static int addr_value(AstNode *n, char **name, long long *off, Type **lty,
                      int *isf);

/* Address of an lvalue designator: `x`, `a[i]`, `s.k`, `(*p)`. */
static int addr_lvalue(AstNode *n, char **name, long long *off, Type **lty,
                       int *isf) {
    if (!n) return 0;
    switch (n->kind) {
    case AST_VAR: {
        Symbol *s = sym_find(n->u.name);
        if (!s) return 0;
        /* A function name is its own address; a local object has no static
         * address at all, and is refused rather than guessed. */
        if (s->kind != SYM_GLOBAL && s->kind != SYM_FUNC) return 0;
        *name = n->u.name;
        *off = 0;
        *lty = s->type ? s->type : n->type;
        *isf = (s->kind == SYM_FUNC);
        return 1;
    }
    case AST_ARRAY_ACCESS: {
        char *bn;
        long long boff, idx;
        Type *bt;
        int bif, es;
        if (!addr_lvalue(n->u.array_access.array, &bn, &boff, &bt, &bif)) return 0;
        if (!eval_const_expr(n->u.array_access.index, &idx)) return 0;
        if (!bt || bt->kind != TYPE_ARRAY || !bt->base) return 0;
        es = type_size(bt->base);
        if (es <= 0) return 0;
        *name = bn;
        *off = boff + idx * es;
        *lty = bt->base;
        *isf = 0;              /* an element is an object, never a function */
        return 1;
    }
    case AST_MEMBER: {
        char *bn;
        long long boff;
        Type *bt;
        int bif;
        /* `s.k` continues from the address of s; `p->k` needs p itself to be
         * an address constant, otherwise the address depends on a load. */
        if (n->u.member.is_arrow) {
            if (!addr_value(n->u.member.obj, &bn, &boff, &bt, &bif)) return 0;
        } else {
            if (!addr_lvalue(n->u.member.obj, &bn, &boff, &bt, &bif)) return 0;
        }
        *name = bn;
        *off = boff + n->u.member.offset;
        *lty = n->type;
        *isf = 0;
        return 1;
    }
    case AST_UNARY:
        if (n->u.unary.op == (int)TOK_MUL)
            return addr_value(n->u.unary.operand, name, off, lty, isf);
        return 0;
    default:
        return 0;
    }
}

/* Value of an expression that is an address constant. */
static int addr_value(AstNode *n, char **name, long long *off, Type **lty,
                      int *isf) {
    if (!n) return 0;
    switch (n->kind) {
    case AST_UNARY:
        if (n->u.unary.op == (int)TOK_BAND)
            return addr_lvalue(n->u.unary.operand, name, off, lty, isf);
        return 0;
    case AST_CAST:
        return addr_value(n->u.unary.operand, name, off, lty, isf);
    case AST_VAR: {
        Symbol *s = sym_find(n->u.name);
        if (!s) return 0;
        /* Function designator and array decay (C11 6.3.2.1p4, 6.3.2.1p3). */
        if (s->kind == SYM_FUNC) {
            *name = n->u.name;
            *off = 0;
            *lty = s->type ? s->type : n->type;
            *isf = 1;
            return 1;
        }
        if (s->kind == SYM_GLOBAL && s->type && s->type->kind == TYPE_ARRAY) {
            *name = n->u.name;
            *off = 0;
            *lty = s->type;
            *isf = 0;
            return 1;
        }
        return 0;
    }
    case AST_BINARY: {
        char *bn;
        long long boff, k, scale;
        Type *bt;
        int bif;
        int op = n->u.binary.op;
        if (op != (int)TOK_PLUS && op != (int)TOK_MINUS) return 0;
        if (addr_value(n->u.binary.left, &bn, &boff, &bt, &bif)) {
            /* `add + 1` is not a constant expression: a function has no
             * stride (C11 6.5.6p2 requires a pointer to complete object type),
             * and the old stride rule happily multiplied by 4. */
            if (bif) return 0;
            if (!eval_const_expr(n->u.binary.right, &k)) return 0;
            scale = addr_elem_size(bt);
            if (scale <= 0) return 0;
            *name = bn;
            *off = boff + (op == (int)TOK_PLUS ? k * scale : -k * scale);
            *lty = bt;
            *isf = 0;
            return 1;
        }
        if (op == (int)TOK_PLUS &&
            addr_value(n->u.binary.right, &bn, &boff, &bt, &bif)) {
            if (bif) return 0;
            if (!eval_const_expr(n->u.binary.left, &k)) return 0;
            scale = addr_elem_size(bt);
            if (scale <= 0) return 0;
            *name = bn;
            *off = boff + k * scale;
            *lty = bt;
            *isf = 0;
            return 1;
        }
        return 0;
    }
    default:
        return 0;
    }
}

/* Does this expression *look* like it is meant to be an address?  Used only to
 * choose the diagnostic: an initialiser in this shape that the walker refuses
 * is a non-constant address (`add + 1`, `&x` of a local object), and saying so
 * is more useful than "this build cannot turn it into data". */
static int looks_like_address(AstNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case AST_UNARY:
        return n->u.unary.op == (int)TOK_BAND;
    case AST_CAST:
        return looks_like_address(n->u.unary.operand);
    case AST_BINARY:
        if (n->u.binary.op != (int)TOK_PLUS && n->u.binary.op != (int)TOK_MINUS)
            return 0;
        if (looks_like_address(n->u.binary.left)) return 1;
        if (n->u.binary.op == (int)TOK_PLUS)
            return looks_like_address(n->u.binary.right);
        return 0;
    case AST_VAR: {
        Symbol *s = sym_find(n->u.name);
        if (!s) return 0;
        if (s->kind == SYM_FUNC) return 1;
        if (s->kind == SYM_GLOBAL && s->type && s->type->kind == TYPE_ARRAY)
            return 1;
        return 0;
    }
    default:
        return 0;
    }
}

/* Fill in the address-constant form of a global initialiser.  Returns 1 when
 * `iv` is one (or when it was meant to be one and was refused); the caller then
 * skips every value-shaped branch. */
static int irgen_try_addr_init(IrGlobal *gv, AstNode *iv) {
    char *name = 0;
    long long off = 0;
    Type *lt = 0;
    int isf = 0;
    StrBuf sb;
    if (!gv || !iv) return 0;
    /* Only a pointer can hold an address; an aggregate initialiser goes
     * through ark_agg_fill (which refuses an address element rather than
     * writing zero for it). */
    if (!gv->type || gv->type->kind != TYPE_PTR) return 0;
    if (!addr_value(iv, &name, &off, &lt, &isf)) {
        /* Refuse out loud.  This used to fall through to the C-source
         * serialiser, which happily wrote text like "add+1", and the backend
         * then stored it as data -- a wrong pointer with a message about
         * aggregates.  An initialiser that cannot be evaluated must not be
         * invented. */
        if (looks_like_address(iv)) {
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "the initialiser of the global '%s' is an address that is "
                      "not a constant expression (C11 6.6p9); a static object can "
                      "only be initialised with the address of an object with "
                      "static storage duration, plus or minus an integer constant",
                      gv->name, 0,0,0,0,0);
            g_irgen_errors++;
            gv->init_aggregate = 0;
            return 1;
        }
        return 0;
    }
    gv->has_init = 1;
    gv->init_sym = name;
    gv->init_sym_off = off;
    /* A function type has two spellings in this compiler -- a real TYPE_FUNC
     * node (from `typedef int binop(int,int)`) and a pointer type carrying
     * is_func_ptr (from `int (*p)(int,int)`) -- so the walker's answer, the
     * type it came back with and the symbol table all get a say.  A backend
     * that cannot name another unit's data can still name its code, so this
     * has to be right. */
    {
        Symbol *s = sym_find(name);
        if (s && s->kind == SYM_FUNC) isf = 1;
        else if (lt && (lt->kind == TYPE_FUNC || lt->is_func_ptr)) isf = 1;
        gv->init_sym_is_func = isf;
    }
    /* Keep the source text too: the C backend re-emits declarations, and a
     * NULL init_aggregate would drop the initialiser from its output. */
    sb_init(&sb);
    g_ser_unknown = 0;
    serialize_const_expr(&sb, iv);
    if (!g_ser_unknown) gv->init_aggregate = sb.data;
    return 1;
}

/* Append an AST expression to sb as C source; only constant expressions and simple operators are supported. */
static void serialize_const_expr(StrBuf *b, AstNode *n) {
    if (!n) { sb_puts(b, "0"); return; }
    switch (n->kind) {
        case AST_NUM:
            if (n->type && (n->type->kind == TYPE_FLOAT || n->type->kind == TYPE_DOUBLE)) {
                sb_putf(b, n->fval);
            } else {
                sb_putd(b, n->u.num_val);
            }
            break;
        case AST_STR:
            sb_putstr_lit(b, n->u.name);
            break;
        case AST_UNARY:
            if (n->u.unary.op == (int)TOK_MINUS) { sb_putc(b, '-'); serialize_const_expr(b, n->u.unary.operand); }
            else if (n->u.unary.op == (int)TOK_BNOT) { sb_putc(b, '~'); serialize_const_expr(b, n->u.unary.operand); }
            /* '&' must survive: dropping it turned `&arr[2]` into `arr[2]`,
             * and an array access then serialised as 0. */
            else if (n->u.unary.op == (int)TOK_BAND) { sb_putc(b, '&'); serialize_const_expr(b, n->u.unary.operand); }
            else serialize_const_expr(b, n->u.unary.operand);
            break;
        case AST_ARRAY_ACCESS:
            serialize_const_expr(b, n->u.array_access.array);
            sb_putc(b, '[');
            serialize_const_expr(b, n->u.array_access.index);
            sb_putc(b, ']');
            break;
        case AST_MEMBER:
            serialize_const_expr(b, n->u.member.obj);
            sb_puts(b, n->u.member.is_arrow ? "->" : ".");
            sb_puts(b, n->u.member.member);
            break;
        case AST_BINARY: {
            const char *op = "+";
            switch (n->u.binary.op) {
                case (int)TOK_PLUS:  op = "+"; break;
                case (int)TOK_MINUS: op = "-"; break;
                case (int)TOK_MUL:   op = "*"; break;
                case (int)TOK_DIV:   op = "/"; break;
                case (int)TOK_MOD:   op = "%"; break;
                case (int)TOK_BAND:  op = "&"; break;
                case (int)TOK_BOR:   op = "|"; break;
                case (int)TOK_BXOR:  op = "^"; break;
                case (int)TOK_SHL:   op = "<<"; break;
                case (int)TOK_SHR:   op = ">>"; break;
                default: break;
            }
            sb_putc(b, '(');
            serialize_const_expr(b, n->u.binary.left);
            sb_puts(b, op);
            serialize_const_expr(b, n->u.binary.right);
            sb_putc(b, ')');
            break;
        }
        case AST_CAST:
            serialize_const_expr(b, n->u.unary.operand);
            break;
        case AST_SIZEOF:
            sb_putd(b, n->u.num_val);
            break;
        case AST_VAR:
            /* If the name refers to a function defined in this translation
             * unit, the C backend emits it as ark_<name>; we must match that
             * prefix here so function pointers in global initializers
             * (e.g. Backend g_arm_backend = { ..., arm_emit, ... }) resolve
             * to the generated ark_arm_emit symbol. */
            if (n->u.name) {
                Symbol *s = sym_find(n->u.name);
                if (s && s->kind == SYM_FUNC) sb_puts(b, "ark_");
                sb_puts(b, n->u.name);
            } else sb_putd(b, 0);
            break;
        default:
            /* Keep the text syntactically valid for the C backend, but say so:
             * the callers report the error instead of writing zero as data. */
            g_ser_unknown = 1;
            sb_putd(b, 0);
            break;
    }
}

static void serialize_init(StrBuf *b, AstNode *n) {
    if (!n) { sb_putd(b, 0); return; }
    if (n->kind == AST_INIT_LIST) {
        int i;
        sb_putc(b, '{');
        for (i = 0; i < n->u.init_list.item_count; i++) {
            AstNode *it = n->u.init_list.items[i];
            if (i) sb_putc(b, ',');
            if (it) {
                if (it->u.init_field.has_field) {
                    sb_putc(b, '.');
                    sb_puts(b, it->u.init_field.field);
                    sb_putc(b, '=');
                } else if (it->u.init_field.has_index) {
                    sb_putc(b, '[');
                    sb_putd(b, it->u.init_field.index);
                    sb_puts(b, "]=");
                }
                serialize_init(b, it->u.init_field.value);
            }
        }
        sb_putc(b, '}');
        return;
    }
    serialize_const_expr(b, n);
}

/* Code generator state for one function: output program, target function,
 * frame cursor and the loop/switch label bookkeeping. */

typedef struct {
    IrProgram *prog;
    IrFunc    *fn;
    int        bp;
    int        max_size;
    int        break_label;
    int        cont_label;
    /* switch compile-time information */
    int       *case_labels;     /* label id for each case value */
    long long *case_values;
    int        case_count;
    int        default_label;
    int        switch_end_label;
} IrGen;

static void irgen_stmt(IrGen *g, AstNode *node);

/* Forward declarations: these two helpers live further down (they need the
 * globals that describe the target value model) but are already used by the
 * object-access paths above. */
static int irgen_is_cplx_float(Type *t);
static int irgen_complex_aux(Type *t, int width);
static void irgen_expr(IrGen *g, AstNode *node);
static void irgen_lvalue_addr(IrGen *g, AstNode *node);
static void irgen_cond_to_bool(IrGen *g, AstNode *cond);
static void irgen_widen_to_64(IrGen *g, Type *src_t, Type *dst_t);
static void irgen_truncate_int(IrGen *g, Type *dst_t);
/* Recursively perform aggregate initialization: write the scalar values of
 * init_list into the object of type t at frame offset off (C99 nested lists, designated initializers). */
static void irgen_local_aggregate_init(IrGen *g, int off, Type *t, AstNode *init_list);
/* C99 compound literal (type){...}: allocate a block-scope object with
 * automatic storage duration (C99 6.5.2.5). See the note below.
 * Bug-I7 FIX: Block-scope compound literals have automatic storage duration.
 * The function allocates a local stack slot, zero-initializes it, and applies
 * the initializer so each block entry re-initializes the object. Returns the
 * local frame offset. */
static int irgen_compound_literal(IrGen *g, AstNode *node);
/* Global compound literal counter: process-wide unique names. */
static int g_compound_lit_counter = 0;

/* C 后端使用 8 字节 long long 槽，无需 32→64 位 widen/narrow 序列。
 * 设为 1 时 irgen_widen_to_64 和 irgen_cond_to_bool 成为 no-op。 */
static int g_irgen_64bit_slots = 0;

void irgen_set_64bit_slots(int enabled) {
    g_irgen_64bit_slots = enabled;
}

/* How many 32-bit IR stack words one `_Complex double` value occupies.
 *   C backend  : 1 (an 8-byte slot whose union has a `double _Complex` member)
 *   ARM / RV32I: 4 (re_lo, re_hi, im_lo, im_hi -- the machine stack words, and
 *                exactly what IR_STORE with aux=16 pops)
 * Everything that pushes or pops a complex value has to agree with this, or the
 * IR stack drifts, so it is one process-wide number set by the driver right
 * next to irgen_set_64bit_slots(). */
static int g_irgen_complex_words = 4;

void irgen_set_complex_words(int words) {
    g_irgen_complex_words = (words > 0) ? words : 1;
}

/* Widen a real operand of a complex expression to a complex double: the
 * imaginary part is +0.0.  aux: 0x800 = source is a float, 0x800|0x200 =
 * source is already a double, 0x800|0x100 = source is an integer. */
static void irgen_widen_real_to_complex(IrGen *g, Type *t) {
    int aux;
    if (!t) aux = IR_AUX_COMPLEX | 0x100;
    else if (t->kind == TYPE_FLOAT)  aux = IR_AUX_COMPLEX;
    else if (t->kind == TYPE_DOUBLE) aux = IR_AUX_COMPLEX | 0x200;
    else {
        aux = IR_AUX_COMPLEX | 0x100;
        if (t->is_unsigned) aux |= 0x400;
    }
    ir_emit(g->fn, IR_I2F, 0, 0, aux);
}

#if ARKCC_FEATURE_FCONST_FOLD
/* 目标类型是 float/double 时，把常量表达式初值在编译期算出来写进 init_float。
 *
 * 为什么需要：初值处理原先只看 AST_NUM，其他形式统一掉进「序列化文本」的分支，
 * 而 ARM 后端把那段文本按整型逐元素写出去。于是
 *     double g = 1.0 / 3.0 * 3.0;
 * 编译通过、无告警，g 的 8 个字节全是 0。
 * 这里用 eval_const_fexpr（见 sema.c）求出数值，并按目标精度舍入。
 * 返回 1 表示已处理（has_init/is_float/init_float 已写好）。 */
static int irgen_try_float_init(IrGlobal *gv, AstNode *iv) {
    double dv = 0.0;
    Type *dt;
    if (!gv || !iv) return 0;
    dt = gv->type;
    if (!dt) return 0;
    if (dt->kind != TYPE_FLOAT && dt->kind != TYPE_DOUBLE) return 0;
    if (!eval_const_fexpr(iv, &dv)) return 0;
    gv->has_init  = 1;
    gv->is_float  = 1;
    gv->init_float = (dt->kind == TYPE_FLOAT) ? (double)(float)dv : dv;
    gv->init_int   = 0;
    return 1;
}
/* _Complex 全局变量的常量初始化。
 *
 * 用在 irgen_try_float_init 之前：后者只看 target 是 float/double，碰到
 * _Complex 就直接返回 0，于是 `double _Complex g = 3.0;` 落回"按 AST_NUM
 * 把 iv->fval 存成 is_float"的旧路径，后端又只写 4 字节。
 * 这里把实部/虚部都折叠好，并用 init_is_complex 告诉后端两个字都要写。
 *
 * 返回 1 表示已处理（has_init/is_float/init_float/init_float_im/init_is_complex）。 */
static int irgen_try_complex_init(IrGlobal *gv, AstNode *iv) {
    double re = 0.0, im = 0.0;
    Type *dt, *bt;
    if (!gv || !iv) return 0;
    dt = gv->type;
    if (!dt || dt->kind != TYPE_COMPLEX) return 0;
    if (!eval_const_cexpr(iv, &re, &im)) {
        /* 折不出来就不装作折出来：留给后续分支（报错）。 */
        return 0;
    }
    bt = dt->base;
    if (bt && bt->kind == TYPE_FLOAT) {
        re = (double)(float)re;
        im = (double)(float)im;
    }
    gv->has_init = 1;
    gv->is_float = 1;
    gv->init_is_complex = 1;
    gv->init_float = re;
    gv->init_float_im = im;
    gv->init_int = 0;
    return 1;
}

/* 整型全局变量用**浮点**常量表达式初始化（隐式转换，C99 6.6）：
 *     int g = 2.5 * 4.0;      -> 10
 * 显式写转换的 `(int)(2.5 * 4.0)` 由 eval_const_expr 处理；这里补的是没有写
 * 转换、但初值表达式本身是浮点类型的那一半。越界仍然拒绝：那是 UB，
 * 折叠出一个整数等于凭空发明答案。
 * 返回 1 表示已处理（*out 已写好）。 */
static int irgen_try_implicit_int_init(IrGlobal *gv, AstNode *iv, long long *out) {
    double dv = 0.0;
    Type *dt;
    if (!gv || !iv || !out) return 0;
    dt = gv->type;
    if (!dt || !type_is_integer(dt)) return 0;
    if (!iv->type) return 0;
    if (iv->type->kind != TYPE_FLOAT && iv->type->kind != TYPE_DOUBLE) return 0;
    if (!eval_const_fexpr(iv, &dv)) return 0;
    return fconst_to_int(dv, dt, out);
}
#endif

static int alloc_label(IrGen *g) {
    return g->fn->label_count++;
}

static int alloc_local(IrGen *g, int size) {
    /* Always align slots to 8 bytes so long long / double bit patterns fit intact;
     * avoids losing the high half of a double when long=4 (LLP64) shares a union. */
    int s = (size + 7) & ~7;
    int off;
    if (s <= 0) s = 8;
    off = g->bp;
    g->bp += s;
    if (g->bp > g->max_size) g->max_size = g->bp;
    return off;
}

/* Build a global-ised name for a function-scope static local: ark_sl_<func>_<var>.
 * Same-named statics in different functions do not collide and stay separate
 * from user-defined global names. */
static char *static_local_name(const char *func, const char *var) {
    int nf = (int)ark_strlen(func);
    int nv = (int)ark_strlen(var);
    char *buf = (char *)mem_pool_alloc(nf + nv + 16);
    int p = 0;
    const char *pre = "ark_sl_";
    while (*pre) buf[p++] = *pre++;
    { int i; for (i = 0; i < nf; i++) buf[p++] = func[i]; }
    buf[p++] = '_';
    { int i; for (i = 0; i < nv; i++) buf[p++] = var[i]; }
    buf[p] = 0;
    return buf;
}

/* First pass over a switch body: collect the label ids for case/default. */
typedef struct CaseList {
    int *labels;
    long long *values;
    int  count;
    int  cap;
    int  default_label;
} CaseList;

static void collect_cases(IrGen *g, AstNode *node, CaseList *cl) {
    if (!node) return;
    if (node->kind == AST_CASE) {
        if (cl->count >= cl->cap) {
            int nc = cl->cap == 0 ? 8 : cl->cap * 2;
            cl->labels = (int *)mem_realloc(cl->labels, cl->cap * (int)sizeof(int), nc * (int)sizeof(int));
            cl->values = (long long *)mem_realloc(cl->values, cl->cap * (int)sizeof(long long), nc * (int)sizeof(long long));
            cl->cap = nc;
        }
        cl->values[cl->count] = node->u.case_stmt.value;
        cl->labels[cl->count] = alloc_label(g);
        node->u.case_stmt.label_id = cl->labels[cl->count];
        cl->count++;
        collect_cases(g, node->u.case_stmt.stmt, cl);
        return;
    }
    if (node->kind == AST_DEFAULT) {
        if (cl->default_label < 0) {
            cl->default_label = alloc_label(g);
            node->u.case_stmt.label_id = cl->default_label;
        }
        collect_cases(g, node->u.case_stmt.stmt, cl);
        return;
    }
    if (node->kind == AST_SWITCH) {
        return; /* a nested switch is handled by itself */
    }
    if (node->kind == AST_BLOCK) {
        int i;
        for (i = 0; i < node->u.block.stmt_count; i++)
            collect_cases(g, node->u.block.stmts[i], cl);
        return;
    }
    if (node->kind == AST_IF) {
        collect_cases(g, node->u.if_stmt.then_stmt, cl);
        collect_cases(g, node->u.if_stmt.else_stmt, cl);
        return;
    }
    if (node->kind == AST_WHILE || node->kind == AST_DO_WHILE) {
        collect_cases(g, node->u.while_stmt.body, cl);
        return;
    }
    if (node->kind == AST_FOR) {
        collect_cases(g, node->u.for_stmt.body, cl);
        return;
    }
    if (node->kind == AST_LABEL) {
        collect_cases(g, node->u.label_stmt.stmt, cl);
        return;
    }
    if (node->kind == AST_CASE) {
        /* already handled above */
    }
}

/* See include/irgen.h. Counted, never cleared implicitly except at entry. */
int g_irgen_errors = 0;

IrProgram *irgen_program(AstNode *prog) {
    IrProgram *out = ir_program_new();
    int i;
    g_irgen_errors = 0;
    sym_init();

    /* Pre-pass: register all function/global symbols so cross-references
     * resolve correctly. Also handles armcc __weak override: a non-weak
     * definition overrides a prior weak definition of the same name. */
    for (i = 0; i < prog->u.program.decl_count; i++) {
        AstNode *d = prog->u.program.decls[i];
        if (d->kind == AST_FUNC_DEF || d->kind == AST_FUNC_DECL ||
            d->kind == AST_ASM_FUNC) {
            Symbol *existing = sym_find(d->u.func.name);
            if (!existing) {
                Symbol *s = sym_add(SYM_FUNC, d->u.func.name, d->u.func.ret_type, 0);
                s->param_count = d->u.func.param_count;
                s->is_variadic = d->u.func.is_variadic;
                /* Store parameter types for 64-bit arg promotion at call sites */
                if (d->u.func.param_count > 0 && d->u.func.params) {
                    int pi;
                    s->param_types = (Type **)mem_pool_alloc(sizeof(Type *) * d->u.func.param_count);
                    for (pi = 0; pi < d->u.func.param_count; pi++) {
                        AstNode *pp = d->u.func.params[pi];
                        s->param_types[pi] = (pp && pp->u.var.var_type) ? pp->u.var.var_type : 0;
                    }
                }
                s->svc_num = d->u.func.svc_num;
                s->is_asm_func = d->u.func.is_asm_func;
                s->alias_target = d->u.func.alias_target;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                s->smc_num = d->u.func.smc_num;
#endif
                if (d->kind == AST_FUNC_DEF || d->kind == AST_ASM_FUNC) s->is_defined = 1;
                if (d->u.func.is_weak) s->is_weak = 1;
            } else {
                /* armcc __weak override: non-weak definition replaces weak. */
                if (existing->is_weak && !d->u.func.is_weak &&
                    (d->kind == AST_FUNC_DEF || d->kind == AST_ASM_FUNC)) {
                    existing->is_weak = 0;
                }
                /* If existing was a non-weak declaration and the actual
                 * definition is __weak, propagate the weak attribute to
                 * the symbol (mirrors sema's "if (node->is_weak) s->is_weak = 1").
                 * Without this, irgen's weak-skip check would wrongly discard
                 * the definition (symbol.is_weak=0 != ast.is_weak=1). */
                else if (!existing->is_weak && d->u.func.is_weak &&
                         (d->kind == AST_FUNC_DEF || d->kind == AST_ASM_FUNC)) {
                    existing->is_weak = 1;
                }
            }
        } else if (d->kind == AST_VAR_DECL && d->u.var.is_global) {
            Symbol *existing = sym_find(d->u.var.name);
            if (!existing) {
                IrGlobal *gv = ir_program_add_global(out, d->u.var.name, d->u.var.var_type);
                Symbol *s = sym_add(SYM_GLOBAL, d->u.var.name, d->u.var.var_type, 0);
                (void)s;
                gv->is_static = d->u.var.is_static;
                gv->is_extern = d->u.var.is_extern;
                gv->is_const = d->u.var.is_const;
                gv->section  = d->u.var.section;
                gv->align_bytes = d->u.var.align_bytes;
                gv->is_used   = d->u.var.is_used;
                gv->is_unused = d->u.var.is_unused;
                gv->has_at    = d->u.var.has_at;
                gv->at_addr   = d->u.var.at_addr;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                gv->is_tls     = d->u.var.is_tls;
                gv->global_reg = d->u.var.global_reg;
#endif
                if (d->u.var.init) {
                    AstNode *iv = d->u.var.init;
#if ARKCC_FEATURE_FCONST_FOLD
                    if (irgen_try_complex_init(gv, iv)) {
                        /* _Complex 常量初始化：实部/虚部已折叠 */
                    } else if (irgen_try_float_init(gv, iv)) {
                        /* 浮点常量表达式初值已在编译期算出 */
                    } else
#endif
                                        if (irgen_try_addr_init(gv, iv)) {
                        /* 静态地址常量初值：符号 + 字节偏移 */
                    } else if (iv->kind == AST_NUM) {
                        gv->has_init = 1;
                        gv->init_int = iv->u.num_val;
                        if (iv->type && (iv->type->kind == TYPE_FLOAT || iv->type->kind == TYPE_DOUBLE)) {
                            gv->is_float = 1;
                            gv->init_float = iv->fval;
                        }
                    } else if (iv->kind == AST_STR) {
                        /* char a[]="hello" or char *p="..." */
                        gv->has_init = 1;
                        gv->init_str = iv->u.name;
                    } else if (iv->kind == AST_INIT_LIST) {
                        StrBuf sb; sb_init(&sb);
                        serialize_init(&sb, iv);
                        gv->has_init = 1;
                        gv->init_aggregate = sb.data;
                    } else if (iv->kind == AST_COMPOUND_LITERAL) {
                        /* Compound literal initialising a global: expand its init list. */
                        StrBuf sb; sb_init(&sb);
                        serialize_init(&sb, iv->u.compound_lit.init);
                        gv->has_init = 1;
                        gv->init_aggregate = sb.data;
                    } else {
                        /* Non-literal initialiser: try constant evaluation first
                         * (sizeof, 1+2, arithmetic on literals, ...).
                         * On success store it as an integer literal in init_int so the ARM
                         * backend can emit proper .word data; the C backend goes through
                         * init_aggregate and passes the source text through (enum refs etc.). */
                        long long cv = 0;
                        gv->has_init = 1;
                        if (eval_const_expr(iv, &cv)) {
                            gv->init_int = cv;
                        } else
#if ARKCC_FEATURE_FCONST_FOLD
                        if (irgen_try_implicit_int_init(gv, iv, &cv)) {
                            /* init is a float constant expression: implicit conversion */
                            gv->init_int = cv;
                        } else
#endif
                        {
                            StrBuf sb; sb_init(&sb);
                            g_ser_unknown = 0;
                            serialize_const_expr(&sb, iv);
                            /* A shape this build cannot place in data must be refused;
                             * writing the object without its initialiser invents zeroes. */
                            if (g_ser_unknown) {
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "the initialiser of the global '%s' is not a constant "
                                      "expression this build can place in data",
                                      gv->name, 0,0,0,0,0);
                            g_irgen_errors++;
                            } else {
                            gv->init_aggregate = sb.data;
                            }
                        }
                    }
                }
            } else if (!d->u.var.is_extern) {
                /* A definition follows an earlier extern declaration
                 * (e.g. header has `extern T x;`, source has `T x;`).
                 * Clear is_extern on the existing IrGlobal so the C
                 * backend emits a definition, not an extern prototype. */
                IrGlobal *eg = ir_program_find_global(out, d->u.var.name);
                if (eg) {
                    eg->is_extern = 0;
                    eg->is_const  = d->u.var.is_const;
                    eg->section   = d->u.var.section;
                    /* #3 FIX: Process initializer when definition follows extern.
                     * Previously the initializer was silently dropped, causing
                     * all .data globals preceded by extern declarations to
                     * have init_int=0 (e.g. SystemCoreClock=16000000 became 0). */
                    if (d->u.var.init && !eg->has_init) {
                        AstNode *iv = d->u.var.init;
#if ARKCC_FEATURE_FCONST_FOLD
                        if (irgen_try_complex_init(eg, iv)) {
                            /* _Complex 常量初始化：实部/虚部已折叠 */
                        } else if (irgen_try_float_init(eg, iv)) {
                            /* 同上：浮点常量表达式 */
                        } else
#endif
                                                if (irgen_try_addr_init(eg, iv)) {
                            /* 静态地址常量初值：符号 + 字节偏移 */
                        } else if (iv->kind == AST_NUM) {
                            eg->has_init = 1;
                            eg->init_int = iv->u.num_val;
                            if (iv->type && (iv->type->kind == TYPE_FLOAT || iv->type->kind == TYPE_DOUBLE)) {
                                eg->is_float = 1;
                                eg->init_float = iv->fval;
                            }
                        } else if (iv->kind == AST_STR) {
                            eg->has_init = 1;
                            eg->init_str = iv->u.name;
                        } else if (iv->kind == AST_INIT_LIST) {
                            StrBuf sb; sb_init(&sb);
                            serialize_init(&sb, iv);
                            eg->has_init = 1;
                            eg->init_aggregate = sb.data;
                        } else if (iv->kind == AST_COMPOUND_LITERAL) {
                            StrBuf sb; sb_init(&sb);
                            serialize_init(&sb, iv->u.compound_lit.init);
                            eg->has_init = 1;
                            eg->init_aggregate = sb.data;
                        } else {
                            long long cv = 0;
                            eg->has_init = 1;
                            if (eval_const_expr(iv, &cv)) {
                                eg->init_int = cv;
                            } else
#if ARKCC_FEATURE_FCONST_FOLD
                            if (irgen_try_implicit_int_init(eg, iv, &cv)) {
                                /* init is a float constant expression: implicit conversion */
                                eg->init_int = cv;
                            } else
#endif
                            {
                                StrBuf sb; sb_init(&sb);
                                g_ser_unknown = 0;
                                serialize_const_expr(&sb, iv);
                                /* A shape this build cannot place in data must be refused;
                                 * writing the object without its initialiser invents zeroes. */
                                if (g_ser_unknown) {
                                diag_tool(DIAG_SEV_ERROR, "arkcc",
                                          "the initialiser of the global '%s' is not a constant "
                                          "expression this build can place in data",
                                          eg->name, 0,0,0,0,0);
                                g_irgen_errors++;
                                } else {
                                eg->init_aggregate = sb.data;
                                }
                            }
                        }
                    }
                }
            }
        } else if (d->kind == AST_TYPEDEF) {
            if (d->u.var.name) sym_add(SYM_TYPEDEF, d->u.var.name, d->u.var.var_type, 0);
        } else if (d->kind == AST_BLOCK) {
            /* Reference to a global variable or enum constant: emit the name. */
            int j;
            for (j = 0; j < d->u.block.stmt_count; j++) {
                AstNode *c = d->u.block.stmts[j];
                if (c->kind == AST_VAR_DECL && c->u.var.is_global) {
                    Symbol *existing_c = sym_find(c->u.var.name);
                    if (!existing_c) {
                        IrGlobal *gc = ir_program_add_global(out, c->u.var.name, c->u.var.var_type);
                        sym_add(SYM_GLOBAL, c->u.var.name, c->u.var.var_type, 0);
                        gc->is_static = c->u.var.is_static;
                        gc->is_extern = c->u.var.is_extern;
                        gc->is_const = c->u.var.is_const;
                        gc->section  = c->u.var.section;
                        gc->align_bytes = c->u.var.align_bytes;
                        gc->is_used   = c->u.var.is_used;
                        gc->is_unused = c->u.var.is_unused;
                        gc->has_at    = c->u.var.has_at;
                        gc->at_addr   = c->u.var.at_addr;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                        gc->is_tls     = c->u.var.is_tls;
                        gc->global_reg = c->u.var.global_reg;
#endif
                        if (c->u.var.init) {
                            AstNode *iv = c->u.var.init;
#if ARKCC_FEATURE_FCONST_FOLD
                            if (irgen_try_complex_init(gc, iv)) {
                                /* _Complex 常量初始化：实部/虚部已折叠 */
                            } else if (irgen_try_float_init(gc, iv)) {
                                /* 同上：浮点常量表达式 */
                            } else
#endif
                                                        if (irgen_try_addr_init(gc, iv)) {
                                /* 静态地址常量初值：符号 + 字节偏移 */
                            } else if (iv->kind == AST_NUM) {
                                gc->has_init = 1;
                                gc->init_int = iv->u.num_val;
                                if (iv->type && (iv->type->kind == TYPE_FLOAT || iv->type->kind == TYPE_DOUBLE)) {
                                    gc->is_float = 1;
                                    gc->init_float = iv->fval;
                                }
                            } else if (iv->kind == AST_STR) {
                                gc->has_init = 1;
                                gc->init_str = iv->u.name;
                            } else if (iv->kind == AST_INIT_LIST) {
                                StrBuf sb; sb_init(&sb);
                                serialize_init(&sb, iv);
                                gc->has_init = 1;
                                gc->init_aggregate = sb.data;
                            } else if (iv->kind == AST_COMPOUND_LITERAL) {
                                StrBuf sb; sb_init(&sb);
                                serialize_init(&sb, iv->u.compound_lit.init);
                                gc->has_init = 1;
                                gc->init_aggregate = sb.data;
                            } else {
                                long long cv = 0;
                                gc->has_init = 1;
                                if (eval_const_expr(iv, &cv)) {
                                    gc->init_int = cv;
                                } else
#if ARKCC_FEATURE_FCONST_FOLD
                                if (irgen_try_implicit_int_init(gc, iv, &cv)) {
                                    /* init is a float constant expression: implicit conversion */
                                    gc->init_int = cv;
                                } else
#endif
                                {
                                    StrBuf sb; sb_init(&sb);
                                    g_ser_unknown = 0;
                                    serialize_const_expr(&sb, iv);
                                    /* A shape this build cannot place in data must be refused;
                                     * writing the object without its initialiser invents zeroes. */
                                    if (g_ser_unknown) {
                                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                                              "the initialiser of the global '%s' is not a constant "
                                              "expression this build can place in data",
                                              gc->name, 0,0,0,0,0);
                                    g_irgen_errors++;
                                    } else {
                                    gc->init_aggregate = sb.data;
                                    }
                                }
                            }
                        }
                    } else if (!c->u.var.is_extern) {
                        IrGlobal *eg = ir_program_find_global(out, c->u.var.name);
                        if (eg) {
                            eg->is_extern = 0;
                            eg->is_const  = c->u.var.is_const;
                            eg->section   = c->u.var.section;
                            /* #3 FIX: Process initializer for block-level globals too */
                            if (c->u.var.init && !eg->has_init) {
                                AstNode *iv = c->u.var.init;
#if ARKCC_FEATURE_FCONST_FOLD
                                if (irgen_try_complex_init(eg, iv)) {
                                    /* _Complex 常量初始化：实部/虚部已折叠 */
                                } else if (irgen_try_float_init(eg, iv)) {
                                    /* 同上：浮点常量表达式 */
                                } else
#endif
                                                                if (irgen_try_addr_init(eg, iv)) {
                                    /* 静态地址常量初值：符号 + 字节偏移 */
                                } else if (iv->kind == AST_NUM) {
                                    eg->has_init = 1;
                                    eg->init_int = iv->u.num_val;
                                    if (iv->type && (iv->type->kind == TYPE_FLOAT || iv->type->kind == TYPE_DOUBLE)) {
                                        eg->is_float = 1;
                                        eg->init_float = iv->fval;
                                    }
                                } else if (iv->kind == AST_STR) {
                                    eg->has_init = 1;
                                    eg->init_str = iv->u.name;
                                } else if (iv->kind == AST_INIT_LIST) {
                                    StrBuf sb; sb_init(&sb);
                                    serialize_init(&sb, iv);
                                    eg->has_init = 1;
                                    eg->init_aggregate = sb.data;
                                } else if (iv->kind == AST_COMPOUND_LITERAL) {
                                    StrBuf sb; sb_init(&sb);
                                    serialize_init(&sb, iv->u.compound_lit.init);
                                    eg->has_init = 1;
                                    eg->init_aggregate = sb.data;
                                } else {
                                    long long cv = 0;
                                    eg->has_init = 1;
                                    if (eval_const_expr(iv, &cv)) {
                                        eg->init_int = cv;
                                    } else
#if ARKCC_FEATURE_FCONST_FOLD
                                    if (irgen_try_implicit_int_init(eg, iv, &cv)) {
                                        /* init is a float constant expression: implicit conversion */
                                        eg->init_int = cv;
                                    } else
#endif
                                    {
                                        StrBuf sb; sb_init(&sb);
                                        g_ser_unknown = 0;
                                        serialize_const_expr(&sb, iv);
                                        /* A shape this build cannot place in data must be refused;
                                         * writing the object without its initialiser invents zeroes. */
                                        if (g_ser_unknown) {
                                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                                  "the initialiser of the global '%s' is not a constant "
                                                  "expression this build can place in data",
                                                  eg->name, 0,0,0,0,0);
                                        g_irgen_errors++;
                                        } else {
                                        eg->init_aggregate = sb.data;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    for (i = 0; i < prog->u.program.decl_count; i++) {
        AstNode *d = prog->u.program.decls[i];
        if (d->kind == AST_ASM_FUNC) {
            IrFunc *fn;
            /* armcc __weak override: skip if this weak asm func was
             * overridden by a non-weak definition (see AST_FUNC_DEF above). */
            {
                Symbol *fsym = sym_find(d->u.func.name);
                if (fsym && !!fsym->is_weak != !!d->u.func.is_weak) {
                    continue;
                }
            }
            fn = ir_func_new(d->u.func.name);
            fn->ret_type = d->u.func.ret_type;
            fn->param_count = d->u.func.param_count;
            fn->is_static = d->u.func.is_static;
            fn->is_asm_func = 1;
            fn->asm_body = d->u.func.asm_body;
            fn->is_used = d->u.func.is_used;
            fn->is_unused = d->u.func.is_unused;
            fn->is_weak = d->u.func.is_weak;
            fn->alias_target = d->u.func.alias_target;
            fn->is_constructor = d->u.func.is_constructor;
            fn->is_destructor  = d->u.func.is_destructor;
            fn->is_deprecated  = d->u.func.is_deprecated;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            fn->is_value_in_regs = d->u.func.is_value_in_regs;
            fn->is_pure          = d->u.func.is_pure;
            fn->is_const_attr    = d->u.func.is_const_attr;
            fn->is_interwork     = d->u.func.is_interwork;
            fn->smc_num          = d->u.func.smc_num;
            fn->global_reg       = d->u.func.global_reg;
#endif
            ir_program_add(out, fn);
            continue;
        }
        if (d->kind == AST_FUNC_DECL) {
            /* A declaration that only carries alias_target still needs an IrFunc so the backend can emit the alias. */
            if (d->u.func.alias_target) {
                IrFunc *fn = ir_func_new(d->u.func.name);
                fn->ret_type = d->u.func.ret_type;
                fn->param_count = d->u.func.param_count;
                fn->is_static = d->u.func.is_static;
                fn->is_used = d->u.func.is_used;
                fn->is_unused = d->u.func.is_unused;
                fn->is_weak = d->u.func.is_weak;
                fn->alias_target = d->u.func.alias_target;
                fn->is_constructor = d->u.func.is_constructor;
                fn->is_destructor  = d->u.func.is_destructor;
                fn->is_deprecated  = d->u.func.is_deprecated;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                fn->is_value_in_regs = d->u.func.is_value_in_regs;
                fn->is_pure          = d->u.func.is_pure;
                fn->is_const_attr    = d->u.func.is_const_attr;
                fn->is_interwork     = d->u.func.is_interwork;
                fn->smc_num          = d->u.func.smc_num;
                fn->global_reg       = d->u.func.global_reg;
#endif
                ir_program_add(out, fn);
            }
            continue;
        }
        if (d->kind == AST_FUNC_DEF) {
            IrGen g;
            int j;
            /* armcc __weak override: skip IR generation for a weak definition
             * that has been overridden by a non-weak definition of the same
             * name. Detected by symbol.is_weak != ast.is_weak (the symbol's
             * is_weak was cleared in the pre-pass when the non-weak override
             * was seen). */
            {
                Symbol *fsym = sym_find(d->u.func.name);
                if (fsym && !!fsym->is_weak != !!d->u.func.is_weak) {
                    continue;
                }
            }
            g.prog = out;
            g.fn = ir_func_new(d->u.func.name);
            g.fn->ret_type = d->u.func.ret_type;
            g.fn->param_count = d->u.func.param_count;
            /* A _Complex *parameter* travels as one address word (the callee
             * copies the payload into its frame, the same shape a >8-byte
             * struct uses) or, when the object is 8 bytes wide, as two raw
             * words -- see the argument loop and the slot accounting below.
             * A _Complex *return value* keeps the four-word value on the IR
             * stack and is narrowed by the backend in IR_RETURN:
             *   _Complex float  (8 bytes)  -> r0:r1 / a0:a1, the real half in
             *                                 the low word (AAPCS composite of
             *                                 two words, same wire shape the
             *                                 parameter uses);
             *   _Complex double (16 bytes) -> sret: the caller passes the
             *                                 address of a result buffer in r0
             *                                 and reads the payload from there.
             * The C backend needs none of this (g_irgen_complex_words == 1). */
            /* Populate param_types so backends can compute AAPCS register
             * assignment (64-bit even-pair alignment) for IR_GETARG.
             * Without this, aapcs_map falls back to 4-byte slots for all
             * params, mis-routing long long/double through wrong registers. */
            if (d->u.func.param_count > 0) {
                g.fn->param_types = (Type **)mem_pool_alloc(
                    sizeof(Type *) * d->u.func.param_count);
                for (j = 0; j < d->u.func.param_count; j++) {
                    AstNode *pp = d->u.func.params[j];
                    g.fn->param_types[j] = (pp && pp->u.var.var_type)
                                            ? pp->u.var.var_type : 0;
                }
            }
            g.fn->is_variadic = d->u.func.is_variadic;
            g.fn->is_static = d->u.func.is_static;
            /* Propagate ARM Compiler 5 function attributes to IrFunc so the backend can emit
             * __attribute__((weak/naked)) and the __irq prologue note. */
            g.fn->is_irq   = d->u.func.is_irq;
            /* Auto-detect ARM interrupt handlers by name suffix.
             * STM32CubeMX/HAL code doesn't use __irq keyword; without this,
             * ISRs would use the normal prologue and clobber r0-r3/r12.
             * Reset_Handler and Default_Handler are entry points, not ISRs. */
            if (!g.fn->is_irq && g.fn->name) {
                int nl = ark_strlen(g.fn->name);
                if (ark_streq(g.fn->name, "Reset_Handler") ||
                    ark_streq(g.fn->name, "Default_Handler")) {
                    /* entry point, not ISR */
                }
                else if (nl >= 8 && ark_streq(g.fn->name + nl - 8, "_Handler"))
                    g.fn->is_irq = 1;
                else if (nl >= 11 && ark_streq(g.fn->name + nl - 11, "_IRQHandler"))
                    g.fn->is_irq = 1;
            }
            g.fn->is_weak  = d->u.func.is_weak;
            g.fn->is_naked = d->u.func.is_naked;
            g.fn->is_used  = d->u.func.is_used;
            g.fn->is_unused= d->u.func.is_unused;
            g.fn->forceinline = d->u.func.forceinline;
            g.fn->alias_target = d->u.func.alias_target;
            g.fn->is_constructor = d->u.func.is_constructor;
            g.fn->is_destructor  = d->u.func.is_destructor;
            g.fn->is_deprecated  = d->u.func.is_deprecated;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            g.fn->is_value_in_regs = d->u.func.is_value_in_regs;
            g.fn->is_pure          = d->u.func.is_pure;
            g.fn->is_const_attr    = d->u.func.is_const_attr;
            g.fn->is_interwork     = d->u.func.is_interwork;
            g.fn->smc_num          = d->u.func.smc_num;
            g.fn->global_reg       = d->u.func.global_reg;
#endif
            g.bp = 0;
            g.max_size = 0;
            g.break_label = -1;
            g.cont_label = -1;
            g.case_labels = 0;
            g.case_values = 0;
            g.case_count = 0;
            g.default_label = -1;
            g.switch_end_label = -1;

            ir_emit(g.fn, IR_FUNC_BEGIN, 0, d->u.func.name, d->u.func.param_count);

            sym_enter_scope();
            {
                int slot = 0;  /* cdecl: each 4-byte slot is one arg position */
                for (j = 0; j < d->u.func.param_count; j++) {
                    AstNode *pp = d->u.func.params[j];
                    Type *ptype = pp->u.var.var_type;
                    int sz = type_size(ptype);
                    int off;
                    int is_struct_val = (ptype &&
                        (ptype->kind == TYPE_STRUCT || ptype->kind == TYPE_UNION) &&
                        sz > 8);
                    if (sz <= 0) sz = 4;
                    off = alloc_local(&g, sz);
                    {
                        Symbol *s = sym_add(SYM_PARAM, pp->u.var.name, ptype, off);
                        (void)s;
                    }
                    /* For struct-by-value parameters (size > 8), the caller pushes
                     * the source struct address. Encode size in aux high bits so
                     * the C backend can emit memcpy(frame+off, src_ptr, size).
                     * For all params, encode size in aux high bits so the x86
                     * backend can handle 64-bit (long long/double) params by
                     * reading both [ebp+8+slot*4] and [ebp+8+slot*4+4]. */
                    ir_emit(g.fn, IR_GETARG, off, pp->u.var.name, (sz << 8) | slot);
                    /* Slots count the words actually transmitted, not the size of
                     * the object: a struct larger than 8 bytes travels as a
                     * pointer (one word), a struct of 5..8 bytes as two words,
                     * an array parameter is a pointer too. This is the same
                     * picture as aapcs_assign() on the callee side, and it has
                     * to match: ceil(size/4) used to give a 12-byte struct three
                     * slots, so every later parameter asked for a slot that
                     * aapcs_soff[] had never filled (measured: the 5th argument
                     * of mixed(int, struct P, int, struct Q, int) was loaded
                     * from [r6, #63], i.e. soff == -1). */
                    {
                        int pwords;
                        if (is_struct_val) pwords = 1;                 /* passed by pointer */
                        else if (ptype && (ptype->kind == TYPE_STRUCT ||
                                           ptype->kind == TYPE_UNION))
                            pwords = (sz > 4) ? 2 : 1;                 /* by value in words */
                        else if (ptype && ptype->kind == TYPE_ARRAY)
                            pwords = 1;                                /* decays to a pointer */
                        else if (g_irgen_complex_words > 1 && ptype &&
                                 ptype->kind == TYPE_COMPLEX && sz > 8)
                            pwords = 1;   /* four-word value, passed as one address word */
                        else
                            pwords = (sz >= 8) ? 2 : 1;   /* long long / double */
                        slot += pwords;
                    }
                }
                /* For variadic functions, also store all 8 args (a0..a7) at a
                 * fixed frame region (ARK_VA_ARG_BASE..+64) so va_start/va_arg
                 * can access the variadic tail. */
                if (d->u.func.is_variadic && g_ir_va_slot_region) {
                    int k;
                    for (k = 0; k < 8; k++) {
                        ir_emit(g.fn, IR_GETARG, 8000 + k * 8, 0, k);
                    }
                }
            }
            irgen_stmt(&g, d->u.func.body);
            ir_emit(g.fn, IR_PUSH_INT, 0, 0, 0);
            ir_emit(g.fn, IR_RETURN, 0, 0, 0);
            ir_emit(g.fn, IR_FUNC_END, 0, 0, 0);
            /* Bug-X4: For variadic functions, the 8000 frame region (used by
             * va_start/va_arg to access the variadic tail) must be included
             * in local_size so the backend prologue allocates enough stack.
             * The region spans offsets 8000..8063 (8 args * 8 bytes).
             *
             * Only for the 8-byte-slot backends (C, x86_64).  ARM and RV32I
             * build their argument image out of the caller's own registers and
             * stack words, so 8 KB of frame would be pure waste -- and on the
             * board it is fatal: a mini-app task gets APP_TASK_STACK_SIZE
             * = 4 KB, so a variadic function could never be called at all.
             * The driver clears the switch for those two targets. */
            if (d->u.func.is_variadic && g_ir_va_slot_region) {
                if (g.max_size < 8064)
                    g.max_size = 8064;
            }
            g.fn->local_size = (g.max_size + 7) & ~7;
            sym_leave_scope();
            ir_program_add(out, g.fn);
        }
    }
    return out;
}

/* Recursively perform aggregate initialization: write the scalar values of
 * init_list into the object of type t at frame offset off (C99 nested lists
 * and designated initializers).
 *   - Array: each item goes to off + idx*elem_sz, by [idx] or position;
 *     an INIT_LIST item recurses according to the element type.
 *   - Struct: each item goes to off + field->offset, by .field or position;
 *     an INIT_LIST item recurses according to the field type. */
/* Store a string literal's bytes into the existing char array object at frame
 * offset `off` (type `arr_t`). Mirrors the `char a[]="..."` declaration path:
 * check for the element type outside, zero-fill is done by the caller, then
 * memcpy the literal (truncated to the array length). Without this, an
 * aggregate initializer such as `struct Q q = {3, "abcd", 5}` fell through to
 * the scalar path and stored the literal *pointer*'s low bytes into the array
 * (observed: printing q.s produced garbage in the C backend). */
static void irgen_local_store_str_lit(IrGen *g, int off, Type *arr_t, AstNode *val) {
    const char *s = val->u.name;
    int slen = s ? ark_strlen(s) : 0;
    int arr_len = (arr_t && arr_t->size > 0) ? arr_t->size : slen + 1;
    int copy_n = slen + 1;
    if (copy_n > arr_len) copy_n = arr_len;
    ir_emit(g->fn, IR_PUSH_INT, copy_n, 0, 0);
    {
        int id = ir_program_add_string(g->prog, s, slen);
        ir_emit(g->fn, IR_PUSH_STR, id, s, 0);
    }
    ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);
    ir_emit(g->fn, IR_CALL, 3, "memcpy", 0);
    ir_emit(g->fn, IR_POP, 0, 0, 0);
}

static void irgen_local_aggregate_init(IrGen *g, int off, Type *t, AstNode *init_list) {
    int i;
    if (!init_list || init_list->kind != AST_INIT_LIST || !t) return;
    if (t->kind == TYPE_ARRAY) {
        Type *elem_t = t->base;
        int elem_sz = type_size(elem_t);
        int cur_idx = 0;
        if (elem_sz <= 0) elem_sz = 4;
        for (i = 0; i < init_list->u.init_list.item_count; i++) {
            AstNode *it = init_list->u.init_list.items[i];
            int idx;
            AstNode *val;
            if (!it) continue;
            idx = it->u.init_field.has_index ? it->u.init_field.index : cur_idx;
            cur_idx = idx + 1;
            val = it->u.init_field.value;
            if (!val) continue;
            if (val->kind == AST_INIT_LIST) {
                irgen_local_aggregate_init(g, off + idx * elem_sz, elem_t, val);
            } else if (val->kind == AST_STR && elem_t->kind == TYPE_ARRAY &&
                       elem_t->base && elem_t->base->kind == TYPE_CHAR) {
                /* char m[2][6] = {"ab", "cd"}: element is itself an array. */
                irgen_local_store_str_lit(g, off + idx * elem_sz, elem_t, val);
            } else {
                irgen_expr(g, val);
                ir_emit(g->fn, IR_STORE, off + idx * elem_sz, 0, elem_sz);
            }
        }
    } else if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION) {
        StructField *f = t->fields;
        for (i = 0; i < init_list->u.init_list.item_count; i++) {
            AstNode *it = init_list->u.init_list.items[i];
            StructField *target = 0;
            AstNode *val;
            if (!it) continue;
            if (it->u.init_field.has_field) {
                target = type_find_field(t, it->u.init_field.field);
                /* after a designated item the positional cursor moves past target */
                if (target) {
                    StructField *p = t->fields;
                    while (p && p != target) p = p->next;
                    f = p ? p->next : 0;
                }
            } else {
                target = f;
                if (f) f = f->next;
            }
            val = it->u.init_field.value;
            if (!target || !val) continue;
            if (val->kind == AST_INIT_LIST) {
                irgen_local_aggregate_init(g, off + target->offset, target->type, val);
            } else if (val->kind == AST_STR && target->type &&
                       target->type->kind == TYPE_ARRAY && target->type->base &&
                       target->type->base->kind == TYPE_CHAR) {
                /* char s[6] member initialized from a string literal. */
                irgen_local_store_str_lit(g, off + target->offset, target->type, val);
            } else {
                int fsz = type_size(target->type);
                if (fsz <= 0) fsz = 4;
                irgen_expr(g, val);
                ir_emit(g->fn, IR_STORE, off + target->offset, 0, fsz);
            }
        }
    }
    /* Only arrays and structs/unions are initialized here; other types are ignored. */
}

/* C99 compound literal (type){...} -- Bug-I7 FIX.
 * This function is only reached from function-body codegen (irgen_expr and
 * irgen_lvalue_addr), so the compound literal is always block-scope and has
 * automatic storage duration per C99 6.5.2.5. We allocate a stack slot via
 * alloc_local, zero-initialize it (so trailing elements/padding are zero,
 * matching aggregate init rules), then apply the initializer on each entry
 * to the block. File-scope compound literals are handled separately in
 * irgen_program via serialize_init and are unaffected here.
 * Returns the local frame offset of the allocated storage. */
static int irgen_compound_literal(IrGen *g, AstNode *node) {
    Type *t = node->u.compound_lit.lit_type;
    AstNode *init = node->u.compound_lit.init;
    int sz = type_size(t);
    int off;
    int k;
    /* Compound literals of array type with an unspecified dimension
     * (e.g. (int[]){1,2,3}) arrive here with t->size == 0; type_size()
     * then reports a single element's worth of storage. Unlike var
     * decls, nobody calls infer_array_size_from_init on the literal's
     * own type, so complete the dimension from the init list here.
     * This ensures alloc_local reserves space for every element and
     * the zero-init loop covers the whole object instead of just the
     * first slot (whose filler 0 would otherwise share the offset of
     * the first initialized element). */
    if (t && t->kind == TYPE_ARRAY && t->size == 0 && init &&
        init->kind == AST_INIT_LIST) {
        int count = 0;
        int pos = 0;
        int i;
        for (i = 0; i < init->u.init_list.item_count; i++) {
            AstNode *item = init->u.init_list.items[i];
            if (item && item->kind == AST_INIT_FIELD) {
                if (item->u.init_field.has_index) {
                    if (item->u.init_field.index + 1 > count)
                        count = item->u.init_field.index + 1;
                } else {
                    pos++;
                }
            } else {
                pos++;
            }
        }
        if (pos > count) count = pos;
        if (count > 0) {
            t->size = count;
            sz = type_size(t);
        }
    }
    if (sz <= 0) sz = 4;
    off = alloc_local(g, sz);
    /* Zero-initialize the storage so uninitialized elements (trailing array
     * elements, padding, absent struct fields) are zero, matching C99
     * aggregate initialization semantics. Store 4 bytes at a time; the
     * frame is 8-byte aligned so partial trailing words are safe. */
    for (k = 0; k < sz; k += 4) {
        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
        ir_emit(g->fn, IR_STORE, off + k, 0, 4);
    }
    if (init) {
        if (init->kind == AST_INIT_LIST) {
            if (t && (t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT ||
                      t->kind == TYPE_UNION)) {
                /* Aggregate compound literal: reuse the same init path as
                 * local var decls (handles designated initializers, nested
                 * aggregates, etc.). */
                irgen_local_aggregate_init(g, off, t, init);
            } else {
                /* Scalar compound literal: (int){42}, (float){1.5f}, etc.
                 * The init list has a single element; extract and store it. */
                if (init->u.init_list.item_count > 0) {
                    AstNode *v = init->u.init_list.items[0];
                    if (v && v->kind == AST_INIT_FIELD) v = v->u.init_field.value;
                    if (v) {
                        irgen_expr(g, v);
                        ir_emit(g->fn, IR_STORE, off, 0, sz);
                    }
                }
            }
        } else {
            /* Non-INIT_LIST init (shouldn't happen per parser, but handle
             * for safety). */
            irgen_expr(g, init);
            ir_emit(g->fn, IR_STORE, off, 0, sz);
        }
    }
    /* Keep the counter alive for bootstrap ABI compatibility. */
    (void)g_compound_lit_counter;
    return off;
}

/* ROUND4-P2 FIX: Discard an expression result from the stack after a
 * statement-level expression. Direct void function calls don't push a
 * return value (backends skip the push), so no IR_POP is needed. Indirect
 * void calls still conservatively push, so they need the POP. 64-bit
 * returns occupy 2 stack slots and need 2 POPs. */
static void irgen_discard_result(IrGen *g, AstNode *node) {
    Type *et = node ? node->type : 0;
    int words = 1;
    if (et && (et->kind == TYPE_LLONG || et->kind == TYPE_DOUBLE)) {
        words = 2;
    } else if (et && (et->kind == TYPE_STRUCT || et->kind == TYPE_UNION)) {
        /* An aggregate expression is passed as a low/high word pair
         * (backends refuse anything wider than 8 bytes by value), so
         * discarding it has to drop both words.  Popping one left four
         * bytes on the IR stack and the next `ret` jumped to 0. */
        int sz = type_size(et);
        /* Both backends push an aggregate result as a low/high word
         * pair up to 8 bytes; wider composites are passed by address,
         * which is one word.  Match that or the IR stack drifts. */
        words = (sz > 4 && sz <= 8) ? 2 : 1;
    } else if (et && et->kind == TYPE_COMPLEX && g_irgen_complex_words > 1) {
        /* The machine backends carry a complex value as four stack words
         * (imaginary low/high below, real low/high on top); dropping one or
         * two would leave the rest of the value on the IR stack and drift the
         * frame by four or eight bytes per statement. */
        words = g_irgen_complex_words;
    }
    if (words > 1) {
        int k;
        for (k = 0; k < words; k++) ir_emit(g->fn, IR_POP, 0, 0, 0);
    } else if (et && et->kind == TYPE_VOID &&
               node->kind == AST_FUNC_CALL &&
               !(node->u.call.name && node->u.call.name[0] == '?')) {
        /* Direct void call: backend doesn't push a return value, skip POP */
    } else {
        ir_emit(g->fn, IR_POP, 0, 0, 0);
    }
}

static void irgen_stmt(IrGen *g, AstNode *node) {
    if (!node) return;
    g_ir_cur_line = node->line;
    switch (node->kind) {
        case AST_FUNC_DECL: {
            /* A prototype inside a block registers a symbol and emits no
             * code.  irgen rebuilds the symbol table itself (sym_init in
             * irgen_program), so the pre-pass over the program decl list
             * never sees this node; without registering it here a later
             * "p = f;" would emit PUSH_GLOBAL instead of PUSH_FUNC.
             * Parameter types are copied for the same reason as in the
             * pre-pass: 64-bit argument promotion is driven by them. */
            if (node->u.func.name && !sym_find(node->u.func.name)) {
                Symbol *s = sym_add(SYM_FUNC, node->u.func.name, node->u.func.ret_type, 0);
                int pi;
                s->param_count = node->u.func.param_count;
                s->is_variadic = node->u.func.is_variadic;
                /* Same attribute set the file-scope pre-pass copies (see
                 * irgen_program).  Leaving smc_num at the sym_add default
                 * turned an ordinary block-scope prototype such as
                 * "extern int f(void);" into an SMC #0 call site. */
                s->svc_num = node->u.func.svc_num;
                s->smc_num = node->u.func.smc_num;
                s->is_asm_func = node->u.func.is_asm_func;
                s->alias_target = node->u.func.alias_target;
                if (node->u.func.param_count > 0 && node->u.func.params) {
                    s->param_types = (Type **)mem_pool_alloc(sizeof(Type *) * node->u.func.param_count);
                    for (pi = 0; pi < node->u.func.param_count; pi++) {
                        AstNode *pp = node->u.func.params[pi];
                        s->param_types[pi] = (pp && pp->u.var.var_type) ? pp->u.var.var_type : 0;
                    }
                }
            }
            break;
        }
        case AST_BLOCK: {
            int i;
            int saved_bp = g->bp;
            /* Enter a scope for this block and restore the frame cursor on exit. */
            if (!node->u.block.is_decl_list) sym_enter_scope();
            for (i = 0; i < node->u.block.stmt_count; i++) {
                irgen_stmt(g, node->u.block.stmts[i]);
            }
            if (!node->u.block.is_decl_list) sym_leave_scope();
            if (!node->u.block.is_decl_list) g->bp = saved_bp;
            break;
        }
        case AST_VAR_DECL: {
            int sz;
            int off;
            Symbol *s;
            if (node->u.var.is_extern) {
                break;
            }
            if (!node->u.var.name) break;
            /* _Complex float is a real 8-byte object: the operator set keeps
             * it in double precision on the expression stack, and every access
             * to the object carries IR_AUX_COMPLEX so the backends widen on the
             * way in and narrow on the way out (see irgen_is_cplx_float). */
            if (node->u.var.is_static) {
                /* static local: promoted to a function-level global whose mangled name is
                 * ark_sl_<func>_<var>, so the value persists across calls. The symbol is
                 * flagged is_static=1 and registered as SYM_GLOBAL so AST_VAR/AST_ASSIGN
                 * use the PUSH_GLOBAL/STORE_GLOBAL path; the lookup name stays unmangled. */
                char *mangled = static_local_name(g->fn->name, node->u.var.name);
                IrGlobal *gv = ir_program_add_global(g->prog, mangled, node->u.var.var_type);
                gv->is_static = 1;
                gv->is_const = node->u.var.is_const;
                gv->section  = node->u.var.section;
                {
                    Symbol *ss = sym_add(SYM_GLOBAL, node->u.var.name,
                                         node->u.var.var_type, 0);
                    ss->is_static = 1;
                    (void)ss;
                }
                if (node->u.var.init) {
                    AstNode *iv = node->u.var.init;
#if ARKCC_FEATURE_FCONST_FOLD
                    if (irgen_try_complex_init(gv, iv)) {
                        /* _Complex 常量初始化：实部/虚部已折叠 */
                    } else if (irgen_try_float_init(gv, iv)) {
                        /* 同上：函数内 static 的浮点常量表达式 */
                    } else
#endif
                                        if (irgen_try_addr_init(gv, iv)) {
                        /* 静态地址常量初值：符号 + 字节偏移 */
                    } else if (iv->kind == AST_NUM) {
                        gv->has_init = 1;
                        gv->init_int = iv->u.num_val;
                        if (iv->type && (iv->type->kind == TYPE_FLOAT || iv->type->kind == TYPE_DOUBLE)) {
                            gv->is_float = 1;
                            gv->init_float = iv->fval;
                        }
                    } else if (iv->kind == AST_STR) {
                        gv->has_init = 1;
                        gv->init_str = iv->u.name;
                    } else if (iv->kind == AST_INIT_LIST) {
                        StrBuf sb; sb_init(&sb);
                        serialize_init(&sb, iv);
                        gv->has_init = 1;
                        gv->init_aggregate = sb.data;
                    } else {
                        long long cv = 0;
                        gv->has_init = 1;
                        if (eval_const_expr(iv, &cv)) {
                            gv->init_int = cv;
                        } else
#if ARKCC_FEATURE_FCONST_FOLD
                        if (irgen_try_implicit_int_init(gv, iv, &cv)) {
                            /* init is a float constant expression: implicit conversion */
                            gv->init_int = cv;
                        } else
#endif
                        {
                            StrBuf sb; sb_init(&sb);
                            g_ser_unknown = 0;
                            serialize_const_expr(&sb, iv);
                            /* A shape this build cannot place in data must be refused;
                             * writing the object without its initialiser invents zeroes. */
                            if (g_ser_unknown) {
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "the initialiser of the global '%s' is not a constant "
                                      "expression this build can place in data",
                                      gv->name, 0,0,0,0,0);
                            g_irgen_errors++;
                            } else {
                            gv->init_aggregate = sb.data;
                            }
                        }
                    }
                }
                break;
            }
            sz = type_size(node->u.var.var_type);
            if (sz <= 0) sz = 4;
            off = alloc_local(g, sz);
            s = sym_add(SYM_VAR, node->u.var.name, node->u.var.var_type, off);
            (void)s;
#if ARKCC_FEATURE_VLA
            /* B3: variable-length array. var_type is already a pointer flagged is_vla;
             * the slot holds a pointer to the dynamically allocated block (4 bytes).
             * Runtime: element count * element size -> IR_ALLOCA -> store pointer in slot. */
            if (node->u.var.is_vla && g_config.enable_vla) {
                Type *elem_t = node->u.var.var_type->base;
                int elem_sz = type_size(elem_t);
                if (elem_sz <= 0) elem_sz = 4;
                /* Evaluate the element count */
                irgen_expr(g, node->u.var.vla_size_expr);
                /* element count * element size -> total bytes */
                ir_emit(g->fn, IR_PUSH_INT, elem_sz, 0, 0);
                ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_MUL);
                /* Allocate the block on the runtime stack */
                ir_emit(g->fn, IR_ALLOCA, 0, 0, 0);
                /* Save the resulting pointer into the local slot */
                ir_emit(g->fn, IR_STORE, off, 0, 0);
                break;
            }
#endif
            if (node->u.var.init) {
                AstNode *iv = node->u.var.init;
                if (iv->kind == AST_INIT_LIST) {
                    /* Local aggregate initialization: zero the whole region first, then write the
                     * explicitly initialized elements (C99 nested lists, designated initializers). */
                    int total = sz;
                    int k;
                    for (k = 0; k < total; k += 4) {
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        ir_emit(g->fn, IR_STORE, off + k, 0, 4);
                    }
                    irgen_local_aggregate_init(g, off, node->u.var.var_type, iv);
                } else if (iv->kind == AST_STR && node->u.var.var_type &&
                           node->u.var.var_type->kind == TYPE_ARRAY) {
                    /* char a[]="...": call memcpy(frame+off, str, n). Arguments are pushed in
                     * reverse order (the last push becomes arg0), so the forward sequence is
                     * push n, push src, push dst. */
                    const char *s2 = iv->u.name;
                    int slen = ark_strlen(s2);
                    int total = type_size(node->u.var.var_type);
                    int arr_len = node->u.var.var_type->size;
                    int copy_n;
                    int kk;
                    if (arr_len <= 0) arr_len = slen + 1;
                    copy_n = slen + 1;
                    if (copy_n > arr_len) copy_n = arr_len;
                    /* Zero-fill the whole array before copying the literal into it. */
                    for (kk = 0; kk < total; kk += 4) {
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        ir_emit(g->fn, IR_STORE, off + kk, 0, 4);
                    }
                    /* push n, src, dst (the last push becomes arg0 = dst) */
                    ir_emit(g->fn, IR_PUSH_INT, copy_n, 0, 0);
                    {
                        int id = ir_program_add_string(g->prog, s2, slen);
                        ir_emit(g->fn, IR_PUSH_STR, id, s2, 0);
                    }
                    ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);
                    ir_emit(g->fn, IR_CALL, 3, "memcpy", 0);
                    ir_emit(g->fn, IR_POP, 0, 0, 0);
                } else if (node->u.var.var_type &&
                           (node->u.var.var_type->kind == TYPE_STRUCT ||
                            node->u.var.var_type->kind == TYPE_UNION)) {
                    /* Struct/union init from a non-init-list expression.
                     * For small aggregates (<= 8 bytes), irgen_expr pushes the
                     * value as a long long bit pattern (covers AST_FUNC_CALL
                     * returns and AST_VAR struct-to-struct copy). Store it
                     * directly via IR_STORE; memcpy cannot be used because
                     * rvalues have no address (treating the value as a pointer
                     * caused STATUS_ACCESS_VIOLATION).
                     * For larger aggregates (> 8 bytes), require an lvalue
                     * source so we can take its address via irgen_lvalue_addr
                     * (handles AST_COMPOUND_LITERAL, AST_UNARY(*), AST_MEMBER,
                     * AST_ARRAY_ACCESS and AST_VAR of array type).
                     * Exception: AST_FUNC_CALL returning a struct is an rvalue
                     * but the C backend stores the result at frame+8176 and
                     * pushes its ADDRESS (not the value). Use irgen_expr to get
                     * the address, then IR_MEMCPY to copy to the destination. */
                    int struct_sz = type_size(node->u.var.var_type);
                    if (struct_sz <= 0) struct_sz = 4;
                    if (iv && iv->kind == AST_FUNC_CALL && struct_sz > 8) {
                        /* Large struct from function: C backend stores result
                         * at frame+8176 and irgen_expr pushes its ADDRESS.
                         * Use IR_MEMCPY (dst on top, src below) to copy. */
                        irgen_expr(g, iv);                        /* push src addr */
                        ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);  /* push dst addr */
                        ir_emit(g->fn, IR_MEMCPY, struct_sz, 0, 0);
                    } else if (iv && iv->kind == AST_COMPOUND_LITERAL) {
                        /* C99 compound literal: irgen_expr(AST_COMPOUND_LITERAL)
                         * pushes the ADDRESS of the literal's storage for
                         * aggregate types, not the aggregate value. Storing
                         * that as if it were the value wrote the POINTER into
                         * the variable (observed: `struct point p =
                         * (struct point){10,20}` then p.x read the pointer's
                         * low half). Copy through the address instead.
                         * Note this must run before the <=8B value path. */
                        irgen_expr(g, iv);                        /* push src addr */
                        ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);  /* push dst addr */
                        ir_emit(g->fn, IR_MEMCPY, struct_sz, 0, 0);
                    } else if (struct_sz <= 8) {
                        /* Small struct (<=8B): irgen_expr pushes the 8-byte
                         * value (covers AST_FUNC_CALL returning small struct
                         * and AST_VAR struct-to-struct copy). Store directly
                         * via IR_STORE; memcpy cannot be used because rvalues
                         * have no address (treating the value as a pointer
                         * caused STATUS_ACCESS_VIOLATION). */
                        irgen_expr(g, iv);
                        ir_emit(g->fn, IR_STORE, off, 0, 8);
                    } else {
                        /* Large struct var-to-var copy: take source address. */
                        irgen_lvalue_addr(g, iv);                 /* push src addr */
                        ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);  /* push dst addr */
                        ir_emit(g->fn, IR_MEMCPY, struct_sz, 0, 0);
                    }
                } else {
                    irgen_expr(g, iv);
                    {
                        Type *st = iv ? iv->type : 0;
                        Type *dt = node->u.var.var_type;
                        int sf = st && (st->kind == TYPE_FLOAT || st->kind == TYPE_DOUBLE);
                        int df = dt && (dt->kind == TYPE_FLOAT || dt->kind == TYPE_DOUBLE);
                        int si = st && type_is_integer(st);
                        int di = dt && type_is_integer(dt);
                        if (sf && di) {
                            int is_dbl = (st && st->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (dt && dt->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_F2I, 0, 0, aux);
                        }
                        else if (si && df) {
                            int is_dbl = (dt && dt->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (st && st->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_I2F, 0, 0, aux);
                        }
                        if (dt && dt->kind == TYPE_BOOL) {
                            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                            ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_NE);
                        }
                        /* Widen int to long long for 64-bit integer locals.
                         * Without this, `long long x = 42` only pushes 4
                         * bytes (the int literal), but IR_STORE with aux=8
                         * pops 8 bytes, reading garbage as the high word.
                         * irgen_widen_to_64 is a no-op when src is already
                         * 64-bit or when types are not both integers. */
                        if (si && di) {
                            irgen_widen_to_64(g, st, dt);
                        }
                        /* real -> _Complex initialiser (C99 6.7.9 + 6.5.16.1):
                         * the destination object holds four stack words on the
                         * machine backends, so a real source is widened here.
                         * Measured before the fix: `float _Complex z = 3.0f;`
                         * pushed one word, the store popped four, and reading z
                         * gave 0 -- a silent wrong answer, not a crash. */
                        if (dt && dt->kind == TYPE_COMPLEX &&
                            (!st || st->kind != TYPE_COMPLEX)) {
                            irgen_widen_real_to_complex(g, st);
                        }
                    }
                    {
                        /* Bug-N3 FIX: add TYPE_LLONG(8) and TYPE_DOUBLE(8) so
                         * 64-bit locals and doubles are stored with the right
                         * width. Without these cases store_aux stayed 0, which
                         * the ARM backend treats as "default 4 bytes",
                         * corrupting adjacent locals when storing long long or
                         * double values. */
                        int store_aux = 0;
                        if (node->u.var.var_type) {
                            switch (node->u.var.var_type->kind) {
                                case TYPE_CHAR:  store_aux = 1; break;
                                case TYPE_SHORT: store_aux = 2; break;
                                case TYPE_INT:
                                case TYPE_ENUM:  store_aux = 4; break;
                                case TYPE_LLONG:  store_aux = 8; break;
                                case TYPE_DOUBLE: store_aux = 8; break;
                                /* _Complex double is 16 bytes, _Complex float
                                 * is 8. Storing a hard-coded 16 for both wrote
                                 * past the 8-byte float complex object and
                                 * clobbered the neighbouring local. */
                                case TYPE_COMPLEX: store_aux = irgen_complex_aux(node->u.var.var_type, type_size(node->u.var.var_type)); break;
                                default:         store_aux = 0; break;
                            }
                            if (node->u.var.var_type->is_volatile)
                                store_aux |= IR_AUX_VOLATILE;
                        }
                        ir_emit(g->fn, IR_STORE, off, node->u.var.name, store_aux);
                    }
                }
            }
            break;
        }
        case AST_TYPEDEF:
            if (node->u.var.name)
                sym_add(SYM_TYPEDEF, node->u.var.name, node->u.var.var_type, 0);
            break;
        case AST_RETURN:
            if (node->u.unary.operand) {
                /* Struct/union return by value (> 8 bytes): the C backend
                 * stores the result at frame+8176 and returns a pointer.
                 * Push the ADDRESS of the struct (not its truncated value).
                 * For AST_FUNC_CALL, irgen_expr already pushes the result
                 * pointer (the callee returns _ark_ret_buf address).
                 * Small structs (<= 8 bytes) are returned as a value, not
                 * a pointer; irgen_expr pushes the value directly. */
                Type *rt = node->u.unary.operand->type;
                if (!rt) rt = g->fn->ret_type;
                if (rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) &&
                    type_size(rt) > 8) {
                    if (node->u.unary.operand->kind == AST_FUNC_CALL) {
                        irgen_expr(g, node->u.unary.operand);
                    } else {
                        irgen_lvalue_addr(g, node->u.unary.operand);
                    }
                } else if (rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) &&
                           type_size(rt) > 4) {
                    /* 5..8-byte composite return: it travels in r0:r1, i.e. two
                     * words on the IR stack.  Evaluating the struct as a plain
                     * expression pushed only its first word, so the callee's
                     * IR_RETURN popped the second half from nowhere and the
                     * caller stored garbage (`struct P add(...)` returned 2
                     * instead of 10). */
                    if (node->u.unary.operand->kind == AST_FUNC_CALL) {
                        /* The call itself already left both words on the stack. */
                        irgen_expr(g, node->u.unary.operand);
                    } else {
                        irgen_lvalue_addr(g, node->u.unary.operand);
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        ir_emit(g->fn, IR_LOAD_IDX, 8, 0, 0);
                    }
                } else {
                    irgen_expr(g, node->u.unary.operand);
                    /* real -> _Complex return (C99 6.8.6.4): the same
                     * conversion an assignment does.  Without it
                     * `double _Complex f(void) { return 3.0; }` left two words
                     * on the stack where the backends read four. */
                    {
                        Type *ot = node->u.unary.operand->type;
                        Type *ft = g->fn->ret_type;
                        if (ft && ft->kind == TYPE_COMPLEX &&
                            (!ot || ot->kind != TYPE_COMPLEX))
                            irgen_widen_real_to_complex(g, ot);
                    }
                }
            } else {
                ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
            }
            ir_emit(g->fn, IR_RETURN, 0, 0, 0);
            break;
        case AST_IF: {
            int else_l = alloc_label(g);
            int end_l  = alloc_label(g);
            irgen_expr(g, node->u.if_stmt.cond);
            irgen_cond_to_bool(g, node->u.if_stmt.cond);
            ir_emit(g->fn, IR_JZ, else_l, 0, 0);
            irgen_stmt(g, node->u.if_stmt.then_stmt);
            ir_emit(g->fn, IR_JMP, end_l, 0, 0);
            ir_emit(g->fn, IR_LABEL, else_l, 0, 0);
            if (node->u.if_stmt.else_stmt)
                irgen_stmt(g, node->u.if_stmt.else_stmt);
            ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            break;
        }
        case AST_WHILE: {
            int start_l = alloc_label(g);
            int end_l   = alloc_label(g);
            int sb = g->break_label, sc = g->cont_label;
            g->break_label = end_l; g->cont_label = start_l;
            ir_emit(g->fn, IR_LABEL, start_l, 0, 0);
            irgen_expr(g, node->u.while_stmt.cond);
            irgen_cond_to_bool(g, node->u.while_stmt.cond);
            ir_emit(g->fn, IR_JZ, end_l, 0, 0);
            irgen_stmt(g, node->u.while_stmt.body);
            ir_emit(g->fn, IR_JMP, start_l, 0, 0);
            ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            g->break_label = sb; g->cont_label = sc;
            break;
        }
        case AST_FOR: {
            int start_l = alloc_label(g);
            int end_l   = alloc_label(g);
            int cont_l  = alloc_label(g);
            int sb = g->break_label, sc = g->cont_label;
            int saved_bp = g->bp;
            sym_enter_scope();
            g->break_label = end_l; g->cont_label = cont_l;
            if (node->u.for_stmt.init) {
                if (node->u.for_stmt.init->kind == AST_VAR_DECL ||
                    node->u.for_stmt.init->kind == AST_BLOCK)
                    irgen_stmt(g, node->u.for_stmt.init);
                else {
                    irgen_expr(g, node->u.for_stmt.init);
                    irgen_discard_result(g, node->u.for_stmt.init);
                }
            }
            ir_emit(g->fn, IR_LABEL, start_l, 0, 0);
            if (node->u.for_stmt.cond) {
                irgen_expr(g, node->u.for_stmt.cond);
                irgen_cond_to_bool(g, node->u.for_stmt.cond);
                ir_emit(g->fn, IR_JZ, end_l, 0, 0);
            }
            irgen_stmt(g, node->u.for_stmt.body);
            ir_emit(g->fn, IR_LABEL, cont_l, 0, 0);
            if (node->u.for_stmt.step) {
                irgen_expr(g, node->u.for_stmt.step);
                irgen_discard_result(g, node->u.for_stmt.step);
            }
            ir_emit(g->fn, IR_JMP, start_l, 0, 0);
            ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            sym_leave_scope();
            g->bp = saved_bp;
            g->break_label = sb; g->cont_label = sc;
            break;
        }
        case AST_DO_WHILE: {
            int start_l = alloc_label(g);
            int end_l   = alloc_label(g);
            int cont_l  = alloc_label(g);
            int sb = g->break_label, sc = g->cont_label;
            g->break_label = end_l; g->cont_label = cont_l;
            ir_emit(g->fn, IR_LABEL, start_l, 0, 0);
            irgen_stmt(g, node->u.while_stmt.body);
            ir_emit(g->fn, IR_LABEL, cont_l, 0, 0);
            irgen_expr(g, node->u.while_stmt.cond);
            irgen_cond_to_bool(g, node->u.while_stmt.cond);
            ir_emit(g->fn, IR_JNZ, start_l, 0, 0);
            ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            g->break_label = sb; g->cont_label = sc;
            break;
        }
        case AST_SWITCH: {
            int end_l = alloc_label(g);
            int sb = g->break_label;
            CaseList cl;
            int i;
            int *sv_labels = g->case_labels;
            long long *sv_values = g->case_values;
            int sv_count = g->case_count;
            int sv_default = g->default_label;
            int sv_end = g->switch_end_label;
            Type *sw_type = node->u.switch_stmt.expr ? node->u.switch_stmt.expr->type : 0;
            int is_llong_sw = (sw_type && sw_type->kind == TYPE_LLONG);

            cl.labels = 0; cl.values = 0; cl.count = 0; cl.cap = 0;
            cl.default_label = -1;
            collect_cases(g, node->u.switch_stmt.body, &cl);

            g->case_labels = cl.labels;
            g->case_values = cl.values;
            g->case_count = cl.count;
            g->default_label = cl.default_label;
            g->switch_end_label = end_l;
            g->break_label = end_l;

            /* Evaluate switch(expr) once and store in a temp local so the
             * dispatch loop doesn't leak the value onto the stack VM.
             * Each case comparison reloads the temp; after JNZ the stack
             * is balanced regardless of whether a case matched.
             * (Previous DUP-based approach left expr on stack when a case
             * matched via JNZ, leaking 1 slot per switch execution.)
             * For long long switch expressions, use 8-byte store/load and
             * IR_64CMP so the full 64-bit value is compared (not just the low
             * 32 bits, which would make distinct 64-bit cases alias). */
            {
                int switch_temp = alloc_local(g, 8);
                int store_aux = is_llong_sw ? 8 : 0;
                int load_aux  = is_llong_sw ? 8 : 0;
                irgen_expr(g, node->u.switch_stmt.expr);
                ir_emit(g->fn, IR_STORE, switch_temp, 0, store_aux);
                for (i = 0; i < cl.count; i++) {
                    ir_emit(g->fn, IR_PUSH_VAR, switch_temp, 0, load_aux);
                    if (is_llong_sw) {
                        long long cv = cl.values[i];
                        int low32  = (int)(unsigned int)(cv & 0xFFFFFFFFLL);
                        int high32 = (int)(unsigned int)((unsigned long long)cv >> 32);
                        ir_emit(g->fn, IR_64PUSH, high32, 0, low32);
                        ir_emit(g->fn, IR_64CMP, 0, 0,
                                (int)TOK_EQ | (sw_type->is_unsigned ? 0x100 : 0));
                    } else {
                        ir_emit(g->fn, IR_PUSH_INT, cl.values[i], 0, 0);
                        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_EQ);
                    }
                    ir_emit(g->fn, IR_JNZ, cl.labels[i], 0, 0);
                }
            }
            if (cl.default_label >= 0) {
                ir_emit(g->fn, IR_JMP, cl.default_label, 0, 0);
            } else {
                ir_emit(g->fn, IR_JMP, end_l, 0, 0);
            }
            irgen_stmt(g, node->u.switch_stmt.body);
            ir_emit(g->fn, IR_LABEL, end_l, 0, 0);

            g->case_labels = sv_labels;
            g->case_values = sv_values;
            g->case_count = sv_count;
            g->default_label = sv_default;
            g->switch_end_label = sv_end;
            g->break_label = sb;
            break;
        }
        case AST_CASE: {
            /* Emit the case label. The switch value is kept in a temp local (see AST_SWITCH
             * above), so the stack is already balanced when control arrives here and no POP
             * is needed on entry.
             * This also keeps fall-through consistent: the stack depth never changes across
             * case labels, so the fall-through path does not pop a second time.
             *   Each case label entry therefore needs no POP at all. */
            ir_emit(g->fn, IR_LABEL, node->u.case_stmt.label_id, 0, 0);
            /* Fall-through note: the previous case reaches this label with a clean stack
             * (the dispatch loop left nothing behind), so the body runs without any POP
             * and the value is only read from the temp local inside the dispatch loop. */
            irgen_stmt(g, node->u.case_stmt.stmt);
            break;
        }
        case AST_DEFAULT: {
            ir_emit(g->fn, IR_LABEL, node->u.case_stmt.label_id, 0, 0);
            irgen_stmt(g, node->u.case_stmt.stmt);
            break;
        }
        case AST_LABEL:
            ir_emit(g->fn, IR_LABEL_NAMED, 0, node->u.label_stmt.name, 0);
            irgen_stmt(g, node->u.label_stmt.stmt);
            break;
        case AST_GOTO:
            ir_emit(g->fn, IR_JMP_NAMED, 0, node->u.name, 0);
            break;
        case AST_BREAK:
            if (g->break_label >= 0)
                ir_emit(g->fn, IR_JMP, g->break_label, 0, 0);
            break;
        case AST_CONTINUE:
            if (g->cont_label >= 0)
                ir_emit(g->fn, IR_JMP, g->cont_label, 0, 0);
            break;
        case AST_EXPR_STMT:
            if (node->u.unary.operand) {
                irgen_expr(g, node->u.unary.operand);
                irgen_discard_result(g, node->u.unary.operand);
            }
            break;
        case AST_ASM: {
            /* Extended inline assembly: glue the template and the three operand
             *   "asm_text" : outputs : inputs : clobbers
             * sections into a complete C expression string. The C backend passes it
             * through to gcc for operand handling; the ARM backend ignores operands.
             * A leading '@' marks the "full extended form"; without it the name is the
             * plain asm text. */
            const char *text = node->u.asm_stmt.text ? node->u.asm_stmt.text : "";
            const char *outs = node->u.asm_stmt.outputs;
            const char *ins  = node->u.asm_stmt.inputs;
            const char *clb  = node->u.asm_stmt.clobbers;
            if (outs || ins || clb) {
                /* Build the extended form with a leading '@'. */
                StrBuf sb; sb_init(&sb);
                sb_putc(&sb, '@');
                sb_putstr_lit(&sb, text);
                sb_puts(&sb, " : ");
                if (outs) sb_puts(&sb, outs);
                sb_puts(&sb, " : ");
                if (ins) sb_puts(&sb, ins);
                sb_puts(&sb, " : ");
                if (clb) sb_puts(&sb, clb);
                ir_emit(g->fn, IR_ASM, 0, sb.data, 0);
            } else {
                ir_emit(g->fn, IR_ASM, 0, text, 0);
            }
            break;
        }
        default:
            irgen_expr(g, node);
            irgen_discard_result(g, node);
            break;
    }
}

static void irgen_lvalue_addr(IrGen *g, AstNode *node) {
    if (!node) return;
    if (node->kind == AST_VAR) {
        Symbol *s = sym_find(node->u.name);
        if (s && s->kind == SYM_FUNC) {
            /* &func / func in a pointer context: function address. */
            ir_emit(g->fn, IR_PUSH_FUNC, 0, node->u.name, 0);
        } else if (s && s->kind == SYM_GLOBAL) {
            /* Taking the address of a global: emit PUSH_GLOBAL_ADDR so the backend pushes
             * the variable's address. Replaces the old PUSH_INT 0 placeholder (the C backend
             * ignored name and produced address 0). Static locals use the mangled name. */
            const char *gname = node->u.name;
            if (s->is_static) gname = static_local_name(g->fn->name, node->u.name);
            ir_emit(g->fn, IR_PUSH_GLOBAL_ADDR, 0, gname, 0);
        } else if (s) {
            ir_emit(g->fn, IR_PUSH_ADDR, s->offset, node->u.name, 0);
        } else {
            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
        }
        return;
    }
    if (node->kind == AST_COMPOUND_LITERAL) {
        /* Bug-I7 FIX: Block-scope compound literal -- the stack slot's
         * address is the lvalue address. */
        int off = irgen_compound_literal(g, node);
        ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);
        return;
    }
    if (node->kind == AST_ARRAY_ACCESS) {
        AstNode *arr = node->u.array_access.array;
        int elem_sz = 4;
        if (arr->type && (arr->type->kind == TYPE_ARRAY || arr->type->kind == TYPE_PTR))
            elem_sz = type_size(arr->type->base);
        else
            elem_sz = type_size(node->type ? node->type : type_int());
        if (elem_sz <= 0) elem_sz = 4;

        if (arr->kind == AST_VAR) {
            Symbol *s = sym_find(arr->u.name);
            if (s && s->type && s->type->kind == TYPE_ARRAY && s->kind != SYM_GLOBAL) {
                ir_emit(g->fn, IR_PUSH_ADDR, s->offset, arr->u.name, 0);
            } else {
                irgen_expr(g, arr);
            }
        } else {
            irgen_expr(g, arr);
        }
        irgen_expr(g, node->u.array_access.index);
        ir_emit(g->fn, IR_PUSH_INT, elem_sz, 0, 0);
        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_MUL);
        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
        return;
    }
    if (node->kind == AST_UNARY && node->u.unary.op == (int)TOK_MUL) {
        irgen_expr(g, node->u.unary.operand);
        return;
    }
    if (node->kind == AST_MEMBER) {
        AstNode *obj = node->u.member.obj;
        if (node->u.member.is_arrow) {
            irgen_expr(g, obj);
        } else if (obj->kind == AST_FUNC_CALL &&
                   obj->type &&
                   (obj->type->kind == TYPE_STRUCT || obj->type->kind == TYPE_UNION) &&
                   type_size(obj->type) <= 8) {
            /* Small composite returned by value, used as an lvalue base
             * (`f().x`).  The call leaves the value, not an address, so it is
             * materialised in a temporary and the member offset is applied to
             * that.  Without this the first word of the value was used as a
             * pointer (measured: `add(a, b).x` read through address 4). */
            int sz = type_size(obj->type);
            int off = alloc_local(g, sz);
            irgen_expr(g, obj);
            ir_emit(g->fn, IR_STORE, off, 0, (sz > 4) ? 8 : 4);
            ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);
        } else if (obj->kind == AST_FUNC_CALL) {
            /* Function call returning a composite larger than 8 bytes: it hands
             * back the address of its own result buffer, so the pointer can be
             * used directly. */
            irgen_expr(g, obj);
        } else {
            irgen_lvalue_addr(g, obj);
        }
        ir_emit(g->fn, IR_PUSH_INT, node->u.member.offset, 0, 0);
        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
        return;
    }
    {
        DiagLoc loc;
        loc.file = 0;
        loc.line = node->line;
        loc.col  = node->col;
        /* armcc #137, measured on both `(a+1) = 2` and `(a+1) += 2`. */
        diag_report(137, DIAG_SEV_ERROR, &loc, 0, 0, 0);
    }
    g_irgen_errors++;
}

/* Find the bit-field info for an AST_MEMBER node.
 * AST_MEMBER carries no bit_offset/bit_width (see ast.h), so look them up again
 * in obj's struct field chain. Returns 1 for a bit-field, 0 for a non-bit-field
 * or a failed lookup; *pbit_off / *pbit_width give the position inside the unit. */
static int member_bitfield_info(AstNode *node, int *pbit_off, int *pbit_width) {
    AstNode *obj = node->u.member.obj;
    Type *st = 0;
    StructField *f;
    if (!obj) return 0;
    if (node->u.member.is_arrow) {
        if (obj->type && obj->type->kind == TYPE_PTR) st = obj->type->base;
    } else {
        st = obj->type;
    }
    if (!st || (st->kind != TYPE_STRUCT && st->kind != TYPE_UNION)) return 0;
    f = type_find_field(st, node->u.member.member);
    if (!f || f->bit_width <= 0) return 0;
    *pbit_off = f->bit_offset;
    *pbit_width = f->bit_width;
    return 1;
}

/* Widen a 32-bit (or smaller) integer value on the stack to 64-bit
 * for long long assignment, OR narrow a 64-bit value to 32-bit for
 * int/short/char assignment. Called after irgen_expr pushed the RHS value.
 * Stack before: [..., val_low]                   (32-bit src)
 * Stack after:  [..., val_low, val_high]          (widened to 64-bit)
 * OR
 * Stack before: [..., val_low, val_high]         (64-bit src, high on top)
 * Stack after:  [..., val_low]                    (narrowed to 32-bit)
 * If src and dst are same size or non-integer, this is a no-op.
 * Bug-N4 FIX: Correct sign extension using (val < 0) ? -1 : 0 instead of
 * the buggy shrl $31 approach which gives 1 (not -1) for negative values. */
static void irgen_widen_to_64(IrGen *g, Type *src_t, Type *dst_t) {
    int src_size, dst_size;
    if (!src_t || !dst_t) return;
    if (!type_is_integer(src_t) || !type_is_integer(dst_t)) return;
    /* C 后端使用 8 字节 long long 槽，32 位值存入槽时已自动符号扩展，
     * 无需推送额外的高字槽。窄化时也不需要丢弃高字（只有一个槽）。 */
    if (g_irgen_64bit_slots) return;
    src_size = type_size(src_t);
    dst_size = type_size(dst_t);
    if (dst_size == 8 && src_size < 8) {
        if (src_t->is_unsigned) {
            /* Zero extend: high word = 0 */
            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
        } else {
            /* Sign extend: (val < 0) ? -1 : 0
             * DUP + PUSH 0 + LT + NEG correctly produces 0xFFFFFFFF for
             * negative values, unlike shrl which gives 1. */
            ir_emit(g->fn, IR_DUP, 0, 0, 0);
            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
            ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_LT);
            ir_emit(g->fn, IR_UNARY, 0, 0, (int)TOK_MINUS);
        }
    }
    else if (src_size == 8 && dst_size < 8) {
        /* Narrow 64-bit to 32-bit: discard the high word (on top of stack).
         * Without this, IR_STORE with aux=4 pops the high word instead of
         * the low word, storing 0 for values like 42 (high=0, low=42). */
        ir_emit(g->fn, IR_POP, 0, 0, 0);
    }
}

/* Duplicate the top `nw` 32-bit words of the IR stack.
 * A 64-bit value occupies TWO stack words on the register-pair backends
 * (ARM / RV32 / x86-64), so duplicating one word there produced a stack
 * like [low, high, high] and the following 8-byte store wrote the high
 * word into both halves of the object.  Measured before the fix:
 *     long long a; a = -7LL;  ->  a == 0xFFFFFFFFFFFFFFFF  (want -7)
 *     long long a; a =  7LL;  ->  a == 0                (want  7)
 * The C backend keeps a 64-bit value in a single 8-byte slot, hence the
 * g_irgen_64bit_slots guard. */
static void irgen_dup_words(IrGen *g, int nw) {
    if (g_irgen_64bit_slots) nw = 1;
    if (nw < 1) nw = 1;
    ir_emit(g->fn, IR_DUP, nw, 0, 0);
}

/* How many stack words IR_DUP must copy before a store of type t so that the
 * store sees the whole object: a complex value is four words on the machine
 * backends and one slot in the C backend, aggregates and 64-bit scalars are
 * two words, everything else one. */
static int irgen_store_dup_words(Type *t) {
    if (!t) return 1;
    if (t->kind == TYPE_COMPLEX) return g_irgen_complex_words;
    if (type_size(t) >= 8) return 2;
    return 1;
}

/* Is this an 8-byte _Complex float *object* on a machine backend?
 *
 * C99 6.2.5p13: sizeof(_Complex float) == sizeof(float) * 2 == 8.  The
 * expression stack of the register-pair backends, however, carries every
 * complex value as four 32-bit words of *double* precision (imaginary low,
 * imaginary high, real low, real high), because that is what the complex
 * arithmetic helpers (__aeabi_*, __muldc3 ...) consume.  So reading such an
 * object has to widen both halves, and storing has to narrow them back.
 *
 * The conversion lives in the backends, keyed off IR_AUX_COMPLEX together
 * with the 8-byte width, so every place that names a complex object keeps
 * emitting the same IR ops it always did.  The C backend keeps one complex
 * value in a single 8-byte slot, so the flag is never set there. */
static int irgen_is_cplx_float(Type *t) {
    if (!t || t->kind != TYPE_COMPLEX) return 0;
    if (g_irgen_64bit_slots) return 0;
    if (g_irgen_complex_words <= 1) return 0;
    return type_size(t) == 8;
}

/* Object-access aux for type t: the storage width, plus IR_AUX_COMPLEX when
 * the object is an 8-byte _Complex float (see irgen_is_cplx_float). */
static int irgen_complex_aux(Type *t, int width) {
    return irgen_is_cplx_float(t) ? (width | IR_AUX_COMPLEX) : width;
}

/* C99 6.3.1.3: convert the top-of-stack integer to dst_t's representation,
 * i.e. keep the low `bits` bits and re-extend them (sign for signed types,
 * zero for unsigned types). Without this a cast like (char)300 leaves 300 on
 * the stack and a (int)unsigned_4000000000 keeps the unsigned value, so both
 * comparisons and arithmetic afterwards use the wrong value. */
static void irgen_truncate_int(IrGen *g, Type *dst_t) {
    int bits;
    if (!dst_t || !type_is_integer(dst_t)) return;
    switch (dst_t->kind) {
        case TYPE_CHAR:  bits = 8;  break;
        case TYPE_SHORT: bits = 16; break;
        case TYPE_INT:
        case TYPE_ENUM:
        case TYPE_LONG:  bits = 32; break;
        default:         return;    /* llong / bool handled elsewhere */
    }
    ir_emit(g->fn, IR_TRUNC, bits, 0, dst_t->is_unsigned ? 1 : 0);
}

/* Narrow loads land in a 32-bit stack slot, and IR_LOAD_IDX carries the
 * element size but not its signedness: a 1- or 2-byte load has to be
 * re-extended from the element type, otherwise `unsigned char c = 200;
 * c > 100` answers false (the byte was sign-extended) and a `short` above
 * 32767 comes out negative.  Both non-C backends implement IR_TRUNC, so
 * re-using it keeps the two of them identical here. */
static void irgen_load_idx(IrGen *g, Type *elem, int es, int aux) {
    /* An 8-byte _Complex float element has to be widened on the way in;
     * this is the single funnel for array subscripts, `*p` and struct
     * members, so marking it here covers all of them. */
    if (irgen_is_cplx_float(elem)) aux |= IR_AUX_COMPLEX;
    ir_emit(g->fn, IR_LOAD_IDX, es, 0, aux);
    if (!elem) return;
    if (elem->kind == TYPE_CHAR)
        ir_emit(g->fn, IR_TRUNC, 8, 0, elem->is_unsigned ? 1 : 0);
    else if (elem->kind == TYPE_SHORT)
        ir_emit(g->fn, IR_TRUNC, 16, 0, elem->is_unsigned ? 1 : 0);
}

/* After evaluating a condition expression, if its type is 64-bit (8 bytes),
 * collapse the two stack words [lo, hi] into a single 4-byte value
 * (lo | hi) so that IR_JZ/IR_JNZ can correctly test truthiness.
 * Without this, a 64-bit condition like `while (x)` where x < 2^32
 * would only test the high word (which is 0) and exit the loop early.
 * For non-64-bit types, this is a no-op. */
static void irgen_cond_to_bool(IrGen *g, AstNode *cond) {
    if (!cond || !cond->type) return;
    /* C 后端使用 8 字节 long long 槽，64 位值只占一个槽，无需 BOR 合并。 */
    if (g_irgen_64bit_slots) return;
    if (type_size(cond->type) == 8 && type_is_integer(cond->type)) {
        /* Stack: [lo, hi] (hi on top). BOR pops both and pushes lo|hi (4 bytes). */
        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_BOR);
    }
}

/* Emit a binary op with proper 64-bit handling. Used by compound
 * assignment paths (x op= rhs) to mirror AST_BINARY's logic.
 * Both operands are already on the stack. lt/rt are their types. */
/* C99 6.5.6: size of the object a pointer/array operand points to.
 * Returns 0 when the type is neither. */
static int ptr_elem_size(Type *t) {
    int s;
    if (!t) return 0;
    if (t->kind != TYPE_PTR && t->kind != TYPE_ARRAY) return 0;
    if (!t->base) return 0;
    s = type_size(t->base);
    return s > 0 ? s : 1;
}

static void irgen_emit_binary_op(IrGen *g, int op, Type *lt, Type *rt) {
    int is_float = 0, is_llong = 0, is_unsigned = 0, is_double = 0;
    int aux_op = op;
    int ptr_div = 0;

    /* C99 6.5.18 compound assignment on a _Complex operand (z += w).  The
     * plain-expression path in AST_BINARY has handled complex for a while, but
     * this helper -- which is what `z op= rhs` goes through -- only looked at
     * TYPE_FLOAT/TYPE_DOUBLE, so `double _Complex z; z += w;` was emitted as a
     * 32-bit integer add: the real half of w landed in z's real word and the
     * imaginary half was thrown away (measured: `(1+2i) += (3-1i)` answered
     * 4 while the correct value is 4+1i).  An integer op on a 16-byte value
     * cannot be "mostly right", so anything that is not one of the seven
     * defined complex operators is refused instead of being emitted. */
    if ((lt && lt->kind == TYPE_COMPLEX) || (rt && rt->kind == TYPE_COMPLEX)) {
        IrOp fop = 0;
        /* rhs is on top of the stack; a real operand has to become a complex
         * with a zero imaginary part before the pair-wise op. */
        if (rt && rt->kind != TYPE_COMPLEX && rt->kind != TYPE_VOID)
            irgen_widen_real_to_complex(g, rt);
        switch (op) {
            case (int)TOK_PLUS:  fop = IR_FADD; break;
            case (int)TOK_MINUS: fop = IR_FSUB; break;
            case (int)TOK_MUL:   fop = IR_FMUL; break;
            case (int)TOK_DIV:   fop = IR_FDIV; break;
            case (int)TOK_EQ: case (int)TOK_NE:
                fop = IR_FCMP; break;
            default: break;
        }
        if (fop) {
            ir_emit(g->fn, fop, 0, 0, op | IR_AUX_COMPLEX);
            return;
        }
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "operator '%s' is not defined for _Complex values (function '%s')",
                  "<complex>", g->fn->name, 0, 0, 0, 0);
        g_irgen_errors++;
        return;
    }
    /* C99 6.5.6 pointer arithmetic. Both operands are already on the stack
     * with rhs on top, so `ptr +/- int` can scale rhs by sizeof(*ptr) right
     * here, and `ptr - ptr` can divide the byte difference after the sub. */
    {
        int les = ptr_elem_size(lt), res = ptr_elem_size(rt);
        int lf  = lt && (lt->kind == TYPE_FLOAT || lt->kind == TYPE_DOUBLE);
        int rf  = rt && (rt->kind == TYPE_FLOAT || rt->kind == TYPE_DOUBLE);
        if (!lf && !rf && les && (op == (int)TOK_PLUS || op == (int)TOK_MINUS)) {
            if (!res) {
                ir_emit(g->fn, IR_PUSH_INT, les, 0, 0);
                ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_MUL);
            } else if (op == (int)TOK_MINUS) {
                ptr_div = les;
            }
        }
    }
    if ((lt && (lt->kind == TYPE_FLOAT || lt->kind == TYPE_DOUBLE)) ||
        (rt && (rt->kind == TYPE_FLOAT || rt->kind == TYPE_DOUBLE))) {
        is_float = 1;
        if ((lt && lt->kind == TYPE_DOUBLE) || (rt && rt->kind == TYPE_DOUBLE))
            is_double = 1;
    }
    if (!is_float) {
        if (op == (int)TOK_SHL || op == (int)TOK_SHR) {
            /* C99 6.5.7: the result type is the *promoted left* operand, so
             * `long long v; v >> 8` is a 64-bit shift even though the count is
             * an int.  Without is_llong here the shift was emitted as a plain
             * IR_BINARY and only the low word moved -- measured: 300LL >> 8
             * answered 0 on the ARM target while the C backend answered 1.
             * Note the count type never matters: `x << (long long)n` is still a
             * 32-bit shift when x is int. */
            Type *pl = usual_arith_conv(lt, 0);
            is_unsigned = pl && pl->is_unsigned;
            is_llong = pl && pl->kind == TYPE_LLONG;
        } else {
            Type *common = usual_arith_conv(lt, rt);
            is_unsigned = common && common->is_unsigned;
            is_llong = common && common->kind == TYPE_LLONG;
        }
    }
    if (is_float) {
        IrOp fop = IR_FADD;
        switch (op) {
            case (int)TOK_PLUS:  fop = IR_FADD; break;
            case (int)TOK_MINUS: fop = IR_FSUB; break;
            case (int)TOK_MUL:   fop = IR_FMUL; break;
            case (int)TOK_DIV:   fop = IR_FDIV; break;
            case (int)TOK_EQ: case (int)TOK_NE:
            case (int)TOK_LT: case (int)TOK_GT:
            case (int)TOK_LE: case (int)TOK_GE:
                fop = IR_FCMP; break;
            default: fop = IR_FADD; break;
        }
        ir_emit(g->fn, fop, 0, 0, op | (is_double ? 0x200 : 0));
    } else if (is_llong) {
        int is_cmp = (op == (int)TOK_EQ || op == (int)TOK_NE ||
                      op == (int)TOK_LT || op == (int)TOK_GT ||
                      op == (int)TOK_LE || op == (int)TOK_GE);
        if (is_cmp)
            ir_emit(g->fn, IR_64CMP, 0, 0, op | (is_unsigned ? 0x100 : 0));
        else
            ir_emit(g->fn, IR_64BINARY, 0, 0, op | (is_unsigned ? 0x100 : 0));
    } else if (op == (int)TOK_DIV || op == (int)TOK_MOD) {
        IrOp dop = (op == (int)TOK_DIV) ? IR_DIV : IR_MOD;
        ir_emit(g->fn, dop, 0, 0, is_unsigned ? 1 : 0);
    } else {
        if (is_unsigned && (op == (int)TOK_LT || op == (int)TOK_GT ||
                            op == (int)TOK_LE || op == (int)TOK_GE ||
                            op == (int)TOK_SHR))
            aux_op = op | 0x100;
        ir_emit(g->fn, IR_BINARY, 0, 0, aux_op);
    }
    if (ptr_div > 0) {
        ir_emit(g->fn, IR_PUSH_INT, ptr_div, 0, 0);
        ir_emit(g->fn, IR_DIV, 0, 0, 0);
    }
}

/* Widen a value on the stack to 64 bits if the target type requires it.
 * Called after pushing rhs and before the binary op in compound assignment. */
static void irgen_maybe_widen_for_binary(IrGen *g, Type *lt, Type *rt) {
    if (!lt || !rt) return;
    if (lt->kind == TYPE_FLOAT || lt->kind == TYPE_DOUBLE) return;
    if (rt->kind == TYPE_FLOAT || rt->kind == TYPE_DOUBLE) return;
    {
        Type *common = usual_arith_conv(lt, rt);
        if (common && common->kind == TYPE_LLONG) {
            irgen_widen_to_64(g, rt, common);
        }
    }
}

static void irgen_expr(IrGen *g, AstNode *node) {
    if (!node) {
        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
        return;
    }
    g_ir_cur_line = node->line;  /* used for DWARF .loc */
    switch (node->kind) {
        case AST_GENERIC: {
            /* sema replaces a _Generic node with the selected association, so
             * this can only be reached when the semantic pass was skipped.  A
             * silent 0 here would be a wrong answer with no diagnostic. */
            DiagLoc loc;
            loc.file = 0;
            loc.line = node->line;
            loc.col  = node->col;
            diag_report_msg(DIAG_SEV_ERROR, &loc,
                            "_Generic reached code generation unresolved "
                            "(semantic analysis skipped)", 0, 0, 0);
            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
            break;
        }
        case AST_NUM:
            if (node->type && node->type->kind == TYPE_COMPLEX) {
                /* C99 complex constant: num_val == -1 selects the imaginary part
                 * (0, fval); otherwise the real part (fval, 0) is used.
                 * Emitted as IR_PUSH_FLOAT with aux 0x600 (double + complex). */
                double re, im;
                if (node->u.num_val == -1) { re = 0.0; im = node->fval; }
                else { re = node->fval; im = 0.0; }
                {
                    int cid = ir_program_add_complex(g->prog, re, im);
                    ir_emit(g->fn, IR_PUSH_FLOAT, cid, 0, IR_AUX_COMPLEX | 0x200);
                }
                break;
            }
            if (node->type && (node->type->kind == TYPE_FLOAT || node->type->kind == TYPE_DOUBLE)) {
                int fid = ir_program_add_float(g->prog, node->fval);
                /* aux high bit 0x200 marks double precision: the ARM backend then emits
                 * a 64-bit constant. val is still the float constant pool index. */
                int is_dbl = (node->type->kind == TYPE_DOUBLE);
                ir_emit(g->fn, IR_PUSH_FLOAT, fid, 0, is_dbl ? 0x200 : 0);
            } else {
                /* Bug-N1: For long long constants, emit IR_64PUSH which
                 * correctly pushes both 32-bit halves (low word first, high
                 * word on top). The old IR_PUSH_INT with aux=8 only pushed the
                 * low 32 bits on ARM (emit_mov_imm truncates to 32 bits),
                 * silently dropping the high word for values >= 2^31.
                 * IR_64PUSH encoding: val=high32, aux=low32. */
                if (node->type && node->type->kind == TYPE_LLONG) {
                    long long v = node->u.num_val;
                    int low32  = (int)(unsigned int)(v & 0xFFFFFFFFLL);
                    int high32 = (int)(unsigned int)((unsigned long long)v >> 32);
                    ir_emit(g->fn, IR_64PUSH, high32, 0, low32);
                } else {
                    ir_emit(g->fn, IR_PUSH_INT, node->u.num_val, 0, 0);
                }
            }
            break;
        case AST_STR: {
            int slen = ark_strlen(node->u.name);
            int id = ir_program_add_string(g->prog, node->u.name, slen);
#if ARKCC_FEATURE_WCHAR
            /* B2: wide string (node->type is wchar_t*) -> mark the IrString so the
             * C backend emits L"..." and a wchar_t array. */
            if (g_config.enable_wchar && node->type &&
                node->type->kind == TYPE_PTR &&
                node->type->base && node->type->base->kind == TYPE_WCHAR) {
                g->prog->strings[id]->is_wide = 1;
            }
#endif
            ir_emit(g->fn, IR_PUSH_STR, id, node->u.name, 0);
            break;
        }
        case AST_SIZEOF:
            ir_emit(g->fn, IR_PUSH_INT, node->u.num_val, 0, 0);
            break;
        case AST_COMPOUND_LITERAL: {
            /* Bug-I7 FIX: Block-scope compound literal -- allocate on stack.
             * For aggregate types (array/struct/union), the address decays
             * to a pointer (PUSH_ADDR). For scalars, the value is loaded
             * (PUSH_VAR with the correct width/unsigned aux).
             * Use lit_type (the declared type) rather than node->type:
             * sema_expr has no case for AST_COMPOUND_LITERAL, so its default
             * arm clobbers node->type to int. Relying on node->type would
             * misclassify an array/struct compound literal as a scalar and
             * load the first element's value (e.g. 1) instead of pushing the
             * storage address, which then crashes when used as a pointer
             * (Bug-I7 regression). */
            Type *lt = node->u.compound_lit.lit_type;
            int off = irgen_compound_literal(g, node);
            if (lt &&
                (lt->kind == TYPE_ARRAY ||
                 lt->kind == TYPE_STRUCT ||
                 lt->kind == TYPE_UNION)) {
                ir_emit(g->fn, IR_PUSH_ADDR, off, 0, 0);
            } else {
                int push_aux = 0;
                if (lt) {
                    int uns = lt->is_unsigned ? 0x100 : 0;
                    switch (lt->kind) {
                        case TYPE_CHAR:  push_aux = 1; break;
                        case TYPE_SHORT: push_aux = 2; break;
                        case TYPE_INT:
                        case TYPE_ENUM:  push_aux = 4; break;
                        case TYPE_LLONG:  push_aux = 8; break;
                        case TYPE_DOUBLE: push_aux = 8; break;
                        default:         push_aux = 0; break;
                    }
                    push_aux |= uns;
                }
                ir_emit(g->fn, IR_PUSH_VAR, off, 0, push_aux);
            }
            break;
        }
        case AST_VAR: {
            Symbol *s = sym_find(node->u.name);
            if (!s) {
                ir_emit(g->fn, IR_PUSH_INT, 0, node->u.name, 0);
                break;
            }
            if (s->kind == SYM_FUNC) {
                /* C99 6.3.2.1p4: a function designator used in a value
                 * context decays to a pointer to that function. Emitting the
                 * plain variable path here would read garbage from the frame
                 * (function symbols carry offset 0), so take the address. */
                ir_emit(g->fn, IR_PUSH_FUNC, 0, node->u.name, 0);
                break;
            }
            if (s->kind == SYM_GLOBAL) {
                /* static global: keep the name; static local uses mangled name */
                const char *gname = node->u.name;
                if (s->is_static) gname = static_local_name(g->fn->name, node->u.name);
                /* Array/struct/union globals must push their base address (no
                 * dereference): arrays decay to a pointer to their first element,
                 * and structs/unions are accessed via member operations on their
                 * address. PUSH_GLOBAL would emit LDR r0,[r0] which loads 4
                 * bytes of the aggregate's storage as a bogus pointer. */
                if (s->type && (s->type->kind == TYPE_ARRAY ||
                                s->type->kind == TYPE_STRUCT ||
                                s->type->kind == TYPE_UNION)) {
                    ir_emit(g->fn, IR_PUSH_GLOBAL_ADDR, 0, gname, 0);
                } else {
                    ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname,
                            irgen_is_cplx_float(s->type) ? IR_AUX_COMPLEX : 0);
                }
            } else if (s->kind == SYM_ENUM_CONST) {
                ir_emit(g->fn, IR_PUSH_INT, s->enum_value, 0, 0);
            } else if (s->type && s->type->kind == TYPE_ARRAY) {
                ir_emit(g->fn, IR_PUSH_ADDR, s->offset, node->u.name, 0);
            } else {
                /* Encode the access width in aux. Narrow objects (e.g. the plain int
                 * written by remquo through an int*) are still passed in an 8-byte slot,
                 * so the load must sign/zero-extend the 4-byte object into the slot.
                 * Objects that already fill the slot (long / long long / pointers) use
                 * aux = 0, i.e. no extension.
                 * aux bits 0..7 = width (1/2/4), bit 8 (0x100) = unsigned. */
                int aux = 0;
                if (s->type) {
                    int uns = s->type->is_unsigned ? 0x100 : 0;
                    switch (s->type->kind) {
                        case TYPE_CHAR:  aux = 1 | uns; break;
                        case TYPE_SHORT: aux = 2 | uns; break;
                        case TYPE_INT:
                        case TYPE_ENUM:  aux = 4 | uns; break;
                        case TYPE_LLONG: aux = 8 | uns; break;  /* #5 FIX: 64-bit local */
                        case TYPE_DOUBLE: aux = 8; break;       /* #5 FIX: double local */
                        case TYPE_COMPLEX: aux = irgen_complex_aux(s->type, type_size(s->type)); break;
                        default:         aux = 0; break;
                    }
                    /* #11 FIX: Mark volatile variable reads */
                    if (s->type->is_volatile) aux |= IR_AUX_VOLATILE;
                }
                ir_emit(g->fn, IR_PUSH_VAR, s->offset, node->u.name, aux);
            }
            break;
        }
        case AST_CAST:
            irgen_expr(g, node->u.unary.operand);
            /* Float <-> int conversion: emit IR_F2I / IR_I2F per source/dest type.
             * aux high bit 0x200 marks double precision so the ARM backend uses the
             * double helpers (__aeabi_d2iz / __aeabi_i2d) instead of the single ones.
             * F2I checks whether the source is double; I2F checks the destination. */
            {
                Type *src_t = node->u.unary.operand ? node->u.unary.operand->type : 0;
                Type *dst_t = node->type;
                int src_float = src_t && (src_t->kind == TYPE_FLOAT || src_t->kind == TYPE_DOUBLE);
                int dst_float = dst_t && (dst_t->kind == TYPE_FLOAT || dst_t->kind == TYPE_DOUBLE);
                int src_int   = src_t && type_is_integer(src_t);
                int dst_int   = dst_t && type_is_integer(dst_t);
                if (src_float && dst_int) {
                    int is_dbl = (src_t->kind == TYPE_DOUBLE);
                    /* Bug-N2: double -> long long requires IR_64F2I (not
                     * IR_F2I) because IR_F2I truncates to 32-bit int.
                     * aux: 0x200=src is double, 0x400=dst is unsigned.
                     * ARM backend selects __aeabi_d2lz/__aeabi_d2ulz (or
                     * __aeabi_f2lz/__aeabi_f2ulz for float source). */
                    if (dst_t->kind == TYPE_LLONG) {
                        int aux = is_dbl ? 0x200 : 0;
                        if (dst_t->is_unsigned) aux |= 0x400;
                        ir_emit(g->fn, IR_64F2I, 0, 0, aux);
                    } else {
                        int aux = is_dbl ? 0x200 : 0;
                        if (dst_t->is_unsigned) aux |= 0x400;
                        ir_emit(g->fn, IR_F2I, 0, 0, aux);
                    }
                }
                else if (src_int && dst_float) {
                    int is_dbl = (dst_t->kind == TYPE_DOUBLE);
                    /* Bug-N2: long long -> double requires IR_64I2F (not
                     * IR_I2F) because IR_I2F expects a 32-bit int source.
                     * aux: 0x200=dst is double, 0x400=src is unsigned.
                     * ARM backend selects __aeabi_l2d/__aeabi_ul2d (or
                     * __aeabi_l2f/__aeabi_ul2f for float destination). */
                    if (src_t->kind == TYPE_LLONG) {
                        int aux = is_dbl ? 0x200 : 0;
                        if (src_t->is_unsigned) aux |= 0x400;
                        ir_emit(g->fn, IR_64I2F, 0, 0, aux);
                    } else {
                        int aux = is_dbl ? 0x200 : 0;
                        if (src_t->is_unsigned) aux |= 0x400;
                        ir_emit(g->fn, IR_I2F, 0, 0, aux);
                    }
                }
                /* Bug-F2F: float <-> float cast. Previously missing — a double->float
                 * narrowing was a no-op, leaving 8 bytes on the stack when only 4
                 * should be there. This corrupted subsequent stores and crashed
                 * the bootstrap compiler.
                 * aux encoding: 0x200=src is double, 0x100=dst is double.
                 *   double->float  : aux=0x200 (narrow 8->4 bytes)
                 *   float ->double  : aux=0x100 (widen 4->8 bytes)
                 *   same-type cast  : no-op (skip) */
                else if (src_float && dst_float) {
                    int src_dbl = (src_t->kind == TYPE_DOUBLE);
                    int dst_dbl = (dst_t->kind == TYPE_DOUBLE);
                    if (src_dbl != dst_dbl) {
                        int aux = src_dbl ? 0x200 : 0x100;
                        ir_emit(g->fn, IR_F2F, 0, 0, aux);
                    }
                    /* same-size float cast: no-op */
                }
                /* Bug-I1: int(32-bit) <-> long long(64-bit) widening/narrowing.
                 * Stack layout for 64-bit values: low word pushed first, high
                 * word pushed last (high word on top of stack). */
                else if (src_int && dst_int) {
                    /* int(32-bit) <-> long long(64-bit) widening/narrowing.
                     * 委托给 irgen_widen_to_64 统一处理，该函数内部会检查
                     * g_irgen_64bit_slots 标志：C 后端使用 8 字节 long long
                     * 槽时跳过 2 槽 widen/narrow 序列。 */
                    irgen_widen_to_64(g, src_t, dst_t);
                    /* Same-width unsigned<->signed and narrowing casts must
                     * actually reinterpret/truncate the value. */
                    if (dst_t->kind != TYPE_BOOL &&
                        (type_size(dst_t) < type_size(src_t) ||
                         (type_size(dst_t) == 4 && src_t->is_unsigned != dst_t->is_unsigned))) {
                        irgen_truncate_int(g, dst_t);
                    }
                }
                /* complex -> real: use IR_F2I with aux 0x400 (the same encoding as
                 * creal() in the C backend), so no extra conversion is required. */
                if (src_t && src_t->kind == TYPE_COMPLEX &&
                    dst_t && (dst_t->kind == TYPE_FLOAT || dst_t->kind == TYPE_DOUBLE || type_is_integer(dst_t))) {
                    ir_emit(g->fn, IR_F2I, 0, 0, IR_AUX_COMPLEX);  /* creal */
                }
                /* Everything else (int->int, float->float, pointer->pointer) is a no-op. */
            }
            break;
        case AST_COMMA:
            irgen_expr(g, node->u.binary.left);
            ir_emit(g->fn, IR_POP, 0, 0, 0);
            irgen_expr(g, node->u.binary.right);
            break;
        case AST_BINARY: {
            int op = node->u.binary.op;
            if (op == (int)TOK_AND) {
                int false_l = alloc_label(g);
                int end_l = alloc_label(g);
                irgen_expr(g, node->u.binary.left);
                irgen_cond_to_bool(g, node->u.binary.left);
                ir_emit(g->fn, IR_JZ, false_l, 0, 0);
                irgen_expr(g, node->u.binary.right);
                irgen_cond_to_bool(g, node->u.binary.right);
                ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_NE);
                ir_emit(g->fn, IR_JMP, end_l, 0, 0);
                ir_emit(g->fn, IR_LABEL, false_l, 0, 0);
                ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            } else if (op == (int)TOK_OR) {
                int true_l = alloc_label(g);
                int end_l = alloc_label(g);
                irgen_expr(g, node->u.binary.left);
                irgen_cond_to_bool(g, node->u.binary.left);
                ir_emit(g->fn, IR_JNZ, true_l, 0, 0);
                irgen_expr(g, node->u.binary.right);
                irgen_cond_to_bool(g, node->u.binary.right);
                ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_NE);
                ir_emit(g->fn, IR_JMP, end_l, 0, 0);
                ir_emit(g->fn, IR_LABEL, true_l, 0, 0);
                ir_emit(g->fn, IR_PUSH_INT, 1, 0, 0);
                ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            } else {
                int is_float = 0;
                int is_llong = 0;
                int is_unsigned = 0;
                int is_double = 0;
                int ptr_diff_div = 0;
                int aux_op = op;  /* operator sent to the backend; high-bit flags may be added */
                Type *lt = node->u.binary.left ? node->u.binary.left->type : 0;
                Type *rt = node->u.binary.right ? node->u.binary.right->type : 0;
                /* C99 6.5.6: ptr + int == int + ptr. Normalise to "pointer on
                 * the left" so the element-size scaling below can always be
                 * applied to the (top-of-stack) right operand. Operand
                 * evaluation order is unspecified in C, so swapping is safe. */
                if (op == (int)TOK_PLUS && !ptr_elem_size(lt) && ptr_elem_size(rt)) {
                    AstNode *txn = node->u.binary.left;
                    Type *txt = lt;
                    node->u.binary.left  = node->u.binary.right;
                    node->u.binary.right = txn;
                    lt = rt;
                    rt = txt;
                }
                if ((lt && (lt->kind == TYPE_FLOAT || lt->kind == TYPE_DOUBLE)) ||
                    (rt && (rt->kind == TYPE_FLOAT || rt->kind == TYPE_DOUBLE))) {
                    is_float = 1;
                    /* Double precision: if either operand is double, compute as double. */
                    if ((lt && lt->kind == TYPE_DOUBLE) ||
                        (rt && rt->kind == TYPE_DOUBLE)) {
                        is_double = 1;
                    }
                }
                if (!is_float) {
                    /* C99 usual arithmetic conversions (6.3.1.8):
                     *   - arithmetic/comparison ops promote both sides to the common type;
                     *     signedness is taken from that common type.
                     *   - shifts promote only the left operand, whose signedness decides.
                     * irgen inserts no conversions; a correct is_unsigned lets the backend choose. */
                    if (op == (int)TOK_SHL || op == (int)TOK_SHR) {
                        /* Shift: usual_arith_conv(lt, 0) degenerates to promote_integer(lt).
                         * C99 6.5.7 makes the *promoted left operand* the result type, so a
                         * long long left operand needs the 64-bit op (IR_64BINARY); the count
                         * is widened too, because that op pops a 64-bit right operand. A
                         * 32-bit left operand stays a 32-bit shift no matter how wide the
                         * count is. */
                        Type *pl = usual_arith_conv(lt, 0);
                        is_unsigned = pl && pl->is_unsigned;
                        is_llong = pl && pl->kind == TYPE_LLONG;
                    } else {
                        Type *common = usual_arith_conv(lt, rt);
                        is_unsigned = common && common->is_unsigned;
                        is_llong = common && common->kind == TYPE_LLONG;
                    }
                }
                /* C99 complex arithmetic: if either operand is TYPE_COMPLEX the operation
                 * is performed on the pair. A real operand is treated as having an
                 * imaginary part of 0, so both operands are 8-byte pairs (double re + im).
                 * aux 0x600 = double (0x200) + complex (0x400), used by IR_FADD/FSUB/FMUL/FDIV. */
                {
                    int is_complex = 0;
                    if (lt && lt->kind == TYPE_COMPLEX) is_complex = 1;
                    if (rt && rt->kind == TYPE_COMPLEX) is_complex = 1;
                    irgen_expr(g, node->u.binary.left);
                    if (is_llong && !is_complex) {
                        irgen_widen_to_64(g, lt, is_unsigned ? type_ullong() : type_llong());
                    }
                    if (is_complex && lt && lt->kind != TYPE_COMPLEX) {
                        irgen_widen_real_to_complex(g, lt);
                    }
                    irgen_expr(g, node->u.binary.right);
                    if (is_llong && !is_complex) {
                        irgen_widen_to_64(g, rt, is_unsigned ? type_ullong() : type_llong());
                    }
                    /* C99 6.5.6: scale the integer operand of `ptr +/- int` by
                     * sizeof(*ptr). `ptr - ptr` divides after the subtraction. */
                    if (!is_float && !is_complex && ptr_elem_size(lt) &&
                        (op == (int)TOK_PLUS || op == (int)TOK_MINUS)) {
                        if (!ptr_elem_size(rt)) {
                            ir_emit(g->fn, IR_PUSH_INT, ptr_elem_size(lt), 0, 0);
                            ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_MUL);
                        } else if (op == (int)TOK_MINUS) {
                            ptr_diff_div = ptr_elem_size(lt);
                        }
                    }
                    if (is_complex && rt && rt->kind != TYPE_COMPLEX) {
                        irgen_widen_real_to_complex(g, rt);
                    }
                    if (is_complex && (op == (int)TOK_PLUS || op == (int)TOK_MINUS ||
                                       op == (int)TOK_MUL || op == (int)TOK_DIV)) {
                        IrOp fop = IR_FADD;
                        switch (op) {
                            case (int)TOK_PLUS:  fop = IR_FADD; break;
                            case (int)TOK_MINUS: fop = IR_FSUB; break;
                            case (int)TOK_MUL:   fop = IR_FMUL; break;
                            case (int)TOK_DIV:   fop = IR_FDIV; break;
                        }
                        ir_emit(g->fn, fop, 0, 0, op | IR_AUX_COMPLEX);
                        break;  /* exit the AST_BINARY case */
                    }
                    if (is_complex && (op == (int)TOK_EQ || op == (int)TOK_NE)) {
                        ir_emit(g->fn, IR_FCMP, 0, 0, op | IR_AUX_COMPLEX);
                        break;
                    }
                }
                if (is_float) {
                    IrOp fop = IR_FADD;
                    switch (op) {
                        case (int)TOK_PLUS:  fop = IR_FADD; break;
                        case (int)TOK_MINUS: fop = IR_FSUB; break;
                        case (int)TOK_MUL:   fop = IR_FMUL; break;
                        case (int)TOK_DIV:   fop = IR_FDIV; break;
                        case (int)TOK_EQ: case (int)TOK_NE:
                        case (int)TOK_LT: case (int)TOK_GT:
                        case (int)TOK_LE: case (int)TOK_GE:
                            fop = IR_FCMP; break;
                        default: fop = IR_FADD; break;
                    }
                    /* aux high bit 0x200 marks double precision: the ARM backend picks
                     * __aeabi_dadd / __aeabi_fadd; the C backend stores it as double. */
                    ir_emit(g->fn, fop, 0, 0, op | (is_double ? 0x200 : 0));
                } else if (is_llong) {
                    /* 64-bit arithmetic: the C backend reuses IR_BINARY (slots already hold
                     * long long); the ARM backend pops 4 slots, computes 64-bit, pushes 2. */
                    int is_cmp = (op == (int)TOK_EQ || op == (int)TOK_NE ||
                                  op == (int)TOK_LT || op == (int)TOK_GT ||
                                  op == (int)TOK_LE || op == (int)TOK_GE);
                    if (is_cmp) {
                        /* Plain 32-bit IR_BINARY cannot carry an unsigned flag for 64-bit
                         * compares, so IR_64CMP takes it in aux (0x100): an unsigned compare
                         * must not treat e.g. (unsigned long long)-1 as smaller than LLONG_MAX. */
                        ir_emit(g->fn, IR_64CMP, 0, 0, op | (is_unsigned ? 0x100 : 0));
                    } else {
                        ir_emit(g->fn, IR_64BINARY, 0, 0, op | (is_unsigned ? 0x100 : 0));
                    }
                } else if (op == (int)TOK_DIV || op == (int)TOK_MOD) {
                    /* Integer division / modulo use dedicated IR_DIV/IR_MOD with aux=1 meaning
                     * unsigned, letting the ARM backend pick __aeabi_idiv / __aeabi_uidiv. */
                    IrOp dop = (op == (int)TOK_DIV) ? IR_DIV : IR_MOD;
                    ir_emit(g->fn, dop, 0, 0, is_unsigned ? 1 : 0);
                } else {
                    /* Unsigned comparisons (TOK_LT/GT/LE/GE) and right shift (TOK_SHR) carry
                     * the unsigned flag in aux bit 8 (0x100) so the backend picks unsigned
                     * compare conditions / logical shift. EQ/NE compare bits, so no flag is needed. */
                    if (is_unsigned && (op == (int)TOK_LT || op == (int)TOK_GT ||
                                        op == (int)TOK_LE || op == (int)TOK_GE ||
                                        op == (int)TOK_SHR)) {
                        aux_op = op | 0x100;
                    }
                    ir_emit(g->fn, IR_BINARY, 0, 0, aux_op);
                    if (ptr_diff_div > 0) {
                        ir_emit(g->fn, IR_PUSH_INT, ptr_diff_div, 0, 0);
                        ir_emit(g->fn, IR_DIV, 0, 0, 0);
                    }
                }
            }
            break;
        }
        case AST_UNARY: {
            int op = node->u.unary.op;
            if (op == (int)TOK_BAND) {
                irgen_lvalue_addr(g, node->u.unary.operand);
            } else if (op == (int)TOK_MUL) {
                int packed_aux = 0;
                /* C99 6.5.3.2p4: unary * applied to a pointer to a function
                 * yields the function designator, which immediately decays
                 * back to the same pointer. Loading through it would read the
                 * first bytes of the function body as a data pointer. */
                if (node->u.unary.operand && node->u.unary.operand->type &&
                    node->u.unary.operand->type->kind == TYPE_PTR &&
                    (node->u.unary.operand->type->is_func_ptr ||
                     (node->u.unary.operand->type->base &&
                      node->u.unary.operand->type->base->kind == TYPE_FUNC))) {
                    irgen_expr(g, node->u.unary.operand);
                    break;
                }
                /* C99 6.3.2.1: dereferencing a pointer to an array (or to a
                 * struct/union) yields an lvalue that decays to its address in
                 * a value context, exactly like arr[i] does. Loading through it
                 * would read type_size bytes as one integer -- for `*(m + 1)`
                 * on int[2][3] that meant a 12-byte "int" used as a pointer,
                 * which segfaults. */
                if (node->type && (node->type->kind == TYPE_ARRAY ||
                                   node->type->kind == TYPE_STRUCT ||
                                   node->type->kind == TYPE_UNION)) {
                    irgen_expr(g, node->u.unary.operand);
                    break;
                }
                irgen_expr(g, node->u.unary.operand);
                ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                /* __packed pointer dereference: aux 0x400 selects byte-wise unaligned access */
                if (node->u.unary.operand->type &&
                    node->u.unary.operand->type->kind == TYPE_PTR &&
                    node->u.unary.operand->type->is_packed) {
                    packed_aux = 0x400;
                }
                irgen_load_idx(g, node->type, type_size(node->type ? node->type : type_int()), packed_aux);
            } else if (op == (int)TOK_INC || op == (int)TOK_DEC) {
                int delta = (op == (int)TOK_INC) ? 1 : -1;
                AstNode *operand = node->u.unary.operand;
                /* Pointer arithmetic: p++ advances p by sizeof(T) bytes for T *p.
                 * Without this scaling, va_arg(ap, type) using *ap++ (where ap is
                 * long long * from stdarg.h) would only advance ap by 1 byte
                 * instead of 8, causing subsequent va_arg reads to return
                 * misaligned garbage (B06: va_test.c printed "sum = 1" instead
                 * of "sum = 10"). */
                if (operand->type && operand->type->kind == TYPE_PTR && operand->type->base) {
                    int pointed_sz = type_size(operand->type->base);
                    if (pointed_sz > 0) delta *= pointed_sz;
                }
                if (operand->kind == AST_VAR) {
                    Symbol *s = sym_find(operand->u.name);
                    if (!s) { ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0); break; }
                    if (s->kind == SYM_GLOBAL) {
                        /* Global / static-local increment and decrement go through
                         * PUSH_GLOBAL/STORE_GLOBAL; static locals use the mangled name. */
                        const char *gname = operand->u.name;
                        if (s->is_static) gname = static_local_name(g->fn->name, operand->u.name);
                        if (s->type && s->type->kind == TYPE_LLONG) {
                            /* 64-bit global ++/--: same fix as the local case
                             * below -- one 32-bit add would wrap the low half
                             * and leave the high half untouched. */
                            if (node->u.unary.is_postfix) {
                                ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                                ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                            } else {
                                ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                            }
                            ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                            ir_emit(g->fn, IR_PUSH_INT, delta < 0 ? -1 : 0, 0, 0);
                            ir_emit(g->fn, IR_64BINARY, 0, 0, (int)TOK_PLUS);
                            ir_emit(g->fn, IR_STORE_GLOBAL, 0, gname, 0);
                            if (!node->u.unary.is_postfix)
                                ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                        } else if (node->u.unary.is_postfix) {
                            ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                            ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                            ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                            ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
                            ir_emit(g->fn, IR_STORE_GLOBAL, 0, gname, 0);
                        } else {
                            ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                            ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                            ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
                            ir_emit(g->fn, IR_STORE_GLOBAL, 0, gname, 0);
                            ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname, 0);
                        }
                    } else if (s->type && s->type->kind == TYPE_LLONG) {
                        /* 64-bit ++/--: the generic path below pushes one word
                         * and adds 1 with a 32-bit ADD, so `unsigned long long
                         * a = 0xFFFFFFFF; a++;` wrapped the low half and left the
                         * high half alone (measured: a came out 0x0000000000000000
                         * instead of 0x0000000100000000). Use the 64-bit add and
                         * the 8-byte store, and push both halves of the delta. */
                        if (node->u.unary.is_postfix) {
                            ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 8);
                            ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 8);
                        } else {
                            ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 8);
                        }
                        ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);          /* low half */
                        ir_emit(g->fn, IR_PUSH_INT, delta < 0 ? -1 : 0, 0, 0); /* high half */
                        ir_emit(g->fn, IR_64BINARY, 0, 0, (int)TOK_PLUS);
                        ir_emit(g->fn, IR_STORE, s->offset, operand->u.name, 8);
                        if (!node->u.unary.is_postfix)
                            ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 8);
                    } else if (node->u.unary.is_postfix) {
                        ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 0);
                        ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 0);
                        ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
                        ir_emit(g->fn, IR_STORE, s->offset, operand->u.name, 0);
                    } else {
                        ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 0);
                        ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
                        ir_emit(g->fn, IR_STORE, s->offset, operand->u.name, 0);
                        ir_emit(g->fn, IR_PUSH_VAR, s->offset, operand->u.name, 0);
                    }
                } else {
                    /* General lvalue: *ptr, arr[i], s.f, p->f, etc.
                     * Use irgen_lvalue_addr to get the address, then
                     * load/increment/store through it. The AST_VAR path
                     * above handles the common simple-variable case; this
                     * path handles all other lvalue kinds. */
                    int elem_sz = type_size(operand->type ? operand->type : type_int());
                    int packed_aux = 0;
                    if (elem_sz <= 0) elem_sz = 4;
                    /* __packed pointer unaligned access for *ptr */
                    if (operand->kind == AST_UNARY && operand->u.unary.op == (int)TOK_MUL &&
                        operand->u.unary.operand->type &&
                        operand->u.unary.operand->type->kind == TYPE_PTR &&
                        operand->u.unary.operand->type->is_packed) {
                        packed_aux = 0x400;
                    }
                    if (node->u.unary.is_postfix) {
                        /* Postfix: result = old value; store new value as side effect.
                         * Stack trace: [addr] -> [addr,addr] -> [addr,old] ->
                         * [old,addr] -> [old,addr,addr] -> [old,addr,val] ->
                         * [old,addr,val,delta] -> [old,addr,new] -> [old,new] -> [old] */
                        irgen_lvalue_addr(g, operand);
                        ir_emit(g->fn, IR_DUP, 0, 0, 0);
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        irgen_load_idx(g, operand->type, elem_sz, packed_aux);
                        ir_emit(g->fn, IR_SWAP, 0, 0, 0);
                        ir_emit(g->fn, IR_DUP, 0, 0, 0);
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        irgen_load_idx(g, operand->type, elem_sz, packed_aux);
                        ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
                        ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, packed_aux);
                        ir_emit(g->fn, IR_POP, 0, 0, 0);
                    } else {
                        /* Prefix: result = new value; store new value as side effect.
                         * Stack trace: [addr] -> [addr,addr] -> [addr,val] ->
                         * [addr,val,delta] -> [addr,new] -> [new] */
                        irgen_lvalue_addr(g, operand);
                        ir_emit(g->fn, IR_DUP, 0, 0, 0);
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        irgen_load_idx(g, operand->type, elem_sz, packed_aux);
                        ir_emit(g->fn, IR_PUSH_INT, delta, 0, 0);
                        ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_PLUS);
                        ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, packed_aux);
                    }
                }
            } else {
                /* val=1: float unary (backend uses fp stack).
                 * val=2: 64-bit integer unary (neg/not must handle 2 stack slots).
                 * val=0: 32-bit integer unary. */
                int is_float = node->type &&
                    (node->type->kind == TYPE_FLOAT || node->type->kind == TYPE_DOUBLE);
                int is_llong = node->type && node->type->kind == TYPE_LLONG;
                irgen_expr(g, node->u.unary.operand);
                ir_emit(g->fn, IR_UNARY, is_float ? 1 : (is_llong ? 2 : 0), 0, op);
            }
            break;
        }
        case AST_ASSIGN: {
            AstNode *L = node->u.binary.left;
            int op = node->u.binary.op;
            if (op == (int)TOK_ASSIGN) {
                /* Whole struct/union assignment: byte-copy via IR_MEMCPY. Stack: top = dst,
                 * next = src, both popped; the expression value is the dst address, which
                 * an enclosing AST_EXPR_STMT pops in statement context. */
                if (L->type && (L->type->kind == TYPE_STRUCT || L->type->kind == TYPE_UNION)) {
                    int sz = type_size(L->type);
                    if (sz <= 0) sz = 4;
                    if (sz <= 8) {
                        /* Small struct (<=8B) -- value semantics, no memcpy:
                         * push the destination address, then the value, then
                         * IR_STORE_IDX with the value's width. The value has to
                         * be read out of the source object for 5..8 bytes; a
                         * plain irgen_expr() on a struct variable pushes only
                         * the first word (measured: `arr[1] = a` stored the
                         * array-element address as the second word and then
                         * crashed in the next store). */
                        int es = (sz > 4) ? 8 : 4;
                        AstNode *r = node->u.binary.right;
                        irgen_lvalue_addr(g, L);                  /* [dst_addr] */
                        if (r && r->kind == AST_FUNC_CALL) {
                            irgen_expr(g, r);                     /* already value words */
                        } else if (es == 8) {
                            irgen_lvalue_addr(g, r);              /* [dst, addr] */
                            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                            ir_emit(g->fn, IR_LOAD_IDX, 8, 0, 0); /* two words */
                        } else {
                            irgen_expr(g, r);
                        }
                        ir_emit(g->fn, IR_STORE_IDX, es, 0, 0);   /* [value] */
                        break;
                    }
                    /* Large struct: use IR_MEMCPY with addresses.
                     * RHS of struct assignment: if it's a function call
                     * returning a struct, irgen_expr pushes the result address
                     * (C backend stores at frame+8176). Otherwise take the
                     * lvalue address. */
                    irgen_lvalue_addr(g, L);                      /* [dst] */
                    if (node->u.binary.right && node->u.binary.right->kind == AST_FUNC_CALL) {
                        irgen_expr(g, node->u.binary.right);     /* [dst, src] */
                    } else {
                        irgen_lvalue_addr(g, node->u.binary.right);   /* [dst, src] */
                    }
                    irgen_lvalue_addr(g, L);                      /* [dst, src, dst] */
                    ir_emit(g->fn, IR_MEMCPY, sz, 0, 0);          /* pops dst,src; leaves [dst] */
                    break;
                }
                if (L->kind == AST_VAR) {
                    Symbol *s = sym_find(L->u.name);
                    irgen_expr(g, node->u.binary.right);
                    /* float <-> int assignment needs an explicit conversion instruction */
                    {
                        AstNode *rv = node->u.binary.right;
                        Type *st = rv ? rv->type : 0;
                        Type *dt = s ? s->type : 0;
                        int sf = st && (st->kind == TYPE_FLOAT || st->kind == TYPE_DOUBLE);
                        int df = dt && (dt->kind == TYPE_FLOAT || dt->kind == TYPE_DOUBLE);
                        int si = st && type_is_integer(st);
                        int di = dt && type_is_integer(dt);
                        if (sf && di) {
                            int is_dbl = (st && st->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (dt && dt->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_F2I, 0, 0, aux);
                        }
                        else if (si && df) {
                            int is_dbl = (dt && dt->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (st && st->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_I2F, 0, 0, aux);
                        }
                        /* _Bool assignment: normalise to 0/1 (C99 6.3.1.2) */
                        if (dt && dt->kind == TYPE_BOOL) {
                            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                            ir_emit(g->fn, IR_BINARY, 0, 0, (int)TOK_NE);
                        }
                        /* Bug-N4 FIX: Widen 32-bit int to 64-bit for long long
                         * local variable assignment. Without this, IR_STORE with
                         * store_aux=8 pops 8 bytes but only 4 were pushed. */
                        if (si && di) {
                            irgen_widen_to_64(g, st, dt);
                        }
                        /* real -> _Complex assignment (C99 6.5.16.1): the
                         * destination object is four stack words on the machine
                         * backends, so a real source has to be widened here.
                         * Without it `float _Complex z = 3.0f;` pushed one word
                         * and the store popped four -- the frame walked and the
                         * object read back as 0 (measured).  A _Complex source
                         * needs nothing: it is already four words, and the
                         * narrower destination widths are handled by the store
                         * itself. */
                        if (dt && dt->kind == TYPE_COMPLEX &&
                            (!st || st->kind != TYPE_COMPLEX)) {
                            irgen_widen_real_to_complex(g, st);
                        }
                    }
                    if (s && s->kind == SYM_GLOBAL) {
                        const char *gname = L->u.name;
                        if (s->is_static) gname = static_local_name(g->fn->name, L->u.name);
                        /* The store below writes the full object width, so the
                         * duplicated copy must cover every stack word. */
                        irgen_dup_words(g, irgen_store_dup_words(s->type));
                        ir_emit(g->fn, IR_STORE_GLOBAL, 0, gname,
                                irgen_is_cplx_float(s->type) ? IR_AUX_COMPLEX : 0);
                    } else if (s) {
                        int store_aux = 0;
                        if (s && s->type) {
                            switch (s->type->kind) {
                                case TYPE_CHAR:  store_aux = 1; break;
                                case TYPE_SHORT: store_aux = 2; break;
                                case TYPE_INT:
                                case TYPE_ENUM:  store_aux = 4; break;
                                case TYPE_LLONG:  store_aux = 8; break;  /* 64-bit local */
                                case TYPE_DOUBLE: store_aux = 8; break;
                                case TYPE_COMPLEX: store_aux = irgen_complex_aux(s->type, type_size(s->type)); break;
                                default:         store_aux = 0; break;
                            }
                            /* #11 FIX: Mark volatile variable writes */
                            if (s->type->is_volatile) store_aux |= IR_AUX_VOLATILE;
                        }
                        ir_emit(g->fn, IR_STORE, s->offset, L->u.name, store_aux);
                        /* Re-load the stored value: `a = b` keeps a value on the
                         * stack for the enclosing expression. The reload MUST
                         * carry the same storage width as the store, otherwise a
                         * 64-bit assignment pushes one word while
                         * irgen_discard_result pops two -- four bytes of stack
                         * leak per statement, which eventually walks the frame
                         * pointer past the saved r5/r6/r7/lr area and makes the
                         * function return to whatever the shifted push wrote
                         * there (measured: `unsigned long long h; h = v >> 8;`
                         * jumped to 0x789ABCDE, the low half of the value).
                         * The narrow widths also matter for the value itself:
                         * without them `char c; c = 300;` reloads the stale high
                         * bytes of the frame slot instead of the sign-extended
                         * byte. Complex (16) is left alone: it has never pushed
                         * more than one word here. */
                        {
                            int push_aux = store_aux & 0xFF;
                            /* A complex object occupies four stack words on
                             * the machine backends, so the reload has to push
                             * all four; the C backend keeps a complex value in
                             * one slot and its reload has always been a single
                             * word (aux 0). */
                            if (push_aux == 16 && g_irgen_complex_words <= 1)
                                push_aux = 0;
                            /* An 8-byte _Complex float reloads as the same four
                             * double words the store consumed. */
                            if (irgen_is_cplx_float(s->type))
                                push_aux = 8 | IR_AUX_COMPLEX;
                            if (s && s->type && s->type->is_unsigned) push_aux |= 0x100;
                            if (s && s->type && s->type->is_volatile) push_aux |= IR_AUX_VOLATILE;
                            ir_emit(g->fn, IR_PUSH_VAR, s->offset, L->u.name, push_aux);
                        }
                    }
                } else if (L->kind == AST_ARRAY_ACCESS) {
                    AstNode *arr = L->u.array_access.array;
                    int elem_sz = 4;
                    int packed_aux = 0;
                    if (arr->type && (arr->type->kind == TYPE_ARRAY || arr->type->kind == TYPE_PTR))
                        elem_sz = type_size(arr->type->base);
                    if (elem_sz <= 0) elem_sz = 4;
                    /* __packed pointer subscript store: aux 0x400 selects byte-wise access */
                    if (arr->type && arr->type->kind == TYPE_PTR && arr->type->is_packed) {
                        packed_aux = 0x400;
                    }
                    irgen_lvalue_addr(g, L);
                    irgen_expr(g, node->u.binary.right);
                    /* Float<->int conversion for array element assignment
                     * (mirrors AST_VAR path). */
                    {
                        AstNode *rv = node->u.binary.right;
                        Type *st = rv ? rv->type : 0;
                        Type *dt = L->type;
                        int sf = st && (st->kind == TYPE_FLOAT || st->kind == TYPE_DOUBLE);
                        int df = dt && (dt->kind == TYPE_FLOAT || dt->kind == TYPE_DOUBLE);
                        int si = st && type_is_integer(st);
                        int di = dt && type_is_integer(dt);
                        if (sf && di) {
                            int is_dbl = (st && st->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (dt && dt->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_F2I, 0, 0, aux);
                        }
                        else if (si && df) {
                            int is_dbl = (dt && dt->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (st && st->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_I2F, 0, 0, aux);
                        }
                        /* Bug-N4 FIX: Widen 32-bit int to 64-bit for long long
                         * array element assignment. */
                        if (si && di) {
                            irgen_widen_to_64(g, st, dt);
                        }
                    }
                    ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, irgen_complex_aux(L->type, packed_aux));
                } else if (L->kind == AST_UNARY && L->u.unary.op == (int)TOK_MUL) {
                    int elem_sz = type_size(L->type ? L->type : type_int());
                    int packed_aux = 0;
                    if (elem_sz <= 0) elem_sz = 4;
                    irgen_expr(g, L->u.unary.operand);
                    irgen_expr(g, node->u.binary.right);
                    /* __packed pointer dereference store: aux 0x400 for byte-wise access */
                    if (L->u.unary.operand->type &&
                        L->u.unary.operand->type->kind == TYPE_PTR &&
                        L->u.unary.operand->type->is_packed) {
                        packed_aux = 0x400;
                    }
                    /* Float<->int conversion for pointer dereference assignment
                     * (mirrors AST_VAR path). */
                    {
                        AstNode *rv = node->u.binary.right;
                        Type *st = rv ? rv->type : 0;
                        Type *dt = L->type;
                        int sf = st && (st->kind == TYPE_FLOAT || st->kind == TYPE_DOUBLE);
                        int df = dt && (dt->kind == TYPE_FLOAT || dt->kind == TYPE_DOUBLE);
                        int si = st && type_is_integer(st);
                        int di = dt && type_is_integer(dt);
                        if (sf && di) {
                            int is_dbl = (st && st->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (dt && dt->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_F2I, 0, 0, aux);
                        }
                        else if (si && df) {
                            int is_dbl = (dt && dt->kind == TYPE_DOUBLE);
                            int aux = is_dbl ? 0x200 : 0;
                            if (st && st->is_unsigned) aux |= 0x400;
                            ir_emit(g->fn, IR_I2F, 0, 0, aux);
                        }
                        /* Bug-N4 FIX: Widen 32-bit int to 64-bit for long long
                         * pointer dereference assignment. */
                        if (si && di) {
                            irgen_widen_to_64(g, st, dt);
                        }
                    }
                    ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, irgen_complex_aux(L->type, packed_aux));
                } else if (L->kind == AST_MEMBER) {
                    int bo, bw;
                    if (member_bitfield_info(L, &bo, &bw)) {
                        /* #I6 FIX: Set 0x100 flag for signed bitfields in
                         * the LOAD_FLD after assignment. */
                        int fld_aux = bw;
                        if (L->type && !L->type->is_unsigned)
                            fld_aux |= 0x100;
                        irgen_lvalue_addr(g, L);                      /* [addr] */
                        irgen_expr(g, node->u.binary.right);          /* [addr, value] */
                        ir_emit(g->fn, IR_STORE_FLD, bo, 0, bw);      /* [] */
                        irgen_lvalue_addr(g, L);                      /* [addr] */
                        ir_emit(g->fn, IR_LOAD_FLD, bo, 0, fld_aux);   /* [masked_value] */
                    } else {
                        int elem_sz = type_size(L->type ? L->type : type_int());
                        if (elem_sz <= 0) elem_sz = 4;
                        irgen_lvalue_addr(g, L);
                        irgen_expr(g, node->u.binary.right);
                        /* Float<->int conversion for struct member assignment.
                         * Same logic as AST_VAR path: emit IR_F2I/IR_I2F before
                         * IR_STORE_IDX so the value on stack matches the
                         * destination width. Without this, g->init_float = 0
                         * (int->double) leaves only 4 bytes on stack but
                         * IR_STORE_IDX with es=8 pops 8 -> stack corrupt. */
                        {
                            AstNode *rv = node->u.binary.right;
                            Type *st = rv ? rv->type : 0;
                            Type *dt = L->type;
                            int sf = st && (st->kind == TYPE_FLOAT || st->kind == TYPE_DOUBLE);
                            int df = dt && (dt->kind == TYPE_FLOAT || dt->kind == TYPE_DOUBLE);
                            int si = st && type_is_integer(st);
                            int di = dt && type_is_integer(dt);
                            if (sf && di) {
                                int is_dbl = (st && st->kind == TYPE_DOUBLE);
                                int aux = is_dbl ? 0x200 : 0;
                                if (dt && dt->is_unsigned) aux |= 0x400;
                                ir_emit(g->fn, IR_F2I, 0, 0, aux);
                            }
                            else if (si && df) {
                                int is_dbl = (dt && dt->kind == TYPE_DOUBLE);
                                int aux = is_dbl ? 0x200 : 0;
                                if (st && st->is_unsigned) aux |= 0x400;
                                ir_emit(g->fn, IR_I2F, 0, 0, aux);
                            }
                            /* Bug-N4 FIX: Widen 32-bit int to 64-bit for long long
                             * member assignment (e.g. g->init_int = 0 where init_int
                             * is long long). Without this, IR_STORE_IDX with es=8
                             * pops 8 bytes but only 4 were pushed, corrupting stack. */
                            if (si && di) {
                                irgen_widen_to_64(g, st, dt);
                            }
                        }
                        ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, irgen_complex_aux(L->type, 0));
                    }
                } else {
                    {
                        DiagLoc loc;
                        loc.file = 0;
                        loc.line = node->line;
                        loc.col  = node->col;
                        /* armcc #137, measured. */
                        diag_report(137, DIAG_SEV_ERROR, &loc, 0, 0, 0);
                    }
                    g_irgen_errors++;
                    ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                }
            } else {
                int bop = (int)TOK_PLUS;
                switch (op) {
                    case TOK_ADD_ASSIGN: bop = (int)TOK_PLUS;  break;
                    case TOK_SUB_ASSIGN: bop = (int)TOK_MINUS; break;
                    case TOK_MUL_ASSIGN: bop = (int)TOK_MUL;   break;
                    case TOK_DIV_ASSIGN: bop = (int)TOK_DIV;   break;
                    case TOK_MOD_ASSIGN: bop = (int)TOK_MOD;   break;
                    case TOK_AND_ASSIGN: bop = (int)TOK_BAND;  break;
                    case TOK_OR_ASSIGN:  bop = (int)TOK_BOR;   break;
                    case TOK_XOR_ASSIGN: bop = (int)TOK_BXOR;  break;
                    case TOK_SHL_ASSIGN: bop = (int)TOK_SHL;   break;
                    case TOK_SHR_ASSIGN: bop = (int)TOK_SHR;   break;
                    default: break;
                }
                if (L->kind == AST_VAR) {
                    Symbol *s = sym_find(L->u.name);
                    if (s && s->kind == SYM_GLOBAL) {
                        const char *gname = L->u.name;
                        if (s->is_static) gname = static_local_name(g->fn->name, L->u.name);
                        ir_emit(g->fn, IR_PUSH_GLOBAL, 0, gname,
                                irgen_is_cplx_float(s->type) ? IR_AUX_COMPLEX : 0);
                        irgen_expr(g, node->u.binary.right);
                        irgen_maybe_widen_for_binary(g, s->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        irgen_emit_binary_op(g, bop, s->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        irgen_dup_words(g, irgen_store_dup_words(s->type));
                        ir_emit(g->fn, IR_STORE_GLOBAL, 0, gname,
                                irgen_is_cplx_float(s->type) ? IR_AUX_COMPLEX : 0);
                    } else if (s) {
                        int store_aux = 0;
                        int push_aux = 0;
                        if (s && s->type) {
                            int uns = s->type->is_unsigned ? 0x100 : 0;
                            switch (s->type->kind) {
                                case TYPE_CHAR:  store_aux = 1; break;
                                case TYPE_SHORT: store_aux = 2; break;
                                case TYPE_INT:
                                case TYPE_ENUM:  store_aux = 4; break;
                                case TYPE_LLONG:  store_aux = 8; break;
                                case TYPE_DOUBLE: store_aux = 8; break;
                                case TYPE_COMPLEX: store_aux = irgen_complex_aux(s->type, type_size(s->type)); break;
                                default:         store_aux = 0; break;
                            }
                            /* push_aux must carry the storage width and unsigned
                             * flag so IR_PUSH_VAR loads the correct number of
                             * bytes. Without this, 64-bit compound assignments
                             * (x /= 10 where x is long long) only push 4 bytes,
                             * causing __udivdi3 to read garbage for the high word. */
                            push_aux = store_aux | uns;
                            /* #I5 FIX: Preserve volatile qualifier in compound
                             * assignment. Without this, __HAL_RCC_*_ENABLE() macros
                             * doing tmpreg |= RCC->CR lose volatile semantics. */
                            if (s->type->is_volatile) {
                                store_aux |= IR_AUX_VOLATILE;
                                push_aux |= IR_AUX_VOLATILE;
                            }
                        }
                        ir_emit(g->fn, IR_PUSH_VAR, s->offset, L->u.name, push_aux);
                        irgen_expr(g, node->u.binary.right);
                        irgen_maybe_widen_for_binary(g, s->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        irgen_emit_binary_op(g, bop, s->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        ir_emit(g->fn, IR_STORE, s->offset, L->u.name, store_aux);
                        ir_emit(g->fn, IR_PUSH_VAR, s->offset, L->u.name, push_aux);
                    }
                } else if (L->kind == AST_UNARY && L->u.unary.op == (int)TOK_MUL) {
                    /* *ptr OP= val : load old, compute new, store back, leave new on stack.
                     * Bug-I4 FIX: Evaluate the lvalue address ONCE and duplicate it
                     * with IR_DUP so that side effects in L (e.g. i++ in *p++ += 5)
                     * are not doubled. The original address stays at the bottom of
                     * the stack while LOAD_IDX consumes the duplicate; STORE_IDX
                     * then consumes the original, so no SWAP is needed.
                     * Bug-I5 FIX: Preserve volatile semantics on *ptr OP= val so
                     * that LOAD_IDX/STORE_IDX carry IR_AUX_VOLATILE, preventing
                     * optimization passes and host compilers from merging or
                     * eliminating the read-modify-write. */
                    int elem_sz = type_size(L->type ? L->type : type_int());
                    int packed_aux = 0;
                    if (elem_sz <= 0) elem_sz = 4;
                    if (L->u.unary.operand->type &&
                        L->u.unary.operand->type->kind == TYPE_PTR &&
                        L->u.unary.operand->type->is_packed) {
                        packed_aux = 0x400;
                    }
                    if (L->type && L->type->is_volatile) {
                        packed_aux |= IR_AUX_VOLATILE;
                    }
                    irgen_lvalue_addr(g, L);                       /* [addr] */
                    ir_emit(g->fn, IR_DUP, 0, 0, 0);               /* [addr, addr] */
                    ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);           /* [addr, addr, 0] */
                    irgen_load_idx(g, L->type, elem_sz, packed_aux); /* [addr, old_val] */
                    irgen_expr(g, node->u.binary.right);            /* [addr, old_val, rhs] */
                    irgen_maybe_widen_for_binary(g, L->type, node->u.binary.right ? node->u.binary.right->type : 0);
                    irgen_emit_binary_op(g, bop, L->type, node->u.binary.right ? node->u.binary.right->type : 0); /* [addr, new_val] */
                    ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, irgen_complex_aux(L->type, packed_aux)); /* [new_val] */
                } else if (L->kind == AST_ARRAY_ACCESS) {
                    /* arr[i] OP= val
                     * Bug-I4 FIX: Evaluate the lvalue address ONCE and duplicate
                     * it with IR_DUP so that side effects in the index expression
                     * (e.g. i++ in arr[i++] += 5) are not doubled.
                     * Bug-I5 FIX: Preserve volatile semantics on arr[i] OP= val. */
                    AstNode *arr = L->u.array_access.array;
                    int elem_sz = 4;
                    int packed_aux = 0;
                    if (arr->type && (arr->type->kind == TYPE_ARRAY || arr->type->kind == TYPE_PTR))
                        elem_sz = type_size(arr->type->base);
                    if (elem_sz <= 0) elem_sz = 4;
                    if (arr->type && arr->type->kind == TYPE_PTR && arr->type->is_packed) {
                        packed_aux = 0x400;
                    }
                    if (L->type && L->type->is_volatile) {
                        packed_aux |= IR_AUX_VOLATILE;
                    }
                    irgen_lvalue_addr(g, L);                       /* [addr] */
                    ir_emit(g->fn, IR_DUP, 0, 0, 0);               /* [addr, addr] */
                    ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);           /* [addr, addr, 0] */
                    irgen_load_idx(g, L->type, elem_sz, packed_aux); /* [addr, old_val] */
                    irgen_expr(g, node->u.binary.right);            /* [addr, old_val, rhs] */
                    irgen_maybe_widen_for_binary(g, L->type, node->u.binary.right ? node->u.binary.right->type : 0);
                    irgen_emit_binary_op(g, bop, L->type, node->u.binary.right ? node->u.binary.right->type : 0); /* [addr, new_val] */
                    ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, irgen_complex_aux(L->type, packed_aux)); /* [new_val] */
                } else if (L->kind == AST_MEMBER) {
                    int bo, bw;
                    if (member_bitfield_info(L, &bo, &bw)) {
                        /* Bitfield compound-assign: read-mask-modify-write.
                         * The address is evaluated once and duplicated so side
                         * effects in the base object are not repeated, exactly
                         * like the non-bitfield member path below. IR_LOAD_FLD
                         * leaves the extracted field on the stack and
                         * IR_STORE_FLD consumes [addr, value], so the new value
                         * is re-read afterwards to leave the expression's
                         * result on the stack. */
                        int fld_aux = bw;
                        if (L->type && !L->type->is_unsigned) fld_aux |= 0x100;
                        irgen_lvalue_addr(g, L);                       /* [addr] */
                        ir_emit(g->fn, IR_DUP, 0, 0, 0);               /* [addr, addr] */
                        ir_emit(g->fn, IR_LOAD_FLD, bo, 0, fld_aux);   /* [addr, old] */
                        irgen_expr(g, node->u.binary.right);           /* [addr, old, rhs] */
                        irgen_maybe_widen_for_binary(g, L->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        irgen_emit_binary_op(g, bop, L->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        ir_emit(g->fn, IR_STORE_FLD, bo, 0, bw);       /* [] */
                        irgen_lvalue_addr(g, L);                       /* [addr] */
                        ir_emit(g->fn, IR_LOAD_FLD, bo, 0, fld_aux);   /* [new] */
                    } else {
                        int elem_sz = type_size(L->type ? L->type : type_int());
                        int vol_aux = 0;
                        if (elem_sz <= 0) elem_sz = 4;
                        /* Bug-I4 FIX: Evaluate the lvalue address ONCE and
                         * duplicate it with IR_DUP so that side effects in the
                         * member's base object are not doubled.
                         * Bug-I5 FIX: Preserve volatile semantics on s.f OP= val. */
                        if (L->type && L->type->is_volatile) {
                            vol_aux = IR_AUX_VOLATILE;
                        }
                        irgen_lvalue_addr(g, L);                       /* [addr] */
                        ir_emit(g->fn, IR_DUP, 0, 0, 0);               /* [addr, addr] */
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);           /* [addr, addr, 0] */
                        irgen_load_idx(g, L->type, elem_sz, vol_aux); /* [addr, old_val] */
                        irgen_expr(g, node->u.binary.right);            /* [addr, old_val, rhs] */
                        irgen_maybe_widen_for_binary(g, L->type, node->u.binary.right ? node->u.binary.right->type : 0);
                        irgen_emit_binary_op(g, bop, L->type, node->u.binary.right ? node->u.binary.right->type : 0); /* [addr, new_val] */
                        ir_emit(g->fn, IR_STORE_IDX, elem_sz, 0, irgen_complex_aux(L->type, vol_aux)); /* [new_val] */
                    }
                } else {
                    {
                        DiagLoc loc;
                        loc.file = 0;
                        loc.line = node->line;
                        loc.col  = node->col;
                        /* armcc #137, measured on `(a+1) += 2` too. */
                        diag_report(137, DIAG_SEV_ERROR, &loc, 0, 0, 0);
                    }
                    g_irgen_errors++;
                    ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                }
            }
            break;
        }
        case AST_FUNC_CALL: {
            int i;
            int n = node->u.call.arg_count;
            int is_indirect = (node->u.call.name && node->u.call.name[0] == '?');
            Symbol *s = is_indirect ? 0 : sym_find(node->u.call.name);

            /* Every backend's argument tables stop at 64 entries and used to
             * just stop counting there, i.e. the 65th and later arguments were
             * never pushed to the right place -- a wrong answer with no
             * diagnostic.  Refuse the call instead of miscompiling it. */
            if (n > 64) {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "a call passes %s arguments; the backends handle at "
                          "most 64 (the rest would be dropped silently)",
                          diag_num(b1, n), 0, 0, 0, 0, 0);
                g_irgen_errors++;
            }

            /* C99 complex builtins: lower the common ones to plain IR here. The
             * complex layout is 16 bytes (double re + double im) while IR_CALL passes
             * one long long (8 bytes) per argument, so special-case them up front. */
            if (s && n == 1) {
                const char *fn = node->u.call.name;
                if (ark_streq(fn, "creal") || ark_streq(fn, "crealf") || ark_streq(fn, "creall")) {
                    irgen_expr(g, node->u.call.args[0]);
                    ir_emit(g->fn, IR_F2I, 0, 0, IR_AUX_COMPLEX);  /* creal */
                    break;
                }
                if (ark_streq(fn, "cimag") || ark_streq(fn, "cimagf") || ark_streq(fn, "cimagl")) {
                    irgen_expr(g, node->u.call.args[0]);
                    ir_emit(g->fn, IR_F2I, 0, 0, IR_AUX_COMPLEX | 0x100);  /* cimag */
                    break;
                }
                if (ark_streq(fn, "conj") || ark_streq(fn, "conjf") || ark_streq(fn, "conjl")) {
                    irgen_expr(g, node->u.call.args[0]);
                    ir_emit(g->fn, IR_UNARY, 0, 0, (int)TOK_BNOT | IR_AUX_COMPLEX);  /* conj */
                    break;
                }
                if (ark_streq(fn, "cabs") || ark_streq(fn, "cabsf") || ark_streq(fn, "cabsl")) {
                    irgen_expr(g, node->u.call.args[0]);
                    ir_emit(g->fn, IR_F2I, 0, 0, IR_AUX_COMPLEX | 0x200);  /* cabs */
                    break;
                }
                if (ark_streq(fn, "carg") || ark_streq(fn, "cargf") || ark_streq(fn, "cargl")) {
                    irgen_expr(g, node->u.call.args[0]);
                    ir_emit(g->fn, IR_F2I, 0, 0, IR_AUX_COMPLEX | 0x300);  /* carg */
                    break;
                }
                if (ark_streq(fn, "cexp") || ark_streq(fn, "cexpf") || ark_streq(fn, "cexpl")) {
                    irgen_expr(g, node->u.call.args[0]);
                    ir_emit(g->fn, IR_FADD, 0, 0, (int)TOK_PLUS | IR_AUX_COMPLEX | 0x100);  /* cexp */
                    break;
                }
            }

            /* An indirect call has no Symbol, but its callee expression still
             * carries the prototype.  Two shapes reach here:
             *   TYPE_FUNC behind the pointer -- a typedef'd function type, with
             *     the parameter list on the FUNC node;
             *   a `(*fp)(args)` declarator -- the pointer's base is only the
             *     return type and the parameter list is stored on the pointer
             *     type itself (see parse_declarator).
             * Without this the parameter widths were unknown for every
             * function-pointer call, so a 16-byte _Complex argument was
             * refused ("the callee cannot tell a four-word value from one
             * address word") instead of being materialised and passed by
             * address the way a named parameter already was. */
            Type **callee_params = 0;
            int callee_param_count = 0;
            int callee_is_variadic = 0;
            if (!s && node->u.call.callee) {
                Type *ct = node->u.call.callee->type;
                while (ct && ct->kind == TYPE_PTR) {
                    if (ct->is_func_ptr && ct->params) {
                        callee_params = ct->params;
                        callee_param_count = ct->param_count;
                        callee_is_variadic = ct->is_variadic;
                        break;
                    }
                    ct = ct->base;
                }
                if (ct && ct->kind == TYPE_FUNC && ct->params) {
                    callee_params = ct->params;
                    callee_param_count = ct->param_count;
                    callee_is_variadic = ct->is_variadic;
                }
            }
            for (i = n - 1; i >= 0; i--) {
                AstNode *arg = node->u.call.args[i];
                Type *at = arg ? arg->type : 0;
                /* Parameter type from function prototype (if available).
                 * Used to emit 64-bit argument passing conversion for the
                 * x86 cdecl ABI. Falls back to NULL for indirect calls or
                 * variadic args beyond the named parameter list. */
                Type *pt = (s && s->kind == SYM_FUNC &&
                            s->param_types && i < s->param_count)
                           ? s->param_types[i]
                           : (callee_params && i < callee_param_count)
                             ? callee_params[i] : 0;
                /* A complex value is four stack words on the machine
                 * backends, which is more than IR_CALL's one-word-per-argument
                 * transfer can carry.  A *named* parameter of the same kind
                 * already travels as one address word (IR_GETARG with a size
                 * above 8 memcpys the payload into the callee's frame), so the
                 * value is materialised in a temporary and its address is
                 * passed.  Both sides then agree on the wire shape: one word.
                 * Through `...` there is no such agreement -- <stdarg.h> walks
                 * consecutive words -- so that shape stays refused instead of
                 * being passed half-materialised. */
                /* A real argument for a _Complex parameter goes through the
                 * same path: C99 6.5.2.2 converts it first, so the value has to
                 * become four words before it is materialised. */
                int arg_cplx = g_irgen_complex_words > 1 &&
                               ((at && at->kind == TYPE_COMPLEX) ||
                                (pt && pt->kind == TYPE_COMPLEX));
                if (arg_cplx && at && at->kind == TYPE_COMPLEX) {
                    int in_va_tail =
                        (s && s->kind == SYM_FUNC && s->is_variadic &&
                         i >= s->param_count) ||
                        (callee_params && callee_is_variadic &&
                         i >= callee_param_count);
                    if (!pt || in_va_tail) {
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "passing a _Complex value without a _Complex "
                                  "parameter type is not implemented for this "
                                  "target: the callee cannot tell a four-word "
                                  "value from one address word (function '%s')",
                                  g->fn->name, 0, 0, 0, 0, 0);
                        g_irgen_errors++;
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        continue;
                    }
                    if (type_size(pt) == 8) {
                        /* _Complex float: the callee's IR_GETARG sees an 8-byte
                         * object and reads two words from the register pair / the
                         * caller's stack, exactly like a long long, so narrow the
                         * four-word value into an 8-byte temporary and push those
                         * two raw words -- no address.
                         * The decision keys off the PARAMETER's width, not the
                         * expression's: `twice(1.5f + 2.5f * I)` has a
                         * _Complex double expression (the pair arithmetic is
                         * done in double) while the parameter is a 8-byte
                         * _Complex float, and choosing by `at` handed the
                         * callee an address where it expected two f32 words
                         * (measured: cplx_ret_fparam answered 0 instead of 35).
                         * The narrowing store converts each half by itself, so
                         * the four-word value is unchanged. */
                        int ctmp = alloc_local(g, 8);
                        irgen_expr(g, arg);
                        if (!at || at->kind != TYPE_COMPLEX)
                            irgen_widen_real_to_complex(g, at);
                        ir_emit(g->fn, IR_STORE, ctmp, 0, 8 | IR_AUX_COMPLEX);
                        ir_emit(g->fn, IR_PUSH_VAR, ctmp, 0, 8);
                        continue;
                    }
                    {
                        int ctmp = alloc_local(g, 16);
                        irgen_expr(g, arg);
                        if (!at || at->kind != TYPE_COMPLEX)
                            irgen_widen_real_to_complex(g, at);
                        ir_emit(g->fn, IR_STORE, ctmp, 0, 16);
                        ir_emit(g->fn, IR_PUSH_ADDR, ctmp, 0, 0);
                    }
                    continue;
                }
                if (arg_cplx) {
                    /* _Complex parameter, real argument: the widening above is
                     * not reached by the block that only fires for a _Complex
                     * *expression*, so do it here and reuse the wire shape. */
                    int in_va_tail2 =
                        (s && s->kind == SYM_FUNC && s->is_variadic &&
                         i >= s->param_count) ||
                        (callee_params && callee_is_variadic &&
                         i >= callee_param_count);
                    if (!pt || in_va_tail2) {
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "passing a real value to a _Complex parameter "
                                  "through '...' is not implemented for this "
                                  "target (function '%s')",
                                  g->fn->name, 0, 0, 0, 0, 0);
                        g_irgen_errors++;
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        continue;
                    }
                    if (type_size(pt) == 8) {
                        int ctmp2 = alloc_local(g, 8);
                        irgen_expr(g, arg);
                        irgen_widen_real_to_complex(g, at);
                        ir_emit(g->fn, IR_STORE, ctmp2, 0, 8 | IR_AUX_COMPLEX);
                        ir_emit(g->fn, IR_PUSH_VAR, ctmp2, 0, 8);
                        continue;
                    }
                    {
                        int ctmp2 = alloc_local(g, 16);
                        irgen_expr(g, arg);
                        irgen_widen_real_to_complex(g, at);
                        ir_emit(g->fn, IR_STORE, ctmp2, 0, 16);
                        ir_emit(g->fn, IR_PUSH_ADDR, ctmp2, 0, 0);
                    }
                    continue;
                }
                /* struct-by-value comes in three shapes, and the callee --
                 * IR_GETARG, which keys off the size in aux -- decides by size:
                 *   > 8 bytes : the caller passes the *address* and the callee
                 *               memcpy's the payload into its own frame slot;
                 *   5..8 bytes: the two words travel by value (AAPCS puts a
                 *               composite of that size in a register pair);
                 *   <= 4 bytes: one word by value.
                 * The 5..8 byte case used to fall into the plain expression
                 * evaluation below, which pushed only the struct's first word
                 * (`IR_PUSH_VAR` with no width) while the callee read two --
                 * measured: examples/test_struct_byval.c answered 2 instead of
                 * 10 on ARM while the C backend answered 10. */
                if (at && (at->kind == TYPE_STRUCT || at->kind == TYPE_UNION) &&
                    type_size(at) > 8) {
                    /* A composite bigger than two words travels two different
                     * ways, and which one is correct depends on whether the
                     * *callee* can see the type:
                     *   named parameter -- this compiler's own convention: one
                     *     address word, and the callee's IR_GETARG memcpys the
                     *     payload into its frame;
                     *   `...` (unnamed) -- the callee has no type for it and
                     *     <stdarg.h> walks consecutive argument words, so the
                     *     payload itself must be on the wire.  Passing the
                     *     address here (what this code used to do for both)
                     *     made va_arg read the address word plus two garbage
                     *     words: measured as 227 instead of 6 in the va gate.
                     *     AAPCS has no indirection for this case either:
                     *     arm-none-eabi-gcc 12.3 passes `f(1, s12)` in
                     *     r1,r2,r3 with the three words by value.
                     * The words go highest-first so that word 0 ends up on
                     * top, mirroring "arg0 is on top of arg1"; the address is
                     * kept on top with DUP/SWAP so a call result is evaluated
                     * exactly once, and dropped at the end. */
                    int vsz = type_size(at);
                    int vw = (vsz + 3) / 4;
                    if ((s && s->kind == SYM_FUNC && s->is_variadic &&
                         i >= s->param_count) ||
                        (callee_params && callee_is_variadic &&
                         i >= callee_param_count)) {
                        if (!g_ir_vararg_words) {
                            char b1[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "passing a %s-byte structure through '...' "
                                      "is implemented for the arm target only; "
                                      "the callee would read the pointer word as "
                                      "data (function '%s')",
                                      diag_num(b1, vsz), g->fn->name, 0,0,0,0);
                            g_irgen_errors++;
                            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        } else {
                            int packed_aux = (type_align(at) < 4) ? 0x400 : 0;
                            int j;
                            if (arg->kind == AST_FUNC_CALL)
                                irgen_expr(g, arg);   /* result buffer address */
                            else
                                irgen_lvalue_addr(g, arg);
                            for (j = vw - 1; j >= 0; j--) {
                                ir_emit(g->fn, IR_DUP, 0, 0, 0);
                                /* IR_LOAD_IDX scales its offset by the element
                                 * size, so this is a word *index*, not a byte
                                 * offset (passing 4*j read word 4j). */
                                ir_emit(g->fn, IR_PUSH_INT, (long long)j, 0, 0);
                                ir_emit(g->fn, IR_LOAD_IDX, 4, 0, packed_aux);
                                ir_emit(g->fn, IR_SWAP, 0, 0, 0);
                            }
                            ir_emit(g->fn, IR_POP, 0, 0, 0);   /* drop the address */
                        }
                    } else if (arg->kind == AST_FUNC_CALL)
                        irgen_expr(g, arg);          /* already the result buffer address */
                    else
                        irgen_lvalue_addr(g, arg);
                } else if (at && (at->kind == TYPE_STRUCT || at->kind == TYPE_UNION) &&
                           type_size(at) > 4) {
                    if (arg->kind == AST_FUNC_CALL) {
                        irgen_expr(g, arg);          /* already two words */
                    } else {
                        irgen_lvalue_addr(g, arg);          /* [addr] */
                        ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                        ir_emit(g->fn, IR_LOAD_IDX, 8, 0, 0);  /* two words, low first */
                    }
                } else {
                    irgen_expr(g, arg);
                    /* 64-bit argument passing fix for 32-bit x86 cdecl ABI:
                     * Ensure the value on the stack is in cdecl order when
                     * the callee expects a long long (8-byte) parameter.
                     *   - If arg is already long long: convert from internal
                     *     model (high word on top) to cdecl order (low word
                     *     on top) via IR_CDECL64.
                     *   - If arg is int/short/char/ptr (<=4 bytes): promote
                     *     to 8 bytes via IR_SEXT (signed) or IR_ZEXT (unsigned).
                     * The C/ARM/RISC-V backends treat these as no-ops. */
                    if (pt && pt->kind == TYPE_LLONG) {
                        if (at && at->kind == TYPE_LLONG) {
                            ir_emit(g->fn, IR_CDECL64, 0, 0, 0);
                        } else {
                            if (at && (at->is_unsigned || at->kind == TYPE_PTR ||
                                       at->kind == TYPE_ARRAY)) {
                                ir_emit(g->fn, IR_ZEXT, 0, 0, 0);
                            } else {
                                ir_emit(g->fn, IR_SEXT, 0, 0, 0);
                            }
                        }
                    } else if (!pt && at && at->kind == TYPE_LLONG) {
                        /* Variadic long long arg (no prototype): convert
                         * internal word order to cdecl order. */
                        ir_emit(g->fn, IR_CDECL64, 0, 0, 0);
                    }
                    /* C99 6.5.2.2p6: default argument promotion turns a float
                     * variadic argument into a double.  Without it the caller
                     * handed over a 4-byte float while va_arg(ap, double) read
                     * 8 bytes, so `printf("%f", 1.5f)`-shaped calls read the
                     * next argument word as the high half (measured on ARM
                     * against gcc: the promoted value must be 8 bytes wide).
                     * The matching 8-byte-width flag is set on argmask below. */
                    if (((s && s->kind == SYM_FUNC && s->is_variadic &&
                          i >= s->param_count) ||
                         (callee_params && callee_is_variadic &&
                          i >= callee_param_count)) &&
                        at && at->kind == TYPE_FLOAT) {
                        ir_emit(g->fn, IR_F2F, 0, 0, 0x100);
                    }
                }
            }
            /* Indirect call: push the function pointer (callee) last so it
             * becomes aa[0] when the C backend pops. n includes the pointer. */
            if (is_indirect && node->u.call.callee) {
                irgen_expr(g, node->u.call.callee);
                n = n + 1;
            }
            /* __svc(n) function: emit an SVC instruction instead of BL */
            /* A name that resolves to an object (function pointer variable)
             * is not a direct call; only real function symbols may become
             * SVC/SMC. sema normally rewrites such calls to indirect form;
             * this is the second line of defense. */
            if (s && s->kind == SYM_FUNC && s->svc_num != 0) {
                ir_emit(g->fn, IR_SVC, s->svc_num, node->u.call.name, 0);
            }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* __smc(n) function: emit an SMC instruction instead of BL */
            else if (s && s->kind == SYM_FUNC && s->smc_num >= 0) {
                ir_emit(g->fn, IR_SMC, s->smc_num, node->u.call.name, 0);
            }
#endif
            else {
                /* Encode argument types into aux bitmask so the C backend can emit
                 * the correct cast for each variadic argument. Each arg uses 2 bits:
                 *   bit(2*i)   = 1 if arg is float/double (default-promoted to double)
                 *   bit(2*i+1) = 1 if arg is long long / unsigned long long (8 bytes)
                 * Default (both bits 0) = int/ptr/short/char (promoted to int, 4 bytes
                 * in the variadic ABI on 32-bit hosts; 8 bytes on 64-bit LP64 but the
                 * high 32 bits are 0). Without this, printf("%d %lld %d", ...) on a
                 * 32-bit host would misread argument offsets because the callee's
                 * va_arg(int) reads 4 bytes while arkcc passed 8, shifting all
                 * subsequent %lld / %s reads and producing garbage.
                 *
                 * NOTE: use arg_count, not n, as the upper bound. For indirect calls
                 * (obj->fnptr()), n includes the function pointer (n = arg_count + 1),
                 * but the callee pointer is stored separately in node->u.call.callee,
                 * NOT in the args[] array. Using n here would read past args[] end
                 * (or dereference NULL when arg_count==0), causing a crash. */
                int argmask = 0;
                /* Wire size of each argument in bytes, and whether any of them
                 * is a size the 2-bit-per-argument argmask cannot state (a
                 * >8-byte composite through `...`).  Collected for every
                 * argument, not just the first 16, because the table has to
                 * describe the whole call. */
                int wires[64];
                int wire_extra = 0;
                int wire_idx = 0;   /* 1 + the run's index in g_ir_argbytes[] */
                int max_args = node->u.call.arg_count;
                if (max_args > 64) max_args = 64;
                for (i = 0; i < max_args; i++) {
                    AstNode *arg = node->u.call.args[i];
                    /* Prefer PARAMETER type for argmask: the x86 backend uses
                     * argmask bit(2*i+1) to decide stack cleanup (8 vs 4 bytes).
                     * If we promoted an int arg to long long for a long long
                     * parameter, the callee expects 8 bytes, so cleanup must
                     * be 8. Fall back to arg type for variadic/unprototyped. */
                    Type *pt = (s && s->kind == SYM_FUNC &&
                                s->param_types && i < s->param_count)
                               ? s->param_types[i]
                               : (callee_params && i < callee_param_count)
                                 ? callee_params[i] : 0;
                    Type *use_t = pt ? pt : (arg ? arg->type : 0);
                    wires[i] = 4;      /* one word unless a wider shape says otherwise */
                    if (use_t) {
                        int k = use_t->kind;
                        /* A float variadic argument has been promoted to double
                         * (see the IR_F2F above), so it occupies 8 bytes on the
                         * wire as well; the width bit is what the ARM/RV32I
                         * backends place the argument by. */
                        if (((s && s->kind == SYM_FUNC && s->is_variadic &&
                              i >= s->param_count) ||
                             (callee_params && callee_is_variadic &&
                              i >= callee_param_count)) &&
                            k == TYPE_FLOAT) {
                            if (i < 16) argmask |= (1 << (2 * i + 1));
                            wires[i] = 8;
                        }
                        /* bit(2*i)   = 1 => float/double (promoted to double)
                         * bit(2*i+1) = 1 => 8-byte type (long long OR double)
                         * Double sets BOTH bits: bit(2*i) for float/double cast
                         * (C backend checks this first), bit(2*i+1) for 8-byte
                         * AAPCS alignment (ARM even-register-pair). Float sets
                         * only bit(2*i) since it is 4 bytes on ARM. */
                        if (i < 16 && (k == TYPE_FLOAT || k == TYPE_DOUBLE)) {
                            argmask |= (1 << (2 * i));
                        }
                        if (i < 16 && (k == TYPE_LLONG || k == TYPE_DOUBLE)) {
                            argmask |= (1 << (2 * i + 1));
                        }
                        if (k == TYPE_LLONG || k == TYPE_DOUBLE) wires[i] = 8;
                        /* _Complex float: an 8-byte object passed as two
                         * words.  Without the 8-byte wire width the ARM
                         * backend popped only r0 and left the imaginary
                         * word stranded on the stack. */
                        if (k == TYPE_COMPLEX && type_size(use_t) == 8) {
                            if (i < 16) argmask |= (1 << (2 * i + 1));
                            wires[i] = 8;
                        }
                        /* A struct/union of 5..8 bytes travels as an 8-byte
                         * value (two words), so it needs the same even-register
                         * pair / 8-byte stack slot treatment as long long.
                         * Without the bit the ARM backend spread a register
                         * argument across two non-paired registers while the
                         * callee read r0:r1. */
                        if ((k == TYPE_STRUCT || k == TYPE_UNION) &&
                            use_t->kind != TYPE_PTR) {
                            int ssz = type_size(use_t);
                            if (ssz > 4 && ssz <= 8) {
                                if (i < 16) argmask |= (1 << (2 * i + 1));
                                wires[i] = 8;
                            } else if (ssz > 8 && pt == 0) {
                                /* Variadic composite: the payload travels
                                 * inline (see the push above), one word per
                                 * four bytes.  A *named* parameter of the same
                                 * size travels as one pointer word, which is
                                 * what wires[] keeps (4) -- it is not this
                                 * branch because pt != 0. */
                                if ((s && s->kind == SYM_FUNC &&
                                     s->is_variadic && i >= s->param_count) ||
                                    (callee_params && callee_is_variadic &&
                                     i >= callee_param_count)) {
                                    wires[i] = (ssz + 3) & ~3;
                                    if (wires[i] != 8) wire_extra = 1;
                                }
                            }
                        }
                    }
                }
                if (wire_extra) {
                    int start = ir_program_add_argbytes(g->prog, wires, max_args);
                    if (start >= 0) wire_idx = start + 1;
                }
                /* Encode indirect-call 64-bit return flag in bit 32 of val.
                 * All backends truncate val to int for the arg count, so the
                 * high bit is free. The ARM backend checks it to decide whether
                 * to push r1 (high word) alongside r0 after an indirect call. */
                {
                    long long call_val = n;
                    /* Result shape, taken from the *call expression's* type --
                     * which is the same thing irgen_discard_result uses to decide
                     * how many words to pop, so the two sides cannot disagree:
                     *   bit 32: the result is two words (long long / double)
                     *   bit 33: the result is void (caller pushes nothing)
                     * The backends previously looked this up in prog->funcs, i.e.
                     * only in functions *defined by this translation unit*.  A
                     * declared-only callee therefore got "returns one word":
                     * `extern void h(void); h();` pushed r0 that nobody popped
                     * (4 bytes of stack leaked per call -- on the board this left
                     * the MiniApp frame 4 bytes off and broke va_arg's 8-byte
                     * alignment), and `extern long long e(void); return e();`
                     * silently dropped the high word.
                     * Both bits are above the argument count, which every backend
                     * truncates to int. */
                    if (node->type &&
                        (node->type->kind == TYPE_LLONG || node->type->kind == TYPE_DOUBLE))
                        call_val |= 0x100000000LL;
                    if (node->type && node->type->kind == TYPE_VOID)
                        call_val |= 0x200000000LL;
                    /* bits 34..35: the result is a _Complex value on the
                     * machine backends, which carry it as four stack words that
                     * no register pair can hold.  The backend turns it into
                     * 1 = _Complex float (8-byte object, r0:r1 / a0:a1) or
                     * 2 = _Complex double (16-byte object, sret pointer).
                     * Without the flag an *indirect* call (a function pointer,
                     * whose type is not in prog->funcs) would leave a single
                     * word on the stack and every later complex operation would
                     * read three words of garbage. */
                    if (node->type && node->type->kind == TYPE_COMPLEX &&
                        g_irgen_complex_words > 1)
                        call_val |= (type_size(node->type) > 8 ? 2LL : 1LL) << 34;
                    /* bits 36..52: the call returns a struct/union wider than
                     * two words, so the payload comes back through a buffer the
                     * *caller* owns (AAPCS sret on ARM, the hidden first
                     * argument of the RISC-V psABI).  bit 36 marks the call,
                     * bits 37..52 carry the payload size in bytes.  Stamping it
                     * here -- from the call expression's own type -- is what
                     * lets a callee that is only *declared*, or one reached
                     * through a function pointer, be handled like a callee
                     * defined in this file; the name tables the arm backend used
                     * to consult could not see either.  Backends that return
                     * such a composite by address (arm, riscv32) read both
                     * fields; the slot-image backends never look above bit 34,
                     * so their behaviour is unchanged. */
                    if (node->type &&
                        (node->type->kind == TYPE_STRUCT || node->type->kind == TYPE_UNION)) {
                        int rsz = type_size(node->type);
                        if (rsz > 8) {
                            char b1[12];
                            if (rsz > 0xFFFF) {
                                diag_tool(DIAG_SEV_ERROR, "arkcc",
                                          "a call returns a %s-byte composite by "
                                          "value; a call records at most 65535 "
                                          "bytes of result buffer",
                                          diag_num(b1, rsz), 0, 0, 0, 0, 0);
                                g_irgen_errors++;
                            } else {
                                call_val |= 0x1000000000LL;          /* bit 36 */
                                call_val |= ((long long)rsz) << 37;  /* size */
                            }
                        }
                    }
                    ir_emit(g->fn, IR_CALL, call_val, node->u.call.name, argmask);
                    /* ir_emit() has no aux2 parameter on purpose: it is used in
                     * hundreds of places and all but this one leave it 0. */
                    if (wire_idx)
                        g->fn->code[g->fn->code_count - 1].aux2 = wire_idx;
                }
            }
            break;
        }
        case AST_ARRAY_ACCESS: {
            int elem_sz;
            int packed_aux = 0;
            AstNode *arr = node->u.array_access.array;
            /* Infer the type of the subscript result: if arr is an array/pointer the
             * result is the element type. An array-valued result (matrix[1] is still
             * int[3]) decays to a pointer to its first element per C semantics -- push
             * the address only, no IR_LOAD_IDX. Scalars load at offset 0 below. */
            if (!node->type && arr && arr->type &&
                (arr->type->kind == TYPE_ARRAY || arr->type->kind == TYPE_PTR)) {
                node->type = arr->type->base;
            }
            if (node->type && node->type->kind == TYPE_ARRAY) {
                irgen_lvalue_addr(g, node);
                return;
            }
            if (arr->type && (arr->type->kind == TYPE_ARRAY || arr->type->kind == TYPE_PTR))
                elem_sz = type_size(arr->type->base);
            else
                elem_sz = type_size(node->type ? node->type : type_int());
            if (elem_sz <= 0) elem_sz = 4;
            /* __packed pointer subscript load: aux 0x400 selects byte-wise unaligned access */
            if (arr->type && arr->type->kind == TYPE_PTR && arr->type->is_packed) {
                packed_aux = 0x400;
            }
            irgen_lvalue_addr(g, node);
            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
            irgen_load_idx(g, node->type, elem_sz, packed_aux);
            break;
        }
        case AST_MEMBER: {
            int bo, bw;
            if (member_bitfield_info(node, &bo, &bw)) {
                /* #I6 FIX: Set 0x100 flag for signed bitfields so the ARM
                 * backend can apply sign extension (LSL+ASR) instead of
                 * zero extension (LSR+AND). */
                int fld_aux = bw;
                if (node->type && !node->type->is_unsigned)
                    fld_aux |= 0x100;
                irgen_lvalue_addr(g, node);
                ir_emit(g->fn, IR_LOAD_FLD, bo, 0, fld_aux);
            } else if (node->type && node->type->kind == TYPE_ARRAY) {
                /* The result is an array type (e.g. data[]): decay to a pointer to the
                 * first element. No IR_LOAD_IDX here; an array-typed lvalue yields its
                 * address, which the caller then uses directly. */
                irgen_lvalue_addr(g, node);
            } else {
                int elem_sz = type_size(node->type ? node->type : type_int());
                if (elem_sz <= 0) elem_sz = 4;
                irgen_lvalue_addr(g, node);
                ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
                irgen_load_idx(g, node->type, elem_sz, 0);
            }
            break;
        }
        case AST_TERNARY: {
            int false_l = alloc_label(g);
            int end_l = alloc_label(g);
            irgen_expr(g, node->u.ternary.cond);
            irgen_cond_to_bool(g, node->u.ternary.cond);
            ir_emit(g->fn, IR_JZ, false_l, 0, 0);
            irgen_expr(g, node->u.ternary.true_expr);
            ir_emit(g->fn, IR_JMP, end_l, 0, 0);
            ir_emit(g->fn, IR_LABEL, false_l, 0, 0);
            irgen_expr(g, node->u.ternary.false_expr);
            ir_emit(g->fn, IR_LABEL, end_l, 0, 0);
            break;
        }
        default:
            ir_emit(g->fn, IR_PUSH_INT, 0, 0, 0);
            break;
    }
}
