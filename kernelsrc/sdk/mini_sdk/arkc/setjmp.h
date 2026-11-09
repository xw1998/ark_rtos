/* Ark C Compiler mini SDK - setjmp.h
 * Non-local jumps, with a real implementation for the ARM target.
 *
 * Why this header exists at all, given that arkc ships stdlib/include/setjmp.h
 *
 *   That header declares the interface and nothing else; its own comment says
 *   so ("Bare-metal ARM targets require an assembly implementation ... this
 *   file does not cover the ARM scenario").  stdlib/src/setjmp.c forwards to
 *   the *host* setjmp, which is correct on a PC and meaningless on the board.
 *
 *   The board has no C library to link against: whatever a program calls has
 *   to be one of the sources in its own SRCS list.  So the ARM implementation
 *   lives here, in the same place as the rest of the mini runtime (rt.c), and
 *   programs put /mnt/nor/setjmp.c on their SRCS line to get it.
 *
 *   `<setjmp.h>` resolves to this file because arkc searches the -I dirs
 *   (INCS = /mnt/nor) *before* its own built-in stdlib/include.  The type is
 *   identical to the built-in one, so a program that ends up using either
 *   header still agrees with setjmp.c about the layout.
 *
 * Layout (kept byte-for-byte compatible with arkc's own jmp_buf):
 *   int[20] = 80 bytes.  This implementation uses the first 40:
 *     [0..7]  r4-r11        the callee-saved integer registers
 *     [8]     sp            the stack pointer at the setjmp() call
 *     [9]     lr            where setjmp() was called from (the jump target)
 *   The remaining 40 bytes are reserved, not written.
 *
 * Return values: setjmp() returns 0 on the direct call and `val` (>0) when
 * control arrives through longjmp(); longjmp(env, 0) is required by C99
 * 7.13.2.1p2 to make setjmp() return 1, and it does.
 *
 * Limits, stated rather than glossed over:
 *   - No floating-point state is saved.  AEABI says s16-s31 are callee-saved,
 *     so a C function that survives longjmp() still sees them intact only
 *     because the *intermediate* frames are abandoned, not unwound; a value
 *     live in s0-s15 across the jump is not preserved.  The C standard makes
 *     non-volatile locals after longjmp() indeterminate anyway.
 *   - Only one context: no signal mask, no thread bookkeeping.  The mini
 *     target has neither.
 */
#ifndef ARK_SETJMP_H
#define ARK_SETJMP_H

typedef int jmp_buf[20];

#ifdef __cplusplus
extern "C" {
#endif

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
}
#endif

#endif /* ARK_SETJMP_H */
