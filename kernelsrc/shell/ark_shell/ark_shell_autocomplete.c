/* ============================================================
 * ark_shell_autocomplete.c - Tab Autocomplete Implementation
 *
 * Strategy:
 *   1. Extract the "current word" before the cursor
 *   2. Gather candidates:
 *        - first word  -> the command table
 *        - later words -> an external provider (shell->complete_fn),
 *                         e.g. the VFS path completer
 *   3. Single match: complete and add a trailing space (or stop at '/' for a
 *      directory, so the next Tab descends into it)
 *   4. Multiple matches: complete the common prefix;
 *      press Tab twice to list all candidates
 *
 * The core knows nothing about filesystems: a provider returns plain strings,
 * and a directory candidate simply ends with '/'.
 * ============================================================ */
#include "ark_shell_autocomplete.h"
#include "ark_shell.h"
#include "platform.h"
#include "ark_shell_commands.h"
#include "ark_shell_config.h"
#include "ark_shell_line_editor.h"

#include <string.h>

#if ARK_SHELL_ENABLE_AUTOCOMPLETE

/* ============================================================
 * Helper: check if character is a word separator (space/tab)
 * ============================================================ */
static int is_separator(char c)
{
    return (c == ' ' || c == '\t');
}

/* ============================================================
 * Helper: compute common prefix length of two strings
 * ============================================================ */
static int common_prefix_len(const char *a, const char *b)
{
    int n = 0;
    while (a[n] != '\0' && b[n] != '\0' && a[n] == b[n]) {
        n++;
    }
    return n;
}

/* Print the candidate list packed into columns the way bash does.  The column
 * width follows the longest candidate so the table stays aligned; the column
 * count is capped by ARK_SHELL_TAB_COLS and by what fits in ARK_SHELL_TERM_COLS. */
static void list_candidates(char cand[][ARK_SHELL_LINE_SIZE], int n)
{
    int cols;
    int colw = 0;
    int col = 0;
    int i;

    for (i = 0; i < n; i++) {
        int l = (int)strlen(cand[i]);
        if (l > colw) {
            colw = l;
        }
    }
    colw += 2;

    cols = ARK_SHELL_TERM_COLS / colw;
    if (cols > ARK_SHELL_TAB_COLS) {
        cols = ARK_SHELL_TAB_COLS;
    }
    if (cols < 1) {
        cols = 1;
    }

    platform_uart_send_string("\r\n");
    for (i = 0; i < n; i++) {
        int pad = colw - (int)strlen(cand[i]);
        platform_uart_send_string(cand[i]);
        while (pad-- > 0) {
            platform_uart_send(' ');
        }
        col++;
        if (col >= cols) {
            platform_uart_send_string("\r\n");
            col = 0;
        }
    }
    if (col != 0) {
        platform_uart_send_string("\r\n");
    }
}

/* Redraw the prompt and the current line, then move the cursor back to its
 * logical position.  Used after listing candidates. */
static void redraw_line(line_editor_t *le)
{
    int back = le->len - le->pos;

    platform_uart_send_string(le->prompt);
    platform_uart_send_buf((const unsigned char *)le->buf, (uint16_t)le->len);
    if (back > 0) {
        char seq[16];
        int k = 0;
        int v = back;
        char tmp[12];
        int tl = 0;
        int m;

        seq[k++] = '\x1b';
        seq[k++] = '[';
        if (v == 0) {
            tmp[tl++] = '0';
        }
        while (v > 0) {
            tmp[tl++] = (char)('0' + v % 10);
            v /= 10;
        }
        for (m = tl - 1; m >= 0; m--) {
            seq[k++] = tmp[m];
        }
        seq[k++] = 'D';
        platform_uart_send_buf((const unsigned char *)seq, (uint16_t)k);
    }
}

/* ============================================================
 * ark_shell_autocomplete - Perform Tab completion
 * ============================================================ */
void ark_shell_autocomplete(struct ark_shell_s *shell)
{
    static char cand[ARK_SHELL_COMPLETE_MAX][ARK_SHELL_LINE_SIZE];
    line_editor_t *le;
    char word[ARK_SHELL_LINE_SIZE];
    int  word_len;
    int  word_start;
    int  i;
    int  n = 0;
    int  common_len = 0;
    int  is_first_word;
    static char s_last_prefix[ARK_SHELL_LINE_SIZE] = "";
    static int  s_last_listed = 0;
    int  prefix_unchanged;

    if (shell == NULL) {
        return;
    }
    le = &shell->editor;

    /* ---- 1. Extract current word ---- */
    word_start = le->pos;
    while (word_start > 0 && !is_separator(le->buf[word_start - 1])) {
        word_start--;
    }
    word_len = le->pos - word_start;

    for (i = 0; i < word_len && i < ARK_SHELL_LINE_SIZE - 1; i++) {
        word[i] = le->buf[word_start + i];
    }
    word[i] = '\0';

    is_first_word = 1;
    for (i = 0; i < word_start; i++) {
        if (!is_separator(le->buf[i])) {
            is_first_word = 0;
            break;
        }
    }

    /* ---- 2. Gather candidates ---- */
    if (is_first_word) {
        for (i = 0; i < g_cmd_count && n < ARK_SHELL_COMPLETE_MAX; i++) {
            const char *nm = g_cmd_table[i].name;
            if (nm != NULL && strncmp(nm, word, (size_t)word_len) == 0) {
                strncpy(cand[n], nm, ARK_SHELL_LINE_SIZE - 1);
                cand[n][ARK_SHELL_LINE_SIZE - 1] = '\0';
                n++;
            }
        }
    } else if (shell->complete_fn != NULL) {
        n = shell->complete_fn(shell, word, 0, cand, ARK_SHELL_COMPLETE_MAX);
        if (n < 0) {
            n = 0;
        }
        if (n > ARK_SHELL_COMPLETE_MAX) {
            n = ARK_SHELL_COMPLETE_MAX;
        }
    }

    if (n == 0) {
        /* No candidate: bell */
        platform_uart_send('\a');
        return;
    }

    /* ---- 3. Common prefix over all candidates ---- */
    common_len = (int)strlen(cand[0]);
    for (i = 1; i < n; i++) {
        int plen = common_prefix_len(cand[0], cand[i]);
        if (plen < common_len) {
            common_len = plen;
        }
    }

    prefix_unchanged = (strcmp(word, s_last_prefix) == 0);

    if (n == 1) {
        /* Single match: extend to the full candidate.  A directory candidate
         * ends with '/', and stopping there lets the next Tab descend into
         * it; anything else gets a trailing space like a finished word. */
        const char *full = cand[0];
        int full_len = (int)strlen(full);
        int j;

        for (j = word_len; j < full_len; j++) {
            line_editor_insert_char(le, full[j]);
        }
        if (full_len == 0 || full[full_len - 1] != '/') {
            line_editor_insert_char(le, ' ');
        }
        s_last_prefix[0] = '\0';
        s_last_listed = 0;
        return;
    }

    /* Multiple matches */
    if (common_len > word_len) {
        /* Extend to the common prefix and remember it. */
        int j;
        for (j = word_len; j < common_len; j++) {
            line_editor_insert_char(le, cand[0][j]);
        }
        for (j = 0; j < common_len && j < ARK_SHELL_LINE_SIZE - 1; j++) {
            s_last_prefix[j] = cand[0][j];
        }
        s_last_prefix[j] = '\0';
        s_last_listed = 0;
        return;
    }

    /* Common prefix already complete: list on the first Tab, and stay quiet on
     * a repeat Tab with the very same prefix. */
    if (prefix_unchanged && s_last_listed) {
        platform_uart_send('\a');
        return;
    }
    if (prefix_unchanged || common_len == word_len) {
        list_candidates(cand, n);
        redraw_line(le);
        for (i = 0; i < common_len && i < ARK_SHELL_LINE_SIZE - 1; i++) {
            s_last_prefix[i] = cand[0][i];
        }
        s_last_prefix[i] = '\0';
        s_last_listed = 1;
    } else {
        int j;
        for (j = 0; j < common_len && j < ARK_SHELL_LINE_SIZE - 1; j++) {
            s_last_prefix[j] = cand[0][j];
        }
        s_last_prefix[j] = '\0';
        s_last_listed = 0;
    }
}
#endif
