/*
 * @file arkrtos_board_config.h
 * @brief Ark RTOS 板级配置 - QEMU virt（LoongArch64，MMU 档）
 *
 * 本文件由 tools/kconfig/genboard.py 生成，不要手改 —— 改了下次
 * --check 就会判过期。要改一项配置，改下面这三个来源之一：
 *
 *   board symbols   config/kconfig/board/Kconfig.qemu_loongarch64
 *   values          config/qemu_loongarch64_defconfig
 *   feature values  顶层 Kconfig（config/kconfig/Kconfig.*）
 *
 * 重新生成：
 *   py -3 tools/kconfig/genboard.py --board qemu_loongarch64
 * 校验：
 *   py -3 tools/kconfig/genboard.py --board qemu_loongarch64 --check
 *   （tools/ci_gate.py 的 board-config-generated 项做的就是这一句）
 *
 * 每一段的长说明写在 Kconfig 的 help 里：menuconfig 里看得见，这里给
 * 简版。MCU + MDK 档不走这套生成器，保留手写头，见
 * docs/Kconfig配置系统.md §9。
 *
 * @note 本文件先于 config/arkrtos_partition.h 被包含（见 arkrtos_config.h
 *       的加载顺序），所以这里定义的 CHIP_* 就是最终值 —— 分区头里的
 *       同名宏都带 #ifndef 守卫。
 * @author xw（生成器；版式由 tools/kconfig/genboard.py 决定）
 */

#ifndef __ARKRTOS_BOARD_CONFIG_H__
#define __ARKRTOS_BOARD_CONFIG_H__

/* ============================================================
 * 一、架构选择
 * 必须在 arkrtos_arch.h 被包含之前定义 ARKRTOS_ARCH_CORE，否则 arch.h
 * 会按未定义的 ARKRTOS_CPU_ARCH 走 #error 分支。这里写的是
 * ARKRTOS_CPU_CORE_LA64 的字面值，因为此刻那个宏还不存在。
 * ============================================================ */
#undef  ARKRTOS_ARCH_CORE
#define ARKRTOS_ARCH_CORE            0x00000302u

/* ============================================================
 * 二、芯片几何（QEMU virt / LoongArch64）
 * 内核镜像区间：QEMU 的 ELF 装载器把 PT_LOAD 段直接拷到这里。基址 2 MB
 * 是硬要求：低 1 MB 被 boot_info 占着。
 *
 * 静态数据 / 内核 RAM 池区间从镜像区间之上开始。
 * KERNEL_RAM_BASE = CHIP_RAM_BASE + SHARE_RAM_SIZE（见分区头的推导）。
 *
 * 没有 CCM / TCM。填 0 让分区头不派生 CCM 段，链接脚本也就不生成它。
 *
 * KERNEL_SIZE 是内核镜像在 Flash 区里占的长度，配置区与镜像池紧跟在
 * 它后面（分区头里 IMAGE_POOL_BASE = CHIP_FLASH_BASE + KERNEL_SIZE +
 * CONFIG_SIZE 就是这么推的）。这条必须够大：板级每次复位会把镜像池
 * 擦成 0xFF，镜像一旦越界，擦除就是在抹自己的代码。
 * ============================================================ */
#define CHIP_FLASH_BASE              0x00200000u
#define CHIP_FLASH_SIZE              (2 * 1024 * 1024)
#ifndef KERNEL_SIZE
#define KERNEL_SIZE                  (512 * 1024)
#endif
#define CHIP_RAM_BASE                0x00400000u
#define CHIP_RAM_SIZE                (8 * 1024 * 1024)
#define CHIP_CCM_BASE                0u
#define CHIP_CCM_SIZE                0u

/* ============================================================
 * 三、平台常量（供 port 与板级代码使用）
 * TCFG 定时器与 rdtime 的计数时钟都由实测标定，标定过程写在 Kconfig 的
 * help 里：InitVal = 2,000,000 时 10 s 内 125 次中断（80 ms/次），
 * rdtime.d 走 1e9 ticks 约 10.0 s。改这两个数之前先重做那次标定。
 *
 * ns16550a 寄存器按字节寻址，与 16550 布局一致。
 * ============================================================ */
#define ARKRTOS_PORT_TIMER_HZ        25000000u
#define ARKRTOS_PORT_RDTIME_HZ       100000000u
#define ARKRTOS_QEMU_UART_BASE       0x1FE001E0u
#ifndef ARKRTOS_QEMU_UART_CLK
#define ARKRTOS_QEMU_UART_CLK        25000000u
#endif
#define ARKRTOS_QEMU_UART_REG_SHIFT  0u

/* ============================================================
 * 四、分页与用户态窗口
 * PLV3 探针页（App 的代码/数据/栈）。DRAM 128 MB，内核占
 * 0x00200000~0x00C00000（镜像 2 MB + RAM 8 MB），16 MB 以上是空地。
 * 取 0x02000000 同时满足 PS=20 条目要求的 2^(20+1) = 2 MB 对齐。
 *
 * App 的 UART 是同页 VA 别名：一页 4 KB 的 PLV3 条目把本页内 0x1E0
 * 偏移映射到 PA 0x1FE001E0。取 0x02202000 而不是 0x02200000 的原因：
 * STLB 组号是 (VA >> 13) & 0xFF，0x02200000 与内核 UART 0x1FE00000
 * 是同一组，两条 4 KB 条目挤在一组里靠随机路号分家（1/8 概率互相
 * 覆盖）。挪一页就换到另一组。
 *
 * 每 App 地址空间的帧数（U-1）：space n 的物理帧 = APP_BASE + n * APP_SIZE。
 * 取 2 只为在 QEMU 上把「同一 VA 映到不同 PA」跑出读数；帧是固定数组，
 * 没有页帧分配器（欠账 U-3）。
 *
 * 4 KB 页帧池（U-3）：避开 App 区、内核区与 pmap test 的 0x03000000 测试
 * VA；8 MB 对齐（PS=22 条目要求 2^(22+1) = 8 MB），port 给它单独补一条
 * 恒等映射；2048 帧正好是 ARKRTOS_USE_PAGING_FRAMES 的编译期上限。
 * ============================================================ */
#define ARKRTOS_QEMU_APP_BASE        0x02000000u
#define ARKRTOS_QEMU_APP_SIZE        (2u * 1024u * 1024u)
#define ARKRTOS_QEMU_APP_UART        0x022021E0u
#define ARKRTOS_PAGING_APPS_MAX      2u
#define ARKRTOS_QEMU_FRAME_BASE      0x04000000u
#define ARKRTOS_QEMU_FRAME_COUNT     2048u
#undef  ARKRTOS_USE_PAGING_FRAMES
#define ARKRTOS_USE_PAGING_FRAMES    1
#undef  ARKRTOS_USE_PAGING_PAGES
#define ARKRTOS_USE_PAGING_PAGES     1
#ifndef ARKRTOS_USE_PAGING_DEMAND
#define ARKRTOS_USE_PAGING_DEMAND    0
#endif
#ifndef ARKRTOS_USE_PAGING_ASID
#define ARKRTOS_USE_PAGING_ASID      0
#endif

/* ============================================================
 * 五、能力开关
 * LA64 硬件有 FPU。打开后做两件事：启动时（start.S）置 EUEN.FPE=1（不开就是
 * PLV3 第一条 FP 指令落 ESTAT.Ecode=15 FPD），以及把 trap 帧 256 → 528
 * 字节、按任务保存 $f0..$f31（eentry / 就地切换 / resume_task 三处统一
 * 存取）。FCSR 与 LSX/LASX 仍不保存，理由见 Kconfig 的 help。
 *
 * 特权分离打开：port 里建立静态 TLB 映射并打开 CRMD.PG，内核页 PLV0、
 * App 页 PLV3。MPU 仍然关掉 —— LoongArch 没有 MPU。
 *
 * 隔离后端选「页表」（MMU）而不是「窗口」：打开后 arkrtos_mpu_* 这套
 * 契约由 arkrtos_paging.c 实现，五个受限窗口变成「静态映射 + PLV」+
 * 装载期跨度审计。与 ARKRTOS_USE_MPU=1 互斥，arkrtos_config.h 里以
 * #error 拦下。
 * ============================================================ */
#undef  ARKRTOS_USE_FPU
#define ARKRTOS_USE_FPU              1
#undef  ARKRTOS_USE_MPU
#define ARKRTOS_USE_MPU              0
#undef  ARKRTOS_USE_PAGING
#define ARKRTOS_USE_PAGING           1
#undef  ARKRTOS_USE_PRIV
#define ARKRTOS_USE_PRIV             1

/* ============================================================
 * 六、可执行文件
 * ELF 装载器：打开后 shell 能直接把 ELF64 可执行文件当命令跑
 * （exec /bin/hello world）。窗口就用 PLV3 探针那 2 MB（硬件映射已由
 * port 建好，装载器只往里拷），所以两个宏直接指向 APP_BASE/SIZE。
 * M1 是单实例 + 共享地址空间；每进程页表是 M2 的欠账。
 *
 * Linux 系统调用 ABI：这块板有 MMU、有 PLV3 用户态，是兼容层的第一个
 * 载体。打开后 ELF 程序可以用 syscall 0 + $a7 的 Linux 约定发请求。
 *
 * U-8：本板打开退出码通道（hello.elf 用 argc 当退出码，exec 打印的读数
 * 与程序自己打印的 argc 互为对证）。
 *
 * VxWorks 式可下载模块与上面的 ELF 装载器并列，不是替代关系。模块区是
 * 内核 .bss 里的一块静态缓冲，不用 heap；地址由链接器决定。代价写清楚：
 * 模块跑 PLV0，等于放弃 PLV3 隔离。
 *
 * 动态链接（PT_INTERP）的解释器区：2 MB 窗口从中间劈开——
 *   主程序  [APP_BASE + 0x000000, APP_BASE + 0x100000)  1 MB
 *   解释器  [APP_BASE + 0x100000, APP_BASE + 0x110000)  64 KB
 *   堆/库   [APP_BASE + 0x110000, APP_BASE + 0x1E0000)
 *   栈      [.., APP_BASE + 0x1F4000)
 *   argv/auxv 区 [APP_BASE + 0x1F4000, APP_BASE + 0x200000)
 * 解释器区与主程序区不重叠，装载器对两者都做区间判据；这两个宏只在
 * ARKRTOS_USE_DYNLOAD=1 时才被读（关档零变化）。
 * ============================================================ */
#undef  ARKRTOS_USE_ELF
#define ARKRTOS_USE_ELF              1
#undef  ARKRTOS_USE_LINUX_ABI
#define ARKRTOS_USE_LINUX_ABI        1
#ifndef ARKRTOS_LINUX_ABI_SYSSTAT
#define ARKRTOS_LINUX_ABI_SYSSTAT    0
#endif
#ifndef ARKRTOS_USE_ELF_TLS
#define ARKRTOS_USE_ELF_TLS          0
#endif
#undef  ARKRTOS_USE_EXIT_CODE
#define ARKRTOS_USE_EXIT_CODE        1
#ifndef ARKRTOS_ELF_WINDOW_BASE
#define ARKRTOS_ELF_WINDOW_BASE      ARKRTOS_QEMU_APP_BASE
#endif
#ifndef ARKRTOS_ELF_WINDOW_SIZE
#define ARKRTOS_ELF_WINDOW_SIZE      ARKRTOS_QEMU_APP_SIZE
#endif
#ifndef ARKRTOS_ELF_INTERP_BASE
#define ARKRTOS_ELF_INTERP_BASE      (ARKRTOS_ELF_WINDOW_BASE + 0x00100000u)
#endif
#ifndef ARKRTOS_ELF_INTERP_SIZE
#define ARKRTOS_ELF_INTERP_SIZE      (64u * 1024u)
#endif
#undef  ARKRTOS_USE_MODLOAD
#define ARKRTOS_USE_MODLOAD          1
#define ARKRTOS_MOD_AREA_SIZE        (64u * 1024u)
#define ARKRTOS_PLATFORM_REBOOT_PORT 1

/* ============================================================
 * 七、设备树（MMU 档）
 * MMU 档按正常项目的做法走：镜像里放一块标准 .dtb，启动时解析——
 * 不是 MCU 档那张 const C 平表。两者来自同一份 dts/qemu_loongarch64.dts，
 * 由 dtc_lite.py 的两个出口分别产出，所以不会互相矛盾。
 *
 * 打开后内核多一个 FDT 解析器与 fdt 命令（只读：不改任何结构、不碰
 * 硬件）。关掉时 arkrtos_fdt.c 不产生任何符号，镜像与命令表不变——
 * 这正是 MCU 档的默认状态。
 * ============================================================ */
#undef  ARKRTOS_USE_FDT
#define ARKRTOS_USE_FDT              1
#undef  ARKRTOS_USE_FDT_CONSOLE
#define ARKRTOS_USE_FDT_CONSOLE      1
#ifndef ARKRTOS_USE_FDT_RES
#define ARKRTOS_USE_FDT_RES          0
#endif
#undef  ARKRTOS_USE_FDT_OVERLAY
#define ARKRTOS_USE_FDT_OVERLAY      1

/* ============================================================
 * 八、可靠性
 * 本板没有 Cortex-M 的 AIRCR，复位由板级 arkrtos_board_reboot() 接管
 * （loongarch virt 也没有复位设备，那里只能明确报「做不到」）。
 *
 * 心跳合同：QEMU 没有看门狗，守卫真咬人的前提不存在，直接关掉。
 * ============================================================ */
#undef  ARKRTOS_USE_KERNEL_GUARD
#define ARKRTOS_USE_KERNEL_GUARD     1
#undef  ARKRTOS_WDG_ENABLE
#define ARKRTOS_WDG_ENABLE           0

/* ============================================================
 * 九、功能裁剪（把需要真实硬件 / 需要 libc 格式化的模块全部关掉）
 * 日志：控制台已经有一条可用的输出通路（ns16550a 轮询），日志走它。
 * LOG_RING 打开后 log show / 崩溃转储能从环形缓冲里翻历史。
 *
 * 存储栈：QEMU 上没有真实 NOR / eMMC，但可以用板级 RAM 介质把 blk 以上
 * 的整条软件栈（blk -> littlefs -> VFS -> procfs）真的跑起来。诚实边界：
 * 介质本身（编程时序、忙等待、掉电中间态）没有验证，那是 drvnor.c /
 * drvflash.c 的事。
 * ============================================================ */
#undef  ARKRTOS_USE_LOG
#define ARKRTOS_USE_LOG              1
#undef  ARKRTOS_USE_LOG_RING
#define ARKRTOS_USE_LOG_RING         1
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
#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP          0
#undef  ARKRTOS_USE_POSIX
#define ARKRTOS_USE_POSIX            0
#undef  ARKRTOS_USE_LWIP
#define ARKRTOS_USE_LWIP             0
#undef  ARKRTOS_USE_SOFTBUS
#define ARKRTOS_USE_SOFTBUS          1
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
#undef  ARKRTOS_USE_EXEC_SIGN
#define ARKRTOS_USE_EXEC_SIGN        1
#undef  ARKRTOS_USE_STACK_GUARD
#define ARKRTOS_USE_STACK_GUARD      1
#define ARKRTOS_SHELL_STACK_SIZE     (1024 * 8)
#undef  ARKRTOS_USE_QSPI
#define ARKRTOS_USE_QSPI             0
#undef  ARKRTOS_AUTOMOUNT_NOR
#define ARKRTOS_AUTOMOUNT_NOR        0

/* ============================================================
 * 十、读数与可观测性
 * RTSTAT 与 SCHED_STAT 默认关（关着时体积与行为零变化），但写成
 * #ifndef 而不是 #undef + #define 0：本板有周期级计数器
 * （rdtime / csrrd 0x40，100 MHz 板级常量，见 port 的
 * arkrtos_port_cyccnt_init），要在命令行打开一份带仪表的构建：
 *     EXTRA_CFLAGS=-DARKRTOS_USE_RTSTAT=1
 * 写成 #undef 会让这条 -D 静默失效（2026-11-20 实测踩过）。
 * ============================================================ */
#ifndef ARKRTOS_USE_RTSTAT
#define ARKRTOS_USE_RTSTAT           0
#endif
#ifndef ARKRTOS_USE_SCHED_STAT
#define ARKRTOS_USE_SCHED_STAT       0
#endif
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

/* ============================================================
 * 十一、遗言与栈
 * 遗言机制（ARKRTOS_HW_SAFE）：机制与架构无关，本板打开。默认档下这块
 * QEMU 板上没有驱动登记安全回调（devs=0），机制按契约工作由 UTEST 的合成
 * 回调取证；打开 ARKRTOS_DEMO_HWSAFE 后板级挂三台板内虚拟设备（led0/1/2）
 * 并声明安全态回调，示例任务走完「遗言 / 驱动回调 / 活着不被中和」三条通道
 * （见 docs/跨架构机制矩阵.md §2.2 与 §2.3）。
 *
 * SMP 的次核栈用 #ifndef：默认档下它是「定义了但没用」的一个数，
 * 打开 SMP 时工程可以用 -D 覆盖，不必改本文件。
 * ============================================================ */
#undef  ARKRTOS_HW_SAFE
#define ARKRTOS_HW_SAFE              1
#ifndef ARKRTOS_DEMO_HWSAFE
#define ARKRTOS_DEMO_HWSAFE          0
#endif
#undef  ARKRTOS_TIMER_TASK_STACK_WORDS
#define ARKRTOS_TIMER_TASK_STACK_WORDS (512)
#ifndef ARKRTOS_USE_SMP
#define ARKRTOS_USE_SMP              0
#endif
#ifndef ARKRTOS_SMP_SEC_STACK_SIZE
#define ARKRTOS_SMP_SEC_STACK_SIZE   8192
#endif
/* 次核「恢复现场前」的临时内核栈（R3）：装窗口与补帧特权级都要用它。
 * 4 KB 对两个 C 函数调用足够；两核各一块，见 arkrtos_smp_sched.c。 */
#ifndef ARKRTOS_SMP_TRAP_STACK_SIZE
#define ARKRTOS_SMP_TRAP_STACK_SIZE  4096
#endif

#endif /* __ARKRTOS_BOARD_CONFIG_H__ */
