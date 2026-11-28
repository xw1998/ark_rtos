/**
* @file stm32f4xx_hal.h
* @brief 宿主桩：让 board/stm32f427 下的驱动能在 x86 上编译
* @details 板级驱动直接调 STM32 HAL（`HAL_GPIO_WritePin` / `HAL_UART_*` /
*          `HAL_IWDG_*` / `HAL_FLASH_*`）并读 `GPIOx` 一类寄存器结构体。
*          真 HAL 头带 ARM CMSIS，宿主上编不过，所以这里给一份**只含宿主用例
*          用得到的那一小块**的替身：类型名、宏名、引脚号与原 HAL 逐字相同，
*          实现由各用例自己给（记录下来再断言"驱动到底把什么交给了硬件"）。
*
*          这不是「模拟 STM32」：它不描述寄存器布局，也不模拟任何时序。
*          `GPIOC` 之类的指针只作为**不透明句柄**传给桩函数，宿主上从不被
*          解引用——一旦有人写出解引用寄存器的代码，这里立刻会炸，那正是
*          我们想要的反馈（那段代码本来就只能在真硅片上有意义）。
*
*          用法：把 `-Ikernelsrc/tests/host_stm32` 放在包含路径**最前面**，
*          它就会盖住 `example/.../Drivers/STM32F4xx_HAL_Driver/Inc` 里的真头。
*          新增一个驱动用例时，往这里补它用到的那几个名字，别去改驱动。
*/

#ifndef HOST_STM32F4XX_HAL_H
#define HOST_STM32F4XX_HAL_H

#include <stdint.h>

/* 板级配置头（`arkrtos_board_config.h`）会先拉进真的 CMSIS 设备头，寄存器
 * 类型、端口指针与引脚号就都由它提供。那份头在宿主上能编（只定义地址常量）
 * 但没有 HAL 的函数式接口，所以这里按「真 CMSIS 在就只补 HAL、不在就整套
 * 自己给」来条件化：`GPIOA` 是 CMSIS 设备头里必有的符号，拿它当探针。 */
#ifndef GPIOA

/* ---------------- 寄存器块（不透明，只作句柄用） ---------------- */

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
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
} USART_TypeDef;

typedef struct
{
    volatile uint32_t KR;
    volatile uint32_t PR;
    volatile uint32_t RLR;
    volatile uint32_t SR;
} IWDG_TypeDef;

/* 地址照 RM0090 抄，但宿主上只当 ID 用：用例靠指针相等区分是哪个端口。 */
#define GPIOA   ((GPIO_TypeDef *)0x40020000u)
#define GPIOB   ((GPIO_TypeDef *)0x40020400u)
#define GPIOC   ((GPIO_TypeDef *)0x40020800u)
#define GPIOD   ((GPIO_TypeDef *)0x40020C00u)
#define GPIOE   ((GPIO_TypeDef *)0x40021000u)

#define USART1  ((USART_TypeDef *)0x40011000u)
#define USART2  ((USART_TypeDef *)0x40004400u)
#define USART3  ((USART_TypeDef *)0x40004800u)

#define IWDG    ((IWDG_TypeDef *)0x40003000u)

#endif /* !GPIOA：下面是 HAL 函数式接口，真 CMSIS 不提供，始终由本头补 */

/* ---------------- GPIO ---------------- */

#define GPIO_PIN_0    ((uint16_t)0x0001u)
#define GPIO_PIN_1    ((uint16_t)0x0002u)
#define GPIO_PIN_2    ((uint16_t)0x0004u)
#define GPIO_PIN_3    ((uint16_t)0x0008u)
#define GPIO_PIN_4    ((uint16_t)0x0010u)
#define GPIO_PIN_5    ((uint16_t)0x0020u)
#define GPIO_PIN_6    ((uint16_t)0x0040u)
#define GPIO_PIN_7    ((uint16_t)0x0080u)
#define GPIO_PIN_8    ((uint16_t)0x0100u)
#define GPIO_PIN_9    ((uint16_t)0x0200u)
#define GPIO_PIN_10   ((uint16_t)0x0400u)
#define GPIO_PIN_11   ((uint16_t)0x0800u)
#define GPIO_PIN_12   ((uint16_t)0x1000u)
#define GPIO_PIN_13   ((uint16_t)0x2000u)
#define GPIO_PIN_14   ((uint16_t)0x4000u)
#define GPIO_PIN_15   ((uint16_t)0x8000u)

typedef enum
{
    GPIO_PIN_RESET = 0,
    GPIO_PIN_SET
} GPIO_PinState;

#define GPIO_MODE_OUTPUT_PP     0x00000001u
#define GPIO_MODE_OUTPUT_OD     0x00000011u
#define GPIO_MODE_INPUT         0x00000000u
#define GPIO_MODE_AF_PP         0x00000002u
#define GPIO_NOPULL             0x00000000u
#define GPIO_PULLUP             0x00000001u
#define GPIO_PULLDOWN           0x00000002u
#define GPIO_SPEED_FREQ_LOW     0x00000000u
#define GPIO_SPEED_FREQ_HIGH    0x00000002u
#define GPIO_SPEED_FREQ_VERY_HIGH 0x00000003u

typedef struct
{
    uint32_t Pin;
    uint32_t Mode;
    uint32_t Pull;
    uint32_t Speed;
    uint32_t Alternate;
} GPIO_InitTypeDef;

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init);
void HAL_GPIO_DeInit(GPIO_TypeDef *port, uint32_t pin);
void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state);
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin);

#define __HAL_RCC_GPIOA_CLK_ENABLE()   do { } while(0)
#define __HAL_RCC_GPIOB_CLK_ENABLE()   do { } while(0)
#define __HAL_RCC_GPIOC_CLK_ENABLE()   do { } while(0)
#define __HAL_RCC_GPIOD_CLK_ENABLE()   do { } while(0)
#define __HAL_RCC_GPIOE_CLK_ENABLE()   do { } while(0)

/* ---------------- 公共类型 ---------------- */

typedef enum
{
    HAL_OK      = 0x00u,
    HAL_ERROR   = 0x01u,
    HAL_BUSY    = 0x02u,
    HAL_TIMEOUT = 0x03u
} HAL_StatusTypeDef;

#endif /* HOST_STM32F4XX_HAL_H */
