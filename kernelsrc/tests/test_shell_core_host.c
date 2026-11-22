/**
* @file test_shell_core_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell.c.
*
* What this proves (the file is the console core, so this is the closest thing
* to a host-side "does the console come up" check):
*   - the nine user-visible name/text tables return the exact wording for
*     every enum value they know, and "?" / "unknown" / "-" for the rest;
*     until now every other shell test stubbed these out, so this is their
*     only real reading;
*   - parse_u32 accepts "0" / "123" and rejects NULL, "" and any non-digit;
*   - register_kernel_commands() writes the whole kernel command table in
*     order: 57 entries, each with the shipped name, handler pointer, help
*     text and max_args, followed by a NULL sentinel, and g_cmd_count is the
*     number of entries actually written;
*   - the table-full guard fires before the first write: starting at slot 31
*     the 50-slot upper bound would land on 81 > ARK_SHELL_MAX_COMMANDS (80),
*     so the function returns -1, changes nothing, and the console task says
*     "command table full" instead of silently overwriting memory;
*   - arkrtos_shell_init() hands the task entry, stack base, stack size,
*     priority and period to arkrtos_task_register() unchanged;
*   - the console loop drains at most SHELL_RX_DRAIN_MAX bytes per round,
*     stops early when the UART has nothing, yields for SHELL_TASK_PERIOD_MS,
*     and discards an over-long line (clearing the editor and resetting the
*     state machine) instead of letting it poison every later command;
*   - arkrtos_shell_release_task() refuses task id 0, drops exactly the user
*     commands owned by the requested task, and returns the count.
*
* What it does NOT prove:
*   - that a real UART, real shell state machine or real scheduler exists:
*     ark_shell_* and arkrtos_task_* are programmable stubs here;
*   - that the 57 handlers do anything: every cmd_* is a stub, so the table
*     check is about registration (name / pointer / help / max_args), not
*     about behaviour;
*   - the locked-console path (ARKRTOS_USE_SHELL_AUTH), the board command
*     hooks (ARKRTOS_SHELL_BOARD_CMDS), arkvim / arkc / ark make registration:
*     all four switches are off in this build, so those lines are not
*     compiled into the module measured here;
*   - the SHELL_ENABLE == 0 fallback stubs at the end of the file: they are
*     the branch this build does not take.
*
* The expected table below is deliberately a full copy of the shipped
* registry: that is the contract, and a silent help-text edit should fail
* here rather than be discovered by a user.
*
* @author xw
*/

#include <setjmp.h>
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
        if ((g_ == NULL) || (strcmp(g_, w_) != 0)) {                    \
            g_failed++;                                                \
            printf("FAIL line %d: %s (got %s want %s)\n",              \
                   __LINE__, #got, (g_ != NULL) ? g_ : "(null)", w_);  \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ capture */

static char g_out[8192];
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

/* ------------------------------------------------------------ globals owned by this test */

ark_shell_cmd_t g_cmd_table[ARK_SHELL_MAX_COMMANDS];
int             g_cmd_count;
arkrtos_ushell_entry_t g_ushell[ARKRTOS_USHELL_MAX_CMDS];

/* ------------------------------------------------------------ stubs: console out */

void platform_uart_send_string(const char *str)
{
    cap_add(str, strlen(str));
}

/* ------------------------------------------------------------ stubs: UART in */

static unsigned char g_rx[256];
static int  g_rx_len;
static int  g_rx_pos;
static int  g_recv_calls;

int platform_uart_recv(void)
{
    g_recv_calls++;
    if (g_rx_pos >= g_rx_len) {
        return -1;
    }
    return (int)g_rx[g_rx_pos++];
}

/* ------------------------------------------------------------ stubs: ark-shell core */

static uint32_t g_baud;
static ark_shell_complete_fn g_completer;
static int  g_proc_calls;
static int  g_proc_set_len;
static int  g_clear_calls;

void ark_shell_init(ark_shell_t *shell, uint32_t baudrate)
{
    g_baud = baudrate;
    shell->editor.len = 0;
    shell->editor.pos = 0;
    shell->state = STATE_NORMAL;
}

void ark_shell_set_completer(ark_shell_t *shell, ark_shell_complete_fn fn)
{
    (void)shell;
    g_completer = fn;
}

void ark_shell_process_byte(ark_shell_t *shell, unsigned char byte)
{
    (void)byte;
    g_proc_calls++;
    if (g_proc_set_len > 0) {
        shell->editor.len = g_proc_set_len;
    }
}

void line_editor_clear(line_editor_t *le)
{
    g_clear_calls++;
    le->len = 0;
    le->pos = 0;
}

int vfs_complete(ark_shell_t *shell, const char *word, int is_first,
                 char cand[][ARK_SHELL_LINE_SIZE], int max)
{
    (void)shell;
    (void)word;
    (void)is_first;
    (void)cand;
    (void)max;
    return 0;
}

/* ------------------------------------------------------------ stubs: kernel hooks */

static void (*g_reg_entry)(void);
static uint32 *g_reg_stack;
static uint32 g_reg_size;
static uint8  g_reg_prio;
static uint32 g_reg_period;
static int32  g_reg_rc;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    g_reg_entry = entry;
    g_reg_stack = stack_bottom;
    g_reg_size = stack_size;
    g_reg_prio = priority;
    g_reg_period = period_ms;
    return g_reg_rc;
}

static jmp_buf g_jmp;
static int g_wait_calls;
static int g_wait_ms;
static int g_wait_limit = 1;

void arkrtos_task_wait_internal(uint32 ms)
{
    g_wait_calls++;
    g_wait_ms = (int)ms;
    /* Return the first g_wait_limit-1 times so the console loop actually
     * goes round once more (that is what makes the loop's back edge real
     * code, not a line gcov only sees when it is skipped); the last call
     * jumps out of the endless loop. */
    if (g_wait_calls >= g_wait_limit) {
        longjmp(g_jmp, 1);
    }
}

static int g_drop_idx[16];
static int g_drop_calls;

void ushell_drop(uint32 i)
{
    if (g_drop_calls < 16) {
        g_drop_idx[g_drop_calls] = (int)i;
    }
    g_drop_calls++;
    g_ushell[i].in_use = 0u;
}

/* ------------------------------------------------------------ stubs: every registered handler */

static int g_cmd_calls;

#define CMD_STUB(name)                                                 \
    int name(int argc, char *argv[])                                   \
    {                                                                  \
        (void)argv;                                                    \
        g_cmd_calls++;                                                 \
        return argc;                                                   \
    }

CMD_STUB(cmd_info)
CMD_STUB(cmd_app)
CMD_STUB(cmd_exec)
CMD_STUB(cmd_wait)
CMD_STUB(cmd_ld)
CMD_STUB(cmd_lkup)
CMD_STUB(cmd_modules)
CMD_STUB(cmd_unld)
CMD_STUB(cmd_drv)
CMD_STUB(cmd_dt)
CMD_STUB(cmd_fdt)
CMD_STUB(cmd_task)
CMD_STUB(cmd_stack)
CMD_STUB(cmd_pmap)
CMD_STUB(cmd_sched)
CMD_STUB(cmd_fault)
CMD_STUB(cmd_install)
CMD_STUB(cmd_log)
CMD_STUB(cmd_syncinfo)
CMD_STUB(cmd_pool)
CMD_STUB(arkrtos_trace_shell_cmd)
CMD_STUB(cmd_cfg)
CMD_STUB(cmd_blk)
CMD_STUB(cmd_fs)
CMD_STUB(cmd_vfs_mount)
CMD_STUB(cmd_vfs_umount)
CMD_STUB(cmd_vfs_ls)
CMD_STUB(cmd_vfs_cat)
CMD_STUB(cmd_vfs_rm)
CMD_STUB(cmd_vfs_mkdir)
CMD_STUB(cmd_vfs_rmdir)
CMD_STUB(cmd_vfs_mv)
CMD_STUB(cmd_vfs_cp)
CMD_STUB(cmd_vfs_touch)
CMD_STUB(cmd_vfs_stat)
CMD_STUB(cmd_vfs_chmod)
CMD_STUB(cmd_vfs_chown)
CMD_STUB(cmd_audit)
CMD_STUB(cmd_guard)
CMD_STUB(cmd_hwsafe)
CMD_STUB(cmd_swap)
CMD_STUB(cmd_crash)
CMD_STUB(arkrtos_softbus_shell_cmd)
CMD_STUB(cmd_mpool)
CMD_STUB(cmd_utest)
CMD_STUB(cmd_rt)
CMD_STUB(cmd_sec)
CMD_STUB(cmd_ps)
CMD_STUB(cmd_pwd)
CMD_STUB(cmd_cd)
CMD_STUB(cmd_df)
CMD_STUB(cmd_history)
CMD_STUB(cmd_uname)
CMD_STUB(cmd_free)
CMD_STUB(cmd_which)
CMD_STUB(cmd_man)
CMD_STUB(cmd_dmesg)

/* ------------------------------------------------------------ expected registry */

typedef struct {
    const char *name;
    int       (*func)(int argc, char *argv[]);
    const char *help;
    int         max_args;
} expect_cmd_t;

static const expect_cmd_t g_expect[] = {
    { "info", cmd_info, "Kernel, partition and task capacity info", 1 },
    { "app", cmd_app, "App slots: app [list | install <path> [slot] | start <slot> | stop <slot> | uninstall <slot>]", 4 },
    { "exec", cmd_exec, "Run an ELF64 executable: exec <path> [args...] (or just type /path/prog)", 8 },
    { "wait", cmd_wait, "Wait for a task to exit and report its exit code: wait <task id> [timeout ms]", 3 },
    { "ld", cmd_ld, "Link and run a relocatable module: ld <path.o> [args...]", 8 },
    { "lkup", cmd_lkup, "Kernel export table: lkup [symbol]", 2 },
    { "modules", cmd_modules, "List resident modules (see also unld)", 1 },
    { "unld", cmd_unld, "Unload a finished module: unld <index>", 2 },
    { "drv", cmd_drv, "Driver slots: drv [list | start <slot> | stop <slot> | uninstall <slot>]", 4 },
    { "dt", cmd_dt, "Dump the compiled device tree (model, nodes, compatible, reg)", 1 },
    { "fdt", cmd_fdt, "Device tree: fdt | fdt path <abs> | fdt phandle <n> | fdt chosen | fdt get <path> <prop> | fdt overlay | fdt save <file> | fdt load <file>", 4 },
    { "task", cmd_task, "List kernel tasks, or dump one task frame: task <id> (ids start at 1)", 3 },
    { "stack", cmd_stack, "Per-task stack usage, measured (fill pattern + low PSP): stack [check]", 2 },
    { "pmap", cmd_pmap, "Paging: pmap [map|check|rd|rb|wrb|test|space|u1|elf|u2|look|unmap|dyn|cu] ...", 6 },
    { "sched", cmd_sched, "Scheduler readiness self-check (0 = consistent)", 1 },
    { "fault", cmd_fault, "Show recorded faults", 1 },
    { "install", cmd_install, "Open a one-shot install window on the console UART: install [slot]", 2 },
    { "log", cmd_log, "Get or set runtime log level: log [0..4]", 2 },
    { "syncinfo", cmd_syncinfo, "Dump semaphore / mutex / condition tables with their waiters", 1 },
    { "pool", cmd_pool, "Free space left in the image pool", 1 },
    { "trace", arkrtos_trace_shell_cmd, "Kernel event trace: trace [start | stop | reset | dump | mark <n>]", 3 },
    { "cfg", cmd_cfg, "Device layout config: cfg [show | load | clear]", 2 },
    { "blk", cmd_blk, "Block devices: blk [list | probe | rd | wr | erase | test]", 5 },
    { "fs", cmd_fs, "File system: fs [mount | unmount | format | info | ls | wr | rd | put | rm | test | err]", 9 },
    { "mount", cmd_vfs_mount, "List mount points, or: mount <target> | mount <dev> <off> <size> <target>", 5 },
    { "umount", cmd_vfs_umount, "Drop a mount point: umount <target>", 2 },
    { "ls", cmd_vfs_ls, "List a directory: ls [-l] [-a] [path]", 4 },
    { "cat", cmd_vfs_cat, "Print a file, or standard input: cat [path]", 2 },
    { "rm", cmd_vfs_rm, "Remove files: rm <path>...", 8 },
    { "mkdir", cmd_vfs_mkdir, "Create directories: mkdir <path>...", 8 },
    { "rmdir", cmd_vfs_rmdir, "Remove empty directories: rmdir <path>...", 8 },
    { "mv", cmd_vfs_mv, "Rename or move: mv <src> <dst>", 3 },
    { "cp", cmd_vfs_cp, "Copy a file: cp <src> <dst>", 3 },
    { "touch", cmd_vfs_touch, "Create empty files: touch <path>...", 8 },
    { "stat", cmd_vfs_stat, "Show file type, mode, owner and size: stat <path>", 2 },
    { "chmod", cmd_vfs_chmod, "Change access bits: chmod <mode> <path>  (mode is octal, e.g. 644)", 3 },
    { "chown", cmd_vfs_chown, "Change numeric owner: chown <uid>[:<gid>] <path>", 3 },
    { "audit", cmd_audit, "Structure self-audit: partition / slots / tasks vs their invariants", 1 },
    { "guard", cmd_guard, "Watchdog state and per-slot heartbeat contracts", 1 },
    { "hwsafe", cmd_hwsafe, "Hardware safe-state (will) ledger: owners, registered devices, neutralized count", 1 },
    { "swap", cmd_swap, "Task hot-swap self-test: frame check / block move / frame rewrite / resume path", 3 },
    { "crash", cmd_crash, "Cross-reset crash journal: boots, per-slot fault counts, held slots and disabled MiniApps", 1 },
    { "bus", arkrtos_softbus_shell_cmd, "Soft bus (ark_softbus): bus [info | selftest | pump [n] | peers | rx | pub <topic> <text> | rpc <text> | stream <n>]", 4 },
    { "mpool", cmd_mpool, "Fixed-block memory pools: mpool [list | test]", 2 },
    { "utest", cmd_utest, "On-board unit tests: utest [list | run [pattern]]", 3 },
    { "rt", cmd_rt, "Real-time latency (DWT cycles): rt [show | reset | run [n]]", 3 },
    { "sec", cmd_sec, "Security baseline self-check: sec [all | pass | fail | find | skip]", 2 },
    { "ps", cmd_ps, "List kernel tasks and installed images", 1 },
    { "pwd", cmd_pwd, "Print the working directory", 1 },
    { "cd", cmd_cd, "Change the working directory: cd [dir]", 2 },
    { "df", cmd_df, "Show mount points and volume usage", 1 },
    { "history", cmd_history, "Show the command history", 1 },
    { "uname", cmd_uname, "System and build information: uname [-a]", 2 },
    { "free", cmd_free, "Memory summary (kernel image pool + kernel RAM window)", 1 },
    { "which", cmd_which, "Resolve an alias to the real command: which <command>", 2 },
    { "man", cmd_man, "Show a command's help text: man <command>", 2 },
    { "dmesg", cmd_dmesg, "Print the kernel log ring: dmesg [-c]", 2 }
};

#define EXPECT_COUNT ((int)(sizeof(g_expect) / sizeof(g_expect[0])))

/* ------------------------------------------------------------ task run helper */

static void run_task_once(void)
{
    g_wait_calls = 0;
    if (setjmp(g_jmp) == 0) {
        g_reg_entry();
    }
}

/* Reset everything a task pass touches, then register the entry once. */
static void prep_task(void)
{
    memset(g_cmd_table, 0, sizeof(g_cmd_table));
    g_cmd_count = 0;
    g_wait_limit = 1;
    g_rx_len = 0;
    g_rx_pos = 0;
    g_recv_calls = 0;
    g_proc_calls = 0;
    g_proc_set_len = 0;
    g_clear_calls = 0;
    g_completer = NULL;
    g_baud = 0u;
    cap_reset();

    CK_I(arkrtos_shell_init(), g_reg_rc);
    CK(g_reg_entry != NULL);
}

static void feed(const char *s)
{
    size_t n = strlen(s);

    if (n > sizeof(g_rx)) {
        n = sizeof(g_rx);
    }
    memcpy(g_rx, s, n);
    g_rx_len = (int)n;
    g_rx_pos = 0;
}

/* ------------------------------------------------------------ tests */

static void t_name_tables(void)
{
    CK_S(slot_state_name(ARKRTOS_APP_SLOT_EMPTY), "EMPTY");
    CK_S(slot_state_name(ARKRTOS_APP_SLOT_LOADED), "LOADED");
    CK_S(slot_state_name(ARKRTOS_APP_SLOT_RUNNING), "RUNNING");
    CK_S(slot_state_name(ARKRTOS_APP_SLOT_INVALID), "INVALID");
    CK_S(slot_state_name(ARKRTOS_APP_SLOT_INSTALLING), "INSTALL");
    CK_S(slot_state_name(ARKRTOS_APP_SLOT_RAW), "RAW");
    CK_S(slot_state_name(77u), "?");

    CK_S(slot_type_name(ARKRTOS_SLOT_APP), "app");
    CK_S(slot_type_name(ARKRTOS_SLOT_DRIVER), "drv");
    CK_S(slot_type_name(ARKRTOS_SLOT_FREE), "free");
    CK_S(slot_type_name(77u), "?");

    CK_S(cfg_mode_text(ARKRTOS_LAYOUT_MODE_FIXED), "fixed");
    CK_S(cfg_mode_text(ARKRTOS_LAYOUT_MODE_AUTO), "auto");
    CK_S(cfg_mode_text(77u), "unknown");

    CK_S(cfg_source_text(ARKRTOS_LAYOUT_SOURCE_CONFIG), "device config region");
    CK_S(cfg_source_text(ARKRTOS_LAYOUT_SOURCE_DEFAULT), "compile-time default");
    CK_S(cfg_source_text(77u), "unknown");

    CK_S(cfg_reclaim_text(ARKRTOS_CFG_RECLAIM_GLOBAL), "global");
    CK_S(cfg_reclaim_text(ARKRTOS_CFG_RECLAIM_MINIMAL), "minimal");
    CK_S(cfg_reclaim_text(ARKRTOS_CFG_RECLAIM_NONE), "none");
    CK_S(cfg_reclaim_text(77u), "unknown");

    CK_S(cfg_type_text(ARKRTOS_CFG_TYPE_APP), "app");
    CK_S(cfg_type_text(ARKRTOS_CFG_TYPE_DRIVER), "drv");
    CK_S(cfg_type_text(77u), "-");

    CK_S(task_state_name(ARKRTOS_TASK_INVALID), "INVALID");
    CK_S(task_state_name(ARKRTOS_TASK_READY), "READY");
    CK_S(task_state_name(ARKRTOS_TASK_WAIT), "WAIT");
    CK_S(task_state_name(ARKRTOS_TASK_RUNNING), "RUNNING");
    CK_S(task_state_name(77u), "?");

    CK_S(fault_type_name(ARKRTOS_FAULT_HARDFAULT), "HARDFAULT");
    CK_S(fault_type_name(ARKRTOS_FAULT_STACKOVF), "STACKOVF");
    CK_S(fault_type_name(ARKRTOS_FAULT_TASKKILL), "TASKKILL");
    CK_S(fault_type_name(ARKRTOS_FAULT_RECOVER), "RECOVER");
    CK_S(fault_type_name(ARKRTOS_FAULT_SCHEDLOCK), "SCHEDLOCK");
    CK_S(fault_type_name(ARKRTOS_FAULT_APPDISABLED), "APPDISABLED");
    CK_S(fault_type_name(ARKRTOS_FAULT_NOSLOT), "NOSLOT");
    CK_S(fault_type_name(ARKRTOS_FAULT_MEMFAULT), "MEMFAULT");
    CK_S(fault_type_name(ARKRTOS_FAULT_BUSFAULT), "BUSFAULT");
    CK_S(fault_type_name(ARKRTOS_FAULT_USGFAULT), "USGFAULT");
    CK_S(fault_type_name(ARKRTOS_FAULT_INSTALLFAIL), "INSTALLFAIL");
    CK_S(fault_type_name(77u), "?");
}

static void t_parse_u32(void)
{
    uint32 v = 0xA5A5A5A5u;

    CK_I(parse_u32(NULL, &v), -1);
    CK_I(v, 0xA5A5A5A5);

    CK_I(parse_u32("", &v), -1);
    CK_I(v, 0xA5A5A5A5);

    CK_I(parse_u32("0", &v), 0);
    CK_I(v, 0u);

    CK_I(parse_u32("123", &v), 0);
    CK_I(v, 123u);

    CK_I(parse_u32("4a", &v), -1);
    CK_I(v, 123u);

    CK_I(parse_u32("a", &v), -1);
    CK_I(v, 123u);
}

static void t_sh_out(void)
{
    cap_reset();
    sh_out("hello core\n");
    CK_S(g_out, "hello core\n");
}

static void t_table_full(void)
{
    prep_task();

    /* 31 + 50 = 81 > ARK_SHELL_MAX_COMMANDS (80): the guard runs before any
     * write, so both the table and g_cmd_count must be untouched. */
    g_cmd_count = ARK_SHELL_MAX_COMMANDS - 49;
    g_cmd_table[g_cmd_count].name = "sentinel-slot";
    run_task_once();

    CK_I(g_cmd_count, ARK_SHELL_MAX_COMMANDS - 49);
    CK_S(g_cmd_table[g_cmd_count].name, "sentinel-slot");
    CK(out_has("shell: command table full, kernel commands unavailable"));
    CK_I(g_clear_calls, 0);
    CK_I(g_wait_calls, 1);
}

static void t_register_ok(void)
{
    int i;

    prep_task();

    feed("");
    run_task_once();

    /* ark_shell_init() and the completer hook are called from the task body,
     * so they are only observable after one pass. */
    CK_I(g_baud, (uint32_t)SHELL_DEV_ARG);
    CK(g_completer == vfs_complete);

    CK_I(g_cmd_count, EXPECT_COUNT);
    for (i = 0; i < EXPECT_COUNT; i++) {
        if ((g_cmd_table[i].name == NULL) ||
            (strcmp(g_cmd_table[i].name, g_expect[i].name) != 0)) {
            printf("FAIL entry %d name: %s\n", i,
                   (g_cmd_table[i].name != NULL) ? g_cmd_table[i].name : "(null)");
            g_failed++;
        }
        g_checks++;
        if (g_cmd_table[i].func != g_expect[i].func) {
            printf("FAIL entry %d (%s) handler pointer differs\n", i, g_expect[i].name);
            g_failed++;
        }
        g_checks++;
        if ((g_cmd_table[i].help == NULL) ||
            (strcmp(g_cmd_table[i].help, g_expect[i].help) != 0)) {
            printf("FAIL entry %d (%s) help text differs\n", i, g_expect[i].name);
            g_failed++;
        }
        g_checks++;
        if (g_cmd_table[i].max_args != g_expect[i].max_args) {
            printf("FAIL entry %d (%s) max_args %d != %d\n", i, g_expect[i].name,
                   g_cmd_table[i].max_args, g_expect[i].max_args);
            g_failed++;
        }
    }

    /* Sentry terminator on the next slot. */
    CK(g_cmd_table[EXPECT_COUNT].name == NULL);
    CK(g_cmd_table[EXPECT_COUNT].func == NULL);
    CK(g_cmd_table[EXPECT_COUNT].help == NULL);
    CK_I(g_cmd_table[EXPECT_COUNT].max_args, 0);
    CK_I(g_cmd_calls, 0);
}

static void t_task_loop(void)
{
    /* 100 bytes offered, budget is SHELL_RX_DRAIN_MAX: exactly that many
     * bytes are consumed and the UART is not polled again. */
    prep_task();
    memset(g_rx, 'x', sizeof(g_rx));
    g_rx_len = 100;
    g_rx_pos = 0;
    g_wait_limit = 2;
    run_task_once();

    /* Round 1 takes the full budget (64), round 2 takes the remaining 36 and
     * then sees an empty UART: per-round budget, not a per-line limit. */
    CK_I(g_proc_calls, 100);
    CK_I(g_recv_calls, 101);
    CK_I(g_wait_calls, 2);
    CK_I(g_wait_ms, SHELL_TASK_PERIOD_MS);

    /* Empty UART: one poll, immediate break, still yields. */
    prep_task();
    run_task_once();
    CK_I(g_proc_calls, 0);
    CK_I(g_recv_calls, 1);
    CK_I(g_wait_calls, 1);

    /* Over-long line: the fragment is dropped and the state machine reset. */
    prep_task();
    feed("a");
    g_proc_set_len = ARK_SHELL_LINE_SIZE - 1;
    run_task_once();
    CK_I(g_proc_calls, 1);
    CK_I(g_clear_calls, 1);
    CK_I((int)g_shell.state, (int)STATE_NORMAL);
    CK_I(g_shell.editor.len, 0);
    CK(out_has("console: line too long, discarded"));
}

static void t_shell_init_args(void)
{
    prep_task();
    CK_I((long)g_reg_size, (long)ARKRTOS_SHELL_STACK_SIZE);
    CK(g_reg_stack != NULL);
    CK_I(g_reg_prio, SHELL_TASK_PRIORITY);
    CK_I(g_reg_period, SHELL_TASK_PERIOD_MS);

    /* Default (no return-code override) is 0; the module must pass it back. */
    g_reg_rc = -7;
    CK_I(arkrtos_shell_init(), -7);
    g_reg_rc = 0;
    CK_I(arkrtos_shell_init(), 0);
}

static void t_release_task(void)
{
    memset(g_ushell, 0, sizeof(g_ushell));
    g_drop_calls = 0;

    /* Owner task 5: slots 0 and 3. Slot 1 is free, slot 2 belongs to task 7. */
    g_ushell[0].in_use = 1u;
    g_ushell[0].owner_task = 5u;
    g_ushell[1].in_use = 0u;
    g_ushell[1].owner_task = 5u;
    g_ushell[2].in_use = 1u;
    g_ushell[2].owner_task = 7u;
    g_ushell[3].in_use = 1u;
    g_ushell[3].owner_task = 5u;

    CK_I(arkrtos_shell_release_task(0u), 0);
    CK_I(g_drop_calls, 0);
    CK_I((int)g_ushell[0].in_use, 1);

    CK_I(arkrtos_shell_release_task(5u), 2);
    CK_I(g_drop_calls, 2);
    CK_I(g_drop_idx[0], 0);
    CK_I(g_drop_idx[1], 3);
    CK_I((int)g_ushell[0].in_use, 0);
    CK_I((int)g_ushell[1].in_use, 0);
    CK_I((int)g_ushell[2].in_use, 1);

    /* Second call finds nothing left to drop. */
    CK_I(arkrtos_shell_release_task(5u), 0);
    CK_I(g_drop_calls, 2);
}

/* ------------------------------------------------------------ main */

int main(void)
{
    g_reg_rc = 0;

    t_name_tables();
    t_parse_u32();
    t_sh_out();
    t_table_full();
    t_register_ok();
    t_task_loop();
    t_shell_init_args();
    t_release_task();

    if (g_failed != 0) {
        printf("SHELL-CORE-HOST FAILED: %d of %d checks\n", g_failed, g_checks);
        return 1;
    }

    printf("SHELL-CORE-HOST OK  %d checks\n", g_checks);
    return 0;
}
