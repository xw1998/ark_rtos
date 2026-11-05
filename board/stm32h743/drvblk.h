/**
* @file drvblk.h
* @brief STM32H743 board level block device registration
* @details Registers the non-volatile backends this board has *today*:
*
*            int0 - the internal flash image pool region (see
*                   config/stm32h743/arkrtos_partition.h). Memory mapped,
*                   erased in 128 KiB sectors.
*
*          The external QSPI NOR (U?, W25Q64, 8 MiB) is registered as `nor0`
*          when USE_QSPI=1, and only then: the descriptor's bring-up reads the
*          JEDEC ID, so "the board has 8 MiB" is never claimed before the chip
*          itself has answered. An unanswered chip is reported as absent, not
*          as a device that fails every access. Read/write/erase go through
*          drvqspi.c (bit-banged, see the note at the top of that file); its
*          sector size is the chip's 4 KiB, not the internal flash's 128 KiB.
*
*          The descriptor does not touch hardware at registration time:
*          bring-up is deferred to the first arkrtos_blk_init() call.
*
* @author xw
* @date 2026.11.04
*/

#ifndef __DRVBLK_H__
#define __DRVBLK_H__

#include "arkrtos_types.h"

/**
* @brief Register the board backends with the kernel block device registry
* @return 0 = registered, -1 = registration was refused
* @note  Called once from arkrtos_dev_board_init(); no hardware is touched.
*/
int32 arkrtos_board_blk_init(void);

#endif /* __DRVBLK_H__ */
