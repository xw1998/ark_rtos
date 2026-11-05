/* ============================================================
 * arkvim_port.h - Shell-Agnostic Porting Interface
 *
 * ArkVim does NOT depend on any specific shell. Instead it calls
 * the host through this small function-pointer table. Any shell,
 * RTOS, or bare-metal program that fills in six functions can host
 * the editor.
 *
 * The host calls arkvim_port_init() once with its own arkvim_port_t
 * (usually a static const), then calls arkvim_editor_run() /
 * arkvim_fileio_cat() / arkvim_fileio_ls() from its own command handler.
 *
 * Reference ports shipped in ../ports/:
 *   arkvim_port_arkshell.c  - wraps Ark Shell platform.h
 *   arkvim_port_minimal.c   - standalone termios/Win32 (no shell)
 *   arkvim_port_template.c  - annotated skeleton for new hosts
 * ============================================================ */
#ifndef ARKVIM_PORT_H
#define ARKVIM_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * arkvim_port_t - Host-provided transport
 *
 * Every field MUST be non-NULL when passed to arkvim_port_init().
 *
 *   send_byte   - emit one byte to the terminal
 *   send_string - emit a NUL-terminated string (convenience; may
 *                 loop over send_byte if the host has no bulk API)
 *   send_buf    - emit len bytes from buf (may equal send_string
 *                 in spirit; both are provided so a DMA/DMA-style
 *                 host can use the bulk path)
 *   recv_byte   - non-blocking read; return the byte, or -1 when
 *                 no input is available THIS call (the editor polls)
 *   tick_ms     - monotonic millisecond counter for ESC-sequence
 *                 timeouts (wraparound is fine; only deltas are used)
 *   cleanup     - optional; called once when the host tears down.
 *                 May be NULL.
 * ============================================================ */
typedef struct arkvim_port_t {
    void  (*send_byte)(uint8_t b);
    void  (*send_string)(const char *s);
    void  (*send_buf)(const uint8_t *buf, uint16_t len);
    int   (*recv_byte)(void);
    uint32_t (*tick_ms)(void);
    void  (*cleanup)(void);          /* may be NULL */
} arkvim_port_t;

/* Install the port. Must be called before any arkvim_*() function.
 * The pointer is retained (not copied), so the caller must keep
 * the arkvim_port_t object alive for the program's lifetime
 * (a static const is the usual choice). Passing NULL unregisters
 * the port and makes all I/O no-ops. */
void arkvim_port_init(const arkvim_port_t *port);

/* Current port (NULL until arkvim_port_init() is called). */
const arkvim_port_t *arkvim_port_get(void);

/* ---- Convenience wrappers (used internally by the editor; also
 *      available to host code that wants the same transport). ---- */
void arkvim_port_send_byte(uint8_t b);
void arkvim_port_send_string(const char *s);
void arkvim_port_send_buf(const uint8_t *buf, uint16_t len);
int  arkvim_port_recv_byte(void);
uint32_t arkvim_port_tick_ms(void);

#ifdef __cplusplus
}
#endif
#endif /* ARKVIM_PORT_H */
