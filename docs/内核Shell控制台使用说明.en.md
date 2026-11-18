# Kernel Shell Console Guide (ark-shell port)

**[简体中文](内核Shell控制台使用说明.md) | English**

> Condensed English edition. The Chinese page (内核Shell控制台使用说明.md) is normative:
> every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese
> page wins. This page adds no new claim and no new measurement.

Source: [`kernelsrc/shell/`](../kernelsrc/shell/) (upstream
[ark-shell](https://gitee.com/xw19981010/ark-shell.git) brought in as-is plus one Ark RTOS
platform adaptation layer). Configuration: `config/arkrtos_partition.h`, section 4.

## 1. What problem it solves

The kernel offers a console task running in **privileged state**, used to observe and control
the system at runtime:

- see live state of the kernel / partitions / task table / fault record without attaching a
  debugger;
- **start and stop Apps and drivers slot by slot** — both "start it like a PC program, by
  clicking" and "shut it down anytime" land here;
- when needed, **install an image over the serial port** (`install`).

"Autostart or not" is **not a runtime switch**: it is written into the image header's `flags`
field at packaging time (`tools/pack_app.py --autostart / --no-autostart`), read by the kernel
during boot scan, and shown in the `auto` column of `app list` / `drv list`. The shell only
handles runtime start / stop.

## 2. Configuration (`config/arkrtos_partition.h`)

| Macro | Default | Meaning |
|---|---|---|
| `SHELL_ENABLE` | 1 | 0 = do not compile the console task (saves ~4 KB kernel RAM plus a task slot). **Value comes from Kconfig `ARKRTOS_USE_SHELL`** (`arkrtos_features.h` wires it as an alias); the 0 profile saves 33184 B Flash / 14620 B RAM in measurement |
| `SHELL_DEV_NAME` | `"ttyS0"` | console serial device name (as registered by the board) |
| `SHELL_DEV_ARG` | 115200 | parameter passed when opening the device (baud rate) |
| `SHELL_TASK_PRIORITY` | 14 | console priority, lowest, so interaction never steals CPU from business tasks |
| `SHELL_TASK_STACK_SIZE` | 3072 | console task stack in bytes, including the ark-shell line-edit buffer |
| `SHELL_TASK_PERIOD_MS` | 2 | poll interval per round |
| `SHELL_INSTALL_TIMEOUT_MS` | 120000 | upper bound waiting for an image header in the `install` window |

> **Serial port ownership**: with `SHELL_ENABLE = 1` the kernel **no longer registers the
> resident install task**; read rights belong to the console, so two readers never randomly
> split the same UART FIFO byte stream. Installation moves to a one-shot window triggered by the
> `install` command (reusing the same device handle). With `SHELL_ENABLE = 0` the resident
> install task returns.

### 2.1 Boot banner

When the console starts it prints ASCII art, then the prompt:

```
 ______     ______      _    ___  ____
/ ___\ \   / / ___|_ __| |_ / _ \/ ___|
\___ \\ \ / / |   | '__| __| | | \___ \
 ___) |\ V /| |___| |  | |_| |_| |___) |
|____/  \_/  \____|_|   \__|\___/|____/

Ark RTOS kernel shell - type 'help' for commands.

ark@arkrtos:~$
```

- The text lives in `ARK_SHELL_WELCOME` in `kernelsrc/shell/ark_shell/ark_shell_config.h`,
  **not in `arkrtos_shell.c`**: `ark_shell_init()` prints the welcome text and then draws the
  prompt, so anything the kernel task prints after init lands **below** the prompt and looks
  like "the banner is stuck inside the command line". Setting `ARK_SHELL_WELCOME_ART` to 0
  returns to the generic text (`Ark Shell v1.0` + `Type 'help' for commands.`).
- The banner appears **once, at startup**; after each command only `ark@arkrtos:~$ ` is
  redrawn. To wait for the device to be ready, wait for the prompt or for silence, not for the
  banner.

## 3. Command overview

| Command | Usage | Meaning |
|---|---|---|
| `help` | `help` | list all commands (including those below) |
| `info` | `info` | kernel build time, partition table (ABI / hardware signature / each section's address and size), task usage, time base |
| `app` | `app [list \| start <slot> \| stop <slot> \| uninstall <slot>]` | App slot table: type / state / auto / task id / crash count / base / size / RAM window / entry; start, stop or uninstall one slot |
| `drv` | `drv [list \| start <slot> \| stop <slot> \| uninstall <slot>]` | driver slot table, same semantics as `app` |
| `task` | `task [<id> [x]]` | without arguments: kernel task table (priority / state / period / wait time / stack peak / entry). Task ids **start at 1** (0 = no task), the same numbering as the `task` column in `app` / `drv`. With an id: that one suspended task's state — `sp` / state / **all 56 frame words verbatim**; a third argument adds `blocks=` and `last_reason=` (4 = HANDOFF observed). The frame words are deliberately unfiltered: whether the FPU extended frame is present decides which word is the PC, and guessing turns a backtrace into a wrong answer |
| `sched` | `sched` | scheduler self-check: compares the ready set (256-bit two-level bitmap plus per-priority lists) **entry by entry** against the task table. Consistent → `sched: consistent (bitmap/links == task table)` plus `ready=<count> top=<highest-priority task>`; inconsistent → prints `#idx prio= status= on_ready= next= prev=` per entry, so the unlinked task is located directly |
| `fault` | `fault` | fault ring record: type, task id, instant (including `INSTALLFAIL`) |
| `install` | `install` | open a one-shot install window and wait for one `.svcapp` image (the host types the command first, then sends the file) |
| `log` | `log [0..4]` | without arguments read the current log level, with an argument change it (0=off…4=debug). Historical ring content is read with `dmesg` |
| `pool` | `pool` | image pool free space: `free` / `largest`, plus fixed slot-type occupancy inside the pool |
| `trace` | `trace [start \| stop \| reset \| dump \| mark <n>]` | start, stop, reset, dump the kernel event trace and place marks |
| `cfg` | `cfg [show \| load \| clear]` | device-side layout configuration: show effective values / reload from the config sector / erase the config sector and return to compile-time defaults |
| `syncinfo` | `syncinfo` | synchronisation health check: the sem / mtx / cond tables (count / owner / creator / waiters) plus per-semaphore `posts takes queued last_waiter qbefore woke reason`, ending with three lines `ghost` / `dup` / `appwait hit`. Criterion in §8 |
| `stack` | `stack [check \| raw]` | task stack watermark: total / used / percentage per task, warn above 75%, danger above 90%. `check` appends a verdict line and expresses the result in the return code; `raw` prints the fill watermark and the lowest PSP separately, plus bottom-of-stack samples, to tell which leg hit the boundary. See "Task stack usage (`stack`)" below |
| `crash` | `crash` | cross-reset crash journal: UNINIT area address and size, `boots` / `seq`, per-slot fault count / last reason / disable reason / disable time |
| `guard` | `guard` | per-task heartbeat contract: declared or not, period, report interval, verdict bit |
| `audit` | `audit` | kernel structure self-audit |
| `sec` | `sec [all \| pass \| fail \| find \| skip]` | security baseline self-test (12 read-only items): compile-time gates, slot RAM windows, structure invariants, image header, boot re-check CRC (SKIP), image signature, hardware security state, watchdog, crash ledger, console authentication, Flash protection (SKIP), real-time readings. Four verdict classes `PASS` / `FAIL` / `FIND` / `SKIP`; return code = number of FAILs. See [安全基线自检](安全基线自检.md) |
| `blk` | `blk [list \| probe \| rd \| wr \| erase \| test]` | block devices (NOR / internal Flash): enumerate, probe, read, write, erase, self-test |
| `fs` | `fs [mount \| unmount \| format \| info \| ls \| wr \| rd \| put \| rm \| test \| err]` | littlefs volume: mount / unmount / format / capacity / list / write / read / receive a file / delete / self-test / error names. A mount point looks like `mounted nor0 at 0x0, 4194304 bytes` |
| `mini` | `mini [list \| run <path> \| stop [n] \| count [n] \| limit [bytes] \| forget <path> \| autostart [list \| add <path> \| del <path> \| now]]` | small programs (the third kind of user-mode application: a file in the filesystem loaded into memory by `run`): list instances / run a path / stop one / query or change the concurrency limit / clear one path's crash count / maintain the boot autostart list `/mini.autostart` |
| `pwd` | `pwd` | print the current working directory (`/` shows as `~`) |
| `cd` | `cd [dir]` | change directory: absolute, relative, `.` / `..` / `~`; a missing target or a non-directory reports an error and **does not change** the current directory |
| `ls` | `ls [-l] [-a] [path]` | list a directory (resolved against the current directory). `-l` adds type characters and sizes; `-a` appends `.` and `..`. **No owner / timestamp fields** (the VFS does not provide them; none are invented) |
| `cat` | `cat <path>` | print a file |
| `rm` | `rm <path>` | delete a file |
| `mount` | `mount [<target> \| <dev> <off> <size> <target>]` | list mount points, or view / create a volume when given no arguments |
| `umount` | `umount <target>` | unmount a mount point |
| `df` | `df` | list mount points and volume usage; unknown capacity prints `-`, never invented |
| `ps` | `ps` | overview of kernel tasks and installed images (**no TTY / CPU-time fields**: the kernel does not keep such accounts) |
| `free` | `free` | memory overview: kernel RAM window and image pool (there is no kernel heap, so `used` / `free` print `n/a`) |
| `uname` | `uname [-a]` | system and build information (architecture / version / build time) |
| `history` | `history` | print command history with numbers |
| `which` | `which <command>` | resolve an alias to the real command |
| `man` | `man <command>` | same as `help <command>`, prints one command's usage |
| `dmesg` | `dmesg [-c]` | print the kernel log ring (oldest → newest); `-c` clears after printing |
| built-in | `version` / `clear` / `echo` / `reboot` | `reboot` goes through the platform hook and writes `AIRCR.SYSRESETREQ` to reset the whole device |

### 3.1 Linux habits cheat sheet

The prompt is the full bash form `ark@arkrtos:<cwd>$`, with `~` shown when `cwd` is `/`:

```
ark@arkrtos:~$ pwd
/
ark@arkrtos:~$ cd /dev
ark@arkrtos:/dev$ ls
BLED  led0  led1  led2  ttyS0
ark@arkrtos:/dev$ cd ..
ark@arkrtos:~$
```

- **Relative paths**: `ls` / `cat` / `rm` / `cd` resolve against the current directory;
  `.` / `..` / `~` / `~/x` and absolute paths all work, and escaping above the root is an error
  (**not** clamped back to the root).
- **Exit code**: `$?` prints the previous command's return code (Linux convention: 0 = success,
  non-zero = failure; unknown command is 127).
- **Single-command help**: `cmd -h` or `cmd --help` prints that command's usage, same as
  `help <cmd>` / `man <cmd>`.
- **Aliases**: `ll`→`ls -l`, `la`→`ls -a`, `dir`→`ls`, `cls`→`clear`, `halt`/`poweroff`→`reboot`;
  `which <alias>` shows the real command.
- **Error wording**: `<name>: command not found`, `<cmd>: too many arguments`,
  `<path>: No such file or directory`, following Linux phrasing.

### 3.2 Line editing and terminal conventions

Echo is the line editor's responsibility alone; input bytes are never echoed again elsewhere.
Two rules follow, to be checked when changing the input paths of
`ark_shell_line_editor.c` / `ark_shell.c`:

| Action | Bytes sent to the terminal | Rationale |
|---|---|---|
| append one character at end of line | just that character | the cursor already sits at the insertion point, no movement needed |
| insert one character mid-line | `ESC[K` → new character + tail → `ESC[nD` moving back n = characters right of the cursor | clear the tail, redraw, then leave the cursor after the new character |
| backspace / Delete / Ctrl+U / Ctrl+K / Ctrl+W | the respective clear + redraw | move the cursor left to the deletion point first, then redraw |

**Never send `ESC[D` when appending at end of line.** The cursor is already at the insertion
point; moving left one cell lands on the previously printed character, and the following
`ESC[K` erases it — the symptom is "type one character, the previous one disappears", leaving
only the last character on screen while the buffer is intact (the command still runs correctly).
This is the root cause of "incomplete display" seen on real terminals such as MobaXterm and
PuTTY; it has nothing to do with the terminal.

**Enter: CR executes, an immediately following LF is swallowed.** Some terminals send `CR LF`
per Enter, some only `CR`, some only `LF`. All three must work, and one Enter must execute
exactly once: `CR` alone or `LF` alone executes normally; with `CRLF`, `CR` executes and the
`LF` is recognised as the same Enter and discarded. Otherwise the second execution is an empty
line, an extra prompt appears on screen and an empty entry lands in history.

Self-test (the PC simulator suffices): capture the echoed bytes and render them as VT100; after
typing `ls` the screen must show `ark> ls`, not `ark> s`; after `echo hi` and Enter the prompt
must appear only once.

### 3.3 Which file implements which command

Commands are split by family under `kernelsrc/shell/`. `arkrtos_shell.c` keeps only the console
body, cross-family helpers and the **single** command registration point; every other file owns
one family:

| File | Commands |
|---|---|
| `arkrtos_shell.c` | console task, `register_kernel_commands()`, `sh_out` / `parse_u32` and other cross-family helpers |
| `arkrtos_shell_image.c` | `info` / `app` / `drv` / `install` |
| `arkrtos_shell_diag.c` | `task` / `stack` / `sched` / `audit` / `guard` / `crash` / `fault` / `log` / `syncinfo` / `rt` / `sec` / `utest` |
| `arkrtos_shell_cfg.c` | `pool` / `cfg` |
| `arkrtos_shell_fs.c` | `blk` / `fs` / `pwd` / `cd` / `df` / `mount` / `umount` / `ls` / `cat` / `rm` / `mkdir` / `rmdir` / `mv` / `cp` / `touch` / `stat` / `chmod` / `chown` |
| `arkrtos_shell_mod.c` | `exec` / `wait` / `ld` / `lkup` / `modules` / `unld` |
| `arkrtos_shell_pmap.c` | `pmap` |
| `arkrtos_shell_misc.c` | `history` / `uname` / `free` / `ps` / `which` / `man` / `dmesg` / `hwsafe` |
| `arkrtos_shell_ushell.c` | App-side command registry and the SVC 0x1A trampoline (registers no concrete command) |
| `arkrtos_shell_internal.h` | the **only** internal header these files may share; includable only by `arkrtos_shell*.c` |

To add a command: find its family file, implement the function, then register it **only once**
in `register_kernel_commands()` in `arkrtos_shell.c`. The command table is a fixed array with a
single registration point, and overflow is a silent out-of-bounds write — so let the `need`
budget report an error rather than letting every file register its own copy.

## 4. Typical operations

### 4.1 Controlling an App (no autostart, exit anytime)

```
ark@arkrtos:~$ app list
id  type  state     auto  task  crash  base        size    ram         entry
 0   app   RUNNING   yes   4     0      0x08060000  131072  0x20014000  0x08060001
ark@arkrtos:~$ app stop 0
app: stopped (image kept in flash)
ark@arkrtos:~$ app list
id  type  state     auto  task  crash  base        size    ram         entry
 0   app   RAW       yes   0     0      0x08060000  131072  0x20014000  0x08060001
ark@arkrtos:~$ app start 0
app: started
```

Column meanings: `ram` is that slot's RAM window base, and bit 0 of `entry` is the Thumb bit.

`stop` removes the task from scheduling and zeroes `task`, while the image **stays in Flash**,
so start / stop is repeatable. The resulting state depends on what the slot holds:

- an installed image → `LOADED` (still treated as a relocatable image at the next compaction);
- a bare-flashed development image → `RAW` (not relocatable, not part of reclamation, see §5).

### 4.2 Boot autostart (system-service style)

```bash
# carry the autostart bit at packaging time (it is on by default; writing it is clearer)
python tools/pack_app.py \
       --project example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx \
       --type app --name APP_DEMO --out build/APP_DEMO/APP_DEMO.svcapp --autostart
# to make it "wait until asked" instead:
python tools/pack_app.py --project <app.uvprojx> --type app --out xxx.svcapp --no-autostart
```

`--info` shows the autostart state directly:

```
$ python tools/pack_app.py --info app_demo.svcapp
flags : 0x00000001（开机自启）
```

### 4.3 Serial installation (console window)

1. Connect a serial terminal to `ttyS0` (115200; PA9/PA10 on this board) and press Enter until
   `ark@arkrtos:~$ ` appears;
2. type `install`; the console prints `install: send the file now`;
3. the host sends the `.svcapp` verbatim (`tools/send_image.py` and similar);
4. the console prints `install: ok, slot 0`; on failure or timeout it prints a message and
   leaves an `INSTALLFAIL` record in `fault`.

> The console does not process input during the window (read rights have gone to the installer);
> the serial link is half-duplex, so this is normal.

### 4.4 Device-side layout configuration (`cfg`)

```
ark@arkrtos:~$ cfg show
... effective policy, slot table, runtime parameters ...
ark@arkrtos:~$ cfg load        # enter receive state, waiting for the host to send the 512-byte record
ark@arkrtos:~$ cfg clear       # erase the config sector, back to compile-time defaults
```

What can be changed, the format, and how a host writes the record: see
[配置区与安装策略.md](配置区与安装策略.md). Two points matter here:

- `cfg load` **receives and writes**: it prints a ready line, waits for 4×128 bytes and writes
  into the CONFIG sector only after validation passes. Typing it manually only waits until
  timeout (`timed out waiting for the record`) — the write is driven by
  `tools/arkrtos_cfg.py write` or the GUI, with the protocol in that page's §6.3;
- policy fields such as `mode` / slot table / reclaim aggressiveness **take full effect only
  after a reboot by default** (pool scan and image start already ran early during boot with the
  then-current layout), so reboot as prompted after writing;
- when the config sector is erased or entirely invalid, the system silently falls back to the
  compile-time default layout and still boots;
- when validation fails (CRC / hardware signature / mode / overlapping slots, …) the existing
  configuration is kept and an error is reported — never applied half-broken.

### 4.5 Image pool free space (`pool`)

```
ark@arkrtos:~$ pool
pool: free 641592 B (largest run 510504 B), tasks 5/48
```

`free` is the number of `0xFF` bytes in Flash, including the unwritten tails of each image slot;
`largest` is the "biggest image that fits right now". Judging whether something fits requires
both.

### 4.6 Small programs: run from the filesystem, no slot (`mini`)

A small program is not installed and takes no partition or slot — it is just a `.svcapp` file
in a volume, loaded into memory by `run`; on exit its code and RAM blocks return to the pool
immediately. The full model is in [小程序设计.md](小程序设计.md).

```
ark@arkrtos:~$ mini autostart add /mini_demo.app     # register for boot (writes /mini.autostart)
will start at boot
ark@arkrtos:~$ mini autostart                        # list: also reports "missing in volume" and "disabled"
[1] /mini_demo.app  1308 bytes
1 listed, 0 missing in the volume, 0 disabled
ark@arkrtos:~$ mini run /mini_demo.app               # run one now
ark@arkrtos:~$ mini list                             # running / registered instances
ark@arkrtos:~$ mini autostart del /mini_demo.app     # cancel autostart (list file deleted when it empties)
will not start at boot
```

A few rules:

- **The autostart list is a ledger, not a switch**: `add` only writes the list; boot tries each
  entry and **never starts one disabled by crashes**, logging `rc=-19`
  (`ARKRTOS_MINI_ERR_DISABLED`) instead of pretending to start it.
- **Concurrency is configurable**: `mini count [n]` reads or changes the runtime limit (it can
  only tighten, never exceed the compile-time `ARKRTOS_MINI_MAX`); when it does not fit it
  reports `NOSPACE` honestly and never evicts a running instance.
- **Repeated crashes disable it**: the same counter as slot applications, journaled in the
  UNINIT area across resets; `mini forget <path>` clears it.
- **`crash` renders the mini accounting area too** (the journal stores only a hash for the path;
  display back-fills from the autostart list, and on mismatch prints `#<hash>` rather than
  guessing a plausible path).

## 5. Relation to other paths

| Path | Note |
|---|---|
| Bare images flashed at fixed addresses | development bypass, **kept**; a bare image has no header, so autostart falls back to the global `APP_AUTO_START` / `DRIVER_AUTO_START` switches |
| Breakpoint debugging under MDK | **kept**; the whole device freezes when the debugger halts, so an unresponsive console is normal then |
| `.svcapp` serial installation | §4.3 here; the per-slot autostart flag travels with the image |
| `.svcapp` written by a programmer | path C in `docs/Ark RTOS应用安装与调试指南.md` |

## 6. Known boundaries

1. **PC2 is shared**: the kernel's `"led2"` (blue LED) and the user-mode driver `BLED_DRV`
   registering `"BLED"` are the same physical LED; running both overwrites each other's display,
   so enable only one in a demo.
2. `reboot` resets the whole device (`AIRCR.SYSRESETREQ`), it is not a soft task restart.
3. The history buffer was reduced from the upstream 32 entries to 8 (`ark_shell_config.h`) to
   make room in kernel RAM.
4. Unknown or multi-byte input passes through byte by byte; Chinese input is not echoed
   correctly.

## Log format (dmesg style)

Each kernel log line looks like:

```
[    1.234] <6>VFS:347 namespace up: / (ramfs) with /tmp /etc /home /mnt /media, /dev (devfs)
```

- `[  sec.msec]`: time since boot;
- `<n>`: syslog priority (`<3>` err, `<4>` warn, `<6>` info, `<7>` debug);
- `tag:line message`: `tag` is the module name and `line` is **the source line that produced the
  log**, so the site can be located directly.

The runtime level is still changed with `log [0..4]`; **historical logs** are printed with
`dmesg` (a log ring, on by default, capacity configurable), and `dmesg -c` clears after
printing.

## Log output latency constraint (SVC 0x19)

Log transmission happens inside the SVC exception handler, so it must **return fast**: if it
returns too slowly, PendSV cannot complete the switch and every task (including the shell) is
starved.

- Each byte is retried at most `ARKRTOS_LOG_TX_SPIN_LIMIT` (currently 64) times; if it still
  cannot be written into the board transmit FIFO, **that byte is dropped** and the next one is
  tried.
- Why that value: a single byte takes at most ~64 retries (on the order of microseconds), so one
  log line holds the handler for a sub-millisecond amount; a value like `200000` would park a
  line in handler mode for seconds, which must be avoided.
- Symptom mapping: on "serial unresponsive for a long time + CPU reported at 100% + stuck in the
  transmit loop of `arkrtos_log.c`", first check whether this budget was enlarged.

## Uninstall semantics and the granularity of space reclamation

`app uninstall <slot>` / `drv uninstall <slot>` do two things: **invalidate** the image in Flash,
then **try** to reclaim the space it occupies.

**Invalidation is written into the `type` field (cleared to 0), not `state`.** `state` can only
express install/move commit points: installation writes UNCOMMITTED (=1) first and rewrites it
to VALID (=0) on commit, both legal 1→0 programming steps. Once an image is committed, `state`
is already 0, and writing it back to 1 would require programming 0 to 1 — Flash can only clear 1
to 0, so the hardware silently keeps 0, uninstall becomes a no-op, and the image is scanned back
in at the next power-up (this is the cause of "it comes back after a power cycle"). `type = 0` is
not a known image type, so `check_header()` rejects it and the boot scan discards it;
`magic` is deliberately kept so reclamation can still use "this range has header content".

**Reclamation granularity is the pool's physical unit (128K on F427).** If even one live image
sits in a unit, the unit cannot be erased; and an image already at the pool bottom cannot move
further down, so as long as "someone else is in the unit", the space stays pinned. The command
distinguishes the two outcomes honestly:

```
drv: uninstalled (image invalidated, its sector was erased)                        # unit empty, sector really erased
drv: uninstalled (image invalidated; space returns once its sector is free)        # invalidated only, space still pinned
```

Measured (`build/BLED_DRV` + `build/BLED_APP`, four images packed into unit 0): stopping and
uninstalling the first three only invalidated and `pool free` did not move; uninstalling the
last one left no live slot in the unit, the sector was erased and `pool free` went from
776244 B back to 786432 B (the whole unit `0xFF`).

`pool`'s `free` is "bytes in Flash that are `0xFF`", so it includes the unwritten tail of each
image slot (1 KB alignment minus the actual length). It is **physical free space**, not "how much
the next install can get"; judging fit requires `largest` as well.

## Filesystem (`fs`)

The volume lives on the **on-board NOR** (W25Q128, 16 MB): it starts at offset 0 and its size
comes from `ARKRTOS_FS_SIZE` (4 MB on F427 today). A successful mount prints one line:

```
fs: mounted nor0 at 0x0, 4194304 bytes
```

**Mounts do not survive a reset.** After reset the volume is unmounted and `fs mount` must be
run again. App code must not call the strict mount itself; use SVC 0x1C subcommand 6 (idempotent
"ensure mounted"), for the reason in [同步原语与令牌守恒.md](同步原语与令牌守恒.md) §6.

Mapping between the 11 `fs` subcommands and SVC 0x1C:

| shell | SVC 0x1C subcommand | Semantics |
|---|---|---|
| `fs wr` / `fs rd` | 1 / 2 | create (truncate) and write / read into the caller's buffer |
| `fs rm` | 3 | delete |
| `fs info` | 4 / 5 | single-file status / volume capacity |
| `fs mount` | 6 | **idempotent** "ensure the default volume is mounted": already mounted answers 0 without calling `arkrtos_fs_mount()` again |
| `fs unmount` | 7 | unmount |
| (no shell verb) | 8 | read a slice at an offset: path, buf, len, off — for Apps / hosts, the shell does not expose it |
| (no shell verb) | 9 | rename: old, new — likewise open only to Apps / hosts |
| `fs ls` | 10 | list directory names into the caller's buffer (dir, buf, size, count out) |

`fs format` / `fs put` / `fs test` / `fs err` map to none of the above: format goes through
`arkrtos_fs_format()` (volume-level format, auto-mounts afterwards, **every file on the volume
is lost**); put receives one file from the console; test is the built-in self-test sequence;
err prints error codes by name.

The `fs ls` buffer format is **`\n`-separated with one trailing NUL** (not a NUL-separated array
of names). Parsers split on `\n`; entries that do not fit are not counted in the returned count.

A typical loop — copy an image into the volume, then let the kernel install it in place:

```bash
py -3 tools/fs_put.py build/APP_DEMO/APP_DEMO.svcapp --port COM3 --path /APP_DEMO.svcapp
# serial:
app stop 4
app uninstall 4        # in LOADED state this answers "raw images are not managed here", so stop first
app install /APP_DEMO.svcapp
app start 4
```

## Crash journal (`crash`)

A crash ledger surviving resets, stored in the UNINIT area at the tail of shared RAM (cleared
only by power loss, survives resets). Field meanings, decision rules and "what it does not do"
are in [崩溃恢复与镜像版本把关.md](崩溃恢复与镜像版本把关.md); here only how to read the output:

```
journal: 0x2000BE00 +512 bytes (uninitialized RAM: kept across reset, cleared by power-on)
boots=35 seq=100

slot type  faults last        hold      held_at
 1   app    3      fault       fault     12840
```

- `boots >= 2` proves the previous content really survived one reset;
- `faults` is the **consecutive** fault count, cleared when an image is reinstalled;
- a non-empty `hold` means that slot is disabled; `app start <slot>` then goes through a manual
  release path and it will not start by itself again.

Companion commands: `guard` for heartbeat contracts, `audit` for structural self-audit, `fault`
for this power-up's fault ring.

## Synchronisation health check (`syncinfo`)

```
sem  used  count  owner  creator  waiters
 0   1     3      -1     1         t4
  sem 0: posts=12 takes=9 queued=1 last_waiter=t4 qbefore=2 woke=t4 reason=4
sync diagnostics: ghost=0 dup=940
sync ghost  : n=0 idx=-1 tid=t0
sync appwait: hit=480
```

Criteria (derivation in [同步原语与令牌守恒.md](同步原语与令牌守恒.md)):

| What to look at | Healthy | Unhealthy |
|---|---|---|
| `ghost` | `0` | anything non-zero: a wake without `ARKRTOS_WAKE_HANDOFF` treated as success, i.e. token conservation broken |
| `dup` | a **normal quantity** under the polling model (every poll hits "already registered" once) | only meaningful together with a non-zero `ghost` |
| `count` and `waiters` | the token is either in `count` or in some ready task's hands | both non-empty and inconsistent means drift |
| `reason` | 4 = HANDOFF, the dispatcher explicitly claimed this task | treating `reason == 0` as success misses failures |
| `appwait hit` | the number of user-mode registrations, i.e. how often the polling path ran | constantly 0 means the App side never polled — check whether it was waiting at all |

The two related counters are in `log`: `tx_drop` is the console's real lost-byte count, while
`fifo_full_retries` is the number of rejected **attempts**, not losses; non-zero `bad_header` /
`bytes_lost` means the FIFO buffer header was overwritten and output on that path was silently
dropped.

## Task stack usage (`stack`)

The watermark switch is `ARKRTOS_USE_STACK_USAGE` (default 1). At task creation the stack is
filled from index 1 to `words-1` with the pattern `0xCDCDCDCD`; the scan counts consecutive
untouched pattern words upward from index 1 to get free, and `used = total - free`.

`stack` (no arguments) and `stack check` print the same table:

```
id  total    used     free     pct    entry
 1   1024     304      720      29%    0x0800A1C1
 4   49152    30668    18484    62%    0x08013E5D
stack: 2 task(s), worst 62% (task 4), 0 unmeasured, 0 at/over 90%  (accounting on)
```

- `used` takes the larger of the **fill watermark** and the **lowest PSP**: the watermark is the
  true peak and cannot underestimate, while PSP is sampled only on context switches and can
  underestimate, so a small PSP alone does not mean a shallow stack (see `raw` below for how the
  two legs reconcile);
- each task is judged in place: `pct >= 90%` prints `^ task N is at X% - raise its stack size`,
  `>= 75%` prints `^ task N is at X% - watch it`;
- `stack check` appends a verdict line and **decides the return code**: `stack: check ok - ...`
  (0) or `stack: check FAILED - ...` (1). A condition that cannot be measured
  (`unmeasured != 0`) also fails — unmeasurable is not a pass.

`stack raw` prints the two legs separately plus one bottom-sample line:

```
id  total    used     fill     psp      pct_f  pct_p  entry
 4   49152    30668    30668    9732      62%    19%   0x08013E5D
      bottom 0x2400EA28  [1/8]=0xCDCDCDCD [4/8]=0xCDCDCDCD [7/8]=0xCDCDCDCD
```

| Column | Meaning | Boundary |
|---|---|---|
| `fill` | peak from the watermark scan, never underestimates | if the task really used that much, it always shows |
| `psp` | lowest SP sampled on context switches | a lower bound only |
| `bottom ...` | **actual memory values** at 1/8, 4/8, 7/8 up from the stack bottom | all `0xCDCDCDCD` = never written there; any other value = that location was written, and `pct_f` is then usually already high |

The point of this sample is that "the scan says it was used" and "what is actually written in
memory" must be cross-checkable; otherwise you cannot tell a real stack use from a scan walking
the wrong addresses.

Three non-numeric exits, **none of which means usage 0**:

| Output | Meaning |
|---|---|
| `n/a (no stack)` / a whole row of `n/a` | this task has no registered stack range, so it cannot be measured |
| `used` looks normal but `pct` is `unmeas` and `free` is `0?` | the read gave `used == 0`. Zero bytes is impossible, so the fill pattern did not take effect (the only detectable form of "switch present, feature inert") |
| `stack: accounting is compiled out (ARKRTOS_USE_STACK_USAGE=0) - no numbers to show` | the whole accounting block is compiled out. The command still exists (still visible in help) but invents no numbers |

### A real case: the stack was truly driven to the bottom

Running `cd /mnt/nor/va && make` on the board (arkmake driving arkc through repeated
`cc --pack`) drove the shell task stack through the floor: 40 KB measured 40 132 B (97.9%),
56 KB measured 56 548 B (98%), 72 KB measured 73 728 B (100%) — **raising the stack did not help,
usage grew with it**.
A hardware write watchpoint on the word below the stack bottom hit with `reg psp = 0x2400E9E8`,
64 bytes below the bottom `0x2400EA28`; the bottom dump showed arkc lexical data (a double
constant and `ct` / `s3` fragments), confirming a real overflow rather than a watermark
misjudgement.

The root cause was not the RTOS: `process()` in `arkc/src/preproc.c` is recursive (macro rescan,
depth limit 64), and each level used to carry a `char temp[16384]`, so four levels meant 64 KB of
stack. The fix moved the buffer from the stack into a pool — cached per `process()` **call slot**
(never shared per macro depth, otherwise nested rescan overwrites each other and measurement
showed `XSTR(MULTI(1,2))` expanding to `1 + 1 + 1`) — and turned "macro expansion too long" from
a silent truncation into a hard error.

After the fix, measured on the same board: boot 2 300 B, `cc -E main.c` 11 236 B,
`cc -c main.c` 17 884 B, `make` 30 668 B; the stack was therefore set to 48 KB (62.4%, 37.6% left).

Lesson: when `stack` reports 100%, do not doubt the watermark logic first — the fill pattern
being overwritten from index 1 upward can only happen if SP really reached the bottom. Such
defects are invisible until it runs, so the gate must really run and read the numbers.
