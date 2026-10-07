/* ============================================================
 * ark_shell.c - Shell Main Framework Implementation
 *
 * Contains:
 *   - Byte state machine (NORMAL / ESC / ESC_BRACKET / SEARCH)
 *   - Main loop ark_shell_run (non-blocking, one byte per call)
 *   - Command execution ark_shell_execute_line (tokenize -> lookup -> execute)
 *   - Ctrl shortcut dispatch
 *   - ESC sequence handling (arrow keys/Home/End/Delete etc.)
 *   - Runtime UART mode switching
 *   - ark_shell_printf formatted output
 * ============================================================ */
#include "ark_shell.h"
#include "ark_shell_line_editor.h"
#include "ark_shell_history.h"
#include "ark_shell_commands.h"
#include "ark_shell_autocomplete.h"
#include "ark_shell_eval.h"
#include "ark_shell_config.h"
#include "platform.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ============================================================
 * Helper: send carriage return + newline
 * ============================================================ */
static void shell_send_newline(void)
{
    platform_uart_send_string("\r\n");
}

/* ============================================================
 * Helper: case-insensitive string equality
 * ============================================================ */
static int shell_streqci(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == *b);
}

/* ============================================================
 * Command aliases (Linux habits)
 *
 * Each alias expands to a target command plus up to two fixed leading
 * arguments, so "ll" runs "ls -l" and "cls" runs "clear".
 * ============================================================ */
typedef struct {
    const char *name;
    const char *target;
    const char *arg1;
    const char *arg2;
} ark_shell_alias_t;

static const ark_shell_alias_t g_aliases[] = {
    { "ll",       "ls",     "-l", 0 },
    { "la",       "ls",     "-a", 0 },
    { "dir",      "ls",      0,   0 },
    { "cls",      "clear",   0,   0 },
    { "halt",     "reboot",  0,   0 },
    { "poweroff", "reboot",  0,   0 },
    { 0, 0, 0, 0 }
};

const char *ark_shell_alias_target(const char *name,
                                   const char **arg1, const char **arg2)
{
    int i;

    if (arg1 != 0) *arg1 = 0;
    if (arg2 != 0) *arg2 = 0;
    if (name == 0) {
        return 0;
    }
    for (i = 0; g_aliases[i].name != 0; i++) {
        if (shell_streqci(g_aliases[i].name, name)) {
            if (arg1 != 0) *arg1 = g_aliases[i].arg1;
            if (arg2 != 0) *arg2 = g_aliases[i].arg2;
            return g_aliases[i].target;
        }
    }
    return 0;
}

/* ============================================================
 * Helper: build the prompt into shell->prompt_buf
 *
 * Linux-style: "user@host:cwd$ ", where cwd shows "~" at HOME and the
 * absolute path elsewhere.  With ARKRTOS_SHELL_PROMPT_LINUX off it is the
 * plain ARK_SHELL_PROMPT.
 * ============================================================ */
static void shell_build_prompt(ark_shell_t *shell)
{
#if ARKRTOS_SHELL_PROMPT_LINUX
    const char *disp = (strcmp(shell->cwd, ARK_SHELL_HOME) == 0) ? "~" : shell->cwd;
    (void)snprintf(shell->prompt_buf, sizeof(shell->prompt_buf), "%s@%s:%s$ ",
                   ARK_SHELL_USER, ARK_SHELL_HOSTNAME, disp);
#else
    (void)snprintf(shell->prompt_buf, sizeof(shell->prompt_buf), "%s",
                   ARK_SHELL_PROMPT);
#endif
}

/* ============================================================
 * Helper: set the last command status ("$?")
 * ============================================================ */
static void shell_set_status(ark_shell_t *shell, int status)
{
    shell->last_status = status;
    (void)snprintf(shell->status_str, sizeof(shell->status_str), "%d", status);
}

/* ============================================================
 * Helper: show prompt
 * ============================================================ */
static void shell_show_prompt(ark_shell_t *shell)
{
    shell_build_prompt(shell);
    platform_console_begin();
#if ARK_SHELL_ENABLE_COLOR
    platform_uart_send_string(ARK_SHELL_COLOR_PROMPT);
    platform_uart_send_string(shell->prompt_buf);
    platform_uart_send_string(ARK_SHELL_COLOR_RESET);
#else
    platform_uart_send_string(shell->prompt_buf);
#endif
    platform_console_end();
}

/* ============================================================
 * ark_shell_init - Initialize Shell
 * ============================================================ */
void ark_shell_set_completer(ark_shell_t *shell, ark_shell_complete_fn fn)
{
    if (shell != NULL) {
        shell->complete_fn = fn;
    }
}

void ark_shell_init(ark_shell_t *shell, uint32_t baudrate)
{
    if (shell == NULL) {
        return;
    }

    /* Initialize platform UART */
    platform_uart_init(baudrate);

    /* Initialize subsystems.  The editor points at the runtime prompt
     * buffer; its contents are rebuilt by shell_build_prompt() whenever the
     * working directory changes. */
    (void)strncpy(shell->cwd, ARK_SHELL_HOME, sizeof(shell->cwd) - 1u);
    shell->cwd[sizeof(shell->cwd) - 1u] = '\0';
    shell_set_status(shell, 0);
    shell_build_prompt(shell);
    line_editor_init(&shell->editor, shell->prompt_buf);
    history_init(&shell->history);

    /* Initialize state machine */
    shell->state          = STATE_NORMAL;
    shell->ctrl_c_flag    = 0;
    shell->uart_mode      = platform_uart_get_mode();
    shell->esc_len        = 0;
    shell->esc_start_time = 0;
    shell->complete_fn    = NULL;

#if ARK_SHELL_ENABLE_HISTORY_SEARCH
    shell->search_buf[0] = '\0';
    shell->search_len    = 0;
    shell->search_match  = NULL;
#endif

    /* Initialize command table */
    ark_shell_commands_init();

    /* Show welcome message */
    platform_console_begin();
    platform_uart_send_string(ARK_SHELL_WELCOME);
    platform_console_end();

    /* Show prompt */
    shell_show_prompt(shell);
}

/* ============================================================
 * ark_shell_execute_line - Execute current command in editor
 *
 * Steps: add to history -> tokenize -> lookup -> execute -> clear editor
 * ============================================================ */
void ark_shell_execute_line(ark_shell_t *shell)
{
    char *line;

    if (shell == NULL) {
        return;
    }

    line = line_editor_get_string(&shell->editor);

    /* Add to history (empty lines are skipped) */
    history_add(&shell->history, line);

    shell_send_newline();

    /* ark_shell_eval_line() takes over from here: quoting, command
     * sequences, pipelines and redirection all live in ark_shell_eval.c. */
    (void)ark_shell_eval_line(shell, line);

    /* Clear editor, prepare for next line */
    line_editor_clear(&shell->editor);
    shell_show_prompt(shell);
}

/* ============================================================
 * shell_handle_ctrl - Handle Ctrl shortcuts
 *
 * Parameters: shell, ch - letter part of Ctrl+letter (already lowercased)
 * Returns: 0=handled, -1=not recognized
 * ============================================================ */
static int shell_handle_ctrl(ark_shell_t *shell, char ch)
{
    switch (ch) {
    case 'a':  /* Ctrl+A: home */
        line_editor_move_home(&shell->editor);
        return 0;
    case 'b':  /* Ctrl+B: left */
        line_editor_move_left(&shell->editor);
        return 0;
    case 'c':  /* Ctrl+C: interrupt current input */
        shell->ctrl_c_flag = 1;
        shell_send_newline();
        line_editor_clear(&shell->editor);
        shell_show_prompt(shell);
        return 0;
    case 'd':  /* Ctrl+D: delete char at cursor (bash default).  On an empty
                * line it means EOF; there is no process to end here, so it is
                * a no-op rather than an error or a spurious logout. */
        if (shell->editor.len == 0) {
            return 0;
        }
        line_editor_delete(&shell->editor);
        return 0;
    case 'e':  /* Ctrl+E: end */
        line_editor_move_end(&shell->editor);
        return 0;
    case 'f':  /* Ctrl+F: right */
        line_editor_move_right(&shell->editor);
        return 0;
    case 'k':  /* Ctrl+K: delete to end */
        line_editor_delete_to_end(&shell->editor);
        return 0;
    case 'l':  /* Ctrl+L: clear screen */
        line_editor_clear_screen(&shell->editor);
        return 0;
#if ARK_SHELL_ENABLE_HISTORY_SEARCH
    case 'r':  /* Ctrl+R: history search */
        shell->state = STATE_SEARCH;
        shell->search_buf[0] = '\0';
        shell->search_len    = 0;
        shell->search_match  = NULL;
        platform_uart_send_string("\r\n(reverse-i-search): ");
        return 0;
#endif
    case 'n':  /* Ctrl+N: next history (same as Down arrow) */
        history_down(&shell->history, &shell->editor);
        return 0;
    case 'p':  /* Ctrl+P: previous history (same as Up arrow) */
        history_up(&shell->history, &shell->editor);
        return 0;
    case 'u':  /* Ctrl+U: delete to home */
        line_editor_delete_to_home(&shell->editor);
        return 0;
    case 'w':  /* Ctrl+W: delete previous word */
        line_editor_delete_word(&shell->editor);
        return 0;
    default:
        return -1;
    }
}

/* ============================================================
 * shell_handle_esc_sequence - Handle ESC sequence
 *
 * Handles:
 *   ESC [ A  - Up arrow       ESC [ B  - Down arrow
 *   ESC [ C  - Right arrow    ESC [ D  - Left arrow
 *   ESC [ H  - Home           ESC [ F  - End
 *   ESC [ 3~ - Delete
 *   ESC O H  - Home (xterm)   ESC O F  - End (xterm)
 * ============================================================ */
static void shell_handle_esc_sequence(ark_shell_t *shell)
{
    const char *seq = shell->esc_buf;
    int len = shell->esc_len;

    if (len < 2) {
        return;
    }

    /* esc_buf holds the sequence *without* the leading ESC, e.g. "[D" for the
     * Left arrow, so the final byte lives at seq[len - 1]. */
    if (seq[0] == '[' || seq[0] == 'O') {
        char fin = seq[len - 1];
        if (len == 2) {
            switch (fin) {
            case 'A':  /* Up arrow */
                history_up(&shell->history, &shell->editor);
                break;
            case 'B':  /* Down arrow */
                history_down(&shell->history, &shell->editor);
                break;
            case 'C':  /* Right arrow */
                line_editor_move_right(&shell->editor);
                break;
            case 'D':  /* Left arrow */
                line_editor_move_left(&shell->editor);
                break;
            case 'H':  /* Home */
                line_editor_move_home(&shell->editor);
                break;
            case 'F':  /* End */
                line_editor_move_end(&shell->editor);
                break;
            default:
                break;
            }
        } else if (len == 3 && seq[1] == '3' && seq[2] == '~') {
            /* Delete key */
            line_editor_delete(&shell->editor);
        }
    }
}

/* ============================================================
 * ark_shell_process_byte - Process a single byte (state machine entry)
 *
 * State transitions:
 *   NORMAL:
 *     0x1B       -> ESC (record timestamp)
 *     0x09       -> Tab completion
 *     0x0D/0x0A  -> execute command
 *     0x7F/0x08  -> backspace
 *     0x01~0x1A  -> Ctrl+letter
 *     0x20~0x7E  -> printable character
 *   ESC:
 *     [ / O      -> ESC_BRACKET
 *     other      -> back to NORMAL
 *   ESC_BRACKET:
 *     collect until sequence complete, then handle and back to NORMAL
 *   SEARCH (Ctrl+R):
 *     0x0D       -> accept match
 *     0x7F       -> delete search char
 *     other      -> exit search
 * ============================================================ */
void ark_shell_process_byte(ark_shell_t *shell, unsigned char byte)
{
    uint32_t now;

    if (shell == NULL) {
        return;
    }

    /* ESC timeout detection: return to NORMAL if in ESC/ESC_BRACKET too long */
    if (shell->state == STATE_ESC || shell->state == STATE_ESC_BRACKET) {
        now = platform_tick_ms();
        if (now - shell->esc_start_time > ARK_SHELL_ESC_TIMEOUT_MS) {
            shell->state = STATE_NORMAL;
        }
    }

    switch (shell->state) {
    /* -------------------- NORMAL state -------------------- */
    case STATE_NORMAL:
        if (byte == 0x1B) {
            /* Enter ESC state */
            shell->state = STATE_ESC;
            shell->esc_len = 0;
            shell->esc_start_time = platform_tick_ms();
            return;
        }
        if (byte == '\t') {
            /* Tab completion */
#if ARK_SHELL_ENABLE_AUTOCOMPLETE
            ark_shell_autocomplete(shell);
#else
            platform_uart_send('\a');
#endif
            return;
        }
        if (byte == '\r' || byte == '\n') {
            /* Enter: execute command */
            ark_shell_execute_line(shell);
            return;
        }
        if (byte == 0x7F || byte == 0x08) {
            /* Backspace */
            line_editor_backspace(&shell->editor);
            return;
        }
        if (byte >= 0x01 && byte <= 0x1A) {
            /* Ctrl+letter (A=1, B=2, ..., Z=26) */
            char ch = (char)('a' + byte - 1);
            shell_handle_ctrl(shell, ch);
            return;
        }
        if (byte >= 0x20 && byte <= 0x7E) {
            /* Printable character */
            line_editor_insert_char(&shell->editor, (char)byte);
            return;
        }
        /* Other control characters ignored */
        break;

    /* -------------------- ESC state -------------------- */
    case STATE_ESC:
        if (byte == '[' || byte == 'O') {
            /* Enter ESC_BRACKET state */
            shell->esc_buf[shell->esc_len++] = (char)byte;
            shell->state = STATE_ESC_BRACKET;
            shell->esc_start_time = platform_tick_ms();
        } else {
            /* Single-char ESC sequence, e.g. ESC pressed alone */
            shell->state = STATE_NORMAL;
        }
        break;

    /* -------------------- ESC_BRACKET state -------------------- */
    case STATE_ESC_BRACKET:
        if (shell->esc_len < ARK_SHELL_ESC_BUF_SIZE - 1) {
            shell->esc_buf[shell->esc_len++] = (char)byte;
        }
        /* Determine if sequence is complete:
         *   - letter (A~Z, a~z) => end
         *   - '~' => end
         *   - digit => continue
         *   - ';' => continue (combo keys, e.g. ESC[1;5C = Ctrl+Right)
         */
        if ((byte >= 'A' && byte <= 'Z') ||
            (byte >= 'a' && byte <= 'z') ||
            byte == '~') {
            /* Sequence complete */
            shell->esc_buf[shell->esc_len] = '\0';
            shell_handle_esc_sequence(shell);
            shell->state = STATE_NORMAL;
        }
        break;

    /* -------------------- SEARCH state (Ctrl+R) -------------------- */
    case STATE_SEARCH:
#if ARK_SHELL_ENABLE_HISTORY_SEARCH
        if (byte == '\r' || byte == '\n') {
            /* Accept match */
            if (shell->search_match != NULL) {
                line_editor_set_string(&shell->editor, shell->search_match);
            }
            shell_send_newline();
            shell_show_prompt(shell);
            shell->state = STATE_NORMAL;
        } else if (byte == 0x7F || byte == 0x08) {
            /* Delete search char */
            if (shell->search_len > 0) {
                shell->search_len--;
                shell->search_buf[shell->search_len] = '\0';
                shell->search_match = history_search(&shell->history, shell->search_buf);
                platform_uart_send_string("\r(reverse-i-search): ");
                platform_uart_send_string(shell->search_buf);
                platform_uart_send_string(" > ");
                if (shell->search_match != NULL) {
                    platform_uart_send_string(shell->search_match);
                }
            }
        } else if (byte == 0x1B) {
            /* ESC to exit search */
            shell_send_newline();
            line_editor_clear(&shell->editor);
            shell_show_prompt(shell);
            shell->state = STATE_NORMAL;
        } else if (byte >= 0x20 && byte <= 0x7E) {
            /* Add search char */
            if (shell->search_len < ARK_SHELL_LINE_SIZE - 1) {
                shell->search_buf[shell->search_len++] = (char)byte;
                shell->search_buf[shell->search_len] = '\0';
                shell->search_match = history_search(&shell->history, shell->search_buf);
                platform_uart_send_string("\r(reverse-i-search): ");
                platform_uart_send_string(shell->search_buf);
                platform_uart_send_string(" > ");
                if (shell->search_match != NULL) {
                    platform_uart_send_string(shell->search_match);
                }
            }
        }
#else
        shell->state = STATE_NORMAL;
#endif
        break;

    default:
        shell->state = STATE_NORMAL;
        break;
    }
}

/* ============================================================
 * ark_shell_run - Shell main loop (non-blocking)
 *
 * Reads one byte from UART and processes it. Returns immediately
 * if no data. Call repeatedly in main() while(1) loop.
 * ============================================================ */
void ark_shell_run(ark_shell_t *shell)
{
    int byte;

    if (shell == NULL) {
        return;
    }

    byte = platform_uart_recv();
    if (byte < 0) {
        return;  /* No data */
    }

    ark_shell_process_byte(shell, (unsigned char)byte);
}

/* ============================================================
 * ark_shell_set_uart_mode - Switch UART working mode at runtime
 * ============================================================ */
void ark_shell_set_uart_mode(ark_shell_t *shell, uart_mode_t mode)
{
    if (shell == NULL) {
        return;
    }

    platform_uart_set_mode(mode);
    shell->uart_mode = platform_uart_get_mode();

    shell_send_newline();
    platform_uart_send_string("UART mode switched to: ");
    switch (shell->uart_mode) {
    case UART_MODE_POLL:
        platform_uart_send_string("POLL");
        break;
    case UART_MODE_IRQ:
        platform_uart_send_string("IRQ");
        break;
    case UART_MODE_DMA:
        platform_uart_send_string("DMA");
        break;
    default:
        platform_uart_send_string("UNKNOWN");
        break;
    }
    platform_uart_send_string("\r\n");
    shell_show_prompt(shell);
}

/* ============================================================
 * ark_shell_printf - Formatted output to UART
 *
 * Depends on platform vsnprintf. Can be disabled on constrained platforms.
 * ============================================================ */
void ark_shell_printf(const char *fmt, ...)
{
    char buf[ARK_SHELL_LINE_SIZE * 2];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len > 0) {
        if (len > (int)sizeof(buf)) {
            len = (int)sizeof(buf);
        }
        platform_uart_send_buf((const unsigned char *)buf, (uint16_t)len);
    }
}


/* ============================================================
 * Working directory / path helpers (see ark_shell.h)
 * ============================================================ */
const char *ark_shell_home(void)
{
    return ARK_SHELL_HOME;
}

void ark_shell_get_cwd(const ark_shell_t *shell, char *out, uint32_t cap)
{
    if (out == 0 || cap == 0u) {
        return;
    }
    if (shell == 0) {
        (void)strncpy(out, ARK_SHELL_HOME, cap - 1u);
        out[cap - 1u] = '\0';
        return;
    }
    (void)strncpy(out, shell->cwd, cap - 1u);
    out[cap - 1u] = '\0';
}

void ark_shell_set_cwd(ark_shell_t *shell, const char *path)
{
    if (shell == 0 || path == 0) {
        return;
    }
    (void)strncpy(shell->cwd, path, sizeof(shell->cwd) - 1u);
    shell->cwd[sizeof(shell->cwd) - 1u] = '\0';
    shell_build_prompt(shell);
}

int ark_shell_last_status(const ark_shell_t *shell)
{
    return (shell != 0) ? shell->last_status : 0;
}

void ark_shell_resolve(const ark_shell_t *shell, const char *in,
                       char *out, uint32_t cap)
{
    char        tmp[ARK_SHELL_PATH_SIZE * 2];
    const char *base;
    const char *rel;
    const char *p;
    uint32_t    oi;

    if (out == 0 || cap < 2u) {
        return;
    }
    out[0] = '\0';

    if (in == 0 || in[0] == '\0') {
        base = (shell != 0) ? shell->cwd : ARK_SHELL_HOME;
        rel  = "";
    } else if (in[0] == '~' && (in[1] == '\0' || in[1] == '/')) {
        base = ARK_SHELL_HOME;
        rel  = (in[1] == '/') ? (in + 2) : "";
    } else if (in[0] == '/') {
        base = "";
        rel  = in;
    } else {
        base = (shell != 0) ? shell->cwd : ARK_SHELL_HOME;
        rel  = in;
    }

    if (rel[0] == '\0') {
        (void)snprintf(tmp, sizeof(tmp), "%s", (base[0] != '\0') ? base : "/");
    } else if (base[0] == '\0') {
        (void)snprintf(tmp, sizeof(tmp), "/%s", rel);
    } else {
        (void)snprintf(tmp, sizeof(tmp), "%s/%s", base, rel);
    }

    /* Normalise: keep a leading '/', drop empty and '.', pop on '..'. */
    oi = 1u;
    out[0] = '/';
    p = tmp;
    while (*p != '\0') {
        const char *seg;
        uint32_t    seglen;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        seg = p;
        while (*p != '\0' && *p != '/') {
            p++;
        }
        seglen = (uint32_t)(p - seg);

        if (seglen == 1u && seg[0] == '.') {
            continue;
        }
        if (seglen == 2u && seg[0] == '.' && seg[1] == '.') {
            while (oi > 1u && out[oi - 1u] != '/') {
                oi--;
            }
            if (oi > 1u) {
                oi--;
            }
            continue;
        }
        if (oi > 1u) {
            if (oi + 1u >= cap) {
                break;
            }
            out[oi++] = '/';
        }
        if (oi + seglen >= cap) {
            break;
        }
        memcpy(&out[oi], seg, seglen);
        oi += seglen;
    }
    out[oi] = '\0';
}
