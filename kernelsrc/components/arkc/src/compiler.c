/* Ark C Compiler - compiler
 * Top-level pipeline: preproc -> lex -> parse -> sema -> irgen -> backend(emit/assemble/link).
 */
#include "compiler.h"
#include "platform.h"
#include "memory.h"
#include "flash_fs.h"
#include "lexer.h"
#include "parser.h"
#include "sema.h"
#include "irgen.h"
#include "ir.h"
#include "preproc.h"
#include "cpu.h"
#include "backend.h"
#include "config.h"
#include "arkcc_config.h"
#include "opt.h"
#include "scatter.h"
#include "diag.h"
/* Note: <stdio.h> intentionally NOT included - all file I/O goes through
 * fs_read_file/fs_write_file (flash_fs abstraction) to avoid pulling in
 * semihosting-dependent C library functions on STM32. */

/* Buffer sizes come from arkcc_config.h (profile-aware): ARK_SRC_BUF_SIZE,
 * ARK_ASM_BUF_SIZE, ARK_CODE_BUF_SIZE, ARK_OBJ_BUF_SIZE, ARK_LINK_BUF_SIZE.
 * STM32 profile uses much smaller buffers to fit in AXI SRAM. */

/* Place large buffers in .ark_buf section so the linker script can put them
 * in AXI SRAM (0x24000000) on STM32H743. On host builds this is a no-op. */
static char          g_src[ARK_SRC_BUF_SIZE]   __attribute__((section(".ark_buf")));
static char          g_asm[ARK_ASM_BUF_SIZE]   __attribute__((section(".ark_buf")));
static unsigned char g_code[ARK_CODE_BUF_SIZE] __attribute__((section(".ark_buf")));

/* Where those buffers ended up.  The STM32H743 target links .ark_buf into
 * the external SDRAM (0xC0000000); the host build and the *_sram.sct
 * fallback keep them on chip.  main() prints this so the layout is a
 * measurement instead of a belief.
 */
unsigned long arkcc_buf_addr(void)
{
    return (unsigned long)(void *)g_src;
}

static int derive_output(const char *input, char *out, int cap, const char *ext) {
    int n = ark_strlen(input);
    int el = ark_strlen(ext);
    int i;
    int dot = -1;
    if (n <= 0 || n + el >= cap - 1) return -1;
    for (i = n - 1; i >= 0; i--) {
        if (input[i] == '/' || input[i] == '\\') break;
        if (input[i] == '.') { dot = i; break; }
    }
    if (dot < 0) dot = n;
    {
        int k;
        for (k = 0; k < dot && k < cap - el - 1; k++) out[k] = input[k];
        for (k = 0; k < el && dot + k < cap - 1; k++) out[dot + k] = ext[k];
        out[dot + el] = 0;
    }
    return 0;
}

/* Assembly-input detection by file extension. ".s"/".S" is the traditional
 * suffix; ".asm"/".ASM" (armasm / MSVC style) is accepted too, matching case
 * insensitively so the same command line works on Windows-style trees. */
static int is_asm_input(const char *path) {
    int n;
    if (!path) return 0;
    n = ark_strlen(path);
    if (n >= 4 && path[n - 4] == '.' &&
        (path[n - 3] == 'a' || path[n - 3] == 'A') &&
        (path[n - 2] == 's' || path[n - 2] == 'S') &&
        (path[n - 1] == 'm' || path[n - 1] == 'M')) return 1;
    if (n >= 2 && path[n - 2] == '.' &&
        (path[n - 1] == 's' || path[n - 1] == 'S')) return 1;
    return 0;
}

static int write_binary(const char *path, const unsigned char *data, int n) {
    int wr = fs_write_file(path, data, n);
    if (wr != n) {
        diag_tool(DIAG_SEV_ERROR, path, "cannot write output file", 0,0,0,0,0,0);
        return ARK_ERR_IO;
    }
    return ARK_OK;
}

static int write_text(const char *path, const char *s) {
    int n = ark_strlen(s);
    int wr = fs_write_file(path, (const unsigned char *)s, n);
    if (wr != n) {
        diag_tool(DIAG_SEV_ERROR, path, "cannot write output file", 0,0,0,0,0,0);
        return ARK_ERR_IO;
    }
    return ARK_OK;
}

/* Name the tool-stage diagnostics are anchored to. armlink reports against the
 * output image; we do the same, and fall back to a literal when no image name
 * is in scope yet. */
static const char *g_ld_image = "<link>";

/* "0x%08X" for diagnostics: diag_num only renders decimal, and a hex address
 * in decimal would be unreadable and unsearchable. */
static const char *ld_hex(char *buf, unsigned v) {
    static const char hexd[] = "0123456789ABCDEF";
    int i;
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) buf[2 + i] = hexd[(v >> ((7 - i) * 4)) & 0xF];
    buf[10] = 0;
    return buf;
}

/* === Binary ARM linker (ARKOBJ2) ===
 * Merges multiple .ark_obj files: concatenates code sections, builds a global
 * symbol table with weak-symbol precedence (strong > weak, first weak wins),
 * and resolves cross-object relocations (BL/B.W/B<cond>.W/R_ARM_ABS32). */
/* Sizes come from arkcc_config.h (section 9) so a big program can raise them
 * without touching the linker; overflow is reported (see ld_table_overflow).
 * The per-profile numbers there are the truth -- do not redefine them here. */
#define LD_MAX_SYMS   ARK_LINK_MAX_SYMS
#define LD_MAX_RELOCS ARK_LINK_MAX_RELOCS

typedef struct LdSym {
    char name[128];
    int  offset;   /* offset within merged code */
    int  is_weak;
} LdSym;

typedef struct LdReloc {
    char name[128];
    int  offset;   /* offset within merged code (adjusted by object base) */
    int  type;     /* 1=BL, 2=B.W, 3=B<cond>.W, 4=R_ARM_ABS32, 6=data addr */
    int  obj;      /* 0-based index of the object it came from (--reloc-data) */
} LdReloc;

/* Per-object SRAM footprint reported by the object's reserved
 * __arkcc_ram_start / __arkcc_ram_end symbols (absolute addresses). */
typedef struct LdRam {
    unsigned start;
    unsigned end;
    int      have_start;
    int      have_end;
} LdRam;

/* Per-object bookkeeping. Sized from arkcc_config.h (host 1024 / board 64);
 * going over it used to be silent (the object's code was merged but its RAM
 * window and __arkcc_init_globals body were skipped), so the count is now
 * checked up front. */
#define LD_MAX_OBJS ARK_LINK_MAX_OBJS

/* The two image-sized link buffers are static arrays on the board but come from
 * the pool on the host, so their capacity cannot be recovered with sizeof();
 * every size check uses this one name. */
#define LD_BUF_SIZE ARK_LINK_BUF_SIZE
static LdRam    g_ld_ram[LD_MAX_OBJS];
static int      g_ld_ram_obj_count = 0;
/* --reloc-data: how far each object's data window had to move, and the address
 * of its own __arkcc_init_globals body (-1 = the object has none).  Both are
 * filled while loading, then used once the whole link set is known. */
static int      g_ld_data_delta[LD_MAX_OBJS];
static int      g_ld_init_body[LD_MAX_OBJS];
/* --reloc-text: where each object started in the merged code buffer, and the
 * base it was compiled against.  The delta the linker has to apply is
 * (image base + code base) - compiled base, which is exactly the difference
 * between where the address will be and where the object thought it would be. */
static int      g_ld_code_base[LD_MAX_OBJS];
static unsigned g_ld_text_base[LD_MAX_OBJS];
static int      g_ld_have_text_base[LD_MAX_OBJS];
/* Highest SRAM address the relocated data reaches; checked against the stack
 * pointer once the scatter file (if any) has been read. */
static unsigned g_ld_data_top = 0;
/* Number of duplicate compiler-runtime symbols silently merged (see ld_add_sym). */
static int      ld_rt_dup_count = 0;
/* Entries that did not fit in the symbol / relocation table. Any drop makes
 * the image wrong (e.g. Reset_Handler disappears -> no vector table), so the
 * link refuses to write output when this is non-zero. */
static int      ld_table_overflow = 0;

static LdSym    g_ld_syms[LD_MAX_SYMS];
static int      g_ld_sym_count = 0;
static LdReloc  g_ld_relocs[LD_MAX_RELOCS];
static int      g_ld_reloc_count = 0;

static int rd_le32(const unsigned char *p) {
    /* Byte-wise through a volatile pointer on purpose: do not let the
     * compiler fold this into one wide load. These reads walk the *byte
     * stream of an object file*, where 4-byte fields frequently land on
     * offsets that are not 4-byte aligned (a name of arbitrary length is
     * followed by the next field). On-chip RAM tolerates that; the 16-bit
     * SDRAM bus on the STM32H743 target does not - measured on the board:
     * the same address read byte-wise gave 45 (the symbol count) while a
     * folded word read gave 0, so the whole symbol table collapsed to zero
     * entries, ld_find_global("Reset_Handler") failed, and the vector
     * table was never written. A wide load here is an optimisation that
     * changes behaviour, so it is opted out explicitly. */
    const volatile unsigned char *v = p;
    return (int)((unsigned)v[0] | ((unsigned)v[1] << 8) |
                 ((unsigned)v[2] << 16) | ((unsigned)v[3] << 24));
}

/* find_global: prefer strong over weak; first weak if no strong.
 * Returns symbol index, or -1 if not found. */
static int ld_find_global(const char *name) {
    int weak_idx = -1;
    int i;
    for (i = 0; i < g_ld_sym_count; i++) {
        if (ark_streq(g_ld_syms[i].name, name)) {
            if (!g_ld_syms[i].is_weak) return i;  /* strong wins */
            if (weak_idx < 0) weak_idx = i;       /* first weak */
        }
    }
    return weak_idx;
}

/* Add a symbol; strong overrides weak, weak skipped if strong exists. */
static void ld_add_sym(const char *name, int offset, int is_weak) {
    int idx, n;
    if (g_ld_sym_count >= LD_MAX_SYMS) { ld_table_overflow++; return; }
    idx = ld_find_global(name);
    if (idx >= 0 && !g_ld_syms[idx].is_weak && is_weak) return;  /* strong exists, skip weak */
    if (idx >= 0 && g_ld_syms[idx].is_weak && !is_weak) {       /* strong overrides weak */
        g_ld_syms[idx].offset = offset;
        g_ld_syms[idx].is_weak = 0;
        return;
    }
    if (idx >= 0) {
        if (!g_ld_syms[idx].is_weak && !is_weak) {
            /* Every object re-emits the private runtime helpers (__aeabi_*,
             * __arkcc_*). They are bit-identical, so "first definition wins"
             * is correct and warning once per helper per object is pure noise;
             * keep a count and report it in one line instead. */
            if (name[0] == '_' && name[1] == '_') {
                ld_rt_dup_count++;
            } else {
                diag_tool(DIAG_SEV_WARNING, g_ld_image,
                          "duplicate strong symbol %sq, using first definition",
                          name, 0,0,0,0,0);
            }
        }
        return;  /* duplicate (same strength), keep first */
    }
    n = 0;
    while (name[n] && n < (int)sizeof(g_ld_syms[g_ld_sym_count].name) - 1) {
        g_ld_syms[g_ld_sym_count].name[n] = name[n]; n++;
    }
    g_ld_syms[g_ld_sym_count].name[n] = 0;
    g_ld_syms[g_ld_sym_count].offset = offset;
    g_ld_syms[g_ld_sym_count].is_weak = is_weak;
    g_ld_sym_count++;
}

static void ld_add_reloc(const char *name, int offset, int type, int obj) {
    int n;
    if (g_ld_reloc_count >= LD_MAX_RELOCS) { ld_table_overflow++; return; }
    n = 0;
    while (name[n] && n < (int)sizeof(g_ld_relocs[g_ld_reloc_count].name) - 1) {
        g_ld_relocs[g_ld_reloc_count].name[n] = name[n]; n++;
    }
    g_ld_relocs[g_ld_reloc_count].name[n] = 0;
    g_ld_relocs[g_ld_reloc_count].offset = offset;
    g_ld_relocs[g_ld_reloc_count].type = type;
    g_ld_relocs[g_ld_reloc_count].obj = obj;
    g_ld_reloc_count++;
}

/* Implemented in the ARM backend (same encoder the assembler uses, so the
 * linker cannot drift from it). */
extern int arm_patch_movw_movt(unsigned char *code, int code_len, int off, int delta);

/* --reloc-data: give every object its own slice of SRAM.
 *
 * Why the linker needs the compiler's help here: an ARM image carries absolute
 * data addresses baked into MOVW/MOVT pairs, so a data section cannot simply be
 * copied elsewhere at link time -- the instructions that use it have to be
 * rewritten too.  Codegen marks those pairs (.arkdata -> relocation type 6) and
 * the assembler records their offsets; this function decides the new bases and
 * applies the delta.
 *
 * Object 0 keeps the base it was compiled against, so a single-object link is
 * unaffected; each following window is placed after the previous one, 8-byte
 * aligned (AAPCS).  A pair whose bytes do not decode as MOVW+same-register-MOVT
 * is a hard error: that object was built without --reloc-data and writing the
 * image anyway would run with stale addresses.
 *
 * Returns 0 on success, -1 when the image must not be written. */
static int ld_relocate_data(unsigned char *code, int code_len) {
    int i, k, placed = 0;
    unsigned cursor = 0;
    g_ld_data_top = 0;
    for (i = 0; i < LD_MAX_OBJS; i++) {
        LdRam *r = &g_ld_ram[i];
        unsigned size, nb, delta;
        if (!r->have_start || !r->have_end) continue;
        if (r->end <= r->start) { g_ld_data_delta[i] = 0; continue; }
        size = r->end - r->start;
        if (placed == 0) {
            g_ld_data_delta[i] = 0;
            cursor = r->end;
            placed++;
            continue;
        }
        nb = (cursor + 7u) & ~7u;
        delta = nb - r->start;
        g_ld_data_delta[i] = (int)delta;
        r->start = nb;
        r->end   = nb + size;
        cursor   = r->end;
        placed++;
        if (delta != 0) {
            int patched = 0;
            for (k = 0; k < g_ld_reloc_count; k++) {
                if (g_ld_relocs[k].type != 6) continue;
                if (g_ld_relocs[k].obj != i) continue;
                if (arm_patch_movw_movt(code, code_len, g_ld_relocs[k].offset,
                                        (int)delta) != 0) {
                    char b1[12], b2[12];
                    diag_tool(DIAG_SEV_ERROR, g_ld_image,
                              "data address at offset %s (object #%s) is not a "
                              "MOVW/MOVT pair; that object was compiled without "
                              "--reloc-data - recompile every object with it, or "
                              "link without it",
                              diag_num(b1, g_ld_relocs[k].offset),
                              diag_num(b2, i), 0,0,0,0);
                    return -1;
                }
                patched++;
            }
            if (patched == 0) {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                          "object #%s declares data but carries no relocatable "
                          "address (compiled without --reloc-data); its globals "
                          "would still point at the previous object's window",
                          diag_num(b1, i), 0,0,0,0,0);
                return -1;
            }
        }
    }
    g_ld_data_top = cursor;
    return 0;
}

/* --reloc-text: rewrite every MOVW/MOVT pair that carries an address inside an
 * object's own code/rodata window.
 *
 * Why this cannot be done earlier: the object was assembled with
 * `g_flash_base + offset` baked straight into the instruction stream, and the
 * linker is the first thing that knows where the object really ends up.  A
 * single-object image whose base equals the compile-time base produces
 * delta 0 and this loop changes nothing, which is why the switch is off by
 * default.
 *
 * The delta is per object, not global: object #1 starts after object #0, so its
 * baked addresses are short by exactly that much.  That is also why the obj
 * file records the compile-time base rather than the linker being asked to
 * assume one -- assuming is how the wrong answer got here in the first place.
 *
 * Returns -1 when the image must not be written: a site whose bytes are not a
 * MOVW+same-register-MOVT pair means something other than codegen wrote it, and
 * a site with no base to compare against means the object lost its descriptor. */
static int ld_relocate_text(unsigned char *code, int code_len,
                            unsigned image_base) {
    int i, k;

    /* Every declared site needs a base.  Check first: patching half the image
     * and then discovering the other half is unattributable would leave a
     * wrong binary on disk. */
    for (k = 0; k < g_ld_reloc_count; k++) {
        int o = g_ld_relocs[k].obj;
        if (g_ld_relocs[k].type != 12) continue;
        if (o < 0 || o >= LD_MAX_OBJS || !g_ld_have_text_base[o]) {
            char b1[12], b2[12];
            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                      "object #%s carries a --reloc-text site at offset %s but no "
                      "__arkcc_text_base descriptor, so its absolute addresses "
                      "cannot be corrected; recompile that object with arkcc "
                      "0.8.0 or drop --reloc-text",
                      diag_num(b1, o), diag_num(b2, g_ld_relocs[k].offset),
                      0,0,0,0);
            return -1;
        }
    }

    for (i = 0; i < LD_MAX_OBJS; i++) {
        unsigned delta;
        if (!g_ld_have_text_base[i]) continue;
        delta = (image_base + (unsigned)g_ld_code_base[i]) - g_ld_text_base[i];
        if (delta == 0) continue;      /* compiled for exactly where it landed */
        for (k = 0; k < g_ld_reloc_count; k++) {
            if (g_ld_relocs[k].type != 12) continue;
            if (g_ld_relocs[k].obj != i) continue;
            if (arm_patch_movw_movt(code, code_len, g_ld_relocs[k].offset,
                                    (int)delta) != 0) {
                char b1[12], b2[12], b3[12];
                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                          "absolute address at offset %s (object #%s) is not a "
                          "MOVW/MOVT pair, so a %s-byte shift cannot be applied; "
                          "the image would call the old address",
                          diag_num(b1, g_ld_relocs[k].offset), diag_num(b2, i),
                          ld_hex(b3, delta), 0, 0, 0);
                return -1;
            }
        }
    }
    return 0;
}

/* Patch a Thumb-2 branch or absolute reference in the merged code buffer. */
static void ld_patch_ref(unsigned char *code, int code_size, int pc, int target, int type) {
    if (pc < 0 || pc + 4 > code_size) {
        {
            char b1[12], b2[12];
            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                      "relocation offset %s out of bounds (code size %s)",
                      diag_num(b1, pc), diag_num(b2, code_size), 0,0,0,0);
        }
        return;
    }
    if (type == 4) {
        /* R_ARM_ABS32 / R_386_32: 4-byte absolute address */
        code[pc]     = (unsigned char)(target & 0xFF);
        code[pc + 1] = (unsigned char)((target >> 8) & 0xFF);
        code[pc + 2] = (unsigned char)((target >> 16) & 0xFF);
        code[pc + 3] = (unsigned char)((target >> 24) & 0xFF);
        return;
    }
    if (type == 10 || type == 11) {
        /* LDR rd, =sym: the object baked MOVW/MOVT of its own flash base
         * with the symbol still unknown, so the patch is a delta of the
         * symbol's offset in the final image (+1 for a function label:
         * a Thumb function pointer must carry bit 0). */
        int delta = target + ((type == 11) ? 1 : 0);
        if (arm_patch_movw_movt(code, code_size, pc, delta) != 0) {
            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                      "a cross-object address does not point at a "
                      "MOVW/MOVT pair; that object was not produced by "
                      "this compiler",
                      0, 0, 0, 0, 0, 0);
        }
        return;
    }
    if (type == 5) {
        /* R_386_PC32: x86 PC-relative disp32 (relative to next instr = pc+4) */
        int disp = target - (pc + 4);
        code[pc]     = (unsigned char)(disp & 0xFF);
        code[pc + 1] = (unsigned char)((disp >> 8) & 0xFF);
        code[pc + 2] = (unsigned char)((disp >> 16) & 0xFF);
        code[pc + 3] = (unsigned char)((disp >> 24) & 0xFF);
        return;
    }
    {
        /* Relative branch: BL(T1) / B.W(T4) / B<cond>.W(T3) */
        int offset = target - pc - 4;
        int off2 = offset >> 1;
        unsigned hi, lo;
        if (type == 1) {
            /* BL T1 - 25-bit signed range */
            int s  = (off2 >> 23) & 1;
            int i1 = (off2 >> 22) & 1;
            int i2 = (off2 >> 21) & 1;
            int imm10 = (off2 >> 11) & 0x3FF;
            int imm11 = off2 & 0x7FF;
            int j1 = (~(i1 ^ s)) & 1;
            int j2 = (~(i2 ^ s)) & 1;
            hi = 0xF000u | (unsigned)(s << 10) | (unsigned)imm10;
            lo = 0xD000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
        } else if (type == 2) {
            /* B.W T4 - 25-bit signed range */
            int s  = (off2 >> 23) & 1;
            int i1 = (off2 >> 22) & 1;
            int i2 = (off2 >> 21) & 1;
            int imm10 = (off2 >> 11) & 0x3FF;
            int imm11 = off2 & 0x7FF;
            int j1 = (~(i1 ^ s)) & 1;
            int j2 = (~(i2 ^ s)) & 1;
            hi = 0xF000u | (unsigned)(s << 10) | (unsigned)imm10;
            lo = 0x9000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
        } else {
            /* B<cond>.W T3 - 21-bit signed range; preserve cond from existing hi word */
            unsigned old_hi = (unsigned)code[pc] | ((unsigned)code[pc + 1] << 8);
            int cond = (old_hi >> 6) & 0xF;
            int s  = (off2 >> 19) & 1;
            int i1 = (off2 >> 18) & 1;
            int i2 = (off2 >> 17) & 1;
            int imm6  = (off2 >> 11) & 0x3F;
            int imm11 = off2 & 0x7FF;
            hi = 0xF000u | (unsigned)(s << 10) | (unsigned)(cond << 6) | (unsigned)imm6;
            lo = 0x8000u | (unsigned)(i1 << 13) | (unsigned)(i2 << 11) | (unsigned)imm11;
        }
        code[pc]     = (unsigned char)(hi & 0xFF);
        code[pc + 1] = (unsigned char)((hi >> 8) & 0xFF);
        code[pc + 2] = (unsigned char)(lo & 0xFF);
        code[pc + 3] = (unsigned char)((lo >> 8) & 0xFF);
    }
}

/* === Cortex-M vector table generation ===
 * Generates a vector table at the start of the linked binary. Each entry is
 * a 4-byte little-endian address. Entry 0 = initial SP; entries 1-15 =
 * system exception handlers; entries 16+ = peripheral IRQ handlers.
 * Handler addresses are resolved from the global symbol table as
 * flash_base + symbol_offset, with the Thumb bit (bit 0) set: on exception
 * entry the core loads EPSR.T from that bit, so an even address makes a
 * Cortex-M core take an INVSTATE UsageFault instead of running the handler.
 * Unresolved entries are filled with Default_Handler (if defined) or 0.
 *
 * NOTE: arm_link() in backend_arm.c builds the same table for the
 * single-file path; both must keep the Thumb bit. */

#define LD_VECTOR_TABLE_ENTRIES 256  /* 1024 bytes; covers STM32F4/H7 */

static const char *g_ld_vector_names[] = {
    NULL,                 /* 0: initial SP */
    "Reset_Handler",      /* 1 */
    "NMI_Handler",        /* 2 */
    "HardFault_Handler",  /* 3 */
    "MemManage_Handler",  /* 4 */
    "BusFault_Handler",   /* 5 */
    "UsageFault_Handler", /* 6 */
    NULL, NULL, NULL, NULL, /* 7-10: reserved */
    "SVC_Handler",        /* 11 */
    "DebugMon_Handler",   /* 12 */
    NULL,                 /* 13: reserved */
    "PendSV_Handler",     /* 14 */
    "SysTick_Handler"     /* 15 */
};
#define LD_SYSTEM_VECTOR_COUNT 16

/* Write a 4-byte LE address into out_buf[off]. */
static void ld_write_word(unsigned char *buf, int off, unsigned val) {
    buf[off]     = (unsigned char)(val & 0xFF);
    buf[off + 1] = (unsigned char)((val >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((val >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((val >> 24) & 0xFF);
}

/* Generate vector table at out_buf[0..vt_size-1].
 * Handler addresses = flash_base + symbol_offset (symbols already adjusted).
 * Returns vt_size in bytes (LD_VECTOR_TABLE_ENTRIES * 4). */
static int ld_build_vector_table(unsigned char *out_buf,
                                  unsigned flash_base,
                                  unsigned initial_sp) {
    int i;
    int vt_size = LD_VECTOR_TABLE_ENTRIES * 4;
    unsigned default_addr = 0;

    /* Find Default_Handler for filling unresolved entries */
    {
        int idx = ld_find_global("Default_Handler");
        if (idx >= 0)   /* bit 0 = Thumb bit; 0 stays 0 ("no handler") */
            default_addr = (flash_base + (unsigned)g_ld_syms[idx].offset) | 1u;
    }

    /* Entry 0: initial SP */
    ld_write_word(out_buf, 0, initial_sp);

    /* Fill entries 1..N with Default_Handler (or 0) */
    for (i = 1; i < LD_VECTOR_TABLE_ENTRIES; i++)
        ld_write_word(out_buf, i * 4, default_addr);

    /* Fill in known system exception handlers */
    for (i = 1; i < LD_SYSTEM_VECTOR_COUNT; i++) {
        if (g_ld_vector_names[i]) {
            int idx = ld_find_global(g_ld_vector_names[i]);
            if (idx >= 0) {
                unsigned addr = (flash_base + (unsigned)g_ld_syms[idx].offset) | 1u;
                ld_write_word(out_buf, i * 4, addr);
            }
        }
    }
    return vt_size;
}

/* Append one ARKOBJ2 symbol-table entry and return the new length:
 * name_len(4) + name(name_len) + offset(4) + type(1) + is_weak(1). */
static int obj_put_sym(unsigned char *buf, int ol, const char *nm,
                       unsigned off, int type, int weak) {
    int nl = ark_strlen(nm) + 1;
    int k;
    buf[ol++] = (unsigned char)(nl & 0xFF);
    buf[ol++] = (unsigned char)((nl >> 8) & 0xFF);
    buf[ol++] = (unsigned char)((nl >> 16) & 0xFF);
    buf[ol++] = (unsigned char)((nl >> 24) & 0xFF);
    for (k = 0; k < nl; k++) buf[ol++] = (unsigned char)nm[k];
    buf[ol++] = (unsigned char)(off & 0xFF);
    buf[ol++] = (unsigned char)((off >> 8) & 0xFF);
    buf[ol++] = (unsigned char)((off >> 16) & 0xFF);
    buf[ol++] = (unsigned char)((off >> 24) & 0xFF);
    buf[ol++] = (unsigned char)type;
    buf[ol++] = (unsigned char)weak;
    return ol;
}

/* Reserved symbol names that describe the object's SRAM footprint rather than
 * a code label. The -link stage consumes them instead of adding them to the
 * resolution table (their "offset" field is an absolute address, not an
 * offset relative to the object's code base). */
#define OBJ_RAM_START_SYM "__arkcc_ram_start"
#define OBJ_RAM_END_SYM   "__arkcc_ram_end"
#define OBJ_SYM_TYPE_RAM  2
/* --reloc-text: the base this object's absolute addresses were computed
 * against.  Stored in the offset field like the two RAM descriptors above, so
 * an object built without the feature is byte-identical to a 0.7.x object. */
#define OBJ_TEXT_BASE_SYM "__arkcc_text_base"
#define OBJ_SYM_TYPE_TEXT 3
/* riscv32: an object set identity tag.  The ARM and RV relocations share one
 * table, but their type numbers mean different things (13/14 are RV auipc /
 * abs32; ARM's 4/5/10/11 are MOVW/MOVT and Thumb branches), so linking a
 * riscv32 object through the ARM path used to "succeed" by reading the RV
 * relocations as Thumb branches -- a corrupt image with exit code 0.  The
 * driver now demands the tag on every riscv32 object and rejects it on an ARM
 * link.  The offset field is unused (0). */
#define OBJ_ISA_RV32_SYM "__arkcc_isa_riscv32"
#define OBJ_SYM_TYPE_ISA 5

/* Write an ARKOBJ2 object file: "ARKOBJ2\n" + u32 code size + code body +
 * u32 symbol count + symbols + u32 reloc count + relocs (readers live in the
 * -link implementation below). Shared by the C front end (-c) and by direct
 * assembly input (.s/.S/.asm with -c), so that the two paths cannot drift.
 *
 * asm_len is the length of the assembly text in g_asm; bin_len is the length
 * of the assembled machine code in g_code. The "c" backend has no assembly
 * step and stores the text itself, so bin_len is unused there.
 * The caller must have run be->assemble() for arm/x86/riscv32 before calling
 * this: riscv32 grows a real assembler in 0.8.x, so its object body is machine
 * code at the image layout its own -link path expects.
 */
static int write_ark_obj(const ArkBackend *be, const ArkOptions *opts,
                         int asm_len, int bin_len) {
    char outpath[512];
    const char *target;
    static unsigned char obj_buf[ARK_OBJ_BUF_SIZE] __attribute__((section(".ark_buf")));
    int ol = 0;
    int is_obj_backend = ark_streq(be->name, "arm") || ark_streq(be->name, "x86") ||
                         ark_streq(be->name, "riscv32");
    int code_len = is_obj_backend ? bin_len : asm_len;
    int isa_extra = ark_streq(be->name, "riscv32") ? 1 : 0;
    /* SRAM window of this unit, only meaningful for the ARM backend. */
    unsigned ram_start = 0, ram_end = 0;
    int ram_extra = 0;

    if (opts->output_path) target = opts->output_path;
    else {
        if (derive_output(opts->input_path, outpath, sizeof(outpath), ".ark_obj") != 0)
            return ARK_ERR_ARG;
        target = outpath;
    }

    /* Write extended header "ARKOBJ2\n" (8 bytes) */
    {
        const char *hdr = "ARKOBJ2\n";
        int hk;
        for (hk = 0; hdr[hk]; hk++) obj_buf[ol++] = (unsigned char)hdr[hk];
    }

    /* Write code size (4 bytes, little-endian) */
    {
        int cs = code_len;
        obj_buf[ol++] = (unsigned char)(cs & 0xFF);
        obj_buf[ol++] = (unsigned char)((cs >> 8) & 0xFF);
        obj_buf[ol++] = (unsigned char)((cs >> 16) & 0xFF);
        obj_buf[ol++] = (unsigned char)((cs >> 24) & 0xFF);
    }

    /* Write code body */
    if (code_len + ol > (int)sizeof(obj_buf)) {
        diag_tool(DIAG_SEV_ERROR, target, "object output too large for buffer", 0,0,0,0,0,0);
        return ARK_ERR_IO;
    }
    {
        int k;
        const unsigned char *src = is_obj_backend ? g_code : (const unsigned char *)g_asm;
        for (k = 0; k < code_len; k++) obj_buf[ol++] = src[k];
    }

    /* Symbol/reloc getters: ARM and x86 backends both emit ARKOBJ2 tables. */
    int   (*p_sym_count)(void)       = 0;
    const char *(*p_sym_name)(int)   = 0;
    int   (*p_sym_offset)(int)       = 0;
    int   (*p_sym_type)(int)         = 0;
    int   (*p_sym_weak)(int)          = 0;
    int   (*p_reloc_count)(void)     = 0;
    const char *(*p_reloc_name)(int) = 0;
    int   (*p_reloc_offset)(int)     = 0;
    int   (*p_reloc_type)(int)       = 0;
    unsigned (*p_text_base)(void)    = 0;
    int   text_extra                 = 0;
    unsigned text_base               = 0;
    if (ark_streq(be->name, "arm")) {
        extern unsigned arm_get_text_base(void);
        extern int   arm_get_symbol_count(void);
        extern const char *arm_get_symbol_name(int idx);
        extern int   arm_get_symbol_offset(int idx);
        extern int   arm_get_symbol_type(int idx);
        extern int   arm_get_symbol_weak(int idx);
        extern int   arm_get_reloc_count(void);
        extern const char *arm_get_reloc_name(int idx);
        extern int   arm_get_reloc_offset(int idx);
        extern int   arm_get_reloc_type(int idx);
        p_sym_count   = arm_get_symbol_count;
        p_sym_name    = arm_get_symbol_name;
        p_sym_offset  = arm_get_symbol_offset;
        p_sym_type    = arm_get_symbol_type;
        p_sym_weak    = arm_get_symbol_weak;
        {
            extern unsigned arm_get_ram_start(void);
            extern unsigned arm_get_ram_end(void);
            ram_start = arm_get_ram_start();
            ram_end   = arm_get_ram_end();
            if (ram_end > ram_start) ram_extra = 2;
        }
        p_reloc_count = arm_get_reloc_count;
        p_reloc_name  = arm_get_reloc_name;
        p_reloc_offset= arm_get_reloc_offset;
        p_reloc_type  = arm_get_reloc_type;
        p_text_base   = arm_get_text_base;
    } else if (ark_streq(be->name, "x86")) {
#if ARKCC_BACKEND_X86_64
        extern int   x86_get_symbol_count(void);
        extern const char *x86_get_symbol_name(int idx);
        extern int   x86_get_symbol_offset(int idx);
        extern int   x86_get_symbol_type(int idx);
        extern int   x86_get_symbol_weak(int idx);
        extern int   x86_get_reloc_count(void);
        extern const char *x86_get_reloc_name(int idx);
        extern int   x86_get_reloc_offset(int idx);
        extern int   x86_get_reloc_type(int idx);
        p_sym_count   = x86_get_symbol_count;
        p_sym_name    = x86_get_symbol_name;
        p_sym_offset  = x86_get_symbol_offset;
        p_sym_type    = x86_get_symbol_type;
        p_sym_weak    = x86_get_symbol_weak;
        p_reloc_count = x86_get_reloc_count;
        p_reloc_name  = x86_get_reloc_name;
        p_reloc_offset= x86_get_reloc_offset;
        p_reloc_type  = x86_get_reloc_type;
#endif
    }
#if ARKCC_BACKEND_RISCV32
    else if (ark_streq(be->name, "riscv32")) {
        /* This backend implements the getters in backend_riscv32.c rather than
         * declaring prototypes in backend.h -- they exist only for this one
         * caller, and only the object path asks for them. */
        extern int   rv_get_symbol_count(void);
        extern const char *rv_get_symbol_name(int idx);
        extern int   rv_get_symbol_offset(int idx);
        extern int   rv_get_symbol_type(int idx);
        extern int   rv_get_symbol_weak(int idx);
        extern int   rv_get_reloc_count(void);
        extern const char *rv_get_reloc_name(int idx);
        extern int   rv_get_reloc_offset(int idx);
        extern int   rv_get_reloc_type(int idx);
        p_sym_count   = rv_get_symbol_count;
        p_sym_name    = rv_get_symbol_name;
        p_sym_offset  = rv_get_symbol_offset;
        p_sym_type    = rv_get_symbol_type;
        p_sym_weak    = rv_get_symbol_weak;
        p_reloc_count = rv_get_reloc_count;
        p_reloc_name  = rv_get_reloc_name;
        p_reloc_offset= rv_get_reloc_offset;
        p_reloc_type  = rv_get_reloc_type;
    }
#endif /* ARKCC_BACKEND_RISCV32 */

    /* --reloc-text: one extra descriptor telling the linker which base every
     * absolute address in this object was computed against.  It has to be
     * decided here, after the backend vtable above is filled in -- asking for
     * p_text_base before it was assigned silently produced objects without the
     * descriptor, which the linker then refused (correctly, but far too late).
     * Emitted only when the feature is on, so an object built without it stays
     * byte-identical to 0.7.x. */
    if (g_config.enable_reloc_text && p_text_base) {
        text_base  = p_text_base();
        text_extra = 1;
    }

    /* Write symbol table for object backends (ARM/x86) */
    if (p_sym_count) {
        int sc = p_sym_count();
        int si;
        /* The RAM-footprint descriptor is appended after the real symbols so
         * that older readers that ignore trailing symbols still work. */
        int sc_total = sc + ram_extra + text_extra + isa_extra;
        /* symbol count (4 bytes LE) */
        obj_buf[ol++] = (unsigned char)(sc_total & 0xFF);
        obj_buf[ol++] = (unsigned char)((sc_total >> 8) & 0xFF);
        obj_buf[ol++] = (unsigned char)((sc_total >> 16) & 0xFF);
        obj_buf[ol++] = (unsigned char)((sc_total >> 24) & 0xFF);
        for (si = 0; si < sc; si++) {
            const char *sn = p_sym_name(si);
            int nl = ark_strlen(sn) + 1; /* include null terminator */
            int off = p_sym_offset(si);
            int tp = p_sym_type(si);
            int wk = p_sym_weak(si);
            /* name length (4 bytes LE) */
            obj_buf[ol++] = (unsigned char)(nl & 0xFF);
            obj_buf[ol++] = (unsigned char)((nl >> 8) & 0xFF);
            obj_buf[ol++] = (unsigned char)((nl >> 16) & 0xFF);
            obj_buf[ol++] = (unsigned char)((nl >> 24) & 0xFF);
            /* name bytes */
            {
                int k;
                for (k = 0; k < nl; k++) obj_buf[ol++] = (unsigned char)sn[k];
            }
            /* offset (4 bytes LE) */
            obj_buf[ol++] = (unsigned char)(off & 0xFF);
            obj_buf[ol++] = (unsigned char)((off >> 8) & 0xFF);
            obj_buf[ol++] = (unsigned char)((off >> 16) & 0xFF);
            obj_buf[ol++] = (unsigned char)((off >> 24) & 0xFF);
            /* type (1 byte) */
            obj_buf[ol++] = (unsigned char)tp;
            /* is_weak (1 byte): 1=weak symbol, 0=strong */
            obj_buf[ol++] = (unsigned char)wk;
        }
        if (ram_extra) {
            ol = obj_put_sym(obj_buf, ol, OBJ_RAM_START_SYM, ram_start,
                             OBJ_SYM_TYPE_RAM, 0);
            ol = obj_put_sym(obj_buf, ol, OBJ_RAM_END_SYM, ram_end,
                             OBJ_SYM_TYPE_RAM, 0);
        }
        if (text_extra) {
            ol = obj_put_sym(obj_buf, ol, OBJ_TEXT_BASE_SYM, (int)text_base,
                             OBJ_SYM_TYPE_TEXT, 0);
        }
        if (isa_extra) {
            ol = obj_put_sym(obj_buf, ol, OBJ_ISA_RV32_SYM, 0,
                             OBJ_SYM_TYPE_ISA, 0);
        }
    } else {
        /* Non-object backend: write 0 symbol count */
        obj_buf[ol++] = 0; obj_buf[ol++] = 0; obj_buf[ol++] = 0; obj_buf[ol++] = 0;
    }

    /* Bug-A2: Write relocation table for ARM backend.
     * Format: reloc_count (4 bytes LE) + for each reloc:
     *   name_len (4 bytes LE) + name (name_len bytes) + offset (4 bytes LE) + type (1 byte)
     * This goes AFTER the symbol table. Existing readers that stop
     * after the symbol table (like build_ark_test_fw.ps1) are unaffected. */
    if (p_reloc_count) {
        int rc = p_reloc_count();
        int ri;
        /* reloc count (4 bytes LE) */
        obj_buf[ol++] = (unsigned char)(rc & 0xFF);
        obj_buf[ol++] = (unsigned char)((rc >> 8) & 0xFF);
        obj_buf[ol++] = (unsigned char)((rc >> 16) & 0xFF);
        obj_buf[ol++] = (unsigned char)((rc >> 24) & 0xFF);
        for (ri = 0; ri < rc; ri++) {
            const char *rn = p_reloc_name(ri);
            int nl = ark_strlen(rn) + 1;
            int off = p_reloc_offset(ri);
            int tp = p_reloc_type(ri);
            /* name length (4 bytes LE) */
            obj_buf[ol++] = (unsigned char)(nl & 0xFF);
            obj_buf[ol++] = (unsigned char)((nl >> 8) & 0xFF);
            obj_buf[ol++] = (unsigned char)((nl >> 16) & 0xFF);
            obj_buf[ol++] = (unsigned char)((nl >> 24) & 0xFF);
            /* name bytes */
            {
                int k;
                for (k = 0; k < nl; k++) obj_buf[ol++] = (unsigned char)rn[k];
            }
            /* offset (4 bytes LE) */
            obj_buf[ol++] = (unsigned char)(off & 0xFF);
            obj_buf[ol++] = (unsigned char)((off >> 8) & 0xFF);
            obj_buf[ol++] = (unsigned char)((off >> 16) & 0xFF);
            obj_buf[ol++] = (unsigned char)((off >> 24) & 0xFF);
            /* type (1 byte) */
            obj_buf[ol++] = (unsigned char)tp;
        }
    }

    if (fs_write_file(target, obj_buf, ol) != ol)
        return ARK_ERR_IO;
    {
        if (opts->verbose) ark_log("[arkcc] wrote object '%s' (%d bytes code, %d syms, %d relocs)\n",
            target, code_len,
            p_sym_count   ? p_sym_count()   : 0,
            p_reloc_count ? p_reloc_count() : 0);
    }
    return ARK_OK;

}
#if ARKCC_FEATURE_BUILD_TIME
/* 把"自 1970-01-01 起的秒数"换算成 C99 要求的两个字符串。
 *
 * 为什么自己算而不是调 gmtime：板端没有 libc，也不想让可复现构建依赖时区数据库。
 * 算法是 Howard Hinnant 的 days_from_civil 逆变换（公历、无闰年例外），
 * 只用整数算术，任何平台结果一致 —— 这正是可复现构建要的性质。
 * 注意 __DATE__ 的格式是 "Mmm dd yyyy"，日不足两位时用空格补（C99 6.10.8.1），
 * 所以 "Nov  6 2026" 里有两个空格。 */
static void format_epoch_utc(long long epoch, char *date_out, char *time_out) {
    static const char *mon[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec" };
    long long days = epoch / 86400;
    long long secs = epoch % 86400;
    long long z, era, doe, yoe, y, doy, mp, d, m;
    int hh = (int)(secs / 3600);
    int mm = (int)((secs % 3600) / 60);
    int ss = (int)(secs % 60);

    /* days_from_civil 的逆：把"自 1970-01-01 的天数"还原成年月日 */
    z   = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = doy - (153 * mp + 2) / 5 + 1;
    m   = mp + (mp < 10 ? 3 : -9);
    y  += (m <= 2);

    /* date_out: "Mmm dd yyyy"（11 字符 + NUL） */
    {
        int i;
        const char *mn = mon[(int)m - 1];
        date_out[0] = mn[0]; date_out[1] = mn[1]; date_out[2] = mn[2];
        date_out[3] = ' ';
        date_out[4] = (char)('0' + (int)(d / 10));
        date_out[5] = (char)('0' + (int)(d % 10));
        date_out[6] = ' ';
        date_out[7] = (char)('0' + (int)((y / 1000) % 10));
        date_out[8] = (char)('0' + (int)((y / 100) % 10));
        date_out[9] = (char)('0' + (int)((y / 10) % 10));
        date_out[10] = (char)('0' + (int)(y % 10));
        date_out[11] = 0;
        /* C99：日若为个位，用空格代替前导 0 */
        if (d < 10) date_out[4] = ' ';
        (void)i;
    }
    time_out[0] = (char)('0' + hh / 10);
    time_out[1] = (char)('0' + hh % 10);
    time_out[2] = ':';
    time_out[3] = (char)('0' + mm / 10);
    time_out[4] = (char)('0' + mm % 10);
    time_out[5] = ':';
    time_out[6] = (char)('0' + ss / 10);
    time_out[7] = (char)('0' + ss % 10);
    time_out[8] = 0;
}
#endif

int ark_compile(const ArkOptions *opts) {
    int src_len;
    Lexer lex;
    AstNode *ast;
    int sema_errs;
    IrProgram *ir;
    char *preprocessed;
    const ArkBackend *be;
    int asm_len;
    int code_size;
    int i;

    if (!opts) return ARK_ERR_ARG;
    if (opts->list_backends) { ark_backend_list(); return ARK_OK; }

#if ARKCC_FEATURE_SCATTER
    /* --scatter: the ARM backend and the linker both read the layout from
     * g_config alone, so an embedding that fills only ArkOptions::scatter_file
     * gets the built-in addresses (0x08000000 / 0x20020000) and emits an image
     * that cannot run where it is loaded -- silently, which is the worst kind.
     * The bridge therefore sits at the very top of ark_compile(), before the
     * -link branch returns: with it placed next to the compile-stage handling
     * the linker never saw it, and `cc -link ... --scatter x.sct` on the H743
     * produced a vector table saying SP=0x20020000 / entry=0x08001250 while
     * the shell had printed the scatter name. */
    if ((opts->scatter_file != 0) && (opts->scatter_file[0] != 0)) {
        if ((g_config.scatter_file == 0) || (g_config.scatter_file[0] == 0)) {
            g_config.scatter_file = opts->scatter_file;
        }
    }
#endif

    /* -link mode: merge multiple .ark_obj files.
     * ARKOBJ2 binary objects are linked via the binary ARM linker (code merge +
     * symbol resolution with weak-symbol precedence + relocation patching).
     * Legacy text objects are concatenated (C backend). */
    if (opts->link_mode) {
        /* Sized like the output buffer, not 64 KB: an object bigger than the
         * read buffer used to be truncated silently and the linker then copied
         * the code size it had read from the header out of whatever followed
         * the buffer, producing a corrupt image with exit code 0 (and a
         * segfault under -v). */
#if ARKCC_PROFILE == ARKCC_PROFILE_STM32_MIN
        static char buf[ARK_LINK_BUF_SIZE] __attribute__((section(".ark_buf")));
        static unsigned char out_buf[ARK_LINK_BUF_SIZE] __attribute__((section(".ark_buf")));
#else
        /* Host: carve the two image-sized buffers out of the arena on first use
         * rather than into .bss. 8 MB each as static arrays put 16 MB of zeroes
         * into the executable; the pool is 64 MB and the linker owns it
         * exclusively after the reset below, so allocation is equivalent at run
         * time and costs nothing at rest.  `once` keeps a second -link in the
         * same process (not a thing today, but cheap to keep honest) from
         * allocating twice. */
        static char *buf;
        static unsigned char *out_buf;
        if (!buf) {
            buf = (char *)mem_pool_alloc(ARK_LINK_BUF_SIZE);
            out_buf = (unsigned char *)mem_pool_alloc(ARK_LINK_BUF_SIZE);
        }
#endif
        int out_len = 0;
        int is_binary = 0;
        const char *out_path;
        int li;
        /* Which relocations the object set speaks.  Resolved through the
         * backend table so an alias (-target rv32) counts as riscv32 too.  The
         * object files themselves carry the answer as a descriptor symbol; the
         * two are cross-checked below, because reading RV relocations through
         * the ARM patcher produces a corrupt image with exit code 0. */
        const ArkBackend *link_be = opts->target ? ark_backend_find(opts->target) : 0;
        int is_rv_link = (link_be && ark_streq(link_be->name, "riscv32")) ? 1 : 0;
        int ld_isa_rv32 = 0;

        /* The link phase allocates its symbol table, relocation table and
         * image buffer from the same bump pool the compiler uses, and this
         * branch returns before the mem_pool_reset() further down.  On the
         * host that was invisible: every run is a fresh process, so the pool
         * always started at zero.  On the board `cc --pack` runs several
         * compiles and links in one process, so the link inherited the peak
         * of the compile that preceded it and the 80 KB arena ran out at
         * link time (measured: 2 x ~450 B sources -> 'pass A aborted: memory
         * pool exhausted', pool 77400/81920 B, while the same link alone
         * needs 8.5 KB).  Nothing from the compile phase survives into the
         * linker -- it re-reads the objects -- so reset here is the fix.
         * Host reproductions cannot see this; the criterion has to come
         * from the board. */
        mem_pool_reset();

        /* Detect ARKOBJ2 binary format from the first input.
         * The probe reads through `buf` -- the same buffer the linker uses for
         * every object -- instead of a small header buffer. fs_read_file() is
         * documented as truncating, but a port is allowed to refuse instead
         * (the Ark RTOS port returns -1 with "File too large"), and a small
         * probe buffer therefore failed on every real object: a 2.5 KB .obj
         * does not fit in 16 bytes, is_binary stayed 0, and -link silently
         * concatenated objects as text until "link output too large". A failed
         * probe is a read error with a reason, never "not an object". */
        {
            int pn = fs_read_file(opts->link_inputs[0], buf, LD_BUF_SIZE);
            if (pn < 0) {
                diag_tool(DIAG_SEV_ERROR, opts->link_inputs[0],
                          "cannot read link input: %s", fs_last_error(),
                          0,0,0,0,0);
                return ARK_ERR_IO;
            }
            if (pn >= 8 && buf[0]=='A' && buf[1]=='R' &&
                buf[2]=='K' && buf[3]=='O' &&
                buf[4]=='B' && buf[5]=='J' &&
                buf[6]=='2' && buf[7]=='\n')
                is_binary = 1;
        }
        out_path = opts->output_path ? opts->output_path :
                   (is_binary ? "a.out.bin" : "a.out.c");
        g_ld_image = out_path;
        diag_set_main_file(out_path);

        if (is_binary) {
            /* Binary ARM linker */
            if (opts->link_input_count > LD_MAX_OBJS) {
                char b1[12], b2[12];
                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                          "%s objects were given but this arkcc links %s; raise "
                          "ARK_LINK_MAX_OBJS and relink (the per-object RAM "
                          "window and init body would otherwise be skipped)",
                          diag_num(b1, opts->link_input_count),
                          diag_num(b2, LD_MAX_OBJS), 0,0,0,0);
                return ARK_ERR_LINK;
            }
            g_ld_sym_count = 0;
            g_ld_reloc_count = 0;
            g_ld_ram_obj_count = 0;
            ld_rt_dup_count = 0;
            { int k; for (k = 0; k < LD_MAX_OBJS; k++) {
                  g_ld_ram[k].start = 0; g_ld_ram[k].end = 0;
                  g_ld_ram[k].have_start = 0; g_ld_ram[k].have_end = 0;
                  g_ld_data_delta[k] = 0; g_ld_init_body[k] = -1;
                  g_ld_code_base[k] = 0; g_ld_text_base[k] = 0;
                  g_ld_have_text_base[k] = 0; } }
            g_ld_data_top = 0;
            ld_table_overflow = 0;
            for (li = 0; li < opts->link_input_count; li++) {
                int n = fs_read_file(opts->link_inputs[li], buf, LD_BUF_SIZE);
                const unsigned char *up = (const unsigned char *)buf;
                int pos = 8;  /* skip "ARKOBJ2\n" header */
                int base;
                if (is_rv_link) {
                    /* A riscv32 object references everything PC-relative, so it
                     * can be placed anywhere; 16 bytes keeps the section
                     * alignment the assembler applied inside it meaningful, and
                     * zero padding keeps the image free of uninitialised bytes.
                     * ARM objects stay back-to-back, exactly as before. */
                    while ((out_len & 15) && out_len < LD_BUF_SIZE)
                        out_buf[out_len++] = 0;
                }
                base = out_len;
                if (li < LD_MAX_OBJS) g_ld_code_base[li] = base;
                int code_sz, sym_cnt, reloc_cnt, si, ri;
                if (n < 0) {
                    diag_tool(DIAG_SEV_ERROR, opts->link_inputs[li],
                              "cannot read object file", 0,0,0,0,0,0);
                    return ARK_ERR_IO;
                }
                {
                    /* Refuse a partially read object instead of linking with
                     * whatever happened to sit after the buffer. */
                    char b1[12], b2[12];
                    int fsz = fs_file_size(opts->link_inputs[li]);
                    if (fsz < 0 || fsz > n) {
                        diag_tool(DIAG_SEV_ERROR, opts->link_inputs[li],
                                  "object file is %s bytes but only %s could be "
                                  "read; raise ARK_LINK_BUF_SIZE and relink",
                                  diag_num(b1, fsz), diag_num(b2, n), 0,0,0,0);
                        return ARK_ERR_IO;
                    }
                }
                /* Code size (4 bytes LE) + code body */
                code_sz = rd_le32(up + pos); pos += 4;
                if (out_len + code_sz > LD_BUF_SIZE) {
                    char b1[12], b2[12], b3[12];
                    diag_tool(DIAG_SEV_ERROR, g_ld_image,
                              "link output too large: %s bytes placed + %s from the object "
                              "does not fit the %s-byte ARK_LINK_BUF_SIZE",
                              diag_num(b1, out_len), diag_num(b2, code_sz),
                              diag_num(b3, LD_BUF_SIZE), 0,0,0);
                    return ARK_ERR_IO;
                }
                { int k; for (k = 0; k < code_sz; k++) out_buf[out_len++] = up[pos + k]; }
                pos += code_sz;
                if (opts->verbose) {
                    /* Where each object landed.  A flat image carries no symbol
                     * table, so this line plus ARKCC_DUMP_SYMS is the only way
                     * to turn an object-relative offset into an address. */
                    ark_log("[link] object %d: base 0x%X, %d bytes\n",
                            li, base, code_sz);
                }
                /* Symbol table: count + entries
                 * Each entry: name_len(4) + name(name_len) + offset(4) + type(1) + is_weak(1) */
                sym_cnt = rd_le32(up + pos); pos += 4;
                for (si = 0; si < sym_cnt; si++) {
                    char sym_name[128];
                    int name_len, sym_off, sym_weak, k;
                    name_len = rd_le32(up + pos); pos += 4;
                    for (k = 0; k < name_len && k < (int)sizeof(sym_name) - 1; k++)
                        sym_name[k] = (char)up[pos + k];
                    sym_name[k] = 0;
                    pos += name_len;
                    sym_off = rd_le32(up + pos); pos += 4;
                    pos += 1;           /* type (unused by linker) */
                    sym_weak = up[pos]; pos += 1;
                    /* RAM footprint descriptors: the offset field is an
                     * absolute address, so it must NOT get the object code
                     * base added, and it is not a resolvable label. */
                    /* --reloc-text descriptor: an absolute base, not a label
                     * and not an offset to which the code base applies. */
                    if (ark_streq(sym_name, OBJ_TEXT_BASE_SYM)) {
                        if (li < LD_MAX_OBJS) {
                            g_ld_text_base[li] = (unsigned)sym_off;
                            g_ld_have_text_base[li] = 1;
                        }
                        continue;
                    }
                    if (ark_streq(sym_name, OBJ_ISA_RV32_SYM)) {
                        ld_isa_rv32 = 1;
                        continue;
                    }
                    if (ark_streq(sym_name, OBJ_RAM_START_SYM)) {
                        if (li < LD_MAX_OBJS) {
                            g_ld_ram[li].start = (unsigned)sym_off;
                            g_ld_ram[li].have_start = 1;
                        }
                        continue;
                    }
                    if (ark_streq(sym_name, "__arkcc_init_globals") &&
                        li < LD_MAX_OBJS && g_ld_init_body[li] < 0) {
                        /* --reloc-data: remember where *this* object's init
                         * body is.  ld_add_sym below keeps only the first
                         * definition of the shared name, so the rest would
                         * never run (see the chain emitted later). */
                        g_ld_init_body[li] = base + sym_off;
                    }
                    if (ark_streq(sym_name, OBJ_RAM_END_SYM)) {
                        if (li < LD_MAX_OBJS) {
                            g_ld_ram[li].end = (unsigned)sym_off;
                            g_ld_ram[li].have_end = 1;
                        }
                        continue;
                    }
                    ld_add_sym(sym_name, base + sym_off, sym_weak);
                }
                /* Relocation table: count + entries
                 * Each entry: name_len(4) + name(name_len) + offset(4) + type(1) */
                reloc_cnt = rd_le32(up + pos); pos += 4;
                for (ri = 0; ri < reloc_cnt; ri++) {
                    char reloc_name[128];
                    int name_len, reloc_off, reloc_type, k;
                    name_len = rd_le32(up + pos); pos += 4;
                    for (k = 0; k < name_len && k < (int)sizeof(reloc_name) - 1; k++)
                        reloc_name[k] = (char)up[pos + k];
                    reloc_name[k] = 0;
                    pos += name_len;
                    reloc_off = rd_le32(up + pos); pos += 4;
                    reloc_type = up[pos]; pos += 1;
                    ld_add_reloc(reloc_name, base + reloc_off, reloc_type, li);
                }
            }
            if (ld_rt_dup_count > 0) {
                ark_log("[link] note: %d duplicate compiler-runtime symbols "
                        "merged (first definition kept)\n", ld_rt_dup_count);
            }
            /* The two object kinds need different relocation semantics, so a
             * set that does not match the selected target is refused here
             * rather than patched with the wrong reader. */
            if (is_rv_link && !ld_isa_rv32) {
                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                          "-target riscv32 was given but this object set carries no "
                          "%s descriptor, so it was produced for another "
                          "architecture; link it with that target instead",
                          OBJ_ISA_RV32_SYM, 0,0,0,0,0);
                return ARK_ERR_LINK;
            }
            if (!is_rv_link && ld_isa_rv32) {
                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                          "these objects were compiled for riscv32 but the link "
                          "target is not; pass -target riscv32 (the riscv32 "
                          "relocations mean something else to the ARM patcher, so "
                          "linking them here would produce a corrupt image)",
                          0,0,0,0,0,0);
                return ARK_ERR_LINK;
            }
            /* === Multi-object SRAM overlap check ===
             * -link concatenates code sections and resolves code relocations,
             * but it does not relocate .data/.bss: every object was laid out
             * from the same SRAM base (default 0x20000000, or the +RW/+ZI
             * region of the shared scatter file). Linking two objects that
             * both declare RAM objects therefore produces an image where the
             * two sets of globals alias the same addresses. That used to be
             * silent; fail loudly instead of shipping a wrong image. */
            if (g_config.enable_reloc_data) {
                /* --reloc-data: instead of refusing the link, place each
                 * object's data after the previous one and rewrite the marked
                 * addresses.  The windows recorded in g_ld_ram are updated to
                 * the new bases, so the overlap check below still runs and now
                 * passes on genuine layout, not on a bypass. */
                if (ld_relocate_data(out_buf, out_len) != 0) return ARK_ERR_LINK;
            }
            {
                int obj_a, obj_b;
                for (obj_a = 0; obj_a < LD_MAX_OBJS; obj_a++) {
                    if (!g_ld_ram[obj_a].have_start || !g_ld_ram[obj_a].have_end)
                        continue;
                    for (obj_b = obj_a + 1; obj_b < LD_MAX_OBJS; obj_b++) {
                        unsigned lo, hi;
                        if (!g_ld_ram[obj_b].have_start || !g_ld_ram[obj_b].have_end)
                            continue;
                        lo = g_ld_ram[obj_a].start > g_ld_ram[obj_b].start ?
                             g_ld_ram[obj_a].start : g_ld_ram[obj_b].start;
                        hi = g_ld_ram[obj_a].end < g_ld_ram[obj_b].end ?
                             g_ld_ram[obj_a].end : g_ld_ram[obj_b].end;
                        if (lo < hi) {
                            char b1[12], b2[12], b3[12], b4[12], b5[12], b6[12];
                            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                      "SRAM overlap between objects #%s and #%s: "
                                      "both place .data/.bss in %s..%s; the "
                                      "overlapping range is %s..%s",
                                      diag_num(b1, obj_a), diag_num(b2, obj_b),
                                      ld_hex(b3, g_ld_ram[obj_a].start),
                                      ld_hex(b4, g_ld_ram[obj_a].end),
                                      ld_hex(b5, lo), ld_hex(b6, hi));
                            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                      "-link does not relocate data sections, so "
                                      "globals from different objects would "
                                      "silently share one address; link a single "
                                      "object or merge the translation units",
                                      0,0,0,0,0,0);
                            return ARK_ERR_LINK;
                        }
                    }
                }
            }
            /* === Scatter-aware linking: fill the hole, never shift ===
             * If a scatter file (.sct or .ld) is provided, parse it to derive
             * flash_base, the RW window and initial_sp.
             *
             * arkc bakes absolute addresses into the instruction stream
             * (movw/movt pairs and literal slots); only external symbols become
             * relocations.  Prepending a 0x400-byte table and shifting the
             * image therefore moved the code but not the constants baked into
             * it: every string literal stayed 0x400 too low.  Measured, not
             * guessed -- "ARKMAKE: link ok" sat at payload +0xA1E while the
             * code held 0x08000000, and the app loaded, printed nothing and
             * exited.
             *
             * The fix is to stop moving anything: the object linked first is
             * compiled with --vector-table, so it reserves a 0x400-byte hole at
             * the front and bases its own addresses at base+0x400.  The linker
             * writes the table INTO that hole.  Code does not move, so every
             * baked address stays correct.
             *
             * That is also why every object of a multi-object link is compiled
             * against its own base (see cc_pack in the Ark RTOS port): with
             * baked addresses, an object's base is not something the linker can
             * choose afterwards. */
            {
                unsigned flash_base = 0x08000000u;
                unsigned initial_sp = 0x20020000u;
                unsigned ram_lo = 0x20000000u;
                unsigned ram_hi = 0u;
                int have_ram_win = 0;
#if ARKCC_FEATURE_SCATTER
                {
                    ScatterDesc ld_scatter;
                    ld_scatter.load_regions = 0;
                    ld_scatter.error_count = 0;
                    if (g_config.scatter_file && g_config.scatter_file[0] &&
                        scatter_parse_file(g_config.scatter_file, &ld_scatter) == 0) {
                        flash_base = scatter_get_flash_base(&ld_scatter);
                        {   ScatterExecRegion *er_rw = scatter_find_er_by_attr(&ld_scatter, SCATTER_ATTR_RW);
                            if (er_rw && er_rw->size > 0) {
                                initial_sp = er_rw->load_addr + er_rw->size;
                                ram_lo = er_rw->load_addr;
                                ram_hi = initial_sp;
                                have_ram_win = 1;
                            }
                        }
                        scatter_free(&ld_scatter);
                    } else if (g_config.linker_script && g_config.linker_script[0] &&
                               linker_script_parse_file(g_config.linker_script, &ld_scatter) == 0) {
                        flash_base = scatter_get_flash_base(&ld_scatter);
                        {   ScatterExecRegion *er_rw = scatter_find_er_by_attr(&ld_scatter, SCATTER_ATTR_RW);
                            if (er_rw && er_rw->size > 0) {
                                initial_sp = er_rw->load_addr + er_rw->size;
                                ram_lo = er_rw->load_addr;
                                ram_hi = initial_sp;
                                have_ram_win = 1;
                            }
                        }
                        scatter_free(&ld_scatter);
                    }
                }
#endif
                if (g_config.enable_reloc_data && g_ld_data_top != 0 &&
                    g_ld_data_top > initial_sp) {
                    char b1[12], b2[12];
                    diag_tool(DIAG_SEV_ERROR, g_ld_image,
                              "relocated .data/.bss ends at %s but the initial "
                              "stack pointer is %s: the link set needs more SRAM "
                              "than the scatter layout provides",
                              ld_hex(b1, g_ld_data_top), ld_hex(b2, initial_sp),
                              0,0,0,0);
                    return ARK_ERR_LINK;
                }
                /* --vector-table: the first object reserved 0x400 for us, so
                 * write the table there.  0x400 is the size codegen reserves,
                 * and the hole must still be untouched: if anything but zero
                 * sits there, the first object was built without the flag and
                 * the table would land on top of somebody's code. */
                if ((ld_find_global("Reset_Handler") >= 0) && (opts->vector_table)) {
                    int vt_size = LD_VECTOR_TABLE_ENTRIES * 4;
                    int hole_ok = (out_len >= vt_size);
                    int hk;
                    for (hk = 0; hole_ok && (hk < vt_size); hk++) {
                        if (out_buf[hk] != 0) hole_ok = 0;
                    }
                    if (!hole_ok) {
                        char b1[12], b2[12];
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "the first %s bytes of the linked image are not a "
                                  "free vector-table hole (image is %s bytes): link "
                                  "an object built with --vector-table first, or "
                                  "drop --vector-table",
                                  diag_num(b1, vt_size), diag_num(b2, out_len), 0,0,0,0);
                        return ARK_ERR_LINK;
                    }
                    ld_build_vector_table(out_buf, flash_base, initial_sp);
                    if (opts->verbose) {
                        ark_log("[link] vector table written into the %d-byte hole "
                                "the first object reserved (SP=0x%08X, entry=0x%08X)\n",
                                vt_size,
                                (unsigned)rd_le32((const unsigned char *)out_buf),
                                (unsigned)(rd_le32((const unsigned char *)out_buf + 4) & ~1u));
                    }
                }
                /* No --vector-table: the image must already carry a usable table
                 * (that is what a single object built with the flag does).  Check
                 * it instead of trusting it. */
                if ((ld_find_global("Reset_Handler") >= 0) && (!opts->vector_table)) {
                    unsigned sp_w = (out_len >= 4) ?
                        (unsigned)rd_le32((const unsigned char *)out_buf) : 0u;
                    unsigned en_w = (out_len >= 8) ?
                        (unsigned)rd_le32((const unsigned char *)out_buf + 4) : 0u;
                    unsigned en_a = en_w & ~1u;
                    int why = 0;
                    if (!have_ram_win) {
                        /* No scatter: the only bound left is the default initial
                         * SP the encoder assumes, so check 0x20000000..that
                         * instead of inventing a window. */
                        ram_lo = 0x20000000u;
                        ram_hi = initial_sp;
                    }
                    if ((out_len < 8) || ((en_w & 1u) == 0u)) {
                        why = 1;    /* no Thumb entry word at all */
                    } else if ((en_a < flash_base) ||
                               ((en_a - flash_base) >= (unsigned)out_len)) {
                        why = 2;    /* entry outside the image */
                    } else if ((sp_w < ram_lo) || (sp_w > ram_hi)) {
                        why = 3;    /* stack pointer outside the RW window */
                    }
                    if (why != 0) {
                        char b1[12], b2[12], b3[12], b4[12], b5[12];
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "the linked image carries no usable vector table "
                                  "(SP=%s entry=%s; flash %s+%s, RAM from %s): "
                                  "compile the object that defines Reset_Handler "
                                  "with --vector-table, then relink",
                                  ld_hex(b1, sp_w), ld_hex(b2, en_w),
                                  ld_hex(b3, flash_base), diag_num(b4, out_len),
                                  ld_hex(b5, ram_lo), 0);
                        return ARK_ERR_LINK;
                    }
                    if (opts->verbose) {
                        ark_log("[link] objects carry the vector table: SP=0x%08X, "
                                "entry=0x%08X (flash base 0x%08X)\n",
                                sp_w, en_a, flash_base);
                    }
                    /* The backend emits `BL __arkcc_init_globals` at the top of
                     * Reset_Handler only when *that* translation unit owns
                     * .data/.bss (the call is written next to the init body it
                     * generates). With -link the globals may live in another
                     * object: the init body is then linked but never runs, and
                     * the app reads zeroed/garbage globals while the link still
                     * succeeds -- a silently wrong image. Point the reset vector
                     * at a two-instruction trampoline in that case:
                     *     BL   __arkcc_init_globals
                     *     B.W  Reset_Handler
                     * Both instructions are filled in by ld_patch_ref, the same
                     * patcher the object relocations use, so there is exactly
                     * one place that knows the ARM encodings. */
                    {
                        int init_idx  = ld_find_global("__arkcc_init_globals");
                        int reset_idx = ld_find_global("Reset_Handler");
                        int calls_init = 0;
                        int k;
                        for (k = 0; k < g_ld_reloc_count; k++) {
                            if (ark_streq(g_ld_relocs[k].name, "__arkcc_init_globals")) {
                                calls_init = 1;
                                break;
                            }
                        }
                        if (init_idx >= 0 && reset_idx >= 0 && !calls_init) {
                            int t0;
                            if ((out_len & 3) != 0) {
                                if (out_len + 2 > LD_BUF_SIZE) {
                                    diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                              "no room for the __arkcc_init_globals "
                                              "trampoline; raise ARK_LINK_BUF_SIZE",
                                              0,0,0,0,0,0);
                                    return ARK_ERR_IO;
                                }
                                out_buf[out_len++] = 0xBF;  /* NOP (align to 4) */
                                out_buf[out_len++] = 0x00;
                            }
                            if (out_len + 8 > LD_BUF_SIZE) {
                                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                          "no room for the __arkcc_init_globals "
                                          "trampoline; raise ARK_LINK_BUF_SIZE",
                                          0,0,0,0,0,0);
                                return ARK_ERR_IO;
                            }
                            if (g_ld_reloc_count + 2 > LD_MAX_RELOCS) {
                                char b1[12], b2[12];
                                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                          "relocation table is full (%s of %s); the "
                                          "startup trampoline cannot be resolved - "
                                          "raise ARK_LINK_MAX_RELOCS and relink",
                                          diag_num(b1, g_ld_reloc_count),
                                          diag_num(b2, LD_MAX_RELOCS), 0,0,0,0);
                                return ARK_ERR_LINK;
                            }
                            t0 = out_len;
                            /* Placeholders: BL T1 = F000 D000, B.W T4 = F000 9000.
                             * Only their low halves carry the branch shape; both
                             * are overwritten by ld_patch_ref below. */
                            out_buf[t0 + 0] = 0xF0; out_buf[t0 + 1] = 0x00;
                            out_buf[t0 + 2] = 0xD0; out_buf[t0 + 3] = 0x00;
                            out_buf[t0 + 4] = 0xF0; out_buf[t0 + 5] = 0x00;
                            out_buf[t0 + 6] = 0x90; out_buf[t0 + 7] = 0x00;
                            ld_add_reloc("__arkcc_init_globals", t0, 1, 0);
                            ld_add_reloc("Reset_Handler", t0 + 4, 2, 0);
                            out_len += 8;
                            ld_write_word(out_buf, 4,
                                          (flash_base + (unsigned)t0) | 1u);
                            if (opts->verbose)
                                ark_log("[link] note: Reset_Handler does not call "
                                        "__arkcc_init_globals (globals live in "
                                        "another object); reset vector points at "
                                        "a trampoline at 0x%08X\n",
                                        flash_base + (unsigned)t0);
                        }
                    }
                    if (opts->verbose) {
                        int ri = ld_find_global("Reset_Handler");
                        if (ri >= 0)
                            ark_log("  Reset     = 0x%08X\n",
                                    flash_base + (unsigned)g_ld_syms[ri].offset);
                    }
                }
                /* --reloc-text: the scatter file has now told us where the
                 * image lands, so the absolute addresses each object baked in
                 * can be corrected.  This has to happen after the vector table
                 * is laid into its hole (it is written at flash_base, which is
                 * exactly the base the correction is measured from) and before
                 * anything else is written. */
                if (g_config.enable_reloc_text) {
                    if (ld_relocate_text(out_buf, out_len, flash_base) != 0) {
                        return ARK_ERR_LINK;
                    }
                }
            }
            /* Startup convention: a hand-written or generated Reset_Handler
             * calls __arkcc_init_globals unconditionally. The body, however,
             * is only emitted by a translation unit that owns .data/.bss, so a
             * link whose objects have no RAM at all would stop on an unresolved
             * symbol that means "nothing to initialise". Define it as a
             * one-instruction stub in that case. An object that *does* declare
             * RAM but comes without an init body is a compiler inconsistency:
             * refuse instead of skipping its initialisation. */
            if (ld_find_global("__arkcc_init_globals") < 0) {
                int referenced = 0;
                int k;
                for (k = 0; k < g_ld_reloc_count; k++) {
                    if (ark_streq(g_ld_relocs[k].name, "__arkcc_init_globals")) {
                        referenced = 1;
                        break;
                    }
                }
                if (referenced) {
                    for (k = 0; k < LD_MAX_OBJS; k++) {
                        if (g_ld_ram[k].have_start && g_ld_ram[k].have_end &&
                            g_ld_ram[k].start < g_ld_ram[k].end) {
                            char b1[12];
                            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                      "object #%s declares .data/.bss but carries "
                                      "no __arkcc_init_globals body; its globals "
                                      "would never be initialised - recompile it "
                                      "with the same arkc that built the link",
                                      diag_num(b1, k), 0,0,0,0,0);
                            return ARK_ERR_LINK;
                        }
                    }
                    if (out_len + 2 > LD_BUF_SIZE) {
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "no room for the __arkcc_init_globals stub; "
                                  "raise ARK_LINK_BUF_SIZE", 0,0,0,0,0,0);
                        return ARK_ERR_IO;
                    }
                    out_buf[out_len++] = 0x70;   /* BX lr - nothing to initialise */
                    out_buf[out_len++] = 0x47;
                    ld_add_sym("__arkcc_init_globals", out_len - 2, 0);
                }
            }
            /* --reloc-data: chain every object's __arkcc_init_globals.
             *
             * Each object that owns .data/.bss emits its own init body, but all
             * of them carry the same symbol name, so ld_add_sym keeps the first
             * and the others would never run - the second object's globals would
             * stay uninitialised while the link still reported success.  Emit a
             * chain at the end of the image that calls each body in object order
             * and then falls through into the one the symbol names, and point
             * the symbol at the chain.  Nothing already placed moves: the chain
             * is appended after the last object.
             */
            if (g_config.enable_reloc_data) {
                int nb = 0, b;
                int idx = ld_find_global("__arkcc_init_globals");
                for (b = 0; b < LD_MAX_OBJS; b++)
                    if (g_ld_init_body[b] >= 0) nb++;
                if (idx >= 0 && nb > 1) {
                    int chain, t;
                    if ((out_len & 3) != 0) {
                        if (out_len + 2 > LD_BUF_SIZE) {
                            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                      "no room for the init chain; raise "
                                      "ARK_LINK_BUF_SIZE", 0,0,0,0,0,0);
                            return ARK_ERR_IO;
                        }
                        out_buf[out_len++] = 0xBF;   /* NOP (align to 4) */
                        out_buf[out_len++] = 0x00;
                    }
                    if (out_len + 4 * (nb + 1) + 4 > LD_BUF_SIZE) {
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "no room for the init chain; raise "
                                  "ARK_LINK_BUF_SIZE", 0,0,0,0,0,0);
                        return ARK_ERR_IO;
                    }
                    chain = out_len;
                    /* PUSH {lr} first: every body clobbers lr, so chaining by
                     * tail-jumping from one body into the next makes the chain
                     * return into itself (measured: an endless bss loop).  With
                     * lr saved once here, each body returns to the next BL and
                     * the final POP {pc} hands control back to Reset_Handler. */
                    out_buf[out_len++] = 0x00;   /* PUSH {lr} = 0xB500 */
                    out_buf[out_len++] = 0xB5;
                    for (b = 0; b < LD_MAX_OBJS; b++) {
                        if (g_ld_init_body[b] < 0) continue;
                        t = out_len;
                        /* BL T1 placeholder; ld_patch_ref fills both halves. */
                        out_buf[t]     = 0xF0;
                        out_buf[t + 1] = 0x00;
                        out_buf[t + 2] = 0xD0;
                        out_buf[t + 3] = 0x00;
                        out_len += 4;
                        ld_patch_ref(out_buf, out_len, t, g_ld_init_body[b], 1);
                    }
                    out_buf[out_len++] = 0x00;   /* POP {pc} = 0xBD00 */
                    out_buf[out_len++] = 0xBD;
                    g_ld_syms[idx].offset = chain;
                    if (opts->verbose)
                        ark_log("[link] note: %d objects own .data/.bss; "
                                "__arkcc_init_globals now runs all %d bodies "
                                "(chain at +0x%X)\n", nb, nb, chain);
                }
            }
            /* Resolve cross-object relocations via find_global (strong > weak).
             * Any reference left unresolved aborts the link below: an image
             * whose references point at the wrong address still loads and still
             * runs, it just jumps somewhere else - the worst shape of "it
             * worked". Measured on the board: a mini app that forgot to define
             * its entry symbol used to link "successfully", run all four pack
             * passes, and then fail in the packing stage with an unrelated -33,
             * so the output never named the actual cause (a missing symbol). */
            {
                int ri;
                int unresolved = 0;
                for (ri = 0; ri < g_ld_reloc_count; ri++) {
                    int idx;
                    if (g_ld_relocs[ri].type == 6 || g_ld_relocs[ri].type == 12)
                        continue;  /* .arkdata / .arktext site: already patched
                        * already handled by ld_relocate_data; it names no
                        * symbol, so treating it as an unresolved reference
                        * would fail the link on a valid image. */
                    idx = ld_find_global(g_ld_relocs[ri].name);
                    if (idx >= 0 && (g_ld_relocs[ri].type == RV_RELOC_AUIPC ||
                                     g_ld_relocs[ri].type == RV_RELOC_ABS32)) {
#if ARKCC_BACKEND_RISCV32
                        /* riscv32: the site carries an instruction pair (or an
                         * address word), and the patch is computed from where
                         * it actually landed.  A site that does not hold that
                         * pattern means the object disagrees with its own
                         * relocation table, so nothing gets written. */
                        if (rv_patch_reloc(out_buf, out_len, g_ld_relocs[ri].offset,
                                           g_ld_syms[idx].offset,
                                           g_ld_relocs[ri].type,
                                           0x08000000u) != 0) {
                            char b1[12], b2[12];
                            diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                      "the riscv32 relocation at offset %s for '%s' "
                                      "does not hold the instruction pair it "
                                      "describes, or its displacement does not fit; "
                                      "no image was written",
                                      diag_num(b1, g_ld_relocs[ri].offset),
                                      g_ld_relocs[ri].name, 0,0,0,0);
                            unresolved++;
                        }
#else
                        /* This build has no riscv32 backend (the STM32 profiles
                         * trim it), so there is no patcher for these types.
                         * Feeding them to the ARM one would write Thumb
                         * encodings into a RISC-V object and exit 0. */
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "this build has no riscv32 backend, so the "
                                  "riscv32 relocation at offset %s for '%s' "
                                  "cannot be applied; no image was written",
                                  diag_num(b1, g_ld_relocs[ri].offset),
                                  g_ld_relocs[ri].name, 0,0,0,0);
                        unresolved++;
#endif /* ARKCC_BACKEND_RISCV32 */
                    } else if (idx >= 0)
                        ld_patch_ref(out_buf, out_len, g_ld_relocs[ri].offset,
                                     g_ld_syms[idx].offset, g_ld_relocs[ri].type);
                    else {
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "unresolved symbol %sq", g_ld_relocs[ri].name,
                                  0,0,0,0,0);
                        unresolved++;
                    }
                }
                if (unresolved > 0)
                    return ARK_ERR_LINK;
            }
            if (ld_table_overflow > 0) {
                char b1[12], b2[12];
                diag_tool(DIAG_SEV_ERROR, g_ld_image,
                          "%s symbol/relocation entries did not fit (limit %s); "
                          "the image would be missing code or references, so "
                          "nothing was written - raise ARK_LINK_MAX_SYMS / "
                          "ARK_LINK_MAX_RELOCS and relink",
                          diag_num(b1, ld_table_overflow),
                          diag_num(b2, ARK_LINK_MAX_SYMS), 0,0,0,0);
                return ARK_ERR_LINK;
            }
            if (fs_write_file(out_path, out_buf, out_len) != out_len) {
                diag_tool(DIAG_SEV_ERROR, out_path, "cannot write link output file", 0,0,0,0,0,0);
                return ARK_ERR_IO;
            }
            if (opts->verbose) ark_log("[arkcc] linked %d objects -> %s (%d bytes, %d syms, "
                                       "%d relocs)\n",
                                       opts->link_input_count, out_path, out_len,
                                       g_ld_sym_count, g_ld_reloc_count);
            return ARK_OK;
        } else {
            /* Text concatenation (C backend) */
            for (li = 0; li < opts->link_input_count; li++) {
                int n = fs_read_file(opts->link_inputs[li], buf, LD_BUF_SIZE);
                int rem;
                const char *p;
                if (n < 0) {
                    diag_tool(DIAG_SEV_ERROR, opts->link_inputs[li],
                              "cannot read object file", 0,0,0,0,0,0);
                    return ARK_ERR_IO;
                }
                {
                    /* Same guard as the binary path: a truncated text object
                     * would be concatenated as if it were complete. */
                    char b1[12], b2[12];
                    int fsz = fs_file_size(opts->link_inputs[li]);
                    if (fsz < 0 || fsz > n) {
                        diag_tool(DIAG_SEV_ERROR, opts->link_inputs[li],
                                  "object file is %s bytes but only %s could be "
                                  "read; raise ARK_LINK_BUF_SIZE and relink",
                                  diag_num(b1, fsz), diag_num(b2, n), 0,0,0,0);
                        return ARK_ERR_IO;
                    }
                }
                p = buf;
                rem = n;
                if (rem >= 7 && p[0] == 'A' && p[1] == 'R' && p[2] == 'K' &&
                    p[3] == 'O' && p[4] == 'B' && p[5] == 'J' && p[6] == '\n') {
                    p += 7; rem -= 7;
                }
                if (li > 0) {
                    while (rem > 0 && (*p == '\n' || *p == '\r')) { p++; rem--; }
                }
                if (out_len + rem + 1 > LD_BUF_SIZE) {
                    char b1[12], b2[12], b3[12];
                    diag_tool(DIAG_SEV_ERROR, g_ld_image,
                              "link output too large: %s bytes already concatenated + "
                              "%s more does not fit the %s-byte ARK_LINK_BUF_SIZE",
                              diag_num(b1, out_len), diag_num(b2, rem + 1),
                              diag_num(b3, LD_BUF_SIZE), 0,0,0);
                    return ARK_ERR_IO;
                }
                { int k; for (k = 0; k < rem; k++) out_buf[out_len++] = (unsigned char)p[k]; }
                out_buf[out_len++] = '\n';
            }
            if (fs_write_file(out_path, out_buf, out_len) != out_len) {
                diag_tool(DIAG_SEV_ERROR, out_path, "cannot write link output file", 0,0,0,0,0,0);
                return ARK_ERR_IO;
            }
            if (opts->verbose) ark_log("[arkcc] linked %d objects -> %s\n", opts->link_input_count, out_path);
            return ARK_OK;
        }
    }

    if (!opts->input_path) return ARK_ERR_ARG;

    /* Detect assembly input (.s / .S / .asm / .ASM): hand it straight to the
     * backend assembler and skip the whole frontend pipeline
     * (preproc -> lex -> parse -> sema -> irgen). */
    if (is_asm_input(opts->input_path)) {
        int asm_len;
        int code_size;
        const ArkBackend *be2;
        char *asm_text;

        be2 = opts->target ? ark_backend_find(opts->target) : ark_backend_default();
        if (!be2) {
            diag_tool(DIAG_SEV_ERROR, "arkcc", "unknown target %sq",
                      opts->target ? opts->target : "(null)", 0,0,0,0,0);
            return ARK_ERR_ARG;
        }
        mem_pool_reset();
        asm_len = fs_read_file(opts->input_path, g_asm, ARK_ASM_BUF_SIZE);
        if (asm_len < 0) {
            diag_tool(DIAG_SEV_ERROR, opts->input_path,
                      "cannot read source file", 0,0,0,0,0,0);
            return ARK_ERR_IO;
        }
        g_asm[asm_len] = 0;
        asm_text = g_asm;

        if (opts->emit_asm) {
            /* -S mode: output assembly text as-is */
            if (opts->output_path && !ark_streq(opts->output_path, "-") &&
                !ark_streq(opts->output_path, "nul") && !ark_streq(opts->output_path, "NUL")) {
                return write_text(opts->output_path, asm_text);
            } else if (!opts->output_path ||
                       ark_streq(opts->output_path, "-")) {
                /* Write to stdout (fs_write_file handles the "-" path) */
                if (fs_write_file("-", (const unsigned char *)asm_text, asm_len) != asm_len)
                    return ARK_ERR_IO;
            }
            return ARK_OK;
        }

        /* -c wants an object, where an undefined symbol becomes a relocation;
         * a flat .bin must refuse one, because nothing will ever fill it in.
         * Only the riscv32 backend reads this flag. */
#if ARKCC_BACKEND_RISCV32
        g_rv_obj_mode = opts->compile_only ? 1 : 0;
#endif
        code_size = be2->assemble(asm_text, g_code, ARK_CODE_BUF_SIZE);
        if (code_size < 0) {
            diag_tool(DIAG_SEV_ERROR, opts->input_path, "assembly failed", 0,0,0,0,0,0);
            return ARK_ERR_ASM;
        }
        /* -c on an assembly input used to be silently ignored and still
         * produced a flat .bin, so the flag changed nothing. Emit a real
         * ARKOBJ2 object now, using the same writer as the C front end. */
        if (opts->compile_only) {
            return write_ark_obj(be2, opts, asm_len, code_size);
        }
        if (be2->link) {
            int rc = be2->link(g_code, code_size, 0);
            if (rc < 0) return ARK_ERR_LINK;
        }
        if (opts->verbose) ark_log("[arkcc] assembled assembly input: %d bytes\n", code_size);
        {
            char outpath[512];
            const char *target;
            if (opts->output_path) {
                target = opts->output_path;
            } else {
                if (derive_output(opts->input_path, outpath, sizeof(outpath), ".bin") != 0)
                    return ARK_ERR_ARG;
                target = outpath;
            }
            return write_binary(target, g_code, code_size);
        }
    }

    /* Bug M2: latch ARKCC_DUMP_SYMS into the ARM backend debug flag once at
     * startup so g_arm_dump_syms is actually populated. Previously the flag
     * was dead code on host (the backend checked getenv() inline at the point
     * of use) and was never set on either profile. No-op on STM32 (getenv
     * unavailable under semihosting; flag set via UART there). */
    arm_dump_syms_init();
    mem_pool_reset();
    preproc_reset();
    preproc_set_main_file(opts->input_path);

#if ARKCC_FEATURE_BUILD_TIME
    /* __DATE__/__TIME__ 的三个来源，优先级从高到低：
     *   1. --source-date-epoch（可复现构建的标准做法）
     *   2. --build-date / --build-time（手工钉死）
     *   3. 平台层（宿主用 localtime，板端可能没有 RTC 于是固定）
     * 前两种都要经过格式校验：长度不对会让后面的代码读越界。 */
    if (opts->build_epoch > 0) {
        char bd[12], bt[9];
        format_epoch_utc(opts->build_epoch, bd, bt);
        if (!preproc_set_build_time(bd, bt)) {
            diag_tool(DIAG_SEV_ERROR, opts->input_path,
                      "--source-date-epoch produced an unusable timestamp", 0,0,0,0,0,0);
        }
    } else if (opts->build_date || opts->build_time) {
        if (!preproc_set_build_time(opts->build_date, opts->build_time)) {
            diag_tool(DIAG_SEV_ERROR, opts->input_path,
                      "--build-date expects \"Mmm dd yyyy\" and --build-time expects \"hh:mm:ss\"",
                      0,0,0,0,0,0);
        }
    }
#endif

    /* C99 6.10.2: #include "..." searches the directory of the source file
     * containing the #include directive. We add the main source file's
     * directory as the highest-priority search path so that paths like
     * #include "Drivers/.../foo.c" (relative to the .c file) resolve
     * correctly, matching gcc/armcc5 behavior. This is essential for
     * --uvprojx builds where the main .c includes HAL .c sources by
     * project-relative paths. Added FIRST so it has highest search priority. */
    {
        const char *src = opts->input_path;
        int slen = ark_strlen(src);
        int di;
        char srcdir[512];
        for (di = slen - 1; di >= 0; di--) {
            if (src[di] == '/' || src[di] == '\\') break;
        }
        if (di > 0 && di < (int)sizeof(srcdir) - 1) {
            int j;
            for (j = 0; j <= di; j++) srcdir[j] = src[j];
            srcdir[di + 1] = 0;
            preproc_add_include_dir(srcdir);
        }
    }

    /* User-specified -I dirs (added after source dir for lower priority) */
    for (i = 0; i < opts->include_dir_count; i++) {
        preproc_add_include_dir(opts->include_dirs[i]);
    }
    preproc_add_include_dir("stdlib/include");

    /* --cpu/-mcpu: override default (M7) predefined macros and FPU/DSP switches based on target CPU.
     * Must be called before preproc_process so later preprocessing sees the correct
     * __TARGET_ARCH_* / __TARGET_FEATURE_DSP / __TARGET_FPU_VFP macros. */
    if (opts->cpu) {
        const CpuInfo *cpu = cpu_lookup(opts->cpu);
        if (!cpu) {
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "unknown CPU %sq (supported: Cortex-M0/M0+/M1/M3/M4/M7/M23/M33)",
                      opts->cpu, 0,0,0,0,0);
            return ARK_ERR_ARG;
        }
        cpu_apply_predefines(cpu);
        /* Which FPU the ARM backend may use for double <-> float conversion:
         * only the DP variants (Cortex-M7 DP) can encode VCVT.F64.*. */
        g_arm_fpu_dp = cpu->has_fpu_dp ? 1 : 0;
        /* CPUs without FPU support automatically disable hardware FPU codegen */
        if (!cpu->has_fpu) g_config.enable_fpu_hw = 0;
        if (opts->verbose) {
            ark_log("[arkcc] cpu=%s arch_m=%d dsp=%d fpu=%d\n",
                    cpu->name, cpu->arch_m, cpu->has_dsp, cpu->has_fpu);
        }
    }

    /* -DNAME[=VALUE]: register command-line macro defines before preprocessing.
     * Each entry is parsed: optional '=VALUE' suffix becomes the macro body;
     * without '=', the macro body is "1". */
    for (i = 0; i < opts->pre_define_count; i++) {
        const char *e = opts->pre_defines[i];
        const char *eq = 0;
        int j;
        for (j = 0; e[j]; j++) { if (e[j] == '=') { eq = e + j; break; } }
        if (eq) {
            /* NAME=VALUE: copy NAME to a temp buffer (not pooled; lives through preproc) */
            static char name_buf[256];
            int nl = (int)(eq - e);
            int k;
            if (nl >= (int)sizeof(name_buf)) nl = (int)sizeof(name_buf) - 1;
            for (k = 0; k < nl; k++) name_buf[k] = e[k];
            name_buf[nl] = 0;
            preproc_define(name_buf, eq + 1);
        } else {
            preproc_define(e, "1");
        }
    }

    /* Variadic functions: which va-region convention the target uses.
     * <stdarg.h> has two ways to walk an argument list and they are not
     * interchangeable:
     *   * the C and x86_64 backends keep ONE 8-byte slot per argument
     *     (ordinal-indexed: irgen asks for frame+8000+8k);
     *   * the ARM and RV32I backends keep one 4-byte word per AAPCS slot,
     *     which is the caller's real layout, so a 64-bit argument is just
     *     two adjacent words (irgen's 8-byte stride is remapped there).
     * The header cannot see which backend is compiling it, so the driver
     * says so here and stdarg.h picks one walk.  Deliberately left
     * undefined for the 8-byte-slot targets: their preprocessed text stays
     * byte-identical, which is what the C-side tests compare against. */
    {
        const ArkBackend *vbe = opts->target ? ark_backend_find(opts->target)
                                             : ark_backend_default();
        if (vbe && vbe->name &&
            (ark_streq(vbe->name, "arm") || ark_streq(vbe->name, "riscv32"))) {
            preproc_define("__ARK_VA_WORD_SLOTS", "1");
            /* ...and irgen must not reserve the C backend's 8 KB slot area:
             * these two backends build the region out of the caller's own
             * words, so reserving it would only cost stack (8 KB against a
             * 4 KB mini-app task stack on the board). */
            g_ir_va_slot_region = 0;
            /* ARM Thumb-2 and RV32I can both put a >8-byte composite through
             * `...`: their va region is the caller's own argument words (the
             * four spilled registers plus the caller's stack block on ARM, the
             * eight spilled registers plus the caller's stack block on RV32I),
             * so the payload is just a few more words.  RV32I used to be
             * excluded on the claim that it has "no stack arguments" -- it
             * does: every word past a0-a7 is one, and <stdarg.h> already walks
             * those words packed.  The C/x86_64 backends keep the 8-byte slot
             * image, where irgen's inline words would be misread, so they
             * still fail the build with a diagnostic. */
            g_ir_vararg_words =
                (ark_streq(vbe->name, "arm") || ark_streq(vbe->name, "riscv32"))
                    ? 1 : 0;
        }
        /* ...and whether those word slots carry the AAPCS32 alignment holes
         * (ARM: a 64-bit argument starts on an even register / an 8-byte
         * aligned stack slot) or are packed (RV32I: this backend numbers
         * argument words 0,1,2,.. with no gaps).  <stdarg.h> needs to know,
         * because "align ap to 8" is only correct where the holes exist. */
        if (vbe && vbe->name && ark_streq(vbe->name, "riscv32"))
            preproc_define("__ARK_VA_PACKED_WORDS", "1");
    }

    /* --split_sections/-ffunction-sections: sync runtime switch to g_config,
     * so ARM/C backends emit a separate section before each function. */
    if (opts->split_sections) g_config.split_sections = 1;
    /* --vector-table: the ARM backend reserves the table area before assembly
     * so the image does not have to be shifted afterwards.  Set here (not in
     * the backend) because it is an option, and the backend sees only opts-free
     * state.  Zero by default: every existing command line is unchanged. */
    g_arm_vt_reserve = (opts->vector_table != 0) ? 1 : 0;
    /* No -cpu: the conservative value (software double<->float conversion).
     * Set before the -cpu block below can raise it. */
    g_arm_fpu_dp = 0;

    {
        /* fs_read_file() truncates at ARK_SRC_BUF_SIZE-1 without saying so,
         * which turns a too-small buffer into a stream of bogus syntax errors
         * (a real 2 MB CMSIS device header reported "#37 the #endif ... is
         * missing" because the tail was never read). Report the real cause. */
        char b1[12], b2[12];
        int fsz = fs_file_size(opts->input_path);
        if (fsz >= ARK_SRC_BUF_SIZE) {
            diag_tool(DIAG_SEV_ERROR, opts->input_path,
                      "input file is %s bytes, larger than the %s-byte source "
                      "buffer; rebuild arkcc with a bigger ARK_SRC_BUF_SIZE",
                      diag_num(b1, fsz), diag_num(b2, ARK_SRC_BUF_SIZE),
                      0,0,0,0);
            return ARK_ERR_IO;
        }
    }
    src_len = fs_read_file(opts->input_path, g_src, ARK_SRC_BUF_SIZE);
    if (src_len < 0) {
        /* armcc's shape:  "no source": Error:  #5: cannot open source input
         * file "x": <reason>. The reason comes from the file layer so that it
         * describes what actually happened instead of a guess. */
        DiagLoc loc;
        loc.file = DIAG_NO_SOURCE_TEXT;
        loc.line = 0;
        loc.col  = 0;
        diag_report(5, DIAG_SEV_ERROR, &loc, "source", opts->input_path,
                    fs_last_error());
        return ARK_ERR_IO;
    }
    /* Preprocessing diagnoses against the raw text: that is what the user sees
     * in their editor, and its line numbers are the ones the directives are on. */
    diag_init(opts->input_path, g_src);
    preprocessed = preproc_process(g_src);
    /* Line numbers in every later diagnostic refer to the *preprocessed*
     * buffer (that is what the lexer counts), so the echo buffer must be the
     * same text or the caret would point at a different line. */
    diag_set_source(preprocessed);

    /* -MMD/--depend: generate dependency file (Makefile style, gcc -MMD compatible).
     * Format: "<output>: <source> <header1> <header2> ...\n"
     * Output path is given by -MF, otherwise defaults to "<input>.d". */
    if (opts->gen_depend) {
        /* 依赖文件能收多少个头文件：宿主与预处理器同源——预处理器记录多少就
         * 取多少（表按实际条数从池里分配），板端仍用 ARKCC_MAX_INC_FILES 的
         * 定长表。两处上限必须同源，否则预处理器记全了、写出来的 .d 还是被截。 */
#if ARKCC_FEATURE_DEP_FILES_DYN
        int ninc_all = preproc_included_count();
        const char **incs = (const char **)mem_pool_alloc(
            (ninc_all > 0 ? ninc_all : 1) * (int)sizeof(*incs));
        int ninc = preproc_get_included_files(incs, ninc_all);
#else
        const char *incs[ARKCC_MAX_INC_FILES];
        int ninc = preproc_get_included_files(incs, ARKCC_MAX_INC_FILES);
#endif
        char dep_path[512];
        const char *dep_target;
        /* Derive output target name: prefer -o (strip extension and add .o), otherwise <input>.o */
        char obj_name[512];
        if (opts->output_path) {
            int ol = ark_strlen(opts->output_path);
            int dot = -1; int k;
            for (k = ol - 1; k >= 0; k--) {
                if (opts->output_path[k] == '/' || opts->output_path[k] == '\\') break;
                if (opts->output_path[k] == '.') { dot = k; break; }
            }
            if (dot < 0) dot = ol;
            for (k = 0; k < dot && k < (int)sizeof(obj_name) - 3; k++) obj_name[k] = opts->output_path[k];
            obj_name[dot] = 0;
            /* Default to .o extension (unless -S outputs .s) */
            if (opts->emit_asm) {
                obj_name[dot] = '.'; obj_name[dot+1] = 's'; obj_name[dot+2] = 0;
            } else {
                obj_name[dot] = '.'; obj_name[dot+1] = 'o'; obj_name[dot+2] = 0;
            }
        } else {
            /* Derive from input_path */
            int il = ark_strlen(opts->input_path);
            int dot = -1; int k;
            for (k = il - 1; k >= 0; k--) {
                if (opts->input_path[k] == '/' || opts->input_path[k] == '\\') break;
                if (opts->input_path[k] == '.') { dot = k; break; }
            }
            if (dot < 0) dot = il;
            for (k = 0; k < dot && k < (int)sizeof(obj_name) - 3; k++) obj_name[k] = opts->input_path[k];
            obj_name[dot] = '.'; obj_name[dot+1] = 'o'; obj_name[dot+2] = 0;
        }
        /* Dependency file path: -MF specifies, otherwise <input>.d */
        if (opts->depend_file) {
            dep_target = opts->depend_file;
        } else {
            int il = ark_strlen(opts->input_path);
            int dot = -1; int k;
            for (k = il - 1; k >= 0; k--) {
                if (opts->input_path[k] == '/' || opts->input_path[k] == '\\') break;
                if (opts->input_path[k] == '.') { dot = k; break; }
            }
            if (dot < 0) dot = il;
            for (k = 0; k < dot && k < (int)sizeof(dep_path) - 3; k++) dep_path[k] = opts->input_path[k];
            dep_path[dot] = '.'; dep_path[dot+1] = 'd'; dep_path[dot+2] = 0;
            dep_target = dep_path;
        }
        /* Build dependency file content: "<obj>: <source> <h1> <h2> ...\n" */
        {
            /* 8 KB 就够写几十个头文件，于是头文件一多，.d 会被截断而构建系统
             * 不再察觉某个头文件改过。宿主按实际条数精确分配，板端保留定长。 */
#if ARKCC_FEATURE_DEP_FILES_DYN
            int dep_cap = 3;
            char *dep_buf;
            {
                int k;
                for (k = 0; obj_name[k]; k++) dep_cap++;
                for (k = 0; opts->input_path[k]; k++) dep_cap++;
                for (k = 0; k < ninc; k++) {
                    int j;
                    for (j = 0; incs[k][j]; j++) dep_cap++;
                    dep_cap++;
                }
            }
            dep_buf = (char *)mem_pool_alloc(dep_cap + 1);
#else
            static char dep_buf_store[8192] __attribute__((section(".ark_buf")));
            char *dep_buf = dep_buf_store;
            int dep_cap = (int)sizeof(dep_buf_store);
#endif
            int dl = 0;
            int k;
            /* Output target */
            for (k = 0; obj_name[k] && dl < dep_cap - 1; k++) dep_buf[dl++] = obj_name[k];
            dep_buf[dl++] = ':';
            dep_buf[dl++] = ' ';
            /* Source file */
            for (k = 0; opts->input_path[k] && dl < dep_cap - 1; k++) dep_buf[dl++] = opts->input_path[k];
            /* Dependent header files */
            for (k = 0; k < ninc && dl < dep_cap - 2; k++) {
                dep_buf[dl++] = ' ';
                {
                    int j;
                    for (j = 0; incs[k][j] && dl < dep_cap - 1; j++) dep_buf[dl++] = incs[k][j];
                }
            }
            dep_buf[dl++] = '\n';
            if (fs_write_file(dep_target, (const unsigned char *)dep_buf, dl) != dl) {
                diag_tool(DIAG_SEV_WARNING, dep_target,
                          "cannot write dependency file", 0,0,0,0,0,0);
                return ARK_ERR_IO;
            }
            if (opts->verbose) ark_log("[arkcc] wrote dependency file '%s' (%d includes)\n", dep_target, ninc);
        }
    }

    /* -E preprocess only */
    if (opts->preprocess_only) {
        const char *target = opts->output_path ? opts->output_path : "-";
        int e_len = ark_strlen(preprocessed);
        if (ark_streq(target, "-")) {
            if (fs_write_file("-", (const unsigned char *)preprocessed, e_len) != e_len)
                return ARK_ERR_IO;
            return ARK_OK;
        }
        return write_text(target, preprocessed);
    }

    be = opts->target ? ark_backend_find(opts->target) : ark_backend_default();
    if (!be) {
        diag_tool(DIAG_SEV_ERROR, "arkcc", "unknown target %sq",
                      opts->target ? opts->target : "(null)", 0,0,0,0,0);
        return ARK_ERR_ARG;
    }
    if (opts->verbose) ark_log("[arkcc] target=%s, source=%d bytes\n", be->name, src_len);

    /* Set pointer size for the selected backend BEFORE parsing/sema/irgen so
     * struct layout and array indexing use the correct pointer width.
     * The C backend runs on the host (8-byte pointers on 64-bit); ARM/RISC-V32
     * use 4-byte pointers; x86_64 uses 8-byte pointers. */
    type_set_ptr_size(be->pointer_size);
    /* C 后端使用 8 字节 long long 虚拟栈槽，irgen 无需生成 32→64 位
     * widen/narrow 序列（DUP+PUSH+LT+NEG）或 64 位条件 BOR 合并。 */
    irgen_set_64bit_slots(ark_streq(be->name, "c"));
    /* The C backend stores a complex value in one 8-byte slot; the machine
     * backends carry four 32-bit words on the expression stack. */
    irgen_set_complex_words(ark_streq(be->name, "c") ? 1 : 4);
    lexer_init(&lex, preprocessed);
    ast = parse_program(&lex);
    if (!ast) return ARK_ERR_PARSE;      /* diagnostics were already printed */
    /* A frontend error means the tree we would hand to sema/codegen is not the
     * program the user wrote. armcc stops here too; continuing only adds
     * follow-on messages ("unresolved label", "cannot be lowered") that point
     * at the symptom instead of the cause. Warnings do not count. */
    if (diag_errors() > 0) return ARK_ERR_PARSE;
    sema_errs = sema_check(ast);
    if (sema_errs > 0) return ARK_ERR_SEMA;
    if (diag_errors() > 0) return ARK_ERR_SEMA;
    {
        int diag_before_irgen = diag_errors();
        ir = irgen_program(ast);
        if (!ir) return ARK_ERR_IRGEN;
        if (g_irgen_errors > diag_before_irgen) {
            /* IR generation could not lower part of the program; the emitted
             * code would be silently wrong, so refuse to produce an image.
             * When irgen already reported a specific diagnostic (e.g. #137) the
             * generic line adds nothing but noise, so it is only printed for
             * failures that were otherwise silent. No armcc code covers this
             * stage, hence no code. */
            if (g_irgen_errors == diag_before_irgen) {
                char nb[12];
                diag_report_msg(DIAG_SEV_ERROR, 0,
                                "%d construct(s) could not be lowered to IR",
                                diag_num(nb, g_irgen_errors), 0, 0);
            }
            return ARK_ERR_IRGEN;
        }
    }
    ir->source_file = opts->input_path;  /* Used for DWARF .file/.loc */

    /* IR-level optimization: constant folding + dead code elimination.
     * Compile-time guarded by ARKCC_FEATURE_OPT_*, runtime guarded by g_config / opt_level. */
#if ARKCC_FEATURE_OPT_CONSTFOLD || ARKCC_FEATURE_OPT_DCE
    {
        int lvl = opts->opt_level;
        if (lvl < g_config.opt_level) lvl = g_config.opt_level;
        opt_run(ir, lvl);
    }
#endif

    /* The ARM backend loads the scatter file lazily, at the start of its own
     * code generation (it only sees g_config, never ArkOptions).  When that
     * parse fails it used to leave every base at the built-in
     * 0x08000000 / 0x20020000 and carry on -- the image linked to addresses
     * the caller never asked for, and nothing in the output said so.  Refuse
     * to emit an image from a codegen run that reported an error: the caller
     * asked for a layout, and the only honest outcomes are "placed per the
     * file" or "failed". */
    {
        int diag_before_codegen = diag_errors();
        asm_len = be->emit_asm(ir, g_asm, ARK_ASM_BUF_SIZE);
        if (asm_len < 0) return ARK_ERR_CODEGEN;
        if (diag_errors() > diag_before_codegen) {
            if (g_config.scatter_file && g_config.scatter_file[0]) {
                /* Factual, no blame: this branch runs on ANY codegen error, and
                 * the old wording ("the requested layout ... could not be
                 * applied") pointed at the layout even when the layout was
                 * fine. Measured: a variadic definition refused by the ARM
                 * backend produced this line right after the real diagnostic,
                 * which sends the reader after a non-existent .sct problem. */
                diag_tool(DIAG_SEV_ERROR, opts->input_path,
                          "nothing was written: codegen stopped on an error "
                          "above, so the layout in %s was never applied "
                          "(there is no fallback to the built-in "
                          "0x08000000 / 0x20020000)",
                          g_config.scatter_file, 0,0,0,0,0);
            }
            return ARK_ERR_CODEGEN;
        }
    }
    g_asm[asm_len] = 0;

    /* -c mode: write an ARKOBJ2 object instead of a flat binary. */
    if (opts->compile_only) {
        int code_len = asm_len;
        if (ark_streq(be->name, "arm") || ark_streq(be->name, "x86") ||
            ark_streq(be->name, "riscv32")) {
            /* riscv32 used to fall through with code_len = asm_len, i.e. the
             * object body was the assembly text -- an "object" nothing could
             * link. It has a real assembler now, so the object carries the
             * machine code at the layout its -link path lays out. */
#if ARKCC_BACKEND_RISCV32
            g_rv_obj_mode = 1;
#endif
            code_len = be->assemble(g_asm, g_code, ARK_CODE_BUF_SIZE);
            if (code_len < 0) {
                diag_tool(DIAG_SEV_ERROR, g_ld_image, "assembly failed", 0,0,0,0,0,0);
                return ARK_ERR_ASM;
            }
        }
        return write_ark_obj(be, opts, asm_len, code_len);
    }

    if (opts->emit_asm) {
        if (opts->output_path && !ark_streq(opts->output_path, "-") &&
            !ark_streq(opts->output_path, "nul") && !ark_streq(opts->output_path, "NUL")) {
            int rc = write_text(opts->output_path, g_asm);
            if (rc != ARK_OK) return rc;
        } else if (opts->output_path && (ark_streq(opts->output_path, "nul") || ark_streq(opts->output_path, "NUL"))) {
            /* Discard output */
        } else {
            /* Write to stdout (fs_write_file handles the "-" path) */
            if (fs_write_file("-", (const unsigned char *)g_asm, asm_len) != asm_len)
                return ARK_ERR_IO;
        }
#if ARKCC_FEATURE_SCATTER
        /* If the user supplied a scatter .sct or a GNU ld .ld file, emit the
         * converted counterpart next to the assembly output. This lets a user
         * feed arkcc an .sct and get a matching .ld for arm-none-eabi-ld, or
         * feed an .ld and get a .sct for armlink. The converted file name is
         * <output_without_ext>.ld or <output_without_ext>.sct respectively. */
        if (opts->scatter_file || opts->linker_script) {
            ScatterDesc sd;
            sd.load_regions = 0;
            sd.error_count = 0;
            if (opts->scatter_file &&
                scatter_parse_file(opts->scatter_file, &sd) == 0) {
                /* .sct -> .ld */
                static char ld_buf[8192] __attribute__((section(".ark_buf")));
                int ln = scatter_to_ld_script(&sd, ld_buf, (int)sizeof(ld_buf) - 1);
                if (ln > 0) {
                    char ld_path[512];
                    const char *out_base = opts->output_path ? opts->output_path : opts->input_path;
                    if (derive_output(out_base, ld_path, sizeof(ld_path), ".ld") == 0) {
                        write_text(ld_path, ld_buf);
                        if (opts->verbose) ark_log("[arkcc] wrote linker script %s\n", ld_path);
                    }
                }
                scatter_free(&sd);
            } else if (opts->linker_script &&
                       linker_script_parse_file(opts->linker_script, &sd) == 0) {
                /* .ld -> .sct */
                static char sct_buf[8192] __attribute__((section(".ark_buf")));
                int sn = scatter_to_sct_file(&sd, sct_buf, (int)sizeof(sct_buf) - 1);
                if (sn > 0) {
                    char sct_path[512];
                    const char *out_base = opts->output_path ? opts->output_path : opts->input_path;
                    if (derive_output(out_base, sct_path, sizeof(sct_path), ".sct") == 0) {
                        write_text(sct_path, sct_buf);
                        if (opts->verbose) ark_log("[arkcc] wrote scatter file %s\n", sct_path);
                    }
                }
                scatter_free(&sd);
            }
        }
#endif
        return ARK_OK;
    }

#if ARKCC_BACKEND_RISCV32
    g_rv_obj_mode = 0;    /* a raw image has no linker: unresolved is fatal */
#endif
    code_size = be->assemble(g_asm, g_code, ARK_CODE_BUF_SIZE);
    if (code_size < 0) {
        diag_tool(DIAG_SEV_ERROR, "arkcc", "assembly failed", 0,0,0,0,0,0);
        return ARK_ERR_ASM;
    }
    /* A raw image has no linker behind it, so a reference the assembler could
     * not resolve keeps its F000 D000 placeholder -- which decodes as a branch
     * to +0x0C000000, i.e. an image that faults outside itself while the
     * compile still exits 0.  Refuse instead.  (-c keeps the reference as a
     * relocation and -link resolves it, so this check is only for .bin.) */
    if (ark_streq(be->name, "arm")) {
        extern int arm_get_unresolved_count(void);
        extern const char *arm_get_unresolved_name(void);
        int unresolved = arm_get_unresolved_count();
        if (unresolved > 0) {
            char b1[12];
            const char *first = arm_get_unresolved_name();
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "%s reference(s) cannot be resolved in a single-file image "
                      "(first: %sq); compile with -c and link the objects, or "
                      "define them in this translation unit",
                      diag_num(b1, unresolved),
                      first ? first : "?", 0,0,0,0);
            return ARK_ERR_ASM;
        }
    }
    if (be->link) {
        int rc = be->link(g_code, code_size, 0);
        if (rc < 0) return ARK_ERR_LINK;
        if (rc > 0) code_size = rc;  /* backend may expand code (e.g. vector table) */
    }
    if (opts->verbose) ark_log("[arkcc] assembled: %d bytes\n", code_size);

    {
        char outpath[512];
        const char *target;
        if (opts->output_path) {
            target = opts->output_path;
        } else {
            const char *ext = ".bin";
            if (ark_streq(be->name, "c")) ext = ".gen.c";
            if (derive_output(opts->input_path, outpath, sizeof(outpath), ext) != 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc", "failed to derive output name", 0,0,0,0,0,0);
                return ARK_ERR_ARG;
            }
            target = outpath;
        }
        return write_binary(target, g_code, code_size);
    }
}

int ark_compile_and_run(const char *path) {
    ArkOptions o;
    int i;
    o.input_path = path;
    o.output_path = 0;
    o.emit_asm = 0;
    o.verbose = 1;
    o.target = 0;
    for (i = 0; i < ARK_PP_MAX_INC_DIRS; i++) o.include_dirs[i] = 0;
    o.include_dir_count = 0;
    o.list_backends = 0;
    o.compile_only = 0;
    o.preprocess_only = 0;
    o.link_mode = 0;
    for (i = 0; i < ARK_LINK_MAX_OBJS; i++) o.link_inputs[i] = 0;
    o.link_input_count = 0;
    o.opt_level = 0;
    o.debug_info = 0;
    o.no_unused_warn = 0;
    o.cpu = 0;
    o.gen_depend = 0;
    o.depend_file = 0;
    o.split_sections = 0;
    o.scatter_file = 0;
    o.linker_script = 0;
    for (i = 0; i < 64; i++) o.pre_defines[i] = 0;
    o.pre_define_count = 0;
    o.uvprojx = 0;
    return ark_compile(&o);
}
