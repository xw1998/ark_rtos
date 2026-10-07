/**
* @brief Ark RTOS 故障记录模块实现
* @details 固定容量环形缓冲保存最近 ARKRTOS_FAULT_RECORD_NUM 条故障记录，
*          记满后覆盖最旧记录。读写均在中断保护下进行，
*          可安全用于 HardFault 处理路径与 tick 上下文。
*/

#include "arkrtos_fault.h"
#include "arkrtos_hal.h"

static arkrtos_fault_record_t arkrtos_faults[ARKRTOS_FAULT_RECORD_NUM];
static int32 arkrtos_fault_wr = 0;        /* 下一个写入位置 */
static int32 arkrtos_fault_total = 0;     /* 累计记录数（用于区分空与满） */

void arkrtos_fault_module_init(void)
{
    int32 i;
    for(i = 0; i < ARKRTOS_FAULT_RECORD_NUM; i++)
    {
        arkrtos_faults[i].type    = 0;
        arkrtos_faults[i].task_id = 0;
        arkrtos_faults[i].tick    = 0;
    }
    arkrtos_fault_wr = 0;
    arkrtos_fault_total = 0;
}

void arkrtos_fault_record(uint32 type, int32 task_id)
{
    /* 用保存-恢复语义，而不是无条件开关中断：
     * 本函数会从故障处理、tick、调度器锁检查等路径调用，调用时可能已经
     * 处在某个临界区内；无条件 ENABLE 会破坏外层临界区
     * （故障路径尤其危险：记录完还要恢复别的任务运行，中间被中断插入
     *   改动任务表就会出问题）。 */
    uint32 state = ARKRTOS_ENTER_CRITICAL();

    arkrtos_faults[arkrtos_fault_wr].type    = type;
    arkrtos_faults[arkrtos_fault_wr].task_id = task_id;
    arkrtos_faults[arkrtos_fault_wr].tick    = arkrtos_kernel_tick;
    arkrtos_fault_wr = (arkrtos_fault_wr + 1) % ARKRTOS_FAULT_RECORD_NUM;
    if(arkrtos_fault_total < ARKRTOS_FAULT_RECORD_NUM)
    {
        arkrtos_fault_total++;
    }

    ARKRTOS_EXIT_CRITICAL(state);
}

const arkrtos_fault_record_t *arkrtos_fault_record_get(int32 index)
{
    int32 oldest;
    int32 slot;

    if(index < 0 || index >= arkrtos_fault_total)
        return 0;

    /* index=0 指向最旧记录；环形缓冲中最早位置取决于是否已写满 */
    if(arkrtos_fault_total < ARKRTOS_FAULT_RECORD_NUM)
    {
        oldest = 0;
    }
    else
    {
        oldest = arkrtos_fault_wr;        /* 写满时 wr 即最旧槽位 */
    }
    slot = (oldest + index) % ARKRTOS_FAULT_RECORD_NUM;
    return &arkrtos_faults[slot];
}

int32 arkrtos_fault_record_count_internal(void)
{
    return arkrtos_fault_total;
}
