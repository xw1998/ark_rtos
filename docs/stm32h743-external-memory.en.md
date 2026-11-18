> **2026-11-07 correction (the "hardware-level negative reading" conclusion in the lower half of this page is void)**
>
> What was measured at the time as "the FMC window answers no access at all, while registers / command engine / pins are all fine" was really caused by **the FMC master gate never being enabled**: `FMC_BCR1.FMCEN` (bit 31). Its reset value is not documented, and while FMC is off: writes to `SDCR/SDTR` still land, `SDSR` reports no error, but the command generator does not run, the refresh counter does not count, and window accesses return as AXI errors (showing up as `IMPRECISERR`, with the address even lost). That is exactly what "registers all correct, window does not answer" looks like.
>
> After adding it (`board/stm32h743/drvfmc.c`, plus explicitly writing `D1CCIPR.FMCSEL = D1HCLK`), on-board measurements:
>
> ```
> sdram info   -> 32 MB @0xC0000000, HCLK3 240 MHz, SDCLK 80 MHz
> sdram verify -> verified - a word went out and came back
> sdram test   -> testing 32 MB (bus, address uniqueness, full sweep)...
>                 PASS - every word of 32 MB read back as written
> ```
>
> With that same change `USE_SDRAM=1` became usable for the first time: `.ark_buf` landed in the 12 MB SDRAM partition and ramfs in the 6 MB partition, on-chip RAM usage dropped from **98.37% to 62.14%**, and the compiler pool went from 80 KB to 4 MB. The MPU's EXTRAM region (`ARKRTOS_MPU_RGN_EXTRAM`, 0xC0000000, Normal non-cacheable) was already wired; only this one enable bit was missing.
>
> In sections 4/6/7 below, the sentences "hardware-level negative reading", "software cannot work around it" and "until the board or chip is retested the correct configuration for this board is USE_SDRAM=0" **are kept verbatim as that period's evidence chain**, but the conclusion follows this note. Demo path and measurements: `docs/arkc板上直跑-M4-hello演示.md`.

# STM32H743 On-Board External Memory (SDRAM / QSPI NOR): wired up, but this board's window did not answer

> Condensed English edition. The Chinese page (stm32h743-external-memory.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

**This document answers one question: the board has 32 MB of SDRAM and 8 MB of NOR Flash soldered on, so why was there "not enough space"? And now that the controller is configured, why was it still unusable?**

The answer has two halves, both backed by on-board readings:

1. The original "not enough space" **had nothing to do with the external devices**; it came entirely from on-chip budget and a few software constants. This part is solved (the mechanism for moving large working areas out of on-chip RAM has landed).
2. But after moving them, it turned out that **this board's FMC memory window answered no access at all**, while its registers, command engine and pin drivers were all fine. That was a hardware-level negative reading which software could not change, and this page pinned it down with evidence. **(2026-11-07: root cause found — the FMC master gate `FMC_BCR1.FMCEN` was never enabled; see the note at the top. This half of the page is kept as the evidence chain of that period.)**

## 1. Board facts (verified against the schematic, not guessed)

Schematic: `broad_example/example/FK743M5-XIH6原理图.pdf`. Text layer extracted with `pdftotext -raw` to read net labels (image OCR misaligns whole lines, see §6 item 4).

| Device | Part | Capacity | Attached to |
|---|---|---|---|
| SDRAM | W9825G6KH-6 (IC3) | 32 MB = 4 bank × 8M × 16bit | FMC SDRAM Bank1 (SDNE0/SDCKE0) ⇒ `0xC0000000` |
| QSPI NOR | W25Q64 | 8 MB | QUADSPI ⇒ `0x90000000` |

MCU: STM32H743XIH6, 2 MB Flash, 512 KB AXI SRAM (`0x24000000`), 128 KB DTCM (`0x20000000`).

SDRAM pins (all AF12): D0 PD14, D1 PD15, D2 PD0, D3 PD1, D4 PE7 … D12 PE15, D13 PD8, D14 PD9, D15 PD10; A0 PF0 … A5 PF5, A6 PF12, A7 PF13, A8 PF14, A9 PF15, A10 PG0, A11 PG1, A12 PG2; BA0 PG4, BA1 PG5; NBL0 PE0, NBL1 PE1; SDNWE PC0, SDNRAS PF11, SDNCAS PG15, SDCLK PG8, **SDNE0 PH3, SDCKE0 PH2**.

These facts are written once in `config/stm32h743/arkrtos_partition.h` (`CHIP_SDRAM_*`), and both `tools/gen_ld.py` and the driver derive from there — an address must not appear in a second place.

## 2. The original "not enough space" was four things, none of them an external device

| Symptom | Real source |
|---|---|
| `RAM 439284 B (97.50%)` | on-chip AXI SRAM holding arkc's `.ark_buf` (178 KB), ramfs's fixed file buffers, kernel `.bss` |
| `KERNEL_SIZE 512K` | arkc text is about 139 KB, so the kernel region has to jump to the next power of two, 512 KB |
| `ARK_RAMFS_FILE_CAP / MAX_FILES` capping large files | both are **software constants** (one fixed buffer per file) |
| `.bss` also holds `g_ld_syms/g_ld_relocs[512]` at 0x11000 each | linker symbol table and relocation table, hard-coded in `.bss` |

So "the board has SDRAM yet reports out of space" holds: at that time there was no SDRAM driver at all, so the compiler could only squeeze into on-chip RAM.

## 3. What has landed (default off = zero behavior and size change)

| Switch | Location | Default | Effect |
|---|---|---|---|
| `ARKRTOS_USE_SDRAM` | `kernelsrc/include/arkrtos_features.h` | 0 | compile in the FMC/SDRAM driver, place `.ark_buf` and ramfs file buffers in the external partitions |
| `ARKRTOS_SHELL_BOARD_CMDS` | same | 0 | compile in board-level shell commands (`sdram`); decided by `USE_SDRAM` |
| `USE_SDRAM` (make variable) | `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | **0** | decides four things at once: the `-D` above, the compiler's buffer budget, ramfs slot count and section ownership, `gen_ld.py --ext-ram` |
| `ARKRTOS_SDRAM_VERIFY_AT_BOOT` | `example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/main.c` | 0 | whether to really touch the window at boot (touching it can trigger a bus fault, so production profiles must keep it off) |

With `make USE_SDRAM=1`: `.ark_buf` → `SDRAMBUF` (12 MB from `0xC0000000`), ramfs file buffers (`.extram`) → `SDRAMFS` (6 MB from `0xC0C00000`), compile budget raised to pool 4 MB / source 512 KB / assembly 2 MB / code 1 MB, ramfs 8 slots × 512 KB.

With `make` (default profile, `USE_SDRAM=0`): `gen_ld.py --ext-ram 0` zeroes both partitions, `.ark_buf` and `.extram` return to on-chip RAM, **the linker script is byte-identical in structure to a board without external RAM**, and RAM usage returns to 439284 B / 97.50%.

> **Why the new `--ext-ram` parameter**: the partition header describes the physical fact "this board has these two devices", which exists unconditionally; but "does this build use them" is a board-level decision. The first version generated `SDRAMBUF/SDRAMFS` straight from the partition header, so with `USE_SDRAM=0` `.ark_buf` was still placed at `0xC0000000` — on-chip RAM freed 232 KB and the compiler took a bus fault as soon as a pool was opened. This trap surfaced only when the default profile was switched back to 0 this round; recorded in §6.

### 3.1 Three-state semantics (the `sdram` state machine)

```
DOWN ──init──> CONFIGURED ──verify──> VERIFIED
```

* `CONFIGURED`: registers are written, but **not one byte has been proven accessible**.
* `VERIFIED`: a word went out and came back; only then may data be placed there.
* board init performs register-only actions (never touches the window); every "touch the window" action lives in the shell; failure is exposed as `PANIC BUS` plus the address, never a silently returned error value.
* VFS gating: when `arkrtos_sdram_ready() != 1` mounting is skipped, because ramfs file buffers live in that memory — better no VFS than writing files into memory that does not exist.

### 3.2 The `sdram` command

```
sdram info        geometry / capacity / clock / verified or not
sdram init        rerun the whole power-up sequence (registers only, cannot fault)
sdram verify      write a word and read it back (touches the window; failure means a bus fault)
sdram probe       touch only the last word of the window
sdram test        full-chip pattern self-test (32 MB, full sweep)
sdram fsm         command-engine instrument: SELF_REFRESH → NORMAL, watch whether MODES1 follows
sdram pins        pin levels / MODER / whether SDCLK is toggling
sdram clk <0..3>  FMCSEL: 0=HCLK3 1=PLL1Q 2=PLL2R 3=PER_CK
sdram regs        raw SDCR1/SDTR1/SDRCMR/SDRTR/SDSR values
```

## 4. Readings of that round: the window did not answer (negative reading)

### 4.1 Half of it was configured, and it was the self-provable half

Debugger (CPU halted at a breakpoint, reading registers not the window):

```
SDCR1 = 0x00002DD9   NC=9 columns, NR=13 rows, MWID=16bit, NB=4bank, CAS=3, SDCLK=/3, RPIPE=1
SDTR1 = 0x01104351   TMRD/TXSR/TRAS/TRC/TWR/TRP/TRCD computed from W9825G6KH ns values at the actual SDCLK
SDRTR = 0x000004B8   refresh count from 64 ms / 8192 rows
SDSR  = 0
AHB3ENR = 0x1000     FMCEN set
D1CFGR  = 0x48 / D1CCIPR = 0   HCLK3 = 240 MHz ⇒ SDCLK = 80 MHz
```

On board `sdram info` prints the same: `configured: 32 MB @0xC0000000, HCLK3 240 MHz, SDCLK 80 MHz`.

Three independent pieces of evidence that this half of the FMC is alive:

1. **Registers are writable and read back**: SDCR1 written and read back matches.
2. **The command engine runs**: `sdram fsm` prints `MODES1 0 -> 1 -> 0`, i.e. both SELF_REFRESH and NORMAL commands executed and `SDSR.MODES1` followed.
3. **Pins are being driven**: `sdram pins` measured SDCKE0 PH2, SDNE0 PH3, SDNRAS PF11, SDNCAS PG15, SDNWE PC0 all in AF with level 1; SDCLK PG8 changed over 512 samples.

### 4.2 Yet not one byte of the window answered

CPU side (`sdram verify`, after the probe was changed to "read first"):

```
*** PANIC BUS ***
CFSR   = 0x00008200   BFARVALID | PRECISERR
BFAR   = 0xC1FFFFFC   exactly the last word of the 32 MB window
MPUCTRL= 0x00000005   (ENABLE|PRIVDEFENA; MMARVALID=0, DACCVIOL=0 ⇒ not an MPU violation)
```

`BFARVALID|PRECISERR` means "the bus gave an error response, and the address is the precise one". This reading can only be produced one way: the CPU issued an AXI read and it was rejected.

Debugger side (AHB-AP, bypassing the CPU): `0x60000000`, `0xC0000000`, `0xC1FFFFFC` in 8/16/32-bit reads and 32-bit writes **all** returned `Failed to read memory`.

### 4.3 Ruled out (each with an experiment)

| Hypothesis | Experiment | Result |
|---|---|---|
| SDRAM timing/geometry wrong | check SDCR1/SDTR1 field by field against HAL's `FMC_SDRAM_Init` encoding | ruled out (and wrong timing cannot produce a bus error, only garbage data) |
| SDCLK frequency wrong | `sdram clk 0/1/2/3` switching FMCSEL, all four clock sources tried | ruled out |
| MPU/cache attributes | MPU_CTRL, CFSR's MMARVALID/DACCVIOL bits; the debugger path bypasses the MPU | ruled out |
| our clock tree / firmware | **bare-metal test in reset state**: no firmware, HSI 64 MHz, only set FMCEN then configure bank1 | still errors ⇒ ruled out |
| bank not enabled (MBKEN/SDNE) | H7's SDCR **has no SDNE bit**; set MBKEN in BCR1 (async SRAM port) then read `0x60000000` | still errors ⇒ ruled out |
| data width / bank selection | 8/16/32-bit read × write × bank1/bank2 matrix | all fail |
| needs an external device to answer | configure bank1 as **pure asynchronous 16-bit SRAM** (MBKEN=1, MTYP=00, MWID=01, FACCEN=1, BTR1=0xFFFFFFFF) — such a bank needs no external device to be read | still errors ⇒ completely unrelated to the SDRAM device, geometry or timing |
| is it "FMC rejected the transaction" or "nobody answered" | compare against unimplemented peripheral addresses `0x52005000`/`0x5200A000` (return 0, no error) and `0x60000000` (errors) | the error shape matches "no slave on AXI at all" (DECERR), not FMC's SLVERR |

Two masters (CM7's AXI port, the debugger's MEM-AP), two states (reset state, firmware running), four widths, two banks, two bank types — all the same result.

### 4.4 Therefore the only remaining class of explanation

Registers and command engine work on the same silicon in the same clock domain, yet its **external memory window** answers no access at all. That is not a layer software can change:

* a device/package-level defect (the FMC's AXI slave port not responding), or
* a non-genuine die (IDCODE can be counterfeited), or
* this board's MCU has that one function broken.

Chip identity readings (debugger, reset state):

```
DBGMCU_IDCODE = 0x20036450   DEV_ID=0x450 (STM32H74x/75x), REV_ID=0x2003 (rev V)
FLASHSIZE     = 0x00000800   2048 KB
UID           = 0x0031003C 0x30335105 0x34393535
```

`DEV_ID/REV_ID/FLASHSIZE` agree with a genuine H743XI rev V, and the clock really runs at the 480 MHz system clock only rev V has — **so this does not prove a counterfeit, it only says the readings are not contradictory**.

> **What this section does not conclude**: it does not say "SDRAM being unusable is a software problem", nor "it is fixed". The only responsible statement available at that time was: **on the software side everything provable had been proven; the window not answering is a hardware-level negative reading.** (Superseded 2026-11-07 by the note at the top: the cause was `FMC_BCR1.FMCEN`.)

## 5. To keep looking, which points to measure (for whoever picks this up)

Cheapest first:

1. Swap in another board of the same model and run the same `make USE_SDRAM=1` firmware with `sdram verify`. Pass ⇒ this board's MCU is broken; fail ⇒ this batch of parts or this project.
2. Multimeter the VDD/VDDQ of IC3 and whether `SDNE0`(PH3)/`SDCKE0`(PH2) reach the chip pins. (Note: **an SDRAM that does not answer does not produce DECERR**, because the FMC does not check the presence of the slave device and simply samples floating levels. So these two can only explain "garbage data read back", not "bus error"; still worth ruling out soldering/supply.)
3. Try another MCU (if this is a prototype board).
4. The QUADSPI path (`0x90000000`, `QSPIEN` settable) errors the same way and can serve as a side check on "is this whole class of external-memory window dead"; it is also the NOR driver entry point for M2.

## 6. Four traps from this round (all reusable)

1. **`0x52004040` is not SDCR1**. SDRAM registers are at `0x52004140` (`FMC_Bank5_6_R` offset 0x140); `0x52004040` is NOR's `BWTR1`. Reading it gives all zeros, which was once misread as "the controller is not configured". The address source must be the `FMC_Bank5_6_R->SDCR[2]` comment in the CMSIS header, not memory.
2. **H7's `SDSR` has no BUSY bit**. Using it as an instrument for "did the command execute" can never sample a true value, so "the command was discarded" was really the instrument's blind spot. The observable quantity is `SDSR.MODES1`: SELF_REFRESH(01) ↔ NORMAL(00) proves the command engine really runs. **Instruments must be chosen from things that really exist on this chip.**
3. **The debugger's unaligned read silently returns 0**. `read_memory <address not aligned to the width> 32 1` on this machine's OpenOCD + CMSIS-DAP also returns `0x00000000` for **unmapped** addresses instead of erroring. That once suggested "the FMC window answered at +2". Test method: use an address that certainly nobody answers (`0xE0100002`, `0xA0000002`) as a control; it also returns 0 ⇒ the reading is invalid.
4. **Vector PDF schematics must be paired by text-layer coordinates**. Reading net labels with image OCR (view_image) misaligns whole lines; PB12/PB13 were once swapped this way. Only after `pdftotext -raw` plus coordinate-aligned pairing was it safe to write the address table.

Two more implementation facts directly relevant to this round:

5. `SystemInit` (`Core/Src/system_stm32h7xx.c`) actively writes `FMC_Bank1_R->BTCR[0] = 0x000030D2` (MBKEN=0) to disable bank1, and toggles FMCEN temporarily. So "BCR1=0x30D2 after reset" is not somebody misconfiguring it, it is the startup code.
6. H7's `SDCR` has no `SDNE` bit; bit 15 cannot be written (writing 0xADD9 reads back 0x2DD9), while `RPIPE=2` can be written. So "use the SDNE bit to enable the bank" does not exist on this chip.
7. **`make` does not track `-D`**. Switching `USE_SDRAM=0` to `1` in the same build directory relinks without recompiling and yields a "half on-chip, half external" image (measured: `USE_SDRAM=1` yet RAM still reported 44.77%, SDRAMBUF 232 KB). Changing a switch requires `make clean`. The check is whether image size and RAM percentage change with it, not whether that `-D` appears on the command line.

## 7. Conclusion

* **"Not enough space" had nothing to do with external devices**; it was on-chip budget plus software constants, and that is solved.
* **The mechanism for moving large working areas to SDRAM has landed and is off by default**: `ARKRTOS_USE_SDRAM` / `USE_SDRAM` / `--ext-ram` align in three places, and the default profile is unchanged.
* **This board's FMC external memory window did not answer** (DECERR type), confirmed repeatedly by two masters, two states, four widths and two banks, while the FMC's registers, command engine and pin drivers were all fine. **That was a hardware-level negative reading software could not work around.** (2026-11-07: the real cause was the missing `FMC_BCR1.FMCEN` master gate; after enabling it, 32 MB verified and `USE_SDRAM=1` became usable.)
* Until the board or chip is retested, the previous correct configuration was `USE_SDRAM=0` (default profile): compiler back to the on-chip budget, ramfs back to 4 slots × 16 KB, VFS mounting normally.
