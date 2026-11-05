/* ============================================================
 * arkc_port.h - what arkc_port_arkrtos.c offers to the command layer
 *
 * The compiler itself is reached through include/platform.h and
 * include/flash_fs.h.  These four are the kernel-side contract that only
 * the "cc" command needs, kept in one header so the two .c files cannot
 * drift apart on a signature.
 * ============================================================ */
#ifndef ARKC_PORT_H
#define ARKC_PORT_H

#include <setjmp.h>
#include <stdint.h>

/* Landing pad for ark_abort(): arkc treats it as a no-return function, so
 * the port longjmps back here instead of hanging the shell task.  Pass NULL
 * to disarm (the port then prints and returns, which is worse but alive). */
void        arkc_port_set_abort_target(jmp_buf *jb);

/* Did the run that just finished end through ark_abort()? */
uint8_t     arkc_port_abort_used(void);

/* Message of that fatal error, valid only when the above returns 1. */
const char *arkc_port_abort_message(void);

/* Zero the .ark_buf arena (compiler pool + source/asm/code/object buffers).
 *
 * That section is NOLOAD: it costs no image bytes and no boot time, but it
 * also is not zeroed by the C runtime, so whatever the previous reset or the
 * previous compile left there is still readable.  arkc resets what it owns
 * on entry, yet a deterministic start is cheaper than trusting that; call it
 * once before each compile.
 *
 * Safe to call even when there is no arena (the linker script is generated,
 * so a build without arkc simply has a zero-length section: the weak symbols
 * are then undefined and this returns doing nothing). */
void        arkc_port_zero_arena(void);

#endif /* ARKC_PORT_H */
