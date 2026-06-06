// 任务调度器
#include "ark_rtos.h"

// 当前任务指针
struct ark_task *ark_current_task;

// 任务列表
ark_list_t ark_task_list[TASK_PRIORITY_MAX];

// 任务休眠列表
ark_list_t ark_task_list_sleep;
