/**
* @file arkrtos_selftest.c
* @brief Startup and runtime self-test (SEC-G14)
* @details 四类诊断，各自的覆盖范围写在 arkrtos_selftest.h 的文件头里。这里
*          只记实现上的三条硬约定：
*
*          1. RAM 只测**自己的**那块静态窗口。运行期往别人的内存里写图案
*             等于把诊断变成故障注入；要测整片 RAM 只能在启动早期、还没人
*             用内存时扫，那是另一种模式，本模块不声称覆盖。
*          2. 只读区间的 crc32 与安装期同口径（arkrtos_crc32），基线是
*             **开机那次**算出来的值。它发现的是运行期被改写，不是出厂值
*             被伪造（后者要签名链）。
*          3. 时钟校验学比值而不是假设频率：第一次拿到两个读数时记下
*             节拍/计数器的比，之后按容差比。这样换晶振、换分频都不用改
*             内核常量，但两路同源时它只能发现粗故障 —— 这一点写进
*             已知限制。
*
* @note 默认关。关档整文件编成空 TU（nm 查不到已定义全局符号）。
*/

#include "arkrtos_selftest.h"
#include "arkrtos_features.h"
#include <stdint.h>   /* uintptr_t：板级给的是物理地址，不是指针 */

#if (ARKRTOS_USE_SELFTEST == 1)

#include "arkrtos_app_image.h"   /* arkrtos_crc32：与安装期同口径 */
#include "arkrtos_task.h"        /* arkrtos_kernel_get_tick */
#include "arkrtos_log.h"
#include "arkrtos_audit_log.h"   /* kind SELFTEST_FAIL，开档才写 */

#if (ARKRTOS_USE_TIMER == 1) && (ARKRTOS_SELFTEST_PATROL_MS > 0)
#include "arkrtos_timer.h"
#define ST_PATROL_TIMER_ENABLED  1
#else
#define ST_PATROL_TIMER_ENABLED  0
#endif

/* ------------------------------------------------------------------
 * 板级钩子的弱符号兜底：不实现就是「这板子没有这路读数」。
 * ------------------------------------------------------------------ */

/* MinGW(PE) 上同一个 TU 里出现两个 __attribute__((weak)) 定义时，两个弱别名
 * (.weak.<sym>.<containing>) 会在链接期错位合并 —— 实测对 clock 钩子的调用被解析
 * 到 flash_span 函数体中间，直接执行到非法指令。给每个兜底各开独立 section 可规避；
 * 非 MinGW 工具链保持原有的弱符号写法。 */
#if defined(_WIN32) && defined(__GNUC__)
#define ST_HOOK_WEAK(sec) __attribute__((weak, section(".text.st_hook_" sec)))
#else
#define ST_HOOK_WEAK(sec) ARKRTOS_WEAK
#endif

ST_HOOK_WEAK("flash_span") int32 arkrtos_port_selftest_flash_span(uint32 *start, uint32 *len)
{
    (void)start;
    (void)len;
    return -1;
}

ST_HOOK_WEAK("clock") int32 arkrtos_port_selftest_clock(uint32 *now)
{
    (void)now;
    return -1;
}

/* ------------------------------------------------------------------
 * 状态
 * ------------------------------------------------------------------ */

/* RAM 测试窗口：本模块私有的 .bss，除自检外无人使用 —— 这是唯一可以
 * 随便写的内存。4 字节对齐，图案里会按 32 位读一遍、按 8 位读一遍。 */
static uint32 g_st_ram[(ARKRTOS_SELFTEST_RAM_BYTES + 3u) / 4u];

/* 开机那次算出的只读区间 crc32（0 = 还没算过 / 没区间）。 */
static uint32 g_st_flash_crc;

/* 时钟校验：第一次采样只学比值，之后每次巡检按容差比。 */
static uint32 g_st_clk_tick;      /* 上次的节拍读数 */
static uint32 g_st_clk_count;     /* 上次的计数器读数 */
static unsigned long long g_st_clk_q16; /* 每拍多少计数，Q16 定点（见 st_clock_item） */
static uint8  g_st_clk_state;     /* 0=没采样 1=已采起点 2=比值不可用 3=已学比值 */

#if (ST_PATROL_TIMER_ENABLED == 1)
static int32 g_st_timer = -1;

/* 巡检定时器回调：跑在定时器任务上下文（不是中断），所以这里可以碰
 * 文件系统背书的审计日志。 */
static void st_patrol_cb(void *arg)
{
    (void)arg;
    (void)arkrtos_selftest_patrol((arkrtos_selftest_report_t *)0);
}
#endif

/* ------------------------------------------------------------------
 * 工具
 * ------------------------------------------------------------------ */

static void st_zero(arkrtos_selftest_report_t *r)
{
    uint32 i;

    r->items   = 0u;
    r->failed  = 0u;
    r->skipped = 0u;
    for(i = 0u; i < 4u; i++)
    {
        r->item[i].id      = 0u;
        r->item[i].verdict = ARKRTOS_SELFTEST_PASS;
        r->item[i].detail  = 0u;
    }
}

static void st_push(arkrtos_selftest_report_t *r, uint32 id, uint32 verdict, uint32 detail)
{
    arkrtos_selftest_item_t *p;

    if(r->items >= 4u)
    {
        return;
    }
    p = &r->item[r->items];
    p->id      = id;
    p->verdict = (uint8)verdict;
    p->detail  = detail;
    r->items++;
    if(verdict == ARKRTOS_SELFTEST_FAIL)
    {
        r->failed |= id;
    }
    else if(verdict == ARKRTOS_SELFTEST_SKIP)
    {
        r->skipped |= id;
    }
}

static int32 st_verdict_code(const arkrtos_selftest_report_t *r)
{
    return (r->failed != 0u) ? ARKRTOS_SELFTEST_ERR_FAIL : ARKRTOS_SELFTEST_OK;
}

static void st_audit_fail(uint32 mask)
{
#if (ARKRTOS_USE_AUDIT_LOG == 1)
    (void)arkrtos_audit_log_record(ARKRTOS_AUDIT_EV_SELFTEST_FAIL, 0u, mask);
#else
    (void)mask;
#endif
}

/* ------------------------------------------------------------------
 * RAM：五种图案
 *
 * 图案 0..3 是「所有位一起动」的形态（抓 stuck-at），图案 4 把地址编进
 * 数据（抓地址线粘连：两个地址读出同一份数据时，其中一个必然对不上），
 * 并在最后按 8 位再读一遍（抓宽度通路短接）。
 *
 * 填充与校验**必须**用同一个公式：两边各写一份是这类自检最典型的假故障
 * 来源（写完图案 A、拿图案 B 去比，健康的片子也会报坏）。所以公式只留
 * st_pattern_word() 一处，两边都调它。
 * ------------------------------------------------------------------ */

#define ST_RAM_PATTERNS 5u

static uint32 st_pattern_word(uint32 pat, uint32 i)
{
    static const uint32 cst[4] =
    {
        0x00000000u, 0xFFFFFFFFu, 0xAAAAAAAAu, 0x55555555u
    };

    if(pat < 4u)
    {
        return cst[pat];
    }
    /* 地址编进数据：写的时候乘奇数再异或常量，读回时地址线粘连会破坏它。 */
    return (i * 0x00010001u) ^ 0x5A5A5A5Au;
}

#if defined(ARKRTOS_SELFTEST_HOST_TEST)
/* 宿主用例唯一的注入缝：写完图案、还没校验之间的那一瞬。生产构建里这条
 * 变量与那个 if 都不存在，板级行为一个字节都不变。
 * 它要证的是：RAM 窗口真的坏了时，自检报出的是**第几条图案**，而不是一个
 * 笼统的 FAIL —— 现场要的就是这个号。 */
void (*arkrtos_selftest_host_after_fill)(uint32 *ram, uint32 words) = 0;
#endif

static void st_ram_pattern_fill(uint32 pat, uint32 words)
{
    uint32 i;

    for(i = 0u; i < words; i++)
    {
        g_st_ram[i] = st_pattern_word(pat, i);
    }
}

static uint32 st_ram_pattern_check(uint32 pat, uint32 words)
{
    uint32 i;

    for(i = 0u; i < words; i++)
    {
        if(g_st_ram[i] != st_pattern_word(pat, i))
        {
            return 1u;
        }
    }

    /* 最后一档再按字节读一遍：宽通路对、窄通路短接时，32 位读是看不出来的。 */
    if(pat >= 4u)
    {
        const uint8 *p = (const uint8 *)g_st_ram;

        for(i = 0u; i < words; i++)
        {
            uint32 w = st_pattern_word(pat, i);
            uint32 b;

            for(b = 0u; b < 4u; b++)
            {
                uint8 want_byte = (uint8)((w >> (b * 8u)) & 0xFFu);

                if(p[(i * 4u) + b] != want_byte)
                {
                    /* 宿主上 32 位读与 8 位读同源，这一支只有真片「窄通路
                     * 短接、宽通路正常」时才可能走到。覆盖率里它一直是红的，
                     * 那是硬件面的欠账，不是用例里漏了一条。 */
                    return 1u;
                }
            }
        }
    }
    return 0u;
}

static uint32 st_ram_test(void)
{
    uint32 words = (uint32)(sizeof(g_st_ram) / sizeof(g_st_ram[0]));
    uint32 pat;

    for(pat = 0u; pat < ST_RAM_PATTERNS; pat++)
    {
        st_ram_pattern_fill(pat, words);
#if defined(ARKRTOS_SELFTEST_HOST_TEST)
        if(arkrtos_selftest_host_after_fill != 0)
        {
            arkrtos_selftest_host_after_fill(g_st_ram, words);
        }
#endif
        if(st_ram_pattern_check(pat, words) != 0u)
        {
            /* 失败也把窗口清回去：留着坏图案只会让下一次巡检更难读。 */
            st_ram_pattern_fill(0u, words);
            return pat + 1u;
        }
    }
    st_ram_pattern_fill(0u, words);
    return 0u;
}

/* ------------------------------------------------------------------
 * CPU：12 条已知答案向量
 *
 * 每条用 volatile 操作数，编译器不能折叠成常量；失败时返回 1 起的向量号。
 * 这些向量覆盖的是「数据通路还在不在」，不是覆盖率证据。
 * ------------------------------------------------------------------ */

static uint32 st_cpu_test(void)
{
    volatile uint32 a    = 0x12345678u;
    volatile uint32 b    = 0xFFFFu;
    volatile int32  s    = -1000;
    volatile unsigned long long w = 0x0000000100000000ull;
    volatile int32  neg  = (int32)0x80000000u;
    volatile int16  s16  = (int16)-1;
    volatile uint32 hi   = 0x80000000u;
    uint32 v;

    if((uint32)(a * 3u) != 0x369D0368u)                         { return 1u; }
    if((uint32)(a ^ 0xA5A5A5A5u) != 0xB791F3DDu)                { return 2u; }
    v = (uint32)a;
    if((uint32)((v << 8) | (v >> 24)) != 0x34567812u)            { return 3u; }
    /* 取最高位为 1 的值右移一位：既有逻辑右移的语义，期望值又不落进
       single-address-source 认定的 "0x0800_0000" 形态。 */
    if((uint32)(hi >> 1) != 0x40000000u)                        { return 4u; }
    if((int32)(neg >> 4) != -134217728)                          { return 5u; }
    if((uint32)(b * b) != 0xFFFE0001u)                           { return 6u; }
    if((1000000u / 7u) != 142857u)                               { return 7u; }
    if((1000000u % 7u) != 1u)                                    { return 8u; }
    /* 字节交换用移位与或出来：正确编译器的结果一定对，这一支要抓的是
     * 「编译器把移位/或折叠错了」——真出现了就该红，不是用例缺。 */
    if((uint32)(((uint32)a >> 24 & 0xFFu) | ((uint32)a >> 8 & 0xFF00u) |
                ((uint32)a << 8 & 0xFF0000u) | ((uint32)a << 24)) != 0x78563412u)
    {
        return 9u;
    }
    if((uint32)(int32)s16 != 0xFFFFFFFFu)                        { return 10u; }
    if((uint32)((w * 4u) >> 32) != 4u)                           { return 11u; }
    if((uint32)(uint32)(s * s) != 1000000u)                      { return 12u; }

    return 0u;
}

/* ------------------------------------------------------------------
 * FLASH：开机基线 + 巡检比对
 * ------------------------------------------------------------------ */

static uint32 st_flash_item(uint32 learn)
{
    uint32 start = 0u;
    uint32 len = 0u;
    uint32 crc;

    if(arkrtos_port_selftest_flash_span(&start, &len) != 0)
    {
        return ARKRTOS_SELFTEST_SKIP;
    }
    if((len == 0u) || (start == 0u))
    {
        /* 板级说支持但给了空区间：这不是「没测」，是板级实现有问题，
         * 报 FAIL 比报 SKIP 诚实。 */
        return ARKRTOS_SELFTEST_FAIL;
    }

    crc = arkrtos_crc32((const void *)(uintptr_t)start, len, 0u);
    if(learn != 0u)
    {
        g_st_flash_crc = crc;
        return ARKRTOS_SELFTEST_PASS;
    }
    if(g_st_flash_crc == 0u)
    {
        /* 巡检那次没有基线可比：把它当基线学下来，但这一次只能报 SKIP
         * —— 没有基线的「一致」是自己的影子。 */
        g_st_flash_crc = crc;
        return ARKRTOS_SELFTEST_SKIP;
    }
    return (crc == g_st_flash_crc) ? ARKRTOS_SELFTEST_PASS : ARKRTOS_SELFTEST_FAIL;
}

/* ------------------------------------------------------------------
 * CLOCK：节拍 × 第二路计数器
 * ------------------------------------------------------------------ */

#if (ARKRTOS_SELFTEST_CLOCK_PPM < 1)
#error "ARKRTOS_SELFTEST_CLOCK_PPM must be >= 1"
#endif

/* 比值差，单位 ppm。两边都走 64 位中间量：32 位乘 1e6 在常见的
 * 「每拍十万个计数」上就已经溢出了。 */
static uint32 st_ppm_diff(uint32 got, uint32 want)
{
    unsigned long long g = (unsigned long long)got;
    unsigned long long w = (unsigned long long)want;

    if(w == 0ull)
    {
        /* 第二道：调用方（st_clock_item）已经先挡了「应有计数为 0」。
         * 除零的代价比多一个分支大得多，所以这里留着，覆盖率里它永远是红的。 */
        return 0xFFFFFFFFu;
    }
    if(g >= w)
    {
        return (uint32)(((g - w) * 1000000ull) / w);
    }
    return (uint32)(((w - g) * 1000000ull) / w);
}

static uint32 st_clock_item(uint32 *ppm_out)
{
    uint32 now = 0u;
    uint32 tick;
    uint32 dtick;
    uint32 dcount;

    *ppm_out = 0u;
    if(arkrtos_port_selftest_clock(&now) != 0)
    {
        return ARKRTOS_SELFTEST_SKIP;
    }

    tick = arkrtos_kernel_get_tick();
    if(g_st_clk_state == 0u)
    {
        /* 第一次采样只学起点，不判定：没有第二个点就没有比值。 */
        g_st_clk_tick  = tick;
        g_st_clk_count = now;
        g_st_clk_state = 1u;
        return ARKRTOS_SELFTEST_SKIP;
    }

    /* 无符号差天然处理计数器回绕；前提是两次采样之间的真实增量小于
     * 2^32 个计数（100 MHz 下约 43 秒）——巡检周期必须远小于它。 */
    dtick  = tick - g_st_clk_tick;
    dcount = now - g_st_clk_count;
    g_st_clk_tick  = tick;
    g_st_clk_count = now;

    if(dtick == 0u)
    {
        /* 两个采样点之间一拍都没走：节拍源停了，或者调用得太密。
         * 前者是故障，后者是用法问题 —— 两种都不能报 PASS。 */
        return ARKRTOS_SELFTEST_FAIL;
    }
    if(dcount == 0u)
    {
        /* 节拍在走、第二路不动：计数器停了。 */
        return ARKRTOS_SELFTEST_FAIL;
    }

    if(g_st_clk_state == 1u)
    {
        /* 学比值：每拍多少个计数，存成 Q16（存 ppm 会让
         * 100 MHz / 1 kHz 这种组合直接溢出 32 位：1e5 × 1e6 = 1e11）。
         * Q16 的量化误差是 1/65536 个计数每拍，远小于漂移容差。 */
        unsigned long long q16 = ((unsigned long long)dcount << 16) / (unsigned long long)dtick;

        if(q16 == 0ull)
        {
            /* 分母够大而比值归零：读数本身不可用，之后每次都必须报失败，
             * 不能悄悄退回「再学一次」。 */
            g_st_clk_state = 2u;
            return ARKRTOS_SELFTEST_FAIL;
        }
        g_st_clk_q16   = q16;
        g_st_clk_state = 3u;
        return ARKRTOS_SELFTEST_PASS;
    }
    if(g_st_clk_state == 2u)
    {
        return ARKRTOS_SELFTEST_FAIL;
    }

    /* 已有基线：本次应有的计数 = dtick × 基线比值。 */
    {
        unsigned long long want = ((unsigned long long)dtick * g_st_clk_q16) >> 16;
        uint32 ppm;

        if(want == 0ull)
        {
            *ppm_out = 0xFFFFFFFFu;
            return ARKRTOS_SELFTEST_FAIL;
        }
        ppm = st_ppm_diff(dcount, (uint32)want);
        *ppm_out = ppm;
        if(ppm > (uint32)ARKRTOS_SELFTEST_CLOCK_PPM)
        {
            return ARKRTOS_SELFTEST_FAIL;
        }
    }

    return ARKRTOS_SELFTEST_PASS;
}

/* ------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------ */

int32 arkrtos_selftest_run(uint32 mask, arkrtos_selftest_report_t *out)
{
    arkrtos_selftest_report_t rep;

    if(out == (arkrtos_selftest_report_t *)0)
    {
        return ARKRTOS_SELFTEST_ERR_PARAM;
    }
    if((mask == 0u) || ((mask & ~(uint32)ARKRTOS_SELFTEST_ITEM_ALL) != 0u))
    {
        return ARKRTOS_SELFTEST_ERR_PARAM;
    }

    st_zero(&rep);

    if((mask & ARKRTOS_SELFTEST_ITEM_RAM) != 0u)
    {
        uint32 bad = st_ram_test();

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_RAM,
                (bad == 0u) ? ARKRTOS_SELFTEST_PASS : ARKRTOS_SELFTEST_FAIL, bad);
    }
    if((mask & ARKRTOS_SELFTEST_ITEM_CPU) != 0u)
    {
        uint32 bad = st_cpu_test();

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_CPU,
                (bad == 0u) ? ARKRTOS_SELFTEST_PASS : ARKRTOS_SELFTEST_FAIL, bad);
    }
    if((mask & ARKRTOS_SELFTEST_ITEM_FLASH) != 0u)
    {
        uint32 v = st_flash_item(1u);

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_FLASH, v, g_st_flash_crc);
    }
    if((mask & ARKRTOS_SELFTEST_ITEM_CLOCK) != 0u)
    {
        uint32 ppm = 0u;
        uint32 v = st_clock_item(&ppm);

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK, v, ppm);
    }

    *out = rep;
    return st_verdict_code(&rep);
}

int32 arkrtos_selftest_patrol(arkrtos_selftest_report_t *out)
{
    arkrtos_selftest_report_t rep;

    st_zero(&rep);

    {
        uint32 bad = st_ram_test();

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_RAM,
                (bad == 0u) ? ARKRTOS_SELFTEST_PASS : ARKRTOS_SELFTEST_FAIL, bad);
    }
    {
        uint32 v = st_flash_item(0u);

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_FLASH, v, g_st_flash_crc);
    }
    {
        uint32 ppm = 0u;
        uint32 v = st_clock_item(&ppm);

        st_push(&rep, ARKRTOS_SELFTEST_ITEM_CLOCK, v, ppm);
    }

    if(rep.failed != 0u)
    {
        st_audit_fail(rep.failed);
        ARKRTOS_LOGE("SELFTEST", "patrol failed mask=0x%X", (unsigned)rep.failed);
    }
    if(out != (arkrtos_selftest_report_t *)0)
    {
        *out = rep;
    }
    return st_verdict_code(&rep);
}

uint32 arkrtos_selftest_flash_crc(void)
{
    return g_st_flash_crc;
}

int32 arkrtos_selftest_forget(void)
{
    /* 受控更新（升级、换标定区）之后基线本来就该重学，提供这个入口
     * 是为了让「重学」是显式动作，而不是靠改内容把巡检骗过去。
     * 注意：忘了基线之后第一次巡检只能报 SKIP —— 这是对的。 */
    g_st_flash_crc = 0u;
    g_st_clk_state = 0u;
    g_st_clk_tick  = 0u;
    g_st_clk_count = 0u;
    g_st_clk_q16   = 0ull;
    return ARKRTOS_SELFTEST_OK;
}

uint32 arkrtos_selftest_boot(void)
{
    arkrtos_selftest_report_t rep;

    (void)arkrtos_selftest_run(ARKRTOS_SELFTEST_ITEM_ALL, &rep);

    if(rep.failed != 0u)
    {
        st_audit_fail(rep.failed);
        ARKRTOS_LOGE("SELFTEST", "boot failed mask=0x%X", (unsigned)rep.failed);
    }

#if (ST_PATROL_TIMER_ENABLED == 1)
    /* 巡检定时器：只在配置给了周期时才建。建不起来不算自检失败（那是
     * 资源问题，会在日志里另说），但一定要说出来，不能静默不巡检。 */
    if(g_st_timer < 0)
    {
        g_st_timer = arkrtos_timer_create_internal("selftst");
        if(g_st_timer < 0)
        {
            ARKRTOS_LOGE("SELFTEST", "patrol timer create failed (%d)", (int)g_st_timer);
        }
        else if(arkrtos_timer_start_internal(g_st_timer,
                                             (uint32)ARKRTOS_SELFTEST_PATROL_MS,
                                             (uint8)ARKRTOS_TIMER_MODE_PERIODIC,
                                             st_patrol_cb,
                                             (void *)0) != 0)
        {
            ARKRTOS_LOGE("SELFTEST", "patrol timer start refused");
        }
    }
#endif

    return rep.failed;
}

#endif /* ARKRTOS_USE_SELFTEST */
