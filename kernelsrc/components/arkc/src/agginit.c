/* Aggregate initialiser -> bytes.  See include/agginit.h for the contract.
 *
 * The walk below walks text and type together and writes bytes at the declared
 * offsets.  Offsets come from the type (field offsets, element stride), never
 * from counting commas, so nesting and padding fall out of the declaration.
 */
#include "agginit.h"

typedef struct {
    const char *s;      /* cursor into the initialiser text */
    int         ok;
    /* Asked about an element that is not an integer constant (see
     * ark_agg_fill_ex): 1 means the caller places that address itself. */
    int       (*addr_hook)(void *ud, int off, Type *t);
    void       *addr_ud;
} ArkAgg;

static void aga_ws(ArkAgg *c) {
    while (*c->s == ' ' || *c->s == '\t' || *c->s == '\n' || *c->s == '\r')
        c->s++;
}

static void aga_store(unsigned char *buf, int off, int sz, long long v) {
    int i;
    for (i = 0; i < sz; i++)
        buf[off + i] = (unsigned char)((unsigned long long)v >> (8 * i));
}

/* One integer constant: decimal or 0x, optional sign, optional U/L suffixes,
 * or a character constant.  Returns 0 when the text is not a constant. */
static int aga_num(ArkAgg *c, long long *out) {
    int neg = 0, base = 10, any = 0;
    unsigned long long v = 0;
    aga_ws(c);
    if (*c->s == '-') { neg = 1; c->s++; }
    else if (*c->s == '+') c->s++;
    if (*c->s == '\'') {                 /* character constant */
        unsigned val = 0;
        c->s++;
        if (*c->s == '\\') {
            c->s++;
            switch (*c->s) {
            case 'n': val = '\n'; break;
            case 't': val = '\t'; break;
            case 'r': val = '\r'; break;
            case '0': val = 0; break;
            case 'a': val = 7; break;
            case 'b': val = 8; break;
            case 'f': val = 12; break;
            case 'v': val = 11; break;
            case '\\': val = '\\'; break;
            case '\'': val = '\''; break;
            case '"': val = '"'; break;
            default: val = (unsigned char)*c->s; break;
            }
            c->s++;
        } else {
            val = (unsigned char)*c->s;
            c->s++;
        }
        if (*c->s != '\'') return 0;
        c->s++;
        *out = neg ? -(long long)val : (long long)val;
        return 1;
    }
    if (c->s[0] == '0' && (c->s[1] == 'x' || c->s[1] == 'X')) {
        base = 16;
        c->s += 2;
    }
    for (;;) {
        char ch = *c->s;
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (base == 16 && ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (base == 16 && ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else break;
        v = v * (unsigned)base + (unsigned)d;
        c->s++;
        any = 1;
    }
    while (*c->s == 'u' || *c->s == 'U' || *c->s == 'l' || *c->s == 'L')
        c->s++;
    if (!any) return 0;
    *out = neg ? (long long)(0ULL - v) : (long long)v;
    return 1;
}

/* A string literal filling `n` bytes at `off`; the tail keeps the zero fill. */
static int aga_string(ArkAgg *c, unsigned char *buf, int gsz, int off, int n) {
    int i = 0;
    if (*c->s != '"') return -1;
    c->s++;
    while (*c->s && *c->s != '"') {
        char ch = *c->s;
        if (ch == '\\') {
            c->s++;
            ch = *c->s;
            if (ch == 'n') ch = '\n';
            else if (ch == 't') ch = '\t';
            else if (ch == 'r') ch = '\r';
            else if (ch == '0') ch = '\0';
            else if (ch == '\\') ch = '\\';
            else if (ch == '"') ch = '"';
        }
        if (i < n && off + i < gsz) buf[off + i] = (unsigned char)ch;
        i++;
        c->s++;
    }
    if (*c->s != '"') return -1;
    c->s++;
    return off + ((i < n) ? i : n);
}

static int ark_agg_value(ArkAgg *c, Type *t, unsigned char *buf, int gsz,
                         int off);
static int ark_agg_object(ArkAgg *c, Type *t, unsigned char *buf, int gsz,
                          int off);
static int ark_agg_flat(ArkAgg *c, Type *t, unsigned char *buf, int gsz,
                        int off);

/* Set `width` bits of a storage unit, little-endian, starting at `bit_off`:
 * the unit is what a bitfield store writes, so the initialiser has to land in
 * exactly the same place.  The rest of the unit keeps whatever the earlier
 * fields put there. */
static void aga_set_bits(unsigned char *buf, int off, int unit_sz, int bit_off,
                         int width, unsigned long long v) {
    unsigned long long unit = 0, mask;
    int i;
    if (unit_sz <= 0 || unit_sz > 8 || width <= 0 || bit_off < 0) return;
    for (i = 0; i < unit_sz; i++)
        unit |= (unsigned long long)buf[off + i] << (8 * i);
    mask = (width >= 64) ? ~0ULL : ((1ULL << width) - 1ULL);
    unit = (unit & ~(mask << bit_off)) | ((v & mask) << bit_off);
    for (i = 0; i < unit_sz; i++)
        buf[off + i] = (unsigned char)((unit >> (8 * i)) & 0xFF);
}

/* One struct/union member at byte offset `off`: a bitfield is a bit range
 * inside its storage unit, everything else is a byte range. */
static int ark_agg_field(ArkAgg *c, StructField *f, unsigned char *buf,
                         int gsz, int off) {
    if (!f) return -1;
    if (f->bit_width > 0) {
        long long v;
        int usz = f->type ? type_size(f->type) : 4;
        if (usz <= 0 || off + f->offset + usz > gsz) return -1;
        if (!aga_num(c, &v)) return -1;
        aga_set_bits(buf, off + f->offset, usz, f->bit_offset, f->bit_width,
                     (unsigned long long)v);
        return off + f->offset + usz;
    }
    return ark_agg_object(c, f->type, buf, gsz, off + f->offset) < 0 ? -1 : 0;
}

/* C99 6.7.8p20 brace elision: `int a[2][2] = {1,2,3,4}` and
 * `struct T{int *p,*q;} t[1] = {&gv,&gv}` spell the sub-objects out without
 * their own braces, so the values of a sub-aggregate are taken from the
 * *enclosing* brace level, in declaration order, until that level ends.
 *
 * `ark_agg_flat()` is one such run.  It never consumes the closing '}' (the
 * level that owns it does), never starts a designator (a designator can only
 * name a member of the brace level it appears in, so `{[0]=1,2}` -- a
 * designator applied to a sub-object -- stays refused), and stops when the
 * value stream is used up, which is exactly how a short initialiser leaves the
 * tail zero-filled. */
static int ark_agg_flat(ArkAgg *c, Type *t, unsigned char *buf, int gsz, int off) {
    aga_ws(c);
    if (!t) t = type_int();
    switch (t->kind) {
    case TYPE_ARRAY: {
        int es = t->base ? type_size(t->base) : 4;
        int n = t->size;
        int cur = 0;
        if (es <= 0 || n <= 0) return -1;
        if (*c->s == '"') return ark_agg_value(c, t, buf, gsz, off);
        for (;;) {
            aga_ws(c);
            if (!*c->s || *c->s == '}') return off;      /* the run ended */
            if (*c->s == '[' || *c->s == '.') return -1; /* designator + elision */
            if (cur >= n) return off;   /* this sub-object is full: the caller
                                         * owns what follows (its comma, or the
                                         * value that overflows its own level) */
            if (*c->s == ',') { c->s++; continue; }
            if (ark_agg_object(c, t->base, buf, gsz, off + cur * es) < 0)
                return -1;
            cur++;
        }
    }
    case TYPE_STRUCT: {
        StructField *f = t->fields;
        for (;;) {
            aga_ws(c);
            if (!*c->s || *c->s == '}') return off;   /* the run ended */
            if (f && (*c->s == '.' || *c->s == '[')) return -1; /* designator */
            if (!f) return off;   /* every member is filled: the caller owns
                                   * the rest of the stream */
            if (*c->s == ',') { c->s++; continue; }
            if (ark_agg_field(c, f, buf, gsz, off) < 0) return -1;
            f = f->next;
        }
    }
    case TYPE_UNION: {
        /* One live member, so an elided union takes exactly one value for its
         * first member (which may itself be an aggregate and expand again). */
        StructField *f = t->fields;
        if (!f) return -1;
        aga_ws(c);
        if (!*c->s || *c->s == '}' || *c->s == ',') return -1;
        if (*c->s == '.' || *c->s == '[') return -1;
        if (ark_agg_field(c, f, buf, gsz, off) < 0) return -1;
        return off;
    }
    default:
        return ark_agg_value(c, t, buf, gsz, off);
    }
}

/* One object of type `t` at `off`, where braces around it are optional: a '{'
 * (or a string standing for a char array) starts its own brace level, anything
 * else continues the enclosing level. */
static int ark_agg_object(ArkAgg *c, Type *t, unsigned char *buf, int gsz, int off) {
    aga_ws(c);
    if (!t) t = type_int();
    if (*c->s == '{') return ark_agg_value(c, t, buf, gsz, off);
    /* A string is its own brace level only where it stands for the object: a
     * char array (`char n[4]` <- "ab").  For a pointer it is an address element
     * and ark_agg_value()'s default branch hands it to the hook; for a struct
     * or union it is the first member's value, i.e. brace elision -- and
     * sending it to ark_agg_value() required a '{' there and failed. */
    if (*c->s == '"' && t->kind == TYPE_ARRAY)
        return ark_agg_value(c, t, buf, gsz, off);
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_STRUCT || t->kind == TYPE_UNION)
        return ark_agg_flat(c, t, buf, gsz, off);
    return ark_agg_value(c, t, buf, gsz, off);
}

/* One initialiser value of type `t` at byte offset `off`; returns the offset
 * just past it, or -1 when the text is not something this build emits. */
static int ark_agg_value(ArkAgg *c, Type *t, unsigned char *buf, int gsz, int off) {
    aga_ws(c);
    if (!t) t = type_int();
    switch (t->kind) {
    case TYPE_ARRAY: {
        int es = t->base ? type_size(t->base) : 4;
        int n = t->size;
        int cur;
        if (es <= 0) return -1;
        if (*c->s == '"') {
            /* A string may stand for a char array of any rank: for `char s[6]`
             * it fills the array, and for `char m[2][6]` it fills one row. */
            int cap = (es == 1) ? n * es : es;
            if (cap <= 0) return -1;
            return aga_string(c, buf, gsz, off, cap);
        }
        if (*c->s != '{') return -1;
        c->s++;
        /* `cur` is the element being filled.  It advances by one per value and
         * jumps on a designator, so `{[1] = 10, [3] = 30}` writes at the
         * declared indices instead of at 0 and 1 (the old backend's answer).
         * The stride is the declared element size, not what a short string
         * happened to fill, so a short row cannot shift the next one. */
        for (cur = 0;;) {
            long long idx;
            aga_ws(c);
            if (*c->s == ',') { c->s++; continue; }
            if (*c->s == '}') { c->s++; return off; }
            if (*c->s == '[') {
                c->s++;
                if (!aga_num(c, &idx)) return -1;
                aga_ws(c);
                if (*c->s != ']') return -1;
                c->s++;
                aga_ws(c);
                if (*c->s != '=') return -1;
                c->s++;
                if (idx < 0 || idx >= (long long)n) return -1;
                cur = (int)idx;
            }
            if (cur >= n) return -1;         /* more initialisers than storage */
            if (ark_agg_object(c, t->base, buf, gsz, off + cur * es) < 0)
                return -1;
            cur++;
        }
    }
    case TYPE_STRUCT: {
        StructField *f;
        if (*c->s != '{') return -1;
        c->s++;
        f = t->fields;
        for (;;) {
            char nm[64];
            int k = 0;
            aga_ws(c);
            if (*c->s == ',') {
                c->s++;
                aga_ws(c);
            }
            if (*c->s == '}') { c->s++; return off; }
            if (*c->s == '.') {              /* .field = value */
                c->s++;
                aga_ws(c);
                while (*c->s && *c->s != '=' && *c->s != ' ' && *c->s != '\t'
                       && k < 63)
                    nm[k++] = *c->s++;
                nm[k] = 0;
                while (*c->s && *c->s != '=') c->s++;
                if (*c->s != '=') return -1;
                c->s++;
                f = type_find_field(t, nm);
                if (!f) return -1;
            }
            if (!f) return -1;
            if (ark_agg_field(c, f, buf, gsz, off) < 0) return -1;
            f = f->next;
        }
    }
    case TYPE_UNION: {
        /* C99 6.7.8p10: an undesignated initialiser initialises the *first*
         * member, and a designator picks another.  Only one member is ever
         * live, so `{.i = 0x41424344}` is well defined -- refusing it used to
         * leave the whole union zero without a word. */
        StructField *f = t->fields;
        int cap = type_size(t);
        aga_ws(c);
        if (*c->s != '{') return -1;
        c->s++;
        aga_ws(c);
        if (*c->s == '}') { c->s++; return off + (cap > 0 ? cap : 0); }
        if (*c->s == '.') {
            char nm[64];
            int k = 0;
            c->s++;
            aga_ws(c);
            while (*c->s && *c->s != '=' && *c->s != ' ' && *c->s != '\t'
                   && k < 63)
                nm[k++] = *c->s++;
            nm[k] = 0;
            while (*c->s && *c->s != '=') c->s++;
            if (*c->s != '=') return -1;
            c->s++;
            f = type_find_field(t, nm);
            if (!f) return -1;
        }
        if (!f) return -1;
        if (ark_agg_field(c, f, buf, gsz, off) < 0) return -1;
        aga_ws(c);
        if (*c->s == ',') c->s++;
        aga_ws(c);
        if (*c->s != '}') return -1;      /* only one member of a union */
        c->s++;
        return off + (cap > 0 ? cap : 0);
    }
    default: {
        long long v;
        int sz;
        if (*c->s == '{') {            /* braces around a scalar: `int x = {5};` */
            c->s++;
            if (ark_agg_object(c, t, buf, gsz, off) < 0) return -1;
            aga_ws(c);
            if (*c->s != '}') return -1;
            c->s++;
            return off + type_size(t);
        }
        if (!aga_num(c, &v)) {
            /* Not an integer constant: an address element of an aggregate
             * initialiser (`int (*tab[2])(int,int) = { add, sub };`, a struct
             * field holding a callback) reaches the caller's hook.  Only a
             * pointer can hold one; asking about anything else would let a
             * float or a bitfield slip through as "handled". */
            if (c->addr_hook && t && (t->kind == TYPE_PTR || t->kind == TYPE_FUNC)) {
                if (c->addr_hook(c->addr_ud, off, t)) {
                    /* The caller placed this address itself.  Its text was not
                     * parsed here, so step over it: advance to the next ',' or
                     * closing brace at this level, honouring nested () [] {}.
                     * Without this the cursor still sat on '&g0' and the next
                     * element looked like one initialiser too many. */
                    int depth = 0;
                    while (*c->s) {
                        char ch = *c->s;
                        if (ch == '(' || ch == '[' || ch == '{') depth++;
                        else if (ch == ')' || ch == ']') { if (depth == 0) break; depth--; }
                        else if (ch == '}') { if (depth == 0) break; depth--; }
                        else if (ch == ',' && depth == 0) break;
                        c->s++;
                    }
                    c->ok = 1;
                    sz = type_size(t);
                    return sz > 0 ? off + sz : off;
                }
            }
            return -1;
        }
        sz = type_size(t);
        if (sz <= 0 || off + sz > gsz) return -1;
        aga_store(buf, off, sz, v);
        return off + sz;
    }
    }
}

int ark_agg_fill(Type *t, const char *text, unsigned char *buf, int bufsz) {
    return ark_agg_fill_ex(t, text, buf, bufsz, 0, 0);
}

int ark_agg_fill_ex(Type *t, const char *text, unsigned char *buf, int bufsz,
                    int (*addr_hook)(void *ud, int off, Type *t), void *ud) {
    ArkAgg c;
    if (!text || !buf || bufsz <= 0) return -1;
    c.s = text;
    c.ok = 1;
    c.addr_hook = addr_hook;
    c.addr_ud = ud;
    if (ark_agg_value(&c, t, buf, bufsz, 0) < 0) return -1;
    aga_ws(&c);
    if (*c.s != 0) return -1;             /* trailing text: gave up halfway */             /* trailing text: gave up halfway */
    return 0;
}

void ark_agg_fill_cstr(const char *s, unsigned char *buf, int bufsz) {
    int k;
    for (k = 0; k < bufsz; k++) buf[k] = 0;
    if (!s) return;
    for (k = 0; s[k] && k < bufsz; k++) buf[k] = (unsigned char)s[k];
}
