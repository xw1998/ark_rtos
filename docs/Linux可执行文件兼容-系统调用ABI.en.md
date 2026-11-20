# Running Linux executables on Ark RTOS: the syscall ABI (MMU profile, LA64 measured)

**[简体中文](Linux可执行文件兼容-系统调用ABI.md) | English([Linux可执行文件兼容-系统调用ABI.en.md](Linux可执行文件兼容-系统调用ABI.en.md))**

*Condensed English edition. The Chinese page is normative; nothing new is
claimed here and no extra measurements are introduced.*

## 0 One line

On the MMU profile the loader already puts a **native Linux static ELF
executable** into a PLV3 space and jumps to `e_entry`. This layer answers the
next question: how that program talks to the kernel. It issues Linux syscall
numbers (`syscall 0` with the number in `$a7`); the kernel recognises that ABI
and returns Linux return-value semantics. A real serial readout from
`qemu-system-loongarch64 -machine virt` (program: `example/qemu_loongarch64/elf/linux_hello.c`,
a purely Linux-ABI static binary with its own `_start`):

```
[linux] write /mnt/ram/linux_hello rc=0
[linux] exec rc=0
[linux] hello from the Linux syscall ABI (a7=64 write)
[linux] uname -> 0 ok
[linux] sysname=ArkRTOS machine=loongarch64
[linux] clock_gettime(MONOTONIC) -> 0 ok
[linux] clock_gettime(REALTIME) -> -38 ok
[linux] getpid==task id>0 -> 1 ok
[linux] brk(0)>0 -> 1 ok
[linux] brk(+4096) -> 34607104 ok
[linux] mmap>0 -> 1 ok
[linux] mmap write/read -> 1 ok
[linux] openat>0 -> 1 ok
[linux] read 4 bytes -> 4 ok
[linux] ELF magic -> 1 ok
[linux] close -> 0 ok
[linux] openat(missing) -> -2 ok
[linux] PASS: every syscall answered as specified
[linux] finished after 0 ms, exit code 7
```

**"A static program runs" is not "Linux is supported".** Section 3 lists what is
explicitly unsupported.

## 1 Supported syscalls (15, all measured)

Numbers follow `asm-generic` (shared by LoongArch / RISC-V / ARM64):

| nr | name | how Ark fulfils it | measured |
|---:|---|---|---|
| 56 | `openat` | `arkrtos_vfs_app_stat()` on the VFS namespace; directories return `-EISDIR` | `openat>0 -> 1 ok` |
| 57 | `close` | release the fd slot, return 0 | `close -> 0 ok` |
| 63 | `read` | `arkrtos_vfs_read_at()` at the fd offset | `read 4 bytes -> 4 ok` |
| 64 | `write` | 128-byte chunks through `arkrtos_board_putc` | the program's own text |
| 93 | `exit` | `arkrtos_thread_exit_code_internal()` | `exit code 7` |
| 94 | `exit_group` | same (single-threaded) | same |
| 96 | `set_tid_address` | remember the address, return the task id (no futex) | same path as getpid |
| 113 | `clock_gettime` | `MONOTONIC` from kernel ticks; `REALTIME` returns `-ENOSYS` | `MONOTONIC -> 0 ok` / `REALTIME -> -38 ok` |
| 160 | `uname` | `sysname=ArkRTOS`, `machine=loongarch64` | `uname -> 0 ok` |
| 172 | `getpid` | internal task id | `getpid==task id>0 -> 1 ok` |
| 174 | `getuid` | always 0 | same path |
| 175 | `geteuid` | always 0 | same path |
| 214 | `brk` | monotonic allocation inside the ELF window | `brk(0)>0` / `brk(+4096) -> 34607104 ok` |
| 215 | `munmap` | returns 0 only, **does not give the address back** | same path |
| 222 | `mmap` | only `MAP_PRIVATE|MAP_ANONYMOUS`; `MAP_FIXED` refused | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |

Unknown numbers return `-ENOSYS` (38) and are counted. Nothing is guessed.

## 2 How the two ABIs are told apart

| arch | Ark ABI | Linux ABI | rule |
|---|---|---|---|
| LoongArch64 | 15-bit immediate in `syscall` | `syscall 0` with the number in `$a7` | read the instruction word at `ERA-4`: immediate == 0 means Linux |
| RV32IMAC | `a7` in 0x10..0x22 | `a7` holds an asm-generic number (smallest 56) | ranges do not overlap |
| Cortex-M3/M4/M7 | SVC immediate | — | always returns `0xFFFFFFFF` |
| LoongArch32 | SVC immediate | — | the same |

The kernel adds exactly one hook: `SVC_Server()` calls
`arkrtos_linux_abi_try(ctx)` after `arkrtos_guard_svc()` and before its
`switch`; a non-zero return means "already handled". Sign extension of the
return value is a separate port primitive
(`arkrtos_port_svc_set_ret_signed`), because in Linux a negative value is
`-errno` and writing it unsigned turns `-2` into 4294967294.

## 3 Explicitly unsupported

`fork`/`clone`/`execve`, signals, fd inheritance / pipes / redirection,
`socket`/`select`, `futex`, real `mmap` semantics (file-backed, shared,
`MAP_FIXED`, permission bits), and threads. Each of these needs an object (a
process, a user-side signal frame, a shared-memory wait queue) that Ark RTOS
does not have; faking them would produce silently wrong answers. See the Chinese
page for the reasoning per row.

Dynamic linking no longer belongs in this list: with `ARKRTOS_USE_DYNLOAD` on,
the kernel loads a `PT_INTERP` interpreter and ships a minimal in-tree
`ld-ark.so` (2026-11-20, QEMU LA64). That is not glibc support - see
[动态链接与PT_INTERP](动态链接与PT_INTERP.en.md).

## 4 Deliberate degradations

- **fd table** is kernel-global (`lx_fd_t s_fd[8]`, path + offset) and is reset
  on every successful `exec`. There are no parents, so inheritance cannot be
  faked.
- **heap**: `brk` and `mmap` share one monotonic region inside the 2 MB ELF
  window (`0x02000000`), leaving 128 KB of stack below the top.
- **`munmap` returns 0 without reclaiming**: pretending to free would leave the
  program using memory another task may own — a silently wrong answer.
- **user pointers are never dereferenced directly**: everything goes through
  `arkrtos_copy_from_user()` / `arkrtos_copy_to_user()`, which judge legality by
  the task's four TCB window fields (`rom_start/rom_size/ram_start/ram_size`).

## 5 Switch and zero footprint

`ARKRTOS_USE_LINUX_ABI` (features header + Kconfig), default **0**, and
`depends on ARKRTOS_USE_ELF`. With the switch off the whole
`arkrtos_linux_abi.c` file compiles to nothing, the port has no
`arkrtos_port_linux_syscall_num`, and `arkrtos_autoconf.h` records the 0. The
gate measures this in both directions (the off-profile `.o` must have an empty
symbol table).

## 6 Gate

```
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py [--no-build]
```

13 items; prints `LINUX-ABI-GATE OK` when all pass and is invoked from
`tools/ci_gate.py` as `linux-abi`: default-off, kconfig-wired, features-guard,
board-switch, off-zero-symbols, port-wiring (two directions), compile-on (5 TUs
under the real LA64 board), syscall-table (15 numbers vs asm-generic),
abi-split-la64, abi-split-rv32, build, qemu-readout (13 checks + PASS + exit 7
+ the unrelated demos still intact), and parser-selfcheck (four deliberately
broken logs must each be rejected).

## 7 Two real bugs and one readout trap found on the way

1. **Every user pointer returned `-EFAULT`**: `arkrtos_elf.c` wrote the task's
   rom/ram window fields inside `#if (ARKRTOS_USE_MPU == 1)`, but QEMU LA64 is
   MPU=0 / PAGING=1, so all four fields stayed 0 and `copy_from_user` rejected
   every buffer. The fix is to write the window fields unconditionally and keep
   only `arkrtos_mpu_build_task()` inside the MPU profile.
2. **`openat` looked up a namespace path in a volume API**: `/mnt/ram/...` is a
   namespace path, `arkrtos_fs_stat_path()` wants a volume-relative one, hence a
   permanent `-ENOENT`. Switched to `arkrtos_vfs_app_stat()` /
   `arkrtos_vfs_read_at()`.
3. **Readout trap**: the `[linux] exec rc=0` line sometimes appeared as `[l`,
   with the rest showing up only after the program finished. That was not lost
   bytes but **a line split by preemption** once the user task started running;
   the demo now prints whole lines under the scheduler lock. Two reader-side
   traps were fixed too: killing on the marker prefix truncates `exit code 7`,
   and `-serial file:` drops the last sub-4 KB block on kill.

## 8 Boundaries

This is the minimal syscall set that lets a static program run, not a Linux
compatibility layer: no ptrace, no Linux `/proc`. The
real-time behaviour is unchanged — every Linux-ABI path completes synchronously
inside the SVC with no extra thread, no blocking point. Going further requires a
process object (for signal frames and a real libc surface) before anything
else. Dynamic linking itself has its own switch (`ARKRTOS_USE_DYNLOAD`) and a
minimal in-tree loader, but running glibc binaries is still out of reach.