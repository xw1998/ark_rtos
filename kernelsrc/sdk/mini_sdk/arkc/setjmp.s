@ Ark C Compiler mini SDK - setjmp.s
@ setjmp / longjmp for ARM Thumb-2, as real functions.
@
@ Why assembly, and why a separate .s rather than C with inline asm
@ ---------------------------------------------------------------
@ setjmp must capture the state of *its caller*: the stack pointer and return
@ address that `main` (or whoever calls it) has when it reaches the call.  A C
@ wrapper gets neither -- inside the wrapper, `sp` is the wrapper's own frame
@ and `lr` points back into the wrapper.  Saving those is subtly wrong in a way
@ that looks right: the wrapper returns normally, its frame is reused by the
@ next call, and the longjmp lands on memory that no longer holds the saved
@ registers.  Measured on this very program (`deep(5)` overwrote it): the jump
@ "worked" and control came back through the normal return path with the wrong
@ value.  So setjmp and longjmp have to be these two instructions sequences,
@ entered directly by their callers.
@
@ arkcc has no operand allocator for extended `asm("..." : : )`, so the code is
@ written here and assembled into an object of its own.  arkc accepts `.s` /
@ `.asm` inputs directly (src/compiler.c: is_asm_input) and its internal Thumb
@ assembler covers every mnemonic used below -- verified by disassembling the
@ linked image, not by reading the assembler.
@
@ Layout of jmp_buf (int[20], 80 bytes; matches arkc's stdlib/include/setjmp.h)
@   [0..7]  r4-r11   callee-saved integer registers
@   [8]     sp       stack pointer at the setjmp() call
@   [9]     lr       where setjmp() was called from == the jump target
@   [10..19]         reserved, never written
@
@   The buffer is 4-byte aligned (int array) and every access below is a word
@   store at a multiple of 4, so no unaligned accesses.
@
@ longjmp(): C99 7.13.2.1p2 says longjmp(env, 0) makes setjmp() return 1.  The
@ check is a forward branch over `movs r1, #1` -- deliberately not an IT block:
@ MOVS inside IT is UNPREDICTABLE on ARMv7-M, and the value is only used as a
@ plain 1/val select here.
@
@ Not saved: the AAPCS callee-saved VFP registers s16-s31.  Frames that a jump
@ abandons are never restored, and C says non-volatile locals are indeterminate
@ after longjmp(), so nothing observable is lost.  Statement of scope, not a
@ promise.
@
@ This file is the single source: the same bytes are mirrored in the compiler
@ repository at stdlib/src/arm/setjmp.s (that tree's ARM execution gate runs it
@ under unicorn, which is how the encoding was first proven).  Change one,
@ change the other -- they must stay identical.

    .syntax unified
    .thumb
    .text

    .global setjmp
    .thumb_func
setjmp:
    str     r4,  [r0, #0]           @ r4-r7 have 16-bit STR forms
    str     r5,  [r0, #4]
    str     r6,  [r0, #8]
    str     r7,  [r0, #12]
    str.w   r8,  [r0, #16]          @ r8-r11 need the 32-bit form
    str.w   r9,  [r0, #20]
    str.w   r10, [r0, #24]
    str.w   r11, [r0, #28]
    mov     r1, sp
    str     r1,  [r0, #32]
    mov     r1, lr
    str     r1,  [r0, #36]
    movs    r0, #0                  @ setjmp() returns 0 on the direct call
    bx      lr

    .global longjmp
    .thumb_func
longjmp:
    cmp     r1, #0
    bne     .L_lj_nonzero
    movs    r1, #1                  @ longjmp(env, 0) must read back as 1
.L_lj_nonzero:
    ldr     r2,  [r0, #32]          @ r2 = sp  (r0 is still needed for the rest)
    ldr     r3,  [r0, #36]          @ r3 = lr  = the jump target
    ldr     r4,  [r0, #0]
    ldr     r5,  [r0, #4]
    ldr     r6,  [r0, #8]
    ldr     r7,  [r0, #12]
    ldr.w   r8,  [r0, #16]
    ldr.w   r9,  [r0, #20]
    ldr.w   r10, [r0, #24]
    ldr.w   r11, [r0, #28]
    mov     sp, r2                  @ unwind: the abandoned frames are simply
    mov     r0, r1                  @ dropped, which is what makes this a jump
    bx      r3                      @ and not a return
