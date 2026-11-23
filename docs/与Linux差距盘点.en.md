# Gap inventory against Linux

**[简体中文](与Linux差距盘点.md) | English**

> Condensed English edition. The Chinese page (与Linux差距盘点.md) is normative: every number below is
> copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no
> new claim and no new measurement.

> **Revised 2026-11-19.** The process/execution-model and memory-management rows below, and the
> "not planned" list at the end, were rewritten against today's facts: **a statically linked native
> ELF executable can already be `exec`ed** (LoongArch64 page table + PLV3, RV32 PMP + U mode; see
> [MMU进程模型与ELF装载](MMU进程模型与ELF装载.md) section 9 and section 5.25 of the QEMU page). What
> is still missing for a real Linux binary is the **system-call ABI**: `ecall` carries Ark's own
> service numbers `0x10..0x22`, not Linux `write=64` / `exit=93`, there are no `brk` / `mmap` /
> `open` numbers, no fd inheritance and no signals, and dynamic linking (`PT_INTERP` / `ld.so`) is
> rejected by the loader. That gap is registered as an acceptance item in [欠账盘点](欠账盘点.md).

> **Revised 2026-11-21**: "the system-call ABI is still missing" is no longer the state of
> things. The Linux-ABI profile (`ARKRTOS_USE_LINUX_ABI`) now speaks 25 asm-generic numbers
> (non-blocking `pipe2` and a minimal signal set included), measured on QEMU LA64 by three
> fixtures (`exit code 7` / `55` / `138`; see
> [Linux可执行文件兼容-系统调用ABI](Linux可执行文件兼容-系统调用ABI.en.md)). What is really
> missing now is the **process object** (`fork` / `clone` / fd inheritance / cross-task signals)
> and the whole glibc compatibility surface.

One question: put Ark RTOS and Linux side by side — what is still missing on the App side and on the
kernel side?

First the basis, so that a difference in purpose is not mistaken for a defect:

- Ark RTOS does **not** aim to be a Linux. It aims to let an MCU "install, upgrade and uninstall
  applications and drivers like a phone does". Linux aims at a general-purpose time-sharing OS
  (process model + virtual memory + multi-user + full ecosystem). What the two **share is only the
  set of names a user-state program uses** (POSIX: files, socket, pthread, semaphore, ...). Aligning
  the names, with semantics that land on a real implementation, is what lowers migration cost.
- So the gaps below are split into three classes, and **only class 2 is a debit**:
  1. **Essential design trade-off** — Linux has it, but the positioning of Ark RTOS rules it out
     (doing it would make it something else);
  2. **Debit within the stated scope** — "use it like Linux" has been promised, but it is not done
     or it explicitly errors out;
  3. **Ecosystem dimension** — the libraries, protocols and certifications Linux accumulated over
     decades; this project does not claim to be stronger there.
- Every conclusion points at a file in the repo; no "roughly" or "should be". For anything
  unsupported the repo's rule is **return an error and set `errno`**, never a plausible-looking
  result (see §1 of [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md)).

## 1. The parts that already behave like Linux

Ignoring ecosystem, and asking only what a user-state program can reach, these really follow Linux
habits:

| Dimension | What already exists | Source |
|---|---|---|
| Path namespace | `/` is volatile ramfs, `/dev` mirrors the device registry, `/mnt/<name>` is a persistent volume, `/proc` is a read-only flat virtual fs; `ls /` merges submounts and lists `dev` and `proc` | [VFS路径命名空间](VFS路径命名空间.md) |
| POSIX names | `pthread` / `semaphore` / `mqueue` / `unistd` / `dirent` / `sys/stat` / `fcntl` / `errno`; an ordinary C program compiles for the board after editing its include list | §4, §10 of [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) |
| File-class POSIX | `open/read/write/lseek/stat/fstat/unlink/opendir/readdir/closedir` go straight to VFS; `FS_DEMO` passes on board | §4.7 of the same page |
| socket / select | lwIP 2.1.3 plus a `socket/select` compatibility surface, `SOCKET_DEMO` `pass=36 fail=0` (**loopback** netif) | [socket与select兼容层](socket与select兼容层.md) |
| `/proc` | `version/uptime/meminfo/cpuinfo/tasks`, generated fresh on every read | `kernelsrc/include/arkrtos_features.h` (`ARKRTOS_USE_PROCFS`) |
| Device model | Device tree (a DTS subset) plus compatible matching; `dt` prints 20 nodes | [设备树](设备树.md) |
| Kernel console | The `console` command family plus `mount/ls/cat/fs`, habits leaning on Linux | [内核Shell控制台使用说明](内核Shell控制台使用说明.md), [Shell与日志Linux化](Shell与日志Linux化.md) |
| Thread service | New kernel SVC 0x1B (`arkrtos_thread_create/self/exit`); POSIX `pthread` is built on it | §5 of [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) |
| Privilege model | Tasks run unprivileged, SVC traps into the kernel, per-task MPU isolation; an overreach is stopped by hardware on the spot (`pass=78 fail=0`) | [内存保护与App越权验证](内存保护与App越权验证.md) |

In one line: **the "behaves like Linux" main line works**; what follows is what does not.

> Conversely Ark RTOS has mechanisms Linux has **no counterpart** for (not gaps — the other
> direction): the kernel's **last words** that bring a device back to a safe state when its owner task
> dies ([App崩溃硬件安全与遗言机制](App崩溃硬件安全与遗言机制.md)), seamless upgrade where two
> MiniApps **hand over control while running** ([无缝升级例程](无缝升级例程.md)), the **softbus**
> between devices (events / RPC / data plane, [软总线接入](软总线接入.md)), and the **on-board unit
> tests** shipped with the image (`utest`, [单元测试框架](单元测试框架.md)). On the Linux side there
> is either no equivalent concept (residual hardware state is each driver's problem) or it is carried
> by user-state middleware.

## 2. Dimension-by-dimension gap tables

### 2.1 Process and execution model (the biggest gap)

| Linux has | Ark RTOS | Class |
|---|---|---|
| Process (own address space) + `fork` / `exec` / `waitpid` | No `fork` semantics: **one App = one address space + several threads**; `fork` / `waitpid` are not provided. `exec` **does** exist in two forms: the shell command (load a new ELF onto a new task) and `execve` (Linux ABI nr 221, 2026-11-21, run under QEMU LA64), which **really replaces the current task's image** (same task id, fds kept, does not return on success); the loaded program receives `sp -> argc/argv` per the Linux entry convention. **`fork` is done** (`ARKRTOS_USE_LINUX_FORK`, default off, 2026-11-23, 21 checks under QEMU LA64): per-process fd tables with inheritance, cross-task `SIGCHLD` and a really blocking `wait4` (restartable syscall) all have readouts; copy-on-write is done as well (`ARKRTOS_USE_PAGING_COW`, default off, 2026-11-23, 26 checks under QEMU LA64: 8 KB pairs with refcounts, stack pages forced private); still no threads (`CLONE_VM`/`CLONE_THREAD` refused) | Done (static ELF loading, same-task `execve`, `fork`+`wait4`, copy-on-write) |
| `getpid` / `getppid` / PID namespace | Only a kernel task id (task-table index); `getpid` is a stub only | Trade-off |
| Dynamic linking, `.so` shared libraries, ELF loader | Two kinds of input coexist: the `.svcapp` container (copied whole into RAM) and a **statically linked native ELF** (`exec` / `ld`, running on LA64 and RV32; 32/64-bit field tables chosen by `__SIZEOF_LONG__`). The ELF loader accepts only `PT_LOAD` + `e_entry`: `PT_INTERP` (dynamic linking) is explicitly rejected (`ERR_FORMAT`); **no `.so`, no relocated shared libraries, no Linux system-call ABI** (`ecall` carries Ark's `0x10..0x22`) | Trade-off (dynamic linking, shared libraries) + debit (Linux syscall ABI, see [欠账盘点](欠账盘点.md)) |
| `argv` / `argc` / environment variables | A `.svcapp` App still enters at `main(void)` with no argv; an **ELF program loaded by `exec` receives `sp -> argc/argv` per the Linux entry convention** (measured `argc=4` and `argv[0..3]` matching exactly), but there is **no `environ`** | Trade-off (`.svcapp`) + done (ELF argv) |
| Signals (`signal` / `sigaction` / `kill`) | Ark's own POSIX layer has **no signal API**; task synchronisation uses event sets, semaphores, mutexes, message queues, condition variables. **The Linux-ABI profile has a minimal set** (2026-11-21, QEMU LA64): `rt_sigaction` / `rt_sigprocmask` / `kill` / `tkill` / `tgkill` / `rt_sigreturn`, self-delivery **and cross-task delivery** (2026-11-23: the target is resolved by pid and woken if it is parked in a blocking `wait4`), always delivered before a syscall returns, no preemptive delivery, no real-time signals, no `siginfo`; the signal frame is a 32-byte header plus a snapshot of the whole trap frame | Partly done (one Linux-ABI signal tier, cross-task included) + trade-off (Ark POSIX layer) |
| `pipe` / `FIFO` / Unix domain socket | `pipe2` is done (2026-11-21, QEMU LA64, Linux ABI number 59): ring buffer with per-end reference counts, **non-blocking**, survives `execve`; FIFO and Unix domain sockets are still absent, the equivalents being message queues plus socket (loopback) | Partly done + trade-off |
| System V / POSIX shared memory `shm` | No `shmget`/`shm_open`; only the MPU shared window (used for MiniApp handover, not exposed as general IPC) | Trade-off |
| `clone` / namespaces / containers | None | Trade-off |

### 2.2 Memory management

| Linux has | Ark RTOS | Class |
|---|---|---|
| MMU virtual memory, address translation, demand paging, swap | **Window profile** (Cortex-M / RV32): only MPU/PMP range protection, no translation. **Page-table profile** (LoongArch64): page tables + PLV3 run, with per-App address spaces (same VA, several instances), per-space ASID, a 4 KB frame allocator, demand paging and page replacement (swap); readings in sections 5.21-5.29 of the QEMU page. **Still missing**: TLB shootdown (targeted invalidation by ASID, IPI on multicore) and real-hardware verification | The window profile is a trade-off (hardware premise); the page-table profile's shootdown is a debit (the remaining half of U-7 in [欠账盘点](欠账盘点.md)) |
| `mmap` / `munmap` / `brk` / `sbrk` | None by default; App memory comes from the slot RAM window plus an App-side arena (`arkrtos_posix_malloc`, default 1 KB). The Linux ABI profile has a minimal implementation: anonymous `mmap` plus file-backed mapping with whole-file write-back (`ARKRTOS_USE_PAGING_FILEMAP`, default off), without `MAP_FIXED` or concurrent writers on a shared file page (page-level write protection exists, section 8.6); see section 8.5 of the Linux ABI page | Trade-off + partial delivery (Linux ABI profile) |
| Kernel manages physical memory centrally | The kernel is not an allocator: addresses are defined once in `config/arkrtos_partition.h`, slot windows are bound at install time | Trade-off |
| Memory overcommit / OOM killer | None. An overreach is stopped on the spot by MPU/PMP or the TLB entry; **the stack sentinel is only written, never checked** (`stack_bottom[0]` gets `0xed01`, nothing in the repo reads it — U-13 in [欠账盘点](欠账盘点.md)) | Trade-off + debit (stack sentinel) |

> On Cortex-M this is architectural: without an MMU there is no virtual memory, hence no COW basis
> for `fork` — not "unfinished", decided by the hardware premise. On LoongArch64 the hardware
> premise is already present (page tables + PLV3 actually run), and the software side now has
> per-space ASID, a frame allocator, demand paging and page replacement; **the only remaining debits
> are TLB shootdown and real-hardware verification** (the `stack_bottom[0]` sentinel is a separate
> debit, see [欠账盘点](欠账盘点.md)).

### 2.3 File system / VFS (where in-scope debits concentrate)

| Linux has | Ark RTOS | Class |
|---|---|---|
| ext4 / xfs / btrfs / tmpfs / sysfs / debugfs / FUSE... | Only four: **ramfs (volatile `/`), littlefs (persistent volume), devfs mirror, procfs (read-only flat)** | Trade-off (small system) |
| Symlinks `symlink` / hard links `link` | VFS **deliberately** does not do symlink/link (see `docs/architecture.md §9`); the POSIX layer now has the three names `link()`/`symlink()`/`readlink()`, the first two **always return `EPERM`**, `readlink()` reports `ENOENT`/`EINVAL` by real semantics and never pretends success | Trade-off (names wired, capability not provided) |
| `truncate` / `ftruncate` | **Supported**: `ark_vfs_truncate`/`ark_vfs_ftruncate` + fsdrv `truncate` callback + `ARK_FS_CAP_TRUNC`, implemented for littlefs and ramfs; POSIX `truncate()`/`ftruncate()` wired | Closed |
| `mkdir` / `rename` / `rmdir` | VFS has `mkdir` / `rename` / `unlink`; the POSIX layer is **fully wired** (`mkdir` in `sys/stat.h`, `rmdir`/`rename`/`unlink` in `unistd.h`) | Closed |
| `inode` semantics (`st_ino` / `st_nlink`) | `struct stat` deliberately carries only what VFS can answer: `st_mode` type bits, `st_size`, `st_mtime`; **no `st_ino`/`st_nlink`** | Trade-off |
| File permission `rwx`, owner/group | **No permission bits, no `chmod`-semantics rwx**; the model is "can this App reach this path" | Trade-off |
| `ioctl` / `fcntl` locks / `flock` | No file locking; the device-side `ctrl` is a private control interface, not POSIX `ioctl` | Trade-off |
| `inotify` / `epoll` | None (`select`/`poll` compatibility exists, underneath is lwIP); no `epoll`, no `inotify` | Trade-off |
| Mount options / loop devices / bind mounts | `mount` mounts a volume by name only; no options, no loop, no bind mount | Trade-off |
| Disk quota / auditing | None | Trade-off |
| Automatic mount discovery / `fstab` | None; `/mnt/<name>` is generated by the kernel from the block-device registry | Trade-off |

> Note: the VFS upstream API `ark_vfs` **does** have `mkdir` / `rename` / `resolve` / `normalize`;
> what was missing was **POSIX-layer wrapping for those names** (see `kernelsrc/sdk/posix/`). That
> item was "interface not wired", not "kernel absent". See [VFS路径命名空间](VFS路径命名空间.md) and
> `kernelsrc/include/arkrtos_vfs.h`.

### 2.4 Privilege and security

| Linux has | Ark RTOS | Class |
|---|---|---|
| Multi-user, uid/gid, `setuid` | **No multi-user, no uid/gid**; POSIX `chmod`/`chown` have names but no enforcing semantics | Trade-off |
| Capabilities, LSM (SELinux/AppArmor), seccomp | None | Trade-off |
| Secure Boot chain of trust, signature verification | An **optional** image signature (HMAC-SHA256, symmetric, default off) — "tamper-proof plus origin claim", **not a chain of trust**; A/B rollback explicitly not done | Trade-off (boundary already drawn) |
| Kernel address-space isolation / KASLR | Unprivileged tasks and the kernel are separated by hardware privilege level + MPU; no ASLR | Trade-off |

### 2.5 Scheduling and concurrency

| Linux has | Ark RTOS | Class |
|---|---|---|
| SMP, CPU affinity, `cgroup` | **Single core**; `arkrtos_spin.h` reserves an abstraction for SMP, but the current implementation is single core | Trade-off |
| Completely fair scheduling / several scheduling classes | Priority preemption plus time slicing at equal priority; the ready set is a 256-bit two-level bitmap (O(1)) | Trade-off |
| PREEMPT_RT patches | It is an RTOS already: determinism beats mainline Linux | — |
| `nice` / `sched_setscheduler` | No matching POSIX names (priority is given at thread creation) | Debit (small) |

### 2.6 Networking

| Linux has | Ark RTOS | Class |
|---|---|---|
| Full TCP/IP, netfilter, netlink, raw sockets, multiple NICs, wireless | lwIP **loopback** (`127.0.0.1/8`), no real PHY transmit/receive; `socket`/`select` compatibility usable | Debit (board lacks PHY) |
| DNS / DHCP / NTP clients | None | Ecosystem |
| TLS / OpenSSL | None | Ecosystem |
| `getaddrinfo` / `if_nametoindex` and the whole set | Only what the compatibility surface covers | Ecosystem |

> Networking is "semantics verifiable, transmit/receive not": see §1 of [lwIP协议栈接入](lwIP协议栈接入.md)
> and the readme list of items never verified on board.

### 2.7 Time

| Linux has | Ark RTOS | Class |
|---|---|---|
| RTC, wall clock, time zones, `localtime` / `strftime` | **No RTC, no calendar**; `CLOCK_REALTIME` is aliased to the monotonic clock (never a made-up answer); `st_mtime` is **seconds since boot**; `localtime` not provided | Trade-off |
| `timerfd` / POSIX timers | Software timers exist (SVC 0x17); no `timerfd` | Trade-off |

### 2.8 System management / user space

| Linux has | Ark RTOS | Class |
|---|---|---|
| init / systemd / service manager | No init system; there is a **boot autostart list** (image-header `auto-start`) | Trade-off |
| Full coreutils, package manager (apt/opkg) | No package manager; the kernel console offers a compact set (`info/app/drv/task/sched/fault/install/log/pool/trace/cfg` + `mount/ls/cat/fs`) | Trade-off |
| Kernel modules `.ko` | No kernel modules; **but drivers and Apps take the same dynamic install path** (`.svcapp` → `install`), and that main line is itself the substitute for "loading a module" | — |
| Users / login / sessions / `su` / `sudo` | None | Trade-off |
| Power management (DVFS / suspend / resume) | None | Trade-off |
| `/sys` / `sysctl` / `udev` hotplug | No `/sys`, no hotplug; devices are enumerated from the device tree at boot | Trade-off |

### 2.9 Development / debugging / toolchain

| Linux has | Ark RTOS | Class |
|---|---|---|
| GCC/Clang + make/cmake + GDB | The main project is Keil (AC5/AC6, `uvprojx`); there is a Kconfig toolchain and `tools/` scripts; debugging goes through `mdkdebug` (UVSOCK), not GDB | Trade-off |
| `ptrace` / `perf` / `ftrace` / `strace` | None; the equivalents are kernel trace/scope, `dmesg`, the crash diary | Trade-off |
| Dynamic instrumentation / eBPF | None | Trade-off |

## 3. Ordering the debits

Only **in-scope** items ("promised to behave like Linux, not done or explicitly erroring"), sorted by
whether it is worth fixing:

| Priority | Debit | Status | Note |
|---|---|---|---|
| High | Real Ethernet transmit/receive | Loopback NIC only | Needs a board-level PHY driver + RMII pins; the dividing line for "can we say we support networking" |
| Medium | Soft/hard link capability | Names wired, `link`/`symlink` always `EPERM` | VFS has no inode, no second name — architectural; changing it moves the namespace model |
| Medium | File locking, `epoll` | None | Add on demand; not a hard migration blocker |
| Low | `nice` / `sched_setscheduler` and other scheduling names | None | Priority is already settable at thread creation; enough |
| Low | A writable `/sys`-style kernel attribute surface | None (`/proc` is read-only) | Do it when needed |

**Not planned** (class 1 trade-offs, written down so they are not repeatedly mistaken for "not done
yet"): dynamic linking and `.so` shared libraries,
multi-user / uid-gid, power management, ASLR, kernel modules, package manager,
shared-memory IPC, `/sys` + hotplug, symmetric-multicore automatic load balancing (SMP already has
AMP core partitioning and explicit task migration; **static ELF loading with the Linux entry
convention, plus the Linux syscall ABI for static programs (non-blocking pipes and the minimal
signal set included), `fork`/`wait4` and copy-on-write are done and are not on this
list**; what `fork` still lacks is threads and preemptive signal delivery).

## 4. One-line conclusion

- **The "behaves like Linux" main line works** (path namespace, POSIX/Windows compatibility layer,
  file-class POSIX, socket/select, pthread, `/proc`, device tree, console).
- **Genuine debits are few**: the ones most worth fixing are the **directory-operation wrappers
  missing from the POSIX layer** (the kernel already has them — pure wiring) and **real Ethernet
  transmit/receive** (the board lacks a PHY).
- **Most other differences are differences in positioning, not in completeness**: without an MMU
  there is no virtual memory, without a process model there is no `fork` — decided jointly by the
  Cortex-M hardware premise and this project's positioning of "dynamically installing applications
  and drivers". Scoring it row by row against a generic Linux checklist turns "never intended" items
  into apparent defects.

## Related documents

- [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) — capability table and boundaries
- [socket与select兼容层](socket与select兼容层.md)
- [VFS路径命名空间](VFS路径命名空间.md)
- [小程序设计](小程序设计.md) — the contrast with ELF
- [崩溃恢复与镜像版本把关](崩溃恢复与镜像版本把关.md) — signing / rollback boundary
- [硬件耦合审计](硬件耦合审计.md) — capability / optional / contract / layout, four layers
