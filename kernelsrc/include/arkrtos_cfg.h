/**
* @brief Ark RTOS 任务配置与任务表声明
* @details 声明全局任务表、任务计数，以及任务配置加载与任务栈初始化接口。
*          arkrtos_cfg_load 在内核启动时加载任务配置，arkrtos_task_stack_init 初始化任务栈帧。
*/

#ifndef __ARKRTOS_CFG_H__
#define __ARKRTOS_CFG_H__

#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

extern arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
extern int32 arkrtos_task_count;

void arkrtos_cfg_load(void);
#if (ARKRTOS_USE_STACK_USAGE == 1)
/**
* @brief 用填充图案覆盖整段任务栈（栈底保护字除外）
* @param stack_bottom 栈底地址
* @param stack_size   栈大小（字节）
* @note 仅在任务创建/重建时调用，用于后续统计峰值栈用量。
*/
void arkrtos_task_stack_fill(uint32 *stack_bottom, uint32 stack_size);
#endif

int32 arkrtos_task_register(void (*entry)(void), uint32 *stack_bottom, uint32 stack_size,
                          uint8 priority, uint32 period_ms);
void arkrtos_task_stack_init(arkrtos_task_t *p_task, void (*entry)(void),
                           uint32 *stack_bottom, uint32 stack_size);

#endif
