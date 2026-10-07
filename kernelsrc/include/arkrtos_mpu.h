/**
* @brief Ark RTOS memory protection manager (kernel internal interface)
* @details Kernel code calls only this layer; all MPU register access lives in
*          the port layer (arkrtos_port_mpu_*). Porting to a new architecture
*          therefore only needs a new port implementation.
*          The whole interface collapses to empty macros when ARKRTOS_USE_MPU == 0.
*/

#ifndef __ARKRTOS_MPU_H__
#define __ARKRTOS_MPU_H__

#include "arkrtos_def.h"
#include "arkrtos_hal.h"
#include "arkrtos_task.h"
#include "arkrtos_config.h"

/* Region index layout used by every task context built in arkrtos_mpu.c. */
#define ARKRTOS_MPU_RGN_CODE       (0u)   /* task code window (read-only, executable) */
#define ARKRTOS_MPU_RGN_DATA       (1u)   /* task data/stack window (read/write, XN) */
#define ARKRTOS_MPU_RGN_SHARE      (2u)   /* kernel shared RAM (partition table) */
#define ARKRTOS_MPU_RGN_PERIPH1    (3u)   /* peripheral window 1 (board configured) */
#define ARKRTOS_MPU_RGN_PERIPH2    (4u)   /* peripheral window 2 (board configured) */

#if (ARKRTOS_USE_MPU == 1)

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

#endif
