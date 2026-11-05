/* Ark C Compiler - arkcc_config.h
 * Compile-time feature switches: decide which features are compiled into arkcc itself.
 * At runtime, g_config.enable_xxx further guards features when compiling user code.
 *
 * === Profile-based trimming ===
 * Define ARKCC_PROFILE to one of:
 *   ARKCC_PROFILE_HOST_FULL      - full host compiler (default): all backends, all features
 *   ARKCC_PROFILE_STM32_MIN     - STM32 self-hosted: only ARM backend, trimmed features
 *   ARKCC_PROFILE_CROSS_COMPILE - Windows cross-compiler for STM32: ARM+C backend, full features
 *
 * If ARKCC_PROFILE is undefined, defaults to ARKCC_PROFILE_HOST_FULL.
 *
 * IMPORTANT: Trimming only affects arkcc's own compilation. The trimmed arkcc still
 *   compiles STM32 user code correctly because core armcc compatibility (keywords,
 *   pragmas, scatter parsing) is always retained. The trim removes unused backends
 *   and optional optimizations, NOT language features needed for STM32 code.
 */
#ifndef ARKCC_CONFIG_H
#define ARKCC_CONFIG_H

/* ===========================================================================
 * 1. Profile selection
 * ========================================================================= */
#ifndef ARKCC_PROFILE
#define ARKCC_PROFILE ARKCC_PROFILE_HOST_FULL
#endif

#define ARKCC_PROFILE_HOST_FULL      0
#define ARKCC_PROFILE_STM32_MIN      1
#define ARKCC_PROFILE_CROSS_COMPILE  2

/* ===========================================================================
 * 2. Backend selection (controlled by profile; overridable via -D)
 *
 * A backend is enabled if its ARKCC_BACKEND_<NAME> macro is 1.
 * The Makefile/MDK project picks the right set via -DARKCC_PROFILE=...
 * or by overriding individual ARKCC_BACKEND_* macros.
 * ========================================================================= */
#ifndef ARKCC_BACKEND_ARM
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_BACKEND_ARM      1   /* STM32 self-hosted: ARM backend mandatory */
# elif ARKCC_PROFILE == ARKCC_PROFILE_CROSS_COMPILE
#   define ARKCC_BACKEND_ARM      1   /* Cross-compile to ARM for STM32 */
# else
#   define ARKCC_BACKEND_ARM      1
# endif
#endif
#ifndef ARKCC_BACKEND_X86_64
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_BACKEND_X86_64   0   /* Trim x86_64 on STM32 */
# elif ARKCC_PROFILE == ARKCC_PROFILE_CROSS_COMPILE
#   define ARKCC_BACKEND_X86_64   0
# else
#   define ARKCC_BACKEND_X86_64   1
# endif
#endif
#ifndef ARKCC_BACKEND_RISCV32
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_BACKEND_RISCV32  0   /* Trim RISC-V on STM32 */
# elif ARKCC_PROFILE == ARKCC_PROFILE_CROSS_COMPILE
#   define ARKCC_BACKEND_RISCV32  0
# else
#   define ARKCC_BACKEND_RISCV32  1
# endif
#endif
#ifndef ARKCC_BACKEND_C
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_BACKEND_C        0   /* Trim C backend on STM32 */
# elif ARKCC_PROFILE == ARKCC_PROFILE_CROSS_COMPILE
#   define ARKCC_BACKEND_C        1   /* Keep C backend for host-side debugging */
# else
#   define ARKCC_BACKEND_C        1
# endif
#endif

/* ===========================================================================
 * 3. Frontend language features
 *
 * These are ALWAYS enabled because STM32 user code depends on them.
 * Trimming them would break arkcc's ability to compile STM32 programs.
 * ========================================================================= */
#define ARKCC_FEATURE_VLA          1   /* variable-length array int a[n] */
#define ARKCC_FEATURE_COMPLEX      1   /* _Complex */
#define ARKCC_FEATURE_WCHAR        1   /* wide character wchar_t / L"..." */
#define ARKCC_FEATURE_TRIGRAPH     1   /* trigraph ??= etc. */

/* ===========================================================================
 * 4. Backend code generation features
 * ========================================================================= */
#ifndef ARKCC_FEATURE_FPU_HW
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_FPU_HW    1   /* STM32H743 has VFPv5-D16 FPU */
# else
#   define ARKCC_FEATURE_FPU_HW    1
# endif
#endif
#ifndef ARKCC_FEATURE_DWARF
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_DWARF     0   /* Trim DWARF debug info on STM32 to save Flash */
# else
#   define ARKCC_FEATURE_DWARF     1
# endif
#endif
#ifndef ARKCC_FEATURE_DWARF_FULL
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_DWARF_FULL 0   /* No full DWARF tables on STM32 */
# else
#   define ARKCC_FEATURE_DWARF_FULL 1
# endif
#endif

/* ===========================================================================
 * 5. Optimization passes
 * ========================================================================= */
#ifndef ARKCC_FEATURE_OPT_CONSTFOLD
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_OPT_CONSTFOLD 1   /* Keep constant folding (cheap) */
# else
#   define ARKCC_FEATURE_OPT_CONSTFOLD 1
# endif
#endif
#ifndef ARKCC_FEATURE_OPT_DCE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_OPT_DCE       1   /* Keep dead code elimination (cheap) */
# else
#   define ARKCC_FEATURE_OPT_DCE        1
# endif
#endif
#ifndef ARKCC_FEATURE_OPT_REGALLOC
# define ARKCC_FEATURE_OPT_REGALLOC  0   /* Register allocation: experimental, default off */
#endif
#ifndef ARKCC_FEATURE_OPT_INLINE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_OPT_INLINE    0   /* Trim auto-inline on STM32 (costs Flash) */
# else
#   define ARKCC_FEATURE_OPT_INLINE    1
# endif
#endif
#ifndef ARKCC_FEATURE_OPT_VECTORIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_OPT_VECTORIZE 0   /* Trim SIMD vectorization on STM32 */
# else
#   define ARKCC_FEATURE_OPT_VECTORIZE 1
# endif
#endif

/* ===========================================================================
 * 6. ARMCC 5 compatibility
 *
 * These are ALWAYS enabled: STM32 user code (CMSIS, HAL, startup) depends on
 * armcc 5 syntax (__irq, __weak, __packed, __asm, #pragma pack, scatter .sct).
 * Trimming these would break compilation of STM32 programs.
 * ========================================================================= */
#define ARKCC_FEATURE_ARMCC_ASM_FUNC  1   /* __asm function body */
#define ARKCC_FEATURE_ARMCC_PRAGMA    1   /* #pragma compatibility */
#define ARKCC_FEATURE_ARMCC_KEYWORDS  1   /* __irq/__weak/__naked etc. */
#define ARKCC_FEATURE_ENUM_IS_INT     1   /* --enum_is_int / -fenum-is-int */
#define ARKCC_FEATURE_SCATTER         1   /* scatter load description file parsing */

/* ===========================================================================
 * 7. Diagnostics
 *
 * DIAG_SINK is the only part of the diagnostic layer that needs stdio (the
 * --errors=<file> sink). On STM32 there is no stdio, so diagnostics go to
 * ark_log() only and diag_open_sink() reports failure.
 * ========================================================================= */
/* DIAG is the master switch for the diagnostic layer (codes, layout, echo,
 * caret, counting, suppression, --errors sink, the generated message table).
 *
 *   1  the shipped behaviour: armcc-compatible presentation (docs/DIAGNOSTICS.md)
 *   0  minimal last resort for footprint-sensitive builds: every entry point
 *      keeps its signature, but a message is printed as one ark_log() line --
 *      no code, no source echo, no caret, no summary, no suppression, no sink,
 *      and src/diag_msg_table.c compiles to nothing. Errors and warnings are
 *      still counted, so the driver's exit code is unchanged.
 *
 * Default 1. The switch exists so the layer can be measured and removed; it is
 * not a runtime option (a runtime flag would have to keep all the code). */
#ifndef ARKCC_FEATURE_DIAG
#   define ARKCC_FEATURE_DIAG 1
#endif

#if !ARKCC_FEATURE_DIAG
#   undef  ARKCC_FEATURE_DIAG_SINK
#   define ARKCC_FEATURE_DIAG_SINK 0
#endif
#ifndef ARKCC_FEATURE_DIAG_SINK
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_DIAG_SINK  0   /* no stdio on the target */
# else
#   define ARKCC_FEATURE_DIAG_SINK  1
# endif
#endif

/* ===========================================================================
 * 8. Runtime memory budget (tunable per profile)
 *
 * These are pool/buffer sizes for arkcc's own arena allocator.
 * STM32 profile uses smaller pools to fit in RAM budget.
 * Override via -DARK_POOL_SIZE=... etc. on the command line if needed.
 * ========================================================================= */
#ifndef ARK_POOL_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_POOL_SIZE          (64 * 1024)     /* 64 KB arena for STM32 (was 128K to fit 512KB AXI) */
# else
#   define ARK_POOL_SIZE          (64 * 1024 * 1024)  /* 64 MB for host */
# endif
#endif
#ifndef ARK_SRC_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_SRC_BUF_SIZE       (16 * 1024)     /* 16 KB source buffer (was 32K to ease 64KB pool pressure) */
# else
#   define ARK_SRC_BUF_SIZE       (1024 * 1024)   /* 1 MB source buffer */
# endif
#endif
#ifndef ARK_ASM_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_ASM_BUF_SIZE      (64 * 1024)      /* 64 KB asm buffer */
# else
#   define ARK_ASM_BUF_SIZE      (8 * 1024 * 1024)  /* 8 MB asm buffer */
# endif
#endif
#ifndef ARK_CODE_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_CODE_BUF_SIZE     (32 * 1024)      /* 32 KB code buffer */
# else
#   define ARK_CODE_BUF_SIZE     (4 * 1024 * 1024)  /* 4 MB code buffer */
# endif
#endif
#ifndef ARK_LINK_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_LINK_BUF_SIZE     (4 * 1024)       /* 4 KB (link mode unused on STM32) */
# else
#   define ARK_LINK_BUF_SIZE     (256 * 1024)     /* 256 KB */
# endif
#endif
#ifndef ARK_OBJ_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_OBJ_BUF_SIZE      (4 * 1024)       /* 4 KB (object mode unused on STM32) */
# else
#   define ARK_OBJ_BUF_SIZE      (ARK_ASM_BUF_SIZE + 16)
# endif
#endif

/* Preprocessor buffer sizes (used by preproc.c). STM32 uses smaller buffers
 * to fit in the AXI SRAM budget. */
#ifndef PP_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define PP_BUF_SIZE           (64 * 1024)      /* 64 KB preprocessed output */
# else
#   define PP_BUF_SIZE           (4 * 1024 * 1024) /* 4 MB */
# endif
#endif
#ifndef PP_MAX_INC_DEPTH
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define PP_MAX_INC_DEPTH      8                /* 8 levels of #include nesting */
# else
#   define PP_MAX_INC_DEPTH      32
# endif
#endif

/* ARM backend symbol/relocation table sizes. STM32 self-hosted builds only
 * compile small programs, so 1024 entries are plenty. */
#ifndef ARM_MAX_SYMBOLS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARM_MAX_SYMBOLS       1024
# else
#   define ARM_MAX_SYMBOLS       4096
# endif
#endif
#ifndef ARM_MAX_RELOCS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARM_MAX_RELOCS        1024
# else
#   define ARM_MAX_RELOCS        4096
# endif
#endif
#ifndef ARM_MAX_GVARS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARM_MAX_GVARS         512
# else
#   define ARM_MAX_GVARS         2048
# endif
#endif

/* Binary linker tables. A symbol that did not fit used to be dropped without
 * a word, which made -link build an image whose Reset_Handler was missing (no
 * vector table) while still exiting 0; exceeding them is now a hard link
 * error, so the fix is to raise these. LdSym / LdReloc are ~136 bytes each. */
#ifndef ARK_LINK_MAX_SYMS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_LINK_MAX_SYMS     512
# else
#   define ARK_LINK_MAX_SYMS     16384
# endif
#endif
#ifndef ARK_LINK_MAX_RELOCS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_LINK_MAX_RELOCS   512
# else
#   define ARK_LINK_MAX_RELOCS   16384
# endif
#endif

/* Report assembler references that no label in the current translation unit
 * can satisfy. With -c (object) mode this is the NORMAL case: the symbol is
 * cross-object and has been recorded as a relocation for -link, so reporting
 * it as an error only makes noise and sets a failing exit code on a file that
 * produced a perfectly good object. Default 0 = stay quiet, -link reports the
 * symbols that really stay unresolved. Set to 1 to debug a missing *local*
 * label (e.g. a codegen bug that loses a label). */
#ifndef ARK_DIAG_UNRESOLVED_LOCAL
# define ARK_DIAG_UNRESOLVED_LOCAL 0
#endif

#endif /* ARKCC_CONFIG_H */
