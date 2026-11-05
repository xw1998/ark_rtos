/* Ark C Compiler - symtab.h
 * Symbol table: supports scope nesting, hash + linked-list implementation.
 */
#ifndef ARK_SYMTAB_H
#define ARK_SYMTAB_H

#include "common.h"
#include "type.h"

typedef enum {
    SYM_VAR = 0,
    SYM_FUNC,
    SYM_PARAM,
    SYM_GLOBAL,
    SYM_TYPEDEF,
    SYM_ENUM_CONST,
    SYM_STRUCT_TAG,
    SYM_UNION_TAG,
    SYM_ENUM_TAG
} SymKind;

typedef struct Symbol {
    SymKind        kind;
    char          *name;
    Type          *type;
    int            scope_level;
    int            offset;       /* local var: offset relative to fp; global/enum const: other use */
    int            is_defined;
    int            param_count;
    int            is_variadic;
    Type         **param_types;  /* function parameter types (parallel to param_count) */
    int            is_static;
    int            is_extern;
    int            enum_value;
    int            svc_num;      /* __svc(n) number; 0 means non-SVC function */
    int            smc_num;      /* __smc(n) number; 0 means non-SMC function */
    int            is_irq;       /* __irq interrupt function */
    int            is_weak;      /* __weak weak symbol */
    int            is_naked;     /* __naked naked function */
    int            is_used;      /* __attribute__((used)) */
    int            is_unused;    /* __attribute__((unused)) */
    int            is_asm_func;  /* __asm whole-function body */
    int            forceinline;  /* __forceinline */
    char          *alias_target; /* __attribute__((alias("xxx"))) */
    struct Symbol *next;
} Symbol;

void    sym_init(void);
void    sym_enter_scope(void);
void    sym_leave_scope(void);
int     sym_current_scope(void);
Symbol *sym_find(const char *name);
Symbol *sym_find_current(const char *name);
Symbol *sym_find_typedef(const char *name);
Symbol *sym_find_tag(const char *name);
Symbol *sym_add(SymKind kind, const char *name, Type *type, int offset);
int     sym_is_typedef(const char *name);

#endif /* ARK_SYMTAB_H */
