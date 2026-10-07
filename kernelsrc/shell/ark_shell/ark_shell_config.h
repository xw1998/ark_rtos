/* ============================================================
 * ark_shell_config.h - Shell Global Configuration
 *
 * All tunable parameters are centralized here. Modify this file
 * when changing MCU or adjusting features.
 * ============================================================ */
#ifndef ARK_SHELL_CONFIG_H
#define ARK_SHELL_CONFIG_H

/* Pull the generated Kconfig header so a Kconfig override reaches this file.
 * Every default below is #ifndef-guarded, so an absent header changes nothing. */
#include "arkrtos_autoconf.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Basic Configuration ---- */

/* Maximum command line length (including '\0') */
#define ARK_SHELL_LINE_SIZE           128

/* Maximum number of arguments (including command name itself) */
#define ARK_SHELL_MAX_ARGS            16

/* Maximum capacity of command registry.  Must stay above the next free slot
 * plus the `need` upper bound in register_kernel_commands() (5 built-ins +
 * 44 kernel commands = 49), with headroom for app/driver commands that
 * share the same table. */
#define ARK_SHELL_MAX_COMMANDS        56

/* Prompt string (fallback when ARKRTOS_SHELL_PROMPT_LINUX is 0) */
#define ARK_SHELL_PROMPT              "ark> "

/* Linux-style prompt: user@host:cwd$ with a working directory. */
#ifndef ARKRTOS_SHELL_PROMPT_LINUX
#define ARKRTOS_SHELL_PROMPT_LINUX    1
#endif

/* Shell operators: quotes and escapes, command sequences (; && ||),
 * pipelines (|) and file redirection (> >> <).  Defaulted on here so
 * the framework still builds when no Kconfig header is generated. */
#ifndef ARKRTOS_SHELL_REDIRECT
#define ARKRTOS_SHELL_REDIRECT        1
#endif

/* Bytes per capture slot; a pipeline alternates two of them. */
#ifndef ARKRTOS_SHELL_REDIRECT_BUF
#define ARKRTOS_SHELL_REDIRECT_BUF    1024
#endif

/* Identity shown in the Linux-style prompt, and the working-directory limits. */
#define ARK_SHELL_USER                "ark"
#define ARK_SHELL_HOSTNAME            "arkrtos"
#define ARK_SHELL_HOME                "/"
#define ARK_SHELL_PATH_SIZE           96
#define ARK_SHELL_PROMPT_SIZE         96

/* Welcome message ----------------------------------------------------
 * Ark RTOS: the product banner goes here, not in arkrtos_shell.c, because
 * ark_shell_init() prints this string and only then draws the prompt -
 * anything the kernel task prints after init would land below an
 * already-drawn prompt.  The art is plain ASCII (figlet "standard",
 * 40 columns max); set ARK_SHELL_WELCOME_ART to 0 for the plain line.
 * ------------------------------------------------------------------- */
#define ARK_SHELL_WELCOME_ART         1
#if (ARK_SHELL_WELCOME_ART == 1)
#define ARK_SHELL_WELCOME             \
    "\r\n"                        \
    " ______     ______      _    ___  ____\r\n"  \
    "/ ___\\ \\   / / ___|_ __| |_ / _ \\/ ___|\r\n"  \
    "\\___ \\\\ \\ / / |   | '__| __| | | \\___ \\\r\n"  \
    " ___) |\\ V /| |___| |  | |_| |_| |___) |\r\n"  \
    "|____/  \\_/  \\____|_|   \\__|\\___/|____/\r\n"  \
    "\r\n"                        \
    "Ark RTOS kernel shell - type 'help' for commands.\r\n\r\n"
#else
#define ARK_SHELL_WELCOME             \
    "\r\n"                        \
    "Ark Shell v1.0\r\n"          \
    "Type 'help' for commands.\r\n\r\n"
#endif

/* ---- History System Configuration ---- */
#define ARK_SHELL_HISTORY_CAPACITY    8      /* Number of history entries.
                                              * Ark RTOS: lowered from upstream 32 to 8;
                                              * the history buffer lives in the kernel RAM
                                              * budget (8 x 128 = 1KB instead of 4KB). */
#define ARK_SHELL_HISTORY_LINE_SIZE   128    /* Max length per history entry */

/* ---- Autocomplete Configuration ---- */
#define ARK_SHELL_MAX_COMPLETIONS     16     /* Max candidates shown by Tab */
#define ARK_SHELL_TAB_COLS            4      /* Max columns per row in the Tab candidate list */
#define ARK_SHELL_TERM_COLS           80     /* Terminal width in columns; the candidate list is
                                              * packed to fit it (see ark_shell_autocomplete.c) */

/* ---- ESC Sequence Buffer ---- */
#define ARK_SHELL_ESC_BUF_SIZE        16     /* Max length of escape sequence */

/* ---- ESC Timeout (milliseconds) ---- */
#define ARK_SHELL_ESC_TIMEOUT_MS      50

/* ---- Feature Switches (1=enable, 0=disable) ---- */
#define ARK_SHELL_ENABLE_HISTORY      1      /* Command history */
#ifndef ARK_SHELL_ENABLE_AUTOCOMPLETE
#define ARK_SHELL_ENABLE_AUTOCOMPLETE 1  /* Tab autocomplete.  #ifndef-guarded so a
                                          * build may override it (a completer keeps
                                          * a candidate table in RAM). */
#endif
#define ARK_SHELL_ENABLE_HISTORY_SEARCH 0    /* Ctrl+R history search */
#define ARK_SHELL_ENABLE_COLOR        1      /* ANSI color output */

/* ---- Color Definitions ---- */
#if ARK_SHELL_ENABLE_COLOR
#define ARK_SHELL_COLOR_PROMPT        "\x1b[1;32m"  /* Bright green */
#define ARK_SHELL_COLOR_ERROR         "\x1b[1;31m"  /* Bright red */
#define ARK_SHELL_COLOR_INFO          "\x1b[36m"    /* Cyan */
#define ARK_SHELL_COLOR_RESET         "\x1b[0m"     /* Reset */
#else
#define ARK_SHELL_COLOR_PROMPT        ""
#define ARK_SHELL_COLOR_ERROR         ""
#define ARK_SHELL_COLOR_INFO          ""
#define ARK_SHELL_COLOR_RESET         ""
#endif

#ifdef __cplusplus
}
#endif

#endif /* ARK_SHELL_CONFIG_H */
