/**
* @file arkrtos_board_config.h
* @brief Ark RTOS 板级配置 - QEMU `mps2-an386`（Cortex-M4，MPS2 AN386 应用笔记板）
* @details 这一份配置同时承担两件事：
*
*          1) **芯片几何**：QEMU 把 AN386 的两块 SSRAM 直接当内存用，没有
*             独立的片上 Flash 控制器。几何由 QEMU 源码 `hw/arm/mps2.c`
*             （M-profile 分支，9.2.3 实测）核实，不是猜的：
*                 SSRAM1   base 0x00000000  size 0x00400000（4 MB，镜像区）
*                 别名     base 0x00400000  （同一块 SSRAM1 的别名）
*                 SSRAM23  base 0x20000000  size 0x00400000（4 MB，数据区）
*                 UART0    base 0x40004000  （CMSDK APB UART，其余 4 个在
*                                           0x40005000/6000/7000/9000）
*                 SYSCLK   25000000 Hz
*             所以「Flash」指的是镜像链接区间（0x0 起 1 MB），「RAM」指的是
*             静态数据 + 内核堆 + 任务栈（0x20000000 起 4 MB）。
*
*          2) **功能裁剪**：这是 ARM 家族在 QEMU 上的第一个 bring-up 档，
*             只要求三条链路：SysTick 节拍、PendSV 任务切换、串口读数。
*             MPU / 特权分离 / 文件系统 / Shell / 日志 / 软总线全部关掉，
*             让第一次上板失败时的可疑面最小。关掉的功能不会在二进制里留下
*             半截桩，这一点由 features.h 的 #ifndef 保证。
*
* @note 本文件先于 config/arkrtos_partition.h 被包含（见 arkrtos_config.h
*       的加载顺序），所以这里定义的 CHIP_* 就是最终值——分区头里的同名
*       宏都带 #ifndef 守卫。
* @author xw
*/

#ifndef __ARKRTOS_BOARD_CONFIG_H__
#define __ARKRTOS_BOARD_CONFIG_H__

/* 设备头（CMSIS core_cm4.h + 本板外设基址）。汇编侧不需要也不该看到它：
 * Makefile 用 -include 把本文件塞给 .s，C 声明进了汇编会直接报语法错，
 * 所以用 __ASSEMBLER__（gcc 预处理 .s 时自动定义）挡掉。 */
#ifndef __ASSEMBLER__
#include "mps2_an386.h"
#endif

/* ============================================================
 * 一、架构选择
 * @brief 必须在 arkrtos_arch.h 被包含之前定义 ARKRTOS_ARCH_CORE，否则
 *        arch.h 会按未定义的 ARKRTOS_CPU_ARCH 走 #error 分支。
 *        这里不能写 ARKRTOS_CPU_CORE_CORTEX_M4（那个宏此刻还不存在），
 *        只能写它的字面值 0x0102，注释标明来源（见 arkrtos_arch.h:35-43）。
 * ============================================================ */
#undef  ARKRTOS_ARCH_CORE
#define ARKRTOS_ARCH_CORE           0x0102u   /* = ARKRTOS_CPU_CORE_CORTEX_M4 */

/* ============================================================
 * 二、芯片几何（QEMU mps2-an386）
 * ============================================================ */

/* 镜像区：SSRAM1 从 0x00000000 起。ELF 的 PT_LOAD 由 QEMU 的 -kernel
 * 装载器按 p_paddr 拷进内存；M-profile 复位时从地址 0 取初始 SP、从 0x4
 * 取复位 PC，因此向量表必须落在 .text 的最前面（见 example 的 start.S）。
 * 1 MB 对当前镜像（含内核 + 示例）绰绰有余。 */
#define CHIP_FLASH_BASE              0x00000000u
#define CHIP_FLASH_SIZE              (1024 * 1024)

/* 数据区：SSRAM23 从 0x20000000 起 4 MB。 */
#define CHIP_RAM_BASE                0x20000000u
#define CHIP_RAM_SIZE                (4 * 1024 * 1024)

/* 没有 CCM / TCM。填 0 让分区头不派生 CCM 段，链接脚本也就不生成它。 */
#define CHIP_CCM_BASE                0u
#define CHIP_CCM_SIZE                0u

/* ============================================================
 * 三、平台常量
 * ============================================================ */

/* SYSCLK：QEMU hw/arm/mps2.c 里 AN385/AN386 的 SYSCLK_FRQ = 25 MHz。
 * Cortex-M4 移植层用 SystemCoreClock 换算 SysTick 重装值与 us 延时，
 * 板级 arkrtos_board.c 里把这个值定义成 SystemCoreClock。 */
#define ARKRTOS_MACHINE_TIMER_HZ     25000000u

/* CMSDK APB UART0（控制台）。寄存器按 32 位字寻址：DATA 0x00、STATE 0x04、
 * CTRL 0x08、INTSTATUS 0x0C、BAUDDIV 0x10。 */
#define ARKRTOS_MPS2_UART_BASE       0x40004000u

/* 复位：Cortex-M 的 AIRCR.SYSRESETREQ（0xE000ED0C）在该核上就是标准写法，
 * 不需要板级代劳，因此不定义 ARKRTOS_PLATFORM_REBOOT_PORT。 */

/* ============================================================
 * 四、能力开关
 * @details M4 硬件上有 FPU 与 MPU，但这一档：
 *          - FPU 关：本档用 -mfloat-abi=soft 构建，上下文切换里不该出现
 *            S16-S31 的存取（开关与编译选项必须一致，否则切出来的帧对不上）；
 *          - MPU 关：第一次 bring-up 先证明节拍与切换能跑，再逐档打开；
 *          - PRIV 关：SVC 通路保留（任务里仍然走 SVC 进内核），但不切 U 模式。
 * ============================================================ */
#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU              0
#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU              0
#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV             0

/* 心跳合同：QEMU 上没有看门狗，守卫真咬人的前提不存在，关掉。 */
#undef  ARKRTOS_USE_KERNEL_GUARD
#define ARKRTOS_USE_KERNEL_GUARD     0
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           0

/* ============================================================
 * 五、功能裁剪（第一档只留调度 + 节拍 + 串口）
 * ============================================================ */
#undef  ARKRTOS_USE_LOG
#define ARKRTOS_USE_LOG              0
#undef  ARKRTOS_USE_LOG_RING
#define ARKRTOS_USE_LOG_RING         0
#undef  ARKRTOS_USE_BLK
#define ARKRTOS_USE_BLK              0
#undef  ARKRTOS_USE_FS
#define ARKRTOS_USE_FS               0
#undef  ARKRTOS_USE_VFS
#define ARKRTOS_USE_VFS              0
#undef  ARKRTOS_USE_VFS_LFS
#define ARKRTOS_USE_VFS_LFS          0
#undef  ARKRTOS_USE_PROCFS
#define ARKRTOS_USE_PROCFS           0
#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP          0
#undef  ARKRTOS_USE_POSIX
#define ARKRTOS_USE_POSIX            0
#undef  ARKRTOS_USE_LWIP
#define ARKRTOS_USE_LWIP             0
#undef  ARKRTOS_USE_SOFTBUS
#define ARKRTOS_USE_SOFTBUS          0
#undef  ARKRTOS_USE_SHELL
#define ARKRTOS_USE_SHELL            0
#undef  ARKRTOS_USE_APPPACK
#define ARKRTOS_USE_APPPACK          0
#undef  ARKRTOS_USE_APPRUN
#define ARKRTOS_USE_APPRUN           0
#undef  ARKRTOS_USE_FLASHAPP
#define ARKRTOS_USE_FLASHAPP         0
#undef  ARKRTOS_USE_IMAGE_SIGN
#define ARKRTOS_USE_IMAGE_SIGN       0
#undef  ARKRTOS_USE_QSPI
#define ARKRTOS_USE_QSPI             0
#undef  ARKRTOS_AUTOMOUNT_NOR
#define ARKRTOS_AUTOMOUNT_NOR        0

/* 度量 / 自检：本档不开。RTSTAT 依赖周期计数器、SCHED_STAT 量的是
 * DWT CYCCNT（M4 上存在，但第一次 bring-up 先用最少的组件），
 * 其余依赖字符串格式化或真实硬件。 */
#undef  ARKRTOS_USE_RTSTAT
#define ARKRTOS_USE_RTSTAT           0
#undef  ARKRTOS_USE_SCHED_STAT
#define ARKRTOS_USE_SCHED_STAT       0
#undef  ARKRTOS_USE_MDK_TRACE
#define ARKRTOS_USE_MDK_TRACE        0
#undef  ARKRTOS_USE_CM_BACKTRACE
#define ARKRTOS_USE_CM_BACKTRACE     0
#undef  ARKRTOS_USE_CRASH_LOG
#define ARKRTOS_USE_CRASH_LOG        0
#undef  ARKRTOS_USE_AUDIT
#define ARKRTOS_USE_AUDIT            0
#undef  ARKRTOS_USE_UTEST
#define ARKRTOS_USE_UTEST            0
#undef  ARKRTOS_USE_SECCHECK
#define ARKRTOS_USE_SECCHECK         0
#undef  ARKRTOS_HW_SAFE
#define ARKRTOS_HW_SAFE              0

/* SMP：Cortex-M4 单核，恒 0；写成 #ifndef 只为让命令行 -D 不会撞宏重定义。 */
#ifndef ARKRTOS_USE_SMP
#define ARKRTOS_USE_SMP              0
#endif

/* 软定时器服务任务的栈（字）。M4 的 SVC/PendSV 帧比 RV32 小，但 96 字
 * （384 B）在这个档里同样偏紧：切到 256 字 = 1 KB，与 RV32/LA32 同档。 */
#undef  ARKRTOS_TIMER_TASK_STACK_WORDS
#define ARKRTOS_TIMER_TASK_STACK_WORDS (256)

/* 启动/idle 上下文栈守卫：这一档也打开，拿得到 idle 栈的实测余量读数。 */
#undef  ARKRTOS_USE_STACK_GUARD
#define ARKRTOS_USE_STACK_GUARD      1

#endif /* __ARKRTOS_BOARD_CONFIG_H__ */
