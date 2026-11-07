/* ============================================================
 * arkvim_modes.c - Editor State Machine and Main Loop
 *
 * Implements the three Vim modes (Normal / Insert / Command) over
 * the gap buffer, line table and renderer. The editor runs its own
 * blocking loop using the shell's platform UART, so it naturally
 * "takes over" the terminal until the user quits, then returns to
 * the shell.
 * ============================================================ */
#include "arkvim_modes.h"
#include "arkvim_config.h"
#include "arkvim_terminal.h"
#include "arkvim_renderer.h"
#include "arkvim_fileio.h"
#include "arkvim_undo.h"
#include "arkvim_syntax.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

static arkvim_editor_t s_editor;

/* ============================================================
 * Status message
 * ============================================================ */
void arkvim_editor_set_status(arkvim_editor_t *e, const char *msg)
{
    if (e == NULL || msg == NULL) {
        return;
    }
    /* Safe truncate. vsnprintf not needed for plain copy. */
    strncpy(e->status_msg, msg, ARKVIM_STATUS_MSG_SIZE - 1);
    e->status_msg[ARKVIM_STATUS_MSG_SIZE - 1] = '\0';
}

static void statusf(arkvim_editor_t *e, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->status_msg, ARKVIM_STATUS_MSG_SIZE, fmt, ap);
    va_end(ap);
}

/* ============================================================
 * Geometry helpers
 * ============================================================ */
/* Width of the line-number gutter in columns: the digits of the
 * largest line number plus one separating space. Small buffers keep a
 * 3-digit column so the text does not shift when the file grows, and
 * the column widens by one at 1000 / 10000 lines. */
static int gutter_width(arkvim_editor_t *e)
{
    int digits = 1;
    int t;

    if (!e->show_numbers) {
        return 0;
    }
    t = (int)lt_line_count(&e->lt, &e->gb);
    while (t >= 10) {
        t /= 10;
        digits++;
    }
    if (digits < 3) {
        digits = 3;
    }
    if (digits > 5) {          /* ARKVIM_LT_MAX_LINES is far below this */
        digits = 5;
    }
    return digits + 1;
}

static int text_cols(arkvim_editor_t *e)
{
    return ARKVIM_SCREEN_COLS - gutter_width(e);
}

/* Length of a line excluding its trailing newline. */
static uint32_t line_text_len(arkvim_editor_t *e, uint16_t row)
{
    uint32_t off = lt_line_offset(&e->lt, &e->gb, row);
    uint32_t len = lt_line_length(&e->lt, &e->gb, row);
    if (len > 0 && gb_get(&e->gb, off + len - 1) == '\n') {
        len--;
    }
    return len;
}

/* ============================================================
 * Cursor movement
 * ============================================================ */
static void cursor_clamp_col(arkvim_editor_t *e, uint16_t row, uint16_t *col)
{
    uint32_t llen = line_text_len(e, row);
    if (*col > llen) {
        *col = (uint16_t)llen;
    }
}

static void move_vert(arkvim_editor_t *e, int dy)
{
    uint16_t row, col;
    uint16_t total;
    int nr;

    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    total = lt_line_count(&e->lt, &e->gb);
    nr = (int)row + dy;
    if (nr < 0) {
        nr = 0;
    }
    if (nr >= total) {
        nr = total - 1;
    }
    if (nr < 0) {
        return;   /* empty buffer */
    }
    cursor_clamp_col(e, (uint16_t)nr, &col);
    e->cursor = lt_pos_to_offset(&e->lt, &e->gb, (uint16_t)nr, col);
}

static void move_horiz(arkvim_editor_t *e, int dx)
{
    uint16_t row, col;
    uint32_t llen;

    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    llen = line_text_len(e, row);
    int nc = (int)col + dx;
    if (nc < 0) {
        nc = 0;
    }
    if (nc > (int)llen) {
        nc = (int)llen;
    }
    e->cursor = lt_pos_to_offset(&e->lt, &e->gb, row, (uint16_t)nc);
}

static void move_line_start(arkvim_editor_t *e)
{
    uint16_t row, col;
    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    e->cursor = lt_line_offset(&e->lt, &e->gb, row);
}

static void move_first_nonblank(arkvim_editor_t *e)
{
    uint16_t row, col;
    uint32_t off;
    uint32_t llen;
    uint32_t i;

    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    off  = lt_line_offset(&e->lt, &e->gb, row);
    llen = line_text_len(e, row);
    for (i = 0; i < llen; i++) {
        char c = gb_get(&e->gb, off + i);
        if (c != ' ' && c != '\t') {
            break;
        }
    }
    e->cursor = off + i;
}

static void move_line_end(arkvim_editor_t *e)
{
    uint16_t row, col;
    uint32_t llen;

    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    llen = line_text_len(e, row);
    e->cursor = lt_line_offset(&e->lt, &e->gb, row) + (llen > 0 ? llen - 1 : 0);
}

static void move_eof(arkvim_editor_t *e)
{
    e->cursor = gb_length(&e->gb);
    /* Vim leaves cursor on the last line's last char, not past it. */
    if (e->cursor > 0) {
        e->cursor--;
    }
    move_line_start(e);
}

static bool is_word_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* Move forward to the start of the next word, `repeat` times. */
static void move_word_fwd(arkvim_editor_t *e, int repeat)
{
    while (repeat-- > 0) {
        char c = gb_get(&e->gb, e->cursor);
        bool in_word = is_word_char(c);
        /* skip current word */
        while (e->cursor < gb_length(&e->gb)) {
            c = gb_get(&e->gb, e->cursor);
            if (in_word != is_word_char(c)) {
                break;
            }
            if (c == ' ' || c == '\t' || c == '\n') {
                break;
            }
            e->cursor++;
        }
        /* skip whitespace */
        while (e->cursor < gb_length(&e->gb)) {
            c = gb_get(&e->gb, e->cursor);
            if (c != ' ' && c != '\t' && c != '\n') {
                break;
            }
            e->cursor++;
        }
    }
}

/* Move back to the start of the previous word. */
static void move_word_back(arkvim_editor_t *e, int repeat)
{
    while (repeat-- > 0) {
        if (e->cursor == 0) {
            break;
        }
        e->cursor--;
        /* skip whitespace */
        while (e->cursor > 0) {
            char c = gb_get(&e->gb, e->cursor);
            if (c != ' ' && c != '\t' && c != '\n') {
                break;
            }
            e->cursor--;
        }
        /* skip word */
        while (e->cursor > 0) {
            char c = gb_get(&e->gb, e->cursor - 1);
            if (c == ' ' || c == '\t' || c == '\n' ||
                is_word_char(c) != is_word_char(gb_get(&e->gb, e->cursor))) {
                /* stop at boundary */
                if (is_word_char(c) == is_word_char(gb_get(&e->gb, e->cursor)) &&
                    c != ' ' && c != '\t' && c != '\n') {
                    e->cursor--;
                    continue;
                }
                break;
            }
            e->cursor--;
        }
    }
}

/* Move forward to the end of the current/next word. */
static void move_word_end(arkvim_editor_t *e, int repeat)
{
    while (repeat-- > 0) {
        if (e->cursor + 1 >= gb_length(&e->gb)) {
            break;
        }
        e->cursor++;
        /* skip whitespace */
        while (e->cursor < gb_length(&e->gb)) {
            char c = gb_get(&e->gb, e->cursor);
            if (c != ' ' && c != '\t' && c != '\n') {
                break;
            }
            e->cursor++;
        }
        /* advance through the word */
        {
            char c = gb_get(&e->gb, e->cursor);
            bool in_word = is_word_char(c);
            while (e->cursor + 1 < gb_length(&e->gb)) {
                char n = gb_get(&e->gb, e->cursor + 1);
                if (n == ' ' || n == '\t' || n == '\n') {
                    break;
                }
                if (is_word_char(n) != in_word) {
                    break;
                }
                e->cursor++;
            }
        }
    }
}

/* ============================================================
 * Edit primitives (record undo)
 * ============================================================ */
static void edit_insert_char(arkvim_editor_t *e, char c)
{
#if ARKVIM_ENABLE_UNDO
    gb_insert_char(&e->gb, e->cursor, c);
    undo_push_insert(e, e->cursor, 1);
#else
    gb_insert_char(&e->gb, e->cursor, c);
#endif
    e->cursor++;
    lt_mark_dirty(&e->lt);
    e->dirty = true;
    ren_mark_all_dirty();
}

static void edit_insert_str(arkvim_editor_t *e, const char *s, uint32_t n)
{
    uint32_t pos = e->cursor;
    gb_insert_str(&e->gb, pos, s, n);
#if ARKVIM_ENABLE_UNDO
    undo_push_insert(e, pos, (uint16_t)n);
#endif
    e->cursor = pos + n;
    lt_mark_dirty(&e->lt);
    e->dirty = true;
    ren_mark_all_dirty();
}

static void edit_delete_at(arkvim_editor_t *e, uint32_t pos)
{
#if ARKVIM_ENABLE_UNDO
    undo_push_delete(e, pos, 1);
#endif
    gb_delete_at(&e->gb, pos);
    lt_mark_dirty(&e->lt);
    e->dirty = true;
    ren_mark_all_dirty();
}

static void edit_delete_range(arkvim_editor_t *e, uint32_t start, uint32_t end)
{
    if (start >= end) {
        return;
    }
#if ARKVIM_ENABLE_UNDO
    undo_push_delete(e, start, (uint16_t)(end - start));
#endif
    gb_delete_range(&e->gb, start, end);
    if (e->cursor > end) {
        e->cursor -= (end - start);
    } else if (e->cursor > start) {
        e->cursor = start;
    }
    lt_mark_dirty(&e->lt);
    e->dirty = true;
    ren_mark_all_dirty();
}

/* ============================================================
 * Yank / paste
 * ============================================================ */
static void yank_range(arkvim_editor_t *e, uint32_t start, uint32_t end, bool linewise)
{
    uint32_t n = end - start;
    if (n > ARKVIM_YANK_MAX) {
        n = ARKVIM_YANK_MAX;
    }
    gb_copy_range(&e->gb, start, start + n, e->yank_buf, ARKVIM_YANK_MAX);
    e->yank_len      = n;
    e->yank_linewise = linewise;
}

static void paste(arkvim_editor_t *e)
{
    if (e->yank_len == 0) {
        return;
    }
    if (e->yank_linewise) {
        /* Insert the yanked lines below the current line. */
        uint16_t row, col;
        uint32_t off;
        uint32_t len;

        lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
        off = lt_line_offset(&e->lt, &e->gb, row);
        len = lt_line_length(&e->lt, &e->gb, row);  /* includes trailing \n */
        e->cursor = off + len;                       /* start of next line */
        edit_insert_str(e, e->yank_buf, e->yank_len);
    } else {
        /* Insert after the cursor. */
        if (e->cursor < gb_length(&e->gb)) {
            e->cursor++;
        }
        edit_insert_str(e, e->yank_buf, e->yank_len);
    }
}

/* ============================================================
 * Drawing
 * ============================================================ */
static void draw_rows(arkvim_editor_t *e)
{
    uint16_t total = lt_line_count(&e->lt, &e->gb);
    int gw = gutter_width(e);
    int tc = text_cols(e);
    int y;

    for (y = 0; y < ARKVIM_EDIT_ROWS; y++) {
        int filerow = (int)e->scroll_row + y;
        int col = 0;

        if (filerow >= total) {
            ren_set_cell(y, 0, '~', ARKVIM_ATTR_NORMAL);
            for (col = 1; col < ARKVIM_SCREEN_COLS; col++) {
                ren_set_cell(y, col, ' ', ARKVIM_ATTR_NORMAL);
            }
            ren_mark_row_dirty(y);
            continue;
        }

        /* line number gutter */
        if (gw > 0) {
            char num[8];
            int i;
            int n = snprintf(num, sizeof(num), "%*d ", gw - 1, filerow + 1);
            for (i = 0; i < gw; i++) {
                char c = (i < n) ? num[i] : ' ';
                ren_set_cell(y, i, c, ARKVIM_ATTR_NORMAL);
            }
        }

        {
            uint32_t off = lt_line_offset(&e->lt, &e->gb, (uint16_t)filerow);
            uint32_t llen = line_text_len(e, (uint16_t)filerow);
            char line_buf[ARKVIM_SCREEN_COLS + 1];
            uint8_t attr_buf[ARKVIM_SCREEN_COLS];
            uint32_t copy = llen;
            int vis_col;
            bool in_block = false;   /* TODO: carry across rows */

            if (copy > (uint32_t)tc) {
                copy = (uint32_t)tc;
            }
            /* read the visible slice, honouring horizontal scroll */
            {
                uint32_t sc = e->scroll_col;
                if (sc > llen) {
                    sc = llen;
                }
                gb_copy_range(&e->gb, off + sc, off + sc + copy,
                              line_buf, sizeof(line_buf));
            }

#if ARKVIM_ENABLE_SYNTAX
            syntax_highlight_line(line_buf, (int)copy, attr_buf, &in_block);
#else
            {
                int i;
                for (i = 0; i < (int)copy; i++) {
                    attr_buf[i] = ARKVIM_ATTR_NORMAL;
                }
            }
#endif
            for (vis_col = 0; vis_col < (int)copy; vis_col++) {
                char ch = line_buf[vis_col];

                /* Never hand a control byte to the terminal as a glyph.
                 * Files written on this board by the shell's `echo` carry
                 * CRLF line ends, so a CR is an ordinary byte inside the
                 * gap buffer; painting it raw sends 0x0D down the wire,
                 * and the terminal answers by jumping to column 1 -- the
                 * row then shows a hole and the rest of the line lands
                 * on top of the gutter. One placeholder cell keeps the
                 * screen truthful and the terminal state intact. */
                if (((unsigned char)ch < 0x20) || (ch == 0x7F)) {
                    /* TAB is included on purpose: the terminal would expand
                     * it to its own tab stops and every later column in this
                     * row (and the cursor we put there) would be off by a
                     * few cells. Expanding tabs properly is a separate
                     * change; until then they show up as '?'. */
                    ch = '?';
                }
                ren_set_cell(y, gw + vis_col, ch, attr_buf[vis_col]);
            }
            for (; gw + vis_col < ARKVIM_SCREEN_COLS; vis_col++) {
                ren_set_cell(y, gw + vis_col, ' ', ARKVIM_ATTR_NORMAL);
            }
        }
        ren_mark_row_dirty(y);
    }
}

static void draw_status_bar(arkvim_editor_t *e)
{
    int row = ARKVIM_STATUS_ROW;
    uint16_t r, c;
    int n = 0;
    char buf[ARKVIM_SCREEN_COLS + 1];
    const char *mode_name;
    const char *name;
    int col;

    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &r, &c);

    mode_name = (e->mode == ARKVIM_MODE_INSERT) ? "-- INSERT --" :
                (e->mode == ARKVIM_MODE_COMMAND) ? "-- COMMAND --" : "";
    name = e->has_filename ? e->filename : "[No Name]";

    n = snprintf(buf, sizeof(buf), " %s  %.30s%s  %uL",
                 mode_name, name, e->dirty ? " [+]" : "",
                 lt_line_count(&e->lt, &e->gb));

    /* right-aligned position */
    {
        char rbuf[32];
        int rlen = snprintf(rbuf, sizeof(rbuf), "%u:%u ",
                            (unsigned)(r + 1), (unsigned)(c + 1));
        int pad = ARKVIM_SCREEN_COLS - n - rlen;
        int i;
        if (pad < 0) {
            pad = 0;
        }
        for (i = 0; i < pad && n < ARKVIM_SCREEN_COLS; i++) {
            buf[n++] = ' ';
        }
        for (i = 0; i < rlen && n < ARKVIM_SCREEN_COLS; i++) {
            buf[n++] = rbuf[i];
        }
    }

    for (col = 0; col < ARKVIM_SCREEN_COLS; col++) {
        char ch = (col < n) ? buf[col] : ' ';
        ren_set_cell(row, col, ch, ARKVIM_ATTR_REVERSE);
    }
    ren_mark_row_dirty(row);
}

static void draw_cmd_line(arkvim_editor_t *e)
{
    int row = ARKVIM_CMDLINE_ROW;
    int col;

    for (col = 0; col < ARKVIM_SCREEN_COLS; col++) {
        ren_set_cell(row, col, ' ', ARKVIM_ATTR_NORMAL);
    }

    if (e->mode == ARKVIM_MODE_COMMAND) {
        /* cmd_buf holds the whole line *including* its ':' or '/'
         * prefix and cmd_len counts that prefix, so paint it as-is.
         * (Painting the prefix by hand and then copying cmd_len
         * characters from cmd_buf[1] showed ":q" as "::q", dropped
         * the last character of a long command, and pushed the
         * string's terminating NUL out to the terminal for "/".) */
        int n = e->cmd_len;
        if (n > ARKVIM_SCREEN_COLS) {
            n = ARKVIM_SCREEN_COLS;
        }
        ren_set_text(row, 0, e->cmd_buf, n, ARKVIM_ATTR_NORMAL);
    } else if (e->status_msg[0] != '\0') {
        int n = (int)strlen(e->status_msg);
        if (n > ARKVIM_SCREEN_COLS) {
            n = ARKVIM_SCREEN_COLS;
        }
        ren_set_text(row, 0, e->status_msg, n, ARKVIM_ATTR_NORMAL);
    }
    ren_mark_row_dirty(row);
}

static void refresh_screen(arkvim_editor_t *e)
{
    uint16_t row, col;
    int tc = text_cols(e);
    int gw = gutter_width(e);

    if (e->lt.dirty) {
        lt_rebuild(&e->lt, &e->gb);
    }

    /* adjust vertical scroll */
    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    if (row < e->scroll_row) {
        e->scroll_row = row;
        ren_mark_all_dirty();
    }
    if (row >= e->scroll_row + ARKVIM_EDIT_ROWS) {
        e->scroll_row = (uint16_t)(row - ARKVIM_EDIT_ROWS + 1);
        ren_mark_all_dirty();
    }
    /* adjust horizontal scroll */
    if (col < e->scroll_col) {
        e->scroll_col = col;
        ren_mark_all_dirty();
    }
    if (col >= e->scroll_col + tc) {
        e->scroll_col = (uint16_t)(col - tc + 1);
        ren_mark_all_dirty();
    }

    draw_rows(e);
    draw_status_bar(e);
    draw_cmd_line(e);

    e->screen_cy = (int)(row - e->scroll_row) + 1;
    e->screen_cx = gw + (int)(col - e->scroll_col) + 1;
    ren_set_cursor(e->screen_cy, e->screen_cx);
    ren_refresh();
}

/* ============================================================
 * Search
 * ============================================================ */
static bool search_forward(arkvim_editor_t *e, const char *pat)
{
    uint32_t plen = (uint32_t)strlen(pat);
    uint32_t start = e->cursor + 1;
    uint32_t i;
    uint32_t len = gb_length(&e->gb);

    if (plen == 0 || plen > len) {
        return false;
    }

    for (i = start; i + plen <= len; i++) {
        uint32_t j;
        bool match = true;
        for (j = 0; j < plen; j++) {
            if (gb_get(&e->gb, i + j) != pat[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            e->cursor = i;
            ren_mark_all_dirty();
            return true;
        }
    }
    /* wrap around */
    for (i = 0; i + plen <= start && i + plen <= len; i++) {
        uint32_t j;
        bool match = true;
        for (j = 0; j < plen; j++) {
            if (gb_get(&e->gb, i + j) != pat[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            e->cursor = i;
            ren_mark_all_dirty();
            return true;
        }
    }
    return false;
}

/* ============================================================
 * Operator + motion application
 * ============================================================ */
/* Compute the target offset of a motion from the current cursor.
 * Returns true and fills `target` / `linewise` if recognised. */
static bool motion_target(arkvim_editor_t *e, char m, int repeat,
                          uint32_t *target, bool *linewise)
{
    uint32_t before = e->cursor;
    switch (m) {
    case 'w':
        move_word_fwd(e, repeat);
        *target = e->cursor;
        *linewise = false;
        break;
    case 'b':
        move_word_back(e, repeat);
        *target = e->cursor;
        *linewise = false;
        break;
    case 'e':
        move_word_end(e, repeat);
        *target = e->cursor + 1;   /* include the word end char */
        *linewise = false;
        break;
    case '$':
        move_line_end(e);
        *target = e->cursor + 1;
        *linewise = false;
        break;
    case '0':
        move_line_start(e);
        *target = e->cursor;
        *linewise = false;
        break;
    case '^':
        move_first_nonblank(e);
        *target = e->cursor;
        *linewise = false;
        break;
    case 'h':
        move_horiz(e, -repeat);
        *target = e->cursor;
        *linewise = false;
        break;
    case 'l':
        move_horiz(e, repeat);
        *target = e->cursor + 1;
        *linewise = false;
        break;
    case 'j':
    case 'k':
        move_vert(e, (m == 'j') ? repeat : -repeat);
        *target = e->cursor;
        *linewise = true;
        break;
    case 'G':
        move_eof(e);
        *target = e->cursor;
        *linewise = true;
        break;
    default:
        e->cursor = before;
        return false;
    }
    e->cursor = before;
    return true;
}

static void apply_operator(arkvim_editor_t *e, char op, uint32_t start, uint32_t end,
                           bool linewise)
{
    if (start > end) {
        uint32_t t = start;
        start = end;
        end = t;
    }
    if (linewise) {
        /* expand to whole lines */
        uint16_t sr, sc;
        lt_offset_to_pos(&e->lt, &e->gb, start, &sr, &sc);
        start = lt_line_offset(&e->lt, &e->gb, sr);
        {
            uint16_t er, ec;
            lt_offset_to_pos(&e->lt, &e->gb, end, &er, &ec);
            if (er + 1 < lt_line_count(&e->lt, &e->gb)) {
                end = lt_line_offset(&e->lt, &e->gb, (uint16_t)(er + 1));
            } else {
                end = gb_length(&e->gb);
            }
        }
    }

    switch (op) {
    case 'd':
        yank_range(e, start, end, linewise);
        edit_delete_range(e, start, end);
        break;
    case 'y':
        yank_range(e, start, end, linewise);
        arkvim_editor_set_status(e, linewise ? "1 line yanked" : "yanked");
        break;
    case 'c':
        yank_range(e, start, end, linewise);
        edit_delete_range(e, start, end);
        e->mode = ARKVIM_MODE_INSERT;
        break;
    }
}

static void line_operator(arkvim_editor_t *e, char op, int repeat)
{
    uint16_t row, col;
    uint16_t total = lt_line_count(&e->lt, &e->gb);
    uint32_t start, end;

    lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
    start = lt_line_offset(&e->lt, &e->gb, row);
    {
        uint16_t end_row = (uint16_t)(row + repeat);
        if (end_row >= total) {
            end_row = total;
        }
        end = lt_line_offset(&e->lt, &e->gb, end_row);
    }
    apply_operator(e, op, start, end, true);
}

/* ============================================================
 * Normal mode
 * ============================================================ */
static void normal_enter_insert(arkvim_editor_t *e, char kind)
{
    switch (kind) {
    case 'i':
        break;
    case 'a':
        if (e->cursor < gb_length(&e->gb)) {
            e->cursor++;
        }
        break;
    case 'I':
        move_first_nonblank(e);
        break;
    case 'A':
        move_line_end(e);
        if (e->cursor < gb_length(&e->gb)) {
            e->cursor++;
        }
        break;
    case 'o':
        move_line_end(e);
        if (e->cursor < gb_length(&e->gb)) {
            e->cursor++;
        }
        edit_insert_char(e, '\n');
        break;
    case 'O':
        move_line_start(e);
        edit_insert_char(e, '\n');
        e->cursor--;
        break;
    default:
        break;
    }
    e->mode = ARKVIM_MODE_INSERT;
    arkvim_editor_set_status(e, "-- INSERT --");
}

static void process_normal(arkvim_editor_t *e, int key)
{
    /* count prefix: digits build the count, but a leading '0' is
     * the "move to first column" command. */
    if (key >= '1' && key <= '9') {
        e->count = e->count * 10 + (key - '0');
        return;
    }
    if (key == '0' && e->count > 0) {
        e->count = e->count * 10;
        return;
    }

    {
        int repeat = (e->count > 0) ? e->count : 1;

        /* operator-pending state */
        if (e->pending_op != 0) {
            char op = e->pending_op;
            e->pending_op = 0;
            if (key == op) {
                /* linewise: dd / yy / cc */
                line_operator(e, op, repeat);
            } else {
                uint32_t target;
                bool linewise;
                if (motion_target(e, (char)key, repeat, &target, &linewise)) {
                    apply_operator(e, op, e->cursor, target, linewise);
                } else {
                    arkvim_editor_set_status(e, "Invalid motion");
                }
            }
            e->count = 0;
            return;
        }

        /* start of an operator? */
        if (key == 'd' || key == 'y' || key == 'c') {
            e->pending_op = (char)key;
            return;
        }

        switch (key) {
        case 'h':
        case ARKVIM_KEY_ARROW_LEFT:
            move_horiz(e, -repeat);
            break;
        case 'l':
        case ARKVIM_KEY_ARROW_RIGHT:
            move_horiz(e, repeat);
            break;
        case 'j':
        case ARKVIM_KEY_ARROW_DOWN:
            move_vert(e, repeat);
            break;
        case 'k':
        case ARKVIM_KEY_ARROW_UP:
            move_vert(e, -repeat);
            break;
        case 'w':
            move_word_fwd(e, repeat);
            break;
        case 'b':
            move_word_back(e, repeat);
            break;
        case 'e':
            move_word_end(e, repeat);
            break;
        case '0':
            move_line_start(e);
            break;
        case '$':
            move_line_end(e);
            break;
        case '^':
            move_first_nonblank(e);
            break;
        case 'G':
            if (e->count > 0) {
                uint16_t total = lt_line_count(&e->lt, &e->gb);
                uint16_t target = (uint16_t)(e->count - 1);
                if (target >= total) {
                    target = (uint16_t)(total - 1);
                }
                e->cursor = lt_line_offset(&e->lt, &e->gb, target);
            } else {
                move_eof(e);
            }
            break;
        case 'g':
            /* gg: go to first line */
            e->cursor = 0;
            break;
        case 'x':
            {
                uint16_t row, col;
                uint32_t llen;
                int n = repeat;
                lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
                llen = line_text_len(e, row);
                while (n-- > 0 && col < llen) {
                    edit_delete_at(e, e->cursor);
                    llen = line_text_len(e, row);
                }
                /* if we deleted past the line end, step left */
                if (col >= llen && col > 0 && e->cursor > 0) {
                    e->cursor--;
                }
            }
            break;
        case 'X':
            {
                int n = repeat;
                while (n-- > 0 && e->cursor > 0) {
                    e->cursor--;
                    edit_delete_at(e, e->cursor);
                }
            }
            break;
        case 'p':
            while (repeat-- > 0) {
                paste(e);
            }
            break;
        case 'u':
            {
                int n = repeat;
                bool any = false;
                while (n-- > 0) {
                    if (!undo_execute(e)) {
                        break;
                    }
                    any = true;
                }
                if (!any) {
                    arkvim_editor_set_status(e, "Already at oldest change");
                }
            }
            break;
        case ARKVIM_KEY_CTRL_R:
            {
                int n = repeat;
                bool any = false;
                while (n-- > 0) {
                    if (!redo_execute(e)) {
                        break;
                    }
                    any = true;
                }
                if (!any) {
                    arkvim_editor_set_status(e, "Already at newest change");
                }
            }
            break;
        case ARKVIM_KEY_CTRL_U:
            {
                uint16_t row, col;
                uint32_t line_off;
                lt_offset_to_pos(&e->lt, &e->gb, e->cursor, &row, &col);
                line_off = lt_line_offset(&e->lt, &e->gb, row);
                if (e->cursor > line_off) {
                    edit_delete_range(e, line_off, e->cursor);
                    e->cursor = line_off;
                }
            }
            break;
        case 'i': case 'a': case 'I': case 'A': case 'o': case 'O':
            normal_enter_insert(e, (char)key);
            break;
        case ':':
        case '/':
            e->mode = ARKVIM_MODE_COMMAND;
            e->cmd_buf[0] = (char)key;
            e->cmd_len = 1;
            e->cmd_buf[1] = '\0';
            e->count = 0;
            return;
        case 'n':
            if (e->last_search[0] != '\0') {
                if (!search_forward(e, e->last_search)) {
                    arkvim_editor_set_status(e, "Pattern not found");
                }
            }
            break;
        case ARKVIM_KEY_PAGE_UP:
            move_vert(e, -ARKVIM_EDIT_ROWS);
            break;
        case ARKVIM_KEY_PAGE_DOWN:
            move_vert(e, ARKVIM_EDIT_ROWS);
            break;
        case ARKVIM_KEY_HOME:
            move_line_start(e);
            break;
        case ARKVIM_KEY_END:
            move_line_end(e);
            break;
        default:
            break;
        }
    }

    e->count = 0;
}

/* ============================================================
 * Insert mode
 * ============================================================ */
static void process_insert(arkvim_editor_t *e, int key)
{
    switch (key) {
    case ARKVIM_KEY_ESC:
    case ARKVIM_KEY_CTRL_C:
        e->mode = ARKVIM_MODE_NORMAL;
        /* Vim moves left one column on leaving insert (if possible). */
        if (e->cursor > 0) {
            e->cursor--;
        }
        arkvim_editor_set_status(e, "");
        break;

    case ARKVIM_KEY_ENTER:
    case ARKVIM_KEY_NL:
        edit_insert_char(e, '\n');
        break;

    case ARKVIM_KEY_BACKSPACE:
    case ARKVIM_KEY_CTRL_H:
        if (e->cursor > 0) {
            e->cursor--;
            edit_delete_at(e, e->cursor);
        }
        break;

    case ARKVIM_KEY_DELETE:
        if (e->cursor < gb_length(&e->gb)) {
            edit_delete_at(e, e->cursor);
        }
        break;

    case ARKVIM_KEY_TAB:
        edit_insert_str(e, "    ", 4);
        break;

    case ARKVIM_KEY_ARROW_LEFT:
        if (e->cursor > 0) {
            e->cursor--;
        }
        break;
    case ARKVIM_KEY_ARROW_RIGHT:
        if (e->cursor < gb_length(&e->gb)) {
            e->cursor++;
        }
        break;
    case ARKVIM_KEY_ARROW_UP:
        move_vert(e, -1);
        break;
    case ARKVIM_KEY_ARROW_DOWN:
        move_vert(e, 1);
        break;

    default:
        if (key >= 0x20 && key <= 0x7E) {
            edit_insert_char(e, (char)key);
        }
        break;
    }
}

/* ============================================================
 * Command mode (Ex commands + search)
 * ============================================================ */
static void exec_ex(arkvim_editor_t *e, const char *cmd)
{
    if (cmd[0] == '\0') {
        return;
    }

    /* search line: "/pattern" */
    if (cmd[0] == '/') {
        const char *pat = cmd + 1;
        strncpy(e->last_search, pat, ARKVIM_SEARCH_MAX - 1);
        e->last_search[ARKVIM_SEARCH_MAX - 1] = '\0';
        if (!search_forward(e, pat)) {
            arkvim_editor_set_status(e, "Pattern not found");
        } else {
            arkvim_editor_set_status(e, "");
        }
        return;
    }

    /* drop leading ':' */
    if (cmd[0] == ':') {
        cmd++;
    }

    /* bare number => goto line */
    if (cmd[0] >= '0' && cmd[0] <= '9') {
        unsigned int ln = 0;
        const char *p = cmd;
        while (*p >= '0' && *p <= '9') {
            ln = ln * 10u + (unsigned int)(*p - '0');
            p++;
        }
        {
            uint16_t total = lt_line_count(&e->lt, &e->gb);
            uint16_t target = (ln == 0) ? 0 : (uint16_t)(ln - 1);
            if (target >= total) {
                target = (uint16_t)(total - 1);
            }
            e->cursor = lt_line_offset(&e->lt, &e->gb, target);
        }
        return;
    }

    if (strcmp(cmd, "w") == 0) {
        if (!e->has_filename) {
            arkvim_editor_set_status(e, "No file name");
            return;
        }
        if (arkvim_fileio_save(e, e->filename) == 0) {
            e->dirty = false;
            statusf(e, "\"%s\" written", e->filename);
        } else {
            statusf(e, "Error writing \"%s\"", e->filename);
        }
        return;
    }
    if (strncmp(cmd, "w ", 2) == 0) {
        const char *fn = cmd + 2;
        strncpy(e->filename, fn, ARKVIM_FILENAME_MAX - 1);
        e->filename[ARKVIM_FILENAME_MAX - 1] = '\0';
        e->has_filename = true;
        if (arkvim_fileio_save(e, e->filename) == 0) {
            e->dirty = false;
            statusf(e, "\"%s\" written", e->filename);
        } else {
            statusf(e, "Error writing \"%s\"", e->filename);
        }
        return;
    }
    if (strcmp(cmd, "q") == 0) {
        if (e->dirty) {
            arkvim_editor_set_status(e, "No write since last change (use :q!)");
        } else {
            e->should_exit = true;
        }
        return;
    }
    if (strcmp(cmd, "q!") == 0) {
        e->should_exit = true;
        return;
    }
    if (strcmp(cmd, "wq") == 0 || strcmp(cmd, "x") == 0) {
        if (e->has_filename && arkvim_fileio_save(e, e->filename) == 0) {
            e->dirty = false;
            e->should_exit = true;
        } else if (!e->has_filename) {
            arkvim_editor_set_status(e, "No file name");
        } else {
            statusf(e, "Error writing \"%s\"", e->filename);
        }
        return;
    }
    if (strncmp(cmd, "set ", 4) == 0) {
        const char *opt = cmd + 4;
        if (strcmp(opt, "nu") == 0 || strcmp(opt, "number") == 0) {
            e->show_numbers = true;
            ren_mark_all_dirty();
        } else if (strcmp(opt, "nonu") == 0 || strcmp(opt, "nonumber") == 0) {
            e->show_numbers = false;
            ren_mark_all_dirty();
        } else {
            arkvim_editor_set_status(e, "Unknown option");
        }
        return;
    }
    if (strcmp(cmd, "help") == 0) {
        arkvim_editor_set_status(e, ":w :q :wq :q! :x :set nu|nonu  /pat n  gg G");
        return;
    }
    if (strcmp(cmd, "version") == 0 || strcmp(cmd, "ver") == 0) {
        statusf(e, "%s v%s - %s", ARKVIM_NAME, ARKVIM_VERSION, ARKVIM_TAGLINE);
        return;
    }

    arkvim_editor_set_status(e, "Unknown command");
}

static void process_command(arkvim_editor_t *e, int key)
{
    switch (key) {
    case ARKVIM_KEY_ESC:
    case ARKVIM_KEY_CTRL_C:
        e->mode = ARKVIM_MODE_NORMAL;
        e->cmd_len = 0;
        e->cmd_buf[0] = '\0';
        break;

    case ARKVIM_KEY_ENTER:
    case ARKVIM_KEY_NL:
        e->cmd_buf[e->cmd_len] = '\0';
        exec_ex(e, e->cmd_buf);
        e->mode = ARKVIM_MODE_NORMAL;
        e->cmd_len = 0;
        e->cmd_buf[0] = '\0';
        break;

    case ARKVIM_KEY_BACKSPACE:
    case ARKVIM_KEY_CTRL_H:
        if (e->cmd_len > 1) {
            e->cmd_len--;
            e->cmd_buf[e->cmd_len] = '\0';
        } else {
            /* cancel back to normal */
            e->mode = ARKVIM_MODE_NORMAL;
            e->cmd_len = 0;
            e->cmd_buf[0] = '\0';
        }
        break;

    default:
        if (key >= 0x20 && key <= 0x7E &&
            e->cmd_len < ARKVIM_CMD_BUF_SIZE - 1) {
            e->cmd_buf[e->cmd_len++] = (char)key;
            e->cmd_buf[e->cmd_len] = '\0';
        }
        break;
    }
}

/* ============================================================
 * Main loop
 * ============================================================ */
void arkvim_editor_run(const char *filename)
{
    arkvim_editor_t *e = &s_editor;

    memset(e, 0, sizeof(*e));
    gb_init(&e->gb, e->gb_mem, ARKVIM_GB_MAX_SIZE);
    lt_init(&e->lt);
    undo_init(&e->undo, &e->redo);
    e->mode = ARKVIM_MODE_NORMAL;
    e->show_numbers = (ARKVIM_ENABLE_LINE_NUMBERS != 0);

    if (filename != NULL && filename[0] != '\0') {
        strncpy(e->filename, filename, ARKVIM_FILENAME_MAX - 1);
        e->filename[ARKVIM_FILENAME_MAX - 1] = '\0';
        e->has_filename = true;
        if (arkvim_fileio_load(e, filename) == 0) {
            statusf(e, "%s v%s  \"%s\" %uL", ARKVIM_NAME, ARKVIM_VERSION,
                    filename, lt_line_count(&e->lt, &e->gb));
        } else {
            statusf(e, "%s v%s  \"%s\" [New File]", ARKVIM_NAME, ARKVIM_VERSION,
                    filename);
        }
    } else {
        statusf(e, "%s v%s  [No Name]", ARKVIM_NAME, ARKVIM_VERSION);
    }

    /* enter full-screen */
    arkvim_term_clear_screen();
    arkvim_term_hide_cursor();
    ren_init();
    ren_mark_all_dirty();

    while (!e->should_exit) {
        int key;
        refresh_screen(e);
        key = arkvim_term_read_key(100);
        if (key == ARKVIM_KEY_NONE) {
            continue;
        }
        switch (e->mode) {
        case ARKVIM_MODE_NORMAL:  process_normal(e, key);  break;
        case ARKVIM_MODE_INSERT:  process_insert(e, key);  break;
        case ARKVIM_MODE_COMMAND: process_command(e, key); break;
        default: e->mode = ARKVIM_MODE_NORMAL; break;
        }
    }

    /* restore terminal for the shell */
    arkvim_term_clear_screen();
    arkvim_term_move_cursor(1, 1);
    arkvim_term_reset_attr();
    arkvim_term_show_cursor();
}
