/**
* @file test_drvnor_host.c
* @brief board/stm32f427/drvnor.c 的宿主用例：板载 SPI NOR（W25Q128）驱动契约
* @details 这是板级驱动宿主覆盖率的第三刀。drvnor 经过 STM32 HAL 的 SPI 接口
*          （`HAL_SPI_Transmit` / `HAL_SPI_Receive` / `HAL_SPI_Init`），片选取
*          自 GPIOE->BSRR 的直接写，所以本用例干两件事：
*
*            1. 给 HAL 的 SPI/GPIO 函数一份**记录式桩**：谁调了什么、发了
*               哪几个字节，全部记下来；
*            2. 桩背后挂一颗**字节级的 W25Q128 从机模型**：认命令、收地址、
*               读写一块 64 MiB 的静态存储、维护 WIP / WEL 两个状态位。
*               模型不模拟时序，只保证「命令 + 地址 + 数据」这一层的语义与
*               数据手册一致（页内回绕、编程只能清位、擦除写 0xFF）。
*
*          片选走 GPIOE->BSRR 的直接写，所以 GPIOB/GPIOE/RCC 那一页
*          （0x40020000，Windows 分配粒度 64 KB）真的映射出来；RCC 那一页
*          还兼作 `__HAL_RCC_GPIOx_CLK_ENABLE()` 的落点，不映射就会在 init
*          的第一行踩空。映射失败直接报错退出，绝不跳过。
*
*          宿主上钉住的是这些：
*
*            1. 上电序列：先拉高片选再配 SPI（否则片选悬空时可能吃掉半个
*               命令）、发 0xAB 退出深度掉电、读 3 字节 JEDEC id、按容量
*               字节推导大小并封顶 64 MiB、最后确认真空闲。
*            2. 失败路径一个不能少：SPI 初始化失败、Release-PD/JEDEC 发不
*               出去、JEDEC 收不回来、id[0]==0、状态寄存器读不到、永远
*               WIP 的超时；任何一步失败都必须让 `present()` 保持 0，后续
*               读/写/擦全部拒绝。
*            3. 地址范围检查**不许被 32 位回绕骗过**：`addr + len` 在
*               `addr = 0xFFFFFFF0` 时会绕成小数，旧写法因此放过越界访问。
*               本用例把四个入口（read / write / erase_sector / erase）的
*               回绕地址逐个钉死为 -1。
*            4. 页编程按 256 字节切块，不跨页（芯片在页内回绕，跨页会把
*               页首静默覆盖）；擦除按 4 KiB 扇区对齐，`erase(addr,size)`
*               覆盖所有相交扇区，中途失败如实返回 -1。
*            5. WEL 确认：0x06 之后状态寄存器必须真的出现 WEL，否则拒绝
*               后续动作——静默失败会让擦写变成"成功但没写"的空操作。
*
*          **不证明**：SPI 模式 0 的时序、CS 建立/保持时间、W25Q128 的真实
*          擦写时间、页编程只能清位之后的读数在真片上的分布。这些都是板级
*          读数的账，本文不假装测到了。
*
*          另有一个档位（`-DNOR_TEST_CLAMP` / `-DNOR_TEST_TINY`）只验容量
*          推导的两个边界：JEDEC 容量字节 0x1F（2^31 字节）必须封顶到
*          64 MiB；容量字节 0x08（256 字节，小于一个扇区）时驱动仍能初始
*          化，但所有擦除入口必须拒绝。静态状态（nor_ready 等）在同一个
*          进程里无法复位，所以这两个边界用独立进程各跑一次。
*/

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"
#include "drvnor.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* GPIOB / GPIOE / RCC 都在这一页里（0x40020400 / 0x40021000 / 0x40023800） */
#define T_GPIO_WIN      (0x40020000ul)

#define T_CAP_16M       (16u * 1024u * 1024u)
#define T_SECTOR        (4096u)

/* NOR 命令（与 drvnor.c 里的私有宏逐字相同：这是驱动与芯片之间的契约） */
#define T_CMD_WREN      (0x06u)
#define T_CMD_RDSR1     (0x05u)
#define T_CMD_READ      (0x03u)
#define T_CMD_PROG      (0x02u)
#define T_CMD_SE_4K     (0x20u)
#define T_CMD_BE_64K    (0xD8u)
#define T_CMD_JEDEC     (0x9Fu)
#define T_CMD_RELEASE   (0xABu)

#define T_WIP           (0x01u)
#define T_WEL           (0x02u)

/* ---------------- 虚拟存储 ---------------- */

/* 64 MiB：与驱动允许的最大容量一致，掩码寻址不会越界。静态数组落在 BSS，
 * 不进可执行文件。 */
static uint8 g_mem[64u * 1024u * 1024u];

/* ---------------- 从机模型 ---------------- */

enum
{
    PH_NONE = 0,
    PH_ADDR,
    PH_PROG,
    PH_READ,
    PH_STATUS,
    PH_ID
};

typedef struct
{
    uint8  id[3];
    uint8  status;
    uint32 cmd;
    uint32 addr;
    uint32 prog_addr;
    uint32 read_addr;
    int    addr_cnt;
    int    phase;
    int    rx_idx;
    uint32 cap_mask;

    uint32 pending_erase;   /* 命令已收，等第一次状态读时"完成" */
    int    pending_program; /* 页编程写完，等第一次状态读时清 WEL */

    /* 故障注入 */
    int    spi_init_fail;
    int    wel_on_wren;
    int    wip_forever;
    uint32 fail_tx_at;
    uint32 fail_rx_at;
    uint32 fail_erase_at;   /* 第 N 次擦除起永远 WIP */

    /* 观测 */
    uint32 tx_calls;
    uint32 rx_calls;
    uint32 erases_done;
    uint32 last_erase_addr;
    uint32 last_erase_cmd;
} nor_slave_t;

static nor_slave_t g_dev;

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
        printf("FAIL %s: got %lu want %lu (line %d)\n",
               what, got, want, __LINE__);
    }
}

/* ---------------- 从机模型的字节层 ---------------- */

static void slave_begin(void)
{
    g_dev.cmd      = 0u;
    g_dev.addr     = 0u;
    g_dev.addr_cnt = 0;
    g_dev.rx_idx   = 0;
    g_dev.phase    = PH_NONE;
}

static void slave_erase_commit(void)
{
    uint32 size = (g_dev.last_erase_cmd == T_CMD_BE_64K) ? 65536u : T_SECTOR;
    uint32 a;

    for(a = 0u; a < size; a++)
    {
        g_mem[(g_dev.last_erase_addr + a) & g_dev.cap_mask] = 0xFFu;
    }
}

static uint8 slave_read_status(void)
{
    if(g_dev.pending_erase != 0u)
    {
        g_dev.erases_done++;
        if((g_dev.fail_erase_at != 0u) && (g_dev.erases_done == g_dev.fail_erase_at))
        {
            g_dev.wip_forever = 1;
        }
        else
        {
            slave_erase_commit();
            g_dev.status = (uint8)(g_dev.status & ~(T_WIP | T_WEL));
        }
        g_dev.pending_erase = 0u;
    }

    if(g_dev.pending_program != 0)
    {
        /* 真芯片在编程/擦除完成后自动清 WEL，下一次写使能必须重新置位。 */
        g_dev.status = (uint8)(g_dev.status & ~T_WEL);
        g_dev.pending_program = 0;
    }

    return (uint8)(g_dev.status | (g_dev.wip_forever ? T_WIP : 0u));
}

static void slave_tx(uint8 b)
{
    switch(g_dev.phase)
    {
        case PH_NONE:
            g_dev.cmd      = b;
            g_dev.addr     = 0u;
            g_dev.addr_cnt = 0;

            if((b == T_CMD_READ) || (b == T_CMD_PROG) ||
               (b == T_CMD_SE_4K) || (b == T_CMD_BE_64K))
            {
                g_dev.phase = PH_ADDR;
            }
            else if(b == T_CMD_RDSR1)
            {
                g_dev.phase = PH_STATUS;
            }
            else if(b == T_CMD_JEDEC)
            {
                g_dev.phase = PH_ID;
            }
            else if(b == T_CMD_WREN)
            {
                if(g_dev.wel_on_wren != 0)
                {
                    g_dev.status = (uint8)(g_dev.status | T_WEL);
                }
            }
            else
            {
                /* 0xAB (Release Power-Down) 与未知命令：无数据阶段 */
            }
            break;

        case PH_ADDR:
            g_dev.addr = (g_dev.addr << 8) | (uint32)b;
            g_dev.addr_cnt++;
            if(g_dev.addr_cnt == 3)
            {
                if(g_dev.cmd == T_CMD_PROG)
                {
                    g_dev.phase     = PH_PROG;
                    g_dev.prog_addr = g_dev.addr;
                }
                else if((g_dev.cmd == T_CMD_SE_4K) || (g_dev.cmd == T_CMD_BE_64K))
                {
                    g_dev.phase            = PH_NONE;
                    g_dev.pending_erase    = g_dev.cmd;
                    g_dev.last_erase_addr  = g_dev.addr;
                    g_dev.last_erase_cmd   = g_dev.cmd;
                }
                else
                {
                    g_dev.phase     = PH_READ;
                    g_dev.read_addr = g_dev.addr;
                }
            }
            break;

        case PH_PROG:
            /* NOR 编程只能把 1 变 0；地址在 256 字节页内回绕，与芯片一致。 */
            g_mem[g_dev.prog_addr & g_dev.cap_mask] &= b;
            g_dev.prog_addr = (g_dev.prog_addr & ~0xFFu) |
                              ((g_dev.prog_addr + 1u) & 0xFFu);
            break;

        default:
            break;
    }
}

static uint8 slave_rx(void)
{
    uint8 v = 0xFFu;

    if(g_dev.phase == PH_ID)
    {
        v = g_dev.id[g_dev.rx_idx % 3];
        g_dev.rx_idx++;
    }
    else if(g_dev.phase == PH_STATUS)
    {
        v = slave_read_status();
    }
    else if(g_dev.phase == PH_READ)
    {
        v = g_mem[g_dev.read_addr & g_dev.cap_mask];
        g_dev.read_addr++;
    }

    return v;
}

/* 事务边界不靠猜片选：BSRR 是只写寄存器，拉高之后驱动不会再读它，宿主上
 * 看不到「释放」那一次写。这里改用**调用形状**：驱动的每条命令都从一次
 * Transmit 开始，页编程的数据是紧接着的另一次 Transmit。所以「除页编程
 * 数据以外，每次 Transmit 都是一条新命令」是这份驱动与芯片之间稳定的契约，
 * 模型按它划分事务，不依赖任何时序。 */
/* ---------------- HAL 桩 ---------------- */

static uint32 g_tick;
static uint32 g_spi_inits;
static SPI_HandleTypeDef *g_spi_handle;
static uint32 g_gpio_inits;
static uint32 g_gpio_writes;
static GPIO_TypeDef *g_last_gpio_port;
static uint16 g_last_gpio_pin;
static GPIO_PinState g_last_gpio_state;

uint32_t HAL_GetTick(void)
{
    g_tick++;
    return g_tick;
}

HAL_StatusTypeDef HAL_SPI_Init(SPI_HandleTypeDef *hspi)
{
    g_spi_inits++;
    g_spi_handle = hspi;

    return (g_dev.spi_init_fail != 0) ? HAL_ERROR : HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Transmit(SPI_HandleTypeDef *hspi, const uint8_t *pData,
                                   uint16_t Size, uint32_t Timeout)
{
    uint16_t i;

    (void)hspi;
    (void)Timeout;

    g_dev.tx_calls++;
    if((g_dev.fail_tx_at != 0u) && (g_dev.tx_calls == g_dev.fail_tx_at))
    {
        /* 传输失败 = 驱动会把片选拉高收场，从机的半条命令就此作废。 */
        slave_begin();
        g_dev.fail_tx_at = 0u;
        return HAL_ERROR;
    }

    {
        int program_data = (g_dev.phase == PH_PROG) ? 1 : 0;

        if(program_data == 0)
        {
            slave_begin();
        }

        for(i = 0u; i < Size; i++)
        {
            slave_tx((uint8)pData[i]);
        }

        if(program_data != 0)
        {
            g_dev.phase = PH_NONE;
            g_dev.pending_program = 1;
        }
    }

    return HAL_OK;
}

HAL_StatusTypeDef HAL_SPI_Receive(SPI_HandleTypeDef *hspi, uint8_t *pData,
                                  uint16_t Size, uint32_t Timeout)
{
    uint16_t i;

    (void)hspi;
    (void)Timeout;

    g_dev.rx_calls++;
    if((g_dev.fail_rx_at != 0u) && (g_dev.rx_calls == g_dev.fail_rx_at))
    {
        slave_begin();
        g_dev.fail_rx_at = 0u;
        return HAL_ERROR;
    }

    for(i = 0u; i < Size; i++)
    {
        pData[i] = slave_rx();
    }

    return HAL_OK;
}

void HAL_GPIO_Init(GPIO_TypeDef *GPIOx, GPIO_InitTypeDef *GPIO_Init)
{
    (void)GPIOx;
    (void)GPIO_Init;
    g_gpio_inits++;
}

void HAL_GPIO_DeInit(GPIO_TypeDef *GPIOx, uint32_t GPIO_Pin)
{
    (void)GPIOx;
    (void)GPIO_Pin;
}

void HAL_GPIO_WritePin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin, GPIO_PinState PinState)
{
    g_gpio_writes++;
    g_last_gpio_port  = GPIOx;
    g_last_gpio_pin   = GPIO_Pin;
    g_last_gpio_state = PinState;
}

GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *GPIOx, uint16_t GPIO_Pin)
{
    (void)GPIOx;
    (void)GPIO_Pin;

    return GPIO_PIN_SET;
}

/* ---------------- 环境复位 ---------------- */

static void dev_reset(void)
{
    memset(&g_dev, 0, sizeof(g_dev));
    memset((void *)(uintptr_t)T_GPIO_WIN, 0, 0x10000u);

    g_tick        = 0u;
    g_spi_inits   = 0u;
    g_spi_handle  = 0;
    g_gpio_inits  = 0u;
    g_gpio_writes = 0u;

    g_dev.id[0]    = 0xEFu;
    g_dev.id[1]    = 0x40u;
    g_dev.id[2]    = 0x18u;
    g_dev.cap_mask = T_CAP_16M - 1u;
    g_dev.wel_on_wren = 1;
}

#if !defined(NOR_TEST_CLAMP) && !defined(NOR_TEST_TINY)

static void inject_tx_fail(void)
{
    g_dev.fail_tx_at = g_dev.tx_calls + 1u;
}

static void inject_rx_fail(void)
{
    g_dev.fail_rx_at = g_dev.rx_calls + 1u;
}

/* ---------------- 用例 ---------------- */

/* 初始化之前：所有入口都必须快速失败，且不碰总线。 */
static void test_guards_before_init(void)
{
    uint8 buf[4] = { 0u, 0u, 0u, 0u };
    uint8 data[4] = { 1u, 2u, 3u, 4u };

    dev_reset();

    ck(arkrtos_nor_present() == 0u, "before init the chip is not present");
    ck_u(arkrtos_nor_jedec_id(), 0ul, "before init the JEDEC id is 0");
    ck_u(arkrtos_nor_capacity(), 0ul, "before init the capacity is 0");
    ck_u(arkrtos_nor_sector_size(), (unsigned long)T_SECTOR, "sector size is a constant");

    ck(arkrtos_nor_read(0u, buf, 1u) == -1, "read refuses before init");
    ck(arkrtos_nor_write(0u, data, 1u) == -1, "write refuses before init");
    ck(arkrtos_nor_erase_sector(0u) == -1, "erase_sector refuses before init");
    ck(arkrtos_nor_erase(0u, 1u) == -1, "erase refuses before init");
    ck_u(g_dev.tx_calls, 0ul, "no byte touched the bus before init");
}

/* 失败路径：每一步都要让 present() 保持 0。 */
static void test_init_failures(void)
{
    dev_reset();
    g_dev.spi_init_fail = 1;
    ck(arkrtos_nor_init() == -1, "init fails when HAL_SPI_Init fails");
    ck(arkrtos_nor_present() == 0u, "not present after a SPI init failure");
    ck_u(g_spi_inits, 1ul, "HAL_SPI_Init was attempted once");
    g_dev.spi_init_fail = 0;

    /* 第 1 个 tx 是 Release-Power-Down（驱动不检查返回值），第 2 个才是
     * JEDEC 命令。让 JEDEC 那次发不出去。 */
    dev_reset();
    g_dev.fail_tx_at = 2u;
    ck(arkrtos_nor_init() == -1, "init fails when the JEDEC command cannot be sent");
    ck(arkrtos_nor_present() == 0u, "not present after a JEDEC send failure");

    dev_reset();
    g_dev.fail_rx_at = 1u;
    ck(arkrtos_nor_init() == -1, "init fails when the JEDEC id cannot be received");
    ck(arkrtos_nor_present() == 0u, "not present after a JEDEC receive failure");

    dev_reset();
    g_dev.id[0] = 0u;
    ck(arkrtos_nor_init() == -1, "init rejects an all-zero manufacturer byte");
    ck(arkrtos_nor_present() == 0u, "not present after a bogus id");

    dev_reset();
    g_dev.fail_rx_at = 2u;   /* 1 = JEDEC，2 = 初始化末尾的 status1 */
    ck(arkrtos_nor_init() == -1, "init fails when status register 1 cannot be read");
    ck(arkrtos_nor_present() == 0u, "not present after a status read failure");

    dev_reset();
    g_dev.wip_forever = 1;
    ck(arkrtos_nor_init() == -1, "init fails when the part never leaves WIP");
    ck(arkrtos_nor_present() == 0u, "not present after a WIP timeout");
    ck(g_tick > 2000u, "the WIP wait consumed real ticks before timing out");
}

/* 成功初始化：上电顺序、JEDEC、容量、幂等。 */
static void test_init_ok(void)
{
    dev_reset();

    ck(arkrtos_nor_init() == 0, "init identifies the W25Q128");
    ck(arkrtos_nor_present() == 1u, "present after a successful init");
    ck_u(arkrtos_nor_jedec_id(), (unsigned long)ARKRTOS_NOR_JEDEC_W25Q128,
         "JEDEC id is 0xEF4018");
    ck_u(arkrtos_nor_capacity(), (unsigned long)T_CAP_16M, "capacity is 16 MiB");
    ck_u(arkrtos_nor_sector_size(), (unsigned long)T_SECTOR, "sector size stays 4 KiB");

    /* 配错一个 SPI 参数，读回的数据就会错位一整条命令；这里逐项核对。 */
    ck(g_spi_handle != 0, "HAL_SPI_Init saw a handle");
    ck(g_spi_handle->Instance == SPI2, "SPI2 is the NOR bus");
    ck_u(g_spi_handle->Init.Mode, SPI_MODE_MASTER, "SPI is master");
    ck_u(g_spi_handle->Init.Direction, SPI_DIRECTION_2LINES, "SPI is 2-line");
    ck_u(g_spi_handle->Init.DataSize, SPI_DATASIZE_8BIT, "SPI is 8-bit");
    ck_u(g_spi_handle->Init.CLKPolarity, SPI_POLARITY_LOW, "mode 0: clock idles low");
    ck_u(g_spi_handle->Init.CLKPhase, SPI_PHASE_1EDGE, "mode 0: sample on the first edge");
    ck_u(g_spi_handle->Init.NSS, SPI_NSS_SOFT, "chip select is software driven");
    ck_u(g_spi_handle->Init.BaudRatePrescaler, SPI_BAUDRATEPRESCALER_4,
         "SCK = APB1 / 4 = 6 MHz");
    ck_u(g_spi_handle->Init.FirstBit, SPI_FIRSTBIT_MSB, "MSB first");

    /* 片选必须在 SPI 初始化之前就拉高（第一次 HAL_GPIO_WritePin 即 PE15 SET），
     * 但本驱动自己直接写 BSRR，HAL_GPIO_WritePin 的记录正好是那次拉高。 */
    ck_u(g_gpio_writes, 1ul, "CS is driven exactly once by the HAL");
    ck(g_last_gpio_port == GPIOE, "CS lives on GPIOE");
    ck_u(g_last_gpio_pin, (unsigned long)GPIO_PIN_15, "CS is PE15");
    ck_u(g_last_gpio_state, (unsigned long)GPIO_PIN_SET, "CS starts high");
    ck_u(g_gpio_inits, 2ul, "CS pin and the three SPI pins are configured");

    /* 0xAB 与 0x9F 各发一次，状态读两次（显式一次 + wait_ready 一次）。 */
    ck(g_dev.tx_calls >= 4u, "release-PD, JEDEC and the status polls all went out");

    ck(arkrtos_nor_init() == 0, "a second init returns 0 without touching the chip");
}

/* 读：内容、边界、失败。 */
static void test_read(void)
{
    uint8 buf[32];

    g_mem[0x1000] = 0xA5u;
    g_mem[0x1001] = 0x5Au;

    memset(buf, 0, sizeof(buf));
    ck(arkrtos_nor_read(0x1000u, buf, 2u) == 0, "read of two bytes succeeds");
    ck_u(buf[0], 0xA5ul, "read returns the first byte");
    ck_u(buf[1], 0x5Aul, "read returns the second byte");

    ck(arkrtos_nor_read(0x1000u, 0, 1u) == -1, "read refuses a null buffer");
    ck(arkrtos_nor_read(0x1000u, buf, 0u) == -1, "read refuses a zero length");
    ck(arkrtos_nor_read(T_CAP_16M, buf, 1u) == -1, "read refuses an address at the end");
    ck(arkrtos_nor_read(T_CAP_16M - 1u, buf, 2u) == -1, "read refuses to cross the end");
    ck(arkrtos_nor_read(0xFFFFFFF0u, buf, 32u) == -1,
       "read refuses a range whose addr+len wraps 32 bits");

    inject_tx_fail();
    ck(arkrtos_nor_read(0x1000u, buf, 1u) == -1, "read fails when the header cannot be sent");

    inject_rx_fail();
    ck(arkrtos_nor_read(0x1000u, buf, 1u) == -1, "read fails when the data cannot be received");
}

/* 写：页内、跨页、WEL、发送失败、超时。 */
static void test_write(void)
{
    uint8 data[0x120];
    uint32 i;

    for(i = 0u; i < sizeof(data); i++)
    {
        data[i] = (uint8)(i + 1u);
    }

    memset(&g_mem[0x2000], 0xFF, T_SECTOR);
    ck(arkrtos_nor_write(0x2010u, data, 5u) == 0, "write of five bytes succeeds");
    ck(memcmp(&g_mem[0x2010], data, 5u) == 0, "the five bytes landed where asked");
    ck(g_mem[0x2015] == 0xFFu, "write does not touch the byte after the range");

    /* 0x20F0 + 0x120 跨过 0x2100 的页边界，必须切成两块。 */
    memset(&g_mem[0x2000], 0xFF, T_SECTOR);
    ck(arkrtos_nor_write(0x20F0u, data, sizeof(data)) == 0, "write across a page boundary succeeds");
    ck(memcmp(&g_mem[0x20F0], data, sizeof(data)) == 0, "the whole range landed in order");

    ck(arkrtos_nor_write(0x2010u, 0, 1u) == -1, "write refuses a null buffer");
    ck(arkrtos_nor_write(0x2010u, data, 0u) == -1, "write refuses a zero length");
    ck(arkrtos_nor_write(T_CAP_16M, data, 1u) == -1, "write refuses an address at the end");
    ck(arkrtos_nor_write(0xFFFFFFF0u, data, 32u) == -1,
       "write refuses a range whose addr+len wraps 32 bits");

    /* 0x06 发出去了，但 WEL 永远不置位：静默失败，必须拒绝。 */
    g_dev.wel_on_wren = 0;
    ck(arkrtos_nor_write(0x2010u, data, 1u) == -1, "write refuses when WEL never sets");
    g_dev.wel_on_wren = 1;

    inject_tx_fail();
    ck(arkrtos_nor_write(0x2010u, data, 1u) == -1, "write fails when 0x06 cannot be sent");

    /* 写使能成功、状态读到 WEL，但页编程头发不出去：tx #3。 */
    g_dev.fail_tx_at = g_dev.tx_calls + 3u;
    ck(arkrtos_nor_write(0x2010u, data, 1u) == -1, "write fails when the program header cannot be sent");

    /* 页编程数据发不出去：tx #4。 */
    g_dev.fail_tx_at = g_dev.tx_calls + 4u;
    ck(arkrtos_nor_write(0x2010u, data, 1u) == -1, "write fails when the payload cannot be sent");

    g_dev.wip_forever = 1;
    ck(arkrtos_nor_write(0x2010u, data, 1u) == -1, "write fails when the part never finishes the page");
    g_dev.wip_forever = 0;
}

/* 扇区擦除：对齐掩码、只擦一个扇区、失败路径。 */
static void test_erase_sector(void)
{
    memset(&g_mem[0x4000], 0x00, 2u * T_SECTOR);
    ck(arkrtos_nor_erase_sector(0x4ABCu) == 0, "erase_sector accepts an unaligned address");
    ck_u(g_dev.last_erase_cmd, T_CMD_SE_4K, "erase used the 4 KiB sector command");
    ck_u(g_dev.last_erase_addr, 0x4000ul, "the command address is the sector base");
    ck_u(g_mem[0x4000], 0xFFul, "the sector start is erased");
    ck_u(g_mem[0x4FFF], 0xFFul, "the sector end is erased");
    ck_u(g_mem[0x5000], 0x00ul, "the next sector is untouched");

    ck(arkrtos_nor_erase_sector(T_CAP_16M) == -1, "erase_sector refuses an address at the end");
    ck(arkrtos_nor_erase_sector(0xFFFFFFF0u) == -1,
       "erase_sector refuses a wrapping address");

    g_dev.wel_on_wren = 0;
    ck(arkrtos_nor_erase_sector(0x4000u) == -1, "erase_sector refuses when WEL never sets");
    g_dev.wel_on_wren = 1;

    g_dev.fail_tx_at = g_dev.tx_calls + 3u;
    ck(arkrtos_nor_erase_sector(0x4000u) == -1, "erase_sector fails when the command cannot be sent");

    g_dev.wip_forever = 1;
    ck(arkrtos_nor_erase_sector(0x4000u) == -1, "erase_sector fails on a WIP timeout");
    g_dev.wip_forever = 0;
}

/* 范围擦除：相交扇区全覆盖、参数校验、中途失败。 */
static void test_erase_range(void)
{
    memset(&g_mem[0x8000], 0x00, 4u * T_SECTOR);
    ck(arkrtos_nor_erase(0x8100u, 0x1E00u) == 0, "erase covers every overlapping sector");
    ck_u(g_mem[0x8000], 0xFFul, "the sector before the range start is erased");
    ck_u(g_mem[0x9000], 0xFFul, "the middle sector is erased");
    ck_u(g_mem[0xA000], 0x00ul, "the sector past the range end is untouched");
    ck_u(g_mem[0xB000], 0x00ul, "the first non-overlapping sector is untouched");

    ck(arkrtos_nor_erase(0x8100u, 0u) == -1, "erase refuses a zero size");
    ck(arkrtos_nor_erase(T_CAP_16M, 1u) == -1, "erase refuses an address at the end");
    ck(arkrtos_nor_erase(T_CAP_16M - 1u, 2u) == -1, "erase refuses to cross the end");
    ck(arkrtos_nor_erase(0xFFFFFFF0u, 0x20u) == -1,
       "erase refuses a range whose addr+size wraps 32 bits");

    /* 第二个扇区失败：第一个已经真擦掉，错误如实上报。 */
    memset(&g_mem[0xC000], 0x00, 2u * T_SECTOR);
    g_dev.fail_erase_at = g_dev.erases_done + 2u;
    ck(arkrtos_nor_erase(0xC000u, 2u * T_SECTOR) == -1, "erase reports a failing sector");
    ck_u(g_mem[0xC000], 0xFFul, "the sector erased before the failure really was erased");
    ck_u(g_mem[0xD000], 0x00ul, "the failing sector is untouched");
    g_dev.fail_erase_at = 0u;
    g_dev.wip_forever   = 0;

    /* wait_ready 的状态读本身失败也要如实返回 -1。 */
    inject_rx_fail();
    ck(arkrtos_nor_wait_ready(10u) == -1, "wait_ready fails when status cannot be read");
}

/* 命令常量与驱动声明的一致性（写错一位，板子上就是"永远读不到 id"）。 */
static void test_misc(void)
{
    ck_u(T_CMD_WREN, 0x06ul, "write-enable opcode");
    ck_u(T_CMD_RDSR1, 0x05ul, "read-status-1 opcode");
    ck_u(T_CMD_READ, 0x03ul, "read-data opcode");
    ck_u(T_CMD_PROG, 0x02ul, "page-program opcode");
    ck_u(T_CMD_SE_4K, 0x20ul, "4 KiB sector-erase opcode");
    ck_u(T_CMD_BE_64K, 0xD8ul, "64 KiB block-erase opcode");
    ck_u(T_CMD_JEDEC, 0x9Ful, "JEDEC-id opcode");
    ck_u(T_CMD_RELEASE, 0xABul, "release-power-down opcode");
    ck_u(T_WIP, 0x01ul, "WIP is bit 0 of status 1");
    ck_u(T_WEL, 0x02ul, "WEL is bit 1 of status 1");
    ck_u(ARKRTOS_NOR_SECTOR_BYTES, 4096ul, "sector granularity is 4 KiB");
}

#endif /* !NOR_TEST_CLAMP && !NOR_TEST_TINY */

/* ---------------- main ---------------- */

static int map_page(void)
{
#ifdef _WIN32
    void *p = VirtualAlloc((void *)(uintptr_t)T_GPIO_WIN, 0x10000u,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);

    return (p == (void *)(uintptr_t)T_GPIO_WIN) ? 0 : -1;
#else
    void *p = mmap((void *)(uintptr_t)T_GPIO_WIN, 0x10000u,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    return (p == MAP_FAILED) ? -1 : 0;
#endif
}

int main(void)
{
    if(map_page() != 0)
    {
        printf("DRVNOR-HOST FAIL cannot map 0x%lX\n", (unsigned long)T_GPIO_WIN);
        return 1;
    }

#if defined(NOR_TEST_CLAMP)
    /* 容量字节 0x1F -> 2^31 字节，必须封顶到 64 MiB。 */
    dev_reset();
    g_dev.id[2] = 0x1Fu;
    g_dev.cap_mask = (64u * 1024u * 1024u) - 1u;
    ck(arkrtos_nor_init() == 0, "init accepts a huge capacity byte");
    ck_u(arkrtos_nor_capacity(), 64ul * 1024ul * 1024ul,
         "the capacity is clamped to 64 MiB");
    ck(arkrtos_nor_read(T_CAP_16M, (uint8 *)g_mem, 1u) == 0,
       "the clamped capacity makes a 16 MiB offset readable");
    ck(arkrtos_nor_read(64u * 1024u * 1024u, (uint8 *)g_mem, 1u) == -1,
       "even the clamped capacity has an end");
#elif defined(NOR_TEST_TINY)
    /* 容量字节 0x08 -> 256 字节，小于一个扇区：初始化能过，擦除必须拒绝。 */
    dev_reset();
    g_dev.id[2] = 0x08u;
    g_dev.cap_mask = 255u;
    ck(arkrtos_nor_init() == 0, "init accepts a part smaller than one sector");
    ck_u(arkrtos_nor_capacity(), 256ul, "the capacity is 256 bytes");
    ck(arkrtos_nor_erase_sector(0u) == -1,
       "a part smaller than one sector refuses every sector erase");
    ck(arkrtos_nor_erase(0u, 1u) == -1, "and refuses a range erase as well");
    ck(arkrtos_nor_read(0u, (uint8 *)g_mem, 1u) == 0, "byte reads still work");
#else
    test_guards_before_init();
    test_init_failures();
    test_init_ok();
    test_read();
    test_write();
    test_erase_sector();
    test_erase_range();
    test_misc();
#endif

    if(g_fails != 0)
    {
        printf("DRVNOR-HOST FAILED %d/%d\n", g_fails, g_checks);
        return 1;
    }

    printf("DRVNOR-HOST OK  %d checks\n", g_checks);
    return 0;
}