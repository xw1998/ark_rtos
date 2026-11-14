/**
* @file arkrtos_eset.c
* @brief Ark RTOS 32 位事件集实现
* @details 位掩码等待（OR / AND）+ 累加式置位 + 可选清除。
*          创建 / 等待 / 置位 / 清除 / 查询 / 删除 六条路径，
*          以及任务下线收尸。语义与边界见 arkrtos_eset.h 文件头。
*
*          为什么单独一个模块而不是扩 arkrtos_event：
*            1) arkrtos_event 的"单标志 + 广播 + 自动复位"是被 POSIX 门面、
*               驱动框架和小程序依赖的现成语义，改它会牵动所有既有调用点；
*            2) 新模块默认关（ARKRTOS_USE_ESET=0），关掉时本文件不产生任何
*               符号与字节，体积零变化；
*            3) 两套对象各自一张表，出错时不会把"谁把谁的位清了"混在一起。
* @author xw
* @date 2026.11.14
*/

#include "arkrtos_eset.h"
#include "arkrtos_hal.h"
#include "arkrtos_cfg.h"
#include "arkrtos_trace.h"

#if (ARKRTOS_USE_ESET == 1)

/** 对象表。宿主门禁（-DARKRTOS_ESET_HOST_TEST）把它导出，好让测试直接摆
 *  两个等待槽，验证「谁先拿」「带 CLEAR 的位只交给一个等待者」这类判定；
 *  默认构建里它就是本文件内部的 static。 */
#if defined(ARKRTOS_ESET_HOST_TEST)
arkrtos_eset_obj_t arkrtos_esets[ARKRTOS_ESET_NUM];
#else
static arkrtos_eset_obj_t arkrtos_esets[ARKRTOS_ESET_NUM];
#endif

/* 这个等待者的条件成立了吗：OR 看"至少一位"，AND 看"所有位"。 */
static uint8 arkrtos_eset_satisfied(uint32 flags, uint32 mask, uint32 opt)
{
    if((opt & ARKRTOS_ESET_OPT_AND) != 0u)
    {
        return ((flags & mask) == mask) ? 1u : 0u;
    }
    return ((flags & mask) != 0u) ? 1u : 0u;
}

/* 登记等待者。返回 -2 = 已在队列里（就地更新掩码与选项），其它负值 = 满。 */
static int32 arkrtos_eset_waiter_add(arkrtos_eset_waiter_t *waiters,
                                    arkrtos_task_t *p_tsk, uint32 mask, uint32 opt)
{
    int32 j;

    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        if(waiters[j].task == p_tsk)
        {
            /* 用户态轮询每拍都会重新登记一次；掩码和选项跟着最新一次走，
             * 但已经交接完成（woken==1）的那一槽不许被覆盖——那个返回值
             * 是 set 算定要交给它的。 */
            if(waiters[j].woken == 0u)
            {
                waiters[j].mask = mask;
                waiters[j].opt  = opt;
            }
            return ARKRTOS_ESET_WAITER_DUP;
        }
    }
    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        if(waiters[j].task == 0)
        {
            waiters[j].task  = p_tsk;
            waiters[j].mask  = mask;
            waiters[j].opt   = opt;
            waiters[j].woken = 0u;
            waiters[j].got   = 0u;
            return 0;
        }
    }
    return ARKRTOS_ESET_ERR_NOSPACE;
}

static int32 arkrtos_eset_waiter_remove(arkrtos_eset_waiter_t *waiters,
                                       arkrtos_task_t *p_tsk)
{
    int32 j;
    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        if(waiters[j].task == p_tsk)
        {
            waiters[j].task  = 0;
            waiters[j].mask  = 0u;
            waiters[j].opt   = 0u;
            waiters[j].woken = 0u;
            waiters[j].got   = 0u;
            return 0;
        }
    }
    return ARKRTOS_ESET_WAITER_ABSENT;
}

/* set / delete 唤醒单个等待者：置 READY + 交班位，与 arkrtos_event.c 同一套写法。 */
static void arkrtos_eset_ready(int32 idx, arkrtos_task_t *p_w)
{
    p_w->wait_time   = 0;
    p_w->wake_reason = ARKRTOS_WAKE_HANDOFF;
    p_w->status      = ARKRTOS_TASK_READY;
    arkrtos_ready_add(ARKRTOS_TASK_IDX(p_w));
    mdk_trace_arkrtos_obj_signal((uint16)idx, (uint8)MDK_TRACE_ARKRTOS_CLASS_EVENT);
}

void arkrtos_eset_module_init(void)
{
    int32 i;
    for(i = 0; i < ARKRTOS_ESET_NUM; i++)
    {
        int32 j;
        for(j = 0; j < 16; j++)
            arkrtos_esets[i].name[j] = 0;
        arkrtos_esets[i].used       = 0;
        arkrtos_esets[i].flags      = 0u;
        arkrtos_esets[i].creator_id = 0;
        for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
        {
            arkrtos_esets[i].waiters[j].task  = 0;
            arkrtos_esets[i].waiters[j].mask  = 0u;
            arkrtos_esets[i].waiters[j].opt   = 0u;
            arkrtos_esets[i].waiters[j].woken = 0u;
            arkrtos_esets[i].waiters[j].got   = 0u;
        }
    }
}

/* 任务下线收尸：摘掉该任务在所有事件集里的等待槽，并回收它创建的对象。
 * 调用方已持临界区（本函数不做关中断）。 */
void arkrtos_eset_release_task(int32 task_id)
{
    arkrtos_task_t *p_tsk;
    int32 i;
    int32 j;

    if(task_id <= 0 || task_id > arkrtos_task_count)
    {
        return;
    }

    p_tsk = &arkrtos_task_table[task_id - 1];

    for(i = 0; i < ARKRTOS_ESET_NUM; i++)
    {
        if(arkrtos_esets[i].used == 0)
        {
            continue;
        }
        (void)arkrtos_eset_waiter_remove(arkrtos_esets[i].waiters, p_tsk);
    }

    /* 死掉的任务创建的对象随之消失：表很小，一个每次运行都建一次的 App
     * 否则几轮 start / stop 就把表用光。内核创建的对象 creator_id==0，留着。
     * 等待槽直接清空而不唤醒——这里在拆除路径上，能等在这个 App 自己
     * 创建的对象上的，只可能是它自己的任务（与 arkrtos_event 同一条理由）。 */
    for(i = 0; i < ARKRTOS_ESET_NUM; i++)
    {
        if((arkrtos_esets[i].used != 0) && (arkrtos_esets[i].creator_id == task_id))
        {
            for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
            {
                arkrtos_esets[i].waiters[j].task  = 0;
                arkrtos_esets[i].waiters[j].mask  = 0u;
                arkrtos_esets[i].waiters[j].opt   = 0u;
                arkrtos_esets[i].waiters[j].woken = 0u;
                arkrtos_esets[i].waiters[j].got   = 0u;
            }
            arkrtos_esets[i].used       = 0;
            arkrtos_esets[i].flags      = 0u;
            arkrtos_esets[i].name[0]    = 0;
            arkrtos_esets[i].creator_id = 0;
        }
    }
}

int32 arkrtos_eset_create_internal(char *name)
{
    int32 i;
    int32 idx = -1;

    if(name == 0)
    {
        return ARKRTOS_ESET_ERR_PARAM;
    }

    ARKRTOS_DISABLE_IRQ();
    for(i = 0; i < ARKRTOS_ESET_NUM; i++)
    {
        if(arkrtos_esets[i].used == 0)
        {
            int32 j;
            for(j = 0; j < 15; j++)
            {
                arkrtos_esets[i].name[j] = name[j];
                if(name[j] == 0)
                    break;
            }
            arkrtos_esets[i].name[15]   = 0;
            arkrtos_esets[i].used       = 1;
            arkrtos_esets[i].flags      = 0u;
            arkrtos_esets[i].creator_id = (arkrtos_current_task_id > 0)
                                        ? arkrtos_current_task_id : 0;
            for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
            {
                arkrtos_esets[i].waiters[j].task  = 0;
                arkrtos_esets[i].waiters[j].mask  = 0u;
                arkrtos_esets[i].waiters[j].opt   = 0u;
                arkrtos_esets[i].waiters[j].woken = 0u;
                arkrtos_esets[i].waiters[j].got   = 0u;
            }
            idx = i;
            break;
        }
    }
    ARKRTOS_ENABLE_IRQ();

    if(idx < 0)
    {
        return ARKRTOS_ESET_ERR_NOSLOT;
    }
    return (idx | ARKRTOS_ESET_HANDLE_FLAG);
}

int32 arkrtos_eset_wait_internal(int32 handle, uint32 mask, uint32 opt,
                                 uint32 *p_got, int32 timeout_ms)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;
    arkrtos_task_t *p_tsk;
    int32 reason;
    uint32 got;

    if(ARKRTOS_ESET_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return ARKRTOS_ESET_ERR_HANDLE;
    if(idx >= ARKRTOS_ESET_NUM || arkrtos_esets[idx].used == 0)
        return ARKRTOS_ESET_ERR_STATE;
    if((mask == 0u) || (p_got == 0))
        return ARKRTOS_ESET_ERR_PARAM;
    /* OR / AND 必须二选一：两个都给或都不给，等待条件就没有确定含义 */
    if(((opt & ARKRTOS_ESET_OPT_OR) != 0u) == ((opt & ARKRTOS_ESET_OPT_AND) != 0u))
        return ARKRTOS_ESET_ERR_OPT;
    opt &= (ARKRTOS_ESET_OPT_OR | ARKRTOS_ESET_OPT_AND | ARKRTOS_ESET_OPT_CLEAR);

    ARKRTOS_DISABLE_IRQ();

    /* 已经满足：就地交接。累加式的位不会"过期"，所以先 set 后 wait 也不丢。 */
    if(arkrtos_eset_satisfied(arkrtos_esets[idx].flags, mask, opt) != 0u)
    {
        got = arkrtos_esets[idx].flags & mask;
        if((opt & ARKRTOS_ESET_OPT_CLEAR) != 0u)
        {
            arkrtos_esets[idx].flags &= ~got;
        }
        ARKRTOS_ENABLE_IRQ();
        *p_got = got;
        return 0;
    }

    /* timeout==0 = 只试一次：不登记、不阻塞，也顺手清掉残留登记
     * （否则 set 会去唤醒一个其实已经不在等的任务）。 */
    if(timeout_ms == 0)
    {
        p_tsk = arkrtos_task_get_current();
        if(p_tsk != 0)
        {
            (void)arkrtos_eset_waiter_remove(arkrtos_esets[idx].waiters, p_tsk);
        }
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SYNC_ERR_TIMEOUT;
    }

    /* 用户态（SVC 里）不做登记：位是累加的，轮询入口自己判条件，
     * 条件成立时自己消费。这样就不需要在异常里把一个任务置 READY。 */
    if(arkrtos_sync_in_handler() != 0u)
    {
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SYNC_ERR_WOULDBLOCK;
    }

    p_tsk = arkrtos_task_get_current();
    if(p_tsk == 0)
    {
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_ESET_ERR_CONTEXT;
    }

    {
        int32 added = arkrtos_eset_waiter_add(arkrtos_esets[idx].waiters, p_tsk, mask, opt);
        if((added < 0) && (added != ARKRTOS_ESET_WAITER_DUP))
        {
            ARKRTOS_ENABLE_IRQ();
            return ARKRTOS_ESET_ERR_NOSPACE;
        }
    }

    /* 登记与置 WAIT 同处一个临界区（关中断进入、关中断返回），消除丢唤醒窗口 */
    arkrtos_trace_wait_obj(MDK_TRACE_ARKRTOS_OBJ(MDK_TRACE_ARKRTOS_CLASS_EVENT, idx));
    reason = arkrtos_task_block_in_critical((uint32)timeout_ms);

    if(reason < 0)
    {
        (void)arkrtos_eset_waiter_remove(arkrtos_esets[idx].waiters, p_tsk);
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_ESET_ERR_CONTEXT;
    }

    if(reason == ARKRTOS_WAKE_OBJ_DELETED)
    {
        (void)arkrtos_eset_waiter_remove(arkrtos_esets[idx].waiters, p_tsk);
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_ESET_ERR_DELETED;
    }

    /* 只有 set 会给等待者带上交班位；没有它就不是"条件被满足"的唤醒 */
    if((reason == ARKRTOS_WAKE_TIMEOUT) || ((reason & ARKRTOS_WAKE_HANDOFF) == 0))
    {
        (void)arkrtos_eset_waiter_remove(arkrtos_esets[idx].waiters, p_tsk);
        ARKRTOS_ENABLE_IRQ();
        return ARKRTOS_SYNC_ERR_TIMEOUT;
    }

    /* 交班那一刻算定的返回值存在自己的槽里；醒过来只读它，不重算。
     * CLEAR 已由 set 在交接时清掉，这里不能再清一遍：中途新到的同一位
     * 会被这次清理误伤。 */
    {
        int32 j;
        got = 0u;
        for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
        {
            if(arkrtos_esets[idx].waiters[j].task == p_tsk)
            {
                got = arkrtos_esets[idx].waiters[j].got;
                (void)arkrtos_eset_waiter_remove(arkrtos_esets[idx].waiters, p_tsk);
                break;
            }
        }
    }

    ARKRTOS_ENABLE_IRQ();
    *p_got = got;
    return 0;
}

int32 arkrtos_eset_set_internal(int32 handle, uint32 mask)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;
    int32 j;
    int32 woke = 0;

    if(ARKRTOS_ESET_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return ARKRTOS_ESET_ERR_HANDLE;
    if(idx >= ARKRTOS_ESET_NUM || arkrtos_esets[idx].used == 0)
        return ARKRTOS_ESET_ERR_STATE;
    if(mask == 0u)
        return ARKRTOS_ESET_ERR_PARAM;

    ARKRTOS_DISABLE_IRQ();

    /* 累加：没有等待者时也留在那里，下一次 wait 直接命中 */
    arkrtos_esets[idx].flags |= mask;

    /* 逐个判定，顺序固定（槽位从 0 到 N-1）。带 CLEAR 的等待者会把交给
     * 它的位从 flags 里拿走，所以排在前面的先拿——同一个位只交给一个人，
     * 这是"只消费一次"在 CLEAR 语义下的落地方式。 */
    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        arkrtos_eset_waiter_t *p_w = &arkrtos_esets[idx].waiters[j];
        uint32 got;

        if((p_w->task == 0) || (p_w->woken != 0u))
        {
            continue;
        }
        if(arkrtos_eset_satisfied(arkrtos_esets[idx].flags, p_w->mask, p_w->opt) == 0u)
        {
            continue;
        }

        got = arkrtos_esets[idx].flags & p_w->mask;
        if((p_w->opt & ARKRTOS_ESET_OPT_CLEAR) != 0u)
        {
            arkrtos_esets[idx].flags &= ~got;
        }
        p_w->got   = got;
        p_w->woken = 1u;

        arkrtos_eset_ready(idx, p_w->task);
        woke++;
    }

    ARKRTOS_ENABLE_IRQ();

    if(woke != 0)
    {
        ARKRTOS_SWITCH_TASK();
    }
    return 0;
}

int32 arkrtos_eset_clear_internal(int32 handle, uint32 mask)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;

    if(ARKRTOS_ESET_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return ARKRTOS_ESET_ERR_HANDLE;
    if(idx >= ARKRTOS_ESET_NUM || arkrtos_esets[idx].used == 0)
        return ARKRTOS_ESET_ERR_STATE;

    ARKRTOS_DISABLE_IRQ();
    arkrtos_esets[idx].flags &= ~mask;
    ARKRTOS_ENABLE_IRQ();
    return 0;
}

int32 arkrtos_eset_get_internal(int32 handle, uint32 *p_flags)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;

    if(ARKRTOS_ESET_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return ARKRTOS_ESET_ERR_HANDLE;
    if(idx >= ARKRTOS_ESET_NUM || arkrtos_esets[idx].used == 0)
        return ARKRTOS_ESET_ERR_STATE;
    if(p_flags == 0)
        return ARKRTOS_ESET_ERR_PARAM;

    ARKRTOS_DISABLE_IRQ();
    *p_flags = arkrtos_esets[idx].flags;
    ARKRTOS_ENABLE_IRQ();
    return 0;
}

int32 arkrtos_eset_delete_internal(int32 handle)
{
    int32 idx = handle & ARKRTOS_HANDLE_RELMASK;
    int32 j;
    int32 woke = 0;

    if(ARKRTOS_ESET_HANDLE_FLAG != (handle & ARKRTOS_HANDLE_MASK))
        return ARKRTOS_ESET_ERR_HANDLE;
    if(idx >= ARKRTOS_ESET_NUM || arkrtos_esets[idx].used == 0)
        return ARKRTOS_ESET_ERR_STATE;

    ARKRTOS_DISABLE_IRQ();

    /* 删除前必须把所有等待者放走（OBJ_DELETED），否则它们会永远睡下去 */
    for(j = 0; j < ARKRTOS_MAX_ESET_WAITERS; j++)
    {
        arkrtos_eset_waiter_t *p_w = &arkrtos_esets[idx].waiters[j];

        if(p_w->task != 0)
        {
            p_w->got          = 0u;
            p_w->woken        = 1u;
            p_w->task->wait_time   = 0;
            p_w->task->wake_reason = ARKRTOS_WAKE_OBJ_DELETED;
            p_w->task->status      = ARKRTOS_TASK_READY;
            arkrtos_ready_add(ARKRTOS_TASK_IDX(p_w->task));
            p_w->task  = 0;
            p_w->mask  = 0u;
            p_w->opt   = 0u;
            p_w->woken = 0u;
            woke++;
        }
    }

    arkrtos_esets[idx].used       = 0;
    arkrtos_esets[idx].flags      = 0u;
    arkrtos_esets[idx].name[0]    = 0;
    arkrtos_esets[idx].creator_id = 0;

    ARKRTOS_ENABLE_IRQ();

    if(woke != 0)
    {
        ARKRTOS_SWITCH_TASK();
    }
    return 0;
}

#endif /* ARKRTOS_USE_ESET == 1 */
