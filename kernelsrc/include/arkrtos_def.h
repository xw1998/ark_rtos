/**
* @brief Ark RTOS 内核公共常量定义
* @details 集中定义句柄标志位、句柄掩码以及各类 SVC 调用号。
*          句柄高位用标志位区分对象类型（事件/设备/信号量/互斥锁），低位为对象索引。
* @note 这些常量在内核与 SDK 间共享，修改时需保证两侧一致。
*/

#ifndef __ARKRTOS_DEF_H__
#define __ARKRTOS_DEF_H__

#include "arkrtos_types.h"

#define ARKRTOS_EVENT_HANDLE_FLAG     (0x01100000)
#define ARKRTOS_DEV_HANDLE_FLAG       (0x01200000)
#define ARKRTOS_HANDLE_MASK           (0xfff00000)
#define ARKRTOS_HANDLE_RELMASK        (0x000fffff)

#define ARKRTOS_SVC_DEV_IO            (0x10)
#define ARKRTOS_SVC_TASK_CTRL         (0x11)
#define ARKRTOS_SVC_SYS_INFO          (0x12)
#define ARKRTOS_SVC_EVENT_CTRL        (0x13)
#define ARKRTOS_SVC_DRV_MGR           (0x14)
#define ARKRTOS_SVC_SYNC_CTRL         (0x15)
#define ARKRTOS_SVC_MQ_CTRL           (0x16)
#define ARKRTOS_SVC_TIMER_CTRL        (0x17)
#define ARKRTOS_SVC_LOG               (0x19)
#define ARKRTOS_SVC_SHELL             (0x1A)
#define ARKRTOS_SVC_APP_MGR           (0x18)
/* User thread service: create / self / exit. Backs the POSIX
 * pthread facade in the App SDK; a thread needs a TCB and a scheduler slot,
 * which only the kernel owns, so this is the one place user code can ask for
 * a new execution context. */
#define ARKRTOS_SVC_THREAD_CTRL       (0x1B)   /* App 镜像管理与分区查询 */

/* File service: path based read / write / remove / stat on the mounted
 * file system. Backs the arkrtos_file_*() facade in the App SDK; the kernel
 * owns littlefs, the caches and the block device, so an unprivileged App
 * asks across the SVC boundary and every pointer it passes is validated
 * against the caller's own memory first. */
#define ARKRTOS_SVC_HEARTBEAT         (0x1D)
#define ARKRTOS_SVC_FILE_SYS          (0x1C)
/* Network service: the App side socket/select face (arkrtos_net.c) */
#define ARKRTOS_SVC_NET               (0x1E)

/* 同步原语（信号量/互斥锁）返回码：0=成功，负值=失败。
 * 超时必须返回负值——调用方据此判断“本次没有拿到资源”；
 * 若与 0（成功）混为一谈，会出现“以为拿到了信号量、计数却没减”的错乱。 */
#define ARKRTOS_SYNC_OK             (0)
#define ARKRTOS_SYNC_ERR_PARAM      (-1)
#define ARKRTOS_SYNC_ERR_TIMEOUT    (-2)
#ifndef ARKRTOS_SYNC_ERR_WOULDBLOCK
#define ARKRTOS_SYNC_ERR_WOULDBLOCK (-4)
#endif
#define ARKRTOS_SYNC_ERR_DELETED    (-3)   /* 等待的同步对象/消息队列已被删除 */

/* 任务唤醒原因（task->wake_reason）：等待类接口据此判断“是否真的拿到了资源”。
 * 对象被删除时等待者必须收到 2，否则会一直睡下去（永久泄漏）。 */
#define ARKRTOS_WAKE_NORMAL         (0)    /* 被显式唤醒：已获得资源/事件 */
#define ARKRTOS_WAKE_TIMEOUT        (1)    /* 等待超时 */
#define ARKRTOS_WAKE_OBJ_DELETED    (2)    /* 等待的同步对象/消息队列已被删除 */
/* Bit, not a value: a wake that carries no ARKRTOS_WAKE_HANDOFF was not produced
 * by a poster/unlocker/signal claiming this task, so it must never be reported
 * to the caller as "you got the token". */
#define ARKRTOS_WAKE_HANDOFF        (4)     /* 唤醒来自确定的令牌交接 */

#define ARKRTOS_SEM_HANDLE_FLAG       (0x01300000)
#define ARKRTOS_MTX_HANDLE_FLAG       (0x01400000)
#define ARKRTOS_MQ_HANDLE_FLAG        (0x01500000)
#define ARKRTOS_TIMER_HANDLE_FLAG     (0x01600000)
#define ARKRTOS_COND_HANDLE_FLAG      (0x01700000)

#endif
