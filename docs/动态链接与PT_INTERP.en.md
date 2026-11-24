# Dynamic linking and PT_INTERP on Ark RTOS (LA64, self-hosted ld.so)

**[简体中文](动态链接与PT_INTERP.md) | English**

*Condensed English edition. The Chinese page is normative; nothing new is
claimed here and no extra measurements are introduced.*

Switch `ARKRTOS_USE_DYNLOAD` (Kconfig: `depends on ARKRTOS_USE_ELF`, **default n**).
Carrier: QEMU LoongArch64. **QEMU counts as "on board", not as silicon.**

## 1 One line

With the switch on, the kernel accepts executables that carry `PT_INTERP`: it
loads the interpreter into a board-provided region, builds the Linux `auxv`
vector on the user stack, and starts at the interpreter entry. The interpreter
is this repo's own minimal `ld.so` (`example/qemu_loongarch64/elf/ldso.c`); it
maps the needed libraries, builds one TLS block, applies relocations and jumps
back to the main program. It does **not** mean a stock distribution binary runs (see §6).

With the switch off (default), `PT_INTERP` is still refused with
`ARKRTOS_ELF_ERR_FORMAT (-3)` (`-9` is `ERR_INTERP`), and none of the fixture
blobs is compiled in.

## 2 Kernel side

Board layout (`board/qemu_loongarch64/arkrtos_board_config.h`):

| Macro | Value | Meaning |
| --- | --- | --- |
| `ARKRTOS_ELF_WINDOW_BASE` / `_SIZE` | `0x02000000` / `2 MB` | ELF user window |
| `ARKRTOS_ELF_INTERP_BASE` | `0x02100000` | interpreter load base |
| `ARKRTOS_ELF_INTERP_SIZE` | `64 KB` | interpreter region |

Rules: the interpreter region must lie inside the window; main image and
interpreter must not overlap (`ERR_LAYOUT` otherwise, no "good enough" fallback).
`lx_heap_base()` starts after the interpreter region when the switch is on.

Load path (`kernelsrc/src/arkrtos_elf.c`): accept `ET_EXEC` and `ET_DYN`;
`elf_find_interp()` reads the `PT_INTERP` string (forced NUL, over-long path
refused); the main image keeps the original two-pass all-or-nothing layout
(`ET_DYN` gets `bias = window base` and must have lowest vaddr 0); the
interpreter is measured and written separately (`ET_EXEC` must be linked at
`INTERP_BASE`, `ET_DYN` is placed there); the two ranges must not intersect;
`AT_PHDR` must be non-zero; `auxv` gets `AT_PHDR / AT_PHENT / AT_PHNUM /
AT_PAGESZ(4096) / AT_BASE / AT_ENTRY / AT_UID / AT_EUID / AT_FLAGS`; the task
entry becomes `interp.entry`. A main program that carries `PT_TLS` still gets the
static-profile block from the kernel, but in the dynamic profile that `$tp` is only
the entry value — ld.so allocates its own unified block and rewrites `$tp`.

Linux ABI additions (`kernelsrc/src/arkrtos_linux_abi.c`): `pread64 (67)`,
`mprotect (226)` — **accepted, not enforced** by default (the window is one
`PS=20` RWX entry; `stat` says `accepted, not enforced`), real 4 KB page-level
write protection behind `ARKRTOS_USE_PAGING_MPROTECT` (section 8.6 of the Linux ABI
page); plus the fifth `stat` variant. `MAP_FIXED` now exists in the demand tier (section 8.7 of the Linux ABI page)
but **this loader does not use it**, so a library address is `mmap` result +
`p_vaddr`.

## 3 The minimal ld.so

`ET_EXEC` linked at `0x02100000` (no self-relocation). Reads
`argc/argv/envp/auxv`; `main_base = AT_PHDR - 64` and **verifies**
`e_phoff == 64` (otherwise it fails loudly rather than guessing); parses
`PT_DYNAMIC`; resolves each `DT_NEEDED` under `/mnt/ram/` then `/lib/`, maps it
anonymously and `pread64`s its `PT_LOAD`s (zeroing `.bss`); resolves symbols by
a linear scan of `.dynsym`; `setup_tls()` then gives every `PT_TLS` object a
16-byte-aligned slot in **one** contiguous block (main program first) and prints
`[ldso] tls base=... size=... objs=... offs=...`; applies `R_LARCH_RELATIVE(3)` /
`R_LARCH_64(2)` / `R_LARCH_JUMP_SLOT(5)` / `R_LARCH_TLS_TPREL64(11)` (library
first, then main program) — a TPREL64 slot holds the signed `S + A - TP` offset,
not an absolute address; prints `[ldso]` lines, sets `$tp` (i.e. `$r2`) when a TLS
block exists, and jumps to `AT_ENTRY`.

Any failure prints `[ldso] FAIL: ...` and exits 127 — the loader is never
allowed to "look successful". Limits: one level of dependencies only, at most 4
objects / 16 program headers, no lazy binding, no `.init_array`. TLS is only the
"static block + TPREL64" (initial-exec) form: `DTPMOD` / `DTPREL`,
`DT_TLSDESC`, and library local-exec immediate offsets are not handled.

## 4 Fixtures

The bundled toolchain (`ld 2.31.1`) does not support `-shared` / `-pie`; `gcc
-shared` silently yields a static `ET_EXEC` with no `.dynamic`. So fixtures are
built by `tools/elf/dynfixture.py`: link statically (needs `.symtab`), then
rewrite into a dynamic image with `PT_INTERP`, `PT_DYNAMIC`, `.dynsym/.dynstr/
DT_HASH/.rela.dyn/.rela.plt`, original `PT_LOAD`s kept byte-for-byte, and one
extra read-only `PT_LOAD` covering ELF header + phdr table so that `AT_PHDR =
base + 64` holds. Relocations are **authored by the manifest**, so each path can
be broken on purpose.

| File | Type | Notes |
| --- | --- | --- |
| `libanswer.so` | `ET_DYN` | exports `answer_add / lib_g_addr / g_base / g_ptr / abs_ptr / got_answer / lib_tls_addr / lib_tls_read / lib_tls_write / lib_tls_off`; 1 RELATIVE + 1 `R_LARCH_64` + 1 TPREL64; `PT_TLS` 8 B (`lib_tls = 4242`) |
| `dynmain` | `ET_DYN` | needs `libanswer.so`; 1 RELATIVE + 2 `R_LARCH_64` + 6 JUMP_SLOT + 1 TPREL64; `PT_TLS` 16 B (`m_tls`, `m_tls_pad`) |
| `dynmain_exec` | `ET_EXEC` | same source, linked at `0x02001000` (other bias branch) |
| `dynmain_bad` | `ET_DYN` | **anti-false-green**: JUMP_SLOT to `no_such_symbol`, which does not exist |

Sizes (measured this round): `ld-ark.link.elf` 27184 B, `libanswer.so` 21667 B,
`dynmain` 23851 B (23883 B signed), `dynmain_exec` 23851 B (23883 B signed),
`dynmain_bad` 23043 B (23075 B signed). Two deliberate TLS design choices make a
false green impossible to miss: `m_tls` sits at offset **8** (this gcc lays
`.tdata` out last-declared-first; `m_tls_pad` is declared after it), and the
library's TLS is reachable only through libanswer's own `got_libtls` slot.

## 5 Measured readout (QEMU, 2026-11-21)

```
[ldso] main base=0x0000000002000000 type=3 entry=0x00000000020014d0 interp_base=0x0000000002100000
[ldso] need libanswer.so
[ldso] lib libanswer.so base=0x0000000002110000 size=24576
[ldso] tls base=0x0000000002116000 size=32 objs=2 offs=0,16,
[ldso] reloc libanswer.so: RELATIVE=1 64=1 JUMP_SLOT=0 TPREL64=1
[ldso] reloc dynmain: RELATIVE=1 64=2 JUMP_SLOT=6 TPREL64=1
[dyn] TPREL64 m_tls offset -> 8 ok
[dyn] TPREL64 offset nonzero -> 1 ok
[dyn] TLS main value -> 77 ok
[dyn] TLS lib offset -> 16 ok
[dyn] TLS lib distinct -> 1 ok
[dyn] TLS lib value -> 4242 ok
[dyn] TLS lib after write -> 4243 ok
[dyn] TLS main untouched -> 77 ok
[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT + TLS all applied
[dyn] info dynamic=1 interp_base=34603008 interp_entry=34610336
[dyn] finished after 0 ms, exit code 105
... (dynmain_exec, type=2: same 14/14 ok, exit code 105)
[ldso] FAIL: unresolved PLT symbol no_such_symbol
[dyn] finished after 0 ms, exit code 127
```

Five conditions must hold together: both variants 14/14 `ok` with `[dyn] PASS`
and exit code 105; `interp_base = 0x02100000`; a `[ldso] tls base` line per load
with `size=32` (16 + 8 + alignment), `objs=2`, `offs=0,16` — the two objects get
**different** offsets, which is the per-object evidence; the bad fixture fails with
`unresolved PLT symbol no_such_symbol` and exit code 127 with **no extra PASS**;
and `[linux] PASS` / `[FS] PASS` / `[ELF] tamper check PASS` from the same boot
are still there.

Anti-false-green control: patching the `type 11` arm of `apply_rela()` to
`return 0` (slot not written, nothing else touched) yields `TPREL64=0`,
`TPREL64 m_tls offset -> 0 FAIL (want 8)` and `TLS lib value -> 4369 FAIL (want
4242)` with exit code 1. `4369 = 0x1111` is exactly `m_tls_pad`, i.e. the code
read the neighbouring variable because the slot was never written.

## 6 Gate and what is explicitly not supported

`tools/dynlink_gate.py` (13 checks, registered in `ci_gate` as `dynlink`):
default-off, Kconfig wiring, `#error` guards, board layout (macros expanded by
the compiler, not parsed by hand), off/on symbol check on `elf_demo.o`, on-state
compile of 5 translation units, a clean build (`make` does not track
`EXTRA_CFLAGS`, so reusing old objects would fake "no dynamic demo"), fixture
shape read byte-wise (`e_phoff == 64`, a `PT_LOAD` covering the phdr table,
`PT_INTERP` string, `DT_NEEDED` count, the bad symbol, ld.so base), fixture TLS
shape (`PT_TLS` sizes, one type-11 reloc per fixture, `STT_TLS` symbol values),
byte-equal fixture rebuild, the QEMU readout above, a parser self-check with seven
mutated logs, and a final restore of the default build.

Default-profile size: `.text` 413752 B vs 411360 B on the pre-change baseline
(+2392 B, +0.58%), from the shared measure/write refactor rather than from the
feature; no fixture blob is compiled in. Behaviour on the default profile is
line-for-line the same as the baseline apart from timing, tick counts and one
module base shifted by the larger image.

Not supported (do not read this as "Linux binaries run now"):

1. stock distribution binaries — their `PT_INTERP` points at glibc's
   `ld-linux-loongarch-lp64d.so.1`, which is not in this repo, and there is no
   LA64 Linux cross toolchain on this machine;
2. glibc's ld.so needs the whole futex surface (only WAIT/WAKE exist here),
   `rseq`, TLS, fd-backed `mmap` (only a one-shot snapshot + whole-file
   write-back tier exists; see section 8.5 of the Linux ABI page), signals, …;
3. `mprotect` is accepted, not enforced by default (the window is RWX); the
   `ARKRTOS_USE_PAGING_MPROTECT` profile enforces the write bit at 4 KB granularity
   but still cannot make pages non-executable;
4. no lazy binding, no `.init_array`; TLS is only the static-block + TPREL64
   (initial-exec) form — no `DT_TLSDESC` / `DTPMOD64` / `DTPREL64`;
5. one dependency level, 4 objects, linear symbol scan;
6. the interpreter itself is not signature-checked (this board has
   `ARKRTOS_USE_EXEC_SIGN` off);
7. all readouts are QEMU, not silicon.

## 7 Reproduce

```bash
make -f example/qemu_loongarch64/Makefile EXTRA_CFLAGS="-DARKRTOS_USE_DYNLOAD=1" -j8
cd build/qemu_loongarch64 && timeout 90 \
  "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -m 512M -kernel ark_qemu_la64.elf -serial stdio -display none
py -3 tools/dynlink_gate.py     # runs every check above and restores the default build
```