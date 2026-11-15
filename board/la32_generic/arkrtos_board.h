/**
* @file arkrtos_board.h
* @brief 通用 LoongArch32 板级接口（参考目标，未在硬件上运行）
* @details 只暴露控制台输出与板级初始化。这份板级是从 la64 的 QEMU virt 版
*          照搬的**参考目标**：地址映射沿用 LoongArch virt 的约定
*          （ns16550a @0x1FE001E0、TCFG 定时器），但**没有任何 LA32 机器
*          真的跑过它**（QEMU 9.2.3 无 loongarch32 系统模拟器）。换真实芯片时
*          改 `ARKRTOS_QEMU_UART_BASE` 与几何配置即可。
* @author xw
*/

#ifndef __ARKRTOS_BOARD_H__
#define __ARKRTOS_BOARD_H__

#include "arkrtos_types.h"

/**
* @brief 初始化控制台 UART（ns16550a，8N1，轮询发送）
*/
void arkrtos_board_console_init(void);

/**
* @brief 轮询发送一个字符（换行自动补 '\r'，QEMU 串口按终端语义显示）
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

#endif /* __ARKRTOS_BOARD_H__ */
