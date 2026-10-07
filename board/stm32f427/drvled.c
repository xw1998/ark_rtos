/**
* @brief ϵͳָʾ��ģ�飨HAL��汾��
* @details ֧��RGB��ɫLED��ͨ��openʱ��id����ѡ����ɫ��
*          LED_ID_RED(0)   -> PC0 ��ɫ
*          LED_ID_GREEN(1) -> PC1 ��ɫ
*          LED_ID_BLUE(2)  -> PC2 ��ɫ
* @author xw
* @date 2026.05.03
*/

#include "drvled.h"
#include "stm32f4xx_hal.h"

#define LED_MAX_NUM     3

static const uint16_t led_gpio_pins[LED_MAX_NUM] = {
    GPIO_PIN_0,
    GPIO_PIN_1,
    GPIO_PIN_2
};

static led_dev_t sysled[LED_MAX_NUM];

arkrtos_dev_hdr_t *led_drv_open(uint32 id, uint32 p)
{
    GPIO_InitTypeDef  GPIO_InitStr;
    led_dev_t *p_dev;

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

    (void)v;                        /* 只有命令码参与；参数保留给将来 */
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
        case ARKRTOS_LED_CTRL_OFF:                  /* 0 */
            p_dev->led_on = 0;
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_SET);
            break;
        case ARKRTOS_LED_CTRL_ON:                   /* 1 */
            p_dev->led_on = 1;
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin, GPIO_PIN_RESET);
            break;
        case ARKRTOS_LED_CTRL_TOGGLE:               /* 2 */
            p_dev->led_on = (p_dev->led_on == 0) ? 1 : 0;
            HAL_GPIO_WritePin(GPIOC, p_dev->gpio_pin,
                              (p_dev->led_on == 0) ? GPIO_PIN_SET : GPIO_PIN_RESET);
            break;
        default:
            /* 未知命令码：明确拒绝，而不是回一个看着像成功的 0
             * （本驱动的 ctrl 会被"遗言"路径调用，静默成功会让 App 以为
             *  硬件已经带回安全态，实际什么都没做）。 */
            return -1;
    }
    return 0;
}

#if ARKRTOS_HW_SAFE
/* Safe-state callback for the hardware-safe ("will") mechanism: if a task
 * that owned this LED dies (exit / kill / fault recovery), the kernel calls
 * this to force it off - a crashed App must not leave an indicator (or, on a
 * real board, an actuator) stuck in its last commanded state. Non-blocking
 * and interrupt-safe by construction: one RAM write plus one GPIO write. */
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
