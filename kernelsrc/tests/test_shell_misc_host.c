/**
* @file test_shell_misc_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_misc.c.
*
* What this proves: the misc command family (history / uname / free / ps /
* which / man / dmesg / hwsafe / swap / lxstat) formats the values it is
* given, returns the exit code it documents, and -- for the two commands
* that can refuse -- says why it refused.  Every kernel-side number comes
* from a stub, so each assertion is about the shell's own logic, not about
* the module underneath.
*
* What it does NOT prove:
*   - that the console really transmits anything (the UART is a buffer here);
*   - the swap success path: it needs the test task to actually run and its
*     counters to advance, which needs a scheduler and a real trap frame.
*     Only the two refusal paths (cannot register / did not start) and the
*     stat line are reachable on the host;
*   - that a real log ring, a real hwsafe ledger or a real Linux ABI table
*     look like the fixtures below.
*
* The fixture values are arbitrary but distinctive, so a command that prints
* the wrong field is caught rather than passing by coincidence.
*
* @author xw
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "arkrtos_share.h"
#include "arkrtos_ptable.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_hwsafe.h"
#include "arkrtos_log.h"
#include "arkrtos_linux_abi.h"
#include "ark_shell.h"
#include "ark_shell_commands.h"
#include "platform.h"

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

/* ------------------------------------------------------------ capture */

static char g_out[8192];
static size_t g_out_len;

static void cap_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0;
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

/* The commands under test -------------------------------------------------- */

int cmd_history(int argc, char *argv[]);
int cmd_uname(int argc, char *argv[]);
int cmd_free(int argc, char *argv[]);
int cmd_ps(int argc, char *argv[]);
int cmd_which(int argc, char *argv[]);
int cmd_man(int argc, char *argv[]);
int cmd_dmesg(int argc, char *argv[]);
int cmd_hwsafe(int argc, char *argv[]);
int cmd_swap(int argc, char *argv[]);
int cmd_lxstat(int argc, char *argv[]);

/* 下面这些是被测文件调用的内核侧符号。它们不在被测文件的头里（真实定义
 * 在 arkrtos_shell.c / 内核里），这里必须自己声明：告警门禁开着
 * -Wmissing-prototypes，没有原型的非 static 定义就是一条告警，而这一档
 * 要求零告警。签名照抄真实定义，形参名与返回值都不许凑。 */
void sh_out(const char *s);
const char *task_state_name(uint32 st);
const char *slot_state_name(uint32 st);
const char *slot_type_name(uint32 t);
void arkrtos_task_wait(uint32 ms);
void arkrtos_task_wait_internal(uint32 ms);

/* ---- stubs: shell core ------------------------------------------------ */

ark_shell_t g_shell;

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

void sh_out(const char *s)
{
    cap_add(s, strlen(s));
}

/* ---- stubs: platform ------------------------------------------------- */

void platform_uart_send_buf(const unsigned char *buf, uint16_t len)
{
    cap_add((const char *)buf, (size_t)len);
}

void platform_console_begin(void)
{
    cap_add("[begin]", 7u);
}

void platform_console_end(void)
{
    cap_add("[end]", 5u);
}

/* ---- stubs: name tables (distinctive so a swap is caught) ------------- */

const char *task_state_name(uint32 st)
{
    return (st == 1u) ? "READY-S" : "OTHER-S";
}

const char *slot_state_name(uint32 st)
{
    return (st == 7u) ? "RUNNING-S" : "IDLE-S";
}

const char *slot_type_name(uint32 t)
{
    return (t == 2u) ? "APP-S" : "DRV-S";
}

/* ---- stubs: command table (what which/man resolve through) ------------ */

static const ark_shell_cmd_t g_cmd_ls = { "ls", NULL, "list things", 2 };
static const char *g_alias_target;      /* NULL => not an alias */
static const char *g_alias_a1;
static const char *g_alias_a2;
static int g_find_hits;

const char *ark_shell_alias_target(const char *name,
                                   const char **arg1, const char **arg2)
{
    (void)name;
    *arg1 = g_alias_a1;
    *arg2 = g_alias_a2;
    return g_alias_target;
}

const ark_shell_cmd_t *ark_shell_find_command(const char *name)
{
    g_find_hits++;
    return (strcmp(name, "ls") == 0) ? &g_cmd_ls : NULL;
}

/* ---- stubs: partition table ----------------------------------------- */

static arkrtos_partition_table_t g_pt;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

static uint32 g_stat_used;
static uint32 g_stat_total;
static uint32 g_stat_frag;

int32 arkrtos_ptable_stats(uint32 *p_used, uint32 *p_total, uint32 *p_frag)
{
    *p_used = g_stat_used;
    *p_total = g_stat_total;
    *p_frag = g_stat_frag;
    return 0;
}

/* ---- stubs: task table (declared by arkrtos_cfg.h) ------------------- */

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_task_count;

/* ---- stubs: hardware safe-state ledger ------------------------------- */

static int32 g_devs;
static int32 g_owners;
static int32 g_last_owner;
static uint32 g_neutralized;
static uint32 g_pc_will, g_pc_fail, g_pc_cb, g_pc_none, g_pc_handover;
static int32 g_will_has;
static int32 g_will_cmd, g_will_val;
static int32 g_hwsafe_bad_idx;          /* >=0 => that index returns -1 */
static int32 g_dev_at_bad;              /* >=0 => that dev index returns -1 */

int32 arkrtos_hwsafe_dev_count(void) { return g_devs; }
int32 arkrtos_hwsafe_owner_count(void) { return g_owners; }
uint32 arkrtos_hwsafe_neutralized_count(void) { return g_neutralized; }
int32 arkrtos_hwsafe_last_owner(void) { return g_last_owner; }

void arkrtos_hwsafe_path_counts(uint32 *will_out, uint32 *fail_out,
                                uint32 *cb_out, uint32 *none_out,
                                uint32 *handover_out)
{
    *will_out = g_pc_will;
    *fail_out = g_pc_fail;
    *cb_out = g_pc_cb;
    *none_out = g_pc_none;
    *handover_out = g_pc_handover;
}

int32 arkrtos_hwsafe_dev_at(int32 idx, char *name_out, uint32 max)
{
    if ((idx < 0) || (idx >= g_devs) || (idx == g_dev_at_bad)) {
        return -1;
    }
    snprintf(name_out, max, "dev%d", (int)idx);
    return 0;
}

int32 arkrtos_hwsafe_info(int32 idx, char *name_out, uint32 max,
                          int32 *owner_out, int32 *dead_out)
{
    if ((idx < 0) || (idx >= g_owners)) {
        return -1;
    }
    snprintf(name_out, max, "own%d", (int)idx);
    *owner_out = 100 + idx;
    *dead_out = 0;
    return (idx == g_hwsafe_bad_idx) ? -1 : 0;
}

int32 arkrtos_hwsafe_will(int32 idx, int32 *has_will,
                          int32 *cmd_out, int32 *val_out)
{
    *has_will = g_will_has;
    *cmd_out = g_will_cmd;
    *val_out = g_will_val;
    (void)idx;
    return 0;
}

/* ---- stubs: log ring ------------------------------------------------- */

static uint32 g_ring_capacity;
static uint32 g_ring_used;
static int g_ring_clear_calls;
static const char *g_ring_text;

uint32 arkrtos_log_ring_capacity(void) { return g_ring_capacity; }
uint32 arkrtos_log_ring_used(void) { return g_ring_used; }
void arkrtos_log_ring_clear(void) { g_ring_clear_calls++; }

uint32 arkrtos_log_ring_dump(void (*put)(const char *data, uint32 len, void *arg),
                             void *arg)
{
    const char *t = (g_ring_text != NULL) ? g_ring_text : "log-line\n";
    put(t, (uint32)strlen(t), arg);
    return (uint32)strlen(t);
}

/* ---- stubs: task swap ------------------------------------------------ */

void arkrtos_task_wait(uint32 ms) { (void)ms; }

void arkrtos_task_wait_internal(uint32 ms) { (void)ms; }

static int32 g_register_rc = 1;
static int32 g_swap_cancel_calls;
static int32 g_swap_arm_rc;
static int32 g_swap_arm_calls;
static int32 g_swap_selftest_calls;

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom,
                            uint32 stack_size, uint8 priority, uint32 period_ms)
{
    (void)entry;
    (void)stack_bottom;
    (void)stack_size;
    (void)priority;
    (void)period_ms;
    return g_register_rc;
}

int32 arkrtos_task_swap_pending(void) { return 0; }
int32 arkrtos_task_swap_cancel(int32 task_id) { (void)task_id; g_swap_cancel_calls++; return 0; }

int32 arkrtos_task_swap_arm(int32 task_id, unsigned long rom_start,
                            uint32 rom_size, unsigned long ram_start,
                            uint32 ram_size, unsigned long entry)
{
    (void)task_id; (void)rom_start; (void)rom_size;
    (void)ram_start; (void)ram_size; (void)entry;
    g_swap_arm_calls++;
    return g_swap_arm_rc;
}

void arkrtos_task_swap_selftest_arm(int32 task_id)
{
    (void)task_id;
    g_swap_selftest_calls++;
}

/* ---- stubs: Linux ABI counters --------------------------------------- */

static uint32 g_lx_stat[5];
static uint32 g_lx_slots;
static uint32 g_lx_slot_nr[4];
static uint32 g_lx_slot_hits[4];
static uint32 g_lx_overflow;

uint32 arkrtos_linux_abi_stat(uint32 which)
{
    return (which < 5u) ? g_lx_stat[which] : 0u;
}

uint32 arkrtos_linux_abi_nr_slots(void) { return g_lx_slots; }
uint32 arkrtos_linux_abi_nr_overflow(void) { return g_lx_overflow; }

uint32 arkrtos_linux_abi_nr_slot(uint32 idx, uint32 *p_nr)
{
    if (idx >= g_lx_slots) {
        return 0u;
    }
    *p_nr = g_lx_slot_nr[idx];
    return g_lx_slot_hits[idx];
}

/* ------------------------------------------------------------ helpers */

static int call1(int (*fn)(int, char **), const char *arg)
{
    char name[32];
    char *argv[2];

    snprintf(name, sizeof(name), "%s", arg);
    argv[0] = name;
    argv[1] = NULL;
    return fn(1, argv);
}

static int call2(int (*fn)(int, char **), const char *a0, const char *a1)
{
    char n0[32];
    char n1[32];
    char *argv[3];

    snprintf(n0, sizeof(n0), "%s", a0);
    snprintf(n1, sizeof(n1), "%s", a1);
    argv[0] = n0;
    argv[1] = n1;
    argv[2] = NULL;
    return fn(2, argv);
}

/* ------------------------------------------------------------ tests */

static void test_history(void)
{
    cap_reset();
    g_shell.history.count = 2;
    snprintf(g_shell.history.buf[0], sizeof(g_shell.history.buf[0]), "ls -l");
    snprintf(g_shell.history.buf[1], sizeof(g_shell.history.buf[1]), "free");

    CK_I(call1(cmd_history, "history"), 0);
    CK(out_has("    1  ls -l\r\n"));
    CK(out_has("    2  free\r\n"));
    CK_I(out_count("\r\n"), 2);

    /* empty history prints nothing at all */
    g_shell.history.count = 0;
    cap_reset();
    CK_I(call1(cmd_history, "history"), 0);
    CK_I(out_count("\r\n"), 0);
}

static void test_uname(void)
{
    cap_reset();
    CK_I(call1(cmd_uname, "uname"), 0);
    CK(out_has("Ark RTOS "));
    CK(!out_has("Ark RTOS arkrtos"));

    /* -a 版本多印一次 "arkrtos"（sysname nodename 两个字段） */
    cap_reset();
    CK_I(call2(cmd_uname, "uname", "-a"), 0);
    CK(out_has("Ark RTOS arkrtos "));

    /* 别的参数按「不带 -a」处理，不报错 */
    cap_reset();
    CK_I(call2(cmd_uname, "uname", "-x"), 0);
    CK(!out_has("Ark RTOS arkrtos"));
}

static void test_free(void)
{
    g_pt.kernel_ram_size = 65536u;

    g_stat_used = 100u;
    g_stat_total = 400u;
    g_stat_frag = 3u;
    cap_reset();
    CK_I(call1(cmd_free, "free"), 0);
    CK(out_has("pool(image)"));
    CK(out_has("400"));
    CK(out_has("300"));         /* free = total - used */
    CK(out_has("25% used"));
    CK(out_has("3 fragments"));
    CK(out_has("65536"));
    CK(out_has("n/a"));

    /* total == 0 时百分比取 0，不除零 */
    g_stat_used = 0u;
    g_stat_total = 0u;
    g_stat_frag = 0u;
    cap_reset();
    CK_I(call1(cmd_free, "free"), 0);
    CK(out_has("0% used"));

    /* used > total 时 free 夹到 0（不报负数） */
    g_stat_used = 500u;
    g_stat_total = 400u;
    cap_reset();
    CK_I(call1(cmd_free, "free"), 0);
    CK(out_has("125% used"));
}

static void test_ps(void)
{
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.kernel_ram_size = 1u;

    arkrtos_task_count = 2;
    arkrtos_task_table[0].priority = 3u;
    arkrtos_task_table[0].status = 1;
    arkrtos_task_table[0].entry = (void (*)(void))(unsigned long)0x1234u;
    arkrtos_task_table[1].priority = 9u;
    arkrtos_task_table[1].status = 0;
    arkrtos_task_table[1].entry = (void (*)(void))(unsigned long)0x5678u;

    /* 三个槽：FREE 跳过，APP 与 DRV 各打一行 */
    g_pt.slot_max = 3u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_FREE;
    g_pt.slot_entry[0] = 0xDEADu;   /* 只有 FREE 槽被打出来时才会出现 */
    g_pt.slot_type[1] = 2u;
    g_pt.slot_state[1] = 7u;
    g_pt.slot_task_id[1] = 4u;
    g_pt.slot_entry[1] = 0xABCDu;
    g_pt.slot_type[2] = 3u;
    g_pt.slot_state[2] = 0u;
    g_pt.slot_task_id[2] = 0u;
    g_pt.slot_entry[2] = 0u;

    cap_reset();
    CK_I(call1(cmd_ps, "ps"), 0);
    CK(out_has("PID  PRIO  STATE"));
    CK(out_has("SLOT TYPE  STATE"));
    CK(out_has("READY-S"));
    CK(out_has("OTHER-S"));
    CK(out_has("0x00001234"));
    CK(out_has("0x00005678"));
    CK(out_has("APP-S"));
    CK(out_has("RUNNING-S"));
    CK(out_has("0x0000ABCD"));
    CK(out_has("DRV-S"));
    CK(out_has("IDLE-S"));
    /* FREE 那一行不打：给它一个独特入口，被打出来才看得到 */
    CK(!out_has("0x0000DEAD"));
}

static void test_which(void)
{
    /* 没有参数：用法 + 返回 1 */
    cap_reset();
    CK_I(cmd_which(1, NULL), 1);
    CK(out_has("usage: which <command>"));

    /* 别名：目标 + 两个固定参数都打出来 */
    g_alias_target = "ls";
    g_alias_a1 = "-l";
    g_alias_a2 = "/";
    cap_reset();
    CK_I(call2(cmd_which, "which", "ll"), 0);
    CK(out_has("ll: aliased to ls -l /"));

    /* 别名只有一个固定参数：第二个不打 */
    g_alias_a2 = NULL;
    cap_reset();
    CK_I(call2(cmd_which, "which", "ll"), 0);
    CK(out_has("ll: aliased to ls -l"));
    CK(!out_has("-l /"));

    /* 别名没有固定参数 */
    g_alias_a1 = NULL;
    cap_reset();
    CK_I(call2(cmd_which, "which", "ll"), 0);
    CK(out_has("ll: aliased to ls"));

    /* 不是别名但命令存在：只打命令名 */
    g_alias_target = NULL;
    g_find_hits = 0;
    cap_reset();
    CK_I(call2(cmd_which, "which", "ls"), 0);
    CK_I(g_find_hits, 1);
    CK(out_has("ls\r\n"));

    /* 两者都不是：像 which(1) 一样什么都不打，返回 1 */
    cap_reset();
    CK_I(call2(cmd_which, "which", "nope"), 1);
    CK_I(out_count("\r\n"), 0);
}

static void test_man(void)
{
    cap_reset();
    CK_I(cmd_man(1, NULL), 1);
    CK(out_has("What manual page do you want?"));

    /* 别名指向存在的命令：两行都打 */
    g_alias_target = "ls";
    g_alias_a1 = NULL;
    g_alias_a2 = NULL;
    cap_reset();
    CK_I(call2(cmd_man, "man", "ll"), 0);
    CK(out_has("ll: aliased to ls"));
    CK(out_has("  ls - list things\r\n"));

    /* 别名指向不存在的命令：只打别名行，不报错 */
    g_alias_target = "ghost";
    cap_reset();
    CK_I(call2(cmd_man, "man", "ll"), 0);
    CK(out_has("ll: aliased to ghost"));
    CK(!out_has(" - "));

    /* 普通命令 */
    g_alias_target = NULL;
    cap_reset();
    CK_I(call2(cmd_man, "man", "ls"), 0);
    CK(out_has("ls - list things\r\n"));

    /* 没有这个手册页：返回 1 */
    cap_reset();
    CK_I(call2(cmd_man, "man", "zzz"), 1);
    CK(out_has("No manual entry for zzz"));
}

static void test_dmesg(void)
{
    /* 不认识的选项 */
    cap_reset();
    CK_I(call2(cmd_dmesg, "dmesg", "-x"), 1);
    CK(out_has("usage: dmesg [-c]"));

    /* 环没开 */
    g_ring_capacity = 0u;
    cap_reset();
    CK_I(call1(cmd_dmesg, "dmesg"), 1);
    CK(out_has("log ring unavailable (ARKRTOS_USE_LOG_RING is off)"));

    /* 环开着但空 */
    g_ring_capacity = 128u;
    g_ring_used = 0u;
    cap_reset();
    CK_I(call1(cmd_dmesg, "dmesg"), 0);
    CK(out_has("log ring is empty"));
    CK(!out_has("[begin]"));

    /* 有内容：dump 在控制台锁内跑，内容经 UART 缓冲出来 */
    g_ring_used = 9u;
    g_ring_text = "hello-log\n";
    g_ring_clear_calls = 0;
    cap_reset();
    CK_I(call1(cmd_dmesg, "dmesg"), 0);
    CK(out_has("[begin]"));
    CK(out_has("hello-log"));
    CK(out_has("[end]"));
    CK_I(g_ring_clear_calls, 0);

    /* -c：dump 之后清环 */
    cap_reset();
    CK_I(call2(cmd_dmesg, "dmesg", "-c"), 0);
    CK_I(g_ring_clear_calls, 1);
    CK(out_has("hello-log"));
}

static void test_hwsafe(void)
{
    g_devs = 2;
    g_owners = 2;
    g_neutralized = 3u;
    g_last_owner = 100;
    g_pc_will = 1u;
    g_pc_fail = 2u;
    g_pc_cb = 4u;
    g_pc_none = 5u;
    g_pc_handover = 6u;
    g_will_has = 1;
    g_will_cmd = 7;
    g_will_val = 9;
    g_hwsafe_bad_idx = -1;
    g_dev_at_bad = -1;

    cap_reset();
    CK_I(call1(cmd_hwsafe, "hwsafe"), 0);
    CK(out_has("hwsafe: enabled, devs=2 owners=2 neutralized=3"));
    CK(out_has("(will=1 cb=4 none=5 fail=2 handover=6)"));
    CK(out_has("last_owner=100"));
    CK(out_has("  dev0: dev0"));
    CK(out_has("  dev1: dev1"));
    CK(out_has("  own0: dev=own0 owner=100 dead=0 will=7:9"));
    CK(out_has("  own1: dev=own1 owner=101 dead=0 will=7:9"));
    CK(!out_has("(no device ownership recorded)"));

    /* 第一条台账没有遗言：走 "none" */
    g_will_has = 0;
    cap_reset();
    CK_I(call1(cmd_hwsafe, "hwsafe"), 0);
    CK(out_has("will=none"));
    CK(!out_has("will=7:9"));

    /* info 报错就停在那一条（不继续往下打） */
    g_will_has = 1;
    g_hwsafe_bad_idx = 0;
    cap_reset();
    CK_I(call1(cmd_hwsafe, "hwsafe"), 0);
    CK(out_has("  dev0: dev0"));
    CK(!out_has("  own0:"));
    CK(!out_has("  own1:"));

    /* 设备枚举报错就停（第二条不再打），但下面的台账循环照旧跑 */
    g_dev_at_bad = 1;
    g_hwsafe_bad_idx = -1;
    cap_reset();
    CK_I(call1(cmd_hwsafe, "hwsafe"), 0);
    CK(out_has("  dev0: dev0"));
    CK(!out_has("  dev1:"));
    CK(out_has("  own0:"));
    g_dev_at_bad = -1;

    /* 没有任何台账：打一行说明 */
    g_devs = 0;
    g_owners = 0;
    cap_reset();
    CK_I(call1(cmd_hwsafe, "hwsafe"), 0);
    CK(out_has("(no device ownership recorded)"));
}

static void test_swap(void)
{
    /* stat 只看数，不启动任何东西 */
    cap_reset();
    CK_I(call2(cmd_swap, "swap", "stat"), 0);
    CK(out_has("swap: pending="));
    CK(out_has("v1="));
    CK(out_has("pass="));

    /* 注册失败：说清是哪一步失败，并返回 1 */
    g_register_rc = 0;          /* <= 0 即失败 */
    g_swap_cancel_calls = 0;
    g_swap_selftest_calls = 0;
    cap_reset();
    CK_I(call1(cmd_swap, "swap"), 1);
    CK(out_has("swap: cannot register the test task"));
    CK_I(g_swap_cancel_calls, 1);
    CK_I(g_swap_selftest_calls, 0);

    /* 注册成功但任务一次都没跑：报「没起来」而不是等下去 */
    g_register_rc = 5;
    cap_reset();
    CK_I(call1(cmd_swap, "swap"), 1);
    CK(out_has("swap: test task did not start"));
    CK_I(g_swap_arm_calls, 0);
    CK_I(g_swap_selftest_calls, 0);

    /* 失败过一次之后 done 标记不置位，所以再跑还是同一条路（不是 PASS/FAIL） */
    cap_reset();
    CK_I(call1(cmd_swap, "swap"), 1);
    CK(out_has("swap: test task did not start"));
}

static void test_lxstat(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        g_lx_stat[i] = (uint32)(i + 1);
    }
    g_lx_slots = 3u;
    g_lx_slot_nr[0] = 56u;      /* openat，名字表里有 */
    g_lx_slot_hits[0] = 4u;
    g_lx_slot_nr[1] = 999u;     /* 名字表里没有 => "?" */
    g_lx_slot_hits[1] = 2u;
    g_lx_slot_nr[2] = 222u;     /* mmap */
    g_lx_slot_hits[2] = 1u;
    g_lx_overflow = 5u;

    cap_reset();
    CK_I(call1(cmd_lxstat, "lxstat"), 0);
    CK(out_has("lxstat: handled=1 enosys=2 efault=3 other=4 mprotect=5"));
    CK(out_has("nr  56 openat"));
    CK(out_has("x4"));
    CK(out_has("nr 999 ?"));
    CK(out_has("nr 222 mmap"));
    CK(out_has("(+5 calls on numbers past the 3-slot table)"));

    /* 一个号都没见过：只打计数，并明确说没有读数的原因 */
    g_lx_slots = 0u;
    g_lx_overflow = 0u;
    cap_reset();
    CK_I(call1(cmd_lxstat, "lxstat"), 0);
    CK(out_has("no Linux syscall seen since the last exec"));
    CK(!out_has("nr "));

    /* 有槽但没有溢出：不打那一行 */
    g_lx_slots = 1u;
    cap_reset();
    CK_I(call1(cmd_lxstat, "lxstat"), 0);
    CK(out_has("nr  56 openat"));
    CK(!out_has("past the"));
}

int main(void)
{
    test_history();
    test_uname();
    test_free();
    test_ps();
    test_which();
    test_man();
    test_dmesg();
    test_hwsafe();
    test_swap();
    test_lxstat();

    if (g_failed != 0) {
        printf("SHELL-MISC-HOST FAILED  %d of %d\n", g_failed, g_checks);
        return 1;
    }
    printf("SHELL-MISC-HOST OK  %d checks\n", g_checks);
    return 0;
}
