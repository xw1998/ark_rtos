# Shell and Logging: leaning on Linux habits

> Condensed English edition. The Chinese page (Shell与日志Linux化.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

Goal: someone coming from Linux should **not have to learn a second set** — prompt, paths, command names, output wording and log form all follow Linux.

Verification tiers: **compiles / host tests pass / verified on board**. As of this page the content is **compiles** (AC6 `-fsyntax-only` rc=0); board status is at the end.

## 1. Prompt and working directory

The prompt is full bash shape:

```
ark@arkrtos:<cwd>$
```

* `cwd` is the current directory; when it equals root `/` it shows `~` (same as bash and `$HOME`).
* The prompt appears immediately at boot; the bare `ark> ` is gone.

```
ark@arkrtos:~$ pwd
/
ark@arkrtos:~$ cd /dev
ark@arkrtos:/dev$ ls
BLED  led0  led1  led2  ttyS0
ark@arkrtos:/dev$ cd ..
ark@arkrtos:~$ 
```

**Path resolution**: arguments to `ls` / `cat` / `rm` / `cd` resolve against the current directory and accept absolute paths, relative paths, `.`, `..`, `~`, `~/x`. Normalisation follows Linux (`//` collapsed, `.` dropped, `..` consumes the preceding segment); walking above root is an error rather than silently folding back to root.

`cd` changes directory only when the path **exists and is a directory**; otherwise it reports an error and leaves the current directory unchanged.

## 2. Command surface

### 2.1 Linux-style commands

| Command | Meaning |
|------|------|
| `pwd` | print working directory |
| `cd [dir]` | change working directory |
| `ls [-l] [-a] [path]` | list a directory; `-l` adds type char and size, `-a` appends `.` / `..` |
| `cat <path>` / `rm <path>` | print / delete a file |
| `mount` / `umount` | list and add/remove mount points |
| `df` | mount points and volume usage |
| `ps` | task and image overview |
| `free` | memory overview (kernel RAM window + image pool) |
| `uname [-a]` | system and build info |
| `history` | command history |
| `which <cmd>` | resolve an alias to the real command |
| `man <cmd>` | same as `help <cmd>` |
| `dmesg [-c]` | print the kernel log ring; `-c` clears after printing |

### 2.2 Aliases

| Alias | Real command |
|------|----------|
| `ll` | `ls -l` |
| `la` | `ls -a` |
| `dir` | `ls` |
| `cls` | `clear` |
| `halt` / `poweroff` | `reboot` |

`which ll` answers `ll: alias for 'ls -l'`.

### 2.3 Exit codes and per-command help

* **`$?`**: return code of the last command (Linux convention: 0 = success, non-zero = failure; unknown command 127).
* **`cmd -h` / `cmd --help`**: prints that command's usage, same as `help <cmd>` / `man <cmd>`.
* **Error text** uses Linux wording: `<name>: command not found`, `<cmd>: too many arguments`, `<path>: No such file or directory`.

### 2.4 Honest reporting (nothing invented)

Whatever the kernel does not track is left blank or printed as `-`; it **never guesses a plausible-looking answer**:

* `ls -l` gives only type char + size + name; **no owner / timestamp** fields (VFS does not provide them).
* `ps` has **no TTY / CPU-time** columns (the kernel keeps no such accounting).
* `free` reports the partition-table RAM segment and the image pool; the kernel **has no heap**, so `used` / `free` print `n/a`.
* `df` prints `-` for unknown capacities.

## 3. Logging: dmesg style

Each line looks like:

```
[    1.234] <6>VFS:347 namespace up: / (ramfs) with /tmp /etc /home /mnt /media, /dev (devfs)
```

| Field | Meaning |
|------|------|
| `[  sec.msec]` | time since boot |
| `<n>` | syslog priority: `<3>` err, `<4>` warn, `<6>` info, `<7>` debug |
| `tag:line` | module name + **the source line that produced the log**, for direct location |

* Runtime level is still set with `log [0..4]` (0=off … 4=debug).
* **History** lives in the log ring: `dmesg` prints it (oldest to newest), `dmesg -c` prints then clears.
* The ring is **on by default**, sized by `ARKRTOS_LOG_RING_SIZE` (default 4096 B); when off, `dmesg` reports capacity 0.
* When full it overwrites the oldest entry; a dump delivered in two callbacks **does not guarantee a contiguous whole line**, so read it segment by segment.

## 4. Devices and namespace

Device nodes are Linux-style:

| Old name | New name |
|------|------|
| `COM1` | `ttyS0` |
| `LED` | `led0` |
| `LED2` | `led1` |
| `LED3` | `led2` |
| `nor0` | unchanged (already Linux-style) |

`/dev` mirrors the device registry, so `ls /dev` and `arkrtos_dev_open()` see **the same set of names**.

The root namespace is the standard Linux subset:

```
/            volatile root (ramfs, lost on power-off)
/dev         device registry mirror (devfs)
/tmp         temporary data (an ordinary directory on the root ramfs)
/etc         configuration (hostname etc.)
/home        user directory
/mnt/<name>  persistent volume (littlefs), mounted explicitly by the upper layer
/media       removable media mount point (reserved)
```

> `/tmp` / `/etc` / `/home` / `/mnt` / `/media` are **ordinary directories on the same ramfs**, not separate mount points — which is why `mount` / `df` count them inside `/`, the same as a system with no dedicated `/tmp` mount.

## 5. Configuration switches

| Macro | Default | Meaning |
|----|------|------|
| `ARKRTOS_SHELL_PROMPT_LINUX` | y | 1 = Linux-style prompt; 0 = fall back to a fixed standby prompt |
| `ARKRTOS_USE_LOG_RING` | y | master switch for the log ring |
| `ARKRTOS_LOG_RING_SIZE` | 4096 | ring capacity in bytes, depends on `ARKRTOS_USE_LOG_RING`; **RAM-tight projects can override it in their own Misc Controls**, see below |
| `SHELL_DEV_NAME` | `"ttyS0"` | console serial device name |
| `INSTALLER_DEV_NAME` | `"ttyS0"` | image-receive device name |

### 5.1 Ring-size override on F401

The F401 kernel RAM window is only 56 KB and this round's cwd / new commands / log ring together exceeded the execution region by about 3 KB, so the linker reported `L6406E: No space in execution regions`. The F401 project added, in its own **Misc Controls**:

```
-DARKRTOS_LOG_RING_SIZE=512
```

The generated `arkrtos_autoconf.h` wraps that macro in `#ifndef`, so the command-line override takes effect while F427 keeps the Kconfig default of 4096. After the override F401 links (RAM free ~480 B, Flash free ~14 KB).

## 6. Verification status

* **Compiles**: kernel source files under AC6 `armclang -fsyntax-only` rc=0 (including valid trimmed variants).
* **Verified on board**: still to be added (F427 / COM3 hardware, see 内核Shell控制台使用说明.md §4.3).
