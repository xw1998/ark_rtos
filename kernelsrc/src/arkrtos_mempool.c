/**
* @brief Ark RTOS 固定块内存池模块实现
* @details 见头文件的口径说明。实现上只有三件事：
*          - 创建：把调用者给的区按 block_size 切成 block_count 块，
*            用侵入式单链表把所有空闲块串起来（块自己的前 sizeof(void*) 字节存 next）；
*          - 分配：摘链头，O(1)；
*          - 释放：先校验指针确实落在本池的某个块首，再插回链头。
*
*          进入临界区是因为 free_list 是共享可变状态，任务与中断都可能碰。
*          分配与释放本身不阻塞、不切任务，所以临界区极短。
*/

#include "arkrtos_mempool.h"
#include "arkrtos_hal.h"

#if (ARKRTOS_USE_MEMPOOL == 1)

/* 池的内部对象（私有类型，只在本文件可见）。
 * 池的内存区由调用者提供，对象本身不持有池内存，只记几何与空闲链头。 */
typedef struct {
    uint32 used;                              /* 1 = 该表项被占用 */
    char   name[ARKRTOS_MEMPOOL_NAME_MAX];
    uint8 *base;                              /* 调用者提供的内存区首址 */
    uint32 block_size;
    uint32 block_count;
    uint32 free_count;
    uint32 alloc_ok;
    uint32 alloc_fail;
    uint32 free_bad;
    uint32 peak_used;
    void  *free_list;                         /* 空闲块侵入式单链链头 */
} arkrtos_mempool_obj_t;

static arkrtos_mempool_obj_t arkrtos_mempools[ARKRTOS_MEMPOOL_NUM];

static int32 mp_name_eq(const char *a, const char *b)
{
    int32 i;
    for(i = 0; i < ARKRTOS_MEMPOOL_NAME_MAX; i++)
    {
        if(a[i] != b[i])
        {
            return 0;
        }
        if(a[i] == 0)
        {
            return 1;
        }
    }
    return 0;
}

static void mp_name_copy(char *dst, const char *src)
{
    int32 i;
    for(i = 0; i < ARKRTOS_MEMPOOL_NAME_MAX - 1; i++)
    {
        dst[i] = src[i];
        if(src[i] == 0)
        {
            break;
        }
    }
    dst[ARKRTOS_MEMPOOL_NAME_MAX - 1] = 0;
}

static arkrtos_mempool_obj_t *mp_get(int32 handle)
{
    if((handle < 0) || (handle >= ARKRTOS_MEMPOOL_NUM))
    {
        return 0;
    }
    if(arkrtos_mempools[handle].used == 0)
    {
        return 0;
    }
    return &arkrtos_mempools[handle];
}

void arkrtos_mempool_module_init(void)
{
    int32 i;
    for(i = 0; i < ARKRTOS_MEMPOOL_NUM; i++)
    {
        arkrtos_mempools[i].used        = 0;
        arkrtos_mempools[i].name[0]     = 0;
        arkrtos_mempools[i].base        = 0;
        arkrtos_mempools[i].block_size  = 0;
        arkrtos_mempools[i].block_count = 0;
        arkrtos_mempools[i].free_count  = 0;
        arkrtos_mempools[i].alloc_ok    = 0;
        arkrtos_mempools[i].alloc_fail  = 0;
        arkrtos_mempools[i].free_bad    = 0;
        arkrtos_mempools[i].peak_used   = 0;
        arkrtos_mempools[i].free_list   = 0;
    }
}

int32 arkrtos_mempool_create_internal(const char *name, void *pool,
                                      uint32 block_size, uint32 block_count)
{
    uint32 state;
    uint32 k;
    int32  i;
    int32  name_len;
    uint8 *b;
    arkrtos_mempool_obj_t *p;

    /* ---- 参数校验：宁可拒绝，也不建一个"看着能用"的池 ---- */
    if((name == 0) || (name[0] == 0) || (pool == 0))
    {
        return -1;
    }
    if(block_count == 0u)
    {
        return -1;
    }
    /* 空闲链要把 next 存进块里，块必须放得下一个指针 */
    if(block_size < (uint32)sizeof(void *))
    {
        return -1;
    }
    /* 块大小必须按指针宽度对齐，否则 (void**)blk 是非对齐写 */
    if((block_size & ((uint32)sizeof(void *) - 1u)) != 0u)
    {
        return -1;
    }
    name_len = 0;
    while((name_len < ARKRTOS_MEMPOOL_NAME_MAX) && (name[name_len] != 0))
    {
        name_len++;
    }
    if(name_len >= ARKRTOS_MEMPOOL_NAME_MAX)
    {
        return -1;
    }

    state = ARKRTOS_ENTER_CRITICAL();

    for(i = 0; i < ARKRTOS_MEMPOOL_NUM; i++)
    {
        if((arkrtos_mempools[i].used != 0) &&
           (mp_name_eq(arkrtos_mempools[i].name, name) != 0))
        {
            ARKRTOS_EXIT_CRITICAL(state);
            return -1;                          /* 重名 */
        }
    }

    for(i = 0; i < ARKRTOS_MEMPOOL_NUM; i++)
    {
        if(arkrtos_mempools[i].used == 0)
        {
            break;
        }
    }
    if(i >= ARKRTOS_MEMPOOL_NUM)
    {
        ARKRTOS_EXIT_CRITICAL(state);
        return -1;                              /* 对象表已满 */
    }

    p = &arkrtos_mempools[i];
    p->used        = 1;
    mp_name_copy(p->name, name);
    p->base        = (uint8 *)pool;
    p->block_size  = block_size;
    p->block_count = block_count;
    p->free_count  = block_count;
    p->alloc_ok    = 0;
    p->alloc_fail  = 0;
    p->free_bad    = 0;
    p->peak_used   = 0;
    p->free_list   = 0;

    /* 从最后一块往前插，链头是最低地址那块 */
    for(k = block_count; k > 0u; k--)
    {
        b = p->base + (k - 1u) * block_size;
        *(void **)b = p->free_list;
        p->free_list = (void *)b;
    }

    ARKRTOS_EXIT_CRITICAL(state);
    return i;
}

void *arkrtos_mempool_alloc_internal(int32 handle)
{
    uint32 state;
    uint32 used;
    void  *blk;
    arkrtos_mempool_obj_t *p = mp_get(handle);

    if(p == 0)
    {
        return 0;
    }

    state = ARKRTOS_ENTER_CRITICAL();

    if(p->free_list == 0)
    {
        p->alloc_fail++;
        ARKRTOS_EXIT_CRITICAL(state);
        return 0;                               /* 无空闲块：返回 0，不阻塞 */
    }

    blk = p->free_list;
    p->free_list = *(void **)blk;
    p->free_count--;
    p->alloc_ok++;
    used = p->block_count - p->free_count;
    if(used > p->peak_used)
    {
        p->peak_used = used;
    }

    ARKRTOS_EXIT_CRITICAL(state);
    return blk;
}

int32 arkrtos_mempool_free_internal(int32 handle, void *p_blk)
{
    uint32 state;
    uint32 off;
    arkrtos_mempool_obj_t *p = mp_get(handle);

    if((p == 0) || (p_blk == 0))
    {
        return -1;
    }

    state = ARKRTOS_ENTER_CRITICAL();

    /* 先判"是否落在池内"（指针比较不会回绕），再算偏移；
     * 用 off + size 判界会因 u32 相加回绕而把池外指针判成合法。 */
    if((uint32)p_blk < (uint32)p->base)
    {
        p->free_bad++;
        ARKRTOS_EXIT_CRITICAL(state);
        return -1;
    }
    off = (uint32)((uint8 *)p_blk - p->base);
    if((off >= (p->block_size * p->block_count)) ||
       ((off % p->block_size) != 0u))
    {
        p->free_bad++;
        ARKRTOS_EXIT_CRITICAL(state);
        return -1;                              /* 越界或不对齐：拒绝，池不变 */
    }

    *(void **)p_blk = p->free_list;
    p->free_list = p_blk;
    p->free_count++;

    ARKRTOS_EXIT_CRITICAL(state);
    return 0;
}

int32 arkrtos_mempool_delete_internal(int32 handle)
{
    uint32 state;
    arkrtos_mempool_obj_t *p = mp_get(handle);

    if(p == 0)
    {
        return -1;
    }

    state = ARKRTOS_ENTER_CRITICAL();
    p->used      = 0;
    p->name[0]   = 0;
    p->free_list = 0;
    ARKRTOS_EXIT_CRITICAL(state);
    return 0;
}

int32 arkrtos_mempool_is_alive(int32 handle)
{
    return (mp_get(handle) != 0) ? 1 : 0;
}

int32 arkrtos_mempool_query_internal(int32 handle, arkrtos_mempool_stat_t *out)
{
    arkrtos_mempool_obj_t *p = mp_get(handle);
    int32 i;

    if((p == 0) || (out == 0))
    {
        return -1;
    }

    for(i = 0; i < ARKRTOS_MEMPOOL_NAME_MAX; i++)
    {
        out->name[i] = p->name[i];
    }
    out->block_size  = p->block_size;
    out->block_count = p->block_count;
    out->free_count  = p->free_count;
    out->alloc_ok    = p->alloc_ok;
    out->alloc_fail  = p->alloc_fail;
    out->free_bad    = p->free_bad;
    out->peak_used   = p->peak_used;
    return 0;
}

#else  /* ARKRTOS_USE_MEMPOOL == 0 : 关掉时整块不进二进制，只留调用桩 */

void  arkrtos_mempool_module_init(void) { }

int32 arkrtos_mempool_create_internal(const char *name, void *pool,
                                      uint32 block_size, uint32 block_count)
{
    (void)name; (void)pool; (void)block_size; (void)block_count;
    return -1;
}

void *arkrtos_mempool_alloc_internal(int32 handle) { (void)handle; return 0; }

int32 arkrtos_mempool_free_internal(int32 handle, void *p)
{
    (void)handle; (void)p; return -1;
}

int32 arkrtos_mempool_delete_internal(int32 handle) { (void)handle; return -1; }

int32 arkrtos_mempool_is_alive(int32 handle) { (void)handle; return 0; }

int32 arkrtos_mempool_query_internal(int32 handle, arkrtos_mempool_stat_t *out)
{
    (void)handle; (void)out; return -1;
}

#endif /* ARKRTOS_USE_MEMPOOL */
