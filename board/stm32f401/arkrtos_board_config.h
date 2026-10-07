/**
* @brief Ark RTOS 板级配置 - STM32F401
* @details 覆盖内核默认配置，适配 STM32F401 硬件特性
*          F401：Cortex-M4F / 最高 84MHz / 96KB SRAM / 无 CCM。
*          本板控制台 = USART2（PA2/PA3），LED = PC13。
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

#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU             0

#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV            0

/* MiniApp (小程序). Loading one needs a 4 KB static relocation-table
 * buffer in the kernel image (the whole table is validated in one piece),
 * and this board has the least kernel RAM of the family: 96 KB SRAM minus
 * 8 KB shared minus 32 KB slot pool leaves 56 KB. With MiniApp compiled in
 * the linker ran out of RAM (L6406E on arkrtos_mini.o(.bss)); the feature
 * switch exists for exactly this situation, so it is off here and on for
 * F427. Nothing else in the kernel depends on it. */
#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP         0

/* Independent watchdog (IWDG). Hardware fact, so it lives with the board:
 * 1 = the kernel guard owns the counter and stops feeding when an invariant
 *     is violated (see arkrtos_guard.c);
 * 0 = no watchdog; nothing resets the board from inside the kernel.
 * Bring-up order matters: the IWDG cannot be stopped once started, so this
 * is enabled only after the feed path has been verified with a long timeout. */
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE          0

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
