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

/* Worst-case option storage; matches the fixed arrays in ArkOptions. */
#define CC_MAX_INCLUDE_DIRS   16
#define CC_MAX_DEFINES        64

static void cc_usage(void)
{
    ark_shell_printf(
        "usage: cc <file.c> [-o <out>] [-S] [-c] [-E] [-target arm]\r\n"
        "          [-O0|-O1|-O2] [-D<name>[=<value>]] [-I <dir>] [-v]\r\n"
#if (ARKRTOS_USE_APPRUN == 1)
        "          [--scatter <file.sct>] [--vector-table]\r\n"
        "          [-link <a.obj> [<b.obj> ...]]\r\n"
#endif
        "  no -o: writes <file>.bin (or prints to the console with -S/-E)\r\n"
        "  -o - : prints to the console (text as-is, binary as hex)\r\n"
#if (ARKRTOS_USE_APPRUN == 1)
        "  --scatter: link for that memory layout (needed for 'app run')\r\n"
        "  --vector-table: reserve 0x400 at the front of the image and put a\r\n"
        "         Cortex-M vector table there.  Use it whenever Reset_Handler is\r\n"
        "         defined: without it arkc prepends the table and shifts the image,\r\n"
        "         which leaves string-literal addresses 0x400 too low\r\n"
        "  -link: link .obj files into one image; every argument up to the next\r\n"
        "         option is an input.  Add --scatter so the image carries a vector\r\n"
        "         table for that window ('app run' reads SP/entry from it)\r\n"
#endif
        );
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
#if (ARKRTOS_USE_APPRUN == 1)
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
#if (ARKRTOS_USE_APPRUN == 1)
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
#if (ARKRTOS_USE_APPRUN == 1)
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

#if (ARKRTOS_USE_APPRUN == 1)
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
#if (ARKRTOS_USE_APPRUN == 1)
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

    /* Runtime feature defaults: the H743 has a double-precision FPU, so
     * hardware FPU code generation is on.  config_init() overwrites the
     * whole struct, so it must run before these two assignments. */
    config_init();
    g_config.enable_fpu_hw = 1;
    g_config.opt_level     = opt_level;

    /* Counters from a previous run must not leak into this one, and neither
     * must the contents of the NOLOAD compiler arena (see arkc_port.h): the
     * buffers arkc does not reset itself would otherwise start from whatever
     * the previous compile left behind. */
    arkc_port_zero_arena();
    diag_init(src, NULL);

#if (ARKRTOS_USE_APPRUN == 1)
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
#if (ARKRTOS_USE_APPRUN == 1)
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
#if (ARKRTOS_USE_APPRUN == 1)
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir] [--scatter f.sct] [--vector-table]",
#else
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]",
#endif
        ARK_SHELL_MAX_ARGS);
    return idx;
}
