/**
* @file arkrtos_paging.c
* @brief Ark RTOS isolation backend #2: page table (MMU)
* @details This file implements exactly the same five entry points as
*          arkrtos_mpu.c (see arkrtos_mpu.h): the kernel keeps calling
*          arkrtos_mpu_module_init/_build_task/_set_app/_set_idle/_reset, and
*          which body answers them is decided once, by config:
*
*            ARKRTOS_USE_MPU    == 1 -> arkrtos_mpu.c     (window backend)
*            ARKRTOS_USE_PAGING == 1 -> this file         (page-table backend)
*
*          arkrtos_config.h rejects a build where both are 1, and rejects
*          ARKRTOS_USE_PAGING == 1 on a core whose ARKRTOS_ARCH_HAS_PAGING_HW
*          is 0. There is no silent fallback in either direction.
*
*          What this backend does, stated so the readout is not read as more
*          than it is:
*
*            1. module_init() brings the port's page tables up. On LoongArch
*               the static map is already installed by la_mmu_init() before
*               interrupts are enabled, so the port call only reports whether
*               that map is usable; it does not rebuild it.
*            2. build_task() audits every address span the kernel recorded in
*               the TCB (rom_start/rom_size, ram_start/ram_size) against the
*               spans this port actually maps. A span that no mapping covers
*               - or covers with fewer rights than the task needs - is counted
*               and remembered, so "the image landed somewhere the TLB will
*               never translate" becomes a number instead of a mysterious
*               refill fault at first fetch.
*            3. set_app() DOES switch address spaces as of U-1 (2026-11-17):
*               each App task carries a space number (task -> space table in
*               this file), and switching to it re-installs the App window's
*               single PS=20 MTLB entry to point at that space's physical
*               frame. The virtual address of the window is identical in every
*               space, so an App cannot name another App's frame at all. Kernel
*               tasks do not switch: they keep whatever space is installed and
*               use arkrtos_paging_space_activate() when they deliberately
*               write into a given frame. There is still no ASID (all 0) and no
*               hardware page walker on this port, so "per-process page table"
*               means "per-process set of entries", not a walkable table.
*
*          What IS here as of the MMIO registration round: map_span(), a 4 KB
*          granular run-time mapping request that the port turns into real TLB
*          entries and that check() then honours. It is kernel-visible only
*          (PLV0); an App-visible dynamic mapping needs the per-process table
*          below, so it is refused rather than approximated.
*
*          What U-1 delivers (2026-11-17) and what it does not:
*            DONE  - App tasks bind to an address space, the scheduler installs
*                    it on switch, the same VA maps to a different physical
*                    frame per space, and the kernel keeps its static direct
*                    map untouched. Demonstrated in QEMU with data written and
*                    read back through the same VA in two spaces (pmap u1).
*            NOT   - a page-frame allocator (frames are a fixed board-level
*                    array, space n -> window base + n * window size);
*                  - the loader running two instances: arkrtos_elf.c still
*                    loads into space 0 and refuses a second program (U-2);
*                  - copy_from_user / copy_to_user; demand paging; ASID;
*                  - the PLV3 reverse probes for a second instance (there is
*                    no second runnable App on this board yet).
*          The acceptance test for the remaining debt is unchanged: two Apps
*          each own a space, the same VA maps to different PA, three reverse
*          probes are refused, and the kernel keeps its static direct map.
*
* @note ARKRTOS_USE_PAGING == 0 removes the whole body: the translation unit is
*       empty, so the default build (ARM Cortex-M, RV32 PMP) keeps byte-for-byte
*       the same image.
*
* @author xw
* @date 2026.11.16
*/

#include "arkrtos_mpu.h"

#if (ARKRTOS_USE_PAGING == 1)

#include "arkrtos_partition.h"    /* kernel only: single source of addresses */
#include "arkrtos_cfg.h"          /* arkrtos_task_table: task -> space binding */

/* ------------------------------------------------------------ audit state */

static uint32 s_pg_init_err;      /* 0 = port brought the map up, else its code */
static uint32 s_pg_checked;       /* spans audited since boot */
static uint32 s_pg_bad;           /* spans that no mapping covers properly */
static uint32 s_pg_bad_base;      /* start address of the first bad span */
static uint32 s_pg_map_pages;     /* pages registered through map_span() */

/* Task -> address space binding. Kept here rather than in the TCB so that the
 * window backend and every non-paging build are untouched: a task that is never
 * bound stays at 0, which is the frame this port has always used. */
static uint8  s_pg_task_space[ARKRTOS_TASK_MAX_NUM];

/* One call may register at most this many 4 KB pages. The port keeps 8 entry
 * pairs (one STLB entry covers two pages), and a device window larger than
 * 64 KB is not something a driver maps page by page anyway: it wants a large
 * page, which this interface deliberately does not hand out. */
#define ARKRTOS_PAGING_MAP_MAX_PAGES    16u

/* ---------------------------------------------------------------- helpers */

/** Is this task's code window in the RAM pool (a MiniApp block) rather than in
 *  the flash image pool? Same test as the window backend uses, and for the same
 *  reason: it is the recorded window that says so, not a flag that could lie. */
static uint8 arkrtos_paging_code_in_ram(const arkrtos_task_t *p_task)
{
    uint32 start = p_task->rom_start;

    return ((start >= ARKRTOS_MPU_SLOT_RAM_BASE) &&
            (start <  (ARKRTOS_MPU_SLOT_RAM_BASE + ARKRTOS_MPU_SLOT_RAM_TOTAL))) ? 1u : 0u;
}

/* Audit one span. A bad span is counted once and never "repaired": the static
 * map cannot be widened here, and guessing a wider span would hand the task
 * memory it never asked for. */
static void arkrtos_paging_audit(uint32 base, uint32 size, arkrtos_mpu_mem_t mem)
{
    if(size == 0u)
    {
        return;
    }

    s_pg_checked++;

    if(arkrtos_port_paging_check(base, size, mem) != 0)
    {
        if(s_pg_bad == 0u)
        {
            s_pg_bad_base = base;
        }

        s_pg_bad++;
    }
}

/* ------------------------------------------------------------ public API */

void arkrtos_mpu_module_init(void)
{
    s_pg_init_err  = 0u;
    s_pg_checked   = 0u;
    s_pg_bad       = 0u;
    s_pg_bad_base  = 0u;
    s_pg_map_pages = 0u;

    {
        uint32 i;

        for(i = 0u; i < (uint32)ARKRTOS_TASK_MAX_NUM; i++)
        {
            s_pg_task_space[i] = 0u;
        }
    }

    if(arkrtos_port_paging_init() != 0)
    {
        s_pg_init_err = 1u;
    }
}

/* Audit the windows the loader wrote into the TCB. Nothing is applied to
 * hardware here: a page table entry for an App lives in the shared static map,
 * and the task's privilege level is carried by its trap frame. */
void arkrtos_mpu_build_task(arkrtos_task_t *p_task)
{
    if(p_task == 0)
    {
        return;
    }

    /* Kernel internal tasks (rom_start == 0) execute the kernel's own image,
     * which the low 16 MB identity map already covers at PLV0. Auditing them
     * would only re-ask a question whose answer is a constant. */
    if(p_task->rom_start != 0u)
    {
        arkrtos_paging_audit(p_task->rom_start, p_task->rom_size,
                             (arkrtos_paging_code_in_ram(p_task) != 0u)
                                 ? ARKRTOS_MPU_MEM_RAMX   /* MiniApp: code in RAM */
                                 : ARKRTOS_MPU_MEM_ROM);

        arkrtos_paging_audit(p_task->ram_start, p_task->ram_size, ARKRTOS_MPU_MEM_RAM);
    }
}

void arkrtos_mpu_set_app(arkrtos_task_t *p_app)
{
    int32 idx;

    if(p_app == 0)
    {
        return;
    }

    /* Kernel tasks (is_priv != 0) do not own an address space: they run on the
     * kernel identity map, and when they touch an App window they mean
     * "whatever is installed right now" (a loader writing into a frame does it
     * through arkrtos_paging_space_activate() first). Switching here would
     * make the meaning of 0x02000000 depend on scheduling order. */
    if(p_app->is_priv != 0u)
    {
        return;
    }

    idx = (int32)(p_app - arkrtos_task_table);
    if((idx < 0) || (idx >= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return;
    }

    (void)arkrtos_port_paging_activate((uint32)s_pg_task_space[idx]);
}

void arkrtos_mpu_set_idle(void)
{
    /* Same reason as set_app(): restoring the idle context is a privilege
     * change, not a table switch. */
}

void arkrtos_mpu_reset(void)
{
    /* The static map is never torn down; the only thing that has state is
     * which App frame is installed, and going back to space 0 is the value
     * every build started with. */
    (void)arkrtos_port_paging_activate(0u);
}

/* ----------------------------------------------------- address spaces (U-1) */

/**
* @brief How many address spaces the board provides
* @details 1 means "only the frame this port has always used": nothing about
*          the build changes until the board asks for more.
*/
uint32 arkrtos_paging_space_count(void)
{
    return arkrtos_port_paging_space_count();
}

/**
* @brief Physical frame base behind an address space
* @return frame base, or 0 when the space does not exist (never a guess)
*/
uint32 arkrtos_paging_space_frame(uint32 space)
{
    return arkrtos_port_paging_space_frame(space);
}

/**
* @brief Which space is currently installed in the App window
*/
uint32 arkrtos_paging_space_current(void)
{
    return arkrtos_port_paging_space();
}

/**
* @brief Install an address space now
* @param space 0 .. arkrtos_paging_space_count()-1
* @return 0 = installed or already installed; -1 = bad space / no TLB path
* @details The scheduler calls this indirectly on every switch to an App
*          (arkrtos_mpu_set_app -> the task's binding). A kernel task calls it
*          directly when it is about to write into a frame that is not the one
*          currently installed - the ELF loader will do exactly that once
*          second-instance loading exists (debt U-2).
*/
int32 arkrtos_paging_space_activate(uint32 space)
{
    if(space >= arkrtos_port_paging_space_count())
    {
        return -1;
    }

    return arkrtos_port_paging_activate(space);
}

/**
* @brief Bind a task to an address space
* @return 0 = bound; -1 = bad task pointer / bad space
* @details Binding only stores a number; the switch happens lazily, at the next
*          schedule to that task. That is deliberate: binding inside the loader
*          must not change the translation of the frame it is still writing.
*/
int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space)
{
    int32 idx;

    if((p_task == 0) || (space >= arkrtos_port_paging_space_count()))
    {
        return -1;
    }

    idx = (int32)(p_task - arkrtos_task_table);
    if((idx < 0) || (idx >= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return -1;
    }

    s_pg_task_space[idx] = (uint8)space;
    return 0;
}

/**
* @brief Space a task is bound to (0 for unbound / unknown)
*/
uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task)
{
    int32 idx;

    if(p_task == 0)
    {
        return 0u;
    }

    idx = (int32)(p_task - arkrtos_task_table);
    if((idx < 0) || (idx >= (int32)ARKRTOS_TASK_MAX_NUM))
    {
        return 0u;
    }

    return (uint32)s_pg_task_space[idx];
}

/**
* @brief Register a run-time dynamic mapping, page by page
* @param va    virtual base (4 KB aligned)
* @param pa    physical base (4 KB aligned)
* @param size  bytes (non-zero multiple of 4 KB)
* @param mem   RAM / ROM / PERIPH_RO / PERIPH_RW
* @return 0 = all pages registered and auditable; -1 = refused
* @details The port owns the TLB, so it also owns the decision of what may be
*          added at run time (static-overlap refusal, STLB way budget). This
*          layer only does the arithmetic: alignment, the page count cap, and
*          the page-by-page loop so a caller never has to remember that one
*          TLB entry covers two pages.
*
*          A failure part-way leaves the earlier pages in place. That is not a
*          silent partial success: the return code is -1 and
*          arkrtos_paging_map_pages() shows how many pages did land. The honest
*          reason is that there is no unmap path on this port yet (debt U-15),
*          and pretending to roll back would need one.
*/
int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrtos_mpu_mem_t mem)
{
    uint32 off;

    if((size == 0u) || ((size & 0xFFFu) != 0u) ||
       ((va & 0xFFFu) != 0u) || ((pa & 0xFFFu) != 0u))
    {
        return -1;
    }

    if(size > (ARKRTOS_PAGING_MAP_MAX_PAGES * 0x1000u))
    {
        return -1;
    }

    for(off = 0u; off < size; off += 0x1000u)
    {
        int32 rc = arkrtos_port_paging_alias(va + off, pa + off, mem);

        if(rc != 0)
        {
            /* port 的拒绝原因（-2 与静态映射重叠 / -3 动态表满 / -5 同一 VA
             * 已登记成别的物理页 / …）原样透出：压成 -1 只能让人猜。 */
            return rc;
        }

        s_pg_map_pages++;
    }

    return 0;
}

/**
* @brief How many pages have been registered through map_span()
* @return cumulative page count (0 on any build without the paging backend)
*/
uint32 arkrtos_paging_map_pages(void)
{
    return s_pg_map_pages;
}

/**
* @brief Read the audit counters (kernel/shell only).
* @param p_checked   Receives the number of spans audited, may be 0.
* @param p_bad       Receives the number of spans no mapping covers, may be 0.
* @param p_bad_base  Receives the base of the first bad span, may be 0.
* @return 0 when the port brought the page tables up, -1 when it did not.
*/
int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p_bad_base)
{
    if(p_checked != 0)
    {
        *p_checked = s_pg_checked;
    }

    if(p_bad != 0)
    {
        *p_bad = s_pg_bad;
    }

    if(p_bad_base != 0)
    {
        *p_bad_base = s_pg_bad_base;
    }

    return (s_pg_init_err == 0u) ? 0 : -1;
}

#endif
