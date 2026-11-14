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
/* 返回值约定：0 成功，负值失败。原先失败处一律写裸 -1，读代码看不出
 * 「为什么失败」；下面这些名字把原因写进代码，**值不得单独修改**
 * （多个名字共用 -1 是有意的，分名只为表意）。 */
#define ARKRTOS_EVENT_ERR_PARAM     (-1)   /* 名字指针为空 */
#define ARKRTOS_EVENT_ERR_HANDLE    (-1)   /* 句柄不是事件对象（标志位不对） */
#define ARKRTOS_EVENT_ERR_STATE     (-1)   /* 对象未创建 / 已被删除（used==0） */
#define ARKRTOS_EVENT_ERR_NOSPACE   (-1)   /* 等待队列已满，登记不上等待者 */
#define ARKRTOS_EVENT_ERR_CONTEXT   (-1)   /* 当前任务不可用 / 未能进入阻塞 */
#define ARKRTOS_EVENT_ERR_DELETED   (-1)   /* 等待期间对象被删除 */
#define ARKRTOS_EVENT_ERR_NOSLOT    (-1)   /* 对象表已满，建不出新事件 */

/* 以下两个不是错误，是比较结果： */
#define ARKRTOS_EVENT_WAITER_DUP    (-2)   /* 同一任务已在等待队列里，无需重复登记 */
#define ARKRTOS_EVENT_WAITER_ABSENT (-1)   /* 摘除时该任务不在等待队列里 */

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
