/**
* @file arkrtos_waitmulti.c
* @brief 多对象同时就绪探测的实现
* @details 只做一次快照 + 判定，不登记等待者、不消费令牌、不阻塞。
*          每个类型的「就绪」定义由各自模块回答（sem/mtx 在 arkrtos_sync.c，
*          mq 在 arkrtos_mq.c，event 在 arkrtos_event.c），本文件只负责
*          句柄分派与 ANY/ALL 判定，避免两套「什么算就绪」的口径漂移。
* @author xw
* @date 2026.11.14
*/

#include "arkrtos_waitmulti.h"
#include "arkrtos_sync.h"
#include "arkrtos_mq.h"
#include "arkrtos_event.h"

#if (ARKRTOS_USE_WAITMULTI == 1)

/* 单个句柄的就绪判定。返回 1=就绪，0=未就绪，-1=句柄不可用（类型不支持
 * 或对象已不存在）。任何一条不可用都让整组探测报 -1：给出一半答案会让
 * 调用方按位去取时撞到无效句柄，比直接拒绝更糟。 */
static int32 arkrtos_wm_one(uint32 handle)
{
    switch(handle & ARKRTOS_HANDLE_MASK)
    {
        case ARKRTOS_SEM_HANDLE_FLAG:
            return arkrtos_sem_ready_internal((int32)handle);
        case ARKRTOS_MTX_HANDLE_FLAG:
            return arkrtos_mtx_ready_internal((int32)handle);
        case ARKRTOS_MQ_HANDLE_FLAG:
            return arkrtos_mq_ready_internal((int32)handle);
        case ARKRTOS_EVENT_HANDLE_FLAG:
            return arkrtos_event_ready_internal((int32)handle);
        default:
            return -1;
    }
}

int32 arkrtos_wm_probe_internal(const uint32 *handles, uint32 n,
                                uint32 opt, uint32 *ready_mask)
{
    uint32 i;
    uint32 mask = 0u;
    uint32 want;

    if(handles == 0 || ready_mask == 0)
    {
        return -1;
    }
    if(n == 0u || n > (uint32)ARKRTOS_WAITMULTI_MAX_OBJS)
    {
        return -1;
    }
    if((opt != ARKRTOS_WM_OPT_ANY) && (opt != ARKRTOS_WM_OPT_ALL))
    {
        return -1;
    }

    /* 一次临界区里取完整张快照：跨对象的「同时」本来只能是近似，
     * 但至少不要让本函数自己在扫描途中被中断改写。 */
    ARKRTOS_DISABLE_IRQ();
    for(i = 0u; i < n; i++)
    {
        int32 r = arkrtos_wm_one(handles[i]);

        if(r < 0)
        {
            ARKRTOS_ENABLE_IRQ();
            *ready_mask = 0u;
            return -1;
        }
        if(r == 1)
        {
            mask |= (1u << i);
        }
    }
    ARKRTOS_ENABLE_IRQ();

    *ready_mask = mask;

    want = 0u;
    for(i = 0u; i < n; i++)
    {
        want |= (1u << i);
    }

    if(opt == ARKRTOS_WM_OPT_ALL)
    {
        return (mask == want) ? 0 : -2;
    }

    return (mask != 0u) ? 0 : -2;
}

#endif /* ARKRTOS_USE_WAITMULTI == 1 */
