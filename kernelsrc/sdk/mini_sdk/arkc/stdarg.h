/* Ark C Compiler stdlib - stdarg.h
 * Varargs support.  Two walking conventions exist because the backends build
 * two different argument images, and they are not interchangeable:
 *
 *   __ARK_VA_WORD_SLOTS   (ARM Thumb-2, RV32I)
 *      One 4-byte word per AAPCS argument word, contiguous: word 0..3 are
 *      r0..r3, word 4+k is word k of the caller's stack argument block.  That
 *      is the caller's real layout, so a 64-bit argument is just two adjacent
 *      words and `va_arg(ap, long long)` is a plain 8-byte read.  The driver
 *      defines this macro for those two targets (src/compiler.c).
 *
 *   default               (C and x86_64 backends)
 *      One 8-byte slot per argument, ordinal-indexed: the backend stores all
 *      eight arguments at frame+8000..+64 and _ark_va_ptr walks them by
 *      sizeof(long long).
 *
 * va_arg is expressed as a reinterpret-cast of the *slot address* rather than
 * a cast of the slot value:  (*(type *)(...)).  Every slot holds the value
 * verbatim, so reading it as `type *` works for integers, pointers AND
 * double/float bit patterns without a per-type helper -- a plain
 * `(type)(*ap++)` cast would instead numerically convert the raw bit pattern,
 * which is why `va_arg(ap, double)` used to return ~4.6e18.
 * Both conventions require a little-endian target (sub-word reads take the low
 * bytes); ARM Thumb-2, RV32I and every host these backends run on qualify.
 */
#ifndef ARK_STDARG_H
#define ARK_STDARG_H

#if defined(__ARK_VA_WORD_SLOTS)

/* ---- 4-byte word slots: ARM Thumb-2 and RV32I -------------------------- */

typedef unsigned int *va_list;

/* The argument image is not a variable: on RV32I it is the 32 bytes at the
 * bottom of the callee's frame, on ARM Thumb-2 it is the four pushed register
 * words followed by the caller's own stack argument block.  Either way both
 * backends answer a reference to _ark_va_ptr with the address of the first
 * variadic word, one 4-byte word per AAPCS argument word. */
extern unsigned int *_ark_va_ptr;

#define va_start(ap, last) ((void)((ap) = _ark_va_ptr))
#define va_end(ap)         ((void)0)
#define va_copy(d, s)      ((d) = (s))

/* Words a value of `type` occupies in the argument image. */
#define ark_va__words(type) ((unsigned)((sizeof(type) + 3) / 4))

#if defined(__ARK_VA_PACKED_WORDS)

/* RV32I: this backend numbers argument words 0,1,2,... with no alignment
 * holes, so the walk is a plain advance. */
#define ark_va__take(ap, type) \
    (((ap) += ark_va__words(type)) - ark_va__words(type))

#else

/* ARM Thumb-2: AAPCS32 puts a 64-bit argument on an even register or an
 * 8-byte-aligned stack slot, so the caller leaves one word empty where that
 * alignment had to be fixed up.  Round the pointer up to 8 first; the region
 * base is 8-byte aligned, so "even word index" and "8-byte aligned address"
 * are the same thing. */
#define ark_va__take(ap, type)                                             \
    (((sizeof(type) == 8)                                                  \
        ? ((ap) = (unsigned int *)(((unsigned int)(ap) + 7u) & ~7u))       \
        : (ap)),                                                           \
     ((ap) += ark_va__words(type)) - ark_va__words(type))

#endif

#define va_arg(ap, type)   (*(type *)ark_va__take(ap, type))

/* double is read from memory, so no long-long bit-pattern reinterpretation is
 * needed; the helper stays for source compatibility with the other mode. */
#define ark_va_arg_d(ap)   va_arg(ap, double)

#else

/* ---- 8-byte slots: C and x86_64 backends ------------------------------- */

typedef long long *va_list;

/* Forward declaration so sema accepts va_start; the C backend provides the
 * actual definition (local to each variadic function). */
extern long long *_ark_va_ptr;

#define va_start(ap, last) ((void)((ap) = _ark_va_ptr))
#define va_arg(ap, type)   (*(type *)(ap++))
#define va_end(ap)         ((void)0)
#define va_copy(d, s)      ((d) = (s))

/* Helper for double varargs: reads the long long bit pattern and reinterprets
 * it as double via _ark_ll2d (declared in the generated preamble). */
#define ark_va_arg_d(ap)   (_ark_ll2d(*ap++))

#endif

#endif
