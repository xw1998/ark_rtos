/**
* @file test_cfg_host.c
* @brief 任务配置与任务表（arkrtos_cfg.c）的宿主单元测试
* @details 这个文件是全内核唯一的 TCB 填表入口：应用 main、内核内置服务任务、
*          加载器拉起 App 都走 arkrtos_task_register()。它错的方式都很安静：

*            - 槽位分配先扫"已释放且没有待恢复标记"的槽，扫不到才扩表。若
*              扫的条件写宽了（少了 recover_pending 判断），一个正等着被
*              arkrtos_task_recover_pending 复活的任务会被当场覆盖——故障
*              恢复就此断掉，而且没有任何报错；
*            - 参数闸门在槽位分配之后。所以注册失败时 count 已经涨过了，
*              读代码的人很容易以为"失败不占槽"。实际不占：失败槽仍是
*              INVALID，下一次注册会把它捡回来复用。这条必须钉住，否则
*              改成"失败回滚 count"会与复用逻辑打架；
*            - 表满与参数非法用的是同一个故障码 NOSLOT，靠返回码区分；
*            - 栈顶对齐用 ~0x7UL，地址已加宽到 unsigned long，掩码若写成
*              ~0x7U 就会在高位上留下垃圾；
*            - 栈填充从下标 1 开始（下标 0 是保护字），填到 words 为止，
*              多填或从 0 开始填都会毁掉保护字或越界。

*          被测的是真实的 arkrtos_cfg.c。就绪队列、MPU 构建、故障记录、
*          port 层栈帧初始化全部由本文件桩住；任务表本身用被测模块定义的
*          那份（不重复定义）。

*          覆盖范围（如实说明）：
*            已验证：cfg_load 清计数并重置就绪集、注册成功路径逐字段
*                    （含 period_ms 到 tick 换算、slice_tick、优先级双写、
*                    NIL 链标记、is_priv/status）、保护字写入、MPU 构建与
*                    栈帧初始化被调用且参数正确、加入就绪集的下标、返回值
*                    为"下标+1"、槽位复用、recover_pending=1 的槽被跳过、
*                    表满返回 -1 并记 NOSLOT、三道参数闸门各自返回 -1 并
*                    记 NOSLOT、失败槽不被永久占用（下次注册捡回）、栈填充
*                    跳过下标 0 且不越界、栈顶 8 字节对齐、peak_low 基准、
*                    以及 STACK_GUARD 开档的两行布防登记。
*            未验证：真实调度器下的就绪集行为（本用例只验下标传对了）、
*                    MPU 区域的实际内容（由 arkrtos_mpu.c 自己的用例负责）、
*                    只有上板才有的路径。
* @note 用法：由 tools/coverage_gate.py 的三条 case 编译并运行，输出
*       CFG-HOST OK（默认档）、CFG-HOST GUARD OK（STACK_GUARD=1 档）或
*       CFG-HOST NOUSAGE OK（STACK_USAGE=0 档）。
*/

#include "arkrtos_cfg.h"
#include "arkrtos_config.h"
#include "arkrtos_def.h"
#include "arkrtos_task.h"
#include "arkrtos_fault.h"
#include "arkrtos_hal.h"
#include "arkrtos_mpu.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------
 * 内核桩
 * ------------------------------------------------------------------ */
static int g_ready_reset_calls = 0;
static int g_ready_add_calls   = 0;
static int32 g_ready_add_last  = -1;

void arkrtos_ready_reset(void) { g_ready_reset_calls++; }

void arkrtos_ready_add(int32 task_idx)
{
    g_ready_add_calls++;
    g_ready_add_last = task_idx;
}

#if ((ARKRTOS_USE_MPU == 1) || (ARKRTOS_USE_PAGING == 1))
static int g_mpu_calls = 0;
static arkrtos_task_t *g_mpu_last = 0;
void arkrtos_mpu_build_task(arkrtos_task_t *p_task)
{
    g_mpu_calls++;
    g_mpu_last = p_task;
}
#endif

static int    g_fault_calls = 0;
static uint32 g_fault_type  = 0u;
static int32  g_fault_task  = -1;
void arkrtos_fault_record(uint32 type, int32 task_id)
{
    g_fault_calls++;
    g_fault_type = type;
    g_fault_task = task_id;
}

static int    g_psi_calls = 0;
static unsigned long g_psi_top = 0ul;
static void (*g_psi_entry)(void) = 0;
static unsigned long g_psi_ret = 0x1234u;

unsigned long arkrtos_port_stack_init(unsigned long stack_top, void (*entry)(void))
{
    g_psi_calls++;
    g_psi_top   = stack_top;
    g_psi_entry = entry;
    return g_psi_ret;
}

/* ------------------------------------------------------------------
 * 断言
 * ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL: %s\n", what);
    }
}

#define CK_EQ(a, b, msg)                                                     \
    do {                                                                     \
        long va = (long)(a);                                                 \
        long vb = (long)(b);                                                 \
        g_checks++;                                                          \
        if(va != vb)                                                         \
        {                                                                    \
            g_fails++;                                                       \
            printf("FAIL %s:%d %s (got %ld, want %ld)\n",                    \
                   __FILE__, __LINE__, msg, va, vb);                         \
        }                                                                    \
    } while(0)

static void task_entry(void) { }

/* 每个用例从"全清"开始：表归零（enum 0 == INVALID），计数归零，桩计数归零。 */
static void reset_all(void)
{
    int32 i;

    memset(&arkrtos_task_table[0], 0, sizeof(arkrtos_task_table));
    for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
    {
        arkrtos_task_table[i].status = ARKRTOS_TASK_INVALID;
    }
    arkrtos_task_count = 0;

    g_ready_reset_calls = 0;
    g_ready_add_calls   = 0;
    g_ready_add_last    = -1;
    g_fault_calls       = 0;
    g_fault_type        = 0u;
    g_fault_task        = -1;
    g_psi_calls         = 0;
    g_psi_top           = 0ul;
    g_psi_entry         = 0;
    g_psi_ret           = 0x1234u;
#if ((ARKRTOS_USE_MPU == 1) || (ARKRTOS_USE_PAGING == 1))
    g_mpu_calls = 0;
    g_mpu_last  = 0;
#endif
}

/* 一块可寻址、可逐字检查的栈。全局是为了让它有稳定地址。 */
static uint32 g_stack[256];

/* ------------------------------------------------------------------
 * 用例
 * ------------------------------------------------------------------ */
static void test_cfg_load(void)
{
    reset_all();
    arkrtos_task_count = 7;
    arkrtos_cfg_load();
    CK_EQ(arkrtos_task_count, 0, "cfg_load clears the task count");
    CK_EQ(g_ready_reset_calls, 1, "cfg_load resets the ready set exactly once");
}

static void test_register_success(void)
{
    int32 id;
    arkrtos_task_t *t;
    uint32 i;

    reset_all();
    for(i = 0; i < 256u; i++) { g_stack[i] = 0xAAAAAAAAu; }

    id = arkrtos_task_register(task_entry, g_stack, 1024u, 3u, 10u);
    t = &arkrtos_task_table[0];

    CK_EQ(id, 1, "first task gets id 1 (index + 1)");
    CK_EQ(arkrtos_task_count, 1, "count grows to 1");
    CK_EQ(t->status, ARKRTOS_TASK_READY, "new task is READY");
    CK_EQ(t->ram_start, (unsigned long)(size_t)g_stack, "ram_start is the stack bottom");
    CK_EQ(t->ram_size, 1024u, "ram_size is the stack size");
    CK_EQ(t->stack_size, 1024u, "stack_size is the stack size");
    CK_EQ(t->rom_start, 0u, "rom_start is zero for a kernel task");
    CK_EQ(t->rom_size, 0u, "rom_size is zero for a kernel task");
    /* MS_TO_TICK(10) = 10 * 1000 / 500 = 20 */
    CK_EQ(t->period, 20, "period_ms is converted to ticks");
    CK_EQ(t->period_time, 20, "period_time starts equal to period");
    CK_EQ(t->priority, 3u, "priority is stored");
    CK_EQ(t->base_priority, 3u, "base_priority mirrors priority");
    CK_EQ(t->shm_attri, 0u, "shm_attri is cleared");
    CK_EQ(t->is_priv, 1u, "kernel-created task runs privileged");
    CK_EQ(t->wait_time, 0, "wait_time is cleared");
    CK_EQ(t->tim_tick, 0u, "tim_tick is cleared");
    CK_EQ(t->touch_tick, 0u, "touch_tick is cleared");
    CK_EQ(t->wake_reason, 0u, "wake_reason is cleared");
    CK_EQ(t->recover_pending, 0u, "recover_pending is cleared");
    CK_EQ(t->ready_next, ARKRTOS_TASK_NIL, "ready_next is NIL");
    CK_EQ(t->ready_prev, ARKRTOS_TASK_NIL, "ready_prev is NIL");
    CK_EQ(t->delay_next, ARKRTOS_TASK_NIL, "delay_next is NIL");
    CK_EQ(t->delay_prev, ARKRTOS_TASK_NIL, "delay_prev is NIL");
    CK_EQ(t->delay_queued, 0u, "delay_queued is cleared");
    CK_EQ(t->delay_tick, 0u, "delay_tick is cleared");
    CK_EQ(t->slice_tick, (int32)ARKRTOS_TIME_SLICE_TICKS, "slice_tick is a full slice");

    CK_EQ(g_stack[0], ARKRTOS_STACK_END_FLAG_VAL, "stack bottom flag is written");
#if (ARKRTOS_USE_STACK_USAGE == 1)
    CK_EQ(g_stack[1], ARKRTOS_STACK_FILL_PATTERN, "fill starts right above the flag");
    CK_EQ(g_stack[255], ARKRTOS_STACK_FILL_PATTERN, "the whole stack is filled");
#endif

#if ((ARKRTOS_USE_MPU == 1) || (ARKRTOS_USE_PAGING == 1))
    CK_EQ(g_mpu_calls, 1, "the isolation context is built once");
    ck(g_mpu_last == t, "and it is built for the same TCB");
#endif
    CK_EQ(g_psi_calls, 1, "the port stack frame is initialised once");
    CK_EQ(g_psi_entry == task_entry, 1, "with the right entry");
    CK_EQ(g_psi_top % 8u, 0u, "stack top is 8-byte aligned");
    CK_EQ(g_psi_top, ((unsigned long)(size_t)g_stack + 1024ul) & ~0x7ul,
          "stack top is bottom + size, aligned down");
    CK_EQ(t->stack_bottom == g_stack, 1, "stack_bottom points at the given buffer");
    CK_EQ(t->stack_top, g_psi_top, "stack_top is what the port was handed");
    CK_EQ(t->entry == task_entry, 1, "entry is recorded for rebuild");
    CK_EQ(t->stack_ptr, g_psi_ret, "stack_ptr is the port's frame pointer");
#if (ARKRTOS_USE_STACK_USAGE == 1)
    CK_EQ(t->stack_peak_low, g_psi_top, "peak low starts at the empty stack top");
#endif
#if (ARKRTOS_USE_STACK_GUARD == 1)
    CK_EQ(t->stack_guard_armed, 1u, "the guard word is registered as armed");
    CK_EQ(t->stack_guard_reported, 0u, "and it has not been reported yet");
#endif

    CK_EQ(g_ready_add_calls, 1, "the new task is put on the ready set once");
    CK_EQ(g_ready_add_last, 0, "with its own index");
    CK_EQ(g_fault_calls, 0, "a clean registration records no fault");
}

static void test_register_slot_reuse(void)
{
    int32 id;

    /* 场景 1：一个已释放（INVALID、无待恢复）的槽应被复用，表不扩。 */
    reset_all();
    arkrtos_task_count = 1;
    arkrtos_task_table[0].status = ARKRTOS_TASK_INVALID;
    arkrtos_task_table[0].recover_pending = 0u;

    id = arkrtos_task_register(task_entry, g_stack, 1024u, 1u, 10u);
    CK_EQ(id, 1, "a released slot is reused");
    CK_EQ(arkrtos_task_count, 1, "and the table does not grow");

    /* 场景 2：带 recover_pending 的 INVALID 槽是"正等着复活"的，必须跳过。 */
    reset_all();
    arkrtos_task_count = 1;
    arkrtos_task_table[0].status = ARKRTOS_TASK_INVALID;
    arkrtos_task_table[0].recover_pending = 1u;

    id = arkrtos_task_register(task_entry, g_stack, 1024u, 1u, 10u);
    CK_EQ(id, 2, "a pending-recovery slot is skipped, so the next slot is used");
    CK_EQ(arkrtos_task_count, 2, "and the table grows by one");
    CK_EQ(arkrtos_task_table[0].recover_pending, 1u,
          "the pending slot is left untouched");

    /* 场景 3：第一个槽被占用、第二个已释放 —— 取第一个可用的。 */
    reset_all();
    arkrtos_task_count = 2;
    arkrtos_task_table[0].status = ARKRTOS_TASK_READY;
    arkrtos_task_table[1].status = ARKRTOS_TASK_INVALID;
    arkrtos_task_table[1].recover_pending = 0u;

    id = arkrtos_task_register(task_entry, g_stack, 1024u, 1u, 10u);
    CK_EQ(id, 2, "the first reusable slot wins even when it is not slot 0");
    CK_EQ(arkrtos_task_count, 2, "and no growth happens");
}

static void test_register_table_full(void)
{
    int32 i;

    reset_all();
    for(i = 0; i < ARKRTOS_TASK_MAX_NUM; i++)
    {
        arkrtos_task_table[i].status = ARKRTOS_TASK_READY;
        arkrtos_task_table[i].recover_pending = 0u;
    }
    arkrtos_task_count = ARKRTOS_TASK_MAX_NUM;

    CK_EQ(arkrtos_task_register(task_entry, g_stack, 1024u, 1u, 10u), -1,
          "a full table refuses registration");
    CK_EQ(g_fault_calls, 1, "and records one fault");
    CK_EQ(g_fault_type, (uint32)ARKRTOS_FAULT_NOSLOT, "of type NOSLOT");
    CK_EQ(arkrtos_task_count, ARKRTOS_TASK_MAX_NUM, "the count does not move");
    CK_EQ(g_psi_calls, 0, "no stack frame is built for a refused task");
    CK_EQ(g_ready_add_calls, 0, "and nothing reaches the ready set");
}

static void test_register_param_gates(void)
{
    int32 id;

    /* entry == 0 */
    reset_all();
    CK_EQ(arkrtos_task_register(0, g_stack, 1024u, 1u, 10u), -1,
          "a null entry is refused");
    CK_EQ(g_fault_type, (uint32)ARKRTOS_FAULT_NOSLOT, "null entry records NOSLOT");
    CK_EQ(g_psi_calls, 0, "no frame for a null entry");
    CK_EQ(g_ready_add_calls, 0, "nothing reaches the ready set");

    /* stack_bottom == 0 */
    reset_all();
    CK_EQ(arkrtos_task_register(task_entry, 0, 1024u, 1u, 10u), -1,
          "a null stack is refused");
    CK_EQ(g_fault_type, (uint32)ARKRTOS_FAULT_NOSLOT, "null stack records NOSLOT");

    /* stack_size < 64 */
    reset_all();
    g_stack[0] = 0xAAAAAAAAu;
    CK_EQ(arkrtos_task_register(task_entry, g_stack, 63u, 1u, 10u), -1,
          "a 63-byte stack is refused");
    CK_EQ(g_fault_type, (uint32)ARKRTOS_FAULT_NOSLOT, "short stack records NOSLOT");
    CK_EQ(g_stack[0], 0xAAAAAAAAu, "the refused call never writes the stack flag");

    /* 64 字节是边界，应通过（这里只验闸门，不重复验字段）。 */
    reset_all();
    memset(g_stack, 0, sizeof(g_stack));
    CK_EQ(arkrtos_task_register(task_entry, g_stack, 64u, 1u, 10u), 1,
          "64 bytes is the smallest accepted stack");

    /* 失败不永久占槽：三次失败的调用把 count 推到 3，但槽位仍是 INVALID，
     * 下一次成功注册会捡回第 0 号槽，count 不再增长。 */
    reset_all();
    (void)arkrtos_task_register(0, g_stack, 1024u, 1u, 10u);
    id = arkrtos_task_register(task_entry, g_stack, 1024u, 1u, 10u);
    CK_EQ(id, 1, "the slot burned by a failed call is reused");
    CK_EQ(arkrtos_task_count, 1, "and the count is not leaked");
}

#if (ARKRTOS_USE_STACK_USAGE == 1)
static void test_stack_fill(void)
{
    uint32 i;
    uint32 buf[8];

    for(i = 0; i < 8u; i++) { buf[i] = 0xAAu; }
    arkrtos_task_stack_fill(buf, 32u);   /* 8 words */

    CK_EQ(buf[0], 0xAAu, "fill leaves index 0 (the guard word) alone");
    for(i = 1u; i < 8u; i++)
    {
        CK_EQ(buf[i], ARKRTOS_STACK_FILL_PATTERN, "fill covers the rest");
    }

    /* 非 4 字节整除的大小：只按整字填，尾部不动。 */
    for(i = 0; i < 8u; i++) { buf[i] = 0xAAu; }
    arkrtos_task_stack_fill(buf, 18u);   /* 4 words + 2 bytes */
    CK_EQ(buf[0], 0xAAu, "index 0 untouched again");
    CK_EQ(buf[1], ARKRTOS_STACK_FILL_PATTERN, "word 1 filled");
    CK_EQ(buf[4], 0xAAu, "the 5th word is beyond 18/4 and stays untouched");
}
#endif

static void test_stack_init(void)
{
    arkrtos_task_t t;
    uint32 buf[64];
    uint32 i;
    unsigned long want_top;

    reset_all();
    memset(&t, 0, sizeof(t));
    for(i = 0; i < 64u; i++) { buf[i] = 0xAAu; }
    g_psi_ret = 0xCAFEu;

    /* size 不是 8 的倍数且基址任意：栈顶必须向下对齐到 8 字节。 */
    arkrtos_task_stack_init(&t, task_entry, buf, 250u);
    want_top = ((unsigned long)(size_t)buf + 250ul) & ~0x7ul;

    CK_EQ(t.stack_bottom == buf, 1, "stack_bottom is recorded");
    CK_EQ(t.stack_top, want_top, "stack_top is aligned down to 8 bytes");
    CK_EQ(t.entry == task_entry, 1, "entry is recorded");
    CK_EQ(t.stack_ptr, 0xCAFEu, "stack_ptr comes from the port layer");
    CK_EQ(g_psi_calls, 1, "the port initialiser is called once");
    CK_EQ(g_psi_top, want_top, "and it is handed the aligned top");

#if (ARKRTOS_USE_STACK_USAGE == 1)
    CK_EQ(t.stack_peak_low, want_top, "peak low starts at the aligned top");
    CK_EQ(buf[0], 0xAAu, "stack_init never writes index 0 on its own");
    CK_EQ(buf[1], ARKRTOS_STACK_FILL_PATTERN, "stack_init fills the stack");
    CK_EQ(buf[61], ARKRTOS_STACK_FILL_PATTERN, "fill covers words 1..61");
    CK_EQ(buf[62], 0xAAu, "word 62 is beyond 250/4 and stays untouched");
#else
    CK_EQ(buf[0], 0xAAu, "with stack usage off the stack is not touched");
    CK_EQ(buf[1], 0xAAu, "and neither is the rest of it");
#endif
}

/* ------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------ */
int main(void)
{
    test_cfg_load();
    test_register_success();
    test_register_slot_reuse();
    test_register_table_full();
    test_register_param_gates();
#if (ARKRTOS_USE_STACK_USAGE == 1)
    test_stack_fill();
#endif
    test_stack_init();

    if(g_fails != 0)
    {
        printf("CFG-HOST FAIL %d/%d\n", g_fails, g_checks);
        return 1;
    }
#if (ARKRTOS_USE_STACK_GUARD == 1)
    printf("CFG-HOST GUARD OK %d checks\n", g_checks);
#elif (ARKRTOS_USE_STACK_USAGE == 0)
    printf("CFG-HOST NOUSAGE OK %d checks\n", g_checks);
#else
    printf("CFG-HOST OK %d checks\n", g_checks);
#endif
    return 0;
}
