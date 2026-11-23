/**
* @file test_paging_host.c
* @brief 分页隔离后端（arkrtos_paging.c）的宿主用例（不需要板子）
*
* 被测的是 ARKRTOS_USE_PAGING=1 那一份后端实现：它把内核的五个隔离入口
* （module_init / build_task / set_app / set_idle / reset）与空间、帧池、
* 动态映射、受控用户拷贝这四组接口，翻译成对 port 的调用与自己的账目。
*
* 钉死的都是「错了也看不出来」的地方：
*   - 跨度审计：只有 rom_start 非 0 的任务才审计（内核任务不重复问常量问题）、
*     代码在 RAM 池里按 RAMX 审计而在镜像池里按 ROM、size==0 不计数、
*     坏跨度只记第一次的基址、module_init 清掉全部计数与任务绑定；
*   - 切换：内核任务（is_priv）不切空间；不在任务表里的指针一律拒绝而不是
*     按越界下标写 s_pg_task_space；绑定时不做切换（延迟到调度点）；
*   - 动态映射：4 KB 对齐、零长度、>16 页三条闸门在本地判，port 的拒绝码
*     原样透出；半路失败时只把已经 land 的页计入读数；
*   - 撤销映射：半路失败时读数只减已经撤掉的页；
*   - 受控用户拷贝：参数 / 没有用户窗口 / 空间不是当前装着的那一个 / 整段
*     越出窗口 / 写方向只认数据窗口，五种拒绝各自对应 -1/-2/-4/-3/-3，
*     并且拒绝发生在第一次 load/store 之前（用例拿窗口外的真实内存做对照，
*     确认一个字节都没被动过）。
*
* 打桩范围：port 的 19 个分页入口、任务表与 arkrtos_task_get_current。
* 也就是说本用例证明的是这一层的算术、闸门与账目，**不证明** TLB 真的换了、
* 用户态真的进不去（那些是 QEMU 与板子的读数）。
*
* 口径写死一条：宿主是 ILP32，`unsigned long` 只有 32 位，所以
* `uva + len` / `base + size` 的回绕用例用 32 位边界构造。LA64 上同一段
* 判据的宽度是 64 位——本用例证明的是分支成立时的行为，不是宽度本身。
*/

#include <stdio.h>
#include <string.h>

#include "arkrtos_mpu.h"
#include "arkrtos_hal.h"
#include "arkrtos_cfg.h"
#include "arkrtos_task.h"
#include "arkrtos_partition.h"

/* ------------------------------------------------------------------ */
/* 断言                                                                */
/* ------------------------------------------------------------------ */
static int g_checks = 0;
static int g_fails  = 0;

static void ck_true(int cond, const char *what, int line)
{
    g_checks++;
    if(!cond)
    {
        g_fails++;
        printf("FAIL line %d: %s\n", line, what);
    }
}

static void ck_u(uint32 got, uint32 want, const char *what, int line)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL line %d: %s (got 0x%X want 0x%X)\n", line, what,
               (unsigned)got, (unsigned)want);
    }
}

static void ck_i(int32 got, int32 want, const char *what, int line)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL line %d: %s (got %d want %d)\n", line, what,
               (int)got, (int)want);
    }
}

#define CK(cond)          ck_true((cond) ? 1 : 0, #cond, __LINE__)
#define CK_U(got, want)   ck_u((uint32)(got), (uint32)(want), #got, __LINE__)
#define CK_I(got, want)   ck_i((int32)(got), (int32)(want), #got, __LINE__)

/* ------------------------------------------------------------------ */
/* 任务表与当前任务（arkrtos_paging.c 只用这两个）                      */
/* ------------------------------------------------------------------ */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];

static arkrtos_task_t *g_current;
static int32 g_current_calls;

arkrtos_task_t *arkrtos_task_get_current(void)
{
    g_current_calls++;
    return g_current;
}

/* ------------------------------------------------------------------ */
/* port 桩                                                             */
/* ------------------------------------------------------------------ */
#define WINDOW_VA       0x02000000UL
#define FRAME0_PA       0x30000000UL
#define FRAME_STEP      0x00200000UL

static int32  g_init_rc;
static int32  g_check_rc;
static uint32 g_check_calls;
static unsigned long g_check_base;
static uint32 g_check_size;
static arkrtos_mpu_mem_t g_check_mem;

static uint32 g_space_count = 4u;
static uint32 g_cur_space;
static int32  g_activate_rc;
static int32  g_switch_rc;
static uint32 g_activate_calls;
static uint32 g_switch_calls;
static uint32 g_activate_arg;
static uint32 g_switch_arg;

static int32  g_release_rc;

static int32  g_alias_fail_at;      /* <0 = never fail; else 0-based page index */
static int32  g_alias_fail_code;
static int32  g_unalias_fail_at;
static int32  g_unalias_fail_code;
static uint32 g_alias_calls;
static uint32 g_unalias_calls;
static uint32 g_alias_va[32];
static uint32 g_alias_pa[32];
static arkrtos_mpu_mem_t g_alias_mem[32];
static uint32 g_unalias_va[32];

static uint32 g_dyn_used;
static uint32 g_flush_calls;

#if (ARKRTOS_USE_SMP == 1)
/* R3：绑定新空间时 paging 侧要请 port 在**新窗口下**把初始帧重造一遍。
 * 重造本身是 port 的事（页表在那一侧），这里只记"请了谁、带什么参数、
 * 被拒绝了会怎样"。 */
static int32  g_reloc_rc;
static uint32 g_reloc_calls;
static uint32 g_reloc_space;
static void  *g_reloc_frame;
static unsigned long g_reloc_stack_top;
static void  *g_reloc_entry;

int32 arkrtos_port_paging_relocate_frame(uint32 space, void *frame,
                                         unsigned long stack_top,
                                         void *entry);

int32 arkrtos_port_paging_relocate_frame(uint32 space, void *frame,
                                         unsigned long stack_top,
                                         void *entry)
{
    g_reloc_calls++;
    g_reloc_space     = space;
    g_reloc_frame     = frame;
    g_reloc_stack_top = stack_top;
    g_reloc_entry     = entry;
    return g_reloc_rc;
}
#endif

int32 arkrtos_port_paging_init(void)
{
    return g_init_rc;
}

int32 arkrtos_port_paging_check(unsigned long base, uint32 size, arkrtos_mpu_mem_t mem)
{
    g_check_calls++;
    g_check_base = base;
    g_check_size = size;
    g_check_mem  = mem;
    return g_check_rc;
}

int32 arkrtos_port_paging_alias(uint32 va, uint32 pa, arkrtos_mpu_mem_t mem)
{
    if((g_alias_fail_at >= 0) && (g_alias_calls == (uint32)g_alias_fail_at))
    {
        g_alias_calls++;
        return g_alias_fail_code;
    }
    if(g_alias_calls < 32u)
    {
        g_alias_va[g_alias_calls]  = va;
        g_alias_pa[g_alias_calls]  = pa;
        g_alias_mem[g_alias_calls] = mem;
    }
    g_alias_calls++;
    return 0;
}

int32 arkrtos_port_paging_unalias(uint32 va)
{
    if((g_unalias_fail_at >= 0) && (g_unalias_calls == (uint32)g_unalias_fail_at))
    {
        g_unalias_calls++;
        return g_unalias_fail_code;
    }
    if(g_unalias_calls < 32u)
    {
        g_unalias_va[g_unalias_calls] = va;
    }
    g_unalias_calls++;
    return 0;
}

uint32 arkrtos_port_paging_dyn_used(void)
{
    return g_dyn_used;
}

void arkrtos_port_paging_flush(uint32 base, uint32 size)
{
    (void)base;
    (void)size;
    g_flush_calls++;
}

int32 arkrtos_port_paging_activate(uint32 space)
{
    g_activate_calls++;
    g_activate_arg = space;
    if(g_activate_rc == 0)
    {
        g_cur_space = space;
    }
    return g_activate_rc;
}

int32 arkrtos_port_paging_switch_to(uint32 space)
{
    g_switch_calls++;
    g_switch_arg = space;
    return g_switch_rc;
}

uint32 arkrtos_port_paging_space(void)
{
    return g_cur_space;
}

uint32 arkrtos_port_paging_space_frame(uint32 space)
{
    if(space >= g_space_count)
    {
        return 0u;
    }
    return (uint32)FRAME0_PA + (space * (uint32)FRAME_STEP);
}

uint32 arkrtos_port_paging_space_count(void)
{
    return g_space_count;
}

int32 arkrtos_port_paging_space_release(uint32 space)
{
    (void)space;
    return g_release_rc;
}

int32 arkrtos_port_paging_frame_alloc(uint32 count, uint32 *p_base)
{
    if(p_base != 0)
    {
        *p_base = 0x40000000u + count;
    }
    return -1;
}

int32 arkrtos_port_paging_frame_free(uint32 base, uint32 count)
{
    return (int32)(base + count);
}

uint32 arkrtos_port_paging_frame_total(void)
{
    return 512u;
}

uint32 arkrtos_port_paging_frame_used(void)
{
    return 3u;
}

uint32 arkrtos_port_paging_pair_total(void)
{
    return 256u;
}

uint32 arkrtos_port_paging_pair_count(uint32 space)
{
    return space + 1u;
}

uint32 arkrtos_port_paging_pair_pa(uint32 space, uint32 idx)
{
    return (space << 16) | idx;
}

unsigned long arkrtos_port_paging_window_va(void)
{
    return WINDOW_VA;
}

/* ------------------------------------------------------------------ */
/* 复位                                                                */
/* ------------------------------------------------------------------ */
static void reset_port(void)
{
    g_init_rc         = 0;
    g_check_rc        = 0;
    g_check_calls     = 0u;
    g_check_base      = 0ul;
    g_check_size      = 0u;
    g_check_mem       = ARKRTOS_MPU_MEM_RAM;
    g_space_count     = 4u;
    g_cur_space       = 0u;
    g_activate_rc     = 0;
    g_switch_rc       = 0;
    g_activate_calls  = 0u;
    g_switch_calls    = 0u;
    g_activate_arg    = 0xFFFFFFFFu;
    g_switch_arg      = 0xFFFFFFFFu;
    g_release_rc      = 0;
    g_alias_fail_at   = -1;
    g_alias_fail_code = -1;
    g_unalias_fail_at = -1;
    g_unalias_fail_code = -1;
    g_alias_calls     = 0u;
    g_unalias_calls   = 0u;
    g_dyn_used        = 0u;
    g_flush_calls     = 0u;
#if (ARKRTOS_USE_SMP == 1)
    g_reloc_rc        = 0;
    g_reloc_calls     = 0u;
    g_reloc_space     = 0xFFFFFFFFu;
    g_reloc_frame     = 0;
    g_reloc_stack_top = 0ul;
    g_reloc_entry     = 0;
#endif
    g_current         = 0;
    g_current_calls   = 0;
}

static void reset_all(void)
{
    uint32 i;

    reset_port();
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
    {
        arkrtos_task_table[i].is_priv = 1u;
    }
    arkrtos_mpu_module_init();
    reset_port();
}

/* ------------------------------------------------------------------ */
/* 1. module_init 的计数与绑定表清空                                    */
/* ------------------------------------------------------------------ */
static void test_module_init(void)
{
    uint32 checked = 9u, bad = 9u, base = 9u;

    CK_U(arkrtos_paging_stats(&checked, &bad, &base), 0u);
    CK_U(checked, 0u);
    CK_U(bad, 0u);
    CK_U(base, 0u);

    /* init 清掉计数：先制造一次坏跨度，再 init */
    arkrtos_task_table[1].rom_start = (unsigned long)CHIP_FLASH_BASE + 0x100ul;
    arkrtos_task_table[1].rom_size  = 0x100u;
    g_check_rc = -1;
    arkrtos_mpu_build_task(&arkrtos_task_table[1]);
    arkrtos_paging_stats(&checked, &bad, &base);
    CK_U(checked, 1u);
    CK_U(bad, 1u);
    CK_U(base, (uint32)CHIP_FLASH_BASE + 0x100u);

    /* 绑定也一起被清 */
    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[2], 2u), 0);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[2]), 2u);
    reset_port();
    arkrtos_mpu_module_init();
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[2]), 0u);
    arkrtos_paging_stats(&checked, &bad, &base);
    CK_U(checked, 0u);
    CK_U(bad, 0u);
    CK_U(base, 0u);

    /* port 报错：stats 返回 -1 但计数照旧可读 */
    g_init_rc = -1;
    arkrtos_mpu_module_init();
    CK_I(arkrtos_paging_stats(&checked, &bad, &base), -1);

    /* 三个输出指针都可以是空 */
    CK_I(arkrtos_paging_stats(0, 0, 0), -1);
}

/* ------------------------------------------------------------------ */
/* 2. build_task 的审计面                                               */
/* ------------------------------------------------------------------ */
static void test_build_task(void)
{
    uint32 checked = 0u, bad = 0u, base = 0u;
    unsigned long ram_base = (unsigned long)ARKRTOS_MPU_SLOT_RAM_BASE;
    unsigned long rom_base = (unsigned long)CHIP_FLASH_BASE + 0x10000ul;

    arkrtos_mpu_module_init();
    reset_port();

    /* 空指针：一个 port 调用都不该有 */
    arkrtos_mpu_build_task(0);
    CK_U(g_check_calls, 0u);

    /* 内核内部任务（rom_start == 0）：不审计，ram 也不看 */
    arkrtos_task_table[0].rom_start = 0ul;
    arkrtos_task_table[0].ram_start = 0x20001000ul;
    arkrtos_task_table[0].ram_size  = 0x100u;
    arkrtos_mpu_build_task(&arkrtos_task_table[0]);
    CK_U(g_check_calls, 0u);

    /* 镜像池里的代码：ROM 属性；RAM 窗口单独一条 */
    arkrtos_task_table[1].rom_start = rom_base;
    arkrtos_task_table[1].rom_size  = 0x2000u;
    arkrtos_task_table[1].ram_start = 0x20010000ul;
    arkrtos_task_table[1].ram_size  = 0x1000u;
    arkrtos_mpu_build_task(&arkrtos_task_table[1]);
    CK_U(g_check_calls, 2u);
    CK_U(g_check_size, 0x1000u);
    CK(bad == 0u);
    arkrtos_paging_stats(&checked, &bad, &base);
    CK_U(checked, 2u);
    CK_U(bad, 0u);

    /* RAM 池里的代码（MiniApp）：RAMX 属性 */
    arkrtos_mpu_module_init();
    reset_port();
    arkrtos_task_table[2].rom_start = ram_base + 0x1000ul;
    arkrtos_task_table[2].rom_size  = 0x2000u;
    arkrtos_task_table[2].ram_start = ram_base + 0x4000ul;
    arkrtos_task_table[2].ram_size  = 0x1000u;
    arkrtos_mpu_build_task(&arkrtos_task_table[2]);
    CK_U(g_check_calls, 2u);
    CK_U((uint32)g_check_mem, (uint32)ARKRTOS_MPU_MEM_RAM);

    /* 第一段（代码）的属性：单独看一次 */
    reset_port();
    arkrtos_mpu_module_init();
    arkrtos_task_table[2].rom_start = ram_base + 0x1000ul;
    arkrtos_task_table[2].rom_size  = 0x2000u;
    arkrtos_task_table[2].ram_size  = 0u;
    arkrtos_mpu_build_task(&arkrtos_task_table[2]);
    CK_U(g_check_calls, 1u);
    CK_U((uint32)g_check_mem, (uint32)ARKRTOS_MPU_MEM_RAMX);

    /* 第二段（数据）的属性 */
    arkrtos_task_table[2].rom_size  = 0u;
    arkrtos_task_table[2].ram_size  = 0x1000u;
    g_check_calls = 0u;
    arkrtos_mpu_build_task(&arkrtos_task_table[2]);
    CK_U(g_check_calls, 1u);
    CK_U((uint32)g_check_mem, (uint32)ARKRTOS_MPU_MEM_RAM);

    /* size == 0 不计数 */
    reset_port();
    arkrtos_mpu_module_init();
    arkrtos_task_table[3].rom_start = rom_base;
    arkrtos_task_table[3].rom_size  = 0u;
    arkrtos_task_table[3].ram_start = 0x20010000ul;
    arkrtos_task_table[3].ram_size  = 0u;
    arkrtos_mpu_build_task(&arkrtos_task_table[3]);
    CK_U(g_check_calls, 0u);

    /* 坏跨度：每段都计数，bad_base 只记第一次 */
    arkrtos_mpu_module_init();
    reset_port();
    g_check_rc = -1;
    arkrtos_task_table[4].rom_start = rom_base;
    arkrtos_task_table[4].rom_size  = 0x2000u;
    arkrtos_task_table[4].ram_start = 0x20010000ul;
    arkrtos_task_table[4].ram_size  = 0x1000u;
    arkrtos_mpu_build_task(&arkrtos_task_table[4]);
    arkrtos_task_table[5].rom_start = rom_base + 0x8000ul;
    arkrtos_task_table[5].rom_size  = 0x100u;
    arkrtos_mpu_build_task(&arkrtos_task_table[5]);
    arkrtos_paging_stats(&checked, &bad, &base);
    CK_U(checked, 3u);
    CK_U(bad, 3u);
    CK_U(base, (uint32)rom_base);
}

/* ------------------------------------------------------------------ */
/* 3. set_app / set_idle / reset                                        */
/* ------------------------------------------------------------------ */
static void test_set_app(void)
{
    static arkrtos_task_t stray;

    arkrtos_mpu_module_init();
    reset_port();

    arkrtos_mpu_set_app(0);
    CK_U(g_switch_calls, 0u);

    /* 内核任务不切空间（含义是「现在装着谁」，不是「我自己的」） */
    arkrtos_task_table[1].is_priv = 1u;
    arkrtos_mpu_set_app(&arkrtos_task_table[1]);
    CK_U(g_switch_calls, 0u);

    /* 不在任务表里的指针：按越界下标拒绝，不写绑定表 */
    stray.is_priv = 0u;
    arkrtos_mpu_set_app(&stray);
    CK_U(g_switch_calls, 0u);

    /* 表尾之后一格 */
    arkrtos_task_table[ARKRTOS_TASK_MAX_NUM - 1].is_priv = 0u;
    arkrtos_mpu_set_app(&arkrtos_task_table[ARKRTOS_TASK_MAX_NUM]);
    CK_U(g_switch_calls, 0u);

    /* 正常的 App 任务：按绑定号调 switch_to（不是 activate） */
    arkrtos_task_table[6].is_priv = 0u;
    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[6], 2u), 0);
    arkrtos_mpu_set_app(&arkrtos_task_table[6]);
    CK_U(g_switch_calls, 1u);
    CK_U(g_switch_arg, 2u);
    CK_U(g_activate_calls, 0u);

    /* 未绑定 = 空间 0 */
    arkrtos_task_table[7].is_priv = 0u;
    arkrtos_mpu_set_app(&arkrtos_task_table[7]);
    CK_U(g_switch_calls, 2u);
    CK_U(g_switch_arg, 0u);

    /* set_idle 是空的；reset 回到空间 0 */
    arkrtos_mpu_set_idle();
    CK_U(g_switch_calls, 2u);
    CK_U(g_activate_calls, 0u);
    arkrtos_mpu_reset();
    CK_U(g_activate_calls, 1u);
    CK_U(g_activate_arg, 0u);
}

/* ------------------------------------------------------------------ */
/* 4. 空间转发与 activate                                               */
/* ------------------------------------------------------------------ */
static void test_space_forwards(void)
{
    reset_port();
    arkrtos_mpu_module_init();

    CK_U(arkrtos_paging_space_count(), 4u);
    CK_U(arkrtos_paging_space_frame(0u), 0x30000000u);
    CK_U(arkrtos_paging_space_frame(3u), 0x30000000u + 3u * 0x00200000u);
    CK_U(arkrtos_paging_space_frame(4u), 0u);
    CK_U((uint32)arkrtos_paging_window_va(), 0x02000000u);
    CK_U(arkrtos_paging_space_current(), 0u);

    /* 越界空间：本地就拒绝，port 一次都不该被调 */
    CK_I(arkrtos_paging_space_activate(4u), -1);
    CK_U(g_activate_calls, 0u);

    CK_I(arkrtos_paging_space_activate(2u), 0);
    CK_U(g_activate_calls, 1u);
    CK_U(g_activate_arg, 2u);
    CK_U(arkrtos_paging_space_current(), 2u);

    /* port 的拒绝码原样透出 */
    g_activate_rc = -1;
    CK_I(arkrtos_paging_space_activate(1u), -1);
    CK_U(g_activate_calls, 2u);
    CK_U(g_activate_arg, 1u);
}

/* ------------------------------------------------------------------ */
/* 5. 绑定与查询                                                        */
/* ------------------------------------------------------------------ */
static void test_bind_and_of(void)
{
    static arkrtos_task_t stray;

    arkrtos_mpu_module_init();
    reset_port();

    CK_I(arkrtos_paging_bind_task(0, 0u), -1);
    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[1], 4u), -1);
    stray.is_priv = 0u;
    CK_I(arkrtos_paging_bind_task(&stray, 1u), -1);

    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[1], 3u), 0);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[1]), 3u);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[2]), 0u);

    CK_U(arkrtos_paging_space_of(0), 0u);
    CK_U(arkrtos_paging_space_of(&stray), 0u);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[ARKRTOS_TASK_MAX_NUM]), 0u);

    /* 重新绑定覆盖旧值（不叠加） */
    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[1], 1u), 0);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[1]), 1u);

#if (ARKRTOS_USE_SMP == 1)
    /* R3：SMP 档多一道动作——请 port 在**新窗口下**重造初始帧。
     * 单核档没有"窗口此刻装着别的空间"这条路，整段编掉（见 paging.c）。 */
    arkrtos_mpu_module_init();
    reset_port();

    arkrtos_task_table[3].stack_ptr  = 0x00012340ul;
    arkrtos_task_table[3].stack_top  = 0x00012400ul;
    arkrtos_task_table[3].entry      = (void (*)(void))0x00001001ul;

    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[3], 2u), 0);
    CK_U(g_reloc_calls, 1u);
    CK_U(g_reloc_space, 2u);
    CK(g_reloc_frame == (void *)arkrtos_task_table[3].stack_ptr);
    CK_U((uint32)g_reloc_stack_top, 0x00012400u);
    CK(g_reloc_entry == (void *)arkrtos_task_table[3].entry);

    /* port 拒绝重造：不许留半截绑定，返回码必须与"参数非法"(-1) 分得开。 */
    g_reloc_rc = -3;
    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[4], 3u), -2);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[4]), 0u);
    CK_U(g_reloc_calls, 2u);
    g_reloc_rc = 0;

    /* 失败后重新绑定成功：半截状态已经被清掉，不是"卡死在那里"。 */
    CK_I(arkrtos_paging_bind_task(&arkrtos_task_table[4], 3u), 0);
    CK_U(arkrtos_paging_space_of(&arkrtos_task_table[4]), 3u);
#endif
}

/* ------------------------------------------------------------------ */
/* 6. 空间回收与帧池/对转发                                             */
/* ------------------------------------------------------------------ */
static void test_release_and_pool(void)
{
    uint32 base = 0u;

    reset_port();

    CK_I(arkrtos_paging_space_release(4u), -1);
    CK_I(arkrtos_paging_space_release(0u), 0);

    g_release_rc = -2;
    CK_I(arkrtos_paging_space_release(1u), -2);
    g_release_rc = -3;
    CK_I(arkrtos_paging_space_release(2u), -3);
    g_release_rc = -4;
    CK_I(arkrtos_paging_space_release(3u), -4);

    CK_I(arkrtos_paging_frame_alloc(2u, &base), -1);
    CK_U(base, 0x40000002u);
    CK_I(arkrtos_paging_frame_free(0x1000u, 2u), 0x1002);
    CK_U(arkrtos_paging_frame_total(), 512u);
    CK_U(arkrtos_paging_frame_used(), 3u);

    CK_U(arkrtos_paging_pair_total(), 256u);
    CK_U(arkrtos_paging_pair_count(2u), 3u);
    CK_U(arkrtos_paging_pair_pa(2u, 5u), 0x00020005u);
}

/* ------------------------------------------------------------------ */
/* 7. 动态映射注册                                                      */
/* ------------------------------------------------------------------ */
static void test_map_span(void)
{
    arkrtos_mpu_module_init();
    reset_port();

    /* 三条本地闸门：零长度 / 非 4 KB 对齐（va、pa、size 各一条） */
    CK_I(arkrtos_paging_map_span(0x1000u, 0x2000u, 0u, ARKRTOS_MPU_MEM_RAM), -1);
    CK_I(arkrtos_paging_map_span(0x1000u, 0x2000u, 0x800u, ARKRTOS_MPU_MEM_RAM), -1);
    CK_I(arkrtos_paging_map_span(0x1800u, 0x2000u, 0x1000u, ARKRTOS_MPU_MEM_RAM), -1);
    CK_I(arkrtos_paging_map_span(0x1000u, 0x2800u, 0x1000u, ARKRTOS_MPU_MEM_RAM), -1);
    CK_U(g_alias_calls, 0u);

    /* 超过 16 页上限（16 页正好可以，17 页拒绝） */
    CK_I(arkrtos_paging_map_span(0x1000u, 0x2000u, 16u * 0x1000u, ARKRTOS_MPU_MEM_RAM), 0);
    CK_U(g_alias_calls, 16u);
    CK_U(arkrtos_paging_map_pages(), 16u);
    CK_U(g_alias_va[0], 0x1000u);
    CK_U(g_alias_pa[0], 0x2000u);
    CK_U(g_alias_va[15], 0x1000u + 15u * 0x1000u);
    CK_U(g_alias_pa[15], 0x2000u + 15u * 0x1000u);
    CK_U((uint32)g_alias_mem[0], (uint32)ARKRTOS_MPU_MEM_RAM);
    CK_I(arkrtos_paging_map_span(0x1000u, 0x2000u, 17u * 0x1000u, ARKRTOS_MPU_MEM_RAM), -1);
    CK_U(g_alias_calls, 16u);

    /* 半路失败：port 的原因码（-3 = 动态表满）原样透出，读数只算 land 的页 */
    arkrtos_mpu_module_init();
    reset_port();
    g_alias_fail_at   = 2;
    g_alias_fail_code = -3;
    CK_I(arkrtos_paging_map_span(0x100000u, 0x200000u, 4u * 0x1000u, ARKRTOS_MPU_MEM_PERIPH_RW), -3);
    CK_U(g_alias_calls, 3u);
    CK_U(arkrtos_paging_map_pages(), 2u);
    CK_U((uint32)g_alias_mem[1], (uint32)ARKRTOS_MPU_MEM_PERIPH_RW);

    /* 动态槽位读数转发 */
    g_dyn_used = 5u;
    CK_U(arkrtos_paging_dyn_used(), 5u);
}

/* ------------------------------------------------------------------ */
/* 8. 动态映射撤销                                                      */
/* ------------------------------------------------------------------ */
static void test_unmap_span(void)
{
    arkrtos_mpu_module_init();
    reset_port();

    CK_I(arkrtos_paging_unmap_span(0x1000u, 0u), -1);
    CK_I(arkrtos_paging_unmap_span(0x1000u, 0x800u), -1);
    CK_I(arkrtos_paging_unmap_span(0x1800u, 0x1000u), -1);
    CK_U(g_unalias_calls, 0u);

    /* 先登记 3 页，再整段撤掉 */
    CK_I(arkrtos_paging_map_span(0x300000u, 0x400000u, 3u * 0x1000u, ARKRTOS_MPU_MEM_RAM), 0);
    CK_U(arkrtos_paging_map_pages(), 3u);
    CK_I(arkrtos_paging_unmap_span(0x300000u, 3u * 0x1000u), 0);
    CK_U(g_unalias_calls, 3u);
    CK_U(g_unalias_va[0], 0x300000u);
    CK_U(g_unalias_va[2], 0x302000u);
    CK_U(arkrtos_paging_map_pages(), 0u);

    /* 半路失败：读数只减已经撤掉的那一页，原因码原样透出 */
    arkrtos_mpu_module_init();
    reset_port();
    CK_I(arkrtos_paging_map_span(0x500000u, 0x600000u, 3u * 0x1000u, ARKRTOS_MPU_MEM_RAM), 0);
    g_unalias_fail_at   = 1;
    g_unalias_fail_code = -4;
    CK_I(arkrtos_paging_unmap_span(0x500000u, 3u * 0x1000u), -4);
    CK_U(g_unalias_calls, 2u);
    CK_U(arkrtos_paging_map_pages(), 2u);
}

/* ------------------------------------------------------------------ */
/* 9. 受控用户拷贝                                                      */
/* ------------------------------------------------------------------ */
static void test_user_copy(void)
{
    uint8 kern[64];
    uint8 user[64];
    uint8 guard[64];
    unsigned long rom = (unsigned long)user;
    unsigned long ram = (unsigned long)guard;
    uint32 i;

    for(i = 0u; i < 64u; i++)
    {
        kern[i] = (uint8)(0xA0u + i);
        user[i] = (uint8)i;
        guard[i] = (uint8)(0x50u + i);
    }

    /* --- 参数闸门 --- */
    CK_I(arkrtos_paging_user_copy(0ul, 0u, 0ul, 0u, 0u, 0u, 0, user, 4u, 0u), -1);
    CK_I(arkrtos_paging_user_copy(0ul, 0u, 0ul, 0u, 0u, 0u, kern, 0, 4u, 0u), -1);
    CK_I(arkrtos_paging_user_copy(0ul, 0u, 0ul, 0u, 0u, 0u, kern, user, 0u, 0u), -1);
    /* uva + len 回绕（32 位宿主口径） */
    CK_I(arkrtos_paging_user_copy(0ul, 0u, 0ul, 0u, 0u, 0u, kern,
                                  (const void *)0xFFFFFFF0ul, 0x20u, 0u), -1);

    /* --- 没有用户窗口 --- */
    CK_I(arkrtos_paging_user_copy(0ul, 0u, 0ul, 0u, 0u, 0u, kern, user, 4u, 0u), -2);

    /* --- 装着的不是自己的空间：在窗口判定之前就拒绝 --- */
    CK_I(arkrtos_paging_user_copy(rom, 64u, ram, 64u, 1u, 0u, kern, user, 4u, 0u), -4);

    /* --- 读：整段落在代码窗口里 --- */
    memset(kern, 0, sizeof(kern));
    CK_I(arkrtos_paging_user_copy(rom, 64u, ram, 64u, 0u, 0u, kern, user + 8, 8u, 0u), 0);
    CK_U(kern[0], 8u);
    CK_U(kern[7], 15u);

    /* --- 读：整段落在数据窗口里 --- */
    memset(kern, 0, sizeof(kern));
    CK_I(arkrtos_paging_user_copy(rom, 64u, ram, 64u, 3u, 3u, kern, guard + 4, 4u, 0u), 0);
    CK_U(kern[0], 0x54u);

    /* --- 读：越过窗口末尾一个字节也拒绝（整段判据） --- */
    CK_I(arkrtos_paging_user_copy(rom, 64u, ram, 64u, 0u, 0u, kern, user + 60, 8u, 0u), -3);

    /* --- 写：数据窗口可以 --- */
    memset(guard, 0, sizeof(guard));
    kern[0] = 0xA0u;                     /* 上一步读用例把 kern 覆盖了 */
    kern[1] = 0xA1u;
    kern[2] = 0xA2u;
    kern[3] = 0xA3u;
    CK_I(arkrtos_paging_user_copy(rom, 64u, ram, 64u, 0u, 0u, kern, guard + 16, 4u, 1u), 0);
    CK_U(guard[16], 0xA0u);
    CK_U(guard[19], 0xA3u);

    /* --- 写：只有代码窗口时拒绝，且一个字节都不动 --- */
    memset(user, 0, sizeof(user));
    CK_I(arkrtos_paging_user_copy(rom, 64u, 0ul, 0u, 0u, 0u, kern, user + 16, 4u, 1u), -3);
    CK_U(user[16], 0u);

    /* --- 窗口记录坏了：size 为 0 / base+size 回绕，都不放行 --- */
    CK_I(arkrtos_paging_user_copy(rom, 0u, ram, 0u, 0u, 0u, kern, user, 4u, 0u), -3);
    CK_I(arkrtos_paging_user_copy((unsigned long)0xFFFFFFF0ul, 0x40u, 0ul, 0u,
                                  0u, 0u, kern, user, 4u, 0u), -3);

    /* --- ram 窗口在、rom 窗口不在：读要落进 ram --- */
    guard[0] = 0x50u;
    guard[1] = 0x51u;
    guard[2] = 0x52u;
    guard[3] = 0x53u;
    memset(kern, 0, sizeof(kern));
    CK_I(arkrtos_paging_user_copy(0ul, 0u, ram, 64u, 0u, 0u, kern, guard, 4u, 0u), 0);
    CK_U(kern[0], 0x50u);
    CK_U(kern[3], 0x53u);
}

/* ------------------------------------------------------------------ */
/* 10. 任务指针版本与当前任务版本                                       */
/* ------------------------------------------------------------------ */
static void test_task_helpers(void)
{
    static arkrtos_task_t kernel_task;
    static arkrtos_task_t app_task;
    uint8 kern[16];
    uint8 user[16];

    reset_port();
    arkrtos_mpu_module_init();

    /* 内核任务 / 空指针一律 -2 */
    kernel_task.is_priv = 1u;
    CK_I(arkrtos_copy_from_user_task(0, kern, user, 4u), -2);
    CK_I(arkrtos_copy_from_user_task(&kernel_task, kern, user, 4u), -2);
    CK_I(arkrtos_copy_to_user_task(0, user, kern, 4u), -2);
    CK_I(arkrtos_copy_to_user_task(&kernel_task, user, kern, 4u), -2);
    CK_U(g_current_calls, 0u);

    /* 空间没对上：-4 */
    app_task.is_priv   = 0u;
    app_task.rom_start = (unsigned long)user;
    app_task.rom_size  = 16u;
    app_task.ram_start = 0ul;
    app_task.ram_size  = 0u;
    g_cur_space = 1u;
    CK_I(arkrtos_copy_from_user_task(&app_task, kern, user, 4u), -4);

    /* 对上之后：读方向走 rom 窗口 */
    g_cur_space = 0u;
    user[0] = 0x42u;
    CK_I(arkrtos_copy_from_user_task(&app_task, kern, user, 4u), 0);
    CK_U(kern[0], 0x42u);

    /* 写方向要 ram 窗口；rom 窗口不给写 */
    CK_I(arkrtos_copy_to_user_task(&app_task, user, kern, 4u), -3);
    app_task.ram_start = (unsigned long)user;
    app_task.ram_size  = 16u;
    CK_I(arkrtos_copy_to_user_task(&app_task, user + 4, kern, 4u), 0);
    CK_U(user[4], 0x42u);

    /* 当前任务版本：转给 arkrtos_task_get_current 的结果 */
    g_current = &kernel_task;
    CK_I(arkrtos_copy_from_user(kern, user, 4u), -2);
    CK_U(g_current_calls, 1u);

    g_current = 0;
    CK_I(arkrtos_copy_to_user(user, kern, 4u), -2);
    CK_U(g_current_calls, 2u);

    g_current = &app_task;
    CK_I(arkrtos_copy_to_user(user + 8, kern, 4u), 0);
    CK_U(g_current_calls, 3u);
}

/* ------------------------------------------------------------------ */
int main(void)
{
    reset_all();
    test_module_init();

    reset_all();
    test_build_task();

    reset_all();
    test_set_app();

    reset_all();
    test_space_forwards();

    reset_all();
    test_bind_and_of();

    reset_all();
    test_release_and_pool();

    reset_all();
    test_map_span();

    reset_all();
    test_unmap_span();

    reset_all();
    test_user_copy();

    reset_all();
    test_task_helpers();

    printf("PAGING-HOST OK  %d checks\n", g_checks);
    if(g_fails != 0)
    {
        printf("PAGING-HOST FAILED  %d of %d\n", g_fails, g_checks);
        return 1;
    }
    return 0;
}
