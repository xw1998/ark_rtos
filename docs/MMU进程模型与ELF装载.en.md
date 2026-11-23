# Running executables like Linux: ELF64 (MMU profile, M1) and ELF32 (PMP profile, U-4)

**[简体中文](MMU进程模型与ELF装载.md) | English**

> Condensed English edition. The Chinese page (MMU进程模型与ELF装载.md) is
> normative: every number below is copied from it as of 2026-11-18, but if the two
> disagree, the Chinese page wins. This page adds no new claim and no new
> measurement.

## 0. In one sentence

The MMU backend (page-table backend; measured on LA64, LA32 shares the arithmetic)
can now run

```
mark@arkrtos:~$ exec /mnt/ram/hello one two
mark@arkrtos:~$ /mnt/ram/hello direct          # typing the path also works
```

- and what runs is a **native ELF64 executable**, not something first converted into a
  bare image. That is exactly Linux's own executable format (Linux's `./prog` is also
  `PT_LOAD` plus a jump to `e_entry`).

M1 delivered "**the file is the program**" + the Linux entry convention
(`sp -> argc/argv`) + a single instance; it did **not** deliver "a process". Two later
rounds filled two of those cells: U-1 gave each app its own address space (space number
= frame number, VA always `0x02000000`), U-2 allowed **two instances concurrently** at the
same VA (loader keeps a per-space ledger, defers the window switch; both QEMU LA64
runs). U-3's three stages (page frame pool -> page-level window -> on-demand faults),
space release (`arkrtos_paging_space_release`, drop window entries first, then return
frames), `copy_from_user`, the exit code channel and page replacement (U-16) are also
done; `wait <task id>` and `main` return value handling landed on 2026-11-17 as well
(shell-side 20 ms polling plus `arkrtos_app_exit` in `app_sdk` - **not** POSIX `wait()`:
no parent/child, no zombie queue). ASID (U-7) landed the same day
(`ARKRTOS_USE_PAGING_ASID`, default off; when on each space gets one ASID and one
dedicated STLB way, and a space switch writes `CSR.ASID` once).
**Do not describe "can run two instances concurrently" as "Ark has a full process
model".**

Scope pinned:

* verified (64-bit): `qemu_loongarch64` (LA64, page-table backend + PLV3 window), QEMU
  runs, see §5;
* verified (32-bit): `qemu_riscv32` (RV32IMAC, **no MMU**, PMP + U mode + a separate
  executable RAM window), QEMU runs, see §9 (U-4, 2026-11-17). The same TU picks the
  ELF32/ELF64 field table from the port's `long` width - it is no longer "64-bit only";
* same origin, never run: `la32_generic` (LA32, 128 B trap frame, loader arithmetic
  sharing RV32; there is no execution carrier - verdict
  `carrier_unavailable_on_this_machine`). The `app run` fixed-window path and ELF
  loading are two different paths; do not mix them.

## 1. "Running ELF" and "running an executable directly" do not contradict

The review question was "with an MMU you should not run ELF, it should just be an
executable, right". Two layers must be separated:

1. **File format layer**: a Linux executable *is* ELF. What `execve()` does is read the
   ELF header, map the `PT_LOAD` segments into the new address space, place
   `argc/argv/envp` on top of the stack, and jump to `e_entry`. So "run an executable
   directly" = "load ELF". There is no intermediate format: `/mnt/ram/hello` is the ELF64
   produced by `loongarch64-newlib-elf-gcc` directly, with no conversion product on disk.
2. **Process model layer**: what Linux feels like is not the file format but **a separate
   address space per process**. M1 puts segments into **one single 2 MB PLV3 static
   window** (`0x02000000`), a single instance, and the program must link at
   `-Ttext=0x02000000`. So M1 is "the file is the program", **not** "there is a
   process". After U-1/U-2 each space has its own frame and two instances can run
   concurrently, but it is still a static window at a fixed base, not an on-demand
   mapped process. The whole gap is listed in §6.

One-line answer: **in format we run exactly an executable (ELF); the gap is address
space, not format.**

## 2. Switch, window, mutual exclusion

| Item | Value | Where |
|---|---|---|
| Switch | `ARKRTOS_USE_ELF` (Kconfig symbol, default `n`) | `config/kconfig/Kconfig.features` |
| Dependencies | `ARKRTOS_USE_VFS=1` and `ARKRTOS_USE_PRIV=1`, otherwise `#error` | `arkrtos_elf.c` |
| Window | `ARKRTOS_ELF_WINDOW_BASE` / `_SIZE`, either missing is `#error` | board config file |
| LA64 QEMU values | `0x02000000` / 2 MB (= `ARKRTOS_QEMU_APP_BASE` / `APP_SIZE`) | `board/qemu_loongarch64/arkrtos_board_config.h` |
| Wide-VA profile (2026-11-20) | `make ... WIN_VA=0x120000000` overrides `ARKRTOS_ELF_WINDOW_BASE` (the board header keeps an `#ifndef` fallback) and lifts the kernel link base and the fixture bases with it; without `WIN_VA` the image and behaviour are byte-identical to before | `example/qemu_loongarch64/Makefile` |

When off: `arkrtos_elf.c` compiles to an empty object (the whole file is inside
`#if (ARKRTOS_USE_ELF == 1)`), every interface in `arkrtos_elf.h` returns `-1`/`0`, and
neither the `exec` command nor the `/path/prog` fallback is registered. **Zero change in
size and behaviour.**

### 2.1 Mutual exclusion with the PLV3 probe task (a stated trade-off)

The load window and the "PLV3 privilege-isolation probe" use **the same 2 MB**. They cannot
run together:

* with ELF on, the example does not register the probe task and **says why in the boot
  log**: `[PRIV] PLV3 probe task skipped: ARKRTOS_USE_ELF owns the app window`;
* conversely, with `ARKRTOS_USE_ELF=0` the probe runs as before (`main.c` decides through
  the single macro `ARK_EXAMPLE_PLV3_PROBE`, so the two judgements cannot drift apart).

This is a **debatable trade-off**: the probe proves "page table + PLV really block", the
ELF demo proves "a file can become a program". Once M2 delivers per-process page tables
both can be present (probe in one process, ELF program in another).

## 3. Load path (two passes, no new memory allocated)

`arkrtos_elf_exec(path, argc, argv)`:

1. **Read the header**: `vfs_read_at` reads the 64 B `Ehdr`; `elf_check_header()` checks
   magic / `EI_CLASS=2` (64-bit) / `EI_DATA=1` (little endian) / `e_machine=258`
   (LoongArch) / `e_type=2` (`ET_EXEC`) / `e_phentsize=56` / `1<=e_phnum<=16`. Any
   `PT_INTERP` (dynamic linking) is `ERR_FORMAT` in the default build - M1 does not
   support an interpreter chain. With `ARKRTOS_USE_DYNLOAD` on, the loader takes the
   interpreter path instead (2026-11-20, QEMU LA64: two `PASS` runs + `exit 105`,
   broken fixture `exit 127`); see
   [动态链接与PT_INTERP](动态链接与PT_INTERP.en.md).
2. **Pass one: validate only, write nothing.** For each `PT_LOAD` check `p_memsz>=p_filesz`,
   that `[p_vaddr, p_vaddr+p_memsz)` lies wholly inside the window, and at most 16
   segments. The point is **all or nothing** (the window may still hold the previous
   program's remains; a half-installed program is worse than a refused one).
3. **Pass two: actually write.** `vfs_read_at` copies `p_filesz` per segment, then zeroes
   the `p_memsz-p_filesz` range (`.bss`).
4. **Build the stack and argv** (all at the top of the window, kernel heap untouched):
   write argument strings downward from `win_top` -> record `addrs[]` -> build the
   `argc / argv[argc] / envp[0]=0` array -> `sp = argv_start`. String budget
   `ARKRTOS_ELF_ARGV_SIZE = 16 KB`, at most `MAX_ARGS=16` arguments of at most
   `MAX_ARG_LEN=256`.
5. **Start the task**: `stack_bottom = argv_start - 32 KB`,
   `arkrtos_task_register(entry, stack_bottom, 32 KB, priority 12, 0)`, then set
   `is_priv` to 0 (unprivileged) - **an ELF program always runs at PLV3**; it writes its
   own `.data`/stack inside the window and faults on the TLB outside it.
6. **One instance per space (U-2, 2026-11-17)**: the ledger is
   `s_elf_task[ARKRTOS_PAGING_APPS_MAX]` + `s_elf_info[]`, read via `elf_slot_live(space)`;
   choosing a space looks only at the ledger, never at addresses (the window VA is the same
   value in every space and carries no identity). Only when all spaces are taken does it
   return `ERR_BUSY`; on exit (dead-task detection) it clears only its own cell. M1's old
   formulation was "`s_elf_task != 0` means `ERR_BUSY`", i.e. one instance globally - U-2
   replaced that. In the paging profile segments are no longer `memcpy`ed straight into a
   window pointer: they are read into the kernel staging buffer `s_elf_stage[4096]`, then
   the window is switched and each 4 KB is copied in.

### 3.1 Entry convention: `sp` points at `argc`

Structurally the same as Linux:

```
sp -> [ argc ][ argv[0] ][ argv[1] ]..[ 0 ][ envp[0]=0 ]
```

**Pitfall (must be in every program author's head)**: `_start` cannot be an ordinary C
function reading `sp`, because the compiler prologue does `addi.d $sp,$sp,-128` first.
The shim `hello.c` uses is:

```c
__asm__(".section .text._start,\"ax\"\n.globl _start\n.type _start, @function\n"
        "_start:\n  move $a0, $sp\n  bl hello_main\n1:\n  b 1b\n"
        ".size _start, .-_start\n.previous\n");
```

Without it the program receives `top-0x80` and reads `argc` as 0.

### 3.2 Talking to the kernel: the SVC argument block is 32-bit words

The kernel's SVC protocol is **32-bit words** (all `ARKRTOS_SVC_ARG/RET/NUM` are
`uint32`), so a 64-bit program must use `uint32 params[N]` and **truncate pointers to
32 bits** (window and stack are both in the low 4 GB, so truncation is safe - but that is
not "64-bit pointers may be passed freely"). A real pitfall: passing a device name through
`unsigned long params[4]`, which the kernel interprets as `uint32*`, so `p[1]` read the
high half (0) and `open("ttyS0")` returned `-1`. After switching to `uint32`,
`dev_open rc=18874368 (0x01200000)`.

Services used: `ARKRTOS_SVC_DEV_IO(0x10)` `OPEN/READ/WRITE/CTRL/CLOSE`, and
`ARKRTOS_SVC_THREAD_CTRL(0x1B)` `CREATE/SELF/EXIT` (`EXIT` without a code).

## 4. Two entries on the shell

| Form | Behaviour |
|---|---|
| `exec <path> [args...]` | load and **wait for it to finish** (cap `SHELL_EXEC_WAIT_MS=30 s`, polling every 20 ms), not Linux's "replace myself" |
| typing `/path/prog [args]` | when the command table misses, `ark_shell_eval.c` falls back to `exec`; only `/x`, `./x`, `../x` forms are accepted, anything else is "unknown command" |

Both print the same load summary:

```
[ELF] /mnt/ram/hello: 1732 B, 2 segment(s), entry 0x02000000, sp 0x021FC000, argc 3, task 7
[ELF] /mnt/ram/hello: finished after 0 ms
```

`1732 B` is the **sum of `p_filesz`**, not the file length (a 6344 B file includes
alignment before symbols, section headers, `.comment` and other non-loaded content). This
difference was once misread as "the loader missed 4 KB", so it is pinned here: **use `ls`
for file length, this line for load size.**

## 5. Measurements (QEMU la64, 2026-11-16)

### 5.1 All three entries work

```
[ELF] demo task id=4 (writes /mnt/ram/hello and runs it)
[ELF] write /mnt/ram/hello rc=0 size=6344
...
mark@arkrtos:~$ [ELF] exec rc=0
[hello] entry sp=35635200 argc=3          <- 35635200 = 0x021FC000
[hello] hello from an ELF64 program
[hello] argc=3
[hello] argv[0]=hello
[hello] argv[1]=world
[hello] argv[2]=42
[hello] bye
TASK:4599 task 7 exits (self kill)
[ELF] demo program finished after 20 ms; try: exec /mnt/ram/hello a b c
```

`exec /mnt/ram/hello one two` and typing `/mnt/ram/hello direct` each ran once with the
output above and argv `one/two` and `direct`. Another round of 10 consecutive `exec`
calls: 11 "task exits" (including the demo task's own run), 11 `bye`, zero `FATAL`.

### 5.2 Debugging record: the 2nd `exec` always crashed with `KERNEL refill` - root cause was the soft-timer stack

This stays in the document because it looks like a loader bug and has nothing to do with
the loader.

**Symptom** (deterministic): idle for 45 s, no crash; run
`exec /mnt/ram/hello` through the shell and the **2nd** call always crashed (the 1st being
the demo task's own run):

```
TASK:4599 task 7 exits (self kill)
[LA64U] KERNEL refill (plv0) badv=0x01000000 era=0x0020b9d8 -> fatal
[Ark RTOS] FATAL: estat=0x000b0800 ecode=0x0000000b era=0x02000558 ...
```

**Localisation, ordered by decisiveness (the first two are negative evidence):**

1. `ecode=0x0b/era=0x02000558` is the **stale ESTAT from the previous `syscall`** (this
   port treats 0xB as SYS, matching QEMU's `EXCCODE_SYS`), not the crash site; a TLB
   refill only updates `TLBRERA/TLBRBADV/TLBRPRMD`. The real site is
   `badv=0x01000000 era=0x0020b9d8`.
2. Symbolise `era`: `0x0020b9d8` is inside the lookup loop of `hwsafe_find_fn`, whose
   bound is `g_hwsafe_dev_n`. Reading it gave `dev_n = 2210696 (0x0021bca8)` - **a code
   address** where the normal value is 0 (`hwsafe: devs=0`). So the loop walked into the
   unmapped `0x01000000`.
3. `0x0021bca8` is inside `mdk_trace_event`. Who wrote a **code address** into an `int32`
   counter? Looking at `.bss`: `g_hwsafe_dev_n` is at `0x304BA8`, exactly the address of
   `g_hwsafe_dev[-1].fn`, and exactly 136 B **below** `arkrtos_timer_task_stack`
   (`0x304C30`, 96 words = 384 B).
4. `task` reported task 5 (`prio=200` = the soft-timer service task) at
   `peak/low = 0x304C60`, i.e. 336 B used, 48 B left. The moment it was scheduled back
   in, the first tick interrupt wrote a **256 B trap frame to `sp-256 = 0x304B60`**, and
   word 18 of that frame landed exactly on `0x304BA8`.

**Root cause**: on LA64 the trap frame is pushed onto the interrupted task's own stack,
256 B per frame; the soft-timer service task's stack was only 384 B with 336 B already
used, so one frame ate through it and overflowed 208 B downward, overwriting
`g_hwsafe_dev_n` with one of its own saved return addresses. The crash only showed up on
the **next** call to `hwsafe_find_fn` (which the ELF program's `open("ttyS0")` triggers) -
hence "crashes on the 2nd exec".

**Verification and fix**:

* counter-check: enlarging that stack 8x (768 words) gave six consecutive `exec` runs
  without a crash (96 words always crashed);
* fix: the board configs `board/qemu_loongarch64/arkrtos_board_config.h` and
  `board/la32_generic/arkrtos_board_config.h` override
  `ARKRTOS_TIMER_TASK_STACK_WORDS` (LA64 = 512 words = 2 KB, LA32 = 256 words = 1 KB),
  with the criterion "**peak + one whole trap frame + the four-level handler chain
  (4x128 B)** still leaves headroom";
* after the fix: 10 consecutive `exec` plus typing the path all pass, and
  `hwsafe: devs=0 owners=2` is no longer corrupted (it had been
  `devs=2210696` in every earlier run).

**Reusable conclusion**: in this port, **any small stack that can run user callbacks must
be sized as "peak + trap frame + handler chain"**, not as "this task only calls two or
three functions". The 96-word default in `arkrtos_config.h` was for 32-bit ports
(128 B frames); 64-bit ports always need more.

### 5.3 Signature gate (`ARKRTOS_USE_EXEC_SIGN`, 2026-11-17)

`exec` is "file is code": whoever can write a file into `/mnt/ram` can start a task at
PLV3. `ARKRTOS_USE_IMAGE_SIGN` covers only the app install/upgrade path, not this one. The
added switch is `ARKRTOS_USE_EXEC_SIGN` (`depends on IMAGE_SIGN`, default **n**):

* format: **file plus a 32-byte HMAC-SHA256 tag at the end**, tag covering `[0, len-32)`.
  ELF reads segments by `p_offset`, and the trailing 32 bytes are in no segment, so they
  naturally do not participate in loading - the existing format does not change at all.
* the check sits **before the ELF header is read** (after `arkrtos_elf_exec`'s argument
  checks): a format error must not paper over "this file is unsigned". To avoid reading
  the whole image into RAM it streams through
  `arkrtos_sign_payload_begin/update/end`, 512 B at a time.
* file length comes from the new facade `arkrtos_vfs_size()`: the return of `lseek(END)`
  differs between the ramfs and the littlefs bridge (new offset vs 0), and using it as a
  length silently drifts.
* failure code `ARKRTOS_ELF_ERR_SIGN (-8)`, whose text names `tools/sign_payload.py`.

QEMU LA64 measurement (2026-11-17, one boot):

```text
[ELF] write /mnt/ram/hello rc=0 size=6376           <- 6344 B body + 32 B tag
[ELF] exec rc=0                                     <- signed version starts normally
[ELF] tamper check PASS (bad tag refused, rc=-8)    <- refused after flipping the last tag byte
```

The negative file is not "a missing file" or "a malformed file": it has the same content
as the positive one with the last tag byte flipped, generated by
`tools/sign_payload.py tamper` and compiled into the image alongside the positive file
(`elf_demo.c` `exec`s it once per boot). That way "the signature gate is open" is actually
exercised, not just a sentence in a document.

The host side is `tools/payload_sign_gate.py`: it computes the MAC with the **real**
`arkrtos_sign.c` on the host and compares byte for byte with `tools/sign_payload.py`
(hashlib), for both the one-shot and streaming entries; it also checks "sign is
idempotent / tamper must be refused / wiring exists when on, zero symbols when off /
`EXEC_SIGN=1 + IMAGE_SIGN=0` must fail at compile time". It is part of `ci_gate`.

Boundary: this is a **symmetric** scheme (one pre-shared key), so anyone who can read the
device key can sign new programs; a trust chain where the device holds only a public key
is out of scope for this round.

## 6. Gap to Linux (M2 debt table)

The Chinese page keeps the full table, with per-item status and where each reading lives.
Condensed, as of 2026-11-18:

| # | Gap | Status |
|---|---|---|
| U-1 | per-process address space | **mechanism done (2026-11-17)**: one app-window entry (PS=20) per space, `arkrtos_mpu_set_app()` rewrites it, space number = frame number, VA always `0x02000000`; still missing a page-frame allocator (U-3 done later, see below) and on-demand faults (U-5, also done later) |
| U-2 | multiple instances at the same VA | **done (2026-11-17)**: per-space ledger, deferred window switch, 4 KB staging; root cause of the crash found mid-round was the trap/switch frame living on the outgoing task's own stack inside the window VA (`era=0xcdcdcdcd`); reading `pmap u2: PASS - two instances, two spaces, neither frame overwritten` |
| U-3 | page-frame allocator + page-level window + on-demand faults | **all three stages done (2026-11-17)**: frame pool `ARKRTOS_QEMU_FRAME_BASE`/`COUNT` (8 MB, 256 B bitmap), 512 frames per space laid out as 256 `PS=12` entries, on-demand faults through `TLBRENTRY`; space release `arkrtos_paging_space_release()` returns frames (drop window entries first, always) |
| U-3a | runtime dynamic mapping (register an MMIO span) | **done (2026-11-17)**: kernel PLV0 only, 4 KB, at most 8 page pairs; app-visible dynamic mapping is still U-1 |
| U-4 | the loader was 64-bit only | **fixed on 2026-11-17** by the 32/64 field table (§9) |
| U-5 | page-fault handling (on-demand paging in) | **done (2026-11-17)**: window empty after `activate`, a fault fills one 8 KB pair through `TLBRENTRY` and `ertn`s back to retry; no PLV distinction, no frame return; page replacement only takes over when `ARKRTOS_USE_PAGING_SWAP` is on |
| U-6 | controlled kernel access to user memory | **done (2026-11-17)**: `copy_from_user`/`copy_to_user` with four refusal codes; the SVC handler still uses its original boolean checks |
| U-7 | ASID / batched TLB invalidation | **ASID profile done (2026-11-17)**, default 0; measured `pmap look 0x02000000` hits `entry=0` in space 0 and `entry=256` in space 1; steady-state switch 34.35 us with readback verification; TLB shootdown for SMP still not done |
| U-8 | exit code channel | **done (2026-11-17)**: `THREAD_CTRL` sub 4, code stored in the TCB, taken by the loader when it observes the task turn `INVALID`; `wait <task id>` polls on the shell side; default off, RV32 image byte-identical when off |
| U-9 | `fork` / a real self-replacing `exec` | **`execve` done (2026-11-21)** as an in-place image swap of the calling task, and **`fork`/`clone` + `wait4` done (2026-11-23, QEMU LA64, `ARKRTOS_USE_LINUX_FORK` default off)**: the parent address space is copied 8 KB pair by pair, the parent trap frame is copied with `a0` zeroed, so the child reads 0 and the parent reads the child id. 21 `[fork]` checks (including a blocking `wait4` that really waited >= 40 ms, the parent read end still reading 3 bytes after the child closed its own copy, and a cross-task `SIGCHLD` really running) + `exit code 0`; a real defect was fixed on the way (frame-address equality cannot mean "no switch" once parent and child share VAs - see [Linux ABI](Linux可执行文件兼容-系统调用ABI.en.md) section 8) | fork only, no threads (`CLONE_VM`/`CLONE_THREAD` are `-EINVAL`); per-process fd tables with inheritance, cross-task signals and a blocking `wait4` (restartable syscall) all have QEMU readouts. **Copy-on-write is done too (2026-11-23, `ARKRTOS_USE_PAGING_COW` default off, 26 checks under QEMU LA64)**: `space_clone()` moves no bytes, it only points the child at every resident 8 KB pair (refcount 1->2, parent entries dropped to read-only), and the first write copies. **Stack pages are forced private** - the trap frame lives on the App's own stack (`eentry` starts with `sp -= 256`), so a read-only stack pair turns the PLV0 frame push into a nested PME (measured: machine stops). Without swap, COW is only deferred copy and 2 address spaces x 2 MB are still bounded by the frame pool. **Anonymous `MAP_SHARED` is done too (2026-11-23, `ARKRTOS_USE_PAGING_SHM` default off, must sit on the COW tier, 8 of the 34 QEMU LA64 checks: built and zeroed at mmap time, fork only bumps the refcount, parent and child see each other's writes)** - see [Linux ABI](Linux可执行文件兼容-系统调用ABI.en.md) section 8.3. No threads yet | M3 (partial: execve, fork+wait4, fd inheritance, cross-task signals and COW done; threads still owed) |
| U-10 | dynamic linking (`PT_INTERP`, `ld.so`) | **done (2026-11-20, QEMU LA64)**: with `ARKRTOS_USE_DYNLOAD` on, the kernel loads the interpreter the Linux way (both `ET_EXEC` and `ET_DYN` mains), lays down 9 auxv entries and enters at the interpreter; ships a minimal in-tree `ld-ark.so` (`ET_EXEC` pinned at `0x02100000`, three relocation kinds). Two `PASS` runs + `exit 105`; broken fixture `exit 127`. Default build still `ERR_FORMAT` (checked before the PT_LOAD scan). **Still cannot run** distro binaries: `PT_INTERP` names glibc's `ld-linux-loongarch-lp64d.so.1`, which is not in the tree, and glibc also needs rseq/TLS/file mappings plus the rest of futex (WAIT/WAKE exist behind a switch, default off) | scope is one-level dependencies and immediate relocation only: no lazy binding, no `.init_array`, `mprotect` has page-level write protection behind `ARKRTOS_USE_PAGING_MPROTECT` (4 KB granularity, section 8.6 of the Linux ABI page) and is accepted-not-enforced (one RWX window) when off, `ld.so` is not signed, and the libc surface is 6 syscalls | M3 (partial) |
| U-11 | fd inheritance / pipes / signals | **pipes done (2026-11-21, QEMU LA64)**: `pipe2` (59) - ring buffer with per-end reference counts, `read`/`write`/`close` dispatch, survives `execve`; deliberately **non-blocking** (empty read `-EAGAIN`, full buffer short write, write with all readers closed `-EPIPE`, `O_CLOEXEC` `-EINVAL`) because pipe I/O deliberately never sleeps (in the fork tier each process owns its fd table, so the two ends can really belong to parent and child, but "sleep until readable" is not wired). 14 checks + `exit code 55`; two mutations verified and reverted. **fd inheritance done** (fork tier: `clone` copies the whole table, pipe reference counts +1, the child's close only drops its own); **cross-task signal delivery done** (2026-11-23) | the non-blocking pipe contract differs from Linux (256 B buffer, max 2 pipes, 8 fd slots) | M3 (partial: pipe2, signals and fd inheritance accepted) |
| U-12 | 32-bit side review of the soft-timer stack | **measured and fixed (2026-11-17, QEMU RV32)**: 96 words = 384 B used 372 B (fill scan, 96%), leaving 12 B, and the RV32 trap frame is 128 B, so any interrupt landing there walks out; RV32 board raised to 256 words (1 KB), same criterion now 372/1024 = 36%, `utest` 13/13 unchanged, text unchanged (`.bss` only). **Also corrects a reading**: the old "peak 184 B, 200 B left" was the psp single path (47%); only the fill scan gives 372 |
| U-13 | stack overflow / trap-frame overrun detection | **done (2026-11-17)**: `ARKRTOS_USE_STACK_GUARD` (default n) checks that `stack_bottom[0]` is still `0xed01` (not a PSP comparison, because a trap frame or wild pointer moves the stack bottom while PSP stays above) |
| U-14 | 32-bit side review of the console task stack | **RV32 measured (2026-11-17), but LA64's use case does not exist there**: RV32's `help` has no `ld`/`modules`/`lkup` (`modld` is refused on 32-bit by a 64-bit guard), so "3 KB used to 100% by `ld`" cannot be reproduced. Only the already-run command set is measured: `help` + `fs ls /` peak 2120 B (69%), `utest run` + `sec all` + `audit` + `dmesg` peak 2172 B (70%). **This is not proof that 3 KB suffices on RV32**, only that these commands do not overrun. LA32/STM32 still unmeasured |
| U-15 | unmap / release mappings | **done (2026-11-17)**: `arkrtos_paging_unmap_span(va, size)` with refusal codes -1 unaligned / -2 static span / -4 never registered; measurement **overturned the original design** - rewriting the MTLB alone is not enough (STLB and QEMU softmmu keep the old translation, so `look` says no entry while reads still return the old page); the fix is `invtlb` both halves first, then `tlbwr` |
| U-16 | page replacement | **done (2026-11-17)**, `ARKRTOS_USE_PAGING_SWAP`, default 0: with an empty pool, evict one 8 KB pair and retry |

| U-17 | PT_TLS (static thread pointer) | **static path done (2026-11-20, QEMU LA64)**: `ARKRTOS_USE_ELF_TLS` (default 0; a non-LoongArch port turns it on as a compile-time `#error`). `elf_measure_image()` reads `p_type=7` and validates it (`filesz<=memsz`, alignment, a second `PT_TLS` refused), records `tls_va = bias+p_vaddr`, and extends the window limit `hi` to the TLS end (`.tbss` usually sits past the last `PT_LOAD`); `elf_write_tls()` copies `filesz` bytes and zeroes `memsz-filesz`; `elf_start_task()` takes a `tp` and writes it after the privilege drop via `arkrtos_port_task_set_tp()` -> `frame[LA_FR_TP]` (frame offset 8 B = `$r2`; `.S` already restores `$tp`). Convention measured, not guessed: two independent toolchains both emit `add.d $r12,$r12,$r2` with TPREL 0/4, so **TP = p_vaddr + bias**. Missing: TCB layout, `set_tid_address`, general-dynamic TLS (`DTPMOD`/`DTPREL`) and patching local-exec immediates inside libraries; only LA64 has a set-tp primitive. The kernel loader does not apply `R_LARCH_TLS_TPREL64` (17 in the real sample) - that tier is done in user space by the in-tree `ld.so` (2026-11-20, commit `966e7df`, see [Dynamic linking and PT_INTERP](动态链接与PT_INTERP.en.md) step 7) | static part accepted, see §10; gate = `tools/elf_gate.py` H1/H2/H3 |
| U-18 | Loader address width / wide VA window | **Done (2026-11-20, QEMU LA64)**: the whole chain uses `unsigned long` (no more `p_vaddr > 0xFFFFFFFF` judgement, 64-bit stack alignment in `elf_build_stack()`, 64-bit `pc` in both port `*_syscall_num()`); the window VA is decoupled from physical frames and `WIN_VA=` lifts window base, kernel link base and fixture bases together. Readings on `WIN_VA=0x120000000`: `linux_hello` (Linux ABI, pointers in 64-bit registers) `[linux] PASS` + `brk(+4096) -> 4832890880` + `exit code 7`, `[MOD]` / `[FS]` PASS, no fatal | the native SVC parameter block is **now covered** (2026-11-21, QEMU LA64): switch `ARKRTOS_USE_WIDE_SVC_ARGS` (default 0) widens each slot to `unsigned long`, the subcommand number is tagged with `ARKRTOS_SVC_SUB_WORD` (`0x80000000UL`) in the high bit of `p[0]`, and all 30 kernel `svc_*()` handlers validate "block in user RAM and marker set" via `SVC_WIDE_GUARD()`. Wide-build reading: with `[hello] entry sp=4833918976` (= `0x1201FC000`, > 4 GB) hello prints its 4 lines and exits with `exit code = argc`, while the default build keeps its 31 signal checks and 8 `PASS` results unchanged; both mixed builds fail (wide kernel + narrow App prints the one-shot `wide-args kernel got a 32-bit param block; ...` log and rejects the call). **Still missing**: per-process VA space (still a single window) and runtime page-table construction; `kernelsrc/sdk/mini_sdk/arkc/` remains `u32`-based (outside this mechanism under wide VA); the SVC return register is only 32 bits, so in the wide build a mempool block above 4 GB yields 0 rather than a truncated address | address width accepted (QEMU LA64, see §11; gates = `tools/elf_gate.py` / `tools/ci_gate.py`) |

## 7. Reproduction

```bash
cd "/d/工作/git_project/ark rtos"

# 1) build LA64 (the Makefile compiles hello.c into ELF64 and then into a C array)
make -f example/qemu_loongarch64/Makefile -j8
ls -l build/qemu_loongarch64/ark_qemu_la64.elf

# 2) run QEMU feeding commands through a pipe (QEMU has no host file injection;
#    hello.elf is turned into an array by elf_blob.py, compiled in, and written to ramfs
#    by the demo task)
python - <<'PY'
import subprocess, time
qemu = r"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe"
p = subprocess.Popen([qemu, "-machine", "virt", "-kernel",
                      "build/qemu_loongarch64/ark_qemu_la64.elf", "-nographic"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
time.sleep(9)
for c in ["exec /mnt/ram/hello one two\n",
          "exec /mnt/ram/hello a0 b\n",
          "exec /mnt/ram/hello a1 b\n",
          "/mnt/ram/hello direct\n",
          "hwsafe\n"]:
    p.stdin.write(c); p.stdin.flush(); time.sleep(2.5)
p.stdin.write("\x01x"); p.stdin.flush()
print(p.communicate(timeout=10)[0])
PY

# 3) criteria (all of them)
#    - every exec prints [hello] ... bye and [ELF] ... finished
#    - no "[LA64U] KERNEL refill" / "FATAL" anywhere
#    - the final hwsafe reports "devs=0 owners=N", dev_n is not an astronomical number
```

Loader gate and the full suite:

```bash
python tools/elf_gate.py      # default profile unchanged / LA64 builds positively /
                              # bad machine, bad type, missing window must all be refused
python tools/ci_gate.py       # judge by the last line; includes paging / elf / modld /
                              # payload-sign / stack-guard groups
```

## 8. Writing a program that runs (minimum checklist)

1. link at the window base:
   `-Ttext=0x02000000 -e _start -Wl,-z,max-page-size=4096 --strip-all`;
2. pass `sp` into C with the assembly shim in §3.1, so the compiler prologue does not move
   `sp` first;
3. use a `uint32` argument block for SVC (§3.2), truncating pointers to 32 bits;
4. exit with `ARKRTOS_SVC_THREAD_CTRL`'s `EXIT` (no code);
5. **do not** assume the program is PLV0: it runs at PLV3, and touching an address
   outside the window is stopped by the TLB immediately.

`example/qemu_loongarch64/elf/hello.c` is a compilable template (including the
`elf_blob.py` packaging step).

## 9. ELF32 on RV32 (no MMU): U-4 (2026-11-17, QEMU runs)

### 9.1 Why ELF works without an MMU

"Running ELF" needs **addresses fixed at load time** and **the program unable to obtain
kernel addresses**, and PMP provides both: the board carves out a 1 MB executable RAM
window (`0x80A00000`, 1 MB aligned - a PMP region must be a power of two and aligned to
itself), the loader writes the `PT_LOAD` segments into it, points both task windows at
it, and sets `is_priv` to 0. A U-mode task then sees only that block; any other address
is an `mcause=5/1` access exception. **It has no address-space isolation (all instances
share the same window); what it has is window isolation** - do not describe this reading
as "RV32 also has a process model".

### 9.2 One loader for 32 and 64 bit

Every field read in `arkrtos_elf.c` goes through `elf_word_t`, with widths and offsets
chosen by a macro table selected from `__SIZEOF_LONG__`:

| Constant | 64-bit (LA64) | 32-bit (RV32) |
|---|---|---|
| `ELF_EHDR_SIZE` / `ELF_PHDR_SIZE` | 64 / 56 | **52 / 32** |
| `ELF_EH_PHOFF` / `_PHENTSIZE` / `_PHNUM` | 32 / 54 / 56 | **28 / 42 / 44** |
| `ELF_PH_OFFSET` / `_VADDR` / `_FILESZ` / `_MEMSZ` | 8 / 16 / 32 / 40 | **4 / 8 / 16 / 20** |
| expected class / machine | `ELFCLASS64` / 258 (LOONGARCH) | **`ELFCLASS32` / 243 (RISCV)** |

A class mismatch with the port width is `ERR_FORMAT` (a 64-bit port loading ELF32 would
read `argc/argv` off the stack in 8-byte units - a silent wrong answer). The G pin in
`tools/elf_gate.py` pins these 11 offsets one by one.

### 9.3 Three pitfalls that only appear on RV32

1. **`-Ttext` pushes the first LOAD segment back one page**: linking at `0x80A00000`
   produced a LOAD at `0x809FF000` in `readelf`, and the loader then reported "segment
   outside the executable window" - looks like a loader bug, actually the link base was one
   page short. Using **`-Wl,-Ttext-segment=0x80A00000`** put LOAD and `.text` both at
   `0x80a00000`. `ELF_WIN` in the Makefile and `ARKRTOS_ELF_WINDOW_BASE` in the board
   config must be equal; the G pin reconciles them.
2. **The sample program's exit-code sub-command may not be compiled in**:
   `ARKRTOS_THREAD_SUB_EXIT_CODE` (sub 4) sits inside
   `#if (ARKRTOS_USE_EXIT_CODE == 1)`, and the RV32 default profile is 0, so sub 4 fell to
   `default` and returned -1; the program dropped into `for(;;)`, and since **an ELF task's
   priority is not below the shell's, the shell never got the CPU back** (symptom: after
   `exec` no shell output at all, not even "finished";
   `arkrtos_elf_exec_space()` stopped before returning). Fix: after sub 4 the sample also
   sends **sub 3 `ARKRTOS_THREAD_SUB_EXIT`** as a fallback - sub 3 always exists in the ABI.
3. **A setup race**: `arkrtos_task_register()` already links the task into the ready set
   while "drop privilege" and "write the MPU window" happen after it. Being switched away
   mid-way starts the task half configured: still in M mode it runs anyway, but once
   demoted with the window not yet written it faults with
   `mcause=1 mtval=0x02000000 from=M` - which shows up as **intermittent failure**
   (2 of 5 consecutive runs failed). Fix: wrap register -> demote -> MPU writeback in
   `ARKRTOS_ENTER_CRITICAL()/ARKRTOS_EXIT_CRITICAL()`.

### 9.4 Readings (5 consecutive runs, all identical)

```
mark@arkrtos:~$ exec /mnt/ram/hello32 a b c
[hello32] entry sp=-2135965696 argc=4          # = 0x80AFC000, top of the window
[hello32] hello from an ELF32 program
[hello32] argc=4
[hello32] argv[0]=/mnt/ram/hello32   argv[1]=a   argv[2]=b   argv[3]=c
[hello32] bye, exit code = argc
TASK:4717 task 8 exits (self kill)
[ELF] /mnt/ram/hello32: 1081 B, 2 segment(s), entry 0x80A00074, sp 0x80AFC000, argc 4, task 8, space 0
[ELF] /mnt/ram/hello32: finished after 0 ms (space 0)
```

The same log holds **expected** negative controls: `mcause=5 mtval=0x02000000 from=U
task=3` (a U-mode read of CLINT stopped by PMP) and `mcause=2 mtval=0x30046073` (a U-mode
write to mstatus refused) - isolation is still on, it is not "protection switched off
because it runs".

### 9.5 Default profile unchanged

With `ARKRTOS_USE_ELF=0`: `arkrtos_elf.c` compiles to an empty object; the board's two
`ARKRTOS_MPU_ELF_WINDOW_*` aliases are hidden by
`#if defined(ARKRTOS_USE_ELF) && (...==1)` (without that guard the window branch in
`mpu.c` compiles into the default image, measured +564 B); `arkrtos_mpu_code_in_ram()`
keeps its pre-change ternary (rewriting it as an equivalent if/return also loses 16 B and
would be mistaken for a real difference); the Makefile's `hello32` blob dependency is
conditional through `$(if $(findstring ARKRTOS_USE_ELF=1,...),...)`. Criterion: under the
same `SOURCE_DATE_EPOCH`, `objcopy --strip-debug` and compare sha256 against a HEAD build -
**byte-identical** (this round `7cf766ae...10b92a4`, text/data/bss = 235828/128/628228).

### 9.6 Known limits

* all RV32 instances share one window, so no concurrency and no instance-to-instance
  isolation (U-4 only delivers "it runs");
* there are only about 8 PMP regions and this scenario uses 2 (RX/RW); the window cannot
  be subdivided per task;
* fault diagnosis: an ELF task's trap frame lives in the `0x80A...` window while
  `arkrtos_port_fault_detail()`'s `[FRAME]` dump still covers kernel RAM (`0x801...`), so
  an ELF task faulting only prints `mcause/mepc/ra`, not the GPRs. To keep the default
  profile byte-identical this was **not** changed.

---

## 10. PT_TLS: static thread pointer (U-17, 2026-11-20, QEMU run)

### 10.1 What was done

`ARKRTOS_USE_ELF_TLS` (default 0) adds three changes:

1. **measure**: `elf_measure_image()` recognises `p_type == 7` (`ELF_PT_TLS`),
   validates `filesz <= memsz`, a `p_align` of 0/1/a power of two with
   `bias+p_vaddr` actually aligned, and refuses a second `PT_TLS`; it records
   `tls_va = bias + p_vaddr` plus the sizes in `elf_image_t` and pushes the window
   limit `hi` to `tls_va + memsz` — **`.tbss` usually sits past the last
   `PT_LOAD`**, so without that push the bytes that must be zeroed are not even
   declared to be in the window;
2. **lay down**: `elf_write_tls()` copies `filesz` bytes from the file to
   `tls_va` after the normal segments (through the 4 KB staging window in the
   paging tier, a direct read otherwise) and zeroes `memsz - filesz`;
3. **write TP**: `elf_start_task()` takes a `tp` and, **after** the privilege
   drop, calls `arkrtos_port_task_set_tp(stack_ptr, tp)`; on LA64 that is
   `frame[LA_FR_TP] = tp` (`LA_FR_TP = 1`, frame offset 8 bytes = `$r2`). The
   restore path in `arkrtos_context.S` already had `st.d $tp, $sp, 8` /
   `ld.d $tp, $s0, 8`, so no assembly changed.

On a non-LoongArch port the switch is a compile-time `#error`, not a no-op (RV32
was compiled to prove that error fires): `arkrtos_port_stack_init()` zeroes every
frame slot, so without a set-tp primitive the honest outcome would be "TP=0 while
claiming TLS support", i.e. a wrong answer.

### 10.2 Convention: TP = PT_TLS.p_vaddr + bias

Not guessed from the psABI but read off two **independent** toolchains (this
repo's newlib cross gcc and Loongson's 8.3 glibc `loongarch64-linux-gnu-gcc`)
compiling the same two-variable TLS fixture; both disassemblies are

```
lu12i.w / ori / lu32i.d / lu52i.d   ; absolute address of the TLS variable
add.d   $r12, $r12, $r2             ; plus the thread pointer
```

with offsets **0** (`.tdata`) and **4** (`.tbss`), i.e. **TP points at the start
of the TLS block** and TPREL is non-negative. That is the static local-exec shape.

**Stated boundary**: this is not glibc's TCB layout (`tcbhead_t`, `dtv`, the self
pointer, per-module offsets are all defined on glibc's side). "TP points at a TLS
block" and "glibc starts" are different claims.

### 10.3 Readout

The fixture `example/qemu_loongarch64/elf/tls_main.c` (5008 B, signed) reports
through its exit code: **9** = `.tdata` template/TP wrong, **10** = `.tbss` not
zeroed, **11** = `write` return wrong, **3** = both cells pass.

```
[tls] write /mnt/ram/tls_main rc=0 size=5008
[tls] exec rc=0
TLSOK
[tls] finished after 0 ms, exit code 3
```

**The fixture must use `volatile`**: the first version did not, `-O1` folded the
TLS accesses into constants and the linked output had **no `PT_TLS` at all** — a
fixture that measures nothing while looking green.

### 10.4 Default profile unchanged

| Reading | default (`ARKRTOS_USE_ELF_TLS=0`) | switch on |
| --- | --- | --- |
| image text/data/bss | `414896 / 448 / 5112340` | `420944 / 448 / 5112340` (+6048 B) |
| `arkrtos_elf.o` | `5003 / 4 / 4248` | `5531 / 4 / 4248` (+528) |
| `arkrtos_port.o` | `11872 / 4 / 1157` | `11920 / 4 / 1157` (+48) |
| `elf_demo.o` | `22278 / 0 / 4096` | `27830 / 0 / 4096` (+5552, incl. the 5008 B fixture blob) |

The default profile is now **414896 (+688 B)** instead of 414208: the extra bytes are
the 64-bit address arithmetic (§11) and the wide-VA diagnostics, and behaviour is
unchanged per the QEMU readings. With the switch off, `PT_TLS` is still not read at
all - `arkrtos_elf.o` text actually shrank by 16 B because the stack-alignment mask
became `~0x7UL`.

### 10.5 Gate

`tools/elf_gate.py` section H:

* **H1** LA64 with `-DARKRTOS_USE_ELF_TLS=1` must compile both `arkrtos_elf.c`
  and the la64 port;
* **H2** RV32 with `-DARKRTOS_USE_ELF=1 -DARKRTOS_USE_ELF_TLS=1` must **fail**,
  naming `ARKRTOS_USE_ELF_TLS`;
* **H3** wiring pins: `ELF_PT_TLS (7u)`, `tls_va`, `elf_write_tls(`,
  `tls_memsz`, `arkrtos_port_task_set_tp(` and `frame[LA_FR_TP] = tp;`, plus one
  mutation self-check (writing 0 into the frame must make the pin fail).

---

## 11. Address width and the wide VA window (2026-11-20, QEMU runs)

A real distribution binary links at `0x120000000`, and the first thing in the way was
**address width**: the loader's judgement, the window split, `elf_build_stack()` and
the port TLB entries were all computed in 32 bits.

### 11.1 What was done

* The whole chain uses `unsigned long`: `elf_measure_image()` no longer has a
  `p_vaddr > 0xFFFFFFFF` rule, and both the stack alignment in `elf_build_stack()`
  and the `pc` used by the two port `*_syscall_num()` helpers are 64-bit.
* Window VA is decoupled from physical frames: the window base is just
  `ARKRTOS_ELF_WINDOW_BASE`, and `WIN_VA=0x120000000` lifts it together with the
  kernel and fixture link bases (without it everything is byte-identical; `make`
  does not track `WIN_VA`, so switching profiles requires `clean`).
* Diagnostics: `arkrtos_board_puthex()` only takes `uint32`, so a fatal line is
  followed by a full-width `[LA64U] full badv=... / full era=...`, printed only when
  the high 32 bits are non-zero, which keeps the default output identical.

### 11.2 The two 32-bit truncations that actually bit

1. `str_low &= ~0x7u;` in `elf_build_stack()`: `~0x7u` is a **uint32** constant, so
   after integer promotion the high 32 bits are zero and the `&` truncates the address
   to `0x0201FFFF8` (the disassembly shows `lu32i.d $r14,0` followed by `and`). Fixed
   with `~0x7UL`; `stack_top` in `arkrtos_task_stack_init()` (`arkrtos_cfg.c`) is the
   same shape.
2. `uint32 pc = frame[LA_FR_ERA];` in `arkrtos_port_syscall_num()` /
   `arkrtos_port_linux_syscall_num()`: an ERA of `0x120000218` became `0x20000218`, so
   `*(volatile uint32 *)(pc - 4)` took a `KERNEL refill` fatal. Fixed with
   `unsigned long`.

### 11.3 Readings (`WIN_VA=0x120000000`)

```
[linux] PASS: every syscall answered as specified
[linux] brk(+4096) -> 4832890880
[linux] finished after 0 ms, exit code 7
[MOD] tamper check PASS
[FS] PASS
```

That is, **the Linux ABI path (pointers in 64-bit registers) really runs in a wide
window**, with no fatal. The native `hello` prints **nothing** in that profile: its
`DEV_IO` parameter block is `uint32[]`, so the truncated pointer is refused by
`arkrtos_kernel_user_ro_ok()`. That path now emits a one-shot diagnostic
(`SVC:... dev_io open: user pointer rejected (32-bit param block; wide-VA app must use
the Linux ABI for pointers)`) instead of failing silently.

### 11.4 Gates and size

The pins in `tools/elf_gate.py` plus the `elf` / `linux-abi` / `realbin` checks in
`tools/ci_gate.py` cover compilation and fixture shape. Default image is
`414896 / 448 / 5112340` (+688 B over the old baseline: 64-bit address arithmetic and
the wide-VA diagnostics); TLS-on is `420944 / 448 / 5112340` (see §10.4).

---

## 12. Boundaries (do not read as more)

1. **Not "a distribution binary runs"**: address width is no longer the blocker
   (see §11, the wide-window profile runs under QEMU), but there is still **no
   per-process VA space and no runtime page-table construction**, and the native SVC
   parameter block is `uint32[]` - under a wide VA an App cannot pass 64-bit pointers
   through `DEV_IO`/`DRV_MGR`.
2. **No TCB**, **no `R_LARCH_TLS_TPREL64` relocations in the kernel loader** (the dynamic tier is
   handled by the in-tree `ld.so`), **no `set_tid_address`**,
   **no dynamic TLS**. This covers static local-exec only.
3. The wide-VA readings come from `qemu-system-loongarch64`, **not real hardware**;
   both 32-bit truncations were found by tracing fatal addresses on QEMU, so a new
   port needs the same audit again.
