/**
* @brief Ark RTOS memory protection manager (kernel internal interface)
* @details Kernel code calls only this layer; all MPU register access lives in
*          the port layer (arkrtos_port_mpu_*). Porting to a new architecture
*          therefore only needs a new port implementation.
*          The whole interface collapses to empty macros when neither
*          ARKRTOS_USE_MPU == 1 nor ARKRTOS_USE_PAGING == 1.
*
* @note This header is the **isolation contract**, not an MPU-private API. The
*       five entry points below are implemented twice and only one of them is
*       ever compiled in:
*         - ARKRTOS_USE_MPU     == 1 -> arkrtos_mpu.c     (window backend:
*                                       ARM MPU / RISC-V PMP registers)
*         - ARKRTOS_USE_PAGING  == 1 -> arkrtos_paging.c  (page-table backend:
*                                       LoongArch TLB + PLV)
*       The names keep the historical "mpu" spelling because eight kernel call
*       sites use them (one of them is a GBK source file); the meaning is
*       "apply this task's isolation context", not "write MPU registers".
*       arkrtos_config.h rejects a build that sets both or that asks for paging
*       on a core without a TLB, so the two bodies never collide.
*/

#ifndef __ARKRTOS_MPU_H__
#define __ARKRTOS_MPU_H__

#include "arkrtos_def.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"


#if ((ARKRTOS_USE_MPU == 1) || (ARKRTOS_USE_PAGING == 1))

void arkrtos_mpu_module_init(void);
void arkrtos_mpu_set_app(arkrtos_task_t *p_app);
void arkrtos_mpu_set_idle(void);
void arkrtos_mpu_build_task(arkrtos_task_t *p_task);
void arkrtos_mpu_reset(void);

#else

#define arkrtos_mpu_module_init()
#define arkrtos_mpu_set_app(p_app)
#define arkrtos_mpu_set_idle()
#define arkrtos_mpu_build_task(p_task)
#define arkrtos_mpu_reset()

#endif

/* Page-table backend audit counters. Only the paging backend keeps them: the
 * window backend applies a window immediately and has nothing to audit after
 * the fact, so asking it for these numbers returns -1 instead of inventing
 * zeroes. */
#if (ARKRTOS_USE_PAGING == 1)

int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p_bad_base);

/** @brief Register a run-time dynamic mapping (4 KB granularity)
 *  @param va    Virtual base, must be 4 KB aligned
 *  @param pa    Physical base, must be 4 KB aligned
 *  @param size  Bytes, must be a non-zero multiple of 4 KB and at most
 *               ARKRTOS_PAGING_MAP_MAX_PAGES * 4 KB
 *  @param mem   One of ARKRTOS_MPU_MEM_RAM / _ROM / _PERIPH_RO / _PERIPH_RW
 *  @return 0 = every page is in the TLB and in the audit span list;
 *          <0 = refused, with the port's own reason passed through
 *          (-1 parameter/attribute, -2 overlaps a static span, -3 table full)
 *  @details This is the driver-side "register this MMIO region" entry. It is
 *           deliberately restricted to kernel-visible mappings: App pages need
 *           their own ASID and per-process tables, which is not built yet, and
 *           pretending otherwise here would be an authoritative-looking wrong
 *           answer. A failed call does not roll back the pages that already
 *           went in - there is no unmap path yet (debt U-15), and the count
 *           from arkrtos_paging_map_pages() is how a caller sees how far it got.
 */
int32 arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrtos_mpu_mem_t mem);

/** @brief How many pages this layer has registered (cumulative readout) */
uint32 arkrtos_paging_map_pages(void);

/** @brief Undo pages registered through arkrtos_paging_map_span() (U-15)
 *  @param va   Base of the span, must be 4 KB aligned
 *  @param size Bytes, must be a non-zero multiple of 4 KB
 *  @return 0 = every page is gone; <0 = the port's own reason for the page
 *          that failed (-1 parameter, -2 static span, -4 not registered)
 *  @details Only pages registered through map_span() can be taken out: static
 *           spans are the kernel's own and the port refuses them. A span that
 *           was only partly mapped reports the reason instead of a "done". */
int32 arkrtos_paging_unmap_span(uint32 va, uint32 size);

/** @brief How many dynamic mapping slots are in use right now (readout) */
uint32 arkrtos_paging_dyn_used(void);

/* ------------------------------------------------------------------
 * Controlled kernel access to user memory (U-6)
 *
 * The kernel dereferences whatever pointer an SVC handed it. The boolean
 * helpers in arkrtos_kernel_check.h answer "may I touch this", but they do
 * not copy, and they judge against the board's global windows rather than
 * the calling task's own recorded span. These two do the whole job: check
 * the range, then copy, and never let an illegal pointer turn into a fault
 * inside the kernel.
 *
 * Refusal codes (identical for both directions):
 *    -1 null pointer / zero length / range wraps at 4 GB
 *    -2 the task has no user window at all (a kernel task)
 *    -3 the whole range is not inside the task's own recorded windows
 *    -4 the installed App window is not this task's address space, so the
 *       copy would read/write another App's frame (silent wrong answer)
 *
 * Reading is allowed from the task's code window and its data window;
 * writing is allowed only from its data window. A task's code window is its
 * own image - a kernel path that could patch it is not something any caller
 * needs, so it is refused rather than merely discouraged.
 * ------------------------------------------------------------------ */

/** @brief Copy from a user buffer into a kernel buffer
 *  @param dst  Kernel destination
 *  @param usrc User source (validated against the calling task's windows)
 *  @param len  Bytes
 *  @return 0 = copied; -1/-2/-3/-4 as listed above
 *  @details The caller is arkrtos_task_get_current(). On a build where the
 *           calling task is a kernel task there is no user window, and the
 *           answer is -2 instead of an unchecked memcpy. */
int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len);

/** @brief Copy from a kernel buffer into a user buffer
 *  @return 0 = copied; -1/-2/-3/-4 as listed above */
int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len);

/** @brief Same, for a task named explicitly (kernel internal)
 *  @details Used where the kernel works on behalf of a task that is not the
 *           current one - a loader writing a program's argument block, for
 *           instance. The caller must have installed that task's address
 *           space first; if it did not, the copy is refused with -4 instead
 *           of quietly reaching into whatever frame is installed. */
int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst,
                                  const void *usrc, uint32 len);
int32 arkrtos_copy_to_user_task(arkrtos_task_t *p_task, void *udst,
                                const void *src, uint32 len);

/** @brief The check-and-copy body, with the task's windows passed as numbers
 *  @param rom_start/rom_size Code window (read only), 0 when there is none
 *  @param ram_start/ram_size Data window (read/write), 0 when there is none
 *  @param space     Address space the task is bound to
 *  @param installed Space currently installed in the App window
 *  @param kern      Kernel side buffer
 *  @param user      User side address
 *  @param len       Bytes
 *  @param write     1 = user is the destination, 0 = user is the source
 *  @return 0 = copied; -1/-2/-3/-4 as listed above
 *  @details This exists so the QEMU self-test can exercise every refusal
 *           without manufacturing a fake TCB: the guards are the same code
 *           the task-based entry points run, only the numbers arrive
 *           directly. Kernel internal; Apps never see it. */
int32 arkrtos_paging_user_copy(uint32 rom_start, uint32 rom_size,
                               uint32 ram_start, uint32 ram_size,
                               uint32 space, uint32 installed,
                               void *kern, const void *user, uint32 len,
                               uint8 write);

/* ------------------------------------------------------------------
 * Per-App address spaces (U-1)
 *
 * What "one page table per process" means on a core without a hardware page
 * walker (this port): each App owns a set of TLB entries, and the scheduler
 * re-installs that set on every switch. The App window keeps the SAME virtual
 * address in every space; only the physical frame behind it changes. So two
 * Apps cannot address each other's memory at all - there is no VA that maps
 * to the other App's frame.
 *
 * Space 0 is the frame the image/loader has always used (PA = the window
 * base), so a build that never binds a task behaves exactly as before.
 * ------------------------------------------------------------------ */

/** @brief How many address spaces the board provides (>= 1) */
uint32 arkrtos_paging_space_count(void);

/** @brief Physical frame base behind a space; 0 when the space does not exist */
uint32 arkrtos_paging_space_frame(uint32 space);

/** @brief Virtual base of the App window (the same VA for every space)
 *  @return the window VA base; 0 when the build has no paging backend
 *  @details Use this - not arkrtos_paging_space_frame() - when poking the
 *           window by address.  With ARKRTOS_USE_PAGING_PAGES=1 the frames come
 *           from the frame pool, so a frame base is no longer a VA. */
uint32 arkrtos_paging_window_va(void);

/** @brief Which space is currently installed in the App window */
uint32 arkrtos_paging_space_current(void);

/** @brief Install a space right now (kernel-side, e.g. before writing a load)
 *  @return 0 = installed or already there; -1 = bad space / no TLB path
 *  @details The scheduler also does this on every switch to an App, from the
 *           task's binding. Calling it from a kernel task is what lets a loader
 *           write into a frame the kernel is not currently "in". */
int32 arkrtos_paging_space_activate(uint32 space);

/** @brief Bind a task to an address space (default for every task is 0)
 *  @return 0 = bound; -1 = bad task or bad space
 *  @details Only App tasks (is_priv == 0) trigger a switch on schedule; a
 *           kernel task leaves whatever space is installed alone, because
 *           kernel access to a frame is a deliberate, momentary thing. */
int32 arkrtos_paging_bind_task(arkrtos_task_t *p_task, uint32 space);

/** @brief Space a task is bound to (0 when unknown) */
uint32 arkrtos_paging_space_of(arkrtos_task_t *p_task);

/* ------------------------------------------------------------------
 * 4 KB page-frame pool (U-3)
 *
 * The pool is a board-given run of physical memory (ARKRTOS_QEMU_FRAME_BASE
 * / COUNT) that the port book-keeps in a bitmap and maps identity so the
 * kernel can reach it.  This layer is deliberately thin: it forwards to the
 * port and turns "there is no pool" into the same refusal codes, so a caller
 * never has to know which build it is in.  It does NOT install mappings and
 * does NOT touch the App window - turning a frame into an App-visible page is
 * a later stage with its own switch.
 *
 *   alloc(count, &base)  0 / -1 bad arg / -2 no run that long
 *   free(base, count)    0 / -1 outside pool / -3 run has a free frame
 * ------------------------------------------------------------------ */

/** @brief Allocate a run of contiguous 4 KB page frames */
int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base);

/** @brief Return a run of contiguous 4 KB page frames (all-or-nothing) */
int32 arkrtos_paging_frame_free(uint32 base, uint32 count);

/** @brief Total frames in the board pool (0 when there is none) */
uint32 arkrtos_paging_frame_total(void);

/** @brief Frames currently allocated from the board pool */
uint32 arkrtos_paging_frame_used(void);

#else

#define arkrtos_paging_stats(p_checked, p_bad, p_bad_base)   (-1)
#define arkrtos_paging_map_span(va, pa, size, mem)           (-1)
#define arkrtos_paging_map_pages()                           (0u)
#define arkrtos_paging_unmap_span(va, size)                  (-1)
#define arkrtos_paging_dyn_used()                            (0u)
#define arkrtos_copy_from_user(dst, usrc, len)               (-1)
#define arkrtos_copy_to_user(udst, src, len)                 (-1)
#define arkrtos_copy_from_user_task(p_task, dst, usrc, len)  (-1)
#define arkrtos_copy_to_user_task(p_task, udst, src, len)    (-1)
#define arkrtos_paging_user_copy(rom_start, rom_size, ram_start, ram_size, \
                                 space, installed, kern, user, len, write) (-1)
#define arkrtos_paging_space_count()                         (1u)
#define arkrtos_paging_space_frame(space)                    (0u)
#define arkrtos_paging_window_va()                           (0u)
#define arkrtos_paging_space_current()                       (0u)
#define arkrtos_paging_space_activate(space)                 (-1)
#define arkrtos_paging_bind_task(p_task, space)              (-1)
#define arkrtos_paging_space_of(p_task)                      (0u)
#define arkrtos_paging_frame_alloc(count, p_base)            (-1)
#define arkrtos_paging_frame_free(base, count)               (-1)
#define arkrtos_paging_frame_total()                         (0u)
#define arkrtos_paging_frame_used()                          (0u)

#endif

#endif
