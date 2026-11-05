/* ============================================================
 * arkvim_renderer.c - Screen Double Buffer Implementation
 * ============================================================ */
#include "arkvim_renderer.h"
#include "arkvim_terminal.h"
#include "arkvim_config.h"

#include <string.h>

static arkvim_cell_t s_front[ARKVIM_SCREEN_ROWS][ARKVIM_SCREEN_COLS];
static arkvim_cell_t s_back[ARKVIM_SCREEN_ROWS][ARKVIM_SCREEN_COLS];
static bool       s_dirty[ARKVIM_SCREEN_ROWS];
static int        s_cur_row = 1;
static int        s_cur_col = 1;

/* ANSI SGR string for each attribute. */
static const char *attr_sgr(uint8_t attr)
{
    switch (attr) {
    case ARKVIM_ATTR_REVERSE:  return "\x1b[7m";
    case ARKVIM_ATTR_KEYWORD:  return "\x1b[36m";   /* cyan   */
    case ARKVIM_ATTR_STRING:   return "\x1b[33m";   /* yellow */
    case ARKVIM_ATTR_COMMENT:  return "\x1b[32m";   /* green  */
    case ARKVIM_ATTR_NUMBER:   return "\x1b[35m";   /* magenta*/
    case ARKVIM_ATTR_OPERATOR: return "\x1b[1;34m"; /* bold blue */
    default:                return "\x1b[0m";
    }
}

void ren_init(void)
{
    memset(s_front, 0, sizeof(s_front));
    ren_clear();
    memset(s_front, 0, sizeof(s_front));
    ren_mark_all_dirty();
}

void ren_clear(void)
{
    int r, c;
    for (r = 0; r < ARKVIM_SCREEN_ROWS; r++) {
        for (c = 0; c < ARKVIM_SCREEN_COLS; c++) {
            s_back[r][c].ch   = ' ';
            s_back[r][c].attr = ARKVIM_ATTR_NORMAL;
        }
    }
}

void ren_set_cell(int row, int col, char ch, uint8_t attr)
{
    if (row < 0 || row >= ARKVIM_SCREEN_ROWS || col < 0 || col >= ARKVIM_SCREEN_COLS) {
        return;
    }
    s_back[row][col].ch   = ch;
    s_back[row][col].attr = attr;
}

void ren_set_text(int row, int col, const char *text, int len, uint8_t attr)
{
    int i;
    if (text == NULL || row < 0 || row >= ARKVIM_SCREEN_ROWS) {
        return;
    }
    for (i = 0; i < len; i++) {
        int c = col + i;
        if (c < 0 || c >= ARKVIM_SCREEN_COLS) {
            break;
        }
        s_back[row][c].ch   = text[i];
        s_back[row][c].attr = attr;
    }
}

void ren_clear_row(int row)
{
    int c;
    if (row < 0 || row >= ARKVIM_SCREEN_ROWS) {
        return;
    }
    for (c = 0; c < ARKVIM_SCREEN_COLS; c++) {
        s_back[row][c].ch   = ' ';
        s_back[row][c].attr = ARKVIM_ATTR_NORMAL;
    }
}

void ren_mark_row_dirty(int row)
{
    if (row >= 0 && row < ARKVIM_SCREEN_ROWS) {
        s_dirty[row] = true;
    }
}

void ren_mark_all_dirty(void)
{
    int r;
    for (r = 0; r < ARKVIM_SCREEN_ROWS; r++) {
        s_dirty[r] = true;
    }
}

void ren_clear_dirty(void)
{
    memset(s_dirty, 0, sizeof(s_dirty));
}

void ren_set_cursor(int row, int col)
{
    s_cur_row = row;
    s_cur_col = col;
}

void ren_refresh(void)
{
    int r, c;

    arkvim_term_hide_cursor();

    for (r = 0; r < ARKVIM_SCREEN_ROWS; r++) {
        int out_col;   /* next column to write from */
        if (!s_dirty[r]) {
            continue;
        }

        c = 0;
        out_col = 0;
        while (c < ARKVIM_SCREEN_COLS) {
            uint8_t attr = s_back[r][c].attr;
            int run_end = c;

            /* Extend the run while the attribute is constant. */
            while (run_end < ARKVIM_SCREEN_COLS && s_back[r][run_end].attr == attr) {
                run_end++;
            }
            /* Now [c, run_end) shares `attr`. Emit the changed
             * prefix of this run, then skip the unchanged tail. */
            out_col = c;
            while (out_col < run_end) {
                int seg = out_col;
                int seg_end = seg;

                if (s_front[r][seg].ch   == s_back[r][seg].ch &&
                    s_front[r][seg].attr == s_back[r][seg].attr) {
                    /* Unchanged cell: advance past any unchanged run. */
                    while (seg_end < run_end &&
                           s_front[r][seg_end].ch   == s_back[r][seg_end].ch &&
                           s_front[r][seg_end].attr == s_back[r][seg_end].attr) {
                        seg_end++;
                    }
                } else {
                    /* Changed cell: gather the maximal changed run. */
                    while (seg_end < run_end &&
                           !(s_front[r][seg_end].ch   == s_back[r][seg_end].ch &&
                             s_front[r][seg_end].attr == s_back[r][seg_end].attr)) {
                        seg_end++;
                    }
                    arkvim_term_move_cursor(r + 1, seg + 1);
                    arkvim_term_puts(attr_sgr(attr));
                    {
                        int k;
                        for (k = seg; k < seg_end; k++) {
                            arkvim_term_putc(s_back[r][k].ch);
                            s_front[r][k] = s_back[r][k];
                        }
                    }
                }
                out_col = seg_end;
            }
            c = run_end;
        }
        s_dirty[r] = false;
    }

    arkvim_term_move_cursor(s_cur_row, s_cur_col);
    arkvim_term_reset_attr();
    arkvim_term_show_cursor();
}
