/* Ark C Compiler - ARM (Thumb-2) backend
 * Translates IR into textual Thumb-2 assembly. Moved here from the former src/codegen.c.
 *
 * Calling convention (used together with this compiler):
 *   - Arguments 0..3 passed in r0..r3; extra arguments go on the stack (consistent
 *     with the runtime convention, indicated by the val field of IR_CALL).
 *   - r6 = local variable base (fp, pointing to the low address of the local var area).
 *   - Prologue:  PUSH {r5,r6,r7,lr}; SUB sp,sp,#N; MOV r6, sp
 *   - Epilogue:  POP {r0}; MOV sp,r6; ADD sp,sp,#N; POP {r5,r6,r7,pc}
 *   - Variadic functions push r0..r3 *before* the saved registers, so the four
 *     register argument words land directly below the caller's stack argument
 *     block and the va region becomes one contiguous image; the epilogue then
 *     has to add those 16 bytes back as well.
 */
#include "backend.h"
#include "codegen.h"
#include "memory.h"
#include "platform.h"
#include "common.h"
#include "config.h"
#include "diag.h"
#include "scatter.h"
#include "agginit.h"
#include "arkcc_config.h"
/* --rt-lazy 的文本压实用 memmove/memcmp（标准 C 库，宿主与板上都有实现）。 */
#include <string.h>
/* On host, <stdlib.h> provides getenv() for the ARKCC_DUMP_SYMS debug flag.
 * On STM32, getenv() is not available (semihosting); g_arm_dump_syms is used
 * instead, and mem_realloc (our own allocator) replaces realloc. */
#if ARKCC_PROFILE != ARKCC_PROFILE_STM32_MIN
#  include <stdlib.h>
#endif

/* --- Internal helpers: emit common instruction fragments --- */
static void emit_line(AsmBuffer *b, const char *s) {
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, s);
    asmbuf_putc(b, '\n');
}
static void emit_add_sp(AsmBuffer *b, int amount) {
    asmbuf_puts(b, "    ADD sp, sp, #");
    asmbuf_int(b, amount);
    asmbuf_putc(b, '\n');
}

static void emit_mov_imm(AsmBuffer *b, int rd, int imm) {
    if (imm >= 0 && imm <= 255) {
        asmbuf_puts(b, "    MOV r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, imm);
        asmbuf_putc(b, '\n');
    } else {
        /* Always emit MOVW + MOVT pair. MOVW only sets bits[15:0];
         * without MOVT, bits[31:16] retain stale values from previous
         * operations, causing incorrect constants (e.g., 0x40022000
         * instead of 0x00002000 for 1u<<13). */
        asmbuf_puts(b, "    MOVW r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, imm & 0xFFFF);
        asmbuf_putc(b, '\n');
        asmbuf_puts(b, "    MOVT r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, (imm >> 16) & 0xFFFF);
        asmbuf_putc(b, '\n');
    }
}

static void emit_push_r0(AsmBuffer *b) { emit_line(b, "PUSH {r0}"); }
static void emit_pop_r0(AsmBuffer *b)  { emit_line(b, "POP {r0}");  }
static void emit_pop_r1(AsmBuffer *b)  { emit_line(b, "POP {r1}");  }

/* sret support: check if a function name is in the sret set */
static int sret_lookup(char **names, int *sizes, int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        if (ark_streq(names[i], name)) return 1;
    }
    return 0;
}

/* sret support: get struct return size for a function name */
static int sret_get_size(char **names, int *sizes, int count, const char *name) {
    int i;
    for (i = 0; i < count; i++) {
        if (ark_streq(names[i], name)) return sizes[i];
    }
    return 0;
}

#if ARKCC_FEATURE_FPU_HW
/* Hardware VFP floating-point path helpers.
 * Convention: floating-point values are stored on the stack as integer bit patterns (single=1 slot, double=2 slots low/high).
 * Pop operands to r0/r1 (single) or r0/r1+r2/r3 (double), move them into VFP registers via VMOV,
 * perform the VFP operation, then move the result back to r0 (single) or r0/r1 (double) and push. */

/* Single-precision binary op: pop b(s1), a(s0), emit VOP.F32 s0,s0,s1, push result s0. */
static void emit_vfp_f32_binop(AsmBuffer *b, const char *vop) {
    emit_pop_r1(b); emit_pop_r0(b);
    emit_line(b, "VMOV s0, r0");
    emit_line(b, "VMOV s1, r1");
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, vop);
    asmbuf_puts(b, ".F32 s0, s0, s1\n");
    emit_line(b, "VMOV r0, s0");
    emit_push_r0(b);
}

/* Double-precision binary op: pop b(d1), a(d0), emit VOP.F64 d0,d0,d1, push result d0. */
static void emit_vfp_f64_binop(AsmBuffer *b, const char *vop) {
    emit_line(b, "POP {r3}");  /* b_high */
    emit_line(b, "POP {r2}");  /* b_low  */
    emit_line(b, "POP {r1}");  /* a_high */
    emit_line(b, "POP {r0}");  /* a_low  */
    emit_line(b, "VMOV s0, r0");
    emit_line(b, "VMOV s1, r1");
    emit_line(b, "VMOV s2, r2");
    emit_line(b, "VMOV s3, r3");
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, vop);
    asmbuf_puts(b, ".F64 d0, d0, d1\n");
    emit_line(b, "VMOV r0, s0");
    emit_line(b, "VMOV r1, s1");
    emit_push_r0(b);
    asmbuf_puts(b, "    PUSH {r1}\n");
}
#endif /* ARKCC_FEATURE_FPU_HW */

static void emit_label(AsmBuffer *b, const char *fn, int n) {
    asmbuf_puts(b, ".L_");
    asmbuf_puts(b, fn);
    asmbuf_putc(b, '_');
    asmbuf_int(b, n);
    asmbuf_puts(b, ":\n");
}

static void emit_b_label(AsmBuffer *b, const char *op, const char *fn, int n) {
    asmbuf_puts(b, "    ");
    asmbuf_puts(b, op);
    asmbuf_puts(b, " .L_");
    asmbuf_puts(b, fn);
    asmbuf_putc(b, '_');
    asmbuf_int(b, n);
    asmbuf_putc(b, '\n');
}

/* Emit binary operation. The high bit 0x100 of aux marks unsigned comparison/shift-right:
 *   - TOK_LT/GT/LE/GE: when unsigned, use LO/HI/LS/HS conditions instead of LT/GT/LE/GE
 *   - TOK_SHR: when unsigned, use LSR (logical shift right) instead of ASR (arithmetic shift right)
 * ARM Thumb-2 condition codes: LO=CC(3), HI(8), LS(9), HS=CS(2) */
static void emit_binary(AsmBuffer *b, int op) {
    int is_unsigned = (op & 0x100) != 0;
    int base_op = op & ~0x100;
    switch (base_op) {
        case (int)TOK_PLUS:  emit_line(b, "ADD r0, r0, r1"); break;
        case (int)TOK_MINUS: emit_line(b, "SUB r0, r0, r1"); break;
        case (int)TOK_MUL:   emit_line(b, "MUL r0, r1");     break;
        case (int)TOK_DIV:   emit_line(b, "SDIV r0, r0, r1"); break;
        case (int)TOK_MOD:
            emit_line(b, "SDIV r2, r0, r1");
            emit_line(b, "MUL r2, r1");
            emit_line(b, "SUB r0, r0, r2");
            break;
        case (int)TOK_BAND:  emit_line(b, "AND r0, r1");     break;
        case (int)TOK_BOR:   emit_line(b, "ORR r0, r1");     break;
        case (int)TOK_BXOR:  emit_line(b, "EOR r0, r1");     break;
        case (int)TOK_SHL:   emit_line(b, "LSL r0, r1");     break;
        case (int)TOK_SHR:
            /* Unsigned uses LSR (logical shift right), signed uses ASR (arithmetic shift right) */
            emit_line(b, is_unsigned ? "LSR r0, r1" : "ASR r0, r1");
            break;
        case (int)TOK_EQ:
            emit_line(b, "CMP r0, r1");
            emit_line(b, "ITE EQ");
            emit_line(b, "MOVEQ r0, #1");
            emit_line(b, "MOVNE r0, #0");
            break;
        case (int)TOK_NE:
            emit_line(b, "CMP r0, r1");
            emit_line(b, "ITE NE");
            emit_line(b, "MOVNE r0, #1");
            emit_line(b, "MOVEQ r0, #0");
            break;
        case (int)TOK_LT:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE LO"); emit_line(b, "MOVLO r0, #1"); emit_line(b, "MOVHS r0, #0"); }
            else             { emit_line(b, "ITE LT"); emit_line(b, "MOVLT r0, #1"); emit_line(b, "MOVGE r0, #0"); }
            break;
        case (int)TOK_GT:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE HI"); emit_line(b, "MOVHI r0, #1"); emit_line(b, "MOVLS r0, #0"); }
            else             { emit_line(b, "ITE GT"); emit_line(b, "MOVGT r0, #1"); emit_line(b, "MOVLE r0, #0"); }
            break;
        case (int)TOK_LE:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE LS"); emit_line(b, "MOVLS r0, #1"); emit_line(b, "MOVHI r0, #0"); }
            else             { emit_line(b, "ITE LE"); emit_line(b, "MOVLE r0, #1"); emit_line(b, "MOVGT r0, #0"); }
            break;
        case (int)TOK_GE:
            emit_line(b, "CMP r0, r1");
            if (is_unsigned) { emit_line(b, "ITE HS"); emit_line(b, "MOVHS r0, #1"); emit_line(b, "MOVLO r0, #0"); }
            else             { emit_line(b, "ITE GE"); emit_line(b, "MOVGE r0, #1"); emit_line(b, "MOVLT r0, #0"); }
            break;
        default:
            emit_line(b, "; unknown binary op");
            break;
    }
}

static void emit_unary(AsmBuffer *b, int op) {
    switch (op) {
        case (int)TOK_MINUS:
            emit_line(b, "NEG r0, r0");
            break;
        case (int)TOK_BNOT:
            emit_line(b, "MVN r0, r0");
            break;
        case (int)TOK_NOT:
            /* FIX(logical-not): the old sequence was
               CMP / MOV r0,#0 / IT EQ / MOVEQ r0,#1.
               "MOV r0, #0" assembles to 16-bit "movs r0,#0" which
               overwrites the flags set by CMP, so IT EQ always fired
               and "!x" was constantly 1 (this deadlocked
               while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) on the H7).
               Use the same ITE + two conditional MOV shape as
               TOK_EQ/TOK_NE: conditional MOVs do not update flags. */
            emit_line(b, "CMP r0, #0");
            emit_line(b, "ITE EQ");
            emit_line(b, "MOVEQ r0, #1");
            emit_line(b, "MOVNE r0, #0");
            break;
        default:
            emit_line(b, "; unknown unary op");
            break;
    }
}

/* ------------------- frame accesses past the imm12 range -------------------
 * "[r6, #off]" carries a 12-bit byte offset.  A function whose frame is larger
 * than that cannot reach its high slots in one instruction -- and a single
 * `char buf[5000];` is enough to push every later local past 4095.  That used to
 * be encoded as "off & 0xFFF", i.e. a load or store of the *wrong* slot with no
 * diagnostic at all.
 *
 * The fix is the base computation the instruction needs:
 *     LDR r0, [r6, #5000]
 *   ->MOVW ip, #5000 / MOVT ip, #0 / ADD ip, r6, ip / LDR r0, [ip]
 * and the same for "ADD rX, r6, #off" (taking a local's address).  ip (r12) is
 * the only register no emitted sequence keeps alive across a local access.
 *
 * This is a correctness pass, not an optimisation: it runs unconditionally and
 * only touches lines that would otherwise be refused by the encoder. */
static void ark_fix_far_offset(AsmBuffer *b, int from) {
    AsmBuffer out;
    int pos = from;
    int changed = 0;
    char line[256];

    /* Cheap first look: most functions have no such access at all. */
    {
        int p2 = from, found = 0;
        while (p2 + 8 <= b->len) {
            if (b->buf[p2] == 'r' && b->buf[p2+1] == '6' && b->buf[p2+2] == ',' &&
                b->buf[p2+3] == ' ') {
                long n = 0;
                const char *q = b->buf + p2 + 4;   /* '#' of "r6, #" */
                int ok = 1;
                if (*q == '#') {
                    q++;
                    if (*q < '0' || *q > '9') ok = 0;
                    while (ok && *q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; }
                    if (ok && n > 4095) { found = 1; break; }
                }
            }
            p2++;
        }
        if (!found) return;
    }

    asmbuf_init(&out);
    for (pos = from; pos < b->len; ) {
        int len = 0, i;
        long n = 0;
        int big = 0;
        const char *br = 0;
        const char *s;
        while (pos + len < b->len && b->buf[pos + len] != '\n') len++;
        if (len >= (int)sizeof(line)) {
            for (i = 0; i < len; i++) asmbuf_putc(&out, b->buf[pos + i]);
            asmbuf_putc(&out, '\n');
            pos += len + 1;
            continue;
        }
        for (i = 0; i < len; i++) line[i] = b->buf[pos + i];
        line[len] = 0;

        /* locate "r6, #<n>" and decide whether n is out of range */
        s = line;
        while (*s && !(s[0] == 'r' && s[1] == '6' && s[2] == ',' && s[3] == ' ')) s++;
        if (*s) {
            const char *q = s + 4;              /* '#' of "r6, #" */
            int ok = (*q == '#');
            if (ok) {
                q++;
                if (*q < '0' || *q > '9') ok = 0;
                while (ok && *q >= '0' && *q <= '9') { n = n * 10 + (*q - '0'); q++; }
            }
            if (ok && n > 4095) {
                big = 1;
                br = s;              /* points at "r6, #..." */
                if (br > line && br[-1] == '[') br--;   /* include the '[' */
            }
        }

        if (!big) {
            for (i = 0; i < len; i++) asmbuf_putc(&out, b->buf[pos + i]);
            asmbuf_putc(&out, '\n');
            pos += len + 1;
            continue;
        }

        /* Emit the address computation, then the original instruction against
         * it.  Two shapes reach here and they need different rewrites:
         *   "    LDR r0, [r6, #4904]"  ->  MOVW/MOVT, ADD ip,r6,ip, "LDR r0, [ip]"
         *   "    ADD r0, r6, #4904"    ->  MOVW/MOVT,             "ADD r0, r6, ip"
         * The second shape takes the address of a far local, and a big frame
         * hits it first: every array past 4095 bytes is reached through its
         * address.  Two separate bugs came from treating it like the first:
         * copying the head and appending "[ip]" produced "ADD r0, [ip]", which
         * the assembler rejects; and emitting "ADD ip, r6, ip" first made the
         * next "ADD r0, r6, ip" compute r6 + (r6 + n), which faults at once.
         */
        asmbuf_puts(&out, "    MOVW ip, #");
        asmbuf_int(&out, (long)n & 0xFFFF);
        asmbuf_putc(&out, '\n');
        asmbuf_puts(&out, "    MOVT ip, #");
        asmbuf_int(&out, ((long)n >> 16) & 0xFFFF);
        asmbuf_putc(&out, '\n');
        if (br != s) {
            int head = (int)(br - line);   /* just before the '[' */
            asmbuf_puts(&out, "    ADD ip, r6, ip\n");
            for (i = 0; i < head; i++) asmbuf_putc(&out, line[i]);
            asmbuf_puts(&out, "[ip]\n");
        } else {
            int head = (int)(br - line);   /* "    ADD Rt, " */
            for (i = 0; i < head; i++) asmbuf_putc(&out, line[i]);
            asmbuf_puts(&out, "r6, ip\n");
        }
        changed++;
        pos += len + 1;
    }

    if (changed) {
        asmbuf_reserve(b, from + out.len);
        if (from + out.len + 1 <= b->cap) {
            int i;
            for (i = 0; i < out.len; i++) b->buf[from + i] = out.buf[i];
            b->buf[from + out.len] = 0;
            b->len = from + out.len;
        }
    }
}

#if ARKCC_FEATURE_OPT_REGALLOC
/* --------------------------- register allocation -------------------------
 * What this is, exactly: after a function has been emitted, its text is walked
 * once and the hottest local slots are moved out of memory and into r8-r11.
 * "In a register" here means the emitted instruction changes shape, not that a
 * graph-colouring allocator has run:
 *      LDR r0, [r6, #off]   ->   MOV r0, rN
 *      STR r0, [r6, #off]   ->   MOV rN, r0
 * and rN joins the same PUSH/POP list the prologue already uses, so the frame
 * layout (and therefore every other offset) is untouched.
 *
 * Why a text pass instead of IR-level liveness: the IR stack machine has ~200
 * opcodes, and each one would have to learn about a new register.  A text pass
 * has exactly one rule to get right, and the rule is checked rather than
 * assumed -- a slot is only ever selected when *every* mention of it in the
 * whole function is one of the two lines above.  One mention in any other shape
 * (LDRB, STRH, "ADD r0, r6, #off", a 64-bit half, a struct field, a volatile
 * access) marks the slot as tainted and it stays in memory.  That is the whole
 * safety argument, and tests/run_regalloc_tests.py fails if it is ever wrong.
 *
 * r8-r11 are the registers: the backend never names them, and AAPCS makes them
 * callee-saved, so they survive calls to __aeabi_* helpers and to user code.
 *
 * The cost model, measured rather than assumed: a near slot ("LDR r0,[r6,#0..124]"
 * with a multiple-of-4 offset) is a 16-bit instruction, and so is "MOV r0, rN" --
 * moving a near slot saves *nothing*.  Only a far slot (offset > 124, or not a
 * multiple of 4) is 32-bit and saves 2 bytes.  The prologue/epilogue PUSH/POP
 * grow by 2 bytes each the moment a high register joins the list, so a function
 * pays 4 bytes once, no matter how many registers it takes.  Taking near slots
 * anyway was measured on src/irgen.c: 1853 slots moved, code grew 148 bytes.
 * Hence two modes:
 *   default              take a slot only when its byte savings pay for the
 *                        4-byte PUSH/POP widening -> the function never grows;
 *   -fopt-regalloc-near  also take near slots (>= 3 accesses): fewer memory
 *                        accesses in the hot path, at ~4 bytes per function.
 *
 * Two frame rules, both found by running the varargs gate with the pass on:
 *
 *   - the saved block anchors everything above the locals.  The prologue pushes
 *     r0-r3 (variadic) and {r5,r6,r7,lr} between sp_entry and the frame, so
 *     every offset above local_size names the saved block or the caller's
 *     argument image, and widening the saved PUSH moves all of them.  They are
 *     shifted, not left stale (ra_shift_line);
 *   - the saved block stays a multiple of 8 bytes.  An odd count of saved words
 *     leaves sp at 4 mod 8, which breaks AAPCS at every call and would unalign
 *     the va region too, so an odd count takes one spare register. */
#define RA_MAX_SLOTS 96
#define RA_MAX_REGS  4
#define RA_MIN_USES  3      /* 2 bytes saved per use, 4 bytes of PUSH/POP to pay */

typedef struct {
    int off;
    int uses;
    int saves;    /* bytes of instruction text this slot would save */
    int taint;
    int reg;
} RaSlot;

static int ra_enabled(void) {
    return g_config.enable_opt_regalloc ? 1 : 0;
}

static int ra_pre8(const char *s, const char *pat) {
    int i;
    for (i = 0; i < 8; i++)
        if (s[i] != pat[i]) return 0;
    return 1;
}

/* Classify one assembly line.
 *   -1  no "[r6, #<n>]" at all
 *    0  exactly "LDR r0, [r6, #n]"   (*off, *is_ldr = 1)
 *    1  exactly "STR r0, [r6, #n]"   (*off, *is_ldr = 0)
 *    2  names that slot in any other shape -- cannot be kept in a register */
static int ra_classify(const char *ln, int *off, int *is_ldr, int *wide) {
    const char *p = ln;
    const char *br;
    int n = 0;
    /* Any "r6, #n" at all -- not just "[r6, #n]".  "ADD r0, r6, #n" is the
     * address of the slot, and a slot whose address escapes cannot live in a
     * register: the pointer would keep writing to memory the variable no
     * longer uses. */
    while (p[0] && !(p[0] == 'r' && p[1] == '6' && p[2] == ',' && p[3] == ' '))
        p++;
    if (!p[0]) return -1;
    if (p == ln || p[-1] != '[') return 2;
    br = p - 1;
    p = br + 5;
    if (*p != '#') return 2;
    p++;
    if (*p < '0' || *p > '9') return 2;
    while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
    *off = n;
    if (*p != ']') return 2;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r') p++;
    if (*p) return 2;                     /* trailing text: not the shape we model */
    /* Same rule the encoder uses: the 16-bit form covers offsets 0..124 that
     * are a multiple of 4.  Everything else is 32-bit, and only that case makes
     * replacing the access with a MOV smaller. */
    *wide = (n > 124 || (n & 3) != 0);
    {
        const char *s = ln;
        while (*s == ' ' || *s == '\t') s++;
        if (s + 8 == br && ra_pre8(s, "LDR r0, ")) { *is_ldr = 1; return 0; }
        if (s + 8 == br && ra_pre8(s, "STR r0, ")) { *is_ldr = 0; return 0; }
    }
    return 2;
}

static int ra_slot(RaSlot *slots, int *n, int off) {
    int i;
    for (i = 0; i < *n; i++)
        if (slots[i].off == off) return i;
    if (*n >= RA_MAX_SLOTS) return -1;    /* table full: treat as tainted */
    slots[*n].off = off;
    slots[*n].uses = 0;
    slots[*n].saves = 0;
    slots[*n].taint = 0;
    slots[*n].reg = 0;
    return (*n)++;
}

/* Write a non-negative decimal, return the new output index. */
static int ra_num_out(char *dst, int o, int v) {
    char num[12];
    int n = 0, t;
    if (v == 0) num[n++] = '0';
    while (v > 0) { num[n++] = (char)('0' + v % 10); v /= 10; }
    for (t = n - 1; t >= 0; t--) dst[o++] = num[t];
    return o;
}

/* Is this line "    ADD rX, r6, rX"?  emit_va_ptr() is the only emitter of
 * that shape: it materialises frame + offset in a register. */
static int ra_adds_r6(const char *ln, int reg) {
    int i = 0, r = 0;
    while (ln[i] == ' ') i++;
    if (!(ln[i] == 'A' && ln[i+1] == 'D' && ln[i+2] == 'D' && ln[i+3] == ' ')) return 0;
    i += 4;
    if (ln[i] != 'r') return 0;
    for (i++; ln[i] >= '0' && ln[i] <= '9'; i++) r = r * 10 + (ln[i] - '0');
    if (r != reg) return 0;
    if (!(ln[i] == ',' && ln[i+1] == ' ' && ln[i+2] == 'r' && ln[i+3] == '6' &&
          ln[i+4] == ',' && ln[i+5] == ' ')) return 0;
    i += 6;
    if (ln[i] != 'r') return 0;
    r = 0;
    for (i++; ln[i] >= '0' && ln[i] <= '9'; i++) r = r * 10 + (ln[i] - '0');
    return r == reg;
}

/* Rewrite the frame offsets of one rebuilt line.  Only two shapes can carry an
 * offset that sits above the locals:
 *
 *     [r6, #N]                          a caller stack argument, or the va region
 *     MOVW rX, #N  +  ADD rX, r6, rX    emit_va_ptr(): frame + va offset
 *
 * `next` is the following line ("" at the end of the function) -- the MOVW is
 * rewritten only when that pair is really an address computation.  `dst` must
 * hold 4*len bytes: a line is at most 255 characters and each rewrite adds at
 * most two digits.  Returns the new length. */
static int ra_shift_line(const char *ln, int len, const char *next,
                         int base, int delta, char *dst) {
    int i = 0, o = 0;

    if (len > 12 && ln[4] == 'M' && ln[5] == 'O' && ln[6] == 'V' && ln[7] == 'W' &&
        ln[8] == ' ' && ln[9] == 'r') {
        int k = 10, reg = 0, n = 0;
        while (k < len && ln[k] >= '0' && ln[k] <= '9') { reg = reg * 10 + (ln[k] - '0'); k++; }
        if (k + 2 < len && ln[k] == ',' && ln[k+1] == ' ' && ln[k+2] == '#') {
            int st = k + 3;
            k = st;
            while (k < len && ln[k] >= '0' && ln[k] <= '9') { n = n * 10 + (ln[k] - '0'); k++; }
            if (k > st && k == len && n >= base && ra_adds_r6(next, reg)) {
                for (i = 0; i < st; i++) dst[o++] = ln[i];
                o = ra_num_out(dst, o, n + delta);
                dst[o] = 0;
                return o;
            }
        }
    }

    while (i < len) {
        if (ln[i] == '[' && i + 6 <= len && ln[i+1] == 'r' && ln[i+2] == '6' &&
            ln[i+3] == ',' && ln[i+4] == ' ' && ln[i+5] == '#') {
            int k = i + 6, n = 0;
            while (k < len && ln[k] >= '0' && ln[k] <= '9') { n = n * 10 + (ln[k] - '0'); k++; }
            if (k > i + 6 && k < len && ln[k] == ']' && n >= base) {
                int t;
                for (t = i; t < i + 6; t++) dst[o++] = ln[t];   /* "[r6, #" */
                o = ra_num_out(dst, o, n + delta);
                i = k;
                continue;
            }
        }
        dst[o++] = ln[i++];
    }
    dst[o] = 0;
    return o;
}

/* Rewrite b->buf[from..b->len) in place.  Called once per emitted function, at
 * the moment its text is complete, so "the last slice of the buffer" really is
 * that function and nothing else. */
static void ark_regalloc_function(AsmBuffer *b, int from, IrFunc *f, int local_eff) {
    static RaSlot slots[RA_MAX_SLOTS];
    int nslots = 0;
    int pos, i, k;
    /* First frame byte above this frame's locals: the saved-register block
     * starts there, and the caller's argument image right after it.  Every
     * local lives below it, so an offset >= base is never a local. */
    int base = local_eff + 16;
    int extra;
    int made[RA_MAX_REGS];
    int nregs = 0;
    int have_prologue = 0;
    AsmBuffer out;
    char line[256];
    char nline[256];

    if (!ra_enabled()) return;

    /* Pass 1: every mention of every frame slot. */
    for (pos = from; pos < b->len; ) {
        int len = 0, off = 0, is_ldr = 0, wide = 0, kind;
        while (pos + len < b->len && b->buf[pos + len] != '\n') len++;
        if (len >= (int)sizeof(line)) { pos += len + 1; continue; }
        for (i = 0; i < len; i++) line[i] = b->buf[pos + i];
        line[len] = 0;
        if (line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ' &&
            ra_pre8(line + 4, "PUSH {r5"))
            have_prologue = 1;
        kind = ra_classify(line, &off, &is_ldr, &wide);
        if (kind >= 0) {
            int s = ra_slot(slots, &nslots, off);
            if (s < 0) { pos += len + 1; continue; }
            if (kind == 2 || off >= base) {
                /* Outside this frame's locals -- a caller stack argument, or an
                 * address that escaped -- so it stays in memory.  Caching an
                 * incoming argument would also change what the function sees
                 * when the slot is written through a pointer. */
                slots[s].taint = 1;
            } else {
                slots[s].uses++;
                if (wide) slots[s].saves += 2;
            }
        }
        pos += len + 1;
    }
    /* Volatile locals go through the same two instruction shapes as ordinary
     * ones, so the text cannot tell them apart -- but keeping one in a register
     * would swallow exactly the accesses the keyword exists to preserve.  The
     * IR knows, so ask it rather than guessing from the listing. */
    if (f) {
        int k2;
        for (k2 = 0; k2 < f->code_count; k2++) {
            IrInst *I2 = &f->code[k2];
            int s;
            if (!(I2->aux & IR_AUX_VOLATILE)) continue;
            if (I2->op != IR_PUSH_VAR && I2->op != IR_STORE && I2->op != IR_PUSH_ADDR)
                continue;
            s = ra_slot(slots, &nslots, (int)I2->val);
            if (s >= 0) slots[s].taint = 1;
        }
    }

    if (!have_prologue || nslots == 0) return;   /* __naked or nothing to do */

    /* Pick the clean slots that pay for themselves.  Default: rank by bytes
     * saved and stop once the 4-byte PUSH/POP widening is covered, so a
     * function never grows.  -fopt-regalloc-near: rank by access count and take
     * near slots too.  Ties go to the lower offset, so the same source always
     * gets the same assignment. */
    {
        int budget = 4;            /* one wider PUSH + one wider POP */
        int near_mode = g_config.enable_opt_regalloc_near ? 1 : 0;
        for (k = 0; k < RA_MAX_REGS; k++) {
            int best = -1;
            for (i = 0; i < nslots; i++) {
                if (slots[i].reg || slots[i].taint) continue;
                if (near_mode) {
                    if (slots[i].uses < RA_MIN_USES) continue;
                } else {
                    if (slots[i].saves <= 0) continue;
                }
                if (best < 0) { best = i; continue; }
                if (near_mode) {
                    if (slots[i].uses > slots[best].uses ||
                        (slots[i].uses == slots[best].uses && slots[i].off < slots[best].off))
                        best = i;
                } else {
                    if (slots[i].saves > slots[best].saves ||
                        (slots[i].saves == slots[best].saves && slots[i].off < slots[best].off))
                        best = i;
                }
            }
            if (best < 0) break;
            if (!near_mode) {
                if (budget - slots[best].saves < 0 && nregs > 0) break;
                budget -= slots[best].saves;
            }
            slots[best].reg = 8 + k;
            made[nregs++] = 8 + k;
        }
        if (!near_mode && nregs > 0 && budget > 0) {
            /* The savings did not cover the wider PUSH/POP after all: hand the
             * registers back rather than grow the function. */
            int j, s2;
            for (j = 0; j < nregs; j++)
                for (s2 = 0; s2 < nslots; s2++)
                    if (slots[s2].reg == made[j]) slots[s2].reg = 0;
            nregs = 0;
        }
    }
    if (nregs == 0) return;

    /* An odd number of saved words would leave sp at 4 mod 8 once the prologue
     * has run: AAPCS wants 8 at every BL, and the va region the prologue builds
     * is 8-aligned only because the saved block is.  Take one spare register
     * instead of giving up a useful one -- a PUSH that names any high register
     * is 32-bit whatever the length of its list, so the spare costs nothing. */
    if (nregs & 1) {
        int r;
        for (r = 8; r <= 11; r++) {
            int j, used = 0;
            for (j = 0; j < nregs; j++)
                if (made[j] == r) used = 1;
            if (!used) { made[nregs++] = r; break; }
        }
    }
    /* Bytes the widened PUSH adds above the frame pointer, and therefore the
     * amount every offset that names the saved block or the caller's argument
     * image has to grow (stage 3 below). */
    extra = 4 * nregs;

    /* Pass 2: rebuild the text of this function. */
    asmbuf_init(&out);
    for (pos = from; pos < b->len; ) {
        int len = 0, off = 0, is_ldr = 0, wide = 0, kind;
        while (pos + len < b->len && b->buf[pos + len] != '\n') len++;
        if (len >= (int)sizeof(line)) {
            for (i = 0; i < len; i++) asmbuf_putc(&out, b->buf[pos + i]);
            asmbuf_putc(&out, '\n');
            pos += len + 1;
            continue;
        }
        for (i = 0; i < len; i++) line[i] = b->buf[pos + i];
        line[len] = 0;

        kind = -1;
        if (line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ')
            kind = ra_classify(line, &off, &is_ldr, &wide);
        if (kind == 0 || kind == 1) {
            int s = ra_slot(slots, &nslots, off);
            int r = (s >= 0) ? slots[s].reg : 0;
            if (r) {
                asmbuf_puts(&out, is_ldr ? "    MOV r0, r" : "    MOV r");
                asmbuf_int(&out, r);
                asmbuf_puts(&out, is_ldr ? "\n" : ", r0\n");
                pos += len + 1;
                continue;
            }
        }
        if (line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ' &&
            ra_pre8(line + 4, "PUSH {r5")) {
            /* Same list, plus the registers this function really holds. */
            asmbuf_puts(&out, "    PUSH {r5, r6, r7");
            for (k = 0; k < nregs; k++) {
                asmbuf_puts(&out, ", r");
                asmbuf_int(&out, made[k]);
            }
            asmbuf_puts(&out, ", lr}\n");
            pos += len + 1;
            continue;
        }
        if (line[0] == ' ' && line[1] == ' ' && line[2] == ' ' && line[3] == ' ' &&
            ra_pre8(line + 4, "POP {r5,")) {
            /* A variadic function returns through POP {r5,r6,r7,lr} and then
             * ADD sp,#16 / BX lr (it has to drop the r0-r3 mirror first), so
             * rewriting that line to POP {..,pc} would jump out 16 bytes early
             * with the pad still on the stack.  Keep lr; the rest of the list
             * is widened exactly like the non-variadic case. */
            int keeps_lr = (strstr(line, ", lr}") != 0);
            asmbuf_puts(&out, "    POP {r5, r6, r7");
            for (k = 0; k < nregs; k++) {
                asmbuf_puts(&out, ", r");
                asmbuf_int(&out, made[k]);
            }
            asmbuf_puts(&out, keeps_lr ? ", lr}\n" : ", pc}\n");
            pos += len + 1;
            continue;
        }
        for (i = 0; i < len; i++) asmbuf_putc(&out, b->buf[pos + i]);
        asmbuf_putc(&out, '\n');
        pos += len + 1;
    }

    /* Stage 3: the saved block grew by `extra` bytes, so every frame offset at
     * or above `base` is that much too small -- the caller's stack arguments
     * and the va region <stdarg.h> walks.  Left alone, a variadic function or
     * one with more than four parameters reads the wrong words; both run in the
     * varargs gate with the pass on.  Locals are all below `base`, so no local
     * slot can be touched. */
    if (extra > 0) {
        AsmBuffer shf;
        int opos;
        asmbuf_init(&shf);
        for (opos = 0; opos < out.len; ) {
            int len = 0, nlen = 0, olen = 0;
            char shl[1024];
            const char *np;
            while (opos + len < out.len && out.buf[opos + len] != '\n') len++;
            if (len >= (int)sizeof(line)) {
                /* Unreachable: pass 2 slices by the same limit and never emits
                 * a longer line.  Copy verbatim rather than guess. */
                for (i = 0; i < len; i++) asmbuf_putc(&shf, out.buf[opos + i]);
                asmbuf_putc(&shf, '\n');
                opos += len + 1;
                continue;
            }
            for (i = 0; i < len; i++) line[i] = out.buf[opos + i];
            line[len] = 0;
            np = out.buf + opos + len;
            if (*np == '\n') np++;
            while (np[nlen] && np[nlen] != '\n' && nlen < (int)sizeof(nline) - 1) {
                nline[nlen] = np[nlen];
                nlen++;
            }
            nline[nlen] = 0;
            olen = ra_shift_line(line, len, nline, base, extra, shl);
            for (i = 0; i < olen; i++) asmbuf_putc(&shf, shl[i]);
            asmbuf_putc(&shf, '\n');
            opos += len + 1;
        }
        asmbuf_reserve(&out, shf.len + 1);
        for (i = 0; i < shf.len; i++) out.buf[i] = shf.buf[i];
        out.buf[shf.len] = 0;
        out.len = shf.len;
    }

    /* Splice back.  In the default mode the selected slots pay for the wider
     * PUSH/POP, so the result is never longer; the guard is here so a future
     * change cannot silently overflow the buffer.  The reserve is not optional
     * now: stage 3 can lengthen a line by a digit or two. */
    asmbuf_reserve(b, from + out.len + 1);
    if (from + out.len + 1 <= b->cap) {
        for (i = 0; i < out.len; i++) b->buf[from + i] = out.buf[i];
        b->buf[from + out.len] = 0;
        b->len = from + out.len;
    }
}
#endif /* ARKCC_FEATURE_OPT_REGALLOC */

/* 16 for a variadic function: the prologue pushed r0-r3 in front of the saved
 * registers, so every epilogue of that function has 16 extra bytes to drop.
 * Zero for everything else, which keeps the non-variadic output byte-identical. */
static int g_va_epilogue_pad;

/* Frame adjustment that does not fit one encoding.  SUBW/ADDW take 0..4095 and
 * the 16-bit forms at most 508, but a function's frame can be larger than that:
 * preproc.c's process() reserves 17208 bytes.  That used to be emitted as a
 * single "SUB sp, sp, #17208", which the assembler truncated with "& 0x7F" and
 * turned into "sub sp, #312" -- a wrong answer on a stack pointer, and it is
 * now a hard error instead.  Split it into <=4095 pieces: a handful of extra
 * instructions, once per function. */
static void emit_sp_adjust(AsmBuffer *b, int amount, int is_sub) {
    while (amount > 0) {
        int piece = (amount > 4095) ? 4095 : amount;
        asmbuf_puts(b, is_sub ? "    SUB sp, sp, #" : "    ADD sp, sp, #");
        asmbuf_int(b, piece);
        asmbuf_putc(b, '\n');
        amount -= piece;
    }
}

static void emit_prologue(AsmBuffer *b, const char *fn_name, int local_size,
                          int va_lo, int va_pad, int is_irq, int is_naked, int dbg) {
    asmbuf_puts(b, fn_name);
    asmbuf_puts(b, ":\n");
    if (dbg) emit_line(b, ".cfi_startproc");
    if (is_naked) {
        /* __naked: emit no prologue; the function body is entirely the user's responsibility */
        return;
    }
    /* Variadic: save the register argument words that can still hold a
     * variadic argument -- r_va_lo .. r3, where va_lo is NCRN rounded down to
     * an even number.  They then sit at [sp_entry-va_pad, sp_entry) -- directly
     * below the caller's stack argument block -- so the va region is the
     * caller's argument image verbatim, and the epilogue has va_pad extra bytes
     * to drop (see emit_epilogue_return).  va_pad is a multiple of 8, so the
     * frame keeps the 8-byte alignment AAPCS requires; va_pad == 0 means the
     * caller already spends r0-r3 on named arguments and nothing is pushed. */
    if (va_pad > 0) {
        int r;
        asmbuf_puts(b, "    PUSH {");
        for (r = va_lo; r < 4; r++) {
            if (r != va_lo) asmbuf_puts(b, ", ");
            asmbuf_puts(b, "r");
            asmbuf_int(b, r);
        }
        asmbuf_puts(b, "}\n");
    }
    if (is_irq) {
        /* __irq: Cortex-M hardware auto-saves r0-r3, r12, lr, pc, xpsr on
         * exception entry. We only need to save r5, r6, r7 (arkcc's
         * callee-saved registers used as frame pointer etc.) and lr
         * (EXC_RETURN) so that POP {pc} triggers exception return. */
        emit_line(b, "PUSH {r5, r6, r7, lr}");
    } else {
        emit_line(b, "PUSH {r5, r6, r7, lr}");
    }
    if (local_size > 0) emit_sp_adjust(b, local_size, 1);
    emit_line(b, "MOV r6, sp");
}

static void emit_epilogue_return(AsmBuffer *b, int local_size,
                                 int is_irq, int is_naked, int value_in_r0) {
    if (is_naked) {
        /* __naked: just return, do not restore stack/registers */
        emit_line(b, "BX lr");
        return;
    }
    /* --reg-top: the return value is already in r0 (the IR stack top lives in
     * r0), so popping it back into r0 would be a no-op pair.  Off, or when the
     * caller did not model it, this is the original emit_pop_r0(). */
    if (!value_in_r0) emit_pop_r0(b);
    emit_line(b, "MOV sp, r6");
    if (local_size > 0) emit_sp_adjust(b, local_size, 0);
    if (g_va_epilogue_pad) {
        /* Variadic: the prologue pushed r_va_lo..r3 *below* the saved registers
         * (so that the register argument words stay contiguous with the
         * caller's stack arguments), so after restoring the saved registers
         * another g_va_epilogue_pad bytes have to be dropped before returning.
         * POP into lr, drop the words, then branch -- POP {.., pc} cannot be
         * followed by a stack adjustment.  For an __irq handler lr holds
         * EXC_RETURN, and BX lr performs the exception return on M-profile, so
         * this shape is correct there too.  When nothing was pushed (all four
         * register words are named arguments) this branch is not taken and the
         * plain POP {.., pc} below is used. */
        emit_line(b, "POP {r5, r6, r7, lr}");
        emit_add_sp(b, g_va_epilogue_pad);
        emit_line(b, "BX lr");
    } else if (is_irq) {
        emit_line(b, "POP {r5, r6, r7, pc}");
    } else {
        emit_line(b, "POP {r5, r6, r7, pc}");
    }
}

/* Global variable SRAM address allocation for ARM backend.
 * Globals are placed in SRAM starting at 0x20000000; __at variables keep
 * their absolute address. Codegen emits MOVW/MOVT to load these addresses
 * instead of LDR =label (which the minimal assembler doesn't fully support). */
static const char *g_gvar_names[ARM_MAX_GVARS];
static unsigned    g_gvar_addrs[ARM_MAX_GVARS];
static int         g_gvar_sizes[ARM_MAX_GVARS];  /* #6 FIX: track global variable sizes */
static int         g_gvar_count = 0;

/* SCT-1/SCT-3: Custom section address tracking. When a global has a
 * g->section (from #pragma arm section / __attribute__((section))),
 * gvar_assign looks up the section's exec region base from the scatter
 * description and allocates variables contiguously from that base. Each
 * entry tracks the next free offset for one named section. Reset per
 * compilation in gvar_assign. */
/* Named sections tracked for #pragma arm section / __attribute__((section)).
 * Host 128 / board 16, overridable with -DARK_MAX_CUSTOM_SECTIONS. */
#define ARM_MAX_CUSTOM_SECTIONS ARK_MAX_CUSTOM_SECTIONS
static struct {
    const char *name;     /* section name (points into IrGlobal, lives through codegen) */
    unsigned    cur_addr; /* next free address within this section's exec region */
} g_custom_sect[ARM_MAX_CUSTOM_SECTIONS];
static int g_custom_sect_count = 0;

/* F3/S5: Track .data/.bss boundaries for startup initialization */
static unsigned g_data_sram_start = 0;  /* First .data SRAM address */
static unsigned g_data_sram_end   = 0;  /* One past last .data SRAM address */
static unsigned g_bss_sram_start  = 0;  /* First .bss SRAM address */
static unsigned g_bss_sram_end    = 0;  /* One past last .bss SRAM address */

/* Does `addr` point into this translation unit's own .data/.bss window?
 *
 * This has to exist in every build, including the ones where --reloc-data is
 * compiled out (ARKCC_PROFILE_STM32_MIN sets ARKCC_FEATURE_ARM_RELOC_DATA to
 * 0): the ten call sites in the store/load paths are shared, and leaving them
 * pointing at a function that the preprocessor removed produced
 * "addr_in_own_data declared implicitly" warnings and a link error, not a
 * working build.  With the feature off the four boundaries stay 0, so the
 * answer is always "no" -- which is exactly "nothing gets marked".
 */
static int addr_in_own_data(unsigned addr) {
    if (g_data_sram_start != g_data_sram_end &&
        addr >= g_data_sram_start && addr < g_data_sram_end) return 1;
    if (g_bss_sram_start != g_bss_sram_end &&
        addr >= g_bss_sram_start && addr < g_bss_sram_end) return 1;
    return 0;
}

#if ARKCC_FEATURE_ARM_RELOC_DATA
/* --reloc-data: data-address patch sites.
 *
 * -link has always concatenated code but has never been able to move a data
 * section: every object laid its globals out from the same SRAM base, so two
 * objects that both declare RAM aliased each other and the link refused with
 * "SRAM overlap".  Moving the data means rewriting every absolute address
 * baked into the instruction stream, and only codegen knows which MOVW/MOVT
 * pairs came from gvar_find() rather than from an unrelated 32-bit constant:
 * the encoding of 0x20000004 does not say whether it is an array address or
 * an integer literal.
 *
 * So codegen marks those pairs with the .arkdata pseudo-op and the assembler
 * records the offsets as relocation type 6; the linker adds the object's data
 * delta to each.  With the option off, not one byte changes: the marker is not
 * emitted and emit_mov_addr32() falls straight through to emit_mov_imm().
 */
#endif

/* Materialise `addr` in rd, flagging it for the linker when `marked`.
 *
 * `marked` is passed by the caller rather than derived here, because two
 * different facts are being expressed:
 *   - a global from gvar_find(): relocatable iff it sits inside this unit's own
 *     window (a __at variable or a variable in another scatter region must NOT
 *     move, so the caller asks addr_in_own_data());
 *   - the .data/.bss bounds used by __arkcc_init_globals: always relocatable,
 *     including the one-past-the-end bound that addr_in_own_data() rejects.
 *
 * The MOVW/MOVT pair is forced in the marked case: a 16-bit MOV has no halfword
 * for the linker to patch, so an armcc-style short form would silently keep a
 * stale address. */
static void emit_mov_addr32(AsmBuffer *b, int rd, unsigned addr, int marked) {
#if ARKCC_FEATURE_ARM_RELOC_DATA
    if (g_config.enable_reloc_data && marked) {
        asmbuf_puts(b, "    .arkdata\n");
        asmbuf_puts(b, "    MOVW r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, (int)(addr & 0xFFFFu));
        asmbuf_putc(b, '\n');
        asmbuf_puts(b, "    MOVT r");
        asmbuf_int(b, rd);
        asmbuf_puts(b, ", #");
        asmbuf_int(b, (int)((addr >> 16) & 0xFFFFu));
        asmbuf_putc(b, '\n');
        return;
    }
#else
    (void)marked;
#endif
    emit_mov_imm(b, rd, (int)addr);
}

/* Scatter (.sct) load description: when --scatter <file> is given, the parsed
 * regions drive address assignment instead of the hardcoded defaults below.
 * g_flash_base is the base for code labels (LR load address); g_data_base /
 * g_bss_base are the SRAM origins for .data (+RW) and .bss (+ZI). These are
 * refreshed by arm_scatter_load() at the start of each codegen_arm() call. */
static ScatterDesc g_scatter;
static int          g_scatter_loaded = 0;
static unsigned     g_flash_base = 0x08000000u;  /* default STM32F4 Flash base */
static unsigned     g_data_base  = 0x20000000u;  /* default SRAM base */
static unsigned     g_bss_base   = 0x20000000u;  /* default SRAM base (.bss follows .data) */

/* Debug flag: when set, arm_assemble dumps the symbol map and scatter layout
 * after assembly. On host it is latched from getenv("ARKCC_DUMP_SYMS") at
 * arkcc startup by arm_dump_syms_init(); on STM32 getenv() is unavailable
 * under semihosting, so the flag is instead set by the UART "debug syms"
 * command. Defined here (early) so arm_scatter_load() can read it. */
int g_arm_dump_syms = 0;

/* --vector-table: reserve ARM_VT_SIZE bytes at the front of the image for the
 * Cortex-M vector table, so the table can be written *into* the reserved gap
 * instead of being prepended at link time.  Prepending shifts every byte by
 * 0x400; PC-relative Thumb-2 branches survive that, absolute addresses baked
 * by the assembler do not.  0 = old behaviour, byte for byte. */
int g_arm_vt_reserve = 0;

/* 1 when the selected CPU has a *double-precision* FPU (Cortex-M7 DP).
 * Double <-> float conversion is the one place where single-precision
 * hardware cannot stand in: VCVT.F64.F32 does not exist on the M4F's
 * fpv4-sp-d16, so emitting it there produces an instruction the assembler
 * rejects (and the core would fault on).  The software helper __aeabi_f2d /
 * __aeabi_d2f is used unless this is set.  Default 0: no -cpu means the
 * conservative path. */
int g_arm_fpu_dp = 0;

/* Cortex-M vector table: 256 entries, 1 KB.  Declared here (not next to
 * arm_link) because both the assembler -- which reserves this many bytes at the
 * front of the image -- and the linker need it. */
#define ARM_VT_ENTRIES 256
#define ARM_VT_SIZE   (ARM_VT_ENTRIES * 4)

/* Parse g_config.scatter_file (if set) into g_scatter and derive the Flash /
 * data / bss base addresses from the described execution regions. Called once
 * per compilation so the backend places sections per the scatter layout. */
/* Derive the code / .data / .bss base addresses from an already parsed
 * scatter description. The .sct path and the -T/--linker-script path must
 * share this rule: when only the linker-script branch derived the bases,
 * "--scatter <file>" parsed fine but left every address at the hardcoded
 * defaults, i.e. it was a silent no-op for code generation. */
#if ARKCC_FEATURE_SCATTER
static void arm_scatter_derive_bases(void) {
    g_flash_base = scatter_get_flash_base(&g_scatter);
    /* +RW region is the load origin for .data; +ZI for .bss.
     * If a +ZI region exists separately, .bss starts there, otherwise it
     * follows .data (set bss_base = data_base; gvar_assign lays them out
     * contiguously and records the real bss_start boundary). */
    g_data_base = scatter_get_data_base(&g_scatter, SCATTER_ATTR_RW);
    if (scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_ZI)) {
        g_bss_base = scatter_get_data_base(&g_scatter, SCATTER_ATTR_ZI);
    } else {
        g_bss_base = g_data_base;
    }
}
#endif

static void arm_scatter_load(void) {
    g_scatter_loaded = 0;
    g_scatter.load_regions = 0;
    g_scatter.error_count = 0;
    g_flash_base = 0x08000000u;
    g_data_base  = 0x20000000u;
    g_bss_base   = 0x20000000u;
#if ARKCC_FEATURE_SCATTER
    if (g_config.scatter_file && g_config.scatter_file[0] &&
        scatter_parse_file(g_config.scatter_file, &g_scatter) == 0) {
        g_scatter_loaded = 1;
        arm_scatter_derive_bases();
    } else if (g_config.linker_script && g_config.linker_script[0] &&
               linker_script_parse_file(g_config.linker_script, &g_scatter) == 0) {
        /* -T/--linker-script: parse GNU ld script the same way as .sct */
        g_scatter_loaded = 1;
        arm_scatter_derive_bases();
    }
#if ARKCC_FEATURE_SCATTER_CHECK
    /* 布局能不能用，在“派生基址之后、生成任何代码之前”就要判定：一旦把错误
     * 布局烘焙进地址，事后无法分辨是用户写错还是编译器读错。 */
    if (g_scatter_loaded) {
        scatter_validate(&g_scatter, g_flash_base, g_arm_vt_reserve);
    }
#endif
    if (g_scatter_loaded && g_arm_dump_syms) {
        ark_log("[scatter] flash=0x%08X data=0x%08X bss=0x%08X\n",
                g_flash_base, g_data_base, g_bss_base);
        scatter_dump(&g_scatter);
    }
#endif
}

static unsigned gvar_find(const char *name) {
    int i;
    for (i = 0; i < g_gvar_count; i++)
        if (ark_streq(g_gvar_names[i], name)) return g_gvar_addrs[i];
    return 0;
}

/* #6 FIX: Find global variable size by name */
static int gvar_find_size(const char *name) {
    int i;
    for (i = 0; i < g_gvar_count; i++)
        if (ark_streq(g_gvar_names[i], name)) return g_gvar_sizes[i];
    return 4;  /* default 4 bytes */
}

/* --- --reg-top: keep the top of the IR stack in r0 ------------------------
 *
 * Every IR op is translated mechanically: operands are popped off the machine
 * stack into r0/r1 and the result is pushed back.  For `t = a + b` that is four
 * instructions of stack traffic around one ADD, and a return emits `PUSH {r0}`
 * immediately followed by `POP {r0}`.
 *
 * The bounded fix is not a register allocator but one invariant: the value on
 * top of the IR stack may live in r0 instead of on the machine stack.  Only the
 * ops listed in ark_top_try() are modelled; any other op first spills r0 back
 * (ark_top_drain) and then runs its existing sequence unchanged, so the blast
 * radius of a mistake is one op, not the whole backend.
 *
 * Control-flow joins drain too: a label can be reached from two paths, and both
 * must agree on where the IR stack's top is.  That is why IR_LABEL and the
 * function entry/exit drain even though they emit nothing themselves.
 *
 * Invariant: when *top is 1, the IR stack top is in r0 and everything below it
 * is on the machine stack in the usual order.
 */
static int ark_top_enabled(void) {
    return g_config.enable_reg_top ? 1 : 0;
}

/* va region helpers (defined further down, next to aapcs_assign because they
 * explain the same word indexing). ark_top_try() runs before them, so their
 * prototypes live here. */
#define ARK_VA_BASE 8000
static void emit_va_ptr(AsmBuffer *b, int reg);
static int  is_va_ptr_sym(const char *name);

/* 1 while the function being emitted is variadic (set by codegen_arm before
 * the spill, cleared for every function, so it never leaks across). */
static int g_va_active;

/* Name of the function whose code is being emitted right now.  Only the
 * diagnostics that fire inside the instruction switch need it, but they fire
 * from two different places (the --reg-top peephole and the main switch), so
 * it is set once per function instead of threaded through both. */
static const char *g_cur_fn_name;

/* _ark_va_ptr outside a variadic function: <stdarg.h> was used where there is
 * no argument block to point at (va_start in a function that is not declared
 * with "...").  The honest answer is an error -- the behaviour this replaces
 * emitted `MOV r0,#0 / LDR r0,[r0]`, i.e. it read word 0 of the vector table
 * (the initial SP) and handed it back as an argument pointer. */
static void va_ptr_needs_variadic(void) {
    diag_tool(DIAG_SEV_ERROR, "arkcc",
              "'_ark_va_ptr' is read in '%s', which is not variadic: "
              "va_start/va_arg only mean something inside a function declared "
              "with '...'",
              g_cur_fn_name ? g_cur_fn_name : "?", 0, 0, 0, 0, 0);
}

static void ark_top_drain(AsmBuffer *b, int *top) {
    if (*top) {
        emit_push_r0(b);
        *top = 0;
    }
}

/* Returns 1 when the instruction was fully emitted and the switch must be
 * skipped, 0 when the caller falls through to the generic path (with *top
 * already drained unless the op is a join point).
 * *ret_in_r0 is set for IR_RETURN so its epilogue can skip the redundant pop. */
static int ark_top_try(AsmBuffer *b, IrInst *I, int *top, int *ret_in_r0) {
    switch (I->op) {
    case IR_FUNC_BEGIN:
        *top = 0;                       /* prologue owns the machine stack */
        return 1;
    case IR_FUNC_END:
        ark_top_drain(b, top);
        return 1;
    case IR_LABEL:
    case IR_LABEL_NAMED:
        ark_top_drain(b, top);          /* join point: let the switch emit it */
        return 0;
    case IR_RETURN:
        *ret_in_r0 = *top;              /* epilogue skips its pop when set */
        *top = 0;
        return 0;                       /* epilogue lives in the switch */
    case IR_PUSH_INT:
        ark_top_drain(b, top);
        emit_mov_imm(b, 0, I->val);
        *top = 1;
        return 1;
    case IR_PUSH_VAR: {
        int width = I->aux & 0xFF;
        /* 64-bit pushes two words; volatile reads need their DMB pair. Both
         * fall back so those paths stay exactly as they were -- but only after
         * putting r0 back on the stack: "fall back" means the IR stack top is
         * where the generic path expects it (on the machine stack). Skipping
         * the drain left the word in r0 and every later pop read one slot too
         * high (measured: `long long b = 200;` under --reg-top stored the stale
         * r0 as the low half and 200 as the high half). */
        if (width == 8) { ark_top_drain(b, top); return 0; }
        if (I->aux & IR_AUX_VOLATILE) { ark_top_drain(b, top); return 0; }
        ark_top_drain(b, top);
        if (width == 1)
            asmbuf_puts(b, (I->aux & 0x100) ? "    LDRB r0, [r6, #"
                                            : "    LDRSB r0, [r6, #");
        else if (width == 2)
            asmbuf_puts(b, (I->aux & 0x100) ? "    LDRH r0, [r6, #"
                                            : "    LDRSH r0, [r6, #");
        else
            asmbuf_puts(b, "    LDR r0, [r6, #");
        asmbuf_int(b, I->val);
        asmbuf_puts(b, "]\n");
        *top = 1;
        return 1;
    }
    case IR_PUSH_ADDR:
        ark_top_drain(b, top);
        asmbuf_puts(b, "    ADD r0, r6, #");
        asmbuf_int(b, I->val);
        asmbuf_putc(b, '\n');
        *top = 1;
        return 1;
    case IR_PUSH_GLOBAL: {
        unsigned addr;
        if (is_va_ptr_sym(I->name)) {
            if (!g_va_active) va_ptr_needs_variadic();
            ark_top_drain(b, top);
            emit_va_ptr(b, 0);
            *top = 1;
            return 1;
        }
        addr = gvar_find(I->name);
        if (gvar_find_size(I->name) >= 8) {           /* two words: fall back */
            ark_top_drain(b, top);
            return 0;
        }
        ark_top_drain(b, top);
        emit_mov_addr32(b, 0, addr, addr_in_own_data(addr));
        emit_line(b, "LDR r0, [r0]");
        *top = 1;
        return 1;
    }
    case IR_PUSH_GLOBAL_ADDR: {
        unsigned addr = gvar_find(I->name);
        ark_top_drain(b, top);
        emit_mov_addr32(b, 0, addr, addr_in_own_data(addr));
        *top = 1;
        return 1;
    }
    case IR_BINARY:
        if (*top) {
            /* right operand is in r0, left one is on the stack under it */
            asmbuf_puts(b, "    MOV r1, r0\n");
            emit_pop_r0(b);
        } else {
            emit_pop_r1(b);
            emit_pop_r0(b);
        }
        emit_binary(b, I->aux);
        *top = 1;
        return 1;
    case IR_UNARY:
        /* 64-bit forms touch two stack slots (I->val carries the width) and a
         * float/double form is not a one-word integer either, so the one-word
         * model does not apply to either: fall back with the stack drained. */
        if (I->val == 1 || I->val == 2) { ark_top_drain(b, top); return 0; }
        if (!*top) emit_pop_r0(b);
        emit_unary(b, I->aux);
        *top = 1;
        return 1;
    case IR_TRUNC:
        if (I->val == 8 || I->val == 16) {
            char op8[8];
            op8[0] = I->aux ? 'U' : 'S';
            op8[1] = 'X';
            op8[2] = 'T';
            op8[3] = (I->val == 8) ? 'B' : 'H';
            op8[4] = 0;
            if (!*top) emit_pop_r0(b);
            asmbuf_puts(b, "    ");
            asmbuf_puts(b, op8);
            asmbuf_puts(b, " r0, r0\n");
            *top = 1;
        }
        /* 32-bit widths need no work either way. */
        return 1;
    case IR_STORE_GLOBAL: {
        unsigned addr = gvar_find(I->name);
        if (gvar_find_size(I->name) >= 8) {
            ark_top_drain(b, top);                    /* two words: fall back */
            return 0;
        }
        if (!*top) emit_pop_r0(b);
        emit_mov_addr32(b, 1, addr, addr_in_own_data(addr));
        emit_line(b, "STR r0, [r1]");
        *top = 0;                        /* the store consumes the value */
        return 1;
    }
    case IR_STORE: {
        /* Local store: the generic path pops the value into r0 and writes it.
         * With the value already in r0 that is one whole push/pop pair saved
         * per assignment, which is the single most common IR pair in C code.
         * Only the plain 1/2/4-byte non-volatile forms are modelled. */
        int sw = I->aux & ~IR_AUX_VOLATILE;
        int is_volatile = (I->aux & IR_AUX_VOLATILE) != 0;
        if (sw == 0) sw = 4;
        if (is_volatile || sw >= 8) {
            ark_top_drain(b, top);                    /* not modelled: fall back */
            return 0;
        }
        if (!*top) emit_pop_r0(b);
        if (sw == 1) {
            asmbuf_puts(b, "    STRB r0, [r6, #");
            asmbuf_int(b, I->val);
            asmbuf_puts(b, "]\n");
        } else if (sw == 2) {
            asmbuf_puts(b, "    STRH r0, [r6, #");
            asmbuf_int(b, I->val);
            asmbuf_puts(b, "]\n");
        } else {
            asmbuf_puts(b, "    STR r0, [r6, #");
            asmbuf_int(b, I->val);
            asmbuf_puts(b, "]\n");
        }
        *top = 0;                        /* the store consumes the value */
        return 1;
    }
    case IR_POP:
        if (*top) *top = 0;              /* dropping a value in a register costs nothing */
        else emit_pop_r0(b);
        return 1;
    case IR_DUP:
        /* val > 1 asks for a multi-word duplicate (a long long is two stack
         * words); the register-top form can only model one word, so let the
         * generic emitter handle it. */
        if (I->val > 1) {
            ark_top_drain(b, top);
            return 0;
        }
        if (*top) {
            asmbuf_puts(b, "    PUSH {r0}\n");   /* one copy on the stack, one in r0 */
            return 1;
        }
        return 0;
    default:
        ark_top_drain(b, top);
        return 0;
    }
}

static int gvar_known(const char *name) {
    int i;
    for (i = 0; i < g_gvar_count; i++)
        if (ark_streq(g_gvar_names[i], name)) return 1;
    return 0;
}

/* SCT-1/SCT-3: Allocate `sz` bytes from the named custom section's exec region.
 * Looks up the section base via scatter_get_er_base_for_section the first time
 * the section is seen, then hands out consecutive addresses from that base.
 * Returns the assigned address, or 0 if the section is not described in the
 * scatter file (or no scatter is loaded) -- in that case the caller falls
 * back to the default .data/.bss sequential allocation. */
static unsigned custom_sect_alloc(const char *section_name, int sz) {
    int i;
    unsigned base, addr;
    /* Existing section: continue from the tracked offset. */
    for (i = 0; i < g_custom_sect_count; i++) {
        if (ark_streq(g_custom_sect[i].name, section_name)) {
            g_custom_sect[i].cur_addr = (g_custom_sect[i].cur_addr + 3) & ~3u;
            addr = g_custom_sect[i].cur_addr;
            g_custom_sect[i].cur_addr += sz;
            return addr;
        }
    }
    /* New section: resolve its base from the scatter description. */
    if (g_custom_sect_count >= ARM_MAX_CUSTOM_SECTIONS) {
        /* Returning 0 makes the caller fall back to default SRAM, i.e. the
         * variable lands at an address the scatter file did not ask for.
         * Report it so the build fails instead of shipping the wrong address. */
        char b1[12];
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "more than %s distinct named sections; raise "
                  "ARK_MAX_CUSTOM_SECTIONS and rebuild arkcc",
                  diag_num(b1, ARM_MAX_CUSTOM_SECTIONS), 0,0,0,0,0);
        return 0;
    }
#if ARKCC_FEATURE_SCATTER
    base = scatter_get_er_base_for_section(&g_scatter, section_name);
#else
    base = 0;
#endif
    if (base == 0) return 0;  /* not found: caller falls back to default */
    g_custom_sect[g_custom_sect_count].name = section_name;
    g_custom_sect[g_custom_sect_count].cur_addr = (base + 3) & ~3u;
    addr = g_custom_sect[g_custom_sect_count].cur_addr;
    g_custom_sect[g_custom_sect_count].cur_addr += sz;
    g_custom_sect_count++;
    return addr;
}

static void gvar_assign(IrProgram *prog) {
    unsigned data_sram = g_data_base;   /* .data origin (scatter +RW or 0x20000000) */
    unsigned bss_sram;                  /* .bss origin (computed below) */
    int gi;
    g_gvar_count = 0;
    /* Both passes below stop at ARM_MAX_GVARS, and a global without an address
     * is worse than a failure: the emit path either reports something obscure
     * or uses a stale address.  Count first, refuse once, name the macro. */
    {
        int need = 0, k;
        for (k = 0; k < prog->global_count; k++) {
            if (!prog->globals[k]->is_extern) need++;
        }
        if (need > ARM_MAX_GVARS) {
            char b1[12], b2[12];
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "%s global objects in this file but the address table holds "
                      "%s; raise ARM_MAX_GVARS and rebuild arkcc",
                      diag_num(b1, need), diag_num(b2, ARM_MAX_GVARS), 0,0,0,0);
            return;
        }
    }
    g_custom_sect_count = 0;  /* SCT-1/SCT-3: reset custom section tracking */
    g_data_sram_start = data_sram;  /* will be updated if .data globals exist */
    g_data_sram_end = data_sram;
    g_bss_sram_start = data_sram;
    g_bss_sram_end = data_sram;

    /* F3/S5: First pass - assign .data (initialized) globals to SRAM.
     * This ensures .data globals are contiguous, followed by .bss globals.
     * The startup code (__arkcc_init_globals) initializes .data and zeros .bss. */
    for (gi = 0; gi < prog->global_count && g_gvar_count < ARM_MAX_GVARS; gi++) {
        IrGlobal *g = prog->globals[gi];
        int sz;
        if (g->is_extern) continue;
        if (g->has_at) continue;  /* __at variables keep their absolute address */
        if (!g->has_init) continue;  /* skip .bss for now */
        /* Zero-initialized scalars go to .bss */
        if (!g->init_aggregate && !g->init_str && !g->is_float && g->init_int == 0) continue;
        sz = g->type ? type_size(g->type) : 4;
        if (sz <= 0) sz = 4;
        /* SCT-1/SCT-3: custom section globals get their address from the
         * scatter exec region rather than the default .data pool. */
        if (g->section && g_scatter_loaded) {
            unsigned caddr = custom_sect_alloc(g->section, sz);
            if (caddr) {
                g_gvar_names[g_gvar_count] = g->name;
                g_gvar_addrs[g_gvar_count] = caddr;
                g_gvar_sizes[g_gvar_count] = sz;
                g_gvar_count++;
                continue;
            }
            /* section not found in scatter: fall through to .data allocation */
        }
        /* Honour __align(n) / _Alignas(n) / __attribute__((aligned(n))):
         * an over-aligned object placed at 4 bytes would silently break
         * every DMA / cache-line assumption the program makes. */
        {
            unsigned req = (g->align_bytes > 4) ? (unsigned)g->align_bytes : 4u;
            unsigned rem = data_sram % req;
            if (rem) data_sram += req - rem;
        }
        g_gvar_names[g_gvar_count] = g->name;
        g_gvar_addrs[g_gvar_count] = data_sram;
        g_gvar_sizes[g_gvar_count] = sz;  /* #6 FIX */
        g_gvar_count++;
        data_sram += sz;
    }
    /* #BSS-ALIGN FIX: Align .data end / .bss start to 4 bytes so the
     * __arkcc_init_globals zeroing loop starts at an aligned address.
     * STR r2, [r0] on an unaligned r0 can fault on Cortex-M. */
    data_sram = (data_sram + 3) & ~3u;
    g_data_sram_end = data_sram;

    /* .bss origin: when scatter defines a separate +ZI region, .bss starts
     * there; otherwise it follows .data contiguously. Take the higher of the
     * +ZI base and the .data end so .data and .bss never overlap. */
    bss_sram = (g_bss_base > data_sram) ? g_bss_base : data_sram;
    g_bss_sram_start = bss_sram;

    /* Second pass - assign .bss (uninitialized/zero) globals after .data */
    for (gi = 0; gi < prog->global_count && g_gvar_count < ARM_MAX_GVARS; gi++) {
        IrGlobal *g = prog->globals[gi];
        int sz;
        if (g->is_extern) continue;
        if (g->has_at) {
            /* __at variables: add to lookup table with their absolute address */
            g_gvar_names[g_gvar_count] = g->name;
            g_gvar_addrs[g_gvar_count] = g->at_addr;
            g_gvar_sizes[g_gvar_count] = g->type ? type_size(g->type) : 4;  /* #6 FIX */
            g_gvar_count++;
            continue;
        }
        if (g->has_init) {
            /* Non-zero initialized globals were assigned in .data pass above.
             * Zero-initialized scalars (init_int==0, no aggregate/str/float) were
             * skipped in .data and should be assigned here in .bss. */
            if (!(g->init_aggregate || g->init_str || g->is_float) && g->init_int == 0) {
                /* zero-initialized scalar: assign in .bss */
            } else {
                continue;  /* already assigned in .data */
            }
        }
        sz = g->type ? type_size(g->type) : 4;
        if (sz <= 0) sz = 4;
        /* SCT-1/SCT-3: custom section globals get their address from the
         * scatter exec region rather than the default .bss pool. */
        if (g->section && g_scatter_loaded) {
            unsigned caddr = custom_sect_alloc(g->section, sz);
            if (caddr) {
                g_gvar_names[g_gvar_count] = g->name;
                g_gvar_addrs[g_gvar_count] = caddr;
                g_gvar_sizes[g_gvar_count] = sz;
                g_gvar_count++;
                continue;
            }
            /* section not found in scatter: fall through to .bss allocation */
        }
        {
            unsigned req = (g->align_bytes > 4) ? (unsigned)g->align_bytes : 4u;
            unsigned rem = bss_sram % req;
            if (rem) bss_sram += req - rem;
        }
        g_gvar_names[g_gvar_count] = g->name;
        g_gvar_addrs[g_gvar_count] = bss_sram;
        g_gvar_sizes[g_gvar_count] = sz;  /* #6 FIX */
        g_gvar_count++;
        bss_sram += sz;
    }
    /* #BSS-ALIGN FIX: Align .bss end to 4 bytes. The __arkcc_init_globals
     * zeroing loop increments r0 by 4 and terminates with CMP r0, r1; BNE.
     * If the last .bss variable's size is not a multiple of 4 (e.g. a char
     * or uint8_t array), r0 would never equal r1, causing an infinite loop
     * that writes zeros past .bss into adjacent memory or HardFault. */
    g_bss_sram_end = (bss_sram + 3) & ~3u;

    /* Scatter overflow check: warn (do not fail) if .data/.bss grew past their
     * declared execution-region size. Helps catch layout mistakes early. */
#if ARKCC_FEATURE_SCATTER
    if (g_scatter_loaded) {
        ScatterExecRegion *rw_er = scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_RW);
        ScatterExecRegion *zi_er = scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_ZI);
        if (rw_er && !scatter_er_fits(rw_er, g_data_sram_start,
                                      g_data_sram_end - g_data_sram_start))
            ark_log("[scatter] warning: .data (0x%X..0x%X) overflows %s (size 0x%X)\n",
                    g_data_sram_start, g_data_sram_end, rw_er->name, rw_er->size);
        if (zi_er && !scatter_er_fits(zi_er, g_bss_sram_start,
                                      g_bss_sram_end - g_bss_sram_start))
            ark_log("[scatter] warning: .bss (0x%X..0x%X) overflows %s (size 0x%X)\n",
                    g_bss_sram_start, g_bss_sram_end, zi_er->name, zi_er->size);
    }
#endif
}

#if ARKCC_FEATURE_RT_LAZY
/* 运行库按需发射（--rt-lazy）。
 *
 * 问题：每个翻译单元都会把全部 __aeabi_* 辅助函数打进产物，于是一个 56 字节的
 * 源文件也能编出 1578 字节代码，其中绝大多数是永远调用不到的 64 位与浮点辅助。
 * 链接器虽然能去重，但单个对象本身的体积仍然虚高。
 *
 * 做法：在汇编文本层面把每个辅助函数整块切下来，从"用户代码 + 已被判定需要的
 * 辅助函数"出发求一次可达闭包（辅助函数之间也会互相调用，例如
 * __aeabi_ldivmod 调 __aeabi_uldivmod），只保留可达的块。
 *
 * 为什么在文本层做而不是在 IR 层做：辅助函数是后端直接写死的汇编文本，没有对应
 * 的 IR 指令；在文本层按名字求闭包，判断依据只有一处（名字是否被引用），不会
 * 出现"IR 认为不需要、后端却发了 BL"的分裂。
 *
 * 安全性：只删「整块、且从未被保留代码引用」的辅助函数；名字匹配带前后边界，
 * 避免 __aeabi_ladd 误命中 __aeabi_laddx 这类前缀相同的情况。
 */
#define RT_MAX_BLOCKS 80

typedef struct {
    int         start;
    int         end;
    const char *name;
    int         name_len;
    int         used;
} RtBlock;

static int rt_is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int rt_refs_name(const char *s, int from, int to, const char *name, int nl) {
    int i;
    for (i = from; i + nl <= to; i++) {
        if (s[i] != name[0]) continue;
        if (memcmp(s + i, name, (unsigned)nl) != 0) continue;
        if (i > from && rt_is_ident_char(s[i - 1])) continue;
        if (i + nl < to && rt_is_ident_char(s[i + nl])) continue;
        return 1;
    }
    return 0;
}

static int rt_prune_asm_text(char *text, int len) {
    RtBlock blk[RT_MAX_BLOCKS];
    int n = 0, i, changed = 1;
    int user_end;

    /* 1. 切块：块首是行首的 ".global __aeabi_"，块尾是下一个行首的 ".global "
     *    或文件末尾。用 ".global " 而不是 ".global __aeabi_" 当结束标志，
     *    是因为最后一个 __aeabi_ 块之后可能紧跟 __arkcc_* 辅助函数
     *    （例如浮点比较用的 __arkcc_fcmp3），那些不属于可删范围。 */
    for (i = 0; i + 16 < len; i++) {
        if (i != 0 && text[i - 1] != '\n') continue;
        if (memcmp(text + i, ".global __aeabi_", 16) != 0) continue;
        {
            int ns = i + 8;
            int e = ns;
            while (e < len && text[e] != '\n' && text[e] != '\r') e++;
            blk[n].start = i;
            blk[n].end = len;
            blk[n].name = text + ns;
            blk[n].name_len = e - ns;
            blk[n].used = 0;
            n++;
            if (n >= RT_MAX_BLOCKS) break;
        }
    }
    if (n == 0) return len;

    for (i = 0; i + 1 < n; i++) blk[i].end = blk[i + 1].start;

    /* 2. 用户区（第一个辅助块之前）里出现的名字就是种子 */
    user_end = blk[0].start;
    for (i = 0; i < n; i++) {
        if (rt_refs_name(text, 0, user_end, blk[i].name, blk[i].name_len)) blk[i].used = 1;
    }

    /* 3. 可达闭包 */
    while (changed) {
        changed = 0;
        for (i = 0; i < n; i++) {
            int j;
            if (!blk[i].used) continue;
            for (j = 0; j < n; j++) {
                if (blk[j].used) continue;
                if (rt_refs_name(text, blk[i].start, blk[i].end, blk[j].name, blk[j].name_len)) {
                    blk[j].used = 1;
                    changed = 1;
                }
            }
        }
    }

    /* 4. 原地压实 */
    {
        int w = 0;
        int r = 0;
        for (i = 0; i < n; i++) {
            if (blk[i].start > r) {
                int gap = blk[i].start - r;
                if (w != r) memmove(text + w, text + r, (unsigned)gap);
                w += gap;
            }
            if (blk[i].used) {
                int sz = blk[i].end - blk[i].start;
                if (w != blk[i].start) memmove(text + w, text + blk[i].start, (unsigned)sz);
                w += sz;
            }
            r = blk[i].end;
        }
        if (len > r) {
            int tail = len - r;
            if (w != r) memmove(text + w, text + r, (unsigned)tail);
            w += tail;
        }
        len = w;
        text[len] = 0;
    }
    return len;
}
#endif /* ARKCC_FEATURE_RT_LAZY */

/* Does a value of this type come back in two core registers (r0:r1)?
 *
 * long long / double always do.  A composite of 5..8 bytes does too: AAPCS
 * puts a composite that fits in two words into r0:r1, and the caller reads it
 * back with IR_STORE_IDX/8.  A smaller composite comes back in r0 alone.
 * Before this the ARM backend only ever pushed r0 back for a struct return, so
 * the caller stored the struct's first word and the second word came from
 * whatever was left on the stack (measured: `struct P add(...)` assigned from
 * `c = add(a, b)` answered 2 instead of 10, and with a struct destination
 * address on the stack it wrote to an unmapped address instead). */
static int ret_is_two_words(Type *t) {
    if (!t) return 0;
    if (t->kind == TYPE_LLONG || t->kind == TYPE_DOUBLE) return 1;
    if (t->kind == TYPE_STRUCT || t->kind == TYPE_UNION) {
        int sz = type_size(t);
        return sz > 4 && sz <= 8;
    }
    return 0;
}

/* ---- va region ----------------------------------------------------------
 * One word per argument slot at frame + ARK_VA_BASE + 4*k, contiguous.
 *
 * irgen asks for the same region with a *stride of 8* (frame+8000+8k), which
 * is the C backend's convention: there one slot holds a whole `long long`
 * per argument (ordinal-indexed), so 8 bytes per slot is what stdarg.h walks.
 * This backend instead uses word-indexed contiguous slots, because AAPCS
 * already numbers arguments in 4-byte words (a 64-bit argument takes two
 * adjacent words) -- so a plain 32-bit walk reproduces the caller's layout
 * exactly, and a 64-bit read is just `*(long long*)ap` over two adjacent
 * words.  <stdarg.h> picks the matching convention through
 * __ARK_VA_WORD_SLOTS, which the driver defines for this target.
 *
 * The IR keeps the C-backend stride; the mapping to 4-byte stride happens in
 * codegen_arm(), in one place (the IR_GETARG case ignores these requests
 * entirely: see the "variadic region" branch there). */
/* Variadic region (ARM Thumb-2).
 *
 * The region is *the caller's own argument image*, not a copy of it: the four
 * register words are pushed at function entry (PUSH {r0-r3}) and the stack
 * words are already where the caller put them, immediately above.  So the
 * region is the 16 bytes at frame + local_size + 16, and word 4+k is the
 * k-th stack argument -- contiguous, nothing is copied, and there is no bound
 * on how many stack arguments a call may pass.
 *
 * The cost is 16 bytes of stack per variadic invocation, not 8 KB: the first
 * version of this mirrored 32 words into the frame, which put an 8 KB frame in
 * front of a printf-like function -- larger than the whole mini-app task stack
 * on the board (4 KB, APP_TASK_STACK_SIZE).  Measured: it could not run there.
 *
 * ARK_VA_BASE stays as the marker irgen uses for the C backend's 8-byte-slot
 * region requests (IR_GETARG with dst_off >= 8000); this backend ignores them
 * because it builds its own region. */

/* Set once per function (the spill runs in the prologue). */
static int g_va_named_words; /* words used by the named parameters     */
static int g_va_nsaa;        /* size of the named stack block, bytes   */
static int g_va_ncrn;        /* next core register after the named args */
static int g_va_ptr_off;     /* r6 + this = first variadic word        */

/* Materialise `frame + g_va_ptr_off` in `reg`. The offset is past 4095, so it
 * cannot be an ADD immediate; MOVW+ADD is two instructions and needs no
 * literal pool. Used for _ark_va_ptr, which stdarg.h reads as a variable. */
static void emit_va_ptr(AsmBuffer *b, int reg) {
    asmbuf_puts(b, "    MOVW r");
    asmbuf_int(b, reg);
    asmbuf_puts(b, ", #");
    asmbuf_int(b, g_va_ptr_off);
    asmbuf_puts(b, "\n    ADD r");
    asmbuf_int(b, reg);
    asmbuf_puts(b, ", r6, r");
    asmbuf_int(b, reg);
    asmbuf_putc(b, '\n');
}

/* _ark_va_ptr is how stdarg.h reaches the va region: it is declared extern
 * there and this backend answers it with a frame address instead of a real
 * global (the region lives in the caller's frame, one per invocation).
 * Same shape as the x86 backend's special case; must be handled in BOTH
 * codegen paths (--reg-top peephole and the main switch), otherwise the
 * output would differ depending on the optimisation level. */
static int is_va_ptr_sym(const char *name) {
    return (name && ark_streq(name, "_ark_va_ptr"));
}

/* AAPCS32 parameter assignment (base standard: core registers then stack).
 *
 * sizes[i] is how many bytes argument i occupies on the wire: 4 for one word,
 * 8 for a double-word scalar or composite (long long / double / a 5..8-byte
 * struct), and anything above 8 for a composite that travels inline through a
 * variadic call (see irgen: named parameters of that size go as one pointer
 * word instead).  Outputs are indexed by cumulative *slot* (one slot = one word),
 * which is what irgen stamps into IR_GETARG:
 *   reg[slot]  = r0..r3 for the word held in a register, -1 if it is on the stack
 *   soff[slot] = byte offset inside the caller's stack argument block for a
 *                stacked word, -1 if it is in a register
 * Returns the size of that block in bytes (NSAA), before the caller rounds it
 * so that sp is 8-byte aligned at the call.
 *
 * The rules (AAPCS32 5.5, base standard):
 *   * a double-word argument must start in an even-numbered core register and
 *     needs two of them; if there is no such pair left it goes on the stack,
 *     and NCRN is set to 4 (no further argument goes in a register);
 *   * a double-word argument on the stack is aligned to 8 bytes;
 *   * a single-word argument goes to the next core register while NCRN < 4,
 *     otherwise to the next word of the stack block.
 *
 * Caller and callee MUST agree, so both sides call this.  They used to keep
 * two hand-written versions of the same rule; the caller laid 64-bit stack
 * arguments out high-word-first while the callee read them low-word-first, so
 * `f(1LL, 2LL, 3LL)` summed the wrong halves (measured in unicorn). */
static int aapcs_assign(const int *sizes, int n, int ncrn_start,
                        int *reg, int *soff) {
    int ncrn = ncrn_start;
    int nsaa = 0;
    int slot = 0;
    int i;
    for (i = 0; i < n; i++) {
        if (sizes[i] > 8) {
            /* More than two words: no all-or-nothing rule applies, the words
             * fill the remaining core register(s) and then the stack in order.
             * Measured against arm-none-eabi-gcc 12.3 -- `f(1, s12)` with three
             * named words ahead of it puts word 0 in r3 and words 1,2 on the
             * stack.  No alignment hole is inserted: nothing here can make
             * <stdarg.h> round up (it decides by sizeof, and a hole would only
             * be visible to a reader that knows the type's alignment). */
            int w = (sizes[i] + 3) / 4;
            int j;
            for (j = 0; j < w; j++) {
                if (ncrn < 4) {
                    reg[slot] = ncrn++;
                    soff[slot] = -1;
                } else {
                    reg[slot] = -1;
                    soff[slot] = nsaa;
                    nsaa += 4;
                }
                slot += 1;
            }
        } else if (sizes[i] == 8) {
            int r = ncrn;
            if (r & 1) r++;                     /* start on an even register */
            if (r <= 2) {
                reg[slot] = r;
                reg[slot + 1] = r + 1;
                soff[slot] = -1;
                soff[slot + 1] = -1;
                ncrn = r + 2;
            } else {
                ncrn = 4;                       /* nothing else in a register */
                nsaa = (nsaa + 7) & ~7;         /* 8-byte aligned on the stack */
                reg[slot] = -1;
                reg[slot + 1] = -1;
                soff[slot] = nsaa;
                soff[slot + 1] = nsaa + 4;
                nsaa += 8;
            }
            slot += 2;
        } else {
            if (ncrn < 4) {
                reg[slot] = ncrn++;
                soff[slot] = -1;
            } else {
                reg[slot] = -1;
                soff[slot] = nsaa;
                nsaa += 4;
            }
            slot += 1;
        }
    }
    return nsaa;
}

/* One place for "a C string as .asciz text": the string pool and the
 * pointer-global initialiser both need it. */
static void arm_put_escaped_str(AsmBuffer *b, const char *s) {
    int k;
    for (k = 0; s[k]; k++) {
        unsigned char c = (unsigned char)s[k];
        if (c == '\n') asmbuf_puts(b, "\\n");
        else if (c == '\t') asmbuf_puts(b, "\\t");
        else if (c == '\r') asmbuf_puts(b, "\\r");
        else if (c == '\\') asmbuf_puts(b, "\\\\");
        else if (c == '"')  asmbuf_puts(b, "\\\"");
        else if (c < 0x20 || c >= 0x7F) {
            char hb[8];
            const char *hh = "0123456789abcdef";
            hb[0] = '\\'; hb[1] = 'x';
            hb[2] = hh[(c >> 4) & 0xF]; hb[3] = hh[c & 0xF]; hb[4] = 0;
            asmbuf_puts(b, hb);
        } else asmbuf_putc(b, (char)c);
    }
}

static char *codegen_arm(IrProgram *prog) {
    AsmBuffer buf;
    int fi;
#if ARKCC_FEATURE_DWARF
    int dbg = (g_config.debug_info || g_config.enable_dwarf) ? 1 : 0;
#else
    int dbg = 0;
#endif
    asmbuf_init(&buf);

    /* Load the scatter description (if --scatter was given) so the base
     * addresses for Flash/.data/.bss come from the .sct layout. Must happen
     * before gvar_assign, which places globals at g_data_base/g_bss_base. */
    arm_scatter_load();

    /* Assign SRAM addresses to all non-extern, non-__at globals.
     * This must happen before generating function code so IR_PUSH_GLOBAL etc.
     * can emit MOVW/MOVT with the correct SRAM address. */
    gvar_assign(prog);

    asmbuf_puts(&buf, "; Ark C Compiler - generated Thumb-2 assembly\n");
    asmbuf_puts(&buf, ".thumb\n");
    asmbuf_puts(&buf, ".text\n\n");

#if ARKCC_FEATURE_DWARF
    /* DWARF: declare the source file for later .loc 1 <line> references */
    if (dbg && prog->source_file) {
        asmbuf_puts(&buf, ".file 1 \"");
        asmbuf_puts(&buf, prog->source_file);
        asmbuf_puts(&buf, "\"\n");
    }
#endif

    /* String literal section */
    if (prog->string_count > 0) {
        int i;
        asmbuf_puts(&buf, ".section .rodata\n");
        for (i = 0; i < prog->string_count; i++) {
            IrString *s = prog->strings[i];
            asmbuf_puts(&buf, ".L_str_");
            asmbuf_int(&buf, s->id);
            asmbuf_puts(&buf, ":\n    .asciz \"");
            {
                int k;
                for (k = 0; k < s->len; k++) {
                    char c = s->data[k];
                    if (c == '\n') asmbuf_puts(&buf, "\\n");
                    else if (c == '\t') asmbuf_puts(&buf, "\\t");
                    else if (c == '\r') asmbuf_puts(&buf, "\\r");
                    else if (c == '\\') asmbuf_puts(&buf, "\\\\");
                    else if (c == '"')  asmbuf_puts(&buf, "\\\"");
                    else if (c == '\b') asmbuf_puts(&buf, "\\b");
                    else if (c == '\f') asmbuf_puts(&buf, "\\f");
                    else if (c == '\v') asmbuf_puts(&buf, "\\v");
                    else if (c == '\a') asmbuf_puts(&buf, "\\a");
                    else if (c == '\0') asmbuf_puts(&buf, "\\0");
                    else if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) {
                        char hb[8]; int hn = 0;
                        const char *hh = "0123456789abcdef";
                        hb[hn++] = '\\'; hb[hn++] = 'x';
                        hb[hn++] = hh[((unsigned char)c >> 4) & 0xF];
                        hb[hn++] = hh[(unsigned char)c & 0xF];
                        hb[hn] = 0;
                        asmbuf_puts(&buf, hb);
                    }
                    else asmbuf_putc(&buf, c);
                }
            }
            asmbuf_puts(&buf, "\"\n");
        }
        asmbuf_puts(&buf, ".text\n\n");
    }

    /* A pointer global can be initialised from a string literal
     * (`char *p = "hi";`).  That string is not in the pool -- the pool
     * is filled while emitting expressions -- and it is not a character
     * array either, so nothing used to emit it: the runtime initialiser
     * then wrote the bytes 'h','i' *into the pointer object* and left
     * the pointer itself uninitialised, i.e. `p` was a wild pointer.
     * Give each one a flash label and store its address instead. */
    {
        int gi, any = 0;
        for (gi = 0; gi < prog->global_count; gi++) {
            IrGlobal *g = prog->globals[gi];
            if (g->is_extern || !g->has_init || !g->init_str) continue;
            if (!g->type || g->type->kind != TYPE_PTR) continue;
            if (!any) { asmbuf_puts(&buf, ".section .rodata\n"); any = 1; }
            asmbuf_puts(&buf, ".L_gstr_");
            asmbuf_int(&buf, gi);
            asmbuf_puts(&buf, ":\n    .asciz \"");
            arm_put_escaped_str(&buf, g->init_str);
            asmbuf_puts(&buf, "\"\n");
        }
        if (any) asmbuf_puts(&buf, ".text\n\n");
    }

    /* Floating-point constant section: emit both single-precision (.L_flt_<i>) and double-precision (.L_dbl_<i>) bit patterns.
     * The high bit 0x200 of IR_PUSH_FLOAT aux determines which to load:
     *   - 0x200 not set: single-precision, 4 bytes, uses __aeabi_f* family
     *   - 0x200 set: double-precision, 8 bytes, uses __aeabi_d* family
     * Stored uniformly as double; the single-precision version is truncated via (float). */
    if (prog->float_count > 0) {
        int i;
        asmbuf_puts(&buf, ".section .rodata\n");
        for (i = 0; i < prog->float_count; i++) {
            union { float f; unsigned int u; } u;
            union { double d; unsigned int u[2]; } du;
            unsigned int bits;
            char hex[9];
            int k;
            const char *hd = "0123456789abcdef";
            /* Single-precision: float bit pattern (4 bytes) */
            u.f = (float)prog->floats[i];
            bits = u.u;
            for (k = 7; k >= 0; k--) { hex[k] = hd[bits & 0xF]; bits >>= 4; }
            hex[8] = 0;
            asmbuf_puts(&buf, ".L_flt_");
            asmbuf_int(&buf, i);
            asmbuf_puts(&buf, ":\n    .word 0x");
            asmbuf_puts(&buf, hex);
            asmbuf_putc(&buf, '\n');
            /* Double-precision: double bit pattern (8 bytes, little-endian: low word first) */
            du.d = prog->floats[i];
            asmbuf_puts(&buf, ".L_dbl_");
            asmbuf_int(&buf, i);
            asmbuf_puts(&buf, ":\n    .word 0x");
            {
                unsigned int lo = du.u[0];
                char hex2[9]; int k2;
                for (k2 = 7; k2 >= 0; k2--) { hex2[k2] = hd[lo & 0xF]; lo >>= 4; }
                hex2[8] = 0;
                asmbuf_puts(&buf, hex2);
            }
            asmbuf_puts(&buf, "\n    .word 0x");
            {
                unsigned int hi = du.u[1];
                char hex2[9]; int k2;
                for (k2 = 7; k2 >= 0; k2--) { hex2[k2] = hd[hi & 0xF]; hi >>= 4; }
                hex2[8] = 0;
                asmbuf_puts(&buf, hex2);
            }
            asmbuf_putc(&buf, '\n');
        }
        asmbuf_puts(&buf, ".text\n\n");
    }

    /* Build sret function name/size set: struct/union returns > 8 bytes
     * use AAPCS sret convention (hidden pointer in r0). */
    char *sret_names[ARK_MAX_SRET_FUNCS];
    int   sret_sizes[ARK_MAX_SRET_FUNCS];
    int   sret_count = 0;
    int   sret_dropped = 0;
    {
        int sfi;
        for (sfi = 0; sfi < prog->func_count; sfi++) {
            IrFunc *sf = prog->funcs[sfi];
            Type *srt = sf->ret_type;
            if (srt && (srt->kind == TYPE_STRUCT || srt->kind == TYPE_UNION)) {
                int sz = type_size(srt);
                if (sz > 8) {
                    /* A function missing from this set is called with the
                     * AAPCS default (no hidden pointer) while its body expects
                     * one: r0 then carries the first argument instead. */
                    if (sret_count >= ARK_MAX_SRET_FUNCS) { sret_dropped++; continue; }
                    sret_names[sret_count] = sf->name;
                    sret_sizes[sret_count] = (sz + 3) & ~3; /* 4-byte aligned */
                    sret_count++;
                }
            }
        }
    }
    if (sret_dropped) {
        char b1[12], b2[12];
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "%s functions return a structure larger than 8 bytes but the "
                  "sret table holds %s; raise ARK_MAX_SRET_FUNCS and rebuild arkcc",
                  diag_num(b1, sret_dropped), diag_num(b2, ARK_MAX_SRET_FUNCS),
                  0,0,0,0);
    }

    for (fi = 0; fi < prog->func_count; fi++) {
        IrFunc *f = prog->funcs[fi];
        /* Names the function whose code is being emitted, for the diagnostics
         * that are raised deep inside the instruction switch (a reference to
         * _ark_va_ptr outside a variadic function, for instance). */
        g_cur_fn_name = f->name;

        /* Variadic functions: the va region is spilled below, once per
         * function, right after the prologue (see "va region" further down).
         * Nothing to do here. */
        int local_size = f->local_size;
        int i;
        /* Variadic functions need no frame space for the va region: the four
         * register words are pushed in the prologue and the stack words stay
         * where the caller put them (see emit_prologue).  irgen does not
         * reserve the C backend's 8 KB slot area for this target either
         * (g_ir_va_slot_region), so a variadic frame is just its locals. */
        int is_sret = 0;
        int fstart = 0;              /* where this function's text begins */
        int sret_off = 0;
        int va_start_slot = 0;       /* first variadic word, in region words */
        int va_lo = 0;               /* first saved argument register (even)   */
        /* P1 FIX (sret caller side): pre-allocate sret buffers in this
         * function's frame so the sret space doesn't occupy SP+0 (where
         * the callee expects to read stack args). Each sret call site gets
         * its own buffer at a known r6-relative offset. */
        int sret_call_buf_off[64];
        int sret_call_n = 0;

        /* Detect sret: struct/union return > 8 bytes */
        {
            Type *rt = f->ret_type;
            if (rt && (rt->kind == TYPE_STRUCT || rt->kind == TYPE_UNION) &&
                type_size(rt) > 8) {
                is_sret = 1;
                local_size += 4;  /* slot for sret pointer */
                sret_off = local_size - 4;
            }
        }

        /* alias function: emit .global / .set directives, do not generate a function body */
        if (f->alias_target) {
            asmbuf_puts(&buf, ".global ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
            asmbuf_puts(&buf, ".set ");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", ");
            asmbuf_puts(&buf, f->alias_target);
            asmbuf_puts(&buf, "\n\n");
            continue;
        }

        /* --split_sections/-ffunction-sections: each function gets its own section .text.<name>,
         * so the linker can dead-strip unused code (--gc-sections). alias functions do not generate a section (only .set). */
        if (g_config.split_sections && !f->alias_target) {
            asmbuf_puts(&buf, ".section .text.");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", \"ax\", %progbits\n");
        }
        asmbuf_puts(&buf, ".global ");
        asmbuf_puts(&buf, f->name);
        asmbuf_putc(&buf, '\n');
        if (f->is_weak) {
            /* __weak: declare weak symbol before the label, allows override by a same-name strong symbol at link time */
            asmbuf_puts(&buf, ".weak ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
        }
        if (f->is_used) {
            /* __attribute__((used)): comment marker only; .global already ensures symbol export */
            asmbuf_puts(&buf, "@ __attribute__((used))\n");
        }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
        /* armcc 5 compatibility function modifiers: comment markers only (ARM backend does not change calling convention).
         *   __value_in_regs / __pure / __interwork / __smc(n) / __global_reg(n) */
        if (f->is_value_in_regs) asmbuf_puts(&buf, "@ __value_in_regs\n");
        if (f->is_pure)          asmbuf_puts(&buf, "@ __pure\n");
        if (f->is_const_attr)    asmbuf_puts(&buf, "@ __attribute__((const))\n");
        if (f->is_interwork)     asmbuf_puts(&buf, "@ __interwork\n");
        if (f->smc_num > 0) {
            asmbuf_puts(&buf, "@ __smc(");
            asmbuf_int(&buf, f->smc_num);
            asmbuf_puts(&buf, ")\n");
        }
        if (f->global_reg >= 0) {
            asmbuf_puts(&buf, "@ __global_reg(");
            asmbuf_int(&buf, f->global_reg);
            asmbuf_puts(&buf, ")\n");
        }
#endif
        /* __asm whole-function body: emit label and user's raw assembly, skip prologue/epilogue */
        if (f->is_asm_func) {
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ":\n");
            if (f->asm_body) {
                const char *s = f->asm_body;
                /* Copy text line by line; add leading indent for GAS friendliness */
                int line_start = 1;
                while (*s) {
                    if (line_start) {
                        /* Skip leading whitespace so we can re-indent */
                        while (*s == ' ' || *s == '\t' || *s == '\r') s++;
                        if (*s == 0) break;
                        if (*s == '\n') { asmbuf_putc(&buf, '\n'); s++; continue; }
                        asmbuf_puts(&buf, "    ");
                        line_start = 0;
                    }
                    if (*s == '\n') { asmbuf_putc(&buf, '\n'); line_start = 1; s++; continue; }
                    asmbuf_putc(&buf, *s);
                    s++;
                }
                if (!line_start) asmbuf_putc(&buf, '\n');
            }
            asmbuf_putc(&buf, '\n');
            continue;
        }
#if ARKCC_FEATURE_DWARF
        if (dbg) {
            asmbuf_puts(&buf, ".type ");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", %function\n");
        }
#endif
        /* Pre-scan IR for sret calls: reserve a buffer in the frame for each.
         * The buffer is addressed via r6 (frame pointer) at the call site,
         * so SP stays at the stack args (callee reads them at SP+0). */
        {
            int sci;
            for (sci = 0; sci < f->code_count && sret_call_n < 64; sci++) {
                IrInst *sci_I = &f->code[sci];
                if (sci_I->op == IR_CALL && sci_I->name && sci_I->name[0] != '?') {
                    if (sret_lookup(sret_names, sret_sizes, sret_count, sci_I->name)) {
                        int sz = sret_get_size(sret_names, sret_sizes, sret_count, sci_I->name);
                        local_size += sz;
                        sret_call_buf_off[sret_call_n] = local_size - sz;
                        sret_call_n++;
                    }
                }
            }
        }
        /* AAPCS: SP must be 8-byte aligned at BL. irgen aligns f->local_size
         * to 8, but the sret adjustments above (sret pointer slot +4, sret call
         * buffers +sz) can leave local_size at 4 mod 8. Round up to 8 so the
         * prologue's SUB sp,sp,#local_size preserves the 8-byte alignment that
         * IR_CALL's need_align parity check relies on. The padding sits at the
         * top of the frame (between the last variable and saved regs); all
         * r6-relative offsets remain valid because the prologue SUB and the
         * stack-arg offset computation both use this padded value. */
        local_size = (local_size + 7) & ~7;
        /* NCRN / nsaa have to be known before the prologue, because the
         * prologue decides how many argument words to save.  This block used to
         * sit in the body loop; nothing in it emits code, so it simply moved
         * up. */
        g_va_epilogue_pad = 0;
        va_lo = 0;
        static int aapcs_map[64];
        static int aapcs_soff[64];
        {
            /* Slot -> register (aapcs_map) and slot -> byte offset in the
             * caller's stack argument block (aapcs_soff), from the one shared
             * rule in aapcs_assign() so the callee reads exactly what the
             * caller wrote. */
            static int psz[64];
            int pj;
            for (pj = 0; pj < 64; pj++) { aapcs_map[pj] = -1; aapcs_soff[pj] = -1; }
            for (pj = 0; pj < f->param_count && pj < 64; pj++) {
                Type *pt = (f->param_types && pj < f->param_count)
                           ? f->param_types[pj] : 0;
                int sz = pt ? type_size(pt) : 4;
                if (sz <= 0) sz = 4;
                /* A struct passed by value travels as a pointer word. */
                if (pt && (pt->kind == TYPE_STRUCT || pt->kind == TYPE_UNION) && sz > 8)
                    sz = 4;
                psz[pj] = sz;
            }
            g_va_nsaa = aapcs_assign(psz, (f->param_count < 64) ? f->param_count : 64,
                                    is_sret ? 1 : 0, aapcs_map, aapcs_soff);
            /* Words used by the named parameters, and where the variadic
             * words start.
             *
             * The start is NOT "the number of named words": AAPCS continues
             * the argument numbering in the next core register, and any named
             * argument on the stack forces NCRN to 4 (everything after it is
             * stacked too). f(int a, long long b, ...) is the case that
             * separates the two: b needs an even register pair, r1 stays
             * unused, NCRN is 4, and a "named words" pointer would hand the
             * first va_arg r3 -- a named argument. Both numbers come out of
             * aapcs_assign(): register -1 marks a stacked named word, and the
             * highest register used plus one is NCRN.
             *
             * The variadic stack words begin at 4 + nsaa/4 in the flattened
             * region (see the spill below). */
            {
                int nw = 0, pj2, any_stack = 0, maxreg = -1;
                for (pj2 = 0; pj2 < f->param_count && pj2 < 64; pj2++)
                    nw += (psz[pj2] == 8) ? 2 : 1;
                for (pj2 = 0; pj2 < nw && pj2 < 64; pj2++) {
                    if (aapcs_map[pj2] < 0) any_stack = 1;
                    else if (aapcs_map[pj2] > maxreg) maxreg = aapcs_map[pj2];
                }
                g_va_named_words = nw;
                g_va_ncrn = any_stack ? 4 : maxreg + 1;
                if (is_sret && g_va_ncrn < 1) g_va_ncrn = 1;  /* r0 = sret */
                /* Only the register words that can still hold a variadic
                 * argument are saved: r_k .. r3 with k = NCRN rounded DOWN to
                 * an even number.  Four things ride on choosing k that way:
                 *
                 *   - the region stays the caller's argument image verbatim:
                 *     its base is always frame + local_size + 16, whatever k
                 *     is, because sp_entry = base + pushed bytes.  Zero copy,
                 *     and the caller's stack arguments stay contiguous with
                 *     the last pushed word, so there is still no limit on how
                 *     many arguments a variadic call may pass on the stack;
                 *   - "round ap up to 8" stays equivalent to "even AAPCS
                 *     argument word" for any k: word j sits at base + 4j, and
                 *     that is 8-aligned iff (j-k) is even, which is exactly
                 *     "k+j, the AAPCS word number, is even";
                 *   - k even makes the pushed byte count a multiple of 8, so
                 *     sp keeps the 8-byte alignment AAPCS requires.  An odd k
                 *     would still walk the region correctly but would leave
                 *     the frame at 4 mod 8, where an 8-byte local (or a
                 *     GCC-compiled callee) can fault;
                 *   - k == 4 pushes nothing: the epilogue goes back to a plain
                 *     POP {r5, r6, r7, pc} and the stack-argument base back to
                 *     local_size + 16.
                 *
                 * Measured cost per signature: int f(int a, ...) 16 B,
                 * int f(int a, int b, ...) 8 B, four named words 0 B. */
                if (f->is_variadic && !f->is_naked) {
                    va_lo = g_va_ncrn & ~1;
                    if (va_lo > 4) va_lo = 4;
                    g_va_epilogue_pad = 4 * (4 - va_lo);
                    va_start_slot = (g_va_ncrn >= 4) ? (g_va_nsaa / 4)
                                                     : (g_va_ncrn - va_lo);
                }
            }
        }
        fstart = buf.len;
        emit_prologue(&buf, f->name, local_size, va_lo, g_va_epilogue_pad,
                      f->is_irq, f->is_naked, dbg);
        /* #2 FIX: Auto-call __arkcc_init_globals at the start of Reset_Handler
         * to initialize .data and zero .bss before main() runs. */
        if (ark_streq(f->name, "Reset_Handler") &&
            (g_data_sram_start != g_data_sram_end ||
             g_bss_sram_start != g_bss_sram_end)) {
            asmbuf_puts(&buf, "    BL __arkcc_init_globals\n");
        }
        /* Save sret pointer (r0) to frame slot for struct-returning functions */
        if (is_sret) {
            asmbuf_puts(&buf, "    STR r0, [r6, #");
            asmbuf_int(&buf, sret_off);
            asmbuf_puts(&buf, "]\n");
        }

        {
        int cur_line = 0;
        int sret_call_idx = 0;
        /* Precompute AAPCS register assignment for each param slot.
         * Maps slot index (cumulative 4-byte slot count) to AAPCS register
         * number (r0-r3), accounting for 64-bit even-pair alignment.
         * -1 = slot not used (gap in struct-by-value slot space). */
        /* static: this compiler also runs on the target board, where the
         * code generator's stack is a tight budget.  codegen_arm() is not
         * recursive and is re-entered only once per function, and both tables
         * are re-initialised at the top of the loop, so a file-scope lifetime
         * changes nothing except the stack cost. */

        /* ---- va region (variadic functions only) ------------------------
         * The region IS the caller's argument image: the prologue pushed
         * r0-r3 at [frame + local_size + 16 .. +31] (r0 lowest), and the
         * caller's stack argument block starts at frame + local_size + 32.
         * So region word k is:
         *   k = 0..3   r0..r3
         *   k = 4+j    stack argument word j
         * which is exactly AAPCS's own numbering.  Nothing is copied here and
         * there is no size limit: <stdarg.h> walks 4-byte words and rounds a
         * 64-bit read up to an 8-byte boundary, which is the same rule AAPCS
         * uses for an even register pair and an 8-byte-aligned stack slot.
         * The base is 8-byte aligned because sp is (AAPCS: 8-byte aligned at
         * BL) and the prologue subtracted 16 and a multiple of 8.
         *
         * `_ark_va_ptr` therefore points at frame + local_size + 16 + 4*slot,
         * where slot counts AAPCS argument words (see va_start_slot). */
        g_va_active = 0;
        g_va_ptr_off = 0;
        if (f->is_variadic && !f->is_naked) {
            g_va_active = 1;
            g_va_ptr_off = local_size + 16 + 4 * va_start_slot;
        }

        /* --reg-top state: whether the IR stack top is in r0, and whether the
         * IR_RETURN being emitted can skip its pop.  Both stay 0 when the
         * option is off, so the generated code is byte-identical. */
        int rtop = 0;
        int rtop_ret = 0;
        for (i = 0; i < f->code_count; i++) {
            IrInst *I = &f->code[i];
#if ARKCC_FEATURE_DWARF
            /* DWARF: emit .loc 1 <line> when the line number changes */
            if (dbg && I->line != 0 && I->line != cur_line) {
                cur_line = I->line;
                asmbuf_puts(&buf, ".loc 1 ");
                asmbuf_int(&buf, cur_line);
                asmbuf_putc(&buf, '\n');
            }
#endif
            rtop_ret = 0;
            if (ark_top_enabled() && ark_top_try(&buf, I, &rtop, &rtop_ret))
                continue;
            switch (I->op) {
                case IR_FUNC_BEGIN:
                case IR_FUNC_END:
                    break;
                case IR_GETARG: {
                    /* AAPCS: first 4 args in r0-r3, args 5+ on caller's stack.
                     * For sret functions, r0 holds the hidden sret pointer;
                     * real args start at r1. The aapcs_map[] (precomputed above)
                     * already accounts for sret by starting at r1, so arg_idx
                     * is used directly as the aapcs_map key — NO manual shift.
                     *
                     * Stack layout (upward from r6 = frame pointer):
                     *   [r6 + 0 .. local_size-1]            this frame's local vars
                     *   [r6 + local_size + 0..15]           saved r5/r6/r7/lr
                     *   [r6 + local_size + 16 .. +31]       variadic only: r0..r3
                     *   [r6 + local_size + 16 + k*4]        the (5+k)th stack arg
                     *                                       (+32, not +16, in a
                     *                                       variadic function)
                     *
                     * aux encoding: low 8 bits = slot index, high 24 bits = type size.
                     *   st_sz > 8  => struct-by-value: reg/stack holds a *pointer*
                     *   st_sz == 8 => 64-bit scalar (long long / double): two words
                     *   st_sz <= 4 => 32-bit or smaller: one word
                     * aapcs_map[slot] gives the AAPCS register (0-3) or -1 for gaps.
                     * For stack args, register number is 4+; offset = (reg-4)*4. */
                    int arg_idx = I->aux & 0xFF;
                    int st_sz   = (I->aux >> 8) & 0xFFFFFF;
                    int dst_off = I->val;
                    int reg     = (arg_idx < 64) ? aapcs_map[arg_idx] : -1;
                    /* A variadic prologue pushed r0-r3 into the frame, so the
                     * caller's stack argument block sits 16 bytes higher. */
                    /* Stack arguments start above the saved registers and
                     * above whatever the prologue pushed for the va region. */
                    int argbase = local_size + 16 + g_va_epilogue_pad;
                    /* The variadic region is spilled once in the prologue
                     * (contiguous words, see "va region spill" above), so
                     * these IR requests -- one per argument word, stamped
                     * with the C backend's 8-byte stride -- need nothing
                     * here. Cheap on purpose: a variadic function gets eight
                     * of them, and none of them must reach the r3 scratch
                     * path below (r0-r3 are gone by this point). */
                    if (dst_off >= ARK_VA_BASE) break;
                    if (st_sz > 8) {
                        /* Struct-by-value (> 8 bytes): register/stack holds a
                         * *pointer* to the source struct. memcpy into frame[dst_off]. */
                        if (reg >= 0 && reg < 4) {
                            asmbuf_puts(&buf, "    MOV r3, r");
                            asmbuf_int(&buf, reg);
                            asmbuf_puts(&buf, "\n");
                        } else {
                            int stack_off = argbase + aapcs_soff[arg_idx];
                            asmbuf_puts(&buf, "    LDR r3, [r6, #");
                            asmbuf_int(&buf, stack_off);
                            asmbuf_puts(&buf, "]\n");
                        }
                        if (st_sz <= 16) {
                            int off;
                            for (off = 0; off < st_sz; off += 4) {
                                asmbuf_puts(&buf, "    LDR r0, [r3, #");
                                asmbuf_int(&buf, off);
                                asmbuf_puts(&buf, "]\n");
                                asmbuf_puts(&buf, "    STR r0, [r6, #");
                                asmbuf_int(&buf, dst_off + off);
                                asmbuf_puts(&buf, "]\n");
                            }
                        } else {
                            asmbuf_puts(&buf, "    ADD r0, r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "\n    MOV r1, r3\n");
                            emit_mov_imm(&buf, 2, st_sz);
                            asmbuf_puts(&buf, "    PUSH {lr}\n    BL __aeabi_memcpy\n    POP {r0}\n");
                        }
                    } else if (st_sz == 8) {
                        /* 64-bit scalar (long long / double): read two words from
                         * the even register pair (r0/r1 or r2/r3) or caller's stack. */
                        int reg_hi = (arg_idx + 1 < 64) ? aapcs_map[arg_idx + 1] : -1;
                        /* Low word */
                        if (reg >= 0 && reg < 4) {
                            asmbuf_puts(&buf, "    STR r");
                            asmbuf_int(&buf, reg);
                            asmbuf_puts(&buf, ", [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        } else {
                            int soff = argbase + aapcs_soff[arg_idx];
                            asmbuf_puts(&buf, "    LDR r0, [r6, #");
                            asmbuf_int(&buf, soff);
                            asmbuf_puts(&buf, "]\n    STR r0, [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        }
                        /* High word */
                        if (reg_hi >= 0 && reg_hi < 4) {
                            asmbuf_puts(&buf, "    STR r");
                            asmbuf_int(&buf, reg_hi);
                            asmbuf_puts(&buf, ", [r6, #");
                            asmbuf_int(&buf, dst_off + 4);
                            asmbuf_puts(&buf, "]\n");
                        } else {
                            int soff = argbase + aapcs_soff[arg_idx + 1];
                            asmbuf_puts(&buf, "    LDR r0, [r6, #");
                            asmbuf_int(&buf, soff);
                            asmbuf_puts(&buf, "]\n    STR r0, [r6, #");
                            asmbuf_int(&buf, dst_off + 4);
                            asmbuf_puts(&buf, "]\n");
                        }
                    } else {
                        /* 32-bit or smaller: one word */
                        if (reg >= 0 && reg < 4) {
                            asmbuf_puts(&buf, "    STR r");
                            asmbuf_int(&buf, reg);
                            asmbuf_puts(&buf, ", [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        } else {
                            int soff = argbase + aapcs_soff[arg_idx];
                            asmbuf_puts(&buf, "    LDR r0, [r6, #");
                            asmbuf_int(&buf, soff);
                            asmbuf_puts(&buf, "]\n    STR r0, [r6, #");
                            asmbuf_int(&buf, dst_off);
                            asmbuf_puts(&buf, "]\n");
                        }
                    }
                    break;
                }
                case IR_PUSH_INT:
                    emit_mov_imm(&buf, 0, I->val);
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_STR:
                    asmbuf_puts(&buf, "    LDR r0, =.L_str_");
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_VAR: {
                    /* aux encodes width + signedness:
                     *   1 = char (LDRSB signed / LDRB unsigned)
                     *   2 = short (LDRSH signed / LDRH unsigned)
                     *   4 or 0 = int/ptr (LDR 4 bytes)
                     *   8 = 64-bit (long long / double): load two words, push both
                     *   0x100 bit = unsigned
                     *   0x10000 bit (IR_AUX_VOLATILE) = volatile read: emit
                     *     DMB SY barriers before and after to enforce ordering. */
                    int width = I->aux & 0xFF;
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int is_volatile = (I->aux & IR_AUX_VOLATILE) != 0;
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: order prior memory ops */
                    if (width == 1) {
                        asmbuf_puts(&buf, is_unsigned
                            ? "    LDRB r0, [r6, #"
                            : "    LDRSB r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);
                    } else if (width == 2) {
                        asmbuf_puts(&buf, is_unsigned
                            ? "    LDRH r0, [r6, #"
                            : "    LDRSH r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);
                    } else if (width == 8) {
                        /* #5 FIX: 64-bit local variable read - load low and high words.
                         * Stack model: push low word first (bottom), then high word (top). */
                        asmbuf_puts(&buf, "    LDR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n    LDR r1, [r6, #");
                        asmbuf_int(&buf, I->val + 4);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);  /* push low word */
                        asmbuf_puts(&buf, "    PUSH {r1}\n");  /* push high word */
                    } else {
                        asmbuf_puts(&buf, "    LDR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        emit_push_r0(&buf);
                    }
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: order subsequent memory ops */
                    break;
                }
                case IR_PUSH_ADDR:
                    asmbuf_puts(&buf, "    ADD r0, r6, #");
                    asmbuf_int(&buf, I->val);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_GLOBAL: {
                    unsigned addr;
                    int gsz;
                    if (is_va_ptr_sym(I->name)) {
                        if (!g_va_active) va_ptr_needs_variadic();
                        emit_va_ptr(&buf, 0);
                        emit_push_r0(&buf);
                        break;
                    }
                    addr = gvar_find(I->name);
                    gsz = gvar_find_size(I->name);
                    if (gsz >= 8) {
                        /* #6 FIX: 64-bit global read - load low and high words */
                        emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                        asmbuf_puts(&buf, "    LDR r0, [r1, #0]\n");
                        asmbuf_puts(&buf, "    LDR r2, [r1, #4]\n");
                        emit_push_r0(&buf);  /* push low word */
                        asmbuf_puts(&buf, "    PUSH {r2}\n");  /* push high word */
                    } else {
                        emit_mov_addr32(&buf, 0, addr, addr_in_own_data(addr));
                        emit_line(&buf, "LDR r0, [r0]");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_TRUNC:
                    /* val = target width, aux = 1 zero-extend (unsigned).
                     * 32-bit widths need no work: the stack word already holds
                     * exactly the truncated 32-bit pattern. */
                    if (I->val == 8 || I->val == 16) {
                        char op8[8];
                        op8[0] = I->aux ? 'U' : 'S';
                        op8[1] = 'X';
                        op8[2] = 'T';
                        op8[3] = (I->val == 8) ? 'B' : 'H';
                        op8[4] = 0;
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    ");
                        asmbuf_puts(&buf, op8);
                        asmbuf_puts(&buf, " r0, r0\n");
                        emit_push_r0(&buf);
                    }
                    break;
                case IR_PUSH_FUNC:
                    /* Address of a function = its label. Use the literal-pool
                     * pseudo-instruction the string/double pools already use. */
                    asmbuf_puts(&buf, "    LDR r0, =");
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                case IR_PUSH_GLOBAL_ADDR:
                    /* Take the global symbol address only (no dereference), for array/struct decay
                     * and compound literal scenarios. */
                    {
                        unsigned addr = gvar_find(I->name);
                        emit_mov_addr32(&buf, 0, addr, addr_in_own_data(addr));
                        emit_push_r0(&buf);
                    }
                    break;
                case IR_STORE_GLOBAL: {
                    unsigned addr = gvar_find(I->name);
                    int gsz = gvar_find_size(I->name);
                    if (gsz >= 8) {
                        /* #6 FIX: 64-bit global store - pop high then low, store both */
                        asmbuf_puts(&buf, "    POP {r1}\n");  /* high word */
                        emit_pop_r0(&buf);                     /* low word */
                        emit_mov_addr32(&buf, 2, addr, addr_in_own_data(addr));
                        asmbuf_puts(&buf, "    STR r0, [r2, #0]\n");
                        asmbuf_puts(&buf, "    STR r1, [r2, #4]\n");
                    } else {
                        emit_pop_r0(&buf);
                        emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                        emit_line(&buf, "STR r0, [r1]");
                    }
                    break;
                }
                case IR_POP:
                    emit_pop_r0(&buf);
                    break;
                case IR_DUP: {
                    /* val = number of 32-bit words to duplicate (0/1 = one).
                     * A long long occupies two stack words with the high word on
                     * top, so duplicating just one word produced
                     * [low, high, high]; the 8-byte store that follows then wrote
                     * the high word into BOTH halves of the object.  Measured
                     * before the fix:
                     *     long long a; a = -7LL;  -> a == -1     (want -7)
                     *     long long a; a =  7LL;  -> a ==  0     (want  7)
                     * which in turn made every signed 64-bit division with a
                     * negative operand return garbage. */
                    int nw = I->val > 0 ? I->val : 1;
                    int i;
                    if (nw > 4) nw = 4;
                    for (i = 0; i < nw; i++) {
                        if (i == 0) {
                            emit_pop_r0(&buf);
                        } else {
                            asmbuf_puts(&buf, "    POP {r");
                            asmbuf_int(&buf, i);
                            asmbuf_puts(&buf, "}\n");
                        }
                    }
                    for (i = nw - 1; i >= 0; i--) {
                        if (i == 0) {
                            emit_push_r0(&buf);
                        } else {
                            asmbuf_puts(&buf, "    PUSH {r");
                            asmbuf_int(&buf, i);
                            asmbuf_puts(&buf, "}\n");
                        }
                    }
                    for (i = nw - 1; i >= 0; i--) {
                        if (i == 0) {
                            emit_push_r0(&buf);
                        } else {
                            asmbuf_puts(&buf, "    PUSH {r");
                            asmbuf_int(&buf, i);
                            asmbuf_puts(&buf, "}\n");
                        }
                    }
                    break;
                }
                case IR_SWAP:
                    /* Pop top two values and push them back in swapped order.
                     * r0 = top, r1 = next -> push r0, push r1 */
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, "    POP {r1}\n");
                    emit_push_r0(&buf);
                    asmbuf_puts(&buf, "    PUSH {r1}\n");
                    break;
                case IR_SEXT:
                case IR_ZEXT:
                case IR_CDECL64:
                    /* x86-cdecl-specific 64-bit arg passing conversion.
                     * ARM AAPCS passes 64-bit values in register pairs or
                     * 8-byte aligned stack slots as a unit; no conversion
                     * needed. Treat as no-op. */
                    break;
                case IR_STORE: {
                    /* aux encodes store width: 1=byte, 2=halfword, 4=word,
                     * 8=double word (two STRs), 16=complex (two 8-byte).
                     * Default (0) = 4 bytes.
                     * N1 FIX: mask off IR_AUX_VOLATILE (0x10000) bit before
                     * checking width — volatile uint32_t has aux=0x10004,
                     * which would wrongly match sw>=8 and sw>=16, causing
                     * 16-byte writes into an 8-byte slot and corrupting
                     * saved registers on the stack.
                     * N5 FIX: volatile stores emit DMB SY before (release) and
                     * after (full barrier) to enforce memory ordering. */
                    int sw = I->aux & ~IR_AUX_VOLATILE;
                    int is_volatile = (I->aux & IR_AUX_VOLATILE) != 0;
                    if (sw == 0) sw = 4;
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: release barrier */
                    if (sw == 1) {
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    STRB r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                    } else if (sw == 2) {
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    STRH r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                    } else if (sw >= 8) {
                        /* 8+ bytes: pop high word then low word, store both */
                        asmbuf_puts(&buf, "    POP {r1}\n");  /* high word */
                        emit_pop_r0(&buf);                     /* low word */
                        asmbuf_puts(&buf, "    STR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                        asmbuf_puts(&buf, "    STR r1, [r6, #");
                        asmbuf_int(&buf, I->val + 4);
                        asmbuf_puts(&buf, "]\n");
                        if (sw >= 16) {
                            asmbuf_puts(&buf, "    POP {r1}\n");
                            emit_pop_r0(&buf);
                            asmbuf_puts(&buf, "    STR r0, [r6, #");
                            asmbuf_int(&buf, I->val + 8);
                            asmbuf_puts(&buf, "]\n");
                            asmbuf_puts(&buf, "    STR r1, [r6, #");
                            asmbuf_int(&buf, I->val + 12);
                            asmbuf_puts(&buf, "]\n");
                        }
                    } else {
                        emit_pop_r0(&buf);
                        asmbuf_puts(&buf, "    STR r0, [r6, #");
                        asmbuf_int(&buf, I->val);
                        asmbuf_puts(&buf, "]\n");
                    }
                    if (is_volatile)
                        emit_line(&buf, "DMB SY");  /* volatile: full barrier */
                    break;
                }
                case IR_LOAD_IDX: {
                    int es = I->val;
                    /* Bug-I5 FIX: mask off IR_AUX_VOLATILE (0x10000) before
                     * checking the packed flag. The volatile bit is carried for
                     * optimizer/host-compiler correctness and does not affect
                     * the ARM load encoding. */
                    int packed = (I->aux & 0x400) ? 1 : 0;
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    if (es > 1) {
                        emit_mov_imm(&buf, 2, es);
                        emit_line(&buf, "MUL r1, r2");
                    }
                    emit_line(&buf, "ADD r0, r0, r1");
                    if (es == 1) {
                        emit_line(&buf, "LDRB r0, [r0, #0]");
                    } else if (es == 2 && !packed) {
                        emit_line(&buf, "LDRSH r0, [r0, #0]");
                    } else if (packed) {
                        /* __packed pointer unaligned access: load byte by byte then assemble.
                         * es==2: LDRB x2 + LSL + ORR
                         * es==4: LDRB x4 + LSL + ORR chain
                         * r0=address, r2/r3 are scratch registers */
                        if (es == 2) {
                            emit_line(&buf, "LDRB r2, [r0, #0]");
                            emit_line(&buf, "LDRB r3, [r0, #1]");
                            emit_line(&buf, "LSL r3, r3, #8");
                            emit_line(&buf, "ORR r0, r2, r3");
                        } else if (es == 4) {
                            emit_line(&buf, "LDRB r2, [r0, #0]");
                            emit_line(&buf, "LDRB r3, [r0, #1]");
                            emit_line(&buf, "LSL r3, r3, #8");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #2]");
                            emit_line(&buf, "LSL r3, r3, #16");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #3]");
                            emit_line(&buf, "LSL r3, r3, #24");
                            emit_line(&buf, "ORR r0, r2, r3");
                        } else {
                            /* es == 8: assemble the low word, push it, then the
                             * high word and push that -- the same "low first,
                             * high on top" order the aligned path uses, so the
                             * consumer cannot tell the two apart.  This used to
                             * fall into the 4-byte branch above and hand back a
                             * silently short long long; it is now real. */
                            emit_line(&buf, "LDRB r2, [r0, #0]");
                            emit_line(&buf, "LDRB r3, [r0, #1]");
                            emit_line(&buf, "LSL r3, r3, #8");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #2]");
                            emit_line(&buf, "LSL r3, r3, #16");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #3]");
                            emit_line(&buf, "LSL r3, r3, #24");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "PUSH {r2}");
                            emit_line(&buf, "LDRB r2, [r0, #4]");
                            emit_line(&buf, "LDRB r3, [r0, #5]");
                            emit_line(&buf, "LSL r3, r3, #8");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #6]");
                            emit_line(&buf, "LSL r3, r3, #16");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "LDRB r3, [r0, #7]");
                            emit_line(&buf, "LSL r3, r3, #24");
                            emit_line(&buf, "ORR r2, r2, r3");
                            emit_line(&buf, "PUSH {r2}");
                            break;
                        }
                    } else if (es == 8) {
                        /* #A4 FIX: 8-byte element (long long/double): load low and high words.
                         * Stack convention (from IR_STORE): high word on top, low below.
                         * So push low first, then high. */
                        emit_line(&buf, "LDR r2, [r0, #0]");  /* r2 = low word */
                        emit_line(&buf, "LDR r3, [r0, #4]");  /* r3 = high word */
                        emit_line(&buf, "PUSH {r2}");  /* push low word */
                        emit_line(&buf, "PUSH {r3}");  /* push high word (on top) */
                        break;
                    } else {
                        emit_line(&buf, "LDR r0, [r0, #0]");
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_STORE_IDX: {
                    int es = I->val;
                    /* Bug-I5 FIX: mask off IR_AUX_VOLATILE (0x10000) before
                     * checking the packed flag. The volatile bit is carried for
                     * optimizer/host-compiler correctness and does not affect
                     * the ARM store encoding. */
                    int packed = (I->aux & 0x400) ? 1 : 0;
                    if (es == 8 && packed) {
                        /* 8-byte __packed store: the same three stack slots as
                         * the aligned path, but written one byte at a time --
                         * an aligned STR to an odd address faults.  r3 is the
                         * shift scratch; the value is pushed back so the store
                         * expression still has a value. */
                        emit_line(&buf, "POP {r1}");          /* r1 = high word (top) */
                        emit_pop_r0(&buf);                     /* r0 = low word */
                        emit_line(&buf, "POP {r2}");          /* r2 = addr */
                        emit_line(&buf, "STRB r0, [r2, #0]");
                        emit_line(&buf, "LSR r3, r0, #8");
                        emit_line(&buf, "STRB r3, [r2, #1]");
                        emit_line(&buf, "LSR r3, r0, #16");
                        emit_line(&buf, "STRB r3, [r2, #2]");
                        emit_line(&buf, "LSR r3, r0, #24");
                        emit_line(&buf, "STRB r3, [r2, #3]");
                        emit_line(&buf, "STRB r1, [r2, #4]");
                        emit_line(&buf, "LSR r3, r1, #8");
                        emit_line(&buf, "STRB r3, [r2, #5]");
                        emit_line(&buf, "LSR r3, r1, #16");
                        emit_line(&buf, "STRB r3, [r2, #6]");
                        emit_line(&buf, "LSR r3, r1, #24");
                        emit_line(&buf, "STRB r3, [r2, #7]");
                        emit_line(&buf, "PUSH {r0}");
                        emit_line(&buf, "PUSH {r1}");
                        break;
                    }
                    if (es == 8) {
                        /* #A4 FIX: 8-byte element (long long/double): stack has
                         * [addr, value_low, value_high] (high on top, per IR_STORE
                         * convention). Pop high, low, addr; store both; push back. */
                        emit_line(&buf, "POP {r1}");          /* r1 = high word (top) */
                        emit_pop_r0(&buf);                     /* r0 = low word */
                        emit_line(&buf, "POP {r2}");          /* r2 = addr */
                        emit_line(&buf, "STR r0, [r2, #0]");
                        emit_line(&buf, "STR r1, [r2, #4]");
                        /* Push value back as expression result: low then high */
                        emit_line(&buf, "PUSH {r0}");
                        emit_line(&buf, "PUSH {r1}");
                        break;
                    }
                    emit_pop_r0(&buf);
                    emit_pop_r1(&buf);
                    if (es == 1) {
                        emit_line(&buf, "STRB r0, [r1, #0]");
                    } else if (es == 2 && !packed) {
                        emit_line(&buf, "STRH r0, [r1, #0]");
                    } else if (packed) {
                        /* __packed pointer unaligned store: write byte by byte.
                         * r0=value, r1=address, r2 is a scratch register */
                        if (es == 2) {
                            emit_line(&buf, "STRB r0, [r1, #0]");
                            emit_line(&buf, "LSR r2, r0, #8");
                            emit_line(&buf, "STRB r2, [r1, #1]");
                        } else {
                            emit_line(&buf, "STRB r0, [r1, #0]");
                            emit_line(&buf, "LSR r2, r0, #8");
                            emit_line(&buf, "STRB r2, [r1, #1]");
                            emit_line(&buf, "LSR r2, r0, #16");
                            emit_line(&buf, "STRB r2, [r1, #2]");
                            emit_line(&buf, "LSR r2, r0, #24");
                            emit_line(&buf, "STRB r2, [r1, #3]");
                        }
                    } else {
                        emit_line(&buf, "STR r0, [r1, #0]");
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_BINARY:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_binary(&buf, I->aux);
                    emit_push_r0(&buf);
                    break;
                case IR_UNARY:
                    if (I->val == 2 &&
                        (I->aux == (int)TOK_MINUS || I->aux == (int)TOK_BNOT)) {
                        /* 64-bit negate / complement (irgen marks the width in
                         * I->val: 2 = two stack slots).  Both words belong to one
                         * value, so both have to change -- the old single-word
                         * path flipped only the high half and left the low half
                         * as it was (measured: -100LL compared unequal to -100
                         * and shifted as +100).
                         * Negate is two's complement: invert both words, add one,
                         * and push the carry from the low word into the high one.
                         * The 16-bit ADD sets the flags the ADC needs. */
                        emit_pop_r1(&buf);
                        emit_pop_r0(&buf);
                        emit_line(&buf, "MVN r0, r0");
                        emit_line(&buf, "MVN r1, r1");
                        if (I->aux == (int)TOK_MINUS) {
                            /* Two-operand ADDS, not three-operand ADD: in
                             * unified syntax the three-operand form assembles to the
                             * 32-bit ADD.W.  That is architecturally equivalent, but
                             * the Unicorn-based gate harness does not update the flags
                             * for ADD.W (measured: with carry pre-set, ADC then added
                             * an extra 1 and -7LL came out as -8).  The 16-bit form
                             * sets the flags correctly on both hardware and the harness
                             * and is one instruction shorter. */
                            emit_line(&buf, "ADDS r0, #1");   /* ADDS, not ADD: GNU as encodes
                             * "add r0, r0, #imm"/"add r0, #imm" without
                             * the S as the 32-bit ADD.W, which leaves the
                             * flags alone (verified with objdump: `adds
                             * r0, #1` -> 3001, `add r0, #1` -> f100 0001),
                             * and the carry into the ADC below depends on
                             * them.  The built-in assembler accepts
                             * ADDS/SUBS unconditionally for this reason. */
                            emit_line(&buf, "MOV r2, #0");
                            emit_line(&buf, "ADC r1, r2");
                        }
                        asmbuf_puts(&buf, "    PUSH {r0}\n");
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                        break;
                    }
                    if (I->val == 1) {
                        /* Floating-point negation.  irgen tags `-x` on a float
                         * or a double with val=1, and this used to run it
                         * through the integer NEG: that mangles the exponent of
                         * every value and turns -0.0 into +0.0, where IEEE only
                         * flips the sign bit.  A float is one word on the stack
                         * and a double's HIGH word is on top -- that word holds
                         * the sign bit in both cases, so popping one word and
                         * flipping bit 31 is right for both, with no need to
                         * know which precision it is. */
                        emit_pop_r0(&buf);
                        if (I->aux == (int)TOK_MINUS) {
                            emit_line(&buf, "EOR r0, r0, #0x80000000");
                        } else {
                            char b1[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "the arm backend does not implement the "
                                      "floating-point unary operator %s "
                                      "(function '%s')",
                                      diag_num(b1, I->aux), f->name, 0,0,0,0);
                        }
                        emit_push_r0(&buf);
                        break;
                    }
                    emit_pop_r0(&buf);
                    emit_unary(&buf, I->aux);
                    emit_push_r0(&buf);
                    break;
                case IR_LABEL:
                    emit_label(&buf, f->name, I->val);
                    break;
                case IR_LABEL_NAMED:
                    asmbuf_puts(&buf, ".L_");
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_puts(&buf, ":\n");
                    break;
                case IR_JMP:
                    emit_b_label(&buf, "B", f->name, I->val);
                    break;
                case IR_JMP_NAMED:
                    asmbuf_puts(&buf, "    B .L_");
                    asmbuf_puts(&buf, f->name);
                    asmbuf_putc(&buf, '_');
                    asmbuf_puts(&buf, I->name);
                    asmbuf_putc(&buf, '\n');
                    break;
                case IR_JZ:
                    emit_pop_r0(&buf);
                    emit_line(&buf, "CMP r0, #0");
                    emit_b_label(&buf, "BEQ", f->name, I->val);
                    break;
                case IR_JNZ:
                    emit_pop_r0(&buf);
                    emit_line(&buf, "CMP r0, #0");
                    emit_b_label(&buf, "BNE", f->name, I->val);
                    break;
                case IR_CMP_JMP_EQ:
                    emit_pop_r0(&buf);
                    emit_mov_imm(&buf, 1, I->val);
                    emit_line(&buf, "CMP r0, r1");
                    emit_b_label(&buf, "BEQ", f->name, I->aux);
                    break;
                /* === Floating-point ops: ARM soft-float ABI ===
                 * The high bit 0x200 of aux marks double-precision (double):
                 *   - single (float): args in r0/r1, result in r0, 1 stack slot (4B), uses __aeabi_f*
                 *   - double (double): args in r0/r1+r2/r3, result in r0/r1, 2 stack slots (8B), uses __aeabi_d*
                 * lr is already saved by the prologue; BL inside the body may freely clobber lr. */
                case IR_PUSH_FLOAT: {
                    int is_dbl = (I->aux & 0x200) != 0;
                    if (is_dbl) {
                        /* Double-precision: load 8 bytes, push 2 slots (low word pushed first) */
                        asmbuf_puts(&buf, "    LDR r0, =.L_dbl_");
                        asmbuf_int(&buf, I->val);
                        asmbuf_putc(&buf, '\n');
                        emit_line(&buf, "LDR r1, [r0]");       /* low word */
                        emit_line(&buf, "LDR r2, [r0, #4]");   /* high word */
                        asmbuf_puts(&buf, "    PUSH {r1}\n");  /* low word pushed first */
                        asmbuf_puts(&buf, "    PUSH {r2}\n");  /* high word pushed last (on top) */
                    } else {
                        /* Single-precision: load 32-bit float bit pattern from the float constant pool into r0 */
                        asmbuf_puts(&buf, "    LDR r0, =.L_flt_");
                        asmbuf_int(&buf, I->val);
                        asmbuf_putc(&buf, '\n');
                        emit_line(&buf, "LDR r0, [r0]");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FADD: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VADD");
                        else        emit_vfp_f32_binop(&buf, "VADD");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_dadd");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fadd");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FSUB: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VSUB");
                        else        emit_vfp_f32_binop(&buf, "VSUB");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_dsub");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fsub");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FMUL: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VMUL");
                        else        emit_vfp_f32_binop(&buf, "VMUL");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_dmul");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fmul");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_FDIV: {
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) emit_vfp_f64_binop(&buf, "VDIV");
                        else        emit_vfp_f32_binop(&buf, "VDIV");
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        emit_line(&buf, "BL __aeabi_ddiv");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_fdiv");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_F2I: {
                    /* float/double -> int (truncation toward zero) */
                    int is_dbl = (I->aux & 0x200) != 0;
                    /* F2I unsigned fix: 0x400 = unsigned destination; select
                     * __aeabi_d2uiz/__aeabi_f2uiz for unsigned targets. */
                    int is_unsigned = (I->aux & 0x400) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        if (is_dbl) {
                            /* double: pop d0, VCVT.S32.F64 s0,d0, move back to r0 */
                            emit_line(&buf, "POP {r1}");  /* high */
                            emit_line(&buf, "POP {r0}");  /* low */
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.U32.F64 s0, d0");
                            else
                                emit_line(&buf, "VCVT.S32.F64 s0, d0");
                            emit_line(&buf, "VMOV r0, s0");
                        } else {
                            emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.U32.F32 s0, s0");
                            else
                                emit_line(&buf, "VCVT.S32.F32 s0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                        }
                        emit_push_r0(&buf);
                        break;
                    }
#endif
                    if (is_dbl) {
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                        if (is_unsigned)
                            emit_line(&buf, "BL __aeabi_d2uiz");
                        else
                            emit_line(&buf, "BL __aeabi_d2iz");
                    } else {
                        emit_pop_r0(&buf);
                        if (is_unsigned)
                            emit_line(&buf, "BL __aeabi_f2uiz");
                        else
                            emit_line(&buf, "BL __aeabi_f2iz");
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_I2F: {
                    /* int -> float/double */
                    int is_dbl = (I->aux & 0x200) != 0;
                    /* Bug-P1-7: 0x400 = unsigned source; select __aeabi_ui2d /
                     * __aeabi_ui2f instead of the signed __aeabi_i2d /
                     * __aeabi_i2f so that (double)(unsigned)v yields the
                     * correct positive value. */
                    int is_unsigned = (I->aux & 0x400) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        emit_pop_r0(&buf);
                        emit_line(&buf, "VMOV s0, r0");
                        if (is_dbl) {
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.F64.U32 d0, s0");
                            else
                                emit_line(&buf, "VCVT.F64.S32 d0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_line(&buf, "VMOV r1, s1");
                            emit_push_r0(&buf);
                            asmbuf_puts(&buf, "    PUSH {r1}\n");
                        } else {
                            if (is_unsigned)
                                emit_line(&buf, "VCVT.F32.U32 s0, s0");
                            else
                                emit_line(&buf, "VCVT.F32.S32 s0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_push_r0(&buf);
                        }
                        break;
                    }
#endif
                    emit_pop_r0(&buf);
                    if (is_dbl) {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ui2d" : "BL __aeabi_i2d");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ui2f" : "BL __aeabi_i2f");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_64I2F: {
                    /* long long (64-bit) -> float/double.
                     * Stack model: [r0_low, r1_high] (high on top).
                     * AAPCS helpers: __aeabi_l2d/ul2d (-> r0/r1 double),
                     *                __aeabi_l2f/ul2f (-> r0 float). */
                    int is_dbl = (I->aux & 0x200) != 0;
                    int is_unsigned = (I->aux & 0x400) != 0;
                    emit_pop_r1(&buf);  /* high */
                    emit_pop_r0(&buf);  /* low */
                    if (is_dbl) {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ul2d" : "BL __aeabi_l2d");
                        /* double result: r0=low, r1=high */
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    } else {
                        emit_line(&buf, is_unsigned ? "BL __aeabi_ul2f" : "BL __aeabi_l2f");
                        emit_push_r0(&buf);
                    }
                    break;
                }
                case IR_64F2I: {
                    /* float/double -> long long (64-bit).
                     * For double: stack has [r0_low, r1_high]; pop both.
                     * For float:  stack has [r0]; pop one.
                     * AAPCS helpers: __aeabi_d2lz/d2ulz (double -> r0/r1),
                     *                __aeabi_f2lz/f2ulz (float  -> r0/r1). */
                    int is_dbl = (I->aux & 0x200) != 0;
                    int is_unsigned = (I->aux & 0x400) != 0;
                    if (is_dbl) {
                        emit_pop_r1(&buf);  /* high */
                        emit_pop_r0(&buf);  /* low */
                        emit_line(&buf, is_unsigned ? "BL __aeabi_d2ulz" : "BL __aeabi_d2lz");
                    } else {
                        emit_pop_r0(&buf);
                        emit_line(&buf, is_unsigned ? "BL __aeabi_f2ulz" : "BL __aeabi_f2lz");
                    }
                    /* 64-bit result: r0=low, r1=high */
                    emit_push_r0(&buf);
                    asmbuf_puts(&buf, "    PUSH {r1}\n");
                    break;
                }
                case IR_F2F: {
                    /* float <-> float cast. On the ARM soft-float stack:
                     *   double = [r0_low, r1_high] (2 slots, high on top)
                     *   float  = [r0]              (1 slot)
                     *   aux 0x200 = double->float (narrow): -1 slot
                     *   aux 0x100 = float->double (widen):  +1 slot
                     * Both directions go to the run-time library: widening is
                     * exact so it could be done here, but keeping one rule is
                     * simpler than explaining why half of this IR op is
                     * self-contained and half is not. */
                    int src_dbl = (I->aux & 0x200) != 0;
                    int dst_dbl = (I->aux & 0x100) != 0;
#if ARKCC_FEATURE_FPU_HW
                    /* A single-precision FPU cannot convert to or from double
                     * (VCVT.F64.* is ARMv8-M / M7 DP only), so those two
                     * directions stay on the software helpers even when
                     * hardware FP is enabled. */
                    if (g_config.enable_fpu_hw && g_arm_fpu_dp) {
                        if (src_dbl && !dst_dbl) {
                            /* double -> float */
                            emit_pop_r1(&buf);
                            emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            emit_line(&buf, "VCVT.F32.F64 s0, d0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_push_r0(&buf);
                        } else if (!src_dbl && dst_dbl) {
                            /* float -> double */
                            emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VCVT.F64.F32 d0, s0");
                            emit_line(&buf, "VMOV r0, s0");
                            emit_line(&buf, "VMOV r1, s1");
                            emit_push_r0(&buf);
                            asmbuf_puts(&buf, "    PUSH {r1}\n");
                        }
                        break;
                    }
#endif
                    if (src_dbl && !dst_dbl) {
                        /* double -> float */
                        emit_pop_r1(&buf);
                        emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_d2f");
                        emit_push_r0(&buf);
                    } else if (!src_dbl && dst_dbl) {
                        /* float -> double */
                        emit_pop_r0(&buf);
                        emit_line(&buf, "BL __aeabi_f2d");
                        emit_push_r0(&buf);
                        asmbuf_puts(&buf, "    PUSH {r1}\n");
                    }
                    /* same-size: no-op */
                    break;
                }
                case IR_FCMP: {
                    /* aux encodes the comparison kind (low 8 bits) + precision flag (0x200);
                     * __aeabi_fcmpeq and __aeabi_dcmpeq family return 0/1 */
                    int op = I->aux & ~0x200;
                    int is_dbl = (I->aux & 0x200) != 0;
#if ARKCC_FEATURE_FPU_HW
                    if (g_config.enable_fpu_hw) {
                        const char *cond = "EQ";
                        if (is_dbl) {
                            emit_line(&buf, "POP {r3}");
                            emit_line(&buf, "POP {r2}");
                            emit_line(&buf, "POP {r1}");
                            emit_line(&buf, "POP {r0}");
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            emit_line(&buf, "VMOV s2, r2");
                            emit_line(&buf, "VMOV s3, r3");
                            emit_line(&buf, "VCMP.F64 d0, d1");
                        } else {
                            emit_pop_r1(&buf); emit_pop_r0(&buf);
                            emit_line(&buf, "VMOV s0, r0");
                            emit_line(&buf, "VMOV s1, r1");
                            emit_line(&buf, "VCMP.F32 s0, s1");
                        }
                        emit_line(&buf, "VMRS APSR_nzcv, FPSCR");
                        emit_mov_imm(&buf, 0, 0);
                        switch (op) {
                            case (int)TOK_EQ: cond = "EQ"; break;
                            case (int)TOK_NE: cond = "NE"; break;
                            case (int)TOK_LT: cond = "LT"; break;
                            case (int)TOK_GT: cond = "GT"; break;
                            case (int)TOK_LE: cond = "LE"; break;
                            case (int)TOK_GE: cond = "GE"; break;
                            default: cond = "EQ"; break;
                        }
                        asmbuf_puts(&buf, "    IT ");
                        asmbuf_puts(&buf, cond);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, "    MOV");
                        asmbuf_puts(&buf, cond);
                        asmbuf_puts(&buf, " r0, #1\n");
                        emit_push_r0(&buf);
                        break;
                    }
#endif
                    {
                    const char *helper = "__aeabi_fcmpeq";
                    switch (op) {
                        case (int)TOK_EQ: helper = is_dbl ? "__aeabi_dcmpeq" : "__aeabi_fcmpeq"; break;
                        case (int)TOK_NE: helper = is_dbl ? "__aeabi_dcmpeq" : "__aeabi_fcmpeq"; break;
                        case (int)TOK_LT: helper = is_dbl ? "__aeabi_dcmplt" : "__aeabi_fcmplt"; break;
                        case (int)TOK_GT: helper = is_dbl ? "__aeabi_dcmpgt" : "__aeabi_fcmpgt"; break;
                        case (int)TOK_LE: helper = is_dbl ? "__aeabi_dcmple" : "__aeabi_fcmple"; break;
                        case (int)TOK_GE: helper = is_dbl ? "__aeabi_dcmpge" : "__aeabi_fcmpge"; break;
                        default: break;
                    }
                    if (is_dbl) {
                        emit_line(&buf, "POP {r3}");
                        emit_line(&buf, "POP {r2}");
                        emit_line(&buf, "POP {r1}");
                        emit_line(&buf, "POP {r0}");
                    } else {
                        emit_pop_r1(&buf); emit_pop_r0(&buf);
                    }
                    asmbuf_puts(&buf, "    BL ");
                    asmbuf_puts(&buf, helper);
                    asmbuf_putc(&buf, '\n');
                    /* NE: use fcmpeq/dcmpeq then flip the low bit */
                    if (op == (int)TOK_NE) emit_line(&buf, "EOR r0, r0, #1");
                    emit_push_r0(&buf);
                    break;
                    }
                }
                case IR_ASM:
                    /* Inline assembly written out as-is.
                     * Extended form with '@' prefix (assembled by irgen): skip past the quote and write the template,
                     * ignoring outputs/inputs/clobbers; also replace %0/%1/... with
                     * r0/r1/... (AAPCS operand index maps to register, armcc-style substitution). */
                    if (I->name && I->name[0] == '@') {
                        const char *s = I->name + 1;
                        /* Skip whitespace to find the first '"' */
                        while (*s == ' ' || *s == '\t') s++;
                        if (*s == '"') {
                            s++;
                            asmbuf_puts(&buf, "    ");
                            /* Copy template content (handle \n / \t escapes), stop at an unescaped '"' */
                            while (*s && *s != '"') {
                                if (*s == '\\' && s[1]) {
                                    if (s[1] == 'n') { asmbuf_putc(&buf, '\n'); asmbuf_puts(&buf, "    "); }
                                    else if (s[1] == 't') asmbuf_putc(&buf, '\t');
                                    else if (s[1] == '\\') asmbuf_putc(&buf, '\\');
                                    else if (s[1] == '"') asmbuf_putc(&buf, '"');
                                    else asmbuf_putc(&buf, s[1]);
                                    s += 2;
                                } else if (*s == '%' && s[1] >= '0' && s[1] <= '3') {
                                    /* Extended inline asm operand substitution: %0 -> r0, %1 -> r1, ... */
                                    asmbuf_putc(&buf, 'r');
                                    asmbuf_putc(&buf, s[1]);
                                    s += 2;
                                } else {
                                    asmbuf_putc(&buf, *s);
                                    s++;
                                }
                            }
                            asmbuf_putc(&buf, '\n');
                        }
                    } else if (I->name) {
                        asmbuf_puts(&buf, "    ");
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                    }
                    break;
                case IR_CALL: {
                    /* AAPCS: first 4 args go in r0-r3, args 5+ go on the stack.
                     * The stack-based IR pushes all args onto the stack (arg0 on top); here we pop the first 4
                     * into r0-r3, leaving the remaining stack_args on the stack for the callee to read.
                     * After the call the caller cleans up the stack args (caller cleanup).
                     * P1 FIX: When stack_args is odd, a 4-byte padding is inserted before BL
                     * and removed after, ensuring 8-byte SP alignment at BL per AAPCS. */
                    int n = I->val;
                    int k;
                    /* Indirect call (obj->fnptr()): I->name starts with '?'. The function
                     * pointer was pushed LAST (it's on top of stack). Pop it into ip (r12)
                     * first, then pop real args into r0-r3, then BLX ip. */
                    int is_indirect = (I->name && I->name[0] == '?');
                    int real_args = is_indirect ? n - 1 : n;
                    int is_sret_call = (!is_indirect && sret_lookup(sret_names, sret_sizes, sret_count, I->name));

                    /* armcc intrinsic functions: emit inline instruction instead of BL.
                     * Arguments (if any) are already on the stack; pop and discard them. */
                    if (!is_indirect && (ark_streq(I->name, "__nop") ||
                        ark_streq(I->name, "__isb") ||
                        ark_streq(I->name, "__dsb") ||
                        ark_streq(I->name, "__dmb") ||
                        ark_streq(I->name, "__memory_changed") ||
                        ark_streq(I->name, "__schedule_barrier") ||
                        ark_streq(I->name, "__force_stores") ||
                        ark_streq(I->name, "__disable_irq") ||
                        ark_streq(I->name, "__enable_irq") ||
                        ark_streq(I->name, "__wfi") ||
                        ark_streq(I->name, "__wfe") ||
                        ark_streq(I->name, "__sev") ||
                        ark_streq(I->name, "__clz") ||
                        ark_streq(I->name, "__rbit"))) {
                        int reg_args = (real_args > 4) ? 4 : real_args;
                        int stack_args = (real_args > 4) ? (real_args - 4) : 0;
                        for (k = 0; k < reg_args; k++) {
                            asmbuf_puts(&buf, "    POP {r");
                            asmbuf_int(&buf, k);
                            asmbuf_puts(&buf, "}\n");
                        }
                        if (stack_args > 0) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #");
                            asmbuf_int(&buf, stack_args * 4);
                            asmbuf_putc(&buf, '\n');
                        }
                        if (ark_streq(I->name, "__nop"))         asmbuf_puts(&buf, "    NOP\n");
                        else if (ark_streq(I->name, "__isb"))    asmbuf_puts(&buf, "    ISB SY\n");
                        else if (ark_streq(I->name, "__dsb"))    asmbuf_puts(&buf, "    DSB SY\n");
                        else if (ark_streq(I->name, "__dmb"))    asmbuf_puts(&buf, "    DMB SY\n");
                        else if (ark_streq(I->name, "__memory_changed")) ; /* compiler barrier: no-op */
                        else if (ark_streq(I->name, "__schedule_barrier")) ; /* CMSIS barrier fence: no instruction */
                        /* armcc __force_stores: flush the compiler's pending
                         * stores. Our codegen always emits the store where the
                         * statement is, so there is nothing to flush and no
                         * instruction is required (it is not a hardware
                         * barrier; that is __dsb). */
                        else if (ark_streq(I->name, "__force_stores")) ;
                        else if (ark_streq(I->name, "__disable_irq")) asmbuf_puts(&buf, "    CPSID i\n");
                        else if (ark_streq(I->name, "__enable_irq"))  asmbuf_puts(&buf, "    CPSIE i\n");
                        else if (ark_streq(I->name, "__wfi"))    asmbuf_puts(&buf, "    WFI\n");
                        else if (ark_streq(I->name, "__wfe"))   asmbuf_puts(&buf, "    WFE\n");
                        else if (ark_streq(I->name, "__sev"))   asmbuf_puts(&buf, "    SEV\n");
                        else if (ark_streq(I->name, "__clz"))   asmbuf_puts(&buf, "    CLZ r0, r0\n");
                        /* armcc __rbit: CMSIS __RBIT maps onto it in the
                         * __CC_ARM branch, so vendor code (HAL_CRYP) calls it
                         * with one argument; the result is uint32_t. */
                        else if (ark_streq(I->name, "__rbit"))  asmbuf_puts(&buf, "    RBIT r0, r0\n");
                        emit_push_r0(&buf);
                        break;
                    }

                    /* sret call: callee returns struct > 8 bytes via hidden pointer in r0.
                     * P1 FIX: sret buffer is pre-allocated in the caller's frame (via r6),
                     * NOT on the stack. This ensures:
                     *   1. Callee reads stack args at SP+0 (not sret space).
                     *   2. sret buffer survives the callee's PUSH (above SP, in the frame).
                     *   3. The sret pointer pushed after BL remains valid (buffer not freed).
                     * AAPCS: r0 = sret pointer, real args start at r1.
                     * 64-bit args align to even register pairs (r2/r3 only option). */
                    if (is_sret_call) {
                        int argmask = I->aux;
                        int aapcs_reg = 1;  /* r0 = sret, args start at r1 */
                        int total_slots = 0;
                        int reg_slots = 0;
                        int stack_args;
                        int need_align;
                        int buf_off = (sret_call_idx < sret_call_n)
                                      ? sret_call_buf_off[sret_call_idx++] : 0;
                        int kk;
                        for (kk = 0; kk < real_args; kk++) {
                            int is_64 = (argmask >> (2 * kk + 1)) & 1;
                            total_slots += is_64 ? 2 : 1;
                        }
                        /* Pop each arg from the IR stack into the correct AAPCS
                         * register. IR stack: arg0 on top. For 64-bit args, high
                         * word is on top, low word below (pushed first). */
                        for (kk = 0; kk < real_args; kk++) {
                            int is_64 = (argmask >> (2 * kk + 1)) & 1;
                            if (is_64) {
                                if (aapcs_reg & 1) aapcs_reg++;  /* even-pair align */
                                if (aapcs_reg + 1 <= 3) {
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aapcs_reg + 1);
                                    asmbuf_puts(&buf, "}\n");
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aapcs_reg);
                                    asmbuf_puts(&buf, "}\n");
                                    reg_slots += 2;
                                    aapcs_reg += 2;
                                } else {
                                    break;  /* both words stay on stack */
                                }
                            } else {
                                if (aapcs_reg <= 3) {
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aapcs_reg);
                                    asmbuf_puts(&buf, "}\n");
                                    reg_slots += 1;
                                    aapcs_reg += 1;
                                } else {
                                    break;
                                }
                            }
                        }
                        stack_args = total_slots - reg_slots;
                        if (stack_args < 0) stack_args = 0;
                        need_align = (stack_args & 1) != 0;
                        /* r0 = sret buffer address (pre-allocated in caller's frame) */
                        asmbuf_puts(&buf, "    ADD r0, r6, #");
                        asmbuf_int(&buf, buf_off);
                        asmbuf_putc(&buf, '\n');
                        if (need_align) {
                            asmbuf_puts(&buf, "    SUB sp, sp, #4\n");
                        }
                        asmbuf_puts(&buf, "    BL ");
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                        if (need_align) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #4\n");
                        }
                        if (stack_args > 0) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #");
                            asmbuf_int(&buf, stack_args * 4);
                            asmbuf_putc(&buf, '\n');
                        }
                        emit_push_r0(&buf);
                        break;
                    }

                    /* Regular (non-sret) call.
                     * AAPCS: first 4 args in r0-r3, 64-bit args aligned to even
                     * register pairs (r0/r1 or r2/r3). Remaining args on stack.
                     * argmask bit(2*k+1)=1 marks 8-byte args (long long / double). */
                    {
                    int argmask = I->aux;
                    int aapcs_reg = 0;
                    int total_slots = 0;
                    int reg_slots = 0;
                    int stack_args;
                    int need_align;
                    int kk;

                    /* Indirect call: pop function pointer into ip (r12) first */
                    if (is_indirect) {
                        asmbuf_puts(&buf, "    POP {ip}\n");
                        asmbuf_puts(&buf, "    ORR ip, ip, #1\n");
                    }
                    /* Argument placement: one rule, shared with the callee
                     * (aapcs_assign).  The IR pushes arguments in reverse order
                     * (arg0 ends up on top) and pushes the two words of a
                     * 64-bit value high word first, so the stack arguments have
                     * to be *permuted* into the AAPCS block, not just padded:
                     * the block is laid out in argument order, each double-word
                     * argument low word first and 8-byte aligned.
                     *
                     * Before this the caller copied the pushed words straight
                     * through, so a 64-bit argument that missed the registers
                     * arrived with its halves swapped (`f(1LL, 2LL, 3LL)` summed
                     * garbage) and an argument that landed at an odd word offset
                     * was misaligned with respect to AAPCS. */
                    {
                        /* static for the same reason as aapcs_map above: 1.8 KiB
                         * of stack per call site would be a real cost on the
                         * board.  The tables are fully rewritten for every call
                         * (aapcs_assign fills the slots it uses and the loops
                         * below only read those). */
                        static int asz[64];
                        static int aslot_reg[192];
                        static int aslot_soff[192];
                        static int areg[64];
                        static int abase[64];
                        static int awords[64];
                        int sl = 0;
                        int nsaa;
                        int i;
                        /* Per-argument wire size: from irgen's side table when
                         * the call has an argument the 2-bit argmask cannot
                         * describe (a >8-byte composite through `...`), from
                         * the argmask otherwise -- in which case the numbers,
                         * and therefore the code, are exactly what they were. */
                        const int *wires = (I->aux2 > 0)
                                           ? &prog->argbytes[I->aux2 - 1] : 0;
                        for (i = 0; i < 192; i++) { aslot_reg[i] = -1; aslot_soff[i] = -1; }
                        for (kk = 0; kk < real_args && kk < 64; kk++) {
                            int b = wires ? wires[kk]
                                          : (((argmask >> (2 * kk + 1)) & 1) ? 8 : 4);
                            int w;
                            if (b < 4) b = 4;
                            w = (b + 3) / 4;
                            asz[kk] = b;
                            abase[kk] = sl;
                            awords[kk] = w;
                            sl += w;
                            total_slots += w;
                        }
                        if (sl > 192) {
                            char b1[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "call to '%s' passes %s argument words; "
                                      "the arm backend handles at most 192",
                                      I->name, diag_num(b1, sl), 0,0,0,0);
                        }
                        nsaa = aapcs_assign(asz, (real_args < 64) ? real_args : 64,
                                            0, aslot_reg, aslot_soff);
                        /* Pop the register arguments.  They are always the
                         * leading ones: once an argument is placed on the stack
                         * NCRN is 4, so every later argument is stacked too.
                         * A word argument (and any inline composite) has word 0
                         * on top, so the pops run in ascending word order; the
                         * two-word argument keeps its older convention, high
                         * word on top (irgen pushes it low word first). */
                        for (kk = 0; kk < real_args && kk < 64; kk++) {
                            int base = abase[kk], w = awords[kk], j;
                            if (w == 2) {
                                if (aslot_reg[base] < 0) break;
                                asmbuf_puts(&buf, "    POP {r");
                                asmbuf_int(&buf, aslot_reg[base] + 1);  /* high word */
                                asmbuf_puts(&buf, "}\n");
                                asmbuf_puts(&buf, "    POP {r");
                                asmbuf_int(&buf, aslot_reg[base]);      /* low word */
                                asmbuf_puts(&buf, "}\n");
                                reg_slots += 2;
                            } else {
                                int stacked = 0;
                                for (j = 0; j < w; j++) {
                                    if (aslot_reg[base + j] < 0) { stacked = 1; break; }
                                    asmbuf_puts(&buf, "    POP {r");
                                    asmbuf_int(&buf, aslot_reg[base + j]);
                                    asmbuf_puts(&buf, "}\n");
                                    reg_slots += 1;
                                }
                                if (stacked) break;
                            }
                        }
                        stack_args = total_slots - reg_slots;
                        if (stack_args < 0) stack_args = 0;
                        if (stack_args > 0) {
                            /* Build the block strictly below the pushed words
                             * and point sp at it.  r5 keeps the old sp and is
                             * also the copy source; r4 is the new sp; r7 is the
                             * data scratch.  r0-r3 already hold register
                             * arguments and r6 is the frame pointer, so none of
                             * those may be touched here. */
                            int si = 0;
                            /* r4 is the new sp of the block below and r4
                             * is callee-saved, so it has to be preserved
                             * across the call.  It used to be used raw: the
                             * callee clobbered the caller's r4 and nothing at
                             * the call site could see it (measured:
                             * __ark_ldiv64 read a dead r4 for its remainder and
                             * returned 0x2001febd where 100 % 3 == 1).  Saving
                             * it inside the call sequence instead of in the
                             * prologue means a function pays the 8 bytes only
                             * if it really passes more than 16 bytes. */
                            asmbuf_puts(&buf, "    PUSH {r4}\n");
                            asmbuf_puts(&buf, "    MOV r5, sp\n");
                            /* r5 must still point at the word image that
                             * the pops left behind; the saved r4 is the
                             * word just below it. */
                            asmbuf_puts(&buf, "    ADD r5, r5, #4\n");
                            asmbuf_puts(&buf, "    SUB r4, sp, #");
                            asmbuf_int(&buf, (nsaa > 0) ? nsaa : 4);
                            asmbuf_puts(&buf, "\n");
                            /* Round the new sp down to an 8-byte boundary.  The
                             * IR stack depth at the call is not tracked, so the
                             * AAPCS alignment is enforced instead of assumed;
                             * the extra word (if any) lands at the top of the
                             * block where nothing reads it. */
                            asmbuf_puts(&buf, "    MOV r7, #7\n");
                            asmbuf_puts(&buf, "    MVN r7, r7\n");
                            asmbuf_puts(&buf, "    AND r4, r7\n");
                            asmbuf_puts(&buf, "    MOV sp, r4\n");
                            for (kk = 0; kk < real_args && kk < 64; kk++) {
                                int base = abase[kk], w = awords[kk], j;
                                if (w == 2) {
                                    int s_off, d_off;
                                    if (aslot_reg[base] >= 0) continue;
                                    s_off = 4 * si;
                                    d_off = aslot_soff[base];
                                    asmbuf_puts(&buf, "    LDR r7, [r5, #");
                                    asmbuf_int(&buf, s_off + 4);  /* low word (pushed second) */
                                    asmbuf_puts(&buf, "]\n");
                                    asmbuf_puts(&buf, "    STR r7, [r4, #");
                                    asmbuf_int(&buf, d_off);
                                    asmbuf_puts(&buf, "]\n");
                                    asmbuf_puts(&buf, "    LDR r7, [r5, #");
                                    asmbuf_int(&buf, s_off);      /* high word */
                                    asmbuf_puts(&buf, "]\n");
                                    asmbuf_puts(&buf, "    STR r7, [r4, #");
                                    asmbuf_int(&buf, d_off + 4);
                                    asmbuf_puts(&buf, "]\n");
                                    si += 2;
                                } else {
                                    /* Word arguments and inline composites: the
                                     * words still on the image are, in
                                     * ascending address order, exactly the ones
                                     * that did not fit in a register (popping
                                     * the rest moved r5 up). */
                                    for (j = 0; j < w; j++) {
                                        int d_off = aslot_soff[base + j];
                                        if (aslot_reg[base + j] >= 0) continue;
                                        asmbuf_puts(&buf, "    LDR r7, [r5, #");
                                        asmbuf_int(&buf, 4 * si);
                                        asmbuf_puts(&buf, "]\n");
                                        asmbuf_puts(&buf, "    STR r7, [r4, #");
                                        asmbuf_int(&buf, d_off);
                                        asmbuf_puts(&buf, "]\n");
                                        si += 1;
                                    }
                                }
                            }
                        }
                    }
                    if (is_indirect) {
                        asmbuf_puts(&buf, "    BLX ip\n");
                    } else {
                        asmbuf_puts(&buf, "    BL ");
                        asmbuf_puts(&buf, I->name);
                        asmbuf_putc(&buf, '\n');
                    }
                    if (stack_args > 0) {
                        /* r5 holds the sp from before the argument block, and
                         * the saved r4 is the single word just below it, so
                         * the restore steps over that word first.  The words
                         * that stayed on
                         * the stack are still there, above sp, so they have to
                         * be dropped as well -- forgetting that left `extra`
                         * under the result and the next IR_STORE_IDX read it as
                         * a destination address (measured: a store to 0x63). */
                        int drop = stack_args * 4;
                        asmbuf_puts(&buf, "    MOV sp, r5\n");
                        asmbuf_puts(&buf, "    SUB sp, sp, #4\n");
                        asmbuf_puts(&buf, "    POP {r4}\n");
                        if (drop <= 252) {
                            asmbuf_puts(&buf, "    ADD sp, sp, #");
                            asmbuf_int(&buf, drop);
                            asmbuf_puts(&buf, "\n");
                        } else {
                            emit_mov_imm(&buf, 7, drop);
                            asmbuf_puts(&buf, "    ADD sp, sp, r7\n");
                        }
                    }
                    /* #9 FIX: Check if called function returns 64-bit (long long/double).
                     * If so, push both r0 (low) and r1 (high).
                     * For indirect calls, the return type is not in prog->funcs,
                     * so irgen encodes the 64-bit return flag in bit 32 of val.
                     * ROUND4-P2 FIX: For direct calls to void functions, skip
                     * the push entirely (the caller's AST_EXPR_STMT no longer
                     * emits IR_POP, so the stack stays balanced). Indirect
                     * calls conservatively push. */
                    {
                        int ret64 = 0;
                        int is_void_ret = 0;
                        if (!is_indirect) {
                            int fi2;
                            int found = 0;
                            for (fi2 = 0; fi2 < prog->func_count; fi2++) {
                                if (ark_streq(prog->funcs[fi2]->name, I->name)) {
                                    Type *rt = prog->funcs[fi2]->ret_type;
                                    found = 1;
                                    if (ret_is_two_words(rt)) {
                                        ret64 = 1;
                                    } else if (rt && rt->kind == TYPE_VOID) {
                                        is_void_ret = 1;
                                    }
                                    break;
                                }
                            }
                            /* Not defined in this translation unit: the return
                             * shape is whatever irgen saw in the prototype.  Use
                             * it instead of assuming one word -- that assumption
                             * unbalanced the stack for every void callee and
                             * dropped the high word of every long long callee. */
                            if (!found) {
                                ret64 = (int)((I->val >> 32) & 1);
                                is_void_ret = (int)((I->val >> 33) & 1);
                            }
                        } else {
                            ret64 = (int)((I->val >> 32) & 1);
                        }
                        if (ret64) {
                            emit_push_r0(&buf);              /* push low word */
                            asmbuf_puts(&buf, "    PUSH {r1}\n");  /* push high word */
                        } else if (!is_void_ret) {
                            emit_push_r0(&buf);              /* push r0 only if non-void */
                        }
                    }
                    }
                    break;
                }
                case IR_SVC: {
                    /* __svc(n) - args are on top of stack, pop to r0 then execute SVC */
                    int n = I->val;
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, "    SVC #");
                    asmbuf_int(&buf, n);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                case IR_SMC: {
                    /* __smc(n) - secure monitor call; args are on top of stack, pop to r0 then execute SMC */
                    int n = I->val;
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, "    SMC #");
                    asmbuf_int(&buf, n);
                    asmbuf_putc(&buf, '\n');
                    emit_push_r0(&buf);
                    break;
                }
#endif
                /* === Integer divide/modulo ===
                 * Top of stack is the divisor (r1), next is the dividend (r0); result pushed back to r0.
                 * Cortex-M4 (ARMv7E-M) has hardware SDIV/UDIV; use them instead of __aeabi_idiv
                 * so single-file .bin output works without linking libgcc. */
                case IR_DIV:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, I->aux ? "    UDIV r0, r0, r1\n" : "    SDIV r0, r0, r1\n");
                    emit_push_r0(&buf);
                    break;
                case IR_MOD:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    asmbuf_puts(&buf, I->aux ? "    UDIV r2, r0, r1\n" : "    SDIV r2, r0, r1\n");
                    emit_line(&buf, "MUL r2, r2, r1");
                    emit_line(&buf, "SUB r0, r0, r2");
                    emit_push_r0(&buf);
                    break;
                /* === 64-bit ops: value occupies two stack slots, low word pushed first, high word pushed last (high word on top) ===
                 * Call __aeabi_l* helpers: a in r0/r1 (low/high), b in r2/r3 (low/high),
                 * result in r0/r1 (low/high). Pop order: b first (r3 high / r2 low), then a (r1 high / r0 low). */
                case IR_64PUSH:
                    /* val=high 32 bits, aux=low 32 bits; low word pushed first */
                    emit_mov_imm(&buf, 0, I->aux);
                    emit_push_r0(&buf);
                    emit_mov_imm(&buf, 0, I->val);
                    emit_push_r0(&buf);
                    break;
                case IR_64BINARY: {
                    /* #I2 FIX: Mask off unsigned flag (0x100) from op, and use
                     * correct AEABI helper names (__aeabi_ldivmod not __aeabi_ldiv). */
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int op = I->aux & ~0x100;
                    const char *helper = 0;
                    int is_div = 0, is_mod = 0;
                    int is_add = 0, is_sub = 0;
                    switch (op) {
                        /* 64-bit add/sub are two instructions -- emit them
                         * inline instead of calling the AEABI helpers.
                         * __aeabi_ladd/__aeabi_lsub are *not* in gcc's
                         * libgcc.a any more (gcc >= 4.x inlines them, so the
                         * archive never defines them), which made every
                         * 64-bit add/sub image fail to link with
                         * "undefined reference to `__aeabi_ladd'". */
                        case (int)TOK_PLUS:  is_add = 1; break;
                        case (int)TOK_MINUS: is_sub = 1; break;
                        case (int)TOK_MUL:   helper = "__aeabi_lmul"; break;
                        case (int)TOK_DIV:   helper = is_unsigned ? "__aeabi_uldivmod" : "__aeabi_ldivmod"; is_div = 1; break;
                        case (int)TOK_MOD:   helper = is_unsigned ? "__aeabi_uldivmod" : "__aeabi_ldivmod"; is_mod = 1; break;
                        /* Shifts: value in r0:r1, count in r2 (0..63).  These are
                         * the standard AEABI helper names, so an image linked
                         * against a real runtime library also resolves them. */
                        case (int)TOK_SHL:   helper = "__aeabi_llsl"; break;
                        case (int)TOK_SHR:   helper = is_unsigned ? "__aeabi_llsr"
                                                                  : "__aeabi_lasr"; break;
                        default: break;
                    }
                    emit_line(&buf, "POP {r3}");
                    emit_line(&buf, "POP {r2}");
                    emit_line(&buf, "POP {r1}");
                    emit_line(&buf, "POP {r0}");
                    /* Bitwise AND/OR/XOR need no helper: both words are
                     * independent.  They used to fall through with no helper at
                     * all, i.e. the result was whatever happened to be in
                     * r0:r1 -- measured: `(-100LL & 1)` did not behave like 0,
                     * which quietly broke the standard "clear the low bit"
                     * idiom used by the reference code in the shift tests. */
                    if (op == (int)TOK_BAND) { emit_line(&buf, "AND r0, r2"); emit_line(&buf, "AND r1, r3"); }
                    else if (op == (int)TOK_BOR)  { emit_line(&buf, "ORR r0, r2"); emit_line(&buf, "ORR r1, r3"); }
                    else if (op == (int)TOK_BXOR) { emit_line(&buf, "EOR r0, r2"); emit_line(&buf, "EOR r1, r3"); }
                    /* ADDS/ADC and SUBS/SBC are emitted in their 16-bit
                     * two/three-operand forms on purpose: a Thumb-2 wide
                     * `ADD.W r0, r0, #imm` does not update APSR under
                     * unicorn 2.1.4, which would silently corrupt the carry
                     * into ADC (measured: -7LL + -1LL came out as -8LL).  The S is
                     * mandatory here: without it GNU as picks the 32-bit
                     * ADD.W/SUB.W (verified with objdump: `add r0, r0, r2` ->
                     * eb00 0002, `adds ...` -> 1880), which leaves the flags
                     * alone and breaks the carry chain.  The built-in assembler
                     * therefore accepts ADDS/SUBS without --asm-extra. */
                    if (is_add) {
                        emit_line(&buf, "ADDS r0, r0, r2");
                        emit_line(&buf, "ADC r1, r1, r3");
                    } else if (is_sub) {
                        emit_line(&buf, "SUBS r0, r0, r2");
                        emit_line(&buf, "SBC r1, r1, r3");
                    }
                    if (helper) {
                        asmbuf_puts(&buf, "    BL ");
                        asmbuf_puts(&buf, helper);
                        asmbuf_putc(&buf, '\n');
                    }
                    if (is_mod) {
                        /* __aeabi_ldivmod/uldivmod: quotient in r0/r1, remainder in r2/r3;
                         * take remainder to r0/r1 for MOD result. */
                        emit_line(&buf, "MOV r0, r2");
                        emit_line(&buf, "MOV r1, r3");
                    }
                    /* For DIV, quotient is already in r0/r1. */
                    emit_push_r0(&buf);                 /* result low 32 bits */
                    asmbuf_puts(&buf, "    PUSH {r1}\n"); /* result high 32 bits */
                    break;
                }
                case IR_64CMP: {
                    /* #I3 FIX: Check unsigned flag (0x100) to select __aeabi_ulcmp
                     * for unsigned comparison instead of always using __aeabi_lcmp. */
                    int is_unsigned = (I->aux & 0x100) != 0;
                    int op = I->aux & ~0x100;
                    const char *cmp_helper = is_unsigned ? "__aeabi_ulcmp" : "__aeabi_lcmp";
                    /* __aeabi_lcmp/ulcmp returns -1/0/1 (a<b / a==b / a>b) in r0;
                     * use conditional MOV to produce a 0/1 comparison result. */
                    emit_line(&buf, "POP {r3}");
                    emit_line(&buf, "POP {r2}");
                    emit_line(&buf, "POP {r1}");
                    emit_line(&buf, "POP {r0}");
                    asmbuf_puts(&buf, "    BL ");
                    asmbuf_puts(&buf, cmp_helper);
                    asmbuf_putc(&buf, '\n');
                    emit_line(&buf, "CMP r0, #0");
                    switch (op) {
                        case (int)TOK_EQ: emit_line(&buf, "ITE EQ"); emit_line(&buf, "MOVEQ r0, #1"); emit_line(&buf, "MOVNE r0, #0"); break;
                        case (int)TOK_NE: emit_line(&buf, "ITE NE"); emit_line(&buf, "MOVNE r0, #1"); emit_line(&buf, "MOVEQ r0, #0"); break;
                        case (int)TOK_LT: emit_line(&buf, "ITE LT"); emit_line(&buf, "MOVLT r0, #1"); emit_line(&buf, "MOVGE r0, #0"); break;
                        case (int)TOK_GT: emit_line(&buf, "ITE GT"); emit_line(&buf, "MOVGT r0, #1"); emit_line(&buf, "MOVLE r0, #0"); break;
                        case (int)TOK_LE: emit_line(&buf, "ITE LE"); emit_line(&buf, "MOVLE r0, #1"); emit_line(&buf, "MOVGT r0, #0"); break;
                        case (int)TOK_GE: emit_line(&buf, "ITE GE"); emit_line(&buf, "MOVGE r0, #1"); emit_line(&buf, "MOVLT r0, #0"); break;
                        default: break;
                    }
                    emit_push_r0(&buf);
                    break;
                }
                /* === Memory copy: top of stack is destination address, next is source; BL memcpy === */
                case IR_MEMCPY:
                    emit_pop_r0(&buf);                 /* dst */
                    emit_pop_r1(&buf);                 /* src */
                    emit_mov_imm(&buf, 2, I->val);     /* n bytes */
                    emit_line(&buf, "BL __aeabi_memcpy");
                    break;
                /* === Bitfield store/load ===
                 * val=bit offset, aux=bit width. mask = (1<<bw)-1. */
                case IR_STORE_FLD: {
                    int bit_off = I->val;
                    int bw = I->aux;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    emit_pop_r0(&buf);                 /* value */
                    emit_pop_r1(&buf);                 /* addr */
                    emit_mov_imm(&buf, 2, (int)mask);  /* r2 = mask */
                    emit_line(&buf, "AND r0, r2");      /* truncate value to bw bits */
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, "    LSL r0, r0, #");
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, "    LSL r2, r2, #");
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                    }
                    emit_line(&buf, "LDR r3, [r1]");
                    emit_line(&buf, "BIC r3, r2");      /* clear old bits */
                    emit_line(&buf, "ORR r3, r0");      /* set new bits */
                    emit_line(&buf, "STR r3, [r1]");
                    break;
                }
                case IR_LOAD_FLD: {
                    int bit_off = I->val;
                    int bw = I->aux & 0xFF;         /* #I6 FIX: mask off 0x100 signed flag */
                    int is_signed = (I->aux & 0x100) != 0;
                    unsigned mask = (bw >= 32) ? 0xFFFFFFFFu : ((1u << bw) - 1u);
                    emit_pop_r0(&buf);                 /* addr */
                    emit_line(&buf, "LDR r0, [r0]");
                    if (bit_off > 0) {
                        asmbuf_puts(&buf, "    LSR r0, r0, #");
                        asmbuf_int(&buf, bit_off);
                        asmbuf_putc(&buf, '\n');
                    }
                    if (is_signed && bw < 32) {
                        /* #I6 FIX: Sign-extend signed bitfield using LSL+ASR.
                         * A 3-bit signed field with value 0x7 should become
                         * 0xFFFFFFFF (-1), not 0x00000007 (7). */
                        int shift = 32 - bw;
                        asmbuf_puts(&buf, "    LSL r0, r0, #");
                        asmbuf_int(&buf, shift);
                        asmbuf_putc(&buf, '\n');
                        asmbuf_puts(&buf, "    ASR r0, r0, #");
                        asmbuf_int(&buf, shift);
                        asmbuf_putc(&buf, '\n');
                    } else {
                        emit_mov_imm(&buf, 2, (int)mask);
                        emit_line(&buf, "AND r0, r2");      /* truncate to bw bits */
                    }
                    emit_push_r0(&buf);
                    break;
                }
                case IR_RETURN:
                    if (is_sret && rtop_ret) {
                        /* The sret path pops the source address itself, so the
                         * register copy has to go back on the stack first, and
                         * the epilogue must pop it like it always did. */
                        emit_push_r0(&buf);
                        rtop_ret = 0;
                    }
                    if (is_sret) {
                        /* Struct return > 8 bytes: pop source address (r1),
                         * load sret pointer from frame, copy struct word-by-word,
                         * then return r0 = sret pointer per AAPCS. */
                        int rsz = type_size(f->ret_type);
                        int words = (rsz + 3) / 4;
                        int w;
                        emit_pop_r1(&buf);  /* r1 = source struct address */
                        /* Load sret pointer into r0 */
                        asmbuf_puts(&buf, "    LDR r0, [r6, #");
                        asmbuf_int(&buf, sret_off);
                        asmbuf_puts(&buf, "]\n");
                        /* Copy struct word by word */
                        for (w = 0; w < words; w++) {
                            asmbuf_puts(&buf, "    LDR r2, [r1, #");
                            asmbuf_int(&buf, w * 4);
                            asmbuf_puts(&buf, "]\n");
                            asmbuf_puts(&buf, "    STR r2, [r0, #");
                            asmbuf_int(&buf, w * 4);
                            asmbuf_puts(&buf, "]\n");
                        }
                        /* Manual epilogue: skip POP {r0} (r0 already holds sret ptr) */
                        emit_line(&buf, "MOV sp, r6");
                        /* Same split as the shared epilogue: this manual one
                         * was missed when emit_sp_adjust() was introduced, and
                         * src/lexer.c (frame 4256) then failed to assemble with
                         * "ADD immediate is out of range". */
                        if (local_size > 0) emit_sp_adjust(&buf, local_size, 0);
                        if (g_va_epilogue_pad) {
                            /* Variadic: drop the pushed argument words after
                             * the saved registers (same shape as the shared
                             * epilogue, same count). */
                            emit_line(&buf, "POP {r5, r6, r7, lr}");
                            emit_add_sp(&buf, g_va_epilogue_pad);
                            emit_line(&buf, "BX lr");
                        } else if (f->is_irq) {
                            emit_line(&buf, "POP {r5, r6, r7, pc}");
                        } else {
                            emit_line(&buf, "POP {r5, r6, r7, pc}");
                        }
                    } else {
                        /* 64-bit integer return value: AAPCS delivers it in
                         * r0:r1 (low:high).  irgen pushed the halves low first,
                         * so the high half has to come back into r1 before the
                         * epilogue pops the low half into r0 -- without this the
                         * caller reads r0 = high half and r1 = stale garbage
                         * (measured: `long long f(long long a){return a;}`
                         * returned 0 for f(7) instead of 7). */
                        if (ret_is_two_words(f->ret_type) && !f->is_naked) {
                            if (rtop_ret) {          /* not modeled for 64-bit */
                                emit_push_r0(&buf);
                                rtop_ret = 0;
                            }
                            emit_pop_r1(&buf);
                        }
                        emit_epilogue_return(&buf, local_size, f->is_irq, f->is_naked,
                                             rtop_ret);
                    }
                    break;
                case IR_ALLOCA: {
                    /* Bug S6 FIX (verified): VLA dynamic stack allocation.
                     * Pop byte count, align UP to 8 bytes (ADD+BIC rounds up,
                     * not down), subtract from sp, push the start address.
                     * irgen emits exactly: PUSH(size_expr); PUSH_INT(elem_sz);
                     * BINARY(MUL); IR_ALLOCA; IR_STORE. The ALLOCA pops 1 value
                     * (byte count) and pushes 1 value (start address) - balanced.
                     * The previously-reported extra PUSH {r0} + ADD sp,#4 does
                     * not exist in the current code; the bug was already fixed. */
                    emit_pop_r0(&buf);                  /* r0 = byte count */
                    emit_line(&buf, "ADD r0, r0, #7");
                    emit_line(&buf, "BIC r0, r0, #7");  /* align up to 8 (clear low 3 bits) */
                    emit_line(&buf, "SUB sp, sp, r0");  /* allocate on stack */
                    emit_line(&buf, "MOV r0, sp");      /* r0 = start address */
                    emit_push_r0(&buf);                 /* push start address */
                    break;
                }
                /* === 0.7.0: SIMD parallel ops (Cortex-M4/M7 DSP instructions) ===
                 * Stack semantics: pop r1 (right operand), pop r0 (left operand),
                 * perform the parallel op and push r0 back. SADD16/SSUB16/SADD8/SSUB8
                 * are ARMv6TE+ DSP extension instructions that add/subtract multiple
                 * signed sub-words within a 32-bit register in parallel. */
                case IR_SIMD_ADD16:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SADD16 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                case IR_SIMD_SUB16:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SSUB16 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                case IR_SIMD_ADD8:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SADD8 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                case IR_SIMD_SUB8:
                    emit_pop_r1(&buf);
                    emit_pop_r0(&buf);
                    emit_line(&buf, "SSUB8 r0, r0, r1");
                    emit_push_r0(&buf);
                    break;
                default:
                    asmbuf_puts(&buf, "    ; unknown ir op ");
                    asmbuf_int(&buf, (int)I->op);
                    asmbuf_putc(&buf, '\n');
                    break;
            }
        }
#if ARKCC_FEATURE_DWARF
        if (dbg) {
            emit_line(&buf, ".cfi_endproc");
            asmbuf_puts(&buf, ".size ");
            asmbuf_puts(&buf, f->name);
            asmbuf_puts(&buf, ", .-");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
        }
#endif
        }
        asmbuf_putc(&buf, '\n');
#if ARKCC_FEATURE_OPT_REGALLOC
        /* The text of this function is complete: it is the last slice of the
         * buffer, so the rewrite sees exactly this function and nothing else. */
        ark_regalloc_function(&buf, fstart, f, local_size);
#endif
        /* Correctness, not an option: a frame slot beyond 4095 bytes cannot be
         * reached by "[r6, #off]" at all. */
        ark_fix_far_offset(&buf, fstart);

        /* __attribute__((constructor/destructor)): register the function address in .init_array/.fini_array;
         * the C runtime startup code iterates and calls them before/after main.
         * armcc/MDK uses .init_array (not .ctors), consistent with ARMCC5. */
        if (f->is_constructor && !f->alias_target) {
            asmbuf_puts(&buf, ".section .init_array, \"aw\"\n");
            asmbuf_puts(&buf, ".align 2\n");
            asmbuf_puts(&buf, ".word ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
            asmbuf_puts(&buf, ".text\n");
        }
        if (f->is_destructor && !f->alias_target) {
            asmbuf_puts(&buf, ".section .fini_array, \"aw\"\n");
            asmbuf_puts(&buf, ".align 2\n");
            asmbuf_puts(&buf, ".word ");
            asmbuf_puts(&buf, f->name);
            asmbuf_putc(&buf, '\n');
            asmbuf_puts(&buf, ".text\n");
        }
    }

    /* F3/S5: Generate __arkcc_init_globals function that initializes .data globals
     * and zeros .bss at startup. The Reset_Handler should call this before main().
     * This replaces the need for a separate .data section in Flash and a linker. */
    if (g_data_sram_start != g_data_sram_end || g_bss_sram_start != g_bss_sram_end) {
        int gi;
        asmbuf_puts(&buf, ".global __arkcc_init_globals\n");
        asmbuf_puts(&buf, ".thumb_func\n");
        asmbuf_puts(&buf, "__arkcc_init_globals:\n");
        asmbuf_puts(&buf, "    PUSH {lr}\n");

        /* Initialize .data globals with their initial values */
        for (gi = 0; gi < prog->global_count; gi++) {
            IrGlobal *g = prog->globals[gi];
            unsigned addr;
            int sz;
            if (g->is_extern || g->has_at) continue;
            if (!g->has_init) continue;
            /* Skip zero-initialized scalars (they're in .bss, already zeroed) */
            if (!g->init_aggregate && !g->init_str && !g->is_float && g->init_int == 0) continue;
            addr = gvar_find(g->name);
            if (addr == 0) continue;
            sz = g->type ? type_size(g->type) : 4;
            if (sz <= 0) sz = 4;

            if (g->is_float) {
                /* Float/double initialization: convert to bit pattern */
                union { float f; unsigned int u; } fu;
                union { double d; unsigned int u[2]; } du;
                int is_double = (g->type && g->type->kind == TYPE_DOUBLE);
                if (is_double) {
                    du.d = g->init_float;
                    emit_mov_imm(&buf, 0, (int)du.u[0]);
                    emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                    emit_mov_imm(&buf, 0, (int)du.u[1]);
                    asmbuf_puts(&buf, "    STR r0, [r1, #4]\n");
                } else {
                    fu.f = (float)g->init_float;
                    emit_mov_imm(&buf, 0, (int)fu.u);
                    emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                }
            } else if (g->init_aggregate) {
                /* Aggregate initialiser: turn the text into bytes with the
                 * shared type-driven filler (src/agginit.c), then store them
                 * into the object.  The old code walked the text by counting
                 * commas and skipped string elements as zero, so a char array
                 * member came out full of NULs; anything else it could not
                 * resolve it also wrote as zero without a word of warning. */
                unsigned char *bytes = (unsigned char *)mem_pool_alloc(sz);
                int bad = 0, k;
                if (!bytes) bad = 1;
                else {
                    for (k = 0; k < sz; k++) bytes[k] = 0;
                    if (ark_agg_fill(g->type, g->init_aggregate, bytes, sz) != 0)
                        bad = 1;
                }
                if (bad) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "the arm backend cannot turn the initialiser of "
                              "the global '%s' into data; only integer "
                              "constants in arrays and structures, string "
                              "literals for char arrays, and a string literal "
                              "for a pointer are implemented",
                              g->name, 0,0,0,0,0);
                    continue;
                }
                /* Word stores on word-aligned runs, byte stores elsewhere.
                 * Every word is written, zeros included: the .data area in
                 * SRAM is not zeroed for us (only .bss is). */
                emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                for (k = 0; k < sz; ) {
                    if (((k & 3) == 0) && k + 4 <= sz) {
                        int w = (int)((unsigned)bytes[k]
                                    | ((unsigned)bytes[k + 1] << 8)
                                    | ((unsigned)bytes[k + 2] << 16)
                                    | ((unsigned)bytes[k + 3] << 24));
                        emit_mov_imm(&buf, 0, w);
                        asmbuf_puts(&buf, "    STR r0, [r1");
                        if (k != 0) {
                            asmbuf_puts(&buf, ", #");
                            asmbuf_int(&buf, k);
                        }
                        asmbuf_puts(&buf, "]\n");
                        k += 4;
                    } else {
                        emit_mov_imm(&buf, 0, (int)bytes[k]);
                        asmbuf_puts(&buf, "    STRB r0, [r1, #");
                        asmbuf_int(&buf, k);
                        asmbuf_puts(&buf, "]\n");
                        k += 1;
                    }
                }
            } else if (g->init_str && g->type && g->type->kind == TYPE_PTR) {
                /* Pointer initialised from a string literal: the bytes live
                 * in flash (see the .L_gstr_ block above), the object holds
                 * their address.  Writing the characters into the pointer
                 * was a silent wild pointer. */
                asmbuf_puts(&buf, "    LDR r0, =.L_gstr_");
                asmbuf_int(&buf, gi);
                asmbuf_putc(&buf, '\n');
                emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                asmbuf_puts(&buf, "    STR r0, [r1]\n");
            } else if (g->init_str) {
                /* String initializer for char array: emit byte-by-byte */
                int si;
                for (si = 0; si < sz && g->init_str[si]; si++) {
                    emit_mov_imm(&buf, 0, (int)(unsigned char)g->init_str[si]);
                    emit_mov_addr32(&buf, 1, addr + (unsigned)si, addr_in_own_data(addr));
                    asmbuf_puts(&buf, "    STRB r0, [r1]\n");
                }
            } else {
                /* Integer scalar initialization */
                emit_mov_imm(&buf, 0, (int)g->init_int);
                emit_mov_addr32(&buf, 1, addr, addr_in_own_data(addr));
                if (sz == 1) {
                    asmbuf_puts(&buf, "    STRB r0, [r1]\n");
                } else if (sz == 2) {
                    asmbuf_puts(&buf, "    STRH r0, [r1]\n");
                } else if (sz >= 8) {
                    /* 64-bit: store low and high words (high=0 for now) */
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                    emit_mov_imm(&buf, 0, (int)(g->init_int >> 32));
                    asmbuf_puts(&buf, "    STR r0, [r1, #4]\n");
                } else {
                    asmbuf_puts(&buf, "    STR r0, [r1]\n");
                }
            }
        }

        /* Zero .bss region */
        if (g_bss_sram_start != g_bss_sram_end) {
            emit_mov_addr32(&buf, 0, g_bss_sram_start, 1);
            emit_mov_addr32(&buf, 1, g_bss_sram_end, 1);
            asmbuf_puts(&buf, "    CMP r0, r1\n");
            asmbuf_puts(&buf, "    BEQ .L_bss_done\n");
            asmbuf_puts(&buf, "    MOV r2, #0\n");
            asmbuf_puts(&buf, ".L_bss_loop:\n");
            asmbuf_puts(&buf, "    STR r2, [r0]\n");
            asmbuf_puts(&buf, "    ADD r0, r0, #4\n");
            asmbuf_puts(&buf, "    CMP r0, r1\n");
            asmbuf_puts(&buf, "    BNE .L_bss_loop\n");
            asmbuf_puts(&buf, ".L_bss_done:\n");
        }

        /* Bug ROUND4-P3 FIX: Use POP {pc} for return instead of POP {r0}; BX r0.
         * In ARM Thumb mode, POP {pc} automatically sets the Thumb bit from
         * bit 0 of the stored LR (which is always 1 for Thumb code), so this
         * is the standard, more efficient epilogue pattern. */
        asmbuf_puts(&buf, "    POP {pc}\n\n");
    }

    /* AEABI run-time helpers.
     *
     * This backend used to emit its own implementations of the AEABI
     * soft-float and 64-bit integer helpers (__aeabi_ladd ... __aeabi_llsl,
     * __aeabi_uldivmod, __aeabi_ldivmod, __aeabi_fadd ... __aeabi_fdiv,
     * __aeabi_f2iz, __aeabi_fcmpeq, the whole d-family).  Running them
     * against the host oracle showed several of them are simply wrong:
     *
     *   __aeabi_f2iz     (int)2.0f                        -> 0    (want 2)
     *   __aeabi_fdiv     4.0f/2.0f == 2.0f                -> false
     *   __aeabi_uldivmod 1000ull/7ull                     -> wrong quotient
     *   __aeabi_ldivmod  1000000ll/1000ll & 127           -> 125  (want 104)
     *   __aeabi_dadd     (a+b)*(a-b)/(b+1) for (3.5,1.5)   -> 25   (want 144)
     *
     * Each of those is a silent wrong answer, the one outcome this compiler
     * refuses to produce.  The AEABI defines these symbols as part of the
     * *target run-time library*, and every real toolchain carries them:
     * arm-none-eabi-gcc ships libgcc.a, armcc ships armlib.  So they are no
     * longer emitted here.  An image that uses floating point, 64-bit
     * division or 64-bit <-> float conversion must link that library; one
     * that does not fails at link time with "undefined reference to
     * __aeabi_..." instead of computing something false.
     *
     * __aeabi_memcpy stays: no run-time library provides it for a
     * freestanding target, the byte loop below cannot be wrong, and struct
     * assignment / struct-by-value arguments would otherwise need libc. */
    {
        /* __aeabi_memcpy: struct assignment, struct-by-value arguments and
         * array initialisation are all lowered to a `BL memcpy`.  That made
         * every one of them a link-time dependency on the C library, which a
         * freestanding target does not have -- the MiniApp SDK on the board
         * defines no memcpy, so `s = va_arg(ap, struct S)` (and any plain
         * `a = b` for structs) failed to link with "undefined reference to
         * memcpy".  Emitting the routine under the AEABI name it belongs to
         * (r0=dst, r1=src, r2=n, same convention as the real one) removes the
         * dependency and still satisfies a translation unit that only ever
         * sees `BL __aeabi_memcpy`.
         *
         * Byte-at-a-time on purpose: no alignment requirement, no tail, and
         * the assembler only has to encode 16-bit forms.  Structs in this
         * language are small; a word loop would be faster but would need the
         * unaligned-tail path proven first. */
        asmbuf_puts(&buf, ".global __aeabi_memcpy\n.thumb_func\n__aeabi_memcpy:\n");
        asmbuf_puts(&buf, "    PUSH {r4}\n");
        asmbuf_puts(&buf, "    MOV r4, r0\n");
        asmbuf_puts(&buf, "    CMP r2, #0\n");
        asmbuf_puts(&buf, "    BEQ .L_arkcc_mcpy_done\n");
        asmbuf_puts(&buf, ".L_arkcc_mcpy_loop:\n");
        asmbuf_puts(&buf, "    LDRB r3, [r1]\n");
        asmbuf_puts(&buf, "    STRB r3, [r4]\n");
        asmbuf_puts(&buf, "    ADD r4, r4, #1\n");
        asmbuf_puts(&buf, "    ADD r1, r1, #1\n");
        /* SUB + CMP, not `SUBS`: the internal assembler takes SUBS only with
         * --asm-extra, and this routine is emitted for every target build.
         * Measured on the board: `cc --pack` refused the whole unit with
         * "unknown mnemonic \"SUBS\"" -- the host gate assembles with GNU as,
         * which takes it, so this only showed up on the target. */
        asmbuf_puts(&buf, "    SUB r2, r2, #1\n");
        asmbuf_puts(&buf, "    CMP r2, #0\n");
        asmbuf_puts(&buf, "    BNE .L_arkcc_mcpy_loop\n");
        asmbuf_puts(&buf, ".L_arkcc_mcpy_done:\n");
        asmbuf_puts(&buf, "    POP {r4}\n");
        asmbuf_puts(&buf, "    BX lr\n\n");


    }

    /* Globals: SRAM addresses are assigned by gvar_assign() above and loaded
     * via MOVW/MOVT in IR_PUSH_GLOBAL etc. .data globals are initialized at
     * startup by __arkcc_init_globals (generated above). .bss globals are
     * zeroed by the same function. __at variables use their absolute address.
     * Note: .set directives for alias functions are emitted in the function
     * code section above, not here. */
    if (0 && prog->global_count > 0) {
        int gi;
        int cur_sec = 0;  /* 0=.text, 1=.data, 2=.bss, 3=.rodata, 4=custom */
        for (gi = 0; gi < prog->global_count; gi++) {
            IrGlobal *g = prog->globals[gi];
            int sz;
            int want_sec = 0;
            const char *want_custom = 0;
            if (g->is_extern) continue;
            /* __at(addr): define the symbol directly as an absolute address constant (no storage allocated).
             *   .global name
             *   .set name, 0xADDR
             * This way every LDR rN, =name loads the MMIO physical address,
             * and subsequent LDR/STR [rN] accesses the peripheral register. */
            if (g->has_at) {
                if (!g->is_static) {
                    asmbuf_puts(&buf, ".global ");
                    asmbuf_puts(&buf, g->name);
                    asmbuf_putc(&buf, '\n');
                }
                asmbuf_puts(&buf, ".set ");
                asmbuf_puts(&buf, g->name);
                asmbuf_puts(&buf, ", 0x");
                {
                    unsigned a = g->at_addr;
                    char hex[9]; int k;
                    const char *hd = "0123456789abcdef";
                    for (k = 7; k >= 0; k--) { hex[k] = hd[a & 0xF]; a >>= 4; }
                    hex[8] = 0;
                    asmbuf_puts(&buf, hex);
                }
                asmbuf_putc(&buf, '\n');
                continue;
            }
            sz = g->type ? type_size(g->type) : 4;
            if (sz <= 0) sz = 4;
            /* Choose section: custom section > no-init (.bss) > const (.rodata) > .data */
            if (g->section) {
                want_custom = g->section;
            } else if (!g->has_init) {
                want_sec = 2;
            } else if (g->is_const) {
                want_sec = 3;
            } else {
                want_sec = 1;
            }
#if ARKCC_FEATURE_ARMCC_KEYWORDS
            /* __thread thread-local storage: use .tbss (no init) / .tdata (has init) instead.
             * These two sections are instantiated per-thread by the loader in each thread context. */
            if (g->is_tls) {
                if (!g->has_init || g->init_aggregate) {
                    want_sec = 5;  /* .tbss */
                } else {
                    want_sec = 6;  /* .tdata */
                }
            }
#endif
            /* Switch section (only emit section directive if different from current) */
            if (want_custom) {
                asmbuf_puts(&buf, ".section \"");
                asmbuf_puts(&buf, want_custom);
                asmbuf_puts(&buf, "\"");
#if ARKCC_FEATURE_SCATTER
                /* SCT-1: annotate the section's scatter exec region base so
                 * the resolved layout is visible in the emitted assembly. */
                if (g_scatter_loaded) {
                    unsigned sect_base =
                        scatter_get_er_base_for_section(&g_scatter, want_custom);
                    if (sect_base) {
                        char hex[9]; int k;
                        const char *hd = "0123456789ABCDEF";
                        asmbuf_puts(&buf, "  @ base 0x");
                        for (k = 7; k >= 0; k--) { hex[k] = hd[sect_base & 0xF]; sect_base >>= 4; }
                        hex[8] = 0;
                        asmbuf_puts(&buf, hex);
                    }
                }
#endif
                asmbuf_putc(&buf, '\n');
                cur_sec = 4;
            } else if (cur_sec != want_sec) {
                if (want_sec == 1) asmbuf_puts(&buf, ".data\n");
                else if (want_sec == 2) asmbuf_puts(&buf, ".bss\n");
                else if (want_sec == 3) asmbuf_puts(&buf, ".section .rodata\n");
#if ARKCC_FEATURE_ARMCC_KEYWORDS
                else if (want_sec == 5) asmbuf_puts(&buf, ".section .tbss,\"aw\",%nobits\n");
                else if (want_sec == 6) asmbuf_puts(&buf, ".section .tdata,\"aw\",%progbits\n");
#endif
                cur_sec = want_sec;
            }
            /* Symbol attributes + label */
            if (!g->is_static) {
                asmbuf_puts(&buf, ".global ");
                asmbuf_puts(&buf, g->name);
                asmbuf_putc(&buf, '\n');
            }
            /* Alignment: __align(n) / aligned(n) takes priority; otherwise default 4 bytes */
            if (g->align_bytes > 4) {
                asmbuf_puts(&buf, ".balign ");
                asmbuf_int(&buf, g->align_bytes);
                asmbuf_putc(&buf, '\n');
            } else {
                asmbuf_puts(&buf, ".align 4\n");
            }
            asmbuf_puts(&buf, g->name);
            asmbuf_puts(&buf, ":\n");
            /* Data */
            if (!g->has_init || g->init_aggregate) {
                /* Zero-init or aggregate (initialized at runtime): reserve space */
                asmbuf_puts(&buf, "    .zero ");
                asmbuf_int(&buf, sz);
                asmbuf_putc(&buf, '\n');
            } else if (g->is_float) {
                /* Float initialization: convert to single-precision bit pattern (consistent with IR_PUSH_FLOAT) */
                union { float f; unsigned int u; } u;
                unsigned int bits;
                char hex[9];
                int k;
                const char *hd = "0123456789abcdef";
                u.f = (float)g->init_float;
                bits = u.u;
                for (k = 7; k >= 0; k--) { hex[k] = hd[bits & 0xF]; bits >>= 4; }
                hex[8] = 0;
                asmbuf_puts(&buf, "    .word 0x");
                asmbuf_puts(&buf, hex);
                asmbuf_putc(&buf, '\n');
            } else if (g->init_str) {
                /* String array initialization: same escaping as IR_PUSH_STR */
                int slen = ark_strlen(g->init_str);
                int k;
                asmbuf_puts(&buf, "    .asciz \"");
                for (k = 0; k < slen; k++) {
                    char c = g->init_str[k];
                    if (c == '\n') asmbuf_puts(&buf, "\\n");
                    else if (c == '\t') asmbuf_puts(&buf, "\\t");
                    else if (c == '\\') asmbuf_puts(&buf, "\\\\");
                    else if (c == '"')  asmbuf_puts(&buf, "\\\"");
                    else if ((unsigned char)c < 0x20 || (unsigned char)c >= 0x7F) {
                        char hb[8]; int hn = 0;
                        const char *hh = "0123456789abcdef";
                        hb[hn++] = '\\'; hb[hn++] = 'x';
                        hb[hn++] = hh[((unsigned char)c >> 4) & 0xF];
                        hb[hn++] = hh[(unsigned char)c & 0xF];
                        hb[hn] = 0;
                        asmbuf_puts(&buf, hb);
                    } else {
                        asmbuf_putc(&buf, c);
                    }
                }
                asmbuf_puts(&buf, "\"\n");
                /* Pad remaining space with zeros (e.g. char[10] = "hi") */
                if (slen + 1 < sz) {
                    asmbuf_puts(&buf, "    .zero ");
                    asmbuf_int(&buf, sz - (slen + 1));
                    asmbuf_putc(&buf, '\n');
                }
            } else {
                /* Scalar initialization */
                asmbuf_puts(&buf, "    .word ");
                asmbuf_int(&buf, g->init_int);
                asmbuf_putc(&buf, '\n');
                if (sz > 4) {
                    asmbuf_puts(&buf, "    .zero ");
                    asmbuf_int(&buf, sz - 4);
                    asmbuf_putc(&buf, '\n');
                }
            }
        }
        /* Restore .text section for later manual appending */
        asmbuf_puts(&buf, ".text\n\n");
    }
#if ARKCC_FEATURE_DWARF_FULL
    /* DWARF debug info (enabled by --dwarf2/--dwarf3):
     * Emit .debug_info/.debug_abbrev/.debug_line sections recording the compilation unit,
     * function symbols and line number maps, for GDB/LLDB single-stepping. Uses a DWARF v2 subset:
     * only the CU header + per-function DIE + line number program; no variable location info (simplified).
     * Detailed type info references predefined DW_TAGs in .debug_abbrev via DW_FORM_ref4. */
    if (g_config.dwarf_version >= 2) {
        int fi2;
        const char *fname = prog->source_file ? prog->source_file : "input.c";
        /* .debug_abbrev: abbreviation table, defines DIE tag/attribute forms */
        asmbuf_puts(&buf, ".section .debug_abbrev,\"\",%progbits\n");
        asmbuf_puts(&buf, ".balign 4\n");
        asmbuf_puts(&buf, "    .byte 1\n");              /* abbrev code 1 */
        asmbuf_puts(&buf, "    .byte 17\n");             /* DW_TAG_compile_unit */
        asmbuf_puts(&buf, "    .byte 0\n");              /* DW_CHILDREN_yes */
        asmbuf_puts(&buf, "    .byte 3\n  .byte 8\n");   /* DW_AT_name, DW_FORM_string */
        asmbuf_puts(&buf, "    .byte 16\n  .byte 6\n");  /* DW_AT_stmt_list, DW_FORM_data4 */
        asmbuf_puts(&buf, "    .byte 0\n  .byte 0\n");   /* end attr/end form */
        asmbuf_puts(&buf, "    .byte 2\n");              /* abbrev code 2 */
        asmbuf_puts(&buf, "    .byte 46\n");             /* DW_TAG_subprogram */
        asmbuf_puts(&buf, "    .byte 0\n");              /* DW_CHILDREN_no */
        asmbuf_puts(&buf, "    .byte 3\n  .byte 8\n");   /* DW_AT_name, DW_FORM_string */
        asmbuf_puts(&buf, "    .byte 17\n  .byte 1\n");  /* DW_AT_low_pc, DW_FORM_addr */
        asmbuf_puts(&buf, "    .byte 18\n  .byte 1\n");  /* DW_AT_high_pc, DW_FORM_addr */
        asmbuf_puts(&buf, "    .byte 0\n  .byte 0\n");   /* end attr/end form */
        asmbuf_puts(&buf, "    .byte 0\n");              /* end abbrev table */
        /* .debug_info: compilation unit + per-function DIE */
        asmbuf_puts(&buf, ".section .debug_info,\"\",%progbits\n");
        asmbuf_puts(&buf, ".balign 4\n");
        asmbuf_puts(&buf, "    .word 0\n");              /* unit_length (placeholder, simplified) */
        asmbuf_puts(&buf, "    .half 2\n");              /* DWARF version 2 */
        asmbuf_puts(&buf, "    .word 0\n");              /* debug_abbrev_offset */
        asmbuf_puts(&buf, "    .byte 4\n");              /* address_size */
        asmbuf_puts(&buf, "    .byte 1\n");              /* abbrev 1 = compile_unit */
        asmbuf_puts(&buf, "    .asciz \"");
        asmbuf_puts(&buf, fname);
        asmbuf_puts(&buf, "\"\n");
        asmbuf_puts(&buf, "    .word 0\n");              /* stmt_list offset */
        for (fi2 = 0; fi2 < prog->func_count; fi2++) {
            IrFunc *f2 = prog->funcs[fi2];
            if (f2->alias_target) continue;
            asmbuf_puts(&buf, "    .byte 2\n");          /* abbrev 2 = subprogram */
            asmbuf_puts(&buf, "    .asciz \"");
            asmbuf_puts(&buf, f2->name);
            asmbuf_puts(&buf, "\"\n");
            asmbuf_puts(&buf, "    .word ");
            asmbuf_puts(&buf, f2->name);
            asmbuf_putc(&buf, '\n');                     /* low_pc */
            asmbuf_puts(&buf, "    .word ");
            asmbuf_puts(&buf, f2->name);
            asmbuf_puts(&buf, "_end\n");                 /* high_pc */
        }
        asmbuf_puts(&buf, "    .byte 0\n");              /* end DIE list */
        /* .debug_line: line number program (file name table + end entry only) */
        asmbuf_puts(&buf, ".section .debug_line,\"\",%progbits\n");
        asmbuf_puts(&buf, ".balign 4\n");
        asmbuf_puts(&buf, "    .word 0\n");              /* unit_length (placeholder) */
        asmbuf_puts(&buf, "    .half 2\n");              /* DWARF version 2 */
        asmbuf_puts(&buf, "    .word 0\n");              /* header_length */
        asmbuf_puts(&buf, "    .byte 4\n");              /* minimum_instruction_length */
        asmbuf_puts(&buf, "    .byte 0\n");              /* default_is_stmt */
        asmbuf_puts(&buf, "    .byte 1\n");              /* line_base */
        asmbuf_puts(&buf, "    .byte 1\n");              /* line_range */
        asmbuf_puts(&buf, "    .byte 13\n");             /* opcode_base */
        asmbuf_puts(&buf, "    .byte 0,1,1,0,1,0,0,1,1,1,1,1\n"); /* standard_opcode_lengths */
        asmbuf_puts(&buf, "    .asciz \"");
        asmbuf_puts(&buf, fname);
        asmbuf_puts(&buf, "\"\n");                       /* include_directories */
        asmbuf_puts(&buf, "    .byte 0\n");
        asmbuf_puts(&buf, "    .asciz \"");
        asmbuf_puts(&buf, fname);
        asmbuf_puts(&buf, "\"\n");                       /* file_names */
        asmbuf_puts(&buf, "    .byte 0,0,0\n");
        asmbuf_puts(&buf, "    .byte 0\n");              /* end file table */
        asmbuf_puts(&buf, "    .byte 0,9,2\n");          /* DW_LNE_end_sequence */
        asmbuf_puts(&buf, ".text\n");
    }
#endif
#if ARKCC_FEATURE_RT_LAZY
    /* --rt-lazy：只保留被引用到的 __aeabi_* 辅助函数。默认关闭时这段完全不执行，
     * 产物与旧版本逐字节相同。 */
    if (g_config.enable_rt_lazy && buf.len > 0) {
        buf.len = rt_prune_asm_text(buf.buf, buf.len);
    }
#endif
    return buf.buf;
}

/* === Minimal Thumb-2 text assembler === */
typedef struct AsmLabel {
    char *name;
    int   addr;
    int   is_weak;
    int   is_func;   /* defined in .text with a non-local name: its address
                      * is a Thumb function pointer, so LDR rd,=name must set
                      * bit 0.  Data and .L labels keep it clear. */
} AsmLabel;
/* --reloc-text: relocation type for a MOVW/MOVT pair that carries an address
 * inside this object's own code/rodata window (a string literal, usually).
 * Unlike types 1..4 this entry names no symbol: the linker adds
 * (the base it links this object at - the base this object was compiled
 * against), and it learns the latter from the __arkcc_text_base symbol the
 * object carries.  12 is the next free number after .arkdata (6), ADR (7),
 * CBZ/CBNZ (8, 9) and the cross-object LDR= sites (10 data / 11 function). */
#define ARM_RELOC_TEXT       12
#define ARM_RELOC_TEXT_NAME  ".arktext"

typedef struct AsmRef   { int pc; char *name; int type; } AsmRef;
/* Literal pool entry: LDR rd,=label at ldr_pc needs a 4-byte literal at pool_offset */
typedef struct AsmLiteral { int ldr_pc; char *name; int pool_offset; } AsmLiteral;

typedef struct {
    const char    *src;
    unsigned char *out;
    int            pc;
    int            out_cap;
    AsmLabel      *labels;
    int            label_count, label_cap;
    AsmRef        *refs;
    int            ref_count, ref_cap;
    int            error_count;
    /* .set symbols (absolute addresses, e.g. __at variables, alias functions) */
    AsmLabel      *sets;
    int            set_count, set_cap;
    /* Literal pool for LDR rd,=label */
    /* Genuine assembly errors (unknown mnemonic, unresolvable internal label).
     * Kept separate from error_count, which also counts unresolved external
     * references that the linker may still satisfy. */
    int            hard_errors;
    AsmLiteral    *literals;
    int            literal_count, literal_cap;
    int            literal_pool_start;  /* pc where the pool begins (-1 = not yet) */
    /* .weak directive sets pending_weak; the next label defined inherits it. */
    int            pending_weak;
    /* Current section: 0=.text, 1=.data, 2=.bss, 3=.init_array, 4=.fini_array */
    int            cur_section;
    /* 1 while pass2_a runs.  Register-list diagnostics are reported
     * there only: pass 1 just measures instruction sizes, and reporting
     * from both passes would print every error twice. */
    int            pass2;
} ArmAsm;

static int is_alpha_a(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.';
}
static int is_alnum_a(char c) {
    return is_alpha_a(c) || (c >= '0' && c <= '9');
}
static int is_space_a(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static int my_toupper_a(int c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}
static int strieq_a(const char *a, const char *b) {
    while (*a && *b) {
        if (my_toupper_a(*a) != my_toupper_a(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}
static int starts_with_a(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return 0;
        s++; prefix++;
    }
    return 1;
}
static void skip_space_a(const char **p) { while (is_space_a(**p)) (*p)++; }
static void skip_to_eol_a(const char **p) { while (**p && **p != '\n') (*p)++; }

static int read_word_a(const char **p, char *out, int cap) {
    int n = 0;
    skip_space_a(p);
    if (**p == '.') {
        /* Directive (e.g. ".text", ".global", ".L1"): include '.' in word */
        while (**p && is_alnum_a(**p) && n < cap - 1) {
            out[n++] = **p; (*p)++;
        }
    } else {
        /* Instruction/label (e.g. "VADD", "add_float"): stop at '.'
         * so VFP suffixes like ".F32" are NOT consumed as part of the
         * mnemonic.  Without this, "VADD.F32" is read as one word and
         * strieq_a(first,"VADD") fails, causing all VFP arithmetic
         * instructions to fall through to NOP. */
        while (**p && is_alnum_a(**p) && **p != '.' && n < cap - 1) {
            out[n++] = **p; (*p)++;
        }
    }
    out[n] = 0;
    return n;
}

static int parse_reg_a(const char **p) {
    int n = 0;
    skip_space_a(p);
    if ((**p == 'r' || **p == 'R')) {
        (*p)++;
        while (**p >= '0' && **p <= '9') {
            n = n * 10 + (**p - '0');
            (*p)++;
        }
        return n;
    }
    if ((**p == 's' || **p == 'S') && ((*p)[1] == 'p' || (*p)[1] == 'P')) { (*p) += 2; return 13; }
    if ((**p == 'l' || **p == 'L') && ((*p)[1] == 'r' || (*p)[1] == 'R')) { (*p) += 2; return 14; }
    if ((**p == 'p' || **p == 'P') && ((*p)[1] == 'c' || (*p)[1] == 'C')) { (*p) += 2; return 15; }
    if ((**p == 'i' || **p == 'I') && ((*p)[1] == 'p' || (*p)[1] == 'P')) { (*p) += 2; return 12; }
    if ((**p == 'f' || **p == 'F') && ((*p)[1] == 'p' || (*p)[1] == 'P')) { (*p) += 2; return 11; }
    return -1;
}

/* #1 FIX: Parse VFP single-precision register (s0-s31).
 * Returns register number 0-31, or -1 if not an S register.
 * Must not match 'sp' (handled by parse_reg_a). */
static int parse_vfp_sreg_a(const char **p) {
    int n = 0;
    skip_space_a(p);
    if ((**p == 's' || **p == 'S') &&
        ((*p)[1] >= '0' && (*p)[1] <= '9')) {
        (*p)++;
        while (**p >= '0' && **p <= '9') {
            n = n * 10 + (**p - '0');
            (*p)++;
        }
        if (n > 31) n = 31;
        return n;
    }
    return -1;
}

/* #1 FIX: Parse VFP double-precision register (d0-d15).
 * Returns register number 0-15, or -1 if not a D register. */
static int parse_vfp_dreg_a(const char **p) {
    int n = 0;
    skip_space_a(p);
    if (**p == 'd' || **p == 'D') {
        (*p)++;
        while (**p >= '0' && **p <= '9') {
            n = n * 10 + (**p - '0');
            (*p)++;
        }
        if (n > 15) n = 15;
        return n;
    }
    return -1;
}

/* #1 FIX: Read VFP instruction suffix (e.g. ".F32", ".F64", ".S32.F32").
 * Concatenates all suffix segments into out (e.g. "F32", "S32F32").
 * Advances p past the suffix. */
static void read_vfp_suffix_a(const char **p, char *out, int cap) {
    int n = 0;
    skip_space_a(p);
    while (**p == '.' && n < cap - 1) {
        (*p)++;
        /* Inner loop: read alnum but NOT '.' (dots are handled by outer loop).
         * Without excluding '.', is_alnum_a('.')==true would consume the
         * entire ".F32.S32" as one segment "F32.S32" (with embedded dot),
         * causing strieq_a(suf,"F32S32") to fail. */
        while (is_alnum_a(**p) && **p != '.' && n < cap - 1) {
            out[n++] = **p;
            (*p)++;
        }
    }
    out[n] = 0;
}


/* FIX(vfp-encode): Split an S/D register number into the two fields the VFP
 * encoders actually use.
 *
 * The classic ARM ARM shorthand "Sd = D:Vd" reads as if a single-precision
 * register number were (16 * half_bit + 4-bit field); that is what this file
 * originally assumed.  Measured against armasm 5.06
 * (--cpu Cortex-M7 --fpu=FPv5_D16, see h7test/v3.lst / v4.lst) the field holds
 * reg[4:1] and the separate "half" bit holds reg[0]:
 *     VADD.F32 S0, S0, S16 -> 0xEE300A08   (field 8 = 16>>1, half 0 = 16&1)
 *     VADD.F32 S0, S0, S1  -> 0xEE300A20   (field 0 =  1>>1, half 1 =  1&1)
 *     VADD.F32 S1, S2, S3  -> 0xEE710A21   (every field shifted the same way)
 * D0-D15 put the whole number in the 4-bit field and leave the half bit clear.
 * The half bit sits at bit22 for Vd, bit7 for Vn and bit5 for Vm. */
static unsigned ark_vfp_nib(int reg, int is_dbl) {
    if (reg < 0) reg = 0;
    return is_dbl ? (unsigned)(reg & 0xF) : (unsigned)((reg >> 1) & 0xF);
}
static unsigned ark_vfp_half(int reg, int is_dbl) {
    if (reg < 0) reg = 0;
    return is_dbl ? (unsigned)((reg >> 4) & 1) : (unsigned)(reg & 1);
}
static int parse_imm_a(const char **p) {
    int neg = 0;
    int v = 0;
    skip_space_a(p);
    if (**p != '#') return 0;
    (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        (*p) += 2;
        for (;;) {
            char c = **p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d;
            (*p)++;
        }
    } else {
        while (**p >= '0' && **p <= '9') {
            v = v * 10 + (**p - '0');
            (*p)++;
        }
    }
    return neg ? -v : v;
}

/* Immediate operand where the '#' is optional (armasm accepts both
 * "SVC 0xAB" and "SVC #0xAB"). Returns 0 and clears *ok when no number is
 * present: parse_imm_a() would return 0 in that case as well, which turns a
 * mangled operand into a silent SVC #0. */
static int parse_imm_opt_hash_a(const char **p, int *ok) {
    int neg = 0;
    int v = 0;
    int digits = 0;
    skip_space_a(p);
    if (**p == '#') { (*p)++; skip_space_a(p); }
    if (**p == '-') { neg = 1; (*p)++; }
    if (**p == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        (*p) += 2;
        for (;;) {
            char c = **p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * 16 + d;
            digits++;
            (*p)++;
        }
    } else {
        while (**p >= '0' && **p <= '9') {
            v = v * 10 + (**p - '0');
            digits++;
            (*p)++;
        }
    }
    *ok = (digits > 0);
    return neg ? -v : v;
}

static void eat_comma_a(const char **p) {
    skip_space_a(p);
    if (**p == ',') (*p)++;
    skip_space_a(p);
}

static void out16(ArmAsm *a, unsigned v) {
    if (a->pc + 2 > a->out_cap) { a->error_count++; return; }
    a->out[a->pc++] = (unsigned char)(v & 0xFF);
    a->out[a->pc++] = (unsigned char)((v >> 8) & 0xFF);
}

static void add_label_a(ArmAsm *a, const char *name, int addr) {
    if (a->label_count >= a->label_cap) {
        int nc = a->label_cap == 0 ? 32 : a->label_cap * 2;
        a->labels = (AsmLabel *)mem_realloc(a->labels,
            a->label_cap * (int)sizeof(AsmLabel),
            nc * (int)sizeof(AsmLabel));
        a->label_cap = nc;
    }
    a->labels[a->label_count].name = mem_strdup(name);
    a->labels[a->label_count].addr = addr;
    a->labels[a->label_count].is_weak = a->pending_weak;
    a->labels[a->label_count].is_func =
        (a->cur_section == 0 && !(name[0] == '.' && name[1] == 'L')) ? 1 : 0;
    a->pending_weak = 0;
    a->label_count++;
}
static int find_label_a(ArmAsm *a, const char *name) {
    int i;
    for (i = 0; i < a->label_count; i++)
        if (ark_streq(a->labels[i].name, name)) return a->labels[i].addr;
    return -1;
}

/* 1 if `name` is a function label (see AsmLabel.is_func). */
static int label_is_func_a(ArmAsm *a, const char *name) {
    int i;
    for (i = 0; i < a->label_count; i++)
        if (ark_streq(a->labels[i].name, name)) return a->labels[i].is_func;
    return 0;
}
static void add_ref_a(ArmAsm *a, int pc, const char *name, int type) {
    if (a->ref_count >= a->ref_cap) {
        int nc = a->ref_cap == 0 ? 32 : a->ref_cap * 2;
        a->refs = (AsmRef *)mem_realloc(a->refs,
            a->ref_cap * (int)sizeof(AsmRef),
            nc * (int)sizeof(AsmRef));
        a->ref_cap = nc;
    }
    a->refs[a->ref_count].pc = pc;
    a->refs[a->ref_count].name = mem_strdup(name);
    a->refs[a->ref_count].type = type;
    a->ref_count++;
}

/* .set symbol management (absolute addresses) */
static void add_set_a(ArmAsm *a, const char *name, int addr) {
    if (a->set_count >= a->set_cap) {
        int nc = a->set_cap == 0 ? 32 : a->set_cap * 2;
        a->sets = (AsmLabel *)mem_realloc(a->sets,
            a->set_cap * (int)sizeof(AsmLabel),
            nc * (int)sizeof(AsmLabel));
        a->set_cap = nc;
    }
    a->sets[a->set_count].name = mem_strdup(name);
    a->sets[a->set_count].addr = addr;
    a->set_count++;
}
static int find_set_a(ArmAsm *a, const char *name) {
    int i;
    for (i = 0; i < a->set_count; i++)
        if (ark_streq(a->sets[i].name, name)) return a->sets[i].addr;
    return -1;
}

/* Literal pool management */
static void add_literal_a(ArmAsm *a, int ldr_pc, const char *name) {
    if (a->literal_count >= a->literal_cap) {
        int nc = a->literal_cap == 0 ? 32 : a->literal_cap * 2;
        a->literals = (AsmLiteral *)mem_realloc(a->literals,
            a->literal_cap * (int)sizeof(AsmLiteral),
            nc * (int)sizeof(AsmLiteral));
        a->literal_cap = nc;
    }
    a->literals[a->literal_count].ldr_pc = ldr_pc;
    a->literals[a->literal_count].name = mem_strdup(name);
    a->literals[a->literal_count].pool_offset = -1;
    a->literal_count++;
}

/* Resolve a symbol: try .set first, then code labels */
static int resolve_symbol_a(ArmAsm *a, const char *name) {
    int addr = find_set_a(a, name);
    if (addr >= 0) return addr;
    addr = find_label_a(a, name);
    return addr;  /* may be -1 if unresolved */
}

/* Determine instruction size for LDR/STR/LDRB/STRB/LDRH/STRH/LDRSH/LDRSB
 * with immediate offset. Returns 2 (T1 16-bit) or 4 (T3 32-bit).
 * T1 limits: word imm5=(off>>2)&0x1F (max 124, step 4),
 *            byte imm5=off&0x1F (max 31),
 *            half imm5=(off>>1)&0x1F (max 62, step 2).
 * T3 supports imm12=off (max 4095 bytes). */
static int ldr_str_size_a(int imm, int is_byte, int is_half) {
    if (imm < 0) return 4;  /* negative offset always needs T3 */
    if (is_byte) {
        if (imm <= 31) return 2;
    } else if (is_half) {
        if (imm <= 62 && (imm % 2 == 0)) return 2;
    } else {
        if (imm <= 124 && (imm % 4 == 0)) return 2;
    }
    return 4;
}

#if ARKCC_FEATURE_ASM_EXTRA
/* --asm-extra: runtime switch and the set of mnemonics the extension adds.
 * Both live here (not at each branch) so that "off" is one decision, not
 * thirteen, and so that the off path can reproduce the old "unknown mnemonic"
 * diagnostic exactly. */
static int ark_asm_extra_on(void) {
    return g_config.enable_asm_extra ? 1 : 0;
}

static int ark_asm_extra_name(const char *op) {
    static const char *names[] = {
        /* ADDS/SUBS are deliberately NOT gated: the ARM backend emits them
         * for the 64-bit add/sub carry chain, so they have to assemble
         * without --asm-extra (otherwise the compiler cannot assemble its
         * own output for `long long` arithmetic). */
        "ANDS", "ORRS", "EORS", "BICS", "MVNS", "MULS",
        "LSLS", "LSRS", "ASRS", "ADR", "CBZ", "CBNZ",
        "LDM", "LDMIA", "LDMDB", "STM", "STMIA", "STMDB",
        "TBB", "TBH", "LDRD", "STRD",
        "MLA", "MLS", "SMULL", "SMLAL", "MRS", "MSR"
    };
    int i;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (strieq_a(op, names[i])) return 1;
    return 0;
}
#endif

/* armasm spells four of these differently.  Canonicalising the mnemonic, rather
 * than teaching every branch a second name, keeps the size pass and the emit
 * pass looking at exactly one spelling -- a rule they cannot disagree about. */
static void canon_alias_a(char *m) {
    const char *to = 0;
    if (strieq_a(m, "LDMFD"))      to = "LDMIA";
    else if (strieq_a(m, "STMFD")) to = "STMDB";
    else if (strieq_a(m, "LDMEA")) to = "LDMDB";
    else if (strieq_a(m, "STMEA")) to = "STMIA";
    if (to) {
        int i = 0;
        while (to[i]) { m[i] = to[i]; i++; }
        m[i] = '\0';
    }
}

/* AND / ORR / EOR / BIC / MVN, every shape the backend and hand-written
 * assembly use:
 *   OP Rd, Rm          (Rd = Rd OP Rm)
 *   OP Rd, #imm        (Rd = Rd OP imm)
 *   OP Rd, Rn, Rm
 *   OP Rd, Rn, #imm
 * Only the first shape has a 16-bit encoding, and only for low registers;
 * everything else is Thumb-2.  Before this existed the encoder assumed the
 * 16-bit form unconditionally and never looked past the second operand, so
 * "ORR ip, ip, #1" -- the Thumb-bit fixup in front of an indirect call --
 * was silently emitted as NOTHING (and an earlier attempt as "orrs r4,r4",
 * masking ip to r4 and dropping the immediate).  The program then branched
 * into the middle of an instruction.
 *
 * Pass 1 asks with emit=0 and pass 2 with emit=1; both get the size from the
 * same decision, so the two passes cannot disagree.
 *
 * ANDS/ORRS/EORS/BICS/MVNS stay in the --asm-extra branch further down: they
 * are opt-in mnemonics and must keep being rejected when the switch is off. */
/* Defined further down; the logical-op encoder above needs it. */
static int thumb_imm12_a(unsigned val, unsigned *out_i, unsigned *out_imm3,
                         unsigned *out_imm8);

static int logic_dp_a(ArmAsm *a, const char *op, const char **pp, int emit) {
    const char *p = *pp;
    int is_bic = strieq_a(op, "BIC");
    int is_orr = strieq_a(op, "ORR");
    int is_eor = strieq_a(op, "EOR");
    int is_mvn = strieq_a(op, "MVN");
    unsigned base_reg, base_imm;
    int rd, rn;

    /* Encode "Rd = Rn OP imm", the Thumb-2 immediate form. */
    /* (written as a macro-free block below because C89-era style here) */
    if (is_bic)      { base_reg = 0xEA20u; base_imm = 0xF020u; }
    else if (is_orr) { base_reg = 0xEA40u; base_imm = 0xF040u; }
    else if (is_eor) { base_reg = 0xEA80u; base_imm = 0xF080u; }
    /* MVN Rd, #imm exists as ORN with Rn = 1111 (measured with GAS:
     * "mvn r0, #1" assembles to f06f 0001), so that form keeps Rn fixed
     * in the opcode word instead of taking it from the text. */
    else if (is_mvn) { base_reg = 0xEA6Fu; base_imm = 0xF06Fu; }
    else             { base_reg = 0xEA00u; base_imm = 0xF000u; }  /* AND */

    rd = parse_reg_a(&p);
    eat_comma_a(&p);
    if (*p == '#') {
        int imm = parse_imm_a(&p);
        unsigned i = 0, imm3 = 0, imm8 = 0;
        if (!thumb_imm12_a((unsigned)imm, &i, &imm3, &imm8)) {
            if (emit) {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "%sq immediate %s cannot be encoded as a modified "
                          "immediate", op, diag_num(b1, imm), 0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
            *pp = p;
            return 4;
        }
        if (emit) {
            unsigned rnw = is_mvn ? 0xFu : (unsigned)(rd & 0xF);
            out16(a, base_imm | ((i & 1) << 10) | rnw);
            out16(a, ((imm3 & 7) << 12) | ((unsigned)(rd & 0xF) << 8) | imm8);
        }
        *pp = p;
        return 4;
    }
    rn = parse_reg_a(&p);
    skip_space_a(&p);
    if (*p == ',') {
        int rm;
        p++; skip_space_a(&p);
        if (*p == '#') {
            int imm = parse_imm_a(&p);
            unsigned i = 0, imm3 = 0, imm8 = 0;
            if (!thumb_imm12_a((unsigned)imm, &i, &imm3, &imm8)) {
                if (emit) {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "%sq immediate %s cannot be encoded as a "
                              "modified immediate", op, diag_num(b1, imm),
                              0,0,0,0);
                    a->hard_errors++;
                    out16(a, 0xBF00);
                    out16(a, 0xBF00);
                }
                *pp = p;
                return 4;
            }
            if (emit) {
                unsigned rnw = is_mvn ? 0xFu : (unsigned)(rn & 0xF);
                out16(a, base_imm | ((i & 1) << 10) | rnw);
                out16(a, ((imm3 & 7) << 12) | ((unsigned)(rd & 0xF) << 8) | imm8);
            }
            *pp = p;
            return 4;
        }
        rm = parse_reg_a(&p);
        if (emit && rd >= 0 && rn >= 0 && rm >= 0) {
            out16(a, is_mvn ? 0xEA6Fu : (base_reg | (unsigned)(rn & 0xF)));
            out16(a, ((unsigned)(rd & 0xF) << 8) | (unsigned)(rm & 0xF));
        }
        *pp = p;
        return 4;
    }
    /* Two operands: the 16-bit form only knows low registers. */
    if (rd >= 0 && rd <= 7 && rn >= 0 && rn <= 7) {
        if (emit) {
            unsigned short16 = is_bic ? 0x4380u : is_orr ? 0x4300u :
                               is_eor ? 0x4040u : is_mvn ? 0x43C0u : 0x4000u;
            out16(a, short16 | ((unsigned)(rn & 7) << 3) | (unsigned)(rd & 7));
        }
        *pp = p;
        return 2;
    }
    /* Two operands with a high register: Thumb-2, Rn = Rd. */
    if (emit && rd >= 0 && rn >= 0) {
        out16(a, is_mvn ? 0xEA6Fu : (base_reg | (unsigned)(rd & 0xF)));
        out16(a, ((unsigned)(rd & 0xF) << 8) | (unsigned)(rn & 0xF));
    }
    *pp = p;
    return 4;
}

static int op_size_a(const char *op) {
    if (strieq_a(op, "BL"))   return 4;
    if (strieq_a(op, "MOVW")) return 4;
    if (strieq_a(op, "MOVT")) return 4;
    if (strieq_a(op, "ADDW") || strieq_a(op, "SUBW")) return 4;
#if ARKCC_FEATURE_ASM_VFP_LDSTM
    /* VFP multi-register load/store: one encoding, always 32 bits. */
    if (strieq_a(op, "VLDMIA") || strieq_a(op, "VLDM") || strieq_a(op, "VLDMDB") ||
        strieq_a(op, "VSTMIA") || strieq_a(op, "VSTM") || strieq_a(op, "VSTMDB") ||
        strieq_a(op, "VPUSH") || strieq_a(op, "VPOP")) return 4;
#endif
    if (strieq_a(op, "SDIV")) return 4;
    if (strieq_a(op, "UDIV")) return 4;
    if (strieq_a(op, "UMULL")) return 4;
    if (strieq_a(op, "B.W"))  return 4;
    /* Unconditional B uses 32-bit B.W (T4, +-1MB) to avoid +-2KB range
     * truncation in large functions (e.g. HAL_GPIO_Init is ~3.5KB;
     * 16-bit B T2 silently truncates imm11 causing the loop back-edge
     * to jump to a wrong address, breaking GPIO config). */
    if (strieq_a(op, "B"))    return 4;
    /* Conditional branches use 32-bit B<cond>.W (T3, +-1MB) to avoid
     * +-256B range truncation in large functions (e.g. HAL_RCC_OscConfig
     * is 8632 bytes; 16-bit B<cond> T1 silently truncates imm8 causing
     * branches to jump to wrong locations). T3 lo word bit12=0 (0x8000 base). */
    if (strieq_a(op, "BEQ") || strieq_a(op, "BNE") || strieq_a(op, "BCS") ||
        strieq_a(op, "BCC") || strieq_a(op, "BGE") || strieq_a(op, "BLT") ||
        strieq_a(op, "BGT") || strieq_a(op, "BLE") ||
        strieq_a(op, "BHI") || strieq_a(op, "BLS") ||
        strieq_a(op, "BLO") || strieq_a(op, "BHS")) return 4;
    /* Barriers DMB/DSB/ISB use 32-bit T1 encoding (0xF3BF 0x8F4x/5x/6x).
     * 16-bit T2 (0xBF50/60/70) is not standard ARMv7-M; use 32-bit form. */
    if (strieq_a(op, "DMB") || strieq_a(op, "DSB") || strieq_a(op, "ISB")) return 4;
    /* RRX is encoded as MOV Rd, Rm, RRX (32-bit T2 data-processing). */
    if (strieq_a(op, "RRX")) return 4;
    /* --asm-extra: multiply-accumulate and special-register moves are 32-bit
     * only.  Their size must be right in pass 1 or every later label and branch
     * in the file shifts by 2 bytes. */
    if (strieq_a(op, "MLA") || strieq_a(op, "MLS") ||
        strieq_a(op, "SMULL") || strieq_a(op, "SMLAL")) return 4;
    if (strieq_a(op, "MRS") || strieq_a(op, "MSR")) return 4;
    if (strieq_a(op, "ADR.W") || strieq_a(op, "CBZ.W") || strieq_a(op, "CBNZ.W")) return 4;
    /* --asm-extra: table branches and the 64-bit load/store pair have no
     * 16-bit encoding at all, so pass 1 must count 4 bytes for them. */
    if (strieq_a(op, "TBB") || strieq_a(op, "TBH") ||
        strieq_a(op, "LDRD") || strieq_a(op, "STRD")) return 4;
    /* CLZ, RBIT and SADD16 only exist as 32-bit Thumb-2 encodings. */
    if (strieq_a(op, "CLZ"))    return 4;
    if (strieq_a(op, "RBIT"))   return 4;
    if (strieq_a(op, "SADD16")) return 4;
    /* #1 FIX: VFP instructions are all 32-bit (two 16-bit halfwords).
     * Without this, pass1 counts 2 bytes but pass2 emits 4, causing
     * all subsequent labels/branches to be off by 2 bytes per VFP instr. */
    if (strieq_a(op, "VMOV") || strieq_a(op, "VADD") || strieq_a(op, "VSUB") ||
        strieq_a(op, "VMUL") || strieq_a(op, "VDIV") || strieq_a(op, "VCVT") ||
        strieq_a(op, "VCMP") || strieq_a(op, "VMRS") || strieq_a(op, "VSTR") ||
        strieq_a(op, "VLDR") || strieq_a(op, "VNEG") || strieq_a(op, "VABS") ||
        strieq_a(op, "VSQRT")) return 4;
    return 2;
}

/* ============================ LDR/STR family =============================
 * One parser and one encoder for LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB/LDRSH.
 *
 * Address modes on ARMv7-M (all four measured against arm-none-eabi-as):
 *   [Rn]                 [Rn, #imm]            [Rn, Rm]
 *   [Rn, Rm, LSL #0..3]  [Rn, #imm]!           [Rn], #imm
 * Register-indexed *writeback* ([Rn, Rm]! and [Rn], Rm) does not exist in
 * Thumb at all -- GAS refuses those lines, and so does this assembler.
 *
 * Widths:
 *   kind 0 word   16-bit T1 (imm5 scaled by 4) | 32-bit T3 (imm12 in bytes)
 *   kind 1 byte   16-bit T1 (imm5)              | 32-bit T3
 *   kind 2 half   16-bit T1 (imm5 scaled by 2)  | 32-bit T3
 *   kind 3 ldrsb  no 16-bit immediate form, 32-bit only
 *   kind 4 ldrsh  same
 * The 32-bit imm12 is a plain 0..4095 byte offset for *every* width; only the
 * 16-bit T1 imm5 is scaled.  (Measured: GAS assembles `ldr.w r0,[r1,#128]` to
 * f8d1 0080, i.e. 0x80 -- scaling belongs to VLDR, not to LDR/STR.)
 * The 32-bit register-offset and writeback forms share one header word
 * (T3 base with bit 7 cleared); the immediate bits differ.
 *
 * Pass 1 asks ldr_str_size2_a() and pass 2 emits on the same answer, so the
 * two passes cannot disagree about a width -- getting that wrong shifts every
 * later label in the file.  */
typedef struct ArmMemOp {
    int rn;      /* base register, -1 = missing */
    int rm;      /* index register, -1 = none */
    int shift;   /* LSL amount for rm, 0..3 */
    int imm;     /* immediate offset, or the post-index value */
    int wb;      /* a writeback form: "!" or a post-index operand */
    int post;    /* post-indexed ([Rn], #imm) rather than pre-indexed */
} ArmMemOp;

/* 0 = LDR/STR, 1 = byte, 2 = halfword, 3 = LDRSB, 4 = LDRSH. -1 if not ours. */
static int ldr_str_kind_a(const char *m, int *is_load) {
    if (m[0] == 'S' || m[0] == 's') *is_load = 0; else *is_load = 1;
    if (strieq_a(m, "LDR") || strieq_a(m, "STR"))   return 0;
    if (strieq_a(m, "LDRB") || strieq_a(m, "STRB")) return 1;
    if (strieq_a(m, "LDRH") || strieq_a(m, "STRH")) return 2;
    if (strieq_a(m, "LDRSB")) return 3;
    if (strieq_a(m, "LDRSH")) return 4;
    return -1;
}

static int is_ldr_str_a(const char *m) {
    int dummy = 0;
    return ldr_str_kind_a(m, &dummy) >= 0;
}

/* quiet != 0 is pass 1: it only needs the same size answer, and reporting the
 * same error twice (once per pass) reads like two separate defects. */
static void parse_memop_a(ArmAsm *a, const char **p, ArmMemOp *m, int quiet) {
    m->rn = -1; m->rm = -1; m->shift = 0; m->imm = 0; m->wb = 0; m->post = 0;
    skip_space_a(p);
    if (**p == '[') { (*p)++; skip_space_a(p); }
    m->rn = parse_reg_a(p);
    skip_space_a(p);
    if (**p == ',') {
        (*p)++; skip_space_a(p);
        if (**p == '#') {
            m->imm = parse_imm_a(p);
        } else {
            m->rm = parse_reg_a(p);
            skip_space_a(p);
            if (**p == ',') {                  /* , LSL #imm2 */
                char opn[8];
                (*p)++; skip_space_a(p);
                read_word_a(p, opn, (int)sizeof(opn));
                skip_space_a(p);
                if (**p == '#') m->shift = parse_imm_a(p);
                if (!strieq_a(opn, "LSL") || m->shift < 0 || m->shift > 3) {
                    if (!quiet) {
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "a register offset may only use 'LSL #0'..'LSL #3' "
                                  "(other shifts and ASR/LSR do not exist here)",
                                  0,0,0,0,0,0);
                        a->hard_errors++;
                    }
                    m->shift = 0;
                }
            }
        }
    }
    skip_space_a(p);
    if (**p == ']') (*p)++;
    skip_space_a(p);
    if (**p == '!') {
        m->wb = 1;
        (*p)++;
    } else if (**p == ',') {                   /* post-indexed */
        (*p)++; skip_space_a(p);
        m->wb = 1;
        m->post = 1;
        if (**p == '#') m->imm = parse_imm_a(p);
        else            m->rm = parse_reg_a(p);
    }
}

/* 2 or 4 bytes. Both passes must get the same answer. */
static int ldr_str_size2_a(int kind, int rt, const ArmMemOp *m) {
    int low = (rt >= 0 && rt <= 7 && m->rn >= 0 && m->rn <= 7);
    if (m->rm >= 0) {
        if (low && m->rm <= 7 && m->shift == 0 && !m->wb) return 2;
        return 4;
    }
    if (m->wb) return 4;
    if (kind == 0 && rt >= 0 && rt <= 7 && m->imm >= 0 && m->imm <= 1020 &&
        (m->imm & 3) == 0 && (m->rn == 13 || m->rn == 15))
        return 2;                            /* SP-relative / literal pool */
    switch (kind) {
    case 1:  return (low && m->imm >= 0 && m->imm <= 31) ? 2 : 4;
    case 2:  return (low && m->imm >= 0 && m->imm <= 62 && (m->imm & 1) == 0) ? 2 : 4;
    case 0:  return (low && m->imm >= 0 && m->imm <= 124 && (m->imm & 3) == 0) ? 2 : 4;
    default: return 4;                       /* LDRSB/LDRSH: no imm16 form */
    }
}

static void emit_ldr_str_a(ArmAsm *a, int kind, int is_load, int rt,
                           const ArmMemOp *m) {
    /* 16-bit T1 bases: [kind][0] = store, [kind][1] = load. */
    static const unsigned t1_imm[5][2] = {
        {0x6000u, 0x6800u},   /* STR   / LDR  */
        {0x7000u, 0x7800u},   /* STRB  / LDRB */
        {0x8000u, 0x8800u},   /* STRH  / LDRH */
        {0x0000u, 0x0000u},   /* LDRSB: no 16-bit immediate form */
        {0x0000u, 0x0000u}    /* LDRSH: same */
    };
    static const unsigned t1_reg[5][2] = {
        {0x5000u, 0x5800u},   /* STR   / LDR  */
        {0x5400u, 0x5C00u},   /* STRB  / LDRB */
        {0x5200u, 0x5A00u},   /* STRH  / LDRH */
        {0x0000u, 0x5600u},   /* LDRSB (load only) */
        {0x0000u, 0x5E00u}    /* LDRSH (load only) */
    };
    static const unsigned t3[5][2] = {          /* 32-bit immediate form */
        {0xF8C0u, 0xF8D0u},   /* STR.W   / LDR.W  */
        {0xF880u, 0xF890u},   /* STRB.W  / LDRB.W */
        {0xF8A0u, 0xF8B0u},   /* STRH.W  / LDRH.W */
        {0x0000u, 0xF990u},   /* LDRSB.W */
        {0x0000u, 0xF9B0u}    /* LDRSH.W */
    };
    int sz;
    unsigned base;

    if (!is_load && (kind == 3 || kind == 4)) {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "LDRSB/LDRSH are loads only; there is no signed store", 0,0,0,0,0,0);
        a->hard_errors++;
        kind = 0;
    }
    if (rt < 0 || rt > 15) {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "LDR/STR needs a destination/source register", 0,0,0,0,0,0);
        a->hard_errors++;
        rt = 0;
    }
    if (m->rn < 0 || m->rn > 15) {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "LDR/STR needs a base register inside []", 0,0,0,0,0,0);
        a->hard_errors++;
    }
    if (m->rm >= 0 && m->rm > 15) {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "index register out of range", 0,0,0,0,0,0);
        a->hard_errors++;
    }
    if (m->wb && m->rm >= 0) {
        /* Thumb has no register-indexed writeback form: not "unimplemented",
         * simply absent from the architecture (GAS refuses the same lines). */
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "register-indexed writeback ([Rn, Rm]! / [Rn], Rm) does not "
                  "exist in Thumb; use an immediate offset or a separate ADD",
                  0,0,0,0,0,0);
        a->hard_errors++;
    }

    /* A writeback with a register index is unencodable: fall back to the plain
     * register-offset form so the object still has the right size. */
    {
        ArmMemOp mm = *m;
        if (mm.wb && mm.rm >= 0) mm.wb = 0;
        sz = ldr_str_size2_a(kind, rt, &mm);
        m = &mm;
    }
    base = t3[kind][is_load ? 1 : 0];

    if (sz == 2) {
        if (m->rm < 0 && m->rn == 13) {
            /* SP-relative T1: 1001 L imm8 | Rt | imm8/4 (word only). */
            out16(a, (is_load ? 0x9800u : 0x9000u) |
                      ((unsigned)(rt & 7) << 8) | (unsigned)((m->imm >> 2) & 0xFF));
            return;
        }
        if (m->rm < 0 && m->rn == 15) {
            /* PC-relative literal load, 01001 Rt imm8 (loads only). */
            out16(a, 0x4800u | ((unsigned)(rt & 7) << 8) |
                      (unsigned)((m->imm >> 2) & 0xFF));
            return;
        }
        if (m->rm >= 0) {
            out16(a, t1_reg[kind][is_load ? 1 : 0] |
                      ((unsigned)(m->rm & 7) << 6) |
                      ((unsigned)(m->rn & 7) << 3) | (unsigned)(rt & 7));
        } else {
            unsigned imm5;
            if (kind == 1)      imm5 = (unsigned)(m->imm & 0x1F);
            else if (kind == 2) imm5 = (unsigned)((m->imm >> 1) & 0x1F);
            else                imm5 = (unsigned)((m->imm >> 2) & 0x1F);
            out16(a, t1_imm[kind][is_load ? 1 : 0] | (imm5 << 6) |
                      ((unsigned)(m->rn & 7) << 3) | (unsigned)(rt & 7));
        }
        return;
    }

    /* 32-bit forms. */
    if (m->rm >= 0) {
        /* Register offset: same header as the writeback form. */
        out16(a, (base & ~0x80u) | (unsigned)(m->rn & 0xF));
        out16(a, ((unsigned)(rt & 0xF) << 12) |
                 ((unsigned)(m->shift & 3) << 4) | (unsigned)(m->rm & 0xF));
        return;
    }
    if (m->wb) {
        int pre = !m->post;
        int neg = (m->imm < 0);
        int mag = neg ? -m->imm : m->imm;
        if (mag > 255) {
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "a writeback offset must be within -255..255", 0,0,0,0,0,0);
            a->hard_errors++;
            mag = 0;
        }
        out16(a, (base & ~0x80u) | (unsigned)(m->rn & 0xF));
        out16(a, ((unsigned)(rt & 0xF) << 12) | 0x800u |
                 (unsigned)(pre ? 0x400 : 0) | (unsigned)(neg ? 0 : 0x200) |
                 0x100u | (unsigned)mag);
        return;
    }
    if (m->imm >= 0) {
        int off = m->imm;
        if (off > 4095) {
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "offset out of range: a 32-bit LDR/STR takes 0..4095 bytes",
                      0,0,0,0,0,0);
            a->hard_errors++;
            off = 0;
        }
        out16(a, base | (unsigned)(m->rn & 0xF));
        out16(a, ((unsigned)(rt & 0xF) << 12) | (unsigned)(off & 0xFFF));
        return;
    }
    {
        int mag = -m->imm;                       /* negative, no writeback */
        if (mag > 255) {
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "a negative offset must be within -255..-1", 0,0,0,0,0,0);
            a->hard_errors++;
            mag = 0;
        }
        out16(a, (base & ~0x80u) | (unsigned)(m->rn & 0xF));
        out16(a, ((unsigned)(rt & 0xF) << 12) | 0x800u | 0x400u | (unsigned)mag);
    }
}

/* Forward declaration: needed in pass1 to decide whether PUSH/POP is T1 or T2 encoded */
static int parse_reglist_a(ArmAsm *a, const char **p, int *has_lr, int *has_pc);

static void pass1_a(ArmAsm *a) {
    a->pass2 = 0;
    const char *p = a->src;
    /* The reserved vector-table area counts as part of the section, so labels
     * inside it (there are none) and after it keep the addresses the final
     * image will really have. */
    int pc = g_arm_vt_reserve ? ARM_VT_SIZE : 0;
    a->label_count = 0;
    a->set_count = 0;
    a->literal_count = 0;
    while (*p) {
        char first[64];
        int n;
        skip_space_a(&p);
        if (*p == '\n') { p++; continue; }
        if (*p == ';') { skip_to_eol_a(&p); continue; }
        if (*p == '\0') break;
        n = read_word_a(&p, first, (int)sizeof(first));
        canon_alias_a(first);
        if (n == 0) { skip_to_eol_a(&p); continue; }
        skip_space_a(&p);
        if (*p == ':') {
            add_label_a(a, first, pc);
            p++;
            skip_space_a(&p);
            if (*p == '\n' || *p == ';' || *p == '\0') { if (*p == ';') skip_to_eol_a(&p); if (*p == '\n') p++; continue; }
            n = read_word_a(&p, first, (int)sizeof(first));
            canon_alias_a(first);
            if (n == 0) { skip_to_eol_a(&p); continue; }
        }
        if (first[0] == '.') {
            /* Pseudo-directives: some affect pc, others are metadata-only */
            if (strieq_a(first, ".set") || strieq_a(first, ".equ")) {
                /* .set name, 0xADDR - record absolute address */
                char sym[64];
                skip_space_a(&p);
                n = read_word_a(&p, sym, (int)sizeof(sym));
                if (n > 0) {
                    skip_space_a(&p);
                    if (*p == ',') p++;
                    skip_space_a(&p);
                    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                        unsigned addr = 0;
                        p += 2;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            char c = *p;
                            int v = (c >= '0' && c <= '9') ? c - '0' :
                                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                            addr = addr * 16 + v;
                            p++;
                        }
                        add_set_a(a, sym, (int)addr);
                    } else {
                        /* .set name, other_label - resolve later, store as ref */
                        char tgt[64];
                        n = read_word_a(&p, tgt, (int)sizeof(tgt));
                        (void)n; (void)tgt;
                        /* For alias functions, defer to resolve_refs via BL lookup */
                    }
                }
            } else if (strieq_a(first, ".zero") || strieq_a(first, ".skip") || strieq_a(first, ".space")) {
                /* .zero N - reserve N bytes */
                int sz = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { sz = sz * 10 + (*p - '0'); p++; }
                pc += sz;
            } else if (strieq_a(first, ".word") || strieq_a(first, ".long")) {
                /* .word value - 4 bytes (may have multiple comma-separated values) */
                int count = 1;
                const char *scan = p;
                while (*scan && *scan != '\n') { if (*scan == ',') count++; scan++; }
                pc += 4 * count;
            } else if (strieq_a(first, ".byte")) {
                int count = 1;
                const char *scan = p;
                while (*scan && *scan != '\n') { if (*scan == ',') count++; scan++; }
                pc += count;
            } else if (strieq_a(first, ".asciz")) {
                /* .asciz "string" - string length + NUL, aligned to 2 */
                int slen = 0;
                skip_space_a(&p);
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"' && *p != '\n') {
                        if (*p == '\\') { slen++; p++; if (*p) p++; }
                        else { slen++; p++; }
                    }
                    if (*p == '"') p++;
                }
                slen++;  /* NUL terminator */
                if (slen & 1) slen++;  /* align to 2 */
                pc += slen;
            } else if (strieq_a(first, ".ascii")) {
                int slen = 0;
                skip_space_a(&p);
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"' && *p != '\n') {
                        if (*p == '\\') { slen++; p++; if (*p) p++; }
                        else { slen++; p++; }
                    }
                    if (*p == '"') p++;
                }
                if (slen & 1) slen++;
                pc += slen;
            } else if (strieq_a(first, ".align") || strieq_a(first, ".balign")) {
                /* Align to N bytes */
                int align = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { align = align * 10 + (*p - '0'); p++; }
                if (align > 1) {
                    int rem = pc % align;
                    if (rem != 0) pc += align - rem;
                }
            } else if (strieq_a(first, ".weak")) {
                /* .weak name - mark the next-defined label as weak.
                 * The symbol name argument is consumed but pending_weak
                 * applies to whichever label is defined next (the .weak
                 * directive always immediately precedes the label in our
                 * emission order: .global / .weak / label:). */
                char wname[64];
                skip_space_a(&p);
                n = read_word_a(&p, wname, (int)sizeof(wname));
                (void)n; (void)wname;
                a->pending_weak = 1;
            } else if (strieq_a(first, ".section")) {
                /* .section .text.foo, "ax", %progbits - track current section.
                 * Section tracking is metadata-only (pc unaffected); the
                 * assembler emits all code/data into a single linear buffer. */
                char sec[64];
                skip_space_a(&p);
                n = read_word_a(&p, sec, (int)sizeof(sec));
                if (n > 0) {
                    if (starts_with_a(sec, ".init_array")) a->cur_section = 3;
                    else if (starts_with_a(sec, ".fini_array")) a->cur_section = 4;
                    else if (starts_with_a(sec, ".data")) a->cur_section = 1;
                    else if (starts_with_a(sec, ".bss")) a->cur_section = 2;
                    else a->cur_section = 0;  /* .text or default */
                }
            } else if (strieq_a(first, ".text")) {
                a->cur_section = 0;
            } else if (strieq_a(first, ".data")) {
                a->cur_section = 1;
            } else if (strieq_a(first, ".bss")) {
                a->cur_section = 2;
            }
            /* .global, .type, .size, .thumb, .file, .loc, .cfi_*,
             * etc. are metadata-only and don't affect pc */
        } else {
            /* Optional ".W"/".N" width suffix (see pass 2): skip it before any
             * size decision.  It stays inside the instruction branch on purpose:
             * a pseudo-directive must never reach the size chain below, or it
             * would add 2 bytes per directive line and shift every label. */
            if (*p == '.' && (my_toupper_a(p[1]) == 'W' || my_toupper_a(p[1]) == 'N') &&
                !is_alnum_a(p[2])) {
                p += 2;
                skip_space_a(&p);
            }
            if (0) {
            } else if (is_ldr_str_a(first)) {
                /* Width comes from ldr_str_size2_a() and nothing else, so pass 1
                 * and pass 2 cannot disagree (a 2-byte disagreement shifts every
                 * later label in the file). */
                int is_load1 = 1;
                int kind1 = ldr_str_kind_a(first, &is_load1);
                int rt1 = parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                if (*p == '=') {
                    /* LDR rd, =label - emit MOVW+MOVT (8 bytes) in pass2 */
                    char name[64];
                    p++;
                    n = read_word_a(&p, name, (int)sizeof(name));
                    (void)n;
                    pc += 8;
                } else {
                    ArmMemOp m1;
                    parse_memop_a(a, &p, &m1, 1);
                    pc += ldr_str_size2_a(kind1, rt1, &m1);
                }
            } else if (strieq_a(first, "MOV")) {
                /* Same rule as pass 2: the 16-bit form only covers r0-r7 with
                 * an 8-bit immediate; everything else is MOVW (4 bytes). */
                int rd1 = parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                if (*p == '#') {
                    int imm1 = parse_imm_a(&p);
                    pc += (rd1 >= 0 && rd1 <= 7 && imm1 >= 0 && imm1 <= 255) ? 2 : 4;
                } else {
                    (void)parse_reg_a(&p);
                    pc += 2;
                }
            } else if (strieq_a(first, "PUSH") || strieq_a(first, "POP")) {
                /* PUSH/POP may be 16-bit T1 (r0-r7 + lr/pc only) or 32-bit T2
                 * (includes r8-r12); size determined by the register list to keep pass1 offsets correct. */
                int has_lr = 0, has_pc = 0;
                int list = parse_reglist_a(a, &p, &has_lr, &has_pc);
                /* The 16-bit form has one bit that is LR in PUSH and PC in POP,
                 * so PUSH{..,pc} and POP{..,lr} only exist as T2 and are twice
                 * as wide.  Pass 2 makes the same decision; if the two ever
                 * disagree every label after this line moves. */
                if (strieq_a(first, "PUSH")) {
                    if (has_pc)                    pc += 0;   /* not encodable */
                    else if (list & 0x1F00)        pc += 4;   /* T2 */
                    else                           pc += 2;   /* T1 */
                } else {
                    if ((list & 0x1F00) || has_lr) pc += 4;   /* T2 */
                    else                           pc += 2;   /* T1 */
                }
            } else if (strieq_a(first, "ADD") || strieq_a(first, "SUB") ||
                       strieq_a(first, "ADDS") || strieq_a(first, "SUBS")) {
                /* ADD/SUB rd, rn, #imm: T1 (2 bytes) if imm<=7 and low regs,
                 * T3 (4 bytes) if imm>7 or high regs. Keep pass1 in sync with pass2.
                 * The flag-setting aliases ADDS/SUBS use the same widths. */
                int rd = parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                int rn = parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    if (rn < 0) rn = rd;                       /* two-operand form */
                    if (rd == 13 && rn == 13)                  /* ADD/SUB sp, #imm7*4 */
                        pc += (imm >= 0 && imm <= 508 && (imm & 3) == 0) ? 2 : 4;
                    else if (rd == rn && rd >= 0 && rd <= 7 &&
                             imm >= 0 && imm <= 255) pc += 2;  /* rd, #imm8 */
                    else if (imm >= 0 && imm <= 7 && rd >= 0 && rd <= 7 && rn >= 0 && rn <= 7)
                        pc += 2;                               /* T1 3-bit imm */
                    else pc += 4;                              /* ADDW / SUBW */
                } else {
                    /* Register forms: the 16-bit encodings only name r0-r7.
                     * ADD with Rd == Rn has a 16-bit form for every register;
                     * SUB has one only for low registers.  Same rule as pass 2. */
                    int rm1 = parse_reg_a(&p);
                    int is_sub1 = strieq_a(first, "SUB") || strieq_a(first, "SUBS");
                    if (rm1 >= 0) {
                        if (rd <= 7 && rn <= 7 && rm1 <= 7) pc += 2;
                        else                                  pc += 4;
                    } else {
                        if (is_sub1) pc += (rd >= 0 && rd <= 7 && rn >= 0 && rn <= 7) ? 2 : 4;
                        else         pc += 2;
                    }
                }
            } else if (strieq_a(first, "LSLS") || strieq_a(first, "LSRS") ||
                       strieq_a(first, "ASRS")) {
                /* --asm-extra: 16-bit immediate / 16-bit two-operand register form,
                 * 32-bit only when a separate shift register is named.  Getting this
                 * wrong shifts every later label by 2 bytes, so it is decided the
                 * same way pass 2 decides it. */
                int third_is_reg = 0;
                (void)parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                (void)parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') {
                    p++; skip_space_a(&p);
                    if (*p != '#') third_is_reg = 1;
                }
                pc += third_is_reg ? 4 : 2;
            } else if (strieq_a(first, "CMP")) {
                /* CMP Rn, #imm: T1 (2B) for imm 0-255, T2 (4B) for larger. */
                int rn = parse_reg_a(&p);
                (void)rn;
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    if (imm >= 0 && imm <= 255) pc += 2;
                    else pc += 4;
                } else {
                    (void)parse_reg_a(&p);
                    pc += 2;
                }
            } else if (strieq_a(first, "MOVEQ") || strieq_a(first, "MOVNE") ||
                       strieq_a(first, "MOVLT") || strieq_a(first, "MOVGT") ||
                       strieq_a(first, "MOVLE") || strieq_a(first, "MOVGE") ||
                       strieq_a(first, "MOVLO") || strieq_a(first, "MOVHI") ||
                       strieq_a(first, "MOVLS") || strieq_a(first, "MOVHS") ||
                       strieq_a(first, "MOVCS") || strieq_a(first, "MOVCC")) {
                /* Conditional MOV in IT block: immediate form uses 32-bit MOV.W
                 * (4 bytes, no flag modification); register form uses 16-bit (2 bytes). */
                int rd = parse_reg_a(&p);
                (void)rd;
                skip_space_a(&p);
                if (*p == ',') { p++; skip_space_a(&p); }
                if (*p == '#') {
                    (void)parse_imm_a(&p);
                    pc += 4;  /* 32-bit MOV.W */
                } else {
                    (void)parse_reg_a(&p);
                    pc += 2;  /* 16-bit MOV */
                }
            } else if (strieq_a(first, "AND") || strieq_a(first, "ORR") ||
                       strieq_a(first, "EOR") || strieq_a(first, "BIC") ||
                       strieq_a(first, "MVN")) {
                /* Same decision function as pass 2: the 16-bit form only
                 * covers two low registers, so a high register or an
                 * immediate operand makes the instruction 4 bytes. */
                pc += logic_dp_a(a, first, &p, 0);
            } else if (strieq_a(first, "TST") ||
                       strieq_a(first, "SXTB") || strieq_a(first, "SXTH") ||
                       strieq_a(first, "UXTB") || strieq_a(first, "UXTH")) {
                /* Two-register forms: 16-bit T1 for r0-r7 operands, 32-bit T2
                 * when either register is high (r8-r15). Must match pass2. */
                int rd = parse_reg_a(&p);
                eat_comma_a(&p);
                {
                    int rm = parse_reg_a(&p);
                    if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) pc += 2;
                    else                                          pc += 4;
                }
            } else if (strieq_a(first, "MOVS")) {
                /* MOVS Rd,#imm: 16-bit for rd in r0-r7 and imm 0..255.
                 * MOVS Rd,Rm  : 16-bit for two low registers.
                 * Everything else takes the 32-bit form. */
                int rd = parse_reg_a(&p);
                eat_comma_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    if (rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255) pc += 2;
                    else                                              pc += 4;
                } else {
                    int rm = parse_reg_a(&p);
                    if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) pc += 2;
                    else                                          pc += 4;
                }
            } else if (strieq_a(first, "SVC")) {
                pc += 2;
            } else if (strieq_a(first, "PUSH") || strieq_a(first, "POP")) {
                /* PUSH/POP are STMDB/LDMIA on sp.  They were not in this
                 * chain at all, so pass 1 took the op_size_a() default of 2
                 * bytes even when pass 2 emitted the 32-bit form -- and the
                 * 32-bit form is exactly what a high register forces:
                 * "POP {ip}" is e8bd 1000.  Every such push/pop shifted every
                 * later label by 2, and because cross-object/label branches
                 * are resolved from pass-1 label addresses against pass-2
                 * code offsets, the branches landed past their target
                 * (measured: a bss-clear loop whose "BNE" jumped to its own
                 * CMP instead of the store, once a call through a function
                 * pointer added a "POP {ip}"). */
                int has_lr1 = 0, has_pc1 = 0, list1;
                list1 = parse_reglist_a(a, &p, &has_lr1, &has_pc1);
                if ((list1 & ~0xFF) == 0) pc += 2;
                else                      pc += 4;
            } else if (strieq_a(first, "LDMIA") || strieq_a(first, "LDM") ||
                       strieq_a(first, "STMIA") || strieq_a(first, "STM") ||
                       strieq_a(first, "LDMDB") || strieq_a(first, "STMDB")) {
                /* The 16-bit T1 form is available only for: base inside
                 * r0-r7, register list inside r0-r7, writeback present and
                 * increment-after addressing.  Everything else takes the
                 * 32-bit T2 form.  Pass 1 has to make the same decision as
                 * pass 2, or every later label in the file shifts by 2. */
                int rn = parse_reg_a(&p);
                int has_wb = 0, has_lr1 = 0, has_pc1 = 0, list1;
                int is_db1 = (first[3] == 'D' || first[3] == 'd');
                skip_space_a(&p);
                if (*p == '!') { has_wb = 1; p++; }
                eat_comma_a(&p);
                list1 = parse_reglist_a(a, &p, &has_lr1, &has_pc1);
                if (has_wb && !is_db1 && !has_lr1 && !has_pc1 &&
                    rn >= 0 && rn <= 7 && list1 != 0 && (list1 & ~0xFF) == 0)
                    pc += 2;
                else
                    pc += 4;
            } else {
                pc += op_size_a(first);
            }
        }
        skip_to_eol_a(&p);
        if (*p == '\n') p++;
    }
    /* LDR =label uses MOVW+MOVT (8 bytes), no literal pool needed */
    a->literal_pool_start = -1;
}

static int parse_reglist_a(ArmAsm *a, const char **p, int *has_lr, int *has_pc) {
    int list = 0;
    *has_lr = 0; *has_pc = 0;
    skip_space_a(p);
    if (**p != '{') return 0;
    (*p)++;
    while (**p && **p != '}') {
        int lo, hi, r;
        skip_space_a(p);
        if (**p == 0 || **p == '}') break;
        lo = parse_reg_a(p);
        if (lo < 0 || lo > 15) {
            /* Refuse rather than guess: a wrong register list is a wrong
             * prologue, i.e. a corrupted stack, and nothing downstream -- not
             * the assembler, not the linker, not the CPU -- would notice. */
            if (a && a->pass2) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "register list: expected r0-r15, lr, pc, sp, ip or fp", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (**p && **p != '}' && **p != ',' && **p != '\n') (*p)++;  /* skip unknown char */
            skip_space_a(p);
            if (**p == ',') (*p)++;
            continue;
        }
        hi = lo;
        skip_space_a(p);
        if (**p == '-') {
            /* rN-rM ranges are the ordinary way to name a callee-saved set.
             * This parser used to skip the '-' as an unknown character and
             * keep only the two endpoints: "PUSH {r4-r7, lr}" was encoded as
             * {r4, r7, lr} (b590), so r5/r6 were never saved and were restored
             * from whatever the frame happened to hold.  A silent wrong
             * answer in the prologue is the worst place for one. */
            (*p)++;
            skip_space_a(p);
            hi = parse_reg_a(p);
            if (hi < lo || hi > 15) {
                if (a && a->pass2) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "register list: descending or out-of-range register range", 0,0,0,0,0,0);
                    a->hard_errors++;
                }
                hi = lo;          /* keep the list well formed; the error stands */
            }
        }
        for (r = lo; r <= hi; r++) list |= (1 << r);
        skip_space_a(p);
        if (**p == ',') (*p)++;
    }
    if (**p == '}') (*p)++;
    if (list & (1 << 14)) *has_lr = 1;   /* lr / pc are also reachable as r14 / r15 */
    if (list & (1 << 15)) *has_pc = 1;
    return list;
}

/* Pre-indexed writeback (`[Rn, #imm]!`) and post-indexed addressing
 * (`[Rn], #imm`) have no encoder here.  Both are easy to ignore by accident:
 * the offset is consumed, the ']' ends the operand, and the instruction is
 * emitted as a plain access -- which silently drops the writeback.  Refuse
 * them instead, so the author writes the ADD out.
 * Called right after the closing ']' has been consumed. */
static void check_no_index_writeback_a(ArmAsm *a, const char **p) {
    skip_space_a(p);
    if (**p == '!') {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "pre-indexed writeback ([Rn, #imm]!) is not emitted; do the "
                  "address update with a separate ADD", 0,0,0,0,0,0);
        a->hard_errors++;
        (*p)++;
    } else if (**p == ',') {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "post-indexed addressing ([Rn], #imm) is not emitted; do the "
                  "address update with a separate ADD", 0,0,0,0,0,0);
        a->hard_errors++;
    }
}

static void emit_bl_a(ArmAsm *a, int target) {
    int offset = target - a->pc - 4;
    int off2 = offset >> 1;
    int s = (off2 >> 23) & 1;
    int i1 = (off2 >> 22) & 1;
    int i2 = (off2 >> 21) & 1;
    int imm10 = (off2 >> 11) & 0x3FF;
    int imm11 = off2 & 0x7FF;
    int j1 = (~(i1 ^ s)) & 1;
    int j2 = (~(i2 ^ s)) & 1;
    unsigned hi = 0xF000u | (unsigned)(s << 10) | (unsigned)imm10;
    unsigned lo = 0xD000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
    out16(a, hi);
    out16(a, lo);
}

/* 32-bit unconditional B.W (T4 encoding) - range +-16MB.
 * hi: 0xF000 | (S<<10) | imm10
 * lo: 10 J1 1 J2 imm11  (bits[15:14]=10, bit12=1 for T4)
 *     BL T1 has bits[15:14]=11 (0xD000 base) - distinct from B.W
 * imm32 = SignExtend(S:I1:I2:imm10:imm11:0, 25)
 * NOTE: lo base MUST be 0x9000 (bit14=0, bit12=1). Using 0xD000
 * (bit14=1) encodes BL T1, which still branches but corrupts LR. */
static void emit_b_w_a(ArmAsm *a, int target) {
    int offset = target - a->pc - 4;
    int off2 = offset >> 1;
    int s = (off2 >> 23) & 1;
    int i1 = (off2 >> 22) & 1;
    int i2 = (off2 >> 21) & 1;
    int imm10 = (off2 >> 11) & 0x3FF;
    int imm11 = off2 & 0x7FF;
    int j1 = (~(i1 ^ s)) & 1;
    int j2 = (~(i2 ^ s)) & 1;
    unsigned hi = 0xF000u | (unsigned)(s << 10) | (unsigned)imm10;
    unsigned lo = 0x9000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
    out16(a, hi);
    out16(a, lo);
}

/* 32-bit conditional B<cond>.W (T3 encoding) - range +-1MB.
 * hi: 0xF000 | (S<<10) | (cond<<6) | imm6
 * lo: 1 0 J1 0 J2 imm11[10:0]  (bit15=1, bit14=0, bit12=0 for T3)
 *     T4 B.W has lo bit12=1 (0x9000 base) — distinguishes from T3.
 *     BL T1  has lo bit14=1,bit12=1 (0xD000 base) — also different.
 * imm32 = SignExtend(S:I1:I2:imm6:imm11, 20) << 1
 * NOTE: T3 and T4 share the same hw2 bit layout (J1 at bit13, J2 at bit11,
 *       imm11 at bits[10:0]), but DIFFERENT J1/J2 decoding:
 *       - T4/BL: I1 = NOT(J1 EOR S)  (inverted)
 *       - T3:    I1 = J1             (direct copy, NO inversion)
 *       Verified empirically via MDK armasm:
 *         MDK forward BEQ.W = 0xF000 0x8046 (J1=0, J2=0) for offset +0x8C.
 *         With T4 formula that would give offset 0x18008C (wrong).
 *         With T3 formula (I1=J1) it gives offset 0x8C (correct).
 *       Using the T4 formula for T3 causes forward conditional branches to
 *       jump ~786KB forward (outside flash) → HardFault. */
static void emit_bcond_w_a(ArmAsm *a, int cond, int target) {
    int offset = target - a->pc - 4;
    int off2 = offset >> 1;
    int s = (off2 >> 19) & 1;
    int i1 = (off2 >> 18) & 1;
    int i2 = (off2 >> 17) & 1;
    int imm6 = (off2 >> 11) & 0x3F;
    int imm11 = off2 & 0x7FF;
    int j1 = i1;
    int j2 = i2;
    unsigned hi = 0xF000u | (unsigned)(s << 10) | (unsigned)(cond << 6) | (unsigned)imm6;
    /* T3 lo word: bit15=1, bit14=0, bit13=J1, bit12=0, bit11=J2, bits[10:0]=imm11 */
    unsigned lo = 0x8000u | (unsigned)(j1 << 13) | (unsigned)(j2 << 11) | (unsigned)imm11;
    out16(a, hi);
    out16(a, lo);
}

static int read_label_name_a(const char **p, char *buf, int cap) {
    int n = 0;
    skip_space_a(p);
    while (**p && **p != ' ' && **p != '\t' && **p != '\n' && **p != ';' && **p != ',' && n < cap - 1) {
        buf[n++] = **p; (*p)++;
    }
    buf[n] = 0;
    return n;
}

/* Reverse of the Thumb-2 ThumbExpandImm() function: split a 32-bit
 * constant into the (i, imm3, imm8) fields of a modified immediate.
 *   rot = imm12[11:10]; value = ROR(bit7-replicated imm8, rot * 8)
 *   rot == 0        : value = ZeroExtend(imm8)
 * Returns 1 when the constant is representable, 0 otherwise. Used by the
 * MOVS/CMP.W immediate forms so an unrepresentable constant is reported
 * instead of being silently truncated to 8 bits. */
static int thumb_imm12_a(unsigned val, unsigned *out_i, unsigned *out_imm3,
                         unsigned *out_imm8) {
    unsigned rot;
    if (val <= 0xFFu) {
        *out_i = 0; *out_imm3 = 0; *out_imm8 = val;
        return 1;
    }
    for (rot = 1; rot < 4; rot++) {
        unsigned sh = rot * 8;
        unsigned unrot = (val << sh) | (val >> (32 - sh));   /* ROL by sh */
        unsigned imm8 = unrot & 0xFFu;
        unsigned cand;
        if ((imm8 & 0x80u) == 0) continue;   /* bit7 must be set here */
        cand = 0xFFFFFF00u | imm8;           /* '1':imm8 */
        if (((cand >> sh) | (cand << (32 - sh))) != val) continue;
        *out_i = (rot >> 1) & 1u;
        *out_imm3 = rot & 7u;
        *out_imm8 = imm8;
        return 1;
    }
    return 0;
}

/* ADDW/SUBW (T4): a plain 12-bit immediate, 0000..4095, no rotation and no
 * flags.  This is the form for every immediate that the 16-bit encodings cannot
 * take: the T3 form takes a *modified* immediate (an 8-bit value rotated by an
 * even amount), so writing a raw 12-bit value into its i:imm3:imm8 fields
 * encodes a different number -- "SUB sp,#2016" used to come out as 480. */
static void emit_addw_a(ArmAsm *a, int is_sub, int rd, int rn, int imm) {
    unsigned i = ((unsigned)imm >> 11) & 1;
    unsigned imm3 = ((unsigned)imm >> 8) & 7;
    unsigned imm8 = (unsigned)imm & 0xFF;
    out16(a, (unsigned)(is_sub ? 0xF2A0 : 0xF200) | (i << 10) | (unsigned)(rn & 0xF));
    out16(a, (imm3 << 12) | ((unsigned)(rd & 0xF) << 8) | imm8);
}

/* ADD/SUB (register) T2: the 32-bit form, where every operand may be any of
 * r0-r15 (SP included).  The 16-bit T1 form only names r0-r7 and silently masks
 * the rest: "ADD ip, r6, ip" came out as "adds r4, r6, r4", and a base address
 * became a write into nothing.  Measured with arm-none-eabi-as:
 *   add.w ip, r6, ip = eb06 0c0c    sub.w sp, sp, r7 = ebad 0d07 */
static void emit_addsub_reg_w_a(ArmAsm *a, int is_sub, int rd, int rn, int rm) {
    out16(a, (unsigned)(is_sub ? 0xEBA0 : 0xEB00) | (unsigned)(rn & 0xF));
    out16(a, ((unsigned)(rd & 0xF) << 8) | (unsigned)(rm & 0xF));
}

/* MOVW: a plain 16-bit immediate into any register. */
static void emit_movw_a(ArmAsm *a, int rd, int imm) {
    unsigned ii = ((unsigned)imm >> 11) & 1;
    unsigned imm4 = ((unsigned)imm >> 12) & 0xF;
    unsigned imm3 = ((unsigned)imm >> 8) & 7;
    unsigned imm8 = (unsigned)imm & 0xFF;
    out16(a, 0xF240u | (ii << 10) | imm4);
    out16(a, (imm3 << 12) | ((unsigned)(rd & 0xF) << 8) | imm8);
}

#if ARKCC_FEATURE_ASM_VFP_LDSTM
/* --------------------- VFP multi-register load/store ---------------------
 * VLDMIA/VLDMDB/VSTMIA/VSTMDB/VPUSH/VPOP.  The encoding carries only "first
 * register + how many", so a register list must be a contiguous run -- {s0-s3}
 * or {s8, s9}, never {s0, s2}.  All bit patterns below were taken from
 * arm-none-eabi-as output (see tests/asm_cases/09_vfp_ldstm.s).
 *
 *   hw1 = 1110 110 P U D W L Rn          0xEC00 | P<<8 | U<<7 | D<<6 | W<<5 | L<<4 | Rn
 *   hw2 = Vd 101 1 imm8  (double)        0x0B00 | count*2
 *         Vd 101 0 imm8  (single)        0x0A00 | count
 * IA sets U, DB sets P; VPUSH is VSTMDB sp! and VPOP is VLDMIA sp!.
 * D16-D31 are reachable through the D bit, but they only exist on an FPU with
 * 32 double registers (fpv5-d32); on fpv5-d16 the hardware has d0-d15 only. */
typedef struct ArmVfpList {
    int first;      /* first register number, -1 = none */
    int count;      /* how many registers */
    int is_double;  /* d-registers rather than s-registers */
    int ok;
} ArmVfpList;

static int parse_vfp_num_a(const char **p) {
    int v = 0, seen = 0;
    while (**p >= '0' && **p <= '9') { v = v * 10 + (**p - '0'); (*p)++; seen = 1; }
    return seen ? v : -1;
}

static ArmVfpList parse_vfplist_a(ArmAsm *a, const char **p, int quiet) {
    ArmVfpList l;
    int last;
    l.first = -1; l.count = 0; l.is_double = 0; l.ok = 0;
    last = -1;
    skip_space_a(p);
    if (**p != '{') {
        if (!quiet) {
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "a VFP register list looks like {s0-s3} or {d0-d1}", 0,0,0,0,0,0);
            a->hard_errors++;
        }
        return l;
    }
    (*p)++;
    for (;;) {
        int start, end, is_dbl;
        skip_space_a(p);
        if (**p == '}') { (*p)++; break; }
        if (**p != 's' && **p != 'S' && **p != 'd' && **p != 'D') {
            if (!quiet) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "a VFP register list names s0-s31 or d0-d31", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            while (**p && **p != '}') (*p)++;
            if (**p == '}') (*p)++;
            l.count = 0;
            return l;
        }
        is_dbl = (**p == 'd' || **p == 'D');
        (*p)++;
        start = parse_vfp_num_a(p);
        skip_space_a(p);
        if (**p == '-') {
            (*p)++; skip_space_a(p);
            /* GAS writes the kind on both ends: {s0-s3}, not {s0-3}. */
            if (**p == 's' || **p == 'S' || **p == 'd' || **p == 'D') (*p)++;
            end = parse_vfp_num_a(p);
        }
        else            end = start;
        if (start < 0 || end < 0 || end < start) {
            if (!quiet) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "malformed VFP register range", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            while (**p && **p != '}') (*p)++;
            if (**p == '}') (*p)++;
            l.count = 0;
            return l;
        }
        if (l.first < 0) { l.first = start; l.is_double = is_dbl; }
        else if (is_dbl != l.is_double || start != last + 1) {
            /* One encoding = one contiguous run of one kind.  Anything else
             * would silently load the wrong registers. */
            if (!quiet) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "a VFP load/store list must be one contiguous run of "
                          "the same register kind ({s0-s3}, {d0-d1}); the "
                          "encoding has no way to name a gap", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            l.count = 0;
        }
        last = end;
        if (l.count >= 0 && l.first >= 0) l.count = last - l.first + 1;
        skip_space_a(p);
        if (**p == ',') (*p)++;
    }
    if (l.first >= 0) {
        int top = l.is_double ? 31 : 31;
        if (last < 0 || last > top) {
            if (!quiet) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "VFP register number out of range (s0-s31, d0-d31)", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            l.count = 0;
        }
        if (l.is_double && l.count > 16) {
            if (!quiet) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "a VFP store/load moves at most 16 double registers", 0,0,0,0,0,0);
                a->hard_errors++;
            }
        }
    }
    l.ok = (l.count > 0);
    return l;
}

static void emit_vfp_ldstm_a(ArmAsm *a, int is_load, int is_db, int wb, int rn,
                             const ArmVfpList *l) {
    unsigned hw1, hw2;
    int vd, dbit, words;
    if (!l->ok || rn < 0 || rn > 15) {
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "VLDM/VSTM needs a base register and a register list", 0,0,0,0,0,0);
        a->hard_errors++;
        out16(a, 0xBF00); out16(a, 0xBF00);
        return;
    }
    if (l->is_double) {
        vd = l->first & 0xF;
        dbit = (l->first >> 4) & 1;
        words = l->count * 2;
    } else {
        vd = l->first >> 1;
        dbit = l->first & 1;
        words = l->count;
    }
    hw1 = 0xEC00u | (unsigned)(is_db ? 0x100 : 0) | (unsigned)(is_db ? 0 : 0x80) |
          (unsigned)(dbit << 6) | (unsigned)(wb ? 0x20 : 0) |
          (unsigned)(is_load ? 0x10 : 0) | (unsigned)(rn & 0xF);
    hw2 = ((unsigned)(vd & 0xF) << 12) |
          (unsigned)(l->is_double ? 0x0B00 : 0x0A00) | (unsigned)(words & 0xFF);
    out16(a, hw1);
    out16(a, hw2);
}

#endif /* ARKCC_FEATURE_ASM_VFP_LDSTM */

static void pass2_a(ArmAsm *a) {
    a->pass2 = 1;
    const char *p = a->src;
    a->pc = g_arm_vt_reserve ? ARM_VT_SIZE : 0;
    a->ref_count = 0;
    while (*p) {
        char first[64];
        int n;
        skip_space_a(&p);
        if (*p == '\n') { p++; continue; }
        if (*p == ';') { skip_to_eol_a(&p); continue; }
        if (*p == '\0') break;
        n = read_word_a(&p, first, (int)sizeof(first));
        canon_alias_a(first);
        if (n == 0) { skip_to_eol_a(&p); continue; }
        skip_space_a(&p);
        if (*p == ':') {
            p++;
            skip_space_a(&p);
            if (*p == '\n' || *p == ';' || *p == '\0') {
                if (*p == ';') skip_to_eol_a(&p);
                if (*p == '\n') p++;
                continue;
            }
            n = read_word_a(&p, first, (int)sizeof(first));
            canon_alias_a(first);
            if (n == 0) { skip_to_eol_a(&p); continue; }
        }
        if (first[0] == '.') {
            /* Pseudo-directives: emit data for .zero/.word/.byte/.asciz, skip metadata */
            if (strieq_a(first, ".set") || strieq_a(first, ".equ")) {
                /* Already processed in pass1; skip */
            } else if (strieq_a(first, ".zero") || strieq_a(first, ".skip") || strieq_a(first, ".space")) {
                int sz = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { sz = sz * 10 + (*p - '0'); p++; }
                while (sz > 0 && a->pc < a->out_cap) { a->out[a->pc++] = 0; sz--; }
            } else if (strieq_a(first, ".word") || strieq_a(first, ".long")) {
                /* Output 4-byte values (comma-separated).
                 * Bug-A5 FIX: Also resolve symbol names (e.g. .word funcname
                 * for .init_array/.fini_array constructor/destructor entries)
                 * by looking them up in the label table. Previously only
                 * numeric literals were parsed, so .word funcname emitted
                 * 0x00000000, causing constructor iteration to jump to
                 * address 0 and HardFault. */
                while (*p && *p != '\n') {
                    int val = 0;
                    skip_space_a(&p);
                    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                        p += 2;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            char c = *p;
                            int v = (c >= '0' && c <= '9') ? c - '0' :
                                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                            val = val * 16 + v;
                            p++;
                        }
                    } else if (*p == '-') {
                        p++;
                        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                        val = -val;
                    } else if (*p >= '0' && *p <= '9') {
                        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                    } else {
                        /* Bug-A5: Token is a symbol name, not a number.
                         * Read the name and look it up in the label table. */
                        char sym_name[128];
                        int sn = 0;
                        while (*p && *p != ',' && *p != '\n' &&
                               *p != ' ' && *p != '\t' &&
                               sn < (int)sizeof(sym_name) - 1) {
                            sym_name[sn++] = *p++;
                        }
                        sym_name[sn] = 0;
                        if (sn > 0) {
                            int addr = find_label_a(a, sym_name);
                            if (addr < 0) addr = find_set_a(a, sym_name);
                            if (addr >= 0) {
                                val = addr;
                            } else {
                                /* Symbol not yet defined; record a 4-byte
                                 * absolute reference (type 4 = R_ARM_ABS32)
                                 * at the current pc for late resolution
                                 * by resolve_refs_a. */
                                add_ref_a(a, a->pc, sym_name, 4);
                            }
                        }
                    }
                    if (a->pc + 3 < a->out_cap) {
                        a->out[a->pc++] = (unsigned char)(val & 0xFF);
                        a->out[a->pc++] = (unsigned char)((val >> 8) & 0xFF);
                        a->out[a->pc++] = (unsigned char)((val >> 16) & 0xFF);
                        a->out[a->pc++] = (unsigned char)((val >> 24) & 0xFF);
                    }
                    skip_space_a(&p);
                    if (*p == ',') p++;
                }
            } else if (strieq_a(first, ".arkdata")) {
                /* --reloc-data: the next instruction is a MOVW/MOVT pair whose
                 * value is an address inside this unit's .data/.bss window.
                 * Record its code offset as relocation type 6 so -link can add
                 * the object's data delta when it has to move the window.
                 * Pass-1 ignores this directive: it occupies no bytes, so the
                 * offsets it records are the ones the image really has. */
                add_ref_a(a, a->pc, ".arkdata", 6);
            } else if (strieq_a(first, ".byte")) {
                while (*p && *p != '\n') {
                    int val = 0;
                    skip_space_a(&p);
                    if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) {
                        p += 2;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            char c = *p;
                            int v = (c >= '0' && c <= '9') ? c - '0' :
                                    (c >= 'a' && c <= 'f') ? c - 'a' + 10 : c - 'A' + 10;
                            val = val * 16 + v;
                            p++;
                        }
                    } else if (*p == '\'') {
                        p++;
                        val = *p;
                        p++;
                        if (*p == '\'') p++;
                    } else {
                        while (*p >= '0' && *p <= '9') { val = val * 10 + (*p - '0'); p++; }
                    }
                    if (a->pc < a->out_cap) a->out[a->pc++] = (unsigned char)(val & 0xFF);
                    skip_space_a(&p);
                    if (*p == ',') p++;
                }
            } else if (strieq_a(first, ".asciz") || strieq_a(first, ".ascii")) {
                int is_asciz = strieq_a(first, ".asciz");
                skip_space_a(&p);
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"' && *p != '\n') {
                        unsigned char c;
                        if (*p == '\\') {
                            p++;
                            switch (*p) {
                                case 'n': c = '\n'; break;
                                case 't': c = '\t'; break;
                                case 'r': c = '\r'; break;
                                case '\\': c = '\\'; break;
                                case '"': c = '"'; break;
                                case '0': c = '\0'; break;
                                case 'x': {
                                    int hv = 0;
                                    p++;
                                    if (*p && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
                                        char ch = *p;
                                        hv = (ch >= '0' && ch <= '9') ? ch - '0' :
                                             (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : ch - 'A' + 10;
                                        p++;
                                        if (*p && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
                                            ch = *p;
                                            hv = hv * 16 + ((ch >= '0' && ch <= '9') ? ch - '0' :
                                                 (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : ch - 'A' + 10);
                                            p++;
                                        }
                                    }
                                    c = (unsigned char)hv;
                                    p--;  /* compensate for p++ below */
                                    break;
                                }
                                default: c = (unsigned char)*p; break;
                            }
                            p++;
                        } else {
                            c = (unsigned char)*p;
                            p++;
                        }
                        if (a->pc < a->out_cap) a->out[a->pc++] = c;
                    }
                    if (*p == '"') p++;
                }
                if (is_asciz) {
                    if (a->pc < a->out_cap) a->out[a->pc++] = 0;
                    /* Align to 2 to match pass1 */
                    if (a->pc & 1) { if (a->pc < a->out_cap) a->out[a->pc++] = 0; }
                } else {
                    if (a->pc & 1) { if (a->pc < a->out_cap) a->out[a->pc++] = 0; }
                }
            } else if (strieq_a(first, ".align") || strieq_a(first, ".balign")) {
                int align = 0;
                skip_space_a(&p);
                if (*p == '#') p++;
                while (*p >= '0' && *p <= '9') { align = align * 10 + (*p - '0'); p++; }
                if (align > 1) {
                    int rem = a->pc % align;
                    if (rem != 0) {
                        int pad = align - rem;
                        while (pad > 0 && a->pc < a->out_cap) { a->out[a->pc++] = 0; pad--; }
                    }
                }
            }
            /* .section, .data, .bss, .text, .global, .type, .size, .thumb, .file, .loc,
             * .cfi_* etc. are metadata-only, skip */
            skip_to_eol_a(&p);
            if (*p == '\n') p++;
            continue;
        }
        skip_space_a(&p);
        /* Optional width suffix: "adds.w", "b.n", "lsls.w".  GAS accepts it and
         * hand-written sources use it; without this the suffix is read as the
         * first operand and every such instruction encodes as garbage.  Only
         * W/N are width hints -- a dotted suffix like ".F32" belongs to the
         * mnemonic and is read by that mnemonic's own encoder. */
        if (*p == '.' && (my_toupper_a(p[1]) == 'W' || my_toupper_a(p[1]) == 'N') &&
            !is_alnum_a(p[2])) {
            p += 2;
            skip_space_a(&p);
        }

        if (strieq_a(first, "PUSH")) {
            int has_lr=0, has_pc=0;
            int list = parse_reglist_a(a, &p, &has_lr, &has_pc);
            if (has_pc) {
                /* STM never stores pc, and the 16-bit PUSH bit at 8 means LR.
                 * Dropping it would leave the reader believing pc was saved. */
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "PUSH cannot store pc (only lr has a bit in this form)",
                          0,0,0,0,0,0);
                a->hard_errors++;
            } else if (list & 0x1F00) {
                /* A single high register cannot use the block form: QEMU (and
                 * whatever rules its Thumb-2 decoder follows) rejects
                 * "stmdb sp!, {r8}" = e92d 0100 outright, and GNU as itself
                 * canonicalises "pop {ip}" to the single-register form below
                 * rather than to e8bd 1000.  Both encodings are 4 bytes, so
                 * pass 1 does not have to know which one is chosen. */
                if ((list & (list - 1)) == 0 && !has_lr) {
                    int rt = 0;
                    while (rt < 16 && !(list & (1 << rt))) rt++;
                    out16(a, 0xF84D);
                    out16(a, ((unsigned)(rt & 0xF) << 12) | 0x0D00u | 4u);
                } else {
                    /* T2 STMDB SP!: can encode r0-r12 + lr */
                    unsigned w2 = (unsigned)(has_lr ? 0x4000 : 0) | (unsigned)(list & 0x1FFF);
                    out16(a, 0xE92D); out16(a, w2);
                }
            } else {
                out16(a, 0xB400 | (has_lr ? 0x0100 : 0) | (list & 0xFF));
            }
        }
        else if (strieq_a(first, "POP")) {
            int has_lr=0, has_pc=0;
            int list = parse_reglist_a(a, &p, &has_lr, &has_pc);
            /* The 16-bit POP has one bit at 8 and it means PC, not LR: the two
             * cannot be expressed in T1 at all.  This used to drop the register
             * on the floor (POP {r5,r6,r7,lr} assembled to bce0, i.e.
             * POP {r5,r6,r7}), which is a silently wrong return sequence -- the
             * variadic epilogue is exactly that shape and lost 4 bytes of stack
             * plus the restored lr on every return.  Refuse the impossible
             * combination instead of picking one. */
            if (has_lr && has_pc) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "POP cannot load both lr and pc", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if ((list & 0x1F00) || has_lr) {
                /* Single high register: see the note on PUSH above.  "POP {ip}"
                 * is emitted by the indirect-call path, and e8bd 1000 does not
                 * execute (measured: UC_ERR_INSN_INVALID), so use
                 * "ldr.w Rt, [sp], #4" = f85d t004. */
                if ((list & (list - 1)) == 0 && !has_pc && !has_lr) {
                    int rt = 0;
                    while (rt < 16 && !(list & (1 << rt))) rt++;
                    out16(a, 0xF85D);
                    out16(a, ((unsigned)(rt & 0xF) << 12) | 0x0B00u | 4u);
                } else {
                    /* T2 LDMIA SP!: can encode r0-r12 + lr or pc */
                    unsigned w2 = (unsigned)(has_pc ? 0x8000 : 0) |
                                  (unsigned)(has_lr ? 0x4000 : 0) |
                                  (unsigned)(list & 0x1FFF);
                    out16(a, 0xE8BD); out16(a, w2);
                }
            } else {
                out16(a, 0xBC00 | (has_pc ? 0x0100 : 0) | (list & 0xFF));
            }
        }
        else if (strieq_a(first, "MOV")) {
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255) {
                    out16(a, 0x2000 | ((rd & 7) << 8) | imm);  /* MOVS rd, #imm8 */
                } else if (rd >= 0 && imm >= 0 && imm <= 0xFFFF) {
                    /* 16-bit MOVS cannot name r8-r15 and its imm8 would turn
                     * "MOV r0, #300" into 44.  MOVW carries the whole value
                     * into any register (and does not touch the flags). */
                    emit_movw_a(a, rd, imm);
                } else {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "MOV immediate is out of range (0..65535) at code offset %s",
                              diag_num(b1, a->pc), 0,0,0,0,0);
                    a->hard_errors++;
                    emit_movw_a(a, rd < 0 ? 0 : rd, 0);
                }
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x4600 | (((rd >> 3) & 1) << 7) | ((rm & 0xF) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "MOVEQ") || strieq_a(first, "MOVNE") ||
                 strieq_a(first, "MOVLT") || strieq_a(first, "MOVGT") ||
                 strieq_a(first, "MOVLE") || strieq_a(first, "MOVGE") ||
                 strieq_a(first, "MOVLO") || strieq_a(first, "MOVHI") ||
                 strieq_a(first, "MOVLS") || strieq_a(first, "MOVHS") ||
                 strieq_a(first, "MOVCS") || strieq_a(first, "MOVCC")) {
            /* Conditional MOV in IT block.
             * Immediate form: 32-bit MOV.W Rd, #imm8 (0xF04F base, no S bit,
             *   avoids UNPREDICTABLE MOVS-in-IT-block on ARMv7-M).
             * Register form: 16-bit MOV Rd, Rm (0x4600, no S bit, safe in IT). */
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                /* 32-bit T2 MOV.W: 1111 0 i 0 0010 0 S 1111 | 0 imm3 Rd imm8
                 * For imm 0-255: i=0, imm3=0, S=0, Rn=1111 (MOV alias) */
                unsigned hi = 0xF04Fu;
                unsigned lo = ((unsigned)(rd & 0xF) << 8) | (unsigned)(imm & 0xFF);
                out16(a, hi);
                out16(a, lo);
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x4600 | (((rd >> 3) & 1) << 7) | ((rm & 0xF) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "MOVW")) {
            int rd = parse_reg_a(&p);
            int imm;
            eat_comma_a(&p);
            imm = parse_imm_a(&p) & 0xFFFF;
            {
                unsigned ii = (imm >> 11) & 1;
                unsigned imm4 = (imm >> 12) & 0xF;
                unsigned imm3 = (imm >> 8) & 0x7;
                unsigned imm8 = imm & 0xFF;
                unsigned hi = 0xF240u | (ii << 10) | imm4;
                unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                out16(a, hi); out16(a, lo);
            }
        }
        else if (strieq_a(first, "MOVT")) {
            int rd = parse_reg_a(&p);
            int imm;
            eat_comma_a(&p);
            imm = parse_imm_a(&p) & 0xFFFF;
            {
                unsigned ii = (imm >> 11) & 1;
                unsigned imm4 = (imm >> 12) & 0xF;
                unsigned imm3 = (imm >> 8) & 0x7;
                unsigned imm8 = imm & 0xFF;
                unsigned hi = 0xF2C0u | (ii << 10) | imm4;
                unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                out16(a, hi); out16(a, lo);
            }
        }
        else if (strieq_a(first, "VLDMIA") || strieq_a(first, "VLDM") ||
                 strieq_a(first, "VLDMDB") || strieq_a(first, "VSTMIA") ||
                 strieq_a(first, "VSTM") || strieq_a(first, "VSTMDB") ||
                 strieq_a(first, "VPUSH") || strieq_a(first, "VPOP")) {
#if !ARKCC_FEATURE_ASM_VFP_LDSTM
            /* Trimmed on the board: the backend never emits these, so the only
             * loss is assembling hand-written .s.  Say so instead of letting the
             * mnemonic fall through as "unknown". */
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "VFP multi-register load/store is not compiled into this "
                      "build (ARKCC_FEATURE_ASM_VFP_LDSTM=0)", 0,0,0,0,0,0);
            a->hard_errors++;
            out16(a, 0xBF00); out16(a, 0xBF00);
#else
            int is_load, is_db = 0, wb = 0, rn;
            ArmVfpList vl;
            if (strieq_a(first, "VPUSH") || strieq_a(first, "VPOP")) {
                is_load = strieq_a(first, "VPOP");
                is_db = !is_load;             /* VPUSH = VSTMDB sp!, VPOP = VLDMIA sp! */
                wb = 1;
                rn = 13;
                eat_comma_a(&p);
            } else {
                is_load = (my_toupper_a(first[1]) == 'L');
                is_db = (strieq_a(first, "VLDMDB") || strieq_a(first, "VSTMDB"));
                rn = parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == '!') { wb = 1; p++; }
                eat_comma_a(&p);
            }
            vl = parse_vfplist_a(a, &p, 0);
            emit_vfp_ldstm_a(a, is_load, is_db, wb, rn, &vl);
#endif
        }
        else if (strieq_a(first, "ADDW") || strieq_a(first, "SUBW")) {
            /* Always the 32-bit form with a plain 12-bit immediate: never the
             * 16-bit ADD/SUB (which sets the flags) and never the rotated T3
             * immediate.  That is the whole point of spelling ADDW. */
            int is_sub_w = strieq_a(first, "SUBW");
            int rd = parse_reg_a(&p);
            int rn, imm = -1;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            if (rn < 0) rn = rd;
            eat_comma_a(&p);
            if (*p == '#') imm = parse_imm_a(&p);
            if (rd < 0 || rn < 0 || imm < 0 || imm > 4095) {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "ADDW/SUBW needs Rd, Rn and a 0..4095 immediate at code offset %s",
                          diag_num(b1, a->pc), 0,0,0,0,0);
                a->hard_errors++;
                emit_addw_a(a, is_sub_w, rd < 0 ? 0 : rd, rn < 0 ? 0 : rn, 0);
            } else {
                emit_addw_a(a, is_sub_w, rd, rn, imm);
            }
        }
        else if (strieq_a(first, "ADD")) {
            int rd = parse_reg_a(&p); int rn;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rn < 0) rn = rd;      /* "ADD r0, #5" == "ADD r0, r0, #5" */
                if (rd == 13 && rn == 13 && imm >= 0 && imm <= 508 && (imm & 3) == 0)
                    out16(a, 0xB000 | (imm >> 2));            /* ADD sp, #imm7*4 */
                else if (rd == rn && rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255)
                    out16(a, 0x3000 | ((rd & 7) << 8) | imm);  /* ADDS rdn, #imm8 */
                else if (imm >= 0 && imm <= 7 && rd >= 0 && rd <= 7 && rn >= 0 && rn <= 7)
                    out16(a, 0x1C00 | ((imm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
                else if (rd >= 0 && rn >= 0 && imm >= 0 && imm <= 4095)
                    emit_addw_a(a, 0, rd, rn, imm);       /* ADDW, plain imm12 */
                else {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "ADD immediate is out of range (0..4095) at code offset %s",
                              diag_num(b1, a->pc), 0,0,0,0,0);
                    a->hard_errors++;
                    emit_addw_a(a, 0, rd, rn, 0);
                }
            } else {
                /* Register forms: `ADD Rd, Rn, Rm` (T1) and the two-operand
                 * `ADD Rdn, Rm` (T2). The two-operand form used to fall into
                 * the three-operand encoder, which then read a *third* register
                 * operand past the end of the line: parse_reg_a returns -1
                 * there and (-1 & 7) == 7, so `ADD r1, r4` silently assembled
                 * as `ADD r1, r4, r7`. That is not theoretical -- __aeabi_lmul
                 * is written in exactly this shape, so every 64-bit multiply
                 * lost its carry fix-up: 0x123456789ABCDEF0 * 2 came out with
                 * a high word 1 too small (measured in unicorn). */
                int rm = parse_reg_a(&p);
                if (rd >= 0 && rn >= 0 && rm >= 0) {
                    if (rd <= 7 && rn <= 7 && rm <= 7) {
                        /* T1 -- note it sets the flags, which is exactly what
                         * the 64-bit helpers' "ADD low / ADC high" pair wants,
                         * and what this encoder has always done. */
                        out16(a, 0x1800 | ((rm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
                    } else {
                        /* T2: any registers, no flags.  The T1 path used to mask
                         * the operands with &7 here and silently add the wrong
                         * ones ("ADD ip, r6, ip" became "adds r4, r6, r4"). */
                        emit_addsub_reg_w_a(a, 0, rd, rn, rm);
                    }
                } else if (rm < 0 && rd >= 0 && rn >= 0) {
                    /* ADD (register) T2: 0100 0100 D Rm Rdn -- Rd = Rd + Rm,
                     * and unlike T1 it accepts the high registers r8-r15. */
                    out16(a, 0x4400 | (((rd >> 3) & 1) << 7) | ((rn & 0xF) << 3) | (rd & 7));
                } else {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "ADD needs a register or immediate operand at code offset %s",
                              diag_num(b1, a->pc), 0, 0, 0, 0, 0);
                    a->hard_errors++;
                    out16(a, 0xBF00);
                }
            }
        }
        else if (strieq_a(first, "SUB")) {
            int rd = parse_reg_a(&p); int rn;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rn < 0) rn = rd;      /* "SUB sp, #12" == "SUB sp, sp, #12" */
                if (rd == 13 && rn == 13 && imm >= 0 && imm <= 508 && (imm & 3) == 0)
                    out16(a, 0xB080 | (imm >> 2));            /* SUB sp, #imm7*4 */
                else if (rd == rn && rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255)
                    out16(a, 0x3800 | ((rd & 7) << 8) | imm);  /* SUBS rdn, #imm8 */
                else if (imm >= 0 && imm <= 7 && rd >= 0 && rd <= 7 && rn >= 0 && rn <= 7)
                    out16(a, 0x1E00 | ((imm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
                else if (rd >= 0 && rn >= 0 && imm >= 0 && imm <= 4095)
                    emit_addw_a(a, 1, rd, rn, imm);       /* SUBW, plain imm12 */
                else {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "SUB immediate is out of range (0..4095) at code offset %s",
                              diag_num(b1, a->pc), 0,0,0,0,0);
                    a->hard_errors++;
                    emit_addw_a(a, 1, rd, rn, 0);
                }
            } else {
                /* Same two shapes as ADD: `SUB Rd, Rn, Rm` (T1) and the
                 * two-operand `SUB Rdn, Rm`. Thumb has no high-register
                 * two-operand subtract, so that case is rejected rather than
                 * emitted with a silently wrong register. */
                int rm = parse_reg_a(&p);
                if (rd >= 0 && rn >= 0 && rm >= 0) {
                    if (rd <= 7 && rn <= 7 && rm <= 7) {
                        out16(a, 0x1A00 | ((rm & 7) << 6) | ((rn & 7) << 3) | (rd & 7));
                    } else {
                        emit_addsub_reg_w_a(a, 1, rd, rn, rm);
                    }
                } else if (rm < 0 && rd >= 0 && rd <= 7 && rn >= 0 && rn <= 7) {
                    /* SUB Rdn, Rm is SUB Rdn, Rdn, Rm */
                    out16(a, 0x1A00 | ((rn & 7) << 6) | ((rd & 7) << 3) | (rd & 7));
                } else if (rm < 0 && rd >= 0 && rn >= 0) {
                    /* high registers: no 16-bit two-operand SUB exists */
                    emit_addsub_reg_w_a(a, 1, rd, rd, rn);
                } else {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "two-operand SUB needs low registers at code offset %s",
                              diag_num(b1, a->pc), 0, 0, 0, 0, 0);
                    a->hard_errors++;
                    out16(a, 0xBF00);
                }
            }
        }
        else if (strieq_a(first, "MUL")) {
            /* MULS T1 (16-bit): MULS <Rdn>, <Rn>, <Rdm> with Rd == Rn.
             * The two-operand form MULS Rd, Rm is the same encoding (Rn = Rd).
             * Three distinct registers need the 32-bit MUL T2 form, which this
             * encoder does not emit: reject it loudly instead of silently
             * dropping the third operand (MOD codegen emits "MUL r2, r2, r1"). */
            int rd = parse_reg_a(&p); int rn, rm, second;
            eat_comma_a(&p); second = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                rn = second;                  /* three-operand: Rd, Rn, Rm */
                eat_comma_a(&p); rm = parse_reg_a(&p);
            } else {
                rn = rd;                      /* two-operand: Rd = Rd * Rm */
                rm = second;
            }
            if (rd != rn) {
                {
                    char b1[12], b2[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "MUL with three distinct registers (Rd=r%s, Rn=r%s) "
                              "needs the 32-bit form, not supported",
                              diag_num(b1, rd), diag_num(b2, rn), 0,0,0,0);
                }
                a->hard_errors++;
            }
            out16(a, 0x4340 | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "ADC") || strieq_a(first, "SBC")) {
            /* Carry-propagating add/subtract.  16-bit Thumb only has the
             * two-operand shape and always sets the flags:
             *   ADCS Rd, Rm   ==  Rd = Rd + Rm + C
             *   SBCS Rd, Rm   ==  Rd = Rd - Rm - !C
             * so the three-operand spelling "ADC Rd, Rn, Rm" is only
             * representable when Rd == Rn (the 32-bit form is not
             * emitted).  This branch used to read only two operands: the
             * third was dropped and the second encoded as the addend, so
             * "ADC r1, r1, r3" (what the 64-bit add tail emits) became
             * "ADCS r1, r1" -- the high word of every 64-bit add/sub was
             * silently off by one (measured: -100LL + 3LL carried
             * 0xFFFFFFFE into the high word and __aeabi_lcmp then said the
             * halves differed). */
            int is_sbc = strieq_a(first, "SBC");
            int rd = parse_reg_a(&p);
            int rn, rm, second;
            eat_comma_a(&p);
            second = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                rn = second;              /* three operands: Rd, Rn, Rm */
                eat_comma_a(&p);
                rm = parse_reg_a(&p);
            } else {
                rn = rd;                  /* two operands: Rd = Rd +/- Rm */
                rm = second;
            }
            if (rd >= 0 && rm >= 0 && rd != rn) {
                if (a->pass2) {
                    char b1[12], b2[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "ADC/SBC with three distinct registers "
                              "(Rd=r%s, Rn=r%s) needs the 32-bit form, "
                              "not supported",
                              diag_num(b1, rd), diag_num(b2, rn), 0,0,0,0);
                    a->hard_errors++;
                }
                rm = rn;                  /* keep the encoding meaningful */
            }
            if (rd >= 0 && rm >= 0) {
                out16(a, (is_sbc ? 0x4180 : 0x4140) | ((rm & 7) << 3) | (rd & 7));
            } else {
                if (a->pass2) {
                    char b1[12];
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "ADC/SBC needs two register operands at "
                              "code offset %s",
                              diag_num(b1, a->pc), 0,0,0,0,0);
                    a->hard_errors++;
                }
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "UMULL")) {
            /* UMULL T1 (32-bit): UMULL RdLo, RdHi, Rn, Rm
             * Encoding: 1111 1011 1010 Rn | RdLo RdHi 0000 Rm */
            int rdlo = parse_reg_a(&p); int rdhi, rn, rm;
            eat_comma_a(&p); rdhi = parse_reg_a(&p);
            eat_comma_a(&p); rn = parse_reg_a(&p);
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0xFBA0 | (rn & 0xF));
            out16(a, ((rdlo & 0xF) << 12) | ((rdhi & 0xF) << 8) | (rm & 0xF));
        }
        else if (strieq_a(first, "SDIV") || strieq_a(first, "UDIV")) {
            int rd = parse_reg_a(&p);
            int rn, rm;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            {
                unsigned op = strieq_a(first, "SDIV") ? 0xFB90u : 0xFBB0u;
                out16(a, op | (rn & 0xF));
                out16(a, 0xF0F0u | ((rd & 0xF) << 8) | (rm & 0xF));
            }
        }
        else if (strieq_a(first, "AND") || strieq_a(first, "ORR") ||
                 strieq_a(first, "EOR") || strieq_a(first, "BIC") ||
                 strieq_a(first, "MVN")) {
            (void)logic_dp_a(a, first, &p, 1);
        }
        else if (strieq_a(first, "LSL")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                /* LSL rd, rm, #imm5 (T1 immediate shift) */
                p++; skip_space_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p) & 0x1F;
                    out16(a, (unsigned)(imm << 6) | ((rm & 7) << 3) | (rd & 7));
                } else {
                    int rm2 = parse_reg_a(&p);
                    out16(a, 0x4080 | ((rm2 & 7) << 3) | (rd & 7));
                }
            } else {
                /* LSL rd, rm (T1 register shift) */
                out16(a, 0x4080 | ((rm & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "LSR")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                /* LSR rd, rm, #imm5 (T1 immediate shift; imm5=0 means 32) */
                p++; skip_space_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    int imm5 = (imm >= 32) ? 0 : (imm & 0x1F);
                    out16(a, 0x0800 | (unsigned)(imm5 << 6) | ((rm & 7) << 3) | (rd & 7));
                } else {
                    int rm2 = parse_reg_a(&p);
                    out16(a, 0x40C0 | ((rm2 & 7) << 3) | (rd & 7));
                }
            } else {
                /* LSR rd, rm (T1 register shift) */
                out16(a, 0x40C0 | ((rm & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "ASR")) {
            /* ASR T1: ASRS Rd, Rm, #imm5 (immediate) or ASRS Rd, Rs (register) */
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                /* ASR rd, rm, #imm5 (T1 immediate shift; imm5=0 means 32) */
                p++; skip_space_a(&p);
                if (*p == '#') {
                    int imm = parse_imm_a(&p);
                    int imm5 = (imm >= 32) ? 0 : (imm & 0x1F);
                    out16(a, 0x1000 | (unsigned)(imm5 << 6) | ((rm & 7) << 3) | (rd & 7));
                } else {
                    int rm2 = parse_reg_a(&p);
                    out16(a, 0x4100 | ((rm2 & 7) << 3) | (rd & 7));
                }
            } else {
                /* ASR rd, rm (T1 register shift) */
                out16(a, 0x4100 | ((rm & 7) << 3) | (rd & 7));
            }
        }
#if ARKCC_FEATURE_ASM_EXTRA
        else if (!ark_asm_extra_on() && ark_asm_extra_name(first)) {
            /* --no-asm-extra (or the feature compiled out): these mnemonics must
             * behave exactly as they did before the extension existed, i.e. be
             * rejected as unknown, so a build cannot quietly depend on them. */
            {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "unknown mnemonic %sq at code offset %s "
                          "(--asm-extra is off; turn it on to assemble it)",
                          first, diag_num(b1, a->pc), 0,0,0,0);
            }
            a->hard_errors++;
            out16(a, 0xBF00);
            skip_to_eol_a(&p);
        }
        else if (strieq_a(first, "ADDS") || strieq_a(first, "SUBS")) {
            /* --asm-extra: flag-setting aliases of ADD/SUB.
             * Three operand shapes have to be told apart before anything can be
             * encoded: `ADDS Rd, Rn, Rm`, `ADDS Rd, Rn, #imm`, and the two
             * operand forms `ADDS Rd, Rm` / `ADDS Rd, #imm` (Rd = Rd +/- x).
             * The 16-bit data-processing forms always set the flags, so they
             * encode exactly like ADD/SUB; only the 32-bit form carries S as a
             * real bit (bit 4 of the first halfword), which is why the plain
             * ADD.W must keep writing S=0 and this branch exists separately. */
            int is_sub = strieq_a(first, "SUBS");
            int rd = parse_reg_a(&p);
            int rn = rd;
            int rm = -1;
            int imm = -1;
            eat_comma_a(&p);
            if (*p == '#') {
                imm = parse_imm_a(&p);                 /* ADDS Rd, #imm */
            } else {
                int r2 = parse_reg_a(&p);
                skip_space_a(&p);
                if (*p == ',') {
                    p++; skip_space_a(&p);
                    rn = r2;
                    if (*p == '#') imm = parse_imm_a(&p);   /* ADDS Rd, Rn, #imm */
                    else           rm  = parse_reg_a(&p);   /* ADDS Rd, Rn, Rm */
                } else {
                    rm = r2;                                /* ADDS Rd, Rm */
                }
            }
            if (imm >= 0) {
                if (rd == 13 && rn == 13) {
                    out16(a, (is_sub ? 0xB080u : 0xB000u) | (((imm + 3) / 4) & 0x7F));
                } else if (rd == rn && imm >= 0 && imm <= 255) {
                    out16(a, (is_sub ? 0x3800u : 0x3000u) | ((rd & 7) << 8) | (imm & 0xFF));
                } else if (imm >= 0 && imm <= 7 && rd <= 7 && rn <= 7) {
                    out16(a, (is_sub ? 0x1E00u : 0x1C00u) | ((imm & 7) << 6) |
                             ((rn & 7) << 3) | (rd & 7));
                } else {
                    int i = (imm >> 11) & 1;
                    int imm3 = (imm >> 8) & 7;
                    unsigned base = is_sub ? 0xF1B0u : 0xF110u;  /* op=1101/1000, S=1 */
                    out16(a, base | (unsigned)(i << 10) | (unsigned)(rn & 0xF));
                    out16(a, (unsigned)((imm3 << 12) | ((rd & 0xF) << 8) | (imm & 0xFF)));
                }
            } else {
                out16(a, (is_sub ? 0x1A00u : 0x1800u) | ((rm & 7) << 6) |
                         ((rn & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "ANDS") || strieq_a(first, "ORRS") ||
                 strieq_a(first, "EORS") || strieq_a(first, "BICS") ||
                 strieq_a(first, "MVNS")) {
            /* --asm-extra: 16-bit data-processing forms all set the flags, so
             * ANDS/ORRS/EORS/BICS/MVNS are the same encodings as AND/ORR/EOR/
             * BIC/MVN with a register operand. */
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            {
                unsigned base;
                if (strieq_a(first, "ANDS"))      base = 0x4000u;
                else if (strieq_a(first, "EORS")) base = 0x4040u;
                else if (strieq_a(first, "ORRS")) base = 0x4300u;
                else if (strieq_a(first, "BICS")) base = 0x4380u;
                else                              base = 0x43C0u;   /* MVNS */
                out16(a, base | ((rm & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "MULS")) {
            /* --asm-extra: MULS Rd, Rm (Rd = Rd * Rm); the 16-bit MUL always
             * sets the flags, so this is MUL's encoding. */
            int rd = parse_reg_a(&p); int rn, rm, second;
            eat_comma_a(&p); second = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                rn = second;
                eat_comma_a(&p); rm = parse_reg_a(&p);
            } else {
                rn = rd;
                rm = second;
            }
            if (rd != rn) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "MULS with three distinct registers needs the 32-bit form, "
                          "which this assembler does not emit", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            out16(a, 0x4340u | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "LSLS") || strieq_a(first, "LSRS") ||
                 strieq_a(first, "ASRS")) {
            /* --asm-extra: flag-setting shifts.  Three shapes:
             *   LSLS Rd, Rn, #imm  -> 16-bit immediate form (always S)
             *   LSLS Rd, Rm        -> 16-bit register form (Rdn <<= Rm)
             *   LSLS Rd, Rn, Rm    -> 32-bit register form (S in bit 4 of hi)
             * The third shape must NOT be encoded as the 16-bit form: that form
             * shifts Rd by Rn's value, not Rm's, so it silently computes
             * something else (GAS emits lsls.w here; measured). */
            unsigned type;
            int rd = parse_reg_a(&p);
            int rn, rm = -1, imm = -1;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                p++; skip_space_a(&p);
                if (*p == '#') imm = parse_imm_a(&p);
                else           rm = parse_reg_a(&p);
            }
            if (strieq_a(first, "LSLS"))      type = 0;
            else if (strieq_a(first, "LSRS")) type = 1;
            else                              type = 2;
            if (imm >= 0) {
                unsigned imm5 = (unsigned)(imm & 0x1F);
                if (imm >= 32) imm5 = 0;             /* encoded as 32 */
                out16(a, (type << 11) | (imm5 << 6) | ((rn & 7) << 3) | (rd & 7));
            } else if (rm >= 0) {
                out16(a, 0xFA00u | (type << 5) | (1u << 4) |
                         (unsigned)(rn & 0xF));
                out16(a, 0xF000u | (unsigned)((rd & 0xF) << 8) | (unsigned)(rm & 0xF));
            } else {
                out16(a, 0x4080u | (type << 6) | ((rn & 7) << 3) | (rd & 7));
            }
        }
        else if (strieq_a(first, "ADR")) {
            /* --asm-extra: ADR Rd, label -- T1, imm8*4 relative to Align(PC,4). */
            int rd = parse_reg_a(&p);
            char name[64];
            eat_comma_a(&p);
            read_label_name_a(&p, name, sizeof(name));
            {
                int addr = find_label_a(a, name);
                if (addr < 0) {
                    add_ref_a(a, a->pc, name, 7);
                    out16(a, 0xA000u | ((rd & 7) << 8));
                } else {
                    int base = (a->pc + 4) & ~3;
                    int off = addr - base;
                    if (off < 0 || off > 1020 || (off & 3) != 0) {
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "ADR target %sq is %s bytes from the aligned PC; "
                                  "the 16-bit form covers 0..1020 in steps of 4",
                                  name, diag_num(b1, off), 0,0,0,0);
                        a->hard_errors++;
                    }
                    out16(a, 0xA000u | ((rd & 7) << 8) | (unsigned)((off >> 2) & 0xFF));
                }
            }
        }
        else if (strieq_a(first, "CBZ") || strieq_a(first, "CBNZ")) {
            /* --asm-extra: CBZ/CBNZ Rd, label -- T1, forward only, 0..62 bytes.
             * imm5 = offset/2 with bit 5 carried in bit 9 of the halfword. */
            int is_nz = strieq_a(first, "CBNZ");
            int rd = parse_reg_a(&p);
            char name[64];
            eat_comma_a(&p);
            read_label_name_a(&p, name, sizeof(name));
            {
                int addr = find_label_a(a, name);
                if (addr < 0) {
                    add_ref_a(a, a->pc, name, is_nz ? 9 : 8);
                    out16(a, (is_nz ? 0xB900u : 0xB100u) | (rd & 7));
                } else {
                    int off = addr - (a->pc + 4);
                    if (off < 0 || off > 62 || (off & 1) != 0) {
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "CBZ/CBNZ target %sq is out of the 16-bit forward "
                                  "range (0..62 bytes, even)", name, 0,0,0,0,0);
                        a->hard_errors++;
                    } else {
                        unsigned imm5 = (unsigned)(off >> 1);
                        out16(a, (is_nz ? 0xB900u : 0xB100u) |
                                 ((imm5 & 0x20u) << 4) | ((imm5 & 0x1Fu) << 3) | (rd & 7));
                    }
                }
            }
        }
        else if (strieq_a(first, "LDMIA") || strieq_a(first, "STMIA") ||
                 strieq_a(first, "LDM") || strieq_a(first, "STM") ||
                 strieq_a(first, "LDMDB") || strieq_a(first, "STMDB")) {
            /* --asm-extra: multiple load/store.
             *   T1  (16-bit) needs all four of: base in r0-r7, every register
             *       in the list in r0-r7, writeback on the base, increment-after.
             *   T2  (32-bit) covers the rest: high registers, lr/pc in the list,
             *       no writeback, and decrement-before.
             *     hw1 = 0xE880 | (DB<<7) | (W<<5) | (is_load<<4) | Rn
             *     hw2 = register list, bit n = rn, bit 14 = lr, bit 15 = pc
             *   Both bit patterns were taken from arm-none-eabi-as output and
             *   are checked instruction-for-instruction by tests/asm_cases. */
            int is_stm = (my_toupper_a(first[0]) == 'S');   /* source may be lowercase */
            int is_db  = (first[3] == 'D' || first[3] == 'd');
            int rn = parse_reg_a(&p);
            int has_lr = 0, has_pc = 0, list, has_bang = 0;
            skip_space_a(&p);
            if (*p == '!') { has_bang = 1; p++; }
            eat_comma_a(&p);          /* "r0!, {..}" - the comma is optional */
            list = parse_reglist_a(a, &p, &has_lr, &has_pc);
            if (rn < 0 || rn > 14) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "LDM/STM needs a base register of r0-r14", 0,0,0,0,0,0);
                a->hard_errors++;
                rn = 0;
            }
            if (list == 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "LDM/STM needs a non-empty register list", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (list & (1 << 13)) {
                /* Bit 13 is the one register a list cannot name: the ARM ARM
                 * makes sp in the list UNPREDICTABLE for every LDM/STM form. */
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "sp may not appear in a LDM/STM register list", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (rn >= 0 && rn <= 14 && (list & (1 << rn))) {
                /* ARM ARM: base register in the list together with writeback is
                 * UNPREDICTABLE; the core may store the old or the new base.
                 * arm-none-eabi-as refuses it, so refuse it here too. */
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "the base register may not appear in the list when the "
                          "instruction writes back (UNPREDICTABLE)", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (is_stm && has_pc) {
                /* ARM ARM: storing pc selects STM (user), which has no useful
                 * meaning on the M-profile; refusing beats encoding it. */
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "STM may not store pc", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (!is_db && has_bang && !has_lr && !has_pc && rn <= 7 &&
                list != 0 && (list & ~0xFF) == 0) {
                /* T1 */
                out16(a, (is_stm ? 0xC000u : 0xC800u) | ((rn & 7) << 8) | (list & 0xFF));
            } else {
                /* T2 */
                /* 0xE880 is the increment-after form; decrement-before clears
                 * U (bit 7) and sets P (bit 8) -> 0xE900.  Writing it as a
                 * base choice rather than an OR matters: bit 7 is already
                 * set in 0xE880, so "| 0x80" would encode IA. */
                unsigned hw1 = (unsigned)(is_db ? 0xE900 : 0xE880) |
                               (unsigned)(has_bang ? 0x20 : 0) |
                               (unsigned)(is_stm ? 0 : 0x10) |
                               (unsigned)(rn & 0xF);
                unsigned hw2 = (unsigned)(list & 0x1FFF) |
                               (unsigned)(has_lr ? 0x4000 : 0) |
                               (unsigned)(has_pc ? 0x8000 : 0);
                out16(a, hw1);
                out16(a, hw2);
            }
        }
        else if (strieq_a(first, "TBB") || strieq_a(first, "TBH")) {
            /* --asm-extra: table branch, 32-bit T1.
             *   TBB [Rn, Rm]        hw1 = 0xE8D0 | Rn   hw2 = 0xF000 | Rm
             *   TBH [Rn, Rm, LSL #1] hw1 = 0xE8D0 | Rn  hw2 = 0xF010 | Rm
             * Neither Rn nor Rm may be pc (UNPREDICTABLE), and TBB takes no
             * shift while TBH accepts exactly LSL #1 -- anything else would
             * change which byte is loaded, so it is an error, not a silence. */
            int is_tbh = strieq_a(first, "TBH");
            int rn, rm, shift = 0, has_shift = 0;
            skip_space_a(&p);
            if (*p == '[') { p++; skip_space_a(&p); }
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                char opn[8];
                p++;
                skip_space_a(&p);
                has_shift = 1;
                read_word_a(&p, opn, (int)sizeof(opn));
                skip_space_a(&p);
                if (*p == '#') shift = parse_imm_a(&p);
                if (!strieq_a(opn, "LSL") || shift != 1) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "TBH accepts only 'LSL #1' as its shift", 0,0,0,0,0,0);
                    a->hard_errors++;
                }
            }
            skip_space_a(&p);
            if (*p == ']') p++;
            check_no_index_writeback_a(a, &p);
            if (has_shift && !is_tbh) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "TBB takes no shift; use TBH [Rn, Rm, LSL #1]",
                          0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (rn < 0 || rn > 14 || rm < 0 || rm > 14) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "TBB/TBH needs two registers of r0-r14 (pc is "
                          "UNPREDICTABLE here)", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            out16(a, 0xE8D0u | (unsigned)(rn & 0xF));
            out16(a, (unsigned)(is_tbh ? 0xF010 : 0xF000) | (unsigned)(rm & 0xF));
        }
        else if (strieq_a(first, "LDRD") || strieq_a(first, "STRD")) {
            /* --asm-extra: LDRD/STRD Rt, Rt2, [Rn, #imm] -- 32-bit T1.
             *   hw1 = 0xE9C0 | (U<<7) | (is_load<<4) | Rn
             *   hw2 = (Rt<<12) | (Rt2<<8) | (|imm| >> 2)
             *   U=1 for a positive offset, U=0 for a negative one; the stored
             *   immediate is always the magnitude, hence the /4 round trip.
             * ARMv7-M makes an odd Rt, or Rt2 != Rt+1, UNPREDICTABLE, and
             * arm-none-eabi-as emits it anyway -- this assembler refuses, so a
             * typo cannot turn into a load of the wrong pair. */
            int is_load = (my_toupper_a(first[0]) == 'L');
            int rt = parse_reg_a(&p);
            int rt2, rn, imm = 0;
            eat_comma_a(&p);
            rt2 = parse_reg_a(&p);
            eat_comma_a(&p);
            skip_space_a(&p);
            if (*p == '[') { p++; skip_space_a(&p); }
            rn = parse_reg_a(&p);
            skip_space_a(&p);
            if (*p == ',') {
                p++;
                skip_space_a(&p);
                if (*p == '#') imm = parse_imm_a(&p);
                else {
                    /* Register-offset addressing ([Rn, Rm]) has no encoder
                     * here.  Ignoring Rm would silently address the base
                     * register instead, so refuse and let the author write
                     * the address arithmetic out. */
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "register-offset addressing ([Rn, Rm]) does "
                              "not exist for this instruction in Thumb; compute "
                              "the address with ADD first", 0,0,0,0,0,0);
                    a->hard_errors++;
                }
            }
            skip_space_a(&p);
            if (*p == ']') p++;
            check_no_index_writeback_a(a, &p);
            if (rn < 0 || rn > 15) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "LDRD/STRD needs a base register", 0,0,0,0,0,0);
                a->hard_errors++;
                rn = 0;
            }
            if (rt < 0 || rt2 < 0 || rt > 12 || rt2 > 14) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "LDRD/STRD needs two registers of r0-r14", 0,0,0,0,0,0);
                a->hard_errors++;
                rt = 0; rt2 = 1;
            } else if ((rt & 1) || rt2 != rt + 1) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "LDRD/STRD needs an even Rt and Rt2 == Rt+1 "
                          "(any other pair is UNPREDICTABLE on ARMv7-M)",
                          0,0,0,0,0,0);
                a->hard_errors++;
            }
            if ((imm & 3) != 0 || imm > 1020 || imm < -1020) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "LDRD/STRD offset must be a multiple of 4 in -1020..1020",
                          0,0,0,0,0,0);
                a->hard_errors++;
                imm = 0;
            }
            {
                int mag = imm < 0 ? -imm : imm;
                /* hw1 = 0xE940 | P(bit6) | U(bit7) | L(bit4) | Rn.
                 * U is 1 for a positive offset and 0 for a negative one;
                 * like LDM/STM above, the two bases differ in a bit that is
                 * set in both, so pick the base, do not OR a flag into it. */
                out16(a, (unsigned)(is_load ? (imm < 0 ? 0xE950 : 0xE9D0)
                                             : (imm < 0 ? 0xE940 : 0xE9C0)) |
                          (unsigned)(rn & 0xF));
                out16(a, (unsigned)((rt & 0xF) << 12) |
                          (unsigned)((rt2 & 0xF) << 8) |
                          (unsigned)((mag >> 2) & 0xFF));
            }
        }
        else if (strieq_a(first, "MLA") || strieq_a(first, "MLS")) {
            /* --asm-extra: MLA/MLS Rd, Rn, Rm, Ra -- 32-bit, hi=11111011000x Rn */
            int is_mls = strieq_a(first, "MLS");
            int rd = parse_reg_a(&p); int rn, rm, ra;
            eat_comma_a(&p); rn = parse_reg_a(&p);
            eat_comma_a(&p); rm = parse_reg_a(&p);
            eat_comma_a(&p); ra = parse_reg_a(&p);
            out16(a, 0xFB00u | (unsigned)(rn & 0xF));
            out16(a, ((unsigned)(ra & 0xF) << 12) | ((unsigned)(rd & 0xF) << 8) |
                     ((unsigned)(is_mls ? 1 : 0) << 4) | (unsigned)(rm & 0xF));
        }
        else if (strieq_a(first, "SMULL") || strieq_a(first, "SMLAL")) {
            /* --asm-extra: SMULL/SMLAL RdLo, RdHi, Rn, Rm -- 32-bit */
            int is_smlal = strieq_a(first, "SMLAL");
            int rdlo = parse_reg_a(&p); int rdhi, rn, rm;
            eat_comma_a(&p); rdhi = parse_reg_a(&p);
            eat_comma_a(&p); rn = parse_reg_a(&p);
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, (is_smlal ? 0xFBC0u : 0xFB80u) | (unsigned)(rn & 0xF));
            out16(a, ((unsigned)(rdlo & 0xF) << 12) | ((unsigned)(rdhi & 0xF) << 8) |
                     (unsigned)(rm & 0xF));
        }
        else if (strieq_a(first, "MRS") || strieq_a(first, "MSR")) {
            /* --asm-extra: MRS Rd, <spec> / MSR <spec>, Rn.
             * sysm: MSP=8, PSP=9, PRIMASK=16, FAULTMASK=17, BASEPRI=19,
             *       CONTROL=20.  Only these six are accepted: anything else
             *       would need a system register the M-profile core may not
             *       implement, and silently encoding a wrong sysm is worse than
             *       refusing. */
            int is_mrs = strieq_a(first, "MRS");
            char sp1[32], sp2[32];
            int sysm = -1;
            int rd, rn;
            if (is_mrs) {
                rd = parse_reg_a(&p);
                eat_comma_a(&p);
                read_label_name_a(&p, sp1, sizeof(sp1));
            } else {
                read_label_name_a(&p, sp2, sizeof(sp2));
                eat_comma_a(&p);
                rn = parse_reg_a(&p);
                rd = rn;
            }
            {
                const char *nm = is_mrs ? sp1 : sp2;
                if (strieq_a(nm, "MSP"))            sysm = 8;
                else if (strieq_a(nm, "PSP"))       sysm = 9;
                else if (strieq_a(nm, "PRIMASK"))   sysm = 16;
                else if (strieq_a(nm, "FAULTMASK")) sysm = 17;
                else if (strieq_a(nm, "BASEPRI"))   sysm = 19;
                else if (strieq_a(nm, "CONTROL"))   sysm = 20;
            }
            if (sysm < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "MRS/MSR supports MSP, PSP, PRIMASK, FAULTMASK, "
                          "BASEPRI and CONTROL only", 0,0,0,0,0,0);
                a->hard_errors++;
                sysm = 16;
            }
            if (is_mrs) {
                out16(a, 0xF3EFu);
                out16(a, 0x8000u | (unsigned)((rd & 0xF) << 8) | (unsigned)sysm);
            } else {
                out16(a, 0xF380u | (unsigned)(rd & 0xF));
                out16(a, 0x8800u | (unsigned)sysm);
            }
        }
#endif
        else if (strieq_a(first, "NEG") || strieq_a(first, "RSB")) {
            int rd = parse_reg_a(&p); int rm;
            eat_comma_a(&p); rm = parse_reg_a(&p);
            out16(a, 0x4240 | ((rm & 7) << 3) | (rd & 7));
            skip_space_a(&p);
            if (*p == ',') { p++; skip_space_a(&p); if (*p == '#') parse_imm_a(&p); }
        }
        else if (strieq_a(first, "CMP")) {
            int rn = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (imm >= 0 && imm <= 255) {
                    /* T1 16-bit: CMP Rn, #imm8 */
                    out16(a, 0x2800 | ((rn & 7) << 8) | (imm & 0xFF));
                } else {
                    /* T2 32-bit: CMP.W Rn, #imm12 (ThumbExpandImm).
                     * hi: 0xF1B0 | i<<10 | Rn, lo: imm3<<12 | 0x0F00 | imm8.
                     * Was truncating the constant to its low 8 bits, which
                     * silently compared against the wrong value. */
                    unsigned i, imm3, imm8;
                    if (!thumb_imm12_a((unsigned)imm, &i, &imm3, &imm8)) {
                        {
                            char b1[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "CMP immediate %s cannot be encoded as a "
                                      "modified immediate", diag_num(b1, imm),
                                      0,0,0,0,0);
                        }
                        a->hard_errors++;
                        out16(a, 0xBF00);
                        out16(a, 0xBF00);
                    } else {
                        out16(a, 0xF1B0u | ((i & 1) << 10) | (unsigned)(rn & 0xF));
                        out16(a, ((imm3 & 7) << 12) | 0x0F00u | imm8);
                    }
                }
            } else {
                int rm = parse_reg_a(&p);
                out16(a, 0x4280 | ((rm & 7) << 3) | (rn & 7));
            }
        }
        else if (is_ldr_str_a(first)) {
            int is_load = 1;
            int kind = ldr_str_kind_a(first, &is_load);
            int rd = parse_reg_a(&p);
            ArmMemOp m;
            eat_comma_a(&p);
            if (*p == '=') {
                /* LDR rd, =label - emit MOVW+MOVT to load absolute address.
                 * .set symbols (e.g. __at variables) use their absolute address directly;
                 * code/data labels (e.g. .L_str_N) use g_flash_base + offset.
                 * g_flash_base is the first LR load address when --scatter is
                 * given, otherwise the default 0x08000000 (STM32F4 Flash). */
                char sym[64];
                int addr;
                int sn;
                p++;  /* skip '=' */
                sn = read_word_a(&p, sym, (int)sizeof(sym));
                (void)sn;
                addr = find_set_a(a, sym);
                if (addr >= 0) {
                    /* .set symbol - absolute address already (e.g. 0x40020800, 0x20000000) */
                } else {
                    int is_func = label_is_func_a(a, sym);
                    int label_addr = find_label_a(a, sym);
                    if (label_addr < 0) {
                        /* Not in this object: a cross-object address.  Baking 0
                         * here used to be a hard error, which made a single
                         * object that merely *names* another object's function
                         * impossible to assemble -- src/compiler.c does exactly
                         * that when it fills its backend vtable.  Record a
                         * relocation instead and let the linker write the real
                         * address; resolve_refs_a() still counts it, so a
                         * single-file .bin build keeps refusing (a baked 0 in a
                         * .bin is an image that jumps to the vector table). */
                        add_ref_a(a, a->pc, sym, is_func ? 11 : 10);
                        addr = (int)g_flash_base;   /* placeholder base */
                    } else {
                        /* code/data label - add Flash base address (scatter-aware),
                         * and set bit 0 for a function: a Thumb function pointer
                         * must carry it, or BLX branches into ARM state and the
                         * core faults on the first instruction. */
                        addr = (int)g_flash_base + label_addr;
                        if (is_func) addr |= 1;
                        /* --reloc-text: the number above assumes this object
                         * sits at g_flash_base.  In a multi-object link the
                         * linker places it somewhere else, and this is the only
                         * moment anybody knows which instruction holds the
                         * value -- flag it so the linker can add the
                         * difference.  Both instructions emitted below are
                         * always MOVW+MOVT (never the short MOV form), so the
                         * site is always patchable. */
                        if (g_config.enable_reloc_text)
                            add_ref_a(a, a->pc, ARM_RELOC_TEXT_NAME,
                                      ARM_RELOC_TEXT);
                    }
                }
                /* Emit MOVW rd, #low16 */
                {
                    unsigned imm = (unsigned)(addr & 0xFFFF);
                    unsigned ii = (imm >> 11) & 1;
                    unsigned imm4 = (imm >> 12) & 0xF;
                    unsigned imm3 = (imm >> 8) & 0x7;
                    unsigned imm8 = imm & 0xFF;
                    unsigned hi = 0xF240u | (ii << 10) | imm4;
                    unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                    out16(a, hi); out16(a, lo);
                }
                /* Emit MOVT rd, #high16 */
                {
                    unsigned imm = (unsigned)((addr >> 16) & 0xFFFF);
                    unsigned ii = (imm >> 11) & 1;
                    unsigned imm4 = (imm >> 12) & 0xF;
                    unsigned imm3 = (imm >> 8) & 0x7;
                    unsigned imm8 = imm & 0xFF;
                    unsigned hi = 0xF2C0u | (ii << 10) | imm4;
                    unsigned lo = (imm3 << 12) | ((rd & 0xF) << 8) | imm8;
                    out16(a, hi); out16(a, lo);
                }
                skip_to_eol_a(&p);
                if (*p == '\n') p++;
                continue;
            }
            parse_memop_a(a, &p, &m, 0);
            emit_ldr_str_a(a, kind, is_load, rd, &m);
        }
        else if (strieq_a(first, "BX")) {
            int rm = parse_reg_a(&p);
            out16(a, 0x4700 | ((rm & 0xF) << 3));
        }
        else if (strieq_a(first, "NOP")) {
            out16(a, 0xBF00);
        }
        else if (strieq_a(first, "IT") || strieq_a(first, "ITT") ||
                 strieq_a(first, "ITE") || strieq_a(first, "ITTT")) {
            /* IT block: encoding 0xBF{firstcond}{mask}.
             * firstcond: EQ=0,NE=1,CS/HS=2,CC/LO=3,MI=4,PL=5,VS=6,VC=7,
             *            HI=8,LS=9,GE=10,LT=11,GT=12,LE=13,AL=14
             *
             * mask[3:0] encodes both the IT block length (position of the
             * least-significant 1 bit) and per-instruction condition inversion:
             *   - For instruction i (1-indexed), if mask bit == firstcond[0],
             *     the instruction uses firstcond (Then); otherwise it uses
             *     NOT(firstcond) (Else).
             *
             * Correct masks (cb = firstcond[0]):
             *   IT   (1 instr):              1000 = 0x8
             *   ITT  (2 instrs, Then-Then):  cb 1 0 0  -> (cb<<3)|0x4
             *   ITE  (2 instrs, Then-Else): ~cb 1 0 0  -> ((~cb&1)<<3)|0x4
             *   ITTT (3 instrs, T-T-T):      cb cb 1 0 -> (cb<<3)|(cb<<2)|0x2
             *
             * BUGFIX #1: previously emitted 0xBF00 (NOP), making conditional
             *   MOVs unconditional (delay loops never exited).
             * BUGFIX #2: previously used fixed masks (ITT=0xC, ITE=0x4) which
             *   are only correct when firstcond[0]==1 (NE,LO,PL,VC,LS,LT,LE).
             *   For firstcond[0]==0 (EQ,HS,MI,VS,HI,GE,GT,AL) the conditions
             *   were silently inverted. E.g. ITE HS emitted 0xBF24 (acts as
             *   ITE LO HS) instead of 0xBF2C, flipping comparison results
             *   and making SysTick 500ms blink toggle at 1ms (invisible). */
            char cond[8];
            int cv = 14;  /* default AL */
            int cb;       /* firstcond[0] */
            int mask;
            skip_space_a(&p);
            read_word_a(&p, cond, sizeof(cond));
            if      (strieq_a(cond, "EQ")) cv = 0;
            else if (strieq_a(cond, "NE")) cv = 1;
            else if (strieq_a(cond, "HS") || strieq_a(cond, "CS")) cv = 2;
            else if (strieq_a(cond, "LO") || strieq_a(cond, "CC")) cv = 3;
            else if (strieq_a(cond, "MI")) cv = 4;
            else if (strieq_a(cond, "PL")) cv = 5;
            else if (strieq_a(cond, "VS")) cv = 6;
            else if (strieq_a(cond, "VC")) cv = 7;
            else if (strieq_a(cond, "HI")) cv = 8;
            else if (strieq_a(cond, "LS")) cv = 9;
            else if (strieq_a(cond, "GE")) cv = 10;
            else if (strieq_a(cond, "LT")) cv = 11;
            else if (strieq_a(cond, "GT")) cv = 12;
            else if (strieq_a(cond, "LE")) cv = 13;
            cb = cv & 1;
            if      (strieq_a(first, "IT"))   mask = 0x8;
            else if (strieq_a(first, "ITT"))  mask = (cb << 3) | 0x4;
            else if (strieq_a(first, "ITE"))  mask = ((cb ^ 1) << 3) | 0x4;
            else                              mask = (cb << 3) | (cb << 2) | 0x2; /* ITTT */
            out16(a, 0xBF00 | (cv << 4) | mask);
        }
        else if (strieq_a(first, "BLX")) {
            /* BLX <Rm>: 16-bit 0x4780 | Rm<<3 (Thumb-2 T1). The backend emits
             * this for an indirect call (obj->fnptr()), so without it every
             * function-pointer call became an "unknown mnemonic" and the whole
             * image failed to assemble. Only the register form exists here:
             * the label form is 32-bit and is never emitted by this backend,
             * so a non-register operand is reported rather than mistranslated. */
            int rm = parse_reg_a(&p);
            if (rm < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "BLX needs a register operand", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
            } else {
                out16(a, 0x4780 | ((rm & 0xF) << 3));
            }
        }
        else if (strieq_a(first, "BL")) {
            char name[64];
            read_label_name_a(&p, name, sizeof(name));
            {
                int addr = find_label_a(a, name);
                if (addr < 0) {
                    add_ref_a(a, a->pc, name, 1);
                    out16(a, 0xF000);
                    out16(a, 0xD000);
                } else {
                    emit_bl_a(a, addr);
                }
            }
        }
        else if (strieq_a(first, "B") || strieq_a(first, "BEQ") ||
                 strieq_a(first, "BNE") || strieq_a(first, "BLT") ||
                 strieq_a(first, "BGT") || strieq_a(first, "BLE") ||
                 strieq_a(first, "BGE") || strieq_a(first, "BCC") ||
                 strieq_a(first, "BCS") || strieq_a(first, "BHI") ||
                 strieq_a(first, "BLS") || strieq_a(first, "BLO") ||
                 strieq_a(first, "BHS")) {
            char name[64];
            int cond = -1;
            int is_uncond = (first[1] == 0);
            if (!is_uncond) {
                if (strieq_a(first, "BEQ")) cond = 0;
                else if (strieq_a(first, "BNE")) cond = 1;
                else if (strieq_a(first, "BCS")) cond = 2;
                else if (strieq_a(first, "BCC")) cond = 3;
                else if (strieq_a(first, "BGE")) cond = 10;
                else if (strieq_a(first, "BLT")) cond = 11;
                else if (strieq_a(first, "BGT")) cond = 12;
                else if (strieq_a(first, "BLE")) cond = 13;
                /* Unsigned comparisons: HI=8, LS=9, LO(CC)=3, HS(CS)=2.
                 * Verified: BHI = 0xD8xx, BLS = 0xD9xx, BLO = 0xD3xx,
                 * BHS = 0xD2xx in the 16-bit T1 form. */
                else if (strieq_a(first, "BHI")) cond = 8;
                else if (strieq_a(first, "BLS")) cond = 9;
                else if (strieq_a(first, "BLO")) cond = 3;
                else if (strieq_a(first, "BHS")) cond = 2;
            }
            read_label_name_a(&p, name, sizeof(name));
            {
                int addr = find_label_a(a, name);
                if (addr < 0) {
                    /* Forward reference: emit placeholder, resolve later.
                     * type=2: 32-bit B.W (T4, +-1MB), type=3: 32-bit B<cond>.W (T3, +-1MB).
                     * For type=3, cond is encoded in placeholder hi word
                     * (bits[9:6]) so resolve_refs_a can recover it. */
                    if (is_uncond) {
                        /* 32-bit B.W (T4) forward ref: type=2, 4-byte placeholder.
                         * resolve_refs_a will call emit_b_w_a to fill in the encoding. */
                        add_ref_a(a, a->pc, name, 2);
                        out16(a, 0xF000);
                        out16(a, 0x9000);
                    } else {
                        /* 32-bit B<cond>.W (T3) forward ref: type=3, 4-byte placeholder.
                         * cond encoded in hi word bits[9:6] for resolve_refs_a. */
                        add_ref_a(a, a->pc, name, 3);
                        out16(a, 0xF000u | (unsigned)(cond << 6));
                        out16(a, 0x8000);
                    }
                } else {
                    /* Backward reference: target known, emit directly */
                    if (is_uncond) {
                        /* 32-bit B.W (T4): +-1MB range, no truncation */
                        emit_b_w_a(a, addr);
                    } else {
                        /* 32-bit B<cond>.W (T3): +-1MB range, no truncation */
                        emit_bcond_w_a(a, cond, addr);
                    }
                }
            }
        }
        else if (strieq_a(first, "CPSID") || strieq_a(first, "CPSIE")) {
            /* CPS T1 16-bit: bit4=change(1=ID,0=IE), bit1=affect_i, bit0=affect_f
             *   CPSID i = 0xB672, CPSIE i = 0xB662, CPSID f = 0xB671, CPSIE f = 0xB661
             * Note: base 0xB660 already has bit5=1; change is bit4 (0x10), NOT bit5.
             * Previously NOP'd, so Error_Handler's CPSID i failed to mask
             * interrupts and HAL_Init's CPSIE i failed to enable them. */
            int is_id = strieq_a(first, "CPSID");
            int affect_i = 0, affect_f = 0;
            skip_space_a(&p);
            while (*p && *p != '\n' && *p != ';') {
                if (*p == 'i' || *p == 'I') affect_i = 1;
                else if (*p == 'f' || *p == 'F') affect_f = 1;
                p++;
            }
            out16(a, 0xB660 | (is_id ? 0x10 : 0) | (affect_i ? 0x2 : 0) | (affect_f ? 0x1 : 0));
        }
        else if (strieq_a(first, "DMB") || strieq_a(first, "DSB") || strieq_a(first, "ISB")) {
            /* T1 32-bit barriers: 0xF3BF 0x8F4x/5x/6x, option=0xF for SY.
             *   DMB SY = F3BF 8F5F, DSB SY = F3BF 8F4F, ISB SY = F3BF 8F6F.
             * Pass1 op_size_a returns 4 for these. Previously NOP'd (2 bytes),
             * causing pass1/pass2 size mismatch and silent instruction drops. */
            unsigned lo_base;
            if      (strieq_a(first, "DMB")) lo_base = 0x8F50;
            else if (strieq_a(first, "DSB")) lo_base = 0x8F40;
            else                             lo_base = 0x8F60;  /* ISB */
            out16(a, 0xF3BF);
            out16(a, lo_base | 0xF);
        }
        else if (strieq_a(first, "SEV") || strieq_a(first, "WFE") || strieq_a(first, "WFI")) {
            /* 16-bit hint instructions (no operands).
             *   SEV = 0xBF40, WFE = 0xBF20, WFI = 0xBF30.
             * Used by HAL_PWR_EnterSLEEPMode/STOPMode/STANDBYMode. */
            if      (strieq_a(first, "SEV")) out16(a, 0xBF40);
            else if (strieq_a(first, "WFE")) out16(a, 0xBF20);
            else                             out16(a, 0xBF30);  /* WFI */
        }
        else if (strieq_a(first, "REV") || strieq_a(first, "REV16") || strieq_a(first, "REVSH")) {
            /* T1 16-bit: 1011 1010 op Rm Rd, where op=00(REV),01(REV16),11(REVSH).
             *   REV=0xBA00, REV16=0xBA40, REVSH=0xBAC0. */
            int rd = parse_reg_a(&p);
            int rm;
            unsigned base;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if      (strieq_a(first, "REV"))   base = 0xBA00;
            else if (strieq_a(first, "REV16")) base = 0xBA40;
            else                               base = 0xBAC0;  /* REVSH */
            out16(a, base | ((rm & 7) << 3) | (rd & 7));
        }
        else if (strieq_a(first, "RRX")) {
            /* 32-bit: MOV Rd, Rm, RRX = 0xEA4F (Rd<<12) 0x30 Rm.
             * Pass1 op_size_a returns 4. */
            int rd = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            out16(a, 0xEA4F);
            out16(a, (unsigned)((rd & 0xF) << 12) | 0x30 | (rm & 0xF));
        }
        /* ===== #1 FIX: VFP floating-point instruction encoders =====
         * Previously ALL VFP instructions fell through to NOP (0xBF00),
         * making every float operation a no-op. Now each is properly
         * encoded as a 32-bit Thumb-2 VFP instruction.
         *
         * VFP register encoding (S<n>, n=0..31 ; D<n>, n=0..15):
         *   the 4-bit register field holds n[4:1] for single precision and
         *   the whole n for double precision; the neighbouring "half" bit
         *   holds n[0] for single precision and is 0 for double.
         *     Vd = field at bits15:12, half at bit22
         *     Vn = field at bits19:16, half at bit7
         *     Vm = field at bits3:0,   half at bit5
         *   The split is done by ark_vfp_nib()/ark_vfp_half(); the bit
         *   positions above are the ones those helpers feed into.
         * sz bit (bit8): 0=F32 single, 1=F64 double. */
        else if (strieq_a(first, "VMOV")) {
            /* VMOV Sn, Rt (core -> VFP) or VMOV Rt, Sn (VFP -> core) */
            skip_space_a(&p);
            if (*p == 's' || *p == 'S') {
                /* VMOV Sn, Rt — core register to VFP single register */
                int sn = parse_vfp_sreg_a(&p);
                int rt;
                unsigned vn, n_bit, hi, lo;
                if (sn < 0) sn = 0;
                eat_comma_a(&p);
                rt = parse_reg_a(&p);
                vn = ark_vfp_nib(sn, 0);
                n_bit = ark_vfp_half(sn, 0);
                hi = 0xEE00u | vn;
                lo = 0x0A10u | (unsigned)((rt & 0xF) << 12) | (n_bit << 7);
                out16(a, hi);
                out16(a, lo);
            } else {
                /* VMOV Rt, Sn — VFP single register to core register */
                int rt = parse_reg_a(&p);
                int sn;
                unsigned vn, n_bit, hi, lo;
                eat_comma_a(&p);
                sn = parse_vfp_sreg_a(&p);
                if (sn < 0) sn = 0;
                vn = ark_vfp_nib(sn, 0);
                n_bit = ark_vfp_half(sn, 0);
                hi = 0xEE10u | vn;   /* bit20=1 means VFP->core */
                lo = 0x0A10u | (unsigned)((rt & 0xF) << 12) | (n_bit << 7);
                out16(a, hi);
                out16(a, lo);
            }
        }
        else if (strieq_a(first, "VADD") || strieq_a(first, "VSUB") ||
                 strieq_a(first, "VMUL") || strieq_a(first, "VDIV")) {
            /* VOP.F32 Sd,Sn,Sm  or  VOP.F64 Dd,Dn,Dm */
            char suf[16];
            int is_f64;
            unsigned base;
            int sd, sn, sm;
            unsigned d, vd, n_bit, vn, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            is_f64 = strieq_a(suf, "F64");
            if      (strieq_a(first, "VADD")) base = is_f64 ? 0xEE300B00u : 0xEE300A00u;
            else if (strieq_a(first, "VSUB")) base = is_f64 ? 0xEE300B40u : 0xEE300A40u;
            else if (strieq_a(first, "VMUL")) base = is_f64 ? 0xEE200B00u : 0xEE200A00u;
            /* FIX(vfp-encode): VDIV was 0xEE000A40/0xEE000B40 = VMLS, so every float division lost its division. */
            else                              base = is_f64 ? 0xEE800B00u : 0xEE800A00u;
            if (is_f64) {
                sd = parse_vfp_dreg_a(&p); eat_comma_a(&p);
                sn = parse_vfp_dreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_dreg_a(&p);
            } else {
                sd = parse_vfp_sreg_a(&p); eat_comma_a(&p);
                sn = parse_vfp_sreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_sreg_a(&p);
            }
            if (sd < 0) sd = 0; if (sn < 0) sn = 0; if (sm < 0) sm = 0;
            d     = ark_vfp_half(sd, is_f64);   /* bit22   = Sd[0] */
            vd    = ark_vfp_nib (sd, is_f64);   /* bits15:12 = Sd[4:1] */
            n_bit = ark_vfp_half(sn, is_f64);   /* bit7    = Sn[0] */
            vn    = ark_vfp_nib (sn, is_f64);   /* bits19:16 = Sn[4:1] */
            m     = ark_vfp_half(sm, is_f64);   /* bit5    = Sm[0] */
            vm    = ark_vfp_nib (sm, is_f64);   /* bits3:0 = Sm[4:1] */
            hi = (base >> 16) | (d << 6) | vn;
            lo = (base & 0xFFFF) | (vd << 12) | (n_bit << 7) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VCVT")) {
            /* VCVT register conversions.  Every base below is the encoding for
             * the S0/D0 form, measured with armasm (h7test/v3.lst, v4.lst):
             *   VCVT.S32.F32 = 0xEEBD0AC0   VCVT.U32.F32 = 0xEEBC0AC0
             *   VCVT.F32.S32 = 0xEEB80AC0   VCVT.F32.U32 = 0xEEB80A40
             *   VCVT.S32.F64 = 0xEEBD0BC0   VCVT.U32.F64 = 0xEEBC0BC0
             *   VCVT.F64.S32 = 0xEEB80BC0   VCVT.F64.U32 = 0xEEB80B40
             *   VCVT.F32.F64 = 0xEEB70BC0   VCVT.F64.F32 = 0xEEB70AC0
             * FIX(vfp-encode): the U32 variants and the .F32.F64/.F64.F32 pairs
             * used to fall through to the S32.F32 base, so unsigned
             * conversions and float<->double casts silently became S32.F32. */
            char suf[32];
            unsigned base;
            int sd = 0, sm = 0;
            int dst_dbl, src_dbl;
            unsigned d, vd, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            if      (strieq_a(suf, "S32F32")) base = 0xEEBD0AC0u;
            else if (strieq_a(suf, "U32F32")) base = 0xEEBC0AC0u;
            else if (strieq_a(suf, "F32S32")) base = 0xEEB80AC0u;
            else if (strieq_a(suf, "F32U32")) base = 0xEEB80A40u;
            else if (strieq_a(suf, "S32F64")) base = 0xEEBD0BC0u;
            else if (strieq_a(suf, "U32F64")) base = 0xEEBC0BC0u;
            else if (strieq_a(suf, "F64S32")) base = 0xEEB80BC0u;
            else if (strieq_a(suf, "F64U32")) base = 0xEEB80B40u;
            else if (strieq_a(suf, "F32F64")) base = 0xEEB70BC0u;
            else if (strieq_a(suf, "F64F32")) base = 0xEEB70AC0u;
            else {
                /* Do not guess: an unknown suffix used to be encoded as
                 * VCVT.S32.F32, i.e. a silently wrong operation. */
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "unsupported VCVT suffix", 0,0,0,0,0,0);
                a->hard_errors++;
                base = 0xEEBD0AC0u;
            }
            dst_dbl = strieq_a(suf, "F64S32") || strieq_a(suf, "F64U32") ||
                      strieq_a(suf, "F64F32");
            src_dbl = strieq_a(suf, "S32F64") || strieq_a(suf, "U32F64") ||
                      strieq_a(suf, "F32F64");
            if (dst_dbl) { sd = parse_vfp_dreg_a(&p); eat_comma_a(&p); }
            else         { sd = parse_vfp_sreg_a(&p); eat_comma_a(&p); }
            if (src_dbl) sm = parse_vfp_dreg_a(&p);
            else         sm = parse_vfp_sreg_a(&p);
            if (sd < 0) sd = 0; if (sm < 0) sm = 0;
            d  = ark_vfp_half(sd, dst_dbl);
            vd = ark_vfp_nib (sd, dst_dbl);
            m  = ark_vfp_half(sm, src_dbl);
            vm = ark_vfp_nib (sm, src_dbl);
            hi = (base >> 16) | (d << 6);
            lo = (base & 0xFFFF) | (vd << 12) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VCMP")) {
            /* VCMP.F32 Sd,Sm / VCMP.F64 Dd,Dm / VCMP.F32|F64 Sd,#0.0
             * Bases measured with armasm (h7test/v3.lst, v5.lst):
             *   VCMP.F32 Sd,Sm    = 0xEEB40A40 + regs
             *   VCMP.F64 Dd,Dm    = 0xEEB40B40 + regs
             *   VCMP.F32 Sd,#0.0  = 0xEEB50A40 + Sd
             *   VCMP.F64 Dd,#0.0  = 0xEEB50B40 + Dd
             * FIX(vfp-encode): "#0.0" used to make parse_vfp_sreg_a() return
             * -1, which was clamped to 0, i.e. the compare silently became
             * "against S0/D0".  It is now encoded properly, and any other
             * immediate is a hard error instead of a wrong answer. */
            char suf[16];
            int is_f64, cmp_zero = 0;
            unsigned base;
            int sd, sm = 0;
            unsigned d, vd, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            is_f64 = strieq_a(suf, "F64");
            base = is_f64 ? 0xEEB40B40u : 0xEEB40A40u;
            if (is_f64) sd = parse_vfp_dreg_a(&p);
            else        sd = parse_vfp_sreg_a(&p);
            if (sd < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad destination register for VCMP", 0,0,0,0,0,0);
                a->hard_errors++;
                sd = 0;
            }
            eat_comma_a(&p);
            skip_space_a(&p);
            if (*p == '#') {
                int nonzero = 0, ndig = 0;
                p++;
                skip_space_a(&p);
                if (*p == '-') p++;
                while ((*p >= '0' && *p <= '9') || *p == '.') {
                    if (*p >= '1' && *p <= '9') nonzero = 1;
                    if (*p != '.') ndig++;
                    p++;
                }
                if (nonzero || ndig == 0) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "VCMP supports only #0.0 as an immediate operand",
                              0,0,0,0,0,0);
                    a->hard_errors++;
                }
                cmp_zero = 1;
                base = is_f64 ? 0xEEB50B40u : 0xEEB50A40u;
            } else if (is_f64) {
                sm = parse_vfp_dreg_a(&p);
            } else {
                sm = parse_vfp_sreg_a(&p);
            }
            if (!cmp_zero && sm < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad operand register for VCMP", 0,0,0,0,0,0);
                a->hard_errors++;
                sm = 0;
            }
            d  = ark_vfp_half(sd, is_f64);
            vd = ark_vfp_nib (sd, is_f64);
            m  = cmp_zero ? 0u : ark_vfp_half(sm, is_f64);
            vm = cmp_zero ? 0u : ark_vfp_nib (sm, is_f64);
            hi = (base >> 16) | (d << 6);
            lo = (base & 0xFFFF) | (vd << 12) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VMRS")) {
            /* VMRS APSR_nzcv, FPSCR — fixed encoding 0xFEF1FA10 */
            out16(a, 0xFEF1);
            out16(a, 0xFA10);
        }
        else if (strieq_a(first, "VNEG") || strieq_a(first, "VABS") ||
                 strieq_a(first, "VSQRT")) {
            /* VNEG/VABS/VSQRT .F32 Sd,Sm  or  .F64 Dd,Dm */
            char suf[16];
            int is_f64;
            unsigned base;
            int sd, sm;
            unsigned d, vd, m, vm;
            unsigned hi, lo;
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            is_f64 = strieq_a(suf, "F64");
            /* FIX(vfp-encode): VNEG is 0xEEB10A40/0xEEB10B40 (0xEEB10AC0 is VSQRT). */
            if      (strieq_a(first, "VNEG"))  base = is_f64 ? 0xEEB10B40u : 0xEEB10A40u;
            else if (strieq_a(first, "VABS"))  base = is_f64 ? 0xEEB00BC0u : 0xEEB00AC0u;
            /* FIX(vfp-encode): VSQRT is 0xEEB10AC0/0xEEB10BC0 (0xEEB11AC0 does not exist). */
            else                               base = is_f64 ? 0xEEB10BC0u : 0xEEB10AC0u;
            if (is_f64) {
                sd = parse_vfp_dreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_dreg_a(&p);
            } else {
                sd = parse_vfp_sreg_a(&p); eat_comma_a(&p);
                sm = parse_vfp_sreg_a(&p);
            }
            if (sd < 0) sd = 0; if (sm < 0) sm = 0;
            d  = ark_vfp_half(sd, is_f64);
            vd = ark_vfp_nib (sd, is_f64);
            m  = ark_vfp_half(sm, is_f64);
            vm = ark_vfp_nib (sm, is_f64);
            hi = (base >> 16) | (d << 6);
            lo = (base & 0xFFFF) | (vd << 12) | (m << 5) | vm;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "VLDR") || strieq_a(first, "VSTR")) {
            /* VLDR/VSTR Sd, [Rn, #imm]  — 32-bit T1 encoding.
             * Base: VLDR.F32 = 0xED900A00, VSTR.F32 = 0xED800A00.
             * imm8 = offset>>2 (word-aligned). F64 sets bit 8 (sz). */
            /* The register name, not the optional suffix, says whether this is
             * a single or a double transfer: reading only the suffix turned
             * every `vldr d0, ...` into a transfer of S0.  A suffix that
             * contradicts the register is a typo and is refused. */
            char suf[16];
            char sym[64];
            int is_f64;
            unsigned base;
            int sd, rn, imm;
            unsigned d, vd;
            unsigned hi, lo;
            unsigned imm8;
            int is_load = strieq_a(first, "VLDR");
            read_vfp_suffix_a(&p, suf, (int)sizeof(suf));
            /* armasm also accepts the ".32"/".64" spelling of the same suffix. */
            skip_space_a(&p);
            is_f64 = (p[0] == 'd' || p[0] == 'D');
            if ((strieq_a(suf, "F64") || strieq_a(suf, "64")) && !is_f64) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "VLDR/VSTR.F64 needs a D register", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if ((strieq_a(suf, "F32") || strieq_a(suf, "32")) && is_f64) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "VLDR/VSTR.F32 needs an S register", 0,0,0,0,0,0);
                a->hard_errors++;
            }
            if (is_f64) sd = parse_vfp_dreg_a(&p);
            else        sd = parse_vfp_sreg_a(&p);
            if (sd < 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "VLDR/VSTR needs an S or D register", 0,0,0,0,0,0);
                a->hard_errors++;
                sd = 0;
            }
            if (is_f64) base = is_load ? 0xED900B00u : 0xED800B00u;
            else        base = is_load ? 0xED900A00u : 0xED800A00u;
            eat_comma_a(&p);
            skip_space_a(&p);
            imm = 0;
            if (*p == '[') {
                p++;
                skip_space_a(&p);
                rn = parse_reg_a(&p);
                if (rn < 0) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "VLDR/VSTR needs a base register inside []",
                              0,0,0,0,0,0);
                    a->hard_errors++;
                    rn = 15;
                }
                skip_space_a(&p);
                if (*p == ',') {
                    p++;
                    skip_space_a(&p);
                    if (*p == '#') imm = parse_imm_a(&p);
                    else {
                        /* Register-offset addressing ([Rn, Rm]) has no encoder
                         * here.  Ignoring Rm would silently address the base
                         * register instead, so refuse and let the author write
                         * the address arithmetic out. */
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "register-offset addressing ([Rn, Rm]) does "
                                  "not exist for this instruction in Thumb; compute "
                                  "the address with ADD first", 0,0,0,0,0,0);
                        a->hard_errors++;
                    }
                }
                skip_space_a(&p);
                if (*p == ']') p++;
                check_no_index_writeback_a(a, &p);
            } else {
                /* Literal pool: `vldr s0, =label` / `vldr s0, label`.  Both
                 * passes already know every label in the file, so this is
                 * resolved here rather than left to a relocation.  A target
                 * that is not in this file, or one too far away, is an error:
                 * encoding offset 0 instead would read the wrong address. */
                int addr;
                if (*p == '=') {
                    /* `=label` asks for the ADDRESS of the label, which is
                     * what LDR does; VLDR reads the value stored there, so a
                     * bare label is the right spelling and this one is a
                     * misunderstanding worth reporting. */
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "VLDR takes a bare label, not '=label': it loads "
                              "from that address", 0,0,0,0,0,0);
                    a->hard_errors++;
                    p++;
                }
                if (!is_load) {
                    /* VSTR <Dd>, <label> selects the pc-relative form, which the
                     * ARM ARM makes UNPREDICTABLE for stores. */
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "VSTR needs a base register ([Rn, #imm]); the "
                              "pc-relative store form is UNPREDICTABLE",
                              0,0,0,0,0,0);
                    a->hard_errors++;
                }
                skip_space_a(&p);
                read_word_a(&p, sym, (int)sizeof(sym));
                addr = find_label_a(a, sym);
                if (addr < 0) {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "VLDR/VSTR literal-pool target %sq must be a label in "
                              "this file", sym, 0,0,0,0,0);
                    a->hard_errors++;
                } else {
                    int pbase = (a->pc + 4) & ~3;
                    imm = addr - pbase;
                    if (imm < 0 || imm > 1020 || (imm & 3) != 0) {
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "VLDR/VSTR target %sq is %s bytes from the aligned "
                                  "PC; the encoding covers 0..1020 in steps of 4",
                                  sym, diag_num(b1, imm), 0,0,0,0);
                        a->hard_errors++;
                        imm = 0;
                    }
                }
                rn = 15;
            }
            if (imm < 0 || imm > 1020 || (imm & 3) != 0) {
                char b1[12];
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "VLDR/VSTR offset %s is not encodable (0..1020, step 4)",
                          diag_num(b1, imm), 0,0,0,0,0);
                a->hard_errors++;
                imm = 0;
            }
            d  = ark_vfp_half(sd, is_f64);
            vd = ark_vfp_nib (sd, is_f64);
            imm8 = (unsigned)((imm >> 2) & 0xFF);
            hi = (base >> 16) | (d << 6) | (unsigned)(rn & 0xF);
            lo = (base & 0xFFFF) | (vd << 12) | imm8;
            out16(a, hi);
            out16(a, lo);
        }
        else if (strieq_a(first, "TST")) {
            /* TST (register): 16-bit T1 0100000000 Rm Rn for r0-r7,
             * 32-bit T2 0xEA1n_0F0m for high registers.
             * Encodings verified against armasm: TST r4,r1 = 0x420C,
             * TST r8,r9 = 0xEA180F09, TST r0,r12 = 0xEA100F0C. */
            int rn = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rn >= 0 && rn <= 7 && rm >= 0 && rm <= 7) {
                out16(a, 0x4200 | ((rm & 7) << 3) | (rn & 7));
            } else if (rn >= 0 && rm >= 0) {
                out16(a, 0xEA10 | (rn & 0xF));
                out16(a, 0x0F00 | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for TST", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "CLZ")) {
            /* CLZ Rd, Rm: 32-bit T1 111110101011 Rm 1111 1000 Rd 0000.
             * Verified: CLZ r2,r0 = 0xFAB0F280, CLZ r12,r11 = 0xFABBFC8B. */
            int rd = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rm >= 0) {
                out16(a, 0xFAB0 | (rm & 0xF));
                out16(a, 0xF080 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for CLZ", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "RBIT")) {
            /* RBIT Rd, Rm: 32-bit T1 111110101001 Rm 1111 Rd 1010 Rm.
             * Verified with armasm 5.06u7 (Cortex-M7):
             *   RBIT r0,r0  = 0xFA90F0A0   RBIT r3,r1   = 0xFA91F3A1
             *   RBIT r9,r12 = 0xFA9CF9AC */
            int rd = parse_reg_a(&p);
            int rm;
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rm >= 0) {
                out16(a, 0xFA90 | (rm & 0xF));
                out16(a, 0xF0A0 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for RBIT", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "SXTB") || strieq_a(first, "SXTH") ||
                 strieq_a(first, "UXTB") || strieq_a(first, "UXTH")) {
            /* Sign/zero extend: 16-bit T1 (0xB240/0xB200/0xB2C0/0xB2B0)
             * for low registers, 32-bit T2 (0xFA4F/0xFA0F/0xFA5F/0xFA1F
             * F080 | Rd<<8 | Rm) otherwise.
             * Verified: SXTB r7,r5 = 0xB26F, SXTH r6,r3 = 0xB21E,
             *           UXTB r1,r2 = 0xB2D1, UXTH r0,r7 = 0xB2B8,
             *           SXTB r0,r8 = 0xFA4FF088, UXTH r9,r3 = 0xFA1FF983. */
            int rd = parse_reg_a(&p);
            int rm;
            unsigned lo_base = 0xB200u;
            unsigned hi_base = 0xFA0Fu;
            if (strieq_a(first, "SXTB")) { lo_base = 0xB240u; hi_base = 0xFA4Fu; }
            else if (strieq_a(first, "UXTB")) { lo_base = 0xB2C0u; hi_base = 0xFA5Fu; }
            else if (strieq_a(first, "UXTH")) { lo_base = 0xB2B0u; hi_base = 0xFA1Fu; }
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) {
                out16(a, lo_base | ((rm & 7) << 3) | (rd & 7));
            } else if (rd >= 0 && rm >= 0) {
                out16(a, hi_base | (rm & 0xF));
                out16(a, 0xF080 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                {
                    char mn[9];
                    int mi;
                    for (mi = 0; mi < 8 && first[mi]; mi++) mn[mi] = first[mi];
                    mn[mi] = 0;
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "bad register operand for %s", mn, 0,0,0,0,0);
                }
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else if (strieq_a(first, "BKPT")) {
            /* BKPT #imm8: 0xBE00 | imm8. On Cortex-M this - not SVC 0xAB -
             * is the semihosting trap: QEMU only intercepts SVC 0xAB for
             * A/R profile cores and sends BKPT 0xAB to the semihosting
             * monitor on M-profile. Verified: BKPT #0xab = 0xBEAB (armasm
             * 5.06 golden bytes). */
            int imm;
            int ok = 0;
            skip_space_a(&p);
            imm = parse_imm_opt_hash_a(&p, &ok);
            if (!ok || imm < 0 || imm > 255) {
                if (!ok)
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "BKPT needs a numeric immediate", 0,0,0,0,0,0);
                else
                    {
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "BKPT immediate %s out of range 0..255",
                                  diag_num(b1, imm), 0,0,0,0,0);
                    }
                a->hard_errors++;
                imm = 0;
            }
            out16(a, 0xBE00 | (imm & 0xFF));
        }
        else if (strieq_a(first, "SVC")) {
            /* SVC #imm8: 0xDF00 | imm8. Verified: SVC #18 = 0xDF12.
             * The '#' is optional (armasm accepts "SVC 0xAB"), but the operand
             * must be a number: a missing operand used to encode SVC #0, which
             * silently breaks semihosting calls at run time. */
            int imm;
            int ok = 0;
            skip_space_a(&p);
            imm = parse_imm_opt_hash_a(&p, &ok);
            if (!ok || imm < 0 || imm > 255) {
                if (!ok)
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "SVC needs a numeric immediate", 0,0,0,0,0,0);
                else
                    {
                        char b1[12];
                        diag_tool(DIAG_SEV_ERROR, "arkcc",
                                  "SVC immediate %s out of range 0..255",
                                  diag_num(b1, imm), 0,0,0,0,0);
                    }
                a->hard_errors++;
                imm = 0;
            }
            out16(a, 0xDF00 | (imm & 0xFF));
        }
        else if (strieq_a(first, "MOVS")) {
            /* MOVS Rd,#imm: 16-bit 0x2000|Rd<<8|imm8, 32-bit MOVS.W
             * (0xF05F base, S=1) otherwise.
             * MOVS Rd,Rm: 16-bit 0x0000|Rm<<3|Rd (LSLS Rd,Rm,#0 alias),
             * 32-bit 0xEA5F_0n0m for high registers.
             * Verified: MOVS r0,#42 = 0x202A, MOVS r5,#7 = 0x2507,
             *           MOVS r0,r1 = 0x0008, MOVS r12,r3 = 0xEA5F0C03. */
            int rd = parse_reg_a(&p);
            eat_comma_a(&p);
            if (*p == '#') {
                int imm = parse_imm_a(&p);
                if (rd >= 0 && rd <= 7 && imm >= 0 && imm <= 255) {
                    out16(a, 0x2000 | ((rd & 7) << 8) | (imm & 0xFF));
                } else {
                    unsigned i, imm3, imm8;
                    if (rd < 0 || !thumb_imm12_a((unsigned)imm, &i, &imm3, &imm8)) {
                        {
                            char b1[12], b2[12];
                            diag_tool(DIAG_SEV_ERROR, "arkcc",
                                      "MOVS immediate %s (Rd=r%s) cannot be encoded",
                                      diag_num(b1, imm), diag_num(b2, rd),
                                      0,0,0,0);
                        }
                        a->hard_errors++;
                        out16(a, 0xBF00);
                        out16(a, 0xBF00);
                    } else {
                        out16(a, 0xF05F | ((i & 1) << 10));
                        out16(a, ((imm3 & 7) << 12) | ((rd & 0xF) << 8) | imm8);
                    }
                }
            } else {
                int rm = parse_reg_a(&p);
                if (rd >= 0 && rd <= 7 && rm >= 0 && rm <= 7) {
                    out16(a, 0x0000 | ((rm & 7) << 3) | (rd & 7));
                } else if (rd >= 0 && rm >= 0) {
                    out16(a, 0xEA5F);
                    out16(a, ((rd & 0xF) << 8) | (rm & 0xF));
                } else {
                    diag_tool(DIAG_SEV_ERROR, "arkcc",
                              "bad register operand for MOVS", 0,0,0,0,0,0);
                    a->hard_errors++;
                    out16(a, 0xBF00);
                }
            }
        }
        else if (strieq_a(first, "SADD16")) {
            /* SADD16 Rd, Rn, Rm: 0xFA90_F000|Rn<<16|Rd<<8|Rm.
             * Verified: SADD16 r0,r0,r1 = 0xFA90F001,
             *           SADD16 r3,r7,r5 = 0xFA97F305. */
            int rd = parse_reg_a(&p);
            int rn, rm;
            eat_comma_a(&p);
            rn = parse_reg_a(&p);
            eat_comma_a(&p);
            rm = parse_reg_a(&p);
            if (rd >= 0 && rn >= 0 && rm >= 0) {
                out16(a, 0xFA90 | (rn & 0xF));
                out16(a, 0xF000 | ((rd & 0xF) << 8) | (rm & 0xF));
            } else {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "bad register operand for SADD16", 0,0,0,0,0,0);
                a->hard_errors++;
                out16(a, 0xBF00);
                out16(a, 0xBF00);
            }
        }
        else {
            /* Unknown mnemonic. This used to silently emit a 16-bit NOP, which
             * turned a typo or an unimplemented instruction into a program that
             * still compiled and linked but did something else. Report it and
             * keep failing at the end of assembly. A NOP is still emitted so
             * that label addresses stay close to the intent and later code does
             * not cascade with bogus addresses. */
            {
                char mn[33];
                char b1[12];
                int mi;
                for (mi = 0; mi < 32 && first[mi]; mi++) mn[mi] = first[mi];
                mn[mi] = 0;
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "unknown mnemonic %sq at code offset %s "
                          "(no instruction emitted)", mn, diag_num(b1, a->pc),
                          0,0,0,0);
            }
            a->hard_errors++;
            out16(a, 0xBF00);
        }
        skip_to_eol_a(&p);
        if (*p == '\n') p++;
    }
}

/* Number of references this object could not resolve to a label.  In -c mode
 * they become linker relocations; in a single-file .bin build nobody will ever
 * resolve them, and the placeholder BL (F000 D000) decodes as a branch to
 * +0x0C000000 -- an image that faults far outside itself.  compiler.c therefore
 * refuses to write a .bin while this is non-zero. */
static int          g_arm_unresolved = 0;
static const char  *g_arm_unresolved_first = 0;

/* Defined below (near arm_patch_movw_movt); resolve_refs_a needs it to
 * rewrite a MOVW/MOVT pair once a cross-object symbol becomes known. */
static void mov_imm_encode_a(int is_movt, int rd, unsigned imm,
                            unsigned *hi, unsigned *lo);

static void resolve_refs_a(ArmAsm *a) {
    int i;
    g_arm_unresolved = 0;
    g_arm_unresolved_first = 0;
    for (i = 0; i < a->ref_count; i++) {
        AsmRef *r = &a->refs[i];
        int addr;
        if (r->type == 6 || r->type == ARM_RELOC_TEXT)
            continue;   /* .arkdata / .arktext: patch sites, not labels */
        addr = find_label_a(a, r->name);
        if (addr < 0) {
            /* Not a local label: this is a cross-object reference and has
             * already been kept as a relocation by arm_save_relocs(), so the
             * linker owns it from here. Reporting it as an error here would
             * make every -c compile of a multi-file program exit non-zero on
             * a valid object (see ARK_DIAG_UNRESOLVED_LOCAL). */
#if ARK_DIAG_UNRESOLVED_LOCAL
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "unresolved label %sq", r->name, 0,0,0,0,0);
#endif
            a->error_count++;
            g_arm_unresolved++;
            if (!g_arm_unresolved_first && r->name[0] != '.')
                g_arm_unresolved_first = r->name;
            continue;
        }
        if (r->type == 7) {
            /* --asm-extra ADR T1: imm8*4 is relative to Align(pc+4, 4). */
            int base = (r->pc + 4) & ~3;
            int off = addr - base;
            unsigned hi = (unsigned)a->out[r->pc] | ((unsigned)a->out[r->pc + 1] << 8);
            if (off < 0 || off > 1020 || (off & 3) != 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "ADR target %sq is out of the 16-bit range (0..1020, "
                          "step 4)", r->name, 0,0,0,0,0);
                a->hard_errors++;
            } else {
                hi = (hi & 0xFF00u) | (unsigned)((off >> 2) & 0xFF);
                a->out[r->pc]     = (unsigned char)(hi & 0xFF);
                a->out[r->pc + 1] = (unsigned char)((hi >> 8) & 0xFF);
            }
        } else if (r->type == 10 || r->type == 11) {
            /* LDR rd, =sym for a symbol defined in another object: the
             * placeholder was MOVW/MOVT of g_flash_base, so rewrite the pair
             * with the resolved address (plus the Thumb bit for a function). */
            unsigned imm = (unsigned)g_flash_base + (unsigned)addr;
            unsigned hi, lo, hi2, lo2;
            int rd = (int)((a->out[r->pc + 3] >> 0) & 0x0F);
            if (r->type == 11) imm |= 1u;
            mov_imm_encode_a(0, rd, imm & 0xFFFFu, &hi, &lo);
            mov_imm_encode_a(1, rd, (imm >> 16) & 0xFFFFu, &hi2, &lo2);
            a->out[r->pc]     = (unsigned char)(hi & 0xFF);
            a->out[r->pc + 1] = (unsigned char)((hi >> 8) & 0xFF);
            a->out[r->pc + 2] = (unsigned char)(lo & 0xFF);
            a->out[r->pc + 3] = (unsigned char)((lo >> 8) & 0xFF);
            a->out[r->pc + 4] = (unsigned char)(hi2 & 0xFF);
            a->out[r->pc + 5] = (unsigned char)((hi2 >> 8) & 0xFF);
            a->out[r->pc + 6] = (unsigned char)(lo2 & 0xFF);
            a->out[r->pc + 7] = (unsigned char)((lo2 >> 8) & 0xFF);
        } else if (r->type == 8 || r->type == 9) {
            /* --asm-extra CBZ (8) / CBNZ (9): forward only, 0..62, even. */
            int off = addr - (r->pc + 4);
            unsigned hi = (unsigned)a->out[r->pc] | ((unsigned)a->out[r->pc + 1] << 8);
            if (off < 0 || off > 62 || (off & 1) != 0) {
                diag_tool(DIAG_SEV_ERROR, "arkcc",
                          "CBZ/CBNZ target %sq is out of the 16-bit forward range",
                          r->name, 0,0,0,0,0);
                a->hard_errors++;
            } else {
                unsigned imm5 = (unsigned)(off >> 1);
                hi = (hi & 0xF8C0u) | ((imm5 & 0x20u) << 4) | ((imm5 & 0x1Fu) << 3);
                a->out[r->pc]     = (unsigned char)(hi & 0xFF);
                a->out[r->pc + 1] = (unsigned char)((hi >> 8) & 0xFF);
            }
        } else if (r->type == 1) {
            /* BL: 32-bit, re-emit at saved pc */
            int saved = a->pc;
            a->pc = r->pc;
            emit_bl_a(a, addr);
            a->pc = saved;
        } else if (r->type == 2) {
            /* B.W: 32-bit unconditional, re-emit at saved pc */
            int saved = a->pc;
            a->pc = r->pc;
            emit_b_w_a(a, addr);
            a->pc = saved;
        } else if (r->type == 3) {
            /* B<cond>.W: 32-bit conditional, recover cond from placeholder hi word */
            unsigned hi = (unsigned)a->out[r->pc] | ((unsigned)a->out[r->pc + 1] << 8);
            int cond = (hi >> 6) & 0xF;
            int saved = a->pc;
            a->pc = r->pc;
            emit_bcond_w_a(a, cond, addr);
            a->pc = saved;
        } else if (r->type == 4) {
            /* Bug-A5: R_ARM_ABS32 - 4-byte absolute address reference
             * (e.g. .word funcname in .init_array/.fini_array).
             * Patch the 4 bytes at r->pc with the resolved address. */
            a->out[r->pc]     = (unsigned char)(addr & 0xFF);
            a->out[r->pc + 1] = (unsigned char)((addr >> 8) & 0xFF);
            a->out[r->pc + 2] = (unsigned char)((addr >> 16) & 0xFF);
            a->out[r->pc + 3] = (unsigned char)((addr >> 24) & 0xFF);
        } else {
            /* type=0: legacy 16-bit branch (B/B<cond>), patch 2 bytes.
             * Kept for safety; new code uses type 2/3 above. */
            int offset = (addr - r->pc - 4) >> 1;
            unsigned hi = (unsigned)a->out[r->pc] | ((unsigned)a->out[r->pc + 1] << 8);
            if ((hi & 0xF000) == 0xE000) hi = (hi & 0xF800) | (offset & 0x7FF);
            else                          hi = (hi & 0xFF00) | (offset & 0xFF);
            a->out[r->pc]     = hi & 0xFF;
            a->out[r->pc + 1] = (hi >> 8) & 0xFF;
        }
    }
}

static int arm_emit(IrProgram *m, char *out, int cap) {
    char *text = codegen_arm(m);
    int n = ark_strlen(text);
    int i;
    if (n + 1 > cap) n = cap - 1;
    for (i = 0; i < n; i++) out[i] = text[i];
    out[n] = 0;
    return n;
}

static void arm_save_syms(ArmAsm *a);  /* forward decl */
static void arm_save_relocs(ArmAsm *a);  /* Bug-A2: forward decl */

/* Latch the ARKCC_DUMP_SYMS environment variable (host only) into
 * g_arm_dump_syms at arkcc startup, so the ARM backend has a single uniform
 * flag on both profiles. On STM32 getenv() is not available and this is a
 * no-op; the flag is set via UART there. Called from ark_compile(). */
void arm_dump_syms_init(void) {
#if ARKCC_PROFILE != ARKCC_PROFILE_STM32_MIN
    if (getenv("ARKCC_DUMP_SYMS")) g_arm_dump_syms = 1;
#endif
}

static int arm_assemble(const char *asm_text, unsigned char *out, int cap) {
    ArmAsm a;
    a.src = asm_text;
    a.out = out;
    a.pc = 0;
    a.out_cap = cap;
    a.labels = 0; a.label_count = 0; a.label_cap = 0;
    a.refs = 0; a.ref_count = 0; a.ref_cap = 0;
    a.error_count = 0;
    a.hard_errors = 0;
    a.sets = 0; a.set_count = 0; a.set_cap = 0;
    a.literals = 0; a.literal_count = 0; a.literal_cap = 0;
    a.literal_pool_start = -1;
    a.pending_weak = 0;
    a.cur_section = 0;
    /* --vector-table: hand the emitter a buffer whose first ARM_VT_SIZE bytes
     * are already claimed, zeroed.  The linker only fills the table in. */
    if (g_arm_vt_reserve) {
        int k;
        if (ARM_VT_SIZE > cap) return -1;
        for (k = 0; k < ARM_VT_SIZE; k++) out[k] = 0;
    }
    pass1_a(&a);
    a.pc = g_arm_vt_reserve ? ARM_VT_SIZE : 0;
    pass2_a(&a);
    resolve_refs_a(&a);
    /* Save symbol table for linker */
    arm_save_syms(&a);
    /* Bug-A2: Save unresolved cross-object references as relocations
     * for the linker to resolve. This must be called AFTER arm_save_syms
     * so that the global symbol/reloc arrays are populated before the
     * object file is written. */
    arm_save_relocs(&a);
    /* Debug: dump label addresses when g_arm_dump_syms is set. On host the
     * flag is latched from getenv("ARKCC_DUMP_SYMS") at arkcc startup (see
     * arm_dump_syms_init); on STM32 it is set via the UART "debug syms"
     * command, since getenv() is unavailable under semihosting. */
    if (g_arm_dump_syms) {
        {
            int di;
            ark_log("=== ARM Symbol Map (%d labels) ===\n", a.label_count);
            for (di = 0; di < a.label_count; di++) {
                if (a.labels[di].name[0] != '.')
                    ark_log("  %s 0x%X (%d)\n",
                            a.labels[di].name, a.labels[di].addr, a.labels[di].addr);
            }
            ark_log("=== End Symbol Map ===\n");
        }
    }
    /* Genuine assembly errors must fail the compile: continuing would hand
     * back machine code that does not match the source. Unresolved external
     * references are NOT in this counter (resolve_refs_a keeps them in
     * error_count), because the linker may satisfy them later.
     * Bug-A2: error_count alone therefore still does not fail the compile. */
    if (a.hard_errors > 0) {
        {
            char b1[12];
            diag_tool(DIAG_SEV_ERROR, "arkcc",
                      "%s assembly error(s), no output written",
                      diag_num(b1, a.hard_errors), 0,0,0,0,0);
        }
        return -1;
    }
    return a.pc;
}

/* arm_link - per-object link stub (intentionally a no-op).
 *
 * Design rationale: arkcc splits linking responsibility across two stages.
 *
 *  1) Backend stage (this function): called once per object right after
 *     arm_assemble, with only that object's code and no link context
 *     (lc is always NULL today - see the call sites in compiler.c).
 *     Cross-object references cannot be resolved here because we only see
 *     one object; the unresolved refs were already captured by
 *     arm_save_relocs() into g_arm_relocs for later use.
 *
 *  2) Linker stage (compiler.c -link mode): the ARKOBJ2 binary linker
 *     merges code sections from every input object, builds a global
 *     symbol table with weak-symbol precedence (ld_add_sym), and
 *     patches cross-object relocations (ld_patch_ref for BL/B.W/
 *     B<cond>.W/R_ARM_ABS32). Unresolved symbols are reported but do
 *     not abort - they may be satisfied by an external library.
 *
 * Per-mode behaviour:
 *   -S      : emits GAS-compatible assembly; real linking is delegated
 *             to arm-none-eabi-ld / arm-none-eabi-gcc externally.
 *   -c      : writes an ARKOBJ2 object (code + symbol table + reloc
 *             table) for the -link stage to consume later.
 *   -link   : runs the ARKOBJ2 binary linker described above.
 *   -> .bin : single-file direct binary; cross-object refs are left
 *             unresolved (acceptable for self-contained units).
 *
 * Keeping this function a no-op is the correct division of labour:
 * the backend assembles and exports symbols/relocs; compiler.c links.
 * Making arm_link do real work would duplicate the -link logic and
 * still could not resolve cross-object refs without all objects.
 *
 * Exception: vector table generation is handled after symbol table is
 * built (see arm_link_impl below, called via the forward declaration). */
static int arm_link(unsigned char *code, int size, struct LinkContext *lc);  /* forward decl */

/* --- Symbol table export for linker --- */
typedef struct ArmSym {
    char *name;
    int   offset;   /* byte offset from start of code section */
    int   type;     /* 0=code label, 1=data/global */
    int   is_weak;  /* 1=weak symbol (overridable by a strong definition) */
} ArmSym;

static ArmSym  g_arm_syms[ARM_MAX_SYMBOLS] __attribute__((section(".ark_buf")));
static int     g_arm_sym_count = 0;

/* Bug-A2: Relocation table for cross-object references.
 * After resolve_refs_a, any ref whose target label was not found in the
 * current object is saved here as a relocation entry. The linker will
 * resolve these using the global symbol table. */
typedef struct ArmReloc {
    char *name;    /* target symbol name */
    int   offset;  /* byte offset within code section to patch */
    int   type;    /* 1=BL(T1), 2=B.W, 3=B<cond>.W, 4=R_ARM_ABS32 (.word) */
} ArmReloc;

static ArmReloc g_arm_relocs[ARM_MAX_RELOCS] __attribute__((section(".ark_buf")));
static int      g_arm_reloc_count = 0;

/* Save symbol table after assembly */
static void arm_save_syms(ArmAsm *a) {
    int i;
    int dropped = 0;
    g_arm_sym_count = 0;
    for (i = 0; i < a->label_count; i++) {
        /* Skip local labels (starting with .L) */
        if (a->labels[i].name[0] == '.' && a->labels[i].name[1] == 'L') continue;
        if (g_arm_sym_count >= ARM_MAX_SYMBOLS) { dropped++; continue; }
        g_arm_syms[g_arm_sym_count].name   = a->labels[i].name;
        g_arm_syms[g_arm_sym_count].offset = a->labels[i].addr;
        g_arm_syms[g_arm_sym_count].type   = 0;  /* code label */
        g_arm_syms[g_arm_sym_count].is_weak = a->labels[i].is_weak;
        g_arm_sym_count++;
    }
    if (dropped) {
        /* A symbol missing from the object is a symbol -link cannot resolve. */
        char b1[12], b2[12];
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "%s symbols did not fit the %s-entry object symbol table; raise "
                  "ARM_MAX_SYMBOLS and rebuild arkcc",
                  diag_num(b1, dropped), diag_num(b2, ARM_MAX_SYMBOLS), 0,0,0,0);
        a->hard_errors++;
    }
}

/* Bug-A2: Save unresolved refs as relocations for the linker.
 * Called after resolve_refs_a; scans the ref table and saves any ref
 * whose target is not defined in the current object. */
static void arm_save_relocs(ArmAsm *a) {
    int i;
    int dropped = 0;
    g_arm_reloc_count = 0;
    for (i = 0; i < a->ref_count; i++) {
        AsmRef *r = &a->refs[i];
        int addr;
        if (r->type == 6 || r->type == ARM_RELOC_TEXT) {
            /* .arkdata / .arktext: not a reference to a label, but the offset of a
             * MOVW/MOVT pair the -link stage may have to shift.  It is saved
             * whether or not the object ends up being relocated, so that the
             * decision stays with the linker (which knows every object). */
            /* Only an entry that is really dropped counts: a ref resolved
             * inside this object never reaches the table. Breaking out of the
             * loop instead would report a full table for a file that has few
             * cross-object refs but many local ones. */
            if (g_arm_reloc_count >= ARM_MAX_RELOCS) { dropped = 1; continue; }
            g_arm_relocs[g_arm_reloc_count].name   = r->name;
            g_arm_relocs[g_arm_reloc_count].offset = r->pc;
            g_arm_relocs[g_arm_reloc_count].type   = r->type;
            g_arm_reloc_count++;
            continue;
        }
        addr = find_label_a(a, r->name);
        if (addr < 0) {
            /* Unresolved: this is a cross-object reference.
             * Save it as a relocation for the linker. */
            if (g_arm_reloc_count >= ARM_MAX_RELOCS) { dropped = 1; continue; }
            g_arm_relocs[g_arm_reloc_count].name   = r->name;
            g_arm_relocs[g_arm_reloc_count].offset = r->pc;
            g_arm_relocs[g_arm_reloc_count].type   = r->type;
            g_arm_reloc_count++;
        }
    }
    if (dropped) {
        char b1[12];
        diag_tool(DIAG_SEV_ERROR, "arkcc",
                  "more than %s cross-object relocations in this file; raise "
                  "ARM_MAX_RELOCS and rebuild arkcc",
                  diag_num(b1, ARM_MAX_RELOCS), 0,0,0,0,0);
        a->hard_errors++;
    }
}

/* Encode imm16 into a MOVW (is_movt=0) or MOVT (is_movt=1) halfword pair.
 * Same fields the pass-2 encoders above use; kept in one place so the linker's
 * relocation cannot drift from the assembler's encoding. */
static void mov_imm_encode_a(int is_movt, int rd, unsigned imm,
                            unsigned *hi, unsigned *lo) {
    unsigned ii   = (imm >> 11) & 1u;
    unsigned imm4 = (imm >> 12) & 0xFu;
    unsigned imm3 = (imm >> 8) & 7u;
    unsigned imm8 = imm & 0xFFu;
    *hi = (is_movt ? 0xF2C0u : 0xF240u) | (ii << 10) | imm4;
    *lo = (imm3 << 12) | ((unsigned)(rd & 0xF) << 8) | imm8;
}

/* Decode a MOVW/MOVT halfword pair back to imm16 + Rd.
 * Returns 0 on success, -1 if `hi` is not the MOVW/MOVT opcode the encoder
 * emits or `lo` does not look like its second halfword. */
static int mov_imm_decode_a(unsigned hi, unsigned lo,
                           int *is_movt, int *rd, unsigned *imm) {
    unsigned base = hi & 0xFBF0u;
    if (base != 0xF240u && base != 0xF2C0u) return -1;
    if ((lo & 0x8000u) != 0) return -1;          /* bit15 must be 0 */
    *is_movt = (base == 0xF2C0u);
    *rd = (int)((lo >> 8) & 0xFu);
    *imm = ((hi & 0xFu) << 12) | (((hi >> 10) & 1u) << 11) |
           (((lo >> 12) & 7u) << 8) | (lo & 0xFFu);
    return 0;
}

/* --reloc-data: shift the MOVW/MOVT pair at `off` by `delta`.
 *
 * Returns 0 when both halfwords were rewritten, -1 when the bytes are not the
 * pair the assembler marked (different register, wrong opcode, truncated
 * image).  The caller must refuse to write the image in that case: the object
 * was built without --reloc-data, and its baked address would stay stale. */
int arm_patch_movw_movt(unsigned char *code, int code_len, int off, int delta) {
    unsigned hi, lo, hi2, lo2, loimm = 0, hiimm = 0, imm;
    int is_movt = 0, rd = 0, is_movt2 = 0, rd2 = 0;
    if (off < 0 || off + 8 > code_len) return -1;
    hi  = (unsigned)code[off]     | ((unsigned)code[off + 1] << 8);
    lo  = (unsigned)code[off + 2] | ((unsigned)code[off + 3] << 8);
    hi2 = (unsigned)code[off + 4] | ((unsigned)code[off + 5] << 8);
    lo2 = (unsigned)code[off + 6] | ((unsigned)code[off + 7] << 8);
    if (mov_imm_decode_a(hi, lo, &is_movt, &rd, &loimm) != 0) return -1;
    if (is_movt) return -1;                       /* pair must start with MOVW */
    if (mov_imm_decode_a(hi2, lo2, &is_movt2, &rd2, &hiimm) != 0) return -1;
    if (!is_movt2 || rd2 != rd) return -1;        /* must be MOVT of the same Rd */
    imm = (loimm | (hiimm << 16)) + (unsigned)delta;
    mov_imm_encode_a(0, rd, imm & 0xFFFFu, &hi, &lo);
    mov_imm_encode_a(1, rd, (imm >> 16) & 0xFFFFu, &hi2, &lo2);
    code[off]     = (unsigned char)(hi & 0xFF);
    code[off + 1] = (unsigned char)((hi >> 8) & 0xFF);
    code[off + 2] = (unsigned char)(lo & 0xFF);
    code[off + 3] = (unsigned char)((lo >> 8) & 0xFF);
    code[off + 4] = (unsigned char)(hi2 & 0xFF);
    code[off + 5] = (unsigned char)((hi2 >> 8) & 0xFF);
    code[off + 6] = (unsigned char)(lo2 & 0xFF);
    code[off + 7] = (unsigned char)((lo2 >> 8) & 0xFF);
    return 0;
}

/* Unresolved references of the last assembly (see resolve_refs_a). */
int arm_get_unresolved_count(void) { return g_arm_unresolved; }
const char *arm_get_unresolved_name(void) { return g_arm_unresolved_first; }

/* Export symbol info for the linker */
int arm_get_symbol_count(void) { return g_arm_sym_count; }
const char *arm_get_symbol_name(int idx) { return g_arm_syms[idx].name; }
int arm_get_symbol_offset(int idx) { return g_arm_syms[idx].offset; }
int arm_get_symbol_type(int idx) { return g_arm_syms[idx].type; }
int arm_get_symbol_weak(int idx) { return g_arm_syms[idx].is_weak; }

/* SRAM window covered by this translation unit's .data + .bss (absolute
 * addresses; both equal g_data_base when the unit declares no RAM objects).
 * Exported so the -link stage can detect two objects that were each laid out
 * from the same SRAM base: -link concatenates code but does not relocate
 * data, so such a pair would silently alias its globals. */
unsigned arm_get_ram_start(void) { return g_data_sram_start; }
/* --reloc-text: the base every absolute address in this object was computed
 * against.  Written into the object as __arkcc_text_base so the linker can
 * work out how far it had to move this object. */
unsigned arm_get_text_base(void) { return (unsigned)g_flash_base; }
unsigned arm_get_ram_end(void)   { return g_bss_sram_end; }

/* Bug-A2: Export relocation info for the linker */
int arm_get_reloc_count(void) { return g_arm_reloc_count; }
const char *arm_get_reloc_name(int idx) { return g_arm_relocs[idx].name; }
int arm_get_reloc_offset(int idx) { return g_arm_relocs[idx].offset; }
int arm_get_reloc_type(int idx) { return g_arm_relocs[idx].type; }

/* === Vector table generation (single-file .bin mode) ===
 * If Reset_Handler is defined, generates a Cortex-M vector table at the start
 * of the binary. Safe because Thumb-2 branches use PC-relative offsets that
 * survive a uniform shift; absolute (.word) refs are already resolved by
 * resolve_refs_a within the object.
 *
 * Every entry must carry the Thumb bit (bit 0): on exception entry the core
 * loads EPSR.T from that bit, so an even address makes a Cortex-M core take
 * an INVSTATE UsageFault instead of running the handler. Real STM32 vector
 * tables are odd for the same reason. */
static const char *g_arm_vector_names[] = {
    NULL, "Reset_Handler", "NMI_Handler", "HardFault_Handler",
    "MemManage_Handler", "BusFault_Handler", "UsageFault_Handler",
    NULL, NULL, NULL, NULL,
    "SVC_Handler", "DebugMon_Handler", NULL,
    "PendSV_Handler", "SysTick_Handler"
};
#define ARM_SYSTEM_VECTOR_COUNT 16

static int arm_link(unsigned char *code, int size, struct LinkContext *lc) {
    int i, reset_idx = -1, default_idx = -1;
    unsigned flash_base, initial_sp;
    int new_size;
    (void)lc;

    /* Find Reset_Handler in symbol table */
    for (i = 0; i < g_arm_sym_count; i++) {
        if (ark_streq(g_arm_syms[i].name, "Reset_Handler")) { reset_idx = i; break; }
    }
    if (reset_idx < 0) return size;  /* no Reset_Handler: skip vector table */

    /* Find Default_Handler for filling unresolved entries */
    for (i = 0; i < g_arm_sym_count; i++) {
        if (ark_streq(g_arm_syms[i].name, "Default_Handler")) { default_idx = i; break; }
    }

    /* Determine flash_base and initial_sp from scatter or defaults */
    flash_base = g_flash_base;
    initial_sp = g_bss_base + 0x20000;  /* default: SRAM + 128KB */
#if ARKCC_FEATURE_SCATTER
    if (g_scatter_loaded) {
        ScatterExecRegion *er_rw = scatter_find_er_by_attr(&g_scatter, SCATTER_ATTR_RW);
        if (er_rw && er_rw->size > 0)
            initial_sp = er_rw->load_addr + er_rw->size;
    }
#endif

    if (g_arm_vt_reserve) {
        /* --vector-table: the emitter started at ARM_VT_SIZE, so the table
         * area is already in place and every address is already correct.
         * Moving anything here would shift the image away from the addresses
         * the assembler encoded (that was the bug this option exists to fix). */
        new_size = size;
    } else {
        new_size = size + ARM_VT_SIZE;
        if (new_size > ARK_CODE_BUF_SIZE) return -1;

        /* Move existing code right by ARM_VT_SIZE bytes */
        for (i = size - 1; i >= 0; i--) code[i + ARM_VT_SIZE] = code[i];

        /* Adjust all symbol offsets by ARM_VT_SIZE */
        for (i = 0; i < g_arm_sym_count; i++)
            g_arm_syms[i].offset += ARM_VT_SIZE;
    }

    /* Generate vector table at code[0] */
    {
        /* Bit 0 is the Thumb bit; a zero entry stays zero ("no handler"). */
        unsigned default_addr = (default_idx >= 0) ?
            ((flash_base + (unsigned)g_arm_syms[default_idx].offset) | 1u) : 0;
        /* Entry 0: initial SP */
        code[0] = (unsigned char)(initial_sp & 0xFF);
        code[1] = (unsigned char)((initial_sp >> 8) & 0xFF);
        code[2] = (unsigned char)((initial_sp >> 16) & 0xFF);
        code[3] = (unsigned char)((initial_sp >> 24) & 0xFF);
        /* Fill entries 1..N with Default_Handler (or 0) */
        for (i = 1; i < ARM_VT_ENTRIES; i++) {
            code[i*4]   = (unsigned char)(default_addr & 0xFF);
            code[i*4+1] = (unsigned char)((default_addr >> 8) & 0xFF);
            code[i*4+2] = (unsigned char)((default_addr >> 16) & 0xFF);
            code[i*4+3] = (unsigned char)((default_addr >> 24) & 0xFF);
        }
        /* Fill in known system exception handlers */
        for (i = 1; i < ARM_SYSTEM_VECTOR_COUNT; i++) {
            if (g_arm_vector_names[i]) {
                int j;
                for (j = 0; j < g_arm_sym_count; j++) {
                    if (ark_streq(g_arm_syms[j].name, g_arm_vector_names[i])) {
                        unsigned addr = (flash_base + (unsigned)g_arm_syms[j].offset) | 1u;
                        code[i*4]   = (unsigned char)(addr & 0xFF);
                        code[i*4+1] = (unsigned char)((addr >> 8) & 0xFF);
                        code[i*4+2] = (unsigned char)((addr >> 16) & 0xFF);
                        code[i*4+3] = (unsigned char)((addr >> 24) & 0xFF);
                        break;
                    }
                }
            }
        }
    }
    return new_size;
}

static const ArkBackend g_arm_backend = {
    "arm",
    "armv7m-thumb2-eabi",
    4,
    arm_emit,
    arm_assemble,
    arm_link,
    ARK_ABI_SYSV
};

const ArkBackend *ark_backend_arm(void) {
    return &g_arm_backend;
}
