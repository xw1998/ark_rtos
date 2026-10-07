/**
* @brief Ark RTOS 事件模块（内核内部）
* @details 定义事件对象结构和内核内部事件操作函数
*          此文件仅供内核内部使用，应用程序应使用 arkrtos.h
*/

#ifndef __ARKRTOS_EVENT_H__
#define __ARKRTOS_EVENT_H__

#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

typedef struct {
    char   name[16];
    uint8  used;                                        /* 该事件对象是否已被占用 */
    uint8  flag;                                        /* 自动复位式置位标记：1=已置位 */
    int32  creator_id;                                  /* creator task id; 0 = kernel. Dropped together with its creator's image */
    arkrtos_task_t *waiting_tasks[ARKRTOS_MAX_EVENT_WAITERS];
} arkrtos_event_obj_t;

void arkrtos_event_module_init(void);
int32 arkrtos_event_create_internal(char *name);
int32 arkrtos_event_wait_internal(int32 handle, int32 timeout_ms);
void arkrtos_event_set_internal(int32 handle);

/* 任务下线收尸：把任务从所有事件的等待队列中摘除。
 * 调用方需自行保证临界区；一般经 arkrtos_task_release_resources() 调用。 */
void arkrtos_event_release_task(int32 task_id);

#endif
