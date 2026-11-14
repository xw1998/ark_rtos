/**
* @brief Ark RTOS 消息队列模块（内核内部头文件）
* @details 提供任务间数据传递原语：变长消息队列（拷贝语义）。
*          队列深度由 ARKRTOS_MQ_DEPTH 约定，单条消息长度上限为
*          ARKRTOS_MQ_MSG_WORDS 个字；每条消息实际占几个字由发送方声明并随消息记录，
*          接收方按自己的缓冲容量取，recv 返回实际取到的字数。
*          支持阻塞 send/recv（带超时）与中断上下文安全的 send_from_isr。
*          应用程序应通过 arkrtos.h 使用，不要直接包含本文件。
*/

#ifndef __ARKRTOS_MQ_H__
#define __ARKRTOS_MQ_H__

#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"
/* 返回值：0 成功，负值失败。历史上每次失败都写裸 -1，读代码看不出
 * 「为什么失败」；下面这些名字把原因写进代码，但**值不得单独修改**——
 * 同一模块里多个名字共用 -1 是有意的（分名只为表意），且 mq 的超时
 * 与 sync 的 -2 是两套契约，本模块 send 侧的超时就是 -1。 */
#define ARKRTOS_MQ_ERR_PARAM     (-1)   /* 参数非法：缓冲为空 / 消息字数越界 */
#define ARKRTOS_MQ_ERR_HANDLE    (-1)   /* 句柄不是消息队列（标志位不对） */
#define ARKRTOS_MQ_ERR_STATE     (-1)   /* 对象未创建 / 已被删除（used==0） */
#define ARKRTOS_MQ_ERR_FULL      (-1)   /* 队列已满（且不等待） */
#define ARKRTOS_MQ_ERR_EMPTY     (-1)   /* 队列为空（且不等待） */
#define ARKRTOS_MQ_ERR_NOSPACE   (-1)   /* 等待队列已满，登记不上等待者 */
#define ARKRTOS_MQ_ERR_CONTEXT   (-1)   /* 当前任务不可用 / 未能进入阻塞 */
#define ARKRTOS_MQ_ERR_TIMEOUT   (-1)   /* 等到超时仍未成功（send 侧超时是 -1） */
#define ARKRTOS_MQ_ERR_DELETED   (-1)   /* 等待期间对象被删除 */
#define ARKRTOS_MQ_ERR_NOSLOT    (-1)   /* 队列表已满，建不出新队列 */
#define ARKRTOS_MQ_ERR_DISABLED  (-1)   /* ARKRTOS_USE_MQ=0：特性未编入 */

/* 以下两个不是错误，是比较结果 / 哨兵值： */
#define ARKRTOS_MQ_WAITER_DUP    (-2)   /* 同一个任务已在等待队列里，无需重复登记 */
#define ARKRTOS_MQ_WAIT_INFINITE (-1)   /* remain_ms 的「无限等待」标记 */


#if (ARKRTOS_USE_MQ == 1)

typedef struct {
    char   name[8];
    uint8  used;                                        /* 槽位是否占用 */
    int32  creator_id;                                  /* creator task id; 0 = kernel. Dropped together with its creator's image */
    uint32 head;                                        /* 读位置 */
    uint32 tail;                                        /* 写位置 */
    uint32 count;                                       /* 当前消息条数 */
    arkrtos_task_t *send_waiters[ARKRTOS_MAX_SYNC_WAITERS]; /* 队列满时阻塞的发送者 */
    arkrtos_task_t *recv_waiters[ARKRTOS_MAX_SYNC_WAITERS]; /* 队列空时阻塞的接收者 */
    uint32 buf[ARKRTOS_MQ_DEPTH * ARKRTOS_MQ_MSG_WORDS];    /* 消息存储区（定长槽） */
    uint8  msglen[ARKRTOS_MQ_DEPTH];                      /* 每个槽里消息实际占的字数 */
} arkrtos_mq_obj_t;

void  arkrtos_mq_module_init(void);

int32 arkrtos_mq_create_internal(char *name);

/* Kernel owned queue: the creator id is pinned to 0, so
 * arkrtos_mq_release_task() never collects it when some task goes away.
 * Use it for queues the kernel itself owns and that must outlive every
 * task (the socket request queue is one). */
int32 arkrtos_mq_create_kernel_internal(char *name);

/* 1 = the handle still names a live queue object, not merely an in-range
 * index. A stored handle keeps passing the plain range checks after the
 * object behind it is gone, so this is the probe that tells the truth. */
int32 arkrtos_mq_is_alive(int32 handle);
int32 arkrtos_mq_send_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms);
int32 arkrtos_mq_recv_internal(int32 handle, uint32 *buf, int32 len_words, int32 timeout_ms);
int32 arkrtos_mq_delete_internal(int32 handle);

int32 arkrtos_mq_send_from_isr_internal(int32 handle, void *buf, int32 len_words);

/* 任务下线收尸：把任务从所有消息队列的收发等待队列摘除。
 * 调用方需自行保证临界区；一般经 arkrtos_task_release_resources() 调用。 */
void  arkrtos_mq_release_task(int32 task_id);

#endif /* ARKRTOS_USE_MQ */

#endif
