/* Ark C Compiler - symtab
 * Symbol table: hash table with nested scopes.
 *
 * Note: to support typedef and struct tags, the symbol table stores both
 * type names and variable names. Leaving a scope only removes higher-level
 * symbols; outer scopes are unaffected.
 */
#include "symtab.h"
#include "memory.h"

#define SYM_HASH_SIZE 128

static Symbol *g_table[SYM_HASH_SIZE];
static int     g_current_scope;

void sym_init(void) {
    int i;
    for (i = 0; i < SYM_HASH_SIZE; i++) g_table[i] = 0;
    g_current_scope = 0;
}

static unsigned sym_hash(const char *s) {
    unsigned h = 0;
    while (*s) {
        h = h * 31 + (unsigned char)*s;
        s++;
    }
    return h % SYM_HASH_SIZE;
}

void sym_enter_scope(void) { g_current_scope++; }

void sym_leave_scope(void) {
    int i;
    for (i = 0; i < SYM_HASH_SIZE; i++) {
        Symbol **pp = &g_table[i];
        while (*pp) {
            if ((*pp)->scope_level >= g_current_scope) {
                *pp = (*pp)->next;
            } else {
                pp = &(*pp)->next;
            }
        }
    }
    if (g_current_scope > 0) g_current_scope--;
}

int sym_current_scope(void) { return g_current_scope; }

Symbol *sym_find(const char *name) {
    unsigned h = sym_hash(name);
    Symbol *s;
    for (s = g_table[h]; s; s = s->next) {
        if (ark_streq(s->name, name)) return s;
    }
    return 0;
}

Symbol *sym_find_current(const char *name) {
    unsigned h = sym_hash(name);
    Symbol *s;
    for (s = g_table[h]; s; s = s->next) {
        if (s->scope_level == g_current_scope && ark_streq(s->name, name))
            return s;
    }
    return 0;
}

Symbol *sym_add(SymKind kind, const char *name, Type *type, int offset) {
    unsigned h = sym_hash(name);
    Symbol *s = (Symbol *)mem_pool_alloc(sizeof(Symbol));
    s->kind = kind;
    s->name = mem_strdup(name);
    s->type = type;
    s->scope_level = g_current_scope;
    s->offset = offset;
    s->is_defined = 0;
    s->param_count = 0;
    s->is_variadic = 0;
    s->param_types = 0;
    s->is_static = 0;
    s->is_extern = 0;
    s->enum_value = 0;
    s->svc_num = 0;
    s->smc_num = 0;
    s->is_irq = 0;
    s->is_weak = 0;
    s->is_naked = 0;
    s->is_used = 0;
    s->is_unused = 0;
    s->is_asm_func = 0;
    s->forceinline = 0;
    s->alias_target = 0;
    s->next = g_table[h];
    g_table[h] = s;
    return s;
}

int sym_is_typedef(const char *name) {
    unsigned h = sym_hash(name);
    Symbol *s;
    for (s = g_table[h]; s; s = s->next) {
        if (ark_streq(s->name, name) && s->kind == SYM_TYPEDEF) return 1;
    }
    return 0;
}

/* Like sym_find but returns the first symbol with kind SYM_TYPEDEF.
 * Needed because a typedef name and a struct tag can share the same
 * identifier (e.g. typedef struct AstNode AstNode;), and sym_find
 * returns whichever was added most recently (often the struct tag). */
Symbol *sym_find_typedef(const char *name) {
    unsigned h = sym_hash(name);
    Symbol *s;
    for (s = g_table[h]; s; s = s->next) {
        if (ark_streq(s->name, name) && s->kind == SYM_TYPEDEF) return s;
    }
    return 0;
}

/* Find a struct/union tag by name, bypassing any typedef with the same
 * name that would otherwise shadow it in sym_find(). Used by
 * parse_struct_or_union so that `typedef struct AstNode AstNode;`
 * followed by `struct AstNode { ... };` completes the SAME type object
 * that the typedef points to (instead of creating a new one). */
Symbol *sym_find_tag(const char *name) {
    unsigned h = sym_hash(name);
    Symbol *s;
    for (s = g_table[h]; s; s = s->next) {
        if (ark_streq(s->name, name) &&
            (s->kind == SYM_STRUCT_TAG || s->kind == SYM_UNION_TAG)) {
            return s;
        }
    }
    return 0;
}
