/**
* @file test_log_host.c
* @brief arkrtos_log 的宿主测试（分级日志 + 控制台锁 + dmesg 环）
*
* @details 这个模块是整个系统出事之后唯一还能说话的地方，所以它的「错」都很具体：
*
*          - **级别过滤写反或写成闭区间**：把 ERROR 也一起静音，或者该被裁掉的
*            DEBUG 还在打——现场看到的是一串日志，看不出少了谁；
*          - **前缀格式**：dmesg 风格的时间戳/优先级是自动化抓取日志时唯一的解析
*            依据，多一个空格、颜色跑到正文上都算格式变了；
*          - **控制台锁**：它存在的意义是「一行不被别人插进中间」，但取不到锁时
*            **仍然要写**（丢输出比偶尔拼接更难查），所以「等满预算就放弃并记数」
*            必须是一条被钉住的行为，而不是实现细节；
*          - **发送管道满**：逐字节等，等满预算才丢，并且丢的是真实丢字节数；
*          - **dmesg 环**：满时覆盖最旧，回卷后 dump 的顺序是「最旧 → 最新」。
*
*          三档：默认档（LOG + RING，环容量被压到 64 字节以便走回卷）、
*          RING=0 档（环整块编掉，capacity/used/dump 都答 0）、LOG=0 档（整模块
*          编成「恒拒绝」的桩）。三档的读数按行号并集合并。
*
*          刻意没验证：
*          - 真实串口时序与 TXE 中断（宿主上写接口是内存数组）；
*          - 多核并发下的锁语义（宿主是单线程 + 可编程 CAS 失败）；
*          - 用户态 SVC 0x19 的分发与字符串校验（那在 SVC 层）。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_config.h"
#include "arkrtos_log.h"
#include "arkrtos_dev.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

#if (ARKRTOS_USE_LOG == 1)
/* ck / ck_str 只在 LOG 打开的那两档里用到；OFF 档只用 ck_eq_u */
static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

static void ck_str(const char *got, const char *want, const char *what)
{
    g_checks++;
    if((got == 0) || (want == 0) || (strcmp(got, want) != 0))
    {
        g_fails++;
        printf("FAIL: %s\n  got : %s\n  want: %s\n", what,
               (got != 0) ? got : "(null)", (want != 0) ? want : "(null)");
    }
}
#endif /* ARKRTOS_USE_LOG == 1 */

static void ck_eq_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL: %s (got %lu, want %lu)\n", what, got, want);
    }
}

/* ------------------------------------------------------------------
 * 桩：设备框架、原子 CAS、DMB、系统时间
 * ------------------------------------------------------------------ */
#define OUT_MAX 4096

static char   g_out[OUT_MAX];
static int    g_out_len = 0;

static int32  g_open_ret    = -1;      /* 设备打开返回的句柄 */
static int    g_open_calls  = 0;
static char   g_open_name[32];
static uint32 g_open_param  = 0u;

/* 写接口三种形态：正常、始终写不进（管道一直满）、前 N 次写不进 */
static int    g_write_full  = 0;
static int    g_write_misses = 0;

static uint32 g_time_ms = 0u;

/* CAS：g_cas_fail_budget 次先失败，用来模拟「锁一直被别人占着」 */
static unsigned long g_cas_fail_budget = 0ul;
static int    g_cas_calls = 0;

#if (ARKRTOS_USE_LOG == 1)
static void reset_all(void)
{
    g_out_len = 0;
    g_out[0] = '\0';
    g_open_ret = -1;
    g_open_calls = 0;
    g_open_name[0] = '\0';
    g_open_param = 0u;
    g_write_full = 0;
    g_write_misses = 0;
    g_time_ms = 0u;
    g_cas_fail_budget = 0ul;
    g_cas_calls = 0;
}
#endif /* ARKRTOS_USE_LOG == 1 */

int32 arkrtos_dev_open_internal(const char *name, uint32 param)
{
    g_open_calls++;
    g_open_param = param;
    if(name != 0)
    {
        (void)snprintf(g_open_name, sizeof(g_open_name), "%s", name);
    }
    return g_open_ret;
}

int32 arkrtos_dev_write_internal(int32 handle, uint8 *pdata, int32 len)
{
    (void)handle;
    if((pdata == 0) || (len <= 0))
    {
        return -1;
    }
    if(g_write_full != 0)
    {
        return 0;                       /* 管道满：一个字节也进不去 */
    }
    if(g_write_misses > 0)
    {
        g_write_misses--;
        return 0;
    }
    if(g_out_len + len < OUT_MAX)
    {
        memcpy(&g_out[g_out_len], pdata, (size_t)len);
        g_out_len += len;
        g_out[g_out_len] = '\0';
    }
    return len;
}

uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
{
    g_cas_calls++;
    if(g_cas_fail_budget > 0ul)
    {
        g_cas_fail_budget--;
        return 0u;
    }
    if(*p_addr == expect)
    {
        *p_addr = new_value;
        return 1u;
    }
    return 0u;
}

void arkrtos_port_dmb(void)
{
}

uint32 arkrtos_kernel_get_time(void)
{
    return g_time_ms;
}

#if (ARKRTOS_USE_LOG == 1)

/* ------------------------------------------------------------------
 * 用例：默认档与 RING=0 档共用
 * ------------------------------------------------------------------ */

/* 控台设备打开之前：所有写路径都答「没有设备」，且不假装写成功。 */
static void test_before_dev(void)
{
    const uint8 one = (uint8)'A';

    reset_all();
    g_open_ret = -1;

    ck_eq_u((unsigned long)(int32)arkrtos_console_handle(), (unsigned long)-1,
            "no device yet: handle is -1");
    ck_eq_u((unsigned long)g_open_calls, 1ul, "handle() is what opens the console");
    ck(arkrtos_console_is_handle(-1) == 1,
       "is_handle(-1) is true while no device is open (same cached value)");
    ck_eq_u((unsigned long)g_open_calls, 2ul,
            "a failed open is NOT a negative cache: the next call retries");
    ck(arkrtos_console_is_handle(7) == 0, "is_handle(7) is false");

    ck_eq_u((unsigned long)(int32)arkrtos_console_write(&one, 1u),
            (unsigned long)ARKRTOS_LOG_ERR_DEVICE, "no device: write is refused");
    ck_eq_u((unsigned long)g_out_len, 0ul, "and nothing was written");
    ck_eq_u((unsigned long)g_open_calls, 4ul, "the refused write tried to open too");

    ck_eq_u((unsigned long)arkrtos_console_write(0, 4u), 0ul,
            "null data is a no-op");
    ck_eq_u((unsigned long)arkrtos_console_write(&one, 0u), 0ul, "zero length is a no-op");
    ck_eq_u((unsigned long)g_open_calls, 6ul,
            "both no-op writes still probe the device first (the open is the first statement)");
    ck_str(g_open_name, SHELL_DEV_NAME, "the console device name is the shell one");
    ck_eq_u((unsigned long)g_open_param, (unsigned long)SHELL_DEV_ARG,
            "and the open arg is the shell one");

    /* 设备一旦打开成功就不再重复 open：见 test_open_once。 */
    g_open_ret = 3;
    ck_eq_u((unsigned long)(int32)arkrtos_console_handle(), 3ul, "the retry succeeds");
    ck_eq_u((unsigned long)g_open_calls, 7ul, "the handle is cached from then on");
    (void)arkrtos_console_handle();
    ck_eq_u((unsigned long)g_open_calls, 7ul, "an opened console is never re-opened");
}

/* 打开之后：init 走同一个缓存，第二次 init 不再 open。
 * 注：g_log_dev 是模块内的静态量，宿主上看不见也改不了，所以这里继承
 * 上一个用例打开成功的句柄（3），不再重新 open——这正是被测契约本身。 */
static void test_open_once(void)
{
    reset_all();
    g_open_ret = 7;

    arkrtos_log_init();
    ck_eq_u((unsigned long)g_open_calls, 0ul,
            "init finds the console already open and does not open again");
    ck_eq_u((unsigned long)(int32)arkrtos_console_handle(), 3ul,
            "the handle stayed the one from the first successful open");
    ck(arkrtos_console_is_handle(3) == 1, "is_handle(opened) is true");
    ck_eq_u((unsigned long)arkrtos_log_get_level(), (unsigned long)ARKRTOS_LOG_LEVEL,
            "init restores the compile-time level");

    arkrtos_log_init();
    ck_eq_u((unsigned long)g_open_calls, 0ul, "a second init still reuses the cached handle");
}

static void test_levels(void)
{
    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();

    arkrtos_log_set_level((uint32)ARKRTOS_LOG_DEBUG);
    ck_eq_u((unsigned long)arkrtos_log_get_level(), (unsigned long)ARKRTOS_LOG_DEBUG,
            "level can be raised to DEBUG");
    arkrtos_log_set_level((uint32)ARKRTOS_LOG_DEBUG + 1u);
    ck_eq_u((unsigned long)arkrtos_log_get_level(), (unsigned long)ARKRTOS_LOG_DEBUG,
            "an out-of-range level is ignored, not clamped");
    arkrtos_log_set_level((uint32)ARKRTOS_LOG_NONE);
    ck_eq_u((unsigned long)arkrtos_log_get_level(), 0ul, "level can be set to NONE");
}

/* 前缀格式：时间戳 + syslog 优先级 + 复位 + tag:line + 空格 + 正文 + CRLF。 */
static void test_emit_format(void)
{
    char want[512];

    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();
    arkrtos_log_set_level((uint32)ARKRTOS_LOG_DEBUG);
    arkrtos_log_ring_clear();

    g_time_ms = 1234u;
    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "APP", 42u, "hello");

    (void)snprintf(want, sizeof(want), "%s[    1.234] <6>%sAPP:42 hello\r\n",
                   ARKRTOS_LOG_COLOR_INFO, ARKRTOS_LOG_COLOR_RESET);
    ck_str(g_out, want, "one INFO line, dmesg shaped, coloured prefix only");

    /* 正文里出现空行不算新行：整串一致就够了 */

    /* line == 0：不打印行号；tag == 0：用 ? 顶上 */
    g_out_len = 0;
    g_out[0] = '\0';
    g_time_ms = 5u;
    arkrtos_log_emit((uint32)ARKRTOS_LOG_WARNING, 0, 0u, "w");
    (void)snprintf(want, sizeof(want), "%s[    0.005] <4>%s? w\r\n",
                   ARKRTOS_LOG_COLOR_WARNING, ARKRTOS_LOG_COLOR_RESET);
    ck_str(g_out, want, "line 0 omits :line, null tag becomes ?");

    /* ERROR 是 3，DEBUG 是 7：优先级映射逐档核对 */
    g_out_len = 0;
    g_out[0] = '\0';
    arkrtos_log_emit((uint32)ARKRTOS_LOG_ERROR, "E", 1u, "m");
    (void)snprintf(want, sizeof(want), "%s[    0.005] <3>%sE:1 m\r\n",
                   ARKRTOS_LOG_COLOR_ERROR, ARKRTOS_LOG_COLOR_RESET);
    ck_str(g_out, want, "ERROR maps to syslog 3 with its own colour");

    /* 注：pri/color 的 default 分支在宿主上不可达——级别是 0..4 的枚举，
     * 而过滤条件先拦掉 >DEBUG，所有过得去的值都在 switch 的命名档里。
     * 那两行是防御性兜底，不动它，也不为了让覆盖率变绿去造假调用。 */

    g_out_len = 0;
    g_out[0] = '\0';
    arkrtos_log_emit((uint32)ARKRTOS_LOG_DEBUG, "D", 2u, "m");
    (void)snprintf(want, sizeof(want), "%s[    0.005] <7>%sD:2 m\r\n",
                   ARKRTOS_LOG_COLOR_DEBUG, ARKRTOS_LOG_COLOR_RESET);
    ck_str(g_out, want, "DEBUG maps to syslog 7 with its own colour");
}

/* 过滤：NONE、高于当前级别、高于 DEBUG、空正文 都不产生任何输出。 */
static void test_emit_filter(void)
{
    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();
    arkrtos_log_set_level((uint32)ARKRTOS_LOG_INFO);
    arkrtos_log_ring_clear();

    arkrtos_log_emit((uint32)ARKRTOS_LOG_DEBUG, "A", 1u, "dropped");
    ck_eq_u((unsigned long)g_out_len, 0ul, "DEBUG is dropped when the level is INFO");

    arkrtos_log_emit((uint32)ARKRTOS_LOG_NONE, "A", 1u, "dropped");
    ck_eq_u((unsigned long)g_out_len, 0ul, "NONE is always dropped");

    arkrtos_log_emit((uint32)ARKRTOS_LOG_DEBUG + 5u, "A", 1u, "dropped");
    ck_eq_u((unsigned long)g_out_len, 0ul, "anything above DEBUG is dropped");

    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "A", 1u, 0);
    ck_eq_u((unsigned long)g_out_len, 0ul, "a null message is dropped before formatting");
#if ARKRTOS_USE_LOG_RING
    ck_eq_u((unsigned long)arkrtos_log_ring_used(), 0ul,
            "and a dropped line never reaches the dmesg ring");
#endif

    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "A", 1u, "");
    ck(g_out_len > 0, "an empty (but non-null) message still prints its prefix");
}

/* 控制台锁：拿到就返回 1；等满预算返回 0 并且计一次「放弃」。 */
static void test_console_lock(void)
{
    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();

    ck_eq_u((unsigned long)(int32)arkrtos_console_lock(), 1ul, "an idle lock is taken");
    arkrtos_console_unlock();
    ck_eq_u((unsigned long)(int32)arkrtos_console_lock(), 1ul, "and can be taken again");
    arkrtos_console_unlock();

    /* console_write 自己取锁（上一行已归还，否则它会等满预算并把锁丢掉） */
    {
        const uint8 one = (uint8)'Z';
        (void)arkrtos_console_write(&one, 1u);
        ck_str(g_out, "Z", "console_write pushes exactly the bytes it was given");
        ck_eq_u((unsigned long)arkrtos_console_busy_count(), 0ul,
                "and it did so without a give-up");
    }

    /* 锁一直被占：等满预算后放弃，但记住这一次放弃，且调用方仍然写 */
    g_cas_fail_budget = 1000000ul;
    ck_eq_u((unsigned long)(int32)arkrtos_console_lock(), 0ul,
            "a lock held for the whole budget is given up on");
    ck_eq_u((unsigned long)arkrtos_console_busy_count(), 1ul,
            "and the give-up is counted");
    g_cas_fail_budget = 0ul;
}

/* 字节日志：管道一直满时，等满预算后丢字节并计数，返回值仍是请求的长度。 */
static void test_tx_drop(void)
{
    uint32 before;

    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();

    before = arkrtos_console_tx_drop_count();
    g_write_full = 1;
    ck_eq_u((unsigned long)(int32)arkrtos_console_write((const uint8 *)"ab", 2u), 2ul,
            "console_write reports the requested length even when bytes are dropped");
    ck_eq_u((unsigned long)(arkrtos_console_tx_drop_count() - before), 2ul,
            "both bytes are counted as dropped");
    ck_eq_u((unsigned long)g_out_len, 0ul, "and nothing reached the device");

    /* 同一份预算在整行输出上也生效：一行 N 字节 → 丢 N 字节 */
    before = arkrtos_console_tx_drop_count();
    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "T", 1u, "x");
    ck(arkrtos_console_tx_drop_count() > before, "a whole line is dropped byte by byte");

    g_write_full = 0;
    g_out_len = 0;
    g_out[0] = '\0';
    g_write_misses = 3;                 /* 每个字节先失败三次再进去 */
    ck_eq_u((unsigned long)(int32)arkrtos_console_write((const uint8 *)"cd", 2u), 2ul,
            "a pipe that frees up within the budget loses nothing");
    ck_str(g_out, "cd", "and the bytes arrive in order");
}

/* SVC 入口：越界级别按 DEBUG 处理，空正文报参数错。 */
static void test_svc(void)
{
    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();
    arkrtos_log_set_level((uint32)ARKRTOS_LOG_DEBUG);
    arkrtos_log_ring_clear();

    ck_eq_u((unsigned long)(int32)arkrtos_log_svc((uint32)ARKRTOS_LOG_DEBUG + 9u, "S", "m"),
            0ul, "an oversized level is clamped, not refused");
    ck(g_out_len > 0, "and the clamped line is emitted (its priority is DEBUG)");
#if ARKRTOS_USE_LOG_RING
    ck(arkrtos_log_ring_used() > 0ul, "and it also landed in the dmesg ring");
#endif

    ck_eq_u((unsigned long)(int32)arkrtos_log_svc((uint32)ARKRTOS_LOG_INFO, "S", 0),
            (unsigned long)ARKRTOS_LOG_ERR_PARAM, "a null message is a parameter error");

    arkrtos_log_set_level((uint32)ARKRTOS_LOG_NONE);
    ck_eq_u((unsigned long)(int32)arkrtos_log_svc((uint32)ARKRTOS_LOG_INFO, "S", "m"), 0ul,
            "a filtered line still reports success (the filter is not an error)");
}

#endif /* ARKRTOS_USE_LOG == 1 */

/* ------------------------------------------------------------------
 * dmesg 环：两档都测「关掉时答 0」，默认档还测回卷与 dump 顺序
 * ------------------------------------------------------------------ */
#if (ARKRTOS_USE_LOG == 1) && (ARKRTOS_USE_LOG_RING == 1)

static char   g_dump[512];
static int    g_dump_len = 0;
static int    g_dump_calls = 0;

static void dump_put(const char *data, uint32 len, void *arg)
{
    (void)arg;
    g_dump_calls++;
    if(g_dump_len + (int)len < (int)sizeof(g_dump))
    {
        memcpy(&g_dump[g_dump_len], data, (size_t)len);
        g_dump_len += (int)len;
        g_dump[g_dump_len] = '\0';
    }
}

static void test_ring(void)
{
    reset_all();
    g_open_ret = 7;
    arkrtos_log_init();
    arkrtos_log_set_level((uint32)ARKRTOS_LOG_INFO);

    arkrtos_log_ring_clear();
    ck_eq_u((unsigned long)arkrtos_log_ring_capacity(),
            (unsigned long)ARKRTOS_LOG_RING_SIZE, "capacity is the configured size");
    ck_eq_u((unsigned long)arkrtos_log_ring_used(), 0ul, "a cleared ring is empty");

    /* 空环 dump：一次回调都不该发生 */
    g_dump_len = 0;
    g_dump[0] = '\0';
    g_dump_calls = 0;
    ck_eq_u((unsigned long)arkrtos_log_ring_dump(dump_put, 0), 0ul,
            "an empty ring dumps nothing");
    ck_eq_u((unsigned long)g_dump_calls, 0ul, "and the callback is not called");

    /* dump 传空回调也不崩 */
    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "R", 1u, "a");
    ck_eq_u((unsigned long)arkrtos_log_ring_dump(0, 0), 0ul,
            "a null callback is refused without crashing");

    /* 未回卷：从 0 开始的一段，dump 与已用字节逐字节一致 */
    arkrtos_log_ring_clear();
    g_time_ms = 7u;
    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "R", 1u, "first");
    ck(arkrtos_log_ring_used() > 0ul, "an emitted line lands in the ring");
    g_dump_len = 0;
    g_dump[0] = '\0';
    g_dump_calls = 0;
    ck_eq_u((unsigned long)arkrtos_log_ring_dump(dump_put, 0),
            (unsigned long)arkrtos_log_ring_used(), "dump returns the used byte count");
    ck_eq_u((unsigned long)g_dump_calls, 1ul, "an unwrapped ring is dumped in one call");
    ck(strstr(g_dump, "R:1 first") != 0, "and the line body is present");

    /* 回卷：连发多行把环灌满，dump 必须按「最旧 → 最新」重新拼出来 */
    arkrtos_log_ring_clear();
    g_time_ms = 8000u;
    {
        int i;
        for(i = 0; i < 40; i++)
        {
            arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "W", (uint32)i, "0123456789");
        }
    }
    ck_eq_u((unsigned long)arkrtos_log_ring_used(),
            (unsigned long)arkrtos_log_ring_capacity(),
            "the ring fills up and then stays at capacity");
    g_dump_len = 0;
    g_dump[0] = '\0';
    g_dump_calls = 0;
    ck_eq_u((unsigned long)arkrtos_log_ring_dump(dump_put, 0),
            (unsigned long)arkrtos_log_ring_capacity(), "a wrapped ring dumps a full ring");
    ck_eq_u((unsigned long)g_dump_len, (unsigned long)arkrtos_log_ring_capacity(),
            "and the two pieces add up to the whole capacity");
    /* 最后一行必然完整地在末尾，最旧的那行必然已经不见了 */
    ck(strstr(g_dump, "W:39 0123456789") != 0, "the newest line is at the end");
    ck(strstr(g_dump, "W:0 0123456789") == 0, "the oldest line has been overwritten");
}

#endif /* LOG && LOG_RING */

int main(void)
{
#if (ARKRTOS_USE_LOG == 1)
    test_before_dev();
    test_open_once();
    test_levels();
    test_emit_format();
    test_emit_filter();
    test_console_lock();
    test_tx_drop();
    test_svc();
#endif

#if (ARKRTOS_USE_LOG == 1) && (ARKRTOS_USE_LOG_RING == 1)
    test_ring();
    printf("LOG-HOST OK  %d checks\n", g_checks);
#elif (ARKRTOS_USE_LOG == 1)
    /* 环被编掉：三个入口必须如实答「没有环」，而不是假装空环 */
    ck_eq_u((unsigned long)arkrtos_log_ring_capacity(), 0ul, "ring off: capacity is 0");
    ck_eq_u((unsigned long)arkrtos_log_ring_used(), 0ul, "ring off: used is 0");
    arkrtos_log_ring_clear();           /* 必须能调用，且什么也不做 */
    ck_eq_u((unsigned long)arkrtos_log_ring_dump(0, 0), 0ul, "ring off: dump answers nothing");
    printf("LOG-HOST NORING OK  %d checks\n", g_checks);
#else
    /* LOG=0：整模块是「恒拒绝」的桩。每一次回答都是「没有」 */
    ck_eq_u((unsigned long)arkrtos_log_get_level(), 0ul, "log off: level reads 0");
    arkrtos_log_set_level(3u);
    ck_eq_u((unsigned long)arkrtos_log_get_level(), 0ul, "log off: setting a level does nothing");
    arkrtos_log_init();
    arkrtos_log_emit((uint32)ARKRTOS_LOG_INFO, "A", 1u, "m");
    ck_eq_u((unsigned long)(int32)arkrtos_log_svc((uint32)ARKRTOS_LOG_INFO, "A", "m"),
            (unsigned long)ARKRTOS_LOG_ERR_DISABLED, "log off: svc answers DISABLED");
    ck_eq_u((unsigned long)(int32)arkrtos_console_handle(),
            (unsigned long)ARKRTOS_LOG_ERR_DISABLED, "log off: handle answers DISABLED");
    ck_eq_u((unsigned long)arkrtos_console_is_handle(-1), 0ul,
            "log off: no handle can be the console (user mode takes the plain device path)");
    ck_eq_u((unsigned long)(int32)arkrtos_console_write((const uint8 *)"x", 1u),
            (unsigned long)ARKRTOS_LOG_ERR_DISABLED, "log off: write answers DISABLED");
    ck_eq_u((unsigned long)(int32)arkrtos_console_lock(), 0ul,
            "log off: lock reports not-taken so callers do not unlock");
    arkrtos_console_unlock();
    ck_eq_u((unsigned long)arkrtos_console_busy_count(), 0ul, "log off: busy count is 0");
    ck_eq_u((unsigned long)arkrtos_console_tx_drop_count(), 0ul, "log off: drop count is 0");
    ck_eq_u((unsigned long)arkrtos_log_ring_capacity(), 0ul, "log off: ring capacity is 0");
    ck_eq_u((unsigned long)arkrtos_log_ring_used(), 0ul, "log off: ring used is 0");
    arkrtos_log_ring_clear();
    ck_eq_u((unsigned long)arkrtos_log_ring_dump(0, 0), 0ul, "log off: ring dump answers 0");
    printf("LOG-HOST OFF OK  %d checks\n", g_checks);
#endif

    if(g_fails != 0)
    {
        printf("FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    return 0;
}
