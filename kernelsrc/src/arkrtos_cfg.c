/**
* @brief Ark RTOS 任务配置加载模块
* @details 管理任务表和任务栈初始化。
*          栈帧格式由port层的 arkrtos_port_stack_init() 实现，
*          内核不再硬编码任何架构特定的寄存器布局。
* @author xw
* @date 2026.05.03
*/

#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_hal.h"
#include "arkrtos_config.h"
#include "arkrtos_fault.h"
#include "arkrtos_mpu.h"
#include "arkrtos_init.h"

arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32 arkrtos_task_count = 0;

/* 任务表 RAM 预算守护：把 ARKRTOS_TASK_MAX_NUM 调得过大时直接编译报错，
 * 避免静态 TCB 表悄悄吃掉内核 RAM。预算见 arkrtos_config.h 的
 * ARKRTOS_TASK_TABLE_RAM_MAX。 */
typedef char arkrtos_task_table_ram_check[
    (sizeof(arkrtos_task_table) <= (ARKRTOS_TASK_TABLE_RAM_MAX)) ? 1 : -1];

void arkrtos_cfg_load(void)
{
    arkrtos_task_count = 0;
    /* 就绪集与延时链归零：静态 TCB 数组默认是 0，而 0 是合法任务下标，
     * 不能当“不在链上”用，所以必须显式置成 ARKRTOS_TASK_NIL。 */
    arkrtos_ready_reset();
}

/* 注册任务的通用辅助：填充 TCB 缺省字段、初始化栈帧并加入任务表。
 * 供应用 main 与内核内置任务（定时器服务）共用。返回 1 起始任务号，失败返回 -1。 */
int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                          uint8 priority, uint32 period_ms)
{
    arkrtos_task_t *p_task;
    int32 free_idx = -1;
    int32 i;

    /* Slot allocation: reuse a released task slot before growing the table.
     * Without this, every App start/stop pair permanently consumes one
     * task slot, and a few dozen cycles exhaust the whole table.
     * Only INVALID slots without a pending recovery flag are reusable
     * (arkrtos_task_recover_mark sets INVALID + recover_pending=1, and such
     * a slot will be revived by arkrtos_task_recover_pending later).
     * Registration failures are always recorded, so a task that never
     * started can be diagnosed through arkrtos_fault_record_read(). */
    for(i = 0; i < arkrtos_task_count; i++)
    {
        if((arkrtos_task_table[i].status == ARKRTOS_TASK_INVALID) &&
           (arkrtos_task_table[i].recover_pending == 0))
        {
            free_idx = i;
            break;
        }
    }

    if(free_idx < 0)
    {
        if(arkrtos_task_count < ARKRTOS_TASK_MAX_NUM)
        {
            free_idx = arkrtos_task_count;
            arkrtos_task_count++;
        }
        else
        {
            arkrtos_fault_record(ARKRTOS_FAULT_NOSLOT, 0);
            return -1;
        }
    }

    if((entry == 0) || (stack_bottom == 0) || (stack_size < 64u))
    {
        arkrtos_fault_record(ARKRTOS_FAULT_NOSLOT, 0);
        return -1;
    }

    stack_bottom[0] = ARKRTOS_STACK_END_FLAG_VAL;

    p_task = &arkrtos_task_table[free_idx];
    p_task->ram_start   = (uint32)stack_bottom;
    p_task->ram_size    = stack_size;
    p_task->stack_size  = stack_size;
    p_task->rom_start   = 0;
    p_task->rom_size    = 0;
    p_task->period      = ARKRTOS_MS_TO_TICK(period_ms);
    p_task->priority      = priority;
    p_task->base_priority = priority;   /* 基准优先级：撤销优先级继承时的恢复依据 */
    p_task->shm_attri   = 0;
    p_task->is_priv     = 1u;   /* kernel-created tasks run privileged */
    p_task->status      = ARKRTOS_TASK_READY;
    p_task->period_time = p_task->period;
    p_task->wait_time   = 0;
    p_task->tim_tick    = 0;
    p_task->touch_tick  = 0;
    p_task->wake_reason = 0;
    p_task->recover_pending = 0;
    p_task->ready_next  = ARKRTOS_TASK_NIL;
    p_task->ready_prev  = ARKRTOS_TASK_NIL;
    p_task->delay_next  = ARKRTOS_TASK_NIL;
    p_task->delay_prev  = ARKRTOS_TASK_NIL;
    p_task->delay_queued = 0u;
    p_task->delay_tick  = 0u;
    p_task->slice_tick  = (int32)ARKRTOS_TIME_SLICE_TICKS;

    /* Fill the MPU context (no-op when ARKRTOS_USE_MPU == 0).
     * The loader calls this again after rewriting rom_start / ram_size for
     * App and Driver tasks, because those windows are only known then. */
    arkrtos_mpu_build_task(p_task);

    arkrtos_task_stack_init(p_task, entry, stack_bottom, stack_size);

    /* 新任务直接加入就绪集：位图/链表在这一步同步完成，
     * 不需要再作废任何缓存。 */
    arkrtos_ready_add(free_idx);

    return (free_idx + 1);
}

#if (ARKRTOS_USE_STACK_USAGE == 1)
/**
* @brief 用填充图案覆盖整段任务栈（跳过下标 0 的栈底保护字）
* @param stack_bottom 栈底地址
* @param stack_size   栈大小（字节）
* @details 任务运行过程中会不断覆盖该图案，查询栈用量时扫描仍保持图案的
*          连续区域即可得到“从未使用过”的栈空间，换算后即为峰值用量。
*/
void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size)
{
    uint32 words = stack_size / 4u;
    uint32 i;

    for(i = 1u; i < words; i++)
    {
        stack_bottom[i] = ARKRTOS_STACK_FILL_PATTERN;
    }
}
#endif


void arkrtos_task_stack_init(arkrtos_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size)
{
    uint32 stack_top = (uint32)stack_bottom + stack_size;

    stack_top &= ~0x7u;

    #if (ARKRTOS_USE_STACK_USAGE == 1)
    /* 先填充整段栈，再建立初始栈帧，便于后续统计峰值栈用量 */
    arkrtos_task_stack_fill(stack_bottom, stack_size);
    #endif

    p_task->stack_bottom = stack_bottom;
    p_task->stack_top    = stack_top;

    #if (ARKRTOS_USE_STACK_USAGE == 1)
    p_task->stack_peak_low = stack_top;     /* 峰值统计基准：初始未使用 */
    #endif


    p_task->entry     = entry;
    p_task->stack_ptr = arkrtos_port_stack_init(stack_top, entry);
}
