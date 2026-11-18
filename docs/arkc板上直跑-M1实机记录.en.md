# arkc On-Board Direct Run — M1 Real-Machine Record (Fixed Window)

> Condensed English edition. The Chinese page (arkc板上直跑-M1实机记录.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

> Automated re-runs of the criteria are in the error-code and regression-ledger page §5
> (`py -3 tools/final_app.py`; its `m1-fixed-window` / `m1-wrong-window` items are the
> positive and negative cases of §6 here).

This is the landing record for **M1 (L1 fixed-window execution)** from the arkc
build-artifact-on-board implementation plan: on an STM32H743 board, compile a bare image
with the on-board arkc (`cc` in the shell), then have the kernel's `app run` load it into a
fixed RAM window and execute it as an unprivileged task.

Date: 2026-11-05. Every reading comes from the real board's serial port (COM3 / 115200);
none is inferred.

---

## 1. One-sentence conclusion

`cc --scatter <sct> --vector-table -o /tmp/p.bin` → `app run /tmp/p.bin` → the serial line
shows **the two lines unique to the source**:

```
BRD-M1-R1 alive
BRD-M1-R1 tag alpha
```

After the program exits itself with `svc 0x1B` the window is returned (`task` back from 5
entries to 4, `fault` empty, pool usage in `free` back to 0), and **running it again still
works**.

---

## 2. Entry contract (M1 final, differs from the plan's early wording in §4.1)

The plan's §4.1 originally imagined "crt0 is the first link input ⇒ entry offset 0". After
measurement this changed to a **vector-table contract**; the reasoning and evidence follow.

- arkc's single-file `-o` output places `.rodata` (string literals) **before all functions**
  in the same source, so "the file's first byte is the first instruction" does not hold.
- When `Reset_Handler` is defined, `arm_link()` auto-generates a 256-entry vector table
  (0x400 B): `+0 = initial SP`, `+4 = Reset_Handler | Thumb`. That is exactly the Cortex-M
  load convention.
- The earlier implementation shifted the whole code region right by 0x400 and then adjusted
  symbol offsets, which made absolute addresses already baked in at assembly time **0x400
  too low** (programs with strings were guaranteed garbage). It is now **reserved in
  place**: the assembler emits from `ARM_VT_SIZE` onward, the first 0x400 bytes of the
  buffer are pre-zeroed, and the link stage **does not move anything**. CLI switch
  `--vector-table`; the default path is byte-identical (host measurement: same source, same
  `.sct`, with and without the switch both 3114 B, `BRD-M1` at `0x400`).

So the kernel validates **the image's own vector table**, not "which byte of the file the
entry is at":

| Check | Error code when violated |
|---|---|
| file ≥ 8 B and `+0` is exactly the window top (`APPRUN_RAM_BASE + APPRUN_RAM_SIZE`) | `-23 APPRUN_ERR_LAYOUT` |
| the entry at `+4` falls inside the code window | same |
| the instruction at the entry is arkc's prologue `0xB5E0` (`PUSH {r5,r6,r7,lr}`) | `-22 APPRUN_ERR_PROLOGUE` |

A mismatch is **refused**: running an image linked for a different layout means executing
arbitrary bytes.

---

## 3. Window and block borrowing

- `APPRUN_BASE = SLOT_RAM_BASE = 0x24070000`, code window 8 KB, data/stack window 8 KB
  (`0x24072000`), window top `0x24074000` (= initial SP), stack takes the last 4 KB of the
  data window.
- Borrowing goes through **the small-program RAM pool first-fit**
  (`arkrtos_ptable_mini_alloc`), **but the result must be checked field by field**: the
  block obtained must equal the window constants exactly, otherwise it is returned
  immediately and `-21 APPRUN_ERR_WINDOW` is reported. If first-fit hands out anything
  else, there is an earlier free block of that size in the pool — running there would jump
  into the void, so it is refused, not accommodated.
- **`arkrtos_ptable_alloc_raw()` cannot be used**: it checks the **Flash** image pool
  (`CONFIG_BASE + CONFIG_SIZE`), not this RAM window; using it to register a RAM range
  always fails (measured `rc=-21`).

---

## 4. Two pitfalls that must be written down

### 4.1 The image lives on VFS ramfs, not on the littlefs volume

Ark RTOS's file system has two layers: `arkrtos_fs_*` (littlefs, volume needs `fs mount`)
and `arkrtos_vfs_*` (`/` volatile ramfs, `/dev`, `/mnt/<name>`). The output of the on-board
`cc` lands at `/tmp/p.bin`, which is **VFS ramfs**. Reading it with `arkrtos_fs_*` yields
only the **wrong answer** "no file system mounted".

- File reading switched to a streaming `arkrtos_vfs_open_read/read/close`; "buffer too
  small" is judged by "after filling, probe one more byte", and **nothing is truncated past
  the window**.
- The precondition changed from "littlefs mounted" to **`arkrtos_vfs_ready()`**; the shell
  prompt text was updated in step (no longer pointing people at `fs mount`).
- Another hard constraint: ramfs has only **3 file slots** (`ARK_RAMFS_MAX_FILES=3`, 16 KB
  static buffer each). `/tmp/p.c` + `/tmp/p.sct` + `/tmp/p.bin` is exactly three, so test
  scripts must `rm` before writing.

### 4.2 `--scatter` reached `opts` but not `g_config` (silently wrong answers)

The board's `cc` put `--scatter` into `ArkOptions::scatter_file`, while the ARM back end
reads only `g_config.scatter_file`. Result: **compilation succeeds, the `.sct` is ignored,
and the image is linked with the built-in `0x08000000 / 0x20020000`**; reading the board
artifact back over serial shows `SP=0x20020000 / entry=0x08000429` (the string at `0x400`
shows the `--vector-table` pass was correct).

Fix (inside the compiler, not once per embedder):

1. bridge `opts.scatter_file` into `g_config` in `ark_compile()` (only when `g_config` is
   empty);
2. the back end **no longer falls back** when parsing the `.sct` fails during codegen (the
   original was `if (...) { use scatter } else { use default }`, silent);
3. `ark_compile()` compares `diag_errors()` before and after `emit_asm`: **any codegen
   error refuses to produce an image**, plus the extra message
   `the requested layout in <sct> could not be applied; refusing to fall back to the built-in layout 0x08000000 / 0x20020000`.

Three host measurements (same source):

| Command | Exit code | Artifact |
|---|---|---|
| `--scatter b1.sct --vector-table` | 0 | 3114 B, VT `24074000 / 24070429` |
| `--scatter nope.sct --vector-table` | 1 | **no image**, two errors |
| without `--scatter` | 0 | byte-identical to the pre-change `arkcc.exe` (md5 `59167791…`) |

---

## 5. Priority: the direct-run program sits **below** the console

`ARKRTOS_APPRUN_PRIORITY = SHELL_TASK_PRIORITY + 6` (20), `ARKRTOS_APPRUN_PERIOD_MS = 0`.

This is not tuning but a measured lesson: originally it reused `APP_TASK_PRIORITY` (10,
above the shell's 14), and a `for(;;){}` program **starved the shell permanently** — the
prompt never came back and even `task`/`fault` were unreachable. At 20:

- while the shell is idle (waiting on serial) the app owns the CPU;
- the moment a human types, the shell (14) gets its slice immediately and `task`/`fault`
  remain usable;
- period 0: a direct-run program has no period contract, and showing 1000 in the table
  would make people think it is periodic.

---

## 6. On-board readings

### 6.1 Normal path (a 3006 B program, run twice)

```
cc: scatter /tmp/p.sct
cc: vector table in the first 0x400 B
[scatter] parsed /tmp/p.sct
cc: done in 42 ms
...
APPRUN: /tmp/p.bin: 3006 B -> code 0x24070000 (8192 B), data/stack 0x24072000 (8192 B), task 4
BRD-M1-R1 alive
BRD-M1-R1 tag alpha
TASK: task 4 exits (self kill)
APPRUN: task 4 exited, window returned to the pool
app run: /tmp/p.bin started (task 4)
task: 4 used / 48 max，第 4 项 prio 20 / period 0 / INVALID / entry 0x24070429
fault: no fault recorded
free: pool(image) 1310720 total / 0 used
```

The second `app run` prints the same output (still task 4, window reclaimed cleanly).

> The task number in "started (task 4)" is taken **at the moment of start**: the app can
> exit before the shell prints, and reading back `arkrtos_apprun_task_id()` would then give
> 0 (producing self-contradictory lines such as "started (task 0)"), hence
> `arkrtos_apprun_run_ex(path, &task)`.

### 6.2 Anti-false-green

| # | What was done | Expectation | Measured |
|---|---|---|---|
| ① | change the source string (`BRD-M2 hot loop`), recompile, rerun | only the new string appears | only `BRD-M2 hot loop` appeared |
| ② | program `for (;;) { }` never exits | it really runs: `READY` in `task`; a second `app run` reports BUSY | `4 20 READY 0`; `rc=-20 - a program is already running` |
| ③ | relink with `.sct` at `0x24040000 / 0x24043000` | explicit window mismatch, **must not** happen to run | `rc=-23 - image was not linked for this window` + the VT constraint note; `task` back to 3 entries |
| ④ | image built with `USE_APPRUN=0` | `run` **absent** from `app` and `help` | both usage and help show only `list/install/start/stop/uninstall` |

---

## 7. Size accounting (switch discipline)

| Profile | FLASH | Note |
|---|---|---|
| HEAD (clean copy, `USE_APPRUN=0`) | 381512 B | baseline |
| this round, `USE_APPRUN=0` | 381808 B | **+296 B** |
| this round, `USE_APPRUN=1` | 385760 B | adds `arkrtos_apprun.o` and the shell's `run` branch |

The +296 B was characterised symbol by symbol with `nm -S` and **all of it lands in the
vendored arkc**: `ark_compile +104`, `arm_assemble +48`, `arm_link +20`,
`pass2_a.constprop.0 → pass2_a` (12900, same size), `CSWTCH +20`, new
`g_arm_vt_reserve` (4). **Kernel and application symbol sizes are unchanged**; the default
compiler path's output is byte-identical to before (§4.2 table). So "profile 0 behaves the
same" still holds, but **it can no longer be written as "byte-identical to HEAD"** — the
size really grew by 296 B, inside the compiler itself.

With `USE_APPRUN=0`, `arkrtos_apprun.c` is excluded from compilation (`sources.mk` filters
by `USE_APPRUN`) and the shell's `run` branch is inside `#if`.

---

## 8. M1's known boundaries (not fixed, belongs to the next stage)

| Boundary | Status | Owner |
|---|---|---|
| only one direct-run program at a time | a second `app run` reports `-20 BUSY`, no queueing | by design (narrow M1 entry) |
| no relocation | the image must be linked for the window address (`.sct` hard-codes `0x24070000`) | M2 (L2 relocatable, via the existing mini path) |
| no persistence | the image sits on VFS ramfs and dies on restart | M3 |
| only 3 file slots | `rm` before writing; `/tmp/x.c` + `/tmp/x.sct` + `/tmp/x.bin` is exactly three | after M3 |
| no multi-input linking | single-file `cc` suffices (`-link` is wired but unused in M1) | M2 |

---

## 9. Related code locations

| File | Content |
|---|---|
| `kernelsrc/src/arkrtos_apprun.c` / `include/arkrtos_apprun.h` | borrow window, read file, vector-table validation, start task, exit reclaim; error codes `-20/-21/-22/-23` |
| `kernelsrc/shell/arkrtos_shell.c` | `app run <path>` subcommand and the explanation for each error code |
| `kernelsrc/src/arkrtos_task.c` | task-exit hook calls `arkrtos_apprun_on_task_exit()` (inside the switch) |
| `kernelsrc/components/arkc/` | vendored arkc (synced by `sync_arkc.py`); `arkc_shell_cmd.c` parses `--scatter` / `--vector-table` / `-link` |
| `config/stm32h743/arkrtos_partition.h` | section 12: window constants + compile-time self-checks; the reasoning for priority/period |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | `USE_APPRUN ?= 0` |
