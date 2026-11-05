/* Ark C Compiler - diag.h
 * Diagnostics: one place that knows how a diagnostic is *presented*.
 *
 * Why this module exists
 * ---------------------
 * Before this file, every stage printed its own line
 *   "[parser] line 3: unexpected token (got token kind 54)"
 * which leaks internal enums, carries no code, cannot be parsed by an IDE,
 * and cannot be suppressed. The stages still own the *decision* to diagnose;
 * they no longer own the *text*.
 *
 * Design rules (kept deliberately small so it stays maintainable)
 * --------------------------------------------------------------
 * 1. Stages call diag_report*() with a code, a location and at most two
 *    arguments. They never format, never print, never count.
 * 2. Message templates live in a generated table (src/diag_msg_table.c), not in
 *    this file and not in the stages. Regenerate with tools/gen_diag_table.py;
 *    the table is derived from measured armcc 5.06u7 message bytes, so it is
 *    reproducible rather than hand-copied.
 * 3. Only codes with real evidence are emitted with an armcc number. An
 *    arkc-specific message keeps DIAG_NO_CODE and is printed without a code
 *    rather than being mapped to a guessed one ("no wrong answers").
 * 4. Output shape is a property of the *style*, decided once per run, not
 *    scattered over call sites (see DiagStyle below).
 *
 * Presentation contract (measured, see docs/DIAGNOSTICS.md)
 * --------------------------------------------------------
 *   ARM style   "f.c", line 4: Error:  #20: identifier "c" is undefined
 *                 int b = c + ;
 *                         ^
 *               f.c: 0 warnings, 2 errors
 *   IDE style   f.c(4,22) : error  #20: identifier "c" is undefined
 *               armcc : f.c: 0 warnings, 2 errors
 *
 * Template placeholders (%sq, %n, %s) are expanded by diag.c; see diag.c
 * "Template expansion" for the exact rule set.
 */
#ifndef ARK_DIAG_H
#define ARK_DIAG_H

#include "common.h"

/* Severity, expressed semantically.
 * Deliberately NOT armcc's internal numbering (4/5/6/7): those numbers are an
 * implementation detail of the original emitter, and copying them would make
 * this table drift with armcc's internals instead of with our meaning. */
typedef enum {
    DIAG_SEV_REMARK  = 0,
    DIAG_SEV_INFO    = 1,
    DIAG_SEV_WARNING = 2,
    DIAG_SEV_ERROR   = 3,
    DIAG_SEV_FATAL   = 4
} DiagSeverity;

/* Output shape. Chosen once per run (a run-global property, because armcc
 * applies --diag_style to everything printed, including text emitted before
 * the option was seen). */
typedef enum {
    DIAG_STYLE_ARM = 0,   /* armcc default */
    DIAG_STYLE_GNU,       /* same as ARM except unquoted "no source" */
    DIAG_STYLE_IDE        /* uVision / Visual Studio style, with column */
} DiagStyle;

/* No armcc code: print the message without a code. */
#define DIAG_NO_CODE (-1)

/* armcc's literal location for diagnostics about the translation unit as a
 * whole ("no source": Error:  #5: ...). Pass it as DiagLoc.file; the
 * formatter recognises it and will not replace it with the main file name. */
#define DIAG_NO_SOURCE_TEXT "no source"

/* Source position. Any field may be absent:
 *   file == NULL  -> the translation unit's main file
 *   line == 0     -> no position (armcc prints "At end of source: ")
 *   col  == 0     -> column unknown (ARM style still works, IDE style omits it)
 * file "no source" is a *real* armcc location (driver-level diagnostics); it
 * is written literally, so it must not be replaced by the main file name. */
typedef struct {
    const char *file;
    int         line;
    int         col;
} DiagLoc;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* main_file  : name used for loc->file == NULL and for the summary line.
 * source_text: the translation unit source, for echoing + caret. NULL disables
 *              echoing (diagnostics still print, just without the two lines).
 *              The pointer must stay valid until diag_shutdown(). */
void diag_init(const char *main_file, const char *source_text);

/* Rename the translation unit. Used by a link-only run, where the "file" a
 * user can act on is the output image, not a source file. Keeps the counters. */
void diag_set_main_file(const char *main_file);

/* Swap the buffer used for echoing and caret positioning, keeping the counters.
 * The preprocessor diagnoses against the raw source (that is the text the user
 * edits), the later stages against the preprocessed text (that is the text
 * whose lines the lexer counts), so the buffer changes exactly once per file. */
void diag_set_source(const char *source_text);

/* Print the summary line (unless clean) and close any --errors sink. */
void diag_shutdown(void);

/* Print the summary line only (idempotent; used on early-exit paths). */
void diag_summary(void);

/* ---------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------- */

/* Every entry point below takes up to three message arguments (NULL =
 * absent). Three is armcc's own worst case: #5 needs "(source) (file)
 * (reason)". Arguments are matched to placeholders either positionally
 * ("identifier %sq is undefined") or by 1-based index ("%sq2: %s3"), because
 * armcc's table uses both forms. */

/* Frontend diagnostic with an armcc code (#<n>). Template comes from the
 * generated table. Reporting a code the table does not carry does not invent
 * wording: it prints the code with arkc's own text. */
void diag_report(int code, DiagSeverity sev, const DiagLoc *loc,
                 const char *a0, const char *a1, const char *a2);

/* Frontend diagnostic anchored at the end of the translation unit, where the
 * offending thing is the absence of a token. armcc prints these as
 *   "f.c", line 3: Error: At end of source:  #67: expected a "}"
 * i.e. the line of the last real token plus an "At end of source" marker and
 * no source echo. `loc->line` should be that last token's line. */
void diag_report_at_eof(int code, DiagSeverity sev, const DiagLoc *loc);

/* arkc-specific diagnostic: no code, caller supplies the text. `msg` may
 * contain the same placeholders as a table template. Use this when no armcc
 * code has been measured for the condition. */
void diag_report_msg(DiagSeverity sev, const DiagLoc *loc, const char *msg,
                     const char *a0, const char *a1, const char *a2);

/* Messages take strings, but several embed numbers. Render `v` into `buf`
 * (at least 12 bytes) and pass the result:
 *   diag_report_msg(..., "width %d exceeds %d bits", diag_num(a, w), diag_num(b, m), 0)
 * Keeping this next to the API stops each stage from growing its own itoa. */
const char *diag_num(char *buf, int v);

/* Driver-level diagnostic with a C####x code (armcc's option layer).
 * Printed without a source position; used before/around compilation.
 * The severity is deliberately NOT a parameter: armcc encodes it as the last
 * letter of the code (U=usage/fatal, E, W, I), so the generated table stays the
 * single source of truth and a call site cannot disagree with the code it
 * prints. Unknown codes are reported as errors with arkc's own wording. */
void diag_driver(const char *code, const char *a0, const char *a1);

/* Tool-stage diagnostic: the linker and the scatter/ld readers report against
 * an output image, not a source line. armlink's own L#### codes were not
 * measured for arkc, so these carry no code -- the shape is
 *   <object>: Error: <text>
 * and the policy (counting, limit, suppression, --errors sink) is shared with
 * everything else. */
void diag_tool(DiagSeverity sev, const char *object, const char *msg,
               const char *a0, const char *a1, const char *a2,
               const char *a3, const char *a4, const char *a5);

/* Command-line-scoped frontend diagnostic: armcc prints
 *   "no source": Error: command-line:  #992: invalid macro definition: X
 * i.e. a literal "no source" location plus a "command-line: " segment. */
void diag_command_line(int code, DiagSeverity sev,
                       const char *a0, const char *a1, const char *a2);

/* ---------------------------------------------------------------------------
 * Counters and policy
 * ------------------------------------------------------------------------- */

int diag_errors(void);
int diag_warnings(void);

/* Non-zero once the error limit has been reached (further errors are dropped:
 * armcc neither prints nor counts them). The limit is a constant
 * (DIAG_LIMIT_DEFAULT = 30); armcc has no option for it ("--diag_limit" is
 * rejected with C3900U), so arkc does not invent one. */
int diag_at_limit(void);

/* Presentation style. diag_set_style_name accepts "arm", "gnu", "ide",
 * "devstudio" (alias of ide) and returns 0 on success, -1 otherwise. */
int diag_set_style_name(const char *name);

/* "#pragma diag_suppress <n>" / "#pragma diag_default <n>" and
 * "--diag_suppress=<n>[,<n>...]" / "--diag_remark=<n>[,<n>...]".
 * Suppression is per code and applies to whatever is reported *after* the
 * pragma is seen. Note: arkc runs the preprocessor before parsing, so a
 * pragma suppresses for the whole translation unit, not from its exact token
 * position (registered limitation, see docs/DIAGNOSTICS.md).
 * Measured: --diag_suppress and --diag_remark are indistinguishable from the
 * outside (no line printed, exit code unchanged), so both use this one set. */
void diag_suppress(int code);
void diag_default(int code);
int  diag_is_suppressed(int code);

/* "--diag_error=<n>[,<n>...]": print and count the code as an error. The -D
 * suffix still follows the class the code has intrinsically (measured:
 * --diag_error=177 prints "Error:  #177-D"), which is why this is a separate
 * override instead of a severity rewrite at the call site. Codes outside
 * 0..DIAG_CODE_MAX are ignored, as in armcc. */
void diag_force_error(int code);

/* "--errors=<file>": redirect the whole diagnostic stream (errors, warnings,
 * echoes, carets, summary) into a file opened at option-scan time, in "w"
 * mode (a clean compile leaves a 0-byte file). Returns 0 on success.
 * Driver-level fatal errors still go to stderr, as in armcc.
 * On freestanding builds this returns -1 (no stdio FILE there). */
int  diag_open_sink(const char *path);
void diag_close_sink(void);

#endif /* ARK_DIAG_H */
