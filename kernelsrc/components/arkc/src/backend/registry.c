/* Ark C Compiler - backend registry
 * Wires up all available backends. To add a new backend, just add a line to g_backends[].
 *
 * Backends can be trimmed via compile-time macros:
 *   ARKCC_BACKEND_ARM / ARKCC_BACKEND_X86_64 /
 *   ARKCC_BACKEND_RISCV32 / ARKCC_BACKEND_C
 * All are enabled by default (host); STM32 ports can disable unused backends via -D,
 * in which case the Makefile no longer compiles the corresponding backend_xxx.c and
 * no link errors occur.
 */
#include "backend.h"
#include "platform.h"
#include "memory.h"

#ifndef ARKCC_BACKEND_ARM
#define ARKCC_BACKEND_ARM      1
#endif
#ifndef ARKCC_BACKEND_X86_64
#define ARKCC_BACKEND_X86_64   1
#endif
#ifndef ARKCC_BACKEND_RISCV32
#define ARKCC_BACKEND_RISCV32  1
#endif
#ifndef ARKCC_BACKEND_C
#define ARKCC_BACKEND_C        1
#endif

static const ArkBackend *g_backends[8];
static int g_count = 0;
static int g_inited = 0;

static void registry_init(void) {
    if (g_inited) return;
    g_inited = 1;
#if ARKCC_BACKEND_ARM
    g_backends[g_count++] = ark_backend_arm();
#endif
#if ARKCC_BACKEND_X86_64
    /* Registered exactly once. The old MSVC variant (ark_backend_x86_64_msvc)
     * returns this same object since B01, so registering it a second time only
     * made --list-backends print "x86" twice and made lookups ambiguous. Name
     * variants such as x86_64 / x86_64-msvc / i386 are handled by the alias
     * table below instead of by extra registry slots. */
    g_backends[g_count++] = ark_backend_x86_64();
#endif
#if ARKCC_BACKEND_RISCV32
    g_backends[g_count++] = ark_backend_riscv32();
#endif
#if ARKCC_BACKEND_C
    g_backends[g_count++] = ark_backend_c();
#endif
}

/* Alternative spellings accepted by -target, resolved after an exact match
 * fails. Keeping them here (instead of registering duplicate backends) leaves
 * ark_backend_list() at one line per backend while users can still write the
 * name they expect from other toolchains. */
static const struct { const char *alias; const char *target; } g_backend_aliases[] = {
    { "x86_64",      "x86" },  /* directory/triple spelling of the i386 backend */
    { "x86-64",      "x86" },
    { "x86_64-msvc", "x86" },  /* legacy B01 name: same cdecl backend now      */
    { "x86-msvc",    "x86" },
    { "i386",        "x86" },
    { "i686",        "x86" },
    { "thumb",       "arm" },
    { "thumb2",      "arm" },
    { "cortex-m",    "arm" }
};
#define BACKEND_ALIAS_COUNT ((int)(sizeof(g_backend_aliases) / sizeof(g_backend_aliases[0])))

/* Returns the canonical backend name for an alias, or 0 if "name" is not an
 * alias. */
static const char *backend_alias(const char *name) {
    int i;
    if (!name) return 0;
    for (i = 0; i < BACKEND_ALIAS_COUNT; i++) {
        if (ark_streq(g_backend_aliases[i].alias, name)) return g_backend_aliases[i].target;
    }
    return 0;
}

const ArkBackend *ark_backend_find(const char *name) {
    int i;
    const char *canonical;
    registry_init();
    if (!name) return 0;
    for (i = 0; i < g_count; i++) {
        if (ark_streq(g_backends[i]->name, name)) return g_backends[i];
    }
    canonical = backend_alias(name);
    if (!canonical) return 0;
    for (i = 0; i < g_count; i++) {
        if (ark_streq(g_backends[i]->name, canonical)) return g_backends[i];
    }
    return 0;
}

const ArkBackend *ark_backend_default(void) {
    registry_init();
    /* Default to ARM, preserve original behavior */
    return g_backends[0];
}

void ark_backend_list(void) {
    int i, j;
    registry_init();
    ark_log("Available targets:\n");
    for (i = 0; i < g_count; i++) {
        const char *name = g_backends[i]->name;
        int first_alias = 1;
        ark_log("  %-10s %s\n", name, g_backends[i]->triple);
        /* Collect every alias that resolves to this backend, so this list is
         * the single place to look up an accepted -target spelling. */
        for (j = 0; j < BACKEND_ALIAS_COUNT; j++) {
            if (!ark_streq(g_backend_aliases[j].target, name)) continue;
            if (first_alias) {
                ark_log("    aliases: ");
                first_alias = 0;
            } else {
                ark_log(", ");
            }
            ark_log("%s", g_backend_aliases[j].alias);
        }
        if (!first_alias) ark_log("\n");
    }
}
