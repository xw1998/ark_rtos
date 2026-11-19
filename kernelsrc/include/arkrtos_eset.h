/**
* @file arkrtos_eset.h
* @brief Ark RTOS 32 位事件集（内核内部）
* @details 定义事件集对象结构与内核内部操作函数。
*          本文件仅供内核内部使用；应用程序经 arkrtos.h 的 arkrtos_eset_*()，
*          由内核 SVC_ESET_CTRL 服务进入这里。
*
*          与 arkrtos_event 的分工（两套对象语义不同，不互相替代）：
*            - arkrtos_event：**单标志 + 广播 + 自动复位**。一个 flag，
*              set 唤醒所有等待者，谁先被调度谁把 flag 消费掉。
*              适合"这件事发生了，所有人都该往前走一步"。
*            - arkrtos_eset ：**32 个独立位 + 按位掩码等待 + 可选清除**。
*              等待者声明"我要哪些位、是任一满足还是全部满足"，set 只唤醒
*              条件真正被满足的等待者，并把命中的位交给它。
*              适合"多个条件同时成立才开工""几条生产线谁先报就到齐"。
*
*          语义要点（写在这里是因为它们都只能在代码里体现一次）：
*            1) set 是**累加**的：flags |= mask。没有等待者时也留在那里，
*               下一次 wait 直接命中，不会丢事件。
*            2) OR / AND 必须二选一；同时给或都不给都是参数错误。
*            3) CLEAR 由**拿到位的那一方**清除：内核态等待者由 set 在交接时
*               清（只清交给它的那些位），用户态等待者由轮询命中时清。
*               因此同一个位只会被一个 CLEAR 等待者消费。
*            4) 交给等待者的值在**交接那一刻**算定并写进它的等待槽，
*               醒来后读槽，不重新算——避免"给回一个与满足时不一致的值"
*               这种静默错答案。
* @author xw
* @date 2026.11.14
*/

#ifndef __ARKRTOS_ESET_H__
#define __ARKRTOS_ESET_H__

#include "arkrtos_features.h"
#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

#if (ARKRTOS_USE_ESET == 1)

/* 等待选项：OR 与 AND 二选一，CLEAR 可叠加。
 * 这三个名字同时出现在应用侧（arkrtos.h），数值是接口契约的一部分，
 * 改名可以，改值不行。 */
#define ARKRTOS_ESET_OPT_OR         (0x01u)   /* 掩码内任一位成立即返回 */
#define ARKRTOS_ESET_OPT_AND        (0x02u)   /* 掩码内所有位都成立才返回 */
#define ARKRTOS_ESET_OPT_CLEAR      (0x04u)   /* 命中后清除命中的位 */

/* 返回值约定：>=0 成功（0 = 已拿到位），负值失败。
 * 与 arkrtos_event 一致，多个名字共用 -1 是有意的：调用方只判断负值，
 * 名字只是让代码读得出失败原因。 */
#define ARKRTOS_ESET_ERR_PARAM      (-1)   /* 名字/输出指针为空，或掩码为 0 */
#define ARKRTOS_ESET_ERR_HANDLE     (-1)   /* 句柄不是事件集对象（标志位不对） */
#define ARKRTOS_ESET_ERR_STATE      (-1)   /* 对象未创建 / 已被删除（used==0） */
#define ARKRTOS_ESET_ERR_OPT        (-1)   /* OR / AND 未二选一 */
#define ARKRTOS_ESET_ERR_NOSPACE    (-1)   /* 等待槽已满，登记不上等待者 */
#define ARKRTOS_ESET_ERR_CONTEXT    (-1)   /* 当前任务不可用 / 未能进入阻塞 */
#define ARKRTOS_ESET_ERR_DELETED    (-1)   /* 等待期间对象被删除 */
#define ARKRTOS_ESET_ERR_NOSLOT     (-1)   /* 对象表已满，建不出新事件集 */

/* 以下两个不是错误，是比较结果： */
#define ARKRTOS_ESET_WAITER_DUP     (-2)   /* 同一任务已在等待队列里，就地更新掩码 */
#define ARKRTOS_ESET_WAITER_ABSENT  (-1)   /* 摘除时该任务不在等待队列里 */

typedef struct {
    arkrtos_task_t *task;          /* 0 = 空槽；非 0 = 该槽属于这个任务 */
    uint32          mask;          /* 这个等待者要的位 */
    uint32          opt;           /* OR / AND / CLEAR 组合 */
    uint8           woken;         /* 1 = 交接已完成，等该任务恢复后自己消费 */
    uint8           reserved[3];
    uint32          got;           /* 交接那一刻算定的返回值（只有 woken==1 时有意义） */
} arkrtos_eset_waiter_t;

typedef struct {
    char                name[16];
    uint8               used;          /* 该对象槽是否已被占用 */
    uint8               reserved[3];
    uint32              flags;         /* 32 位事件集本体：累加式，位 0..31 */
    int32               creator_id;    /* 创建者任务 id；0 = 内核持有，随镜像存活 */
    arkrtos_eset_waiter_t waiters[ARKRTOS_MAX_ESET_WAITERS];
} arkrtos_eset_obj_t;

void  arkrtos_eset_module_init(void);
int32 arkrtos_eset_create_internal(const char *name);
int32 arkrtos_eset_wait_internal(int32 handle, uint32 mask, uint32 opt,
                                 uint32 *p_got, int32 timeout_ms);
int32 arkrtos_eset_set_internal(int32 handle, uint32 mask);
int32 arkrtos_eset_clear_internal(int32 handle, uint32 mask);
int32 arkrtos_eset_get_internal(int32 handle, uint32 *p_flags);
int32 arkrtos_eset_delete_internal(int32 handle);

/* 任务下线收尸：把任务从所有事件集的等待队列中摘除，并回收它创建的对象。
 * 调用方需自行保证临界区；一般经 arkrtos_task_release_resources() 调用。 */
void  arkrtos_eset_release_task(int32 task_id);

#endif /* ARKRTOS_USE_ESET == 1 */

#endif /* __ARKRTOS_ESET_H__ */
