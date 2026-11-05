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
/* Sizes come from arkcc_config.h so a big program can raise them without
 * touching the linker; overflow is reported (see ld_table_overflow). */
#ifndef ARK_LINK_MAX_SYMS
#define ARK_LINK_MAX_SYMS   256
#endif
#ifndef ARK_LINK_MAX_RELOCS
#define ARK_LINK_MAX_RELOCS 512
#endif
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
    int  type;     /* 1=BL, 2=B.W, 3=B<cond>.W, 4=R_ARM_ABS32 */
} LdReloc;

/* Per-object SRAM footprint reported by the object's reserved
 * __arkcc_ram_start / __arkcc_ram_end symbols (absolute addresses). */
typedef struct LdRam {
    unsigned start;
    unsigned end;
    int      have_start;
    int      have_end;
} LdRam;

#define LD_MAX_OBJS 64
static LdRam    g_ld_ram[LD_MAX_OBJS];
static int      g_ld_ram_obj_count = 0;
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
    return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) |
                 ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
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

static void ld_add_reloc(const char *name, int offset, int type) {
    int n;
    if (g_ld_reloc_count >= LD_MAX_RELOCS) { ld_table_overflow++; return; }
    n = 0;
    while (name[n] && n < (int)sizeof(g_ld_relocs[g_ld_reloc_count].name) - 1) {
        g_ld_relocs[g_ld_reloc_count].name[n] = name[n]; n++;
    }
    g_ld_relocs[g_ld_reloc_count].name[n] = 0;
    g_ld_relocs[g_ld_reloc_count].offset = offset;
    g_ld_relocs[g_ld_reloc_count].type = type;
    g_ld_reloc_count++;
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

/* Write an ARKOBJ2 object file: "ARKOBJ2\n" + u32 code size + code body +
 * u32 symbol count + symbols + u32 reloc count + relocs (readers live in the
 * -link implementation below). Shared by the C front end (-c) and by direct
 * assembly input (.s/.S/.asm with -c), so that the two paths cannot drift.
 *
 * asm_len is the length of the assembly text in g_asm; bin_len is the length
 * of the assembled machine code in g_code. Text backends (c/riscv32) have no
 * assembly step and store the text itself, so bin_len is unused there.
 * The caller must have run be->assemble() for arm/x86 before calling this.
 */
static int write_ark_obj(const ArkBackend *be, const ArkOptions *opts,
                         int asm_len, int bin_len) {
    char outpath[512];
    const char *target;
    static unsigned char obj_buf[ARK_OBJ_BUF_SIZE] __attribute__((section(".ark_buf")));
    int ol = 0;
    int is_obj_backend = ark_streq(be->name, "arm") || ark_streq(be->name, "x86");
    int code_len = is_obj_backend ? bin_len : asm_len;
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
    if (ark_streq(be->name, "arm")) {
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

    /* Write symbol table for object backends (ARM/x86) */
    if (p_sym_count) {
        int sc = p_sym_count();
        int si;
        /* The RAM-footprint descriptor is appended after the real symbols so
         * that older readers that ignore trailing symbols still work. */
        int sc_total = sc + ram_extra;
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
        static char buf[ARK_LINK_BUF_SIZE] __attribute__((section(".ark_buf")));
        static unsigned char out_buf[ARK_LINK_BUF_SIZE] __attribute__((section(".ark_buf")));
        int out_len = 0;
        int is_binary = 0;
        const char *out_path;
        int li;

        /* Detect ARKOBJ2 binary format from the first input.
         * NOTE: fs_read_file() reads at most max_len-1 bytes (it reserves one
         * byte for the NUL terminator), so asking for an 8-byte header with an
         * 8-byte limit returned 7 bytes and the magic never matched. Every
         * -link therefore silently fell back to raw text concatenation and
         * reported success. Use a wider buffer than the header we compare. */
        {
            char hdr[16];
            int pn = fs_read_file(opts->link_inputs[0], hdr, (int)sizeof(hdr));
            if (pn >= 8 && hdr[0]=='A' && hdr[1]=='R' &&
                hdr[2]=='K' && hdr[3]=='O' &&
                hdr[4]=='B' && hdr[5]=='J' &&
                hdr[6]=='2' && hdr[7]=='\n')
                is_binary = 1;
        }
        out_path = opts->output_path ? opts->output_path :
                   (is_binary ? "a.out.bin" : "a.out.c");
        g_ld_image = out_path;
        diag_set_main_file(out_path);

        if (is_binary) {
            /* Binary ARM linker */
            g_ld_sym_count = 0;
            g_ld_reloc_count = 0;
            g_ld_ram_obj_count = 0;
            ld_rt_dup_count = 0;
            { int k; for (k = 0; k < LD_MAX_OBJS; k++) {
                  g_ld_ram[k].start = 0; g_ld_ram[k].end = 0;
                  g_ld_ram[k].have_start = 0; g_ld_ram[k].have_end = 0; } }
            ld_table_overflow = 0;
            for (li = 0; li < opts->link_input_count; li++) {
                int n = fs_read_file(opts->link_inputs[li], buf, sizeof(buf));
                const unsigned char *up = (const unsigned char *)buf;
                int pos = 8;  /* skip "ARKOBJ2\n" header */
                int base = out_len;
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
                if (out_len + code_sz > (int)sizeof(out_buf)) {
                    diag_tool(DIAG_SEV_ERROR, g_ld_image, "link output too large", 0,0,0,0,0,0);
                    return ARK_ERR_IO;
                }
                { int k; for (k = 0; k < code_sz; k++) out_buf[out_len++] = up[pos + k]; }
                pos += code_sz;
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
                    if (ark_streq(sym_name, OBJ_RAM_START_SYM)) {
                        if (li < LD_MAX_OBJS) {
                            g_ld_ram[li].start = (unsigned)sym_off;
                            g_ld_ram[li].have_start = 1;
                        }
                        continue;
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
                    ld_add_reloc(reloc_name, base + reloc_off, reloc_type);
                }
            }
            if (ld_rt_dup_count > 0) {
                ark_log("[link] note: %d duplicate compiler-runtime symbols "
                        "merged (first definition kept)\n", ld_rt_dup_count);
            }
            /* === Multi-object SRAM overlap check ===
             * -link concatenates code sections and resolves code relocations,
             * but it does not relocate .data/.bss: every object was laid out
             * from the same SRAM base (default 0x20000000, or the +RW/+ZI
             * region of the shared scatter file). Linking two objects that
             * both declare RAM objects therefore produces an image where the
             * two sets of globals alias the same addresses. That used to be
             * silent; fail loudly instead of shipping a wrong image. */
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
            /* === Scatter-aware linking: generate vector table ===
             * If a scatter file (.sct or .ld) is provided, parse it to derive
             * flash_base and initial_sp. If Reset_Handler is found in the symbol
             * table, generate a Cortex-M vector table at the start of the binary,
 * shift code right by vt_size, and adjust all symbol/reloc offsets. */
            {
                unsigned flash_base = 0x08000000u;
                unsigned initial_sp = 0x20020000u;
                int vt_size = 0;
#if ARKCC_FEATURE_SCATTER
                {
                    ScatterDesc ld_scatter;
                    ld_scatter.load_regions = 0;
                    ld_scatter.error_count = 0;
                    if (g_config.scatter_file && g_config.scatter_file[0] &&
                        scatter_parse_file(g_config.scatter_file, &ld_scatter) == 0) {
                        flash_base = scatter_get_flash_base(&ld_scatter);
                        {   ScatterExecRegion *er_rw = scatter_find_er_by_attr(&ld_scatter, SCATTER_ATTR_RW);
                            if (er_rw && er_rw->size > 0)
                                initial_sp = er_rw->load_addr + er_rw->size;
                        }
                        scatter_free(&ld_scatter);
                    } else if (g_config.linker_script && g_config.linker_script[0] &&
                               linker_script_parse_file(g_config.linker_script, &ld_scatter) == 0) {
                        flash_base = scatter_get_flash_base(&ld_scatter);
                        {   ScatterExecRegion *er_rw = scatter_find_er_by_attr(&ld_scatter, SCATTER_ATTR_RW);
                            if (er_rw && er_rw->size > 0)
                                initial_sp = er_rw->load_addr + er_rw->size;
                        }
                        scatter_free(&ld_scatter);
                    }
                }
#endif
                /* Generate vector table if Reset_Handler symbol exists */
                if (ld_find_global("Reset_Handler") >= 0) {
                    vt_size = LD_VECTOR_TABLE_ENTRIES * 4;
                    if (out_len + vt_size > (int)sizeof(out_buf)) {
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "vector table exceeds output buffer", 0,0,0,0,0,0);
                        return ARK_ERR_IO;
                    }
                    /* Move existing code right by vt_size bytes */
                    { int k; for (k = out_len - 1; k >= 0; k--) out_buf[k + vt_size] = out_buf[k]; }
                    /* Adjust all symbol offsets by vt_size */
                    for (li = 0; li < g_ld_sym_count; li++)
                        g_ld_syms[li].offset += vt_size;
                    /* Adjust all relocation offsets by vt_size */
                    for (li = 0; li < g_ld_reloc_count; li++)
                        g_ld_relocs[li].offset += vt_size;
                    /* Generate vector table at the beginning */
                    ld_build_vector_table(out_buf, flash_base, initial_sp);
                    out_len += vt_size;
                    if (opts->verbose) {
                        int ri = ld_find_global("Reset_Handler");
                        ark_log("[arkcc] link: vector table %d bytes, SP=0x%08X, Flash=0x%08X\n",
                                vt_size, initial_sp, flash_base);
                        if (ri >= 0)
                            ark_log("  Reset     = 0x%08X\n", flash_base + (unsigned)g_ld_syms[ri].offset);
                    }
                }
            }
            /* Resolve cross-object relocations via find_global (strong > weak) */
            {
                int ri;
                for (ri = 0; ri < g_ld_reloc_count; ri++) {
                    int idx = ld_find_global(g_ld_relocs[ri].name);
                    if (idx >= 0)
                        ld_patch_ref(out_buf, out_len, g_ld_relocs[ri].offset,
                                     g_ld_syms[idx].offset, g_ld_relocs[ri].type);
                    else
                        diag_tool(DIAG_SEV_ERROR, g_ld_image,
                                  "unresolved symbol %sq", g_ld_relocs[ri].name,
                                  0,0,0,0,0);
                }
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
            if (opts->verbose) ark_log("[arkcc] linked %d objects -> %s (%d bytes, %d syms)\n",
                                       opts->link_input_count, out_path, out_len, g_ld_sym_count);
            return ARK_OK;
        } else {
            /* Text concatenation (C backend) */
            for (li = 0; li < opts->link_input_count; li++) {
                int n = fs_read_file(opts->link_inputs[li], buf, sizeof(buf));
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
                if (out_len + rem + 1 > (int)sizeof(out_buf)) {
                    diag_tool(DIAG_SEV_ERROR, g_ld_image, "link output too large", 0,0,0,0,0,0);
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

    /* --split_sections/-ffunction-sections: sync runtime switch to g_config,
     * so ARM/C backends emit a separate section before each function. */
    if (opts->split_sections) g_config.split_sections = 1;

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
        const char *incs[64];
        int ninc = preproc_get_included_files(incs, 64);
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
            static char dep_buf[8192] __attribute__((section(".ark_buf")));
            int dl = 0;
            int k;
            /* Output target */
            for (k = 0; obj_name[k] && dl < (int)sizeof(dep_buf) - 1; k++) dep_buf[dl++] = obj_name[k];
            dep_buf[dl++] = ':';
            dep_buf[dl++] = ' ';
            /* Source file */
            for (k = 0; opts->input_path[k] && dl < (int)sizeof(dep_buf) - 1; k++) dep_buf[dl++] = opts->input_path[k];
            /* Dependent header files */
            for (k = 0; k < ninc && dl < (int)sizeof(dep_buf) - 2; k++) {
                dep_buf[dl++] = ' ';
                {
                    int j;
                    for (j = 0; incs[k][j] && dl < (int)sizeof(dep_buf) - 1; j++) dep_buf[dl++] = incs[k][j];
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

    asm_len = be->emit_asm(ir, g_asm, ARK_ASM_BUF_SIZE);
    if (asm_len < 0) return ARK_ERR_CODEGEN;
    g_asm[asm_len] = 0;

    /* -c mode: write an ARKOBJ2 object instead of a flat binary. */
    if (opts->compile_only) {
        int code_len = asm_len;
        if (ark_streq(be->name, "arm") || ark_streq(be->name, "x86")) {
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

    code_size = be->assemble(g_asm, g_code, ARK_CODE_BUF_SIZE);
    if (code_size < 0) {
        diag_tool(DIAG_SEV_ERROR, "arkcc", "assembly failed", 0,0,0,0,0,0);
        return ARK_ERR_ASM;
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
    for (i = 0; i < 16; i++) o.include_dirs[i] = 0;
    o.include_dir_count = 0;
    o.list_backends = 0;
    o.compile_only = 0;
    o.preprocess_only = 0;
    o.link_mode = 0;
    for (i = 0; i < 32; i++) o.link_inputs[i] = 0;
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
