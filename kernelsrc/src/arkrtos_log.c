/**
* @file arkrtos_log.c
* @brief Ark RTOS 内核日志：分级输出与运行期过滤的实现
* @details 输出目标是内核控制台串口（SHELL_DEV_NAME）。这里不复用 shell 的
*          平台层句柄，而是自己惰性打开一次：日志允许在 shell 之前使用
*          （比如启动横幅），也允许在 SHELL_ENABLE=0 的裁剪配置里单独存在。
*          两次 open 拿到的是同一个设备对象，写接口串行调用，互不干扰。
*
*          级别过滤分两道：
*            - 编译期：ARKRTOS_LOG_LEVEL 决定哪些宏展开成空语句（见 arkrtos_log.h）；
*            - 运行期：本文件的 g_log_level，可由 shell 的 `log <level>` 或
*              SVC 0x19 之外的内部调用改变，全局生效（内核 + 所有 App/驱动）。
*
*          行内不做重入保护：日志只能在任务上下文调用，这是设备框架的
*          写接口本身的前提（见 arkrtos_dev.h）。
*
* @note 本模块由内核特权态调用；用户态经 SVC 0x19 进入 arkrtos_log_svc()。
*/

#include "arkrtos_config.h"
#include "arkrtos_log.h"
#include "arkrtos_dev.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"       /* arkrtos_kernel_get_time(): 日志时间戳 */
#if ARKRTOS_USE_LOG

/* Per-byte budget while the transmit pipe is full. */
/* The console device queues into a 128 byte pipe that the TXE interrupt */
/* drains, so a byte waits about 87 us at 115200 - and longer when the */
/* producers stay ahead of the line, which is the normal case for a burst. */
/* Measured on board: with a 4000 iteration budget the counters showed */
/* tx_drop=420 B alongside 1.69M full-pipe retries, i.e. the budget was */
/* still shorter than the time one byte needed.  Budget for a complete pipe */
/* drain (127 x 87 us ~= 11 ms) so a burst is never cut; the value stays */
/* bounded, so a dead port still cannot hang the caller forever.  Lower it */
/* with the `log` counters in hand, never on a guess.  Same value as the */
/* shell platform layer (SHELL_TX_SPIN_LIMIT): the shell never lost a */
/* byte while the console path used 64, and that gap was the whole */
/* difference between the two. */
#define ARKRTOS_CONSOLE_TX_WAIT     (200000u)

/* 前缀缓冲：颜色 + [级别] + [tag:行号] + 空格 */
#define ARKRTOS_LOG_HEAD_SIZE       (64)

/* 控制台句柄（-1 = 尚未打开） */
static int32  g_log_dev   = -1;

/* 运行期级别：初值取编译期配置，保证「配置里关掉的级别」不会因为
 * 忘了调用 arkrtos_log_set_level() 而在运行期被打开 */
static uint32 g_log_level = (uint32)ARKRTOS_LOG_LEVEL;

/* Console sink lock.  Held across one complete output unit so that no two
 * writers can interleave half a line.  Deliberately does NOT mask
 * interrupts: the console device is interrupt driven, so masking them
 * stops the transmit FIFO from draining and the byte loop then drops the
 * line.  A plain CAS busy flag is also the right shape here - the
 * re-entrant spinlock would silently hand the lock to a handler that
 * preempted the holder, which is exactly the case this lock exists for.
 * See arkrtos_log.h. */
static volatile uint32 g_console_busy    = 0u;
static volatile uint32 g_console_busy_ev = 0u;

/* Bytes the writer gave up on after the whole wait budget.  This is real
 * output loss, not a retry: the transmit pipe stayed full for the entire
 * budget and the byte never left the MCU. */
static volatile uint32 g_console_tx_drop = 0u;

/* Bounded spin: long enough to cover a console line, short enough that a
 * handler never looks hung. */
#define ARKRTOS_CONSOLE_SPIN_LIMIT  (400000u)


/* ============================================================
 * 输出通道
 * ============================================================ */

static int32 arkrtos_log_dev(void)
{
    if(g_log_dev < 0)
    {
        char name[] = SHELL_DEV_NAME;

        g_log_dev = arkrtos_dev_open_internal(name, (uint32)SHELL_DEV_ARG);
    }

    return g_log_dev;
}

/* Console API ------------------------------------------------------- */

int32 arkrtos_console_handle(void)
{
    return arkrtos_log_dev();
}

int32 arkrtos_console_is_handle(int32 handle)
{
    return (handle == arkrtos_log_dev()) ? 1 : 0;
}

int32 arkrtos_console_lock(void)
{
    uint32 spin = 0u;

    while(arkrtos_port_atomic_cas(&g_console_busy, 0u, 1u) == 0u)
    {
        if(++spin > ARKRTOS_CONSOLE_SPIN_LIMIT)
        {
            /* Console stayed busy for the whole budget.  Write the unit
             * anyway: output is never dropped because of contention, and
             * no context can deadlock waiting for a task that cannot run. */
            g_console_busy_ev++;
            return 0;
        }
    }

    ARKRTOS_DMB();
    return 1;
}

void arkrtos_console_unlock(void)
{
    ARKRTOS_DMB();
    g_console_busy = 0u;
}

uint32 arkrtos_console_busy_count(void)
{
    return g_console_busy_ev;
}

uint32 arkrtos_console_tx_drop_count(void)
{
    return g_console_tx_drop;
}

int32 arkrtos_console_write(const uint8 *p_data, uint32 len)
{
    int32  dev = arkrtos_log_dev();
    int32  held;
    uint32 i;

    if((p_data == 0) || (len == 0u))
    {
        return 0;
    }

    if(dev < 0)
    {
        return -1;
    }

    held = arkrtos_console_lock();
    for(i = 0u; i < len; i++)
    {
        uint32 spin = 0u;
        int32  queued = 0;

        do
        {
            if(arkrtos_dev_write_internal(dev, (uint8 *)&p_data[i], 1) == 1)
            {
                queued = 1;
                break;
            }
            spin++;
        } while(spin < ARKRTOS_CONSOLE_TX_WAIT);

        if(queued == 0)
        {
            g_console_tx_drop++;
        }
    }
    if(held != 0)
    {
        arkrtos_console_unlock();
    }

    return (int32)len;
}

/* 逐字节写：板级发送管道按字节计账，写满即短写，整串一次写会写不进去
 * （管道已放大到 1024 B，但逐字节 + 自旋才是这里的契约） */
/* Raw byte loop.  Callers must hold the console lock; arkrtos_log_emit()
 * takes it once for the whole line. */
static void arkrtos_log_puts_raw(const char *s)
{
    int32 dev = arkrtos_log_dev();

    if((s == 0) || (dev < 0))
    {
        return;
    }

    while(*s != '\0')
    {
        uint32 spin = 0u;
        int32  queued = 0;

        do
        {
            if(arkrtos_dev_write_internal(dev, (uint8 *)s, 1) == 1)
            {
                queued = 1;
                break;
            }
            spin++;
        } while(spin < ARKRTOS_CONSOLE_TX_WAIT);

        if(queued == 0)
        {
            g_console_tx_drop++;
        }
        s++;
    }
}

/* ============================================================
 * 级别辅助
 * ============================================================ */

/* syslog 优先级：把本模块的级别数值映射到 Linux dmesg 的 <n>。
 * 3=err 4=warning 6=info 7=debug，与 Linux 内核日志取值一致。 */
static uint32 arkrtos_log_pri(uint32 level)
{
    switch(level)
    {
        case ARKRTOS_LOG_ERROR:   return 3u;
        case ARKRTOS_LOG_WARNING: return 4u;
        case ARKRTOS_LOG_INFO:    return 6u;
        case ARKRTOS_LOG_DEBUG:   return 7u;
        default:                  return 7u;
    }
}

static const char *arkrtos_log_level_color(uint32 level)
{
    switch(level)
    {
        case ARKRTOS_LOG_ERROR:   return ARKRTOS_LOG_COLOR_ERROR;
        case ARKRTOS_LOG_WARNING: return ARKRTOS_LOG_COLOR_WARNING;
        case ARKRTOS_LOG_INFO:    return ARKRTOS_LOG_COLOR_INFO;
        case ARKRTOS_LOG_DEBUG:   return ARKRTOS_LOG_COLOR_DEBUG;
        default:                return ARKRTOS_LOG_COLOR_RESET;
    }
}

/* ============================================================
 * dmesg 日志环（ARKRTOS_USE_LOG_RING）
 *
 * 存整行格式化日志（含前缀与行尾），满时覆盖最旧。写侧在 emit 里与
 * 控制台输出共用同一把 console 锁，读侧（shell dmesg）在主循环上下文，
 * 单核下读写不交错。
 * ============================================================ */
#if ARKRTOS_USE_LOG_RING
static char   g_ring[ARKRTOS_LOG_RING_SIZE];
static uint32 g_ring_head = 0u;                 /* 下一个写入位置 */
static uint32 g_ring_used = 0u;                 /* 有效字节数 */

static void arkrtos_log_ring_put(const char *s)
{
    uint32 i;

    for(i = 0u; s[i] != '\0'; i++)
    {
        g_ring[g_ring_head] = s[i];
        g_ring_head++;
        if(g_ring_head >= (uint32)sizeof(g_ring))
        {
            g_ring_head = 0u;
        }
        if(g_ring_used < (uint32)sizeof(g_ring))
        {
            g_ring_used++;
        }
    }
}

uint32 arkrtos_log_ring_capacity(void)
{
    return (uint32)sizeof(g_ring);
}

uint32 arkrtos_log_ring_used(void)
{
    return g_ring_used;
}

void arkrtos_log_ring_clear(void)
{
    g_ring_head = 0u;
    g_ring_used = 0u;
}

uint32 arkrtos_log_ring_dump(void (*put)(const char *data, uint32 len, void *arg), void *arg)
{
    uint32 start;
    uint32 first;

    if((put == 0) || (g_ring_used == 0u))
    {
        return 0u;
    }

    if(g_ring_used < (uint32)sizeof(g_ring))
    {
        /* 未回卷：从 0 开始的连续一段 */
        put(&g_ring[0], g_ring_used, arg);
    }
    else
    {
        /* 已回卷：最旧 = head，先到环尾，再从 0 到 head */
        start = g_ring_head;
        first = (uint32)sizeof(g_ring) - start;
        put(&g_ring[start], first, arg);
        if(start != 0u)
        {
            put(&g_ring[0], start, arg);
        }
    }

    return g_ring_used;
}
#else
uint32 arkrtos_log_ring_capacity(void) { return 0u; }
uint32 arkrtos_log_ring_used(void)     { return 0u; }
void   arkrtos_log_ring_clear(void)    { }
uint32 arkrtos_log_ring_dump(void (*put)(const char *data, uint32 len, void *arg), void *arg)
{
    (void)put; (void)arg; return 0u;
}
#endif /* ARKRTOS_USE_LOG_RING */

/* ============================================================
 * 对外接口
 * ============================================================ */

void arkrtos_log_init(void)
{
    g_log_level = (uint32)ARKRTOS_LOG_LEVEL;

    (void)arkrtos_log_dev();
}

void arkrtos_log_set_level(uint32 level)
{
    if(level > (uint32)ARKRTOS_LOG_DEBUG)
    {
        return;                 /* 非法级别不改变现状，避免误配后全体静音 */
    }

    g_log_level = level;
}

uint32 arkrtos_log_get_level(void)
{
    return g_log_level;
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    char   head[ARKRTOS_LOG_HEAD_SIZE];
    uint32 ms;
    uint32 sec;
    uint32 frac;
    uint32 pri;
    int32  held;

    if(msg == 0)
    {
        return;
    }

    /* 级别过滤：NONE 与高于当前级别的日志直接丢弃 */
    if((level == ARKRTOS_LOG_NONE) || (level > g_log_level) ||
       (level > (uint32)ARKRTOS_LOG_DEBUG))
    {
        return;
    }

    if(tag == 0)
    {
        tag = "?";
    }

    /* 时间戳取系统运行时间（ms），形如 [    1.234]，与 Linux dmesg 对齐 */
    ms   = arkrtos_kernel_get_time();
    sec  = ms / 1000u;
    frac = ms % 1000u;
    pri  = arkrtos_log_pri(level);

    /* dmesg 风格：<颜色>[运行时间] <syslog 优先级><复位>tag:行号 正文
     * 颜色只覆盖时间戳+优先级这段前缀，正文与 tag:行号 不着色。 */
    if(line != 0u)
    {
        (void)snprintf(head, sizeof(head), "%s[%5u.%03u] <%u>%s%s:%u ",
                       arkrtos_log_level_color(level),
                       (unsigned)sec, (unsigned)frac, (unsigned)pri,
                       ARKRTOS_LOG_COLOR_RESET,
                       tag, (unsigned)line);
    }
    else
    {
        (void)snprintf(head, sizeof(head), "%s[%5u.%03u] <%u>%s%s ",
                       arkrtos_log_level_color(level),
                       (unsigned)sec, (unsigned)frac, (unsigned)pri,
                       ARKRTOS_LOG_COLOR_RESET,
                       tag);
    }

    /* One output unit = one lock.  Nothing can be spliced into the
     * middle of a log line.  The dmesg ring is written under the same
     * lock so a reader never sees a half line. */
    held = arkrtos_console_lock();
    arkrtos_log_puts_raw(head);
    arkrtos_log_puts_raw(msg);
    arkrtos_log_puts_raw("\r\n");
#if ARKRTOS_USE_LOG_RING
    arkrtos_log_ring_put(head);
    arkrtos_log_ring_put(msg);
    arkrtos_log_ring_put("\r\n");
#endif
    if(held != 0)
    {
        arkrtos_console_unlock();
    }
}

int32 arkrtos_log_svc(uint32 level, const char *tag, const char *msg)
{
    /* 用户态只能调高到 DEBUG 为止；越界按 DEBUG 处理而不是拒绝：
     * 日志接口不值得让调用者去处理错误码。 */
    if(level > (uint32)ARKRTOS_LOG_DEBUG)
    {
        level = (uint32)ARKRTOS_LOG_DEBUG;
    }

    if(msg == 0)
    {
        return -1;
    }

    arkrtos_log_emit(level, tag, 0u, msg);
    return 0;
}

#else   /* ARKRTOS_USE_LOG == 0 */
/* Kernel log compiled out.  The entry points stay so that every caller -
 * the tick, the SVC dispatcher, the device write path, the config module -
 * keeps compiling, and each one answers "off" rather than pretending the
 * line went somewhere.  arkrtos_console_is_handle() returning 0 is what sends
 * a user mode write down the plain device path instead of the console
 * shortcut, which is the honest routing when there is no console. */

void arkrtos_log_init(void)
{
}

void arkrtos_log_set_level(uint32 level)
{
    (void)level;
}

uint32 arkrtos_log_get_level(void)
{
    return 0u;
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)level;
    (void)tag;
    (void)line;
    (void)msg;
}

int32 arkrtos_log_svc(uint32 level, const char *tag, const char *msg)
{
    (void)level;
    (void)tag;
    (void)msg;
    return -1;
}

int32 arkrtos_console_handle(void)
{
    return -1;
}

int32 arkrtos_console_is_handle(int32 handle)
{
    (void)handle;
    return 0;
}

int32 arkrtos_console_write(const uint8 *p_data, uint32 len)
{
    (void)p_data;
    (void)len;
    return -1;
}

uint32 arkrtos_console_tx_drop_count(void)
{
    return 0u;
}

/* 0 = "the lock was not taken", so a caller that checks the return value
 * does not go on to unlock something that was never held. */
int32 arkrtos_console_lock(void)
{
    return 0;
}

void arkrtos_console_unlock(void)
{
}

uint32 arkrtos_console_busy_count(void)
{
    return 0u;
}

/* dmesg ring compiled out with the log.  capacity() answers 0 so the shell
 * `dmesg` command can say the ring is unavailable rather than print nothing. */
uint32 arkrtos_log_ring_capacity(void) { return 0u; }
uint32 arkrtos_log_ring_used(void)     { return 0u; }
void   arkrtos_log_ring_clear(void)    { }
uint32 arkrtos_log_ring_dump(void (*put)(const char *data, uint32 len, void *arg), void *arg)
{
    (void)put; (void)arg; return 0u;
}
#endif /* ARKRTOS_USE_LOG */
