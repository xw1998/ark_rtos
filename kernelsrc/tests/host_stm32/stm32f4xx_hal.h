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

/* ---------------- UART ---------------- */

#define GPIO_AF7_USART1  0x07u
#define GPIO_AF7_USART2  0x07u
#define GPIO_AF7_USART3  0x07u

#define UART_WORDLENGTH_8B      0x00000000u
#define UART_WORDLENGTH_9B      0x00001000u
#define UART_STOPBITS_1         0x00000000u
#define UART_STOPBITS_2         0x00002000u
#define UART_PARITY_NONE        0x00000000u
#define UART_PARITY_EVEN        0x00000400u
#define UART_PARITY_ODD         0x00000600u
#define UART_MODE_RX            0x00000004u
#define UART_MODE_TX            0x00000008u
#define UART_MODE_TX_RX         (UART_MODE_TX | UART_MODE_RX)
#define UART_HWCONTROL_NONE     0x00000000u
#define UART_HWCONTROL_RTS      0x00000100u
#define UART_HWCONTROL_CTS      0x00000200u
#define UART_OVERSAMPLING_16    0x00000000u
#define UART_OVERSAMPLING_8     0x00008000u

typedef struct
{
    uint32_t BaudRate;
    uint32_t WordLength;
    uint32_t StopBits;
    uint32_t Parity;
    uint32_t Mode;
    uint32_t HwFlowCtl;
    uint32_t OverSampling;
} UART_InitTypeDef;

typedef struct
{
    USART_TypeDef     *Instance; /* 两种探针分支都给了 USART_TypeDef */
    UART_InitTypeDef   Init;
    uint8_t            RXBlockSize;
    uint8_t            TXBlockSize;
} UART_HandleTypeDef;

/* 中断码的编码与真 HAL 一致：高 4 位选寄存器（2 = CR1，3 = CR2，4 = CR3），
 * 低 16 位是位掩码。驱动只用 CR1 的 RXNEIE / TCIE / TXEIE，但宏照真 HAL
 * 的三种寄存器都实现，免得将来有人加一个 CR3 中断却被静默吞掉。 */
#define UART_CR1_REG_INDEX  0x2u
#define UART_CR2_REG_INDEX  0x3u
#define UART_CR3_REG_INDEX  0x4u
/* 真 HAL 把「哪个寄存器」编码在高 4 位、把「哪一位」编码在低位掩码里。
 * 这里保持同样的形状：高 4 位是寄存器索引（2=CR1、3=CR2、4=CR3），低 16 位
 * 直接写成该寄存器里的位置掩码，这样编码一旦写错，错的是 CR1 的某一位
 * （用例当场看得见），而不是悄悄落到某个无关位上。 */
#define UART_IT_MASK        0x0000FFFFu

#define UART_IT_RXNE        0x20000020u /* CR1, bit 5  RXNEIE */
#define UART_IT_TC          0x20000040u /* CR1, bit 6  TCIE   */
#define UART_IT_TXE         0x20000080u /* CR1, bit 7  TXEIE  */

#define __HAL_UART_ENABLE_IT(__HANDLE__, __INTERRUPT__)                     \
    ((((__INTERRUPT__) >> 28U) == UART_CR1_REG_INDEX)                       \
     ? ((__HANDLE__)->Instance->CR1 |= ((__INTERRUPT__) & UART_IT_MASK))     \
     : (((__INTERRUPT__) >> 28U) == UART_CR2_REG_INDEX)                     \
       ? ((__HANDLE__)->Instance->CR2 |= ((__INTERRUPT__) & UART_IT_MASK))   \
       : ((__HANDLE__)->Instance->CR3 |= ((__INTERRUPT__) & UART_IT_MASK)))

#define __HAL_UART_DISABLE_IT(__HANDLE__, __INTERRUPT__)                    \
    ((((__INTERRUPT__) >> 28U) == UART_CR1_REG_INDEX)                       \
     ? ((__HANDLE__)->Instance->CR1 &= ~((__INTERRUPT__) & UART_IT_MASK))    \
     : (((__INTERRUPT__) >> 28U) == UART_CR2_REG_INDEX)                     \
       ? ((__HANDLE__)->Instance->CR2 &= ~((__INTERRUPT__) & UART_IT_MASK))  \
       : ((__HANDLE__)->Instance->CR3 &= ~((__INTERRUPT__) & UART_IT_MASK)))

#define __HAL_RCC_USART1_CLK_ENABLE()  do { } while(0)
#define __HAL_RCC_USART2_CLK_ENABLE()  do { } while(0)
#define __HAL_RCC_USART3_CLK_ENABLE()  do { } while(0)

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *huart);
HAL_StatusTypeDef HAL_UART_DeInit(UART_HandleTypeDef *huart);
void HAL_NVIC_SetPriority(int32_t irqn, uint32_t preempt, uint32_t sub);
void HAL_NVIC_EnableIRQ(int32_t irqn);

#endif /* HOST_STM32F4XX_HAL_H */
