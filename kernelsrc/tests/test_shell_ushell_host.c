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
#include "arkrtos_ushell_upcall.h"

/* Mirror of the policy default in arkrtos_shell_ushell.c, so this one file can
 * be built twice: with -DARKRTOS_USE_USHELL_APP_CB=1 (the registry behaviour)
 * and without (the fail-closed refusal on a privilege-separated + paging
 * board). Keep the two in step; ci_gate builds both arms. */
#ifndef ARKRTOS_USE_USHELL_APP_CB
#  if (ARKRTOS_USE_PRIV == 1) && (ARKRTOS_USE_PAGING == 1)
#    define ARKRTOS_USE_USHELL_APP_CB 0
#  else
#    define ARKRTOS_USE_USHELL_APP_CB 1
#  endif
#endif

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

/* Only referenced by the fail-closed build (ARKRTOS_USE_USHELL_APP_CB == 0):
 * arkrtos_shell_ext_register() asks who is calling so it can refuse an App
 * (is_priv == 0) on a privilege-separated + paging build. */
static arkrtos_task_t g_me;

arkrtos_task_t *arkrtos_task_get_current(void)
{
    return &g_me;
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


/* ------------------------------------------------------- upcall iso stubs */

/* SEC-G08 M-08-2: the upcall core lives in the same translation unit as the
 * registry, so every symbol it touches has to exist here. These stubs are
 * plain recorders: the point of this test is that the *bookkeeping* is right
 * (busy cleared on every path, rc slot only valid after ret(), the caller cannot
 * fake a return), not that a real LoongArch task ever runs. */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_task_count;

static arkrtos_elf_info_t g_up_info;
static int g_up_info_rc;
static int g_up_running = 1;

int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out)
{
    (void)space;
    if ((g_up_info_rc != 0) || (p_out == NULL)) {
        return -1;
    }
    *p_out = g_up_info;
    return 0;
}

/* SEC-G08 M-08-4: the trampoline asks whether the space still has a live ELF
 * instance before it starts a worker ("image unloaded?" == no). */
int32 arkrtos_elf_running_in(uint32 space)
{
    (void)space;
    return g_up_running;
}

/* ... and the registration gate asks which space the caller is bound to. */
static uint32 g_me_space;

uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task)
{
    (void)p_task;
    return g_me_space;
}

static int g_up_stub_supported = 1;

int32 arkrtos_port_ushell_stub_supported(void)
{
    return g_up_stub_supported;
}

static int g_up_stub_rc;
static uint32 g_up_stub_space;
static unsigned long g_up_stub_va;

int32 arkrtos_port_ushell_stub(uint32 space, unsigned long stub_va)
{
    g_up_stub_space = space;
    g_up_stub_va = stub_va;
    return g_up_stub_rc;
}

static unsigned long g_up_frame;
static uint32 g_up_start_space;
static unsigned long g_up_start_stub_va;
static unsigned long g_up_start_sp;
static int g_up_start_argc;
static char **g_up_start_argv;
static int (*g_up_start_func)(int, char **);

unsigned long arkrtos_port_ushell_start(uint32 space, unsigned long stub_va,
                                        unsigned long upcall_sp,
                                        int (*func)(int, char **argv),
                                        int argc, char **argv)
{
    g_up_start_space = space;
    g_up_start_stub_va = stub_va;
    g_up_start_sp = upcall_sp;
    g_up_start_argv = argv;
    g_up_start_func = func;
    g_up_start_argc = argc;
    return g_up_frame;
}

/* SEC-G08 M-08-4: before the worker starts, the arguments are copied into the
 * App's own window -- a PLV3 callback cannot read kernel memory, and the
 * shell's argv points at kernel memory. The host has no window, so the words
 * are kept here and asserted on instead of being written anywhere. */
static unsigned char g_up_ws[ARKRTOS_USHELL_UP_ARGS_MAX];
static uint32 g_up_ws_bytes;
static unsigned long g_up_ws_va;
static uint32 g_up_ws_space;
static int g_up_ws_rc;

/* The window write goes through arkrtos_port_paging_write_space(), whose
 * element is one target word (8 bytes). On the host that is two 32-bit longs,
 * so the bytes are what matters: copy n * sizeof(long) of them and read the
 * 8-byte slots back by hand. */
int32 arkrtos_port_paging_write_space(uint32 space, unsigned long va,
                                      const unsigned long *words, uint32 n)
{
    uint32 bytes = n * (uint32)sizeof(unsigned long);

    g_up_ws_space = space;
    g_up_ws_va = va;
    g_up_ws_bytes = (bytes > (uint32)ARKRTOS_USHELL_UP_ARGS_MAX)
                    ? (uint32)ARKRTOS_USHELL_UP_ARGS_MAX : bytes;
    memcpy(g_up_ws, words, g_up_ws_bytes);
    return g_up_ws_rc;
}

static unsigned long long up_ws_u64(uint32 at)
{
    unsigned long long v = 0ull;
    uint32 i;

    for (i = 0u; i < 8u; i++) {
        v |= ((unsigned long long)g_up_ws[at + i]) << (i * 8u);
    }
    return v;
}

uint32 arkrtos_port_enter_critical(void)
{
    return 0u;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
}

static uint32 g_up_prev_space = 3u;
static uint32 g_up_act_space;
static int g_up_act_calls;

uint32 arkrtos_paging_space_current(void)
{
    return g_up_prev_space;
}

int32 arkrtos_paging_space_activate(uint32 space)
{
    g_up_act_space = space;
    g_up_act_calls++;
    return 0;
}

static arkrtos_task_t *g_up_bind_task;
static uint32 g_up_bind_space;

int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space)
{
    g_up_bind_task = p_task;
    g_up_bind_space = space;
    return 0;
}

static int32 g_up_reg_id = 1;
static unsigned long g_up_reg_entry;
static uint32 g_up_reg_bottom;
static uint32 g_up_reg_bytes;
static uint8 g_up_reg_prio;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    (void)period_ms;
    if (g_up_reg_id <= 0) {
        return g_up_reg_id;
    }
    g_up_reg_entry = (unsigned long)(uintptr_t)entry;
    g_up_reg_bottom = (uint32)(uintptr_t)stack_bottom;
    g_up_reg_bytes = stack_size;
    g_up_reg_prio = priority;
    arkrtos_task_table[g_up_reg_id - 1].status = ARKRTOS_TASK_READY;
    return g_up_reg_id;
}

static int g_up_ready_del_calls;
static int32 g_up_ready_del_idx;

void arkrtos_ready_del(int32 task_idx)
{
    g_up_ready_del_calls++;
    g_up_ready_del_idx = task_idx;
}

static int g_up_killed;

void arkrtos_task_kill_internal(void)
{
    g_up_killed++;
}

static int32 g_up_sem_next = 0x01300000;
static int g_up_sem_created;
static int g_up_sem_deleted;
static int g_up_sem_posted;
static int g_up_sem_wait_calls;
static int g_up_sem_wait_rc;
static int32 g_up_sem_wait_to;
static void (*g_up_wait_hook)(void);

int32 arkrtos_sem_create_internal(const char *name, int32 init_count)
{
    (void)name;
    (void)init_count;
    g_up_sem_created++;
    return g_up_sem_next;
}

int32 arkrtos_sem_wait_internal(int32 handle, int32 timeout_ms)
{
    (void)handle;
    g_up_sem_wait_calls++;
    g_up_sem_wait_to = timeout_ms;
    if (g_up_wait_hook != NULL) {
        g_up_wait_hook();
    }
    return g_up_sem_wait_rc;
}

int32 arkrtos_sem_post_internal(int32 handle)
{
    (void)handle;
    g_up_sem_posted++;
    return 0;
}

int32 arkrtos_sem_delete_internal(int32 handle)
{
    (void)handle;
    g_up_sem_deleted++;
    return 0;
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

/* ------------------------------------------------------------ fail-closed */

/* The privilege-separated + paging build must refuse App registrations: the
 * trampoline would otherwise run App code in PLV0 on the console task stack.
 * Kernel-side registration (is_priv != 0) keeps working, which is what the
 * board and drivers use.
 *
 * Guarded by the same switch as main(): the APP_CB=1 arm never calls it, and
 * an unguarded definition would trip -Wunused-function in warn_gate. */
#if (ARKRTOS_USE_USHELL_APP_CB == 0)
static void t_app_cb_refused(void)
{
    arkrtos_ushell_cmd_t c;

    reset_env();

    CK_I(ARKRTOS_USE_USHELL_APP_CB, 0);
    CK_I(ARKRTOS_USHELL_ERR_PRIV, -6);
    CK(ARKRTOS_USHELL_ERR_PRIV != ARKRTOS_USHELL_ERR_OFF);

    /* an App caller is refused before any registry row is touched */
    g_me.is_priv = 0u;
    arkrtos_current_task_id = 7;
    c = mk("appcmd", cb_ok, "help", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_PRIV);
    CK_I(g_cmd_count, 0);
    CK_I(g_ushell[0].in_use, 0u);
    CK_I(g_cmd_table[0].name == NULL, 1);

    /* a second attempt is still refused (no partial row, no dup) */
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_PRIV);
    CK_I(g_cmd_count, 0);

    /* the kernel task registers normally and the row works */
    g_me.is_priv = 1u;
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 1);
    CK_I(g_ushell[0].in_use, 1u);
    CK_S(g_ushell[0].name, "appcmd");
    CK_I((long)g_ushell[0].owner_task, 7);

    /* and unregistration still shifts the row out */
    CK_I(arkrtos_shell_ext_unregister("appcmd"), ARKRTOS_USHELL_OK);
    CK_I(g_cmd_count, 0);
    CK_I(g_ushell[0].in_use, 0u);
}

/* SEC-G08 M-08-4: the same build no longer refuses every App registration --
 * it refuses the ones whose space cannot host an isolated call.  Every branch
 * here is the gate itself, so a regression that opens it unconditionally is
 * caught by the first three checks and a regression that closes it again is
 * caught by the fourth. */
static void t_app_cb_isolated(void)
{
    arkrtos_ushell_cmd_t c;

    reset_env();
    memset(&g_up_info, 0, sizeof(g_up_info));
    g_up_info_rc = 0;
    g_up_stub_supported = 1;
    g_up_running = 1;
    g_me.is_priv = 0u;
    /* Space 1, not 2: ARKRTOS_USHELL_UP_MAX_SPACES follows the board's
     * ARKRTOS_PAGING_APPS_MAX (= 2), so 0..1 are the real spaces. That also
     * exercises the range half of the gate below. */
    g_me_space = 1u;
    arkrtos_current_task_id = 9;

    /* (a) the caller's space has no upcall area: still refused, row untouched */
    c = mk("appcmd", cb_ok, "help", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_PRIV);
    CK_I(g_cmd_count, 0);
    CK_I((int)g_ushell[0].in_use, 0);
    CK_I((long)g_ushell[0].has_space, 0);

    /* (b) that space can host an isolated call: the very same call passes,
     *     and the row remembers which space (not "whatever is current later") */
    g_up_info.upcall_base = 0x02100000ul;
    g_up_info.stub_va = 0x02100000ul;
    g_up_info.upcall_sp = 0x02101FF0ul;
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I((int)g_ushell[0].in_use, 1);
    CK_I((long)g_ushell[0].space, 1);
    CK_I((long)g_ushell[0].has_space, 1);
    CK_I((long)g_ushell[0].owner_task, 9);
    CK_I(g_cmd_count, 1);

    /* (c) a port that cannot write the stub: the area alone is not enough */
    CK_I(arkrtos_shell_ext_unregister("appcmd"), ARKRTOS_USHELL_OK);
    g_up_stub_supported = 0;
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_ERR_PRIV);
    CK_I(g_cmd_count, 0);
    g_up_stub_supported = 1;

    /* (d) a kernel task is never isolated: its handler is kernel code already */
    g_me.is_priv = 1u;
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I((long)g_ushell[0].has_space, 0);
    CK_I((long)g_ushell[0].space, 0);
    CK_I((long)g_ushell[0].owner_task, 9);
}
#endif /* ARKRTOS_USE_USHELL_APP_CB == 0 */

/* ------------------------------------------------------------ upcall iso */

#if (ARKRTOS_USE_USHELL_APP_CB == 1)

/* Geometry the fake ELF ledger reports: the default 8 KB upcall region =
 * one 4 KB stub page plus a 4 KB stack, top word reserved for the stub's
 * entry convention. The values are arbitrary but internally consistent. */
#define UP_BASE   (0x02100000ul)
#define UP_SP     (UP_BASE + 8192ul - 16ul)
#define UP_FRAME  (UP_SP - 0x200ul)

static int32 g_up_nested_rc;
static int32 g_up_spoof_rc;

static void up_reset(void)
{
    memset(&g_up_info, 0, sizeof(g_up_info));
    g_up_info_rc = 0;
    g_up_running = 1;
    g_up_stub_supported = 1;
    g_up_stub_rc = 0;
    g_up_stub_space = 0xFFFFFFFFu;
    g_up_stub_va = 0ul;
    g_up_frame = UP_FRAME;
    g_up_start_space = 0xFFu;
    g_up_start_stub_va = 0ul;
    g_up_start_sp = 0ul;
    g_up_start_argc = -1;
    g_up_start_func = NULL;
    g_up_prev_space = 3u;
    g_up_act_space = 0xFFFFFFFFu;
    g_up_act_calls = 0;
    g_up_bind_task = NULL;
    g_up_bind_space = 0xFFFFFFFFu;
    g_up_start_argv = NULL;
    g_up_ws_bytes = 0u;
    g_up_ws_va = 0ul;
    g_up_ws_space = 0xFFFFFFFFu;
    g_up_ws_rc = 0;
    memset(g_up_ws, 0, sizeof(g_up_ws));
    g_up_reg_id = 1;
    g_up_reg_entry = 0ul;
    g_up_reg_bottom = 0u;
    g_up_reg_bytes = 0u;
    g_up_reg_prio = 0u;
    g_up_ready_del_calls = 0;
    g_up_ready_del_idx = -1;
    g_up_killed = 0;
    g_up_sem_next = 0x01300000;
    g_up_sem_created = 0;
    g_up_sem_deleted = 0;
    g_up_sem_posted = 0;
    g_up_sem_wait_calls = 0;
    g_up_sem_wait_rc = 0;
    g_up_sem_wait_to = 0;
    g_up_wait_hook = NULL;
    g_up_nested_rc = 0;
    g_up_spoof_rc = 0;
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_count = 1;
    arkrtos_current_task_id = 5;
}

static int up_cb(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 0;
}

/* Normal return: the worker reports 77 and falls off into kill_internal(). */
static void up_hook_ret(void)
{
    arkrtos_current_task_id = g_up_reg_id;
    (void)arkrtos_ushell_upcall_ret(77, 3u);
    arkrtos_current_task_id = 5;
}

/* Nested call from inside the first one: the second must be refused BUSY,
 * then the first still returns normally. */
static void up_hook_busy(void)
{
    int32 r = 0;

    arkrtos_current_task_id = g_up_reg_id;
    g_up_nested_rc = arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &r);
    (void)arkrtos_ushell_upcall_ret(11, 3u);
    arkrtos_current_task_id = 5;
}

/* Impersonation: another task on the same space tries to report a return
 * while the real worker is still running. It must be refused, and the
 * refusal must not disturb the ledger. */
static void up_hook_spoof(void)
{
    int32 save = arkrtos_current_task_id;

    arkrtos_current_task_id = 3;
    g_up_spoof_rc = arkrtos_ushell_upcall_ret(99, 3u);

    arkrtos_current_task_id = g_up_reg_id;
    (void)arkrtos_ushell_upcall_ret(77, 3u);
    arkrtos_current_task_id = save;
}

static void t_upcall_iso(void)
{
    int32 rc = 0;
    char *argv[2];
    char up_argv0[] = "hi";
    char up_argv1[] = "b";
    arkrtos_task_t *p_t;

    reset_env();
    up_reset();

    /* ---- ready(): each prerequisite has its own readout ---- */
    CK_I(arkrtos_ushell_upcall_ready(0u), 0);          /* never loaded */
    g_up_info_rc = -1;
    CK_I(arkrtos_ushell_upcall_ready(0u), 0);
    g_up_info_rc = 0;                                  /* loaded, no upcall area */
    CK_I(arkrtos_ushell_upcall_ready(0u), 0);
    g_up_info.upcall_base = UP_BASE;
    g_up_info.stub_va = UP_BASE;
    g_up_info.upcall_sp = UP_SP;
    CK_I(arkrtos_ushell_upcall_ready(0u), 1);
    g_up_stub_supported = 0;                           /* port cannot write a stub */
    CK_I(arkrtos_ushell_upcall_ready(0u), 0);
    g_up_stub_supported = 1;
    CK_I(arkrtos_ushell_upcall_ready(ARKRTOS_USHELL_UP_MAX_SPACES), 0);
    /* ready() does not care about busy: registration during a running call
     * must not be refused (only call() has to refuse). */
    CK_I(arkrtos_ushell_upcall_ready(0u), 1);

    /* ---- call(): parameter gate ---- */
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, NULL),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, NULL, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, -1, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 1, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(arkrtos_ushell_upcall_call(0u, 0u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(arkrtos_ushell_upcall_call(0u, (uint32)ARKRTOS_TASK_MAX_NUM + 1u,
                                    up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(arkrtos_ushell_upcall_call(ARKRTOS_USHELL_UP_MAX_SPACES, 1u, up_cb,
                                    0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    CK_I(g_up_sem_created, 0);
    CK_I(g_up_reg_entry, 0ul);

    /* ---- no area / no stub ---- */
    g_up_info.upcall_base = 0ul;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_NOAREA);
    g_up_info.upcall_base = UP_BASE;
    g_up_stub_supported = 0;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_NOAREA);
    g_up_stub_supported = 1;

    /* ---- geometry: the stack must exist above the stub page ---- */
    g_up_info.upcall_sp = UP_BASE + 4000ul;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_NOAREA);
    g_up_info.upcall_sp = UP_SP;
    CK_I(g_up_sem_created, 0);

    /* ---- the argument copy: every refusal happens before anything is
     *      allocated, so no half-built worker is left behind ---- */
    argv[0] = up_argv0;
    argv[1] = up_argv1;
    g_up_ws_rc = -1;                        /* the window write was refused */
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 2, argv, &rc),
         ARKRTOS_USHELL_UP_ERR_EXEC);
    CK_I(g_up_sem_created, 0);
    CK_I(g_up_reg_entry, 0ul);
    g_up_ws_rc = 0;
    /* a NULL element inside argc is a caller bug, not an empty string */
    argv[1] = NULL;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 2, argv, &rc),
         ARKRTOS_USHELL_UP_ERR_PARAM);
    argv[1] = up_argv1;
    /* one argument longer than the whole argument area: refused, not truncated */
    {
        static char big[ARKRTOS_USHELL_UP_ARGS_MAX + 8u];
        char *bigv[2];

        memset(big, 'x', sizeof(big) - 1u);
        big[sizeof(big) - 1u] = '\0';
        bigv[0] = big;
        bigv[1] = NULL;
        CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 1, bigv, &rc),
             ARKRTOS_USHELL_UP_ERR_PARAM);
    }
    /* every string fits on its own, but the pointer array alone does not:
     * 60 x "hi" = 480 bytes of strings + 61 x 8 bytes of pointers > 512.
     * The per-string bound cannot see this one; only the total can. */
    {
        static char *many[60];
        int k;

        for (k = 0; k < 60; k++) {
            many[k] = up_argv0;
        }
        CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 60, many, &rc),
             ARKRTOS_USHELL_UP_ERR_PARAM);
    }
    /* a stack that exists (call()'s 64-byte floor is met) but has no room for
     * the argument block plus a real call frame: NOAREA, and nothing is
     * allocated -- a half-built worker is worse than a refusal. */
    g_up_info.upcall_sp = UP_BASE + (unsigned long)ARKRTOS_USHELL_UP_PAGE_SIZE + 64ul;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 1, argv, &rc),
         ARKRTOS_USHELL_UP_ERR_NOAREA);
    g_up_info.upcall_sp = UP_SP;
    CK_I(g_up_sem_created, 0);

    /* ---- semaphore refused ---- */
    g_up_sem_next = -1;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_TASK);
    g_up_sem_next = 0x01300000;

    /* ---- task table full: nothing else was touched ---- */
    g_up_reg_id = 0;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_TASK);
    CK_I(g_up_sem_deleted, 1);
    CK_I(g_up_ready_del_calls, 0);
    g_up_reg_id = 1;

    /* ---- stub page refused: the half-built task must be dropped ---- */
    g_up_stub_rc = -1;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_EXEC);
    CK_I(g_up_ready_del_calls, 1);
    CK_I(g_up_ready_del_idx, 0);
    CK_I((int)arkrtos_task_table[0].status, (int)ARKRTOS_TASK_INVALID);
    CK_I(g_up_sem_deleted, 2);
    g_up_stub_rc = 0;

    /* ---- initial frame refused: same cleanup path ---- */
    g_up_frame = 0ul;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_EXEC);
    CK_I(g_up_ready_del_calls, 2);
    CK_I((int)arkrtos_task_table[0].status, (int)ARKRTOS_TASK_INVALID);
    g_up_frame = UP_FRAME;

    /* ---- success: rc comes from ret(), and the task is really configured ---- */
    argv[0] = up_argv0;
    argv[1] = up_argv1;
    g_up_wait_hook = up_hook_ret;
    g_up_sem_wait_rc = 0;
    arkrtos_current_task_id = 5;
    rc = -123;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 2, argv, &rc),
         ARKRTOS_USHELL_UP_OK);
    CK_I(rc, 77);
    CK_I(g_up_killed, 1);                    /* ret() took the worker down */
    CK_I(g_up_sem_posted, 1);
    CK_I(g_up_sem_wait_to, (int32)ARKRTOS_USHELL_UP_TIMEOUT_MS);
    CK_I(g_up_stub_space, 0u);
    CK(g_up_stub_va == UP_BASE);
    CK_I(g_up_start_space, 0);
    CK(g_up_start_stub_va == UP_BASE);
    /* the copied arguments live at the top of the upcall stack, the worker's
     * sp sits right below them, and start() got the window-side array */
    CK(g_up_start_sp < UP_SP);
    CK(g_up_start_sp >= (UP_BASE + (unsigned long)ARKRTOS_USHELL_UP_PAGE_SIZE));
    CK_I(g_up_ws_space, 0);
    CK(g_up_ws_va == g_up_start_sp);
    CK_I((int)g_up_ws_bytes, 40);           /* "hi" + "b" (8 each) + 3 ptrs */
    CK_I((int)g_up_ws[0], 'h');
    CK_I((int)g_up_ws[1], 'i');
    CK_I((int)g_up_ws[2], 0);
    CK_I((int)g_up_ws[8], 'b');             /* second string, 8-byte slot */
    CK_I((int)g_up_ws[9], 0);
    /* the three pointers are 8-byte little-endian slots right after the
     * strings: argv[0] -> "hi", argv[1] -> "b", argv[2] == NULL */
    CK(up_ws_u64(16u) == (unsigned long long)g_up_start_sp);
    CK(up_ws_u64(24u) == (unsigned long long)(g_up_start_sp + 8ul));
    CK(up_ws_u64(32u) == 0ull);
    CK(g_up_start_argv == (char **)(uintptr_t)(g_up_start_sp + 16ul));
    CK(g_up_start_func == up_cb);
    CK_I(g_up_start_argc, 2);
    CK(g_up_reg_entry == UP_BASE);           /* the entry IS the stub page */
    CK(g_up_reg_bottom == (uint32)(uintptr_t)(UP_BASE + (unsigned long)ARKRTOS_USHELL_UP_PAGE_SIZE));
    /* The registered stack stops at the worker's sp, NOT at the top of the
     * upcall stack: task_register() builds a trap frame at "bottom + size"
     * (and fills the whole range when stack-usage accounting is on), so a
     * top of UP_SP would paint over the marshaled arguments. Losing this
     * shows up on target as "argc/argv pointers fine, argv[i] all zero". */
    CK_I((int)g_up_reg_bytes,
         (int)(g_up_start_sp - (UP_BASE + (unsigned long)ARKRTOS_USHELL_UP_PAGE_SIZE)));
    CK_I((int)g_up_reg_prio, (int)ARKRTOS_USHELL_UP_TASK_PRIORITY);
    CK(g_up_bind_task == &arkrtos_task_table[0]);
    CK_I(g_up_bind_space, 0);
    p_t = &arkrtos_task_table[0];
    CK_I((int)p_t->is_priv, 0);              /* the whole point: PLV3 */
    CK(p_t->rom_start == (unsigned long)ARKRTOS_ELF_WINDOW_BASE);
    CK_I((int)p_t->rom_size, (int)ARKRTOS_ELF_WINDOW_SIZE);
    CK(p_t->ram_start == (unsigned long)ARKRTOS_ELF_WINDOW_BASE);
    CK(p_t->stack_top == g_up_start_sp);
    CK(p_t->stack_ptr == UP_FRAME);
    CK_I(g_up_act_space, 3u);                /* console task's space restored */
    CK_I(g_up_sem_deleted, 4);               /* one per call that got that far */

    /* ---- busy: a second call in the same space is refused, first survives ---- */
    g_up_wait_hook = up_hook_busy;
    rc = 0;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_OK);
    CK_I(g_up_nested_rc, ARKRTOS_USHELL_UP_ERR_BUSY);
    CK_I(rc, 11);

    /* ---- impersonation inside a live call is refused and harmless ---- */
    g_up_wait_hook = up_hook_spoof;
    rc = 0;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_OK);
    CK_I(g_up_spoof_rc, ARKRTOS_USHELL_UP_ERR_CALLER);
    CK_I(rc, 77);

    /* ---- timeout: reported as such, busy cleared for the next call ---- */
    g_up_wait_hook = NULL;
    g_up_sem_wait_rc = ARKRTOS_SYNC_ERR_TIMEOUT;
    rc = 5;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_TIMEOUT);
    CK_I(rc, 0);

    /* a wait failure that is not a timeout is EXEC, not TIMEOUT */
    g_up_sem_wait_rc = -1;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_ERR_EXEC);
    g_up_sem_wait_rc = 0;

    /* the timeouts cleared busy: a later call still starts a worker */
    g_up_wait_hook = up_hook_ret;
    rc = 0;
    CK_I(arkrtos_ushell_upcall_call(0u, 1u, up_cb, 0, NULL, &rc),
         ARKRTOS_USHELL_UP_OK);
    CK_I(rc, 77);
    g_up_wait_hook = NULL;

    /* ---- ret() outside any call: nobody to report to ---- */
    arkrtos_current_task_id = 0;
    CK_I(arkrtos_ushell_upcall_ret(1, 3u), ARKRTOS_USHELL_UP_ERR_CALLER);
    arkrtos_current_task_id = 3;
    CK_I(arkrtos_ushell_upcall_ret(1, 3u), ARKRTOS_USHELL_UP_ERR_CALLER);
    arkrtos_current_task_id = 5;
}

/* SEC-G08 M-08-4: the trampoline's isolated half. The handler pointer is a
 * genuine window VA -- the fixture never calls it (call() only hands it to the
 * port stub recorder), so "is this handler inside the App's executable region"
 * is decided by the value, not by where this test binary happens to be loaded.
 * Everything below therefore reads the *bookkeeping and the log lines* of the
 * isolated path, which is exactly what a wrong implementation gets wrong. */
#define ISO_FUNC_VA \
    ((int (*)(int, char **))(uintptr_t)((unsigned long)ARKRTOS_ELF_WINDOW_BASE + 0x1000ul))

static int (*g_tr)(int, char **);
static int g_reenter_rc;
static uint32 g_up_flag_seen = 7u;

/* Re-entrant dispatch from inside the wait: busy[0] is already 1, so a second
 * isolated dispatch of the same space must answer "busy" instead of starting a
 * second worker. This is the only way the BUSY arm is reachable single-threaded
 * -- and it is the same interleaving a second console task would produce. */
static void up_hook_reenter(void)
{
    char *av[1];
    char nm[] = "iso";

    av[0] = nm;
    g_reenter_rc = call_tr(g_tr, 1, av);
    arkrtos_current_task_id = g_up_reg_id;
    (void)arkrtos_ushell_upcall_ret(5, 3u);
    arkrtos_current_task_id = 5;
}

static void up_hook_ret_flag(void)
{
    g_up_flag_seen = arkrtos_kernel_ushell_active;
    arkrtos_current_task_id = g_up_reg_id;
    (void)arkrtos_ushell_upcall_ret(77, 3u);
    arkrtos_current_task_id = 5;
}

static void t_trampoline_isolated(void)
{
    arkrtos_ushell_cmd_t c;
    char *argv[2];
    char nm_iso[] = "iso";
    char nm_other[] = "other";
    char nm_arg[] = "a";
    char kname[8];   /* 可写缓冲：argv 是 char*，不许从字符串常量强转 */

    reset_env();
    up_reset();
    /* a live image in space 0 with an upcall area, and a row that says
     * "this one runs isolated there" */
    g_up_info.upcall_base = UP_BASE;
    g_up_info.stub_va = UP_BASE;
    g_up_info.upcall_sp = UP_SP;
    g_up_info.task = 1;
    g_up_running = 1;
    c = mk("iso", ISO_FUNC_VA, "h", 2u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    g_ushell[0].space = 0u;
    g_ushell[0].has_space = 1u;
    g_ushell[0].owner_task = 1u;
    g_tr = table_fn();
    argv[0] = nm_iso;
    argv[1] = nm_arg;

    /* (a) success: the rc comes back, the log line carries this run's evidence,
     *     and the kernel-side active flag is NOT raised on the isolated path */
    g_up_wait_hook = up_hook_ret_flag;
    g_up_sem_wait_rc = 0;
    g_up_flag_seen = 7u;
    arkrtos_kernel_ushell_active = 0u;
    cap_reset();
    CK_I(call_tr(g_tr, 2, argv), 77);
    CK_I((long)g_up_flag_seen, 0);
    CK_I((long)arkrtos_kernel_ushell_active, 0);
    CK(g_up_start_func == ISO_FUNC_VA);
    CK_I(g_up_start_space, 0);
    CK_I(g_up_start_argc, 2);
    CK(out_has("[ushell] isolated rc=77 tid=1 space=0 plv=3"));

    /* (b) the image is gone: refuse, do not fall back to a kernel-stack call */
    g_up_running = 0;
    g_up_wait_hook = up_hook_ret_flag;
    cb_called = 0;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("handler no longer available"));
    CK_I(cb_called, 0);
    g_up_running = 1;

    /* (c) no upcall area any more (reloaded with UPCALL_SIZE=0) */
    g_up_info.upcall_base = 0ul;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("handler no longer available"));
    g_up_info.upcall_base = UP_BASE;

    /* (d) the handler is not inside this space's executable region */
    g_ushell[0].func = cb_ok;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("handler no longer available"));
    CK_I(cb_called, 0);
    g_ushell[0].func = ISO_FUNC_VA;

    /* (d2) the row's space is out of range (registry/ledger disagreement):
     * refuse without even asking the ELF ledger -- an out-of-range index
     * must never reach a table lookup. */
    g_ushell[0].space = (uint32)ARKRTOS_USHELL_UP_MAX_SPACES;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("handler no longer available"));
    CK_I(cb_called, 0);
    g_ushell[0].space = 0u;

    /* (d3) the ELF ledger cannot be read: "cannot prove it is alive" wins
     * over "the address looks fine" -- never jump on a guess. */
    g_up_info_rc = -1;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("handler no longer available"));
    CK_I(cb_called, 0);
    g_up_info_rc = 0;

    /* (e) a second dispatch while one is running is refused with its own line */
    g_up_wait_hook = up_hook_reenter;
    g_reenter_rc = 0;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), 5);          /* the outer worker still reports */
    CK_I(g_reenter_rc, -1);
    CK(out_has("busy (another upcall in this space)"));

    /* (f) the worker cannot be created: -4, its own line, and busy is cleared
     *     (a later dispatch still starts a worker -- checked right after) */
    g_up_wait_hook = up_hook_ret_flag;
    g_up_reg_id = 0;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("cannot start isolated worker (-4)"));
    g_up_reg_id = 1;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), 77);

    /* (g) the stub page / initial frame could not be written: -5 */
    g_up_stub_rc = -1;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("cannot start isolated worker (-5)"));
    g_up_stub_rc = 0;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), 77);

    /* (h) the callback never came back: its own line, not "cannot start" */
    g_up_wait_hook = NULL;                   /* nobody reports a return */
    g_up_sem_wait_rc = ARKRTOS_SYNC_ERR_TIMEOUT;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("isolated worker timed out after 10000 ms"));
    g_up_sem_wait_rc = 0;
    g_up_wait_hook = up_hook_ret_flag;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), 77);

    /* (h2) a row whose owner_task is gone (0 = kernel-side registration, the
     * one shape call() refuses with PARAM): the trampoline still answers
     * "not available" instead of falling back to a kernel-context call. This
     * is the default arm -- registration gates every other refusal already,
     * so a state change after registration is the only way in. */
    g_ushell[0].owner_task = 0u;
    cb_called = 0;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("handler no longer available"));
    CK_I(cb_called, 0);
    g_ushell[0].owner_task = 1u;

    /* (i) an unmatched name still answers the console-level message */
    argv[0] = nm_other;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), -1);
    CK(out_has("user command not available"));
    argv[0] = nm_iso;

    /* (j) a kernel-side row is untouched by all of this: direct call, flag
     *     raised for its duration (that is what §4.6 keeps) */
    reset_env();
    up_reset();
    g_me.is_priv = 1u;
    g_pt.slot_max = 2u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_state[0] = ARKRTOS_APP_SLOT_LOADED;
    g_pt.slot_base[0] = addr_of(cb_ok) - 0x10u;
    g_pt.slot_size[0] = 0x100u;
    kname[0] = 'k'; kname[1] = 'c'; kname[2] = 'm'; kname[3] = 'd';
    kname[4] = '\0';
    c = mk("kcmd", cb_ok, "h", 1u);
    CK_I(arkrtos_shell_ext_register(&c), ARKRTOS_USHELL_OK);
    CK_I((long)g_ushell[0].has_space, 0);
    g_tr = table_fn();
    argv[0] = kname;
    cb_called = 0;
    cb_active_seen = 7u;
    cap_reset();
    CK_I(call_tr(g_tr, 1, argv), 42);
    CK_I(cb_called, 1);
    CK_I((long)cb_active_seen, 1);
    CK_I(g_out_len, 0u);                     /* no isolated log line either */
}

#endif /* ARKRTOS_USE_USHELL_APP_CB == 1 */

/* ------------------------------------------------------------ main */

int main(void)
{
#if (ARKRTOS_USE_USHELL_APP_CB == 0)
    /* fail-closed variant: only the policy is under test here, the full
     * registry behaviour is covered by the ARKRTOS_USE_USHELL_APP_CB=1
     * build of this same file. */
    t_app_cb_refused();
    t_app_cb_isolated();

    if (g_failed != 0) {
        printf("USHELL-HOST REFUSED FAIL  %d/%d checks failed\n",
               g_failed, g_checks);
        return 1;
    }
    printf("USHELL-HOST REFUSED  %d checks\n", g_checks);
    return 0;
#else
    t_name_rules();
    t_copy_bounds();
    t_register_refusals();
    t_register_success();
    t_unregister_shift();
    t_trampoline_lookup();
    t_trampoline_alive();
    t_print_n();
    t_upcall_iso();
    t_trampoline_isolated();

    if (g_failed != 0) {
        printf("USHELL-HOST FAIL  %d/%d checks failed\n", g_failed, g_checks);
        return 1;
    }
    printf("USHELL-HOST OK  %d checks\n", g_checks);
    return 0;
#endif
}
