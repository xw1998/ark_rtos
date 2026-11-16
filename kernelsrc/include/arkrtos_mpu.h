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

#else

#define arkrtos_paging_stats(p_checked, p_bad, p_bad_base)   (-1)

#endif

#endif
