/**
* @file arkrtos_board_config.h
* @brief Ark RTOS 板级配置 - QEMU `virt` 机器（RISC-V RV32IMAC，单 hart，M 模式）
* @details 这一份配置同时承担两件事：
*
*          1) **芯片几何**：QEMU virt 把「Flash」也做成 RAM（同一片 DRAM，
*             没有独立的片上 Flash），所以 CHIP_FLASH_* 指的是内核镜像的
*             链接区间，CHIP_RAM_* 指的是静态数据与内核堆的区间。两者必须
*             互不重叠，且都在 QEMU 给的 128 MB DRAM（0x80000000 起）之内。
*             实测几何（由 `-machine virt,dumpdtb` 解析得到）：
*                 DRAM  base 0x80000000  size 128 MB
*                 CLINT base 0x02000000  （msip / mtimecmp / mtime）
*                 UART  16550 @ 0x10000000
*                 timebase-frequency = 10 MHz
*
*          2) **功能裁剪**：QEMU 上没有 Flash 控制器、没有看门狗、没有
*             文件系统介质，也没有第二块板子。凡是需要真实硬件的功能一律
*             关掉，让第一次 bring-up 的编译面尽量小；关掉的功能不会在
*             二进制里留下半截桩，这一点由 features.h 的 #ifndef 保证。
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
 *        这里不能写 ARKRTOS_CPU_CORE_RV32IMAC（那个宏此刻还不存在），
 *        只能写它的字面值 0x0201，注释标明来源。
 * ============================================================ */
#undef  ARKRTOS_ARCH_CORE
#define ARKRTOS_ARCH_CORE           0x0201u   /* = ARKRTOS_CPU_CORE_RV32IMAC */

/* ============================================================
 * 二、芯片几何（QEMU virt / RISC-V）
 * ============================================================ */

/* 内核镜像区间：QEMU 的 ELF 装载器把 PT_LOAD 段直接拷到这里。
 * 1 MB 足够装下内核 + 示例（当前实测镜像约 40 KB）。 */
#define CHIP_FLASH_BASE              0x80000000u
#define CHIP_FLASH_SIZE              (1024 * 1024)

/* 静态数据 / 内核 RAM 池区间：从内核镜像区间之上开始，8 MB。
 * KERNEL_RAM_BASE = CHIP_RAM_BASE + SHARE_RAM_SIZE（见分区头的推导）。 */
#define CHIP_RAM_BASE                0x80100000u
#define CHIP_RAM_SIZE                (8 * 1024 * 1024)

/* 没有 CCM / TCM。填 0 让分区头不派生 CCM 段，链接脚本也就不生成它。 */
#define CHIP_CCM_BASE                0u
#define CHIP_CCM_SIZE                0u

/* ============================================================
 * 三、平台常量（供 port 与板级代码使用）
 * ============================================================ */

/* mtime 的计数频率（QEMU virt 的 timebase-frequency）。节拍换算与
 * arkrtos_port_delay_us 都用它，不能与 CLINT 的实际频率不符。 */
#define ARKRTOS_MACHINE_TIMER_HZ     10000000u

/* CLINT 基址。mtimecmp/mtime 的偏移由 RISC-V 特权规范固定，
 * 只有基址是平台相关的，所以只需要这一个宏。 */
#define ARKRTOS_PORT_CLINT_BASE      0x02000000u

/* 16550 UART（控制台）。 */
#define ARKRTOS_QEMU_UART_BASE       0x10000000u
#define ARKRTOS_QEMU_UART_REG_SHIFT  0u        /* 寄存器按字节寻址，不移位 */

/* 外部中断控制器 PLIC。virt 机器把 PLIC 放在 0x0C000000，
 * 设备树里的中断号：UART0 = 10、UART1 = 11、VIRTIO-MMIO = 1..8。
 * 这两件事都是平台常量（不是我们自己选的），所以放在板级配置里。 */
#define ARKRTOS_PORT_PLIC_BASE       0x0C000000u
#define ARKRTOS_QEMU_UART_IRQ        10u

/* ============================================================
 * 三·B、U 模式任务可用的外设窗（MPU / PMP 用的那一份）
 * @details 只开控制台 UART 的 4 KB 读写。理由：U 模式任务要能**自己**
 *          把读数打出来，否则"保护生效了"这件事只能由 M 模式的任务
 *          转述，证据链就断了一环。
 *
 *          故意**不开** CLINT（0x02000000）：U 模式没有任何正当理由去碰
 *          定时器比较寄存器，而它正好是 example/qemu_riscv32/main.c 里
 *          任务 C 的越权负对照目标（读它 → PMP 拒绝 → mcause=5）。
 * ============================================================ */
#define ARKRTOS_MPU_PERIPH_BASE      0x10000000u
#define ARKRTOS_MPU_PERIPH_SIZE      0x00001000u
#define ARKRTOS_MPU_PERIPH_RW        1

/* ============================================================
 * 四、能力开关
 * @details RV32IMAC 没有 FPU。内存保护与特权分离**已经实现**：
 *          U 模式 + PMP（见 kernelsrc/port/riscv/rv32imac/arkrtos_port.c
 *          的「任务隔离（U 模式 + PMP）」段）。这两个开关必须打开，
 *          否则内核里 is_priv 的判定与逐任务窗口会整段被编译掉，
 *          隔离也就无从谈起。
 *
 *          两个外设窗故意留空（SIZE = 0）：QEMU 上给 U 模式任务开放
 *          CLINT / 16550 没有正当用途（打印走 SVC，由 M 模式的内核代劳），
 *          留空反而让「U 模式去摸外设」成为一条可复现的越权负对照。
 * ============================================================ */
#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU              0
#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU              1
#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV             1

/* 心跳合同：QEMU 没有看门狗，守卫真咬人的前提不存在，直接关掉。 */
#undef  ARKRTOS_USE_KERNEL_GUARD
#define ARKRTOS_USE_KERNEL_GUARD     0
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           0

/* ============================================================
 * 五、功能裁剪（把需要真实硬件 / 需要 libc 格式化的模块全部关掉）
 * ============================================================ */

/* 日志走设备框架 + snprintf，bring-up 阶段没有设备框架，关掉。
 * 关掉后 ARKRTOS_LOGE/W/I/D 整体展开为空语句，格式化代码不进二进制。 */
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
 * 其余几项要么依赖 CMSIS，要么依赖字符串格式化，全部关掉。 */
#undef  ARKRTOS_USE_RTSTAT
#define ARKRTOS_USE_RTSTAT           0
/* 调度器统计（ARKRTOS_USE_SCHED_STAT）默认是 1，但它量的是 DWT CYCCNT：
 * 本 port 的 arkrtos_port_get_cyccnt() 返回 0、cyccnt_init() 返回 -1，
 * 开着只会得到 "每次切换都是 0 周期" 的假读数。所以这里关掉 —— 关掉是
 * "这个计数器在本平台不存在"，不是"统计不重要"。 */
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

/* 单 hart：SMP 关掉（QEMU 能给多 hart，后续做 SMP 验证时再开） */
#undef  ARKRTOS_USE_SMP
#define ARKRTOS_USE_SMP              0

#endif /* __ARKRTOS_BOARD_CONFIG_H__ */
