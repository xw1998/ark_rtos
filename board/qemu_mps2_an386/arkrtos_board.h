/**
* @file arkrtos_board.h
* @brief QEMU `mps2-an386`（Cortex-M4）板级接口
* @details 只暴露控制台输出与板级初始化。真实芯片的板级头会声明 LED、
*          Flash、看门狗等驱动；这一档只有一颗 CMSDK APB UART0。
* @author xw
*/

#ifndef __ARKRTOS_BOARD_H__
#define __ARKRTOS_BOARD_H__

#include "arkrtos_types.h"

/**
* @brief 初始化控制台 UART（CMSDK APB UART0，8N1，轮询发送）
*/
void arkrtos_board_console_init(void);

/**
* @brief 轮询发送一个字符（换行自动补 '\r'）
*/
void arkrtos_board_putc(char c);

/**
* @brief 发送一个字符串
*/
void arkrtos_board_puts(const char *s);

/**
* @brief 以十进制发送一个无符号整数
*/
void arkrtos_board_putu(uint32 value);

/**
* @brief 以十六进制发送一个无符号整数（不带 0x 前缀，定宽 8 位）
*/
void arkrtos_board_puthex(uint32 value);

/**
* @brief 异常现场输出（覆盖 port 层弱钩子，可选）
* @details 本档没有故障恢复链路，四个 fault body 打印完现场就停机。
*/
void arkrtos_board_fault_report(const char *name, uint32 cfsr, uint32 hfsr,
                                uint32 mmfar, uint32 bfar);

#endif /* __ARKRTOS_BOARD_H__ */
