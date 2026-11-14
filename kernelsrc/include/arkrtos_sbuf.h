/**
* @brief 流缓冲 / 消息缓冲模块（内核内部头文件）
* @details 补齐 FreeRTOS 有、Ark RTOS 一直缺的两种字节流 IPC：
*          - 流缓冲（stream buffer）：单写者、单读者，按字节流收发。
*            send 写入尽量多的字节（可能部分成功）并返回实际写入量；
*            recv 读出尽量多的字节并返回实际读出量。
*          - 消息缓冲（message buffer）：建在流缓冲之上的变长消息。
*            每条消息前带 ARKRTOS_MBUF_HDR_BYTES 字节长度头，send 整条
*            原子写入（要么整条进去，要么一个字节都不动），recv 整条取出。
*
*          与消息队列（arkrtos_mq）的分工：
*            - mq 是定长槽 + 拷贝语义，适合短小控制消息，深度受限；
*            - 流/消息缓冲是连续字节区，适合串口字节流、变长帧，
*              代价是没有 mq 那种逐条独立槽位（读一半就断开不会丢别的消息）。
*          这是真实取舍，不是"更好的 mq"：按用途选，不要替换。
*
*          句柄由内核分配（对象表 + 静态缓冲），没有任何动态内存。
*          缓冲大小与对象数在 arkrtos_config.h 里配置。
* @note 本文件仅供内核内部使用；应用请通过 arkrtos.h 使用。
*/

#ifndef __ARKRTOS_SBUF_H__
#define __ARKRTOS_SBUF_H__

#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

/* 返回 -1 的名字。与上面 ARKRTOS_SBUF_ERR_*(值 -1/-2/-3/-5/-6) 区分：
 * 那组的负值携带原因；这里三个是 -1 的别名。值不得单独修改。 */
#define ARKRTOS_SBUF_ERR_NOSPACE     (-1)   /* 等待者表已满（与 -5 的 NOSPC 不是一回事） */
#define ARKRTOS_SBUF_ERR_BADLEN      (-1)   /* 消息长度头损坏（流/消息混用） */
#define ARKRTOS_SBUF_WAITER_DUP      (-2)   /* 同一任务已在等待表里（等价于登记成功） */
#define ARKRTOS_SBUF_WAIT_INFINITE   (-1)   /* remain_ms 的"无限等待"哨兵（不是错误码） */


#if (ARKRTOS_USE_SBUF == 1)

/* 返回码：与同步原语一致地使用负值表示"这次没成"，
 * 但"空间不够"与"参数不对"必须分开——否则调用方无法判断该重试还是该改代码。 */
#define ARKRTOS_SBUF_OK             (0)
#define ARKRTOS_SBUF_ERR_PARAM      (-1)   /* 句柄/参数非法（含长度为 0/负） */
#define ARKRTOS_SBUF_ERR_TIMEOUT    (-2)   /* 等到超时也没有进展 */
#define ARKRTOS_SBUF_ERR_DELETED    (-3)   /* 等待期间对象被删除 */
#define ARKRTOS_SBUF_ERR_NOSPC      (-5)   /* 消息比整块缓冲还大，永远放不下 */
#define ARKRTOS_SBUF_ERR_TOO_SMALL  (-6)   /* 接收缓冲放不下这条消息（消息已被丢弃） */

typedef struct {
    char   name[8];
    uint8  used;
    int32  creator_id;                                   /* 0 = 内核自建，任务下线不回收 */
    uint32 wt;                                           /* 写索引（环形，按字节回绕） */
    uint32 rd;                                           /* 读索引 */
    uint32 count;                                        /* 当前存了多少字节 */
    arkrtos_task_t *send_waiters[ARKRTOS_MAX_SYNC_WAITERS]; /* 没空间时等着的写者 */
    arkrtos_task_t *recv_waiters[ARKRTOS_MAX_SYNC_WAITERS]; /* 没数据时等着的读者 */
    uint8  data[ARKRTOS_SBUF_BUF_SIZE];
} arkrtos_sbuf_obj_t;

void  arkrtos_sbuf_module_init(void);

int32 arkrtos_sbuf_create_internal(char *name);          /* 创建者 = 当前任务 */
/* 内核自建变体：creator_id 固定为 0，任何任务下线都不会把它收走 */
int32 arkrtos_sbuf_create_kernel_internal(char *name);

/* 句柄是否仍然指向一个活着的对象（光看范围检查挡不住槽位复用） */
int32 arkrtos_sbuf_is_alive(int32 handle);

/* 流语义：返回实际收发的字节数（>0）；0 只在 len<=0 时出现，那是参数错误。
 * timeout_ms：0 = 不等待，<0 = 永久等待。 */
int32 arkrtos_sbuf_send_internal(int32 handle, const void *buf, int32 len, int32 timeout_ms);
int32 arkrtos_sbuf_recv_internal(int32 handle, void *buf, int32 len, int32 timeout_ms);
/* 中断上下文安全版本：只入缓冲 + 唤醒读者，不阻塞、不切任务 */
int32 arkrtos_sbuf_send_from_isr_internal(int32 handle, const void *buf, int32 len);

int32 arkrtos_sbuf_used_internal(int32 handle);          /* 已存字节数 */
int32 arkrtos_sbuf_free_internal(int32 handle);          /* 剩余空间字节数 */
int32 arkrtos_sbuf_reset_internal(int32 handle);
int32 arkrtos_sbuf_delete_internal(int32 handle);

/* 消息语义：send 返回 0 = 整条写入；recv 返回消息长度（>0）。
 * recv 的 len 比消息短时整条丢弃并返回 ARKRTOS_SBUF_ERR_TOO_SMALL——
 * 不返回半条，也不把剩下的字节留在缓冲里污染下一条。 */
int32 arkrtos_mbuf_send_internal(int32 handle, const void *buf, int32 len, int32 timeout_ms);
int32 arkrtos_mbuf_recv_internal(int32 handle, void *buf, int32 len, int32 timeout_ms);
int32 arkrtos_mbuf_send_from_isr_internal(int32 handle, const void *buf, int32 len);
/* 下一条完整消息的长度；没有完整消息时返回负值。用于"先问长度再分配"。 */
int32 arkrtos_mbuf_peek_len_internal(int32 handle);

/* 任务下线收尸：把任务从收发等待队列摘除，并回收它创建的对象 */
void  arkrtos_sbuf_release_task(int32 task_id);

#endif /* ARKRTOS_USE_SBUF */

#endif
