/* ============================================================
 * arkvim_modes.h - Editor State and Modal State Machine
 *
 * Defines the editor state struct (arkvim_editor_t) and the entry
 * point arkvim_editor_run(), which takes over the terminal until the
 * user quits, then returns control to the shell.
 *
 * The editor struct is public because the file I/O backend and the
 * undo stack operate on its buffer directly.
 * ============================================================ */
#ifndef ARKVIM_MODES_H
#define ARKVIM_MODES_H

#include <stdint.h>
#include <stdbool.h>

#include "arkvim_config.h"
#include "arkvim_gap_buffer.h"
#include "arkvim_line_table.h"
#include "arkvim_undo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARKVIM_MODE_NORMAL = 0,
    ARKVIM_MODE_INSERT,
    ARKVIM_MODE_COMMAND
} arkvim_mode_t;

/* Forward-declared in arkvim_undo.h and arkvim_fileio.h; the full
 * definition lives here. */
typedef struct arkvim_editor_s arkvim_editor_t;

struct arkvim_editor_s {
    /* text + line index */
    arkvim_gap_buffer_t gb;
    char             gb_mem[ARKVIM_GB_MAX_SIZE];
    arkvim_line_table_t lt;

    /* cursor / view */
    uint32_t cursor;        /* logical offset into the buffer */
    uint16_t scroll_row;    /* first visible line (0-based)    */
    uint16_t scroll_col;    /* first visible column (0-based)  */

    /* mode + file state */
    arkvim_mode_t mode;
    bool       dirty;
    char       filename[ARKVIM_FILENAME_MAX];
    bool       has_filename;

    /* command line + status */
    char       cmd_buf[ARKVIM_CMD_BUF_SIZE];
    int        cmd_len;
    char       status_msg[ARKVIM_STATUS_MSG_SIZE];

    /* view cursor (1-based, for renderer) */
    int        screen_cx;
    int        screen_cy;

    /* options */
    bool       show_numbers;
    bool       should_exit;

    /* normal-mode command parsing */
    int        count;       /* pending count prefix, 0 = none  */
    char       pending_op;  /* pending operator d/y/c, 0 = none */

    /* undo / redo */
    arkvim_action_stack_t undo;
    arkvim_action_stack_t redo;

    /* yank ring (single slot) */
    char       yank_buf[ARKVIM_YANK_MAX];
    uint32_t   yank_len;
    bool       yank_linewise;

    /* search */
    char       last_search[ARKVIM_SEARCH_MAX];
};

/* Take over the terminal and edit `filename` (may be NULL for a
 * scratch buffer). Returns when the user quits. */
void arkvim_editor_run(const char *filename);

/* Status line helper. */
void arkvim_editor_set_status(arkvim_editor_t *e, const char *msg);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_MODES_H */
