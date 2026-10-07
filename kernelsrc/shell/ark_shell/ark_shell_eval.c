/* ============================================================
 * ark_shell_eval.c - Command line evaluation
 *
 * Pipeline execution model
 * ------------------------
 * A stage is an ordinary command handler that writes to the console.  To
 * feed its output to the next stage we run it store-and-forward: the port
 * diverts every console write into a RAM buffer (ark_shell_capture_put),
 * and that buffer becomes the right stage's standard input.  Two buffers
 * are alternated so a stage never reads the buffer it is writing.
 *
 * This is a deliberate simplification.  There is no task per stage and no
 * streaming, so a stage whose output exceeds the buffer fails with a
 * message instead of quietly truncating.  On a 4 KB log ring that is the
 * honest behaviour: the alternative is a pipe that silently loses data.
 * ============================================================ */
#include "ark_shell_eval.h"
#include "ark_shell.h"
#include "ark_shell_commands.h"
#include "platform.h"
#include "arkrtos_types.h"

#include <string.h>
#include <stdio.h>

#if ARKRTOS_SHELL_REDIRECT
#include "arkrtos_vfs.h"
#endif

/* A line longer than ARK_SHELL_LINE_SIZE cannot produce more tokens than
 * this, and the operators themselves take tokens too. */
#define ARK_EVAL_MAX_TOKENS   24

/* Token kinds.  Words carry a decoded string; operators carry their code
 * and a NULL text. */
enum {
    EVAL_WORD = 0,
    EVAL_PIPE,      /* |                */
    EVAL_SEMI,      /* ;                */
    EVAL_AND,       /* &&               */
    EVAL_OR,        /* ||               */
    EVAL_OUT,       /* >                */
    EVAL_APPEND,    /* >>               */
    EVAL_IN         /* <                */
};

typedef struct {
    char         *text;
    unsigned char op;
} eval_tok_t;

/* ============================================================
 * Status bookkeeping ("$?")
 * ============================================================ */
static void eval_set_status(ark_shell_t *shell, int status)
{
    shell->last_status = status;
    (void)snprintf(shell->status_str, sizeof(shell->status_str), "%d", status);
}

static int eval_syntax(ark_shell_t *shell, const char *msg)
{
    ark_shell_printf("ark: syntax error: %s\r\n", msg);
    eval_set_status(shell, 2);
    return 2;
}

/* ============================================================
 * Output capture buffer (see ark_shell_eval.h)
 * ============================================================ */
#if ARKRTOS_SHELL_REDIRECT
static char   s_cap[2][ARKRTOS_SHELL_REDIRECT_BUF];
static int    s_cap_slot = -1;          /* -1: capture off */
static uint32 s_cap_len[2];
static int    s_cap_overflow;
#endif

int ark_shell_capture_active(void)
{
#if ARKRTOS_SHELL_REDIRECT
    return (s_cap_slot >= 0) ? 1 : 0;
#else
    return 0;
#endif
}

int ark_shell_capture_put(const unsigned char *p, unsigned int n)
{
#if ARKRTOS_SHELL_REDIRECT
    unsigned int i;
    uint32       slot;

    if (s_cap_slot < 0) {
        return 0;
    }
    slot = (uint32)s_cap_slot;
    for (i = 0u; i < n; i++) {
        if (s_cap_len[slot] < (uint32)ARKRTOS_SHELL_REDIRECT_BUF) {
            s_cap[slot][s_cap_len[slot]++] = (char)p[i];
        } else {
            s_cap_overflow = 1;
        }
    }
    return 1;
#else
    (void)p;
    (void)n;
    return 0;
#endif
}

/* ============================================================
 * Standard input of the current stage
 * ============================================================ */
static const char *s_in_buf;
static uint32      s_in_len;
static uint32      s_in_pos;

int ark_shell_stdin_getc(void)
{
    if (s_in_pos < s_in_len) {
        return (int)(unsigned char)s_in_buf[s_in_pos++];
    }
    return -1;
}

int ark_shell_stdin_ready(void)
{
    return (s_in_pos < s_in_len) ? 1 : 0;
}

#if ARKRTOS_SHELL_REDIRECT
static void stdin_set(const char *buf, uint32 len)
{
    s_in_buf = buf;
    s_in_len = len;
    s_in_pos = 0u;
}

static void stdin_clear(void)
{
    s_in_buf = 0;
    s_in_len = 0u;
    s_in_pos = 0u;
}
#endif /* ARKRTOS_SHELL_REDIRECT */

/* ============================================================
 * eval_tokenize - Split a line into words and operators
 *
 * Quotes and escapes are decoded in place by compaction (the write
 * cursor never overtakes the read cursor, so the buffer is safe):
 *   '...'   literal, no escapes
 *   "..."   literal except \" \\ \$
 *   \x      literal x (outside quotes)
 * An unterminated quote simply runs to the end of the line.  A lone '&'
 * is a syntax error rather than background execution, which this kernel
 * does not have.
 *
 * Returns the token count, or -1 when the line holds more tokens than
 * max (the caller reports it; it is not a crash).
 * ============================================================ */
static int eval_is_blank(char c)
{
    return (c == ' ' || c == '\t');
}

static int eval_is_meta(char c)
{
    return (c == '|' || c == ';' || c == '<' || c == '>' || c == '&');
}

/* Decode the operator whose first character is c0 and optional second
 * character is c1.  Returns how many characters it spans, or -1 when c0
 * is a lone '&' (this kernel has no job control).
 *
 * The characters are passed by value rather than as a pointer because the
 * caller may have saved them across an in-place terminator write. */
static int eval_op_span(char c0, char c1)
{
    if (c0 == '|') {
        return (c1 == '|') ? 2 : 1;
    }
    if (c0 == '>') {
        return (c1 == '>') ? 2 : 1;
    }
    if (c0 == '<') {
        return 1;
    }
    if (c0 == ';') {
        return 1;
    }
    return (c1 == '&') ? 2 : -1;
}

static unsigned char eval_op_kind(char c0, char c1)
{
    if (c0 == '|') {
        return (c1 == '|') ? EVAL_OR : EVAL_PIPE;
    }
    if (c0 == '>') {
        return (c1 == '>') ? EVAL_APPEND : EVAL_OUT;
    }
    if (c0 == '<') {
        return EVAL_IN;
    }
    if (c0 == ';') {
        return EVAL_SEMI;
    }
    return EVAL_AND;
}

static int eval_tokenize(char *line, eval_tok_t *toks, int max)
{
    char *r = line;
    char *w = line;
    int   n = 0;

    while (*r != '\0') {
        if (eval_is_blank(*r)) {
            r++;
            continue;
        }

        if (n >= max) {
            return -1;
        }

        if (eval_is_meta(*r)) {
            int span = eval_op_span(*r, r[1]);

            if (span < 0) {
                return -2;
            }
            toks[n].text = 0;
            toks[n].op = eval_op_kind(*r, r[1]);
            n++;
            r += span;
            continue;
        }

        /* A word: decode until whitespace or an unquoted metacharacter. */
        {
            char *start = w;

            toks[n].text = start;
            toks[n].op = EVAL_WORD;
            n++;

            while ((*r != '\0') && !eval_is_blank(*r) && !eval_is_meta(*r)) {
                char c = *r;

                if (c == '\\') {
                    r++;
                    if (*r == '\0') {
                        break;
                    }
                    *w++ = *r++;
                } else if (c == '\'') {
                    r++;
                    while ((*r != '\0') && (*r != '\'')) {
                        *w++ = *r++;
                    }
                    if (*r == '\'') {
                        r++;
                    }
                } else if (c == '"') {
                    r++;
                    while ((*r != '\0') && (*r != '"')) {
                        if ((*r == '\\') &&
                            ((r[1] == '"') || (r[1] == '\\') || (r[1] == '$'))) {
                            r++;
                            *w++ = *r++;
                        } else {
                            *w++ = *r++;
                        }
                    }
                    if (*r == '"') {
                        r++;
                    }
                } else {
                    *w++ = *r++;
                }
            }
            {
                char delim = *r;

                *w++ = '\0';
                if (delim == '\0') {
                    /* End of line: nothing left to scan. */
                } else if (eval_is_blank(delim)) {
                    r++;
                } else if (w - 1 == r) {
                    /* No compaction happened, so the terminator landed on
                     * the metacharacter itself and *r now reads '\0'.
                     * Decode the operator from the saved character and the
                     * untouched next byte, then step over it. */
                    int span = eval_op_span(delim, r[1]);

                    if (span < 0) {
                        return -2;
                    }
                    if (n >= max) {
                        return -1;
                    }
                    toks[n].text = 0;
                    toks[n].op = eval_op_kind(delim, r[1]);
                    n++;
                    r += span;
                }
                /* else: compaction moved the write cursor left, so r still
                 * points at the live metacharacter and the outer loop
                 * scans it normally. */
            }
        }
    }
    return n;
}

/* ============================================================
 * run_stage - Run one simple command (alias, $?, help, limits, handler)
 *
 * skip (may be NULL) marks tokens that belong to a redirection and must
 * not reach argv.
 * ============================================================ */
static int run_stage(ark_shell_t *shell, eval_tok_t *toks, int a, int b,
                     const unsigned char *skip)
{
    char *argv[ARK_SHELL_MAX_ARGS];
    char *xargv[ARK_SHELL_MAX_ARGS];
    int   argc = 0;
    int   xargc;
    int   i;
    const ark_shell_cmd_t *cmd;
    const char *alias_target;
    const char *alias_a1;
    const char *alias_a2;
    int   rc;

    for (i = a; i < b; i++) {
        if (toks[i].op != EVAL_WORD) {
            continue;
        }
        if ((skip != 0) && (skip[i] != 0u)) {
            continue;
        }
        if (argc >= ARK_SHELL_MAX_ARGS) {
            ark_shell_printf("ark: too many arguments (max %d)\r\n",
                             ARK_SHELL_MAX_ARGS - 1);
            eval_set_status(shell, 1);
            return 1;
        }
        argv[argc++] = toks[i].text;
    }

    if (argc == 0) {
        return 0;
    }

    /* Alias expansion: "ll" -> "ls -l", "cls" -> "clear", ... */
    alias_target = ark_shell_alias_target(argv[0], &alias_a1, &alias_a2);
    if (alias_target != 0) {
        xargc = 0;
        xargv[xargc++] = (char *)alias_target;
        if (alias_a1 != 0) {
            xargv[xargc++] = (char *)alias_a1;
        }
        if (alias_a2 != 0) {
            xargv[xargc++] = (char *)alias_a2;
        }
        for (i = 1; (i < argc) && (xargc < ARK_SHELL_MAX_ARGS); i++) {
            xargv[xargc++] = argv[i];
        }
    } else {
        for (i = 0; (i < argc) && (i < ARK_SHELL_MAX_ARGS); i++) {
            xargv[i] = argv[i];
        }
        xargc = argc;
    }

    /* "$?": a lone "$?" becomes the previous command's status. */
    for (i = 0; i < xargc; i++) {
        if (strcmp(xargv[i], "$?") == 0) {
            xargv[i] = shell->status_str;
        }
    }

    cmd = ark_shell_find_command(xargv[0]);
    if (cmd == 0) {
        ark_shell_printf("%s%s: command not found%s\r\n",
#if ARK_SHELL_ENABLE_COLOR
                         ARK_SHELL_COLOR_ERROR,
#else
                         "",
#endif
                         xargv[0],
#if ARK_SHELL_ENABLE_COLOR
                         ARK_SHELL_COLOR_RESET
#else
                         ""
#endif
                         );
        eval_set_status(shell, 127);
        return 127;
    }

    if ((xargc >= 2) &&
        ((strcmp(xargv[1], "-h") == 0) || (strcmp(xargv[1], "--help") == 0))) {
        ark_shell_printf("%s - %s\r\n", xargv[0], cmd->help);
        eval_set_status(shell, 0);
        return 0;
    }

    if ((cmd->max_args > 0) && (xargc > cmd->max_args)) {
        ark_shell_printf("%s%s: too many arguments (max %d)%s\r\n",
#if ARK_SHELL_ENABLE_COLOR
                         ARK_SHELL_COLOR_ERROR,
#else
                         "",
#endif
                         xargv[0], cmd->max_args,
#if ARK_SHELL_ENABLE_COLOR
                         ARK_SHELL_COLOR_RESET
#else
                         ""
#endif
                         );
        eval_set_status(shell, 1);
        return 1;
    }

    rc = cmd->func(xargc, xargv);
    eval_set_status(shell, rc);
    return rc;
}

#if ARKRTOS_SHELL_REDIRECT
/* ============================================================
 * Redirection helpers (VFS backed)
 * ============================================================ */
static int eval_vfs_path(ark_shell_t *shell, const char *in, char *out, uint32 cap)
{
    char rpath[ARK_SHELL_PATH_SIZE];

    ark_shell_resolve(shell, in, rpath, sizeof(rpath));
    if (strlen(rpath) >= (size_t)cap) {
        return -1;
    }
    (void)strcpy(out, rpath);
    return 0;
}

/* Read a whole file into buf.  A file that does not fit is an error, not a
 * prefix: feeding half a file to a pipeline is the kind of plausible wrong
 * answer this shell must not give. */
static int eval_redir_slurp(ark_shell_t *shell, const char *path, char *buf,
                            uint32 cap, uint32 *out_len)
{
    char   vpath[ARKRTOS_VFS_PATH_MAX];
    uint32 size = 0u;
    uint32 mode = 0u;
    int32  fd;
    int32  n = 0;
    uint32 total = 0u;

    if (eval_vfs_path(shell, path, vpath, sizeof(vpath)) != 0) {
        ark_shell_printf("ark: %s: path too long\r\n", path);
        return -1;
    }
    if (arkrtos_vfs_app_stat(vpath, &size, &mode) != 0) {
        ark_shell_printf("ark: %s: No such file or directory\r\n", path);
        return -1;
    }
    if ((mode & ARKRTOS_VFS_S_IFMT) == ARKRTOS_VFS_S_IFDIR) {
        ark_shell_printf("ark: %s: Is a directory\r\n", path);
        return -1;
    }
    if (size >= cap) {
        ark_shell_printf("ark: %s: %u bytes does not fit the %u byte buffer\r\n",
                         path, (unsigned)size, (unsigned)cap);
        return -1;
    }

    fd = arkrtos_vfs_open_read(vpath);
    if (fd < 0) {
        ark_shell_printf("ark: %s: %s\r\n", path, arkrtos_vfs_error_name(fd));
        return -1;
    }
    while (total < size) {
        n = arkrtos_vfs_read(fd, (uint8 *)buf + total, size - total);
        if (n <= 0) {
            break;
        }
        total += (uint32)n;
    }
    (void)arkrtos_vfs_close(fd);
    if (n < 0) {
        ark_shell_printf("ark: %s: read error\r\n", path);
        return -1;
    }

    *out_len = total;
    return 0;
}

static int eval_redir_store(ark_shell_t *shell, const char *path, const char *buf,
                            uint32 len, int append)
{
    char vpath[ARKRTOS_VFS_PATH_MAX];

    if (eval_vfs_path(shell, path, vpath, sizeof(vpath)) != 0) {
        ark_shell_printf("ark: %s: path too long\r\n", path);
        return -1;
    }

    if (append != 0) {
        int32 h = arkrtos_vfs_app_open(vpath, ARKRTOS_VFS_O_WRONLY |
                                       ARKRTOS_VFS_O_CREAT | ARKRTOS_VFS_O_APPEND);
        int32 w;
        int32 c;

        if (h < 0) {
            ark_shell_printf("ark: %s: %s\r\n", path, arkrtos_vfs_error_name(h));
            return -1;
        }
        w = (len > 0u) ? arkrtos_vfs_app_write(h, (const uint8 *)buf, len) : 0;
        c = arkrtos_vfs_app_close(h);
        if ((w < 0) || (c != 0)) {
            ark_shell_printf("ark: %s: write failed\r\n", path);
            return -1;
        }
    } else {
        int32 rc = arkrtos_vfs_write_file(vpath, (const uint8 *)buf, len);

        if (rc != 0) {
            ark_shell_printf("ark: %s: %s\r\n", path, arkrtos_vfs_error_name(rc));
            return -1;
        }
    }
    return 0;
}
#endif /* ARKRTOS_SHELL_REDIRECT */

/* ============================================================
 * run_command - Run one command: pipelines and redirection
 * ============================================================ */
static int run_command(ark_shell_t *shell, eval_tok_t *toks, int a, int b)
{
    int i;

#if ARKRTOS_SHELL_REDIRECT
    int           pipes[ARK_EVAL_MAX_TOKENS];
    int           npipes = 0;
    int           in_idx = -1;
    int           out_idx = -1;
    int           out_append = 0;
    int           in_slot = -1;
    int           out_slot = -1;
    int           nstages;
    int           stage;
    int           rc = 0;
    uint32        in_len = 0u;
    uint32        prev_len = 0u;
    unsigned char skip[ARK_EVAL_MAX_TOKENS];

    for (i = 0; i < ARK_EVAL_MAX_TOKENS; i++) {
        skip[i] = 0u;
    }

    for (i = a; i < b; i++) {
        if (toks[i].op == EVAL_PIPE) {
            if (npipes >= ARK_EVAL_MAX_TOKENS) {
                return eval_syntax(shell, "too many pipes");
            }
            pipes[npipes++] = i;
        } else if (toks[i].op == EVAL_IN) {
            if ((in_idx >= 0) || (i + 1 >= b) || (toks[i + 1].op != EVAL_WORD)) {
                return eval_syntax(shell, "'<' needs one file name");
            }
            in_idx = i;
            skip[i] = 1u;
            skip[i + 1] = 1u;
        } else if ((toks[i].op == EVAL_OUT) || (toks[i].op == EVAL_APPEND)) {
            if ((out_idx >= 0) || (i + 1 >= b) || (toks[i + 1].op != EVAL_WORD)) {
                return eval_syntax(shell, "'>' needs one file name");
            }
            out_idx = i;
            out_append = (toks[i].op == EVAL_APPEND) ? 1 : 0;
            skip[i] = 1u;
            skip[i + 1] = 1u;
        }
    }

    nstages = npipes + 1;

    if (in_idx >= 0) {
        if (eval_redir_slurp(shell, toks[in_idx + 1].text, s_cap[0],
                             (uint32)ARKRTOS_SHELL_REDIRECT_BUF, &in_len) != 0) {
            eval_set_status(shell, 1);
            return 1;
        }
        stdin_set(s_cap[0], in_len);
        in_slot = 0;
    } else {
        stdin_clear();
    }

    for (stage = 0; stage < nstages; stage++) {
        int sa = (stage == 0) ? a : (pipes[stage - 1] + 1);
        int sb = (stage == nstages - 1) ? b : pipes[stage];
        int words = 0;
        int capture;

        for (i = sa; i < sb; i++) {
            if ((toks[i].op == EVAL_WORD) && (skip[i] == 0u)) {
                words++;
            }
        }
        if (words == 0) {
            stdin_clear();
            return eval_syntax(shell, "empty command next to an operator");
        }

        if (stage > 0) {
            stdin_set(s_cap[in_slot], prev_len);
        }

        capture = ((stage < nstages - 1) || (out_idx >= 0)) ? 1 : 0;
        if (capture != 0) {
            out_slot = (in_slot == 0) ? 1 : 0;
            s_cap_slot = out_slot;
            s_cap_len[out_slot] = 0u;
            s_cap_overflow = 0;
        }

        rc = run_stage(shell, toks, sa, sb, skip);

        if (capture != 0) {
            s_cap_slot = -1;
            if (s_cap_overflow != 0) {
                stdin_clear();
                eval_set_status(shell, 2);
                ark_shell_printf("ark: a stage produced more than %u bytes "
                                 "(raise ARKRTOS_SHELL_REDIRECT_BUF)\r\n",
                                 (unsigned)ARKRTOS_SHELL_REDIRECT_BUF);
                return 2;
            }
            prev_len = s_cap_len[out_slot];
            in_slot = out_slot;
        }
    }

    stdin_clear();

    if (out_idx >= 0) {
        if (eval_redir_store(shell, toks[out_idx + 1].text, s_cap[out_slot],
                             s_cap_len[out_slot], out_append) != 0) {
            eval_set_status(shell, 1);
            return 1;
        }
    }

    return rc;
#else
    for (i = a; i < b; i++) {
        if ((toks[i].op == EVAL_PIPE) || (toks[i].op == EVAL_OUT) ||
            (toks[i].op == EVAL_APPEND) || (toks[i].op == EVAL_IN)) {
            ark_shell_printf("ark: pipes and redirection are disabled in this build\r\n");
            eval_set_status(shell, 2);
            return 2;
        }
    }
    return run_stage(shell, toks, a, b, 0);
#endif
}

/* ============================================================
 * ark_shell_eval_line - Run a whole line
 * ============================================================ */
int ark_shell_eval_line(ark_shell_t *shell, char *line)
{
    eval_tok_t toks[ARK_EVAL_MAX_TOKENS];
    int        n;
    int        i;
    int        cmd_start = 0;
    int        run = 1;
    int        status = 0;

    if ((shell == 0) || (line == 0)) {
        return 0;
    }

    n = eval_tokenize(line, toks, ARK_EVAL_MAX_TOKENS);
    if (n == -1) {
        return eval_syntax(shell, "too many tokens");
    }
    if (n == -2) {
        return eval_syntax(shell, "'&' is only valid as '&&' (no job control)");
    }
    if (n == 0) {
        eval_set_status(shell, 0);
        return 0;
    }

    for (i = 0; i < n; i++) {
        unsigned char op = toks[i].op;

        if ((op != EVAL_SEMI) && (op != EVAL_AND) && (op != EVAL_OR)) {
            continue;
        }
        if (run != 0) {
            status = run_command(shell, toks, cmd_start, i);
        }
        if (op == EVAL_AND) {
            run = (status == 0) ? 1 : 0;
        } else if (op == EVAL_OR) {
            run = (status == 0) ? 0 : 1;
        } else {
            run = 1;
        }
        cmd_start = i + 1;
    }

    if (cmd_start < n) {
        if (run != 0) {
            status = run_command(shell, toks, cmd_start, n);
        }
    }

    return status;
}
