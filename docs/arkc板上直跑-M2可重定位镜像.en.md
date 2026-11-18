# arkc On-Board Direct Run — M2 Relocatable Image (four-pass differencing)

> Condensed English edition. The Chinese page (arkc板上直跑-M2可重定位镜像.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

This is the landing record for **M2 (L2 relocatable, via the existing mini path)** from the
arkc build-artifact-on-board implementation plan: on an STM32H743 board, link the same
source **four times** with the on-board arkc, difference out a relocation table, pack a
standard `.svcm`, and hand it to `mini run` — **which did not change by a single byte for
this purpose** — to be installed into any free block of the RAM pool and executed.

Date: 2026-11-05. All readings come from the real board's serial port (COM3 / 115200) and a
host comparison harness; no number is inferred.

> Automated re-runs of the criteria are in the error-code and regression-ledger page §5:
> `py -3 tools/final_app.py`'s `m2-pack-run` and `m2-two-landings` items are the cases of
> §5 / §5.2 here. Note `m2-two-landings` needs firmware whose `MINI_PRIORITY` is below the
> console (§5.3).

---

## 1. One-sentence conclusion

`cc --pack /tmp/p.c -o /tmp/p.svcm` → `mini run /tmp/p.svcm` → the two lines unique to the
source appear:

```
BRD-M2-R1 alive
BRD-M2-R1 tag alpha
```

The program exits itself and **both the code block and the RAM block return to the pool**.
The image is 3288 B (payload 3008 + table 6×4), four links took 267 ms in total. The four
relocation kinds (whole-word ROM / whole-word RAM / MOVW-ROM / MOVW-RAM) are each **non-zero**;
no kind is "zero hits, therefore effectively unverified".

---

## 2. What four-pass differencing actually does

The image is linked at a **nominal base** but at run time gets installed into any
power-of-two-aligned free block in the RAM pool. Every absolute address in the code
(function pointers, string literals, pointers into its own RAM, addresses inside
`MOVW/MOVT` immediates) must shift with the landing point. Differenced linking is the only
way that **does not require the compiler's cooperation** (no upstream codegen change):

| Pass | Link base |
|----|----------|
| A | nominal (ROM = `rom_base`, RAM = `ram_base`) |
| B | ROM = `rom_base + delta`, RAM unchanged |
| C | RAM = `ram_base + delta`, ROM unchanged |
| D | ROM = `rom_base + 2*delta`, RAM = `ram_base + delta` (**verification only**, never becomes an image) |

Criteria:

- A→B shifts by exactly `delta` while A→C is unchanged ⇒ **ROM class**
- A→C shifts by exactly `delta` while A→B is unchanged ⇒ **RAM class**
- A, B and C are all identical ⇒ not an absolute address
- anything else ⇒ **refuse** (`-31`), because that position depends on the base in a way
  outside the model

The last packing step applies the table back to A and requires it to be **byte-identical to
D**; otherwise no image is produced (`-32`). So "can this image be moved to another
power-of-two-aligned landing point" is proven at packing time rather than left to crash
randomly on the board.

`delta` must be ≥ two window lengths: with a smaller delta, B/C's address ranges overlap A,
and "the value at this position differs by exactly delta" stops being evidence of an absolute
address — an ordinary constant in the overlapping range can satisfy it too. This is not
conservatism but the precondition for the criterion to hold (`-30`).

---

## 3. Five pitfalls (each one produced a wrong answer first, then was localised)

The first three are of the "**produces a runnable image with wrong semantics**" kind, the
fourth "**produces a correct table but silently drops entries**", the fifth "**verification
passes but verifies the wrong thing**". None can be found by asking "did it run".

### 3.1 MOVW/MOVT immediate pairs must be tested before whole words

`MOVT`'s first half-word (`0xF2C0 | (imm4<<8) | (i<<12) | rd`) is **identical** in A/B/C;
only the `imm8` in `MOVW`'s second half-word changes with the base. So **viewed as a 4-byte
word**, the pair happens to equal `+delta` and is misclassified as a "whole-word ROM entry".
Its start often lands at `2 mod 4` — the loader would refuse a misaligned whole-word entry
anyway, but the worse part is that `MOVW`'s second half-word is treated as "already handled",
so **the real MOVW pair is never scanned again**.

Fix (identical on both sides): insert **step 0** in the scan loop — first try to interpret
`pos` as a `MOVW/MOVT` immediate pair (`pos+8 <= len`, all three passes decode a pair, and the
three values are **not all equal**); on a hit, record a MOVW entry and `pos += 8`. Whole-word
testing becomes step 1, half-word-identical testing steps 2/3.

### 3.2 Whole-word entries must satisfy `pos % 4 == 0`

Consistent with the C side: the loader `arkrtos_loader.c` refuses whole-word entries not
aligned to 4 bytes. The Python side lacked this gate and could produce "entries that pass
`verify`" at positions like `0x482` — **`verify` passing only proves "table applied equals D",
not that the entry sits at a legal position**. The fix adds `pos % RELOC_SIZE == 0` to Python,
aligning Python to C's strictness rather than loosening C.

### 3.3 `kind != 0` must not be used as the hit test (the most hidden one this round)

`ARKRTOS_APP_RELOC_KIND_ROM == 0`. The C side originally wrote:

```c
if (kind) { record entry; }
```

So **every whole-word ROM entry was silently dropped** — the differencing did compute "this
is a ROM absolute address", but "did it hit" was judged by "kind non-zero", and the ROM
class's kind value is 0 by definition. The symptom was a table with fewer entries than
Python's while `verify` still passed (with one missing, the D pass may simply not cover that
position).

Fix: introduce a separate `uint32 hit = 0u;`, set it from whether a usable difference was
computed, and test `hit` rather than `kind`.

### 3.4 The payload must be A, not "the patched copy"

`verify` works by applying the table to a copy of A and comparing with D. Originally that
copy **was used directly as the image payload**, so the image was "nominal addresses + delta"
— header CRC correct, `mini run` installs successfully, then it runs away. Fix: after
`verify`, `for (i...) payload[i] = a[i];` restores the nominal form.

### 3.5 `image_id` = payload CRC, and must be written **before** the header CRC

- `image_id` means **payload CRC32** (not image CRC).
- It lives inside the header region covered by the header CRC. Filling it in **after**
  computing the header CRC guarantees the header CRC mismatches; the board shows `mini run`
  returning `rc=-5` (CRC check failed).

Fix: `p_hdr->image_id = arkrtos_crc32(payload, len, 0u);` before computing the header CRC,
and delete the trailing back-fill.

Image-CRC coverage order (`arkrtos_loader_crc_header`): header (`crc32` / `state` /
`runtime_ram_base` substituted as 0, using four real zero bytes) → relocation table → payload.

---

## 4. Host comparison: find the defects at 1/100 the cost

Differencing and packing are pure computation. Move them to the host and compile
`kernelsrc/src/arkrtos_apppack.c` directly with MinGW gcc:

```sh
cc -std=c99 -DARKRTOS_USE_APPPACK=1 -Ikernelsrc/include -Iconfig/stm32h743 \
   kernelsrc/src/arkrtos_apppack.c <harness.c> -o harness.exe
```

(`variably modified` is pre-existing noise, not introduced this round.)

The harness **writes its own real `arkrtos_crc32`** (bit-identical to zlib, poly
`0xEDB88320`, inverted before and after) **and `arkrtos_loader_crc_header`** — otherwise a CRC
mismatch cannot be told apart from a wrong field.

```
rc=0 payload_equals_A=1 image_id_ok=1 header_crc_ok=1
n_rom=1 n_ram=1 n_rom_movw=2 n_ram_movw=2
out_len=3396 reloc_count=6
0x0118 kind=1 / 0x011C kind=0 / 0x0596 kind=2 / 0x05D4 kind=3 / 0x065A kind=2 / 0x0748 kind=3
```

Python `diff_reloc` on the same input produces
`0x0000 kind=1 / 0x0004 kind=0 / 0x047E kind=2 / 0x04BC kind=3 / 0x0542 kind=2 / 0x0630 kind=3`
and `verify OK`. The offset difference of `0x118` is exactly `payload_prefix` ⇒ **the two
sides agree entry by entry**.

### 4.1 Anti-false-green (host, nearly zero cost)

| # | Injection | Expectation | Measured |
|---|---|---|---|
| ① | `D_FILE = pB.bin` (the verify pass is not really D) | refused, reports verify | `-32` |
| ② | `C_FILE = pD.bin` (the window pass got crossed) | refused, reports diff | `-31` |
| ③ | `DELTA = 0x1000` (smaller than the window) | refused, reports param | `-30` |
| ④ | `DELTA = 0x90000` (inconsistent with the one D used) | refused | `-31` |
| ⑤ | Python deletes one table entry at a time and replays | full table ⇒ equals D; **deleting any single entry** (6/6) ⇒ never equals D | all 6/6 unequal |

Item ⑤ is the host equivalent of "criterion A's 'deliberately deleting an entry must cause a
problem'": if deleting some entry still equals D, that entry was never covered by the D pass
and the criterion never tested that address.

---

## 5. On-board readings

```
cc: pack /tmp/p.c -> /tmp/p.svcm (4 passes, -O1)
cc:   A 3006 B / B 3006 B / C 3006 B / D 3006 B
cc: /tmp/p.svcm: payload 3008 + table 6 x 4 = 3288 B
cc:   reloc rom 1 / ram 1 / rom_movw 2 / ram_movw 2, entry +0x428
cc: packed in 267 ms
mini run /tmp/p.svcm
[...] MINI:568 /tmp/p.svcm: 3008 B payload -> code 0x24070000 (4096 B block),
        RW/stack 0x24072000 (8192 B), task 4
BRD-M2-R1 alive
BRD-M2-R1 tag alpha
[...] MINI:862 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
mini run: /tmp/p.svcm loaded but no longer running (…)
mini: none running (usage: mini run <path>)
```

- The payload is zero-padded to a multiple of 4: 3006 → 3008 (`arkrtos_apppack_build` requires
  a multiple of 4).
- All four entry counts are non-zero and **equal to the same-named host counts** (1 / 1 / 2 / 2).
- After the run **both** the code block and the RAM block are returned; `mini: none running` is
  the normal ending (the program exited itself).

### 5.1 2026-11-07: `--pack` restored; root cause is `rd_le32` (compiler.c)

After syncing arkc 0.8.0, `cc --pack` was stuck at the last step:

```
cc:   A vector: SP 0x0 entry 0x0 (rom base 0x8000000, window 8192 B)
cc: pack failed: no usable vector table at payload +0/+4 (compile with --vector-table) (-34)
```

All four passes compiled; the failure was in "reading the payload header". At the time the
evidence was `cc -link -v` reporting **0 syms** for that object while the host reported 43
syms for the same source ⇒ the link stage did not read the object symbol table ⇒
`ld_find_global("Reset_Handler") < 0` ⇒ the vector table was skipped wholesale.

The root cause was not the linker but the byte-reading function:

```c
/* before: gcc fuses these four reads into one 32-bit unaligned load */
static uint32_t rd_le32(const unsigned char *p) { ... }
```

After `.ark_buf` moved from internal RAM (32-bit bus) to external SDRAM (16-bit bus),
**unaligned 32-bit reads return 0 on this board**. Object fields (`name_len` followed by the
name) are naturally unaligned, so the symbol count read as 0. Measured evidence: reading the
same address byte by byte gives `45,0,0,0` (45 symbols) while `rd_le32` gives 0; an earlier
occurrence even took a BUS FAULT (`BFAR=0x21744c9a`, `PC=0x0803b158` inside `rd_le32`, `LR`
in `ld_find_global`).

The fix forces four byte-wise reads and forbids the compiler from fusing a wide read:

```c
static uint32_t rd_le32(const unsigned char *p)
{
    const volatile unsigned char *v = p;   /* volatile: byte-wise, no fused wide read */
    return (uint32_t)v[0] | ((uint32_t)v[1] << 8) |
           ((uint32_t)v[2] << 16) | ((uint32_t)v[3] << 24);
}
```

On the board afterwards:

```
cc:   A vector: SP 0x20002000 entry 0x8000418 (rom base 0x8000000, window 8192 B)
cc: /tmp/m2.svcm: payload 3292 + table 6 x 4 = 3572 B
cc: packed in 916 ms
```

The shape of this defect is worth recording: **it appears only when "wider bus + unaligned
field" hold together**, so the internal-RAM profile, the host x86 and every byte-wise read
path never see it; and on the board its symptoms (`0 syms`, `(-34)`, BUS FAULT) all pointed at
the linker, i.e. the wrong direction.

### 5.1 `cc --pack` constants (on board)

| Constant | Value |
|------|-----|
| `CC_PACK_WIN_SIZE` | 8192 |
| `CC_PACK_OUT_CAP` | 16384 |
| `CC_PACK_ARENA` | 32768 (**re-borrowed from the pool on every pass, not four coexisting copies**) |
| `CC_PACK_ROM_BASE` | `0x08000000` |
| `CC_PACK_RAM_BASE` | `0x20000000` |
| `CC_PACK_DELTA` | `0x00080000` |
| `CC_PACK_SCT` | `/tmp/pp.sct` (one file shared by four passes, deleted at the end) |

`cc_reset_state()` (`config_init` + `g_config.enable_fpu_hw=1` + `arkc_port_zero_arena()` +
`diag_init`) is **called on every pass**, so each pass gets a clean arena — the precondition
for "linking four times in one process" to work.

### 5.2 Criterion B: the same image at two landing points (`r2.c`, 3144 B payload)

The program itself computes "absolute address of the string literal − `0x24070000`", shifts
right by 12, and prints that block index as one character `'0'+n`. So the `0` / `1` on the
serial line is **measured by the program**, not read out of a log by a human:

```
mini run /tmp/r2.svcm
MINI:579 /tmp/r2.svcm: 3144 B payload -> code 0x24070000 (4096 B block), RW/stack 0x24072000 (8192 B), task 4
R2 alive
0
mini run /tmp/r2.svcm            # same file, not repacked
MINI:579 /tmp/r2.svcm: 3144 B payload -> code 0x24071000 (4096 B block), RW/stack 0x24074000 (8192 B), task 5
R2 alive
1
mini
mini: 2 running of 2 allowed
  [0] task 4 entry 0x24070419
      code 3144 B in 4096 B block @ 0x24070000
      RW/ZI+stack 8192 B block @ 0x24072000, crc 0x444C3FCF
  [1] task 5 entry 0x24071419
      code 3144 B in 4096 B block @ 0x24071000
      RW/ZI+stack 8192 B block @ 0x24074000, crc 0x444C3FCF
  block entries: 2 of 4 in use
mini stop
mini stopped: all blocks returned to the pool
```

Three readings each cover one thing:

- **same `crc` (`0x444C3FCF`)** ⇒ genuinely the same image, not a second packing;
- **same entry offset (block-relative `+0x419` both times)** ⇒ the code did not shift because
  the landing point changed;
- **ROM literals and RAM globals both correct** (`R2 alive` comes from a literal in the code
  block, and the `g_anchor == 0xA5A5A5A5u` test did not print `!`) ⇒ both ROM and RAM
  relocation classes were covered by this change of landing point.

### 5.3 Why a second run was possible this time: `MINI_PRIORITY`

The first attempt (default firmware) hit the other half of M1's lesson: mini-program tasks use
`APP_TASK_PRIORITY` (10), **above the console (14)**. A `for (;;) { }` mini program starved the
console — the first `mini run` printed normally, then typing `mini` produced not even an echo
and a reset was the only way out (the predecessor of §5.2 failed exactly that way).

So `mini` gained an override hook; **with the macro undefined, behaviour and size are
byte-identical** (profile 0 still 381808 B, ON profile still 388704 B):

```makefile
# example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile
MINI_PRIORITY ?=          # empty = keep APP_TASK_PRIORITY
```

```c
/* kernelsrc/src/arkrtos_mini.c */
#ifndef ARKRTOS_MINI_TASK_PRIORITY
#define ARKRTOS_MINI_TASK_PRIORITY   APP_TASK_PRIORITY
#endif
```

Criterion B's firmware was built with `make USE_APPPACK=1 USE_MINI_VFS=1 MINI_PRIORITY=22`.
The hook is not test-only: mini programs are the "read now, run now, most likely to loop
forever" form, and once looping forever they can no longer be stopped — putting their priority
below the console is the only way to keep rescuing a live system.

---

## 6. Size accounting

| Profile | FLASH | RAM |
|----|-------|-----|
| profile 0 (`make clean && make -j8`) | **381808 B** | same as M1 |
| ON (`USE_APPPACK=1 USE_MINI_VFS=1`) | 388704 B (74.14%) | 439300 B (97.50%) |

Profile 0 matches the M1 baseline **in byte count** (the md5 difference comes only from
`__DATE__/__TIME__`). `arkrtos_apppack.c` is filtered into `sources.mk` by `USE_APPPACK` and not
compiled by default.

`ARK_RAMFS_MAX_FILES = 3` (16 KB static buffer per file): `/tmp/p.c` + `/tmp/pp.sct` +
`/tmp/p.svcm` is exactly three, so test scripts must `rm` before writing; a fourth always fails.

---

## 7. Status of M2's criteria

| Criterion | Status |
|------|--------|
| A `cc --pack` → `mini run` prints, exits, blocks returned | **pass** (§5) |
| B the same image runs once at two different landing points | **pass** (§5.2): the same `/tmp/r2.svcm` run twice, landing at `0x24070000` and `0x24071000`, RW blocks at `0x24072000` and `0x24074000`, the program's self-measured landing number `0` and `1`, `R2 alive` both times; after `mini stop` all blocks returned. **Precondition**: this firmware put the mini task priority below the console (`MINI_PRIORITY=22`), otherwise a looping mini program starves the console and the second `mini run` can never be issued — §5.3 |
| C program contains an initialised global / string literal / function pointer / cross-file call | **partial**: `p.c` covers string literals and function pointers (MOVW pairs ×4), **cross-file calls not covered** — ramfs has only 3 slots and `-link` with multiple inputs needs four or more files |
| anti-false-green ① deleting an entry must cause a problem | **pass** (host 6/6, §4.1 item ⑤) |
| anti-false-green ② print the four class counts | **pass** (1 / 1 / 2 / 2) |
| anti-false-green ③ `verify`'s D pass must really run and reject on mismatch | **pass** (injecting a wrong D ⇒ `-32`; table applied back to A compared byte-wise with D) |

**Weak point recorded honestly**: only criterion C's "cross-file call" is unfinished. The
blocker at the time was ramfs's 3 slots — `-link` with multiple inputs needs "two sources + two
objects + scatter file + output", six files, which does not fit; **that blocker has since been
removed by M3-b's NOR persistent volume**, so this item is now "not done" rather than "cannot
be done". It does not change the conclusion "a relocatable image compiled on the board can run
on the board", but **M2's criteria cannot be called all-green either**. Criterion B also used to
be missing and was added this round (§5.2): same image, two landing points, block number
measured by the program itself, identical `crc` and entry offset.

---

## 8. Known boundaries

| Boundary | Status | Owner |
|------|--------|-------|
| single-file sources | `cc` compiles one `.c` at a time; `-link` is wired but ramfs lacks slots | after M2 (slots or persistence) |
| image on VFS ramfs | lost on power loss | M3-b (QSPI W25Q64 + littlefs) |
| board artifact 108 B smaller than host (3006 vs 3114) | **resolved (2026-11-09)**: the two were not the same optimisation level — on-board `cc` defaults to `-O1`, host `arkcc.exe` defaults to `-O0`. At the same level the two artifacts are **byte-identical** (see the error-code and regression-ledger page §6.1: `-O0/-O1/-O2` give 1528/1444/1444 B, all equal) | closed, no longer a pending item |
| 80 KB pool | long sources exhaust the pool in the `-O1` full pipeline ⇒ test sources must be short. Measured: an 809 B source hits `[arkc] FATAL: memory pool exhausted` in pass A (pool 71304/81920 B); trimmed to 569 B all four passes complete | known, not fixed (the error is explicit, not a silently bad image) |
| mini default priority above the console | a looping mini program makes the console unresponsive and only a reset recovers; `MINI_PRIORITY` can lower it (§5.3), the kernel default is unchanged. **Since 2026-11-07 the demo/regression project defaults to `MINI_PRIORITY=20`** (below the console's 14); `make MINI_PRIORITY=` reproduces the kernel default | current design + an escape hatch |

---

## 9. Related code locations

| File | Content |
|------|---------|
| `kernelsrc/src/arkrtos_apppack.c` / `include/arkrtos_apppack.h` | four-pass differencing, table assembly, D-pass verification; error codes `-30/-31/-32/-33/-34` |
| `tools/pack_app.py` | Python implementation of the same algorithm (the other half of the PC comparison); this round aligned its scan order and alignment gate to C |
| `kernelsrc/include/arkrtos_app_image.h` | image header, entry encoding (`(off/2)<<2 | kind`), the four kind values |
| `kernelsrc/components/arkc/arkc_shell_cmd.c` | `--pack` wiring (four links + apppack + write to disk) |
| `kernelsrc/components/arkc/arkc_port_arkrtos.c` | per-pass arena reset and capture hooks |
| `kernelsrc/src/arkrtos_mini.c` | reads the image from ramfs when `ARKRTOS_MINI_READ_VFS=1` (M3-a) |
| `kernelsrc/src/arkrtos_vfs.c` | `arkrtos_vfs_read_at(path, buf, len, off, &got)` |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | `USE_APPPACK ?= 0`, `MINI_PRIORITY ?=` (empty = keep `APP_TASK_PRIORITY`) |
