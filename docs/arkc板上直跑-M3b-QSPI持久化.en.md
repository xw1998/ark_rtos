# arkc On-Board Direct Run M3-b: QSPI NOR Persistence

> Condensed English edition. The Chinese page (arkc板上直跑-M3b-QSPI持久化.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

**Status: loop closed on the real board (2026-11-05); the read side got more than 1000x faster the same day (§3, §8.1).**
The artifact of the on-board `cc --pack` lands on the external NOR; **after a reset the files
are still there and `mini run` can still load and execute them**. The first round's read
channel was bit-banged (1.5 KB/s), so `ls /mnt/nor` took 258 s; after switching **reads** to
QUADSPI memory mapping, with byte-by-byte self-proof via bit-bang, the same `ls` takes
0.249 s and on-board `make` drops from 41.95 s to 3.84 s. Writes/erases are still bit-banged
(reasons in §3).

This page only records readings and pitfalls measured on the real board in this round; the
loading rules of M1/M2 are not repeated here (see
[arkc板上直跑-M1实机记录.md](arkc板上直跑-M1实机记录.md) and
[arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)).

---

## 1. The questions this round answered

| Question | Reading | Verdict |
|---|---|---|
| Is the external NOR detected, and how big is it | JEDEC `0xEF4017` (W25Q64), SR1 `0x00`, 8 MiB mapped at `0x90000000` | detected |
| Can it read, write and erase | `nor verify` four steps: erase → all `0xFF` → write a 256 B ramp → byte compare 256/256 | yes |
| Can a volume be created and keep its contents across mounts | `fs format` → `fs mount` → `fs test: PASS` (write 512 B → read back → unmount → remount → read back → delete) | yes |
| Does the volume show up in the VFS tree | `mount /mnt/nor` → `vfs:451 volume nor0 mounted at /mnt/nor` | yes |
| Can on-board `cc --pack` write its artifact into NOR | `cc --pack /mnt/nor/k.c -o /mnt/nor/k.svcm` → `payload 2940 + table 6 x 4 = 3220 B`, `ls` shows 3220 B | yes (24811 ms) |
| **After reset, is the artifact still there and still runnable** | reset → `fs mount` → `mount /mnt/nor` → `ls -l` shows `k.c 447` + `k.svcm 3220` → `mini run /mnt/nor/k.svcm` → prints `NOR: app alive` / `NOR: ctor ok`, the task exits by itself and both blocks go back | **closed** |

Serial output (the acceptance log of this round):

```
[    0.000] <6>NOR:1885 W25Q64 answered: JEDEC 0xEF4017, SR1 0x00, 8 MiB at 0x90000000 (mm)
...
>>> fs mount
mounted nor0 at 0x0, 4194304 bytes
>>> mount /mnt/nor
[    6.913] <6>vfs:451 volume nor0 mounted at /mnt/nor
>>> ls -l /mnt/nor
  -rw-r--r--   1 0     0          447  k.c
  -rw-r--r--   1 0     0         3220  k.svcm
  2 entries
>>> mini run /mnt/nor/k.svcm
---- load+run took 33.7 s ----
[   47.038] <6>MINI:579 /mnt/nor/k.svcm: 2940 B payload -> code 0x24070000 (4096 B block),
           RW/stack 0x24072000 (8192 B), task 4
NOR: app alive
NOR: ctor ok
[   47.039] <6>TASK:2584 task 4 exits (self kill)
[   47.039] <6>MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
```

**Why this counts as closed**: `k.c` was written on the host and pushed in with `fs put`,
while `k.svcm` was produced by the **on-board** compiler; both survive the reset; the two
lines the `.svcm` prints exist only in that source (`NOR: app alive` / `NOR: ctor ok`) and
are not printed by the loader itself.

---

## 2. Pins and AF: the authoritative table is in the CubeMX device library

The schematic gives net names (`QSPI_IO0..3` / `CLK` / `NCS`) and **no AF numbers**. This
round did not guess: the local STM32CubeIDE device library carries a per-pin alternate
function table.

- Source: `D:\ST\STM32CubeIDE_1.16.1\STM32CubeIDE\plugins\com.st.stm32cube.common.mx_6.12.1.202409122256\db\mcu\IP\GPIO-STM32H747_gpio_v1_0_Modes.xml`
  (H743 and H747 share the same GPIO IP)
- Parsing: split by `<GPIO_Pin PortName="PF" Name="PF6">`, take the `<PossibleValue>GPIO_AFn_xxx</PossibleValue>`
  that follows each `<PinSignal Name="...">`

Measured mapping (QSPI1 / W25Q64):

| Pin | Signal | AF |
|---|---|---|
| PF6 | QUADSPI_BK1_IO3 | **9** |
| PF7 | QUADSPI_BK1_IO2 | **9** |
| PF8 | QUADSPI_BK1_IO0 | **10** |
| PF9 | QUADSPI_BK1_IO1 | **10** |
| PF10 | QUADSPI_CLK | **9** |
| PG6 | QUADSPI_BK1_NCS | **10** |

> In this group the two 8/9 pins are **AF10**, not AF9. When the whole group was first
> configured as AF9 the read-back was all `0x00`; the same device library also says PF8 AF9
> = TIM13_CH1 and PF9 AF9 = TIM14_CH1, i.e. those two were never wired to QUADSPI. This was
> the only place this round where a wrong configuration looked like dead hardware.

---

## 3. Data-path trade-off: reads use memory mapping (only after self-proof), writes/erases stay bit-banged

Indirect QUADSPI reads on this board behave like this:

1. The **first address-bearing command after a full reset gets swallowed** (`fifo empty SR=0`,
   the command is never sent);
2. Once a write path fails, the peripheral is left with residual bytes in the FIFO (measured
   `SR=0x124`, `FLEVEL=1`); afterwards `nor id` / `nor read` keep returning `0xFF`, SR1 reads
   `0xFF` (MISO floating), and only a full reset recovers it;
3. The write completion bit sits in `SR` bit 2, while CMSIS's `QUADSPI_SR_TCF` is bit 1
   (RM naming and header disagree), so waiting per the header times out with
   `wr timeout instr=20 SR=0x4`.

The first round (M3-b closure) replaced the **whole data path with bit-bang**: read `0x03`,
page program `0x02`, sector erase `0x20`, status `0x05`, `WREN 0x06`, each step individually
read back and verified. This routes around both pitfalls above.

The cost is stated plainly: bit-bang calls `nor_delay_us(1)` per half clock, and measurement
works out to **about 616 us per byte** (`nor speed` measured 4096 B = 2525280 us) ⇒
**1.5 KB/s**, an order of magnitude slower than the paper estimate of "1 us per half clock"
(one iteration of the `volatile` spin loop in `nor_delay_us()` is about 26 us, not 1 us).
Hence `ls /mnt/nor` over 24 entries takes **257.888 s** and on-board `make` takes 41.95 s.

### 3.1 Round two: reads go memory-mapped, writes/erases untouched

The read side goes back to QUADSPI **memory mapping** (FMODE=11, command `0x03` 1-1-1, one
MM round per read), while **writes/erases stay bit-banged**. The two pitfalls are of
different natures: the write path's problems are "peripheral left dirty after a failure" and
"completion bit misplaced", which bit-bang avoids; the read side may take the fast path as
long as it can **self-prove**.

The self-proof lives in `nor_mm_selftest()`: read 32 bytes at the same address through MM and
through bit-bang, `memcmp` them, **and require that the content is not a run of one value** —
otherwise a fault where `0xFF`/`0x00` means "the bus never moved" would also match and the
proof would be a false green. Candidate windows are the volume head (0, where littlefs
metadata lives) and the first 32 bytes of the last 4 KiB sector; if neither yields
"not all one value" the test fails.

Three gates, all required:

1. **Boot self-test**: if it fails, `read=bit-bang` and the log says so
   (`read channel = mm 0x03 1-1-1 (self-tested)` is the enabled form);
2. **Runtime fallback**: any failed MM read permanently reverts to bit-bang plus a log line —
   no "automatic retry" dressing up an intermittent fault as normal; re-enabling requires an
   explicit `nor mmode 1`;
3. **CCR read-back**: after entering MM, read `CCR.FMODE` back; if it did not take, abort with
   an error code — otherwise when MM is not in effect, reading `0x90000000` yields an
   **AXI bus error (PANIC BUS)** instead of an error code.

Same board, same firmware, switching the channel live with `nor mmode 0|1`:

| Action | Bit-bang | Memory-mapped `0x03` | Factor |
|---|---|---|---|
| `nor speed 4` (read 4096 B) | 2525280 us = **1.5 KB/s** | 1250 us = **3197 KB/s** | **2132 x** |
| `ls /mnt/nor` (24 entries) | **257.888 s** | **0.249 s** | **1036 x** |
| `fs mount` | 2.691 s | 0.031 s | 87 x |
| on-board `make` (2 sources → app.svcm 4776 B) | 41.95 s | **3.84 s** | 10.9 x |

Content identity is not "looks the same": `nor speed` reads the same 4096 B / 8192 B through
both paths and reports `identical` after a byte-wise `memcmp`; `fs test` runs "write 512 B →
read back and compare → remount → read back and compare → delete" and passes; on-board `make`
and `mini run` produce artifacts of the same byte size as in the bit-bang era (payload 4504 +
table 16 = 4776 B) and print the same `ARKMAKE: cross-file add3=6`.

Why `make` is only 11x faster is written here: the bottleneck is now the compiler itself
(`cc: packed in 3763 ms`); **the read side is no longer the bottleneck**.

### 3.2 Why quad read (`0xEB`) is not done

`nor sr2` reads `SR2=0x02` (QE=1), the precondition looks satisfied, but the self-test of
`nor mmode 2` fails every time; sweeping `nor dummy` from 0 to 9, **not one setting passes** —
so this is not a dummy-cycle-count problem.

The real cause is at the pin level: `nor_pins_init()` pins **PF6/PF7 as GPIO push-pull outputs
high** for single-line `/WP` and `/HOLD`, and those two pins were never connected to QUADSPI's
IO2/IO3 (single-line reads do not look at them, so it never showed). Using quad read requires
resolving the role conflict "data line while reading, held high while writing/erasing".

The reason for not doing it is zero benefit: single-line reads are already 3197 KB/s and
`make` time is entirely compiler CPU. The switch `ARKRTOS_NOR_MM_QUAD` (default 0) and
`nor dummy` stay in the code so the next person who wants quad read does not repeat the work.

---

## 4. Pitfalls that only show up on the real board

| Pitfall | Symptom | Root cause | Fix |
|---|---|---|---|
| **/WP and /HOLD must be held high in single-line mode** | after `WREN`, bit WEL in `SR1` never sets and an erase changes nothing | in single (non-quad) mode IO2 = **/WP**, IO3 = **/HOLD**; driven by QUADSPI they are pulled low and the chip enters write protection | configure those two pins as **GPIO push-pull output high** during writes/erases, and bit-bang the write/erase |
| **Quad read never works no matter how it is tuned** | `nor mmode 2` self-test always fails, `nor dummy` 0..9 all fail | not dummy cycles: PF6/PF7 are pinned as GPIO outputs high (single-line /WP, /HOLD) and were never connected to QUADSPI IO2/IO3 | not fixed (§3.2): single-line is already 3197 KB/s, zero benefit |
| **`ADSIZE` was once written as 3** | the address goes out as 4 bytes and the whole command shifts | `ADSIZE` encoding is 0=8 bit / 1=16 bit / 2=24 bit / 3=32 bit | fixed at `2` (24 bit) |
| **A newly created file is judged an I/O error** | `cc --pack ... -o /mnt/nor/k.svcm` reports `cannot write /mnt/nor/k.svcm: I/O error`, log `ark_vfs:101 open/stat failed on /k.svcm: last_error=-2 (LFS_ERR_NOENT)`; the same artifact written to `/tmp` (ramfs) is fine | the VFS littlefs bridge `stat`s before `open` and treats "entry does not exist" as a real error: the criterion was `arkrtos_fs_last_error() != 0`, and the facade records `LFS_ERR_NOENT` into `last_error` exactly when the entry is missing | the facade gains `arkrtos_fs_last_is_noent()` (1 = the last failure was only a missing entry); the bridge now reports IO **only when it is not NOENT**, and NOENT continues down the create branch |

The third one is the **only kernel defect** of this round (not a configuration issue): it only
shows when **creating** a file on a littlefs volume, is never visible on ramfs, and so the M2
test batch stayed all-green and still missed it.

The corresponding change (three files, all necessary):

```
kernelsrc/include/arkrtos_fs.h      + uint8 arkrtos_fs_last_is_noent(void);   (declaration + contract)
kernelsrc/src/arkrtos_fs.c          + implementation (true iff LFS_ERR_NOENT; the ARKRTOS_USE_FS=0 stub returns 0)
kernelsrc/src/arkrtos_vfs_lfs.c     criterion goes from last_error != 0 to last_is_noent() == 0
```

`arkrtos_fs.c` is a CRLF file left over from the GBK era; this time it was a **byte-level
patch** (CRLF count 1269/1269 unchanged); `arkrtos_fs.h` and `arkrtos_vfs_lfs.c` were already
LF/UTF-8.

---

## 5. Block device and volume

- Driver: `board/stm32h743/drvqspi.c` (new), registers block device **`nor0`**, `erase_unit`
  4096, capacity 8 MiB;
- **registered only after the JEDEC answer** (`blk_nor_bringup` returns an error if the ID read
  fails), avoiding "claim 8 MiB then fail every access";
- in the partition header `config/stm32h743/arkrtos_partition.h`, `ARKRTOS_NOR_CHIP_SIZE` goes
  from `16 MiB` to **`8 MiB`** (the old value was copied from the F427 board and does not match
  the W25Q64 fitted here);
- volume: `ARKRTOS_FS_DEV_NAME "nor0"`, `ARKRTOS_FS_BASE 0`, `ARKRTOS_FS_SIZE 4 MiB`.

Mounting takes two steps (order matters; reversed you get `-7: already exists`):

```
fs mount            -> mounted nor0 at 0x0, 4194304 bytes
mount /mnt/nor      -> vfs:451 volume nor0 mounted at /mnt/nor
```

The `fs` command family operates on the littlefs layer (volume root), while `ls /mnt/nor` goes
through the VFS namespace; both point at the same volume, but **volume root ≠ VFS root**, so a
file path must carry the mount point.

---

## 6. Host→board file channel: `fs put`

The board shell has no `echo`, and feeding multi-line source through vim is fragile on the real
board (this round hit it: ESC did not take effect, subsequent command lines were written into
the buffer as text, footer `31L`). **Do not use vim for automated uploads.**

`fs put` is the existing binary upload channel (`kernelsrc/shell/arkrtos_shell.c`):

```
fs put <path> <len>        # the board first reports "len bytes in 256-byte chunks"
                           # then takes data 256 B at a time, replying 0x06 only after landing (0x15 = stop)
```

Host-side script `fsput.py` (written this round, kept in a scratch directory):

```
python fsput.py <local file> <path in volume> [--mount]
# e.g. fsput.py norapp.c /k.c --mount   ->  fs put: ok, 447 bytes in /k.c
```

Two points:
- **MSYS/Git-Bash rewrites `/k.c` into a Windows path**, so `MSYS_NO_PATHCONV=1` is mandatory,
  otherwise the board receives `fs put C:/Users/.../PortableGit/k.c` (hit on the first try);
- each `0x06` is returned only when the data is **already on the volume**, not when it is
  received — bit-bang writes are slow and this semantics is exactly what stops the host from
  overrunning and blowing up the 128 B console FIFO.

---

## 7. Full-chain record (round one, bit-bang read era)

> The timings in this section (`packed in 24811 ms`, 33.7 s load) are from the bit-bang read
> era. After the read channel moved to memory mapping on 2026-11-05, the same pack takes about
> 3.7 s and loading 0.37 s (see §3.1). The log text is not edited — edited logs are not records.

```
# 1) host: upload the source
fsput.py norapp.c /k.c --mount                  -> fs put: ok, 447 bytes in /k.c

# 2) board: compile into NOR (four-pass differential + packing)
cc --pack /mnt/nor/k.c -o /mnt/nor/k.svcm
  cc: pack /mnt/nor/k.c -> /mnt/nor/k.svcm (4 passes, -O1)
  cc:   A 2940 B / B 2940 B / C 2940 B / D 2940 B
  cc: /mnt/nor/k.svcm: payload 2940 + table 6 x 4 = 3220 B
  cc:   reloc rom 1 / ram 1 / rom_movw 2 / ram_movw 2, entry +0x422
  cc: packed in 24811 ms

# 3) reset, remount, confirm the files are still there
ls -l /mnt/nor      -> k.c 447 / k.svcm 3220

# 4) run
mini run /mnt/nor/k.svcm   -> NOR: app alive / NOR: ctor ok / task exits, blocks returned
```

The resulting firmware needs three switches (size ledger in §9):

```
make USE_QSPI=1 USE_APPPACK=1 USE_MINI_VFS=1
```

- `USE_QSPI=1` — compiles in `drvqspi.c` and the `nor` commands;
- `USE_APPPACK=1` — the four-pass differential packing of `cc --pack` (an M2 capability,
  default off);
- `USE_MINI_VFS=1` — `mini run` reads through the VFS namespace (required to read
  `/mnt/nor/...`).

`make` does not track `-D` or headers: **`make clean` before changing switches**.

---

## 8. Speed: why 24.8 s / 33.7 s (round one, bit-bang era)

| Action | Target volume | Time |
|---|---|---|
| `cc --pack` producing 3220 B | ramfs `/tmp` | **168 ms** |
| `cc --pack` producing 3220 B | littlefs `/mnt/nor` | **24811 ms** |
| `mini run` loading a 2940 B payload | ramfs `/tmp` | same batch as above, within a second |
| `mini run` loading a 2940 B payload | littlefs `/mnt/nor` | **33.7 s** (to the first output line) |

The two-order-of-magnitude gap is not simply "NOR is slow"; it stacks three layers:

1. **Bit-bang itself** (§3): 62 KB/s, a hundredth of QUADSPI;
2. **littlefs write amplification**: writing 3220 B repeatedly rewrites metadata blocks, and
   every change can trigger a 4 KB sector erase (order of magnitude: 24.8 s ÷ one sector erase
   ≈ hundreds of them), which dominates the write side;
3. **The load path re-reads metadata on every open**: `arkrtos_mini_read_exact` calls
   `arkrtos_vfs_read_at` per chunk, and that is internally open + seek + read + close, with only
   one file cache in `g_fs`, so every chunk re-reads metadata from NOR.

Directions for speed-up, by benefit:

- ~~**Switch reads back to QUADSPI memory mapping**~~ — **done, see §3.1** (`ls` 258 s → 0.249 s);
- **Remove the microsecond delay from bit-bang** (`nor_delay_us()`'s spin loop is about 26 us
  per iteration, not 1 us; converting suggests another order of magnitude) — affects only
  writes/erases, currently about 0.4 s inside `make`, low priority;
- **Fewer opens on the load side** (add a "read whole file" entry to the VFS bridge, or enlarge
  the chunk in `mini` to read it in one go) — after the read-side speed-up the per-chunk metadata
  cost is already cheap, so the benefit shrinks.

After the read-side speed-up, **M3-b's usable result is "it persists, it runs, reads are no
longer the bottleneck"**; writes/erases are still bit-banged, and a single 4 KiB sector erase or
write is a small item at second scale (`make` is 3.84 s end to end, of which 3763 ms is the
compiler).

---

## 9. Size ledger

| Build | FLASH | RAM |
|---|---|---|
| tier 0 (all defaults, before `arkrtos_fs_last_is_noent`) | 381808 | 439308 |
| tier 0 (all defaults, **including** this round's noent fix) | **381832** (+24) | 439284 (−24) |
| `USE_QSPI=1` (probe only, before the AF fix) | 392824 | — |
| `USE_QSPI=1` (AF fix + write/erase + bit-bang channel, before noent) | 398320 | 439308 |
| `USE_QSPI=1 USE_APPPACK=1 USE_MINI_VFS=1` (round-one board firmware) | **405240** | **439324** (97.51%) |
| the same + memory-mapped read channel (**round-two board firmware**) | **419680** | **444744** (98.71%) |

- The **+24 B** in tier 0 is the inherent cost of the noent fix (one predicate function plus one
  call site); it is not a switchable feature and cannot be expected to be "zero change when off"
  — fixing a defect changes size, recorded honestly.
- RAM is only **98.71%** free; adding features requires looking at the pool and stack first, do
  not assume headroom.
- The read channel costs **FLASH +14440 B / RAM +5420 B** this round: 1 KiB of that is the two
  512 B comparison buffers of `nor speed` (used only while that command runs), the rest is
  self-test / channel selection / MM read code. Not one line of the bit-bang path was deleted —
  it is now "the fallback when the self-test fails", not dead code.
- The firmware is **not byte-reproducible**: the `build <__DATE__> <__TIME__>` in the `BOOT`
  banner makes each build's `.bin` hash different (measured: two builds of the same source give
  different md5), so the rule "same source → same bin" does not hold on this chain; verification
  must be based on serial readings.

---

## 10. Known limitations and debts

1. ~~**Boot still requires a manual mount**~~ **solved (2026-11-09, M3-d)**: the kernel mounts
   nor0 by itself at boot and reads the manifest; after a reset the serial output is directly
   `boot autostart: 1 started from /mini.autostart`, nobody types `fs mount` / `mini run`. The
   criterion is the `mini-autostart-reboot` item in `tools/final_app.py`: run once by hand first
   (to separate a manifest error from an image error), write the manifest and read it back,
   reset, then delete the manifest and reset again — it **must not run**.
2. ~~**The `mini autostart` manifest path and volume binding**~~ **solved, and simpler than
   recorded**: the manifest `/mini.autostart` goes through `arkrtos_fs`, and `arkrtos_fs` and the
   VFS `/mnt/nor` are **the same littlefs volume** (`arkrtos_vfs_lfs.c`'s `lfs_mount` explicitly
   does not mount a volume itself, it only checks `arkrtos_fs_mounted()`) — so the manifest and
   the image it names live and die together; a power cut cannot leave only half of them.
3. ~~**M3-c not done**~~ **done**: the `fs-mount-blank` case — a first mount of an unformatted
   volume must fail explicitly and **write not one byte** (mounting again and failing again
   proves it does not silently format), and the real volume then still mounts normally.
4. **Speed**: the read side is optimised (§3.1, §8), **writes/erases are still bit-banged**:
   one iteration of `nor_delay_us()`'s spin loop is about 26 us (not 1 us), so bit-bang should
   be another order of magnitude faster, but the write side is only about 0.4 s in the current
   workload, not done.
5. **The QUADSPI write path is not fixed**: the QUADSPI versions of
   `nor_write_enable/nor_page_program/nor_sector_erase` remain in the code but are not in the
   data path, and the `SR` completion-bit semantics are not settled.
6. **Coupling between the memory-mapped window and D-Cache**: this board does not enable
   D-Cache (no `SCB_EnableDCache` anywhere in the repo), so MM reads need no cache maintenance.
   **If D-Cache is enabled later, the window starting at `0x90000000` must first be configured
   non-cacheable (MPU)**, otherwise stale CPU lines are read — "the data is right, just not
   current" only appears intermittently under load and is harder to find than a read failure.
7. **Quad read (`0xEB`) is unusable and the cause is located**: PF6/PF7 are pinned as GPIO
   outputs high (single-line `/WP`, `/HOLD`) and were never connected to QUADSPI IO2/IO3. See
   §3.2; using it requires resolving the role conflict, current benefit is zero, not done.

---

## 11. How to re-run

Host scripts (scratch directory, not in the repo):

| Script | Purpose |
|---|---|
| `fsput.py <local> <path in volume> [--mount]` | push a file into the volume via `fs put`; `--mount` first does `fs unmount/mount/mount /mnt/nor` |
| `m1_drive.py <command file> [--noflash] [--log x]` | flash + send commands line by line (`@sec` prefix controls that line's wait) |
| `m3b_final.py [--log x]` | one-shot acceptance: reset → mount → `ls` → `stat` → `mini run` (150 s window) → `mini list` |
| `reset.sh` | reset only, no flash (OpenOCD `reset run`) |
| `time_cmd.py <command>...` | wall-clock time from Enter to the prompt reappearing (the A/B basis; this round's `ls` 257.888 s / 0.249 s came from it) |

New on-board read-channel diagnostics (this round):

| Command | Purpose |
|---|---|
| `nor speed [KiB]` | read the same data through bit-bang and memory mapping, report us / KB/s and a byte-wise memcmp |
| `nor mmode [0\|1\|2]` | switch the read channel (0=bit-bang 1=MM 0x03 2=MM 0xEB); self-tested, refuses if it fails |
| `nor sr2` | read status register 2 (QE bit); this driver **never writes** SR2 |
| `nor dummy [0..31]` | dummy cycles for quad read (sweep use, default 6) |

A/B basis: same firmware, same board, measure once with `nor mmode 0` and once with
`nor mmode 1`. Do not compare numbers across firmwares — the spin-loop ratio in
`nor_delay_us()` changes with compiler options.

A command-line pitfall: **do not pipe a long task's output into `head`** — `head` closing the
pipe early makes Python die with `BrokenPipeError` and it looks like "the board hung" (hit once
this round: the log stops at `>>> mini run` with exit code 1).

## 12. Change list for this round

| File | Nature |
|---|---|
| `board/stm32h743/drvqspi.c` / `.h` | new: QSPI NOR driver (bit-bang data path + QUADSPI evidence + `nor` commands) |
| same (round-two change) | read channel gains memory mapping (`ARKRTOS_NOR_MM_READ` default 1, falls back to bit-bang when the self-test fails), four subcommands `nor sr2|mmode|dummy|speed`, CCR read-back gate; the write/erase path **unchanged** |
| `board/stm32h743/drvblk.c` / `.h` | new `nor0` block device (bringup reads JEDEC) |
| `board/stm32h743/arkrtos_board.c` | board wiring |
| `config/stm32h743/arkrtos_partition.h` | `ARKRTOS_NOR_CHIP_SIZE` 16 MiB → 8 MiB, comments aligned with the real part |
| `kernelsrc/include/arkrtos_fs.h`, `kernelsrc/src/arkrtos_fs.c` | new `arkrtos_fs_last_is_noent()` |
| `kernelsrc/src/arkrtos_vfs_lfs.c` | fix: creating a file was judged an I/O error |
| `kernelsrc/include/arkrtos_features.h`, `example/.../sources.mk`, `Makefile` | switch wiring (`USE_QSPI` / `USE_APPPACK` / `USE_MINI_VFS`) |
