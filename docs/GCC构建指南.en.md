# GCC Build Guide (GNU Make + arm-none-eabi-gcc)

> Condensed English edition. The Chinese page (GCC构建指南.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

Besides the Keil MDK (AC5/AC6) projects, Ark RTOS ships a **GNU Make + arm-none-eabi-gcc** project
set for the **STM32F427** examples, for people who do not want MDK, want to build in CI, or want a
command line plus an open toolchain.

Both project sets **share one source file set**: the source list is derived from `.uvprojx` by
`tools/gen_gcc_sources.py`, and the linker script from `config/arkrtos_partition.h` by
`tools/gen_ld.py`. That is the "addresses defined once, everything else derived" red line landing
under the GCC toolchain — **do not hand-edit the generated files** (`sources.mk` / `*.ld`); change
the Keil project or the config header and regenerate.

## 1. Project locations

| project | directory | artifacts |
|---|---|---|
| kernel (F427) | `example/stm32f427/kernel/ARKRTOS_TEST/GCC/` | `build/ARKRTOS_TEST.elf/.bin/.hex` |
| kernel (H743) | `example/stm32h743/kernel/ARKRTOS_TEST/GCC/` | same |
| app example | `example/stm32f427/app_sdk/APP_DEMO/GCC/` | `build/app_demo.elf/.bin/.hex` |
| driver example | `example/stm32f427/driver_sdk/DRV_DEMO/GCC/` | `build/drv_demo.elf/.bin/.hex` |
| small-program example | `example/stm32f427/app_sdk/MINI_DEMO/GCC/` | `build/mini_demo.elf/.bin/.hex` |

## 2. Toolchain

Needs `arm-none-eabi-gcc` (with `ld` / `objcopy` / `size`) and GNU Make.

- **GCC**: any arm-none-eabi-gcc (verified locally with STM32CubeIDE's bundled
  `arm-none-eabi-gcc 12.3.rel1`). GNU ld must be **≥ 2.35** (the kernel link uses
  `--enable-non-contiguous-regions`, see §5).
- **Make**: GNU Make 3.82 (MinGW make locally) or newer.

When the toolchain is not on `PATH`, pick one:

```bash
# option 1: override on the command line (temporary)
make CROSS=/path/to/bin/arm-none-eabi-

# option 2: write it into a local private file (not tracked, already gitignored)
echo 'CROSS := D:/path/to/bin/arm-none-eabi-' > toolchain.local.mk
```

> On Windows prefer **Windows-style** paths (`D:/...`) over MSYS-style (`/d/...`): GNU Make runs
> recipe lines without shell metacharacters through `CreateProcess`, and MSYS paths give
> `make (e=2): The system cannot find the file specified`.

## 3. Usage (inside each project's GCC directory)

```bash
make              # generate defconfig + autoconf.h + ld, compile and link, produce elf/bin/hex + print size
make kconfig      # only generate and print this build's per-build defconfig
make size         # only print the firmware size
make gen-sources  # re-derive sources.mk from .uvprojx (run after changing the Keil project)
make clean
```

After building the kernel you can flash and verify with pyocd:

```bash
pyocd flash -t stm32f427vgtx --format hex build/ARKRTOS_TEST.hex
```

### When `make clean` is mandatory

Header dependencies are tracked automatically by `-MMD -MP` (`.d` files next to the objects):
change one header and every translation unit that includes it rebuilds; no manual clean needed.

**Feature switches are not in that category (since 2026-11-14).** Both GCC projects now generate a
per-build `arkrtos_autoconf.h`; `USE_*` knobs (e.g. `make USE_ARKVIM=0`) go into the per-build
defconfig, so when the generated header changes the `-MMD` dependencies go stale ⇒ automatic rebuild.
Switches no longer go through `-D`, so the "command line changed but the objects were not rebuilt"
trap is gone. Mechanism: [Kconfig配置系统.en.md](Kconfig配置系统.en.md) §8.1.

Measured (2026-11-14, F427 GCC): `make USE_SHELL=0` gives `text 170008 / data 336 / bss 88792`, the
default build is `259412 / 368 / 113536` — 89436 B Flash and 24744 B RAM apart. These numbers are only
evidence that "the knob is wired and the generated header really did force a rebuild"; **do not** read
them as "how big Shell is": the two toolchains drop different symbol sets (same sources, different
libc and inlining decisions). The Keil-side reading is in [功能开关.en.md](功能开关.en.md).

**Two changes still need `make clean`**: editing the partition header
(`config/<chip>/arkrtos_partition.h`, which decides addresses and capacities), and editing the source
set in `sources.mk`. Neither goes into the generated header.

> Historical fact: before this, feature switches were `-D`, so changing a switch required
> `make clean`; otherwise only the objects whose command line changed were rebuilt and the rest kept
> the old value.

### What a struct change without clean looks like (2026-11-05 instance)

Adding a field to `ark_shell_t` and then running `make` after only touching the `.c`: the translation
unit that gained the field was rebuilt, while `arkrtos_shell.o` — which holds an instance of the
struct — still had the old layout. The link passed and the size was printed, but at run time the two
sides were 4 bytes apart on the same object's field offsets; `help` first printed a screen of garbage
and then:

```
*** PANIC BUS ***   CFSR=0x00008200 (PRECISERR|BFARVALID)  BFAR=0x06020017
```

Criterion: the `PANIC` address is a wild value belonging to no legal segment (here `0x0602xxxx`), and
the failing command only reads data and touches no peripheral. Then first check whether object
timestamps are older than the header:

```bash
find build/obj -name '*.o' ! -newer ../../../../../kernelsrc/shell/ark_shell/ark_shell.h
```

Any output means an inconsistent layout — `make clean` and start over. With `.d` files in place this
command should always be empty.

## 4. Correspondence with the Keil projects

| concern | Keil side | GCC side |
|---|---|---|
| source list | `.uvprojx` file set | `sources.mk` (derived by `gen_gcc_sources.py`) |
| link layout | `build/*.sct` (derived by `gen_scatter.py`) | `build/*.ld` (derived by `gen_ld.py`) |
| startup entry | `arkrtos_*_start.s` (armasm, jumps to `__main`) | `arkrtos_*_start_gcc.s` (GNU as; does its own `.data` copy + `.bss` clear + `__libc_init_array`) |
| context switch | `arkrtos_context.s` | `arkrtos_context_gcc.s` (**lowercase `.s`**, see §5) |
| entry symbols | App=`APPSTART`, driver=`DRVSTART`, small program=`MINISTART` | **same names**, the two sides must agree |

When deriving the source list, `gen_gcc_sources.py` swaps the Keil startup and context-switch files
for their GCC counterparts (a `REPLACE` table), so both projects pick up the same business sources.

## 5. Three pitfalls you must know (GCC-side specific)

**① Assembly extensions are lowercase `.s` throughout.**
On Windows, GNU Make reports `No rule to make target` when both `%.s` and `%.S` pattern rules exist.
So all assembly in the repo is lowercase `.s`; preprocessing is forced on with
`-x assembler-with-cpp` (independent of the extension's case).

**② The kernel `.bss` spilling into CCM relies on `--enable-non-contiguous-regions`.**
On the Keil side, scatter's `.ANY (+RW +ZI)` **overflows automatically** into `RW_CCM` (64K) once
`RW_KERNEL` (56K) is full; GNU ld does not cross regions by itself. So `gen_ld.py` splits `.bss` into
`.bss >RAM` and `.bss.ccm >CCMRAM` and adds `--enable-non-contiguous-regions(-warnings)` to the link
line to reproduce the same fill order. **Requirement**: the startup file must clear **both**
`_sbss.._ebss` and `_sbss_ccm.._ebss_ccm` (a single-segment clear loop would run across the
discontiguous hole).

**③ No heap: a `_sbrk` that always fails.**
`--specs=nosys.specs`'s `_sbrk` has **no upper bound check** (confirmed by disassembly: the heap
pointer is initialised from `end` once, then only `heap_end += incr`), so as soon as the stdio chain
pulls it in it walks into the stack. The project therefore ships `arkrtos_gcc_syscalls.c` providing a
`_sbrk` that always fails (`ENOMEM`), matching the Keil side's "no heap" semantics and also removing
`undefined reference to 'end'`.

## 6. GCC layout of images (App / driver / small program)

The image directories' Makefiles generate the linker script with `gen_ld.py --target image`, in the
**development debug raw image** (`--raw`) layout: the payload lands directly on the development slot's
unit base, with no image header and no relocation table, for MDK/probe flashing and breakpoint
debugging.

- `APP_DEMO` → `--dev-slot 1 --raw` (App type)
- `DRV_DEMO` → `--dev-slot 2 --raw` (driver type)
- `MINI_DEMO` → `--type miniapp --unit 3 --units 1 --raw`

`--dev-slot N` reads "unit / unit count / type" from the development slot table in
`config/arkrtos_partition.h`, with the same semantics as `gen_scatter.py --dev-slot N`, so both sides
get the same interval and the same stack size.

The **topmost segment of the RAM window is reserved for the task stack the kernel sets up** (App/small
program 4K, driver 1K): `gen_ld.py` limits the `.data/.bss` area to the window minus the stack size,
and the kernel sets the stack top at load time — the same convention as the Keil scatter's `RW_IMG`
(already reduced by the stack) + `ARM_LIB_STACK`, so **the stack cannot be hit at compile time**.

> Note: the above is the "development debug raw image" layout. To go the **install path**
> (`.svcapp`, loaded dynamically by the kernel) an image must be linked with
> `gen_ld.py --target image` (without `--raw`, including the header offset) or along the nominal path,
> and the packing tool must generate the relocation table — that part is **not** wired on the GCC side
> yet (see §7).

## 7. Verification status

| item | status |
|---|---|
| kernel GCC project | **verified on board (F427)**: shell `version` / `info` / `mpool test` 9/9 |
| App / driver / small-program GCC projects | **compile only** (link succeeded, entry symbols and memory layout checked; no flash/load/run verification) |

Not done: **install-path packing** of GCC-side images (`.svcapp` header + relocation table) is not
implemented; only the "debug raw image" is covered. If needed later, add the nominal path to
`gen_ld.py` and wire in `tools/pack_app.py`.

**2026-11-12 accounting note (debt C6c)**: the reason this is not being done is that **it cannot be
verified on this machine**, not that it was forgotten. The existing `tools/pack_app.py` has only two
entry points — `--project <uvprojx>` (four-pass Keil differential) and offline `--bin-a..d` (with
`--axf` or `--entry-offset`) — and `probe_elf` only accepts ELF32; `tools/gen_ld.py` has only the
`--raw` layout, **no nominal/delta support**. And `which arm-none-eabi-gcc` is empty here (no GCC
cross toolchain), so the four-pass differential cannot even run, and hand-written code could not be
reconciled against the real toolchain ⇒ recorded as debt per "rather no answer than a wrong one".
Doing it needs: ① install an `arm-none-eabi` toolchain; ② add the nominal layout to `gen_ld.py`;
③ add a "GCC four passes" entry to `pack_app.py`; ④ load and run it on real hardware.
