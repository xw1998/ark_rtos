/**
* @file test_audit_host.c
* @brief 内核自检（arkrtos_audit.c）的宿主用例
* @details 被测对象回答的问题只有一个：**这些结构里的字段，还满足内核其它部分
*          假定它们满足的那些不变式吗？** 它是只读的，只报「哪一组、第一个越界
*          的下标」，不修、不猜原因。所以用例的做法是：先把表建成干净的，确认
*          一次全绿；再**一次只掰一处**字段，确认恰好那一组亮灯、下标对、其它
*          组不受牵连；最后把灯掰回去，确认它又绿了。
*
*          宿主上跑它必须先解决两件事：
*            1. 整个模块按**绝对地址**读共享内存（SHARE_RAM_BASE / IMAGE_POOL_BASE
*               来自 config/arkrtos_partition.h），所以 main() 开头用 VirtualAlloc
*               / mmap(MAP_FIXED) 真把这两段占住，失败即 FAIL，绝不跳过；
*            2. 它引用任务表与四个外部函数（调度自检、故障记录、crash 区几何、
*               日志），这些在宿主上由本文件如实提供——桩只回答读数，不代替
*               被审计的结构。
*
*          钉住的语义里有两条容易读错：
*            - 同一组只在**第一次**违规时记下标（后面同类的违规只置位），所以
*              「两处都坏」时下标必须是最先走到的那一处；
*            - ARKRTOS_AUDIT_PARTITION 的下标恒为 INDEX_NONE：分区表是一整块，
*              没有「第几条」可指，编造一个下标比不报更坏。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_audit.h"
#include "arkrtos_task.h"
#include "arkrtos_ptable.h"
#include "arkrtos_share.h"
#include "arkrtos_partition.h"
#include "arkrtos_layout_def.h"
#include "arkrtos_app_image.h"
#include "arkrtos_fault.h"
#include "arkrtos_crash.h"
#include "arkrtos_log.h"
#include "arkrtos_features.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if (ARKRTOS_USE_AUDIT == 1)

/* 与布局无关的任意入口地址：从分区常量派生，不在源码里写死地址字面量 */
#define FAKE_ENTRY (CHIP_FLASH_BASE + 0x1001u)

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %lu want %lu (line %d)\n", what, got, want, __LINE__);
    }
}

/* ---------------- 绝对地址映射 ---------------- */

static int map_at(unsigned long base, unsigned long size)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)base, (SIZE_T)size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    return (p == (void *)(uintptr_t)base) ? 0 : -1;
#else
    void *p = mmap((void *)(uintptr_t)base, (size_t)size,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    return (p == MAP_FAILED) ? -1 : 0;
#endif
}

/* ---------------- 端口桩（ptable 需要） ---------------- */

static uint32 g_crit_depth;

uint32 arkrtos_port_enter_critical(void)
{
    return ++g_crit_depth;
}

void arkrtos_port_exit_critical(uint32 state)
{
    (void)state;
    g_crit_depth--;
}

uint32 arkrtos_port_atomic_cas(volatile uint32 *p_addr, uint32 expect, uint32 new_value)
{
    if(*p_addr == expect)
    {
        *p_addr = new_value;
        return 1u;
    }

    return 0u;
}

uint32 arkrtos_port_cpu_id(void)
{
    return 0u;
}

void arkrtos_port_dmb(void)
{
}

void arkrtos_port_spin_hint(void)
{
}

/* ---------------- 被审计结构的另一半 ---------------- */

/* 任务表：真实内核里由 arkrtos_cfg.c 定义，宿主上由本文件提供。
 * 用例要审计的就是「这张表里的字段」，所以它必须是真数组而不是指针。 */
arkrtos_task_t arkrtos_task_table[ARKRTOS_TASK_MAX_NUM];
int32          arkrtos_task_count;

/* 调度自检：真实实现在 task.c，宿主不链接它。返回值可编程，
 * 因为 audit 只问「它答 0 还是不 0」。 */
static int32 g_sched_check_ret;

int32 arkrtos_sched_check(void)
{
    return g_sched_check_ret;
}

/* 故障记录：audit_boot 失败时必须记一条 AUDITFAIL。 */
static unsigned long g_fault_calls;
static uint32        g_fault_type;
static int32         g_fault_task;

void arkrtos_fault_record(uint32 type, int32 task_id)
{
    g_fault_calls++;
    g_fault_type = type;
    g_fault_task = task_id;
}

/* crash 区的运行期几何：默认与分区宏一致，可编程掰开（只掰它，不掰宏）。 */
static uint32 g_crash_base;
static uint32 g_crash_size;

uint32 arkrtos_crash_area_base(void)
{
    return g_crash_base;
}

uint32 arkrtos_crash_area_size(void)
{
    return g_crash_size;
}

/* 日志：只记「有没有发、哪一级、正文里有没有 mask」。 */
static const char *g_log_level_str = "?";
static char        g_log_msg[160];

uint32 arkrtos_log_get_level(void)
{
    return (uint32)ARKRTOS_LOG_LEVEL;
}

void arkrtos_log_emit(uint32 level, const char *tag, uint32 line, const char *msg)
{
    (void)tag;
    (void)line;
    g_log_level_str = (level == ARKRTOS_LOG_ERROR) ? "error" : "other";
    snprintf(g_log_msg, sizeof(g_log_msg), "%s", (msg != 0) ? msg : "");
}

/* ---------------- 表工具 ---------------- */

static arkrtos_partition_table_t g_saved;

static arkrtos_partition_table_t *T(void)
{
    return arkrtos_ptable_get();
}

static void restore_all(void)
{
    g_sched_check_ret = 0;
    g_crash_base      = (uint32)CRASH_LOG_BASE;
    g_crash_size      = (uint32)CRASH_LOG_SIZE;
    g_fault_calls     = 0ul;
    g_fault_type      = 0u;
    g_fault_task      = 0;
    g_log_level_str   = "?";
    g_log_msg[0]      = 0;

    memcpy(T(), &g_saved, sizeof(g_saved));

    arkrtos_task_count = 0;
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
}

static arkrtos_audit_result_t run_audit(void)
{
    arkrtos_audit_result_t r;

    memset(&r, 0xAA, sizeof(r));
    arkrtos_audit_run(&r);

    return r;
}

/* 只关心 mask 时用这个：一次只掰一处，断言亮了哪一组。 */
static uint32 mask_of(void)
{
    return run_audit().mask;
}

/* ---------------- 干净表 ---------------- */

static void test_clean(void)
{
    arkrtos_audit_result_t r;

    arkrtos_ptable_init();
    memcpy(&g_saved, T(), sizeof(g_saved));

    /* crash 区几何的桩默认值与分区宏一致：它们是对账对象，不是常量抄写 */
    g_crash_base = (uint32)CRASH_LOG_BASE;
    g_crash_size = (uint32)CRASH_LOG_SIZE;

    r = run_audit();
    ck_u(r.mask, 0ul, "a freshly initialised table audits clean");
    ck_u(r.index, (unsigned long)ARKRTOS_AUDIT_INDEX_NONE, "and names no offender");
    ck(r.checks > 0u, "and does perform checks (a zero count would mean nothing was looked at)");
    ck_u(arkrtos_audit_last_mask(), 0ul, "last_mask mirrors the last run");
}

/* ---------------- 分区表：身份与 Flash 几何 ---------------- */

static void test_partition_flash(void)
{
    arkrtos_partition_table_t *t;

    restore_all();
    t = T();
    t->version += 1u;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a different layout version is reported");

    restore_all();
    t = T();
    t->hw_compat_id = 0u;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a zero hardware id is reported");

    restore_all();
    t = T();
    t->kernel_size = 0u;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a zero kernel size is reported");

    restore_all();
    t = T();
    t->kernel_base = (uint32)CHIP_FLASH_BASE - 4u;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a kernel below the chip is reported");

    restore_all();
    t = T();
    t->kernel_size = (uint32)CHIP_FLASH_SIZE;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a kernel past the chip end is reported");

    restore_all();
    t = T();
    t->pool_size = 0u;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a zero pool size is reported");

    restore_all();
    t = T();
    t->pool_base = t->kernel_base;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_PARTITION, "a pool sitting on the kernel is reported");

    /* 同一组的违规只在第一次记下标，而且分区表这一组根本没有下标可指 */
    ck_u(run_audit().index, (unsigned long)ARKRTOS_AUDIT_INDEX_NONE,
         "the partition group never invents an offender index");
}

/* ---------------- 分区表：池几何 / 配置区 / RAM / crash / 镜像池 ---------------- */

static void test_partition_geometry(void)
{
    arkrtos_partition_table_t *t;
    uint32 bit = (uint32)ARKRTOS_AUDIT_PARTITION;

    restore_all();
    t = T();
    t->pool_sector = 0u;
    ck_u(mask_of(), (unsigned long)bit, "a zero pool sector is reported");

    restore_all();
    t = T();
    t->pool_base += 1u;
    ck_u(mask_of(), (unsigned long)bit, "an unaligned pool base is reported");

    restore_all();
    t = T();
    t->pool_size += 1u;
    ck_u(mask_of(), (unsigned long)bit, "an unaligned pool size is reported");

    restore_all();
    t = T();
    t->pool_units += 1u;
    ck_u(mask_of(), (unsigned long)bit, "a unit count that does not match the size is reported");

    restore_all();
    t = T();
    t->pool_reserve = t->pool_units + 1u;
    ck_u(mask_of(), (unsigned long)bit, "a reserve larger than the pool is reported");

    restore_all();
    t = T();
    t->pool_usable_size += 1u;
    ck_u(mask_of(), (unsigned long)bit, "a usable size that does not match the reserve is reported");

    restore_all();
    t = T();
    t->pool_alloc_unit = 3u;
    ck_u(mask_of(), (unsigned long)bit, "an allocation unit that is not a power of two is reported");

    restore_all();
    t = T();
    t->pool_alloc_unit = 2u;
    ck_u(mask_of(), (unsigned long)bit, "an allocation unit below four bytes is reported");

    restore_all();
    t = T();
    t->pool_alloc_unit = t->pool_sector * 2u;
    ck_u(mask_of(), (unsigned long)bit, "an allocation unit larger than a sector is reported");

    /* 配置区：缺席是合法的（两个字段都为 0），半个配置区不是 */
    restore_all();
    t = T();
    t->config_base = 0u;
    t->config_size = 1u;
    ck_u(mask_of(), (unsigned long)bit, "a size without a base is reported");

    restore_all();
    t = T();
    t->config_base = (uint32)CHIP_FLASH_BASE;
    t->config_size = 0u;
    ck_u(mask_of(), (unsigned long)bit, "a base without a size is reported");

    restore_all();
    t = T();
    t->config_base = (uint32)(CHIP_FLASH_BASE + CHIP_FLASH_SIZE) - 4u;
    t->config_size = 8u;
    ck_u(mask_of(), (unsigned long)bit, "a config region past the chip end is reported");

    restore_all();
    t = T();
    t->config_base = t->kernel_base;
    ck_u(mask_of(), (unsigned long)bit, "a config region on top of the kernel is reported");

    /* RAM：三段不许互压，且都要在片内 */
    restore_all();
    t = T();
    t->share_ram_size = 0u;
    ck_u(mask_of(), (unsigned long)bit, "a zero shared-RAM size is reported");

    restore_all();
    t = T();
    t->share_ram_base = (uint32)CHIP_RAM_BASE - 4u;
    ck_u(mask_of(), (unsigned long)bit, "a shared-RAM base below the chip is reported");

    restore_all();
    t = T();
    t->kernel_ram_size = 0u;
    ck_u(mask_of(), (unsigned long)bit, "a zero kernel-RAM size is reported");

    restore_all();
    t = T();
    t->image_ram_total = 0u;
    ck_u(mask_of(), (unsigned long)bit, "a zero image-RAM total is reported");

    restore_all();
    t = T();
    t->kernel_ram_base = t->share_ram_base;
    ck_u(mask_of(), (unsigned long)bit, "kernel RAM overlapping shared RAM is reported");

    restore_all();
    t = T();
    t->image_ram_base = t->share_ram_base;
    ck_u(mask_of(), (unsigned long)bit, "image RAM overlapping shared RAM is reported");

    /* crash 区：宏说它在共享 RAM 尾部，运行期说它在别处 —— 两边不一致必须报 */
    restore_all();
    g_crash_base += 4u;
    ck_u(mask_of(), (unsigned long)bit, "a crash area that disagrees with the macros is reported");

    restore_all();
    g_crash_size -= 4u;
    ck_u(mask_of(), (unsigned long)bit, "a crash size that disagrees with the macros is reported");

    /* 镜像 RAM 池：2 的幂 + 能把池切完 */
    restore_all();
    t = T();
    t->image_ram_min_block = 3u;
    ck_u(mask_of(), (unsigned long)bit, "a min block that is not a power of two is reported");

    restore_all();
    t = T();
    t->image_ram_max_block = 3u;
    ck_u(mask_of(), (unsigned long)bit, "a max block that is not a power of two is reported");

    restore_all();
    t = T();
    t->image_ram_min_block = t->image_ram_max_block * 2u;
    ck_u(mask_of(), (unsigned long)bit, "min above max is reported");

    restore_all();
    t = T();
    t->image_ram_total += 1u;
    ck_u(mask_of(), (unsigned long)bit, "a total that the min block cannot tile is reported");

    restore_all();
    t = T();
    t->image_ram_base += 1u;
    ck_u(mask_of(), (unsigned long)bit, "a base the min block cannot align is reported");
}

/* ---------------- 布局旋钮 ---------------- */

static void test_layout(void)
{
    arkrtos_partition_table_t *t;
    uint32 bit = (uint32)ARKRTOS_AUDIT_LAYOUT;

    restore_all();
    t = T();
    t->layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED + 1u;
    ck_u(mask_of(), (unsigned long)bit, "an unknown layout mode is reported");

    restore_all();
    t = T();
    t->layout_source = (uint32)ARKRTOS_LAYOUT_SOURCE_CONFIG + 1u;
    ck_u(mask_of(), (unsigned long)bit, "an unknown layout source is reported");

    restore_all();
    t = T();
    t->reclaim_mode = (uint32)ARKRTOS_CFG_RECLAIM_NONE + 1u;
    ck_u(mask_of(), (unsigned long)bit, "an unknown reclaim mode is reported");

    restore_all();
    t = T();
    t->slot_max = 0u;
    ck_u(mask_of(), (unsigned long)bit, "zero slot records is reported");

    restore_all();
    t = T();
    t->slot_max = (uint32)ARKRTOS_SLOT_ARRAY_MAX + 1u;
    ck_u(mask_of(), (unsigned long)bit, "more slot records than the array holds is reported");

    restore_all();
    t = T();
    t->cfg_slot_count = t->slot_max + 1u;
    ck_u(mask_of(), (unsigned long)bit, "a fixed-slot count above slot_max is reported");
}

/* ---------------- 槽位记录 ---------------- */

static void put_slot(arkrtos_partition_table_t *t, uint32 i, uint32 type, uint32 state,
                     uint32 base, uint32 size)
{
    t->slot_type[i]   = type;
    t->slot_state[i]  = state;
    t->slot_base[i]   = base;
    t->slot_size[i]   = size;
    t->slot_entry[i]  = 0u;
    t->slot_task_id[i] = 0u;
    t->slot_autostart[i] = 0u;
    t->slot_ram_base[i]  = 0u;
    t->slot_ram_size[i]  = 0u;
}

static void test_slots(void)
{
    arkrtos_partition_table_t *t;

    /* 一条健康记录：不亮任何灯。先证明「好」的路径是好的，再掰。 */
    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_entry[0] = (uint32)FAKE_ENTRY;
    ck_u(mask_of(), 0ul, "one healthy app record audits clean");

    restore_all();
    t = T();
    put_slot(t, 0u, 99u, (uint32)ARKRTOS_APP_SLOT_LOADED, t->pool_base, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS, "an unknown slot type is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, 99u, t->pool_base, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS, "an unknown slot state is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_FREE, (uint32)ARKRTOS_APP_SLOT_EMPTY, 0u, 0u);
    t->slot_task_id[0] = 1u;
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS,
         "leftovers in a free record are reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, (uint32)ARKRTOS_APP_HEADER_SIZE - 1u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS,
         "an image too small to hold a header is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base - t->pool_alloc_unit, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS, "an image before the pool is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base + 1u, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS,
         "an image off the allocation boundary is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base + t->pool_usable_size - 16u, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS,
         "an image past the usable pool is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_RUNNING,
             t->pool_base, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS,
         "a running image with no entry point is reported");

    restore_all();
    arkrtos_task_count = 1;
    arkrtos_task_table[0].status       = ARKRTOS_TASK_READY;
    arkrtos_task_table[0].priority     = 5u;
    arkrtos_task_table[0].stack_bottom = (uint32 *)(uintptr_t)0x1000;
    arkrtos_task_table[0].stack_size   = 1024u;
    arkrtos_task_table[0].ready_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].ready_prev   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_prev   = ARKRTOS_TASK_NIL;
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_task_id[0] = 2u;
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOTS,
         "a slot naming a task the table does not have is reported");

    /* RAM 块：半个块是撕裂的更新，不是分配器能产生的状态 */
    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_ram_size[0] = t->image_ram_min_block;
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOT_RAM,
         "a RAM size without a base is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_ram_base[0] = t->image_ram_base;
    t->slot_ram_size[0] = 3000u;
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOT_RAM,
         "a RAM block that is not a power of two is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_ram_size[0] = t->image_ram_min_block;
    t->slot_ram_base[0] = t->image_ram_base + 1u;
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOT_RAM,
         "a RAM block off its own alignment is reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    put_slot(t, 1u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOT_FLASH,
         "two images in one Flash range are reported");

    restore_all();
    t = T();
    put_slot(t, 0u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_ram_base[0] = t->image_ram_base;
    t->slot_ram_size[0] = t->image_ram_min_block;
    put_slot(t, 1u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base + 4096u, 4096u);
    t->slot_ram_base[1] = t->image_ram_base;
    t->slot_ram_size[1] = t->image_ram_min_block;
    ck_u(run_audit().mask, (unsigned long)ARKRTOS_AUDIT_SLOT_RAM,
         "two images sharing one RAM block are reported");

    /* 两处都坏时下标必须是先走到的那一处（第 1 条），不是第 2 条 */
    restore_all();
    t = T();
    put_slot(t, 0u, 99u, (uint32)ARKRTOS_APP_SLOT_LOADED, t->pool_base, 4096u);
    put_slot(t, 1u, (uint32)ARKRTOS_SLOT_APP, (uint32)ARKRTOS_APP_SLOT_LOADED,
             t->pool_base, 4096u);
    t->slot_size[1] = 1u;
    ck_u(run_audit().index, 0ul, "the offender index is the first one seen, not the worst");
}

/* ---------------- 任务表 ---------------- */

static void test_tasks(void)
{
    arkrtos_task_t *q;
    uint32 bit = (uint32)ARKRTOS_AUDIT_TASKS;

    restore_all();
    arkrtos_task_count = 2;
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_table[0].status      = ARKRTOS_TASK_READY;
    arkrtos_task_table[0].priority    = 5u;
    arkrtos_task_table[0].stack_bottom = (uint32 *)(uintptr_t)0x1000;
    arkrtos_task_table[0].stack_size   = 1024u;
    arkrtos_task_table[0].ready_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].ready_prev   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_prev   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[1] = arkrtos_task_table[0];
    ck_u(mask_of(), 0ul, "two healthy tasks audit clean");

    restore_all();
    arkrtos_task_count = -1;
    ck_u(run_audit().mask, (unsigned long)bit, "a negative task count is reported");

    restore_all();
    arkrtos_task_count = (int32)ARKRTOS_TASK_MAX_NUM + 1;
    ck_u(run_audit().mask, (unsigned long)bit, "a task count past the table is reported");

    restore_all();
    arkrtos_task_count = 1;
    q = &arkrtos_task_table[0];
    q->status = (arkrtos_task_status_t)99;
    ck_u(run_audit().mask, (unsigned long)bit, "an unknown task status is reported");

    restore_all();
    arkrtos_task_count = 1;
    q = &arkrtos_task_table[0];
    q->status       = ARKRTOS_TASK_READY;
    q->priority     = 255u;
    q->stack_bottom = (uint32 *)(uintptr_t)0x1000;
    q->stack_size   = 1024u;
    ck_u(run_audit().mask, (unsigned long)bit, "priority 255 (the two-level bitmap has 256) is reported");

    restore_all();
    arkrtos_task_count = 1;
    q = &arkrtos_task_table[0];
    q->status     = ARKRTOS_TASK_READY;
    q->stack_size = 1024u;
    ck_u(run_audit().mask, (unsigned long)bit, "a task with no stack bottom is reported");

    restore_all();
    arkrtos_task_count = 1;
    q = &arkrtos_task_table[0];
    q->status       = ARKRTOS_TASK_READY;
    q->stack_bottom = (uint32 *)(uintptr_t)0x1000;
    ck_u(run_audit().mask, (unsigned long)bit, "a task with a zero stack size is reported");

    restore_all();
    arkrtos_task_count = 2;
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_table[0].status       = ARKRTOS_TASK_READY;
    arkrtos_task_table[0].stack_bottom = (uint32 *)(uintptr_t)0x1000;
    arkrtos_task_table[0].stack_size   = 1024u;
    arkrtos_task_table[0].ready_next   = 2u;
    arkrtos_task_table[0].ready_prev   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_prev   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[1] = arkrtos_task_table[0];
    arkrtos_task_table[1].ready_next = ARKRTOS_TASK_NIL;
    ck_u(run_audit().mask, (unsigned long)bit, "a ready-next past the table is reported");

    restore_all();
    arkrtos_task_count = 2;
    memset(arkrtos_task_table, 0, sizeof(arkrtos_task_table));
    arkrtos_task_table[0].status       = ARKRTOS_TASK_READY;
    arkrtos_task_table[0].stack_bottom = (uint32 *)(uintptr_t)0x1000;
    arkrtos_task_table[0].stack_size   = 1024u;
    arkrtos_task_table[0].ready_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].ready_prev   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_next   = ARKRTOS_TASK_NIL;
    arkrtos_task_table[0].delay_prev   = 2u;
    arkrtos_task_table[1] = arkrtos_task_table[0];
    arkrtos_task_table[1].delay_prev = ARKRTOS_TASK_NIL;
    ck_u(run_audit().mask, (unsigned long)bit, "a delay-prev past the table is reported");

    /* 调度器自检是第七组：表本身干净，但它说对不上 */
    restore_all();
    arkrtos_task_count = 0;
    g_sched_check_ret  = 1;
    ck_u(mask_of(), (unsigned long)ARKRTOS_AUDIT_SCHED,
         "a scheduler that reports an inconsistency lights the sched bit");

    /* 两处都坏时下标以任务表先看到的那一处为准 */
    restore_all();
    arkrtos_task_count = 2;
    arkrtos_task_table[0].status = (arkrtos_task_status_t)99;
    arkrtos_task_table[1].status = (arkrtos_task_status_t)99;
    ck_u(run_audit().index, 0ul, "and the task offender index is the first one seen");
}

/* ---------------- 入口契约 ---------------- */

static void test_entry_points(void)
{
    arkrtos_audit_result_t r;
    uint32 bits[7];
    uint32 i;

    bits[0] = (uint32)ARKRTOS_AUDIT_PARTITION;
    bits[1] = (uint32)ARKRTOS_AUDIT_LAYOUT;
    bits[2] = (uint32)ARKRTOS_AUDIT_SLOTS;
    bits[3] = (uint32)ARKRTOS_AUDIT_SLOT_RAM;
    bits[4] = (uint32)ARKRTOS_AUDIT_SLOT_FLASH;
    bits[5] = (uint32)ARKRTOS_AUDIT_TASKS;
    bits[6] = (uint32)ARKRTOS_AUDIT_SCHED;

    ck(ARKRTOS_AUDIT_BIT_MAX == ARKRTOS_AUDIT_SCHED,
       "the highest bit is the one the shell loops to");

    for(i = 0u; i < 7u; i++)
    {
        const char *nm = arkrtos_audit_name(bits[i]);

        ck(nm != 0, "every bit this module can set has a printable name");
        ck_u((unsigned long)((nm != 0) ? strlen(nm) : 0ul) > 0ul, 1ul,
             "and the name is not empty");
    }
    ck(arkrtos_audit_name(0x80u) == 0, "a bit this module never sets has no name");

    /* run(NULL) 是契约内的输入：什么都不做，不许崩 */
    arkrtos_audit_run(0);
    ck(1, "run(NULL) is a no-op instead of a crash");

    /* boot：干净时不记故障、发一条 info */
    restore_all();
    ck_u(arkrtos_audit_boot(), 0ul, "a clean boot returns a zero mask");
    ck_u(g_fault_calls, 0ul, "and records no fault");
    ck(strcmp(g_log_level_str, "other") == 0, "and logs at info, not error");

    /* boot：脏时必须记 AUDITFAIL，且返回同一个 mask */
    restore_all();
    T()->version += 1u;
    r.mask = arkrtos_audit_boot();
    ck_u(r.mask, (unsigned long)ARKRTOS_AUDIT_PARTITION, "a dirty boot returns the failing mask");
    ck_u(g_fault_calls, 1ul, "and records exactly one fault");
    ck_u(g_fault_type, (unsigned long)ARKRTOS_FAULT_AUDITFAIL, "and the fault type is AUDITFAIL");
    ck_u((unsigned long)g_fault_task, (unsigned long)0u, "with no task id");
    ck(strcmp(g_log_level_str, "error") == 0, "and logs at error");
    ck(strstr(g_log_msg, "mask 0x") != 0, "and the message carries the mask");
}

/* ---------------- 主 ---------------- */

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);

    if(map_at((unsigned long)CHIP_RAM_BASE, (unsigned long)SHARE_RAM_SIZE) != 0)
    {
        printf("AUDIT-HOST FAIL: cannot map share RAM at 0x%08lX\n",
               (unsigned long)CHIP_RAM_BASE);
        return 1;
    }

    if(map_at((unsigned long)CHIP_FLASH_BASE, (unsigned long)CHIP_FLASH_SIZE) != 0)
    {
        printf("AUDIT-HOST FAIL: cannot map flash at 0x%08lX\n",
               (unsigned long)CHIP_FLASH_BASE);
        return 1;
    }

    test_clean();
    test_partition_flash();
    test_partition_geometry();
    test_layout();
    test_slots();
    test_tasks();
    test_entry_points();

    if(g_fails != 0)
    {
        printf("AUDIT-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("AUDIT-HOST OK  %d checks\n", g_checks);
    return 0;
}

#else  /* ARKRTOS_USE_AUDIT == 0 */

/* 自检被编译掉时，模块对外的四个入口必须如实回答「没有做检查」：mask 恒 0、
 * last_mask 恒 0、boot 恒 0、名字是 "audit disabled"。这一档不去映射共享内存，
 * 也不需要分区表——它证明的是「关掉之后不假装检查过」，不是任何结构是对的。 */
int main(void)
{
    int checks = 0;
    int fails = 0;
    arkrtos_audit_result_t r;

    setvbuf(stdout, 0, _IONBF, 0);

    memset(&r, 0xAA, sizeof(r));
    arkrtos_audit_run(&r);
    checks++;
    if(r.mask != 0u || r.index != 0u || r.checks != 0u)
    {
        fails++;
        printf("AUDIT-HOST OFF FAIL: run did not zero the result (mask=0x%08lX index=0x%08lX checks=%lu)\n",
               (unsigned long)r.mask, (unsigned long)r.index, (unsigned long)r.checks);
    }

    checks++;
    if(arkrtos_audit_last_mask() != 0u)
    {
        fails++;
        printf("AUDIT-HOST OFF FAIL: last_mask is non-zero after run\n");
    }

    /* boot 必须什么都不做：不记录故障、不返回非零。桩没提供故障接口，编译期就能
     * 看出它没有调用任何外部函数。 */
    checks++;
    if(arkrtos_audit_boot() != 0u)
    {
        fails++;
        printf("AUDIT-HOST OFF FAIL: boot returned non-zero\n");
    }

    /* run(NULL) 在 OFF 档同样被允许（memset 前判空）。 */
    arkrtos_audit_run(0);
    checks++;

    checks++;
    {
        const char *nm = arkrtos_audit_name(ARKRTOS_AUDIT_PARTITION);
        if(nm == 0 || strcmp(nm, "audit disabled") != 0)
        {
            fails++;
            printf("AUDIT-HOST OFF FAIL: name(PARTITION) is not \"audit disabled\"\n");
        }
    }

    /* 每个位名都应是同一个占位串，而不是把某个位置成 NULL 或空串。 */
    checks++;
    {
        uint32 bit;
        int ok = 1;
        for(bit = 1u; bit <= ARKRTOS_AUDIT_BIT_MAX; bit <<= 1)
        {
            const char *nm = arkrtos_audit_name(bit);
            if(nm == 0 || strcmp(nm, "audit disabled") != 0)
            {
                ok = 0;
                break;
            }
        }
        if(ok == 0)
        {
            fails++;
            printf("AUDIT-HOST OFF FAIL: some audit bit name is not the disabled placeholder\n");
        }
    }

    if(fails != 0)
    {
        printf("AUDIT-HOST OFF FAILED %d/%d\n", fails, checks);
        return 1;
    }

    printf("AUDIT-HOST OFF OK  %d checks\n", checks);
    return 0;
}
#endif /* ARKRTOS_USE_AUDIT */
