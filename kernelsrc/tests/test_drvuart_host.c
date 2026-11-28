/**
* @file test_drvuart_host.c
* @brief board/stm32f427/drvuart.c 的宿主用例：ttyS0 的驱动契约
* @details 这是**唯一一份真碰寄存器语义的板级驱动用例**：drvled / drvwdg /
*          drvnor / drvflash 的寄存器要么只当句柄传、要么整页映射后只读，
*          而这边驱动的中断收发路径**全部**是直接读写 `USART1->SR` /
*          `DR` / `CR1`。所以用例在 0x40010000 真映射一页（USART1 在
*          0x40011000），把 SR/DR/CR1 当普通内存写，再调用
*          `USART1_IRQHandler()` 走驱动自己的中断路径——测的是驱动对
*          「硬件刚发生了什么」的判断，而不是桩替它做的判断。
*
*          宿主上钉住的六件事：
*
*            1. open 的边界与初始化：越界 id 返回 0 且一次 HAL 都不碰；
*               PA9 推挽复用上拉、PA10 复用无上下拉、AF7、低速；HAL 的
*               八个参数逐项；NVIC 优先级 (0,0) 与 IRQ 37 使能；CR1 只
*               开 RXNEIE、关掉 TXEIE / TCIE；重复 open 不重复初始化。
*            2. HAL_UART_Init 失败时**不**声称端口已打开（返回 0、opened
*               复位、不使能 RXNEIE），下一次 open 会重试。
*            3. 接收：RXNE 与 ORE 各自都能把 DR 的字节交进接收环；一次
*               中断里 RX 与 TX 两条分支**互不饿死**（这是旧 if/else 的
*               真实缺陷，见 handle_usart_service 的注释）。
*            4. 发送：write 只把字节放进环、置 tx_idle=0 并踢 TXEIE；
*               中断按字节出队，队列空了就关 TXEIE、开 TCIE、回 idle；
*               TC 只在 idle 时清 TCIE（否则会风暴）。
*            5. 临界区：write 的「判 tx_idle → 踢 TXEIE」确实在关中断的
*               区间里完成——钩子记下进入/退出那一刻的 (tx_idle, TXEIE)，
*               断言中间态不可能被别的执行流看见。
*            6. close / ctrl 的诚实性：close 清三个中断使能、复位 tx_idle、
*               关 UE；关掉之后 read/write 都是 -1（不再交出旧字节、也不
*               谎报「写了 0 字节」）；ctrl 对任何命令码返回 -1，不再静默
*               成功。
*
*          **不证明**：真 USART 的收发时序、波特率误差、ORE 的真实产生
*          过程、NVIC 抢占是否真的按 (0,0) 生效、以及 `__disable_irq()`
*          在真 ARM 上的时序。临界区在宿主上用影子 PRIMASK + 钩子建模：
*          能证明「这段代码在关中断区间内」，不能证明「关中断花了多久」。
*          中断钩子 `host_primask_hook` 就是为此而设，见 min_cmsis/stm32f4xx.h。
*/

#include <stdio.h>
#include <string.h>

#include "stm32f4xx.h"      /* min_cmsis 桩：真布局的 USART + PRIMASK 替身 */
#include "stm32f4xx_hal.h"  /* HAL 函数式桩：只记录，不模拟时序 */
#include "drvuart.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* USART1 在 0x40011000；整页 64 KiB，一次映射够用（同页还有别的 APB2 外设，
 * 但本用例不碰它们）。 */
#define T_WIN_BASE   ((uintptr_t)0x40010000u)
#define T_WIN_SIZE   ((size_t)0x10000u)

/* ---------------- 断言账本 ---------------- */

static int g_checks;
static int g_fails;

static void ck(int cond, const char *what)
{
    g_checks++;
    if(cond == 0)
    {
        g_fails++;
        printf("FAIL %s\n", what);
    }
}

static void ck_u(unsigned long got, unsigned long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got 0x%lX want 0x%lX\n", what, got, want);
    }
}

static void ck_i(long got, long want, const char *what)
{
    g_checks++;
    if(got != want)
    {
        g_fails++;
        printf("FAIL %s: got %ld want %ld\n", what, got, want);
    }
}

/* ---------------- PRIMASK 替身与临界区钩子 ---------------- */

uint32_t host_primask_shadow;
void (*host_primask_hook)(int entering);

/* 钩子记下「关中断那一刻」与「恢复那一刻」的驱动器状态，用来证明
 * write 的判定与踢中断在同一个临界区里。 */
static uart_dev_t *g_dev;
static int g_hook_n;
static int g_state_enter_idle;      /* 进入临界区时的 tx_idle   */
static uint32_t g_state_enter_txe;  /* 进入临界区时的 CR1.TXEIE */
static int g_state_exit_idle;       /* 恢复中断时的 tx_idle     */
static uint32_t g_state_exit_txe;   /* 恢复中断时的 CR1.TXEIE   */

static void prim_hook(int entering)
{
    g_hook_n++;
    if(g_dev == 0)
    {
        return;
    }
    if(entering != 0)
    {
        g_state_enter_idle = (int)g_dev->tx_idle;
        g_state_enter_txe = USART1->CR1 & USART_CR1_TXEIE;
    }
    else
    {
        g_state_exit_idle = (int)g_dev->tx_idle;
        g_state_exit_txe = USART1->CR1 & USART_CR1_TXEIE;
    }
}

uint32_t host_get_primask(void)
{
    return host_primask_shadow;
}

void host_disable_irq(void)
{
    host_primask_shadow = 1u;
    if(host_primask_hook != 0)
    {
        host_primask_hook(1);
    }
}

void host_set_primask(uint32_t primask)
{
    host_primask_shadow = primask;
    if(host_primask_hook != 0)
    {
        host_primask_hook(0);
    }
}

/* ---------------- HAL 记录桩 ---------------- */

typedef struct
{
    GPIO_TypeDef *port;
    uint32_t      pin;
    uint32_t      mode;
    uint32_t      pull;
    uint32_t      speed;
    uint32_t      alternate;
} t_gpio_call_t;

#define T_MAX_GPIO 8

static t_gpio_call_t g_gpio[T_MAX_GPIO];
static int           g_gpio_n;

static int             g_uart_init_calls;
static UART_HandleTypeDef *g_uart_init_handle;
static UART_InitTypeDef   g_uart_init_seen;
static HAL_StatusTypeDef  g_uart_init_status;

static int  g_nvic_prio_calls;
static int  g_nvic_prio_irq;
static long g_nvic_prio_pre;
static long g_nvic_prio_sub;
static int  g_nvic_enable_calls;
static int  g_nvic_enable_irq;

void HAL_GPIO_Init(GPIO_TypeDef *port, GPIO_InitTypeDef *init)
{
    if(g_gpio_n < T_MAX_GPIO)
    {
        g_gpio[g_gpio_n].port      = port;
        g_gpio[g_gpio_n].pin       = init->Pin;
        g_gpio[g_gpio_n].mode      = init->Mode;
        g_gpio[g_gpio_n].pull      = init->Pull;
        g_gpio[g_gpio_n].speed     = init->Speed;
        g_gpio[g_gpio_n].alternate = init->Alternate;
    }
    g_gpio_n++;
}

HAL_StatusTypeDef HAL_UART_Init(UART_HandleTypeDef *huart)
{
    g_uart_init_calls++;
    g_uart_init_handle = huart;
    g_uart_init_seen   = huart->Init;
    return g_uart_init_status;
}

void HAL_NVIC_SetPriority(int32_t irqn, uint32_t preempt, uint32_t sub)
{
    g_nvic_prio_calls++;
    g_nvic_prio_irq = (int)irqn;
    g_nvic_prio_pre = (long)preempt;
    g_nvic_prio_sub = (long)sub;
}

void HAL_NVIC_EnableIRQ(int32_t irqn)
{
    g_nvic_enable_calls++;
    g_nvic_enable_irq = (int)irqn;
}

/* ---------------- 环境复位 / 映射 ---------------- */

static void reset_all(void)
{
    g_gpio_n = 0;
    g_uart_init_calls = 0;
    g_uart_init_handle = 0;
    memset(&g_uart_init_seen, 0, sizeof(g_uart_init_seen));
    g_uart_init_status = HAL_OK;
    g_nvic_prio_calls = 0;
    g_nvic_enable_calls = 0;
    g_hook_n = 0;
    g_state_enter_idle = -1;
    g_state_exit_idle = -1;
    g_state_enter_txe = 0xFFu;
    g_state_exit_txe = 0xFFu;
    host_primask_shadow = 0u;
    host_primask_hook = prim_hook;

    USART1->SR  = 0u;
    USART1->DR  = 0u;
    USART1->CR1 = 0u;
    USART1->CR2 = 0u;
    USART1->CR3 = 0u;
}

static int map_window(void)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)T_WIN_BASE, T_WIN_SIZE,
                           MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    return (p == (void *)T_WIN_BASE) ? 1 : 0;
#else
    void *p = mmap((void *)T_WIN_BASE, T_WIN_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    return (p == (void *)T_WIN_BASE) ? 1 : 0;
#endif
}

/* ---------------- 被测驱动的入口 ---------------- */

static arkrtos_dev_hdr_t *dv_open(uint32 name, uint32 baud)
{
    return usart_drv.drv_open(name, baud);
}

static int32 dv_close(arkrtos_dev_hdr_t *p)
{
    return usart_drv.drv_close(p);
}

static int32 dv_read(arkrtos_dev_hdr_t *p, uint8 *buf, int32 len)
{
    return usart_drv.drv_read(p, buf, len);
}

static int32 dv_write(arkrtos_dev_hdr_t *p, uint8 *buf, int32 len)
{
    return usart_drv.drv_write(p, buf, len);
}

static int32 dv_ctrl(arkrtos_dev_hdr_t *p, uint32 code, uint32 value)
{
    return usart_drv.drv_ctrl(p, code, value);
}

static uart_dev_t *as_dev(arkrtos_dev_hdr_t *p)
{
    return (uart_dev_t *)p;
}

/* ---------------- 1. 闸门 ---------------- */

static void test_guards(void)
{
    arkrtos_dev_hdr_t bad;
    arkrtos_dev_hdr_t *p;
    uint8 b = 0u;

    reset_all();

    p = dv_open(UART_DEV_NUM, 115200u);
    ck(p == 0, "open(id == UART_DEV_NUM) refused");
    p = dv_open(0xFFFFFFFFu, 115200u);
    ck(p == 0, "open(id == 0xFFFFFFFF) refused");
    ck_i(g_gpio_n, 0, "a refused open touches no GPIO");
    ck_i(g_uart_init_calls, 0, "a refused open does not init the UART");
    ck_i(g_nvic_prio_calls, 0, "a refused open does not touch the NVIC");
    ck_i((long)USART1->CR1, 0, "a refused open leaves CR1 alone");

    ck_i(dv_read(0, &b, 1), -1, "read(NULL) is -1");
    ck_i(dv_write(0, &b, 1), -1, "write(NULL) is -1");
    ck_i(dv_close(0), -1, "close(NULL) is -1");
    ck_i(dv_ctrl(0, 0u, 0u), -1, "ctrl(NULL) is -1");

    /* 设备管理器靠 block_size 认人：块大小不对就是别人的对象。 */
    bad.block_size = 0u;
    ck_i(dv_read(&bad, &b, 1), -1, "read() on a foreign block is -1");
    ck_i(dv_write(&bad, &b, 1), -1, "write() on a foreign block is -1");
    ck_i(dv_close(&bad), -1, "close() on a foreign block is -1");
    ck_i(dv_ctrl(&bad, 0u, 0u), -1, "ctrl() on a foreign block is -1");
}

/* ---------------- 2. open 的初始化合同 ---------------- */

static void test_open(void)
{
    arkrtos_dev_hdr_t *p;
    arkrtos_dev_hdr_t *p2;

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 921600u);
    ck(p != 0, "open(ttyS0) returns a handle");
    if(p == 0)
    {
        return;
    }
    g_dev = as_dev(p);

    /* 两笔 GPIO：PA9 = TX（复用推挽、上拉、低速、AF7），PA10 = RX（无上下拉）。 */
    ck_i(g_gpio_n, 2, "open initialises exactly two pins");
    if(g_gpio_n == 2)
    {
        ck(g_gpio[0].port == GPIOA, "PA9 is on GPIOA");
        ck_u(g_gpio[0].pin, (unsigned long)GPIO_PIN_9, "first pin is PA9");
        ck_u(g_gpio[0].mode, (unsigned long)GPIO_MODE_AF_PP, "PA9 is alternate push-pull");
        ck_u(g_gpio[0].pull, (unsigned long)GPIO_PULLUP, "PA9 is pulled up");
        ck_u(g_gpio[0].speed, (unsigned long)GPIO_SPEED_FREQ_LOW, "PA9 is low speed");
        ck_u(g_gpio[0].alternate, (unsigned long)GPIO_AF7_USART1, "PA9 is AF7");

        ck(g_gpio[1].port == GPIOA, "PA10 is on GPIOA");
        ck_u(g_gpio[1].pin, (unsigned long)GPIO_PIN_10, "second pin is PA10");
        ck_u(g_gpio[1].mode, (unsigned long)GPIO_MODE_AF_PP, "PA10 is alternate push-pull");
        ck_u(g_gpio[1].pull, (unsigned long)GPIO_NOPULL, "PA10 has no pull");
        ck_u(g_gpio[1].speed, (unsigned long)GPIO_SPEED_FREQ_LOW, "PA10 is low speed");
        ck_u(g_gpio[1].alternate, (unsigned long)GPIO_AF7_USART1, "PA10 is AF7");
    }

    /* HAL 的八个参数逐项：换一个就是另一个波特率/帧格式。 */
    ck_i(g_uart_init_calls, 1, "open inits the UART once");
    if(g_uart_init_calls == 1)
    {
        ck(g_uart_init_handle != 0 && g_uart_init_handle->Instance == USART1,
           "HAL init sees USART1");
        ck_u(g_uart_init_seen.BaudRate, 921600u, "baud rate is passed through");
        ck_u(g_uart_init_seen.WordLength, (unsigned long)UART_WORDLENGTH_8B, "8 data bits");
        ck_u(g_uart_init_seen.StopBits, (unsigned long)UART_STOPBITS_1, "1 stop bit");
        ck_u(g_uart_init_seen.Parity, (unsigned long)UART_PARITY_NONE, "no parity");
        ck_u(g_uart_init_seen.Mode, (unsigned long)UART_MODE_TX_RX, "TX and RX");
        ck_u(g_uart_init_seen.HwFlowCtl, (unsigned long)UART_HWCONTROL_NONE, "no flow control");
        ck_u(g_uart_init_seen.OverSampling, (unsigned long)UART_OVERSAMPLING_16,
             "oversampling 16");
    }

    /* 中断优先级必须数值上低于 SVCall：SVC 处理里会自旋等发送环腾位置，
     * 串口中断若抢不过这段自旋，环永远排不空。 */
    ck_i(g_nvic_prio_calls, 1, "NVIC priority set once");
    ck_i(g_nvic_prio_irq, (int)USART1_IRQn, "priority set for USART1_IRQn");
    ck_i(g_nvic_prio_pre, 0, "preemption priority 0");
    ck_i(g_nvic_prio_sub, 0, "sub priority 0");
    ck_i(g_nvic_enable_calls, 1, "USART1 IRQ enabled once");
    ck_i(g_nvic_enable_irq, (int)USART1_IRQn, "the enabled line is USART1_IRQn");

    /* CR1：只开接收中断，发送与完成中断关着，避免 init 完就进风暴。 */
    ck((USART1->CR1 & USART_CR1_RXNEIE) != 0u, "RXNEIE is enabled");
    ck((USART1->CR1 & USART_CR1_TXEIE) == 0u, "TXEIE stays disabled after open");
    ck((USART1->CR1 & USART_CR1_TCIE) == 0u, "TCIE stays disabled after open");

    ck_u(p->block_size, (unsigned long)(uint32)sizeof(uart_dev_t),
         "block_size matches uart_dev_t");
    ck(g_dev->usart_addr == USART1, "the device points at USART1");
    ck_i((long)g_dev->opened, 1, "opened is 1");
    ck_i((long)g_dev->tx_idle, 1, "tx_idle is 1 after open");
    ck(g_dev->tx_pipe != 0 && g_dev->rx_pipe != 0, "both pipes exist");
    ck(g_dev->tx_pipe != g_dev->rx_pipe, "the two pipes are distinct");

    /* 重复 open：同一个 id 不重复初始化、返回同一块 RAM。 */
    p2 = dv_open(UART_DEV_TTYS0, 9600u);
    ck(p2 == p, "a second open returns the same handle");
    ck_i(g_gpio_n, 2, "a second open does not reconfigure GPIO");
    ck_i(g_uart_init_calls, 1, "a second open does not re-init the UART");
    ck_i(g_nvic_enable_calls, 1, "a second open does not re-enable the IRQ");
    ck_u(g_uart_init_seen.BaudRate, 921600u, "the first open's baud rate still stands");
}

/* ---------------- 3. HAL 初始化失败 ---------------- */

static void test_init_failure(void)
{
    arkrtos_dev_hdr_t *p;

    /* 驱动静态态在进程内不能复位，先把上一个用例留下的打开态关掉。 */
    (void)dv_close((arkrtos_dev_hdr_t *)g_dev);
    reset_all();
    g_uart_init_status = HAL_ERROR;
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p == 0, "a refused HAL init means open returns 0");
    ck_i((USART1->CR1 & USART_CR1_RXNEIE), 0, "a failed open does not enable RXNEIE");
    ck_i((USART1->CR1 & USART_CR1_TXEIE), 0, "a failed open does not enable TXEIE");

    /* 失败之后不能把端口记成「已打开」：下一次 open 必须真的重试。 */
    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "a retry after a failed init succeeds");
    ck_i(g_uart_init_calls, 1, "the retry really calls HAL again");
    ck((USART1->CR1 & USART_CR1_RXNEIE) != 0u, "the retry enables RXNEIE");
    ck_i(dv_close(p), 0, "close after retry");
}

/* ---------------- 4. 接收路径 ---------------- */

static void test_rx(void)
{
    arkrtos_dev_hdr_t *p;
    uint8 buf[8];

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "open for the RX test");
    if(p == 0)
    {
        return;
    }
    g_dev = as_dev(p);

    /* RXNE：中断读 DR，把字节交进接收环。 */
    USART1->DR = (uint32)'A';
    USART1->SR = USART_SR_RXNE;
    USART1_IRQHandler();
    ck_i(dv_read(p, buf, 1), 1, "one received byte is readable");
    ck_i((long)buf[0], (long)'A', "the byte is the one DR held");

    /* ORE 也必须读 DR：溢出时数据仍然有效，丢掉它等于吞掉一帧。 */
    USART1->DR = (uint32)'B';
    USART1->SR = USART_SR_ORE;
    USART1_IRQHandler();
    ck_i(dv_read(p, buf, 1), 1, "an overrun still delivers the byte");
    ck_i((long)buf[0], (long)'B', "the overrun byte is DR's byte");

    /* 没有标志位的中断什么也不做。 */
    USART1->SR = 0u;
    USART1_IRQHandler();
    ck_i(dv_read(p, buf, 1), 0, "an interrupt without flags receives nothing");

    /* 读得比环里多：返回实际字节数，不假装读满。 */
    USART1->DR = (uint32)'x';
    USART1->SR = USART_SR_RXNE;
    USART1_IRQHandler();
    USART1->DR = (uint32)'y';
    USART1->SR = USART_SR_RXNE;
    USART1_IRQHandler();
    ck_i(dv_read(p, buf, 8), 2, "read returns what is actually buffered");
    ck_i((long)buf[0], (long)'x', "first buffered byte");
    ck_i((long)buf[1], (long)'y', "second buffered byte");

    ck_i(dv_close(p), 0, "close after the RX test");
}

/* ---------------- 5. 发送路径 ---------------- */

static void test_tx(void)
{
    arkrtos_dev_hdr_t *p;
    uint8 msg[2];
    uint8 buf[4];

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "open for the TX test");
    if(p == 0)
    {
        return;
    }
    g_dev = as_dev(p);

    /* 写路径：只入环 + 踢 TXEIE，字节一个都不直接写 DR。 */
    msg[0] = (uint8)'a';
    msg[1] = (uint8)'b';
    USART1->DR = 0x5Au;                /* 哨兵：证明 write 没有直接写 DR */
    ck_i(dv_write(p, msg, 2), 2, "write accepts both bytes");
    ck_i((long)g_dev->tx_idle, 0, "write clears tx_idle");
    ck((USART1->CR1 & USART_CR1_TXEIE) != 0u, "write arms TXEIE");
    ck_u(USART1->DR, 0x5Au, "write does not write DR itself");
    ck_i((long)host_primask_shadow, 0, "write restores the interrupt mask");

    /* 临界区：进入时仍是 idle、退出时已经踢好——中间态没有缝隙。 */
    ck_i(g_hook_n, 2, "write enters and leaves the critical section once");
    ck_i(g_state_enter_idle, 1, "at mask time the pipe still looks idle");
    ck_u(g_state_enter_txe, 0u, "at mask time TXEIE is still clear");
    ck_i(g_state_exit_idle, 0, "at unmask time tx_idle is already clear");
    ck_u(g_state_exit_txe, (unsigned long)USART_CR1_TXEIE,
         "at unmask time TXEIE is already armed");

    /* 中断逐个出队。 */
    USART1->SR = USART_SR_TXE;
    USART1_IRQHandler();
    ck_u(USART1->DR, (unsigned long)'a', "the first byte goes out");
    ck((USART1->CR1 & USART_CR1_TXEIE) != 0u, "TXEIE stays armed while data remains");
    ck((USART1->CR1 & USART_CR1_TCIE) == 0u, "TCIE is not armed while data remains");

    USART1->SR = USART_SR_TXE;
    USART1_IRQHandler();
    ck_u(USART1->DR, (unsigned long)'b', "the second byte goes out");

    /* 环空了：关 TXEIE、开 TCIE、回 idle，并且不再动 DR。 */
    USART1->SR = USART_SR_TXE;
    USART1_IRQHandler();
    ck_u(USART1->DR, (unsigned long)'b', "an empty pipe does not rewrite DR");
    ck((USART1->CR1 & USART_CR1_TXEIE) == 0u, "TXEIE is cleared when the pipe empties");
    ck((USART1->CR1 & USART_CR1_TCIE) != 0u, "TCIE is armed to wait for the last stop bit");
    ck_i((long)g_dev->tx_idle, 1, "tx_idle is set when the pipe empties");

    /* TC：idle 时才清 TCIE；非 idle 时不能清（那是上一笔传输的尾巴）。 */
    USART1->SR = USART_SR_TC;
    USART1_IRQHandler();
    ck((USART1->CR1 & USART_CR1_TCIE) == 0u, "TC clears TCIE when idle");

    g_dev->tx_idle = 0;                /* 手工造出「传输中」的状态 */
    USART1->CR1 |= USART_CR1_TCIE;
    USART1->SR = USART_SR_TC;
    USART1_IRQHandler();
    ck((USART1->CR1 & USART_CR1_TCIE) != 0u, "TC leaves TCIE alone while a transfer is live");
    g_dev->tx_idle = 1;

    /* 接收与发送同一次中断：两条分支都要跑，不能互相饿死。 */
    USART1->SR = 0u;
    USART1->DR = 0u;
    msg[0] = (uint8)'C';
    ck_i(dv_write(p, msg, 1), 1, "queue one byte for the combined test");
    USART1->DR = (uint32)'r';          /* 硬件给的接收字节 */
    USART1->SR = USART_SR_RXNE | USART_SR_TXE;
    USART1_IRQHandler();
    ck_u(USART1->DR, (unsigned long)'C', "the TX branch runs while RX is pending");
    ck_i(dv_read(p, buf, 1), 1, "the RX branch runs while TX is pending");
    ck_i((long)buf[0], (long)'r', "the received byte is the one the line gave");

    ck_i(dv_close(p), 0, "close after the TX test");
}

/* ---------------- 6. 发送环满 ---------------- */

static void test_tx_full(void)
{
    arkrtos_dev_hdr_t *p;
    uint8 chunk[64];
    int i;
    int got;
    int total;

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "open for the overflow test");
    if(p == 0)
    {
        return;
    }
    g_dev = as_dev(p);

    memset(chunk, (int)'z', sizeof(chunk));
    total = 0;
    /* UART_PIPE_SIZE 是缓冲区字节数：FIFO 头占 8 字节（magic/rd/wt/size），
     * 环里再留一个空位区分满与空，所以真正装得下的是 1024-8-1 = 1015。
     * 这个 -1 不好从容量常量上看出来，用断言把它钉住。 */
    for(i = 0; i < 40; i++)
    {
        got = dv_write(p, chunk, (int32)sizeof(chunk));
        if(got <= 0)
        {
            break;
        }
        total += got;
    }
    ck_i(total, UART_PIPE_SIZE - 9,
         "the transmit pipe accepts UART_PIPE_SIZE - 9 bytes");
    ck_i(dv_write(p, chunk, 1), 0, "a full pipe accepts zero more bytes");
    ck(g_dev->tx_idle == 0, "the pipe is not idle while full");
    ck(arkrtos_fifo_write_refused() > 0u, "the FIFO counts the refused byte");

    ck_i(dv_close(p), 0, "close after the overflow test");
}

/* ---------------- 7. close 的诚实性 ---------------- */

static void test_close(void)
{
    arkrtos_dev_hdr_t *p;
    uint8 buf[4];
    uint8 b = (uint8)'q';

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "open for the close test");
    if(p == 0)
    {
        return;
    }
    g_dev = as_dev(p);

    /* 先留一个接收字节，证明 close 之后它不会冒充「刚收到的数据」。 */
    USART1->DR = (uint32)'k';
    USART1->SR = USART_SR_RXNE;
    USART1_IRQHandler();
    USART1->CR1 |= (USART_CR1_TXEIE | USART_CR1_TCIE | USART_CR1_UE);
    g_dev->tx_idle = 0;

    ck_i(dv_close(p), 0, "close returns 0");
    ck_i((long)g_dev->opened, 0, "close clears opened");
    ck((USART1->CR1 & USART_CR1_TXEIE) == 0u, "close clears TXEIE");
    ck((USART1->CR1 & USART_CR1_TCIE) == 0u, "close clears TCIE");
    ck((USART1->CR1 & USART_CR1_RXNEIE) == 0u, "close clears RXNEIE");
    ck((USART1->CR1 & USART_CR1_UE) == 0u, "close clears UE");
    ck_i((long)g_dev->tx_idle, 1, "close returns tx_idle to its power-on value");

    ck_i(dv_read(p, buf, 1), -1, "read on a closed port is -1, not stale bytes");
    ck_i(dv_write(p, &b, 1), -1, "write on a closed port is -1, not a silent zero");
    ck_i(dv_close(p), 0, "close is idempotent");
}

/* ---------------- 8. 关后重开 ---------------- */

static void test_reopen(void)
{
    arkrtos_dev_hdr_t *p;
    uint8 buf[4];

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "open after close succeeds");
    ck_i(g_uart_init_calls, 1, "reopen really re-inits the UART");
    ck((USART1->CR1 & USART_CR1_RXNEIE) != 0u, "reopen re-enables RXNEIE");
    ck((USART1->CR1 & USART_CR1_TXEIE) == 0u, "reopen leaves TXEIE clear");
    ck((USART1->CR1 & USART_CR1_TCIE) == 0u, "reopen clears a stale TCIE");
    ck_i((long)as_dev(p)->tx_idle, 1, "reopen starts idle");
    ck_i(dv_read(p, buf, 1), 0, "the reopened port starts with an empty receive pipe");

    ck_i(dv_close(p), 0, "close after the reopen test");
}

/* ---------------- 9. ctrl 不再静默成功 ---------------- */

static void test_ctrl(void)
{
    arkrtos_dev_hdr_t *p;
    uint32_t cr1_before;

    reset_all();
    p = dv_open(UART_DEV_TTYS0, 115200u);
    ck(p != 0, "open for the ctrl test");
    if(p == 0)
    {
        return;
    }

    cr1_before = USART1->CR1;
    ck_i(dv_ctrl(p, 0u, 0u), -1, "ctrl(0) is -1: the driver implements no command");
    ck_i(dv_ctrl(p, 0xDEADu, 0xFFFFu), -1, "ctrl(unknown) is -1");
    ck_u(USART1->CR1, (unsigned long)cr1_before, "ctrl touches no register");

    ck_i(dv_close(p), 0, "close after the ctrl test");
}

int main(void)
{
    if(map_window() == 0)
    {
        printf("DRVUART-HOST FAIL cannot map 0x40010000\n");
        return 1;
    }

    /* 顺序有意义：open 的静态态在进程里不复位，所以先打开再关，后面每个
     * 用例都从「关着的端口」起步（test_open 除外，它自己负责开）。 */
    test_guards();
    test_open();
    test_init_failure();
    test_rx();
    test_tx();
    test_tx_full();
    test_close();
    test_reopen();
    test_ctrl();

    if(g_fails != 0)
    {
        printf("DRVUART-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }
    printf("DRVUART-HOST OK %d checks\n", g_checks);
    return 0;
}
