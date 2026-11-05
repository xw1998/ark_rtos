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
        "  no -o: writes <file>.bin (or prints to the console with -S/-E)\r\n"
        "  -o - : prints to the console (text as-is, binary as hex)\r\n");
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

    ark_shell_printf("cc: %s -> %s (target %s, -O%d)%s\r\n",
                     src,
                     (out != NULL) ? out : "(default)",
                     target, opt_level,
                     (emit_asm != 0) ? ", -S" :
                     (compile_only != 0) ? ", -c" :
                     (preprocess_only != 0) ? ", -E" : "");

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
        "Ark C compiler: cc <file.c> [-o out] [-S] [-c] [-D..] [-I dir]",
        ARK_SHELL_MAX_ARGS);
    return idx;
}
