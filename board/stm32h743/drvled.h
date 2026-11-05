/**
* @brief 系统指示灯模块 - STM32H743 (FK743M5-XIH6)
* @details 板载 LED 接在 PC13，阴极接引脚、阳极经限流电阻到 3.3V：
*          引脚输出低电平点亮，输出高电平熄灭。
*/

#ifndef __DRV_LED_H__
#define __DRV_LED_H__

#include "arkrtos_def.h"
#include "arkrtos_dev.h"

#define LED_ID_STATUS  0        /* 沿用 F427 板的命名：led0 */

#define LED_MAX_NUM    1

typedef struct {
    arkrtos_dev_hdr_t hdr;
    uint8 opened;
    uint8 led_on;
    uint16 gpio_pin;
} led_dev_t;

/* ctrl codes accepted by led_drv_ctrl (and by the "will" the kernel runs) */
#define ARKRTOS_LED_CTRL_OFF     (0u)
#define ARKRTOS_LED_CTRL_ON      (1u)
#define ARKRTOS_LED_CTRL_TOGGLE  (2u)

extern arkrtos_dev_drv_t led_drv;

#if ARKRTOS_HW_SAFE
/* Hardware-safe ("will") callback: force this LED back off. */
int32 led_drv_safe(arkrtos_dev_hdr_t *p, uint32 owner);
#endif

#endif
