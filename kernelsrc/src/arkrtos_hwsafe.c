/**
* @brief Ark RTOS 硬件安全态（"遗言"）机制实现
* @details 见 kernelsrc/include/arkrtos_hwsafe.h 的设计说明。
*          本文件只在 ARKRTOS_HW_SAFE=1 时编译出内容；开关关闭时不产生代码，
*          设备框架与任务终止路径上对本模块的所有调用点也都被同一开关挡住。
*/

#include "arkrtos_hwsafe.h"
#include "arkrtos_hal.h"     /* ARKRTOS_ENTER_CRITICAL / EXIT_CRITICAL */

#if ARKRTOS_HW_SAFE

/* ---------------------------------------------------------------- 表 */

typedef struct {
    char              name[8];
    arkrtos_hwsafe_fn fn;
} hwsafe_dev_t;

typedef struct {
    char               name[8];
    int32              owner;
    arkrtos_dev_hdr_t *obj;
    uint8              dead;
    uint8              has_will;   /* owner declared a ctrl() to run on death */
    uint8              handover;   /* this death is a planned hand-over */
    int32              will_cmd;
    int32              will_val;
} hwsafe_own_t;

static hwsafe_dev_t g_hwsafe_dev[ARKRTOS_HWSAFE_MAX_DEV];
static int32        g_hwsafe_dev_n;
static hwsafe_own_t g_hwsafe_own[ARKRTOS_HWSAFE_MAX_OWN];
static int32        g_hwsafe_own_n;
static uint32       g_hwsafe_neutralized;
static int32        g_hwsafe_last_owner;
/* Neutralization is observable per path, so a test can tell "the will ran"
 * from "the device callback ran" from "nothing was known to be safe" without
 * reading the source. Counters only: no console I/O on the fault path. */
static uint32       g_hwsafe_will_ran;
static uint32       g_hwsafe_cb_ran;
static uint32       g_hwsafe_none_ran;
/* Invoked but the driver said no: the hardware did NOT reach its safe state.
 * Kept as its own counter so it can never be read as a success. */
static uint32       g_hwsafe_failed;
/* Planned hand-over: ledger dropped on purpose, nothing was driven. */
static uint32       g_hwsafe_handover_skipped;

/* 设备名按 8 字节含结尾 0 比较（与 arkrtos_dev_desc_t.dev_name 同宽） */
static int32 hwsafe_name_eq(const char *a, const char *b)
{
    int32 i;
    for(i = 0; i < 8; i++)
    {
        if(a[i] != b[i])
        {
            return 0;
        }
        if(a[i] == 0)
        {
            return 1;
        }
    }
    return 1;
}

/* 最多拷 7 字符并补 0（超长直接截断，绝不越界） */
static void hwsafe_name_cpy(char *dst, const char *src)
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

/* void 清一条（保留 name 首字节为 0 作为空槽标记） */
static void hwsafe_own_clear(hwsafe_own_t *e)
{
    e->name[0]  = 0;
    e->owner    = 0;
    e->obj      = 0;
    e->dead     = 0;
    e->has_will = 0;
    e->handover = 0;
    e->will_cmd = 0;
    e->will_val = 0;
}

/* 移除第 idx 条：把最后一条搬过来补位（台账内部顺序无意义） */
static void hwsafe_own_remove(int32 idx)
{
    if(idx < 0 || idx >= g_hwsafe_own_n)
    {
        return;
    }
    g_hwsafe_own[idx] = g_hwsafe_own[g_hwsafe_own_n - 1];
    hwsafe_own_clear(&g_hwsafe_own[g_hwsafe_own_n - 1]);
    g_hwsafe_own_n--;
}

void arkrtos_hwsafe_reset(void)
{
    int32 i;
    for(i = 0; i < (int32)ARKRTOS_HWSAFE_MAX_DEV; i++)
    {
        g_hwsafe_dev[i].name[0] = 0;
        g_hwsafe_dev[i].fn      = 0;
    }
    for(i = 0; i < (int32)ARKRTOS_HWSAFE_MAX_OWN; i++)
    {
        hwsafe_own_clear(&g_hwsafe_own[i]);
    }
    g_hwsafe_dev_n       = 0;
    g_hwsafe_own_n       = 0;
    g_hwsafe_neutralized = 0u;
    g_hwsafe_last_owner  = 0;
}

/* ---------------------------------------------------------------- 回调登记 */

int32 arkrtos_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn)
{
    int32 i;

    if(dev_name == 0 || fn == 0)
    {
        return -2;
    }
    /* 同名重复登记：报错而不是静默覆盖——两个驱动对同一台设备给出两套安全态
     * 是配置错误，静默留下其中一个会让"按哪个走"变成不可解释的行为。 */
    for(i = 0; i < g_hwsafe_dev_n; i++)
    {
        if(hwsafe_name_eq(g_hwsafe_dev[i].name, dev_name))
        {
            return -2;
        }
    }
    if(g_hwsafe_dev_n >= (int32)ARKRTOS_HWSAFE_MAX_DEV)
    {
        return -1;
    }
    hwsafe_name_cpy(g_hwsafe_dev[g_hwsafe_dev_n].name, dev_name);
    g_hwsafe_dev[g_hwsafe_dev_n].fn = fn;
    g_hwsafe_dev_n++;
    return 0;
}

static arkrtos_hwsafe_fn hwsafe_find_fn(const char *dev_name)
{
    int32 i;
    for(i = 0; i < g_hwsafe_dev_n; i++)
    {
        if(hwsafe_name_eq(g_hwsafe_dev[i].name, dev_name))
        {
            return g_hwsafe_dev[i].fn;
        }
    }
    return 0;
}

/* find the device's callback-table slot; -1 when not registered */
static int32 hwsafe_find_dev_idx(const char *dev_name)
{
    int32 i;
    for(i = 0; i < g_hwsafe_dev_n; i++)
    {
        if(hwsafe_name_eq(g_hwsafe_dev[i].name, dev_name))
        {
            return i;
        }
    }
    return -1;
}

int32 arkrtos_hwsafe_unregister(const char *dev_name)
{
    int32 idx;

    if(dev_name == 0)
    {
        return -1;
    }
    idx = hwsafe_find_dev_idx(dev_name);
    if(idx < 0)
    {
        return -1;                  /* no such registration: say so, do not fake */
    }
    g_hwsafe_dev[idx] = g_hwsafe_dev[g_hwsafe_dev_n - 1];
    g_hwsafe_dev[g_hwsafe_dev_n - 1].name[0] = 0;
    g_hwsafe_dev[g_hwsafe_dev_n - 1].fn      = 0;
    g_hwsafe_dev_n--;
    return 0;
}

/* ---------------------------------------------------------------- 台账 */

void arkrtos_hwsafe_claim(const char *dev_name, int32 owner, arkrtos_dev_hdr_t *obj)
{
    int32 i;
    uint32 state;

    if(dev_name == 0 || owner <= 0)
    {
        return;                     /* 内核/空闲上下文打开的不记名 */
    }

    state = ARKRTOS_ENTER_CRITICAL();

    for(i = 0; i < g_hwsafe_own_n; i++)
    {
        if(g_hwsafe_own[i].owner == owner &&
           hwsafe_name_eq(g_hwsafe_own[i].name, dev_name))
        {
            g_hwsafe_own[i].obj  = obj;      /* 重复 open：刷新实例即可 */
            g_hwsafe_own[i].dead = 0;
            ARKRTOS_EXIT_CRITICAL(state);
            return;
        }
    }

    if(g_hwsafe_own_n < (int32)ARKRTOS_HWSAFE_MAX_OWN)
    {
        hwsafe_name_cpy(g_hwsafe_own[g_hwsafe_own_n].name, dev_name);
        g_hwsafe_own[g_hwsafe_own_n].owner    = owner;
        g_hwsafe_own[g_hwsafe_own_n].obj      = obj;
        g_hwsafe_own[g_hwsafe_own_n].dead     = 0;
        g_hwsafe_own[g_hwsafe_own_n].has_will = 0;
        g_hwsafe_own[g_hwsafe_own_n].handover = 0;
        g_hwsafe_own[g_hwsafe_own_n].will_cmd = 0;
        g_hwsafe_own[g_hwsafe_own_n].will_val = 0;
        g_hwsafe_own_n++;
    }
    /* 表满：宁可不记，也不覆盖别人的条目——覆盖会把另一台设备的安全态弄丢。
     * 这条只能靠调用方把 ARKRTOS_HWSAFE_MAX_OWN 配够。 */

    ARKRTOS_EXIT_CRITICAL(state);
}

void arkrtos_hwsafe_release(const char *dev_name, int32 owner)
{
    int32 i;
    uint32 state;

    if(dev_name == 0 || owner <= 0)
    {
        return;
    }

    state = ARKRTOS_ENTER_CRITICAL();
    for(i = 0; i < g_hwsafe_own_n; i++)
    {
        if(g_hwsafe_own[i].owner == owner &&
           hwsafe_name_eq(g_hwsafe_own[i].name, dev_name))
        {
            hwsafe_own_remove(i);
            break;
        }
    }
    ARKRTOS_EXIT_CRITICAL(state);
}

/* find the (dev_name, owner) ledger slot; -1 when absent. Caller holds the
 * critical section. */
static int32 hwsafe_find_own_idx(const char *dev_name, int32 owner)
{
    int32 i;
    for(i = 0; i < g_hwsafe_own_n; i++)
    {
        if(g_hwsafe_own[i].owner == owner &&
           hwsafe_name_eq(g_hwsafe_own[i].name, dev_name))
        {
            return i;
        }
    }
    return -1;
}

int32 arkrtos_hwsafe_set_will(const char *dev_name, int32 owner,
                              int32 cmd, int32 val)
{
    int32 idx;
    uint32 state;

    if(dev_name == 0 || owner <= 0)
    {
        return -2;
    }

    state = ARKRTOS_ENTER_CRITICAL();
    idx = hwsafe_find_own_idx(dev_name, owner);
    if(idx < 0)
    {
        /* The owner does not hold this device: refuse. Otherwise any task
         * could leave a "will" on somebody else's device - that is not a
         * will, that is ordering another owner's hardware around. */
        ARKRTOS_EXIT_CRITICAL(state);
        return -1;
    }
    g_hwsafe_own[idx].has_will = 1;
    g_hwsafe_own[idx].will_cmd = cmd;
    g_hwsafe_own[idx].will_val = val;
    ARKRTOS_EXIT_CRITICAL(state);
    return 0;
}

int32 arkrtos_hwsafe_clear_will(const char *dev_name, int32 owner)
{
    int32 idx;
    uint32 state;

    if(dev_name == 0 || owner <= 0)
    {
        return -1;
    }

    state = ARKRTOS_ENTER_CRITICAL();
    idx = hwsafe_find_own_idx(dev_name, owner);
    if(idx < 0)
    {
        ARKRTOS_EXIT_CRITICAL(state);
        return -1;
    }
    g_hwsafe_own[idx].has_will = 0;
    g_hwsafe_own[idx].will_cmd = 0;
    g_hwsafe_own[idx].will_val = 0;
    ARKRTOS_EXIT_CRITICAL(state);
    return 0;
}

/* ---------------------------------------------------------------- 两阶段中和 */

void arkrtos_hwsafe_on_death_reason(int32 task_id, int32 handover)
{
    int32 i;
    uint32 state;

    if(task_id <= 0)
    {
        return;
    }

    state = ARKRTOS_ENTER_CRITICAL();
    for(i = 0; i < g_hwsafe_own_n; i++)
    {
        if(g_hwsafe_own[i].owner == task_id)
        {
            g_hwsafe_own[i].dead     = 1;
            g_hwsafe_own[i].handover = (handover != 0) ? 1u : 0u;
        }
    }
    ARKRTOS_EXIT_CRITICAL(state);
}

void arkrtos_hwsafe_on_death(int32 task_id)
{
    arkrtos_hwsafe_on_death_reason(task_id, 0);
}

int32 arkrtos_hwsafe_run_pending(void)
{
    int32 i = 0;
    int32 ran = 0;

    while(i < g_hwsafe_own_n)
    {
        if(g_hwsafe_own[i].dead != 0u)
        {
            arkrtos_hwsafe_fn fn = hwsafe_find_fn(g_hwsafe_own[i].name);
            arkrtos_dev_hdr_t *obj = g_hwsafe_own[i].obj;
            int32 owner = g_hwsafe_own[i].owner;

            /* Priority: the owner's will (a specific ctrl on the device it
             * held) beats the device's generic registered callback. Neither
             * needs code from the dead App: the kernel drives the device
             * through its resident driver. */
            /* 没有登记安全态的设备：只能移除条目。这不是"已经安全"，
             * 而是"驱动没告诉内核怎么让它安全"——由 docs 明说，不在这里假装成功。 */
            if(g_hwsafe_own[i].handover != 0u)
            {
                /* Planned hand-over: the successor is taking over, so
                 * driving the hardware to its safe state here would be a
                 * glitch, not safety. Drop the entry, drive nothing, and
                 * keep it OUT of the neutralized count. */
                g_hwsafe_handover_skipped++;
                hwsafe_own_remove(i);
                continue;
            }
            if((g_hwsafe_own[i].has_will != 0u) && (obj != 0))
            {
                /* The counter follows the DRIVER's answer. A will the
                 * driver refused did not bring the hardware to its safe
                 * state, and counting it as "will ran" would be exactly
                 * the confident-but-wrong answer this kernel refuses. */
                if(arkrtos_dev_ctrl_obj(obj,
                                        (uint32)g_hwsafe_own[i].will_cmd,
                                        (uint32)g_hwsafe_own[i].will_val) == 0)
                {
                    g_hwsafe_will_ran++;
                }
                else
                {
                    g_hwsafe_failed++;
                }
                ran++;
            }
            else if(fn != 0 && obj != 0)
            {
                if(fn(obj, (uint32)owner) == 0)
                {
                    g_hwsafe_cb_ran++;
                }
                else
                {
                    g_hwsafe_failed++;
                }
                ran++;
            }
            else
            {
                g_hwsafe_none_ran++;
            }
            g_hwsafe_neutralized++;
            g_hwsafe_last_owner = owner;
            hwsafe_own_remove(i);
            continue;                       /* remove 把最后一条搬了过来，idx 不前进 */
        }
        i++;
    }
    return ran;
}

/* ---------------------------------------------------------------- 观测 */

int32 arkrtos_hwsafe_owner_count(void)
{
    return g_hwsafe_own_n;
}

int32 arkrtos_hwsafe_dev_count(void)
{
    return g_hwsafe_dev_n;
}

int32 arkrtos_hwsafe_info(int32 idx, char *name_out, uint32 max,
                          int32 *owner_out, int32 *dead_out)
{
    uint32 k;

    if(idx < 0 || idx >= g_hwsafe_own_n)
    {
        return -1;
    }
    if(name_out != 0 && max > 0u)
    {
        for(k = 0; k + 1u < max; k++)
        {
            name_out[k] = g_hwsafe_own[idx].name[k];
            if(name_out[k] == 0)
            {
                break;
            }
        }
        name_out[max - 1u] = 0;
    }
    if(owner_out != 0)
    {
        *owner_out = g_hwsafe_own[idx].owner;
    }
    if(dead_out != 0)
    {
        *dead_out = (int32)g_hwsafe_own[idx].dead;
    }
    return 0;
}

int32 arkrtos_hwsafe_dev_at(int32 idx, char *name_out, uint32 max)
{
    uint32 k;

    if(idx < 0 || idx >= g_hwsafe_dev_n)
    {
        return -1;
    }
    if(name_out != 0 && max > 0u)
    {
        for(k = 0; k + 1u < max; k++)
        {
            name_out[k] = g_hwsafe_dev[idx].name[k];
            if(name_out[k] == 0)
            {
                break;
            }
        }
        name_out[max - 1u] = 0;
    }
    return 0;
}

int32 arkrtos_hwsafe_registered(const char *dev_name)
{
    if(dev_name == 0)
    {
        return 0;
    }
    return (hwsafe_find_fn(dev_name) != 0) ? 1 : 0;
}

int32 arkrtos_hwsafe_will(int32 idx, int32 *has_will,
                          int32 *cmd_out, int32 *val_out)
{
    if(idx < 0 || idx >= g_hwsafe_own_n)
    {
        return -1;
    }
    if(has_will != 0)
    {
        *has_will = (int32)g_hwsafe_own[idx].has_will;
    }
    if(cmd_out != 0)
    {
        *cmd_out = g_hwsafe_own[idx].will_cmd;
    }
    if(val_out != 0)
    {
        *val_out = g_hwsafe_own[idx].will_val;
    }
    return 0;
}

uint32 arkrtos_hwsafe_neutralized_count(void)
{
    return g_hwsafe_neutralized;
}

int32 arkrtos_hwsafe_last_owner(void)
{
    return g_hwsafe_last_owner;
}

void arkrtos_hwsafe_path_counts(uint32 *will_out, uint32 *fail_out, uint32 *cb_out,
                                  uint32 *none_out, uint32 *handover_out)
{
    if(will_out != 0)
    {
        *will_out = g_hwsafe_will_ran;
    }
    if(fail_out != 0)
    {
        *fail_out = g_hwsafe_failed;
    }
    if(cb_out != 0)
    {
        *cb_out = g_hwsafe_cb_ran;
    }
    if(none_out != 0)
    {
        *none_out = g_hwsafe_none_ran;
    }
    if(handover_out != 0)
    {
        *handover_out = g_hwsafe_handover_skipped;
    }
}

#else  /* ARKRTOS_HW_SAFE == 0 */

/* 功能关闭：本模块不产生任何代码。留一条声明让翻译单元在 ISO C 下合法
 * （空翻译单元是未定义行为），不占 ROM / RAM，也不改变任何行为。 */
typedef int arkrtos_hwsafe_not_built_t;

#endif /* ARKRTOS_HW_SAFE */
