// 任务调度器
#include "ark_rtos.h"

// 当前任务指针
struct ark_task *ark_current_task;

// 任务列表
ark_list_t ark_task_list[TASK_PRIORITY_MAX];

// 任务休眠列表
ark_list_t ark_task_list_sleep;

void ark_scheduler_init(void)
{
    register ark_base_t offset;
    for(offset = 0; offset < TASK_PRIORITY_MAX; offset++)
    {
        ark_list_init(&ark_task_list[offset]);
    }

    ark_current_task = NULL;
    ark_list_init(&ark_task_list_sleep);
}

