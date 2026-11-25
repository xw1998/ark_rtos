/**
* @file arkrtos_fi.c
* @brief Fault injection and tamper watch (SEC-G16, software half)
* @details 能力边界与两半各自「证明什么、不证明什么」写在 arkrtos_fi.h 的
*          文件头里。这里只记实现上的四条约定：
*
*          1. 注入点是**决定**，不是内存。三个 hook 站点各自把「这一次按
*             失败处理」写进原来的错误路径，因此注入走完之后，模块自己的
*             失败计数（alloc_fail / 槽位 / 节拍）必须已经动过 —— 这就是
*             「错误路径被走到」的证据，而不是本模块自己说自己成功。
*          2. 预算用满即自动撤防。测试用例忘了解除也不会把系统留在「每次
*             分配都失败」的状态里；这一点比「方便」重要。
*          3. 注入的记账在临界区语义上很轻（读-减-写），但调用点已经分别
*             在临界区（内存池）或节拍体（中断）里，本模块自己不再嵌套临界
*             区 —— 嵌套在 LA64 上是可重入的，在 ARM 上不是，别赌。
*          4. 篡改监视用真 crc32（与安装期同口径），基线是**登记那一刻**的
*             内容。它发现不了「登记时已经被改过」；那需要签名链。
*
* @note 默认关。关档整文件编成空 TU（nm 查不到已定义全局符号）。
*/

#include "arkrtos_fi.h"
#include "arkrtos_features.h"

#if (ARKRTOS_USE_FAULT_INJECT == 1)

#include <stdint.h>              /* uintptr_t：登记的是物理/虚拟地址 */
#include "arkrtos_app_image.h"   /* arkrtos_crc32：与安装期同口径 */
#include "arkrtos_task.h"        /* arkrtos_kernel_get_tick */
#include "arkrtos_log.h"
#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"   /* kind TAMPER，开档才写 */
#endif

/* ------------------------------------------------------------------
 * 状态
 * ------------------------------------------------------------------ */

/* 三个注入点按位序号索引；kind 不是单个位时 fi_index 返回 -1。 */
#define FI_KIND_COUNT   (3u)

static arkrtos_fi_slot_t s_fi_slot[FI_KIND_COUNT];

/* 篡改监视：used=1 表示这一段登记过。 */
typedef struct
{
    uint32 used;
    uint32 addr;
    uint32 len;
    uint32 baseline;
    uint32 scans;
    uint32 hits;
    uint32 last_crc;
} fi_watch_t;

static fi_watch_t s_fi_watch[ARKRTOS_FI_WATCH_MAX];

/* ------------------------------------------------------------------
 * 注入
 * ------------------------------------------------------------------ */

static int32 fi_index(uint32 kind)
{
    switch(kind)
    {
        case ARKRTOS_FI_KIND_ALLOC: return 0;
        case ARKRTOS_FI_KIND_TASK:  return 1;
        case ARKRTOS_FI_KIND_TICK:  return 2;
        default:                    return -1;
    }
}

int32 arkrtos_fi_arm(uint32 kind, uint32 budget)
{
    int32 idx = fi_index(kind);

    if(idx < 0)
    {
        return ARKRTOS_FI_ERR_PARAM;
    }
    s_fi_slot[idx].armed = (budget == 0u) ? ARKRTOS_FI_DEFAULT_BUDGET : budget;
    return ARKRTOS_FI_OK;
}

int32 arkrtos_fi_disarm(uint32 kind)
{
    uint32 i;

    if(kind == ARKRTOS_FI_KIND_ALL)
    {
        for(i = 0u; i < FI_KIND_COUNT; i++)
        {
            s_fi_slot[i].armed = 0u;
            s_fi_slot[i].fired = 0u;
            s_fi_slot[i].last_tick = 0u;
        }
        return ARKRTOS_FI_OK;
    }

    {
        int32 idx = fi_index(kind);

        if(idx < 0)
        {
            return ARKRTOS_FI_ERR_PARAM;
        }
        s_fi_slot[idx].armed = 0u;
    }
    return ARKRTOS_FI_OK;
}

uint32 arkrtos_fi_armed(uint32 kind)
{
    int32 idx = fi_index(kind);

    return (idx < 0) ? 0u : s_fi_slot[idx].armed;
}

uint32 arkrtos_fi_fired(uint32 kind)
{
    int32 idx = fi_index(kind);

    return (idx < 0) ? 0u : s_fi_slot[idx].fired;
}

int32 arkrtos_fi_stat(uint32 kind, arkrtos_fi_slot_t *out)
{
    int32 idx = fi_index(kind);

    if((idx < 0) || (out == 0))
    {
        return ARKRTOS_FI_ERR_PARAM;
    }
    *out = s_fi_slot[idx];
    return ARKRTOS_FI_OK;
}

uint32 arkrtos_fi_inject(uint32 kind)
{
    int32 idx = fi_index(kind);

    if(idx < 0)
    {
        return 0u;
    }
    if(s_fi_slot[idx].armed == 0u)
    {
        return 0u;
    }

    s_fi_slot[idx].armed--;
    s_fi_slot[idx].fired++;
    s_fi_slot[idx].last_tick = arkrtos_kernel_get_tick();
    return 1u;
}

/* 自检用的私有缓冲：只有这里会写它，所以改一个字节不会碰到任何人的状态。 */
static uint32 s_fi_selftest_buf[4];

uint32 arkrtos_fi_selftest(void)
{
    static const uint32 kinds[FI_KIND_COUNT] =
    {
        ARKRTOS_FI_KIND_ALLOC, ARKRTOS_FI_KIND_TASK, ARKRTOS_FI_KIND_TICK
    };
    uint32 pass = 0u;
    uint32 i;

    for(i = 0u; i < FI_KIND_COUNT; i++)
    {
        uint32 kind = kinds[i];
        uint32 before;
        uint32 ok = 1u;

        before = arkrtos_fi_fired(kind);

        /* 预算 2：前两次注入，第三次必须自动撤防。 */
        if(arkrtos_fi_arm(kind, 2u) != ARKRTOS_FI_OK)                  { ok = 0u; }
        if(ok && (arkrtos_fi_armed(kind) != 2u))                      { ok = 0u; }
        if(ok && (arkrtos_fi_inject(kind) != 1u))                     { ok = 0u; }
        if(ok && (arkrtos_fi_armed(kind) != 1u))                      { ok = 0u; }
        if(ok && (arkrtos_fi_inject(kind) != 1u))                     { ok = 0u; }
        if(ok && (arkrtos_fi_armed(kind) != 0u))                      { ok = 0u; }
        if(ok && (arkrtos_fi_inject(kind) != 0u))                     { ok = 0u; }
        if(ok && (arkrtos_fi_fired(kind) != (before + 2u)))           { ok = 0u; }

        /* 预算传 0 走默认值；撤防后必须回到 0。 */
        if(ok && (arkrtos_fi_arm(kind, 0u) != ARKRTOS_FI_OK))         { ok = 0u; }
        if(ok && (arkrtos_fi_armed(kind) != ARKRTOS_FI_DEFAULT_BUDGET)) { ok = 0u; }
        if(ok && (arkrtos_fi_disarm(kind) != ARKRTOS_FI_OK))          { ok = 0u; }
        if(ok && (arkrtos_fi_armed(kind) != 0u))                      { ok = 0u; }

        if(ok)
        {
            pass |= kind;
        }
    }

    /* 自检不能留下武装状态：它自己的失败就是这个模块最该避免的事。
     * 逐点撤防（不是 ALL）：ALL 会清 fired，而"注入确实发生过"正是这次
     * 自检要留下的证据。 */
    for(i = 0u; i < FI_KIND_COUNT; i++)
    {
        (void)arkrtos_fi_disarm(kinds[i]);
    }

    /* 篡改监视那一条：登记 → 干净 → 改一字节 → 检出 → 还原 → 再次干净。 */
    {
        uint32 ok = 1u;

        s_fi_selftest_buf[0] = 0x11111111u;
        s_fi_selftest_buf[1] = 0x22222222u;
        s_fi_selftest_buf[2] = 0x33333333u;
        s_fi_selftest_buf[3] = 0x44444444u;

        if(arkrtos_fi_watch(0u, (uint32)(uintptr_t)s_fi_selftest_buf,
                            sizeof(s_fi_selftest_buf)) != ARKRTOS_FI_OK)  { ok = 0u; }
        if(ok && (arkrtos_fi_scan(0u) != 0))                              { ok = 0u; }
        s_fi_selftest_buf[2] ^= 0x00000001u;
        if(ok && (arkrtos_fi_scan(0u) != 1))                              { ok = 0u; }
        s_fi_selftest_buf[2] ^= 0x00000001u;
        if(ok && (arkrtos_fi_scan(0u) != 0))                              { ok = 0u; }

        /* 摘掉临时监视段，别让 `fi list` 里多出一条来路不明的记录。 */
        s_fi_watch[0].used = 0u;
        s_fi_watch[0].addr = 0u;
        s_fi_watch[0].len = 0u;
        s_fi_watch[0].baseline = 0u;
        s_fi_watch[0].scans = 0u;
        s_fi_watch[0].hits = 0u;
        s_fi_watch[0].last_crc = 0u;

        if(ok)
        {
            pass |= ARKRTOS_FI_SELFTEST_WATCH;
        }
    }

    return pass;
}

/* ------------------------------------------------------------------
 * 篡改监视
 * ------------------------------------------------------------------ */

int32 arkrtos_fi_watch(uint32 index, uint32 addr, uint32 len)
{
    if((index >= ARKRTOS_FI_WATCH_MAX) || (addr == 0u) || (len == 0u))
    {
        return ARKRTOS_FI_ERR_PARAM;
    }
    if((addr + len) < addr)   /* 回绕：一段地址不可能跨过 4 GB 顶端 */
    {
        return ARKRTOS_FI_ERR_PARAM;
    }

    s_fi_watch[index].used = 1u;
    s_fi_watch[index].addr = addr;
    s_fi_watch[index].len = len;
    s_fi_watch[index].baseline = arkrtos_crc32((const void *)(uintptr_t)addr, len, 0u);
    s_fi_watch[index].scans = 0u;
    s_fi_watch[index].hits = 0u;
    s_fi_watch[index].last_crc = s_fi_watch[index].baseline;
    return ARKRTOS_FI_OK;
}

int32 arkrtos_fi_scan(uint32 index)
{
    uint32 crc;

    if((index >= ARKRTOS_FI_WATCH_MAX) || (s_fi_watch[index].used == 0u))
    {
        return ARKRTOS_FI_ERR_PARAM;
    }

    crc = arkrtos_crc32((const void *)(uintptr_t)s_fi_watch[index].addr,
                        s_fi_watch[index].len, 0u);
    s_fi_watch[index].scans++;
    s_fi_watch[index].last_crc = crc;

    if(crc == s_fi_watch[index].baseline)
    {
        return 0;
    }

    s_fi_watch[index].hits++;
    ARKRTOS_LOGE("FI", "tamper on watch %u (crc 0x%08X, baseline 0x%08X)",
                 (unsigned)index, (unsigned)crc,
                 (unsigned)s_fi_watch[index].baseline);
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    (void)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_TAMPER, 0u, index);
#endif
    return 1;
}

int32 arkrtos_fi_scan_all(void)
{
    uint32 i;
    int32 bad = 0;

    for(i = 0u; i < ARKRTOS_FI_WATCH_MAX; i++)
    {
        if(s_fi_watch[i].used == 0u)
        {
            continue;
        }
        if(arkrtos_fi_scan(i) == 1)
        {
            bad++;
        }
    }
    return bad;
}

int32 arkrtos_fi_watch_stat(uint32 index, arkrtos_fi_watch_t *out)
{
    if((index >= ARKRTOS_FI_WATCH_MAX) || (out == 0) || (s_fi_watch[index].used == 0u))
    {
        return ARKRTOS_FI_ERR_PARAM;
    }
    out->addr = s_fi_watch[index].addr;
    out->len = s_fi_watch[index].len;
    out->baseline = s_fi_watch[index].baseline;
    out->scans = s_fi_watch[index].scans;
    out->hits = s_fi_watch[index].hits;
    out->last_crc = s_fi_watch[index].last_crc;
    return ARKRTOS_FI_OK;
}

void arkrtos_fi_module_init(void)
{
    uint32 i;

    for(i = 0u; i < FI_KIND_COUNT; i++)
    {
        s_fi_slot[i].armed = 0u;
        s_fi_slot[i].fired = 0u;
        s_fi_slot[i].last_tick = 0u;
    }
    for(i = 0u; i < ARKRTOS_FI_WATCH_MAX; i++)
    {
        s_fi_watch[i].used = 0u;
        s_fi_watch[i].addr = 0u;
        s_fi_watch[i].len = 0u;
        s_fi_watch[i].baseline = 0u;
        s_fi_watch[i].scans = 0u;
        s_fi_watch[i].hits = 0u;
        s_fi_watch[i].last_crc = 0u;
    }
}

#endif /* ARKRTOS_USE_FAULT_INJECT */
