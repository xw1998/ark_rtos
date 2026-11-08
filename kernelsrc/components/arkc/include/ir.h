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

/* Bit 0x800 in aux marks C99 _Complex ("this operand is a complex value").
 * It is set on IR_PUSH_FLOAT, IR_F2I, IR_I2F, IR_UNARY, IR_FADD/IR_FSUB/
 * IR_FMUL/IR_FDIV and IR_FCMP.  It used to be spelled with 0x400, which those
 * very instructions already use for "unsigned operand": `(double)(unsigned)v`
 * and `creal(z)` were literally the same instruction, so the C backend answered
 * every unsigned<->float conversion with creal() and the x86 backend narrowed
 * the imaginary part instead of converting -- silent wrong answers in both.
 * The complex bits are now disjoint from the arithmetic ones.  The operator
 * itself stays in the low bits (every TokenKind is < 0x100), and 0x100 is the
 * second flag, so the encoding is:
 *
 *   IR_PUSH_FLOAT  aux = 0x800 | 0x200               val = complex pool index
 *   IR_F2I         aux = 0x800                       creal  (complex -> double)
 *                  aux = 0x800 | 0x100               cimag  (complex -> double)
 *                  aux = 0x800 | 0x200               cabs   (needs libm)
 *                  aux = 0x800 | 0x300               carg   (needs libm)
 *   IR_I2F         aux = 0x800                       float  -> complex
 *                  aux = 0x800 | 0x200               double -> complex
 *                  aux = 0x800 | 0x100               int    -> complex
 *                        (| 0x400 when the int is unsigned)
 *   IR_UNARY       aux = TOK_BNOT | 0x800            conj
 *   IR_FADD        aux = TOK_PLUS | 0x900            cexp   (needs libm)
 *   IR_FADD/FSUB/FMUL/FDIV  aux = TOK_xxx | 0x800    complex arithmetic
 *   IR_FCMP        aux = TOK_EQ|0x800 or TOK_NE|0x800
 *
 * A complex value is four stack words on the machine backends (re_lo, re_hi,
 * im_lo, im_hi) and one 8-byte slot in the C backend.
 */
#define IR_AUX_COMPLEX 0x800

typedef struct IrInst {
    IrOp  op;
    long long val;   /* immediate / offset / label id */
    const char *name;  /* label name / function name / string ref */
    int   aux;   /* aux value: BINARY operator / flags */
    int   line;  /* source line (for .loc debug info; 0 = unknown) */
    /* IR_CALL only: 1 + the index into g_ir_argbytes[] of this call's
     * per-argument wire sizes in bytes, or 0 when every argument is 4 or 8
     * bytes -- the only two sizes the 2-bit-per-argument argmask can state.
     * A variadic function's caller cannot hand a >8-byte composite over as a
     * pointer the way a *named* parameter does: the callee has no type for it
     * and <stdarg.h> walks consecutive words, so the word count has to travel
     * outside argmask.  Everything else about the instruction is unchanged,
     * and backends that do not read it are unaffected (they refuse the case
     * before code generation, see g_ir_vararg_words). */
    int   aux2;
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

/* One address element inside an aggregate initialiser:
 *   int (*tab[2])(int,int) = { add, sub };
 *   struct S s = { 1, &gv };
 * The bytes of the object still come from init_aggregate (ark_agg_fill keeps
 * zeros where an address sits); each entry here says "the 4 bytes at `offset`
 * are the address of `sym` + `add`", which a machine backend turns into a
 * symbol reference instead of a baked number. */
typedef struct IrSymRef {
    int       offset;   /* byte offset inside the object */
    char     *sym;      /* symbol named by the initialiser (0 for a string) */
    long long add;      /* byte offset inside that symbol */
    int       is_func;  /* 1 when sym is a function */
    /* `char *cs[2] = { "ab", "cd" };` -- the element is the address of a string
     * literal, which has no name yet: each backend pools it in its own read-only
     * section and labels it, so the address is `sym` when `str` is 0 and the
     * pool label when `str` is not. */
    char     *str;
} IrSymRef;

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
    /* Address constant initialiser (C11 6.6p9): the address of a symbol plus a
     * byte offset.  init_sym names the symbol -- a function, or an object with
     * static storage duration -- and init_sym_off is added to that address.
     *
     * It is kept apart from init_int/init_aggregate because a machine backend
     * cannot bake it as a byte pattern: the address of a function is not known
     * until the linker has placed it, so the store has to carry a symbol
     * reference (a label plus a relocation).  init_aggregate is still filled
     * with the source text so the C backend round-trips the declaration.
     */
    char   *init_sym;
    long long init_sym_off;
    /* 1 when init_sym names a function (its address is a code label, which an
     * object can carry as a relocation), 0 when it names an object.  The
     * distinction matters to a backend that cannot name another unit's data
     * but can name its code. */
    int     init_sym_is_func;
    /* Address elements inside an aggregate initialiser (see IrSymRef).  Only
     * an initialiser that spells out every brace level is walked, because a
     * brace-elided initialiser cannot say where an element starts without
     * re-deriving the layout -- and a wrong offset here stores an address at
     * the wrong field instead of failing. */
    IrSymRef *init_refs;
    int     init_ref_count;
    /* Floating point initializer */
    double  init_float;
    int     is_float;
    /* _Complex initialiser: init_float carries the real part and
     * init_float_im the imaginary part; is_float is set as well, so every
     * backend that only understands a real scalar must test init_is_complex
     * first or it will store half a value with the wrong width (that is how
     * `double _Complex g = 3.0;` used to come out as (float)3.0 in one word). */
    int     init_is_complex;
    double  init_float_im;
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
    /* Per-call argument sizes for IrInst.aux2 (see ir_program_add_argbytes).
     * Owned by the program so it is released by the same pool reset as the IR
     * that indexes it. */
    int *argbytes;
    int  argbytes_count;
    int  argbytes_cap;
} IrProgram;

IrFunc    *ir_func_new(const char *name);
void       ir_emit(IrFunc *f, IrOp op, long long val, const char *name, int aux);

/* 1 (default): a variadic function gets the C backend's 8 x 8-byte argument
 * slot region at frame+8000, which the prologue must reserve.  Set to 0 by the
 * driver for the backends that build the argument image from the caller's own
 * words (ARM Thumb-2, RV32I) -- see <stdarg.h> and g_ir_va_slot_region in
 * irgen.c. */
extern int g_ir_va_slot_region;

/* IR_CALL argument sizes, in bytes, for the calls that need a size the
 * argmask cannot express.  irgen appends one run per call to the *program's*
 * table (IrProgram.argbytes) and stamps the run's index (1-based) into
 * IrInst.aux2; the run is exactly as long as the call's argument count
 * (IrInst.val & 0xFFFFFFFF).  Entries are the *wire* sizes: 4 for a word, 8 for
 * a double word (even register pair / 8-byte stack slot), and for a composite
 * passed through `...` the struct's size rounded up to a multiple of 4 -- one
 * argument word per 4 bytes, laid out consecutively.
 *
 * The table lives in the program, not in a module global, because the driver
 * resets the memory pool between translation units (compiler.c calls
 * mem_pool_reset() per compile).  A global pointer into that pool went stale on
 * the board's `cc --pack` -- three sources compiled in one process -- and the
 * backend then read garbage argument sizes ("passes 900035835 argument
 * words"). */
int ir_program_add_argbytes(IrProgram *prog, const int *vals, int n);

/* 1: this target can pass a composite larger than two words through `...`
 * (ARM Thumb-2 only, whose va region is the caller's own argument words).
 * 0 (default): irgen rejects the construct with an error instead of emitting
 * code that would read the wrong words. */
extern int g_ir_vararg_words;

IrProgram *ir_program_new(void);
void       ir_program_add(IrProgram *prog, IrFunc *func);
int        ir_program_add_string(IrProgram *prog, const char *data, int len);
int        ir_program_add_float(IrProgram *prog, double v);
int        ir_program_add_complex(IrProgram *prog, double re, double im);
IrGlobal  *ir_program_add_global(IrProgram *prog, const char *name, Type *t);
IrGlobal  *ir_program_find_global(IrProgram *prog, const char *name);
const char *ir_op_name(IrOp op);

#endif /* ARK_IR_H */
