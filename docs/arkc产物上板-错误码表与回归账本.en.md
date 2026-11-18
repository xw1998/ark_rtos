# arkc Artifacts on Board — Error Codes, Size Ledger and Board Regression

> Condensed English edition. The Chinese page (arkc产物上板-错误码表与回归账本.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

This page is the landing record for **M4 (close-out)** of
[arkc编译产物上板运行-实施计划.en.md](arkc编译产物上板运行-实施计划.en.md), and the index /
reconciliation page for M1 ([fixed window](arkc板上直跑-M1实机记录.en.md)) and
M2 ([relocatable](arkc板上直跑-M2可重定位镜像.en.md)):

- which code to look at when a load fails (§2, §3);
- how much got compiled in (§4);
- how to re-run these conclusions in one command (§5).

Date: 2026-11-05. All readings come from the real board (COM3 / 115200) and clean builds.

---

## 1. Why this page exists

On the M1/M2 paths the same "it will not run" has five completely different causes: the image was
not linked for this window, one position in the relocation table cannot be explained, the CRC does
not match, the block cannot be taken from the pool, the task table is full. Their **fixes differ**,
so the codes must stay separate — an error code degraded to `-1` throws away the "where do I look"
information too.

Likewise "it ran" cannot be a conclusion: a plausible line on the serial port may be the residue of
a previous image, or the program may sit on a wrong landing and happen to read an address that does
not fault. So the M1/M2 criterion must be **a source-only string + the landing number the program
measures itself + the block returned after exit**, and all three must be checkable automatically (§5).

---

## 2. Error codes of the three load paths (one table, read per path)

Every path uses the same `ARKRTOS_LOADER_ERR_x` set (`kernelsrc/include/arkrtos_loader.h`); only
cases unique to one path get their own code. **Two numbers are deliberately shared across paths**
(same digit, different meaning) — the price is that the message text must carry the path name,
otherwise a reader will misread it.

| code | `loader` (install/App) | `mini` (payload in a file) | `apprun` (fixed-window direct run) | `flashapp` (run from Flash) | `apppack` (pack time) |
|----|----|----|----|----|----|
| -1 | PARAM illegal argument | same as loader | — | same as loader | PARAM pointer/window/length illegal |
| -2 | MAGIC image header magic wrong | same as loader | — | same as loader | — |
| -3 | COMPAT hardware signature mismatch | same as loader | — | same as loader | — |
| -4 | SIZE length illegal / exceeds space | same as loader | — | same as loader | — |
| -5 | CRC32 failed | same as loader | — | same as loader | — |
| -6 | FLASH erase/write failed | — | — | — | — |
| -7 | NO_SLOT no free slot | — | — | — | — |
| -8 | TASK task registration failed | same as loader | — | same as loader | — |
| -9 | STATE slot state not allowed | same as loader | — | — | — |
| -10 | ADDR landing illegal | same as loader | — | — | — |
| -11 | ENTRY entry offset out of range | same as loader | — | — | — |
| -12 | OCCUPIED interval overlap | same as loader | — | — | — |
| -13 | RELOC table illegal / inconsistent after patching | same as loader | — | — | — |
| -14 | NOSPACE not enough space in the pool | same as loader | — | — | — |
| -15 | BUSY image is running | BUSY instance count full / a load already in progress | — | — | — |
| -16 | VERSION version rollback | — | — | — | — |
| -17 | DUP duplicate copy | — | — | — | — |
| -18 | SIGN signature failed | **FS: image read failed** (reason in `read: %d`) | — | — | — |
| -19 | — | DISABLED crashed up to the limit, disabled | — | — | — |
| -20 | — | **SHORT_READ hit end of file** | — | **BUSY one is already running** | — |
| -21 | — | — | — | WINDOW could not take the block from the pool | — |
| -22 | — | — | — | PROLOGUE first halfword is not an arkc prologue | — |
| -23 | — | — | — | LAYOUT image is not linked for this window | — |
| -30..-34 | — | — | — | — | PARAM / DIFF / VERIFY / SIZE / ENTRY |
| -40 | — | — | — | **BUSY a direct run / write already in progress** | — |
| -41 | — | — | — | WINDOW data window unavailable (held by `app run` / pool table full) | — |
| -42 | — | — | — | LAYOUT artifact is not linked for this Flash region (SP/entry out of range) | — |
| -43 | — | — | — | PROLOGUE entry first halfword is not an arkc prologue | — |
| -44 | — | — | — | META no valid artifact in the region (never written / erased / metadata bad) | — |
| -45 | — | — | — | VERIFY read-back differs (what is in Flash is not the file) | — |
| -46 | — | — | — | FLASH low-level erase/write failed | — |

The two overlaps: **-18** (loader signature failure vs mini read failure), **-20** (mini short read
vs apprun already running). Hence the shell text is always written as
`mini run: <path> failed, rc=-18` + `read: -1 (...)`, never as a bare number.

---

## 3. Where the five typical failures land (plan §M4 asks for one row each)

| symptom | code | source | measured evidence |
|------|----|------|----------|
| **link table overflow** (symbols/relocations beyond arkc's `LD_MAX_*`) | arkc's own `ld_table_overflow`, not the codes above | `kernelsrc/components/arkc/src/compiler.c` | M0 measured single-file programs far below 64 symbols; with four-pass packing it shows up as `cc: pass X aborted: ...` on one pass rather than as a bad image |
| **four passes disagree** (some position is not explained by one base shift) | `-31 DIFF` | `arkrtos_apppack.c` | host injecting `C_FILE=pD.bin` ⇒ `-31`; `DELTA` not matching the D used ⇒ `-31` |
| **entry signature mismatch** | pack time `-34 ENTRY`; run time `-22 PROLOGUE` | same / `arkrtos_apprun.c` | the `-34` text says "compile with --vector-table"; M1 feeding a non-arkc artifact to `app run` ⇒ `-22` |
| **window occupied** | M1 `-21 WINDOW`; mini `-15 BUSY` | `arkrtos_apprun.c` / `arkrtos_mini.c` | measured this round: with a mini instance still holding `0x24070000`, `app run` reports `rc=-21 - the run window could not be taken`; with two mini instances, `mini run` reports `rc=-15 running 2 of 2 allowed` |
| **image exceeds cap** | `-33 SIZE` (table or output buffer too small / entry beyond `ARKRTOS_APP_RELOC_MAX`) | `arkrtos_apppack.c` | `cc --pack` checks `CC_PACK_OUT_CAP` and the window before packing and refuses rather than truncating |

Two more that only appear on the board:

- when `cc --pack` cannot get its 32 KB + 16 KB pool blocks it **refuses the lend instead of
  shrinking**: `cc: --pack needs 32 KB + 16 KB from the RAM pool; nothing free (the lend is refused, not shrunk)`.
  Measured trigger this round: small programs fragmenting the pool (two instances holding 4 KB + 8 KB
  each), **not** a shortage of total memory.
- arkc's pool exhaustion reports `[arkc] FATAL: memory pool exhausted` + `cc: pass A aborted: memory pool exhausted`,
  plus `cc: pool <used>/81920 B`. Measured this round: an 809 B source hits it already in pass A;
  trimmed to 569 B all four passes succeed.

---

## 4. Size ledger (clean build, measured one by one after `make clean`)

| `USE_ARKC` | `USE_APPRUN` | `USE_APPPACK` | `USE_MINI_VFS` | FLASH | RAM |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | **190600 B** (36.35%) | 147036 B (32.63%) |
| 0 | 1 | 0 | 0 | 193292 B (36.87%) | 147068 B (32.64%) |
| 1 | 0 | 0 | 0 | **381808 B** (72.82%) | 439284 B (97.50%) |
| 1 | 1 | 0 | 0 | 385768 B (73.58%) | 439316 B (97.50%) |
| 1 | 0 | 1 | 1 | 388704 B (74.14%) | 439300 B (97.50%) |
| 1 | 1 | 1 | 1 | 391408 B (74.66%) | 439332 B (97.51%) |

How to read it:

- **arkc is the real heavyweight of this build**: `USE_ARKC` 0 → 1 costs FLASH +191 KB and
  **RAM +292 KB** (the compile pool and work areas are static). Compiling apps on the board is
  almost exactly this one row; the M1/M2 switches are rounding error
  (apprun +3.9 KB, apppack +6.9 KB, mini VFS +2.9 KB FLASH).
- RAM is already at 97.5%, near the ceiling of this board (440 KB kernel window): adding features
  needs a partition-table change, not another switch.
- All M1/M2 switches default off, and the 0 row is **byte-identical** to its baseline
  (381808 B) — that is the "zero change" criterion.

---

## 5. Board regression: `tools/final_app.py`

Turns the M1/M2 criteria into one command. **Every case is PASS or FAIL**, never "looks fine".

```sh
py -3 tools/final_app.py            # flash build/ARKRTOS_TEST.bin then run all cases
py -3 tools/final_app.py --noflash  # board already has the firmware, just run
py -3 tools/final_app.py --list     # list cases
py -3 tools/final_app.py --only m2-pack
```

Prerequisites: board on `--port` (default COM3); firmware built with
`USE_ARKC=1 USE_APPRUN=1 USE_APPPACK=1 USE_MINI_VFS=1`; `m2-two-landings` additionally needs
`MINI_PRIORITY` lower than the console (otherwise a looping small program starves the console and
the second `mini run` never gets sent).

| case | what it proves |
|------|------------|
| `board-clean` (prelude) | the previous round's small programs are stopped, pool and window are empty |
| `m1-fixed-window` | the `.bin` compiled on board runs in the fixed window, prints a source-only string, returns the window on exit, and `fault` holds no real exception |
| `m1-wrong-window` | swapping the `.sct` for another base ⇒ must be `rc=-23`, and `M1 alive` **must not** appear (anti-false-green) |
| `m2-pack-run` | four-pass differential packing succeeds (A/B/C/D equal length, all four table-entry counts non-zero), `mini run` prints, block returned after `mini stop` |
| `m2-two-landings` | the same `.svcm` run twice, landings `0x24070000` / `0x24071000`, block numbers the program measures itself `0` / `1` |

Measured this round: `all 4 cases PASS`.

### 5.1 Four pitfalls the script itself hit (kept as comments — do not change them back)

| pitfall | symptom | conclusion |
|----|------|------|
| **anchor at line start** | all four cases FAIL although the log clearly contains the output | kernel logs carry the `[  15.837] <6>` prefix and program output often follows the prompt `ark@arkrtos:~$ ` ⇒ a "source-only string" may only be anchored at **end of line**, so the end anchor proves it is not a prefix of someone else's line |
| **not stopping the small program first** | `app run` reports `-21 window could not be taken`; `cc --pack` reports `32 KB + 16 KB not available` | the small program holds exactly `0x24070000` and also fragments the 64 KB pool ⇒ every case starts with `mini stop` (the prelude case `board-clean`) |
| **not clearing `/tmp`** | `cc --pack` reports `cannot write ...: I/O error` | ramfs has only 3 file slots ⇒ every case begins by `rm`-ing all known filenames (`CLEAN`) |
| **asserting `fault` is empty** | `fault` contains `TASKKILL` entries | **`mini stop` records every task it kills as a TASKKILL entry** (measured: 4 entries after two clean-ups) ⇒ the criterion became "none of the four CPU exception names may appear" |

The common thread: **the place the failure message points at is often not the real cause.** So every
"action added just to make it run" in the script (clean-up, stopping instances) must be able to
report FAIL itself instead of quietly papering over the state.

---

## 6. Still not done (honest accounting)

| item | status | owner |
|----|------|------|
| M2 criterion C "cross-file calls" | **still not done, but unblocked**: `-link` is wired but has never been used (needs two sources + two objects + a scatter file + the artifact — six files, ramfs has 3 slots). The M3-b NOR persistent volume already works (see `arkc板上直跑-M3b-QSPI持久化.en.md`), so putting the six files on `/mnt/nor` is enough — it is **not done**, not undoable | next round, case goes into `tools/final_app.py` |
| the 108 B difference between board and host artifacts | **resolved (2026-11-09)**: the two sides were not at the same optimisation level — board `cc` defaults to `-O1`, host `arkcc.exe` to `-O0`. Pinning both to one level makes them **byte-identical** (§6.1) | conclusion landed, "pending" withdrawn |
| M3-b (QSPI W25Q64 + littlefs persistence) | **done (real board, 2026-11-05)**: artifacts written to NOR, still there after reset, loadable by `mini run`; after switching the read side to QUADSPI memory mapping `ls /mnt/nor` went 258 s → 0.249 s. **But** writes/erases still bit-bang (~24.8 s for 3 KB), boot still needs a manual `fs mount` + `mount /mnt/nor`, and cross-reset autostart does not hold — see the seven limits in §10 of that page | see the M3-b page; this round only reconciled the books |

### 6.1 The "108 B difference": not a compiler divergence, a wrong reference level

**Symptom**: the same `m1.c` + `m1.sct` gives 1444 B from board `cc` and 1528 B from host
`arkcc.exe`, i.e. −84 B; the earlier M2 case was 3006 vs 3114 (−108 B). Both were logged as
"cause unknown".

**How it was pinned down** (earlier rounds took detours; recorded here so they are not repeated):

1. Stop guessing at `.bin` bytes; capture `cc -S` **assembly text** instead (`cat` is lossless for
   printable ASCII) and reconcile board assembly line by line against each host level.
2. The first comparison shows "board artifact = host `-O2` artifact minus DWARF directives" — the
   board had constant-folded `i*4`, the old host reference still had `MOV/PUSH/POP/MUL`.
3. Regenerating all three host levels: **`-O0` is 257 lines / 1528 B, `-O1` and `-O2` are both
   215 lines / 1444 B**. So what had been taken for "host -O1" was really **-O0**; the `MUL`
   disappeared because of a one-level difference, not because the board silently raised the level.
4. Reading the code found nowhere that could raise it: `config_init()` sets `g_config.opt_level`
   to 0; `cc_reset_state()`/`cc_pack_fill()` in `arkc_shell_cmd.c` both set `opt_level`
   (`-O` parsing, 1 by default); `compiler.c:2229` takes `max(command-line level, g_config.opt_level)`;
   `config.c:237`'s `-O` parsing only turns on inline/vectorize at `>=2`, and nothing ever sets 1 to 2.

**Measured on board (regression firmware, `cc` and `arkcc.exe` from the same `ca969c6`)**:

| level | host `arkcc.exe` | board `cc` | result |
|------|-----------------|----------|------|
| `-O0` | 1528 B | 1528 B | byte-identical |
| `-O1` | 1444 B | 1444 B | byte-identical |
| `-O2` | 1444 B | 1444 B | byte-identical |

`-O1`/`-O2` being the same size on this source is one fact: the file has a single function with
nothing to inline, so the extra `opt_inline`/`opt_vectorize` of `-O2` have nothing to do.

**Re-run**: `python <workspace>/board_cmp_lvl.py` (all three levels), `board_cmp_o1.py` (one level).
Both pull bytes back with `cat`, decode by the shell escaping rules, and compare byte by byte with
the host artifact; the decoded length must equal the `stat` size, otherwise the script itself rules
"the channel dropped bytes, the conclusion does not hold".

**One limitation still on record**: the `cat` channel is ambiguous when the data contains a literal
`0x5C` (the script prints the ambiguous positions); this time both levels had `0` ambiguous
positions, and larger projects exceeding the 3 ramfs slots did not go through this path.

### 6.2 No riscv32 backend on board (trimmed build) — RV objects must be refused, not patched as ARM

The STM32 profile (`ARKCC_PROFILE=STM32_MIN` / `CROSS_COMPILE`) sets `ARKCC_BACKEND_RISCV32` to 0,
so this on-board arkc **does not compile `backend_riscv32.c`**: `ark_backend_find("riscv32")`
returns NULL and `-target riscv32` is simply unreachable on board.

Upstream 0.9 added `-c`/`-link` for riscv32 (objects carry machine code; two kinds of relocation,
cross-object `auipc` pairs and `.word` absolute addresses), so `src/compiler.c` gained a batch of
references that exist only in host builds (nine `rv_get_*` getters, `g_rv_obj_mode`,
`rv_patch_reloc`). All of them **sit inside `#if ARKCC_BACKEND_RISCV32`** — without that, the
on-board link stage would emit a run of `undefined reference to 'rv_get_symbol_count'` and the
firmware would not build at all.

That `#else` is not an empty statement, it is an explicit error:

```
this build has no riscv32 backend, so the riscv32 relocation at offset <n>
for '<sym>' cannot be applied; no image was written
```

because the correct consequence of "the function does not exist" is to **refuse the object**, not to
hand it to ARM's `ld_patch_ref` — that would write Thumb branch encodings into a RISC-V object and
still exit 0. The same logic appears on the host side as the two-way check of the
`__arkcc_isa_riscv32` descriptor (see "RV32I object mode" in the upstream README).

Size cost: this round (upstream 0.9's RV object mode plus the ISA check) the H743 clean build is
`text 412972 / data 2256 / bss 463532`, 792 B more than the previous `text 412180`, FLASH 79.20%.
The board can only compile ARM, so these bytes are "never reachable but must exist" guards and are
not trimmed further (trimming down to `#error` would make the board behave differently from the host
when handed an RV object).

**2026-11-08 appendix (upstream `f1995fa`: RV backend gains inline assembly / `__svc` / `__smc` /
packing-lane arithmetic, plus a silent wrong answer in `opt_vectorize` fixed)**: the board size
**did not move a single byte** — `text 412972 / data 2256 / bss 463532`. That is not "it was not
compiled in"; each of the three changes falls exactly outside the compile boundary, checked one by one:

| upstream change | why the board size does not change |
|---------|--------------|
| five new cases plus `ecall`/`ebreak` in `backend_riscv32.c` | the whole file is not in the STM32 profile (`ARKCC_BACKEND_RISCV32=0`), not one line is compiled |
| vectorisation narrowing fix and `volatile`/`__packed` rejection in `opt.c` | the only call site of `opt_vectorize` is inside `#if ARKCC_FEATURE_OPT_VECTORIZE`, which is 0 on board ⇒ the function has no reference and `--gc-sections` drops it whole (map shows `.text.opt_vectorize 0x0 0x3c4`) |
| RV getter trimming in `compiler.c` | identical to the 0.9 round, unchanged this round |

One reproducible criterion is left on record: compiling `opt.c` alone with the same `CFLAGS_ARKC`
gives **+96 B** (3116 → 3212) in the object file, but the difference goes to zero after linking —
**the object grew, the firmware did not**, for the reason in the second row. This is recorded
because "size unchanged" is easily read as "the change did not take effect", and here both are true
at once.

---

## 7. Related files

| file | contents |
|------|------|
| `tools/final_app.py` | board regression (M1/M2 cases + prelude clean-up; `--only` / `--noflash` / `--list`) |
| `docs/arkc板上直跑-M1实机记录.md` | M1 entry contract, window checks, priority and anti-false-green |
| `docs/arkc板上直跑-M2可重定位镜像.md` | M2 four-pass differential, the five silent-wrong-answer pitfalls, two-landing criterion |
| `kernelsrc/include/arkrtos_loader.h` | the single definition of `ARKRTOS_LOADER_ERR_x` |
| `kernelsrc/include/arkrtos_apprun.h` / `arkrtos_mini.h` / `arkrtos_apppack.h` | each path's own codes |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | all switches (`USE_ARKC` / `USE_APPRUN` / `USE_APPPACK` / `USE_MINI_VFS` / `MINI_PRIORITY`) |

---

## Dated size appendices (upstream bumps, board build = STM32H743, `USE_ARKC=1`, same CFLAGS / linker script)

**2026-11-08 appendix (`d412208` + `2f3da38`: ARM 16-byte `_Complex` return moved to `r0-r3`, RV32I
`_Complex float` return value loss fixed, `(*fp)(params)` keeps the parameter list)**: this round the
board size **really changed**, unlike the previous round's "outside the compile boundary, not one byte":

| item | old | new | delta |
|---|---|---|---|
| `text` | 412 972 | 428 532 | **+15 560** |
| `data` | 2 256 | 2 260 | +4 |
| `bss` | 463 532 | 463 528 | −4 |
| FLASH usage | 79.20% | **82.17%** | +2.97 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | same | 0 |

Per object file (same `CFLAGS_ARKC`, `text` column of `arm-none-eabi-size`):

| object | old | new | delta |
|---|---|---|---|
| `backend_arm.o` | 78 572 | 90 110 | +11 538 |
| `irgen.o` | 24 386 | 27 797 | +3 411 |
| `sema.o` | 5 495 | 6 063 | +568 |
| `parser.o` | 22 051 | 22 075 | +24 |
| `ir.o` | 1 706 | 1 710 | +4 |
| `compiler.o` | 18 826 | 18 834 | +8 |

Reason: `ARKCC_FEATURE_COMPLEX` at that time gated only two parser syntax spots; the machine
backends' four-word complex value paths (16-byte `IR_STORE`/`IR_LOAD`, real/imag extraction,
`float ↔ double` widening, return-register expansion) were **compiled in unconditionally**; the
RV32I backend is wholly outside the STM32 profile, so none of the RV-side new code entered the
firmware. `bss` losing 4 bytes is symbol layout, not a deleted object; `data` gaining 4 bytes is a
new complex initialiser constant.

Verdict: FLASH 82.17%, RAM/CCMRAM unchanged, still inside budget, **accepted**.

> **2026-11-09 update**: the note written then — "trimming per build needs gates in irgen and the
> four backends; logged as debt, no claim that it can be switched off" — **has been done**. With the
> same CFLAGS plus `-DARKCC_FEATURE_COMPLEX=0`, **13 824 B** are recovered
> (`text 438812 → 424988`, FLASH 84.13% → 81.49%). Mechanism and measurements are in the
> "Debt ④" section at the end of this file; the original verdict is left untouched here because it
> is the factual record of that moment. The ~1.7 KB gap to 15 560 is the shared double-precision
> path and constant serialisation, which the gate cannot switch off; not chased further.

Reproducible criterion: `git stash push -- kernelsrc/components/arkc` → rebuild gives `text 412972`;
`git stash pop` → rebuild gives `text 428532`. Except for arkc sources everything (CFLAGS, linker
script, other objects) is identical between the two builds.

**2026-11-08 appendix (`060b008`: RV32I large-struct returns implemented per psABI sret, ARM's sret
decision moved onto an IR bit, `ARK_MAX_SRET_FUNCS` redefined as "per-function cap on large-struct
return call sites")**:

| item | old | new | delta |
|---|---|---|---|
| `text` | 428 532 | 428 740 | **+208** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 528 | 463 528 | 0 |
| FLASH usage | 82.17% | 82.21% | +0.04 pp |

| object | old | new | delta |
|---|---|---|---|
| `irgen.o` | 27 797 | 28 013 | +216 |
| `backend_arm.o` | 90 110 | 90 110 | 0 |

**Why only irgen changes**: the RV32I backend is wholly outside the STM32 profile, so the psABI sret
implementation never enters the firmware; on the ARM side "delete the name-lookup `sret_lookup`/
`sret_get_size` and the per-translation-unit table build" is traded for "read `IR_CALL` bit36 /
payload field + 8-byte-align each buffer + report on overflow", and the two are exactly break-even
(the compiled output differs byte-wise but `.text` is the same size). `bss` is unchanged because
`sret_call_buf_off` is a stack array and the board build was already at 64 entries.

Verdict: FLASH 82.21%, RAM/CCMRAM/NOINIT unchanged, inside budget, **accepted**.

**2026-11-08 appendix (`80f98b4`: function-type typedef `typedef Ret F(args);`)**: `text`
428 740 → 430 188 (**+1 448**), `data` 2 260, `bss` 463 528, FLASH 82.21% → 82.49% (+0.28 pp).
Per object over the full 122-object table: `parser.o` 22 075 → 23 482 (+1 407), the other 121
unchanged. Segment-level: `.text` 357 620 → 358 796 (+1 176), `.rodata` 70 232 → 70 504 (+272),
total +1 448; `parser.o`'s own text column is +1 407, and the 41-byte gap comes from segment
alignment padding (the `.rodata` growth moved an alignment boundary), not from other code changes.
Reason: the RV32I backend is wholly outside the STM32 profile, so `rv_*` tests never enter the
firmware; the ARM side adds pure front-end code (`parse_func_type_tail` / `params_from_func_type` /
two typedef registrations / function-type rejection in `sizeof` and `_Alignof` / the
`if (base) goto done;` in `parse_declspec`), and `irgen.o` / `backend_arm.o` being byte-identical is
the positive evidence for "only the parsing layer moved, code generation did not". Verdict:
FLASH 82.49%, RAM/CCMRAM/NOINIT unchanged, inside budget, **accepted**.

**2026-11-08 appendix (`30502d0`: global address-constant initialisers, C11 6.6p9)**: `text`
430 188 → 432 628 (**+2 440**), `data` 2 260, `bss` 463 528 → 463 532 (**+4**), FLASH 82.49% →
82.95% (+0.46 pp), RAM/CCMRAM/NOINIT unchanged.

| object | text old -> new | delta | bss old -> new |
|---|---|---|---|
| `irgen.o` | 28 013 -> 29 810 | +1 797 | 12 -> 16 |
| `backend_arm.o` | 90 110 -> 90 742 | +632 | 9 428 -> 9 428 |
| `ir.o` | 1 710 -> 1 718 | +8 | 4 -> 4 |
| other 119 objects | — | 0 | — |
| total (text of 122 .o) | 492 081 -> 494 518 | +2 437 | — |

ELF `text` +2 440 is 3 bytes more than the object total +2 437, from segment alignment padding.
Where it lands: `irgen.o` is the bulk — a new address-constant walker (`addr_elem_size` /
`addr_value` / `addr_lvalue` / `looks_like_address` / `irgen_try_addr_init`) + three branches in
`serialize_const_expr` (`AST_ARRAY_ACCESS`/`AST_MEMBER`/`TOK_BAND`) + five call sites and five
`g_ser_unknown` closings; `bss +4` is the new `static int g_ser_unknown`. `backend_arm.o` +632 is
`init_sym`'s generation branches (`LDR r0,=name` forwarding / `emit_mov_addr32(base+offset)`) plus
three zero-scalar guards. `ir.o` +8 is the initialisers for three `IrGlobal` fields. **The RV32I
backend is wholly outside the STM32 profile**, so its `.word sym` branch never entered the firmware —
again the positive evidence for "new path only, existing code generation untouched" (the other 119
objects are column-identical). Verdict: FLASH 82.95%, RAM/CCMRAM/NOINIT unchanged, inside the
512 KB budget (89.05 KB left), **accepted**.

**2026-11-08 appendix (`ce98151`: address elements in aggregate initialisers + function-pointer
array declarators)**: `text` 432 628 → 435 012 (**+2 384**), `data` 2 260, `bss` 463 532, FLASH
82.95% → 83.41% (+0.46 pp). Only 5 arkc objects change:

| object | text old -> new | delta |
|---|---|---|
| `irgen.o` | 29 810 -> 31 242 | +1 432 |
| `backend_arm.o` | 90 742 -> 91 242 | +500 |
| `parser.o` | 23 482 -> 23 802 | +320 |
| `agginit.o` | 2 728 -> 2 868 | +140 |
| `ir.o` | 1 718 -> 1 722 | +4 |
| five together | — | +2 396 |

ELF `text` +2 384 is 12 bytes less than the object total +2 396 — segment alignment/padding, not
other code changes. Where it lands: `irgen.o` is the new second AST walk (`irgen_add_ref` /
`irgen_agg_addr_scalar` / `irgen_agg_addr_walk` / `irgen_init_has_addr` / `irgen_try_agg_addr_init`)
plus 9 call sites; `parser.o` is `(*name)` support for array dimensions inside parentheses;
`agginit.o` is `ark_agg_fill_ex()`'s `addr_hook` and "advance the text cursor after a hit";
`backend_arm.o` is `arm_agg_addr_hook()` and the loop that overwrites addresses from `init_refs`
after aggregate initialisation. `bss` unchanged — no new file-scope variables. **The RV32I backend is
still not in the STM32 profile**, its `.word sym` branch never entered the firmware. Verdict:
FLASH 83.41%, RAM/CCMRAM/NOINIT unchanged, inside the 512 KB budget (84.59 KB left), **accepted**.

**2026-11-08 appendix (upstream `b8378f4`: string-literal elements in aggregate initialisers)**:
`text` 435 012 → 435 652 (**+640**), `data` 2 260, `bss` 463 532, FLASH 83.41% → 83.53%
(+0.12 pp). Only 2 arkc objects change: `irgen.o` 31 242 → 31 700 (+458), `backend_arm.o`
91 242 → 91 423 (+181), two together +639 (ELF +640 is 1 byte of alignment). Where it lands:
`irgen.o` is `irgen_add_ref_ex()` (trailing `str` parameter), the `AST_STR` branch of
`irgen_agg_addr_scalar()` (including the wide-literal error) and the new type-aware scan
`irgen_init_has_str_addr()`; `backend_arm.o` is the loop emitting `.L_gstr_<gi>_<off>` labels per
element offset into `.rodata`, plus the `if (r->str)` dispatch in the refs write loop. `bss`
unchanged. The RV32I backend's `.Lgstr_<name>_<off>` and `.word` branches never entered the
firmware (positive evidence again). Verdict: FLASH 83.53%, inside budget (84.34 KB left),
**accepted**.

**2026-11-08 appendix (upstream `6fce783`: designators and union address elements in aggregate
initialisers + designer subscript folded to a constant)**: `text` 435 652 → 436 108 (**+456**),
`data` 2 260, `bss` 463 532, FLASH 83.53% → 83.61% (+0.08 pp). `irgen.o` 31 700 → 31 833 (+133),
`parser.o` 23 802 → 24 126 (+324), three together +457 (ELF +456, 1 byte of padding). Where it
lands: `irgen.o` is the walk rewrite (new `irgen_agg_addr_place()`, the `cur` cursor in the array
branch, the `f` cursor and `.name=` relocation in the struct/`union` branch, the single-active-member
restriction on `union`); `parser.o` is the new `parse_designator_index()` (`parse_cond_expr` +
`eval_const_expr` folding, erroring up to the `]`). `backend_arm.o` is 0 — no code-generation path
was touched, only "how the front end fixes the offset of an address element" and "how the parser
reads a designer subscript". Verdict: FLASH 83.61%, inside budget (83.86 KB left), **accepted**.

**2026-11-08 appendix (upstream `c6ebdc6`: brace elision — global data / address elements /
block scope, three routes + dimension inference + scalar braces)**: `text` 436 108 → 438 724
(**+2 616**), `data` 2 260, `bss` 463 532, FLASH 83.61% → 84.11% (+0.50 pp). `irgen.o`
31 833 → 33 436 (+1 603), `parser.o` 24 126 → 24 430 (+304), `agginit.o` 2 868 → 3 588 (+720),
three together +2 627. Segment split (`arm-none-eabi-size -A`, matching the ELF total):
`.text` 362 972 → 365 076 (**+2 104**), `.rodata` 72 248 → 72 760 (**+512**), exactly +2 616.
Where it lands: `irgen.o` is two of the three routes' AST walks (`AaggWalk`'s
`aw_item_value/aw_object/aw_fill/aw_braced` and `LocalAggWalk`'s
`lw_value/lw_designator/lw_object/lw_field/lw_fill`) rewritten wholesale, plus the new
`irgen_unwrap_scalar_braces()` and `_Complex` boundary diagnostics; `agginit.o` is the cursoring of
the global serialisation text fill `ark_agg_flat()` / `ark_agg_object()` (array branch returns `off`
on full, the comma skip moved after the fullness test, the string short-circuit only under
`TYPE_ARRAY`); `parser.o` exports `ark_infer_array_size_from_init()` and `infer_leaf_slots()`.
`backend_arm.o` is 0 — neither backend's code generation changed at all; brace elision is entirely
"flatten the initialiser into (offset, value) pairs" in the front end. Verdict: FLASH 84.11%,
inside the 512 KB budget (81.24 KB left), **accepted**. (Block-scope string initialisation expands
into a `memcpy` call, so this round's block-scope cases really do pull `memcpy` into the link; the
board size already includes it.)

Regression ledger (full re-run this round, host side, `c6ebdc6`):

| gate | result |
|---|---|
| `tests/run_agg_addr_tests.py --arm-as --arm-libgcc --rv-as` | **83 passed / 0 failed** (CASES 32 × 2 backends + MUST_FAIL 8 × 2 + RV asm 3; 9 of them are brace elision) |
| `tests/run_func_ptr_global_tests.py` | 39 / 0 |
| `tests/run_complex_tests.py` | 76 / 0 |
| `tests/run_arm_int64_tests.py` (does not take `--rv-as`) | 12 / 0 |
| `tests/run_c11_keywords_tests.py` (does not take `--arm-libgcc`) | 30 / 0 |
| `examples/run_va_gate.py --arm-as --arm-libgcc --rv-as` | 59 / 0 / 2 targets, `--opt-suite` 5/5 green |
| `run_rv_image` / `alloca` / `sret` / `misc_ir` / `float` / `packed` (`--rv-as`) | 20 / 5 / 13 / 12 / 11 / 7, all 0 failed |
| `tests/run_rv_link_tests.py` | 10 / 0 |
| `tests/run_func_typedef_tests.py` | 24 / 0 |
| `run_capacity_tests.py` / `run_diag_tests.py` / `run_diag_boundary.py` / `run_regalloc_tests.py` | 4 / 0; 0 failure; all boundary held; all claims hold |
| `tests/run_reloc_text_tests.py ./arkcc.exe` | all checks passed |
| `/tmp/runps2.py` | done rc=0 |

Two conventions (hit this round, written down here):
1. `run_agg_addr_tests.py`'s `--arm-as` / `--rv-as` take a **directory** (it finds tools by the
   `arm-none-eabi-` / `riscv64-unknown-elf-` prefix); passing an executable path gives
   `TypeError: 'NoneType' object is not callable`.
2. The gate runners take the **process exit code**, i.e. `main`'s return value **mod 256**; expected
   values in test cases must stay below 256, otherwise "oracle mismatch" is all fake FAIL from
   modular arithmetic.

---

## Debt ④: making `ARKCC_FEATURE_COMPLEX` a real trimming switch

Background: `ARKCC_FEATURE_COMPLEX` used to gate only two parser syntax spots while the machine
backends' four-word complex paths (materialise / extract real-imag / widen / return-register
expansion) were **compiled in unconditionally** — 15.5 KB of firmware for the complex feature, and
flipping the switch did nothing. This round made it a real switch.

Approach (three constant folds + four hand-written `#if`s):

- `include/ir.h`: `IR_AUX_COMPLEX` defined in two flavours — `0x800` when the gate is on, **`0`**
  when off. `arkcc_config.h` changed from an unconditional `#define` to `#ifndef`, otherwise `-D`
  would be overridden by the header (hit this round: the two builds were byte-identical because of
  exactly this).
- `include/type.h`: new `ARK_IS_CX(t)` — `((t)->kind == TYPE_COMPLEX)` when on, **`0`** when off.
  All 47 `kind == TYPE_COMPLEX` comparisons in `irgen.c` / `sema.c` / the three machine backends
  now use it.
- `src/parser.c`: `case TOK_COMPLEX:` reports in both flavours. **A silent wrong answer was fixed
  along the way** — run-time `-fno-complex` used to *ignore* the keyword, so `double _Complex z`
  silently became `double z`; now it reports
  `` `_Complex` is switched off by the run-time configuration (complex=0) ``.
- Value-driven parts that cannot be folded get hand-written `#if`: `eval_const_cexpr` in `sema.c`,
  `irgen_try_complex_init` in `irgen.c` (the five call sites wrap only that one `if` in
  `#if ARKCC_FEATURE_FCONST_FOLD && ARKCC_FEATURE_COMPLEX`, leaving the `else` outside to keep the
  `else if` chain), and `cplx_ret` in the ARM and RV32I backends (off build becomes
  `const int cplx_ret = 0` and the two branches below fold away).

Size (H743 firmware `example/stm32h743/kernel/ARKRTOS_TEST/GCC`, both builds `make clean` full
rebuild, the only difference being one extra `-DARKCC_FEATURE_COMPLEX=0` at the end of `OPTFLAGS`):

| measure | gate on | gate off | delta |
|---|---|---|---|
| `text` | 438 812 | 424 988 | **−13 824 B** |
| FLASH usage | 84.13% | 81.49% | −2.64 pp |
| `data` / `bss` | 2 260 / 463 532 | 2 260 / 463 532 | unchanged |

Host x86-64 (all four backends): `text 556 740 → 535 484` (−21 256 B).

ARM target set (same `STM32_CFLAGS` as the firmware, per-file `-c` then `size -t` summed, **not**
the firmware link value): `246 554 → 232 926` (−13 628 B); per object `backend_arm.o −8 664`,
`irgen.o −4 264`, `sema.o −608`, `parser.o −92`.

Two conventions (must be read together):
1. With the gate on, the firmware is **88 B** larger than the previous ledger entry (`text 438 724`) —
   that is the new diagnostic (the parser's run-time rejection branch). With the gate on all other
   output is byte-identical, and host-side `text` is still 556 740 (unchanged from before the change).
2. The ~**1.7 KB** gap to the historical `+15 560 B` is not chased: that increment counted "growth
   of the whole complex feature", part of which is the **double-precision path and constant
   serialisation**, which also serve `double` and cannot be gated off; plus the `init_is_complex`
   field wiring and those 88 B. It reports −13 824 (recomputable) and does not write unrecovered
   bytes as recovered.

New gate `tests/run_complex_gate.py` (host; needs both binaries present, **does not build them**):

| check | criterion |
|---|---|
| `full/accepts-complex` | the full build must compile `_Complex` (positive control) |
| `trimmed/rejects-complex` | the trimmed build must fail |
| `trimmed/reason-is-configured-out` | the failure reason must be `is not available in this build` |
| `trimmed/not-a-crash` | must not be a signal / segfault |
| `trimmed/accepts-plain-C` | the trimmed build must still accept plain C |
| `both/plain-asm-identical` | sources without `_Complex` must produce **byte-identical** assembly text in both builds |
| `full/-fno-complex-is-an-error` | the run-time switch must report an error rather than silently ignore |
| `selfcheck/plain-build-has-no-config-msg` | self-check of the self-check: a plain source must not show the trimmed build's message |

Measured (`--arkcc ./arkcc.exe --arkcc-off ./arkcc_off.exe --arm-as … --arm-libgcc … --rv-as …`):
**9 checks, all green** (including `full/complex-semantic-suite` 76 passed / 0 failed).

Regression ledger (gate-on build, full re-run; item by item identical to the previous round, no new
failures): `agg 83/0`, `func_ptr_global 39/0`, `complex 76/0`, `arm_int64 12/0`, `c11 30/0`,
`va_gate 59/0/2 targets + opt 5/5`, `rv_{image,alloca,sret,misc_ir,float,packed}` 20/5/13/12/11/7,
`rv_link 10/0`, `func_typedef 24/0`, `capacity 4/0`, `diag 0 failure`, `diag_boundary all boundary
held`, `regalloc all claims hold`, `reloc_text all passed`, `complex_gate 9/9`.

---

## 2026-11-09: block-scope excess initialisers, silent drop → GCC-style warning

**Background (a known item on the list, not a new feature)**: `int b[2] = {1, 2, 3};` inside a
function is a constraint violation per C99 6.7.9p2 and must be diagnosed. arkc used to silently drop
the extra elements **at block scope** with no message — the conclusion was not false, but the user
believed three elements had been written. That is exactly the shape "rather report an error than a
wrong answer" cannot tolerate (silently swallowing what the user wrote). File scope always had a
diagnostic (`cannot turn the initialiser of the global 'a' into data`), imprecise wording but at
least not silent; **not touched this round**.

**Approach** (`src/irgen.c`, diagnostics only, no byte written changes):

- `LocalAggWalk` gains a shared `int *excess` flag; the `sub` created for a child aggregate in
  `lw_object` copies the pointer down ⇒ `int b[2][2] = {1,2,3,4,5}` warns once for the inner
  overflow, not once per line.
- The array branch calls `lw_warn_excess(w, cap, is_array)` at `cur >= len`; the struct branch at
  "all members filled" (it must first count the member list to get the total — `f` there is only a
  "is there a member left" cursor).
- Brace elision (`int b[2][2] = {1,2,3,4}`) goes through the `!braced` early-return branch and
  **does not** false-warn.
- `diag_tool`'s signature is `(sev, object, msg, a0..a5)` — six strings; numbers must be rendered
  with `diag_num(buf, v)` first, passing an `int` directly segfaults (hit this round, `rc=139`).

**Measured (board `make clean` full rebuild, default build, only the source synced)**:

| measure | before | after | delta |
|---|---|---|---|
| `text` | 438 812 | 439 060 | +248 B |
| FLASH usage | 84.13% | 84.18% | +0.05 pp |
| `data` / `bss` | 2 260 / 463 532 | 2 260 / 463 532 | unchanged |

Host x86-64 (all four backends): `text 556 740 → 557 100` (+360 B). The whole difference is the new
diagnostic text itself, **no initialisation write changes** (the `-S` output is unchanged except for
the diagnostic paths).

Behaviour: `int b[2]={1,2,3};` → `arkcc: Warning: excess elements in the initialiser are ignored: the array holds only 2`, `rc=0`
(consistent with GCC: warning, not error); `struct S s={1,2,3};` (two members) →
`... the object holds only 2`; `int b[2][2]={1,2,3,4};` → **no output** (brace elision, legal).

---

## 2026-11-09: `-mcpu Cortex-M7`'s double-precision switch was zeroed (root cause of `__aeabi_f2d` on board)

**Symptom**: the `mini-sdk-va` case (board `cc` compiling standard C `va_start/va_arg` taking a
`double`) fails at link with `unresolved symbol "__aeabi_f2d"`.

**Root cause in three layers — fixing fewer than all three does not work**:

1. The previous round changed `float` through `...` to be **promoted to `double`** per the C standard
   (correct), which needs a `float -> double` conversion instruction.
2. The board port never told the compiler the target CPU, so the backend takes the conservative path
   and emits `BL __aeabi_f2d`; the small-program SDK runtime
   (`kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c`) has **no `__aeabi_f2d`/`__aeabi_d2f`** —
   that symbol does not exist on board.
3. The deeper layer is upstream: `src/compiler.c`'s `-mcpu` block already computes
   `g_arm_fpu_dp = cpu->has_fpu_dp` from the CPU table, but a later line **`g_arm_fpu_dp = 0;`** zeroes
   it again ⇒ `-mcpu Cortex-M7` looks accepted while `BL __aeabi_f2d` is still emitted. Not a board
   problem: a compiler defect.

**Fix**:

- Upstream `src/compiler.c`: move `g_arm_fpu_dp = 0` from downstream to **before** the `-mcpu` block.
  It must keep "reset" semantics — the board calls `ark_compile()` repeatedly in one process, so a 1
  left by a previous `-mcpu Cortex-M7` must not leak into a compile without `-mcpu`.
- Board port `kernelsrc/components/arkc/arkc_shell_cmd.c`: both `cc_pack_fill()` and the single-compile
  path explicitly set `cpu = "cortex-m7"` (the board is an H743, VFPv5-D16). **No hand-written soft-float
  helper** — use the hardware the board really has.

**Measured**: with `--cpu cortex-m7`, `float->double` emits `VCVT.F64.F32 d0, s0`; without `-mcpu`
it still emits `BL __aeabi_f2d` (conservative path unchanged, not one byte of existing host output
moved). Board regression `mini-sdk-va` goes FAIL → PASS.

---

## 2026-11-09: M2 criterion B now computes from the real landing instead of a hard-coded block size

Criterion B must verify "the program read its own absolute address correctly", so the expected value
must be computed from the base `mini run` reports: `want = (base - 0x24070000) >> 12`. The previous
version hard-coded "4096 B block, landings 0x24070000/0x24071000, print 0/1", but pool blocks are
"the smallest power of two ≥ the payload" — `m2.svcm` with a 1652 B payload gets a 2048 B block,
landings `0x24070000` / `0x24070800`, which are both 0 after `>>12`. Hard-coding the block size turns
the criterion into "how many bytes a block is on this board today", which is not the object under test.

A real bug in the criterion was fixed at the same time: the regex for the program's self-reported
number was written as `^M2 alive\n...`, but the serial echo has the prompt in front
(`ark@arkrtos:~$ M2 alive`) ⇒ it never matched, the case false-FAILed, and because the program is an
infinite loop two leftover instances filled the pool, so **the following three `mini-sdk-*` cases all
failed in cascade**. Dropping the `^` (keeping `re.M`) made it 9/9 green.

---

## 2026-11-09: board regression 9/9 PASS (firmware `FLASH 488988 B / 93.27%`)

Build line (**missing one item false-FAILs**; the previous version gave only
`USE_ARKC/USE_APPRUN/USE_APPPACK/USE_MINI_VFS` ⇒ `/mnt/nor` was not mounted at all,
`make: command not found`, 5 cases red):

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
```

Measured size: `FLASH 488988 B (93.27%)`, `RAM 308240 B (68.41%)`, `SDRAMBUF 10744 KB / 12 MB`,
`SDRAMFS 5882656 B / 6 MB`; `text 486692 / data 2284 / bss 17191012`.

Result (`python tools/final_app.py --noflash`): `m1-fixed-window`, `m1-wrong-window`, `m2-pack-run`,
`m2-two-landings`, `mini-sdk-std-hello`, `mini-sdk-va`, `mini-sdk-missing-main`, `stack-usage`,
`vfs-create` — **all 9 cases PASS**.

---

## 2026-11-09: M3-d cross-reset autostart + regression 12/12 PASS + structural gate 12/12 green

### Three real defects (all only visible on real hardware)

**① Autostart ran before the VFS mount point existed ⇒ every manifest entry returned -18.**
Autostart used to fire inside `arkrtos_fs_mount()`, a function called by
`arkrtos_vfs_mount_volume()` — at that moment `ark_vfs_mount()` has not run and `/mnt/nor` does not
exist. The manifest uses `mini run` syntax (`/mnt/nor/xxx.svcm`), resolved via the mount point, so
every entry failed with `-18`; worse, `g_mini_auto_boot_done` was already set, so **the whole boot
never retried**. Fix: the VFS build moves autostart to after the mount succeeds (a new call site in
`arkrtos_vfs.c`), and `arkrtos_fs.c` gains `&& (ARKRTOS_MINI_READ_VFS == 0)` for non-VFS builds only,
keeping "zero behaviour change when off".

**② The manifest path existence check asked the wrong layer.**
`mini autostart` used `arkrtos_fs_stat_path()` (volume-root coordinates) to look up
`/mnt/nor/c3auto.svcm`, while the kernel reads images through the VFS namespace ⇒
"`mini run` just succeeded, then `mini autostart` reports `not in the volume (fs err -2)`". Fix: the
VFS build asks `arkrtos_vfs_app_stat()`, and the error text goes through `arkrtos_vfs_error_name()`
too — whatever layer you ask, speak that layer's language.

**③ A Kconfig string symbol was generated as `""`, eating the header default.**
`ARKRTOS_AUTOMOUNT_NOR_POINT` defaults to `"/mnt/nor"` in Kconfig, but its dependency
(`ARKRTOS_AUTOMOUNT_NOR`) is `n` in defconfig and the board turns it on **in the Makefile with
`-DARKRTOS_AUTOMOUNT_NOR=$(USE_QSPI)`**. So genconfig parsed an empty value and wrote
`#define ARKRTOS_AUTOMOUNT_NOR_POINT ""` verbatim; `#ifndef` fallback could never apply, the boot
mount point became an empty string, `mount nor0 at : -2`, **the whole NOR was not mounted and 6 cases
went red**. Fix: `tools/kconfig/genconfig.py` **does not generate** empty strings (bool is still
always generated as 0; the reason in that comment does not hold for strings: `""` is a legal value,
so a header default cannot distinguish "configured empty" from "not configured"). This is "rather
error than a wrong answer" landing in a generator — an empty path must not be treated as a usable
mount point.

Also: `main.c` called `arkrtos_fs_last_error()` / `arkrtos_fs_err_is_unformatted()` without including
`arkrtos_fs.h` (implicit declarations, brought in by d32fb84); fixed.

### Packer nominal bases now come from the partition header

`CC_PACK_ROM_BASE/CC_PACK_RAM_BASE` in `arkc_shell_cmd.c` were hard-coded
`0x08000000/0x20000000`; they now use `ARKRTOS_APP_NOMINAL_ROM_BASE` /
`ARKRTOS_APP_NOMINAL_RAM_BASE` (the single source in `config/arkrtos_partition.h`). Semantics
unchanged: those two values only go into the image header and the loader compensates at run time by
"landing − nominal"; what changes is that delta goes from "large offset" to "small offset in the
pool", and the three cases `m2-pack-run` / `m2-two-landings` / `m2-cross-tu-link` measured no
regression. The concrete address in the help text became "the image ROM base" so that `ci_gate`'s
`single-address-source` really has a single source.

### Gates

```sh
env -u PYTHONPATH -u PYTHONHOME <py310> -X utf8 tools/ci_gate.py
# 12 check(s) passed, 0 failed
```

Four added Kconfig symbols (`ARKRTOS_USE_QSPI` / `ARKRTOS_AUTOMOUNT_NOR` /
`ARKRTOS_AUTOMOUNT_NOR_POINT` / `ARKRTOS_USE_ARKMAKE`) turn `features-switches` green; after
regenerating `arkrtos_autoconf.h`, `config-generated` turns green.

### Board regression: all 12 cases PASS

```sh
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
# FLASH 492900 B (94.01%) / RAM 308240 B (68.41%)
# text 490596 / data 2284 / bss 17191012
python tools/final_app.py     # reflashes, because the firmware changed
```

The new case **`mini-autostart-reboot`** is the positive evidence plus anti-false-green for this
chain: `cc --pack` an svcm, `mini run` it by hand to confirm it runs, `mini autostart add` it to the
manifest, then **reset** (`openocd reset run`, no reflash — reflashing changes flash, while this case
must prove "same firmware, same NOR, just boot again and it runs itself"), asserting three lines:
`autostart: /mnt/nor/c3auto.svcm started`, `boot autostart: 1 started from /mini.autostart`, and the
program's own `C3 autostart alive`. Anti-false-green: remove it from the manifest and reset again —
those two kinds of lines **must no longer appear** (if they do, they come from another path, e.g. the
loader's slot autostart).

### Two new conventions registered (reusable)

**⑤ "What happens at boot" must come after the objects it depends on.** Ordering defects do not show
up in unit tests (every function returns success); they only show up on the one real cold boot. The
criterion must cross a reset (`tools/final_app.py`'s `Reset` step) and the anti-false-green must be
"delete the trigger and reset". **⑥ The same path has different coordinates at different layers.**
Before asking about existence, confirm which layer the kernel uses to read it (fs volume-root
coordinates vs VFS namespace coordinates; a mismatch yields "it just ran but reports not found").
**⑦ A generator's empty-value policy is part of the criterion.** `""` and "not configured" must be
representable separately; the `#ifndef` fallback only works when the symbol was **not generated**.

## 2026-11-09: setjmp/longjmp become real functions + three real defects in `cc --pack` multi-object data segments + regression 14/14 PASS

This round filled the last two language-level debts of "a complete usable C compiler": the
`<setjmp.h>` pair and `__thread`. Both **only reach conclusions on real hardware**, and the reading
method is the same: board `make` + `mini run`, expected strings hard-coded, gates carry their own
anti-false-green.

### c6-1 `setjmp`/`longjmp` must be assembly functions, not C wrappers

The SDK gains `kernelsrc/sdk/mini_sdk/arkc/setjmp.h` (`typedef int jmp_buf[20]`, using the first 40 B:
`[0..7]=r4-r11`, `[8]=sp`, `[9]=lr`) and `setjmp.s`. Project template `example/mini_setjmp/`
(`main.c` is pure standard C, `SRCS` includes `setjmp.s`).

**Why not a C wrapper plus a machine-code blob**: `setjmp` must save the **caller's** `sp`/`lr`, and a
C wrapper only ever sees its own frame; after the wrapper returns that frame is reused by the next
call, so `longjmp` restoring `sp` and `bx`-ing back unwinds along the normal return path. This project
measured that wrong answer: expected `longjmp(env,7)` to give main 7, got **99** (`deep(5)` returned
normally and main took its own `return 99`). With `.s` the host unicorn is immediately
`status=returned r0=0` and all five assertions pass.

One more choice pitfall: `MOVS` inside `it eq; moveq` is UNPREDICTABLE on ARMv7-M, so this
implementation uses `cmp r0,#0 / bne .L_nonzero / movs r1,#1`. The internal assembler does not support
numeric local labels (`1:` / `1f`, it reports `unresolved symbol "1f"`), so labels must be `.L_xxx`.
Verified byte by byte in the image: `2900 f040 8001 2101` is `cmp r0,#0; bne.w; movs r1,#1`.

Real board reading (`mini run sj.svcm`):

```
sj: first pass r=0
sj: jumped back r=7 (want 7)
sj: frames abandoned = 6 (want 6)
sj: longjmp(env, 0) -> 1 (want 1)
sj: ok
```

Four boundaries are written in the header and must not be read as "complete C standard implemented":
no VFP state saved, no signal mask, `jmp_buf` layout is private to this implementation, and the same
bytes are mirrored in the arkc repo as `stdlib/src/arm/setjmp.s` (the two must stay identical).

### c6-2 Three real defects (all only visible when packing multiple objects on board)

**Defect ⑧ `cc --pack` does not turn on `--reloc-data`, so two objects with globals collide in SRAM.**
Symptom: `/tmp/ppA.bin: Error: SRAM overlap between objects #1 and #2: both place .data/.bss in
0x24070000..0x240700C8` → `make: link failed (-12)`. Root cause: `enable_reloc_data` defaults to 0 in
`config_init()` and only the `cc` command line sets it, under `-link`/`--scatter`; `cc --pack` takes
another path (it always links, and several objects are in flight). Fix: the wiring file
`arkc_shell_cmd.c`'s `cc_pack_obj()` and `cc_pack_link()` each add
`g_config.enable_reloc_data = 1;` **after** their `cc_reset_state()` (it must be after, `config_init()`
zeroes it). Previously only the hello/va projects (one object with globals) existed, so this never
showed.

**Defect ⑨ Assembly objects lie about the previous translation unit's RAM window.**
After ⑧ the symptom changed: `object #2 declares data but carries no relocatable address (compiled
without --reloc-data)`, and #2 is exactly `setjmp.s`. Root cause: `g_data_sram_start/end`,
`g_bss_sram_start/end` are only written by `codegen_arm()`, and assembly input (`cc -c x.s`) never
enters codegen — the four bounds still hold **the previous** C file's values (rt.c/start.c), so this
object "declares" a RAM block out of thin air, the linker tries to copy it, finds no patch site and
refuses the whole image. Fix upstream (`src/backend/arm/backend_arm.c` gains
`arm_reset_ram_window()`, called from `src/compiler.c`'s assembly-input branch after `mem_pool_reset()`),
then `tools/sync_arkc.py --apply` syncs the vendored copy. Convention: **"this object has no RAM
window" must be stated explicitly, never inherited from the previous object's state.**

**Defect ⑩ (convention registered, not a defect) `__thread` equals a plain global on ARM.**
`__thread int counter = 5;` and `int counter = 5;` generate exactly the same thing: the ARM backend
emits no data segment at all, global initial values are all compiled into `__arkcc_init_globals()`
writing RAM entry by entry, so the `.tbss/.tdata` section choice makes no difference on this path and
there is no TP base either. Single-threaded it gives no wrong answer, but writing it as "one per
thread" silently breaks in multi-threaded use. Real board reading (`mini run tls.svcm`):

```
tls: init=5 (want 5)
tls: after write=9 (want 9)
tls: zeroed=0 (want 0)
tls: ok (single instance, no TP base -- see the header)
```

**No** warning was added for this downgrade (it would break the `0 warnings` assertions in
`tests/arm_exec` and `run_full_test.ps1`, and there is no multi-threaded small program on board); the
boundary is written here and in `example/mini_tls/main.c`'s header, and pinned as a repeatable reading
by an on-board case.

### Board regression: all 14 cases PASS

```sh
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
# FLASH 492940 B (94.02%) / RAM 308240 B (68.41%)
python tools/final_app.py            # reflashes; --noflash reuses the flashed firmware
```

Two new cases:

* `mini-setjmp` — `ls` confirms `/mnt/nor/{setjmp.h,setjmp.s,sj}`, `cat main.c` shows standard
  `<setjmp.h>`, `make` asserts `pack 4 sources` and `ram_movw [1-9]\d*` (proof the multi-object data
  segment was really copied), `mini run` gives all five readings right; `forbid` pins this round's two
  error texts (`SRAM overlap between objects` / `carries no relocatable address`).
* `mini-tls` — three readings plus the "single instance, no TP base" boundary.

Each case hit one **assertion-shape** pitfall (not a device problem, recorded so it is not repeated):
① a small program's first output line is glued to the shell prompt
(`ark@arkrtos:/mnt/nor/sj$ sj: first pass r=0`), so **the first line must not be anchored with `^`**
while other lines keep whole-line anchors — `mini-sdk-va` already did it this way; ② board `cat`
escapes non-ASCII as `\xE5...`, so the end of a `cat main.c` line may be a comment — anchor line
starts only, never assert line ends.

### Three new conventions registered (reusable)

**⑧ `--pack` is inherently "multiple objects + link"**: any data-segment semantics invisible with a
single object (`reloc_data`, `reloc_text`) must not inherit the single-object defaults; the test is to
ask "can a second object with globals be in flight on this path".

**⑨ Every object must explicitly declare what it has and has not.** When compiling repeatedly inside
one process, "static state left by the previous unit" is inherited by the next, and what is inherited
is invisible in single-object scenarios; the fix is to zero it at entry, not to judge at exit.

**⑩ `__thread`'s portability must be asked per target backend, not per language standard.**
"the compiler knows the `__thread` keyword" and "the target has one per thread" are two different
things; on this chain only the former holds.

---

## 2026-11-09: stage 3c — artifact flashed into on-chip Flash and executed from there (`app flash`) + regression 16/16 PASS

Everything before this kept artifacts alive in RAM, lost on power-off. This round added the last
segment: the image is written to `0x081E0000 + 128K` (tail of bank2) and executed with instruction
fetch from Flash, still there after reset.

### Measured readings

```sh
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1 USE_FLASHAPP=1
# FLASH 497700 B (94.93%) / RAM 309360 B (68.66%)
python tools/final_app.py            # all 16 cases PASS
python tools/ci_gate.py              # 12 check(s) passed, 0 failed
```

```
cc: done in 58 ms                                   -> 1452 B
app flash write /tmp/m5.bin
    FLASHAPP:410 /tmp/m5.bin: 1452 B, crc 0x612B4A1E -> 0x081E0000..0x081E05AC (verified)
app flash info
    flash app: 1452 B, crc 0x612B4A1E, entry 0x081E0424, area 0x081E0000 +128K
app flash run
    FLASHAPP:412 1452 B from flash 0x081E0000, entry 0x081E0424, data 0x24072000 (8192 B), task 4
    M5 alive in flash
    M5 tag bravo
    FLASHAPP:413 task 4 exited, data window returned to the pool
(reset; ramfs cleared, ls /tmp has no m5.bin)
app flash info / run                                -> still succeeds
app flash erase;  app flash run                     -> rc=-44
```

### Two new cases

* `m5-flashapp` — `info` on an empty region reports `-44` → `cc --scatter --vector-table` → `write`
  (with length / CRC / target address / `(verified)`) → `info` → `run` → **Reset** → `ls /tmp` has no
  `m5.bin` → `info`/`run` still succeed → `erase` → `run` reports `-44` → `app flash bogus` prints
  usage → no exception throughout.
* `m5-wrong-window` — first write a good image successfully, then write a bad one with the **RAM
  window shifted 4 KB** (initial SP becomes `0x24075000`); it must report `-42`, **and the copy in the
  region must still be `info`/`run`-able**.

### Three new conventions

**⑪ "It was written" does not equal "it was written correctly".** `arkrtos_port_flash_write()` is a
read-modify-write of each 32 B flash word (a block can only be written once), so only **read-back
byte comparison** proves the content; length and CRC are only "I computed it", not "this is what is in
Flash now". The `(verified)` on `write` is the result of that comparison, not an inference.

**⑫ Verify the shape before any destructive action.** An obviously wrong image must not erase the
runnable copy in the region. Putting the SP / entry / prologue checks all before `erase` buys the
property "bad input does not lose existing artifacts" — `m5-wrong-window` is its criterion (looking at
the error code alone is not enough; the old copy must still be there).

**⑬ Verifying "instruction fetch from Flash" requires looking at two things at once.** Proving it only
with a source-only string would also pass if the image were copied into RAM and run; the criterion must
additionally require **the entry point in the log to lie in the Flash region**. This is the same idea
as `m2-two-landings` "let the program compute its own landing": make the device report the number that
only appears if it really ran that way.

### A silent-fallback hazard fixed along the way

`ARKRTOS_MPU_IMAGE_POOL_SIZE` used to cover only the RAM pool. When a FlashApp task's `rom_start`
falls outside the window, `arkrtos_mpu_rom_window()` **silently falls back to the kernel Flash window**
and the first fetch faults — the symptom is "the program dies the moment it starts" while every check
passes. The macro now means "pool + FlashApp together" and gained compile-time checks (the region must
lie in bank2, the region end must equal the end of Flash, `MAX_IMAGE` must at least hold header + 256 B).

### Assertion-shape pitfall in the regression case

After `Reset` the case used to assert `^ark@arkrtos:~/`, but the prompt after ANSI cleaning is
`ark@arkrtos:~$ ` (**with a trailing space**), so `$` did not match ⇒ false FAIL. `Reset` now only
keeps `expect=[r"Ark RTOS kernel shell"]` and the prompt is not asserted again elsewhere.

### Off-build measurement and one real defect (measured 2026-11-09)

Building once with `USE_FLASHAPP=0` and flashing:

```
FLASH 492956 B (94.02%) / RAM 308240 B (68.41%)      # on build is 497700 B / 309360 B
app bogus  -> usage: app [... | run <path>]           # no flash
app flash info -> falls to the same usage             # neither fakes success nor silently ignores
```

**A real defect caught on the way**: the `app` usage line was unconditionally written with
`| flash <write ...>` inside the `ARKRTOS_USE_APPRUN == 1` branch. In the off build that command does
not exist yet appears in the help — the user would think their arguments were wrong rather than "this
build does not include this feature". Fix: wrap it in another `#if (ARKRTOS_USE_FLASHAPP == 1)`.
**Help text is an assertion too**: what it declares must really exist.

### ⑭ The partition is a static decision, it does not follow feature switches

`ARKRTOS_FLASHAPP_SIZE` defaults to `IMAGE_POOL_SECTOR` and **is reserved even with the switch off**
(cost: the pool loses one sector, firmware +16 B). Not an oversight: the `.sct` / `.ld` generators and
the C code read the same header, and if this value followed the switch the same header would tell
different tools different things — exactly what this repo's "addresses are defined once, in one
place" rule exists to prevent. If the area is genuinely not needed, change it on the compile command
line, but then point the generators at the same configuration.

Likewise the 16 B difference in the off build is a constant change from pool cells 10 → 9, not code
residue: with `USE_FLASHAPP=0`, `arkrtos_flashapp.c` is not a translation unit at all (it is listed in
`sources.mk`, but the `#if` and the command table sit behind the `arkrtos_features.h` gate).
