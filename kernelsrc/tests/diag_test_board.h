/**
* @file diag_test_board.h
* @brief Host-test view of board/qemu_loongarch64 for the diag command family.
*
* @details
* What changes: ARKRTOS_USE_MINIAPP only.  The board header sets it to 0 with
* #undef/#define, so a command-line -D cannot raise it; the crash command's
* MiniApp journal branch (path-hash lookup through arkrtos_mini_*) is only
* compiled when it is 1, and that branch is part of what this case measures.
* Wrapping the board header is the same mechanism host_board_la64.h uses to
* move CHIP_FLASH_BASE, and it is contained to this file.
*
* Everything else (task count, shell size, guard/crash/audit/utest/sec/fdt/
* vfs/log) still comes from the real qemu_loongarch64 board.  One declaration
* is added (arkrtos_board_fdt_overlay_demo) so the overlay command compiles
* without a warning; the test provides its definition.  RTSTAT and
* TICKLESS are plain -D flags on the command line: the board header does not
* touch them and features.h guards both with #ifndef.
*
* What this is NOT: a board.  No kernel build points ARKRTOS_BOARD_CONFIG
* here; only tools/coverage_gate.py's shelldiag case does.
*/

#ifndef DIAG_TEST_BOARD_H
#define DIAG_TEST_BOARD_H

#include "arkrtos_board_config.h"

#undef  ARKRTOS_USE_MINIAPP
#define ARKRTOS_USE_MINIAPP 1

/* The overlay demo fragment is declared in arkrtos_fdt_overlay.h (added next
 * to arkrtos_fdt_overlay(): the kernel shell must not include a board header,
 * so the board contract lives on the kernel side).  The declaration used to be
 * repeated here as a workaround; it is not any more - the host test only has to
 * provide the definition of the symbol (the stub at the bottom of
 * test_shell_diag_host.c), which it already does. */

#endif /* DIAG_TEST_BOARD_H */
