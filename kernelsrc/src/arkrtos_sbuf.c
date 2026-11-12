/**
* @brief 流缓冲 / 消息缓冲实现
* @details 单写者 / 单读者字节流，以及建在它上面的变长消息。
*
*          并发模型与 arkrtos_mq 一致：
*            - 状态变更一律在"关中断"临界区内完成；
*            - 没空间 / 没数据时把当前任务挂进等待表，再在同一个临界区里
*              调用 arkrtos_task_block_in_critical()（先挂表后睡，否则 ISR
*              的唤醒会投给一个还没睡下的任务 = 唤醒丢失）；
*            - 被唤醒不等于拿到资源（另一个读者可能先取走了），所以醒来后
*              按剩余超时重新检查/重睡，而不是直接返回成功。
*
*          为什么 send 可能"部分成功"：流语义下写者拿到多少空间就写多少，
*          返回实际字节数，调用方据此推进偏移；这与 FreeRTOS 的
*          xStreamBufferSend 一致。消息语义则相反——整条原子，要么全进要么不进，
*          否则读者会读到半条帧。
*
*          环形区用 rd/wt 单调索引 + 手工回绕：缓冲大小不必是 2 的幂，
*          所以不能用位与，只能在越过末尾时减去 size。
*/

#include "arkrtos_sbuf.h"
#include "arkrtos_cfg.h"
#include "arkrtos_hal.h"
#include "arkrtos_trace.h"

#if (ARKRTOS_USE_SBUF == 1)

#define ARKRTOS_SBUF_SIZE  ((uint32)ARKRTOS_SBUF_BUF_SIZE)
#define ARKRTOS_MBUF_HDR   ((uint32)ARKRTOS_MBUF_HDR_BYTES)

/* 编译期约束：长度头必须能装下整块缓冲的长度，且缓冲本身不能是 0 */
#if (ARKRTOS_SBUF_BUF_SIZE < 16)
#error "ARKRTOS_SBUF_BUF_SIZE must be at least 16 bytes"
#endif
#if (ARKRTOS_MBUF_HDR_BYTES < 2) || (ARKRTOS_MBUF_HDR_BYTES > 4)
#error "ARKRTOS_MBUF_HDR_BYTES must be 2..4"
#endif

static arkrtos_sbuf_obj_t arkrtos_sbufs[ARKRTOS_SBUF_NUM];

/* ------------------------------------------------------------------
 * 对象表
 * ------------------------------------------------------------------ */
void arkrtos_sbuf_module_init(void)
{
    int32 i, j;

    for(i = 0; i < ARKRTOS_SBUF_NUM; i++)
    {
        arkrtos_sbufs[i].name[0]    = 0;
        arkrtos_sbufs[i].used       = 0;
        arkrtos_sbufs[i].creator_id = 0;
        arkrtos_sbufs[i].wt         = 0;
        arkrtos_sbufs[i].rd         = 0;
        arkrtos_sbufs[i].count      = 0;
        for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
        {
            arkrtos_sbufs[i].send_waiters[j] = 0;
            arkrtos_sbufs[i].recv_waiters[j] = 0;
        }
    }
}

static void arkrtos_sbuf_copy_name(char *dst, char *src)
{
    int32 i;

    for(i = 0; i < 7; i++)
    {
        dst[i] = src[i];
        if(src[i] == 0)
        {
            break;
        }
    }
    dst[7] = 0;
}

static void arkrtos_sbuf_clear_waiters(arkrtos_sbuf_obj_t *p_sb)
{
    int32 j;

    for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
    {
        p_sb->send_waiters[j] = 0;
        p_sb->recv_waiters[j] = 0;
    }
}

static int32 arkrtos_sbuf_alloc(char *name, int32 creator_id)
{
    int32 i;
    int32 handle = -1;

    ARKRTOS_DISABLE_IRQ();
    for(i = 0; i < ARKRTOS_SBUF_NUM; i++)
    {
        if(arkrtos_sbufs[i].used == 0)
        {
            arkrtos_sbuf_copy_name(arkrtos_sbufs[i].name, name);
            arkrtos_sbufs[i].used       = 1;
            arkrtos_sbufs[i].creator_id = creator_id;
            arkrtos_sbufs[i].wt         = 0;
            arkrtos_sbufs[i].rd         = 0;
            arkrtos_sbufs[i].count      = 0;
            arkrtos_sbuf_clear_waiters(&arkrtos_sbufs[i]);
            handle = i | ARKRTOS_SBUF_HANDLE_FLAG;
            break;
        }
    }
    ARKRTOS_ENABLE_IRQ();
    return handle;
}

int32 arkrtos_sbuf_create_internal(char *name)
{
    if(name == 0)
    {
        return -1;
    }
    return arkrtos_sbuf_alloc(name, (arkrtos_current_task_id > 0)
                                    ? arkrtos_current_task_id : 0);
}

int32 arkrtos_sbuf_create_kernel_internal(char *name)
{
    if(name == 0)
    {
        return -1;
    }
    return arkrtos_sbuf_alloc(name, 0);
}

/* 句柄解析：返回值是对象指针，0 表示句柄无效。
 * 三件事都要查——标志位、范围、占用位；只查范围会让"上一个 App 的旧句柄"
 * 在槽位被新对象复用后指向别人的缓冲。 */
static arkrtos_sbuf_obj_t *arkrtos_sbuf_get(int32 handle)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;

    if((handle & ARKRTOS_HANDLE_MASK) != ARKRTOS_SBUF_HANDLE_FLAG)
    {
        return 0;
    }
    if(idx < 0 || idx >= ARKRTOS_SBUF_NUM)
    {
        return 0;
    }
    if(arkrtos_sbufs[idx].used == 0)
    {
        return 0;
    }
    return &arkrtos_sbufs[idx];
}

int32 arkrtos_sbuf_is_alive(int32 handle)
{
    return (arkrtos_sbuf_get(handle) != 0) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * 等待表：与 mq 同一套规则（一任务一槽、重复登记不算错误）
 * ------------------------------------------------------------------ */
static int32 arkrtos_sbuf_waiter_add(arkrtos_task_t **waiters, arkrtos_task_t *p_tsk)
{
    int32 j;

    for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            return -2;              /* 已经在表里，等价于登记成功 */
        }
    }
    for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == 0)
        {
            waiters[j] = p_tsk;
            return 0;
        }
    }
    return -1;                      /* 表满 */
}

static void arkrtos_sbuf_waiter_remove(arkrtos_task_t **waiters, arkrtos_task_t *p_tsk)
{
    int32 j;

    for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] == p_tsk)
        {
            waiters[j] = 0;
        }
    }
}

/* 只唤醒一个：流/消息缓冲是单读者单写者模型，多唤只会让他们互相抢、
 * 抢不到的那个白白跑一趟。 */
static void arkrtos_sbuf_wake_one(arkrtos_task_t **waiters)
{
    int32 j;

    for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            if(waiters[j]->status == ARKRTOS_TASK_INVALID)
            {
                waiters[j] = 0;
                continue;
            }
            waiters[j]->wait_time   = 0;
            waiters[j]->wake_reason = ARKRTOS_WAKE_HANDOFF;
            waiters[j]->status      = ARKRTOS_TASK_READY;
            arkrtos_ready_add(ARKRTOS_TASK_IDX(waiters[j]));
            waiters[j] = 0;
            return;
        }
    }
}

static void arkrtos_sbuf_wake_all(arkrtos_task_t **waiters, int32 reason)
{
    int32 j;

    for(j = 0; j < ARKRTOS_MAX_SYNC_WAITERS; j++)
    {
        if(waiters[j] != 0)
        {
            if(waiters[j]->status != ARKRTOS_TASK_INVALID)
            {
                waiters[j]->wait_time   = 0;
                waiters[j]->wake_reason = reason;
                waiters[j]->status      = ARKRTOS_TASK_READY;
                arkrtos_ready_add(ARKRTOS_TASK_IDX(waiters[j]));
            }
            waiters[j] = 0;
        }
    }
}

void arkrtos_sbuf_release_task(int32 task_id)
{
    arkrtos_task_t *p_tsk;
    int32 i;

    if(task_id <= 0 || task_id > arkrtos_task_count)
    {
        return;
    }

    p_tsk = &arkrtos_task_table[task_id - 1];

    for(i = 0; i < ARKRTOS_SBUF_NUM; i++)
    {
        if(arkrtos_sbufs[i].used == 0)
        {
            continue;
        }

        arkrtos_sbuf_waiter_remove(arkrtos_sbufs[i].send_waiters, p_tsk);
        arkrtos_sbuf_waiter_remove(arkrtos_sbufs[i].recv_waiters, p_tsk);

        /* 自己创建的对象随任务一起消失：对象表很小，反复安装/卸载会把它耗干。
         * 内核自建（creator_id == 0）的留下。这里在任务拆除路径上跑，
         * 只摘登记、不重新排队。 */
        if(arkrtos_sbufs[i].creator_id == task_id)
        {
            arkrtos_sbuf_clear_waiters(&arkrtos_sbufs[i]);
            arkrtos_sbufs[i].used       = 0;
            arkrtos_sbufs[i].name[0]    = 0;
            arkrtos_sbufs[i].creator_id = 0;
            arkrtos_sbufs[i].wt         = 0;
            arkrtos_sbufs[i].rd         = 0;
            arkrtos_sbufs[i].count      = 0;
        }
    }
}

/* ------------------------------------------------------------------
 * 环形缓冲字节搬运（调用方已持临界区）
 * ------------------------------------------------------------------ */
static uint32 arkrtos_sbuf_room(const arkrtos_sbuf_obj_t *p_sb)
{
    return ARKRTOS_SBUF_SIZE - p_sb->count;
}

static void arkrtos_sbuf_put_bytes(arkrtos_sbuf_obj_t *p_sb,
                                   const uint8 *src, uint32 len)
{
    uint32 first;

    if(len == 0u)
    {
        return;
    }

    first = ARKRTOS_SBUF_SIZE - p_sb->wt;
    if(first > len)
    {
        first = len;
    }

    {
        uint32 i;
        for(i = 0; i < first; i++)
        {
            p_sb->data[p_sb->wt + i] = src[i];
        }
    }
    p_sb->wt += first;
    if(p_sb->wt >= ARKRTOS_SBUF_SIZE)
    {
        p_sb->wt -= ARKRTOS_SBUF_SIZE;
    }

    {
        uint32 rest = len - first;
        uint32 i;
        for(i = 0; i < rest; i++)
        {
            p_sb->data[p_sb->wt + i] = src[first + i];
        }
        p_sb->wt += rest;
        if(p_sb->wt >= ARKRTOS_SBUF_SIZE)
        {
            p_sb->wt -= ARKRTOS_SBUF_SIZE;
        }
    }

    p_sb->count += len;
}

static void arkrtos_sbuf_get_bytes(arkrtos_sbuf_obj_t *p_sb,
                                   uint8 *dst, uint32 len)
{
    uint32 first;

    if(len == 0u)
    {
        return;
    }

    first = ARKRTOS_SBUF_SIZE - p_sb->rd;
    if(first > len)
    {
        first = len;
    }

    {
        uint32 i;
        for(i = 0; i < first; i++)
        {
            dst[i] = p_sb->data[p_sb->rd + i];
        }
    }
    p_sb->rd += first;
    if(p_sb->rd >= ARKRTOS_SBUF_SIZE)
    {
        p_sb->rd -= ARKRTOS_SBUF_SIZE;
    }

    {
        uint32 rest = len - first;
        uint32 i;
        for(i = 0; i < rest; i++)
        {
            dst[first + i] = p_sb->data[p_sb->rd + i];
        }
        p_sb->rd += rest;
        if(p_sb->rd >= ARKRTOS_SBUF_SIZE)
        {
            p_sb->rd -= ARKRTOS_SBUF_SIZE;
        }
    }

    p_sb->count -= len;
}

/* 从环形区读 offset 处的 n 个字节但不消费（只看不取，用于读长度头） */
static void arkrtos_sbuf_peek_at(const arkrtos_sbuf_obj_t *p_sb,
                                 uint32 offset, uint8 *dst, uint32 n)
{
    uint32 pos = p_sb->rd + offset;
    uint32 i;

    if(pos >= ARKRTOS_SBUF_SIZE)
    {
        pos -= ARKRTOS_SBUF_SIZE;
    }

    for(i = 0; i < n; i++)
    {
        dst[i] = p_sb->data[pos];
        pos++;
        if(pos >= ARKRTOS_SBUF_SIZE)
        {
            pos = 0;
        }
    }
}

/* 超时换算：deadline == 0 表示无限等待 */
static int32 arkrtos_sbuf_remain_ms(uint32 start_tick, uint32 deadline)
{
    uint32 elapsed;

    if(deadline == 0u)
    {
        return -1;
    }

    elapsed = arkrtos_kernel_tick - start_tick;
    if(elapsed >= deadline)
    {
        return 0;
    }

    return (int32)((deadline - elapsed) * ARKRTOS_TICK_PERIOD_US / 1000u);
}

/* ------------------------------------------------------------------
 * 流语义
 * ------------------------------------------------------------------ */
int32 arkrtos_sbuf_send_internal(int32 handle, const void *buf, int32 len, int32 timeout_ms)
{
    arkrtos_sbuf_obj_t *p_sb;
    arkrtos_task_t *p_tsk;
    uint32 start_tick;
    uint32 deadline;
    int32 remain;
    int32 ret;
    int32 written = 0;

    if(buf == 0 || len <= 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    p_sb = arkrtos_sbuf_get(handle);
    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    if((uint32)len > ARKRTOS_SBUF_SIZE)
    {
        /* 比整块缓冲还大：写进去也永远读不全，这是参数错误而不是"等一等" */
        return ARKRTOS_SBUF_ERR_NOSPC;
    }

    ARKRTOS_DISABLE_IRQ();

    /* 有地方就先把能写的写掉（部分成功也算成功） */
    {
        uint32 room = arkrtos_sbuf_room(p_sb);

        if(room > 0u)
        {
            uint32 n = ((uint32)len < room) ? (uint32)len : room;

            arkrtos_sbuf_put_bytes(p_sb, (const uint8 *)buf, n);
            arkrtos_sbuf_wake_one(p_sb->recv_waiters);
            mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                         (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
            ARKRTOS_ENABLE_IRQ();
            ARKRTOS_SWITCH_TASK();
            return (int32)n;
        }
    }

    if(timeout_ms == 0)
    {
        /* 不等待：顺手清掉自己在用户态轮询里留下的旧登记 */
        p_tsk = arkrtos_task_get_current();
        if(p_tsk != 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->send_waiters, p_tsk);
        }
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_TIMEOUT;
    }

    p_tsk = arkrtos_task_get_current();
    if(p_tsk == 0)
    {
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_PARAM;   /* 内核态/调度器未启动，没有可阻塞的任务 */
    }

    start_tick = arkrtos_kernel_tick;
    deadline   = (timeout_ms > 0) ? ARKRTOS_MS_TO_TICK((uint32)timeout_ms) : 0u;

    for(;;)
    {
        if(arkrtos_sbuf_room(p_sb) == 0u)
        {
            ret = arkrtos_sbuf_waiter_add(p_sb->send_waiters, p_tsk);
            if((ret < 0) && (ret != -2))
            {
                ARKRTOS_ENABLE_IRQ();
                return ARKRTOS_SBUF_ERR_PARAM;   /* 等待表满 */
            }
        }

        /* 用户态等待者只登记，真正让出 CPU 由用户态包装里的轮询做 */
        if(arkrtos_sync_in_handler() != 0u)
        {
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SYNC_ERR_WOULDBLOCK;
        }

        remain = arkrtos_sbuf_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->send_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SBUF_ERR_TIMEOUT;
        }

        arkrtos_trace_wait_obj(MDK_TRACE_ARKRTOS_OBJ(MDK_TRACE_ARKRTOS_CLASS_QUEUE,
                                                     (handle & ARKRTOS_HANDLE_RELMASK)));
        ret = arkrtos_task_block_in_critical((remain < 0) ? 0u : (uint32)remain);

        if((ret & ARKRTOS_WAKE_HANDOFF) == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->send_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            if(ret == ARKRTOS_WAKE_TIMEOUT)
            {
                return ARKRTOS_SBUF_ERR_TIMEOUT;
            }
            return ARKRTOS_SBUF_ERR_PARAM;   /* 对象被删 / 未能阻塞 */
        }

        if(arkrtos_sbuf_room(p_sb) > 0u)
        {
            uint32 room = arkrtos_sbuf_room(p_sb);
            uint32 n = ((uint32)len < room) ? (uint32)len : room;

            arkrtos_sbuf_put_bytes(p_sb, (const uint8 *)buf, n);
            arkrtos_sbuf_wake_one(p_sb->recv_waiters);
            mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                         (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
            ARKRTOS_ENABLE_IRQ();
            ARKRTOS_SWITCH_TASK();
            return (int32)n;
        }
        /* 被唤醒但空间又被别人占走：按剩余超时重睡，不返回假成功 */
    }
}

int32 arkrtos_sbuf_recv_internal(int32 handle, void *buf, int32 len, int32 timeout_ms)
{
    arkrtos_sbuf_obj_t *p_sb;
    arkrtos_task_t *p_tsk;
    uint32 start_tick;
    uint32 deadline;
    int32 remain;
    int32 ret;

    if(buf == 0 || len <= 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    p_sb = arkrtos_sbuf_get(handle);
    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    ARKRTOS_DISABLE_IRQ();

    if(p_sb->count > 0u)
    {
        uint32 n = (p_sb->count < (uint32)len) ? p_sb->count : (uint32)len;

        arkrtos_sbuf_get_bytes(p_sb, (uint8 *)buf, n);
        arkrtos_sbuf_wake_one(p_sb->send_waiters);
        mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                     (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
        ARKRTOS_ENABLE_IRQ();
        ARKRTOS_SWITCH_TASK();
        return (int32)n;
    }

    if(timeout_ms == 0)
    {
        p_tsk = arkrtos_task_get_current();
        if(p_tsk != 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->recv_waiters, p_tsk);
        }
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_TIMEOUT;
    }

    p_tsk = arkrtos_task_get_current();
    if(p_tsk == 0)
    {
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    start_tick = arkrtos_kernel_tick;
    deadline   = (timeout_ms > 0) ? ARKRTOS_MS_TO_TICK((uint32)timeout_ms) : 0u;

    for(;;)
    {
        if(p_sb->count == 0u)
        {
            ret = arkrtos_sbuf_waiter_add(p_sb->recv_waiters, p_tsk);
            if((ret < 0) && (ret != -2))
            {
                ARKRTOS_ENABLE_IRQ();
                return ARKRTOS_SBUF_ERR_PARAM;
            }
        }

        if(arkrtos_sync_in_handler() != 0u)
        {
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SYNC_ERR_WOULDBLOCK;
        }

        remain = arkrtos_sbuf_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->recv_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SBUF_ERR_TIMEOUT;
        }

        arkrtos_trace_wait_obj(MDK_TRACE_ARKRTOS_OBJ(MDK_TRACE_ARKRTOS_CLASS_QUEUE,
                                                     (handle & ARKRTOS_HANDLE_RELMASK)));
        ret = arkrtos_task_block_in_critical((remain < 0) ? 0u : (uint32)remain);

        if((ret & ARKRTOS_WAKE_HANDOFF) == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->recv_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            if(ret == ARKRTOS_WAKE_TIMEOUT)
            {
                return ARKRTOS_SBUF_ERR_TIMEOUT;
            }
            return ARKRTOS_SBUF_ERR_PARAM;
        }

        if(p_sb->count > 0u)
        {
            uint32 n = (p_sb->count < (uint32)len) ? p_sb->count : (uint32)len;

            arkrtos_sbuf_get_bytes(p_sb, (uint8 *)buf, n);
            arkrtos_sbuf_wake_one(p_sb->send_waiters);
            mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                         (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
            ARKRTOS_ENABLE_IRQ();
            ARKRTOS_SWITCH_TASK();
            return (int32)n;
        }
    }
}

int32 arkrtos_sbuf_send_from_isr_internal(int32 handle, const void *buf, int32 len)
{
    arkrtos_sbuf_obj_t *p_sb;
    uint32 room;
    uint32 n;
    uint32 state;

    if(buf == 0 || len <= 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    p_sb = arkrtos_sbuf_get(handle);
    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    state = ARKRTOS_ENTER_CRITICAL();
    room = arkrtos_sbuf_room(p_sb);
    if(room == 0u)
    {
        ARKRTOS_EXIT_CRITICAL(state);
        return ARKRTOS_SBUF_ERR_TIMEOUT;      /* 满了就是满了：ISR 里不能等 */
    }
    n = ((uint32)len < room) ? (uint32)len : room;
    arkrtos_sbuf_put_bytes(p_sb, (const uint8 *)buf, n);
    arkrtos_sbuf_wake_one(p_sb->recv_waiters);
    ARKRTOS_EXIT_CRITICAL(state);
    return (int32)n;
}

int32 arkrtos_sbuf_used_internal(int32 handle)
{
    arkrtos_sbuf_obj_t *p_sb = arkrtos_sbuf_get(handle);

    return (p_sb != 0) ? (int32)p_sb->count : ARKRTOS_SBUF_ERR_PARAM;
}

int32 arkrtos_sbuf_free_internal(int32 handle)
{
    arkrtos_sbuf_obj_t *p_sb = arkrtos_sbuf_get(handle);

    return (p_sb != 0) ? (int32)arkrtos_sbuf_room(p_sb) : ARKRTOS_SBUF_ERR_PARAM;
}

int32 arkrtos_sbuf_reset_internal(int32 handle)
{
    arkrtos_sbuf_obj_t *p_sb = arkrtos_sbuf_get(handle);

    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    ARKRTOS_DISABLE_IRQ();
    p_sb->wt    = 0;
    p_sb->rd    = 0;
    p_sb->count = 0;
    /* 清空是"有空间了"：写者可以继续工作 */
    arkrtos_sbuf_wake_one(p_sb->send_waiters);
    ARKRTOS_ENABLE_IRQ();
    ARKRTOS_SWITCH_TASK();
    return ARKRTOS_SBUF_OK;
}

int32 arkrtos_sbuf_delete_internal(int32 handle)
{
    arkrtos_sbuf_obj_t *p_sb = arkrtos_sbuf_get(handle);

    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    ARKRTOS_DISABLE_IRQ();
    /* 等待者必须收到"对象没了"，否则会睡到自己的超时、或被别人的唤醒误认为拿到了数据 */
    arkrtos_sbuf_wake_all(p_sb->send_waiters, ARKRTOS_WAKE_OBJ_DELETED);
    arkrtos_sbuf_wake_all(p_sb->recv_waiters, ARKRTOS_WAKE_OBJ_DELETED);
    p_sb->used       = 0;
    p_sb->name[0]    = 0;
    p_sb->creator_id = 0;
    p_sb->wt         = 0;
    p_sb->rd         = 0;
    p_sb->count      = 0;
    ARKRTOS_ENABLE_IRQ();
    ARKRTOS_SWITCH_TASK();
    return ARKRTOS_SBUF_OK;
}

/* ------------------------------------------------------------------
 * 消息语义：长度头 + 载荷，整条原子
 * ------------------------------------------------------------------ */
static void arkrtos_mbuf_put_hdr(uint8 *dst, uint32 len)
{
    int32 i;

    for(i = 0; i < (int32)ARKRTOS_MBUF_HDR; i++)
    {
        dst[i] = (uint8)((len >> (8 * i)) & 0xffu);
    }
}

static uint32 arkrtos_mbuf_get_hdr(const uint8 *src)
{
    uint32 len = 0u;
    int32 i;

    for(i = 0; i < (int32)ARKRTOS_MBUF_HDR; i++)
    {
        len |= ((uint32)src[i]) << (8 * i);
    }
    return len;
}

int32 arkrtos_mbuf_send_internal(int32 handle, const void *buf, int32 len, int32 timeout_ms)
{
    arkrtos_sbuf_obj_t *p_sb;
    arkrtos_task_t *p_tsk;
    uint32 need;
    uint32 start_tick;
    uint32 deadline;
    int32 remain;
    int32 ret;
    uint8 hdr[ARKRTOS_MBUF_HDR_BYTES];

    if(buf == 0 || len <= 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    p_sb = arkrtos_sbuf_get(handle);
    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    need = (uint32)len + ARKRTOS_MBUF_HDR;
    if(need > ARKRTOS_SBUF_SIZE)
    {
        /* 消息比整块缓冲还大：等多久都放不下，立刻报错而不是无限重试 */
        return ARKRTOS_SBUF_ERR_NOSPC;
    }

    arkrtos_mbuf_put_hdr(hdr, (uint32)len);

    ARKRTOS_DISABLE_IRQ();
    if(arkrtos_sbuf_room(p_sb) >= need)
    {
        arkrtos_sbuf_put_bytes(p_sb, hdr, ARKRTOS_MBUF_HDR);
        arkrtos_sbuf_put_bytes(p_sb, (const uint8 *)buf, (uint32)len);
        arkrtos_sbuf_wake_one(p_sb->recv_waiters);
        mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                     (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
        ARKRTOS_ENABLE_IRQ();
        ARKRTOS_SWITCH_TASK();
        return ARKRTOS_SBUF_OK;
    }

    if(timeout_ms == 0)
    {
        p_tsk = arkrtos_task_get_current();
        if(p_tsk != 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->send_waiters, p_tsk);
        }
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_TIMEOUT;
    }

    p_tsk = arkrtos_task_get_current();
    if(p_tsk == 0)
    {
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    start_tick = arkrtos_kernel_tick;
    deadline   = (timeout_ms > 0) ? ARKRTOS_MS_TO_TICK((uint32)timeout_ms) : 0u;

    for(;;)
    {
        if(arkrtos_sbuf_room(p_sb) < need)
        {
            ret = arkrtos_sbuf_waiter_add(p_sb->send_waiters, p_tsk);
            if((ret < 0) && (ret != -2))
            {
                ARKRTOS_ENABLE_IRQ();
                return ARKRTOS_SBUF_ERR_PARAM;
            }
        }

        if(arkrtos_sync_in_handler() != 0u)
        {
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SYNC_ERR_WOULDBLOCK;
        }

        remain = arkrtos_sbuf_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->send_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SBUF_ERR_TIMEOUT;
        }

        arkrtos_trace_wait_obj(MDK_TRACE_ARKRTOS_OBJ(MDK_TRACE_ARKRTOS_CLASS_QUEUE,
                                                     (handle & ARKRTOS_HANDLE_RELMASK)));
        ret = arkrtos_task_block_in_critical((remain < 0) ? 0u : (uint32)remain);

        if((ret & ARKRTOS_WAKE_HANDOFF) == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->send_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            if(ret == ARKRTOS_WAKE_TIMEOUT)
            {
                return ARKRTOS_SBUF_ERR_TIMEOUT;
            }
            return ARKRTOS_SBUF_ERR_PARAM;
        }

        if(arkrtos_sbuf_room(p_sb) >= need)
        {
            arkrtos_sbuf_put_bytes(p_sb, hdr, ARKRTOS_MBUF_HDR);
            arkrtos_sbuf_put_bytes(p_sb, (const uint8 *)buf, (uint32)len);
            arkrtos_sbuf_wake_one(p_sb->recv_waiters);
            mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                         (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
            ARKRTOS_ENABLE_IRQ();
            ARKRTOS_SWITCH_TASK();
            return ARKRTOS_SBUF_OK;
        }
        /* 空间被别人抢走：整条还没写入一个字节，按剩余超时重试 */
    }
}

/* 下一条完整消息的长度（不消费）；没有完整消息返回负值。
 * 调用方需持临界区。 */
static int32 arkrtos_mbuf_hdr_peek(arkrtos_sbuf_obj_t *p_sb)
{
    uint8 hdr[ARKRTOS_MBUF_HDR_BYTES];
    uint32 len;

    if(p_sb->count < ARKRTOS_MBUF_HDR)
    {
        return -1;
    }

    arkrtos_sbuf_peek_at(p_sb, 0u, hdr, ARKRTOS_MBUF_HDR);
    len = arkrtos_mbuf_get_hdr(hdr);
    if(len == 0u || (len + ARKRTOS_MBUF_HDR) > ARKRTOS_SBUF_SIZE)
    {
        /* 长度头本身坏了（流/消息混用、或调用方自己写了裸字节）。
         * 报错而不是按一个荒唐的长度去读。 */
        return -1;
    }
    return (int32)len;
}

int32 arkrtos_mbuf_peek_len_internal(int32 handle)
{
    arkrtos_sbuf_obj_t *p_sb = arkrtos_sbuf_get(handle);
    int32 len;

    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    ARKRTOS_DISABLE_IRQ();
    len = arkrtos_mbuf_hdr_peek(p_sb);
    if(len > 0)
    {
        if(p_sb->count < ((uint32)len + ARKRTOS_MBUF_HDR))
        {
            len = -1;                    /* 头到了、身子没到齐（不该出现，但不装 */
        }
    }
    ARKRTOS_ENABLE_IRQ();
    return len;
}

int32 arkrtos_mbuf_recv_internal(int32 handle, void *buf, int32 len, int32 timeout_ms)
{
    arkrtos_sbuf_obj_t *p_sb;
    arkrtos_task_t *p_tsk;
    uint32 start_tick;
    uint32 deadline;
    int32 remain;
    int32 ret;
    int32 mlen;
    uint8 hdr[ARKRTOS_MBUF_HDR_BYTES];

    if(buf == 0 || len <= 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    p_sb = arkrtos_sbuf_get(handle);
    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    ARKRTOS_DISABLE_IRQ();
    mlen = arkrtos_mbuf_hdr_peek(p_sb);
    if((mlen > 0) && (p_sb->count >= ((uint32)mlen + ARKRTOS_MBUF_HDR)))
    {
        int32 ret_len;

        if(mlen > len)
        {
            /* 放不下：整条丢掉。留一半会让下一次 recv 把剩下的半个身子
             * 当成长度头来读，从此整条流都错位。 */
            uint8 scratch[16];
            uint32 left = (uint32)mlen + ARKRTOS_MBUF_HDR;

            while(left > 0u)
            {
                uint32 n = (left > sizeof(scratch)) ? (uint32)sizeof(scratch) : left;

                arkrtos_sbuf_get_bytes(p_sb, scratch, n);
                left -= n;
            }
            arkrtos_sbuf_wake_one(p_sb->send_waiters);
            ARKRTOS_ENABLE_IRQ();
            ARKRTOS_SWITCH_TASK();
            return ARKRTOS_SBUF_ERR_TOO_SMALL;
        }

        /* 正常取整条：跳过长度头，再取载荷 */
        arkrtos_sbuf_get_bytes(p_sb, hdr, ARKRTOS_MBUF_HDR);
        ret_len = (int32)arkrtos_mbuf_get_hdr(hdr);
        arkrtos_sbuf_get_bytes(p_sb, (uint8 *)buf, (uint32)ret_len);
        arkrtos_sbuf_wake_one(p_sb->send_waiters);
        mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                     (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
        ARKRTOS_ENABLE_IRQ();
        ARKRTOS_SWITCH_TASK();
        return ret_len;
    }

    if(timeout_ms == 0)
    {
        p_tsk = arkrtos_task_get_current();
        if(p_tsk != 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->recv_waiters, p_tsk);
        }
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_TIMEOUT;
    }

    p_tsk = arkrtos_task_get_current();
    if(p_tsk == 0)
    {
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SBUF_ERR_PARAM;
    }

    start_tick = arkrtos_kernel_tick;
    deadline   = (timeout_ms > 0) ? ARKRTOS_MS_TO_TICK((uint32)timeout_ms) : 0u;

    for(;;)
    {
        ret = arkrtos_sbuf_waiter_add(p_sb->recv_waiters, p_tsk);
        if((ret < 0) && (ret != -2))
        {
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SBUF_ERR_PARAM;
        }

        if(arkrtos_sync_in_handler() != 0u)
        {
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SYNC_ERR_WOULDBLOCK;
        }

        remain = arkrtos_sbuf_remain_ms(start_tick, deadline);
        if(remain == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->recv_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_SBUF_ERR_TIMEOUT;
        }

        arkrtos_trace_wait_obj(MDK_TRACE_ARKRTOS_OBJ(MDK_TRACE_ARKRTOS_CLASS_QUEUE,
                                                     (handle & ARKRTOS_HANDLE_RELMASK)));
        ret = arkrtos_task_block_in_critical((remain < 0) ? 0u : (uint32)remain);

        if((ret & ARKRTOS_WAKE_HANDOFF) == 0)
        {
            arkrtos_sbuf_waiter_remove(p_sb->recv_waiters, p_tsk);
            ARKRTOS_ENABLE_IRQ();
            if(ret == ARKRTOS_WAKE_TIMEOUT)
            {
                return ARKRTOS_SBUF_ERR_TIMEOUT;
            }
            return ARKRTOS_SBUF_ERR_PARAM;
        }

        mlen = arkrtos_mbuf_hdr_peek(p_sb);
        if((mlen > 0) && (p_sb->count >= ((uint32)mlen + ARKRTOS_MBUF_HDR)))
        {
            int32 ret_len;

            if(mlen > len)
            {
                uint8 scratch[16];
                uint32 left = (uint32)mlen + ARKRTOS_MBUF_HDR;

                while(left > 0u)
                {
                    uint32 n = (left > sizeof(scratch)) ? (uint32)sizeof(scratch) : left;

                    arkrtos_sbuf_get_bytes(p_sb, scratch, n);
                    left -= n;
                }
                arkrtos_sbuf_wake_one(p_sb->send_waiters);
                ARKRTOS_ENABLE_IRQ();
                ARKRTOS_SWITCH_TASK();
                return ARKRTOS_SBUF_ERR_TOO_SMALL;
            }

            arkrtos_sbuf_get_bytes(p_sb, hdr, ARKRTOS_MBUF_HDR);
            ret_len = (int32)arkrtos_mbuf_get_hdr(hdr);
            arkrtos_sbuf_get_bytes(p_sb, (uint8 *)buf, (uint32)ret_len);
            arkrtos_sbuf_wake_one(p_sb->send_waiters);
            mdk_trace_arkrtos_obj_signal((uint16)(handle & ARKRTOS_HANDLE_RELMASK),
                                         (uint8)MDK_TRACE_ARKRTOS_CLASS_QUEUE);
            ARKRTOS_ENABLE_IRQ();
            ARKRTOS_SWITCH_TASK();
            return ret_len;
        }
    }
}

int32 arkrtos_mbuf_send_from_isr_internal(int32 handle, const void *buf, int32 len)
{
    arkrtos_sbuf_obj_t *p_sb;
    uint32 need;
    uint32 state;
    uint8 hdr[ARKRTOS_MBUF_HDR_BYTES];

    if(buf == 0 || len <= 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    p_sb = arkrtos_sbuf_get(handle);
    if(p_sb == 0)
    {
        return ARKRTOS_SBUF_ERR_PARAM;
    }
    need = (uint32)len + ARKRTOS_MBUF_HDR;
    if(need > ARKRTOS_SBUF_SIZE)
    {
        return ARKRTOS_SBUF_ERR_NOSPC;
    }

    arkrtos_mbuf_put_hdr(hdr, (uint32)len);

    state = ARKRTOS_ENTER_CRITICAL();
    if(arkrtos_sbuf_room(p_sb) < need)
    {
        ARKRTOS_EXIT_CRITICAL(state);
        return ARKRTOS_SBUF_ERR_TIMEOUT;      /* ISR 里不能等，也绝不写半条 */
    }
    arkrtos_sbuf_put_bytes(p_sb, hdr, ARKRTOS_MBUF_HDR);
    arkrtos_sbuf_put_bytes(p_sb, (const uint8 *)buf, (uint32)len);
    arkrtos_sbuf_wake_one(p_sb->recv_waiters);
    ARKRTOS_EXIT_CRITICAL(state);
    return ARKRTOS_SBUF_OK;
}

#endif /* ARKRTOS_USE_SBUF */
