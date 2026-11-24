/**
* @file test_shell_diag_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_diag.c.
*
* What this proves: the diagnostic command family on the host, every command
* and every printed branch executed and checked:
*   - task: the table listing (with the stack-peak column when accounting is
*     on), the per-task 56-word frame dump, the "no such task" refusals
*     (non-numeric id, id 0, id past the table, a task slot with no stack)
*     and the optional blocks/wake-reason line;
*   - stack: the default listing with its three "cannot measure" forms
*     (query refused, total 0, used 0 must not read as "free"), the warn and
*     danger marks, check's three verdicts (unmeasured, at/over the danger
*     line, ok), raw's two read-out sources and its n/a row, and the idle
*     context line both armed and refused;
*   - guard: disabled / requested-but-not-running / armed, the per-slot rows
*     (including an unset age printing "-" and an empty slot skipped) and the
*     "no image declared a period" note;
*   - crash: an empty journal, per-slot rows, a MiniApp journal that matches
*     a known autostart path and one that only has its raw hash, and the
*     empty MiniApp case;
*   - audit: a clean pass and a failing pass (BAD rows, the offender count
*     returned as the exit code, and a bit whose name is unknown being
*     skipped rather than printed);
*   - sched: consistent and inconsistent, with the per-task mismatch line
*     printed for both directions (on the ready chain but not ready /
*     ready but not on the chain);
*   - dt: a device node printed and a non-device node skipped;
*   - fdt: not-ready, the whole-tree dump (including a walk that ends on an
*     error), path (found / refused / usage), phandle (bad argument /
*     refused / resolved), chosen (absent / string / raw cell / resolve
*     refused / resolved), get (missing property / string / string list /
*     cells with a non-cell tail), overlay (no fragment / refused / merged
*     but rejected / applied), save (no tree / written / refused) and load
*     (read refused / blob refused, previous tree kept / applied);
*   - fault: no record and a list where one entry is a hole;
*   - syncinfo: sem / mtx / cond rows, an empty row skipped, a row with
*     waiters, and the semaphore event ledger;
*   - log: the level listing, a level change, the refusal of an
*     out-of-range level, and the console / fifo / sync counters;
*   - rt: no cycle counter, a normal report (both counter states, both
*     clock-source texts, a row with no sample, a row with late/skip counts,
*     jitter present and absent, the tickless line), reset, run (bad n,
*     counter still dead, wake scenario refused, preempt scenario refused
*     with its reason code, and the successful run) and the usage line;
*   - sec: all five filters, an unknown filter, the per-item print with and
*     without a format string, and both verdicts;
*   - utest: list, a passing run, a failing run (the failure count is the
*     exit code), and the usage line;
*   - pool: auto layout (two lines only) and fixed layout (a slot matched to
*     a live record and one that reads as empty).
*
* What it does NOT prove:
*   - anything about the real hardware behind these read-outs: every task
*     table, watchdog, crash journal, device tree, RT sampler, sync table and
*     security item is a stub here.  What is checked is the command's own
*     decoding, formatting and exit code;
*   - the stack other branch: qemu_loongarch64 builds with
*     ARKRTOS_USE_STACK_USAGE=1, so the "accounting is compiled out" text is
*     not in the module measured here;
*   - that a real DWT cycle counter behaves like the stub: rt is exercised
*     for its branches, not for its measurement quality;
*   - the fixed-slot RT/TICKLESS combinations the board does not build.
*
* Board note: the one thing the wrapper header changes is
* ARKRTOS_USE_MINIAPP (0 -> 1), so the MiniApp journal branch of crash is
* compiled and measured.  RTSTAT and TICKLESS are plain -D flags.  See
* kernelsrc/tests/diag_test_board.h.
*
* @author xw
*/

#if defined(_WIN32)
#include <windows.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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
            printf("FAIL line %d: %s (got \"%s\" want \"%s\")\n",      \
                   __LINE__, #got, g_ ? g_ : "(null)", w_);            \
        }                                                              \
    } while (0)

/* ------------------------------------------------------------ capture */

static char g_out[65536];
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

static int out_count(const char *needle)
{
    const char *p = g_out;
    size_t n = strlen(needle);
    int c = 0;

    if (n == 0u) {
        return 0;
    }
    while ((p = strstr(p, needle)) != NULL) {
        c++;
        p += n;
    }
    return c;
}

/* Writable copies for the entry points that take char *argv[]. */
static char *W(const char *s)
{
    static char bufs[16][64];
    static unsigned idx;
    char *b = bufs[idx % 16u];
    size_t n = strlen(s);

    idx++;
    if (n >= sizeof(bufs[0])) {
        n = sizeof(bufs[0]) - 1u;
    }
    memcpy(b, s, n);
    b[n] = '\0';
    return b;
}

/* ------------------------------------------------------------ shell glue */

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

void sh_out(const char *s)
{
    cap_add(s, strlen(s));
}

/* Decimal only.  Every command that parses a number here parses a count,
 * an index or a phandle, never an address. */
int parse_u32(const char *s, uint32 *out)
{
    uint32 v = 0u;

    if ((s == NULL) || (out == NULL) || (*s == '\0')) {
        return -1;
    }
    while (*s != '\0') {
        if ((*s < '0') || (*s > '9')) {
            return -1;
        }
        v = (v * 10u) + (uint32)(*s - '0');
        s++;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------------ name tables
 * These are the helpers the module imports from arkrtos_shell_misc.c.  The
 * strings are the contract the commands print, so they are copied verbatim
 * from their only real implementation. */
const char *task_state_name(uint32 st)
{
    switch (st) {
    case 0u: return "invalid";
    case 1u: return "ready";
    case 2u: return "wait";
    case 3u: return "run";
    default: return "-";
    }
}

const char *fault_type_name(uint32 t)
{
    switch (t) {
    case 0u:  return "none";
    case 1u:  return "hardfault";
    case 13u: return "guardhb";
    default: return "other";
    }
}

const char *slot_type_name(uint32 t)
{
    switch (t) {
    case 0u: return "free";
    case 1u: return "app";
    case 2u: return "drv";
    default: return "?";
    }
}

const char *slot_state_name(uint32 st)
{
    switch (st) {
    case 0u: return "empty";
    case 1u: return "loaded";
    case 2u: return "running";
    case 3u: return "invalid";
    case 4u: return "installing";
    case 5u: return "raw";
    default: return "?";
    }
}

const char *cfg_type_text(uint32 t)
{
    switch (t) {
    case 0u: return "none";
    case 1u: return "app";
    case 2u: return "drv";
    default: return "?";
    }
}

/* ------------------------------------------------------------ task table */

int32 arkrtos_task_count;
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
volatile uint8 arkrtos_dbg_wake_last[ARKRTOS_TASK_MAX_NUM];
volatile uint8 arkrtos_dbg_wake_hits[ARKRTOS_TASK_MAX_NUM];

static uint32 g_frame_words[56];
static uint32 g_stack_bottom_buf[2048];

static int32 g_si_rc[ARKRTOS_TASK_MAX_NUM];
static uint32 g_si3[ARKRTOS_TASK_MAX_NUM][3];
static int32 g_raw_rc[ARKRTOS_TASK_MAX_NUM];
static uint32 g_raw4[ARKRTOS_TASK_MAX_NUM][4];
static int32 g_ctx_rc;
static uint32 g_ctx4[4];

int32 arkrtos_task_stack_info_internal(int32 task_id, uint32 *out3)
{
    if ((task_id < 1) || (task_id > arkrtos_task_count)) {
        return -1;
    }
    if (g_si_rc[task_id - 1] != 0) {
        return g_si_rc[task_id - 1];
    }
    out3[0] = g_si3[task_id - 1][0];
    out3[1] = g_si3[task_id - 1][1];
    out3[2] = g_si3[task_id - 1][2];
    return 0;
}

int32 arkrtos_task_stack_info_raw(int32 task_id, uint32 *out4)
{
    if ((task_id < 1) || (task_id > arkrtos_task_count)) {
        return -1;
    }
    if (g_raw_rc[task_id - 1] != 0) {
        return g_raw_rc[task_id - 1];
    }
    out4[0] = g_raw4[task_id - 1][0];
    out4[1] = g_raw4[task_id - 1][1];
    out4[2] = g_raw4[task_id - 1][2];
    out4[3] = g_raw4[task_id - 1][3];
    return 0;
}

int32 arkrtos_stack_guard_context_info(uint32 out[4])
{
    if (g_ctx_rc != 0) {
        return g_ctx_rc;
    }
    out[0] = g_ctx4[0];
    out[1] = g_ctx4[1];
    out[2] = g_ctx4[2];
    out[3] = g_ctx4[3];
    return 0;
}

/* ------------------------------------------------------------ scheduler */

static uint32 g_ready_count;
static int32 g_ready_top_v;
static int32 g_sched_bad;
static uint8 g_ready_on[ARKRTOS_TASK_MAX_NUM];

uint32 arkrtos_sched_ready_count(void)
{
    return g_ready_count;
}

int32 arkrtos_ready_top(void)
{
    return g_ready_top_v;
}

int32 arkrtos_sched_check(void)
{
    return g_sched_bad;
}

uint8 arkrtos_ready_contains(int32 task_idx)
{
    if (task_idx < 0) {
        return 0u;
    }
    return g_ready_on[task_idx];
}

/* ------------------------------------------------------------ guard */

static arkrtos_guard_status_t g_guard_st;
static arkrtos_guard_task_t g_guard_t[8];
static uint32 g_guard_slots;
static int g_guard_fail_at = -1;

void arkrtos_guard_status(arkrtos_guard_status_t *p_out)
{
    *p_out = g_guard_st;
}

int32 arkrtos_guard_task_at(uint32 idx, arkrtos_guard_task_t *p_out)
{
    if ((g_guard_fail_at >= 0) && (idx == (uint32)g_guard_fail_at)) {
        return -1;
    }
    if (idx >= g_guard_slots) {
        return -1;
    }
    *p_out = g_guard_t[idx];
    return 0;
}

uint32 arkrtos_guard_task_slots(void)
{
    return g_guard_slots;
}

/* ------------------------------------------------------------ partition */

static arkrtos_partition_table_t g_pt;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

/* ------------------------------------------------------------ crash */

static uint32 g_crash_cnt[ARKRTOS_SLOT_ARRAY_MAX];
static uint32 g_crash_hold[ARKRTOS_SLOT_ARRAY_MAX];
static uint32 g_crash_last[ARKRTOS_SLOT_ARRAY_MAX];
static uint32 g_crash_tick[ARKRTOS_SLOT_ARRAY_MAX];
static uint32 g_crash_boots_v;
static uint32 g_crash_seq_v;

static uint32 g_mini_used[ARKRTOS_CRASH_MINI_MAX];
static uint32 g_mini_hash[ARKRTOS_CRASH_MINI_MAX];
static uint32 g_mini_cnt[ARKRTOS_CRASH_MINI_MAX];
static uint32 g_mini_last[ARKRTOS_CRASH_MINI_MAX];
static uint32 g_mini_hold[ARKRTOS_CRASH_MINI_MAX];
static uint32 g_mini_tick[ARKRTOS_CRASH_MINI_MAX];

static const char *g_mini_list[4];
static uint32 g_mini_list_n;

uint32 arkrtos_crash_area_base(void)
{
    return 0x2001F000u;
}

uint32 arkrtos_crash_area_size(void)
{
    return 512u;
}

uint32 arkrtos_crash_boots(void)
{
    return g_crash_boots_v;
}

uint32 arkrtos_crash_seq(void)
{
    return g_crash_seq_v;
}

uint32 arkrtos_crash_count(uint32 slot)
{
    return (slot < ARKRTOS_SLOT_ARRAY_MAX) ? g_crash_cnt[slot] : 0u;
}

uint32 arkrtos_crash_disabled(uint32 slot)
{
    return (slot < ARKRTOS_SLOT_ARRAY_MAX) ? g_crash_hold[slot] : 0u;
}

uint32 arkrtos_crash_last_reason(uint32 slot)
{
    return (slot < ARKRTOS_SLOT_ARRAY_MAX) ? g_crash_last[slot] : 0u;
}

uint32 arkrtos_crash_disabled_tick(uint32 slot)
{
    return (slot < ARKRTOS_SLOT_ARRAY_MAX) ? g_crash_tick[slot] : 0u;
}

const char *arkrtos_crash_reason_name(uint32 reason)
{
    switch (reason) {
    case 0u: return "none";
    case 1u: return "fault";
    case 2u: return "heartbeat";
    case 3u: return "audit";
    default: return "?";
    }
}

uint32 arkrtos_crash_hash_path(const char *path)
{
    uint32 h = 2166136261u;
    const unsigned char *p = (const unsigned char *)path;

    while (*p != 0u) {
        h ^= (uint32)(*p);
        h *= 16777619u;
        p++;
    }
    return h;
}

uint32 arkrtos_crash_mini_at(uint32 idx, uint32 *hash, uint32 *cnt,
                             uint32 *last, uint32 *hold, uint32 *tick)
{
    if ((idx >= ARKRTOS_CRASH_MINI_MAX) || (g_mini_used[idx] == 0u)) {
        return 0u;
    }
    *hash = g_mini_hash[idx];
    *cnt  = g_mini_cnt[idx];
    *last = g_mini_last[idx];
    *hold = g_mini_hold[idx];
    *tick = g_mini_tick[idx];
    return 1u;
}

int32 arkrtos_mini_autostart_foreach(arkrtos_mini_auto_cb_t cb, void *arg)
{
    uint32 i;

    for (i = 0u; i < g_mini_list_n; i++) {
        if (cb(g_mini_list[i], arg) != 0) {
            return 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------ audit */

static arkrtos_audit_result_t g_audit;
static uint32 g_audit_hide_name_bit;

void arkrtos_audit_run(arkrtos_audit_result_t *p_out)
{
    *p_out = g_audit;
}

const char *arkrtos_audit_name(uint32 bit)
{
    if (bit == g_audit_hide_name_bit) {
        return NULL;
    }
    switch (bit) {
    case ARKRTOS_AUDIT_PARTITION:  return "partition";
    case ARKRTOS_AUDIT_LAYOUT:     return "layout";
    case ARKRTOS_AUDIT_SLOTS:      return "slots";
    case ARKRTOS_AUDIT_SLOT_RAM:   return "slot-ram";
    case ARKRTOS_AUDIT_SLOT_FLASH: return "slot-flash";
    case ARKRTOS_AUDIT_TASKS:      return "tasks";
    case ARKRTOS_AUDIT_SCHED:      return "sched";
    default: return NULL;
    }
}

/* ------------------------------------------------------------ dt */

static arkrtos_dt_node_t g_dt_nodes[4];

uint32 arkrtos_dt_count(void)
{
    return 4u;
}

const arkrtos_dt_node_t *arkrtos_dt_node(uint32 idx)
{
    if (idx == 2u) {
        return NULL;              /* a hole: the loop must skip it */
    }
    if (idx < 4u) {
        return &g_dt_nodes[idx];
    }
    return NULL;
}

const char *arkrtos_dt_model(void)
{
    return "ark,qemu-test";
}

/* ------------------------------------------------------------ fdt */

static int g_fdt_ready_v;
static const char *g_fdt_model_v;
static arkrtos_fdt_node_t g_fdt_nodes[4];
static const char *g_fdt_compat[4];
static uint32 g_fdt_regs[4][2];
static int g_fdt_enabled[4];
static uint32 g_fdt_n;
static uint32 g_fdt_walk_i;
static int32 g_fdt_walk_end_rc;

static int32 g_path_rc;
static arkrtos_fdt_node_t g_path_node;
static const char *g_path_compat;
static uint32 g_path_reg[2];
static int g_path_enabled;

static int32 g_ph_rc;
static const char *g_ph_path;

static const char *g_stdout_str;
static const void *g_stdout_raw_p;
static uint32 g_stdout_raw_len;
static int32 g_stdout_rc;
static arkrtos_fdt_console_t g_stdout_c;

static const void *g_blob_p;
static uint32 g_blob_size;

static const void *g_prop_p;
static uint32 g_prop_len;
static uint32 g_prop_cells[4];

static int32 g_overlay_rc;
static const char *g_overlay_reason;
static uint32 g_overlay_out_len;
static const void *g_overlay_frag;
static uint32 g_overlay_frag_len;

static int32 g_load_rc;
static int32 g_vfs_read_rc;
static int32 g_vfs_write_rc;

int arkrtos_fdt_ready(void)
{
    return g_fdt_ready_v;
}

uint32 arkrtos_fdt_node_count(void)
{
    return g_fdt_n;
}

const char *arkrtos_fdt_model(void)
{
    return g_fdt_model_v;
}

int arkrtos_fdt_iter_init(arkrtos_fdt_iter_t *it)
{
    it->off = 0u;
    it->depth = 0u;
    g_fdt_walk_i = 0u;
    return 0;
}

int arkrtos_fdt_next(arkrtos_fdt_iter_t *it, arkrtos_fdt_node_t *out)
{
    (void)it;
    if (g_fdt_walk_i < g_fdt_n) {
        *out = g_fdt_nodes[g_fdt_walk_i];
        g_fdt_walk_i++;
        return 1;
    }
    return (int)g_fdt_walk_end_rc;
}

/* The shell hands the helpers a *copy* of the node (its own local struct),
 * so pointer identity never matches; look the row up by name instead.  A
 * name that is not in the walk table is the single node the path probe
 * returns, and that one reads its own globals. */
static int fdt_node_index(const arkrtos_fdt_node_t *node)
{
    int i;

    if (node->name == NULL) {
        return -1;
    }
    for (i = 0; i < 4; i++) {
        if ((g_fdt_nodes[i].name != NULL) &&
            (strcmp(g_fdt_nodes[i].name, node->name) == 0)) {
            return i;
        }
    }
    return -1;
}

const char *arkrtos_fdt_prop_str(const arkrtos_fdt_node_t *node,
                                 const char *name)
{
    int i = fdt_node_index(node);

    if (strcmp(name, "compatible") != 0) {
        return NULL;
    }
    if (i < 0) {
        return g_path_compat;
    }
    return g_fdt_compat[i];
}

int32 arkrtos_fdt_prop_u32(const arkrtos_fdt_node_t *node, const char *name,
                           uint32 idx, uint32 *out)
{
    int i = fdt_node_index(node);

    if (strcmp(name, "cells") == 0) {
        if (idx >= 4u) {
            return -1;
        }
        *out = g_prop_cells[idx];
        return 0;
    }
    if ((strcmp(name, "reg") != 0) || (idx > 1u)) {
        return -1;
    }
    if (i < 0) {
        *out = g_path_reg[idx];
        return 0;
    }
    *out = g_fdt_regs[i][idx];
    return 0;
}

int arkrtos_fdt_node_enabled(const arkrtos_fdt_node_t *node)
{
    int i = fdt_node_index(node);

    return (i < 0) ? g_path_enabled : g_fdt_enabled[i];
}

int arkrtos_fdt_node_by_path(const char *path, arkrtos_fdt_node_t *out)
{
    (void)path;
    if (g_path_rc != ARKRTOS_FDT_OK) {
        return (int)g_path_rc;
    }
    *out = g_path_node;
    return ARKRTOS_FDT_OK;
}

int arkrtos_fdt_path_of_phandle(uint32 ph, char *buf, uint32 cap)
{
    (void)ph;
    if (g_ph_rc != ARKRTOS_FDT_OK) {
        return (int)g_ph_rc;
    }
    snprintf(buf, (size_t)cap, "%s", g_ph_path);
    return ARKRTOS_FDT_OK;
}

const char *arkrtos_fdt_stdout_path(void)
{
    return g_stdout_str;
}

const void *arkrtos_fdt_stdout_raw(uint32 *len)
{
    *len = g_stdout_raw_len;
    return g_stdout_raw_p;
}

int arkrtos_fdt_stdout_resolve(arkrtos_fdt_console_t *out)
{
    if (g_stdout_rc != ARKRTOS_FDT_OK) {
        return (int)g_stdout_rc;
    }
    *out = g_stdout_c;
    return ARKRTOS_FDT_OK;
}

const void *arkrtos_fdt_blob(void)
{
    return g_blob_p;
}

uint32 arkrtos_fdt_size(void)
{
    return g_blob_size;
}

int arkrtos_fdt_load(void *dst, uint32 cap, const void *src, uint32 len)
{
    (void)dst;
    (void)cap;
    (void)src;
    (void)len;
    return (int)g_load_rc;
}

const void *arkrtos_fdt_prop(const arkrtos_fdt_node_t *node, const char *name,
                             uint32 *len)
{
    (void)node;
    (void)name;
    *len = g_prop_len;
    return g_prop_p;
}

/* Declared here, not pulled in: arkrtos_fdt_overlay.h is included by the
 * shell file only, and an implicit-declaration warning in the test would be
 * a warn_gate failure. */
int arkrtos_fdt_overlay(void *dst, uint32 cap,
                        const void *base, uint32 base_len,
                        const void *frag, uint32 frag_len,
                        uint32 *out_len);
const char *arkrtos_fdt_overlay_reason(int rc);

int arkrtos_fdt_overlay(void *dst, uint32 cap, const void *base, uint32 base_len,
                        const void *frag, uint32 frag_len, uint32 *out_len)
{
    (void)dst;
    (void)cap;
    (void)base;
    (void)base_len;
    (void)frag;
    (void)frag_len;
    if (g_overlay_rc != ARKRTOS_FDT_OK) {
        return (int)g_overlay_rc;
    }
    *out_len = g_overlay_out_len;
    return ARKRTOS_FDT_OK;
}

const char *arkrtos_fdt_overlay_reason(int rc)
{
    (void)rc;
    return g_overlay_reason;
}

const void *arkrtos_board_fdt_overlay_demo(uint32 *size)
{
    *size = g_overlay_frag_len;
    return g_overlay_frag;
}

int32 arkrtos_vfs_read_file(const char *path, uint8 *buf, uint32 cap, uint32 *out)
{
    (void)path;
    (void)buf;
    (void)cap;
    if (g_vfs_read_rc != 0) {
        return g_vfs_read_rc;
    }
    *out = 64u;
    return 0;
}

int32 arkrtos_vfs_write_file(const char *path, const uint8 *buf, uint32 len)
{
    (void)path;
    (void)buf;
    (void)len;
    return g_vfs_write_rc;
}

/* ------------------------------------------------------------ fault */

static arkrtos_fault_record_t g_fr[3];
static int32 g_fr_count;

int32 arkrtos_fault_record_count_internal(void)
{
    return g_fr_count;
}

const arkrtos_fault_record_t *arkrtos_fault_record_get(int32 index)
{
    if ((index < 0) || (index >= 3)) {
        return NULL;
    }
    if ((index == 0) && (g_fr_count >= 2)) {
        return NULL;              /* a hole: the loop must skip it */
    }
    return &g_fr[index];
}

/* ------------------------------------------------------------ sync */

static arkrtos_sync_dbg_row_t g_rows[3][3];
static uint32 g_sync_n[3];
static int g_sync_bad = -1;

uint32 arkrtos_sync_sem_num(void)
{
    return g_sync_n[0];
}

uint32 arkrtos_sync_mtx_num(void)
{
    return g_sync_n[1];
}

uint32 arkrtos_sync_cond_num(void)
{
    return g_sync_n[2];
}

static int32 sync_debug(int which, int32 idx, arkrtos_sync_dbg_row_t *out)
{
    if ((idx < 0) || ((uint32)idx >= g_sync_n[which]) ||
        (idx == g_sync_bad)) {
        return -1;
    }
    *out = g_rows[which][idx];
    return 0;
}

int32 arkrtos_sync_sem_debug(int32 idx, arkrtos_sync_dbg_row_t *out)
{
    return sync_debug(0, idx, out);
}

int32 arkrtos_sync_mtx_debug(int32 idx, arkrtos_sync_dbg_row_t *out)
{
    return sync_debug(1, idx, out);
}

int32 arkrtos_sync_cond_debug(int32 idx, arkrtos_sync_dbg_row_t *out)
{
    return sync_debug(2, idx, out);
}

static uint32 g_trace[4];
static uint32 g_handoff[3];
static uint32 g_qbefore;

void arkrtos_sync_sem_trace(int32 idx, uint32 *posts, uint32 *takes,
                            uint32 *queued, uint32 *last_waiter)
{
    (void)idx;
    *posts = g_trace[0];
    *takes = g_trace[1];
    *queued = g_trace[2];
    *last_waiter = g_trace[3];
}

void arkrtos_sync_sem_handoff(int32 idx, uint32 *woke, uint32 *reason,
                              uint32 *pendsv_also)
{
    (void)idx;
    *woke = g_handoff[0];
    *reason = g_handoff[1];
    *pendsv_also = g_handoff[2];
}

uint32 arkrtos_sync_sem_qbefore(int32 idx)
{
    (void)idx;
    return g_qbefore;
}

volatile uint32 arkrtos_diag_sync_ghost;
volatile uint32 arkrtos_diag_sync_dup;
volatile uint32 dbg_sem_ghost_cnt;
volatile uint32 dbg_sem_ghost_idx;
volatile uint32 dbg_sem_ghost_tid;
volatile uint32 dbg_sem_to_cnt;
volatile uint32 dbg_sem_appq_hit;

/* ------------------------------------------------------------ log / fifo */

static uint32 g_log_level_v;

void arkrtos_log_set_level(uint32 level)
{
    g_log_level_v = level;
}

uint32 arkrtos_log_get_level(void)
{
    return g_log_level_v;
}

static uint32 g_console_busy;
static uint32 g_console_drop;
static uint32 g_fifo_refused;
static uint32 g_fifo_bad;
static uint32 g_fifo_bad_bytes;

uint32 arkrtos_console_busy_count(void)
{
    return g_console_busy;
}

uint32 arkrtos_console_tx_drop_count(void)
{
    return g_console_drop;
}

uint32 arkrtos_fifo_write_refused(void)
{
    return g_fifo_refused;
}

uint32 arkrtos_fifo_bad_magic(void)
{
    return g_fifo_bad;
}

uint32 arkrtos_fifo_bad_magic_bytes(void)
{
    return g_fifo_bad_bytes;
}

/* ------------------------------------------------------------ rt */

static uint32 g_per_tick;
static uint32 g_cycle_clock;
static int32 g_clock_measured;
static int32 g_counter_alive;
static arkrtos_rt_row_t g_rt_rows[ARKRTOS_RT_ROWS];
static uint32 g_rt_rows_n;
static int32 g_jitter_rc;
static int32 g_jitter_min, g_jitter_max;
static uint32 g_jitter_n;
static int32 g_rt_last_err;
static int32 g_wake_rc;
static int32 g_preempt_rc;
static uint32 g_tl_isrs, g_tl_sleeps;
static uint32 g_kernel_tick;

void arkrtos_rt_init(void) { }
void arkrtos_rt_reset(void) { }

int32 arkrtos_rt_counter_alive(void)
{
    return g_counter_alive;
}

uint32 arkrtos_rt_cycles_per_tick(void)
{
    return g_per_tick;
}

uint32 arkrtos_rt_to_us(uint32 c)
{
    return c / 2u;
}

uint32 arkrtos_port_get_cycle_clock(void)
{
    return g_cycle_clock;
}

int32 arkrtos_port_cycle_clock_measured(void)
{
    return g_clock_measured;
}

uint32 arkrtos_rt_snapshot(arkrtos_rt_row_t *rows, uint32 max_rows)
{
    uint32 i;

    for (i = 0u; (i < g_rt_rows_n) && (i < max_rows); i++) {
        rows[i] = g_rt_rows[i];
    }
    return (g_rt_rows_n < max_rows) ? g_rt_rows_n : max_rows;
}

uint32 arkrtos_rt_jitter(int32 *min_c, int32 *max_c, uint32 *n)
{
    if (g_jitter_rc == 0) {
        return 0u;
    }
    *min_c = g_jitter_min;
    *max_c = g_jitter_max;
    *n = g_jitter_n;
    return 1u;
}

int32 arkrtos_rt_measure_wake(uint32 n)
{
    (void)n;
    return g_wake_rc;
}

int32 arkrtos_rt_measure_preempt(uint32 n)
{
    (void)n;
    return g_preempt_rc;
}

int32 arkrtos_rt_last_error(void)
{
    return g_rt_last_err;
}

void arkrtos_tickless_stats(uint32 *isrs, uint32 *sleeps)
{
    *isrs = g_tl_isrs;
    *sleeps = g_tl_sleeps;
}

uint32 arkrtos_kernel_get_tick(void)
{
    return g_kernel_tick;
}

/* ------------------------------------------------------------ sec */

static arkrtos_sec_report_t g_sec_rep;
static uint32 g_sec_fails;

uint32 arkrtos_sec_run(arkrtos_sec_report_t *p_out)
{
    *p_out = g_sec_rep;
    return g_sec_fails;
}

const char *arkrtos_sec_state_name(uint32 state)
{
    switch (state) {
    case ARKRTOS_SEC_PASS:    return "pass";
    case ARKRTOS_SEC_FAIL:    return "fail";
    case ARKRTOS_SEC_FINDING: return "finding";
    case ARKRTOS_SEC_SKIP:    return "skip";
    default: return "?";
    }
}

/* ------------------------------------------------------------ utest */

static int g_utest_out_set;
static uint32 g_utest_failed;
static ark_utest_out_t g_utest_out;

void ark_utest_set_output(ark_utest_out_t out)
{
    g_utest_out = out;
    g_utest_out_set = 1;
}

uint32_t ark_utest_list(void)
{
    if (g_utest_out != NULL) {
        g_utest_out("case one");
        g_utest_out("case two");
    }
    return 0u;
}

uint32_t ark_utest_run(const char *pattern, ark_utest_summary_t *out)
{
    (void)pattern;
    (void)out;
    if (g_utest_out != NULL) {
        g_utest_out("ran");
    }
    return (uint32_t)g_utest_failed;
}

/* ------------------------------------------------------------ pool */

static uint32 g_pool_total;
static uint32 g_pool_largest;
static arkrtos_cfg_slot_t g_ls[2];
static uint32 g_ls_n;

uint32 arkrtos_loader_pool_free(uint32 *p_largest)
{
    *p_largest = g_pool_largest;
    return g_pool_total;
}

const arkrtos_cfg_slot_t *arkrtos_layout_slots(void)
{
    return g_ls;
}

uint32 arkrtos_layout_slot_count(void)
{
    return g_ls_n;
}

/* ============================================================ fixtures */

static void reset_all(void)
{
    uint32 i;

    cap_reset();

    memset(&g_pt, 0, sizeof(g_pt));
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    memset(g_si_rc, 0, sizeof(g_si_rc));
    memset(g_si3, 0, sizeof(g_si3));
    memset(g_raw_rc, 0, sizeof(g_raw_rc));
    memset(g_raw4, 0, sizeof(g_raw4));
    arkrtos_task_count = 0;
    g_ctx_rc = -1;

    g_ready_count = 0u;
    g_ready_top_v = -1;
    g_sched_bad = 0;
    memset(g_ready_on, 0, sizeof(g_ready_on));

    memset(&g_guard_st, 0, sizeof(g_guard_st));
    memset(g_guard_t, 0, sizeof(g_guard_t));
    g_guard_slots = 0u;

    memset(g_crash_cnt, 0, sizeof(g_crash_cnt));
    memset(g_crash_hold, 0, sizeof(g_crash_hold));
    memset(g_crash_last, 0, sizeof(g_crash_last));
    memset(g_crash_tick, 0, sizeof(g_crash_tick));
    g_crash_boots_v = 0u;
    g_crash_seq_v = 0u;
    memset(g_mini_used, 0, sizeof(g_mini_used));
    g_mini_list_n = 0u;
    for (i = 0u; i < 4u; i++) {
        g_mini_list[i] = NULL;
    }

    memset(&g_audit, 0, sizeof(g_audit));
    g_audit_hide_name_bit = 0u;

    memset(g_dt_nodes, 0, sizeof(g_dt_nodes));

    g_fdt_ready_v = 0;
    g_fdt_model_v = NULL;
    memset(g_fdt_nodes, 0, sizeof(g_fdt_nodes));
    for (i = 0u; i < 4u; i++) {
        g_fdt_compat[i] = NULL;
        g_fdt_regs[i][0] = 0u;
        g_fdt_regs[i][1] = 0u;
        g_fdt_enabled[i] = 0;
    }
    g_fdt_n = 0u;
    g_fdt_walk_i = 0u;
    g_fdt_walk_end_rc = 0;
    g_path_rc = ARKRTOS_FDT_OK;
    g_path_compat = NULL;
    g_path_reg[0] = 0u;
    g_path_reg[1] = 0u;
    g_path_enabled = 1;
    g_ph_rc = ARKRTOS_FDT_OK;
    g_ph_path = "/soc/uart@1000";
    g_stdout_str = NULL;
    g_stdout_raw_p = NULL;
    g_stdout_raw_len = 0u;
    g_stdout_rc = ARKRTOS_FDT_OK;
    memset(&g_stdout_c, 0, sizeof(g_stdout_c));
    g_blob_p = NULL;
    g_blob_size = 0u;
    g_prop_p = NULL;
    g_prop_len = 0u;
    g_overlay_rc = ARKRTOS_FDT_OK;
    g_overlay_reason = "unsupported";
    g_overlay_out_len = 512u;
    g_overlay_frag = NULL;
    g_overlay_frag_len = 0u;
    g_load_rc = ARKRTOS_FDT_OK;
    g_vfs_read_rc = 0;
    g_vfs_write_rc = 0;

    memset(g_fr, 0, sizeof(g_fr));
    g_fr_count = 0;

    memset(g_rows, 0, sizeof(g_rows));
    memset(g_sync_n, 0, sizeof(g_sync_n));
    memset(g_trace, 0, sizeof(g_trace));
    memset(g_handoff, 0, sizeof(g_handoff));
    g_qbefore = 0u;
    g_sync_bad = -1;
    arkrtos_diag_sync_ghost = 0u;
    arkrtos_diag_sync_dup = 0u;
    dbg_sem_ghost_cnt = 0u;
    dbg_sem_ghost_idx = 0u;
    dbg_sem_ghost_tid = 0u;
    dbg_sem_appq_hit = 0u;

    g_log_level_v = 3u;
    g_console_busy = 0u;
    g_console_drop = 0u;
    g_fifo_refused = 0u;
    g_fifo_bad = 0u;
    g_fifo_bad_bytes = 0u;

    g_per_tick = 1000u;
    g_cycle_clock = 1000000u;
    g_clock_measured = 1;
    g_counter_alive = 1;
    memset(g_rt_rows, 0, sizeof(g_rt_rows));
    g_rt_rows_n = 0u;
    g_jitter_rc = 0;
    g_jitter_min = 0;
    g_jitter_max = 0;
    g_jitter_n = 0u;
    g_rt_last_err = 0;
    g_wake_rc = 0;
    g_preempt_rc = 0;
    g_tl_isrs = 0u;
    g_tl_sleeps = 0u;
    g_kernel_tick = 0u;

    memset(&g_sec_rep, 0, sizeof(g_sec_rep));
    g_sec_fails = 0u;
    g_utest_out_set = 0;
    g_utest_failed = 0u;

    g_pool_total = 0u;
    g_pool_largest = 0u;
    memset(g_ls, 0, sizeof(g_ls));
    g_ls_n = 0u;

    for (i = 0u; i < 56u; i++) {
        g_frame_words[i] = 0xA5A50000u + i;
    }
    for (i = 0u; i < 2048u; i++) {
        g_stack_bottom_buf[i] = 0xCDCDCDCDu;
    }
}

/* Build a task table entry.  Ids are 1-based to the shell; the table is
 * 0-based, which is exactly the mapping the source documents. */
static void task_make(int idx, uint32 prio, int32 status, int32 period,
                      int32 wait_t, uint32 *stack_ptr, void (*entry)(void))
{
    arkrtos_task_table[idx].priority = (uint8)prio;
    arkrtos_task_table[idx].status = (arkrtos_task_status_t)status;
    arkrtos_task_table[idx].period = period;
    arkrtos_task_table[idx].wait_time = wait_t;
    arkrtos_task_table[idx].stack_ptr = (unsigned long)(uintptr_t)stack_ptr;
    arkrtos_task_table[idx].stack_bottom = g_stack_bottom_buf;
    arkrtos_task_table[idx].stack_peak_low = 0x1000u;
    arkrtos_task_table[idx].entry = entry;
}

/* ============================================================ call helpers */

static void entry_noop(void) { }

static int c0(int (*fn)(int, char *[]))
{
    char *v[1];

    v[0] = W("cmd");
    return fn(1, v);
}

static int c1(int (*fn)(int, char *[]), const char *a1)
{
    char *v[2];

    v[0] = W("cmd");
    v[1] = W(a1);
    return fn(2, v);
}

static int c2(int (*fn)(int, char *[]), const char *a1, const char *a2)
{
    char *v[3];

    v[0] = W("cmd");
    v[1] = W(a1);
    v[2] = W(a2);
    return fn(3, v);
}

static int c3(int (*fn)(int, char *[]), const char *a1, const char *a2,
              const char *a3)
{
    char *v[4];

    v[0] = W("cmd");
    v[1] = W(a1);
    v[2] = W(a2);
    v[3] = W(a3);
    return fn(4, v);
}

/* ============================================================ task */

static void t_task(void)
{
    reset_all();
    arkrtos_task_count = 2;
    task_make(0, 3u, 1, 100, 7, g_frame_words, entry_noop);
    task_make(1, 5u, 2, 0, 0, g_frame_words, entry_noop);

    CK_I(c0(cmd_task), 0);
    CK(out_has("id  prio  state     period  wait    peak/low  entry"));
    {
        char want[80];

        snprintf(want, sizeof want, "tasks: 2 used / %d max  (ids 1..2)",
                 (int)ARKRTOS_TASK_MAX_NUM);
        CK(out_has(want));
    }
    /* accounting is on in this build: the peak column prints the number */
    CK(out_has("4096"));

    cap_reset();
    CK_I(c1(cmd_task, "0"), 1);
    CK(out_has("task: no such task (ids run 1..2)"));
    cap_reset();
    CK_I(c1(cmd_task, "abc"), 1);
    cap_reset();
    CK_I(c1(cmd_task, "3"), 1);
    cap_reset();
    arkrtos_task_table[1].stack_ptr = 0u;
    CK_I(c1(cmd_task, "2"), 1);
    arkrtos_task_table[1].stack_ptr = (unsigned long)(uintptr_t)g_frame_words;

    cap_reset();
    CK_I(c1(cmd_task, "1"), 0);
    CK(out_has("task 1 sp=0x"));
    CK(out_has("frame words:"));
    CK_I(out_count("  sp+"), 56);

    cap_reset();
    arkrtos_dbg_wake_hits[0] = 7u;
    arkrtos_dbg_wake_last[0] = 4u;
    CK_I(c2(cmd_task, "1", "why"), 0);
    CK(out_has("  blocks=7 last_reason=4"));
}

/* ============================================================ stack */

static void stack_mixed_profile(void)
{
    reset_all();
    arkrtos_task_count = 4;
    task_make(0, 3u, 1, 0, 0, g_frame_words, entry_noop);
    task_make(1, 3u, 1, 0, 0, g_frame_words, entry_noop);
    task_make(2, 3u, 1, 0, 0, g_frame_words, entry_noop);
    task_make(3, 3u, 1, 0, 0, g_frame_words, entry_noop);
    g_si_rc[0] = -1;                       /* query refused -> n/a */
    g_si3[1][0] = 4096u;                   /* used 0 -> "0?" */
    g_si3[1][1] = 0u;
    g_si3[1][2] = 4096u;
    g_si3[2][0] = 100u;                    /* 95% -> danger */
    g_si3[2][1] = 95u;
    g_si3[2][2] = 5u;
    g_si3[3][0] = 100u;                    /* 80% -> warn */
    g_si3[3][1] = 80u;
    g_si3[3][2] = 20u;
    g_ctx_rc = 0;
    g_ctx4[0] = 0x2000u;
    g_ctx4[1] = 0x2100u;
    g_ctx4[2] = 0x2080u;
    g_ctx4[3] = 128u;
}

static void t_stack(void)
{
    stack_mixed_profile();
    CK_I(c0(cmd_stack), 0);
    CK(out_has("n/a"));
    CK(out_has("0?"));
    CK(out_has("unmeas"));
    CK(out_has("is at 95% - raise its stack size"));
    CK(out_has("is at 80% - watch it"));
    CK(out_has("worst 95% (task 3)"));
    CK(out_has("2 unmeasured, 1 at/over 90%"));
    CK(out_has("stack ctx: idle/boot 0x2000..0x2100"));
    CK(out_has("used=128 free=128"));

    /* check reports the unmeasured count first */
    cap_reset();
    CK_I(c1(cmd_stack, "check"), 1);
    CK(out_has("check FAILED - 2 task(s) could not be measured"));

    /* check stops on the danger line when everything can be measured */
    reset_all();
    arkrtos_task_count = 1;
    task_make(0, 3u, 1, 0, 0, g_frame_words, entry_noop);
    g_si3[0][0] = 100u;
    g_si3[0][1] = 95u;
    g_si3[0][2] = 5u;
    CK_I(c1(cmd_stack, "check"), 1);
    CK(out_has("check FAILED - 1 task(s) at/over 90% of their stack"));

    /* check ok, with the idle context refused rather than silent */
    g_si3[0][1] = 10u;
    g_si3[0][2] = 90u;
    cap_reset();
    CK_I(c1(cmd_stack, "check"), 0);
    CK(out_has("check ok - 1 task(s) measured, worst 10% (task 1)"));
    CK(out_has("stack ctx: not armed (watch_context refused: alignment/size/top/bottom)"));

    /* raw: both read-out sources, one n/a row, one short stack with no
     * bottom sample */
    reset_all();
    arkrtos_task_count = 3;
    task_make(0, 3u, 1, 0, 0, g_frame_words, entry_noop);
    task_make(1, 3u, 1, 0, 0, g_frame_words, entry_noop);
    task_make(2, 3u, 1, 0, 0, g_frame_words, entry_noop);
    g_raw_rc[0] = -1;
    g_raw4[1][0] = 4096u;
    g_raw4[1][1] = 2048u;
    g_raw4[1][2] = 1024u;
    g_raw4[1][3] = 2048u;
    g_raw4[2][0] = 512u;
    g_raw4[2][1] = 100u;
    g_raw4[2][2] = 100u;
    g_raw4[2][3] = 80u;
    CK_I(c1(cmd_stack, "raw"), 0);
    CK(out_has("n/a (no stack)"));
    CK(out_has("pct_f  pct_p  entry"));
    CK(out_has("      bottom 0x"));
    CK(out_has("[1/8]=0xCDCDCDCD"));
    CK(out_has("stack raw: fill=scan of untouched fill pattern, psp=lowest "
                  "SP seen (take the larger)"));
    CK(out_has("take the larger)"));

    /* unknown option */
    cap_reset();
    CK_I(c1(cmd_stack, "x"), 1);
    CK(out_has("stack: unknown option 'x' (usage: stack [check|raw])"));
}

/* ============================================================ guard */

static void t_guard(void)
{
    reset_all();
    /* disabled by the board */
    g_guard_st.enabled = 0u;
    CK_I(c0(cmd_guard), 0);
    CK(out_has("watchdog: disabled by the board"));
    CK(out_has("note: no image declared a heartbeat period"));

    /* requested but the port refused it */
    cap_reset();
    g_guard_st.enabled = 1u;
    g_guard_st.timeout_ms = 0u;
    CK_I(c0(cmd_guard), 0);
    CK(out_has("watchdog: requested but NOT running (the port refused it)"));

    /* armed, with three slots: one empty, one full, one unknown state */
    cap_reset();
    g_guard_st.enabled = 1u;
    g_guard_st.timeout_ms = 250u;
    g_guard_st.feeds = 11u;
    g_guard_st.starves = 2u;
    g_guard_st.verdict = 3u;
    g_guard_st.audit_mask = 0x20u;
    g_guard_st.audit_runs = 4u;
    g_guard_st.declared = 2u;
    g_guard_st.violating = 1u;
    g_guard_slots = 3u;
    g_guard_t[0].task_id = 0;
    g_guard_t[0].declared = 0u;
    g_guard_t[1].task_id = 2;
    g_guard_t[1].slot = 0;
    g_guard_t[1].declared = 1u;
    g_guard_t[1].period_ms = 100u;
    g_guard_t[1].beat_age_ms = 5u;
    g_guard_t[1].svc_age_ms = 0xFFFFFFFFu;  /* "-" */
    g_guard_t[1].cpu_age_ms = 7u;
    g_guard_t[1].misses = 0u;
    g_guard_t[1].state = (uint32)ARKRTOS_TASK_RUNNING;
    g_guard_t[2].task_id = 3;
    g_guard_t[2].slot = -1;
    g_guard_t[2].declared = 1u;
    g_guard_t[2].period_ms = 200u;
    g_guard_t[2].beat_age_ms = 9u;
    g_guard_t[2].svc_age_ms = 9u;
    g_guard_t[2].cpu_age_ms = 9u;
    g_guard_t[2].misses = 1u;
    g_guard_t[2].state = 99u;               /* unknown -> "-" */
    CK_I(c0(cmd_guard), 0);
    CK(out_has("watchdog: armed, hardware timeout 250 ms"));
    CK(out_has("feeds=11 starves=2 verdict=0x3 audit_mask=0x20 audit_runs=4"));
    CK(out_has("contracts: 2 declared, 1 violating now"));
    CK(out_has("task slot period beat_ms svc_ms cpu_ms misses state"));
    CK(out_has("2 0 100 5 - 7 0 run"));
    CK(out_has("3 -1 200 9 9 9 1 -"));
    CK(!out_has("note: no image declared"));

    /* the three declared-but-not-running states print their own word, and a
     * slot the port cannot read stops the table instead of printing garbage */
    cap_reset();
    memset(g_guard_t, 0, sizeof(g_guard_t));
    g_guard_slots = 4u;
    g_guard_t[0].task_id = 1;
    g_guard_t[0].declared = 1u;
    g_guard_t[0].period_ms = 10u;
    g_guard_t[0].state = (uint32)ARKRTOS_TASK_INVALID;
    g_guard_t[1].task_id = 2;
    g_guard_t[1].declared = 1u;
    g_guard_t[1].period_ms = 20u;
    g_guard_t[1].state = (uint32)ARKRTOS_TASK_READY;
    g_guard_t[2].task_id = 3;
    g_guard_t[2].declared = 1u;
    g_guard_t[2].period_ms = 30u;
    g_guard_t[2].state = (uint32)ARKRTOS_TASK_WAIT;
    g_guard_t[3].task_id = 4;
    g_guard_t[3].declared = 1u;
    g_guard_t[3].period_ms = 40u;
    g_guard_t[3].state = (uint32)ARKRTOS_TASK_RUNNING;
    g_guard_fail_at = 3;
    CK_I(c0(cmd_guard), 0);
    CK(out_has("1 0 10 0 0 0 0 invalid"));
    CK(out_has("2 0 20 0 0 0 0 ready"));
    CK(out_has("3 0 30 0 0 0 0 wait"));
    CK(!out_has("4 0 40"));
    g_guard_fail_at = -1;
}

/* ============================================================ crash */

static void t_crash(void)
{
    reset_all();
    g_pt.slot_max = 3u;
    CK_I(c0(cmd_crash), 0);
    CK(out_has("journal: 0x2001F000 +512 bytes"));
    CK(out_has("no slot has a fault on record since the last power-on."));
    CK(out_has("no MiniApp has a fault on record since the last power-on."));

    /* one live slot, one empty slot skipped, two MiniApp entries: one whose
     * hash matches a path the autostart list still names, one that does not */
    cap_reset();
    g_crash_boots_v = 9u;
    g_crash_seq_v = 12u;
    g_pt.slot_max = 2u;
    g_pt.slot_type[0] = 1u;
    g_pt.slot_type[1] = 2u;
    g_crash_cnt[0] = 2u;
    g_crash_hold[0] = 1u;
    g_crash_last[0] = 2u;
    g_crash_tick[0] = 99u;
    g_crash_cnt[1] = 0u;
    g_crash_hold[1] = 0u;

    g_mini_used[0] = 1u;
    g_mini_hash[0] = arkrtos_crash_hash_path("a.mini");
    g_mini_cnt[0] = 3u;
    g_mini_last[0] = 1u;
    g_mini_hold[0] = 2u;
    g_mini_tick[0] = 50u;
    g_mini_used[1] = 1u;
    g_mini_hash[1] = 0xDEADBEEFu;
    g_mini_cnt[1] = 1u;
    g_mini_last[1] = 0u;
    g_mini_hold[1] = 0u;
    g_mini_tick[1] = 0u;
    g_mini_list[0] = "x.mini";
    g_mini_list[1] = "a.mini";
    g_mini_list_n = 2u;

    CK_I(c0(cmd_crash), 0);
    CK(out_has("boots=9 seq=12"));
    CK(out_has(" 0   app   2       heartbeat   fault     99"));
    CK(out_count(" 1   drv") == 0);
    CK(out_has("hold = the kernel will not autostart this slot"));
    CK(out_has(" a.mini      3       fault       heartbeat  50"));
    CK(out_has(" #DEADBEEF    1       none        none      0"));
    CK(out_has("path is known only while /mini.autostart still names it"));
}

/* ============================================================ audit */

static void t_audit(void)
{
    reset_all();
    g_audit.checks = 40u;
    g_audit.index = ARKRTOS_AUDIT_INDEX_NONE;
    g_audit_hide_name_bit = ARKRTOS_AUDIT_SLOTS;
    CK_I(c0(cmd_audit), 0);
    CK(out_has("  partition: ok"));
    CK(out_has("  sched: ok"));
    CK(out_has("checks=40 bad_groups=0 mask=0x00000000 first_offender=4294967295"));
    CK(out_has("audit: all structures self-consistent"));
    CK(out_count("  slots:") == 0);          /* unknown name -> skipped */

    cap_reset();
    g_audit.mask = ARKRTOS_AUDIT_PARTITION | ARKRTOS_AUDIT_TASKS;
    g_audit.index = 2u;
    CK_I(c0(cmd_audit), 2);
    CK(out_has("  partition: BAD"));
    CK(out_has("  tasks: BAD"));
    CK(out_has("bad_groups=2 mask=0x00000021 first_offender=2"));
    CK(out_has("audit: INCONSISTENT (see the group lines above)"));
}

/* ============================================================ sched */

static void t_sched(void)
{
    reset_all();
    g_ready_count = 3u;
    g_ready_top_v = 0;
    CK_I(c0(cmd_sched), 0);
    CK(out_has("ready=3 top=0"));
    CK(out_has("sched: consistent (bitmap/links == task table)"));

    /* two mismatches, one in each direction */
    cap_reset();
    arkrtos_task_count = 2;
    task_make(0, 3u, (int32)ARKRTOS_TASK_READY, 0, 0, g_frame_words, entry_noop);
    task_make(1, 5u, (int32)ARKRTOS_TASK_WAIT, 0, 0, g_frame_words, entry_noop);
    g_ready_on[0] = 0u;                      /* ready but not on the chain */
    g_ready_on[1] = 1u;                      /* on the chain but waiting */
    g_sched_bad = 2;
    CK_I(c0(cmd_sched), 2);
    CK(out_has("#0 prio=3 status=1 on_ready=0 next=0 prev=0"));
    CK(out_has("#1 prio=5 status=2 on_ready=1 next=0 prev=0"));
    CK(out_has("sched: INCONSISTENT, 2 mismatch(es)"));
}

/* ============================================================ dt */

static void t_dt(void)
{
    reset_all();
    g_dt_nodes[0].name = "cpus";
    g_dt_nodes[0].is_device = 0u;
    g_dt_nodes[1].name = "uart0";
    g_dt_nodes[1].compatible = "ns16550a";
    g_dt_nodes[1].reg_base = 0x10000000u;
    g_dt_nodes[1].reg_size = 0x100u;
    g_dt_nodes[1].status_okay = 1u;
    g_dt_nodes[1].is_device = 1u;
    g_dt_nodes[3].name = "spi0";
    g_dt_nodes[3].compatible = "ark,spi";
    g_dt_nodes[3].reg_base = 0x2000u;
    g_dt_nodes[3].reg_size = 0x40u;
    g_dt_nodes[3].status_okay = 0u;
    g_dt_nodes[3].is_device = 1u;

    CK_I(c0(cmd_dt), 0);
    CK(out_has("model=ark,qemu-test nodes=4"));
    CK(out_count("  uart0 compat=ns16550a reg=0x10000000/0x100 okay") == 1);
    CK(out_has("  spi0 compat=ark,spi reg=0x2000/0x40 disabled"));
    CK(!out_has("cpus"));
}

/* ============================================================ fdt */

static void t_fdt(void)
{
    const char prop_list[] = "a\0b";
    static char raw_cell[4] = { 1, 0, 0, 0 };
    static char prop_tail[10] = { 'a', 0, 0, 0, 'b', 0, 0, 0, 'c', 1 };
    static char prop_ctl[4] = { 'a', 1, 0, 0 };

    /* no blob parsed at boot: every subcommand has to say so, not print an
     * empty tree that reads like a board without devices */
    reset_all();
    CK_I(c0(cmd_fdt), 1);
    CK(out_has("fdt: no device tree blob parsed at boot"));

    /* whole-tree dump: a node without compatible is skipped, a device is
     * printed, and the walk ending on an error is reported with its code */
    cap_reset();
    g_fdt_ready_v = 1;
    g_fdt_model_v = NULL;
    g_fdt_n = 2u;
    g_fdt_nodes[0].name = "cpus";
    g_fdt_compat[0] = NULL;
    g_fdt_nodes[1].name = "uart0";
    g_fdt_nodes[1].depth = 0u;
    g_fdt_nodes[1].n_props = 2u;
    g_fdt_compat[1] = "ns16550a";
    g_fdt_regs[1][0] = 0x10000000u;
    g_fdt_regs[1][1] = 0x100u;
    g_fdt_enabled[1] = 1;
    g_fdt_walk_end_rc = 0;
    CK_I(c0(cmd_fdt), 0);
    CK(out_has("model=(none) nodes=2"));
    CK(out_count("  uart0 compat=ns16550a reg=0x10000000/0x100 okay") == 1);
    CK(!out_has("cpus"));

    cap_reset();
    g_fdt_walk_end_rc = -3;
    CK_I(c0(cmd_fdt), 1);
    CK(out_has("fdt: walk stopped by error -3"));

    /* path: usage, a refused lookup, then a node with and without compat */
    cap_reset();
    CK_I(c1(cmd_fdt, "path"), 1);
    CK(out_has("fdt path <absolute path>"));

    cap_reset();
    g_path_rc = -4;
    CK_I(c2(cmd_fdt, "path", "/soc/nope"), 1);
    CK(out_has("fdt: path /soc/nope rc=-4"));

    cap_reset();
    g_path_rc = ARKRTOS_FDT_OK;
    g_path_node.name = "uart@1000";
    g_path_node.depth = 1u;
    g_path_node.n_props = 3u;
    g_path_compat = "ns16550a";
    g_path_reg[0] = 0x1000u;
    g_path_reg[1] = 0x20u;
    g_path_enabled = 0;
    CK_I(c2(cmd_fdt, "path", "/soc/uart@1000"), 0);
    CK(out_has("path=/soc/uart@1000 name=uart@1000 depth=1 props=3 disabled"));
    CK(out_has("  compat=ns16550a reg=0x1000/0x20"));

    cap_reset();
    g_path_compat = NULL;
    g_path_enabled = 1;
    CK_I(c2(cmd_fdt, "path", "/soc/uart@1000"), 0);
    CK(out_has("props=3 okay"));
    CK(out_has("  compat=(none) reg=0x1000/0x20"));

    /* phandle: usage (missing and non-numeric), refused, resolved */
    cap_reset();
    CK_I(c1(cmd_fdt, "phandle"), 1);
    CK(out_has("fdt phandle <decimal>"));

    cap_reset();
    CK_I(c2(cmd_fdt, "phandle", "zz"), 1);
    CK(out_has("fdt phandle <decimal>"));

    cap_reset();
    g_ph_rc = -5;
    CK_I(c2(cmd_fdt, "phandle", "7"), 1);
    CK(out_has("fdt: phandle 7 rc=-5"));

    cap_reset();
    g_ph_rc = ARKRTOS_FDT_OK;
    g_ph_path = "/soc/serial@1fe001e0";
    CK_I(c2(cmd_fdt, "phandle", "7"), 0);
    CK(out_has("phandle 7 -> /soc/serial@1fe001e0"));

    /* chosen: absent property */
    cap_reset();
    g_stdout_raw_p = NULL;
    CK_I(c1(cmd_fdt, "chosen"), 1);
    CK(out_has("chosen stdout-path=(absent)"));

    /* chosen: string form, resolved (parity absent prints '-') */
    cap_reset();
    g_stdout_raw_p = raw_cell;
    g_stdout_raw_len = 4u;
    g_stdout_str = "serial0:115200n8";
    g_stdout_rc = ARKRTOS_FDT_OK;
    snprintf(g_stdout_c.path, sizeof(g_stdout_c.path), "%s", "/soc/serial@1fe001e0");
    g_stdout_c.baud = 115200u;
    g_stdout_c.bits = 8u;
    g_stdout_c.parity = 0;
    g_stdout_c.from_alias = 1u;
    CK_I(c1(cmd_fdt, "chosen"), 0);
    CK(out_has("chosen stdout-path=\"serial0:115200n8\""));
    CK(out_has("  path=/soc/serial@1fe001e0 baud=115200 bits=8 parity=- alias=1"));

    /* chosen: cell form, resolve refused -> says NOT applied */
    cap_reset();
    g_stdout_str = NULL;
    g_stdout_raw_len = 4u;
    g_stdout_rc = -8;
    CK_I(c1(cmd_fdt, "chosen"), 1);
    CK(out_has("chosen stdout-path=<4 byte cell>"));
    CK(out_has("  resolve rc=-8 -> NOT applied, board default kept"));

    /* chosen: cell form that does resolve, parity present */
    cap_reset();
    g_stdout_rc = ARKRTOS_FDT_OK;
    g_stdout_c.parity = 'n';
    CK_I(c1(cmd_fdt, "chosen"), 0);
    CK(out_has("chosen stdout-path=<4 byte cell>"));
    CK(out_has("parity=n alias=1"));

    /* get: usage, refused path, missing property, string, string list,
     * cells with a non-cell tail */
    cap_reset();
    CK_I(c2(cmd_fdt, "get", "/soc"), 1);
    CK(out_has("fdt get <absolute path> <property>"));

    cap_reset();
    g_path_rc = -6;
    CK_I(c3(cmd_fdt, "get", "/soc", "model"), 1);
    CK(out_has("fdt: path /soc rc=-6"));

    cap_reset();
    g_path_rc = ARKRTOS_FDT_OK;
    g_prop_p = NULL;
    CK_I(c3(cmd_fdt, "get", "/soc", "model"), 1);
    CK(out_has("fdt: /soc has no property 'model'"));

    cap_reset();
    g_prop_p = "qemu-la64";
    g_prop_len = 10u;
    CK_I(c3(cmd_fdt, "get", "/soc", "model"), 0);
    CK(out_has("/soc model len=10 str=\"qemu-la64\""));

    cap_reset();
    g_prop_p = prop_list;
    g_prop_len = 4u;
    CK_I(c3(cmd_fdt, "get", "/soc", "compatible"), 0);
    CK(out_has("/soc compatible len=4 str=\"a\",\"b\""));

    cap_reset();
    g_prop_p = prop_tail;
    g_prop_len = 10u;
    g_prop_cells[0] = 7u;
    g_prop_cells[1] = 0x20u;
    CK_I(c3(cmd_fdt, "get", "/soc", "cells"), 0);
    CK(out_has("/soc cells len=10 cells=0x7 0x20 (tail 2 byte(s) not cells)"));

    /* a NUL-terminated value that still carries a control byte: not a string */
    cap_reset();
    g_prop_p = prop_ctl;
    g_prop_len = 4u;
    g_prop_cells[0] = 0x61u;
    CK_I(c3(cmd_fdt, "get", "/soc", "cells"), 0);
    CK(out_has("/soc cells len=4 cells=0x61"));

    /* overlay: fragment not built in, refused with its reason, merged but
     * rejected by init, then applied */
    cap_reset();
    g_overlay_frag = NULL;
    g_overlay_frag_len = 0u;
    CK_I(c1(cmd_fdt, "overlay"), 1);
    CK(out_has("fdt: overlay demo fragment is not built into this image"));

    cap_reset();
    g_overlay_frag = raw_cell;
    g_overlay_frag_len = 8u;
    g_overlay_rc = -6;
    g_overlay_reason = "unsupported shape";
    g_fdt_n = 5u;
    CK_I(c1(cmd_fdt, "overlay"), 1);
    CK(out_has("fdt: overlay rc=-6 (unsupported shape) -> refused, tree kept (nodes=5)"));

    cap_reset();
    g_overlay_rc = ARKRTOS_FDT_OK;
    g_overlay_out_len = 512u;
    g_load_rc = -1;
    CK_I(c1(cmd_fdt, "overlay"), 1);
    CK(out_has("fdt: overlay merged 512 bytes but init rc=-1 -> tree kept"));

    cap_reset();
    g_load_rc = ARKRTOS_FDT_OK;
    g_fdt_model_v = NULL;
    CK_I(c1(cmd_fdt, "overlay"), 0);
    CK(out_has("fdt: overlay ok bytes=512 nodes=5 (was 5) model=(none)"));

    /* save: usage, no tree, written, write refused */
    cap_reset();
    CK_I(c1(cmd_fdt, "save"), 1);
    CK(out_has("fdt save <path>"));

    cap_reset();
    g_blob_p = NULL;
    CK_I(c2(cmd_fdt, "save", "/dtb.bin"), 1);
    CK(out_has("fdt: no tree parsed, nothing to save"));

    cap_reset();
    g_blob_p = raw_cell;
    g_blob_size = 64u;
    g_vfs_write_rc = 0;
    CK_I(c2(cmd_fdt, "save", "/dtb.bin"), 0);
    CK(out_has("fdt: save /dtb.bin rc=0 bytes=64"));

    cap_reset();
    g_vfs_write_rc = -2;
    CK_I(c2(cmd_fdt, "save", "/dtb.bin"), 1);
    CK(out_has("fdt: save /dtb.bin rc=-2 bytes=64"));

    /* load: usage, read refused, blob refused (previous tree kept), applied */
    cap_reset();
    CK_I(c1(cmd_fdt, "load"), 1);
    CK(out_has("fdt load <path>"));

    cap_reset();
    g_vfs_read_rc = -3;
    g_fdt_n = 4u;
    CK_I(c2(cmd_fdt, "load", "/dtb.bin"), 1);
    CK(out_has("fdt: load /dtb.bin read rc=-3 (tree untouched, nodes=4)"));

    cap_reset();
    g_vfs_read_rc = 0;
    g_load_rc = -2;
    CK_I(c2(cmd_fdt, "load", "/dtb.bin"), 1);
    CK(out_has("fdt: load /dtb.bin rc=-2 bytes=64 -> refused, previous tree kept (nodes=4)"));

    cap_reset();
    g_load_rc = ARKRTOS_FDT_OK;
    CK_I(c2(cmd_fdt, "load", "/dtb.bin"), 0);
    CK(out_has("fdt: load /dtb.bin rc=0 bytes=64 nodes=4 (was 4)"));

    /* unknown subcommand */
    cap_reset();
    CK_I(c1(cmd_fdt, "bogus"), 1);
    CK(out_has("fdt: unknown subcommand (path|phandle|chosen|get|overlay|save|load)"));
}

/* ============================================================ fault */

static void t_fault(void)
{
    reset_all();
    CK_I(c0(cmd_fault), 0);
    CK(out_has("no fault recorded"));

    cap_reset();
    g_fr_count = 2;
    g_fr[0].type = 1u;
    g_fr[0].task_id = 1;
    g_fr[0].tick = 10u;
    g_fr[1].type = 13u;
    g_fr[1].task_id = 3;
    g_fr[1].tick = 77u;
    CK_I(c0(cmd_fault), 0);
    CK(out_has("#   tick      task  type"));
    CK(out_count("#   tick") == 1);
    CK(out_has("1   77        3     guardhb"));
    CK(!out_has("1   10        1     hardfault"));   /* entry 0 is a hole */
}

/* ============================================================ syncinfo */

static void t_syncinfo(void)
{
    reset_all();
    g_sync_n[0] = 2u;
    g_rows[0][0].used = 0u;                 /* skipped row */
    g_rows[0][1].used = 1u;
    g_rows[0][1].count = 2;
    g_rows[0][1].owner_id = 0;
    g_rows[0][1].creator_id = 4;
    g_rows[0][1].nwait = 0;                 /* "-" */
    g_sync_n[1] = 1u;
    g_rows[1][0].used = 1u;
    g_rows[1][0].owner_id = 5;
    g_rows[1][0].nwait = 2;
    g_rows[1][0].waiter_id[0] = 1u;
    g_rows[1][0].waiter_id[1] = 2u;
    g_sync_n[2] = 1u;
    g_rows[2][0].used = 1u;
    g_rows[2][0].nwait = 1;
    g_rows[2][0].waiter_id[0] = 5u;

    g_trace[0] = 11u;
    g_trace[1] = 7u;
    g_trace[2] = 3u;
    g_trace[3] = 6u;
    g_handoff[0] = 4u;
    g_handoff[1] = 4u;
    g_handoff[2] = 1u;
    g_qbefore = 2u;
    arkrtos_diag_sync_ghost = 1u;
    arkrtos_diag_sync_dup = 2u;
    dbg_sem_ghost_cnt = 3u;
    dbg_sem_ghost_idx = 4u;
    dbg_sem_ghost_tid = 5u;
    dbg_sem_appq_hit = 6u;

    CK_I(c0(cmd_syncinfo), 0);
    CK(out_has("sem  used  count  owner  creator  waiters"));
    CK(out_has(" 1   1     2      0      4        -"));
    CK(out_has("mtx  used  count  owner  creator  waiters"));
    CK(out_has(" 0   1     0      5      0        t1,t2"));
    CK(out_has("cond  used  count  owner  creator  waiters"));
    CK(out_has(" 0   1     0      0      0        t5"));
    CK(out_has("  sem 1: posts=11 takes=7 queued=3 last_waiter=t6 qbefore=2 woke=t4 reason=4"));
    CK(out_has("sync diagnostics: ghost=1 dup=2"));
    CK(out_has("sync ghost  : n=3 idx=4 tid=t5"));
    CK(out_has("sync appwait: hit=6"));

    cap_reset();
    g_sync_n[0] = 3u;
    g_sync_n[1] = 2u;
    g_sync_n[2] = 2u;
    g_sync_bad = 1;
    CK_I(c0(cmd_syncinfo), 0);
    CK(out_has("sem  used  count  owner  creator  waiters"));
    g_sync_bad = -1;
}

/* ============================================================ log */

static void t_log(void)
{
    reset_all();
    g_console_busy = 1u;
    g_console_drop = 2u;
    g_fifo_refused = 3u;
    g_fifo_bad = 4u;
    g_fifo_bad_bytes = 5u;
    CK_I(c0(cmd_log), 0);
    CK(out_has("log level: 3 (info)"));
    CK(out_has("console   : lock_giveup=1 tx_drop=2 B fifo_full_retries=3"));
    CK(out_has("fifo      : bad_header=4 bytes_lost=5"));

    cap_reset();
    CK_I(c1(cmd_log, "4"), 0);
    CK(out_has("log level: 4 (debug)"));
    CK_I(g_log_level_v, 4);

    cap_reset();
    CK_I(c1(cmd_log, "5"), -1);
    CK(out_has("usage: log <0..4>"));
    CK_I(g_log_level_v, 4);

    cap_reset();
    CK_I(c1(cmd_log, "abc"), -1);
    CK(out_has("usage: log <0..4>"));
}

/* ============================================================ rt */

static void t_rt(void)
{
    /* no cycle counter at all */
    reset_all();
    g_per_tick = 0u;
    CK_I(c0(cmd_rt), -1);
    CK(out_has("cycle counter unavailable - no measurement possible"));
    CK(out_has("nothing is estimated from the tick period on purpose"));

    /* a full report: counter dead, board constant, one empty row, one row
     * with late/skip, jitter present, tickless line */
    cap_reset();
    g_per_tick = 1000u;
    g_cycle_clock = 1000000u;
    g_clock_measured = 0;
    g_counter_alive = 0;
    g_rt_rows_n = 2u;
    g_rt_rows[0].name = "wake";
    g_rt_rows[0].n = 0u;
    g_rt_rows[1].name = "switch";
    g_rt_rows[1].n = 3u;
    g_rt_rows[1].min_c = 10u;
    g_rt_rows[1].avg_c = 20u;
    g_rt_rows[1].max_c = 30u;
    g_rt_rows[1].min_us = 5u;
    g_rt_rows[1].avg_us = 10u;
    g_rt_rows[1].max_us = 15u;
    g_rt_rows[1].late_n = 2u;
    g_rt_rows[1].skip_n = 1u;
    g_jitter_rc = 1;
    g_jitter_n = 5u;
    g_jitter_min = -2;
    g_jitter_max = 3;
    g_tl_isrs = 4u;
    g_tl_sleeps = 3u;
    g_kernel_tick = 10u;
    CK_I(c0(cmd_rt), 0);
    CK(out_has("cycles per tick: 1000  (cycle clock 1000000 Hz [board constant], tick 500 us)"));
    CK(out_has("counter: NOT running (it does not advance) - numbers below are stale"));
    CK(out_has("wake      -       (no sample)"));
    CK(out_has("switch    3       10        20        30        5        10       15"));
    CK(out_has("late=2  skip=1"));
    CK(out_has("jitter    n=5  min=-2  max=3"));
    CK(out_has("tickless  isr=4  sleeps=3  skipped=6 of 10 tick(s)  cap=8"));
    CK(out_has("they are not a formal WCET bound"));

    /* counter running, measured clock, no jitter sample */
    cap_reset();
    g_clock_measured = 1;
    g_counter_alive = 1;
    g_jitter_rc = 0;
    CK_I(c0(cmd_rt), 0);
    CK(out_has("[measured]"));
    CK(out_has("counter: running"));
    CK(out_has("jitter    (no sample)"));

    /* reset */
    cap_reset();
    CK_I(c1(cmd_rt, "reset"), 0);
    CK(out_has("real-time statistics cleared"));

    /* run: rejected sample counts */
    cap_reset();
    CK_I(c1(cmd_rt, "run"), 0);              /* default 200, then success */
    CK_I(c2(cmd_rt, "run", "0"), -1);
    CK(out_has("usage: rt run [n]   (1 <= n <= 5000)"));
    cap_reset();
    CK_I(c2(cmd_rt, "run", "5001"), -1);
    CK(out_has("usage: rt run [n]   (1 <= n <= 5000)"));

    /* run: counter still dead */
    cap_reset();
    g_per_tick = 0u;
    CK_I(c2(cmd_rt, "run", "10"), -1);
    CK(out_has("cycle counter still not running - cannot measure"));

    /* run: wake scenario refused */
    cap_reset();
    g_per_tick = 1000u;
    g_wake_rc = -1;
    CK_I(c2(cmd_rt, "run", "10"), -1);
    CK(out_has("wake scenario: sampler not ready (DWT unavailable)"));

    /* run: preempt scenario refused, with its reason code */
    cap_reset();
    g_wake_rc = 0;
    g_preempt_rc = -1;
    g_rt_last_err = 5;
    CK_I(c2(cmd_rt, "run", "10"), -1);
    CK(out_has("preempt scenario: failed (reason code 5)"));
    CK(out_has("1/2 = semaphore create, 5 = helper task register"));

    /* unknown subcommand */
    cap_reset();
    CK_I(c1(cmd_rt, "bogus"), -1);
    CK(out_has("usage: rt [show | reset | run [n]]"));
}

/* ============================================================ sec */

static void sec_fill(void)
{
    reset_all();
    g_sec_rep.count = 4u;
    g_sec_rep.pass = 1u;
    g_sec_rep.fail = 1u;
    g_sec_rep.finding = 1u;
    g_sec_rep.skip = 1u;
    g_sec_rep.item[0].name = "mpu.windows";
    g_sec_rep.item[0].state = ARKRTOS_SEC_PASS;
    g_sec_rep.item[0].fmt = "v=%u/%u";
    g_sec_rep.item[0].v[0] = 4u;
    g_sec_rep.item[0].v[1] = 4u;
    g_sec_rep.item[1].name = "stack.guard";
    g_sec_rep.item[1].state = ARKRTOS_SEC_FAIL;
    g_sec_rep.item[1].fmt = "";
    g_sec_rep.item[2].name = "tlb.asid";
    g_sec_rep.item[2].state = ARKRTOS_SEC_FINDING;
    g_sec_rep.item[2].fmt = "x=%u";
    g_sec_rep.item[2].v[0] = 7u;
    g_sec_rep.item[3].name = "tz.m33";
    g_sec_rep.item[3].state = ARKRTOS_SEC_SKIP;
    g_sec_rep.item[3].fmt = "";
}

static void t_sec(void)
{
    sec_fill();
    CK_I(c0(cmd_sec), 0);
    CK(out_has("sec: 4 checks - pass 1, fail 1, finding 1, skip 1"));
    CK(out_has("[pass] mpu.windows        v=4/4"));
    CK(out_has("[fail] stack.guard        "));
    CK(out_has("[finding] tlb.asid           x=7"));
    CK(out_has("[skip] tz.m33            "));
    CK(out_has("verdict: no hard failure"));

    cap_reset();
    CK_I(c1(cmd_sec, "pass"), 0);
    CK(out_count("[pass]") == 1);
    CK(!out_has("[fail]"));

    cap_reset();
    CK_I(c1(cmd_sec, "skip"), 0);
    CK(out_count("[skip]") == 1);

    cap_reset();
    CK_I(c1(cmd_sec, "find"), 0);
    CK(out_count("[finding]") == 1);

    cap_reset();
    CK_I(c1(cmd_sec, "finding"), 0);
    CK(out_count("[finding]") == 1);

    /* a hard failure turns the verdict and the exit code */
    cap_reset();
    g_sec_fails = 1u;
    CK_I(c1(cmd_sec, "fail"), -1);
    CK(out_count("[fail]") == 1);
    CK(out_has("verdict: 1 hard failure(s) - this build is not what you think it is"));

    cap_reset();
    CK_I(c1(cmd_sec, "bogus"), -1);
    CK(out_has("usage: sec [all | pass | fail | find | skip]"));
}

/* ============================================================ utest */

static void t_utest(void)
{
    reset_all();
    CK_I(c1(cmd_utest, "list"), 0);
    CK_I(g_utest_out_set, 1);
    CK(out_has("case one"));
    CK(out_has("case two"));

    cap_reset();
    CK_I(c0(cmd_utest), 0);                  /* default sub is "run" */
    CK_I(g_utest_failed, 0);

    cap_reset();
    g_utest_failed = 2u;
    CK_I(c1(cmd_utest, "run"), 2);
    CK_I(g_utest_failed, 2);

    cap_reset();
    CK_I(c1(cmd_utest, "bogus"), -1);
    CK(out_has("usage: utest [list | run [pattern]]"));
}

/* ============================================================ pool */

static void t_pool(void)
{
    reset_all();
    g_pool_total = 65536u;
    g_pool_largest = 32768u;
    CK_I(c0(cmd_pool), 0);
    CK(out_has("pool free : 65536 B (64K)"));
    CK(out_has("largest   : 32768 B (32K)"));
    CK(!out_has("fixed slots"));

    /* fixed layout: slot 1 matches a live record, slot 0 reads empty */
    cap_reset();
    g_pt.layout_mode = ARKRTOS_LAYOUT_MODE_FIXED;
    g_pt.slot_max = 2u;
    g_pt.slot_type[0] = 1u;
    g_pt.slot_base[0] = 0x2000u;
    g_pt.slot_size[0] = 0x200u;
    g_pt.slot_state[0] = 2u;
    g_ls_n = 2u;
    g_ls[0].base = 0x1000u;
    g_ls[0].size = 0x100u;
    g_ls[0].type = 1u;
    g_ls[1].base = 0x2000u;
    g_ls[1].size = 0x200u;
    g_ls[1].type = 2u;
    CK_I(c0(cmd_pool), 0);
    CK(out_has("fixed slots"));
    CK(out_has(" id  type  base        size     state"));
    CK(out_has(" 0   app   0x00001000  256       empty"));
    CK(out_has(" 1   drv   0x00002000  512       running"));
}

/* ============================================================ main */

int main(void)
{
    reset_all();

    t_task();
    t_stack();
    t_guard();
    t_crash();
    t_audit();
    t_sched();
    t_dt();
    t_fdt();
    t_fault();
    t_syncinfo();
    t_log();
    t_rt();
    t_sec();
    t_utest();
    t_pool();

    if (g_failed == 0) {
        printf("SHELL-DIAG-HOST OK  %d checks\n", g_checks);
        return 0;
    }
    printf("SHELL-DIAG-HOST FAILED  %d of %d\n", g_failed, g_checks);
    return 1;
}
