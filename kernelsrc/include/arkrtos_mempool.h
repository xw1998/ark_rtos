/**
* @brief Ark RTOS 固定块内存池模块（内核内部头文件）
* @details 把一段**由调用者提供**的内存切成 N 个等长块，分配与释放都是 O(1)：
*          空闲块用侵入式单链表串起来（空闲块自己的前几个字节存 next），
*          所以池本身不额外占 RAM，也不需要堆。没有外部碎片，回收不合并。
*
*          与堆的区别（为什么不直接用 POSIX 层的 arena）：
*          - 变长分配反复申请释放会产生碎片，长时间运行的工业设备受不了；
*          - 固定块池的最坏分配时间是常数，能算进实时预算；
*          - 池的容量在创建时就是确定的，不会"跑着跑着分配失败"。
*
*          错误口径（宁可报错，不给"看起来对的错答案"）：
*          - block_size 小于一个指针宽度、或没有按指针宽度对齐 → 创建失败；
*          - 名字为空 / 重名 / 句柄非法 → 创建或操作失败；
*          - free() 传入不在本池内、或未按块对齐的指针 → 失败且**不动池状态**
*            （绝不做"尽力而为"的插入，那会静默毁掉整条空闲链）。
*
*          两种使用面：
*          - 内核侧 API（本头文件）：内核组件、shell 自检直接调用；
*          - App / 驱动：经 SVC 0x20（`ARKRTOS_SVC_MEMPOOL_CTRL`）使用，
*            见 `kernelsrc/sdk/app_sdk/arkrtos_oslib.c` 的 `arkrtos_mpool_*`。
*            用户态的池内存区由调用者自己在**本任务可见 RAM** 里划出，
*            内核在开放通道前会校验整段 [pool, pool + block_size*block_count)
*            落在用户可见 RAM 内（否则就等于让 App 借内核之手在任意地址上
*            发块，那是提权而不是分配）。
*
*          生命周期：池记着创建者任务号，任务下线时由
*          arkrtos_mempool_release_task() 回收（否则反复装卸的 App 会把
*          对象表占满，表现成"第一次能用，装几次就不能用了"）。
*/

#ifndef __ARKRTOS_MEMPOOL_H__
#define __ARKRTOS_MEMPOOL_H__

#include "arkrtos_def.h"
#include "arkrtos_config.h"
/* 快照结构与池名长度直接复用应用侧公开头文件，避免"两份定义悄悄跑偏"
 * 导致 SVC 回填的统计数字错位。 */
#include "arkrtos.h"

#if (ARKRTOS_USE_MEMPOOL == 1)

#define ARKRTOS_MEMPOOL_NAME_MAX   ARKRTOS_MPOOL_NAME_MAX

/** @brief 池的只读快照，供 shell / 自检使用（定义在 arkrtos.h，两侧共用一份） */
typedef arkrtos_mpool_stat_t arkrtos_mempool_stat_t;

/** @brief 对象表初始化；由 arkrtos_kernel_init() 调用 */
void  arkrtos_mempool_module_init(void);

/**
 * @brief 在一个调用者提供的内存区上创建池
 * @param name        池名，非空，长度 < ARKRTOS_MEMPOOL_NAME_MAX
 * @param pool        内存区首地址（调用者保证其生命周期不短于池）
 * @param block_size  每块字节数，必须 >= sizeof(void*) 且按指针宽度对齐
 * @param block_count 块数，必须 > 0
 * @return >=0 句柄；-1 参数非法 / 名字重复 / 对象表已满
 */
int32 arkrtos_mempool_create_internal(const char *name, void *pool,
                                      uint32 block_size, uint32 block_count);

/**
 * @brief 取一块；无空闲块时返回 0（不阻塞）
 * @return 块指针；0 表示当前无空闲块或句柄非法
 */
void *arkrtos_mempool_alloc_internal(int32 handle);

/**
 * @brief 归还一块
 * @return 0 = 归还成功；-1 = 句柄非法或指针不属于本池（池状态不变）
 */
int32 arkrtos_mempool_free_internal(int32 handle, void *p);

/** @brief 销毁池（对象表项释放；调用者提供的内存区不受影响） @return 0/-1 */
int32 arkrtos_mempool_delete_internal(int32 handle);

/** @brief 1 = 句柄指向活着的池（不是仅落在下标范围内） */
int32 arkrtos_mempool_is_alive(int32 handle);

/** @brief 取池快照 @return 0 = 成功；-1 = 句柄非法 */
int32 arkrtos_mempool_query_internal(int32 handle, arkrtos_mempool_stat_t *out);

/**
 * @brief 回收某个任务创建的池（任务下线时由内核调用）
 * @note 只释放对象表项；池占用的内存区属于调用者，不在这里释放。
 *       内核/无人认领（creator_id==0）的池不会被任何任务下线带走。
 */
void  arkrtos_mempool_release_task(int32 task_id);

#endif /* ARKRTOS_USE_MEMPOOL */

#endif /* __ARKRTOS_MEMPOOL_H__ */
