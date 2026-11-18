# Memory Protection and Peripheral Permission (App privilege-escape verification)

**[简体中文](内存保护与App越权验证.md) | English**

> Condensed English edition. The Chinese page (内存保护与App越权验证.md) is
> normative: every number below is copied from it as of 2026-11-18, but if the two
> disagree, the Chinese page wins. This page adds no new claim and no new
> measurement.

Three evidence tiers are used throughout, and they are never mixed: **compiles
only** / **verified on hardware** / **unverified**.

* Sections 1-6: **verified on hardware** (STM32F427VGTx, DAPLink over SWD, kernel
  built from the F427 project, app built from `app_sdk/APP_DEMO`).
* Section 7 (the MMU backend): **QEMU runs**, not hardware. LA64 runs the real
  kernel, the real `utest` and the real `sec` command. QEMU readings are not lent
  to hardware conclusions.

## 1. What this page answers

"Let an MCU install applications like a phone does" requires that an installed
third-party app cannot touch the kernel or another app. This page states how far
that has been reached: what the mechanism is, how it was verified, and which
boundaries are explicitly **not** solved.

## 2. Mechanism: three layers, none optional

| Layer | What it does | Where |
|---|---|---|
| Build time | The app links against a nominal layout; at install time the kernel relocates the absolute RAM/ROM addresses to the **actual** placement | `tools/gen_scatter.py`, `arkrtos_loader_reloc_apply()` |
| Task creation | Every TCB carries `is_priv`. App tasks are unprivileged (`CONTROL.nPRIV = 1`), kernel tasks are privileged | `arkrtos_task.h:is_priv`, `arkrtos_cfg.c`, `arkrtos_sched_activate()` |
| Run time | The MPU builds one region set per task: its own RAM window (RW, XN), kernel RAM and peripherals (read-only or no-access as required) | `arkrtos_mpu.c:arkrtos_mpu_build_task()` |

The app RAM window comes from `SLOT_RAM_*` in `config/arkrtos_partition.h`: at
install time the kernel reserves a power-of-two block from the image pool, hands
the base to the MPU region builder, and uses **that same base** for the RAM
relocation.

## 3. The fix in this round: the RAM base must be pinned

### Symptom

After `app start <slot>` the device **resets** (`RCC_CSR` reads IWDGRSTF) and
crash-limit disables the slot. Captured with a `DEMCR.VC_MMERR` vector catch:

```
PC  = 0x080650AA   (__scatterload_copy: stmhs r1!,{r3,r4,r5,r6})
R1  = 0x20014000   (destination = the exclusive upper bound of the app RAM window)
CFSR = 0x82 (MMARVALID | DACCVIOL)   MMFAR = 0x20014000
```

### Root cause

At startup `__scatterload` copies RW initialisers using the scatter table baked
into the image, whose `dest` is the RAM base as it was at install time. But the
**allocation result is not persisted**: the power-on scan re-allocates in
"ascending pool address" order. Whenever the two allocations differ - and they do
when the app is reinstalled while the old binding still occupies the pool - every
absolute RAM address in the image is void. The copy lands on `0x20014000`, one
past the current 8 KB window, and hits the MPU.

Two readings from the board, same image, same board:

| | RAM base | Source |
|---|---|---|
| At install time (baked into flash) | `0x20014000` | `dest` field of the scatter table in device flash |
| Power-on rebuild (old behaviour) | `0x20012000` | `slot_ram_base[1]` in the partition table |

The old code comment claimed "allocation order is ascending pool address, so the
rebuild matches the load" - an assumption that does not hold across reinstall.

### Fix

**Write the RAM base allocated at install time into the image header; the
power-on scan reuses it and no longer re-allocates.**

| Change | File |
|---|---|
| New `runtime_ram_base` at the end of the image header (header length and other offsets **unchanged**; old images have 0 there, so it is backward compatible by construction) | `kernelsrc/include/arkrtos_app_image.h` |
| New `arkrtos_ptable_ram_reserve(base, size)`: reserve at an explicit base, validating bounds, alignment and overlap | `kernelsrc/src/arkrtos_ptable.c` |
| Install writes the allocated base; the scan reserves it **first** and, if it cannot, **reports an error and leaves the image without a RAM block** (`start` refuses and asks for a reinstall) instead of silently picking a different address | `kernelsrc/src/arkrtos_loader.c` |
| CRC: `runtime_ram_base` enters as 0, like `crc32`/`state` - it is local information filled in just before install, not transported content; including it would make every install report a CRC mismatch | two CRC sites in `arkrtos_loader.c` |
| Packaging side: `verify` rejects a non-zero value (that means a board-dumped image was used as a packaging product) | `tools/pack_app.py` |

**Why "rather than error, survive" is not an option**: one changed RAM base
invalidates hundreds of absolute addresses at once. Starting it at a different
address only yields a MemManage nobody can read. Saying "reinstall it" is the only
honest answer.

## 4. Verification

### 4.1 Positive: the app runs its full self-test unprivileged

```
app install /APP_DEMO.svcapp      ->  app install: ok, slot 2
APP_TEST summary: pass=78 fail=0
APP_ALIVE t=... cpu=0% ...        (110 s continuous, no reset)
```

Coverage: device IO, tasks and delay, semaphore/mutex/event, message queue, soft
timers, time base, CPU load, fault counters, slot state, logging, shell
registration, **FPU**, the POSIX/Win compatibility layer, the file system. All
pass, so the SVC path, the shared window and FPU use are not damaged by the MPU in
unprivileged state.

### 4.2 Negative: two deliberately offending images

`example/stm32f427/app_sdk/APP_BAD` (a probe, not a sample):

| Probe | Behaviour | Expected |
|---|---|---|
| probe1 | Write a word into kernel private RAM (`0x20003000`, outside this app's window) | MemManage |
| probe2 | Unprivileged write to `SCB->SHCSR` (`0xE000ED24`) | BusFault / HardFault |

The `fault` reading on the board:

```
#   tick      task  type
 0   134746    7     MEMFAULT    <- probe1
 1   134746    7     RECOVER
 2   190824    8     BUSFAULT    <- probe2
 3   190824    8     RECOVER
```

At the same time:

* The `"NOT blocked"` branch after each probe **never printed** - both attempts
  were blocked.
* Both probe slots (4, 5) show `crash=1` and are still `RUNNING` - per the
  terminal policy the kernel **recovers the task**, not the board.
* Another instance that kept offending (slot 3) shows `crash=3`, `INVALID`,
  `held=crash-limit` - repeated escapes get the slot disabled, which is the
  intended end state.
* The shell stayed responsive (`app list`, `fault`) and slot 2's APP_DEMO kept its
  heartbeat - **the rest of the system is unaffected**.

## 5. Known boundaries (not solved - do not read as solved)

1. **Only unprivileged tasks are constrained.** Kernel tasks (including driver
   framework code that runs privileged) are not MPU-limited. That is design, not
   an omission.
2. **The shared window is still writable by apps.** The shared partition table and
   the crash log live in one window; making that window read-only would block the
   kernel's own write path, so it stays writable. An app could in theory edit
   partition-table fields (not treated as a security boundary today).
3. **App-registered shell callbacks execute app code in a privileged context.**
   The shell runs inside a kernel task, so the callback gets privilege. Fixing it
   needs an SVC trampoline plus an unprivileged stack - not cheap, not done.
4. **`arkrtos_loader_accept` does not check the payload CRC (header only)**, and
   `signature[64]` is not verified - left to a later decision (today only
   integrity: monotonic version + image_id + CRC).
5. **The fault ring is cleared by reset** (it lives in RAM). To see "the moment it
   happened", read before the reset; cross-reset crash counts are carried by
   `arkrtos_crash_journal` in the `.noinit` section.

## 6. Reproduction

```bash
# 1. Build and flash the kernel (F427)
UV4 -r example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx
UV4 -f example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx

# 2. Package and upload the app
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx \
      --type app --name APP_DEMO --out build/APP_DEMO/APP_DEMO.svcapp
py -3 tools/fs_put.py build/APP_DEMO/APP_DEMO.svcapp --port COM3 --path /APP_DEMO.svcapp

# 3. In the shell
fs mount
app install /APP_DEMO.svcapp     # expect pass=78 fail=0
```

> Evidence tip: before a long memory dump, freeze the independent watchdog by
> writing `DBGMCU_APB1_FZ` bit12 (`DBG_IWDG_STOP`), otherwise the dump itself resets
> the scene. To catch the offending instruction use `DEMCR` bit4 (`VC_MMERR`) as a
> vector catch: the fault handler runs at microsecond scale and
> halt/read/resume polling will always miss it.

## 7. Two isolation backends: windows vs page tables (2026-11-16)

> Tier: this section is **QEMU runs** (LA64 running the real kernel, `utest` and
> `sec`; RV32 for regression), **not hardware**. Not one word of the hardware
> conclusions in sections 1-6 is changed or overridden here.

### 7.1 Why two backends

"Keep apps out of the kernel" lands on **completely different hardware** on
different chips:

| Backend | Switch | Isolation by | Cores | Implementation |
|---|---|---|---|---|
| Window | `ARKRTOS_USE_MPU=1` | ARM MPU / RISC-V PMP **physical window registers**: a few base+size ranges, anything outside faults | Cortex-M3/M4/M7, RV32IMAC (PMP) | `kernelsrc/src/arkrtos_mpu.c` |
| Page table | `ARKRTOS_USE_PAGING=1` | **TLB mapping plus privilege level** (LoongArch PLV0 kernel / PLV3 user): the address is mapped first, then permission-checked | LoongArch LA32/LA64; a future RV64 port | `kernelsrc/src/arkrtos_paging.c` |

The first is the typical MCU shape, the second the typical MMU-chip shape. They are
**not two APIs** but one contract with two implementations.

### 7.2 One contract, two implementations (names unchanged)

The five entries in `kernelsrc/include/arkrtos_mpu.h` are the contract:

```c
arkrtos_mpu_module_init();      /* initialise the isolation hardware */
arkrtos_mpu_build_task(tcb);    /* attach this task's isolation context */
arkrtos_mpu_set_app();          /* switch to the app side */
arkrtos_mpu_set_idle();         /* switch back to the kernel side */
arkrtos_mpu_reset();            /* reset */
```

`mpu` in those names is historical. The repo has **8 call sites**, one of them in
`arkrtos_task.c`; renaming would touch the header, the 8 call sites, the port layer
and the docs for cosmetic gain only. So the names stay and the semantics are read
as "attach this task's isolation context". A mutual-exclusion macro keeps the two
translation units from clashing: with `USE_PAGING=1` the `arkrtos_mpu.c` object is
empty, otherwise `arkrtos_paging.c` is entirely inside `#if` and yields an empty
TU.

### 7.3 Three guards, all `#error`, no silent downgrade

In `kernelsrc/include/arkrtos_config.h`:

| Condition | Error text (excerpt) |
|---|---|
| `USE_PAGING=1` but the core has `ARKRTOS_ARCH_HAS_PAGING_HW != 1` | `ARKRTOS_USE_PAGING=1 但本核心没有页表/TLB` |
| Both backends are 1 | `ARKRTOS_USE_PAGING=1 与 ARKRTOS_USE_MPU=1 互斥` |
| `USE_PAGING=1` but `USE_PRIV != 1` | `ARKRTOS_USE_PAGING=1 需要 ARKRTOS_USE_PRIV=1` |

Why not "pick one automatically": violating any of the three yields not a
suboptimal configuration but **a binary that claims isolation and has none**.
Erroring out is the only honest answer.

Two easily confused symbols, kept apart:

| Symbol | What it is | Who reads it |
|---|---|---|
| `ARKRTOS_ARCH_HAS_PAGING_HW` | **Hardware fact**: does this core have a TLB/page table. Per core, defined in `arkrtos_arch.h` (M3/M4/M7/RV32IMAC = 0, RV64GC/LA32/LA64 = 1) | Only the guards in `arkrtos_config.h` |
| `ARKRTOS_USE_PAGING` | **Does this build use** the paging backend. Kconfig symbol, default 0 | `arkrtos_mpu.h`, `arkrtos_paging.c`, the port layer, `arkrtos_sec.c` |

RV64GC has the hardware bit set to 1, but there is no RV64 port and no page-table
init here, so `USE_PAGING` is still 0. `sec`'s `iso=` reads only the latter; reading
the hardware bit would make a build with paging switched off print `iso=2` - a
false green.

### 7.4 What LA64 actually does: static mapping plus a load-time span audit

**The main mappings are static**, built once by `la_mmu_init()` from
`arkrtos_port_irq_init()` in `kernelsrc/port/loongarch/la64/arkrtos_port.c`. Since
2026-11-17 there are also runtime-registered 4 KB entries (see §5.16 of the QEMU
page); the four static ones are unchanged:

| Span | Size / page size | Permission |
|---|---|---|
| Kernel low 16 MB | 16 MB, PS=22 x2 | PLV0, RWX |
| Kernel UART | 4 KB | PLV0 |
| App area `0x02000000` | 2 MB, PS=20 | **PLV3**, RWX |
| App UART alias `0x02202000` | 4 KB | **PLV3**, RW |

`ASID` is fixed at 0: this machine has no hardware page-table walk, so "switch page
table" means **rewriting the single PS=20 app-window entry**. `arkrtos_mpu_set_app()`
rewrites that entry according to the task's binding when switching to an app task;
kernel tasks do not switch (they run on the identity map), and the privilege level
is still delivered by `s_next_pplv` and the exception frame. `0x02000000` is
therefore the same VA in every space but a different physical frame: one app cannot
even name another app's frame. None of the static entries move.

`ARKRTOS_PAGING_APPS_MAX` defaults to **1**: then `activate()` returns early on
"already in this space" and the dispatch path is byte-identical to before; the
sample board sets it to 2 only to produce readings.

**So what is the real gain of the paging backend?** The **load-time span audit**:
`arkrtos_mpu_build_task()` calls `arkrtos_port_paging_check(base, size, mem)` for
each task's ROM and RAM range, requiring the whole range to fall inside a
**single** mapped span with matching attribute bits; otherwise it records the first
`bad_base` and increments `bad`.

```c
int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p_bad_base);
```

Three semantics pinned, all to avoid a plausible-looking wrong answer:

1. **Audit only, never repair**: an out-of-span range is counted, not silently
   widened until the check passes;
2. **`size == 0` is skipped**: an empty range is not a violation;
3. **Runtime dynamic mapping supports only kernel-visible (PLV0) 4 KB pages**
   (2026-11-17): a driver registering an MMIO window absent from the static table
   calls `arkrtos_paging_map_span(va, pa, size, mem)`. **App-visible** dynamic
   mapping needs ASIDs or per-process tables (U-1), so the interface refuses
   instead of approximating.
4. **Unmap (U-15, 2026-11-17)**: `arkrtos_paging_unmap_span(va, size)` removes page
   by page; static spans are refused with -2 (removing a kernel entry is the next
   fetch refill) and an unregistered span with -4 rather than silent success. The
   port side does two steps - invalidate the STLB/softmmu by VA with `invtlb`, then
   write the MTLB slot back as V=0 - because doing only the second leaves the books
   clean while the old translation still serves (measured: rebind got all 16 words
   wrong). A whole-table flush is still not done.

### 7.5 QEMU readings

| Item | LA64 (`USE_PAGING=1`) | RV32 (default, `USE_PAGING=0`) |
|---|---|---|
| Build | `make -f example/qemu_loongarch64/Makefile -j8` | `make -f example/qemu_riscv32/Makefile -j8` |
| Image | `ark_qemu_la64.elf` = **2,335,944 B** | `ark_qemu_rv32.elf` = **2,012,924 B** |
| `utest` | **13 matched, 13 passed, 0 failed** | **13 matched / 133 assertions / 0 failed** |
| `sec` `gates.kernel` | `[PASS] ... iso=2 sign/hwsafe=0x3` | `[PASS] ... iso=1` |

How to read it:

* LA64's `iso=2` proves the **page-table backend is actually running**, not "switch
  on, code not wired". That bit was 0 before the change (LA64 has no MPU entity and
  `USE_MPU=0`), so bit0 was permanently FAIL - in other words LA64 had **no address
  isolation backend at all** before this.
* RV32's byte-identical size is the evidence for "default off means nothing
  changes". The Chinese page carries the full history of that number (2,012,860 B
  at first measurement, 2,012,876 B on `127ce6d`, 2,012,924 B after
  `ARKRTOS_USE_ELF` was added with `text` growing 12 B from one diagnostic guard in
  `arkrtos_task_kill_internal`). The criterion stays **measured down to the byte**,
  not "read the code and it looks fine".
* LA64 with `USE_PAGING=0` was **not measured separately** this round (that
  configuration has no usable isolation backend on LA64 and is not releasable); the
  blank is not "verified zero change".

### 7.6 Done versus outstanding (do not read "can switch" as "MMU port finished")

| Capability | Status |
|---|---|
| Two backends selectable by Kconfig, contradictory configs rejected at compile time | **Done** (three `#error` guards plus `tools/paging_gate.py`) |
| Static mappings (kernel / app / UART) and PLV3 user mode | **Done** (LA64 port, QEMU runs) |
| Load-time span audit with counters (`arkrtos_paging_stats`) | **Done** |
| `sec` reports `iso=2`, distinguishable from `iso=1` on the window backend | **Done** |
| App runs its full self-test unprivileged | **Done** (LA64 QEMU `utest` 13/13) |
| Installing an **executable file** into the user window and running it (`exec`, or typing the path, Linux entry convention) | **Done** (`ARKRTOS_USE_ELF=1`; M1 is one window and one instance, U-1/U-2 made it "one frame per space, two concurrent instances"; see the ELF page). This is "file is program", **not** a full process |
| One address space per app (the **mechanism**) | **Done (2026-11-17)**: the space number is the frame number (`space n -> 0x02000000 + n*2 MB`), `set_app` rewrites the PS=20 app entry, VA stays `0x02000000`; reading `pmap u1: PASS - same VA, two frames` (space 0 reads `0xA5A50001`, space 1 reads `0xA5A50002`), and `pmap look` prints entry number and PPN. Pitfall: QEMU's `tlbwr` does not invalidate large-page **odd** pages; before switching you must issue `invtlb 3` by VA or the switch silently fails |
| Concurrent multiple instances at the same VA | **Done (U-2, 2026-11-17)**: the loader keeps a per-space ledger (`s_elf_task[]`, `elf_slot_live()`), segments are staged through the kernel and written into the target space's frame in 4 KB steps; `pmap u2` shows both instances finishing and saying `bye`, `pmap elf` shows 1732 B per space and `0 live`. Root cause that cost the round: the trap frame and the inline switch frame live on the **outgoing task's own stack**, which for an app is inside the window VA, so rewriting the window entry mid-switch made the caller read the other app's frame (`era=0xcdcdcdcd`). Fix: when `sp` is inside the window only record it as pending; the `.S` restore path first switches to the identity-mapped static stack and only then performs `activate` plus the PRMD writeback |
| Page-frame allocator | **Not done** |
| Page-fault handling | **Not done** |
| `copy_from_user` / `copy_to_user` | **Done (U-6, 2026-11-17)**: `arkrtos_copy_from_user` / `arkrtos_copy_to_user`, `*_task` variants and the backend `arkrtos_paging_user_copy`; four refusal codes **-1 bad argument/wrap, -2 no user window, -3 range not wholly inside the window (write direction only accepts the RAM window), -4 the attached space is not the caller's**; the criterion is the whole range, not the first address. Reading `pmap cu: PASS - from/to user, refusals -1/-2/-3/-4, kernel survived` (the kernel keeps printing after bad pointers). Non-paging builds fall back to `(-1)` and the default build stays byte-identical; the SVC handler itself still does not route through `copy_*` |
| Runtime dynamic mapping (`arkrtos_port_paging_alias`, `arkrtos_paging_map_span`) | **Done, kernel PLV0 only, 4 KB, at most 8 page pairs**: same VA different PA returns -5, a range inside a static span returns -2, a real device page reads the same value through the new VA and the write direction emits a character |
| Unmap / release | **Done (U-15)**: `pmap unmap`, `pmap dyn`, refusal codes -1/-2/-4, the slot is reclaimed when both halves are empty |
| SMP (cross-core TLB shootdown) | **Not done** |

Acceptance criteria for these outstanding items - measure against these, not
against "it starts":

1. Two apps each have their own page table, **the same VA** in the two apps points
   at different physical pages and they do not interfere (**mechanism reached**:
   `pmap u1`; **concurrency** also reached: `pmap u2`);
2. Three negative probes (write kernel RAM / write another app's RAM / jump to an
   unmapped address) are **all** blocked, each with its own fault reading;
3. The kernel keeps a static direct map and is not affected by user page tables;
4. `copy_from_user` returns an error for an illegal user pointer and **does not
   fault the kernel** (**reached**, U-6: 11 criteria in `pmap cu`).

### 7.7 Reproduction (QEMU)

```bash
# LA64 (paging backend, iso=2)
make -f example/qemu_loongarch64/Makefile -j8
cd build/qemu_loongarch64 && (sleep 6; printf 'utest\n'; sleep 22) | \
  timeout 36 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -machine virt -kernel ark_qemu_la64.elf -nographic

# RV32 (default, MPU backend, iso=1)
make -f example/qemu_riscv32/Makefile -j8
cd build/qemu_riscv32 && (sleep 6; printf 'utest\n'; sleep 14) | \
  timeout 24 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe" \
  -machine virt -cpu rv32 -bios none -kernel ark_qemu_rv32.elf -nographic

# Gate: default 0 / LA64 builds with it on / RV32 must be refused / both-on must be refused
python tools/paging_gate.py
```

> Do not invent QEMU arguments: `-cpu loongarch64` does not exist
> (`unable to find CPU model`), and RV32 without `-cpu rv32 -bios none` collides with
> the opensbi region. The `-machine` / `-cpu` / `-bios` / `-kernel` values match the
> `run:` targets in `example/*/Makefile`; the only difference is that `run:` uses
> `-serial file:out.txt` while the pipelines above use stdio to feed commands. The
> `sleep` timings are empirical - feed the command only once the `utest` prompt
> really appears, or it is dropped.
