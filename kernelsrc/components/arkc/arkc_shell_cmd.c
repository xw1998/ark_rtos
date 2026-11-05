/* ============================================================
 * arkc_shell_cmd.c - The "cc" shell command
 *
 * This is the board-side command line the compiler never had: the
 * upstream STM32 port calls arkcc_stm32_compile("main.c") from main(),
 * with the output path fixed and no way to say -S or -D.  Here the
 * options are parsed from argv instead, in the same shape as the host
 * driver (src/main.c), and the result is reported with the compiler's
 * own return code rather than a guess.
 *
 * Four things this file gets right:
 *   1) the abort pad.  arkc calls ark_abort() when the arena runs out;
 *      upstream that is `for(;;)`, which would take the shell task with
 *      it.  setjmp/longjmp brings control back here so the prompt
 *      survives and the failure is reported as a failure.
 *   2) per-run reset.  Diagnostics counters and g_config are reset on
 *      every invocation: a previous run's error count must not make
 *      the next compile fail before it starts.
 *   3) options that this build cannot honour are refused, not ignored.
 *      A silently dropped -o would leave the user hunting for a file
 *      that was never written.
 *   4) no guessing about the backend: default is "arm" (the only one
 *      compiled in), and a wrong -target is reported by arkc itself.
 * ============================================================ */
#include "arkc_shell_cmd.h"
#include "arkc_port.h"

#include "include/compiler.h"
#include "include/common.h"
#include "include/config.h"
#include "include/diag.h"
#include "include/memory.h"
#include "include/platform.h"
#include "include/arkcc_config.h"

#include "ark_shell.h"
#include "ark_shell_commands.h"

#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#if (ARKRTOS_USE_APPPACK == 1)
#include "include/flash_fs.h"      /* fs_write_file / fs_last_error */
#include "arkrtos_apppack.h"       /* the four-pass diff itself (pure) */
#include "arkrtos_ptable.h"        /* four payloads + the image come from the pool */
#include "arkrtos_vfs.h"           /* removes the temporary .sct again */
#endif

/* --scatter / --vector-table / -link are the "layout" options and have two
 * users: direct running of an app (APPRUN) and relocatable packing (APPPACK,
 * whose four passes each need a layout).  With both switches off the command
 * surface and every image are byte-identical to the build that had neither. */
#if (ARKRTOS_USE_APPPACK == 1) || (ARKRTOS_USE_APPRUN == 1)
#define CC_LAYOUT_OPTS   1
#else
#define CC_LAYOUT_OPTS   0
#endif

/* Worst-case option storage; matches the fixed arrays in ArkOptions. */
#define CC_MAX_INCLUDE_DIRS   16
#define CC_MAX_DEFINES        64

/* Everything one compilation must start from a known state: the runtime
 * feature defaults (config_init() overwrites the whole struct, so it has to
 * run first), the diagnostic counters, and the NOLOAD compiler arena whose
 * buffers arkc does not clear itself.  A single `cc` needs this once;
 * `cc --pack` runs the compiler four times and needs it four times, so the
 * body lives in one place. */
static void cc_reset_state(const char *src, int opt_level)
{
    config_init();
    g_config.enable_fpu_hw = 1;   /* the H743 has a double-precision FPU */
    g_config.opt_level     = opt_level;

    arkc_port_zero_arena();
    diag_init(src, NULL);
}

static void cc_usage(void)
{
    ark_shell_printf(
        "usage: cc <file.c> [-o <out>] [-S] [-c] [-E] [-target arm]\r\n"
        "          [-O0|-O1|-O2] [-D<name>[=<value>]] [-I <dir>] [-v]\r\n"
#if (CC_LAYOUT_OPTS == 1)
        "          [--scatter <file.sct>] [--vector-table]\r\n"
        "          [-link <a.obj> [<b.obj> ...]]\r\n"
#endif
        "  no -o: writes <file>.bin (or prints to the console with -S/-E)\r\n"
        "  -o - : prints to the console (text as-is, binary as hex)\r\n"
#if (CC_LAYOUT_OPTS == 1)
        "  --scatter: link for that memory layout (needed for 'app run')\r\n"
        "  --vector-table: reserve 0x400 at the front of the image and put a\r\n"
        "         Cortex-M vector table there.  Use it whenever Reset_Handler is\r\n"
        "         defined: without it arkc prepends the table and shifts the image,\r\n"
        "         which leaves string-literal addresses 0x400 too low\r\n"
        "  -link: link .obj files into one image; every argument up to the next\r\n"
        "         option is an input.  Add --scatter so the image carries a vector\r\n"
        "         table for that window ('app run' reads SP/entry from it)\r\n"
#endif
#if (ARKRTOS_USE_APPPACK == 1)
        "          [--pack -o <f.svcm>]\r\n"
#endif
        );
#if (ARKRTOS_USE_APPPACK == 1)
    ark_shell_printf(
        "  --pack: link the file four times (nominal / ROM+d / RAM+d /\r\n"
        "         ROM+2d and RAM+d), diff the four into a relocation table\r\n"
        "         and write one loadable image.  Needs -o.  Install it with\r\n"
        "         'mini run', not 'app run'\r\n");
#endif
}

/* How much of the compiler arena this run consumed.  Printed on every path
 * that ends without a normal "done": when the pool is what ran out, the
 * number is the difference between "raise ARK_POOL_SIZE by 20%" and guessing.
 * mem_pool_used() is arkc's own bump pointer, so this is the real figure. */
static void cc_report_pool(void)
{
    ark_shell_printf("cc: pool %d/%d B\r\n",
                     mem_pool_used(), (int)ARK_POOL_SIZE);
}

/* The text of an option that this build must not swallow. */
static int cc_bad_option(const char *what, const char *arg)
{
    ark_shell_printf("cc: %s '%s' is not supported by this build\r\n",
                     what, (arg != NULL) ? arg : "");
    return 1;
}

#if (ARKRTOS_USE_APPPACK == 1)
/* ============================================================
 * --pack: four-pass differential packing (M2)
 *
 * One source file is linked four times, only the link bases differ:
 *     A = nominal                (its payload is what the image carries)
 *     B = ROM + d
 *     C = RAM + d
 *     D = ROM + 2d, RAM + d      (proof: C/A applied back must equal D)
 * All four live in memory (borrowed from the RAM pool), because writing them
 * into ramfs would need three file slots that a source + .sct + output already
 * fill, and they would have to be read back byte by byte.
 *
 * Why the four passes run on the board rather than packing on the host and
 * copying the result over: that is the claim under test.  The host copy
 * (tools/pack_app.py) stays as the byte-for-byte referee -- same algorithm in
 * two places, so a disagreement means one of them is wrong.
 *
 * Memory: 4 x 8 KB payloads + 16 KB output, borrowed from the pool and
 * returned on every path.  Static RAM has ~11 KB left, and ramfs has 3 slots.
 * ============================================================ */

/* One payload may not exceed the app-run code window; four of them are one
 * 32 KB block of the pool. */
#define CC_PACK_WIN_SIZE     8192u
/* 256 header + 1024 x 4 table + 8192 payload = 12544 -> one 16 KB block. */
#define CC_PACK_OUT_CAP      16384u
#define CC_PACK_ARENA        (4u * CC_PACK_WIN_SIZE)
/* The nominal bases only end up in the image header (nominal_base /
 * nominal_ram_base); the loader adds (landing point - nominal) at run time, so
 * these values never have to be reached.  ARM's default bases keep them
 * comparable with a plain .sct. */
#define CC_PACK_ROM_BASE     0x08000000u
#define CC_PACK_RAM_BASE     0x20000000u
/* Base increment: must be at least one window (see arkrtos_apppack.h) so that
 * the four address ranges cannot overlap; 512 KB is far larger than 8 KB. */
#define CC_PACK_DELTA        0x00080000u
#define CC_PACK_SCT          "/tmp/pp.sct"

/* One output name per pass.  With capture armed these files are never created;
 * they exist so a failure line can say which pass it was, and so that a capture
 * that silently did not arm would leave visible files behind instead of an
 * empty buffer nobody notices. */
static const char *const s_cc_pack_out[4] = {
    "/tmp/ppA.bin", "/tmp/ppB.bin", "/tmp/ppC.bin", "/tmp/ppD.bin"
};
static const char s_cc_pack_tag[4] = { 'A', 'B', 'C', 'D' };

/* "0x%08X" without pulling printf's format machinery into the image for four
 * lines of text. */
static int cc_hex32(char *p, uint32_t v)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;

    p[0] = '0';
    p[1] = 'x';
    for (i = 0; i < 8; i++) {
        p[2 + i] = hex[(v >> (28 - (4 * i))) & 0xFu];
    }
    return 10;
}

static int cc_str(char *p, const char *s)
{
    int n = 0;

    while (s[n] != '\0') {
        p[n] = s[n];
        n++;
    }
    return n;
}

/* Write pass idx's layout.  The body is character-identical to the .sct that
 * M1 verified on the board, including the CRLF line ends (that file came from
 * `echo ... >>` on the shell, which is the only form ever proven to parse). */
static int cc_pack_write_sct(int idx)
{
    /* Which pass moves which window, spelled out per window so that "what this
     * pass differs in" is readable without indexing arithmetic. */
    static const uint32_t rom_add[4] = { 0u, CC_PACK_DELTA, 0u, 2u * CC_PACK_DELTA };
    static const uint32_t ram_add[4] = { 0u, 0u, CC_PACK_DELTA, CC_PACK_DELTA };
    char txt[256];
    uint32_t rom = CC_PACK_ROM_BASE + rom_add[idx];
    uint32_t ram = CC_PACK_RAM_BASE + ram_add[idx];
    int  n = 0;

    n += cc_str(txt + n, "LR_IROM1 ");
    n += cc_hex32(txt + n, rom);
    n += cc_str(txt + n, " 0x00002000\r\n{\r\nER_IROM1 ");
    n += cc_hex32(txt + n, rom);
    n += cc_str(txt + n, " 0x00002000 { .ANY (+RO) }\r\nER_IRAM1 ");
    n += cc_hex32(txt + n, ram);
    n += cc_str(txt + n, " 0x00002000 { .ANY (+RW +ZI) }\r\n}\r\n");

    return (fs_write_file(CC_PACK_SCT, (const unsigned char *)txt, n) == n) ? 0 : -1;
}

/* One pass.  Returns the number of bytes captured (>= 0) or a negative code:
 *   -1 compile failed, -2 capture already armed, -3 aborted (arena ran out).
 * The abort pad lives in this frame: arkc calls ark_abort() when the pool runs
 * out, and longjmp back into a frame that has already been left would corrupt
 * the task, so each pass gets a fresh pad of its own. */
static int cc_pack_pass(const char *src, const char *target, int opt_level,
                        int verbose, const char *const *includes, int n_includes,
                        const char *const *defines, int n_defines,
                        const char *out_name, uint8_t *win)
{
    ArkOptions popts;
    jmp_buf    pad;
    int        rc;
    int        i;

    memset(&popts, 0, sizeof(popts));
    popts.input_path     = src;
    popts.output_path    = out_name;
    popts.verbose        = verbose;
    popts.target         = target;
    popts.opt_level      = opt_level;
    popts.scatter_file   = CC_PACK_SCT;
    popts.vector_table   = 1;
    for (i = 0; (i < n_includes) && (i < CC_MAX_INCLUDE_DIRS); i++) {
        popts.include_dirs[i] = includes[i];
    }
    popts.include_dir_count = n_includes;
    for (i = 0; (i < n_defines) && (i < CC_MAX_DEFINES); i++) {
        popts.pre_defines[i] = defines[i];
    }
    popts.pre_define_count = n_defines;

    /* Every pass starts from the same state; skipping this once is enough to
     * make pass two emit different bytes than pass one at the same layout. */
    cc_reset_state(src, opt_level);

    if (arkc_port_capture_begin(win, CC_PACK_WIN_SIZE) != 0) {
        return -2;
    }

    arkc_port_set_abort_target(&pad);
    if (setjmp(pad) != 0) {
        arkc_port_set_abort_target(NULL);
        (void)arkc_port_capture_end();   /* must disarm, or the next pass is refused */
        return -3;
    }
    rc = ark_compile(&popts);
    arkc_port_set_abort_target(NULL);

    if (rc != ARK_OK) {
        (void)arkc_port_capture_end();
        return -1;
    }
    return (int)arkc_port_capture_end();
}

static int cc_pack(const char *src, const char *out, const char *target,
                   int opt_level, int verbose,
                   const char *const *includes, int n_includes,
                   const char *const *defines, int n_defines)
{
    uint8_t *img[4];
    uint8_t *outbuf;
    uint32_t idx = 0u;
    uint32_t code_base = 0u;
    uint32_t code_size = 0u;
    uint32_t ram_base = 0u;
    uint32_t ram_size = 0u;
    uint32_t len[4];
    uint32_t len4;
    uint32_t out_len = 0u;
    uint32_t n_ent;
    uint32_t t0;
    int32_t  rc;
    int      i;
    int      got;
    int      ret = 1;
    arkrtos_apppack_win_t   win;
    arkrtos_apppack_stats_t st;

    for (i = 0; i < 4; i++) {
        img[i] = NULL;
        len[i] = 0u;
    }
    memset(&st, 0, sizeof(st));

    /* One 32 KB code block holds the four payloads, one 16 KB RAM block holds
     * the image.  The two blocks are lent and returned together (that is the
     * only atomic lend the pool has), so there is one borrow and one return. */
    rc = arkrtos_ptable_mini_alloc(CC_PACK_ARENA, CC_PACK_OUT_CAP, &idx,
                                   &code_base, &code_size, &ram_base, &ram_size);
    if (rc != 0) {
        ark_shell_printf("cc: --pack needs 32 KB + 16 KB from the RAM pool; "
                         "nothing free (the lend is refused, not shrunk)\r\n");
        return 1;
    }
    if ((code_size < CC_PACK_ARENA) || (ram_size < CC_PACK_OUT_CAP)) {
        /* The pool rounds up to powers of two, so it only ever returns more.
         * Less means the bookkeeping and the allocation disagree, and writing
         * anyway would trample somebody else's block. */
        (void)arkrtos_ptable_mini_free(idx);
        ark_shell_printf("cc: --pack got smaller blocks than it asked for "
                         "(%u/%u B)\r\n", (unsigned)code_size, (unsigned)ram_size);
        return 1;
    }
    for (i = 0; i < 4; i++) {
        img[i] = (uint8_t *)(void *)(code_base + ((uint32_t)i * CC_PACK_WIN_SIZE));
    }
    outbuf = (uint8_t *)(void *)(ram_base);

    t0 = platform_tick_ms();
    ark_shell_printf("cc: pack %s -> %s (4 passes, -O%d)\r\n", src, out, opt_level);

    for (i = 0; i < 4; i++) {
        if (cc_pack_write_sct(i) != 0) {
            ark_shell_printf("cc: cannot write %s: %s\r\n", CC_PACK_SCT, fs_last_error());
            goto done;
        }
        got = cc_pack_pass(src, target, opt_level, verbose, includes, n_includes,
                           defines, n_defines, s_cc_pack_out[i], img[i]);
        if (got < 0) {
            if (got == -2) {
                ark_shell_printf("cc: pass %c: a capture is already armed\r\n",
                                 s_cc_pack_tag[i]);
            } else if (got == -3) {
                ark_shell_printf("cc: pass %c aborted: %s\r\n", s_cc_pack_tag[i],
                                 arkc_port_abort_message());
                cc_report_pool();
            } else {
                ark_shell_printf("cc: pass %c failed\r\n", s_cc_pack_tag[i]);
                cc_report_pool();
            }
            goto done;
        }
        if (got == 0) {
            /* Compiling succeeded but wrote nothing: either -o was dropped or
             * the capture is not wired to the write.  Neither can be packed. */
            ark_shell_printf("cc: pass %c produced no output bytes\r\n",
                             s_cc_pack_tag[i]);
            goto done;
        }
        len[i] = (uint32_t)got;
        diag_summary();
        ark_shell_printf("cc:   %c %u B\r\n", s_cc_pack_tag[i], (unsigned)len[i]);
    }

    if ((len[0] != len[1]) || (len[0] != len[2]) || (len[0] != len[3])) {
        /* A size that depends on the base means the link result depends on the
         * base, and a table derived from that is meaningless. */
        ark_shell_printf("cc: the four passes disagree on size (%u/%u/%u/%u): "
                         "the link result depends on the base, no table possible\r\n",
                         (unsigned)len[0], (unsigned)len[1],
                         (unsigned)len[2], (unsigned)len[3]);
        goto done;
    }

    /* The diff works on whole words, so pad to a multiple of four.  All four
     * are padded with 0: the padding word is identical in every pass and can
     * never be mistaken for an absolute address. */
    len4 = (len[0] + 3u) & ~3u;
    if ((len4 == 0u) || (len4 > CC_PACK_WIN_SIZE)) {
        ark_shell_printf("cc: image %u B does not fit the %u B window\r\n",
                         (unsigned)len[0], (unsigned)CC_PACK_WIN_SIZE);
        goto done;
    }
    for (i = 0; i < 4; i++) {
        uint32_t q;
        for (q = len[i]; q < len4; q++) {
            img[i][q] = 0u;
        }
    }

    win.rom_base = CC_PACK_ROM_BASE;
    win.rom_size = CC_PACK_WIN_SIZE;
    win.ram_base = CC_PACK_RAM_BASE;
    win.ram_size = CC_PACK_WIN_SIZE;

    rc = arkrtos_apppack_build(img[0], img[1], img[2], img[3], len4,
                               CC_PACK_DELTA, &win, ARKRTOS_APP_TYPE_MINIAPP, 1u,
                               outbuf, CC_PACK_OUT_CAP, &out_len, &st);
    if (rc != 0) {
        ark_shell_printf("cc: pack failed: %s (%d)\r\n",
                         arkrtos_apppack_error_name(rc), (int)rc);
        goto done;
    }
    if (fs_write_file(out, outbuf, (int)out_len) != (int)out_len) {
        ark_shell_printf("cc: cannot write %s: %s\r\n", out, fs_last_error());
        goto done;
    }

    n_ent = st.n_rom + st.n_ram + st.n_rom_movw + st.n_ram_movw;
    ark_shell_printf("cc: %s: payload %u + table %u x 4 = %u B\r\n",
                     out, (unsigned)st.image_size, (unsigned)n_ent,
                     (unsigned)out_len);
    /* The four kinds are reported separately on purpose: a class that stays 0
     * means this program never exercised it, so that class is unverified here
     * (that is what the anti-false-green checks look at). */
    ark_shell_printf("cc:   reloc rom %u / ram %u / rom_movw %u / ram_movw %u, "
                     "entry +0x%X\r\n",
                     (unsigned)st.n_rom, (unsigned)st.n_ram,
                     (unsigned)st.n_rom_movw, (unsigned)st.n_ram_movw,
                     (unsigned)st.entry_offset);
    ark_shell_printf("cc: packed in %u ms\r\n",
                     (unsigned)(platform_tick_ms() - t0));
    ret = 0;

done:
    /* Return before anything else: an unreturned lend makes the next pack or
     * mini run fail.  The temporary .sct goes too, or it, the source and the
     * image together take all three ramfs slots. */
    (void)arkrtos_ptable_mini_free(idx);
    (void)arkrtos_vfs_remove(CC_PACK_SCT);
    return ret;
}
#endif /* ARKRTOS_USE_APPPACK */

static int cmd_cc(int argc, char *argv[])
{
    ArkOptions opts;
    jmp_buf    pad;
    int        i;
    int        rc;
    uint8_t    verbose = 0u;
    const char *src = NULL;
    const char *out = NULL;
    const char *target = "arm";
#if (CC_LAYOUT_OPTS == 1)
    /* Only the app-run window needs a caller-chosen layout on the target, so
     * this option is part of that feature: a build without `app run` keeps a
     * byte-identical cc command (and image) instead of carrying an option it
     * cannot use. */
    const char *scatter = NULL;
    /* -link produces the image `app run` consumes: one bare file placed at the
     * window addresses.  Upstream `-link` collects every following non-option
     * argument as an input, and so does this, so `cc -link a.obj -o x.bin`
     * means the same thing on the board as on the host. */
    int        link_mode = 0;
    const char *link_inputs[32];
    int        link_input_count = 0;
    /* --vector-table: emit the table into a reserved 0x400 gap instead of
     * prepending it, so the linked addresses stay the ones that run. */
    int        vector_table = 0;
#endif
#if (ARKRTOS_USE_APPPACK == 1)
    /* --pack: same source, four layouts, one relocatable image.  Left at 0
     * the rest of cmd_cc behaves exactly as before this option existed. */
    int        pack_mode = 0;
#endif
    int        emit_asm = 0;
    int        compile_only = 0;
    int        preprocess_only = 0;
    int        opt_level = 1;
    const char *defines[CC_MAX_DEFINES];
    const char *includes[CC_MAX_INCLUDE_DIRS];
    int        n_defines = 0;
    int        n_includes = 0;
    uint32_t   t0;

    if (argc < 2) {
        cc_usage();
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
#if (CC_LAYOUT_OPTS == 1)
        if ((link_mode != 0) && (a[0] != '-')) {
            /* Objects after -link.  A second bare file used to be an error
             * ("more than one input file"); in link mode it is the point. */
            if (link_input_count >= (int)(sizeof(link_inputs) / sizeof(link_inputs[0]))) {
                ark_shell_printf("cc: more than %d -link inputs\r\n",
                                 (int)(sizeof(link_inputs) / sizeof(link_inputs[0])));
                return 1;
            }
            link_inputs[link_input_count++] = a;
            continue;
        }
#endif
        if (a[0] != '-') {
            if (src != NULL) {
                ark_shell_printf("cc: more than one input file ('%s' after '%s')\r\n",
                                 a, src);
                return 1;
            }
            src = a;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            cc_usage();
            return 0;
        } else if (strcmp(a, "-v") == 0) {
            verbose = 1u;
        } else if (strcmp(a, "-S") == 0) {
            emit_asm = 1;
        } else if (strcmp(a, "-c") == 0) {
            compile_only = 1;
        } else if (strcmp(a, "-E") == 0) {
            preprocess_only = 1;
        } else if (strcmp(a, "-o") == 0) {
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: -o needs a file name\r\n");
                return 1;
            }
            out = argv[++i];
        } else if (strcmp(a, "-target") == 0) {
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: -target needs a name\r\n");
                return 1;
            }
            target = argv[++i];
#if (CC_LAYOUT_OPTS == 1)
        } else if (strcmp(a, "--vector-table") == 0) {
            vector_table = 1;
        } else if (strcmp(a, "-link") == 0) {
            /* Turns the following bare arguments into inputs; nothing else is
             * needed here because the branch above already routes them. */
            link_mode = 1;
        } else if (strcmp(a, "--scatter") == 0) {
            /* Link for a given layout.  Without it the image is linked at the
             * ARM default bases, which is not where an app-run window sits, so
             * the program would be correct code at the wrong addresses. */
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: --scatter needs a file name\r\n");
                return 1;
            }
            scatter = argv[++i];
#if (ARKRTOS_USE_APPPACK == 1)
        } else if (strcmp(a, "--pack") == 0) {
            pack_mode = 1;
#endif
#endif
        } else if (strcmp(a, "-I") == 0) {
            if ((i + 1) >= argc) {
                ark_shell_printf("cc: -I needs a directory\r\n");
                return 1;
            }
            if (n_includes >= CC_MAX_INCLUDE_DIRS) {
                ark_shell_printf("cc: more than %d -I directories\r\n",
                                 CC_MAX_INCLUDE_DIRS);
                return 1;
            }
            includes[n_includes++] = argv[++i];
        } else if ((a[0] == '-') && (a[1] == 'O') &&
                   (a[2] >= '0') && (a[2] <= '2') && (a[3] == '\0')) {
            opt_level = a[2] - '0';
        } else if ((a[0] == '-') && (a[1] == 'D')) {
            const char *d = a + 2;
            if (d[0] == '\0') {
                /* "-D NAME" (space form) is what armcc accepts too. */
                if ((i + 1) >= argc) {
                    ark_shell_printf("cc: -D needs a macro name\r\n");
                    return 1;
                }
                d = argv[++i];
            }
            if (n_defines >= CC_MAX_DEFINES) {
                ark_shell_printf("cc: more than %d -D macros\r\n", CC_MAX_DEFINES);
                return 1;
            }
            defines[n_defines++] = d;
        } else {
            return cc_bad_option("option", a);
        }
    }

#if (CC_LAYOUT_OPTS == 1)
    if (link_mode != 0) {
        /* Refuse a source file instead of quietly compiling it: -link only
         * reads object files, and a `.c` handed to it would be copied
         * verbatim into the image (seen on the host: 676 bytes of source). */
        if (src != NULL) {
            ark_shell_printf("cc: -link takes objects, not source ('%s')\r\n", src);
            return 1;
        }
        if (link_input_count == 0) {
            ark_shell_printf("cc: -link needs at least one .obj\r\n");
            cc_usage();
            return 1;
        }
    } else
#endif
    if (src == NULL) {
        ark_shell_printf("cc: no input file\r\n");
        cc_usage();
        return 1;
    }

#if (ARKRTOS_USE_APPPACK == 1)
    if (pack_mode != 0) {
        /* -S/-c/-E change what the compiler emits; -link/--scatter/
         * --vector-table decide the layout, and --pack decides it itself.
         * Accepting any of them would produce an image whose table describes
         * something the caller did not ask for. */
        if ((emit_asm != 0) || (compile_only != 0) || (preprocess_only != 0)) {
            ark_shell_printf("cc: --pack links an image; it takes no -S/-c/-E\r\n");
            return 1;
        }
#if (ARKRTOS_USE_APPRUN == 1)
        if ((link_mode != 0) || (scatter != NULL) || (vector_table != 0)) {
            ark_shell_printf("cc: --pack chooses its own layout; "
                             "drop -link/--scatter/--vector-table\r\n");
            return 1;
        }
#endif
        if (out == NULL) {
            /* No default name: a stray hello.svcm next to a hello.bin is
             * exactly the kind of file the user then installs by mistake. */
            ark_shell_printf("cc: --pack needs -o <file.svcm> (no default name)\r\n");
            return 1;
        }
        return cc_pack(src, out, target, opt_level, (int)verbose,
                       includes, n_includes, defines, n_defines);
    }
#endif
    /* The upstream STM32 entry point zeros this struct by hand because a
     * bare-metal build has no memset; a GCC build does, so memset here is
     * both shorter and complete. */
    memset(&opts, 0, sizeof(opts));
    opts.input_path     = src;
    opts.output_path    = out;
    opts.emit_asm       = emit_asm;
    opts.compile_only   = compile_only;
    opts.preprocess_only = preprocess_only;
    opts.verbose        = (int)verbose;
    opts.target         = target;
    opts.opt_level      = opt_level;
#if (CC_LAYOUT_OPTS == 1)
    opts.scatter_file   = scatter;
    opts.vector_table   = vector_table;
    if (link_mode != 0) {
        opts.link_mode        = 1;
        opts.link_input_count = link_input_count;
        for (i = 0; i < link_input_count; i++) {
            opts.link_inputs[i] = link_inputs[i];
        }
    }
#endif
    for (i = 0; i < n_includes && i < 16; i++) {
        opts.include_dirs[i] = includes[i];
    }
    opts.include_dir_count = n_includes;
    for (i = 0; i < n_defines && i < 64; i++) {
        opts.pre_defines[i] = defines[i];
    }
    opts.pre_define_count = n_defines;

    cc_reset_state(src, opt_level);

#if (CC_LAYOUT_OPTS == 1)
    if (link_mode != 0) {
        ark_shell_printf("cc: link %d obj -> %s\r\n",
                         link_input_count,
                         (out != NULL) ? out : "(default)");
    } else
#endif
    ark_shell_printf("cc: %s -> %s (target %s, -O%d)%s\r\n",
                     src,
                     (out != NULL) ? out : "(default)",
                     target, opt_level,
                     (emit_asm != 0) ? ", -S" :
                     (compile_only != 0) ? ", -c" :
                     (preprocess_only != 0) ? ", -E" : "");
#if (CC_LAYOUT_OPTS == 1)
    if (scatter != NULL) {
        /* Name the layout on the same line as the files: which base a binary
         * was linked for is the first thing to check when it will not run. */
        ark_shell_printf("cc: scatter %s\r\n", scatter);
    }
    if (vector_table != 0) {
        ark_shell_printf("cc: vector table in the first 0x400 B\r\n");
    }
#endif

    t0 = platform_tick_ms();
    arkc_port_set_abort_target(&pad);
    if (setjmp(pad) != 0) {
        /* Came back from ark_abort(). */
        /* Disarm before returning: this frame is about to go away, and a
         * pad pointing into a dead frame would corrupt the task on the next
         * fatal error. */
        arkc_port_set_abort_target(NULL);
        ark_shell_printf("cc: aborted: %s (%u ms)\r\n",
                         arkc_port_abort_message(),
                         (unsigned)(platform_tick_ms() - t0));
        cc_report_pool();
        return 1;
    }
    rc = ark_compile(&opts);
    arkc_port_set_abort_target(NULL);

    diag_summary();

    if (rc == ARK_OK) {
        cc_report_pool();
        ark_shell_printf("cc: done in %u ms\r\n",
                         (unsigned)(platform_tick_ms() - t0));
        return 0;
    }
    ark_shell_printf("cc: failed with code %d in %u ms\r\n",
                     rc, (unsigned)(platform_tick_ms() - t0));
    return 1;
}

int arkc_shell_register(int idx)
{
    if ((idx < 0) || ((idx + 1) > ARK_SHELL_MAX_COMMANDS)) {
        platform_uart_send_string("shell: no room for the cc command\r\n");
        return idx;
    }
    /* max_args counts the command name too, so 2 would allow only
     * "cc file.c" and reject every option as "too many arguments".  The
     * real limit is the shell's own argv array; cmd_cc reports what it
     * cannot honour. */
    g_cmd_table[idx++] = ARK_SHELL_CMD("cc", cmd_cc,
#if (CC_LAYOUT_OPTS == 1) && (ARKRTOS_USE_APPPACK == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]"
        " [--scatter f.sct] [--vector-table] [--pack -o x.svcm]",
#elif (CC_LAYOUT_OPTS == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]"
        " [--scatter f.sct] [--vector-table]",
#elif (ARKRTOS_USE_APPPACK == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]"
        " [--pack -o x.svcm]",
#else
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]",
#endif
        ARK_SHELL_MAX_ARGS);
    return idx;
}
