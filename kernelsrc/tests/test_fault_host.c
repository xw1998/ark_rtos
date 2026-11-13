/**
* @file test_fault_host.c
* @brief arkrtos_fault 的宿主测试（故障记录环形缓冲）
*
* @details 覆盖 arkrtos_fault_module_init / arkrtos_fault_record /
*          arkrtos_fault_record_get / arkrtos_fault_record_count_internal。
*
*          已验证：
*          - init 之后缓冲为空：count 0，get(0) 返回 0。
*          - 未写满时 index 0 是最旧记录，顺序与写入顺序一致。
*          - 写满之后 index 仍是「0 = 最旧、count-1 = 最新」，覆盖最旧一条。
*          - 溢出多圈后旧记录被正确丢弃，count 封顶在 ARKRTOS_FAULT_RECORD_NUM。
*          - 越界 index（负数、>= count）返回 0，不解引用任何槽位。
*          - tick 取自 arkrtos_kernel_tick 的当前值（每条独立记录）。
*          - 每次记录都在 enter/exit 临界区之间，且成对（桩计数配平）。
*
*          刻意没验证：
*          - 真实中断嵌套下的原子性（桩把临界区做成空操作，配平只证明
*            「进入与退出一一对应」，不证明它真的关中断）；
*          - HardFault 路径上调用它的可行性（那要板子）；
*          - SVC 读取接口（arkrtos_fault_record_read）的打包格式。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_fault.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond) do {                                                   \
        if(cond) { g_pass++; }                                             \
        else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while(0)

/* ---------------- 桩：临界区与 tick ---------------- */

uint32 arkrtos_kernel_tick = 0u;      /* 内核在别处定义，这里由测试直接写 */

static int g_enter = 0;
static int g_exit  = 0;

uint32 arkrtos_port_enter_critical(void)
{
    g_enter++;
    return 0u;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
    g_exit++;
}

/* 每条记录的类型值就带序号，便于校验顺序 */
static uint32 tag(uint32 i)
{
    return 0x1000u + i;
}

static void test_init_empty(void)
{
    arkrtos_fault_module_init();

    CHECK(arkrtos_fault_record_count_internal() == 0);
    CHECK(arkrtos_fault_record_get(0) == 0);
    CHECK(arkrtos_fault_record_get(-1) == 0);
}

static void test_order_before_full(void)
{
    int32 i;
    int32 n = ARKRTOS_FAULT_RECORD_NUM;

    arkrtos_fault_module_init();

    /* 只写 n-1 条：此时 wr 尚未回到 0，最旧位置仍是 0 号槽 */
    for(i = 0; i < n - 1; i++)
    {
        arkrtos_kernel_tick = (uint32)(100 + i);
        arkrtos_fault_record(tag((uint32)i), i * 3);
    }

    CHECK(arkrtos_fault_record_count_internal() == n - 1);
    for(i = 0; i < n - 1; i++)
    {
        const arkrtos_fault_record_t *p = arkrtos_fault_record_get(i);
        CHECK(p != 0);
        if(p != 0)
        {
            CHECK(p->type == tag((uint32)i));
            CHECK(p->task_id == i * 3);
            CHECK(p->tick == (uint32)(100 + i));
        }
    }
    CHECK(arkrtos_fault_record_get(n - 1) == 0);   /* 还没写到 */
    CHECK(arkrtos_fault_record_get(n) == 0);
}

static void test_wrap(void)
{
    int32 i;
    int32 n = ARKRTOS_FAULT_RECORD_NUM;
    int32 total;

    arkrtos_fault_module_init();

    /* 写满整整一轮：count 封顶，index 0..n-1 就是写入顺序 */
    for(i = 0; i < n; i++)
    {
        arkrtos_fault_record(tag((uint32)i), i);
    }
    CHECK(arkrtos_fault_record_count_internal() == n);
    for(i = 0; i < n; i++)
    {
        const arkrtos_fault_record_t *p = arkrtos_fault_record_get(i);
        CHECK(p != 0 && p->type == tag((uint32)i));
    }

    /* 再写一条：最旧（0 号记录）被覆盖，最新的排到末尾 */
    arkrtos_fault_record(tag((uint32)n), n);
    CHECK(arkrtos_fault_record_count_internal() == n);
    {
        const arkrtos_fault_record_t *p0 = arkrtos_fault_record_get(0);
        const arkrtos_fault_record_t *pl = arkrtos_fault_record_get(n - 1);
        CHECK(p0 != 0 && p0->type == tag(1u));      /* 旧记录 0 已丢 */
        CHECK(pl != 0 && pl->type == tag((uint32)n));
    }

    /* 溢出三圈半：count 仍是 n，index 0 指向「总数 - n」那条 */
    total = n + 3 * n + 3;
    for(i = n + 1; i < total; i++)
    {
        arkrtos_fault_record(tag((uint32)i), i);
    }
    CHECK(arkrtos_fault_record_count_internal() == n);
    {
        int32 first = total - n;                    /* 仍存活的最旧序号 */
        for(i = 0; i < n; i++)
        {
            const arkrtos_fault_record_t *p = arkrtos_fault_record_get(i);
            CHECK(p != 0 && p->type == tag((uint32)(first + i)));
            CHECK(p != 0 && p->task_id == first + i);
        }
        CHECK(arkrtos_fault_record_get(n) == 0);
        CHECK(arkrtos_fault_record_get(-1) == 0);
    }
}

static void test_critical_section_balance(void)
{
    int32 i;

    arkrtos_fault_module_init();
    g_enter = 0;
    g_exit = 0;

    for(i = 0; i < 5; i++)
    {
        arkrtos_fault_record(ARKRTOS_FAULT_HARDFAULT, 0);
    }

    CHECK(g_enter == 5);
    CHECK(g_exit == 5);
}

static void test_reinit_clears(void)
{
    arkrtos_fault_module_init();
    arkrtos_fault_record(ARKRTOS_FAULT_STACKOVF, 7);
    CHECK(arkrtos_fault_record_count_internal() == 1);

    /* 重新初始化必须把读写游标也归零，否则下一轮会从旧位置续写，
     * 导致 get(0) 指到上一轮残留的记录上 */
    arkrtos_fault_module_init();
    CHECK(arkrtos_fault_record_count_internal() == 0);
    CHECK(arkrtos_fault_record_get(0) == 0);

    arkrtos_fault_record(ARKRTOS_FAULT_BUSFAULT, 9);
    {
        const arkrtos_fault_record_t *p = arkrtos_fault_record_get(0);
        CHECK(p != 0 && p->type == ARKRTOS_FAULT_BUSFAULT && p->task_id == 9);
    }
}

int main(void)
{
    test_init_empty();
    test_order_before_full();
    test_wrap();
    test_critical_section_balance();
    test_reinit_clears();

    printf("fault-host: %d passed, %d failed\n", g_pass, g_fail);
    if(g_fail == 0)
    {
        printf("FAULT-HOST OK\n");
        return 0;
    }
    return 1;
}
