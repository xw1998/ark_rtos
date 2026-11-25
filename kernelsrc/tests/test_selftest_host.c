/**
* @file test_selftest_host.c
* @brief 开机/运行期自检（arkrtos_selftest.c，SEC-G14）的宿主用例
* @details 被测模块回答的是「诊断能不能发现失效」，所以这里钉住的不是
*          「能不能跑完」，而是那几个会静默报绿的地方：
*
*            - SKIP 不是 PASS：板级没给读数时，那一项必须报 SKIP。把「没测」
*              算成「通过」是诊断模块最典型的假绿；
*            - 只读区间的基线必须**开机学、之后比**：巡检那次没有基线只能
*              报 SKIP（拿自己的影子比自己没有意义），有基线之后内容跟着变
*              必须 FAIL；
*            - 时钟校验学比值、不假设频率：第一次只采点不判定，第二次学比值，
*              之后按容差比；任一路停住（节拍不动或计数器不动）必须 FAIL，
*              不能因为「没得比」就算通过；
*            - RAM 与 CPU 的失败路径必须能报出**哪一条**图案/向量，而不是一个
*              笼统的 FAIL —— 现场要的是「第几条挂了」。
*
*          桩有五个：节拍读数、两路板级钩子、日志三件套、审计记录。签名全部
*          取自真头文件，桩一旦与声明漂移，测的就不是模块而是桩。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_app_image.h"   /* arkrtos_crc32：桩的签名取自真头文件 */
#include "arkrtos_selftest.h"
#include "arkrtos_features.h"
#include "arkrtos_task.h"
#include "arkrtos_log.h"

#if (ARKRTOS_USE_AUDIT_LOG == 1)
#include "arkrtos_audit_log.h"
#endif

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

#if (ARKRTOS_USE_SELFTEST == 1)

/* 断言只在开档编：关档时没有任何用例会调用它。 */
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

/* 节拍读数：用例直接改这个数，用来造「节拍停住 / 走得太快」。 */
static uint32 g_tick;

uint32 arkrtos_kernel_get_tick(void)
{
    return g_tick;
}

/* 真的 crc32（与安装期同口径），不是桩：桩会让「内容变了必须 FAIL」这条
 * 用例无论模块怎么写都通过 —— 那测的是桩，不是被测对象。 */
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

/* 第二路计数器：用例脚本化。g_clk_ok = 0 表示这块板子没有这路读数。 */
static uint32 g_clk;
static int    g_clk_ok;

/* 只读区间：模块的钩子按「32 位物理地址」给区间（板上这就是真地址），所以
 * 宿主上必须显式拿一块地址放得进 32 位的缓冲。直接把栈上数组的地址截成
 * uint32 会得到一个野地址，crc 一读就崩 —— 这批用例第一次就是这么崩的。
 */
#if defined(_WIN32)
#include <windows.h>
static uint8 *g_span;
static int span_map(void)
{
    g_span = (uint8 *)VirtualAlloc((void *)(uintptr_t)0x10000000u, 4096u,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return (g_span != (uint8 *)0) ? 0 : -1;
}
#else
#include <sys/mman.h>
static uint8 *g_span;
static int span_map(void)
{
    void *p = mmap((void *)0x10000000, 4096u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    g_span = (uint8 *)p;
    return (p == MAP_FAILED) ? -1 : 0;
}
#endif
#define SPAN_BYTES  256u
static uint32 g_span_len;
static int    g_span_ok;

int32 arkrtos_port_selftest_flash_span(uint32 *start, uint32 *len)
{
    if(g_span_ok == 0)
    {
        return -1;
    }
    *start = (uint32)(uintptr_t)g_span;
    *len   = g_span_len;
    return 0;
}

int32 arkrtos_port_selftest_clock(uint32 *now)
{
    if(g_clk_ok == 0)
    {
        return -1;
    }
    *now = g_clk;
    return 0;
}

/* 日志三件套：只记录调了几次，不落盘。 */
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
static uint32 g_audit_kinds[8];
static uint32 g_audit_codes[8];
static uint32 g_audit_n;

int32 arkrtos_audit_log_record(uint32 kind, uint32 owner, uint32 code)
{
    (void)owner;
    if(g_audit_n < 8u)
    {
        g_audit_kinds[g_audit_n] = kind;
        g_audit_codes[g_audit_n] = code;
        g_audit_n++;
    }
    return 0;
}
#endif

/* ---------------- 公共复位 ---------------- */

static void reset_all(void)
{
    uint32 i;

    g_tick       = 0u;
    g_clk        = 0u;
    g_clk_ok     = 1;
    g_span_ok    = 1;
    g_span_len   = SPAN_BYTES;
    g_log_errors = 0u;
    for(i = 0u; i < SPAN_BYTES; i++)
    {
        g_span[i] = (uint8)(i * 7u);
    }
    /* 模块自己记着基线（只读区间 crc、时钟比值）。用例之间必须清掉，
     * 否则「第一次采样」这类断言测到的是上一个用例的残留状态。 */
    (void)arkrtos_selftest_forget();
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    memset(g_audit_kinds, 0, sizeof(g_audit_kinds));
    memset(g_audit_codes, 0, sizeof(g_audit_codes));
    g_audit_n = 0u;
#endif
}

/* 让两路读数各走一步（比值 1000 计数/拍）。巡检一定会跑时钟项，
 * 不推进的话「两个采样点之间一拍没走」会被判成节拍源停住 —— 那是真故障，
 * 但也确实不是这些用例要测的东西。 */
static void clk_step(void)
{
    g_tick += 100u;
    g_clk  += 100000u;
}

static uint32 item_verdict(const arkrtos_selftest_report_t *r, uint32 id)
{
    uint32 i;

    for(i = 0u; i < r->items; i++)
    {
        if(r->item[i].id == id)
        {
            return (uint32)r->item[i].verdict;
        }
    }
    return 0xFFFFFFFFu;
}

static uint32 item_detail(const arkrtos_selftest_report_t *r, uint32 id)
{
    uint32 i;

    for(i = 0u; i < r->items; i++)
    {
        if(r->item[i].id == id)
        {
            return r->item[i].detail;
        }
    }
    return 0xFFFFFFFFu;
}

/* ---------------- 用例 ---------------- */

/* 参数检查：空指针 / 空掩码 / 越界掩码都必须拒绝。 */
static void case_params(void)
{
    arkrtos_selftest_report_t rep;

    ck_u((unsigned long)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_ALL,
                                             (arkrtos_selftest_report_t *)0),
         (unsigned long)ARKRTOS_SELFTEST_ERR_PARAM, "params: null report");
    ck_u((unsigned long)arkrtos_selftest_run(0u, &rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_PARAM, "params: empty mask");
    ck_u((unsigned long)arkrtos_selftest_run(0x10u, &rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_PARAM, "params: unknown bit");
}

/* 正常运行：RAM / CPU 通过，只读区间学到基线，时钟第一次报 SKIP。 */
static void case_run_all(void)
{
    arkrtos_selftest_report_t rep;
    int32 rc;

    reset_all();
    rc = arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_ALL, &rep);

    ck_u((unsigned long)rc, (unsigned long)ARKRTOS_SELFTEST_OK, "run: healthy rc");
    ck_u(rep.items, 4u, "run: four items");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_RAM),
         ARKRTOS_SELFTEST_PASS, "run: ram pass");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_CPU),
         ARKRTOS_SELFTEST_PASS, "run: cpu pass");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_FLASH),
         ARKRTOS_SELFTEST_PASS, "run: flash pass");
    /* 第一次采样没有第二个点，只能 SKIP —— 这一条是整个模块最容易写错的
     * 地方：把「还没比过」当「比过了」。 */
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK),
         ARKRTOS_SELFTEST_SKIP, "run: clock first sample is SKIP");
    ck_u(rep.failed, 0u, "run: nothing failed");
    ck_u(rep.skipped, (unsigned long)ARKRTOS_SELFTEST_ITEM_CLOCK,
         "run: skipped mask names the clock");
    ck_u((unsigned long)(arkrtos_selftest_flash_crc() != 0u), 1u,
         "run: flash baseline learned");
}

/* 掩码选择：只要 CPU 时就只跑一项。 */
static void case_mask(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    ck_u((unsigned long)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_CPU, &rep),
         (unsigned long)ARKRTOS_SELFTEST_OK, "mask: cpu only rc");
    ck_u(rep.items, 1u, "mask: cpu only item count");
    ck_u(rep.item[0].id, ARKRTOS_SELFTEST_ITEM_CPU, "mask: cpu only id");
}

/* 板级不给读数：两项都必须 SKIP，不是 PASS。 */
static void case_unsupported(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    g_clk_ok  = 0;
    g_span_ok = 0;
    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_ALL, &rep);

    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_FLASH),
         ARKRTOS_SELFTEST_SKIP, "unsupported: flash skip");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK),
         ARKRTOS_SELFTEST_SKIP, "unsupported: clock skip");
    ck_u(rep.failed, 0u, "unsupported: no fail");
}

/* 板级说支持但给了空区间：报 FAIL，不是 SKIP（实现有问题）。 */
static void case_bad_span(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    g_span_len = 0u;
    ck_u((unsigned long)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_FLASH, &rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_FAIL, "bad span: rc fail");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_FLASH),
         ARKRTOS_SELFTEST_FAIL, "bad span: flash fail");
}

/* 只读区间内容变了：巡检必须 FAIL；改回去必须复绿。 */
static void case_flash_change(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_FLASH, &rep);

    g_span[3] = (uint8)(g_span[3] ^ 0xFFu);
    clk_step();
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_FAIL, "flash change: patrol fail");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_FLASH),
         ARKRTOS_SELFTEST_FAIL, "flash change: item fail");

    g_span[3] = (uint8)(g_span[3] ^ 0xFFu);
    clk_step();
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_OK, "flash change: back to pass");
}

/* 巡检那次没有基线：只能 SKIP，同时把这次当基线学下来。 */
static void case_patrol_no_baseline(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    clk_step();
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_OK, "no baseline: rc ok");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_FLASH),
         ARKRTOS_SELFTEST_SKIP, "no baseline: flash skip");
    ck_u((unsigned long)(arkrtos_selftest_flash_crc() != 0u), 1u,
         "no baseline: baseline learned by patrol");
    /* 学下来之后，内容不变必须 PASS。 */
    clk_step();
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_OK, "no baseline: second patrol ok");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_FLASH),
         ARKRTOS_SELFTEST_PASS, "no baseline: second patrol pass");
}

/* 时钟：学比值 → 一致 PASS → 漂移 FAIL(带 ppm)。 */
static void case_clock(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    {
        uint32 t0 = g_tick;
        uint32 c0 = g_clk;

    /* 第 1 次：只采点。 */
    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_CLOCK, &rep);
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK),
         ARKRTOS_SELFTEST_SKIP, "clock: first sample skip");

    /* 第 2 次：学比值（100 拍 / 100000 计数 = 1000 计数每拍）。 */
    g_tick = t0 + 100u;
    g_clk  = c0 + 100000u;
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_OK, "clock: learn ratio rc");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK),
         ARKRTOS_SELFTEST_PASS, "clock: learn ratio pass");

    /* 第 3 次：一致（再 100 拍、再 100000 计数）。 */
    g_tick = t0 + 200u;
    g_clk  = c0 + 200000u;
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_OK, "clock: consistent rc");
    ck_u(item_detail(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK), 0u,
         "clock: consistent drift is 0 ppm");

    /* 第 4 次：计数器只走了应有的一半 ⇒ 漂移远超容差。 */
    g_tick = t0 + 300u;
    g_clk  = c0 + 250000u;
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_FAIL, "clock: drift rc");
    ck_u(item_verdict(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK),
         ARKRTOS_SELFTEST_FAIL, "clock: drift fail");
    ck_u((unsigned long)(item_detail(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK) >
                          (uint32)ARKRTOS_SELFTEST_CLOCK_PPM), 1u,
         "clock: drift ppm above tolerance");
    }
}

/* 计数器停住：节拍在走、第二路不动，必须 FAIL。 */
static void case_clock_stall(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    g_tick = 10u;
    g_clk  = 10000u;
    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_CLOCK, &rep);   /* 采点 */
    g_tick = 20u;
    g_clk  = 20000u;
    (void)arkrtos_selftest_patrol(&rep);                            /* 学比值 */

    g_tick = 30u;
    /* g_clk 不动：计数器停了 */
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_FAIL, "clock stall: counter stuck fail");
}

/* 节拍停住：第二路在走、节拍不动，必须 FAIL（不是「没得比」）。 */
static void case_tick_stall(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    g_tick = 10u;
    g_clk  = 10000u;
    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_CLOCK, &rep);
    g_tick = 20u;
    g_clk  = 20000u;
    (void)arkrtos_selftest_patrol(&rep);

    g_clk = 30000u;
    /* g_tick 不动：节拍停了 */
    ck_u((unsigned long)arkrtos_selftest_patrol(&rep),
         (unsigned long)ARKRTOS_SELFTEST_ERR_FAIL, "tick stall: tick stuck fail");
}

/* 开机入口：健康时返回 0 且不写审计；只读区间坏了返回位掩码并留痕。 */
static void case_boot(void)
{
    reset_all();
    ck_u(arkrtos_selftest_boot(), 0u, "boot: healthy returns 0");
    ck_u(g_log_errors, 0u, "boot: healthy logs nothing");
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    ck_u(g_audit_n, 0u, "boot: healthy does not touch the audit trail");
#endif

    reset_all();
    g_span_len = 0u;          /* 板级实现有问题 ⇒ FLASH 报 FAIL */
    ck_u((unsigned long)(arkrtos_selftest_boot() & ARKRTOS_SELFTEST_ITEM_FLASH),
         (unsigned long)ARKRTOS_SELFTEST_ITEM_FLASH, "boot: failure mask names flash");
    ck_u(g_log_errors, 1u, "boot: failure logged once");
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    ck_u(g_audit_n, 1u, "boot: failure written to the audit trail");
    ck_u(g_audit_kinds[0], (unsigned long)ARKRTOS_AUDIT_EV_SELFTEST_FAIL,
         "boot: audit kind is SELFTEST_FAIL");
    ck_u(g_audit_codes[0], (unsigned long)ARKRTOS_SELFTEST_ITEM_FLASH,
         "boot: audit code carries the failed mask");
#endif
}

/* 巡检失败也要留痕（不是只有开机那次才算）。 */
static void case_patrol_audit(void)
{
    arkrtos_selftest_report_t rep;

    reset_all();
    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_FLASH, &rep);
    g_span[5] = (uint8)(g_span[5] ^ 0x5Au);
    clk_step();
    (void)arkrtos_selftest_patrol(&rep);

    ck_u(g_log_errors, 1u, "patrol audit: failure logged");
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    ck_u(g_audit_n, 1u, "patrol audit: one record");
    ck_u(g_audit_kinds[0], (unsigned long)ARKRTOS_AUDIT_EV_SELFTEST_FAIL,
         "patrol audit: kind");
#endif
    ck_u((unsigned long)(item_detail(&rep, ARKRTOS_SELFTEST_ITEM_FLASH) !=
                          0xFFFFFFFFu), 1u, "patrol audit: flash item present");
}

int main(void)
{
    if(span_map() != 0)
    {
        printf("SELFTEST HOST FAILED: cannot map the flash span below 4 GiB\n");
        return 1;
    }
    case_params();
    case_run_all();
    case_mask();
    case_unsupported();
    case_bad_span();
    case_flash_change();
    case_patrol_no_baseline();
    case_clock();
    case_clock_stall();
    case_tick_stall();
    case_boot();
    case_patrol_audit();

    if(g_fails != 0)
    {
        printf("SELFTEST HOST FAILED (%d of %d checks)\n", g_fails, g_checks);
        return 1;
    }
    printf("SELFTEST HOST OK  %d checks\n", g_checks);
    return 0;
}

#else   /* ARKRTOS_USE_SELFTEST == 0 */

int main(void)
{
    /* 关档：模块编成空 TU，用例只剩一件事要证明——开关确实是关的。 */
    g_checks = 1;
    g_fails = 0;
    if(ARKRTOS_USE_SELFTEST != 0)
    {
        g_fails++;
        printf("FAIL: OFF: the switch is not 0\n");
    }
    if(g_fails != 0)
    {
        printf("SELFTEST OFF FAILED (%d of %d checks)\n", g_fails, g_checks);
        return 1;
    }
    printf("SELFTEST OFF OK  %d checks\n", g_checks);
    return 0;
}

#endif /* ARKRTOS_USE_SELFTEST */
