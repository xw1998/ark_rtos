/**
* @file test_shell_ushell_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_ushell.c.
*
* What this proves: the user-command registry (SVC 0x1A on the App side)
* behaves as documented -- registration refuses what it says it refuses,
* the copied name/help are bounded, unregistration removes both the registry
* row and the trampoline row in the kernel command table (shifting the later
* rows so help order has no hole), and the trampoline refuses to call a
* handler that is no longer inside a loaded image instead of jumping into
* erased Flash.
*
* What it does NOT prove:
*   - that a real App can reach this code through SVC 0x1A: the entry is
*     called directly here, with no privilege transition and no user RAM
*     window check;
*   - that arkrtos_kernel_ushell_active really changes how buffered SVC
*     services are validated -- this only asserts the flag is raised for the
*     duration of the callback and dropped afterwards;
*   - that a real partition table has the shape of the fixture below.
*
* The fixture values are arbitrary but distinctive, so a registry that
* mismatches a row is caught rather than passing by coincidence.
*
* @author xw
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_shell_internal.h"

/* ------------------------------------------------------------ assertions */

static int g_checks;
static int g_failed;

#define CK(cond)                                                       \
    do {                                                               \
        g_checks++;                                                    \
        if (!(cond)) {                                                 \
            g_failed++;                                                \
            printf("FAIL line %d: %s\n", __LINE__, #cond);             \
        }                                                              \
    } while (0)

#define CK_I(got, want)                                                \
    do {                                                               \
        long g_ = (long)(got);                                         \
        long w_ = (long)(want);                                        \
        g_checks++;                                                    \
        if (g_ != w_) {                                                \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got %ld want %ld)\n",            \
                   __LINE__, #got, g_, w_);                            \
        }                                                              \
    } while (0)

#define CK_S(got, want)                                                \
    do {                                                               \
        const char *g_ = (got);                                        \
        const char *w_ = (want);                                       \
        g_checks++;                                                    \
        if ((g_ == NULL) || (strcmp(g_, w_) != 0)) {                   \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got \"%s\" want \"%s\")\n",      \
                   __LINE__, #got, (g_ != NULL) ? g_ : "(null)", w_);  \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ capture */

static char g_out[8192];
static size_t g_out_len;
static unsigned g_calls;
static unsigned g_chunk_len[32];

static void cap_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0u;
    g_calls = 0u;
    memset(g_chunk_len, 0, sizeof(g_chunk_len));
}

static void cap_add(const char *s, size_t n)
{
    size_t room = sizeof(g_out) - 1u - g_out_len;

    if (n > room) {
        n = room;
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
    g_out[g_out_len] = '\0';
}

static int out_has(const char *needle)
{
    return strstr(g_out, needle) != NULL;
}

/* ------------------------------------------------------------ stubs */

arkrtos_ushell_entry_t g_ushell[ARKRTOS_USHELL_MAX_CMDS];
ark_shell_cmd_t g_cmd_table[ARK_SHELL_MAX_COMMANDS];
int g_cmd_count;
int32 arkrtos_current_task_id;
uint32 arkrtos_kernel_ushell_active;

static arkrtos_partition_table_t g_pt;
static arkrtos_partition_table_t *g_ptp;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return g_ptp;
}

void sh_out(const char *s)
{
    size_t n = strlen(s);

    if (g_calls < (unsigned)(sizeof(g_chunk_len) / sizeof(g_chunk_len[0]))) {
        g_chunk_len[g_calls] = (unsigned)n;
    }
    g_calls++;
    cap_add(s, n);
}

void ark_shell_printf(const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        cap_add(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1u);
    }
}


/* ------------------------------------------------------------ helpers */

static void reset_env(void)
{
    memset(g_ushell, 0, sizeof(g_ushell));
    memset(g_cmd_table, 0, sizeof(g_cmd_table));
    memset(&g_pt, 0, sizeof(g_pt));
    g_ptp = &g_pt;
    g_cmd_count = 0;
    arkrtos_current_task_id = 0;
    arkrtos_kernel_ushell_active = 0u;
    cap_reset();
}

static arkrtos_ushell_cmd_t mk(const char *name, int (*fn)(int, char **),
                               const char *help, uint32 max_args)
{
    arkrtos_ushell_cmd_t c;

    c.name = name;
    c.func = fn;
    c.help = help;
    c.max_args = max_args;
    return c;
}

static int cb_args;
static int cb_called;
static int cb_rc = 42;
static uint32 cb_active_seen = 7u;

static int cb_ok(int argc, char **argv)
{
    (void)argv;
    cb_called++;
    cb_args = argc;
    cb_active_seen = arkrtos_kernel_ushell_active;
    return cb_rc;
}

static int cb_other(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cb_called++;
    return -9;
}

static uint32 addr_of(int (*fn)(int, char **))
{
    return (uint32)(uintptr_t)fn;
}

/* The trampoline is file-static; the only handle we have is the row it puts
 * in the kernel command table.  Every lookup test goes through that row, so
 * what is exercised is the path a real dispatch would take. */
static int (*table_fn(void))(int, char **)
{
    if (g_cmd_count <= 0) {
        return NULL;
    }
    return g_cmd_table[g_cmd_count - 1].func;
}

static int call_tr(int (*fn)(int, char **), int argc, char **argv)
{
    return fn(argc, argv);
}

/* ------------------------------------------------------------ name rules */

static void t_name_rules(void)
{
    arkrtos_ushell_cmd_t c;

    reset_env();
    c = mk("Alpha", cb_ok, "h", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);

    /* duplicate is decided case-insensitively, over the whole name */
    c = mk("aLPHa", cb_ok, "h", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_DUP);
    CK_I(g_cmd_count, 1);

    /* same prefix but a longer name is not the same command */
    c = mk("Alphabet", cb_ok, "h", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 2);

    /* unregister is case-insensitive too */
    CK_I(arkrtos_shell_ext_unregister("ALPHA"), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 1);
    CK_I(arkrtos_shell_ext_unregister("alpha"), ARKRTOS_USHELL_ERR_NOTFND);
}

/* ------------------------------------------------------------ copy bounds */

static void t_copy_bounds(void)
{
    arkrtos_ushell_cmd_t c;
    char name[40];
    char help[120];
    uint32 i;

    reset_env();

    /* exactly NAME_MAX characters is accepted, one more is not */
    for (i = 0u; i < (uint32)ARKRTOS_USHELL_NAME_MAX; i++) {
        name[i] = (char)('a' + (char)(i % 26u));
    }
    name[ARKRTOS_USHELL_NAME_MAX] = '\0';
    c = mk(name, cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I((int)strlen(g_ushell[0].name), ARKRTOS_USHELL_NAME_MAX);
    CK_S(g_ushell[0].name, name);

    name[ARKRTOS_USHELL_NAME_MAX] = 'z';
    name[ARKRTOS_USHELL_NAME_MAX + 1] = '\0';
    c = mk(name, cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_PARAM);

    /* help is clipped to HELP_MAX rather than rejected or overflowed */
    for (i = 0u; i < (uint32)ARKRTOS_USHELL_HELP_MAX + 30u; i++) {
        help[i] = (char)('0' + (char)(i % 10u));
    }
    help[ARKRTOS_USHELL_HELP_MAX + 30u] = '\0';
    c = mk("hm", cb_ok, help, 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I((int)strlen(g_ushell[1].help), ARKRTOS_USHELL_HELP_MAX);
    CK(memcmp(g_ushell[1].help, help, (size_t)ARKRTOS_USHELL_HELP_MAX) == 0);
    CK_I((int)g_ushell[1].help[ARKRTOS_USHELL_HELP_MAX], 0);

    /* a NULL help is copied as the empty string, not left as garbage */
    c = mk("nh", cb_ok, NULL, 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I((int)strlen(g_ushell[2].help), 0);
}

/* ------------------------------------------------------------ refusals */

static void t_register_refusals(void)
{
    arkrtos_ushell_cmd_t c;
    arkrtos_ushell_cmd_t bad;
    uint32 i;

    reset_env();

    CK_I(arkrtos_shell_ext_register(NULL), ARKRTOS_USHELL_ERR_PARAM);

    bad = mk(NULL, cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&bad), ARKRTOS_USHELL_ERR_PARAM);

    bad = mk("nofn", NULL, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&bad), ARKRTOS_USHELL_ERR_PARAM);

    bad = mk("", cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&bad), ARKRTOS_USHELL_ERR_PARAM);

    /* not one byte of state moved for any of the four refusals */
    CK_I(g_cmd_count, 0);
    CK_I((int)g_ushell[0].in_use, 0);

    /* the kernel command table being full is checked first */
    g_cmd_count = ARK_SHELL_MAX_COMMANDS;
    c = mk("any", cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_FULL);
    CK_I((int)g_ushell[0].in_use, 0);

    /* the registry itself filling up is a different FULL */
    reset_env();
    for (i = 0u; i < (uint32)ARKRTOS_USHELL_MAX_CMDS; i++) {
        char nm[8];
        nm[0] = 'c';
        nm[1] = (char)('0' + (char)i);
        nm[2] = '\0';
        c = mk(nm, cb_ok, "h", 1u);
        CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    }
    CK_I(g_cmd_count, (int)ARKRTOS_USHELL_MAX_CMDS);
    c = mk("one-too-many", cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_FULL);
    CK_I(g_cmd_count, (int)ARKRTOS_USHELL_MAX_CMDS);
}

/* ------------------------------------------------------------ success path */

static void t_register_success(void)
{
    arkrtos_ushell_cmd_t c;

    reset_env();

    arkrtos_current_task_id = 7;
    c = mk("hello", cb_ok, "say hello", 3u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 1);
    CK_I((int)g_ushell[0].in_use, 1);
    CK_S(g_ushell[0].name, "hello");
    CK_S(g_ushell[0].help, "say hello");
    CK(g_ushell[0].func == cb_ok);
    CK_I(g_ushell[0].max_args, 3);
    CK_I(g_ushell[0].owner_task, 7);

    /* the kernel row points at the copied strings, not at the caller's */
    CK(g_cmd_table[0].name == g_ushell[0].name);
    CK(g_cmd_table[0].help == g_ushell[0].help);
    CK(g_cmd_table[0].max_args == 3);
    CK(g_cmd_table[0].func != NULL);
    CK(g_cmd_table[0].func != cb_ok);

    /* a kernel-side registration (task id 0) is not tied to a task */
    arkrtos_current_task_id = 0;
    c = mk("kern", cb_ok, "k", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I(g_ushell[1].owner_task, 0);

    /* a freed slot is reused before an untouched one */
    CK_I(arkrtos_shell_ext_unregister("hello"), ARKRTOS_USHELL_OK);
    c = mk("again", cb_other, "a", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_S(g_ushell[0].name, "again");
    CK(g_ushell[0].func == cb_other);
    CK_I(g_cmd_count, 2);
}

/* ------------------------------------------------------------ unregister */

static void t_unregister_shift(void)
{
    arkrtos_ushell_cmd_t c;

    reset_env();

    c = mk("alpha", cb_ok, "A", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    c = mk("bravo", cb_ok, "B", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    c = mk("charlie", cb_ok, "C", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 3);

    CK_I(arkrtos_shell_ext_unregister("bravo"), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 2);

    /* order is help order: charlie moved up, no hole left behind */
    CK_S(g_cmd_table[0].name, "alpha");
    CK_S(g_cmd_table[1].name, "charlie");
    /* the row past g_cmd_count is not read by anyone: the shift does not
     * clear it, and the stale pointer is only kept alive by the registry
     * array that still owns the storage -- documented, not asserted */

    /* the registry slot is cleared, strings included */
    CK_I((int)g_ushell[1].in_use, 0);
    CK(g_ushell[1].func == NULL);
    CK_I((int)g_ushell[1].name[0], 0);
    CK_I((int)g_ushell[1].help[0], 0);

    /* the surviving rows still point at their own registry strings */
    CK(g_cmd_table[0].name == g_ushell[0].name);
    CK(g_cmd_table[1].name == g_ushell[2].name);

    CK_I(arkrtos_shell_ext_unregister("nope"), ARKRTOS_USHELL_ERR_NOTFND);
    CK_I(arkrtos_shell_ext_unregister(NULL), ARKRTOS_USHELL_ERR_PARAM);

    /* removing the first row shifts both followers */
    CK_I(arkrtos_shell_ext_unregister("alpha"), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 1);
    CK_S(g_cmd_table[0].name, "charlie");
}

/* ------------------------------------------------------------ trampoline */

static void t_trampoline_lookup(void)
{
    arkrtos_ushell_cmd_t c;
    int (*tr)(int, char **);
    char *argv[2];
    char nm_mycm[] = "MyCm";
    char nm_mycmd[] = "MYCMD";
    char nm_other[] = "other";

    reset_env();

    c = mk("MyCmd", cb_ok, "h", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    tr = table_fn();
    CK(tr != NULL);

    /* no argv / no argv[0] / argc <= 0 are all "not available", never a call */
    cb_called = 0;
    cap_reset();
    CK_I(call_tr(tr, 0, NULL), -1);
    CK_I(cb_called, 0);
    CK(out_has("user command not available"));

    cap_reset();
    CK_I(call_tr(tr, 1, NULL), -1);
    CK_I(cb_called, 0);
    CK(out_has("user command not available"));

    argv[0] = NULL;
    argv[1] = NULL;
    cap_reset();
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);
    CK(out_has("user command not available"));

    /* a name that only shares a prefix does not match */
    argv[0] = nm_mycm;
    cap_reset();
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);
    CK(out_has("user command not available"));

    /* matched case-insensitively, but the handler is not inside any loaded
     * image in this fixture, so it must refuse instead of calling */
    argv[0] = nm_mycmd;
    cap_reset();
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);
    CK(out_has("handler no longer available"));
    CK(out_has("MyCmd"));

    /* a command that was never registered */
    argv[0] = nm_other;
    cap_reset();
    CK_I(call_tr(tr, 1, argv), -1);
    CK(out_has("user command not available"));
}

static void t_trampoline_alive(void)
{
    arkrtos_ushell_cmd_t c;
    int (*tr)(int, char **);
    char *argv[2];
    char nm_run[] = "run";
    uint32 a = addr_of(cb_ok);

    reset_env();
    c = mk("run", cb_ok, "h", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    tr = table_fn();
    argv[0] = nm_run;
    argv[1] = NULL;

    /* one loaded slot really containing the handler */
    g_pt.slot_max = 2u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0] = a - 0x10u;
    g_pt.slot_size[0] = 0x100u;

    cb_called = 0;
    cb_args = 0;
    cb_active_seen = 7u;
    arkrtos_kernel_ushell_active = 0u;
    cap_reset();
    CK_I(call_tr(tr, 2, argv), 42);
    CK_I(cb_called, 1);
    CK_I(cb_args, 2);
    CK_I((long)cb_active_seen, 1);          /* raised for the call */
    CK_I((long)arkrtos_kernel_ushell_active, 0);  /* dropped right after */
    CK_I(g_out_len, 0u);                    /* nothing printed */

    /* RUNNING is alive too */
    g_pt.slot_state[0] = ARKRTOS_APP_SLOT_RUNNING;
    cb_called = 0;
    CK_I(call_tr(tr, 1, argv), 42);
    CK_I(cb_called, 1);

    /* boundary: the first byte is inside, one past the end is not */
    g_pt.slot_base[0] = a;
    g_pt.slot_size[0] = 1u;
    cb_called = 0;
    CK_I(call_tr(tr, 1, argv), 42);
    CK_I(cb_called, 1);

    g_pt.slot_base[0] = a + 1u;
    g_pt.slot_size[0] = 0x100u;
    cb_called = 0;
    cap_reset();
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);
    CK(out_has("handler no longer available"));

    /* a slot the table calls free is skipped even if its window covers us */
    g_pt.slot_type[0] = ARKRTOS_SLOT_FREE;
    g_pt.slot_base[0] = a - 0x10u;
    g_pt.slot_size[0] = 0x100u;
    g_pt.slot_state[0] = ARKRTOS_APP_SLOT_LOADED;
    cb_called = 0;
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);

    /* base 0 is treated as "no window", never as "covers address 0..size" */
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[0] = 0u;
    g_pt.slot_size[0] = 0xFFFFFFFFu;
    cb_called = 0;
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);

    /* a slot that is not loaded/running is skipped; the second slot can
     * still match, so the search must not stop at the first candidate */
    g_pt.slot_max = 3u;
    g_pt.slot_state[0] = ARKRTOS_APP_SLOT_EMPTY;
    g_pt.slot_base[0] = a - 0x10u;
    g_pt.slot_size[0] = 0x100u;
    g_pt.slot_type[1] = ARKRTOS_SLOT_APP;
    g_pt.slot_state[1] = ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[1] = a - 0x10u;
    g_pt.slot_size[1] = 0x100u;
    cb_called = 0;
    CK_I(call_tr(tr, 1, argv), 42);
    CK_I(cb_called, 1);

    /* slot_max clamps the loop: a match beyond it is not seen */
    g_pt.slot_max = 1u;
    cb_called = 0;
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);

    /* no partition table at all is "cannot prove it is alive", not a call */
    g_ptp = NULL;
    cb_called = 0;
    cap_reset();
    CK_I(call_tr(tr, 1, argv), -1);
    CK_I(cb_called, 0);
    CK(out_has("handler no longer available"));
    g_ptp = &g_pt;
}

/* ------------------------------------------------------------ print_n */

static void t_print_n(void)
{
    char big[200];
    char expect[200];
    uint32 i;

    reset_env();

    CK_I(arkrtos_shell_print_n(NULL, 10u), ARKRTOS_USHELL_ERR_PARAM);
    CK_I(g_calls, 0u);

    /* zero length is a no-op that still reports success */
    CK_I(arkrtos_shell_print_n("abc", 0u), ARKRTOS_USHELL_OK);
    CK_I(g_calls, 0u);
    CK_I(g_out_len, 0u);

    /* one short message is one call, byte for byte */
    CK_I(arkrtos_shell_print_n("hello", 5u), ARKRTOS_USHELL_OK);
    CK_I(g_calls, 1u);
    CK_S(g_out, "hello");

    /* a message longer than the chunk is split 64 / 64 / rest */
    cap_reset();
    for (i = 0u; i < (uint32)sizeof(big) - 1u; i++) {
        big[i] = (char)('a' + (char)(i % 26u));
    }
    big[sizeof(big) - 1u] = '\0';
    CK_I(arkrtos_shell_print_n(big, 130u), ARKRTOS_USHELL_OK);
    CK_I(g_calls, 3u);
    CK_I((long)g_chunk_len[0], 64);
    CK_I((long)g_chunk_len[1], 64);
    CK_I((long)g_chunk_len[2], 2);
    memcpy(expect, big, 130u);
    expect[130] = '\0';
    CK_S(g_out, expect);

    /* an exact multiple of the chunk does not emit an empty tail */
    cap_reset();
    CK_I(arkrtos_shell_print_n(big, 128u), ARKRTOS_USHELL_OK);
    CK_I(g_calls, 2u);
    CK_I((long)g_chunk_len[0], 64);
    CK_I((long)g_chunk_len[1], 64);

    /* the embedded copy is not NUL-walking: bytes after len are ignored */
    cap_reset();
    CK_I(arkrtos_shell_print_n("abc\0def", 3u), ARKRTOS_USHELL_OK);
    CK_S(g_out, "abc");
}

/* ------------------------------------------------------------ main */

int main(void)
{
    t_name_rules();
    t_copy_bounds();
    t_register_refusals();
    t_register_success();
    t_unregister_shift();
    t_trampoline_lookup();
    t_trampoline_alive();
    t_print_n();

    if (g_failed != 0) {
        printf("USHELL-HOST FAIL  %d/%d checks failed\n", g_failed, g_checks);
        return 1;
    }
    printf("USHELL-HOST OK  %d checks\n", g_checks);
    return 0;
}
