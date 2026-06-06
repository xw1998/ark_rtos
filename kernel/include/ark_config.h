#ifndef __ARK_CONFIG_H__
#define __ARK_CONFIG_H__
#include "ark_rtos.h"

//任务名称最大长度
#define NAME_MAX_LEN 16

// 任务优先级最大值
#define TASK_PRIORITY_MAX 32

// 任务栈对齐大小
#define ARK_ALIGN_SIZE 4     

//TCB配置如果是静态任务列表，需要定义MAX_TASK_NUM
//如果如果是动态任务列表，不需要定义MAX_TASK_NUM
#define STATIC_TASK_LIST 1
#ifdef STATIC_TASK_LIST
#define MAX_TASK_NUM 8
#endif

// 任务调度器配置1为时间片轮转调度器，0为协程调度器
#define SCHEDULER_ENABLE 1
#ifdef SCHEDULER_ENABLE
#define SCHEDULER_TYPE 1
#else
#define SCHEDULER_TYPE 0
#endif



#endif