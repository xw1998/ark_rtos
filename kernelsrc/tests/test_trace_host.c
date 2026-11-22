/**
* @file test_trace_host.c
* @brief arkrtos_trace 的宿主测试（内核事件追踪门面 + 控制台命令）
*
* @details 这一层自己不实现环，只把内核的调用点翻译成 mdk_trace 组件的事件，
*          再把组件的计数器与控制台命令接出去。所以它的「错」分成两类：
*
*          - **事件漏了 / 多了一条**：追踪的价值全在「每个切换、每次睡下、
*            每次进出异常都恰好一条」，多一条会让人以为调度器多切了一次，
*            少一条会让时间线上出现没有解释的空洞；
*          - **开关关掉之后还在记**：g_tr_on 是「不改被测对象」的承诺，
*            关掉之后除 PendSV 往返统计外一条都不能进环，切换计数也不能涨。
*
*          三档：
*            SWD  —— 压缩字节环（Cortex-M 的常规配置），覆盖率读 ctrl 字段；
*            BUFF —— 定长记录环（无调试器 / 非 ARM 内核），读数来自组件 API；
*            OFF  —— ARKRTOS_USE_MDK_TRACE=0，整段编成空实现。
*          三档按行号并集合并。
*
*          刻意没验证：
*          - 真实的环编码/压缩（那是 mdk_trace 组件的用例，这里只给它桩）；
*          - SWD 主机侧解码与排空时序（需要调试器）；
*          - 真实 DWT 周期数、中断延迟（宿主上 now 是手调的）。
*/

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "arkrtos_config.h"
#include "arkrtos_trace.h"
#include "mdk_trace.h"

#ifndef TRACE_HOST_SWD
#define TRACE_HOST_SWD   0
#endif
#ifndef TRACE_HOST_BUFF
#define TRACE_HOST_BUFF  0
#endif
#ifndef TRACE_HOST_OFF
#define TRACE_HOST_OFF   0
#endif

#if TRACE_HOST_BUFF
#include "mdk_trace_buff.h"
#elif TRACE_HOST_SWD
#include "mdk_trace_swd.h"
#endif

/* 逐字节比较的两个入口是模块内部的；控制台输出由 shell 头声明，
 * 这里给桩一个同签名原型，避免 -Wmissing-prototypes。 */
void ark_shell_printf(const char *fmt, ...);

#if (ARKRTOS_USE_SCHED_STAT == 1)
/* 这四个账本在 arkrtos_trace.c 里是全局符号，调用点（arkrtos_task.c）
 * 就地 extern 声明——仓库里没有集中头，这里同样就地声明。 */
extern volatile uint32 arkrtos_sched_stat_n;
extern volatile uint32 arkrtos_sched_stat_sum;
extern volatile uint32 arkrtos_sched_stat_min;
extern volatile uint32 arkrtos_sched_stat_max;
extern volatile uint32 arkrtos_pendsv_stat_n;
extern volatile uint32 arkrtos_pendsv_stat_sum;
extern volatile uint32 arkrtos_pendsv_stat_min;
extern volatile uint32 arkrtos_pendsv_stat_max;
void arkrtos_sched_stat_close(uint32 t0);
#endif

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static char g_out[4096];
static int  g_out_len;
static int  g_printf_calls;

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("TRACE-HOST FAIL: %s: got %lu want %lu\n", what, got, want);
    }
}

#if TRACE_HOST_SWD
static void ck_true(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("TRACE-HOST FAIL: %s\n", what);
    }
}
#endif

#if ARKRTOS_USE_MDK_TRACE
static void ck_has(const char *what)
{
    g_checks++;
    if(strstr(g_out, what) == 0)
    {
        g_fails++;
        printf("TRACE-HOST FAIL: output lacks \"%s\": <%s>\n", what, g_out);
    }
}
#endif

/* ------------------------------------------------------------------
 * 桩：控制台输出
 * ------------------------------------------------------------------ */
void ark_shell_printf(const char *fmt, ...)
{
    va_list ap;
    int n;

    g_printf_calls++;
    va_start(ap, fmt);
    n = vsnprintf(g_out + g_out_len, sizeof(g_out) - (size_t)g_out_len, fmt, ap);
    va_end(ap);
    if(n > 0)
    {
        g_out_len += n;
        if(g_out_len > (int)sizeof(g_out) - 1)
        {
            g_out_len = (int)sizeof(g_out) - 1;
        }
    }
}

static void out_reset(void)
{
    g_out[0] = '\0';
    g_out_len = 0;
    g_printf_calls = 0;
}

/* ------------------------------------------------------------------
 * 桩：mdk_trace 组件
 * ------------------------------------------------------------------ */
typedef struct {
    uint16_t id;
    uint8_t  kind;
    uint32_t arg;
} ev_t;

static ev_t     g_ev[64];
static int      g_ev_n;
static uint16_t g_sched_from[32];
static uint16_t g_sched_to[32];
static int      g_sched_n;
static uint16_t g_isr_id[32];
static uint8_t  g_isr_kind[32];
static int      g_isr_n;
static uint32_t g_mark;
static int      g_mark_n;
static uint32_t g_now;
static int      g_init_calls;
static int      g_ark_init_calls;

void mdk_trace_init(void)
{
    g_init_calls++;
}

void mdk_trace_event(uint16_t id, uint8_t kind, uint32_t arg)
{
    if(g_ev_n < (int)(sizeof(g_ev) / sizeof(g_ev[0])))
    {
        g_ev[g_ev_n].id   = id;
        g_ev[g_ev_n].kind = kind;
        g_ev[g_ev_n].arg  = arg;
    }
    g_ev_n++;
}

void mdk_trace_mark(uint32_t tag)
{
    g_mark = tag;
    g_mark_n++;
}

void mdk_trace_sched(uint16_t from, uint16_t to)
{
    if(g_sched_n < 32)
    {
        g_sched_from[g_sched_n] = from;
        g_sched_to[g_sched_n]   = to;
    }
    g_sched_n++;
}

void mdk_trace_isr(uint16_t id, uint8_t kind)
{
    if(g_isr_n < 32)
    {
        g_isr_id[g_isr_n]   = id;
        g_isr_kind[g_isr_n] = kind;
    }
    g_isr_n++;
}

uint32_t mdk_trace_now(void)
{
    return g_now;
}

uint32_t mdk_trace_hz(void)
{
    return 1000u;
}

void mdk_trace_arkrtos_init(void)
{
    g_ark_init_calls++;
}

#if TRACE_HOST_SWD
/* 组件在真实板子上把控制块放在这个符号里；宿主自己定义一份，
 * 用例直接把 ctrl 字段摆成想要的读数。 */
mdk_trace_swd_blob_t mdk_trace_swd_blob;
#endif

#if TRACE_HOST_BUFF
mdk_trace_buff_blob_t mdk_trace_buff_blob;

static uint32_t g_buff_cap;
static uint32_t g_buff_total;
static uint32_t g_buff_lost;

uint32_t mdk_trace_buff_capacity(void)
{
    return g_buff_cap;
}

uint32_t mdk_trace_buff_total(void)
{
    return g_buff_total;
}

uint32_t mdk_trace_buff_lost(void)
{
    return g_buff_lost;
}
#endif

/* ------------------------------------------------------------------
 * 夹具
 * ------------------------------------------------------------------ */
static void reset_counters(void)
{
    g_ev_n = 0;
    g_sched_n = 0;
    g_isr_n = 0;
    g_mark = 0u;
    g_mark_n = 0;
    g_now = 0u;
    g_init_calls = 0;
    g_ark_init_calls = 0;
    out_reset();
}

#if ARKRTOS_USE_MDK_TRACE
static void reset_all(void)
{
    reset_counters();
    arkrtos_trace_enable(1u);
    arkrtos_trace_reset();
}
#endif

/* ------------------------------------------------------------------
 * 用例：阻塞对象登记（文档契约，与模块开关无关）
 * ------------------------------------------------------------------ */
static void test_wait_obj(void)
{
    arkrtos_trace_wait_obj(0u);
    ck_u(arkrtos_trace_take_wait_obj(), 0ul, "take after clearing returns 0");

    arkrtos_trace_wait_obj(0x2C05u);
    ck_u(arkrtos_trace_take_wait_obj(), 0x2C05ul, "registered object returned");
    ck_u(arkrtos_trace_take_wait_obj(), 0ul, "object consumed exactly once");

    arkrtos_trace_wait_obj(0xFFFFu);
    ck_u(arkrtos_trace_take_wait_obj(), 0xFFFFul, "all bits survive the round trip");
}

/* ------------------------------------------------------------------
 * 用例：生命周期 + 开关
 * ------------------------------------------------------------------ */
#if ARKRTOS_USE_MDK_TRACE
static void test_lifecycle(void)
{
    reset_counters();
    arkrtos_trace_enable(0u);
    arkrtos_trace_init();

    ck_u((unsigned long)g_init_calls, 1ul, "init calls the component");
    ck_u((unsigned long)g_ark_init_calls, 1ul, "init installs the kernel hooks once");
    ck_u(arkrtos_trace_enabled(), 1ul, "init arms the facade");

    /* init 清切换计数：先记两次，再 init，再查。 */
    arkrtos_trace_switch(1u, 2u);
    arkrtos_trace_switch(0u, 2u);
    ck_u(arkrtos_trace_switch_count(2u), 2ul, "switch count before init");
    arkrtos_trace_init();
    ck_u(arkrtos_trace_switch_count(2u), 0ul, "init drops the switch counters");

    /* reset 只清计数，不动开关。 */
    arkrtos_trace_switch(3u, 3u);
    arkrtos_trace_reset();
    ck_u(arkrtos_trace_switch_count(3u), 0ul, "reset drops the switch counters");
    ck_u(arkrtos_trace_enabled(), 1ul, "reset leaves the facade armed");

    /* enable 是硬闸门：非 0 一律变 1。 */
    arkrtos_trace_enable(0u);
    ck_u(arkrtos_trace_enabled(), 0ul, "enable(0) disarms");
    arkrtos_trace_enable(7u);
    ck_u(arkrtos_trace_enabled(), 1ul, "any non-zero arms");
}

/* ------------------------------------------------------------------
 * 用例：记录路径与开关语义
 * ------------------------------------------------------------------ */
static void test_record(void)
{
    reset_all();
    arkrtos_trace_record(ARKRTOS_TR_EV_MARK, 42u);
    ck_u((unsigned long)g_ev_n, 1ul, "one record -> one event");
    ck_u(g_ev[0].id, ARKRTOS_TR_EV_MARK, "event id passed through");
    ck_u(g_ev[0].kind, MDK_TRACE_KIND_POINT, "record is a point event");
    ck_u(g_ev[0].arg, 42ul, "payload passed through");

    arkrtos_trace_wait(7u);
    ck_u((unsigned long)g_ev_n, 2ul, "wait -> one event");
    ck_u(g_ev[1].id, ARKRTOS_TR_EV_WAIT, "wait event id");
    ck_u(g_ev[1].arg, 7ul, "wait carries the task index");

    arkrtos_trace_switch(1u, 2u);
    ck_u((unsigned long)g_sched_n, 1ul, "switch -> one sched event");
    ck_u(g_sched_from[0], 1ul, "sched from");
    ck_u(g_sched_to[0], 2ul, "sched to");
    ck_u(arkrtos_trace_switch_count(2u), 1ul, "switched-in task counted");
    ck_u(arkrtos_trace_switch_count(1u), 0ul, "switched-out task not counted");

    /* 越界 id 不进计数器，但事件照样上报（主机可能用高位 id 表示别的上下文）。 */
    arkrtos_trace_switch(0u, 200u);
    ck_u((unsigned long)g_sched_n, 2ul, "out-of-range to still reported");
    ck_u(arkrtos_trace_switch_count(200u), 0ul, "out-of-range id reads 0");
    ck_u(arkrtos_trace_switch_count(15u), 0ul, "unused in-range id reads 0");

    arkrtos_trace_isr(ARKRTOS_TR_IRQ_SYSTICK, ARKRTOS_TR_ISR_ENTER);
    ck_u((unsigned long)g_isr_n, 1ul, "isr -> one isr event");
    ck_u(g_isr_id[0], ARKRTOS_TR_IRQ_SYSTICK, "isr id");
    ck_u(g_isr_kind[0], ARKRTOS_TR_ISR_ENTER, "isr kind");

    /* 关掉之后：一条都不许进，计数一个都不许涨。 */
    arkrtos_trace_enable(0u);
    g_ev_n = 0;
    g_sched_n = 0;
    g_isr_n = 0;
    arkrtos_trace_record(ARKRTOS_TR_EV_MARK, 1u);
    arkrtos_trace_wait(1u);
    arkrtos_trace_switch(1u, 2u);
    arkrtos_trace_isr(ARKRTOS_TR_IRQ_SYSTICK, ARKRTOS_TR_ISR_ENTER);
    ck_u((unsigned long)g_ev_n, 0ul, "disabled: no point events");
    ck_u((unsigned long)g_sched_n, 0ul, "disabled: no sched events");
    ck_u((unsigned long)g_isr_n, 0ul, "disabled: no isr events");
    ck_u(arkrtos_trace_switch_count(2u), 1ul, "disabled: counter neither grows nor resets");

    arkrtos_trace_enable(1u);
}

/* ------------------------------------------------------------------
 * 用例：调度开销统计（PendSV 往返与切换耗时）
 * ------------------------------------------------------------------ */
static void test_sched_stat(void)
{
#if (ARKRTOS_USE_SCHED_STAT == 1)
    /* 切换耗时：由调度器在切换点调用 close(t0)。 */
    arkrtos_sched_stat_n = 0u;
    arkrtos_sched_stat_sum = 0u;
    arkrtos_sched_stat_min = 0xFFFFFFFFu;
    arkrtos_sched_stat_max = 0u;

    g_now = 100u;
    arkrtos_sched_stat_close(40u);
    g_now = 160u;
    arkrtos_sched_stat_close(100u);

    ck_u(arkrtos_sched_stat_n, 2ul, "sched stat: two samples");
    ck_u(arkrtos_sched_stat_sum, 120ul, "sched stat: sum of deltas");
    ck_u(arkrtos_sched_stat_min, 60ul, "sched stat: min delta");
    ck_u(arkrtos_sched_stat_max, 60ul, "sched stat: max delta");

    /* PendSV 往返：夹在 isr(14, enter/exit) 之间，且 trace 关掉也能量。 */
    arkrtos_pendsv_stat_n = 0u;
    arkrtos_pendsv_stat_sum = 0u;
    arkrtos_pendsv_stat_min = 0xFFFFFFFFu;
    arkrtos_pendsv_stat_max = 0u;
    arkrtos_trace_enable(0u);

    g_now = 10u;
    arkrtos_trace_isr(14u, 0u);
    g_now = 34u;
    arkrtos_trace_isr(14u, 1u);
    g_now = 50u;
    arkrtos_trace_isr(14u, 0u);
    g_now = 90u;
    arkrtos_trace_isr(14u, 1u);

    ck_u(arkrtos_pendsv_stat_n, 2ul, "pendsv stat: two round trips");
    ck_u(arkrtos_pendsv_stat_sum, 64ul, "pendsv stat: sum");
    ck_u(arkrtos_pendsv_stat_min, 24ul, "pendsv stat: min");
    ck_u(arkrtos_pendsv_stat_max, 40ul, "pendsv stat: max");
    ck_u((unsigned long)g_isr_n, 0ul, "pendsv stat measured while trace is off");

    /* 非 PendSV 的异常不进这张账。 */
    arkrtos_trace_isr(ARKRTOS_TR_IRQ_SYSTICK, 0u);
    ck_u(arkrtos_pendsv_stat_n, 2ul, "other irqs do not touch pendsv stat");

    arkrtos_trace_enable(1u);
#endif
}

/* ------------------------------------------------------------------
 * 用例：后台读数（三档各自的来源）
 * ------------------------------------------------------------------ */
static void test_backend_readouts(void)
{
    g_now = 0x12345678u;
    ck_u(arkrtos_trace_last_cycles(), 0x12345678ul, "last_cycles forwards now");

#if TRACE_HOST_SWD
    mdk_trace_swd_blob.ctrl.cap = 8192u;
    mdk_trace_swd_blob.ctrl.events = 42u;
    mdk_trace_swd_blob.ctrl.lost_events = 3u;
    mdk_trace_swd_blob.ctrl.lost_bytes = 17u;
    mdk_trace_swd_blob.ctrl.head = 100u;
    mdk_trace_swd_blob.ctrl.drained = 40u;
    mdk_trace_swd_blob.ctrl.seq = 5u;
    mdk_trace_swd_blob.ctrl.tokens = 50u;

    ck_u(arkrtos_trace_capacity(), 8192ul, "swd capacity from ctrl");
    ck_u(arkrtos_trace_total(), 42ul, "swd total from ctrl");
    ck_u(arkrtos_trace_overrun(), 3ul, "swd overrun is lost_events");
#elif TRACE_HOST_BUFF
    g_buff_cap = 2048u;
    g_buff_total = 17u;
    g_buff_lost = 2u;
    mdk_trace_buff_blob.ctrl.flags = (uint32_t)MDK_TRACE_BUFF_FLAG_WRAPPED;

    ck_u(arkrtos_trace_capacity(), 2048ul, "buff capacity from component");
    ck_u(arkrtos_trace_total(), 17ul, "buff total from component");
    ck_u(arkrtos_trace_overrun(), 2ul, "buff overrun is lost");
#else
    ck_u(arkrtos_trace_capacity(), 0ul, "off: capacity 0");
    ck_u(arkrtos_trace_total(), 0ul, "off: total 0");
    ck_u(arkrtos_trace_overrun(), 0ul, "off: overrun 0");
#endif
}

/* ------------------------------------------------------------------
 * 用例：控制台命令
 * ------------------------------------------------------------------ */
static void test_shell(void)
{
    char *argv[3];
    char s_trace[] = "trace";
    char s_start[] = "start";
    char s_stop[] = "stop";
    char s_reset[] = "reset";
    char s_dump[] = "dump";
    char s_mark[] = "mark";
    char s_123[] = "123";
    char s_12x3[] = "12x3";
    char s_startx[] = "startx";

    /* start：arming + 报告容量/频率 */
    reset_counters();
    arkrtos_trace_enable(0u);
    argv[0] = s_trace;
    argv[1] = s_start;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "start returns 0");
    ck_u(arkrtos_trace_enabled(), 1ul, "start arms");
    ck_u((unsigned long)g_init_calls, 1ul, "start calls init");
    ck_has("armed");
    ck_has("1000 ticks/s");

    /* stop：disarm + 报告累计 */
    argv[1] = s_stop;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "stop returns 0");
    ck_u(arkrtos_trace_enabled(), 0ul, "stop disarms");
    ck_has("stopped");

    /* reset：只清切换计数 */
    arkrtos_trace_enable(1u);
    arkrtos_trace_switch(1u, 4u);
    ck_u(arkrtos_trace_switch_count(4u), 1ul, "counter before reset");
    reset_counters();
    argv[1] = s_reset;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "reset returns 0");
    ck_u(arkrtos_trace_switch_count(4u), 0ul, "reset clears counters");
    ck_u(arkrtos_trace_enabled(), 1ul, "reset keeps trace armed");
    ck_has("switch counters cleared");

    /* dump / status：健康度一行 */
    argv[1] = s_dump;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "dump returns 0");
    ck_has("trace : ARMED");

    reset_counters();
    ck_u((unsigned long)arkrtos_trace_shell_cmd(1, argv), 0ul, "no subcommand returns 0");
    ck_has("trace : ARMED");
    ck_has("usage : trace");

    /* mark：缺省 0，按十进制解析，遇到非数字停 */
    reset_counters();
    argv[1] = s_mark;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "mark without value returns 0");
    ck_u((unsigned long)g_mark_n, 1ul, "mark emitted once");
    ck_u(g_mark, 0ul, "mark defaults to 0");

    argv[2] = s_123;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(3, argv), 0ul, "mark with value returns 0");
    ck_u(g_mark, 123ul, "mark parses decimal");

    argv[2] = s_12x3;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(3, argv), 0ul, "mark stops at non-digit");
    ck_u(g_mark, 12ul, "mark takes the digit prefix");

    /* 近似子命令不能被当成 start/stop：逐字符相等才算 */
    reset_counters();
    argv[1] = s_startx;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "unknown returns 0");
    ck_u((unsigned long)g_init_calls, 0ul, "startx does not init");
    ck_has("usage : trace");

#if TRACE_HOST_SWD
    /* SWD 档：pending 与丢弃提示 */
    reset_all();
    mdk_trace_swd_blob.ctrl.cap = 8192u;
    mdk_trace_swd_blob.ctrl.events = 42u;
    mdk_trace_swd_blob.ctrl.lost_events = 3u;
    mdk_trace_swd_blob.ctrl.lost_bytes = 17u;
    mdk_trace_swd_blob.ctrl.head = 100u;
    mdk_trace_swd_blob.ctrl.drained = 40u;
    mdk_trace_swd_blob.ctrl.seq = 5u;
    mdk_trace_swd_blob.ctrl.tokens = 50u;

    argv[1] = s_dump;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "swd dump returns 0");
    ck_has("seq=5");
    ck_has("cap=8192");
    ck_has("events=42");
    ck_has("tokens=50");
    ck_has("pending=60");
    ck_has("lost=17 B");
    ck_has("3 events were dropped");
    ck_has("trace_swd_status");

    /* 没有丢弃时不打提示行 */
    mdk_trace_swd_blob.ctrl.lost_events = 0u;
    reset_counters();
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "swd dump clean returns 0");
    ck_true(strstr(g_out, "were dropped") == 0, "no drop hint when nothing was lost");
#elif TRACE_HOST_BUFF
    /* BUFF 档：records / lost / wrapped */
    reset_all();
    g_buff_cap = 2048u;
    g_buff_total = 17u;
    g_buff_lost = 2u;
    mdk_trace_buff_blob.ctrl.flags = (uint32_t)MDK_TRACE_BUFF_FLAG_WRAPPED;

    argv[1] = s_dump;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "buff dump returns 0");
    ck_has("cap=2048");
    ck_has("total=17");
    ck_has("lost=2");
    ck_has("wrapped=1");
    ck_has("mdk_trace_buff_blob");

    mdk_trace_buff_blob.ctrl.flags = 0u;
    reset_counters();
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "buff dump clean returns 0");
    ck_has("wrapped=0");
#else
    /* 无后端档（SWD/BUFF 都没开）：只报 ARMED 与「没有片上后端」 */
    reset_all();
    argv[1] = s_dump;
    ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv), 0ul, "no-backend dump returns 0");
    ck_has("no on-chip backend");
#endif
}
#endif /* ARKRTOS_USE_MDK_TRACE */

/* ------------------------------------------------------------------
 * 主函数
 * ------------------------------------------------------------------ */
int main(void)
{
    test_wait_obj();

#if ARKRTOS_USE_MDK_TRACE
    test_lifecycle();
    test_record();
    test_sched_stat();
    test_backend_readouts();
    test_shell();
#else
    {
        /* 关档：所有入口都是空实现，读数恒 0，控制台命令恒拒绝。 */
        char *argv[2];
        char s_trace[] = "trace";
        char s_start[] = "start";

        reset_counters();
        arkrtos_trace_init();
        arkrtos_trace_enable(1u);
        arkrtos_trace_reset();

        ck_u((unsigned long)g_init_calls, 0ul, "off: init emits nothing");
        ck_u((unsigned long)g_ark_init_calls, 0ul, "off: hooks not installed");
        ck_u(arkrtos_trace_enabled(), 0ul, "off: stays disabled");
        ck_u(arkrtos_trace_capacity(), 0ul, "off: capacity");
        ck_u(arkrtos_trace_total(), 0ul, "off: total");
        ck_u(arkrtos_trace_last_cycles(), 0ul, "off: last_cycles");
        ck_u(arkrtos_trace_switch_count(0u), 0ul, "off: switch_count");
        ck_u(arkrtos_trace_overrun(), 0ul, "off: overrun");

        arkrtos_trace_record(ARKRTOS_TR_EV_MARK, 1u);
        arkrtos_trace_switch(1u, 2u);
        arkrtos_trace_wait(1u);
        arkrtos_trace_isr(ARKRTOS_TR_IRQ_SYSTICK, ARKRTOS_TR_ISR_ENTER);
        ck_u((unsigned long)g_ev_n, 0ul, "off: no events");
        ck_u((unsigned long)g_sched_n, 0ul, "off: no sched events");
        ck_u((unsigned long)g_isr_n, 0ul, "off: no isr events");

        argv[0] = s_trace;
        argv[1] = s_start;
        ck_u((unsigned long)arkrtos_trace_shell_cmd(2, argv),
             (unsigned long)ARKRTOS_TRACE_ERR_DISABLED, "off: shell refuses");
        ck_u((unsigned long)g_printf_calls, 0ul, "off: shell prints nothing");
    }
#endif

#if ARKRTOS_USE_MDK_TRACE
    printf("TRACE-HOST OK  %d checks\n", g_checks);
#else
    printf("TRACE-HOST OFF OK  %d checks\n", g_checks);
#endif
    if(g_fails != 0)
    {
        printf("TRACE-HOST FAIL: %d of %d checks failed\n", g_fails, g_checks);
        return 1;
    }
    return 0;
}
