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
*            NOT   - a concurrent second instance is possible since U-2
*                    (exec_space); space recycling still needs a caller that
*                    frees the frames it was handed;
*                  - demand paging; ASID; TLB shootdown;
*                  - the App window is still one 2 MB PS=20 entry per space,
*                    NOT a set of 4 KB pages.  U-3 stage 1 (2026-11-17) added
*                    the 4 KB page-frame pool and the alloc/free/total/used
*                    interface (pmap frame / pmap ftest) but deliberately did
*                    not rewire the window: a pool-sized allocation is only
*                    useful once the window can be paged, which is the next
*                    stage behind its own switch;
*                  - the SVC handlers still validate pointers with the
*                    boolean helpers in arkrtos_kernel_check.h: U-6 added
*                    copy_from_user / copy_to_user and self-tests them, but
*                    did not rewrite those handlers to stage through them;
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

    /* switch_to(), not activate(): this runs inside the switch path, on the
     * outgoing task's own stack. When that task is an App the stack itself lives
     * in the App window, so rewriting the window entry here would make the
     * caller's next stack access read the other task's frame. The port defers
     * the write to its restore path in that case (debt U-2). */
    (void)arkrtos_port_paging_switch_to((uint32)s_pg_task_space[idx]);
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
* @brief Virtual base of the App window (the same VA for every space)
* @details 窗口的 VA 是 port 的属性，物理侧才按空间换。页级窗口
*          （ARKRTOS_USE_PAGING_PAGES=1）下帧来自帧池，帧基址不再是 VA，
*          所以按地址摸窗口的代码必须问这一条。
*/
unsigned long arkrtos_paging_window_va(void)
{
    return arkrtos_port_paging_window_va();
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
* @brief 把一个地址空间的帧还给帧池（空间回收，U-3 遗留）
* @param space 0 .. arkrtos_paging_space_count()-1
* @return 0 = 已归还（本来就没拿过也算 0，幂等）；-1 = 空间号越界；
*         -2 = 这一档没有帧池（PS=20 静态窗口）；-3 = 调用者正站在窗口里，
*         本轮一个字节都不动；-4 = 帧池拒绝（原样透出它的 -1/-3）
* @details 调用点只有装载器：它挑中的空间一定没有活实例（台账判据），所以
*          撤窗口条目不会打断任何正在跑的程序。撤条目的理由见 port 侧注释
*          —— 帧回到池子里而旧条目还指着它，是「两个空间共享同一块物理页」
*          这种只在很久以后才显形的错。归还之后空间仍然存在，下一次
*          activate() 重新要帧，于是反复 exec 不再把池子一格一格吃掉。
*/
int32 arkrtos_paging_space_release(uint32 space)
{
    if(space >= arkrtos_port_paging_space_count())
    {
        return -1;
    }

    return arkrtos_port_paging_space_release(space);
}

/* ------------------------------------------------------------------
 * 4 KB page-frame pool (U-3)
 *
 * Thin forwards to the port.  They exist so the shell and any kernel code
 * talk to one name and get the same refusal codes whether or not the board
 * has a pool; when there is none the macros in arkrtos_mpu.h answer instead
 * and these bodies never compile.
 * ------------------------------------------------------------------ */

/** @brief Allocate a run of contiguous 4 KB page frames from the board pool */
int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base)
{
    return arkrtos_port_paging_frame_alloc(count, p_base);
}

/** @brief Return a run of contiguous 4 KB page frames (all-or-nothing) */
int32 arkrtos_paging_frame_free(uint32 base, uint32 count)
{
    return arkrtos_port_paging_frame_free(base, count);
}

/** @brief Total frames in the board pool (0 when there is none) */
uint32 arkrtos_paging_frame_total(void)
{
    return arkrtos_port_paging_frame_total();
}

/** @brief Frames currently allocated from the board pool */
uint32 arkrtos_paging_frame_used(void)
{
    return arkrtos_port_paging_frame_used();
}

#if (ARKRTOS_USE_PAGING_SWAP == 1)
/** @brief Page replacement readout (U-16): forwards to the port's ledger */
uint32 arkrtos_paging_swap_stat(uint32 which)
{
    return arkrtos_port_paging_swap_stat(which);
}
#endif

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
* @brief Undo pages registered through map_span() (U-15)
* @param va   Base of the span, 4 KB aligned
* @param size Bytes, non-zero multiple of 4 KB
* @return 0 = every page is gone; <0 = the port's own reason for the page that
*         failed (-1 alignment, -2 static span, -4 not registered)
* @details The loop stops at the first refusal on purpose: a caller that asks
*          to unmap a span it never mapped should hear about it instead of
*          getting a "done" that quietly skipped half the pages. Pages already
*          unmapped stay unmapped (same honest partial-progress rule as
*          map_span(), and for the same reason: no rollback without a re-map).
*
*          s_pg_map_pages counts pages that ARE mapped, so it drops by exactly
*          the number of pages this call took out.
*/
int32 arkrtos_paging_unmap_span(uint32 va, uint32 size)
{
    uint32 off;
    uint32 done = 0u;

    if((size == 0u) || ((size & 0xFFFu) != 0u) || ((va & 0xFFFu) != 0u))
    {
        return -1;
    }

    for(off = 0u; off < size; off += 0x1000u)
    {
        int32 rc = arkrtos_port_paging_unalias(va + off);

        if(rc != 0)
        {
            s_pg_map_pages -= done;
            return rc;
        }

        done++;
    }

    s_pg_map_pages -= done;
    return 0;
}

/**
* @brief How many dynamic mapping slots are in use right now
* @return used slot count (0 on any build without the paging backend)
* @details This is the readout that was missing while maps could only be
*          added: a driver that registered eight 4 KB pages had no way to see
*          how close it was to the table being full.
*/
uint32 arkrtos_paging_dyn_used(void)
{
    return arkrtos_port_paging_dyn_used();
}

/* ------------------------------------------------- user copies (U-6) */

/**
* @brief 整段是否落在给定窗口里
* @param base/size 窗口（base 不为 0 才算有效）
* @param uva/len   待判的一段
* @return 1 = 整段在窗口内；0 = 不在 / 窗口记录坏了
* @details 判据是「整段」不是「首地址」：只查首字节时，一个刚好跨出窗口
*          末尾的指针会让拷贝循环走进未映射页，在内核态变成 refill 致命
*          —— 那正是这条接口要消灭的「内核陪着 fault」。
*/
static uint8 arkrtos_paging_span_has(unsigned long base, uint32 size,
                                     unsigned long uva, uint32 len)
{
    if((base == 0u) || (size == 0u) || ((base + size) < base))
    {
        return 0u;                       /* 窗口记录坏了：不放行，也不猜 */
    }

    return ((uva >= base) && ((uva + len) <= (base + size))) ? 1u : 0u;
}

/**
* @brief 受控访问用户地址的本体（读写两个方向共用一套判据）
* @return 0 = 已拷贝；-1 参数非法；-2 没有用户窗口；-3 整段不在窗口内；
*         -4 当前装着的不是调用者自己的地址空间
* @details 拷贝走的是「当前装着的那份翻译」，所以 -4 不是多余的保守：
*          空间不对时读到的是另一个 App 的帧，那是**静默错答案**，比报错
*          糟得多。判据全部在第一次 load/store 之前完成，因此拒绝一次非法
*          指针不会在内核态产生任何异常。
*/
int32 arkrtos_paging_user_copy(unsigned long rom_start, uint32 rom_size,
                               unsigned long ram_start, uint32 ram_size,
                               uint32 space, uint32 installed,
                               void *kern, const void *user, uint32 len,
                               uint8 write)
{
    unsigned long uva = (unsigned long)user;
    uint32 i;

    if((kern == 0) || (user == 0) || (len == 0u) ||
       ((~0UL - uva) < (unsigned long)len))
    {
        return -1;
    }

    if((rom_start == 0u) && (ram_start == 0u))
    {
        return -2;                       /* 没有用户窗口：这不是用户指针 */
    }

    if(space != installed)
    {
        return -4;
    }

    if(arkrtos_paging_span_has(ram_start, ram_size, uva, len) == 0u)
    {
        /* 写方向只认数据窗口：代码窗口是任务自己的镜像，内核路径没有
         * 任何理由去改它。 */
        if((write != 0u) ||
           (arkrtos_paging_span_has(rom_start, rom_size, uva, len) == 0u))
        {
            return -3;
        }
    }

    if(write != 0u)
    {
        volatile uint8 *p = (volatile uint8 *)user;

        for(i = 0u; i < len; i++)
        {
            p[i] = ((const uint8 *)kern)[i];
        }
    }
    else
    {
        const volatile uint8 *p = (const volatile uint8 *)user;

        for(i = 0u; i < len; i++)
        {
            ((uint8 *)kern)[i] = p[i];
        }
    }

    return 0;
}

/**
* @brief 从指定任务的用户缓冲区拷到内核缓冲区（内核内部）
* @return 0 = 已拷贝；-1/-2/-3/-4 同上
* @details 内核任务（is_priv != 0）一律 -2：它没有用户窗口，拿一个「用户
*          指针」进来是契约错误，不应该被当成合法调用。
*/
int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst,
                                  const void *usrc, uint32 len)
{
    if((p_task == 0) || (p_task->is_priv != 0u))
    {
        return -2;
    }

    return arkrtos_paging_user_copy(p_task->rom_start, p_task->rom_size,
                                    p_task->ram_start, p_task->ram_size,
                                    arkrtos_paging_space_of(p_task),
                                    arkrtos_port_paging_space(),
                                    dst, usrc, len, 0u);
}

/**
* @brief 从内核缓冲区拷到指定任务的用户缓冲区（内核内部）
*/
int32 arkrtos_copy_to_user_task(arkrtos_task_t *p_task, void *udst,
                                const void *src, uint32 len)
{
    if((p_task == 0) || (p_task->is_priv != 0u))
    {
        return -2;
    }

    return arkrtos_paging_user_copy(p_task->rom_start, p_task->rom_size,
                                    p_task->ram_start, p_task->ram_size,
                                    arkrtos_paging_space_of(p_task),
                                    arkrtos_port_paging_space(),
                                    (void *)src, udst, len, 1u);
}

/**
* @brief 从当前任务的用户缓冲区拷出
*/
int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len)
{
    return arkrtos_copy_from_user_task(arkrtos_task_get_current(), dst, usrc, len);
}

/**
* @brief 往当前任务的用户缓冲区拷入
*/
int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len)
{
    return arkrtos_copy_to_user_task(arkrtos_task_get_current(), udst, src, len);
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
