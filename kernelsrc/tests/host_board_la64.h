/**
* @file host_board_la64.h
* @brief Host-test view of board/qemu_loongarch64: same board, one address moved.
*
* @details
* Why this exists: the pmap self-test (cmd_pmap "test") treats the kernel ROM
* base as a physical address and dereferences it directly, so a host test has
* to be able to map that address.  The real board value is 0x00200000, and a
* freshly started Windows process has its main-thread stack reserved exactly
* there (measure it: VirtualQuery(0x00200000) reports MEM_RESERVE of ~1 MB,
* owned by the loader, and neither VirtualAlloc nor MapViewOfFileEx can take
* it).  The base is a compile-time constant derived from CHIP_FLASH_BASE in
* config/arkrtos_partition.h and has no override hook, so moving it here is
* the only way to exercise the direct-dereference paths on the host.
*
* What changes: CHIP_FLASH_BASE only, and only after the board header has run,
* so every switch, task count and shell size still comes from the real
* qemu_loongarch64 board.  Measured on this host, a fresh process lays out
* 0x00200000 stack, 0x00400000 image, 0x00500000 heap; 0x00A90000-0x75A40000
* is free, so 0x02000000 is 2 MB-aligned, 16 MB below the self-test VA
* (0x03000000) and outside every loader reservation.
*
* What this is NOT: a board.  No kernel build ever points
* ARKRTOS_BOARD_CONFIG here; only tools/coverage_gate.py's shellpmap case does,
* and the test file says so in its own header.
*
* @author xw
*/

#ifndef HOST_BOARD_LA64_H
#define HOST_BOARD_LA64_H

#include "arkrtos_board_config.h"

#undef  CHIP_FLASH_BASE
#define CHIP_FLASH_BASE 0x02000000u

#endif /* HOST_BOARD_LA64_H */
