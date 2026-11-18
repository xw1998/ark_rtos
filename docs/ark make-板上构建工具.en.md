# ark make: the On-Board Build Tool

> Condensed English edition. The Chinese page (ark make-板上构建工具.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

## 0. One line

The board already has `cc`, `vim`, `shell` and `vfs`, but packing **several `.c` files into one runnable image** still required hand-typing a long command line: `cc --pack a.c b.c c.c -o app.svcm`. Forgetting one source is an invisible error — the image links fine, it just lacks that translation unit. `ark make` turns this into one project description file plus one command:

```
ark@arkrtos:/mnt/nor$ make
make: /mnt/nor/app.svcm <- /mnt/nor/a.c (+more)
make:   src /mnt/nor/b.c
cc: pack 2 sources -> /mnt/nor/app.svcm (4 passes, -O1)
...
cc: /mnt/nor/app.svcm: payload 4504 + table 4 x 4 = 4776 B
cc: packed in 38011 ms
make: ok
ark@arkrtos:/mnt/nor$ mini run /mnt/nor/app.svcm
ARKMAKE: link ok
ARKMAKE: cross-file add3=6
```

## 1. Usage

```
make [<makefile>] [-f <makefile>] [-o <out>] [-v] [-j]
make clean [<makefile>] [-f <makefile>] [-o <out>]
```

| Argument | Meaning |
|---|---|
| `<makefile>` / `-f <f>` | project description, default `makefile` (resolved against the current directory) |
| `-o <out>` | overrides the project's `OUT` |
| `-v` | print `cc --pack`'s per-object readings (per pass, per source: code length and placement) |
| `-j` / `-jN` | **recognized but has no effect**: one core, one compile pool, the build is serial by construction |
| `clean` | delete the **finished image** this project builds (no intermediates exist, see below) |

A wrong option is reported **immediately**, not after thirty seconds: a build takes tens of seconds, and discovering a bad argument at the end is the most wasteful kind of failure.

`-j` is handled by **saying so rather than pretending or erroring out**: someone copying `make -j8` from a desktop project should read one plain sentence, not believe they got parallel speedup.

```
ark@arkrtos:/mnt/nor$ make -j8
make: -j has no effect: one core and one compile pool, the build is serial by construction
make: /mnt/nor/app.svcm <- /mnt/nor/a.c (+more)
...
```

`make clean` deletes the **finished image**, because object files do not exist once the four passes finish — they live in `cc_pack`'s compile pool (or a static arena) and never touch the volume. So it does exactly one thing:

```
ark@arkrtos:/mnt/nor$ make clean
make: clean: removed /mnt/nor/app.svcm (4776 bytes)
make: clean: no object files to remove (they live in the compile pool)
ark@arkrtos:/mnt/nor$ make clean
make: clean: /mnt/nor/app.svcm: no such image
```

The second returns success (GNU make's clean also succeeds when there is nothing to delete), but "not found" must be said out loud, never done silently. `clean` also runs the full makefile parse: if `OUT`/`TARGET` cannot be determined it cannot know what to delete, and that step fails rather than guessing a name.

## 2. Why it is deliberately not GNU make

All three are "not true on this board", not "not finished":

| GNU make feature | Why it does not hold on board |
|---|---|
| timestamp dependencies (`mtime`) | littlefs / ramfs do not guarantee reliable `mtime`. Deciding whether to rebuild by comparing times gives a random answer — and when wrong the image still looks perfectly fine |
| implicit / suffix rules | need pattern matching and search-path semantics; their failure modes are "no rule available" and "a rule you never thought of", both hard to explain on a board |
| recipes (one shell per line) | there is no shell child process and no `sh -c` on board. Leaving a fake recipe only makes people think they can write `mkdir -p` |
| parallel build (`-j`) | one core, one compile pool (object slots are a static arena), nothing to parallelize. The flag is accepted, and its non-effect stated |
| `clean` | only the "delete the finished image" layer is kept: no `.o` to delete and no timestamp dependency to clean up |

**Everything else is an error, never silently ignored.** Building something while failing to parse is the failure mode this tool must avoid most. Measured:

```
ark@arkrtos:/mnt/nor$ make -f bad.mk
make: bad.mk: line 4:  expected `=` after the variable name (rules and recipes are not supported)
ark@arkrtos:/mnt/nor$ make -f nope.mk
make: /mnt/nor/nope.mk: line 0:  cannot read the makefile
```

## 3. Syntax

Only three forms:

1. `NAME = value` — overwrite assignment
2. `NAME += value` — append (appending to an undefined variable is an error, not implicit creation)
3. `#` to end of line — comment

`$(NAME)` in values expands recursively (depth limit 8, self-reference is an error). Only five project fields:

| Key | Required | Meaning |
|---|---|---|
| `TARGET` | yes | product name; default output name `<TARGET>.svcm` |
| `OUT` | no | full output path; empty means derive from `TARGET` |
| `SRCS` | yes | space-separated sources, **limit 4** (aligned with `CC_PACK_MAX_SRCS`) |
| `DEFS` | no | space-separated macros, limit 16, form `LOG_LEVEL=2` |
| `INCS` | no | space-separated include directories, limit 8 |

Minimal working example:

```
TARGET = app
OUT    = /mnt/nor/app.svcm
SRCS   = /mnt/nor/a.c /mnt/nor/b.c
```

Absolute paths in `SRCS` are the safest: paths resolve against the **current working directory**, and which directory you typed `make` in is up to you.

## 4. On-board measurements (2026-11-05, STM32H743)

> **2026-11-07 correction**: the table below is the **on-chip RAM / 80 KB compile pool** profile. After switching to `USE_SDRAM=1` (4 MB pool) the same two-source project measured **1522 ms** (see arkc板上直跑-M4-hello演示.md §3); the difference is the pool, not the algorithm — a small pool must shuffle objects on every pass, so four passes × two sources is four times the overhead. Also mind the **timing convention**: the same command once measured 11124 ms because a `for(;;)` mini program was running at `APP_TASK_PRIORITY`(10) and starved the console (14), so `make` got only scraps of CPU. Whenever a duration looks unreasonable, first confirm no higher-priority task is occupying the CPU (the demo project defaults to `MINI_PRIORITY=20`, see docs/功能开关.md).

| Case | Sources | Product | Time |
|---|---|---|---|
| `make` (a.c 52 B + b.c 452 B) | 2, cross-TU call to `add3` | `app.svcm` 4776 B (payload 4504 + 16 B table) | 38.0 s / 45.2 s (`-v`) |
| `make -f mk2` (DEFS/INCS probe) | p.c 134 B + q.c 482 B | `probe.svcm` 4800 B | 19.5 s |
| `make -f mk3b` (missing source) | one nonexistent | — | fails immediately |

Running them:

```
mini run /mnt/nor/app.svcm   ->  ARKMAKE: link ok
                                 ARKMAKE: cross-file add3=6
mini run /mnt/nor/probe.svcm ->  ARKMAKE: defs+incs ok, probe=42
```

The second probe is anti-false-green: `probe()` returns 42 only if `DEFS=ARKMAKE_DEFS_PROBE` really reached the compiler and `#include "probe.h"` really was found through `INCS=/mnt/nor`. That run's working directory was `/`, so there was no second route to the header besides `-I`.

## 5. Three hard constraints

**1. Objects must be based at their real placement, so objects cannot be reused across passes.**
arkc bakes absolute addresses into instructions at codegen time (`movw/movt` pairs, literal slots); only external symbols remain relocatable in an object. The four packing passes differ exactly in each pass's `.sct`, so a `-c` product is only meaningful under **the same `.sct`**. Hence `make` always does a full rebuild and reuses no intermediates — not laziness, but the only way to avoid "objects that moved".

**2. `make` is only a driver, not a second packing path.**
`ops->pack` calls `arkc_cc_pack()` directly, i.e. the body of `cc --pack`. Two independent packing implementations would eventually produce different images on the board, and only one of them would ever have been executed. The cost is stated at compile time:

```
#if ARKRTOS_USE_ARKMAKE && !ARKRTOS_USE_APPPACK
#error "ARKRTOS_USE_ARKMAKE=1 requires ARKRTOS_USE_APPPACK=1 ..."
#endif
```

**3. Error codes are translated by "which stage failed", never collapsed into one "failed".**
A `--pack` has five stages (compile / link / layout check / differential pack / write). The same "failed" on different stages means a completely different next step. So `arkc_cc_pack()` carries an extra stage out-parameter, and `ark make` translates it into its own error vocabulary:

| arkc stage | arkmake code | On-screen text |
|---|---|---|
| `SETUP` (borrow workspace / register object slot / write .sct) | `ARKMAKE_ERR_SETUP` | `cannot start the build` |
| `COMPILE` (including pool exhaustion) | `ARKMAKE_ERR_COMPILE` | `compile failed` |
| `LINK` | `ARKMAKE_ERR_LINK` | `link failed` |
| `LAYOUT` (four passes disagree on size / does not fit the window) | `ARKMAKE_ERR_SIZE` | `image too large` |
| `PACK` (no differential table / no vector table) | `ARKMAKE_ERR_PACK` | `cannot pack the image` |
| `WRITE` | `ARKMAKE_ERR_WRITE` | `cannot write the image` |

Measured (missing source):

```
cc: pass A: compiling /mnt/nor/nosuch.c failed
make: compile failed (-11)
```

Before the stage code was added this read `make: link failed (-12)` — a wrong answer that sends the reader to the linker while the real cause was compilation.

## 6. Component wiring

```
kernelsrc/components/arkmake/
├── include/arkmake.h          <- upstream (tool headers)
├── src/arkmake_parse.c        <- upstream (text -> project description, pure functions)
├── src/arkmake_build.c        <- upstream (project description -> one build)
├── arkmake_port.h             <- this repo: ctx and two entries
├── arkmake_port_arkrtos.c     <- this repo: read / log / pack callbacks
├── arkmake_shell_cmd.c        <- this repo: the make command
├── arkmake_shell_cmd.h        <- this repo: registration hook
└── VENDOR.md                  <- sync provenance and boundary
```

The upstream tree (gitee `xw19981010/ark_make`) is synced by `tools/sync_arkmake.py`; the four wiring files at the component root belong to the Ark RTOS side and are never touched by sync. **Sync deletes nothing**: extra files in the target are only reported — deleting a still-used file during a sync is the easiest mistake this copy-style sync can make.

Build wiring (`example/stm32h743/kernel/ARKRTOS_TEST/GCC/`):

```
USE_ARKMAKE ?= 0                       # top of Makefile
-DARKRTOS_USE_ARKMAKE=$(USE_ARKMAKE)   # DEFS
-I$(ROOT)/kernelsrc/components/arkmake           # wiring headers
-I$(ROOT)/kernelsrc/components/arkmake/include   # upstream headers
ifeq ($(USE_ARKMAKE),1)                # sources.mk
SRCS += src/arkmake_parse.c src/arkmake_build.c ...
endif
```

The kernel switch is in `kernelsrc/include/arkrtos_features.h` (`ARKRTOS_USE_ARKMAKE`, default 0), with two `#error` dependencies: missing `ARKRTOS_USE_APPPACK` or `ARKRTOS_USE_ARKC` fails to compile.

## 7. Zero change when off (measured comparison)

Same working tree, only `USE_ARKMAKE` changed:

| Build | text | data | bss | symbols | `"project file"` string |
|---|---|---|---|---|---|
| `USE_ARKMAKE=0` | 407484 | 2248 | 437756 | no `arkmake_*` / `cmd_make` | 0 occurrences |
| `USE_ARKMAKE=1` | 414516 | 2248 | 441976 | 9 | 2 occurrences |

The off build's three sizes are **byte-identical** to the firmware before this component (407484 / 2248 / 437756); the on build adds 7032 B of code and 4220 B of static (a 2 KB makefile text buffer + a 2 KB `arkmake_project_t`, both in `.bss`, not on a task stack).

## 8. Two real defects caught this round

**1. `arkmake_build()`'s summary line dropped the first source.**
The original implementation used `const char *parts[] = { "make: ", out, " <- ", NULL, NULL }` and glued with `while (parts[k] != NULL)`; the third entry is always `NULL`, so the loop stopped before appending the first source and the screen showed `make: out <-  (+more)`. And that very line is the only place where "one source is missing" is visible. Fixed in the source repo, with an assertion added in `tests/test_build.c` (the summary line must contain `a.c`); host `make test` all green.

**2. arkc's `-link` branch did not reset the pool (invisible on the host).**
Both the C and assembly paths of `ark_compile()` call `mem_pool_reset()` at entry, but the `-link` branch `return`s before that line. On the host every run is a new process, so the pool naturally starts empty and the defect cannot be reproduced there; on board `cc --pack` does **four passes in one process**, so the link stage inherited the previous compile's peak (`q.c` peak 64720 B while linking itself needs only 8488 B), blowing through the 80 KB pool in the link stage:

```
cc: pool 77400/81920 B
cc: pass A aborted: memory pool exhausted
make: link failed (-12)          <- and this was masked one layer further by the stage-code problem
```

Fix: call `mem_pool_reset()` at the entry of the `-link` branch (the linker re-reads what it needs from object files; none of the compile-time allocations are needed). After the fix the same project went from "abort on pass A" to all four passes in 19.5 s.

## 9. Cost and debts

* **Always a full build**: two sources take 19–45 s (four passes × each source + four linking passes; 80 KB pool profile — the SDRAM profile measured 1.5 s). No incremental mode, and none planned — see "no timestamps" above.
* **Per-source pool ceiling**: with an 80 KB pool at -O1, the measured peak for a 482 B `q.c` is 64720 B; two ~450 B sources already push the pool to 77 KB. Larger sources need `ARK_POOL_SIZE` raised in `ARKC_DEFS` (RAM has only about 6.8 KB of headroom now; free space elsewhere first).
* **`SRCS` limit 4**: aligned with `cc --pack`'s object slots; exceeding it fails at the parse layer with `too many entries`, not after four passes. This number **is not arbitrary**: raising it means changing four places at once — `ARKMAKE_MAX_SRCS` in `arkmake.h` (upstream), `CC_PACK_MAX_SRCS` in `arkc_shell_cmd.c`, the object-slot budget (slot capacity = arena / source count), and re-verifying on board. The concrete blockers: in the SDRAM profile the arena is 48 KB, so eight sources leave only 6 KB per slot, while a measured 10-function object with printf is 8660 B — `File too large` on the spot; the on-chip profile has only a 16 KB arena, where even four sources need 12 KB per slot to just fit. So the limit stays at 4, exceeding it is explained, never silently truncated.
* **`-j` is only "recognized"**: one core + one compile pool, the build is serial. The flag does not error, but prints the "has no effect" line every time, never pretending to be parallel.
* **`clean` deletes only the product**: there are no intermediates on board (objects live in the compile pool and vanish when the build ends), and no timestamp dependency to clean. `clean` touches neither sources nor makefile.

## 10. Reproducing

```bash
# 1. build the firmware (make does not track -D; make clean when switching switches)
make -j8 USE_QSPI=1 USE_MINI_VFS=1 USE_APPPACK=1 USE_ARKMAKE=1

# 2. after flashing, mount NOR (mount only stabilises about 3 s after boot)
mount /mnt/nor

# 3. upload the project description. fs put takes an absolute path inside littlefs
#    (/makefile), not the mount-point path (/mnt/nor/makefile) — it calls
#    arkrtos_fs_open_write directly without VFS prefix resolution, so a mount-point
#    path returns LFS_ERR_NOENT
fs put /makefile <byte count>

# 4. build and run
cd /mnt/nor
make -v
mini run /mnt/nor/app.svcm    # mini run needs an absolute path
```
