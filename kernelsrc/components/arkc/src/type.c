/* Ark C Compiler - type
 * Type system implementation. Most composite types are allocated from the memory pool.
 */
#include "type.h"
#include "memory.h"
#include "config.h"
/* Note: <stdio.h> intentionally NOT included - no stdio functions used in
 * type.c, and we want to avoid pulling in semihosting on STM32. */

static Type ty_void   = { TYPE_VOID,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_char   = { TYPE_CHAR,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_short  = { TYPE_SHORT,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_int    = { TYPE_INT,    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_long   = { TYPE_LONG,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_llong  = { TYPE_LLONG,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_bool   = { TYPE_BOOL,   0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_float  = { TYPE_FLOAT,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_double = { TYPE_DOUBLE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_uchar  = { TYPE_CHAR,   0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_ushort = { TYPE_SHORT,  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_uint   = { TYPE_INT,    0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_ulong  = { TYPE_LONG,   0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static Type ty_ullong = { TYPE_LLONG,  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
/* B2: wchar_t - passed through to gcc by host/C backend, treated as 4-byte unsigned integer */
static Type ty_wchar  = { TYPE_WCHAR,  0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

Type *type_void(void)   { return &ty_void;  }
Type *type_char(void)   { return &ty_char;  }
Type *type_short(void)  { return &ty_short; }
Type *type_int(void)    { return &ty_int;   }
Type *type_long(void)   { return &ty_long;  }
Type *type_llong(void)  { return &ty_llong; }
Type *type_bool(void)   { return &ty_bool;  }
Type *type_float(void)  { return &ty_float; }
Type *type_double(void) { return &ty_double;}
Type *type_uchar(void)  { return &ty_uchar; }
Type *type_ushort(void) { return &ty_ushort;}
Type *type_uint(void)   { return &ty_uint;  }
Type *type_ulong(void)  { return &ty_ulong; }
Type *type_ullong(void) { return &ty_ullong;}

Type *type_pointer(Type *base) {
    Type *t = (Type *)mem_pool_alloc(sizeof(Type));
    t->kind = TYPE_PTR;
    t->base = base;
    return t;
}

/* F2: Create a qualified copy of a type with volatile/const flags set.
 * Does not modify the original (important for singleton basic types).
 * If no qualifiers are requested, returns the original type unchanged. */
Type *type_qualified(Type *t, int is_volatile, int is_const) {
    Type *q;
    if (!t) return t;
    if (!is_volatile && !is_const) return t;
    q = (Type *)mem_pool_alloc(sizeof(Type));
    *q = *t;  /* shallow copy all fields */
    q->is_volatile = is_volatile;
    q->is_const = is_const;
    return q;
}

Type *type_array(Type *base, int size) {
    Type *t = (Type *)mem_pool_alloc(sizeof(Type));
    t->kind = TYPE_ARRAY;
    t->base = base;
    t->size = size;
    return t;
}

Type *type_struct(const char *tag, int is_union) {
    Type *t = (Type *)mem_pool_alloc(sizeof(Type));
    t->kind = is_union ? TYPE_UNION : TYPE_STRUCT;
    if (tag) t->tag = mem_strdup(tag);
    return t;
}

Type *type_enum(const char *tag) {
    Type *t = (Type *)mem_pool_alloc(sizeof(Type));
    t->kind = TYPE_ENUM;
    if (tag) t->tag = mem_strdup(tag);
    return t;
}

Type *type_func(Type *ret, Type **params, int pc, int variadic) {
    Type *t = (Type *)mem_pool_alloc(sizeof(Type));
    t->kind = TYPE_FUNC;
    t->base = ret;
    t->params = params;
    t->param_count = pc;
    t->is_variadic = variadic;
    return t;
}

/* B2: wchar_t - same size as host int, 4-byte unsigned */
Type *type_wchar(void) { return &ty_wchar; }

/* B4: _Complex - base points to float/double; size is 2*base */
Type *type_complex(Type *base) {
    Type *t = (Type *)mem_pool_alloc(sizeof(Type));
    t->kind = TYPE_COMPLEX;
    t->base = base;
    return t;
}

/* #pragma pack(n) global alignment control: when >0, limits struct field natural alignment to no more than this value.
 * 0 means restore default (natural alignment per field). */
static int g_pack_align = 0;

void type_set_pack(int n) { g_pack_align = n; }
int  type_get_pack(void)  { return g_pack_align; }

void type_struct_add_field(Type *st, const char *name, Type *ft) {
    StructField *f = (StructField *)mem_pool_alloc(sizeof(StructField));
    int al = type_align(ft);
    int fs = type_size(ft);
    int cur = st->size;
    int pack_val = 0;
    int is_flex = 0;  /* Flexible array member: ft is an array with size==0, not counted in sizeof */
    /* Union members all start at offset 0; only struct fields accumulate offsets. */
    if (st->kind == TYPE_UNION) cur = 0;
    /* Both #pragma pack and __attribute__((packed)) tighten alignment:
     *   pack_val = g_pack_align; if is_packed, force to 1 (packed takes precedence).
     * Field alignment is min(natural alignment, pack_val). */
    if (g_pack_align > 0) pack_val = g_pack_align;
    if (st->is_packed)    pack_val = 1;
    if (pack_val > 0 && al > pack_val) al = pack_val;
    if (al < 1) al = 1;
    /* Detect flexible array member: array type with size==0 */
    if (ft && ft->kind == TYPE_ARRAY && ft->size == 0) {
        is_flex = 1;
    }
    /* Record pack into align_override, so type_size/type_align can tighten overall alignment */
    if (pack_val > 0 && st->align_override == 0) st->align_override = pack_val;
    /* Field alignment */
    cur = (cur + al - 1) & ~(al - 1);
    f->name = mem_strdup(name ? name : "");
    f->type = ft;
    f->offset = cur;
    f->bit_offset = 0;
    f->bit_width = 0;
    f->next = 0;
    /* Append to tail of list, preserving declaration order */
    if (!st->fields) {
        st->fields = f;
    } else {
        StructField *p = st->fields;
        while (p->next) p = p->next;
        p->next = f;
    }
    if (st->kind == TYPE_UNION) {
        if (!is_flex && fs > st->size) st->size = fs;
    } else {
        if (!is_flex) st->size = cur + fs;
        /* Flexible array member does not increase struct size, but offset still records its start position */
    }
}

void type_struct_add_bitfield(Type *st, const char *name, Type *ft, int width) {
    /* Bitfield implementation: same-type bitfields share a storage unit where possible, packed tightly.
     *   - unit_bits = sizeof(ft) * 8 is the number of bits in a storage unit
     *   - If the previous field is a same-type bitfield with enough remaining bits, append after it
     *   - Otherwise open a new storage unit at a new aligned offset
     * Field offset points to the storage unit start; bit_offset is the in-unit bit offset;
     * bit_width is the bit width (>0 indicates a bitfield). irgen emits IR_LOAD_FLD/IR_STORE_FLD accordingly. */
    int unit_sz   = type_size(ft);
    int unit_bits = unit_sz * 8;
    int unit_al   = type_align(ft);
    int pack_val  = 0;
    StructField *f;
    StructField *last = 0;
    int cur_off;
    int bit_off;

    if (g_pack_align > 0) pack_val = g_pack_align;
    if (st->is_packed)    pack_val = 1;
    if (pack_val > 0 && unit_al > pack_val) unit_al = pack_val;
    if (pack_val > 0 && st->align_override == 0) st->align_override = pack_val;
    if (unit_al < 1) unit_al = 1;

    /* C99 6.7.2.1: A bit-field with width 0 indicates that no further
     * bit-field is to be packed into the unit in which the previous bit-field,
     * if any, was placed. No field is added; the next bitfield must start at
     * a new storage unit boundary. Named zero-width bitfields are forbidden by
     * C99 but accepted as a GCC extension (treated as anonymous separator). */
    if (width == 0) {
        st->bf_break = 1;
        return;
    }
    if (width < 0) {
        type_struct_add_field(st, name, ft);
        return;
    }
    /* Bit width cannot exceed storage unit bit count */
    if (width > unit_bits) width = unit_bits;

    /* Find the last field */
    if (st->fields) {
        last = st->fields;
        while (last->next) last = last->next;
    }
    /* Try to share a storage unit with the previous same-type bitfield.
     * bf_break (set by a preceding width=0 bitfield) forces a new unit. */
    /* Share the current storage unit with the previous bit-field when the two
     * have the same container size and alignment. Comparing the Type pointer
     * (the previous rule) split `unsigned a:3; int c:4;` into two units even
     * though armcc/gcc pack both into one 32-bit word: an int and an unsigned
     * int are distinct Type singletons but the same container class. Signedness
     * is irrelevant to packing (only to extraction), so it is not compared. */
    if (last && last->bit_width > 0 &&
        type_size(last->type) == unit_sz && type_align(last->type) == unit_al &&
        last->bit_offset + last->bit_width + width <= unit_bits &&
        !st->bf_break) {
        cur_off = last->offset;
        bit_off = last->bit_offset + last->bit_width;
    } else {
        /* New storage unit: align current size to unit_al */
        st->bf_break = 0;
        cur_off = st->size;
        cur_off = (cur_off + unit_al - 1) & ~(unit_al - 1);
        bit_off = 0;
        if (st->kind == TYPE_UNION) {
            if (unit_sz > st->size) st->size = unit_sz;
        } else {
            st->size = cur_off + unit_sz;
        }
    }

    f = (StructField *)mem_pool_alloc(sizeof(StructField));
    f->name = mem_strdup(name ? name : "");
    f->type = ft;
    f->offset = cur_off;
    f->bit_offset = bit_off;
    f->bit_width = width;
    f->next = 0;
    if (!st->fields) {
        st->fields = f;
    } else {
        StructField *p = st->fields;
        while (p->next) p = p->next;
        p->next = f;
    }
}

/* Anonymous struct/union field promotion: add each named field in child with
 * "base address (parent size aligned) + field relative offset" as the new offset, to the parent field list tail.
 *   - Alignment: round parent's current size up to align(child), as the base address.
 *   - Fields: preserve original bit_offset/bit_width.
 *   - parent size: base + child size.
 * For unions: parent size = max(original size, child size). */
void type_struct_add_anonymous(Type *parent, Type *child) {
    int base;
    int al = 1;
    int csz;
    StructField *f;
    if (!parent || !child) return;
    al = type_align(child);
    if (al < 1) al = 1;
    /* In UNION, all members start at offset 0 (overlapping storage);
     * in STRUCT, append after aligning current size */
    if (parent->kind == TYPE_UNION) {
        base = 0;
    } else {
        base = parent->size;
        base = (base + al - 1) & ~(al - 1);
    }
    csz = type_size(child);
    for (f = child->fields; f; f = f->next) {
        if (!f->name || f->name[0] == 0) continue;
        {
            StructField *nf = (StructField *)mem_pool_alloc(sizeof(StructField));
            nf->name = mem_strdup(f->name);
            nf->type = f->type;
            nf->offset = base + f->offset;
            nf->bit_offset = f->bit_offset;
            nf->bit_width = f->bit_width;
            nf->next = 0;
            if (!parent->fields) parent->fields = nf;
            else {
                StructField *p = parent->fields;
                while (p->next) p = p->next;
                p->next = nf;
            }
        }
    }
    if (parent->kind == TYPE_UNION) {
        if (csz > parent->size) parent->size = csz;
    } else {
        parent->size = base + csz;
    }
}

/* Pointer size for the selected backend. Defaults to 4 (32-bit ARM/RISC-V).
 * The C backend overrides this to sizeof(void*) (8 on 64-bit hosts) because
 * generated code runs on the host, where pointers are 8 bytes. Without this,
 * argv[i] (char**) would compute argv+i*4 and load 4 bytes, corrupting the
 * pointer and crashing with STATUS_ACCESS_VIOLATION. */
static int g_ptr_size = 4;

void type_set_ptr_size(int bytes) {
    if (bytes == 4 || bytes == 8) g_ptr_size = bytes;
}

int type_size(Type *t) {
    if (!t) return 4;
    switch (t->kind) {
        case TYPE_VOID:   return 0;
        case TYPE_CHAR:   return 1;
        case TYPE_BOOL:   return 1;
        case TYPE_SHORT:  return 2;
        case TYPE_INT:    return 4;
        case TYPE_LONG:   return 4;
        case TYPE_LLONG:  return 8;
        case TYPE_FLOAT:  return 4;
        case TYPE_DOUBLE: return 8;
        case TYPE_PTR:    return g_ptr_size;
        case TYPE_WCHAR:  return 4;  /* B2 */
        case TYPE_COMPLEX: return 2 * type_size(t->base); /* B4: real + imaginary parts */
        case TYPE_ARRAY:  return type_size(t->base) * (t->size > 0 ? t->size : 1);
        case TYPE_STRUCT: {
            /* Total size is padded to the maximum field alignment.
             * #pragma pack / __attribute__((packed)) tighten overall alignment via align_override
             * (smaller pack_val); here we cap field natural alignment to that value,
             * so struct tail padding also follows the tight alignment. aligned(N) widening is
             * handled by type_align returning align_override directly; not widened here to preserve existing behavior. */
            int max_align = 1, s = t->size;
            StructField *f;
            for (f = t->fields; f; f = f->next) {
                int al = type_align(f->type);
                if (al > max_align) max_align = al;
            }
            if (max_align < 1) max_align = 1;
            if (t->align_override > 0 && t->align_override < max_align)
                max_align = t->align_override;
            s = (s + max_align - 1) & ~(max_align - 1);
            return s;
        }
        case TYPE_UNION:  return t->size;
        case TYPE_ENUM:
            /* --enum_is_int / -fenum-is-int: force enum to int (4 bytes).
             * armcc by default picks the smallest type based on enum value range; arkcc always uses int,
             * so it returns 4 regardless of the switch. The switch only exists for armcc CLI compatibility. */
            (void)g_config.enum_is_int;
            return 4;
        case TYPE_FUNC:   return 4;
    }
    return 4;
}

int type_align(Type *t) {
    if (!t) return 4;
    if (t->align_override > 0) return t->align_override;
    if (t->is_packed) return 1;
    switch (t->kind) {
        case TYPE_CHAR:
        case TYPE_BOOL:   return 1;
        case TYPE_SHORT:  return 2;
        case TYPE_LLONG:
        case TYPE_DOUBLE: return 8;
        case TYPE_PTR:    return g_ptr_size; /* pointer is naturally aligned */
        case TYPE_COMPLEX: return type_align(t->base); /* B4 */
        case TYPE_ARRAY:  return type_align(t->base);
        case TYPE_STRUCT:
        case TYPE_UNION: {
            int max_align = 1;
            StructField *f;
            for (f = t->fields; f; f = f->next) {
                int al = type_align(f->type);
                if (al > max_align) max_align = al;
            }
            return max_align;
        }
        default:          return 4;
    }
}

int type_equal(Type *a, Type *b) {
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    if (a->kind == TYPE_PTR || a->kind == TYPE_ARRAY)
        return type_equal(a->base, b->base);
    if (a->kind == TYPE_COMPLEX)
        return type_equal(a->base, b->base);
    if (a->is_unsigned != b->is_unsigned) return 0;
    return 1;
}

int type_is_integer(Type *t) {
    if (!t) return 0;
    return t->kind == TYPE_CHAR || t->kind == TYPE_SHORT ||
           t->kind == TYPE_INT  || t->kind == TYPE_LONG  ||
           t->kind == TYPE_LLONG|| t->kind == TYPE_BOOL  ||
           t->kind == TYPE_ENUM ||
           t->kind == TYPE_WCHAR;  /* B2: wchar_t is an integer type */
}

int type_is_arith(Type *t) {
    if (!t) return 0;
    return type_is_integer(t) || t->kind == TYPE_FLOAT || t->kind == TYPE_DOUBLE;
}

int type_is_scalar(Type *t) {
    if (!t) return 0;
    return type_is_arith(t) || t->kind == TYPE_PTR;
}

StructField *type_find_field(Type *t, const char *name) {
    StructField *f;
    if (!t || !name) return 0;
    for (f = t->fields; f; f = f->next) {
        if (ark_streq(f->name, name)) return f;
    }
    return 0;
}
