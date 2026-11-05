/* ============================================================
 * arkvim_config.h - ArkVim Global Configuration
 *
 * All tunable parameters for the editor are centralized here.
 * Adjust when retargeting to a different MCU or terminal size.
 *
 * ArkVim is a shell-agnostic modal text editor component. It talks
 * to the host through arkvim_port.h (six function pointers). Any shell
 * or bare-metal program can host it.
 * ============================================================ */
#ifndef ARKVIM_CONFIG_H
#define ARKVIM_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Identity ---- */
#define ARKVIM_NAME             "ArkVim"
#define ARKVIM_VERSION          "1.0"
#define ARKVIM_TAGLINE          "modal editor for embedded shells"

/* ---- Terminal geometry ---- */
/* Visible screen size. The editor keeps two screen rows for the
 * status bar and command line; the rest is the text area. */
#ifndef ARKVIM_SCREEN_ROWS
#define ARKVIM_SCREEN_ROWS           24
#endif
#ifndef ARKVIM_SCREEN_COLS
#define ARKVIM_SCREEN_COLS           80
#endif

/* ---- Text buffer ---- */
/* Physical backing store for the gap buffer. On a constrained MCU
 * reduce this to fit RAM. 16KB comfortably holds config files and
 * small source files. */
#ifndef ARKVIM_GB_MAX_SIZE
#define ARKVIM_GB_MAX_SIZE           (16 * 1024)
#endif

/* ---- Line table ---- */
#ifndef ARKVIM_LT_MAX_LINES
#define ARKVIM_LT_MAX_LINES          1024
#endif

/* ---- Command line ---- */
#ifndef ARKVIM_CMD_BUF_SIZE
#define ARKVIM_CMD_BUF_SIZE          128
#endif
#ifndef ARKVIM_STATUS_MSG_SIZE
#define ARKVIM_STATUS_MSG_SIZE       96
#endif
#ifndef ARKVIM_FILENAME_MAX
#define ARKVIM_FILENAME_MAX          64
#endif

/* ---- Undo / Redo ---- */
#ifndef ARKVIM_UNDO_MAX_DEPTH
#define ARKVIM_UNDO_MAX_DEPTH        64
#endif
/* Per-action captured text. Operations longer than this only keep
 * the head, which is enough to undo the structural change. */
#ifndef ARKVIM_UNDO_TEXT_MAX
#define ARKVIM_UNDO_TEXT_MAX         64
#endif

/* ---- Search ---- */
#ifndef ARKVIM_SEARCH_MAX
#define ARKVIM_SEARCH_MAX            64
#endif

/* ---- Kill ring (yank buffer) ---- */
#ifndef ARKVIM_YANK_MAX
#define ARKVIM_YANK_MAX              512
#endif

/* ---- ESC sequence timing ---- */
/* Milliseconds to wait for the next byte of an ESC sequence before
 * treating a lone ESC as the Escape key. Matches the shell. */
#ifndef ARKVIM_ESC_TIMEOUT_MS
#define ARKVIM_ESC_TIMEOUT_MS        50
#endif

/* ---- Feature switches ---- */
#ifndef ARKVIM_ENABLE_SYNTAX
#define ARKVIM_ENABLE_SYNTAX         1   /* C syntax highlight */
#endif
#ifndef ARKVIM_ENABLE_UNDO
#define ARKVIM_ENABLE_UNDO           1   /* Undo/redo stacks */
#endif
#ifndef ARKVIM_ENABLE_LINE_NUMBERS
#define ARKVIM_ENABLE_LINE_NUMBERS   0   /* :set nu default */
#endif

/* ---- Screen layout helpers (derived, do not edit) ---- */
#define ARKVIM_EDIT_ROWS             (ARKVIM_SCREEN_ROWS - 2)
#define ARKVIM_STATUS_ROW            (ARKVIM_SCREEN_ROWS - 2)
#define ARKVIM_CMDLINE_ROW           (ARKVIM_SCREEN_ROWS - 1)

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_CONFIG_H */
