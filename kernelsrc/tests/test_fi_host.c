/**
* @file test_fi_host.c
* @brief 故障注入与篡改监视（arkrtos_fi.c，SEC-G16 软件那一半）的宿主用例
* @details 这个模块最怕的是「注入根本没发生，但自检报绿」和「预算用完之后
*          还在注入」。所以钉住的不是 happy path，而是那几处会静默骗人的地方：
*
*            - 预算语义：武装 n 次就恰好注入 n 次，第 n+1 次必须返回 0，
*              并且 armed 必须已经自己回到 0（忘记撤防不能把系统留在坏状态）；
*            - 注入点互不串扰：武装 alloc 不许让 tick 也返回 1；
*            - 参数边界：非单个位的 kind、越界 index、零地址、零长度、
*              地址回绕都必须拒绝，且**不动**已有状态；
*            - 篡改监视：基线是登记那一刻算的；改一个字节必须检出、hits 加一；
*              改回去必须重新判 0（基线不跟着内容漂移）；检出时必须留下
*              日志与持久审计（审计开档那一组验）；
*            - 自检不留下武装状态：`fi test` 跑完 armed 全 0。
*
*          桩四个：节拍读数、日志两件套、审计记录。crc32 用真实现 —— 桩会让
*          「内容变了必须检出」这条用例无论模块怎么写都通过，那测的是桩。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_fi.h"
#include "arkrtos_features.h"
#include "arkrtos_task.h"
#include "arkrtos_log.h"

#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"
#endif

static int g_checks;
static int g_fails;

#if (ARKRTOS_USE_FAULT_INJECT == 1)

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %lu want %lu)\n", what, got, want);
    }
}

/* ---------------- 桩 ---------------- */

static uint32 g_tick;

uint32 arkrtos_kernel_get_tick(void)
{
    return g_tick;
}

/* 真的 crc32（多项式与安装期同口径），不是桩。
 * 显式声明一次：宿主 TU 只调它，不链安装期模块，而 -Wmissing-prototypes
 * 要求「对外可见的定义」必须有一份前置声明。 */
uint32 arkrtos_crc32(const void *data, uint32 len, uint32 crc);

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

static uint32 g_log_errors;

uint32 arkrtos_log_get_level(void)
{
    return (uint32)ARKRTOS_LOG_ERROR;
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)tag;
    (void)line;
    (void)msg;
    if(level == (uint32)ARKRTOS_LOG_ERROR)
    {
        g_log_errors++;
    }
}

#if (ARKRTOS_USE_AUDIT_LOG == 1)
static uint32 g_audit_n;
static uint32 g_audit_kind[8];
static uint32 g_audit_code[8];

int32 arkrtos_audit_log_record(uint32 kind, uint32 owner, uint32 code)
{
    (void)owner;
    if(g_audit_n < 8u)
    {
        g_audit_kind[g_audit_n] = kind;
        g_audit_code[g_audit_n] = code;
        g_audit_n++;
    }
    return 0;
}
#endif

/* ---------------- 用例 ---------------- */

static void case_budget(void)
{
    arkrtos_fi_slot_t st;

    arkrtos_fi_module_init();

    ck_u((unsigned long)arkrtos_fi_arm(ARKRTOS_FI_KIND_ALLOC, 2u), 0, "arm returns ok");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_ALLOC), 2u, "armed is the budget");

    ck_u(arkrtos_fi_inject(ARKRTOS_FI_KIND_ALLOC), 1u, "first injection fires");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_ALLOC), 1u, "one left");
    ck_u(arkrtos_fi_inject(ARKRTOS_FI_KIND_ALLOC), 1u, "second injection fires");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_ALLOC), 0u, "budget spent, self-disarmed");
    ck_u(arkrtos_fi_inject(ARKRTOS_FI_KIND_ALLOC), 0u, "third injection does not fire");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_ALLOC), 2u, "fired counts exactly the budget");

    ck_u((unsigned long)arkrtos_fi_stat(ARKRTOS_FI_KIND_ALLOC, &st), 0, "stat ok");
    ck_u(st.armed, 0u, "stat armed 0");
    ck_u(st.fired, 2u, "stat fired 2");
    ck_u(st.last_tick, g_tick, "stat last_tick comes from the kernel tick");

    /* 预算 0 用默认值；撤防把 armed 清零但不动 fired。 */
    ck_u((unsigned long)arkrtos_fi_arm(ARKRTOS_FI_KIND_TICK, 0u), 0, "arm default");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_TICK), ARKRTOS_FI_DEFAULT_BUDGET,
         "default budget");
    ck_u((unsigned long)arkrtos_fi_disarm(ARKRTOS_FI_KIND_TICK), 0, "disarm ok");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_TICK), 0u, "disarmed");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_TICK), 0u, "disarm keeps no fake firing");
}

static void case_isolation(void)
{
    arkrtos_fi_module_init();

    (void)arkrtos_fi_arm(ARKRTOS_FI_KIND_ALLOC, 1u);
    ck_u(arkrtos_fi_inject(ARKRTOS_FI_KIND_TASK), 0u, "armed alloc does not fire task");
    ck_u(arkrtos_fi_inject(ARKRTOS_FI_KIND_TICK), 0u, "armed alloc does not fire tick");
    ck_u(arkrtos_fi_inject(ARKRTOS_FI_KIND_ALLOC), 1u, "armed alloc fires alloc");

    (void)arkrtos_fi_arm(ARKRTOS_FI_KIND_TASK, 3u);
    (void)arkrtos_fi_arm(ARKRTOS_FI_KIND_TICK, 4u);
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_ALLOC), 1u, "alloc fired count untouched");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_TASK), 0u, "task fired count untouched");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_TICK), 0u, "tick fired count untouched");

    /* ALL 撤防清计数。 */
    ck_u((unsigned long)arkrtos_fi_disarm(ARKRTOS_FI_KIND_ALL), 0, "disarm all");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_ALLOC), 0u, "disarm all clears fired");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_TASK), 0u, "disarm all clears armed");
}

static void case_param(void)
{
    arkrtos_fi_slot_t st;
    arkrtos_fi_watch_t w;

    arkrtos_fi_module_init();

    ck_u((unsigned long)arkrtos_fi_arm(0u, 1u), (unsigned long)ARKRTOS_FI_ERR_PARAM,
         "kind 0 refused");
    ck_u((unsigned long)arkrtos_fi_arm(ARKRTOS_FI_KIND_ALL, 1u),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "ALL is not a single kind");
    ck_u((unsigned long)arkrtos_fi_disarm(0x80u), (unsigned long)ARKRTOS_FI_ERR_PARAM,
         "disarm unknown kind refused");
    ck_u(arkrtos_fi_inject(0x80u), 0u, "inject unknown kind never fires");
    ck_u(arkrtos_fi_armed(0x80u), 0u, "armed of unknown kind is 0");
    ck_u(arkrtos_fi_fired(0x80u), 0u, "fired of unknown kind is 0");
    ck_u((unsigned long)arkrtos_fi_stat(ARKRTOS_FI_KIND_ALLOC, 0),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "null stat out refused");
    ck_u((unsigned long)arkrtos_fi_stat(0x8u, &st),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "stat of an unknown kind refused");

    ck_u((unsigned long)arkrtos_fi_watch(ARKRTOS_FI_WATCH_MAX, (uint32)(uintptr_t)&st, 4u),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "watch index out of range");
    ck_u((unsigned long)arkrtos_fi_watch(0u, 0u, 4u),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "watch null address");
    ck_u((unsigned long)arkrtos_fi_watch(0u, (uint32)(uintptr_t)&st, 0u),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "watch zero length");
    ck_u((unsigned long)arkrtos_fi_watch(0u, 0xFFFFFFFCu, 8u),
         (unsigned long)ARKRTOS_FI_ERR_PARAM, "watch length wraps");
    ck_u((unsigned long)arkrtos_fi_scan(0u), (unsigned long)ARKRTOS_FI_ERR_PARAM,
         "scan of unregistered span refused");
    ck_u((unsigned long)arkrtos_fi_watch_stat(0u, &w), (unsigned long)ARKRTOS_FI_ERR_PARAM,
         "watch stat of unregistered span refused");
    ck_u((unsigned long)arkrtos_fi_scan_all(), 0, "scan_all with nothing registered");
}

static void case_watch(void)
{
    uint32 buf[4];
    arkrtos_fi_watch_t w;

    arkrtos_fi_module_init();

    buf[0] = 0x11223344u;
    buf[1] = 0x55667788u;
    buf[2] = 0x99AABBCCu;
    buf[3] = 0xDDEEFF00u;

    ck_u((unsigned long)arkrtos_fi_watch(0u, (uint32)(uintptr_t)buf, sizeof(buf)),
         0, "watch registered");
    ck_u((unsigned long)arkrtos_fi_scan(0u), 0, "clean buffer unchanged");
    ck_u((unsigned long)arkrtos_fi_scan(0u), 0, "second clean scan unchanged");

    buf[2] ^= 0x00000001u;
    ck_u((unsigned long)arkrtos_fi_scan(0u), 1, "one flipped bit is detected");
    ck_u((unsigned long)arkrtos_fi_scan(0u), 1, "still detected while it differs");

    buf[2] ^= 0x00000001u;
    ck_u((unsigned long)arkrtos_fi_scan(0u), 0, "restored content matches baseline again");

    ck_u((unsigned long)arkrtos_fi_watch_stat(0u, &w), 0, "watch stat ok");
    ck_u(w.addr, (unsigned long)(uintptr_t)buf, "watch stat addr");
    ck_u(w.len, sizeof(buf), "watch stat len");
    ck_u(w.scans, 5u, "watch stat counts every scan");
    ck_u(w.hits, 2u, "watch stat counts both detections");
    ck_u(w.last_crc, w.baseline, "restored content leaves last_crc at the baseline");

    /* 重新登记即重学基线。 */
    buf[0] = 0u;
    ck_u((unsigned long)arkrtos_fi_scan(0u), 1, "change detected before rebaseline");
    ck_u((unsigned long)arkrtos_fi_watch(0u, (uint32)(uintptr_t)buf, sizeof(buf)),
         0, "re-register rebaselines");
    ck_u((unsigned long)arkrtos_fi_scan(0u), 0, "rebaseline accepts the new content");
    ck_u((unsigned long)arkrtos_fi_watch_stat(0u, &w), 0, "stat after rebaseline");
    ck_u(w.hits, 0u, "rebaseline clears the hit counter");

    /* 两段互不影响：段 0 管 buf[0..1]，段 1 管 buf[2..3]。 */
    (void)arkrtos_fi_watch(0u, (uint32)(uintptr_t)&buf[0], 2u * sizeof(uint32));
    (void)arkrtos_fi_watch(1u, (uint32)(uintptr_t)&buf[2], 2u * sizeof(uint32));
    ck_u((unsigned long)arkrtos_fi_scan_all(), 0, "scan_all sees no change yet");
    buf[3] = 0u;
    ck_u((unsigned long)arkrtos_fi_scan_all(), 1, "scan_all counts the changed span");
    ck_u((unsigned long)arkrtos_fi_scan(0u), 0, "the other span stayed clean");
}

static void case_selftest(void)
{
    uint32 pass;
    arkrtos_fi_watch_t w0;

    arkrtos_fi_module_init();
    pass = arkrtos_fi_selftest();
    ck_u(pass, ARKRTOS_FI_SELFTEST_ALL, "selftest covers the kinds and the watch");
    ck_u((unsigned long)arkrtos_fi_watch_stat(0u, &w0), (unsigned long)ARKRTOS_FI_ERR_PARAM,
         "selftest removes its temporary watch span");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_ALLOC), 0u, "selftest leaves alloc disarmed");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_TASK), 0u, "selftest leaves task disarmed");
    ck_u(arkrtos_fi_armed(ARKRTOS_FI_KIND_TICK), 0u, "selftest leaves tick disarmed");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_ALLOC), 2u, "selftest fired alloc twice");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_TASK), 2u, "selftest fired task twice");
    ck_u(arkrtos_fi_fired(ARKRTOS_FI_KIND_TICK), 2u, "selftest fired tick twice");

    /* 跑第二遍仍然全过：自检不能依赖"第一次才成立"的状态。 */
    ck_u(arkrtos_fi_selftest(), ARKRTOS_FI_SELFTEST_ALL, "selftest is repeatable");
}

#if (ARKRTOS_USE_AUDIT_LOG == 1)
static void case_tamper_audit(void)
{
    uint32 buf[2] = { 1u, 2u };

    arkrtos_fi_module_init();
    g_audit_n = 0u;
    g_log_errors = 0u;
    ck_u((unsigned long)arkrtos_fi_watch(3u, (uint32)(uintptr_t)buf, sizeof(buf)),
         0, "watch for audit");
    ck_u((unsigned long)arkrtos_fi_scan(3u), 0, "baseline clean");
    ck_u(g_audit_n, 0u, "no audit record on a clean scan");

    buf[0] = 0xFFFFFFFFu;
    ck_u((unsigned long)arkrtos_fi_scan(3u), 1, "tamper detected");
    ck_u(g_audit_n, 1u, "one audit record written");
    ck_u(g_audit_kind[0], (unsigned long)ARKRTOS_AUDIT_EV_TAMPER, "record kind is TAMPER");
    ck_u(g_audit_code[0], 3u, "record code carries the watch index");
    ck_u(g_log_errors >= 1u, 1u, "the detection is logged as an error");
}
#endif

int main(void)
{
    g_fails = 0;
    g_checks = 0;

    case_budget();
    case_isolation();
    case_param();
    case_watch();
    case_selftest();
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    case_tamper_audit();
#endif

    if(g_fails != 0)
    {
        printf("FI HOST FAILED (%d of %d checks)\n", g_fails, g_checks);
        return 1;
    }
    printf("FI HOST OK  %d checks\n", g_checks);
    return 0;
}

#else   /* ARKRTOS_USE_FAULT_INJECT == 0 */

int main(void)
{
    g_checks = 1;
    g_fails = 0;
    if(ARKRTOS_USE_FAULT_INJECT != 0)
    {
        g_fails++;
        printf("FAIL: OFF: the switch is not 0\n");
    }
    if(arkrtos_fi_inject(ARKRTOS_FI_KIND_ALLOC) != 0u)
    {
        g_fails++;
        printf("FAIL: OFF: the hook macro is not a constant 0\n");
    }
    if(g_fails != 0)
    {
        printf("FI OFF FAILED (%d of %d checks)\n", g_fails, g_checks);
        return 1;
    }
    printf("FI OFF OK  %d checks\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_FAULT_INJECT */
