/**
* @file test_mempool_host.c
* @brief 固定块内存池的宿主单元测试（不依赖板子、不依赖调度器）
* @details 为什么需要它：内存池的缺陷几乎都在"指针算术"里——块边界算错、
*          外来指针被当成池内、双释放把同一个块挂两次、区大小回绕让越界
*          判成合法。这些在板子上只能靠眼睛看串口，在宿主上可以逐条断言，
*          而且"拒绝时池状态必须一动不动"这条也能直接查。
*
*          覆盖范围（如实说明）：
*            已验证：创建参数校验（块太小/未对齐/池首址未对齐/区大小回绕/
*                    空名/重名）、O(1) 分配与耗尽、释放合法性（外来指针、
*                    未对齐指针、池外指针一律拒绝且池不变）、统计字段、
*                    删除后句柄失效、按创建者任务回收。
*            未验证：SVC 0x20 的参数校验与用户指针可见性检查（需要内核
*                    地址空间与 SVC 边界，只有上板用例能证明）；App 侧
*                    arkrtos_mpool_* 封装（只有真实 SVC 才能跑）。
* @note 用法：由 tools/mempool_host_gate.py 编译并运行，输出 MEMPOOL-HOST OK。
*/

#include "arkrtos_mempool.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩：只提供链接所需的符号，行为尽量"什么都不做"
 * ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32  arkrtos_task_count      = 0;
int32  arkrtos_current_task_id = 0;
uint32 arkrtos_kernel_tick     = 0u;

void arkrtos_port_disable_irq(void) { }
void arkrtos_port_enable_irq(void)  { }
void arkrtos_port_switch_task(void) { }
uint32 arkrtos_port_enter_critical(void) { return 0u; }
void arkrtos_port_exit_critical(uint32 state) { (void)state; }

arkrtos_task_t *arkrtos_task_get_current(void) { return 0; }

/* ------------------------------------------------------------------
 * 测试脚手架
 * ------------------------------------------------------------------ */
static int g_fail = 0;
static int g_checks = 0;

static void check(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fail++;
        fputs("  FAIL: ", stdout);
        fputs(what, stdout);
        fputc('\n', stdout);
    }
}

/* 池内存区：必须按指针宽度对齐，所以用 uint32 数组。
 * 块大小取 2 个指针宽（32 位宿主 8 字节、64 位宿主 16 字节），
 * 所以每个区都按"块数 × 16 字节"给够，免得换编译器就踩到数组尾巴。 */
#define BS   ((uint32)sizeof(void *) * 2u)   /* 每块字节数 */
#define BA   ((uint32)sizeof(void *) - 1u)   /* 对齐掩码 */
static uint32 g_arena[64];   /* 256 B = 16 块 × 16 B */
static uint32 g_small[16];   /*  64 B =  4 块 × 16 B */
static uint32 g_other[32];   /* 128 B =  8 块 × 16 B */

int main(void)
{
    int32 h;
    int32 h2;
    void *blk;
    int32 i;
    arkrtos_mempool_stat_t st;

    arkrtos_mempool_module_init();

    /* ---- 1. 创建参数校验：宁可拒绝，也不建一个"看着能用"的池 ---- */
    check(arkrtos_mempool_create_internal("", (void *)g_arena, BS, 16u) < 0, "empty name refused");
    check(arkrtos_mempool_create_internal("toolongxx", (void *)g_arena, BS, 16u) < 0, "name >= 8 chars refused");
    check(arkrtos_mempool_create_internal("p", 0, BS, 16u) < 0, "null arena refused");
    check(arkrtos_mempool_create_internal("p", (void *)g_arena, 1u, 16u) < 0, "block smaller than a pointer refused");
    check(arkrtos_mempool_create_internal("p", (void *)g_arena, (uint32)sizeof(void *) + 1u, 16u) < 0,
      "unaligned block size refused");
    check(arkrtos_mempool_create_internal("p", (void *)((uint8 *)g_arena + 1), BS, 16u) < 0, "unaligned arena refused");
    check(arkrtos_mempool_create_internal("p", (void *)g_arena, BS, 0u) < 0, "zero block count refused");
    check(arkrtos_mempool_create_internal("p", (void *)g_arena, BS, 0x80000000u) < 0, "wrapping arena size refused");

    /* ---- 2. 创建 / 重名 ---- */
    h = arkrtos_mempool_create_internal("pool", (void *)g_arena, BS, 16u);
    check(h >= 0, "create returns a handle");
    check(arkrtos_mempool_is_alive(h) == 1, "fresh handle is alive");
    check(arkrtos_mempool_create_internal("pool", (void *)g_small, BS, 4u) < 0, "duplicate name refused");

    check(arkrtos_mempool_query_internal(h, &st) == 0, "query succeeds");
    check(strcmp(st.name, "pool") == 0, "query reports the name");
    check(st.block_size == BS, "query reports block size");
    check(st.block_count == 16u, "query reports block count");
    check(st.free_count == 16u, "fresh pool has every block free");
    check((st.alloc_ok == 0u) && (st.alloc_fail == 0u) && (st.free_bad == 0u) && (st.peak_used == 0u),
          "fresh pool counters are zero");

    /* ---- 3. 分配：互不相同、对齐、耗尽后返回 0 ---- */
    {
        void *seen[16];
        int   dup = 0;
        int   bad_align = 0;

        for(i = 0; i < 16; i++)
        {
            seen[i] = arkrtos_mempool_alloc_internal(h);
            if(seen[i] == 0)
            {
                break;
            }
            if(((uint32)seen[i] & BA) != 0u)
            {
                bad_align = 1;
            }
        }
        check(i == 16, "all 16 blocks can be taken");
        check(bad_align == 0, "every block is pointer-aligned");
        for(i = 0; i < 16; i++)
        {
            int j;
            for(j = i + 1; j < 16; j++)
            {
                if(seen[i] == seen[j]) { dup = 1; }
            }
        }
        check(dup == 0, "no block is handed out twice");
        check(arkrtos_mempool_alloc_internal(h) == 0, "exhausted pool returns 0 (never blocks)");

        (void)arkrtos_mempool_query_internal(h, &st);
        check(st.free_count == 0u, "free_count is 0 when exhausted");
        check(st.alloc_ok == 16u, "alloc_ok counts the 16 successful takes");
        check(st.alloc_fail == 1u, "alloc_fail counts the refused take");
        check(st.peak_used == 16u, "peak_used remembers the high-water mark");

        /* ---- 4. 释放：外来 / 未对齐 / 池外一律拒绝，且池状态不动 ---- */
        check(arkrtos_mempool_free_internal(h, (void *)g_other) < 0, "pointer from another arena refused");
        check(arkrtos_mempool_free_internal(h, (void *)((uint8 *)g_arena + 1)) < 0, "unaligned pointer refused");
        check(arkrtos_mempool_free_internal(h, (void *)0) < 0, "null pointer refused");
        (void)arkrtos_mempool_query_internal(h, &st);
        check(st.free_count == 0u, "refused frees leave free_count alone");
        check(st.free_bad == 2u, "free_bad counts every refused free (null is refused before the pool is touched)");

        check(arkrtos_mempool_free_internal(h, seen[3]) == 0, "a block from this pool is accepted");
        (void)arkrtos_mempool_query_internal(h, &st);
        check(st.free_count == 1u, "free_count goes back up by one");
        blk = arkrtos_mempool_alloc_internal(h);
        check(blk == seen[3], "the freed block is the next one handed out (LIFO)");
    }

    /* ---- 5. 删除：句柄失效，第二次删除失败 ---- */
    check(arkrtos_mempool_delete_internal(h) == 0, "delete returns 0");
    check(arkrtos_mempool_is_alive(h) == 0, "handle is dead after delete");
    check(arkrtos_mempool_alloc_internal(h) == 0, "dead handle yields no block");
    check(arkrtos_mempool_delete_internal(h) < 0, "second delete is refused");
    check(arkrtos_mempool_is_alive(9999) == 0, "out-of-range handle is not alive");

    /* ---- 6. 名字释放后可以重新使用 ---- */
    h = arkrtos_mempool_create_internal("pool", (void *)g_small, BS, 4u);
    check(h >= 0, "the name is reusable after delete");

    /* ---- 7. 按创建者任务回收 ---- */
    h2 = arkrtos_mempool_create_internal("other", (void *)g_other, BS, 8u);
    check(h2 >= 0, "second pool created");
    arkrtos_current_task_id = 5;
    {
        int32 h3 = arkrtos_mempool_create_internal("app", (void *)g_arena, BS, 16u);
        check(h3 >= 0, "pool created by task 5");
        /* 先确认三个池是不同的表项 */
        check((h3 != h) && (h3 != h2), "task-owned pool gets its own slot");

        arkrtos_mempool_release_task(5);
        check(arkrtos_mempool_is_alive(h3) == 0, "release_task reclaims the task's pool");
        check(arkrtos_mempool_is_alive(h) == 1, "another task's pool is untouched");
        check(arkrtos_mempool_is_alive(h2) == 1, "and so is the third one");

        /* 回收之后名字也放开了 */
        check(arkrtos_mempool_create_internal("app", (void *)g_arena, BS, 16u) >= 0,
              "the reclaimed pool's name is free again");
    }
    arkrtos_current_task_id = 0;
    arkrtos_mempool_release_task(0);
    check(arkrtos_mempool_is_alive(h) == 1, "release_task(0) never touches kernel/unowned pools");

    if(g_fail == 0)
    {
        fputs("MEMPOOL-HOST OK\n", stdout);
        return 0;
    }
    printf("MEMPOOL-HOST FAILED (%d/%d)\n", g_fail, g_checks);
    return 1;
}
