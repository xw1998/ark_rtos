/**
* @file arkrtos_board.h
* @brief QEMU `virt`（LoongArch64）板级接口
* @details 只暴露控制台输出与板级初始化。真实芯片的板级头会声明 LED、
*          Flash、看门狗等驱动；QEMU 上这些都不存在，所以这里只有 ns16550a。
* @author xw
*/

#ifndef __ARKRTOS_BOARD_H__
#define __ARKRTOS_BOARD_H__

#include "arkrtos_types.h"
#include "arkrtos_features.h"

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

/**
* @brief 把 ns16550a 注册成设备框架里的 ttyS0（`SHELL_DEV_NAME`）
* @details Shell 不直接用板级打印函数，它按**设备**打开控制台，所以板级必须
*          先把这个名字注册进去（见 board/qemu_loongarch64/drvuart.c）。
* @return 0 成功；负值失败（同名 / 表满 / 描述符缺项）
*/
int32 arkrtos_board_uart_dev_register(void);

#if (ARKRTOS_USE_FDT == 1)
/**
* @brief 编进镜像的标准设备树 blob（.dtb）与它的字节数
* @details MMU 档不 link MCU 那张 const C 平表：镜像里放的是一块标准
*          .dtb（tools/dts/dtc_lite.py --format dtb），由启动代码交给
*          arkrtos_fdt_init() 解析。返回的指针指向只读数据，生命周期与
*          镜像相同。
*/
const void *arkrtos_board_dtb(uint32 *size);
#endif

/**
* @brief 整机复位
* @note QEMU loongarch `virt` 没有 sifive_test 那种复位设备（RISC-V virt 才有），
*       本实现只能打印一句说明后停机：假装复位成功比明确说"做不到"更糟。
*/
void arkrtos_board_reboot(void);

#endif /* __ARKRTOS_BOARD_H__ */
