/* Ark C Compiler - common.h
 * Common types, Token kinds, AST node kinds, IR op enums and error-handling macros.
 *
 * Note: In TokenKind, the numeric values of TOK_PLUS..TOK_SHR_ASSIGN are used by
 * codegen/irgen as operator op-ids. New tokens MUST be appended to the end of the
 * enum; never insert into the middle.
 */
#ifndef ARK_COMMON_H
#define ARK_COMMON_H

#include <stddef.h>
#include <stdint.h>

/* Keep in sync with README.md / docs/MANUAL.md, which advertise this
 * version to users; the release notes are organised by this number. */
#define ARK_VERSION "0.8.0"

/* Error codes */
#define ARK_OK              0
#define ARK_ERR_IO          -1
#define ARK_ERR_LEX         -2
#define ARK_ERR_PARSE       -3
#define ARK_ERR_SEMA        -4
#define ARK_ERR_IRGEN       -5
#define ARK_ERR_CODEGEN     -6
#define ARK_ERR_ASM         -7
#define ARK_ERR_LINK        -8
#define ARK_ERR_OOM         -9
#define ARK_ERR_ARG         -10

/* Token kinds */
typedef enum {
    TOK_EOF = 0,

    /* C89 keywords */
    TOK_INT, TOK_CHAR, TOK_VOID, TOK_RETURN,
    TOK_IF, TOK_ELSE, TOK_WHILE, TOK_FOR,
    TOK_BREAK, TOK_CONTINUE, TOK_DO,

    /* Literals and identifiers */
    TOK_NUM,
    TOK_STR,
    TOK_IDENT,

    /* Operators */
    TOK_PLUS, TOK_MINUS, TOK_MUL, TOK_DIV, TOK_MOD,
    TOK_ASSIGN,
    TOK_EQ, TOK_NE,
    TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_AND, TOK_OR, TOK_NOT,
    TOK_BAND, TOK_BOR, TOK_BXOR, TOK_BNOT,
    TOK_SHL, TOK_SHR,

    /* Compound assignment */
    TOK_ADD_ASSIGN, TOK_SUB_ASSIGN,
    TOK_MUL_ASSIGN, TOK_DIV_ASSIGN,
    TOK_MOD_ASSIGN,
    TOK_AND_ASSIGN, TOK_OR_ASSIGN, TOK_XOR_ASSIGN,
    TOK_SHL_ASSIGN, TOK_SHR_ASSIGN,

    /* Increment / decrement */
    TOK_INC, TOK_DEC,

    /* Punctuation / separators */
    TOK_LPAREN, TOK_RPAREN,
    TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMICOLON, TOK_COMMA,
    TOK_COLON, TOK_QUESTION,

    TOK_HASH,

    /* === Below: 0.2.0 additions, MUST stay at end === */
    /* C99 keywords */
    TOK_SHORT, TOK_LONG, TOK_SIGNED, TOK_UNSIGNED,
    TOK_FLOAT, TOK_DOUBLE,
    TOK_BOOL,             /* _Bool */
    TOK_CONST, TOK_VOLATILE, TOK_RESTRICT, TOK_INLINE,
    TOK_STATIC, TOK_EXTERN, TOK_AUTO, TOK_REGISTER,
    TOK_STRUCT, TOK_UNION, TOK_ENUM, TOK_TYPEDEF,
    TOK_SIZEOF,
    TOK_SWITCH, TOK_CASE, TOK_DEFAULT,
    TOK_GOTO,
    TOK_FCONST,           /* float literal (value in t.fvalue) */

    /* Member access */
    TOK_DOT, TOK_ARROW,
    /* Variadic ellipsis */
    TOK_ELLIPSIS,

    /* === 0.3.0 additions (MUST stay at end) === */
    TOK_ATTRIBUTE,        /* __attribute__ */
    TOK_ASM,              /* asm / __asm / __asm__ */
    TOK_FUNC_NAME,        /* __func__ -> lexer emits it as a string literal; enum placeholder kept */
    TOK_SVC,              /* __svc (ARM Compiler 5 SVC declaration) */
    TOK_IRQ,              /* __irq (ARM Compiler 5 interrupt function) */
    TOK_WEAK,             /* __weak (ARM Compiler 5 weak symbol) */
    TOK_NAKED,            /* __naked (ARM Compiler 5 naked function) */
    TOK_PACKED_KW,        /* __packed (ARM Compiler 5 packed struct) */
    TOK_ALIGN_KW,         /* __align(n) (ARM Compiler 5 alignment) */
    TOK_AT,               /* __at(addr) (ARM Compiler 5 absolute address) */
    TOK_SOFTFP,           /* __softfp */
    TOK_FORCEINLINE,      /* __forceinline */
    TOK_ASM_FN,           /* __asm function body (ARM Compiler 5) */

    /* === 0.5.x additions (B4: _Complex keyword) === */
    TOK_COMPLEX,          /* _Complex */

    /* === 0.6.x additions: armcc 5 compatibility keywords (guarded by ARKCC_FEATURE_ARMCC_KEYWORDS) === */
    TOK_VALUE_IN_REGS,    /* __value_in_regs */
    TOK_THREAD,           /* __thread */
    TOK_GLOBAL_REG,       /* __global_reg */
    TOK_PURE,             /* __pure */
    TOK_INTERWORK,        /* __interwork */
    TOK_SMC,              /* __smc */
    TOK_INT64,            /* __int64 */
    TOK_INT32,            /* __int32 */
    TOK_INT16,            /* __int16 */
    TOK_INT8,             /* __int8 */
    TOK_TYPEDEF_KW,       /* __typedef (equivalent to typedef) */
    TOK_ARGS,             /* __args */
    TOK_RETURN_KW,        /* __return */
    TOK_TP_SOFT,          /* __tp_soft */
    TOK_NOTHROW,          /* __nothrow */
    TOK_DEPRECATED_KW,    /* __deprecated (keyword form) */
    TOK_MAY_ALIAS,        /* __may_alias */
    TOK_DECLSPEC,         /* __declspec (MSVC-style attribute) */

    /* === C11 keyword additions (appended, never inserted) === */
    TOK_ALIGNOF,          /* _Alignof */
    TOK_ALIGNAS,          /* _Alignas */
    TOK_STATIC_ASSERT,    /* _Static_assert */
    TOK_GENERIC,          /* _Generic */

    TOK__SENTINEL
} TokenKind;

/* AST node kinds */
typedef enum {
    AST_PROGRAM = 0,
    AST_FUNC_DEF,
    AST_FUNC_DECL,
    AST_VAR_DECL,
    AST_BLOCK,
    AST_RETURN,
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_DO_WHILE,
    AST_BREAK,
    AST_CONTINUE,
    AST_BINARY,
    AST_UNARY,
    AST_TERNARY,
    AST_ASSIGN,
    AST_NUM,
    AST_VAR,
    AST_ARRAY_ACCESS,
    AST_FUNC_CALL,
    AST_EXPR_STMT,

    /* === 0.2.0 additions === */
    AST_STR,           /* string literal; u.name holds content (escapes resolved) */
    AST_CAST,          /* (T)expr; u.unary.operand = expr, node->type is target type */
    AST_MEMBER,        /* expr.member or expr->member */
    AST_SIZEOF,        /* sizeof(T) or sizeof(expr); u.num_val = byte count */
    AST_SWITCH,
    AST_CASE,
    AST_DEFAULT,
    AST_GOTO,
    AST_LABEL,
    AST_COMMA,         /* comma expression */
    AST_TYPEDEF,       /* top-level typedef; no runtime meaning */

    /* === 0.3.0 additions === */
    AST_INIT_LIST,     /* { e1, e2, .field=ev, [i]=ev } */
    AST_INIT_FIELD,    /* .field = expr or [index] = expr initializer item */
    AST_ASM,           /* inline assembly */
    AST_COMPOUND_LITERAL, /* (T){...} */

    /* === 0.4.0 additions === */
    AST_ASM_FUNC,      /* __asm void fn(...) { ... } whole-function-body assembly */

    /* === C11 additions === */
    AST_GENERIC        /* _Generic; resolved (squashed) by sema, never reaches irgen */
} AstNodeKind;

/* IR operations */
typedef enum {
    IR_NOP = 0,
    IR_PUSH_INT,
    IR_PUSH_VAR,
    IR_PUSH_ADDR,
    IR_POP,
    IR_STORE,
    IR_STORE_IDX,
    IR_LOAD_IDX,
    IR_BINARY,
    IR_UNARY,
    IR_CALL,
    IR_RETURN,
    IR_LABEL,
    IR_JMP,
    IR_JZ,
    IR_JNZ,
    IR_FUNC_BEGIN,
    IR_FUNC_END,
    IR_LOCAL_ALLOC,
    IR_ARG,
    IR_GETARG,

    /* === 0.2.0 additions === */
    IR_PUSH_STR,       /* name = string content; val = strpool index */
    IR_PUSH_GLOBAL,    /* name = global variable name */
    IR_STORE_GLOBAL,   /* name = global variable name */
    IR_DUP,            /* duplicate stack top */
    IR_LABEL_NAMED,    /* name = user-defined label */
    IR_JMP_NAMED,      /* name = user-defined label */
    IR_CMP_JMP_EQ,     /* pop a value, compare with immediate val, jump to aux label if equal */

    /* === 0.3.0 additions === */
    IR_PUSH_FLOAT,     /* aux indexes program->floats[]; C backend treats as double */
    IR_FADD,           /* float arithmetic */
    IR_FSUB,
    IR_FMUL,
    IR_FDIV,
    IR_FCMP,           /* aux = TOK_EQ / TOK_NE / TOK_LT ... */
    IR_F2I,            /* float -> int */
    IR_I2F,            /* int -> float */
    IR_F2F,            /* float <-> float cast; aux: 0x200=src double, 0x100=dst double */
    IR_ASM,            /* inline assembly fragment; name = raw template text */
    IR_SVC,            /* ARM SVC instruction; val = SVC number */
    IR_DIV,            /* integer division; aux=0 signed, aux=1 unsigned */
    IR_MOD,            /* modulo; aux=0 signed, aux=1 unsigned */
    IR_64BINARY,       /* 64-bit binary op; aux = TOK_xxx */
    IR_64PUSH,         /* push 64-bit immediate; val=high32, aux=low32 */
    IR_64CMP,          /* 64-bit compare; aux = TOK_xxx */
    IR_64F2I,          /* 64-bit float -> 64-bit int */
    IR_64I2F,          /* 64-bit int -> 64-bit float */
    IR_MEMCPY,         /* memory copy; val=byte count (struct assignment / pass-by-value) */
    IR_STORE_FLD,      /* bitfield store; val=bit offset, aux=bit width */
    IR_LOAD_FLD,       /* bitfield load; val=bit offset, aux=bit width */
    IR_PUSH_GLOBAL_ADDR, /* take address of global; name = global var name (no value load) */
    IR_ALLOCA,         /* B3: VLA dynamic stack alloc; pops byte count, pushes region base */
    IR_SMC,            /* __smc(n) Secure Monitor Call; val=SMC number */

    /* === 0.7.0 additions: SIMD parallel ops (Cortex-M4/M7 DSP instructions) ===
     * Stack semantics same as IR_BINARY: pop right operand r1, left operand r0,
     * perform per-lane parallel op, push result r0 back.
     *   IR_SIMD_ADD16 / SUB16: 2x16-bit signed lanes
     *   IR_SIMD_ADD8  / SUB8  : 4x8-bit signed lanes */
    IR_SIMD_ADD16,
    IR_SIMD_SUB16,
    IR_SIMD_ADD8,
    IR_SIMD_SUB8,

    /* === 0.7.1 additions === */
    IR_SWAP,           /* swap top two stack values: [a, b] -> [b, a] */

    /* === 0.8.0 additions: 64-bit argument passing for cdecl (32-bit x86) ===
     * The internal stack model stores 64-bit values as two 4-byte slots
     * with the HIGH word on top. The cdecl ABI (i386) requires the LOW
     * word at the lower address (on top after push). These instructions
     * convert from the internal model to cdecl order for function call
     * arguments. The C backend treats them as no-ops (its virtual stack
     * uses 8-byte long long slots, so no conversion is needed). */
    IR_SEXT,           /* sign-extend top-of-stack int to long long (cdecl order) */
    IR_ZEXT,           /* zero-extend top-of-stack int to long long (cdecl order) */
    IR_CDECL64,        /* convert top long long from internal model to cdecl order */
    IR_PUSH_FUNC,      /* take the address of a function; name = source function name */
    IR_TRUNC           /* truncate top-of-stack integer to val bits, re-extend:
                        * val = 8/16/32 target width, aux = 1 zero-extend (unsigned) */
} IrOp;

/* Simple string comparison to avoid depending on string.h */
int ark_streq(const char *a, const char *b);
int ark_strlen(const char *s);

#endif /* ARK_COMMON_H */
