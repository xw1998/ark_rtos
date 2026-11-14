/**
* @brief Ark RTOS App SDK - 系统调用封装实现
* @details 把应用侧 API 封装成 SVC 调用；应用通过 arkrtos.h 使用，不直接接触内核。
* @author xw
* @date 2026.05.03
*/

#include "arkrtos.h"
#include "arkrtos_ulog.h"
#include "arkrtos_svc_call.h"
#include "arkrtos_def.h"

ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_DEV_IO, arkrtos_call_dev_io, uint32 *);
ARKRTOS_SVC_DECL_V2(ARKRTOS_SVC_TASK_CTRL, arkrtos_call_task_ctrl, uint32, uint32);
ARKRTOS_SVC_DECL_1(uint32, ARKRTOS_SVC_SYS_INFO, arkrtos_call_sys_info, uint32);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_EVENT_CTRL, arkrtos_call_event_ctrl, uint32 *);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_SYNC_CTRL, arkrtos_call_sync_ctrl, uint32 *);

int32 arkrtos_dev_open(char *name, uint32 param)
{
    uint32 parameters[4];
    parameters[0] = ARKRTOS_DEV_SUB_OPEN;
    parameters[1] = (uint32)name;
    parameters[2] = param;
    return arkrtos_call_dev_io(parameters);
}

int32 arkrtos_dev_read(int32 handle, void *pdata, int32 len)
{
    uint32 parameters[4];
    parameters[0] = ARKRTOS_DEV_SUB_READ;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;
    return arkrtos_call_dev_io(parameters);
}

int32 arkrtos_dev_write(int32 handle, void *pdata, int32 len)
{
    uint32 parameters[4];
    parameters[0] = ARKRTOS_DEV_SUB_WRITE;
    parameters[1] = handle;
    parameters[2] = (uint32)pdata;
    parameters[3] = len;
    return arkrtos_call_dev_io(parameters);
}

int32 arkrtos_dev_ctrl(int32 handle, uint32 code, uint32 value)
{
    uint32 parameters[4];
    parameters[0] = ARKRTOS_DEV_SUB_CTRL;
    parameters[1] = handle;
    parameters[2] = code;
    parameters[3] = value;
    return arkrtos_call_dev_io(parameters);
}

int32 arkrtos_dev_close(int32 handle)
{
    uint32 parameters[4];
    parameters[0] = ARKRTOS_DEV_SUB_CLOSE;
    parameters[1] = handle;
    return arkrtos_call_dev_io(parameters);
}

int32 arkrtos_dev_set_will(const char *dev_name, int32 cmd, int32 val)
{
    uint32 parameters[4];
    parameters[0] = ARKRTOS_DEV_SUB_SET_WILL;                 /* DEV_IO sub 6: declare this task's will */
    parameters[1] = (uint32)dev_name;
    parameters[2] = (uint32)cmd;
    parameters[3] = (uint32)val;
    return arkrtos_call_dev_io(parameters);
}

void arkrtos_task_wait(uint32 ms)
{
    arkrtos_call_task_ctrl(1, ms);
}

void arkrtos_task_wait_period(void)
{
    arkrtos_call_task_ctrl(2, 0);
}

void arkrtos_task_delay(uint32 us)
{
    arkrtos_call_task_ctrl(3, us);
}

void arkrtos_task_kill(void)
{
    arkrtos_call_task_ctrl(4, 0);
}

uint32 arkrtos_get_time_ms(void)
{
    return arkrtos_call_sys_info(1);
}

uint32 arkrtos_get_cpu_usage(void)
{
    /* The kernel counts "a task was running" ticks inside a 1024 tick window,
     * so the raw value is 0..1024. arkrtos.h publishes a 0..100 percentage, so
     * scale it here instead of leaking the internal fixed point value. */
    uint32 busy = arkrtos_call_sys_info(2);

    if(busy > 1024u)
    {
        busy = 1024u;
    }

    return (busy * 100u) / 1024u;
}
/* ------------------------------------------------------------------
 * User-mode polling for blocking waits
 *
 * The kernel cannot yield while it is inside an SVC handler (PendSV never
 * preempts SVC), so a wait that arrives from App code only registers and
 * answers ARKRTOS_SYNC_ERR_WOULDBLOCK.  The real yield has to happen here, in
 * Thread mode: give up a millisecond, ask again, until the token arrives,
 * the deadline passes or the object reports an error.  timeout <= 0 means
 * "wait forever".
 * ------------------------------------------------------------------ */
#define ARKRTOS_APP_POLL_MS   (1u)

static int32 arkrtos_sync_wait_expired(uint32 start_ms, int32 timeout)
{
    if(timeout <= 0)
    {
        return 0;                           /* unbounded wait */
    }
    return ((arkrtos_get_time_ms() - start_ms) >= (uint32)timeout) ? 1 : 0;
}


int32 arkrtos_event_create(char *name)
{
    uint32 p[3];
    p[0] = ARKRTOS_EVENT_SUB_CREATE;
    p[1] = (uint32)name;
    return arkrtos_call_event_ctrl(p);
}

static int32 arkrtos_event_wait_raw(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = ARKRTOS_EVENT_SUB_WAIT;
    p[1] = handle;
    p[2] = timeout;
    return (int32)arkrtos_call_event_ctrl(p);
}

int32 arkrtos_event_wait(int32 handle, int32 timeout)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        int32 r = arkrtos_event_wait_raw(handle, timeout);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout) != 0)
        {
            /* The kernel no longer arms a timer for a user-mode waiter:
             * close it out here.  timeout 0 makes the kernel drop the own
             * registration and answer TIMEOUT. */
            (void)arkrtos_event_wait_raw(handle, 0);
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

void arkrtos_event_set(int32 handle)
{
    uint32 p[3];
    p[0] = ARKRTOS_EVENT_SUB_SET;
    p[1] = handle;
    arkrtos_call_event_ctrl(p);
}

int32 arkrtos_sem_create(char *name, int32 init_count)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_SEM_CREATE;
    p[1] = (uint32)name;
    p[2] = (uint32)init_count;
    return arkrtos_call_sync_ctrl(p);
}

static int32 arkrtos_sem_wait_raw(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_SEM_WAIT;
    p[1] = handle;
    p[2] = (uint32)timeout;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_sem_wait(int32 handle, int32 timeout)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        int32 r = arkrtos_sem_wait_raw(handle, timeout);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout) != 0)
        {
            (void)arkrtos_sem_wait_raw(handle, 0);
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_sem_post(int32 handle)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_SEM_POST;
    p[1] = handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_sem_delete(int32 handle)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_SEM_DELETE;
    p[1] = handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_mutex_create(char *name)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_MTX_CREATE;
    p[1] = (uint32)name;
    return arkrtos_call_sync_ctrl(p);
}

static int32 arkrtos_mutex_lock_raw(int32 handle, int32 timeout)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_MTX_LOCK;
    p[1] = handle;
    p[2] = (uint32)timeout;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_mutex_lock(int32 handle, int32 timeout)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        int32 r = arkrtos_mutex_lock_raw(handle, timeout);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout) != 0)
        {
            (void)arkrtos_mutex_lock_raw(handle, 0);
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_mutex_unlock(int32 handle)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_MTX_UNLOCK;
    p[1] = handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_mutex_delete(int32 handle)
{
    uint32 p[3];
    p[0] = ARKRTOS_SYNC_SUB_MTX_DELETE;
    p[1] = handle;
    return arkrtos_call_sync_ctrl(p);
}

/* ============================================================
 * 条件变量（E 项：POSIX 补齐）
 *
 * 子命令 9..13 与内核 SYNC_CTRL 的 case 一一对应。cond_wait 要传四个参数
 * （子命令 + cond + mutex + timeout），所以数组开到 p[4]
 * —— 内核方会读 p[3]，开 p[3] 是越界。
 * ============================================================ */
int32 arkrtos_cond_create(char *name)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_CREATE;
    p[1] = (uint32)name;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_cond_enqueue(int32 cond_handle)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_ENQUEUE;
    p[1] = (uint32)cond_handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_cond_poll(int32 cond_handle)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_POLL;
    p[1] = (uint32)cond_handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_cond_abort(int32 cond_handle)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_ABORT;
    p[1] = (uint32)cond_handle;
    return arkrtos_call_sync_ctrl(p);
}

/* POSIX semantics, three user-mode steps:
 *   1. register on the cond (idempotent) and release the mutex,
 *   2. poll until signal/broadcast pops us off the queue, or the deadline,
 *   3. take the mutex back before returning, whichever way it ended.
 * The kernel side never blocks here, so no token is floating around: a
 * signalled waiter is simply no longer in the queue. */
int32 arkrtos_cond_wait(int32 cond_handle, int32 mutex_handle, int32 timeout)
{
    int32  r;
    int32  woken = 0;
    uint32 start = arkrtos_get_time_ms();

    r = arkrtos_cond_enqueue(cond_handle);
    if(r != ARKRTOS_SYNC_OK)
    {
        return r;
    }

    r = arkrtos_mutex_unlock(mutex_handle);
    if(r != ARKRTOS_SYNC_OK)
    {
        (void)arkrtos_cond_abort(cond_handle);
        return r;
    }

    for(;;)
    {
        r = arkrtos_cond_poll(cond_handle);
        if(r != 0)
        {
            woken = (r == 1) ? 1 : 0;        /* 1 = signalled, <0 = error */
            break;
        }
        if(arkrtos_sync_wait_expired(start, timeout) != 0)
        {
            (void)arkrtos_cond_abort(cond_handle);
            break;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }

    /* Whatever happened, the mutex must be held again on return. */
    for(;;)
    {
        r = arkrtos_mutex_lock(mutex_handle, -1);
        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            break;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
    if(r != ARKRTOS_SYNC_OK)
    {
        return r;
    }

    return (woken != 0) ? ARKRTOS_SYNC_OK : ARKRTOS_SYNC_ERR_TIMEOUT;
}

int32 arkrtos_cond_signal(int32 handle)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_SIGNAL;
    p[1] = (uint32)handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_cond_broadcast(int32 handle)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_BROADCAST;
    p[1] = (uint32)handle;
    return arkrtos_call_sync_ctrl(p);
}

int32 arkrtos_cond_delete(int32 handle)
{
    uint32 p[4];
    p[0] = ARKRTOS_SYNC_SUB_COND_DELETE;
    p[1] = (uint32)handle;
    return arkrtos_call_sync_ctrl(p);
}

/* ============================================================
 * 消息队列 / 软定时器 / 任务与故障查询（P0 扩展）
 * ============================================================ */
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_MQ_CTRL, arkrtos_call_mq_ctrl, uint32 *);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_SBUF_CTRL, arkrtos_call_sbuf_ctrl, uint32 *);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_MEMPOOL_CTRL, arkrtos_call_mpool_ctrl, uint32 *);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_TIMER_CTRL, arkrtos_call_timer_ctrl, uint32 *);
ARKRTOS_SVC_DECL_2(int32, ARKRTOS_SVC_TASK_CTRL, arkrtos_call_task_ctrl_ret, uint32, uint32);
ARKRTOS_SVC_DECL_3(uint32, ARKRTOS_SVC_SYS_INFO, arkrtos_call_sys_info_ext, uint32, uint32, uint32);

int32 arkrtos_mq_create(char *name)
{
    uint32 p[6];
    p[0] = ARKRTOS_MQ_SUB_CREATE; p[1] = (uint32)name; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_mq_ctrl(p);
}

static int32 arkrtos_mq_send_raw(int32 handle, void *buf, int32 len_words, int32 timeout)
{
    uint32 p[6];
    p[0] = ARKRTOS_MQ_SUB_SEND; p[1] = handle; p[2] = (uint32)buf;
    p[3] = len_words; p[4] = timeout; p[5] = 0;
    return arkrtos_call_mq_ctrl(p);
}

int32 arkrtos_mq_send(int32 handle, void *buf, int32 len_words, int32 timeout)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        int32 r = arkrtos_mq_send_raw(handle, buf, len_words, timeout);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout) != 0)
        {
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

static int32 arkrtos_mq_recv_raw(int32 handle, void *buf, int32 len_words, int32 timeout)
{
    uint32 p[6];
    p[0] = ARKRTOS_MQ_SUB_RECV; p[1] = handle; p[2] = (uint32)buf;
    p[3] = len_words; p[4] = timeout; p[5] = 0;
    return arkrtos_call_mq_ctrl(p);
}

int32 arkrtos_mq_recv(int32 handle, void *buf, int32 len_words, int32 timeout)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        int32 r = arkrtos_mq_recv_raw(handle, buf, len_words, timeout);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout) != 0)
        {
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_mq_delete(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_MQ_SUB_DELETE; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_mq_ctrl(p);
}

/* ============================================================
 * 流缓冲 / 消息缓冲（SVC 0x1F）
 * 阻塞语义与消息队列一致：内核在 SVC 里只登记，真正让出 CPU 由这里的
 * 轮询做（WOULDBLOCK -> 等 1 ms -> 重问 -> 直到超时）。
 * ============================================================ */
int32 arkrtos_sbuf_create(char *name)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_CREATE; p[1] = (uint32)name; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

/* 流语义下"部分成功"是正常返回，不重试；只有 WOULDBLOCK 才重问。 */
int32 arkrtos_sbuf_send(int32 handle, const void *buf, int32 len, int32 timeout_ms)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        uint32 p[6];
        int32 r;

        p[0] = ARKRTOS_SBUF_SUB_SEND; p[1] = (uint32)handle; p[2] = (uint32)buf;
        p[3] = (uint32)len; p[4] = (uint32)timeout_ms; p[5] = 0;
        r = arkrtos_call_sbuf_ctrl(p);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout_ms) != 0)
        {
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_sbuf_recv(int32 handle, void *buf, int32 len, int32 timeout_ms)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        uint32 p[6];
        int32 r;

        p[0] = ARKRTOS_SBUF_SUB_RECV; p[1] = (uint32)handle; p[2] = (uint32)buf;
        p[3] = (uint32)len; p[4] = (uint32)timeout_ms; p[5] = 0;
        r = arkrtos_call_sbuf_ctrl(p);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout_ms) != 0)
        {
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_sbuf_send_from_isr(int32 handle, const void *buf, int32 len)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_SEND_ISR; p[1] = (uint32)handle; p[2] = (uint32)buf;
    p[3] = (uint32)len; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_sbuf_used(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_USED; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_sbuf_free(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_FREE; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_sbuf_reset(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_RESET; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_sbuf_delete(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_DELETE; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_mbuf_send(int32 handle, const void *buf, int32 len, int32 timeout_ms)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        uint32 p[6];
        int32 r;

        p[0] = ARKRTOS_SBUF_SUB_MBUF_SEND; p[1] = (uint32)handle; p[2] = (uint32)buf;
        p[3] = (uint32)len; p[4] = (uint32)timeout_ms; p[5] = 0;
        r = arkrtos_call_sbuf_ctrl(p);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout_ms) != 0)
        {
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_mbuf_recv(int32 handle, void *buf, int32 len, int32 timeout_ms)
{
    uint32 start = arkrtos_get_time_ms();

    for(;;)
    {
        uint32 p[6];
        int32 r;

        p[0] = ARKRTOS_SBUF_SUB_MBUF_RECV; p[1] = (uint32)handle; p[2] = (uint32)buf;
        p[3] = (uint32)len; p[4] = (uint32)timeout_ms; p[5] = 0;
        r = arkrtos_call_sbuf_ctrl(p);

        if(r != ARKRTOS_SYNC_ERR_WOULDBLOCK)
        {
            return r;
        }
        if(arkrtos_sync_wait_expired(start, timeout_ms) != 0)
        {
            return ARKRTOS_SYNC_ERR_TIMEOUT;
        }
        (void)arkrtos_task_wait(ARKRTOS_APP_POLL_MS);
    }
}

int32 arkrtos_mbuf_send_from_isr(int32 handle, const void *buf, int32 len)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_MBUF_SEND_ISR; p[1] = (uint32)handle; p[2] = (uint32)buf;
    p[3] = (uint32)len; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_mbuf_peek_len(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_SBUF_SUB_MBUF_PEEK_LEN; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_sbuf_ctrl(p);
}

int32 arkrtos_timer_create(char *name)
{
    uint32 p[6];
    p[0] = ARKRTOS_TIMER_SUB_CREATE; p[1] = (uint32)name; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_timer_ctrl(p);
}

int32 arkrtos_timer_start(int32 handle, uint32 period_ms, uint32 mode,
                        void (*cb)(void *), void *arg)
{
    uint32 p[6];
    p[0] = ARKRTOS_TIMER_SUB_START; p[1] = (uint32)handle; p[2] = period_ms; p[3] = mode;
    p[4] = (uint32)cb; p[5] = (uint32)arg;
    return arkrtos_call_timer_ctrl(p);
}

int32 arkrtos_timer_stop(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_TIMER_SUB_STOP; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_timer_ctrl(p);
}

int32 arkrtos_timer_delete(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_TIMER_SUB_DELETE; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_timer_ctrl(p);
}

int32 arkrtos_task_status_get(int32 task_id)
{
    return arkrtos_call_task_ctrl_ret(5, (uint32)task_id);
}

int32 arkrtos_task_recover_req(int32 task_id)
{
    return arkrtos_call_task_ctrl_ret(6, (uint32)task_id);
}

int32 arkrtos_fault_record_count(void)
{
    return (int32)arkrtos_call_sys_info_ext(3, 0, 0);
}

int32 arkrtos_fault_record_read(int32 index, uint32 *out3)
{
    return (int32)arkrtos_call_sys_info_ext(4, (uint32)index, (uint32)out3);
}

/* ============================================================
 * 调度器锁 / 任务栈用量查询（SVC 0x11 子命令 7~10）
 * ============================================================ */

/* 3 参数版本的任务控制调用：r0=子命令，r1/r2=参数 */
ARKRTOS_SVC_DECL_3(int32, ARKRTOS_SVC_TASK_CTRL, arkrtos_call_task_ctrl_arg2, uint32, uint32, uint32);

void arkrtos_sched_lock(void)
{
    arkrtos_call_task_ctrl_ret(7, 0);
}

void arkrtos_sched_unlock(void)
{
    arkrtos_call_task_ctrl_ret(8, 0);
}

int32 arkrtos_sched_lock_count(void)
{
    return arkrtos_call_task_ctrl_ret(9, 0);
}

int32 arkrtos_task_stack_info(int32 task_id, uint32 *out3)
{
    return arkrtos_call_task_ctrl_arg2(10, (uint32)task_id, (uint32)out3);
}

/* ============================================================
 * App 镜像管理与分区查询（SVC 0x18 子命令 2~5）
 * ============================================================ */
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_APP_MGR, arkrtos_call_app_mgr, uint32 *);

int32 arkrtos_app_load(int32 dev, uint32 image_len)
{
    uint32 p[6];
    p[0] = ARKRTOS_APP_SUB_LOAD_DEV; p[1] = (uint32)dev; p[2] = image_len; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_app_mgr(p);
}

int32 arkrtos_app_start(uint32 slot)
{
    uint32 p[6];
    p[0] = ARKRTOS_APP_SUB_START; p[1] = slot; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_app_mgr(p);
}

int32 arkrtos_app_stop(uint32 slot)
{
    uint32 p[6];
    p[0] = ARKRTOS_APP_SUB_STOP; p[1] = slot; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_app_mgr(p);
}

uint32 arkrtos_app_status(uint32 slot)
{
    uint32 p[6];
    p[0] = ARKRTOS_APP_SUB_STATUS; p[1] = slot; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return (uint32)arkrtos_call_app_mgr(p);
}

int32 arkrtos_driver_load(int32 dev, uint32 image_len)
{
    uint32 p[6];
    p[0] = ARKRTOS_APP_SUB_DRIVER_LOAD; p[1] = (uint32)dev; p[2] = image_len; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_app_mgr(p);
}

/* ============================================================
 * Log and console services (SVC 0x19 / 0x1A)
 *
 * The formatting itself is done on this side (see arkrtos_ulog.h), so the
 * kernel only has to filter by level and push the bytes out. One level
 * setting therefore applies to the kernel and to every App / driver.
 * ============================================================ */
ARKRTOS_SVC_DECL_3(int32, ARKRTOS_SVC_LOG, arkrtos_call_log, uint32, uint32, uint32);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_SHELL, arkrtos_call_shell_svc, uint32 *);

int32 arkrtos_log_print(uint32 level, const char *tag, const char *msg)
{
    return arkrtos_call_log(level, (uint32)tag, (uint32)msg);
}

int32 arkrtos_shell_cmd_register(const arkrtos_ushell_cmd_t *cmd)
{
    uint32 p[3];
    p[0] = ARKRTOS_SHELL_SUB_REGISTER; p[1] = (uint32)cmd; p[2] = 0;
    return arkrtos_call_shell_svc(p);
}

int32 arkrtos_shell_cmd_unregister(const char *name)
{
    uint32 p[3];
    p[0] = ARKRTOS_SHELL_SUB_UNREGISTER; p[1] = (uint32)name; p[2] = 0;
    return arkrtos_call_shell_svc(p);
}

int32 arkrtos_shell_print(const char *msg)
{
    uint32 p[3];
    p[0] = ARKRTOS_SHELL_SUB_PRINT; p[1] = (uint32)msg; p[2] = 0;
    return arkrtos_call_shell_svc(p);
}

/* ============================================================
 * User thread service (SVC 0x1B)
 *
 * The kernel owns the TCB and the scheduler, so a user mode thread has to be
 * asked for across the SVC boundary. The entry point and the stack window are
 * validated kernel side against the caller's own regions.
 * ============================================================ */
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_THREAD_CTRL, arkrtos_call_thread_ctrl, uint32 *);

int32 arkrtos_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
                          uint32 priority, uint32 period_ms)
{
    uint32 p[6];
    p[0] = ARKRTOS_THREAD_SUB_CREATE; p[1] = (uint32)entry; p[2] = (uint32)stack;
    p[3] = stack_size; p[4] = priority; p[5] = period_ms;
    return arkrtos_call_thread_ctrl(p);
}

int32 arkrtos_thread_self(void)
{
    uint32 p[6];
    p[0] = ARKRTOS_THREAD_SUB_SELF; p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_thread_ctrl(p);
}

void arkrtos_thread_exit(void)
{
    uint32 p[6];
    p[0] = ARKRTOS_THREAD_SUB_EXIT; p[1] = 0; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    (void)arkrtos_call_thread_ctrl(p);
}

/* ============================================================
 * File service (SVC 0x1C)
 *
 * Path based: the kernel opens the file, moves the bytes and closes it
 * inside one call, so an App never holds a kernel side handle that could
 * outlive it or be guessed by somebody else. The kernel validates the path
 * and the buffer against the caller's own memory; littlefs and the block
 * device stay entirely on the kernel side.
 * ============================================================ */
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_FILE_SYS, arkrtos_call_file_svc, uint32 *);
ARKRTOS_SVC_DECL_1(int32, ARKRTOS_SVC_HEARTBEAT, arkrtos_call_heartbeat, uint32 *);

int32 arkrtos_file_write(const char *path, const void *data, uint32 len)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_WRITE;
    parameters[1] = (uint32)path;
    parameters[2] = (uint32)data;
    parameters[3] = len;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_file_read(const char *path, void *buf, uint32 max)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_READ;
    parameters[1] = (uint32)path;
    parameters[2] = (uint32)buf;
    parameters[3] = max;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_file_remove(const char *path)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_REMOVE;
    parameters[1] = (uint32)path;
    parameters[2] = 0;
    parameters[3] = 0;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_file_stat(const char *path, uint32 *size, uint32 *is_dir)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_STAT;
    parameters[1] = (uint32)path;
    parameters[2] = (uint32)size;
    parameters[3] = (uint32)is_dir;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_heartbeat(uint32 period_ms)
{
    uint32 parameters[2];

    parameters[0] = ARKRTOS_HB_SUB_REPORT;
    parameters[1] = period_ms;
    return arkrtos_call_heartbeat(parameters);
}

int32 arkrtos_file_info(uint32 *total, uint32 *used)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_INFO;
    parameters[1] = (uint32)total;
    parameters[2] = (uint32)used;
    parameters[3] = 0;
    return arkrtos_call_file_svc(parameters);
}

/* ---- mount / random access / rename / list (FILE_SYS sub 6..10) ----
 * A mount does not survive a reset, so an App that needs the volume has to
 * ask for it itself instead of assuming the shell did it earlier. */
int32 arkrtos_file_mount(void)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_MOUNT;
    parameters[1] = 0;
    parameters[2] = 0;
    parameters[3] = 0;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_file_unmount(void)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_UNMOUNT;
    parameters[1] = 0;
    parameters[2] = 0;
    parameters[3] = 0;
    return arkrtos_call_file_svc(parameters);
}

/* Returns the number of bytes actually read (>= 0), or a negative error. */
int32 arkrtos_file_read_at(const char *path, void *buf, uint32 len, uint32 off)
{
    uint32 parameters[5];

    parameters[0] = ARKRTOS_FILE_SUB_READ_AT;
    parameters[1] = (uint32)path;
    parameters[2] = (uint32)buf;
    parameters[3] = len;
    parameters[4] = off;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_file_rename(const char *old_path, const char *new_path)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_FILE_SUB_RENAME;
    parameters[1] = (uint32)old_path;
    parameters[2] = (uint32)new_path;
    parameters[3] = 0;
    return arkrtos_call_file_svc(parameters);
}

/* Names separated by newlines; *count receives how many entries fit. */
int32 arkrtos_file_list_names(const char *dir, char *buf, uint32 size, uint32 *count)
{
    uint32 parameters[5];

    parameters[0] = ARKRTOS_FILE_SUB_LIST;
    parameters[1] = (uint32)dir;
    parameters[2] = (uint32)buf;
    parameters[3] = size;
    parameters[4] = (uint32)count;
    return arkrtos_call_file_svc(parameters);
}

/* ============================================================
 * The VFS namespace (SVC 0x1C, sub 11..18)
 *
 * Same service number as the volume calls above, different sub commands:
 * 1..10 take a path inside the mounted volume, 11..18 take an absolute
 * namespace path and keep a kernel side handle open between calls. The
 * kernel validates the path and every buffer against the caller's own RAM
 * and refuses a handle that belongs to another task.
 * ============================================================ */
int32 arkrtos_path_open(const char *path, uint32 flags)
{
    uint32 parameters[3];

    parameters[0] = ARKRTOS_PATH_SUB_OPEN;
    parameters[1] = (uint32)path;
    parameters[2] = flags;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_read(int32 handle, void *buf, uint32 len)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_PATH_SUB_READ;
    parameters[1] = (uint32)handle;
    parameters[2] = (uint32)buf;
    parameters[3] = len;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_write(int32 handle, const void *buf, uint32 len)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_PATH_SUB_WRITE;
    parameters[1] = (uint32)handle;
    parameters[2] = (uint32)buf;
    parameters[3] = len;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_close(int32 handle)
{
    uint32 parameters[2];

    parameters[0] = ARKRTOS_PATH_SUB_CLOSE;
    parameters[1] = (uint32)handle;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_stat(const char *path, uint32 *size, uint32 *mode)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_PATH_SUB_STAT;
    parameters[1] = (uint32)path;
    parameters[2] = (uint32)size;
    parameters[3] = (uint32)mode;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_seek(int32 handle, int32 off, uint32 whence)
{
    uint32 parameters[4];

    parameters[0] = ARKRTOS_PATH_SUB_SEEK;
    parameters[1] = (uint32)handle;
    parameters[2] = (uint32)off;
    parameters[3] = whence;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_readdir(int32 handle, char *name, uint32 name_size,
                         uint32 *mode, uint32 *size)
{
    uint32 parameters[6];

    parameters[0] = ARKRTOS_PATH_SUB_READDIR;
    parameters[1] = (uint32)handle;
    parameters[2] = (uint32)name;
    parameters[3] = name_size;
    parameters[4] = (uint32)mode;
    parameters[5] = (uint32)size;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_unlink(const char *path)
{
    uint32 parameters[2];

    parameters[0] = ARKRTOS_PATH_SUB_UNLINK;
    parameters[1] = (uint32)path;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_mkdir(const char *path)
{
    uint32 parameters[2];

    parameters[0] = ARKRTOS_PATH_SUB_MKDIR;
    parameters[1] = (uint32)path;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_rmdir(const char *path)
{
    uint32 parameters[2];

    parameters[0] = ARKRTOS_PATH_SUB_RMDIR;
    parameters[1] = (uint32)path;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_rename(const char *old_path, const char *new_path)
{
    uint32 parameters[3];

    parameters[0] = ARKRTOS_PATH_SUB_RENAME;
    parameters[1] = (uint32)old_path;
    parameters[2] = (uint32)new_path;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_truncate(const char *path, uint32 size)
{
    uint32 parameters[3];

    parameters[0] = ARKRTOS_PATH_SUB_TRUNCATE;
    parameters[1] = (uint32)path;
    parameters[2] = size;
    return arkrtos_call_file_svc(parameters);
}

int32 arkrtos_path_ftruncate(int32 handle, uint32 size)
{
    uint32 parameters[3];

    parameters[0] = ARKRTOS_PATH_SUB_FTRUNCATE;
    parameters[1] = (uint32)handle;
    parameters[2] = size;
    return arkrtos_call_file_svc(parameters);
}

/* ============================================================
 * 固定块内存池（SVC 0x20）
 * 池的内存区由调用者提供（必须在本任务可见 RAM 内，内核会校验整段范围），
 * 所以这里不需要任何内核侧的动态内存；分配/释放都不阻塞。
 * ============================================================ */
int32 arkrtos_mpool_create(char *name, void *pool, uint32 block_size, uint32 block_count)
{
    uint32 p[6];
    p[0] = ARKRTOS_MPOOL_SUB_CREATE; p[1] = (uint32)name; p[2] = (uint32)pool;
    p[3] = block_size; p[4] = block_count; p[5] = 0;
    return arkrtos_call_mpool_ctrl(p);
}

void *arkrtos_mpool_alloc(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_MPOOL_SUB_ALLOC; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return (void *)arkrtos_call_mpool_ctrl(p);
}

int32 arkrtos_mpool_free(int32 handle, void *ptr)
{
    uint32 p[6];
    p[0] = ARKRTOS_MPOOL_SUB_FREE; p[1] = (uint32)handle; p[2] = (uint32)ptr;
    p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_mpool_ctrl(p);
}

int32 arkrtos_mpool_query(int32 handle, arkrtos_mpool_stat_t *out)
{
    uint32 p[6];
    p[0] = ARKRTOS_MPOOL_SUB_QUERY; p[1] = (uint32)handle; p[2] = (uint32)out;
    p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_mpool_ctrl(p);
}

int32 arkrtos_mpool_delete(int32 handle)
{
    uint32 p[6];
    p[0] = ARKRTOS_MPOOL_SUB_DELETE; p[1] = (uint32)handle; p[2] = 0; p[3] = 0; p[4] = 0; p[5] = 0;
    return arkrtos_call_mpool_ctrl(p);
}
