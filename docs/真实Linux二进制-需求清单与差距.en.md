# Stock distribution LA64 binaries: what they need and where Ark stands

> Carrier: QEMU LoongArch64 (`board/qemu_loongarch64`). **QEMU counts as on-board,
> not as real hardware.**
> Instruments: `tools/elf/realbin_probe.py` (probe) + `tools/realbin_gate.py`
> (7 gates). `ci_gate` tracks it as the `realbin` item.

---

## 1. The one-line conclusion

**A stock distribution binary does not run today, and the gap is not "one more
missing feature".** What this round delivers is a **ledger plus an instrument**:
the probe prices a real ELF against what the loader actually supports and prints
the blockers in dependency order, and a gate keeps that ledger honest.

The difference is not size, it is **what the binary asks the kernel for**: which
virtual address it wants, whether it has `PT_TLS`, whether its dynamic section
carries `GNU_HASH` / symbol versions / `INIT_ARRAY`, whether its relocations
include TLS and `IRELATIVE`, and how deep the dependency chain goes. `readelf`
prints all of it, but `readelf` never tells you which of those Ark cannot do.

---

## 2. What this page delivers, and what it does not

| | Status | Evidence |
| --- | --- | --- |
| A recomputable list of "what a real binary needs" | **Delivered** | two real samples, §4 |
| The probe cannot pass bad input and cannot disagree with `ldso.c` | **Delivered** | 7 gates green, §9 |
| Per-number syscall histogram (which numbers really get issued) | **Delivered, switch defaults off** | QEMU readout, §8 |
| A stock distribution binary **actually running** | **No** | none of the six gates is passed, §5 |
| The full syscall profile of one distribution image | **No** | cannot be sampled before the binary reaches `main`, §8 |

Evidence grading is unchanged: *compiles only* / *verified under QEMU* /
*unverified*. Every number below is the second or third kind, and the third kind
is labelled as such.

---

## 3. Using the probe

```bash
py -3 tools/elf/realbin_probe.py hello_dyn.elf hello_static.elf
py -3 tools/elf/realbin_probe.py hello_dyn.elf --sysroot <sysroot dir>
py -3 tools/elf/realbin_probe.py hello_dyn.elf --sysroot <dir> --json
```

Exit-code semantics are the part that gets misread:

- `0` — everything this binary needs is already supported and it lands in the
  current window;
- `1` — there are blockers; the list *is* the to-do list, in dependency order;
- **`1` is a normal probe result, not a script failure**.

The probe is pure `struct` parsing: it never shells out to `readelf`, so the gate
runs anywhere. The relocation-name table is reconciled against binutils by the
gate's `reloc-name-audit` instead (SKIP with a printed reason when the toolchain
is absent, never a silent pass).

`--sysroot` is recursive (depth 3, cycles marked `cyclic`). "The dependency
exists" is **not** a blocker; "the dependency has blockers of its own" is.

---

## 4. Measured: two real samples

Both built with the local Loongson toolchain
(`loongson-gnu-toolchain-8.3-i686-mingw-loongarch64-linux-gnu-rc1.4`, glibc
rc1.4 / gcc 8.3.0) rather than downloading an Alpine/Debian image: reproducible,
offline, and path-controlled.

### 4.1 Dynamically linked (`hello_dyn.elf`, 20 464 B)

```
type=ET_EXEC machine=258 entry=0x120000570 size=20464 B
PT_LOAD x2  span=32896 B  [0x120000000, 0x120008080)  max_align=0x4000
PT_INTERP: /lib64/ld.so.1
PT_TLS: no   RELRO=True   exec-stack=False
DT_NEEDED: libc.so.6
hash: gnu=True sysv=True | versym=True verneed=True | init_array=True | relr=False | bind_now=False
relocs: R_LARCH_64 x5, R_LARCH_JUMP_SLOT x3
symbol versions needed: libc.so.6:GLIBC_2.27
blockers (4, dependency order):
  1. [address]         span [0x120000000, 0x120008080) vs window [0x2000000, 0x2200000)
  2. [interp-path]     /lib64/ld.so.1 is not in the VFS
  3. [dynamic-tags]    INIT_ARRAY, FINI_ARRAY, INIT_ARRAYSZ, FINI_ARRAYSZ,
                       GNU_HASH, VERSYM, VERNEED, VERNEEDNUM
  4. [needed]          libc.so.6 itself has blockers (interp-path/tls/dynamic-tags/relocations)
```

Three things that are easy to miss:

- **The main program is nearly clean** (5 `R_LARCH_64` + 3 `R_LARCH_JUMP_SLOT`).
  The hard part is `libc.so.6`: 1 571 208 B and 1 353 relocations
  (`R_LARCH_RELATIVE x1239, R_LARCH_64 x68, R_LARCH_IRELATIVE x20,
  R_LARCH_TLS_TPREL64 x17, R_LARCH_JUMP_SLOT x9`, matching `readelf -rW`
  item by item).
- `R_LARCH_IRELATIVE` is IFUNC: glibc uses it to pick `memcpy`/`strlen`
  implementations, so it is not "one more relocation kind" but **running user
  code at load time**.
- `GLIBC_2.27` in the version table means that even exporting every symbol under
  the right name would bind the wrong version without `VERSYM`/`VERNEED`
  matching.

### 4.2 Statically linked (`hello_static.elf`, 623 728 B)

```
type=ET_EXEC machine=258 entry=0x1200008e0 size=623728 B
PT_LOAD x2  span=572216 B  [0x120000000, 0x12008bb38)  max_align=0x4000
PT_INTERP: (none, static)
PT_TLS: yes   RELRO=True   exec-stack=False
DT_NEEDED: (none)
relocs: (none)
blockers (2, dependency order):
  1. [address] PT_LOAD span [0x120000000, 0x12008bb38) vs window [0x2000000, 0x2200000)
  2. [tls]     PT_TLS filesz=32 memsz=96 align=8
```

Static is **the closest tier**: no dynamic tags, no relocations, no dependency
chain — only address and TLS. But 623 728 B is **larger than all of `/mnt/ram`**
(see gate 6).

---

## 5. The six gates

| # | Gate | What the real binary wants (measured) | Ark today | Missing | Probe tag |
| --- | --- | --- | --- | --- | --- |
| 1 | Address space | `[0x120000000, …)` | fixed 2 MB window `[0x02000000, 0x02200000)` | load at the VA the ELF asks for: per-process space + page tables built at run time | `[address]` |
| 2 | Interpreter | `PT_INTERP=/lib64/ld.so.1` | default: rejected (`ERR_FORMAT`); with `ARKRTOS_USE_DYNLOAD`: loads this repo's `ld-ark.so` only | the real glibc `ld.so` capability surface (§7) | `[interp-path]` |
| 3 | TLS | `PT_TLS` (16/144 inherited from libc; 32/96 static) + 17 `R_LARCH_TLS_TPREL64` + `set_tid_address` | `arkrtos_elf.c` **never reads `PT_TLS`** | build the TCB, lay out tdata/tbss, point TP at it | `[tls]` |
| 4 | Dynamic tags | `INIT_ARRAY`/`FINI_ARRAY`, `GNU_HASH`, `VERSYM`/`VERNEED`/`VERDEF`, `RELACOUNT`, `FLAGS` | `ld-ark.so` handles only `NEEDED/STRTAB/SYMTAB/RELA/RELASZ/RELAENT/STRSZ/SYMENT/JMPREL/HASH` | run constructors, match symbol versions, GNU hash, `DF_1_NOW` | `[dynamic-tags]` |
| 5 | Relocations | `RELATIVE`, `64`, `JUMP_SLOT`, `IRELATIVE` (IFUNC), `TLS_TPREL64` | only `R_LARCH_64 / RELATIVE / JUMP_SLOT` (both-aligned with `ldso.c` by the gate) | an IFUNC resolver (user code at load time) and TLS relocations | `[relocations]` |
| 6 | Dependency chain and storage | `libc.so.6` 1 571 208 B; static hello 623 728 B | `/mnt/ram` is a littlefs on a 512 KiB RAM block device (`ARKRTOS_QEMU_BLK_SIZE = 512 * 1024`); flash 2 MB, RAM 8 MB | storage first (bigger volume, or map straight from flash), then dependency recursion | `[needed]` + capacity |

Two readings to keep straight:

- Gate 6 is a **precondition**, not an optimisation. If it does not fit, there is
  nothing to measure.
- Until gate 1 is solved, the other five cannot be sampled on a real binary —
  which is exactly the root cause of the §8 boundary.

---

## 6. Four tiers and what each costs

Nearest first. "Passed" means the probe reports 0 blockers **and** QEMU produces
a readout — not "it compiles".

| Tier | Target | Gates already passed | Still missing | Size of work |
| --- | --- | --- | --- | --- |
| 0 | **Probe + ledger** (this round) | — | — | **delivered**, 7 gates green |
| 1 | static musl hello | static already has only address + TLS | load at the requested VA, TLS/TCB, plus musl's startup numbers (`set_tid_address`, `rt_sigaction`, `ioctl`, …) | medium: VA model is harder than the syscall table |
| 2 | dynamic musl (`ld-musl-loongarch64.so.1` + `libc.so`) | as above | musl has no IFUNC (the biggest chunk drops), but needs real `PT_INTERP` resolution, `NEEDED` recursion, `RELATIVE`/`JUMP_SLOT`/TLS relocations | medium-large: `ld.so` must serve a real libc |
| 3 | glibc dynamic (the question as asked) | — | all six gates + IFUNC + symbol versions + `rseq`/`futex` + a much wider syscall surface | project-level |
| — | static glibc (between 1 and 3) | static two + `INIT_ARRAY` | glibc's startup needs a pile of numbers before `__libc_start_main` | large |

**How to read it**: the smallest honest step toward "run a distribution binary"
is tier 1 — it turns address and TLS from "not done" into "done with a readout",
and both are prerequisites for every later tier.

---

## 7. Why not "write our own `ld.so` and point it at glibc"

The tempting shortcut is: we already load `PT_INTERP`, so ship a loader that
serves glibc. It fails on contract, not on effort:

1. glibc's `ld.so` and `libc.so.6` are **a pair**: libc references `_dl_*`
   (`_dl_find_object`, `_dl_catch_exception`, …) exported by that exact loader.
2. glibc's startup needs TLS (`__libc_setup_tls` assumes a usable TP),
   `INIT_ARRAY`, IFUNC via `IRELATIVE`, `rseq` registration and `futex`
   **before** `__libc_start_main`. Missing one is not a degradation — it does
   not start at all.
3. Symbol versions require `VERSYM`/`VERNEED` matching; name-only binding picks
   the old implementation, which fails silently and computes the wrong answer —
   exactly the "authoritative-looking wrong answer" this project forbids.

`ld-ark.so` serves **this repo's own libraries** (three relocation kinds, no
symbol versions). It proves the kernel-side Linux convention and the relocation
path work; it does **not** prove glibc compatibility. The two must not be read
as the same thing.

---

## 8. Per-number histogram (`ARKRTOS_USE_LINUX_ABI_SYSSTAT`)

The totals in the syscall-ABI page (handled / enosys / efault) cannot answer
"which numbers did this binary actually issue". With the switch on the kernel
keeps a **16-slot sparse table** (first-seen order, overflow counted and never
silently dropped) and the shell gains `lxstat`.

```bash
make clean
make -f example/qemu_loongarch64/Makefile -j8 \
     EXTRA_CFLAGS="-DARKRTOS_USE_DYNLOAD=1 -DARKRTOS_LINUX_ABI_SYSSTAT=1"
```

QEMU readout (`elf_demo.c` prints it right after the Linux demo, because this
board has no interactive input — an unprinted table is an unverified function):

```
[lxstat] slots=11 overflow=0
[lxstat] nr 64 x102   (write)      [lxstat] nr 160 x1  (uname)
[lxstat] nr 113 x2    (clock_gettime)
[lxstat] nr 278 x1    (getrandom: unknown number, returns -38)
[lxstat] nr 172 x1    (getpid)     [lxstat] nr 214 x2  (brk)
[lxstat] nr 222 x1    (mmap)       [lxstat] nr 56 x2   (openat)
[lxstat] nr 63 x1     (read)       [lxstat] nr 57 x1   (close)
[lxstat] nr 93 x1     (exit)
```

`getrandom(unknown nr) -> -38 ok`, `PASS: every syscall answered as specified`
and `exit code 7` all stay in the same boot, and the `[ELF]`/`[FS]` readouts are
untouched.

**Boundary, stated plainly**: those 11 numbers come from the **synthetic** traffic
of `example/qemu_loongarch64/elf/linux_hello.c`, not from a real distribution
binary. The real profile needs gate 1 solved first. The `lxstat` shell command
itself has compile-and-register evidence only (no interactive stdin under QEMU);
the serial evidence is the demo printout above. `linux_hello.c` also gained a
negative case this round — `getrandom(278) -> -ENOSYS` — because none of the
previous 13 checks ever hit an unimplemented number, so "unknown numbers get
counted" was unproven.

---

## 9. The gate

```bash
py -3 tools/realbin_gate.py        # 7 checks, prints REALBIN-GATE OK
```

| Check | What it pins |
| --- | --- |
| `fixture-near` | in-window static fixture: 0 blockers (exit 0) |
| `fixture-far` | far base `0x120000000`: **exactly one** `address` blocker (exit 1) |
| `fixture-dyn` | dynamic fixture: 0 blockers (`interp=/mnt/ram/ld-ark.so` is not misjudged) |
| `probe-negatives` | four bad inputs (not ELF / ELF32 / `e_phentsize=40` / `e_phoff` out of range) all rejected with reasons |
| `table-alignment` | 12 DT tags and 3 relocation types aligned **both ways** with `ldso.c` |
| `reloc-name-audit` | 5 relocation names and counts match `readelf -rW` (SKIP with reason when absent) |
| `sysroot-semantics` | a missing and an empty sysroot are both stated aloud |

`probe-negatives` is not ceremony: it caught a real defect in the probe itself
(bad ELF → `AttributeError` on `e.e_type`). The negative cases test the
instrument.

Two implementation notes worth keeping:

- **Relocation de-duplication**: glibc's `DT_RELASZ` spans `.rela.plt` as well
  (`32472 = 31776 + 696`), so `DT_JMPREL` points into the same bytes and two
  scans double-count (`JUMP_SLOT` 9→18, `IRELATIVE` 20→40). The probe de-dups
  by file offset and then matches `readelf` exactly — which also re-confirms
  `R_LARCH_IRELATIVE=12` and `R_LARCH_TLS_TPREL64=11`.
- **`DT_RELAENT` has no `case`**: `ldso.c` hard-codes a 24-byte entry, so the
  probe checks `DT_RELAENT != 24` on its own and the gate classifies it as
  "covered by a dedicated check", not "unwired".

### Default-off is zero change, measured

Two layers, both measured.

**(a) Kernel code: identical object code.** The four touched TUs were compiled
with the same default flags from `HEAD` and from the working tree; `size`
text/data/bss match item by item:

| file | HEAD | working tree |
| --- | --- | --- |
| `kernelsrc/src/arkrtos_linux_abi.c` | `4236 / 0 / 632` | `4236 / 0 / 632` |
| `kernelsrc/shell/arkrtos_shell.c` | `10341 / 0 / 9752` | same |
| `kernelsrc/shell/arkrtos_shell_misc.c` | `3706 / 0 / 0` | same |
| `example/qemu_loongarch64/elf_demo.c` | `22278 / 0 / 4096` | same |

This was not true at first: the three readout functions started as
"return 0 when off" stubs, which added 128 B of object code to the default
build. The fix moves the definitions inside
`#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)`, with the header not declaring them when
off — **a stub is still size**.

**(b) Image: the extra 56 B is the demo program, not the kernel.** A clean
rebuild gives `.text / .data / .bss = 413808 / 448 / 786964` against a
`413752 / 448 / 786964` baseline. All 56 B come from the **`linux_hello` blob
compiled into the image**: the same source without this round's `getrandom`
check compiles to 2736 B of text instead of 2792 B. Kernel side 0 B,
`.data/.bss` 0 B.

**(c) No new symbols in a default build**: `nm` counts zero for `lxstat` /
`nr_slot` / `nr_overflow`.

---

## 10. Boundaries and what is not done

1. Stock distribution binaries **do not run** — this round delivers the ledger
   and the instrument, not the capability.
2. Static hello 623 728 B and `libc.so.6` 1 571 208 B **both exceed the 512 KiB
   `/mnt/ram` volume**, so "just copy the library in and try" cannot even start.
3. The `[lxstat]` numbers are **synthetic** (11 numbers), not a real profile.
4. The probe does not parse `PT_NOTE` or `.gnu.attributes` (ABI flags), and the
   `RELRO=True` field is descriptive, not a blocker.
5. The sysroot search covers 7 conventional subdirectories to depth 3; a
   `RUNPATH=$ORIGIN/../lib` from a distribution image is not resolved.
6. No compatibility testing against a real `ld.so`, and no experiment aimed at
   satisfying glibc's contract (§7 explains why that is not a work item).

**The smallest next step**, if this continues: tier 1 — make the loader place
segments at the VA the ELF asks for (per-process space, page tables built at run
time), then read `PT_TLS` and build the TCB. Both can be pinned by QEMU
readouts, and both are shared prerequisites for every later tier.