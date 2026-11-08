/* Ark C Compiler - ir
 * IR: the intermediate representation.
 */
#include "ir.h"
#include "memory.h"
/* Note: <stdio.h> intentionally NOT included - no stdio functions used in
 * ir.c, and we want to avoid pulling in semihosting on STM32. */

#define IR_INIT_CAP 64

/* Current source line, set by irgen through ir_emit and copied into
 * IrInst.line by ir.c. 0 means "unknown". */
int g_ir_cur_line = 0;

IrFunc *ir_func_new(const char *name) {
    IrFunc *f = (IrFunc *)mem_pool_alloc(sizeof(IrFunc));
    f->name = mem_strdup(name);
    f->code_capacity = IR_INIT_CAP;
    f->code = (IrInst *)mem_pool_alloc(sizeof(IrInst) * f->code_capacity);
    f->code_count = 0;
    f->local_size = 0;
    f->label_count = 0;
    f->param_count = 0;
    f->is_variadic = 0;
    f->is_static = 0;
    f->is_irq = 0;
    f->is_weak = 0;
    f->is_naked = 0;
    f->is_used = 0;
    f->is_unused = 0;
    f->is_asm_func = 0;
    f->forceinline = 0;
    f->is_constructor = 0;
    f->is_destructor = 0;
    f->is_deprecated = 0;
    f->alias_target = 0;
    f->asm_body = 0;
    /* 0.6.x: armcc 5 compatibility attributes carried over */
    f->is_value_in_regs = 0;
    f->is_pure = 0;
    f->is_const_attr = 0;
    f->is_interwork = 0;
    f->smc_num = -1;
    f->global_reg = -1;
    f->param_names = 0;
    f->param_names_count = 0;
    f->param_types = 0;
    f->ret_type = 0;
    return f;
}

void ir_emit(IrFunc *f, IrOp op, long long val, const char *name, int aux) {
    if (f->code_count >= f->code_capacity) {
        int nc = f->code_capacity * 2;
        f->code = (IrInst *)mem_realloc(f->code,
            f->code_capacity * (int)sizeof(IrInst),
            nc * (int)sizeof(IrInst));
        f->code_capacity = nc;
    }
    {
        IrInst *I = &f->code[f->code_count++];
        I->op = op;
        I->val = val;
        I->name = name;
        I->aux = aux;
        I->line = g_ir_cur_line;
        I->aux2 = 0;   /* IR_CALL's side-table index; see include/ir.h */
    }
}

IrProgram *ir_program_new(void) {
    IrProgram *p = (IrProgram *)mem_pool_alloc(sizeof(IrProgram));
    p->funcs = 0;
    p->func_count = 0;
    p->func_capacity = 0;
    p->strings = 0;
    p->string_count = 0;
    p->string_capacity = 0;
    p->globals = 0;
    p->global_count = 0;
    p->global_capacity = 0;
    p->floats = 0;
    p->float_count = 0;
    p->float_capacity = 0;
    p->complexes = 0;
    p->complex_count = 0;
    p->complex_capacity = 0;
    p->source_file = 0;
    p->argbytes = 0;
    p->argbytes_count = 0;
    p->argbytes_cap = 0;
    return p;
}

/* Append one call's argument sizes; the caller stamps 1 + the returned index
 * into IrInst.aux2 (0 means "no table": every argument is 4 or 8 bytes).
 * Returns the start index, and 0 for an empty run -- a 0 index and a 0 aux2
 * would otherwise be the same thing. */
int ir_program_add_argbytes(IrProgram *prog, const int *vals, int n) {
    int start;
    int i;
    if (!prog || !vals || n <= 0) return -1;
    start = prog->argbytes_count;
    if (prog->argbytes_count + n > prog->argbytes_cap) {
        int nc = prog->argbytes_cap ? prog->argbytes_cap : 64;
        while (nc < prog->argbytes_count + n) nc *= 2;
        prog->argbytes = (int *)mem_realloc(prog->argbytes,
                                            prog->argbytes_cap * (int)sizeof(int),
                                            nc * (int)sizeof(int));
        prog->argbytes_cap = nc;
    }
    for (i = 0; i < n; i++) prog->argbytes[start + i] = vals[i];
    prog->argbytes_count += n;
    return start;
}

int ir_program_add_float(IrProgram *prog, double v) {
    int n = prog->float_count;
    if (n >= prog->float_capacity) {
        int nc = prog->float_capacity == 0 ? 8 : prog->float_capacity * 2;
        prog->floats = (double *)mem_realloc(prog->floats,
            prog->float_capacity * (int)sizeof(double),
            nc * (int)sizeof(double));
        prog->float_capacity = nc;
    }
    prog->floats[n] = v;
    prog->float_count = n + 1;
    return n;
}

int ir_program_add_complex(IrProgram *prog, double re, double im) {
    int n = prog->complex_count;
    if (n >= prog->complex_capacity) {
        int nc = prog->complex_capacity == 0 ? 8 : prog->complex_capacity * 2;
        prog->complexes = (IrComplexVal *)mem_realloc(prog->complexes,
            prog->complex_capacity * (int)sizeof(IrComplexVal),
            nc * (int)sizeof(IrComplexVal));
        prog->complex_capacity = nc;
    }
    prog->complexes[n].re = re;
    prog->complexes[n].im = im;
    prog->complex_count = n + 1;
    return n;
}

void ir_program_add(IrProgram *prog, IrFunc *func) {
    int n = prog->func_count;
    if (n >= prog->func_capacity) {
        int nc = prog->func_capacity == 0 ? 8 : prog->func_capacity * 2;
        prog->funcs = (IrFunc **)mem_realloc(prog->funcs,
            prog->func_capacity * (int)sizeof(IrFunc *),
            nc * (int)sizeof(IrFunc *));
        prog->func_capacity = nc;
    }
    prog->funcs[n] = func;
    prog->func_count = n + 1;
}

int ir_program_add_string(IrProgram *prog, const char *data, int len) {
    int n = prog->string_count;
    IrString *s = (IrString *)mem_pool_alloc(sizeof(IrString));
    s->id = n;
    s->len = len;
    s->is_wide = 0;
    s->data = (char *)mem_pool_alloc(len + 1);
    {
        int i;
        for (i = 0; i < len; i++) s->data[i] = data[i];
        s->data[len] = 0;
    }
    if (n >= prog->string_capacity) {
        int nc = prog->string_capacity == 0 ? 8 : prog->string_capacity * 2;
        prog->strings = (IrString **)mem_realloc(prog->strings,
            prog->string_capacity * (int)sizeof(IrString *),
            nc * (int)sizeof(IrString *));
        prog->string_capacity = nc;
    }
    prog->strings[n] = s;
    prog->string_count = n + 1;
    return n;
}

IrGlobal *ir_program_add_global(IrProgram *prog, const char *name, Type *t) {
    int n = prog->global_count;
    IrGlobal *g = (IrGlobal *)mem_pool_alloc(sizeof(IrGlobal));
    g->name = mem_strdup(name);
    g->type = t;
    g->is_static = 0;
    g->is_extern = 0;
    g->has_init = 0;
    g->init_int = 0;
    g->init_str = 0;
    g->init_aggregate = 0;
    g->init_float = 0;
    g->init_is_complex = 0;
    g->init_float_im = 0.0;
    g->is_float = 0;
    g->section = 0;
    g->is_const = 0;
    g->at_addr = 0;
    g->has_at = 0;
    g->align_bytes = 0;
    g->is_used = 0;
    g->is_unused = 0;
    /* 0.6.x: TLS / register-variable attributes */
    g->is_tls = 0;
    g->global_reg = -1;
    if (n >= prog->global_capacity) {
        int nc = prog->global_capacity == 0 ? 8 : prog->global_capacity * 2;
        prog->globals = (IrGlobal **)mem_realloc(prog->globals,
            prog->global_capacity * (int)sizeof(IrGlobal *),
            nc * (int)sizeof(IrGlobal *));
        prog->global_capacity = nc;
    }
    prog->globals[n] = g;
    prog->global_count = n + 1;
    return g;
}

/* Find an existing IrGlobal by name. Returns 0 if not found. */
IrGlobal *ir_program_find_global(IrProgram *prog, const char *name) {
    int i;
    if (!prog || !name) return 0;
    for (i = 0; i < prog->global_count; i++) {
        if (prog->globals[i] && prog->globals[i]->name &&
            ark_streq(prog->globals[i]->name, name)) {
            return prog->globals[i];
        }
    }
    return 0;
}

const char *ir_op_name(IrOp op) {
    switch (op) {
        case IR_PUSH_INT:     return "PUSH_INT";
        case IR_PUSH_VAR:     return "PUSH_VAR";
        case IR_PUSH_ADDR:    return "PUSH_ADDR";
        case IR_POP:          return "POP";
        case IR_STORE:        return "STORE";
        case IR_STORE_IDX:    return "STORE_IDX";
        case IR_LOAD_IDX:     return "LOAD_IDX";
        case IR_BINARY:       return "BINARY";
        case IR_UNARY:        return "UNARY";
        case IR_CALL:         return "CALL";
        case IR_RETURN:       return "RETURN";
        case IR_LABEL:        return "LABEL";
        case IR_JMP:          return "JMP";
        case IR_JZ:           return "JZ";
        case IR_JNZ:          return "JNZ";
        case IR_FUNC_BEGIN:   return "FUNC_BEGIN";
        case IR_FUNC_END:     return "FUNC_END";
        case IR_LOCAL_ALLOC:  return "LOCAL_ALLOC";
        case IR_ARG:          return "ARG";
        case IR_GETARG:       return "GETARG";
        case IR_PUSH_STR:     return "PUSH_STR";
        case IR_PUSH_GLOBAL:  return "PUSH_GLOBAL";
        case IR_STORE_GLOBAL: return "STORE_GLOBAL";
        case IR_DUP:          return "DUP";
        case IR_LABEL_NAMED:  return "LABEL_NAMED";
        case IR_JMP_NAMED:    return "JMP_NAMED";
        case IR_CMP_JMP_EQ:   return "CMP_JMP_EQ";
        case IR_PUSH_FLOAT:   return "PUSH_FLOAT";
        case IR_FADD:         return "FADD";
        case IR_FSUB:         return "FSUB";
        case IR_FMUL:         return "FMUL";
        case IR_FDIV:         return "FDIV";
        case IR_FCMP:         return "FCMP";
        case IR_F2I:          return "F2I";
        case IR_I2F:          return "I2F";
        case IR_F2F:          return "F2F";
        case IR_ASM:          return "ASM";
        case IR_SVC:          return "SVC";
        case IR_DIV:          return "DIV";
        case IR_MOD:          return "MOD";
        case IR_64BINARY:     return "64BINARY";
        case IR_64PUSH:       return "64PUSH";
        case IR_64CMP:        return "64CMP";
        case IR_64F2I:        return "64F2I";
        case IR_64I2F:        return "64I2F";
        case IR_MEMCPY:       return "MEMCPY";
        case IR_STORE_FLD:    return "STORE_FLD";
        case IR_LOAD_FLD:     return "LOAD_FLD";
        case IR_PUSH_GLOBAL_ADDR: return "PUSH_GLOBAL_ADDR";
        case IR_ALLOCA:       return "ALLOCA";
        case IR_SMC:          return "SMC";
        case IR_SIMD_ADD16:   return "SIMD_ADD16";
        case IR_SIMD_SUB16:   return "SIMD_SUB16";
        case IR_SIMD_ADD8:    return "SIMD_ADD8";
        case IR_SIMD_SUB8:    return "SIMD_SUB8";
        case IR_SWAP:         return "SWAP";
        case IR_SEXT:         return "SEXT";
        case IR_ZEXT:         return "ZEXT";
        case IR_CDECL64:      return "CDECL64";
        case IR_PUSH_FUNC:    return "PUSH_FUNC";
        case IR_TRUNC:        return "TRUNC";
        default:              return "NOP";
    }
}
