/**
* @brief 系统指示灯模块（HAL 库版本）- STM32H743
* @details 板载唯一一颗状态灯接在 PC13，低电平点亮。
*          对外仍沿用 "led0" 这个名字，App 与内核示例不必因换板而改代码。
*/

#include "drvled.h"
#include "stm32h7xx_hal.h"

static const uint16_t led_gpio_pins[LED_MAX_NUM] = {
    GPIO_PIN_13
};

static led_dev_t sysled[LED_MAX_NUM];

arkrtos_dev_hdr_t *led_drv_open(uint32 id, uint32 p)
{
    GPIO_InitTypeDef  GPIO_InitStr;
    led_dev_t *p_dev;

    (void)p;
    if(id >= LED_MAX_NUM)
    {
        return 0;
    }

    p_dev = &sysled[id];

    if(p_dev->opened == 0)
    {
        p_dev->opened = 1;
        p_dev->led_on = 0;
        p_dev->gpio_pin = led_gpio_pins[id];
        p_dev->hdr.block_size = sizeof(led_dev_t);

        __HAL_RCC_GPIOC_CLK_ENABLE();

        GPIO_InitStr.Mode  = GPIO_MODE_OUTPUT_PP;
        GPIO_InitStr.Pin   = p_dev->gpio_pin;
        GPIO_InitStr.Pull  = GPIO_PULLUP;
        GPIO_InitStr.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(GPIOC, &GPIO_InitStr);

        HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_SET);
    }

    return (arkrtos_dev_hdr_t *)p_dev;
}

int32 led_drv_read(arkrtos_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    (void)pdata;
    (void)len;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }

    if(p_dev->opened)
    {
        return p_dev->led_on;
    }
    return -1;
}

int32 led_drv_write(arkrtos_dev_hdr_t *p, uint8 *pdata, int32 len)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    (void)pdata;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }

    if(p_dev->opened)
    {
        if(len > 0)
        {
            p_dev->led_on = 1;
        }
        else if(len == 0)
        {
            p_dev->led_on = 0;
        }
        else
        {
            p_dev->led_on = !p_dev->led_on;
        }
        if(p_dev->led_on == 0)
        {
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_SET);
        }
        else
        {
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_RESET);
        }
        return 0;
    }
    else
    {
        return -1;
    }
}

int32 led_drv_close(arkrtos_dev_hdr_t *p)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }

    p_dev->opened = 0;
    HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_SET);
    return 0;
}

int32 led_drv_ctrl(arkrtos_dev_hdr_t *p, uint32 c, uint32 v)
{
    led_dev_t *p_dev = (led_dev_t *)p;

    (void)v;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }
    if(p_dev->opened == 0)
    {
        return -1;
    }

    switch(c)
    {
        case ARKRTOS_LED_CTRL_OFF:
            p_dev->led_on = 0;
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_SET);
            break;
        case ARKRTOS_LED_CTRL_ON:
            p_dev->led_on = 1;
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_RESET);
            break;
        case ARKRTOS_LED_CTRL_TOGGLE:
            p_dev->led_on = (p_dev->led_on == 0) ? 1 : 0;
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin,
                              (p_dev->led_on == 0) ? GPIO_PIN_SET : GPIO_PIN_RESET);
            break;
        default:
            /* Unknown command code: refuse loudly instead of returning a 0 that
             * looks like success (this path is also used by the "will"). */
            return -1;
    }
    return 0;
}

#if ARKRTOS_HW_SAFE
/* Safe-state callback: a crashed owner must not leave the indicator on.
 * Non-blocking and interrupt-safe by construction. */
int32 led_drv_safe(arkrtos_dev_hdr_t *p, uint32 owner)
{
    led_dev_t *p_dev = (led_dev_t *)p;
    (void)owner;
    if((p == 0) || (sizeof(led_dev_t) != p->block_size))
    {
        return -1;
    }
    if(p_dev->opened)
    {
        p_dev->led_on = 0;
        HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_SET);
    }
    return 0;
}
#endif

arkrtos_dev_drv_t led_drv =
{
    led_drv_open,
    led_drv_close,
    led_drv_read,
    led_drv_write,
    led_drv_ctrl
};
