/**
* @file arkrtos_board_config.h
* @brief Ark RTOS 板级配置 - 通用 LoongArch32 参考目标（单核，PLV0）
* @details 这一份配置同时承担两件事：
*
*          1) **芯片几何**：沿用 LoongArch virt 的约定（DRAM 0x0 起 128 MB、
*             低 1 MB 归 boot_info、内核镜像从 2 MB 起、静态区从 3 MB 起）。
*             这些数字在 la64 上是实测标定过的，**在 LA32 上没有任何实测**：
*                 DRAM   base 0x00000000  size 128 MB（低 1 MB 归 boot_info）
*                 UART   ns16550a @ 0x1FE001E0
*                 EENTRY 异常入口（CSR 0xC），无独立向量表基址
*                 rdtime 100 MHz / 定时器 TCFG 计数 25 MHz（la64 实测值）
*
*          2) **功能裁剪**：这个参考目标上没有 Flash 控制器、没有看门狗、没有
*             文件系统介质。凡是需要真实硬件的功能一律关掉，让第一次
*             bring-up 的编译面尽量小。
*
*          ## 验证状态（必读）
*
*          **没有任何 LA32 机器跑过这份配置。** QEMU 9.2.3 只提供
*          `qemu-system-loongarch64`，上游没有 loongarch32 系统模拟器；环境里
*          也没有 LA32 板子。因此本目标只验证到「编译 + 链接产出 ELF」，
*          所有时序/外设常量都是 la64 的实测值搬过来的，**不得当成 LA32 的
*          实测结论**。
*
* @note 本文件先于 config/arkrtos_partition.h 被包含（见 arkrtos_config.h
*       的加载顺序），所以这里定义的 CHIP_* 就是最终值——分区头里的同名
*       宏都带 #ifndef 守卫。
* @author xw
*/

#ifndef __ARKRTOS_BOARD_CONFIG_H__
#define __ARKRTOS_BOARD_CONFIG_H__

/* ============================================================
 * 一、架构选择
 * @brief 必须在 arkrtos_arch.h 被包含之前定义 ARKRTOS_ARCH_CORE，否则
 *        arch.h 会按未定义的 ARKRTOS_CPU_ARCH 走 #error 分支。
 *        这里不能写 ARKRTOS_CPU_CORE_LA64（那个宏此刻还不存在），
 *        只能写它的字面值 0x0302，注释标明来源。
 * ============================================================ */
#undef  ARKRTOS_ARCH_CORE
#define ARKRTOS_ARCH_CORE           0x0301u   /* = ARKRTOS_CPU_CORE_LA32 */

/* ============================================================
 * 二、芯片几何（参考映射：LoongArch virt 约定 / LoongArch32）
 * ============================================================ */

/* 内核镜像区间：ELF 装载器把 PT_LOAD 段直接拷到这里。
 * 基址 2 MB 沿用 la64 virt 的要求（低 1 MB 是 boot_info）。本目标当前没有
 * 可运行的装载器，这个值只是与 la64 保持同一套几何，便于将来接真实板。 */
#define CHIP_FLASH_BASE              0x00200000u
#define CHIP_FLASH_SIZE              (1024 * 1024)

/* 静态数据 / 内核 RAM 池区间：从内核镜像区间之上开始，8 MB。
 * KERNEL_RAM_BASE = CHIP_RAM_BASE + SHARE_RAM_SIZE（见分区头的推导）。 */
#define CHIP_RAM_BASE                0x00300000u
#define CHIP_RAM_SIZE                (8 * 1024 * 1024)

/* 没有 CCM / TCM。填 0 让分区头不派生 CCM 段，链接脚本也就不生成它。 */
#define CHIP_CCM_BASE                0u
#define CHIP_CCM_SIZE                0u

/* ============================================================
 * 三、平台常量（供 port 与板级代码使用）
 * ============================================================ */

/* TCFG 定时器的计数时钟。实测标定：InitVal = 2,000,000 时 10 s 内进入
 * 125 次中断（80 ms/次），折合 25 MHz；InitVal = 1,000,000 测得 245 次，
 * 略低于线性预期，以 2M 那组为准。 */
#define ARKRTOS_PORT_TIMER_HZ        25000000u

/* rdtime 的计数时钟。实测：rdtime.d 走 1e9 ticks 用掉约 10.0 s 主机时间
 * （含启动约 10.19 s），折合 100 MHz。delay_us 与 get_timer_counter 用它。 */
#define ARKRTOS_PORT_RDTIME_HZ       100000000u

/* ns16550a（控制台）。寄存器按字节寻址，与 16550 布局一致。 */
#define ARKRTOS_QEMU_UART_BASE       0x1FE001E0u
#define ARKRTOS_QEMU_UART_REG_SHIFT  0u

/* ============================================================
 * 四、能力开关
 * @details LA64 硬件有 FPU，但这一版没有启用 FPU 上下文保存（任务切换不存
 *          FPR/ FCCR），所以 FPU 开关必须关掉：打开却没有上下文管理，
 *          多任务共用浮点寄存器会静默算错。
 *          MPU / 特权分离同样关掉——本移植只跑 PLV0，没有页表或地址窗口
*           隔离，关掉不等于"做不到"，只是这一版没有搭起来。
 * ============================================================ */
#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU              0
#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU              0
#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV             0

/* 心跳合同：QEMU 没有看门狗，守卫真咬人的前提不存在，直接关掉。 */
#undef  ARKRTOS_USE_KERNEL_GUARD
#define ARKRTOS_USE_KERNEL_GUARD     0
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           0

/* ============================================================
 * 五、功能裁剪（把需要真实硬件 / 需要 libc 格式化的模块全部关掉）
 * ============================================================ */

#undef  ARKRTOS_USE_LOG
#define ARKRTOS_USE_LOG              0
#undef  ARKRTOS_USE_LOG_RING
#define ARKRTOS_USE_LOG_RING         0

/* 存储栈：QEMU 上没有 NOR / eMMC，也没有镜像池可写 */
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

/* 动态装载 / MiniApp / POSIX / 网络：都不是这次要验证的东西 */
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

/* 调试 / 度量 / 自检：RTSTAT 需要周期计数器（本 port 明确返回 -1），
 * SCHED_STAT 量的是 DWT CYCCNT（本 port 明确返回 0），开着只会得到
 * "每次切换都是 0 周期" 的假读数，所以一并关掉。 */
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

/* 单核：SMP 关掉（QEMU 能给多核，后续做 SMP 验证时再开） */
#undef  ARKRTOS_USE_SMP
#define ARKRTOS_USE_SMP              0

#endif /* __ARKRTOS_BOARD_CONFIG_H__ */
