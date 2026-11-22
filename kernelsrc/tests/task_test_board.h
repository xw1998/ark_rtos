/**
* @file task_test_board.h
* @brief 宿主用例专用的板级配置包装（arkrtos_task 的测试档）
* @details 与 mini_test_board.h 同一套写法：先取一份真实板级配置（QEMU
*          LoongArch64 / MMU 档），再把本用例不需要的可选子系统**显式关掉**。
*
*          为什么关：arkrtos_task.c 是内核调度与 SVC 分发的枢纽，它在编译期
*          引用了几乎所有可选子系统（文件系统、网络、软总线、壳、小程序、
*          Linux ABI……）。宿主用例只量调度器自身（指针校验 / 就绪位图 /
*          延时链 / 调度锁 / SVC 分发 / 线程创建与退出 / 栈守卫 / 任务换入），
*          这些被关掉的子系统各自已有独立的宿主用例（test_fs_host.c、
*          test_vfs_host.c、test_softbus_host.c、test_shell_auth_host.c、
*          test_linux_abi_host.c、test_mini_host.c……），不需要在这里重测。
*
*          关档之后仍会剩下「同核同步对象」（sem/mtx/mq/cond/event/timer/
*          mempool/dev）的内部函数引用 —— 它们是 SVC 分发的目标，属于另一批
*          模块，本用例一律在 test_task_host.c 里打桩，并让桩返回可数的账。
*
*          保留的档位：
*            USE_SEM / USE_MTX / USE_MQ / USE_COND / USE_EVENT / USE_TIMER /
*            USE_DEV  —— 板级原值（1），SVC 号表要按板上形状编出来；
*            USE_EXIT_CODE=1   —— 量 arkrtos_thread_exit_code() 与退出码存放；
*            USE_SCHED_LOCK=1  —— features.h 写明「没有诚实的替代品」，必须 1；
*            USE_STACK_GUARD=1 —— task.c 里有 200 多行栈守卫，宿主可测；
*            USE_STACK_USAGE=1 / USE_CPU_LOAD=1 / USE_PRIV=1 —— 同上，宿主可测；
*            USE_SPINLOCK=1    —— 板级原值；
*            USE_TASK_SWAP=1   —— task.c 里 500 多行换入逻辑，端口层全部打桩。
*          关掉的档位见下（每一项都写清为什么关）。
*
*          用法（coverage_gate 的 CASE 里）：
*              -DARKRTOS_BOARD_CONFIG="task_test_board.h"
*              -Ikernelsrc/tests -Iboard/qemu_loongarch64
*          ——这两个 -I 必须排在 COMMON_INC 之前，否则会选中 stm32f427 那份
*          板级头（宿主上编不过）。
*
* @author xw
* @date 2026.11.22
*/

#ifndef ARKRTOS_TASK_TEST_BOARD_H
#define ARKRTOS_TASK_TEST_BOARD_H

#include "arkrtos_board_config.h"

/* 1. 小程序装载器：独立用例在 test_mini_host.c，这里不重测。 */
#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP          0

/* 2. Linux ABI：独立用例在 test_linux_abi_host.c；同时它要求 ELF=1，
 *    一并关掉把 ELF 指针窗口校验那条分支也去掉（那条分支由 test_elf_host.c
 *    覆盖）。 */
#undef  ARKRTOS_USE_LINUX_ABI
#define ARKRTOS_USE_LINUX_ABI        0
#undef  ARKRTOS_USE_ELF
#define ARKRTOS_USE_ELF              0

/* 3. 块层 / 文件系统 / VFS / procfs：分别有 test_blk_host.c、test_fs_host.c、
 *    test_vfs_host.c、test_vfs_lfs_host.c。FS 依赖 BLK，VFS 依赖 BLK，
 *    PROCFS 依赖 VFS，必须一起关（features.h 有 #error 守着）。 */
#undef  ARKRTOS_USE_PROCFS
#define ARKRTOS_USE_PROCFS           0
#undef  ARKRTOS_USE_VFS_LFS
#define ARKRTOS_USE_VFS_LFS          0
#undef  ARKRTOS_USE_VFS
#define ARKRTOS_USE_VFS              0
#undef  ARKRTOS_USE_FS
#define ARKRTOS_USE_FS               0
#undef  ARKRTOS_USE_BLK
#define ARKRTOS_USE_BLK              0

/* 4. 软总线 / 壳 / 审计 / 崩溃记账：各有独立用例。崩溃记账关掉之后
 *    arkrtos_crash_* 那组引用也一并消失。 */
#undef  ARKRTOS_USE_SOFTBUS
#define ARKRTOS_USE_SOFTBUS          0
#undef  ARKRTOS_USE_SHELL
#define ARKRTOS_USE_SHELL            0
#undef  ARKRTOS_USE_AUDIT
#define ARKRTOS_USE_AUDIT            0
#undef  ARKRTOS_USE_CRASH_LOG
#define ARKRTOS_USE_CRASH_LOG        0

/* 5. 内核 MDK trace：trace 有自己的 test_trace_host.c。关掉省掉三个
 *    mdk_trace_* 外部引用（它们不在本模块的被测面里）。 */
#undef  ARKRTOS_USE_MDK_TRACE
#define ARKRTOS_USE_MDK_TRACE        0

/* 6. 遗言机制：test_hwsafe_host.c 单独测；task.c 里 80 多行死亡回调链
 *    不是调度器本体。 */
#undef  ARKRTOS_HW_SAFE
#define ARKRTOS_HW_SAFE              0

/* 7. 内存池 / 流缓冲 / 事件集 / 多对象等待 / 递归互斥：core 之外的可选项，
 *    各有独立模块与用例；关掉之后 SVC 号表里对应分支不编入。 */
#undef  ARKRTOS_USE_MEMPOOL
#define ARKRTOS_USE_MEMPOOL          0
#undef  ARKRTOS_USE_SBUF
#define ARKRTOS_USE_SBUF             0
#undef  ARKRTOS_USE_ESET
#define ARKRTOS_USE_ESET             0
#undef  ARKRTOS_USE_WAITMULTI
#define ARKRTOS_USE_WAITMULTI        0
#undef  ARKRTOS_USE_MTX_RECURSIVE
#define ARKRTOS_USE_MTX_RECURSIVE    0

/* 8. 板端自测框架：arkrtos_utest_cases 是 shell 上下文用例，不参与宿主
 *    链接。 */
#undef  ARKRTOS_USE_UTEST
#define ARKRTOS_USE_UTEST            0

/* 9. 签名双闸：本用例不装载任何镜像，签名门在安装/装载路径上。 */
#undef  ARKRTOS_USE_IMAGE_SIGN
#define ARKRTOS_USE_IMAGE_SIGN       0
#undef  ARKRTOS_USE_EXEC_SIGN
#define ARKRTOS_USE_EXEC_SIGN        0

/* 10. 任务换入（A/B 无缝升级的内核一侧）：板级默认关，这里显式打开，
 *     task.c 里那 250 行 arm/take 才有东西可量。LA64 是 features.h 列出的
 *     三个受支持架构之一（Cortex-M / RV32IMAC / LA64），端口层在用例里打桩。 */
#undef  ARKRTOS_USE_TASK_SWAP
#define ARKRTOS_USE_TASK_SWAP        1

/* 11. 设备树：test_fdt_* 系列已覆盖，与调度器无关。 */
#undef  ARKRTOS_USE_FDT
#define ARKRTOS_USE_FDT              0
#undef  ARKRTOS_USE_FDT_CONSOLE
#define ARKRTOS_USE_FDT_CONSOLE      0
#undef  ARKRTOS_USE_FDT_OVERLAY
#define ARKRTOS_USE_FDT_OVERLAY      0

#endif /* ARKRTOS_TASK_TEST_BOARD_H */