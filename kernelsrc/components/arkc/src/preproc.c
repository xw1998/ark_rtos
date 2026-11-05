/* Ark C Compiler - preproc
 * C preprocessor implementation.
 *   - Object-like macros: #define NAME body
 *   - Function-like macros: #define MAX(a,b) (...)
 *   - Conditional compilation: #if/#ifdef/#ifndef/#elif/#else/#endif
 *   - #undef / #error / #warning / #pragma and other directives
 *   - #include "x.h" or <x.h>, nested up to 8 levels
 *   - Predefined macros: __FILE__, __LINE__, __ARK_CC__, __STDC__, __ARK_VERSION__
 *
 * Design notes ("rescan" refers to macro re-scanning):
 *   - Macro expansion uses a stop-set/blue-paint mechanism to avoid infinite
 *     recursion; # and ## operators are applied during expansion.
 *   - #if expressions support integer constant evaluation including
 *     defined(X)/defined X, &&/||/!, and arithmetic on intmax_t.
 *   - Stringification/token-pasting honor C99 6.10.3.x rules.
 */
#include "preproc.h"
#include "memory.h"
#include "platform.h"
#include "flash_fs.h"
#include "config.h"
#include "arkcc_config.h"
#include "type.h"   /* type_set_pack/type_get_pack drive #pragma pack handling. */
#include "diag.h"   /* codes, caret positioning, summary line. */

/* PP_BUF_SIZE and PP_MAX_INC_DEPTH come from arkcc_config.h (profile-aware).
 * STM32 profile: 64KB output / 8 include depth.
 * Host profile: 4MB output / 32 include depth. */
/* Per-include file content buffer. Kept out of the memory pool (a static 2D
 * array indexed by include depth) so that many #include directives no longer
 * exhaust the bump allocator. STM32 vendor headers (e.g. stm32f401xe.h) can
 * exceed 600 KB, so the host profile uses a 1 MB buffer per include depth.
 * Override via -DPP_INC_BUF=<bytes> if a larger header is needed. */
#ifndef PP_INC_BUF
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define PP_INC_BUF  (8 * 1024)         /* 8 KB per include for STM32 (was 16K to fit 512KB AXI) */
# else
#   define PP_INC_BUF  (1024 * 1024)      /* 1 MB per include for host */
# endif
#endif
#define PP_MAX_INC_DIRS  16
#define PP_PACK_STACK    32

#if ARKCC_FEATURE_TRIGRAPH
/* Trigraph translation: C99 defines 9 trigraph sequences (each 3 bytes -> 1).
 * ??=  #      ??(  [      ??/  \      ??)  ]
 * ??'  ^      ??<  {      ??>  }      ??!  |
 * ??-  ~
 * Enabled only when g_config.enable_trigraph is set.
 *
 * Because trigraphs are 3 input bytes per 1 output byte, in-place translation
 * is always safe (output is never longer than input). We use a write pointer
 * that may lag behind the read pointer; the source buffer is NUL-terminated. */
static int apply_trigraphs(char *src) {
    char *w = src;
    const char *p = src;
    while (*p) {
        if (p[0] == '?' && p[1] == '?') {
            char r = 0;
            switch (p[2]) {
                case '=': r = '#';  break;
                case '(': r = '[';  break;
                case '/': r = '\\'; break;
                case ')': r = ']';  break;
                case '\'': r = '^'; break;
                case '<': r = '{';  break;
                case '>': r = '}';  break;
                case '!': r = '|';  break;
                case '-': r = '~';  break;
                default:  r = 0;    break;
            }
            if (r) { *w++ = r; p += 3; continue; }
        }
        *w++ = *p++;
    }
    *w = 0;
    return (int)(w - src);
}
#endif

typedef struct Macro {
    char         *name;
    char         *body;
    int           is_func;
    int           is_variadic;  /* C99 variadic macro (...): __VA_ARGS__ captures trailing args. */
    char        **params;
    int           param_count;
    struct Macro *next;
} Macro;

static Macro *g_macros;
static int    g_inc_depth;
/* Per-depth include file content buffers (kept out of the memory pool so many
 * #include directives don't exhaust the bump allocator). Indexed by g_inc_depth.
 * Placed in .ark_buf section so the linker can put them in AXI SRAM on STM32H743. */
static char   g_inc_bufs[PP_MAX_INC_DEPTH][PP_INC_BUF] __attribute__((section(".ark_buf")));
static const char *g_inc_dirs[PP_MAX_INC_DIRS];
static int    g_inc_dir_count;
static char   g_main_file[256];
/* Track included files for dependency emission (-MMD). When a header resolves
 * to a path already recorded, we skip re-include via the include guard path. */
#define PP_MAX_INC_FILES 64
static const char *g_inc_files[PP_MAX_INC_FILES];
static int         g_inc_file_count;
/* #pragma pack(push/pop) stack for struct alignment control. */
static int    g_pack_stack[PP_PACK_STACK];
static int    g_pack_top;
/* Recursion guard for macro expansion. */
static int    g_macro_depth;
/* C99 6.10.3.4 "blue paint": a macro that is currently being expanded is
 * painted so it does not re-expand itself; the stop-set is a per-expansion
 * list of macro names that must not be re-expanded. */
#define PP_BLUE_STACK 64
static const char *g_blue_stack[PP_BLUE_STACK];
static int         g_blue_top;

static int is_blue_painted(const char *name) {
    int i;
    for (i = 0; i < g_blue_top; i++)
        if (ark_streq(g_blue_stack[i], name)) return 1;
    return 0;
}

static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alnum(char c) { return is_alpha(c) || is_digit(c); }
static int is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

static Macro *find_macro(const char *name) {
    Macro *m;
    for (m = g_macros; m; m = m->next) {
        if (ark_streq(m->name, name)) return m;
    }
    return 0;
}

static void remove_macro(const char *name) {
    Macro **pp = &g_macros;
    while (*pp) {
        if (ark_streq((*pp)->name, name)) { *pp = (*pp)->next; return; }
        pp = &(*pp)->next;
    }
}

static void add_object_macro(const char *name, const char *body) {
    Macro *m = (Macro *)mem_pool_alloc(sizeof(Macro));
    m->name = mem_strdup(name);
    m->body = mem_strdup(body ? body : "");
    m->is_func = 0;
    m->next = g_macros;
    g_macros = m;
}

static void add_func_macro(const char *name, char **params, int pc,
                            int is_variadic, const char *body) {
    Macro *m = (Macro *)mem_pool_alloc(sizeof(Macro));
    m->name = mem_strdup(name);
    m->body = mem_strdup(body ? body : "");
    m->is_func = 1;
    m->is_variadic = is_variadic;
    m->params = params;
    m->param_count = pc;
    m->next = g_macros;
    g_macros = m;
}

static void buf_putc(char *buf, int *plen, int cap, char c) {
    if (*plen + 1 >= cap) return;
    buf[(*plen)++] = c;
}

static void buf_puts(char *buf, int *plen, int cap, const char *s) {
    while (*s) buf_putc(buf, plen, cap, *s++);
}

/* Append the stringified form of a macro argument (C99 6.10.3.2 `#` operator):
 * leading/trailing whitespace is deleted, each interior whitespace run becomes
 * one space, and every " or \ is escaped so the result is a valid string
 * literal body. Whitespace inside a character/string literal that appears in
 * the argument is preserved verbatim.
 * Without this, a macro whose replacement list starts on the continuation line
 * (`#define M(a,b) \<newline>    do{...}while(0)`) surfaced that leading
 * indentation inside the quoted result, so XSTR(M(1,2))[0] was ' ' not 'd'. */
static void buf_put_stringified(char *buf, int *plen, int cap, const char *s) {
    int pending_space = 0;
    int emitted = 0;
    char open_quote = 0;
    const char *p = s;
    if (!s) return;
    for (; *p; p++) {
        char c = *p;
        if (open_quote) {
            if (c == '\\' || c == '"') buf_putc(buf, plen, cap, '\\');
            buf_putc(buf, plen, cap, c);
            if (c == open_quote) open_quote = 0;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            pending_space = 1;
            continue;
        }
        if (pending_space && emitted) buf_putc(buf, plen, cap, ' ');
        pending_space = 0;
        if (c == '"' || c == '\'') open_quote = c;
        if (c == '"' || c == '\\') buf_putc(buf, plen, cap, '\\');
        buf_putc(buf, plen, cap, c);
        emitted = 1;
    }
}

/* Format a signed int into the output buffer. */
static void emit_int(char *buf, int *plen, int cap, int v) {
    char tmp[16];
    int n = 0;
    int i;
    int neg = 0;
    unsigned u;
    if (v < 0) { neg = 1; u = (unsigned)(-v); } else u = (unsigned)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg) buf_putc(buf, plen, cap, '-');
    for (i = n - 1; i >= 0; i--) buf_putc(buf, plen, cap, tmp[i]);
}

void preproc_reset(void) {
    g_macros = 0;
    g_inc_depth = 0;
    g_inc_dir_count = 0;
    g_main_file[0] = 0;
    g_inc_file_count = 0;
    g_pack_top = 0;
    g_macro_depth = 0;
    g_blue_top = 0;
    type_set_pack(0);
}

/* Public API used by cpu.c (built-in macro definitions): defines NAME as body.
 * Removes any prior definition first to keep the last value assigned. */
void preproc_define(const char *name, const char *body) {
    remove_macro(name);
    add_object_macro(name, body);
}

void preproc_undef(const char *name) {
    remove_macro(name);
}

void preproc_add_include_dir(const char *dir) {
    if (g_inc_dir_count < PP_MAX_INC_DIRS) {
        g_inc_dirs[g_inc_dir_count++] = mem_strdup(dir);
    }
}

void preproc_set_main_file(const char *path) {
    int n = ark_strlen(path);
    int i;
    if (n >= (int)sizeof(g_main_file)) n = (int)sizeof(g_main_file) - 1;
    for (i = 0; i < n; i++) g_main_file[i] = path[i];
    g_main_file[n] = 0;
}

/* ---------------------------------------------------------------------------
 * Diagnostics
 *
 * The preprocessor diagnoses in the file it is *currently* reading, which is
 * not always the translation unit: an #include switches it for the duration of
 * the nested process() call. The position of the directive under inspection is
 * therefore file-scope state here, shared with the expression evaluator (a
 * separate function, which has to point the caret at the operator that divided
 * by zero).
 *
 * Columns are 1-based and measured against the source line, because the caret
 * line in diag.c replays the same line. A column of 0 means "not established"
 * (the text was joined from several lines, or it came from a macro body); the
 * IDE style then omits it rather than printing a made-up number.
 * ------------------------------------------------------------------------- */
static const char *g_pp_file;        /* file being read; 0 = the main file */
static const char *g_pp_line_start;  /* first byte of the current line */
static int         g_pp_line;        /* 1-based line number inside g_pp_file */
static const char *g_pp_expr_base;   /* text handed to the expression evaluator */
static int         g_pp_expr_len;
static int         g_pp_expr_col;    /* column of g_pp_expr_base[0]; 0 = unknown */

/* 1-based column of `q` on the current line. Callers pass the position armcc
 * points at, which for directive-level errors is usually one past the last
 * character it consumed. */
static int pp_col(const char *q) {
    return (int)(q - g_pp_line_start) + 1;
}

static void pp_diag(int line, int col, int code, DiagSeverity sev,
                    const char *a0, const char *a1, const char *a2) {
    DiagLoc loc;
    loc.file = g_pp_file;
    loc.line = line;
    loc.col  = col;
    diag_report(code, sev, &loc, a0, a1, a2);
}

/* Conditions armcc has no code for (arkc limits such as the include depth, and
 * the macro size caps) are reported without a number instead of borrowing a
 * neighbouring one. */
static void pp_diag_msg(int line, int col, DiagSeverity sev, const char *msg,
                        const char *a0, const char *a1) {
    DiagLoc loc;
    loc.file = g_pp_file;
    loc.line = line;
    loc.col  = col;
    diag_report_msg(sev, &loc, msg, a0, a1, 0);
}

/* Point the caret machinery at the expression about to be evaluated.
 * `start` is where expression text begins on the source line; `spliced` records
 * that the text was joined from several lines, which makes a text offset unable
 * to name a column. */
static void pp_expr_begin(const char *expr, const char *start, int spliced) {
    g_pp_expr_base = expr;
    g_pp_expr_len  = ark_strlen(expr);
    g_pp_expr_col  = spliced ? 0 : pp_col(start);
}

/* Column of a byte inside the expression being evaluated, or 0 when it cannot
 * be derived (see pp_expr_begin). A macro body evaluated inline lives in a
 * different buffer, so an offset outside [base, base+len) is not ours. */
static int pp_expr_col(const char *at) {
    if (!at || !g_pp_expr_base || g_pp_expr_col <= 0) return 0;
    if (at < g_pp_expr_base || at >= g_pp_expr_base + g_pp_expr_len) return 0;
    return g_pp_expr_col + (int)(at - g_pp_expr_base);
}

/* A header that does not fit in `cap` used to be truncated in silence: the
 * tail of a big CMSIS header holds its closing #endif, so the only symptom was
 * "#37 the #endif for this directive is missing" with the caret at the top of
 * the file. Report the real cause and return READ_INCLUDE_CUT so the caller
 * does not also blame a missing file.
 *   >= 0  bytes read
 *   -1    not found anywhere
 *   -2    found but larger than the buffer (already reported) */
#define READ_INCLUDE_CUT (-2)

static int read_include_one(const char *name, char *buf, int cap) {
    int rd = fs_read_file(name, buf, cap);
    if (rd < 0) return -1;
    if (fs_file_size(name) > rd) {
        char b1[12], b2[12];
        pp_diag_msg(g_pp_line, 1, DIAG_SEV_ERROR,
                    "include file is %s bytes, larger than the %s-byte "
                    "include buffer; rebuild arkcc with a bigger PP_INC_BUF",
                    diag_num(b1, fs_file_size(name)), diag_num(b2, cap));
        return READ_INCLUDE_CUT;
    }
    return rd;
}

/* Read an include file by name; search include dirs if not found.
 * -1 on miss, READ_INCLUDE_CUT on a file that did not fit. */
static int read_include(const char *name, char *buf, int cap) {
    int rd = read_include_one(name, buf, cap);
    if (rd != -1) return rd;
    {
        int i;
        for (i = 0; i < g_inc_dir_count; i++) {
            char path[512];
            int dl = ark_strlen(g_inc_dirs[i]);
            int nl = ark_strlen(name);
            int k;
            if (dl + 1 + nl + 1 > (int)sizeof(path)) continue;
            for (k = 0; k < dl; k++) path[k] = g_inc_dirs[i][k];
            path[dl] = '/';
            for (k = 0; k < nl; k++) path[dl + 1 + k] = name[k];
            path[dl + 1 + nl] = 0;
            rd = read_include_one(path, buf, cap);
            if (rd != -1) return rd;
        }
    }
    return -1;
}

/* === Integer constant expression evaluator (C99 6.10.1). === */
static const char *expr_p;
static long long eval_expr(int min_prec);

/* Whitespace and comments are both "space" to the preprocessor expression
 * evaluator (C99 6.10.1 evaluates the directive's token sequence, and a
 * comment is not a token).  Without the comment cases a trailing comment on
 * a #if line reads as a division whose right operand is '*': a bogus #39. */
static void skip_ws_p(void) {
    for (;;) {
        while (*expr_p == ' ' || *expr_p == '\t' || *expr_p == '\r' ||
               *expr_p == '\n') expr_p++;
        if (expr_p[0] == '/' && expr_p[1] == '*') {
            const char *p = expr_p + 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            expr_p = (*p) ? p + 2 : p;
            continue;
        }
        if (expr_p[0] == '/' && expr_p[1] == '/') {
            const char *p = expr_p + 2;
            while (*p && *p != '\n') p++;
            expr_p = p;
            continue;
        }
        break;
    }
}

static long long parse_primary(void) {
    long long v = 0;
    skip_ws_p();
    if (*expr_p == '(') {
        expr_p++;
        v = eval_expr(0);
        skip_ws_p();
        if (*expr_p == ')') expr_p++;
        return v;
    }
    if (*expr_p == '!') {
        expr_p++;
        return !parse_primary();
    }
    if (*expr_p == '-') { expr_p++; return -parse_primary(); }
    if (*expr_p == '+') { expr_p++; return parse_primary(); }
    if (*expr_p == '~') { expr_p++; return ~parse_primary(); }
    if (is_digit(*expr_p)) {
        /* C99 integer literal: 0x hex / 0 octal / decimal.
         * We use long long arithmetic and assume INTMAX_MAX range per
         * C99 6.10.1, since #if evaluates in intmax_t. */
        if (*expr_p == '0' && (expr_p[1] == 'x' || expr_p[1] == 'X')) {
            expr_p += 2;
            while (is_hex(*expr_p)) {
                v = v * 16 + hex_val(*expr_p);
                expr_p++;
            }
        } else if (*expr_p == '0' && expr_p[1] >= '0' && expr_p[1] <= '7') {
            expr_p++;
            while (*expr_p >= '0' && *expr_p <= '7') {
                v = v * 8 + (*expr_p - '0');
                expr_p++;
            }
        } else {
            while (is_digit(*expr_p)) { v = v * 10 + (*expr_p - '0'); expr_p++; }
        }
        /* Consume u/U/l/L suffix. */
        while (*expr_p == 'u' || *expr_p == 'U' || *expr_p == 'l' || *expr_p == 'L') expr_p++;
        return v;
    }
    if (is_alpha(*expr_p)) {
        const char *st = expr_p;
        char nm[64];
        int nlen = 0;
        while (is_alnum(*expr_p)) {
            if (nlen < 63) nm[nlen++] = *expr_p;
            expr_p++;
        }
        nm[nlen] = 0;
        if (ark_streq(nm, "defined")) {
            int paren = 0;
            char arg[64];
            int alen = 0;
            skip_ws_p();
            if (*expr_p == '(') { paren = 1; expr_p++; skip_ws_p(); }
            while (is_alnum(*expr_p) && alen < 63) { arg[alen++] = *expr_p++; }
            arg[alen] = 0;
            skip_ws_p();
            if (paren && *expr_p == ')') expr_p++;
            return find_macro(arg) ? 1 : 0;
        }
        /* Undefined identifier evaluates to 0 per C99 6.10.1. */
        {
            Macro *m = find_macro(nm);
            if (m && !m->is_func) {
                /* Object macro: inline-expand its body and recurse. */
                const char *save = expr_p;
                long long rv;
                expr_p = m->body;
                rv = eval_expr(0);
                expr_p = save;
                return rv;
            }
        }
        (void)st;
        return 0;
    }
    return 0;
}

static long long eval_expr(int min_prec) {
    long long lhs = parse_primary();
    for (;;) {
        int prec = -1, op = 0;
        const char *op_pos;
        skip_ws_p();
        op_pos = expr_p;   /* caret target for division-by-zero */
        if (expr_p[0] == '&' && expr_p[1] == '&') { prec = 1; op = 1; }
        else if (expr_p[0] == '|' && expr_p[1] == '|') { prec = 0; op = 2; }
        else if (expr_p[0] == '=' && expr_p[1] == '=') { prec = 2; op = 3; }
        else if (expr_p[0] == '!' && expr_p[1] == '=') { prec = 2; op = 4; }
        else if (expr_p[0] == '<' && expr_p[1] == '=') { prec = 3; op = 7; }
        else if (expr_p[0] == '>' && expr_p[1] == '=') { prec = 3; op = 8; }
        else if (expr_p[0] == '<' && expr_p[1] == '<') { prec = 3; op = 13; }
        else if (expr_p[0] == '>' && expr_p[1] == '>') { prec = 3; op = 14; }
        else if (expr_p[0] == '&') { prec = 3; op = 15; }
        else if (expr_p[0] == '|') { prec = 1; op = 16; }
        else if (expr_p[0] == '^') { prec = 2; op = 17; }
        else if (expr_p[0] == '<') { prec = 3; op = 5; }
        else if (expr_p[0] == '>') { prec = 3; op = 6; }
        else if (expr_p[0] == '+') { prec = 4; op = 9; }
        else if (expr_p[0] == '-') { prec = 4; op = 10; }
        else if (expr_p[0] == '*') { prec = 5; op = 11; }
        else if (expr_p[0] == '/') { prec = 5; op = 12; }
        else if (expr_p[0] == '%') { prec = 5; op = 18; }
        if (prec < min_prec) break;
        if (op == 1 || op == 2 || op == 3 || op == 4 || op == 7 || op == 8 ||
            op == 13 || op == 14) expr_p += 2;
        else expr_p++;
        {
            long long rhs = eval_expr(prec + 1);
            switch (op) {
                case 1:  lhs = lhs && rhs; break;
                case 2:  lhs = lhs || rhs; break;
                case 3:  lhs = lhs == rhs; break;
                case 4:  lhs = lhs != rhs; break;
                case 5:  lhs = lhs <  rhs; break;
                case 6:  lhs = lhs >  rhs; break;
                case 7:  lhs = lhs <= rhs; break;
                case 8:  lhs = lhs >= rhs; break;
                case 9:  lhs = lhs +  rhs; break;
                case 10: lhs = lhs -  rhs; break;
                case 11: lhs = lhs *  rhs; break;
                case 12:
                    if (rhs == 0) {
                        pp_diag(g_pp_line, pp_expr_col(op_pos), 39,
                                DIAG_SEV_ERROR, 0, 0, 0);
                        lhs = 0;
                    } else lhs = lhs / rhs;
                    break;
                case 13: lhs = lhs << rhs; break;
                case 14: lhs = lhs >> rhs; break;
                case 15: lhs = lhs &  rhs; break;
                case 16: lhs = lhs |  rhs; break;
                case 17: lhs = lhs ^  rhs; break;
                case 18:
                    if (rhs == 0) {
                        /* Measured: armcc has a separate code for "%" (#179)
                         * and points the caret at the "%". */
                        pp_diag(g_pp_line, pp_expr_col(op_pos), 179,
                                DIAG_SEV_ERROR, 0, 0, 0);
                        lhs = 0;
                    } else lhs = lhs % rhs;
                    break;
                default: break;
            }
        }
    }
    return lhs;
}

static int eval_pp_expr(const char *s) {
    expr_p = s;
    return eval_expr(0) ? 1 : 0;
}

/* Current source line counter; updated during pre-processing for __LINE__. */
static int g_cur_line;
/* Macro-expansion recursion flag (used to limit depth). */
static int g_macro_expand;
/* C99 __DATE__/__TIME__: cached once in preproc_process via ark_get_date_time
 * so that all references in the same translation unit return a stable value. */
static char g_build_date[12];
static char g_build_time[9];

static void process(const char *src, char *out, int *out_len, int out_cap,
                    int is_file);

/* Parse macro arguments: p must point at '('. Returns pointer past matching ')'.
 * Args are stored in args[] (max_args entries, currently 16). */
static const char *parse_macro_args(const char *p, char **args, int *parg_count, int max_args) {
    int paren = 1;
    int n = 0;
    char buf[1024];
    int bl = 0;
    if (*p != '(') { *parg_count = 0; return p; }
    p++;
    while (*p && paren > 0) {
        if (*p == '(') { paren++; if (bl < 1023) buf[bl++] = *p; p++; continue; }
        if (*p == ')') {
            paren--;
            if (paren == 0) {
                buf[bl] = 0;
                if (n < max_args) args[n++] = mem_strdup(buf);
                bl = 0;
                p++;
                break;
            }
            if (bl < 1023) buf[bl++] = *p; p++; continue;
        }
        if (*p == ',' && paren == 1) {
            buf[bl] = 0;
            if (n < max_args) args[n++] = mem_strdup(buf);
            bl = 0;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p;
            if (bl < 1023) buf[bl++] = *p;
            p++;
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) { if (bl < 1022) { buf[bl++] = *p; buf[bl++] = p[1]; } p += 2; continue; }
                if (bl < 1023) buf[bl++] = *p;
                p++;
            }
            if (*p) { if (bl < 1023) buf[bl++] = *p; p++; }
            continue;
        }
        if (bl < 1023) buf[bl++] = *p;
        p++;
    }
    if (bl >= 1023) {
        /* arkc limit, not an armcc behaviour: no code to borrow, and no column
         * worth guessing (the argument may span lines). */
        pp_diag_msg(g_pp_line, 0, DIAG_SEV_WARNING,
                    "macro argument truncated at 1023 bytes", 0, 0);
    }
    *parg_count = n;
    return p;
}

/* Substitute macro body using the params (matched by name) and the actual args.
 * For variadic macros, args[param_count..] are concatenated into __VA_ARGS__. */
static void expand_func_macro(Macro *m, char **args, int argc,
                              char *out, int *out_len, int out_cap) {
    const char *p = m->body;
    while (*p) {
        if (*p == '#' && p[1] == '#') {
            /* token concatenation */
            const char *after_hash;
            p += 2;
            /* Trim trailing whitespace in the output buffer. */
            while (*out_len > 0 && (out[*out_len - 1] == ' ' || out[*out_len - 1] == '\t'))
                (*out_len)--;
            while (*p == ' ' || *p == '\t') p++;
            /* GNU extension: ,##__VA_ARGS__ deletes the comma when __VA_ARGS__ is empty. */
            after_hash = p;
            if (m->is_variadic &&
                after_hash[0] == '_' && after_hash[1] == '_' &&
                after_hash[2] == 'V' && after_hash[3] == 'A' &&
                after_hash[4] == '_' && after_hash[5] == 'A' &&
                after_hash[6] == 'R' && after_hash[7] == 'G' &&
                after_hash[8] == 'S' && after_hash[9] == '_' &&
                after_hash[10] == '_') {
                /* __VA_ARGS__ is empty. */
                if (argc <= m->param_count) {
                    /* Strip the leading comma (GNU comma-deletion). */
                    while (*out_len > 0 && (out[*out_len - 1] == ' ' || out[*out_len - 1] == '\t'))
                        (*out_len)--;
                    if (*out_len > 0 && out[*out_len - 1] == ',')
                        (*out_len)--;
                    p += 11;  /* consume __VA_ARGS__ */
                    continue;
                }
            }
            continue;
        }
        if (*p == '#' && is_alpha(p[1])) {
            /* Stringification (#param): emit the argument as a quoted string. */
            const char *q = p + 1;
            char nm[64];
            int nl = 0;
            while (is_alnum(*q) && nl < 63) { nm[nl++] = *q++; }
            nm[nl] = 0;
            /* Try to match a parameter name. */
            {
                int i;
                int found = -1;
                for (i = 0; i < m->param_count; i++)
                    if (ark_streq(m->params[i], nm)) { found = i; break; }
                if (found >= 0 && found < argc) {
                    buf_putc(out, out_len, out_cap, '"');
                    buf_put_stringified(out, out_len, out_cap, args[found]);
                    buf_putc(out, out_len, out_cap, '"');
                    p = q;
                    continue;
                }
            }
            buf_putc(out, out_len, out_cap, *p++);
            continue;
        }
        if (is_alpha(*p)) {
            const char *st = p;
            char nm[64];
            int nl = 0;
            int matched = -1;
            int i;
            while (is_alnum(*p) && nl < 63) { nm[nl++] = *p++; }
            nm[nl] = 0;
            /* C99 variadic: __VA_ARGS__ references all args after the named ones. */
            if (m->is_variadic && ark_streq(nm, "__VA_ARGS__")) {
                int vi;
                for (vi = m->param_count; vi < argc; vi++) {
                    if (vi > m->param_count)
                        buf_puts(out, out_len, out_cap, ", ");
                    /* Rescan the argument so nested macros expand. */
                    process(args[vi], out, out_len, out_cap, 0);
                }
                continue;
            }
            for (i = 0; i < m->param_count; i++)
                if (ark_streq(m->params[i], nm)) { matched = i; break; }
            if (matched >= 0 && matched < argc) {
                /* Rescan substituted argument so its macros expand. */
                process(args[matched], out, out_len, out_cap, 0);
            } else {
                int k;
                for (k = 0; k < nl; k++) buf_putc(out, out_len, out_cap, st[k]);
            }
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p;
            buf_putc(out, out_len, out_cap, *p++);
            while (*p && *p != q) {
                if (*p == '\\' && p[1]) {
                    buf_putc(out, out_len, out_cap, *p++);
                }
                buf_putc(out, out_len, out_cap, *p++);
            }
            if (*p) buf_putc(out, out_len, out_cap, *p++);
            continue;
        }
        buf_putc(out, out_len, out_cap, *p++);
    }
}

static int handle_if(const char *expr_text) {
    return eval_pp_expr(expr_text) ? 1 : 0;
}

/* #pragma diag_suppress <n>[, <n>...]  and  #pragma diag_default <n>...
 * armcc also has diag_warning / diag_error, which *change* a message's
 * severity; arkc does not implement severity remapping, so those are left
 * alone rather than half-honoured. */
static void handle_pragma_diag(const char *r) {
    char word[32];
    int n = 0;
    int suppress;

    while (*r && is_alnum(*r) && n < 31) word[n++] = *r++;
    word[n] = 0;
    if (ark_streq(word, "diag_suppress")) suppress = 1;
    else if (ark_streq(word, "diag_default")) suppress = 0;
    else return;

    for (;;) {
        while (*r == ' ' || *r == '\t' || *r == ',') r++;
        if (*r < '0' || *r > '9') break;
        {
            int code = 0;
            while (*r >= '0' && *r <= '9') { code = code * 10 + (*r - '0'); r++; }
            if (suppress) diag_suppress(code);
            else          diag_default(code);
        }
    }
}

/* Handle the #pragma pack directive; q points right after the "pack" token.
 * Supported forms:
 *   pack        -> reset to default alignment
 *   pack()      -> reset to default (0)
 *   pack(n)     -> set current alignment to n
 *   pack(push)  -> push current alignment onto stack
 *   pack(push,n)-> push current and set new alignment to n
 *   pack(pop)   -> pop alignment from stack
 * The named-pack and push/pop stacks are tracked separately; the only special
 * case is "if no pack has ever been set, packed attribute is honored" which we
 * model by leaving the stack empty and returning pack=0 (no explicit align). */
static int handle_pragma_pack(const char *q) {
    /* q is past "pack"; skip whitespace and expect an opening '('. */
    while (*q == ' ' || *q == '\t') q++;
    if (*q != '(') return 0;
    q++;
    while (*q == ' ' || *q == '\t') q++;
    if (*q == ')') {
        /* pack(): reset to default. */
        type_set_pack(0);
        return 1;
    }
    if (q[0] == 'p' && q[1] == 'u' && q[2] == 's' && q[3] == 'h') {
        /* push [n] */
        int old = type_get_pack();
        q += 4;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ',') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            if (*q >= '0' && *q <= '9') {
                int n = 0;
                while (*q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; }
                if (g_pack_top < PP_PACK_STACK) g_pack_stack[g_pack_top++] = old;
                type_set_pack(n);
                return 1;
            }
        }
        /* pack(push): just save the current alignment. */
        if (g_pack_top < PP_PACK_STACK) g_pack_stack[g_pack_top++] = old;
        return 1;
    }
    if (q[0] == 'p' && q[1] == 'o' && q[2] == 'p') {
        /* pop */
        if (g_pack_top > 0) {
            int v = g_pack_stack[--g_pack_top];
            type_set_pack(v);
        }
        return 1;
    }
    if (*q >= '0' && *q <= '9') {
        /* pack(n) */
        int n = 0;
        while (*q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; }
        type_set_pack(n);
        return 1;
    }
    return 0;
}

/* Main processing entry point.
 *
 * is_file separates a real file from a macro body being rescanned: only a real
 * file has a file identity, line numbers of its own, and can be left with an
 * unclosed #if at its end. */
static void process(const char *src, char *out, int *out_len, int out_cap,
                    int is_file) {
    const char *p = src;
    const char *outer_file = g_pp_file;   /* the file our caller was reading */
    /* Skip UTF-8 BOM (EF BB BF) if present at start of file. */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    /* Conditional compilation stack: tracks both taken state and skip mask. */
    int cond_stack[32];
    int cond_taken[32];   /* parent already-taken mask for skipping branches */
    int cond_line[32];    /* line of the #if that opened each level */
    int cond_else[32];    /* an #else was already seen at this level */
    int cond_top = 0;
    int active = 1;
    int saved_line = g_cur_line;
    int line_no = 1;

#define ACTIVE() (active)

    while (*p) {
        /* C99 phase 2: line continuation (backslash-newline) is removed
         * before any other processing. This applies to all code, not just
         * macro definitions (e.g. multi-line if statements in HAL sources). */
        if (*p == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
            p += (p[1] == '\r') ? 3 : 2;
            line_no++;
            if (ACTIVE()) buf_putc(out, out_len, out_cap, ' ');
            continue;
        }

        /* Detect directive lines (lines whose first non-ws char is '#'). */
        const char *line_start = p;
        const char *q;
        int is_pp_line = 0;
        if (is_file) { g_pp_line_start = line_start; g_pp_line = line_no; }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#') {
            is_pp_line = 1;
        } else {
            p = line_start;
        }

        if (is_pp_line) {
            char cmd[16];
            int n = 0;
            q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            while (*q && is_alpha(*q) && n < 15) cmd[n++] = *q++;
            cmd[n] = 0;
            while (*q == ' ' || *q == '\t') q++;

            if (ark_streq(cmd, "define") && ACTIVE()) {
                char name[64];
                int nl = 0;
                int is_fn = 0;
                int is_variadic = 0;
                char **params = 0;
                int pc = 0;
                int pc_cap = 0;
                char body[16384];
                int bl = 0;
                while (*q && is_alnum(*q) && nl < 63) name[nl++] = *q++;
                name[nl] = 0;
                if (nl == 0) {
                    pp_diag(g_pp_line, pp_col(q), 40, DIAG_SEV_ERROR, 0, 0, 0);
                }
                if (*q == '(') {
                    is_fn = 1;
                    q++;
                    for (;;) {
                        char pname[64];
                        int pl = 0;
                        while (*q == ' ' || *q == '\t') q++;
                        if (*q == ')') { q++; break; }
                        /* C99 variadic: ... declares __VA_ARGS__. */
                        if (*q == '.' && q[1] == '.' && q[2] == '.') {
                            q += 3;
                            is_variadic = 1;
                            while (*q == ' ' || *q == '\t') q++;
                            if (*q == ')') q++;
                            break;
                        }
                        while (*q && is_alnum(*q) && pl < 63) pname[pl++] = *q++;
                        pname[pl] = 0;
                        if (pl > 0) {
                            if (pc >= pc_cap) {
                                int nc = pc_cap ? pc_cap * 2 : 4;
                                params = (char **)mem_realloc(params, pc_cap * (int)sizeof(char*), nc*(int)sizeof(char*));
                                pc_cap = nc;
                            }
                            params[pc++] = mem_strdup(pname);
                        }
                        while (*q == ' ' || *q == '\t') q++;
                        if (*q == ',') { q++; continue; }
                        if (*q == ')') { q++; break; }
                        break;
                    }
                }
                while (*q == ' ' || *q == '\t') q++;
                while (*q && *q != '\n' && bl < 16383) {
                    /* Line continuation via trailing backslash-newline.
                     * Handle both Unix (\n) and Windows (\r\n) line endings. */
                    if (*q == '\\' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) {
                        q += (q[1] == '\r') ? 3 : 2;
                        line_no++;
                        body[bl++] = ' ';
                        continue;
                    }
                    /* Strip comments from macro body (C99 phase 3: comments -> space).
                     * Without this, a body like "1 / star comment star /" would leave
                     * the slash-star sequence in the body, which eval_pp_expr later
                     * mis-reads as division-by-star, making the macro evaluate to 0. */
                    if (*q == '/' && q[1] == '*') {
                        q += 2;
                        while (*q && !(*q == '*' && q[1] == '/')) {
                            if (*q == '\n') line_no++;
                            q++;
                        }
                        if (*q == '*' && q[1] == '/') q += 2;
                        body[bl++] = ' ';
                        continue;
                    }
                    if (*q == '/' && q[1] == '/') {
                        /* Line comment: rest of line is comment */
                        break;
                    }
                    body[bl++] = *q++;
                }
                if (bl >= 16383 && *q && *q != '\n') {
                    pp_diag_msg(g_pp_line, 0, DIAG_SEV_WARNING,
                                "macro '%s' body truncated at 16383 bytes",
                                nl > 0 ? name : "?", 0);
                }
                /* If macro body was truncated (exceeded buffer), consume the rest of
                 * the macro definition (including line continuations) to avoid emitting
                 * the remaining body as regular code. */
                while (*q) {
                    if (*q == '\\' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) {
                        q += (q[1] == '\r') ? 3 : 2;
                        line_no++;
                        continue;
                    }
                    if (*q == '\n') break;
                    q++;
                }
                while (bl > 0 && (body[bl-1] == ' ' || body[bl-1] == '\t' || body[bl-1] == '\r')) bl--;
                body[bl] = 0;
                if (nl > 0) {
                    if (is_fn) add_func_macro(name, params, pc, is_variadic, body);
                    else add_object_macro(name, body);
                }
            }
            else if (ark_streq(cmd, "undef") && ACTIVE()) {
                char name[64];
                int nl = 0;
                while (*q && is_alnum(*q) && nl < 63) name[nl++] = *q++;
                name[nl] = 0;
                if (nl > 0) remove_macro(name);
                else pp_diag(g_pp_line, pp_col(q), 40, DIAG_SEV_ERROR, 0, 0, 0);
            }
            else if (ark_streq(cmd, "include") && ACTIVE()) {
                char path[256];
                int pl = 0;
                char quote;
                if (*q != '"' && *q != '<') {
                    /* Measured: "#13: expected a file name" with the caret one
                     * past the end of the directive. */
                    pp_diag(g_pp_line, pp_col(q), 13, DIAG_SEV_ERROR, 0, 0, 0);
                } else {
                    quote = (*q == '<') ? '>' : '"';
                    q++;
                    while (*q && *q != quote && pl < 255) path[pl++] = *q++;
                    path[pl] = 0;
                    if (*q == quote) q++;
                    if (g_inc_depth >= PP_MAX_INC_DEPTH) {
                        char d1[12], d2[12];
                        /* armcc has no depth limit (400 levels compile
                         * silently), so there is no code to borrow: this is an
                         * arkc limit and is reported as one. */
                        pp_diag_msg(g_pp_line, pp_col(q), DIAG_SEV_ERROR,
                                    "include nested too deeply (%s levels, limit %s)",
                                    diag_num(d1, g_inc_depth),
                                    diag_num(d2, PP_MAX_INC_DEPTH));
                    } else {
                        char *inc_buf = g_inc_bufs[g_inc_depth];
                        int rd = read_include(path, inc_buf, PP_INC_BUF);
                        if (rd == READ_INCLUDE_CUT) {
                            /* read_include already reported the size; adding
                             * "cannot open source input file" here would name
                             * the wrong cause. */
                        } else if (rd < 0) {
                            /* Same code and shape as a missing main input file,
                             * with the caret one past the closing quote and the
                             * reason from the file layer (measured). */
                            pp_diag(g_pp_line, pp_col(q), 5, DIAG_SEV_ERROR,
                                    "source", path, fs_last_error());
                        } else {
                            /* Record include for -MMD dependency output. We
                             * mem_strdup because the path buffer goes out of
                             * scope after this block exits. */
                            if (g_inc_file_count < PP_MAX_INC_FILES) {
                                g_inc_files[g_inc_file_count++] = mem_strdup(path);
                            }
                            {
                                /* Diagnostics inside the header name the header,
                                 * not the translation unit. `path` outlives the
                                 * nested call, and the pointer is only used while
                                 * a diagnostic is being emitted. */
                                const char *caller_file = g_pp_file;
                                g_pp_file = path;
                                g_inc_depth++;
                                process(inc_buf, out, out_len, out_cap, 1);
                                g_inc_depth--;
                                g_pp_file = caller_file;
                            }
                        }
                    }
                }
            }
            else if (ark_streq(cmd, "if")) {
                int v = 0;
                char expr[512];
                int el = 0;
                const char *expr_start = q;
                int spliced = 0;
                /* '\r' ends the line too: sources here are CRLF, and letting it
                 * into the expression would make an empty one look non-empty. */
                while (*q && *q != '\n' && *q != '\r' && el < 511) {
                    /* Line continuation: backslash-newline joins lines */
                    if (*q == '\\' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) {
                        q += (q[1] == '\r') ? 3 : 2;
                        line_no++;
                        expr[el++] = ' ';
                        spliced = 1;
                        continue;
                    }
                    expr[el++] = *q++;
                }
                expr[el] = 0;
                while (el > 0 && (expr[el - 1] == ' ' || expr[el - 1] == '\t'))
                    expr[--el] = 0;
                if (el == 0) {
                    /* Measured: "#29: expected an expression" at the end of the
                     * directive line. The group still opens, so a missing
                     * #endif is reported separately. */
                    pp_diag(g_pp_line, pp_col(q), 29, DIAG_SEV_ERROR, 0, 0, 0);
                }
                pp_expr_begin(expr, expr_start, spliced);
                if (cond_top < 32) {
                    cond_stack[cond_top] = ACTIVE();
                    cond_else[cond_top] = 0;
                    cond_line[cond_top] = g_pp_line;
                    if (ACTIVE()) {
                        v = handle_if(expr);
                        cond_taken[cond_top] = v;
                        active = v;
                    } else {
                        cond_taken[cond_top] = 1; /* already inactive: pretend taken so else is skipped */
                    }
                    cond_top++;
                }
            }
            else if (ark_streq(cmd, "ifdef") || ark_streq(cmd, "ifndef")) {
                int neg = ark_streq(cmd, "ifndef");
                char name[64];
                int nl = 0;
                while (*q && is_alnum(*q) && nl < 63) name[nl++] = *q++;
                name[nl] = 0;
                if (nl == 0) {
                    pp_diag(g_pp_line, pp_col(q), 40, DIAG_SEV_ERROR, 0, 0, 0);
                }
                if (cond_top < 32) {
                    cond_stack[cond_top] = ACTIVE();
                    cond_else[cond_top] = 0;
                    cond_line[cond_top] = g_pp_line;
                    if (ACTIVE()) {
                        int defined = find_macro(name) ? 1 : 0;
                        int v = neg ? !defined : defined;
                        cond_taken[cond_top] = v;
                        active = v;
                    } else {
                        cond_taken[cond_top] = 1;
                    }
                    cond_top++;
                }
            }
            else if (ark_streq(cmd, "elif")) {
                /* The expression is collected even when the branch is already
                 * dead: armcc still checks the directive there, and reports an
                 * empty one as a warning rather than an error. */
                char expr[512];
                int el = 0;
                const char *expr_start = q;
                int spliced = 0;
                /* '\r' ends the line too: sources here are CRLF, and letting it
                 * into the expression would make an empty one look non-empty. */
                while (*q && *q != '\n' && *q != '\r' && el < 511) {
                    if (*q == '\\' && (q[1] == '\n' || (q[1] == '\r' && q[2] == '\n'))) {
                        q += (q[1] == '\r') ? 3 : 2;
                        line_no++;
                        expr[el++] = ' ';
                        spliced = 1;
                        continue;
                    }
                    expr[el++] = *q++;
                }
                expr[el] = 0;
                while (el > 0 && (expr[el - 1] == ' ' || expr[el - 1] == '\t'))
                    expr[--el] = 0;
                pp_expr_begin(expr, expr_start, spliced);

                if (cond_top == 0) {
                    pp_diag(g_pp_line, 2, 36, DIAG_SEV_ERROR, 0, 0, 0);
                } else if (cond_else[cond_top - 1]) {
                    pp_diag(g_pp_line, 2, 38, DIAG_SEV_ERROR, 0, 0, 0);
                    active = 0;
                } else if (cond_taken[cond_top - 1]) {
                    if (el == 0) {
                        pp_diag(g_pp_line, pp_col(q), 29, DIAG_SEV_WARNING,
                                0, 0, 0);
                    }
                    active = 0;
                } else if (cond_stack[cond_top - 1]) {
                    int v;
                    if (el == 0) {
                        pp_diag(g_pp_line, pp_col(q), 29, DIAG_SEV_ERROR,
                                0, 0, 0);
                    }
                    v = handle_if(expr);
                    cond_taken[cond_top - 1] = v;
                    active = v;
                } else {
                    active = 0;
                }
            }
            else if (ark_streq(cmd, "else")) {
                if (cond_top == 0) {
                    pp_diag(g_pp_line, 2, 36, DIAG_SEV_ERROR, 0, 0, 0);
                } else if (cond_else[cond_top - 1]) {
                    pp_diag(g_pp_line, 2, 38, DIAG_SEV_ERROR, 0, 0, 0);
                } else {
                    int outer = cond_stack[cond_top - 1];
                    cond_else[cond_top - 1] = 1;
                    if (cond_taken[cond_top - 1]) active = 0;
                    else if (outer) { cond_taken[cond_top - 1] = 1; active = 1; }
                    else active = 0;
                }
            }
            else if (ark_streq(cmd, "endif")) {
                if (cond_top > 0) {
                    cond_top--;
                    active = cond_stack[cond_top];
                } else {
                    pp_diag(g_pp_line, 2, 36, DIAG_SEV_ERROR, 0, 0, 0);
                }
            }
            else if (ark_streq(cmd, "error") && ACTIVE()) {
                char msg[256]; int ml = 0;
                while (*q && *q != '\n' && *q != '\r' && ml < 255) msg[ml++] = *q++;
                msg[ml] = 0;
                /* Measured: the caret sits at column 2 (just past the '#'),
                 * and the text is the rest of the line. */
                pp_diag(g_pp_line, 2, 35, DIAG_SEV_ERROR, msg, 0, 0);
            }
            else if (ark_streq(cmd, "warning") && ACTIVE()) {
                char msg[256]; int ml = 0;
                while (*q && *q != '\n' && *q != '\r' && ml < 255) msg[ml++] = *q++;
                msg[ml] = 0;
                pp_diag(g_pp_line, 2, 1215, DIAG_SEV_WARNING, msg, 0, 0);
            }
            else if (ark_streq(cmd, "pragma")) {
                /* Pragma dispatch. q points right after "pragma"; we look at the
                 * next token ("pack", "location", etc.) and forward accordingly.
                 * Recognized pragmas: pack/location/anon_unions/once/O*.
                 * #pragma location=addr is rewritten as a __at(addr) prefix so
                 *   the parser sees it as a declaration attribute. */
                const char *r = q;
                if (r[0] == 'p' && r[1] == 'a' && r[2] == 'c' && r[3] == 'k' &&
                    (r[4] == '(' || r[4] == ' ' || r[4] == '\t')) {
                    handle_pragma_pack(r + 4);
                }
                /* #pragma location=addr: rewrite as __at(N) so the parser
                 *   picks it up as a declaration attribute on the next
                 *   declaration. We accept decimal and 0x.. hex forms. */
#if ARKCC_FEATURE_ARMCC_PRAGMA
                else if (r[0] == 'l' && r[1] == 'o' && r[2] == 'c' &&
                         r[3] == 'a' && r[4] == 't' && r[5] == 'i' &&
                         r[6] == 'o' && r[7] == 'n') {
                    const char *t2 = r + 8;
                    while (*t2 == ' ' || *t2 == '\t' || *t2 == '=') t2++;
                    if (*t2) {
                        unsigned addr = 0;
                        int got = 0;
                        if (t2[0] == '0' && (t2[1] == 'x' || t2[1] == 'X')) {
                            t2 += 2;
                            while ((*t2 >= '0' && *t2 <= '9') ||
                                   (*t2 >= 'a' && *t2 <= 'f') ||
                                   (*t2 >= 'A' && *t2 <= 'F')) {
                                int d;
                                if (*t2 <= '9') d = *t2 - '0';
                                else if (*t2 <= 'F') d = *t2 - 'A' + 10;
                                else d = *t2 - 'a' + 10;
                                addr = addr * 16 + (unsigned)d;
                                t2++; got = 1;
                            }
                        } else {
                            while (*t2 >= '0' && *t2 <= '9') {
                                addr = addr * 10 + (unsigned)(*t2 - '0');
                                t2++; got = 1;
                            }
                        }
                        if (got) {
                            /* Emit "__at(addr) " as a parser-visible prefix;
                             * decimal form avoids lexer NUM parsing ambiguity. */
                            buf_puts(out, out_len, out_cap, "__at(");
                            emit_int(out, out_len, out_cap, (int)addr);
                            buf_puts(out, out_len, out_cap, ") ");
                        }
                    }
                }
                /* Silently ignored armcc pragmas: arkcc does not implement
                 *   diag_warning / diag_error
                 *   arm / thumb (arkcc always emits Thumb-2)
                 *   import(__use_no_semihosting)
                 *   push / pop pragma stack (only #pragma pack uses a stack)
                 *   O0/O1/O2/O3/Ospace/Otime/optimize
                 * We emit a comment in the output to preserve line numbers. */
                else if (r[0] == 'd' && r[1] == 'i' && r[2] == 'a' && r[3] == 'g' && r[4] == '_') {
                    handle_pragma_diag(r);
                }
                else if (ark_streq(r, "arm") || ark_streq(r, "thumb")) {
                    /* #pragma arm / #pragma thumb */
                }
                else if (r[0] == 'i' && r[1] == 'm' && r[2] == 'p' && r[3] == 'o' &&
                         r[4] == 'r' && r[5] == 't') {
                    /* #pragma import(...) */
                }
                else if (ark_streq(r, "push") || ark_streq(r, "pop")) {
                    /* #pragma push / #pragma pop: armcc pragma stack. Only
                     * #pragma pack has a real stack; others are ignored. */
                }
                else if ((r[0] == 'O' && (r[1] == 0 || r[1] == ' ' || r[1] == '\t')) ||
                         (r[0] == 'O' && (r[1] == 's' || r[1] == 't')) ||
                         (r[0] == 'o' && r[1] == 'p' && r[2] == 't' && r[3] == 'i' &&
                          r[4] == 'm' && r[5] == 'i' && r[6] == 'z' && r[7] == 'e')) {
                    /* #pragma O0/O1/O2/O3/Ospace/Otime/optimize */
#endif /* ARKCC_FEATURE_ARMCC_PRAGMA */
                }
            }
            else if (ark_streq(cmd, "line")) {
                /* #line: accepted and ignored (no line remapping yet). */
            }
            else if (cmd[0] && ACTIVE()) {
                /* armcc rejects directives it does not know; a bare '#' (null
                 * directive) is legal C and stays silent. Measured: the caret
                 * sits at column 2 and the id keeps the -D tag even though the
                 * message is reported as an error. */
                pp_diag(g_pp_line, 2, 11, DIAG_SEV_ERROR, 0, 0, 0);
            }
            // skip to EOL, but properly consume multi-line slash-star comments
            // that started on the directive line (e.g. ARM HAL style
            //   #endif slash-star LSI_VALUE star-slash slash-star-bang-bla multi-line star-slash
            // Without this, an unclosed slash-star on the directive line would
            // leak the comment body into the output as raw text.
            {
                while (*q) {
                    if (*q == '/' && q[1] == '*') {
                        q += 2;
                        while (*q && !(*q == '*' && q[1] == '/')) {
                            if (*q == '\n') line_no++;
                            q++;
                        }
                        if (*q) q += 2;
                    } else if (*q == '/' && q[1] == '/') {
                        while (*q && *q != '\n') q++;
                        break;
                    } else if (*q == '\n') {
                        break;
                    } else {
                        q++;
                    }
                }
            }
            if (*q == '\n') { q++; line_no++; }
            p = q;
            buf_putc(out, out_len, out_cap, '\n'); /* preserve directive line */
            continue;
        }

        /* String/char literals: copy verbatim so macro tokens inside are not
         * re-expanded and so we honor line counting inside multi-line strings. */
        if (*p == '"' || *p == '\'') {
            char qc = *p;
            if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
            p++;
            while (*p && *p != qc) {
                if (*p == '\\' && p[1]) {
                    if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
                    p++;
                }
                if (*p == '\n') line_no++;
                if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
                p++;
            }
            if (*p) {
                if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
                p++;
            }
            continue;
        }

        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') {
                if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
                p++;
            }
            continue;
        }
        if (*p == '/' && p[1] == '*') {
            if (ACTIVE()) { buf_putc(out, out_len, out_cap, *p); buf_putc(out, out_len, out_cap, p[1]); }
            p += 2;
            while (*p && !(*p == '*' && p[1] == '/')) {
                if (*p == '\n') line_no++;
                if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
                p++;
            }
            if (*p) {
                if (ACTIVE()) { buf_putc(out, out_len, out_cap, *p); buf_putc(out, out_len, out_cap, p[1]); }
                p += 2;
            }
            continue;
        }

        /* Identifier: try macro expansion. */
        if (is_alpha(*p)) {
            const char *st = p;
            char name[64];
            int len = 0;
            Macro *m;
            while (is_alnum(*p) && len < 63) { name[len++] = *p++; }
            name[len] = 0;
            if (!ACTIVE()) continue;
            /* C99 _Pragma("string"): equivalent to #pragma string. We currently
             * strip the _Pragma(...) form so the parser does not have to handle
             * it; only #pragma is dispatched above. Notably this drops GCC
             * diagnostic pragmas and _Pragma("pack(1)") style pack directives;
             * the latter is rare because most code uses #pragma pack(1)
             * directly, which handle_pragma_pack already handles. */
            if (ark_streq(name, "_Pragma")) {
                const char *r = p;
                while (*r == ' ' || *r == '\t') r++;
                if (*r == '(') {
                    int paren = 1;
                    r++;
                    /* Skip the parenthesized _Pragma argument up to matching ')'. */
                    while (*r && paren > 0) {
                        if (*r == '"') {
                            r++;
                            while (*r && *r != '"') {
                                if (*r == '\\' && r[1]) r += 2;
                                else r++;
                            }
                            if (*r) r++;
                        } else {
                            if (*r == '(') paren++;
                            else if (*r == ')') { paren--; if (paren == 0) { r++; break; } }
                            if (paren > 0) r++;
                        }
                    }
                    p = r;
                }
                continue;
            }
            /* armcc-style __asm void fn(){...}: copy the function body verbatim
             * because the inline-asm body contains tokens like "    #42" that
             * the macro expander would otherwise mis-handle as directives. */
            if (ark_streq(name, "__asm")) {
                /* Emit "__asm" verbatim and let the lexer/parser handle it. */
                int i;
                for (i = 0; i < len; i++) buf_putc(out, out_len, out_cap, st[i]);
                /* Look ahead for a function signature (...) ending in '{' and,
                 * if found, copy the entire braced body verbatim to avoid any
                 * expansion of operators/directives inside. */
                {
                    const char *r = p;
                    int saw_lparen = 0, saw_rparen = 0;
                    while (*r && *r != '{' && *r != ';' && *r != '"' && *r != '\'') {
                        if (*r == '(') saw_lparen = 1;
                        if (*r == ')') saw_rparen = 1;
                        r++;
                    }
                    if (*r == '{' && saw_lparen && saw_rparen) {
                        /* Copy up to '{', then copy through the matching '}' verbatim. */
                        while (p < r) {
                            if (*p == '\n') line_no++;
                            buf_putc(out, out_len, out_cap, *p);
                            p++;
                        }
                        /* Now *p == '{'; emit it and copy the brace-balanced body. */
                        buf_putc(out, out_len, out_cap, *p);
                        p++;
                        {
                            int depth = 1;
                            while (*p && depth > 0) {
                                if (*p == '{') depth++;
                                else if (*p == '}') {
                                    depth--;
                                    if (depth == 0) {
                                        buf_putc(out, out_len, out_cap, *p);
                                        p++;
                                        break;
                                    }
                                }
                                if (*p == '\n') line_no++;
                                buf_putc(out, out_len, out_cap, *p);
                                p++;
                            }
                        }
                        continue;
                    }
                }
                continue;
            }
            /* Predefined macros. */
            if (ark_streq(name, "__LINE__")) {
                emit_int(out, out_len, out_cap, line_no);
                continue;
            }
            if (ark_streq(name, "__FILE__")) {
                buf_putc(out, out_len, out_cap, '"');
                buf_puts(out, out_len, out_cap, g_main_file);
                buf_putc(out, out_len, out_cap, '"');
                continue;
            }
            if (ark_streq(name, "__STDC__")) {
                buf_putc(out, out_len, out_cap, '1');
                continue;
            }
            if (ark_streq(name, "__ARK_CC__")) {
                buf_putc(out, out_len, out_cap, '1');
                continue;
            }
            /* C99 6.10.8: __DATE__ is "Mmm dd yyyy"; __TIME__ is "hh:mm:ss". */
            if (ark_streq(name, "__DATE__")) {
                buf_putc(out, out_len, out_cap, '"');
                buf_puts(out, out_len, out_cap, g_build_date);
                buf_putc(out, out_len, out_cap, '"');
                continue;
            }
            if (ark_streq(name, "__TIME__")) {
                buf_putc(out, out_len, out_cap, '"');
                buf_puts(out, out_len, out_cap, g_build_time);
                buf_putc(out, out_len, out_cap, '"');
                continue;
            }
            m = find_macro(name);
            if (m && is_blue_painted(name)) {
                /* C99 blue paint: do not re-expand a macro already being expanded. */
                m = 0;
            }
            if (m) {
                if (m->is_func) {
                    const char *r = p;
                    while (*r == ' ' || *r == '\t') r++;
                    if (*r == '(') {
                        char *args[16] = {0};
                        int argc = 0;
                        char temp[16384];
                        int temp_len = 0;
                        r = parse_macro_args(r, args, &argc, 16);
                        /* Substitute into a temp buffer first. */
                        expand_func_macro(m, args, argc, temp, &temp_len, (int)sizeof(temp) - 1);
                        temp[temp_len] = 0;
                        /* C99 6.10.3.4: rescan the substituted token sequence
                         * for further macro expansion. The blue-paint stop-set
                         * is pushed during rescan to prevent infinite recursion;
                         * the rescan also lets nested macros expand. */
                        if (g_macro_depth < 64) {
                            const char *saved_blue = 0;
                            if (g_blue_top < PP_BLUE_STACK) {
                                g_blue_stack[g_blue_top] = m->name;
                                saved_blue = g_blue_stack[g_blue_top];
                                g_blue_top++;
                            }
                            g_macro_depth++;
                            process(temp, out, out_len, out_cap, 0);
                            g_macro_depth--;
                            if (g_blue_top > 0 && g_blue_stack[g_blue_top - 1] == saved_blue) {
                                g_blue_top--;
                            }
                        } else {
                            buf_puts(out, out_len, out_cap, temp);
                        }
                        p = r;
                        continue;
                    } else {
                        /* Function-like macro without trailing (): emit verbatim. */
                        int i;
                        for (i = 0; i < len; i++) buf_putc(out, out_len, out_cap, st[i]);
                        continue;
                    }
                }
                /* Object-like macro: rescan the body for further expansion.
                 *
                 * C99 6.10.3.4p1 rescans the replacement list *together with
                 * the rest of the source file*, so with
                 *     #define REAL(x) ((x)+1)
                 *     #define ALIAS REAL
                 *     ALIAS(2)
                 * the argument list belongs to REAL. Expanding the body on its
                 * own left the parentheses behind and turned ALIAS(2) into a
                 * call to a function named REAL (unresolved at link time).
                 * A body that is exactly one identifier is the shape vendor HAL
                 * headers use for clock-source aliases
                 * (__HAL_RCC_GET_I2C1_SOURCE -> __HAL_RCC_GET_I2C123_SOURCE),
                 * so follow such a chain and, when it ends at a function-like
                 * macro whose argument list follows in the input, expand it
                 * here. Any other body is expanded alone, as before. */
                {
                    Macro *fm = m;
                    int hops = 0;
                    while (fm && !fm->is_func && hops < 8) {
                        char idb[128];
                        int n = 0;
                        const char *b = fm->body;
                        Macro *next;
                        while (*b == ' ' || *b == '\t') b++;
                        while (is_alnum(*b) && n < (int)sizeof(idb) - 1) idb[n++] = *b++;
                        while (*b == ' ' || *b == '\t') b++;
                        idb[n] = 0;
                        if (n == 0 || *b != 0 || !is_alpha(idb[0])) break;
                        if (is_blue_painted(idb)) break;
                        next = find_macro(idb);
                        if (!next || next == fm) break;
                        fm = next;
                        hops++;
                    }
                    if (fm && fm != m && fm->is_func) {
                        const char *r2 = p;
                        while (*r2 == ' ' || *r2 == '\t') r2++;
                        if (*r2 == '(') {
                            char *args[16] = {0};
                            int argc = 0;
                            char temp[16384];
                            int temp_len = 0;
                            const char *saved_blue = 0;
                            r2 = parse_macro_args(r2, args, &argc, 16);
                            expand_func_macro(fm, args, argc, temp, &temp_len,
                                              (int)sizeof(temp) - 1);
                            temp[temp_len] = 0;
                            if (g_macro_depth < 64) {
                                if (g_blue_top < PP_BLUE_STACK) {
                                    g_blue_stack[g_blue_top] = fm->name;
                                    saved_blue = g_blue_stack[g_blue_top];
                                    g_blue_top++;
                                }
                                g_macro_depth++;
                                process(temp, out, out_len, out_cap, 0);
                                g_macro_depth--;
                                if (g_blue_top > 0 &&
                                    g_blue_stack[g_blue_top - 1] == saved_blue)
                                    g_blue_top--;
                            } else {
                                buf_puts(out, out_len, out_cap, temp);
                            }
                            p = r2;
                            continue;
                        }
                    }
                }
                if (g_macro_depth < 64) {
                    const char *saved_blue = 0;
                    if (g_blue_top < PP_BLUE_STACK) {
                        g_blue_stack[g_blue_top] = m->name;
                        saved_blue = g_blue_stack[g_blue_top];
                        g_blue_top++;
                    }
                    g_macro_depth++;
                    process(m->body, out, out_len, out_cap, 0);
                    g_macro_depth--;
                    if (g_blue_top > 0 && g_blue_stack[g_blue_top - 1] == saved_blue) {
                        g_blue_top--;
                    }
                } else {
                    buf_puts(out, out_len, out_cap, m->body);
                }
                continue;
            }
            {
                int i;
                for (i = 0; i < len; i++) buf_putc(out, out_len, out_cap, st[i]);
            }
            continue;
        }

        if (*p == '\n') line_no++;
        if (ACTIVE()) buf_putc(out, out_len, out_cap, *p);
        p++;
    }
    /* An #if left open when the file ends. Measured: armcc reports the
     * innermost open directive first and then walks outwards, each at column 2
     * of the #if line that opened it. */
    while (is_file && cond_top > 0) {
        cond_top--;
        pp_diag(cond_line[cond_top], 2, 37, DIAG_SEV_ERROR, 0, 0, 0);
    }
    g_pp_file = outer_file;
    g_cur_line = saved_line;
}

char *preproc_process(const char *src) {
    /* Static output buffer kept out of the memory pool so the bump allocator
     * has the full pool available for AST/IR/symbol tables. Single-threaded
     * compiler use means reentry between translation units is safe because
     * preproc_process completes before any pool consumer runs.
     * Placed in .ark_buf section so the linker can put it in AXI SRAM on STM32H743. */
    static char out_buf[PP_BUF_SIZE] __attribute__((section(".ark_buf")));
    char *out = out_buf;
    int out_len = 0;
    const char *src_ptr = src;
#if ARKCC_FEATURE_TRIGRAPH
    /* Apply trigraph translation in place before line-continuation joining.
     * The in-place pass is safe because every trigraph collapses 3 input
     * bytes into 1 output byte, so the write pointer never overtakes the
     * read pointer. The modified src is held by g_src (compiler.c) which
     * owns the storage; no other code uses the input after this. */
    if (g_config.enable_trigraph) {
        apply_trigraphs((char *)src);
        src_ptr = src;
    }
#endif
    /* Note: preproc_reset() is called by compiler.c before each translation unit. */
    g_cur_line = 1;
    g_macro_expand = 1;
    /* Cache build date/time once per translation unit so all __DATE__/__TIME__
     * references return the same value (C99 6.10.8.1). */
    ark_get_date_time(g_build_date, g_build_time);
    /* Predefined ARM Compiler 5 (armcc) macros so existing MDK/STM32 source
     * files compile unchanged. User code may #undef these to override.
     * __FILE__/__LINE__/__STDC__/__ARK_CC__ are handled inline above and
     * therefore do not need a macro entry here. */
    if (!find_macro("__arm__"))             add_object_macro("__arm__", "1");
    if (!find_macro("__thumb__"))           add_object_macro("__thumb__", "2");
    /* armcc 5 also accepts the no-underscore forms of __arm/__thumb. */
    if (!find_macro("__arm"))               add_object_macro("__arm", "1");
    if (!find_macro("__thumb"))             add_object_macro("__thumb", "2");
    if (!find_macro("__ARMCC_VERSION"))     add_object_macro("__ARMCC_VERSION", "5060750");
    /* armcc 5 also defines __CC_ARM (selector used by CMSIS cmsis_compiler.h
     * to distinguish armcc 4/5 from armclang 6+ which only sets __ARMCC_VERSION). */
    if (!find_macro("__CC_ARM"))            add_object_macro("__CC_ARM", "1");
    if (!find_macro("__TARGET_ARCH_7_M"))   add_object_macro("__TARGET_ARCH_7_M", "1");
    if (!find_macro("__TARGET_ARCH_7E_M"))  add_object_macro("__TARGET_ARCH_7E_M", "1");
    if (!find_macro("__TARGET_FPU_VFP"))    add_object_macro("__TARGET_FPU_VFP", "1");
    if (!find_macro("__BIG_ENDIAN"))        add_object_macro("__BIG_ENDIAN", "0");
    if (!find_macro("__LITTLE_ENDIAN__"))   add_object_macro("__LITTLE_ENDIAN__", "1");
    if (!find_macro("__sizeof_int"))        add_object_macro("__sizeof_int", "4");
    if (!find_macro("__sizeof_ptr"))        add_object_macro("__sizeof_ptr", "4");
    /* C99 conformance: arkcc implements a C99 freestanding subset, so hosted=0
     * even on host builds (we provide our own libc subset for STM32). */
    if (!find_macro("__STDC_VERSION__"))    add_object_macro("__STDC_VERSION__", "199901L");
    if (!find_macro("__STDC_HOSTED__"))     add_object_macro("__STDC_HOSTED__", "0");
    /* C99 <stddef.h> derives these types; arkcc defines size_t as unsigned int,
     * ptrdiff_t as int, and wchar_t as unsigned int, matching STM32 widths. */
    if (!find_macro("__SIZE_TYPE__"))       add_object_macro("__SIZE_TYPE__", "unsigned int");
    if (!find_macro("__PTRDIFF_TYPE__"))    add_object_macro("__PTRDIFF_TYPE__", "int");
    if (!find_macro("__WCHAR_TYPE__"))      add_object_macro("__WCHAR_TYPE__", "unsigned int");
    /* armcc 5 is based on EDG front-end and uses APCS interwork ABI. __EDG__
     * is defined for EDG-based compilers (armcc 5 is one). __APCS_INTERWORK
     * is set for ARM/Thumb interworking support (always enabled in our config). */
    if (!find_macro("__EDG__"))             add_object_macro("__EDG__", "1");
    if (!find_macro("__APCS_INTERWORK"))    add_object_macro("__APCS_INTERWORK", "1");
    process(src_ptr, out, &out_len, PP_BUF_SIZE - 1, 1);
    out[out_len] = 0;
    return out;
}

/* Return the list of files included during the last preproc_process call.
 * Both "... " and <...> forms are recorded; the array is filled into out
 * up to max_count entries and the actual count is returned. The caller
 * owns the storage; the strings themselves are owned by the preproc state. */
int preproc_get_included_files(const char **out, int max_count) {
    int i;
    int n = g_inc_file_count;
    if (n > max_count) n = max_count;
    for (i = 0; i < n; i++) out[i] = g_inc_files[i];
    return n;
}
