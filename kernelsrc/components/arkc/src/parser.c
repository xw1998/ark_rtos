/* Ark C Compiler - parser
 * Recursive descent syntax parser.
 *
 * Coverage (C99 practical subset):
 *   - Types: void/char/short/int/long/long long/_Bool/float/double/signed/unsigned
 *   - Qualifiers: const/volatile/restrict/inline (const/volatile preserved in Type; restrict/inline ignored)
 *   - Storage classes: static/extern/auto/register
 *   - struct/union/enum with anonymous definitions, tag references
 *   - typedef
 *   - Multi-dimensional arrays, pointers, function pointers (declaration syntax only)
 *   - sizeof(type) / sizeof expr
 *   - (type)expr explicit type cast
 *   - switch/case/default/break
 *   - goto + label
 *   - Declarations at any position in a block + for-init declarations
 *   - String literals, adjacent concatenation (done by lex)
 *
 * Since the parser does not maintain a "global symbol table", typedef is already
 * tokenized as IDENT by the lexer; sym_is_typedef identifies it as a type name.
 */
#include "parser.h"
#include "memory.h"
#include "platform.h"
#include "symtab.h"
#include "sema.h"
#include "config.h"
#include "arkcc_config.h"
#include "diag.h"

static AstNode *parse_top_decl(Parser *p);
static AstNode *parse_block(Parser *p);
static AstNode *parse_statement(Parser *p);
static AstNode *parse_expr(Parser *p);
static AstNode *parse_assign_expr(Parser *p);
static AstNode *parse_cond_expr(Parser *p);
static AstNode *parse_or_expr(Parser *p);
static AstNode *parse_and_expr(Parser *p);
static AstNode *parse_bitor_expr(Parser *p);
static AstNode *parse_bitxor_expr(Parser *p);
static AstNode *parse_bitand_expr(Parser *p);
static AstNode *parse_eq_expr(Parser *p);
static AstNode *parse_rel_expr(Parser *p);
static AstNode *parse_shift_expr(Parser *p);
static AstNode *parse_add_expr(Parser *p);
static AstNode *parse_mul_expr(Parser *p);
static AstNode *parse_cast_expr(Parser *p);
static AstNode *parse_unary_expr(Parser *p);
static AstNode *parse_postfix_expr(Parser *p);
static AstNode *parse_primary_expr(Parser *p);
static Type    *parse_declspec(Parser *p, int *p_storage, int *p_is_typedef);
static Type    *parse_declarator(Parser *p, Type *base, char **out_name);
static AstNode *parse_local_var_decl(Parser *p, Type *base, int storage, int is_typedef);
static AstNode *parse_struct_or_union(Parser *p, int is_union);
static AstNode *parse_enum(Parser *p);
static AstNode *parse_initializer(Parser *p);
static void     parse_attribute(Parser *p, Type *attach_type);
static AstNode *parse_asm_statement(Parser *p);
static AstNode *parse_init_list(Parser *p);

/* Storage class bits */
#define STO_STATIC   1
#define STO_EXTERN   2
#define STO_AUTO     4
#define STO_REGISTER 8

/* Currently parsed function name, for __func__ substitution */
static const char *g_current_func_name = 0;

static int is_decl_start(Parser *p);

static int is_basic_type_token(TokenKind k) {
    switch (k) {
        case TOK_INT: case TOK_CHAR: case TOK_VOID:
        case TOK_SHORT: case TOK_LONG: case TOK_SIGNED: case TOK_UNSIGNED:
        case TOK_FLOAT: case TOK_DOUBLE: case TOK_BOOL:
        case TOK_STRUCT: case TOK_UNION: case TOK_ENUM:
        case TOK_CONST: case TOK_VOLATILE: case TOK_RESTRICT: case TOK_INLINE:
        case TOK_STATIC: case TOK_EXTERN: case TOK_AUTO: case TOK_REGISTER:
        case TOK_TYPEDEF:
        case TOK_PACKED_KW:  /* __packed can start a cast/decl qualifier */
        case TOK_COMPLEX:    /* _Complex */
        /* armcc fixed-width integer types */
        case TOK_INT8: case TOK_INT16: case TOK_INT32: case TOK_INT64:
        /* armcc function/variable modifier keywords (can appear at declaration start) */
        case TOK_THREAD: case TOK_PURE: case TOK_INTERWORK:
        case TOK_VALUE_IN_REGS: case TOK_SMC: case TOK_GLOBAL_REG:
        /* function modifier keywords can also start a declaration (e.g. __irq void f()) */
        case TOK_IRQ: case TOK_WEAK: case TOK_NAKED: case TOK_SOFTFP: case TOK_FORCEINLINE:
        case TOK_DECLSPEC:  /* __declspec(noreturn) void func() */
            return 1;
        default: return 0;
    }
}

/* Check if current token can start a declaration */
static int is_decl_start(Parser *p) {
    Token t = lexer_peek(p->lex);
    if (is_basic_type_token(t.kind)) return 1;
    if (t.kind == TOK_IDENT && sym_is_typedef(t.str)) return 1;
    return 0;
}

/* Syntax errors are reported at the current token: that is the position armcc
 * points its caret at, and the only position whose column we know exactly.
 * `msg` is arkc's own wording unless a measured armcc code exists, so these go
 * through diag_report_msg and carry no fabricated code number. */
static void perror_at(Parser *p, const char *msg) {
    Token t = lexer_peek(p->lex);
    DiagLoc loc;
    loc.file = 0;
    loc.line = t.line;
    loc.col  = t.col;
    diag_report_msg(DIAG_SEV_ERROR, &loc, msg, 0, 0, 0);
    p->error_count++;
}

/* C11 6.7.10: _Static_assert ( constant-expression , string-literal ) ;
 * The condition is folded here because the parser already needs constant
 * folding for array bounds and enum values (eval_const_expr).  A condition
 * that does not fold is reported instead of being ignored: a static
 * assertion whose truth we cannot decide would let a broken build through,
 * which is the one thing a static assertion exists to prevent. */
static void parse_static_assert(Parser *p) {
    Token kw = lexer_peek(p->lex);
    DiagLoc loc;
    AstNode *ce;
    long long v = 0;
    const char *msg = 0;
    int have_msg = 0;
    loc.file = 0;
    loc.line = kw.line;
    loc.col  = kw.col;
    lexer_advance(p->lex);                 /* _Static_assert */
    if (lexer_peek(p->lex).kind != TOK_LPAREN) {
        perror_at(p, "expected '(' after _Static_assert");
        return;
    }
    lexer_advance(p->lex);                 /* '(' */
    ce = parse_cond_expr(p);
    if (lexer_peek(p->lex).kind == TOK_COMMA) {
        lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_STR) {
            msg = lexer_peek(p->lex).str;
            have_msg = 1;
            lexer_advance(p->lex);
        } else {
            perror_at(p, "expected a string literal in _Static_assert");
        }
    }
    if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    else perror_at(p, "expected ')' to close _Static_assert");
    if (lexer_peek(p->lex).kind == TOK_SEMICOLON) lexer_advance(p->lex);
    else perror_at(p, "expected ';' after _Static_assert");

    if (!ce || !eval_const_expr(ce, &v)) {
        diag_report_msg(DIAG_SEV_ERROR, &loc,
                        "_Static_assert condition is not an integer constant expression",
                        0, 0, 0);
        p->error_count++;
        return;
    }
    if (v == 0) {
        if (have_msg)
            diag_report_msg(DIAG_SEV_ERROR, &loc, "static assertion failed: %s",
                            msg ? msg : "", 0, 0);
        else
            diag_report_msg(DIAG_SEV_ERROR, &loc, "static assertion failed",
                            0, 0, 0);
        p->error_count++;
    }
}

AstNode *parse_program(Lexer *lex) {
    Parser p;
    AstNode *prog;

    p.lex = lex;
    p.error_count = 0;

    /* The whole analysis uses the sym table for typedef info (read-only: used for checks in parser) */
    sym_init();

    prog = ast_new(AST_PROGRAM, 1);
    prog->u.program.decls = 0;
    prog->u.program.decl_count = 0;
    prog->u.program.decl_capacity = 0;

    while (lexer_peek(lex).kind != TOK_EOF) {
        /* Enough.  armcc stops *reporting* at 30 errors (diag_at_limit), but
         * this loop used to keep parsing -- and allocating an AST for every
         * statement of a file it no longer understood -- until the 4 MB pool
         * ran out.  The loudest line was then "memory pool exhausted", which
         * hid the actual cause (measured on the board: `cc -S va.c` without
         * -I printed #5 "cannot open ... stdarg.h", then #65 cascades, then
         * died in the allocator).  Stop parsing instead; the errors already
         * reported decide the exit code. */
        if (diag_at_limit()) break;
        /* C11 6.8.2 allows a lone ';' as an empty declaration, and both
         * armcc and GCC accept it at file scope.  Real headers rely on
         * it: a declaration macro that already ends with ';' is often
         * used at a point that supplies another one.  Skip it instead
         * of reporting "expected declaration at top level". */
        if (lexer_peek(lex).kind == TOK_SEMICOLON) { lexer_advance(lex); continue; }
        /* C11 6.7.10: a static assertion is a declaration that produces no
         * object, so it is handled here and contributes nothing to the AST. */
        if (lexer_peek(lex).kind == TOK_STATIC_ASSERT) { parse_static_assert(&p); continue; }
        AstNode *decl = parse_top_decl(&p);
        if (decl) {
            int count = prog->u.program.decl_count;
            if (count >= prog->u.program.decl_capacity) {
                int nc = prog->u.program.decl_capacity ? prog->u.program.decl_capacity * 2 : 16;
                prog->u.program.decls = (AstNode **)mem_realloc(
                    prog->u.program.decls,
                    prog->u.program.decl_capacity * (int)sizeof(AstNode *),
                    nc * (int)sizeof(AstNode *));
                prog->u.program.decl_capacity = nc;
            }
            prog->u.program.decls[count] = decl;
            prog->u.program.decl_count = count + 1;
        } else {
            if (lexer_peek(lex).kind != TOK_EOF) lexer_advance(lex);
        }
    }
    /* A syntax error means we do not know what the program says. Emitting code
     * from a half-understood AST produces a binary that silently computes the
     * wrong thing (or crashes), which is worse than refusing to build. */
    /* No extra line here: armcc's own summary is printed once, by
     * diag_shutdown(). The exit code carries the failure. */
    if (p.error_count > 0) return 0;
    return prog;
}

/* Skip all type qualifiers (const/volatile/restrict).
 * If saw_volatile/saw_const are non-NULL, report what was seen (F2). */
static void skip_qualifiers(Parser *p, int *saw_volatile, int *saw_const) {
    for (;;) {
        TokenKind k = lexer_peek(p->lex).kind;
        if (k == TOK_CONST || k == TOK_VOLATILE || k == TOK_RESTRICT || k == TOK_INLINE) {
            if (k == TOK_VOLATILE && saw_volatile) *saw_volatile = 1;
            if (k == TOK_CONST && saw_const) *saw_const = 1;
            lexer_advance(p->lex);
        } else if (k == TOK_ATTRIBUTE) {
            parse_attribute(p, 0);
        } else break;
    }
}

/* __svc(n) - ARM Compiler 5 SVC declaration; returns SVC number, 0 means not found */
static int parse_svc(Parser *p) {
    int svc_num = 0;
    if (lexer_peek(p->lex).kind != TOK_SVC) return 0;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind != TOK_LPAREN) return 0;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind == TOK_NUM) {
        svc_num = lexer_peek(p->lex).value;
        lexer_advance(p->lex);
    }
    if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    return svc_num;
}

/* __attribute__((weak/naked/interrupt)) identified in parse_attribute and written to these globals,
 * read and reset by parse_top_decl when constructing the function node. */
static int g_attr_is_weak;
static int g_attr_is_naked;
static int g_attr_is_irq;
/* __attribute__((section("..."))) and const qualifiers:
 * written by parse_attribute / parse_declspec, read by parse_top_decl / parse_local_var_decl
 * and reset. const is used for .rodata section selection; section for custom sections. */
static char *g_attr_section;
static int   g_decl_is_const;
static int   g_decl_is_volatile;  /* F2: volatile qualifier seen in declspec */
/* 0.4.0 added attribute / keyword-driven properties.
 *   - used/unused: prevent linker dead code elimination or suppress unused warnings
 *   - alias("target"): function alias (same address)
 *   - aligned(n) / __align(n): force alignment
 *   - softfp / forceinline: ARM Compiler 5 function attributes */
static int   g_attr_is_used;
static int   g_attr_is_unused;
static char *g_attr_alias_target;
static int   g_attr_align_bytes;
/* C11 _Alignas request for the declaration currently being parsed.  Kept
 * separate from g_attr_align_bytes so that __align(n) keeps its exact old
 * behaviour (an explicit __align(1) is still honoured verbatim), while a
 * _Alignas request is clamped up to the natural alignment at the point where
 * the declared type is known (C11 6.7.5p6: _Alignas may not weaken). */
static int   g_alignas_bytes;
static int   g_decl_align_bytes;
/* __packed applies to pointer types (e.g. `__packed int *p`): marked for parse_declarator,
 * sets is_packed=1 when constructing TYPE_PTR; irgen/ARM backend uses byte-wise access accordingly.
 * Reset at the start of each declaration in parse_top_decl / parse_local_var_decl. */
static int   g_decl_packed_ptr;
/* B3: VLA size expression. Written by parse_declarator when parsing non-constant array dimensions,
 * consumed and reset by parse_local_var_decl / parse_top_decl when constructing variable nodes.
 * Only the first VLA dimension is recorded (most VLA usage is single-dim int a[n]). */
static AstNode *g_vla_size_expr;
static int   g_attr_is_softfp;
static int   g_attr_is_forceinline;
/* armcc-style __asm function body that appears inside the declaration specifier
 * (e.g. `static __inline __asm uint32_t __REV16(...)`). Set by parse_declspec
 * when TOK_ASM is followed by a type token, read and reset by parse_top_decl. */
static int   g_decl_is_asm_fn;
/* constructor/destructor/deprecated: function attributes, written by parse_attribute,
 * read and reset by parse_top_decl when constructing the function node. */
static int   g_attr_is_constructor;
static int   g_attr_is_destructor;
static int   g_attr_is_deprecated;
/* __at(addr) / #pragma location=addr: stores the absolute address of the current declaration.
 *   - g_attr_has_at=1 means the current declaration hit at(N), address in g_attr_at_addr.
 *   - g_pragma_location_addr/has set by #pragma location, next var
 *     declaration reads and clears it. */
static unsigned g_attr_at_addr;
static int      g_attr_has_at;

/* Scan and consume __irq/__weak/__naked/__softfp/__forceinline keywords (ARM Compiler 5
 * style), returning a bitmask:
 *   bit0 = is_irq, bit1 = is_weak, bit2 = is_naked,
 *   bit3 = is_softfp, bit4 = is_forceinline
 * These keywords can appear before return type, between type and function name, after parameter list, etc.
 *
 * 0.6.x: also recognize armcc 5 compatible keywords __value_in_regs/__pure/__interwork/
 * __nothrow/__deprecated/__may_alias/__tp_soft/__args/__return etc,
 * passed to parse_top_decl via globals (mostly semantic-only markers).
 *   - g_func_is_value_in_regs / g_func_is_pure / g_func_is_const_attr
 *   - g_func_is_interwork / g_func_is_deprecated_kw
 *   Silently ignored: __nothrow/__may_alias/__tp_soft/__args/__return
 */
static int g_func_is_value_in_regs;
static int g_func_is_pure;
static int g_func_is_const_attr;
static int g_func_is_interwork;
static int g_func_is_deprecated_kw;
/* Accumulator for function modifier bitmask (irq/weak/naked/softfp/forceinline) collected
 * inside parse_declspec when these keywords appear between storage class and base type
 * (e.g. `static __forceinline int func()`). Read and ORed into func_mods by parse_top_decl. */
static int g_func_mods_declspec;

static int parse_func_modifiers(Parser *p) {
    int flags = 0;
    for (;;) {
        TokenKind k = lexer_peek(p->lex).kind;
        if (k == TOK_IRQ)             flags |= 1;
        else if (k == TOK_WEAK)       flags |= 2;
        else if (k == TOK_NAKED)      flags |= 4;
        else if (k == TOK_SOFTFP)     flags |= 8;
        else if (k == TOK_FORCEINLINE) flags |= 16;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        else if (k == TOK_VALUE_IN_REGS) { g_func_is_value_in_regs = 1; }
        else if (k == TOK_PURE)          { g_func_is_pure = 1; }
        else if (k == TOK_INTERWORK)     { g_func_is_interwork = 1; }
        else if (k == TOK_DEPRECATED_KW) { g_func_is_deprecated_kw = 1; }
        /* Silently ignored keywords: __nothrow/__may_alias/__tp_soft/__args/__return */
        else if (k == TOK_NOTHROW || k == TOK_MAY_ALIAS ||
                 k == TOK_TP_SOFT || k == TOK_ARGS || k == TOK_RETURN_KW) {
            /* Silently ignored */
        }
        else break;
#else
        else break;
#endif
        lexer_advance(p->lex);
    }
    return flags;
}

/* __attribute__((a1, a2(arg), ...)) : identify packed/aligned and assign to attach_type */
static void parse_attribute(Parser *p, Type *attach_type) {
    if (lexer_peek(p->lex).kind != TOK_ATTRIBUTE) return;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind != TOK_LPAREN) return;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind != TOK_LPAREN) return;
    lexer_advance(p->lex);
    while (lexer_peek(p->lex).kind != TOK_RPAREN &&
           lexer_peek(p->lex).kind != TOK_EOF) {
        Token t = lexer_peek(p->lex);
        if (t.kind == TOK_IDENT) {
            const char *nm = t.str;
            lexer_advance(p->lex);
            /* Process arguments ( ... ) */
            if (lexer_peek(p->lex).kind == TOK_LPAREN) {
                int paren = 1;
                int first_val = 0;
                int saw_val = 0;
                const char *sec_str = 0;
                lexer_advance(p->lex);
                if (lexer_peek(p->lex).kind == TOK_NUM) {
                    first_val = lexer_peek(p->lex).value;
                    saw_val = 1;
                } else if (lexer_peek(p->lex).kind == TOK_STR) {
                    sec_str = lexer_peek(p->lex).str;
                }
                while (paren > 0 && lexer_peek(p->lex).kind != TOK_EOF) {
                    if (lexer_peek(p->lex).kind == TOK_LPAREN) paren++;
                    else if (lexer_peek(p->lex).kind == TOK_RPAREN) paren--;
                    if (paren > 0) lexer_advance(p->lex);
                }
                if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
                if (saw_val && (ark_streq(nm, "aligned") || ark_streq(nm, "__aligned__"))) {
                    if (attach_type) attach_type->align_override = first_val;
                    g_attr_align_bytes = first_val;
                }
                /* at(N) / __at__(N): absolute address placement (ARM Compiler 5 / IAR style) */
                if (saw_val && (ark_streq(nm, "at") || ark_streq(nm, "__at__"))) {
                    g_attr_at_addr = (unsigned)first_val;
                    g_attr_has_at = 1;
                }
                /* section("name"): capture section name for var node */
                if (sec_str && (ark_streq(nm, "section") || ark_streq(nm, "__section__"))) {
                    g_attr_section = mem_strdup(sec_str);
                }
                /* alias("target"): function alias, record target symbol */
                if (sec_str && (ark_streq(nm, "alias") || ark_streq(nm, "__alias__"))) {
                    g_attr_alias_target = mem_strdup(sec_str);
                }
            } else {
                if (attach_type && (ark_streq(nm, "packed") || ark_streq(nm, "__packed__"))) {
                    attach_type->is_packed = 1;
                }
                if (ark_streq(nm, "aligned") || ark_streq(nm, "__aligned__")) {
                    if (attach_type) attach_type->align_override = 8;
                    g_attr_align_bytes = 8;
                }
                /* Function-level attributes: weak/naked/interrupt(irq). These are not attached to Type,
                 * but passed to parse_top_decl via globals to write to the function node. */
                if (ark_streq(nm, "weak") || ark_streq(nm, "__weak__")) {
                    g_attr_is_weak = 1;
                }
                if (ark_streq(nm, "naked") || ark_streq(nm, "__naked__")) {
                    g_attr_is_naked = 1;
                }
                if (ark_streq(nm, "interrupt") || ark_streq(nm, "__interrupt__") ||
                    ark_streq(nm, "irq") || ark_streq(nm, "__irq__")) {
                    g_attr_is_irq = 1;
                }
                /* used/unused: passed through to C backend, let gcc control dead code elimination/warnings */
                if (ark_streq(nm, "used") || ark_streq(nm, "__used__")) {
                    g_attr_is_used = 1;
                }
                if (ark_streq(nm, "unused") || ark_streq(nm, "__unused__")) {
                    g_attr_is_unused = 1;
                }
                /* armcc style: softfp / forceinline also supported via attribute */
                if (ark_streq(nm, "softfp") || ark_streq(nm, "__softfp__")) {
                    g_attr_is_softfp = 1;
                }
                if (ark_streq(nm, "always_inline") || ark_streq(nm, "__always_inline__") ||
                    ark_streq(nm, "forceinline")) {
                    g_attr_is_forceinline = 1;
                }
                /* constructor/destructor: execute before/after main, C backend passes to gcc,
                 * ARM backend generates .init_array/.fini_array entries.
                 * Optional priority argument constructor(N), already consumed in the ( ... ) branch above. */
                if (ark_streq(nm, "constructor") || ark_streq(nm, "__constructor__")) {
                    g_attr_is_constructor = 1;
                }
                if (ark_streq(nm, "destructor") || ark_streq(nm, "__destructor__")) {
                    g_attr_is_destructor = 1;
                }
                /* deprecated: pass through to gcc to trigger deprecation warning (with optional message string) */
                if (ark_streq(nm, "deprecated") || ark_streq(nm, "__deprecated__")) {
                    g_attr_is_deprecated = 1;
                }
                /* 0.6.x: armcc 5 / GNU compatible attribute supplements.
                 *   - pure/const: pass through to gcc, mark function as having no side effects.
                 *   - may_alias/nothrow/visibility/nonnull/warn_unused_result/
                 *     sentinel/malloc/fastcall/stdcall/cdecl: silently ignored. */
                if (ark_streq(nm, "pure") || ark_streq(nm, "__pure__")) {
                    g_func_is_pure = 1;
                }
                if (ark_streq(nm, "const") || ark_streq(nm, "__const__")) {
                    g_func_is_const_attr = 1;
                }
                /* Silently ignored attributes: may_alias/nothrow/visibility/nonnull/
                 * warn_unused_result/sentinel/malloc/fastcall/stdcall/cdecl */
                if (ark_streq(nm, "may_alias") || ark_streq(nm, "__may_alias__") ||
                    ark_streq(nm, "nothrow") || ark_streq(nm, "__nothrow__") ||
                    ark_streq(nm, "visibility") || ark_streq(nm, "__visibility__") ||
                    ark_streq(nm, "nonnull") || ark_streq(nm, "__nonnull__") ||
                    ark_streq(nm, "warn_unused_result") || ark_streq(nm, "__warn_unused_result__") ||
                    ark_streq(nm, "sentinel") || ark_streq(nm, "__sentinel__") ||
                    ark_streq(nm, "malloc") || ark_streq(nm, "__malloc__") ||
                    ark_streq(nm, "fastcall") || ark_streq(nm, "__fastcall__") ||
                    ark_streq(nm, "stdcall") || ark_streq(nm, "__stdcall__") ||
                    ark_streq(nm, "cdecl") || ark_streq(nm, "__cdecl__")) {
                    /* Silently ignored */
                }
                /* noreturn etc silently ignored */
            }
        } else {
            lexer_advance(p->lex);
        }
        if (lexer_peek(p->lex).kind == TOK_COMMA) lexer_advance(p->lex);
    }
    if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
}

/* Parse initializer: { ... } or plain expression */
static AstNode *parse_init_list(Parser *p) {
    int line = lexer_peek(p->lex).line;
    AstNode *n = ast_new(AST_INIT_LIST, line);
    n->u.init_list.items = 0;
    n->u.init_list.item_count = 0;
    n->u.init_list.item_capacity = 0;
    lexer_advance(p->lex); /* '{' */
    while (lexer_peek(p->lex).kind != TOK_RBRACE &&
           lexer_peek(p->lex).kind != TOK_EOF) {
        AstNode *item = 0;
        AstNode *value;
        int has_field = 0, has_index = 0, idx_val = 0;
        char *field_name = 0;
        if (lexer_peek(p->lex).kind == TOK_DOT) {
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_IDENT) {
                field_name = mem_strdup(lexer_peek(p->lex).str);
                has_field = 1;
                lexer_advance(p->lex);
            }
            /* C99 designated initializer: .field1.field2 = value or
             * .field1[idx].field2 = value; arkcc flattens field chains,
             * designator sets value position; non-designator assigns to next position.
             * Each designator+value pair is a separate INIT_LIST element. */
            if (lexer_peek(p->lex).kind == TOK_DOT ||
                lexer_peek(p->lex).kind == TOK_LBRACKET) {
                /* Handle ".f2.f3 = value" shorthand: equivalent to {.f2.f3 = value} */
                AstNode *sub = ast_new(AST_INIT_LIST, line);
                AstNode *sub_item;
                int has_sub_field = 0, has_sub_index = 0;
                char *sub_field = 0;
                int sub_idx = 0;
                if (lexer_peek(p->lex).kind == TOK_DOT) {
                    lexer_advance(p->lex);
                    if (lexer_peek(p->lex).kind == TOK_IDENT) {
                        sub_field = mem_strdup(lexer_peek(p->lex).str);
                        has_sub_field = 1;
                        lexer_advance(p->lex);
                    }
                } else {
                    /* [index] */
                    lexer_advance(p->lex);
                    if (lexer_peek(p->lex).kind == TOK_NUM) {
                        sub_idx = lexer_peek(p->lex).value;
                        has_sub_index = 1;
                        lexer_advance(p->lex);
                    }
                    lexer_expect(p->lex, TOK_RBRACKET);
                }
                if (lexer_peek(p->lex).kind == TOK_ASSIGN) lexer_advance(p->lex);
                {
                    AstNode *sub_val = parse_initializer(p);
                    sub_item = ast_new(AST_INIT_FIELD, line);
                    sub_item->u.init_field.field = sub_field;
                    sub_item->u.init_field.has_field = has_sub_field;
                    sub_item->u.init_field.index = sub_idx;
                    sub_item->u.init_field.has_index = has_sub_index;
                    sub_item->u.init_field.value = sub_val;
                    sub->u.init_list.items = (AstNode **)mem_pool_alloc(sizeof(AstNode *));
                    sub->u.init_list.items[0] = sub_item;
                    sub->u.init_list.item_count = 1;
                }
                /* Assign item value to init_list; subsequent value continues */
                item = ast_new(AST_INIT_FIELD, line);
                item->u.init_field.field = field_name;
                item->u.init_field.has_field = has_field;
                item->u.init_field.index = 0;
                item->u.init_field.has_index = 0;
                item->u.init_field.value = sub;
                {
                    int c = n->u.init_list.item_count;
                    if (c >= n->u.init_list.item_capacity) {
                        int nc = n->u.init_list.item_capacity ? n->u.init_list.item_capacity * 2 : 8;
                        n->u.init_list.items = (AstNode **)mem_realloc(
                            n->u.init_list.items,
                            n->u.init_list.item_capacity * (int)sizeof(AstNode *),
                            nc * (int)sizeof(AstNode *));
                        n->u.init_list.item_capacity = nc;
                    }
                    n->u.init_list.items[c] = item;
                    n->u.init_list.item_count = c + 1;
                }
                if (lexer_peek(p->lex).kind == TOK_COMMA) lexer_advance(p->lex);
                continue;
            }
            if (lexer_peek(p->lex).kind == TOK_ASSIGN) lexer_advance(p->lex);
        } else if (lexer_peek(p->lex).kind == TOK_LBRACKET) {
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_NUM) {
                idx_val = lexer_peek(p->lex).value;
                has_index = 1;
                lexer_advance(p->lex);
            }
            /* A missing "]" used to be skipped silently, so `int a[2;`
             * compiled with no diagnostic at all. Report it (armcc #17). */
            lexer_expect(p->lex, TOK_RBRACKET);
            if (lexer_peek(p->lex).kind == TOK_ASSIGN) lexer_advance(p->lex);
        }
        value = parse_initializer(p);
        item = ast_new(AST_INIT_FIELD, line);
        item->u.init_field.field = field_name;
        item->u.init_field.has_field = has_field;
        item->u.init_field.index = idx_val;
        item->u.init_field.has_index = has_index;
        item->u.init_field.value = value;
        {
            int c = n->u.init_list.item_count;
            if (c >= n->u.init_list.item_capacity) {
                int nc = n->u.init_list.item_capacity ? n->u.init_list.item_capacity * 2 : 8;
                n->u.init_list.items = (AstNode **)mem_realloc(
                    n->u.init_list.items,
                    n->u.init_list.item_capacity * (int)sizeof(AstNode *),
                    nc * (int)sizeof(AstNode *));
                n->u.init_list.item_capacity = nc;
            }
            n->u.init_list.items[c] = item;
            n->u.init_list.item_count = c + 1;
        }
        if (lexer_peek(p->lex).kind == TOK_COMMA) lexer_advance(p->lex);
    }
    if (lexer_peek(p->lex).kind == TOK_RBRACE) lexer_advance(p->lex);
    return n;
}

static AstNode *parse_initializer(Parser *p) {
    if (lexer_peek(p->lex).kind == TOK_LBRACE) return parse_init_list(p);
    return parse_assign_expr(p);
}

/* Deduce unspecified outermost array dimension:
 *   char s[] = "abc"  -> size = strlen+1
 *   int  a[] = {1,2,3} -> size = 3
 *   int  a[] = {[2]=5} -> size = 3
 * Only handle outermost size==0 arrays; multi-layer unspecified dimensions need more complex deduction, not handled here. */
static void infer_array_size_from_init(Type *t, AstNode *init) {
    if (!t || t->kind != TYPE_ARRAY || t->size != 0) return;
    if (!init) return;
    if (init->kind == AST_STR) {
        const char *s = init->u.name ? init->u.name : "";
        int n = 0;
        while (s[n]) n++;
        t->size = n + 1;  /* Include terminating '\0' */
        return;
    }
    if (init->kind == AST_INIT_LIST) {
        int i;
        int sz = 0;
        int pos = 0;
        for (i = 0; i < init->u.init_list.item_count; i++) {
            AstNode *item = init->u.init_list.items[i];
            if (item && item->kind == AST_INIT_FIELD) {
                if (item->u.init_field.has_index) {
                    if (item->u.init_field.index + 1 > sz) sz = item->u.init_field.index + 1;
                } else {
                    pos++;
                }
            } else {
                /* Non-field item: plain value or nested init list (e.g.
                 * struct array {{...},{...}}). Each item counts as one element. */
                pos++;
            }
        }
        if (pos > sz) sz = pos;
        if (sz > 0) t->size = sz;
        return;
    }
    if (init->kind == AST_COMPOUND_LITERAL &&
        init->u.compound_lit.lit_type &&
        init->u.compound_lit.lit_type->kind == TYPE_ARRAY &&
        init->u.compound_lit.lit_type->size > 0) {
        t->size = init->u.compound_lit.lit_type->size;
        return;
    }
}

/* asm ("template" : outs : ins : clobbers) ;
 * Extended inline asm: capture template and three segments (outputs/inputs/clobbers) as raw strings.
 * Segments separated by top-level ':', whole thing ends with ')'. Each segment rebuilt as string from token spellings. */
static AstNode *parse_asm_statement(Parser *p) {
    int line = lexer_peek(p->lex).line;
    AstNode *n = ast_new(AST_ASM, line);
    int is_vol = 0;
    n->u.asm_stmt.text = 0;
    n->u.asm_stmt.outputs = 0;
    n->u.asm_stmt.inputs = 0;
    n->u.asm_stmt.clobbers = 0;
    n->u.asm_stmt.is_volatile = 0;
    lexer_advance(p->lex); /* asm */
    while (lexer_peek(p->lex).kind == TOK_VOLATILE ||
           lexer_peek(p->lex).kind == TOK_CONST ||
           lexer_peek(p->lex).kind == TOK_INLINE) {
        if (lexer_peek(p->lex).kind == TOK_VOLATILE) is_vol = 1;
        lexer_advance(p->lex);
    }
    n->u.asm_stmt.is_volatile = is_vol;
    if (lexer_peek(p->lex).kind != TOK_LPAREN) return n;
    lexer_advance(p->lex);
    /* Template string (may be multi-segment concatenation, merged by lexer) */
    {
        char *buf = 0;
        int blen = 0;
        while (lexer_peek(p->lex).kind == TOK_STR) {
            const char *s = lexer_peek(p->lex).str;
            int sl = ark_strlen(s);
            int nl = blen + sl;
            char *nb = (char *)mem_pool_alloc(nl + 1);
            int i;
            for (i = 0; i < blen; i++) nb[i] = buf[i];
            for (i = 0; i < sl; i++) nb[blen + i] = s[i];
            nb[nl] = 0;
            buf = nb;
            blen = nl;
            lexer_advance(p->lex);
        }
        n->u.asm_stmt.text = buf ? buf : mem_strdup("");
    }
    /* Extended asm operand segments: split by ':' to capture outputs / inputs / clobbers.
     * Each segment ends at ':' or ')', rebuilt from token spellings. */
    {
        int section = -1;  /* After first ':', becomes 0=outputs, 1=inputs, 2=clobbers */
        while (lexer_peek(p->lex).kind != TOK_EOF &&
               lexer_peek(p->lex).kind != TOK_RPAREN) {
            /* Skip section separator ':': each colon increments section number, tokens go to that section */
            if (lexer_peek(p->lex).kind == TOK_COLON) {
                lexer_advance(p->lex);
                section++;
                if (section > 2) section = 2;
                continue;
            }
            /* Collect current segment until top-level ':' or ')' */
            {
                char *buf = 0;
                int blen = 0;
                int depth = 0;
                while (lexer_peek(p->lex).kind != TOK_EOF) {
                    Token t = lexer_peek(p->lex);
                    TokenKind k = t.kind;
                    const char *sp = 0;
                    char numbuf[16];
                    if (depth == 0 && (k == TOK_COLON || k == TOK_RPAREN)) break;
                    /* Append token spelling */
                    if (blen > 0) {
                        char *nb = (char *)mem_pool_alloc(blen + 2);
                        int i; for (i = 0; i < blen; i++) nb[i] = buf[i];
                        nb[blen] = ' '; nb[blen + 1] = 0;
                        buf = nb; blen = blen + 1;
                    }
                    switch (k) {
                        case TOK_STR: {
                            /* String literal: rebuild with quotes */
                            int sl = ark_strlen(t.str ? t.str : "");
                            char *nb = (char *)mem_pool_alloc(blen + sl + 3);
                            int i, j = 0;
                            for (i = 0; i < blen; i++) nb[j++] = buf[i];
                            nb[j++] = '"';
                            for (i = 0; i < sl; i++) nb[j++] = t.str[i];
                            nb[j++] = '"';
                            nb[j] = 0;
                            buf = nb; blen = j;
                            break;
                        }
                        case TOK_NUM: {
                            /* Integer literal */
                            int v = t.value;
                            int neg = 0; int m = 0; unsigned u;
                            if (v < 0) { neg = 1; u = (unsigned)(-v); } else u = (unsigned)v;
                            if (u == 0) numbuf[m++] = '0';
                            while (u) { numbuf[m++] = (char)('0' + (int)(u % 10)); u /= 10; }
                            {
                                char *nb = (char *)mem_pool_alloc(blen + m + neg + 1);
                                int i, j = 0;
                                for (i = 0; i < blen; i++) nb[j++] = buf[i];
                                if (neg) nb[j++] = '-';
                                for (i = m - 1; i >= 0; i--) nb[j++] = numbuf[i];
                                nb[j] = 0;
                                buf = nb; blen = j;
                            }
                            break;
                        }
                        case TOK_IDENT: sp = t.str ? t.str : ""; break;
                        case TOK_LPAREN: depth++; sp = "("; break;
                        case TOK_RPAREN: depth--; sp = ")"; break;
                        case TOK_COMMA:  sp = ","; break;
                        case TOK_MINUS:  sp = "-"; break;
                        case TOK_PLUS:   sp = "+"; break;
                        case TOK_MUL:    sp = "*"; break;
                        case TOK_BAND:   sp = "&"; break;
                        case TOK_ASSIGN: sp = "="; break;
                        case TOK_DOT:    sp = "."; break;
                        case TOK_ARROW:  sp = "->"; break;
                        case TOK_LBRACKET: sp = "["; break;
                        case TOK_RBRACKET: sp = "]"; break;
                        case TOK_LBRACE: sp = "{"; break;
                        case TOK_RBRACE: sp = "}"; break;
                        default:         sp = ""; break;
                    }
                    if (sp) {
                        int sl = ark_strlen(sp);
                        char *nb = (char *)mem_pool_alloc(blen + sl + 1);
                        int i, j = 0;
                        for (i = 0; i < blen; i++) nb[j++] = buf[i];
                        for (i = 0; i < sl; i++) nb[j++] = sp[i];
                        nb[j] = 0;
                        buf = nb; blen = j;
                    }
                    lexer_advance(p->lex);
                }
                /* Write to corresponding segment (section determined by preceding ':'; <0 means no colon after template, ignore) */
                {
                    char *sec = buf ? buf : mem_strdup("");
                    if (section < 0) {
                        /* Non-colon token after template: ignore to avoid mis-write */
                    } else if (section == 0)      n->u.asm_stmt.outputs = sec;
                    else if (section == 1)       n->u.asm_stmt.inputs = sec;
                    else                         n->u.asm_stmt.clobbers = sec;
                }
            }
        }
        if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    }
    if (lexer_peek(p->lex).kind == TOK_SEMICOLON) lexer_advance(p->lex);
    return n;
}

/* Parse declspec: return base type; also return storage class and is_typedef via pointers */
/* C11 6.7.5: _Alignas ( type-name ) or _Alignas ( constant-expression ).
 * The result is the requested alignment in bytes (0 = none/invalid).  It is
 * parsed here, in the declaration-specifier position, and merged into the
 * declaration by the caller. */
static int parse_alignas(Parser *p) {
    Token kw = lexer_peek(p->lex);
    long long v = 0;
    int al = 0;
    DiagLoc loc;
    loc.file = 0;
    loc.line = kw.line;
    loc.col  = kw.col;
    lexer_advance(p->lex);              /* _Alignas */
    if (lexer_peek(p->lex).kind != TOK_LPAREN) {
        perror_at(p, "expected '(' after _Alignas");
        return 0;
    }
    lexer_advance(p->lex);              /* '(' */
    if (is_basic_type_token(lexer_peek(p->lex).kind) ||
        (lexer_peek(p->lex).kind == TOK_IDENT && sym_is_typedef(lexer_peek(p->lex).str))) {
        int sto = 0, td = 0;
        char *nm = 0;
        Type *base = parse_declspec(p, &sto, &td);
        Type *t = base ? parse_declarator(p, base, &nm) : 0;
        (void)nm;
        al = type_align(t);
        if (al <= 0) al = 1;
    } else {
        AstNode *ce = parse_cond_expr(p);
        if (!ce || !eval_const_expr(ce, &v)) {
            diag_report_msg(DIAG_SEV_ERROR, &loc,
                            "_Alignas requires an integer constant expression or a type name",
                            0, 0, 0);
            p->error_count++;
            al = 0;
        } else if (v <= 0 || (v & (v - 1)) != 0) {
            diag_report_msg(DIAG_SEV_ERROR, &loc,
                            "_Alignas requires a power-of-two alignment",
                            0, 0, 0);
            p->error_count++;
            al = 0;
        } else {
            al = (int)v;
        }
    }
    lexer_expect(p->lex, TOK_RPAREN);
    return al;
}

static Type *parse_declspec(Parser *p, int *p_storage, int *p_is_typedef) {
    int sto = 0;
    int is_td = 0;
    int sign = 0;     /* 0=default, 1=signed, 2=unsigned */
    int long_count = 0;
    Type *base = 0;
    int saw_kw = 0;
    int packed_pending = 0;  /* __packed standalone form: applies to next struct/union or pointer */
    int complex_pending = 0; /* B4: _Complex modifier */
    Token tok;
    /* Reset packed pointer flag at declspec start to avoid cross-declaration/cast leakage.
     * If this declspec sees __packed, the loop will set it back to 1. */
    g_decl_packed_ptr = 0;
    /* Reset the declspec-local function modifier accumulator; parse_top_decl reads it back. */
    g_func_mods_declspec = 0;
    /* Reset asm-fn marker; will be set if TOK_ASM appears between storage class
     * keywords and the base type (e.g. `static __inline __asm uint32_t fn()`). */
    g_decl_is_asm_fn = 0;
    /* Parse keywords until no more match */
    for (;;) {
        tok = lexer_peek(p->lex);
        switch (tok.kind) {
            case TOK_ATTRIBUTE:
                /* declspec-level attribute: discarded for now */
                parse_attribute(p, 0);
                saw_kw = 1;
                continue;
            case TOK_CONST: case TOK_VOLATILE: case TOK_RESTRICT: case TOK_INLINE:
                if (tok.kind == TOK_CONST) g_decl_is_const = 1;
                if (tok.kind == TOK_VOLATILE) g_decl_is_volatile = 1;
                lexer_advance(p->lex);
                saw_kw = 1;
                continue;
            case TOK_ASM:
                /* armcc-style __asm function body inside declspec:
                 *   static __inline __asm uint32_t __REV16(uint32_t value) { ... }
                 * Distinguish from __asm("...") statement or __asm { ... } block
                 * by looking ahead: if next token can start a type (basic type token
                 * or typedef identifier), treat __asm as a function qualifier and
                 * continue parsing the type. Otherwise exit (handled by caller). */
                {
                    Token t2 = lexer_peek2(p->lex);
                    if (is_basic_type_token(t2.kind) ||
                        (t2.kind == TOK_IDENT && sym_is_typedef(t2.str))) {
                        g_decl_is_asm_fn = 1;
                        lexer_advance(p->lex);  /* Consume __asm */
                        saw_kw = 1;
                        continue;
                    }
                    goto done;
                }
            case TOK_ALIGNAS: {
                /* C11 _Alignas in the declaration-specifier position. */
                int al = parse_alignas(p);
                if (al > 0) g_alignas_bytes = al;
                saw_kw = 1;
                continue;
            }
            case TOK_PACKED_KW:
                /* ARM Compiler 5 style: __packed appears in declspec.
                 * For struct/union types, equivalent to __attribute__((packed));
                 * For pointer types (e.g. __packed int *p), mark as packed pointer,
                 * irgen/ARM backend uses byte-wise access accordingly. */
                packed_pending = 1;
                g_decl_packed_ptr = 1;
                lexer_advance(p->lex);
                saw_kw = 1;
                continue;
            case TOK_STATIC:   sto |= STO_STATIC;   lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_EXTERN:   sto |= STO_EXTERN;   lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_AUTO:     sto |= STO_AUTO;     lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_REGISTER: sto |= STO_REGISTER; lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_TYPEDEF:  is_td = 1; lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_SIGNED:   sign = 1; lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_UNSIGNED: sign = 2; lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_LONG:     long_count++; lexer_advance(p->lex); saw_kw = 1; continue;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            case TOK_TYPEDEF_KW:
                /* __typedef: equivalent to typedef */
                is_td = 1; lexer_advance(p->lex); saw_kw = 1; continue;
            case TOK_THREAD:
                /* __thread: thread-local storage. Sets storage class flag bit 0x10000,
                 * read by parse_top_decl/parse_local_var_decl and written to var.is_tls.
                 * Does not affect type deduction. */
                sto |= 0x10000;
                lexer_advance(p->lex);
                saw_kw = 1;
                continue;
            case TOK_INT8:    lexer_advance(p->lex); if (!base) base = type_char();  saw_kw = 1; continue;
            case TOK_INT16:   lexer_advance(p->lex); if (!base) base = type_short(); saw_kw = 1; continue;
            case TOK_INT32:   lexer_advance(p->lex); if (!base) base = type_int();   saw_kw = 1; continue;
            case TOK_INT64:   lexer_advance(p->lex); if (!base) base = type_llong(); saw_kw = 1; continue;
            /* __declspec(attr): MSVC-style attribute. Skip the parenthesized attribute list.
             * Common forms: __declspec(noreturn), __declspec(dllimport), __declspec(thread),
             * __declspec(align(n)). Currently all are silently ignored (matching __attribute__
             * behavior for unsupported attributes). */
            case TOK_DECLSPEC:
                lexer_advance(p->lex);  /* consume __declspec */
                if (lexer_peek(p->lex).kind == TOK_LPAREN) {
                    int paren = 1;
                    lexer_advance(p->lex);  /* consume ( */
                    while (paren > 0 && lexer_peek(p->lex).kind != TOK_EOF) {
                        if (lexer_peek(p->lex).kind == TOK_LPAREN) paren++;
                        else if (lexer_peek(p->lex).kind == TOK_RPAREN) paren--;
                        lexer_advance(p->lex);
                    }
                }
                saw_kw = 1;
                continue;
            /* ARMCC5 function modifier keywords can appear inside declaration specifiers,
             * e.g. `static __forceinline int func()` or `__pure int func()`.
             * Delegate to parse_func_modifiers which consumes all consecutive modifier
             * keywords and sets the corresponding globals / returns a bitmask.
             * The bitmask is accumulated in g_func_mods_declspec and later ORed into
             * func_mods by parse_top_decl. */
            case TOK_PURE: case TOK_INTERWORK: case TOK_VALUE_IN_REGS: case TOK_DEPRECATED_KW:
            case TOK_NOTHROW: case TOK_MAY_ALIAS: case TOK_TP_SOFT: case TOK_ARGS: case TOK_RETURN_KW:
            case TOK_IRQ: case TOK_WEAK: case TOK_NAKED: case TOK_SOFTFP: case TOK_FORCEINLINE:
                g_func_mods_declspec |= parse_func_modifiers(p);
                saw_kw = 1;
                continue;
#endif
            case TOK_SHORT:    /* Set short flag */ {
                lexer_advance(p->lex);
                if (!base) base = type_short();
                saw_kw = 1; continue;
            }
            case TOK_INT:
                lexer_advance(p->lex);
                if (!base) base = type_int();
                saw_kw = 1; continue;
            case TOK_CHAR:
                lexer_advance(p->lex);
                if (!base) base = type_char();
                saw_kw = 1; continue;
            case TOK_VOID:
                lexer_advance(p->lex);
                if (!base) base = type_void();
                saw_kw = 1; continue;
            case TOK_BOOL:
                lexer_advance(p->lex);
                if (!base) base = type_bool();
                saw_kw = 1; continue;
            case TOK_FLOAT:
                lexer_advance(p->lex);
                if (!base) base = type_float();
                saw_kw = 1; continue;
            case TOK_DOUBLE:
                lexer_advance(p->lex);
                if (!base) base = type_double();
                saw_kw = 1; continue;
            case TOK_COMPLEX:
#if ARKCC_FEATURE_COMPLEX
                /* B4: _Complex modifier; applies to subsequent float/double (default double).
                 * Compile-time + run-time dual guard. */
                if (g_config.enable_complex) complex_pending = 1;
#endif
                lexer_advance(p->lex);
                saw_kw = 1; continue;
            case TOK_STRUCT:
            case TOK_UNION: {
                int is_union = (tok.kind == TOK_UNION);
                AstNode *def;
                def = parse_struct_or_union(p, is_union);
                if (def) base = def->type;
                /* __packed prefix: equivalent to __attribute__((packed)) */
                if (packed_pending && base) {
                    base->is_packed = 1;
                    packed_pending = 0;
                }
                saw_kw = 1;
                continue;
            }
            case TOK_ENUM: {
                AstNode *en = parse_enum(p);
                if (en) base = en->type;
                else base = type_int();
                saw_kw = 1;
                continue;
            }
            case TOK_IDENT: {
                /* A type has already been parsed, so an identifier that
                 * happens to name a typedef is the *declarator* name, not a
                 * second type specifier (C11 6.7p2 allows at most one, and
                 * the standard explicitly permits redefining a typedef to
                 * the same type).  Without this, `typedef int F(int);
                 * typedef int F(int);` parsed the second `F` as the base
                 * type and then had no name left -- "expected identifier in
                 * declaration". */
                if (base) goto done;
                if (sym_is_typedef(tok.str)) {
                    Symbol *s = sym_find_typedef(tok.str);
                    lexer_advance(p->lex);
                    if (s && s->type) base = s->type;
                    else base = type_int();
                    saw_kw = 1;
                    continue;
                }
                /* Otherwise not part of declaration, exit */
                goto done;
            }
            default:
                goto done;
        }
    }
done:
    if (!base) {
        /* Check integer combination: verify long_count and sign consistency,
         * determine long_count and sign combination,
         * "unsigned long long" with sign==2 returns type_uint(), etc. */
        if (long_count >= 2) base = type_llong();
        else if (long_count == 1) base = type_long();
        else if (sign == 2) base = type_uint();
        else if (saw_kw) base = type_int();
        else return 0;
        if (sign == 2) {
            /* Switch to unsigned variant (integer types only) */
            switch (base->kind) {
                case TYPE_CHAR:  base = type_uchar();  break;
                case TYPE_SHORT: base = type_ushort(); break;
                case TYPE_INT:   base = type_uint();   break;
                case TYPE_LONG:  base = type_ulong();  break;
                case TYPE_LLONG: base = type_ullong(); break;
                default: break;
            }
        }
    } else {
        if (long_count >= 2 && base->kind == TYPE_INT) base = type_llong();
        else if (long_count == 1 && base->kind == TYPE_INT) base = type_long();
        if (sign == 2) {
            /* Switch to unsigned variant (integer types only) */
            switch (base->kind) {
                case TYPE_CHAR:  base = type_uchar();  break;
                case TYPE_SHORT: base = type_ushort(); break;
                case TYPE_INT:   base = type_uint();   break;
                case TYPE_LONG:  base = type_ulong();  break;
                case TYPE_LLONG: base = type_ullong(); break;
                default: break;
            }
        }
    }
    if (p_storage) *p_storage = sto;
    if (p_is_typedef) *p_is_typedef = is_td;
    (void)packed_pending; /* __packed silently ignored for non-struct types */
#if ARKCC_FEATURE_COMPLEX
    /* B4: _Complex modifier. _Complex double -> type_complex(double);
     * _Complex float -> type_complex(float);
     * _Complex (without float/double) defaults to _Complex double. */
    if (complex_pending && g_config.enable_complex) {
        if (!base || (base->kind != TYPE_FLOAT && base->kind != TYPE_DOUBLE)) {
            base = type_double();
        }
        base = type_complex(base);
    }
#endif
    /* F2: Apply const/volatile qualifiers to the base type.
     * Creates a qualified copy to avoid modifying singleton basic types.
     * The globals remain set so callers (parse_top_decl/parse_local_var_decl)
     * can still read them for section selection etc. */
    if (g_decl_is_volatile || g_decl_is_const) {
        base = type_qualified(base, g_decl_is_volatile, g_decl_is_const);
    }
    return base;
}

/* struct/union: parse definition or reference, register type in sym table tag namespace */
static AstNode *parse_struct_or_union(Parser *p, int is_union) {
    char *tag = 0;
    Type *t = 0;
    Type *attr_tmp = 0;   /* Temp type to carry leading __attribute__ */
    int   has_attr = 0;
    Token tk;
    int line = lexer_peek(p->lex).line;
    lexer_advance(p->lex); /* struct/union */
    /* Leading __attribute__ can appear before tag: struct __attribute__((packed)) Tag {...}
     * Use temp type to carry packed/aligned, then merge into actual type. */
    while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) {
        if (!attr_tmp) attr_tmp = type_struct(0, is_union);
        parse_attribute(p, attr_tmp);
        has_attr = 1;
    }
    tk = lexer_peek(p->lex);
    if (tk.kind == TOK_IDENT) {
        tag = mem_strdup(tk.str);
        lexer_advance(p->lex);
        {
            /* Find existing definition in tag namespace (struct/union tag,
             * NOT typedef -- a typedef with the same name may shadow it). */
            Symbol *s = sym_find_tag(tag);
            if (s) t = s->type;
        }
    }
    if (lexer_peek(p->lex).kind == TOK_LBRACE) {
        if (!t) {
            t = type_struct(tag, is_union);
            if (tag) {
                Symbol *sm = sym_add(is_union ? SYM_UNION_TAG : SYM_STRUCT_TAG, tag, t, 0);
                (void)sm;
            }
        }
        /* If t already exists (forward declaration from typedef), reuse it
         * directly so the typedef still points to the now-complete type. */
        /* Merge leading attribute: must set before parsing fields, since type_struct_add_field
         * adjusts alignment based on is_packed/align_override. */
        if (has_attr && attr_tmp) {
            if (attr_tmp->is_packed) t->is_packed = 1;
            if (attr_tmp->align_override) t->align_override = attr_tmp->align_override;
        }
        lexer_advance(p->lex);
        while (lexer_peek(p->lex).kind != TOK_RBRACE &&
               lexer_peek(p->lex).kind != TOK_EOF) {
            int sto = 0; int td = 0;
            Type *ft_base;
            /* C11 6.7.10 allows a static assertion among the members. */
            if (lexer_peek(p->lex).kind == TOK_STATIC_ASSERT) {
                parse_static_assert(p);
                continue;
            }
            g_alignas_bytes = 0;
            ft_base = parse_declspec(p, &sto, &td);
            if (!ft_base) { perror_at(p, "expected field type"); lexer_advance(p->lex); continue; }
            for (;;) {
                char *fname = 0;
                Type *ft = parse_declarator(p, ft_base, &fname);
                int bit_w = -1;
                /* C11 6.7.5: _Alignas on a member raises the field type's
                 * alignment, and type_struct_add_field lays the struct out
                 * with type_align(field type), so this is the whole change.
                 * A request weaker than the natural alignment is ignored
                 * (6.7.5p6), never applied as a reduction. */
                if (g_alignas_bytes > 0 && ft) {
                    int nat = type_align(ft);
                    if (g_alignas_bytes > nat) ft->align_override = g_alignas_bytes;
                }
                if (lexer_peek(p->lex).kind == TOK_COLON) {
                    /* Bit-field: may have no field name */
                    lexer_advance(p->lex);
                    if (lexer_peek(p->lex).kind == TOK_NUM) {
                        bit_w = lexer_peek(p->lex).value;
                        lexer_advance(p->lex);
                    }
                }
                /* Field trailing may have __attribute__((packed)) etc */
                while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) parse_attribute(p, t);
                if (bit_w >= 0) {
                    /* C99 6.7.2.1p4: the width may not exceed the number of
                     * bits in the declared type. Additionally every backend
                     * extracts a bit-field from a 32-bit word, so widths above
                     * 32 (legal only for long long containers) would silently
                     * read back truncated. Report instead of clamping: a wrong
                     * value with no diagnostic is worse than a hard error. */
                    int ft_bits = (ft ? type_size(ft) : 4) * 8;
                    if (bit_w > ft_bits || bit_w > 32) {
                        /* arkc-specific condition (no measured armcc code), so
                         * the wording is ours; the position is the token the
                         * parser is looking at. */
                        char nb[12], fb[12];
                        DiagLoc loc;
                        Token bt = lexer_peek(p->lex);
                        loc.file = 0; loc.line = bt.line; loc.col = bt.col;
                        if (bit_w > ft_bits) {
                            diag_report_msg(DIAG_SEV_ERROR, &loc,
                                            "bit-field width %d exceeds its type (%d bits)",
                                            diag_num(nb, bit_w),
                                            diag_num(fb, ft_bits), 0);
                            bit_w = ft_bits;
                        } else {
                            diag_report_msg(DIAG_SEV_ERROR, &loc,
                                            "bit-field width %d exceeds 32; only 32-bit containers are supported",
                                            diag_num(nb, bit_w), 0, 0);
                            bit_w = 32;
                        }
                        p->error_count++;
                    }
                    type_struct_add_bitfield(t, fname ? fname : "", ft, bit_w);
                } else if (fname) {
                    type_struct_add_field(t, fname, ft);
                } else if (ft && (ft->kind == TYPE_STRUCT || ft->kind == TYPE_UNION)) {
                    /* Anonymous struct/union member (common in CMSIS register layouts): flatten
                     * inner fields to parent at same offset base, names accessible from outer scope. */
                    type_struct_add_anonymous(t, ft);
                }
                if (lexer_peek(p->lex).kind == TOK_COMMA) { lexer_advance(p->lex); continue; }
                break;
            }
            lexer_expect(p->lex, TOK_SEMICOLON);
        }
        lexer_expect(p->lex, TOK_RBRACE);
        /* Merge trailing __attribute__((packed/aligned)) */
        while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) parse_attribute(p, t);
    } else if (!t) {
        /* forward declaration */
        t = type_struct(tag, is_union);
        if (has_attr && attr_tmp) {
            if (attr_tmp->is_packed) t->is_packed = 1;
            if (attr_tmp->align_override) t->align_override = attr_tmp->align_override;
        }
        if (tag) sym_add(is_union ? SYM_UNION_TAG : SYM_STRUCT_TAG, tag, t, 0);
    } else if (has_attr && attr_tmp) {
        /* Apply leading attribute when referencing existing type */
        if (attr_tmp->is_packed) t->is_packed = 1;
        if (attr_tmp->align_override) t->align_override = attr_tmp->align_override;
    }
    {
        AstNode *n = ast_new(AST_VAR_DECL, line);
        n->type = t;
        n->u.var.var_type = t;
        return n;
    }
}

static AstNode *parse_enum(Parser *p) {
    char *tag = 0;
    Type *t;
    int next_val = 0;
    int line = lexer_peek(p->lex).line;
    lexer_advance(p->lex); /* enum */
    if (lexer_peek(p->lex).kind == TOK_IDENT) {
        tag = mem_strdup(lexer_peek(p->lex).str);
        lexer_advance(p->lex);
    }
    t = type_enum(tag);
    if (tag) sym_add(SYM_ENUM_TAG, tag, t, 0);
    if (lexer_peek(p->lex).kind == TOK_LBRACE) {
        lexer_advance(p->lex);
        while (lexer_peek(p->lex).kind != TOK_RBRACE &&
               lexer_peek(p->lex).kind != TOK_EOF) {
            Token nt = lexer_peek(p->lex);
            if (nt.kind != TOK_IDENT) { perror_at(p, "expected enum name"); lexer_advance(p->lex); continue; }
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_ASSIGN) {
                /* C99 6.7.2.2: enum values are integer constant expressions.
                 * Support full constant expressions including !ID (e.g. SET = !RESET),
                 * bitwise/arithmetic ops, etc. parse_cond_expr + eval_const_expr
                 * handles these; previous enum consts are already in the symbol table. */
                lexer_advance(p->lex);
                {
                    AstNode *ce = parse_cond_expr(p);
                    long long cv = 0;
                    if (ce && eval_const_expr(ce, &cv)) {
                        next_val = (int)cv;
                    } else {
                        perror_at(p, "expected enum value (constant expression)");
                    }
                }
            }
            {
                Symbol *s = sym_add(SYM_ENUM_CONST, nt.str, type_int(), 0);
                s->enum_value = next_val;
            }
            next_val++;
            if (lexer_peek(p->lex).kind == TOK_COMMA) lexer_advance(p->lex);
        }
        lexer_expect(p->lex, TOK_RBRACE);
    }
    {
        AstNode *n = ast_new(AST_VAR_DECL, line);
        n->type = t;
        n->u.var.var_type = t;
        return n;
    }
}

/* True when a constant expression mentions a float/double operand.  Used to
 * reject array dimensions such as `2.5` or `(int)(1.0/0.0)` instead of
 * silently substituting the placeholder dimension 1. */
static int expr_mentions_float(AstNode *n) {
    if (!n) return 0;
    switch (n->kind) {
    case AST_NUM:
        return n->type && (n->type->kind == TYPE_FLOAT || n->type->kind == TYPE_DOUBLE);
    case AST_CAST:
    case AST_UNARY:
        return expr_mentions_float(n->u.unary.operand);
    case AST_BINARY:
        return expr_mentions_float(n->u.binary.left) ||
               expr_mentions_float(n->u.binary.right);
    case AST_TERNARY:
        return expr_mentions_float(n->u.ternary.cond) ||
               expr_mentions_float(n->u.ternary.true_expr) ||
               expr_mentions_float(n->u.ternary.false_expr);
    default:
        return 0;
    }
}

/* declarator: handle * modifier, array dimensions, function params; return final type, write name to out_name.
 *
 * Parenthesized declarators are supported for the one shape that matters in
 * practice: `( * name ) ( params )`, i.e. a pointer to function.  Its parameter
 * list is parsed for real and kept on the pointer type (Type::params), because
 * a call through the pointer needs the parameter widths to materialise an
 * argument that is wider than one IR word.  Nested/returning-function-pointer
 * shapes such as `int (*pick(int))(int)` are still not parsed.
 */
static void parse_param_list(Parser *p, AstNode ***out_params, int *out_count,
                             int *out_variadic, int line);

static Type *parse_declarator(Parser *p, Type *base, char **out_name) {
    Type *t = base;
    p->name_line = 0;
    p->name_col  = 0;
    /* Pointer */
    while (lexer_peek(p->lex).kind == TOK_MUL) {
        int ptr_vol = 0, ptr_const = 0;
        lexer_advance(p->lex);
        skip_qualifiers(p, &ptr_vol, &ptr_const);
        t = type_pointer(t);
        /* F2: pointer-level qualifiers (e.g., `int *volatile p`) */
        if (ptr_vol) t->is_volatile = 1;
        if (ptr_const) t->is_const = 1;
        /* __packed pointer: mark this pointer type as packed, irgen/ARM backend accordingly
         * uses byte-wise access instead of LDR/STR to avoid unaligned access exceptions.
         * Flag is set at each pointer level (supports __packed int **p etc). */
        if (g_decl_packed_ptr) t->is_packed = 1;
    }
    /* Optional parenthesized declarator (function pointer): ("(" "*" name ")" "(" params ")") */
    if (lexer_peek(p->lex).kind == TOK_LPAREN &&
        lexer_peek2(p->lex).kind == TOK_MUL) {
        /* Skip (*name) */
        lexer_advance(p->lex); /* '(' */
        lexer_advance(p->lex); /* '*' */
        while (lexer_peek(p->lex).kind == TOK_MUL) lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_IDENT) {
            if (out_name) *out_name = mem_strdup(lexer_peek(p->lex).str);
            p->name_line = lexer_peek(p->lex).line;
            p->name_col  = lexer_peek(p->lex).col;
            lexer_advance(p->lex);
        }
        lexer_expect(p->lex, TOK_RPAREN);
        /* Parameter list.  It used to be skipped as balanced tokens, which threw
         * the prototypes away: every function-pointer call then had unknown
         * parameter widths, so a 16-byte _Complex (or a >8-byte struct)
         * argument could not be materialised and was refused instead.  The type
         * itself stays "pointer whose base is the return type" -- that is what
         * `*fp` and the call lowering expect -- so the parameter types are kept
         * on the pointer type, where irgen looks for them. */
        t = type_pointer(t);
        t->is_func_ptr = 1;
        if (lexer_peek(p->lex).kind == TOK_LPAREN) {
            AstNode **fparams = 0;
            int fpc = 0, fva = 0, fi;
            lexer_advance(p->lex);
            parse_param_list(p, &fparams, &fpc, &fva, p->name_line);
            lexer_expect(p->lex, TOK_RPAREN);
            t->is_variadic = fva;
            if (fpc > 0 && fparams) {
                Type **ftypes = (Type **)mem_pool_alloc(fpc * (int)sizeof(Type *));
                for (fi = 0; fi < fpc; fi++)
                    ftypes[fi] = !fparams[fi] ? 0
                                 : (fparams[fi]->type ? fparams[fi]->type
                                                       : fparams[fi]->u.var.var_type);
                t->params = ftypes;
                t->param_count = fpc;
            }
        }
        /* Treat as function pointer. The pointee is a function, not a data
         * object: mark it so `*fp` dereferences to the designator itself
         * instead of loading the function's first bytes as a value. */
        return t;
    }
    if (lexer_peek(p->lex).kind == TOK_IDENT) {
        if (out_name) *out_name = mem_strdup(lexer_peek(p->lex).str);
        p->name_line = lexer_peek(p->lex).line;
        p->name_col  = lexer_peek(p->lex).col;
        lexer_advance(p->lex);
    }
    /* armcc/GCC register binding: `register T name __asm("reg")`.
     * Skip the __asm("regname") clause - we don't honor register binding
     * (would require backend support for specific register allocation). */
    if (lexer_peek(p->lex).kind == TOK_ASM) {
        lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_LPAREN) {
            lexer_advance(p->lex);
            while (lexer_peek(p->lex).kind != TOK_RPAREN &&
                   lexer_peek(p->lex).kind != TOK_EOF)
                lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
        }
    }
    /* Array dimensions (multi-dimensional supported) */
    {
        /* Collect dimensions, then construct array(array(...)) */
        int dims[ARK_MAX_ARRAY_DIMS];
        int dim_count = 0;
        int vla_dim = -1;  /* B3: non-constant dimension index; -1 means no VLA */
        while (lexer_peek(p->lex).kind == TOK_LBRACKET) {
            int v = 0;
            int is_param_array = 0;  /* [static expr] or [*] in parameter context */
            lexer_advance(p->lex);  /* Consume '[' */
            /* C99 6.7.5.2/6.7.5.3: array parameter declarators support [static expr]
             * and [*] VLA notation. We need to consume these forms.
             * [*] means VLA of unspecified size; static and star markers are skipped. */
            if (lexer_peek(p->lex).kind == TOK_STATIC) {
                lexer_advance(p->lex);
                is_param_array = 1;  /* Mark as parameter array (decays to pointer, not VLA) */
            }
            if (lexer_peek(p->lex).kind == TOK_MUL) {
                /* VLA [*] form: treat as unsized */
                lexer_advance(p->lex);
                v = 0;
            }
            else if (lexer_peek(p->lex).kind == TOK_RBRACKET) {
                v = 0;  /* No dimension: to be deduced (e.g. char s[] = "...") */
            } else {
                /* C99: array dimensions must be integer constant expressions. After parsing
                 * conditional expression, use eval_const_expr for compile-time evaluation;
                 * fallback to 1 on failure to avoid empty arrays. Supports 1+2, SIZE+2 (macro expansion), sizeof(int)*2 etc. */
                AstNode *dim_expr = parse_cond_expr(p);
                long long folded = 0;
                if (dim_expr && eval_const_expr(dim_expr, &folded) && folded > 0) {
                    v = folded;
                } else {
                    /* `int a[2.5]` and `int a[(int)(1.0/0.0)]` are not
                     * integer constant expressions (the first has a float
                     * type, the second is UB and is deliberately not
                     * folded).  Both used to fall through to the placeholder
                     * dimension 1 below, i.e. a one-element array with no
                     * diagnostic at all.  A dimension that merely is not
                     * constant (a VLA, `int a[n]`) does not mention a float,
                     * so the VLA path below is untouched. */
                    if (dim_expr && expr_mentions_float(dim_expr))
                        perror_at(p, "array dimension is not an integer constant expression");
#if ARKCC_FEATURE_VLA
                    /* B3: non-constant dimension -> VLA. Only record first VLA dimension expression,
                     * consumed by parse_local_var_decl / parse_top_decl.
                     * Compile-time + run-time dual guard.
                     * For [static expr] parameter form, size is known but decays to pointer.
                     * Actual VLA uses alloca. */
                    if (g_config.enable_vla && vla_dim < 0 && !g_vla_size_expr && !is_param_array) {
                        vla_dim = dim_count;
                        g_vla_size_expr = dim_expr;
                        v = 0;  /* Placeholder; actual type uses type_pointer */
                    } else {
                        v = 1;  /* Non-constant or eval failed: placeholder dimension */
                    }
#else
                    v = 1;  /* Non-constant or eval failed: placeholder dimension */
#endif
                }
            }
            /* A missing "]" used to be skipped silently, so `int a[2;`
             * compiled with no diagnostic at all. Report it (armcc #17). */
            lexer_expect(p->lex, TOK_RBRACKET);
            if (dim_count < ARK_MAX_ARRAY_DIMS) {
                dims[dim_count++] = v;
            } else {
                /* Dropping a dimension silently changes the array's rank, i.e.
                 * its element type and every index computation after it. */
                perror_at(p, "too many array dimensions");
            }
        }
        if (dim_count > 0) {
            int i;
            /* Construct nested arrays from right to left; keep size==0 for to-be-deduced (e.g. char s[] = "...").
             * Flexible array members also use size==0 form, handled by type_struct_add_field. */
            for (i = dim_count - 1; i >= 0; i--) {
                if (i == vla_dim) {
                    /* B3: VLA dimension replaced by pointer; mark is_vla.
                     * Subsequent irgen emits IR_ALLOCA for dynamic allocation at variable declaration. */
                    t = type_pointer(t);
                    t->is_vla = 1;
                } else {
                    t = type_array(t, dims[i]);
                }
            }
        }
    }
    return t;
}

/* __align(n) - ARM Compiler 5 alignment keyword; writes to global g_decl_align_bytes */
/* __at(addr) - ARM Compiler 5 absolute address; token already determined as TOK_AT.
 * Form like __at(0x40020014): consume keyword and parens, write to g_attr_has_at/g_attr_at_addr.
 * Caller should call this at appropriate positions; at least supports before declspec and after var name. */
static void parse_at_keyword(Parser *p) {
    if (lexer_peek(p->lex).kind != TOK_AT) return;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind == TOK_LPAREN) {
        lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_NUM) {
            g_attr_at_addr = (unsigned)lexer_peek(p->lex).value;
            g_attr_has_at = 1;
            lexer_advance(p->lex);
        }
        if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    }
}

#if ARKCC_FEATURE_ARMCC_KEYWORDS
/* __global_reg(n) - ARM Compiler 5 global register variable declaration.
 * Form like __global_reg(6) int x;: returns register number, -1 means not matched. */
static int parse_global_reg(Parser *p) {
    int reg = -1;
    if (lexer_peek(p->lex).kind != TOK_GLOBAL_REG) return -1;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind == TOK_LPAREN) {
        lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_NUM) {
            reg = lexer_peek(p->lex).value;
            lexer_advance(p->lex);
        }
        if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    }
    return reg;
}

/* __smc(n) - ARM Compiler 5 SMC (Secure Monitor Call) declaration.
 * Same structure as __svc(n), returns number, 0 means not matched. */
static int parse_smc(Parser *p) {
    int num = 0;
    if (lexer_peek(p->lex).kind != TOK_SMC) return 0;
    lexer_advance(p->lex);
    if (lexer_peek(p->lex).kind == TOK_LPAREN) {
        lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_NUM) {
            num = lexer_peek(p->lex).value;
            lexer_advance(p->lex);
        }
        if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
    }
    return num;
}
#endif

/* Top-level declaration */
/* Parameter list of a function declarator; the caller has already consumed
 * the '('.  Both the file-scope path and block-scope declarations
 * (C99 6.2.2p5, e.g. "extern int f(void);" inside a body) need exactly this,
 * and a prototype that is parsed in one place but not the other would make
 * call sites promote arguments differently depending on where the function
 * happened to be declared. */
static void parse_param_list(Parser *p, AstNode ***out_params, int *out_count,
                             int *out_variadic, int line) {
    AstNode **params = 0;
    int param_count = 0;
    int param_capacity = 0;
    int is_variadic = 0;
            if (lexer_peek(p->lex).kind == TOK_VOID && lexer_peek2(p->lex).kind == TOK_RPAREN) {
                lexer_advance(p->lex);
            } else if (lexer_peek(p->lex).kind != TOK_RPAREN) {
                for (;;) {
                    int psto = 0, ptd = 0;
                    Type *pbase;
                    Type *pt;
                    char *pname = 0;
                    if (lexer_peek(p->lex).kind == TOK_ELLIPSIS) {
                        lexer_advance(p->lex);
                        is_variadic = 1;
                        break;
                    }
                    pbase = parse_declspec(p, &psto, &ptd);
                    if (!pbase) { perror_at(p, "expected param type"); break; }
                    pt = parse_declarator(p, pbase, &pname);
                    /* C99 6.7.5.3p7: array parameters decay; "array of T" becomes "pointer to T".
                     * First dimension decays: int arr[5] -> int *arr;
                     * int m[3][4] -> int (*m)[4]; only outermost decays to pointer.
                     * For [static expr] parameter form, size is known but still decays to pointer.
                     * After decay, arr[i] access uses pointer arithmetic semantics.
                     * This matches C standard parameter decay rules. */
                    if (pt && pt->kind == TYPE_ARRAY) {
                        pt = type_pointer(pt->base);
                    }
                    if (!pname) pname = mem_strdup("_anon");
                    {
                        AstNode *param = ast_var_decl(pname, pt, 0, line);
                        param->col = p->name_col;   /* for the caret on #100 */
                        if (param_count >= param_capacity) {
                            int nc = param_capacity ? param_capacity * 2 : 4;
                            params = (AstNode **)mem_realloc(params,
                                param_capacity * (int)sizeof(AstNode *),
                                nc * (int)sizeof(AstNode *));
                            param_capacity = nc;
                        }
                        params[param_count++] = param;
                    }
                    if (lexer_peek(p->lex).kind == TOK_COMMA) { lexer_advance(p->lex); continue; }
                    break;
                }
            }
    *out_params = params;
    *out_count = param_count;
    *out_variadic = is_variadic;
}

/* Consume a `( params )` tail and build the function type `ret(params)`.
 * Used for typedefs that name a function type (`typedef int binop(int,int);`).
 * parse_declarator stops in front of the parameter list for that spelling --
 * it only knows the `(*name)(params)` pointer form -- so the caller has to
 * finish the type.  Returns 0 (consuming nothing) when the next token is not
 * '('. */
static int parse_func_type_tail(Parser *p, Type *ret, Type **out_ft, int line) {
    AstNode **params = 0;
    int param_count = 0;
    int is_variadic = 0;
    Type **ptypes = 0;
    int i;
    if (lexer_peek(p->lex).kind != TOK_LPAREN) return 0;
    lexer_advance(p->lex);              /* '(' */
    parse_param_list(p, &params, &param_count, &is_variadic, line);
    lexer_expect(p->lex, TOK_RPAREN);
    while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) parse_attribute(p, ret);
    if (param_count > 0 && params) {
        ptypes = (Type **)mem_pool_alloc(param_count * (int)sizeof(Type *));
        for (i = 0; i < param_count; i++)
            ptypes[i] = !params[i] ? 0
                        : (params[i]->type ? params[i]->type
                                           : params[i]->u.var.var_type);
    }
    if (out_ft) *out_ft = type_func(ret, ptypes, param_count, is_variadic);
    return 1;
}

/* The AST prototype node wants AstNode parameters; a function *type* only
 * carries Type pointers.  Wrap each one in a plain AST_VAR -- for a
 * declaration only the types matter, argument conversion reads them. */
static AstNode **params_from_func_type(Type *ft, int *out_count, int line) {
    AstNode **ps = 0;
    int n = (ft && ft->params && ft->param_count > 0) ? ft->param_count : 0;
    int i;
    if (n > 0) {
        ps = (AstNode **)mem_pool_alloc(n * (int)sizeof(AstNode *));
        for (i = 0; i < n; i++)
            ps[i] = ast_var_decl(mem_strdup("_anon"), ft->params[i], 0, line);
    }
    if (out_count) *out_count = n;
    return ps;
}

static AstNode *parse_top_decl(Parser *p) {
    int storage = 0;
    int is_td = 0;
    int decl_name_col = 0;        /* column of the declared name; for carets */
    Type *base;
    int line = lexer_peek(p->lex).line;
    int func_mods = 0;            /* __irq/__weak/__naked bitmask: 1/2/4 */
    int decl_is_const = 0;        /* const state of this declaration (captured then reset global) */
    char *decl_section = 0;       /* section name of this declaration (captured then reset global) */
    int   is_asm_fn = 0;          /* __asm void fn(){...} function-body-asm marker */
    int   decl_align = 0;         /* __align(n) parse result */
    int   decl_is_tls = 0;        /* __thread thread-local storage marker */
    int   smc_num = -1;           /* __smc(n) number; -1=unused */
    int   global_reg_num = -1;    /* __global_reg(n) register number; -1=unused */
    int   func_is_value_in_regs = 0;
    int   func_is_pure = 0;
    int   func_is_const_attr = 0;
    int   func_is_interwork = 0;
    int   func_is_deprecated_kw = 0;
    /* Reset __attribute__-driven function attribute globals to avoid cross-declaration leakage */
    g_attr_is_weak = 0; g_attr_is_naked = 0; g_attr_is_irq = 0;
    g_attr_section = 0; g_decl_is_const = 0; g_decl_is_volatile = 0;
    g_attr_is_used = 0; g_attr_is_unused = 0;
    g_attr_alias_target = 0;
    g_attr_align_bytes = 0; g_decl_align_bytes = 0;
    g_attr_is_softfp = 0; g_attr_is_forceinline = 0;
    g_attr_has_at = 0; g_attr_at_addr = 0;
    g_attr_is_constructor = 0; g_attr_is_destructor = 0; g_attr_is_deprecated = 0;
    g_decl_packed_ptr = 0;  /* __packed pointer flag: reset at each declaration start */
    g_vla_size_expr = 0;  /* B3: file scope does not allow VLA, reset to prevent leakage */
    /* 0.6.x: reset armcc 5 compatible function modifier globals */
    g_func_is_value_in_regs = 0;
    g_func_is_pure = 0;
    g_func_is_const_attr = 0;
    g_func_is_interwork = 0;
    g_func_is_deprecated_kw = 0;
    /* armcc-style __asm void fn(){...}: detect and set is_asm_fn. */
#if ARKCC_FEATURE_ARMCC_ASM_FUNC
    if (lexer_peek(p->lex).kind == TOK_ASM) {
        /* Distinguish __asm <type> name(...){...} from __asm("...");
         * Look ahead at the second token: if it's a type or a typedef
         * identifier, treat __asm as a function qualifier and continue. */
        Token t2 = lexer_peek2(p->lex);
        if (is_basic_type_token(t2.kind) ||
            (t2.kind == TOK_IDENT && sym_is_typedef(t2.str))) {
            is_asm_fn = 1;
            lexer_advance(p->lex);  /* Consume __asm */
        }
    }
    /* __align(n): can appear before declspec */
    if (lexer_peek(p->lex).kind == TOK_ALIGN_KW) {
        lexer_advance(p->lex);
        if (lexer_peek(p->lex).kind == TOK_LPAREN) {
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_NUM) {
                decl_align = lexer_peek(p->lex).value;
                lexer_advance(p->lex);
            }
            if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
        }
    }
    /* __at(addr): can appear before declspec */
    if (lexer_peek(p->lex).kind == TOK_AT) parse_at_keyword(p);
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    /* __global_reg(n): can appear before declspec (global register variable) */
    if (lexer_peek(p->lex).kind == TOK_GLOBAL_REG) {
        global_reg_num = parse_global_reg(p);
    }
#endif
    func_mods |= parse_func_modifiers(p);  /* Leading __irq etc (before return type) */
    base = parse_declspec(p, &storage, &is_td);
    /* Pick up function modifier keywords (irq/weak/naked/softfp/forceinline) that appeared
     * inside the declaration specifier, e.g. `static __forceinline int func()`. */
    func_mods |= g_func_mods_declspec;
    /* Pick up __asm function-body marker set by parse_declspec when __asm appears
     * between storage class keywords and the return type
     * (e.g. `static __inline __asm uint32_t __REV16(...)`). */
    is_asm_fn |= g_decl_is_asm_fn;
    decl_is_const = g_decl_is_const;
    decl_section = g_attr_section;
    /* A _Alignas request wins only if it is at least the natural alignment;
     * base's alignment is the conservative bound available before the
     * declarator is parsed, and the assignment below clamps again per
     * declared object. */
    if (g_alignas_bytes > 0) {
        int nat = type_align(base);
        int want = g_alignas_bytes > nat ? g_alignas_bytes : nat;
        if (want > g_attr_align_bytes) g_attr_align_bytes = want;
        g_alignas_bytes = 0;
    }
    if (g_attr_align_bytes && !decl_align) decl_align = g_attr_align_bytes;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
    /* __thread sets storage 0x10000 bit in parse_declspec */
    decl_is_tls = (storage & 0x10000) ? 1 : 0;
    /* Check __global_reg again after parsing (can appear after type) */
    if (lexer_peek(p->lex).kind == TOK_GLOBAL_REG && global_reg_num < 0) {
        global_reg_num = parse_global_reg(p);
    }
#endif
    if (!base) {
        perror_at(p, "expected declaration at top level");
        return 0;
    }
    /* Only struct/union/enum;; */
    if (lexer_peek(p->lex).kind == TOK_SEMICOLON) {
        lexer_advance(p->lex);
        return ast_new(AST_BLOCK, line); /* Empty placeholder */
    }

    /* Collect one declarator */
    {
        char *name = 0;
        int svc_num = 0;
        func_mods |= parse_func_modifiers(p);  /* After type, before declarator */
        /* __svc(n) appears between type and declarator */
        if (lexer_peek(p->lex).kind == TOK_SVC) {
            svc_num = parse_svc(p);
        }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        /* __smc(n) appears between type and declarator, same position as __svc */
        if (lexer_peek(p->lex).kind == TOK_SMC) {
            smc_num = parse_smc(p);
        }
#endif
        func_mods |= parse_func_modifiers(p);  /* After __svc */
        /* Capture armcc 5 compatible attributes after parsing function modifiers */
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        func_is_value_in_regs = g_func_is_value_in_regs;
        func_is_pure          = g_func_is_pure;
        func_is_const_attr    = g_func_is_const_attr;
        func_is_interwork     = g_func_is_interwork;
        func_is_deprecated_kw = g_func_is_deprecated_kw;
#endif
        Type *t = parse_declarator(p, base, &name);
        if (!name) {
            perror_at(p, "expected identifier in declaration");
            return 0;
        }
        /* Where the name itself was: declaration-level diagnostics are anchored
         * to the identifier, so the node has to remember the column. */
        decl_name_col = p->name_col;

        /* `typedef Ret F(params);` -- a typedef that names a function type.
         * parse_declarator returns before the parameter list for this
         * spelling, so the '(' used to be taken by the function-declaration
         * branch below and the typedef was dropped on the floor: the name
         * was registered as SYM_FUNC, `sizeof(binop)` folded to the generic
         * 4, and `binop *p;` was rejected as a data declaration.  Record a
         * real function type instead (C99 6.7.1 + 6.7.5: the declared type
         * is "function returning Ret"), so the name behaves exactly like any
         * other type name afterwards. */
        if (is_td) {
            Type *ft = 0;
            if (parse_func_type_tail(p, t, &ft, line)) {
                Type *first_ft = ft;
                sym_add(SYM_TYPEDEF, name, ft, 0);
                while (lexer_peek(p->lex).kind == TOK_COMMA) {
                    char *nm2 = 0;
                    Type *t2, *ft2 = 0;
                    lexer_advance(p->lex);
                    t2 = parse_declarator(p, base, &nm2);
                    if (!nm2) break;
                    if (parse_func_type_tail(p, t2, &ft2, line)) t2 = ft2;
                    sym_add(SYM_TYPEDEF, nm2, t2, 0);
                }
                lexer_expect(p->lex, TOK_SEMICOLON);
                {
                    AstNode *n = ast_new(AST_TYPEDEF, line);
                    n->u.var.name = name;
                    n->u.var.var_type = first_ft;
                    return n;
                }
            }
        }

        /* Function definition */
        if (lexer_peek(p->lex).kind == TOK_LPAREN) {
            AstNode **params = 0;
            int param_count = 0;
            int param_capacity = 0;
            int is_variadic = 0;
            Type *ret_type = t;
            AstNode *body = 0;

            lexer_advance(p->lex);
            parse_param_list(p, &params, &param_count, &is_variadic, line);
            lexer_expect(p->lex, TOK_RPAREN);
            /* Function declaration/definition may have trailing __attribute__ */
            while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) parse_attribute(p, ret_type);
            func_mods |= parse_func_modifiers(p);  /* __irq etc after parameter list */
            /* Merge __attribute__((weak/naked/interrupt)) driven function attributes */
            if (g_attr_is_irq)   func_mods |= 1;
            if (g_attr_is_weak)  func_mods |= 2;
            if (g_attr_is_naked) func_mods |= 4;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* armcc 5 compatible modifiers appearing after parameter list, merge to local */
            if (g_func_is_value_in_regs) func_is_value_in_regs = 1;
            if (g_func_is_pure)          func_is_pure = 1;
            if (g_func_is_const_attr)    func_is_const_attr = 1;
            if (g_func_is_interwork)     func_is_interwork = 1;
            if (g_func_is_deprecated_kw) func_is_deprecated_kw = 1;
#endif
            /* Register the function in the parser symbol table. Purely
             * syntactic folds run before sema and need the return type:
             * `sizeof(f(...))` folds to a constant while parsing, and sema
             * only fills AST types afterwards. Same rationale as the
             * SYM_VAR registration for globals below. sema_func reuses this
             * entry (it looks up sym_find_current first), so no duplicate
             * definition appears. */
            if (name) {
                Symbol *fsym = sym_find_current(name);
                if (!fsym || fsym->kind != SYM_FUNC) {
                    fsym = sym_add(SYM_FUNC, name, ret_type, 0);
                } else if (!fsym->type || fsym->type->kind == TYPE_VOID) {
                    fsym->type = ret_type;
                }
            }
            if (lexer_peek(p->lex).kind == TOK_SEMICOLON) {
                AstNode *n;
                lexer_advance(p->lex);
                n = ast_func_def(name, ret_type, params, param_count, 0, line);
                if (n) n->col = decl_name_col;
                n->kind = AST_FUNC_DECL;
                n->u.func.is_variadic = is_variadic;
                n->u.func.is_static = (storage & STO_STATIC) ? 1 : 0;
                n->u.func.svc_num = svc_num;
                n->u.func.is_irq   = (func_mods & 1) ? 1 : 0;
                n->u.func.is_weak  = (func_mods & 2) ? 1 : 0;
                n->u.func.is_naked = (func_mods & 4) ? 1 : 0;
                n->u.func.is_softfp     = ((func_mods & 8) || g_attr_is_softfp) ? 1 : 0;
                n->u.func.forceinline   = ((func_mods & 16) || g_attr_is_forceinline) ? 1 : 0;
                n->u.func.is_used       = g_attr_is_used;
                n->u.func.is_unused     = g_attr_is_unused;
                n->u.func.alias_target  = g_attr_alias_target;
                n->u.func.is_constructor = g_attr_is_constructor;
                n->u.func.is_destructor  = g_attr_is_destructor;
                n->u.func.is_deprecated  = g_attr_is_deprecated;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                n->u.func.is_value_in_regs = func_is_value_in_regs;
                n->u.func.is_pure          = func_is_pure;
                n->u.func.is_const_attr    = func_is_const_attr;
                n->u.func.is_interwork     = func_is_interwork;
                n->u.func.smc_num          = smc_num;
                n->u.func.global_reg       = global_reg_num;
                n->u.func.is_deprecated   |= func_is_deprecated_kw;
#endif
                return n;
            }
            /* __asm function body: copy the literal source between { and } into
             * a string; the backend emits it verbatim. We do not lex the body
             * because it contains arbitrary asm tokens that the lexer would
             * mis-parse. */
            if (is_asm_fn) {
                AstNode *n;
                const char *body_start = 0;
                const char *body_end = 0;
                char *body_text = 0;
                if (lexer_peek(p->lex).kind != TOK_LBRACE) {
                    perror_at(p, "expected '{' in __asm function body");
                } else {
                    Token lb = lexer_peek(p->lex);
                    /* lb.src_start points to '{' position */
                    body_start = lb.src_start ? lb.src_start + 1 : 0;
                    /* Advance lexer past '}' using bracket depth counting */
                    {
                        int depth = 0;
                        for (;;) {
                            TokenKind k = lexer_peek(p->lex).kind;
                            if (k == TOK_EOF) break;
                            if (k == TOK_LBRACE) {
                                depth++;
                                lexer_advance(p->lex);
                                continue;
                            }
                            if (k == TOK_RBRACE) {
                                depth--;
                                if (depth == 0) {
                                    body_end = lexer_peek(p->lex).src_start;
                                    lexer_advance(p->lex);
                                    break;
                                }
                                lexer_advance(p->lex);
                                continue;
                            }
                            lexer_advance(p->lex);
                        }
                    }
                    if (body_start && body_end && body_end >= body_start) {
                        int blen = (int)(body_end - body_start);
                        body_text = mem_strndup(body_start, blen);
                    } else {
                        body_text = mem_strdup("");
                    }
                }
                n = ast_func_def(name, ret_type, params, param_count, 0, line);
                if (n) n->col = decl_name_col;
                n->kind = AST_ASM_FUNC;
                n->u.func.is_variadic = is_variadic;
                n->u.func.is_static = (storage & STO_STATIC) ? 1 : 0;
                n->u.func.is_asm_func = 1;
                n->u.func.asm_body = body_text;
                n->u.func.is_used       = g_attr_is_used;
                n->u.func.is_unused     = g_attr_is_unused;
                n->u.func.alias_target  = g_attr_alias_target;
                n->u.func.is_constructor = g_attr_is_constructor;
                n->u.func.is_destructor  = g_attr_is_destructor;
                n->u.func.is_deprecated  = g_attr_is_deprecated;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                n->u.func.is_value_in_regs = func_is_value_in_regs;
                n->u.func.is_pure          = func_is_pure;
                n->u.func.is_const_attr    = func_is_const_attr;
                n->u.func.is_interwork     = func_is_interwork;
                n->u.func.smc_num          = smc_num;
                n->u.func.global_reg       = global_reg_num;
                n->u.func.is_deprecated   |= func_is_deprecated_kw;
#endif
                return n;
            }
#endif /* ARKCC_FEATURE_ARMCC_ASM_FUNC */
            g_current_func_name = name;
            body = parse_block(p);
            {
                AstNode *n = ast_func_def(name, ret_type, params, param_count, body, line);
                if (n) n->col = decl_name_col;
                n->u.func.is_variadic = is_variadic;
                n->u.func.is_static = (storage & STO_STATIC) ? 1 : 0;
                n->u.func.svc_num = svc_num;
                n->u.func.is_irq   = (func_mods & 1) ? 1 : 0;
                n->u.func.is_weak  = (func_mods & 2) ? 1 : 0;
                n->u.func.is_naked = (func_mods & 4) ? 1 : 0;
                n->u.func.is_softfp     = ((func_mods & 8) || g_attr_is_softfp) ? 1 : 0;
                n->u.func.forceinline   = ((func_mods & 16) || g_attr_is_forceinline) ? 1 : 0;
                n->u.func.is_used       = g_attr_is_used;
                n->u.func.is_unused     = g_attr_is_unused;
                n->u.func.alias_target  = g_attr_alias_target;
                n->u.func.is_constructor = g_attr_is_constructor;
                n->u.func.is_destructor  = g_attr_is_destructor;
                n->u.func.is_deprecated  = g_attr_is_deprecated;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                n->u.func.is_value_in_regs = func_is_value_in_regs;
                n->u.func.is_pure          = func_is_pure;
                n->u.func.is_const_attr    = func_is_const_attr;
                n->u.func.is_interwork     = func_is_interwork;
                n->u.func.smc_num          = smc_num;
                n->u.func.global_reg       = global_reg_num;
                n->u.func.is_deprecated   |= func_is_deprecated_kw;
#endif
                g_current_func_name = 0;
                return n;
            }
        }

        /* Top-level variable or typedef */
        if (is_td) {
            sym_add(SYM_TYPEDEF, name, t, 0);
            /* Multiple typedef: same line a, b, c; */
            while (lexer_peek(p->lex).kind == TOK_COMMA) {
                char *nm2 = 0;
                lexer_advance(p->lex);
                t = parse_declarator(p, base, &nm2);
                if (nm2) sym_add(SYM_TYPEDEF, nm2, t, 0);
            }
            lexer_expect(p->lex, TOK_SEMICOLON);
            {
                AstNode *n = ast_new(AST_TYPEDEF, line);
                n->u.var.name = name;
                n->u.var.var_type = t;
                return n;
            }
        }
        /* A declaration whose type is a function type, spelled through a
         * typedef: `binop add;`.  Without this the name became a 4-byte data
         * object, so every call to it stayed unresolved and the argument
         * conversions never ran.  Build the same AST_FUNC_DECL that
         * `int add(int, int);` produces.  A function *definition* cannot be
         * written through a typedef (C99 6.9.1 requires the identifier in the
         * declarator), so anything else here is reported instead of guessed
         * at. */
        if (t && t->kind == TYPE_FUNC) {
            if (lexer_peek(p->lex).kind != TOK_SEMICOLON) {
                perror_at(p, "a declaration with a function type must end with ';' "
                             "(a function definition cannot use a typedef'd type)");
                return 0;
            }
            {
                AstNode **fparams = 0;
                int fpc = 0;
                AstNode *fn;
                fparams = params_from_func_type(t, &fpc, line);
                if (name) {
                    Symbol *fsym = sym_find_current(name);
                    if (!fsym || fsym->kind != SYM_FUNC) {
                        sym_add(SYM_FUNC, name, t->base, 0);
                    } else if (!fsym->type || fsym->type->kind == TYPE_VOID) {
                        fsym->type = t->base;
                    }
                }
                lexer_advance(p->lex);      /* ';' */
                fn = ast_func_def(name, t->base, fparams, fpc, 0, line);
                if (fn) fn->col = decl_name_col;
                fn->kind = AST_FUNC_DECL;
                fn->u.func.is_variadic = t->is_variadic;
                fn->u.func.is_static = (storage & STO_STATIC) ? 1 : 0;
                fn->u.func.svc_num = 0;
                fn->u.func.smc_num = -1;
                return fn;
            }
        }

        /* Global variable */
        {
            AstNode *init = 0;
            char *cur_section = decl_section;     /* First variable uses section from declspec/leading attribute */
            int   cur_const    = decl_is_const;
            /* Global variable name may have trailing __attribute__((...)) or __at(...) */
            while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE ||
                   lexer_peek(p->lex).kind == TOK_AT) {
                if (lexer_peek(p->lex).kind == TOK_AT) {
                    parse_at_keyword(p);
                } else {
                    g_attr_section = 0;
                    parse_attribute(p, t);
                    if (g_attr_section) { cur_section = g_attr_section; }
                }
            }
            if (lexer_peek(p->lex).kind == TOK_ASSIGN) {
                lexer_advance(p->lex);
                init = parse_initializer(p);
            }
            /* Deduce unspecified array dimension */
            infer_array_size_from_init(t, init);
            /* Register global var in parser symtab so that sizeof(var) and
             * sizeof(var[i]) can be resolved at parse time (e.g. the
             * sizeof(g_cpu_table)/sizeof(g_cpu_table[0]) array-count idiom
             * in cpu.c). sema_decl upgrades this SYM_VAR entry to SYM_GLOBAL. */
            sym_add(SYM_VAR, name, t, 0);
            {
                AstNode *n = ast_var_decl(name, t, init, line);
                n->col = decl_name_col;
                n->u.var.is_global = 1;
                n->u.var.is_static = (storage & STO_STATIC) ? 1 : 0;
                n->u.var.is_extern = (storage & STO_EXTERN) ? 1 : 0;
                n->u.var.is_const  = cur_const;
                n->u.var.section   = cur_section;
                n->u.var.align_bytes = decl_align ? decl_align : g_attr_align_bytes;
                n->u.var.is_used    = g_attr_is_used;
                n->u.var.is_unused  = g_attr_is_unused;
                n->u.var.has_at     = g_attr_has_at;
                n->u.var.at_addr    = g_attr_at_addr;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                n->u.var.is_tls     = decl_is_tls;
                n->u.var.global_reg = global_reg_num;
#endif
                /* Handle same-line multiple variable declarations a, b = 2; */
                while (lexer_peek(p->lex).kind == TOK_COMMA) {
                    char *nm2 = 0;
                    Type *t2;
                    AstNode *init2 = 0;
                    char *sec2 = 0;
                    lexer_advance(p->lex);
                    t2 = parse_declarator(p, base, &nm2);
                    while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) {
                        g_attr_section = 0;
                        parse_attribute(p, t2);
                        if (g_attr_section) sec2 = g_attr_section;
                    }
                    if (lexer_peek(p->lex).kind == TOK_ASSIGN) {
                        lexer_advance(p->lex);
                        init2 = parse_initializer(p);
                    }
                    infer_array_size_from_init(t2, init2);
                    /* Register subsequent same-line global vars too, for sizeof lookup. */
                    if (nm2) sym_add(SYM_VAR, nm2, t2, 0);
                    if (nm2) {
                        AstNode *n2 = ast_var_decl(nm2, t2, init2, line);
                        n2->col = p->name_col;
                        n2->u.var.is_global = 1;
                        n2->u.var.is_static = (storage & STO_STATIC) ? 1 : 0;
                        n2->u.var.is_extern = (storage & STO_EXTERN) ? 1 : 0;
                        n2->u.var.is_const  = cur_const;
                        n2->u.var.section   = sec2;
                        /* Wrap via block: use BLOCK to represent a group of declarations */
                        if (n->kind != AST_BLOCK) {
                            AstNode *blk = ast_block(line);
                            block_add_stmt(blk, n);
                            block_add_stmt(blk, n2);
                            n = blk;
                        } else {
                            block_add_stmt(n, n2);
                        }
                    }
                }
                lexer_expect(p->lex, TOK_SEMICOLON);
                return n;
            }
        }
    }
}

static AstNode *parse_block(Parser *p) {
    int line = lexer_peek(p->lex).line;
    AstNode *blk;
    lexer_expect(p->lex, TOK_LBRACE);
    sym_enter_scope();
    blk = ast_block(line);
    while (lexer_peek(p->lex).kind != TOK_RBRACE &&
           lexer_peek(p->lex).kind != TOK_EOF) {
        AstNode *s;
        const char *before;
        /* Same stop as parse_program: no point building an AST for statements
         * of a function we already failed to understand. */
        if (diag_at_limit()) break;
        before = p->lex->p;
        s = parse_statement(p);
        if (s) {
            block_add_stmt(blk, s);
        }
        /* Recovery has to consume something.  A statement the parser cannot
         * even start used to be retried at the same token forever: measured on
         * the board with `u.v = va_arg(ap, long long);` and no <stdarg.h>,
         * #65/#29 repeated on line 36 until the 4 MB pool was gone (595 ms),
         * and the last thing printed was "memory pool exhausted" instead of
         * the missing include. */
        if (p->lex->p == before) lexer_advance(p->lex);
    }
    sym_leave_scope();
    lexer_expect(p->lex, TOK_RBRACE);
    return blk;
}

/* Local variable declaration (including same-line multiple variables) */
static AstNode *parse_local_var_decl(Parser *p, Type *base, int storage, int is_typedef) {
    int line = lexer_peek(p->lex).line;
    AstNode *blk = 0;
    /* Capture const in declspec; local variable const usually has no section meaning but keep passing */
    int cur_const = g_decl_is_const;
    for (;;) {
        char *name = 0;
        Type *t = parse_declarator(p, base, &name);
        int decl_name_col = p->name_col;
        AstNode *init = 0;
        char *cur_section = 0;
        if (!name) {
            perror_at(p, "expected identifier in declaration");
            break;
        }
        /* Block-scope function declaration: "extern int f(void);", and the
         * same without "extern" (C99 6.2.2p5 gives it external linkage
         * either way).  parse_declarator only knows the (*name)(...)
         * function-pointer form, so it stops before the parameter list;
         * without this branch the '(' was read as the start of an
         * initialiser and the statement failed with "#65: expected a ';'".
         * src/compiler.c and src/lexer.c hit exactly that.
         *
         * The node is an AST_FUNC_DECL -- the same node the file-scope path
         * builds -- so sema and irgen treat it as an ordinary prototype: no
         * code is emitted, and the parameter types are recorded so call
         * sites promote arguments exactly as they would from a file-scope
         * declaration.  Using the real AST also keeps the symbol alive for
         * the statements that follow inside the same block, which is what
         * lets `p = arm_get_symbol_count;` resolve. */
        /* Block-scope typedef of a function type: `typedef int binop(int,int);`
         * inside a function body.  Same bug and same fix as at file scope --
         * the prototype branch below would otherwise swallow the '(' and
         * register the name as a function. */
        if (is_typedef) {
            Type *ft = 0;
            if (parse_func_type_tail(p, t, &ft, line)) {
                sym_add(SYM_TYPEDEF, name, ft, 0);
                while (lexer_peek(p->lex).kind == TOK_COMMA) {
                    char *nm2 = 0;
                    Type *t2, *ft2 = 0;
                    lexer_advance(p->lex);
                    t2 = parse_declarator(p, base, &nm2);
                    if (!nm2) break;
                    if (parse_func_type_tail(p, t2, &ft2, line)) t2 = ft2;
                    sym_add(SYM_TYPEDEF, nm2, t2, 0);
                }
                lexer_expect(p->lex, TOK_SEMICOLON);
                {
                    AstNode *n = ast_new(AST_TYPEDEF, line);
                    n->u.var.name = name;
                    n->u.var.var_type = ft;
                    if (!blk) blk = n;
                    return blk;
                }
            }
        }
        if (name && lexer_peek(p->lex).kind == TOK_LPAREN) {
            AstNode **params = 0;
            int param_count = 0;
            int is_variadic = 0;
            AstNode *fn;
            lexer_advance(p->lex);           /* '(' */
            parse_param_list(p, &params, &param_count, &is_variadic, line);
            lexer_expect(p->lex, TOK_RPAREN);
            while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) parse_attribute(p, t);
            fn = ast_func_def(name, t, params, param_count, 0, line);
            if (fn) {
                fn->kind = AST_FUNC_DECL;
                fn->col  = decl_name_col;
                fn->u.func.is_variadic = is_variadic;
                fn->u.func.is_static = (storage & STO_STATIC) ? 1 : 0;
                /* __svc(n)/__smc(n) are not parsed in this position (armcc
                 * writes them before the declarator, which the local-decl
                 * path does not accept -- it reports an error instead of
                 * guessing).  Say "no SVC/SMC" explicitly: irgen accepts any
                 * smc_num >= 0 as a real __smc function, and the pool hands
                 * out zero-filled nodes. */
                fn->u.func.svc_num = 0;
                fn->u.func.smc_num = -1;
            }
            if (lexer_peek(p->lex).kind == TOK_COMMA) { lexer_advance(p->lex); continue; }
            lexer_expect(p->lex, TOK_SEMICOLON);
            /* The ';' is consumed here; falling out of the loop would make
             * the caller expect a second one, and the next statement then
             * failed with "#65: expected a ';'".  A mixed declarator list
             * ("int a, f(void);") keeps whatever came before it. */
            if (!blk) {
                blk = fn ? fn : ast_new(AST_BLOCK, line);
            } else if (fn) {
                AstNode *b = ast_block(line);
                b->u.block.is_decl_list = 1;
                block_add_stmt(b, blk);
                block_add_stmt(b, fn);
                blk = b;
            }
            return blk;
        }
        /* Block-scope prototype written through a function-type typedef:
         * `binop add;` (C99 6.2.2p5: still external).  Same node as the
         * `int add(int,int);` spelling, so call sites convert arguments
         * identically. */
        if (!is_typedef && t && t->kind == TYPE_FUNC) {
            AstNode *fn;
            if (lexer_peek(p->lex).kind != TOK_SEMICOLON) {
                perror_at(p, "a declaration with a function type must end with ';'");
                break;
            }
            {
                AstNode **fparams = 0;
                int fpc = 0;
                fparams = params_from_func_type(t, &fpc, line);
                fn = ast_func_def(name, t->base, fparams, fpc, 0, line);
                if (fn) fn->col = decl_name_col;
                fn->kind = AST_FUNC_DECL;
                fn->u.func.is_variadic = t->is_variadic;
                fn->u.func.is_static = (storage & STO_STATIC) ? 1 : 0;
                fn->u.func.svc_num = 0;
                fn->u.func.smc_num = -1;
            }
            lexer_advance(p->lex);          /* ';' */
            if (!blk) {
                blk = fn;
            } else {
                AstNode *b = ast_block(line);
                b->u.block.is_decl_list = 1;
                block_add_stmt(b, blk);
                block_add_stmt(b, fn);
                blk = b;
            }
            return blk;
        }
        if (is_typedef) {
            sym_add(SYM_TYPEDEF, name, t, 0);
        }
        while (lexer_peek(p->lex).kind == TOK_ATTRIBUTE) {
            g_attr_section = 0;
            parse_attribute(p, t);
            if (g_attr_section) cur_section = g_attr_section;
        }
        if (lexer_peek(p->lex).kind == TOK_ASSIGN) {
            lexer_advance(p->lex);
            init = parse_initializer(p);
        }
        infer_array_size_from_init(t, init);
        {
            AstNode *n = ast_var_decl(name, t, init, line);
            n->col = decl_name_col;
            /* For char buf[sizeof(x)] form, deduce size via sizeof at parse time.
             * z is a placeholder; actual size computed during sema. parse_block handles this.
             * This is a compile-time construct; sema does the actual size computation. */
            if (!is_typedef && name) {
                sym_add(SYM_VAR, name, t, 0);
            }
            n->u.var.is_static = (storage & STO_STATIC) ? 1 : 0;
            n->u.var.is_extern = (storage & STO_EXTERN) ? 1 : 0;
            n->u.var.is_const  = cur_const;
            n->u.var.section   = cur_section;
            n->u.var.align_bytes = g_attr_align_bytes;
            n->u.var.is_used    = g_attr_is_used;
            n->u.var.is_unused  = g_attr_is_unused;
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* __thread local variable (TLS marker, only meaningful for static locals) */
            n->u.var.is_tls = (storage & 0x10000) ? 1 : 0;
#endif
#if ARKCC_FEATURE_VLA
            /* B3: consume VLA size expression recorded by parse_declarator */
            if (g_vla_size_expr && g_config.enable_vla) {
                n->u.var.is_vla = 1;
                n->u.var.vla_size_expr = g_vla_size_expr;
                g_vla_size_expr = 0;
            }
#endif
            if (!blk) blk = n;
            else {
                if (blk->kind != AST_BLOCK) {
                    AstNode *b = ast_block(line);
                    b->u.block.is_decl_list = 1;  /* Comma-separated declaration block: no nested scope */
                    block_add_stmt(b, blk);
                    block_add_stmt(b, n);
                    blk = b;
                } else {
                    block_add_stmt(blk, n);
                }
            }
        }
        if (lexer_peek(p->lex).kind == TOK_COMMA) { lexer_advance(p->lex); continue; }
        break;
    }
    lexer_expect(p->lex, TOK_SEMICOLON);
    return blk;
}

static AstNode *parse_statement(Parser *p) {
    Token t = lexer_peek(p->lex);
    int line = t.line;

    if (t.kind == TOK_ASM) {
        AstNode *n = parse_asm_statement(p);
        return n;
    }

    if (t.kind == TOK_STATIC_ASSERT) {
        parse_static_assert(p);
        return ast_new(AST_BLOCK, line);
    }

    if (is_decl_start(p) || lexer_peek(p->lex).kind == TOK_ALIGN_KW ||
        lexer_peek(p->lex).kind == TOK_ALIGNAS) {
        int sto = 0, td = 0;
        Type *base;
        int local_align = 0;
        /* Reset const/section globals to avoid cross-statement leakage */
        g_decl_is_const = 0; g_decl_is_volatile = 0; g_attr_section = 0;
        g_attr_align_bytes = 0;
        g_attr_is_used = 0; g_attr_is_unused = 0;
        g_decl_packed_ptr = 0;
        /* __align(n) appears before local declaration */
        if (lexer_peek(p->lex).kind == TOK_ALIGN_KW) {
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_LPAREN) {
                lexer_advance(p->lex);
                if (lexer_peek(p->lex).kind == TOK_NUM) {
                    local_align = lexer_peek(p->lex).value;
                    lexer_advance(p->lex);
                }
                if (lexer_peek(p->lex).kind == TOK_RPAREN) lexer_advance(p->lex);
            }
        }
        base = parse_declspec(p, &sto, &td);
        if (local_align && !g_attr_align_bytes) g_attr_align_bytes = local_align;
        if (g_alignas_bytes > 0) {
            if (g_alignas_bytes > 8) {
                /* The ARM/RISC-V backends build the frame as
                 * "sp -= frame; frame base = sp", with sp only guaranteed
                 * 8-byte aligned by AAPCS and every slot rounded to 8.  A
                 * block-scope request above 8 therefore cannot be honoured;
                 * saying so beats placing the object at 8 and letting the
                 * program assume 16/32/64 (DMA, cache-line, NEON). */
                Token lt = lexer_peek(p->lex);
                DiagLoc loc;
                loc.file = 0; loc.line = lt.line; loc.col = lt.col;
                diag_report_msg(DIAG_SEV_ERROR, &loc,
                    "block-scope _Alignas above 8 bytes is not supported"
                    " (the frame is 8-byte aligned)", 0, 0, 0);
                p->error_count++;
                g_alignas_bytes = 0;
            } else {
                int nat = type_align(base);
                int want = g_alignas_bytes > nat ? g_alignas_bytes : nat;
                if (want > g_attr_align_bytes) g_attr_align_bytes = want;
                g_alignas_bytes = 0;
            }
        }
        if (lexer_peek(p->lex).kind == TOK_SEMICOLON) {
            /* struct A {...}; empty declaration like this */
            lexer_advance(p->lex);
            return ast_new(AST_BLOCK, line);
        }
        return parse_local_var_decl(p, base, sto, td);
    }

    /* Label statement ident ':' */
    if (t.kind == TOK_IDENT && lexer_peek2(p->lex).kind == TOK_COLON) {
        char *name = mem_strdup(t.str);
        AstNode *body;
        lexer_advance(p->lex); /* ident */
        lexer_advance(p->lex); /* : */
        body = parse_statement(p);
        {
            AstNode *n = ast_new(AST_LABEL, line);
            n->u.label_stmt.name = name;
            n->u.label_stmt.stmt = body;
            return n;
        }
    }

    switch (t.kind) {
        case TOK_RETURN: {
            AstNode *val = 0;
            AstNode *n;
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind != TOK_SEMICOLON)
                val = parse_expr(p);
            lexer_expect(p->lex, TOK_SEMICOLON);
            n = ast_new(AST_RETURN, line);
            n->u.unary.operand = val;
            n->u.unary.op = 0;
            return n;
        }
        case TOK_IF: {
            AstNode *cond, *then_s, *else_s = 0;
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_LPAREN);
            cond = parse_expr(p);
            lexer_expect(p->lex, TOK_RPAREN);
            then_s = parse_statement(p);
            if (lexer_peek(p->lex).kind == TOK_ELSE) {
                lexer_advance(p->lex);
                else_s = parse_statement(p);
            }
            return ast_if(cond, then_s, else_s, line);
        }
        case TOK_WHILE: {
            AstNode *cond, *body;
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_LPAREN);
            cond = parse_expr(p);
            lexer_expect(p->lex, TOK_RPAREN);
            body = parse_statement(p);
            return ast_while(cond, body, line);
        }
        case TOK_FOR: {
            AstNode *init = 0, *cond = 0, *step = 0, *body;
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_LPAREN);
            sym_enter_scope();
            if (lexer_peek(p->lex).kind != TOK_SEMICOLON) {
                if (is_decl_start(p)) {
                    int sto = 0, td = 0;
                    Type *base;
                    g_decl_is_const = 0; g_decl_is_volatile = 0; g_attr_section = 0;
                    base = parse_declspec(p, &sto, &td);
                    init = parse_local_var_decl(p, base, sto, td);
                    /* parse_local_var_decl already ate ';' */
                    goto for_have_init_semi;
                } else {
                    init = parse_expr(p);
                }
            }
            lexer_expect(p->lex, TOK_SEMICOLON);
        for_have_init_semi:
            if (lexer_peek(p->lex).kind != TOK_SEMICOLON)
                cond = parse_expr(p);
            lexer_expect(p->lex, TOK_SEMICOLON);
            if (lexer_peek(p->lex).kind != TOK_RPAREN)
                step = parse_expr(p);
            lexer_expect(p->lex, TOK_RPAREN);
            body = parse_statement(p);
            sym_leave_scope();
            return ast_for(init, cond, step, body, line);
        }
        case TOK_DO: {
            AstNode *body, *cond;
            AstNode *n;
            lexer_advance(p->lex);
            body = parse_statement(p);
            lexer_expect(p->lex, TOK_WHILE);
            lexer_expect(p->lex, TOK_LPAREN);
            cond = parse_expr(p);
            lexer_expect(p->lex, TOK_RPAREN);
            lexer_expect(p->lex, TOK_SEMICOLON);
            n = ast_new(AST_DO_WHILE, line);
            n->u.while_stmt.cond = cond;
            n->u.while_stmt.body = body;
            return n;
        }
        case TOK_SWITCH: {
            AstNode *e, *body, *n;
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_LPAREN);
            e = parse_expr(p);
            lexer_expect(p->lex, TOK_RPAREN);
            body = parse_statement(p);
            n = ast_new(AST_SWITCH, line);
            n->u.switch_stmt.expr = e;
            n->u.switch_stmt.body = body;
            n->u.switch_stmt.cases = 0;
            n->u.switch_stmt.case_count = 0;
            n->u.switch_stmt.default_case = 0;
            return n;
        }
        case TOK_CASE: {
            AstNode *n;
            AstNode *ce;
            long long cv = 0;
            lexer_advance(p->lex);
            /* C99 6.10: case label is an integer constant expression.
             * Use the full conditional-expression parser so that cast
             * expressions (e.g. `case (int)TOK_PLUS:`), arithmetic
             * (e.g. `case 1+2:`), and enum constants all work. */
            ce = parse_cond_expr(p);
            if (ce) eval_const_expr(ce, &cv);
            lexer_expect(p->lex, TOK_COLON);
            n = ast_new(AST_CASE, line);
            n->u.case_stmt.value = cv;
            n->u.case_stmt.stmt = parse_statement(p);
            return n;
        }
        case TOK_DEFAULT: {
            AstNode *n;
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_COLON);
            n = ast_new(AST_DEFAULT, line);
            n->u.case_stmt.stmt = parse_statement(p);
            return n;
        }
        case TOK_GOTO: {
            AstNode *n;
            Token nt;
            lexer_advance(p->lex);
            nt = lexer_peek(p->lex);
            if (nt.kind != TOK_IDENT) { perror_at(p, "expected label name after goto"); }
            else lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_SEMICOLON);
            n = ast_new(AST_GOTO, line);
            n->u.name = nt.kind == TOK_IDENT ? mem_strdup(nt.str) : mem_strdup("_bad_label");
            return n;
        }
        case TOK_BREAK:
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_SEMICOLON);
            return ast_new(AST_BREAK, line);
        case TOK_CONTINUE:
            lexer_advance(p->lex);
            lexer_expect(p->lex, TOK_SEMICOLON);
            return ast_new(AST_CONTINUE, line);
        case TOK_LBRACE:
            return parse_block(p);
        case TOK_SEMICOLON:
            lexer_advance(p->lex);
            return ast_new(AST_BLOCK, line);
        default: {
            AstNode *e = parse_expr(p);
            AstNode *s;
            lexer_expect(p->lex, TOK_SEMICOLON);
            s = ast_new(AST_EXPR_STMT, line);
            s->u.unary.operand = e;
            return s;
        }
    }
}

static AstNode *parse_expr(Parser *p) {
    AstNode *e = parse_assign_expr(p);
    while (lexer_peek(p->lex).kind == TOK_COMMA) {
        int line = lexer_peek(p->lex).line;
        AstNode *r;
        AstNode *n;
        lexer_advance(p->lex);
        r = parse_assign_expr(p);
        n = ast_new(AST_COMMA, line);
        n->u.binary.left = e;
        n->u.binary.right = r;
        n->u.binary.op = (int)TOK_COMMA;
        e = n;
    }
    return e;
}

/* Source column for the caret. Nodes built from a token record that token's
 * start column; derived nodes inherit the column of the sub-expression they
 * stand for, so the caret lands on the construct the message talks about
 * rather than at the start of the line. 0 means "column unknown" and the
 * source echo is printed without a caret. */
static AstNode *anchor(AstNode *n, int col) {
    if (n && n->col <= 0) n->col = col;
    return n;
}

static AstNode *parse_assign_expr(Parser *p) {
    AstNode *left = parse_cond_expr(p);
    Token t = lexer_peek(p->lex);
    if (t.kind == TOK_ASSIGN || t.kind == TOK_ADD_ASSIGN || t.kind == TOK_SUB_ASSIGN ||
        t.kind == TOK_MUL_ASSIGN || t.kind == TOK_DIV_ASSIGN || t.kind == TOK_MOD_ASSIGN ||
        t.kind == TOK_AND_ASSIGN || t.kind == TOK_OR_ASSIGN || t.kind == TOK_XOR_ASSIGN ||
        t.kind == TOK_SHL_ASSIGN || t.kind == TOK_SHR_ASSIGN) {
        int op = (int)t.kind;
        AstNode *right;
        lexer_advance(p->lex);
        right = parse_assign_expr(p);
        return anchor(ast_assign(op, left, right, t.line),
                      left && left->col > 0 ? left->col : t.col);
    }
    return left;
}

static AstNode *parse_cond_expr(Parser *p) {
    AstNode *cond = parse_or_expr(p);
    if (lexer_peek(p->lex).kind == TOK_QUESTION) {
        int line = lexer_peek(p->lex).line;
        AstNode *te, *fe;
        AstNode *n;
        lexer_advance(p->lex);
        te = parse_expr(p);
        lexer_expect(p->lex, TOK_COLON);
        fe = parse_cond_expr(p);
        n = ast_new(AST_TERNARY, line);
        n->u.ternary.cond = cond;
        n->u.ternary.true_expr = te;
        n->u.ternary.false_expr = fe;
        return n;
    }
    return cond;
}

#define DEFINE_BINOP_LAYER(fn, next, cond_expr) \
static AstNode *fn(Parser *p) {                                  \
    AstNode *left = next(p);                                      \
    while (1) {                                                   \
        TokenKind k = lexer_peek(p->lex).kind;                    \
        if (cond_expr) {                                          \
            Token op = lexer_peek(p->lex);                        \
            AstNode *right;                                       \
            lexer_advance(p->lex);                                \
            right = next(p);                                      \
            left = anchor(ast_binary((int)k, left, right, op.line), \
                          left && left->col > 0 ? left->col : op.col); \
        } else break;                                             \
    }                                                             \
    return left;                                                  \
}

DEFINE_BINOP_LAYER(parse_or_expr,     parse_and_expr,    (k == TOK_OR))
DEFINE_BINOP_LAYER(parse_and_expr,    parse_bitor_expr,  (k == TOK_AND))
DEFINE_BINOP_LAYER(parse_bitor_expr,  parse_bitxor_expr, (k == TOK_BOR))
DEFINE_BINOP_LAYER(parse_bitxor_expr, parse_bitand_expr, (k == TOK_BXOR))
DEFINE_BINOP_LAYER(parse_bitand_expr, parse_eq_expr,     (k == TOK_BAND))
DEFINE_BINOP_LAYER(parse_eq_expr,     parse_rel_expr,    (k == TOK_EQ || k == TOK_NE))
DEFINE_BINOP_LAYER(parse_rel_expr,    parse_shift_expr,  (k == TOK_LT || k == TOK_GT || k == TOK_LE || k == TOK_GE))
DEFINE_BINOP_LAYER(parse_shift_expr,  parse_add_expr,    (k == TOK_SHL || k == TOK_SHR))
DEFINE_BINOP_LAYER(parse_add_expr,    parse_mul_expr,    (k == TOK_PLUS || k == TOK_MINUS))
DEFINE_BINOP_LAYER(parse_mul_expr,    parse_cast_expr,   (k == TOK_MUL || k == TOK_DIV || k == TOK_MOD))

/* Parse a type name (declspec + declarator) and hand back the type.
 * Callers decide what to do with it: `sizeof` needs the size, but a
 * function type has no size at all (C99 6.5.3.4p1 makes sizeof on a
 * function type a constraint violation), and the old helper returned the
 * generic 4 for TYPE_FUNC -- a silently wrong answer. */
static Type *type_name_of_token(Parser *p) {
    int sto = 0, td = 0;
    Type *base = parse_declspec(p, &sto, &td);
    char *nm = 0;
    if (!base) return 0;
    return parse_declarator(p, base, &nm);
}

static AstNode *parse_cast_expr(Parser *p) {
    /* Check if it is ( type-name ) expr */
    if (lexer_peek(p->lex).kind == TOK_LPAREN) {
        Token t2 = lexer_peek2(p->lex);
        if (is_basic_type_token(t2.kind) ||
            (t2.kind == TOK_IDENT && sym_is_typedef(t2.str))) {
            int sto = 0, td = 0;
            int line = lexer_peek(p->lex).line;
            Type *base;
            Type *t;
            char *nm = 0;
            AstNode *operand;
            lexer_advance(p->lex); /* '(' */
            base = parse_declspec(p, &sto, &td);
            t = parse_declarator(p, base, &nm);
            lexer_expect(p->lex, TOK_RPAREN);
            /* C99 compound literal: (type){ ... } -- treat as anonymous aggregate and take address */
            if (lexer_peek(p->lex).kind == TOK_LBRACE) {
                AstNode *init = parse_init_list(p);
                AstNode *n = ast_new(AST_COMPOUND_LITERAL, line);
                n->type = t;
                n->u.compound_lit.lit_type = t;
                n->u.compound_lit.init = init;
                return n;
            }
            operand = parse_cast_expr(p);
            return ast_cast(t, operand, line);
        }
    }
    return parse_unary_expr(p);
}

static AstNode *parse_unary_expr(Parser *p) {
    Token t = lexer_peek(p->lex);
    int line = t.line;
    switch (t.kind) {
        case TOK_PLUS:
            lexer_advance(p->lex);
            return parse_cast_expr(p);
        case TOK_MINUS:
        case TOK_NOT:
        case TOK_BNOT:
        case TOK_INC:
        case TOK_DEC: {
            int op = (int)t.kind;
            lexer_advance(p->lex);
            return anchor(ast_unary(op, parse_cast_expr(p), line), t.col);
        }
        case TOK_MUL:
            lexer_advance(p->lex);
            return anchor(ast_unary((int)TOK_MUL, parse_cast_expr(p), line), t.col);
        case TOK_BAND:
            lexer_advance(p->lex);
            return anchor(ast_unary((int)TOK_BAND, parse_cast_expr(p), line), t.col);
        case TOK_SIZEOF: {
            int sz;
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind == TOK_LPAREN) {
                Token t2 = lexer_peek2(p->lex);
                if (is_basic_type_token(t2.kind) ||
                    (t2.kind == TOK_IDENT && sym_is_typedef(t2.str))) {
                    lexer_advance(p->lex); /* '(' */
                    {
                        Type *st = type_name_of_token(p);
                        if (st && st->kind == TYPE_FUNC) {
                            /* C99 6.5.3.4p1: sizeof shall not be applied to
                             * a function type.  Reporting beats folding to
                             * the generic 4 (which is what `sizeof(binop)`
                             * silently returned for a -function-type
                             * typedef). */
                            perror_at(p, "invalid application of 'sizeof' to a function type");
                            sz = 4;
                        } else {
                            sz = st ? type_size(st) : 4;
                        }
                    }
                    lexer_expect(p->lex, TOK_RPAREN);
                    return ast_sizeof_val(sz, line);
                }
            }
            /* sizeof expr where expr is a flexible array member type;
             * for AST_VAR, the type may be NULL (sema fills it later);
             * for array types like char[64], size is 64, not multiplied by element size 4. */
            {
                AstNode *e = parse_unary_expr(p);
                int s = 4;
                if (e && e->kind == AST_STR) {
                    /* C11 6.5.3.4: a string literal has type char[n+1], so
                     * sizeof "abc" is 4 and sizeof("long...") is its
                     * length + 1 -- not the pointer size.  ast_str() gives
                     * the node a char* type, so this has to be checked
                     * BEFORE the generic type path below; otherwise the
                     * literal folds to 4 and
                     *     sizeof("written by arkcc ...") - 1
                     * becomes 3 (three bytes of garbage on the wire).
                     * u.name holds the escapes-resolved content. */
                    int sl = 0;
                    while (e->u.name && e->u.name[sl]) sl++;
                    s = sl + 1;
                } else if (e && e->type) {
                    s = type_size(e->type);
                } else if (e && e->kind == AST_VAR) {
                    Symbol *sym = sym_find(e->u.name);
                    if (sym && sym->type) s = type_size(sym->type);
                } else if (e && e->kind == AST_FUNC_CALL &&
                           e->u.call.name && e->u.call.name[0] != '?') {
                    /* sizeof(call): the parser runs before sema, so the call
                     * node carries no type yet. Resolve the callee in the
                     * symbol table and take its return type -- otherwise the
                     * default 4 was reported for every call, e.g.
                     * sizeof(gd()) with `double gd(void)` yielded 4 not 8. */
                    Symbol *fsym = sym_find(e->u.call.name);
                    if (fsym && fsym->type) {
                        Type *ftype = fsym->type;
                        if (fsym->kind == SYM_FUNC) {
                            /* SYM_FUNC stores the return type directly. */
                            s = type_size(ftype);
                        } else {
                            if (ftype->kind == TYPE_PTR && ftype->base) ftype = ftype->base;
                            if (ftype->kind == TYPE_FUNC && ftype->base)
                                s = type_size(ftype->base);
                        }
                    }
                } else if (e && e->kind == AST_ARRAY_ACCESS) {
                    /* sizeof(arr[i]): resolve arr's type from symtab, then
                     * take the element type. Without this, sizeof returns
                     * the default 4 (breaks sizeof(g_cpu_table)/sizeof(g_cpu_table[0])
                     * array-count idiom in cpu.c). */
                    AstNode *arr = e->u.array_access.array;
                    if (arr && arr->kind == AST_VAR) {
                        Symbol *sym = sym_find(arr->u.name);
                        if (sym && sym->type && sym->type->kind == TYPE_ARRAY) {
                            s = type_size(sym->type->base);
                        }
                    }
                }
                if (s <= 0) s = 4;
                return ast_sizeof_val(s, line);
            }
        }
        case TOK_ALIGNOF: {
            /* C11 6.5.3.4: _Alignof ( type-name ).  The operand is a type
             * name and never an expression; guessing an expression's type at
             * parse time would be a fabricated answer, so anything that is
             * not a type name is reported. */
            int al;
            lexer_advance(p->lex);          /* _Alignof */
            if (lexer_peek(p->lex).kind != TOK_LPAREN) {
                perror_at(p, "expected '(' after _Alignof");
                return ast_sizeof_val(4, line);
            }
            {
                Token t2 = lexer_peek2(p->lex);
                if (!(is_basic_type_token(t2.kind) ||
                      (t2.kind == TOK_IDENT && sym_is_typedef(t2.str)))) {
                    perror_at(p, "_Alignof requires a type name");
                    lexer_advance(p->lex);  /* '(' */
                    return ast_sizeof_val(4, line);
                }
            }
            lexer_advance(p->lex);          /* '(' */
            {
                int sto = 0, td = 0;
                char *nm = 0;
                Type *base = parse_declspec(p, &sto, &td);
                Type *t = base ? parse_declarator(p, base, &nm) : 0;
                (void)nm;
                /* C11 6.5.3.4p1: _Alignof shall not be applied to a function
                 * type.  A function-type typedef (`typedef int binop(int);`)
                 * would otherwise answer the generic 4. */
                if (t && t->kind == TYPE_FUNC) {
                    perror_at(p, "invalid application of '_Alignof' to a function type");
                    al = 1;
                } else {
                    al = type_align(t);
                }
            }
            lexer_expect(p->lex, TOK_RPAREN);
            if (al <= 0) al = 1;
            return ast_sizeof_val(al, line);
        }
        default:
            return parse_postfix_expr(p);
    }
}

static AstNode *parse_postfix_expr(Parser *p) {
    AstNode *node = parse_primary_expr(p);
    for (;;) {
        Token t = lexer_peek(p->lex);
        int line = t.line;
        if (t.kind == TOK_LBRACKET) {
            AstNode *idx;
            lexer_advance(p->lex);
            idx = parse_expr(p);
            lexer_expect(p->lex, TOK_RBRACKET);
            node = anchor(ast_array_access(node, idx, line),
                          node->col > 0 ? node->col : t.col);
        } else if (t.kind == TOK_LPAREN) {
            AstNode **args = 0;
            int arg_count = 0;
            int arg_capacity = 0;
            char *callee_name;
            lexer_advance(p->lex);
            if (lexer_peek(p->lex).kind != TOK_RPAREN) {
                for (;;) {
                    AstNode *a = parse_assign_expr(p);
                    if (arg_count >= arg_capacity) {
                        int nc = arg_capacity ? arg_capacity * 2 : 4;
                        args = (AstNode **)mem_realloc(args,
                            arg_capacity * (int)sizeof(AstNode *),
                            nc * (int)sizeof(AstNode *));
                        arg_capacity = nc;
                    }
                    args[arg_count++] = a;
                    if (lexer_peek(p->lex).kind == TOK_COMMA) {
                        lexer_advance(p->lex);
                    } else break;
                }
            }
            {
                /* armcc anchors "too few arguments" (#165) on the ')' that
                 * closes the argument list -- measured with `g()`, `g(1)` and
                 * `g( 1 , 2 )` -- so the call node carries that column. #140
                 * is moved onto the first surplus argument in sema.c. */
                Token rp = lexer_peek(p->lex);
                int call_col = rp.col > 0 ? rp.col : t.col;
                lexer_expect(p->lex, TOK_RPAREN);
                if (node->kind != AST_VAR) {
                    /* Function pointer call: set callee_name to placeholder and
                     * save the callee expression (function pointer) for irgen. */
                    AstNode *callee = node;
                    callee_name = mem_strdup("?indirect");
                    node = ast_func_call(callee_name, args, arg_count, line);
                    node->u.call.callee = callee;
                } else {
                    callee_name = node->u.name;
                    node = ast_func_call(callee_name, args, arg_count, line);
                }
                node = anchor(node, call_col);
            }
        } else if (t.kind == TOK_DOT || t.kind == TOK_ARROW) {
            int is_arrow = (t.kind == TOK_ARROW);
            Token mt;
            lexer_advance(p->lex);
            mt = lexer_peek(p->lex);
            if (mt.kind != TOK_IDENT) {
                perror_at(p, "expected member name");
                return node;
            }
            lexer_advance(p->lex);
            node = anchor(ast_member(node, mt.str, is_arrow, line),
                          node->col > 0 ? node->col : t.col);
        } else if (t.kind == TOK_INC || t.kind == TOK_DEC) {
            AstNode *u;
            lexer_advance(p->lex);
            u = anchor(ast_unary((int)t.kind, node, line),
                       node->col > 0 ? node->col : t.col);
            u->u.unary.is_postfix = 1;
            node = u;
        } else break;
    }
    return node;
}

static AstNode *parse_primary_expr(Parser *p) {
    Token t = lexer_peek(p->lex);
    int line = t.line;
    if (t.kind == TOK_GENERIC) {
        /* C11 6.5.1.1:
         *   _Generic ( assignment-expression , generic-association-list )
         * The controlling expression's type is not known until it has been
         * typed, and typing happens in sema, so the node is built here and
         * resolved there.  Guessing a type at parse time would be a fabricated
         * answer -- the exact failure mode this project forbids. */
        AstNode *n = anchor(ast_new(AST_GENERIC, line), t.col);
        n->u.generic.ctrl = 0;
        n->u.generic.default_expr = 0;
        n->u.generic.types = 0;
        n->u.generic.exprs = 0;
        n->u.generic.count = 0;
        n->u.generic.cap = 0;
        lexer_advance(p->lex);              /* _Generic */
        if (lexer_peek(p->lex).kind != TOK_LPAREN) {
            perror_at(p, "expected '(' after _Generic");
            return n;
        }
        lexer_advance(p->lex);              /* '(' */
        n->u.generic.ctrl = parse_assign_expr(p);
        while (lexer_peek(p->lex).kind == TOK_COMMA) {
            AstNode *e;
            lexer_advance(p->lex);          /* ',' */
            if (lexer_peek(p->lex).kind == TOK_DEFAULT) {
                lexer_advance(p->lex);      /* default */
                lexer_expect(p->lex, TOK_COLON);
                e = parse_assign_expr(p);
                if (n->u.generic.default_expr)
                    perror_at(p, "more than one default association in _Generic");
                n->u.generic.default_expr = e;
                continue;
            }
            {
                int sto = 0, td = 0;
                char *nm = 0;
                Type *base = parse_declspec(p, &sto, &td);
                Type *at = base ? parse_declarator(p, base, &nm) : 0;
                (void)nm;
                if (!at) {
                    perror_at(p, "expected a type name in _Generic association");
                    return n;
                }
                lexer_expect(p->lex, TOK_COLON);
                e = parse_assign_expr(p);
                if (n->u.generic.count >= n->u.generic.cap) {
                    int nc = n->u.generic.cap ? n->u.generic.cap * 2 : 4;
                    n->u.generic.types = (Type **)mem_realloc(
                        n->u.generic.types,
                        n->u.generic.cap * (int)sizeof(Type *),
                        nc * (int)sizeof(Type *));
                    n->u.generic.exprs = (AstNode **)mem_realloc(
                        n->u.generic.exprs,
                        n->u.generic.cap * (int)sizeof(AstNode *),
                        nc * (int)sizeof(AstNode *));
                    n->u.generic.cap = nc;
                }
                n->u.generic.types[n->u.generic.count] = at;
                n->u.generic.exprs[n->u.generic.count] = e;
                n->u.generic.count++;
            }
        }
        lexer_expect(p->lex, TOK_RPAREN);
        if (!n->u.generic.default_expr && n->u.generic.count == 0)
            perror_at(p, "_Generic requires at least one association");
        return n;
    }
    if (t.kind == TOK_FCONST) {
        /* Floating-point literal: reuse AST_NUM node, with new field fval and type=double */
        AstNode *node;
        lexer_advance(p->lex);
        node = anchor(ast_new(AST_NUM, line), t.col);
        node->u.num_val = (int)t.fvalue;
        node->fval = t.fvalue;
        if (t.is_imag) {
            /* C99 imaginary literal: 1.0i or (0+1i), type=_Complex double;
             * num_val=-1 as marker; irgen emits (0, fval) complex number. */
            node->type = type_complex(type_double());
            node->u.num_val = -1;
        } else if (t.is_float) {
            /* C99 float suffix (f/F): 10.0f has type float (not double). */
            node->type = type_float();
        } else {
            node->type = type_double();
        }
        return node;
    }
    if (t.kind == TOK_NUM) {
        AstNode *n;
        lexer_advance(p->lex);
        n = anchor(ast_num(t.value, line), t.col);
        /* C99 6.4.4.1: an integer literal's type follows its suffix. Without
         * this, `-1 < 1u` compared as signed and 0xFFFFFFFFu / 1u<<31 got
         * the signed int type. */
        /* FIX(long-literal): the `L`/`UL` suffix must map to long /
         * unsigned long, not to long long.  long is 32-bit on every
         * target we model, so mapping UL to ullong forced 64-bit
         * arithmetic for constants such as 0xE000E000UL; the 64-bit
         * path then produced the wrong low word (SCB 0xE000ED24 came
         * out as 0x24, i.e. a wild store -> IMPRECISERR HardFault). */
        if (t.lit_llong) {
            if (t.lit_unsigned) n->type = type_ullong();
            else                n->type = type_llong();
        } else if (t.lit_long) {
            if (t.lit_unsigned) n->type = type_ulong();
            else                n->type = type_long();
        } else if (t.lit_unsigned) {
            n->type = type_uint();
        }
        return n;
    }
    if (t.kind == TOK_STR) {
        AstNode *sn;
        int is_wide = (t.value == 1 && g_config.enable_wchar);
        int cap = 64, len = 0;
        char *concat = 0;
        int first = 1;
        /* C99 5.1.1.2 translation phase 6: adjacent string literal concatenation.
         * The lexer already merges consecutive TOK_STR tokens into one;
         * here we handle string literal concatenation in the parser. */
        for (;;) {
            Token s = lexer_peek(p->lex);
            if (s.kind != TOK_STR) break;
            lexer_advance(p->lex);
            if (s.value == 1 && g_config.enable_wchar) is_wide = 1;
            {
                const char *text = s.str ? s.str : "";
                int tl = ark_strlen(text);
                if (first) {
                    concat = (char *)mem_pool_alloc(cap);
                    first = 0;
                }
                while (len + tl + 1 >= cap) {
                    int nc = cap * 2;
                    char *nb = (char *)mem_pool_alloc(nc);
                    int i;
                    for (i = 0; i < len; i++) nb[i] = concat[i];
                    concat = nb;
                    cap = nc;
                }
                {
                    int i;
                    for (i = 0; i < tl; i++) concat[len++] = text[i];
                }
            }
        }
        if (first) {
            /* Direct use of STR: already a string literal */
            concat = (char *)mem_pool_alloc(1);
            concat[0] = 0;
        }
        concat[len] = 0;
        sn = anchor(ast_str(concat, line), t.col);
#if ARKCC_FEATURE_WCHAR
        if (is_wide) {
            sn->type = type_pointer(type_wchar());
        }
#endif
        return sn;
    }
    if (t.kind == TOK_IDENT) {
        /* __func__ handling: return current function name as string literal */
        if (ark_streq(t.str, "__func__") || ark_streq(t.str, "__FUNCTION__") ||
            ark_streq(t.str, "__PRETTY_FUNCTION__")) {
            lexer_advance(p->lex);
            return ast_str(g_current_func_name ? g_current_func_name : "?", line);
        }
        /* Enum constant directly expands to literal */
        Symbol *s = sym_find(t.str);
        if (s && s->kind == SYM_ENUM_CONST) {
            lexer_advance(p->lex);
            return ast_num(s->enum_value, line);
        }
        lexer_advance(p->lex);
        {
            /* Keep the token column: semantic diagnostics are anchored to the
             * identifier itself, and the caret has to land under it. */
            AstNode *id = ast_var(t.str, line);
            id->col = t.col;
            return id;
        }
    }
    if (t.kind == TOK_LPAREN) {
        AstNode *e;
        lexer_advance(p->lex);
        e = parse_expr(p);
        lexer_expect(p->lex, TOK_RPAREN);
        /* The visible start of a parenthesised expression is its '(', so the
         * caret is moved there even when the inner node already has a column
         * (armcc does the same for `(a+1) = 2` -> #137 under the '('). */
        if (e) e->col = t.col;
        return e;
    }
    /* A delimiter where an expression was required is armcc #29, measured on
     * `int a = ;`. The delimiter is deliberately left in the stream so the
     * enclosing parser consumes it the way it normally would: that is why
     * armcc reports one error here instead of a cascade of follow-ons. */
    if (t.kind == TOK_SEMICOLON || t.kind == TOK_RPAREN || t.kind == TOK_COMMA ||
        t.kind == TOK_RBRACKET || t.kind == TOK_RBRACE) {
        DiagLoc loc;
        loc.file = 0;
        loc.line = t.line;
        loc.col  = t.col;
        diag_report(29, DIAG_SEV_ERROR, &loc, 0, 0, 0);
        return ast_num(0, line);
    }
    perror_at(p, "unexpected token in expression");
    lexer_advance(p->lex);
    return ast_num(0, line);
}
