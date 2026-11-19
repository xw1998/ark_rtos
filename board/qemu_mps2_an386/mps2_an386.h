/**
* @file mps2_an386.h
* @brief QEMU `mps2-an386` 的最小设备头（只为拉起 CMSIS core_cm4.h）
* @details STM32 工程靠厂商的 stm32f4xx.h 提供 __CM4_REV / __NVIC_PRIO_BITS
*          等 CMSIS 必需的设备常量；QEMU 的 MPS2 板没有厂商头，所以这里
*          把 core_cm4.h 需要的那几个常量直接给出，再包含它。
*
*          取值依据（QEMU hw/arm/mps2.c，9.2.3 实测）：
*            - AN385/AN386 的 armv7m "num-irq" = 32；QEMU 的 ARMv7M 默认
*              优先级位宽为 3（8 级），与 ARM 的 AN386 参考设计一致；
*            - Cortex-M4 有 MPU（__MPU_PRESENT = 1，本档未用）；
*            - 本档以 -mfloat-abi=soft 构建，因此 __FPU_PRESENT = 0，
*              与板级配置里的 ARKRTOS_USE_FPU = 0 是同一件事的两面。
*
* @note 这个头只在 C 编译单元里被包含（板级配置里用 __ASSEMBLER__ 挡掉了
*       汇编侧），所以不会把 CMSIS 的 C 声明塞进 .S 的预处理结果。
* @author xw
*/

#ifndef __MPS2_AN386_H__
#define __MPS2_AN386_H__

/* ---- CMSIS 设备常量（core_cm4.h 要求先有这几项） ---- */
#define __CM4_REV                 0x0001U   /* r0p1（QEMU 的 cortex-m4） */
#define __MPU_PRESENT             1U
#define __NVIC_PRIO_BITS          3U        /* 8 个优先级 */
#define __Vendor_SysTickConfig    0U        /* 用 CMSIS 的标准 SysTick 定义 */
#define __FPU_PRESENT             0U        /* 与 -mfloat-abi=soft 一致 */

/* CMSIS 的 core_cm4.h 假定设备头已经给出 IRQn_Type（STM32 的厂商头里有）。
 * QEMU 的 MPS2 板没有厂商头，所以这里按 ARMv7-M 体系结构定义的核内异常号
 * 给出这一份：负号是内核异常，0 起是外设中断（本档用不到外设中断）。 */
typedef enum
{
    NonMaskableInt_IRQn   = -14,
    HardFault_IRQn        = -13,
    MemoryManagement_IRQn = -12,
    BusFault_IRQn         = -11,
    UsageFault_IRQn       = -10,
    SVCall_IRQn           =  -5,
    DebugMonitor_IRQn     =  -4,
    PendSV_IRQn           =  -2,
    SysTick_IRQn          =  -1
} IRQn_Type;

#include "core_cm4.h"

/* 内核移植层用的是 __weak（STM32 的 HAL 头原本提供这个别名），
 * 这份 CMSIS 只给 __WEAK，所以在这里补一个同义定义。 */
#ifndef __weak
#define __weak __attribute__((weak))
#endif

/* CMSIS 5.x 把 SystemCoreClock 的声明从 core_cm4.h 移到了厂商的 system_<dev>.h。
 * 本板没有厂商启动文件，声明与定义都放在板级（定义在 arkrtos_board.c）。 */
extern uint32_t SystemCoreClock;

/* ---- 本板用到的外设基址（与 QEMU hw/arm/mps2.c 对账） ---- */
#define MPS2_SSRAM1_BASE          0x00000000u   /* 镜像区 4 MB            */
#define MPS2_SSRAM23_BASE         0x20000000u   /* 数据区 4 MB            */
#define MPS2_UART0_BASE           0x40004000u   /* CMSDK APB UART0        */
#define MPS2_SYSCLK_HZ            25000000u     /* SYSCLK_FRQ             */

#endif /* __MPS2_AN386_H__ */
