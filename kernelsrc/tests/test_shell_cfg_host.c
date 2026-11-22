/**
* @file test_shell_cfg_host.c
* @brief Host-side unit test for kernelsrc/shell/arkrtos_shell_cfg.c.
*
* What this proves: the two command families in that file do what they say on
* the host, with every refusal path executed:
*   - "mpool list" prints one row per live pool, silently skips a handle that
*     is alive but cannot be queried, and prints "(no pool)" only when nothing
*     was printed at all;
*   - "mpool test" reports each self-check individually and returns -1 as soon
*     as any of them fails (create / alloc-all / exhaust / stats / free-reuse /
*     reject-bad-free / duplicate name / tiny block / delete / alive-after-delete);
*   - "cfg show" prints the effective layout, the record verdict and the slot
*     table, and stops before the table when the slot count is zero;
*   - "cfg load" drains the console, then answers one flow byte per 128-byte
*     chunk: ACK for the first three, and for the last chunk either ACK after a
*     successful write or a reason line followed by NAK;
*   - "cfg clear" and the usage paths.
*
* What it does NOT prove:
*   - that a real UART delivers a 512-byte record intact: the console here is a
*     byte array with a programmable read shape;
*   - that the fixed-block pool really hands out non-overlapping memory: the
*     mempool below is a four-block bookkeeping stub, not the kernel pool
*     (that one has its own test, test_mempool_host.c);
*   - that arkrtos_layout_write() puts a record where the next boot looks for
*     it: the layout module is stubbed down to return codes;
*   - that the 5 s per-chunk timeout is wall-clock: task_wait_internal() only
*     counts milliseconds here.
*
* The fixture values are arbitrary but distinctive, so a swapped field is
* caught rather than passing by coincidence.
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

/* 可写副本：被测接口收 char *，而字面量是 const char[]（-Wwrite-strings）。 */
static char *W(const char *s)
{
    static char bufs[8][48];
    static unsigned idx;
    char *b = bufs[idx % 8u];
    size_t n = strlen(s);

    idx++;
    if (n >= sizeof(bufs[0])) {
        n = sizeof(bufs[0]) - 1u;
    }
    memcpy(b, s, n);
    b[n] = '\0';
    return b;
}

/* ---- stubs: shell core ------------------------------------------------ */

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

const char *cfg_mode_text(uint32 mode)
{
    return (mode == 2u) ? "MODE-TWO-S" : "MODE-OTHER-S";
}

const char *cfg_reclaim_text(uint32 rm)
{
    return (rm == 1u) ? "RECLAIM-MIN-S" : "RECLAIM-GLOBAL-S";
}

const char *cfg_source_text(uint32 src)
{
    return (src == ARKRTOS_LAYOUT_SOURCE_CONFIG) ? "config-S" : "default-S";
}

const char *cfg_type_text(uint32 t)
{
    return (t == ARKRTOS_CFG_TYPE_APP) ? "APP-S" : "DRV-S";
}

uint32 arkrtos_log_get_level(void)
{
    return 3u;
}

/* ---- stubs: task / device -------------------------------------------- */

static uint32 g_wait_ms;

void arkrtos_task_wait_internal(uint32 ms)
{
    g_wait_ms += ms;
}

static int32 g_uart_handle = 7;

int32 arkrtos_shell_uart_open(void)
{
    return g_uart_handle;
}

/* 串口读形状可编程：正常送字节 / 一直 0（超时）/ 先空转 N 次再送。 */
#define RX_MAX 4096u

static uint8  g_rx[RX_MAX];
static size_t g_rx_len;
static size_t g_rx_pos;
static uint32 g_rx_gate;
static int32  g_rx_chunk;
static int    g_rx_always_zero;

static void rx_set(const uint8 *bytes, size_t n)
{
    if (n > RX_MAX) {
        n = RX_MAX;
    }
    memcpy(g_rx, bytes, n);
    g_rx_len = n;
    g_rx_pos = 0u;
}

static void rx_reset(void)
{
    g_rx_len = 0u;
    g_rx_pos = 0u;
    g_rx_gate = 0u;
    g_rx_chunk = 0;
    g_rx_always_zero = 0;
}

int32 arkrtos_dev_read_internal(int32 handle, uint8 *pdata, int32 len)
{
    size_t avail;
    size_t n;
    size_t want;

    (void)handle;
    if (g_rx_always_zero) {
        return 0;
    }
    if (g_rx_gate > 0u) {
        g_rx_gate--;
        return 0;
    }
    avail = g_rx_len - g_rx_pos;
    if ((avail == 0u) || (len <= 0)) {
        return 0;
    }
    want = (size_t)len;
    n = (avail < want) ? avail : want;
    if ((g_rx_chunk > 0) && ((size_t)g_rx_chunk < n)) {
        n = (size_t)g_rx_chunk;
    }
    memcpy(pdata, g_rx + g_rx_pos, n);
    g_rx_pos += n;
    return (int32)n;
}

/* 流控字节收集 */
static uint8 g_flow[16];
static unsigned g_flow_n;

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    int32 i;

    (void)handle;
    for (i = 0; (i < len) && (g_flow_n < sizeof(g_flow)); i++) {
        g_flow[g_flow_n] = pdata[i];
        g_flow_n++;
    }
    return len;
}

static unsigned flow_count(uint8 v)
{
    unsigned i;
    unsigned c = 0u;

    for (i = 0u; i < g_flow_n; i++) {
        if (g_flow[i] == v) {
            c++;
        }
    }
    return c;
}

/* ---- stubs: layout ---------------------------------------------------- */

#define ST_SLOT_MAX 4u

static arkrtos_cfg_slot_t g_slots[ST_SLOT_MAX];
static uint32 g_slot_count;
static uint32 g_mode;
static uint32 g_source;
static uint32 g_reason;
static uint32 g_reclaim;
static uint32 g_cfg_log;
static uint32 g_cfg_restart;
static uint32 g_boot_delay;
static uint32 g_raw;

static uint32 g_validate_rc;
static int32  g_write_rc;
static uint32 g_write_erased;
static uint32 g_write_len;
static uint8  g_write_rec[ARKRTOS_CFG_RECORD_SIZE];
static int32  g_erase_rc;

uint32 arkrtos_layout_mode(void)
{
    return g_mode;
}

uint32 arkrtos_layout_source(void)
{
    return g_source;
}

uint32 arkrtos_layout_slot_count(void)
{
    return g_slot_count;
}

uint32 arkrtos_layout_reason(void)
{
    return g_reason;
}

uint32 arkrtos_layout_reclaim_mode(void)
{
    return g_reclaim;
}

uint32 arkrtos_layout_cfg_log_level(void)
{
    return g_cfg_log;
}

uint32 arkrtos_layout_cfg_restart_max(void)
{
    return g_cfg_restart;
}

uint32 arkrtos_layout_boot_delay_ms(void)
{
    return g_boot_delay;
}

uint32 arkrtos_layout_raw_allow(void)
{
    return g_raw;
}

const arkrtos_cfg_slot_t *arkrtos_layout_slots(void)
{
    return g_slots;
}

const char *arkrtos_layout_reason_text(uint32 reason)
{
    return (reason == ARKRTOS_CFG_ERR_CRC) ? "crc-bad-S" : "other-bad-S";
}

uint32 arkrtos_layout_validate(const arkrtos_cfg_record_t *rec)
{
    (void)rec;
    return g_validate_rc;
}

int32 arkrtos_layout_write(const uint8 *record, uint32 len, uint32 *p_erased)
{
    if (g_write_rc >= 0) {
        uint32 n = (len < ARKRTOS_CFG_RECORD_SIZE) ? len : ARKRTOS_CFG_RECORD_SIZE;

        memcpy(g_write_rec, record, (size_t)n);
        g_write_len = n;
        if (p_erased != NULL) {
            *p_erased = g_write_erased;
        }
    }
    return g_write_rc;
}

int32 arkrtos_layout_erase(void)
{
    return g_erase_rc;
}

/* ---- stubs: fixed-block pool ----------------------------------------- */

#define ST_BLOCKS     4
#define ST_BLOCK_SIZE 32u

static uint8 g_mp_area[ST_BLOCKS][ST_BLOCK_SIZE];
static int   g_mp_used[ST_BLOCKS];
static int   g_mp_alive[ARKRTOS_MEMPOOL_NUM];
static int   g_mp_created;
static int   g_mp_dup_accept;
static int   g_mp_tiny_accept;
static int   g_mp_alloc_starve;      /* >0：只允许这么多次成功分配 */
static int   g_mp_alloc_calls;
static int   g_mp_query_rc;
static int   g_mp_query_bad_stats;   /* 1：返回对不上的统计 */
static int   g_mp_free_rc;           /* 1：合法块也拒绝 */
static int   g_mp_bad_free_accept_at;/* 第几次非法释放放行（0=从不） */
static int   g_mp_bad_free_calls;
static int   g_mp_reuse_bad;         /* 1：释放一次之后发一块别的 */
static int   g_mp_freed_once;
static int   g_mp_exhaust_bad;       /* 1：池空了仍然发一块 */
static int   g_mp_delete_rc;
static int   g_mp_alive_after_delete;
static arkrtos_mempool_stat_t g_mp_stat;

static void mp_reset(void)
{
    memset(g_mp_used, 0, sizeof(g_mp_used));
    memset(g_mp_alive, 0, sizeof(g_mp_alive));
    g_mp_created = 0;
    g_mp_dup_accept = 0;
    g_mp_tiny_accept = 0;
    g_mp_alloc_starve = 0;
    g_mp_alloc_calls = 0;
    g_mp_query_rc = 0;
    g_mp_query_bad_stats = 0;
    g_mp_free_rc = 0;
    g_mp_bad_free_accept_at = 0;
    g_mp_bad_free_calls = 0;
    g_mp_reuse_bad = 0;
    g_mp_freed_once = 0;
    g_mp_exhaust_bad = 0;
    g_mp_delete_rc = 0;
    g_mp_alive_after_delete = 0;
    memset(&g_mp_stat, 0, sizeof(g_mp_stat));
}

int32 arkrtos_mempool_create_internal(const char *name, void *pool,
                                      uint32 block_size, uint32 block_count)
{
    (void)pool;
    if ((name == NULL) || (name[0] == '\0')) {
        return -1;
    }
    if ((block_size < (uint32)sizeof(void *)) ||
        ((block_size % (uint32)sizeof(void *)) != 0u)) {
        if (!g_mp_tiny_accept) {
            return -1;
        }
    }
    if (block_count == 0u) {
        return -1;
    }
    if (g_mp_created && (strcmp(name, "mptest") == 0) && !g_mp_dup_accept) {
        return -1;
    }
    memset(g_mp_used, 0, sizeof(g_mp_used));
    g_mp_created = 1;
    g_mp_alive[0] = 1;
    return 0;
}

void *arkrtos_mempool_alloc_internal(int32 handle)
{
    int i;

    if ((handle < 0) || (handle >= ARKRTOS_MEMPOOL_NUM)) {
        return NULL;
    }
    g_mp_alloc_calls++;
    if ((g_mp_alloc_starve > 0) && (g_mp_alloc_calls > g_mp_alloc_starve)) {
        return NULL;
    }
    if (g_mp_reuse_bad && g_mp_freed_once) {
        /* 故意绕过空闲链：释放过一块之后仍发一块不相干的内存 */
        return (void *)&g_mp_area[ST_BLOCKS - 2][0];
    }
    for (i = 0; i < ST_BLOCKS; i++) {
        if (!g_mp_used[i]) {
            g_mp_used[i] = 1;
            return (void *)&g_mp_area[i][0];
        }
    }
    if (g_mp_exhaust_bad) {
        return (void *)&g_mp_area[0][0];
    }
    return NULL;
}

int32 arkrtos_mempool_free_internal(int32 handle, void *p)
{
    int i;

    if ((handle < 0) || (p == NULL)) {
        return -1;
    }
    for (i = 0; i < ST_BLOCKS; i++) {
        if (p == (void *)&g_mp_area[i][0]) {
            if (g_mp_free_rc) {
                return -1;
            }
            g_mp_used[i] = 0;
            g_mp_freed_once = 1;
            return 0;
        }
    }
    g_mp_bad_free_calls++;
    if ((g_mp_bad_free_accept_at > 0) &&
        (g_mp_bad_free_calls >= g_mp_bad_free_accept_at)) {
        return 0;
    }
    return -1;
}

int32 arkrtos_mempool_delete_internal(int32 handle)
{
    if ((handle < 0) || (handle >= ARKRTOS_MEMPOOL_NUM)) {
        return -1;
    }
    if (g_mp_delete_rc) {
        return -1;
    }
    g_mp_created = 0;
    g_mp_alive[handle] = g_mp_alive_after_delete ? 1 : 0;
    return 0;
}

int32 arkrtos_mempool_is_alive(int32 handle)
{
    if ((handle < 0) || (handle >= ARKRTOS_MEMPOOL_NUM)) {
        return 0;
    }
    return g_mp_alive[handle];
}

int32 arkrtos_mempool_query_internal(int32 handle, arkrtos_mempool_stat_t *out)
{
    int i;
    uint32 used = 0u;

    if ((handle < 0) || (out == NULL) || (g_mp_query_rc != 0)) {
        return -1;
    }
    for (i = 0; i < ST_BLOCKS; i++) {
        if (g_mp_used[i]) {
            used++;
        }
    }
    memset(&g_mp_stat, 0, sizeof(g_mp_stat));
    memcpy(g_mp_stat.name, "mptest", 7u);
    g_mp_stat.block_size = ST_BLOCK_SIZE;
    g_mp_stat.block_count = ST_BLOCKS;
    g_mp_stat.free_count = (uint32)ST_BLOCKS - used;
    g_mp_stat.alloc_ok = 5u;
    g_mp_stat.alloc_fail = 1u;
    g_mp_stat.free_bad = 2u;
    g_mp_stat.peak_used = 4u;
    if (g_mp_query_bad_stats) {
        g_mp_stat.free_count = 3u;
        g_mp_stat.alloc_fail = 0u;
    }
    *out = g_mp_stat;
    return 0;
}

/* ------------------------------------------------------------ fixtures */

static void fixture_reset(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    g_slot_count = 0u;
    g_mode = 0u;
    g_source = ARKRTOS_LAYOUT_SOURCE_DEFAULT;
    g_reason = ARKRTOS_CFG_OK;
    g_reclaim = 0u;
    g_cfg_log = 0u;
    g_cfg_restart = 0u;
    g_boot_delay = 0u;
    g_raw = 0u;
    g_validate_rc = ARKRTOS_CFG_OK;
    g_write_rc = 1;
    g_write_erased = 0u;
    g_write_len = 0u;
    memset(g_write_rec, 0, sizeof(g_write_rec));
    g_erase_rc = 0;
    g_flow_n = 0u;
    g_wait_ms = 0u;
    rx_reset();
    mp_reset();
}

/* ------------------------------------------------------------ mpool list */

static void t_mpool_list_none(void)
{
    int rc;

    fixture_reset();
    cap_reset();
    rc = cmd_mpool(1, (char *[]){ W("mpool") });
    CK_I(rc, 0);
    CK(out_has("memory pools (fixed-block)"));
    CK(out_has("(no pool)"));
    CK_I(out_count("block "), 0);
}

static void t_mpool_list_rows(void)
{
    int rc;

    fixture_reset();
    g_mp_alive[0] = 1;
    g_mp_alive[2] = 1;          /* alive but query fails -> skipped */
    memset(g_mp_used, 1, sizeof(g_mp_used));
    g_mp_query_rc = 0;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("list") });
    CK_I(rc, 0);
    CK(!out_has("(no pool)"));
    CK_I(out_count("block 32 B x 4"), 2);
    CK(out_has("free 0  used 4 (peak 4)"));
    CK(out_has("alloc 5  fail 1  bad-free 2"));
    CK(out_has("#0 mptest"));
}

static void t_mpool_list_query_fail(void)
{
    int rc;

    fixture_reset();
    g_mp_alive[1] = 1;
    g_mp_query_rc = -1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("list") });
    CK_I(rc, 0);
    CK(out_has("(no pool)"));
}

static void t_mpool_usage(void)
{
    int rc;

    fixture_reset();
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("bogus") });
    CK_I(rc, -1);
    CK(out_has("usage: mpool [list | test]"));
}

/* ------------------------------------------------------------ mpool test */

/* 每次跑完把结论行取出来，断言 PASS/FAIL 与失败行数。 */
static void t_selftest_pass(void)
{
    int rc;

    fixture_reset();
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, 0);
    CK(out_has("mempool selftest (fixed-block pool)"));
    CK(out_has("ok   create"));
    CK(out_has("ok   alloc all 4 blocks"));
    CK(out_has("ok   exhaust -> 0"));
    CK(out_has("ok   stats free=0 fail=1"));
    CK(out_has("ok   free + reuse same block"));
    CK(out_has("ok   reject bad free"));
    CK(out_has("ok   reject duplicate name"));
    CK(out_has("ok   reject tiny block"));
    CK(out_has("ok   delete"));
    CK(out_has("failed 0 -- ALL PASS"));
    CK(!out_has("FAIL"));
}

static void t_selftest_create_fail(void)
{
    int rc;

    fixture_reset();
    g_mp_alive[0] = 0;
    g_mp_tiny_accept = 1;   /* 让 create 走到 tiny 分支后仍失败：直接用名字空 */
    cap_reset();
    /* 用一个会让 create 失败的形状：block_count 0 不可由被测代码触发，
     * 这里改用「tiny 分支 + 不放行」的组合由 g_mp_tiny_accept 控制：
     * 恒失败由下面的 dup 桩不适用，故直接让 create 返回 -1 的办法是把
     * 池名表占满 —— 用 g_mp_created 预置 + 名字 mptest 即为重复，且
     * g_mp_dup_accept=0，create 必返回 -1。 */
    g_mp_created = 1;
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL create"));
    CK(!out_has("ok   create"));
    CK(out_has("failed 1 -- FAIL"));
}

static void t_selftest_alloc_starve(void)
{
    int rc;

    fixture_reset();
    g_mp_alloc_starve = 2;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL alloc all (2/4)"));
    CK(out_has("failed "));
}

static void t_selftest_exhaust_bad(void)
{
    int rc;

    fixture_reset();
    g_mp_exhaust_bad = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL exhaust (empty pool handed out a block)"));
}

static void t_selftest_query_fail(void)
{
    int rc;

    fixture_reset();
    g_mp_query_rc = -1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL query"));
}

static void t_selftest_stats_bad(void)
{
    int rc;

    fixture_reset();
    g_mp_query_bad_stats = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL stats free=3 fail=0"));
}

static void t_selftest_free_fail(void)
{
    int rc;

    fixture_reset();
    g_mp_free_rc = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL free"));
    /* 释放失败时复用检查属于 else-if，不会单独报，这里只确认它没被误报 */
    CK(!out_has("FAIL reuse"));
}

static void t_selftest_reuse_bad(void)
{
    int rc;

    fixture_reset();
    g_mp_reuse_bad = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL reuse (did not hand back the same block)"));
}

static void t_selftest_bad_free_range(void)
{
    int rc;

    fixture_reset();
    g_mp_bad_free_accept_at = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL reject out-of-range free"));
}

static void t_selftest_bad_free_misaligned(void)
{
    int rc;

    fixture_reset();
    g_mp_bad_free_accept_at = 2;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(!out_has("FAIL reject out-of-range free"));
    CK(out_has("FAIL reject misaligned free"));
}

static void t_selftest_dup_accepted(void)
{
    int rc;

    fixture_reset();
    g_mp_dup_accept = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL reject duplicate name"));
}

static void t_selftest_tiny_accepted(void)
{
    int rc;

    fixture_reset();
    g_mp_tiny_accept = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL reject tiny block"));
}

static void t_selftest_delete_fail(void)
{
    int rc;

    fixture_reset();
    g_mp_delete_rc = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL delete"));
}

static void t_selftest_alive_after_delete(void)
{
    int rc;

    fixture_reset();
    g_mp_alive_after_delete = 1;
    cap_reset();
    rc = cmd_mpool(2, (char *[]){ W("mpool"), W("test") });
    CK_I(rc, -1);
    CK(out_has("FAIL alive after delete"));
}

/* ------------------------------------------------------------ cfg recv */

static void t_cfg_recv_exact(void)
{
    uint8 buf[8];
    uint8 src[8] = { 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h' };
    int32 rc;

    fixture_reset();
    rx_set(src, sizeof(src));
    memset(buf, 0, sizeof(buf));
    rc = cfg_recv_exact(7, buf, sizeof(buf), 100u);
    CK_I(rc, 0);
    CK(memcmp(buf, src, sizeof(src)) == 0);
    CK_I(g_wait_ms, 0u);

    /* 一次只给 3 字节：循环必须拼起来 */
    fixture_reset();
    rx_set(src, sizeof(src));
    g_rx_chunk = 3;
    memset(buf, 0, sizeof(buf));
    rc = cfg_recv_exact(7, buf, sizeof(buf), 100u);
    CK_I(rc, 0);
    CK(memcmp(buf, src, sizeof(src)) == 0);

    /* 一直收不到：累计到 timeout 后放弃 */
    fixture_reset();
    g_rx_always_zero = 1;
    rc = cfg_recv_exact(7, buf, sizeof(buf), 100u);
    CK_I(rc, -1);
    CK_I(g_wait_ms, 100u);
}

/* ------------------------------------------------------------ cfg show */

static void t_cfg_show_default(void)
{
    int rc;

    fixture_reset();
    cap_reset();
    rc = cmd_cfg(1, (char *[]){ W("cfg") });
    CK_I(rc, 0);
    CK(out_has("layout    : MODE-OTHER-S (source: default-S)"));
    CK(out_has("config    : 0x"));
    CK(out_has("record(s) per sector"));
    CK(out_has("reclaim   : RECLAIM-GLOBAL-S"));
    CK(out_has("knobs     : log 3 (record 0), restart max 0"));
    CK(out_has("boot      : hold 0 ms before autostart, raw images refused"));
    CK(out_has("record    : none, compile-time default in use"));
    CK(!out_has("slot  type"));
}

static void t_cfg_show_accepted(void)
{
    int rc;

    fixture_reset();
    g_source = ARKRTOS_LAYOUT_SOURCE_CONFIG;
    g_mode = 2u;
    g_reclaim = 1u;
    g_cfg_log = 4u;
    g_cfg_restart = 5u;
    g_boot_delay = 1500u;
    g_raw = 1u;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("show") });
    CK_I(rc, 0);
    CK(out_has("layout    : MODE-TWO-S (source: config-S)"));
    CK(out_has("reclaim   : RECLAIM-MIN-S"));
    CK(out_has("knobs     : log 3 (record 4), restart max 5"));
    CK(out_has("hold 1500 ms before autostart, raw images allowed"));
    CK(out_has("record    : accepted"));
    CK(!out_has("compile-time default in use"));
}

static void t_cfg_show_rejected_and_slots(void)
{
    int rc;

    fixture_reset();
    g_reason = ARKRTOS_CFG_ERR_CRC;
    g_slot_count = 2u;
    g_slots[0].type = ARKRTOS_CFG_TYPE_APP;
    g_slots[0].base = 0x11220000u;
    g_slots[0].size = 0x10000u;
    g_slots[0].ram_base = 0x55660000u;
    g_slots[0].autostart = 1u;
    g_slots[1].type = ARKRTOS_CFG_TYPE_DRIVER;
    g_slots[1].base = 0x33440000u;
    g_slots[1].size = 0x20000u;
    g_slots[1].ram_base = 0x77880000u;
    g_slots[1].autostart = 0u;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("show") });
    CK_I(rc, 0);
    CK(out_has("record    : rejected, crc-bad-S"));
    CK(out_has("slot  type  base        size     ram         auto"));
    CK(out_has("APP-S"));
    CK(out_has("DRV-S"));
    CK(out_has("0x11220000"));
    CK(out_has("0x33440000"));
    CK(out_has("0x55660000"));
    CK(out_has(" 0    APP-S"));
    CK(out_has(" 1    DRV-S"));
    CK_I(out_count("slot  type"), 1);
}

/* ------------------------------------------------------------ cfg load */

static void fill_record(uint8 *rec)
{
    uint32 i;

    for (i = 0u; i < ARKRTOS_CFG_RECORD_SIZE; i++) {
        rec[i] = (uint8)(i & 0xFFu);
    }
}

static void t_cfg_load_ok_erased(void)
{
    uint8 rec[ARKRTOS_CFG_RECORD_SIZE];
    int rc;

    fixture_reset();
    fill_record(rec);
    rx_set(rec, sizeof(rec));
    g_rx_gate = 1u;             /* 排空那一读先在空缓冲上 */
    g_write_rc = 3;
    g_write_erased = 1u;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("load") });
    CK_I(rc, 0);
    CK(out_has("cfg load: ready, send 512 bytes as 4 chunks of 128"));
    CK(out_has("one flow byte per chunk"));
    CK(out_has("cfg load: ok, record 3 (region erased first)"));
    CK(out_has("applied immediately"));
    CK(out_has("reboot recommended"));
    CK_I(flow_count(0x06u), 4u);
    CK_I(flow_count(0x15u), 0u);
    CK_I(g_write_len, ARKRTOS_CFG_RECORD_SIZE);
    CK_I(memcmp(g_write_rec, rec, sizeof(rec)), 0);
}

static void t_cfg_load_ok_plain(void)
{
    uint8 rec[ARKRTOS_CFG_RECORD_SIZE];
    int rc;

    fixture_reset();
    fill_record(rec);
    rx_set(rec, sizeof(rec));
    g_rx_gate = 1u;             /* 排空那一读先在空缓冲上 */
    g_write_rc = 7;
    g_write_erased = 0u;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("load") });
    CK_I(rc, 0);
    CK(out_has("cfg load: ok, record 7\r\n"));
    CK(!out_has("region erased first"));
    CK_I(flow_count(0x06u), 4u);
}

static void t_cfg_load_timeout(void)
{
    int rc;

    fixture_reset();
    g_rx_always_zero = 1;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("load") });
    CK_I(rc, 0);
    CK(out_has("cfg load: timed out waiting for the record"));
    CK_I(flow_count(0x06u), 0u);
    CK_I(flow_count(0x15u), 1u);
    CK_I(g_wait_ms, 5000u);
    CK_I(g_write_len, 0u);
}

static void t_cfg_load_validate_fail(void)
{
    uint8 rec[ARKRTOS_CFG_RECORD_SIZE];
    int rc;

    fixture_reset();
    fill_record(rec);
    rx_set(rec, sizeof(rec));
    g_rx_gate = 1u;
    g_validate_rc = ARKRTOS_CFG_ERR_CRC;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("load") });
    CK_I(rc, 0);
    CK(out_has("cfg load: record rejected, crc-bad-S"));
    CK_I(flow_count(0x06u), 3u);
    CK_I(flow_count(0x15u), 1u);
    CK_I(g_write_len, 0u);
}

static void t_cfg_load_write_fail(void)
{
    uint8 rec[ARKRTOS_CFG_RECORD_SIZE];
    int rc;

    fixture_reset();
    fill_record(rec);
    rx_set(rec, sizeof(rec));
    g_rx_gate = 1u;
    g_write_rc = -5;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("load") });
    CK_I(rc, 0);
    CK(out_has("cfg load: write failed (-5), config region not usable"));
    CK_I(flow_count(0x06u), 3u);
    CK_I(flow_count(0x15u), 1u);
}

static void t_cfg_load_drain_full(void)
{
    uint8 stream[64u * 32u + ARKRTOS_CFG_RECORD_SIZE];
    int rc;

    fixture_reset();
    memset(stream, 0x5A, sizeof(stream));
    fill_record(&stream[64u * 32u]);
    rx_set(stream, sizeof(stream));
    g_write_rc = 1;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("load") });
    CK_I(rc, 0);
    CK(out_has("cfg load: ok, record 1\r\n"));
    CK_I(flow_count(0x06u), 4u);
    /* 记录没被排空吃掉：写进去的就是原来那 512 字节 */
    CK_I(memcmp(g_write_rec, &stream[64u * 32u], ARKRTOS_CFG_RECORD_SIZE), 0);
}

/* ------------------------------------------------------------ cfg clear */

static void t_cfg_clear_ok(void)
{
    int rc;

    fixture_reset();
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("clear") });
    CK_I(rc, 0);
    CK(out_has("cfg clear: config region erased, compile-time default layout in use"));
    CK(out_has("cfg clear: reboot recommended"));
}

static void t_cfg_clear_fail(void)
{
    int rc;

    fixture_reset();
    g_erase_rc = -3;
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("clear") });
    CK_I(rc, -1);
    CK(out_has("cfg clear: failed (-3)"));
}

static void t_cfg_usage(void)
{
    int rc;

    fixture_reset();
    cap_reset();
    rc = cmd_cfg(2, (char *[]){ W("cfg"), W("bogus") });
    CK_I(rc, -1);
    CK(out_has("usage: cfg [show | load | clear]"));
}

/* ------------------------------------------------------------ main */

int main(void)
{
    t_mpool_list_none();
    t_mpool_list_rows();
    t_mpool_list_query_fail();
    t_mpool_usage();

    t_selftest_pass();
    t_selftest_create_fail();
    t_selftest_alloc_starve();
    t_selftest_exhaust_bad();
    t_selftest_query_fail();
    t_selftest_stats_bad();
    t_selftest_free_fail();
    t_selftest_reuse_bad();
    t_selftest_bad_free_range();
    t_selftest_bad_free_misaligned();
    t_selftest_dup_accepted();
    t_selftest_tiny_accepted();
    t_selftest_delete_fail();
    t_selftest_alive_after_delete();

    t_cfg_recv_exact();

    t_cfg_show_default();
    t_cfg_show_accepted();
    t_cfg_show_rejected_and_slots();

    t_cfg_load_ok_erased();
    t_cfg_load_ok_plain();
    t_cfg_load_timeout();
    t_cfg_load_validate_fail();
    t_cfg_load_write_fail();
    t_cfg_load_drain_full();

    t_cfg_clear_ok();
    t_cfg_clear_fail();
    t_cfg_usage();

    if (g_failed != 0) {
        printf("SHELL-CFG-HOST FAIL  %d/%d checks failed\n", g_failed, g_checks);
        return 1;
    }
    printf("SHELL-CFG-HOST OK  %d checks\n", g_checks);
    return 0;
}
