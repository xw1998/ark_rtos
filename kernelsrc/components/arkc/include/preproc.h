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

/* 覆盖 __DATE__ / __TIME__ 的取值（C99 6.10.8.1 允许实现决定它们的来源）。
 * 板上没有 RTC 时平台层只能返回固定串，于是"同一天编译两次时间相同"；需要
 * 可复现构建时也必须能钉死这两个值。传 0 表示恢复平台层给的默认值。
 * 返回 1 表示接受（格式已校验），0 表示格式不合法。 */
int preproc_set_build_time(const char *date, const char *time);

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
/* Number of files recorded for -MMD so far. The host grows that list instead
 * of capping it, so callers size their destination from this value. */
int preproc_included_count(void);

#endif /* ARK_PREPROC_H */
