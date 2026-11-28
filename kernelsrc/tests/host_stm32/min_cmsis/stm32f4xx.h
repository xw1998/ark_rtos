/**
* @file stm32f4xx.h
* @brief 宿主桩：`board/stm32f427/drvuart.c` 用例用的最小 CMSIS 设备头
* @details 真 CMSIS（`Drivers/CMSIS/...`）在 x86 上编不过，原因只有一个：
*          `cmsis_gcc.h` 里的 `__get_PRIMASK` / `__disable_irq` /
*          `__set_PRIMASK` 是内联的 **ARM 汇编**（`MRS ... primask` /
*          `CPSID i`），调用点一被内联，x86 汇编器就报
*          `no such instruction: 'mrs %eax,primask'`。
*
*          这个驱动的临界区正是靠这三个内建函数实现的（写路径要「判
*          tx_idle 再踢 TXEIE」成一个原子步），所以不能绕开它们，只能把
*          它们换成宿主实现——于是这个头干脆不引入整份 CMSIS，只给出
*          驱动真正用到的那几个寄存器与位：
*
*            - `USART_TypeDef` 按 RM0090 的真实布局（SR 0x00 / DR 0x04 /
*              BRR 0x08 / CR1 0x0C / CR2 0x10 / CR3 0x14 / GTPR 0x18）。
*              布局必须对：用例在 0x40011000 真映射一页，驱动直接读写
*              这几个字，偏移错了就测的是别的东西。
*            - `GPIO_TypeDef` 与 `GPIOx` / `USARTx` 指针：只作句柄传给 HAL
*              桩，宿主上从不解引用。
*            - `USART_SR_*` / `USART_CR1_*` 位、`USART1_IRQn`。
*            - `__get_PRIMASK` / `__disable_irq` / `__set_PRIMASK` 三个
*              内建函数的宿主替身：影子寄存器 + 一个可选的观测钩子，让
*              用例能断言「写路径确实关过中断、并在返回前恢复」。
*
*          这不是「模拟 STM32」：没有时钟树、没有 NVIC、没有真实时序。
*          换一个外设（比如 GPIO 中断）要重看这个文件。
*/

#ifndef HOST_STM32F4XX_H
#define HOST_STM32F4XX_H

#include <stdint.h>

typedef struct
{
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

typedef struct
{
    volatile uint32_t SR;   /* 0x00 status */
    volatile uint32_t DR;   /* 0x04 data */
    volatile uint32_t BRR;  /* 0x08 baud rate */
    volatile uint32_t CR1;  /* 0x0C control 1 (UE / RXNEIE / TCIE / TXEIE) */
    volatile uint32_t CR2;  /* 0x10 control 2 */
    volatile uint32_t CR3;  /* 0x14 control 3 */
    volatile uint32_t GTPR; /* 0x18 guard time / prescaler */
} USART_TypeDef;

/* 地址照 RM0090 抄；USART1 那一页由用例真映射，其余只当 ID 用。 */
#define GPIOA   ((GPIO_TypeDef *)0x40020000u)
#define GPIOB   ((GPIO_TypeDef *)0x40020400u)
#define GPIOC   ((GPIO_TypeDef *)0x40020800u)
#define GPIOD   ((GPIO_TypeDef *)0x40020C00u)
#define GPIOE   ((GPIO_TypeDef *)0x40021000u)

#define USART1  ((USART_TypeDef *)0x40011000u)
#define USART2  ((USART_TypeDef *)0x40004400u)
#define USART3  ((USART_TypeDef *)0x40004800u)

/* 状态寄存器（RM0090 §30.6.1） */
#define USART_SR_PE     ((uint32_t)0x00000001u)
#define USART_SR_FE     ((uint32_t)0x00000002u)
#define USART_SR_NF     ((uint32_t)0x00000004u)
#define USART_SR_ORE    ((uint32_t)0x00000008u)
#define USART_SR_IDLE   ((uint32_t)0x00000010u)
#define USART_SR_RXNE   ((uint32_t)0x00000020u)
#define USART_SR_TC     ((uint32_t)0x00000040u)
#define USART_SR_TXE    ((uint32_t)0x00000080u)
#define USART_SR_LBD    ((uint32_t)0x00000100u)
#define USART_SR_CTS    ((uint32_t)0x00000200u)

/* 控制寄存器 1（RM0090 §30.6.4） */
#define USART_CR1_SBK   ((uint32_t)0x00000001u)
#define USART_CR1_RWU   ((uint32_t)0x00000002u)
#define USART_CR1_RE    ((uint32_t)0x00000004u)
#define USART_CR1_TE    ((uint32_t)0x00000008u)
#define USART_CR1_IDLEIE ((uint32_t)0x00000010u)
#define USART_CR1_RXNEIE ((uint32_t)0x00000020u)
#define USART_CR1_TCIE  ((uint32_t)0x00000040u)
#define USART_CR1_TXEIE ((uint32_t)0x00000080u)
#define USART_CR1_PEIE  ((uint32_t)0x00000100u)
#define USART_CR1_PS    ((uint32_t)0x00000200u)
#define USART_CR1_PCE   ((uint32_t)0x00000400u)
#define USART_CR1_WAKE  ((uint32_t)0x00000800u)
#define USART_CR1_M     ((uint32_t)0x00001000u)
#define USART_CR1_UE    ((uint32_t)0x00002000u)
#define USART_CR1_OVER8 ((uint32_t)0x00008000u)

typedef enum
{
    USART1_IRQn = 37
} IRQn_Type;

/* ---------------- 临界区内建函数的宿主替身 ----------------
 *
 * 影子寄存器：`__disable_irq()` 置 1、`__set_PRIMASK(m)` 写回 m。真 ARM 上
 * 这是 PRIMASK 的读改写；宿主上只要能被读出来就够，因为用例断言的是
 * 「关过、且恢复成原值」这件事，而不是中断延迟。
 *
 * 钩子：`host_primask_hook` 非空时，每次进入/退出临界区都会回调一次
 * （参数 1 = 关中断，0 = 恢复），用例据此记录调用序列。
 */
extern uint32_t host_primask_shadow;
extern void (*host_primask_hook)(int entering);

uint32_t host_get_primask(void);
void     host_disable_irq(void);
void     host_set_primask(uint32_t primask);

#define __get_PRIMASK()          host_get_primask()
#define __disable_irq()          host_disable_irq()
#define __set_PRIMASK(p)         host_set_primask(p)

#endif /* HOST_STM32F4XX_H */
