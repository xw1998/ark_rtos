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

int32 arkrtos_board_blk_init(void)
{
    if(arkrtos_blk_register(&g_blk_int) != 0)
    {
        return -1;
    }

    return 0;
}
