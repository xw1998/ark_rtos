/**
* @file drvblk.c
* @brief STM32F427 board level block device registration - see drvblk.h
* @details Two thin adapters, nothing more. Both backends already existed in
*          some form:
*            - the internal adapter translates "offset in the pool" into the
*              CPU address the port layer expects and hands the work to
*              arkrtos_port_flash_*(), which is the same code path the installer
*              has been using;
*            - the external adapter forwards straight to drvnor.c.
*
*          The point of the adapter is that nothing above this line needs to
*          know which of the two it is talking to.
*
* @author xw
* @date 2026.09.21
*/

#include "arkrtos_blk.h"
#include "arkrtos_partition.h"
#include "arkrtos_hal.h"
#include "drvblk.h"
#include "drvnor.h"

#include <string.h>

/* ============================================================
 * int0 - internal flash image pool
 *
 * Memory mapped, so a read is a memcpy. Write and erase must go through the
 * port layer because the flash controller needs its own sequence.
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

/* ============================================================
 * nor0 - external SPI NOR (W25Q128)
 *
 * Capacity is answered by the chip, not by a macro: the same adapter has to
 * work on a board fitted with a smaller or larger part, and a wrong capacity
 * would let the loader place an image past the end of the device.
 * ============================================================ */
static const arkrtos_blk_dev_t g_blk_nor =
{
    "nor0",
    0u,                             /* unknown until bring-up reads the id  */
    ARKRTOS_NOR_SECTOR_BYTES,
    arkrtos_nor_init,
    arkrtos_nor_capacity,
    arkrtos_nor_read,
    arkrtos_nor_write,
    arkrtos_nor_erase
};

int32 arkrtos_board_blk_init(void)
{
    int32 rc = 0;

    if(arkrtos_blk_register(&g_blk_int) != 0)
    {
        rc = -1;
    }

    if(arkrtos_blk_register(&g_blk_nor) != 0)
    {
        rc = -1;
    }

    return rc;
}
