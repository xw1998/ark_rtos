/**
* @file drvblk.h
* @brief STM32H743 board level block device registration
* @details Registers the non-volatile backends this board has *today*:
*
*            int0 - the internal flash image pool region (see
*                   config/stm32h743/arkrtos_partition.h). Memory mapped,
*                   erased in 128 KiB sectors.
*
*          The external QSPI NOR (U?, W25Q64, 8 MiB) is described in
*          dts/stm32h743.dts but not registered yet: the QUADSPI driver is
*          the first item of M2. Registering a device with no driver behind
*          it would answer "the board has 8 MiB" and then fail every access,
*          which is exactly the kind of wrong answer this project does not
*          give - absent is reported as absent.
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
