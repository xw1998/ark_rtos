/* Ark C Compiler - type.h
 * Type system: basic types + pointer + array + struct/union + function types.
 */
#ifndef ARK_TYPE_H
#define ARK_TYPE_H

#include "common.h"

typedef enum {
    TYPE_VOID = 0,
    TYPE_CHAR,
    TYPE_SHORT,
    TYPE_INT,
    TYPE_LONG,
    TYPE_LLONG,
    TYPE_BOOL,
    TYPE_FLOAT,
    TYPE_DOUBLE,
    TYPE_PTR,
    TYPE_ARRAY,
    TYPE_STRUCT,
    TYPE_UNION,
    TYPE_FUNC,
    TYPE_ENUM,
    TYPE_WCHAR,     /* wide character wchar_t (B2) */
    TYPE_COMPLEX    /* _Complex (B4): base points to float/double */
} ValueType;

typedef struct StructField {
    char               *name;
    struct Type        *type;
    int                 offset;
    int                 bit_offset;   /* bitfield start bit (packed info passthrough only) */
    int                 bit_width;    /* bitfield width; 0 means non-bitfield */
    struct StructField *next;
} StructField;

typedef struct Type {
    ValueType    kind;
    struct Type *base;       /* PTR/ARRAY element type; FUNC return type; COMPLEX real type */
    int          size;       /* array length; struct/union total byte count */
    int          is_unsigned;
    int          is_variadic;/* function: whether variadic */
    char        *tag;        /* struct/union/enum tag */
    StructField *fields;     /* struct/union field linked list */
    struct Type **params;    /* function parameter type array */
    int          param_count;
    int          is_packed;       /* __attribute__((packed)) */
    int          align_override;  /* __attribute__((aligned(N))) explicit alignment, 0=default */
    int          is_vla;          /* variable-length array flag (B3): runtime allocated */
    int          is_volatile;     /* volatile type qualifier (F2) */
    int          is_const;        /* const type qualifier (F2) */
    int          is_func_ptr;     /* pointer created by a `(*name)(params)` declarator:
                                   * the pointee is a function, not a data object. */
    int          bf_break;        /* C99 6.7.2.1: width=0 bitfield forces next bitfield to a new storage unit */
} Type;

Type *type_void(void);
Type *type_char(void);
Type *type_short(void);
Type *type_int(void);
Type *type_long(void);
Type *type_llong(void);
Type *type_bool(void);
Type *type_float(void);
Type *type_double(void);
Type *type_uchar(void);
Type *type_ushort(void);
Type *type_uint(void);
Type *type_ulong(void);
Type *type_ullong(void);
Type *type_pointer(Type *base);
Type *type_array(Type *base, int size);
Type *type_struct(const char *tag, int is_union);
Type *type_enum(const char *tag);
Type *type_func(Type *ret, Type **params, int pc, int variadic);
Type *type_wchar(void);                       /* B2: wchar_t */
Type *type_complex(Type *base);               /* B4: _Complex base (base is float/double) */
Type *type_qualified(Type *t, int is_volatile, int is_const); /* F2: qualified copy */

void  type_struct_add_field(Type *st, const char *name, Type *ft);
void  type_struct_add_bitfield(Type *st, const char *name, Type *ft, int width);
/* Anonymous struct/union field promotion: copy all named fields from child
 * into parent (relative offset added to the base determined by parent's current size).
 * The child itself occupies space as an anonymous block; parent->size increases by child size.
 * Used to support the anonymous struct/union nesting heavily used by CMSIS. */
void  type_struct_add_anonymous(Type *parent, Type *child);

int   type_size(Type *t);
int   type_align(Type *t);
int   type_equal(Type *a, Type *b);
/* Override pointer size/alignment for the selected backend.
 * Defaults to 4 (32-bit targets); the C backend sets 8 on 64-bit hosts
 * because generated code runs on the host where pointers are 8 bytes.
 * Must be called before parsing/sema/irgen so struct layout matches. */
void  type_set_ptr_size(int bytes);
int   type_is_scalar(Type *t);
int   type_is_integer(Type *t);
int   type_is_arith(Type *t);

StructField *type_find_field(Type *t, const char *name);

/* #pragma pack(n) global alignment control */
void type_set_pack(int n);
int  type_get_pack(void);

#endif /* ARK_TYPE_H */
