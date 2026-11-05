/**
* @file drvfmc.c
* @brief STM32H743 external SDRAM bring-up (FMC SDRAM Bank1) + self test
* @details Why this file exists: the kernel RAM budget used to be "whatever is
*          left of the 512 KiB AXI SRAM after SHARE + the image pool", so the
*          compiler's work area competed with the kernel for on-chip RAM and
*          the answer was to shrink the compiler.  The board also carries a
*          32 MiB SDRAM (W9825G6KH-6) that nothing was using.  This driver is
*          the missing half of that: with it up, the big scratch buffers move
*          off the on-chip RAM entirely and the compiler's limits stop being a
*          function of how much internal RAM is left.
*
*          Wiring, from the FK743M5-XIH6 schematic (sheet "SDRAM"), FMC SDRAM
*          Bank1 => SDNE0 / SDCKE0 => memory mapped at 0xC0000000:
*            D0..D15   PD14 PD15 PD0 PD1 PE7..PE15 PD8 PD9 PD10
*            A0..A12   PF0..PF5 PF12..PF15 PG0 PG1 PG2
*            BA0/BA1   PG4 PG5
*            NBL0/1    PE0 PE1
*            SDNWE PC0  SDNRAS PF11  SDNCAS PG15  SDCLK PG8
*            SDNE0 PH3  SDCKE0 PH2
*
*          Timing is derived at runtime from the clock the FMC actually runs
*          on, not from a constant: HCLK3 is read out of RCC->D1CFGR, SDCLK is
*          HCLK3/3, and every register field is computed from the W9825G6KH
*          nanosecond minimums.  A clock change therefore cannot leave a stale
*          timing table behind; if the resulting SDCLK is outside the range the
*          part and the controller accept, init fails and says so instead of
*          running with numbers nobody checked.
*
* @author xw
* @date 2026.11.05
*/

#include "drvfmc.h"
#include "arkrtos_config.h"
#include "arkrtos_partition.h"
#include "arkrtos_log.h"
#include "ark_shell.h"              /* ark_shell_printf for the sdram command */

#ifdef ARKRTOS_BOARD_CONFIG
#include ARKRTOS_BOARD_CONFIG          /* stm32h7xx.h via the board config */
#endif

#if (ARKRTOS_USE_SDRAM == 1)

/* ---------------------------------------------------------------- constants */

#define SDRAM_AF                12u          /* FMC alternate function on H7   */
#define SDRAM_SDCLK_DIV         3u           /* SDCLK = HCLK3 / 3              */

/* Accepted SDCLK window: the SDRAM interface tops out at 100 MHz; the floor
 * exists only to catch a divider/clock typo before it turns into a silent
 * timing violation. */
#define SDRAM_SDCLK_MIN_MHZ     40u
#define SDRAM_SDCLK_MAX_MHZ     100u

/* W9825G6KH-6 minimum timings in nanoseconds (datasheet AC table). */
#define SDRAM_TMRD_NS           12u
#define SDRAM_TXSR_NS           72u
#define SDRAM_TRAS_NS           42u
#define SDRAM_TRC_NS            60u
#define SDRAM_TWR_NS            12u
#define SDRAM_TRP_NS            18u
#define SDRAM_TRCD_NS           18u
#define SDRAM_REFRESH_PERIOD_MS 64u

/* Mode register: burst length 1, sequential, CAS latency 3, standard
 * operating mode, single write burst (bit 9).  Same value ST's BSPs write. */
#define SDRAM_MRD               (0x0030u | 0x0200u)

/* SDCMR command codes (RM0433 FMC_SDCMR MODE field). */
#define SDRAM_CMD_CLK_ENABLE    0x1u
#define SDRAM_CMD_PALL          0x2u
#define SDRAM_CMD_AUTOREFRESH   0x3u
#define SDRAM_CMD_LOAD_MODE     0x4u
#define SDRAM_CMD_NORMAL        0x0u
#define SDRAM_CMD_SELF_REFRESH   0x5u

/* FMC_SDSR has no BUSY bit on the H7: the field that F4/F7 call BUSY is gone,
 * which is why the CMSIS header ships no macro for it.  The only command-engine
 * observable left is MODES1, the bank's mode: SELF_REFRESH drives it to 01b and
 * NORMAL back to 00b, so reading it across that pair proves a command reached
 * the engine and was executed. */
#define SDRAM_MODES_MASK        0x6u
#define SDRAM_MODE_SELFREFRESH  1u

/* ---------------------------------------------------------------- state */

/* Three states, and the split is not cosmetic.
 *
 * "Configured" is everything a register readback can prove: the clock is on,
 * SDCR/SDTR hold their values, the command sequence was written.  It is all
 * board init is allowed to claim, because a single bus error there happens
 * before the fault handlers and the console exist - the board then goes
 * silent, with no boot log and no PANIC to say why.
 *
 * "Verified" means one word was written through the window and read back.
 * That step is driven from the shell (or explicitly at boot), where a bus
 * error is reported instead of swallowed, and it is the only state in which
 * anything is allowed to put data in external memory. */
#define SDRAM_STATE_DOWN        (0)
#define SDRAM_STATE_CONFIGURED  (1)
#define SDRAM_STATE_VERIFIED    (2)

static volatile int32  g_sdram_state     = SDRAM_STATE_DOWN;
static volatile int32  g_sdram_err       = 0;   /* first failing step of the last init */
static volatile uint32 g_sdram_mode      = 0xFFu;/* last SDSR.MODES1 sample             */
static arkrtos_sdram_geo_t g_geo;

/* ---------------------------------------------------------------- helpers */

static void sdram_delay_us(uint32 us)
{
    /* Board init runs before SysTick exists, so this cannot use the port
     * delay (which reads SysTick).  Roughly 1 cycle per loop iteration at
     * 480 MHz with -O2; over-waiting is harmless (JEDEC sets minimums only). */
    volatile uint32 n = us * 480u;

    while(n > 0u)
    {
        n--;
    }
}

static uint32 sdram_hclk_divider(void)
{
    static const uint16 div_tab[8] = {2u, 4u, 8u, 16u, 64u, 128u, 256u, 512u};
    uint32 hpre = (RCC->D1CFGR >> RCC_D1CFGR_HPRE_Pos) & 0xFu;

    if(hpre < 8u)
    {
        return 1u;
    }
    return (uint32)div_tab[hpre - 8u];
}

/* ceil(ns * hz / 1e9) without 64-bit math: ns * kHz / 1e6 has the same value
 * and cannot overflow for any clock this part runs at. */
static uint32 sdram_ns_to_cycles(uint32 ns, uint32 hz)
{
    uint32 c = (ns * (hz / 1000u) + 999999u) / 1000000u;

    return (c == 0u) ? 1u : c;
}

static void sdram_gpio_pin(GPIO_TypeDef *port, uint32 pin)
{
    uint32 shift = (pin & 7u) * 4u;

    port->MODER    = (port->MODER & ~(3uL << (pin * 2u))) | (2uL << (pin * 2u));
    port->OTYPER  &= ~(1uL << pin);
    port->OSPEEDR |= (3uL << (pin * 2u));
    port->PUPDR   &= ~(3uL << (pin * 2u));

    if(pin < 8u)
    {
        port->AFR[0] = (port->AFR[0] & ~(0xFuL << shift)) | ((uint32)SDRAM_AF << shift);
    }
    else
    {
        port->AFR[1] = (port->AFR[1] & ~(0xFuL << shift)) | ((uint32)SDRAM_AF << shift);
    }
}

static void sdram_gpio_init(void)
{
    static const uint8 pd_pins[] = { 0u, 1u, 8u, 9u, 10u, 14u, 15u};
    static const uint8 pe_pins[] = { 0u, 1u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
    static const uint8 pf_pins[] = { 0u, 1u, 2u, 3u, 4u, 5u, 11u, 12u, 13u, 14u, 15u};
    static const uint8 pg_pins[] = { 0u, 1u, 2u, 4u, 5u, 8u, 15u};
    static const uint8 pc_pins[] = { 0u};
    static const uint8 ph_pins[] = { 2u, 3u};
    uint32 i;

    RCC->AHB4ENR |= RCC_AHB4ENR_GPIODEN | RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN |
                    RCC_AHB4ENR_GPIOGEN | RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIOHEN;
    (void)RCC->AHB4ENR;

    for(i = 0u; i < (uint32)sizeof(pd_pins); i++) { sdram_gpio_pin(GPIOD, pd_pins[i]); }
    for(i = 0u; i < (uint32)sizeof(pe_pins); i++) { sdram_gpio_pin(GPIOE, pe_pins[i]); }
    for(i = 0u; i < (uint32)sizeof(pf_pins); i++) { sdram_gpio_pin(GPIOF, pf_pins[i]); }
    for(i = 0u; i < (uint32)sizeof(pg_pins); i++) { sdram_gpio_pin(GPIOG, pg_pins[i]); }
    for(i = 0u; i < (uint32)sizeof(pc_pins); i++) { sdram_gpio_pin(GPIOC, pc_pins[i]); }
    for(i = 0u; i < (uint32)sizeof(ph_pins); i++) { sdram_gpio_pin(GPIOH, ph_pins[i]); }
}

/* The bank's stored mode (SDSR.MODES1): 0 = normal, 1 = self refresh,
 * 2 = power down.  Reading a register, so it cannot fault and it works on a
 * board whose memory window answers nothing. */
static uint32 sdram_mode_status(void)
{
    return (uint32)((FMC_Bank5_6_R->SDSR & SDRAM_MODES_MASK) >> 1);
}

/* Issue one command to the SDRAM engine.
 *
 * nrefs is the refresh count the caller means (8 = eight AUTOREFRESH pulses);
 * the H7 register field holds count-1 (HAL subtracts in the same place), so the
 * subtraction happens once, here, instead of at every call site.
 *
 * There is no BUSY bit to wait on, so each command is followed by a settle long
 * enough for the longest one: sixteen refreshes at 80 MHz is under 2 us, and
 * the JEDEC sequence only sets minimums, so over-waiting is harmless.  A lost
 * command is therefore not masked by a wait - it shows up later as a bank that
 * reports the wrong mode or a window that answers nothing. */
static int32 sdram_command(uint32 mode, uint32 nrefs, uint32 mrd)
{
    uint32 field = (nrefs > 0u) ? (nrefs - 1u) : 0u;

    FMC_Bank5_6_R->SDCMR = (mode)
                         | (field << FMC_SDCMR_NRFS_Pos)
                         | (mrd << FMC_SDCMR_MRD_Pos)
                         | FMC_SDCMR_CTB1;

    sdram_delay_us(10u);
    g_sdram_mode = sdram_mode_status();
    return 0;
}

/* ---------------------------------------------------------------- init */

/* One word at the far end of the window: the cheapest honest answer to "does
 * this memory respond at all".  The last word, not the base, so a chip that is
 * only half populated is caught as well. */
static int32 sdram_probe(void)
{
    volatile uint32 *last = (volatile uint32 *)(CHIP_SDRAM_BASE + CHIP_SDRAM_SIZE - 4u);
    uint32 seed;

    /* Read before write on purpose.  A fault on a read is precise: the fault
     * status register carries PRECISERR and BFAR names the exact address the
     * bus refused.  A store that faults first can only report IMPRECISERR with
     * BFAR cleared, which loses the witness address -- so the store below is
     * only ever reached once the read has proven the window answers. */
    seed = *last;
    (void)seed;

    *last = 0x5AA5C33Cu;
    if(*last != 0x5AA5C33Cu)
    {
        return -1;
    }
    *last = 0xA5A5A5A5u;
    return (*last == 0xA5A5A5A5u) ? 0 : -1;
}

static int32 sdram_bring_up(void)
{
    uint32 hclk, sdclk, div;
    uint32 cr, tr, refresh;

    if((CHIP_SDRAM_SIZE == 0u) || (CHIP_SDRAM_ROW_BITS == 0u) || (CHIP_SDRAM_COL_BITS == 0u))
    {
        return -1;      /* this board declares no SDRAM: nothing to bring up */
    }

    /* ---- clock: HCLK3 = SYSCLK / HPRE, SDCLK = HCLK3 / 3 ---------- */
    hclk  = SystemCoreClock / sdram_hclk_divider();
    div   = SDRAM_SDCLK_DIV;
    sdclk = hclk / div;
    if((sdclk / 1000000u < SDRAM_SDCLK_MIN_MHZ) || (sdclk / 1000000u > SDRAM_SDCLK_MAX_MHZ))
    {
        return -2;      /* SDCLK out of range: refuse rather than guess */
    }

    /* ---- GPIO + FMC clock ---------------------------------------- */
    sdram_gpio_init();
    RCC->AHB3ENR |= RCC_AHB3ENR_FMCEN;
    (void)RCC->AHB3ENR;

    /* ---- SDCR: geometry, CAS latency, clock divider, read pipeline -- */
    /* NC  : 9 column bits  = 01b   (00=8, 01=9, 10=10, 11=11)
     * NR  : 13 row bits    = 10b   (00=11, 01=12, 10=13, 11=14)
     * MWID: 16-bit bus     = 01b
     * NB  : 4 banks        = 1
     * CAS : 3 clocks       = 11b
     * SDCLK: HCLK/3        = 11b on H7 (10b = /2)
     * RBURST: single reads (no data cache on this port, so a burst buys nothing)
     * RPIPE: read-pipeline delay in HCLK cycles.  With SDCLK = HCLK/3 the
     *        captured data is 2 HCLK cycles behind the command, so the delay
     *        is 2 - 1 = 1 (FMC_SDRAM_RPIPE_DELAY_1 in stm32h7xx_ll_fmc.h),
     *        not 2: an extra cycle shifts every read word by one position. */
    cr = FMC_SDCRx_NC_0
       | FMC_SDCRx_NR_1
       | FMC_SDCRx_MWID_0
       | FMC_SDCRx_NB
       | FMC_SDCRx_CAS_0 | FMC_SDCRx_CAS_1
       | FMC_SDCRx_SDCLK_0 | FMC_SDCRx_SDCLK_1
       | (((div >= 2u) ? (div - 2u) : 0u) << FMC_SDCRx_RPIPE_Pos);

    /* ---- SDTR: every field is (cycles - 1) ----------------------- */
    {
        uint32 tmrcd = sdram_ns_to_cycles(SDRAM_TMRD_NS, sdclk);
        uint32 txsr  = sdram_ns_to_cycles(SDRAM_TXSR_NS, sdclk);
        uint32 tras  = sdram_ns_to_cycles(SDRAM_TRAS_NS, sdclk);
        uint32 trc   = sdram_ns_to_cycles(SDRAM_TRC_NS,  sdclk);
        uint32 twr   = sdram_ns_to_cycles(SDRAM_TWR_NS,  sdclk);
        uint32 trp   = sdram_ns_to_cycles(SDRAM_TRP_NS,  sdclk);
        uint32 trcd  = sdram_ns_to_cycles(SDRAM_TRCD_NS, sdclk);

        if(tmrcd < 2u) { tmrcd = 2u; }      /* RM0433: TMRD minimum is 2 */

        tr = ((tmrcd - 1u) << FMC_SDTRx_TMRD_Pos)
           | ((txsr  - 1u) << FMC_SDTRx_TXSR_Pos)
           | ((tras  - 1u) << FMC_SDTRx_TRAS_Pos)
           | ((trc   - 1u) << FMC_SDTRx_TRC_Pos)
           | ((twr   - 1u) << FMC_SDTRx_TWR_Pos)
           | ((trp   - 1u) << FMC_SDTRx_TRP_Pos)
           | ((trcd  - 1u) << FMC_SDTRx_TRCD_Pos);
    }

    FMC_Bank5_6_R->SDCR[0] = cr;
    FMC_Bank5_6_R->SDTR[0] = tr;

    /* ---- JEDEC power-up sequence --------------------------------- */
    if(sdram_command(SDRAM_CMD_CLK_ENABLE, 0u, 0u) != 0) { return -3; }
    sdram_delay_us(200u);                       /* >= 100 us after clock on */

    if(sdram_command(SDRAM_CMD_PALL, 0u, 0u) != 0)        { return -4; }
    if(sdram_command(SDRAM_CMD_AUTOREFRESH, 8u, 0u) != 0) { return -5; }
    if(sdram_command(SDRAM_CMD_LOAD_MODE, 0u, SDRAM_MRD) != 0) { return -6; }
    sdram_delay_us(200u);                       /* tMRD + margin */

    /* ---- refresh: COUNT = (tREFI / tCK) - 20, tREFI = 64ms / rows -- */
    {
        uint32 trefi_ns = (SDRAM_REFRESH_PERIOD_MS * 1000000u) / (1u << CHIP_SDRAM_ROW_BITS);

        refresh = (trefi_ns * (sdclk / 1000u)) / 1000000u;
        refresh = (refresh > 20u) ? (refresh - 20u) : 1u;
    }
    FMC_Bank5_6_R->SDRTR = (refresh << FMC_SDRTR_COUNT_Pos);

    if(sdram_command(SDRAM_CMD_NORMAL, 0u, 0u) != 0) { return -7; }

    /* A controller that has not reached Normal mode refuses every access to
     * the window with a bus error; a controller that has, answers.  The mode
     * readback below is therefore part of correctness, not decoration: a
     * configured controller is not the same thing as a chip that responds.
     * The response itself is checked by the opt-in probe further down. */
    if((FMC_Bank5_6_R->SDSR & FMC_SDSR_MODES1_Msk) != 0u)
    {
        return -8;      /* stuck outside Normal mode (self refresh / power down) */
    }
    /* Deliberately no memory access here: see SDRAM_STATE_CONFIGURED. */

    g_geo.base      = CHIP_SDRAM_BASE;
    g_geo.size      = CHIP_SDRAM_SIZE;
    g_geo.hclk_mhz  = hclk / 1000000u;
    g_geo.sdclk_mhz = sdclk / 1000000u;
    g_geo.col_bits  = CHIP_SDRAM_COL_BITS;
    g_geo.row_bits  = CHIP_SDRAM_ROW_BITS;
    g_geo.bus_bits  = CHIP_SDRAM_BUS_BITS;

    return 0;
}

int32 arkrtos_sdram_init(void)
{
    int32 rc;

    if(g_sdram_state >= SDRAM_STATE_CONFIGURED)
    {
        return 0;
    }

    rc = sdram_bring_up();
    g_sdram_err = (rc == 0) ? 0 : rc;
    if(rc != 0)
    {
        /* Kept for "sdram info": board init runs before the console exists,
         * so this cannot rely on a log line to reach the operator. */
        return rc;
    }

    g_sdram_state = SDRAM_STATE_CONFIGURED;

    ARKRTOS_LOGI("SDRAM", "configured: %u MB @0x%08X, HCLK3 %u MHz, SDCLK %u MHz (unverified)",
                 (unsigned)(CHIP_SDRAM_SIZE / (1024u * 1024u)),
                 (unsigned)CHIP_SDRAM_BASE, (unsigned)g_geo.hclk_mhz,
                 (unsigned)g_geo.sdclk_mhz);
    return 0;
}

int32 arkrtos_sdram_verify(void)
{
    if(g_sdram_state < SDRAM_STATE_CONFIGURED)
    {
        return -1;      /* nothing configured, so there is nothing to check */
    }
    if(g_sdram_state == SDRAM_STATE_VERIFIED)
    {
        return 0;
    }
    if(sdram_probe() != 0)
    {
        return -2;      /* the window answered, the data did not come back */
    }
    g_sdram_state = SDRAM_STATE_VERIFIED;
    return 0;
}

int32 arkrtos_sdram_last_error(void)
{
    return g_sdram_err;
}

int32 arkrtos_sdram_ready(void)
{
    return (g_sdram_state == SDRAM_STATE_VERIFIED) ? 1 : 0;
}

int32 arkrtos_sdram_configured(void)
{
    return (g_sdram_state >= SDRAM_STATE_CONFIGURED) ? 1 : 0;
}

void arkrtos_sdram_geo(arkrtos_sdram_geo_t *out)
{
    uint32 i;

    if(out == NULL)
    {
        return;
    }
    if(g_sdram_state < SDRAM_STATE_CONFIGURED)
    {
        for(i = 0u; i < (uint32)(sizeof(*out) / sizeof(uint32)); i++)
        {
            ((uint32 *)out)[i] = 0u;
        }
        return;
    }
    *out = g_geo;
}

/* ---------------------------------------------------------------- self test */

static void sdram_record(int32 *p_count, uint32 *bad, uint32 *exp, uint32 *got,
                         uint32 addr, uint32 expect, uint32 read)
{
    if(*p_count == 0)
    {
        if(bad != NULL) { *bad = addr; }
        if(exp != NULL) { *exp = expect; }
        if(got != NULL) { *got = read; }
    }
    (*p_count)++;
}

int32 arkrtos_sdram_selftest(uint32 *fail_addr, uint32 *expect, uint32 *got)
{
    static const uint32 bus_patterns[4] = {0x00000000u, 0xFFFFFFFFu,
                                           0xAAAAAAAAu, 0x55555555u};
    volatile uint32 *base = (volatile uint32 *)(CHIP_SDRAM_BASE);
    uint32 i, v;
    int32 fails = 0;

    if(fail_addr != NULL) { *fail_addr = 0u; }
    if(expect    != NULL) { *expect    = 0u; }
    if(got       != NULL) { *got       = 0u; }

    if(g_sdram_state < SDRAM_STATE_CONFIGURED)
    {
        return -1;
    }

    /* 1) data bus: constant patterns + walking ones ----------------- */
    for(i = 0u; i < 4u; i++)
    {
        base[0] = bus_patterns[i];
        v = base[0];
        if(v != bus_patterns[i])
        {
            sdram_record(&fails, fail_addr, expect, got, CHIP_SDRAM_BASE, bus_patterns[i], v);
        }
    }
    for(i = 0u; i < 32u; i++)
    {
        uint32 pattern = 1uL << i;

        base[0] = pattern;
        v = base[0];
        if(v != pattern)
        {
            sdram_record(&fails, fail_addr, expect, got, CHIP_SDRAM_BASE, pattern, v);
        }
    }
    if(fails != 0)
    {
        return fails;               /* the bus itself is wrong: stop here */
    }

    /* 2) address uniqueness: one word per 4 KiB page, address-derived.
     *    Aliasing (wrong row/column geometry, timing violation) shows up as
     *    two addresses reading back each other's value. */
    {
        uint32 pages = CHIP_SDRAM_SIZE / 4096u;

        for(i = 0u; i < pages; i++)
        {
            base[(i * 4096u) / 4u] = 0xA5A50000u | i;
        }
        for(i = 0u; i < pages; i++)
        {
            uint32 e = 0xA5A50000u | i;

            v = base[(i * 4096u) / 4u];
            if(v != e)
            {
                sdram_record(&fails, fail_addr, expect, got,
                             CHIP_SDRAM_BASE + i * 4096u, e, v);
                if(fails > 16)
                {
                    return fails;   /* enough to diagnose, do not walk 32 MiB */
                }
            }
        }
    }

    /* 3) full sweep: every 32-bit word, value derived from its address.
     *    This is what proves the whole 32 MiB really is 32 MiB. */
    {
        uint32 words = CHIP_SDRAM_SIZE / 4u;

        for(i = 0u; i < words; i++)
        {
            base[i] = (i * 2654435761u) ^ 0x5A5A5A5Au;
        }
        for(i = 0u; i < words; i++)
        {
            uint32 e = (i * 2654435761u) ^ 0x5A5A5A5Au;

            v = base[i];
            if(v != e)
            {
                sdram_record(&fails, fail_addr, expect, got,
                             CHIP_SDRAM_BASE + i * 4u, e, v);
                if(fails > 32)
                {
                    return fails;
                }
            }
        }
    }

    return fails;
}

/* ---------------------------------------------------------------- shell */

/* Command-engine probe: self refresh in, normal out, mode read in between.
 *
 * This replaces an earlier tight-loop sample of SDSR.BUSY.  That instrument
 * could never succeed on this part - the H7 has no BUSY bit at all - so its
 * "the commands are being dropped" verdict was the instrument's own blind
 * spot, not the controller's.  MODES1 is the observable that exists: it is
 * the mode the bank is actually in, written by the command engine itself.
 *
 *   rc  1 : mode went 0 -> 1 -> 0, the engine executes commands
 *   rc  0 : mode never moved, nothing reached the engine
 *   rc -1 : it moved to self refresh and did not come back on NORMAL
 *           (the engine runs, the exit command was lost) */
static int32 sdram_fsm_probe(void)
{
    uint32 before, in_refresh, after;

    before = sdram_mode_status();
    (void)sdram_command(SDRAM_CMD_SELF_REFRESH, 0u, 0u);
    sdram_delay_us(500u);
    in_refresh = sdram_mode_status();
    (void)sdram_command(SDRAM_CMD_NORMAL, 0u, 0u);
    sdram_delay_us(500u);
    after = sdram_mode_status();

    ark_shell_printf("sdram: MODES1 %u -> %u -> %u (0 normal, 1 self refresh)\r\n",
                     (unsigned)before, (unsigned)in_refresh, (unsigned)after);
    if((before != 0u) || (after != 0u))
    {
        ark_shell_printf("       note: the bank did not start/end in normal mode\r\n");
    }
    if(in_refresh == SDRAM_MODE_SELFREFRESH)
    {
        if(after == 0u)
        {
            ark_shell_printf("       engine executes commands (mode followed the pair)\r\n");
            return 1;
        }
        return -1;
    }
    ark_shell_printf("       engine never changed mode; commands are not reaching it\r\n");
    return 0;
}

/* Pin-level probe.
 *
 * GPIOx->IDR keeps reporting the pin level in alternate-function mode, so the
 * FMC's output lines can be read back without touching MODER.  Two different
 * questions get answered here, and they are usually confused with each other:
 *   - "is the controller driving its outputs at all?" -> CKE0/NE0/RAS/CAS/WE
 *     are all high when a bank is enabled and idle, so all-zero means the
 *     pins are muxed but nothing is driving them;
 *   - "is SDCLK actually toggling?" -> sampled repeatedly; at 80 MHz a core
 *     loop at 480 MHz sees the level change on nearly every sample.
 * Also prints MODER so "not muxed" cannot be mistaken for "idle". */
static void sdram_pin_report(void)
{
    uint32 i, toggles = 0u, last, cur;

    last = (GPIOG->IDR >> 8) & 1u;
    for(i = 0u; i < 512u; i++)
    {
        cur = (GPIOG->IDR >> 8) & 1u;
        if(cur != last)
        {
            toggles++;
        }
        last = cur;
    }

    ark_shell_printf("\r\npins (level / MODER[2=AFF, 0=input]):\r\n");
    ark_shell_printf("  SDCKE0 PH2 = %u / %u   SDNE0 PH3 = %u / %u\r\n",
                     (unsigned)((GPIOH->IDR >> 2) & 1u), (unsigned)((GPIOH->MODER >> 4) & 3u),
                     (unsigned)((GPIOH->IDR >> 3) & 1u), (unsigned)((GPIOH->MODER >> 6) & 3u));
    ark_shell_printf("  SDNRAS PF11= %u / %u   SDNCAS PG15= %u / %u   SDNWE PC0 = %u / %u\r\n",
                     (unsigned)((GPIOF->IDR >> 11) & 1u), (unsigned)((GPIOF->MODER >> 22) & 3u),
                     (unsigned)((GPIOG->IDR >> 15) & 1u), (unsigned)((GPIOG->MODER >> 30) & 3u),
                     (unsigned)((GPIOC->IDR >> 0) & 1u), (unsigned)((GPIOC->MODER >> 0) & 3u));
    ark_shell_printf("  SDCLK PG8: level changed %u time(s) in 512 samples -> %s\r\n",
                     (unsigned)toggles, (toggles > 0u) ? "edges seen" : "NO edges (clock dead)");
}

int arkrtos_sdram_shell_cmd(int argc, char *argv[])
{
    arkrtos_sdram_geo_t geo;
    const char *sub = (argc > 1) ? argv[1] : "info";
    int is_init = ((sub[0] == 'i') || (sub[0] == 'I')) && ((sub[1] == 'n') || (sub[1] == 'N'));

    arkrtos_sdram_geo(&geo);

    if(((sub[0] == 'i') || (sub[0] == 'I')) && (is_init == 0))
    {
        if(arkrtos_sdram_configured() != 1)
        {
            ark_shell_printf("sdram: not configured (init stopped at step %d)\r\n",
                             (int)arkrtos_sdram_last_error());
            return -1;
        }
        if(arkrtos_sdram_ready() != 1)
        {
            ark_shell_printf("sdram: configured, NOT verified - no data may be placed there\r\n");
        }
        ark_shell_printf("\r\nsdram base  : 0x%08X\r\n", (unsigned)geo.base);
        ark_shell_printf("size        : %u MB\r\n", (unsigned)(geo.size / (1024u * 1024u)));
        ark_shell_printf("geometry    : %u-bit bus, %u row bits, %u col bits, 4 banks\r\n",
                         (unsigned)geo.bus_bits, (unsigned)geo.row_bits,
                         (unsigned)geo.col_bits);
        ark_shell_printf("clock       : HCLK3 %u MHz / 3 = SDCLK %u MHz\r\n",
                         (unsigned)geo.hclk_mhz, (unsigned)geo.sdclk_mhz);
        return 0;
    }

    if(is_init != 0)
    {
        int32 rc;

        /* Re-run the whole bring-up.  Useful after changing the kernel clock
         * source, and it cannot fault: it only touches registers. */
        g_sdram_state = SDRAM_STATE_DOWN;
        g_sdram_mode = 0xFFu;
        rc = arkrtos_sdram_init();
        ark_shell_printf("sdram: init rc=%d, bank mode after sequence=%u (0 normal)\r\n",
                         (int)rc, (unsigned)g_sdram_mode);
        return (rc == 0) ? 0 : -1;
    }

    if((sub[0] == 'v') || (sub[0] == 'V'))
    {
        int32 rc = arkrtos_sdram_verify();

        if(rc == 0)
        {
            ark_shell_printf("sdram: verified - a word went out and came back\r\n");
            return 0;
        }
        ark_shell_printf("sdram: verify failed rc=%d (no bus error reached the shell,\r\n"
                         "       so the window answered something wrong)\r\n", (int)rc);
        return -1;
    }

    if((sub[0] == 'f') || (sub[0] == 'F'))
    {
        int32 rc = sdram_fsm_probe();

        return (rc == 1) ? 0 : -1;
    }

    if((sub[0] == 'c') || (sub[0] == 'C'))
    {
        uint32 src = (argc > 2) ? (uint32)(argv[2][0] - '0') : 0u;

        if(src > 3u)
        {
            ark_shell_printf("usage: sdram clk <0=HCLK 1=PLL1Q 2=PLL2R 3=PER_CK>\r\n");
            return -1;
        }
        /* Warning worth printing: selecting a clock source that is not
         * running wedges the FMC's AHB side as well - the board then has to
         * be reset, so this is a deliberate, operator-driven experiment. */
        ark_shell_printf("sdram: FMCSEL <- %u (a stopped source hangs the FMC; reset to undo)\r\n",
                         (unsigned)src);
        RCC->D1CCIPR = (RCC->D1CCIPR & ~3u) | src;
        return 0;
    }

    if(((sub[0] == 'p') || (sub[0] == 'P')) && ((sub[1] == 'i') || (sub[1] == 'I')))
    {
        /* Pure read of GPIO/IDR: cannot fault, works even when the memory
         * window answers nothing. */
        sdram_pin_report();
        return 0;
    }

    if((sub[0] == 'p') || (sub[0] == 'P'))
    {
        int32 rc;

        if(arkrtos_sdram_configured() != 1)
        {
            ark_shell_printf("sdram: not configured\r\n");
            return -1;
        }
        ark_shell_printf("sdram: probing 0x%08X (a bus error here ends in PANIC BUS)\r\n",
                         (unsigned)(CHIP_SDRAM_BASE + CHIP_SDRAM_SIZE - 4u));
        rc = sdram_probe();
        ark_shell_printf("sdram: probe %s\r\n", (rc == 0) ? "PASS" : "FAIL");
        return rc;
    }

    if((sub[0] == 't') || (sub[0] == 'T'))
    {
        uint32 bad = 0u, exp = 0u, got = 0u;
        int32 rc;

        if(arkrtos_sdram_configured() != 1)
        {
            ark_shell_printf("sdram: not configured\r\n");
            return -1;
        }

        ark_shell_printf("sdram: testing %u MB (bus, address uniqueness, full sweep)...\r\n",
                         (unsigned)(CHIP_SDRAM_SIZE / (1024u * 1024u)));
        rc = arkrtos_sdram_selftest(&bad, &exp, &got);
        if(rc == 0)
        {
            ark_shell_printf("sdram: PASS - every word of %u MB read back as written\r\n",
                             (unsigned)(CHIP_SDRAM_SIZE / (1024u * 1024u)));
            return 0;
        }
        ark_shell_printf("sdram: FAIL - %d error(s), first at 0x%08X: wrote 0x%08X read 0x%08X\r\n",
                         (int)rc, (unsigned)bad, (unsigned)exp, (unsigned)got);
        return -1;
    }

    if((sub[0] == 'r') || (sub[0] == 'R'))
    {
        /* Raw snapshot.  When init does not complete, the interesting facts
         * are here rather than in any return code: whether the controller kept
         * what was written, whether it reports a mode at all, and whether the
         * FMC even has a clock.  Reading registers cannot fault, so this works
         * on a board whose memory window does not answer. */
        ark_shell_printf("\r\nstate  = %d (0 down, 1 configured, 2 verified)  mode=%u\r\n",
                         (int)g_sdram_state, (unsigned)g_sdram_mode);
        ark_shell_printf("SDCR1  = 0x%08X  SDTR1 = 0x%08X\r\n",
                         (unsigned)FMC_Bank5_6_R->SDCR[0],
                         (unsigned)FMC_Bank5_6_R->SDTR[0]);
        ark_shell_printf("SDRTR  = 0x%08X  SDSR  = 0x%08X (BUSY=%u MODES1=%u)\r\n",
                         (unsigned)FMC_Bank5_6_R->SDRTR,
                         (unsigned)FMC_Bank5_6_R->SDSR,
                         (unsigned)((FMC_Bank5_6_R->SDSR >> 5) & 1u),
                         (unsigned)((FMC_Bank5_6_R->SDSR >> 1) & 3u));
        ark_shell_printf("AHB3ENR= 0x%08X  D1CFGR=0x%08X  D1CCIPR=0x%08X\r\n",
                         (unsigned)RCC->AHB3ENR, (unsigned)RCC->D1CFGR,
                         (unsigned)RCC->D1CCIPR);
        /* The PLL readouts are here because the only remaining alternative
         * clock for the FMC is a PLL output: without these three lines
         * "sdram clk 1" would be a guess about which source is even running. */
        ark_shell_printf("PLLCFGR=0x%08X  PLL1DIVR=0x%08X  PLL2DIVR=0x%08X\r\n",
                         (unsigned)RCC->PLLCFGR, (unsigned)RCC->PLL1DIVR,
                         (unsigned)RCC->PLL2DIVR);
        ark_shell_printf("PLL ON: 1=%u 2=%u 3=%u  (CR=0x%08X)\r\n",
                         (unsigned)((RCC->CR >> 24) & 1u),
                         (unsigned)((RCC->CR >> 26) & 1u),
                         (unsigned)((RCC->CR >> 28) & 1u),
                         (unsigned)RCC->CR);
        ark_shell_printf("step   = %d (0 = ok)\r\n", (int)arkrtos_sdram_last_error());
        return 0;
    }

    ark_shell_printf("usage: sdram [info | init | verify | probe | test | fsm | pins | clk <n> | regs]\r\n");
    return -1;
}

#endif /* ARKRTOS_USE_SDRAM */
