# arkc Build Artifacts Running on Board — Implementation Plan

> Condensed English edition. The Chinese page (arkc编译产物上板运行-实施计划.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

- Status: **plan; originally not started** (this page describes what to do and how to judge
  pass/fail). The per-stage results landed later and are recorded inline in §4/§8/§9.
- Date: 2026-11-05
- Target platform: STM32H743 (`example/stm32h743/kernel/ARKRTOS_TEST/GCC`)
- Preceding result: on-board `cc`/`vim` can already compile (28 KB console stack finalised; see
  `kernelsrc/components/arkc/VENDOR.md`)

---

## 0. One-sentence goal

Make **the machine code the on-board compiler just produced actually execute on the board**, with
criteria proving "what ran is the thing just compiled" — not something else, not a leftover, not a
run that merely happened not to crash.

---

## 1. Split "it runs" into three layers, accepted separately

| Layer | Meaning | Key property | Stage in this plan |
|----|------|----------|-----------|
| **L1** | execute directly in a fixed window | no relocation, single instance, landing point hard-coded | M1 |
| **L2** | relocatable, any landing point | reuses the kernel's existing `mini` path and relocation table | M2 |
| **L3** | resident on a filesystem, can autostart at boot | survives power loss, `mini autostart` | M3 |

The three layers must be **independently acceptable**. L1 passing does not imply L2, L2 passing
does not imply L3 — and the reverse: L3's persistence cannot substitute for L1's execution evidence.

---

## 2. What already exists (all verified down to code lines)

This section is the cost baseline of the plan: **container, loader, relocation, exit reclaim and SDK
skeleton all exist already**, and this round does not rebuild them.

| Capability | Location | Verification |
|------|----------|----------|
| image format: 256 B header + relocation table + payload | `kernelsrc/include/arkrtos_app_image.h` | field order / CRC rule / `runtime_ram_base` explicit; `TYPE_MINIAPP=3` defined |
| relocation patching (whole word + MOVW/MOVT pair) | `kernelsrc/src/arkrtos_loader.c:622` `reloc_movw()` / `:661` `reloc_apply()` | only 4 kinds: ROM/RAM whole word, ROM/RAM_MOVW |
| six-step load (header → table → verify → borrow block → copy+patch → start task) | `kernelsrc/src/arkrtos_mini.c:440..540` | entry `code_base + entry_offset`, `|= 1` sets Thumb; `is_priv=0`; two MPU windows |
| buddy pool 64 KB @ `0x24070000` | `config/stm32h743/arkrtos_partition.h:125-132` | blocks 1 KB…32 KB, powers of two; `ARKRTOS_APP_NOMINAL_RAM_BASE = SLOT_RAM_BASE` |
| automatic return of both blocks on exit | `kernelsrc/src/arkrtos_task.c:2618` → `arkrtos_mini.c:777` | the app calling `arkrtos_thread_exit()` frees them, nothing leaks |
| mini SDK (crt0 + main skeleton + docs) | `kernelsrc/sdk/mini_sdk/{arkrtos_mini_start.s, _gcc.s, arkrtos_mini_main.c, README.md}` | MDK version jumps to `__main`; GCC version copies `.data` and clears `.bss` itself |
| host packing tool (four-pass differential + table + header + CRC) | `tools/pack_app.py` (`diff_reloc:414`/`verify_reloc:481`/`assemble:583`) | supports offline input `--bin-a..d`, no toolchain dependency |
| on-board `cc` emitting flat bin / ARKOBJ2 / assembly | `kernelsrc/components/arkc/arkc_shell_cmd.c:74` | already has `-S/-c/-E/-o/-O0..2/-D/-I` with `done in N ms` timing |
| linker multi-target and scatter/linker-script | upstream `include/compiler.h:19-35` (`link_mode` + `link_inputs[32]` + `scatter_file` + `linker_script`) | **upstream supported it all along**; only the on-board shell did not expose it |
| arkc absolute addresses always go through MOVW/MOVT | upstream `src/backend/arm/backend_arm.c:34` `emit_mov_imm()`, `:4334` (`LDR =label` also lowers to MOVW/MOVT, no literal pool) | ⇒ hits exactly the existing `reloc_movw`, **no new reloc kind needed** |
| `__CC_ARM` predefined | upstream `src/preproc.c` (predefined macro table) | ⇒ `arkrtos_oslib.c` takes the `__svc` branch and compiles with arkc **unchanged** |
| global init/clear emitted as code | upstream `backend_arm.c:2331` generates `__arkcc_init_globals()`; `:820` only auto-`BL`s it inside `Reset_Handler` | the app side must call it itself (see G4) |

---

## 3. What is missing (each with evidence, not guesswork)

| No. | Gap | Evidence | Impact |
|------|------|----------|--------|
| **G1** | on-board `cc` cannot change the link base | `cc_usage()` and argv parsing in `arkc_shell_cmd.c` have no `--scatter`/`-T` (the host `src/main.c:433/437` does) | artifacts can only link at `0x08000000`, while mini executes in RAM. **M0 addendum**: on the host `--scatter` was also a no-op at that time (§3.2), so this is not merely "add option parsing" |
| **G2** | on-board `cc` refuses multiple input files | `arkc_shell_cmd.c`: a second non-option argument gives `more than one input file` | cannot link crt0 + main + oslib |
| **G3** | on-board `cc` does not pack | only `-o x.bin`; no header, no relocation table, no CRC | the artifact is not acceptable to `mini run` |
| **G4** | no crt0 on the app side | arkc only auto-`BL`s `__arkcc_init_globals` inside `Reset_Handler`, and **does not generate that function at all without .data/.bss** | calling it directly fails to link; not calling it leaves globals as garbage |
| **G5** | `mini` only reads from an `arkrtos_fs` volume | `arkrtos_mini.c:192` `arkrtos_mini_read_exact()` → `arkrtos_fs_read_at()` (the single read point) | H743 has no volume (ramfs sits at the `ark_vfs` layer, a different interface) |
| **G6** | on-board link table caps at 64/64 | `GCC/Makefile`: `-DARK_LINK_MAX_SYMS=64 -DARK_LINK_MAX_RELOCS=64`; `LdSym` = 136 B (`name[128]`) | raising to 256 costs +34.8 KB RAM while kernel RAM is already 97.50% (about 11 KB free) ⇒ **cannot be solved by enlarging the table** |
| **G7** | key numbers unmeasured | — | **M0 measured**: on-board compile 46–48 ms, host/board artifacts byte-identical, single-file direct compile does not go through the link symbol table (§3.2) |
| **G8** | on-board ramfs single-file cap 16 KB | `GCC/Makefile`: `-DARK_RAMFS_FILE_CAP=16384`; measured: a `-S` listing of a 2.2 KB source is 18 801 B, and `cc -S -o /tmp/x.s` immediately says `cannot write output file` | `-S` listings can only go to the console; intermediate artifacts cannot be kept side by side |
| **G9** | on-board ramfs has only 3 slots | `-DARK_RAMFS_MAX_FILES=3`; measured: the `/tmp` directory itself takes one (a directory + 2 files fills it) | M2's "source + temp + artifact" exactly fills it; a fourth always fails |

### 3.2 M0 measured conclusions (2026-11-05, all on-site readings; no verdict was changed without evidence)

M0's first job was to measure, and the measurements overturned three assumptions in the plan itself.
All three are recorded here because they decided how M1/M2 were written:

| # | Original assumption | Measurement | Impact |
|---|--------|-----------|--------|
| 1 | "not one line of upstream arkc changes" | **overturned**. ① `arkrtos_oslib.c` has 9 places `ARKRTOS_SVC_DECL_*(…);` (the macro already carries `;` plus the call-site `;`) ⇒ a top-level empty declaration, which arkc rejects with `expected declaration at top level` while armcc/GCC accept it; ② `--scatter` is **completely ineffective** on codegen (two `.sct` files differing only in `LR_IROM1` compile the same source to byte-identical artifacts) | two upstream fixes, synced by `sync_arkc.py` (below) |
| 2 | "the crt0 prologue is `PUSH {r7,lr}`" | **overturned**: the first instruction measured is `0xB5E0` = `PUSH {r5,r6,r7,lr}` | the pack check is written to 0xB5E0, not to memory |
| 3 | "multi-file linking works as long as symbols < 64" | **partly overturned, partly corrected**. Four objects (including `arkrtos_oslib.c`) link successfully but the payload is **33 384 B**; without oslib, two objects are only **3 148 B** (a realistic case with strings/globals/svc: **3 880 B**). ⇒ the real culprit of the size explosion is **oslib**: it itself fails with `FATAL: memory pool exhausted` when compiled on the board, and linking it drags in another copy of the runtime | the program shape defaults to **self-contained single file** (2 272 B, cheapest and most robust); multi-object stays an option (crt0 + app, < 4 KB) but **oslib is forbidden**; the SDK supplies only a "thin header" (pure `__svc` declarations, no implementation) |

The two upstream fixes (made in `D:\工作\git_project\ark c compiler`, synced into
`kernelsrc/components/arkc`):

- `src/parser.c`: the top-level loop skips a lone `TOK_SEMICOLON` (C11 6.8.2 allows empty
  declarations; armcc/GCC accept them, so arkc rejecting them was a compatibility defect).
- `src/backend/arm/backend_arm.c`: the `.sct` branch set only `g_scatter_loaded = 1` but **never
  derived** `g_flash_base/g_data_base/g_bss_base` (only the `-T` branch did), so `--scatter` parsed
  successfully and every address stayed at its default — a silent no-op. Now extracted into
  `arm_scatter_derive_bases()`, shared by both branches.

After the fix (measured with `m0_single.c`, containing a function pointer + an initialised global +
a string + `__svc`):

| Base combination | Artifact size | Difference from baseline | What is read |
|---|---|---|---|
| default (no `--scatter`) | 2134 B | — | code `0x08000000`, data `0x20000000` |
| `LR_IROM1 0x08004000` + `RW_IRAM1 0x24070000` | 2134 B | 57 bytes | function pointer `=bump` → `MOVW #0x4000 / MOVT #0x0800`; global → `MOVT #0x2407` |
| same but `LR_IROM1` becomes `0x08014000` | 2134 B | **1 byte** (high half 0x0800→0x0801) | exactly the "whole word +delta" shape it should have |

**A silent trap in `-link` (measured)**: `-link` collects only the non-option arguments
**immediately following it**. Writing `arkcc a.obj b.obj -link -o x.bin` treats both objects as
ordinary inputs and produces **0 bytes** with no error; the correct form is
`arkcc -link a.obj b.obj -o x.bin`. The packing tool must build the command line the latter way
**and verify the output is non-empty** — 0 bytes must not count as "link succeeded".

**One rule that must be nailed down**: the differential must not use **0** as a base. With base 0,
arkc folds `MOVW+MOVT` into a single `MOV` (high half zero), the artifact shrinks from 2272 B to
1918 B, and the difference is no longer an address but the whole code layout — the differential
would produce a wrong table. The base must be **non-zero and 4-byte aligned**, with delta at the
0x10000 level.

On-board readings (`m0_board.c`, 653 B source):

| Item | Reading |
|---|---|
| host vs board bin | **byte-identical**: all 2260 B equal (after decoding the board's `\xNN` escapes from `cat`; longest common prefix = full length) |
| `cc -o bin` time | 46 / 47 / 46 ms (three runs at -O1), -O0 48 ms, -O2 46 ms |
| `cc -c` time | 47 ms (-O1) / 46 ms (-O2) |
| pool usage | 66 248 / 81 920 B |
| `cc -S -o -` | 1549 ms — **not compile time**, but the time for an 18.8 KB listing over a 115200 UART |
| `cc -S -o <file>` | unusable: the listing is 18 801 B > `ARK_RAMFS_FILE_CAP` 16 384 B (G8) |

### 3.1 Hard constraint derived from G6

A mini program's symbol count must stay under 64. **Corollary**: the program must be **one file or
few files**, and must not carry a full C library. This constraint directly decides the M1/M2 program
shape, so **M0's first job is to measure it** — decide the shape after measuring, rather than write
code and discover it will not link.

---

## 4. Stage plan

Every stage states: change points, switches, criteria, false-green checks, ledger. **No stage
proceeds to the next without passing.**

### M0 — measurement prerequisite (**done; readings in §3.2**)

Change no board code, only measure the inputs of every later decision. The table keeps the original
criteria in the left column and the measured result in the notes.

| Item | Method | Criterion |
|----|------|---------|
| M0-1 symbol/relocation count | host arkc compiles a "single-file mini program" (1 serial write + 1 initialised global + 1 string literal + 1 `__svc` call), see whether linking reports `ld_table_overflow` | **conclusion reversed**: single-file direct compile does not touch the link symbol table at all — setting `ARK_LINK_MAX_SYMS` to 1 still succeeds with an unchanged artifact. ⇒ 64/64 only constrains "multi-object linking", and multi-object linking explodes in size (33 384 B), so the shape is fixed as a self-contained single file, to which the symbol cap does not apply |
| M0-2 on-board compile time | on-board `cc` on the same file, read `done in N ms` | **T = 46–48 ms** (compile + assemble + write). M2's four-pass upper bound ≈ 4T ≈ 190 ms, entirely acceptable on the board |
| M0-3 host/board compile equivalence | compile the same source to bin on host and board, compare byte by byte | **must be identical**; any difference means "the on-board compiler cannot be trusted" and the whole plan stops (fix the compiler first) |
| M0-4 crt0 shape finalised | write `arkrtos_mini_start_arkc.c` (C-language crt0, see §4.1) | produced; first instruction measured `0xB5E0` = `PUSH {r5,r6,r7,lr}` (**not** `PUSH {r7,lr}`); the pack check follows that |
| M0-5 base and delta values | pick the nominal link base (code block base) and the differential delta | **done**: `--scatter` really changes codegen now; changing the base by +0x10000 only moves the MOVT high half (whole-word +delta shape). Rule: the base **must not be 0** (it folds into a single `MOV`, artifact 2272→1918 B, differential distorted) and must be non-zero and 4-byte aligned |

**M0's false-green check**: M0-3 must really compare byte by byte — not just sizes, not just CRC —
CRC equality with different bytes is theoretically possible, and the strong conclusion wanted here
is "compiler equivalence".

### 4.1 crt0 design (without changing upstream codegen)

Written in C (arkc does not accept assembly input), three things:

```
void MINISTART(void)
{
    __arkcc_init_globals();      /* extern declaration; generated by arkc when .data/.bss exist */
    main();                      /* the project's own standard C entry */
    arkrtos_thread_exit();
}
```

**The G4 solution (key)**: without .data/.bss arkc does not generate `__arkcc_init_globals`, and the
link then reports an undefined symbol explicitly — which fits "rather an error than a wrong answer".
Putting a **non-zero-initialised anchor global** in the SDK skeleton
(`volatile unsigned g_mini_sdk_anchor = 0xA5A5A5A5u;`) guarantees `.data` is non-empty and the
function is generated. **So crt0 needs no upstream cooperation.** Note this does not contradict the
two upstream fixes in §3.2 — those are "arkc versus armcc compatibility defects" (empty declaration,
`.sct` no-op), unrelated to the generation condition of `__arkcc_init_globals`.

Getting the entry offset: the convention is that crt0 is the first section of the **first link
input** ⇒ `entry_offset = 0`; at pack time the first two payload instructions are **verified** to
equal crt0's fixed prologue, and the image is refused otherwise (no "guessing an entry").

### M1 — L1 fixed-window execution (**done; board readings in [arkc板上直跑-M1实机记录.md](arkc板上直跑-M1实机记录.md)**)

Minimal loop: a bin compiled on the board is read into a fixed RAM window and executed directly.

> Three differences from this section's early wording (the board decides; reasons in the record
> document): ① the entry contract became **image-supplied vector table + `--vector-table` reserved
> emission** (not "crt0 at offset 0"); ② reading files goes through **VFS** (`/tmp` is ramfs), not
> littlefs; ③ the direct-run program's priority sits **below the console** (above it starves the
> shell).

| Item | Content |
|----|------|
| change 1 | `arkc_shell_cmd.c`: `cc` gains `--scatter <file>` (into `opts.scatter_file`) and `-link <obj>...` (into `opts.link_mode/link_inputs`). **Only ArkRTOS wiring files, not upstream** |
| change 2 | new component `kernelsrc/src/arkrtos_apprun.c` + switch `ARKRTOS_USE_APPRUN` (default **0**): `app run <path>` reads the file into the fixed window from `SLOT_RAM_BASE` of `APPRUN_WINDOW_SIZE`, creates an unprivileged task + two MPU windows, reclaims on exit |
| change 3 | mini SDK gains `arkrtos_mini_start_arkc.c` (§4.1) and a short README |
| switch discipline | with `USE_APPRUN=0` the source is **not compiled**, FLASH/RAM byte-identical to now (controlled in the makefile the same way as `USE_ARKC`) |
| criterion A | `cc /tmp/hello.c --scatter /tmp/app.sct -o /tmp/hello.bin` → `app run /tmp/hello.bin` → the serial port prints a **string that exists only in the source** (not a leftover log) |
| criterion B | the program calls `arkrtos_thread_exit()` itself; afterwards `fault` is empty, `task` has no leftovers, and **running it again still works** (proves the window is reclaimed cleanly) |
| criterion C | deliberately change the base in `--scatter` → a clear error (measured `rc=-23 image was not linked for this window`), and it must **not** happen to work (proves the criteria are address-sensitive) |
| false-green | ① change one string, recompile, rerun — the serial content must change; ② run a `while(1)` program to confirm it really runs (does not return) rather than returning immediately; ③ in a build with `use apprun` off, the `app` command does not exist (the switch really works) |
| ledger | FLASH/RAM before and after (must change only with `USE_APPRUN=1`); the relation between the `SLOT_RAM` window size and `APPRUN_WINDOW_SIZE` goes into comments |

**M1 explicitly does not do**: no relocation, no shared buddy pool (only one fixed-window instance at
a time; a second must be refused by the command). Reason: L1's value is **getting the earliest
evidence that "machine code produced by the on-board compiler executes on the board"**; mixing it
with relocation makes it impossible to tell an entry error from a base error from a patch error.

### M2 — L2 relocatable, through the existing mini (**done; board readings in [arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)**)

Move `pack_app.py`'s four-pass differential onto the board (C implementation), producing a standard
`.svcm` loaded by the **unmodified** `mini run`.

| Item | Content |
|----|------|
| change 1 | new component `kernelsrc/src/arkrtos_apppack.c`: four links (A nominal / B ROM+delta / C RAM+delta / D double delta verification) → differential table → assemble 256 B header + table + payload + CRC → write the file |
| change 2 | `cc` gains `--pack -o x.svcm` (calls apppack internally); `--pack` and `-S/-c/-E` are mutually exclusive, conflicts error out |
| memory discipline | the payload cap is bounded by both `ARK_CODE_BUF_SIZE=16384` and the ramfs 16 KB single-file cap; A lands in `/tmp`, B/C/D reuse the same temp file, **no second 16 KB static buffer** (only ~11 KB RAM left — a hard constraint, not a preference) |
| kernel side | **zero change**: goes through `arkrtos_loader_reloc_apply()` |
| criterion A | `cc /tmp/hello.c --pack -o /tmp/hello.svcm` → `mini run /tmp/hello.svcm` → prints, exits, `mini stat` shows the blocks returned |
| criterion B | the same image runs once at each of two different landing points (occupy a block first, then run it) → both fine, identical output |
| criterion C | the program contains: 1 initialised global, 1 string literal, 1 function pointer, 1 cross-file call — each of the four absolute-address classes hit at least once |
| false-green | ① after packing, **deliberately delete one relocation entry** and reload → it must crash or misbehave (if it still works, the criterion did not test that address); ② print the entry count and class distribution; if one of ROM/RAM/whole-word/MOVW is always 0, explain why (otherwise that class was never verified); ③ `verify_reloc`'s D pass must really run and **refuse to emit an image on mismatch** |
| ledger | table entries × 4 B against image size; total time of 4 passes (a multiple of M0-2's T); ramfs slot usage (source + temp + artifact = 3 slots, exactly at `ARK_RAMFS_MAX_FILES=3`, so state that "source and artifact cannot both keep a fourth copy") |

> **Differences from this section's wording (the board decides; details in the record doc §2–§4):**
> ① the four passes **share one** `/tmp/pp.sct` (rewritten each pass) and all four payloads borrow
> the RAM pool arena, **no intermediate files** — cheaper in slots than "A to /tmp, B/C/D reuse one
> temp file";
> ② the differential scan order is fixed as "**MOVW/MOVT immediate pairs first → whole word (must be
> 4-byte aligned) → half-word consistent, skip Thumb length → fall back to interpreting as a MOVW
> pair, error if that fails**", with the Python side aligned;
> ③ criterion B (two landing points) **was added**: the same `.svcm` run twice, landing at
> `0x24070000` / `0x24071000` with RW blocks at `0x24072000` / `0x24074000`, and the program measures
> block number `0` / `1` itself; for this `mini` gained `MINI_PRIORITY` (default empty = inherit
> `APP_TASK_PRIORITY`, tier-0/on sizes byte-identical), otherwise a busy-loop program starves the
> console. Criterion C's "cross-file call" **was added** (2026-11-09): the ramfs 3-slot limit is
> bypassed by M3-b's persistent NOR volume — two sources + one `.sct` + two objects + the artifact in
> `/mnt/nor`, `cc --scatter -c` twice, `cc -link --scatter` once, then `app run`; the four classes
> (initialised global / string literal / function pointer / cross-file call) are each hit once. Case
> name `m2-cross-tu-link`.

### M3 — L3 somewhere for the image to live (estimated 1–2 days)

| Sub-step | Content | Criterion |
|------|------|---------|
| M3-a **(done)** | `mini`'s read backend becomes switchable: a VFS branch in `arkrtos_mini_read_exact` (switch `ARKRTOS_MINI_READ_VFS`, default **0** keeps `arkrtos_fs`); the VFS side gains a facade `arkrtos_vfs_read_at(path, buf, len, off, &got)` = `open_read` + `ark_vfs_lseek` + `read` + `close` | with the default off, mini behaves byte-identically; with it on, a `.svcm` can be read from ramfs and `mini run` succeeds. **Note ramfs is lost on power loss: this step only answers "can it be read", not persistence** |
| M3-b **(now closed; at the time one of two criteria met)** | QSPI W25Q64 + littlefs: `&qspi` in the dts was `status="disabled"` (`NCS=PG6`/`CLK=PF10`/`IO0..3=PF8/PF9/PF7/PF6`); mounted as `/mnt/nor` | after a power cycle the `.svcm` is still there (**met**); `mini autostart` works (**not met at the time**: the manifest was at the ramfs root and lost on reset, see the M3-b record §10) |
| M3-c | the format path of `arkrtos_vfs_mount_volume()` must be explicit (no auto-format) | first mount of an unformatted volume must **fail explicitly**, never silently format |

M3-b carries the highest risk (NOR driver + filesystem + unverified pins), so it comes last, and the
M1/M2 conclusions do not depend on it.

### M4 — closure (**done, see [arkc产物上板-错误码表与回归账本.md](arkc产物上板-错误码表与回归账本.md)**)

- add an "arkc artifacts running on board" section to `docs/`, and cross-reference this page from `VENDOR.md`;
- error-code table: link table overflow, four-pass mismatch, entry signature mismatch, window busy,
  image over cap — each needs a **distinct** code, not all collapsing to `-1`;
- regression script: turn M1-A/B/C and M2-A/B/C into `final_app.py` (same style as the existing
  `final_acc.py`);
- size ledger: measured FLASH/RAM table for the four combinations of `USE_APPRUN=0/1` × `USE_ARKC=0/1`;
- memory: board-level pitfalls (G1–G6) + the three cases into the wiki.

**Landed results** (details on the record pages):
① the error-code table lists the three load paths and pack time separately and notes two
**cross-path numbering overlaps** (-18, -20) ⇒ messages must name the path;
② regression script `tools/final_app.py`: leading cleanup + four cases (M1 positive/negative, M2
pack-and-run, M2 two landing points), measured this round as `all 4 cases PASS`; the difference from
the plan is that "M1-A/B/C and M2-A/B/C" were merged into four cases; M2's class C (cross-file call)
was added later on 2026-11-09 (see §4 criterion C), and the 14 cases of that round include it;
③ a six-row size ledger (`USE_ARKC` × `USE_APPRUN` × `USE_APPPACK/MINI_VFS`): arkc alone is +191 KB
FLASH / **+292 KB RAM**, while the M1/M2 switches are pocket change; RAM at 97.5% is already near this
board's ceiling.

---

## 5. Things explicitly not done (and why)

| Not done | Reason |
|------|------|
| no new reloc kind | 100% of arkc's absolute addresses are MOVW/MOVT pairs (including `LDR =label`), exactly covered by the existing `reloc_movw`. A new kind would only let two patching logics coexist |
| no upstream arkc changes "for convenience" | the original "not one line" was overturned by M0 (§3.2): the empty declaration and the `--scatter` no-op are **compatibility defects** that must be fixed; fixed and synced. Beyond that upstream is untouched: multi-target linking and linker-script already exist upstream, only the on-board shell's option parsing was missing — and that is our own wiring file. Run `sync_arkc.py --check` first; it must report "consistent" |
| do not make `__arkcc_init_globals` "always generated" | one initialised anchor global in the SDK makes the function necessarily generated; cheaper than changing upstream and better aligned with "do not guess" |
| do not pack `.data/.bss` into the code block (single RWX block) | a different shape from the board-verified MDK path, and it would leave the "RAM relocation" class permanently unverified; kept as a later block-count optimisation |
| no RAM block device for the filesystem | kernel RAM is 97.50%, SDRAM is not connected, and anything built would be volatile fake persistence. **2026-11-05 addendum**: the SDRAM driver and partition mechanism have landed (`ARKRTOS_USE_SDRAM`, default off). Per the user's judgement this board's FMC external-memory window **works** (vendor examples run); my integration not coming up is a configuration issue, **not a hardware-level conclusion**; frozen as instructed ("leave it for now"), no RAM block device for now |
| do not shorten the link pipeline "to make four passes faster" | the four passes must take **the same** full pipeline (same `-O`, same `.sct` shape), otherwise B/C differences mix in optimisation differences. Measured four passes at 267 ms, acceptable; to go faster, speed up everything, do not cut corners inside the differential |
| do not touch `SHELL_TASK_STACK_SIZE` | 28 KB is finalised and measured at 22 000/28 672 B, unrelated to this page; do not change it in passing |

---

## 6. Main risks and stop conditions

| Risk | Trigger | Handling |
|------|----------|----------|
| R1 mini program symbol count above 64 | M0-1 reports `ld_table_overflow` | first shrink the program shape (single file, few libraries); if still over, **stop**, because enlarging the table needs 34.8 KB RAM, conflicting with 97.5%, and needs a separate design (e.g. packing emits only a **lean symbol reference** of the object sections) — outside this plan |
| R2 on-board and host compilers not equivalent | M0-3 bytes differ | **stop everything**, fix the compiler first. Every "it ran" before that is not evidence |
| R3 arkc artifacts contain absolute addresses that are not MOVW/MOVT | M0 scans the `-S` line by line and finds a literal pool or `ADR`-type absolute reference | stop and re-assess: may need a new kind or codegen changes, a completely different cost bracket |
| R4 fixed window conflicts with the buddy pool | M1 criterion B (run twice) fails | the window was not really reclaimed; "a reboot fixes it" is not allowed to paper over it |
| R5 time budget | M2 has not passed criterion C after 3 days | shrink scope: M2 only guarantees relocatability of the M1 program, with the limitation stated, no pretending it is general |

**Risk status (at M2 closure, 2026-11-05)**: R1 not triggered (single-file program, symbols far below
64); R2 covered by the host comparison and by equal values on both ends (four payload lengths
identical for the same source and `.sct`, and the four relocation-class counts identical); the 108 B
board/host artifact difference was explained on 2026-11-09 (different optimisation tiers: board
defaults to -O1, host to -O0; pinned to the same tier they are byte-identical, see
[arkc产物上板-错误码表与回归账本.md](arkc产物上板-错误码表与回归账本.md) §6.1), and no equivalence
conclusion is drawn from it; R3 not triggered (the whole scan is dominated by MOVW/MOVT pairs, no
literal-pool absolute reference was misjudged); R4 covered by M1 criterion B (run twice, window
reclaimed); R5 not triggered (the bulk of M2 closed within half a day), and the gaps in criteria B/C
were handled as "shrink scope + state the limitation".

---

## 7. Order and dependencies

```
M0 ──→ M1 ──→ M2 ──→ M4
        └────→ M3-a（can run in parallel with M2, since it only touches the read backend）
                     M3-b（independent, last）
```

- M0 is the only master gate: if R2 triggers, everything is void, so it must come first and its
  conclusion must be nailed down.
- M1's and M2's criteria cannot substitute for each other.
- M3-b's completeness plays no part in the L1/L2 conclusions.

---

## 8. Decision status

**Decided (user approved 2026-11-05)**

1. Start in the order of this document: M0 → stop and report after conclusions → M1 (not all at once);
2. the previous round's arkc on-board compilation results are **not committed separately**, but
   merged with this plan's results (push still needs separate consent).
3. **M2 was implemented as "four-pass differential + existing `mini` loading" and passed on the real
   board** (2026-11-05); M3-a (`ARKRTOS_MINI_READ_VFS`) landed in the same batch, defaulting to **0**
   to keep the old backend. M2's criterion B was later measured and passed (same image, two landing
   points); criterion C's "cross-file call" was then blocked by the ramfs 3-slot limit and was closed
   on 2026-11-09 with the NOR persistent volume, with 14/14 board regressions PASS in the same round.
4. The 108 B board/host artifact difference **was explained (2026-11-09)**: not a compiler divergence,
   but comparing a host default `-O0` artifact with a board default `-O1` artifact; pinned to the same
   tier, `-O0/-O1/-O2` are byte-identical on both sides (see
   [arkc产物上板-错误码表与回归账本.md](arkc产物上板-错误码表与回归账本.md) §6.1).
5. **M3-b closed (2026-11-05, real board)**: JEDEC `0xEF4017` (W25Q64 / 8 MiB), `nor verify` four
   steps PASS, `fs test` PASS, `cc --pack` artifact lands in NOR, **after a reset the `.svcm` is still
   there and `mini run` prints the two source-only lines**; the price is slowness (pack 24.8 s, load
   33.7 s), reasons and speed-up directions in
   [arkc板上直跑-M3b-QSPI持久化.md](arkc板上直跑-M3b-QSPI持久化.md) §8. The same round fixed one kernel
   defect: creating a file on a littlefs volume was judged an I/O error by the VFS bridge (never
   visible on ramfs). **Cross-reset `mini autostart` was not achieved at the time**, recorded honestly,
   not written up as all-green.
6. **M4 done**: error-code table, size ledger, `tools/final_app.py` board regression (four cases,
   measured 4/4 PASS) and the wiki memory have all landed; M2 criterion C's "cross-file call" **was
   added on 2026-11-09**: the six files `-link` needs sit in `/mnt/nor` (the ramfs 3-slot limit no
   longer applies), and the case `m2-cross-tu-link` entered `tools/final_app.py`.

**Still pending a user decision after M0**

1. ~~**Whether M1's `app run` accepts the "single-instance fixed window" intermediate state**~~ — **implemented
   as single-instance and passed on the real board** (user instruction 2026-11-05: "compile and run
   with this compiler on STM32, keep going", see the boundary table in the record doc §8): it is a
   narrow entry that first nails down the "compile→execute" segment, later superseded by `mini`.
7. **Stage 3c (M5) closed (2026-11-09, real board)**: `app flash write/run/erase/info` are online, the
   image lands at `0x081E0000 + 128K` (tail of bank2) and executes by fetching from flash, still there
   after reset; the write path validates before erasing and reads back byte by byte; criteria and
   false-green checks in §9.

---

## 9. M5 / stage 3c: artifacts flashed into on-chip Flash and executed (closed 2026-11-09)

M0–M4's artifacts all lived in RAM (`app run`'s fixed window, `mini`'s pool blocks) and vanished on
power loss. Stage 3c adds the last segment: **the image lands in on-chip Flash, is still there after
reset, and can execute directly**.

### 9.1 Shape

Not a bootloader shape (bare VTOR jump + `NVIC_SystemReset` takeover); instead it reuses the verified
apprun path: `app flash write` writes the image into the flash app area (`0x081E0000 + 128K`, tail of
bank2), and `app flash run` borrows a RAM window via `mini_alloc` + `task_register` to start an
unprivileged task with **PC landing directly in the flash area**. Power retention holds naturally, and
no second scheduling/exception/exit path is introduced (on completion the same `SVC 0x1B` runs and the
data window goes back to the pool).

### 9.2 Change points

| File | Change |
| --- | --- |
| `config/stm32h743/arkrtos_partition.h` | new `ARKRTOS_FLASHAPP_*` (area base/size/metadata/cap/magic/licence code/RAM alias); `IMAGE_POOL_SIZE` reduced by that area; `ARKRTOS_MPU_IMAGE_POOL_SIZE` becomes the sum of "pool + FlashApp"; new compile-time check `arkrtos_flashapp_layout_check` |
| `kernelsrc/include/arkrtos_features.h` | new `ARKRTOS_USE_FLASHAPP` (default 0) + two `#error` dependencies (needs `USE_APPRUN` and `USE_VFS`) |
| `kernelsrc/include/arkrtos_flashapp.h` | **new**: error codes `-40..-46` and five entry points |
| `kernelsrc/src/arkrtos_flashapp.c` | **new**: `write` (scan + shape check → erase → chunked write → read-back compare → metadata) / `erase` / `info` / `run` / `on_task_exit` / `task_id` / `error_name` |
| `kernelsrc/src/arkrtos_task.c` | the task exit hook calls `arkrtos_flashapp_on_task_exit()` (**GBK/CRLF: byte-level patch, ASCII only**) |
| `kernelsrc/shell/arkrtos_shell.c` | `app flash <write <path>\|run\|erase\|info>` four subcommands + usage |
| `config/kconfig/Kconfig.features` | new `config ARKRTOS_USE_FLASHAPP` (`depends on` expressing the same rule as the C-side `#error`) |
| `kernelsrc/include/arkrtos_autoconf.h` | regenerated by `tools/kconfig/genconfig.py` (one new `#define ... 0`) |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/{sources.mk,Makefile}` | add the source, `USE_FLASHAPP ?= 0`, `-DARKRTOS_USE_FLASHAPP=$(USE_FLASHAPP)`, `SWITCH_SIG` |
| `tools/final_app.py` | new scatter templates `SCT_M5` / `SCT_M5_BAD` and two cases |

### 9.3 Criteria

* **Positive** (`m5-flashapp`): write reports `length/CRC/target address + (verified)`; `info` reports
  the entry; `run`'s log shows the entry point **inside the `0x081E0*** area** and the program prints
  a source-only string; after `Reset`, `/tmp` is empty (`ls` shows no `m5.bin`) yet `info`/`run` still
  work; after `erase`, `run` reports `-44`; `app flash bogus` prints usage.
* **False-green** (`m5-wrong-window`): first write a good image successfully, then write a bad image
  whose RAM window is 4 KB off; it must report `-42`, **and the copy in the area must still `info`/`run`
  — proving "validate before erase" rather than a lucky failure.

### 9.4 Three deliberate false-green designs

1. **Two criteria must coexist**: judging only on the source-only string would also pass if the image
   were copied into RAM and run; the log's entry point inside the flash area must be checked too.
2. **Validate before erase**: a clearly wrong file must not erase a copy that still runs, so the shape
   checks (SP at the top of the RAM window, entry inside the image, the two bytes before the entry are
   `0xB5E0`) all happen before `erase`.
3. **Default off, zero change**: with `USE_FLASHAPP=0` the source is not compiled in and the command
   table has no `flash` subcommand. The exception is that **the 128 KB on the partition is still
   reserved** (pool loses one sector, firmware +16 B): partitioning is a static board-level decision
   and the `.sct`/`.ld` generators and the C code must see the same number. When on, the `#error` in
   `arkrtos_features.h` and the Kconfig `depends on` are two expressions of the same rule.

### 9.5 Measured readings

```
cc: 58 ms                                  -> 1452 B
app flash write  -> FLASHAPP:410 1452 B, crc 0x612B4A1E -> 0x081E0000..0x081E05AC (verified)
app flash info   -> 1452 B, crc 0x612B4A1E, entry 0x081E0424, area 0x081E0000 +128K
app flash run    -> entry 0x081E0424, data 0x24072000 (8192 B), task 4
                    M5 alive in flash / M5 tag bravo / task 4 exited
after reset      -> no m5.bin in /tmp, info / run still succeed
```

Firmware size (everything on, `USE_FLASHAPP=1`): `FLASH 497700 B / 512 KB / 94.93%`,
`RAM 309360 B / 68.66%`. Board regression `all 16 cases PASS`; structure gate `12 passed, 0 failed`.

### 9.6 Boundary

It is not a "take over the machine" bootloader: the kernel is still there, the shell is still there,
and the program in flash is a task. One at a time, sharing the same RAM window segment as `app run`.
