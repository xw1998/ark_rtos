/**
* @file arkrtos_board.h
* @brief QEMU `virt`（RISC-V RV32）板级接口
* @details 只暴露控制台输出与板级初始化。真实芯片的板级头会声明 LED、
*          Flash、看门狗等驱动；QEMU 上这些都不存在，所以这里只有 16550。
* @author xw
*/

#ifndef __ARKRTOS_BOARD_H__
#define __ARKRTOS_BOARD_H__

#include "arkrtos_types.h"

/**
* @brief 初始化控制台 UART（16550，8N1，轮询发送）
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

/**
* @brief 开启 16550 的中断接收（IER.RDA）+ 使能 PLIC 上的 UART0 源
* @details 必须在 arkrtos_port_irq_init() 之后调：PLIC 只能在内核已能接
*          外部中断之后才启用源，否则第一个字节就可能把系统栽在中断入口里。
*/
void arkrtos_board_uart_irq_init(void);

/**
* @brief 从接收环形缓冲取数据（任务上下文）
*/
uint32 arkrtos_board_uart_rx_read(uint8 *buf, uint32 max);

/** 中断层读数：进过几次串口 ISR / 收过多少字节 / 满缓冲丢了多少 */
uint32 arkrtos_board_uart_isr_count(void);
uint32 arkrtos_board_uart_rx_bytes(void);
uint32 arkrtos_board_uart_rx_dropped(void);

/** 投到本板但不属于 UART0 的外部中断号个数（正常应为 0） */
uint32 arkrtos_board_uart_unknown_irq(void);

/**
* @brief 每收到一个字节在 ISR 里调一次（弱钩子，应用覆盖）
* @note 只能做中断安全的事：不能阻塞、不能长循环。
*/
void arkrtos_board_uart_rx_hook(uint8 byte);

/**
* @brief PMP 违规现场（覆盖 port 层弱钩子；mcause / mtval / 来源特权）
* @note 只在 ARKRTOS_USE_PRIV == 1 且真的发生违规时被调用。
*/
void arkrtos_port_fault_detail(uint32 mcause, uint32 mtval, uint32 from_priv);

/**
* @brief 把 16550 注册成设备框架里的 ttyS0（SHELL_DEV_NAME）
* @details Shell 不直接用板级打印函数，它按设备打开控制台，所以板级必须先把名字注册进去。
* @return 0 成功；负值失败（同名 / 表满 / 描述符缺项）
*/
int32 arkrtos_board_uart_dev_register(void);

/**
* @brief 整机复位（QEMU virt 的 sifive_test：0x00100000 写 0x7777）
* @details Cortex-M 上的 AIRCR 写法在这里没有对应映射，
*          所以 Shell 的 reboot 命令走这条板级实现。
*/
void arkrtos_board_reboot(void);

#endif /* __ARKRTOS_BOARD_H__ */
