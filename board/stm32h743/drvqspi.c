/**
* @file drvqspi.c
* @brief STM32H743 board level QSPI NOR (W25Q64) bring-up - see drvqspi.h
* @details 只做"能不能读到这颗芯片"的取证：JEDEC ID、状态寄存器、间接读、
*          内存映射读、QUADSPI 寄存器。没有擦写，没有文件系统。
*
*          引脚（来自 dts/stm32h743.dts 的记录，不是从数据手册推的）：
*            NCS = PG6  (AF10)
*            CLK = PF10 (AF9)
*            IO0 = PF8  (AF9)
*            IO1 = PF9  (AF9)
*            IO2 = PF7  (AF9)
*            IO3 = PF6  (AF9)
*
*          时钟：QUADSPI 挂在 AHB3 上；分频取 (8+1)=9 分频，把 SCK 压到
*          几十 MHz 量级——bring-up 阶段宁可慢，也不要把"信号完整性问题"
*          伪装成"驱动写错了"。
*
*          为什么直连寄存器、不用 HAL_QSPI：这块 H7 的 HAL QSPI 会顺带拖进
*          MDMA（stm32h7xx_hal_qspi.c 里直接调 HAL_MDMA_Abort），而这里只需要
*          "发一条命令、收 3 个字节"，MDMA 的源码/中断/时钟全都是净增负担，
*          并且链接期就报 undefined reference。寄存器级实现只碰 CR/DCR/SR/
*          FCR/DLR/CCR/AR/DR 八个寄存器，与 HAL 的写入顺序逐条对齐（见下
*          各处注释），所以两条路线在器件侧看到的总线波形应当一致。
*
* @author xw
* @date 2026.11.05
*/

#include "drvqspi.h"
#include "arkrtos_config.h"
#include "arkrtos_log.h"
#include "ark_shell.h"              /* ark_shell_printf for the nor command */

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG          /* stm32h7xx.h via the board config */
#endif

#if (ARKRTOS_USE_QSPI == 1)

/* ============================================================
 * 常量
 * ============================================================ */

#define NOR_MM_BASE          (0x90000000u)  /* QUADSPI 的内存映射窗口         */
#define NOR_PRESCALER        (8u)           /* SCK = f_qspi / (8+1)           */
#define NOR_FIFO_THRESHOLD   (4u)           /* 字节数；寄存器字段是 n-1       */
#define NOR_CS_HIGH_CYCLES   (4u)           /* 片选保持高电平的周期数 1..8    */

/* CSHT 编码：0 => 1 周期 ... 7 => 8 周期（RM0433 DCR.CSHT）。 */
#define NOR_CSHT_FIELD(cyc)  ((uint32)((cyc) - 1u))

/* FSIZE 编码：字段值 n 表示 2^(n+1) 字节；8 MiB => 2^23 => 23。 */
#define NOR_FSIZE_FIELD      (23u)

/* ADSIZE 编码：00=8 位、01=16 位、10=24 位、11=32 位（RM0433 CCR.ADSIZE）。
 * 这里曾经写成 3（32 位），对 8 MiB 的器件是错的：24 位地址是 8 MiB 的
 * 寻址宽度。 */
#define NOR_ADSIZE_24BITS    (2u)

/* 一次命令最多等这么多轮轮询。一次 32 字节的间接读在 9 分频下是几微秒量级，
 * 这里留三个数量级的余量：目的不是"等得够久"，而是"卡死时能报错而不是挂住
 * 整个启动流程"。 */
#define NOR_POLL_LIMIT       (500000u)
#define NOR_ABORT_LIMIT      (2000000u)

/* QUADSPI 引脚复用号：逐根写，不按下标推断。
 *
 * 来源 = ST 自家 CubeMX 器件库 GPIO-STM32H747_gpio_v1_0_Modes.xml 的
 * GPIO_Pin 分节（H743/H747 共用同一个 GPIO IP，映射一致）：
 *
 *   PF6  QUADSPI_BK1_IO3  AF9
 *   PF7  QUADSPI_BK1_IO2  AF9
 *   PF8  QUADSPI_BK1_IO0  AF10   <-- 与 IO2/IO3/CLK 不同号
 *   PF9  QUADSPI_BK1_IO1  AF10   <-- 同上
 *   PF10 QUADSPI_CLK      AF9
 *   PG6  QUADSPI_BK1_NCS  AF10
 *
 * 另外 PF6/PF7 在单线模式下不是数据线，而是 /HOLD 与 /WP：它们必须钉成
 * GPIO 输出高，见 nor_gpio_out_high() 的注释（否则芯片只读不写）。
 *
 * 这里曾经把 PF6..PF10 整组写成 AF9：现象是外设寄存器全对、传输能完成、
 * 读回全 0x00，且 EXTI 抓不到 CLK/NCS 的任何边沿——因为 IO0/IO1 两根数据
 * 线根本没接到 QUADSPI 上，外设侧等不到数据就整笔超时。教训：同一外设在
 * 同一端口上也可能用两个不同的复用号，绝不能按端口推断。 */
#define NOR_AF_IO23          (9u)           /* PF6 / PF7           */
#define NOR_AF_IO0           (10u)          /* PF8                 */
#define NOR_AF_IO1           (10u)          /* PF9                 */
#define NOR_AF_CLK           (9u)           /* PF10                */
#define NOR_AF_NCS           (10u)          /* PG6                 */

#define NOR_CMD_WRITE_ENABLE   (0x06u)
#define NOR_CMD_READ_STATUS1   (0x05u)
#define NOR_CMD_PAGE_PROGRAM   (0x02u)
#define NOR_CMD_SECTOR_ERASE   (0x20u)      /* 4 KiB，W25Q64 的最小擦除单位 */

#define NOR_STATUS_WIP         (0x01u)      /* write in progress   */
#define NOR_STATUS_WEL         (0x02u)      /* write enable latch  */

#define NOR_PAGE_SIZE          (256u)
#define NOR_SECTOR_SIZE        (4096u)

#define NOR_TMO_POLL_MS        (10000u)     /* 扇区擦除典型 45 ms，留足余量 */
#define NOR_SCRATCH_SECTOR     (ARKRTOS_NOR_SIZE - NOR_SECTOR_SIZE)

/** @brief 把一个毫秒超时换算成 DWT 周期数（本板 DWT 计的是 CPU 周期） */
static uint32 nor_ms_to_cyc(uint32 ms)
{
    return (uint32)((SystemCoreClock / 1000u) * ms);
}


/* ============================================================
 * 内部状态
 * ============================================================ */

/* 前置声明：多个取证函数要在收尾里中止传输，而 nor_abort 定义在后面。 */
static int32 nor_abort(void);

/* 前置声明：读路径在文件前部就要用位敲通道（定义在中部）。 */
static void nor_bb_begin(void);
static void nor_bb_end(void);
static void nor_bb_read_stream(uint32 addr, uint8 *buf, uint32 len);

static uint32 g_nor_inited;         /* 1 = 时钟/引脚/CR/DCR 已配置          */
static uint32 g_nor_ready;          /* 1 = ID 读到且等于 W25Q64             */
static uint32 g_nor_id;             /* 最近一次读到的 JEDEC ID              */
static uint32 g_nor_sr1;            /* 最近一次读到的状态寄存器 1           */
static int32  g_nor_last_err;       /* 0 = 无                               */

/* ============================================================
 * 引脚与时钟
 * ============================================================ */

static void nor_delay_loops(uint32 n)
{
    /* 板级 init 早于 SysTick，不能调用 port 的延时时基。这里按周期粗算：
     * 多等无害（NOR 上电后只需要 tRES1 = 几十微秒），少等才会毁掉第一帧。 */
    volatile uint32 i;

    for(i = 0u; i < n; i++)
    {
    }
}

static void nor_delay_us(uint32 us)
{
    nor_delay_loops(us * 480u);
}

/** @brief 把 DWT 周期计数器打开并清零（只用于量"一次传输到底花了多久"） */
static void nor_cyc_start(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    DWT->CYCCNT = 0u;
}

/** @brief 读 DWT 周期计数（480 MHz 下 1 us = 480 个周期） */
static uint32 nor_cyc_now(void)
{
    return (uint32)DWT->CYCCNT;
}


static void nor_gpio_pin(GPIO_TypeDef *port, uint32 pin, uint32 af)
{
    uint32 shift = (pin & 7u) * 4u;

    port->MODER    = (port->MODER & ~(3uL << (pin * 2u))) | (2uL << (pin * 2u));
    port->OTYPER  &= ~(1uL << pin);
    port->OSPEEDR |= (3uL << (pin * 2u));
    /* 上拉：CLK/IO 在器件未应答时应当读到高电平，这样"没接上"的表现是
     * 0xFFFFFF 而不是随机的 0x000000，两种失败在串口上就分得开。 */
    port->PUPDR    = (port->PUPDR & ~(3uL << (pin * 2u))) | (1uL << (pin * 2u));

    if(pin < 8u)
    {
        port->AFR[0] = (port->AFR[0] & ~(0xFuL << shift)) | ((uint32)af << shift);
    }
    else
    {
        port->AFR[1] = (port->AFR[1] & ~(0xFuL << shift)) | ((uint32)af << shift);
    }
}

/**
* @brief 把一根引脚配成推挽输出并置高
* @details 为什么需要它：W25Q64 是标准 SPI + 双/四线共引脚封装，引脚 3 在单线
*          模式下是 /WP、引脚 7 是 /HOLD。QUADSPI 在单线模式里不驱动 IO2/IO3，
*          但 pad 仍在 AF 输出状态——实测 /WP 被拉低，芯片于是"允许读、拒绝
*          写"：0x9F/0x03/0x05 全对，而 WREN 之后 WEL 永远读回 0，扇区擦除
*          一字不动。所以单线模式下必须把这两根钉成 GPIO 输出高电平。
*/
static void nor_gpio_out_high(GPIO_TypeDef *port, uint32 pin)
{
    uint32 shift = (pin & 7u) * 4u;
    uint32 idx   = (pin >> 3) & 1u;

    port->BSRR = (1u << pin);                       /* 先把输出锁存写成 1 */
    port->OTYPER &= ~(1u << pin);                   /* 推挽             */
    port->OSPEEDR |= (3u << (pin * 2u));            /* 最高速度         */
    port->PUPDR &= ~(3u << (pin * 2u));             /* 不需要内部上下拉 */
    port->AFR[idx] &= ~(0xFu << shift);             /* 与 AF 无关，清掉 */
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (1u << (pin * 2u));
}

/**
* @brief 打开 QUADSPI 与 GPIOF/GPIOG 的时钟，配置 6 根引脚
* @note 复用号是芯片事实（PF6/7/10 = AF9、PF8/9 = AF10、PG6 = AF10，逐根见文件头
*       NOR_AF_* 的注释），不是可调参数；写错的表现
*       是"读到 0x000000 / 0xFFFFFF"，与"芯片没焊"极像，所以每一根都单独
*       写出来，不靠循环下标推断。
*/
static void nor_pins_init(void)
{
    RCC->AHB4ENR |= RCC_AHB4ENR_GPIOFEN | RCC_AHB4ENR_GPIOGEN;
    (void)RCC->AHB4ENR;

    /* 单线模式：IO3(引脚7)=/HOLD、IO2(引脚3)=/WP，必须由软件钉成高电平。
     * /WP 被拉低时芯片只允许读、WREN 后 WEL 永远不置位（实测见 nor verify）。 */
    nor_gpio_out_high(GPIOF, 6u);           /* IO3 -> /HOLD = 1 */
    nor_gpio_out_high(GPIOF, 7u);           /* IO2 -> /WP   = 1 */
    nor_gpio_pin(GPIOF, 8u,  NOR_AF_IO0);   /* IO0 (DI)  */
    nor_gpio_pin(GPIOF, 9u,  NOR_AF_IO1);   /* IO1 (DO)  */
    nor_gpio_pin(GPIOF, 10u, NOR_AF_CLK);   /* CLK       */
    nor_gpio_pin(GPIOG, 6u,  NOR_AF_NCS);   /* NCS       */
}

/**
* @brief 把 6 根引脚整体切到指定的复用号（PF* 一个、PG6 一个）
* @details 真实映射是 PF6/7/10=AF9、PF8/9=AF10、PG6=AF10，**两组不同号**，
*          所以这个"整组同号"的入口已经无法表达正确的组合——它只留作反证：
*          把整组打到 AF10 或 AF11，能复现"传输不完成"；把整组打到 AF9，
*          能复现"传输完成但零数据"。要让芯片真的应答，请走 nor_pins_init()。
*          这个入口只为取证存在，不许它参与正常启动路径。
*/
static void nor_set_af(uint32 af_pf, uint32 af_pg)
{
    nor_gpio_pin(GPIOF, 6u,  af_pf);
    nor_gpio_pin(GPIOF, 7u,  af_pf);
    nor_gpio_pin(GPIOF, 8u,  af_pf);
    nor_gpio_pin(GPIOF, 9u,  af_pf);
    nor_gpio_pin(GPIOF, 10u, af_pf);
    nor_gpio_pin(GPIOG, 6u,  af_pg);
}

/**
* @brief 打开 QUADSPI 外设时钟、复位一次、写 CR/DCR，并把回读值核对一遍
* @return 0 = 寄存器与写入值一致；负值为 ARKRTOS_NOR_ERR_x
*/
static int32 nor_reg_init(void)
{
    uint32 cr;
    uint32 dcr;

    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;
    (void)RCC->AHB3ENR;

    /* 复位一次，避免上一次运行（热复位时外设不复位）留下的 CCR/DLR 干扰。 */
    RCC->AHB3RSTR |= RCC_AHB3RSTR_QSPIRST;
    nor_delay_us(2u);
    RCC->AHB3RSTR &= ~RCC_AHB3RSTR_QSPIRST;
    nor_delay_us(2u);

    /* CR：先关 EN，把分频/阈值写好，最后再开 EN。
     * PRESCALER 只在 EN=1 之外可写（RM 要求修改配置前 BUSY=0），所以顺序
     * 是：EN=0 -> 配置 -> EN=1，而不是读改写里顺手把 EN 带上去。 */
    QUADSPI->CR = 0u;
    QUADSPI->DCR = 0u;
    (void)QUADSPI->CR;

    cr = ((uint32)NOR_PRESCALER << QUADSPI_CR_PRESCALER_Pos) |
         ((uint32)(NOR_FIFO_THRESHOLD - 1u) << QUADSPI_CR_FTHRES_Pos);
    dcr = ((uint32)NOR_FSIZE_FIELD << QUADSPI_DCR_FSIZE_Pos) |
          (NOR_CSHT_FIELD(NOR_CS_HIGH_CYCLES) << QUADSPI_DCR_CSHT_Pos);

    QUADSPI->DCR = dcr;
    QUADSPI->CR  = cr;                  /* EN 仍为 0：先落配置 */
    QUADSPI->CR  = cr | QUADSPI_CR_EN;
    nor_delay_us(1u);

    /* 回读：写进去的和读出来的一致，才算"配置这件事成立了"。不一致时不要
     * 继续发命令——那只会把"寄存器写不进去"伪装成"芯片不应答"。 */
    if((QUADSPI->CR != (cr | QUADSPI_CR_EN)) || (QUADSPI->DCR != dcr))
    {
        return ARKRTOS_NOR_ERR_HAL;
    }

    g_nor_inited = 1u;

    /* 预热一条命令：实测"整块复位 QUADSPI 之后的第一条命令"会被吞掉
     * （CCR 写下去 SR 一直是 0、BUSY 不置位），第二条起才正常。这里发一条
     * 结果无关紧要的 0x05，把这一下吸收掉，别让第一次真正的读去做牺牲品。
     * 失败也继续：真正的判据是随后那条命令，不是这条。 */
    {
        uint32 n;

        QUADSPI->DLR = 0u;
        QUADSPI->AR  = 0u;
        QUADSPI->ABR = 0u;
        QUADSPI->CCR = (0x05u << QUADSPI_CCR_INSTRUCTION_Pos) |
                       (1u << QUADSPI_CCR_IMODE_Pos) |
                       (1u << QUADSPI_CCR_DMODE_Pos) |
                       (1u << QUADSPI_CCR_FMODE_Pos);
        for(n = 0u; n < 100000u; n++)
        {
            if((QUADSPI->SR & (QUADSPI_SR_TCF | QUADSPI_SR_TEF)) != 0u)
            {
                break;
            }
        }
        while((QUADSPI->SR & QUADSPI_SR_FLEVEL_Msk) != 0u)
        {
            (void)*(volatile uint8 *)&QUADSPI->DR;
        }
        QUADSPI->FCR = QUADSPI_FCR_CTCF | QUADSPI_FCR_CTEF |
                       QUADSPI_FCR_CTOF | QUADSPI_FCR_CSMF;
    }
    return 0;
}

/* ============================================================
 * 位敲（bit-bang）兜底探针
 * ------------------------------------------------------------
 * 为什么要有它：QUADSPI 走完一条命令、数据阶段也真的收到了字节，但收到的是
 * 0x000000 —— 这时有两种完全不同的原因：外设/配置写错，或者这颗芯片根本不
 * 应答。两者在"读到的值"上无法区分，能区分它们的只有"换一条完全独立的通路
 * 再问一次"。位敲把 CLK/DI/DO/CS 降级成普通 GPIO，用软件打 SPI 时序：
 *     - 位敲读到 0xEF4017 ⇒ 芯片在、引脚对，问题在 QUADSPI 配置侧；
 *     - 位敲也读到 0x00/0xFF ⇒ 问题在芯片/供电/连线上，与 QUADSPI 无关。
 * 速率只有 ~100 kHz（每半周期 1 us），比 QUADSPI 慢两个数量级，所以"时序
 * 太快"这条原因在它这里不成立。
 * ============================================================ */

/** @brief 把一根 QSPI 引脚配成普通 GPIO（1 = 输出，0 = 输入） */
static void nor_bb_pin_cfg(GPIO_TypeDef *port, uint32 pin, uint32 is_output)
{
    uint32 shift = pin * 2u;

    port->OTYPER  &= ~(1uL << pin);
    port->OSPEEDR |= (3uL << shift);
    port->PUPDR    = (port->PUPDR & ~(3uL << shift)) | (1uL << shift);
    port->MODER    = (port->MODER & ~(3uL << shift)) |
                     ((is_output != 0u) ? (1uL << shift) : 0u);
}

/** @brief SPI mode 0 收发一个字节（MSB 先出，上升沿采样） */
static uint8 nor_bb_xfer(uint8 out)
{
    uint8 in = 0u;
    uint32 i;

    for(i = 0u; i < 8u; i++)
    {
        if((out & 0x80u) != 0u)
        {
            GPIOF->BSRR = (1uL << 8);               /* DI 高 */
        }
        else
        {
            GPIOF->BSRR = (1uL << (8u + 16u));      /* DI 低 */
        }
        out = (uint8)(out << 1);
        nor_delay_us(1);

        GPIOF->BSRR = (1uL << 10);                  /* CLK 上升沿：器件在此采样 */
        nor_delay_us(1);
        in = (uint8)((in << 1) |
                     (((GPIOF->IDR & (1uL << 9)) != 0u) ? 1u : 0u));
        GPIOF->BSRR = (1uL << (10u + 16u));         /* CLK 下降沿 */
        nor_delay_us(1);
    }
    return in;
}

/**
* @brief 用 GPIO 位敲读一次 JEDEC ID（0x9F），读完把引脚与外设都还回去
* @return 0 = 位敲流程走完（注意：这不代表芯片应答，看 raw 才是）
* @note 唯一目的就是"换一条独立通路再问一次"，所以它不做重试、不做判据。
*/
/** @brief 位敲模式进出：把 6 根引脚从 QUADSPI 手上接管/交还 */
static void nor_bb_enter(void)
{
    QUADSPI->CR &= ~QUADSPI_CR_EN;
    nor_delay_us(2);
    nor_bb_pin_cfg(GPIOF, 10u, 1u);                 /* CLK */
    nor_bb_pin_cfg(GPIOF, 8u, 1u);                  /* DI  */
    nor_bb_pin_cfg(GPIOF, 9u, 0u);                  /* DO  */
    nor_bb_pin_cfg(GPIOF, 7u, 0u);                  /* IO2(/WP)  — 输入即可 */
    nor_bb_pin_cfg(GPIOF, 6u, 0u);                  /* IO3(/HOLD) — 输入即可 */
    nor_bb_pin_cfg(GPIOG, 6u, 1u);                  /* NCS */
    GPIOF->BSRR = (1uL << (10u + 16u));             /* CLK 低（mode 0） */
    GPIOG->BSRR = (1uL << 6);                       /* CS 高 */
    nor_delay_us(2);
}

/** @brief 把引脚交回 QUADSPI 并让它重新起作用 */
static void nor_bb_leave(void)
{
    nor_pins_init();
    g_nor_inited = 0u;
    (void)nor_reg_init();
}

/**
* @brief 位敲发一条命令（可选读回 outlen 字节）
* @details 它是整个驱动的"外部见证人"：位敲只依赖 GPIO，不依赖 QUADSPI 的
*          任何配置。外设说的话与它对不上时，以位敲为准。
*/
static void nor_bb_cmd(uint8 cmd, uint8 *out, uint32 outlen)
{
    uint32 i;

    GPIOG->BSRR = (1uL << (6u + 16u));              /* CS 低 */
    nor_delay_us(2);
    (void)nor_bb_xfer(cmd);
    for(i = 0u; i < outlen; i++)
    {
        out[i] = nor_bb_xfer(0x00u);
    }
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << 6);                       /* CS 高 */
    nor_delay_us(2);
}

static int nor_bitbang_id(uint8 raw[3])
{
    uint32 i;

    /* 1. 先关 QUADSPI：EN=1 时外设还在驱动这几根 AF 引脚。 */
    QUADSPI->CR &= ~QUADSPI_CR_EN;
    nor_delay_us(2);

    /* 2. 引脚降级成 GPIO。IO2(PF7)/IO3(PF6) 留作输入：单线模式下它们不参与。 */
    nor_bb_pin_cfg(GPIOF, 10u, 1u);                 /* CLK */
    nor_bb_pin_cfg(GPIOF, 8u, 1u);                  /* DI  */
    nor_bb_pin_cfg(GPIOF, 9u, 0u);                  /* DO  */
    nor_bb_pin_cfg(GPIOF, 7u, 0u);                  /* IO2 */
    nor_bb_pin_cfg(GPIOF, 6u, 0u);                  /* IO3 */
    nor_bb_pin_cfg(GPIOG, 6u, 1u);                  /* NCS */

    /* 3. 空闲电平：CLK 低（mode 0）、CS 高。 */
    GPIOF->BSRR = (1uL << (10u + 16u));
    GPIOG->BSRR = (1uL << 6);
    nor_delay_us(2);

    GPIOG->BSRR = (1uL << (6u + 16u));              /* CS 拉低 */
    nor_delay_us(2);
    (void)nor_bb_xfer(0x9Fu);                       /* JEDEC ID */
    for(i = 0u; i < 3u; i++)
    {
        raw[i] = nor_bb_xfer(0x00u);
    }
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << 6);                       /* CS 释放 */
    nor_delay_us(2);

    /* 4. 引脚交回 QUADSPI 并重新起作用：位敲只借这一次，之后仍走外设。 */
    nor_pins_init();
    g_nor_inited = 0u;
    (void)nor_reg_init();
    return 0;
}

/* ============================================================
 * 寄存器级小工具
 * ============================================================ */

/** @brief 等 BUSY 落下（上一次传输结束） */
static int32 nor_wait_idle(void)
{
    uint32 n;

    for(n = 0u; n < NOR_POLL_LIMIT; n++)
    {
        if((QUADSPI->SR & QUADSPI_SR_BUSY) == 0u)
        {
            return 0;
        }
    }
    return ARKRTOS_NOR_ERR_HAL;
}

/** @brief 清 TCF/TEF，避免上一笔的完成位被当成本笔的 */
/**
* @brief 清掉 QUADSPI 的四个标志位
* @details 只清 TCF/TEF 是不够的：一次失败或超时之后，FIFO 里可能还留着没被
*          取走的字节（SR.FLEVEL>0），下一条读命令的第一个字节就会读到上一条
*          命令遗留的数据，末尾那一个字节则永远等不到——现象是"读全 0/读超时"，
*          看起来像芯片坏了。所以这里把 TOF/SMF 一起清掉，并让调用方先排空 FIFO。
*/
static void nor_clear_flags(void)
{
    QUADSPI->FCR = QUADSPI_FCR_CTCF | QUADSPI_FCR_CTEF |
                   QUADSPI_FCR_CTOF | QUADSPI_FCR_CSMF;
}

/** @brief 把 FIFO 里残留的字节读走（每次取 DR 都会让 FLEVEL 减一） */
static void nor_drain_fifo(void)
{
    uint32 guard = 0u;

    while(((QUADSPI->SR & QUADSPI_SR_FLEVEL_Msk) != 0u) && (guard < 64u))
    {
        (void)*(volatile uint8 *)&QUADSPI->DR;
        guard++;
    }
}

/**
* @brief 把外设退回"干净可发命令"的状态
* @details 用在任何一次失败之后。做法是整块复位 QUADSPI（RCC 复位位）再按配置
*          重来：比逐位修补可靠，因为坏状态的具体形状不止一种（残留 FIFO、
*          ABORT 位没落、CCR 停在半截命令）。复位是幂等的，代价只有微秒级。
*/
static int32 nor_recover(void)
{
    int32 rc;

    if(nor_reg_init() != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    nor_drain_fifo();
    nor_clear_flags();

    rc = nor_wait_idle();
    return (rc == 0) ? 0 : ARKRTOS_NOR_ERR_HAL;
}

/**
* @brief 从 FIFO 取一个字节
* @note 只在 FLEVEL>0 时读 DR：TC 已置但 FIFO 空，说明命令早结束了却少给了
*       数据（引脚/配置问题），这时读 DR 会拿到上一次的残留，属于"错答案"，
*       所以直接报错。
*/
static int32 nor_fifo_get(uint8 *out)
{
    uint32 n;

    for(n = 0u; n < NOR_POLL_LIMIT; n++)
    {
        uint32 sr = QUADSPI->SR;

        if((sr & QUADSPI_SR_TEF) != 0u)
        {
            nor_clear_flags();
            return ARKRTOS_NOR_ERR_HAL;
        }
        if((sr & QUADSPI_SR_FLEVEL_Msk) != 0u)
        {
            *out = *(volatile uint8 *)&QUADSPI->DR;
            return 0;
        }
        if((sr & QUADSPI_SR_TCF) != 0u)
        {
            /* 传输已完成但 FIFO 里没有数据 */
            return ARKRTOS_NOR_ERR_HAL;
        }
    }
    /* 轮询到上限：把现场打出来，否则"读失败"只剩一个 rc=-2。 */
    ark_shell_printf("nor: fifo empty, SR=0x%08X BUSY=%u FLEVEL=%u\r\n",
                     (unsigned)QUADSPI->SR,
                     (unsigned)((QUADSPI->SR & QUADSPI_SR_BUSY) != 0u),
                     (unsigned)((QUADSPI->SR & QUADSPI_SR_FLEVEL_Msk) >>
                                QUADSPI_SR_FLEVEL_Pos));
    return ARKRTOS_NOR_ERR_HAL;
}

/**
* @brief 发一条 1 线间接读命令并收 len 个字节
* @param instr 指令字节（0x9F / 0x05 / 0x03 ...）
* @param addr  24 位地址（has_addr = 0 时不使用）
* @param has_addr 是否带地址段（JEDEC/状态寄存器命令不带）
* @note 写入顺序与 HAL 一致：DLR -> AR -> CCR（写 CCR 触发传输）。
*/
static int32 nor_indirect_read(uint8 instr, uint32 addr, uint32 has_addr,
                              uint8 *buf, uint32 len)
{
    uint32 ccr;
    uint32 i;
    int32 rc;

    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }
    rc = nor_wait_idle();
    if(rc != 0)
    {
        return rc;
    }
    nor_drain_fifo();
    nor_clear_flags();

    QUADSPI->DLR = len - 1u;
    QUADSPI->AR  = (has_addr != 0u) ? addr : 0u;
    QUADSPI->ABR = 0u;

    ccr = ((uint32)instr << QUADSPI_CCR_INSTRUCTION_Pos) |
          (1u << QUADSPI_CCR_IMODE_Pos) |      /* 指令 1 线 */
          (1u << QUADSPI_CCR_DMODE_Pos) |      /* 数据 1 线 */
          (1u << QUADSPI_CCR_FMODE_Pos);       /* 01 = 间接读 */
    if(has_addr != 0u)
    {
        ccr |= (1u << QUADSPI_CCR_ADMODE_Pos) |       /* 地址 1 线 */
               ((uint32)NOR_ADSIZE_24BITS << QUADSPI_CCR_ADSIZE_Pos);
    }
    QUADSPI->CCR = ccr;                     /* 写 CCR = 开始传输 */

    for(i = 0u; i < len; i++)
    {
        rc = nor_fifo_get(&buf[i]);
        if(rc != 0)
        {
            return rc;
        }
    }

    /* 收完数据后等 TC 并清掉：不清的话下一次命令会先看到旧的完成位。 */
    if(nor_wait_idle() != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    nor_drain_fifo();
    nor_clear_flags();
    return 0;
}

/**
* @brief 量一遍"QUADSPI 寄存器访问有多慢"和"同一笔传输在不同分频下花多久"
* @details 为什么需要它：同一笔 0x9F（32 个时钟）在 PRESC=8 与 PRESC=255 下
*          实测耗时几乎相同，这只有在"耗时根本不是 SCK 决定"时才可能。要么读
*          寄存器本身被拖住（说明外设的 kernel clock 与假设不同），要么传输没
*          真正走 SCK。这个子命令把两件事分开量：先用 1000 次 SR 读算出一次
*          寄存器访问的周期数，再在三个分频下量同一笔传输的周期数。
*          判读：传输周期应随 (PRESC+1) 线性变化；若三条一样，问题不在分频。
* @return 0 = 量完
*/
/**
* @brief 同一笔 0x9F 用两种宽度取 DR：8 位逐字节 vs 32 位整字
* @details 为什么需要它：外设确实在按 SCK 时钟跑（周期随分频线性变化），
*          但每个字节读出来都是 0x00。若 32 位整字能读到 0xEF4017，那问题
*          就在"FIFO 读取宽度"这一层，而不是引脚或波形；反之才能把"数据没
*          进来"钉在总线侧。
*/
static int nor_dr_width(void)
{
    uint32 i;
    uint32 w;
    uint8 b;

    /* 第一遍：0x9F，4 个字节，用 32 位整字读一次 */
    if(nor_wait_idle() != 0)
    {
        return -1;
    }
    nor_clear_flags();
    QUADSPI->DLR = 3u;
    QUADSPI->AR  = 0u;
    QUADSPI->ABR = 0u;
    QUADSPI->CCR = (0x9Fu << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (1u << QUADSPI_CCR_IMODE_Pos) |
                   (1u << QUADSPI_CCR_DMODE_Pos) |
                   (1u << QUADSPI_CCR_FMODE_Pos);
    while(((QUADSPI->SR & QUADSPI_SR_TCF) == 0u) &&
          ((QUADSPI->SR & QUADSPI_SR_TEF) == 0u))
    {
    }
    ark_shell_printf("nor: dr32 SR=0x%08X\r\n", (unsigned)QUADSPI->SR);
    w = *(volatile uint32 *)&QUADSPI->DR;
    ark_shell_printf("nor: DR as 32-bit = 0x%08X\r\n", (unsigned)w);
    nor_clear_flags();

    /* 第二遍：同样命令，用 8 位逐字节读 */
    if(nor_wait_idle() != 0)
    {
        return -1;
    }
    nor_clear_flags();
    QUADSPI->DLR = 3u;
    QUADSPI->AR  = 0u;
    QUADSPI->ABR = 0u;
    QUADSPI->CCR = (0x9Fu << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (1u << QUADSPI_CCR_IMODE_Pos) |
                   (1u << QUADSPI_CCR_DMODE_Pos) |
                   (1u << QUADSPI_CCR_FMODE_Pos);
    for(i = 0u; i < 4u; i++)
    {
        b = *(volatile uint8 *)&QUADSPI->DR;
        ark_shell_printf("nor: DR as 8-bit [%u] = 0x%02X\r\n", (unsigned)i,
                         (unsigned)b);
    }
    nor_clear_flags();
    return 0;
}

/**
* @brief 用 QUADSPI 发一条 0x06（Write Enable），再用位敲读 SR1 看 WEL 位
* @details 这是把"写路径"和"读路径"分开的关键实验：
*            - 0x06 没有数据阶段，只要 CS/CLK/DI 三根线真的到了器件，器件就会
*              置 WEL；位敲读回 SR1 时 WEL=1 就证明"外设确实在驱总线"。
*            - 若 WEL 一直是 0，那 QUADSPI 根本没有把命令送到器件上，问题在
*              引脚/外设侧，而不是"读回来的字节为什么是 0"。
*          位敲读 SR1 会临时接管引脚，所以函数末尾把 AF 与 QUADSPI 都还原。
*/
static int nor_write_path_probe(void)
{
    uint8 sr = 0u;
    uint8 cmd_sr = 0x05u;
    uint8 raw[3];

    if(nor_wait_idle() != 0)
    {
        ark_shell_printf("nor: wel probe idle timeout\r\n");
        return -1;
    }
    nor_clear_flags();

    /* 0x06 Write Enable：只有指令阶段，没有地址/数据阶段。 */
    QUADSPI->DLR = 0u;
    QUADSPI->AR  = 0u;
    QUADSPI->ABR = 0u;
    QUADSPI->CCR = (0x06u << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (1u << QUADSPI_CCR_IMODE_Pos) |
                   (0u << QUADSPI_CCR_FMODE_Pos);      /* 间接写 */
    while(((QUADSPI->SR & QUADSPI_SR_TCF) == 0u) &&
          ((QUADSPI->SR & QUADSPI_SR_TEF) == 0u))
    {
    }
    ark_shell_printf("nor: 0x06 sent, SR=0x%08X\r\n", (unsigned)QUADSPI->SR);
    nor_clear_flags();

    /* 位敲读 SR1（0x05）——这一步会把 QUADSPI 关掉并把引脚变成 GPIO。 */
    (void)nor_bitbang_id(raw);          /* 借用它的接管/还原流程 */
    sr = raw[0];                        /* 这里只是占位，下面重新读 */

    /* 真正的 SR1 读：重新接管引脚，发 0x05 */
    QUADSPI->CR &= ~QUADSPI_CR_EN;
    nor_delay_us(2);
    nor_bb_pin_cfg(GPIOF, 10u, 1u);
    nor_bb_pin_cfg(GPIOF, 8u, 1u);
    nor_bb_pin_cfg(GPIOF, 9u, 0u);
    nor_bb_pin_cfg(GPIOF, 7u, 0u);
    nor_bb_pin_cfg(GPIOF, 6u, 0u);
    nor_bb_pin_cfg(GPIOG, 6u, 1u);
    GPIOF->BSRR = (1uL << (10u + 16u));
    GPIOG->BSRR = (1uL << 6);
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << (6u + 16u));
    nor_delay_us(2);
    (void)nor_bb_xfer(cmd_sr);
    sr = nor_bb_xfer(0x00u);
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << 6);
    nor_delay_us(2);

    nor_pins_init();
    g_nor_inited = 0u;
    (void)nor_reg_init();

    ark_shell_printf("nor: bit-bang SR1=0x%02X -> WEL=%u (1 = QUADSPI's 0x06 "
                     "reached the chip)\r\n",
                     (unsigned)sr, (unsigned)((sr & 0x02u) ? 1u : 0u));
    return 0;
}

/**
* @brief 用 EXTI 的 pending 位当"硬件示波器"：传输期间 CLK/NCS/DI 到底有没有动
* @details 前面所有引脚观察都有一个共同缺陷：CPU 在写 CCR 期间被总线挡住，
*          等它回头读 GPIO->IDR 时传输已经结束，于是永远看到 0 个边沿。EXTI 的
*          pending 位由硬件在边沿上置位，不需要 CPU 参与，传输结束后再读即可。
*          这就是"这条线到底通不通"的判定仪器：
*            CLK rising 有 pending ⇒ 外设真的在驱 SCK；
*            NCS falling 有 pending ⇒ 外设真的在拉片选。
*          两者都在、数据却还是 0，问题就只能落在数据输入这根线上。
*/
static int nor_edge_probe(void)
{
    uint32 pr;

    /* SYSCFG 时钟（EXTICR 在这里），APB4 */
    RCC->APB4ENR |= RCC_APB4ENR_SYSCFGEN;
    (void)RCC->APB4ENR;

    /* 线 6 -> PG6；线 8 -> PF8；线 10 -> PF10（端口号 F=5, G=6） */
    SYSCFG->EXTICR[1] = (SYSCFG->EXTICR[1] & ~(0xFuL << 8)) | (6uL << 8);
    SYSCFG->EXTICR[2] = (SYSCFG->EXTICR[2] & ~(0xFuL << 0)) | (5uL << 0);
    SYSCFG->EXTICR[2] = (SYSCFG->EXTICR[2] & ~(0xFuL << 8)) | (5uL << 8);

    /* IMR 也打开：这颗片子上 pending 位是否与 IMR 无关没有实测过，
     * 与其猜，不如把线打开——NVIC 不使能就不会进中断，pending 照样记。 */
    EXTI->IMR1  |= ((1uL << 6) | (1uL << 8) | (1uL << 10));
    EXTI->RTSR1 |= (1uL << 8) | (1uL << 10);                   /* PF8/PF10 上升沿 */
    EXTI->FTSR1 |= (1uL << 6);                                 /* PG6 下降沿   */
    EXTI->PR1    = (1uL << 6) | (1uL << 8) | (1uL << 10);      /* 清 pending   */

    if(nor_wait_idle() != 0)
    {
        return -1;
    }
    nor_clear_flags();
    QUADSPI->DLR = 2u;
    QUADSPI->AR  = 0u;
    QUADSPI->ABR = 0u;
    QUADSPI->CCR = (0x9Fu << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (1u << QUADSPI_CCR_IMODE_Pos) |
                   (1u << QUADSPI_CCR_DMODE_Pos) |
                   (1u << QUADSPI_CCR_FMODE_Pos);

    pr = EXTI->PR1;
    ark_shell_printf("nor: EXTI PR=0x%08X -> CLK(PF10) seen=%u  NCS(PG6) seen=%u  "
                     "DI(PF8) toggled=%u\r\n",
                     (unsigned)pr,
                     (unsigned)((pr >> 10) & 1u),
                     (unsigned)((pr >> 6) & 1u),
                     (unsigned)((pr >> 8) & 1u));
    nor_clear_flags();
    (void)nor_abort();

    /* 仪器自检：把 PF10 收回 GPIO 输出，人为打一个上升沿。若 EXTI 连自己造的
     * 边沿都看不见，那上面那个 0 只能说明仪器坏了，不能说明引脚没动。 */
    QUADSPI->CR &= ~QUADSPI_CR_EN;
    nor_bb_pin_cfg(GPIOF, 10u, 1u);
    GPIOF->BSRR = (1uL << (10u + 16u));
    nor_delay_us(2);
    EXTI->PR1 = (1uL << 10);
    nor_delay_us(2);
    GPIOF->BSRR = (1uL << 10);
    nor_delay_us(2);
    pr = EXTI->PR1;
    ark_shell_printf("nor: self-check: forced PF10 rising -> PR10=%u (%s)\r\n",
                     (unsigned)((pr >> 10) & 1u),
                     (((pr >> 10) & 1u) != 0u) ? "instrument works"
                                               : "INSTRUMENT BLIND - result above is void");

    nor_pins_init();
    g_nor_inited = 0u;
    (void)nor_reg_init();
    return 0;
}

static int nor_bench(void)
{
    static const uint32 presc[3] = {8u, 64u, 255u};
    uint32 i;
    uint32 t1;
    uint32 sr;

    nor_cyc_start();
    for(i = 0u; i < 1000u; i++)
    {
        sr = QUADSPI->SR;
    }
    t1 = nor_cyc_now();
    (void)sr;
    ark_shell_printf("nor: bench 1000x SR reads = %u cycles (%.2f cycles/read)\r\n",
                     (unsigned)t1, (double)t1 / 1000.0);

    for(i = 0u; i < 3u; i++)
    {
        uint32 n = 0u;
        uint8 b;

        if(nor_wait_idle() != 0)
        {
            ark_shell_printf("nor: presc %u idle timeout\r\n", (unsigned)presc[i]);
            continue;
        }
        nor_clear_flags();
        QUADSPI->CR = (QUADSPI->CR & ~(QUADSPI_CR_PRESCALER | QUADSPI_CR_EN)) |
                      (presc[i] << QUADSPI_CR_PRESCALER_Pos) | QUADSPI_CR_EN;

        nor_cyc_start();
        QUADSPI->DLR = 2u;
        QUADSPI->AR  = 0u;
        QUADSPI->ABR = 0u;
        QUADSPI->CCR = (0x9Fu << QUADSPI_CCR_INSTRUCTION_Pos) |
                       (1u << QUADSPI_CCR_IMODE_Pos) |
                       (1u << QUADSPI_CCR_DMODE_Pos) |
                       (1u << QUADSPI_CCR_FMODE_Pos);

        for(;;)
        {
            uint32 s2 = QUADSPI->SR;

            if((s2 & QUADSPI_SR_FLEVEL_Msk) != 0u)
            {
                b = *(volatile uint8 *)&QUADSPI->DR;
                n++;
                (void)b;
            }
            else if((s2 & QUADSPI_SR_TCF) != 0u)
            {
                break;
            }
            else if(nor_cyc_now() > 20000000u)
            {
                break;
            }
        }
        t1 = nor_cyc_now();
        ark_shell_printf("nor: presc %3u -> %8u cycles (%6u us), %u bytes\r\n",
                         (unsigned)presc[i], (unsigned)t1, (unsigned)(t1 / 480u),
                         (unsigned)n);
        nor_clear_flags();
    }
    return 0;
}

/**
* @brief 在一条长读命令期间用 GPIO->IDR 看总线：CLK 到底动没动、NCS 到底拉没拉
* @details 为什么需要它：位敲（一样的三根线、一样的电平）能读到 0xEF4017，
*          QUADSPI 却只能收到 0x000000。这两种结果同时成立时，"引脚不对/芯片不应答"
*          已经被位敲排除了，剩下唯一没被观测的就是"外设到底有没有在驱这几根线"。
*          AF 模式下 GPIOx->IDR 仍然回报引脚真实电平（drvfmc.c 用同一个办法看
*          FMC 输出），所以这个探针不占引脚、不改配置，只是站在旁边看。
* @return 0 = 探针跑完（读数含义在打印里，不在这里判）
*/
static int nor_bus_probe(void)
{
    uint32 i;
    uint32 ncs_low = 0u;
    uint32 clk_edges = 0u;
    uint32 di_high = 0u;
    uint32 do_high = 0u;
    uint32 samples = 0u;
    uint32 bytes = 0u;
    uint32 sr_start;
    uint32 prev;
    uint32 pins;
    uint8 b;

    if(nor_wait_idle() != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    nor_clear_flags();

    /* 一条足够长的 0x03 读：只要外设在驱总线，CLK 必然持续翻转。 */
    QUADSPI->DLR = 4095u;
    QUADSPI->AR  = 0u;
    QUADSPI->ABR = 0u;
    QUADSPI->CCR = (0x03u << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (1u << QUADSPI_CCR_IMODE_Pos) |
                   (1u << QUADSPI_CCR_ADMODE_Pos) |
                   ((uint32)NOR_ADSIZE_24BITS << QUADSPI_CCR_ADSIZE_Pos) |
                   (1u << QUADSPI_CCR_DMODE_Pos) |
                   (1u << QUADSPI_CCR_FMODE_Pos);
    sr_start = QUADSPI->SR;

    prev = GPIOF->IDR;
    for(i = 0u; i < 400000u; i++)
    {
        uint32 idr = GPIOF->IDR;

        pins = GPIOG->IDR;
        if((((idr ^ prev) & (1uL << 10)) != 0u))          /* PF10 = CLK */
        {
            clk_edges++;
        }
        if((idr & (1uL << 8)) != 0u)                      /* PF8 = DI    */
        {
            di_high++;
        }
        if((idr & (1uL << 9)) != 0u)                      /* PF9 = DO    */
        {
            do_high++;
        }
        prev = idr;
        if((pins & (1uL << 6)) == 0u)                     /* PG6 = NCS   */
        {
            ncs_low++;
        }
        samples++;

        if((QUADSPI->SR & QUADSPI_SR_FLEVEL_Msk) != 0u)
        {
            b = *(volatile uint8 *)&QUADSPI->DR;
            bytes++;
            (void)b;
        }
        if((QUADSPI->SR & QUADSPI_SR_TCF) != 0u)
        {
            break;
        }
    }

    ark_shell_printf("nor: drive sr@start=0x%08X loops=%u bytes=%u\r\n",
                     (unsigned)sr_start, (unsigned)samples, (unsigned)bytes);
    ark_shell_printf("nor: clk edges=%u  ncs low=%u/%u  di high=%u  do high=%u\r\n",
                     (unsigned)clk_edges, (unsigned)ncs_low, (unsigned)samples,
                     (unsigned)di_high, (unsigned)do_high);
    ark_shell_printf("nor: SR now=0x%08X (want clk edges > 0 and ncs low = loops)\r\n",
                     (unsigned)QUADSPI->SR);

    (void)nor_abort();
    return 0;
}

/** @brief 中止当前传输（内存映射模式必须先退出，否则下一次命令拿不到总线） */
static int32 nor_abort(void)
{
    uint32 n;

    if((QUADSPI->SR & QUADSPI_SR_BUSY) == 0u)
    {
        nor_clear_flags();
        return 0;
    }

    QUADSPI->CR |= QUADSPI_CR_ABORT;

    for(n = 0u; n < NOR_ABORT_LIMIT; n++)
    {
        if((QUADSPI->SR & QUADSPI_SR_BUSY) == 0u)
        {
            break;
        }
    }
    if(n >= NOR_ABORT_LIMIT)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }

    /* ABORT 位在硬件里完成后自行落下；这里读回确认一次，若仍是 1 就清掉，
     * 免得留在里面让后面每条命令都立刻被中止。 */
    if((QUADSPI->CR & QUADSPI_CR_ABORT) != 0u)
    {
        QUADSPI->CR &= ~QUADSPI_CR_ABORT;
    }
    nor_clear_flags();
    return 0;
}

/* ============================================================
 * 读 ID / 状态
 * ============================================================ */

int32 arkrtos_board_nor_id(uint32 *jedec)
{
    uint8 raw[3];

    if(jedec == 0)
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }
    if(nor_indirect_read(0x9Fu, 0u, 0u, raw, 3u) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }

    *jedec = ((uint32)raw[0] << 16) | ((uint32)raw[1] << 8) | (uint32)raw[2];
    g_nor_id = *jedec;
    return 0;
}

int32 arkrtos_board_nor_status(uint8 *sr1)
{
    if(sr1 == 0)
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }
    if(nor_indirect_read(0x05u, 0u, 0u, sr1, 1u) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    g_nor_sr1 = *sr1;
    return 0;
}

/* ============================================================
 * 两种读：间接（0x03）与内存映射
 * ============================================================ */

int32 arkrtos_board_nor_read(uint32 addr, uint8 *buf, uint32 len)
{
    if((buf == 0) || (len == 0u) || (addr >= ARKRTOS_NOR_SIZE) ||
       ((addr + len) > ARKRTOS_NOR_SIZE))
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }
    /* 数据通路统一走位敲，理由见文件头"为什么读也不走 QUADSPI"。 */
    nor_bb_begin();
    nor_bb_read_stream(addr, buf, len);
    nor_bb_end();
    return 0;
}

int32 arkrtos_board_nor_mm_read(uint32 addr, uint8 *buf, uint32 len)
{
    uint32 ccr;
    uint32 i;
    int32 rc;

    if((buf == 0) || (len == 0u) || (addr >= ARKRTOS_NOR_SIZE) ||
       ((addr + len) > ARKRTOS_NOR_SIZE))
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }
    rc = nor_wait_idle();
    if(rc != 0)
    {
        return rc;
    }
    nor_clear_flags();

    /* 内存映射模式：FMODE=11，DLR 被忽略。AR 在这里只是"起始地址"的存档，
     * 真正的地址由 AXI 总线给出（这就是下面用 NOR_MM_BASE + addr 读的原因，
     * 与 ST 的 HAL_QSPI_MemoryMapped 用法一致）。 */
    QUADSPI->DLR = 0u;
    QUADSPI->AR  = addr;
    QUADSPI->ABR = 0u;
    ccr = (0x03u << QUADSPI_CCR_INSTRUCTION_Pos) |
          (1u << QUADSPI_CCR_IMODE_Pos) |
          (1u << QUADSPI_CCR_ADMODE_Pos) |
          ((uint32)NOR_ADSIZE_24BITS << QUADSPI_CCR_ADSIZE_Pos) |
          (1u << QUADSPI_CCR_DMODE_Pos) |
          (3u << QUADSPI_CCR_FMODE_Pos);
    QUADSPI->CCR = ccr;

    for(i = 0u; i < len; i++)
    {
        buf[i] = *(volatile uint8 *)(NOR_MM_BASE + addr + i);
    }

    /* 一定要退出 MM 模式：留在里面时下一次普通命令会拿不到总线。 */
    rc = nor_abort();
    if(rc != 0)
    {
        return rc;
    }
    return 0;
}

/**
* @brief 一笔间接写：指令（+可选地址，1 线）+ 数据（1 线）
* @param instr 指令字节（0x06 / 0x02 / 0x20 ...）
* @param addr  24 位地址；has_addr = 0 时不发地址段
* @param has_addr 是否带地址段
* @param buf   数据缓冲；len = 0 表示这笔命令没有数据段（如 WREN / SE）
* @param len   数据字节数（<= 256，页内）
* @return 0 = 外设报告传输完成；负值 = 错
* @note 为什么用 8 位写 DR：DCR.ABMODE/DMODE 都是 1 线，FIFO 按字节推进，
*       与读路径一一对应；用 32 位写会在 len 不是 4 的倍数时要额外处理尾字节。
*       观察到的事实：写 CCR 的 AHB 写会一直阻塞到这笔传输结束（与读路径
*       相同），所以发完 CCR 再逐字节写 DR 时，芯片侧已经在等数据了。
*/
static int32 nor_indirect_write(uint8 instr, uint32 addr, uint32 has_addr,
                                const uint8 *buf, uint32 len)
{
    uint32 ccr;
    uint32 i;
    int32 rc;

    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }
    rc = nor_wait_idle();
    if(rc != 0)
    {
        return rc;
    }
    nor_drain_fifo();
    nor_clear_flags();

    QUADSPI->DLR = (len == 0u) ? 0u : (len - 1u);
    QUADSPI->AR  = (has_addr != 0u) ? addr : 0u;
    QUADSPI->ABR = 0u;

    ccr = ((uint32)instr << QUADSPI_CCR_INSTRUCTION_Pos) |
          (1u << QUADSPI_CCR_IMODE_Pos);
    if(has_addr != 0u)
    {
        ccr |= (1u << QUADSPI_CCR_ADMODE_Pos) |
               ((uint32)NOR_ADSIZE_24BITS << QUADSPI_CCR_ADSIZE_Pos);
    }
    if(len != 0u)
    {
        ccr |= (1u << QUADSPI_CCR_DMODE_Pos);
    }
    /* FMODE = 00 即"间接写"，这就是默认值，不必显式置位。 */
    QUADSPI->CCR = ccr;

    for(i = 0u; i < len; i++)
    {
        *(volatile uint8 *)&QUADSPI->DR = buf[i];
    }

    /* 等这笔写结束。
     *
     * 判据为什么不是"等 TCF 一位"：实测（nor er / nor wr 的 SR 打印）
     *   - 读路径结束时 SR 出现 0x00000002（bit1）；
     *   - 写路径结束时 SR 出现 0x00000004（bit2）。
     * ST 的 CMSIS 头把 QUADSPI_SR_TCF 定义在 bit1，而 RM0433 的位名是
     * TC(1) / TCF(2) —— 头文件与手册对不上，两个方向实测落点也不同。
     * 所以这里不押某一位：TCF 置位，或 BUSY 落下来且已经过至少一轮轮询，
     * 都算完成；只有 TEF（bit0）算错。完成之后由调用者用回读数据判真假，
     * 那才是唯一的证据。
     */
    {
        uint32 t0 = nor_cyc_now();
        uint32 limit = nor_ms_to_cyc(NOR_TMO_POLL_MS);
        uint32 seen = 0u;
        uint32 polls = 0u;

        for(;;)
        {
            uint32 sr = QUADSPI->SR;

            polls++;
            if(polls <= 16u)
            {
                seen |= sr & 0x3Fu;
            }
            if((sr & QUADSPI_SR_TEF) != 0u)
            {
                ark_shell_printf("nor: wr TEF instr=%02X CCR=0x%08X SR=0x%08X seen=0x%02X\r\n",
                                 (unsigned)instr, (unsigned)ccr, (unsigned)sr,
                                 (unsigned)seen);
                nor_clear_flags();
                return ARKRTOS_NOR_ERR_HAL;
            }
            if((sr & QUADSPI_SR_TCF) != 0u)
            {
                nor_clear_flags();
                return 0;
            }
            if(((sr & QUADSPI_SR_BUSY) == 0u) && (polls > 3u))
            {
                /* BUSY 已落：外设已经把 CS 释放了，这笔命令发完了。 */
                nor_clear_flags();
                return 0;
            }
            if((nor_cyc_now() - t0) > limit)
            {
                ark_shell_printf("nor: wr timeout instr=%02X CCR=0x%08X SR=0x%08X seen=0x%02X\r\n",
                                 (unsigned)instr, (unsigned)ccr, (unsigned)sr,
                                 (unsigned)seen);
                (void)nor_abort();
                return ARKRTOS_NOR_ERR_HAL;
            }
        }
    }
}

/* ============================================================
 * 写与擦：页编程 / 扇区擦除 / 状态轮询
 * ------------------------------------------------------------
 * 三条硬事实先写在这里，因为每一条都曾经让"写成功"变成假话：
 *   1) 芯片只允许把 1 写成 0。写一块"已是 0x00"的区域再读回来会一模一样，
 *      所以"写完读回一致"必须建立在这块区域**先被擦成 FF** 的前提下；
 *   2) 页编程（0x02）不能跨 256 字节页边界——越界时芯片在页内回绕，
 *      会把本页开头悄悄覆盖掉；
 *   3) 擦除/编程都是"发完命令芯片自己在忙"，必须靠 SR1.WIP 轮询到清零，
 *      中间插一笔别的命令会被芯片忽略（CS 已释放，但内部时序未完成）。
 * 因此本节的写入路径只有一条：write_enable → 立即回读 WEL 确认 →
 * 发命令 → 等 WIP 落 → 由调用者读回比对。WEL 不置位就报错，不假装成功。
 * ============================================================ */

/**
* @brief 读 SR1（0x05），顺带把 WIP/WEL 交给调用者判断
* @return 0 = 读到；负值 = 传输失败
*/
static int32 nor_read_sr1(uint8 *sr)
{
    return nor_indirect_read(NOR_CMD_READ_STATUS1, 0u, 0u, sr, 1u);
}

/**
* @brief 等芯片内部操作结束（SR1.WIP 清零）
* @param tmo_ms 超时毫秒
* @return 0 = 空闲；-1 = 超时或读状态失败
* @note 为什么要等：擦除/编程在 CS 拉高之后仍继续，期间任何新命令都会被
*       芯片丢弃。不等就发下一条命令，读回的就是"上一次的内容"。
*/
static int32 nor_wait_ready(uint32 tmo_ms)
{
    uint32 t0 = nor_cyc_now();
    uint32 limit = nor_ms_to_cyc(tmo_ms);

    for(;;)
    {
        uint8 sr = 0u;

        if(nor_read_sr1(&sr) != 0)
        {
            return -1;
        }
        if((sr & NOR_STATUS_WIP) == 0u)
        {
            g_nor_sr1 = sr;
            return 0;
        }
        if((nor_cyc_now() - t0) > limit)
        {
            g_nor_sr1 = sr;
            return -1;
        }
    }
}

/**
* @brief 发 WREN 并回读确认 WEL 真的置位
* @return 0 = WEL=1；负值 = 命令没到/被丢弃
* @note 少了这一步，后面那次编程会一字不写却"返回成功"，读回还是 FF，
*       现象与"擦除没生效"完全一样，排查会绕很久。
*/
static int32 nor_write_enable(void)
{
    uint8 sr = 0u;

    if(nor_indirect_write(NOR_CMD_WRITE_ENABLE, 0u, 0u, 0, 0u) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    if(nor_read_sr1(&sr) != 0)
    {
        ark_shell_printf("nor: WREN status read failed\r\n");
        return ARKRTOS_NOR_ERR_HAL;
    }
    if((sr & NOR_STATUS_WEL) == 0u)
    {
        ark_shell_printf("nor: WREN did not set WEL (SR1=0x%02X)\r\n", (unsigned)sr);
        return ARKRTOS_NOR_ERR_HAL;
    }
    return 0;
}

/**
* @brief 页编程：一次最多写一页（256B），调用者负责不跨页、且目标已擦
* @return 0 = 芯片报告完成；负值 = 失败
*/
static int32 nor_page_program(uint32 addr, const uint8 *data, uint32 len)
{
    if(nor_write_enable() != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    if(nor_indirect_write(NOR_CMD_PAGE_PROGRAM, addr, 1u, data, len) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    if(nor_wait_ready(NOR_TMO_POLL_MS) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    return 0;
}

/**
* @brief 扇区擦除（4 KiB）
* @param addr 必须是 4 KiB 对齐的地址
* @return 0 = 擦完；负值 = 参数/命令/超时失败
*/
static int32 nor_sector_erase(uint32 addr)
{
    if((addr % NOR_SECTOR_SIZE) != 0u)
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(nor_write_enable() != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    if(nor_indirect_write(NOR_CMD_SECTOR_ERASE, addr, 1u, 0, 0u) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    if(nor_wait_ready(NOR_TMO_POLL_MS) != 0)
    {
        return ARKRTOS_NOR_ERR_HAL;
    }
    return 0;
}

/* ============================================================
 * 位敲写通道（WREN / 页编程 / 扇区擦除 / 状态轮询）
 * ------------------------------------------------------------
 * 为什么写和擦不走 QUADSPI：实测 QUADSPI 发"带地址、无数据段"的命令
 * （0x20 扇区擦除、0xD8 块擦除）之后，SR.BUSY 一直不落、FIFO 里留半个字节，
 * 之后所有读命令都返回 0xFF、SR1 读成 0xFF（MISO 悬空），只有整块复位
 * QUADSPI 才能恢复。这不是配置问题，是"这条命令形态在外设侧没完成"。
 * 而位敲路径在同一颗芯片上每一步都成立（ID、SR1、WREN 全部有独立证据），
 * 擦除只花 32 个时钟、页编程 2 K 个时钟，代价可以接受。
 *
 * 取舍写在明处：读走 QUADSPI（要带宽），写/擦走位敲（要确定性）。
 * 两条通道共用同一组引脚，切换由 nor_bb_begin()/nor_bb_end() 独占完成。
 * ============================================================ */

/** @brief 接管引脚：关 QUADSPI、把 6 根脚变成 GPIO */
static void nor_bb_begin(void)
{
    QUADSPI->CR &= ~QUADSPI_CR_EN;      /* EN=0 后外设放开引脚 */
    nor_delay_us(2);
    nor_bb_enter();                     /* 复用同一套引脚切换（含空闲电平） */
}

/** @brief 交还引脚：恢复 AF 配置并整块复位 QUADSPI（清掉可能的残留状态） */
static void nor_bb_end(void)
{
    nor_bb_leave();                      /* nor_pins_init + nor_reg_init */
    g_nor_inited = 1u;
}

/** @brief 位敲顺序读（0x03 + 24 位地址 + 数据），调用前必须已经 nor_bb_begin() */
static void nor_bb_read_stream(uint32 addr, uint8 *buf, uint32 len)
{
    uint32 i;

    GPIOG->BSRR = (1uL << (6u + 16u));              /* CS 低 */
    nor_delay_us(2);
    (void)nor_bb_xfer(0x03u);
    (void)nor_bb_xfer((uint8)(addr >> 16));
    (void)nor_bb_xfer((uint8)(addr >> 8));
    (void)nor_bb_xfer((uint8)(addr));
    for(i = 0u; i < len; i++)
    {
        buf[i] = nor_bb_xfer(0x00u);
    }
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << 6);                       /* CS 高 */
    nor_delay_us(2);
}

/** @brief 位敲读 SR1 */
static int32 nor_bb_read_sr1(uint8 *sr)
{
    nor_bb_cmd(NOR_CMD_READ_STATUS1, sr, 1u);
    return 0;
}

/** @brief 位敲等 WIP 落（芯片内部操作结束） */
static int32 nor_bb_wait_ready(uint32 tmo_ms)
{
    uint32 t0 = nor_cyc_now();
    uint32 limit = nor_ms_to_cyc(tmo_ms);

    for(;;)
    {
        uint8 sr = 0u;

        (void)nor_bb_read_sr1(&sr);
        if((sr & NOR_STATUS_WIP) == 0u)
        {
            g_nor_sr1 = sr;
            return 0;
        }
        if((nor_cyc_now() - t0) > limit)
        {
            g_nor_sr1 = sr;
            return -1;
        }
    }
}

/** @brief 位敲发 WREN 并回读确认 WEL */
static int32 nor_bb_wren(void)
{
    uint8 sr = 0u;

    nor_bb_cmd(NOR_CMD_WRITE_ENABLE, 0, 0u);
    (void)nor_bb_read_sr1(&sr);
    return ((sr & NOR_STATUS_WEL) != 0u) ? 0 : -1;
}

/** @brief 位敲页编程：写一页（<=256B），目标必须已擦 */
static int32 nor_bb_page_program(uint32 addr, const uint8 *buf, uint32 len)
{
    uint32 i;

    if(nor_bb_wren() != 0)
    {
        ark_shell_printf("nor: bb WREN failed (sr1=0x%02X)\r\n", (unsigned)g_nor_sr1);
        return -1;
    }

    GPIOG->BSRR = (1uL << (6u + 16u));              /* CS 低 */
    nor_delay_us(2);
    (void)nor_bb_xfer(NOR_CMD_PAGE_PROGRAM);
    (void)nor_bb_xfer((uint8)(addr >> 16));
    (void)nor_bb_xfer((uint8)(addr >> 8));
    (void)nor_bb_xfer((uint8)(addr));
    for(i = 0u; i < len; i++)
    {
        (void)nor_bb_xfer(buf[i]);
    }
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << 6);                       /* CS 高 */
    nor_delay_us(2);

    return nor_bb_wait_ready(NOR_TMO_POLL_MS);
}

/** @brief 位敲扇区擦除（0x20，4 KiB） */
static int32 nor_bb_erase_sector(uint32 addr)
{
    if(nor_bb_wren() != 0)
    {
        ark_shell_printf("nor: bb WREN failed (sr1=0x%02X)\r\n", (unsigned)g_nor_sr1);
        return -1;
    }

    GPIOG->BSRR = (1uL << (6u + 16u));              /* CS 低 */
    nor_delay_us(2);
    (void)nor_bb_xfer(NOR_CMD_SECTOR_ERASE);
    (void)nor_bb_xfer((uint8)(addr >> 16));
    (void)nor_bb_xfer((uint8)(addr >> 8));
    (void)nor_bb_xfer((uint8)(addr));
    nor_delay_us(2);
    GPIOG->BSRR = (1uL << 6);                       /* CS 高 */
    nor_delay_us(2);

    return nor_bb_wait_ready(NOR_TMO_POLL_MS);
}

/* ============================================================
 * 对外的写 / 擦 / 往返自检
 * ============================================================ */

int32 arkrtos_board_nor_write(uint32 addr, const uint8 *buf, uint32 len)
{
    uint32 off = addr;
    const uint8 *p = buf;
    uint32 left = len;

    if((buf == 0) || (len == 0u) || (addr >= ARKRTOS_NOR_SIZE) ||
       ((addr + len) > ARKRTOS_NOR_SIZE))
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }

    nor_bb_begin();
    while(left > 0u)
    {
        /* 绝不跨页：芯片在页内回绕，会把本页开头覆盖掉。 */
        uint32 page_left = NOR_PAGE_SIZE - (off & (NOR_PAGE_SIZE - 1u));
        uint32 chunk = (left < page_left) ? left : page_left;
        int32 rc;

        rc = nor_bb_page_program(off, p, chunk);
        if(rc != 0)
        {
            nor_bb_end();
            return ARKRTOS_NOR_ERR_HAL;
        }
        off += chunk;
        p += chunk;
        left -= chunk;
    }
    nor_bb_end();
    return 0;
}

int32 arkrtos_board_nor_erase(uint32 addr, uint32 len)
{
    uint32 off;
    uint32 end;

    if((len == 0u) || ((addr % NOR_SECTOR_SIZE) != 0u) ||
       ((len % NOR_SECTOR_SIZE) != 0u) || (addr >= ARKRTOS_NOR_SIZE) ||
       ((addr + len) > ARKRTOS_NOR_SIZE))
    {
        return ARKRTOS_NOR_ERR_PARAM;
    }
    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }

    nor_bb_begin();
    end = addr + len;
    for(off = addr; off < end; off += NOR_SECTOR_SIZE)
    {
        if(nor_bb_erase_sector(off) != 0)
        {
            nor_bb_end();
            return ARKRTOS_NOR_ERR_HAL;
        }
    }
    nor_bb_end();
    return 0;
}

/** @brief 一块缓冲区里是否全是 value */
static uint32 nor_all_bytes(const uint8 *buf, uint32 len, uint8 value)
{
    uint32 i;

    for(i = 0u; i < len; i++)
    {
        if(buf[i] != value)
        {
            return 0u;
        }
    }
    return 1u;
}

/**
* @brief 用最后 4 KiB 扇区做一次"擦→写→读回→再擦"的往返自检
* @return 0 = 四步全过；负值 = 哪一步先失败
* @details 为什么必须做这一遍才敢注册块设备：JEDEC ID 只能证明"芯片在、
*          引脚通"，证明不了数据通路是双向的。写路径专用的三件事——WREN 的
*          WEL 置位、页编程完成、擦除后回读全 FF——每一条都可能单独失败，
*          而失败现场读起来都像"文件系统坏了"。这里把它们分开验。
*
*          为什么用最后那个扇区：文件系统卷只占 0..4 MiB，尾部扇区不归任何
*          人用；拿它当草稿不会碰到任何数据。**注意这条自检是破坏性的**：
*          它会擦掉 0x7FF000 起的 4 KiB，跑之前不能有东西住在那里。
*/
int32 arkrtos_board_nor_roundtrip(void)
{
    uint8 wr[NOR_PAGE_SIZE];
    uint8 rd[NOR_PAGE_SIZE];
    uint32 base = NOR_SCRATCH_SECTOR;
    uint32 i;

    if(g_nor_inited == 0u)
    {
        return ARKRTOS_NOR_ERR_STATE;
    }

    ark_shell_printf("nor: round trip on sector 0x%06X\r\n", (unsigned)base);

    /* 1) 擦 */
    if(arkrtos_board_nor_erase(base, NOR_SECTOR_SIZE) != 0)
    {
        ark_shell_printf("nor: FAIL at erase (sr1=0x%02X)\r\n", (unsigned)g_nor_sr1);
        return -1;
    }
    ark_shell_printf("nor: step1 erase  ok\r\n");

    /* 2) 擦后必须是全 FF：说明擦除命令真的落到了阵列上 */
    if((arkrtos_board_nor_read(base, rd, sizeof(rd)) != 0) ||
       (nor_all_bytes(rd, sizeof(rd), 0xFFu) == 0u))
    {
        ark_shell_printf("nor: FAIL after erase, first bytes %02X %02X %02X %02X\r\n",
                         (unsigned)rd[0], (unsigned)rd[1],
                         (unsigned)rd[2], (unsigned)rd[3]);
        return -1;
    }
    ark_shell_printf("nor: step2 blank  ok (all 0xFF)\r\n");

    /* 3) 页编程 + 跨页写各一段：0x00..0xFF 斜坡最容易看出"错位"或"回绕" */
    for(i = 0u; i < sizeof(wr); i++)
    {
        wr[i] = (uint8)(i & 0xFFu);
    }
    if(arkrtos_board_nor_write(base, wr, sizeof(wr)) != 0)
    {
        ark_shell_printf("nor: FAIL at program (sr1=0x%02X)\r\n", (unsigned)g_nor_sr1);
        return -1;
    }
    ark_shell_printf("nor: step3 program ok (256B ramp)\r\n");

    /* 4) 读回逐字节比对 */
    if(arkrtos_board_nor_read(base, rd, sizeof(rd)) != 0)
    {
        ark_shell_printf("nor: FAIL at read-back\r\n");
        return -1;
    }
    for(i = 0u; i < sizeof(rd); i++)
    {
        if(rd[i] != wr[i])
        {
            ark_shell_printf("nor: FAIL compare at +%u: wrote %02X read %02X\r\n",
                             (unsigned)i, (unsigned)wr[i], (unsigned)rd[i]);
            return -1;
        }
    }
    ark_shell_printf("nor: step4 compare ok (256/256 bytes)\r\n");

    /* 5) 收尾擦干净：草稿扇区不留给后人 */
    if(arkrtos_board_nor_erase(base, NOR_SECTOR_SIZE) != 0)
    {
        ark_shell_printf("nor: FAIL at final erase\r\n");
        return -1;
    }
    ark_shell_printf("nor: round trip PASS\r\n");
    return 0;
}

/* ============================================================
 * 逐拍取证：一条 0x9F 从写寄存器到收数据的每一步 SR
 * ------------------------------------------------------------
 * 为什么需要它：外设寄存器读回全对、命令写完 SR 却始终是 0（BUSY 不置位）、
 * 数据阶段又偶尔能拿到 0x00 —— 这三种现象必须能分开看，否则每一个结论都
 * 只是"可能是"。这个子命令把每一步的 SR 都打出来，是给下一个人（或下一轮
 * 的自己）留的现场记录，不是给自己找台阶。
 * ============================================================ */

static void nor_trace_sr(const char *tag, uint32 sr)
{
    ark_shell_printf("nor: %-12s SR=0x%08X BUSY=%u TCF=%u TEF=%u FLEVEL=%u\r\n",
                     tag, (unsigned)sr,
                     (unsigned)((sr & QUADSPI_SR_BUSY) != 0u),
                     (unsigned)((sr & QUADSPI_SR_TCF) != 0u),
                     (unsigned)((sr & QUADSPI_SR_TEF) != 0u),
                     (unsigned)((sr & QUADSPI_SR_FLEVEL_Msk) >>
                                QUADSPI_SR_FLEVEL_Pos));
}

static int nor_trace(uint8 instr, uint32 addr, uint32 has_addr, uint32 len)
{
    uint32 i;
    uint32 ccr;
    uint32 sr_after;
    uint32 prev;
    uint32 n = 0u;
    uint32 loops = 0u;
    uint32 clk_edges = 0u;
    uint32 ncs_low = 0u;
    uint32 cyc;
    uint8 buf[32];

    ark_shell_printf("nor: CR=0x%08X DCR=0x%08X (EN=%u PRESC=%u)\r\n",
                     (unsigned)QUADSPI->CR, (unsigned)QUADSPI->DCR,
                     (unsigned)((QUADSPI->CR & QUADSPI_CR_EN) != 0u),
                     (unsigned)(QUADSPI->CR >> QUADSPI_CR_PRESCALER_Pos));
    nor_trace_sr("before", QUADSPI->SR);

    if(nor_wait_idle() != 0)
    {
        ark_shell_printf("nor: wait idle failed, SR=0x%08X\r\n", (unsigned)QUADSPI->SR);
        return -1;
    }
    nor_clear_flags();
    nor_trace_sr("cleared", QUADSPI->SR);

    QUADSPI->DLR = len - 1u;
    QUADSPI->AR  = (has_addr != 0u) ? addr : 0u;
    QUADSPI->ABR = 0u;
    ark_shell_printf("nor: wrote DLR=0x%08X AR=0x%08X\r\n",
                     (unsigned)QUADSPI->DLR, (unsigned)QUADSPI->AR);

    ccr = ((uint32)instr << QUADSPI_CCR_INSTRUCTION_Pos) |
          (1u << QUADSPI_CCR_IMODE_Pos) |
          (1u << QUADSPI_CCR_DMODE_Pos) |
          (1u << QUADSPI_CCR_FMODE_Pos);
    if(has_addr != 0u)
    {
        ccr |= (1u << QUADSPI_CCR_ADMODE_Pos) |
               ((uint32)NOR_ADSIZE_24BITS << QUADSPI_CCR_ADSIZE_Pos);
    }
    ark_shell_printf("nor: writing CCR=0x%08X (instr 0x%02X, addr %u)\r\n",
                     (unsigned)ccr, (unsigned)instr, (unsigned)has_addr);
    nor_cyc_start();
    QUADSPI->CCR = ccr;
    sr_after = QUADSPI->SR;
    ark_shell_printf("nor: readback CCR=0x%08X, SR right after=0x%08X\r\n",
                     (unsigned)QUADSPI->CCR, (unsigned)sr_after);

    /* 一边收数据一边看引脚：AF 模式下 GPIOx->IDR 回报的是引脚真实电平，
     * 所以这里能直接回答"外设到底有没有驱 CLK / 拉 NCS"。 */
    prev = GPIOF->IDR;
    for(i = 0u; (i < 2000000u) && (n < len); i++)
    {
        uint32 idr = GPIOF->IDR;
        uint32 sr;

        if(((idr ^ prev) & (1uL << 10)) != 0u)
        {
            clk_edges++;
        }
        prev = idr;
        if((GPIOG->IDR & (1uL << 6)) == 0u)
        {
            ncs_low++;
        }
        loops++;

        sr = QUADSPI->SR;
        if((sr & QUADSPI_SR_FLEVEL_Msk) != 0u)
        {
            buf[n] = *(volatile uint8 *)&QUADSPI->DR;
            n++;
        }
        else if((sr & QUADSPI_SR_TCF) != 0u)
        {
            break;
        }
    }

    cyc = nor_cyc_now();
    ark_shell_printf("nor: bus clk edges=%u ncs low=%u/%u got %u/%u bytes\r\n",
                     (unsigned)clk_edges, (unsigned)ncs_low, (unsigned)loops,
                     (unsigned)n, (unsigned)len);
    ark_shell_printf("nor: transfer took %u core cycles (%u us at 480 MHz), %u bits\r\n",
                     (unsigned)cyc, (unsigned)(cyc / 480u),
                     (unsigned)((len * 8u) + (has_addr != 0u ? 32u : 8u)));
    for(i = 0u; (i < n) && (i < 16u); i++)
    {
        ark_shell_printf("nor: byte[%u]=0x%02X\r\n", (unsigned)i, (unsigned)buf[i]);
    }
    nor_trace_sr("after bytes", QUADSPI->SR);

    (void)nor_abort();
    nor_trace_sr("after abort", QUADSPI->SR);
    return 0;
}

/* ============================================================
 * 对外入口
 * ============================================================ */

int32 arkrtos_board_nor_init(void)
{
    int32 rc;
    uint32 id = 0u;
    uint8 sr1 = 0xFFu;

    if(g_nor_ready == 1u)
    {
        return 0;
    }

    /* 每次都从头来：引脚 + 复位 + 寄存器，任何一步失败都能单独指认。 */
    g_nor_inited = 0u;
    nor_pins_init();

    rc = nor_reg_init();
    if(rc != 0)
    {
        g_nor_last_err = rc;
        ARKRTOS_LOGE("NOR", "QUADSPI register init failed (%d): CR=0x%08X "
                     "DCR=0x%08X SR=0x%08X (want CR=0x%08X DCR=0x%08X)",
                     (int)rc, (unsigned)QUADSPI->CR, (unsigned)QUADSPI->DCR,
                     (unsigned)QUADSPI->SR,
                     (unsigned)(((uint32)NOR_PRESCALER << QUADSPI_CR_PRESCALER_Pos) |
                                ((uint32)(NOR_FIFO_THRESHOLD - 1u) << QUADSPI_CR_FTHRES_Pos) |
                                QUADSPI_CR_EN),
                     (unsigned)(((uint32)NOR_FSIZE_FIELD << QUADSPI_DCR_FSIZE_Pos) |
                                (NOR_CSHT_FIELD(NOR_CS_HIGH_CYCLES) << QUADSPI_DCR_CSHT_Pos)));
        return rc;
    }

    rc = arkrtos_board_nor_id(&id);
    if(rc != 0)
    {
        g_nor_last_err = rc;
        ARKRTOS_LOGE("NOR", "JEDEC ID read failed (%d): CR=0x%08X SR=0x%08X "
                     "(pins NCS=PG6 CLK=PF10 IO=PF6..PF9)",
                     (int)rc, (unsigned)QUADSPI->CR, (unsigned)QUADSPI->SR);
        return rc;
    }

    if(id != ARKRTOS_NOR_JEDEC_W25Q64)
    {
        /* 读到别的值不等于"芯片坏"：更可能是引脚对不上或者器件不在这条
         * 总线上。把实际读到的值打出来，不猜它是什么。 */
        g_nor_last_err = ARKRTOS_NOR_ERR_ID;
        ARKRTOS_LOGE("NOR", "unexpected JEDEC ID 0x%06X (W25Q64 is 0x%06X) "
                     "SR=0x%08X",
                     (unsigned)id, (unsigned)ARKRTOS_NOR_JEDEC_W25Q64,
                     (unsigned)QUADSPI->SR);
        return ARKRTOS_NOR_ERR_ID;
    }

    (void)arkrtos_board_nor_status(&sr1);
    g_nor_ready = 1u;
    g_nor_last_err = 0;
    ARKRTOS_LOGI("NOR", "W25Q64 answered: JEDEC 0x%06X, SR1 0x%02X, %u MiB at 0x%08X (mm)",
                 (unsigned)id, (unsigned)sr1, (unsigned)(ARKRTOS_NOR_SIZE >> 20),
                 (unsigned)NOR_MM_BASE);
    return 0;
}

int32 arkrtos_board_nor_last_error(void)
{
    return g_nor_last_err;
}

uint32 arkrtos_board_nor_last_id(void)
{
    return g_nor_id;
}

/* ============================================================
 * Shell 命令
 * ============================================================ */

/** @brief "0x1A2B" / "1A2B" 都按十六进制解析；成功返回 0 */
static int nor_parse_hex(const char *s, uint32 *out)
{
    uint32 v = 0u;
    uint32 digits = 0u;

    if((s == 0) || (out == 0))
    {
        return -1;
    }
    if((s[0] == '0') && ((s[1] == 'x') || (s[1] == 'X')))
    {
        s += 2;
    }
    while(*s != 0)
    {
        uint32 d;

        if((*s >= '0') && (*s <= '9'))
        {
            d = (uint32)(*s - '0');
        }
        else if((*s >= 'a') && (*s <= 'f'))
        {
            d = (uint32)(*s - 'a') + 10u;
        }
        else if((*s >= 'A') && (*s <= 'F'))
        {
            d = (uint32)(*s - 'A') + 10u;
        }
        else
        {
            return -1;
        }
        v = (v << 4) | d;
        digits++;
        s++;
    }
    if((digits == 0u) || (digits > 8u))
    {
        return -1;
    }
    *out = v;
    return 0;
}

/** @brief 十进制解析（只用于长度，避免把 10 当成 0x10） */
static int nor_parse_dec(const char *s, uint32 *out)
{
    uint32 v = 0u;
    uint32 digits = 0u;

    if((s == 0) || (out == 0))
    {
        return -1;
    }
    while(*s != 0)
    {
        if((*s < '0') || (*s > '9'))
        {
            return -1;
        }
        v = (v * 10u) + (uint32)(*s - '0');
        digits++;
        s++;
    }
    if(digits == 0u)
    {
        return -1;
    }
    *out = v;
    return 0;
}

/** @brief 大小写不敏感的全串比较（不用 libc：这里只需要比较命令名） */
static int nor_streq(const char *a, const char *b)
{
    while((*a != 0) && (*b != 0))
    {
        char ca = *a;
        char cb = *b;

        if((ca >= 'A') && (ca <= 'Z'))
        {
            ca = (char)(ca - 'A' + 'a');
        }
        if((cb >= 'A') && (cb <= 'Z'))
        {
            cb = (char)(cb - 'A' + 'a');
        }
        if(ca != cb)
        {
            return 0;
        }
        a++;
        b++;
    }
    return (*a == 0) && (*b == 0);
}

static void nor_dump(const char *tag, uint32 addr, const uint8 *buf, uint32 len)
{
    uint32 i;

    ark_shell_printf("%s 0x%06X:", tag, (unsigned)addr);
    for(i = 0u; i < len; i++)
    {
        ark_shell_printf(" %02X", (unsigned)buf[i]);
    }
    ark_shell_printf("\r\n");
}

/** @brief 把 "read"/"mm" 共用的参数解析出来；失败时已经打印用法 */
static int nor_parse_rd_args(int argc, char *argv[], const char *usage,
                            uint32 *addr, uint32 *len)
{
    if(argc < 3)
    {
        ark_shell_printf("%s\r\n", usage);
        return -1;
    }
    if(nor_parse_hex(argv[2], addr) != 0)
    {
        ark_shell_printf("nor: bad address '%s'\r\n", argv[2]);
        return -1;
    }
    *len = 16u;
    if(argc > 3)
    {
        if((nor_parse_dec(argv[3], len) != 0) || (*len == 0u) || (*len > 32u))
        {
            ark_shell_printf("nor: bad length '%s' (1..32)\r\n", argv[3]);
            return -1;
        }
    }
    return 0;
}

/** @brief 打印 6 根 QSPI 引脚的复用与电平（只读寄存器，不会 fault） */
static void nor_dump_pins(void)
{
    ark_shell_printf("pin   MODER AF   IDR\r\n");
    ark_shell_printf("PF6  %5u %4u %5u\r\n",
                     (unsigned)((GPIOF->MODER >> 12) & 3u),
                     (unsigned)((GPIOF->AFR[0] >> 24) & 0xFu),
                     (unsigned)((GPIOF->IDR >> 6) & 1u));
    ark_shell_printf("PF7  %5u %4u %5u\r\n",
                     (unsigned)((GPIOF->MODER >> 14) & 3u),
                     (unsigned)((GPIOF->AFR[0] >> 28) & 0xFu),
                     (unsigned)((GPIOF->IDR >> 7) & 1u));
    ark_shell_printf("PF8  %5u %4u %5u\r\n",
                     (unsigned)((GPIOF->MODER >> 16) & 3u),
                     (unsigned)((GPIOF->AFR[1] >> 0) & 0xFu),
                     (unsigned)((GPIOF->IDR >> 8) & 1u));
    ark_shell_printf("PF9  %5u %4u %5u\r\n",
                     (unsigned)((GPIOF->MODER >> 18) & 3u),
                     (unsigned)((GPIOF->AFR[1] >> 4) & 0xFu),
                     (unsigned)((GPIOF->IDR >> 9) & 1u));
    ark_shell_printf("PF10 %5u %4u %5u\r\n",
                     (unsigned)((GPIOF->MODER >> 20) & 3u),
                     (unsigned)((GPIOF->AFR[1] >> 8) & 0xFu),
                     (unsigned)((GPIOF->IDR >> 10) & 1u));
    ark_shell_printf("PG6  %5u %4u %5u\r\n",
                     (unsigned)((GPIOG->MODER >> 12) & 3u),
                     (unsigned)((GPIOG->AFR[0] >> 24) & 0xFu),
                     (unsigned)((GPIOG->IDR >> 6) & 1u));
    ark_shell_printf("MODER 2 = alternate function; expect PF6/7/10=AF9, PF8/9=AF10, PG6=AF10\r\n");
}

int arkrtos_board_nor_shell_cmd(int argc, char *argv[])
{
    const char *sub = (argc > 1) ? argv[1] : "info";

    if(nor_streq(sub, "id"))
    {
        uint32 id = 0u;
        int32 rc;

        if(g_nor_inited == 0u)
        {
            rc = arkrtos_board_nor_init();
            if(rc != 0)
            {
                ark_shell_printf("nor: init failed, rc=%d\r\n", (int)rc);
                return -1;
            }
        }
        rc = arkrtos_board_nor_id(&id);
        if(rc != 0)
        {
            ark_shell_printf("nor: JEDEC read failed, rc=%d (CR=0x%08X SR=0x%08X)\r\n",
                             (int)rc, (unsigned)QUADSPI->CR, (unsigned)QUADSPI->SR);
            return -1;
        }
        ark_shell_printf("nor: JEDEC ID 0x%06X (%s)\r\n", (unsigned)id,
                         (id == ARKRTOS_NOR_JEDEC_W25Q64) ? "W25Q64" : "not W25Q64");
        return (id == ARKRTOS_NOR_JEDEC_W25Q64) ? 0 : -1;
    }

    if(nor_streq(sub, "sr"))
    {
        uint8 sr1 = 0xFFu;
        int32 rc = arkrtos_board_nor_status(&sr1);

        if(rc != 0)
        {
            ark_shell_printf("nor: status read failed, rc=%d\r\n", (int)rc);
            return -1;
        }
        ark_shell_printf("nor: SR1 0x%02X%s%s%s\r\n", (unsigned)sr1,
                         (sr1 & 0x01u) ? " BUSY" : "",
                         (sr1 & 0x02u) ? " WEL" : "",
                         (sr1 & 0x1Cu) ? " BP" : "");
        return 0;
    }

    if(nor_streq(sub, "read"))
    {
        uint8 buf[32];
        uint32 addr = 0u;
        uint32 len = 16u;
        int32 rc;

        if(nor_parse_rd_args(argc, argv, "usage: nor read <hexaddr> [len<=32]",
                             &addr, &len) != 0)
        {
            return -1;
        }
        rc = arkrtos_board_nor_read(addr, buf, len);
        if(rc != 0)
        {
            ark_shell_printf("nor: read failed, rc=%d\r\n", (int)rc);
            return -1;
        }
        nor_dump("nor read", addr, buf, len);
        return 0;
    }

    if(nor_streq(sub, "verify"))
    {
        int32 rc = arkrtos_board_nor_roundtrip();

        ark_shell_printf("nor: verify rc=%d\r\n", (int)rc);
        return rc;
    }

    if(nor_streq(sub, "er"))
    {
        /* nor er <hexaddr>：擦一个 4 KiB 扇区（地址必须 4 KiB 对齐） */
        uint32 addr = 0u;
        int32 rc;

        if((argc < 3) || (nor_parse_hex(argv[2], &addr) != 0))
        {
            ark_shell_printf("usage: nor er <hexaddr 4K-aligned>\r\n");
            return -1;
        }
        rc = arkrtos_board_nor_erase(addr, NOR_SECTOR_SIZE);
        ark_shell_printf("nor: erase 0x%06X rc=%d (sr1=0x%02X)\r\n",
                         (unsigned)addr, (int)rc, (unsigned)g_nor_sr1);
        return rc;
    }

    if(nor_streq(sub, "wr"))
    {
        /* nor wr <hexaddr> <len<=64>：写一段 0x00.. 的斜坡，配 'nor read' 看结果。
         * 用斜坡而不是让用户敲十六进制，是因为"某一位错了"在斜坡上一眼可见。 */
        uint32 addr = 0u;
        uint32 len = 16u;
        uint8 buf[64];
        uint32 i;
        int32 rc;

        if(argc < 3 || (nor_parse_hex(argv[2], &addr) != 0))
        {
            ark_shell_printf("usage: nor wr <hexaddr> [len<=64]\r\n");
            return -1;
        }
        if(argc > 3)
        {
            if((nor_parse_dec(argv[3], &len) != 0) || (len == 0u) || (len > 64u))
            {
                ark_shell_printf("nor: bad length '%s' (1..64)\r\n", argv[3]);
                return -1;
            }
        }
        for(i = 0u; i < len; i++)
        {
            buf[i] = (uint8)(((addr + i) & 0xFFu));
        }
        rc = arkrtos_board_nor_write(addr, buf, len);
        ark_shell_printf("nor: write 0x%06X len=%u rc=%d\r\n",
                         (unsigned)addr, (unsigned)len, (int)rc);
        return rc;
    }

    if(nor_streq(sub, "raw"))
    {
        /* nor raw <instr_hex> [len] [addr_hex]：addr 给了就带 24 位地址段。
         * 这是给"为什么带地址的命令不启动"这类问题做正交实验用的。 */
        uint32 instr = 0x9Fu;
        uint32 len = 3u;
        uint32 addr = 0u;
        uint32 has_addr = 0u;

        if((argc >= 3) && (nor_parse_hex(argv[2], &instr) != 0))
        {
            ark_shell_printf("nor: bad instruction '%s'\r\n", argv[2]);
            return -1;
        }
        if(argc >= 4)
        {
            if((nor_parse_dec(argv[3], &len) != 0) || (len == 0u) || (len > 32u))
            {
                ark_shell_printf("nor: bad length '%s' (1..32)\r\n", argv[3]);
                return -1;
            }
        }
        if(argc >= 5)
        {
            if(nor_parse_hex(argv[4], &addr) != 0)
            {
                ark_shell_printf("nor: bad address '%s'\r\n", argv[4]);
                return -1;
            }
            has_addr = 1u;
        }
        return (nor_trace((uint8)instr, addr, has_addr, len) == 0) ? 0 : -1;
    }

    if(nor_streq(sub, "af"))
    {
        uint32 af_pf = 9u;
        uint32 af_pg = 10u;

        if(argc < 3)
        {
            ark_shell_printf("usage: nor af <pf_af 0..15> [pg_af 0..15]\r\n");
            return -1;
        }
        if((nor_parse_dec(argv[2], &af_pf) != 0) || (af_pf > 15u))
        {
            ark_shell_printf("nor: bad AF '%s'\r\n", argv[2]);
            return -1;
        }
        if(argc > 3)
        {
            if((nor_parse_dec(argv[3], &af_pg) != 0) || (af_pg > 15u))
            {
                ark_shell_printf("nor: bad AF '%s'\r\n", argv[3]);
                return -1;
            }
        }
        nor_set_af(af_pf, af_pg);
        ark_shell_printf("nor: PF6..PF10 = AF%u, PG6 = AF%u "
                         "(now run 'nor id' / 'nor bb')\r\n",
                         (unsigned)af_pf, (unsigned)af_pg);
        return 0;
    }

    if(nor_streq(sub, "edg"))
    {
        return (nor_edge_probe() == 0) ? 0 : -1;
    }

    if(nor_streq(sub, "wel"))
    {
        return (nor_write_path_probe() == 0) ? 0 : -1;
    }

    if(nor_streq(sub, "dr32"))
    {
        return (nor_dr_width() == 0) ? 0 : -1;
    }

    if(nor_streq(sub, "bench"))
    {
        return (nor_bench() == 0) ? 0 : -1;
    }

    if(nor_streq(sub, "drive"))
    {
        int32 rc = nor_bus_probe();

        if(rc != 0)
        {
            ark_shell_printf("nor: drive probe failed, rc=%d SR=0x%08X\r\n",
                             (int)rc, (unsigned)QUADSPI->SR);
            return -1;
        }
        return 0;
    }

    if(nor_streq(sub, "clk"))
    {
        uint32 n = 0u;
        uint32 cr;

        if((argc < 3) || (nor_parse_dec(argv[2], &n) != 0) || (n > 255u))
        {
            ark_shell_printf("usage: nor clk <0..255>  (SCK = f_qspi/(n+1))\r\n");
            return -1;
        }
        if(nor_wait_idle() != 0)
        {
            ark_shell_printf("nor: BUSY stuck, SR=0x%08X\r\n", (unsigned)QUADSPI->SR);
            return -1;
        }
        /* PRESCALER 只在 EN=0 且不 BUSY 时改；改完重新开 EN 并回读自证。 */
        cr = QUADSPI->CR & ~(QUADSPI_CR_PRESCALER | QUADSPI_CR_EN);
        QUADSPI->CR = cr;
        QUADSPI->CR = cr | ((uint32)n << QUADSPI_CR_PRESCALER_Pos) | QUADSPI_CR_EN;
        ark_shell_printf("nor: prescaler=%u CR=0x%08X (SCK = f_qspi/%u)\r\n",
                         (unsigned)n, (unsigned)QUADSPI->CR, (unsigned)(n + 1u));
        return 0;
    }

    if(nor_streq(sub, "mm"))
    {
        uint8 buf[32];
        uint32 addr = 0u;
        uint32 len = 16u;
        int32 rc;

        if(nor_parse_rd_args(argc, argv, "usage: nor mm <hexaddr> [len<=32]",
                             &addr, &len) != 0)
        {
            return -1;
        }
        /* 内存映射窗口没起来时读 0x90000000 是总线错误（PANIC BUS），不是错误码。
         * 所以只有 ID 已验证过才允许走这条路：宁可拒绝，不给一个把控制台打断的
         * "读数"。 */
        if(g_nor_ready != 1u)
        {
            ark_shell_printf("nor: mm refused - NOR not verified (run 'nor id' first; "
                             "reading an unbacked window faults the bus)\r\n");
            return -1;
        }
        rc = arkrtos_board_nor_mm_read(addr, buf, len);
        if(rc != 0)
        {
            ark_shell_printf("nor: mm read failed, rc=%d\r\n", (int)rc);
            return -1;
        }
        nor_dump("nor mm", addr, buf, len);
        return 0;
    }

    if(nor_streq(sub, "regs"))
    {
        ark_shell_printf("nor: CR=0x%08X DCR=0x%08X SR=0x%08X CCR=0x%08X\r\n",
                         (unsigned)QUADSPI->CR, (unsigned)QUADSPI->DCR,
                         (unsigned)QUADSPI->SR, (unsigned)QUADSPI->CCR);
        ark_shell_printf("nor: DLR=0x%08X AR=0x%08X ABR=0x%08X\r\n",
                         (unsigned)QUADSPI->DLR, (unsigned)QUADSPI->AR,
                         (unsigned)QUADSPI->ABR);
        ark_shell_printf("nor: AHB3ENR=0x%08X AHB3RSTR=0x%08X D1CCIPR=0x%08X "
                         "(QSPISEL=%u)\r\n",
                         (unsigned)RCC->AHB3ENR, (unsigned)RCC->AHB3RSTR,
                         (unsigned)RCC->D1CCIPR,
                         (unsigned)((RCC->D1CCIPR & RCC_D1CCIPR_QSPISEL_Msk) >>
                                    RCC_D1CCIPR_QSPISEL_Pos));
        return 0;
    }

    if(nor_streq(sub, "bb"))
    {
        uint8 raw[3] = {0u, 0u, 0u};
        uint32 id;

        (void)nor_bitbang_id(raw);
        id = ((uint32)raw[0] << 16) | ((uint32)raw[1] << 8) | (uint32)raw[2];
        ark_shell_printf("nor: bit-bang JEDEC 0x%06X (raw %02X %02X %02X) %s\r\n",
                         (unsigned)id, (unsigned)raw[0], (unsigned)raw[1],
                         (unsigned)raw[2],
                         (id == ARKRTOS_NOR_JEDEC_W25Q64) ? "= W25Q64" : "!= W25Q64");
        return (id == ARKRTOS_NOR_JEDEC_W25Q64) ? 0 : -1;
    }

    if(nor_streq(sub, "rst"))
    {
        /* nor rst：软件复位芯片（0x66 Enable Reset + 0x99 Reset）。
         * 用途：芯片可能被一条半截命令（NCS 没抬起来 / 数量不对的时钟）留在
         * "等后续字节"的状态里，此时所有命令都被它忽略、DO 悬空读回 0xFF。
         * 复位会把它拉回上电默认（单线、无保护、非 QPI），代价只有几毫秒。 */
        uint8 sr = 0u;
        uint32 i;

        nor_bb_enter();
        nor_bb_cmd(0x66u, 0, 0u);            /* Enable Reset */
        for(i = 0u; i < 200u; i++)           /* ~1 ms */
        {
            nor_delay_us(5u);
        }
        nor_bb_cmd(0x99u, 0, 0u);            /* Reset Device */
        for(i = 0u; i < 1200u; i++)          /* ~5 ms，tRST 上限 */
        {
            nor_delay_us(5u);
        }
        nor_bb_cmd(NOR_CMD_READ_STATUS1, &sr, 1u);
        nor_bb_leave();
        ark_shell_printf("nor: reset sent (0x66/0x99), bit-bang SR1=0x%02X\r\n",
                         (unsigned)sr);
        /* 复位后 QUADSPI 已被 nor_bb_leave() 重新配置，这里读一次作对照 */
        {
            uint8 buf[4];
            uint32 id = 0u;

            if((arkrtos_board_nor_id(&id) == 0) &&
               (arkrtos_board_nor_read(0u, buf, sizeof(buf)) == 0))
            {
                ark_shell_printf("nor: after reset id=0x%06X read0=%02X %02X %02X %02X\r\n",
                                 (unsigned)id, (unsigned)buf[0], (unsigned)buf[1],
                                 (unsigned)buf[2], (unsigned)buf[3]);
            }
            else
            {
                ark_shell_printf("nor: after reset id or read still failing\r\n");
            }
        }
        return 0;
    }

    if(nor_streq(sub, "er2"))
    {
        /* nor er2 <hexaddr>：擦除命令变体——地址段之后补 1 个哑数据字节。
         * 目的：检验"带地址 + 无数据段"这一形态是不是 QUADSPI 的卡点。 */
        uint32 addr = 0u;
        uint8 dummy = 0x00u;
        int32 rc;
        uint32 t0;

        if((argc < 3) || (nor_parse_hex(argv[2], &addr) != 0))
        {
            ark_shell_printf("usage: nor er2 <hexaddr>\r\n");
            return -1;
        }
        rc = nor_write_enable();
        ark_shell_printf("nor: er2 WREN rc=%d\r\n", (int)rc);
        if(rc == 0)
        {
            t0 = nor_cyc_now();
            rc = nor_indirect_write(NOR_CMD_SECTOR_ERASE, addr, 1u, &dummy, 1u);
            ark_shell_printf("nor: er2 erase(addr+1dummy) rc=%d cycles=%u SR=0x%08X\r\n",
                             (int)rc, (unsigned)(nor_cyc_now() - t0),
                             (unsigned)QUADSPI->SR);
            if(rc == 0)
            {
                rc = nor_wait_ready(NOR_TMO_POLL_MS);
                ark_shell_printf("nor: er2 wait_ready rc=%d sr1=0x%02X\r\n",
                                 (int)rc, (unsigned)g_nor_sr1);
            }
        }
        return rc;
    }

    if(nor_streq(sub, "wren"))
    {
        /* nor wren：三种发 0x06 的方式各试一次，每次都用位敲读 SR1 作证。
         * (a) 间接写、无数据段（当前实现）
         * (b) 间接写、跟 1 个哑数据字节
         * (c) 间接读模式、无数据段（FMODE=01 + DMODE=0）
         * 判读：哪一条能让 WEL=1，就说明"无数据段的间接写"这一形态本身有问题。 */
        uint8 sr = 0u;
        uint8 dummy = 0x00u;
        int32 rc;

        /* (a) */
        rc = nor_indirect_write(NOR_CMD_WRITE_ENABLE, 0u, 0u, 0, 0u);
        nor_bb_enter();
        nor_bb_cmd(NOR_CMD_READ_STATUS1, &sr, 1u);
        nor_bb_leave();
        ark_shell_printf("nor: (a) write,no-data rc=%d -> SR1=0x%02X WEL=%u\r\n",
                         (int)rc, (unsigned)sr,
                         (unsigned)((sr & NOR_STATUS_WEL) != 0u));

        /* (b) */
        rc = nor_indirect_write(NOR_CMD_WRITE_ENABLE, 0u, 0u, &dummy, 1u);
        nor_bb_enter();
        nor_bb_cmd(NOR_CMD_READ_STATUS1, &sr, 1u);
        nor_bb_leave();
        ark_shell_printf("nor: (b) write,1-dummy rc=%d -> SR1=0x%02X WEL=%u\r\n",
                         (int)rc, (unsigned)sr,
                         (unsigned)((sr & NOR_STATUS_WEL) != 0u));

        /* (c) 读模式、无数据段 */
        if(nor_wait_idle() == 0)
        {
            nor_clear_flags();
            QUADSPI->DLR = 0u;
            QUADSPI->AR  = 0u;
            QUADSPI->ABR = 0u;
            QUADSPI->CCR = ((uint32)NOR_CMD_WRITE_ENABLE << QUADSPI_CCR_INSTRUCTION_Pos) |
                           (1u << QUADSPI_CCR_IMODE_Pos) |
                           (1u << QUADSPI_CCR_FMODE_Pos);
        }
        nor_bb_enter();
        nor_bb_cmd(NOR_CMD_READ_STATUS1, &sr, 1u);
        nor_bb_leave();
        ark_shell_printf("nor: (c) read-mode,no-data -> SR1=0x%02X WEL=%u\r\n",
                         (unsigned)sr,
                         (unsigned)((sr & NOR_STATUS_WEL) != 0u));
        return 0;
    }

    if(nor_streq(sub, "bbwr"))
    {
        /* nor bbwr：全部位敲（0x06 + 0x05）。用来把"芯片锁住了"和
         * "QUADSPI 发的 0x06 没到芯片"分开——两条路径都读不到 WEL 才是芯片侧。 */
        uint8 sr = 0u;

        nor_bb_enter();
        nor_bb_cmd(NOR_CMD_WRITE_ENABLE, 0, 0u);
        nor_bb_cmd(NOR_CMD_READ_STATUS1, &sr, 1u);
        nor_bb_leave();
        ark_shell_printf("nor: bit-bang WREN -> SR1=0x%02X %s\r\n",
                         (unsigned)sr,
                         ((sr & NOR_STATUS_WEL) != 0u) ? "WEL=1" : "WEL=0");
        return ((sr & NOR_STATUS_WEL) != 0u) ? 0 : -1;
    }

    if(nor_streq(sub, "pins"))
    {
        nor_dump_pins();
        return 0;
    }

    if(nor_streq(sub, "abort"))
    {
        int32 rc = nor_abort();

        ark_shell_printf("nor: abort rc=%d SR=0x%08X\r\n", (int)rc,
                         (unsigned)QUADSPI->SR);
        return (rc == 0) ? 0 : -1;
    }

    if(nor_streq(sub, "init"))
    {
        int32 rc;

        g_nor_ready = 0u;
        g_nor_inited = 0u;
        rc = arkrtos_board_nor_init();
        ark_shell_printf("nor: init rc=%d (JEDEC 0x%06X)\r\n", (int)rc,
                         (unsigned)g_nor_id);
        return (rc == 0) ? 0 : -1;
    }

    if(!nor_streq(sub, "info"))
    {
        ark_shell_printf("nor: unknown subcommand '%s'\r\n", sub);
    }

    /* info（默认）：把"有没有应答"与"为什么"分开说清楚 */
    ark_shell_printf("\r\nnor  : %s (inited=%u)\r\n",
                     (g_nor_ready == 1u) ? "W25Q64 answered" : "NOT verified",
                     (unsigned)g_nor_inited);
    ark_shell_printf("jedec: 0x%06X (expect 0x%06X)\r\n",
                     (unsigned)g_nor_id, (unsigned)ARKRTOS_NOR_JEDEC_W25Q64);
    ark_shell_printf("sr1  : 0x%02X\r\n", (unsigned)g_nor_sr1);
    ark_shell_printf("size : %u MiB\r\n", (unsigned)(ARKRTOS_NOR_SIZE >> 20));
    ark_shell_printf("mm   : 0x%08X\r\n", (unsigned)NOR_MM_BASE);
    ark_shell_printf("pins : NCS=PG6(AF10) CLK=PF10(AF9) IO0=PF8(AF10) IO1=PF9(AF10) IO2/3=PF7/PF6(AF9)\r\n");
    ark_shell_printf("last : err=%d\r\n", (int)g_nor_last_err);
    ark_shell_printf("usage: nor [info | init | id | sr | read <hexaddr> [len] | "
                     "mm <hexaddr> [len] | regs | pins | bb | drive | bench | edg | wel | dr32 | af <n> [pg] | raw <instr> [len] [addr] | clk <n> | abort | bbwr | wren | er2 <addr> | rst | verify | er <addr> | wr <addr> [len]]\r\n");
    return 0;
}

#endif /* ARKRTOS_USE_QSPI */
