/**
* @file drvflash.c
* @brief STM32H743 片内 Flash 擦写驱动实现（HAL 库）
* @details H743XI 是 2MB 双 bank 器件，每个 bank 8 个 128KB 扇区：
*            bank1  0x08000000 ~ 0x080FFFFF  sector 0~7
*            bank2  0x08100000 ~ 0x081FFFFF  sector 8~15（HAL 口径仍写 0~7 + BANK_2）
*
*          两处与 F4 时代不同的硬约束：
*          1) 擦除必须按 bank 单独下发（H7 的 SER/SNB 位在 CR1 与 CR2 上各有一份，
*             一次调用同时指定两个 bank 只会把同一个扇区号在两个 bank 上各擦一遍）；
*          2) 编程粒度是 256 bit flash word（32 字节），地址与源缓冲都必须 32 字节
*             对齐，字节级编程在 H7 上不存在。因此写入按「读-改-写」逐 32 字节块
*             进行：读出当前内容（擦除后是全 0xFF）、覆盖本次要写的字节、再整块编程。
*             代价是同一个 32 字节块只能被写一次（未擦除的块无法二次编程），
*             这与「镜像按顺序写入已擦除的池」的用法一致。
*
*          地址不写死在本文件：调用方的地址来自 config/stm32h743/arkrtos_partition.h。
*/

#include "drvflash.h"
#include "arkrtos_config.h"
#include "arkrtos_partition.h"    /* 全工程唯一地址源头：CHIP_FLASH_BASE */

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG      /* 引入 stm32h7xx.h → HAL 头文件 */
#endif

#define ARKRTOS_FLASH_BASE_ADDR     ((uint32)CHIP_FLASH_BASE)
#define ARKRTOS_FLASH_SECTOR_BYTES  (128u * 1024u)   /* H743 全片统一 128KB 扇区 */
#define ARKRTOS_FLASH_BANK_SECTORS  (8u)             /* 每个 bank 8 个扇区 */
#define ARKRTOS_FLASH_TOTAL_SIZE    ((uint32)CHIP_FLASH_SIZE)
#define ARKRTOS_FLASH_WORD_BYTES    (32u)            /* 256 bit flash word */

/* HAL 把「擦哪个 bank」编码在 FLASH_BANK_x 上，扇区号在 bank 内从 0 数。 */
static void flash_bank_of(uint32 idx, uint32 *p_bank, uint32 *p_sector)
{
    if(idx < ARKRTOS_FLASH_BANK_SECTORS)
    {
        *p_bank   = FLASH_BANK_1;
        *p_sector = idx;
    }
    else
    {
        *p_bank   = FLASH_BANK_2;
        *p_sector = idx - ARKRTOS_FLASH_BANK_SECTORS;
    }
}

/**
* @brief 计算地址所在的扇区号（0 ~ 15，跨 bank 连续编号）
* @return 扇区号；地址非法返回 0xFFFFFFFF
*/
static uint32 arkrtos_flash_sector_index(uint32 addr)
{
    uint32 offset;

    if(addr < ARKRTOS_FLASH_BASE_ADDR)
    {
        return 0xFFFFFFFFu;
    }

    offset = addr - ARKRTOS_FLASH_BASE_ADDR;

    /* 上界必须挡：越界地址按公式会算出「合法」扇区号并被交给 HAL 去擦。 */
    if(offset >= ARKRTOS_FLASH_TOTAL_SIZE)
    {
        return 0xFFFFFFFFu;
    }

    return offset / ARKRTOS_FLASH_SECTOR_BYTES;
}

uint32 arkrtos_port_flash_sector_size(uint32 addr)
{
    if(arkrtos_flash_sector_index(addr) == 0xFFFFFFFFu)
    {
        return 0u;
    }
    return ARKRTOS_FLASH_SECTOR_BYTES;
}

int32 arkrtos_port_flash_erase(uint32 addr, uint32 size)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32 first;
    uint32 last;
    uint32 idx;
    uint32_t sector_error = 0;   /* HAL 接口要求 uint32_t（与内核 uint32 是不同类型） */
    int32  ret = 0;

    if(size == 0u)
    {
        return 0;
    }

    first = arkrtos_flash_sector_index(addr);
    last  = arkrtos_flash_sector_index(addr + size - 1u);

    if((first == 0xFFFFFFFFu) || (last == 0xFFFFFFFFu))
    {
        return -1;
    }

    HAL_FLASH_Unlock();

    for(idx = first; idx <= last; idx++)
    {
        flash_bank_of(idx, &erase_init.Banks, &erase_init.Sector);

        erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
        erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;    /* 2.7V ~ 3.6V */
        erase_init.NbSectors    = 1u;

        if(HAL_FLASHEx_Erase(&erase_init, &sector_error) != HAL_OK)
        {
            ret = -1;
            break;
        }
    }

    HAL_FLASH_Lock();
    return ret;
}

int32 arkrtos_port_flash_write(uint32 addr, const uint8 *data, uint32 len)
{
    /* 源缓冲必须 32 字节对齐：H7 的 flash word 编程由硬件按 256 bit 读取。 */
    static uint8 word_buf[ARKRTOS_FLASH_WORD_BYTES] __attribute__((aligned(32)));
    int32 ret = 0;

    if((data == 0) || (len == 0u))
    {
        return 0;
    }

    /* 越界（含跨越 Flash 末尾的半块）直接拒绝，不交给 HAL 去猜。 */
    if(((addr + len) > (ARKRTOS_FLASH_BASE_ADDR + ARKRTOS_FLASH_TOTAL_SIZE)) ||
       (addr < ARKRTOS_FLASH_BASE_ADDR))
    {
        return -1;
    }

    HAL_FLASH_Unlock();

    while(len > 0u)
    {
        uint32 block  = addr & ~(ARKRTOS_FLASH_WORD_BYTES - 1u);
        uint32 offset = addr - block;
        uint32 n      = ARKRTOS_FLASH_WORD_BYTES - offset;
        uint32 i;

        if(n > len)
        {
            n = len;
        }

        for(i = 0u; i < ARKRTOS_FLASH_WORD_BYTES; i++)
        {
            word_buf[i] = *(volatile const uint8 *)(block + i);
        }
        for(i = 0u; i < n; i++)
        {
            word_buf[offset + i] = data[i];
        }

        if(HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, block,
                             (uint32)word_buf) != HAL_OK)
        {
            ret = -1;
            break;
        }

        addr += n;
        data += n;
        len  -= n;
    }

    HAL_FLASH_Lock();
    return ret;
}
