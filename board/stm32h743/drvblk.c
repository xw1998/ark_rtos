/**
* @file drvblk.c
* @brief STM32H743 board level block device registration - see drvblk.h
* @details One thin adapter for the internal flash image pool. The pool is
*          memory mapped, so a read is a memcpy; write and erase must go
*          through the port layer (drvflash.c) because the flash controller
*          needs its own sequence.
*
*          Nothing above this line needs to know which silicon it is talking
*          to - that is the whole point of the adapter.
*
* @author xw
* @date 2026.11.04
*/

#include "arkrtos_blk.h"
#include "arkrtos_partition.h"
#include "arkrtos_hal.h"
#include "drvblk.h"

#if (ARKRTOS_USE_QSPI == 1)
#include "drvqspi.h"
#endif

#include <string.h>

/* ============================================================
 * int0 - internal flash image pool
 * ============================================================ */
static int32 blk_int_read(uint32 off, uint8 *buf, uint32 len)
{
    memcpy(buf, (const void *)(IMAGE_POOL_BASE + off), len);
    return 0;
}

static int32 blk_int_write(uint32 off, const uint8 *buf, uint32 len)
{
    return arkrtos_port_flash_write((uint32)(IMAGE_POOL_BASE + off), buf, len);
}

static int32 blk_int_erase(uint32 off, uint32 len)
{
    return arkrtos_port_flash_erase((uint32)(IMAGE_POOL_BASE + off), len);
}

static const arkrtos_blk_dev_t g_blk_int =
{
    "int0",
    (uint32)IMAGE_POOL_SIZE,
    (uint32)IMAGE_POOL_SECTOR,
    0,                              /* bringup: always powered and mapped */
    0,                              /* size_fn: compile-time constant       */
    blk_int_read,
    blk_int_write,
    blk_int_erase
};

#if (ARKRTOS_USE_QSPI == 1)
/* ============================================================
 * nor0 - external QSPI NOR (W25Q64, 8 MiB, 4 KiB sectors)
 * ------------------------------------------------------------
 * bringup 是真的去读一次 JEDEC ID：注册这个设备等于声明"板上有一块 8 MiB"，
 * 而"有没有这块芯片"只能由芯片自己回答。ID 不对就返回 -1，块设备保持
 * 不可用——上层看到的是"没有卷"，而不是"每次访问都失败"。
 * ============================================================ */
static int32 blk_nor_bringup(void)
{
    return (arkrtos_board_nor_init() == 0) ? 0 : -1;
}

static int32 blk_nor_read(uint32 off, uint8 *buf, uint32 len)
{
    return (arkrtos_board_nor_read(off, buf, len) == 0) ? 0 : -1;
}

static int32 blk_nor_write(uint32 off, const uint8 *buf, uint32 len)
{
    return (arkrtos_board_nor_write(off, buf, len) == 0) ? 0 : -1;
}

static int32 blk_nor_erase(uint32 off, uint32 len)
{
    return (arkrtos_board_nor_erase(off, len) == 0) ? 0 : -1;
}

static const arkrtos_blk_dev_t g_blk_nor =
{
    "nor0",
    (uint32)ARKRTOS_NOR_SIZE,
    4096u,                          /* W25Q64 的最小擦除单位 */
    blk_nor_bringup,                /* 读 JEDEC 才认为设备在 */
    0,                              /* size_fn: 固定 8 MiB          */
    blk_nor_read,
    blk_nor_write,
    blk_nor_erase
};
#endif /* ARKRTOS_USE_QSPI */

int32 arkrtos_board_blk_init(void)
{
    if(arkrtos_blk_register(&g_blk_int) != 0)
    {
        return -1;
    }

#if (ARKRTOS_USE_QSPI == 1)
    if(arkrtos_blk_register(&g_blk_nor) != 0)
    {
        return -1;
    }
#endif

    return 0;
}
