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

/* 复位不由内核的 Cortex-M 实现接管：0xE000ED0C 在 RV32 上没有映射，
 * 写它只会得到 store fault。定义这个宏会让平台层改调板级的
 * arkrtos_board_reboot()（sifive_test 写 0x7777）。 */
#define ARKRTOS_PLATFORM_REBOOT_PORT 1

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
 * 三·C、可执行 RAM 窗口（ELF32 装载，可选档）
 * @details 只在 `-DARKRTOS_USE_ELF=1` 时被用到（默认档 ARKRTOS_USE_ELF=0，
 *          本段宏只是"如果打开，窗口在哪"的答案，不产生任何代码）。
 *
 *          位置：DRAM 白地。0x80000000 起是内核镜像区（1 MB），
 *          0x80100000 起是内核 RAM（8 MB，到 0x80900000 为止），
 *          QEMU virt 的 DRAM 到 0x88000000，所以 0x80A00000 起 1 MB
 *          既在 DRAM 内、又不与任何分区重叠。1 MB 对齐是硬约束：
 *          PMP 区域必须是 2 的幂且按自身对齐（mpu.c 里有编译期闸门）。
 *
 *          程序与它的栈、argv 区都在这块窗口里（栈从窗口顶往下切），
 *          所以 U 模式任务的两个 PMP 区域（代码 RX / 数据 RW）都指向它。
 * ============================================================ */
/* 窗口基址/大小本身总是给出（文档与工具对账要用）。但**给 MPU 的那两个
 * 别名只在开 ELF 档时定义**：mpu.c 用 `#if defined(...)` 判断要不要编进
 * ELF 窗口分支，默认档若也定义这两个宏，分支就会被编进默认镜像（实测体积
 * +564 B），违反"默认档零变化"。这里用 ARKRTOS_USE_ELF 把关 —— 该宏在
 * 功能档里来自命令行 -D（先于 -include 生效），默认档由 autoconf 给 0，
 * 此刻尚未可见，"未定义"与"0"都走同一个分支。 */
#ifndef ARKRTOS_ELF_WINDOW_BASE
#define ARKRTOS_ELF_WINDOW_BASE      0x80A00000u
#endif
#ifndef ARKRTOS_ELF_WINDOW_SIZE
#define ARKRTOS_ELF_WINDOW_SIZE      (1024u * 1024u)
#endif
#if defined(ARKRTOS_USE_ELF) && (ARKRTOS_USE_ELF == 1)
#define ARKRTOS_MPU_ELF_WINDOW_BASE  ARKRTOS_ELF_WINDOW_BASE
#define ARKRTOS_MPU_ELF_WINDOW_SIZE  ARKRTOS_ELF_WINDOW_SIZE
#endif

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
#define ARKRTOS_USE_KERNEL_GUARD     1
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           0

/* ============================================================
 * 五、功能裁剪（把需要真实硬件 / 需要 libc 格式化的模块全部关掉）
 * ============================================================ */

/* 日志走设备框架（SHELL_DEV_NAME）+ snprintf。设备框架在本板已经起来
 * （drvuart.c 把 16550 注册成 ttyS0），格式化由 example/qemu_riscv32/
 * libc_shim.c 的 vsnprintf 提供，所以这一层可以真开而不是留桩。
 * LOG_RING 保留最近 4 KB 日志行，供 shell 的 dmesg 回看。 */
#undef  ARKRTOS_USE_LOG
#define ARKRTOS_USE_LOG              1
#undef  ARKRTOS_USE_LOG_RING
#define ARKRTOS_USE_LOG_RING         1

/* 存储栈：QEMU virt 没有 Flash 控制器、没有 SPI NOR，也没有 eMMC。
 * 但文件系统 / VFS / 安装链路属于「全功能」里必须验证的那一层，缺了它
 * 只能证明调度器会跑。所以本板带一块 RAM 模拟的 NOR 语义块设备 ram0
 * （见 board/qemu_riscv32/drvblk.c：擦除单位 4 KB、擦完全 0xFF、写只能
 * 落在已擦除字节），把 blk 以上的整条软件栈真的跑起来。
 * 诚实边界：**介质本身**（编程时序、忙等待、掉电中间态）没有验证，
 * 那是 drvnor.c / drvflash.c 的事，这里一行都用不上。 */
#undef  ARKRTOS_USE_BLK
#define ARKRTOS_USE_BLK              1
#undef  ARKRTOS_USE_FS
#define ARKRTOS_USE_FS               1
#undef  ARKRTOS_USE_VFS
#define ARKRTOS_USE_VFS              1
#undef  ARKRTOS_USE_VFS_LFS
#define ARKRTOS_USE_VFS_LFS          1
#undef  ARKRTOS_USE_PROCFS
#define ARKRTOS_USE_PROCFS           1

/* 动态装载 / MiniApp / POSIX / 网络：都不是这次要验证的东西 */
#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP          0
#undef  ARKRTOS_USE_POSIX
#define ARKRTOS_USE_POSIX            0
#undef  ARKRTOS_USE_LWIP
#define ARKRTOS_USE_LWIP             0
#undef  ARKRTOS_USE_SOFTBUS
#define ARKRTOS_USE_SOFTBUS          1
/* 内核 Shell 控制台：任务优先级/栈/周期都取 config/arkrtos_partition.h 的
 * SHELL_TASK_*（14 / 3 KB / 2 ms），设备名取 SHELL_DEV_NAME（ttyS0），
 * 打开动作在 arkrtos_shell_task 里由 platform_uart_init() 完成 —— 因此
 * 板级必须先把这个名字注册进设备框架（board/qemu_riscv32/drvuart.c）。 */
#undef  ARKRTOS_USE_SHELL
#define ARKRTOS_USE_SHELL            1
#undef  ARKRTOS_USE_APPPACK
#define ARKRTOS_USE_APPPACK          0
#undef  ARKRTOS_USE_APPRUN
#define ARKRTOS_USE_APPRUN           0
#undef  ARKRTOS_USE_FLASHAPP
#define ARKRTOS_USE_FLASHAPP         0
#undef  ARKRTOS_USE_IMAGE_SIGN
#define ARKRTOS_USE_IMAGE_SIGN       1
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
#define ARKRTOS_USE_MDK_TRACE        1
#undef  ARKRTOS_USE_CM_BACKTRACE
#define ARKRTOS_USE_CM_BACKTRACE     0
#undef  ARKRTOS_USE_CRASH_LOG
#define ARKRTOS_USE_CRASH_LOG        1
#undef  ARKRTOS_USE_AUDIT
#define ARKRTOS_USE_AUDIT            1
#undef  ARKRTOS_USE_UTEST
#define ARKRTOS_USE_UTEST            1
#undef  ARKRTOS_USE_SECCHECK
#define ARKRTOS_USE_SECCHECK         1
#undef  ARKRTOS_HW_SAFE
#define ARKRTOS_HW_SAFE              1

/* 单 hart：SMP 关掉（QEMU 能给多 hart，后续做 SMP 验证时再开） */
#undef  ARKRTOS_USE_SMP
#define ARKRTOS_USE_SMP              0

/* 软定时器服务任务的栈（字）。默认 96 字 = 384 B 在 RV32 上实测不够：
 * 2026-11-17 QEMU 实测（`stack raw`，fill 扫描取大值），跑过
 * `utest run` / `sec all` / `audit` / `dmesg` 之后 timer daemon 用到
 * 372/384 B = 96%，只剩 12 B。本 port 的陷阱帧压在被中断任务的栈上
 * （一帧 128 B），任何一次中断正好切到它就踩出去。
 * 给 256 字 = 1 KB（与 board/la32_generic 同值），改后同口径复测。 */
#undef  ARKRTOS_TIMER_TASK_STACK_WORDS
#define ARKRTOS_TIMER_TASK_STACK_WORDS (256)

/* 启动/idle 上下文栈守卫：与 qemu_loongarch64 同档打开，否则这一档就拿不到
 * 实测余量读数（见 docs/QEMU模拟器移植与验证.md 5.13.4）。 */
#undef  ARKRTOS_USE_STACK_GUARD
#define ARKRTOS_USE_STACK_GUARD      1


#endif /* __ARKRTOS_BOARD_CONFIG_H__ */
