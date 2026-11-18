# Splitting the Shell Command File (implemented 2026-11-17)

> Condensed English edition. The Chinese page (Shell命令拆分方案.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

## 0. One line

`kernelsrc/shell/arkrtos_shell.c` was **6244 lines / 37 sections / 111 static functions** at the time of the read-only survey, with the `fs` family alone taking 2190 lines (35.1%). Only **10 functions + 8 static variables** were actually shared across families, so the split is "mechanical move + one internal header" and **changes no command logic**. This page records the method, the constraints and the acceptance criteria; the survey itself touched no code (no DAP probe on this machine, so a broken build could not be checked on board).

## 1. Survey readings (6244-line snapshot)

| Metric | Reading |
|---|---|
| File | `kernelsrc/shell/arkrtos_shell.c`, **6244 lines** (UTF-8 + LF, no NUL) |
| Sections | 37 `/* ==== */` sections |
| File-level static functions | 111 |
| File-level static variables | 8 (`g_shell`, `g_shell_stack`, `g_ushell`, `g_ushell_out`, `g_blk_scratch`, `g_fs_scratch`, `rt_unit_c`, `rt_unit_us`) |
| Largest command family | `fs`: lines 3292–5481, **2190 lines** |
| Next largest | `app` 438 / `blk` 399 / `crash+audit+sched+dt` 267 / `cfg` 246 / `stack` 192 |
| Command registration | **one place**: `register_kernel_commands()` (from about line 5909), bound `need = 49 + 4 optional features` |

## 2. Why split at all

* In a 6244-line file, reading `fs` pulls the whole file into context; the default landing spot for a new command is always "pile it onto this file".
* But **do not split by function length**: most of the 111 functions are a few dozen lines. The problem is not function granularity but **one file holding dozens of unrelated command families**. The goal is one family per file, not one function per file.
* And **do not change logic along the way**: the previous SVC split (1318 → 58 lines + 17 handlers) was pure moving and still cost `Code` +408 B. So "size unchanged" is not a usable criterion; "behavior equivalent + size change honestly recorded" is.

## 3. Target file layout (planned)

| Target file | What moves (source line ranges, snapshot values) | Approx lines | Gate |
|---|---|---|---|
| `arkrtos_shell.c` (retained) | file header/includes, small utilities 97–260, registration comment 1692–1695, `register_kernel_commands()`, console task 6107–6245 | ~420 | outer `SHELL_ENABLE` |
| `arkrtos_shell_internal.h` (new) | declarations of shared symbols (§4), no implementations | ~60 | — |
| `arkrtos_shell_image.c` | `info` 261–361, `app` 362–799, `drv` 800–908, `install` 1593–1691 | ~750 | no family gate |
| `arkrtos_shell_diag.c` | `task` 909–1000, `stack` 1001–1192, `sched/audit/guard/crash/dt` 1193–1558, `fault` 1559–1592, `log/syncinfo` 2008–2151, `rt` 2315–2480, `sec` 2481–2560, `utest` 2561–2646 | ~1160 | each family's `ARKRTOS_USE_*` (`STACK_USAGE` / `LOG` / `RTSTAT` / `SECCHECK` / `UTEST`) |
| `arkrtos_shell_fs.c` | `blk` 2893–3291, `fs` 3292–5481, `pwd/cd` 5482–5537, `df` 5662–5712 | ~2700 | `ARKRTOS_USE_BLK` / `USE_FS` / `USE_VFS` |
| `arkrtos_shell_cfg.c` | `pool` 2152–2314, `cfg` 2647–2892 | ~410 | `ARKRTOS_USE_MEMPOOL` (pool section gated) |
| `arkrtos_shell_ushell.c` | user command registry + kernel trampoline 1696–2007 | ~312 | outer gate such as `ARKRTOS_SHELL_USE_USER_CMD` |
| `arkrtos_shell_misc.c` | `history`, `uname`, `free`, `ps`, `which`, `man`, `dmesg`, `hwsafe` 5538–5661, 5713–6106 | ~420 | `HW_SAFE` etc. |

The gap against 6244 lines comes from section comments and `#if/#endif` boundaries, not from something left behind. If `fs` is still too big, a second stage may split it internally into read path / write path / evidence self-check, but **file-level first**.

## 4. Cross-family shared symbols (measured list)

These are referenced from **two or more sections**, so they must go into `arkrtos_shell_internal.h` (declarations) with the definition kept in exactly one `.c` (never copy a `static` per family — that creates N copies that may drift):

| Symbol | Defined at | Used by |
|---|---|---|
| `sh_out` | 119 | nearly everything (app/blk/cd/cfg/crash/df/dmesg/drv/fault/free/fs/guard/install/log/man/utest/which/console/ushell) |
| `parse_u32` | 125 | app/blk/drv/fs/install/log/rt/task |
| `slot_state_name` | 148 | app/drv/info/ps/utest |
| `slot_type_name` | 162 | app/crash/drv/info/ps |
| `cfg_type_text` | 216 | cfg/utest |
| `task_state_name` | 230 | ps/task |
| `cfg_recv_exact` | 2672 | cfg/fs |
| `ushell_drop` | 1787 | ushell/console task |
| `cmd_stack` | 1026 / 1181 | **two definitions with the same name**: the real implementation under `#if ARKRTOS_USE_STACK_USAGE` and a notice stub under `#else`; they must move together |
| `cmd_pool` | 2596 | registration point in one place, definition in another; do not go by section alone |

Among static variables only `g_shell`, `g_shell_stack` (the console itself) and `g_ushell`, `g_ushell_out` (user-command output buffer) need cross-file visibility; the rest (`g_blk_scratch`, `g_fs_scratch`, `rt_unit_*`) can travel with their family.

## 5. Three hard constraints

1. **Command registration stays in one place.** `g_cmd_table` is a fixed array `ark_shell_cmd_t g_cmd_table[ARK_SHELL_MAX_COMMANDS + 1]` (`ARK_SHELL_MAX_COMMANDS = 56`) and the capacity check happens once in `register_kernel_commands()` (`need = 49 + optional features`). If the split turns into "each family registers itself", they must share one `g_cmd_count` and each check remaining capacity: **overflow is a silent out-of-bounds write** — no error, no crash, just a clobbered neighbour variable.
2. **Only one output exit.** All printing goes through `sh_out` / `ark_shell_printf`; user-command output additionally shares `g_ushell_out`, the buffer the SVC `0x1A` trampoline copies back to the App block by block. A second exit produces "the same command prints differently on the console and in an App".
3. **Gates travel with the family; never split a gate across files.** The outer whole-file gate is `SHELL_ENABLE` (in the partition header); inner families have `ARKRTOS_USE_BLK / _FS / _VFS / _LOG / _UTEST / _SECCHECK / _RTSTAT / HW_SAFE`. Putting a family's `#if` head in one file and its `#else` stub in another is the easiest way to break the "still links when switched off" contract.

## 6. Migration order (every step is a stopping point)

1. Add only `arkrtos_shell_internal.h`: turn the §4 symbols into "declared in the header, defined in the `.c`", dropping `static`. **This step changes no behavior**; get all gates green first (28 checks at the time of writing).
2. Move `ushell` (312 lines, the cleanest boundary): it has its own registry and trampoline, and its only cross-family dependency is `sh_out`.
3. Move `fs + blk` (~2700 lines, the biggest block, and the easiest to prove "the commands are still there").
4. Move `image`, `cfg`, `diag`, `misc`, running the gate once per family.
5. Only then tidy the `need` bound and the file-header comment in `register_kernel_commands()`.

Acceptance per step: `env -u PYTHONHOME -u PYTHONPATH py -3 tools/ci_gate.py` all green + Keil project 0 error + the `help` command set byte-identical to before + the `Code` delta honestly recorded.

## 7. Why it was not done at first (judgement of 2026-11-13; implemented 2026-11-17, see §10)

* No DAP probe on this machine (`pyocd list` shows nothing), so a split could only prove "compiles + gates pass", not "every command behaves the same on the board". The same limit applied to the SVC split, but the shell surface is wider (44 commands), so blind splitting costs more.
* Pure moving still changes `Code` layout (function order, ICF, inlining decisions), so "size unchanged" is not a criterion. A board `help` baseline and at least one read-only command per family must exist before equivalence can even be discussed.
* Hence that round delivered only the map; before touching anything, get a probe or record a `help` baseline while a board is available.

## 8. Risks

| Risk | Note |
|---|---|
| Line numbers drift | the ranges are readings of the 6244-line snapshot; when reorganizing, find by **symbol name**, not by line number |
| Duplicate definitions | `cmd_stack` has two (real implementation / notice stub); move them together or you get either a duplicate definition or "does not link when the switch is off" |
| Copied static variables | defining buffers like `g_ushell_out` once per family compiles fine and behaves differently |
| Encoding | this file is UTF-8 + LF (not GBK), so splitting avoids transcoding; but new files must match, or the `mixed-eol` / `encoding` gates go red |
| Gate combinations | compile both the all-off and all-on profiles: a dozen of the 44 commands sit inside `#if`, and one missing `#else` stub is a link error |

## 9. Acceptance criteria

* every new file states at the top which families it owns and which gate applies;
* the command table in `docs/内核Shell控制台使用说明.md` maps one-to-one to the split files;
* `ci_gate` all green; Keil full build 0 error;
* the `help` command set is byte-identical to before the split (same order too);
* `Code / RO-data / RW-data / ZI-data` are all reported, with changes written into this page or `docs/欠账盘点.md`.

## 10. Implementation record (done 2026-11-17)

By the time work started, the main file was no longer the 6244-line snapshot: SVC splitting plus `sec` / `utest` / `rt` / `pool` commands had been added over the year, and it was actually **7952 lines**. Line ranges were therefore re-cut against 7952, but the principle held: **cut by symbol, do not copy this page's line numbers**.

The split script is `build/split_shell.py` (`build/` is ignored and not committed). It does exactly three things: move by section ranges; drop `static` from functions **really used across files** (family-internal helpers stay file-visible, so the whole shell namespace is not poured into globals); write the shared declarations into the internal header. Command bodies were not changed by a single character.

| File | Lines | Owns |
|---|---|---|
| `arkrtos_shell.c` | 569 | console instance and stack, cross-family utilities (`sh_out` / `parse_u32` / name tables), `register_kernel_commands()`, console task |
| `arkrtos_shell_internal.h` | 205 | internal header: includes + feature fallback macros + shared declarations (67 prototypes + `g_shell` / `g_ushell`) |
| `arkrtos_shell_ushell.c` | 313 | user command registry + SVC 0x1A trampoline |
| `arkrtos_shell_image.c` | 796 | `info` / `app` / `drv` / `install` |
| `arkrtos_shell_diag.c` | 1196 | `task` / `stack` / `sched` / `audit` / `guard` / `crash` / `fault` / `log` / `syncinfo` / `rt` / `sec` / `utest` |
| `arkrtos_shell_cfg.c` | 420 | `pool` / `cfg` |
| `arkrtos_shell_fs.c` | 2730 | `blk` / `fs` / `pwd` / `cd` / `df` / the whole VFS command set |
| `arkrtos_shell_misc.c` | 318 | `history` / `uname` / `free` / `ps` / `which` / `man` / `dmesg` / `hwsafe` |
| `arkrtos_shell_mod.c` | 357 | `exec` / `wait` / `ld` / `lkup` / `modules` / `unld` |
| `arkrtos_shell_pmap.c` | 1249 | the whole `pmap` family |

### 10.1 How behavior equivalence is proven (not "looks the same")

1. **Silence the noise first**: the periodic prints of `task_a` / `task_b` in `example/qemu_loongarch64/main.c` are wrapped in `#if !defined(ARKRTOS_QEMU_DEMO_QUIET) || (ARKRTOS_QEMU_DEMO_QUIET == 0)` (undefined by default ⇒ zero behavior change). A separate `build/qemu_la64_quiet` built with `-DARKRTOS_QEMU_DEMO_QUIET=1` is what makes two runs comparable line by line (measured).
2. **Run 37 read-only commands** and diff the transcripts line by line (`build/shell_regress_cmds.txt` + `build/shell_cmp.py`). The comparator does exactly four things: strip ANSI escapes; delete `[A]`/`[B]` timing lines **together with their newline** (otherwise fragments like `bus in` remain); strip the prompt; drop empty lines.
3. **Mask only what naturally varies**: all hex is masked (a layout change shifts entry addresses in the task table en masse); the numbers of `info` / `task` / `stack` / `fault` / `syncinfo` / `trace dump` / `dmesg` / `guard` are masked too.
4. **Prove the criterion itself has resolving power first**: the same image run twice must also match line by line. This caught two real noise sources — the `task` state column flipping between `WAIT` / `READY`, and `stack` high-water marks differing per run (same image, two runs: task 3 reported 2324 / 2180, task 5 reported 3044 / 2708). Both were demoted by evidence to "structure only" — **not** loosened to make the result green.

Readings (post-split vs pre-split baseline, same quiet profile):

* `SHELL-CMP OK: 37 条命令输出逐行相同` (all 37 outputs identical line by line)
* the `help` set of 59 commands is byte-identical in content and order (baseline in `build/help_baseline_cmds.txt`)

### 10.2 Size (same configuration, before/after)

| | text | data | bss |
|---|---|---|---|
| before | 367304 | 416 | 767508 |
| after | 366744 | 440 | 767508 |

text **−560 B**, data **+24 B**, bss unchanged. The minus sign is not an "optimization": across TUs both inlining decisions and string-merge boundaries change (§2 already says pure moving changes layout), so size is bookkeeping only, never a criterion.

### 10.3 How the gates had to change (this part matters more than the split)

Five gates used to grep strings directly in `kernelsrc/shell/arkrtos_shell.c` as pins. Once the commands moved, those pins **matched nothing at all and still passed** — that is idling, not coverage. So:

* new `tools/shell_src.py` (`rel_list()` returns the full `kernelsrc/shell/arkrtos_shell*.c` list), and the pins of `elf_gate` / `shell_auth_gate` / `paging_gate` now read the **union**;
* the `rtstat` / `seccheck` / `swap` "compiled-in" profiles now compile **every** shell source file, so the profile is not just an empty shell;
* `elf_gate`'s F2 pin was loosened from `static int cmd_wait(...)` to `int cmd_wait(...)` (the function is cross-file now, `static` cannot remain). The criterion itself was not relaxed: it still checks that wait treats INVALID as end, takes the code from the TCB, and distinguishes "no code reported" from "code is 0"; all three mutation self-checks still run.

Full gate: `44 check(s) passed, 0 failed, 0 skipped`.

### 10.4 Board readings (as they are)

* No DAP probe on this machine: **on-board verification was not done**. This round reached only "QEMU runs + 37 command outputs identical line by line + all gates green".
* The `stack` "watch it" warning line is not usable as evidence (see §10.1 item 4): across three readings task 5's high-water mark floated between 2708 / 3044 / 3092, worst case 75%, still 1004 B of headroom to the 90% warning.
