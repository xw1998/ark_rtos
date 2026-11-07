/* ============================================================
 * arkvim_terminal.h - Terminal I/O and Key Reading
 *
 * A thin layer over the arkvim_port.h transport. It provides:
 *   - byte send/receive helpers
 *   - ANSI escape sequence emission
 *   - a blocking key reader that decodes escape sequences into
 *     high-level key codes (arrows, Home, End, PageUp/Down, Delete)
 *
 * Key codes >= 0x100 are special keys and never collide with the
 * 0..255 byte range, so callers can treat printable bytes as
 * their ASCII value directly.
 * ============================================================ */
#ifndef ARKVIM_TERMINAL_H
#define ARKVIM_TERMINAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Special key codes (outside the 0..255 byte range) ---- */
enum {
    ARKVIM_KEY_NONE      = 0x1000,  /* no input available               */
    ARKVIM_KEY_ARROW_UP,
    ARKVIM_KEY_ARROW_DOWN,
    ARKVIM_KEY_ARROW_LEFT,
    ARKVIM_KEY_ARROW_RIGHT,
    ARKVIM_KEY_HOME,
    ARKVIM_KEY_END,
    ARKVIM_KEY_PAGE_UP,
    ARKVIM_KEY_PAGE_DOWN,
    ARKVIM_KEY_DELETE
};

/* ASCII control keys used by the editor. Kept as plain byte values
 * so they can be compared directly with raw input. */
#define ARKVIM_KEY_ESC        0x1B
#define ARKVIM_KEY_ENTER      0x0D
#define ARKVIM_KEY_NL         0x0A
#define ARKVIM_KEY_TAB        0x09
#define ARKVIM_KEY_BACKSPACE  0x7F
#define ARKVIM_KEY_CTRL_H     0x08
#define ARKVIM_KEY_CTRL_C     0x03
#define ARKVIM_KEY_CTRL_R     0x12
#define ARKVIM_KEY_CTRL_U     0x15
#define ARKVIM_KEY_CTRL_W     0x17

/* ---- Raw byte I/O (wrap arkvim_port.h) ---- */
void    arkvim_term_putc(char c);
void    arkvim_term_puts(const char *str);
void    arkvim_term_write(const char *data, uint32_t len);
int     arkvim_term_getc(void);            /* non-blocking, -1 if no data  */

/* ---- ANSI emission ---- */
void    arkvim_term_clear_screen(void);
void    arkvim_term_clear_line(void);
void    arkvim_term_clear_to_eol(void);
void    arkvim_term_move_cursor(int row, int col);   /* 1-based */
/* Format the same sequence into `out` without emitting it.
 * Returns the length written; `out` must hold >= 24 bytes.
 * Lets a caller that batches output avoid one transport write
 * per cursor move. */
int     arkvim_term_fmt_move_cursor(int row, int col, char *out);
void    arkvim_term_hide_cursor(void);
void    arkvim_term_show_cursor(void);
void    arkvim_term_reset_attr(void);
void    arkvim_term_inverse(int on);

/* ---- Key reading ---- */
/* Block up to timeout_ms for a complete key. Returns ARKVIM_KEY_NONE
 * on timeout. Decodes CSI/SS3 escape sequences. A lone ESC with no
 * following byte within ARKVIM_ESC_TIMEOUT_MS returns ARKVIM_KEY_ESC. */
int     arkvim_term_read_key(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_TERMINAL_H */
