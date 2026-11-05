/* Ark C Compiler - preproc.h
 * Preprocessor: object macros + function macros + conditional compilation + #include nesting.
 *
 * Include search order: current file directory -> custom -I paths -> built-in stdlib/include.
 */
#ifndef ARK_PREPROC_H
#define ARK_PREPROC_H

#include "common.h"

/* Add extra search paths (reset before each compile). */
void preproc_reset(void);
void preproc_add_include_dir(const char *dir);
void preproc_set_main_file(const char *path);

/* Public macro define/undef interface: lets modules like cpu.c inject CPU-related
 * predefined macros before preproc_process. Same-name existing macros are overwritten. */
void preproc_define(const char *name, const char *body);
void preproc_undef(const char *name);

/* Take raw source code, return the expanded string (from memory pool). */
char *preproc_process(const char *src);

/* Dependency file generation: return list of file paths #included during preprocessing.
 * out array is caller-provided, up to max_count entries; returns the actual count filled.
 * Paths come from preproc's internal include tracking, kept as-is (relative/absolute). */
int preproc_get_included_files(const char **out, int max_count);

#endif /* ARK_PREPROC_H */
