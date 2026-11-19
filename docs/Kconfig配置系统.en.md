# Kconfig Configuration System

> Condensed English edition. The Chinese page (Kconfig配置系统.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

Ark RTOS collects "what to compile and how big" out of scattered header `#define`s into
a Linux-shaped Kconfig menu. It pulls in no third-party dependency: parser, generator
and menu editor are all plain Python under `tools/kconfig/`, runnable on any machine
with Python 3.

## 1. Design baseline

| Choice | What we did | Why |
|---|---|---|
| Syntax | Subset of Linux Kconfig | Anyone who has used the kernel needs to learn nothing new |
| Parser | Self-written pure Python (`kconfig.py`) | No pip dependency, offline builds |
| Symbol names | **Same name as the C macro**, no `CONFIG_` prefix | The name in the menu is the name in the code; no translation layer |
| Output | `kernelsrc/include/arkrtos_autoconf.h` | One header, included by kernel and SDK alike |
| Override | Every macro wrapped in `#ifndef` | Board headers (capability/resources) can still override; **project-level macros are now cleared**, see §8 |

Symbol `ARKRTOS_USE_LWIP` therefore generates:

```c
#ifndef ARKRTOS_USE_LWIP
#define ARKRTOS_USE_LWIP 1
#endif
```

## 2. Directory layout

```
Kconfig                        # root menu: mainmenu + 4 source lines
config/kconfig/
    Kconfig.features           # feature gating (core kernel / sync / services / reliability)
    Kconfig.capacity           # static capacity limits
    Kconfig.posix              # POSIX layer capacity
    Kconfig.net                # lwIP port capacity
    board/Kconfig.qemu_loongarch64  # MMU-tier board layer: geometry / clocks / capability (read only by genboard.py)
config/arkrtos_defconfig       # source of the shared header (= F427 example config, used by the Keil project)
config/stm32h743_defconfig     # per-build base for the H743 GCC project
config/stm32f427_gcc_defconfig # per-build base for the F427 GCC project (differs only in RTSTAT/SECCHECK=n)
config/minimal_defconfig       # measured minimum profile (every optional feature off; sizes in the feature-switch page)
config/minimal_noshell_defconfig # minimum + Shell off (the ≈37.8 KB row)
config/qemu_loongarch64_defconfig # MMU-tier board values (-> board/qemu_loongarch64/arkrtos_board_config.h)
kernelsrc/include/arkrtos_autoconf.h   # generated artifact (committed)
tools/kconfig/
    kconfig.py                 # tokenizer + recursive-descent evaluation + fixed-point solve
    genconfig.py               # generates autoconf.h, doubles as --check
    genboard.py                # generates the MMU-tier board header, doubles as --check (section 9)
    menuconfig.py              # line-oriented config editor (testable via --script)
```

> Note: the Windows filesystem is case-insensitive, so a file named `Kconfig` and a
> directory named `kconfig/` cannot coexist at the repository root. The four menu
> fragments therefore live in `config/kconfig/`.

## 3. Injection point

`kernelsrc/include/arkrtos_types.h` is the lowest header every translation unit of
kernel and SDK reaches; the generated file is included there:

```c
#include <arkrtos_autoconf.h>
```

This layer is chosen so that App side (POSIX / socket capacity macros) and kernel side
see the same values.

**All three includes use angle brackets** (`arkrtos_types.h:15`,
`arkrtos_features.h:31`, `ark_shell/ark_shell_config.h:12`). This is not style: the GCC
projects inject a per-build header at the front of `-I` to shadow the shared header
(§8), and `#include "…"` **searches the includer's own directory first**, then `-I`, so
placing `-I` first cannot shadow it. The observed symptom is "changing the defconfig
changes nothing".

## 4. Usage

```bash
# list all symbols with current values
py -3 tools/kconfig/genconfig.py --list

# generate the header from defaults (--kconfig defaults to ./Kconfig: must be explicit from a subdirectory)
py -3 tools/kconfig/genconfig.py --kconfig Kconfig --defconfig config/arkrtos_defconfig \
      --out kernelsrc/include/arkrtos_autoconf.h

# verify the committed header matches Kconfig + defconfig (CI)
py -3 tools/kconfig/genconfig.py --kconfig Kconfig --defconfig config/arkrtos_defconfig \
      --out kernelsrc/include/arkrtos_autoconf.h --check

# interactive edit (also accepts --script for automation)
py -3 tools/kconfig/menuconfig.py --defconfig config/arkrtos_defconfig
```

`--savedefconfig` writes the current configuration back as a defconfig;
`--olddefconfig` fills new symbols from an old configuration.

### 4.1 Inactive `int` symbols generate their default, not the lower bound (2026-11-17)

An `int` / `hex` symbol whose `depends on` is unsatisfied still needs a **usable
literal** in the generated header, because overriding the parent switch with `-D` is
legal usage: `-DARKRTOS_USE_SHELL_AUTH=1` turns on a sub-switch whose parameters were
configured while the tree had it off.

The early implementation sent inactive `int`s through the sentinel `0`, and
`_apply_ranges()` then lifted `0` to the `range` lower bound — so
`ARKRTOS_SHELL_AUTH_TRIES` (`range 1 100`, `default 5`) generated `1` in the OFF tree,
and after `-D...=1` overrode the parent the **failure tolerance silently became 1**,
while the documentation and the `arkrtos_features.h` default both say 5. Now an
inactive `int` / `hex` takes its own first satisfied `default` and then goes through
the same `range` clamp (`_compute()` in `tools/kconfig/kconfig.py`). Only
`ARKRTOS_SHELL_AUTH_TRIES` is affected in the current tree; the other 108 symbols
generate byte-identical values.

Acceptance: after regeneration, `--list` must show `5` for
`ARKRTOS_SHELL_AUTH_TRIES`, and `git diff kernelsrc/include/arkrtos_autoconf.h` must
contain only the added-symbol lines.

## 5. Dependencies

Hard constraints between features are written in Kconfig (`depends on`) and in the
`#error`s of `arkrtos_features.h` — the same rule expressed twice, so whichever fires
first blocks a bad combination:

| Turn on | Requires |
|---|---|
| `ARKRTOS_USE_FS` | `ARKRTOS_USE_BLK` |
| `ARKRTOS_USE_VFS` | `ARKRTOS_USE_BLK` |
| `ARKRTOS_USE_VFS_LFS` | `ARKRTOS_USE_VFS` && `ARKRTOS_USE_FS` |
| `ARKRTOS_USE_MINIAPP` | `ARKRTOS_USE_FS` |
| `ARKRTOS_USE_LWIP` | `ARKRTOS_USE_MQ` |

Setting a dependency-unsatisfied item to `y` in menuconfig is refused explicitly
(`REFUSED`) instead of quietly producing a configuration that will not compile.

### 5.1 `ARKRTOS_USE_PAGING` is deliberately absent from that table

`ARKRTOS_USE_PAGING` selects which isolation back end to use (see the feature-switch
page and §7 of the memory-protection page). It cannot be expressed as `depends on`
for a concrete reason: what it depends on is **not a Kconfig symbol** but two things
Kconfig does not own —

* `ARKRTOS_USE_PRIV`: a board capability (§8), not in Kconfig;
* `ARKRTOS_ARCH_HAS_PAGING_HW`: a **hardware fact bit** per core
  (`kernelsrc/include/arkrtos_arch.h`).

So its legal combinations are guarded by three `#error`s in `arkrtos_config.h`: paging
without a TLB / paging together with `ARKRTOS_USE_MPU` / paging without privilege
levels. All hard errors, no automatic downgrade. `tools/paging_gate.py` turns these
four cases (default off + three refusals) into a re-runnable gate and is item 36 of
`tools/ci_gate.py`; item 37 is `tools/elf_gate.py` (the same pattern for the ELF64
executable loading switch), item 38 is `tools/modld_gate.py` (VxWorks-style
downloadable modules), item 39 is `tools/payload_sign_gate.py`
(`ARKRTOS_USE_EXEC_SIGN`: trailing-tag signature gate for ELF and loadable modules,
including byte-for-byte kernel-MAC vs Python comparison and ON/OFF symbol probes), and
item 40 is `tools/stack_guard_gate.py` (`ARKRTOS_USE_STACK_GUARD`: stack-bottom guard
word, judged by reading `stack_bottom[0]` rather than comparing PSP; hooks in the
scheduler switch and the LA64/RV32 C trap entries; zero symbols when off).

Further switches that add **no new ci_gate item**:

* `ARKRTOS_USE_EXIT_CODE` (U-8 task exit-code channel, default n, only
  `board/qemu_loongarch64` sets 1). Its gate lives in the existing
  `tools/elf_gate.py` (F pin, with mutation self-check), because the channel spans
  `svc_abi.h` / `arkrtos_task.c` / `arkrtos_elf.c` / shell / `oslib` — five places —
  and a separate gate would duplicate the same "eight places must exist together"
  assertion.
* `ARKRTOS_USE_PAGING_FRAMES` (U-3 stage 1, 4 KB frame pool, default n, only
  `board/qemu_loongarch64` sets 1). In Kconfig it is a bool `depends on
  ARKRTOS_USE_PAGING`, but the real legality (`ARKRTOS_USE_PAGING == 1`, the board
  supplying `ARKRTOS_QEMU_FRAME_BASE` / `ARKRTOS_QEMU_FRAME_COUNT`, base 8 MB aligned,
  frame count 1..2048) is guarded by four `#error`s in the port. The gate is the
  **R pin** of `tools/paging_gate.py` (two mutation self-checks), because the switch
  spans seven places and "missing one place means silently no effect" is exactly what
  a pin is for.
* `ARKRTOS_USE_PAGING_PAGES` (U-3 stage 2, page-level App window, default n, only
  `board/qemu_loongarch64` sets 1) is a bool `depends on ARKRTOS_USE_PAGING` **plus**
  `depends on ARKRTOS_USE_PAGING_FRAMES` — the window is laid out in pages and pages
  can only come from the pool, so either one missing fails to compile (two matching
  `#error`s in the port, plus "window size must be a multiple of 8 KB"). It changes
  **how the window is mapped** (2 MB as one PS=20 entry → 256 entries with PS=12,
  frames from the pool, per-page `invtlb` on switch), not the load timing: still eager
  (fully populated at load). Frame reclamation is the `pmap rel` line in §5.24. The
  gate is the **S pin** of the same `tools/paging_gate.py` (two mutation self-checks),
  asserting the three switch sites, window geometry, combination guards, the `-3`/`-2`
  refusal codes, last-page even-page convention, the two fixed UART ways, **the old
  PS=20 path still present**, and the `window_va` forwarding chain in hal / mpu.h /
  paging.c / shell.
* `ARKRTOS_USE_PAGING_DEMAND` (U-3 stage 3 / U-5 demand paging, default n, **default 0
  in board configs too**) is the **only switch that produces an image from the command
  line `EXTRA_CFLAGS` alone** without a board setting: it changes the shape of the
  refill entry in `arkrtos_context.S` (whether all GPRs are saved), and the baseline
  build must stay byte-identical, so the demand profile gets its own `BUILD` directory:

```bash
make -f example/qemu_loongarch64/Makefile BUILD=build/qemu_la64_demand \
     EXTRA_CFLAGS=-DARKRTOS_USE_PAGING_DEMAND=1 -j8
```

  In Kconfig it is a bool `depends on ARKRTOS_USE_PAGING` **plus** `depends on
  ARKRTOS_USE_PAGING_PAGES` (demand paging decides *when* to install those 256
  entries, so without the page window there is nowhere to land), and legality is
  additionally guarded by two port `#error`s (`=1 requires
  ARKRTOS_USE_PAGING_PAGES=1` and `=1 requires ARKRTOS_USE_PRIV=1`). The gate is the
  **T pin** of `tools/paging_gate.py` (two mutation self-checks + **a real compile**:
  `-fsyntax-only` for C, real assembly to `.o` for the `.S`). Note `EXTRA_CFLAGS`
  enters both `CFLAGS` and `ASFLAGS`: `.S` goes through cpp and must see board macros
  to pick the branch; the `-include` itself changes not one byte of the default
  profile (with a fixed `SOURCE_DATE_EPOCH`, the `--strip-debug` image hashes the same
  as before the change).

## 6. Capacity limits

Formerly hard-coded limits in headers (task table, sync object table, queues, timers,
device table, sockets, file descriptors, pthread keys, directory entry name length …)
are now Kconfig symbols.

The migration principle is **zero behaviour change**: Kconfig only supplies defaults,
the C side keeps the same defaults behind `#ifndef` guards. That means —

* without running Kconfig, behaviour is exactly as before;
* to change a limit, edit the defconfig or pass `-D` on the command line; both work.

The F427 kernel reports `Code=172734` both before and after the migration — the
measured evidence for that principle.

## 7. Relationship with `arkrtos_features.h`

`arkrtos_features.h` remains the place where code evaluates `#if ARKRTOS_USE_XXX`;
Kconfig **decides** the values and guarantees they form a legal combination. The two
responsibilities do not overlap.

This division requires "every switch in features.h must have a `config` in Kconfig" —
otherwise it is invisible in menuconfig and cannot be changed from a defconfig.
Several switches once existed only in features.h (`ARKRTOS_USE_LWIP / _MEMPOOL /
_PROCFS / _SOFTBUS / _UTEST`, `ARKRTOS_HW_SAFE` and two MiniApp knobs); all are now
in Kconfig and aligned one-to-one with the `#ifndef` defaults. The
`features-switches` check in `tools/ci_gate.py` watches this constraint — miss one and
it goes red.

After the fill-in, the F427 kernel rebuild gives `Code=211002 RO-data=17378 RW-data=888
ZI-data=116316`, byte-identical to before: the macro values were already the same,
Kconfig merely made them **menu-configurable**.

## 8. Project-level macros cleared: Kconfig is the single source of values (2026-11-14)

Before this, the `#ifndef` in `arkrtos_autoconf.h` meant **a command-line `-D` always
won**: `ARKRTOS_USE_LWIP=1` etc. hard-coded in Keil's `Options → C/C++ → Define`
could not be turned off by setting `n` in Kconfig (trimming to the minimum profile
died on `#error "ARKRTOS_USE_LWIP=1 requires ARKRTOS_USE_MQ=1"`). This round cleared
the project-level macros and moved the "per-build knobs" onto Kconfig too:

| Project | Old project macros | Now |
|---|---|---|
| F427 Keil | `ARKRTOS_USE_LWIP=1` `_SOFTBUS=1` `_HW_SAFE=1` `_RTSTAT=1` `_SECCHECK=1` `ARKRTOS_USE_FPU=1` | `<Define>` keeps only `USE_HAL_DRIVER,STM32F427xx,ARKRTOS_BOARD_CONFIG="…"`; those 5 feature switches moved into `config/arkrtos_defconfig` |
| F401 Keil | same as above + `ARKRTOS_USE_FPU=1` | `<Define>` keeps three entries; the 5 extras in the shared header (= F427 config) are turned off by `#undef` + `#define 0` in `board/stm32f401/arkrtos_board_config.h` |
| H743 GCC | `-DARKRTOS_USE_ARKVIM=1` `-DARKRTOS_USE_SDRAM=1` and more | no `-DARKRTOS_USE_*` left in `DEFS`; `make USE_ARKVIM=0` becomes a **knob** written into the per-build defconfig |
| F427 GCC | `-DARKRTOS_USE_LWIP=1` `-DARKRTOS_USE_SOFTBUS=1` `-DARKRTOS_USE_HW_SAFE=1` | same; only `-DUSE_HAL_DRIVER -DSTM32F427xx -DARKRTOS_USE_FPU=1 -DARKRTOS_BOARD_CONFIG=…` remain |

Four categories stay as `-D`; **none of them is an Ark kernel feature switch**, so do
not move them into Kconfig:

* `ARKRTOS_USE_FPU` / `ARKRTOS_USE_MPU` / `ARKRTOS_USE_PRIV`: board capabilities; the
  root `Kconfig` says explicitly "Board capability is NOT here"; also the context
  switch `.s` goes through gcc/armasm preprocessing and never sees a board header.
  **`ARKRTOS_USE_PAGING` is not in this category**: though paired with
  `ARKRTOS_USE_MPU`, it does not describe whether the hardware has that unit but
  "which isolation back end this build chooses" — a pure software trade-off, hence a
  Kconfig symbol (`config/kconfig/Kconfig.features`, `default n`, landing in
  `arkrtos_autoconf.h` as `#define ARKRTOS_USE_PAGING 0`). One symbol governs
  capability, the other governs choice; do not put them on the same layer because the
  names look alike;
* `ARKRTOS_BOARD_CONFIG`: it *is* the "which board header" selector — chicken and egg;
* `MDK_TRACE_*` / `LFS_NO_*` / `ARK_BUS_*` / `ARKVIM_*` / `ARKC_*` / `ARK_RAMFS_*`:
  upstream components' own compile-time budgets, not kernel menu items;
* `USE_HAL_DRIVER` / `STM32F4xx`: the chip vendor SDK's convention.

### 8.1 GCC side: a per-build header shadows the shared one

The GCC projects must keep `make USE_ARKVIM=0` (one value per invocation) working
while not letting `-D` beat Kconfig. The mechanism is **generating a header per build
and putting it first in `-I`**:

```
make
  ├─ generate build/kconfig/<board>_defconfig  = config/<board>_defconfig + this invocation's knobs
  ├─ generate build/kconfig/arkrtos_autoconf.h (genconfig.py, --kconfig $(ROOT)/Kconfig)
  └─ INCS starts with -Ibuild/kconfig  ⇒  #include <arkrtos_autoconf.h> hits it
```

Three supporting details; missing one yields "looks wired up, actually inert":

1. **`.DEFAULT_GOAL := all` must be explicit.** Once a "generate defconfig" rule
   exists, make takes the first target as default; `make` then only generates the
   defconfig and exits: no artifact timestamp change, `exit 0`, nothing compiled.
2. **`cmp` guards mtime**: the defconfig recipe runs every time (via `FORCE`) but does
   not overwrite the file when content is unchanged, otherwise every `make` triggers a
   full rebuild.
3. **`--kconfig $(ROOT)/Kconfig` must be explicit**: it defaults to `./Kconfig`, so
   running from `example/.../GCC` fails with `Kconfig not found: Kconfig`.

The side effect is welcome: change a knob ⇒ generated header changes ⇒ `-MMD`
dependency mismatch ⇒ automatic rebuild; "switching a switch requires `make clean`"
is gone.

### 8.2 Roles of the five defconfigs (do not write to the wrong one)

| File | Used by | Semantics |
|---|---|---|
| `config/arkrtos_defconfig` | source of the committed shared header `kernelsrc/include/arkrtos_autoconf.h`; F427 / F401 Keil projects | default profile = F427 example config (LWIP / SOFTBUS / HW_SAFE / RTSTAT / SECCHECK / SHELL / UTEST all on) |
| `config/stm32h743_defconfig` | H743 GCC (first line `KCFG_BASE`) | H743 example's measured config: LWIP / SOFTBUS / RTSTAT / SECCHECK etc. off |
| `config/stm32f427_gcc_defconfig` | F427 GCC (first line `KCFG_BASE`) | differs from the shared defconfig by only `RTSTAT=n` `SECCHECK=n` — historical fact + Flash 99.1% |
| `config/minimal_defconfig` | manual `--defconfig` | every optional feature off (including `ARKRTOS_USE_UTEST=n`) |
| `config/minimal_noshell_defconfig` | manual `--defconfig` | the above + `ARKRTOS_USE_SHELL=n` |

**Board differences do not go into these files**: boards express them with `#undef` +
`#define 0` in `board/<chip>/arkrtos_board_config.h`, included via
`#include ARKRTOS_BOARD_CONFIG` at `arkrtos_config.h:21` *before* autoconf ⇒ the board
value wins.

### 8.3 Gates must follow, or they go falsely red / falsely green

Once the shared header turns on lwIP and softbus with F427's real values, **gates that
run `armclang -fsyntax-only` with only `kernelsrc/include` in the include path
immediately fail to compile**:

```
kernelsrc/include/arkrtos_softbus.h:55: fatal error: 'ark_bus.h' file not found
example/stm32f427/.../main.c:49: fatal error: 'lwip_port.h' file not found
```

This is not broken code under test; the gate simply did not mirror the project
environment. The F427 project needs two more things to compile those two features:

1. include search paths: `kernelsrc/components/lwip/port`,
   `kernelsrc/components/lwip/src/include`,
   `kernelsrc/components/ark_softbus/include`;
2. softbus MCU-profile macros `-DARK_BUS_*` (in the uvprojx `MiscControls`; the
   `#error`s in `arkrtos_softbus.h` use them as gates).

`tools/arkrtos_f427_env.py` is the **single copy** of that environment; the four ON
profile gates (`tickless_host_gate` / `rtstat_syntax_gate` / `seccheck_syntax_gate` /
`swap_syntax_gate`) all take it from there. When changing the uvprojx `IncludePath` /
`MiscControls` or the softbus profile, change only this one file and all four gates
follow — do not patch `INCLUDES` inside individual gates, or the next profile change
goes red again.

The payoff of keeping this constraint is that the gates became real: changing the
default of any switch in the shared header now trips here first, instead of being
discovered on the board as "a stray `-D` in the project pushed the value back".


## 9. The board layer: generated header on the MMU tier, hand-written on MCU + MDK (2026-11-19)

Everything up to here is about *what the build contains*. **What the board has**
was, until 2026-11-19, a pile of hand-written `#define`s in
`board/<chip>/arkrtos_board_config.h`: geometry, clocks, paging windows and
capability switches in one file. Changing an address meant editing the header and
the prose that describes it, with nothing to go red when the two drifted.

The MMU tier now follows the normal-project shape: **menu + defconfig + generated
artifact + gate**. The MCU + MDK tier deliberately does not follow, see 9.3.

### 9.1 Three sources, one artifact

| Role | File | Read by |
|---|---|---|
| what this board **has** | `config/kconfig/board/Kconfig.<board>` | `menuconfig`, `genboard.py` |
| what this build **asks for** | `config/<board>_defconfig` | `genboard.py` |
| kernel feature **values** | the top-level `Kconfig` (`config/kconfig/Kconfig.*`) | `genboard.py` (same tree as `genconfig.py`) |
| artifact | `board/<board>/arkrtos_board_config.h` | the compiler (`-DARKRTOS_BOARD_CONFIG=...`) |

The generator is `tools/kconfig/genboard.py`:

```
py -3 tools/kconfig/genboard.py --board qemu_loongarch64         # generate
py -3 tools/kconfig/genboard.py --board qemu_loongarch64 --check # verify
```

The board fragment is **not** sourced from the top-level `Kconfig`. That tree has
exactly one artifact, `arkrtos_autoconf.h`, shared by every board including the
Keil MCU projects; putting something like `CHIP_FLASH_BASE` into it would compile a
LoongArch address into an STM32F427. `genboard.py` therefore merges both trees in
memory - board symbols from `config/kconfig/board/`, feature symbols from the
top-level tree - and asserts the two do not declare the same name before merging.

### 9.2 Two things the generator refuses to do

The artifact is committed so a clone builds without running the generator, which
makes "it looks right" cheap: a value can be silently eaten by a `depends on`, or a
name in the defconfig can be declared by nobody. Both are hard failures in
`genboard.py`, not warnings:

1. **Every value the defconfig asks for must survive resolution unchanged.** It
   compares `user` against `values` line by line and exits 1 with
   "defconfig asks X, Kconfig resolved Y". Without this, one wrong `depends on`
   removes a board capability and the generated header merely has a few fewer 1s.
2. **Every symbol the renderer needs must exist.** A typo, or a board fragment
   missing a `config`, reports the symbol name instead of a KeyError.

The `board-config-generated` item in `tools/ci_gate.py` runs `--check`: a
hand-edited artifact, or a defconfig change without regeneration, goes red here.

### 9.3 MCU + MDK is a **degraded tier**, not a debt

`board/stm32f401` / `stm32f427` / `stm32h743` and the two QEMU MCU boards keep a
hand-written header, for concrete reasons:

* uVision reads that header directly (`ARKRTOS_BOARD_CONFIG` in `<Define>`); adding
  a generated layer means running Python before every configuration change, with no
  make-level dependency to hang it on;
* the MCU capability set is fixed by the chip (FPU/MPU/PRIV/clock) and changes far
  less often than the MMU tier's window and frame-pool geometry;
* the long comments in the hand-written headers are bring-up calibration records
  ("125 interrupts in 10 s at InitVal = 2,000,000"), and a generator cannot produce
  that density of context.

The cost is stated plainly: **these tiers have no "change one place and the other
goes red" protection**; only the mirrored-project-environment gates of 8.3 backstop
them.

### 9.4 Current coverage

| Board | Tier | Board header |
|---|---|---|
| `qemu_loongarch64` | MMU (LA64, page table + PLV3) | **generated** (`Kconfig.qemu_loongarch64` + `qemu_loongarch64_defconfig`) |
| `la32_generic` | has paging hardware, no runtime carrier | hand-written (degraded) |
| `stm32f401` / `stm32f427` / `stm32h743` | MCU + MPU / MDK | hand-written (degraded) |
| `qemu_riscv32` / `qemu_mps2_an386` | QEMU MCU | hand-written (degraded) |

Adding a new MMU-tier board means two files (`Kconfig.<board>` and
`<board>_defconfig`) plus one `--board <board>` run; the gate's board list is
scanned from `config/kconfig/board/`, so no code changes.
