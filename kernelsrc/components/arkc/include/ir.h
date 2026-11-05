/* Ark C Compiler - ir.h
 * IR definition.
 */
#ifndef ARK_IR_H
#define ARK_IR_H

#include "common.h"
#include "type.h"

/* #11 FIX: Volatile qualifier support.
 * Bit 0x10000 in aux marks volatile memory accesses (IR_PUSH_VAR,
 * IR_PUSH_GLOBAL, IR_STORE, IR_STORE_GLOBAL). Optimization passes must
 * not eliminate, fold, or combine these accesses. */
#define IR_AUX_VOLATILE 0x10000

typedef struct IrInst {
    IrOp  op;
    long long val;   /* immediate / offset / label id */
    const char *name;  /* label name / function name / string ref */
    int   aux;   /* aux value: BINARY operator / flags */
    int   line;  /* source line (for .loc debug info; 0 = unknown) */
} IrInst;

/* irgen sets this to the current source line; ir_emit stamps it onto IrInst.line,
 * so every instruction produced for one AST node carries that line (0 = unknown). */
extern int g_ir_cur_line;

typedef struct IrFunc {
    char    *name;
    IrInst  *code;
    int      code_count;
    int      code_capacity;
    int      local_size;
    int      label_count;
    int      param_count;
    int      is_variadic;
    int      is_static;
    int      is_irq;       /* __irq function */
    int      is_weak;      /* __weak function */
    int      is_naked;     /* __naked function */
    int      is_used;      /* __attribute__((used)) */
    int      is_unused;    /* __attribute__((unused)) */
    int      is_asm_func;  /* __asm function body */
    int      forceinline;  /* __forceinline */
    int      is_constructor; /* __attribute__((constructor)) */
    int      is_destructor;  /* __attribute__((destructor)) */
    int      is_deprecated;  /* __attribute__((deprecated)) */
    char    *alias_target; /* __attribute__((alias("xxx"))) */
    char    *asm_body;     /* __asm function body text */
    /* 0.6.x: ARM Compiler 5 attributes carried over for compatibility */
    int      is_value_in_regs; /* __value_in_regs */
    int      is_pure;          /* __pure / __attribute__((pure)) */
    int      is_const_attr;    /* __attribute__((const)) */
    int      is_interwork;     /* __interwork (ARM/Thumb interworking) */
    int      smc_num;          /* __smc(n) number; -1 = not an SMC */
    int      global_reg;       /* __global_reg(n) number; -1 = not set */
    int      param_names_count;
    char   **param_names;
    Type   **param_types;
    Type    *ret_type;
} IrFunc;

typedef struct IrString {
    int    id;
    char  *data;
    int    len;
    int    is_wide;  /* B2: wide string literal L"..." */
} IrString;

typedef struct IrGlobal {
    char  *name;
    Type  *type;
    int    is_static;
    int    is_extern;
    /* Which initializer form is present: init_int, init_str or
     * init_aggregate. Exactly one is used per definition; sema and irgen both
     * rely on this to emit the right storage. */
    int     has_init;
    /* #3 FIX: init_int changed from int to long long to support 64-bit
     * global initializers (long long, double bit patterns). */
    long long init_int;
    char   *init_str;
    /* Aggregate initializer as C source text: "..." including the braces */
    char   *init_aggregate;
    /* Floating point initializer */
    double  init_float;
    int     is_float;
    /* Target section name from __attribute__((section(...))) */
    char   *section;
    /* Declared const: placed in .rodata */
    int     is_const;
    /* __at(addr): the fixed address; has_at=1 when present */
    unsigned at_addr;
    int     has_at;
    /* Required alignment in bytes; 0 = default */
    int     align_bytes;
    /* __attribute__((used/unused)) */
    int     is_used;
    int     is_unused;
    /* 0.6.x: TLS and register-variable attributes */
    int     is_tls;       /* __thread storage class */
    int     global_reg;   /* __global_reg(n) number; -1 = not set */
} IrGlobal;

typedef struct { double re, im; } IrComplexVal;

typedef struct IrProgram {
    IrFunc   **funcs;
    int        func_count;
    int        func_capacity;
    IrString **strings;
    int        string_count;
    int        string_capacity;
    IrGlobal **globals;
    int        global_count;
    int        global_capacity;
    /* Floating point constant pool */
    double    *floats;
    int        float_count;
    int        float_capacity;
    /* C99 _Complex double constant pool (real + imaginary) */
    IrComplexVal *complexes;
    int        complex_count;
    int        complex_capacity;
    /* Current source file for DWARF .file/.loc; NULL = unknown */
    const char *source_file;
} IrProgram;

IrFunc    *ir_func_new(const char *name);
void       ir_emit(IrFunc *f, IrOp op, long long val, const char *name, int aux);
IrProgram *ir_program_new(void);
void       ir_program_add(IrProgram *prog, IrFunc *func);
int        ir_program_add_string(IrProgram *prog, const char *data, int len);
int        ir_program_add_float(IrProgram *prog, double v);
int        ir_program_add_complex(IrProgram *prog, double re, double im);
IrGlobal  *ir_program_add_global(IrProgram *prog, const char *name, Type *t);
IrGlobal  *ir_program_find_global(IrProgram *prog, const char *name);
const char *ir_op_name(IrOp op);

#endif /* ARK_IR_H */
