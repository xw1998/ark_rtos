/**
* @brief Ark RTOS 软定时器模块实现
* @details 架构：
*          - 定时器对象为静态池（ARKRTOS_TIMER_NUM），对象创建/start/stop/delete 走 SVC
*          - arkrtos_timer_tick_handler() 在 tick 中断里对激活定时器做倒计时，
*            到期时打 expired 标记并唤醒定时器服务任务
*          - 定时器服务任务（优先级 ARKRTOS_TIMER_TASK_PRI，可被业务任务抢占）
*            被唤醒后逐个执行到期回调；单次定时器回调后自动停止
*          - 用户回调永远运行在任务上下文，不占用中断时间、不破坏特权隔离
*/

#include "arkrtos_timer.h"
#include "arkrtos_hal.h"
#include "arkrtos_cfg.h"

#if (ARKRTOS_USE_TIMER == 1)

static arkrtos_timer_obj_t arkrtos_timers[ARKRTOS_TIMER_NUM];

/* 定时器服务任务的栈与任务号（install 时分配） */
static uint32 arkrtos_timer_task_stack[ARKRTOS_TIMER_TASK_STACK_WORDS];
static int32  arkrtos_timer_task_id = -1;

void arkrtos_timer_module_init(void)
{
    int32 i;
    for(i = 0; i < ARKRTOS_TIMER_NUM; i++)
    {
        arkrtos_timers[i].name[0]  = 0;
        arkrtos_timers[i].used     = 0;
        arkrtos_timers[i].active   = 0;
        arkrtos_timers[i].mode     = ARKRTOS_TIMER_MODE_PERIODIC;
        arkrtos_timers[i].expired  = 0;
        arkrtos_timers[i].period_ticks = 0;
        arkrtos_timers[i].remain_ticks = 0;
        arkrtos_timers[i].cb       = 0;
        arkrtos_timers[i].arg      = 0;
    }
    arkrtos_timer_task_id = -1;
}

/* 定时器服务任务主体：无事件时永久阻塞，被 tick 唤醒后执行到期回调 */
static void arkrtos_timer_daemon(void)
{
    while(1)
    {
        int32 i;
        int32 has_work = 0;

        ARKRTOS_DISABLE_IRQ();
        for(i = 0; i < ARKRTOS_TIMER_NUM; i++)
        {
            if(arkrtos_timers[i].used == 1 && arkrtos_timers[i].expired == 1)
            {
                has_work = 1;
                break;
            }
        }
        ARKRTOS_ENABLE_IRQ();

        if(has_work == 0)
        {
            arkrtos_task_block_internal();    /* 等待下一次到期唤醒 */
            continue;
        }

        /* 逐个执行到期回调（快照 cb/arg 后清除标记，允许回调内 stop/delete 自身） */
        for(i = 0; i < ARKRTOS_TIMER_NUM; i++)
        {
            void (*p_cb)(void *);
            void *p_arg;

            ARKRTOS_DISABLE_IRQ();
            if(arkrtos_timers[i].used == 1 && arkrtos_timers[i].expired == 1)
            {
                p_cb = arkrtos_timers[i].cb;
                p_arg = arkrtos_timers[i].arg;
                arkrtos_timers[i].expired = 0;
                if(arkrtos_timers[i].mode == ARKRTOS_TIMER_MODE_ONESHOT)
                {
                    arkrtos_timers[i].active = 0;
                }
                ARKRTOS_ENABLE_IRQ();
            }
            else
            {
                ARKRTOS_ENABLE_IRQ();
                continue;
            }

            if(p_cb != 0)
            {
                p_cb(p_arg);
            }
        }
    }
}

/* 在内核初始化（cfg_load 之后、调度启动之前）调用，把服务任务注册进任务表 */
void arkrtos_timer_task_install(void)
{
    arkrtos_timer_task_id = arkrtos_task_register(arkrtos_timer_daemon,
                                              arkrtos_timer_task_stack,
                                              sizeof(arkrtos_timer_task_stack),
                                              (uint8)ARKRTOS_TIMER_TASK_PRI,
                                              0);
}

void arkrtos_timer_tick_handler(void)
{
    int32 i;
    int32 need_wake = 0;

    for(i = 0; i < ARKRTOS_TIMER_NUM; i++)
    {
        if(arkrtos_timers[i].used == 1 && arkrtos_timers[i].active == 1)
        {
            if(arkrtos_timers[i].remain_ticks > 0)
            {
                arkrtos_timers[i].remain_ticks--;
            }
            if(arkrtos_timers[i].remain_ticks == 0)
            {
                arkrtos_timers[i].expired = 1;
                if(arkrtos_timers[i].mode == ARKRTOS_TIMER_MODE_PERIODIC)
                {
                    arkrtos_timers[i].remain_ticks = arkrtos_timers[i].period_ticks;
                }
                need_wake = 1;
            }
        }
    }

    if(need_wake == 1 && arkrtos_timer_task_id > 0)
    {
        arkrtos_task_t *p_tsk = &arkrtos_task_table[arkrtos_timer_task_id - 1];
        if(p_tsk->status == ARKRTOS_TASK_WAIT)
        {
            p_tsk->wait_time = 0;
            p_tsk->wake_reason = ARKRTOS_WAKE_HANDOFF;
            p_tsk->status = ARKRTOS_TASK_READY;
            arkrtos_ready_add(arkrtos_timer_task_id - 1);
        }
    }
}

static void arkrtos_timer_copy_name(char *dst, char *src)
{
    int32 i;
    for(i = 0; i < 7; i++)
    {
        dst[i] = src[i];
        if(src[i] == 0)
            break;
    }
    dst[7] = 0;
}

int32 arkrtos_timer_create_internal(char *name)
{
    int32 i;
    ARKRTOS_DISABLE_IRQ();
    for(i = 0; i < ARKRTOS_TIMER_NUM; i++)
    {
        if(arkrtos_timers[i].used == 0)
        {
            arkrtos_timer_copy_name(arkrtos_timers[i].name, name);
            arkrtos_timers[i].used    = 1;
            arkrtos_timers[i].active  = 0;
            arkrtos_timers[i].expired = 0;
            arkrtos_timers[i].cb      = 0;
            arkrtos_timers[i].arg     = 0;
            ARKRTOS_ENABLE_IRQ();
            return (i | ARKRTOS_TIMER_HANDLE_FLAG);
        }
    }
    ARKRTOS_ENABLE_IRQ();
    return -1;
}

int32 arkrtos_timer_start_internal(int32 handle, uint32 period_ms, uint8 mode,
                                 void (*cb)(void *), void *arg)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;
    uint32 ticks;

    if(ARKRTOS_TIMER_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return -1;
    if(idx >= ARKRTOS_TIMER_NUM || arkrtos_timers[idx].used == 0)
        return -1;
    if(cb == 0 || period_ms == 0)
        return -1;
    if(mode != ARKRTOS_TIMER_MODE_ONESHOT && mode != ARKRTOS_TIMER_MODE_PERIODIC)
        return -1;

    ticks = ARKRTOS_MS_TO_TICK(period_ms);
    if(ticks == 0)
        ticks = 1;

    ARKRTOS_DISABLE_IRQ();
    arkrtos_timers[idx].mode         = mode;
    arkrtos_timers[idx].period_ticks = ticks;
    arkrtos_timers[idx].remain_ticks = ticks;
    arkrtos_timers[idx].expired      = 0;
    arkrtos_timers[idx].cb           = cb;
    arkrtos_timers[idx].arg          = arg;
    arkrtos_timers[idx].active       = 1;
    ARKRTOS_ENABLE_IRQ();
    return 0;
}

int32 arkrtos_timer_stop_internal(int32 handle)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;

    if(ARKRTOS_TIMER_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return -1;
    if(idx >= ARKRTOS_TIMER_NUM || arkrtos_timers[idx].used == 0)
        return -1;

    ARKRTOS_DISABLE_IRQ();
    arkrtos_timers[idx].active  = 0;
    arkrtos_timers[idx].expired = 0;
    ARKRTOS_ENABLE_IRQ();
    return 0;
}

int32 arkrtos_timer_delete_internal(int32 handle)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;

    if(ARKRTOS_TIMER_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return -1;
    if(idx >= ARKRTOS_TIMER_NUM)
        return -1;

    ARKRTOS_DISABLE_IRQ();
    arkrtos_timers[idx].used    = 0;
    arkrtos_timers[idx].active  = 0;
    arkrtos_timers[idx].expired = 0;
    arkrtos_timers[idx].name[0] = 0;
    arkrtos_timers[idx].cb      = 0;
    arkrtos_timers[idx].arg     = 0;
    ARKRTOS_ENABLE_IRQ();
    return 0;
}

#else   /* ARKRTOS_USE_TIMER == 0 */
/* Software timers compiled out.  Nothing is armed and every request is
 * refused, so a caller is told "no" instead of getting a timer id whose
 * callback will never run. */

void arkrtos_timer_module_init(void)
{
}

void arkrtos_timer_task_install(void)
{
}

void arkrtos_timer_tick_handler(void)
{
}

int32 arkrtos_timer_create_internal(char *name)
{
    (void)name;
    return -1;
}

int32 arkrtos_timer_start_internal(int32 handle, uint32 period_ms, uint8 mode,
                                 void (*cb)(void *), void *arg)
{
    (void)handle;
    (void)period_ms;
    (void)mode;
    (void)cb;
    (void)arg;
    return -1;
}

int32 arkrtos_timer_stop_internal(int32 handle)
{
    (void)handle;
    return -1;
}

int32 arkrtos_timer_delete_internal(int32 handle)
{
    (void)handle;
    return -1;
}

#endif /* ARKRTOS_USE_TIMER */
