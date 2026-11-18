# VFS: a Linux-style path namespace

**[简体中文](VFS路径命名空间.md) | English**

> Condensed English edition. The Chinese page (VFS路径命名空间.md) is normative: every number
> below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This
> page adds no new claim and no new measurement.
>
> Corresponds to commit `f77b1b7`. Verification is stated in three grades: compiles / host tests
> pass / verified on board.

## Why this layer exists

Before it, the kernel had only two kinds of "name":

| Name | Entry point | What it can do |
|---|---|---|
| device name | `arkrtos_dev_open("ttyS0")` | open a device |
| volume path | `arkrtos_fs_open_write("/a.txt")` | read/write files in littlefs |

The two were disjoint: a device is not a path and a path could not reach a device. To "open this
path", an App had to know in advance whether it was a device or a file and then take two different
APIs.

This version mounts [ark_vfs] onto the kernel so every resource lands in one tree:

```
/            volatile root (ramfs, lost on power-off; temporary data; contains /tmp /etc /home /mnt /media)
/dev         mirror of the device registry; /dev/ttyS0 can be opened directly by path
/tmp         temporary data (an ordinary directory on the root ramfs, not a separate mount)
/etc         configuration (hostname etc.)
/home        user directory
/mnt/<name>  persistent volume (littlefs), explicitly mounted by the upper layer
/media       removable-media mount point (reserved)
```

Path semantics follow Linux: `//` collapses, `.` is dropped, `..` is consumed by the preceding
segment, and a `..` that walks above the root is an error (`ARK_E_LOOP`) rather than silently
folding back to the root. Mount points use longest-prefix matching, so `/mnt/nor` shadows the
corresponding stretch of `/`.

## Composition

| File | Role |
|---|---|
| `kernelsrc/components/ark_vfs/` | vendored component (upstream: the ark_vfs repo; source commit recorded once in that directory's `VENDOR.md` so it never goes stale here) |
| `kernelsrc/src/arkrtos_vfs.c` | facade: mounts `/` and `/dev`, mounts/umounts volumes, read/write/remove/list |
| `kernelsrc/src/arkrtos_vfs_lfs.c` | littlefs bridge: wires `arkrtos_fs` in as the ark_vfs fsdrv `"lfs"` |
| `kernelsrc/src/arkrtos_dev.c` | adds `arkrtos_dev_name_at()`: how devfs enumerates the device table |
| `kernelsrc/include/arkrtos_vfs.h` | the kernel's only public surface (shell and Apps see only this) |

**Why the kernel does not include `ark_vfs.h` directly**: ark_vfs is an external component that
versions independently. The kernel uses it only through this one layer; if ark_vfs changes its API
later, what changes is this layer, not the shell and not the Apps.

## Facade API

```c
int32 arkrtos_vfs_init(void);                     /* idempotent; mounts / and /dev */
uint8 arkrtos_vfs_ready(void);
int32 arkrtos_vfs_mount_volume(dev, off, size, target);   /* mount a persistent volume */
int32 arkrtos_vfs_umount(target);
int32 arkrtos_vfs_mount_info(idx, out);
int32 arkrtos_vfs_mount_count(void);
int32 arkrtos_vfs_read_file(path, buf, max, out_len);     /* read a whole file */
int32 arkrtos_vfs_write_file(path, buf, len);             /* write a whole file */
int32 arkrtos_vfs_open_read(path);                        /* streaming read, three calls */
int32 arkrtos_vfs_read(fd, buf, len);
int32 arkrtos_vfs_close(fd);
int32 arkrtos_vfs_remove(path);
int32 arkrtos_vfs_list(dir, cb, arg);
const char *arkrtos_vfs_error_name(rc);
```

Errors are always ark_vfs negative codes; the symbolic name comes from
`arkrtos_vfs_error_name()` as an addition — **the number is always kept in reports**, the symbol is
only commentary.

## "Who knows what" is the mount-point question

The kernel knows the **device registry** (hence it owns `/dev`), but it does not know **which
offset on which device holds a file system** — that is board-layout knowledge. Therefore:

- `/` gets a hardware-independent volatile root (ramfs; temporary data goes to `/tmp`);
- persistent volumes get an explicit entry `arkrtos_vfs_mount_volume()`, ordered by board code or
  the shell.

Mounting **never formats**. An unformatted volume returns `ARK_E_IO`; it will not quietly wipe
somebody else's data — formatting is destructive and needs an explicit order.

## Boot ordering

`example/*/Core/Src/main.c` brings the namespace up **before starting any image**, because that is
the whole point of a namespace: an App should be able to `open("/dev/uart0")` from its first line
instead of waiting for somebody to remember to initialize it. Failure is not fatal (the console and
images are still there) but it is logged, and the error name tells you which layer refused.

## Shell commands

| Command | Meaning |
|---|---|
| `mount` | list mount points |
| `mount <target>` | mount the **on-board default volume** (`ARKRTOS_FS_DEV_NAME/BASE/SIZE`) at `<target>` |
| `mount <dev> <off> <size> <target>` | mount a window on `<dev>` (`size 0` = to the end of the device) |
| `umount <target>` | unmount one mount point |
| `ls [path]` | list a directory, `/` by default |
| `cat <path>` | print a file (non-printable bytes escaped as `\xNN`) |
| `rm <path>` | remove a file |

Division of labour with the existing `fs` commands: `fs` manages **one volume** (mount / format /
read and write inside it), while this set walks the **namespace**. The names follow Linux —
`ls` / `cat` / `mount` already mean what everybody expects.

### `size 0` is not "the board's volume"

`size 0` literally means **to the end of the device**. This is a 16 MiB NOR, so
`mount nor0 0 0 /mnt/nor` means "from 0 to 16 MiB", while the on-board default volume is 4 MiB
(`ARKRTOS_FS_SIZE`). The two windows are not the same; littlefs reads the on-disk superblock, finds
a mismatched `block_count`, and returns `LFS_ERR_INVAL(-22)`.

To mount "the volume on the board", write `mount /mnt/nor` — do not assemble a window with four
arguments. The four-argument form is for **other windows** (partitions, a second extent), and then
you are responsible for the window width; a wrong width is not a silent downgrade but an immediate
error, with both the requested window and littlefs's raw code in the log:

```
[E][vfs:250] arkrtos_fs_mount(nor0, off=0, size=16777216) failed: last_error=-22 (LFS_ERR_INVAL)
mount: FAILED (-11: backend error)
```

### When a volume is already mounted, the window must match

`arkrtos_fs` has exactly one volume. If it already holds a volume (e.g. an App brought it up through
an SVC at boot) and the requested `dev/off/size` is **not the same window**, the request is
**refused** (`ARK_E_EXIST`) instead of silently reusing it:

```
[E][vfs:266] a different volume is already mounted: nor0 off=0 size=4194304 (asked for nor0 off=0 size=16777216)
mount: FAILED (-7: already exists)
```

Matching windows are reused (the normal path where the App and the shell share one volume). Why
guard this: silently reusing would drop `dev/off/size` entirely, so the same `mount` line would
yield **different geometry** depending on whether a volume was already mounted, while both states
look identical on the command line — the hardest kind of wrong answer to diagnose.

## Feature switches

`kernelsrc/include/arkrtos_features.h`:

| Switch | Default | Effect |
|---|---|---|
| `ARKRTOS_USE_VFS` | 1 | facade + ark_vfs component + the 5 shell commands |
| `ARKRTOS_USE_VFS_LFS` | 1 | littlefs bridge (persistent volumes under `/mnt`) |

Dependencies are hard-wired as `#error`: `ARKRTOS_USE_VFS` needs `ARKRTOS_USE_BLK`,
`ARKRTOS_USE_VFS_LFS` needs `ARKRTOS_USE_FS`.

**A feature switched off is not a fault**, and the two must stay distinguishable:

- the two port `.c` files compile to empty translation units (zero symbols in `nm`);
- every facade entry returns `ARKRTOS_VFS_ENOSYS` (same value as `ARK_E_NOSYS`, pinned by static
  assertion) instead of letting callers hit an undefined symbol at link time;
- the bring-up block in `main.c` is wrapped in `#if` — otherwise a VFS-off build prints
  "arkrtos_vfs_init failed: -12" on every boot, reporting "off by configuration" as "boot failure".

## Verification status

**Compiles** (host `armclang -fsyntax-only` with zero warnings + full Keil build):

| Board | Result | Code |
|---|---|---|
| F427 | 0 Error / 1 Warning (the only warning is the baseline `arkrtos_context.S` A1581W) | 92206 → **103070** |
| F401 | 0 Error / 1 Warning (same warning) | 86222 → — |

> **Correction 2026-11-12**: the `Code 86222` above for F401 is the reading from the build when VFS
> landed and must not be taken as the current value. Afterwards the F401 project did **not include**
> `arkrtos_mempool.c` / `arkrtos_utest.c` / `arkrtos_utest_cases.c` (F427 did); earlier paths never
> exposed it, until the full build on 2026-11-12 hit it. After adding the three sources plus
> `MDK_TRACE_SWD_BYTES` 8192→4096 and `-DARKRTOS_USE_UTEST=0 -DARKRTOS_USE_PROCFS=0` (Flash needed
> 139,948 B > 131,072 B, short by 8,876 B) the re-check reads:
> `Code=116006 RO-data=13034 RW-data=476 ZI-data=53472`, **0 Error / 1 Warning** (still the
> `arkrtos_context.S` A1581W padding); ROM headroom 1,556 B, RAM headroom 3,396 B.
> Grading: **compiles only (full Keil link), F401 not verified on board.**

Trimmed configurations also compile: `-DARKRTOS_USE_VFS_LFS=0`,
`-DARKRTOS_USE_VFS=0 -DARKRTOS_USE_VFS_LFS=0`.

**Host tests pass** (in the ark_vfs repo): 161 core checks + 70 Ark RTOS port checks = 231, all
green, clean under `-Wall -Wextra -Werror`.

**Verified on board** (F427, USART1/COM3, firmware `Code=103070`):

| What is verified | Device echo verbatim |
|---|---|
| namespace | `mount` → `/dev`(devfs) + `/`(ramfs) + `/mnt/nor`(lfs, rw) |
| `/dev` enumeration | `ls /dev` → led0 / led1 / led2 / ttyS0 / BLED |
| persistent mount | `mount /mnt/nor` → `[I][vfs:291] volume nor0 mounted at /mnt/nor` |
| list inside the volume | `ls /mnt/nor` → 9 entries (including `keep.txt` 15 B) |
| read inside the volume | `cat /mnt/nor/keep.txt` → `persistent-data` / `15 bytes` |
| create (volume root) | `touch /mnt/nor/c3gate.txt` → `c3gate.txt 0` appears in `ls /mnt/nor` |
| create (subdirectory) | `touch /mnt/nor/va/c3gate.txt` → a 0 B file appears in the subdirectory too (same volume, not "root-only") |
| missing parent | `touch /mnt/nor/c3nodir/x.txt` → `-1: no such file or mount`, log says `parent directory missing`; **not** `-11: backend error` |
| oversized window refused | `mount nor0 0 0 /mnt/nor` → `-11: backend error`, log carries `size=16777216` |
| window mismatch when mounted | `mount nor0 0 0 /mnt/nor` → `-7: already exists`, log states both windows |
| error paths | `ls\|cat\|rm /mnt/nor/nosuch` → `-1: no such file or mount` |
| directory read as file | `cat /` → `-5: is a directory` |
| character device read | `cat /dev/LED` → `-11: backend error` (driver refused, not EOF) |
| unmount | `umount /mnt/nor` → success, `mount` back to two lines |

### When a create fails, the cause must not collapse into backend error

The `O_CREAT` path has two steps that can fail, and their causes must be reported separately:

| Failure point | Real cause | What should be reported |
|---|---|---|
| `stat` says "no such entry" | this is the **normal** precondition of a create, not an error | continue into the create |
| `open_write` returns `LFS_ERR_NOENT` | parent directory does not exist | `-1: no such file or mount` |
| `open_write` returns anything else | a genuine driver/space problem | `-11: backend error` + raw code in the log |

The second row used to travel with other errors as `ARK_E_IO`, so `/mnt/nor/nodir/x.txt` was
reported as "backend error" — calling "path does not exist" a "driver fault", which sends people
looking at the QSPI driver. The opposite mistake is equally wrong: treating every failure as NOENT
would report a real I/O fault as a bad path.

Half of the same contract lives on the `arkrtos_fs` side: every failure return of
`fs_stream_open()` must write `last_error`, otherwise the bridge reads the previous stale code, and
deciding on a stale code is guessing.

**Verified on board (concurrency)**: item `10c)` of the self-test in
`example/stm32f427/app_sdk/APP_DEMO` pushes three tasks onto one path — two writers (this task
writing 32 B of `'A'`, a pthread writing 48 B of `'B'`, each doing 40 rounds of "write then read back
and verify") plus a third, read-only reader. The criterion is that the reader must see both
**complete** payloads (`vfs.conc_saw_both`) — seeing only one means the two writers were in fact
serialized, and the other assertions do not constitute evidence. Two clean boots on F427:
`pass=128 fail=0`, all six `vfs.conc_*` items OK, with `sched: consistent` and
`fault: no fault recorded`.

Sizing note: the POSIX layer's pthread pool has only `ARKRTOS_POSIX_THREAD_MAX` (default 2) slots,
so in "two writers + one reader" one block must be played by the App main task; opening all three
as pthreads silently loses one (the third `pthread_create` fails and the reader never runs).

## Known limits (stated plainly, not silent downgrades)

1. `arkrtos_fs` allows only one write stream per volume at a time. So a second write-open under
   `/mnt` gets `ARK_E_BUSY`, and reads also get `BUSY` while a write stream is running — reads and
   writes share the same file cache.
2. One kernel mounts only one persistent volume (`g_vol_target` is a single slot); that is an
   `arkrtos_fs` limit, not one added by this layer.
3. `arkrtos_vfs_open_read()` only admits read-only opens. Writes all go through
   `arkrtos_vfs_write_file()`; adding a half-finished flag set would only make people believe those
   combinations had been tested.
4. The read-only mount (`ro`) field already exists in `mount_info`, but the facade has no
   "mount read-only" entry yet.
5. The window in `mount <dev> <off> <size> <target>` is the caller's responsibility: a window larger
   than the on-disk volume (common with `size 0`) is rejected by littlefs rather than shrunk to the
   real size.

## Upstream documentation (ark_vfs repo)

The component's own design and usage are not in this file; see the three upstream documents (the
repo is open: `https://gitee.com/xw19981010/ark_vfs`, locally `D:/工作/git_project/ark_vfs`):

| Document | Contents |
|---|---|
| `docs/architecture.md` | layering and what each layer can reach, static tables and measured footprint, path and mount resolution, fd table, recursive locks only at the edges, error model, what the core deliberately does not do |
| `docs/fsdrv.md` | the fsdrv contract item by item, what may stay `NULL`, the six rules for returning errors, a complete compilable `romfs` example, backend contract, pre-handover checklist |
| `docs/porting.md` | the three steps limits/backend/hooks + mounting, build recipes for four toolchains, measured size (core + ramfs ≈ 14.5 KiB flash / 5.4 KiB .bss), graded porting-done criteria |

Names such as `ARK_E_*`, `ark_vfs_fsdrv_t` and `ark_vfs_hooks_t` in this file all refer to the
upstream public surface; the kernel side uses it only through `arkrtos_vfs.h`.

## App side: how file-class POSIX maps onto this layer

Done (SVC 0x1C sub 11..18): an App writes ordinary POSIX and a path starting with `/` lands here.
`open/read/write/lseek/close/stat/fstat/opendir/readdir/closedir/unlink` → `arkrtos_path_*`, with
finite capacity (8 fd slots, 2 directory streams) reporting `EMFILE`/`ENOMEM` truthfully when the
table is empty. Capability table and field semantics are in POSIX与Windows兼容层说明.md §4.7; the
test piece is `example/stm32f427/app_sdk/FS_DEMO/` (**verified on board**: F427 fixed slot 0 after an
overwrite install, file-class POSIX self-check run item by item, each `ok`, ending with
`result : pass=32 fail=0`).

## Not done

- App side: socket / select **do not go through this layer** — they hit the kernel socket service
  via SVC `0x1E` (lwIP 2.1.3, loopback interface up), see socket与select兼容层.md and
  lwIP协议栈接入.md.
- Component upgrade: run `tools/sync_ark_vfs.py --apply` and then one full build.

[ark_vfs]: https://gitee.com/xw19981010/ark_vfs
