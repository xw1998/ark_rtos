/**
* @brief Ark RTOS 板级配置 - STM32H743 (FK743M5-XIH6)
* @details 覆盖内核默认配置，适配 STM32H743XIH6 硬件特性。
*          通过编译选项 -DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\" 包含。
*          本文件同时包含 CMSIS 设备头文件，供 port 层使用寄存器定义。
*/

#ifndef __ARKRTOS_BOARD_CONFIG_H__
#define __ARKRTOS_BOARD_CONFIG_H__

#include "stm32h7xx.h"

#undef  ARKRTOS_CPU_ARCH
#define ARKRTOS_CPU_ARCH            ARKRTOS_ARCH_CORTEX_M7

#define ARKRTOS_SHARE_MEM_SIZE      (0x8000)

#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU             1

/* Memory protection (Cortex-M7 MPU is the same ARMv7-M architected unit as on
 * the M4: 8 regions used here, AP/XN encoding identical). An App task runs
 * unprivileged and can only reach kernel services through an SVC; drivers stay
 * privileged on purpose because they program peripheral registers directly. */
#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU             1

#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV            1

/* Independent watchdog. Kept OFF for the first H743 bring-up: the IWDG cannot
 * be stopped once started, and a wrong LSI/timeout assumption would reset the
 * board before the console is up. Turn it on after the shell is verified. */
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE          0

#undef  ARKRTOS_WDG_TIMEOUT_MS
#define ARKRTOS_WDG_TIMEOUT_MS      4000

/* Development bypass: allow a raw image burned directly at a pool slot base
 * to be recognised and run (fixed-address flash + debugger workflow).
 * Release firmware must set this to 0 and accept only CRC-checked .svcapp. */
#undef  APP_ALLOW_RAW_IMAGE
#define APP_ALLOW_RAW_IMAGE         1

#endif
