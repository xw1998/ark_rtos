# QEMU emulator ports and verification

> Condensed English edition. The Chinese page (QEMU模拟器移植与验证.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

> Evidence tiers used throughout:
> - **Build-only**: verified up to a linked ELF, never executed on the target.
> - **QEMU-executed**: actually run inside QEMU, with raw serial output as evidence.
> - **Not verified**: designed only; neither compiled nor run.
>
> Every "passes" in this page means **QEMU-executed**, not "should work".

## 1. Why this line exists

The main verification carrier is STM32F427 / STM32H743 hardware. When the boards are away, architecture-level changes (trap entry, context switch, syscall ABI, timers) have no landing evidence — you can only "infer correctness" by reading code, which is exactly the part hardest to self-check and most prone to silent wrong answers. QEMU gives a software target machine that **really executes instructions**: it does not verify peripheral drivers, but it does verify the most dangerous part of a port (push/pop order, `mepc` write-back, register-save completeness, timer reload, SVC argument and return paths).

So this line is positioned as **execution-level checking of the port layer**, not a replacement for the board.

## 2. Environment

### 2.1 QEMU

- Version **QEMU 9.2.3** (MSYS2 mingw-w64 build).
- Path `D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe` (same directory holds `qemu-system-loongarch64.exe`).
- Source: Tsinghua MSYS2 mirror package `mingw-w64-x86_64-qemu-9.2.3-2-any.pkg.tar.zst`, about 163 dependency packages unpacked, 136+ DLLs in `bin/` at the end.
- **LA32 is not in this environment**: neither stock QEMU 9.2.3 nor upstream ships `qemu-system-loongarch32`; the Loongson-education LA32 fork publishes Linux binaries only and this machine cannot execute them (§6.1).

Two pitfalls while filling dependencies (you will hit them again on another machine):

1. **`libnfs-14.dll` must come from libnfs 5.0.3.** Other builds export an incomplete symbol set and QEMU dies at startup with `0xC0000139` (entry point not found).
2. **`vulkan-1.dll` is not in MSYS2's qemu dependency tree** and must be added separately.

Under Git Bash a missing DLL shows up as **exit 127 with zero output**, which looks like "command not found" and is easily misread as a bad path. Locate it by running through Python `subprocess` and reading the real return code:

| Return code (decimal) | Hex | Meaning |
|---|---|---|
| 3221225785 | 0xC0000139 | Entry point not found (wrong DLL version) |
| 3221225595 | 0xC000007B | Bitness mismatch (32/64-bit DLLs mixed) |

To find which dependency is missing: use `pefile` to diff the QEMU import table against the export tables of every DLL in `bin/`.

### 2.2 RISC-V toolchain

- Path `D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe`.
- Version **Xuantie-900 V2.6.1 / GCC 10.2.0**.
- **Hard constraint: this toolchain has only three multilibs.**

  ```
  rv32imafc          / ilp32f
  rv32imafdc_xtheade / ilp32d
  rv64imafdc_zfh_xtheadc / lp64d
  ```

  There is no `rv32imac` / `ilp32` multilib, therefore:
  - **no libgcc** (it would look for a nonexistent multilib) — `-lgcc` is deliberately omitted from the link line;
  - **no libc** — every libc function used must be supplied locally (see §3.7 `libc_shim.c`);
  - **no 64-bit multiply or divide** — `__udivdi3` / `__divdi3` live in libgcc, so generating one is a link-time `undefined reference`. All time arithmetic is written as 32-bit integer expressions.

  Compiling with `-march=rv32imac -mabi=ilp32` is allowed (it only affects codegen, not multilib selection) as long as no link library is touched.

### 2.3 LoongArch toolchain

- Path `D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe` (GCC 8.3.0).
- These toolchain executables are **32-bit PE**, so the i686 runtimes (`libgcc_s_dw2-1.dll` etc.) must be copied into **both `bin/` and `libexec/gcc/<target>/8.3.0/`**. With only `bin/` populated, `cc1` is a subprocess that goes through the libexec directory and you get **exit 127 / 0xC000007B / `cc1` failing silently** (the driver prints its version but a real compile produces nothing).
- LoongArch32 uses a separate toolchain: `D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-gcc.exe` (also GCC 8.3.0), with `-march=loongarch32 -mabi=ilp32d`. It has **no runnable QEMU target** (see §6) and is used for build/link verification only.

## 3. RISC-V RV32 port (implemented, QEMU-executed)

### 3.1 Directory layout

```
kernelsrc/port/riscv/rv32imac/
    arkrtos_port.c          port implementation (interrupts, timers, atomics, stack init)
    arkrtos_context.S       context switch and trap entry
board/qemu_riscv32/
    arkrtos_board.c         16550 UART console + device init hooks
    arkrtos_board_config.h  board switches (including CHIP_* physical parameter overrides)
example/qemu_riscv32/
    main.c                  self-test program (A/B tasks + SVC path check)
    start.S                 reset entry, BSS clear, jump to main
    link.ld.in              linker script template
    gen_ldconsts.py         evaluates the partition header into a pure-numeric header
    libc_shim.c             minimal libc (printf family + memory functions)
    Makefile
```

### 3.2 Execution mode

| Item | Value | Note |
|---|---|---|
| Privilege | single **M mode** | `USE_MPU=0`, `USE_PRIV=0` |
| harts | 1 | `USE_SMP=0` |
| FPU | off | `USE_FPU=0` |
| Syscall number | **a7** | `ARKRTOS_ARCH_SVC_NUM_BITS = -1` (`ecall` has no immediate field) |

### 3.3 Trap frame: 144 bytes, 4-byte slots

`RV_FR_A0=9`, `RV_FR_A7=16`, `RV_FR_MEPC=31`, `RV_FR_MSTATUS=32`, `RV_FR_WORDS=36`. `mepc += 4` is done by the port, not by the trap entry. `mstatus` is set to `0x1880`.

### 3.4 Task switch and timer

- `start_timer = us * 10` (timebase 10 MHz).
- Interrupt-enable order matters: the CLINT `mtimecmp` reset value is 0, so enabling `MIE` before programming the comparator enters a trap immediately.
- RAM base `0x80000000`, CLINT `0x02000000`, 16550 UART at `0x10000000`.

### 3.5 Addresses and linking

- Six `CHIP_*` overrides in `board/qemu_riscv32/arkrtos_board_config.h`.
- The linker script cannot include the partition header, so `gen_ldconsts.py` evaluates it and generates `ldconsts.h` with plain numeric constants (semantics shared with the ARM scatter generation rule: addresses are defined once, in the partition header).
- Link line has no `-lgcc` and no libc.

### 3.6 Three build-time pitfalls

1. `-include arkrtos_board_config.h` must be forced into the compile so port sources see board macros.
2. `-Ikernelsrc/components/mdk_trace` is needed once trace support is vendored.
3. LittleFS trimming macros must be defined, and `-MMD -MP` must be used.

Two MinGW `make` quirks: it does not accept `/d/...` style paths, and a recipe ending without a shell metacharacter may be executed without a shell — appending `;` forces the shell path.

### 3.7 libc shim

`libc_shim.c` provides the printf family and the memory functions. It also has to provide libgcc intrinsics (`__popcountsi2`, `__bswapsi2`, …) and `strspn` / `strcspn` — see §6.8.3 for why this only surfaces when the feature set is really switched on.

### 3.8 Startup order and SVC

`start.S` clears `.bss`, runs the constructor table, then jumps to `main`. SVC numbers go through `a7`; the user-side path is verified by `svc_ok` printing all-green.

### 3.9 Measured results

```
text 46544 / data 48 / bss 22128        # with U mode + PMP + separate fault stack
text 42816 / data 48 / bss 15968        # single-M-mode build
```

Three readings from the serial output: 1000 ticks ↔ 500 ms, scheduling alternates correctly, and the SVC path prints `svc_ok` all green.

### 3.10 U mode + PMP

Four negative controls, all measured:

| Reading | Meaning |
|---|---|
| `mcause=5 mtval=0x02000000 mepc=0x8000a248 from=U task=3` | U-mode load from CLINT refused |
| printing must still succeed | the refusal is contained, the kernel keeps running |
| `ecall_u` increments 1→5 while `ecall_m` stays 1 | the U-mode ecall really arrives at the M-mode handler |
| `mcause=2 mtval=0x30046073 mepc=0x8000a21e` | U-mode privileged instruction (`mstatus` write) is illegal |

Three defects that were only found by executing:

1. **Really enabling `MIE` inside the trap handler allows nesting**, and a task silently ends up running in M mode. Located with `g_rv_ecall_u/m` counters; QEMU `-d int` identified `m_timer epc=0x800096b0`.
2. **The fault-restart path refilled the task stack**, producing a `0xcdcdcdcc` recursion: 8 frames per round, each 0xF0 = 240 B lower. Fixed with `rv_fault_enter()` plus a 256-word fault stack.
3. **U-mode privileged instructions surface as `mcause=2`**, not as a load/store fault.

Two constraints on example tasks: `arkrtos_task_delay` busy-waits and the tick only advances at ~5 Hz (18 ticks in 8 s), so real waits must use `arkrtos_task_wait`.

§3.10.4 states the boundary: **what is isolated is the privilege level, not tasks from each other.**

### 3.11 Interrupt-driven UART via PLIC

- PLIC base `0x0C000000`; enable bitmap at `+0x2000`; threshold at `+0x200000`; claim/complete at `+0x200004`.
- Claim limit is 8; no context switch inside the handler; 16550 `IER = RDA|RLS`; a `guard < 64` bound prevents an unbounded loop.
- Reading: `[R] wake=1 n=6 isr=2 rx=6 drop=0 -> hello.`
- End-to-end script `uart_irq_check.py` uses a TCP chardev with `wait=on`. Two constraints: `-kernel` must not contain a non-ASCII absolute path, and `[R]` is matched as a substring.

### 3.12 Full-feature build

- Reading `written=1024 rejected=1 erases=2`, `[FS] PASS`.
- Size `text 171296 / data 112 / bss 582672`.
- §3.12.1 random M-mode faults, root cause: `g_fs.stream.cfg` dangled. `lfs_file_opencfg` borrows the pointer and does not copy it, so `attr_count` retained `0xffffffac` (-84) from the stack; the unmasked `LFS_MKTAG` carried bit31, was decoded as a from-disk tag and dereferenced NULL, giving `mtval=0x4`. Fixed by giving LittleFS its own `g_fs.stream_cfg`. Verification went from 5 FAIL in 8 runs to 20/20 PASS.

### 3.13 Stack usage, U-12 / U-14

Soft-timer stack was 96 words = 384 B, measured 372 B (**96%** by the fill method), raised to 256 words = 1 KB (36%). `utest` stayed 13/13. Image `text 235828 / data 128 / bss 628228`, +640 B.

Same-method numbers on other ports: RV32 1024 B / 372 B = 36%, shell 3072 B / 2172 B = 70%; LA64 4096 B / 2172 B = 53%, shell 8192 B / 2700 B = 33%. **U-14 cannot be read across to RV32.**

## 4. Known limitations

Seventeen rows: no dedicated stack for ordinary traps; traps are not nestable; no task-to-task isolation; `mcause=2` halts on the M side; `mepc += 4`; no FPU; single hart; QEMU does not model peripherals; no IPI; PLV3 relies on static TLB entries; App pages are whole-page RWX; the low 16 MB mapping only holds on virt; serial receive is interrupt-driven on RV32 only; no generic external-interrupt registration API; LA32 has no execution carrier.

## 5. LoongArch64 port (implemented, QEMU-executed)

### 5.1 Directory layout

```
kernelsrc/port/loongarch/la64/arkrtos_port.c
kernelsrc/port/loongarch/la64/arkrtos_context.S
board/qemu_loongarch64/arkrtos_board.{c,h}
board/qemu_loongarch64/arkrtos_board_config.h
example/qemu_loongarch64/{main.c,start.S,link.ld.in,gen_ldconsts.py,libc_shim.c,Makefile}
```

### 5.2 Execution mode

Single PLV0 for kernel tasks, PLV3 for Apps; one hart; FPU off; SVC number in `a7`. **IPI is useless on this machine** (`ESTAT.IS` written directly is not delivered), so the switch is done in place.

### 5.3 Trap frame: 256 bytes, 8-byte slots

| Offset | Content | Offset | Content |
|---|---|---|---|
| 0 | ra | 88–152 | t0–t8 |
| 8 | tp | 160 | fp |
| 16 | original sp | 168 | s0 |
| 24–80 | a0–a7 | 176–232 | s1–s8 |
| 240 | ERA | 248 | PRMD |

`$r21` is not in the frame (psABI-reserved, usable as scratch — see §5.23).

### 5.4 PRMD bit order (a real trap)

`PPLV = bits[1:0]`, `PIE = bit2`, `PWE = bit3`; the initial value is `0x4` (return to PLV0 with interrupts on). Reading it as `0x1` hung the PLV1 transition with PC `0x20b824` and `Ecode=0x48` (ADEM). Fix: `andi $t0, $t0, 7`.

### 5.5 Addresses, UART, timers

| Item | Value |
|---|---|
| RAM base / link base | 0 / `0x200000` |
| UART (ns16550a) | `0x1FE001E0`, register shift 0 |
| `rdtime` | 100 MHz |
| Timer | TCFG 25 MHz, `TCFG = ((initval & 0x3FFFFFFF) << 2) | 0x3` |

CSR numbers are listed in the normative page. `-bios none` cannot be used, and `-serial file:` accepts relative paths only.

### 5.6 Build constraints

- Must be invoked from the repository root (`make -f example/qemu_loongarch64/Makefile`).
- Single multilib `lp64d`.
- Exclude `arkrtos_flashapp.c` and `arkrtos_vfs_lfs.c` in the minimal configuration.
- `gen_ldconsts.py` needs a trailing semicolon in its generated `#define`s.

### 5.7 Two LP64 silent wrong answers

1. `uint32` became 64-bit through a conditional typedef.
2. Frame slots must be `la_frame_word` = `unsigned long`; using `uint32` silently truncates saved registers.

### 5.8 Measured results

```
text 54536 / data 80 / bss 18032
```

### 5.9 Monitor over telnet

`-monitor telnet:127.0.0.1:4567,server,nowait`, with an ANSI/backspace cleanup regex on the captured text.

### 5.10 Software full stack

- Switches: `BLK / FS / VFS / VFS_LFS / PROCFS = 1`, `MPU / PRIV = 0`.
- Four project files; readings `mounts=4`, `overwrite-without-erase rc=4294967295 rejected=1`, `[FS] PASS`.
- Size `text 131656 / data 160 / bss 558096` (512 KB of that is the RAM block medium).
- Stack high-water marks: FS 2040/4096 = 49.8%, A 568/2048, B 552/2048, idle 1020/1024.
- **Bug A**: `$t0` was stored at `sp+256` instead of `sp+88`.
- **Bug B**: adjacent `u8` fields merged into an unaligned `st.w` under LP64. Fix: `-mstrict-align` **and a full rebuild**.
- §5.10.4 intermittent ADEM: `255` was used as a task number. `ready_top()` returned `0xFF` ⇒ 256 ⇒ `task_table[255]` ⇒ `0xFFFFFFFF` ⇒ ADEM. Three layers of containment: `ready_top` clears the bit and re-reads in place, a task-number range check, and an assertion that `next_sp` lies in `[0x300000, 0xB00000)` and is 16-byte aligned. After the fix, 90 consecutive runs produced 0 FATAL, but the guard counters were all 0 — so "no longer reproduces" is not "cannot happen again".

### 5.11 Shell, log, utest

- Polled serial, no reset, 10 criteria.
- **Erratum 2026-11-16**: the `utest` criterion used to be a false green (`0 matched, 0 passed, 0 failed`).
- **Erratum 2026-11-18**: the `stack` criterion was hard-coded at `5 task(s)`. The example later registered ELF/MOD demo tasks, the count became 9, and the script had been failing for a reason unrelated to the object under test. Replaced by three criteria that do not grow with the example: count >= 5, `stack` count == `ps` PID rows (two independent views disagreeing means one of them is inventing numbers), plus `0 unmeasured` and `0 at/over 90%`. Current reading `9 task(s)`, script PASS again.
- §5.11.3, the third silent wrong answer: enabling interrupts inside the exception path left `s_in_trap` permanently off by 1, so `d_tick=0` while SVC lagged 500 ms. Fix: `enable_irq` returns immediately when `s_in_trap != 0`. Before the fix, 6 of 14 runs showed it; after, 0 of 18.

### 5.12 PLV3 + static TLB

Five entries:

| # | Entry |
|---|---|
| 1–2 | two PS=22 kernel large pages |
| 3 | one 4 KB UART STLB entry, set `0x00` |
| 4 | one PS=20 App page `0x02000000 -> 0x00300000`, PLV3 |
| 5 | one 4 KB App UART alias `0x02202000 -> 0x1FE00000`, set `0x01` |

- `LOONGARCH_STLB 2048 / MTLB 64 / TLB_MAX 2112`, so MTLB indices are 2048..2111.
- Large pages must use `tlbwr` (`tlbfill` picks a random way); 4 KB entries use `tlbfill`.
- `STLBPS` reset value is 0 — `LA_CSRW(LA_CSR_STLBPS, 12UL)` is required.
- `.appprobe` has four disciplines; `la_app_magic = 0x4C413634`.

Three negative controls (all `plv=3`): `ecode=7 PPI badv=0x00300000`, TLB refill `badv=0x05000000`, `ecode=14 IPE badv=0x020012b0`. Positive control `svc_ok`.

Readings: `utest: 9 matched, 9 passed, 0 failed, 0 skipped; assertions 90 run / 0 failed`.

§5.12.9 explains why the test uses `SUB_WAIT` instead of `SUB_DELAY`: busy-waiting for 500 ms advanced the host clock by only 37 ms in 20 s. §5.12.10 lists six things not done.

### 5.13 Three real defects and one unresolved issue

1. `delay_us` threshold was off by 1000× — a 500 ms wait became 42.9 s.
2. The PI test case stack was too small — `era=0x300`.
3. `utest` false green. All three targets got `KEEP(.*.ctors)` and the criterion was tightened to `N matched, N passed, 0 failed` with `N>=1`. After the fix: 9/9, then 10/10, `assertions 93 run / 0 failed`.

§5.13.4, unresolved: a 1-in-12 intermittent `ecode=0x0d era=0 badv=0x0` (stack fully unwound ⇒ an entry function returned). The switch log's six fields showed every non-idle target at `st=0x1`; `heal=6`. After adding `kill_ret` diagnostics, 48 sessions produced 0 reproductions. Later closure: that diagnostic is a false positive under `in_isr`, so an `arkrtos_port_in_isr() == 0u` gate was added, `text +12 B`, RV32 image `2012876 -> 2012924 B`. **The root cause is still undetermined.**

### 5.14 Soft-timer stack could not hold a trap frame

96 words = 384 B, `peak/low=0x00304C60`, 48 B left. A 256-byte frame written to `sp-256` put its 18th word on top of `g_hwsafe_dev_n`, so `devs=2210696` and the kernel touched unmapped `0x01000000` (`KERNEL refill`). Blowing the stack up 8× did not crash. Fix: board-level `ARKRTOS_TIMER_TASK_STACK_WORDS (512)`, 256 on LA32. After the fix, 10 `exec` runs had zero FATAL, `hwsafe: enabled, devs=0 owners=2 neutralized=8 last_owner=7`. RV32 still uses 96 words with 200 B left and only ~72 B in the handling chain, registered as U-12 and not fixed.

### 5.15 Stack bottom guard, U-13

`ARKRTOS_USE_STACK_GUARD` defaults to n; it is different from `STACK_CHECK`. It checks `stack_bottom[0] == 0xed01`, has two landing points and two flag bits, and is zero-symbol / zero-size when off.

Measured: `[STK] task 4: stack bottom flag overwritten`, plus `fault` table row `0 STACKOVF`. Repro: `python tools/qemu_shell_drive.py --stack-guard-check --boot 45 --port 45571`.

§5.15.1: the guard caught the shell's 3 KB stack failing to hold `ld` (empty 2452/3072 = 79%, after `ld` 3072 = 100%). Fix: board hook `ARKRTOS_SHELL_STACK_SIZE` = `1024*8`, after which 3956/8192 = 48%. It also exposed that `--modld-check` expected `argv[0]=modhello.o`, which had always been a **fake reading produced by corrupting memory**; the true value is the full path.

### 5.16 Runtime dynamic mapping: drivers registering MMIO spans (debt #5, 2026-11-17)

Before this round `arkrtos_port_paging_alias()` was a **placeholder that always returned -1**. This round made it real and produced QEMU readings.

#### 5.16.1 Interface and contract

```c
int32  arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrtos_mpu_mem_t mem);
uint32 arkrtos_paging_map_pages(void);
```

- `map_span()` registers `[va, va+size)` 4 KB at a time onto `[pa, pa+size)`. Returning 0 means every page went into the TLB **and** into the audit span table.
- Only **4 KB granularity**: any of `va`/`pa`/`size` misaligned, `size==0`, or `size > 16` pages (`ARKRTOS_PAGING_MAP_MAX_PAGES`) returns -1.
- **Kernel-visible (PLV0) mappings only.** App-visible dynamic mappings need ASID or a per-process table, which is U-1; approximating it here would be a "seemingly authoritative wrong answer", so it is refused explicitly.
- **No rollback on failure**: pages already registered stay registered (no re-map, no rollback). Callers must treat the call as "all or partial success"; the return value only says where it broke.
- Rejection codes are passed through unchanged, not flattened to -1:

| Code | Meaning |
|---|---|
| -1 | bad parameter/attribute (misaligned, illegal size, `mem` not one of RAM/ROM/PERIPH_RW/PERIPH_RO) |
| -2 | overlaps a static span already in `s_pg_spans[]` |
| -3 | dynamic table full (`LA_PG_DYN_PAIRS` = 8) |
| -5 | same VA already registered to a **different** PA or attribute; an exact duplicate is idempotent and returns 0 |

- `arkrtos_paging_map_pages()` returns the cumulative page count (monotonic, because there is no undo).

#### 5.16.2 Root cause: `tlbfill` picks a random STLB way and evicts the kernel UART entry

The first implementation reused `la_tlb_fill()`, which goes through `tlbfill`. On LoongArch, `tlbfill` with `PS == STLBPS(12)` **picks one of the 8 STLB ways at random**. The kernel UART static entry (VA `0x1FE00000`, set `(VA >> 13) & 0xFF = 0`) shares a set with the test VA `0x03000000`, so registration succeeded and the very next console print died:

```text
KERNEL refill (plv0) badv=0x1fe001e5 -> fatal
```

(`0x1fe001e5` is the UART LSR address; reading it triggered a refill because the entry was gone.) This is not intermittent — repeatedly writing 4 KB entries at runtime without being able to choose a way **necessarily** evicts same-set static entries.

#### 5.16.3 Fix: deterministic `tlbwr` into MTLB

New `la_tlb_write(vaddr, ps, index, lo0, lo1)` goes entirely through `tlbwr` (caller supplies the index, so behaviour is deterministic). Dynamic 4 KB entries are **deliberately placed in MTLB** (absolute index `LA_MTLB_INDEX + 3 + slot`, i.e. from 2048) rather than STLB:

- the MTLB index is deterministic and `tlbwr` writes exactly that entry;
- MTLB lookups compare VPPN against the entry's own PS and do not compete for the STLB set budget;
- the static large pages already occupy 2048..2050, so dynamics start at `2048 + 3`.

The cost is that MTLB has only 64 entries, which pins the dynamic table: `LA_PG_DYN_PAIRS` = **8 pairs** (a pair = even page + odd page, since one TLB entry covers LO0 and `LO0 + 2^PS`).

**The unregistered half must be written with V=0**; never "just open up" the neighbour page.

After the fix: `pmap: compare 16 words, mismatch=0`, `pmap test: PASS`, and the console no longer dies.

#### 5.16.4 Audit side: `check()` also honours dynamic entries

`arkrtos_port_paging_check()` gained a dynamic scan: the whole span must fall **inside one page** and that page's permission mask must cover `mem`. **RW covers RO** (a page registered `rw` checks 0 as `ro` and -1 as `ram`).

#### 5.16.5 `pmap` subcommands and measured transcript

```text
pmap                                      readings (init_rc / checked / bad / bad_base / pages)
pmap map  <va> <pa> <size> <ram|rom|ro|rw>
pmap check <va> <size> <ram|rom|ro|rw>    audit probe (cross-page / unmapped both return -1)
pmap rd   <va> [count]                    read through the mapping (one word per line, max 8)
pmap rb   <va>                            read one byte through the mapping
pmap wrb  <va> <byte>                     write one byte through the mapping (MMIO only)
pmap test                                 self-test: map a kernel ROM page and compare 16 words
```

`rd/rb/wrb` **do no protective checks** — an unmapped address is a fatal refill. That is deliberate: they verify "does it work", not "should it".

QEMU LA64, executed:

```text
$ pmap
pmap: init_rc=0 checked=0 bad=0 bad_base=0x00000000 pages=0

$ pmap test
pmap: map ROM 0x03000000 <- 0x00200000 rc=0
pmap: compare 16 words, mismatch=0
pmap test: PASS

$ pmap map 0x03000000 0x00300000 0x1000 ram      # same VA, different PA
pmap: map 0x03000000 <- 0x00300000 size=4096 rc=-5 pages=1

$ pmap map 0x02000000 0x00300000 0x1000 ram      # inside the App static span
pmap: map 0x02000000 <- 0x00300000 size=4096 rc=-2 pages=1

$ pmap map 0x03002000 0x1FE00000 0x1000 rw       # real device page: UART
pmap: map 0x03002000 <- 0x1FE00000 size=4096 rc=0 pages=2

$ pmap rb 0x030021E5      -> pmap: [0x030021E5] = 0x60     # UART LSR via the new VA
$ pmap rb 0x1FE001E5      -> pmap: [0x1FE001E5] = 0x60     # direct, same value
$ pmap wrb 0x030021E0 0x5A -> 0x5A written to UART THR; a real 'Z' came out of the serial line

$ pmap check 0x03002000 0x1000 ro  -> rc=0      # RW covers RO
$ pmap check 0x03002000 0x1000 ram -> rc=-1     # attribute mismatch
$ pmap check 0x03003000 0x1000 rw  -> rc=-1     # unmapped

$ pmap map 0x03004000 0x00300000 0x2000 ram -> rc=0 pages=3   # two pages in one call
$ pmap test (second time) -> still PASS (idempotent), page count keeps growing
```

Reading notes:

1. **`-5` was a real defect found by execution** — the first version had no "same VA, different PA" check, so `pmap map 0x03002000 0x00300000 ...` returned rc=0 and **really changed the translation**; a later `wrb` then hit kernel RAM instead of the UART. Only after adding the `pa[2]` comparison did -5 exist.
2. **Both UART read paths return the same value** (`0x60`), proving the dynamic entry really translates rather than "happening to map the same memory".
3. **`wrb` printing 'Z'** proves the write direction works too (not a read-only copy).

Regression in the same change:

```text
$ python tools/qemu_shell_drive.py --port <p> --modld-check
SHELL-DRIVE OK: all 7 commands matched

$ python tools/qemu_shell_drive.py --port <p> --stack-guard-check
SHELL-DRIVE OK: 1 command (0 173 4 STACKOVF)

$ python tools/paging_gate.py
PAGING-GATE OK: default build = 0 / LA64 build compiles (2 TU) / RV32 negative refused /
two backends mutually exclusive refused / dynamic-mapping wiring pins E-H hold and self-checks are effective
```

#### 5.16.6 Gate: pins E–H, each with a self-check

`tools/paging_gate.py` gained four **source pins**, each verified by corrupting the source string once and confirming the pin then fails:

| pin | Assertion |
|---|---|
| E | `arkrtos_paging.c` defines `arkrtos_paging_map_span(` and its body calls `arkrtos_port_paging_alias(` |
| F | the port has `static void la_tlb_write(`; the alias body contains `la_tlb_write(` and does **not** contain `(void)va;` (placeholder residue) |
| G | `#else` fallbacks: `arkrtos_hal.h` alias returns -1, `arkrtos_mpu.h` `map_span` returns -1 |
| H | the shell registers `pmap`, `#if (ARKRTOS_USE_PAGING == 1)` appears before the registration point, and `need` contains `(ARKRTOS_USE_PAGING ? 1 : 0)` |

Pin F's self-check was **vacuous** in its first version: the mutation string `la_tlb_write(pair, 12u` occurs twice in the alias body and `replace(..., 1)` only corrupted the idempotent branch, so the pin still passed. Replacing every occurrence fixed it.

#### 5.16.7 Explicitly not done

- **Revoking / reclaiming mappings**: this round completed it (U-15, §5.18). The concern that "issuing `invtlb` by VA would take out kernel entries too" was real, so the implementation is "invalidate by VA, then write a V=0 entry back at the MTLB absolute index", and static spans are always refused (-2). `arkrtos_port_paging_flush()` (whole-table flush) is still a no-op.
- **App-visible dynamic mappings**: belongs to U-1 (per-process tables); the interface always refuses.

### 5.17 Per-App address space: one VA, two physical frames (U-1, 2026-11-17)

#### 5.17.1 This port has no hardware page-table walk, so "per-process table" = one entry per process

The QEMU LA64 board runs with `cpu->ptw = OFF`: translation comes only from the TLB. So U-1 here can only mean **one App-window entry per address space, rewritten on switch**, not "a traversable page table per process".

- Space number is the frame number: `space n -> PA = ARKRTOS_QEMU_APP_BASE + n * ARKRTOS_QEMU_APP_SIZE` (2 MB stride; space 0 = `0x02000000`, space 1 = `0x02200000`).
- The VA is identical in every space (`0x02000000`), so an App **cannot even name** another App's frame — not "access denied", the address simply does not exist.
- Frames come from a fixed board array (`ARKRTOS_PAGING_APPS_MAX`), **default 1**: in that case `activate()` returns immediately when `space == s_pg_app_space` and the scheduling path is byte-identical to before.
- Kernel tasks (`is_priv != 0`) do not switch space; they run on the kernel identity map and must call `arkrtos_paging_space_activate()` explicitly to write a frame. The switch happens inside `arkrtos_mpu_set_app()`, and App tasks have `is_priv == 0`, so only Apps carry a space.
- The ELF loader lands in **whatever space is currently installed**: after writing the segments it binds the new task to that same space number (`arkrtos_paging_bind_task()`), otherwise you get "wrote frame A, ran frame B".

| Layer | Interface | Note |
|---|---|---|
| port | `arkrtos_port_paging_activate(space)` | 0 = done/already; -1 out of range; **-2 = write-then-readback mismatch** |
| port | `arkrtos_port_paging_space()` / `space_frame(n)` / `space_count()` | current space / frame base / board-provided count |
| port | `arkrtos_port_paging_lookup(va, &idx, &ppn, &ps)` | read-only diagnostic: which entry translates this VA |
| paging | `arkrtos_paging_space_count/space_frame/space_current/space_activate` | backend decoupled from port; window build always reports 1 |
| paging | `arkrtos_paging_bind_task(task, space)` / `space_of(task)` | binding stores only the number; the switch happens at the next schedule |
| shell | `pmap space [n]` / `pmap look <va>` / `pmap u1` | read, switch, inspect entry, run self-test |

#### 5.17.2 Root cause: QEMU's `tlbwr` does not flush the **odd page** of a large page

The first implementation rewrote entry 2050 (PS=20) and `pmap u1` failed outright:

```
pmap u1: va=0x02100000 space0=0xA5A50002 space1=0xA5A50002
pmap u1: FAIL (1 of 2 reads wrong)
```

A value written in `space 1` was readable in `space 0`, yet `pmap space 1` reported `rc=0` — the state said "switched", the translation did not change. The root cause is in QEMU, not here: in `target/loongarch/tcg/tlb_helper.c`, `invalidate_tlb_entry()` computes the even-page address with `& ~mask`, but the odd page with `& pagesize` (should be `| pagesize`):

```c
pagesize = MAKE_64BIT_MASK(tlb_ps, 1);            /* ps=20 -> 0x1FFFFE */
mask = MAKE_64BIT_MASK(0, tlb_ps + 1);            /*      -> 0x1FFFFF */
if (tlb_v0) { addr = (tlb_vppn << 13) & ~mask; ... }   /* 0x02000000: correct */
if (tlb_v1) { addr = (tlb_vppn << 13) & pagesize; ...} /* 0x02000000 & 0x1FFFFE = 0: wrong */
```

So the upper 1 MB (odd page) of a PS=20 entry never gets its softmmu translation invalidated: `tlbwr` changes the TLB entry to frame 1, but instruction fetch / loads / stores go through QEMU's translation cache, still pointing at frame 0. `pmap u1` puts its probe at `base + 1 MB` (middle of the odd page), so every read was wrong.

Fix: issue `invtlb` op=3 (PAGEASID) by VA before switching:

```c
static void la_tlb_inval_page(uint32 va)
{
    __asm__ volatile("invtlb 3, $zero, %0" :: "r"((unsigned long)va) : "memory");
}
```

`helper_invtlb_page_asid()` ends with a **whole-table** `tlb_flush()`, so the cache is cleared as well. The order must be "invalidate, then `tlbwr`", because `invtlb` matches VA against the **old** entry's PS. The cost is only at the switch instant (`APPS_MAX == 1` never reaches this path).

#### 5.17.3 `pmap look`: read-back diagnostics, and `tlbsrch` not writing `TLBELO`

`arkrtos_port_paging_lookup()` uses `tlbsrch` + `tlbrd`. The first version only did `tlbsrch` and read `CSR_TLBELO0` as the PPN — that is a **stale value left by the previous `tlbwr`**: `tlbsrch` only writes `TLBIDX` (INDEX/NE); the entry's VPPN/PS/LO0 must be read back with `tlbrd` at `TLBIDX.INDEX`. Without that step `activate()`'s read-back check "always matches" — a textbook false green. Current form:

```
pmap look: va=0x02000000 entry=2050 ps=20 ppn=0x02000000
pmap: space 1 rc=0 (current=1)
pmap look: va=0x02100000 entry=2050 ps=20 ppn=0x02200000
```

(Entry 2050 = MTLB absolute `LA_MTLB_INDEX + 2`; `tlbrd` also writes `CSR.ASID` from the entry, harmless here because ASID is always 0 in this build.)

#### 5.17.4 Readings

```
$ python tools/qemu_shell_drive.py --boot 50 --port <p> --cmd \
    "pmap space;pmap look 0x02000000;pmap u1;pmap test;exec /mnt/ram/hello a b c"

pmap: spaces=2 current=0
  space 0 -> frame 0x02000000
  space 1 -> frame 0x02200000
pmap look: va=0x02000000 entry=2050 ps=20 ppn=0x02000000
pmap u1: va=0x02100000 space0=0xA5A50001 space1=0xA5A50002
pmap u1: frame0=0x02000000 frame1=0x02200000
pmap u1: rc0=0 rc1=0
pmap u1: look0 entry=2050 ps=20 ppn=0x02000000
pmap u1: look1 entry=2050 ps=20 ppn=0x02200000
pmap u1: PASS - same VA, two frames
pmap: compare 16 words, mismatch=0
pmap test: PASS
[hello] argc=4
[hello] argv[1]=a
[ELF] /mnt/ram/hello: finished after 0 ms
```

Also verified: `exec` inside `space 1` loads and finishes in frame 1 (`argv[1]=z` as usual), proving the load point and the running space are bound together.

#### 5.17.5 Gate: pins I–M

| pin | Assertion |
|---|---|
| I | the port has `la_tlb_inval_page()` containing `invtlb 3`; `activate()` calls `la_tlb_inval_page(` **before** `la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u` |
| J | `set_app()` calls `arkrtos_port_paging_switch_to(` and **does not** contain `arkrtos_port_paging_activate(` (the latter is the U-2 crash); `arkrtos_hal.h` off-switch fallback `lookup` returns 0; `arkrtos_mpu.h` declares `arkrtos_paging_space_activate(` |
| K | the port defines `arkrtos_port_paging_switch_to()` branching on `la_sp_in_app_window()` and only recording `s_pg_pending_space` inside the window; `la_switch_prepare()` clears pending, honours `new_frame == old_frame`, patches `LA_FR_PRMD`, and really calls `activate(space)` |
| L | `.S` contains `bl la_switch_prepare` and `la.abs $sp, la_switch_stack_top` twice each (the `eentry` and `switch_inline` restore paths), the stack swap precedes the first `bl`, and `.bss` holds `la_switch_stack_top` with `.space 2048` |
| M | `arkrtos_elf.c` has `elf_slot_live()` / `elf_copy_to_window()`, `exec_space()` calls `arkrtos_paging_space_activate(space)`, `arkrtos_paging_space_activate(prev)` and `elf_slot_live(space)`; the shell has the `pmap u2` PASS string |

Pin I is the guard for this exact pitfall: without `invtlb` the switch silently fails on QEMU while every state reading still says "success".

#### 5.17.6 Two programs running concurrently on the same VA (U-2, done 2026-11-17)

Loader (`kernelsrc/src/arkrtos_elf.c`):

- `s_elf_task[ARKRTOS_PAGING_APPS_MAX]` / `s_elf_info[]` / `elf_slot_live()` — the ledger goes from "one instance" to "one per space"; `exec_space()` picks the first free space and returns `ERR_BUSY` only when all are taken.
- Segments are no longer `memcpy`'d straight to the window pointer: with paging on they are first read into kernel staging `s_elf_stage[4096]`, then `arkrtos_paging_space_activate(space)` is called and the bytes are copied 4 KB at a time (`elf_copy_to_window()`). Reason: during load the kernel runs on the identity map, and the window VA belongs to the target App; a direct write relies on the implicit premise "the window happens to point at it".
- The current space is saved and restored around the load (`arkrtos_paging_space_current()`), including on failure, otherwise every later shell memory command reads someone else's frame.

Scheduling path (the real trap):

```
arkrtos_sched_activate() -> arkrtos_mpu_set_app() -> rewrite the App window MTLB entry
```

But the LA64 trap frame (`eentry`) and the inline switch frame (`arkrtos_task_switch`) both live on **the outgoing task's own stack**, and an App task's stack is inside its own 2 MB window VA. The moment the entry changes, that C code's return address, `$s0` frame-pointer slot and locals are read from **another App's frame** at the next `ld.d` or function return. Measured:

```
FATAL estat=0x00080000 ecode=0x00000008 era=0xcdcdcdcd badv=0xcdcdcdcd
      badi=0x002b0010 crmd=0x00000010
```

`0xcdcdcdcd` is this port's stack fill pattern, so `ertn` jumped there and ADEF halted in PLV3.

Fix (deferred window switch):

1. `arkrtos_port_paging_switch_to(space)` (contract in `arkrtos_hal.h`, off-switch macro returns -1): out of range -1; when `la_sp_in_app_window()` is true (current `sp` in `[APP_BASE, APP_BASE+APP_SIZE)`) it only records `s_pg_pending_space` and returns 0; otherwise it is `activate()`. The criterion is `sp`, not `current->is_priv`: the dangerous exact condition is "the frame we are about to restore from lives on the same VA".
2. The two restore paths in `arkrtos_context.S` (`eentry`, `arkrtos_port_switch_task_inline`), after `la_trap_c` / `la_switch_inline` returns and the target frame is available, first do `la.abs $sp, la_switch_stack_top` (a 2 KB static stack in `.bss`, identity-mapped), then `bl la_switch_prepare(new_frame, old_frame)`. After that no stack is touched; registers are restored via `$s0` only.
3. `la_switch_prepare()` (`arkrtos_port.c`): with no pending it returns; if `new_frame == old_frame` (the scheduler did not actually switch) it clears pending **without switching**, otherwise the restore would read from another App's frame; otherwise it calls `activate(space)` and then patches `new_frame[LA_FR_PRMD]` (the C-side `LA_PRIV_APPLY` ran while the window was still the old one, so it wrote into the old window's memory instead of the target frame).

The fault and refill paths need no change: they already swap `sp` to `la_fault_stack` / `la_refill_stack` before entering C, so `la_sp_in_app_window()` is false and they degrade to "take effect immediately".

Measured (`build/u2run2.log`, port 45841, `grep -c FATAL` = 0):

```
$ pmap u2
pmap u2: /mnt/ram/hello -> space 0 task 4, space 1 task 9
pmap u2: frame0=0x02000000 frame1=0x02200000
pmap u2: same VA 0x02000000 reads 0x00150064 / 0x00150064, other slot before/after 0xA5A50202 / 0x00150064
[hello] entry sp=35635200 argc=1
[hello] hello from an ELF64 program
[hello] argc=1
[hello] argv[0]=/mnt/ram/hello
[hello] bye
[hello] entry sp=35635200 argc=1
[hello] hello from an ELF64 program
[hello] argc=1
[hello] argv[0]=/mnt/ram/hello
[hello] bye
pmap u2: PASS - two instances, two spaces, neither frame overwritten
$ pmap elf
pmap elf: spaces=2 last=1
  space 0 frame=0x02000000 task=4 live=0 bytes=1732 entry=0x02000000
  space 1 frame=0x02200000 task=9 live=0 bytes=1732 entry=0x02000000
pmap elf: 0 live instance(s)
```

Criterion note: reading the **same** word (`0x00150064` = hello's first instruction, not the ELF magic — the entry is `0x02000000`) from two spaces is correct; identity is distinguished by `frame0/frame1`, `task 4/9`, and "the other slot was the `0xA5A50202` sentinel before load and got overwritten". The two `bye` lines are the evidence that both instances finished — `PASS` without two `bye` lines is not concurrency.

Regression (`build/regress_u2.log`, port 45851): `--modld-check` 7 + `--stack-guard-check` 1 + a single-instance `exec /mnt/ram/hello a b` (still `space 0`, `finished after 0 ms`) + `pmap elf`, `SHELL-DRIVE OK: all 10 commands matched`.

#### 5.17.7 Explicitly not done

- **Page-frame allocator (U-3)**: frames are a fixed board array sized by `ARKRTOS_PAGING_APPS_MAX`.
- **ASID / TLB shootdown (U-7)**: this section's image keeps ASID at 0 and switches by rewriting entries; the ASID build was done and measured the same day (§5.28), but shootdown still does not exist.
- **`copy_from_user` (U-6)**, demand paging (U-5) and App-side reverse probing are not in this round. The isolation evidence here is "same VA -> two physical memories"; privilege evidence still comes from the PLV/PLV3 probe set (see the memory-protection page).

### 5.18 Revoking dynamic mappings: unmap and slot reclaim (U-15, 2026-11-17)

#### 5.18.1 Interfaces and rejection codes

| Layer | Interface | Semantics |
|---|---|---|
| port | `int32 arkrtos_port_paging_unalias(uint32 va)` | 0 = revoked; -1 not 4 KB aligned; -2 inside a static span; -4 never registered |
| port | `uint32 arkrtos_port_paging_dyn_used(void)` | occupied dynamic slots (counts `used` flags, does not read TLB) |
| paging | `int32 arkrtos_paging_unmap_span(uint32 va, uint32 size)` | page by page; returns the first failing code; `s_pg_map_pages` decreases only for revoked pages |
| paging | `uint32 arkrtos_paging_dyn_used(void)` | delegates to the port |
| shell | `pmap unmap <va> <size>` / `pmap dyn` | command entry and reading |

Three design trade-offs, all for "rather an error than a seemingly authoritative wrong answer":

1. **Never registered => -4**, not silent success. A caller revoking a span it never registered is far more dangerous hearing "done" than "no such thing".
2. **Static spans always -2.** Revoking any of kernel image / App window / the two UART entries makes the next instruction fetch a KERNEL refill — a fault, not a "release".
3. **No rollback on partial failure** (same rule and same reason as `map_span()`), but already-revoked pages are honestly deducted from `s_pg_map_pages`.

A slot is reclaimed only when **both halves of an 8 KB pair** are empty: one MTLB entry covers a pair, so revoking one half leaves the other valid and `used` must not go to zero, or the next `map_span()` would hand the slot to a different VA pair.

#### 5.18.2 A real defect found this round: rewriting MTLB is not enough

The original design (and the old wording in §5.16.7) was "dynamic entries live at fixed MTLB indices, so overwriting the index with `tlbwr` removes the old translation, therefore revoking needs no `invtlb`". **The first half is right, the conclusion is wrong.**

Measured (first implementation, `tlbwr` only):

```text
pmap: map ROM 0x03000000 <- 0x00200000 rc=0
pmap: compare 16 words, mismatch=0
pmap: unmap 0x03000000 rc=0 left_entry=0 dyn=0
pmap: rebind 0x03000000 <- 0x00201000 rc=0 dyn=1
pmap: rebind compare 16 words, mismatch=16      <-- all 16 words wrong
pmap test: FAIL (rebind translation)
```

Root cause: once a VA has been **used**, the translation exists both in the STLB and in QEMU's softmmu cache. `tlbwr` only changes the machine TLB and never looks back at the STLB; QEMU's softmmu caches translated pages. That produces the worst combination:

- `pmap look` reports `no entry` (it queries MTLB, whose bookkeeping really is empty) — **looks successful**;
- reads through the same VA still return the old physical page — **actually not revoked**.

The fix is two steps whose order matters (`invtlb` matches against the old entry's PS, so writing it later would not find the old entry):

```c
la_tlb_inval_page(pair);            /* invtlb 3: invalidate STLB + softmmu by VA */
la_tlb_inval_page(pair + 0x1000u);  /* odd half separately (QEMU's odd-page computation is off) */
la_tlb_write(pair, 12u, ...);       /* then write the V=0 entry back into the machine TLB */
```

After the fix: `rebind compare 16 words, mismatch=0`, `pmap test: PASS`.

This is the same class as §5.17.2 (large-page switch not flushing the odd page): **written does not mean in effect; only read-back counts.**

#### 5.18.3 Readings (QEMU LA64, executed)

Positive (`pmap test` chains map / unmap / rebind / unmap into one criterion):

```text
pmap: map ROM 0x03000000 <- 0x00200000 rc=0
pmap: compare 16 words, mismatch=0
pmap: unmap 0x03000000 rc=0 left_entry=0 dyn=0
pmap: rebind 0x03000000 <- 0x00201000 rc=0 dyn=1
pmap: rebind compare 16 words, mismatch=0
pmap: final unmap rc=0 left_entry=0 dyn=0
pmap test: PASS (map / unmap / rebind / unmap)
```

The `rebind` step is the criterion for "the slot really was reclaimed": if only bookkeeping were cleared without reclaiming, rebinding the same VA to another PA would hit `-5`.

Rejection codes, each executed:

```text
pmap: unmap 0x00200000 size=4096 rc=-2 pages=0 dyn=0    # kernel image static span
pmap: unmap 0x03000123 size=4096 rc=-1 pages=0 dyn=0    # not 4 KB aligned
pmap: unmap 0x03000000 size=4096 rc=-4 pages=0 dyn=0    # never registered
pmap: unmap 0x03000000 size=6144 rc=-1 pages=0 dyn=0    # not a multiple of 4 KB
```

Negative evidence (actually touch the address after revoking — that is "revoked", not "bookkeeping zeroed"):

```text
pmap: map 0x03000000 <- 0x00200000 size=4096 rc=0 pages=1
pmap: [0x03000000] = 0x14016003
pmap: [0x03000004] = 0x03800063
pmap: unmap 0x03000000 size=4096 rc=0 pages=0 dyn=0
pmap look: va=0x03000000 no entry
pmap rd 0x03000000 1
[Ark RTOS] FATAL: estat=0x00010000 ecode=0x00000001 era=0x00225b20 badv=0x03000000 ...
```

`ecode=0x1` is a TLB refill and `badv` is exactly the revoked VA.

Regression: `utest` 13 matched / 13 passed / 0 failed (133 assertions); `pmap u1` and `pmap u2` still PASS. RV32 image identical to before: `text 235828 / data 128 / bss 628228` (with paging off, `unalias`/`dyn_used` are macro fallbacks and not one line enters the image).

#### 5.18.4 Gate: pins N / O with self-checks

| pin | Assertion |
|---|---|
| N | the port has `arkrtos_port_paging_unalias(` whose body contains `la_pg_static_hit(`, `la_tlb_inval_page(`, `la_tlb_write(`, the both-halves-empty test and `used = 0u`; `arkrtos_port_paging_dyn_used(` counts `used` flags |
| O | `arkrtos_paging_unmap_span(` exists, calls `arkrtos_port_paging_unalias(` and settles `s_pg_map_pages -= done`; `arkrtos_paging_dyn_used` delegates; two off-switch fallbacks in hal / mpu.h; the shell has `unmap` / `dyn` branches and the `pmap test` criterion chains unmap/rebind |

Pin N deliberately requires `la_tlb_inval_page(`: delete that line and the feature still **looks right** (`look` reports no entry), and only reading through the VA exposes it. This kind of check has to live in the gate, not only in the document.

#### 5.18.5 Explicitly not done

- **ASID and bulk invalidation**: ASID is still 0 in this build (ASID build in §5.28); no `flush_tlb_range` and no whole-table flush (`arkrtos_port_paging_flush()` is still a no-op).
- **Revoking App-visible dynamic mappings**: the App window goes through space switching (U-1) and its PS=20 entry is rewritten by `activate()`, never through `unalias`.
- **Physical page reclaim after revoke (U-3)**: the slot is reclaimed, the physical page is still held by the fixed board array; there is no frame allocator to return it to.
- **Invalidate-by-VA takes out the window entry too** (only discovered at U-6): `invtlb 3` also drops the PS=20 entry; the fix and evidence are in §5.19.3.

### 5.19 Controlled user-address access: copy_from_user / copy_to_user (U-6, 2026-11-17)

Before U-5, kernel paths reading user buffers relied on two verbal conventions: "the caller is definitely a user task" and "the pointer is definitely inside the window". If either fails, the kernel reads through the current translation — possibly another App's frame, more often an unmapped page giving the kernel itself a TLB refill. U-6 turns this into a primitive that can refuse.

#### 5.19.1 Interfaces and four rejection codes

```c
int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len);
int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len);
int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst, const void *usrc, uint32 len);
int32 arkrtos_copy_to_user_task(arkrtos_task_t *p_task, void *udst, const void *src, uint32 len);
int32 arkrtos_paging_user_copy(uint32 rom_start, uint32 rom_size,
                               uint32 ram_start, uint32 ram_size,
                               uint32 space, uint32 installed,
                               void *kern, const void *user, uint32 len, uint8 write);
```

Three layers: `arkrtos_paging_user_copy` is the arithmetic + byte-copy body (no task table), `*_task` fetches the four spans and the bound space from the TCB, and `copy_*_user` uses the current task. The self-test drives the body layer, which runs the same gate code as production, without having to fake a TCB on the stack (`arkrtos_paging_space_of()` subtracting pointers that are not in the table is undefined behaviour).

0 means the whole span was copied; otherwise a negative value:

| Code | Meaning |
|---|---|
| -1 | bad parameter, or `uva + len` wraps 4 GB |
| -2 | this task has no user window (kernel task / `is_priv` non-zero) |
| -3 | the whole span is not inside the allowed window (write direction only accepts the RAM window) |
| -4 | a different App's address space is installed (copying would use the wrong translation — a silent wrong answer) |

The criterion is the **whole span**, not just the first address: checking only the first byte lets a pointer crossing the window end walk into unmapped pages and the kernel refills anyway — exactly the shape this interface exists to remove. The write direction only allows the RAM window; the code window is the task's own image and kernel paths have no reason to modify it, so it is -3 rather than "a gentle warning".

The body uses a `volatile uint8` byte loop: no word-wide copy that might cross pages; performance is not the current goal. On non-paging builds (ARM / RV32) all four entries are macro fallbacks to `(-1)` — those backends have no "per-task virtual space" concept, and pretending to support it would be worse than -1; this also keeps the default build byte-identical.

#### 5.19.2 Measurements (QEMU LA64, executed)

`pmap cu` runs after `pmap test`; 11 criteria all pass:

```
pmap cu: PASS - from/to user, refusals -1/-2/-3/-4, kernel survived
```

Coverage: `from_user` on the RAM window (0 + byte check), `from_user` on the ROM window, whole span outside (-3), kernel image pointer `0x00200000` (-3), unmapped pointer `0x80000000` (-3), range wraparound `0xFFFFF000+0x2000` (-1), `to_user` into the ROM window (-3), `to_user` into the RAM window (0 + byte check), no user window (-2), kernel task via `arkrtos_copy_from_user` (-2), space mismatch (-4, needs `space_count() >= 2`). "kernel survived" means the kernel printed its conclusion normally after all those illegal pointers, with no fault.

Same-round regression: `utest` 13/13 (133 assertions), `pmap u1` / `pmap u2` / `pmap test` still PASS, `pmap dyn` 0 slots 0 pages. RV32 image `text 235828 / data 128 / bss 628228`, byte-identical to after U-15.

#### 5.19.3 A real defect found along the way: `invtlb 3` also drops the App window's PS=20 entry

The first time `pmap cu` ran after `pmap test`, the kernel died:

```
pmap look: va=0x02000000 entry=2050 ps=20 ppn=0x02000000
pmap test: PASS (map / unmap / rebind / unmap)
pmap look: va=0x02000000 no entry
[LA64U] KERNEL refill (plv0) badv=0x02100000 era=0x00225c38 -> ...
[Ark RTOS] FATAL: ... ecode=0x0
```

Root cause: QEMU 9.2.3's `invtlb 3, $zero, <va>` (invalidate by VA) drops **every** entry of every page size at that VA, including the App window's PS=20 entry at MTLB+2; and `pmap test` ends by revoking its own dynamic mapping and doing an invalidate-by-VA. More expensive is `activate()`'s short-circuit: when `s_pg_app_space == space` it returns 0 saying "already installed" even though the entry is gone — a textbook silent wrong answer.

Two-step fix (both in `kernelsrc/port/loongarch/la64/arkrtos_port.c`):

1. New `la_pg_assert_window()`: rewrite the window entry as-is from `s_pg_app_space` -> `la_pg_app_frame()` (`la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u, ...)`), called after each of the two invalidate-by-VA sites in `unalias()`. Content unchanged, cost one `tlbwr`.
2. `activate()`'s "already this space" short-circuit first calls `arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE, 0, &probed, 0)` to confirm the entry still exists and the PPN matches `la_pg_app_frame(space)` before returning 0; otherwise it rewrites as usual.

After the fix the same command chain (`pmap test` -> `look` -> `cu` -> `dyn`) is all green and `look` shows `entry=2050 ps=20 ppn=0x02000000` again. This also explains why it only appeared "after a self-test that invalidates" — booting straight into the window never sees it.

#### 5.19.4 Gate: pins P / Q with self-checks

| pin | Assertion |
|---|---|
| P | `arkrtos_paging_user_copy` has the wraparound test, `((rom_start == 0u) && (ram_start == 0u))`, `space != installed`, byte loops in both directions, `if((write != 0u) ||`; `*_task` has `p_task->is_priv != 0u`, `arkrtos_paging_space_of(p_task)`, `arkrtos_port_paging_space()`; `copy_from_user` has `arkrtos_task_get_current()`; the shell has a `pmap cu` branch, five probe strings and the PASS string; mpu.h has both fallback macros |
| Q | the port has `static void la_pg_assert_window(void)`, `unalias` calls it, `activate` contains `lookup(ARKRTOS_QEMU_APP_BASE` and `probed == la_pg_app_frame(space)` |

The self-check here has a real pitfall: the first mutation string was `... < len` -> `... < lenX`, and the pin criterion is substring containment, so `lenX` still contains `len`; the mutation did not land yet the self-check reported "pin is vacuous". Rewriting it as `(0xFFFFFFFFu - uva)` -> `(0xFFFFFFFFu - uvaMut)` broke the criterion for real. **A mutation string must destroy the asserted substring itself, not its prefix or suffix.**

#### 5.19.5 Explicitly not done

- **The SVC handlers were not converted**: the boolean checks in `arkrtos_kernel_check.h` are unchanged; `copy_*_user` only delivers the primitive plus self-tests, it does not move the whole layer onto controlled copies.
- **No fixup table**: a mid-copy failure returns a negative value; there is no "already copied N bytes" partial-success semantics.
- **No copy-on-write / user page swap-out**: everything inside the window is a fixed board frame, permanently resident.
- **No performance work**: byte-wise `volatile` copy, not `memcpy` throughput.

### 5.20 Task exit-code channel (U-8, 2026-11-17)

#### 5.20.1 What was owed

After U-6 the shell could move user addresses into the kernel in a controlled way, but `exec` still only reported "finished, took N ms, in which space". To judge a program's success a script had to grep the strings the program printed — what the program **said**, not its **conclusion**. Linux's answer is `exit_group(status)` plus `wait()`. This step does the kernel half first (exit code plus ledger), and the shell-side `wait` was added the same day (§5.20.7) — it **polls the task table**, it is not POSIX semantics: no parent/child relation, no zombie queue, and no "wait for any child". The kernel half is:

- `THREAD_CTRL` (SVC `0x1B`) gained sub-command **4 = `ARKRTOS_THREAD_SUB_EXIT_CODE`**, with `p[1]` the exit code (`int32`, negatives preserved). Behaviour equals sub-command 3 (`EXIT`) — both end the calling task; the only difference is "whether there is a readable code".
- The code lands in the **TCB** first, then the **loader** takes it during "the one observation where the task became `INVALID`" and stores it in the instance ledger; the shell's `exec` reads the ledger when printing the closing line.

#### 5.20.2 Switch and size

| Item | Content |
|---|---|
| Switch | `ARKRTOS_USE_EXIT_CODE` (`config/kconfig/Kconfig.features`; default **0** in `arkrtos_features.h`) |
| On where | only `board/qemu_loongarch64/arkrtos_board_config.h` (`#undef` + `#define ... 1`) |
| Off cost | sub-command macro, two TCB fields, the `*_internal` implementation, the SVC branch, the shell's coded print and two ELF ledger fields are all removed by `#if` => **RV32 image `text 235828 / data 128 / bss 628228`, byte-identical to after U-6** |
| On cost | 8 bytes per TCB (4-byte code + 1-byte flag + 3 bytes padding), image about +240 B (first ungated version measured `text 236068` on RV32, i.e. +240) |

`arkrtos_thread_exit_code()` is **declared and implemented in both builds**: with the switch off it degrades to `(void)code; arkrtos_thread_exit();`, so App call sites need no `#if` (a macro switch should not force users to edit sources).

#### 5.20.3 Why no callback inside the exit path

Task suicide happens **inside the scheduler's switch path** (`arkrtos_thread_exit_internal()` never returns). Calling the loader there would mean the switch path knows about the ELF ledger, holds its lock, and guarantees the outgoing stack is still usable — U-2 already stepped on "touching the window in a switch path eats someone else's frame". So the exit point writes two TCB fields and **notifies nobody**; the loader does lazy cleanup:

```
elf_slot_live(space)               # reached by load / query / exec wait loop
  └ id = s_elf_task[space]
    └ status == INVALID ?          # precisely "task dead, slot not yet reused"
        arkrtos_task_exit_code(id, &code)   # write the ledger only if a code exists
        s_elf_task[space] = 0
```

`exit_code_set` exists for exactly one reason: `exit code 0` is a **valid conclusion** and must not be confused with "never reported". When nothing was reported the shell still prints the original codeless line (`finished after N ms (space N)`) and **does not invent a 0 that looks like an exit code**.

#### 5.20.4 Measurements (QEMU LA64, executed)

The `exec` wait loop is 20 ms per round and calls `arkrtos_elf_running_in(space)` each round, which triggers `elf_slot_live`, so the code is usually collected on the first poll after the task dies. `hello.c` deliberately uses **argc as the exit code**, so the program's own printed argc and the `exec` exit code corroborate each other:

```
$ exec /mnt/ram/hello one two
[hello] entry sp=35635200 argc=3
[hello] argc=3
[hello] bye, exit code = argc
[ELF] /mnt/ram/hello: finished after 0 ms (space 0, exit code 3)

$ exec /mnt/ram/hello
[hello] argc=1
[ELF] /mnt/ram/hello: finished after 20 ms (space 0, exit code 1)
```

Other criteria in the same run: `utest: 13 matched, 13 passed`, `pmap u1` / `u2` / `test` / `cu` still PASS, `dyn slot(s) in use=0`, `SHELL-DRIVE OK: all 3 commands matched`.

> One **measurement environment** pitfall (not a kernel defect): this round `qemu_shell_drive.py` ran three times in a row and each time "connected over TCP but received not one byte in 90 s". The same image with `-serial file:` immediately showed the full banner and prompt => the kernel is fine, it was a host-side TCP chardev race. **Criterion: an empty `buf` does not mean the kernel is dead**; confirm once through a file serial before deciding whether to retry or read code.

#### 5.20.5 Gates

`tools/elf_gate.py` gained **pin F** (8 files, ring-by-ring assertions): `svc_abi.h` has the sub-4 macro -> the TCB has two fields before `is_priv` -> `arkrtos_task.c` has the implementation, the `case`, and `arkrtos_thread_exit_code_internal((int32)p[1])` -> `arkrtos_elf.h` ledger fields -> `arkrtos_elf.c` calls `arkrtos_task_exit_code(id, &code)` -> the shell has `exit code %d` -> the user API is declared -> `hello.c` uses `ARKRTOS_THREAD_SUB_EXIT_CODE` with `params[1] = (ark_u32)argc;`. Pin F self-checks (`"exit code %d"` -> `"exit code X%d"`) and reports "pin is vacuous" if the self-check fails.

> The same lesson a second time: the first mutation string was `"exit code %d"` -> `"exit code %dX"`, the criterion is substring containment, `%dX` still contains `%d`, so **the mutation did not land** (same as §5.19.4).

**Pin F2 (`wait` and main-return conversion)**: 8 assertions over two files — `cmd_wait` is defined inside `#if (ARKRTOS_USE_EXIT_CODE == 1)`, the registry has `ARK_SHELL_CMD("wait", cmd_wait` with `min_args` 3, it refuses itself (`arkrtos_current_task_id`), it has `no such task`, it tests `ARKRTOS_TASK_INVALID` for completion, it calls `arkrtos_task_exit_code`, and the two print strings `exited (exit code %d)` and `exited (no exit code reported)`; `arkrtos_app_main.c`'s `arkrtos_app_exit` calls `arkrtos_thread_exit_code(code)` when the switch is on and `arkrtos_thread_exit()` in the `#else`. F2 has two self-checks.

**Pin F2b (two more exit paths)**: the mini-program SDK (`arkrtos_mini_main.c`'s `arkrtos_mini_exit(code)`) and downloadable modules (`arkrtos_modld.c`'s `mod_task_tramp`) — the former requires `exit(code)` to call `arkrtos_thread_exit_code(code)` inside the gate, the latter requires the entry return value to be **caught** first (`rc = (int32)p_fn(...)`) and then handed over (`arkrtos_thread_exit_code_internal(rc)`). "Is there an `#if`" cannot catch "code received then dropped", so the assertions sit on the catch and the hand-over, each with a self-check.

Pin F's print-string assertion was also **narrowed** from `"exit code %d"` to `"finished after %u ms (space %u, exit code %d)"`: `cmd_wait` also contains `"exit code %d"`, so without narrowing, the "corrupt the exec site" mutation would no longer fail pin F and the self-check would silently go vacuous (hit once this round).

#### 5.20.6 Explicitly not done

- **`wait` is not POSIX `wait()`**: no parent/child relation, no zombie queue, and the code is not delivered by the kernel. `wait <task id> [timeout ms]` is a shell-side 20 ms poll of the task table that **read-only** fetches the TCB code once `status` becomes `ARKRTOS_TASK_INVALID`. So it is not "reaping a child", and it cannot "wait for any child" (no such concept); after a slot is reused the code read belongs to the new task.
- **driver_sdk is not wired** (a trade-off, not a gap): driver tasks are not "run to completion" programs, they have no `exec` / `wait` observer, so a code in the TCB would have no reader. `app_sdk` and `mini_sdk` are both wired (the C library `exit()` code is converted to `arkrtos_thread_exit_code(code)`), and downloadable modules are forwarded by the loader after `module_main` returns (`arkrtos_thread_exit_code_internal(rc)`).
- **The code is only valid until the TCB slot is reused**: if a burst of create/delete happens right after the task dies, the code is lost (the 20 ms exec poll makes this practically unreachable, but there is no mechanism-level protection).
- **Only the LA64 board enables it**: `board/qemu_loongarch64/arkrtos_board_config.h` redefines `ARKRTOS_USE_EXIT_CODE` to 1 and the command line cannot turn it off; the RV32 board config does not enable it, so the 32-bit side only has the "off build unchanged" reading, **there is no RV32 QEMU reading with the switch on**.
- **ARM side compiles only, never on hardware**: `example/stm32f427/app_sdk/HELLO_APP/GCC` compiles and links `build/hello_app.elf` both off and on (`DEFS="-DARKRTOS_USE_EXIT_CODE=1"`); the mini-program `HELLO_MINI/GCC` also compiles both ways (off 484/8/28, on 480/8/28 — smaller when on because the "ignore the code and just exit" wrapper is gone), with no hardware reading for either.

#### 5.20.7 Readings: `wait` and main-return conversion (2026-11-17)

From this round `tools/qemu_shell_drive.py --script` supports **pipe expectations** (`command | expected substring | expected substring 2`); without a pipe it only sends. This has to be recorded honestly: **`SHELL-DRIVE OK` obtained through `--script` before this round had an empty expectation set — it only proved the commands were sent and those words appeared in the echo.**

LA64 default build (`build/qemu_la64_zero`: text 367744 for the `wait` change, **367824** after wiring the mini-program and module exit-code forwarding / data 416 / bss 767508); five criteria passed on the first run:

| Command | Expected substring | Observed echo |
| --- | --- | --- |
| `exec /mnt/ram/hello one two` | `exit code 3` | `finished after 0 ms (space 0, exit code 3)`, hello on task 4 |
| `wait 4` | `exited (exit code 3)` | `task 4 exited (exit code 3)` |
| `wait 5 100` | `still running after 100 ms` | `task 5 still running after 100 ms (status WAIT)` |
| `wait 47` | `no such task 47` | `wait: no such task 47` |
| `wait 0` | `usage` | usage string |

Module path (`ld /mnt/ram/modhello.o alpha 42`): the example module's `module_main` now `return argc;`, and the loader's exit path forwards it as the task exit code, so `wait 4` -> `task 4 exited (exit code 3)` — a **non-zero** reading, so 0 and "never reported" are distinguishable in the output (the never-reported line is `exited (no exit code reported)`, measured this round before the module wiring).

The mini-program (`mini_sdk`) path only reaches compilation: its host is an ARM project and there is no board reading here, so in the table it is "wired, not on hardware" — do not read it as "mini-program exit codes verified".

**Off-build zero change** is proven on RV32 byte for byte: the current tree (with the LA64 `wait` and app_main changes present, but `ARKRTOS_USE_EXIT_CODE` 0 on RV32) builds text 235828 / data 128 / bss 628228, and `objcopy -O binary` gives sha256 `2251eb0996a0485778da8e24e82a54596e4de35024d807b3fd0a4cd2a1b807bd`; a tree unpacked separately with `git archive HEAD` (`/d/tmp_ark_head2`) built the same way yields the **same sha256**. LA64 cannot prove this because that board hard-wires the switch to 1 (§5.20.6).

### 5.21 4 KB page-frame pool (U-3, first stage, 2026-11-17)

#### 5.21.1 What was owed

U-1 gave each App its own address space, but **where frames come from** was hard-wired: `space n -> APP_BASE + n * APP_SIZE` (2 MB stride). So three things were impossible:

- space number bound to frame number, so deleting an App never returned its frame (spaces could not be reclaimed);
- 2 MB granularity is meaningless for demand paging — a fault can only fall on a 2 MB boundary, and a `PS=20` entry is either wholly valid or wholly invalid;
- the kernel had no "borrow a page" interface, so future drivers, shared memory and page cache would each need their own scheme.

This stage only adds the first layer: **a 4 KB frame pool with alloc/free/count**. It deliberately leaves the App window mapping (still one `PS=20` per space) and every existing behaviour untouched: with the switch on the window is still that same entry and the `pmap u1/u2` readings are word-for-word identical.

#### 5.21.2 Switch and geometry

| Item | Content |
|---|---|
| Switch | `ARKRTOS_USE_PAGING_FRAMES` (default **0** in `arkrtos_features.h`; `depends on ARKRTOS_USE_PAGING` in `Kconfig.features`; board `#undef/#define 1`) |
| Off | the port only has four stubs returning `-1`/`0`, **not one byte of .bss** (RV32 off-build `text 235828 / data 128 / bss 628228`, byte-identical) |
| Pool | `ARKRTOS_QEMU_FRAME_BASE = 0x04000000` (64 MB), `ARKRTOS_QEMU_FRAME_COUNT = 2048` (8 MB) |
| Bitmap | `static uint32 s_fr_bits[(COUNT + 31)/32]` = 64 words = **256 B**, plus an `s_fr_used` counter |
| Mapping | `la_mmu_init()` adds a `PS=22` identity mapping for the pool at `MTLB[2059]` (`LA_MTLB_INDEX + 11`), avoiding the eight dynamic-mapping slots (2051..2058) |
| Compile-time guard | base must be 8 MB aligned, `1 <= COUNT <= 2048`, otherwise `#error` |

Why 8 MB alignment: a `PS=22` entry covers 2^23 bytes, so VA and PA must both be aligned to 2^(22+1). Why the identity mapping is mandatory: in paging mode the kernel accesses frames by **physical address**; without that entry it could only do so during the `DA=1` window, which ends early in boot, so allocated frames would be black holes the kernel cannot touch and the self-test pattern write would fault immediately.

Why `0x04000000` and **not** `0x03000000`: the latter is `pmap test`'s dynamic-mapping target VA (`PMAP_TEST_VA`). A `PS=22` static entry and a 4 KB dynamic entry on the same VA means whichever wins depends on entry order — a random wrong answer. The first version did put the pool at `0x03000000`; `pmap test` still reported PASS (it happened to hit the dynamic entry), but two mechanisms sharing one VA is itself the defect. Changing the base is the fix, not "the test happened to pass".

#### 5.21.3 Interfaces and return codes

Four port interfaces (declared in `arkrtos_hal.h`, forwarded by `arkrtos_paging.c` to `arkrtos_paging_frame_*`, macro `(-1)`/`(0u)` when off):

| Interface | Return |
|---|---|
| `arkrtos_paging_frame_alloc(count, &base)` | `0` success (`base` = first frame PA); `-1` bad parameter (`count == 0` / over total / null `p_base`); `-2` no **contiguous** free run that long |
| `arkrtos_paging_frame_free(base, count)` | `0` returned; `-1` outside the pool / not 4 KB aligned / `count` out of range; `-3` the span **contains a frame that was not allocated** (nothing is touched) |
| `arkrtos_paging_frame_total()` | total frames (0 when off) |
| `arkrtos_paging_frame_used()` | allocated frames (0 when off) |

Three deliberate "no guessing" rules:

1. Allocation must return a **contiguous** run (a future 2 MB window load needs one block); otherwise `-2`, never "here is part of it";
2. first-fit, scanning low to high, with **no defragmentation** — moving frames someone is using is not something an allocator should do quietly;
3. free validates everything before changing anything: if any frame in the span was already free the whole call is refused (`-3`). Otherwise one frame would be considered owned by two holders, and that bug only shows up much later.

#### 5.21.4 QEMU measurements (2026-11-17)

```
$ pmap frame
pmap: frames total=2048 used=0 free=2048

$ pmap ftest
pmap: frame alloc 1 rc=0 base=0x04000000
pmap: frame alloc 3 rc=0 base=0x04001000
pmap: frame free 1 rc=0
pmap: frame double-free rc=-3 (want -3)
pmap: frame free outside rc=-1 (want -1)
pmap: frame free 3 rc=0
pmap ftest: PASS (alloc/free/double-free/pattern, used back to 0)
```

`pmap ftest` criteria in order: a 1-frame and a 3-frame contiguous allocation both succeed; each of the four frames gets a 64-word pattern (`0xA5A50000 ^ (frame<<8) ^ k`) **and is read back word by word**; double free returns `-3`; free outside the pool returns `-1`; after both are returned `used` is back to its initial value. The pattern write/read-back is essential — it verifies both "the allocator accounting is right" and "the pool's `PS=22` identity mapping really works"; without it an implementation that accounts correctly but whose frames are unreachable from the kernel would also PASS.

Regression in the same boot (word for word unchanged):

```
$ pmap test      -> pmap test: PASS (map / unmap / rebind / unmap)
$ pmap u1        -> pmap u1: PASS - same VA, two frames   (entry=2050 ps=20)
$ pmap u2        -> pmap u2: PASS - two instances, two spaces, neither frame overwritten
$ utest          -> utest: 13 matched, 13 passed, 0 failed, 0 skipped
$ exec /mnt/ram/hello one two
                 -> [ELF] /mnt/ram/hello: finished after 0 ms (space 0, exit code 3)
```

#### 5.21.5 Gate

`tools/paging_gate.py` gained **pin R** (source pin with two anti-false-green self-checks): `s_fr_bits` bitmap and the `run == count` contiguous-run test; both `frame_free` refusals `-1`/`-3`; `la_tlb_fill(ARKRTOS_QEMU_FRAME_BASE, 22u, LA_MTLB_INDEX + 11u, ...)` in `la_mmu_init()`; `(-1)` fallbacks and the declaration in `arkrtos_hal.h` / `arkrtos_mpu.h`; the `arkrtos_paging.c` wrapper really forwarding; and the shell's `pmap frame` / `pmap ftest` plus the PASS string.

Two self-checks: change `run == count` to `run =X= count` (the pin must report "no contiguous-run search"); change the PASS string to `PASSX` (the pin must report "PASS string missing").

**The self-check itself hit a pitfall**: the first version changed `run == count` to `run == countX` and the pin **still passed** — because `"run == count"` is still a prefix of `"run == countX"`. A mutation string must destroy the judged substring itself, not merely append a suffix.

The `ARKRTOS_USE_PAGING_FRAMES` gate hangs on pin R in `paging_gate.py` and **adds no `ci_gate` item** (same convention as U-8).

#### 5.21.6 Explicitly not done

- **The App window is still one `PS=20` per space**: this stage is only "where frames come from", not "how the window is loaded page by page". Slicing the window into 4 KB pages needs one `PS=12` entry per page (2048 STLB entries hold 2 MB = 512 pages, 2 per set), and `activate` / `la_pg_assert_window` / the `pmap u1/u2` criteria all change with it — a separate stage.
- **No demand paging**: the "swap one in on demand" branch in `la_refill_c()` is not written.
- **Allocated frames have no owner**: this stage does not take over the App window, so nothing calls `frame_alloc`. The real users are the next stage's window load plus exit release.
- **No reclaimer check**: whoever allocates returns; the pool does not record "which space uses which frames".
- **Only the LA64 board enables it**: the RV32 board config does not, so the 32-bit side only has "off build unchanged".

### 5.22 Page-level App window (U-3, second stage, 2026-11-17)

#### 5.22.1 What was owed

§5.21 gave a frame pool but no user: the App window was still **one** `PS=20` (2 MB) entry per space, space number = frame number (`space n -> APP_BASE + n * 2 MB`). So:

- frames did not come from the pool; space and physical address were still bound, and `pmap frame`'s `used` stayed 0 forever;
- no page granularity. A 2 MB entry has only "whole block valid / whole block invalid", so demand paging (U-5) had nowhere to land.

This stage turns the window into **512 page mappings**: each address space takes 512 frames (2 MB) from the pool as one block and lays the window out as 256 `PS=12` entries. **Still eager, still no return** — it changes "how the window is loaded", not "when" or "how to reclaim".

#### 5.22.2 Geometry: one PS=12 entry covers two adjacent pages

LoongArch TLB entries come in pairs: `TLBELO0` covers `vaddr`, `TLBELO1` covers `vaddr + 4 KB`. Therefore

```
2 MB window = 512 pages = 256 PS=12 entries
set = (VA >> 13) & 0xFF; window base 0x02000000 -> set 0
entry = way * 256 + set   => 256 entries exactly fill one STLB way (256 sets)
```

The window owns **way 0**. That raises a problem which must be solved together: the two 4 KB UART pages (kernel UART and App UART, both in set 0) used `la_tlb_fill()` (whose way is **random**); one collision with way 0 evicts the window's first mapping, and the symptom is an App faulting on its first fetch to an address that "is obviously mapped". So with this stage on, the two UART pages take fixed ways: kernel UART **way 7**, App UART **way 6**.

With the switch off (`ARKRTOS_USE_PAGING_PAGES = 0`) these two `#define`s do not compile and `la_tlb_fill()` is unchanged.

#### 5.22.3 Switch flow and three "no guessing" rules

`arkrtos_port_paging_activate(space)` (page-level build):

1. `la_pg_space_ensure(space)`: the first time a space is installed, take `LA_PG_WIN_PAGES` (512) frames from the pool as one block; if unavailable `return -3` and **not one window entry is touched** — half a window (half valid, half refilling) is the hardest failure to diagnose;
2. same space, first entry PPN == that space's frame base and `ps == 12`: return 0 directly (the space number alone is not enough; an `invtlb` from another path can drop the entry, see §5.18);
3. `la_pg_drop_window()`: issue `invtlb 3` over the whole window VA 4 KB at a time. QEMU's softmmu records translations by VA, and `tlbwr` only clears its own 8 KB **with the odd-page address computed wrong** (`addr & pagesize`), so a window switch must explicitly invalidate 512 times;
4. `la_pg_install_space(space)`: 256 `la_tlb_write(va, 12u, ...)`;
5. spot-check **first and last page**: if either PPN / ps mismatches, `return -2` and `s_pg_app_space` is not changed (reporting an error and staying put beats the lie "switched").

Step 5 had a pitfall: `lookup` returns the PPN of the entry's **TLBELO0 (even page)**, so the expected value for the last page `0x02000000 + 2MB - 4KB` is the **even base** of that 8 KB pair, `base + 2MB - 8KB`; writing the last page itself gives a false failure (first QEMU run reported `pmap u1: rc1=-2`, after the fix `rc0=0 rc1=0`). A comment is left in the code.

Frame origin and ownership: `static uint32 s_pg_space_base[ARKRTOS_PAGING_APPS_MAX]`, `0` = not yet allocated; ~~taken and never returned~~ (honestly registered as debt at the time; **reclaim was added in §5.24** — not a "quietly returned and possibly taken by someone else" implementation: reclaim strictly does "drop window entries first, then return frames").

#### 5.22.4 Window VA moved out of the shell

In the page-level build the "space number -> VA" equality is gone (the VA is always the window base; the frame base only exists physically), so a new port interface `arkrtos_port_paging_window_va()` (-> `arkrtos_paging_window_va()`, fallback `(0u)` when off) was added. The shell's `pmap u1` / `pmap u2` / `pmap cu` used to hard-code `0x02000000` in three places; all now read this interface. The window VA is a port property, not something the shell should know.

#### 5.22.5 QEMU measurements (2026-11-17)

After boot, `pmap frame`'s `used` goes from 0 to **512** — the frame pool finally has a real user. Verbatim (`build/s2_run2.log`, `--boot 25 --timeout 4`, 7 commands):

```
$ pmap frame
pmap: frames total=2048 used=512 free=1536

$ pmap look 0x02000000
pmap look: va=0x02000000 entry=0 ps=12 ppn=0x04000000

$ pmap u1
pmap u1: va=0x02100000 space0=0xA5A50001 space1=0xA5A50002
pmap u1: frame0=0x04000000 frame1=0x04200000
pmap u1: rc0=0 rc1=0
pmap u1: look0 entry=128 ps=12 ppn=0x04100000
pmap u1: look1 entry=128 ps=12 ppn=0x04300000
pmap u1: PASS - same VA, two frames

$ pmap u2      -> PASS - two instances, two spaces, neither frame overwritten
$ pmap cu      -> PASS - from/to user, refusals -1/-2/-3/-4, kernel survived
$ pmap test    -> PASS (map / unmap / rebind / unmap)
$ utest run    -> 13 matched, 13 passed, 0 failed; assertions 133 run / 0 failed
```

Three readings must be read separately:

- `pmap look 0x02000000` reports `ps=12` (not 20) — the window really changed its mapping form;
- `used=512` and `free=1536` — each space takes a whole 2 MB, and space 0 took it in `la_pg_boot_map()`;
- `pmap u1`'s `look0 entry=128`: set 0, way 0, entry 0 = `0*256+0` (the first entry); `look1 entry=128` for the same reason, because both spaces' **same VA** is on the first entry — same number, different PPN (`0x04100000` vs `0x04300000`), which is exactly "one VA, two physical memories".

`pmap u1`'s VA is `0x02100000` (window + 1 MB), which pair it falls on is computed by the port, unrelated to the "last page is TLBELO1" pitfall; both reads here are even pages.

The first log (`build/s2_run1.log`) also has `pmap ftest` under the page-level build: the `used` baseline is 512 (not 0), `pmap: frame alloc 1 rc=0 base=0x04200000`, and the closing line is still `PASS (alloc/free/double-free/pattern, used back to 512)` — the pool's own contract does not change because the window uses it.

RV32 off build: `text 235828 / data 128 / bss 628228`, **byte-identical**.

#### 5.22.6 Gate

`tools/paging_gate.py` gained **pin S** (two self-checks), covering eight places:

- three switch sites: `arkrtos_features.h` default 0 / `Kconfig.features` `config ARKRTOS_USE_PAGING_PAGES` with two `depends on` (PAGING and FRAMES) / board `#define ARKRTOS_USE_PAGING_PAGES     1`;
- port: `LA_PG_WIN_ENTRIES` geometry, the combined guard `#error "ARKRTOS_USE_PAGING_PAGES=1 requires ARKRTOS_USE_PAGING_FRAMES=1"`, `frame_alloc(LA_PG_WIN_PAGES, &base)` plus `return -3;` in `la_pg_space_ensure`, `la_tlb_write(va, 12u, LA_STLB_INDEX(va, LA_PG_WIN_WAY),` in `la_pg_install_space`, `la_tlb_inval_page(` in `la_pg_drop_window`, `return -2;` plus the last-page expectation `(s_pg_space_base[space] + ARKRTOS_QEMU_APP_SIZE - 0x2000u)` and `(probed_ps != 12u)` in `activate`, `la_pg_boot_map();` actually called, and the two fixed UART ways;
- **the old PS=20 path is still there**: `la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u` appears >= 2 times (one each in `la_mmu_init` and `la_pg_assert_window`; without it the default build has no window mapping);
- forwarding chain: hal declaration and `(0u)` fallback, mpu.h declaration and fallback, `paging.c`'s `window_va` really forwarding to the port, and >= 3 shell sites using `arkrtos_paging_window_va()`.

Two self-checks: change `#define LA_PG_WIN_ENTRIES` to `LA_PG_WIN_ENTRXIES`; change `LA_PG_WIN_PAGES` inside `frame_alloc(LA_PG_WIN_PAGES, &base) != 0` to `LA_PG_WIN_PAGEZ` (destroying the judged substring itself, not appending).

`ARKRTOS_USE_PAGING_PAGES` also hangs on `paging_gate.py` and **adds no `ci_gate` item** (same convention as U-8 / pin R).

#### 5.22.7 Explicitly not done

- **No demand paging (U-5)**: load still hands out 512 frames first and lays the whole span. `tlb_refill_entry` has no "BADV inside the window => swap one in" branch, and that entry currently **does not save general registers**, so adding the branch requires completing save/restore first — a separate stage.
- ~~**No space reclaim**~~: at this stage frames were taken and never returned, so once spaces ran out the next request was `-3`. **Added in §5.24** (`arkrtos_paging_space_release`; the loader reclaims the space's old window before activate). Page replacement **was added in §5.26** (the `ARKRTOS_USE_PAGING_SWAP` build; the default still falls back to the original fatal path).
- **No ASID in the default build**: switching spaces still does a whole-span `invtlb` (512 times). The U-7 ASID build exists and was measured (§5.28), but cross-core TLB shootdown is still out of reach.
- **The window owns way 0**: kernel UART / App UART moved to way 7 / 6 to get out of the way, but any other entry entering the STLB through `la_tlb_fill()` could still compete.
- **Only enabled on the LA64 board**: the RV32 side only has "off build unchanged".

### 5.23 Demand paging (U-3 third stage / U-5, 2026-11-17)

#### 5.23.1 What was owed

After §5.22 the window had page granularity but was still **eager**: `activate()` took all 512 frames and filled 256 entries. So "how much memory the App uses" and "how big the App is" were still unrelated numbers — a 2 MB window paid 512 frames even if it touched 4 KB.

The fault handling layer was missing not just code but three **preconditions**:

- `tlb_refill_entry` never **saves general registers**. The `la_refill_c()` it calls has only two exits: switch to another task's frame, or halt. Demand paging is different — after installing the entry it must `ertn` back to **the faulting instruction** and retry, so the interrupted registers must be restored exactly;
- the refill context (`TLBRERA.ISTLBR = 1`) exposes different CSRs than normal (see 5.23.2);
- assembly: §5.22 had the window "owning way 0, way number hard-coded", while demand paging needs the hardware `tlbfill` to choose set and way itself (see 5.23.4).

This stage still **does not return frames**: it changes "when to load", not "how to reclaim after loading". Reclaim is added separately in §5.24 (page-level build returns a whole block, demand build returns pair by pair).

#### 5.23.2 Three silent wrong answers (all hit in QEMU)

The difficulty here is not logic but three shapes that **run but answer wrong**. Each surfaced exactly once in a single real run and is worth pinning down:

1. **You cannot install entries in the refill context with `TLBEHI` + `tlbwr`.** With `ISTLBR = 1`, `helper_tlbwr` / `helper_tlbfill` only honour `CSR_TLBREHI / TLBRELO0 / TLBRELO1`, ignoring `TLBEHI / TLBELO0/1`. Measured shape: writing `TLBEHI` then `tlbwr` in the refill handler installs the entry at the hardware-filled `TLBREHI` (= the faulting address's VPPN) with `TLBELO` taken from the **stale** `TLBRELO0/1` => the entry has **V = 0**. The next access hits a V=0 entry, QEMU reports `TLBRET_INVALID` (**not** `TLBRET_NOMATCH`, so `ISTLBR` is not set) => `eentry` => `la_trap_c` treats it as a fatal PIS exception and halts: `FATAL ecode=0x2 (PIS) era=elf_memcpy`. The correct shape is the usual hardware refill idiom: write `TLBREHI / TLBRELO0 / TLBRELO1` then `tlbfill` (`TLBREHI.PS` is in bits[5:0] and VPPN starts at bit13, so OR-ing the page base with `12` gives the complete value).
2. **`CSR.SAVE0` (0x30) is unusable on QEMU 9.2.3.** The spec makes it optional; QEMU reads back **0**. The first version used it to stash `sp`, and the symptom was `sp = 0` in `la_refill_c`, dying a second time: `KERNEL refill (plv0) badv=0x15591570 era=arkrtos_sched_activate`. Only after moving to a static slot did it progress.
3. **You cannot use a register you need to save as the frame base.** The second version used `$t0` as the frame pointer: `la.abs $t0, <frame>` already **destroys the interrupted `$t0`**, so what `st.d $t0, ...` later stores is the frame address. Extremely subtle — boot, load and the first App all behaved, until `elf_memcpy` (where GCC uses `$t0`/`$t1` as source/destination pointers) faulted for the first time: source and destination pointers were shifted by a whole segment, the loop terminator no longer equalled `n`, and the **2 MB window was written 8 KB at a time past its end**, stopping at `badv=0x02200000 era=elf_memcpy`. Located by temporarily instrumenting the loader and disassembling (`21f4e0`–`21f504` show the loop using `$t0` as the source pointer). The decision was **`$r21`**: LoongArch psABI reserves it (the kernel and psABI-compiled Apps never use it, and the comment in `example/*/start.S` says the same), so it can be used as a pointer and need not be restored. The frame is 256 bytes, 16-byte aligned, GPRs only (`ERA` / `PRMD` stay in CSRs).

#### 5.23.3 Implementation: switch, granularity, ledger, flow

Three switch sites: `ARKRTOS_USE_PAGING_DEMAND` (`arkrtos_features.h` default 0 / `Kconfig.features` `config ARKRTOS_USE_PAGING_DEMAND` with `depends on` PAGING and PAGES / board default 0). The port has two combined guards: `=1 requires ARKRTOS_USE_PAGING_PAGES=1` and `=1 requires ARKRTOS_USE_PRIV=1`.

Granularity is **one 8 KB pair**, not one page: a `PS=12` entry yields two pages (`TLBELO0` covers `vaddr`, `TLBELO1` covers `vaddr + 4 KB`), and "half an entry" cannot be expressed => take both frames or install nothing.

The ledger is `s_pg_pair_pa[space][256]` (0 = this pair has not been touched). In the demand build `s_pg_space_base` is always 0 — a space no longer has "one whole 2 MB base", physical pages are taken pair by pair. This also changes the shell reading convention: `pmap u1` / `pmap u2` show `frame0` / `frame1` as `0x00000000` in this build (`look` reports `no entry ps=0`), and their PASS criterion relies on "the same VA reads two different contents and the other slot was not overwritten", not on frame numbers.

Flow:

- `activate(space)`: `invtlb` drops the old space's translations and only `s_pg_app_space` is changed — **not one entry is laid** and there is no "already this space" short-circuit (the truth in this build is `s_pg_pair_pa`; the short-circuit saves little and adds a lying path);
- first touch of a pair => `TLBRENTRY` => `la_refill_demand_c()`: read `TLBRBADV`, and if inside the window `frame_alloc(2u, &pa)`, record it, `tlbfill` one entry and `ertn` back to retry the instruction; outside the window / pool empty => `return 0` and the assembly falls back to `la_refill_c()` (the original report / kill path, which does not return);
- `la_pg_assert_window()` (re-lay after revoking a dynamic mapping): only re-fills the pairs **already paid for**.

This build deliberately **does not distinguish PLV**: kernel copies through the window (ELF load, `pmap` self-tests) and the App's own accesses take the same path, otherwise the loader's first write would die on "KERNEL refill -> fatal".

#### 5.23.4 `tlbfill` chooses a random way => the two MMIO pages must move into MTLB

§5.22 dodged the conflict with "window owns way 0, UART on way 7 / 6". That does not hold in the demand build: `tlbfill` with `pagesize == CSR.STLBPS` (12) goes through `set = get_random_tlb(0, 7)` — **the way is chosen by hardware at random** (`index = way*256 + set`, `set = (VA>>13) & 0xFF`). The window base `0x02000000` spans exactly one set (0..255) one entry each, so the window's own 256 entries cannot collide with each other; but **set 0 shares with kernel UART `0x1fe00000`** and set 1 shares with the App UART alias `0x02202000`, so the window's pair 0 (or 1) landing on that way evicts the console page.

Measured shape (twice, same form): `pmap u2` / `pmap test` fatal halfway with `badv=0x1fe001e5`. The fix moves the two MMIO pages into **MTLB** (`tlbfill` only touches STLB 0..2047 and never collides with MTLB), with slot assignment pinned in a comment in `la_mmu_init()`:

```
+0/+1     two kernel PS=22 large pages
+2        App whole block (non-page-level build, PS=20)
+3..+10   dynamic alias (LA_MTLB_DYN_FIRST, 8 pairs)
+11       page-frame pool (PS=22)
+12/+13   kernel UART / App UART alias (demand build only)
```

The first version put them at `+3/+4`, exactly competing with `arkrtos_port_paging_alias()` — `pmap test` registering a dynamic mapping evicted the console page (same shape, second occurrence), so they moved to `+12/+13`.

#### 5.23.5 Build and measured readings

The demand build gets its own `BUILD` directory; the baseline directory is untouched:

```bash
make -f example/qemu_loongarch64/Makefile -j8                      # baseline
make -f example/qemu_loongarch64/Makefile BUILD=build/qemu_la64_demand \
     EXTRA_CFLAGS=-DARKRTOS_USE_PAGING_DEMAND=1 -j8                # demand
```

`EXTRA_CFLAGS` goes into both `CFLAGS` and `ASFLAGS` — the `.S` also decides "whether the refill entry saves general registers" from the switch, so the assembly side must see board macros (`-include arkrtos_board_config.h` added to `ASFLAGS`; this alone changes not one byte of the default build, see 5.23.6).

Size: `size` reports text 366120 -> **366152** (+32 B), data 416 / bss 767508 unchanged (baseline `build/sde_final`, demand `build/qemu_la64_demand`).

Executed (`--boot 25 --timeout 6`, `build/s3_run1.log`):

```
$ pmap frame
pmap: frames total=2048 used=14 free=2034

$ exec /mnt/ram/hello one two
[ELF] /mnt/ram/hello: 1748 B, 2 segment(s), entry 0x02000000, sp 0x021FC000, argc 3, task 4, space 0
[hello] argv[0]=/mnt/ram/hello / [1]=one / [2]=two
[ELF] /mnt/ram/hello: finished after 0 ms (space 0, exit code 3)

$ pmap look 0x02000000
pmap look: va=0x02000000 no entry

$ pmap u1
pmap u1: va=0x02100000 space0=0xA5A50001 space1=0xA5A50002
pmap u1: frame0=0x00000000 frame1=0x00000000
pmap u1: rc0=0 rc1=0
pmap u1: look0 entry=0 ps=0 ppn=0x00000000
pmap u1: PASS - same VA, two frames

$ pmap u2
pmap u2: same VA 0x02000000 reads 0x00150064 / 0x00150064, other slot before/after 0xA5A50202 / 0x00150064
pmap u2: PASS - two instances, two spaces, neither frame overwritten

$ pmap cu      -> PASS - from/to user, refusals -1/-2/-3/-4, kernel survived
$ pmap test    -> PASS (map / unmap / rebind / unmap)
$ utest run    -> 13 matched, 13 passed, 0 failed; assertions 133 run / 0 failed
```

Three groups must be read separately:

- `used=14` instead of §5.22's 512 — the window pays per touched pair. One `hello` (two PT_LOAD segments + stack + argv area) touches very few pairs, and with the self-test's scratch space the total is 14;
- **`pmap look 0x02000000` reporting `no entry`** is the correct shape for the demand build, not a defect — after `activate` the window is empty and untouched pairs have no entry. The same for `pmap u1`'s `look0/look1 entry=0 ps=0` (it reads the post-switch space view);
- `pmap u1` / `pmap u2`'s `frame0/frame1 = 0x0` is a ledger convention (`s_pg_space_base` does not exist in this build), not "no frames" — the substance of the criterion is that the same VA reads two different contents in two spaces and the other slot is not overwritten after exit.

Off-build regression (baseline directory) all as before: `used=512`, `pmap look 0x02000000 entry=0 ps=12 ppn=0x04000000`, `pmap u1/u2/cu/test` all PASS, `utest 13/13`.

#### 5.23.6 The correct way to prove "off build unchanged"

This one hit a methodological error worth recording. The first comparison was "baseline image with changes sha256 != image rebuilt after `git stash` to HEAD", which suggested the new `-include` in `ASFLAGS` let `.S` see board macros. **That conclusion was wrong** — the two images differ anyway because each carries a `__DATE__` / `__TIME__` build-time string (`build : Nov 17 2026 17:12:43` inside `arkrtos_shell.o`).

The correct method is two steps:

```bash
# 1) pin the build timestamp (GCC honours SOURCE_DATE_EPOCH)
SOURCE_DATE_EPOCH=1600000000 make -f example/qemu_loongarch64/Makefile BUILD=<dir> -j8
# 2) strip debug info before comparing (DWARF line tables legitimately change with source edits)
loongarch64-newlib-elf-objcopy --strip-debug <dir>/ark_qemu_la64.elf <dir>/stripped.elf
sha256sum <head>/stripped.elf <now>/stripped.elf
```

Measured: HEAD and the modified workspace each produced a `stripped.elf` that was **byte-identical** (`027dcf53...`, 460264 B). The `-include` suspicion was also disproved: passing `EXTRA_CFLAGS` explicitly as `-DARKRTOS_USE_PAGING_DEMAND=0` (equivalent to "unconditionally let `.S` see board macros") produced the same hash.

The only remaining differences are two classes, neither behavioural nor size-related: the `__DATE__/__TIME__` string (unavoidably different without a pinned timestamp — HEAD itself rebuilt twice differs) and DWARF (sources changed, line tables follow).

#### 5.23.7 Gate

`tools/paging_gate.py` gained **pin T** (two self-checks), and this one **really compiles**:

- source assertions: three switch sites, the two port `#error`s, `s_pg_pair_pa`, `frame_alloc(2u, &pa) != 0` and `la_tlb_fill_refill(va,` in `la_pg_demand_fault`, `LA_CSRW(LA_CSR_TLBREHI` and `"tlbfill"` in `la_tlb_fill_refill`, `LA_CSRR(LA_CSR_TLBRBADV` in `la_refill_demand_c`, the MMIO slots `LA_MTLB_INDEX + 12u` / `+ 13u`, **the old path still present** (`LA_STLB_INDEX(... KUART_WAY)`), and on the assembly side `la.abs  $r21, la_refill_stack_top`, `st.d    $t0, $r21, 88`, `bl      la_refill_demand_c`, `ld.d    $sp, $sp, 16`, `ertn`, plus **`la.abs  $t0, la_refill_stack_top` must not exist** (pinning 5.23.2 item 3), and the old build's `la.abs  $sp, la_refill_stack_top` + `bl la_refill_c` still present;
- two self-checks: corrupt `s_pg_pair_pa`, and swap `$r21` back to `$t0`; pin T must fail both times;
- **real compilation**: `arkrtos_port.c` through `-fsyntax-only` with `-DARKRTOS_USE_PAGING_DEMAND=1`; `arkrtos_context.S` really assembled to `.o` with the same macros (`-fsyntax-only` does not cover assembly, and this must use `-c`, otherwise the whole class of "assembly written wrong" false greens is invisible).

Same convention as pin R / pin S: **no new `ci_gate.py` item**, the gate hangs on `paging_gate.py`.

#### 5.23.8 Explicitly not done

- ~~**Frames not returned**~~: at this stage the demand build only "pays per pair"; neither exit nor space switch reclaims. **Added in §5.24** (demand build frees `frame_free(pa, 2u)` pair by pair and clears the ledger); page replacement **added in §5.26** (switch `ARKRTOS_USE_PAGING_SWAP`, default 0).
- **No ASID in the default build**: switching still does a whole-span `invtlb`; the ASID build is §5.28 (U-7).
- **refill does not distinguish PLV**: kernel and App take the same path (5.23.3 explains why it must).
- ~~**No page replacement / eviction**~~: the default build still does "pool empty => `return 0` => original fatal path"; with `ARKRTOS_USE_PAGING_SWAP=1` it swaps out an 8 KB pair and retries (§5.26).
- **Measured on the LA64 board only**: the RV32 side only has "off build unchanged".
- **`pmap u1/u2` shows frame number 0x0**: the ledger in this build is `s_pg_pair_pa`, and the shell has no demand-build-specific reading (seeing every pair would need a new command, registered as optional).

### 5.24 Releasing a page-space window (U-3 leftover, 2026-11-17)

`paging_space_release(space)` undoes what `paging_space_activate` installed. Return codes:

| Code | Meaning |
|---|---|
| `0` | Released, or already free (idempotent) |
| `-1` | `space` out of range, or `PRIV == 0` (thread mode) |
| `-2` | No frame pool in this build (`PAGES != 1`) |
| `-3` | Caller `sp` lies inside the window being torn down |
| `-4` | Frame pool refused the free |

Two implementations exist:

1. **Whole-block**: `frame_free(base, LA_PG_WIN_PAGES)` frees the page-level window in one call.
2. **Per-pair on demand**: `frame_free(pa, 2u)` per pair, and each free clears its ledger entry `s_pg_pair_pa[space][i] = 0u`. This is what demand paging needs, because only some pairs may actually be resident.

**Order is not optional: retract the TLB entries before returning the frames.** Reversing it lets the MMU hand out a translation to a frame that has already been returned to the pool.

In the loader, `release` must run **before** `activate` of the next space. The gate pins this down by comparing source positions of `find` on the two calls, so a refactor that reorders them fails the check instead of silently corrupting the pool.

Readings (11 assertions):

```
pmap rel 1   -> used 512 -> 512   (idempotent, nothing to free)
pmap rel 0   -> used 512 -> 0
after exec   -> used 1024
```

`-3` (caller stack inside the window) has a source-level criterion only; it was never exercised at runtime, because no shell command can place `sp` inside the window.

Pin **U** covers the release path. The RV32 side only shows "off build unchanged", byte-identical: `7cf766ae...`.

### 5.25 Loading an ELF32 image on RV32 (U-4, 2026-11-17)

The RV32 side had no ELF loader; the LA64 loader in §5.10 assumed 64-bit ELF throughout. U-4 makes one translation unit serve both widths.

- **PMP window**: 1 MB at `0x80A00000`, same as the LA64 loader.
- **One TU, two tables**: the header layouts are selected at compile time by `__SIZEOF_LONG__`:

| Field | ELF32 | ELF64 |
|---|---|---|
| `EHDR` size | 52 | 64 |
| `PHDR` size | 32 | 56 |
| `e_phoff` offset | 28 | 32 |
| `e_phentsize` offset | 42 | 54 |
| `p_filesz` offset | 16 | 32 |

- `elf_gate.py`'s C-level criterion flips under U-4, and pin **G** covers 11 offsets.
- **`arkrtos_mpu_rom_window()` needed a new branch.** Without it the loader faulted with `mcause=1 mtval=0x02000000`: the MPU ROM window still described the old 64-bit image region, so the first fetch after the jump landed outside it.

Three defects appeared **only under a real run**:

1. **`-Ttext` fell back one page.** The linker placed `.text` a page lower than the loader's expectation. Fixed with `-Wl,-Ttext-segment=`.
2. **Sub-command 4 was not compiled in.** The build file listed the shell command but the object was missing from `KERNEL_SRCS`; the shell compiled, linked, and then **never returned** because the dispatch table pointed at an absent implementation.
3. **Register-then-demote-and-write-MPU race.** The loader registers the window, drops privilege, and writes the MPU in three steps; a trap between them could start an App whose window was not yet enforced. Fixed by wrapping the sequence in `ENTER_CRITICAL()` / `EXIT_CRITICAL()`.

Five consecutive runs, transcript:

```
entry sp=-2135965696
TASK:4717 task 8 exits
entry 0x80A00074
```

The default build changes in three places, all guarded:

- `ARKRTOS_MPU_ELF_WINDOW_*` conditionally defined: **+564 B**.
- `code_in_ram()` rewritten to an equivalent form: **-16 B**.
- The `hello32` blob is a conditional dependency.

Boundary: the ELF window is **shared**, so there is **no address-space isolation** between an App image and anything else mapped there. This is an image-loading facility, not a protection boundary.

### 5.26 Page replacement (U-16, 2026-11-17)

Swap is off by default: `ARKRTOS_USE_PAGING_SWAP=1`.

- **The swap area lives in a `.bss` array and does not save memory.** `bss` grows by 73 728 B (8 slots x 8192 B plus one 8192 B bounce buffer).

Three things the implementation refuses to guess:

1. **Never install a half-written page.** A slot is filled completely before it is linked into the ledger.
2. **On swap-out, `invtlb` first, then `frame_free`.** Same ordering rule as §5.24.
3. **On swap-in, copy into the bounce buffer first, then request a frame.** Requesting the frame first can deadlock when the pool is empty and the eviction path needs the frame it just handed out.

Slot numbering uses `k + 1` so that zero means "not in a slot".

Two-round victim selection: first prefer a pair belonging to **another** space; if none, advance a round-robin hand `s_pg_swap_hand`. `take_pair` gives up after at most 8 rounds rather than looping.

Shell surface: `pmap swap` prints five fields; `pmap swaptest` asserts three criteria.

Reading:

```
swap out 0 -> 20, in 0 -> 12, mismatch=0
```

Sizes: `366744` / `366824` / **`367784` text, plus `bss` 841236**.

Pin **V** carries three mutation self-checks (each mutation must break the assertion it targets) plus a real compile.

### 5.27 Console login gate (SEC-G01, 2026-11-17)

- **No plaintext password is stored.** The build keeps a 16-byte salt and a sha256 digest.
- **Constant-time comparison**: 32 XORs, no early exit on the first differing byte.
- `ARKRTOS_SHELL_AUTH_TRIES` defaults to **5**; after that the console is hard-locked until reset.
- **The gate hook sits before `ark_shell_find_command()`.** Placed after it, a `./prog` invocation would bypass the gate entirely.
- `tools/shell_auth_cred.py` generates `arkrtos_shell_auth_local.h`, which `.gitignore` blocks. Demo credentials: salt `ark-rtos-console`, password `arkrtos-demo`.

Three expected command sequences:

```
(1/5) ... (5/5)
ok, console unlocked
```

Size: `366744` -> **`370320` text**, `+24` data.

`KERNEL_SRCS` now wraps the list in `$(sort)`. Without it, NTFS directory enumeration order reorders the objects, which **invalidated the previous reproducible baseline**. New criterion: `objcopy -O binary` compared at `a510d493...`, 31 462 512 B.

Gate `shell_auth_gate.py` covers A through K; `ci_gate` went from 43 to 44 checks.

Honest boundaries: the password crosses the serial line in the clear; offline brute force against the digest is possible; the hard lock does not survive a reset; none of this is device identity or attestation.

### 5.28 Per-space ASID (U-7, 2026-11-17)

`ARKRTOS_USE_PAGING_ASID` defaults to **0**; it requires `PAGES` and `PRIV`, and `APPS_MAX <= 8`.

Mapping: space `s` gets ASID `s + 1` and way `s % 8`.

Installation order for a space: call `la_asid_swap` first, then write the 256 entries. Switching writes `CSR.ASID` and then **reads the first and last pair back** to verify. Release switches ASID first. Boot sets space 0. Dynamically mapped MMIO entries carry `LA_PTE_G`.

Readings:

```
pmap asid: mode=on current=1 spaces=2
entry=0   ppn=0x04000000
entry=256 ppn=0x04200000
look0 entry=128   look1 entry=384
```

Switch cost, three runs: **104910 / 97170 / 54960 ticks**, steady state **34.35 us per switch**. This includes the two `tlbsrch` read-back verifications; the CSR write alone has no independent reading.

`6859ec53...` was later invalidated by §5.29.4.

Pin **W** plus a real negative test.

### 5.29 ASID invalidation semantics (U-7b, 2026-11-17)

Rather than assume what each `invtlb` op does, U-7b measured it op by op:

| Op | Behavior |
|---|---|
| 0 / 1 / 2 / 6 | Kill the **entire** table: `badv=0x0023da58` |
| 4 / 5 | **No-op** (equivalent to doing nothing) |
| 7 | Illegal instruction: `ecode=0xd badi=0x0649b587` |
| 3 | Selects by **VA only**, ignoring ASID |

`pmap asidinval` A through D, two rounds, both print `OP3 IGNORES ASID`.

Consequence: only two diagnostic entry points survive, `asid_raw_swap` and `inval_va`. `inval_va_asid()` (which tried `invtlb 6`) was **deleted wholesale** — it killed the whole table, so it could not have provided per-ASID invalidation anyway.

Same-argument cross-check on three trees: all `3a6c4ba7...`; `6859ec53...` is void.

Sizes: `text 367264`; ASID build `369848`.

Three boundaries: this must **not** be read as "the kernel does not support shootdown" (it does not support per-ASID selective invalidation on this QEMU, which is a different statement); no real hardware was tested; and the four steps in `space_release` cannot be skipped.

### 5.30 Image pool on a QEMU flash backend (SEC-G17, 2026-11-17)

On QEMU the flash device powers up all `0x00`. The pool's `find_clean` looks for `0xFF` (erased), so it concluded **the pool had zero bytes free**:

```
pool free : 0 B
-14 (ERR_NOSPACE)
```

Fix: the board layer overrides five pool functions —

- `QEMU_POOL_FLASH_BEGIN` / `QEMU_POOL_FLASH_END`
- erase writes `0xFF`
- write is byte-wise
- sector size
- `board_init` erases the whole pool **before** `trace_init`

After the fix:

```
pool free : 524288 B (512K)
```

`loader_crc_recheck_pool_e2e` then failed on first install with `-5`:

```
DIAG got=f2a5e2fa want=56821d7f base=260000 total=148
```

Root cause: `load_buffer` never folded the relocation table into `crc_written`, so the CRC covered the payload but not the applied relocations. **The fix applies to the default build as well**, so the image hash moved: `6bbcda8b...` -> **`fbd1094e...`**, and `text 367264` -> **`367536`** (+272 B). This is one of the few places where a "fix" is explicitly *not* byte-identical in the default build, and it must not be described as such.

Readings: switch-on build 17/17, `assertions 155`; default build 13/13, `133`; `sec: 12 checks - pass 7, fail 0, finding 2, skip 3`.

`tools/sec_crc_gate.py` covers 8 pins and both builds of 4 translation units.
#### 5.30.5 Later size change (2026-11-18, unrelated to this section)

After a later purely structural cleanup the same default image is no longer `fbd1094e...`.
`svc_file_sys()` in `kernelsrc/src/arkrtos_task.c` (458 lines) was split along the path-model
boundary its own comment already described, into `svc_fs_volume()` (subcommands 1..10,
volume relative), `svc_fs_namespace()` (11..23, namespace absolute) and a `svc_file_sys()`
that is now only an argument check plus one branch. Case bodies were moved line for line and
dedented by 4 spaces; no return value or error code changed.

Same criterion (`SOURCE_DATE_EPOCH=1600000000`, `objcopy -O binary`):

```
before  text 367536 / data 440 / bss 767508   sha256 fbd1094e...   31,462,512 B
after   text 367584 / data 440 / bss 767508   sha256 66a09c13...   31,462,512 B
```

That is `text` +48 B (one extra call level plus constants `-O2` no longer shares); `data`,
`bss` and the image length are unchanged. **Do not claim byte-identical** - the binary did
change, only the behaviour did not. The behavioural criterion is that
`python example/qemu_loongarch64/shell_check.py` still prints `PASS` (all nine groups),
with `stack` reporting `9 task(s)`, `0 unmeasured`, `0 at/over 90%`.

#### 5.30.6 One more cleanup: `elf_load()` split into three stages (2026-11-18)

Directly on top of the previous subsection, `elf_load()` in `kernelsrc/src/arkrtos_elf.c`
(278 lines) was split along the three stages its own comment already named - **pass 1 validate,
pass 2 write, then register and drop privilege** - into three `static` functions:

| Function | Location | Responsibility |
|---|---|---|
| `elf_check_segments()` | `arkrtos_elf.c:558` | Walks the program headers for validation only (rejects `PT_INTERP`, rejects `p_filesz > p_memsz`, uses the subtraction form of the window range test); writes nothing; returns the visible `PT_LOAD` segment count |
| `elf_write_segments()` | `arkrtos_elf.c:621` | Does the actual writing: copy the `filesz` part, then zero `memsz - filesz` (BSS); paging builds go through `elf_copy_to_window()` / `elf_zero_window()`, non-paging builds through `elf_read_exact()` / `elf_memzero()` |
| `elf_start_task()` | `arkrtos_elf.c:694` | The whole `register -> drop privilege -> write back MPU window -> bind frames` sequence (`ARKRTOS_ENTER_CRITICAL()` through `ARKRTOS_EXIT_CRITICAL()`), returning the task id |

`elf_load()` itself drops from 278 to **143 lines**, keeping only argument checks, reading the
ELF header, the entry-point test, the three stage calls and filling `p_info`. Equivalence was
again argued with the normalised line multiset: the only permitted differences are the new
function skeletons, the `*p_segs` / `*p_bytes` / `return id` tails, the three calls, and
`win_base` becoming `(uint32)ARKRTOS_ELF_WINDOW_BASE` inside `elf_start_task()` (that function
no longer takes the parameter).

Same criterion (`SOURCE_DATE_EPOCH=1600000000`, `objcopy -O binary`):

```
before  text 367584 / data 440 / bss 767508   sha256 66a09c13...   31,462,512 B
after   text 367600 / data 440 / bss 767508   sha256 2968c36e...   31,462,512 B
```

`text` +16 B, `data` / `bss` and the image length unchanged; **again do not claim
byte-identical**. The behavioural criterion is that `example/qemu_loongarch64/shell_check.py`
still prints `PASS` (all nine groups, including the `[ELF]` / `[MOD]` tamper checks).

After the split the longest function reported by `doc_audit.py funcs` is `mod_one_rela()` in
`arkrtos_modld.c` (252 lines); `elf_load` leaves the front of the list.

#### 5.30.7 One more: `mod_one_rela()` split by machine class (2026-11-18)

`mod_one_rela()` in `kernelsrc/src/arkrtos_modld.c` (252 lines) was one big `switch` that
already grouped relocation types into four unrelated machines. It is now four `static`
functions, each keeping only its own `switch`, with `default` returning a local sentinel
`MOD_RELA_PASS` (= 1, meaning "not my type"):

| Function | Types it owns | What it needs |
|---|---|---|
| `mod_rela_push()` | `R_LA_SOP_PUSH_ABSOLUTE` / `PUSH_PCREL` / `PUSH_PLT_PCREL` / `PUSH_GPREL` | symbol value `S`, address `P`, the stack; `GPREL` also looks up the GOT slot |
| `mod_rela_alu()` | `R_LA_SOP_ADD` / `SUB` / `SL` / `SR` / `NOT` / `ASSERT` | the SOP stack only |
| `mod_rela_pop()` | `POP_32_S_10_12` / `S_5_20` / `S_10_16_S2` / `S_0_10_10_16_S2` / `POP_32_U` | the stack top plus `P` (it reads and writes instruction words) |
| `mod_rela_write()` | `R_LA_32` / `R_LA_64` | symbol value `S`, `addend`, `P` |

`mod_one_rela()` keeps only "compute `P` and `S` -> ask the four in turn -> if none of them
claims the type, set `c->last_reloc = type` and return `ARKRTOS_MOD_ERR_RELOC`", about 40
lines. Case bodies were moved line for line; equivalence was argued with the normalised line
multiset, where the only permitted differences are the four function skeletons, the
`MOD_RELA_PASS` `default`, the four dispatch calls and the signatures. The dispatch order is
irrelevant because the four type sets are disjoint.

Same criterion:

```
before  text 367600 / data 440 / bss 767508   sha256 2968c36e...   31,462,512 B
after   text 367392 / data 440 / bss 767508   sha256 1d94199f...   31,462,512 B
```

That is `text` **-208 B** (each helper only sees its own handful of `case` labels, so the jump
tables and constant loads under `-O2` shrink); `data` / `bss` and the image length are
unchanged. The behavioural criterion `shell_check.py` gave two `PASS` out of three runs; the
third failed on the `help` line and its raw transcript ends with the pre-existing intermittent
`FATAL estat=0x000d0000 ecode=0x0000000d era=0x00000008` described in section 5.13.4 - by the
conclusion recorded there this kind of intermittent is unrelated to code changes, and both
`PASS` transcripts contain the `[ELF]` / `[MOD]` tamper checks.

After this split `doc_audit.py funcs` reports **31** functions over 120 lines instead of 32;
the longest is now `arkrtos_apppack_build()` in `arkrtos_apppack.c` (244 lines).

#### 5.30.8 Third one: `arkrtos_apppack_build()` split into five stages (2026-11-18)

`arkrtos_apppack_build()` in `kernelsrc/src/arkrtos_apppack.c` (244 lines) was already a five-stage
pipeline separated by comments. Each stage is now a `static` function and the entry point only
wires them together and fills `stats`:

| Function | Lines | Responsibility | In / out |
|---|---|---|---|
| `apppack_resolve_entry()` | ~30 | the two legal entry forms (`e_entry` inside the payload / the header `entry_off` field) | out `*p_entry_off` |
| `apppack_count_pass()` | ~31 | first pass: count patch entries, write nothing | out `*p_count` / `*p_n` |
| `apppack_write_table()` | ~29 | second pass: fill the table, deterministic reconciliation, rebase offsets from payload-relative to image-relative | in `n`/`payload_off`, out `table[]` |
| `apppack_verify_payload()` | ~61 | copy A, apply the table, compare byte for byte against D, restore A | in `table`/`n`, out `payload` |
| `apppack_fill_header()` | ~51 | zero the header, fill fields, `image_id`, three CRCs | in `payload`/`table`/`n` |
| `arkrtos_apppack_build()` itself | **101** (was 244) | check arguments, derive `payload_off`/`total` and bounds, call the five, `*out_len = total`, fill `stats` | - |

`payload_off` / `total` / `table` / `payload` stay in the entry point: they only exist after the
first pass knows `n`. Case bodies were moved line for line, and equivalence was again argued with
the normalised line multiset: only-OLD is reduced to the deliberately rewritten statements
(`entry_off =` -> `*p_entry_off =`, the `count.n_* = 0u` resets,
`apppack_scan(..., &count)` -> `(..., p_count)`, `n = count.n_rom + ...` -> `p_count->...`) plus the
comment heads moved out of the function; only-NEW is function skeletons, five calls and
`if (rc != 0) { return rc; }`. **No statement was lost.**

**This change is byte-identical in the default configuration**, unlike the previous two splits, and
the reason has to be stated:

```
before  text 367392 / data 440 / bss 767508   sha256 1d94199f...   31,462,512 B
after   text 367392 / data 440 / bss 767508   sha256 1d94199f...   31,462,512 B
```

With `ARKRTOS_USE_APPPACK=0` the whole translation unit is an empty target, so splitting it cannot
change a single byte of the default build. That is not evidence that the split is invisible; it is
only evidence that this path is off by default.

**How was the `APPPACK=1` path checked?** Every board header does `#undef` then `#define ... 0`, so
a command-line `-D` cannot win, and neither `tools/arkmake_syntax_gate.py` nor `ci_gate.py`
compiles this TU. The workaround is a board-header shim used only for checking (written under
`build/`, not tracked):

```c
/* build/_apppack_chk/arkrtos_board_config.h */
#include "../../board/qemu_loongarch64/arkrtos_board_config.h"
#undef  ARKRTOS_USE_APPPACK
#define ARKRTOS_USE_APPPACK 1
```

Placed first in the `-I` list together with `-include arkrtos_board_config.h`, and compiled with the
same LA64 flags as the makefile (`-c kernelsrc/src/arkrtos_apppack.c`): `exit=0`, zero warnings, and
`nm` shows two real symbols, `T arkrtos_apppack_build` and `T arkrtos_apppack_error_name`. **This is
currently the only way to show the TU is type-correct with `APPPACK=1`.** It is not a behavioural
check; end-to-end behaviour of `APPPACK=1` remains unverified.

Readings and criteria:

- build: `make -f example/qemu_loongarch64/Makefile` `exit=0`, no warning in `arkrtos_apppack.c`;
- behaviour: `shell_check.py` **PASS once** (banner/help/uname/dt/ls/cat/stack/utest, with the
  `[ELF]` / `[MOD]` tamper checks present in the transcript);
- `format_gate.py`: `361 own C/asm file(s) clean on F1-F4`;
- `doc_audit.py funcs`: functions over 120 lines go **31 -> 30**, `arkrtos_apppack_build` leaves the
  list, and the longest function in `apppack.c` becomes `apppack_scan()` (185 lines, untouched).

Evidence tier: **compiles plus default-config QEMU end-to-end**; `APPPACK=1` only reaches
"type-correct", with no runtime evidence.

#### 5.30.9 Fourth one: `svc_fs_namespace()` split by four subcommand ranges (2026-11-18)

`svc_fs_namespace()` in `kernelsrc/src/arkrtos_task.c` (232 lines) serves SVC 0x1C subcommands
11..23. Those numbers already form four contiguous ranges that map to four kinds of object, so the
function is now four `static` helpers:

| Function | Subcommands | Responsibility | Lines |
|---|---|---|---|
| `svc_fs_ns_io()` | 11..14 | open / read / write / close (byte stream on a handle) | 74 |
| `svc_fs_ns_meta()` | 15..17 | stat / seek / readdir | 73 |
| `svc_fs_ns_tree()` | 18..21 | unlink / mkdir / rmdir / rename | 76 |
| `svc_fs_ns_size()` | 22..23 | truncate(path) / ftruncate(handle) | 36 |
| `svc_fs_namespace()` itself | - | four boundary tests, unknown number returns `EINVAL` | **24** (was 232) |

Case bodies were moved line for line and each family keeps the original `switch` / `default`
style. Equivalence was argued with the normalised line multiset: **only-OLD is empty** (no
statement lost), only-NEW is the four function skeletons, three extra `default` blocks and four
boundary tests. **No scheduler lock was added or removed**: the original note that ark_vfs's port
lock is `arkrtos_sched_lock`, so this side must not nest it, stays in the first family's comment
and the other three follow the same rule. The numbering is contiguous, so the entry point selects
a family by boundary only; 24 and above still falls through to `EINVAL`, as the old `default` did.

One real pitfall from this split, recorded for later: the generated section comment placed above
`switch` was missing its closing `*/`, so the C comment swallowed up to the first `*/` at the end
of the first `case` line, taking `switch`, `{` and that `case` with it. The normalised comparison
caught it immediately (only-OLD lost `case ARKRTOS_PATH_SUB_OPEN:`, only-NEW gained two `switch`
lines); `format_gate` would not have, because it only checks byte rules. **A split script has to
compare by itself; do not rely on the compiler or the format gate to catch this class.**

Readings:

```
before  text 367392 / data 440 / bss 767508   sha256 1d94199f...   31,462,512 B
after   text 367336 / data 440 / bss 767508   sha256 b0ad6a91...   31,462,512 B
```

`text` **-56 B**; `data` / `bss` and the image length are unchanged. `shell_check.py` `PASS` once;
`arkrtos_task.c` reports the same **147** warnings as before the change (all pre-existing
pointer-width casts; the new code adds none). `doc_audit.py funcs`: functions over 120 lines
**30 -> 29**, and the longest is now `arkrtos_mini_load_image()` (229 lines).

#### 5.30.10 Fifth one: `arkrtos_mini_load_image()` split by stage, stream and finish (2026-11-18)

`arkrtos_mini_load_image()` in `kernelsrc/src/arkrtos_mini.c` (229 lines) did three things: read the
image header and relocation table, borrow two blocks sized from what the header declares, and stream
the payload from the file into RAM while accumulating a CRC, then fix up the entry. It is now two
`static` helpers plus a short entry point:

| Function | Responsibility | Lines |
|---|---|---|
| `mini_load_stage()` | read header, `arkrtos_loader_check_header(p_hdr, 0, MASK_MINI)`, read the relocation table, check again, compute `code_size` / `code_span` / `ram_size_hdr` / `code_want`, the wrap-around self check, the `max_block` and stack-floor tests, then `arkrtos_ptable_mini_alloc()`; fills `pt_index` / `code_base` / `code_block` / `code_size` / `ram_base` / `ram_block` / `ram_size` on `p_img` | 92 |
| `mini_load_stream()` | `while(cur < total)` reads a chunk, copies it straight into `p_img->code_base` / `p_img->ram_base`, updates the CRC; `yield_credits` stays inside `#if (ARKRTOS_MINI_YIELD_CHUNKS > 0)`; one output `*p_crc` | 93 |
| `arkrtos_mini_load_image()` itself | environment test (`ARKRTOS_MINI_READ_VFS`), take the scheduler lock, call both helpers, compare the CRC, compute the entry (set the Thumb bit on ARM), unlock, fill `entry` / `crc`; each stage failure rolls back what that stage owns | **61** (was 229) |

Quantities that only exist once something has been counted (`payload_off`, `total`, `delta_rom`,
`delta_ram`) are not passed out as parameters; `mini_load_stream()` recomputes them from `p_hdr` /
`p_img`, otherwise the second helper would take seven or eight arguments. `free_out` / `unlock_out`
stay in the entry point only, and both helpers report failures with `return <code>`. Equivalence was
again argued with the normalised line multiset: only-OLD contains only the deliberately rewritten
statements (9 of `ret = X; goto unlock_out;` / `goto free_out;`, plus locals moved to their
declaration point), **no statement was lost**.

**This split caught two real errors that the default build cannot see at all.** `ARKRTOS_USE_MINIAPP`
defaults to 0 in every board header, so the whole translation unit is compiled away in the default
build (`size -A` on `arkrtos_mini.o` shows `.text 0`) and a plain `make` passing proves nothing. A
shim header (`#include` the board header, then `#undef` + `#define ARKRTOS_USE_MINIAPP 1`, placed
first on the `-I` list with `-include arkrtos_board_config.h`) forced the switch on for a `-c` build,
which immediately reported `code_base` undeclared in three places: it used to be a local of the main
function and does not exist inside `mini_load_stream()`. Those three uses (copy target, CRC range,
end test) became `p_img->code_base`, and `entry = code_base + ...` in the entry point became
`p_img->code_base + ...`. After the fix the `MINIAPP=1` build exits 0 with **5 warnings, the same 5
as the HEAD baseline**, and `nm` shows the 18 `T` symbols.

Readings:

```
before  text 367336 / data 440 / bss 767508   sha256 b0ad6a91...   31,462,512 B
after   text 367336 / data 440 / bss 767508   sha256 b0ad6a91...   31,462,512 B
```

**The default image is byte-identical**, but not because the split left no trace: with
`ARKRTOS_USE_MINIAPP=0` the translation unit is an empty object, so editing it cannot change a single
byte of the default build. It is the same shape as `apppack` in 5.30.8 and **must not be written as an
"minus N bytes" result**. `shell_check.py` still reports `PASS`, but it never enters this path in the
default build, so that reads as "the default build was not broken" and **not** as behavioural evidence
for this split. Verdict level: compiles (including the forced-switch shim build); **behaviour not
verified**. `doc_audit.py funcs`: functions over 120 lines **29 -> 28**, and the longest is now
`arkrtos_mod_load()` (220 lines).

#### 5.30.11 Sixth one: `arkrtos_mod_load()` split by read, parse, link and commit (2026-11-18)

`arkrtos_mod_load()` in `kernelsrc/src/arkrtos_modld.c` (220 lines) is the module loader's entry
point: validate arguments, read the whole object (plus an optional signature tag), parse the ELF
header and section table, run layout / GOT / copy / relocation / entry lookup, then record the slot
and start the task. It is now five `static` helpers:

| Function | Responsibility | Lines |
|---|---|---|
| `mod_args_valid()` | every `argv[i]` non-null and shorter than `MOD_ARG_LEN`; the "default to a single argument" shape is *not* built here, because that needs an array which outlives the call, so the caller keeps it | 17 |
| `mod_read_image()` | read the whole object into `s_mod_file`; under `ARKRTOS_USE_EXEC_SIGN` verify the 32-byte trailing tag and shrink `len` back to the body length (otherwise a crafted object can make the tag look like a section header table) | 35 |
| `mod_parse_elf()` | zero the link context, fill `img` / `base` / `size` from this slot, check the ELF header (magic / class / machine / `ET_REL`), check section table bounds, find `.symtab` / `.strtab` | 67 |
| `mod_link_image()` | `mod_place` -> `mod_got_alloc` -> `mod_copy` -> `mod_got_fill` -> `mod_reloc_all` (recording the missing symbol name or bad relocation type for diagnostics) -> `mod_find_entry` | 46 |
| `mod_commit_slot()` | basename, slot name and geometry, copy `argv` into the slot's own static area and add `argv[argc] = NULL` | 35 |
| `arkrtos_mod_load()` itself | argument shape and default, clear diagnostics, pick a free slot, chain the five stages, `arkrtos_task_register` (clearing `used` on failure) | **69** (was 220) |

The helpers take a `mod_link_t *`, so `field` becomes `->field`; `mod_commit_slot()` takes it as
`const`. Equivalence was again argued with the normalised line multiset: only-OLD contains just the
33 `.` -> `->` / `&c` -> `c` rewrites, **no statement was lost**.

**This is the first split in this round with real runtime readings.** `ARKRTOS_USE_MODLOAD=1` in the
LA64 default build, and `example/qemu_loongarch64/mod_demo.c` writes the embedded `modhello.o` into
ramfs after boot and links it on the spot. The QEMU serial output shows:

```
[MOD] write /mnt/ram/modhello.o rc=0 size=6256
[MOD] ld[modhello] hello from a relocatable module
[MOD] module finished after 0 ms; try: ld /mnt/ram/modhello.o a b
[MOD] tamper check PASS (bad tag refused, rc=4294967285)
```

That covers loading, relocation, entry lookup, argument passing, normal exit and the signature-tag
rejection. `shell_check.py` also reports `PASS`, but it does not run the `mod` command itself; the
module evidence is the serial output above.

Readings:

```
before  text 367336 / data 440 / bss 767508   sha256 b0ad6a91...   31,462,512 B
after   text 367400 / data 440 / bss 767508   sha256 a5403823...   31,462,512 B
```

`text` **+64 B** (five function prologues/epilogues plus one `slot` argument that `-O2` did not
inline away); `data` / `bss` and the image length are unchanged; `arkrtos_modld.c` compiles with
**0 warnings**. `doc_audit.py funcs`: functions over 120 lines **28 -> 27**, and the longest is now
`arkrtos_loader_stream_image()` (212 lines).

## 6. LoongArch32 port (compiles, never executed)

### 6.1 No carrier on this machine

QEMU 9.2.3 ships `qemu-system-loongarch64` only. There is no `qemu-system-loongarch32`.

The only candidate is the `la32r-QEMU` fork. Its machine is `ls3a5k32`, `max_cpus=32`, ROM at `0x1c000000`, and it initializes serial with `serial_mm_init(0x1fe001e0)`. It reads time with a `rdtime.d` register pair — a 64-bit read — and it **exits with `exit(1)` if no `-drive if=sd` is present**.

§6.1.1 lists four carriers in three tiers:

| Tier | Count |
|---|---|
| Can be built here | 2 |
| Linux prebuilt binary | 1 |
| Runs natively on this machine | **0** |

§6.1.2 records the five commands that were tried locally and the verdict:

> `carrier_unavailable_on_this_machine`

The user ruled on 2026-11-17 to **close this line as-is** rather than keep shopping for a carrier. The port therefore stays at "compiles, never executed", and that classification must not be softened.

### 6.2 Toolchain

`D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-*`, with `-march=loongarch32 -mabi=ilp32d`.

Note: `-mabi=ilp32` is **not accepted**; only `ilp32d` works.

### 6.3 Trap frame: 128 bytes, 4-byte slots

| Constant | Value |
|---|---|
| `LA_FR_WORDS` | 32 |
| `LA_FR_A0` | 3 |
| `LA_FR_ERA` | 30 |
| `LA_FR_PRMD` | 31 |
| ERA offset | 120 |
| PRMD offset | 124 |

`rdtime.w` is **not recognized** by this toolchain.

### 6.4 The most expensive trap: the toolchain does not reject `.d` instructions

The LA32 assembler happily accepts 64-bit `.d`-suffixed instructions (for example `st.d`, `ld.d`, `addi.d`, `lu32i.d`, `lu52i.d`, `pcalau12i`) and emits something, even though they are not valid in the 32-bit encoding. Nothing fails at build time.

Mitigation is a written discipline, applied in four places: **only `.w` forms are used**. §7.3 gives the grep that should return zero occurrences.

Cross-reference: §6.4 and §8.4.7 both quote the disassembly line

```
21a050: 0000680c  rdtime.d $r12,$r0
```

as the concrete example of what slips through.

### 6.5 Minimum build

`52280` / `56` / `15968` (text / data / total).

`run` deliberately `exit 1` — it fails loudly rather than pretending to run, because there is no carrier.

### 6.6 Six rows carried over from la64

§6.7 tabulates six items copied from the LA64 port with the 32-bit adjustment applied (frame layout, PRMD bit order, address constants, UART, timer, linker script). This table exists so a future carrier can be brought up without re-deriving them.

### 6.7 Full-feature build

`284336` / `216` / `615956`.

Three steps: `52280` -> `213480` -> `267712` -> `284336`.

Under `-nostdlib`, littlefs needs four symbols the LA32 newlib does not provide: `__popcountsi2`, `__bswapsi2`, `strspn`, `strcspn`. They are supplied locally.

§6.8.4 is explicit: **no reading in this section was ever executed on a carrier.** §6.8.5 gives the first-run command and six groups of expected readings, recorded so that whoever brings up a carrier has something to compare against.

## 7. Reproducing these results

### 7.1 RV32

```
cd example/qemu_riscv32
make -j4
```

### 7.2 LA64

**Must run from the repository root:**

```
make -f example/qemu_loongarch64/Makefile
python tools/shell_check.py
```

The monitor session (§5.9) is typed by hand over telnet.

### 7.3 LA32

`run` only reports the missing carrier.

Full rebuild is required after any change: `rm -rf build/la32_generic`.

Then verify no 64-bit instruction slipped in:

```
grep -cE "\b(st\.d|ld\.d|addi\.d|lu32i\.d|lu52i\.d|pcalau12i)\b"
```

Expected result: **0**.

Windows quirk: file locks during parallel builds surface as `Invalid argument`.

## 8. Three-target feature matrix

### 8.1 Measured

| Target | Status | text | data | total |
|---|---|---|---|---|
| RV32 | runs | 235800 | 128 | 624132 |
| LA64 | runs | 305024 | 416 | 636436 |
| LA32 | **compiles only** | 284336 | 216 | 615956 |

All three print a `[boot]` line; the LA32 line is the only one that cannot be produced by an execution.

### 8.2 Matrix

Roughly 22 rows, each in one of three states: **executed**, **compiles**, **not done**. The matrix is the single place where the three-tier classification is applied consistently; a feature is only marked "executed" if a §-reference points at a transcript.

### 8.3 Four gaps

Four capability gaps remain and are listed with the reason each is blocked (carrier, hardware, or scope).

### 8.4 Per-switch evidence

8.4.1 through 8.4.10 record each configuration switch separately:

| § | Switch | Key reading |
|---|---|---|
| 8.4.1 | AUDIT | `checks=30/29` |
| 8.4.2 | CRASH_LOG | `.noinit` section, `journal: 0x80101E00 +512 bytes` |
| 8.4.3 | MDK_TRACE | `total=14729 lost=0 wrapped=1`; `MDK_TRACE_USE_RDTIME` |
| 8.4.4 | SECCHECK | `iso=1/2`; requires `ARKRTOS_ARCH_HAS_PAGING` |
| 8.4.5 | SOFTBUS | `checks 56 failed 0`; `ARK_BUS_FLAGS` must be set **before** `CFLAGS :=` |
| 8.4.6 | RTSTAT / SCHED_STAT | deliberately left off (cost) |
| 8.4.7 | IMAGE_SIGN | `-18` on unsigned image |
| 8.4.8 | HW_SAFE | `devs=0 owners=2` |
| 8.4.9 | KERNEL_GUARD | `[FIND] guard.watchdog` |
| 8.4.10 | KERNEL_GUARD self-audit | one genuine race found: not ISR-safe |

## 9. SMP status

### 9.1 Modeling

9.1 tabulates the three machine models (RV32 `virt`, LA64 `virt`, LA32 hypothetical) with core count and IPI modeling.

### 9.2 Current state

`USE_SMP=0`; `cpu_start` and `send_ipi` return `-1`.

- **RV32 `-smp 2`: crashes.** Serial output tears (`mepc=0x0`), `exit 124` (timeout). The second hart starts executing without the kernel having set up a per-hart stack, and both harts write the same UART.
- **LA64 `-smp 2`: 13/13 checks pass, but the second hart neither crashes nor participates.** It is parked.

### 9.3 Four gaps

Four items are listed in §9.3 with what each requires.

### 9.4 Verdict

> `smp_not_implemented_four_gaps`

Three readings that are **not** permitted:

1. It must not be read as "SMP is hard on this kernel" — the gaps are unimplemented code, not discovered obstacles.
2. It must not be read as "the LA64 result shows SMP works" — a parked hart proves nothing.
3. It must not be read as "RV32 SMP is broken" — it was never implemented; the crash is the expected consequence.

### 9.5 Why the second hart cannot be started under QEMU `-kernel`

> `smp_park_unreachable_on_qemu_kernel_boot`

QEMU's `hw/loongarch/boot.c` sets `kernel_entry` on **`first_cpu` only**. Every other CPU gets `VIRT_FLASH0_BASE` as its entry, so secondary harts never execute `start.S` at all. There is no code path in the guest that could bring them up under `-kernel` boot.

Four places were landed anyway, so that a real bootloader path would work:

1. `start.S` reads `CSR.TID` (`0x40`).
2. `cpu_id` derives from it.
3. `smp_init` parks non-zero IDs.
4. The board layer guards the block with `#ifndef`.

Size: SMP build `text 366440` (**+320 B**).

Pin **U** asserts the three instructions:

```
csrrd $t0, 0x40
beqz  $t0, 7f
idle  0
```

Cross-check: `027dcf53...6114b`, 460264 B, byte-identical across the three trees.
