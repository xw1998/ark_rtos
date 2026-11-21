/**
* @file test_ptable_host.c
* @brief 分区表运行期实现（arkrtos_ptable.c）的宿主用例
* @details 被测对象是「把编译期布局翻译成共享内存里的表」以及两个分配器：
*          统一镜像池的字节粒度首适配、镜像 RAM 池的 2 的幂分配（外加不挂
*          槽位的小程序两块）。
*
*          宿主上跑它必须先解决一件事：整个模块按**绝对地址**访问共享内存
*          （SHARE_RAM_BASE / IMAGE_POOL_BASE 来自 config/arkrtos_partition.h），
*          而宿主进程的地址空间里那两个地址原本是空的。所以这里在 main() 开头
*          把两段区间真正映射出来（Windows VirtualAlloc / POSIX mmap MAP_FIXED）：
*            - [CHIP_RAM_BASE, +SHARE_RAM_SIZE)   分区表本体
*            - [CHIP_FLASH_BASE, +CHIP_FLASH_SIZE) 镜像池（只有 read_header 会读内容）
*          映射失败直接报错退出，绝不「跳过」——跳过会让整套断言在空指针上跑，
*          读起来像通过。
*
*          钉住的是这些容易只在现场暴露的行为：
*            - 初始化必须显式清掉小程序的两块（共享 RAM 活过复位，残留值会让
*              首次分配直接报「已有小程序在跑」）；
*            - clear_slots 只清槽位记录，不动 layout_mode（那是配置区解析出来的）；
*            - 分配器的落点策略：优先 2 的幂对齐落点，实在放不下才退化为缺口起点
*              并把 p_aligned 置 0；
*            - 重叠、越界、未对齐、记录用尽、池内无空间各自的返回值；
*            - RAM 块必须按块大小对齐（MPU 单区域覆盖），reserve 只校验不记账；
*            - 小程序两块同生同死，归还空项如实报错；
*            - 统计里的碎片数（>1 表示池内有空洞）。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_partition.h"
#include "arkrtos_ptable.h"
#include "arkrtos_share.h"
#include "arkrtos_app_image.h"
#include "arkrtos_hal.h"            /* port 函数原型（下面要提供它们的桩） */
#include "arkrtos_layout_def.h"    /* 策略模式 / 来源常量 */
#include "arkrtos_log_defs.h"      /* 默认日志级别 */

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* 用例里只需要一个「与布局无关的任意入口地址」：从分区常量派生，
 * 不在镜像代码里写死地址字面量（ci_gate 的 single-address-source 项
 * 要求布局地址只在 config/ 里定义一次）。 */
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

/* ---------------- 端口桩 ---------------- */

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

/* ---------------- 小工具 ---------------- */

static arkrtos_partition_table_t *tab(void)
{
    return arkrtos_ptable_get();
}

static uint32 pool_units(uint32 n)
{
    return IMAGE_POOL_BASE + (n * ARKRTOS_POOL_ALLOC_UNIT);
}

/* ---------------- 初始化 ---------------- */

static void test_init_layout(void)
{
    arkrtos_partition_table_t *pt;
    uint32 i;

    arkrtos_ptable_init();
    pt = tab();

    ck_u(pt->magic, (unsigned long)ARKRTOS_PARTITION_MAGIC, "init writes the table magic");
    ck_u(pt->version, (unsigned long)ARKRTOS_PARTITION_VERSION, "and the layout version");
    ck_u(pt->hw_compat_id, (unsigned long)ARKRTOS_HW_COMPAT_ID, "and the hardware compat id");

    ck_u(pt->kernel_base, (unsigned long)KERNEL_BASE, "kernel_base comes from the layout header");
    ck_u(pt->kernel_size, (unsigned long)KERNEL_SIZE, "kernel_size too");
    ck_u(pt->pool_base, (unsigned long)IMAGE_POOL_BASE, "pool_base too");
    ck_u(pt->pool_size, (unsigned long)IMAGE_POOL_SIZE, "pool_size too");
    ck_u(pt->pool_usable_size, (unsigned long)IMAGE_POOL_USABLE_SIZE,
         "and the usable size excludes the compaction reserve");
    ck_u(pt->pool_alloc_unit, (unsigned long)ARKRTOS_POOL_ALLOC_UNIT, "alloc unit");
    ck_u(pt->pool_sector, (unsigned long)IMAGE_POOL_SECTOR, "sector size");
    ck_u(pt->pool_units, (unsigned long)IMAGE_POOL_UNITS, "unit count");
    ck_u(pt->pool_reserve, (unsigned long)POOL_RESERVE_SECTORS, "reserve sectors");
    ck_u(pt->slot_max, (unsigned long)SLOT_MAX, "slot record limit");
    ck_u(pt->layout_mode, (unsigned long)ARKRTOS_LAYOUT_DEFAULT_MODE, "default layout mode");
    ck_u(pt->layout_source, (unsigned long)ARKRTOS_LAYOUT_SOURCE_DEFAULT, "default layout source");
    ck_u(pt->config_base, (unsigned long)CONFIG_BASE, "config region base");
    ck_u(pt->config_size, (unsigned long)CONFIG_SIZE, "config region size");
    ck_u(pt->cfg_slot_count, 0ul, "no configured slot table on a fresh init");
    ck_u(pt->reclaim_mode, (unsigned long)ARKRTOS_RECLAIM_MODE, "reclaim mode");
    ck_u(pt->cfg_log_level, (unsigned long)ARKRTOS_LOG_LEVEL, "default log level");
    ck_u(pt->cfg_restart_max, (unsigned long)APP_CRASH_RESTART_MAX, "restart budget");
    ck_u(pt->cfg_boot_delay_ms, 0ul, "no boot delay by default");
    ck_u(pt->cfg_raw_allow, (unsigned long)APP_ALLOW_RAW_IMAGE, "raw image policy");

    ck_u(pt->share_ram_base, (unsigned long)SHARE_RAM_BASE, "share RAM base");
    ck_u(pt->share_ram_size, (unsigned long)SHARE_RAM_SIZE, "share RAM size");
    ck_u(pt->kernel_ram_base, (unsigned long)KERNEL_RAM_BASE, "kernel RAM base");
    ck_u(pt->kernel_ram_size, (unsigned long)KERNEL_RAM_SIZE, "kernel RAM size");
    ck_u(pt->image_ram_base, (unsigned long)SLOT_RAM_BASE, "image RAM pool base");
    ck_u(pt->image_ram_total, (unsigned long)SLOT_RAM_TOTAL, "image RAM pool total");
    ck_u(pt->image_ram_min_block, (unsigned long)SLOT_RAM_MIN_BLOCK, "image RAM min block");
    ck_u(pt->image_ram_max_block, (unsigned long)SLOT_RAM_MAX_BLOCK, "image RAM max block");

    for(i = 0u; i < ARKRTOS_MINI_ARRAY_MAX; i++)
    {
        ck((pt->mini_code_base[i] == 0u) && (pt->mini_code_size[i] == 0u) &&
           (pt->mini_ram_base[i] == 0u) && (pt->mini_ram_size[i] == 0u),
           "init zeroes both mini blocks (shared RAM survives a reset)");
    }

    for(i = 0u; i < ARKRTOS_SLOT_ARRAY_MAX; i++)
    {
        ck(pt->slot_type[i] == ARKRTOS_SLOT_FREE, "init marks every slot record free");
        ck(pt->slot_state[i] == ARKRTOS_APP_SLOT_EMPTY, "and empty");
    }

    ck_u(pt->dev_slot_count, 0ul, "no dev slots on a fresh init");
    ck(tab() == pt, "get returns the same table every time");
}

/* 极早期被调用（magic 还没写）：get 必须就地补一次初始化。 */
static void test_get_autocreate(void)
{
    arkrtos_partition_table_t *pt = tab();

    (void)memset((void *)(uintptr_t)SHARE_RAM_BASE, 0, (size_t)SHARE_RAM_SIZE);

    ck_u(pt->magic, 0ul, "the wiped table really is wiped");
    ck_u(pt->pool_base, 0ul, "including its layout fields");

    pt = tab();
    ck_u(pt->magic, (unsigned long)ARKRTOS_PARTITION_MAGIC,
         "get() re-runs init when the magic is missing");
    ck_u(pt->pool_base, (unsigned long)IMAGE_POOL_BASE, "and the layout is valid again");
    ck_u(pt->slot_type[0], (unsigned long)ARKRTOS_SLOT_FREE, "with empty slot records");
}

/* clear_slots 只清记录：布局字段来自配置区，不能被当成种子数据抹回默认。 */
static void test_clear_slots_keeps_layout(void)
{
    arkrtos_partition_table_t *pt;

    arkrtos_ptable_init();
    pt = tab();

    pt->layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    pt->cfg_slot_count = 3u;
    pt->dev_slot_count = 2u;
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, IMAGE_POOL_BASE,
                            ARKRTOS_POOL_ALLOC_UNIT) >= 0, "a slot record is taken");

    arkrtos_ptable_clear_slots();

    ck_u(pt->layout_mode, (unsigned long)ARKRTOS_LAYOUT_MODE_FIXED,
         "clear_slots keeps the resolved layout mode");
    ck_u(pt->cfg_slot_count, 3ul, "and the configured slot count");
    ck_u(pt->slot_type[0], (unsigned long)ARKRTOS_SLOT_FREE, "but drops the slot records");
    ck_u(pt->slot_base[0], 0ul, "including their base");
    ck_u(pt->dev_slot_count, 0ul, "and the dev slot counter");
}

/* ---------------- 区间校验 ---------------- */

static void test_range_check(void)
{
    arkrtos_partition_table_t *pt;

    arkrtos_ptable_init();
    pt = tab();

    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE, 0u) < 0, "zero size is refused");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE - 1u, 4u) < 0, "a base below the pool is refused");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE + 1u, 4u) < 0,
       "a base that is not on the alloc unit is refused");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE, ARKRTOS_POOL_ALLOC_UNIT) == 0,
       "the first unit is legal");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE,
                                  ARKRTOS_POOL_ALLOC_UNIT) < 0,
       "the compaction reserve is not allocatable in auto mode");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE, IMAGE_POOL_USABLE_SIZE + 1u) < 0,
       "a size that spills past the usable end is refused");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE -
                                  ARKRTOS_POOL_ALLOC_UNIT, ARKRTOS_POOL_ALLOC_UNIT) == 0,
       "the last usable unit is legal");

    pt->layout_mode = (uint32)ARKRTOS_LAYOUT_MODE_FIXED;
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE,
                                  ARKRTOS_POOL_ALLOC_UNIT) == 0,
       "fixed-slot mode may use the whole pool (no compaction ever runs)");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE + IMAGE_POOL_SIZE,
                                  ARKRTOS_POOL_ALLOC_UNIT) < 0,
       "but not past the pool end");
    ck(arkrtos_ptable_range_check(IMAGE_POOL_BASE + IMAGE_POOL_SIZE - 4u, 8u) < 0,
       "and a size running off the end is refused");
}

/* ---------------- 槽位记录 ---------------- */

static void test_alloc_basic(void)
{
    arkrtos_partition_table_t *pt;
    int32 slot;
    int32 again;
    uint32 i;

    arkrtos_ptable_init();
    pt = tab();

    slot = arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT);
    ck_u((unsigned long)slot, 0ul, "the first record is slot 0");
    ck_u(pt->slot_type[0], (unsigned long)ARKRTOS_SLOT_APP, "type recorded");
    ck_u(pt->slot_base[0], (unsigned long)pool_units(0u), "base recorded");
    ck_u(pt->slot_size[0], (unsigned long)ARKRTOS_POOL_ALLOC_UNIT, "size recorded");
    ck_u(pt->slot_state[0], (unsigned long)ARKRTOS_APP_SLOT_INSTALLING,
         "a fresh record starts INSTALLING (not startable yet)");
    ck_u(pt->slot_ram_base[0], 0ul, "no RAM block bound yet");
    ck_u(pt->slot_entry[0], 0ul, "no entry yet");
    ck_u(pt->slot_task_id[0], 0ul, "no task yet");
    ck_u(pt->slot_crash_cnt[0], 0ul, "crash counter cleared");
    ck_u(pt->slot_autostart[0], 0ul, "autostart cleared");

    again = arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT);
    ck_u((unsigned long)again, 0ul, "the same range re-registers into the same record");

    slot = arkrtos_ptable_alloc(ARKRTOS_SLOT_DRIVER, pool_units(2u), ARKRTOS_POOL_ALLOC_UNIT);
    ck_u((unsigned long)slot, 1ul, "a different range takes the next record");

    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u),
                            ARKRTOS_POOL_ALLOC_UNIT * 2u) < 0,
       "a range that overlaps a record is refused (same base, longer size)");
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(1u), ARKRTOS_POOL_ALLOC_UNIT) >= 0,
       "an adjacent range is not an overlap");
    arkrtos_ptable_free(2u);
    ck(arkrtos_ptable_alloc(3u, pool_units(4u), ARKRTOS_POOL_ALLOC_UNIT) < 0,
       "only APP and DRIVER may be registered");
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(4u) + 1u,
                            ARKRTOS_POOL_ALLOC_UNIT) < 0,
       "an unaligned base is refused");

    /* 把剩余记录占满，再要一条必须报「没有记录」，而不是静默覆盖某一条 */
    for(i = 2u; i < SLOT_MAX; i++)
    {
        ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(i + 2u),
                                ARKRTOS_POOL_ALLOC_UNIT) >= 0,
           "records can be filled up to the limit");
    }

    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(SLOT_MAX + 2u),
                            ARKRTOS_POOL_ALLOC_UNIT) < 0,
       "one record past the limit is refused");
}

static void test_alloc_raw(void)
{
    arkrtos_partition_table_t *pt;
    int32 slot;
    uint32 i;

    arkrtos_ptable_init();
    pt = tab();

    slot = arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_DRIVER,
                                    IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE,
                                    ARKRTOS_POOL_ALLOC_UNIT, FAKE_ENTRY);
    ck_u((unsigned long)slot, 0ul, "a raw range in the compaction reserve is recorded");
    ck_u(pt->slot_state[0], (unsigned long)ARKRTOS_APP_SLOT_RAW, "with the RAW state");
    ck_u(pt->slot_entry[0], (unsigned long)FAKE_ENTRY, "and the given entry");

    ck_u((unsigned long)arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_DRIVER,
                                                 IMAGE_POOL_BASE + IMAGE_POOL_USABLE_SIZE,
                                                 ARKRTOS_POOL_ALLOC_UNIT, 0u),
         0ul, "the same raw range re-registers into the same record");

    ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, IMAGE_POOL_BASE, 0u, 0u) < 0,
       "zero size is refused");
    ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, IMAGE_POOL_BASE - 4u, 4u, 0u) < 0,
       "a base below the pool is refused");
    ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, IMAGE_POOL_BASE + IMAGE_POOL_SIZE,
                                4u, 0u) < 0, "a base at the pool end is refused");
    ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, IMAGE_POOL_BASE, IMAGE_POOL_SIZE + 4u, 0u) < 0,
       "a size that runs off the end is refused");
    ck(arkrtos_ptable_alloc_raw(9u, IMAGE_POOL_BASE, 4u, 0u) < 0, "only APP and DRIVER");

    for(i = 1u; i < SLOT_MAX; i++)
    {
        ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, pool_units(i), ARKRTOS_POOL_ALLOC_UNIT,
                                    0u) >= 0, "raw records fill up to the limit");
    }

    ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, pool_units(SLOT_MAX + 8u),
                                ARKRTOS_POOL_ALLOC_UNIT, 0u) < 0,
       "one raw record past the limit is refused");
}

static void test_free_and_move(void)
{
    arkrtos_partition_table_t *pt;
    int32 a;
    int32 b;

    arkrtos_ptable_init();
    pt = tab();

    a = arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT);
    b = arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(4u), ARKRTOS_POOL_ALLOC_UNIT);
    ck((a == 0) && (b == 1), "two records");

    (void)arkrtos_ptable_set_slot((uint32)a, (uint32)ARKRTOS_APP_SLOT_LOADED, FAKE_ENTRY, 7u);
    ck_u((unsigned long)arkrtos_ptable_find_task(7u), (unsigned long)a,
         "the task id is searchable");

    arkrtos_ptable_free((uint32)a);
    ck_u(pt->slot_type[(uint32)a], (unsigned long)ARKRTOS_SLOT_FREE, "free drops the type");
    ck_u(pt->slot_base[(uint32)a], 0ul, "and the base");
    ck_u(pt->slot_size[(uint32)a], 0ul, "and the size");
    ck_u(pt->slot_state[(uint32)a], (unsigned long)ARKRTOS_APP_SLOT_EMPTY, "and the state");
    ck_u(pt->slot_entry[(uint32)a], 0ul, "and the entry");
    ck_u(pt->slot_task_id[(uint32)a], 0ul, "and the task id");
    ck_u(pt->slot_autostart[(uint32)a], 0ul, "and autostart");
    ck_u((unsigned long)arkrtos_ptable_find_task(7u), (unsigned long)-1,
         "a freed record is no longer found by task id");

    a = arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT);
    ck_u((unsigned long)a, 0ul, "the freed record is reused first");

    ck(arkrtos_ptable_move((uint32)a, pool_units(8u)) == 0, "the image can be moved");
    ck_u(pt->slot_base[(uint32)a], (unsigned long)pool_units(8u), "and the base follows");
    ck(arkrtos_ptable_move((uint32)a, pool_units(4u)) < 0,
       "a move onto another record is refused");
    ck(arkrtos_ptable_move((uint32)a, pool_units(8u) + 1u) < 0, "an unaligned move is refused");
    ck(arkrtos_ptable_move((uint32)a, IMAGE_POOL_BASE + IMAGE_POOL_SIZE) < 0,
       "a move outside the pool is refused");
    ck(arkrtos_ptable_move(SLOT_MAX, pool_units(12u)) < 0, "an out-of-range record is refused");
    ck_u(pt->slot_base[(uint32)a], (unsigned long)pool_units(8u),
         "a refused move leaves the base alone");

    arkrtos_ptable_free((uint32)a);
    arkrtos_ptable_free(SLOT_MAX);
    arkrtos_ptable_free(ARKRTOS_SLOT_ARRAY_MAX + 1u);
    ck(1, "freeing an out-of-range record is a no-op");
}

static void test_find_addr(void)
{
    uint32 base = 0u;
    uint32 size = 0u;

    arkrtos_ptable_init();

    ck_u((unsigned long)arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u),
                                             ARKRTOS_POOL_ALLOC_UNIT * 2u), 0ul,
         "a two-unit record");

    ck_u((unsigned long)arkrtos_ptable_find_addr(pool_units(1u), &base, &size), 0ul,
         "an address inside the range finds the record");
    ck_u(base, (unsigned long)pool_units(0u), "and reports its base");
    ck_u(size, (unsigned long)(ARKRTOS_POOL_ALLOC_UNIT * 2u), "and its size");

    ck_u((unsigned long)arkrtos_ptable_find_addr(pool_units(0u), 0, 0), 0ul,
         "the output pointers are optional");

    ck_u((unsigned long)arkrtos_ptable_find_addr(pool_units(2u), 0, 0), (unsigned long)-1,
         "the first byte past the range is not a hit");
    ck_u((unsigned long)arkrtos_ptable_find_addr(IMAGE_POOL_BASE - 1u, 0, 0),
         (unsigned long)-1, "nor is an address below the pool");

    (void)arkrtos_ptable_set_slot(0u, (uint32)ARKRTOS_APP_SLOT_EMPTY, 0u, 0u);
    ck_u((unsigned long)arkrtos_ptable_find_addr(pool_units(1u), 0, 0), (unsigned long)-1,
         "a record whose state is EMPTY is not a hit even if it still holds a range");
}

static void test_slot_accessors(void)
{
    uint32 type = 0u;
    uint32 base = 0u;
    uint32 size = 0u;
    uint32 state = 0u;
    uint32 entry = 0u;
    uint32 task = 0u;

    arkrtos_ptable_init();

    ck_u((unsigned long)arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u),
                                             ARKRTOS_POOL_ALLOC_UNIT), 0ul, "a record");

    ck(arkrtos_ptable_slot_info(0u, &type, &base, &size) == 0, "slot_info succeeds");
    ck_u(type, (unsigned long)ARKRTOS_SLOT_APP, "type");
    ck_u(base, (unsigned long)pool_units(0u), "base");
    ck_u(size, (unsigned long)ARKRTOS_POOL_ALLOC_UNIT, "size");
    ck(arkrtos_ptable_slot_info(0u, 0, 0, 0) == 0, "slot_info accepts null outputs");
    ck(arkrtos_ptable_slot_info(SLOT_MAX, &type, 0, 0) < 0, "slot_info rejects a bad record");

    ck(arkrtos_ptable_set_slot(0u, (uint32)ARKRTOS_APP_SLOT_LOADED, FAKE_ENTRY, 7u) == 0,
       "set_slot writes the triple");
    ck(arkrtos_ptable_get_slot(0u, &state, &entry, &task) == 0, "get_slot reads it back");
    ck_u(state, (unsigned long)ARKRTOS_APP_SLOT_LOADED, "state");
    ck_u(entry, (unsigned long)FAKE_ENTRY, "entry");
    ck_u(task, 7ul, "task id");
    ck(arkrtos_ptable_get_slot(0u, 0, 0, 0) == 0, "get_slot accepts null outputs");
    ck(arkrtos_ptable_get_slot(SLOT_MAX, &state, 0, 0) < 0, "get_slot rejects a bad record");
    ck(arkrtos_ptable_set_slot(SLOT_MAX, 0u, 0u, 0u) < 0, "set_slot rejects a bad record");

    ck_u((unsigned long)arkrtos_ptable_find_task(7u), 0ul, "the task id resolves to its record");
    ck(arkrtos_ptable_find_task(0u) < 0, "task id 0 is not a search key");
    ck_u((unsigned long)arkrtos_ptable_find_task(9u), (unsigned long)-1, "an unknown task id misses");
}

/* read_header 读的是 Flash 里的镜像头——宿主上那段 Flash 由 main 映射出来。 */
static void test_read_header(void)
{
    arkrtos_app_header_t hdr;
    arkrtos_app_header_t *p_flash;
    int32 raw_slot;

    arkrtos_ptable_init();

    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT) == 0,
       "an app record");
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(8u), ARKRTOS_POOL_ALLOC_UNIT) == 1,
       "a second one");
    raw_slot = arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, pool_units(16u),
                                        ARKRTOS_POOL_ALLOC_UNIT, 0u);
    ck(raw_slot == 2, "and a raw one");

    p_flash = (arkrtos_app_header_t *)(uintptr_t)pool_units(0u);
    (void)memset((void *)p_flash, 0, sizeof(*p_flash));
    p_flash->magic = ARKRTOS_APP_MAGIC;
    p_flash->version = 0x0102u;

    (void)memset(&hdr, 0, sizeof(hdr));
    ck(arkrtos_ptable_read_header(0u, &hdr) == 0, "a valid image header reads back");
    ck_u(hdr.magic, (unsigned long)ARKRTOS_APP_MAGIC, "with its magic");
    ck_u(hdr.version, 0x0102ul, "and its version");
    ck(arkrtos_ptable_read_header(0u, 0) == 0, "the output pointer is optional");

    p_flash->magic = ARKRTOS_APP_MAGIC + 1u;
    ck_u((unsigned long)arkrtos_ptable_read_header(0u, &hdr), (unsigned long)-2,
         "a wrong magic is reported as its own code (-2, not -1)");

    ck(arkrtos_ptable_read_header(8u, &hdr) < 0, "an unregistered record has no header");
    ck(arkrtos_ptable_read_header((uint32)raw_slot, &hdr) < 0,
       "a raw image has no header to read");
    ck(arkrtos_ptable_read_header(SLOT_MAX, &hdr) < 0, "an out-of-range record is refused");
}

/* ---------------- Flash 池分配 ---------------- */

static void test_find_free(void)
{
    uint32 base = 0u;
    uint32 aligned = 0u;

    arkrtos_ptable_init();

    ck(arkrtos_ptable_find_free(ARKRTOS_POOL_ALLOC_UNIT, &base, &aligned) == 0,
       "an empty pool has room");
    ck_u(base, (unsigned long)IMAGE_POOL_BASE, "the first free unit is the pool base");
    ck_u(aligned, 1ul, "and it is a power-of-two aligned landing (exact MPU window)");

    ck(arkrtos_ptable_find_free(0u, &base, &aligned) < 0, "zero size is refused");
    ck(arkrtos_ptable_find_free(ARKRTOS_POOL_ALLOC_UNIT, 0, &aligned) < 0,
       "a null output is refused");
    ck(arkrtos_ptable_find_free(IMAGE_POOL_USABLE_SIZE + ARKRTOS_POOL_ALLOC_UNIT,
                                &base, &aligned) < 0,
       "a request larger than the whole usable pool finds nothing");

    /* 造一个「起点没按跨度对齐、对齐落点又放不下」的缺口：
     * 占用 [0,1) 与 [3,4) 单位，于是 [1,3) 两单位正好装下 size=2 单位，
     * 但 align_up(1 单位, 2 单位) 会把落点推到 2 单位、末尾越出缺口。 */
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT) >= 0,
       "occupy the pool base");
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(3u), ARKRTOS_POOL_ALLOC_UNIT) >= 0,
       "and the unit after the gap");

    ck(arkrtos_ptable_find_free(ARKRTOS_POOL_ALLOC_UNIT * 2u, &base, &aligned) == 0,
       "the gap is used even though the aligned landing does not fit");
    ck_u(base, (unsigned long)pool_units(1u), "so the landing is the gap start");
    ck_u(aligned, 0ul, "and it is reported as not exactly aligned (ROM window must widen)");

    /* 记录不按基址顺序占用时，收集阶段必须把它们插到正确位置 */
    arkrtos_ptable_init();
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(8u), ARKRTOS_POOL_ALLOC_UNIT) >= 0,
       "a high record first");
    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT) >= 0,
       "then a low one (the range table has to be re-sorted)");
    ck(arkrtos_ptable_find_free(ARKRTOS_POOL_ALLOC_UNIT, &base, &aligned) == 0,
       "the sorted table still yields a gap");
    ck_u(base, (unsigned long)pool_units(1u), "the first gap is between the two records");

    /* 只剩一条尾巴、且连它都装不下：最后一个缺口也要试过才报无空间 */
    arkrtos_ptable_init();
    ck(arkrtos_ptable_alloc_raw(ARKRTOS_SLOT_APP, IMAGE_POOL_BASE,
                                IMAGE_POOL_USABLE_SIZE - 512u, 0u) >= 0,
       "occupy the pool leaving a 512-byte tail");
    ck(arkrtos_ptable_find_free(ARKRTOS_POOL_ALLOC_UNIT, &base, &aligned) < 0,
       "a tail smaller than one unit is not a landing");
}

static void test_stats(void)
{
    uint32 used = 0u;
    uint32 total = 0u;
    uint32 frag = 0u;

    arkrtos_ptable_init();

    ck(arkrtos_ptable_stats(&used, &total, &frag) == 0, "stats on an empty pool");
    ck_u(used, 0ul, "nothing used");
    ck_u(total, (unsigned long)IMAGE_POOL_USABLE_SIZE, "total is the usable pool");
    ck_u(frag, 1ul, "one free run");

    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(0u), ARKRTOS_POOL_ALLOC_UNIT) == 0,
       "one image");
    ck(arkrtos_ptable_stats(&used, 0, &frag) == 0, "stats with optional outputs");
    ck_u(used, (unsigned long)ARKRTOS_POOL_ALLOC_UNIT, "used counts the image");
    ck_u(frag, 1ul, "still one free run (the tail)");

    ck(arkrtos_ptable_alloc(ARKRTOS_SLOT_APP, pool_units(4u), ARKRTOS_POOL_ALLOC_UNIT) >= 0,
       "a second, non-adjacent image");
    ck(arkrtos_ptable_stats(&used, &total, &frag) == 0, "stats again");
    ck_u(used, (unsigned long)(ARKRTOS_POOL_ALLOC_UNIT * 2u), "both images counted");
    ck_u(frag, 2ul, "now there is a hole between them and the tail");
    ck(arkrtos_ptable_stats(0, 0, 0) == 0, "all outputs are optional");
}

/* ---------------- 镜像 RAM 池 ---------------- */

static void test_ram_alloc(void)
{
    uint32 base = 0u;
    uint32 size = 0u;
    uint32 big_base = 0u;
    uint32 big_size = 0u;

    arkrtos_ptable_init();

    ck(arkrtos_ptable_ram_alloc(0u, &base, &size) < 0, "zero bytes is refused");
    ck(arkrtos_ptable_ram_alloc(SLOT_RAM_MIN_BLOCK, 0, &size) < 0, "a null output is refused");

    ck(arkrtos_ptable_ram_alloc(1u, &base, &size) == 0, "a tiny request is rounded up");
    ck_u(base, (unsigned long)SLOT_RAM_BASE, "the first block starts at the pool base");
    ck_u(size, (unsigned long)SLOT_RAM_MIN_BLOCK, "to the minimum block");
    /* ram_alloc 只算地址、不记账：要让它看见这块，得由调用方 bind 到槽位上
     * （这正是它和 mini_alloc 的分界，见头文件）。 */
    ck(arkrtos_ptable_ram_bind(0u, base, size) == 0, "and it is bound to a record");

    ck(arkrtos_ptable_ram_alloc(SLOT_RAM_MIN_BLOCK + 1u, &base, &size) == 0,
       "a request just over the minimum rounds to the next power of two");
    ck_u(size, (unsigned long)(SLOT_RAM_MIN_BLOCK * 2u), "which is twice the minimum");
    ck_u(base, (unsigned long)(SLOT_RAM_BASE + (2u * SLOT_RAM_MIN_BLOCK)),
         "and it starts on the next multiple of its own size, not right after the bound one");
    ck(arkrtos_ptable_ram_bind(1u, base, size) == 0, "bound as well");

    ck(arkrtos_ptable_ram_alloc(SLOT_RAM_MAX_BLOCK + 1u, &base, &size) < 0,
       "a request above the per-image cap is refused (not truncated)");

    ck(arkrtos_ptable_ram_alloc(SLOT_RAM_MAX_BLOCK, &big_base, &big_size) == 0,
       "the largest allowed block fits");
    ck_u(big_size, (unsigned long)SLOT_RAM_MAX_BLOCK, "at the cap");
    ck_u(big_base % SLOT_RAM_MAX_BLOCK, 0ul,
         "and lands on a multiple of its own size (one MPU region covers it)");
    ck((big_base >= (SLOT_RAM_BASE + SLOT_RAM_MIN_BLOCK)) &&
       ((big_base + big_size) <= (SLOT_RAM_BASE + SLOT_RAM_TOTAL)),
       "inside the pool");
    ck(arkrtos_ptable_ram_bind(2u, big_base, big_size) == 0, "bound as well");

    ck(arkrtos_ptable_ram_alloc(SLOT_RAM_MAX_BLOCK, &base, &size) < 0,
       "the pool cannot hold two maximum blocks");

    /* 槽位绑定的那块也必须被分配器看见 */
    arkrtos_ptable_init();
    ck(arkrtos_ptable_ram_bind(0u, SLOT_RAM_BASE, SLOT_RAM_MIN_BLOCK) == 0,
       "a slot can be bound to a block");
    ck(arkrtos_ptable_ram_alloc(SLOT_RAM_MIN_BLOCK, &base, &size) == 0,
       "the allocator skips the bound block");
    ck_u(base, (unsigned long)(SLOT_RAM_BASE + SLOT_RAM_MIN_BLOCK),
       "and hands out the next one");
}

static void test_ram_reserve_and_info(void)
{
    uint32 base = 0xFFFFFFFFul;
    uint32 size = 0xFFFFFFFFul;

    arkrtos_ptable_init();

    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE, SLOT_RAM_MIN_BLOCK) == 0,
       "a free aligned block can be reserved");
    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE, SLOT_RAM_MIN_BLOCK) == 0,
       "reserving it again is still fine (reserve does not keep a note)");

    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE, SLOT_RAM_MIN_BLOCK + 1u) < 0,
       "a size that is not a power of two is refused");
    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE + 512u, SLOT_RAM_MIN_BLOCK) < 0,
       "a base that is not aligned to the block size is refused");
    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE - SLOT_RAM_MIN_BLOCK, SLOT_RAM_MIN_BLOCK) < 0,
       "a base below the pool is refused");
    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE + SLOT_RAM_TOTAL, SLOT_RAM_MIN_BLOCK) < 0,
       "a block past the pool end is refused");
    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE + SLOT_RAM_TOTAL - SLOT_RAM_MIN_BLOCK,
                                  SLOT_RAM_MIN_BLOCK) == 0,
       "the last aligned block in the pool is legal");

    (void)arkrtos_ptable_ram_bind(0u, SLOT_RAM_BASE, SLOT_RAM_MIN_BLOCK);
    ck(arkrtos_ptable_ram_reserve(SLOT_RAM_BASE, SLOT_RAM_MIN_BLOCK) < 0,
       "a block already bound to a slot is refused (no silent relocation)");

    ck(arkrtos_ptable_ram_info(0u, &base, &size) == 0, "ram_info on a bound record");
    ck_u(base, (unsigned long)SLOT_RAM_BASE, "reports the base");
    ck_u(size, (unsigned long)SLOT_RAM_MIN_BLOCK, "and the size");
    ck(arkrtos_ptable_ram_info(1u, &base, &size) == 0, "an unbound record is not an error");
    ck_u(base, 0ul, "it reports base 0");
    ck_u(size, 0ul, "and size 0");
    ck(arkrtos_ptable_ram_info(0u, 0, 0) == 0, "the outputs are optional");
    ck(arkrtos_ptable_ram_info(SLOT_MAX, &base, &size) < 0, "a bad record is refused");

    ck(arkrtos_ptable_ram_bind(0u, SLOT_RAM_BASE + SLOT_RAM_MIN_BLOCK, SLOT_RAM_MIN_BLOCK) == 0,
       "bind overwrites the previous binding");
    ck(arkrtos_ptable_ram_bind(SLOT_MAX, 0u, 0u) < 0, "bind rejects a bad record");

    (void)memset(&g_crit_depth, 0, sizeof(g_crit_depth));
}

/* ---------------- 小程序的代码块 + RAM 块 ---------------- */

static void test_mini(void)
{
    arkrtos_partition_table_t *pt;
    uint32 idx = 0u;
    uint32 code_base = 0u, code_size = 0u;
    uint32 ram_base = 0u, ram_size = 0u;
    uint32 used = 0u, max = 0u;
    uint32 a = 0u, b = 0u;

    arkrtos_ptable_init();
    pt = tab();

    ck(arkrtos_ptable_mini_alloc(0u, 1024u, &idx, &code_base, &code_size,
                                 &ram_base, &ram_size) < 0, "zero code bytes is refused");
    ck(arkrtos_ptable_mini_alloc(1024u, 1024u, 0, &code_base, &code_size,
                                 &ram_base, &ram_size) < 0, "a null output is refused");
    ck(arkrtos_ptable_mini_alloc(SLOT_RAM_MAX_BLOCK + 1u, 1024u, &idx, &code_base, &code_size,
                                 &ram_base, &ram_size) < 0,
       "a code block above the per-image cap is refused");
    ck(arkrtos_ptable_mini_alloc(1024u, 1u, &idx, &code_base, &code_size,
                                 &ram_base, &ram_size) == 0,
       "a RAM request below the minimum block is padded up");
    ck_u(ram_size, (unsigned long)SLOT_RAM_MIN_BLOCK, "to the minimum block");
    arkrtos_ptable_mini_free(idx);

    arkrtos_ptable_init();
    pt = tab();

    ck(arkrtos_ptable_mini_alloc(500u, 3000u, &idx, &code_base, &code_size,
                                 &ram_base, &ram_size) == 0, "the first mini program fits");
    ck_u(idx, 0ul, "and takes index 0");
    ck_u(code_size, (unsigned long)SLOT_RAM_MIN_BLOCK, "code rounds up to the minimum block");
    ck_u(ram_size, 4096ul, "RAM rounds up to the next power of two");
    ck_u(code_base, (unsigned long)SLOT_RAM_BASE, "code starts at the pool base");
    ck((ram_base != code_base) &&
       ((code_base + code_size) <= ram_base || (ram_base + ram_size) <= code_base),
       "the two blocks never overlap");

    ck(arkrtos_ptable_mini_alloc(1024u, 1024u, &idx, &code_base, &code_size,
                                 &ram_base, &ram_size) == 0, "a second mini program fits");
    ck_u(idx, 1ul, "and takes index 1");

    ck(arkrtos_ptable_mini_used(&used, &max) == 0, "mini_used works");
    ck_u(used, 2ul, "two entries are in use");
    ck_u(max, (unsigned long)ARKRTOS_MINI_ARRAY_MAX, "the limit is the array size");
    ck(arkrtos_ptable_mini_used(0, 0) == 0, "its outputs are optional");

    ck(arkrtos_ptable_mini_info(0u, &a, &b, 0, 0) == 0, "mini_info reads one entry");
    ck_u(a, (unsigned long)SLOT_RAM_BASE, "code base");
    ck_u(b, (unsigned long)SLOT_RAM_MIN_BLOCK, "code size");
    ck(arkrtos_ptable_mini_info(0u, 0, 0, 0, 0) < 0, "mini_info wants at least one output");
    ck(arkrtos_ptable_mini_info(0u, 0, 0, &a, &b) == 0, "mini_info can read just the RAM block");
    ck_u(a, (unsigned long)pt->mini_ram_base[0], "which is the recorded base");
    ck_u(b, (unsigned long)pt->mini_ram_size[0], "and the recorded size");
    ck(arkrtos_ptable_mini_info(ARKRTOS_MINI_ARRAY_MAX, &a, &b, 0, 0) < 0,
       "mini_info rejects a bad index");

    /* 借出的两块必须被普通 RAM 分配看见 */
    ck(arkrtos_ptable_ram_alloc(4096u, &a, &b) == 0, "the pool still has room");
    ck((a != pt->mini_ram_base[0]) && (a != pt->mini_ram_base[1]),
       "but not on top of a borrowed mini block");
    arkrtos_ptable_mini_free(0u);   /* 把这块还回去，下面还要用池 */

    ck(arkrtos_ptable_mini_free(1u) == 0, "an entry can be returned");
    ck(arkrtos_ptable_mini_free(1u) < 0, "returning an empty entry is an error, not a no-op");
    ck(arkrtos_ptable_mini_free(ARKRTOS_MINI_ARRAY_MAX) < 0, "a bad index is refused");
    ck_u(pt->mini_code_base[1], 0ul, "the returned entry is zeroed");

    /* 项满：只剩 empty 记录才算可借 */
    while(arkrtos_ptable_mini_used(&used, &max) == 0 && used < ARKRTOS_MINI_ARRAY_MAX)
    {
        if(arkrtos_ptable_mini_alloc(1024u, 1024u, &idx, &code_base, &code_size,
                                     &ram_base, &ram_size) != 0)
        {
            break;
        }
    }
    ck_u(used, (unsigned long)ARKRTOS_MINI_ARRAY_MAX, "all entries are in use");
    ck(arkrtos_ptable_mini_alloc(1024u, 1024u, &idx, &code_base, &code_size,
                                 &ram_base, &ram_size) < 0,
       "one more mini program is refused rather than sharing a block");
}

/* ---------------- main ---------------- */

int main(void)
{
    if(map_at((unsigned long)CHIP_RAM_BASE, (unsigned long)SHARE_RAM_SIZE) != 0)
    {
        printf("PTABLE-HOST FAIL: cannot map share RAM at 0x%08lX\n",
               (unsigned long)CHIP_RAM_BASE);
        return 1;
    }

    if(map_at((unsigned long)CHIP_FLASH_BASE, (unsigned long)CHIP_FLASH_SIZE) != 0)
    {
        printf("PTABLE-HOST FAIL: cannot map flash at 0x%08lX\n",
               (unsigned long)CHIP_FLASH_BASE);
        return 1;
    }

    test_init_layout();
    test_get_autocreate();
    test_clear_slots_keeps_layout();
    test_range_check();
    test_alloc_basic();
    test_alloc_raw();
    test_free_and_move();
    test_find_addr();
    test_slot_accessors();
    test_read_header();
    test_find_free();
    test_stats();
    test_ram_alloc();
    test_ram_reserve_and_info();
    test_mini();

    if(g_fails != 0)
    {
        printf("PTABLE-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("PTABLE-HOST OK  %d checks\n", g_checks);
    return 0;
}
