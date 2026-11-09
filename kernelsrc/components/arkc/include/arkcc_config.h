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
#define ARKCC_FEATURE_WCHAR        1   /* wide character wchar_t / L"..." */
#define ARKCC_FEATURE_TRIGRAPH     1   /* trigraph ??= etc. */

/* ARKCC_FEATURE_COMPLEX: C99 _Complex.  Overridable with -D because it really
 * is removable: the bit that marks a complex operand is spelled 0 in a
 * no-complex build, every `aux & IR_AUX_COMPLEX` test then folds to a
 * constant, and the four-word value paths (materialise / creal / cimag /
 * widen / return register pair) fall out with it.  `_Complex` is refused by
 * the parser in that build, so the folded blocks were unreachable.
 *
 *   Default 1 everywhere, including STM32_MIN.  Measured on the H743
 *   firmware (example/stm32h743/kernel/ARKRTOS_TEST, -DARKCC_FEATURE_COMPLEX=0
 *   on the compile line, everything else unchanged): text 438812 -> 424988,
 *   i.e. -13824 B and FLASH 84.13% -> 81.49%; data/bss unchanged.
 *   tests/run_complex_gate.py proves both directions (full build accepts,
 *   trimmed build refuses with a reason, and code without _Complex is
 *   byte-identical in the two builds).
 *
 *   The default stays 1 on purpose: flipping a default is a behaviour change,
 *   so it has to be a deliberate decision, not a side effect of adding the
 *   switch.  A board build that wants the 13.8 KB passes the -D itself. */
#ifndef ARKCC_FEATURE_COMPLEX
#   define ARKCC_FEATURE_COMPLEX   1
#endif

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
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_OPT_REGALLOC   0   /* 板上裁掉：省 Flash，板端本来也跑不到这条路径 */
# else
#   define ARKCC_FEATURE_OPT_REGALLOC   1   /* 宿主/交叉编译：-fopt-regalloc 才有得开 */
# endif
#endif
/* 运行期默认值仍然是 0（config.c: enable_opt_regalloc = ARKCC_FEATURE_...，
 * 但 --regalloc/-fopt-regalloc 才置 1）——见 config.c 的默认值设定 */
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
/* ---------------------------------------------------------------------------
 * 0.8.0 预处理器的三处可配置点
 *
 * 三项都遵守同一条纪律：默认关闭时编译产物与旧行为逐字节相同；打开后必须
 * 有真正走到该分支的用例（见 tests/features/）。
 *
 *  ARKCC_FEATURE_MACRO_REDEF_CHECK  C99 6.10.3p2：同名宏的重定义只有在「形式、
 *                                   形参、替换列表全部相同」时才合法，否则要
 *                                   告警。arkc 之前一律静默覆盖。
 *  ARKCC_FEATURE_MACRO_BODY_DYN     替换文本超过栈缓冲时改为从内存池翻倍扩容，
 *                                   不再截断。硬上限由 ARKCC_MACRO_BODY_MAX 定。
 *  ARKCC_MAX_INC_FILES              -MMD 依赖文件能记录的 #include 个数。之前固定
 *                                   64 且超出后静默丢弃。
 *  ARKCC_FEATURE_CAST_F64F32        float<->double 之间插入 IR_F2F。关掉时恢复
 *                                   旧行为（不插转换节点，两种精度被当成同一表示）。
 *  ARKCC_FEATURE_FCONST_FOLD        浮点常量表达式折叠。关掉时全局浮点初值回到
 *                                   "求不出值就写 0"的旧路径。
 * ------------------------------------------------------------------------- */
/* 0.8.0 的新开关（默认值就是“新行为是否默认生效”的答案）：
 *   ARKCC_FEATURE_OPT_DCE_CFG    基本块级可达性 DCE。默认 0：线性 DCE 已足够，
 *                                CFG 版要用户显式 --dce-cfg 打开。
 *   ARKCC_FEATURE_ARM_RELOC_DATA 多目标文件的数据段可搬迁。默认 0：需要
 *                                --reloc-data，老的单对象流程逐字节不变。
 *   ARKCC_FEATURE_OPT_REG_TOP    栈顶值常驻 r0。默认 0：--reg-top 打开。
 *   ARKCC_FEATURE_RT_LAZY        运行库按需发射。默认 0：--rt-lazy 打开。
 *   ARKCC_FEATURE_ASM_EXTRA      汇编器扩展指令集。默认 0：--asm-extra 打开。 */
/* 编译期：这几段代码是否进入产物。板上（STM32_MIN）默认裁掉以省 Flash；
 * 宿主/交叉编译保留，于是运行时开关 --xxx 才有的可开。
 * 运行时的默认值一律是 0（见 config.c），所以默认行为与旧版本逐字节相同。
 *
 * 例外：ARKCC_FEATURE_ARM_RELOC_DATA 在板上也必须编进去。它不是"省 Flash 的
 * 优化项"，而是"多目标文件能不能链接"的硬条件——板上 build 的最小演示就是
 * 两个 .c 各带 .data 链成一个镜像。裁掉它的后果不是"这个开关无效"，而是
 * 后端不发 .arkdata 站点（emit_mov_addr32 的 marked 分支整个被编译掉），
 * 链接期 ld_relocate_data 于是 patch 到 0 个站点、报 "declares data but
 * carries no relocatable address"，而运行时 --reloc-data 明明已经 on。
 * 当时把它当成"可选优化"裁掉，正是板上 m2-cross-tu-link 反复失败的真因。
 * 代价很小（一个分支 + 标记发射），运行时不打开时输出仍逐字节不变。 */
#ifndef ARKCC_FEATURE_OPT_DCE_CFG
#   if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#       define ARKCC_FEATURE_OPT_DCE_CFG      0
#   else
#       define ARKCC_FEATURE_OPT_DCE_CFG      1
#   endif
#endif
#ifndef ARKCC_FEATURE_ARM_RELOC_DATA
#   define ARKCC_FEATURE_ARM_RELOC_DATA       1
#endif
/* 0.8.0: 段内绝对地址也能交给链接器修正（--reloc-text）。
 *
 * 后端把 g_flash_base + offset 直接编进 MOVW/MOVT，于是"这个对象会被链接到
 * 哪里"在编译期就被写死在指令里。单对象、且基址恰好等于 LR 基址时没问题；
 * 多对象拼接、或镜像被装到别处时那些地址全部偏掉——字符串字面量（.rodata）
 * 最明显，因为它既不是全局变量、也没有跨对象符号可用来解析。
 *
 * 打开后：本地 label 的 LDR rd, =label 记一条 type 12 重定位，对象尾部附一个
 * __arkcc_text_base 符号告诉链接器"我是按哪个基址编的"，链接器再用
 * (它给这个对象的基址 - 那个基址) 去 patch 那对 MOVW/MOVT。
 *
 * 与 ARKCC_FEATURE_ARM_RELOC_DATA 不同，这一项要编进板端产物：板上 build
 * 命令正是靠它把两个对象里的字面量地址纠正过来。代码量很小（一个分支 +
 * 一次 patch 循环），所以 STM32_MIN 也保留；真正的开关是运行时的
 * --reloc-text，默认 0，关掉时一个字节都不变。 */
#ifndef ARKCC_FEATURE_ARM_RELOC_TEXT
#   define ARKCC_FEATURE_ARM_RELOC_TEXT       1
#endif
#ifndef ARKCC_FEATURE_OPT_REG_TOP
#   define ARKCC_FEATURE_OPT_REG_TOP          1
#endif
#ifndef ARKCC_FEATURE_RT_LAZY
#   define ARKCC_FEATURE_RT_LAZY              1
#endif
/* 手写汇编才用得到的形态（VLDM/VSTM/VPUSH/VPOP 多寄存器存取）。后端自己
 * 从来不发射它们，所以板端裁掉只损失"能汇编手写的 .s"，不损失"能编译 .c"；
 * 裁掉时这几条助记符会明确报"not compiled into this build"，而不是变成
 * unknown mnemonic 让人以为拼错了。 */
#ifndef ARKCC_FEATURE_ASM_VFP_LDSTM
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_ASM_VFP_LDSTM   0
# else
#   define ARKCC_FEATURE_ASM_VFP_LDSTM   1
# endif
#endif

#ifndef ARKCC_FEATURE_ASM_EXTRA
#   define ARKCC_FEATURE_ASM_EXTRA            1
#endif

#ifndef ARKCC_FEATURE_SCATTER_CHECK
#   define ARKCC_FEATURE_SCATTER_CHECK    1   /* 分散文件布局校验 */
#endif
#ifndef ARKCC_FEATURE_BUILD_TIME
#   define ARKCC_FEATURE_BUILD_TIME       1   /* --build-date/--build-time/--source-date-epoch */
#endif

#ifndef ARKCC_FEATURE_CAST_F64F32
#   define ARKCC_FEATURE_CAST_F64F32      1   /* float<->double 插 IR_F2F */
#endif

#ifndef ARKCC_FEATURE_FCONST_FOLD
#   define ARKCC_FEATURE_FCONST_FOLD      1   /* 浮点常量表达式折叠 */
#endif

#ifndef ARKCC_FEATURE_MACRO_REDEF_CHECK
#   define ARKCC_FEATURE_MACRO_REDEF_CHECK 1
#endif

#ifndef ARKCC_FEATURE_MACRO_BODY_DYN
#   define ARKCC_FEATURE_MACRO_BODY_DYN    1
#endif

#ifndef ARKCC_MACRO_BODY_MAX
#   if ARKCC_PROFILE == ARKCC_PROFILE_HOST_FULL || ARKCC_PROFILE == ARKCC_PROFILE_CROSS_COMPILE
#       define ARKCC_MACRO_BODY_MAX (1024 * 1024)   /* 1 MiB：池是 64 MB，够大 */
#   else
#       define ARKCC_MACRO_BODY_MAX 16383           /* 板端池只有 80 KB，守住 16 KB */
#   endif
#endif

#ifndef ARKCC_MAX_INC_FILES
#   if ARKCC_PROFILE == ARKCC_PROFILE_HOST_FULL
#       define ARKCC_MAX_INC_FILES 1024
#   else
#       define ARKCC_MAX_INC_FILES 64
#   endif
#endif

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
#   define ARK_LINK_BUF_SIZE     (16 * 1024)      /* 16 KB: holds the largest object read
                                                   * and the linked image on the board */
# else
#   define ARK_LINK_BUF_SIZE     (8 * 1024 * 1024) /* 8 MB: a real STM32H743
#                                        * image is 1-2 MB, and this buffer
#                                        * holds both the largest object and
#                                        * the linked image (host only) */
# endif
#endif
#ifndef ARK_OBJ_BUF_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_OBJ_BUF_SIZE      (16 * 1024)      /* 16 KB: a hello-world object is 4.2 KB;
                                                   * at 4 KB the symbol table silently wrote
                                                   * 66 bytes past the end of obj_buf */
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

/* ===========================================================================
 * 9. 容量上限 -- host is complete, the board trims by configuration
 *
 * 原则：**宿主/交叉编译不许有会挡住真实工程的人为上限**；板端的每一处上限都是
 * 一个可 -D 覆盖的宏，裁剪掉的是"这张板子放不下的量"，不是"语言能力"。
 *
 * 每一项都在越界时**报错并拒绝产出**（不是静默丢弃），所以把数字调小只是
 * 更早失败，不会生成一个看起来成功的错镜像。
 *
 * 覆盖方法（板端裁剪的正式入口）：
 *     make CFLAGS+="-DARK_LINK_MAX_OBJS=32 -DARK_PP_MAX_INC_DIRS=8"
 * ========================================================================= */
/* -link 一次能合并的目标文件个数。宿主 1024（真实 STM32 工程常见 100~300 个），
 * 板端 64。超过就报错，而不是静默跳过按对象的 RAM 处理。 */
#ifndef ARK_LINK_MAX_OBJS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_LINK_MAX_OBJS     64
# else
#   define ARK_LINK_MAX_OBJS     1024
# endif
#endif
/* -I 搜索路径条数。旧实现满 16 条后**静默丢弃**，症状是
 * "cannot open source input file"；宿主给 128。 */
#ifndef ARK_PP_MAX_INC_DIRS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_PP_MAX_INC_DIRS   16
# else
#   define ARK_PP_MAX_INC_DIRS   128
# endif
#endif
/* -D 预定义宏条数。宿主 512（Makefile 驱动的工程常常一次给几十上百个），
 * 板端 64，可 -D 覆盖。 */
#ifndef ARK_MAX_PRE_DEFINES
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_MAX_PRE_DEFINES   64
# else
#   define ARK_MAX_PRE_DEFINES   512
# endif
#endif
/* 一个声明里最多的数组维数（C 标准只要求支持实现定义的上限）。宿主 16 / 板端 8。 */
#ifndef ARK_MAX_ARRAY_DIMS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_MAX_ARRAY_DIMS    8
# else
#   define ARK_MAX_ARRAY_DIMS    16
# endif
#endif
/* 一个函数里“按值返回大结构体（struct/union，size > 8）的调用站点”个数上限。
 * 每个站点在后端要给调用者帧里开一块结果缓冲，所以这是**调用点**计数，
 * 不是函数个数、也不是嵌套深度；超了报错而不是少开一块缓冲。
 * 宿主 256 / 板端 64。 */
#ifndef ARK_MAX_SRET_FUNCS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_MAX_SRET_FUNCS    64
# else
#   define ARK_MAX_SRET_FUNCS    256
# endif
#endif
/* --scatter 里能跟踪的命名段个数（#pragma arm section / __attribute__((section)) 的落点）。 */
#ifndef ARK_MAX_CUSTOM_SECTIONS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_MAX_CUSTOM_SECTIONS 16
# else
#   define ARK_MAX_CUSTOM_SECTIONS 128
# endif
#endif
/* -target c 后端：函数内标签数上限（只影响宿主上的可读性调试输出）。 */
#ifndef ARK_C_MAX_LABELS
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_C_MAX_LABELS      4096
# else
#   define ARK_C_MAX_LABELS      16384
# endif
#endif
/* 符号表哈希桶数。只影响查找速度：128 桶在几千个符号时退化成链表扫描。 */
#ifndef ARK_SYM_HASH_SIZE
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARK_SYM_HASH_SIZE     128
# else
#   define ARK_SYM_HASH_SIZE     4096
# endif
#endif
/* -MMD 依赖表的文件名数组：宿主按需翻倍扩容（没有人为条数上限），板端用
 * ARKCC_MAX_INC_FILES 的定长表并在越界时告警一次。 */
#ifndef ARKCC_FEATURE_DEP_FILES_DYN
# if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
#   define ARKCC_FEATURE_DEP_FILES_DYN 0
# else
#   define ARKCC_FEATURE_DEP_FILES_DYN 1
# endif
#endif

#endif /* ARKCC_CONFIG_H */
