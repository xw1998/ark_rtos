/**
* @file test_audit_log_host.c
* @brief 持久审计日志环（arkrtos_audit_log.c）的宿主用例
* @details 被测对象只有一个职责：把「谁在什么时候做了什么、被拒绝了没有」
*          写进卷上的一个环形文件，并且**下次开机还读得回来**。所以这里钉住
*          的不是「能不能写」，而是那几条会静默撒谎的地方：
*
*            - 一条记录只有在自己的 crc 说它是记录时才算内容。截断的尾记录
*              必须被识别成 torn（bad_crc），不能把半条记录的字段当成一次真事件；
*            - 记录先落盘、指针（头部）后落盘。所以断电截断的那条记录一定落在
*              可读窗口之外——**这是顺序本身给出的保证**，不是扫描给的；
*            - 头部本身坏了才扫描。重建从槽位里按 seq 把最新的那条找回来，
*              并把新头部写回卷上（下次开机不该再重建第二次）；
*            - 写没落全就是丢了一条，RAM 里的环位置要跟着回滚。位置前进了而记录
*              没落盘，下次开机会把空槽当成最新记录，再下一次就覆盖它。
*
*          两条容易读错的语义，用例特意分开：
*            1. 开一次就追加一条 LOG_OPEN（code = 回收到的记录条数）。所以
*               「重挂载之后 used 多一条、最新一条是 LOG_OPEN」是正确行为，
*               不是泄漏；断言的写法必须把这个 +1 算进去。
*            2. 头部完好时不扫描，因此 torn 槽位在正常重启里**不**计入 bad_crc
*               （它在窗口外，谁也读不到它）；只有头部坏了触发扫描时才会被数出来。
*
*          宿主上跑它要先接受一件事：**这里的卷是内存数组**，所以本用例证明的是
*          「记录的格式与扫描/重建逻辑对」，证明不了「真 Flash 上跨复位还在」——
*          那条只在真板上成立。
*
*          桩只有五个外部符号：fs 的 mounted/read_at/write_at、内核 tick、crc32。
*          crc32 用真实现（不是恒等），否则 torn 记录这一整类判断就是在自证。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_audit_log.h"
#include "arkrtos_features.h"

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

#if (ARKRTOS_USE_AUDIT_LOG == 1)

/* ---------------- 假卷 ---------------- */

#define VOL_RECORDS  ((uint32)ARKRTOS_AUDIT_LOG_RECORDS)
#define VOL_BYTES    (32u + (VOL_RECORDS * 32u))

static uint8  g_vol[VOL_BYTES];
static uint32 g_vol_len;          /* bytes that "exist" on the file */
static int    g_mounted;
static int    g_fail_write;       /* 1 = every write_at fails */
static uint32 g_tick;

static void vol_reset(void)
{
    uint32 i;

    for(i = 0u; i < VOL_BYTES; i++)
    {
        g_vol[i] = 0u;
    }
    g_vol_len = 0u;
    g_mounted = 1;
    g_fail_write = 0;
    g_tick = 0u;
}

uint8 arkrtos_fs_mounted(void)
{
    return (g_mounted != 0) ? 1u : 0u;
}

int32 arkrtos_fs_read_at(const char *path, uint8 *buf, uint32 len, uint32 off,
                         uint32 *out_len)
{
    uint32 avail;

    (void)path;
    *out_len = 0u;
    if((buf == 0) || (g_mounted == 0))
    {
        return -1;
    }
    if(off >= g_vol_len)
    {
        return 0;       /* past EOF: zero bytes, not an error */
    }
    avail = g_vol_len - off;
    if(avail > len)
    {
        avail = len;
    }
    memcpy(buf, &g_vol[off], avail);
    *out_len = avail;
    return 0;
}

int32 arkrtos_fs_write_at(const char *path, const uint8 *buf, uint32 len,
                          uint32 off)
{
    (void)path;
    if((buf == 0) || (g_mounted == 0) || (g_fail_write != 0))
    {
        return -1;
    }
    if((off + len) > VOL_BYTES)
    {
        return -1;      /* the file cannot grow past the ring it was sized for */
    }
    memcpy(&g_vol[off], buf, len);
    if((off + len) > g_vol_len)
    {
        g_vol_len = off + len;
    }
    return 0;
}

uint32 arkrtos_kernel_get_tick(void)
{
    return g_tick;
}

/* The real thing, so that a torn record is really torn: if this were a stub
 * the "crc rejects a cut slot" case would pass no matter what the module did. */
uint32 arkrtos_crc32(const void *data, uint32 len, uint32 crc)
{
    const uint8 *p = (const uint8 *)data;
    uint32 i;
    int b;

    crc = ~crc;
    for(i = 0u; i < len; i++)
    {
        crc ^= (uint32)p[i];
        for(b = 0; b < 8; b++)
        {
            crc = ((crc & 1u) != 0u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return ~crc;
}

/* ---------------- helpers ---------------- */

static uint32 rd32(const uint8 *p)
{
    return (uint32)p[0] | ((uint32)p[1] << 8) |
           ((uint32)p[2] << 16) | ((uint32)p[3] << 24);
}

static void wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)(v & 0xFFu);
    p[1] = (uint8)((v >> 8) & 0xFFu);
    p[2] = (uint8)((v >> 16) & 0xFFu);
    p[3] = (uint8)((v >> 24) & 0xFFu);
}

static uint8 *slot(uint32 i)
{
    return &g_vol[32u + (i * 32u)];
}

/* Re-open as if this were the next boot. The module keeps its ring position in
 * RAM and there is no public way to forget it, so the test drives the same
 * entry point a reboot takes: init() trusts a valid header and rebuilds an
 * invalid one. Every such call also appends one LOG_OPEN. */
static void reboot(void)
{
    (void)arkrtos_audit_log_init();
}

/* The module keeps its counters for the life of the process, so a case that
 * wants "this reboot rebuilt the header exactly once" has to subtract what the
 * previous cases already did. Taking the baseline right after vol_reset() also
 * covers the first init of a zeroed volume, which legitimately counts as a
 * rebuild (there is no header there yet). */
static uint32 g_rebuilds0;
static uint32 g_dropped0;

static void case_begin(void)
{
    arkrtos_audit_log_stats_t st;

    arkrtos_audit_log_stats(&st);
    g_rebuilds0 = st.rebuilds;
    g_dropped0 = st.dropped;
}

static uint32 rebuilds_since(void)
{
    arkrtos_audit_log_stats_t st;

    arkrtos_audit_log_stats(&st);
    return st.rebuilds - g_rebuilds0;
}

/* Write a record-shaped slot with a deliberately wrong crc: what a reset in the
 * middle of an append leaves behind (magic landed, tail did not). */
static void plant_torn(uint32 i, uint32 seq)
{
    uint8 *p = slot(i);

    memset(p, 0x00, 32u);
    wr32(p + 0u, ARKRTOS_AUDIT_REC_MAGIC);
    wr32(p + 4u, seq);
    wr32(p + 12u, ARKRTOS_AUDIT_EV_EXEC_EXIT);
    wr32(p + 28u, 0xDEADBEEFu);        /* never the crc of the bytes above */
    if(((i * 32u) + 64u) > g_vol_len)
    {
        g_vol_len = (i * 32u) + 64u;
    }
}

/* ---------------- 用例 ---------------- */

static void case_no_volume(void)
{
    vol_reset();
    g_mounted = 0;
    case_begin();

    ck_u((unsigned long)arkrtos_audit_log_init(),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_NOMOUNT,
         "no volume: init reports NOMOUNT");
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_EXEC_START, 1u, 1u),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_NOMOUNT,
         "no volume: record reports NOMOUNT");

    {
        arkrtos_audit_log_stats_t st;

        arkrtos_audit_log_stats(&st);
        ck_u(st.ready, 0ul, "no volume: not ready");
        ck_u(st.used, 0ul, "no volume: nothing readable");
        ck(st.dropped - g_dropped0 >= 1u,
           "no volume: the refusal is counted, not silent");
    }

    ck_u((unsigned long)g_vol_len, 0ul, "no volume: nothing was written");
}

static void case_create_and_append(void)
{
    arkrtos_audit_log_stats_t st;
    arkrtos_audit_rec_t rec;

    vol_reset();
    case_begin();

    ck_u((unsigned long)arkrtos_audit_log_init(), 0ul, "empty volume: init OK");
    /* Header plus the one LOG_OPEN that init appends: the file grows as records
     * are added, it is not preallocated to the full ring. */
    ck_u((unsigned long)g_vol_len, 64ul, "init writes header + one LOG_OPEN");

    arkrtos_audit_log_stats(&st);
    ck_u(st.used, 1ul, "init records exactly one LOG_OPEN");
    ck_u((unsigned long)arkrtos_audit_log_read(0u, &rec), 0ul, "read LOG_OPEN");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_LOG_OPEN, "LOG_OPEN kind");
    ck_u(rec.seq, 1ul, "LOG_OPEN is seq 1");
    ck_u(rec.code, 0ul, "LOG_OPEN code = records recovered (0 here)");

    g_tick = 1234u;
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_AUTH_FAIL, 7u, 2u),
         0ul, "append AUTH_FAIL");
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_EXEC_START, 9u, 4u),
         0ul, "append EXEC_START");

    arkrtos_audit_log_stats(&st);
    ck_u(st.used, 3ul, "three records readable");
    ck_u(st.first_seq, 1ul, "oldest seq is 1");
    ck_u(st.last_seq, 3ul, "newest seq is 3");
    ck_u(st.wrapped, 0ul, "not wrapped yet");

    ck_u((unsigned long)arkrtos_audit_log_read(1u, &rec), 0ul, "read index 1");
    ck_u(rec.seq, 2ul, "index 1 is the second record");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_AUTH_FAIL, "index 1 kind");
    ck_u(rec.owner, 7ul, "index 1 owner");
    ck_u(rec.code, 2ul, "index 1 code");
    ck_u(rec.tick, 1234ul, "index 1 tick");

    ck_u((unsigned long)arkrtos_audit_log_read(2u, &rec), 0ul, "read index 2");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_EXEC_START, "index 2 kind");
    ck_u(rec.code, 4ul, "EXEC_START code carries the task id");

    ck_u((unsigned long)arkrtos_audit_log_read(3u, &rec),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_EMPTY, "read past newest = EMPTY");
    ck_u((unsigned long)arkrtos_audit_log_read(0u, 0),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_PARAM, "read with no out pointer = PARAM");
    ck_u((unsigned long)arkrtos_audit_log_record(0u, 0u, 0u),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_PARAM, "kind 0 = PARAM");
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_LAST + 1u, 0u, 0u),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_PARAM, "kind past LAST = PARAM");

    /* The header pointer must agree with the slots: seq_next is one past the
     * newest record, which is what a fresh open trusts without scanning. */
    ck_u(rd32(&g_vol[20]), 4ul, "header seq_next = 4");
    ck_u(rd32(&g_vol[12]), 3ul, "header head = 3");
    ck_u(rd32(&g_vol[16]), 0ul, "header wrapped = 0");

    /* A clean reboot adds exactly one LOG_OPEN and nothing else. */
    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(rebuilds_since(), 0ul, "clean reboot does not rebuild");
    ck_u(st.used, 4ul, "clean reboot appends exactly one LOG_OPEN");
    ck_u((unsigned long)arkrtos_audit_log_read(3u, &rec), 0ul, "read the new LOG_OPEN");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_LOG_OPEN, "newest is LOG_OPEN");
    ck_u(rec.code, 3ul, "LOG_OPEN code = records recovered by the open");
}

static void case_wrap(void)
{
    arkrtos_audit_log_stats_t st;
    arkrtos_audit_rec_t rec;
    uint32 i;
    uint32 first;

    vol_reset();
    case_begin();
    (void)arkrtos_audit_log_init();

    for(i = 0u; i < VOL_RECORDS; i++)
    {
        g_tick = 100u + i;
        ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_AUTH_LOCK, 1u, i),
             0ul, "append while filling the ring");
    }

    arkrtos_audit_log_stats(&st);
    ck_u(st.used, (unsigned long)VOL_RECORDS, "ring holds exactly cap records");
    ck_u(st.wrapped, 1ul, "ring reports wrapped");
    ck_u(st.first_seq, 2ul, "the LOG_OPEN was overwritten by the oldest kept record");
    ck_u(st.last_seq, (unsigned long)(VOL_RECORDS + 1u), "newest seq");

    first = st.first_seq;
    ck_u((unsigned long)arkrtos_audit_log_read(0u, &rec), 0ul, "read oldest kept");
    ck_u(rec.seq, (unsigned long)first, "oldest kept seq matches stats");
    ck_u(rec.code, 0ul, "oldest kept carries the first AUTH_LOCK code");

    ck_u((unsigned long)arkrtos_audit_log_read(VOL_RECORDS - 1u, &rec), 0ul,
         "read newest");
    ck_u(rec.seq, (unsigned long)(VOL_RECORDS + 1u), "newest is the last append");
    ck_u((unsigned long)arkrtos_audit_log_read(VOL_RECORDS, &rec),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_EMPTY, "one past cap = EMPTY");

    /* A reboot with a valid header must not rescan and must keep the ordering
     * relation intact while the ring stays full. */
    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(rebuilds_since(), 0ul, "clean reboot does not rebuild");
    ck_u(st.used, (unsigned long)VOL_RECORDS, "clean reboot keeps the ring full");
    ck_u(st.last_seq, (unsigned long)(VOL_RECORDS + 2u),
         "clean reboot adds one LOG_OPEN");
    ck_u(st.first_seq, (unsigned long)(st.last_seq - VOL_RECORDS + 1u),
         "first_seq is last_seq - cap + 1 when wrapped");
    ck_u((unsigned long)arkrtos_audit_log_read(VOL_RECORDS - 1u, &rec), 0ul,
         "read newest after reboot");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_LOG_OPEN, "newest after reboot");
}

static void case_torn_outside_window(void)
{
    arkrtos_audit_log_stats_t st;
    arkrtos_audit_rec_t rec;

    vol_reset();
    case_begin();
    (void)arkrtos_audit_log_init();          /* LOG_OPEN at slot 0, head 1 */

    /* The record that was being written when the reset came: it sits at head.
     * Because the header is only written after the record, the header still
     * says slot 1 is the next slot - so the torn bytes are outside the readable
     * window, which is the whole reason the write order is this way. */
    plant_torn(1u, 2u);

    reboot();
    arkrtos_audit_log_stats(&st);
    /* The open itself lands on that slot, and that is the point: the torn bytes
     * are replaced by a record whose crc holds, instead of sitting in the ring
     * pretending to be an event. */
    ck_u(st.used, 2ul, "the torn slot is overwritten by the open's own record");
    ck_u(st.last_seq, 2ul, "the seq continues without a gap");
    ck_u(rebuilds_since(), 0ul, "a valid header means no scan was needed");
    ck_u(st.bad_crc, 0ul,
         "no scan ran, so nothing was counted as torn (it is outside the window)");
    ck_u((unsigned long)arkrtos_audit_log_read(1u, &rec), 0ul, "read the reused slot");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_LOG_OPEN,
         "the slot holds the open's record, not the torn one");
    ck_u(rec.code, 1ul, "the open reports the one record it recovered");

    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_AUTH_FAIL, 1u, 1u),
         0ul, "append after a torn tail");
    arkrtos_audit_log_stats(&st);
    ck_u(st.used, 3ul, "the ring continues from the last whole record");
    ck_u(st.last_seq, 3ul, "seq keeps advancing one at a time");
}

static void case_scan_counts_bad_crc(void)
{
    arkrtos_audit_log_stats_t st;
    arkrtos_audit_rec_t rec;

    vol_reset();
    case_begin();
    (void)arkrtos_audit_log_init();          /* LOG_OPEN at slot 0, head 1 */
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_IMAGE_OK, 5u, 0u),
         0ul, "append IMAGE_OK");

    /* Damage slot 1 (a real record) and the header crc at once: that is the
     * only state in which the scan runs, and it must count the slot as torn
     * instead of reading its fields. */
    slot(1u)[8] ^= 0xFFu;
    g_vol[28] ^= 0x5Au;

    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(rebuilds_since(), 1ul, "a broken header forces one rebuild");
    ck(st.bad_crc >= 1u, "the scan counts the torn slot instead of reading it");
    ck_u(st.used, 2ul, "rebuild keeps the records that still hold (LOG_OPEN + new open)");
    ck_u((unsigned long)arkrtos_audit_log_read(0u, &rec), 0ul, "read oldest after rebuild");
    ck_u(rec.seq, 1ul, "the oldest record is still the first LOG_OPEN");

    /* The rebuilt header must be on the volume: the next open trusts it. */
    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(rebuilds_since(), 1ul, "the rebuilt header was written back (no second rebuild)");
}

static void case_header_rebuild(void)
{
    arkrtos_audit_log_stats_t st;
    arkrtos_audit_rec_t rec;

    vol_reset();
    case_begin();
    (void)arkrtos_audit_log_init();
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_IMAGE_OK, 5u, 0u),
         0ul, "append IMAGE_OK");
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_IMAGE_REFUSE, 5u,
                                                 (unsigned long)-7),
         0ul, "append IMAGE_REFUSE");

    /* A reset between the record write and the header write leaves a header one
     * record short. Rebuild must take the newest seq from the slots. */
    wr32(&g_vol[12], 1u);                 /* head = 1 */
    wr32(&g_vol[20], 2u);                 /* seq_next = 2 */
    g_vol[28] ^= 0x5Au;                   /* and the header crc no longer matches */

    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(rebuilds_since(), 1ul, "a broken header is rebuilt once");
    ck_u(st.used, 4ul, "rebuild recovers three records, then appends LOG_OPEN");
    ck_u(st.last_seq, 4ul, "the newest seq after rebuild is the new LOG_OPEN");

    ck_u((unsigned long)arkrtos_audit_log_read(2u, &rec), 0ul, "read recovered tail");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_IMAGE_REFUSE, "recovered tail kind");
    ck_u(rec.code, (unsigned long)(uint32)-7, "recovered tail carries the loader code");
    ck_u(rec.seq, 3ul, "recovered tail keeps its seq");

    ck_u((unsigned long)arkrtos_audit_log_read(3u, &rec), 0ul, "read the new LOG_OPEN");
    ck_u(rec.kind, (unsigned long)ARKRTOS_AUDIT_EV_LOG_OPEN, "newest is LOG_OPEN");
    ck_u(rec.code, 3ul, "LOG_OPEN code = records recovered (3)");

    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(rebuilds_since(), 1ul, "the rebuilt header was written back (no second rebuild)");
}

static void case_write_failure(void)
{
    arkrtos_audit_log_stats_t st;
    arkrtos_audit_rec_t rec;

    vol_reset();
    case_begin();
    (void)arkrtos_audit_log_init();

    g_fail_write = 1;
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_EXEC_START, 1u, 1u),
         (unsigned long)ARKRTOS_AUDIT_LOG_ERR_WRITE, "a refused write is reported");
    arkrtos_audit_log_stats(&st);
    ck(st.dropped - g_dropped0 >= 1u, "a refused write is counted as dropped");
    ck_u(st.last_seq, 1ul, "a refused write does not consume a seq");

    g_fail_write = 0;
    ck_u((unsigned long)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_EXEC_START, 1u, 8u),
         0ul, "the next write succeeds");

    /* The slot must be the one the failed write was aimed at: if the RAM ring
     * had advanced over it, seq 2 would land in slot 2 and slot 1 would stay
     * blank forever. */
    ck_u((unsigned long)arkrtos_audit_log_read(1u, &rec), 0ul, "read the recovered slot");
    ck_u(rec.seq, 2ul, "seq did not skip");
    ck_u(rec.code, 8ul, "the record that landed is the one that succeeded");
    ck_u(rd32(slot(1u)) , (unsigned long)ARKRTOS_AUDIT_REC_MAGIC,
         "slot 1 holds the record, not slot 2");

    reboot();
    arkrtos_audit_log_stats(&st);
    ck_u(st.used, 3ul, "after a reboot the trail holds the landed records + LOG_OPEN");
    ck_u(st.last_seq, 3ul, "and the seq continues from where it stopped");
}

#endif /* ARKRTOS_USE_AUDIT_LOG */

int main(void)
{
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    /* ABI facts: if the stride ever changes, every committed record on every
     * board becomes unreadable, so they are checked before anything else. */
    ck_u((unsigned long)sizeof(arkrtos_audit_rec_t), 32ul, "record stride is 32");
    ck_u((unsigned long)ARKRTOS_AUDIT_REC_SIZE, 32ul, "REC_SIZE is 32");
    ck_u((unsigned long)ARKRTOS_AUDIT_REC_MAGIC, 0x41554452ul, "record magic is AUDR");

    case_no_volume();
    case_create_and_append();
    case_wrap();
    case_torn_outside_window();
    case_scan_counts_bad_crc();
    case_header_rebuild();
    case_write_failure();

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if(g_fails != 0)
    {
        printf("AUDIT-LOG HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("AUDIT-LOG HOST OK  %d checks\n", g_checks);
#else
    /* Off is a real configuration, not a broken one: the module compiles to a
     * single typedef and exports nothing. The ABI constants still exist because
     * they describe the volume format, which outlives the switch. */
    ck(ARKRTOS_USE_AUDIT_LOG == 0, "OFF: the switch really is 0 in this build");
    ck_u((unsigned long)ARKRTOS_AUDIT_REC_SIZE, 32ul, "OFF: REC_SIZE is still 32");
    ck_u((unsigned long)ARKRTOS_AUDIT_EV_LAST, 9ul, "OFF: kinds are still numbered");
    ck_u((unsigned long)ARKRTOS_AUDIT_LOG_ERR_BUSY, (unsigned long)-6,
         "OFF: status codes are still the published ones");

    printf("checks=%d fails=%d\n", g_checks, g_fails);
    if(g_fails != 0)
    {
        printf("AUDIT-LOG OFF FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("AUDIT-LOG OFF OK  %d checks\n", g_checks);
#endif
    return (g_fails == 0) ? 0 : 1;
}
