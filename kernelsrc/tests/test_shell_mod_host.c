/**
* @file test_shell_mod_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_mod.c.
*
* What this proves: the two execution models behind exec / wait / ld / lkup /
* modules / unld are reported the way the source documents them -- the exec
* path prints the loader's own reading (never a guessed one) and gives the
* prompt back at the 30 s cap instead of killing the program; the wait path
* refuses every malformed argument before it touches the task table; the
* module path points at the missing symbol or the bad relocation number
* instead of a bare rc; and the list/lookup/unload commands print the fields
* of the fixture below rather than a second copy of the module ledger.
*
* What it does NOT prove:
*   - that a real ELF64 image loads: arkrtos_elf_exec_space / info_space /
*     running_in are stubs here, so nothing is parsed, mapped or scheduled;
*   - that the 30 s cap is 30 s of wall time -- arkrtos_task_wait_internal is
*     a counter stub, so this only proves the loop yields and stops after
*     30000/20 polls;
*   - that a real module can be loaded, relocated or unloaded: the module
*     loader and the export table are stubs with conventional return codes;
*   - that the shell task's own stack survives a real load (the 3080 B peak
*     noted in arkrtos_modld.h) -- no stack is used here beyond the host's.
*
* The fixture values are arbitrary but distinctive, so a command that prints
* the wrong field is caught rather than passing by coincidence.
*
* @author xw
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

static char g_out[16384];
static size_t g_out_len;

static void cap_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0u;
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

/* Writable argv strings. -Wwrite-strings makes the literal itself const, and
 * casting that away is exactly what -Wcast-qual is there to catch; copying
 * into a rotating buffer keeps both switches quiet without hiding a cast. */
static char *W(const char *s)
{
    static char buf[8][48];
    static unsigned next;
    char *p = buf[next % 8u];

    next++;
    strncpy(p, s, 47u);
    p[47] = '\0';
    return p;
}

static unsigned out_count(const char *needle)
{
    const char *p = g_out;
    unsigned n = 0u;
    size_t len = strlen(needle);

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += len;
    }
    return n;
}

/* ------------------------------------------------------------ stubs */

/* ELF side: one "last exec" record and one "space info" record, kept apart
 * so a command that reads the wrong one is caught. */
static int32 g_exec_rc;
static uint32 g_exec_space;
static int g_exec_calls;
static char g_exec_path[64];
static int32 g_exec_argc;

static int32 g_info_rc;
static arkrtos_elf_info_t g_info;
static uint32 g_info_space;

/* running_in: either always busy, or "left" more polls, or idle. */
static int g_run_always;
static unsigned g_run_left;
static unsigned g_run_calls;
static uint32 g_run_space;

/* Task side. */
static int32 g_exit_code_rc;
static int32 g_exit_code;
static int32 g_exit_code_seen;
static unsigned g_wait_calls;
static unsigned g_wait_flip_at;   /* flip target task to INVALID at this poll */
static uint32 g_wait_ms;

/* Module side. */
static int32 g_mod_load_rc;
static int32 g_mod_count_before;
static int32 g_mod_count_after;
static int32 g_mod_info_rc;
static arkrtos_mod_info_t g_mod_info;
static int32 g_mod_info_index;
static int32 g_mod_unload_rc;
static int32 g_mod_unload_index;
static char g_mod_missing[32];
static uint32 g_mod_bad_reloc;

/* Export side. */
static int32 g_exp_lookup_rc;
static unsigned long g_exp_addr;
static const arkrtos_export_t *g_exp_tab;
static uint32 g_exp_n;

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_current_task_id;

int32 arkrtos_elf_exec_space(const char *path, int32 argc, char *argv[],
                             uint32 *p_space)
{
    (void)argv;
    g_exec_calls++;
    g_exec_argc = argc;
    g_exec_path[0] = '\0';
    if (path != NULL) {
        strncpy(g_exec_path, path, sizeof(g_exec_path) - 1u);
        g_exec_path[sizeof(g_exec_path) - 1u] = '\0';
    }
    if (g_exec_rc == ARKRTOS_ELF_OK) {
        if (p_space != NULL) {
            *p_space = g_exec_space;
        }
    }
    return g_exec_rc;
}

int32 arkrtos_elf_info_space(uint32 space, arkrtos_elf_info_t *p_out)
{
    g_info_space = space;
    if (g_info_rc != 0) {
        return g_info_rc;
    }
    if (p_out != NULL) {
        *p_out = g_info;
    }
    return 0;
}

int32 arkrtos_elf_running_in(uint32 space)
{
    g_run_calls++;
    g_run_space = space;
    if (g_run_always != 0) {
        return 1;
    }
    if (g_run_left > 0u) {
        g_run_left--;
        return 1;
    }
    return 0;
}

const char *arkrtos_elf_error_name(int32 rc)
{
    switch (rc) {
    case ARKRTOS_ELF_ERR_FS:
        return "vfs read failed";
    case ARKRTOS_ELF_ERR_BUSY:
        return "no free address space";
    default:
        return "elf error";
    }
}

void arkrtos_task_wait_internal(uint32 ms)
{
    g_wait_calls++;
    g_wait_ms = ms;
    if ((g_wait_flip_at != 0u) && (g_wait_calls >= g_wait_flip_at)) {
        uint32 i;

        for (i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++) {
            arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
        }
    }
}

int32 arkrtos_task_exit_code(int32 task_id, int32 *p_code)
{
    g_exit_code_seen = task_id;
    if (p_code != NULL) {
        *p_code = g_exit_code;
    }
    return g_exit_code_rc;
}

const char *task_state_name(uint32 st)
{
    return (st == (uint32)ARKRTOS_TASK_INVALID) ? "STUB-INVALID" : "STUB-READY";
}

int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if ((s == NULL) || (s[0] == '\0')) {
        return -1;
    }
    while (*s != '\0') {
        if ((*s < '0') || (*s > '9')) {
            return -1;
        }
        v = (v * 10u) + (uint32)(*s - '0');
        s++;
    }
    if (out != NULL) {
        *out = v;
    }
    return 0;
}

void sh_out(const char *s)
{
    cap_add(s, strlen(s));
}

void ark_shell_printf(const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        cap_add(tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1u);
    }
}

int32 arkrtos_mod_load(const char *path, int32 argc, char *argv[])
{
    (void)path;
    (void)argc;
    (void)argv;
    return g_mod_load_rc;
}

int32 arkrtos_mod_count(void)
{
    int32 n = g_mod_count_before;

    g_mod_count_before = g_mod_count_after;
    return n;
}

int32 arkrtos_mod_info(int32 index, arkrtos_mod_info_t *p_out)
{
    g_mod_info_index = index;
    if (g_mod_info_rc != 0) {
        return g_mod_info_rc;
    }
    if (p_out != NULL) {
        *p_out = g_mod_info;
    }
    return 0;
}

int32 arkrtos_mod_unload(int32 index)
{
    g_mod_unload_index = index;
    return g_mod_unload_rc;
}

const char *arkrtos_mod_error_name(int32 rc)
{
    switch (rc) {
    case ARKRTOS_MOD_ERR_FS:
        return "vfs read failed";
    case ARKRTOS_MOD_ERR_SYMBOL:
        return "unknown symbol";
    case ARKRTOS_MOD_ERR_RELOC:
        return "unsupported relocation";
    case ARKRTOS_MOD_ERR_BUSY:
        return "module still running";
    default:
        return "module error";
    }
}

const char *arkrtos_mod_missing_symbol(void)
{
    return g_mod_missing;
}

uint32 arkrtos_mod_bad_reloc(void)
{
    return g_mod_bad_reloc;
}

int32 arkrtos_export_lookup(const char *name, unsigned long *p_addr)
{
    (void)name;
    if (g_exp_lookup_rc != 0) {
        return g_exp_lookup_rc;
    }
    if (p_addr != NULL) {
        *p_addr = g_exp_addr;
    }
    return 0;
}

const arkrtos_export_t *arkrtos_export_table(uint32 *p_count)
{
    if (p_count != NULL) {
        *p_count = g_exp_n;
    }
    return g_exp_tab;
}

/* ------------------------------------------------------------ helpers */

static void reset_env(void)
{
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));

    g_exec_rc = ARKRTOS_ELF_OK;
    g_exec_space = 7u;
    g_exec_calls = 0;
    g_exec_path[0] = '\0';
    g_exec_argc = 0;
    g_info_rc = 0;
    memset(&g_info, 0, sizeof(g_info));
    g_info_space = 0u;

    g_run_always = 0;
    g_run_left = 0u;
    g_run_calls = 0u;
    g_run_space = 0u;

    g_exit_code_rc = 0;
    g_exit_code = 0;
    g_exit_code_seen = 0;
    g_wait_calls = 0u;
    g_wait_flip_at = 0u;
    g_wait_ms = 0u;

    g_mod_load_rc = ARKRTOS_MOD_OK;
    g_mod_count_before = 0;
    g_mod_count_after = 0;
    g_mod_info_rc = 0;
    memset(&g_mod_info, 0, sizeof(g_mod_info));
    g_mod_info_index = -1;
    g_mod_unload_rc = ARKRTOS_MOD_OK;
    g_mod_unload_index = -1;
    g_mod_missing[0] = '\0';
    g_mod_bad_reloc = 0u;

    g_exp_lookup_rc = 0;
    g_exp_addr = 0ul;
    g_exp_tab = NULL;
    g_exp_n = 0u;

    arkrtos_current_task_id = 1;
    cap_reset();
}

/* ------------------------------------------------------------ exec */

static void t_exec_usage_and_load_errors(void)
{
    char *argv[2];

    reset_env();
    CK_I(cmd_exec(0, NULL), 1);
    CK(out_has("usage: exec <path> [args...]"));

    reset_env();
    g_exec_rc = ARKRTOS_ELF_ERR_FS;
    argv[0] = W("exec");       /* never read: usage path took argv[1] */
    argv[1] = W("hi");
    CK_I(cmd_exec(2, argv), 1);
    CK(out_has("hi: vfs read failed (rc=-2)"));
    CK_I(g_exec_calls, 1);
    CK_S(g_exec_path, "hi");
    CK_I(g_exec_argc, 1);

    /* BUSY keeps the loader's wording, not a shell-side one */
    reset_env();
    g_exec_rc = ARKRTOS_ELF_ERR_BUSY;
    CK_I(cmd_exec(2, argv), 1);
    CK(out_has("hi: no free address space (rc=-7)"));
}

static void t_exec_readings(void)
{
    char *argv[2];

    reset_env();
    argv[0] = W("exec");
    argv[1] = W("prog");
    g_info.bytes = 1234u;
    g_info.segs = 2u;
    g_info.entry = 0x12345678ul;
    g_info.sp = 0x12349000ul;
    g_info.argc = 3u;
    g_info.task = 5;
    g_info.exit_code_set = 1u;
    g_info.exit_code = -9;
    g_run_left = 2u;

    CK_I(cmd_exec(2, argv), 0);
    CK(out_has("[ELF] prog: 1234 B, 2 segment(s)"));
    CK(out_has("entry 0x0000000012345678"));
    CK(out_has("sp 0x0000000012349000"));
    CK(out_has("argc 3, task 5, space 7"));
    CK(out_has("finished after 40 ms (space 7, exit code -9)"));
    CK_I(g_run_space, 7u);
    CK_I(g_wait_calls, 2u);
    CK_I(g_wait_ms, 20u);

    /* no exit code reported: only the duration, no invented number */
    reset_env();
    g_info.exit_code_set = 0u;
    g_run_left = 0u;
    CK_I(cmd_exec(2, argv), 0);
    CK(out_has("finished after 0 ms (space 7)"));
    CK(!out_has("exit code"));

    /* the final reading itself failed: still say it finished, say nothing
     * about a code it could not read */
    reset_env();
    g_info_rc = -1;
    g_run_left = 1u;
    CK_I(cmd_exec(2, argv), 0);
    CK(!out_has("segment(s)"));
    CK(out_has("finished after 20 ms (space 7)"));
}

static void t_exec_timeout(void)
{
    char *argv[2];

    reset_env();
    argv[0] = W("exec");
    argv[1] = W("spin");
    g_run_always = 1;

    CK_I(cmd_exec(2, argv), 0);
    CK(out_has("still running after 30000 ms"));
    CK(out_has("prompt is back (see 'task')"));
    CK_I(g_wait_calls, 1500u);
}

static void t_exec_fallback(void)
{
    char *argv[3];
    char p_slash[] = "/bin/x";
    char p_dot[] = "./x";
    char p_up[] = "../x";
    char p_plain[] = "nosuch";
    char p_dotdot[] = "..";
    char p_dotone[] = ".";

    reset_env();
    argv[0] = p_slash;
    argv[1] = NULL;

    /* nothing that does not look like a path is taken */
    CK_I(ark_shell_exec_fallback(0, argv), -1);
    CK_I(ark_shell_exec_fallback(1, NULL), -1);
    argv[0] = NULL;
    CK_I(ark_shell_exec_fallback(1, argv), -1);
    CK_I(g_exec_calls, 0);

    /* the three accepted prefixes */
    argv[0] = p_slash;
    CK_I(ark_shell_exec_fallback(1, argv), 0);
    CK_S(g_exec_path, "/bin/x");
    CK_I(g_exec_argc, 1);

    argv[0] = p_dot;
    CK_I(ark_shell_exec_fallback(2, argv), 0);
    CK_S(g_exec_path, "./x");

    argv[0] = p_up;
    CK_I(ark_shell_exec_fallback(3, argv), 0);
    CK_S(g_exec_path, "../x");
    CK_I(g_exec_argc, 3);

    /* a bare word, ".." and "." are not paths: the shell must still report
     * command not found for them */
    argv[0] = p_plain;
    CK_I(ark_shell_exec_fallback(1, argv), -1);
    argv[0] = p_dotdot;
    CK_I(ark_shell_exec_fallback(1, argv), -1);
    argv[0] = p_dotone;
    CK_I(ark_shell_exec_fallback(1, argv), -1);
    CK_I(g_exec_calls, 3);
}

/* ------------------------------------------------------------ wait */

static void t_wait_refusals(void)
{
    char *argv[3];

    reset_env();
    argv[0] = W("wait");
    argv[1] = NULL;
    argv[2] = NULL;

    CK_I(cmd_wait(1, argv), 1);
    CK(out_has("usage: wait <task id> [timeout ms]"));

    argv[1] = W("abc");
    CK_I(cmd_wait(2, argv), 1);
    CK(out_has("usage: wait <task id>"));

    argv[1] = W("0");
    CK_I(cmd_wait(2, argv), 1);
    CK(out_has("usage: wait <task id>"));

    argv[1] = W("49");
    CK_I(cmd_wait(2, argv), 1);
    CK(out_has("usage: wait <task id>"));

    /* bad timeout is its own message, not the usage line */
    cap_reset();
    argv[1] = W("5");
    argv[2] = W("1x");
    CK_I(cmd_wait(3, argv), 1);
    CK(out_has("wait: timeout must be a decimal number of milliseconds"));
    CK(!out_has("usage: wait"));

    /* the shell's own task */
    cap_reset();
    arkrtos_current_task_id = 5;
    argv[2] = NULL;
    CK_I(cmd_wait(2, argv), 1);
    CK(out_has("wait: that is the shell's own task"));

    /* an unused slot: stack_ptr stays 0 */
    cap_reset();
    arkrtos_current_task_id = 1;
    {
        char idbuf[16];
        char want[48];

        snprintf(idbuf, sizeof idbuf, "%d", (int)ARKRTOS_TASK_MAX_NUM);
        snprintf(want, sizeof want, "wait: no such task %d",
                 (int)ARKRTOS_TASK_MAX_NUM);
        argv[1] = W(idbuf);
        CK_I(cmd_wait(2, argv), 1);
        CK(out_has(want));
        CK_I(g_wait_calls, 0u);
    }
}

static void t_wait_exit(void)
{
    char *argv[3];

    reset_env();
    argv[0] = W("wait");
    argv[1] = W("3");
    argv[2] = NULL;
    arkrtos_task_table[2].stack_ptr = 0x8000ul;
    arkrtos_task_table[2].status = ARKRTOS_TASK_INVALID;
    g_exit_code_rc = 1;
    g_exit_code = -7;

    CK_I(cmd_wait(2, argv), 0);
    CK(out_has("task 3 exited (exit code -7)"));
    CK_I(g_exit_code_seen, 3);
    CK_I(g_wait_calls, 0u);

    /* exited without reporting a code: say so, do not print 0 as if it had */
    reset_env();
    arkrtos_task_table[2].stack_ptr = 0x8000ul;
    arkrtos_task_table[2].status = ARKRTOS_TASK_INVALID;
    g_exit_code_rc = 0;
    CK_I(cmd_wait(2, argv), 0);
    CK(out_has("task 3 exited (no exit code reported)"));
    CK(!out_has("(exit code"));
}

static void t_wait_timeout_and_infinite(void)
{
    char *argv[3];

    reset_env();
    argv[0] = W("wait");
    argv[1] = W("4");
    argv[2] = W("40");
    arkrtos_task_table[3].stack_ptr = 0x8000ul;
    arkrtos_task_table[3].status = ARKRTOS_TASK_READY;

    CK_I(cmd_wait(3, argv), 0);
    CK(out_has("task 4 still running after 40 ms (status STUB-READY)"));
    CK_I(g_wait_calls, 2u);

    /* timeout 0 means "wait until it exits": the poll loop keeps going until
     * the task really goes INVALID -- here the stub flips it at poll 3 */
    reset_env();
    argv[2] = W("0");
    arkrtos_task_table[3].stack_ptr = 0x8000ul;
    arkrtos_task_table[3].status = ARKRTOS_TASK_READY;
    g_wait_flip_at = 3u;
    g_exit_code_rc = 1;
    g_exit_code = 12;

    CK_I(cmd_wait(3, argv), 0);
    CK(out_has("task 4 exited (exit code 12)"));
    CK_I(g_wait_calls, 3u);
}

/* ------------------------------------------------------------ ld */

static void t_ld_refusals(void)
{
    char *argv[2];

    reset_env();
    CK_I(cmd_ld(1, NULL), 1);
    CK(out_has("usage: ld <path.o> [args...]"));

    /* unknown symbol: the loader names it, the shell must repeat the name
     * and point at the export table */
    reset_env();
    argv[0] = W("ld");
    argv[1] = W("modhello.o");
    g_mod_load_rc = ARKRTOS_MOD_ERR_SYMBOL;
    strcpy(g_mod_missing, "arkrtos_log_write");
    CK_I(cmd_ld(2, argv), 1);
    CK(out_has("modhello.o: unknown symbol (rc=-6)"));
    CK(out_has("missing symbol: arkrtos_log_write"));
    CK(out_has("add it to kernelsrc/src/arkrtos_exports.c"));

    /* symbol failure without a name: no empty "missing symbol:" line */
    cap_reset();
    g_mod_missing[0] = '\0';
    CK_I(cmd_ld(2, argv), 1);
    CK(out_has("unknown symbol (rc=-6)"));
    CK(!out_has("missing symbol:"));

    /* bad relocation: print the type number the loader saw */
    cap_reset();
    g_mod_load_rc = ARKRTOS_MOD_ERR_RELOC;
    g_mod_bad_reloc = 44u;
    CK_I(cmd_ld(2, argv), 1);
    CK(out_has("unsupported relocation type: 44"));

    /* any other failure is one line and nothing more */
    cap_reset();
    g_mod_load_rc = ARKRTOS_MOD_ERR_FS;
    CK_I(cmd_ld(2, argv), 1);
    CK(out_has("modhello.o: vfs read failed (rc=-2)"));
    CK(!out_has("missing symbol"));
    CK(!out_has("unsupported relocation"));
}

static void t_ld_success(void)
{
    char *argv[2];

    reset_env();
    argv[0] = W("ld");
    argv[1] = W("m.o");
    g_mod_count_before = 1;
    g_mod_count_after = 2;
    strcpy(g_mod_info.name, "m");
    g_mod_info.base = 0x1234u;
    g_mod_info.size = 96u;
    g_mod_info.entry = 0x12A0u;
    g_mod_info.relocs = 17u;
    g_mod_info.imports = 3u;
    g_mod_info.task = 6;

    CK_I(cmd_ld(2, argv), 0);
    CK_I(g_mod_info_index, 1);
    CK(out_has("[MOD] m: base 0x00001234, 96 B, entry 0x000012A0"));
    CK(out_has("17 reloc(s), 3 import(s), task 6"));

    /* the info read fails: the load still succeeded, so the rc is 0 and no
     * [MOD] line is printed */
    reset_env();
    g_mod_count_before = 1;
    g_mod_count_after = 2;
    g_mod_info_rc = -1;
    CK_I(cmd_ld(2, argv), 0);
    CK(!out_has("[MOD]"));

    /* the count did not grow: nothing to describe */
    reset_env();
    g_mod_count_before = 2;
    g_mod_count_after = 2;
    CK_I(cmd_ld(2, argv), 0);
    CK(!out_has("[MOD]"));
    CK_I(g_mod_info_index, -1);
}

/* ------------------------------------------------------------ lkup */

static void t_lkup(void)
{
    char *argv[2];
    arkrtos_export_t tab[2];

    reset_env();
    argv[0] = W("lkup");
    argv[1] = W("arkrtos_log_write");
    g_exp_addr = 0x5678ul;
    CK_I(cmd_lkup(2, argv), 0);
    CK(out_has("arkrtos_log_write = 0x00005678"));

    cap_reset();
    g_exp_lookup_rc = -1;
    CK_I(cmd_lkup(2, argv), 1);
    CK(out_has("arkrtos_log_write: not exported"));

    /* no argument: the table is the answer, and a table-less build says so */
    cap_reset();
    CK_I(cmd_lkup(1, argv), 1);
    CK(out_has("export table not compiled in"));

    /* two rows, one of them without a name: print "?" rather than "(null)" */
    cap_reset();
    tab[0].name = "ark_ok";
    tab[0].addr = 0xABCul;
    tab[1].name = NULL;
    tab[1].addr = 0xDEFul;
    g_exp_tab = tab;
    g_exp_n = 2u;
    CK_I(cmd_lkup(1, argv), 0);
    CK(out_has("kernel exports (2):"));
    CK(out_has("0x00000ABC  ark_ok"));
    CK(out_has("0x00000DEF  ?"));
}

/* ------------------------------------------------------------ modules / unld */

static void t_modules(void)
{
    char *argv[2];

    reset_env();
    argv[0] = W("modules");
    g_mod_count_before = 0;
    CK_I(cmd_modules(1, argv), 0);
    CK(out_has("no module loaded"));

    /* three rows, the middle one unreadable: it is skipped, not printed as
     * a zeroed line */
    reset_env();
    cap_reset();
    g_mod_count_before = 3;
    strcpy(g_mod_info.name, "alpha");
    g_mod_info.base = 0x800u;
    g_mod_info.size = 64u;
    g_mod_info.entry = 0x840u;
    g_mod_info.relocs = 4u;
    g_mod_info.imports = 2u;
    g_mod_info.task = 9;
    g_mod_info.state = 1;

    CK_I(cmd_modules(1, argv), 0);
    CK(out_has("idx  name             base      size   entry     reloc  imp  task  state"));
    CK(out_has("alpha"));
    CK(out_has("run"));

    reset_env();
    cap_reset();
    g_mod_count_before = 2;
    g_mod_info_rc = -1;
    CK_I(cmd_modules(1, argv), 0);
    CK(out_has("idx  name"));
    CK(!out_has("run"));

    /* a finished module shows "done", not "run" */
    reset_env();
    cap_reset();
    g_mod_count_before = 1;
    strcpy(g_mod_info.name, "beta");
    g_mod_info.state = 0;
    g_mod_info.task = 0;
    CK_I(cmd_modules(1, argv), 0);
    CK(out_has("beta"));
    CK(out_has("done"));
    CK(!out_has("run\n"));
}

static void t_unld(void)
{
    char *argv[2];

    reset_env();
    argv[0] = W("unld");
    argv[1] = NULL;
    CK_I(cmd_unld(1, argv), 1);
    CK(out_has("usage: unld <index>"));

    argv[1] = W("2x");
    CK_I(cmd_unld(2, argv), 1);
    CK(out_has("unld: bad index '2x'"));
    CK_I(g_mod_unload_index, -1);

    /* the loader refuses because the module is still running */
    cap_reset();
    argv[1] = W("2");
    g_mod_unload_rc = ARKRTOS_MOD_ERR_BUSY;
    CK_I(cmd_unld(2, argv), 1);
    CK(out_has("unld 2: module still running (rc=-10)"));
    CK_I(g_mod_unload_index, 2);

    /* and the success line says the slot was released, nothing more */
    cap_reset();
    g_mod_unload_rc = ARKRTOS_MOD_OK;
    CK_I(cmd_unld(2, argv), 0);
    CK(out_has("unld 2: slot released"));
    CK_I(out_count("unld 2:"), 1u);
}

/* ------------------------------------------------------------ main */

int main(void)
{
    t_exec_usage_and_load_errors();
    t_exec_readings();
    t_exec_timeout();
    t_exec_fallback();
    t_wait_refusals();
    t_wait_exit();
    t_wait_timeout_and_infinite();
    t_ld_refusals();
    t_ld_success();
    t_lkup();
    t_modules();
    t_unld();

    if (g_failed != 0) {
        printf("SHELL-MOD-HOST FAIL  %d/%d checks failed\n", g_failed, g_checks);
        return 1;
    }
    printf("SHELL-MOD-HOST OK  %d checks\n", g_checks);
    return 0;
}
