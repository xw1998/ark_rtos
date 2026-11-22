/**
* @file test_crash_host.c
* @brief 跨复位崩溃日志（arkrtos_crash.c）的宿主用例
* @details 这个模块记的是「复位之后还得算数」的那半本账：某个槽位连续崩了几次、
*          为什么崩、是不是已经被停用。它不在任何一次运行的返回码上——一块板子
*          被同一个镜像反复带进复位、直到再也没人看得到串口，正是它要防的事故。
*          所以这里钉住的是「复位后账本还是不是原来那本」以及三件事不能混：
*
*            - 记录身份是**镜像在池内的地址**，不是槽位下标。复位后记录按地址
*              重排，账要跟着地址走；地址换了（重装/搬移）旧账必须作废，不能
*              让新镜像替别人的崩溃次数背锅；
*            - 没有任何池地址的槽位（还没装、或裸镜像）没有跨复位的身份，只记
*              RAM 镜像，日志一份都不写——返回值会如实说这是 RAM 计数；
*            - 停用是**粘性**的：一旦到达上限，后续崩溃只加计数、不改原因与
*              时刻；上限本身来自设备端配置，0 表示「按编译期默认」，不是
*              「不限次」——一个没填的字段静默关掉策略正是这个模块要堵的洞。
*
*          只依赖三个外部符号：分区表（本文件给桩并按调用记录参数）、
*          arkrtos_kernel_tick（本文件给变量）与 port 临界区（配对计数桩）。
*          日志宏在本档被编掉（-DARKRTOS_LOG_LEVEL=0），不走真实控制台。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_crash.h"
#include "arkrtos_ptable.h"        /* arkrtos_ptable_get / set_slot 原型 */
#include "arkrtos_hal.h"           /* port_enter/exit_critical 原型 */
#include "arkrtos_share.h"         /* ARKRTOS_SLOT_x / ARKRTOS_APP_SLOT_INVALID */

/* journal 不在公开头里声明（它是靠 section 名被链接器摆位置的全局量），
 * 用例自己按同一个类型声明一次：名字或类型变了，链接就会红。 */
extern arkrtos_crash_journal_t arkrtos_crash_journal;

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %lu want %lu)\n", what, got, want);
    }
}

/* ---------------- 临界区桩 ---------------- */

static unsigned long g_crit_enters;
static unsigned long g_crit_exits;

uint32 arkrtos_port_enter_critical(void)
{
    g_crit_enters++;
    return (uint32)g_crit_enters;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
    g_crit_exits++;
}

/* ---------------- 分区表桩 ---------------- */

static arkrtos_partition_table_t g_pt;
static unsigned long g_set_slot_calls;
static unsigned long g_set_slot_slot;
static unsigned long g_set_slot_state;
static unsigned long g_set_slot_entry;
static unsigned long g_set_slot_task;

arkrtos_partition_table_t *arkrtos_ptable_get(void)
{
    return &g_pt;
}

int32 arkrtos_ptable_set_slot(uint32 slot, uint32 state, uint32 entry, uint32 task_id)
{
    g_set_slot_calls++;
    g_set_slot_slot  = (unsigned long)slot;
    g_set_slot_state = (unsigned long)state;
    g_set_slot_entry = (unsigned long)entry;
    g_set_slot_task  = (unsigned long)task_id;
    if(slot < ARKRTOS_SLOT_ARRAY_MAX)
    {
        g_pt.slot_state[slot] = state;
    }
    return 0;
}

uint32 arkrtos_kernel_tick;

/* ---------------- 公共夹具 ---------------- */

#define BASE0 (0x00210000u)
#define BASE1 (0x00220000u)
#define BASE2 (0x00230000u)

#if (ARKRTOS_USE_CRASH_LOG == 1)
/* 账本在 OFF 档根本不存在（没有那个全局量），所以清零夹具只在 ON 档编。 */
static void journal_zero(void)
{
    memset(&arkrtos_crash_journal, 0, sizeof(arkrtos_crash_journal));
}
#endif

static void pt_reset(void)
{
    memset(&g_pt, 0, sizeof(g_pt));
    g_pt.slot_max = 4u;
    g_pt.slot_type[0] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[0] = BASE0;
    g_pt.slot_entry[0] = BASE0;
    g_pt.slot_autostart[0] = 1u;
    g_pt.slot_type[1] = ARKRTOS_SLOT_DRIVER;
    g_pt.slot_base[1] = BASE1;
    g_pt.slot_entry[1] = BASE1;
    g_pt.slot_type[2] = ARKRTOS_SLOT_FREE;
    g_pt.slot_base[2] = 0u;
    g_pt.slot_type[3] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[3] = BASE2;
    g_pt.slot_entry[3] = BASE2;
    g_set_slot_calls = 0ul;
    g_set_slot_slot  = 0ul;
    g_set_slot_state = 0ul;
    g_set_slot_entry = 0ul;
    g_set_slot_task  = 0ul;
}

#if (ARKRTOS_USE_CRASH_LOG == 1)
static void all_reset(void)
{
    journal_zero();
    pt_reset();
    g_crit_enters = 0ul;
    g_crit_exits  = 0ul;
    arkrtos_kernel_tick = 1000u;
}
#endif

/* ============================================================
 * 路径哈希（两种档都编进来）
 * ============================================================ */

static void test_hash(void)
{
    ck_u((unsigned long)arkrtos_crash_hash_path(0), 0ul, "hash(NULL) == 0");
    ck_u((unsigned long)arkrtos_crash_hash_path(""), 0ul, "hash(\"\") == 0");
    /* FNV-1a 已知答案：外部真值，不是自己算自己验。 */
    ck_u((unsigned long)arkrtos_crash_hash_path("a"), 0xe40c292cul,
         "hash(\"a\") == FNV-1a");
    ck_u((unsigned long)arkrtos_crash_hash_path("abc"), 0x1a47e90bul,
         "hash(\"abc\") == FNV-1a");
    ck(arkrtos_crash_hash_path("/apps/x.mini") != 0ul, "hash(path) != 0");
    ck(arkrtos_crash_hash_path("/apps/x.mini") !=
       arkrtos_crash_hash_path("/apps/y.mini"), "不同路径不同哈希");
}

#if (ARKRTOS_USE_CRASH_LOG == 1)

/* ============================================================
 * init：有效日志要接着用，无效日志要清干净
 * ============================================================ */

static void test_init(void)
{
    all_reset();

    arkrtos_crash_init();
    ck_u((unsigned long)arkrtos_crash_journal.magic, (unsigned long)ARKRTOS_CRASH_MAGIC,
         "fresh init writes magic");
    ck_u((unsigned long)arkrtos_crash_journal.version, (unsigned long)ARKRTOS_CRASH_VERSION,
         "fresh init writes version");
    ck_u((unsigned long)arkrtos_crash_boots(), 1ul, "fresh init: boots == 1");
    ck_u((unsigned long)arkrtos_crash_seq(), 1ul, "fresh init: seq == 1");
    ck_u((unsigned long)arkrtos_crash_count(0), 0ul, "fresh init: count 0");
    ck_u((unsigned long)arkrtos_crash_disabled(0), 0ul, "fresh init: not disabled");

    /* 写一点历史，再「复位」一次：有效日志必须被保留，只加一次 boots。 */
    g_pt.slot_max = 4u;
    (void)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT);
    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_init();
        ck_u((unsigned long)arkrtos_crash_boots(), 2ul, "surviving init: boots == 2");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
             "surviving init bumps seq once");
        ck_u((unsigned long)arkrtos_crash_count(0), 1ul, "surviving init keeps count");
        ck_u((unsigned long)arkrtos_crash_journal.base[0], (unsigned long)BASE0,
             "surviving init keeps base");
    }

    /* 坏魔数：不是证据，必须从头来（否则会拿随机 RAM 字节当历史）。 */
    arkrtos_crash_journal.magic = 0u;
    arkrtos_crash_journal.crash_cnt[0] = 9u;
    arkrtos_crash_init();
    ck_u((unsigned long)arkrtos_crash_boots(), 1ul, "bad magic resets boots");
    ck_u((unsigned long)arkrtos_crash_count(0), 0ul, "bad magic clears count");

    /* 坏版本：同一类判断，单独钉一次（版本号的用途就是拒绝旧布局）。 */
    ck_u((unsigned long)arkrtos_crash_journal.version, (unsigned long)ARKRTOS_CRASH_VERSION,
         "journal version restored after bad magic");
    arkrtos_crash_journal.version = (uint32)ARKRTOS_CRASH_VERSION + 1u;
    arkrtos_crash_journal.crash_cnt[1] = 7u;
    arkrtos_crash_init();
    ck_u((unsigned long)arkrtos_crash_count(1), 0ul, "bad version clears count");
    ck_u((unsigned long)arkrtos_crash_boots(), 1ul, "bad version resets boots");

    /* 坏校验：半写的记录。 */
    arkrtos_crash_journal.check ^= 0xFFFFFFFFu;
    arkrtos_crash_journal.crash_cnt[2] = 5u;
    arkrtos_crash_init();
    ck_u((unsigned long)arkrtos_crash_count(2), 0ul, "bad checksum clears count");
    ck_u((unsigned long)arkrtos_crash_boots(), 1ul, "bad checksum resets boots");

    /* mini 表也得一起清：残留的路径哈希会被读成「这些路径已经崩过 N 次」。 */
    arkrtos_crash_journal.mini_hash[0] = 0x11223344u;
    arkrtos_crash_journal.mini_cnt[0] = 4u;
    arkrtos_crash_journal.check ^= 1u;
    arkrtos_crash_init();
    ck_u((unsigned long)arkrtos_crash_journal.mini_hash[0], 0ul,
         "invalid journal clears mini table");
    ck_u((unsigned long)arkrtos_crash_mini_count(0x11223344u), 0ul,
         "invalid journal: mini count 0");
}

/* ============================================================
 * fault：RAM 镜像 + 日志 + 停用
 * ============================================================ */

static void test_fault(void)
{
    all_reset();
    arkrtos_crash_init();

    /* 有池地址的槽位：RAM 与日志同步涨。 */
    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT), 1ul,
         "fault #1 returns 1");
    ck_u((unsigned long)g_pt.slot_crash_cnt[0], 1ul, "fault mirrors into ptable");
    ck_u((unsigned long)arkrtos_crash_count(0), 1ul, "fault writes journal count");
    ck_u((unsigned long)arkrtos_crash_journal.base[0], (unsigned long)BASE0,
         "fault tags the entry with the pool address");
    ck_u((unsigned long)arkrtos_crash_last_reason(0), (unsigned long)ARKRTOS_CRASH_REASON_FAULT,
         "fault stores the reason");

    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_HEARTBEAT), 2ul,
         "fault #2 returns 2");
    ck_u((unsigned long)arkrtos_crash_last_reason(0),
         (unsigned long)ARKRTOS_CRASH_REASON_HEARTBEAT, "reason follows the latest fault");
    ck_u((unsigned long)arkrtos_crash_disabled(0), 0ul, "still enabled at 2 < limit 3");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(0), 0ul, "no disable tick yet");

    /* 第三次到上限：停用并记住时刻与原因。 */
    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_AUDIT), 3ul,
         "fault #3 returns 3");
    ck_u((unsigned long)arkrtos_crash_disabled(0), (unsigned long)ARKRTOS_CRASH_REASON_AUDIT,
         "reaching the limit disables with the reason");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(0), 1000ul,
         "disable tick records arkrtos_kernel_tick");

    /* 是粘性的：再崩只加计数，原因与时刻不动。 */
    arkrtos_kernel_tick = 2000u;
    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT), 4ul,
         "fault after disable keeps counting");
    ck_u((unsigned long)arkrtos_crash_disabled(0), (unsigned long)ARKRTOS_CRASH_REASON_AUDIT,
         "disable reason is sticky");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(0), 1000ul,
         "disable tick is sticky");
    ck_u((unsigned long)arkrtos_crash_last_reason(0),
         (unsigned long)ARKRTOS_CRASH_REASON_FAULT, "last reason still updates");

    /* 没有池地址的槽位：只有 RAM 计数，日志不许被涂。 */
    {
        uint32 seq_before = arkrtos_crash_seq();
        g_pt.slot_base[3] = 0u;
        ck_u((unsigned long)arkrtos_crash_fault(3u, ARKRTOS_CRASH_REASON_FAULT), 1ul,
             "untagged slot counts in RAM");
        ck_u((unsigned long)arkrtos_crash_count(3), 0ul, "untagged slot leaves journal alone");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
             "untagged slot does not commit");
        g_pt.slot_base[3] = BASE2;
    }

    /* 地址变了（重装复用记录）：旧账不是这个镜像的，从 1 重新算。 */
    {
        uint32 seq_before;
        arkrtos_crash_journal.base[1] = 0x00ABCDEFu;
        arkrtos_crash_journal.crash_cnt[1] = 6u;
        arkrtos_crash_journal.last_reason[1] = ARKRTOS_CRASH_REASON_FAULT;
        arkrtos_crash_journal.dis_reason[1] = ARKRTOS_CRASH_REASON_FAULT;
        arkrtos_crash_journal.dis_tick[1] = 77u;
        seq_before = arkrtos_crash_seq();
        ck_u((unsigned long)arkrtos_crash_fault(1u, ARKRTOS_CRASH_REASON_HEARTBEAT), 1ul,
             "moved image starts the count at 1");
        ck_u((unsigned long)arkrtos_crash_journal.base[1], (unsigned long)BASE1,
             "moved image re-tags the base");
        ck_u((unsigned long)arkrtos_crash_disabled(1), 0ul,
             "moved image clears the old disable");
        ck_u((unsigned long)arkrtos_crash_disabled_tick(1), 0ul,
             "moved image clears the old disable tick");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
             "moved image commits once");
    }

    /* 越界槽位：返回 0 且什么都不动。 */
    {
        uint32 seq_before = arkrtos_crash_seq();
        ck_u((unsigned long)arkrtos_crash_fault(4u, ARKRTOS_CRASH_REASON_FAULT), 0ul,
             "fault beyond slot_max returns 0");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
             "fault beyond slot_max does not commit");
        ck_u((unsigned long)arkrtos_crash_fault(99u, ARKRTOS_CRASH_REASON_FAULT), 0ul,
             "fault beyond the array returns 0");
    }

    /* 查询入口的越界行为：按「没有这本账」回答，而不是读越界。 */
    ck_u((unsigned long)arkrtos_crash_disabled(99u), 0ul, "disabled(oob) == 0");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(99u), 0ul, "disabled_tick(oob) == 0");
    ck_u((unsigned long)arkrtos_crash_count(99u), 0ul, "count(oob) == 0");
    ck_u((unsigned long)arkrtos_crash_last_reason(99u),
         (unsigned long)ARKRTOS_CRASH_REASON_NONE, "last_reason(oob) == NONE");

    /* 临界区必须成对（fault 的每一段都在临界区里改账）。 */
    ck_u(g_crit_enters, g_crit_exits, "critical sections are paired");
}

/* ============================================================
 * 上限来自设备端配置：0 = 按编译期默认，不是「不限次」
 * ============================================================ */

static void test_limit(void)
{
    all_reset();
    arkrtos_crash_init();

    g_pt.cfg_restart_max = 2u;
    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT), 1ul,
         "limit 2: first fault");
    ck_u((unsigned long)arkrtos_crash_disabled(0), 0ul, "limit 2: not disabled at 1");
    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT), 2ul,
         "limit 2: second fault");
    ck_u((unsigned long)arkrtos_crash_disabled(0), (unsigned long)ARKRTOS_CRASH_REASON_FAULT,
         "limit 2: disabled at 2");

    /* 0 回落到编译期默认（3），不是「不限次」。 */
    all_reset();
    arkrtos_crash_init();
    g_pt.cfg_restart_max = 0u;
    (void)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT);
    (void)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT);
    ck_u((unsigned long)arkrtos_crash_disabled(0), 0ul, "cfg 0 keeps the compile default (2 < 3)");
    (void)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT);
    ck_u((unsigned long)arkrtos_crash_disabled(0), (unsigned long)ARKRTOS_CRASH_REASON_FAULT,
         "cfg 0 keeps the compile default (3 disables)");
}

/* ============================================================
 * forget / 名字 / 区域
 * ============================================================ */

static void test_forget_name_area(void)
{
    all_reset();
    arkrtos_crash_init();
    (void)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT);
    ck_u((unsigned long)arkrtos_crash_count(0), 1ul, "before forget: count 1");
    ck_u((unsigned long)g_pt.slot_crash_cnt[0], 1ul, "before forget: ptable mirror 1");

    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_forget(0u);
        ck_u((unsigned long)arkrtos_crash_count(0), 0ul, "forget clears the journal count");
        ck_u((unsigned long)g_pt.slot_crash_cnt[0], 0ul, "forget clears the ptable mirror");
        ck_u((unsigned long)arkrtos_crash_journal.base[0], 0ul, "forget clears the base");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
             "forget commits");
    }

    /* 越界 forget 是一个安全的空操作。 */
    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_forget(99u);
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
             "forget(oob) does not commit");
    }

    /* 原因名字：四个具名 + 一个兜底。 */
    ck(strcmp(arkrtos_crash_reason_name(ARKRTOS_CRASH_REASON_NONE), "none") == 0,
       "reason_name(NONE)");
    ck(strcmp(arkrtos_crash_reason_name(ARKRTOS_CRASH_REASON_FAULT), "crash-limit") == 0,
       "reason_name(FAULT)");
    ck(strcmp(arkrtos_crash_reason_name(ARKRTOS_CRASH_REASON_HEARTBEAT), "heartbeat") == 0,
       "reason_name(HEARTBEAT)");
    ck(strcmp(arkrtos_crash_reason_name(ARKRTOS_CRASH_REASON_AUDIT), "audit") == 0,
       "reason_name(AUDIT)");
    ck(strcmp(arkrtos_crash_reason_name(12345u), "unknown") == 0, "reason_name(unknown)");

    /* 区域读数就是编进镜像的那两个常量。 */
    ck_u((unsigned long)arkrtos_crash_area_base(), (unsigned long)CRASH_LOG_BASE,
         "area base is CRASH_LOG_BASE");
    ck_u((unsigned long)arkrtos_crash_area_size(), (unsigned long)CRASH_LOG_SIZE,
         "area size is CRASH_LOG_SIZE");
    ck((unsigned long)sizeof(arkrtos_crash_journal_t) <= (unsigned long)CRASH_LOG_SIZE,
       "journal fits the reserved region");
}

/* ============================================================
 * scan_apply：把账本对回这次开机重新扫出来的分区表
 * ============================================================ */

static void test_scan(void)
{
    /* 1) 记录重排：账在旧下标 k，地址属于新下标 i → 整条搬过去。 */
    all_reset();
    arkrtos_crash_init();
    arkrtos_crash_journal.base[2] = BASE0;       /* 旧下标 2 */
    arkrtos_crash_journal.crash_cnt[2] = 2u;
    arkrtos_crash_journal.last_reason[2] = ARKRTOS_CRASH_REASON_HEARTBEAT;
    arkrtos_crash_journal.dis_reason[2] = ARKRTOS_CRASH_REASON_NONE;
    arkrtos_crash_journal.dis_tick[2] = 0u;
    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_scan_apply();
        ck_u((unsigned long)arkrtos_crash_journal.base[0], (unsigned long)BASE0,
             "rebind: entry lands on the slot that owns the address");
        ck_u((unsigned long)arkrtos_crash_journal.crash_cnt[0], 2ul,
             "rebind: count moves with the entry");
        ck_u((unsigned long)arkrtos_crash_journal.crash_cnt[2], 0ul,
             "rebind: source entry is blanked");
        ck_u((unsigned long)g_pt.slot_crash_cnt[0], 2ul,
             "rebind: ptable mirror is published");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
             "rebind commits once");
    }

    /* 2) 已经在下标 0 上、地址也对：不搬、不提交。 */
    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_scan_apply();
        ck_u((unsigned long)arkrtos_crash_journal.crash_cnt[0], 2ul,
             "aligned entry keeps its count");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
             "no change: scan does not commit");
    }

    /* 3) 无人认领的账（镜像卸载了）：必须丢掉，不能等着下一个落点相同的
     *    镜像来背锅。 */
    arkrtos_crash_journal.base[3] = 0x00DEAD00u;
    arkrtos_crash_journal.crash_cnt[3] = 5u;
    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_scan_apply();
        ck_u((unsigned long)arkrtos_crash_journal.crash_cnt[3], 0ul,
             "unclaimed entry is dropped");
        ck_u((unsigned long)arkrtos_crash_journal.base[3], 0ul,
             "unclaimed entry loses its base");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
             "dropping commits once");
    }

    /* 4) 开机时就已经超限：槽位回到停用态，autostart 清掉、状态置 INVALID，
     *    但 entry 保留（'app list' 不能读成「槽位空了」）。 */
    all_reset();
    arkrtos_crash_init();
    arkrtos_crash_journal.base[0] = BASE0;
    arkrtos_crash_journal.crash_cnt[0] = 3u;
    arkrtos_crash_journal.last_reason[0] = ARKRTOS_CRASH_REASON_HEARTBEAT;
    arkrtos_crash_scan_apply();
    ck_u((unsigned long)arkrtos_crash_disabled(0), (unsigned long)ARKRTOS_CRASH_REASON_HEARTBEAT,
         "boot-time limit: disabled with the last reason");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(0), 0ul,
         "boot-time limit: disable tick is 0 (before this boot)");
    ck_u((unsigned long)g_pt.slot_autostart[0], 0ul, "boot-time limit: autostart cleared");
    ck_u(g_set_slot_calls, 1ul, "boot-time limit: set_slot called once");
    ck_u(g_set_slot_slot, 0ul, "boot-time limit: set_slot hits slot 0");
    ck_u(g_set_slot_state, (unsigned long)ARKRTOS_APP_SLOT_INVALID,
         "boot-time limit: slot marked INVALID");
    ck_u(g_set_slot_entry, (unsigned long)BASE0,
         "boot-time limit: entry is preserved");
    ck_u(g_set_slot_task, 0ul, "boot-time limit: task id cleared");

    /* 5) 最后一次故障没有原因（理论上不会），停用原因回落到 FAULT。 */
    all_reset();
    arkrtos_crash_init();
    arkrtos_crash_journal.base[1] = BASE1;
    arkrtos_crash_journal.crash_cnt[1] = 3u;
    arkrtos_crash_journal.last_reason[1] = ARKRTOS_CRASH_REASON_NONE;
    arkrtos_crash_scan_apply();
    ck_u((unsigned long)arkrtos_crash_disabled(1), (unsigned long)ARKRTOS_CRASH_REASON_FAULT,
         "boot-time limit with no reason falls back to FAULT");
    ck_u(g_set_slot_slot, 1ul, "fallback reason: set_slot hits slot 1");

    /* 6) 上一轮已经停用（日志活过复位）：保持停用，不再重写原因与时刻。 */
    all_reset();
    arkrtos_crash_init();
    arkrtos_crash_journal.base[0] = BASE0;
    arkrtos_crash_journal.crash_cnt[0] = 1u;
    arkrtos_crash_journal.dis_reason[0] = ARKRTOS_CRASH_REASON_AUDIT;
    arkrtos_crash_journal.dis_tick[0] = 4242u;
    arkrtos_crash_scan_apply();
    ck_u((unsigned long)arkrtos_crash_disabled(0), (unsigned long)ARKRTOS_CRASH_REASON_AUDIT,
         "surviving disable keeps its reason");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(0), 4242ul,
         "surviving disable keeps its tick");
    ck_u((unsigned long)g_pt.slot_autostart[0], 0ul,
         "surviving disable clears autostart");
    ck_u(g_set_slot_calls, 1ul, "surviving disable re-marks the slot");

    /* 7) 已停用、且这一轮不是新发现的：每次开机都必须重新压一遍。操作员
     *    上一次会话里可能手工 'app start' 过，那会清掉 INVALID 与 autostart
     *    的压制；复位后账本还记着停用原因，所以这里不能“省掉”这次写入。
     *    但账本本身没有变化（dis_reason 早在 pass 3 之前就非 NONE），
     *    所以不该再 commit 一次。 */
    {
        uint32 seq_before = arkrtos_crash_seq();
        unsigned long calls_before = g_set_slot_calls;
        arkrtos_crash_scan_apply();
        ck_u(g_set_slot_calls, calls_before + 1ul,
             "second scan re-marks the slot (the hold is re-applied every boot)");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
             "second scan does not commit");
    }

    /* 8) slot_max 超过数组长度时按数组长度截断（不许读越界）。 */
    all_reset();
    arkrtos_crash_init();
    g_pt.slot_max = ARKRTOS_SLOT_ARRAY_MAX + 8u;
    arkrtos_crash_journal.base[15] = BASE2;
    arkrtos_crash_journal.crash_cnt[15] = 1u;
    g_pt.slot_type[15] = ARKRTOS_SLOT_APP;
    g_pt.slot_base[15] = BASE2;
    arkrtos_crash_scan_apply();
    ck_u((unsigned long)g_pt.slot_crash_cnt[15], 1ul,
         "slot_max is capped at the array length");

    /* 9) 落在本次 slot_max 之外的尾部条目：pass 1 根本不看那些下标，pass 2
     *    必须把它们清掉——否则表缩小之后（例如分区重新规划过），旧账会永远
     *    躺在数组尾巴上，等着某个将来恰好落到同一地址的镜像来继承。 */
    all_reset();
    arkrtos_crash_init();
    arkrtos_crash_journal.base[5] = 0x00BEEF00u;
    arkrtos_crash_journal.crash_cnt[5] = 4u;
    arkrtos_crash_journal.last_reason[5] = ARKRTOS_CRASH_REASON_HEARTBEAT;
    {
        uint32 seq_before = arkrtos_crash_seq();
        arkrtos_crash_scan_apply();
        ck_u((unsigned long)arkrtos_crash_journal.crash_cnt[5], 0ul,
             "tail entry beyond slot_max is dropped");
        ck_u((unsigned long)arkrtos_crash_journal.base[5], 0ul,
             "tail entry loses its base");
        ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
             "dropping a tail entry commits once");
    }
}

/* ============================================================
 * MiniApp：没有槽位、没有池地址，按路径哈希记账
 * ============================================================ */

static void test_mini(void)
{
    all_reset();
    arkrtos_crash_init();

    ck_u((unsigned long)arkrtos_crash_mini_fault(0u, ARKRTOS_CRASH_REASON_FAULT), 0ul,
         "mini_fault(hash 0) is refused");
    ck_u((unsigned long)arkrtos_crash_mini_disabled(0u), 0ul, "mini_disabled(0) == 0");
    ck_u((unsigned long)arkrtos_crash_mini_count(0u), 0ul, "mini_count(0) == 0");
    ck_u((unsigned long)arkrtos_crash_mini_disabled_tick(0u), 0ul, "mini_disabled_tick(0) == 0");

    {
        uint32 h = arkrtos_crash_hash_path("/apps/a.mini");
        ck_u((unsigned long)arkrtos_crash_mini_fault(h, ARKRTOS_CRASH_REASON_FAULT), 1ul,
             "mini first fault returns 1");
        ck_u((unsigned long)arkrtos_crash_mini_count(h), 1ul, "mini_count tracks the path");
        ck_u((unsigned long)arkrtos_crash_journal.mini_hash[0], (unsigned long)h,
             "mini entry stores the hash");
        ck_u((unsigned long)arkrtos_crash_mini_disabled(h), 0ul, "mini enabled at 1");

        (void)arkrtos_crash_mini_fault(h, ARKRTOS_CRASH_REASON_HEARTBEAT);
        arkrtos_kernel_tick = 5555u;
        ck_u((unsigned long)arkrtos_crash_mini_fault(h, ARKRTOS_CRASH_REASON_AUDIT), 3ul,
             "mini third fault returns 3");
        ck_u((unsigned long)arkrtos_crash_mini_disabled(h),
             (unsigned long)ARKRTOS_CRASH_REASON_AUDIT, "mini disabled with the reason");
        ck_u((unsigned long)arkrtos_crash_mini_disabled_tick(h), 5555ul,
             "mini disable tick recorded");
        (void)arkrtos_crash_mini_fault(h, ARKRTOS_CRASH_REASON_FAULT);
        ck_u((unsigned long)arkrtos_crash_mini_disabled(h),
             (unsigned long)ARKRTOS_CRASH_REASON_AUDIT, "mini disable is sticky");

        /* mini_at：越界不写；命中写满六个出参并返回 1。 */
        {
            uint32 hash = 0xFFFFu;
            uint32 cnt = 0xFFFFu;
            uint32 last = 0xFFFFu;
            uint32 hold = 0xFFFFu;
            uint32 tick = 0xFFFFu;
            ck_u((unsigned long)arkrtos_crash_mini_at(ARKRTOS_CRASH_MINI_MAX, &hash, &cnt,
                                                     &last, &hold, &tick), 0ul,
                 "mini_at(oob) returns 0");
            ck_u((unsigned long)hash, 0xFFFFul, "mini_at(oob) does not write hash");
            ck_u((unsigned long)arkrtos_crash_mini_at(0u, &hash, &cnt, &last, &hold, &tick),
                 1ul, "mini_at(0) returns 1 for a live entry");
            ck_u((unsigned long)hash, (unsigned long)arkrtos_crash_hash_path("/apps/a.mini"),
                 "mini_at returns the hash");
            ck_u((unsigned long)cnt, 4ul, "mini_at returns the count");
            ck_u((unsigned long)last, (unsigned long)ARKRTOS_CRASH_REASON_FAULT,
                 "mini_at returns the last reason");
            ck_u((unsigned long)hold, (unsigned long)ARKRTOS_CRASH_REASON_AUDIT,
                 "mini_at returns the hold reason");
            ck_u((unsigned long)tick, 5555ul, "mini_at returns the disable tick");
            /* 出参可以全给 0：只查存在性。 */
            ck_u((unsigned long)arkrtos_crash_mini_at(0u, 0, 0, 0, 0, 0), 1ul,
                 "mini_at accepts null out-parameters");
        }

        /* 空条目：返回 0，出参清零。 */
        {
            uint32 hash = 0xFFFFu;
            ck_u((unsigned long)arkrtos_crash_mini_at(7u, &hash, 0, 0, 0, 0), 0ul,
                 "mini_at(empty) returns 0");
            ck_u((unsigned long)hash, 0ul, "mini_at(empty) writes a zero hash");
        }

        /* forget：清掉这一条并提交；未知哈希与 0 是空操作。 */
        {
            uint32 seq_before = arkrtos_crash_seq();
            arkrtos_crash_mini_forget(0u);
            ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
                 "mini_forget(0) does not commit");
            arkrtos_crash_mini_forget(0xABCDEF01u);
            ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before,
                 "mini_forget(unknown) does not commit");
            arkrtos_crash_mini_forget(h);
            ck_u((unsigned long)arkrtos_crash_seq(), (unsigned long)seq_before + 1ul,
                 "mini_forget(known) commits");
            ck_u((unsigned long)arkrtos_crash_mini_count(h), 0ul,
                 "mini_forget clears the count");
            ck_u((unsigned long)arkrtos_crash_journal.mini_hash[0], 0ul,
                 "mini_forget clears the hash");
        }
    }

    /* 受害者选择：先填空位，其次挑「没停用且计数最小」的那条；全停用就
     * 老实返回 0，而不是编一个计数。 */
    {
        uint32 i;
        uint32 h_fresh = arkrtos_crash_hash_path("/apps/fresh.mini");
        all_reset();
        arkrtos_crash_init();

        /* 7 条已停用、1 条空 == 仍然有空位：新路径进空位。 */
        for(i = 0u; i < (uint32)ARKRTOS_CRASH_MINI_MAX - 1u; i++)
        {
            arkrtos_crash_journal.mini_hash[i] = 0x1000u + i;
            arkrtos_crash_journal.mini_cnt[i] = 3u;
            arkrtos_crash_journal.mini_dis_reason[i] = ARKRTOS_CRASH_REASON_FAULT;
        }
        ck_u((unsigned long)arkrtos_crash_mini_fault(h_fresh, ARKRTOS_CRASH_REASON_FAULT),
             1ul, "empty mini slot is used first");
        ck_u((unsigned long)arkrtos_crash_journal.mini_hash[7], (unsigned long)h_fresh,
             "the fresh path lands in the empty slot");

        /* 全部停用：没有地方可写，返回 0。 */
        for(i = 0u; i < (uint32)ARKRTOS_CRASH_MINI_MAX; i++)
        {
            arkrtos_crash_journal.mini_dis_reason[i] = ARKRTOS_CRASH_REASON_FAULT;
        }
        ck_u((unsigned long)arkrtos_crash_mini_fault(arkrtos_crash_hash_path("/apps/x.mini"),
                                                    ARKRTOS_CRASH_REASON_FAULT), 0ul,
             "all-disabled table refuses a new path");

        /* 有一个未停用且计数最小：复用它的条目，计数从 1 重来。 */
        all_reset();
        arkrtos_crash_init();
        for(i = 0u; i < (uint32)ARKRTOS_CRASH_MINI_MAX; i++)
        {
            arkrtos_crash_journal.mini_hash[i] = 0x2000u + i;
            arkrtos_crash_journal.mini_cnt[i] = 8u;
            arkrtos_crash_journal.mini_dis_reason[i] = ARKRTOS_CRASH_REASON_FAULT;
        }
        arkrtos_crash_journal.mini_cnt[5] = 2u;
        arkrtos_crash_journal.mini_dis_reason[5] = ARKRTOS_CRASH_REASON_NONE;
        ck_u((unsigned long)arkrtos_crash_mini_fault(h_fresh, ARKRTOS_CRASH_REASON_FAULT),
             1ul, "least-faulted enabled entry is recycled, count restarts at 1");
        ck_u((unsigned long)arkrtos_crash_journal.mini_hash[5], (unsigned long)h_fresh,
             "the recycled entry takes the new hash");
    }
}

/* ============================================================ */

static void run_on(void)
{
    test_hash();
    test_init();
    test_fault();
    test_limit();
    test_forget_name_area();
    test_scan();
    test_mini();

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if(g_fails != 0)
    {
        printf("CRASH-HOST FAILED %d/%d\n", g_fails, g_checks);
        return;
    }
    printf("CRASH-HOST OK  %d checks\n", g_checks);
}

#else   /* ARKRTOS_USE_CRASH_LOG == 0 */

static void run_off(void)
{
    uint32 hash = 0xFFFFu;
    uint32 cnt = 0xFFFFu;
    uint32 last = 0xFFFFu;
    uint32 hold = 0xFFFFu;
    uint32 tick = 0xFFFFu;

    test_hash();

    pt_reset();
    g_pt.slot_crash_cnt[0] = 9u;
    arkrtos_crash_init();
    arkrtos_crash_scan_apply();
    ck_u((unsigned long)g_pt.slot_crash_cnt[0], 9ul,
         "OFF: init/scan do not touch the partition table");
    ck_u((unsigned long)arkrtos_crash_fault(0u, ARKRTOS_CRASH_REASON_FAULT), 0ul,
         "OFF: fault always returns 0");
    ck_u((unsigned long)arkrtos_crash_count(0u), 0ul, "OFF: count always 0");
    ck_u((unsigned long)arkrtos_crash_disabled(0u), 0ul, "OFF: nothing is ever disabled");
    ck_u((unsigned long)arkrtos_crash_disabled_tick(0u), 0ul, "OFF: no disable tick");
    ck_u((unsigned long)arkrtos_crash_last_reason(0u), 0ul, "OFF: no last reason");
    arkrtos_crash_forget(0u);
    ck_u((unsigned long)arkrtos_crash_boots(), 0ul, "OFF: no boot counter");
    ck_u((unsigned long)arkrtos_crash_seq(), 0ul, "OFF: no sequence");
    ck(strcmp(arkrtos_crash_reason_name(0u), "none") == 0, "OFF: reason_name(0)");
    ck(strcmp(arkrtos_crash_reason_name(1u), "crash journal disabled") == 0,
       "OFF: reason_name(other) says the journal is off");
    ck_u((unsigned long)arkrtos_crash_area_base(), 0ul, "OFF: no area base");
    ck_u((unsigned long)arkrtos_crash_area_size(), 0ul, "OFF: no area size");

    ck_u((unsigned long)arkrtos_crash_mini_fault(0x1234u, ARKRTOS_CRASH_REASON_FAULT), 0ul,
         "OFF: mini_fault returns 0");
    ck_u((unsigned long)arkrtos_crash_mini_count(0x1234u), 0ul, "OFF: mini_count 0");
    ck_u((unsigned long)arkrtos_crash_mini_disabled(0x1234u), 0ul, "OFF: mini_disabled 0");
    ck_u((unsigned long)arkrtos_crash_mini_disabled_tick(0x1234u), 0ul,
         "OFF: mini_disabled_tick 0");
    ck_u((unsigned long)arkrtos_crash_mini_at(0u, &hash, &cnt, &last, &hold, &tick), 0ul,
         "OFF: mini_at returns 0");
    ck_u((unsigned long)hash, 0ul, "OFF: mini_at writes zero hash");
    ck_u((unsigned long)cnt, 0ul, "OFF: mini_at writes zero count");
    ck_u((unsigned long)last, 0ul, "OFF: mini_at writes zero last reason");
    ck_u((unsigned long)hold, 0ul, "OFF: mini_at writes zero hold reason");
    ck_u((unsigned long)tick, 0ul, "OFF: mini_at writes zero tick");
    ck_u((unsigned long)arkrtos_crash_mini_at(99u, 0, 0, 0, 0, 0), 0ul,
         "OFF: mini_at(oob) returns 0");
    arkrtos_crash_mini_forget(0x1234u);

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if(g_fails != 0)
    {
        printf("CRASH-HOST OFF FAILED %d/%d\n", g_fails, g_checks);
        return;
    }
    printf("CRASH-HOST OFF OK  %d checks\n", g_checks);
}

#endif /* ARKRTOS_USE_CRASH_LOG */

int main(void)
{
#if (ARKRTOS_USE_CRASH_LOG == 1)
    run_on();
#else
    run_off();
#endif
    return (g_fails == 0) ? 0 : 1;
}
