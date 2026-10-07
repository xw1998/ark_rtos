/**
* @brief Ark RTOS 板级配置 - STM32F427
* @details 覆盖内核默认配置，适配 STM32F427 硬件特性
*          通过编译选项 -DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\" 包含
*          此文件同时包含CMSIS设备头文件，供port层使用寄存器定义
*/

#ifndef __ARKRTOS_BOARD_CONFIG_H__
#define __ARKRTOS_BOARD_CONFIG_H__

#include "stm32f4xx.h"

#undef  ARKRTOS_CPU_ARCH
#define ARKRTOS_CPU_ARCH            ARKRTOS_ARCH_CORTEX_M4


#define ARKRTOS_SHARE_MEM_SIZE      (0x8000)

#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU             1

/* Memory protection (Cortex-M4 MPU) plus the privilege separation that makes
 * it bite: with ARKRTOS_USE_PRIV = 1 an App task runs unprivileged, so it can
 * only reach kernel services through an SVC and can only touch the windows
 * arkrtos_mpu.c builds for it. Drivers stay privileged on purpose (they program
 * peripheral registers directly), and PRIVDEFENA gives privileged code the
 * default memory map, so enabling this costs the drivers nothing.
 * Honest boundary: only unprivileged tasks are confined. The kernel and the
 * drivers still run privileged - this is fault containment, not a defence
 * against hostile code. The shared partition table window is still writable
 * by an App (see the memory-protection guide under docs/). */
#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU             1

#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV            1

/* Independent watchdog (IWDG). Hardware fact, so it lives with the board:
 * 1 = the kernel guard owns the counter and stops feeding when an invariant
 *     is violated (see arkrtos_guard.c);
 * 0 = no watchdog; nothing resets the board from inside the kernel.
 * Bring-up order matters: the IWDG cannot be stopped once started, so this
 * is enabled only after the feed path has been verified with a long timeout. */
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE          1

/* Timeout asked for when ARKRTOS_WDG_ENABLE is 1. The guard reports what the
 * hardware actually got (arkrtos_port_wdg_timeout_ms), not this number. */
#undef  ARKRTOS_WDG_TIMEOUT_MS
#define ARKRTOS_WDG_TIMEOUT_MS      4000


/* Development bypass: let the kernel treat a raw image burned directly
 * at the slot base as runnable, so the fixed-address flash + MDK
 * breakpoint workflow keeps working. Release firmware must reset it to 0
 * and accept only CRC-checked .svcapp packages. */
#undef  APP_ALLOW_RAW_IMAGE
#define APP_ALLOW_RAW_IMAGE       1

#endif
