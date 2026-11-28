/**
* @file test_drvled_host.c
* @brief board/stm32f427/drvled.c 的宿主用例：三色指示灯的驱动契约
* @details 这是**第一份板级 GPIO 驱动的宿主用例**，也是 `kernelsrc/tests/
*          host_stm32/stm32f4xx_hal.h` 这份 HAL 宿主桩的第一位用户。
*
*          宿主上钉住的是驱动**对外承诺**的那几件事，不是硅片：
*
*            1. open 的边界：id 只有 0..2（红/绿/蓝），越界返回 0 且**一次
*               HAL 都不碰**；同一 id 重复 open 不重复初始化（不重复拉时钟、
*               不重复写引脚），返回同一块 RAM；块大小与 `arkrtos_dev_hdr_t`
*               的契约一致，设备管理器靠它认人。
*            2. 引脚映射固定：0/1/2 -> PC0/PC1/PC2，且初始化参数是推挽输出、
*               上拉、低速——换了引脚或模式，板子上就是另一个灯亮。
*            3. 写的三态语义：`len > 0` 亮、`len == 0` 灭、`len < 0` 翻转，
*               电平极性是**反的**（亮 = RESET、灭 = SET，共阳接法）。
*            4. 未 open 时 read/write/ctrl 一律 -1，绝不假装成功；handle 或
*               块大小不对一律 -1（这一段是内核设备管理器的第二道闸）。
*            5. `ctrl` 的未知命令码返回 -1 且不写 GPIO——这条特别重要：`ctrl`
*               会被崩溃恢复的「遗言」路径调用，静默返回 0 会让 App 以为硬件
*               已被带回安全态，实际什么也没做。
*            6. HW_SAFE 档的 `led_drv_safe()`：open 着的灯被强制灭，未 open
*               的灯不产生任何 GPIO 动作（不碰别人的硬件）。
*
*          **不证明**：真 GPIO 输出电平、时钟门控是否真的打开、PC0..2 在
*          某个具体板子上是否接灯。HAL 在这里是桩，`GPIOC` 只当句柄传，
*          宿主上从不被解引用。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "arkrtos_types.h"
#include "arkrtos_dev.h"
#include "drvled.h"
#include "stm32f4xx_hal.h"

/* 五个入口是 drvled.c 的文件内符号，对外只有这张驱动表——所以用例也只
 * 走表：它顺便钉住「表里填的就是那几个入口」（设备管理器那一层由
 * arkrtos_dev 自己的用例负责，不在本档）。 */

/* ---------------- 断言 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s (line %d)\n", what, __LINE__);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %lu want %lu (line %d)\n", what, got, want, __LINE__);
    }
}

/* ---------------- HAL 桩：把每次调用记下来 ---------------- */

#define REC_MAX  64

typedef struct
{
    int             is_write;   /* 1 = WritePin，0 = Init */
    GPIO_TypeDef   *port;
    uint16_t        pin;
    GPIO_PinState   state;
    uint32_t        mode;
    uint32_t        pull;
    uint32_t        speed;
} hal_rec_t;

static hal_rec_t g_rec[REC_MAX];
static int       g_rec_n;

static void rec_reset(void)
{
    memset(g_rec, 0, sizeof(g_rec));
    g_rec_n = 0;
}

static void rec_push(hal_rec_t *r)
{
    if(g_rec_n < REC_MAX)
    {
        g_rec[g_rec_n] = *r;
        g_rec_n++;
    }
}

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
    hal_rec_t r;

    memset(&r, 0, sizeof(r));
    r.is_write = 0;
    r.port     = port;
    if(init != 0)
    {
        r.pin   = (uint16_t)init->Pin;
        r.mode  = init->Mode;
        r.pull  = init->Pull;
        r.speed = init->Speed;
    }
    rec_push(&r);
}

void HAL_GPIO_DeInit(GPIO_TypeDef *port, uint32_t pin)
{
    (void)port;
    (void)pin;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *port, uint16_t pin, GPIO_PinState state)
{
    hal_rec_t r;

    memset(&r, 0, sizeof(r));
    r.is_write = 1;
    r.port     = port;
    r.pin      = pin;
    r.state    = state;
    rec_push(&r);
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *port, uint16_t pin)
{
    (void)port;
    (void)pin;
    return GPIO_PIN_SET;
}

/* 取第 n 条记录（越界返回空记录，配合 ck 报错而不是越界读） */
static const hal_rec_t *rec_at(int n)
{
    static const hal_rec_t empty;
    if(n < 0 || n >= g_rec_n)
    {
        return &empty;
    }
    return &g_rec[n];
}

/* ---------------- 用例 ---------------- */

static arkrtos_dev_hdr_t *g_red;

static void test_table(void)
{
    /* 五个槽位都得填，而且不能互相串位（把 read 填进 write 是上板才看得出
     * 的错，宿主上只能靠「互不相同」这一层来挡）。 */
    ck(led_drv.drv_open  != 0, "driver table: open wired");
    ck(led_drv.drv_close != 0, "driver table: close wired");
    ck(led_drv.drv_read  != 0, "driver table: read wired");
    ck(led_drv.drv_write != 0, "driver table: write wired");
    ck(led_drv.drv_ctrl  != 0, "driver table: ctrl wired");
    ck((void *)led_drv.drv_read != (void *)led_drv.drv_write,
       "driver table: read and write are distinct");
    ck((void *)led_drv.drv_open != (void *)led_drv.drv_ctrl,
       "driver table: open and ctrl are distinct");
}

static void test_open(void)
{
    arkrtos_dev_hdr_t *p2;

    rec_reset();

    /* id 越界：三种越界都要挡住，且不能碰硬件。 */
    ck(led_drv.drv_open(3u, 0u) == 0, "open id 3 refused");
    ck(led_drv.drv_open(200u, 0u) == 0, "open id 200 refused");
    ck(led_drv.drv_open(0xFFFFFFFFu, 0u) == 0, "open id 0xFFFFFFFF refused");
    ck_u((unsigned long)g_rec_n, 0ul, "out-of-range open touches no HAL");

    /* 正常打开红灯：一次 Init + 一次 WritePin(SET)，顺序不能反。 */
    g_red = led_drv.drv_open(LED_ID_RED, 0u);
    ck(g_red != 0, "open red returns a device");
    ck_u((unsigned long)g_rec_n, 2ul, "open does one Init and one pin write");

    ck_u((unsigned long)rec_at(0)->is_write, 0ul, "first HAL call is GPIO init");
    ck(rec_at(0)->port == GPIOC, "red led is on port C");
    ck_u((unsigned long)rec_at(0)->pin, (unsigned long)GPIO_PIN_0, "red led is PC0");
    ck_u((unsigned long)rec_at(0)->mode, (unsigned long)GPIO_MODE_OUTPUT_PP,
         "red led is push-pull output");
    ck_u((unsigned long)rec_at(0)->pull, (unsigned long)GPIO_PULLUP, "red led is pulled up");
    ck_u((unsigned long)rec_at(0)->speed, (unsigned long)GPIO_SPEED_FREQ_LOW,
         "red led is low speed");

    ck_u((unsigned long)rec_at(1)->is_write, 1ul, "second HAL call is a pin write");
    ck_u((unsigned long)rec_at(1)->pin, (unsigned long)GPIO_PIN_0, "red initial write is PC0");
    ck_u((unsigned long)rec_at(1)->state, (unsigned long)GPIO_PIN_SET,
         "red starts off (SET is off, the led is active-low)");

    /* 设备头的契约：块大小必须等于结构体本身。 */
    ck_u((unsigned long)g_red->block_size, (unsigned long)sizeof(led_dev_t),
         "header block_size matches led_dev_t");

    /* 重复 open：返回同一块 RAM，且一次 HAL 都不再碰（不重新初始化）。 */
    rec_reset();
    p2 = led_drv.drv_open(LED_ID_RED, 0u);
    ck(p2 == g_red, "second open returns the same device block");
    ck_u((unsigned long)g_rec_n, 0ul, "second open does not touch HAL again");

    /* 另外两路：绿色 PC1、蓝色 PC2。 */
    rec_reset();
    p2 = led_drv.drv_open(LED_ID_GREEN, 0u);
    ck(p2 != 0, "open green");
    p2 = led_drv.drv_open(LED_ID_BLUE, 0u);
    ck(p2 != 0, "open blue");
    ck_u((unsigned long)g_rec_n, 4ul, "green and blue each init + write");
    ck_u((unsigned long)rec_at(0)->pin, (unsigned long)GPIO_PIN_1, "green led is PC1");
    ck_u((unsigned long)rec_at(2)->pin, (unsigned long)GPIO_PIN_2, "blue led is PC2");
}

static void test_read(void)
{
    /* 驱动内部就是 `static led_dev_t sysled[3]`，g_red 是第 0 个，
     * 所以 +1/+2 恰好是绿/蓝——这里靠驱动自己的布局取句柄，不另外 open。 */
    arkrtos_dev_hdr_t *p_green = (arkrtos_dev_hdr_t *)&((led_dev_t *)g_red)[1];
    arkrtos_dev_hdr_t *p_blue  = (arkrtos_dev_hdr_t *)&((led_dev_t *)g_red)[2];
    uint8 buf[4] = { 0 };
    uint32 saved;

    /* 空句柄 / 块大小不对：一律 -1（不读 RAM 里别的东西）。 */
    ck(led_drv.drv_read(0, buf, 1) == -1, "read NULL is refused");

    saved = g_red->block_size;
    g_red->block_size = 0u;
    ck(led_drv.drv_read(g_red, buf, 1) == -1, "read with a wrong block_size is refused");
    ck(led_drv.drv_write(g_red, buf, 1) == -1, "write with a wrong block_size is refused");
    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_ON, 0u) == -1,
       "ctrl with a wrong block_size is refused");
    ck(led_drv.drv_close(g_red) == -1, "close with a wrong block_size is refused");
    ck_u((unsigned long)((led_dev_t *)g_red)->opened, 1ul,
         "refused close leaves the device open");
    g_red->block_size = saved;

    /* 未 open 的设备：read 返回 -1，而不是那个残留的 led_on。 */
    ck(led_drv.drv_close(p_blue) == 0, "close blue");
    ck(led_drv.drv_read(p_blue, buf, 1) == -1, "read on a closed device is refused");
    ck(led_drv.drv_write(p_blue, buf, 1) == -1, "write on a closed device is refused");
    ck(led_drv.drv_ctrl(p_blue, ARKRTOS_LED_CTRL_ON, 0u) == -1,
       "ctrl on a closed device is refused");

    /* open 着的红灯：read 回的是驱动自己的状态。 */
    ck_u((unsigned long)led_drv.drv_read(g_red, buf, 1), 0ul, "led starts off");
    ck(led_drv.drv_write(g_red, buf, 1) == 0, "turn red on");
    ck_u((unsigned long)led_drv.drv_read(g_red, buf, 1), 1ul, "read reports on");
    ck_u((unsigned long)led_drv.drv_read(p_green, buf, 1), 0ul, "each led keeps its own state");

    (void)p_blue;
}

static void test_write(void)
{
    rec_reset();

    /* len > 0：亮 -> 引脚拉低（RESET）。 */
    {
        uint8 one = (uint8)'x';

        ck(led_drv.drv_write(g_red, &one, 1) == 0, "write len>0 returns 0");
    }
    ck_u((unsigned long)g_rec_n, 1ul, "write len>0 writes one pin");
    ck_u((unsigned long)rec_at(0)->state, (unsigned long)GPIO_PIN_RESET,
         "on means the pin is driven low");

    /* len == 0：灭 -> 引脚拉高（SET）。 */
    rec_reset();
    ck(led_drv.drv_write(g_red, 0, 0) == 0, "write len==0 returns 0");
    ck_u((unsigned long)g_rec_n, 1ul, "write len==0 writes one pin");
    ck_u((unsigned long)rec_at(0)->state, (unsigned long)GPIO_PIN_SET,
         "off means the pin is driven high");

    /* len < 0：翻转（从灭到亮）。 */
    rec_reset();
    ck(led_drv.drv_write(g_red, 0, -1) == 0, "write len<0 returns 0");
    ck_u((unsigned long)rec_at(0)->state, (unsigned long)GPIO_PIN_RESET,
         "toggle from off drives low");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 1ul, "toggle leaves the state on");

    /* 再翻一次：从亮回灭。 */
    rec_reset();
    ck(led_drv.drv_write(g_red, 0, -7) == 0, "any negative len toggles");
    ck_u((unsigned long)rec_at(0)->state, (unsigned long)GPIO_PIN_SET,
         "toggle from on drives high");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 0ul, "toggle leaves the state off");

    ck(led_drv.drv_write(0, 0, 1) == -1, "write NULL is refused");
}

static void test_ctrl(void)
{
    rec_reset();

    ck(led_drv.drv_ctrl(0, ARKRTOS_LED_CTRL_ON, 0u) == -1, "ctrl NULL is refused");

    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_ON, 0u) == 0, "ctrl ON returns 0");
    ck_u((unsigned long)rec_at(g_rec_n - 1)->state, (unsigned long)GPIO_PIN_RESET,
         "ctrl ON drives the pin low");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 1ul, "ctrl ON leaves the state on");

    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_OFF, 0u) == 0, "ctrl OFF returns 0");
    ck_u((unsigned long)rec_at(g_rec_n - 1)->state, (unsigned long)GPIO_PIN_SET,
         "ctrl OFF drives the pin high");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 0ul, "ctrl OFF leaves the state off");

    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_TOGGLE, 0u) == 0, "ctrl TOGGLE returns 0");
    ck_u((unsigned long)rec_at(g_rec_n - 1)->state, (unsigned long)GPIO_PIN_RESET,
         "ctrl TOGGLE from off drives low");
    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_TOGGLE, 0u) == 0, "ctrl TOGGLE twice returns 0");
    ck_u((unsigned long)rec_at(g_rec_n - 1)->state, (unsigned long)GPIO_PIN_SET,
         "ctrl TOGGLE from on drives high");

    /* 未知命令码：必须明确拒绝，且不许动引脚。
     * 遗言路径靠这个返回值判断「硬件是否已回安全态」，静默成功是安全缺口。 */
    rec_reset();
    ck(led_drv.drv_ctrl(g_red, 7u, 0u) == -1, "unknown ctrl code is refused");
    ck_u((unsigned long)g_rec_n, 0ul, "refused ctrl touches no pin");
    ck(led_drv.drv_ctrl(g_red, 0xFFFFFFFFu, 0u) == -1, "0xFFFFFFFF ctrl code is refused");

    /* ctrl 不看 value 参数（保留给将来），非零值不能改变语义。 */
    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_ON, 0x12345678u) == 0,
       "ctrl ignores the value argument");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 1ul, "ctrl with a value still turns on");
}

static void test_close(void)
{
    rec_reset();

    ck(led_drv.drv_close(0) == -1, "close NULL is refused");

    ck(led_drv.drv_close(g_red) == 0, "close returns 0");
    ck_u((unsigned long)g_rec_n, 1ul, "close writes the pin once");
    ck_u((unsigned long)rec_at(0)->state, (unsigned long)GPIO_PIN_SET,
         "close drives the pin high (led off)");
    ck_u((unsigned long)((led_dev_t *)g_red)->opened, 0ul, "close clears the opened flag");

    /* 再关一次：仍然 0（幂等），仍把灯写到灭——不会因为「已经关了」就跳过。 */
    rec_reset();
    ck(led_drv.drv_close(g_red) == 0, "second close is idempotent");
    ck_u((unsigned long)g_rec_n, 1ul, "second close still writes the pin");

    /* 关过之后再打开：重新初始化并回到灭，块大小不变。 */
    rec_reset();
    g_red = led_drv.drv_open(LED_ID_RED, 0u);
    ck(g_red != 0, "reopen after close works");
    ck_u((unsigned long)g_rec_n, 2ul, "reopen initialises the pin again");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 0ul, "reopened led is off");
}

#if ARKRTOS_HW_SAFE
static void test_safe(void)
{
    arkrtos_dev_hdr_t *p_green = (arkrtos_dev_hdr_t *)&((led_dev_t *)g_red)[1];

    /* 亮着的灯被强制灭。 */
    ck(led_drv.drv_ctrl(g_red, ARKRTOS_LED_CTRL_ON, 0u) == 0, "safe: turn red on");
    rec_reset();
    ck(led_drv_safe(g_red, 1u) == 0, "safe on an open led returns 0");
    ck_u((unsigned long)g_rec_n, 1ul, "safe writes the pin once");
    ck_u((unsigned long)rec_at(0)->state, (unsigned long)GPIO_PIN_SET,
         "safe drives the pin high (led off)");
    ck_u((unsigned long)led_drv.drv_read(g_red, 0, 0), 0ul, "safe clears the state");

    /* 未 open 的灯：返回 0 但一个动作都不做（不去碰别人的硬件）。 */
    ck(led_drv.drv_close(p_green) == 0, "safe: close green first");
    rec_reset();
    ck(led_drv_safe(p_green, 1u) == 0, "safe on a closed led returns 0");
    ck_u((unsigned long)g_rec_n, 0ul, "safe on a closed led touches no pin");

    ck(led_drv_safe(0, 1u) == -1, "safe NULL is refused");

    /* 块大小不对：同样 -1，不动引脚。 */
    {
        uint32 saved = g_red->block_size;

        g_red->block_size = 0u;
        rec_reset();
        ck(led_drv_safe(g_red, 1u) == -1, "safe with a wrong block_size is refused");
        ck_u((unsigned long)g_rec_n, 0ul, "refused safe touches no pin");
        g_red->block_size = saved;
    }
}
#endif

/* ---------------- main ---------------- */

int main(void)
{
    test_table();
    test_open();
    test_read();
    test_write();
    test_ctrl();
    test_close();
#if ARKRTOS_HW_SAFE
    test_safe();
#endif

    if(g_fails != 0)
    {
        printf("DRVLED-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

#if ARKRTOS_HW_SAFE
    printf("DRVLED-HOST OK  %d checks\n", g_checks);
#else
    printf("DRVLED-HOST OFF OK  %d checks\n", g_checks);
#endif

    return 0;
}
