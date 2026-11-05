/**
* @file drvfmc.h
* @brief STM32H743 board level external SDRAM (FMC SDRAM Bank1) bring-up
* @details The board carries a Winbond W9825G6KH-6: 32 MiB, 16-bit bus, four
*          banks, 13 row address bits (A0..A12) and 9 column address bits
*          (A0..A8), wired to FMC SDRAM Bank1 (SDNE0 / SDCKE0), therefore
*          memory mapped at 0xC0000000.  W9812G6KH (16 MiB, 12 row bits) and
*          the ISSI IS42S16xxx parts share the footprint; switching to one of
*          them is a change of the geometry macros in
*          config/stm32h743/arkrtos_partition.h, not of this driver.
*
*          Nothing here is registered as a device: SDRAM is plain memory, it
*          has no kernel-visible handle.  The only runtime surface is the
*          "sdram" shell command, which reports the geometry and can run a
*          full-chip self test (that is how the wiring and the timing were
*          verified on the bench: by reading back every byte, not by looking
*          at a signal).
*
* @author xw
* @date 2026.11.05
*/

#ifndef __DRVFMC_H__
#define __DRVFMC_H__

#include "arkrtos_types.h"

/**
* @brief Bring the FMC SDRAM controller up and run the JEDEC init sequence
* @return 0 = ready, negative = the first step that failed
* @note  Must run before anything touches the SDRAM region (the arkc arena and
*        the ramfs file buffers live there).  Called from
*        arkrtos_port_board_init(), which the kernel calls before any module
*        init.
*/
int32 arkrtos_sdram_init(void);

/**
* @brief Prove the window: write one word at the far end and read it back
* @return 0 = verified, -1 = not configured, -2 = the window did not give the
*         word back
* @details Separate from arkrtos_sdram_init() on purpose.  Init is called from
*          board init, before the fault handlers and the console exist, so it
*          may only do things that cannot fault; a bus error there leaves the
*          board silent with nothing on the UART to explain it.  Anything that
*          actually touches the window belongs here, where a failure is
*          reported (as PANIC BUS with the address) instead of swallowed.
*          If the memory path is wedged, this call does not return - the
*          fault handler runs.  That is the honest outcome, not a defect.
*/
int32 arkrtos_sdram_verify(void);

/**
* @brief 1 = verified (data may be placed in the SDRAM), 0 = not verified
* @note  The distinction is what keeps "the registers look right" from being
*        reported as "the memory works": the arkc arena and the ramfs buffers
*        are only moved there when this says 1.
*/
int32 arkrtos_sdram_ready(void);

/** @brief 1 = controller programmed (whether or not the window answers) */
int32 arkrtos_sdram_configured(void);

/**
* @brief Step number at which the last init stopped (0 = succeeded)
* @details Board init runs before the console is up, so a failed bring-up
*          cannot report itself through a log line; this is what the shell
*          command and the boot path read instead.
*          -1 = this board declares no SDRAM, -2 = SDCLK out of range,
*          -3..-7 = the JEDEC step that never completed, -8 = the controller
*          did not report Normal mode.  A window that answers nothing is not
*          in here: bring-up never touches the window (see
*          arkrtos_sdram_verify).
*/
int32 arkrtos_sdram_last_error(void);

/** @brief Geometry actually programmed, for logs and the shell command */
typedef struct
{
    uint32 base;          /**< memory mapped base (CHIP_SDRAM_BASE)   */
    uint32 size;          /**< capacity in bytes (CHIP_SDRAM_SIZE)    */
    uint32 sdclk_mhz;     /**< effective SDCLK frequency              */
    uint32 hclk_mhz;      /**< FMC kernel clock (HCLK3)               */
    uint32 col_bits;      /**< column address bits programmed         */
    uint32 row_bits;      /**< row address bits programmed            */
    uint32 bus_bits;      /**< data bus width programmed              */
} arkrtos_sdram_geo_t;

/** @brief Copy the programmed geometry out (zeros when not initialised) */
void arkrtos_sdram_geo(arkrtos_sdram_geo_t *out);

/**
* @brief Walk the whole chip: bus tests, address-uniqueness test, full sweep
* @param fail_addr  first failing address (may be NULL)
* @param expect     value expected at that address (may be NULL)
* @param got        value read back (may be NULL)
* @return 0 = pass, >0 = number of failures, negative = not initialised
* @details Address uniqueness is the part that matters: a wrong row/column
*          geometry or a timing violation does not produce random garbage, it
*          produces aliasing (two addresses hitting the same cell).  A pattern
*          written at an address-derived value catches exactly that.
*/
int32 arkrtos_sdram_selftest(uint32 *fail_addr, uint32 *expect, uint32 *got);

/**
* @brief Shell command "sdram": see arkrtos_sdram_shell_cmd() usage string
* @return 0 = ok, -1 = not configured / the request failed
* @note  Declared here and registered by the board through the public
*        external-command table, so the kernel shell does not need to know
*        this board exists (at ARKRTOS_USE_SDRAM=0 the command table is
*        byte-for-byte what it was).
*/
int arkrtos_sdram_shell_cmd(int argc, char *argv[]);

#endif /* __DRVFMC_H__ */
