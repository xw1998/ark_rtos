/* Ark C Compiler - backend.h
 * Backend abstraction layer: turns "emit assembly/machine code" into an interface.
 *
 * Each backend is implemented in src/backend/<name>/backend_<name>.c and exposes
 * an ArkBackend pointer via the exported function ark_backend_<name>().
 * registry.c links all available backends together; main selects one via -target.
 *
 * Most backends do the bulk of their work in emit_asm; assemble/link can be stubs
 * for software-style backends (c/x86_64/riscv32), simply dumping the assembly text
 * as bytes and letting the user post-process with gcc/as/nasm.
 */
#ifndef ARK_BACKEND_H
#define ARK_BACKEND_H

#include "common.h"
#include "arkcc_config.h"   /* ARKCC_BACKEND_* switches live here */
#include "ir.h"

/* Placeholder link context, used by the ARM backend; other backends may ignore it. */
struct LinkContext {
    int dummy;
};

/* ABI selector (B01: x86 backend now targets i386 cdecl).
 *   0 = cdecl (default): all args on stack, caller cleans up.
 *   1 = MSVC x64 ABI (legacy, now maps to cdecl). ARK_ABI_MSVC is retained
 *       for source compatibility but the x86 backend ignores it. */
#define ARK_ABI_SYSV 0
#define ARK_ABI_MSVC 1

typedef struct ArkBackend {
    const char *name;            /* "arm", "x86_64", "riscv32", "c" */
    const char *triple;          /* description string */
    int  pointer_size;           /* bytes */
    /* IR -> text assembly. Returns bytes written (excluding terminating NUL); -1 on failure. */
    int  (*emit_asm)(IrProgram *m, char *out, int cap);
    /* Text assembly -> machine code. Returns -1 on failure. */
    int  (*assemble)(const char *asm_text, unsigned char *out, int cap);
    /* Linker relocation. Returns -1 on failure. */
    int  (*link)(unsigned char *code, int size, struct LinkContext *lc);
    int  abi;                    /* x86 ABI: ARK_ABI_SYSV (cdecl, default) or ARK_ABI_MSVC (legacy) */
} ArkBackend;

const ArkBackend *ark_backend_find(const char *name);
const ArkBackend *ark_backend_default(void);
void ark_backend_list(void);

/* Per-backend exported functions */
const ArkBackend *ark_backend_arm(void);
const ArkBackend *ark_backend_x86_64(void);
const ArkBackend *ark_backend_x86_64_msvc(void);
const ArkBackend *ark_backend_riscv32(void);
const ArkBackend *ark_backend_c(void);

/* ARM backend debug flag (defined in backend_arm.c). When set, the ARM
 * assembler dumps its symbol map after assembly. On host it is latched from
 * the ARKCC_DUMP_SYMS environment variable at arkcc startup via
 * arm_dump_syms_init(); on STM32 getenv() is unavailable under semihosting,
 * so the flag is set via a UART "debug syms" command instead. */
extern int g_arm_dump_syms;

/* ARM backend: reserve the Cortex-M vector-table area at the front of the code
 * image (--vector-table).  When set, the assembler starts emitting at
 * ARM_VT_SIZE instead of 0 and the linker writes the table into the reserved
 * gap, so code keeps the addresses the assembler gave it.
 *
 * Why this exists: arm_link used to *prepend* the table and shift the whole
 * image right by 0x400 at link time.  Thumb-2 branches survive a uniform shift
 * (they are PC-relative), but absolute addresses baked in by the assembler do
 * not: a string literal linked at 0x24070000 stays encoded as 0x24070000 while
 * its bytes move to 0x24070400.  Reserving up front removes the shift, so both
 * kinds of reference stay correct. */
extern int g_arm_vt_reserve;
/* 1 = the selected CPU has a double-precision FPU (see backend_arm.c). */
extern int g_arm_fpu_dp;
void arm_dump_syms_init(void);

#endif /* ARK_BACKEND_H */
