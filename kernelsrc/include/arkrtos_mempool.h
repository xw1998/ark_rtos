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
*          本模块当前只提供内核侧 API（内核组件、shell 自检使用）。
*          App / 驱动经 SVC 使用内存池的通道尚未开放，见 docs/与成熟RTOS对比.md。
*/

#ifndef __ARKRTOS_MEMPOOL_H__
#define __ARKRTOS_MEMPOOL_H__

#include "arkrtos_def.h"
#include "arkrtos_config.h"

#if (ARKRTOS_USE_MEMPOOL == 1)

#define ARKRTOS_MEMPOOL_NAME_MAX   (8)

/** @brief 池的只读快照，供 shell / 自检使用 */
typedef struct {
    char     name[ARKRTOS_MEMPOOL_NAME_MAX];
    uint32   block_size;      /* 每块字节数 */
    uint32   block_count;     /* 总块数 */
    uint32   free_count;      /* 当前空闲块数 */
    uint32   alloc_ok;        /* 累计分配成功次数 */
    uint32   alloc_fail;      /* 累计分配失败次数 */
    uint32   free_bad;        /* 累计非法释放（被拒绝）次数 */
    uint32   peak_used;       /* 历史最大同时占用块数 */
} arkrtos_mempool_stat_t;

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

#endif /* ARKRTOS_USE_MEMPOOL */

#endif /* __ARKRTOS_MEMPOOL_H__ */
