/**
* @file mini_test_board_off.h
* @brief arkrtos_mini 关档用例的板级配置包装（ARKRTOS_USE_MINIAPP=0）
* @details 先取 mini_test_board.h（它已经把 BLK/FS/VFS 留在板级原值、
*          打开了 LOG 依赖），再把被测模块那一个开关压回 0。
*
*          为什么不能直接 -DARKRTOS_USE_MINIAPP=0：板级头对每一项都是
*          「先 #undef 再 #define」写死的，命令行 -D 会被它覆盖。包装头的
*          做法是「包含板级头之后再改」，arkrtos_config.h 的二次包含被
*          include guard 吃掉，改动因此保留到翻译单元末尾。
*
*          关档用例要证明的不是「功能还在」，而是「模块一个符号都没编进来，
*          每个读数都如实回答没有这件事」——所以这一档必须真的走 MINIAPP=0
*          的预处理分支，而不是靠运行时把开关关掉。
*
* @author xw
* @date 2026.11.22
*/

#ifndef ARKRTOS_MINI_TEST_BOARD_OFF_H
#define ARKRTOS_MINI_TEST_BOARD_OFF_H

#include "mini_test_board.h"

#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP           0

#endif /* ARKRTOS_MINI_TEST_BOARD_OFF_H */
