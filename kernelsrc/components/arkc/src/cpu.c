/* Ark C Compiler - cpu
 * Target CPU selection: look up CPU info via --cpu/-mcpu and apply predefined macros.
 *
 * Covers common Cortex-M cores from armcc 5: M0/M0+/M1/M3/M4/M7/M23/M33.
 * The chosen CPU sets values of __TARGET_ARCH_*, __TARGET_FEATURE_DSP,
 * __TARGET_FPU_VFP etc., and affects runtime switches like g_config.enable_fpu_hw.
 */
#include "cpu.h"
#include "preproc.h"
#include "platform.h"

/* CPU lookup table: common Cortex-M cores for armcc 5 / CMSIS.
 *   arch_m   : value of __TARGET_ARCH_<x>_M (6/7/8 etc.; 0 means undefined)
 *   arch_em  : whether to define __TARGET_ARCH_7E_M (M4/M7/M33 with DSP extension)
 *   has_dsp  : whether to define __TARGET_FEATURE_DSP (M4/M7/M33)
 *   has_fpu  : whether to define __TARGET_FPU_VFP (M4F/M7/M33)
 *   has_fpu_dp: whether double-precision (only M7 DP variant)
 *   thumb_only: whether Thumb-only instruction set (all Cortex-M are thumb-only)
 *   fpu_name : FPU unit name (for diagnostics/future use)
 */
static const CpuInfo g_cpu_table[] = {
    /* Cortex-M0 : ARMv6-M, Thumb-only, no DSP/FPU */
    { "Cortex-M0",   6, 0, 0, 0, 0, 1, 0 },
    /* Cortex-M0+ : ARMv6-M, same as M0 but smaller */
    { "Cortex-M0+",  6, 0, 0, 0, 0, 1, 0 },
    /* Cortex-M1  : ARMv6-M, FPGA-optimized variant */
    { "Cortex-M1",   6, 0, 0, 0, 0, 1, 0 },
    /* Cortex-M3  : ARMv7-M, Thumb-2, no DSP/FPU */
    { "Cortex-M3",   7, 0, 0, 0, 0, 1, 0 },
    /* Cortex-M4  : ARMv7E-M, DSP extension, optional single-precision FPU */
    { "Cortex-M4",   7, 1, 1, 1, 0, 1, "VFPv4-SP" },
    /* Cortex-M7  : ARMv7E-M, DSP + double-precision FPU (default DP) */
    { "Cortex-M7",   7, 1, 1, 1, 1, 1, "VFPv5-D16" },
    /* Cortex-M23 : ARMv8-M, Thumb-only, no DSP/FPU (Baseline) */
    { "Cortex-M23",  8, 0, 0, 0, 0, 1, 0 },
    /* Cortex-M33 : ARMv8-M, DSP + single-precision FPU (Mainline) */
    { "Cortex-M33",  8, 1, 1, 1, 0, 1, "VFPv5-SP" },
};

#define CPU_TABLE_COUNT ((int)(sizeof(g_cpu_table) / sizeof(g_cpu_table[0])))

/* Case-insensitive ASCII string compare, avoiding strcasecmp dependency. */
static int cpu_strcaseeq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == 0 && *b == 0);
}

/* Compare after stripping the "Cortex-" prefix, so "M4" equals "Cortex-M4".
 * Also accepts case variants like "cortex-m4" / "CORTEX-M4". */
static int cpu_name_matches(const char *user, const char *table_name) {
    const char *prefix = "Cortex-";
    int plen = 7;
    int i;
    /* 1) Direct full-name comparison */
    if (cpu_strcaseeq(user, table_name)) return 1;
    /* 2) table_name must start with "Cortex-" (case-insensitive) */
    for (i = 0; i < plen; i++) {
        char pc = prefix[i];
        char tc = table_name[i];
        if (pc >= 'A' && pc <= 'Z') pc = (char)(pc - 'A' + 'a');
        if (tc >= 'A' && tc <= 'Z') tc = (char)(tc - 'A' + 'a');
        if (pc != tc) return 0;
    }
    /* 3) Compare user input against the prefix-stripped part (e.g. "M4" matches "M4") */
    if (cpu_strcaseeq(user, table_name + plen)) return 1;
    return 0;
}

const CpuInfo *cpu_lookup(const char *name) {
    int i;
    if (!name || !*name) return 0;
    for (i = 0; i < CPU_TABLE_COUNT; i++) {
        if (cpu_name_matches(name, g_cpu_table[i].name)) return &g_cpu_table[i];
    }
    return 0;
}

/* Convert integer to string into a static buffer (used for macro bodies, immediate values only) */
static void int_to_str(int v, char *out) {
    int n = 0;
    char tmp[16];
    int i;
    unsigned u;
    if (v < 0) { out[0] = '-'; out++; u = (unsigned)(-v); }
    else u = (unsigned)v;
    if (u == 0) tmp[n++] = '0';
    while (u) { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    for (i = n - 1; i >= 0; i--) out[n - 1 - i] = tmp[i];
    out[n] = 0;
}

void cpu_apply_predefines(const CpuInfo *cpu) {
    char buf[16];
    if (!cpu) return;

    /* Clear all old __TARGET_ARCH_* / __TARGET_FEATURE_* / __TARGET_FPU_* macros,
     * then redefine per the current CPU. This overrides the hardcoded M7 defaults in preproc_process. */
    preproc_undef("__TARGET_ARCH_6_M");
    preproc_undef("__TARGET_ARCH_7_M");
    preproc_undef("__TARGET_ARCH_7E_M");
    preproc_undef("__TARGET_ARCH_8_M");
    preproc_undef("__TARGET_ARCH_8_M_BASE");
    preproc_undef("__TARGET_ARCH_8_M_MAIN");
    preproc_undef("__TARGET_FEATURE_DSP");
    preproc_undef("__TARGET_FPU_VFP");
    preproc_undef("__TARGET_FPU_FPV5");
    preproc_undef("__TARGET_FPU_DP");

    /* Define architecture macro based on arch_m value */
    switch (cpu->arch_m) {
        case 6:
            preproc_define("__TARGET_ARCH_6_M", "1");
            break;
        case 7:
            if (cpu->arch_em) preproc_define("__TARGET_ARCH_7E_M", "1");
            else              preproc_define("__TARGET_ARCH_7_M", "1");
            break;
        case 8:
            if (cpu->arch_em) preproc_define("__TARGET_ARCH_8_M_MAIN", "1");
            else              preproc_define("__TARGET_ARCH_8_M_BASE", "1");
            preproc_define("__TARGET_ARCH_8_M", "1");
            break;
        default:
            break;
    }

    /* DSP extension */
    if (cpu->has_dsp) {
        preproc_define("__TARGET_FEATURE_DSP", "1");
    }

    /* FPU flags: single/double precision */
    if (cpu->has_fpu) {
        preproc_define("__TARGET_FPU_VFP", "1");
        /* M7 defaults to VFPv5-D16; M4 is VFPv4-SP; M33 is VFPv5-SP.
         * Simplification: if fpu_name is non-null, define an extra FPU unit macro. */
        if (cpu->fpu_name) {
            /* Only VFPv5 series defines __TARGET_FPU_FPV5 */
            if (cpu->fpu_name[0] == 'V' && cpu->fpu_name[1] == 'F' &&
                cpu->fpu_name[2] == 'P' && cpu->fpu_name[3] == 'v' &&
                cpu->fpu_name[4] == '5') {
                preproc_define("__TARGET_FPU_FPV5", "1");
            }
        }
        if (cpu->has_fpu_dp) {
            preproc_define("__TARGET_FPU_DP", "1");
        }
    }

    /* Define a __TARGET_CPU reflection macro for user-code diagnostics (non-standard, arkcc extension) */
    preproc_define("__TARGET_CPU_NAME", cpu->name);

    /* Define __TARGET_ARCH_M (major version number) as an integer string for #if comparisons */
    int_to_str(cpu->arch_m, buf);
    preproc_define("__TARGET_ARCH_M", buf);
}
