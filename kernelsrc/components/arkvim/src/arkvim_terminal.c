/* ============================================================
 * arkvim_terminal.c - Terminal I/O and Key Reading
 *
 * All transport access goes through arkvim_port.h, so the editor is
 * independent of any particular shell. The escape-sequence decoder
 * here is editor-specific (it must distinguish a bare ESC from the
 * start of a CSI/SS3 sequence) and therefore separate from a
 * shell's own line-editor decoder.
 * ============================================================ */
#include "arkvim_terminal.h"
#include "arkvim_config.h"
#include "arkvim_port.h"

#include <string.h>

/* ---- Internal helpers (defined below) ---- */
static int int_to_dec_unsigned(unsigned int v, char *out);
static int int_to_dec(int v, char *out);

/* ============================================================
 * Raw byte I/O
 * ============================================================ */
void arkvim_term_putc(char c)
{
    arkvim_port_send_byte((uint8_t)c);
}

void arkvim_term_puts(const char *str)
{
    if (str == NULL) {
        return;
    }
    arkvim_port_send_string(str);
}

void arkvim_term_write(const char *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    arkvim_port_send_buf((const uint8_t *)data, (uint16_t)len);
}

int arkvim_term_getc(void)
{
    return arkvim_port_recv_byte();
}

/* ============================================================
 * ANSI escape emission
 * ============================================================ */
void arkvim_term_clear_screen(void)
{
    arkvim_port_send_string("\x1b[2J\x1b[H");
}

void arkvim_term_clear_line(void)
{
    arkvim_port_send_string("\x1b[2K");
}

void arkvim_term_clear_to_eol(void)
{
    arkvim_port_send_string("\x1b[0K");
}

void arkvim_term_move_cursor(int row, int col)
{
    /* ESC [ row ; col H, 1-based. Cap to avoid huge buffers. */
    char buf[24];
    int r = row < 1 ? 1 : row;
    int c = col < 1 ? 1 : col;
    int n = 0;
    buf[n++] = '\x1b';
    buf[n++] = '[';
    n += int_to_dec(r, &buf[n]);
    buf[n++] = ';';
    n += int_to_dec(c, &buf[n]);
    buf[n++] = 'H';
    arkvim_port_send_buf((const uint8_t *)buf, (uint16_t)n);
}

void arkvim_term_hide_cursor(void) { arkvim_port_send_string("\x1b[?25l"); }
void arkvim_term_show_cursor(void) { arkvim_port_send_string("\x1b[?25h"); }
void arkvim_term_reset_attr(void)  { arkvim_port_send_string("\x1b[0m");  }

void arkvim_term_inverse(int on)
{
    arkvim_port_send_string(on ? "\x1b[7m" : "\x1b[27m");
}

/* ============================================================
 * Small integer -> decimal helper (avoids snprintf in hot path)
 * Returns number of digits written. Buffer must hold >= 11 bytes.
 * ============================================================ */
static int int_to_dec_unsigned(unsigned int v, char *out)
{
    char tmp[16];
    int  n = 0;
    int  i;

    if (v == 0) {
        out[0] = '0';
        return 1;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (i = 0; i < n; i++) {
        out[i] = tmp[n - 1 - i];
    }
    return n;
}

static int int_to_dec(int v, char *out)
{
    if (v < 0) {
        out[0] = '-';
        return 1 + int_to_dec_unsigned((unsigned int)(-v), &out[1]);
    }
    return int_to_dec_unsigned((unsigned int)v, out);
}

/* ============================================================
 * Key reading
 *
 * Strategy: read the first byte (blocking up to timeout). If it is
 * ESC, peek for a follow-up byte with a short timeout. A CSI
 * sequence has the form  ESC [ <params> <final> ; an SS3 sequence
 * has the form  ESC O <final>. We collect up to a final letter or
 * '~' and map known sequences to special keys.
 * ============================================================ */
static int peek_byte(uint32_t timeout_ms)
{
    uint32_t start = arkvim_port_tick_ms();
    for (;;) {
        int b = arkvim_port_recv_byte();
        if (b >= 0) {
            return b;
        }
        if (arkvim_port_tick_ms() - start >= timeout_ms) {
            return -1;
        }
    }
}

int arkvim_term_read_key(uint32_t timeout_ms)
{
    int c = peek_byte(timeout_ms);
    if (c < 0) {
        return ARKVIM_KEY_NONE;
    }

    if (c != ARKVIM_KEY_ESC) {
        return c;   /* printable or control byte */
    }

    /* ESC: wait briefly for a follow-up byte. */
    c = peek_byte(ARKVIM_ESC_TIMEOUT_MS);
    if (c < 0) {
        return ARKVIM_KEY_ESC;   /* lone Escape */
    }

    if (c == '[') {
        /* CSI: collect until a final byte (letter or '~'). */
        char params[8];
        int  plen = 0;
        int  final = -1;
        for (;;) {
            int b = peek_byte(ARKVIM_ESC_TIMEOUT_MS);
            if (b < 0) {
                return ARKVIM_KEY_ESC;
            }
            if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~') {
                final = b;
                break;
            }
            if (plen < (int)sizeof(params) - 1) {
                params[plen++] = (char)b;
            }
        }
        params[plen] = '\0';

        switch (final) {
        case 'A': return ARKVIM_KEY_ARROW_UP;
        case 'B': return ARKVIM_KEY_ARROW_DOWN;
        case 'C': return ARKVIM_KEY_ARROW_RIGHT;
        case 'D': return ARKVIM_KEY_ARROW_LEFT;
        case 'H': return ARKVIM_KEY_HOME;
        case 'F': return ARKVIM_KEY_END;
        case '~':
            if (plen == 1) {
                switch (params[0]) {
                case '1': return ARKVIM_KEY_HOME;
                case '4': return ARKVIM_KEY_END;
                case '5': return ARKVIM_KEY_PAGE_UP;
                case '6': return ARKVIM_KEY_PAGE_DOWN;
                case '3': return ARKVIM_KEY_DELETE;
                default: break;
                }
            }
            break;
        default: break;
        }
        return ARKVIM_KEY_ESC;
    }

    if (c == 'O') {
        /* SS3: ESC O <final> */
        int b = peek_byte(ARKVIM_ESC_TIMEOUT_MS);
        if (b < 0) {
            return ARKVIM_KEY_ESC;
        }
        switch (b) {
        case 'H': return ARKVIM_KEY_HOME;
        case 'F': return ARKVIM_KEY_END;
        case 'A': return ARKVIM_KEY_ARROW_UP;
        case 'B': return ARKVIM_KEY_ARROW_DOWN;
        case 'C': return ARKVIM_KEY_ARROW_RIGHT;
        case 'D': return ARKVIM_KEY_ARROW_LEFT;
        default: break;
        }
        return ARKVIM_KEY_ESC;
    }

    /* ESC followed by an unexpected byte: treat as ESC. */
    return ARKVIM_KEY_ESC;
}
