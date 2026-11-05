/* Ark C Compiler - ARM (Thumb-2) backend
 * Translates IR into textual Thumb-2 assembly. Moved here from the former src/codegen.c.
 *
 * Calling convention (used together with this compiler):
 *   - Arguments 0..3 passed in r0..r3; extra arguments go on the stack (consistent
 *     with the runtime convention, indicated by the val field of IR_CALL).
 *   - r6 = local variable base (fp, pointing to the low address of the local var area).
 *   - Prologue:  PUSH {r5,r6,r7,lr}; SUB sp,sp,#N; MOV r6, sp
 *   - Epilogue:  POP {r0}; MOV sp,r6; ADD sp,sp,#N; POP {r5,r6,r7,pc}
 */
#include "backend.h"
#include "codegen.h"
#include "memory.h"
#include "platform.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "scatter.h"
#include "arkcc_config.h"
/* On host, <stdlib.h> provides getenv() for the ARKCC_DUMP_SYMS debug flag.
 * On STM32, getenv() is not available (semihosting); g_arm_dump_syms is used
 * instead, and mem_realloc (our own allocator) replaces realloc. */
#if ARKCC_PROFILE != ARKCC_PROFILE_STM32_MIN
#  include <stdlib.h>
#endif

/* --- Internal helpers: emit common instruction fragments --- */
static void emit_line(AsmBuffer *b, const char *s) {
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, s);
    asmbuf_putc(b, '\n');
}

static void emit_mov_imm(AsmBuffer *b, int rd, int imm) {
    if (imm >= 0 && imm <= 255) {
        asmbuf_puts(b, "    MOV r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, imm);
        asmbuf_putc(b, '\n');
    } else {
        /* Always emit MOVW + MOVT pair. MOVW only sets bits[15:0];
         * without MOVT, bits[31:16] retain stale values from previous
         * operations, causing incorrect constants (e.g., 0x40022000
         * instead of 0x00002000 for 1u<<13). */
        asmbuf_puts(b, "    MOVW r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, imm & 0xFFFF);
        asmbuf_putc(b, '\n');
        asmbuf_puts(b, "    MOVT r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, (imm >> 16) & 0xFFFF);
        asmbuf_putc(b, '\n');
    }
}

static void emit_push_r0(AsmBuffer *b) { emit_line(b, "PUSH {r0}"); }
static void emit_pop_r0(AsmBuffer *b)  { emit_line(b, "POP {r0}");  }
static void emit_pop_r1(AsmBuffer *b)  { emit_line(b, "POP {r1}");  }

/* sret support: check if a function name is in the sret set */
static int sret_lookup(char **names, int *sizes, int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        if (ark_streq(names[i], name)) return 1;
    }
    return 0;
}

/* sret support: get struct return size for a function name */
static int sret_get_size(char **names, int *sizes, int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        if (ark_streq(names[i], name)) return sizes[i];
    }
    return 0;
}

#if ARKCC_FEATURE_FPU_HW
/* Hardware VFP floating-point path helpers.
 * Convention: floating-point values are stored on the stack as integer bit patterns (single=1 slot, double=2 slots low/high).
 * Pop operands to r0/r1 (single) or r0/r1+r2/r3 (double), move them into VFP registers via VMOV,
 * perform the VFP operation, then move the result back to r0 (single) or r0/r1 (double) and push. */

/* Single-precision binary op: pop b(s1), a(s0), emit VOP.F32 s0,s0,s1, push result s0. */
static void emit_vfp_f32_binop(AsmBuffer *b, const char *vop) {
    emit_pop_r1(b); emit_pop_r0(b);
    emit_line(b, "VMOV s0, r0");
    emit_line(b, "VMOV s1, r1");
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, vop);
    asmbuf_puts(b, ".F32 s0, s0, s1\n");
    emit_line(b, "VMOV r0, s0");
    emit_push_r0(b);
}

/* Double-precision binary op: pop b(d1), a(d0), emit VOP.F64 d0,d0,d1, push result d0. */
static void emit_vfp_f64_binop(AsmBuffer *b, const char *vop) {
    emit_line(b, "POP {r3}");  /* b_high */
    emit_line(b, "POP {r2}");  /* b_low  */
    emit_line(b, "POP {r1}");  /* a_high */
    emit_line(b, "POP {r0}");  /* a_low  */
    emit_line(b, "VMOV s0, r0");
    emit_line(b, "VMOV s1, r1");
    emit_line(b, "VMOV s2, r2");
    emit_line(b, "VMOV s3, r3");
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, vop);
    asmbuf_puts(b, ".F64 d0, d0, d1\n");
    emit_line(b, "VMOV r0, s0");
    emit_line(b, "VMOV r1, s1");
    emit_push_r0(b);
    asmbuf_puts(b, "    PUSH {r1}\n");
}
#endif /* ARKCC_FEATURE_FPU_HW */

static void emit_label(AsmBuffer *b, const char *fn, int n) {
    asmbuf_puts(b, ".L_");
    asmbuf_puts(b, fn);
    asmbuf_putc(b, '_');
    asmbuf_int(b, n);
    asmbuf_puts(b, ":\n");
}

static void emit_b_label(AsmBuffer *b, const char *op, const char *fn, int n) {
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, op);
    asmbuf_puts(b, " .L_");
    asmbuf_puts(b, fn);
    asmbuf_putc(b, '_');
    asmbuf_int(b, n);
    asmbuf_putc(b, '\n');
}

/* Emit binary operation. The high bit 0x100 of aux marks unsigned comparison/shift-right:
 *   - TOK_LT/GT/LE/GE: when unsigned, use LO/HI/LS/HS conditions instead of LT/GT/LE/GE
 *   - TOK_SHR: when unsigned, use LSR (logical shift right) instead of ASR (arithmetic shift right)
 * ARM Thumb-2 condition codes: LO=CC(3), HI(8), LS(9), HS=CS(2) */
static void emit_binary(AsmBuffer *b, int op) {
    int is_unsigned = (op & 0x100) != 0;
    int base_op = op & ~0x100;
    switch (base_op) {
        case (int)TOK_PLUS:  emit_line(b, "ADD r0, r0, r1"); break;
        case (int)TOK_MINUS: emit_line(b, "SUB r0, r0, r1"); break;
        case (int)TOK_MUL:   emit_line(b, "MUL r0, r1");     break;
        case (int)TOK_DIV:   emit_line(b, "SDIV r0, r0, r1"); break;
        case (int)TOK_MOD:
            emit_line(b, "SDIV r2, r0, r1");
            emit_line(b, "MUL r2, r1");
            emit_line(b, "SUB r0, r0, r2");
            break;
        case (int)TOK_BAND:  emit_line(b, "AND r0, r1");     break;
        case (int)TOK_BOR:   emit_line(b, "ORR r0, r1");     break;
        case (int)TOK_BXOR:  emit_line(b, "EOR r0, r1");     break;
        case (int)TOK_SHL:   emit_line(b, "LSL r0, r1");     break;
        case (int)TOK_SHR:
            /* Unsigned uses LSR (logical shift right), signed uses ASR (arithmetic shift right) */
            emit_line(b, is_unsigned ? "LSR r0, r1" : "ASR r0, r1");
            break;
        case (int)TOK_EQ:
            emit_line(b, "CMP r0, r1");
            emit_line(b, "ITE EQ");
            emit_line(b, "MOVEQ r0, #1");
            emit_line(b, "MOVNE r0, #0");
            break;
        case (int)TOK_NE:
            emit_line(b, "CMP r0, r1");
            emit_line(b, "ITE NE");
            emit_line(b, "MOVNE r0, #1");
            emit_line(b, "MOVEQ r0, #0");
            break;
        case (int)TOK_LT:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE LO"); emit_line(b, "MOVLO r0, #1"); emit_line(b, "MOVHS r0, #0"); }
            else             { emit_line(b, "ITE LT"); emit_line(b, "MOVLT r0, #1"); emit_line(b, "MOVGE r0, #0"); }
            break;
        case (int)TOK_GT:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE HI"); emit_line(b, "MOVHI r0, #1"); emit_line(b, "MOVLS r0, #0"); }
            else             { emit_line(b, "ITE GT"); emit_line(b, "MOVGT r0, #1"); emit_line(b, "MOVLE r0, #0"); }
            break;
        case (int)TOK_LE:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE LS"); emit_line(b, "MOVLS r0, #1"); emit_line(b, "MOVHI r0, #0"); }
            else             { emit_line(b, "ITE LE"); emit_line(b, "MOVLE r0, #1"); emit_line(b, "MOVGT r0, #0"); }
            break;
        case (int)TOK_GE:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE HS"); emit_line(b, "MOVHS r0, #1"); emit_line(b, "MOVLO r0, #0"); }
            else             { emit_line(b, "ITE GE"); emit_line(b, "MOVGE r0, #1"); emit_line(b, "MOVLT r0, #0"); }
            break;
        default:
            emit_line(b, "; unknown binary op");
            break;
    }
}

static void emit_unary(AsmBuffer *b, int op) {
    switch (op) {
        case (int)TOK_MINUS:
            emit_line(b, "NEG r0, r0");
            break;
        case (int)TOK_BNOT:
            emit_line(b, "MVN r0, r0");
            break;
        case (int)TOK_NOT:
            /* FIX(logical-not): the old sequence was
               CMP / MOV r0,#0 / IT EQ / MOVEQ r0,#1.
               "MOV r0, #0" assembles to 16-bit "movs r0,#0" which
               overwrites the flags set by CMP, so IT EQ always fired
               and "!x" was constantly 1 (this deadlocked
               while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) on the H7).
               Use the same ITE + two conditional MOV shape as
               TOK_EQ/TOK_NE: conditional MOVs do not update flags. */
            emit_line(b, "CMP r0, #0");
            emit_line(b, "ITE EQ");
            emit_line(b, "MOVEQ r0, #1");
            emit_line(b, "MOVNE r0, #0");
            break;
        default:
            emit_line(b, "; unknown unary op");
            break;
    }
}

static void emit_prologue(AsmBuffer *b, const char *fn_name, int local_size,
                          int is_irq, int is_naked, int dbg) {
    asmbuf_puts(b, fn_name);
    asmbuf_puts(b, ":\n");
    if (dbg) emit_line(b, ".cfi_startproc");
    if (is_naked) {
        /* __naked: emit no prologue; the function body is entirely the user's responsibility */
        return;
    }
    if (is_irq) {
        /* __irq: Cortex-M hardware auto-saves r0-r3, r12, lr, pc, xpsr on
         * exception entry. We only need to save r5, r6, r7 (arkcc's
         * callee-saved registers used as frame pointer etc.) and lr
         * (EXC_RETURN) so that POP {pc} triggers exception return. */
        emit_line(b, "PUSH {r5, r6, r7, lr}");
    } else {
        emit_line(b, "PUSH {r5, r6, r7, lr}");
    }
    if (local_size > 0) {
        asmbuf_puts(b, "    SUB sp, sp, #");
        asmbuf_int(b, local_size);
        asmbuf_putc(b, '\n');
    }
    emit_line(b, "MOV r6, sp");
}

static void emit_epilogue_return(AsmBuffer *b, int local_size,
                                 int is_irq, int is_naked) {
    if (is_naked) {
        /* __naked: just return, do not restore stack/registers */
        emit_line(b, "BX lr");
        return;
    }
    emit_pop_r0(b);
    emit_line(b, "MOV sp, r6");
    if (local_size > 0) {
        asmbuf_puts(b, "    ADD sp, sp, #");
        asmbuf_int(b, local_size);
        asmbuf_putc(b, '\n');
    }
    if (is_irq) {
        emit_line(b, "POP {r5, r6, r7, pc}");
    } else {
        emit_line(b, "POP {r5, r6, r7, pc}");
    }
}

/* Global variable SRAM address allocation for ARM backend.
 * Globals are placed in SRAM starting at 0x20000000; __at variables keep
 * their absolute address. Codegen emits MOVW/MOVT to load these addresses
 * instead of LDR =label (which the minimal assembler doesn't fully support). */
static const char *g_gvar_names[ARM_MAX_GVARS];
static unsigned    g_gvar_addrs[ARM_MAX_GVARS];
static int         g_gvar_sizes[ARM_MAX_GVARS];  /* #6 FIX: track global variable sizes */
static int         g_gvar_count = 0;

/* SCT-1/SCT-3: Custom section address tracking. When a global has a
 * g->section (from #pragma arm section / __attribute__((section))),
 * gvar_assign looks up the section's exec region base from the scatter
 * description and allocates variables contiguously from that base. Each
 * entry tracks the next free offset for one named section. Reset per
 * compilation in gvar_assign. */
#define ARM_MAX_CUSTOM_SECTIONS 16
static struct {
    const char *name;     /* section name (points into IrGlobal, lives through codegen) */
    unsigned    cur_addr; /* next free address within this section's exec region */
} g_custom_sect[ARM_MAX_CUSTOM_SECTIONS];
static int g_custom_sect_count = 0;

/* F3/S5: Track .data/.bss boundaries for startup initialization */
static unsigned g_data_sram_start = 0;  /* First .data SRAM address */
static unsigned g_data_sram_end   = 0;  /* One past last .data SRAM address */
static unsigned g_bss_sram_start  = 0;  /* First .bss SRAM address */
static unsigned g_bss_sram_end    = 0;  /* One past last .bss SRAM address */

/* Scatter (.sct) load description: when --scatter <file> is given, the parsed
 * regions drive address assignment instead of the hardcoded defaults below.
 * g_flash_base is the base for code labels (LR load address); g_data_base /
 * g_bss_base are the SRAM origins for .data (+RW) and .bss (+ZI). These are
 * refreshed by arm_scatter_load() at the start of each codegen_arm() call. */
static ScatterDesc g_scatter;
static int          g_scatter_loaded = 0;
static unsigned     g_flash_base = 0x08000000u;  /* default STM32F4 Flash base */
static unsigned     g_data_base  = 0x20000000u;  /* default SRAM base */
static unsigned     g_bss_base   = 0x20000000u;  /* default SRAM base (.bss follows .data) */

/* Debug flag: when set, arm_assemble dumps the symbol map and scatter layout
 * after assembly. On host it is latched from getenv("ARKCC_DUMP_SYMS") at
 * arkcc startup by arm_dump_syms_init(); on STM32 getenv() is unavailable
 * under semihosting, so the flag is instead set by the UART "debug syms"
 * command. Defined here (early) so arm_scatter_load() can read it. */
int g_arm_dump_syms = 0;

/* --vector-table: reserve ARM_VT_SIZE bytes at the front of the image for the
 * Cortex-M vector table, so the table can be written *into* the reserved gap
 * instead of being prepended at link time.  Prepending shifts every byte by
 * 0x400; PC-relative Thumb-2 branches survive that, absolute addresses baked
 * by the assembler do not.  0 = old behaviour, byte for byte. */
int g_arm_vt_reserve = 0;

/* Cortex-M vector table: 256 entries, 1 KB.  Declared here (not next to
 * arm_link) because both the assembler -- which reserves this many bytes at the
 * front of the image -- and the linker need it. */
#define ARM_VT_ENTRIES 256
#define ARM_VT_SIZE   (ARM_VT_ENTRIES * 4)

/* Parse g_config.scatter_file (if set) into g_scatter and derive the Flash /
 * data / bss base addresses from the described execution regions. Called once
 * per compilation so the backend places sections per the scatter layout. */
/* Derive the code / .data / .bss base addresses from an already parsed
 * scatter description. The .sct path and the -T/--linker-script path must
 * share this rule: when only the linker-script branch derived the bases,
 * "--scatter <file>" parsed fine but left every address at the hardcoded
 * defaults, i.e. it was a silent no-op for code generation. */
#if ARKCC_FEATURE_SCATTER
static void arm_scatter_derive_bases(void) {
    g_flash_base = scatter_get_flash_base(&g_scatter);
    /* +RW region is the load origin for .data; +ZI for .bss.
     * If a +ZI region exists separately, .bss starts there, otherwise it
     * follows .data (set bss_base = data_base; gvar_assign lays them out
     * contiguously and records the real bss_start boundary). */
    g_data_base = scatter_get_data_base(&g_scatter, SCATTER_ATTR_RW);
    if (scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_ZI)) {
        g_bss_base = scatter_get_data_base(&g_scatter, SCATTER_ATTR_ZI);
    } else {
        g_bss_base = g_data_base;
    }
}
#endif

static void arm_scatter_load(void) {
    g_scatter_loaded = 0;
    g_scatter.load_regions = 0;
    g_scatter.error_count = 0;
    g_flash_base = 0x08000000u;
    g_data_base  = 0x20000000u;
    g_bss_base   = 0x20000000u;
#if ARKCC_FEATURE_SCATTER
    if (g_config.scatter_file && g_config.scatter_file[0] &&
        scatter_parse_file(g_config.scatter_file, &g_scatter) == 0) {
        g_scatter_loaded = 1;
        arm_scatter_derive_bases();
    } else if (g_config.linker_script && g_config.linker_script[0] &&
               linker_script_parse_file(g_config.linker_script, &g_scatter) == 0) {
        /* -T/--linker-script: parse GNU ld script the same way as .sct */
        g_scatter_loaded = 1;
        arm_scatter_derive_bases();
    }
    if (g_scatter_loaded && g_arm_dump_syms) {
        ark_log("[scatter] flash=0x%08X data=0x%08X bss=0x%08X\n",
                g_flash_base, g_data_base, g_bss_base);
        scatter_dump(&g_scatter);
    }
#endif
}

static unsigned gvar_find(const char *name) {
    int i;
    for (i = 0; i < g_gvar_count; i++)
        if (ark_streq(g_gvar_names[i], name)) return g_gvar_addrs[i];
    return 0;
}

/* #6 FIX: Find global variable size by name */
static int gvar_find_size(const char *name) {
    int i;
    for (i = 0; i < g_gvar_count; i++)
        if (ark_streq(g_gvar_names[i], name)) return g_gvar_sizes[i];
    return 4;  /* default 4 bytes */
}

static int gvar_known(const char *name) {
    int i;
    for (i = 0; i < g_gvar_count; i++)
        if (ark_streq(g_gvar_names[i], name)) return 1;
    return 0;
}

/* SCT-1/SCT-3: Allocate `sz` bytes from the named custom section's exec region.
 * Looks up the section base via scatter_get_er_base_for_section the first time
 * the section is seen, then hands out consecutive addresses from that base.
 * Returns the assigned address, or 0 if the section is not described in the
 * scatter file (or no scatter is loaded) -- in that case the caller falls
 * back to the default .data/.bss sequential allocation. */
static unsigned custom_sect_alloc(const char *section_name, int sz) {
    int i;
    unsigned base, addr;
    /* Existing section: continue from the tracked offset. */
    for (i = 0; i < g_custom_sect_count; i++) {
        if (ark_streq(g_custom_sect[i].name, section_name)) {
            g_custom_sect[i].cur_addr = (g_custom_sect[i].cur_addr + 3) & ~3u;
            addr = g_custom_sect[i].cur_addr;
            g_custom_sect[i].cur_addr += sz;
            return addr;
        }
    }
    /* New section: resolve its base from the scatter description. */
    if (g_custom_sect_count >= ARM_MAX_CUSTOM_SECTIONS) return 0;
#if ARKCC_FEATURE_SCATTER
    base = scatter_get_er_base_for_section(&g_scatter, section_name);
#else
    base = 0;
#endif
    if (base == 0) return 0;  /* not found: caller falls back to default */
    g_custom_sect[g_custom_sect_count].name = section_name;
    g_custom_sect[g_custom_sect_count].cur_addr = (base + 3) & ~3u;
    addr = g_custom_sect[g_custom_sect_count].cur_addr;
    g_custom_sect[g_custom_sect_count].cur_addr += sz;
    g_custom_sect_count++;
    return addr;
}

static void gvar_assign(IrProgram *prog) {
    unsigned data_sram = g_data_base;   /* .data origin (scatter +RW or 0x20000000) */
    unsigned bss_sram;                  /* .bss origin (computed below) */
    int gi;
    g_gvar_count = 0;
    g_custom_sect_count = 0;  /* SCT-1/SCT-3: reset custom section tracking */
    g_data_sram_start = data_sram;  /* will be updated if .data globals exist */
    g_data_sram_end = data_sram;
    g_bss_sram_start = data_sram;
    g_bss_sram_end = data_sram;

    /* F3/S5: First pass - assign .data (initialized) globals to SRAM.
     * This ensures .data globals are contiguous, followed by .bss globals.
     * The startup code (__arkcc_init_globals) initializes .data and zeros .bss. */
    for (gi = 0; gi < prog->global_count && g_gvar_count < ARM_MAX_GVARS; gi++) {
        IrGlobal *g = prog->globals[gi];
        int sz;
        if (g->is_extern) continue;
        if (g->has_at) continue;  /* __at variables keep their absolute address */
        if (!g->has_init) continue;  /* skip .bss for now */
        /* Zero-initialized scalars go to .bss */
        if (!g->init_aggregate && !g->init_str && !g->is_float && g->init_int == 0) continue;
        sz = g->type ? type_size(g->type) : 4;
        if (sz <= 0) sz = 4;
        /* SCT-1/SCT-3: custom section globals get their address from the
         * scatter exec region rather than the default .data pool. */
        if (g->section && g_scatter_loaded) {
            unsigned caddr = custom_sect_alloc(g->section, sz);
            if (caddr) {
                g_gvar_names[g_gvar_count] = g->name;
                g_gvar_addrs[g_gvar_count] = caddr;
                g_gvar_sizes[g_gvar_count] = sz;
                g_gvar_count++;
                continue;
            }
            /* section not found in scatter: fall through to .data allocation */
        }
        data_sram = (data_sram + 3) & ~3u;
        g_gvar_names[g_gvar_count] = g->name;
        g_gvar_addrs[g_gvar_count] = data_sram;
        g_gvar_sizes[g_gvar_count] = sz;  /* #6 FIX */
        g_gvar_count++;
        data_sram += sz;
    }
    /* #BSS-ALIGN FIX: Align .data end / .bss start to 4 bytes so the
     * __arkcc_init_globals zeroing loop starts at an aligned address.
     * STR r2, [r0] on an unaligned r0 can fault on Cortex-M. */
    data_sram = (data_sram + 3) & ~3u;
    g_data_sram_end = data_sram;

    /* .bss origin: when scatter defines a separate +ZI region, .bss starts
     * there; otherwise it follows .data contiguously. Take the higher of the
     * +ZI base and the .data end so .data and .bss never overlap. */
    bss_sram = (g_bss_base > data_sram) ? g_bss_base : data_sram;
    g_bss_sram_start = bss_sram;

    /* Second pass - assign .bss (uninitialized/zero) globals after .data */
    for (gi = 0; gi < prog->global_count && g_gvar_count < ARM_MAX_GVARS; gi++) {
        IrGlobal *g = prog->globals[gi];
        int sz;
        if (g->is_extern) continue;
        if (g->has_at) {
            /* __at variables: add to lookup table with their absolute address */
            g_gvar_names[g_gvar_count] = g->name;
            g_gvar_addrs[g_gvar_count] = g->at_addr;
            g_gvar_sizes[g_gvar_count] = g->type ? type_size(g->type) : 4;  /* #6 FIX */
            g_gvar_count++;
            continue;
        }
        if (g->has_init) {
            /* Non-zero initialized globals were assigned in .data pass above.
             * Zero-initialized scalars (init_int==0, no aggregate/str/float) were
             * skipped in .data and should be assigned here in .bss. */
            if (!(g->init_aggregate || g->init_str || g->is_float) && g->init_int == 0) {
                /* zero-initialized scalar: assign in .bss */
            } else {
                continue;  /* already assigned in .data */
            }
        }
        sz = g->type ? type_size(g->type) : 4;
        if (sz <= 0) sz = 4;
        /* SCT-1/SCT-3: custom section globals get their address from the
         * scatter exec region rather than the default .bss pool. */
        if (g->section && g_scatter_loaded) {
            unsigned caddr = custom_sect_alloc(g->section, sz);
            if (caddr) {
                g_gvar_names[g_gvar_count] = g->name;
                g_gvar_addrs[g_gvar_count] = caddr;
                g_gvar_sizes[g_gvar_count] = sz;
                g_gvar_count++;
                continue;
            }
            /* section not found in scatter: fall through to .bss allocation */
        }
        bss_sram = (bss_sram + 3) & ~3u;
        g_gvar_names[g_gvar_count] = g->name;
        g_gvar_addrs[g_gvar_count] = bss_sram;
        g_gvar_sizes[g_gvar_count] = sz;  /* #6 FIX */
        g_gvar_count++;
        bss_sram += sz;
    }
    /* #BSS-ALIGN FIX: Align .bss end to 4 bytes. The __arkcc_init_globals
     * zeroing loop increments r0 by 4 and terminates with CMP r0, r1; BNE.
     * If the last .bss variable's size is not a multiple of 4 (e.g. a char
     * or uint8_t array), r0 would never equal r1, causing an infinite loop
     * that writes zeros past .bss into adjacent memory or HardFault. */
    g_bss_sram_end = (bss_sram + 3) & ~3u;

    /* Scatter overflow check: warn (do not fail) if .data/.bss grew past their
     * declared execution-region size. Helps catch layout mistakes early. */
#if ARKCC_FEATURE_SCATTER
    if (g_scatter_loaded) {
        ScatterExecRegion *rw_er = scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_RW);
        ScatterExecRegion *zi_er = scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_ZI);
        if (rw_er && !scatter_er_fits(rw_er, g_data_sram_start,
                                      g_data_sram_end - g_data_sram_start))
            ark_log("[scatter] warning: .data (0x%X..0x%X) overflows %s (size 0x%X)\n",
                    g_data_sram_start, g_data_sram_end, rw_er->name, rw_er->size);
        if (zi_er && !scatter_er_fits(zi_er, g_bss_sram_start,
                                      g_bss_sram_end - g_bss_sram_start))
            ark_log("[scatter] warning: .bss (0x%X..0x%X) overflows %s (size 0x%X)\n",
                    g_bss_sram_start, g_bss_sram_end, zi_er->name, zi_er->size);
    }
#endif
}

static char *codegen_arm(IrProgram *prog) {
    AsmBuffer buf;
    int fi;
#if ARKCC_FEATURE_DWARF
    int dbg = (g_config.debug_info || g_config.enable_dwarf) ? 1 : 0;
#else
    int dbg = 0;
#endif
    asmbuf_init(&buf);

    /* Load the scatter description (if --scatter was given) so the base
     * addresses for Flash/.data/.bss come from the .sct layout. Must happen
     * before gvar_assign, which places globals at g_data_base/g_bss_base. */
    arm_scatter_load();

    /* Assign SRAM addresses to all non-extern, non-__at globals.
     * This must happen before generating function code so IR_PUSH_GLOBAL etc.
     * can emit MOVW/MOVT with the correct SRAM address. */
    gvar_assign(prog);

    asmbuf_puts(&buf, "; Ark C Compiler - generated Thumb-2 assembly\n");
    asmbuf_puts(&buf, ".thumb\n");
    asmbuf_puts(&buf, ".text\n\n");

#if ARKCC_FEATURE_DWARF
    /* DWARF: declare the source file for later .loc 1 <line> references */
    if (dbg && prog->source_file) {
        asmbuf_puts(&buf, ".file 1 \"");
        asmbuf_puts(&buf, prog->source_file);
        asmbuf_puts(&buf, "\"\n");
    }
#endif

    /* String literal section */
    if (prog->string_count > 0) {
        int i;
        asmbuf_puts(&buf, ".section .rodata\n");
        for (i = 0; i < prog->string_count; i++) {
            IrString *s = prog->strings[i];
            asmbuf_puts(&buf, ".L_str_");
            asmbuf_int(&buf, s->id);
            asmbuf_puts(&buf, ":\n    .asciz \"");
            {
                int k;
                for (k = 0; k < s->len; k++) {
                    char c = s->data[k];
                    if (c == '\n') asmbuf_puts(&buf, "\\n");
                    else if (c == '\t') asmbuf_puts(&buf, "\\t");
                    else if (c == '\r') asmbuf_puts(&buf, "\\r");
                    else if (c == '\\') asmbuf_puts(&buf, "\\\\");
                    else if (c == '"')  asmbuf_puts(&buf, "\\\"");
                    else if (c == '\b') asmbuf_puts(&buf, "\\b");
                    else if (c == '\f') asmbuf_puts(&buf, "\\f");
                    else if (c == '\v') asmbuf_puts(&buf, "\\v");
                    else if (c == '\a') asmbuf_puts(&buf, "\\a");
                    else if (c == '\0') asmbuf_puts(&buf, "\\0");
                    else if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) {
                        char hb[8]; int hn = 0;
                        const char *hh = "0123456789abcdef";
                        hb[hn++] = '\\'; hb[hn++] = 'x';
                        hb[hn++] = hh[((unsigned char)c >> 4) & 0xF];
                        hb[hn++] = hh[(unsigned char)c & 0xF];
                        hb[hn] = 0;
                        asmbuf_puts(&buf, hb);
                    }
                    else asmbuf_putc(&buf, c);
                }
            }
            asmbuf_puts(&buf, "\"\n");
        }
        asmbuf_puts(&buf, ".text\n\n");
    }

    /* Floating-point constant section: emit both single-precision (.L_flt_<i>) and double-precision (.L_dbl_<i>) bit patterns.
     * The high bit 0x200 of IR_PUSH_FLOAT aux determines which to load:
     *   - 0x200 not set: single-precision, 4 bytes, uses __aeabi_f* family
     *   - 0x200 set: double-precision, 8 bytes, uses __aeabi_d* family
     * Stored uniformly as double; the single-precision version is truncated via (float). */
    if (prog->float_count > 0) {
        int i;
        asmbuf_puts(&buf, ".section .rodata\n");
        for (i = 0; i < prog->float_count; i++) {
            union { float f; unsigned int u; } u;
            union { double d; unsigned int u[2]; } du;
            unsigned int bits;
            char hex[9];
            int k;
            const char *hd = "0123456789abcdef";
            /* Single-precision: float bit pattern (4 bytes) */
            u.f = (float)prog->floats[i];
            bits = u.u;
            for (k = 7; k >= 0; k--) { hex[k] = hd[bits & 0xF]; bits >>= 4; }
            hex[8] = 0;
            asmbuf_puts(&buf, ".L_flt_");
            asmbuf_int(&buf, i);
            asmbuf_puts(&buf, ":\n    .word 0x");
            asmbuf_puts(&buf, hex);
            asmbuf_putc(&buf, '\n');
            /* Double-precision: double bit pattern (8 bytes, little-endian: low word first) */
            du.d = prog->floats[i];
            asmbuf_puts(&buf, ".L_dbl_");
            asmbuf_int(&buf, i);
            asmbuf_puts(&buf, ":\n    .word 0x");
            {
                unsigned int lo = du.u[0];
                char hex2[9]; int k2;
                for (k2 = 7; k2 >= 0; k2--) { hex2[k2] = hd[lo & 0xF]; lo >>= 4; }
                hex2[8] = 0;
                asmbuf_puts(&buf, hex2);
            }
            asmbuf_puts(&buf, "\n    .word 0x");
            {
                unsigned int hi = du.u[1];
                char hex2[9]; int k2;
                for (k2 = 7; k2 >= 0; k2--) { hex2[k2] = hd[hi & 0xF]; hi >>= 4; }
                hex2[8] = 0;
                asmbuf_puts(&buf, hex2);
            }
            asmbuf_putc(&buf, '\n');
        }
        asmbuf_puts(&buf, ".text\n\n");
    }

    /* Build sret function name/size set: struct/union returns > 8 bytes
     * use AAPCS sret convention (hidden pointer in r0). */
    char *sret_names[256];
    int   sret_sizes[256];
    int   sret_count = 0;
    {
        int sfi;
        for (sfi = 0; sfi < prog->func_count && sret_count < 256; sfi++) {
            IrFunc *sf = prog->funcs[sfi];
            Type *srt = sf->ret_type;
            if (srt && (srt->kind == TYPE_STRUCT || srt->kind == TYPE_UNION)) {
                int sz = type_size(srt);
                if (sz > 8) {
                    sret_names[sret_count] = sf->name;
                    sret_sizes[sret_count] = (sz + 3) & ~3; /* 4-byte aligned */
                    sret_count++;
                }
            }
        }
    }

    for (fi = 0; fi < prog->func_count; fi++) {
        IrFunc *f = prog->funcs[fi];
        int local_size = f->local_size;
        int i;
        int is_sret = 0;
        int sret_off = 0;
        /* P1 FIX (sret caller side): pre-allocate sret buffers in this
         * function's frame so the sret space doesn't occupy SP+0 (where
         * the callee expects to read stack args). Each sret call site gets
         * its own buffer at a known r6-relative offset. */
        int sret_call_buf_off[64];
        int sret_call_n = 0;

        /* Detect sret: struct/union return > 8 bytes */
        {
            Type *rt = f->ret_type;
            if (rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) &&
                type_size(rt) > 8) {
                is_sret = 1;
                local_size += 4;  /* slot for sret pointer */
                sret_off = local_size - 4;
            }
        }

        /* alias function: emit .global / .set directives, do not generate a function body */
        if (f->alias_target) {
            asmbuf_puts(&buf, ".global ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
            asmbuf_puts(&buf, ".set ");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", ");
            asmbuf_puts(&buf, f->alias_target);
            asmbuf_puts(&buf, "\n\n");
            continue;
        }

        /* --split_sections/-ffunction-sections: each function gets its own section .text.<name>,
         * so the linker can dead-strip unused code (--gc-sections). alias functions do not generate a section (only .set). */
        if (g_config.split_sections && !f->alias_target) {
            asmbuf_puts(&buf, ".section .text.");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", \"ax\", %progbits\n");
        }
        asmbuf_puts(&buf, ".global ");
        asmbuf_puts(&buf, f->name);
        asmbuf_putc(&buf, '\n');
        if (f->is_weak) {
            /* __weak: declare weak symbol before the label, allows override by a same-name strong symbol at link time */
            asmbuf_puts(&buf, ".weak ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
        }
        if (f->is_used) {
            /* __attribute__((used)): comment marker only; .global already ensures symbol export */
            asmbuf_puts(&buf, "@ __attribute__((used))\n");
        }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        /* armcc 5 compatibility function modifiers: comment markers only (ARM backend does not change calling convention).
         *   __value_in_regs / __pure / __interwork / __smc(n) / __global_reg(n) */
        if (f->is_value_in_regs) asmbuf_puts(&buf, "@ __value_in_regs\n");
        if (f->is_pure)          asmbuf_puts(&buf, "@ __pure\n");
        if (f->is_const_attr)    asmbuf_puts(&buf, "@ __attribute__((const))\n");
        if (f->is_interwork)     asmbuf_puts(&buf, "@ __interwork\n");
        if (f->smc_num > 0) {
            asmbuf_puts(&buf, "@ __smc(");
            asmbuf_int(&buf, f->smc_num);
            asmbuf_puts(&buf, ")\n");
        }
        if (f->global_reg >= 0) {
            asmbuf_puts(&buf, "@ __global_reg(");
            asmbuf_int(&buf, f->global_reg);
            asmbuf_puts(&buf, ")\n");
        }
#endif
        /* __asm whole-function body: emit label and user's raw assembly, skip prologue/epilogue */
        if (f->is_asm_func) {
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ":\n");
            if (f->asm_body) {
                const char *s = f->asm_body;
                /* Copy text line by line; add leading indent for GAS friendliness */
                int line_start = 1;
                while (*s) {
                    if (line_start) {
                        /* Skip leading whitespace so we can re-indent */
                        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
                        if (*s == 0) break;
                        if (*s == '\n') { asmbuf_putc(&buf, '\n'); s++; continue; }
                        asmbuf_puts(&buf, "    ");
                        line_start = 0;
                    }
                    if (*s == '\n') { asmbuf_putc(&buf, '\n'); line_start = 1; s++; continue; }
                    asmbuf_putc(&buf, *s);
                    s++;
                }
                if (!line_start) asmbuf_putc(&buf, '\n');
            }
            asmbuf_putc(&buf, '\n');
            continue;
        }
#if ARKCC_FEATURE_DWARF
        if (dbg) {
            asmbuf_puts(&buf, ".type ");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", %function\n");
        }
#endif
        /* Pre-scan IR for sret calls: reserve a buffer in the frame for each.
         * The buffer is addressed via r6 (frame pointer) at the call site,
         * so SP stays at the stack args (callee reads them at SP+0). */
        {
            int sci;
            for (sci = 0; sci < f->code_count && sret_call_n < 64; sci++) {
                IrInst *sci_I = &f->code[sci];
                if (sci_I->op == IR_CALL && sci_I->name && sci_I->name[0] != '?') {
                    if (sret_lookup(sret_names, sret_sizes, sret_count, sci_I->name)) {
                        int sz = sret_get_size(sret_names, sret_sizes, sret_count, sci_I->name);
                        local_size += sz;
                        sret_call_buf_off[sret_call_n] = local_size - sz;
                        sret_call_n++;
                    }
                }
            }
        }
        /* AAPCS: SP must be 8-byte aligned at BL. irgen aligns f->local_size
         * to 8, but the sret adjustments above (sret pointer slot +4, sret call
         * buffers +sz) can leave local_size at 4 mod 8. Round up to 8 so the
         * prologue's SUB sp,sp,#local_size preserves the 8-byte alignment that
         * IR_CALL's need_align parity check relies on. The padding sits at the
         * top of the frame (between the last variable and saved regs); all
         * r6-relative offsets remain valid because the prologue SUB and the
         * stack-arg offset computation both use this padded value. */
        local_size = (local_size + 7) & ~7;
        emit_prologue(&buf, f->name, local_size, f->is_irq, f->is_naked, dbg);
        /* #2 FIX: Auto-call __arkcc_init_globals at the start of Reset_Handler
         * to initialize .data and zero .bss before main() runs. */
        if (ark_streq(f->name, "Reset_Handler") &&
            (g_data_sram_start != g_data_sram_end ||
             g_bss_sram_start != g_bss_sram_end)) {
            asmbuf_puts(&buf, "    BL __arkcc_init_globals\n");
        }
        /* Save sret pointer (r0) to frame slot for struct-returning functions */
        if (is_sret) {
            asmbuf_puts(&buf, "    STR r0, [r6, #");
            asmbuf_int(&buf, sret_off);
            asmbuf_puts(&buf, "]\n");
        }

        {
        int cur_line = 0;
        int sret_call_idx = 0;
        /* Precompute AAPCS register assignment for each param slot.
         * Maps slot index (cumulative 4-byte slot count) to AAPCS register
         * number (r0-r3), accounting for 64-bit even-pair alignment.
         * -1 = slot not used (gap in struct-by-value slot space). */
        int aapcs_map[64];
        {
            int aapcs_slot = 0;
            int aapcs_reg = is_sret ? 1 : 0;  /* sret: r0=ptr, args start at r1 */
            int pj;
            for (pj = 0; pj < 64; pj++) aapcs_map[pj] = -1;
            for (pj = 0; pj < f->param_count; pj++) {
                Type *pt = (f->param_types && pj < f->param_count)
                           ? f->param_types[pj] : 0;
                int psz = pt ? type_size(pt) : 4;
                int is_sv = (pt && (pt->kind == TYPE_STRUCT ||
                                    pt->kind == TYPE_UNION) && psz > 8);
                if (psz <= 0) psz = 4;
                if (is_sv) {
                    /* Struct-by-value: 1 register holds pointer, sz/4 slots */
                    if (aapcs_slot < 64) aapcs_map[aapcs_slot] = aapcs_reg;
                    aapcs_reg += 1;
                    aapcs_slot += (psz + 3) / 4;
                } else if (psz > 4) {
                    /* 64-bit: align to even register pair (r0/r1 or r2/r3) */
                    if (aapcs_reg & 1) aapcs_reg++;
                    if (aapcs_slot < 64) aapcs_map[aapcs_slot] = aapcs_reg;
                    if (aapcs_slot + 1 < 64) aapcs_map[aapcs_slot + 1] = aapcs_reg + 1;
                    aapcs_reg += 2;
                    aapcs_slot += 2;
                } else {
                    if (aapcs_slot < 64) aapcs_map[aapcs_slot] = aapcs_reg;
                    aapcs_reg += 1;
                    aapcs_slot += 1;
                }
            }
        }
        for (i = 0; i < f->code_count; i++) {
            IrInst *I = &f->code[i];
#if ARKCC_FEATURE_DWARF
            /* DWARF: emit .loc 1 <line> when the line number changes */
            if (dbg && I->line != 0 && I->line != cur_line) {
                cur_line = I->line;
                asmbuf_puts(&buf, ".loc 1 ");
                asmbuf_int(&buf, cur_line);
                asmbuf_putc(&buf, '\n');
            }
#endif
            switch (I->op) {
                case IR_FUNC_BEGIN:
                case IR_FUNC_END:
                    break;
                case IR_GETARG: {
                    /* AAPCS: first 4 args in r0-r3, args 5+ on caller's stack.
                     * For sret functions, r0 holds the hidden sret pointer;
                     * real args start at r1. The aapcs_map[] (precomputed above)
                     * already accounts for sret by starting at r1, so arg_idx
                     * is used directly as the aapcs_map key — NO manual shift.
                     *
                     * Stack layout (upward from r6 = frame pointer):
                     *   [r6 + 0 .. local_size-1]            this frame's local vars
                     *   [r6 + local_size + 0..15]           saved r5/r6/r7/lr
                     *   [r6 + local_size + 16 + k*4]        the (5+k)th stack arg
                     *
                     * aux encoding: low 8 bits = slot index, high 24 bits = type size.
                     *   st_sz > 8  => struct-by-value: reg/stack holds a *pointer*
                     *   st_sz == 8 => 64-bit scalar (long long / double): two words
                     *   st_sz <= 4 => 32-bit or smaller: one word
                     * aapcs_map[slot] gives the AAPCS register (0-3) or -1 for gaps.
                     * For stack args, register number is 4+; offset = (reg-4)*4. */
                    int arg_idx = I->aux & 0xFF;
                    int st_sz   = (I->aux >> 8) & 0xFFFFFF;
                    int dst_off = I->val;
                    int reg     = (arg_idx < 64) ? aapcs_map[arg_idx] : -1;
                    if (dst_off >= 8000) break;  /* variadic region: C-backend only */
                    if (st_sz > 8) {
                        /* Struct-by-value (> 8 bytes): register/stack holds a
                         * *pointer* to the source struct. memcpy into frame[dst_off]. */
                        if (reg >= 0 && reg < 4) {
                            asmbuf_puts(&buf, "    MOV r3, r");
                            asmbuf_int(&buf, reg);
                            asmbuf_puts(&buf, "\n");
                        } else {
                            int stack_off = local_size + 16 + (reg - 4) * 4;
                            asmbuf_puts(&buf, "    LDR r3, [r6, #");
                            asmbuf_int(&buf, stack_off);
                            asmbuf_puts(&buf, "]\n");
                        }
                        if (st_sz <= 16) {
                            int off;
                            for (off = 0; off < st_sz; off += 4) {
                                asmbuf_puts(&buf, "    LDR r0, [r3, #");
                                asmbuf_int(&buf, off);
                                asmbuf_puts(&buf, "]\n");
                                asmbuf_puts(&buf, "    STR r0, [r6, #");
                                asmbuf_int(&buf, dst_off + off);
                                asmbuf_puts(&buf, "]\n");
                            }
                        } else {
                            asmbuf_puts(&buf, "    ADD r0, r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "\n    MOV r1, r3\n");
                            emit_mov_imm(&buf, 2, st_sz);
                            asmbuf_puts(&buf, "    PUSH {lr}\n    BL memcpy\n    POP {r0}\n");
                        }
                    } else if (st_sz == 8) {
                        /* 64-bit scalar (long long / double): read two words from
                         * the even register pair (r0/r1 or r2/r3) or caller's stack. */
                        int reg_hi = (arg_idx + 1 < 64) ? aapcs_map[arg_idx + 1] : -1;
                        /* Low word */
                        if (reg >= 0 && reg < 4) {
                            asmbuf_puts(&buf, "    STR r");
                            asmbuf_int(&buf, reg);
                            asmbuf_puts(&buf, ", [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        } else {
                            int soff = local_size + 16 + (reg - 4) * 4;
                            asmbuf_puts(&buf, "    LDR r0, [r6, #");
                            asmbuf_int(&buf, soff);
                            asmbuf_puts(&buf, "]\n    STR r0, [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        }
                        /* High word */
                        if (reg_hi >= 0 && reg_hi < 4) {
                            asmbuf_puts(&buf, "    STR r");
                            asmbuf_int(&buf, reg_hi);
                            asmbuf_puts(&buf, ", [r6, #");
                            asmbuf_int(&buf, dst_off + 4);
                            asmbuf_puts(&buf, "]\n");
                        } else {
                            int soff = local_size + 16 + (reg_hi - 4) * 4;
                            asmbuf_puts(&buf, "    LDR r0, [r6, #");
                            asmbuf_int(&buf, soff);
                            asmbuf_puts(&buf, "]\n    STR r0, [r6, #");
                            asmbuf_int(&buf, dst_off + 4);
                            asmbuf_puts(&buf, "]\n");
                        }
                    } else {
                        /* 32-bit or smaller: one word */
                        if (reg >= 0 && reg < 4) {
                            asmbuf_puts(&buf, "    STR r");
                            asmbuf_int(&buf, reg);
                            asmbuf_puts(&buf, ", [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        } else {
                            int soff = local_size + 16 + (reg - 4) * 4;
                            asmbuf_puts(&buf, "    LDR r0, [r6, #");
                            asmbuf_int(&buf, soff);
                            asmbuf_puts(&buf, "]\n    STR r0, [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        }
                    }
                    break;
                }
                case IR_PUSH_INT:
                    emit_mov_imm(&buf, 0, I->val);
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_STR:
                    asmbuf_puts(&buf, "    LDR r0, =.L_str_");
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_VAR: {
                    /* aux encodes width + signedness:
                     *   1 = char (LDRSB signed / LDRB unsigned)
                     *   2 = short (LDRSH signed / LDRH unsigned)
                     *   4 or 0 = int/ptr (LDR 4 bytes)
                     *   8 = 64-bit (long long / double): load two words, push both
                     *   0x100 bit = unsigned
                     *   0x10000 bit (IR_AUX_VOLATILE) = volatile read: emit
                     *     DMB SY barriers before and after to enforce ordering. */
                    int width = I->aux & 0xFF;
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int is_volatile = (I->aux & IR_AUX_VOLATILE) != 0;
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: order prior memory ops */
                    if (width == 1) {
                        asmbuf_puts(&buf, is_unsigned
                            ? "    LDRB r0, [r6, #"
                            : "    LDRSB r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);
                    } else if (width == 2) {
                        asmbuf_puts(&buf, is_unsigned
                            ? "    LDRH r0, [r6, #"
                            : "    LDRSH r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);
                    } else if (width == 8) {
                        /* #5 FIX: 64-bit local variable read - load low and high words.
                         * Stack model: push low word first (bottom), then high word (top). */
                        asmbuf_puts(&buf, "    LDR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n    LDR r1, [r6, #");
                        asmbuf_int(&buf, I->val + 4);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);  /* push low word */
                        asmbuf_puts(&buf, "    PUSH {r1}\n");  /* push high word */
                    } else {
                        asmbuf_puts(&buf, "    LDR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);
                    }
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: order subsequent memory ops */
                    break;
                }
                case IR_PUSH_ADDR:
                    asmbuf_puts(&buf, "    ADD r0, r6, #");
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_GLOBAL: {
                    unsigned addr = gvar_find(I->name);
                    int gsz = gvar_find_size(I->name);
                    if (gsz >= 8) {
                        /* #6 FIX: 64-bit global read - load low and high words */
                        emit_mov_imm(&buf, 1, (int)addr);
                        asmbuf_puts(&buf, "    LDR r0, [r1, #0]\n");
                        asmbuf_puts(&buf, "    LDR r2, [r1, #4]\n");
                        emit_push_r0(&buf);  /* push low word */
                        asmbuf_puts(&buf, "    PUSH {r2}\n");  /* push high word */
                    } else {
                        emit_mov_imm(&buf, 0, (int)addr);
                        emit_line(&buf, "LDR r0, [r0]");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_TRUNC:
                    /* val = target width, aux = 1 zero-extend (unsigned).
                     * 32-bit widths need no work: the stack word already holds
                     * exactly the truncated 32-bit pattern. */
                    if (I->val == 8 || I->val == 16) {
                        char op8[8];
                        op8[0] = I->aux ? 'U' : 'S';
                        op8[1] = 'X';
                        op8[2] = 'T';
                        op8[3] = (I->val == 8) ? 'B' : 'H';
                        op8[4] = 0;
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    ");
                        asmbuf_puts(&buf, op8);
                        asmbuf_puts(&buf, " r0, r0\n");
                        emit_push_r0(&buf);
                    }
                    break;
                case IR_PUSH_FUNC:
                    /* Address of a function = its label. Use the literal-pool
                     * pseudo-instruction the string/double pools already use. */
                    asmbuf_puts(&buf, "    LDR r0, =");
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_GLOBAL_ADDR:
                    /* Take the global symbol address only (no dereference), for array/struct decay
                     * and compound literal scenarios. */
                    {
                        unsigned addr = gvar_find(I->name);
                        emit_mov_imm(&buf, 0, (int)addr);
                        emit_push_r0(&buf);
                    }
                    break;
                case IR_STORE_GLOBAL: {
                    unsigned addr = gvar_find(I->name);
                    int gsz = gvar_find_size(I->name);
                    if (gsz >= 8) {
                        /* #6 FIX: 64-bit global store - pop high then low, store both */
                        asmbuf_puts(&buf, "    POP {r1}\n");  /* high word */
                        emit_pop_r0(&buf);                     /* low word */
                        emit_mov_imm(&buf, 2, (int)addr);
                        asmbuf_puts(&buf, "    STR r0, [r2, #0]\n");
                        asmbuf_puts(&buf, "    STR r1, [r2, #4]\n");
                    } else {
                        emit_pop_r0(&buf);
                        emit_mov_imm(&buf, 1, (int)addr);
                        emit_line(&buf, "STR r0, [r1]");
                    }
                    break;
                }
                case IR_POP:
                    emit_pop_r0(&buf);
                    break;
                case IR_DUP:
                    emit_pop_r0(&buf);
                    emit_push_r0(&buf);
                    emit_push_r0(&buf);
                    break;
                case IR_SWAP:
                    /* Pop top two values and push them back in swapped order.
                     * r0 = top, r1 = next -> push r0, push r1 */
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, "    POP {r1}\n");
                    emit_push_r0(&buf);
                    asmbuf_puts(&buf, "    PUSH {r1}\n");
                    break;
                case IR_SEXT:
                case IR_ZEXT:
                case IR_CDECL64:
                    /* x86-cdecl-specific 64-bit arg passing conversion.
                     * ARM AAPCS passes 64-bit values in register pairs or
                     * 8-byte aligned stack slots as a unit; no conversion
                     * needed. Treat as no-op. */
                    break;
                case IR_STORE: {
                    /* aux encodes store width: 1=byte, 2=halfword, 4=word,
                     * 8=double word (two STRs), 16=complex (two 8-byte).
                     * Default (0) = 4 bytes.
                     * N1 FIX: mask off IR_AUX_VOLATILE (0x10000) bit before
                     * checking width — volatile uint32_t has aux=0x10004,
                     * which would wrongly match sw>=8 and sw>=16, causing
                     * 16-byte writes into an 8-byte slot and corrupting
                     * saved registers on the stack.
                     * N5 FIX: volatile stores emit DMB SY before (release) and
                     * after (full barrier) to enforce memory ordering. */
                    int sw = I->aux & ~IR_AUX_VOLATILE;
                    int is_volatile = (I->aux & IR_AUX_VOLATILE) != 0;
                    if (sw == 0) sw = 4;
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: release barrier */
                    if (sw == 1) {
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    STRB r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                    } else if (sw == 2) {
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    STRH r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                    } else if (sw >= 8) {
                        /* 8+ bytes: pop high word then low word, store both */
                        asmbuf_puts(&buf, "    POP {r1}\n");  /* high word */
                        emit_pop_r0(&buf);                     /* low word */
                        asmbuf_puts(&buf, "    STR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        asmbuf_puts(&buf, "    STR r1, [r6, #");
                        asmbuf_int(&buf, I->val + 4);
                        asmbuf_puts(&buf, "]\n");
                        if (sw >= 16) {
                            asmbuf_puts(&buf, "    POP {r1}\n");
                            emit_pop_r0(&buf);
                            asmbuf_puts(&buf, "    STR r0, [r6, #");
                            asmbuf_int(&buf, I->val + 8);
                            asmbuf_puts(&buf, "]\n");
                            asmbuf_puts(&buf, "    STR r1, [r6, #");
                            asmbuf_int(&buf, I->val + 12);
                            asmbuf_puts(&buf, "]\n");
                        }
                    } else {
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    STR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                    }
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: full barrier */
                    break;
                }
                case IR_LOAD_IDX: {
                    int es = I->val;
                    /* Bug-I5 FIX: mask off IR_AUX_VOLATILE (0x10000) before
                     * checking the packed flag. The volatile bit is carried for
                     * optimizer/host-compiler correctness and does not affect
                     * the ARM load encoding. */
                    int packed = (I->aux & 0x400) ? 1 : 0;
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    if (es > 1) {
                        emit_mov_imm(&buf, 2, es);
                        emit_line(&buf, "MUL r1, r2");
                    }
                    emit_line(&buf, "ADD r0, r0, r1");
                    if (es == 1) {
                        emit_line(&buf, "LDRB r0, [r0, #0]");
                    } else if (es == 2 && !packed) {
                        emit_line(&buf, "LDRSH r0, [r0, #0]");
                    } else if (packed) {
                        /* __packed pointer unaligned access: load byte by byte then assemble.
                         * es==2: LDRB x2 + LSL + ORR
                         * es==4: LDRB x4 + LSL + ORR chain
                         * r0=address, r2/r3 are scratch registers */
                        if (es == 2) {
                            emit_line(&buf, "LDRB r2, [r0, #0]");
                            emit_line(&buf, "LDRB r3, [r0, #1]");
                            emit_line(&buf, "LSL r3, r3, #8");
                            emit_line(&buf, "ORR r0, r2, r3");
                        } else {
                            /* Default to 4-byte handling (es>=4 takes the low 4 bytes) */
                            emit_line(&buf, "LDRB r2, [r0, #0]");
                            emit_line(&buf, "LDRB r3, [r0, #1]");
                            emit_line(&buf, "LSL r3, r3, #8");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #2]");
                            emit_line(&buf, "LSL r3, r3, #16");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #3]");
                            emit_line(&buf, "LSL r3, r3, #24");
                            emit_line(&buf, "ORR r0, r2, r3");
                        }
                    } else if (es == 8) {
                        /* #A4 FIX: 8-byte element (long long/double): load low and high words.
                         * Stack convention (from IR_STORE): high word on top, low below.
                         * So push low first, then high. */
                        emit_line(&buf, "LDR r2, [r0, #0]");  /* r2 = low word */
                        emit_line(&buf, "LDR r3, [r0, #4]");  /* r3 = high word */
                        emit_line(&buf, "PUSH {r2}");  /* push low word */
                        emit_line(&buf, "PUSH {r3}");  /* push high word (on top) */
                        break;
                    } else {
                        emit_line(&buf, "LDR r0, [r0, #0]");
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_STORE_IDX: {
                    int es = I->val;
                    /* Bug-I5 FIX: mask off IR_AUX_VOLATILE (0x10000) before
                     * checking the packed flag. The volatile bit is carried for
                     * optimizer/host-compiler correctness and does not affect
                     * the ARM store encoding. */
                    int packed = (I->aux & 0x400) ? 1 : 0;
                    if (es == 8) {
                        /* #A4 FIX: 8-byte element (long long/double): stack has
                         * [addr, value_low, value_high] (high on top, per IR_STORE
                         * convention). Pop high, low, addr; store both; push back. */
                        emit_line(&buf, "POP {r1}");          /* r1 = high word (top) */
                        emit_pop_r0(&buf);                     /* r0 = low word */
                        emit_line(&buf, "POP {r2}");          /* r2 = addr */
                        emit_line(&buf, "STR r0, [r2, #0]");
                        emit_line(&buf, "STR r1, [r2, #4]");
                        /* Push value back as expression result: low then high */
                        emit_line(&buf, "PUSH {r0}");
                        emit_line(&buf, "PUSH {r1}");
                        break;
                    }
                    emit_pop_r0(&buf);
                    emit_pop_r1(&buf);
                    if (es == 1) {
                        emit_line(&buf, "STRB r0, [r1, #0]");
                    } else if (es == 2 && !packed) {
                        emit_line(&buf, "STRH r0, [r1, #0]");
                    } else if (packed) {
                        /* __packed pointer unaligned store: write byte by byte.
                         * r0=value, r1=address, r2 is a scratch register */
                        if (es == 2) {
                            emit_line(&buf, "STRB r0, [r1, #0]");
                            emit_line(&buf, "LSR r2, r0, #8");
                            emit_line(&buf, "STRB r2, [r1, #1]");
                        } else {
                            emit_line(&buf, "STRB r0, [r1, #0]");
                            emit_line(&buf, "LSR r2, r0, #8");
                            emit_line(&buf, "STRB r2, [r1, #1]");
                            emit_line(&buf, "LSR r2, r0, #16");
                            emit_line(&buf, "STRB r2, [r1, #2]");
                            emit_line(&buf, "LSR r2, r0, #24");
                            emit_line(&buf, "STRB r2, [r1, #3]");
                        }
                    } else {
                        emit_line(&buf, "STR r0, [r1, #0]");
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_BINARY:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_binary(&buf, I->aux);
                    emit_push_r0(&buf);
                    break;
                case IR_UNARY:
                    emit_pop_r0(&buf);
                    emit_unary(&buf, I->aux);
                    emit_push_r0(&buf);
                    break;
                case IR_LABEL:
                    emit_label(&buf, f->name, I->val);
                    break;
                case IR_LABEL_NAMED:
                    asmbuf_puts(&buf, ".L_");
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, ":\n");
                    break;
                case IR_JMP:
                    emit_b_label(&buf, "B", f->name, I->val);
                    break;
                case IR_JMP_NAMED:
                    asmbuf_puts(&buf, "    B .L_");
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_JZ:
                    emit_pop_r0(&buf);
                    emit_line(&buf, "CMP r0, #0");
                    emit_b_label(&buf, "BEQ", f->name, I->val);
                    break;
                case IR_JNZ:
                    emit_pop_r0(&buf);
                    emit_line(&buf, "CMP r0, #0");
                    emit_b_label(&buf, "BNE", f->name, I->val);
                    break;
                case IR_CMP_JMP_EQ:
                    emit_pop_r0(&buf);
                    emit_mov_imm(&buf, 1, I->val);
                    emit_line(&buf, "CMP r0, r1");
                    emit_b_label(&buf, "BEQ", f->name, I->aux);
                    break;
                /* === Floating-point ops: ARM soft-float ABI ===
                 * The high bit 0x200 of aux marks double-precision (double):
                 *   - single (float): args in r0/r1, result in r0, 1 stack slot (4B), uses __aeabi_f*
                 *   - double (double): args in r0/r1+r2/r3, result in r0/r1, 2 stack slots (8B), uses __aeabi_d*
                 * lr is already saved by the prologue; BL inside the body may freely clobber lr. */
                case IR_PUSH_FLOAT: {
                    int is_dbl = (I->aux & 0x200) != 0;
                    if (is_dbl) {
                        /* Double-precision: load 8 bytes, push 2 slots (low word pushed first) */
                        asmbuf_puts(&buf, "    LDR r0, =.L_dbl_");
                        asmbuf_int(&buf, I->val);
                        asmbuf_putc(&buf, '\n');
                        emit_line(&buf, "LDR r1, [r0]");       /* low word */
                        emit_line(&buf, "LDR r2, [r0, #4]");   /* high word */
                        asmbuf_puts(&buf, "    PUSH {r1}\n");  /* low word pushed first */
                        asmbuf_puts(&buf, "    PUSH {r2}\n");  /* high word pushed last (on top) */
                    } else {
                        /* Single-precision: load 32-bit float bit pattern from the float constant pool into r0 */
                        asmbuf_puts(&buf, "    LDR r0, =.L_flt_");
                        asmbuf_int(&buf, I->val);
                        asmbuf_putc(&buf, '\n');
                        emit_line(&buf, "LDR r0, [r0]");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FADD: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VADD");
                        else        emit_vfp_f32_binop(&buf, "VADD");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_dadd");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fadd");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FSUB: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VSUB");
                        else        emit_vfp_f32_binop(&buf, "VSUB");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_dsub");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fsub");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FMUL: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VMUL");
                        else        emit_vfp_f32_binop(&buf, "VMUL");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_dmul");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fmul");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FDIV: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VDIV");
                        else        emit_vfp_f32_binop(&buf, "VDIV");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_ddiv");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fdiv");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_F2I: {
                    /* float/double -> int (truncation toward zero) */
                    int is_dbl = (I->aux & 0x200) != 0;
                    /* F2I unsigned fix: 0x400 = unsigned destination; select
                     * __aeabi_d2uiz/__aeabi_f2uiz for unsigned targets. */
                    int is_unsigned = (I->aux & 0x400) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) {
                            /* double: pop d0, VCVT.S32.F64 s0,d0, move back to r0 */
                            emit_line(&buf, "POP {r1}");  /* high */
                            emit_line(&buf, "POP {r0}");  /* low */
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.U32.F64 s0, d0");
                            else
                                emit_line(&buf, "VCVT.S32.F64 s0, d0");
                            emit_line(&buf, "VMOV r0, s0");
                        } else {
                            emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.U32.F32 s0, s0");
                            else
                                emit_line(&buf, "VCVT.S32.F32 s0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                        }
                        emit_push_r0(&buf);
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        if (is_unsigned)
                            emit_line(&buf, "BL __aeabi_d2uiz");
                        else
                            emit_line(&buf, "BL __aeabi_d2iz");
                    } else {
                        emit_pop_r0(&buf);
                        if (is_unsigned)
                            emit_line(&buf, "BL __aeabi_f2uiz");
                        else
                            emit_line(&buf, "BL __aeabi_f2iz");
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_I2F: {
                    /* int -> float/double */
                    int is_dbl = (I->aux & 0x200) != 0;
                    /* Bug-P1-7: 0x400 = unsigned source; select __aeabi_ui2d /
                     * __aeabi_ui2f instead of the signed __aeabi_i2d /
                     * __aeabi_i2f so that (double)(unsigned)v yields the
                     * correct positive value. */
                    int is_unsigned = (I->aux & 0x400) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        emit_pop_r0(&buf);
                        emit_line(&buf, "VMOV s0, r0");
                        if (is_dbl) {
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.F64.U32 d0, s0");
                            else
                                emit_line(&buf, "VCVT.F64.S32 d0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_line(&buf, "VMOV r1, s1");
                            emit_push_r0(&buf);
                            asmbuf_puts(&buf, "    PUSH {r1}\n");
                        } else {
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.F32.U32 s0, s0");
                            else
                                emit_line(&buf, "VCVT.F32.S32 s0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_push_r0(&buf);
                        }
                        break;
                    }
#endif
                    emit_pop_r0(&buf);
                    if (is_dbl) {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ui2d" : "BL __aeabi_i2d");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ui2f" : "BL __aeabi_i2f");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_64I2F: {
                    /* long long (64-bit) -> float/double.
                     * Stack model: [r0_low, r1_high] (high on top).
                     * AAPCS helpers: __aeabi_l2d/ul2d (-> r0/r1 double),
                     *                __aeabi_l2f/ul2f (-> r0 float). */
                    int is_dbl = (I->aux & 0x200) != 0;
                    int is_unsigned = (I->aux & 0x400) != 0;
                    emit_pop_r1(&buf);  /* high */
                    emit_pop_r0(&buf);  /* low */
                    if (is_dbl) {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ul2d" : "BL __aeabi_l2d");
                        /* double result: r0=low, r1=high */
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ul2f" : "BL __aeabi_l2f");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_64F2I: {
                    /* float/double -> long long (64-bit).
                     * For double: stack has [r0_low, r1_high]; pop both.
                     * For float:  stack has [r0]; pop one.
                     * AAPCS helpers: __aeabi_d2lz/d2ulz (double -> r0/r1),
                     *                __aeabi_f2lz/f2ulz (float  -> r0/r1). */
                    int is_dbl = (I->aux & 0x200) != 0;
                    int is_unsigned = (I->aux & 0x400) != 0;
                    if (is_dbl) {
                        emit_pop_r1(&buf);  /* high */
                        emit_pop_r0(&buf);  /* low */
                        emit_line(&buf, is_unsigned ? "BL __aeabi_d2ulz" : "BL __aeabi_d2lz");
                    } else {
                        emit_pop_r0(&buf);
                        emit_line(&buf, is_unsigned ? "BL __aeabi_f2ulz" : "BL __aeabi_f2lz");
                    }
                    /* 64-bit result: r0=low, r1=high */
                    emit_push_r0(&buf);
                    asmbuf_puts(&buf, "    PUSH {r1}\n");
                    break;
                }
                case IR_F2F: {
                    /* float <-> float cast. On the ARM soft-float stack:
                     *   double = [r0_low, r1_high] (2 slots, high on top)
                     *   float  = [r0]              (1 slot)
                     *   aux 0x200 = double->float (narrow): -1 slot
                     *   aux 0x100 = float->double (widen):  +1 slot
                     * AAPCS helpers: __aeabi_d2f (r0/r1 -> r0),
                     *                __aeabi_f2d (r0 -> r0/r1). */
                    int src_dbl = (I->aux & 0x200) != 0;
                    int dst_dbl = (I->aux & 0x100) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (src_dbl && !dst_dbl) {
                            /* double -> float */
                            emit_pop_r1(&buf);
                            emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            emit_line(&buf, "VCVT.F32.F64 s0, d0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_push_r0(&buf);
                        } else if (!src_dbl && dst_dbl) {
                            /* float -> double */
                            emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VCVT.F64.F32 d0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_line(&buf, "VMOV r1, s1");
                            emit_push_r0(&buf);
                            asmbuf_puts(&buf, "    PUSH {r1}\n");
                        }
                        break;
                    }
#endif
                    if (src_dbl && !dst_dbl) {
                        /* double -> float */
                        emit_pop_r1(&buf);
                        emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_d2f");
                        emit_push_r0(&buf);
                    } else if (!src_dbl && dst_dbl) {
                        /* float -> double */
                        emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_f2d");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    }
                    /* same-size: no-op */
                    break;
                }
                case IR_FCMP: {
                    /* aux encodes the comparison kind (low 8 bits) + precision flag (0x200);
                     * __aeabi_fcmpeq and __aeabi_dcmpeq family return 0/1 */
                    int op = I->aux & ~0x200;
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        const char *cond = "EQ";
                        if (is_dbl) {
                            emit_line(&buf, "POP {r3}");
                            emit_line(&buf, "POP {r2}");
                            emit_line(&buf, "POP {r1}");
                            emit_line(&buf, "POP {r0}");
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            emit_line(&buf, "VMOV s2, r2");
                            emit_line(&buf, "VMOV s3, r3");
                            emit_line(&buf, "VCMP.F64 d0, d1");
                        } else {
                            emit_pop_r1(&buf); emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            emit_line(&buf, "VCMP.F32 s0, s1");
                        }
                        emit_line(&buf, "VMRS APSR_nzcv, FPSCR");
                        emit_mov_imm(&buf, 0, 0);
                        switch (op) {
                            case (int)TOK_EQ: cond = "EQ"; break;
                            case (int)TOK_NE: cond = "NE"; break;
                            case (int)TOK_LT: cond = "LT"; break;
                            case (int)TOK_GT: cond = "GT"; break;
                            case (int)TOK_LE: cond = "LE"; break;
                            case (int)TOK_GE: cond = "GE"; break;
                            default: cond = "EQ"; break;
                        }
                        asmbuf_puts(&buf, "    IT ");
                        asmbuf_puts(&buf, cond);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, "    MOV");
                        asmbuf_puts(&buf, cond);
                        asmbuf_puts(&buf, " r0, #1\n");
                        emit_push_r0(&buf);
                        break;
                    }
#endif
                    {
                    const char *helper = "__aeabi_fcmpeq";
                    switch (op) {
                        case (int)TOK_EQ: helper = is_dbl ? "__aeabi_dcmpeq" : "__aeabi_fcmpeq"; break;
                        case (int)TOK_NE: helper = is_dbl ? "__aeabi_dcmpeq" : "__aeabi_fcmpeq"; break;
                        case (int)TOK_LT: helper = is_dbl ? "__aeabi_dcmplt" : "__aeabi_fcmplt"; break;
                        case (int)TOK_GT: helper = is_dbl ? "__aeabi_dcmpgt" : "__aeabi_fcmpgt"; break;
                        case (int)TOK_LE: helper = is_dbl ? "__aeabi_dcmple" : "__aeabi_fcmple"; break;
                        case (int)TOK_GE: helper = is_dbl ? "__aeabi_dcmpge" : "__aeabi_fcmpge"; break;
                        default: break;
                    }
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                    }
                    asmbuf_puts(&buf, "    BL ");
                    asmbuf_puts(&buf, helper);
                    asmbuf_putc(&buf, '\n');
                    /* NE: use fcmpeq/dcmpeq then flip the low bit */
                    if (op == (int)TOK_NE) emit_line(&buf, "EOR r0, r0, #1");
                    emit_push_r0(&buf);
                    break;
                    }
                }
                case IR_ASM:
                    /* Inline assembly written out as-is.
                     * Extended form with '@' prefix (assembled by irgen): skip past the quote and write the template,
                     * ignoring outputs/inputs/clobbers; also replace %0/%1/... with
                     * r0/r1/... (AAPCS operand index maps to register, armcc-style substitution). */
                    if (I->name && I->name[0] == '@') {
                        const char *s = I->name + 1;
                        /* Skip whitespace to find the first '"' */
                        while (*s == ' ' || *s == '\t') s++;
                        if (*s == '"') {
                            s++;
                            asmbuf_puts(&buf, "    ");
                            /* Copy template content (handle \n / \t escapes), stop at an unescaped '"' */
                            while (*s && *s != '"') {
                                if (*s == '\\' && s[1]) {
                                    if (s[1] == 'n') { asmbuf_putc(&buf, '\n'); asmbuf_puts(&buf, "    "); }
                                    else if (s[1] == 't') asmbuf_putc(&buf, '\t');
                                    else if (s[1] == '\\') asmbuf_putc(&buf, '\\');
                                    else if (s[1] == '"') asmbuf_putc(&buf, '"');
                                    else asmbuf_putc(&buf, s[1]);
                                    s += 2;
                                } else if (*s == '%' && s[1] >= '0' && s[1] <= '3') {
                                    /* Extended inline asm operand substitution: %0 -> r0, %1 -> r1, ... */
                                    asmbuf_putc(&buf, 'r');
                                    asmbuf_putc(&buf, s[1]);
                                    s += 2;
                                } else {
                                    asmbuf_putc(&buf, *s);
                                    s++;
                                }
                            }
                            asmbuf_putc(&buf, '\n');
                        }
                    } else if (I->name) {
                        asmbuf_puts(&buf, "    ");
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                    }
                    break;
                case IR_CALL: {
                    /* AAPCS: first 4 args go in r0-r3, args 5+ go on the stack.
                     * The stack-based IR pushes all args onto the stack (arg0 on top); here we pop the first 4
                     * into r0-r3, leaving the remaining stack_args on the stack for the callee to read.
                     * After the call the caller cleans up the stack args (caller cleanup).
                     * P1 FIX: When stack_args is odd, a 4-byte padding is inserted before BL
                     * and removed after, ensuring 8-byte SP alignment at BL per AAPCS. */
                    int n = I->val;
                    int k;
                    /* Indirect call (obj->fnptr()): I->name starts with '?'. The function
                     * pointer was pushed LAST (it's on top of stack). Pop it into ip (r12)
                     * first, then pop real args into r0-r3, then BLX ip. */
                    int is_indirect = (I->name && I->name[0] == '?');
                    int real_args = is_indirect ? n - 1 : n;
                    int is_sret_call = (!is_indirect && sret_lookup(sret_names, sret_sizes, sret_count, I->name));

                    /* armcc intrinsic functions: emit inline instruction instead of BL.
                     * Arguments (if any) are already on the stack; pop and discard them. */
                    if (!is_indirect && (ark_streq(I->name, "__nop") ||
                        ark_streq(I->name, "__isb") ||
                        ark_streq(I->name, "__dsb") ||
                        ark_streq(I->name, "__dmb") ||
                        ark_streq(I->name, "__memory_changed") ||
                        ark_streq(I->name, "__schedule_barrier") ||
                        ark_streq(I->name, "__force_stores") ||
                        ark_streq(I->name, "__disable_irq") ||
                        ark_streq(I->name, "__enable_irq") ||
                        ark_streq(I->name, "__wfi") ||
                        ark_streq(I->name, "__wfe") ||
                        ark_streq(I->name, "__sev") ||
                        ark_streq(I->name, "__clz") ||
                        ark_streq(I->name, "__rbit"))) {
                        int reg_args = (real_args > 4) ? 4 : real_args;
                        int stack_args = (real_args > 4) ? (real_args - 4) : 0;
                        for (k = 0; k < reg_args; k++) {
                            asmbuf_puts(&buf, "    POP {r");
                            asmbuf_int(&buf, k);
                            asmbuf_puts(&buf, "}\n");
                        }
                        if (stack_args > 0) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #");
                            asmbuf_int(&buf, stack_args * 4);
                            asmbuf_putc(&buf, '\n');
                        }
                        if (ark_streq(I->name, "__nop"))         asmbuf_puts(&buf, "    NOP\n");
                        else if (ark_streq(I->name, "__isb"))    asmbuf_puts(&buf, "    ISB SY\n");
                        else if (ark_streq(I->name, "__dsb"))    asmbuf_puts(&buf, "    DSB SY\n");
                        else if (ark_streq(I->name, "__dmb"))    asmbuf_puts(&buf, "    DMB SY\n");
                        else if (ark_streq(I->name, "__memory_changed")) ; /* compiler barrier: no-op */
                        else if (ark_streq(I->name, "__schedule_barrier")) ; /* CMSIS barrier fence: no instruction */
                        /* armcc __force_stores: flush the compiler's pending
                         * stores. Our codegen always emits the store where the
                         * statement is, so there is nothing to flush and no
                         * instruction is required (it is not a hardware
                         * barrier; that is __dsb). */
                        else if (ark_streq(I->name, "__force_stores")) ;
                        else if (ark_streq(I->name, "__disable_irq")) asmbuf_puts(&buf, "    CPSID i\n");
                        else if (ark_streq(I->name, "__enable_irq"))  asmbuf_puts(&buf, "    CPSIE i\n");
                        else if (ark_streq(I->name, "__wfi"))    asmbuf_puts(&buf, "    WFI\n");
                        else if (ark_streq(I->name, "__wfe"))   asmbuf_puts(&buf, "    WFE\n");
                        else if (ark_streq(I->name, "__sev"))   asmbuf_puts(&buf, "    SEV\n");
                        else if (ark_streq(I->name, "__clz"))   asmbuf_puts(&buf, "    CLZ r0, r0\n");
                        /* armcc __rbit: CMSIS __RBIT maps onto it in the
                         * __CC_ARM branch, so vendor code (HAL_CRYP) calls it
                         * with one argument; the result is uint32_t. */
                        else if (ark_streq(I->name, "__rbit"))  asmbuf_puts(&buf, "    RBIT r0, r0\n");
                        emit_push_r0(&buf);
                        break;
                    }

                    /* sret call: callee returns struct > 8 bytes via hidden pointer in r0.
                     * P1 FIX: sret buffer is pre-allocated in the caller's frame (via r6),
                     * NOT on the stack. This ensures:
                     *   1. Callee reads stack args at SP+0 (not sret space).
                     *   2. sret buffer survives the callee's PUSH (above SP, in the frame).
                     *   3. The sret pointer pushed after BL remains valid (buffer not freed).
                     * AAPCS: r0 = sret pointer, real args start at r1.
                     * 64-bit args align to even register pairs (r2/r3 only option). */
                    if (is_sret_call) {
                        int argmask = I->aux;
                        int aapcs_reg = 1;  /* r0 = sret, args start at r1 */
                        int total_slots = 0;
                        int reg_slots = 0;
                        int stack_args;
                        int need_align;
                        int buf_off = (sret_call_idx < sret_call_n)
                                      ? sret_call_buf_off[sret_call_idx++] : 0;
                        int kk;
                        for (kk = 0; kk < real_args; kk++) {
                            int is_64 = (argmask >> (2 * kk + 1)) & 1;
                            total_slots += is_64 ? 2 : 1;
                        }
                        /* Pop each arg from the IR stack into the correct AAPCS
                         * register. IR stack: arg0 on top. For 64-bit args, high
                         * word is on top, low word below (pushed first). */
                        for (kk = 0; kk < real_args; kk++) {
                            int is_64 = (argmask >> (2 * kk + 1)) & 1;
                            if (is_64) {
                                if (aapcs_reg & 1) aapcs_reg++;  /* even-pair align */
                                if (aapcs_reg + 1 <= 3) {
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aapcs_reg + 1);
                                    asmbuf_puts(&buf, "}\n");
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aapcs_reg);
                                    asmbuf_puts(&buf, "}\n");
                                    reg_slots += 2;
                                    aapcs_reg += 2;
                                } else {
                                    break;  /* both words stay on stack */
                                }
                            } else {
                                if (aapcs_reg <= 3) {
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aapcs_reg);
                                    asmbuf_puts(&buf, "}\n");
                                    reg_slots += 1;
                                    aapcs_reg += 1;
                                } else {
                                    break;
                                }
                            }
                        }
                        stack_args = total_slots - reg_slots;
                        if (stack_args < 0) stack_args = 0;
                        need_align = (stack_args & 1) != 0;
                        /* r0 = sret buffer address (pre-allocated in caller's frame) */
                        asmbuf_puts(&buf, "    ADD r0, r6, #");
                        asmbuf_int(&buf, buf_off);
                        asmbuf_putc(&buf, '\n');
                        if (need_align) {
                            asmbuf_puts(&buf, "    SUB sp, sp, #4\n");
                        }
                        asmbuf_puts(&buf, "    BL ");
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                        if (need_align) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #4\n");
                        }
                        if (stack_args > 0) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #");
                            asmbuf_int(&buf, stack_args * 4);
                            asmbuf_putc(&buf, '\n');
                        }
                        emit_push_r0(&buf);
                        break;
                    }

                    /* Regular (non-sret) call.
                     * AAPCS: first 4 args in r0-r3, 64-bit args aligned to even
                     * register pairs (r0/r1 or r2/r3). Remaining args on stack.
                     * argmask bit(2*k+1)=1 marks 8-byte args (long long / double). */
                    {
                    int argmask = I->aux;
                    int aapcs_reg = 0;
                    int total_slots = 0;
                    int reg_slots = 0;
                    int stack_args;
                    int need_align;
                    int kk;

                    /* Indirect call: pop function pointer into ip (r12) first */
                    if (is_indirect) {
                        asmbuf_puts(&buf, "    POP {ip}\n");
                        asmbuf_puts(&buf, "    ORR ip, ip, #1\n");
                    }
                    for (kk = 0; kk < real_args; kk++) {
                        int is_64 = (argmask >> (2 * kk + 1)) & 1;
                        total_slots += is_64 ? 2 : 1;
                    }
                    for (kk = 0; kk < real_args; kk++) {
                        int is_64 = (argmask >> (2 * kk + 1)) & 1;
                        if (is_64) {
                            if (aapcs_reg & 1) aapcs_reg++;  /* even-pair align */
                            if (aapcs_reg + 1 <= 3) {
                                asmbuf_puts(&buf, "    POP {r");
                                asmbuf_int(&buf, aapcs_reg + 1);
                                asmbuf_puts(&buf, "}\n");
                                asmbuf_puts(&buf, "    POP {r");
                                asmbuf_int(&buf, aapcs_reg);
                                asmbuf_puts(&buf, "}\n");
                                reg_slots += 2;
                                aapcs_reg += 2;
                            } else {
                                break;  /* remaining args stay on stack */
                            }
                        } else {
                            if (aapcs_reg <= 3) {
                                asmbuf_puts(&buf, "    POP {r");
                                asmbuf_int(&buf, aapcs_reg);
                                asmbuf_puts(&buf, "}\n");
                                reg_slots += 1;
                                aapcs_reg += 1;
                            } else {
                                break;
                            }
                        }
                    }
                    stack_args = total_slots - reg_slots;
                    if (stack_args < 0) stack_args = 0;
                    need_align = (stack_args & 1) != 0;
                    if (need_align) {
                        asmbuf_puts(&buf, "    SUB sp, sp, #4\n");
                    }
                    if (is_indirect) {
                        asmbuf_puts(&buf, "    BLX ip\n");
                    } else {
                        asmbuf_puts(&buf, "    BL ");
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                    }
                    if (need_align) {
                        asmbuf_puts(&buf, "    ADD sp, sp, #4\n");
                    }
                    if (stack_args > 0) {
                        asmbuf_puts(&buf, "    ADD sp, sp, #");
                        asmbuf_int(&buf, stack_args * 4);
                        asmbuf_putc(&buf, '\n');
                    }
                    /* #9 FIX: Check if called function returns 64-bit (long long/double).
                     * If so, push both r0 (low) and r1 (high).
                     * For indirect calls, the return type is not in prog->funcs,
                     * so irgen encodes the 64-bit return flag in bit 32 of val.
                     * ROUND4-P2 FIX: For direct calls to void functions, skip
                     * the push entirely (the caller's AST_EXPR_STMT no longer
                     * emits IR_POP, so the stack stays balanced). Indirect
                     * calls conservatively push. */
                    {
                        int ret64 = 0;
                        int is_void_ret = 0;
                        if (!is_indirect) {
                            int fi2;
                            for (fi2 = 0; fi2 < prog->func_count; fi2++) {
                                if (ark_streq(prog->funcs[fi2]->name, I->name)) {
                                    Type *rt = prog->funcs[fi2]->ret_type;
                                    if (rt && (rt->kind == TYPE_LLONG || rt->kind == TYPE_DOUBLE)) {
                                        ret64 = 1;
                                    } else if (rt && rt->kind == TYPE_VOID) {
                                        is_void_ret = 1;
                                    }
                                    break;
                                }
                            }
                        } else {
                            ret64 = (int)((I->val >> 32) & 1);
                        }
                        if (ret64) {
                            emit_push_r0(&buf);              /* push low word */
                            asmbuf_puts(&buf, "    PUSH {r1}\n");  /* push high word */
                        } else if (!is_void_ret) {
                            emit_push_r0(&buf);              /* push r0 only if non-void */
                        }
                    }
                    }
                    break;
                }
                case IR_SVC: {
                    /* __svc(n) - args are on top of stack, pop to r0 then execute SVC */
                    int n = I->val;
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, "    SVC #");
                    asmbuf_int(&buf, n);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                case IR_SMC: {
                    /* __smc(n) - secure monitor call; args are on top of stack, pop to r0 then execute SMC */
                    int n = I->val;
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, "    SMC #");
                    asmbuf_int(&buf, n);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                }
#endif
                /* === Integer divide/modulo ===
                 * Top of stack is the divisor (r1), next is the dividend (r0); result pushed back to r0.
                 * Cortex-M4 (ARMv7E-M) has hardware SDIV/UDIV; use them instead of __aeabi_idiv
                 * so single-file .bin output works without linking libgcc. */
                case IR_DIV:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, I->aux ? "    UDIV r0, r0, r1\n" : "    SDIV r0, r0, r1\n");
                    emit_push_r0(&buf);
                    break;
                case IR_MOD:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, I->aux ? "    UDIV r2, r0, r1\n" : "    SDIV r2, r0, r1\n");
                    emit_line(&buf, "MUL r2, r2, r1");
                    emit_line(&buf, "SUB r0, r0, r2");
                    emit_push_r0(&buf);
                    break;
                /* === 64-bit ops: value occupies two stack slots, low word pushed first, high word pushed last (high word on top) ===
                 * Call __aeabi_l* helpers: a in r0/r1 (low/high), b in r2/r3 (low/high),
                 * result in r0/r1 (low/high). Pop order: b first (r3 high / r2 low), then a (r1 high / r0 low). */
                case IR_64PUSH:
                    /* val=high 32 bits, aux=low 32 bits; low word pushed first */
                    emit_mov_imm(&buf, 0, I->aux);
                    emit_push_r0(&buf);
                    emit_mov_imm(&buf, 0, I->val);
                    emit_push_r0(&buf);
                    break;
                case IR_64BINARY: {
                    /* #I2 FIX: Mask off unsigned flag (0x100) from op, and use
                     * correct AEABI helper names (__aeabi_ldivmod not __aeabi_ldiv). */
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int op = I->aux & ~0x100;
                    const char *helper = 0;
                    int is_div = 0, is_mod = 0;
                    switch (op) {
                        case (int)TOK_PLUS:  helper = "__aeabi_ladd"; break;
                        case (int)TOK_MINUS: helper = "__aeabi_lsub"; break;
                        case (int)TOK_MUL:   helper = "__aeabi_lmul"; break;
                        case (int)TOK_DIV:   helper = is_unsigned ? "__aeabi_uldivmod" : "__aeabi_ldivmod"; is_div = 1; break;
                        case (int)TOK_MOD:   helper = is_unsigned ? "__aeabi_uldivmod" : "__aeabi_ldivmod"; is_mod = 1; break;
                        default: break;
                    }
                    emit_line(&buf, "POP {r3}");
                    emit_line(&buf, "POP {r2}");
                    emit_line(&buf, "POP {r1}");
                    emit_line(&buf, "POP {r0}");
                    if (helper) {
                        asmbuf_puts(&buf, "    BL ");
                        asmbuf_puts(&buf, helper);
                        asmbuf_putc(&buf, '\n');
                    }
                    if (is_mod) {
                        /* __aeabi_ldivmod/uldivmod: quotient in r0/r1, remainder in r2/r3;
                         * take remainder to r0/r1 for MOD result. */
                        emit_line(&buf, "MOV r0, r2");
                        emit_line(&buf, "MOV r1, r3");
                    }
                    /* For DIV, quotient is already in r0/r1. */
                    emit_push_r0(&buf);                 /* result low 32 bits */
                    asmbuf_puts(&buf, "    PUSH {r1}\n"); /* result high 32 bits */
                    break;
                }
                case IR_64CMP: {
                    /* #I3 FIX: Check unsigned flag (0x100) to select __aeabi_ulcmp
                     * for unsigned comparison instead of always using __aeabi_lcmp. */
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int op = I->aux & ~0x100;
                    const char *cmp_helper = is_unsigned ? "__aeabi_ulcmp" : "__aeabi_lcmp";
                    /* __aeabi_lcmp/ulcmp returns -1/0/1 (a<b / a==b / a>b) in r0;
                     * use conditional MOV to produce a 0/1 comparison result. */
                    emit_line(&buf, "POP {r3}");
                    emit_line(&buf, "POP {r2}");
                    emit_line(&buf, "POP {r1}");
                    emit_line(&buf, "POP {r0}");
                    asmbuf_puts(&buf, "    BL ");
                    asmbuf_puts(&buf, cmp_helper);
                    asmbuf_putc(&buf, '\n');
                    emit_line(&buf, "CMP r0, #0");
                    switch (op) {
                        case (int)TOK_EQ: emit_line(&buf, "ITE EQ"); emit_line(&buf, "MOVEQ r0, #1"); emit_line(&buf, "MOVNE r0, #0"); break;
                        case (int)TOK_NE: emit_line(&buf, "ITE NE"); emit_line(&buf, "MOVNE r0, #1"); emit_line(&buf, "MOVEQ r0, #0"); break;
                        case (int)TOK_LT: emit_line(&buf, "ITE LT"); emit_line(&buf, "MOVLT r0, #1"); emit_line(&buf, "MOVGE r0, #0"); break;
                        case (int)TOK_GT: emit_line(&buf, "ITE GT"); emit_line(&buf, "MOVGT r0, #1"); emit_line(&buf, "MOVLE r0, #0"); break;
                        case (int)TOK_LE: emit_line(&buf, "ITE LE"); emit_line(&buf, "MOVLE r0, #1"); emit_line(&buf, "MOVGT r0, #0"); break;
                        case (int)TOK_GE: emit_line(&buf, "ITE GE"); emit_line(&buf, "MOVGE r0, #1"); emit_line(&buf, "MOVLT r0, #0"); break;
                        default: break;
                    }
                    emit_push_r0(&buf);
                    break;
                }
                /* === Memory copy: top of stack is destination address, next is source; BL memcpy === */
                case IR_MEMCPY:
                    emit_pop_r0(&buf);                 /* dst */
                    emit_pop_r1(&buf);                 /* src */
                    emit_mov_imm(&buf, 2, I->val);     /* n bytes */
                    emit_line(&buf, "BL memcpy");
                    break;
                /* === Bitfield store/load ===
                 * val=bit offset, aux=bit width. mask = (1<<bw)-1. */
                case IR_STORE_FLD: {
                    int bit_off = I->val;
                    int bw = I->aux;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    emit_pop_r0(&buf);                 /* value */
                    emit_pop_r1(&buf);                 /* addr */
                    emit_mov_imm(&buf, 2, (int)mask);  /* r2 = mask */
                    emit_line(&buf, "AND r0, r2");      /* truncate value to bw bits */
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, "    LSL r0, r0, #");
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, "    LSL r2, r2, #");
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                    }
                    emit_line(&buf, "LDR r3, [r1]");
                    emit_line(&buf, "BIC r3, r2");      /* clear old bits */
                    emit_line(&buf, "ORR r3, r0");      /* set new bits */
                    emit_line(&buf, "STR r3, [r1]");
                    break;
                }
                case IR_LOAD_FLD: {
                    int bit_off = I->val;
                    int bw = I->aux & 0xFF;         /* #I6 FIX: mask off 0x100 signed flag */
                    int is_signed = (I->aux & 0x100) != 0;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    emit_pop_r0(&buf);                 /* addr */
                    emit_line(&buf, "LDR r0, [r0]");
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, "    LSR r0, r0, #");
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                    }
                    if (is_signed && bw < 32) {
                        /* #I6 FIX: Sign-extend signed bitfield using LSL+ASR.
                         * A 3-bit signed field with value 0x7 should become
                         * 0xFFFFFFFF (-1), not 0x00000007 (7). */
                        int shift = 32 - bw;
                        asmbuf_puts(&buf, "    LSL r0, r0, #");
                        asmbuf_int(&buf, shift);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, "    ASR r0, r0, #");
                        asmbuf_int(&buf, shift);
                        asmbuf_putc(&buf, '\n');
                    } else {
                        emit_mov_imm(&buf, 2, (int)mask);
                        emit_line(&buf, "AND r0, r2");      /* truncate to bw bits */
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_RETURN:
                    if (is_sret) {
                        /* Struct return > 8 bytes: pop source address (r1),
                         * load sret pointer from frame, copy struct word-by-word,
                         * then return r0 = sret pointer per AAPCS. */
                        int rsz = type_size(f->ret_type);
                        int words = (rsz + 3) / 4;
                        int w;
                        emit_pop_r1(&buf);  /* r1 = source struct address */
                        /* Load sret pointer into r0 */
                        asmbuf_puts(&buf, "    LDR r0, [r6, #");
                        asmbuf_int(&buf, sret_off);
                        asmbuf_puts(&buf, "]\n");
                        /* Copy struct word by word */
                        for (w = 0; w < words; w++) {
                            asmbuf_puts(&buf, "    LDR r2, [r1, #");
                            asmbuf_int(&buf, w * 4);
                            asmbuf_puts(&buf, "]\n");
                            asmbuf_puts(&buf, "    STR r2, [r0, #");
                            asmbuf_int(&buf, w * 4);
                            asmbuf_puts(&buf, "]\n");
                        }
                        /* Manual epilogue: skip POP {r0} (r0 already holds sret ptr) */
                        emit_line(&buf, "MOV sp, r6");
                        if (local_size > 0) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #");
                            asmbuf_int(&buf, local_size);
                            asmbuf_putc(&buf, '\n');
                        }
                        if (f->is_irq) {
                            emit_line(&buf, "POP {r5, r6, r7, pc}");
                        } else {
                            emit_line(&buf, "POP {r5, r6, r7, pc}");
                        }
                    } else {
                        emit_epilogue_return(&buf, local_size, f->is_irq, f->is_naked);
                    }
                    break;
                case IR_ALLOCA: {
                    /* Bug S6 FIX (verified): VLA dynamic stack allocation.
                     * Pop byte count, align UP to 8 bytes (ADD+BIC rounds up,
                     * not down), subtract from sp, push the start address.
                     * irgen emits exactly: PUSH(size_expr); PUSH_INT(elem_sz);
                     * BINARY(MUL); IR_ALLOCA; IR_STORE. The ALLOCA pops 1 value
                     * (byte count) and pushes 1 value (start address) - balanced.
                     * The previously-reported extra PUSH {r0} + ADD sp,#4 does
                     * not exist in the current code; the bug was already fixed. */
                    emit_pop_r0(&buf);                  /* r0 = byte count */
                    emit_line(&buf, "ADD r0, r0, #7");
                    emit_line(&buf, "BIC r0, r0, #7");  /* align up to 8 (clear low 3 bits) */
                    emit_line(&buf, "SUB sp, sp, r0");  /* allocate on stack */
                    emit_line(&buf, "MOV r0, sp");      /* r0 = start address */
                    emit_push_r0(&buf);                 /* push start address */
                    break;
                }
                /* === 0.7.0: SIMD parallel ops (Cortex-M4/M7 DSP instructions) ===
                 * Stack semantics: pop r1 (right operand), pop r0 (left operand),
                 * perform the parallel op and push r0 back. SADD16/SSUB16/SADD8/SSUB8
                 * are ARMv6TE+ DSP extension instructions that add/subtract multiple
                 * signed sub-words within a 32-bit register in parallel. */
                case IR_SIMD_ADD16:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SADD16 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                case IR_SIMD_SUB16:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SSUB16 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                case IR_SIMD_ADD8:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SADD8 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                case IR_SIMD_SUB8:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SSUB8 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                default:
                    asmbuf_puts(&buf, "    ; unknown ir op ");
                    asmbuf_int(&buf, (int)I->op);
                    asmbuf_putc(&buf, '\n');
                    break;
            }
        }
#if ARKCC_FEATURE_DWARF
        if (dbg) {
            emit_line(&buf, ".cfi_endproc");
            asmbuf_puts(&buf, ".size ");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", .-");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
        }
#endif
        }
        asmbuf_putc(&buf, '\n');

        /* __attribute__((constructor/destructor)): register the function address in .init_array/.fini_array;
         * the C runtime startup code iterates and calls them before/after main.
         * armcc/MDK uses .init_array (not .ctors), consistent with ARMCC5. */
        if (f->is_constructor && !f->alias_target) {
            asmbuf_puts(&buf, ".section .init_array, \"aw\"\n");
            asmbuf_puts(&buf, ".align 2\n");
            asmbuf_puts(&buf, ".word ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
            asmbuf_puts(&buf, ".text\n");
        }
        if (f->is_destructor && !f->alias_target) {
            asmbuf_puts(&buf, ".section .fini_array, \"aw\"\n");
            asmbuf_puts(&buf, ".align 2\n");
            asmbuf_puts(&buf, ".word ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
            asmbuf_puts(&buf, ".text\n");
        }
    }

    /* F3/S5: Generate __arkcc_init_globals function that initializes .data globals
     * and zeros .bss at startup. The Reset_Handler should call this before main().
     * This replaces the need for a separate .data section in Flash and a linker. */
    if (g_data_sram_start != g_data_sram_end || g_bss_sram_start != g_bss_sram_end) {
        int gi;
        asmbuf_puts(&buf, ".global __arkcc_init_globals\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__arkcc_init_globals:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");

        /* Initialize .data globals with their initial values */
        for (gi = 0; gi < prog->global_count; gi++) {
            IrGlobal *g = prog->globals[gi];
            unsigned addr;
            int sz, elem_sz, elem_count, ei;
            const char *agg;
            if (g->is_extern || g->has_at) continue;
            if (!g->has_init) continue;
            /* Skip zero-initialized scalars (they're in .bss, already zeroed) */
            if (!g->init_aggregate && !g->init_str && !g->is_float && g->init_int == 0) continue;
            addr = gvar_find(g->name);
            if (addr == 0) continue;
            sz = g->type ? type_size(g->type) : 4;
            if (sz <= 0) sz = 4;

            if (g->is_float) {
                /* Float/double initialization: convert to bit pattern */
                union { float f; unsigned int u; } fu;
                union { double d; unsigned int u[2]; } du;
                int is_double = (g->type && g->type->kind == TYPE_DOUBLE);
                if (is_double) {
                    du.d = g->init_float;
                    emit_mov_imm(&buf, 0, (int)du.u[0]);
                    emit_mov_imm(&buf, 1, (int)addr);
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                    emit_mov_imm(&buf, 0, (int)du.u[1]);
                    asmbuf_puts(&buf, "    STR r0, [r1, #4]\n");
                } else {
                    fu.f = (float)g->init_float;
                    emit_mov_imm(&buf, 0, (int)fu.u);
                    emit_mov_imm(&buf, 1, (int)addr);
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                }
            } else if (g->init_aggregate) {
                /* Aggregate initializer: parse "{v1, v2, v3, ...}" and emit element-by-element.
                 * #4 FIX: Improved parser now handles:
                 *   - Nested {} (flattened for multi-dim arrays / struct fields)
                 *   - Negative numbers
                 *   - Designated initializers (.field=value — field name skipped)
                 *   - String elements (skipped, emit 0)
                 *   - Float elements (skipped, emit 0)
                 *   - Hex and decimal integers */
                agg = g->init_aggregate;
                /* For arrays, element size = size of base type; for structs, use 4 (word) */
                if (g->type && g->type->kind == TYPE_ARRAY && g->type->base) {
                    elem_sz = type_size(g->type->base);
                    if (elem_sz <= 0) elem_sz = 4;
                    elem_count = g->type->size;
                } else {
                    elem_sz = 4;
                    elem_count = sz / 4;
                }
                if (elem_count <= 0) elem_count = sz;  /* fallback: byte-by-byte */
                /* Skip opening '{' */
                if (*agg == '{') agg++;
                for (ei = 0; ei < elem_count; ei++) {
                    int val = 0;
                    int neg = 0;
                    int got_val = 0;
                    /* Skip whitespace and nested opening braces */
                    while (*agg == ' ' || *agg == '\t' || *agg == '\n' || *agg == '{') agg++;
                    /* Handle designated initializer: .field=value or [idx]=value */
                    if (*agg == '.') {
                        /* Skip field name until '=' */
                        while (*agg && *agg != '=') agg++;
                        if (*agg == '=') agg++;
                        while (*agg == ' ' || *agg == '\t') agg++;
                    }
                    if (*agg == '[') {
                        /* Skip array index designation [idx]= */
                        while (*agg && *agg != ']') agg++;
                        if (*agg == ']') agg++;
                        if (*agg == '=') agg++;
                        while (*agg == ' ' || *agg == '\t') agg++;
                    }
                    /* Handle string elements (skip) */
                    if (*agg == '"') {
                        agg++;
                        while (*agg && *agg != '"') agg++;
                        if (*agg == '"') agg++;
                        val = 0; got_val = 1;
                    }
                    /* Handle negative sign */
                    else if (*agg == '-') { neg = 1; agg++; }
                    /* Parse hex or decimal */
                    if (!got_val) {
                        if (*agg == '0' && (agg[1] == 'x' || agg[1] == 'X')) {
                            agg += 2;
                            while ((*agg >= '0' && *agg <= '9') || (*agg >= 'a' && *agg <= 'f') || (*agg >= 'A' && *agg <= 'F')) {
                                char c = *agg;
                                int v = (c >= '0' && c <= '9') ? c - '0' :
                                        (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                                val = val * 16 + v;
                                agg++;
                            }
                            got_val = 1;
                        } else if (*agg >= '0' && *agg <= '9') {
                            while (*agg >= '0' && *agg <= '9') { val = val * 10 + (*agg - '0'); agg++; }
                            got_val = 1;
                        } else if (*agg == '\'') {
                            /* Character literal */
                            agg++;
                            if (*agg == '\\') {
                                agg++;
                                switch (*agg) {
                                    case 'n': val = '\n'; break;
                                    case 't': val = '\t'; break;
                                    case 'r': val = '\r'; break;
                                    case '0': val = '\0'; break;
                                    case '\\': val = '\\'; break;
                                    case '\'': val = '\''; break;
                                    case '"': val = '"'; break;
                                    default: val = *agg; break;
                                }
                                agg++;
                            } else {
                                val = (unsigned char)*agg;
                                agg++;
                            }
                            if (*agg == '\'') agg++;
                            got_val = 1;
                        }
                        /* Skip float values (contains '.' or 'e') */
                        if (!got_val && ((*agg >= 'a' && *agg <= 'z') || (*agg >= 'A' && *agg <= 'Z'))) {
                            /* Skip identifier/float */
                            while ((*agg >= '0' && *agg <= '9') || (*agg >= 'a' && *agg <= 'z') || (*agg >= 'A' && *agg <= 'Z') || *agg == '.' || *agg == '+' || *agg == '-') agg++;
                            val = 0; got_val = 1;
                        }
                    }
                    if (neg) val = -val;
                    /* Skip to next comma or closing brace, handling nested braces */
                    {
                        int depth = 0;
                        while (*agg && !(*agg == ',' && depth == 0) && !(*agg == '}' && depth == 0)) {
                            if (*agg == '{') depth++;
                            else if (*agg == '}') depth--;
                            agg++;
                        }
                    }
                    if (*agg == ',') agg++;
                    /* Skip nested closing braces */
                    while (*agg == '}') agg++;

                    /* Emit store for this element */
                    emit_mov_imm(&buf, 0, val);
                    emit_mov_imm(&buf, 1, (int)(addr + ei * elem_sz));
                    if (elem_sz == 1) {
                        asmbuf_puts(&buf, "    STRB r0, [r1]\n");
                    } else if (elem_sz == 2) {
                        asmbuf_puts(&buf, "    STRH r0, [r1]\n");
                    } else {
                        asmbuf_puts(&buf, "    STR r0, [r1]\n");
                    }
                }
            } else if (g->init_str) {
                /* String initializer for char array: emit byte-by-byte */
                int si;
                for (si = 0; si < sz && g->init_str[si]; si++) {
                    emit_mov_imm(&buf, 0, (int)(unsigned char)g->init_str[si]);
                    emit_mov_imm(&buf, 1, (int)(addr + si));
                    asmbuf_puts(&buf, "    STRB r0, [r1]\n");
                }
            } else {
                /* Integer scalar initialization */
                emit_mov_imm(&buf, 0, (int)g->init_int);
                emit_mov_imm(&buf, 1, (int)addr);
                if (sz == 1) {
                    asmbuf_puts(&buf, "    STRB r0, [r1]\n");
                } else if (sz == 2) {
                    asmbuf_puts(&buf, "    STRH r0, [r1]\n");
                } else if (sz >= 8) {
                    /* 64-bit: store low and high words (high=0 for now) */
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                    emit_mov_imm(&buf, 0, (int)(g->init_int >> 32));
                    asmbuf_puts(&buf, "    STR r0, [r1, #4]\n");
                } else {
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                }
            }
        }

        /* Zero .bss region */
        if (g_bss_sram_start != g_bss_sram_end) {
            emit_mov_imm(&buf, 0, (int)g_bss_sram_start);
            emit_mov_imm(&buf, 1, (int)g_bss_sram_end);
            asmbuf_puts(&buf, "    CMP r0, r1\n");
            asmbuf_puts(&buf, "    BEQ .L_bss_done\n");
            asmbuf_puts(&buf, "    MOV r2, #0\n");
            asmbuf_puts(&buf, ".L_bss_loop:\n");
            asmbuf_puts(&buf, "    STR r2, [r0]\n");
            asmbuf_puts(&buf, "    ADD r0, r0, #4\n");
            asmbuf_puts(&buf, "    CMP r0, r1\n");
            asmbuf_puts(&buf, "    BNE .L_bss_loop\n");
            asmbuf_puts(&buf, ".L_bss_done:\n");
        }

        /* Bug ROUND4-P3 FIX: Use POP {pc} for return instead of POP {r0}; BX r0.
         * In ARM Thumb mode, POP {pc} automatically sets the Thumb bit from
         * bit 0 of the stored LR (which is always 1 for Thumb code), so this
         * is the standard, more efficient epilogue pattern. */
        asmbuf_puts(&buf, "    POP {pc}\n\n");
    }

    /* #A1 FIX: Emit __aeabi_* runtime helper functions for 64-bit arithmetic.
     * The ARM backend emits BL __aeabi_ladd/__aeabi_lmul/__aeabi_ldivmod etc.
     * for long long operations. These follow the ARM EABI calling convention:
     *   - 64-bit values passed in r0/r1 (low/high) and r2/r3 (low/high)
     *   - 64-bit return value in r0/r1 (low/high)
     *   - ldivmod/uldivmod: quotient in r0/r1, remainder in r2/r3
     * Note: 16-bit Thumb ADD/SUB/LSR/ASR always set flags (S suffix implicit).
     * The assembler matches base mnemonics (ADD not ADDS). RSC is unavailable
     * in Thumb-2; 64-bit negate uses CMP/BEQ/RSB/MVN. ADC/SBC/ORR/EOR only
     * support 2-operand form (Rd = Rd op Rm). Numeric labels (1f/1b) are not
     * supported; use named labels. Only r0-r7 used in uldivmod (high registers
     * not supported by 16-bit immediate/data-processing instructions). */
    {
        /* __aeabi_ladd: 64-bit addition (a = r0:r1, b = r2:r3 -> r0:r1) */
        asmbuf_puts(&buf, ".global __aeabi_ladd\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_ladd:\n");
        asmbuf_puts(&buf, "    ADD r0, r0, r2\n");
        asmbuf_puts(&buf, "    ADC r1, r3\n");          /* r1 = r1 + r3 + C */
        asmbuf_puts(&buf, "    BX lr\n\n");

        /* __aeabi_lsub: 64-bit subtraction (a = r0:r1, b = r2:r3 -> r0:r1) */
        asmbuf_puts(&buf, ".global __aeabi_lsub\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_lsub:\n");
        asmbuf_puts(&buf, "    SUB r0, r0, r2\n");
        asmbuf_puts(&buf, "    SBC r1, r3\n");          /* r1 = r1 - r3 - NOT(C) */
        asmbuf_puts(&buf, "    BX lr\n\n");

        /* __aeabi_lmul: 64-bit multiplication (a = r0:r1, b = r2:r3 -> r0:r1)
         * result = (a_lo * b_lo) + ((a_lo * b_hi + a_hi * b_lo) << 32)
         * MUL T1 only supports Rd = Rd * Rm (2-operand); use MOV to copy first. */
        asmbuf_puts(&buf, ".global __aeabi_lmul\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_lmul:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, lr}\n");
        asmbuf_puts(&buf, "    MOV r4, r3\n");          /* r4 = b_hi */
        asmbuf_puts(&buf, "    MUL r4, r0\n");          /* r4 = r0 * b_hi */
        asmbuf_puts(&buf, "    MOV r5, r1\n");          /* r5 = a_hi */
        asmbuf_puts(&buf, "    MUL r5, r2\n");          /* r5 = a_hi * b_lo */
        asmbuf_puts(&buf, "    UMULL r0, r1, r0, r2\n"); /* r0:r1 = a_lo * b_lo */
        asmbuf_puts(&buf, "    ADD r1, r4\n");          /* r1 += a_lo*b_hi */
        asmbuf_puts(&buf, "    ADD r1, r5\n");          /* r1 += a_hi*b_lo */
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n\n");

        /* __aeabi_lcmp: 64-bit signed compare (a = r0:r1, b = r2:r3 -> r0 = -1/0/1)
         * MOV r0, #-1 is buggy (encodes as #255), use MOV+MVN for -1. */
        asmbuf_puts(&buf, ".global __aeabi_lcmp\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_lcmp:\n");
        asmbuf_puts(&buf, "    CMP r1, r3\n");
        asmbuf_puts(&buf, "    BGT .L_lcmp_gt\n");
        asmbuf_puts(&buf, "    BLT .L_lcmp_lt\n");
        asmbuf_puts(&buf, "    CMP r0, r2\n");
        asmbuf_puts(&buf, "    BHI .L_lcmp_gt\n");
        asmbuf_puts(&buf, "    BCC .L_lcmp_lt\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    BX lr\n");
        asmbuf_puts(&buf, ".L_lcmp_lt:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    MVN r0, r0\n");          /* r0 = -1 */
        asmbuf_puts(&buf, "    BX lr\n");
        asmbuf_puts(&buf, ".L_lcmp_gt:\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    BX lr\n\n");

        /* __aeabi_ulcmp: 64-bit unsigned compare (a = r0:r1, b = r2:r3 -> r0 = -1/0/1) */
        asmbuf_puts(&buf, ".global __aeabi_ulcmp\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_ulcmp:\n");
        asmbuf_puts(&buf, "    CMP r1, r3\n");
        asmbuf_puts(&buf, "    BHI .L_ulcmp_gt\n");
        asmbuf_puts(&buf, "    BCC .L_ulcmp_lt\n");
        asmbuf_puts(&buf, "    CMP r0, r2\n");
        asmbuf_puts(&buf, "    BHI .L_ulcmp_gt\n");
        asmbuf_puts(&buf, "    BCC .L_ulcmp_lt\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    BX lr\n");
        asmbuf_puts(&buf, ".L_ulcmp_lt:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    MVN r0, r0\n");          /* r0 = -1 */
        asmbuf_puts(&buf, "    BX lr\n");
        asmbuf_puts(&buf, ".L_ulcmp_gt:\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    BX lr\n\n");

        /* __aeabi_uldivmod: 64-bit unsigned division/modulo
         * Input: r0:r1 = dividend, r2:r3 = divisor
         * Output: r0:r1 = quotient, r2:r3 = remainder
         * Algorithm: shift-and-subtract (64 iterations)
         * Register allocation (r0-r7 only, use stack for temps):
         *   r0:r1 = dividend -> quotient (shifted left, quotient bits set at LSB)
         *   r2:r3 = remainder (init 0, shifted left, dividend MSB brought in)
         *   r4 = counter (64 iterations)
         *   r5 = scratch (dividend MSB, carry)
         *   r6:r7 = saved divisor (for comparison and subtraction) */
        asmbuf_puts(&buf, ".global __aeabi_uldivmod\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_uldivmod:\n");
        asmbuf_puts(&buf, "    Push {r4, r5, r6, r7, lr}\n");
        asmbuf_puts(&buf, "    MOV r6, r2\n");          /* r6 = divisor_lo */
        asmbuf_puts(&buf, "    MOV r7, r3\n");          /* r7 = divisor_hi */
        asmbuf_puts(&buf, "    MOV r2, #0\n");          /* r2 = remainder_lo */
        asmbuf_puts(&buf, "    MOV r3, #0\n");          /* r3 = remainder_hi */
        asmbuf_puts(&buf, "    MOV r4, #64\n");         /* r4 = counter */
        /* Check divide by zero: ORR r5, r6, r7 -> r5 = r5 | r6 | r7 */
        asmbuf_puts(&buf, "    MOV r5, r6\n");
        asmbuf_puts(&buf, "    ORR r5, r7\n");
        asmbuf_puts(&buf, "    CMP r5, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_uldiv_zero\n");
        asmbuf_puts(&buf, ".L_uldiv_loop:\n");
        /* r5 = MSB of dividend (bit 63 = MSB of r1) */
        asmbuf_puts(&buf, "    LSR r5, r1, #31\n");
        /* Shift dividend left by 1 (64-bit): need MSB of r0 -> r1 bit 0.
         * Save dividend MSB on stack, use r5 for carry. */
        asmbuf_puts(&buf, "    Push {r5}\n");
        asmbuf_puts(&buf, "    LSR r5, r0, #31\n");     /* r5 = MSB of r0 */
        asmbuf_puts(&buf, "    LSL r0, r0, #1\n");
        asmbuf_puts(&buf, "    LSL r1, r1, #1\n");
        asmbuf_puts(&buf, "    ORR r1, r5\n");          /* r1 |= carry from r0 */
        asmbuf_puts(&buf, "    Pop {r5}\n");             /* r5 = dividend MSB */
        /* Shift remainder left by 1, bring in dividend MSB (r5).
         * Need MSB of r2 -> r3 bit 0. Save counter, use r4 as temp. */
        asmbuf_puts(&buf, "    Push {r4}\n");
        asmbuf_puts(&buf, "    LSR r4, r2, #31\n");     /* r4 = MSB of r2 */
        asmbuf_puts(&buf, "    LSL r2, r2, #1\n");
        asmbuf_puts(&buf, "    ORR r2, r5\n");          /* r2 |= dividend MSB */
        asmbuf_puts(&buf, "    LSL r3, r3, #1\n");
        asmbuf_puts(&buf, "    ORR r3, r4\n");          /* r3 |= carry from r2 */
        asmbuf_puts(&buf, "    Pop {r4}\n");            /* restore counter */
        /* Compare remainder (r2:r3) with divisor (r6:r7) unsigned */
        asmbuf_puts(&buf, "    CMP r3, r7\n");
        asmbuf_puts(&buf, "    BHI .L_uldiv_sub\n");
        asmbuf_puts(&buf, "    BCC .L_uldiv_skip\n");
        asmbuf_puts(&buf, "    CMP r2, r6\n");
        asmbuf_puts(&buf, "    BCC .L_uldiv_skip\n");
        /* Subtract divisor from remainder */
        asmbuf_puts(&buf, ".L_uldiv_sub:\n");
        asmbuf_puts(&buf, "    SUB r2, r2, r6\n");
        asmbuf_puts(&buf, "    SBC r3, r7\n");          /* r3 = r3 - r7 - NOT(C) */
        /* Set quotient bit 0 (in dividend/quotient r0) */
        asmbuf_puts(&buf, "    MOV r5, #1\n");
        asmbuf_puts(&buf, "    ORR r0, r5\n");
        asmbuf_puts(&buf, ".L_uldiv_skip:\n");
        asmbuf_puts(&buf, "    SUB r4, r4, #1\n");
        asmbuf_puts(&buf, "    BNE .L_uldiv_loop\n");
        /* Result: quotient in r0:r1, remainder in r2:r3 (already in place) */
        asmbuf_puts(&buf, "    Pop {r4, r5, r6, r7, pc}\n");
        /* Division by zero: return 0 */
        asmbuf_puts(&buf, ".L_uldiv_zero:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    MOV r1, #0\n");
        asmbuf_puts(&buf, "    MOV r2, #0\n");
        asmbuf_puts(&buf, "    MOV r3, #0\n");
        asmbuf_puts(&buf, "    Pop {r4, r5, r6, r7, pc}\n\n");

        /* __aeabi_ldivmod: 64-bit signed division/modulo
         * Input: r0:r1 = dividend, r2:r3 = divisor
         * Output: r0:r1 = quotient, r2:r3 = remainder
         * Handles signs: negate inputs as needed, call uldivmod, fixup signs.
         * Remainder has same sign as dividend.
         * 64-bit negate (RSC unavailable): if low != 0 { low=-low; high=~high }
         * else { high=-high; }. Uses ASR r5, r1, #31 for sign extraction. */
        asmbuf_puts(&buf, ".global __aeabi_ldivmod\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__aeabi_ldivmod:\n");
        asmbuf_puts(&buf, "    Push {r4, r5, r6, r7, lr}\n");
        /* r5 = dividend sign (0xFFFFFFFF if negative, 0 if positive) */
        asmbuf_puts(&buf, "    ASR r5, r1, #31\n");
        asmbuf_puts(&buf, "    MOV r6, r5\n");          /* r6 = saved dividend sign */
        asmbuf_puts(&buf, "    CMP r5, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_posd\n");
        /* Negate dividend: if low != 0 { low=-low; high=~high } else { high=-high } */
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_neg_d_hi\n");
        asmbuf_puts(&buf, "    RSB r0, r0\n");          /* r0 = -r0 */
        asmbuf_puts(&buf, "    MVN r1, r1\n");          /* r1 = ~r1 */
        asmbuf_puts(&buf, "    B .L_ldiv_posd\n");
        asmbuf_puts(&buf, ".L_ldiv_neg_d_hi:\n");
        asmbuf_puts(&buf, "    RSB r1, r1\n");          /* r1 = -r1, r0 stays 0 */
        asmbuf_puts(&buf, ".L_ldiv_posd:\n");
        /* Negate divisor if negative */
        asmbuf_puts(&buf, "    ASR r5, r3, #31\n");
        asmbuf_puts(&buf, "    CMP r5, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_posb\n");
        asmbuf_puts(&buf, "    CMP r2, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_neg_b_hi\n");
        asmbuf_puts(&buf, "    RSB r2, r2\n");
        asmbuf_puts(&buf, "    MVN r3, r3\n");
        asmbuf_puts(&buf, "    B .L_ldiv_posb\n");
        asmbuf_puts(&buf, ".L_ldiv_neg_b_hi:\n");
        asmbuf_puts(&buf, "    RSB r3, r3\n");
        asmbuf_puts(&buf, ".L_ldiv_posb:\n");
        /* Call uldivmod (r0:r1 / r2:r3 -> r0:r1 = quot, r2:r3 = rem) */
        asmbuf_puts(&buf, "    BL __aeabi_uldivmod\n");
        /* Quotient sign: negative if dividend and divisor signs differ.
         * EOR r4, r6, r5 -> r4 = r4 ^ r5 ^ r6. Use MOV first for 2-operand EOR. */
        asmbuf_puts(&buf, "    MOV r4, r6\n");
        asmbuf_puts(&buf, "    EOR r4, r5\n");
        asmbuf_puts(&buf, "    CMP r4, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_qpos\n");
        /* Negate quotient */
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_neg_q_hi\n");
        asmbuf_puts(&buf, "    RSB r0, r0\n");
        asmbuf_puts(&buf, "    MVN r1, r1\n");
        asmbuf_puts(&buf, "    B .L_ldiv_qpos\n");
        asmbuf_puts(&buf, ".L_ldiv_neg_q_hi:\n");
        asmbuf_puts(&buf, "    RSB r1, r1\n");
        asmbuf_puts(&buf, ".L_ldiv_qpos:\n");
        /* Remainder sign: same as dividend (r6) */
        asmbuf_puts(&buf, "    CMP r6, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_rpos\n");
        asmbuf_puts(&buf, "    CMP r2, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_ldiv_neg_r_hi\n");
        asmbuf_puts(&buf, "    RSB r2, r2\n");
        asmbuf_puts(&buf, "    MVN r3, r3\n");
        asmbuf_puts(&buf, "    B .L_ldiv_rpos\n");
        asmbuf_puts(&buf, ".L_ldiv_neg_r_hi:\n");
        asmbuf_puts(&buf, "    RSB r3, r3\n");
        asmbuf_puts(&buf, ".L_ldiv_rpos:\n");
        asmbuf_puts(&buf, "    Pop {r4, r5, r6, r7, pc}\n\n");
    }

    /* Bug-A1: Soft-float __aeabi helpers for environments without libgcc/
     * compiler-rt (bare-metal STM32). These are minimal but correct
     * implementations of the ARM EABI floating-point helper functions.
     * They use integer bit manipulation on the IEEE 754 representation.
     * For performance, enable hardware FPU with -f fpu (ARKCC_FEATURE_FPU_HW). */
    {
        /* __aeabi_f2iz: float (r0) -> signed int (r0)
         * Truncate toward zero. IEEE 754 single in r0, extract exponent,
         * shift mantissa, apply sign. */
        asmbuf_puts(&buf, ".global __aeabi_f2iz\n.thumb_func\n__aeabi_f2iz:\n");
        asmbuf_puts(&buf, "    LSR r1, r0, #23\n");      /* r1 = exponent (8 bits) */
        asmbuf_puts(&buf, "    LSL r1, r1, #1\n");       /* align */
        asmbuf_puts(&buf, "    LSR r1, r1, #1\n");       /* r1 = exp */
        asmbuf_puts(&buf, "    CMP r1, #150\n");         /* exp >= 150? value >= 2^23, need shift left */
        asmbuf_puts(&buf, "    BGE .L_f2iz_shl\n");
        asmbuf_puts(&buf, "    CMP r1, #126\n");         /* exp < 126? |val| < 0.5, result 0 */
        asmbuf_puts(&buf, "    BLT .L_f2iz_zero\n");
        asmbuf_puts(&buf, "    SUB r2, r1, #127\n");     /* shift = exp - 127 (0..22) */
        asmbuf_puts(&buf, "    LSL r0, r0, #9\n");       /* mantissa << 9, drop sign+exp */
        asmbuf_puts(&buf, "    LSR r0, r0, r2\n");       /* shift right by (23-shift) ... approximate */
        asmbuf_puts(&buf, "    B .L_f2iz_sign\n");
        asmbuf_puts(&buf, ".L_f2iz_shl:\n");
        asmbuf_puts(&buf, "    SUB r2, r1, #150\n");     /* shift left amount */
        asmbuf_puts(&buf, "    MOV r0, #0x800000\n");    /* implied 1 bit */
        asmbuf_puts(&buf, "    LSL r0, r0, r2\n");       /* shift left */
        asmbuf_puts(&buf, ".L_f2iz_sign:\n");
        asmbuf_puts(&buf, "    LSR r1, r0, #31\n");      /* r1 = sign bit */
        asmbuf_puts(&buf, "    CMP r1, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_f2iz_done\n");
        asmbuf_puts(&buf, "    RSB r0, r0, #0\n");       /* negate */
        asmbuf_puts(&buf, ".L_f2iz_done:\n");
        asmbuf_puts(&buf, "    BX lr\n");
        asmbuf_puts(&buf, ".L_f2iz_zero:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    BX lr\n\n");

        /* __aeabi_f2uiz: float (r0) -> unsigned int (r0)
         * Same as f2iz but unsigned result. */
        asmbuf_puts(&buf, ".global __aeabi_f2uiz\n.thumb_func\n__aeabi_f2uiz:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __aeabi_f2iz\n");
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n\n");

        /* __aeabi_i2f: signed int (r0) -> float (r0)
         * Convert signed integer to IEEE 754 single. */
        asmbuf_puts(&buf, ".global __aeabi_i2f\n.thumb_func\n__aeabi_i2f:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    BGE .L_i2f_pos\n");
        asmbuf_puts(&buf, "    RSB r0, r0, #0\n");       /* negate */
        asmbuf_puts(&buf, "    MOV r1, #1\n");           /* sign flag */
        asmbuf_puts(&buf, "    B .L_i2f_conv\n");
        asmbuf_puts(&buf, ".L_i2f_pos:\n");
        asmbuf_puts(&buf, "    MOV r1, #0\n");
        asmbuf_puts(&buf, ".L_i2f_conv:\n");
        asmbuf_puts(&buf, "    CLZ r2, r0\n");           /* count leading zeros */
        asmbuf_puts(&buf, "    RSB r2, r2, #31\n");      /* r2 = bit position of MSB */
        asmbuf_puts(&buf, "    MOV r3, #127\n");
        asmbuf_puts(&buf, "    ADD r3, r3, r2\n");       /* exponent = 127 + msb_pos */
        asmbuf_puts(&buf, "    LSL r3, r3, #23\n");      /* shift exponent into position */
        asmbuf_puts(&buf, "    LSL r0, r0, r2\n");       /* shift mantissa so MSB at bit 31 */
        asmbuf_puts(&buf, "    LSL r0, r0, #1\n");       /* drop implied 1 bit */
        asmbuf_puts(&buf, "    LSR r0, r0, #9\n");       /* align to mantissa field */
        asmbuf_puts(&buf, "    ORR r0, r0, r3\n");       /* combine exp + mantissa */
        asmbuf_puts(&buf, "    CMP r1, #0\n");           /* if negative */
        asmbuf_puts(&buf, "    BEQ .L_i2f_done\n");
        asmbuf_puts(&buf, "    MOV r1, #1\n");
        asmbuf_puts(&buf, "    LSL r1, r1, #31\n");      /* sign bit */
        asmbuf_puts(&buf, "    ORR r0, r0, r1\n");
        asmbuf_puts(&buf, ".L_i2f_done:\n");
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n\n");

        /* __aeabi_ui2f: unsigned int (r0) -> float (r0) */
        asmbuf_puts(&buf, ".global __aeabi_ui2f\n.thumb_func\n__aeabi_ui2f:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    MOV r1, #0\n");           /* sign = positive */
        asmbuf_puts(&buf, "    B .L_i2f_conv\n\n");
        /* __aeabi_ul2f: unsigned long long (r0=low, r1=high) -> float (r0).
         * Split-and-scale: high * 2^32 + low, both halves through ui2f. The
         * multiply by a power of two is exact, and every other step matches
         * the truncating behaviour of the rest of this TU-local runtime. */
        asmbuf_puts(&buf, ".global __aeabi_ul2f\n.thumb_func\n__aeabi_ul2f:\n");
        asmbuf_puts(&buf, "    CMP r1, #0\n");
        asmbuf_puts(&buf, "    BNE .L_ul2f_hi\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");            /* high word zero: 32-bit path */
        asmbuf_puts(&buf, "    BL __aeabi_ui2f\n");
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n");
        asmbuf_puts(&buf, ".L_ul2f_hi:\n");
        asmbuf_puts(&buf, "    PUSH {r4, lr}\n");
        asmbuf_puts(&buf, "    MOV r4, r0\n");           /* keep low half */
        asmbuf_puts(&buf, "    MOV r0, r1\n");
        asmbuf_puts(&buf, "    BL __aeabi_ui2f\n");     /* float(high) */
        asmbuf_puts(&buf, "    VMOV s0, r0\n");
        asmbuf_puts(&buf, "    MOVW r0, #0x0000\n");     /* 2^32 as float = 0x4F800000 */
        asmbuf_puts(&buf, "    MOVT r0, #0x4F80\n");
        asmbuf_puts(&buf, "    VMOV s1, r0\n");
        asmbuf_puts(&buf, "    VMUL.F32 s0, s0, s1\n");
        asmbuf_puts(&buf, "    MOV r0, r4\n");
        asmbuf_puts(&buf, "    BL __aeabi_ui2f\n");     /* float(low) */
        asmbuf_puts(&buf, "    VMOV s1, r0\n");
        asmbuf_puts(&buf, "    VADD.F32 s0, s0, s1\n");
        asmbuf_puts(&buf, "    VMOV r0, s0\n");
        asmbuf_puts(&buf, "    POP {r4}\n");            /* r0 keeps the result */
        asmbuf_puts(&buf, "    POP {r1}\n");            /* r1 = return address */
        asmbuf_puts(&buf, "    BX r1\n\n");

        /* __aeabi_fadd: float (r0) + float (r1) -> float (r0)
         * 真正的 IEEE 754 单精度加法：提取符号/指数/尾数，对齐指数，
         * 同号相加异号相减，规格化后组装结果。NaN/Inf 未做完整处理。 */
        asmbuf_puts(&buf, ".global __aeabi_fadd\n.thumb_func\n__aeabi_fadd:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, r6, r7, lr}\n");
        asmbuf_puts(&buf, "    LSR r2, r0, #31\n");          /* a 符号 */
        asmbuf_puts(&buf, "    LSR r3, r0, #23\n");
        asmbuf_puts(&buf, "    LSL r3, r3, #24\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #24\n");          /* a 指数(8位) */
        asmbuf_puts(&buf, "    LSL r4, r0, #9\n");
        asmbuf_puts(&buf, "    LSR r4, r4, #9\n");           /* a 尾数(23位) */
        asmbuf_puts(&buf, "    LSR r5, r1, #31\n");          /* b 符号 */
        asmbuf_puts(&buf, "    LSR r6, r1, #23\n");
        asmbuf_puts(&buf, "    LSL r6, r6, #24\n");
        asmbuf_puts(&buf, "    LSR r6, r6, #24\n");          /* b 指数 */
        asmbuf_puts(&buf, "    LSL r7, r1, #9\n");
        asmbuf_puts(&buf, "    LSR r7, r7, #9\n");           /* b 尾数 */
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fadd_ret_b\n");        /* a 为零返回 b */
        asmbuf_puts(&buf, "    CMP r6, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fadd_ret_a\n");        /* b 为零返回 a */
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");          /* 0x800000 隐含位 */
        asmbuf_puts(&buf, "    ORR r4, r0\n");               /* a 尾数补隐含位 */
        asmbuf_puts(&buf, "    ORR r7, r0\n");               /* b 尾数补隐含位 */
        asmbuf_puts(&buf, "    SUB r0, r3, r6\n");           /* 指数差 */
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    BGE .L_fadd_a_ge\n");
        asmbuf_puts(&buf, "    RSB r0, r0, #0\n");           /* 移位量 = -差 */
        asmbuf_puts(&buf, "    LSR r4, r4, r0\n");           /* a 尾数右移对齐 */
        asmbuf_puts(&buf, "    MOV r3, r6\n");               /* 结果指数取大者 */
        asmbuf_puts(&buf, "    B .L_fadd_aligned\n");
        asmbuf_puts(&buf, ".L_fadd_a_ge:\n");
        asmbuf_puts(&buf, "    LSR r7, r7, r0\n");           /* b 尾数右移对齐 */
        asmbuf_puts(&buf, ".L_fadd_aligned:\n");
        asmbuf_puts(&buf, "    CMP r2, r5\n");
        asmbuf_puts(&buf, "    BEQ .L_fadd_add\n");          /* 同号相加 */
        asmbuf_puts(&buf, "    CMP r4, r7\n");
        asmbuf_puts(&buf, "    BCS .L_fadd_sub_ab\n");       /* a 尾数 >= b 尾数 */
        asmbuf_puts(&buf, "    SUB r4, r7, r4\n");           /* 结果 = b - a */
        asmbuf_puts(&buf, "    MOV r2, r5\n");               /* 符号取 b */
        asmbuf_puts(&buf, "    B .L_fadd_norm\n");
        asmbuf_puts(&buf, ".L_fadd_sub_ab:\n");
        asmbuf_puts(&buf, "    SUB r4, r4, r7\n");           /* 结果 = a - b */
        asmbuf_puts(&buf, "    B .L_fadd_norm\n");
        asmbuf_puts(&buf, ".L_fadd_add:\n");
        asmbuf_puts(&buf, "    ADD r4, r4, r7\n");           /* 结果 = a + b */
        asmbuf_puts(&buf, ".L_fadd_norm:\n");
        asmbuf_puts(&buf, "    CMP r4, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fadd_zero\n");
        asmbuf_puts(&buf, "    MOV r1, #1\n");
        asmbuf_puts(&buf, "    LSL r1, r1, #24\n");          /* bit24 掩码 */
        asmbuf_puts(&buf, "    TST r4, r1\n");
        asmbuf_puts(&buf, "    BEQ .L_fadd_nocarry\n");
        asmbuf_puts(&buf, "    LSR r4, r4, #1\n");           /* 加法进位：右移1 */
        asmbuf_puts(&buf, "    ADD r3, r3, #1\n");
        asmbuf_puts(&buf, "    B .L_fadd_chk\n");
        asmbuf_puts(&buf, ".L_fadd_nocarry:\n");
        asmbuf_puts(&buf, "    MOV r1, #1\n");
        asmbuf_puts(&buf, "    LSL r1, r1, #23\n");          /* bit23 掩码 */
        asmbuf_puts(&buf, "    TST r4, r1\n");
        asmbuf_puts(&buf, "    BNE .L_fadd_chk\n");          /* 已规格化 */
        asmbuf_puts(&buf, ".L_fadd_shl:\n");
        asmbuf_puts(&buf, "    LSL r4, r4, #1\n");           /* 左移补位 */
        asmbuf_puts(&buf, "    SUB r3, r3, #1\n");
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BLE .L_fadd_zero\n");         /* 指数下溢 */
        asmbuf_puts(&buf, "    TST r4, r1\n");
        asmbuf_puts(&buf, "    BEQ .L_fadd_shl\n");
        asmbuf_puts(&buf, ".L_fadd_chk:\n");
        asmbuf_puts(&buf, "    CMP r3, #255\n");
        asmbuf_puts(&buf, "    BGE .L_fadd_inf\n");          /* 溢出 -> Inf */
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BLE .L_fadd_zero\n");
        asmbuf_puts(&buf, "    MOV r1, #1\n");
        asmbuf_puts(&buf, "    LSL r1, r1, #23\n");          /* bit23 掩码 */
        asmbuf_puts(&buf, "    BIC r4, r1\n");               /* 清除隐含位 */
        asmbuf_puts(&buf, "    LSL r3, r3, #23\n");          /* 指数就位 */
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");          /* 符号就位 */
        asmbuf_puts(&buf, "    MOV r0, r3\n");
        asmbuf_puts(&buf, "    ORR r0, r4\n");
        asmbuf_puts(&buf, "    ORR r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fadd_ret_b:\n");
        asmbuf_puts(&buf, "    MOV r0, r1\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fadd_ret_a:\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fadd_zero:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fadd_inf:\n");
        asmbuf_puts(&buf, "    MOV r1, #255\n");
        asmbuf_puts(&buf, "    LSL r1, r1, #23\n");          /* exp=0xFF -> Inf */
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    MOV r0, r1\n");
        asmbuf_puts(&buf, "    ORR r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n\n");

        /* __aeabi_fsub: 翻转 b 符号位后尾调用 fadd */
        asmbuf_puts(&buf, ".global __aeabi_fsub\n.thumb_func\n__aeabi_fsub:\n");
        asmbuf_puts(&buf, "    MOV r2, #1\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");          /* 0x80000000 */
        asmbuf_puts(&buf, "    EOR r1, r2\n");               /* 翻转 b 符号 */
        asmbuf_puts(&buf, "    B __aeabi_fadd\n\n");

        /* __aeabi_fmul: float (r0) * float (r1) -> float (r0)
         * 尾数用 UMULL 做 32x32->64 乘法，取乘积>>23 规格化。 */
        asmbuf_puts(&buf, ".global __aeabi_fmul\n.thumb_func\n__aeabi_fmul:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, r6, r7, lr}\n");
        asmbuf_puts(&buf, "    LSR r2, r0, #31\n");          /* a 符号 */
        asmbuf_puts(&buf, "    LSR r3, r0, #23\n");
        asmbuf_puts(&buf, "    LSL r3, r3, #24\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #24\n");          /* a 指数 */
        asmbuf_puts(&buf, "    LSL r4, r0, #9\n");
        asmbuf_puts(&buf, "    LSR r4, r4, #9\n");           /* a 尾数 */
        asmbuf_puts(&buf, "    LSR r5, r1, #31\n");          /* b 符号 */
        asmbuf_puts(&buf, "    LSR r6, r1, #23\n");
        asmbuf_puts(&buf, "    LSL r6, r6, #24\n");
        asmbuf_puts(&buf, "    LSR r6, r6, #24\n");          /* b 指数 */
        asmbuf_puts(&buf, "    LSL r7, r1, #9\n");
        asmbuf_puts(&buf, "    LSR r7, r7, #9\n");           /* b 尾数 */
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fmul_zero\n");
        asmbuf_puts(&buf, "    CMP r6, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fmul_zero\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");          /* 隐含位 */
        asmbuf_puts(&buf, "    ORR r4, r0\n");
        asmbuf_puts(&buf, "    ORR r7, r0\n");
        asmbuf_puts(&buf, "    EOR r2, r5\n");               /* 结果符号 */
        asmbuf_puts(&buf, "    ADD r3, r3, r6\n");
        asmbuf_puts(&buf, "    SUB r3, r3, #127\n");         /* 结果指数 */
        asmbuf_puts(&buf, "    UMULL r0, r1, r4, r7\n");     /* r1:r0 = a_mant * b_mant */
        asmbuf_puts(&buf, "    LSR r0, r0, #23\n");          /* 低字 >> 23 */
        asmbuf_puts(&buf, "    LSL r1, r1, #9\n");           /* 高字 << 9 */
        asmbuf_puts(&buf, "    MOV r4, r1\n");
        asmbuf_puts(&buf, "    ORR r4, r0\n");               /* r4 = 乘积 >> 23 */
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #24\n");          /* bit24 掩码 */
        asmbuf_puts(&buf, "    TST r4, r0\n");
        asmbuf_puts(&buf, "    BEQ .L_fmul_chk\n");
        asmbuf_puts(&buf, "    LSR r4, r4, #1\n");           /* 进位右移1 */
        asmbuf_puts(&buf, "    ADD r3, r3, #1\n");
        asmbuf_puts(&buf, ".L_fmul_chk:\n");
        asmbuf_puts(&buf, "    CMP r3, #255\n");
        asmbuf_puts(&buf, "    BGE .L_fmul_inf\n");
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BLE .L_fmul_zero\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");          /* bit23 掩码 */
        asmbuf_puts(&buf, "    BIC r4, r0\n");               /* 清除隐含位 */
        asmbuf_puts(&buf, "    LSL r3, r3, #23\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    MOV r0, r3\n");
        asmbuf_puts(&buf, "    ORR r0, r4\n");
        asmbuf_puts(&buf, "    ORR r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fmul_zero:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fmul_inf:\n");
        asmbuf_puts(&buf, "    MOV r0, #255\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    ORR r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n\n");

        /* __aeabi_fdiv: float (r0) / float (r1) -> float (r0)
         * 尾数 = (a_mant<<23) / b_mant，64 位除法复用 __aeabi_uldivmod。 */
        asmbuf_puts(&buf, ".global __aeabi_fdiv\n.thumb_func\n__aeabi_fdiv:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, r6, r7, lr}\n");
        asmbuf_puts(&buf, "    LSR r2, r0, #31\n");          /* a 符号 */
        asmbuf_puts(&buf, "    LSR r3, r0, #23\n");
        asmbuf_puts(&buf, "    LSL r3, r3, #24\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #24\n");          /* a 指数 */
        asmbuf_puts(&buf, "    LSL r4, r0, #9\n");
        asmbuf_puts(&buf, "    LSR r4, r4, #9\n");           /* a 尾数 */
        asmbuf_puts(&buf, "    LSR r5, r1, #31\n");          /* b 符号 */
        asmbuf_puts(&buf, "    LSR r6, r1, #23\n");
        asmbuf_puts(&buf, "    LSL r6, r6, #24\n");
        asmbuf_puts(&buf, "    LSR r6, r6, #24\n");          /* b 指数 */
        asmbuf_puts(&buf, "    LSL r7, r1, #9\n");
        asmbuf_puts(&buf, "    LSR r7, r7, #9\n");           /* b 尾数 */
        asmbuf_puts(&buf, "    CMP r6, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fdiv_inf\n");          /* 除以零 -> Inf */
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fdiv_zero\n");         /* 0 / x = 0 */
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");          /* 隐含位 */
        asmbuf_puts(&buf, "    ORR r4, r0\n");
        asmbuf_puts(&buf, "    ORR r7, r0\n");
        asmbuf_puts(&buf, "    EOR r5, r2\n");               /* 结果符号 -> r5 */
        asmbuf_puts(&buf, "    SUB r6, r3, r6\n");
        asmbuf_puts(&buf, "    ADD r6, r6, #127\n");         /* 结果指数 -> r6 */
        asmbuf_puts(&buf, "    LSR r1, r4, #9\n");           /* 被除数高字 = a_mant>>9 */
        asmbuf_puts(&buf, "    LSL r0, r4, #23\n");          /* 被除数低字 = a_mant<<23 */
        asmbuf_puts(&buf, "    MOV r2, r7\n");               /* 除数 = b_mant */
        asmbuf_puts(&buf, "    MOV r3, #0\n");
        asmbuf_puts(&buf, "    BL __aeabi_uldivmod\n");      /* r0:r1 = 商 */
        asmbuf_puts(&buf, "    MOV r4, r0\n");               /* r4 = 结果尾数 */
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");          /* bit23 掩码 */
        asmbuf_puts(&buf, "    TST r4, r0\n");
        asmbuf_puts(&buf, "    BNE .L_fdiv_chk\n");          /* bit23 已置位 */
        asmbuf_puts(&buf, "    LSL r4, r4, #1\n");           /* 否则左移1，指数-1 */
        asmbuf_puts(&buf, "    SUB r6, r6, #1\n");
        asmbuf_puts(&buf, ".L_fdiv_chk:\n");
        asmbuf_puts(&buf, "    CMP r6, #255\n");
        asmbuf_puts(&buf, "    BGE .L_fdiv_inf\n");
        asmbuf_puts(&buf, "    CMP r6, #0\n");
        asmbuf_puts(&buf, "    BLE .L_fdiv_zero\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");
        asmbuf_puts(&buf, "    BIC r4, r0\n");               /* 清除隐含位 */
        asmbuf_puts(&buf, "    LSL r6, r6, #23\n");
        asmbuf_puts(&buf, "    LSL r5, r5, #31\n");
        asmbuf_puts(&buf, "    MOV r0, r6\n");
        asmbuf_puts(&buf, "    ORR r0, r4\n");
        asmbuf_puts(&buf, "    ORR r0, r5\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fdiv_zero:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n");
        asmbuf_puts(&buf, ".L_fdiv_inf:\n");
        asmbuf_puts(&buf, "    MOV r0, #255\n");
        asmbuf_puts(&buf, "    LSL r0, r0, #23\n");
        asmbuf_puts(&buf, "    LSL r5, r5, #31\n");
        asmbuf_puts(&buf, "    ORR r0, r5\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, r7, pc}\n\n");

        /* Double-precision helpers: for now, truncate to single and use
         * single-precision helpers. This loses precision but allows linking. */
        asmbuf_puts(&buf, ".global __aeabi_d2iz\n.thumb_func\n__aeabi_d2iz:\n");
        asmbuf_puts(&buf, "    POP {r1}\n");             /* discard high word */
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __aeabi_f2iz\n");
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n\n");

        asmbuf_puts(&buf, ".global __aeabi_d2uiz\n.thumb_func\n__aeabi_d2uiz:\n");
        asmbuf_puts(&buf, "    POP {r1}\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __aeabi_f2uiz\n");
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n\n");

        asmbuf_puts(&buf, ".global __aeabi_i2d\n.thumb_func\n__aeabi_i2d:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __aeabi_i2f\n");
        asmbuf_puts(&buf, "    MOV r1, #0\n");           /* high word = 0 */
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n\n");

        asmbuf_puts(&buf, ".global __aeabi_ui2d\n.thumb_func\n__aeabi_ui2d:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __aeabi_ui2f\n");
        asmbuf_puts(&buf, "    MOV r1, #0\n");
        asmbuf_puts(&buf, "    POP {r0}\n");
        asmbuf_puts(&buf, "    BX r0\n\n");

        /* 双精度算术：a=r0(低):r1(高)，b=r2(低):r3(高)，结果=r0(低):r1(高)。
         * 策略：将两个 double 截断为 single（__arkcc_d2f_pair 返回 r0/r1），
         * 调用对应单精度运算，再用 __arkcc_f2d_ext 扩展回 double。精度有限但可链接。 */
        asmbuf_puts(&buf, ".global __aeabi_dadd\n.thumb_func\n__aeabi_dadd:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");      /* r0=a_s, r1=b_s */
        asmbuf_puts(&buf, "    BL __aeabi_fadd\n");
        asmbuf_puts(&buf, "    BL __arkcc_f2d_ext\n");       /* r0 -> r0:r1 */
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_dsub\n.thumb_func\n__aeabi_dsub:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fsub\n");
        asmbuf_puts(&buf, "    BL __arkcc_f2d_ext\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_dmul\n.thumb_func\n__aeabi_dmul:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fmul\n");
        asmbuf_puts(&buf, "    BL __arkcc_f2d_ext\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_ddiv\n.thumb_func\n__aeabi_ddiv:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fdiv\n");
        asmbuf_puts(&buf, "    BL __arkcc_f2d_ext\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        /* 单精度比较：返回 1 为真，0 为假。NaN 时除 != 外皆返回 0（!= 由调用方翻转）。
         * __arkcc_fcmp3 返回 r0: -1(a<b) / 0(==) / 1(a>b) / 2(无序/NaN)。
         * 用符号-幅值到二进制补码的单调映射做整数比较，+0==-0。 */
        asmbuf_puts(&buf, ".global __aeabi_fcmpeq\n.thumb_func\n__aeabi_fcmpeq:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_fcmp3\n");
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    ITE EQ\n");
        asmbuf_puts(&buf, "    MOVEQ r0, #1\n");
        asmbuf_puts(&buf, "    MOVNE r0, #0\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_fcmplt\n.thumb_func\n__aeabi_fcmplt:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_fcmp3\n");
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    ITE LT\n");
        asmbuf_puts(&buf, "    MOVLT r0, #1\n");
        asmbuf_puts(&buf, "    MOVGE r0, #0\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_fcmpgt\n.thumb_func\n__aeabi_fcmpgt:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_fcmp3\n");
        asmbuf_puts(&buf, "    CMP r0, #1\n");
        asmbuf_puts(&buf, "    ITE EQ\n");
        asmbuf_puts(&buf, "    MOVEQ r0, #1\n");
        asmbuf_puts(&buf, "    MOVNE r0, #0\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_fcmple\n.thumb_func\n__aeabi_fcmple:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_fcmp3\n");
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    ITE LE\n");
        asmbuf_puts(&buf, "    MOVLE r0, #1\n");
        asmbuf_puts(&buf, "    MOVGT r0, #0\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_fcmpge\n.thumb_func\n__aeabi_fcmpge:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_fcmp3\n");
        asmbuf_puts(&buf, "    CMP r0, #2\n");              /* NaN -> 假 */
        asmbuf_puts(&buf, "    BEQ .L_fcmpge_false\n");
        asmbuf_puts(&buf, "    CMP r0, #0\n");
        asmbuf_puts(&buf, "    BGE .L_fcmpge_true\n");      /* 0 或 1 -> 真 */
        asmbuf_puts(&buf, ".L_fcmpge_false:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    B .L_fcmpge_done\n");
        asmbuf_puts(&buf, ".L_fcmpge_true:\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, ".L_fcmpge_done:\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        /* 双精度比较：截断到单精度后调用对应单精度比较。
         * a=r0:r1，b=r2:r3，返回 r0 (0/1)。 */
        asmbuf_puts(&buf, ".global __aeabi_dcmpeq\n.thumb_func\n__aeabi_dcmpeq:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fcmpeq\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_dcmplt\n.thumb_func\n__aeabi_dcmplt:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fcmplt\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_dcmpgt\n.thumb_func\n__aeabi_dcmpgt:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fcmpgt\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_dcmple\n.thumb_func\n__aeabi_dcmple:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fcmple\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        asmbuf_puts(&buf, ".global __aeabi_dcmpge\n.thumb_func\n__aeabi_dcmpge:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_pair\n");
        asmbuf_puts(&buf, "    BL __aeabi_fcmpge\n");
        asmbuf_puts(&buf, "    POP {pc}\n\n");

        /* === 内部辅助函数（非导出，仅供上述 helper 调用）=== */

        /* __arkcc_fcmp3: 单精度三值比较。r0=a, r1=b -> r0 ∈ {-1,0,1,2}。
         * NaN(指数=0xFF 且尾数!=0) -> 2；+0==-0；否则按单调映射做有符号整数比较。 */
        asmbuf_puts(&buf, ".thumb_func\n__arkcc_fcmp3:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, lr}\n");
        asmbuf_puts(&buf, "    LSL r2, r0, #1\n");
        asmbuf_puts(&buf, "    LSR r2, r2, #24\n");          /* a 指数 */
        asmbuf_puts(&buf, "    LSL r3, r0, #9\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #9\n");           /* a 尾数 */
        asmbuf_puts(&buf, "    CMP r2, #255\n");
        asmbuf_puts(&buf, "    BNE .L_fc3_a_ok\n");
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BNE .L_fc3_unord\n");         /* a 是 NaN */
        asmbuf_puts(&buf, ".L_fc3_a_ok:\n");
        asmbuf_puts(&buf, "    LSL r2, r1, #1\n");
        asmbuf_puts(&buf, "    LSR r2, r2, #24\n");          /* b 指数 */
        asmbuf_puts(&buf, "    LSL r3, r1, #9\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #9\n");           /* b 尾数 */
        asmbuf_puts(&buf, "    CMP r2, #255\n");
        asmbuf_puts(&buf, "    BNE .L_fc3_b_ok\n");
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BNE .L_fc3_unord\n");         /* b 是 NaN */
        asmbuf_puts(&buf, ".L_fc3_b_ok:\n");
        asmbuf_puts(&buf, "    LSL r2, r0, #1\n");
        asmbuf_puts(&buf, "    LSR r2, r2, #1\n");           /* a 幅值(清符号) */
        asmbuf_puts(&buf, "    LSL r3, r1, #1\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #1\n");           /* b 幅值 */
        asmbuf_puts(&buf, "    MOV r4, r2\n");
        asmbuf_puts(&buf, "    ORR r4, r3\n");               /* r4 = a_mag | b_mag */
        asmbuf_puts(&buf, "    CMP r4, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fc3_eq\n");            /* 两者皆零 -> 相等 */
        asmbuf_puts(&buf, "    LSR r4, r0, #31\n");          /* a 符号 */
        asmbuf_puts(&buf, "    CMP r4, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fc3_apos\n");
        asmbuf_puts(&buf, "    RSB r2, r2, #0\n");           /* Ka = -a_mag */
        asmbuf_puts(&buf, ".L_fc3_apos:\n");
        asmbuf_puts(&buf, "    LSR r5, r1, #31\n");          /* b 符号 */
        asmbuf_puts(&buf, "    CMP r5, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_fc3_bpos\n");
        asmbuf_puts(&buf, "    RSB r3, r3, #0\n");           /* Kb = -b_mag */
        asmbuf_puts(&buf, ".L_fc3_bpos:\n");
        asmbuf_puts(&buf, "    CMP r2, r3\n");
        asmbuf_puts(&buf, "    BEQ .L_fc3_eq\n");
        asmbuf_puts(&buf, "    BGT .L_fc3_gt\n");
        asmbuf_puts(&buf, "    MVN r0, #0\n");               /* a < b -> -1 */
        asmbuf_puts(&buf, "    B .L_fc3_done\n");
        asmbuf_puts(&buf, ".L_fc3_gt:\n");
        asmbuf_puts(&buf, "    MOV r0, #1\n");
        asmbuf_puts(&buf, "    B .L_fc3_done\n");
        asmbuf_puts(&buf, ".L_fc3_eq:\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    B .L_fc3_done\n");
        asmbuf_puts(&buf, ".L_fc3_unord:\n");
        asmbuf_puts(&buf, "    MOV r0, #2\n");
        asmbuf_puts(&buf, ".L_fc3_done:\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n\n");

        /* __arkcc_d2f_trunc: double (r0=低, r1=高) 截断为 single (r0)。
         * 高字含符号/指数/尾数高位；单精度尾数取双精度尾数高 23 位。 */
        asmbuf_puts(&buf, ".thumb_func\n__arkcc_d2f_trunc:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, lr}\n");
        asmbuf_puts(&buf, "    LSR r2, r1, #31\n");          /* 符号 */
        asmbuf_puts(&buf, "    LSR r3, r1, #20\n");
        asmbuf_puts(&buf, "    LSL r3, r3, #21\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #21\n");          /* double 指数(11位) */
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_d2f_zero\n");          /* 零/非规格化 */
        asmbuf_puts(&buf, "    MOVW r4, #0x7FF\n");
        asmbuf_puts(&buf, "    CMP r3, r4\n");
        asmbuf_puts(&buf, "    BEQ .L_d2f_inf\n");           /* Inf/NaN */
        asmbuf_puts(&buf, "    SUB r4, r3, #896\n");         /* single 指数 = exp_d - 896 */
        asmbuf_puts(&buf, "    CMP r4, #0\n");
        asmbuf_puts(&buf, "    BLE .L_d2f_zero\n");          /* 下溢 */
        asmbuf_puts(&buf, "    CMP r4, #255\n");
        asmbuf_puts(&buf, "    BGE .L_d2f_inf\n");           /* 上溢 */
        asmbuf_puts(&buf, "    LSL r5, r1, #12\n");
        asmbuf_puts(&buf, "    LSR r5, r5, #12\n");          /* r1 & 0xFFFFF (尾数高位) */
        asmbuf_puts(&buf, "    LSL r5, r5, #3\n");           /* << 3 */
        asmbuf_puts(&buf, "    LSR r0, r0, #29\n");          /* 低字 >> 29 (尾数低位) */
        asmbuf_puts(&buf, "    ORR r5, r0\n");               /* 23 位尾数 */
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    LSL r4, r4, #23\n");
        asmbuf_puts(&buf, "    MOV r0, r4\n");
        asmbuf_puts(&buf, "    ORR r0, r5\n");
        asmbuf_puts(&buf, "    ORR r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n");
        asmbuf_puts(&buf, ".L_d2f_zero:\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    MOV r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n");
        asmbuf_puts(&buf, ".L_d2f_inf:\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    MOV r4, #255\n");
        asmbuf_puts(&buf, "    LSL r4, r4, #23\n");
        asmbuf_puts(&buf, "    MOV r0, r4\n");
        asmbuf_puts(&buf, "    ORR r0, r2\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n\n");

        /* __arkcc_f2d_ext: single (r0) 扩展为 double (r0=低, r1=高)。
         * 双精度尾数 = 单精度 23 位尾数左移到高 23 位，低 29 位补 0。 */
        asmbuf_puts(&buf, ".thumb_func\n__arkcc_f2d_ext:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, lr}\n");
        asmbuf_puts(&buf, "    LSR r2, r0, #31\n");          /* 符号 */
        asmbuf_puts(&buf, "    LSR r3, r0, #23\n");
        asmbuf_puts(&buf, "    LSL r3, r3, #24\n");
        asmbuf_puts(&buf, "    LSR r3, r3, #24\n");          /* single 指数 */
        asmbuf_puts(&buf, "    CMP r3, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_f2d_zero\n");
        asmbuf_puts(&buf, "    CMP r3, #255\n");
        asmbuf_puts(&buf, "    BEQ .L_f2d_inf\n");
        asmbuf_puts(&buf, "    ADD r4, r3, #896\n");         /* double 指数 */
        asmbuf_puts(&buf, "    LSL r5, r0, #9\n");
        asmbuf_puts(&buf, "    LSR r5, r5, #9\n");           /* single 尾数 */
        asmbuf_puts(&buf, "    LSL r3, r5, #29\n");          /* 低字 = 尾数 << 29 */
        asmbuf_puts(&buf, "    LSR r5, r5, #3\n");           /* 尾数高位 = 尾数 >> 3 */
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    LSL r4, r4, #20\n");
        asmbuf_puts(&buf, "    MOV r1, r4\n");
        asmbuf_puts(&buf, "    ORR r1, r5\n");
        asmbuf_puts(&buf, "    ORR r1, r2\n");
        asmbuf_puts(&buf, "    MOV r0, r3\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n");
        asmbuf_puts(&buf, ".L_f2d_zero:\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    MOV r1, r2\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n");
        asmbuf_puts(&buf, ".L_f2d_inf:\n");
        asmbuf_puts(&buf, "    LSL r2, r2, #31\n");
        asmbuf_puts(&buf, "    MOVW r3, #0x7FF\n");
        asmbuf_puts(&buf, "    LSL r3, r3, #20\n");          /* 0x7FF00000 */
        asmbuf_puts(&buf, "    MOV r1, r3\n");
        asmbuf_puts(&buf, "    ORR r1, r2\n");
        asmbuf_puts(&buf, "    MOV r0, #0\n");
        asmbuf_puts(&buf, "    POP {r4, r5, pc}\n\n");

        /* __arkcc_d2f_pair: 两个 double (a=r0:r1, b=r2:r3) 截断为两个 single，
         * 返回 r0=a_single, r1=b_single。b 先保存到 r4/r5 以免被 d2f_trunc 破坏。 */
        asmbuf_puts(&buf, ".thumb_func\n__arkcc_d2f_pair:\n");
        asmbuf_puts(&buf, "    PUSH {r4, r5, r6, lr}\n");
        asmbuf_puts(&buf, "    MOV r4, r2\n");               /* 保存 b 低 */
        asmbuf_puts(&buf, "    MOV r5, r3\n");               /* 保存 b 高 */
        asmbuf_puts(&buf, "    BL __arkcc_d2f_trunc\n");     /* r0 = a single */
        asmbuf_puts(&buf, "    MOV r6, r0\n");               /* 暂存 a single */
        asmbuf_puts(&buf, "    MOV r0, r4\n");
        asmbuf_puts(&buf, "    MOV r1, r5\n");
        asmbuf_puts(&buf, "    BL __arkcc_d2f_trunc\n");     /* r0 = b single */
        asmbuf_puts(&buf, "    MOV r1, r0\n");
        asmbuf_puts(&buf, "    MOV r0, r6\n");
        asmbuf_puts(&buf, "    POP {r4, r5, r6, pc}\n\n");
    }

    /* Globals: SRAM addresses are assigned by gvar_assign() above and loaded
     * via MOVW/MOVT in IR_PUSH_GLOBAL etc. .data globals are initialized at
     * startup by __arkcc_init_globals (generated above). .bss globals are
     * zeroed by the same function. __at variables use their absolute address.
     * Note: .set directives for alias functions are emitted in the function
     * code section above, not here. */
    if (0 && prog->global_count > 0) {
        int gi;
        int cur_sec = 0;  /* 0=.text, 1=.data, 2=.bss, 3=.rodata, 4=custom */
        for (gi = 0; gi < prog->global_count; gi++) {
            IrGlobal *g = prog->globals[gi];
            int sz;
            int want_sec = 0;
            const char *want_custom = 0;
            if (g->is_extern) continue;
            /* __at(addr): define the symbol directly as an absolute address constant (no storage allocated).
             *   .global name
             *   .set name, 0xADDR
             * This way every LDR rN, =name loads the MMIO physical address,
             * and subsequent LDR/STR [rN] accesses the peripheral register. */
            if (g->has_at) {
                if (!g->is_static) {
                    asmbuf_puts(&buf, ".global ");
                    asmbuf_puts(&buf, g->name);
                    asmbuf_putc(&buf, '\n');
                }
                asmbuf_puts(&buf, ".set ");
                asmbuf_puts(&buf, g->name);
                asmbuf_puts(&buf, ", 0x");
                {
                    unsigned a = g->at_addr;
                    char hex[9]; int k;
                    const char *hd = "0123456789abcdef";
                    for (k = 7; k >= 0; k--) { hex[k] = hd[a & 0xF]; a >>= 4; }
                    hex[8] = 0;
                    asmbuf_puts(&buf, hex);
                }
                asmbuf_putc(&buf, '\n');
                continue;
            }
            sz = g->type ? type_size(g->type) : 4;
            if (sz <= 0) sz = 4;
            /* Choose section: custom section > no-init (.bss) > const (.rodata) > .data */
            if (g->section) {
                want_custom = g->section;
            } else if (!g->has_init) {
                want_sec = 2;
            } else if (g->is_const) {
                want_sec = 3;
            } else {
                want_sec = 1;
            }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* __thread thread-local storage: use .tbss (no init) / .tdata (has init) instead.
             * These two sections are instantiated per-thread by the loader in each thread context. */
            if (g->is_tls) {
                if (!g->has_init || g->init_aggregate) {
                    want_sec = 5;  /* .tbss */
                } else {
                    want_sec = 6;  /* .tdata */
                }
            }
#endif
            /* Switch section (only emit section directive if different from current) */
            if (want_custom) {
                asmbuf_puts(&buf, ".section \"");
                asmbuf_puts(&buf, want_custom);
                asmbuf_puts(&buf, "\"");
#if ARKCC_FEATURE_SCATTER
                /* SCT-1: annotate the section's scatter exec region base so
                 * the resolved layout is visible in the emitted assembly. */
                if (g_scatter_loaded) {
                    unsigned sect_base =
                        scatter_get_er_base_for_section(&g_scatter, want_custom);
                    if (sect_base) {
                        char hex[9]; int k;
                        const char *hd = "0123456789ABCDEF";
                        asmbuf_puts(&buf, "  @ base 0x");
                        for (k = 7; k >= 0; k--) { hex[k] = hd[sect_base & 0xF]; sect_base >>= 4; }
                        hex[8] = 0;
                        asmbuf_puts(&buf, hex);
                    }
                }
#endif
                asmbuf_putc(&buf, '\n');
                cur_sec = 4;
            } else if (cur_sec != want_sec) {
                if (want_sec == 1) asmbuf_puts(&buf, ".data\n");
                else if (want_sec == 2) asmbuf_puts(&buf, ".bss\n");
                else if (want_sec == 3) asmbuf_puts(&buf, ".section .rodata\n");
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                else if (want_sec == 5) asmbuf_puts(&buf, ".section .tbss,\"aw\",%nobits\n");
                else if (want_sec == 6) asmbuf_puts(&buf, ".section .tdata,\"aw\",%progbits\n");
#endif
                cur_sec = want_sec;
            }
            /* Symbol attributes + label */
            if (!g->is_static) {
                asmbuf_puts(&buf, ".global ");
                asmbuf_puts(&buf, g->name);
                asmbuf_putc(&buf, '\n');
            }
            /* Alignment: __align(n) / aligned(n) takes priority; otherwise default 4 bytes */
            if (g->align_bytes > 4) {
                asmbuf_puts(&buf, ".balign ");
                asmbuf_int(&buf, g->align_bytes);
                asmbuf_putc(&buf, '\n');
            } else {
                asmbuf_puts(&buf, ".align 4\n");
            }
            asmbuf_puts(&buf, g->name);
            asmbuf_puts(&buf, ":\n");
            /* Data */
            if (!g->has_init || g->init_aggregate) {
                /* Zero-init or aggregate (initialized at runtime): reserve space */
                asmbuf_puts(&buf, "    .zero ");
                asmbuf_int(&buf, sz);
                asmbuf_putc(&buf, '\n');
            } else if (g->is_float) {
                /* Float initialization: convert to single-precision bit pattern (consistent with IR_PUSH_FLOAT) */
                union { float f; unsigned int u; } u;
                unsigned int bits;
                char hex[9];
                int k;
                const char *hd = "0123456789abcdef";
                u.f = (float)g->init_float;
                bits = u.u;
                for (k = 7; k >= 0; k--) { hex[k] = hd[bits & 0xF]; bits >>= 4; }
                hex[8] = 0;
                asmbuf_puts(&buf, "    .word 0x");
                asmbuf_puts(&buf, hex);
                asmbuf_putc(&buf, '\n');
            } else if (g->init_str) {
                /* String array initialization: same escaping as IR_PUSH_STR */
                int slen = ark_strlen(g->init_str);
                int k;
                asmbuf_puts(&buf, "    .asciz \"");
                for (k = 0; k < slen; k++) {
                    char c = g->init_str[k];
                    if (c == '\n') asmbuf_puts(&buf, "\\n");
                    else if (c == '\t') asmbuf_puts(&buf, "\\t");
                    else if (c == '\\') asmbuf_puts(&buf, "\\\\");
                    else if (c == '"')  asmbuf_puts(&buf, "\\\"");
                    else if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) {
                        char hb[8]; int hn = 0;
                        const char *hh = "0123456789abcdef";
                        hb[hn++] = '\\'; hb[hn++] = 'x';
                        hb[hn++] = hh[((unsigned char)c >> 4) & 0xF];
                        hb[hn++] = hh[(unsigned char)c & 0xF];
                        hb[hn] = 0;
                        asmbuf_puts(&buf, hb);
                    } else {
                        asmbuf_putc(&buf, c);
                    }
                }
                asmbuf_puts(&buf, "\"\n");
                /* Pad remaining space with zeros (e.g. char[10] = "hi") */
                if (slen + 1 < sz) {
                    asmbuf_puts(&buf, "    .zero ");
                    asmbuf_int(&buf, sz - (slen + 1));
                    asmbuf_putc(&buf, '\n');
                }
            } else {
                /* Scalar initialization */
                asmbuf_puts(&buf, "    .word ");
                asmbuf_int(&buf, g->init_int);
                asmbuf_putc(&buf, '\n');
                if (sz > 4) {
                    asmbuf_puts(&buf, "    .zero ");
                    asmbuf_int(&buf, sz - 4);
                    asmbuf_putc(&buf, '\n');
                }
            }
        }
        /* Restore .text section for later manual appending */
        asmbuf_puts(&buf, ".text\n\n");
    }
#if ARKCC_FEATURE_DWARF_FULL
    /* DWARF debug info (enabled by --dwarf2/--dwarf3):
     * Emit .debug_info/.debug_abbrev/.debug_line sections recording the compilation unit,
     * function symbols and line number maps, for GDB/LLDB single-stepping. Uses a DWARF v2 subset:
     * only the CU header + per-function DIE + line number program; no variable location info (simplified).
     * Detailed type info references predefined DW_TAGs in .debug_abbrev via DW_FORM_ref4. */
    if (g_config.dwarf_version >= 2) {
        int fi2;
        const char *fname = prog->source_file ? prog->source_file : "input.c";
        /* .debug_abbrev: abbreviation table, defines DIE tag/attribute forms */
        asmbuf_puts(&buf, ".section .debug_abbrev,\"\",%progbits\n");
        asmbuf_puts(&buf, ".balign 4\n");
        asmbuf_puts(&buf, "    .byte 1\n");              /* abbrev code 1 */
        asmbuf_puts(&buf, "    .byte 17\n");             /* DW_TAG_compile_unit */
        asmbuf_puts(&buf, "    .byte 0\n");              /* DW_CHILDREN_yes */
        asmbuf_puts(&buf, "    .byte 3\n  .byte 8\n");   /* DW_AT_name, DW_FORM_string */
        asmbuf_puts(&buf, "    .byte 16\n  .byte 6\n");  /* DW_AT_stmt_list, DW_FORM_data4 */
        asmbuf_puts(&buf, "    .byte 0\n  .byte 0\n");   /* end attr/end form */
        asmbuf_puts(&buf, "    .byte 2\n");              /* abbrev code 2 */
        asmbuf_puts(&buf, "    .byte 46\n");             /* DW_TAG_subprogram */
        asmbuf_puts(&buf, "    .byte 0\n");              /* DW_CHILDREN_no */
        asmbuf_puts(&buf, "    .byte 3\n  .byte 8\n");   /* DW_AT_name, DW_FORM_string */
        asmbuf_puts(&buf, "    .byte 17\n  .byte 1\n");  /* DW_AT_low_pc, DW_FORM_addr */
        asmbuf_puts(&buf, "    .byte 18\n  .byte 1\n");  /* DW_AT_high_pc, DW_FORM_addr */
        asmbuf_puts(&buf, "    .byte 0\n  .byte 0\n");   /* end attr/end form */
        asmbuf_puts(&buf, "    .byte 0\n");              /* end abbrev table */
        /* .debug_info: compilation unit + per-function DIE */
        asmbuf_puts(&buf, ".section .debug_info,\"\",%progbits\n");
        asmbuf_puts(&buf, ".balign 4\n");
        asmbuf_puts(&buf, "    .word 0\n");              /* unit_length (placeholder, simplified) */
        asmbuf_puts(&buf, "    .half 2\n");              /* DWARF version 2 */
        asmbuf_puts(&buf, "    .word 0\n");              /* debug_abbrev_offset */
        asmbuf_puts(&buf, "    .byte 4\n");              /* address_size */
        asmbuf_puts(&buf, "    .byte 1\n");              /* abbrev 1 = compile_unit */
        asmbuf_puts(&buf, "    .asciz \"");
        asmbuf_puts(&buf, fname);
        asmbuf_puts(&buf, "\"\n");
        asmbuf_puts(&buf, "    .word 0\n");              /* stmt_list offset */
        for (fi2 = 0; fi2 < prog->func_count; fi2++) {
            IrFunc *f2 = prog->funcs[fi2];
            if (f2->alias_target) continue;
            asmbuf_puts(&buf, "    .byte 2\n");          /* abbrev 2 = subprogram */
            asmbuf_puts(&buf, "    .asciz \"");
            asmbuf_puts(&buf, f2->name);
            asmbuf_puts(&buf, "\"\n");
            asmbuf_puts(&buf, "    .word ");
            asmbuf_puts(&buf, f2->name);
            asmbuf_putc(&buf, '\n');                     /* low_pc */
            asmbuf_puts(&buf, "    .word ");
            asmbuf_puts(&buf, f2->name);
            asmbuf_puts(&buf, "_end\n");                 /* high_pc */
        }
        asmbuf_puts(&buf, "    .byte 0\n");              /* end DIE list */
        /* .debug_line: line number program (file name table + end entry only) */
        asmbuf_puts(&buf, ".section .debug_line,\"\",%progbits\n");
        asmbuf_puts(&buf, ".balign 4\n");
        asmbuf_puts(&buf, "    .word 0\n");              /* unit_length (placeholder) */
        asmbuf_puts(&buf, "    .half 2\n");              /* DWARF version 2 */
        asmbuf_puts(&buf, "    .word 0\n");              /* header_length */
        asmbuf_puts(&buf, "    .byte 4\n");              /* minimum_instruction_length */
        asmbuf_puts(&buf, "    .byte 0\n");              /* default_is_stmt */
        asmbuf_puts(&buf, "    .byte 1\n");              /* line_base */
        asmbuf_puts(&buf, "    .byte 1\n");              /* line_range */
        asmbuf_puts(&buf, "    .byte 13\n");             /* opcode_base */
        asmbuf_puts(&buf, "    .byte 0,1,1,0,1,0,0,1,1,1,1,1\n"); /* standard_opcode_lengths */
        asmbuf_puts(&buf, "    .asciz \"");
        asmbuf_puts(&buf, fname);
        asmbuf_puts(&buf, "\"\n");                       /* include_directories */
        asmbuf_puts(&buf, "    .byte 0\n");
        asmbuf_puts(&buf, "    .asciz \"");
        asmbuf_puts(&buf, fname);
        asmbuf_puts(&buf, "\"\n");                       /* file_names */
        asmbuf_puts(&buf, "    .byte 0,0,0\n");
        asmbuf_puts(&buf, "    .byte 0\n");              /* end file table */
        asmbuf_puts(&buf, "    .byte 0,9,2\n");          /* DW_LNE_end_sequence */
        asmbuf_puts(&buf, ".text\n");
    }
#endif
    return buf.buf;
}

/* === Minimal Thumb-2 text assembler === */
typedef struct AsmLabel { char *name; int addr; int is_weak; } AsmLabel;
typedef struct AsmRef   { int pc; char *name; int type; } AsmRef;
/* Literal pool entry: LDR rd,=label at ldr_pc needs a 4-byte literal at pool_offset */
typedef struct AsmLiteral { int ldr_pc; char *name; int pool_offset; } AsmLiteral;

typedef struct {
    const char    *src;
    unsigned char *out;
    int            pc;
    int            out_cap;
    AsmLabel      *labels;
    int            label_count, label_cap;
    AsmRef        *refs;
    int            ref_count, ref_cap;
    int            error_count;
    /* .set symbols (absolute addresses, e.g. __at variables, alias functions) */
    AsmLabel      *sets;
    int            set_count, set_cap;
    /* Literal pool for LDR rd,=label */
    /* Genuine assembly errors (unknown mnemonic, unresolvable internal label).
     * Kept separate from error_count, which also counts unresolved external
     * references that the linker may still satisfy. */
    int            hard_errors;
    AsmLiteral    *literals;
    int            literal_count, literal_cap;
    int            literal_pool_start;  /* pc where the pool begins (-1 = not yet) */
    /* .weak directive sets pending_weak; the next label defined inherits it. */
    int            pending_weak;
    /* Current section: 0=.text, 1=.data, 2=.bss, 3=.init_array, 4=.fini_array */
    int            cur_section;
} ArmAsm;

static int is_alpha_a(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
static int is_alnum_a(char c) {
    return is_alpha_a(c) || (c >= '0' && c <= '9');
}
static int is_space_a(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int my_toupper_a(int c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}
static int strieq_a(const char *a, const char *b) {
    while (*a && *b) {
        if (my_toupper_a(*a) != my_toupper_a(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}
static int starts_with_a(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}
static void skip_space_a(const char **p) { while (is_space_a(**p)) (*p)++; }
static void skip_to_eol_a(const char **p) { while (**p && **p != '\n') (*p)++; }

static int read_word_a(const char **p, char *out, int cap) {
    int n = 0;
    skip_space_a(p);
    if (**p == '.') {
        /* Directive (e.g. ".text", ".global", ".L1"): include '.' in word */
        while (**p && is_alnum_a(**p) && n < cap - 1) {
            out[n++] = **p; (*p)++;
        }
    } else {
        /* Instruction/label (e.g. "VADD", "add_float"): stop at '.'
         * so VFP suffixes like ".F32" are NOT consumed as part of the
         * mnemonic.  Without this, "VADD.F32" is read as one word and
         * strieq_a(first,"VADD") fails, causing all VFP arithmetic
         * instructions to fall through to NOP. */
        while (**p && is_alnum_a(**p) && **p != '.' && n < cap - 1) {
            out[n++] = **p; (*p)++;
        }
    }
    out[n] = 0;
    return n;
}

static int parse_reg_a(const char **p) {
    int n = 0;
    skip_space_a(p);
    if ((**p == 'r' || **p == 'R')) {
        (*p)++;
        while (**p >= '0' && **p <= '9') {
            n = n * 10 + (**p - '0');
            (*p)++;
        }
        return n;
    }
    if ((**p == 's' || **p == 'S') && ((*p)[1] == 'p' || (*p)[1] == 'P')) { (*p) += 2; return 13; }
    if ((**p == 'l' || **p == 'L') && ((*p)[1] == 'r' || (*p)[1] == 'R')) { (*p) += 2; return 14; }
    if ((**p == 'p' || **p == 'P') && ((*p)[1] == 'c' || (*p)[1] == 'C')) { (*p) += 2; return 15; }
    if ((**p == 'i' || **p == 'I') && ((*p)[1] == 'p' || (*p)[1] == 'P')) { (*p) += 2; return 12; }
    if ((**p == 'f' || **p == 'F') && ((*p)[1] == 'p' || (*p)[1] == 'P')) { (*p) += 2; return 11; }
    return -1;
}

/* #1 FIX: Parse VFP single-precision register (s0-s31).
 * Returns register number 0-31, or -1 if not an S register.
 * Must not match 'sp' (handled by parse_reg_a). */
static int parse_vfp_sreg_a(const char **p) {
    int n = 0;
    skip_space_a(p);
    if ((**p == 's' || **p == 'S') &&
        ((*p)[1] >= '0' && (*p)[1] <= '9')) {
        (*p)++;
        while (**p >= '0' && **p <= '9') {
            n = n * 10 + (**p - '0');
            (*p)++;
        }
        if (n > 31) n = 31;
        return n;
    }
    return -1;
}

/* #1 FIX: Parse VFP double-precision register (d0-d15).
 * Returns register number 0-15, or -1 if not a D register. */
static int parse_vfp_dreg_a(const char **p) {
    int n = 0;
    skip_space_a(p);
    if (**p == 'd' || **p == 'D') {
        (*p)++;
        while (**p >= '0' && **p <= '9') {
            n = n * 10 + (**p - '0');
            (*p)++;
        }
        if (n > 15) n = 15;
        return n;
    }
    return -1;
}

/* #1 FIX: Read VFP instruction suffix (e.g. ".F32", ".F64", ".S32.F32").
 * Concatenates all suffix segments into out (e.g. "F32", "S32F32").
 * Advances p past the suffix. */
static void read_vfp_suffix_a(const char **p, char *out, int cap) {
    int n = 0;
    skip_space_a(p);
    while (**p == '.' && n < cap - 1) {
        (*p)++;
        /* Inner loop: read alnum but NOT '.' (dots are handled by outer loop).
         * Without excluding '.', is_alnum_a('.')==true would consume the
         * entire ".F32.S32" as one segment "F32.S32" (with embedded dot),
         * causing strieq_a(suf,"F32S32") to fail. */
        while (is_alnum_a(**p) && **p != '.' && n < cap - 1) {
            out[n++] = **p;
            (*p)++;
        }
    }
    out[n] = 0;
}


/* FIX(vfp-encode): Split an S/D register number into the two fields the VFP
 * encoders actually use.
 *
 * The classic ARM ARM shorthand "Sd = D:Vd" reads as if a single-precision
 * register number were (16 * half_bit + 4-bit field); that is what this file
 * originally assumed.  Measured against armasm 5.06
 * (--cpu Cortex-M7 --fpu=FPv5_D16, see h7test/v3.lst / v4.lst) the field holds
 * reg[4:1] and the separate "half" bit holds reg[0]:
 *     VADD.F32 S0, S0, S16 -> 0xEE300A08   (field 8 = 16>>1, half 0 = 16&1)
 *     VADD.F32 S0, S0, S1  -> 0xEE300A20   (field 0 =  1>>1, half 1 =  1&1)
 *     VADD.F32 S1, S2, S3  -> 0xEE710A21   (every field shifted the same way)
 * D0-D15 put the whole number in the 4-bit field and leave the half bit clear.
 * The half bit sits at bit22 for Vd, bit7 for Vn and bit5 for Vm. */
static unsigned ark_vfp_nib(int reg, int is_dbl) {
    if (reg < 0) reg = 0;
    return is_dbl ? (unsigned)(reg & 0xF) : (unsigned)((reg >> 1) & 0xF);
}
static unsigned ark_vfp_half(int reg, int is_dbl) {
    if (reg < 0) reg = 0;
    return is_dbl ? (unsigned)((reg >> 4) & 1) : (unsigned)(reg & 1);
}
static int parse_imm_a(const char **p) {
    int neg = 0;
    int v = 0;
    skip_space_a(p);
    if (**p != '#') return 0;
    (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        (*p) += 2;
        for (;;) {
            char c = **p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d;
            (*p)++;
        }
    } else {
        while (**p >= '0' && **p <= '9') {
            v = v * 10 + (**p - '0');
            (*p)++;
        }
    }
    return neg ? -v : v;
}

/* Immediate operand where the '#' is optional (armasm accepts both
 * "SVC 0xAB" and "SVC #0xAB"). Returns 0 and clears *ok when no number is
 * present: parse_imm_a() would return 0 in that case as well, which turns a
 * mangled operand into a silent SVC #0. */
static int parse_imm_opt_hash_a(const char **p, int *ok) {
    int neg = 0;
    int v = 0;
    int digits = 0;
    skip_space_a(p);
    if (**p == '#') { (*p)++; skip_space_a(p); }
    if (**p == '-') { neg = 1; (*p)++; }
    if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        (*p) += 2;
        for (;;) {
            char c = **p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d;
            digits++;
            (*p)++;
        }
    } else {
        while (**p >= '0' && **p <= '9') {
            v = v * 10 + (**p - '0');
            digits++;
            (*p)++;
        }
    }
    *ok = (digits > 0);
    return neg ? -v : v;
}

static void eat_comma_a(const char **p) {
    skip_space_a(p);
    if (**p == ',') (*p)++;
    skip_space_a(p);
}

static void out16(ArmAsm *a, unsigned v) {
    if (a->pc + 2 > a->out_cap) { a->error_count++; return; }
    a->out[a->pc++] = (unsigned char)(v & 0xFF);
    a->out[a->pc++] = (unsigned char)((v >> 8) & 0xFF);
}

static void add_label_a(ArmAsm *a, const char *name, int addr) {
    if (a->label_count >= a->label_cap) {
        int nc = a->label_cap == 0 ? 32 : a->label_cap * 2;
        a->labels = (AsmLabel *)mem_realloc(a->labels,
            a->label_cap * (int)sizeof(AsmLabel),
            nc * (int)sizeof(AsmLabel));
        a->label_cap = nc;
    }
    a->labels[a->label_count].name = mem_strdup(name);
    a->labels[a->label_count].addr = addr;
    a->labels[a->label_count].is_weak = a->pending_weak;
    a->pending_weak = 0;
    a->label_count++;
}
static int find_label_a(ArmAsm *a, const char *name) {
    int i;
    for (i = 0; i < a->label_count; i++)
        if (ark_streq(a->labels[i].name, name)) return a->labels[i].addr;
    return -1;
}
static void add_ref_a(ArmAsm *a, int pc, const char *name, int type) {
    if (a->ref_count >= a->ref_cap) {
        int nc = a->ref_cap == 0 ? 32 : a->ref_cap * 2;
        a->refs = (AsmRef *)mem_realloc(a->refs,
            a->ref_cap * (int)sizeof(AsmRef),
            nc * (int)sizeof(AsmRef));
        a->ref_cap = nc;
    }
    a->refs[a->ref_count].pc = pc;
    a->refs[a->ref_count].name = mem_strdup(name);
    a->refs[a->ref_count].type = type;
    a->ref_count++;
}

/* .set symbol management (absolute addresses) */
static void add_set_a(ArmAsm *a, const char *name, int addr) {
    if (a->set_count >= a->set_cap) {
        int nc = a->set_cap == 0 ? 32 : a->set_cap * 2;
        a->sets = (AsmLabel *)mem_realloc(a->sets,
            a->set_cap * (int)sizeof(AsmLabel),
            nc * (int)sizeof(AsmLabel));
        a->set_cap = nc;
    }
    a->sets[a->set_count].name = mem_strdup(name);
    a->sets[a->set_count].addr = addr;
    a->set_count++;
}
static int find_set_a(ArmAsm *a, const char *name) {
    int i;
    for (i = 0; i < a->set_count; i++)
        if (ark_streq(a->sets[i].name, name)) return a->sets[i].addr;
    return -1;
}

/* Literal pool management */
static void add_literal_a(ArmAsm *a, int ldr_pc, const char *name) {
    if (a->literal_count >= a->literal_cap) {
        int nc = a->literal_cap == 0 ? 32 : a->literal_cap * 2;
        a->literals = (AsmLiteral *)mem_realloc(a->literals,
            a->literal_cap * (int)sizeof(AsmLiteral),
            nc * (int)sizeof(AsmLiteral));
        a->literal_cap = nc;
    }
    a->literals[a->literal_count].ldr_pc = ldr_pc;
    a->literals[a->literal_count].name = mem_strdup(name);
    a->literals[a->literal_count].pool_offset = -1;
    a->literal_count++;
}

/* Resolve a symbol: try .set first, then code labels */
static int resolve_symbol_a(ArmAsm *a, const char *name) {
    int addr = find_set_a(a, name);
    if (addr >= 0) return addr;
    addr = find_label_a(a, name);
    return addr;  /* may be -1 if unresolved */
}

/* Determine instruction size for LDR/STR/LDRB/STRB/LDRH/STRH/LDRSH/LDRSB
 * with immediate offset. Returns 2 (T1 16-bit) or 4 (T3 32-bit).
 * T1 limits: word imm5=(off>>2)&0x1F (max 124, step 4),
 *            byte imm5=off&0x1F (max 31),
 *            half imm5=(off>>1)&0x1F (max 62, step 2).
 * T3 supports imm12=off (max 4095 bytes). */
static int ldr_str_size_a(int imm, int is_byte, int is_half) {
    if (imm < 0) return 4;  /* negative offset always needs T3 */
    if (is_byte) {
        if (imm <= 31) return 2;
    } else if (is_half) {
        if (imm <= 62 && (imm % 2 == 0)) return 2;
    } else {
        if (imm <= 124 && (imm % 4 == 0)) return 2;
    }
    return 4;
}

static int op_size_a(const char *op) {
    if (strieq_a(op, "BL"))   return 4;
    if (strieq_a(op, "MOVW")) return 4;
    if (strieq_a(op, "MOVT")) return 4;
    if (strieq_a(op, "SDIV")) return 4;
    if (strieq_a(op, "UDIV")) return 4;
    if (strieq_a(op, "UMULL")) return 4;
    if (strieq_a(op, "B.W"))  return 4;
    /* Unconditional B uses 32-bit B.W (T4, +-1MB) to avoid +-2KB range
     * truncation in large functions (e.g. HAL_GPIO_Init is ~3.5KB;
     * 16-bit B T2 silently truncates imm11 causing the loop back-edge
     * to jump to a wrong address, breaking GPIO config). */
    if (strieq_a(op, "B"))    return 4;
    /* Conditional branches use 32-bit B<cond>.W (T3, +-1MB) to avoid
     * +-256B range truncation in large functions (e.g. HAL_RCC_OscConfig
     * is 8632 bytes; 16-bit B<cond> T1 silently truncates imm8 causing
     * branches to jump to wrong locations). T3 lo word bit12=0 (0x8000 base). */
    if (strieq_a(op, "BEQ") || strieq_a(op, "BNE") || strieq_a(op, "BCS") ||
        strieq_a(op, "BCC") || strieq_a(op, "BGE") || strieq_a(op, "BLT") ||
        strieq_a(op, "BGT") || strieq_a(op, "BLE") ||
        strieq_a(op, "BHI") || strieq_a(op, "BLS") ||
        strieq_a(op, "BLO") || strieq_a(op, "BHS")) return 4;
    /* Barriers DMB/DSB/ISB use 32-bit T1 encoding (0xF3BF 0x8F4x/5x/6x).
     * 16-bit T2 (0xBF50/60/70) is not standard ARMv7-M; use 32-bit form. */
    if (strieq_a(op, "DMB") || strieq_a(op, "DSB") || strieq_a(op, "ISB")) return 4;
    /* RRX is encoded as MOV Rd, Rm, RRX (32-bit T2 data-processing). */
    if (strieq_a(op, "RRX")) return 4;
    /* CLZ, RBIT and SADD16 only exist as 32-bit Thumb-2 encodings. */
    if (strieq_a(op, "CLZ"))    return 4;
    if (strieq_a(op, "RBIT"))   return 4;
    if (strieq_a(op, "SADD16")) return 4;
    /* #1 FIX: VFP instructions are all 32-bit (two 16-bit halfwords).
     * Without this, pass1 counts 2 bytes but pass2 emits 4, causing
     * all subsequent labels/branches to be off by 2 bytes per VFP instr. */
    if (strieq_a(op, "VMOV") || strieq_a(op, "VADD") || strieq_a(op, "VSUB") ||
        strieq_a(op, "VMUL") || strieq_a(op, "VDIV") || strieq_a(op, "VCVT") ||
        strieq_a(op, "VCMP") || strieq_a(op, "VMRS") || strieq_a(op, "VSTR") ||
        strieq_a(op, "VLDR") || strieq_a(op, "VNEG") || strieq_a(op, "VABS") ||
        strieq_a(op, "VSQRT")) return 4;
    return 2;
}

/* Forward declaration: needed in pass1 to decide whether PUSH/POP is T1 or T2 encoded */
static int parse_reglist_a(const char **p, int *has_lr, int *has_pc);

static void pass1_a(ArmAsm *a) {
    const char *p = a->src;
    /* The reserved vector-table area counts as part of the section, so labels
     * inside it (there are none) and after it keep the addresses the final
     * image will really have. */
    int pc = g_arm_vt_reserve ? ARM_VT_SIZE : 0;
    a->label_count = 0;
    a->set_count = 0;
    a->literal_count = 0;
    while (*p) {
        char first[64];
        int n;
        skip_space_a(&p);
        if (*p == '\n') { p++; continue; }
        if (*p == ';') { skip_to_eol_a(&p); continue; }
        if (*p == '\0') break;
        n = read_word_a(&p, first, (int)sizeof(first));
        if (n == 0) { skip_to_eol_a(&p); continue; }
        skip_space_a(&p);
        if (*p == ':') {
            add_label_a(a, first, pc);
            p++;
            skip_space_a(&p);
            if (*p == '\n' || *p == ';' || *p == '\0') { if (*p == ';') skip_to_eol_a(&p); if (*p == '\n') p++; continue; }
            n = read_word_a(&p, first, (int)sizeof(first));
            if (n == 0) { skip_to_eol_a(&p); continue; }
        }
        if (first[0] == '.') {
            /* Pseudo-directives: some affect pc, others are metadata-only */
            if (strieq_a(first, ".set") || strieq_a(first, ".equ")) {
                /* .set name, 0xADDR - record absolute address */
                char sym[64];
                skip_space_a(&p);
                n = read_word_a(&p, sym, (int)sizeof(sym));
                if (n > 0) {
                    skip_space_a(&p);
                    if (*p == ',') p++;
                    skip_space_a(&p);
                    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                        unsigned addr = 0;
                        p += 2;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            char c = *p;
                            int v = (c >= '0' && c <= '9') ? c - '0' :
                                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                            addr = addr * 16 + v;
                            p++;
                        }
                        add_set_a(a, sym, (int)addr);
                    } else {
                        /* .set name, other_label - resolve later, store as ref */
                        char tgt[64];
                        n = read_word_a(&p, tgt, (int)sizeof(tgt));
                        (void)n; (void)tgt;
                        /* For alias functions, defer to resolve_refs via BL lookup */
                    }
                }
            } else if (strieq_a(first, ".zero") || strieq_a(first, ".skip") || strieq_a(first, ".space")) {
                /* .zero N - reserve N bytes */
                int sz = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { sz = sz * 10 + (*p - '0'); p++; }
                pc += sz;
            } else if (strieq_a(first, ".word") || strieq_a(first, ".long")) {
                /* .word value - 4 bytes (may have multiple comma-separated values) */
                int count = 1;
                const char *scan = p;
                while (*scan && *scan != '\n') { if (*scan == ',') count++; scan++; }
                pc += 4 * count;
            } else if (strieq_a(first, ".byte")) {
                int count = 1;
                const char *scan = p;
                while (*scan && *scan != '\n') { if (*scan == ',') count++; scan++; }
                pc += count;
            } else if (strieq_a(first, ".asciz")) {
                /* .asciz "string" - string length + NUL, aligned to 2 */
                int slen = 0;
                skip_space_a(&p);
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"' && *p != '\n') {
                        if (*p == '\\') { slen++; p++; if (*p) p++; }
                        else { slen++; p++; }
                    }
                    if (*p == '"') p++;
                }
                slen++;  /* NUL terminator */
                if (slen & 1) slen++;  /* align to 2 */
                pc += slen;
            } else if (strieq_a(first, ".ascii")) {
                int slen = 0;
                skip_space_a(&p);
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"' && *p != '\n') {
                        if (*p == '\\') { slen++; p++; if (*p) p++; }
                        else { slen++; p++; }
                    }
                    if (*p == '"') p++;
                }
                if (slen & 1) slen++;
                pc += slen;
            } else if (strieq_a(first, ".align") || strieq_a(first, ".balign")) {
                /* Align to N bytes */
                int align = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { align = align * 10 + (*p - '0'); p++; }
                if (align > 1) {
                    int rem = pc % align;
                    if (rem != 0) pc += align - rem;
                }
            } else if (strieq_a(first, ".weak")) {
                /* .weak name - mark the next-defined label as weak.
                 * The symbol name argument is consumed but pending_weak
                 * applies to whichever label is defined next (the .weak
                 * directive always immediately precedes the label in our
                 * emission order: .global / .weak / label:). */
                char wname[64];
                skip_space_a(&p);
                n = read_word_a(&p, wname, (int)sizeof(wname));
                (void)n; (void)wname;
                a->pending_weak = 1;
            } else if (strieq_a(first, ".section")) {
                /* .section .text.foo, "ax", %progbits - track current section.
                 * Section tracking is metadata-only (pc unaffected); the
                 * assembler emits all code/data into a single linear buffer. */
                char sec[64];
                skip_space_a(&p);
                n = read_word_a(&p, sec, (int)sizeof(sec));
                if (n > 0) {
                    if (starts_with_a(sec, ".init_array")) a->cur_section = 3;
                    else if (starts_with_a(sec, ".fini_array")) a->cur_section = 4;
                    else if (starts_with_a(sec, ".data")) a->cur_section = 1;
                    else if (starts_with_a(sec, ".bss")) a->cur_section = 2;
                    else a->cur_section = 0;  /* .text or default */
                }
            } else if (strieq_a(first, ".text")) {
                a->cur_section = 0;
            } else if (strieq_a(first, ".data")) {
                a->cur_section = 1;
            } else if (strieq_a(first, ".bss")) {
                a->cur_section = 2;
            }
            /* .global, .type, .size, .thumb, .file, .loc, .cfi_*,
             * etc. are metadata-only and don't affect pc */
        } else if (strieq_a(first, "LDR") || strieq_a(first, "LDRB") ||
                   strieq_a(first, "STR") || strieq_a(first, "STRB") ||
                   strieq_a(first, "LDRH") || strieq_a(first, "STRH") ||
                   strieq_a(first, "LDRSH") || strieq_a(first, "LDRSB")) {
            /* LDR/STR size depends on offset: T1 (2B) for small offsets,
             * T3 (4B) for offsets beyond T1 imm5 range. Must match pass2. */
            int is_byte = (first[3] == 'B' || first[3] == 'b' ||
                           (first[0] == 'L' && first[2] == 'B') ||
                           (first[0] == 'S' && first[2] == 'B'));
            int is_half = (strieq_a(first, "LDRH") || strieq_a(first, "STRH") ||
                           strieq_a(first, "LDRSH") || strieq_a(first, "LDRSB"));
            int rd = parse_reg_a(&p);
            (void)rd;
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); }
            if (*p == '=') {
                /* LDR rd, =label - emit MOVW+MOVT (8 bytes) in pass2 */
                char name[64];
                p++;
                n = read_word_a(&p, name, (int)sizeof(name));
                (void)n;
                pc += 8;
            } else {
                /* Parse [Rn, #imm] to determine T1 vs T3 size */
                int imm = 0;
                if (*p == '[') { p++; skip_space_a(&p); }
                int rn = parse_reg_a(&p);
                (void)rn;
                skip_space_a(&p);
                if (*p == ',') {
                    p++; skip_space_a(&p);
                    if (*p == '#') imm = parse_imm_a(&p);
                }
                pc += ldr_str_size_a(imm, is_byte, is_half);
            }
        } else if (strieq_a(first, "PUSH") || strieq_a(first, "POP")) {
            /* PUSH/POP may be 16-bit T1 (r0-r7 + lr/pc only) or 32-bit T2
             * (includes r8-r12); size determined by the register list to keep pass1 offsets correct. */
            int has_lr = 0, has_pc = 0;
            int list = parse_reglist_a(&p, &has_lr, &has_pc);
            (void)has_lr; (void)has_pc;
            if (list & 0x1F00) pc += 4;  /* T2: includes r8-r12 */
            else               pc += 2;  /* T1 */
        } else if (strieq_a(first, "ADD") || strieq_a(first, "SUB")) {
            /* ADD/SUB rd, rn, #imm: T1 (2 bytes) if imm<=7 and low regs,
             * T3 (4 bytes) if imm>7 or high regs. Keep pass1 in sync with pass2. */
            int rd = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); }
            int rn = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); }
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd == 13 && rn == 13) pc += 2;       /* ADD/SUB sp, sp, #imm */
                else if (rd == rn) pc += 2;               /* ADD/SUB rd, rd, #imm8 */
                else if (imm <= 7 && rd <= 7 && rn <= 7) pc += 2;  /* T1 3-bit imm */
                else pc += 4;                              /* T3 32-bit */
            } else {
                pc += 2;  /* register variant, 16-bit */
            }
        } else if (strieq_a(first, "CMP")) {
            /* CMP Rn, #imm: T1 (2B) for imm 0-255, T2 (4B) for larger. */
            int rn = parse_reg_a(&p);
            (void)rn;
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); }
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (imm >= 0 && imm <= 255) pc += 2;
                else pc += 4;
            } else {
                (void)parse_reg_a(&p);
                pc += 2;
            }
        } else if (strieq_a(first, "MOVEQ") || strieq_a(first, "MOVNE") ||
                   strieq_a(first, "MOVLT") || strieq_a(first, "MOVGT") ||
                   strieq_a(first, "MOVLE") || strieq_a(first, "MOVGE") ||
                   strieq_a(first, "MOVLO") || strieq_a(first, "MOVHI") ||
                   strieq_a(first, "MOVLS") || strieq_a(first, "MOVHS") ||
                   strieq_a(first, "MOVCS") || strieq_a(first, "MOVCC")) {
            /* Conditional MOV in IT block: immediate form uses 32-bit MOV.W
             * (4 bytes, no flag modification); register form uses 16-bit (2 bytes). */
            int rd = parse_reg_a(&p);
            (void)rd;
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); }
            if (*p == '#') {
                (void)parse_imm_a(&p);
                pc += 4;  /* 32-bit MOV.W */
            } else {
                (void)parse_reg_a(&p);
                pc += 2;  /* 16-bit MOV */
            }
        } else if (strieq_a(first, "TST") ||
                   strieq_a(first, "SXTB") || strieq_a(first, "SXTH") ||
                   strieq_a(first, "UXTB") || strieq_a(first, "UXTH")) {
            /* Two-register forms: 16-bit T1 for r0-r7 operands, 32-bit T2
             * when either register is high (r8-r15). Must match pass2. */
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            {
                int rm = parse_reg_a(&p);
                if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) pc += 2;
                else                                          pc += 4;
            }
        } else if (strieq_a(first, "MOVS")) {
            /* MOVS Rd,#imm: 16-bit for rd in r0-r7 and imm 0..255.
             * MOVS Rd,Rm  : 16-bit for two low registers.
             * Everything else takes the 32-bit form. */
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255) pc += 2;
                else                                              pc += 4;
            } else {
                int rm = parse_reg_a(&p);
                if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) pc += 2;
                else                                          pc += 4;
            }
        } else if (strieq_a(first, "SVC")) {
            pc += 2;
        } else {
            pc += op_size_a(first);
        }
        skip_to_eol_a(&p);
        if (*p == '\n') p++;
    }
    /* LDR =label uses MOVW+MOVT (8 bytes), no literal pool needed */
    a->literal_pool_start = -1;
}

static int parse_reglist_a(const char **p, int *has_lr, int *has_pc) {
    int list = 0;
    *has_lr = 0; *has_pc = 0;
    skip_space_a(p);
    if (**p != '{') return 0;
    (*p)++;
    while (**p && **p != '}') {
        skip_space_a(p);
        if (**p == 'l' || **p == 'L') {
            if (((*p)[1] == 'r' || (*p)[1] == 'R')) { *has_lr = 1; (*p) += 2; }
            else (*p)++;  /* advance on unknown 'l?' to avoid infinite loop */
        } else if (**p == 'p' || **p == 'P') {
            if (((*p)[1] == 'c' || (*p)[1] == 'C')) { *has_pc = 1; (*p) += 2; }
            else (*p)++;  /* advance on unknown 'p?' to avoid infinite loop */
        } else {
            int r = parse_reg_a(p);
            if (r >= 0) list |= (1 << r);
            else if (**p && **p != '}' && **p != ',' && **p != '\n') (*p)++;  /* skip unknown char */
        }
        skip_space_a(p);
        if (**p == ',') (*p)++;
    }
    if (**p == '}') (*p)++;
    return list;
}

static void emit_bl_a(ArmAsm *a, int target) {
    int offset = target - a->pc - 4;
    int off2 = offset >> 1;
    int s = (off2 >> 23) & 1;
    int i1 = (off2 >> 22) & 1;
    int i2 = (off2 >> 21) & 1;
    int imm10 = (off2 >> 11) & 0x3FF;
    int imm11 = off2 & 0x7FF;
    int j1 = (~(i1 ^ s)) & 1;
    int j2 = (~(i2 ^ s)) & 1;
    unsigned hi = 0xF000u | (unsigned)(s << 10) | (unsigned)imm10;
    unsigned lo = 0xD000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
    out16(a, hi);
    out16(a, lo);
}

/* 32-bit unconditional B.W (T4 encoding) - range +-16MB.
 * hi: 0xF000 | (S<<10) | imm10
 * lo: 10 J1 1 J2 imm11  (bits[15:14]=10, bit12=1 for T4)
 *     BL T1 has bits[15:14]=11 (0xD000 base) - distinct from B.W
 * imm32 = SignExtend(S:I1:I2:imm10:imm11:0, 25)
 * NOTE: lo base MUST be 0x9000 (bit14=0, bit12=1). Using 0xD000
 * (bit14=1) encodes BL T1, which still branches but corrupts LR. */
static void emit_b_w_a(ArmAsm *a, int target) {
    int offset = target - a->pc - 4;
    int off2 = offset >> 1;
    int s = (off2 >> 23) & 1;
    int i1 = (off2 >> 22) & 1;
    int i2 = (off2 >> 21) & 1;
    int imm10 = (off2 >> 11) & 0x3FF;
    int imm11 = off2 & 0x7FF;
    int j1 = (~(i1 ^ s)) & 1;
    int j2 = (~(i2 ^ s)) & 1;
    unsigned hi = 0xF000u | (unsigned)(s << 10) | (unsigned)imm10;
    unsigned lo = 0x9000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
    out16(a, hi);
    out16(a, lo);
}

/* 32-bit conditional B<cond>.W (T3 encoding) - range +-1MB.
 * hi: 0xF000 | (S<<10) | (cond<<6) | imm6
 * lo: 1 0 J1 0 J2 imm11[10:0]  (bit15=1, bit14=0, bit12=0 for T3)
 *     T4 B.W has lo bit12=1 (0x9000 base) — distinguishes from T3.
 *     BL T1  has lo bit14=1,bit12=1 (0xD000 base) — also different.
 * imm32 = SignExtend(S:I1:I2:imm6:imm11, 20) << 1
 * NOTE: T3 and T4 share the same hw2 bit layout (J1 at bit13, J2 at bit11,
 *       imm11 at bits[10:0]), but DIFFERENT J1/J2 decoding:
 *       - T4/BL: I1 = NOT(J1 EOR S)  (inverted)
 *       - T3:    I1 = J1             (direct copy, NO inversion)
 *       Verified empirically via MDK armasm:
 *         MDK forward BEQ.W = 0xF000 0x8046 (J1=0, J2=0) for offset +0x8C.
 *         With T4 formula that would give offset 0x18008C (wrong).
 *         With T3 formula (I1=J1) it gives offset 0x8C (correct).
 *       Using the T4 formula for T3 causes forward conditional branches to
 *       jump ~786KB forward (outside flash) → HardFault. */
static void emit_bcond_w_a(ArmAsm *a, int cond, int target) {
    int offset = target - a->pc - 4;
    int off2 = offset >> 1;
    int s = (off2 >> 19) & 1;
    int i1 = (off2 >> 18) & 1;
    int i2 = (off2 >> 17) & 1;
    int imm6 = (off2 >> 11) & 0x3F;
    int imm11 = off2 & 0x7FF;
    int j1 = i1;
    int j2 = i2;
    unsigned hi = 0xF000u | (unsigned)(s << 10) | (unsigned)(cond << 6) | (unsigned)imm6;
    /* T3 lo word: bit15=1, bit14=0, bit13=J1, bit12=0, bit11=J2, bits[10:0]=imm11 */
    unsigned lo = 0x8000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
    out16(a, hi);
    out16(a, lo);
}

static int read_label_name_a(const char **p, char *buf, int cap) {
    int n = 0;
    skip_space_a(p);
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != ';' && **p != ',' && n < cap - 1) {
        buf[n++] = **p; (*p)++;
    }
    buf[n] = 0;
    return n;
}

/* Reverse of the Thumb-2 ThumbExpandImm() function: split a 32-bit
 * constant into the (i, imm3, imm8) fields of a modified immediate.
 *   rot = imm12[11:10]; value = ROR(bit7-replicated imm8, rot * 8)
 *   rot == 0        : value = ZeroExtend(imm8)
 * Returns 1 when the constant is representable, 0 otherwise. Used by the
 * MOVS/CMP.W immediate forms so an unrepresentable constant is reported
 * instead of being silently truncated to 8 bits. */
static int thumb_imm12_a(unsigned val, unsigned *out_i, unsigned *out_imm3,
                         unsigned *out_imm8) {
    unsigned rot;
    if (val <= 0xFFu) {
        *out_i = 0; *out_imm3 = 0; *out_imm8 = val;
        return 1;
    }
    for (rot = 1; rot < 4; rot++) {
        unsigned sh = rot * 8;
        unsigned unrot = (val << sh) | (val >> (32 - sh));   /* ROL by sh */
        unsigned imm8 = unrot & 0xFFu;
        unsigned cand;
        if ((imm8 & 0x80u) == 0) continue;   /* bit7 must be set here */
        cand = 0xFFFFFF00u | imm8;           /* '1':imm8 */
        if (((cand >> sh) | (cand << (32 - sh))) != val) continue;
        *out_i = (rot >> 1) & 1u;
        *out_imm3 = rot & 7u;
        *out_imm8 = imm8;
        return 1;
    }
    return 0;
}

static void pass2_a(ArmAsm *a) {
    const char *p = a->src;
    a->pc = g_arm_vt_reserve ? ARM_VT_SIZE : 0;
    a->ref_count = 0;
    while (*p) {
        char first[64];
        int n;
        skip_space_a(&p);
        if (*p == '\n') { p++; continue; }
        if (*p == ';') { skip_to_eol_a(&p); continue; }
        if (*p == '\0') break;
        n = read_word_a(&p, first, (int)sizeof(first));
        if (n == 0) { skip_to_eol_a(&p); continue; }
        skip_space_a(&p);
        if (*p == ':') {
            p++;
            skip_space_a(&p);
            if (*p == '\n' || *p == ';' || *p == '\0') {
                if (*p == ';') skip_to_eol_a(&p);
                if (*p == '\n') p++;
                continue;
            }
            n = read_word_a(&p, first, (int)sizeof(first));
            if (n == 0) { skip_to_eol_a(&p); continue; }
        }
        if (first[0] == '.') {
            /* Pseudo-directives: emit data for .zero/.word/.byte/.asciz, skip metadata */
            if (strieq_a(first, ".set") || strieq_a(first, ".equ")) {
                /* Already processed in pass1; skip */
            } else if (strieq_a(first, ".zero") || strieq_a(first, ".skip") || strieq_a(first, ".space")) {
                int sz = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { sz = sz * 10 + (*p - '0'); p++; }
                while (sz > 0 && a->pc < a->out_cap) { a->out[a->pc++] = 0; sz--; }
            } else if (strieq_a(first, ".word") || strieq_a(first, ".long")) {
                /* Output 4-byte values (comma-separated).
                 * Bug-A5 FIX: Also resolve symbol names (e.g. .word funcname
                 * for .init_array/.fini_array constructor/destructor entries)
                 * by looking them up in the label table. Previously only
                 * numeric literals were parsed, so .word funcname emitted
                 * 0x00000000, causing constructor iteration to jump to
                 * address 0 and HardFault. */
                while (*p && *p != '\n') {
                    int val = 0;
                    skip_space_a(&p);
                    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                        p += 2;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            char c = *p;
                            int v = (c >= '0' && c <= '9') ? c - '0' :
                                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                            val = val * 16 + v;
                            p++;
                        }
                    } else if (*p == '-') {
                        p++;
                        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                        val = -val;
                    } else if (*p >= '0' && *p <= '9') {
                        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                    } else {
                        /* Bug-A5: Token is a symbol name, not a number.
                         * Read the name and look it up in the label table. */
                        char sym_name[128];
                        int sn = 0;
                        while (*p && *p != ',' && *p != '\n' &&
                               *p != ' ' && *p != '\t' &&
                               sn < (int)sizeof(sym_name) - 1) {
                            sym_name[sn++] = *p++;
                        }
                        sym_name[sn] = 0;
                        if (sn > 0) {
                            int addr = find_label_a(a, sym_name);
                            if (addr < 0) addr = find_set_a(a, sym_name);
                            if (addr >= 0) {
                                val = addr;
                            } else {
                                /* Symbol not yet defined; record a 4-byte
                                 * absolute reference (type 4 = R_ARM_ABS32)
                                 * at the current pc for late resolution
                                 * by resolve_refs_a. */
                                add_ref_a(a, a->pc, sym_name, 4);
                            }
                        }
                    }
                    if (a->pc + 3 < a->out_cap) {
                        a->out[a->pc++] = (unsigned char)(val & 0xFF);
                        a->out[a->pc++] = (unsigned char)((val >> 8) & 0xFF);
                        a->out[a->pc++] = (unsigned char)((val >> 16) & 0xFF);
                        a->out[a->pc++] = (unsigned char)((val >> 24) & 0xFF);
                    }
                    skip_space_a(&p);
                    if (*p == ',') p++;
                }
            } else if (strieq_a(first, ".byte")) {
                while (*p && *p != '\n') {
                    int val = 0;
                    skip_space_a(&p);
                    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                        p += 2;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            char c = *p;
                            int v = (c >= '0' && c <= '9') ? c - '0' :
                                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                            val = val * 16 + v;
                            p++;
                        }
                    } else if (*p == '\'') {
                        p++;
                        val = *p;
                        p++;
                        if (*p == '\'') p++;
                    } else {
                        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                    }
                    if (a->pc < a->out_cap) a->out[a->pc++] = (unsigned char)(val & 0xFF);
                    skip_space_a(&p);
                    if (*p == ',') p++;
                }
            } else if (strieq_a(first, ".asciz") || strieq_a(first, ".ascii")) {
                int is_asciz = strieq_a(first, ".asciz");
                skip_space_a(&p);
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"' && *p != '\n') {
                        unsigned char c;
                        if (*p == '\\') {
                            p++;
                            switch (*p) {
                                case 'n': c = '\n'; break;
                                case 't': c = '\t'; break;
                                case 'r': c = '\r'; break;
                                case '\\': c = '\\'; break;
                                case '"': c = '"'; break;
                                case '0': c = '\0'; break;
                                case 'x': {
                                    int hv = 0;
                                    p++;
                                    if (*p && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
                                        char ch = *p;
                                        hv = (ch >= '0' && ch <= '9') ? ch - '0' :
                                             (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : ch - 'A' + 10;
                                        p++;
                                        if (*p && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
                                            ch = *p;
                                            hv = hv * 16 + ((ch >= '0' && ch <= '9') ? ch - '0' :
                                                 (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : ch - 'A' + 10);
                                            p++;
                                        }
                                    }
                                    c = (unsigned char)hv;
                                    p--;  /* compensate for p++ below */
                                    break;
                                }
                                default: c = (unsigned char)*p; break;
                            }
                            p++;
                        } else {
                            c = (unsigned char)*p;
                            p++;
                        }
                        if (a->pc < a->out_cap) a->out[a->pc++] = c;
                    }
                    if (*p == '"') p++;
                }
                if (is_asciz) {
                    if (a->pc < a->out_cap) a->out[a->pc++] = 0;
                    /* Align to 2 to match pass1 */
                    if (a->pc & 1) { if (a->pc < a->out_cap) a->out[a->pc++] = 0; }
                } else {
                    if (a->pc & 1) { if (a->pc < a->out_cap) a->out[a->pc++] = 0; }
                }
            } else if (strieq_a(first, ".align") || strieq_a(first, ".balign")) {
                int align = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { align = align * 10 + (*p - '0'); p++; }
                if (align > 1) {
                    int rem = a->pc % align;
                    if (rem != 0) {
                        int pad = align - rem;
                        while (pad > 0 && a->pc < a->out_cap) { a->out[a->pc++] = 0; pad--; }
                    }
                }
            }
            /* .section, .data, .bss, .text, .global, .type, .size, .thumb, .file, .loc,
             * .cfi_* etc. are metadata-only, skip */
            skip_to_eol_a(&p);
            if (*p == '\n') p++;
            continue;
        }
        skip_space_a(&p);

        if (strieq_a(first, "PUSH")) {
            int has_lr=0, has_pc=0;
            int list = parse_reglist_a(&p, &has_lr, &has_pc);
            (void)has_pc;
            if (list & 0x1F00) {
                /* T2 STMDB SP!: can encode r0-r12 + lr */
                unsigned w2 = (unsigned)(has_lr ? 0x4000 : 0) | (unsigned)(list & 0x1FFF);
                out16(a, 0xE92D); out16(a, w2);
            } else {
                out16(a, 0xB400 | (has_lr ? 0x0100 : 0) | (list & 0xFF));
            }
        }
        else if (strieq_a(first, "POP")) {
            int has_lr=0, has_pc=0;
            int list = parse_reglist_a(&p, &has_lr, &has_pc);
            (void)has_lr;
            if (list & 0x1F00) {
                /* T2 LDMIA SP!: can encode r0-r12 + pc */
                unsigned w2 = (unsigned)(has_pc ? 0x8000 : 0) | (unsigned)(list & 0x1FFF);
                out16(a, 0xE8BD); out16(a, w2);
            } else {
                out16(a, 0xBC00 | (has_pc ? 0x0100 : 0) | (list & 0xFF));
            }
        }
        else if (strieq_a(first, "MOV")) {
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                out16(a, 0x2000 | ((rd & 7) << 8) | (imm & 0xFF));
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x4600 | (((rd >> 3) & 1) << 7) | ((rm & 0xF) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "MOVEQ") || strieq_a(first, "MOVNE") ||
                 strieq_a(first, "MOVLT") || strieq_a(first, "MOVGT") ||
                 strieq_a(first, "MOVLE") || strieq_a(first, "MOVGE") ||
                 strieq_a(first, "MOVLO") || strieq_a(first, "MOVHI") ||
                 strieq_a(first, "MOVLS") || strieq_a(first, "MOVHS") ||
                 strieq_a(first, "MOVCS") || strieq_a(first, "MOVCC")) {
            /* Conditional MOV in IT block.
             * Immediate form: 32-bit MOV.W Rd, #imm8 (0xF04F base, no S bit,
             *   avoids UNPREDICTABLE MOVS-in-IT-block on ARMv7-M).
             * Register form: 16-bit MOV Rd, Rm (0x4600, no S bit, safe in IT). */
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                /* 32-bit T2 MOV.W: 1111 0 i 0 0010 0 S 1111 | 0 imm3 Rd imm8
                 * For imm 0-255: i=0, imm3=0, S=0, Rn=1111 (MOV alias) */
                unsigned hi = 0xF04Fu;
                unsigned lo = ((unsigned)(rd & 0xF) << 8) | (unsigned)(imm & 0xFF);
                out16(a, hi);
                out16(a, lo);
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x4600 | (((rd >> 3) & 1) << 7) | ((rm & 0xF) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "MOVW")) {
            int rd = parse_reg_a(&p);
            int imm;
            eat_comma_a(&p);
            imm = parse_imm_a(&p) & 0xFFFF;
            {
                unsigned ii = (imm >> 11) & 1;
                unsigned imm4 = (imm >> 12) & 0xF;
                unsigned imm3 = (imm >> 8) & 0x7;
                unsigned imm8 = imm & 0xFF;
                unsigned hi = 0xF240u | (ii << 10) | imm4;
                unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                out16(a, hi); out16(a, lo);
            }
        }
        else if (strieq_a(first, "MOVT")) {
            int rd = parse_reg_a(&p);
            int imm;
            eat_comma_a(&p);
            imm = parse_imm_a(&p) & 0xFFFF;
            {
                unsigned ii = (imm >> 11) & 1;
                unsigned imm4 = (imm >> 12) & 0xF;
                unsigned imm3 = (imm >> 8) & 0x7;
                unsigned imm8 = imm & 0xFF;
                unsigned hi = 0xF2C0u | (ii << 10) | imm4;
                unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                out16(a, hi); out16(a, lo);
            }
        }
        else if (strieq_a(first, "ADD")) {
            int rd = parse_reg_a(&p); int rn;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd == 13 && rn == 13) out16(a, 0xB000 | (((imm + 3) / 4) & 0x7F));
                else if (rd == rn) out16(a, 0x3000 | ((rd & 7) << 8) | (imm & 0xFF));
                else if (imm <= 7 && rd <= 7 && rn <= 7)
                    out16(a, 0x1C00 | ((imm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
                else {
                    /* T3: ADD.W Rd, Rn, #imm12
                     * Encoding: 11110 i 0 10000 Rn | 0 imm3 Rd imm8
                     * op=1000 (ADD), S=0 -> bits[8:4]=10000 -> first halfword base 0xF100
                     * BUGFIX: was 0xF040 (op=0010=ORR), caused ADD.W to decode as ORR.W */
                    int i = (imm >> 11) & 1;
                    int imm3 = (imm >> 8) & 7;
                    int imm8 = imm & 0xFF;
                    out16(a, 0xF100 | (i << 10) | (rn & 0xF));
                    out16(a, (imm3 << 12) | ((rd & 0xF) << 8) | imm8);
                }
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x1800 | ((rm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "SUB")) {
            int rd = parse_reg_a(&p); int rn;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd == 13 && rn == 13) out16(a, 0xB080 | (((imm + 3) / 4) & 0x7F));
                else if (rd == rn) out16(a, 0x3800 | ((rd & 7) << 8) | (imm & 0xFF));
                else if (imm <= 7 && rd <= 7 && rn <= 7)
                    out16(a, 0x1E00 | ((imm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
                else {
                    /* T3: SUB.W Rd, Rn, #imm12
                     * Encoding: 11110 i 0 1011 0 Rn | 0 imm3 Rd imm8
                     * op=1011 (SUB), S=0 -> bits[8:4]=10110 -> first halfword base 0xF160
                     * BUGFIX: was 0xF2C0 (plain-binary-immediate op=01100=MOVT),
                     * caused SUB.W to decode as MOVT */
                    int i = (imm >> 11) & 1;
                    int imm3 = (imm >> 8) & 7;
                    int imm8 = imm & 0xFF;
                    out16(a, 0xF160 | (i << 10) | (rn & 0xF));
                    out16(a, (imm3 << 12) | ((rd & 0xF) << 8) | imm8);
                }
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x1A00 | ((rm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "MUL")) {
            /* MULS T1 (16-bit): MULS <Rdn>, <Rn>, <Rdm> with Rd == Rn.
             * The two-operand form MULS Rd, Rm is the same encoding (Rn = Rd).
             * Three distinct registers need the 32-bit MUL T2 form, which this
             * encoder does not emit: reject it loudly instead of silently
             * dropping the third operand (MOD codegen emits "MUL r2, r2, r1"). */
            int rd = parse_reg_a(&p); int rn, rm, second;
            eat_comma_a(&p); second = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                rn = second;                  /* three-operand: Rd, Rn, Rm */
                eat_comma_a(&p); rm = parse_reg_a(&p);
            } else {
                rn = rd;                      /* two-operand: Rd = Rd * Rm */
                rm = second;
            }
            if (rd != rn) {
                {
                    char b1[12], b2[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "MUL with three distinct registers (Rd=r%s, Rn=r%s) "
                              "needs the 32-bit form, not supported",
                              diag_num(b1, rd), diag_num(b2, rn), 0,0,0,0);
                }
                a->hard_errors++;
            }
            out16(a, 0x4340 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "ADC")) {
            /* ADC T1 (16-bit): ADCS Rd, Rn (always sets flags in 16-bit Thumb) */
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4140 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "SBC")) {
            /* SBC T1 (16-bit): SBCS Rd, Rn (always sets flags in 16-bit Thumb) */
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4180 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "UMULL")) {
            /* UMULL T1 (32-bit): UMULL RdLo, RdHi, Rn, Rm
             * Encoding: 1111 1011 1010 Rn | RdLo RdHi 0000 Rm */
            int rdlo = parse_reg_a(&p); int rdhi, rn, rm;
            eat_comma_a(&p); rdhi = parse_reg_a(&p);
            eat_comma_a(&p); rn = parse_reg_a(&p);
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0xFBA0 | (rn & 0xF));
            out16(a, ((rdlo & 0xF) << 12) | ((rdhi & 0xF) << 8) | (rm & 0xF));
        }
        else if (strieq_a(first, "SDIV") || strieq_a(first, "UDIV")) {
            int rd = parse_reg_a(&p);
            int rn, rm;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            {
                unsigned op = strieq_a(first, "SDIV") ? 0xFB90u : 0xFBB0u;
                out16(a, op | (rn & 0xF));
                out16(a, 0xF0F0u | ((rd & 0xF) << 8) | (rm & 0xF));
            }
        }
        else if (strieq_a(first, "AND")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4000 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "ORR")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4300 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "EOR")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4040 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "LSL")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                /* LSL rd, rm, #imm5 (T1 immediate shift) */
                p++; skip_space_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p) & 0x1F;
                    out16(a, (unsigned)(imm << 6) | ((rm & 7) << 3) | (rd & 7));
                } else {
                    int rm2 = parse_reg_a(&p);
                    out16(a, 0x4080 | ((rm2 & 7) << 3) | (rd & 7));
                }
            } else {
                /* LSL rd, rm (T1 register shift) */
                out16(a, 0x4080 | ((rm & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "LSR")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                /* LSR rd, rm, #imm5 (T1 immediate shift; imm5=0 means 32) */
                p++; skip_space_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    int imm5 = (imm >= 32) ? 0 : (imm & 0x1F);
                    out16(a, 0x0800 | (unsigned)(imm5 << 6) | ((rm & 7) << 3) | (rd & 7));
                } else {
                    int rm2 = parse_reg_a(&p);
                    out16(a, 0x40C0 | ((rm2 & 7) << 3) | (rd & 7));
                }
            } else {
                /* LSR rd, rm (T1 register shift) */
                out16(a, 0x40C0 | ((rm & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "ASR")) {
            /* ASR T1: ASRS Rd, Rm, #imm5 (immediate) or ASRS Rd, Rs (register) */
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                /* ASR rd, rm, #imm5 (T1 immediate shift; imm5=0 means 32) */
                p++; skip_space_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    int imm5 = (imm >= 32) ? 0 : (imm & 0x1F);
                    out16(a, 0x1000 | (unsigned)(imm5 << 6) | ((rm & 7) << 3) | (rd & 7));
                } else {
                    int rm2 = parse_reg_a(&p);
                    out16(a, 0x4100 | ((rm2 & 7) << 3) | (rd & 7));
                }
            } else {
                /* ASR rd, rm (T1 register shift) */
                out16(a, 0x4100 | ((rm & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "BIC")) {
            /* BIC rd, rm or BIC rd, rn, rm (T1: rd = rd AND NOT rm) */
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); rm = parse_reg_a(&p); }
            out16(a, 0x4380 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "MVN")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x43C0 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "NEG") || strieq_a(first, "RSB")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4240 | ((rm & 7) << 3) | (rd & 7));
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); if (*p == '#') parse_imm_a(&p); }
        }
        else if (strieq_a(first, "CMP")) {
            int rn = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (imm >= 0 && imm <= 255) {
                    /* T1 16-bit: CMP Rn, #imm8 */
                    out16(a, 0x2800 | ((rn & 7) << 8) | (imm & 0xFF));
                } else {
                    /* T2 32-bit: CMP.W Rn, #imm12 (ThumbExpandImm).
                     * hi: 0xF1B0 | i<<10 | Rn, lo: imm3<<12 | 0x0F00 | imm8.
                     * Was truncating the constant to its low 8 bits, which
                     * silently compared against the wrong value. */
                    unsigned i, imm3, imm8;
                    if (!thumb_imm12_a((unsigned)imm, &i, &imm3, &imm8)) {
                        {
                            char b1[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "CMP immediate %s cannot be encoded as a "
                                      "modified immediate", diag_num(b1, imm),
                                      0,0,0,0,0);
                        }
                        a->hard_errors++;
                        out16(a, 0xBF00);
                        out16(a, 0xBF00);
                    } else {
                        out16(a, 0xF1B0u | ((i & 1) << 10) | (unsigned)(rn & 0xF));
                        out16(a, ((imm3 & 7) << 12) | 0x0F00u | imm8);
                    }
                }
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x4280 | ((rm & 7) << 3) | (rn & 7));
            }
        }
        else if (strieq_a(first, "LDR") || strieq_a(first, "LDRB") ||
                 strieq_a(first, "STR") || strieq_a(first, "STRB")) {
            int is_load = (first[0] == 'L' || first[0] == 'l');
            int is_byte = (first[2] == 'B' || first[2] == 'b' || first[3] == 'B' || first[3] == 'b');
            int rd = parse_reg_a(&p);
            int rn;
            int imm = 0;
            eat_comma_a(&p);
            if (*p == '=') {
                /* LDR rd, =label - emit MOVW+MOVT to load absolute address.
                 * .set symbols (e.g. __at variables) use their absolute address directly;
                 * code/data labels (e.g. .L_str_N) use g_flash_base + offset.
                 * g_flash_base is the first LR load address when --scatter is
                 * given, otherwise the default 0x08000000 (STM32F4 Flash). */
                char sym[64];
                int addr;
                int sn;
                p++;  /* skip '=' */
                sn = read_word_a(&p, sym, (int)sizeof(sym));
                (void)sn;
                addr = find_set_a(a, sym);
                if (addr >= 0) {
                    /* .set symbol - absolute address already (e.g. 0x40020800, 0x20000000) */
                } else {
                    addr = find_label_a(a, sym);
                    if (addr < 0) {
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "unresolved symbol %sq in LDR =label", sym,
                                  0,0,0,0,0);
                        a->error_count++;
                        addr = 0;
                    } else {
                        /* code/data label - add Flash base address (scatter-aware) */
                        addr = (int)g_flash_base + addr;
                    }
                }
                /* Emit MOVW rd, #low16 */
                {
                    unsigned imm = (unsigned)(addr & 0xFFFF);
                    unsigned ii = (imm >> 11) & 1;
                    unsigned imm4 = (imm >> 12) & 0xF;
                    unsigned imm3 = (imm >> 8) & 0x7;
                    unsigned imm8 = imm & 0xFF;
                    unsigned hi = 0xF240u | (ii << 10) | imm4;
                    unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                    out16(a, hi); out16(a, lo);
                }
                /* Emit MOVT rd, #high16 */
                {
                    unsigned imm = (unsigned)((addr >> 16) & 0xFFFF);
                    unsigned ii = (imm >> 11) & 1;
                    unsigned imm4 = (imm >> 12) & 0xF;
                    unsigned imm3 = (imm >> 8) & 0x7;
                    unsigned imm8 = imm & 0xFF;
                    unsigned hi = 0xF2C0u | (ii << 10) | imm4;
                    unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                    out16(a, hi); out16(a, lo);
                }
                skip_to_eol_a(&p);
                if (*p == '\n') p++;
                continue;
            }
            if (*p == '[') { p++; skip_space_a(&p); }
            rn = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                p++; skip_space_a(&p);
                if (*p == '#') imm = parse_imm_a(&p);
            }
            skip_space_a(&p);
            if (*p == ']') p++;
            {
                if (ldr_str_size_a(imm, is_byte, 0) == 4) {
                    /* T3 32-bit encoding: LDR.W/STR.W/LDRB.W/STRB.W Rt, [Rn, #imm12] */
                    unsigned hi, lo;
                    unsigned imm12 = (unsigned)(imm & 0xFFF);
                    if (is_load && !is_byte)      hi = 0xF8D0u | (unsigned)(rn & 0xF);  /* LDR.W */
                    else if (is_load && is_byte)  hi = 0xF890u | (unsigned)(rn & 0xF);  /* LDRB.W */
                    else if (!is_load && !is_byte)hi = 0xF8C0u | (unsigned)(rn & 0xF);  /* STR.W */
                    else                          hi = 0xF880u | (unsigned)(rn & 0xF);  /* STRB.W */
                    lo = ((unsigned)(rd & 0xF) << 12) | imm12;
                    out16(a, hi);
                    out16(a, lo);
                } else {
                    /* T1 16-bit encoding */
                    unsigned imm5;
                    if (is_byte) imm5 = imm & 0x1F;
                    else         imm5 = (unsigned)((imm >> 2) & 0x1F);
                    if (is_load) {
                        if (is_byte) out16(a, 0x7800 | (imm5 << 6) | ((rn & 7) << 3) | (rd & 7));
                        else         out16(a, 0x6800 | (imm5 << 6) | ((rn & 7) << 3) | (rd & 7));
                    } else {
                        if (is_byte) out16(a, 0x7000 | (imm5 << 6) | ((rn & 7) << 3) | (rd & 7));
                        else         out16(a, 0x6000 | (imm5 << 6) | ((rn & 7) << 3) | (rd & 7));
                    }
                }
            }
        }
        else if (strieq_a(first, "BX")) {
            int rm = parse_reg_a(&p);
            out16(a, 0x4700 | ((rm & 0xF) << 3));
        }
        else if (strieq_a(first, "NOP")) {
            out16(a, 0xBF00);
        }
        else if (strieq_a(first, "IT") || strieq_a(first, "ITT") ||
                 strieq_a(first, "ITE") || strieq_a(first, "ITTT")) {
            /* IT block: encoding 0xBF{firstcond}{mask}.
             * firstcond: EQ=0,NE=1,CS/HS=2,CC/LO=3,MI=4,PL=5,VS=6,VC=7,
             *            HI=8,LS=9,GE=10,LT=11,GT=12,LE=13,AL=14
             *
             * mask[3:0] encodes both the IT block length (position of the
             * least-significant 1 bit) and per-instruction condition inversion:
             *   - For instruction i (1-indexed), if mask bit == firstcond[0],
             *     the instruction uses firstcond (Then); otherwise it uses
             *     NOT(firstcond) (Else).
             *
             * Correct masks (cb = firstcond[0]):
             *   IT   (1 instr):              1000 = 0x8
             *   ITT  (2 instrs, Then-Then):  cb 1 0 0  -> (cb<<3)|0x4
             *   ITE  (2 instrs, Then-Else): ~cb 1 0 0  -> ((~cb&1)<<3)|0x4
             *   ITTT (3 instrs, T-T-T):      cb cb 1 0 -> (cb<<3)|(cb<<2)|0x2
             *
             * BUGFIX #1: previously emitted 0xBF00 (NOP), making conditional
             *   MOVs unconditional (delay loops never exited).
             * BUGFIX #2: previously used fixed masks (ITT=0xC, ITE=0x4) which
             *   are only correct when firstcond[0]==1 (NE,LO,PL,VC,LS,LT,LE).
             *   For firstcond[0]==0 (EQ,HS,MI,VS,HI,GE,GT,AL) the conditions
             *   were silently inverted. E.g. ITE HS emitted 0xBF24 (acts as
             *   ITE LO HS) instead of 0xBF2C, flipping comparison results
             *   and making SysTick 500ms blink toggle at 1ms (invisible). */
            char cond[8];
            int cv = 14;  /* default AL */
            int cb;       /* firstcond[0] */
            int mask;
            skip_space_a(&p);
            read_word_a(&p, cond, sizeof(cond));
            if      (strieq_a(cond, "EQ")) cv = 0;
            else if (strieq_a(cond, "NE")) cv = 1;
            else if (strieq_a(cond, "HS") || strieq_a(cond, "CS")) cv = 2;
            else if (strieq_a(cond, "LO") || strieq_a(cond, "CC")) cv = 3;
            else if (strieq_a(cond, "MI")) cv = 4;
            else if (strieq_a(cond, "PL")) cv = 5;
            else if (strieq_a(cond, "VS")) cv = 6;
            else if (strieq_a(cond, "VC")) cv = 7;
            else if (strieq_a(cond, "HI")) cv = 8;
            else if (strieq_a(cond, "LS")) cv = 9;
            else if (strieq_a(cond, "GE")) cv = 10;
            else if (strieq_a(cond, "LT")) cv = 11;
            else if (strieq_a(cond, "GT")) cv = 12;
            else if (strieq_a(cond, "LE")) cv = 13;
            cb = cv & 1;
            if      (strieq_a(first, "IT"))   mask = 0x8;
            else if (strieq_a(first, "ITT"))  mask = (cb << 3) | 0x4;
            else if (strieq_a(first, "ITE"))  mask = ((cb ^ 1) << 3) | 0x4;
            else                              mask = (cb << 3) | (cb << 2) | 0x2; /* ITTT */
            out16(a, 0xBF00 | (cv << 4) | mask);
        }
        else if (strieq_a(first, "BLX")) {
            /* BLX <Rm>: 16-bit 0x4780 | Rm<<3 (Thumb-2 T1). The backend emits
             * this for an indirect call (obj->fnptr()), so without it every
             * function-pointer call became an "unknown mnemonic" and the whole
             * image failed to assemble. Only the register form exists here:
             * the label form is 32-bit and is never emitted by this backend,
             * so a non-register operand is reported rather than mistranslated. */
            int rm = parse_reg_a(&p);
            if (rm < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "BLX needs a register operand", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
            } else {
                out16(a, 0x4780 | ((rm & 0xF) << 3));
            }
        }
        else if (strieq_a(first, "BL")) {
            char name[64];
            read_label_name_a(&p, name, sizeof(name));
            {
                int addr = find_label_a(a, name);
                if (addr < 0) {
                    add_ref_a(a, a->pc, name, 1);
                    out16(a, 0xF000);
                    out16(a, 0xD000);
                } else {
                    emit_bl_a(a, addr);
                }
            }
        }
        else if (strieq_a(first, "B") || strieq_a(first, "BEQ") ||
                 strieq_a(first, "BNE") || strieq_a(first, "BLT") ||
                 strieq_a(first, "BGT") || strieq_a(first, "BLE") ||
                 strieq_a(first, "BGE") || strieq_a(first, "BCC") ||
                 strieq_a(first, "BCS") || strieq_a(first, "BHI") ||
                 strieq_a(first, "BLS") || strieq_a(first, "BLO") ||
                 strieq_a(first, "BHS")) {
            char name[64];
            int cond = -1;
            int is_uncond = (first[1] == 0);
            if (!is_uncond) {
                if (strieq_a(first, "BEQ")) cond = 0;
                else if (strieq_a(first, "BNE")) cond = 1;
                else if (strieq_a(first, "BCS")) cond = 2;
                else if (strieq_a(first, "BCC")) cond = 3;
                else if (strieq_a(first, "BGE")) cond = 10;
                else if (strieq_a(first, "BLT")) cond = 11;
                else if (strieq_a(first, "BGT")) cond = 12;
                else if (strieq_a(first, "BLE")) cond = 13;
                /* Unsigned comparisons: HI=8, LS=9, LO(CC)=3, HS(CS)=2.
                 * Verified: BHI = 0xD8xx, BLS = 0xD9xx, BLO = 0xD3xx,
                 * BHS = 0xD2xx in the 16-bit T1 form. */
                else if (strieq_a(first, "BHI")) cond = 8;
                else if (strieq_a(first, "BLS")) cond = 9;
                else if (strieq_a(first, "BLO")) cond = 3;
                else if (strieq_a(first, "BHS")) cond = 2;
            }
            read_label_name_a(&p, name, sizeof(name));
            {
                int addr = find_label_a(a, name);
                if (addr < 0) {
                    /* Forward reference: emit placeholder, resolve later.
                     * type=2: 32-bit B.W (T4, +-1MB), type=3: 32-bit B<cond>.W (T3, +-1MB).
                     * For type=3, cond is encoded in placeholder hi word
                     * (bits[9:6]) so resolve_refs_a can recover it. */
                    if (is_uncond) {
                        /* 32-bit B.W (T4) forward ref: type=2, 4-byte placeholder.
                         * resolve_refs_a will call emit_b_w_a to fill in the encoding. */
                        add_ref_a(a, a->pc, name, 2);
                        out16(a, 0xF000);
                        out16(a, 0x9000);
                    } else {
                        /* 32-bit B<cond>.W (T3) forward ref: type=3, 4-byte placeholder.
                         * cond encoded in hi word bits[9:6] for resolve_refs_a. */
                        add_ref_a(a, a->pc, name, 3);
                        out16(a, 0xF000u | (unsigned)(cond << 6));
                        out16(a, 0x8000);
                    }
                } else {
                    /* Backward reference: target known, emit directly */
                    if (is_uncond) {
                        /* 32-bit B.W (T4): +-1MB range, no truncation */
                        emit_b_w_a(a, addr);
                    } else {
                        /* 32-bit B<cond>.W (T3): +-1MB range, no truncation */
                        emit_bcond_w_a(a, cond, addr);
                    }
                }
            }
        }
        else if (strieq_a(first, "STRH") || strieq_a(first, "LDRH") ||
                 strieq_a(first, "LDRSH") || strieq_a(first, "LDRSB")) {
            /* Halfword load/store: T1 (16-bit) for small offsets, T3 (32-bit) for large.
             * T1: imm5=(offset>>1)&0x1F, max 62 bytes (even).
             * T3: imm12=offset, max 4095 bytes.
             *   STRH.W:  0xF8A0|Rn,  LDRH.W:  0xF8B0|Rn
             *   LDRSH.W: 0xF9B0|Rn,  LDRSB.W: 0xF990|Rn */
            int rd = parse_reg_a(&p);
            int rn;
            int imm = 0;
            eat_comma_a(&p);
            if (*p == '[') { p++; skip_space_a(&p); }
            rn = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                p++; skip_space_a(&p);
                if (*p == '#') imm = parse_imm_a(&p);
            }
            skip_space_a(&p);
            if (*p == ']') p++;
            if (ldr_str_size_a(imm, 0, 1) == 4) {
                /* T3 32-bit encoding */
                unsigned hi, lo;
                unsigned imm12 = (unsigned)(imm & 0xFFF);
                if      (strieq_a(first, "STRH"))  hi = 0xF8A0u | (unsigned)(rn & 0xF);
                else if (strieq_a(first, "LDRH"))  hi = 0xF8B0u | (unsigned)(rn & 0xF);
                else if (strieq_a(first, "LDRSH")) hi = 0xF9B0u | (unsigned)(rn & 0xF);
                else                               hi = 0xF990u | (unsigned)(rn & 0xF);  /* LDRSB */
                lo = ((unsigned)(rd & 0xF) << 12) | imm12;
                out16(a, hi);
                out16(a, lo);
            } else {
                /* T1 16-bit encoding */
                unsigned imm5 = (unsigned)((imm >> 1) & 0x1F);
                unsigned base;
                if      (strieq_a(first, "STRH"))  base = 0x8000;
                else if (strieq_a(first, "LDRH"))  base = 0x8800;
                else if (strieq_a(first, "LDRSH")) base = 0x9800;
                else                               base = 0x9000;  /* LDRSB */
                out16(a, base | (imm5 << 6) | ((rn & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "CPSID") || strieq_a(first, "CPSIE")) {
            /* CPS T1 16-bit: bit4=change(1=ID,0=IE), bit1=affect_i, bit0=affect_f
             *   CPSID i = 0xB672, CPSIE i = 0xB662, CPSID f = 0xB671, CPSIE f = 0xB661
             * Note: base 0xB660 already has bit5=1; change is bit4 (0x10), NOT bit5.
             * Previously NOP'd, so Error_Handler's CPSID i failed to mask
             * interrupts and HAL_Init's CPSIE i failed to enable them. */
            int is_id = strieq_a(first, "CPSID");
            int affect_i = 0, affect_f = 0;
            skip_space_a(&p);
            while (*p && *p != '\n' && *p != ';') {
                if (*p == 'i' || *p == 'I') affect_i = 1;
                else if (*p == 'f' || *p == 'F') affect_f = 1;
                p++;
            }
            out16(a, 0xB660 | (is_id ? 0x10 : 0) | (affect_i ? 0x2 : 0) | (affect_f ? 0x1 : 0));
        }
        else if (strieq_a(first, "DMB") || strieq_a(first, "DSB") || strieq_a(first, "ISB")) {
            /* T1 32-bit barriers: 0xF3BF 0x8F4x/5x/6x, option=0xF for SY.
             *   DMB SY = F3BF 8F5F, DSB SY = F3BF 8F4F, ISB SY = F3BF 8F6F.
             * Pass1 op_size_a returns 4 for these. Previously NOP'd (2 bytes),
             * causing pass1/pass2 size mismatch and silent instruction drops. */
            unsigned lo_base;
            if      (strieq_a(first, "DMB")) lo_base = 0x8F50;
            else if (strieq_a(first, "DSB")) lo_base = 0x8F40;
            else                             lo_base = 0x8F60;  /* ISB */
            out16(a, 0xF3BF);
            out16(a, lo_base | 0xF);
        }
        else if (strieq_a(first, "SEV") || strieq_a(first, "WFE") || strieq_a(first, "WFI")) {
            /* 16-bit hint instructions (no operands).
             *   SEV = 0xBF40, WFE = 0xBF20, WFI = 0xBF30.
             * Used by HAL_PWR_EnterSLEEPMode/STOPMode/STANDBYMode. */
            if      (strieq_a(first, "SEV")) out16(a, 0xBF40);
            else if (strieq_a(first, "WFE")) out16(a, 0xBF20);
            else                             out16(a, 0xBF30);  /* WFI */
        }
        else if (strieq_a(first, "REV") || strieq_a(first, "REV16") || strieq_a(first, "REVSH")) {
            /* T1 16-bit: 1011 1010 op Rm Rd, where op=00(REV),01(REV16),11(REVSH).
             *   REV=0xBA00, REV16=0xBA40, REVSH=0xBAC0. */
            int rd = parse_reg_a(&p);
            int rm;
            unsigned base;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if      (strieq_a(first, "REV"))   base = 0xBA00;
            else if (strieq_a(first, "REV16")) base = 0xBA40;
            else                               base = 0xBAC0;  /* REVSH */
            out16(a, base | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "RRX")) {
            /* 32-bit: MOV Rd, Rm, RRX = 0xEA4F (Rd<<12) 0x30 Rm.
             * Pass1 op_size_a returns 4. */
            int rd = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            out16(a, 0xEA4F);
            out16(a, (unsigned)((rd & 0xF) << 12) | 0x30 | (rm & 0xF));
        }
        /* ===== #1 FIX: VFP floating-point instruction encoders =====
         * Previously ALL VFP instructions fell through to NOP (0xBF00),
         * making every float operation a no-op. Now each is properly
         * encoded as a 32-bit Thumb-2 VFP instruction.
         *
         * VFP register encoding (S<n>, n=0..31 ; D<n>, n=0..15):
         *   the 4-bit register field holds n[4:1] for single precision and
         *   the whole n for double precision; the neighbouring "half" bit
         *   holds n[0] for single precision and is 0 for double.
         *     Vd = field at bits15:12, half at bit22
         *     Vn = field at bits19:16, half at bit7
         *     Vm = field at bits3:0,   half at bit5
         *   The split is done by ark_vfp_nib()/ark_vfp_half(); the bit
         *   positions above are the ones those helpers feed into.
         * sz bit (bit8): 0=F32 single, 1=F64 double. */
        else if (strieq_a(first, "VMOV")) {
            /* VMOV Sn, Rt (core -> VFP) or VMOV Rt, Sn (VFP -> core) */
            skip_space_a(&p);
            if (*p == 's' || *p == 'S') {
                /* VMOV Sn, Rt — core register to VFP single register */
                int sn = parse_vfp_sreg_a(&p);
                int rt;
                unsigned vn, n_bit, hi, lo;
                if (sn < 0) sn = 0;
                eat_comma_a(&p);
                rt = parse_reg_a(&p);
                vn = ark_vfp_nib(sn, 0);
                n_bit = ark_vfp_half(sn, 0);
                hi = 0xEE00u | vn;
                lo = 0x0A10u | (unsigned)((rt & 0xF) << 12) | (n_bit << 7);
                out16(a, hi);
                out16(a, lo);
            } else {
                /* VMOV Rt, Sn — VFP single register to core register */
                int rt = parse_reg_a(&p);
                int sn;
                unsigned vn, n_bit, hi, lo;
                eat_comma_a(&p);
                sn = parse_vfp_sreg_a(&p);
                if (sn < 0) sn = 0;
                vn = ark_vfp_nib(sn, 0);
                n_bit = ark_vfp_half(sn, 0);
                hi = 0xEE10u | vn;   /* bit20=1 means VFP->core */
                lo = 0x0A10u | (unsigned)((rt & 0xF) << 12) | (n_bit << 7);
                out16(a, hi);
                out16(a, lo);
            }
        }
        else if (strieq_a(first, "VADD") || strieq_a(first, "VSUB") ||
                 strieq_a(first, "VMUL") || strieq_a(first, "VDIV")) {
            /* VOP.F32 Sd,Sn,Sm  or  VOP.F64 Dd,Dn,Dm */
            char suf[16];
            int is_f64;
            unsigned base;
            int sd, sn, sm;
            unsigned d, vd, n_bit, vn, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            is_f64 = strieq_a(suf, "F64");
            if      (strieq_a(first, "VADD")) base = is_f64 ? 0xEE300B00u : 0xEE300A00u;
            else if (strieq_a(first, "VSUB")) base = is_f64 ? 0xEE300B40u : 0xEE300A40u;
            else if (strieq_a(first, "VMUL")) base = is_f64 ? 0xEE200B00u : 0xEE200A00u;
            /* FIX(vfp-encode): VDIV was 0xEE000A40/0xEE000B40 = VMLS, so every float division lost its division. */
            else                              base = is_f64 ? 0xEE800B00u : 0xEE800A00u;
            if (is_f64) {
                sd = parse_vfp_dreg_a(&p); eat_comma_a(&p);
                sn = parse_vfp_dreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_dreg_a(&p);
            } else {
                sd = parse_vfp_sreg_a(&p); eat_comma_a(&p);
                sn = parse_vfp_sreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_sreg_a(&p);
            }
            if (sd < 0) sd = 0; if (sn < 0) sn = 0; if (sm < 0) sm = 0;
            d     = ark_vfp_half(sd, is_f64);   /* bit22   = Sd[0] */
            vd    = ark_vfp_nib (sd, is_f64);   /* bits15:12 = Sd[4:1] */
            n_bit = ark_vfp_half(sn, is_f64);   /* bit7    = Sn[0] */
            vn    = ark_vfp_nib (sn, is_f64);   /* bits19:16 = Sn[4:1] */
            m     = ark_vfp_half(sm, is_f64);   /* bit5    = Sm[0] */
            vm    = ark_vfp_nib (sm, is_f64);   /* bits3:0 = Sm[4:1] */
            hi = (base >> 16) | (d << 6) | vn;
            lo = (base & 0xFFFF) | (vd << 12) | (n_bit << 7) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VCVT")) {
            /* VCVT register conversions.  Every base below is the encoding for
             * the S0/D0 form, measured with armasm (h7test/v3.lst, v4.lst):
             *   VCVT.S32.F32 = 0xEEBD0AC0   VCVT.U32.F32 = 0xEEBC0AC0
             *   VCVT.F32.S32 = 0xEEB80AC0   VCVT.F32.U32 = 0xEEB80A40
             *   VCVT.S32.F64 = 0xEEBD0BC0   VCVT.U32.F64 = 0xEEBC0BC0
             *   VCVT.F64.S32 = 0xEEB80BC0   VCVT.F64.U32 = 0xEEB80B40
             *   VCVT.F32.F64 = 0xEEB70BC0   VCVT.F64.F32 = 0xEEB70AC0
             * FIX(vfp-encode): the U32 variants and the .F32.F64/.F64.F32 pairs
             * used to fall through to the S32.F32 base, so unsigned
             * conversions and float<->double casts silently became S32.F32. */
            char suf[32];
            unsigned base;
            int sd = 0, sm = 0;
            int dst_dbl, src_dbl;
            unsigned d, vd, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            if      (strieq_a(suf, "S32F32")) base = 0xEEBD0AC0u;
            else if (strieq_a(suf, "U32F32")) base = 0xEEBC0AC0u;
            else if (strieq_a(suf, "F32S32")) base = 0xEEB80AC0u;
            else if (strieq_a(suf, "F32U32")) base = 0xEEB80A40u;
            else if (strieq_a(suf, "S32F64")) base = 0xEEBD0BC0u;
            else if (strieq_a(suf, "U32F64")) base = 0xEEBC0BC0u;
            else if (strieq_a(suf, "F64S32")) base = 0xEEB80BC0u;
            else if (strieq_a(suf, "F64U32")) base = 0xEEB80B40u;
            else if (strieq_a(suf, "F32F64")) base = 0xEEB70BC0u;
            else if (strieq_a(suf, "F64F32")) base = 0xEEB70AC0u;
            else {
                /* Do not guess: an unknown suffix used to be encoded as
                 * VCVT.S32.F32, i.e. a silently wrong operation. */
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "unsupported VCVT suffix", 0,0,0,0,0,0);
                a->hard_errors++;
                base = 0xEEBD0AC0u;
            }
            dst_dbl = strieq_a(suf, "F64S32") || strieq_a(suf, "F64U32") ||
                      strieq_a(suf, "F64F32");
            src_dbl = strieq_a(suf, "S32F64") || strieq_a(suf, "U32F64") ||
                      strieq_a(suf, "F32F64");
            if (dst_dbl) { sd = parse_vfp_dreg_a(&p); eat_comma_a(&p); }
            else         { sd = parse_vfp_sreg_a(&p); eat_comma_a(&p); }
            if (src_dbl) sm = parse_vfp_dreg_a(&p);
            else         sm = parse_vfp_sreg_a(&p);
            if (sd < 0) sd = 0; if (sm < 0) sm = 0;
            d  = ark_vfp_half(sd, dst_dbl);
            vd = ark_vfp_nib (sd, dst_dbl);
            m  = ark_vfp_half(sm, src_dbl);
            vm = ark_vfp_nib (sm, src_dbl);
            hi = (base >> 16) | (d << 6);
            lo = (base & 0xFFFF) | (vd << 12) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VCMP")) {
            /* VCMP.F32 Sd,Sm / VCMP.F64 Dd,Dm / VCMP.F32|F64 Sd,#0.0
             * Bases measured with armasm (h7test/v3.lst, v5.lst):
             *   VCMP.F32 Sd,Sm    = 0xEEB40A40 + regs
             *   VCMP.F64 Dd,Dm    = 0xEEB40B40 + regs
             *   VCMP.F32 Sd,#0.0  = 0xEEB50A40 + Sd
             *   VCMP.F64 Dd,#0.0  = 0xEEB50B40 + Dd
             * FIX(vfp-encode): "#0.0" used to make parse_vfp_sreg_a() return
             * -1, which was clamped to 0, i.e. the compare silently became
             * "against S0/D0".  It is now encoded properly, and any other
             * immediate is a hard error instead of a wrong answer. */
            char suf[16];
            int is_f64, cmp_zero = 0;
            unsigned base;
            int sd, sm = 0;
            unsigned d, vd, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            is_f64 = strieq_a(suf, "F64");
            base = is_f64 ? 0xEEB40B40u : 0xEEB40A40u;
            if (is_f64) sd = parse_vfp_dreg_a(&p);
            else        sd = parse_vfp_sreg_a(&p);
            if (sd < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad destination register for VCMP", 0,0,0,0,0,0);
                a->hard_errors++;
                sd = 0;
            }
            eat_comma_a(&p);
            skip_space_a(&p);
            if (*p == '#') {
                int nonzero = 0, ndig = 0;
                p++;
                skip_space_a(&p);
                if (*p == '-') p++;
                while ((*p >= '0' && *p <= '9') || *p == '.') {
                    if (*p >= '1' && *p <= '9') nonzero = 1;
                    if (*p != '.') ndig++;
                    p++;
                }
                if (nonzero || ndig == 0) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "VCMP supports only #0.0 as an immediate operand",
                              0,0,0,0,0,0);
                    a->hard_errors++;
                }
                cmp_zero = 1;
                base = is_f64 ? 0xEEB50B40u : 0xEEB50A40u;
            } else if (is_f64) {
                sm = parse_vfp_dreg_a(&p);
            } else {
                sm = parse_vfp_sreg_a(&p);
            }
            if (!cmp_zero && sm < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad operand register for VCMP", 0,0,0,0,0,0);
                a->hard_errors++;
                sm = 0;
            }
            d  = ark_vfp_half(sd, is_f64);
            vd = ark_vfp_nib (sd, is_f64);
            m  = cmp_zero ? 0u : ark_vfp_half(sm, is_f64);
            vm = cmp_zero ? 0u : ark_vfp_nib (sm, is_f64);
            hi = (base >> 16) | (d << 6);
            lo = (base & 0xFFFF) | (vd << 12) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VMRS")) {
            /* VMRS APSR_nzcv, FPSCR — fixed encoding 0xFEF1FA10 */
            out16(a, 0xFEF1);
            out16(a, 0xFA10);
        }
        else if (strieq_a(first, "VNEG") || strieq_a(first, "VABS") ||
                 strieq_a(first, "VSQRT")) {
            /* VNEG/VABS/VSQRT .F32 Sd,Sm  or  .F64 Dd,Dm */
            char suf[16];
            int is_f64;
            unsigned base;
            int sd, sm;
            unsigned d, vd, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            is_f64 = strieq_a(suf, "F64");
            /* FIX(vfp-encode): VNEG is 0xEEB10A40/0xEEB10B40 (0xEEB10AC0 is VSQRT). */
            if      (strieq_a(first, "VNEG"))  base = is_f64 ? 0xEEB10B40u : 0xEEB10A40u;
            else if (strieq_a(first, "VABS"))  base = is_f64 ? 0xEEB00BC0u : 0xEEB00AC0u;
            /* FIX(vfp-encode): VSQRT is 0xEEB10AC0/0xEEB10BC0 (0xEEB11AC0 does not exist). */
            else                               base = is_f64 ? 0xEEB10BC0u : 0xEEB10AC0u;
            if (is_f64) {
                sd = parse_vfp_dreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_dreg_a(&p);
            } else {
                sd = parse_vfp_sreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_sreg_a(&p);
            }
            if (sd < 0) sd = 0; if (sm < 0) sm = 0;
            d  = ark_vfp_half(sd, is_f64);
            vd = ark_vfp_nib (sd, is_f64);
            m  = ark_vfp_half(sm, is_f64);
            vm = ark_vfp_nib (sm, is_f64);
            hi = (base >> 16) | (d << 6);
            lo = (base & 0xFFFF) | (vd << 12) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VLDR") || strieq_a(first, "VSTR")) {
            /* VLDR/VSTR Sd, [Rn, #imm]  — 32-bit T1 encoding.
             * Base: VLDR.F32 = 0xED900A00, VSTR.F32 = 0xED800A00.
             * imm8 = offset>>2 (word-aligned). F64 sets bit 8 (sz). */
            char suf[16];
            int is_f64;
            unsigned base;
            int sd, rn, imm;
            unsigned d, vd;
            unsigned hi, lo;
            unsigned imm8;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            /* armasm also accepts the ".32"/".64" spelling of the same suffix. */
            is_f64 = strieq_a(suf, "F64") || strieq_a(suf, "64");
            if (strieq_a(first, "VLDR")) base = is_f64 ? 0xED900B00u : 0xED900A00u;
            else                         base = is_f64 ? 0xED800B00u : 0xED800A00u;
            if (is_f64) sd = parse_vfp_dreg_a(&p);
            else        sd = parse_vfp_sreg_a(&p);
            if (sd < 0) sd = 0;
            eat_comma_a(&p);
            skip_space_a(&p);
            if (*p == '[') p++;
            rn = parse_reg_a(&p);
            skip_space_a(&p);
            imm = 0;
            if (*p == ',') { p++; imm = parse_imm_a(&p); }
            skip_space_a(&p);
            if (*p == ']') p++;
            d  = ark_vfp_half(sd, is_f64);
            vd = ark_vfp_nib (sd, is_f64);
            imm8 = (unsigned)((imm >> 2) & 0xFF);
            hi = (base >> 16) | (d << 6) | (unsigned)(rn & 0xF);
            lo = (base & 0xFFFF) | (vd << 12) | imm8;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "TST")) {
            /* TST (register): 16-bit T1 0100000000 Rm Rn for r0-r7,
             * 32-bit T2 0xEA1n_0F0m for high registers.
             * Encodings verified against armasm: TST r4,r1 = 0x420C,
             * TST r8,r9 = 0xEA180F09, TST r0,r12 = 0xEA100F0C. */
            int rn = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rn >= 0 && rn <= 7 && rm >= 0 && rm <= 7) {
                out16(a, 0x4200 | ((rm & 7) << 3) | (rn & 7));
            } else if (rn >= 0 && rm >= 0) {
                out16(a, 0xEA10 | (rn & 0xF));
                out16(a, 0x0F00 | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for TST", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "CLZ")) {
            /* CLZ Rd, Rm: 32-bit T1 111110101011 Rm 1111 1000 Rd 0000.
             * Verified: CLZ r2,r0 = 0xFAB0F280, CLZ r12,r11 = 0xFABBFC8B. */
            int rd = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rm >= 0) {
                out16(a, 0xFAB0 | (rm & 0xF));
                out16(a, 0xF080 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for CLZ", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "RBIT")) {
            /* RBIT Rd, Rm: 32-bit T1 111110101001 Rm 1111 Rd 1010 Rm.
             * Verified with armasm 5.06u7 (Cortex-M7):
             *   RBIT r0,r0  = 0xFA90F0A0   RBIT r3,r1   = 0xFA91F3A1
             *   RBIT r9,r12 = 0xFA9CF9AC */
            int rd = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rm >= 0) {
                out16(a, 0xFA90 | (rm & 0xF));
                out16(a, 0xF0A0 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for RBIT", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "SXTB") || strieq_a(first, "SXTH") ||
                 strieq_a(first, "UXTB") || strieq_a(first, "UXTH")) {
            /* Sign/zero extend: 16-bit T1 (0xB240/0xB200/0xB2C0/0xB2B0)
             * for low registers, 32-bit T2 (0xFA4F/0xFA0F/0xFA5F/0xFA1F
             * F080 | Rd<<8 | Rm) otherwise.
             * Verified: SXTB r7,r5 = 0xB26F, SXTH r6,r3 = 0xB21E,
             *           UXTB r1,r2 = 0xB2D1, UXTH r0,r7 = 0xB2B8,
             *           SXTB r0,r8 = 0xFA4FF088, UXTH r9,r3 = 0xFA1FF983. */
            int rd = parse_reg_a(&p);
            int rm;
            unsigned lo_base = 0xB200u;
            unsigned hi_base = 0xFA0Fu;
            if (strieq_a(first, "SXTB")) { lo_base = 0xB240u; hi_base = 0xFA4Fu; }
            else if (strieq_a(first, "UXTB")) { lo_base = 0xB2C0u; hi_base = 0xFA5Fu; }
            else if (strieq_a(first, "UXTH")) { lo_base = 0xB2B0u; hi_base = 0xFA1Fu; }
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) {
                out16(a, lo_base | ((rm & 7) << 3) | (rd & 7));
            } else if (rd >= 0 && rm >= 0) {
                out16(a, hi_base | (rm & 0xF));
                out16(a, 0xF080 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                {
                    char mn[9];
                    int mi;
                    for (mi = 0; mi < 8 && first[mi]; mi++) mn[mi] = first[mi];
                    mn[mi] = 0;
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "bad register operand for %s", mn, 0,0,0,0,0);
                }
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "BKPT")) {
            /* BKPT #imm8: 0xBE00 | imm8. On Cortex-M this - not SVC 0xAB -
             * is the semihosting trap: QEMU only intercepts SVC 0xAB for
             * A/R profile cores and sends BKPT 0xAB to the semihosting
             * monitor on M-profile. Verified: BKPT #0xab = 0xBEAB (armasm
             * 5.06 golden bytes). */
            int imm;
            int ok = 0;
            skip_space_a(&p);
            imm = parse_imm_opt_hash_a(&p, &ok);
            if (!ok || imm < 0 || imm > 255) {
                if (!ok)
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "BKPT needs a numeric immediate", 0,0,0,0,0,0);
                else
                    {
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "BKPT immediate %s out of range 0..255",
                                  diag_num(b1, imm), 0,0,0,0,0);
                    }
                a->hard_errors++;
                imm = 0;
            }
            out16(a, 0xBE00 | (imm & 0xFF));
        }
        else if (strieq_a(first, "SVC")) {
            /* SVC #imm8: 0xDF00 | imm8. Verified: SVC #18 = 0xDF12.
             * The '#' is optional (armasm accepts "SVC 0xAB"), but the operand
             * must be a number: a missing operand used to encode SVC #0, which
             * silently breaks semihosting calls at run time. */
            int imm;
            int ok = 0;
            skip_space_a(&p);
            imm = parse_imm_opt_hash_a(&p, &ok);
            if (!ok || imm < 0 || imm > 255) {
                if (!ok)
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "SVC needs a numeric immediate", 0,0,0,0,0,0);
                else
                    {
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "SVC immediate %s out of range 0..255",
                                  diag_num(b1, imm), 0,0,0,0,0);
                    }
                a->hard_errors++;
                imm = 0;
            }
            out16(a, 0xDF00 | (imm & 0xFF));
        }
        else if (strieq_a(first, "MOVS")) {
            /* MOVS Rd,#imm: 16-bit 0x2000|Rd<<8|imm8, 32-bit MOVS.W
             * (0xF05F base, S=1) otherwise.
             * MOVS Rd,Rm: 16-bit 0x0000|Rm<<3|Rd (LSLS Rd,Rm,#0 alias),
             * 32-bit 0xEA5F_0n0m for high registers.
             * Verified: MOVS r0,#42 = 0x202A, MOVS r5,#7 = 0x2507,
             *           MOVS r0,r1 = 0x0008, MOVS r12,r3 = 0xEA5F0C03. */
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255) {
                    out16(a, 0x2000 | ((rd & 7) << 8) | (imm & 0xFF));
                } else {
                    unsigned i, imm3, imm8;
                    if (rd < 0 || !thumb_imm12_a((unsigned)imm, &i, &imm3, &imm8)) {
                        {
                            char b1[12], b2[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "MOVS immediate %s (Rd=r%s) cannot be encoded",
                                      diag_num(b1, imm), diag_num(b2, rd),
                                      0,0,0,0);
                        }
                        a->hard_errors++;
                        out16(a, 0xBF00);
                        out16(a, 0xBF00);
                    } else {
                        out16(a, 0xF05F | ((i & 1) << 10));
                        out16(a, ((imm3 & 7) << 12) | ((rd & 0xF) << 8) | imm8);
                    }
                }
            } else {
                int rm = parse_reg_a(&p);
                if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) {
                    out16(a, 0x0000 | ((rm & 7) << 3) | (rd & 7));
                } else if (rd >= 0 && rm >= 0) {
                    out16(a, 0xEA5F);
                    out16(a, ((rd & 0xF) << 8) | (rm & 0xF));
                } else {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "bad register operand for MOVS", 0,0,0,0,0,0);
                    a->hard_errors++;
                    out16(a, 0xBF00);
                }
            }
        }
        else if (strieq_a(first, "SADD16")) {
            /* SADD16 Rd, Rn, Rm: 0xFA90_F000|Rn<<16|Rd<<8|Rm.
             * Verified: SADD16 r0,r0,r1 = 0xFA90F001,
             *           SADD16 r3,r7,r5 = 0xFA97F305. */
            int rd = parse_reg_a(&p);
            int rn, rm;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rn >= 0 && rm >= 0) {
                out16(a, 0xFA90 | (rn & 0xF));
                out16(a, 0xF000 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for SADD16", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else {
            /* Unknown mnemonic. This used to silently emit a 16-bit NOP, which
             * turned a typo or an unimplemented instruction into a program that
             * still compiled and linked but did something else. Report it and
             * keep failing at the end of assembly. A NOP is still emitted so
             * that label addresses stay close to the intent and later code does
             * not cascade with bogus addresses. */
            {
                char mn[33];
                char b1[12];
                int mi;
                for (mi = 0; mi < 32 && first[mi]; mi++) mn[mi] = first[mi];
                mn[mi] = 0;
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "unknown mnemonic %sq at code offset %s "
                          "(no instruction emitted)", mn, diag_num(b1, a->pc),
                          0,0,0,0);
            }
            a->hard_errors++;
            out16(a, 0xBF00);
        }
        skip_to_eol_a(&p);
        if (*p == '\n') p++;
    }
}

static void resolve_refs_a(ArmAsm *a) {
    int i;
    for (i = 0; i < a->ref_count; i++) {
        AsmRef *r = &a->refs[i];
        int addr = find_label_a(a, r->name);
        if (addr < 0) {
            /* Not a local label: this is a cross-object reference and has
             * already been kept as a relocation by arm_save_relocs(), so the
             * linker owns it from here. Reporting it as an error here would
             * make every -c compile of a multi-file program exit non-zero on
             * a valid object (see ARK_DIAG_UNRESOLVED_LOCAL). */
#if ARK_DIAG_UNRESOLVED_LOCAL
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "unresolved label %sq", r->name, 0,0,0,0,0);
#endif
            a->error_count++;
            continue;
        }
        if (r->type == 1) {
            /* BL: 32-bit, re-emit at saved pc */
            int saved = a->pc;
            a->pc = r->pc;
            emit_bl_a(a, addr);
            a->pc = saved;
        } else if (r->type == 2) {
            /* B.W: 32-bit unconditional, re-emit at saved pc */
            int saved = a->pc;
            a->pc = r->pc;
            emit_b_w_a(a, addr);
            a->pc = saved;
        } else if (r->type == 3) {
            /* B<cond>.W: 32-bit conditional, recover cond from placeholder hi word */
            unsigned hi = (unsigned)a->out[r->pc] | ((unsigned)a->out[r->pc + 1] << 8);
            int cond = (hi >> 6) & 0xF;
            int saved = a->pc;
            a->pc = r->pc;
            emit_bcond_w_a(a, cond, addr);
            a->pc = saved;
        } else if (r->type == 4) {
            /* Bug-A5: R_ARM_ABS32 - 4-byte absolute address reference
             * (e.g. .word funcname in .init_array/.fini_array).
             * Patch the 4 bytes at r->pc with the resolved address. */
            a->out[r->pc]     = (unsigned char)(addr & 0xFF);
            a->out[r->pc + 1] = (unsigned char)((addr >> 8) & 0xFF);
            a->out[r->pc + 2] = (unsigned char)((addr >> 16) & 0xFF);
            a->out[r->pc + 3] = (unsigned char)((addr >> 24) & 0xFF);
        } else {
            /* type=0: legacy 16-bit branch (B/B<cond>), patch 2 bytes.
             * Kept for safety; new code uses type 2/3 above. */
            int offset = (addr - r->pc - 4) >> 1;
            unsigned hi = (unsigned)a->out[r->pc] | ((unsigned)a->out[r->pc + 1] << 8);
            if ((hi & 0xF000) == 0xE000) hi = (hi & 0xF800) | (offset & 0x7FF);
            else                          hi = (hi & 0xFF00) | (offset & 0xFF);
            a->out[r->pc]     = hi & 0xFF;
            a->out[r->pc + 1] = (hi >> 8) & 0xFF;
        }
    }
}

static int arm_emit(IrProgram *m, char *out, int cap) {
    char *text = codegen_arm(m);
    int n = ark_strlen(text);
    int i;
    if (n + 1 > cap) n = cap - 1;
    for (i = 0; i < n; i++) out[i] = text[i];
    out[n] = 0;
    return n;
}

static void arm_save_syms(ArmAsm *a);  /* forward decl */
static void arm_save_relocs(ArmAsm *a);  /* Bug-A2: forward decl */

/* Latch the ARKCC_DUMP_SYMS environment variable (host only) into
 * g_arm_dump_syms at arkcc startup, so the ARM backend has a single uniform
 * flag on both profiles. On STM32 getenv() is not available and this is a
 * no-op; the flag is set via UART there. Called from ark_compile(). */
void arm_dump_syms_init(void) {
#if ARKCC_PROFILE != ARKCC_PROFILE_STM32_MIN
    if (getenv("ARKCC_DUMP_SYMS")) g_arm_dump_syms = 1;
#endif
}

static int arm_assemble(const char *asm_text, unsigned char *out, int cap) {
    ArmAsm a;
    a.src = asm_text;
    a.out = out;
    a.pc = 0;
    a.out_cap = cap;
    a.labels = 0; a.label_count = 0; a.label_cap = 0;
    a.refs = 0; a.ref_count = 0; a.ref_cap = 0;
    a.error_count = 0;
    a.hard_errors = 0;
    a.sets = 0; a.set_count = 0; a.set_cap = 0;
    a.literals = 0; a.literal_count = 0; a.literal_cap = 0;
    a.literal_pool_start = -1;
    a.pending_weak = 0;
    a.cur_section = 0;
    /* --vector-table: hand the emitter a buffer whose first ARM_VT_SIZE bytes
     * are already claimed, zeroed.  The linker only fills the table in. */
    if (g_arm_vt_reserve) {
        int k;
        if (ARM_VT_SIZE > cap) return -1;
        for (k = 0; k < ARM_VT_SIZE; k++) out[k] = 0;
    }
    pass1_a(&a);
    a.pc = g_arm_vt_reserve ? ARM_VT_SIZE : 0;
    pass2_a(&a);
    resolve_refs_a(&a);
    /* Save symbol table for linker */
    arm_save_syms(&a);
    /* Bug-A2: Save unresolved cross-object references as relocations
     * for the linker to resolve. This must be called AFTER arm_save_syms
     * so that the global symbol/reloc arrays are populated before the
     * object file is written. */
    arm_save_relocs(&a);
    /* Debug: dump label addresses when g_arm_dump_syms is set. On host the
     * flag is latched from getenv("ARKCC_DUMP_SYMS") at arkcc startup (see
     * arm_dump_syms_init); on STM32 it is set via the UART "debug syms"
     * command, since getenv() is unavailable under semihosting. */
    if (g_arm_dump_syms) {
        {
            int di;
            ark_log("=== ARM Symbol Map (%d labels) ===\n", a.label_count);
            for (di = 0; di < a.label_count; di++) {
                if (a.labels[di].name[0] != '.')
                    ark_log("  %s 0x%X (%d)\n",
                            a.labels[di].name, a.labels[di].addr, a.labels[di].addr);
            }
            ark_log("=== End Symbol Map ===\n");
        }
    }
    /* Genuine assembly errors must fail the compile: continuing would hand
     * back machine code that does not match the source. Unresolved external
     * references are NOT in this counter (resolve_refs_a keeps them in
     * error_count), because the linker may satisfy them later.
     * Bug-A2: error_count alone therefore still does not fail the compile. */
    if (a.hard_errors > 0) {
        {
            char b1[12];
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "%s assembly error(s), no output written",
                      diag_num(b1, a.hard_errors), 0,0,0,0,0);
        }
        return -1;
    }
    return a.pc;
}

/* arm_link - per-object link stub (intentionally a no-op).
 *
 * Design rationale: arkcc splits linking responsibility across two stages.
 *
 *  1) Backend stage (this function): called once per object right after
 *     arm_assemble, with only that object's code and no link context
 *     (lc is always NULL today - see the call sites in compiler.c).
 *     Cross-object references cannot be resolved here because we only see
 *     one object; the unresolved refs were already captured by
 *     arm_save_relocs() into g_arm_relocs for later use.
 *
 *  2) Linker stage (compiler.c -link mode): the ARKOBJ2 binary linker
 *     merges code sections from every input object, builds a global
 *     symbol table with weak-symbol precedence (ld_add_sym), and
 *     patches cross-object relocations (ld_patch_ref for BL/B.W/
 *     B<cond>.W/R_ARM_ABS32). Unresolved symbols are reported but do
 *     not abort - they may be satisfied by an external library.
 *
 * Per-mode behaviour:
 *   -S      : emits GAS-compatible assembly; real linking is delegated
 *             to arm-none-eabi-ld / arm-none-eabi-gcc externally.
 *   -c      : writes an ARKOBJ2 object (code + symbol table + reloc
 *             table) for the -link stage to consume later.
 *   -link   : runs the ARKOBJ2 binary linker described above.
 *   -> .bin : single-file direct binary; cross-object refs are left
 *             unresolved (acceptable for self-contained units).
 *
 * Keeping this function a no-op is the correct division of labour:
 * the backend assembles and exports symbols/relocs; compiler.c links.
 * Making arm_link do real work would duplicate the -link logic and
 * still could not resolve cross-object refs without all objects.
 *
 * Exception: vector table generation is handled after symbol table is
 * built (see arm_link_impl below, called via the forward declaration). */
static int arm_link(unsigned char *code, int size, struct LinkContext *lc);  /* forward decl */

/* --- Symbol table export for linker --- */
typedef struct ArmSym {
    char *name;
    int   offset;   /* byte offset from start of code section */
    int   type;     /* 0=code label, 1=data/global */
    int   is_weak;  /* 1=weak symbol (overridable by a strong definition) */
} ArmSym;

static ArmSym  g_arm_syms[ARM_MAX_SYMBOLS] __attribute__((section(".ark_buf")));
static int     g_arm_sym_count = 0;

/* Bug-A2: Relocation table for cross-object references.
 * After resolve_refs_a, any ref whose target label was not found in the
 * current object is saved here as a relocation entry. The linker will
 * resolve these using the global symbol table. */
typedef struct ArmReloc {
    char *name;    /* target symbol name */
    int   offset;  /* byte offset within code section to patch */
    int   type;    /* 1=BL(T1), 2=B.W, 3=B<cond>.W, 4=R_ARM_ABS32 (.word) */
} ArmReloc;

static ArmReloc g_arm_relocs[ARM_MAX_RELOCS] __attribute__((section(".ark_buf")));
static int      g_arm_reloc_count = 0;

/* Save symbol table after assembly */
static void arm_save_syms(ArmAsm *a) {
    int i;
    g_arm_sym_count = 0;
    for (i = 0; i < a->label_count && g_arm_sym_count < ARM_MAX_SYMBOLS; i++) {
        /* Skip local labels (starting with .L) */
        if (a->labels[i].name[0] == '.' && a->labels[i].name[1] == 'L') continue;
        g_arm_syms[g_arm_sym_count].name   = a->labels[i].name;
        g_arm_syms[g_arm_sym_count].offset = a->labels[i].addr;
        g_arm_syms[g_arm_sym_count].type   = 0;  /* code label */
        g_arm_syms[g_arm_sym_count].is_weak = a->labels[i].is_weak;
        g_arm_sym_count++;
    }
}

/* Bug-A2: Save unresolved refs as relocations for the linker.
 * Called after resolve_refs_a; scans the ref table and saves any ref
 * whose target is not defined in the current object. */
static void arm_save_relocs(ArmAsm *a) {
    int i;
    g_arm_reloc_count = 0;
    for (i = 0; i < a->ref_count && g_arm_reloc_count < ARM_MAX_RELOCS; i++) {
        AsmRef *r = &a->refs[i];
        int addr = find_label_a(a, r->name);
        if (addr < 0) {
            /* Unresolved: this is a cross-object reference.
             * Save it as a relocation for the linker. */
            g_arm_relocs[g_arm_reloc_count].name   = r->name;
            g_arm_relocs[g_arm_reloc_count].offset = r->pc;
            g_arm_relocs[g_arm_reloc_count].type   = r->type;
            g_arm_reloc_count++;
        }
    }
}

/* Export symbol info for the linker */
int arm_get_symbol_count(void) { return g_arm_sym_count; }
const char *arm_get_symbol_name(int idx) { return g_arm_syms[idx].name; }
int arm_get_symbol_offset(int idx) { return g_arm_syms[idx].offset; }
int arm_get_symbol_type(int idx) { return g_arm_syms[idx].type; }
int arm_get_symbol_weak(int idx) { return g_arm_syms[idx].is_weak; }

/* SRAM window covered by this translation unit's .data + .bss (absolute
 * addresses; both equal g_data_base when the unit declares no RAM objects).
 * Exported so the -link stage can detect two objects that were each laid out
 * from the same SRAM base: -link concatenates code but does not relocate
 * data, so such a pair would silently alias its globals. */
unsigned arm_get_ram_start(void) { return g_data_sram_start; }
unsigned arm_get_ram_end(void)   { return g_bss_sram_end; }

/* Bug-A2: Export relocation info for the linker */
int arm_get_reloc_count(void) { return g_arm_reloc_count; }
const char *arm_get_reloc_name(int idx) { return g_arm_relocs[idx].name; }
int arm_get_reloc_offset(int idx) { return g_arm_relocs[idx].offset; }
int arm_get_reloc_type(int idx) { return g_arm_relocs[idx].type; }

/* === Vector table generation (single-file .bin mode) ===
 * If Reset_Handler is defined, generates a Cortex-M vector table at the start
 * of the binary. Safe because Thumb-2 branches use PC-relative offsets that
 * survive a uniform shift; absolute (.word) refs are already resolved by
 * resolve_refs_a within the object.
 *
 * Every entry must carry the Thumb bit (bit 0): on exception entry the core
 * loads EPSR.T from that bit, so an even address makes a Cortex-M core take
 * an INVSTATE UsageFault instead of running the handler. Real STM32 vector
 * tables are odd for the same reason. */
static const char *g_arm_vector_names[] = {
    NULL, "Reset_Handler", "NMI_Handler", "HardFault_Handler",
    "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler",
    NULL, NULL, NULL, NULL,
    "SVC_Handler", "DebugMon_Handler", NULL,
    "PendSV_Handler", "SysTick_Handler"
};
#define ARM_SYSTEM_VECTOR_COUNT 16

static int arm_link(unsigned char *code, int size, struct LinkContext *lc) {
    int i, reset_idx = -1, default_idx = -1;
    unsigned flash_base, initial_sp;
    int new_size;
    (void)lc;

    /* Find Reset_Handler in symbol table */
    for (i = 0; i < g_arm_sym_count; i++) {
        if (ark_streq(g_arm_syms[i].name, "Reset_Handler")) { reset_idx = i; break; }
    }
    if (reset_idx < 0) return size;  /* no Reset_Handler: skip vector table */

    /* Find Default_Handler for filling unresolved entries */
    for (i = 0; i < g_arm_sym_count; i++) {
        if (ark_streq(g_arm_syms[i].name, "Default_Handler")) { default_idx = i; break; }
    }

    /* Determine flash_base and initial_sp from scatter or defaults */
    flash_base = g_flash_base;
    initial_sp = g_bss_base + 0x20000;  /* default: SRAM + 128KB */
#if ARKCC_FEATURE_SCATTER
    if (g_scatter_loaded) {
        ScatterExecRegion *er_rw = scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_RW);
        if (er_rw && er_rw->size > 0)
            initial_sp = er_rw->load_addr + er_rw->size;
    }
#endif

    if (g_arm_vt_reserve) {
        /* --vector-table: the emitter started at ARM_VT_SIZE, so the table
         * area is already in place and every address is already correct.
         * Moving anything here would shift the image away from the addresses
         * the assembler encoded (that was the bug this option exists to fix). */
        new_size = size;
    } else {
        new_size = size + ARM_VT_SIZE;
        if (new_size > ARK_CODE_BUF_SIZE) return -1;

        /* Move existing code right by ARM_VT_SIZE bytes */
        for (i = size - 1; i >= 0; i--) code[i + ARM_VT_SIZE] = code[i];

        /* Adjust all symbol offsets by ARM_VT_SIZE */
        for (i = 0; i < g_arm_sym_count; i++)
            g_arm_syms[i].offset += ARM_VT_SIZE;
    }

    /* Generate vector table at code[0] */
    {
        /* Bit 0 is the Thumb bit; a zero entry stays zero ("no handler"). */
        unsigned default_addr = (default_idx >= 0) ?
            ((flash_base + (unsigned)g_arm_syms[default_idx].offset) | 1u) : 0;
        /* Entry 0: initial SP */
        code[0] = (unsigned char)(initial_sp & 0xFF);
        code[1] = (unsigned char)((initial_sp >> 8) & 0xFF);
        code[2] = (unsigned char)((initial_sp >> 16) & 0xFF);
        code[3] = (unsigned char)((initial_sp >> 24) & 0xFF);
        /* Fill entries 1..N with Default_Handler (or 0) */
        for (i = 1; i < ARM_VT_ENTRIES; i++) {
            code[i*4]   = (unsigned char)(default_addr & 0xFF);
            code[i*4+1] = (unsigned char)((default_addr >> 8) & 0xFF);
            code[i*4+2] = (unsigned char)((default_addr >> 16) & 0xFF);
            code[i*4+3] = (unsigned char)((default_addr >> 24) & 0xFF);
        }
        /* Fill in known system exception handlers */
        for (i = 1; i < ARM_SYSTEM_VECTOR_COUNT; i++) {
            if (g_arm_vector_names[i]) {
                int j;
                for (j = 0; j < g_arm_sym_count; j++) {
                    if (ark_streq(g_arm_syms[j].name, g_arm_vector_names[i])) {
                        unsigned addr = (flash_base + (unsigned)g_arm_syms[j].offset) | 1u;
                        code[i*4]   = (unsigned char)(addr & 0xFF);
                        code[i*4+1] = (unsigned char)((addr >> 8) & 0xFF);
                        code[i*4+2] = (unsigned char)((addr >> 16) & 0xFF);
                        code[i*4+3] = (unsigned char)((addr >> 24) & 0xFF);
                        break;
                    }
                }
            }
        }
    }
    return new_size;
}

static const ArkBackend g_arm_backend = {
    "arm",
    "armv7m-thumb2-eabi",
    4,
    arm_emit,
    arm_assemble,
    arm_link,
    ARK_ABI_SYSV
};

const ArkBackend *ark_backend_arm(void) {
    return &g_arm_backend;
}
