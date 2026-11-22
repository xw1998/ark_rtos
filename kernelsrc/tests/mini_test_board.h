/**
* @file mini_test_board.h
* @brief 宿主用例专用的板级配置包装（arkrtos_mini 的测试档）
* @details 板级头是生成物，里面每一项都是「先 #undef 再 #define」写死的，所以
*          命令行 -D 传进去的值会被它覆盖掉（linuxabi 那两个用例就是踩了这条
*          才必须 -include 板级头）。这里换成另一条写法：**先取一份真实板级
*          配置，再改本用例需要的那几项**。
*
*            1. 基础仍然是 QEMU LoongArch64（MMU 档）：
*                 - USE_BLK / USE_FS / USE_VFS 都已打开，满足
*                   ARKRTOS_USE_MINIAPP=1 与 ARKRTOS_MINI_READ_VFS=1 的编译期
*                   依赖（features.h 里有 #error 守着）；
*                 - 架构核是 LA64(0x0302)，是 ARKRTOS_USE_TASK_SWAP 支持的三种
*                   之一（Cortex-M / RV32IMAC / LA64）。
*               board/qemu_loongarch64/arkrtos_board_config.h 有 include guard，
*               所以 arkrtos_config.h 稍后再包含它一次不会把这里的改动冲掉。
*            2. ARKRTOS_USE_MINIAPP=1：被测模块本身就是它。
*            3. ARKRTOS_USE_IMAGE_SIGN=0：小程序装载不校签名（签名门在安装
*               路径上），关掉可以不在用例里塞一把假密钥。
*            4. ARKRTOS_USE_TASK_SWAP=1：把「无缝升级」那条路一起编进来量。
*            5. ARKRTOS_MINI_READ_VFS=1：走 VFS 那条读路径，顺带量
*               arkrtos_mini_last_read_error()。
*            6. ARKRTOS_USE_CRASH_LOG / SPINLOCK 保持板级原值：崩溃记账与自旋
*               锁在用例里都是桩，只要求这两组接口的形状与板上一致。
*
*          用法（coverage_gate 的 CASE 里）：
*              -DARKRTOS_BOARD_CONFIG="mini_test_board.h"
*              -Ikernelsrc/tests -Iboard/qemu_loongarch64
*          ——这两个 -I 必须排在 COMMON_INC 之前，否则同名板级头会选中
*          stm32f427 那一份（宿主上编不过）。
*
* @author xw
* @date 2026.11.22
*/

#ifndef ARKRTOS_MINI_TEST_BOARD_H
#define ARKRTOS_MINI_TEST_BOARD_H

#include "arkrtos_board_config.h"

#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP           1

#undef  ARKRTOS_USE_IMAGE_SIGN
#define ARKRTOS_USE_IMAGE_SIGN        0

/* 板级头把 EXEC_SIGN 也跟着设成 1，而 features.h 里有 #error 要求
   EXEC_SIGN=1 必须 IMAGE_SIGN=1；这里两档一起关，保持自洽。 */
#undef  ARKRTOS_USE_EXEC_SIGN
#define ARKRTOS_USE_EXEC_SIGN         0

#undef  ARKRTOS_USE_TASK_SWAP
#define ARKRTOS_USE_TASK_SWAP         1

#undef  ARKRTOS_MINI_READ_VFS
#define ARKRTOS_MINI_READ_VFS         1

#endif /* ARKRTOS_MINI_TEST_BOARD_H */
