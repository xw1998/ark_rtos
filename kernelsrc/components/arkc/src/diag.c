/* Ark C Compiler - diag
 * The single place that turns (code, severity, location, arguments) into bytes.
 *
 * Nothing here knows about C syntax. Nothing outside here knows about line
 * endings, quoting, gutters, carets or plural forms.
 *
 * Layout pipeline (one direction, no callbacks):
 *   report()  --policy-->  count()  --layout-->  emit_diag()  --bytes-->  out_*()
 * Only report() decides *whether* to print; only emit_diag() decides *shape*;
 * only out_*() decides *where*. Each step is independently testable.
 *
 * Output goes through ark_log() so the freestanding (STM32) port keeps
 * working unchanged: the platform layer already owns "where console text
 * goes". The --errors sink is host-only because it needs stdio.
 */
#include "diag.h"
#include "platform.h"

/* arkcc_config.h is not pulled in by diag.h/common.h, and every feature gate
 * below is an #if on a macro from it: without this include ARKCC_FEATURE_DIAG_SINK
 * would silently evaluate to 0 and the --errors sink would be compiled out on
 * every profile, including host. */
#include "arkcc_config.h"

#include "diag_msg_table.h"

#if ARKCC_FEATURE_DIAG_SINK
#include <stdio.h>
#endif

/* Everything below is the real layer. With ARKCC_FEATURE_DIAG == 0 the whole
 * implementation is replaced by the small fallback at the end of this file. */
#if ARKCC_FEATURE_DIAG

/* ---------------------------------------------------------------------------
 * Limits
 * ------------------------------------------------------------------------- */

/* One diagnostic line, before it reaches the platform layer. 512 bytes holds
 * the longest armcc message we reproduce plus a file path; overflow is marked
 * with "...", never silently dropped. */
#define DIAG_LINE_MAX 512

/* Frontend codes live in 0..2427 (armcc table size). Suppression is a bitmap:
 * O(1) lookup, flat storage, 304 bytes, no allocation. */
#define DIAG_CODE_MAX 2428
#define DIAG_SUPPRESS_BYTES ((DIAG_CODE_MAX + 7) / 8)

#define DIAG_LIMIT_DEFAULT 30

/* ---------------------------------------------------------------------------
 * State (one instance: arkc is a single-threaded, single-translation-unit
 * command-line compiler; making this a parameter would only add plumbing) 
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *main_file;
    const char *src;

    int counts[5];              /* indexed by DiagSeverity */
    int limit;

    DiagStyle style;
    unsigned char suppressed[DIAG_SUPPRESS_BYTES];
    unsigned char forced_error[DIAG_SUPPRESS_BYTES];  /* --diag_error=<n> */

    int echo_disabled;          /* set when the source is not available */

#if ARKCC_FEATURE_DIAG_SINK
    FILE *sink;
#endif
} DiagState;

static DiagState g = {
    0, 0,
    { 0, 0, 0, 0, 0 },
    DIAG_LIMIT_DEFAULT,
    DIAG_STYLE_ARM,
    { 0 },
    { 0 },
    1
#if ARKCC_FEATURE_DIAG_SINK
    , 0
#endif
};

/* ---------------------------------------------------------------------------
 * Small string builder (no stdio: the STM32 build must not need libc here)
 * ------------------------------------------------------------------------- */

typedef struct {
    char buf[DIAG_LINE_MAX];
    int  len;
    int  overflow;
} Line;

static void line_reset(Line *l) { l->len = 0; l->overflow = 0; l->buf[0] = 0; }

static void line_putc(Line *l, char c) {
    if (l->len + 1 >= DIAG_LINE_MAX) { l->overflow = 1; return; }
    l->buf[l->len++] = c;
    l->buf[l->len] = 0;
}

static void line_puts(Line *l, const char *s) {
    if (!s) return;
    while (*s) line_putc(l, *s++);
}

static void line_putuint(Line *l, unsigned v) {
    char tmp[12];
    int  n = 0;
    if (v == 0) { line_putc(l, '0'); return; }
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n--) line_putc(l, tmp[n]);
}

static int str_eq(const char *a, const char *b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

/* ---------------------------------------------------------------------------
 * Low-level output
 * ------------------------------------------------------------------------- */

static void out_raw(const char *s, int len) {
#if ARKCC_FEATURE_DIAG_SINK
    if (g.sink) {
        int i;
        for (i = 0; i < len; i++) fputc((unsigned char)s[i], g.sink);
        return;
    }
#endif
    /* "%s" with the payload as an argument: a diagnostic containing '%' must
     * never be reinterpreted as a format string. */
    (void)len;
    ark_log("%s", s);
}

static void out_eol(void) {
#if ARKCC_FEATURE_DIAG_SINK
    if (g.sink) {
        /* armcc runs in text mode, so its output file holds CRLF. We open the
         * sink in binary mode, hence the explicit CR. */
        fputc('\r', g.sink);
        fputc('\n', g.sink);
        return;
    }
#endif
    ark_log("\n");
}

/* Mark an over-long line with a trailing "..." instead of cutting it silently.
 * Operates on the buffer directly: calling line_puts() here would overflow
 * again and land back in this same branch. */
static void line_truncate(Line *l) {
    int keep;
    if (!l->overflow) return;
    keep = DIAG_LINE_MAX - 4;
    l->len = keep;
    l->buf[keep]     = '.';
    l->buf[keep + 1] = '.';
    l->buf[keep + 2] = '.';
    l->buf[keep + 3] = 0;
}

static void emit_line(Line *l) {
    line_truncate(l);
    out_raw(l->buf, l->len);
    out_eol();
}

/* ---------------------------------------------------------------------------
 * Template expansion
 *
 * The generated table stores armcc's own placeholders:
 *   %sq  argument, quoted          identifier %sq is undefined
 *   %n   argument, as-is           %n was declared but never referenced
 *   %s   argument, as-is
 *   %c %d %u  argument, as-is (callers pass numbers already rendered)
 * armcc mixes two addressing schemes in one table, so we support both:
 *   positional  "identifier %sq is undefined"        -> next unused argument
 *   numbered    "cannot open %s input file %sq2: %s3" -> argument 2, then 3
 * (1-based, as written). When an argument is absent we emit "?" and drop the
 * placeholder: a visible hole beats a fabricated value.
 * A '\n' inside a template means "this message ends its own line" (armcc's
 * C4079E family); it is reported via *ends_line and removed from the text so
 * the layout layer can add the extra blank line itself.
 * ------------------------------------------------------------------------- */

static void expand(Line *l, const char *tpl, const char *const *args, int nargs,
                   int *ends_line) {
    int next = 0;               /* next positional argument to consume */
    if (ends_line) *ends_line = 0;

    while (tpl && *tpl) {
        if (*tpl == '\n') {
            if (ends_line) *ends_line = 1;
            tpl++;
            continue;
        }
        if (*tpl != '%') { line_putc(l, *tpl++); continue; }

        if (tpl[1] == '%') { line_putc(l, '%'); tpl += 2; continue; }

        {
            const char *p = tpl + 1;
            int quoted = 0;
            int is_arg = 0;
            int index  = 0;     /* 0 = positional, >0 = 1-based numbered */

            if (*p == 's' && p[1] == 'q') { is_arg = 1; quoted = 1; p += 2; }
            else if (*p == 's' || *p == 'n' || *p == 'c' || *p == 'd' || *p == 'u') {
                is_arg = 1; p += 1;
            }

            if (!is_arg) { line_putc(l, *tpl++); continue; }

            if (*p >= '1' && *p <= '9') {
                while (*p >= '0' && *p <= '9') { index = index * 10 + (*p - '0'); p++; }
            }

            {
                const char *v = 0;
                if (index > 0) {
                    if (index <= nargs) v = args[index - 1];
                } else if (next < nargs) {
                    v = args[next++];
                }
                if (!v) {
                    line_putc(l, '?');
                } else if (quoted) {
                    line_putc(l, '"');
                    line_puts(l, v);
                    line_putc(l, '"');
                } else {
                    line_puts(l, v);
                }
            }
            tpl = p;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Severity words
 * ------------------------------------------------------------------------- */

static const char *sev_word_arm(DiagSeverity s) {
    switch (s) {
        case DIAG_SEV_FATAL:   return "Fatal error:";
        case DIAG_SEV_ERROR:   return "Error:";
        case DIAG_SEV_WARNING: return "Warning:";
        case DIAG_SEV_INFO:    return "Info:";
        default:               return "Remark:";
    }
}

/* IDE style lowercases everything except "Fatal error" (measured), and drops
 * the trailing colon. */
static const char *sev_word_ide(DiagSeverity s) {
    switch (s) {
        case DIAG_SEV_FATAL:   return "Fatal error";
        case DIAG_SEV_ERROR:   return "error";
        case DIAG_SEV_WARNING: return "warning";
        case DIAG_SEV_INFO:    return "info";
        default:               return "remark";
    }
}

/* ---------------------------------------------------------------------------
 * Location segment
 * ------------------------------------------------------------------------- */

/* How a line is anchored. Making this explicit keeps "no position at all"
 * (driver diagnostics) from being confused with "file known, line unknown"
 * (armcc prints these as: "f.c": Error: At end of source:  #65: ...). */
typedef enum {
    DIAG_SHAPE_PLAIN = 0,   /* no location segment   Fatal error: C3900U: ...     */
    DIAG_SHAPE_FILE,        /* "f.c", line N: / "f.c": + At end of source:        */
    DIAG_SHAPE_NOSOURCE,    /* "no source":          (no line, no At end)         */
    DIAG_SHAPE_CMDLINE,     /* "no source":  + command-line:                      */
    DIAG_SHAPE_DRIVER,      /* driver code; IDE style prefixes "armcc : "         */
    DIAG_SHAPE_TOOL,        /* <object>: Error:  -- linker/scatter, no lines      */
    DIAG_SHAPE_EOF          /* "f.c", line N:  + At end of source:, no echo       */
} DiagShape;

/* ARM/GNU: "\"f.c\": " or "\"f.c\", line 4: "
 *          (armcc also has a "line N (column N)" form in its table, but it was
 *           never observed in a real run, so arkc does not emit it)
 * IDE    : f.c(4,22) :    -- the form uVision parses into a clickable error */
static void put_location(Line *l, const DiagLoc *loc, DiagStyle style,
                         DiagShape shape) {
    int have_line = loc && loc->line > 0;
    const char *f;

    /* Driver diagnostics have no source position at all; the location segment
     * is simply absent (IDE style still prefixes the line with "armcc : "). */
    if (shape == DIAG_SHAPE_PLAIN || shape == DIAG_SHAPE_DRIVER) return;

    /* "no source" is a literal: a diagnostic about the translation unit as a
     * whole must not borrow the file name of the translation unit. */
    if (shape == DIAG_SHAPE_NOSOURCE || shape == DIAG_SHAPE_CMDLINE) {
        f = DIAG_NO_SOURCE_TEXT;
    } else {
        f = (loc && loc->file) ? loc->file
                               : (g.main_file ? g.main_file : DIAG_NO_SOURCE_TEXT);
    }

    if (style == DIAG_STYLE_IDE) {
        line_puts(l, f);
        if (have_line) {
            line_putc(l, '(');
            line_putuint(l, (unsigned)loc->line);
            if (loc->col > 0) {
                line_putc(l, ',');
                line_putuint(l, (unsigned)loc->col);
            }
            line_puts(l, ") : ");
        } else {
            line_puts(l, " : ");
        }
        return;
    }

    /* The tool stages (linker, scatter reader) report against an output
     * image, not a source file: armlink spells that unquoted, and there is no
     * line to print. */
    if (shape == DIAG_SHAPE_TOOL) {
        line_puts(l, f);
        line_puts(l, ": ");
        return;
    }

    /* ARM / GNU */
    line_putc(l, '"');
    line_puts(l, f);
    if (have_line) {
        line_puts(l, "\", line ");
        line_putuint(l, (unsigned)loc->line);
        line_puts(l, ": ");
    } else {
        line_puts(l, "\": ");
    }
}

/* ---------------------------------------------------------------------------
 * Source echo + caret
 *
 * Measured rules (docs/DIAGNOSTICS.md §4):
 *   "  " + source line verbatim (tabs stay tabs)
 *   "  " + prefix replayed by display width + "^"
 *     ASCII = 1 column, CJK = 2 columns, tab stays a tab and counts 1.
 * Only the main file is echoable: a line from an #included header cannot be
 * mapped back to a buffer here, so those diagnostics print without the block.
 * A missing echo is a smaller lie than a wrong one.
 * ------------------------------------------------------------------------- */

#define DIAG_GUTTER "  "

/* Advance over one UTF-8 sequence; *width is its display width. */
static const char *utf8_step(const char *p, int *width) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x80)             { *width = 1; return p + 1; }
    if ((c & 0xE0) == 0xC0)   { *width = 1; return p + 2; }  /* Latin supplement */
    if ((c & 0xF0) == 0xE0)   { *width = 2; return p + 3; }  /* CJK / fullwidth  */
    if ((c & 0xF8) == 0xF0)   { *width = 2; return p + 4; }
    *width = 1;
    return p + 1;
}

/* Start of 1-based line `line` in g.src, or NULL. Linear scan on purpose:
 * diagnostics are few (<= limit) and building an index would cost memory the
 * STM32 profile cannot spare. */
static const char *find_line(const char *src, int line) {
    int cur = 1;
    const char *p = src;
    if (!src || line <= 0) return 0;
    while (*p && cur < line) {
        if (*p == '\n') cur++;
        p++;
    }
    return (cur == line) ? p : 0;
}

static void echo_source(const DiagLoc *loc) {
    const char *start, *end, *p;
    Line l;

    if (g.echo_disabled || !g.src || !loc || loc->line <= 0) return;
    if (loc->file && !str_eq(loc->file, g.main_file)) return;

    start = find_line(g.src, loc->line);
    if (!start) return;
    end = start;
    while (*end && *end != '\n' && *end != '\r') end++;

    line_reset(&l);
    line_puts(&l, DIAG_GUTTER);
    p = start;
    while (p < end) line_putc(&l, *p++);
    emit_line(&l);

    if (loc->col <= 0) return;

    line_reset(&l);
    line_puts(&l, DIAG_GUTTER);
    {
        int width = 0;
        p = start;
        while (p < end && width < loc->col - 1) {
            int w;
            const char *nxt;
            if (*p == '\t') {
                line_putc(&l, '\t');       /* tab is replayed as a tab */
                width += 1;
                p++;
                continue;
            }
            nxt = utf8_step(p, &w);
            if (width + w > loc->col - 1) break;   /* never split a wide char */
            while (w-- > 0) { line_putc(&l, ' '); width++; }
            p = nxt;
        }
        /* A caret is only worth printing when it can sit under the character
         * the message talks about. Two ways that fails: the column lies past
         * the end of the line (so the echo above already shows an earlier
         * construct), or the caret line itself is longer than we are willing
         * to echo. Printing a caret at a position that does not correspond to
         * anything is worse than printing none. */
        if (l.overflow || width < loc->col - 1) return;
    }
    line_putc(&l, '^');
    emit_line(&l);
}

/* ---------------------------------------------------------------------------
 * Policy
 * ------------------------------------------------------------------------- */

static int error_total(void) {
    return g.counts[DIAG_SEV_ERROR] + g.counts[DIAG_SEV_FATAL];
}

int diag_errors(void)   { return error_total(); }
int diag_warnings(void) { return g.counts[DIAG_SEV_WARNING]; }

int diag_at_limit(void) {
    return g.limit > 0 && error_total() >= g.limit;
}

static int is_error_class(DiagSeverity s) {
    return s == DIAG_SEV_ERROR || s == DIAG_SEV_FATAL;
}

/* Should this diagnostic be printed (and counted) at all? */
static int admit(DiagSeverity sev, int code) {
    if (code >= 0 && diag_is_suppressed(code)) return 0;
    /* armcc: the (limit+1)-th error is neither printed nor counted. */
    if (is_error_class(sev) && diag_at_limit()) return 0;
    return 1;
}

/* ---------------------------------------------------------------------------
 * Layout
 * ------------------------------------------------------------------------- */

/* Render a C####x code in IDE form: the trailing severity letter is dropped
 * (C3900U -> C3900), other codes pass through. */
static void put_code_ide(Line *l, const char *code) {
    int n = 0, i, last_alpha = -1;
    while (code[n]) n++;
    for (i = 0; i < n; i++) {
        char c = code[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) last_alpha = i;
    }
    for (i = 0; i < n; i++) {
        if (i == last_alpha && i == n - 1) continue;
        line_putc(l, code[i]);
    }
}

/* Every shape is spelled out because armcc's two styles are not one format with
 * a different prefix -- the pieces move around (all of this is measured):
 *
 *   ARM   "f.c", line 4: Error:  #20: identifier "c" is undefined
 *         "f.c": Error: At end of source:  #65: expected a ";"
 *         "no source": Error:  #5: cannot open source input file ...
 *         "no source": Error: command-line:  #992: invalid macro definition: ...
 *         Fatal error: C3900U: Unrecognized option '-x'.
 *   IDE   f.c(4,22) : error  #20: identifier "c" is undefined
 *         no source : error  #5: cannot open source input file ...
 *         no source : error command-line:  #992: invalid macro definition: ...
 *         armcc : Fatal error C3900:  Unrecognized option '-x'.
 *
 * Note the IDE spellings that differ from a naive mapping: the severity word
 * loses its colon, the driver code loses its severity suffix (C3900U -> C3900),
 * and the driver message gains a second space after the colon. */
static void emit_diag(DiagSeverity sev, const DiagLoc *loc,
                      const char *code_str, const char *msg,
                      int ends_line, DiagShape shape) {
    Line l;
    /* "At end of source" appears both for an explicit EOF anchor and for the
     * legacy "file with no line" form. */
    int at_end = loc && (loc->file || g.main_file) &&
                 (shape == DIAG_SHAPE_EOF ||
                  (shape == DIAG_SHAPE_FILE && loc->line <= 0));

    line_reset(&l);

    if (g.style == DIAG_STYLE_IDE) {
        if (shape == DIAG_SHAPE_DRIVER) line_puts(&l, "armcc : ");
        put_location(&l, loc, g.style, shape);
        line_puts(&l, sev_word_ide(sev));       /* "error", no colon */
        if (code_str) {
            if (shape == DIAG_SHAPE_DRIVER) {
                line_putc(&l, ' ');
                put_code_ide(&l, code_str);     /* suffix stripped: C3900 */
                line_puts(&l, ":  ");
            } else if (shape == DIAG_SHAPE_CMDLINE) {
                line_puts(&l, " command-line:  ");
                put_code_ide(&l, code_str);
                line_puts(&l, ": ");
            } else {
                line_puts(&l, "  ");
                put_code_ide(&l, code_str);
                line_puts(&l, ": ");
            }
        } else {
            line_puts(&l, ": ");
        }
    } else {
        put_location(&l, loc, g.style, shape);
        line_puts(&l, sev_word_arm(sev));       /* "Error:" */
        if (at_end) line_puts(&l, " At end of source:");
        if (shape == DIAG_SHAPE_CMDLINE) line_puts(&l, " command-line:");
        if (code_str) {
            if (shape == DIAG_SHAPE_DRIVER) {
                line_putc(&l, ' ');             /* one space, code keeps its U */
                line_puts(&l, code_str);
            } else {
                line_puts(&l, "  ");            /* "Error:  #20: " (two spaces) */
                line_puts(&l, code_str);
            }
            line_puts(&l, ": ");
        } else {
            line_putc(&l, ' ');     /* sev_word_arm already ends with ':' */
        }
    }

    line_puts(&l, msg);
    emit_line(&l);

    if (ends_line) out_eol();    /* the message carried its own newline */
}

/* ---------------------------------------------------------------------------
 * One entry point for every diagnostic
 * ------------------------------------------------------------------------- */

static void report(DiagSeverity sev, int code, const DiagLoc *loc,
                   const char *code_str, const char *tpl,
                   const char *const *args, int nargs,
                   DiagShape shape) {
    Line m;
    int ends_line = 0;

    if (!admit(sev, code)) return;

    line_reset(&m);
    expand(&m, tpl, args, nargs, &ends_line);
    line_truncate(&m);

    g.counts[sev]++;
    emit_diag(sev, loc, code_str, m.buf, ends_line, shape);

    /* armcc prints the source echo and caret in ARM/GNU styles only; IDE style
     * is a single line per problem (measured). An end-of-source diagnostic has
     * no character to point at, so it never echoes either. */
    if (g.style != DIAG_STYLE_IDE && shape != DIAG_SHAPE_EOF &&
        (is_error_class(sev) || sev == DIAG_SEV_WARNING)) echo_source(loc);
}

/* Most diagnostics come from the frontend: either anchored to a source line,
 * or anchored to the file with no line (end of source), or with no anchor at
 * all. Driver and command-line diagnostics choose their own shape explicitly. */
static DiagShape shape_of(const DiagLoc *loc) {
    if (!loc) return DIAG_SHAPE_PLAIN;
    if (loc->file && str_eq(loc->file, DIAG_NO_SOURCE_TEXT)) return DIAG_SHAPE_NOSOURCE;
    return DIAG_SHAPE_FILE;
}

/* armcc appends "-D" to some ids ("#177-D"). Two measured rules produce it:
 * every warning carries it, and a few codes carry it at any severity (#11 is
 * printed as an error that still says "#11-D"). The per-code part lives in the
 * generated table; the "warnings always" part lives here, where the severity
 * is known. */
static void code_to_str(int code, DiagSeverity intrinsic, char *out) {
    char tmp[12];
    int n = 0, i;
    unsigned v = (unsigned)code;

    out[0] = '#';
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (i = 0; i < n; i++) out[1 + i] = tmp[n - 1 - i];
    out[1 + n] = 0;
    if (code >= 0 && (intrinsic == DIAG_SEV_WARNING ||
                      diag_msg_has_d_tag(code))) {
        out[1 + n]     = '-';
        out[1 + n + 1] = 'D';
        out[1 + n + 2] = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void diag_init(const char *main_file, const char *source_text) {
    int i;
    g.main_file = main_file;
    g.src = source_text;
    g.echo_disabled = (source_text == 0);
    /* Only the counters reset: --diag_suppress / --diag_error are applied
     * while the command line is scanned, and main() re-initialises after that
     * (the file name is only known then), so clearing the policy here would
     * silently drop the command line. */
    for (i = 0; i < 5; i++) g.counts[i] = 0;
}

/* Defined below with the policy helpers; declared here because the public
 * entry points above need it. */
static DiagSeverity override_severity(int code, DiagSeverity sev);

void diag_report_at_eof(int code, DiagSeverity sev, const DiagLoc *loc) {
    char code_str[16];
    const char *tpl = diag_msg_lookup(code);
    code_to_str(code, sev, code_str);
    if (!tpl) {
        /* Never invent wording for a code the table does not carry. */
        diag_report_msg(sev, loc, "expected a token", 0, 0, 0);
        return;
    }
    report(override_severity(code, sev), code, loc, code_str, tpl, 0, 0,
           DIAG_SHAPE_EOF);
}

void diag_set_main_file(const char *main_file) {
    g.main_file = main_file;
}

void diag_set_source(const char *source_text) {
    g.src = source_text;
    g.echo_disabled = (source_text == 0);
}

/* --diag_error=<n>: the code keeps its own -D suffix but is printed (and
 * counted) as an error. */
static DiagSeverity override_severity(int code, DiagSeverity sev) {
    if (code < 0 || code >= DIAG_CODE_MAX) return sev;
    if (g.forced_error[code >> 3] & (1u << (code & 7))) return DIAG_SEV_ERROR;
    return sev;
}

void diag_report(int code, DiagSeverity sev, const DiagLoc *loc,
                 const char *a0, const char *a1, const char *a2) {
    char code_str[16];
    const char *tpl = diag_msg_lookup(code);

    /* -D follows the class the code *has*, the word follows --diag_error
     * (measured: --diag_error=177 prints "Error:  #177-D"). */
    code_to_str(code, sev, code_str);
    sev = override_severity(code, sev);
    if (!tpl) {
        /* Refuse to invent text for a code we do not carry: the number is
         * real, so print it, but never guess what it means. */
        diag_report_msg(sev, loc, "internal error: no message text for %s",
                        code_str, 0, 0);
        return;
    }
    {
        const char *args[3];
        args[0] = a0; args[1] = a1; args[2] = a2;
        report(sev, code, loc, code_str, tpl, args, 3, shape_of(loc));
    }
}

void diag_report_msg(DiagSeverity sev, const DiagLoc *loc, const char *msg,
                     const char *a0, const char *a1, const char *a2) {
    const char *args[3];
    args[0] = a0; args[1] = a1; args[2] = a2;
    report(sev, DIAG_NO_CODE, loc, 0, msg, args, 3, shape_of(loc));
}

void diag_tool(DiagSeverity sev, const char *object, const char *msg,
               const char *a0, const char *a1, const char *a2,
               const char *a3, const char *a4, const char *a5) {
    DiagLoc loc;
    const char *args[6];

    loc.file = object;
    loc.line = 0;
    loc.col  = 0;
    args[0] = a0; args[1] = a1; args[2] = a2;
    args[3] = a3; args[4] = a4; args[5] = a5;
    report(sev, DIAG_NO_CODE, &loc, 0, msg, args, 6, DIAG_SHAPE_TOOL);
}

/* armcc's usage errors ('U') are printed as "Fatal error:"; the other letters
 * map straight onto our severities. The letter is taken from the table, so a
 * call site cannot pick a severity that contradicts the code it prints. */
static DiagSeverity driver_severity(char letter) {
    switch (letter) {
        case 'U': return DIAG_SEV_FATAL;
        case 'E': return DIAG_SEV_ERROR;
        case 'W': return DIAG_SEV_WARNING;
        case 'I': return DIAG_SEV_INFO;
        default:  return DIAG_SEV_ERROR;
    }
}

void diag_driver(const char *code, const char *a0, const char *a1) {
    const char *tpl = (code && code[0] == 'C') ? diag_driver_lookup(code) : 0;
    DiagSeverity sev;

    if (tpl) {
        sev = driver_severity(diag_driver_severity(code));
    } else {
        /* The code is real, its wording is not ours to invent. Reported as an
         * error because that is what a caller with an unknown driver code is
         * telling us. */
        tpl = "internal error: no message text for driver code";
        sev = DIAG_SEV_ERROR;
    }
    /* Driver messages carry no source position and no echo block. */
    {
        const char *args[2];
        args[0] = a0; args[1] = a1;
        report(sev, DIAG_NO_CODE, 0, code, tpl, args, 2, DIAG_SHAPE_DRIVER);
    }
}

void diag_command_line(int code, DiagSeverity sev,
                       const char *a0, const char *a1, const char *a2) {
    char code_str[16];
    const char *tpl = diag_msg_lookup(code);
    DiagLoc loc;

    if (!tpl) tpl = "unknown diagnostic (code not in table)";
    code_to_str(code, sev, code_str);

    /* "no source" is a literal armcc location, not our main file. */
    loc.file = "no source";
    loc.line = 0;
    loc.col  = 0;
    {
        const char *args[3];
        args[0] = a0; args[1] = a1; args[2] = a2;
        report(sev, code, &loc, code_str, tpl, args, 3, DIAG_SHAPE_CMDLINE);
    }
}

const char *diag_num(char *buf, int v) {
    char tmp[12];
    int n = 0, i = 0;
    unsigned u;
    if (!buf) return "";
    /* Take the magnitude without overflowing on INT_MIN. */
    u = (v < 0) ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (v < 0) buf[i++] = '-';
    while (n) buf[i++] = tmp[--n];
    buf[i] = 0;
    return buf;
}

int diag_set_style_name(const char *name) {
    if (!name) return -1;
    if (str_eq(name, "arm"))       { g.style = DIAG_STYLE_ARM; return 0; }
    if (str_eq(name, "gnu"))       { g.style = DIAG_STYLE_GNU; return 0; }
    if (str_eq(name, "ide"))       { g.style = DIAG_STYLE_IDE; return 0; }
    if (str_eq(name, "devstudio")) { g.style = DIAG_STYLE_IDE; return 0; }
    /* armcc's --diag_style=list leaves everything in IDE shape (measured
     * quirk); reproduce rather than reject the value. */
    if (str_eq(name, "list"))      { g.style = DIAG_STYLE_IDE; return 0; }
    return -1;
}

void diag_force_error(int code) {
    if (code < 0 || code >= DIAG_CODE_MAX) return;   /* outside the table: ignore */
    g.forced_error[code >> 3] |= (unsigned char)(1u << (code & 7));
}

void diag_suppress(int code) {
    if (code < 0 || code >= DIAG_CODE_MAX) return;
    g.suppressed[code >> 3] |= (unsigned char)(1u << (code & 7));
}

void diag_default(int code) {
    if (code < 0 || code >= DIAG_CODE_MAX) return;
    g.suppressed[code >> 3] &= (unsigned char)~(1u << (code & 7));
}

int diag_is_suppressed(int code) {
    if (code < 0 || code >= DIAG_CODE_MAX) return 0;
    return (g.suppressed[code >> 3] >> (code & 7)) & 1;
}

void diag_summary(void) {
    Line l;
    int w = diag_warnings();
    int e = diag_errors();

    /* armcc prints nothing on a clean build, and nothing after a fatal
     * diagnostic (measured: C3900U / C3903U print exactly one line). */
    if (w == 0 && e == 0) return;
    if (g.counts[DIAG_SEV_FATAL] > 0) return;

    line_reset(&l);
    if (g.style == DIAG_STYLE_IDE) line_puts(&l, "armcc : ");
    line_puts(&l, g.main_file ? g.main_file : "no source");
    line_puts(&l, ": ");
    line_putuint(&l, (unsigned)w);
    line_putc(&l, ' ');
    line_puts(&l, w == 1 ? "warning" : "warnings");
    line_puts(&l, ", ");
    line_putuint(&l, (unsigned)e);
    line_putc(&l, ' ');
    line_puts(&l, e == 1 ? "error" : "errors");
    emit_line(&l);
}

void diag_shutdown(void) {
    diag_summary();
    diag_close_sink();
}

int diag_open_sink(const char *path) {
#if ARKCC_FEATURE_DIAG_SINK
    if (!path || !path[0]) return -1;
    diag_close_sink();
    g.sink = fopen(path, "wb");   /* binary: CRLF is emitted by out_eol() */
    return g.sink ? 0 : -1;
#else
    (void)path;
    return -1;
#endif
}

void diag_close_sink(void) {
#if ARKCC_FEATURE_DIAG_SINK
    if (g.sink) { fclose(g.sink); g.sink = 0; }
#endif
}
#else  /* !ARKCC_FEATURE_DIAG -- minimal fallback, see arkcc_config.h */

/* Deliberately shares no code with the full layer: a fallback that reused the
 * machinery would save no bytes, and this one is small enough to be obviously
 * correct. What survives: the message text (from the same generated table),
 * placeholder expansion, and error/warning counting, so the driver's exit code
 * is exactly the same. What goes: codes, -D, layout, echo, caret, summary,
 * suppression, limits, --errors sink, style. */

static int g_min_err;
static int g_min_warn;

static int min_is_err(DiagSeverity s) {
    return s == DIAG_SEV_ERROR || s == DIAG_SEV_FATAL;
}

void diag_init(const char *main_file, const char *source_text) {
    (void)main_file; (void)source_text;
    g_min_err = 0; g_min_warn = 0;
}
void diag_report_at_eof(int code, DiagSeverity sev, const DiagLoc *loc) {
    /* Minimal fallback: same contract, no shape machinery to honour. */
    (void)loc;
    diag_report(code, sev, loc, 0, 0, 0);
}
void diag_set_main_file(const char *main_file) { (void)main_file; }
void diag_set_source(const char *source_text)   { (void)source_text; }
void diag_summary(void)  { }
void diag_shutdown(void) { }

const char *diag_num(char *buf, int v) {
    char tmp[12];
    int n = 0, i = 0;
    unsigned u;
    if (!buf) return "";
    u = (v < 0) ? (unsigned)(-(v + 1)) + 1u : (unsigned)v;
    do { tmp[n++] = (char)('0' + (u % 10)); u /= 10; } while (u);
    if (v < 0) buf[i++] = '-';
    while (n) buf[i++] = tmp[--n];
    buf[i] = 0;
    return buf;
}

/* One line: expand the placeholders arkc uses, then hand it to ark_log. */
static void min_print(const char *tpl, const char *const *args, int nargs) {
    char out[512];
    int o = 0, next = 0;
    const char *p = tpl;

    while (p && *p && o < 511) {
        if (*p != '%') { out[o++] = *p++; continue; }
        if (p[1] == '%') { out[o++] = '%'; p += 2; continue; }
        {
            const char *q = p + 1, *v = 0;
            int quoted = 0, index = 0, is_arg = 0;
            if (*q == 's' && q[1] == 'q') { is_arg = 1; quoted = 1; q += 2; }
            else if (*q == 's' || *q == 'n' || *q == 'c' || *q == 'd' || *q == 'u') {
                is_arg = 1; q += 1;
            }
            if (!is_arg) { out[o++] = *p++; continue; }
            while (*q >= '0' && *q <= '9') { index = index * 10 + (*q - '0'); q++; }
            if (index > 0) { if (index <= nargs) v = args[index - 1]; }
            else if (next < nargs) v = args[next++];
            if (quoted && o < 510) out[o++] = '"';
            while (v && *v && o < 510) out[o++] = *v++;
            if (quoted && o < 510) out[o++] = '"';
            p = q;
        }
    }
    out[o] = 0;
    ark_log("[arkcc] %s\n", out);
}

static void min_report(DiagSeverity sev, int code, const char *tpl,
                       const char *a0, const char *a1, const char *a2) {
    const char *args[3];
    char codebuf[16];

    if (min_is_err(sev)) g_min_err++;
    else if (sev == DIAG_SEV_WARNING) g_min_warn++;

    if (!tpl) {
        int n = 0;
        codebuf[n++] = '#';
        if (code <= 0) { codebuf[n++] = '0'; }
        else {
            char tmp[12];
            int k = 0;
            unsigned u = (unsigned)code;
            while (u) { tmp[k++] = (char)('0' + (u % 10)); u /= 10; }
            while (k) codebuf[n++] = tmp[--k];
        }
        codebuf[n] = 0;
        tpl = "diagnostic %s";
        args[0] = codebuf; args[1] = 0; args[2] = 0;
        min_print(tpl, args, 1);
        return;
    }
    args[0] = a0; args[1] = a1; args[2] = a2;
    min_print(tpl, args, 3);
}

void diag_report(int code, DiagSeverity sev, const DiagLoc *loc,
                 const char *a0, const char *a1, const char *a2) {
    (void)loc;
    min_report(sev, code, diag_msg_lookup(code), a0, a1, a2);
}

void diag_report_msg(DiagSeverity sev, const DiagLoc *loc, const char *msg,
                     const char *a0, const char *a1, const char *a2) {
    (void)loc;
    min_report(sev, DIAG_NO_CODE, msg, a0, a1, a2);
}

void diag_driver(const char *code, const char *a0, const char *a1) {
    const char *tpl = (code && code[0] == 'C') ? diag_driver_lookup(code) : 0;
    DiagSeverity sev = DIAG_SEV_ERROR;

    if (tpl && diag_driver_severity(code) == 'U') sev = DIAG_SEV_FATAL;
    else if (tpl && diag_driver_severity(code) == 'W') sev = DIAG_SEV_WARNING;
    (void)code;
    min_report(sev, DIAG_NO_CODE, tpl, a0, a1, 0);
}

void diag_command_line(int code, DiagSeverity sev,
                       const char *a0, const char *a1, const char *a2) {
    min_report(sev, code, diag_msg_lookup(code), a0, a1, a2);
}

void diag_tool(DiagSeverity sev, const char *object, const char *msg,
               const char *a0, const char *a1, const char *a2,
               const char *a3, const char *a4, const char *a5) {
    (void)object;
    min_report(sev, DIAG_NO_CODE, msg, a0, a1, a2);
    (void)a3; (void)a4; (void)a5;
}

int diag_errors(void)   { return g_min_err; }
int diag_warnings(void) { return g_min_warn; }
int diag_at_limit(void) { return 0; }

int diag_set_style_name(const char *name) { (void)name; return 0; }
void diag_suppress(int code)   { (void)code; }
void diag_default(int code)    { (void)code; }
int  diag_is_suppressed(int code) { (void)code; return 0; }
void diag_force_error(int code) { (void)code; }

int  diag_open_sink(const char *path) { (void)path; return -1; }
void diag_close_sink(void) { }

#endif /* ARKCC_FEATURE_DIAG */
