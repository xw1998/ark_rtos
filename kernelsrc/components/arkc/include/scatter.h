/* Ark C Compiler - scatter.h
 * ARM linker scatter load description file parser.
 * Supports a practical subset of armlink scatter syntax:
 *   LR_<name> <addr> [attrs] { ER_<name> <addr> [attrs] { <selectors> } ... }
 * Parse results are stored in ScatterDesc for the backend/link stage to query section placement.
 *
 * Also provides a GNU ld .ld script parser (linker_script_parse_file) and
 * conversion helpers (scatter_to_ld_script / scatter_to_sct_file) so the ARM
 * backend can emit a matching linker script when the user supplies either form.
 */
#ifndef ARK_SCATTER_H
#define ARK_SCATTER_H

/* Re-export the feature macro from arkcc_config.h so callers that only include
 * scatter.h still see the switch. arkcc_config.h is the single source of truth. */
#include "arkcc_config.h"

/* Section attribute flags parsed from selector text such as (+RO +RW +ZI +First +Last).
 * Used by the linker/backend to map an input section (.text/.data/.bss/.rodata)
 * to the execution region that should hold it. */
#define SCATTER_ATTR_RO     0x0001  /* +RO : read-only code/rodata  -> .text/.rodata */
#define SCATTER_ATTR_RW     0x0002  /* +RW : read-write initialized  -> .data */
#define SCATTER_ATTR_ZI     0x0004  /* +ZI : zero-initialized        -> .bss */
#define SCATTER_ATTR_XO     0x0008  /* +XO : execute-only           -> .text */
#define SCATTER_ATTR_FIRST  0x0010  /* +First: place at region start (e.g. vector table RESET) */
#define SCATTER_ATTR_LAST   0x0020  /* +Last : place at region end   (e.g. checksum) */

/* Execution region (ER): corresponds to a runtime address space, placing several input section selectors. */
typedef struct ScatterExecRegion {
    char *name;            /* ER name */
    unsigned load_addr;    /* execution address */
    unsigned size;         /* region size (0 = unspecified) */
    char *selectors;       /* section selector raw text (e.g. "*.o (RO)"), stored as a whole string for simplicity */
    /* Parsed selector summary (derived from `selectors` by scatter_parse_file).
     * module_pattern/section_name capture the first explicit *.o (Name) selector,
     * if any; is_any is set for `.ANY` selectors. attr_flags holds the +RO/+RW/+ZI/... bits.
     * These let the backend pick an ER for a given input section without re-parsing. */
    unsigned attr_flags;   /* bitmask of SCATTER_ATTR_* */
    int is_any;            /* 1 if the ER uses `.ANY (...)` as its selector */
    char *module_pattern;  /* module glob e.g. "*.o" (mem_strdup'd, may be NULL) */
    char *section_name;    /* named section e.g. "RESET"/"InRoot$$Sections" (may be NULL) */
    struct ScatterExecRegion *next;
} ScatterExecRegion;

/* Load region (LR): corresponds to a load address space, contains several execution regions. */
typedef struct ScatterLoadRegion {
    char *name;            /* LR name */
    unsigned load_addr;    /* load address */
    unsigned size;         /* region size (0 = unspecified) */
    ScatterExecRegion *exec_regions;  /* execution region linked list */
    struct ScatterLoadRegion *next;
} ScatterLoadRegion;

/* Parse result of the whole scatter file. */
typedef struct ScatterDesc {
    ScatterLoadRegion *load_regions;
    int error_count;
} ScatterDesc;

/* Parse a scatter file path; returns 0 on success, non-zero on error.
 * Parse result is stored in *out (caller-provided pointer). Multiple calls accumulate (no reset). */
int scatter_parse_file(const char *path, ScatterDesc *out);

/* Parse a GNU ld .ld script path; returns 0 on success.
 * The parser recognizes MEMORY{} and SECTIONS{} blocks enough to extract
 * region addresses; output is converted into ScatterDesc form. */
int linker_script_parse_file(const char *path, ScatterDesc *out);

/* Free resources outside the memory pool held by the parse result (arkcc mostly uses memory pools; here only clears list heads). */
void scatter_free(ScatterDesc *desc);

/* Print parse result to log (for debugging). */
void scatter_dump(const ScatterDesc *desc);

/* Look up the execution address for a given section selector.
 * Returns 0 if not found; otherwise returns the address.
 * Used by the ARM backend to place sections at scatter-defined addresses. */
unsigned scatter_lookup_addr(const ScatterDesc *desc, const char *section_name);

/* SCT-3: Return the execution base address for a named custom section.
 * Searches exec regions for one whose parsed section_name matches exactly,
 * then falls back to a substring match on the raw selectors text. Returns 0
 * when no scatter is loaded or the section is not found; the caller should
 * then fall back to default .data/.bss sequential allocation. */
unsigned scatter_get_er_base_for_section(const ScatterDesc *desc, const char *section_name);

/* Find the first execution region whose parsed attr_flags include `attr`
 * (one of SCATTER_ATTR_RO/RW/ZI). Returns NULL if none matches.
 * The search walks load regions in declaration order, then their ERs. */
ScatterExecRegion *scatter_find_er_by_attr(const ScatterDesc *desc, unsigned attr);

/* Return the load address of the first load region (the Flash load base for
 * code). Falls back to 0x08000000 when no scatter description is loaded.
 * Used by the backend to resolve code-label absolute addresses. */
unsigned scatter_get_flash_base(const ScatterDesc *desc);

/* Return the execution base address for a given attribute (RW or ZI).
 * Falls back to 0x20000000 (default SRAM) when no matching ER exists.
 * Used by the backend to place .data (+RW) / .bss (+ZI) globals. */
unsigned scatter_get_data_base(const ScatterDesc *desc, unsigned attr);

/* Check whether [addr, addr+sz) fits inside er's declared range.
 * Returns 1 when size==0 (unspecified) or it fits; 0 on overflow. */
int scatter_er_fits(const ScatterExecRegion *er, unsigned addr, unsigned sz);

/* Convert a ScatterDesc into a GNU ld .ld script string.
 * Writes to buf; returns required length (may exceed cap, caller should retry). */
int scatter_to_ld_script(const ScatterDesc *desc, char *buf, int cap);

/* Convert a ScatterDesc into an ARM scatter .sct string. */
int scatter_to_sct_file(const ScatterDesc *desc, char *buf, int cap);

#if ARKCC_FEATURE_SCATTER_CHECK
/* 校验分散布局与 --vector-table 的相容性，以及各执行区是否互相重叠。
 * 返回 0 表示布局可用，非 0 表示发现了致命问题（已通过诊断层报告）。
 * 只在用户要求了向量表（--vector-table）时才检查向量表位置：没要求向量表时，
 * RESET 段放在哪里都不影响启动，报错反而会挡住合法的镜像。 */
int scatter_validate(const ScatterDesc *desc, unsigned flash_base, int want_vector_table);
#endif

#endif /* ARK_SCATTER_H */
