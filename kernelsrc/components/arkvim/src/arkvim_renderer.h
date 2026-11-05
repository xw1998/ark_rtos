/* ============================================================
 * arkvim_renderer.h - Screen Double Buffer
 *
 * Maintains a front buffer (what is on screen) and a back buffer
 * (what we want on screen). On refresh, only cells that differ are
 * re-emitted, minimising UART traffic. A per-row dirty flag avoids
 * even comparing unchanged rows.
 *
 * A cell holds a character plus a highlight attribute. Attributes
 * map to ANSI colours for syntax highlight and the inverse-video
 * status bar.
 * ============================================================ */
#ifndef ARKVIM_RENDERER_H
#define ARKVIM_RENDERER_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Highlight attributes. Order matters for the ANSI mapping table. */
enum {
    ARKVIM_ATTR_NORMAL  = 0,
    ARKVIM_ATTR_REVERSE,      /* status bar */
    ARKVIM_ATTR_KEYWORD,
    ARKVIM_ATTR_STRING,
    ARKVIM_ATTR_COMMENT,
    ARKVIM_ATTR_NUMBER,
    ARKVIM_ATTR_OPERATOR
};

typedef struct {
    char    ch;
    uint8_t attr;
} arkvim_cell_t;

void ren_init(void);
void ren_clear(void);

/* Write into the back buffer. */
void ren_set_cell(int row, int col, char ch, uint8_t attr);
void ren_set_text(int row, int col, const char *text, int len, uint8_t attr);
void ren_clear_row(int row);

/* Dirty tracking. */
void ren_mark_row_dirty(int row);
void ren_mark_all_dirty(void);
void ren_clear_dirty(void);

/* Emit only changed/dirty cells, then restore the saved cursor. */
void ren_refresh(void);

/* Cursor the renderer restores after refresh (1-based). */
void ren_set_cursor(int row, int col);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_RENDERER_H */
