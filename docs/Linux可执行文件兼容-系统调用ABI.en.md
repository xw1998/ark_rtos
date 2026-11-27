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

## 1 Supported syscalls (61; 47 measured on QEMU, 13 socket numbers host-tier only, 1 table-and-argument only)

Numbers follow `asm-generic` (shared by LoongArch / RISC-V / ARM64):

| nr | name | how Ark fulfils it | measured |
|---:|---|---|---|
| 23 | `dup` | copy one fd slot (pipe/file/standard stream) into the lowest free number (8.12) | `dup(writer) is a new fd -> 1 ok` / `read through the copy -> 3 ok` / `dup(999) -> -EBADF = -9` / `exit code 56` |
| 24 | `dup3` | copy to a chosen number; `fd 0..2` becomes an alias instead of a shared slot; `O_CLOEXEC` is **accepted and recorded**, `old==new` is refused (8.12) | `dup3(writer, 1) returns 1 -> 1 ok` / `dup3(1, 6, O_CLOEXEC) returns 6 -> 6 ok` / `dup3(reader, writer) overwrites -> 4 ok` |
| 25 | `fcntl` | six commands: `F_DUPFD` / `F_GETFD` / `F_SETFD` / `F_GETFL` / `F_SETFL` / `F_DUPFD_CLOEXEC` (the last is `F_DUPFD` plus `FD_CLOEXEC`); every other command returns `-EINVAL` (8.13) | `F_GETFD reads FD_CLOEXEC back -> 1 ok` / `F_GETFL(reader) = O_RDONLY\|O_NONBLOCK = 2048 ok` / `F_DUPFD_CLOEXEC(9) -> 9 = 9 ok` |
| 56 | `openat` | `arkrtos_vfs_app_stat()` on the VFS namespace; directories return `-EISDIR` | `openat>0 -> 1 ok` |
| 57 | `close` | release the fd slot, return 0 (a pipe end drops its reference count) | `close -> 0 ok` |
| 59 | `pipe2` | ring-buffer pipe with per-end reference counts; **blocking by default**, `O_NONBLOCK` mode alongside (section 6) | `read empty -> -EAGAIN = -11` (non-blocking mode) / `blocking pipe read waited >= 40 ms -> 1 ok` (fork tier, section 8.15) / `exit code 55` |
| 63 | `read` | `arkrtos_vfs_read_at()` at the fd offset | `read 4 bytes -> 4 ok` |
| 64 | `write` | 128-byte chunks through `arkrtos_board_putc` | the program's own text |
| 67 | `pread64` | read at `pos` **without moving the fd cursor** (what `ld.so` needs to walk a segment table) | no fixture readout (1.1) |
| 93 | `exit` | `arkrtos_thread_exit_code_internal()` | `exit code 7` |
| 94 | `exit_group` | same (single-threaded) | same |
| 96 | `set_tid_address` | remember the address, return the task id (not involved in futex; see 8.4) | same path as getpid |
| 98 | `futex` | eleven ops: `WAIT`/`WAKE`/`REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`/`WAKE_BITSET` with timeout and bitsets, the PI trio `LOCK_PI`/`UNLOCK_PI`/`TRYLOCK_PI` and the condition-variable PI variants `WAIT_REQUEUE_PI`/`CMP_REQUEUE_PI`; key = physical frame + offset in page (`ARKRTOS_USE_LINUX_FUTEX`, 8.4 / 8.4b / 8.4c) | `futex(WAIT) on a different value == -EAGAIN -> -11 ok` / `futex(WAIT) returned 0 after being woken -> 0 ok` / `futex2 bitset waker saw 1, woke only matching (bits=7) -> 7 ok` / `futex(WAIT) with 50 ms timeout == -ETIMEDOUT -> -110 ok` / `futex LOCK_PI on a free word == 0 -> 0 ok` / `futex LOCK_PI twice == -EDEADLK -> -35 ok` / `futex TRYLOCK_PI on an owned word == -EAGAIN -> -11 ok` |
| 99 | `set_robust_list` | register the chain of locks that must be settled if the owner dies: non-null head, 8-byte aligned, length fits `next/futex_offset/pending`, else `-EINVAL` (8.4b) | `set_robust_list with a short length == -EINVAL -> -22 ok` / `set_robust_list on a well formed head == 0 -> 0 ok` |
| 100 | `get_robust_list` | own pid only (there is no second process identity to query); a foreign pid honestly returns `-ESRCH`, a null out-pointer `-EFAULT` (8.4b) | `get_robust_list(self) == 0 -> 0 ok` / `get_robust_list reports the length we registered -> 24 ok` / `get_robust_list on a foreign pid == -ESRCH -> -3 ok` |
| 113 | `clock_gettime` | `MONOTONIC` from kernel ticks; `REALTIME` returns `-ENOSYS` | `MONOTONIC -> 0 ok` / `REALTIME -> -38 ok` |
| 118 | `sched_setparam` | priority only, policy unchanged; `priority != 0` on `OTHER`/`BATCH`/`IDLE` and anything on `DEADLINE` return `-EINVAL` (as on Linux: use `sched_setattr` for the latter) (section 8.17) | part of the 63 `[sched]` checks; `getparam FIFO priority is 88 -> 88 ok`; error face in the 633 host checks |
| 119 | `sched_setscheduler` | `pid` / `policy` / `sched_param*`; FIFO/RR land on `100-priority`, OTHER/BATCH on `128+nice`, IDLE on 147; `DEADLINE` has no field for the triplet, so `-EINVAL` (section 8.17) | `getscheduler default -> 0 ok` / `getparam FIFO priority is 88 -> 88 ok` |
| 120 | `sched_getscheduler` | reads `sched_policy` from the TCB (policy follows the task slot; no private table) (section 8.17) | `getattr policy is DEADLINE -> 6 ok` (same field) |
| 121 | `sched_getparam` | derived from the kernel priority: `100-priority` for FIFO/RR, 0 otherwise (section 8.17) | `getparam FIFO priority is 88 -> 88 ok` |
| 122 | `sched_setaffinity` | single core: `len >= sizeof(unsigned long)`, mask accepts only bit 0 and it must be set, else `-EINVAL` (section 8.17) | `setaffinity short len -> EINVAL -> -22 ok` |
| 123 | `sched_getaffinity` | same checks, then writes back 1 (CPU0 only) (section 8.17) | mask bit reads back as 1 in the `[sched]` block |
| 124 | `sched_yield` | moves itself to the tail of its priority bucket and posts a reschedule (a no-op when alone) (section 8.17) | `sched_yield -> 0 ok` |
| 125 | `sched_get_priority_max` | 99 for FIFO/RR; 0 for OTHER/BATCH/IDLE/DEADLINE; `-EINVAL` for an unknown policy (section 8.17) | bound assertions in the 633 host checks |
| 126 | `sched_get_priority_min` | 1 for FIFO/RR; same otherwise (section 8.17) | same |
| 127 | `sched_rr_get_interval` | rotation = 10 ticks x 500 us = 5 ms, returned as a `struct timespec` (two 8-byte longs) (section 8.17) | `rr interval nsec is 5000000 -> 5000000 ok` |
| 129 | `kill` | only towards the calling task (`pid != self` gives `-ESRCH`); sets pending (section 7) | `kill rc survives handler -> 0 ok` / `kill other pid -> -ESRCH = -3 ok` |
| 130 | `tkill` | same entity as `kill` while single-threaded | same path as `kill` |
| 131 | `tgkill` | requires `tgid == tid`, else `-ESRCH` | `tgkill tgid!=tid -> -ESRCH = -3 ok` |
| 134 | `rt_sigaction` | install/query; `sigsetsize` must be 8; `SIGKILL`/`SIGSTOP` and a missing non-zero restorer give `-EINVAL` (section 7) | `rt_sigaction rc = 0 ok` / `old handler was SIG_DFL = 0 ok` / `no restorer -> -EINVAL = -22 ok` |
| 135 | `rt_sigprocmask` | `BLOCK`/`UNBLOCK`/`SETMASK`; `KILL`/`STOP` can never be masked (section 7) | `query sees blocked bit = 1 ok` / `delivered on unblock = 2 ok` |
| 139 | `rt_sigreturn` | validates frame `magic` and length, then restores the whole frame; privilege level is not taken from the user frame (section 7) | triggered by the restorer; readouts stay correct after the handler returns |
| 160 | `uname` | `sysname=ArkRTOS`, `machine=loongarch64` | `uname -> 0 ok` |
| 168 | `getcpu` | two 4-byte out-pointers (either may be NULL, out-of-window `-EFAULT`) plus a cache argument (ignored); single core, so both the CPU number and the NUMA node number are written as 0 - they must equal the `cpu_id` `rseq` writes back (section 8.14) | `getcpu rc = 0 ok` / `getcpu cpu = 0 ok` / `getcpu node = 0 ok` |
| 172 | `getpid` | internal task id | `getpid==task id>0 -> 1 ok` |
| 174 | `getuid` | always 0 | same path |
| 175 | `geteuid` | always 0 | same path |
| 214 | `brk` | monotonic allocation inside the ELF window | `brk(0)>0` / `brk(+4096) -> 34607104 ok` |
| 215 | `munmap` | returns 0 only, **does not give the address back** | same path |
| 220 | `clone` | fork only: `flags` must be 0 or `SIGCHLD`, `child_stack` must be 0; copies the whole address space and makes a new task, the child returns 0 and the parent the child id (section 8, `ARKRTOS_USE_LINUX_FORK`) | `clone(child_stack!=0) == -EINVAL -> -22 ok` / `parent child=12` / `child pid != parent pid -> 1 ok` / `exit code 0` |
| 221 | `execve` | replace the image of the **calling task**: load segments into its own window, write entry/`sp`/`tp` back into the trap frame; does not return on success | `stage2 pid==stage1 pid -> 11 ok` / `image reloaded (.data marker) -> 1 ok` / `fd 3 survived -> 1 ok` / `heap cursor reset -> 34668544 ok` / `exit code 42` (section 5) |
| 222 | `mmap` | anonymous only: `MAP_PRIVATE|MAP_ANONYMOUS` or `MAP_SHARED|MAP_ANONYMOUS` (the latter needs `ARKRTOS_USE_PAGING_SHM`, section 8.3); `MAP_FIXED` and `PRIVATE|SHARED` together are refused | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |
| 260 | `wait4` | only `options 0/WNOHANG`; **without `WNOHANG` it really blocks** (restartable syscall, section 8.1), never invents a status, the bookkeeping entry is consumed on reap (section 8) | `wait4 returned child pid -> 12 ok` / `WEXITSTATUS == 42 -> 42 ok` / `wait4(child) again == -ECHILD -> -10 ok` / `blocking wait4 blocked >= 40 ms -> 1 ok` |
| 274 | `sched_setattr` | `struct sched_attr` read field by field as bytes (VER0 48 bytes, `size < 48` gives `-EINVAL`); any non-zero syscall `flags` or `sched_flags` gives `-EINVAL`; `DEADLINE` goes to `arkrtos_task_dl_set()`, admission failure `-EBUSY`; the three time parameters must all be 0 on non-deadline policies (section 8.17) | `setattr DEADLINE 100% bandwidth -> EBUSY -> -16 ok` / `setattr DEADLINE 1ms/2ms/100ms is admitted -> 0 ok` |
| 275 | `sched_getattr` | writes back the same layout; with `size >= 56` both util fields are written as 0 (no utilisation tracking, but the fields need a defined value) (section 8.17) | `getattr policy is DEADLINE -> 6 ok` / `getattr DEADLINE deadline round-trips -> 2000000 ok` |
| 226 | `mprotect` | **real page-level write protection** (`ARKRTOS_USE_PAGING_MPROTECT`, section 8.6): flips the `WE` bit at 4 KB granularity, a violating store kills the task with SIGSEGV; `PROT_EXEC` / `PROT_NONE` are recorded but not enforced; with the switch off it falls back to "validate, return 0" | `mprotect RO == 0 -> 0 ok` / `sibling 4 KB half of the same pair stays writable -> 13107 ok` / `mprotect violation reports SIGSEGV status -> 11 ok` (section 8.6, 11 checks) |
| 227 | `msync` | writes back only this process's "writable shared file mapping" bookkeeping; a hit inside the heap window syncs, a miss inside it returns 0, outside it `-ENOMEM` (`ARKRTOS_USE_PAGING_FILEMAP`, section 8.5) | `filemap msync rc -> 0 ok` / `filemap private msync rc (no writeback) -> 0 ok` / `filemap shared write persisted, private write not (bits=31) -> 31 ok` |
| 293 | `rseq` | registration contract (`len` must be 32, `flags` must be 0, 32-byte aligned address, `NULL` deregisters) plus `cpu_id_start`/`cpu_id` written as 0; the two abort points of `rseq_cs` (before signal delivery, before a tick preemption, section 8.14) | `rseq register rc = 0 ok` / `cpu_id readback 0 = 0 ok` / `signal aborted the critical section = 1 ok` / `rseq_cs cleared by the abort = 0 ok` / `exit code 57` |

The socket family (`198-210`, `242`, 13 numbers) is described in section 8.9: it is
implemented in the translation layer and covered by 768 host assertions, but the **QEMU
fixture has no socket section yet**, so it is not counted among the 47 numbers with QEMU
readouts.

Unknown numbers return `-ENOSYS` (38) and are counted. Nothing is guessed.

### 1.1 The one that is table-and-argument only

`pread64`(67) exists for `ld.so`, and this round has **no runtime fixture** for
it: only "the number matches `asm-generic` plus it compiles under the LA64 board
profile".

`mprotect`(226) used to be on that list too (validate the arguments, return 0,
**change no page permission**). Batch C-2 turned it into real TLB edits
(section 8.6), so it now has 11 serial readouts and is no longer a table-only
number.

The other 47 numbers all have serial readouts from the `[linux]` / `[pipe]` /
`sig` / `[dup]` / `[fork]` / `[rseq]` / `[sched]` fixtures.

The twelve scheduling numbers (118-127, 274/275) report under the `[sched]` prefix:
they are the translation-layer evidence of section 8.17, which is a different job
from the "selection order and budget" evidence for the kernel deadline class (52
host checks in `test_deadline_class`). Neither can stand in for the other.

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

`dup`/`dup2` redirection, `socket`/`select`, the fd classes of `futex`
(eleven futex ops plus robust-list cleanup exist behind `ARKRTOS_USE_LINUX_FUTEX`,
default off, sections 8.4 / 8.4b / 8.4c), threads. File-backed `mmap`
exists (section 8.5), page-level write protection exists (section 8.6), and real
`mmap` semantics `MAP_FIXED` exists in the demand tier (section 8.7) - it accepts
anonymous private mappings only. Anonymous `MAP_SHARED`
exists in the SHM tier (section 8.3, default off) and is inherited through `fork`
only. `fork`/`clone` is no longer in this
list: with `ARKRTOS_USE_LINUX_FORK` on (default off, 2026-11-23, QEMU LA64) a
process can split once and be reaped via `wait4` - fork only, no threads
(`CLONE_VM`/`CLONE_THREAD` are `-EINVAL`), see section 8. Signal delivery now
crosses tasks (`kill`/`tkill`/`tgkill` resolve the target by pid, 2026-11-23,
QEMU LA64), but there is still no preemptive delivery, no real-time signal and no
`siginfo` (section 7). fd inheritance exists in the fork tier: each process owns
its fd table and a pipe end holds its own reference count (section 4/8). `futex`
has eleven ops behind a switch (sections 8.4 / 8.4b / 8.4c, default off): the key is the
physical frame plus the offset, since a VA means different frames in parent and
child; WAIT / WAKE / REQUEUE / CMP_REQUEUE / WAIT_BITSET / WAKE_BITSET work, as
do the PI trio LOCK_PI / UNLOCK_PI / TRYLOCK_PI, robust-list cleanup
(`set_robust_list` / `get_robust_list`, syscalls 99/100) and the condition-variable
PI variants WAIT_REQUEUE_PI / CMP_REQUEUE_PI (8.4c). The fd classes are
`-ENOSYS` rather than guessed. See the
Chinese page for the reasoning per row.

`execve` replaces the image of the current task in place (section 5), and
`fork` copies the address space into a second task (section 8). What is still
missing is the rest of the *process* model: no threads, no `dup2`, no job
control; copy-on-write is available behind `ARKRTOS_USE_PAGING_COW` (section
8.2, stack pages private), and the fd table is per-process only in the fork tier.

Dynamic linking no longer belongs in this list: with `ARKRTOS_USE_DYNLOAD` on,
the kernel loads a `PT_INTERP` interpreter and ships a minimal in-tree
`ld-ark.so` (2026-11-20, QEMU LA64). That is not glibc support - see
[动态链接与PT_INTERP](动态链接与PT_INTERP.en.md).

## 4 Deliberate degradations

- **fd table** comes in two tiers. The default tier (`ARKRTOS_USE_LINUX_FORK=0`)
  is kernel-global (`lx_fd_t s_fd[8]`, path + offset) and is reset by the shell's
  `exec` (which starts a *new* task); `execve` keeps the fds opened before the
  call, as Linux does. The fork tier makes it **per process** (`lx_proc_t { fd[8];
  sig[NSIG]; sig_pending; sig_blocked; }`, `static lx_proc_t s_proc[LX_PROC_MAX]`):
  `clone` copies the whole table and bumps both pipe reference counts, and the
  child's `close` only drops its own reference (the pipe object survives on the
  parent side). The implementation is a macro alias (`#define s_fd
  (lx_proc_curp()->fd)`), so the 500+ existing call sites stay untouched.
- **heap**: `brk` and `mmap` share one monotonic region inside the 2 MB ELF
  window (`0x02000000`), leaving 128 KB of stack below the top. A successful
  `execve` resets the cursor: the old cursor's address meaning belongs to the
  old image.
- **`munmap` returns 0 without reclaiming**: pretending to free would leave the
  program using memory another task may own — a silently wrong answer.
- **user pointers are never dereferenced directly**: everything goes through
  `arkrtos_copy_from_user()` / `arkrtos_copy_to_user()`, which judge legality by
  the task's four TCB window fields (`rom_start/rom_size/ram_start/ram_size`).

## 5 execve (221): replacing the image of the same task (2026-11-21, QEMU LA64)

Linux `execve` replaces the current process image and does not return on
success. Ark's window model (one static window per task, with the trap frame
sitting in the user stack) lets it implement the **real** semantics instead of
"start a new task and pretend it was an exec":

- `elf_load()` gained a `start_task` switch: with 0 it only lays the segments
  into the **caller's own window**, builds the user stack and returns
  entry / `sp` / `tp` - no task is created;
- `arkrtos_port_svc_exec_frame()` writes those three values into the **current
  trap frame** (`ERA` / `SP` / `TP`, clears `a0..a7` and `ra`, keeps only the
  PLV bits of `PRMD`) and ends with `dbar 0` + `ibar 0`. Overwriting the same
  VAs without an icache sync is "run half of the old program"; QEMU cannot see
  the difference, real hardware can;
- why the in-place overwrite is safe: `elf_check_range()` confines segment
  writes below `stack_bottom`, while the live stack, trap frame and kernel C
  stack live in `[stack_bottom, argv_start)` and the new argc/argv are written
  above `argv_start`.

Four differences that stay in the title:

1. `envp` is not consumed (this machine has no environment layer; the new image
   receives the same empty table);
2. the **fd table is preserved** (same as Linux, unlike the shell's `exec`,
   which starts a new task and resets it);
3. the old image's heap cursor is void; already-allocated addresses are not
   reclaimed in any way (the old image is gone);
4. **fds marked `FD_CLOEXEC` are now released here** (since 2026-11-27):
   `lx_execve()` calls `lx_fd_close_cloexec()` right after `s_heap_cur = 0`,
   releasing every slot with the bit set. The position is deliberate: segments
   loaded, trap frame patched and heap cursor reset are the one point on the
   path that cannot fail; putting it earlier would mean rolling back someone
   else's fd table on a later failure. The remaining fds survive as before (the
   `fd 3 survived` reading in section 5 still holds - it never carried
   `O_CLOEXEC`).

Argument discipline: `path` and `argv` live in user memory and are overwritten
as soon as loading starts, so they are copied into the kernel first (16 x 256
byte static staging; over the limit returns `-E2BIG`).

### Measured readout (`[execve]` lines, QEMU LA64)

```
[execve] stage1 pid=11
[execve] stage1 fd=3
[execve] stage1 brk(0)=34668544
[execve] stage1 brk(+8192) -> 34676736 ok
[execve] execve(missing) == -ENOENT -> -2 ok
[execve] stage1 calling execve(/mnt/ram/linux_execve)
[execve] stage2 pid=11
[execve] stage2 pid==stage1 pid -> 11 ok
[execve] image reloaded (.data marker) -> 1 ok
[execve] fd 3 survived (ELF magic) -> 1 ok
[execve] stage2 brk(0)=34668544
[execve] heap cursor reset -> 34668544 ok
[execve] PASS: same task, image reloaded, fd kept, heap reset
[execve] finished after 0 ms, exit code 42
```

Each of the four criteria rules out one "looks right" implementation: equal
`pid` rules out "a new task was started"; the `.data` marker returning to its
file initial value rules out "only the trap frame was patched, PT_LOAD was not
rewritten"; fd 3 still reading the ELF magic rules out "the fd table was reset
along the way"; `brk(0)` returning to the base rules out "the old heap cursor
was reused".

**Anti-false-green (measured)**: turning `arkrtos_port_svc_exec_frame()` into
"validate, then `return 0` without touching the frame" flips the readout to
`[execve] FAIL: execve returned 35651560 (success must not return)` + `exit
code 1`. The fixture carries its own second control: it first calls
`execve("/mnt/ram/nope")` and expects `-ENOENT`, after which the program must
keep running - failure is the returnable path.

### Boundaries

- No `fork` / `clone`: that needs address-space copying, and without it there
  is no "who waits for whom" after `execve`.
- No `execveat`, no `FEAT_EXECFD`; the argument limit is Ark's own 16 x 256,
  not Linux `MAX_ARG_STRLEN` semantics.
- Other ports (RV32 / Cortex-M / LA32) have no
  `arkrtos_port_svc_exec_frame`; the weak definition in `hal.h` keeps them
  linking and `execve` returns `-ENOSYS` at run time instead of pretending.

## 6 pipe2 (59): blocking by default, plus an `O_NONBLOCK` mode (2026-11-27, U-26, QEMU LA64)

Programs see Linux's `int pipe2(int fds[2], int flags)`: two fds (read end first)
written back into the user `int[2]`. Ark keeps one reference count per end plus a
ring buffer:

- a static table `s_pipe[2]` (`LX_PIPE_MAX 2`), 256 bytes each (`LX_PIPE_BUF`),
  with `readers` / `writers` counters;
- `lx_fd_t` gained a `kind` field (file / read end / write end) and a `pipe`
  index, so `read` / `write` / `close` dispatch correctly; `execve` keeps the fd
  table, so **pipes survive `execve`** as on Linux;
- transfers are synchronous in-kernel copies of the ring buffer; the data never
  touches ramfs and no thread is created.

### Semantics (blocking by default, matching Linux)

`pipe2(..., 0)` hands out a **blocking** pipe (Linux's default); `O_NONBLOCK` gives
"read/write only as much as fits right now". Blocking is implemented as
"register + replay": the wait is recorded per **task** (`s_pipe_wait[]`), it
sleeps `LX_WAIT_SLICE_MS` (100 ms), and on wake the whole syscall is **replayed**
(the argument registers are left untouched and `done` accumulates inside the wait
slot across replays, so the same bytes are never written twice). Returning
`LX_RET_RETRY` is the same convention as socket / pselect6.

| event | Ark (blocking default) | Ark (`O_NONBLOCK`) | Linux | note |
|---|---|---|---|---|
| read on an empty buffer, writer alive | sleeps until someone writes or a signal arrives | `-EAGAIN` (11) | same as both Ark columns | the peer wakes the waiter actively; the 100 ms slice only covers a lost wakeup |
| read on an empty buffer, all writers closed | returns `0` (EOF), **no sleep** | returns `0` | returns 0 | EOF is a return condition, not a wait condition |
| write into a full buffer | sleeps until the reader drains, then **writes everything** | short write (actual byte count) | same as both Ark columns | a blocking write is complete: a block over 256 B is split and waits for space |
| write with every reader closed | `-EPIPE` (32) plus SIGPIPE raised | same | `SIGPIPE` / `-EPIPE` | decided before copying: free space does not change the answer |
| an unblocked signal that really interrupts | `-EINTR` with no byte moved; short write if progress was made | same | same | signals whose **default action is "ignore"** (SIGCHLD etc.) do not count |
| `flags` carrying `O_CLOEXEC` | accepted, both ends record `FD_CLOEXEC` | same | accepted | `execve` releases those slots after the frame swap (8.13) |
| clear / set `O_NONBLOCK` (`F_SETFL`) | allowed; either end switches freely | same | allowed | since U-26; before that clearing returned `-EINVAL` |

`SIGPIPE` takes the existing path "set pending -> deliver before the syscall
returns to user mode": with no handler the default action ends the task
(`exit code 141`); with a handler it runs once before the return (`a0` = 13) and
after `rt_sigreturn` `a0` is still `-EPIPE`.

Ceiling: `LX_MAX_FD 8` fd slots => at most two pipes (4 slots) plus 4 plain files;
a missing pipe or slot gives `-ENFILE`. If `fds[2]` cannot be written
(`copy_to_user` fails) the whole pipe rolls back with `-EFAULT` - no half-open
pipe is left behind.

### Measured readout (`[pipe]` lines, QEMU LA64, default tier)

```
[pipe] write /mnt/ram/linux_pipe rc=0 size=8832
[pipe] pipe2 rc = 0 ok                     <- this group passes O_NONBLOCK explicitly
[pipe] fds = 3 4
[pipe] fds are two increasing numbers = 1 ok
[pipe] write 5 bytes = 5 ok
[pipe] read 5 bytes = 5 ok
[pipe] content matches = 1 ok
[pipe] read empty -> -EAGAIN = -11 ok
[pipe] close write end = 0 ok
[pipe] read after writer closed -> EOF = 0 ok
[pipe] pipe2(O_CLOEXEC) rc = 0 ok
[pipe] pipe2(O_CLOEXEC) hands out two fds = 1 ok
[pipe] second pipe2 rc = 0 ok
[pipe] write 400 into 256 buffer -> short write = 256 ok
[pipe] read back what fits = 256 ok
[pipe] write again after drain = 256 ok
[pipe] sigaction(SIGPIPE, SIG_IGN) before EPIPE = 0 ok
[pipe] write with reader closed -> -EPIPE = -32 ok
[pipe] blocking pipe2 rc = 0 ok            <- below: flags=0, blocking default
[pipe] blocking write with data present = 5 ok
[pipe] blocking read with data present = 5 ok
[pipe] blocking read content matches = 1 ok
[pipe] blocking close write end = 0 ok
[pipe] blocking read after writer closed -> EOF = 0 ok
[pipe] sigaction(SIGPIPE, SIG_IGN) = 0 ok
[pipe] blocking pipe2 (EPIPE) rc = 0 ok
[pipe] blocking write, reader closed, SIG_IGN -> -EPIPE = -32 ok
[pipe] sigaction(SIGPIPE, handler) = 0 ok
[pipe] blocking write, reader closed, handler -> -EPIPE = -32 ok
[pipe] SIGPIPE handler ran once = 1 ok
[pipe] SIGPIPE handler got signo = 13 ok
[pipe] blocking close write end (EPIPE pipe) = 0 ok
[pipe] PASS: pipe2 two fds, data round trip, EAGAIN on empty, EOF on writer close, O_CLOEXEC accepted, short write and EPIPE, blocking defaults and SIGPIPE delivery
[pipe] finished after 0 ms, exit code 55
```

30 `check()` calls in `example/qemu_loongarch64/elf/linux_pipe.c` (a pure Linux
ABI static program with its own `_start`). The first 16 are the existing
`O_NONBLOCK` readouts (two increasing fds, byte-exact round trip, `-EAGAIN` on an
empty read, EOF when the writer closes, `O_CLOEXEC` accepted, short write when
full, `-EPIPE` with the reader closed - that one now has to `SIG_IGN` first,
otherwise the default action 141 ends the task). The last 14 are what the
blocking mode **can prove on a single execution stream**: a blocking read with
data present returns at once, a blocking read sees EOF once the writer is closed,
`-EPIPE` under `SIG_IGN` does not hang, and with a handler installed `SIGPIPE` is
really delivered once before the syscall returns.

"Empty read suspends until someone writes" and "full write suspends until someone
drains" **cannot be proven on a single execution stream** (once suspended it never
comes back). Those readouts live in the fork tier's `[fork]` block (section 8.15).

### Boundaries

- 256-byte buffer, `LX_PIPE_MAX 2` pipes, 8 fd slots.
- The wait granularity is a 100 ms slice: the peer always wakes the waiter
  actively; the slice only covers "preempted between registering and sleeping, so
  the wakeup was lost", hence the worst case is one extra slice, never a
  permanent stall.
- Blocking needs the port's `arkrtos_port_svc_restart()` (argument replay). A
  port without it never pretends: reads return `-ENOSYS`, writes return the
  **bytes already written** (short write) - data that made it into the ring
  buffer is never reported as unwritten.
- `-EINTR` is a defensive path: this layer delivers deliverable signals before
  returning to user mode, so it is hard to hit; signals whose default action is
  "ignore" (SIGCHLD/SIGCONT/SIGURG/SIGWINCH) **do not** interrupt.
- Single core only: cross-task blocking reads were only exercised on QEMU LA64
  single-core; "two cores sleeping on the same pipe" is unverified on SMP.
- `SIGPIPE` covers write / the write end; `siginfo` and `SA_RESTART` semantics are
  absent (the signal layer is still the minimal tier, section 7).

## 7 Signals (`rt_sigaction` 134 / `rt_sigprocmask` 135 / `kill` 129 / `tkill` 130 / `tgkill` 131 / `rt_sigreturn` 139)

Landed 2026-11-21, measured on QEMU LA64 (`example/qemu_loongarch64/elf/linux_signal.c`,
31 `[sig]` checks plus a PASS line and `exit code 138`).

### Semantics (how far it goes, stated plainly)

| item | status |
|---|---|
| delivery target | **self or, in the fork tier, another process**: `kill`/`tkill`/`tgkill` resolve the pid to a process slot and its task (2026-11-23). A pid that maps to no live task gives `-ESRCH`; `tgkill` additionally wants `tgid == tid` |
| numbers | 1..31; 0 is an existence probe (returns 0), >= 32 gives `-EINVAL`; no real-time signals |
| delivery point | **before the syscall returns to user mode** (the single exit of the dispatcher). No preemptive delivery: a signal only arrives the next time the program enters the kernel |
| handler address | must be a fetchable user address (`copy_from_user(..., 4)` probes it first) |
| restorer | **required** and non-zero, otherwise `rt_sigaction` gives `-EINVAL` and the old action is not installed - there is no "where to return" path and the address is never guessed |
| signal frame | 32-byte header (`magic` / `signo` / saved `blocked` / frame length) + **a snapshot of the whole trap frame** (256 / 528 / 1040 B, the port reports the length; 528 on this board's FPU profile) |
| placement | computed by the port: `fsp = (current kernel sp - frame - 128) & ~15`, then checked against the task stack bottom (64 B margin) |
| `sigreturn` | validates `magic` and frame length, restores the whole frame; **the privilege level in the user frame is ignored** (no privilege escalation) |
| default actions | ignored {17,18,23,28}; stop-family {19,20,21,22} treated as ignored and counted separately (there is nothing to stop); everything else `exit(128 + signo)` |
| `SIG_IGN` | really dropped and counted |
| `SIGKILL` / `SIGSTOP` | cannot be installed (`-EINVAL`) and cannot be masked (the bits are cleared in `rt_sigprocmask`) |

### Why the port computes the frame address from the *current kernel sp*

This is the only trap in this round that **silently destroys the live context**.
`eentry` here does `addi.d $sp,$sp,-528` and builds the trap frame directly on the
**interrupted task's own stack** - there is no separate exception stack. The SVC
handling chain (C code) therefore lives below the user stack. The first version
computed the signal frame from "the user sp stored in the frame", which pointed
into the stack the kernel C calls were using: `copy_to_user` overwrote its own
return address, so the moment the copy returned the PC was 0 and the machine
reported `INE` (`ecode=0x0d`) before a single check could print.

The fix uses the **current kernel sp** (`move %0, $sp`) as the base, moves down
by `frame + 128`, and keeps a 64 B margin above the task stack bottom. The return
chain only pops and never pushes, so that location is safe for the kernel.

Readout: before the fix `FATAL estat=0x000d0800 ecode=0x0d(INE) era=0`; after it
31/31 checks pass with `exit code 138`.

### What the 31 checks are

Besides the action/old-action, `kill` return value across the handler, hits/signo,
nesting (hits/`depth`/rc), block/query/unblock, the six `-EINVAL`/`-ESRCH`
rejections, `SIG_IGN` silence and the final `SIG_DFL` exit, the last two checks
cover the **floating-point register area**: the `$f0` seen inside the handler is
still the pattern the main flow loaded, and after `sigreturn` the whole frame is
restored, so `$f0` is that pattern again.

The LSX profile (`EXTRA_CFLAGS=-DARKRTOS_USE_LSX=1`) is also 31 checks, but the
last two are the **vector area** instead: the `$vr0` seen inside the handler is
the 128-bit pattern the main flow loaded, and after `sigreturn` it is restored.
The two pairs **cannot coexist**: `$f0` *is* the low 64 bits of `$vr0`, so
"write pattern B into `$f0`" inside the handler also rewrites the low half of
`$vr0`. The first version did exactly that and all four checks went red while the
kernel was correct all along (same kernel, fixture rebuilt without `-mlsx`,
instantly green). The LSX profile therefore only probes `$vr0`, whose 128-bit
pattern already covers the low 64 bits (`$f0`).

### Anti-false-green (four mutations, all rejected by the gate)

1. Commenting out the delivery point at the dispatcher tail gives gate exit=1:
   the check count becomes 21 != 31 (the gate constant was still 29 back then
   and reported 21 != 29), and both the PASS line and `exit code 138` are
   missing.
2. Making `frame_import` restore only `ERA`/`SP` gives gate exit=1:
   `old handler was SIG_DFL = 33554800 FAIL`, `rt_sigprocmask unblock rc = 12
   FAIL`, `[sig] FAIL: 2 checks failed`, `exit code 1`. This proves the full
   snapshot is not decoration - drop a register and the handler returns into a
   broken context.
3. (LSX profile) Making `arkrtos_port_svc_frame_bytes()` always return 528, so
   the delivered snapshot carries no vector area: `lsx vr0 seen in handler = 1
   ok` but `lsx vr0 restored after sigreturn = 0 FAIL`, `[sig] FAIL: 1 checks
   failed`, `exit code 1`. So the "restored after sigreturn" check really does
   discriminate the *upper half of the vector area*, it does not ride along on
   the first half.
4. (LSX profile) Pointing `vld $vr0, \base, 528` in `LA_FP_RESTORE` at `512`
   instead (the FCSR slot) turns both checks red (`seen = 0`, `restored = 0`)
   and additionally `[lsx] A bad=50 | B bad=50 -> FAIL (vector regs leaked
   between tasks)` - the same restore path is broken for task switches too,
   which is evidence that the red comes from that kernel instruction and not
   from the fixture.

Reverting all of them brings back 31/31 ok on both profiles, `exit code 138`,
a 13/13 green gate and a format gate clean on 392 files.

### Boundaries

- **Cross-task delivery exists** (2026-11-23): `kill`/`tkill`/`tgkill` set the
target's `sig_pending`; if the target is parked in a blocking `wait4`,
`arkrtos_task_wakeup_internal()` wakes it, and the signal is delivered on the way
out of its next syscall. Self-delivery still goes through the same path.
- **No preemptive delivery**: only at the syscall return point. A program that
  never enters the kernel keeps the signal pending (`kill` returns 0, no handler).
- No real-time signals, no `sigaltstack`, no `siginfo`, no `SA_RESTART`
  semantics (an interrupted syscall is not retried).
- Stop-family defaults are treated as ignored: there is no other process to stop.
- The frame includes the FPU area (measured on the 528 B profile); with the LSX
  profile the whole 1040 B block is copied too and the **vector area is measured
  separately** (two checks above). LASX (`$xr0..$xr31`) is not saved.

## 8 fork (220 `clone`) + wait4 (260): parent and child (2026-11-23, QEMU LA64)

`ARKRTOS_USE_LINUX_FORK` (default off) connects "one process becomes two":
`clone` copies the whole address space and creates a new task, the child resumes
at the return point and reads 0, the parent reads the child id; `wait4` hands the
exit code back. It depends on three layers and `features.h` raises `#error` if
any is missing: `ARKRTOS_USE_LINUX_ABI` (numbers), `ARKRTOS_USE_EXIT_CODE`
(exit-code reporting), `ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND`
(copyable page window and the deferred window switch U-2).

| item | state |
|---|---|
| accepted `clone` arguments | `flags == 0` or `SIGCHLD(17)`, and `child_stack == 0`; everything else is `-EINVAL` (no `CLONE_VM`, hence no threads) |
| address space | `arkrtos_paging_space_clone()` copies the parent 8 KB pair by pair; the child gets its **own** frame-pool space, writes are not visible across |
| child's starting point | a copy of the parent's trap frame with `a0` (the return slot) zeroed, so the child continues after `clone` and reads 0 |
| task id | a fresh TCB (`is_priv = 0`); `generation` tells a reused task number from the old instance; `getpid` reads `current_task_id` |
| ordering | copy space and frame first, then register the task and patch its TCB inside a critical section (same reason Linux `copy_process` calls `wake_up_new_task` last: a half-configured task must never enter the ready set) |
| accepted `wait4` arguments | `options == 0` or `WNOHANG(1)`; `pid` must be a child this process registered |
| `wait4` returns | child alive + `WNOHANG` -> `0`; child alive without `WNOHANG` -> **sleeps until woken** (restartable syscall, section 8.1); not our child -> `-ECHILD`; child dead but exit code never reported -> `-EIO` (never invent 0 or SIGKILL); after the 100 ms fallback timeout a rescan that still finds no exit code for a dead child -> `-EIO` |
| `status` | `(code & 0xFF) << 8`, written as 4 bytes via `copy_to_user`; a failed copy returns `-EFAULT` and **keeps** the bookkeeping (retryable) |
| bookkeeping | `s_child[4]` (`LX_MAX_CHILD`); `used = 0` after a successful reap; a reused slot is recognised by `generation` |
| practical ceiling | four bookkeeping entries, but the real gate is the number of address spaces (`ARKRTOS_PAGING_APPS_MAX 2` on this board): parent plus child fill both, a further `clone` returns `-EAGAIN` (`ARKRTOS_ELF_ERR_BUSY` -> `-EAGAIN`: a concurrency limit, not an out-of-memory) |

With the switch off, `arkrtos_elf_fork` / `arkrtos_port_paging_space_clone` /
`arkrtos_port_paging_write_space` / `arkrtos_port_paging_clone_frame` are not
compiled at all and `nm` shows zero symbols (gate item B).

### Why the deferred window switch (U-2) has to hold here

Both `clone` and `wait4` run while the parent is **standing in its own window**:
the frame it needs to resume from (and the return address) sits on the very same
VA. So `arkrtos_mpu_set_app()` only records `s_pg_pending_space`, and the real
`activate()` is performed by `la_switch_prepare()` after the assembly has moved
`sp` onto a kernel static stack.

### A real defect found here: frame-address equality cannot mean "no switch"

The first version of `la_switch_prepare()` treated `new_frame == old_frame` as
"the scheduler handed the frame back", and that breaks on `fork`: **parent and
child share the same virtual addresses**, so the parent's trap frame inside
`wait4` and the child's saved frame can sit at the identical address (measured:
both `0x21fbdc0`). The switch was then judged a no-op, the window stayed on the
parent's space, yet the child kept running inside that parent window - every
`write` came back `-14` (`-EFAULT`, because `space_of(child) != current window
space`) and the child then continued along the parent's code while the parent
looked hung. It failed about one run in three, and the failing runs never printed
`TASK: task 12 exits`.

Fix: an explicit `s_pg_commit` flag - set at the only two places that really
decide a switch (`la_trap_c()` exit and `la_switch_inline()` after the frame gate)
and consumed/cleared by `la_switch_prepare()`, which now skips when
`commit == 0`. After the change 6/6 + 3/3 consecutive runs pass and `rc=-14` is
gone. The lesson matches how Linux and RT-Thread schedule: **"did it switch" must
be answered by the point that made the decision, never inferred afterwards from
an external reading that may just happen to be equal**.

### 8.1 Blocking `wait4` via a restartable syscall (Linux `ERESTARTSYS` shape)

The first attempt - "sleep a slice, then rescan" - deadlocked the machine: this
kernel has **no true sleep path inside an SVC handler**. `arkrtos_task_block_in_critical()`
only sets WAIT, removes the task from the ready set, arms the delayed wakeup and
sets `s_switch_pending`; the actual switch is settled at the end of `la_trap_c()`.
Called from an SVC it merely removes the parent from the ready set and keeps
spinning with interrupts off, so the child never ran. The symptom was the log
stopping at `dbg parent entering blocking wait4` with no `TASK: ... exits` line.

The fix copies Linux: the syscall **returns after going to sleep and is replayed**.

- New port primitive `arkrtos_port_svc_restart(void *p_exc_ctx)`: the LA64
  implementation subtracts 4 from `ERA`, i.e. backs up onto the `syscall`
  instruction, so the next time the task is scheduled it re-executes the same
  `syscall`; the weak definition in `hal.h` returns `-1` (unsupported) and the ABI
  layer then reports `-EAGAIN` - it never pretends it blocked.
- Argument registers are untouched in the trap frame, so the replay reads the
  same `pid`/`status`/`options`.
- Two wake paths: (1) `arkrtos_linux_abi_on_task_exit()` sets the parent's
  `SIGCHLD` pending and calls `arkrtos_task_wakeup_internal(parent)`; (2) a
  100 ms fallback timeout (`LX_WAIT_SLICE_MS`) for a child that died on a fault
  path and never ran the exit hook - the rescan then finds "dead but no exit
  code" and returns `-EIO`.
- `s_lx_wait[task]` marks "parked in wait4"; cross-task `kill` consults it when
  deciding whether to wake the target.

The cost is stated plainly: this replays the whole syscall instead of resuming at
the sleep point. That is semantically equivalent (`wait4` is an idempotent scan)
but every wakeup rescans the child table, and no other blocking syscall (empty
`read`) is wired to this path; `futex` WAIT uses the same replay mechanism
(section 8.4).

### 8.2 Copy-on-write (`ARKRTOS_USE_PAGING_COW`, 2026-11-23, QEMU LA64)

By default `fork` still clones the whole address space pair by pair. With
`ARKRTOS_USE_PAGING_COW` on, `space_clone()` moves no bytes at all: every
resident 8 KB pair of the parent is pointed at by the child (refcount 1->2) and
the parent's shared entries are dropped to read-only (WE=0). Whichever side
writes first hits a read-only entry, `la_pg_cow_fault()` copies that pair,
installs a writable entry and `ertn`s back to the faulting instruction. The
boundaries are fixed:

| Item | Status |
|---|---|
| Granularity | **8 KB pair**: one PS=12 entry carries lo0/lo1 and a single WE bit, so "one page writable, one read-only" cannot be expressed; we copy 4 KB more, never show one byte more |
| Refcount | per 4 KB frame (the first-fit frame allocator does not guarantee 8 KB alignment); the even frame number of a pair is its handle; **`ref > 1` means shared**, there is no separate boolean (an extra bit is one more place to disagree with the count) |
| Read-only entry | `LA_PTE_APP_RO` = WE cleared; both fault codes are accepted: V=0 is PIS(2), **V=1 with WE=0 is PME(4)** (QEMU LA64 reports 4), and PLV0 is treated exactly like PLV3 |
| Stack pages | **forced private**: at fork the child gets an immediate copy (`stack=<n>` in the log counts those pairs). See below; `copied=0` still holds (`copied` only counts swap-slot materialisation) |
| Refill | the demand tier drops the whole window translation on every space switch, so a shared pair must be refilled **read-only** - otherwise COW survives only for the instant of the fork |
| Ceiling | without swap, COW is only "deferred copy": once both sides have written, both hold real frames, and 2 address spaces x 2 MB are still bounded by the 2048-frame pool |

**Why the stack must be private.** The exception entry `eentry` starts with
`addi.d $sp,$sp,-256` and writes the general registers into that frame - **the
trap frame lives on the App's own stack** (unlike Linux, which switches to a
kernel stack first). If the stack pair were shared read-only, that PLV0 write is
a PME and **the frame has not landed yet**: the C handler (`la_trap_c`) never
runs, the nested exception stops the machine (measured: `FATAL ecode=0x4` with
ERA inside `eentry`, followed by `[STK] ... stack bottom flag overwritten`). So
fork immediately copies every pair overlapping the task stack for the child and
leaves the parent writable; neither side ever has a "shared read-only" window.
This is not a tunable: it exists whenever COW is on, and removing it requires
giving traps their own kernel stack first.

Readouts (`tools/fork_gate.py`, fork+COW tier): 52 checks all `ok`, 5 of them in
the COW section - `cow child saw pre-fork values (bits=3) -> 3 ok` (the child
still sees the pre-fork values) and `parent cow write kept -> 48879 ok` (the
parent's write stays in its own copy). Four kernel-log assertions: at least one
`[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>` with `shared>=1`,
`stack>=1`, `copied==0`; at least one `[LA64U] pg cow copy idx=<n> ref=2`; no
copy line with `ref=1` (copying an unshared pair is moving bytes for nothing);
and the unrelated demos still intact. The gate runs three rounds (fork tier 47
checks, fork+COW tier 52, fork+COW+SHM tier 60) and **never removes the default
whole-clone path from testing**.

Two real root causes were found here, both invisible to the compiler and to
static checks: (1) the demand tier drops all window translations on a space
switch, so a refill that always installs writable leaves COW alive only for the
instant of the fork (the symptom was the parent's globals being dirtied by the
child, with not a single `pg cow fault` printed); (2) writing a read-only page
is **PME(4)**, not PIS(2), and accepting only 2 pushed it into the fatal path.

### 8.3 Anonymous `MAP_SHARED` pages (`ARKRTOS_USE_PAGING_SHM`, 2026-11-23, QEMU LA64)

COW only covers "both sides write their own copy after the fork". For parent and
child to see each other's words there has to be an explicit shared path. With
`ARKRTOS_USE_PAGING_SHM` (stacked on top of the COW tier, enforced by `#error`),
`mmap(MAP_SHARED|MAP_ANONYMOUS)` builds every 8 KB pair as a real frame, zeroes it
and marks it shared **at mmap time**:

| Item | Current state |
|---|---|
| Granularity | still an **8 KB pair**: a 4 KB shared mapping owns a whole pair, and putting a private mapping into the same pair is refused wholesale (`-ENOMEM`) |
| When pages are made | **at mmap time**. Deferring to the fork lets both sides fault in their own frames, which turns "shared" into two private allocations - that implementation reports no error at all, it just silently stops sharing |
| Bookkeeping | a separate table (`s_pg_pair_shr[space][idx]`). `ref > 1` only says "somebody shares it", it cannot tell "shared because COW copied it" from "shared on purpose" |
| fork | a shared pair only gets `ref++`: not dropped to read-only, not stack-privatised, not copied; refill and space install put the entry back writable; the write fault and `write_space` skip the pair |
| Inheritance | inherited through `fork` only. No `shm_open`; a shared mapping is always anonymous. With the switch off `MAP_SHARED` is always `-ENOSYS` |
| Refused forms | missing `MAP_ANONYMOUS`, `PRIVATE\|SHARED` together, and `MAP_SHARED` with `MAP_FIXED` are all `-ENOSYS` (this kernel's rule: unsupported means `ENOSYS`, not `EINVAL`) |

Readouts (`tools/fork_gate.py`, fork+COW+SHM tier, 60 checks): the SHM section has
8 - two refused forms returning `-38`, `mmap(MAP_SHARED|ANON) > 0`,
`shared page starts zeroed -> 0`, `clone for shm child > 0`,
`parent saw child shared write -> 300`, `shm child exit reaped -> 12` and
`shm child saw parent post-fork write (bits=1) -> 1`. The kernel log adds at least
one `[LA64U] pg shm map va=0x<hex> pairs=<n>` with `pairs >= 1`. In the same run
the `shared` count of `[LA64U] pg cow fork shared=<n> ...` is one pair larger than
in the pure COW tier (a shared pair is a shared pair by any name).

**Why the shared bit needs its own table.** "`ref > 1` means shared" only holds for
COW: a `MAP_SHARED` pair with exactly a parent and a child also has `ref == 2`, but
its first write **must not** copy (copying would split the sharing). It cannot be
derived from the refcount, so it is stored separately.

**One real defect fixed on the way: `write(1/2)` did not own the console.** With
SHM in place the child's COW log could land between the parent's `[fork] ... -> 1`
and the trailing ` ok`, which made the gate look like "one readout is missing" -
none was missing, the line had been cut in half. `lx_write` now holds
`arkrtos_console_lock()` for the whole call, and the kernel log takes the same lock,
so the two no longer splice each other.

**The order the readout depends on is constructed, not gambled.** "Parent writes
after fork, child reads it" used to let the child spin 4M times in a pure user
loop, betting that the parent would get the CPU during that spin. Right after
`clone` the child usually runs first, and a user-mode spin loop has no yield
point, so the bet loses now and then: `bits=0` is read, which looks like "the
shared pair was privately copied" but is really the fixture gambling on
scheduling. The child now **polls** a **release pipe** - pipes in this kernel are
blocking by default since U-26 (an empty read suspends until someone writes, section 6), so it keeps
reading until the parent's byte is actually taken - and the parent writes 200
before sending that byte; "parent saw
the child's 300" is read **after `wait4` returns**, when the child is gone, so
there is no race. The same trick orders the blocking `wait4` section: the parent
reads its start timestamp before releasing the child to sleep 60 ms - otherwise
"the child already ran part of its sleep" occasionally reads as
`blocking wait4 waited 2 ms`, which looks like wait4 did not block. With both
fixed the fork+cow+shm tier is stable 3/3 and the default tier 5/5.

### 8.4 `futex`: eleven ops (`ARKRTOS_USE_LINUX_FUTEX`, 2026-11-23, extended 2026-11-24, 2026-11-26 and 2026-11-27, QEMU LA64)

`clone` creates a second execution stream and `MAP_SHARED` lets both sides see the
same memory, but "sleep until this word changes" was still missing - without it
not one pthread mutex, condvar or semaphore can start. futex is that brick: the
kernel does not synchronise for you, it only parks a task while the word still
matches and wakes it when someone asks, so the common path stays in user mode.

With `ARKRTOS_USE_LINUX_FUTEX` on (`depends on ARKRTOS_USE_LINUX_ABI`), number 98
lands in four batches, eleven ops in total. The first (2026-11-23) is the minimal
tier: `FUTEX_WAIT` / `FUTEX_WAKE`. The second (2026-11-24) adds what the glibc
condvar actually walks: `FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE` /
`FUTEX_WAIT_BITSET` / `FUTEX_WAKE_BITSET`, plus `timeout` and `-ETIMEDOUT`. The
third (2026-11-26) adds the PI trio the default glibc mutex walks: `FUTEX_LOCK_PI`
/ `FUTEX_UNLOCK_PI` / `FUTEX_TRYLOCK_PI` (see 8.4b). The fourth (2026-11-27) is
**PI mutexes paired with condition variables**: `FUTEX_WAIT_REQUEUE_PI` /
`FUTEX_CMP_REQUEUE_PI` (see 8.4c). Missing any of
`WAIT_BITSET|CLOCK_REALTIME` + `CMP_REQUEUE` leaves pthread condvars unable to
run; missing the PI trio stops every default `pthread_mutex` dead - so each batch
was done whole rather than piece by piece.

| Item | Current state |
|---|---|
| ops | `WAIT`(0) / `WAKE`(1) / `REQUEUE`(3) / `CMP_REQUEUE`(4) / `WAIT_BITSET`(9) / `WAKE_BITSET`(10). `FUTEX_PRIVATE_FLAG`(128) is **accepted and stripped** - the key is physical anyway, so private and inter-process are the same thing here; `FUTEX_CLOCK_REALTIME`(256) is only legal with `WAIT_BITSET`, and since this machine has no wall clock it honestly returns `-ENOSYS` **when a timeout is present** (uptime is never passed off as an absolute instant); the PI trio `LOCK_PI`(6)/`UNLOCK_PI`(7)/`TRYLOCK_PI`(8) is covered in 8.4b and the
condition-variable PI variants `WAIT_REQUEUE_PI`(11)/`CMP_REQUEUE_PI`(12) in 8.4c;
still honestly `-ENOSYS` are `FUTEX_FD`(2), `WAKE_OP`(5), 13/14 and the fd
classes - no guessing |
| key | **physical frame number + offset in page**, not the VA. Each process owns its own 8 KB pair window, so the same VA names different frames in parent and child; a VA key would let two unrelated processes wake each other on the same address - a bug that reports no error and only wakes one waiter too many. Outside the window (stack, kernel direct map) no frame resolves, so it is `-ENOSYS` rather than a fake VA key |
| sleep | no infinite sleep exists here: the task registers, calls `arkrtos_task_block_in_critical(LX_WAIT_SLICE_MS = 100 ms)` and `arkrtos_port_svc_restart()`, the same restartable-syscall path as 8.1. On wake it checks `s_futex_woke` first (someone called, return 0), then the registered deadline (reached means `-ETIMEDOUT`), otherwise re-reads the word: changed means woken, unchanged means another slice. **Without a timeout it never times out** (as in Linux). The timeout is stored as a **deadline**, not a duration: a relative timeout counts from the first entry only, because re-adding it on every slice replay would never expire. `WAIT` takes the timeout as **relative**, `WAIT_BITSET` as an **absolute** instant (as in Linux; this kernel's clock is monotonic milliseconds). A port without restart support returns `-ENOSYS` and clears the slot - it never pretends it slept. On a replay `a0` has already been clobbered to 0 by the return value (trap 2), so which word to read and what value to expect come from the registered copy, never from the replayed arguments |
| `-EAGAIN` / `-EINTR` | word != `val` at entry gives `-EAGAIN` at once; an unblocked pending signal gives `-EINTR` at once, and the replay checks again so a signal can break a sleeper out |
| wait slot | one per task (`s_futex_key / s_futex_used / s_futex_woke / s_futex_uaddr / s_futex_val / s_futex_bitset / s_futex_deadline / s_futex_timed[ARKRTOS_TASK_MAX_NUM]`). `uaddr`/`val` remember the address and expected value of the last real sleep, because on a replay `a0` has been clobbered to 0 and without them the only thing left to read is address 0; `bitset` remembers the bit mask (a plain WAIT registers all ones, so a plain `WAKE` can wake it), `deadline` the instant, and `timed` whether a timeout exists (a **separate flag, never 0 as a sentinel**, since the monotonic clock starts at 0 and 0 is a legal deadline). A sleeping task cannot start a second WAIT, so one slot is enough; `on_task_exit` clears all eight, otherwise a reused task id would inherit an "already waiting" record |
| `WAKE` / `WAKE_BITSET` | scans every task; for each registered one on the same key it sets `s_futex_woke = 1` and calls `arkrtos_task_wakeup_internal()`, up to `val` of them, returning how many were woken; no waiter returns 0 (as in Linux). `WAKE_BITSET` additionally filters on the `val3` mask: an empty mask is `-EINVAL`, and only waiters with `bitset & mask` non-zero are woken; a plain `WAKE` ignores bit masks, so it can wake a bitset sleeper (as in Linux) |
| `REQUEUE` / `CMP_REQUEUE` | Linux packs `nr_requeue` **into the `timeout` slot** (argument 4) and `uaddr2` into argument 5, and this copies that reuse: wake `val` of them first, then re-queue at most `nr_requeue` of the rest onto `uaddr2`, returning **the number woken plus re-queued** (man futex: "woken up or requeued"). Linux callers rely on that count to tell whether the requeue happened at all; a `CMP_REQUEUE` with `nr_wake=0` that returned only 0 would leave glibc's `pthread_cond_broadcast` unable to decide. `CMP_REQUEUE` first compares the word at `uaddr` with `val3` and returns `-EAGAIN` on a mismatch; a missing `uaddr2` is `-EINVAL`. A re-queued waiter has its **expected value updated to the current word at `uaddr2`**: requeue never wakes in Linux, and this kernel returns to user mode through a slice replay, so without that update the replay would see two different words and treat it as "woken" - a deliberate deviation inside the model, written down in the code |
| check order | a waking WAIT decides in this order: someone `WAKE`d it -> the deadline passed -> the word changed (counts as woken) -> an unblocked signal (`-EINTR`) -> sleep another slice. The order is the priority: `WAKE` beats the timeout, so a call that is both woken and expired at the same instant returns 0 |

Readouts (`tools/fork_gate.py`, fork+COW+SHM+futex tier, **147 checks**, 87 of them
futex - 9 basic pairing, 7 multi-slice, 20 for the second batch (bitsets, timeout,
requeue), 37 for the fourth batch (PI trio and robust list, see 8.4b) and 14 for
the fifth batch (PI condition variables, see 8.4c)): the
shared page is mapped (`> 0`), `WAKE` with no waiter returns 0,
`WAIT` on a different value returns `-11`, `LOCK_PI` on a free word returns 0 (the
old readout here asserted "an unknown op returns `-38`" with `LOCK_PI` as the
example; once PI landed that assertion would fail on correct code and was removed),
the waker is a real cloned process, the parent's `WAIT` returns 0 after being woken, the
parent sees the `2` the waker wrote, the waker is reaped, and
`futex waker saw 1 and woke 1 (bits=3)` carries both evidence bits: bit 0 = the
child read the same `1` the parent was about to wait on (so parent and child
really share the word), bit 1 = `WAKE` really woke one waiter.

The multi-slice group adds seven more: its own shared page and clone, a `WAIT`
that crosses at least one 100 ms slice and still returns 0, the parent seeing the
`2` the waker wrote, an elapsed wall clock >= 150 ms between entering and leaving
`WAIT` (proof it really crossed a slice instead of matching once), the waker
reaped, and `bits == 3` again in the multi-slice scenario.

The second batch adds twenty. Nine are single-process argument and semantics
readouts: an empty bitset is `-EINVAL` on both `WAIT_BITSET` and `WAKE_BITSET`,
`WAKE_BITSET` with no waiter returns 0, a zero relative timeout and an absolute
deadline already in the past both return `-ETIMEDOUT` (`-110`), `REQUEUE` with no
waiter returns 0, `CMP_REQUEUE` with a mismatched expected value returns
`-EAGAIN`, and a `REQUEUE` without `uaddr2` returns `-EINVAL`. Two more run a
real 50 ms relative timeout (the return code and an elapsed wall clock >= 50 ms,
so a mere parameter check cannot pass). Four are cross-process bitsets: the parent
sleeps on mask 2, the child first calls `WAKE_BITSET(1)` and **must get 0**
(disjoint masks do not wake it), then loops `WAKE_BITSET(2)` until it gets 1; the
readout is the parent returning 0 plus `bits == 7`, where bit 1 is exactly "the
disjoint mask did not wake it" - if bitsets were ignored, the child's first call
would have woken the parent and that bit could never be set. Five are
cross-process requeue: the parent sleeps on `q`, the child calls
`CMP_REQUEUE(q -> q+8, expected 1, nr_wake 0, nr_requeue 1)` and then `WAKE`s the
new address; the parent returns 0 on the re-queued word, a `WAKE` on the old
address returns 0 (the waiter really moved instead of waking on both), and
`bits == 3` again. The two "child reaped" rows are checked by shape, not by pid,
because task ids are reused. Before the fix in trap
2 this WAIT died with `-EFAULT` on its first slice expiry, because the replay read
address 0; the child busy-waits 250 ms, i.e. two slices, before writing the value
and calling `WAKE`, so the board scheduler itself exercises the replay path.

The ordering here **retries until it converges** instead of assuming a schedule:
user mode can be preempted by the time slice, so the child may finish before the
parent ever reaches `WAIT` (this was actually observed once: parent `WAIT`
returned `-EAGAIN`, child `WAKE` returned 0, which looks like broken pairing).
The child now loops "write 2 + `WAKE`" until it gets 1, and the parent re-sleeps
on the current value when it was overtaken; if futex pairing were really broken
neither side would converge and the readout stays red. "The parent really slept"
is proven by the `WAIT` returning 0 line, not by bit 0.

**Three traps only a board run exposes.**

(1) **The 8 KB pair granularity of a shared mapping eats address space.** A 4 KB
`MAP_SHARED` also owns a whole pair, so the mapping must start on a pair boundary
and the heap cursor must advance by whole pairs. The first version advanced by
4 KB, so the second shared mapping landed in the other half of the first pair and
was refused wholesale as `-ENOMEM` - which reads like "out of memory" but is
really alignment. `mmap` now does `lx_pair_up(start)` for the shared branch and
sets `s_heap_cur = lx_pair_up(end)` (the leftover 4 KB hole is not returned to the
allocator: handing it out would overlap the shared frames).

(2) **The return register is the first argument register.** On the LA64 Linux ABI
the syscall result is written to `a0`, and `futex` reads its first argument
(`uaddr`) from `a0` too. When the blocking path returns 0 the dispatcher writes 0
into `a0`, so **the replay reads `uaddr == 0`**. The replay cannot be recognised by
address, only by "this slot is still registered": `uaddr == 0` plus `used == 1`
takes back the key the task recorded. This is not theoretical tidiness - the
`wait4` replay reads `pid == 0` too, but 0 is a legal `wait4` input (wait for any
child), so it never showed; futex is the first place that has to say it out loud.
A host case feeds exactly this shape: register a WAIT, then re-enter with
`uaddr = 0`; it must be taken as a replay and keep sleeping on the registered
address, not `-EFAULT`.

**That step was wrong at first, and the wrong answer had been written down as the
expected value.** The first version recognised the replay but still read the word
with the replayed `uaddr` (= 0), so `copy_from_user` always failed with `-EFAULT`
and the slot was dropped. A WAIT that is not woken within 100 ms - the most normal
futex use there is - therefore died with an error code. It never showed on the
board fixture because the basic pairing group has the child write and `WAKE`
almost immediately, so the parent is usually woken inside one slice; and the host
case asserted `-EFAULT` as if it were correct, freezing the bug into the test.
The fix is `s_futex_uaddr` / `s_futex_val`: store the real address and expected
value when registering, take them back on replay. The host case now asserts
"keeps sleeping / value changed counts as woken / a genuinely unreadable
registered address is `-EFAULT`" and the board's multi-slice group pins the same
behaviour to the real scheduler. It is worth its own entry because the shape is
"**works before and after, and returns a plausible errno either way**" - you only
catch it by treating "waits a long time" as a normal path.

**(2b) The replay test cannot be "the address matches".** Fixing "which word does
the replay read" left a sibling trap: `REQUEUE` **moves the slot's key**
(`s_futex_key` becomes `uaddr2`), while the replay still computes its key from the
old `uaddr` the caller wrote. `resumed` used to mean "the slot's key equals the key
computed now", so **a requeued waiter could not recognise itself**: it fell into the
first-entry branch, read the old word, found it still equal to the expected value,
and went back to sleep on the old address - and a `WAKE` on the new address could
never wake it again. That is exactly how the `fork+cow+shm+futex` profile of the
`fork` gate went red: 72 != 76 readouts, the serial log stopping right after
`clone for futex2 requeue child > 0`, with the parent cycling through
`-ETIMEDOUT` retries until the runner timed out. The test is now "**my own slot is
still in use**": a task cannot start a second `WAIT` while it sleeps, so a used slot
can only be a replay; the value to compare against also comes from that slot (it was
updated to the current word at `uaddr2` when the requeue happened). The host case
used to simulate the replay with `uaddr = 0`, which only reaches the "`a0` was
cleared to 0 by the return value" fallback and could never show this shape - it now
re-enters with **the address the caller actually wrote**, plus an assertion that a
`WAKE` on the old address still returns 0 after the replay, pinning "it did not go
back to sleep on the old address".

9. **Two arguments change meaning with the op: the `timeout` slot is `nr_requeue`
   inside `REQUEUE`.** The futex number table reuses the same argument slot across
   ops: in `REQUEUE`/`CMP_REQUEUE` argument 4 is `nr_requeue` (not a timeout
   pointer), argument 5 is `uaddr2` and argument 6 is `val3`; and `WAIT` takes its
   timeout as a **relative** duration while `WAIT_BITSET` takes an **absolute**
   instant. Writing any of those three the "obvious WAIT way" fails silently: a
   `REQUEUE` reads a pointer as a count (or a count as a pointer, giving
   `-EFAULT`), and a `WAIT_BITSET` sleeps a full duration on every call and never
   expires. The parser is therefore factored into
   `lx_futex_timeout(p_ctx, abs_flag, &dl)`, with the caller stating whether that
   op is absolute or relative, and the result is stored as a **deadline** rather
   than a duration - a duration cannot express "how long have I already waited"
   inside a slice-replay model at all. Lesson: **when porting an op table, draw a
   table of argument positions before writing code**; one position meaning
   different things under different ops is normal for old Linux-style interfaces.

(3) **One `write` is not one line.** Section 8.3 made `lx_write` hold the console
lock, which fixes "a kernel log line lands inside a user line"; it does not cover a
fixture that splits one logical line into three `write` calls (prefix, number,
newline) - another process's whole line can land between two of them. The full gate
hit exactly that: the child's `[fork] child pid=12` appeared between
`[fork] parent child=` and its number, producing
`[fork] parent child=[fork] child pid=12`, so a line-oriented gate reported "the
child pid readout is missing" while every readout had been printed. The fix lives in
the fixture: lines with numbers are formatted into a stack buffer and written with a
**single `write`** (`out_line3` / `out_line5`). Linux line-buffered `printf` does the
same thing; `rt_kprintf` in RT-Thread looks immune because it formats the whole line
and emits it with interrupts off.

Host accounting: the union line coverage of the `linuxabi` / `linuxabioff` /
`linuxabifutex` / `linuxabifilemap` / `linuxabiselect` profiles is **98.22%** (the
pinned baseline in `tools/coverage_baseline.json` is 97.96%; this round only moved
up and neither lowered the baseline nor ran `--update`), the `linuxabi` profile runs
**270 checks**, the futex profile **434** (63 more for the PI trio and robust list,
2026-11-26), the filemap profile **358** and the select profile **327**, all
printing `LINUX-ABI HOST OK`. The
`ARKRTOS_USE_PAGING_SHM=1` shared branches are **not in the host ledger** (the host
fixture was never compiled with `LINUX_FORK=1`); that half is measured only by the
40 SHM, 73 futex and 20 filemap board readouts. The 24 remaining uncovered lines
were each inspected and are all dead/defensive branches (two `-EBUSY` re-entry
returns, the overflow/alignment `-ENOMEM` variants, `-E2BIG`, embedded weak-symbol
fallbacks), not "untested".

### 8.4b futex fourth batch: the PI trio and robust list (2026-11-26, 63 host + 37 QEMU LA64 checks)

Section 8.4 answers "sleep until this word changes". The default glibc `pthread_mutex`, however,
is a **PI (priority-inheritance)** mutex and walks a different path: `FUTEX_LOCK_PI` to take,
`FUTEX_UNLOCK_PI` to release, `FUTEX_TRYLOCK_PI` to try. On top of that, `pthread_mutex_lock`
must survive "the owner died inside the critical section and the chain would block forever",
which is what syscalls 99/100 (`set_robust_list` / `get_robust_list`) and the exit-time robust
cleanup provide. Both need the kernel to know **who owns and who queues** - a plain futex lets
anyone wake anyone, while PI must **hand the lock** to the waiter that most deserves it, or a
high-priority task is pushed back indefinitely behind medium-priority tasks queued behind a
low-priority owner (priority inversion).

| Item | Current state |
|---|---|
| ops | `LOCK_PI`(6) / `UNLOCK_PI`(7) / `TRYLOCK_PI`(8), nine ops together with the six in 8.4 (the two condition-variable variants are 8.4c). `FUTEX_PRIVATE_FLAG`(128) is accepted and stripped; `FUTEX_CLOCK_REALTIME`(256) with a timeout honestly returns `-ENOSYS` (no wall clock) |
| word layout | low 30 bits = owner tid (`FUTEX_TID_MASK = 0x3FFFFFFF`), bit 31 `FUTEX_WAITERS = 0x80000000`, bit 30 `FUTEX_OWNER_DIED = 0x40000000`. Bit-for-bit the Linux layout - glibc reads this word directly, so one bit off is a different ABI |
| priority inheritance | queueing raises the waiter's `base_priority` onto the owner and release/hand-over computes it back, all through the single `arkrtos_ready_reprio()` entry that reorders the ready set. **The task's `base_priority` plus the "who waits on me" table is the only source of truth**; no cached "effective priority" snapshot that can be poisoned by an intermediate value - otherwise "raised twice, discounted once" only shows up with several waiters |
| hand-over, not wake-then-race | `UNLOCK_PI` and robust cleanup **write the word directly** as "taker tid | kept bits", so ownership moves at once. Waking every waiter to race again is exactly the cause of priority inversion and the semantic line between PI and plain futex - and the anti-false-green check targets it: if the implementation degenerates to wake-then-race, the parent in QEMU group 2 reads `-EAGAIN` or still finds the dead child's tid, i.e. a red readout rather than a hang |
| who takes over | `lx_pi_top_waiter()`: lower `base_priority` first, then lower task id (stable order). With no waiter the word is simply cleared |
| wait and replay | the same restartable-syscall path as 8.4 (100 ms slice + `arkrtos_port_svc_restart()`); the replay takes `uaddr` and the expected value from the wait slot, never from the return register. `LOCK_PI`'s `timeout` is a relative duration, as in Linux |
| errors | `LOCK_PI` twice, or `TRYLOCK_PI` while holding it, gives `-EDEADLK`(35); `UNLOCK_PI` by a non-owner gives `-EPERM`; `TRYLOCK_PI` on a word owned by someone else gives `-EAGAIN` **and sets `WAITERS`** (the owner learns somebody queues, otherwise it never hands over); a sleeping task hit by an unblocked signal gets `-EINTR` |
| dead-owner takeover | a word carrying `OWNER_DIED` is taken over directly by `LOCK_PI`, and `OWNER_DIED` is **left for the taker**: user mode (glibc) only learns "the previous owner died in the critical section" from that bit and has to repair its data. Clearing it in the kernel would be non-conforming |
| no cross-address-space reads at registration | PI registration **copies** the owner tid into the kernel's own `s_futex_pi_owner[]` instead of reading the other process's word - a cross-process read can catch a half-written value and is untrustworthy for deciding who owns |
| robust chain | `set_robust_list(head, len)`: non-null `head`, 8-byte aligned, `len >= 12` (`next` / `futex_offset` / `list_op_pending`), else `-EINVAL`. Each entry is `{ next | PI bit }` where the lowest bit of `next` marks a PI mutex; `futex_offset` is a **byte offset from the node itself** (`offsetof` semantics), not an element count - trap (1) below |
| exit cleanup | in `on_task_exit` it first runs `lx_robust_exit()` (rewriting user memory: every chained word becomes `OWNER_DIED`, PI entries handed over by PI rules) and only then `lx_futex_forget()` (drops the kernel registration). The order cannot be swapped - dropping the registration first loses the waiter information needed for the hand-over. The chain is truncated at `ROBUST_LIST_LIMIT = 2048`: dying inside the exit hook stops the whole system, which is worse than missing one entry |
| cross-process boundary | `get_robust_list(pid, ...)` accepts only `pid == self`; a foreign pid honestly returns `-ESRCH` (there is no queryable second process identity, and no pretending to read another address space); a null out-pointer is `-EFAULT` |
| differences from Linux | the condition-variable PI variants `WAIT_REQUEUE_PI`(11) / `CMP_REQUEUE_PI`(12) are not in this batch - they have their own implementation (8.4c); `FUTEX_CLOCK_REALTIME` with a timeout is `-ENOSYS`; waiter choice looks at `base_priority` only - the deadline class applies to ready-set selection, not to futex waiter ordering (see 8.17) |

Readouts (37 of the 147 in the fork+COW+SHM+futex tier; the host side has another 63 in
`t_futex_pi()`):

| Readout | What it pins |
|---|---|
| `futex4 mmap shared page > 0 -> 1` | the fourth batch's shared page exists (whole 8 KB page requested) |
| `futex LOCK_PI on a free word == 0 -> 0` / `... wrote our tid into the word -> <tid>` | the free word is taken and holds this task's tid, not an arbitrary marker |
| `futex LOCK_PI twice == -EDEADLK -> -35` / `futex TRYLOCK_PI while held == -EDEADLK -> -35` | waiting on yourself must fail at once instead of sleeping |
| `futex UNLOCK_PI as owner == 0 -> 0` / `... cleared the word -> 0` | release succeeds and the word is clean (leftover tid or `WAITERS` would mislead the next owner) |
| `futex UNLOCK_PI when not the owner == -EPERM -> -1` | a non-owner cannot release (as in Linux) |
| `futex TRYLOCK_PI on an owned word == -EAGAIN -> -11` | try-lock fails at once instead of sleeping |
| `futex TRYLOCK_PI set the WAITERS bit while failing -> 2147483648` | the failing try sets `WAITERS` - that bit is how the owner knows somebody queues |
| `futex TRYLOCK_PI left the owner tid alone -> 4242` | a failing try does not touch the owner tid |
| `futex LOCK_PI on a dead owner == 0 -> 0` / `... took over from the dead owner -> <tid>` | an `OWNER_DIED` word is taken over and rewritten with the new owner |
| `futex LOCK_PI kept OWNER_DIED for the taker -> 1073741824` | the taker sees `OWNER_DIED`, so user mode can repair |
| `futex UNLOCK_PI after consistent == 0 -> 0` | release works after user mode cleared `OWNER_DIED` |
| `set_robust_list with a short length / unaligned head / null head == -EINVAL -> -22` | all three malformed heads refused |
| `set_robust_list on a well formed head == 0 -> 0` | a legal head registers |
| `get_robust_list(self) == 0 -> 0` / `... reports the head we registered -> <addr>` / `... reports the length we registered -> 24` | what is read back is what was registered, not a private copy |
| `get_robust_list on a foreign pid == -ESRCH -> -3` | a foreign pid is honestly refused |
| `clone for PI robust child > 0 -> 1` | the cross-process PI owner is a real cloned process |
| `PI robust: parent observed the child holding the lock -> 1` | the handshake word was written (the child really took the lock) |
| `PI robust: parent acquired after the holder died -> 0` | **the most important one here**: after the child exits holding the lock the parent acquires it - robust cleanup ran and ownership was handed over |
| `PI robust: word holds the parent tid -> <tid>` / `... carries OWNER_DIED for the taker -> 1073741824` | after the hand-over the word holds the parent's tid and keeps `OWNER_DIED` |
| `PI robust child saw register+lock+tid+waiter (bits=15) -> 15` | bit 0 registered / bit 1 took the lock / bit 2 the word holds itself / bit 3 saw the parent queue (`WAITERS`) |
| `clone for non-PI robust child > 0 -> 1` | a plain (non-PI) glibc robust mutex owner is also a real cloned process |
| `non-PI robust: parent observed the child owning the word -> 1` | the child **writes the word directly** (no kernel involved) and owns it |
| `non-PI robust: parent took over the dead owner's word -> 0` / `... word carries OWNER_DIED after the death -> 1073741824` / `... word now holds the parent tid -> <tid>` | non-PI entries are settled too: the dead word becomes `OWNER_DIED` and the parent's tid after takeover |
| `non-PI robust child registered and owned (bits=3) -> 3` | both registration and ownership held in the child |

**Three traps only a board run exposes (fixture-side contract errors that all read like kernel bugs).**

(1) **`futex_offset` is a byte offset, not an element count.** Every fixture variable is an
`unsigned long *`, and the first version wrote `rh[1] = 0x40ul` meaning "0x40 elements ahead".
The kernel reads it with Linux's `offsetof` semantics, i.e. 0x40 **bytes**, and landed on
0x108940 instead of the lock word 0x108B00 - read 0, which is not the dead tid, so cleanup was
silently skipped and the parent waited forever. Computing it as
`(const char *)lock - (const char *)nod` turned the group green. What makes this bug nasty is
that it **reports nothing**: a wrong offset just reads some other word, and "read 0" is a legal
intermediate state in robust semantics.

(2) **A futex used as a handshake must wait on the "not ready yet" value, never on the current
value.** Two "wait for the child to release us" `WAIT`s used to pass `v = *go`; if the child had
already released (word is 1) the kernel concluded "value unchanged, sleep" and swallowed the
wakeup forever, so the parent could never be woken. Passing a fixed 0 makes an already-released
word return `-EAGAIN` immediately and ends the loop. Every futex handshake in Linux must obey
this; the only difference is that on real Linux it shows up as an occasional hang instead of a
guaranteed one.

(3) **A shared page comes in 8 KB (0x2000) pairs, so element `0x400` already falls outside the
page.** The non-PI robust group put `go2` at `pw + 0x400` (= 0x2000 bytes), exactly the next page,
while `mmap` asked for only 4096 - so the handshake word was not mapped at all and the parent
replayed forever on 0x0210a000 with `resumed=1 sval=0`. Mapping 8192 and moving `go2` to
`pw + 0x380` fixed it. In the same group the child used to spin 4,000,000 times waiting for
`WAITERS`, which on a single core starves the parent so it never reaches `LOCK_PI` (readout
`bits=7`, bit 3 missing); it now loops 2000 times on a `FUTEX_WAIT` with a 0.5 ms timeout
(there is no `sched_yield`; this is the only call that yields here) and reads `bits=15`. Lesson:
**waiting for another execution stream must really yield the CPU** - a pure spin on one core
starves the other side.

Host side: `t_futex_pi()` adds **63** assertions on the mechanism (registration / blocking /
hand-over / wake semantics / priority raise and discount / robust argument surface / exit-cleanup
bookkeeping); the cross-process surface is left to the 37 QEMU readouts. They do not overlap -
the host controls exactly who owns and who waits, QEMU proves that two processes really share one
physical frame and that the exit hook really reaches user memory.

### 8.4c futex fifth batch: the condition-variable PI variants (2026-11-27, 68 host + 14 QEMU LA64 checks)

8.4b finishes PI mutexes, but **a PI mutex paired with a condition variable** still has a path of
its own: glibc's `pthread_cond_wait` uses `FUTEX_WAIT_REQUEUE_PI`(11) when the mutex is PI, and
`pthread_cond_signal` uses `FUTEX_CMP_REQUEUE_PI`(12). The semantics differ from every earlier
batch: the waiter **already owns** the mutex (`cond_wait` releases it first and then sleeps on the
condition word), and on wake it must **directly receive** the mutex - not "wake it and let it
`LOCK_PI` again" (that would throw the PI guarantee away and fall back to priority inversion), but
the kernel writes the mutex word **to the waiter's tid on the spot** and then wakes it.

| Item | Current state |
|---|---|
| ops | `WAIT_REQUEUE_PI`(11) / `CMP_REQUEUE_PI`(12), eleven ops together with the earlier nine. Both share the 8.4b word layout (low 30 bits owner tid / bit31 `WAITERS` / bit30 `OWNER_DIED`); `FUTEX_PRIVATE_FLAG` is accepted and stripped as before |
| waiter registration | `uaddr2` must be **held by someone**, otherwise `-EINVAL` at once - the requeue target is a PI lock somebody grips, and without an owner there is no position to hand over (Linux judges the same way); before registering, bit `WAITERS` is set on `uaddr2`, otherwise the owner's `UNLOCK_PI` would never know it has to hand over. The wait slot carries one extra flag, `s_futex_rqpi`: rqpi waiters are PI class too, so a plain `WAKE` / `REQUEUE` **does not touch them** (the class test is `s_futex_pi`), and only `CMP_REQUEUE_PI` moves the whole queue by `s_futex_rqpi` |
| hand-over | `CMP_REQUEUE_PI` does, in order: check that the word at `uaddr` equals `val3` (else `-EAGAIN`) -> check `uaddr2` has an owner (else `-EINVAL`) -> **first** wake up to `nr_wake` ordinary waiters on `uaddr` (same order as Linux: wake before moving, or the woken task races in and rewrites the word) -> pick the rqpi waiter with the highest `base_priority` as the head, write `head tid` into `uaddr2` (`OWNER_DIED` carried over verbatim; `WAITERS` kept only when more rqpi waiters remain and this call is allowed to move them) -> wake the head -> move the rest (up to `nr_requeue`) onto `uaddr2`, updating key, address, expected value and owner (the owner is the new head, so priority inheritance lands on whoever really grips the lock) |
| return value | **woken plus requeued** (head included), the `man futex` "woken up or requeued" convention: with `nr_wake=0` the caller can still tell from the return value whether the move succeeded |
| replay | the same restartable path as 8.4. When the head wakes, `s_futex_woke` is already set: the slot is cleared and 0 returned, **without re-reading the word** - the kernel has just written the ownership, and user mode may already have changed it to 0 or another bit, so re-reading could only misjudge |
| error codes | value mismatch `-EAGAIN`; `uaddr2` null or unowned `-EINVAL`; either address outside the window `-ENOSYS`; user-memory faults `-EFAULT` (a failed write to `uaddr2` also cancels the wake-up, so nobody runs with an ownership that never reached memory); a signal while parked `-EINTR`; a port without restart support `-ENOSYS` |
| differences from Linux | (1) **no FIFO wait queue**: the head is chosen by `base_priority` (task id as tie-break), while Linux follows the rtmutex queue order. For PI semantics this is a stronger property (the task that should run first takes over), but "first come, first served" is not guaranteed here; (2) sleeping is 100 ms slice replay (no infinite sleep in this kernel), externally equivalent; (3) `timeout` is **relative** only; `FUTEX_CLOCK_REALTIME` honestly returns `-ENOSYS` (no wall clock); (4) mixed queues are split by the `s_futex_rqpi` flag - a waiter that went to sleep on the same word with a plain `WAIT` is only woken up to `nr_wake` times and never moved, whereas Linux has its own judgement for mixed queues; (5) rescheduling looks at `base_priority` only; the deadline class does not order futex waiters (see 8.17) |

Readouts (the 14 QEMU LA64 checks of this batch, all inside `tools/fork_gate.py`'s futex tier):

| Readout | What it pins |
|---|---|
| `futex5 mmap shared page > 0 -> 1` | the fifth batch's shared page exists |
| `futex(WAIT_REQUEUE_PI) on a different value == -EAGAIN -> -11` | a stale value does not sleep (same rule as `WAIT`; the fast path never reaches the move logic) |
| `futex(WAIT_REQUEUE_PI) with a free mutex == -EINVAL -> -22` | an unowned target lock is refused on the spot - an unowned lock is not treated as a position to take over |
| `futex LOCK_PI before the condvar handoff == 0 -> 0` | the parent really is the owner of a PI lock before the hand-off (everything about `WAITERS` and hand-over assumes it) |
| `futex(CMP_REQUEUE_PI) with a mismatched word == -EAGAIN -> -11` | the moving side checks the expected value too |
| `futex(CMP_REQUEUE_PI) without uaddr2 == -EINVAL -> -22` | a missing `uaddr2` is refused |
| `clone for condvar rqpi child > 0 -> 1` | the waiter is a second real process |
| `futex rqpi: child queued on the mutex (WAITERS) -> 1` | the child really parked and the kernel set `WAITERS` for the owner (the owner relies on that bit to know someone queues) |
| `futex(CMP_REQUEUE_PI) handed the mutex over == 1 -> 1` | the move count is 1 (head included) - a "wake but do not move" implementation reads 0 here |
| `futex(CMP_REQUEUE_PI) wrote the child tid into the mutex -> <tid>` | ownership **transfers on the spot**: the word holds the child's tid, not the parent's |
| `futex(CMP_REQUEUE_PI) left no stray WAITERS -> 0` | with a single rqpi waiter no `WAITERS` is left behind, otherwise the new owner's `UNLOCK_PI` would hand over an empty queue |
| `futex rqpi child saw the handoff (bits=7) -> 7` | the child sets all three bits: woken by the move (bit0) / word holds its tid (bit1) / the inherited lock unlocks cleanly (bit2) |
| `futex rqpi child reaped -> <n>` / `futex rqpi mutex cleared after the inherited unlock -> 0` | the child exits normally and what it releases is **the lock it inherited** (the word ends at 0, no tid left) |

Host side: `t_futex_requeue_pi()` adds **68** assertions (43 `ck_ret` + 25 `ck` / `ck_u`) covering what
QEMU cannot control: replay keeps sleeping / CMP with a stale value / the two-waiter priority choice
(base 3 beats base 9) / the moved waiter replays and the word finally clears / `nr_requeue=0` moves
only the head (word `6` without `WAITERS`) / a failed write to `uaddr2` returns `-EFAULT` and does
**not** wake anyone / both `-EFAULT` reads of CMP / no waiter returns 0 with the word untouched /
zero timeout and a port without restart / a fault while reading the timeout structure / `-EINTR` /
the three replay exits (`-ENOSYS`, `-ETIMEDOUT`, `-EINTR`).

Two fixture-side rules worth writing down (otherwise they will be stepped on again):

(1) **The head must replay and clear its slot before it unlocks.** If the test lets the moved head
call `UNLOCK_PI` directly, `lx_pi_top_waiter()` picks "itself, still queued" as the successor and the
word keeps `tid | WAITERS` (`0x80000007`) instead of 0. Real Linux does hand-over plus wake-up
atomically inside one syscall; this kernel models it as "register + slice replay", so the user-side
unlock must happen after the replay returns - which is exactly glibc's order (`cond_wait` returns
before the lock is used again).

(2) **With a single mover, the `WAITERS` bit depends on whether anyone else is left.** The fixture
has to give the two waiters explicit `base_priority` 4 / 5 (tid 6 / 7) to reproduce "tid 7 wins";
with default priorities the choice varies and the assertion flips between red and green - any test
of priority inheritance must write `base_priority` explicitly instead of relying on defaults.

### 8.5 File mapping and write-back (`ARKRTOS_USE_PAGING_FILEMAP`, 2026-11-23, QEMU LA64)

`clone` gives a second execution stream and `MAP_SHARED` lets both see the same
memory, but **using a file as memory** was still missing: without it `ld.so` can
only read a whole library into anonymous memory, there is no path to put modified
content back, and `msync` has nothing to mean. With
`ARKRTOS_USE_PAGING_FILEMAP` (`depends on ARKRTOS_USE_LINUX_ABI` + `PAGING` +
`PAGING_PAGES` + `PAGING_DEMAND`) number 222 `mmap` takes the file path when
`MAP_ANONYMOUS` is absent, and number 227 `msync` appears.

| item | current state |
|---|---|
| accepted shapes | no `MAP_ANONYMOUS` means a file mapping; `MAP_FIXED` with a file mapping is still `-ENOSYS` (discard-then-re-read needs write-back semantics); exactly one of `MAP_PRIVATE` / `MAP_SHARED`; `prot` must include `PROT_READ`; `offset` must be 4 KB aligned |
| filling | at `mmap` time the file content is copied into the heap window in 128-byte chunks (`arkrtos_vfs_read_at` + `arkrtos_copy_to_user`); `offset` applies; **bytes past EOF read as zero** (no faked SIGBUS). Changes to the file after the mapping are **not** visible - this is a one-shot snapshot, not demand paging from the file |
| address | shares the same heap cursor as anonymous `mmap`; the cursor is **not** rolled back on failure (rolling back would let two mappings share a page - a silent wrong answer) |
| write-back bookkeeping | only "`MAP_SHARED` + `PROT_WRITE` + opened `O_RDWR`" mappings, at most **2 per process** (`LX_FILEMAP_MAX`); private mappings never reach the file |
| write-back moments | `msync` (227), `munmap`, and task exit (top of `on_task_exit`, before any critical section - write-back calls VFS and must not run with interrupts off) |
| how | VFS has no `write_at`, so: read the whole file, overlay the mapped range, write the whole file back. If the file **changed length** while mapped the call returns `-EIO` (never truncate somebody's longer file); a range entirely past EOF is a no-op (the file does not grow) |
| buffer | one kernel-side 8 KB buffer plus a critical-section busy flag: a second concurrent write-back honestly returns `-EBUSY` instead of letting two copies overwrite each other in one buffer |
| limits | a writable shared mapping of a file larger than 8 KB fails at `mmap` time with `-ENOMEM` (early, rather than writing back half of it later); private mappings are not capped by this, but `len` is still limited by the heap window |
| `msync` | address 4 KB aligned, `len` non-zero, flags limited to `MS_ASYNC` / `MS_INVALIDATE` / `MS_SYNC`; a hit in the bookkeeping syncs and counts; a miss inside the window returns 0 (private/anonymous have nothing to write back, same as Linux); outside the window `-ENOMEM` |
| plain file `write` | still `-ENOSYS`: content reaches the file only through the mapping, and nothing pretends otherwise |
| `mprotect` | unaffected: still validation-only (section 1); `PROT_EXEC` cannot be enforced in hardware on this LA64 path (no NR/NX bit) |

Readouts (the fifth tier of `tools/fork_gate.py`, fork+COW+SHM+futex+filemap,
**167 checks**, 20 of them the filemap block):

| readout | what it pins down |
|---|---|
| `filemap open read-only fd > 2 -> 1` | the file really exists in ramfs and opens by namespace path |
| `filemap private map > 0` / `first byte is file first byte` / `byte 25 is Z` | the content really came from the file, not from a zero page |
| `filemap last byte of the page -> 'N'` | a 5000-byte file still has content at the end of the first 4 KB page |
| `filemap misaligned offset == -EINVAL -> -22` | 4 KB offset alignment is enforced |
| `filemap bad fd == -EBADF -> -9` | a bad fd does not fall through to the anonymous path |
| `filemap writable shared on read-only fd == -EACCES -> -13` | writable shared mappings require `O_RDWR` (Linux semantics) |
| `filemap offset=4096 map > 0` / `offset content starts at 4096 -> 'O'` | `offset` is honoured, not ignored |
| `filemap past EOF reads zero -> 0` | past EOF reads zero (no faked SIGBUS) |
| `filemap O_RDWR open on existing file > 0` | the one exception this tier allows really works |
| `filemap writable shared map > 0` / `msync rc -> 0` / `munmap rc -> 0` | a writable shared mapping can be built and both write-back moments return 0 |
| `filemap private writable map > 0` / `private msync rc (no writeback) -> 0` | a private `msync` returns 0 and does **not** write back |
| `filemap remap after writeback > 0` | after write-back a fresh mapping reads the same bytes back |
| `filemap shared write persisted, private write not (bits=31) -> 31` | five evidence bits at once: the shared write reached the file and the private write did not |

**A trap only a board can show: the SVC argument cap is 4.** The first version
failed 7 checks on QEMU and every file mapping returned `-9`: `ARKRTOS_ARCH_SVC_ARG_MAX`
is 4 and the port's `arkrtos_port_svc_get_arg()` returns 0 for `idx >= 4`, so
`mmap`'s `fd` (`a4`) and `offset` (`a5`) were **always 0** => `lx_fd_find(0)`
failed => `-EBADF`. The host fixture passed because its `svc_get_arg` stub allows
six arguments - **this class of defect only shows up on hardware.** The fix widens
the cap to 6 in the LA64 and RV32 ports when `ARKRTOS_USE_LINUX_ABI == 1`: the
trap frame already carries `$a0..$a7` (`LA_FR_A0 + 0..7`), so the frame layout is
untouched and native SVC still uses 0..3 only. RV32 was widened for the same
reason, so the "reads as 0" trap is not left behind there either.

### 8.6 `mprotect` page-level write protection (`ARKRTOS_USE_PAGING_MPROTECT`, 2026-11-23, QEMU LA64)

Up to section 8.5, syscall 226 still only validated its arguments, returned 0 and
bumped a counter - **not one page permission changed**. That means the RELRO
protection `ld.so` installs (making `.text` and `.data.rel.ro` read-only) simply
did not exist: a stray store could rewrite the relocation tables silently.
Turning on `ARKRTOS_USE_PAGING_MPROTECT` (`depends on ARKRTOS_USE_LINUX_ABI` +
`PAGING` + `PAGING_PAGES` + `PAGING_DEMAND`) makes `mprotect` edit real TLB
entries.

| item | current state |
|---|---|
| which bit is enforced | only the **write** bit: `PROT_WRITE` decides the page's `WE`. `PROT_EXEC` / `PROT_NONE` are recorded but not enforced - this LA64 path has no NR/NX, so hardware non-executability is impossible, and `PROT_NONE` is not made inaccessible either (reads still read). Stated as a boundary, not pretended away |
| granularity | **4 KB**, not one 8 KB "pair": the ledger has one page per bit (bit0 = low 4 KB, bit1 = high 4 KB) and the port does two passes. `mprotect(addr, 4096, RO)` makes only the low 4 KB read-only; the high 4 KB of the same pair stays writable |
| ledger constraint | a pair's entries are written two at a time (`tlbfill` installs pairs), so the protected page must be **resident as a whole pair**: if the pair has no frame yet, fault it in first, then rewrite the entries |
| how a violation is classified | in the trap, **mprotect is checked before COW**: a hit means `arkrtos_cpu_fault_kill()` - **kill the task, do not restart it** (restarting would re-execute the same store forever); a PLV0 violation halts explicitly (it must not look like "the user program was killed") |
| status word | being killed by a signal must be distinct from `exit(11)`: the TCB grew one byte, `exit_by_signal`, and the `wait4` status word carries the signal number in its low 7 bits (SIGSEGV = 11) while a normal exit is `code << 8`. Otherwise one out-of-bounds store looks like "a normal exit(11)" to the parent and `WIFEXITED` / `WIFSIGNALED` are both wrong |
| switching away from the trap | after the kill, `s_pg_commit` must be set explicitly: a forked parent and child share one VA set, so the target frame address and the faulting frame address **can be byte-identical** (both measured as `0x021fbcb0`); `la_switch_prepare` then reads "equal addresses" as "no switch", the window is not swapped, and the frame restored is the faulting one again |
| lifetime | inherited page by page across `fork` (`la_pg_space_clone`) and cleared when the space is released |
| revoking | another `mprotect` with `PROT_READ|PROT_WRITE` revokes it. A COW-shared pair is read-only anyway, and revoking mprotect does not open it up (that would bypass copy-on-write and let both processes write their own frame) |
| log | a violation prints `[LA64U] mprotect violation badv=0x... era=0x... tid=... -> kill task` (the `tid` tells operators which task was reaped; the gate uses this line as cross evidence) |

Readouts (fifth tier plus `-DARKRTOS_USE_PAGING_MPROTECT=1`, **178 checks**, 11 of
them the mprotect block):

| readout | what it pins down |
|---|---|
| `mprotect mmap 8 KB > 0 -> 1` | the block really got an 8 KB anonymous mapping (everything below depends on it) |
| `mprotect RO == 0 -> 0` | `mprotect(4096, PROT_READ)` is accepted |
| `read of a read-only page still works -> 4369` | the read-only page can still be **read** (read-only was not turned into inaccessible) |
| `sibling 4 KB half of the same pair stays writable -> 13107` | the other 4 KB of the same pair **is still writable** - granularity is 4 KB, not 8 KB |
| `mprotect RW == 0 -> 0` / `write after mprotect(RW) took effect -> 17476` | protection is **revocable**, not one-way |
| `mprotect RO again == 0 -> 0` | setting read-only a second time works (idempotent) |
| `clone for mprotect child > 0 -> 1` | the violating child really forked |
| `mprotect child reaped (killed, not restarted) -> 1` | the violating store is **really blocked**, and blocked by killing the task rather than restarting it (a restart would re-execute the same instruction) |
| `mprotect violation reports SIGSEGV status -> 11` | the status word uses the Linux "killed by signal" encoding (low 7 bits = signal number), not the high byte of `exit(11)` |
| `parent page unchanged after the violation -> 17476` | **not one byte** of the child's violating store reached the parent's page |

Plus one serial-side cross evidence line: `[LA64U] mprotect violation badv=0x...
era=0x... tid=... -> kill task`. Without it, "the child was reaped" could simply
mean the child exited on its own; this line shows it went through the mprotect
decision path.

**A trap only a board reveals: identical parent/child frame addresses, and
`s_pg_commit` does not set itself.** The first board run printed **two identical**
violation lines, the second one with the parent's `tid`, then the whole demo
ended early. The root cause was not the ledger but the "kill the task and switch
away from the exception" step: the forked parent and child share one VA set, so
the frame address returned equals the faulting frame address, `la_switch_prepare`
concluded "equal addresses means no switch", the window stayed the child's, the
frame restored was the child's again, and the same store ran once more - this
time killing the parent. The fix is an explicit `s_pg_commit = 1u;` after
`LA_PRIV_APPLY()` in the kill branch. **Address equality says nothing here**, and
that applies to any future "switch tasks from inside the trap" path.

**Second trap: the wording of a readout line is part of the judgement.** The
fixture originally wrote `check("...SIGSEGV status -> 11", ...)`, so it printed
`... -> 11 -> 11 ok` while the gate demanded the literal `... -> 11 ok`; the six
tier gate therefore failed on its last tier - the code was right, the readout was
right, **only the label carried one layer too many**. The fix is to keep the
expected value out of the label and let `check` print it.

### 8.7 Real `mmap` semantics: `MAP_FIXED` (demand tier, 2026-11-24, QEMU LA64)

Up to section 8.6, syscall 222 `mmap` had one shape only: take a **fresh**
address above the heap-window cursor. `MAP_FIXED` is the exact opposite - "**use
this address**, throw away whatever is there". The difference is not a return
value: the first guarantees no other mapping is disturbed, the second is a
statement that this range now belongs to the caller. `ld.so` reserving and then
laying out in place, `brk` shrinking and growing again, and any "map a placeholder,
then fill it in place" scheme all rest on the second one.

**Anonymous private only.** `MAP_SHARED|MAP_FIXED` and `MAP_FIXED` on a file
mapping are both `-ENOSYS`: a shared pair can only be discarded when the **whole
pair** lies inside the span (see below), and discard-then-re-read on a file needs
write-back semantics this kernel does not have. **Unsupported means refuse**, never
"allocate elsewhere and hand back the same address" - the program would believe it
owns that memory, which is the hardest green to spot.

| item | current state |
|---|---|
| semantics | discard in place: every page covered by `[want, want+len)` is **zeroed in place** and the entry is installed writable. On success the return value is `want` itself |
| frames are not returned to the pool | a resident private pair is **zeroed in place**, not freed. Two reasons: a space's frames are only borrowed anyway, and freeing would make a later unrelated allocation fail; zeroing keeps the address stable so the first store after `MAP_FIXED` does not fault again |
| never-touched pages | `pa == 0` is skipped - under demand paging it is zero already, there is no frame to clear and no entry to install |
| swapped pages | the swap slot is freed too: those bytes become unreachable immediately, keeping the slot would be a leak |
| COW-shared pairs | **copied first**: the other side keeps its bytes, we get a fresh pair and then zero it. This keeps `MAP_FIXED` from modifying somebody else's page |
| `MAP_SHARED` pairs | discarded only when the **whole pair** is inside the range; a partial overlap returns `-1` - "this half private, the other half shared" is a state this page-table format cannot express, so the whole call is refused instead of half-done |
| `mprotect` ledger | the read-only bit of the discarded page is cleared (a fresh mapping is writable) and the entry is installed writable |
| range | 4 KB aligned and entirely inside the heap window `[lx_heap_base(), lx_heap_end())`; misaligned or `want < base` is `-EINVAL`, `len > limit - want` is `-ENOMEM` |
| never half-done | the shared-pair split check runs **before anything is touched**; an empty frame pool returns `-2` (translated to `-ENOMEM` by the ABI layer) with the ledger untouched |
| cursor | on success `s_heap_cur` moves past the span (monotonically), so the next "any address" `mmap` does not hand out the same range again |
| switch off | with `ARKRTOS_USE_PAGING_DEMAND=0`, `mmap(MAP_FIXED)` honestly returns `-ENOSYS` (without per-pair entry installation there is nothing to discard), no downgrade and no pretending |

Readouts (all six `tools/fork_gate.py` tiers compile with
`-DARKRTOS_USE_PAGING_DEMAND=1`, so this block runs in every tier; 6 checks):

| readout | what it pins down |
|---|---|
| `mmap for MAP_FIXED > 0 -> 1` | the block really got a mapping that can be discarded |
| `MAP_FIXED returned the same address -> 1` | the address returned is the one **requested**, not somewhere else |
| `MAP_FIXED discarded the old bytes -> 1` | a marker byte is written at the start and at the end, and both read back as 0 after `MAP_FIXED` - the judgement is on **content**, not on the return value |
| `MAP_FIXED page writable afterwards -> 1` | the page can be written and read back after the discard |
| `MAP_FIXED misaligned -> -EINVAL -> -22` | `want + 1` is refused |
| `MAP_FIXED shared -> -ENOSYS -> -38` | `MAP_SHARED|MAP_ANONYMOUS|MAP_FIXED` is refused instead of silently accepted |

The third row is the important one: an implementation that only checks "same
address came back" can leave the old page untouched, and the program reads its own
stale data - the hardest green to spot. That is why the judgement has to sit on
the bytes.

**Fixture trap: every switch must be mirrored by hand.** In
`example/qemu_loongarch64/Makefile`, `$(LXFORK_ELF)` is built with
`$(ARCH_FLAGS) $(LXFORK_DEFS)` only - **not** CFLAGS / EXTRA_CFLAGS - so the
fixture cannot see the kernel switches. This block only exists when
`ARKRTOS_USE_PAGING_DEMAND=1`, and DEMAND was never mirrored, so all six tiers
reported "check count 26 != 32" while the code was perfectly correct. The fix is
an `ARK_DEMAND_ON` mirror in the Makefile, with a comment saying that every switch
a fixture needs must be mirrored explicitly and that a missing mirror shows up as
"those readouts never appear".

**The other real defect: `la_pg_install_pair` lived inside the COW block.** It was
defined inside `#if (ARKRTOS_USE_PAGING_COW == 1)`, while
`arkrtos_port_paging_discard()` calls it unconditionally - so the
"demand on, COW off" tier failed to link with
`undefined reference to 'la_pg_install_pair'`. That combination is exactly the
first `[fork]` tier: in other words, **DEMAND-only had never been linked**. The
fix moves the function (with its comment) outside the COW block, right after
`la_pg_pair_pte()`; everything it depends on (`la_tlb_write`,
`la_tlb_inval_page`, `LA_STLB_INDEX`, `LA_PG_WIN_WAY_OF`, `la_asid_swap`) is
unconditionally defined.

### 8.8 `pselect6`: readiness wait across fds (`ARKRTOS_USE_LINUX_SELECT`, 2026-11-24, QEMU LA64)

Up to §8.7 the compatibility layer could "wait" in two ways: `wait4` for a child to
exit (§8.1) and `futex` for a word to change (§8.4). The missing third one is what
shells, initscripts and any select loop depend on - **"tell me when one of these fds
can be read / written"**. On asm-generic that is syscall 72 `pselect6` (`select`
itself is not a separate syscall on la64 any more; libc translates `select` into it).

| Item | Status |
|---|---|
| Syscall and args | 72 `pselect6`: 0=`nfds`, 1=`readfds`, 2=`writefds`, 3=`exceptfds`, 4=`timespec*`, 5=temporary signal mask pointer |
| Timeout | As in Linux: `NULL` waits forever (replayed in time slices here), `0` is a poll returning 0 immediately, otherwise a **relative** duration that is **not written back** (that is the `pselect` / `select` difference) |
| Return value | Number of ready descriptors; ready bits are written back to the user sets and everything else is cleared. All three sets `NULL` with `nfds>0` means "no objects", i.e. a pure timeout wait |
| Read readiness | fd 0 always readable (returns EOF); regular files always ready; pipe read end when `count > 0 \|\| writers == 0` (**writer gone means EOF and must be reported readable**); pipe write end and unopened fds are not readable |
| Write readiness | fds 1 / 2 always writable (console); regular files always ready; pipe write end when `readers == 0 \|\| count < 256` (**full means no space** - the criterion is capacity, not "write would be callable"); pipe read end and unopened fds are not writable |
| `exceptfds` | Accepted but **never reported**: there is no out-of-band data path and no async error queue here. They are cleared on write-back too, not left untouched |
| fd validity | A selected fd must be backed by an object (fd <= 2, or present in this process's fd table); otherwise `-EBADF`. Silently ignoring it is the worst kind of false green: the caller then waits until its timeout |
| Argument checks | `nfds > 1024` (`FD_SETSIZE`) returns `-EINVAL`; a non-NULL sixth argument (the temporary signal mask) returns `-ENOSYS` instead of being silently ignored |
| `fd_set` access | Read and written **one 32-bit word at a time** with `copy_from_user` / `copy_to_user`; the full 1024-bit (128 byte) set is never placed on the kernel stack, and `nfds` decides how many words are touched |
| Blocking shape | Same as futex: `arkrtos_port_svc_restart` plus `arkrtos_task_block_in_critical(100 ms)`, with the **deadline** stored per task (not the remaining duration - otherwise every replay restarts the timer); without a restartable port it returns `-EAGAIN` honestly |
| Task id reuse | Task exit and `reset` both clear `s_sel_armed/s_sel_deadline`: task ids are reused, and a stale record means a new task inherits "already waiting" |

Readouts (seventh `tools/fork_gate.py` tier, fixture 178 -> **194 checks**, 15 of them select):

| Readout | What it pins |
|---|---|
| `pselect6 pipe2 == 0 -> 0` | The pipe is created first (every other readout depends on it) |
| `pselect6 empty pipe read end not ready -> 0` | An empty pipe read end is **not** ready (`timeout=0` returns 0 at once) |
| `pselect6 clears the user set when nothing is ready -> 0` | With nothing ready the user `fd_set` is cleared, not returned unchanged |
| `pselect6 timed wait returns 0 at the deadline -> 0` | A 50 ms relative timeout returns 0 when it expires |
| `pselect6 timed wait really waited >= 40 ms -> 1` | The wait **really** elapsed wall-clock time (an implementation that restarts the timer on every replay turns red here) |
| `pselect6 pipe wrote 3 bytes -> 3` | The fixture write is complete |
| `pselect6 read end ready after 3 bytes -> 1` | The read end is ready once data is buffered |
| `pselect6 keeps the ready bit in the user set -> 1` | The ready bit is written back (treating the user mask as input only turns red here) |
| `pselect6 write end ready while the pipe has space -> 1` | A write end with space is ready |
| `pselect6 stdout is always writable -> 1` | fd 1 is always writable |
| `pselect6 write end not ready when the pipe is full -> 0` | After the pipe is full (256 bytes) the write end is **not** ready - capacity, not "write would be callable" |
| `pselect6 read end ready on EOF after the writer closed -> 1` | With every writer closed the read end becomes ready by EOF (Linux must report readable, or a `read` loop never ends) |
| `pselect6 never reports exceptfds -> 0` | Only `exceptfds` given, fd readable: returns 0 |
| `pselect6 clears exceptfds -> 0` | That set is cleared as well |
| `pselect6 bad fd == -EBADF -> -9` | A selected fd with no backing object is rejected honestly |

**A trap only a board run shows: the fixture bit layout.** The first version wrote
"stdout is always writable" as `ws[1] = 2u`, meaning "the bit of fd 1" - but `fd_set`
is an array of 32-bit words, so bit 1 of `ws[1]` is **fd 33**, and an `nfds=2` call
contains no fd 1 at all; the readout honestly returned 0. The code was right and the
test was wrong, and only a real run exposed it (the host case writes the same word
through `u_put32` and never hit it). The fix is to clear the whole set and then set
`ws[0] = 2u` for fd 1.

**Why a separate switch.** The readiness criteria depend on the concrete shape of the
fd table and pipe objects (capacity 256, `writers` / `readers` counts); with sockets
they would have to be rewritten. Putting select and a future socket layer behind one
switch would turn "zero footprint when off" into "half the feature when off".
`ARKRTOS_USE_LINUX_SELECT` depends only on `ARKRTOS_USE_LINUX_ABI`.

### 8.9 socket (U-23): translation layer and the QEMU eighth tier (2026-11-24, 768 host checks + 232 QEMU checks)

13 numbers: `socket`(198), `bind`(200), `listen`(201), `accept`(202), `connect`(203),
`getsockname`(204), `getpeername`(205), `sendto`(206), `recvfrom`(207), `setsockopt`(208),
`getsockopt`(209), `shutdown`(210), `accept4`(242). The older `read`/`write`/`close`
dispatch on the fd kind (the same fd table holds files, pipes and sockets).

| Semantics | Implementation |
|---|---|
| handle | the fd slot carries `kind == socket` plus the kernel net handle; sockets share one table with files and pipes |
| `bind` / `connect` | address copied from the user `sockaddr_in`; family must be `AF_INET` (else `-EAFNOSUPPORT`), `alen < 8` -> `-EINVAL`, copy failure -> `-EFAULT` |
| `accept` / `accept4` | two-step: `step=1` takes the result and aux handle, `step=2` writes the address; a failed address write **releases the just-taken fd**. `accept4` honours `SOCK_NONBLOCK` / `SOCK_CLOEXEC` |
| `getsockname` / `getpeername` | after the name query, the 16-byte `sockaddr_in` and `addrlen` are written only when asked; `r[1]==0` returns early and **never touches the user `addrlen`** |
| `sendto` / `recvfrom` | only `r[4]!=0` takes the address form, otherwise it degrades to `SEND`/`RECV`; `recvfrom` length `>0xFFFF` -> `-EMSGSIZE`, `-ECONNRESET` returned directly |
| `setsockopt` / `getsockopt` | under `SOL_SOCKET` it translates `SO_REUSEADDR`(2), `SO_SNDBUF`(7), `SO_RCVBUF`(8), `SO_ERROR`(4); `SO_TYPE`(3), `SO_DOMAIN`(39) and `SO_ERROR` are answered locally; writing `SO_ERROR` -> `-EINVAL`; non-`SOL_SOCKET` or unknown opt -> `-ENOPROTOOPT`; unreadable value/length -> `-EFAULT`/`-EINVAL` |
| blocking | a pending kernel result becomes `-EAGAIN` under `O_NONBLOCK`/`MSG_DONTWAIT`, otherwise the call is replayed through `arkrtos_port_svc_restart()`; if replay is unavailable it returns `-EAGAIN` **without sleeping** |
| identity | `lx_sock_me_ok` checks the task identity before the fd: `task_id==0` yields `-ENOSYS` for every entry and **issues no net request at all** |

**Host readout.** The sixth Linux ABI coverage tier `linuxabisocket`
(`tools/coverage_gate.py`) runs the socket section of `test_linux_abi_host.c` on the host:
98.51% on that tier alone, 98.84% union across six tiers (baseline 98.82%, re-pinned on
2026-11-27 together with the `rseq`/`getcpu` host assertions), **768 checks** green.

**QEMU eighth tier: 232 checks all `ok` + `exit code 0`.** The eighth tier of
`tools/fork_gate.py` (`SELECT+SOCKET+LWIP`) really runs the socket section of
`example/qemu_loongarch64/elf/linux_fork.c` on LA64 (38 checks) plus one performance
regression check, stacked on the 194 checks of the seventh tier:

What it reads is the **real lwIP loopback** (`127.0.0.1`), not a stub: `socket` creates both
a stream and a dgram handle; the errno of a bad domain / bad type / unsupported type;
`bind` + `listen` + `getsockname` reads the port back as 8081 in network order; `connect` over
loopback, `accept` takes a new fd and writes back a peer whose family is `AF_INET`;
`send` 5 bytes / `recv` 5 bytes compared byte for byte (`hiyo` + `u`); `getpeername`'s IP is
`127.0.0.1`; `setsockopt` accepts REUSEADDR, `getsockopt`'s `SO_TYPE` / `SO_ERROR` are
answered locally by this layer, an unknown opt gives `-ENOPROTOOPT` and writing `SO_ERROR`
gives `-EINVAL`; after `shutdown(SHUT_WR)` the peer's `recv` returns 0 (a real EOF); after
closing, `read`/`write` return `-EBADF`; UDP goes through `sendto`/`recvfrom` and the peer
port of `recvfrom` must equal the client's `getsockname` port.

**Three pits that only a real run exposes (all fixed).**

1. **`sendto`/`recvfrom`/`setsockopt` must use the 6-argument entry.** The first version used
   the 4-argument `lx4`, argument 0 was never passed, and `send` always returned `-22`; with
   `lx6` it is fine.
2. **`getsockopt`'s 4-byte `int` must not be received through `lx_long`.** The kernel writes
   4 bytes, the upper 32 bits of `lx_long` keep their old value, and the read comes back as
   `-4294967295` - a fake number that looks deep; passing an `unsigned int pack32` and its
   address is the correct form.
3. **The netconn pool only has 4 slots.** `MEMP_NUM_NETCONN = 4`,
   `ARKRTOS_NET_MAX_SOCKETS = 4` and `LX_MAX_FD = 8` (fd 3..10) are the same order of
   magnitude; three TCP plus one UDP at the same time exhausts the pool. The fix is for the
   **fixture to close what it opened** (real programs do the same), not to enlarge the pool -
   enlarging it changes the default BSS and breaks "zero change when off". Likewise the pipe
   fds left over from the shm section are explicitly closed here.

**lwIP service task stack 384 -> 1024 words, lwIP task stack 1 KB -> 4 KB.** The first version
hung forever in `tcp recv 5`: the net service task running one round of `netconn` on a 384-word
(1.5 KB) stack punched through it, the STK guard reported
`stack bottom flag overwritten`, and the neighbouring `.bss` being stepped on looks like a
protocol-stack assertion. `ARKRTOS_NET_TASK_STACK_WORDS` is synchronised in three places to
1024 (`Kconfig.net` default, `arkrtos_autoconf.h`, the `arkrtos_net.h` fallback), and the lwIP
task stack of the non-virtio + socket build in `lwip_port.c` goes to 1024 words (the same
value as the virtio build; the reason is in a comment: this round also has to host a lo NIC).

**The "one socket round trip = one 100 ms slice" performance defect (fixed).** The first
version of the eighth tier passed, but the whole socket readout took 3.0 s
(`[fork] finished after 3000 ms`, right at the limit). Section timings measured the account:
`t_setup 507 ms / t_conn 403 / t_acc 202 / t_recv 202`, every step exactly an integer multiple
of `LX_WAIT_SLICE_MS = 100 ms` - the request was long done and the waiter still slept out a
whole slice. The root cause is that the request/reply slot only has a "state word" and no
readiness notification at all: `arkrtos_net_submit` returns `EPENDING` and the translation
layer blindly sleeps one slice and replays.

The fix follows what Linux / RT-Thread do and turns "waiting" into event-driven
(`kernelsrc/src/arkrtos_net.c`):

- After the service task publishes `REQ_DONE`, if this request came from the Linux ABI layer
  (`g_net_req_wake`) it calls `arkrtos_task_wakeup_internal(owner)` and wakes the waiter
  directly. A bare Ark SVC caller has no such contract (it may be waiting for something
  else), so the wake right is registered together with the request instead of "wake up
  whoever is waiting".
- That wake has a race: the service task may write the reply **before**
  `arkrtos_net_call()` above returns, at which point the flag is not set yet and no wake can
  be sent. So the waiter asks `arkrtos_net_reply_ready(me)` before going to sleep, and
  neither ordering wastes a slice.
- The slice stays as the fallback for a lost wake - one missed wake must not hang forever.
- The two-phase wait of `connect` ("the stack is still making progress") is replayed at
  `LX_SOCK_POLL_MS = 10 ms`: that kind of state change usually happens within a few
  milliseconds, so 100 ms per poll is a 121 ms local connect.

Measured: `t_tcp 2023 -> 168 ms`, the whole section 3.0 s -> **0.58 s**
(`[fork] finished after 580 ms, exit code 0`). This is a check too:
`socket round trips do not cost a wait slice (< 800 ms) -> 1 ok` - lose the wake and a dozen
round trips are back at 2 s level, so the check goes red immediately.

**Two construction constraints of the fixture (nothing to do with the kernel, but the fixture
is only stable with them).**

- **The child does not read the clock in user mode to wait.** The multi-slice futex WAIT used
  to spin on `clock_gettime` for 250 ms; on QEMU this path occasionally lets one
  `clock_gettime` return a positive value of unknown origin (the kernel side has no `rc=3`
  record and there is no Linux dispatch fall-through record either), the spin is cut short and
  the cross-slice check goes falsely red. The child now sleeps three kernel-accounted 100 ms
  timed `futex(WAIT)` calls and reads no clock at all; the parent reads the wall clock twice
  with 8 retries. The anomaly itself is recorded in
  [Known limitations](已知限制.md) as an open item.
- **The `N` in the two `child reaped -> N ok` readouts is the child's pid** and varies with
  the tier (6 / 12 / 15 have all been seen), so the check only judges the shape
  (`^\[fork\] futex child reaped -> \d+ ok$`) and never a literal pid.

**Build tier with lwIP and no NIC.** `example/qemu_loongarch64/Makefile` gained
`ARK_LWIP_ON := $(findstring ARKRTOS_USE_LWIP=1,$(EXTRA_CFLAGS))`: with lwIP on and
`ARKRTOS_USE_NET_VIRTIO` off it compiles only `$(LWIP_SRCS)` (no `drvnet.c`/`net_demo.c`),
and pushes `ARKRTOS_USE_LINUX_SOCKET=1` together with lwIP into `LXFORK_DEFS`.

### 8.10 Threads (`CLONE_THREAD`, U-25): one address space shared by a group of tasks (2026-11-24, ninth QEMU LA64 tier, 265 checks)

The thread branch of `clone(220)` is in, behind `ARKRTOS_USE_LINUX_THREAD` (default **0**,
`depends on ARKRTOS_USE_LINUX_FORK`). The split between the fork branch and this one is
purely by flags: `CLONE_VM` together with `CLONE_THREAD` takes the thread path, while
`flags == 0`/`SIGCHLD` with `child_stack == 0` takes the fork path. Everything else is
`-EINVAL` (`CLONE_VFORK`, `CLONE_VM` without `CLONE_THREAD`, `child_stack == 0`, a stack
that is not 16-byte aligned).

| Semantics | Implementation |
|---|---|
| address space | **not copied**: the child is bound to the parent's `space` - the same pair ledger, the same physical frames, so the parent's and the child's globals are literally the same words |
| frame | the child's initial trap frame sits at `child_stack - LA_FR_SIZE` (1040 B) with `SP = child_stack` and `A0 = 0`; every other register (including `ERA`) is **inherited from the parent's frame**, so the child resumes at the instruction **after** the `syscall` with return value 0 - exactly the legacy `clone(2)` contract glibc's `pthread_create` relies on |
| frame pages | unlike fork the thread stack is a **new** page and the kernel path has no demand faulting, so this code takes the pairs from the frame pool itself and **zeroes them unconditionally** (a pair may come back from another space; once swap exists "fresh pools are clean" is false); on failure it hands back what it took |
| process state slot | **shared** with the parent through a refcount. The binding happens inside the loader's critical section (`lx_thread_bind_hook`), not when the child first enters the ABI - one step late and the child lazily allocates its own slot, which shows up only as "a thread cannot close the main thread's fd" and reports nothing |
| fd / signals | one fd table, one set of signal actions and masks (both carried by the shared slot) |
| `CLONE_SETTLS` | `a2` goes into the child's `$tp`; `tls == 0` is treated as "not given" (`$tp` follows the parent) - **"set `$tp` to 0" cannot be expressed**, recorded in the known-limitations page |
| `PARENT_SETTID` / `CHILD_SETTID` | both write a 4-byte `int`; the kernel probes writability before creating (a bad pointer discovered afterwards would leave a running thread and an unexplainable return code) |
| `CHILD_CLEARTID` | on exit the word is cleared and one `FUTEX_WAKE` is issued - this is where `pthread_join` waits |
| `getpid` / `gettid` | `getpid(172)` returns the **thread group id** (leader task id), `gettid(178)` returns this thread; with the switch off `gettid` still returns `-ENOSYS` |

**Exit semantics.** `exit(93)` ends only the calling thread; `exit_group(94)` ends the whole
group. The teardown path splits on whether siblings are still alive:

- **siblings alive**: only this thread's `CLEARTID` is cleared and the waiter woken; it
  **must not** run `sock_close_all`, fd release or slot recycling, or the first thread to exit
  would take the whole group's fd table and socket handles with it;
- **last one out (including the leader)**: the full teardown runs (close sockets, release every
  fd, clear and unbind the slot) and the exit code is recorded on the leader's TCB, which is
  what `wait4` reads.

Threads are **not** registered in `s_child[]`: Linux's `wait4` cannot see threads, so the
group keeps exactly one "process" facing the outside.

**Ninth QEMU tier: 265 checks all `ok` + `exit code 0` (600 ms).** The ninth tier of
`tools/fork_gate.py` is the eighth (232 checks) plus 33 thread checks. Six readings matter most:

- `clone(CLONE_THREAD) > 0 -> 1 ok`, `thread PARENT_SETTID wrote the tid -> 6 ok`;
- `futex WAIT on CHILD_CLEARTID woke with 0 -> 0 ok` - the **real join path**
  (`CHILD_CLEARTID` + `FUTEX_WAIT`) covers CLEARTID, WAKE and WAIT in one go;
- `CLONE_SETTLS landed in $tp -> 1515847680 ok` (`0x5a5a0000`);
- `thread getpid == tgid (parent pid) -> 14 ok` - and the gate carries a **cross-check**: that
  14 must equal the fixture's `[fork] parent pid=`, otherwise "tgid is some constant" passes too;
- `exit_group from a thread reaped the group -> 6 ok` / `exit_group-from-leader code 7 -> 7 ok`,
  both issued inside a **child process** so the test process survives; in between,
  `fork after threads reaped -> 6 ok` shows a thread exit did not tear down the process's fd
  table or slot early.

Anti-false-green: the four argument gates (no `CLONE_VM`, `child_stack == 0`, misaligned stack,
`CLONE_VFORK`) must each be refused while the switch is on; `judge()` requires each of seven
thread readings to appear **exactly once**, and `self_check()` has seven bad samples (CLEARTID
not cleared, WAIT not woken, SETTLS not landed, `getpid` not equal to the tgid, two stacks at the
same address, wrong `exit_group` code, missing clone reading) that must be judged red.

**Zero change when off.** `arkrtos_elf_thread` / `arkrtos_port_paging_thread_frame` and the four
symbols `lx_thread_group_exit` / `lx_clone_thread` / `lx_thread_bind_hook` / `lx_thread_clear_tid`
are not emitted at all; the `off-zero-symbols` gate checks each with `nm`.

**Four pits only a real run exposes (all fixed).**

1. **`gettid` was written as 186.** 186 is x86_64's number; LA64 follows the asm-generic table
   and uses **178**. A wrong number does not fail loudly - a real glibc program asking for
   `gettid` would get `-ENOSYS`, while the fixture uses the same constant and stays green.
2. **The child does not "jump to the thread body"; it continues after the `syscall`.** The first
   fixture assumed the parent's continuation, so the child re-ran the parent's whole section of
   checks (the same line appearing twice, the second reading 0) and the body never ran. The real
   form is `if (thr == 0) { body(); }`, and that branch must sit between the `syscall` and the
   test with **no compiler-inserted stack access** (the child is already on its own stack): the
   fixture copies musl `__clone` and puts `syscall` / `bnez` / `jirl` in one asm block.
3. **`PARENT_SETTID` writes a 4-byte `int`.** The fixture received it through `lx_long` with an
   initial value of `-1` and read back `-4294967290` (low 32 bits are the tid, high 32 bits are
   still `0xFFFFFFFF`). Same class as the `getsockopt` pit in section 8.9; same fix: `unsigned int`.
4. **`FUTEX_WAIT` can be interrupted by the `SIGCHLD` of a child process in the same process
   (`-EINTR`).** The first requeue fixture retried only on `-EAGAIN`, so one legal interruption in
   the ninth tier was read as a false red (`rc10 = -4`). Real callers retry `-EINTR` too, so the
   fixture now retries `-EAGAIN` / `-EINTR` / `-ETIMEDOUT` with a 100 ms per-round, 60-round cap
   (it can neither hang nor turn "did not wait" into "waited").

**Boundaries written down.** `exit_group` kills tasks that may be running on another core and
there is no inter-core stop protocol, so the thread tier was only measured on **single-core**
QEMU. `execve` from a non-leader thread is **undefined** here: it replaces the calling task's
image while siblings still point at the old space (Linux ends the whole group and keeps only the
caller); it is neither implemented nor refused. There is no TLS module allocation; `SET_TID_ADDRESS` only reports the tid and stores
nothing. The robust list (99/100) and exit cleanup exist now (8.4b) and are accounted
per task, so any member of a thread group only settles its own chain.
A thread is an ordinary task: same priority, same time slice, no extra scheduling semantics.

### 8.11 Per-App task quota (`ARKRTOS_USE_APP_QUOTA`, 2026-11-24, tenth QEMU LA64 tier, 277 checks)

The tenth tier = the ninth tier (265 checks) + the quota section. It answers "can one App eat
the whole task table", which is not a new syscall: the loader hands out a non-zero owner id per
ELF load (`elf_app_owner_new()`), fork and `clone(CLONE_THREAD)` **inherit** it, and once the
owner's **live** task count reaches `ARKRTOS_APP_MAX_TASKS` creation fails --
`ARKRTOS_ELF_ERR_QUOTA` is mapped to `-EAGAIN` by `lx_fork_errno()`, plus one
`[appq] refused <fork|thread>: owner N quota M tasks reached` line per refusal.

**Why the cap is 3.** This board has `ARKRTOS_PAGING_APPS_MAX = 2` (at most parent + one child
process), and `ERR_QUOTA` and `ERR_BUSY` both map to `-EAGAIN`, so the return value cannot tell
which one refused. Only "a thread takes no new address space" separates them, so the fixture
fills the three slots with "one process + two threads" and then reads two **unambiguous**
refusals: a thread over the cap (cannot be BUSY) and a fork over the cap **while a space is
free**. 3 is also above the peak of 2 in the third (thread) section, so the two sections do not
interfere; the counting logic is independent of the constant, and the default 8 goes through the
same code.

**What the quota section reads (`[fork] quota …`)**: (1) fork under the cap succeeds; (2) a
thread takes the last slot; (3) fork over the cap returns `-11`; (4) thread over the cap returns
`-11`; (5) the blocked child is really reaped by `wait4` (the cap counts live tasks, so it has to
be alive); (6) a second thread under the cap succeeds; (7) fork still returns `-EAGAIN` **while
a space is free** (the cleanest reading of the section); (8) a third thread returns `-EAGAIN`;
(9) both threads rejoin through the real path (`CHILD_CLEARTID` cleared + `FUTEX_WAIT`);
(10) a fork succeeds immediately after the quota is released; (11) that child's exit code still
comes back as 3. Plus three `[appq]` **cross-evidence** lines: exactly 3 (2 fork + 1 thread),
one and the same owner, cap reading 3 -- without that line the caller sees only `EAGAIN` and
cannot tell "quota exhausted" from "address space exhausted".

**Two pits that only a real run exposes (both fixed).**

1. **Pipe reads do not block, so the child killed itself.** Here `lx_pipe_read()` returns
   `-EAGAIN` when the buffer is empty and a writer is still open (see the note on `LX_PIPE_BUF`;
   that is an existing trade-off and was not changed). The first fixture wrote
   `while (read(...) > 0) {}`; a negative value fell straight through, the child called `exit`,
   the quota was never filled, and the symptom was two reds at once ("thread was let through" and
   "nothing to reap") -- the `[DBG] cnt` live list no longer contained that tid. The fix is for
   the child to sleep on a 100 ms `futex(FUTEX_WAIT)` timeout and retry, leaving only on EOF(0)
   (that is a kernel-accounted futex timeout, not a user-space clock). **The sleep word must be a
   global inside the App window**: `lx_futex_key_task()` returns invalid for
   `uaddr < LA_WIN_VA`, so a word on the stack never gets woken.
2. **Classifying a thread stack under COW needs a hint window.** A thread frame lands at
   `child_stack - 1040`, and `clone` provides no `stack_size`, so `la_pg_stack_pair()` only knew
   `[hi-size, hi)` and classified the thread stack as "not a stack", installing read-only pages
   at fork time. The first `st.d` then raised a PME (`ecode=4`), while eentry starts with
   `addi.d $sp,$sp,-528`, so the exception frame hit the same page and nested forever without the
   C handler ever running. The fix: for a thread with `stack_size == 0`, classify by a 32 KB hint
   window below the stack top (`LA_PG_THREAD_STACK_HINT`). **That is a classification hint, not
   a stack-bottom guarantee** -- stack guards, bounds checks and overflow decisions must not rely
   on it.

**Boundaries written down.** The quota covers the **task table** only: the fd table is already
capped per process (`LX_MAX_FD`) and page frames are already bounded by the App's own 2 MB
window, so one App cannot exhaust either; **a per-App quota for the memory pool is still not
done** (it needs a per-owner ledger in the allocator). The number of concurrent processes is
bounded by `ARKRTOS_PAGING_APPS_MAX`; `ERR_QUOTA` and `ERR_BUSY` are both `-EAGAIN`, so only the
`[appq]` log separates them. See section 4 of [已知限制](已知限制.md) and section 7 SEC-G07 of
[安全认证准备](安全认证准备.md).

**Zero footprint when off.** `elf_app_owner_new`, `elf_app_task_count` and `lx_appq_log`, plus
the `app_owner` field in the TCB, produce nothing when `ARKRTOS_USE_APP_QUOTA=0`; the gate's
`off-zero-symbols` step checks them with `nm` one by one, and `features.h` holds the
`ARKRTOS_USE_APP_QUOTA=1 requires ARKRTOS_USE_ELF=1` `#error`.

### 8.12 `dup`(23) / `dup3`(24): fd-slot copies, standard-stream aliases, console references (2026-11-27, QEMU LA64, 56 checks)

This pair used to answer `-ENOSYS`; that debt is paid. They are **live in the default tier**:
no new switch, no footprint tier — `ARKRTOS_USE_LINUX_ABI=1` is enough, because making one fd
into two needs no new kernel mechanism, only a second copy of one fd-table slot.

**Semantics (aligned with Linux one by one; deviations stated in the open)**

| shape | answer |
|---|---|
| `dup(oldfd)` succeeds | returns the **lowest free number** (>= 3); the source and the copy are two independent descriptors, closing either leaves the other valid |
| `dup3(oldfd, newfd)`, `newfd >= 3` and free | returns `newfd` |
| `dup3(oldfd, newfd)`, `newfd >= 3` and in use | releases the old target first (a pipe end drops one reference), then installs the copy; returns `newfd` |
| `dup3(oldfd, 1)` / `(?, 2)` / `(?, 0)` | **takes a real slot** and records it as that standard stream's alias (`lx_proc_t.std_slot[]`). A standard stream is "0/1/2 without an alias", so `int saved = dup(1); dup2(fd, 1); ...; dup2(saved, 1);` works |
| `dup(0)` / `dup(1)` / `dup(2)` (source has no alias) | copies the **console object** (`LX_FD_CONSOLE`): writes reach the console, reads return EOF, `select` reports writable |
| source missing / `oldfd` out of range | `-EBADF` (9) |
| `dup3` with `oldfd == newfd` | `-EINVAL` (22) — the only difference between `dup3` and `dup2`, as in Linux |
| `dup3` with `O_CLOEXEC` in `flags` | **accepted**; the new slot records `FD_CLOEXEC`, and `lx_fd_close_cloexec()` releases it after a successful `execve` frame swap (section 5 item 4, 8.13). Before 2026-11-27 this returned `-EINVAL` on the grounds that "this layer has no exec-close semantics" - that ground no longer holds |
| any other flag bit | `-EINVAL` |
| `newfd >= LX_MAX_FD + 3` | `-EBADF` |
| fd table full (`dup` needs a new slot) | `-ENFILE` (23). Linux answers `EMFILE` when the per-process fd table is full; this layer has a single fixed table (not "kernel file table + per-process fd table"), and `pipe2` / `openat` already use `ENFILE` for the same condition, so `dup` follows the same convention rather than inventing a second answer |
| source or target is a socket | `-EINVAL` (22). **Explicitly unsupported**: a socket slot refers to an lwIP net handle, and two slots sharing one handle needs an open-file-description layer with reference counting, which this layer does not have; refusing honestly beats producing two handles that quietly break each other |

**What is copied is the slot, not the open file description — the one visible deviation.**
`dup` copies `kind / pipe / size / wr / path`; the two slots keep their own `off`. Pipes and the
console have no cursor, so semantics match Linux exactly; for a file opened with `openat` the two
copies **each read from their own current offset** (Linux shares it). This is a deliberate
trade-off: an open-file-description layer would put an indirection and a reference count in front
of every `read`/`write`, while this layer only exposes `openat` + `read` + `pread64` (section 4).
`pread64` never moves the cursor, so neither path is affected.

**Why a standard-stream alias (`std_slot[3]`) takes a real slot instead of sharing the source
slot.** Sharing cannot make the alias survive `close(source)` — after `dup3(w, 1)`, `close(w)`
must not invalidate fd 1. So when `fd 0..2` has an alias, `read` / `write` / `close` / `select`
route to that slot first; a second `dup3` onto the same stream **reuses the existing slot**
(drop the alias, then release it), so it still succeeds when the table is full. A forked child
copies `std_slot` 1:1 (the parent's aliases stay valid in the child).

**A trap only visible in hindsight.** `lx_fd_release()` used to decide "pipe?" by
`kind != LX_FD_FILE`. The new `LX_FD_CONSOLE` has `pipe == 0` and would have been treated as
"reader of pipe 0" and decremented it — a real pipe silently reaching zero blows up somewhere
else. The test is now explicit: `kind == LX_FD_PIPE_R || kind == LX_FD_PIPE_W`, so adding a
further fd kind cannot bend it again.

**Measured readout (`[dup]` lines, QEMU LA64, 56 checks all `ok`)**

```
[dup] write /mnt/ram/linux_dup rc=0 size=9792
[dup] pipe2 rc = 0 ok
[dup] dup(writer) is a new fd = 1 ok
[dup] write through the copy = 5 ok
[dup] read from the original reader = 5 ok
[dup] content matches = 1 ok
[dup] close the original writer = 0 ok
[dup] copy still writable after close = 3 ok
[dup] dup(999) -> -EBADF = -9 ok
[dup] dup3(1, 1, 0) -> -EINVAL = -22 ok
[dup] dup3(1, 6, O_CLOEXEC) returns 6 = 6 ok
[dup] F_GETFD(6) sees FD_CLOEXEC = 1 ok
[dup] dup3(999, 1, 0) -> -EBADF = -9 ok
[dup] dup3(writer, 1) returns 1 = 1 ok
[dup] write(1) lands in the pipe = 3 ok
[dup] close(1) drops the alias = 0 ok
[dup] dup3(reader, writer) overwrites = 4 ok
[dup] overwritten fd is no longer a writer = -9 ok
[dup] dup(0) is a console reference = 1 ok
[dup] read the console reference -> EOF = 0 ok
[dup] fcntl: pipe2 rc = 0 ok
[dup] F_GETFD on a fresh fd -> 0 = 0 ok
[dup] F_SETFD(FD_CLOEXEC) -> 0 = 0 ok
[dup] F_GETFD reads FD_CLOEXEC back = 1 ok
[dup] F_GETFL(reader) = O_RDONLY|O_NONBLOCK = 2048 ok
[dup] F_GETFL(writer) = O_WRONLY|O_NONBLOCK = 2049 ok
[dup] F_SETFL clearing O_NONBLOCK on a pipe -> 0 = 0 ok
[dup] F_GETFL after clearing shows O_RDONLY = 0 ok
[dup] F_SETFL setting O_NONBLOCK back -> 0 = 0 ok
[dup] F_GETFL after setting shows O_NONBLOCK again = 2048 ok
[dup] F_GETFL(console) = O_RDWR|O_NONBLOCK = 2050 ok
[dup] F_DUPFD(0) allocates a new fd = 1 ok
[dup] F_DUPFD_CLOEXEC(9) -> 9 = 9 ok
[dup] the F_DUPFD_CLOEXEC copy carries FD_CLOEXEC = 1 ok
[dup] unknown cmd 99 -> -EINVAL = -22 ok
[dup] F_GETFD on a bad fd -> -EBADF = -9 ok
[dup] PASS: dup new fd, data through the copy, alias survives close, dup3 redirects stdout,
            dup3 overwrites, console reference, O_CLOEXEC accepted with FD_CLOEXEC readback,
            fcntl FD/flags/dupfd, EBADF/EINVAL on the failure surfaces
[dup] finished after 0 ms, exit code 56
```

**How to read the 56 checks.** (0) The first 36 are dup/dup3 (groups 1-5) and the last 20 are fcntl(25) (group 6, broken down in 8.13). (1) Copying is not aliasing a name: `copy still writable after
close = 3` proves each descriptor holds its own writer reference; (2) `read -> EOF = 0` after
both ends are closed proves the count really reached zero (no leak, no early zero); (3) after
`dup3(writer, 1)`, `write(1)`'s bytes come back out of the **pipe**, proving `write` routes
through the alias instead of printing to the console; (4) `close(1)` only drops the alias and
leaves the source alone; (5) `dup3(reader, writer)` overwrites a number that is already a
writer, and `overwritten fd is no longer a writer = -9` plus `overwrite dropped the only writer
-> EOF = 0` together prove the "release, then install" order; (6) the failure surfaces
(`EBADF` / `old==new` / invalid source) each read back a specific negative errno, while the
`O_CLOEXEC` slot is now a **success** reading (`returns 6 = 6 ok` plus
`F_GETFD(6) sees FD_CLOEXEC = 1 ok`);
(7) the fixture **prints on fd 2** — case group 3 redirects fd 1 into a pipe, so printing on
fd 1 would be eaten by the fixture itself, a readout trap hit while writing it.

**Boundaries.** (1) sockets cannot be `dup`ed (reason above); (2) file copies keep independent
offsets (the only visible deviation); (3) `O_CLOEXEC` now has real semantics (fds are released
after the exec frame swap), and four fd-creating paths record it (`openat` / `dup3` / `pipe2` /
`socket`), plus `fcntl(F_SETFD)` (8.13); (4) `dup3`'s `newfd` ceiling is `LX_MAX_FD + 3`, above
which it returns `-EBADF` (Linux also returns `-EBADF` past `RLIMIT_NOFILE`, it just uses an
rlimit instead of a fixed table); (5) the host tier adds 94 assertions (`t_dup` 40 + `t_fdflags` 54 in
`test_linux_abi_host.c`) covering states that are awkward to build in a QEMU fixture, such as a
full table answering `-ENFILE`, reusing an existing alias slot when the table is full, and
`dup3(pw, 1, O_CLOEXEC)` marking the alias slot so fd 1 really returns to the console after an
exec.

### 8.13 `fcntl`(25): FD flags and status flags (2026-11-27, QEMU LA64 20 checks + 54 host checks)

`fcntl` is one of the first numbers glibc touches at start-up (`isatty` asks
`F_GETFL`, `__libc_fcntl64` tags fds with `FD_CLOEXEC`). This section covers the commands the
**fd table layer** can honour (the three record-lock commands share the same syscall and
live in section 8.16; `F_SETOWN` / `F_GETOWN` and `O_ASYNC` live in section 8.18);
everything else returns `-EINVAL`, because returning 0 would claim work that never happened.

| cmd | reply |
|---|---|
| `F_DUPFD`(0) | lowest free number at or above `max(arg, 3)`; `arg < 0` or past `LX_MAX_FD + 3` gives `-EINVAL`; a full table gives `-ENFILE` |
| `F_GETFD`(1) | `cloexec ? FD_CLOEXEC : 0` when the fd exists; `-EBADF` otherwise |
| `F_SETFD`(2) | only the `FD_CLOEXEC` bit is honoured (other bits in `arg` are ignored, as in Linux); returns 0 |
| `F_GETFL`(3) | returns `O_ACCMODE` + `O_APPEND` + `O_NONBLOCK` (plus `O_ASYNC` since U-29); a pipe reports the `O_NONBLOCK` bit it was created with (only if `pipe2` got `O_NONBLOCK`; the default is 0, i.e. blocking - section 6); the console returns `O_RDWR\|O_NONBLOCK` |
| `F_SETFL`(4) | changes `O_APPEND` / `O_NONBLOCK`, returns 0; **on a pipe both setting and clearing `O_NONBLOCK` are accepted** (since U-26 the pipe itself can block, so clearing is no longer an empty promise - same as Linux); since U-29 the `O_ASYNC` bit is honoured too when `ARKRTOS_USE_LINUX_SIGIO` is on (section 8.18) |
| `F_DUPFD_CLOEXEC`(1030) | same as `F_DUPFD`, new slot carries `FD_CLOEXEC` |
| `F_GETLK`(5) / `F_SETLK`(6) / `F_SETLKW`(7) | see section 8.16 (record locks: owner is the process, identity is the file path, unit is a byte range) |
| `F_SETOWN`(8) / `F_GETOWN`(9) | see section 8.18; with `ARKRTOS_USE_LINUX_SIGIO` off they fall back to `-EINVAL` (bit-identical to pre-U-29) |
| any other cmd | `-EINVAL` (and Linux also returns `EINVAL` for a command it does not know - not `ENOSYS`) |

**The semantics of `F_SETFL` on a pipe changed in U-26.** Under the old contract (pipes
always non-blocking) `F_SETFL` returned `-EINVAL` for "clear `O_NONBLOCK`", so that the 0
would not be an empty promise. Since U-26 made pipes **blocking by default** (section 6),
clearing the bit really blocks and setting it really polls, so both directions are honoured
with 0 exactly as on Linux. The four readouts below (clear -> 0 / read back `O_RDONLY` /
set again -> 0 / read back `O_NONBLOCK`) are the evidence for that change.

**Where `FD_CLOEXEC` is honoured: `execve`.** After loading segments, patching the trap
frame and resetting the heap cursor, `lx_execve()` calls `lx_fd_close_cloexec()`, which walks
the current process's fd slots and `lx_fd_release()`s every `used && cloexec` slot. The
position is deliberate: it is the one step on the path that cannot fail; anywhere earlier and
a later failure would force a rollback of someone else's fd table. One subtlety when taking
it back out: `dup3(old, 1, O_CLOEXEC)` produces a **standard-stream alias**, and alias slots
are invisible to `lx_fd_find()` (it returns -1 for any fd below 3), so `lx_dup3` resolves with
`(rc <= 2) ? lx_std_slot(rc) : lx_fd_find(rc)`. That gap was caught by the host assertion
"the pipe reader returns EOF after the exec": with the bit missed, fd 1 still pointed at the
pipe and the assertion read `got 3 want 0`.

**Measured readout (group 6 of the `[dup]` block, QEMU LA64)**

```
[dup] fcntl: pipe2 rc = 0 ok
[dup] F_GETFD on a fresh fd -> 0 = 0 ok
[dup] F_SETFD(FD_CLOEXEC) -> 0 = 0 ok
[dup] F_GETFD reads FD_CLOEXEC back = 1 ok
[dup] F_GETFL(reader) = O_RDONLY|O_NONBLOCK = 2048 ok
[dup] F_GETFL(writer) = O_WRONLY|O_NONBLOCK = 2049 ok
[dup] F_SETFL clearing O_NONBLOCK on a pipe -> 0 = 0 ok
[dup] F_GETFL after clearing shows O_RDONLY = 0 ok
[dup] F_SETFL setting O_NONBLOCK back -> 0 = 0 ok
[dup] F_GETFL after setting shows O_NONBLOCK again = 2048 ok
[dup] F_GETFL(console) = O_RDWR|O_NONBLOCK = 2050 ok
[dup] F_DUPFD(0) allocates a new fd = 1 ok
[dup] F_DUPFD_CLOEXEC(9) -> 9 = 9 ok
[dup] the F_DUPFD_CLOEXEC copy carries FD_CLOEXEC = 1 ok
[dup] unknown cmd 99 -> -EINVAL = -22 ok
[dup] F_GETFD on a bad fd -> -EBADF = -9 ok
```

`2048` is `O_NONBLOCK`, `2049` is `O_WRONLY|O_NONBLOCK` and `2050` is `O_RDWR|O_NONBLOCK` -
printed in decimal on purpose: those three are exactly the `asm-generic` bit values, and in
hex they would hide the "`F_GETFL` reports exactly the bits that were set, adding nothing"
contract. (This `pipe2` deliberately passes `O_NONBLOCK`; after U-26 a default pipe would
report `0` / `1`.)

**The 54 host checks** (`t_fdflags` in `test_linux_abi_host.c`) fill in what a QEMU fixture
cannot build: an alias slot from `dup3(pw, 1, O_CLOEXEC)` inherits CLOEXEC and fd 1 really
returns to the console after an exec (the pipe reader reads EOF once the original writer is
closed), `openat(O_CLOEXEC)` records the bit and the fd is released by an exec, `F_DUPFD(11)`
past the table gives `-EINVAL`, `arg < 0` gives `-EINVAL`, a `F_GETFL` copy follows its
source, and so on.

**Boundaries.** (1) record locks landed on 2026-11-27 (section 8.16), but they identify a
file by **path** only, the whole table is 8 slots and there is no deadlock detection;
(2) `F_SETOWN` / `F_GETOWN` / `O_ASYNC` landed on 2026-11-27 (section 8.18), but they only
report "this fd became readable (including EOF)"; the writable direction, real-time queueing,
`si_fd`, and process groups are all out, and the owner is a single task id;
(3) no `F_GETPIPE_SZ` / `F_SETPIPE_SZ` - the pipe buffer is a fixed 256 bytes (section 6).

### 8.14 `rseq`(293) and `getcpu`(168): restartable sequences and the CPU number (2026-11-27, 16 QEMU LA64 checks + 40 host checks)

`rseq` is the "lock-free critical section" primitive used by glibc and by the Rust standard
library: a program registers the `[start_ip, post_commit_offset)` range of a critical section
and its `abort_ip` in a `struct rseq_cs`, and the kernel consults that table **at the moment it
interrupts user execution** - if the PC falls inside the range it is moved to `abort_ip`, so the
program continues on the "start over" path instead of returning into the middle of a section
with a torn assumption. Without that, the ordering guarantees inside the section are fiction.

**Registration contract (`rseq(293)`).** Four arguments: `struct rseq *`, `rseq_len`, `flags`,
`sig`.
- `flags != 0` -> `-EINVAL`; `rseq_len != 32` -> `-EINVAL` (`struct rseq` is exactly 32 bytes in
  the Linux UAPI; a caller whose `sizeof` disagrees cannot read the fields back anyway);
- the address must be 32-byte aligned, otherwise `-EINVAL`;
- a `NULL` pointer **deregisters** (same meaning as Linux) and returns 0;
- on success `cpu_id_start` (offset 0) and `cpu_id` (offset 4) are both written as 0; **if the
  write fails the call returns `-EFAULT` and leaves no registration behind** - a successful
  registration that lets the program read a bogus CPU number is far worse than an outright
  refusal;
- the fourth argument `sig` (the signature used by the abort instruction stream) is not looked
  at by this layer: the kernel never executes the user's abort instruction, the signature is
  only interpreted by the abort path, and ignoring it changes nothing semantically.

The other fields of `struct rseq` (`flags` / `node_id` / `mm_cid` and so on) are not written by
this layer and keep whatever the program put there.

**`getcpu(168)`.** Two 4-byte out-parameters (either may be `NULL`; out of range is `-EFAULT`)
plus a `cache` argument that is ignored. On a single-core machine both the CPU number and the
NUMA node number are 0 - and they **must agree with the `cpu_id` that `rseq` writes back**: two
places reporting different numbers would show the program two different worlds on the fast path
(reading `rseq.cpu_id`) and the slow path (`getcpu`), and it would then treat "migrated CPUs" as
fact when reasoning about memory ordering.

**The two abort points.** This layer consults `rseq_cs` only where user execution is genuinely
interrupted:

| Moment | Call site | Why there |
|---|---|---|
| signal delivery | in `lx_signal_deliver()`, **before** `lx_sig_build_frame()` | Linux runs `rseq_ip_fixup` before the signal handler too. Fixing the PC before the frame is laid down means the saved frame naturally holds `abort_ip`, and `rt_sigreturn` resumes on the abort path instead of in the middle of the critical section |
| tick preemption | the switch point of `la_trap_c()` on LA64 (before the stack is actually switched), and only when the interrupted context is user mode (`PRMD & 3 != 0`) | being interrupted by the timer and by a signal are the same event to the program: a live `rseq_cs` means it is inside the critical section |

The predicates match Linux one for one: `rseq` registered / `rseq_cs` non-zero / the block is
readable / `version` and `flags` are both 0 / the PC lies inside
`[start_ip, start_ip + post_commit_offset)`. If any of them fails the PC is **left alone**.
Clobbering a context that is not on this data is far more dangerous than doing nothing.

**The order is hard: clear `rseq_cs` first, then move the PC.** The other way round, if moving
the PC succeeded and clearing failed, the program would come back on the abort path, see "cs is
still live" and run the critical section a second time.

**One deliberate difference from Linux (stated openly).** When `rseq_cs` has a non-zero
`version`/`flags`, Linux raises `SIGSEGV` and kills the process; this layer has no place to
deliver a signal at the switch point, so it **only clears the pointer and leaves the PC alone** -
the conservative direction: better to lose one abort (degrading to "as if there were no rseq")
than to jump into a block with invalid contents.

**Measured readout (the `[rseq]` lines, QEMU LA64)**

```
  [rseq] write /mnt/ram/linux_rseq rc=0 size=8232
  [rseq] exec rc=0
  [rseq] getcpu rc = 0 ok
  [rseq] getcpu cpu = 0 ok
  [rseq] getcpu node = 0 ok
  [rseq] rseq register rc = 0 ok
  [rseq] cpu_id_start written 0 = 0 ok
  [rseq] cpu_id readback 0 = 0 ok
  [rseq] signal aborted the critical section = 1 ok
  [rseq] post-commit did not run = 0 ok
  [rseq] rseq_cs cleared by the abort = 0 ok
  [rseq] handler ran once = 1 ok
  [rseq] normal critical section completed = 1 ok
  [rseq] normal section not falsely aborted = 1 ok
  [rseq] rseq_cs cleared by the commit = 0 ok
  [rseq] rseq len != 32 -> -EINVAL = -22 ok
  [rseq] rseq flags != 0 -> -EINVAL = -22 ok
  [rseq] rseq(NULL) deregisters = 0 ok
  [rseq] PASS: getcpu both outputs zero, rseq registered with cpu_id written, signal delivery aborted the critical section at abort_ip with rseq_cs cleared, commit path completed, invalid contract refused, NULL deregisters
  [rseq] finished after 0 ms, exit code 57
```

`exit code 57` is the fixture's own success code; `post-commit did not run = 0` is positive
evidence that the commit half never ran, and `normal section not falsely aborted = 1` is the
negative control that nothing is aborted when no signal arrives - only the pair shows that the
abort is decided by the range rather than by "jump whenever `rseq_cs` is live".

**The 40 host checks** (`t_rseq` in `test_linux_abi_host.c`) cover the states a QEMU fixture
cannot build: `getcpu` three positive and two negative cases (`NULL` out-parameter, out of range
`-EFAULT`); every rejection face of the registration contract (`len` / `flags` / alignment /
`NULL` deregistration / `-ESRCH`); `g_to_budget=1` to force "the `cpu_id` write fails" ->
`-EFAULT` with no registration left behind; at the preemption point, a PC inside the range ->
returns 1 with `ERA` equal to `abort_ip` and `rseq_cs` cleared to 0, while a PC outside the
range / `cs == 0` / `version != 0` / an unreadable `cs` pointer -> returns 0 and leaves the PC
alone; invalidation after deregistration and after `reset`; and the signal-delivery point uses
`g_export_era` to capture "the user PC at the moment the frame was exported", turning "the
signal frame really holds `abort_ip`" into a real assertion instead of an inference.

**Why the fixture is built `-O0` (a real toolchain defect that was found).** This fixture uses
`&&label` to delimit the three endpoints of the restartable sequence. The toolchain in use (the
LoongArch port of LoongIDE GCC 8.3) generates unreliable code for taking the address of a label
under `-O1`/`-O2`:
- when the abort block is unreachable in the CFG it is **deleted outright** (the store of
  `s_aborted = 1` disappears) and `&&label` then lands on the start of a neighbouring block
  (measured: `start` and `abort` both became the entry of `linux_main` and `post` became 0, so
  `[start, start+post)` degraded to an empty range and the abort never fired);
- with several labels on the same page the immediates are computed wrongly (minimal repro: two
  labels 8 bytes apart compile to a difference of -4).

The fix is two-fold: build the fixture with `-O0` (addresses are correct at that level), and
add a **branch on a `volatile` that is always 0**, `if (s_zero_guard) goto cs_abort;` - giving
the abort block an edge the compiler can see, so the block survives and the label addresses come
out right; at run time the branch is never taken. That `-O0` and the comment in
`example/qemu_loongarch64/Makefile` exist for exactly this reason.

**Gates.** In `tools/linux_abi_gate.py` `syscall-table` grows from 27 to **30** entries
(`RSEQ` 293 and `GETCPU` 168 join the number table), `qemu-readout` gains "16 rseq checks all
ok + the PASS line + `exit code 57`", and `parser-selfcheck` grows from 12 bad samples to **16**
(rseq block: PASS line removed, exit code changed to 1, a FAIL line inserted, one check missing).

**Boundaries.** (1) single core: `cpu_id`/`node_id` are always 0, there is no migration and no
`mm_cid`; (2) the tick-preemption path has no fixture on this machine (the fixture only
exercises the signal-delivery point); the two host assertions about preemption cover the same
`lx_rseq_fixup`; (3) invalid `rseq_cs` contents only clear the pointer, the task is not killed
(see "one deliberate difference from Linux" above); (4) `RSEQ_FLAG_UNREGISTER` is not
implemented (`flags != 0` is always `-EINVAL`).

### 8.15 Cross-task semantics of a blocking pipe (U-26, 2026-11-27, QEMU LA64 fork tier, 20 checks)

Section 6 covers what one **execution stream** can prove (data present, EOF,
`-EPIPE` + SIGPIPE). "An empty read suspends until someone writes" and "a full
write suspends until someone drains" need two streams - a single stream that
suspends never comes back, so it is not a falsifiable readout. This group
therefore lives in `example/qemu_loongarch64/elf/linux_fork.c` (only built with
`ARKRTOS_USE_LINUX_FORK=1`, run once by **each of the ten tiers** of
`tools/fork_gate.py`).

**Group A: a blocking read waits for someone else to write**

1. The parent creates `pipe2(..., 0)` (blocking default) and `clone`s a child.
2. The parent **closes its own write end first** - the only writer is now the
   child, so the parent's blocking read must really suspend.
3. The child busy-waits 60 ms (`clock_gettime` polling, no `nanosleep`), writes
   `"abc"`, closes the write end and exits 11.
4. The parent's read returns `3` with the content `"abc"`, and the **wall clock
   is >= 40 ms**.
5. With the child gone and every write end closed, the parent's next blocking
   read returns `0` **at once** (EOF is a return condition; it must not burn a
   100 ms slice for nothing).
6. `wait4` reaps the child and reports exit code `11`.

**Group B: a blocking write waits for someone to drain**

1. The parent creates a data pipe and a status pipe.
2. The parent first writes 256 `'A'`s to **fill** the ring buffer, `clone`s the
   child, then writes a 300-byte pattern (`(uint8)(i & 0xFF)`) - not a single
   byte fits, so it must suspend.
3. The child busy-waits 60 ms, drains the 256 `'A'`s, then drains the 300 bytes,
   comparing **byte by byte** with the pattern.
4. The parent's write returns `300` (a blocking write is **complete**, not a short
   write) and the **wall clock is >= 40 ms**.
5. `wait4` reaps the child, exit code `13`.
6. The child writes back, into the status pipe, two 5-byte numbers: "556 bytes
   read in total" and "pattern fully matched (0)" - a cross-process readout cannot
   rest on the exit code alone, which holds only 8 bits.

**Measured readout (`[fork]` lines, QEMU LA64 fork tier)**

```
[fork] cross-task blocking read pipe2 == 0 -> 0 ok
[fork] clone for blocking reader > 0 -> 1 ok
[fork] blocking pipe read waited 61 ms
[fork] blocking pipe read returned 3 -> 3 ok
[fork] blocking pipe read content -> 1 ok
[fork] blocking pipe read waited >= 40 ms -> 1 ok
[fork] blocking pipe read after writer exit -> EOF -> 0 ok
[fork] EOF read did not sleep a slice -> 1 ok
[fork] blocking reader child reaped -> 12 ok
[fork] blocking reader child exit 11 -> 11 ok
[fork] cross-task blocking write pipe2 == 0 -> 0 ok
[fork] status pipe2 == 0 -> 0 ok
[fork] fill the 256-byte pipe buffer -> 256 ok
[fork] clone for blocking writer > 0 -> 1 ok
[fork] blocking pipe write waited 161 ms
[fork] blocking pipe write wrote all 300 -> 300 ok
[fork] blocking pipe write waited >= 40 ms -> 1 ok
[fork] blocking writer child reaped -> 12 ok
[fork] blocking writer child exit 13 -> 13 ok
[fork] child report came back -> 5 ok
[fork] child read 556 bytes in total -> 556 ok
[fork] child saw the exact byte pattern -> 0 ok
```

(`waited 161 ms` rather than 60 ms: the parent's blocking write has to wait for
the child to drain in two rounds, which necessarily crosses at least one 100 ms
slice. The checks only assert ">= 40 ms" and never pin an exact millisecond.)

**A real defect this group exposed and fixed: `SIGCHLD` must not interrupt a
blocking call.** In the first implementation `lx_sig_pending_unblocked()` only
tested "pending & ~blocked != 0", so after `wait4` reaped a child (this layer does
**not** clear the SIGCHLD pending bit, and neither does Linux) the parent's next
blocking `read`/`write` returned `-EINTR` immediately. Linux's `get_signal()`
discards pending signals whose **default action is "ignore"** (SIGCHLD / SIGCONT /
SIGURG / SIGWINCH under `SIG_DFL`, plus explicit `SIG_IGN`) and returns 0: no frame
is laid down and the syscall is not interrupted. The predicate now looks at the
per-signal disposition, one bit at a time. The host futex `-EINTR` case still
proves itself with a SIGUSR1 that has a handler installed, so its semantics are
unchanged.

**Gate.** All ten `EXPECT_*_CHECKS` in `tools/fork_gate.py` went up by 20
(47 / 52 / 60 / 147 / 167 / 178 / 194 / 232 / 265 / 277; after U-27 each gains another
32, see section 8.16), and three new anchors
went into the mandatory list of `judge()`:
`blocking pipe read waited >= 40 ms -> 1 ok`, `blocking pipe write wrote all 300 -> 300 ok`,
`child saw the exact byte pattern -> 0 ok` - counting only the total would let "one
check short, one check extra" produce the same number, which is false green.

**Boundaries.** (1) Single core: two processes sleeping on the same pipe at the
same time is unverified; (2) wait granularity is a 100 ms slice; (3) 256-byte
buffer, at most two pipes; (4) blocking reads and writes need the port to provide
`arkrtos_port_svc_restart()` (without replay a read gives `-ENOSYS` and a write
gives the bytes already written); (5) `SA_RESTART` is not implemented and `-EINTR`
is a defensive path.

### 8.16 `fcntl` record locks (5 / 6 / 7): byte-range locks across processes (2026-11-27, 32 QEMU LA64 fork-tier checks + 56 host checks)

**Why a separate section.** A POSIX record lock belongs to the **process**, not to an fd,
not to a task and not to an open file description. Three rules follow, and all three are
honoured here:

* taking a lock twice in the same process never conflicts with itself - it replaces that
  process's old lock on the range (cut / split);
* `close` on **any** fd of that file releases **all** of this process's locks on it - that
  is Linux semantics (the lock belongs to the process, and `close` cannot know whether
  other fds still refer to the same file);
* locks are **not inherited across `fork`** (the child gets a fresh process slot) but
  **survive `execve`** (`execve` does not touch the process slot).

A single execution stream cannot prove either of the last two: one owner never conflicts,
and `close` release cannot distinguish "per fd" from "per process". The falsifiable
readouts therefore live in the `ARKRTOS_USE_LINUX_FORK=1` fixture (the same `linux_fork.c`
as section 8.15, run by all ten tiers of `tools/fork_gate.py`); the host side only covers
**capacity** and argument checking.

**Data and identity.** `s_rslk[LX_RSLK_MAX]` (8 slots), each
`{used, type, owner, pid, start, len, path[LX_PATH_MAX]}`, `owner = process slot + 1`
(0 = empty). File identity is the **path only** - there is no inode here, so hard links and
bind mounts ("different paths, same file") cannot be told apart; that goes into the known
limitations. A full table returns `-ENOLCK` and **never evicts somebody else's lock**:
silently dropping a lock another process is relying on is far worse than refusing.

**Layout.** Parsed as the **64-bit** asm-generic `struct flock`: `l_type@0`, `l_whence@2`,
`l_start@8`, `l_len@16`, `l_pid@24`, 32 bytes. Fields are read and written little-endian
byte by byte (no `memcpy` into a struct: host and target word sizes differ). The high 32
bits of `off_t` **must be the sign extension of the low 32**; anything else gives `-EINVAL`
- truncating an out-of-range offset into a "plausible" fake range is worse than refusing.
`l_len == 0` means "to EOF" (upper bound `0xFFFFFFFF`), and `start + len` overflow is also
clamped to EOF.

**How the three commands answer.**

| cmd | answer |
|---|---|
| `F_SETLK`(6) | install when there is no conflict; a conflict returns `-EAGAIN` (as on Linux - only `F_SETLKW` may wait) |
| `F_GETLK`(5) | with a conflict, report the one with the **smallest start** (Linux picks the lowest f_pos too), filling all five fields; without a conflict only `l_type` becomes `F_UNLCK` and the other four fields are **left untouched** (Linux leaves them too). Asking with `l_type = F_UNLCK` returns `-EINVAL` |
| `F_SETLKW`(7) | records "who waits on which fd" (`s_rslk_wait[task id]`, 4 bytes), sleeps one `LX_WAIT_SLICE_MS = 100` slice and returns `LX_RET_RETRY` to replay; an unblocked pending signal gives `-EINTR`; a port that cannot replay gives `-ENOSYS` |

Conflict means: same path + overlapping ranges + "I want to write or it wants to write"
(two read locks do not conflict). `F_UNLCK` cuts: full coverage deletes the entry, a head
or tail trim shortens it, a hole in the middle splits it in two - a split needs a free
slot, so the code **counts first and mutates after**, returning `-ENOLCK` when short.

Wakeups happen only on **unlock** and **release**, and only wake: the decision is made in
the replaying round (the condition can change between wake and replay; spurious wakeups are
harmless). `lx_fd_release()` calls `lx_rslk_release_path(slot, path)` for a file fd; a reset
process slot calls `lx_rslk_release_owner(slot)` plus an indiscriminate wake-all.

**Measured readouts (`[fork]` lines, QEMU LA64 fork tier; all 32 lock checks ok)**

```
[fork] lock parent F_SETLK [0,100) -> 0 ok
[fork] lock own lock is not a conflict -> 2 ok
[fork] lock child SETLK on parent lock == -EAGAIN -> -11 ok
[fork] lock child disjoint SETLK -> 0 ok
[fork] lock child SEEK_END SETLK -> 0 ok
[fork] lock parent GETLK disjoint start is 200 -> 200 ok
[fork] lock parent GETLK eof pid is child -> 6 ok
[fork] lock child GETLK type is WRLCK -> 1 ok
[fork] lock child GETLK pid is not self -> 1 ok
[fork] lock child GETLK start is absolute 0 -> 0 ok
[fork] lock child GETLK whence is SEEK_SET -> 0 ok
[fork] lock waiter blocked >= 40 ms -> 1 ok
[fork] lock free after child close (conflict range) -> 0 ok
[fork] lock child reaped -> 12 ok
```

The `pid` returned by `F_GETLK` is the **child's** task id 6, not the caller's; `whence` is
rewritten to `SEEK_SET` and `start` is absolute - the three together rule out an
"echo the request back" fake implementation. The blocking check never compares absolute
timestamps: the child reports `'P'`, the parent deliberately waits 100 ms before unlocking,
and the child measures its own wall clock at >= 40 ms.

**Gate.** `tools/fork_gate.py` has `LOCK_CHECKS = 32`, and all ten `EXPECT_*_CHECKS` add it
(79 / 84 / 92 / 179 / 199 / 210 / 226 / 264 / 297 / 309); `judge()` pins three anchors:
`lock child GETLK pid is not self -> 1 ok`, `lock waiter blocked >= 40 ms -> 1 ok`,
`lock free after child close (conflict range) -> 0 ok`. Comparing totals alone is not
enough - "drop one, add one" can add up to the same number.

**Coverage accounting (this change re-anchored `arkrtos_linux_abi` from 98.82% to 97.00%).**
The record-lock section adds roughly 730 lines, of which **61 are unreachable on the host
by construction**: with `ARKRTOS_USE_LINUX_FORK=0` the host build has `lx_proc_cur()`
returning slot 0 always, so every lock in the table has the same owner and the
`owner == owner` branch in `lx_rslk_conflict()` skips every used entry - the conflict scan
body, the split-on-install path, the waiter wake table, the blocking sleep and the
`F_GETLK` hit write-back are all unreachable. What the host can observe is **capacity**
(8 slots, the 9th gets `-ENOLCK`) plus argument checking (56 host checks). Those 61 lines
are covered by the ten `fork_gate` tiers on the target (the readouts above are the
evidence); they are not turned into dead code or `#if`-ed fake branches just to make a
number look better.

**Boundaries.** (1) No deadlock detection: a cyclic `F_SETLKW` wait sleeps until some lock
is released or a signal arrives, `EDEADLK` is never returned; (2) no OFD locks
(`F_OFD_SETLK`) and no leases (`F_SETLEASE`); (3) one table of 8 slots (`-ENOLCK`), not one
per inode; (4) no `/proc/locks`; (5) only the 64-bit `struct flock` (LA64 is the only Linux
ABI carrier here; the 32-bit 16-byte layout has no carrier); (6) single core: two processes
sleeping on the same lock at once is unverified (wake is broadcast and the decision is made
on replay, so it should not be lost in principle).

### 8.17 Scheduling policies (U-28): the kernel deadline class and the twelve `sched_*` numbers (2026-11-27, 63 QEMU LA64 checks + 52 + 633 host checks)

**Why this section needs two places to prove it.** There are two kinds of readout here, and
lumping them together can only prove half of each:

* **numbers, arguments, error codes** (the translation layer) - round trips such as
  "`sched_setscheduler(0, SCHED_FIFO, {88})` returns 0, then `sched_getscheduler(0)` returns 1
  and `sched_getparam(0)` returns 88" are falsifiable on QEMU;
* **selection order and budget** (the kernel EDF class) - "who runs first" on QEMU depends on
  wall clock and scheduling jitter, so it is not a falsifiable readout. This half lives in
  `test_task_host.c` only, driven by a controllable `arkrtos_kernel_tick` and TCBs arranged by
  hand (`test_deadline_class`, 52 checks).

Only both together are complete evidence: with the first alone an EDF that is really ordered
by priority still stays green; with the second alone a mis-translated syscall table still stays
green.

**Kernel side: one deadline class, not a CFS.** This kernel is a fixed-priority preemptive
scheduler. U-28 did not turn it into a weighted-fair scheduler; it inserts a deadline class in
front of the fixed priorities (Linux layers the same way: `stop > dl > rt > fair > idle`):

* `arkrtos_dl_top()` selects before **all** fixed-priority tasks: the earliest absolute deadline
  (`dl_abs_tick`) wins, ties go to the smaller task id; throttled tasks, tasks not in
  `READY`/`RUNNING` and secondary-core tasks are skipped. Both `arkrtos_ready_top()` and
  `sc_scan_best()` ask it first, so "the deadline class outranks every fixed priority" is one
  fact on both selection paths, not two separate copies.
* Each deadline task carries six numbers in its TCB: `dl_runtime_ns/dl_deadline_ns/dl_period_ns`
  (the original parameters, for inspection and bandwidth maths) and
  `dl_runtime_tick/dl_deadline_tick/dl_period_tick` (converted to ticks, for the scheduler), plus
  `dl_budget_tick`, `dl_abs_tick`, `dl_throttled` and `dl_active`.
* Time parameters are converted to ticks by **rounding up**: a 1 ns budget must not become 0
  ticks (which would mean "set but not set"); rounding up is monotonic, so
  `runtime <= deadline <= period` still holds after conversion.

**Admission.** `Σ(runtime/period) <= 95%` (Linux's default RT bandwidth); exceeding it returns
`-EBUSY` (not `-EINVAL`: the parameters are legal, the bandwidth is not). The task's **own old
share is subtracted first** - otherwise re-setting the same task is pushed out by its own
previous bandwidth. The arithmetic uses the original ns parameters, not ticks converted back,
because ticks were rounded up and inverting them biases the ratio.

**Settlement per tick (`arkrtos_dl_tick()`, inside the tick body, before `sched_tick_sweep`).**

1. First the **period boundary**: when `dl_abs_tick` is reached or passed, advance one period,
   refill the budget, clear throttling and put the task back on the ready set; if it is still
   behind after advancing (the system is already overloaded), re-align to `now + period`
   directly and **do not replay a run of historical budgets** - that would turn one overload
   into an avalanche.
2. Then charge **the current task one tick**: `arkrtos_current_task_id` is exactly the task this
   tick interrupted, so charging it charges "the tick that really owns the CPU". The idle task
   (`<= 0`) charges nobody.
3. When `dl_budget_tick` hits zero the task is throttled: `dl_throttled = 1` and it is **removed
   from the ready set**. It must not be "skipped when selecting" - that would leave it at the
   head of the bucket and block ordinary tasks of the same priority forever.
4. With only **one** deadline task there is no throttling (as on Linux): with no peer of the same
   class there is nobody to yield to, and throttling would only throw budget away; the budget is
   still accounted, so a second task added later does not find a whole free round already saved up.

**Linux side: the twelve numbers.**

| nr | name | answer here |
|---|---|---|
| 118 | `sched_setparam` | priority only, policy unchanged; `priority != 0` on `OTHER`/`BATCH`/`IDLE` gives `-EINVAL`, and `DEADLINE` gives `-EINVAL` (same as Linux: use `sched_setattr`) |
| 119 | `sched_setscheduler` | `pid` / `policy` / `sched_param*`; `DEADLINE` has no field for the triplet, so `-EINVAL` |
| 120 | `sched_getscheduler` | reads `sched_policy` from the TCB |
| 121 | `sched_getparam` | derived from the kernel priority: `100 - priority` for FIFO/RR, 0 otherwise |
| 122 / 123 | `sched_set/getaffinity` | single core: `len >= sizeof(unsigned long)`, mask accepts only bit 0 and it must be set, else `-EINVAL`; reads back 1 |
| 124 | `sched_yield` | moves itself to the tail of its priority bucket (a no-op when alone) and posts a reschedule |
| 125 / 126 | `sched_get_priority_max/min` | 99 / 1 for FIFO/RR; 0 for `OTHER`/`BATCH`/`IDLE`/`DEADLINE`; `-EINVAL` for an unknown policy |
| 127 | `sched_rr_get_interval` | rotation = 10 ticks x 500 us = 5 ms (`sec=0 / nsec=5000000`) |
| 274 | `sched_setattr` | `struct sched_attr` read field by field as bytes (VER0 is 48 bytes; `size < 48` gives `-EINVAL`); any non-zero syscall `flags` or `sched_flags` gives `-EINVAL`; `DEADLINE` goes to `arkrtos_task_dl_set()`, admission failure gives `-EBUSY`; the three time parameters must all be 0 on non-deadline policies |
| 275 | `sched_getattr` | writes back the same layout; with `size >= 56` the two util fields are written as 0 (this kernel does not track utilisation, but the fields exist and must get a defined value, not garbage) |

Policy placement (fixed priority means "smaller number is higher"):

| policy | Linux parameter | kernel priority |
|---|---|---|
| `SCHED_FIFO`(1) / `SCHED_RR`(2) | `priority` 1..99 | `100 - priority` (99 -> 1, 1 -> 99) |
| `SCHED_OTHER`(0) / `SCHED_BATCH`(3) | `nice` -20..19 | `128 + nice` (108..147) |
| `SCHED_IDLE`(5) | `priority` must be 0 | 147 (last slot of the normal range, **not** "runs only when idle") |
| `SCHED_DEADLINE`(6) | only the triplet via `sched_setattr` | the kernel deadline class |

**Where `policy` lives.** In the TCB (`sched_policy`), cleared by `arkrtos_task_register()`. No
private ABI-layer table: task slots are reused, and a private table would eventually drift out of
sync with them (a reused slot would carry the previous task's policy).

**Why `SCHED_IDLE` / `SCHED_BATCH` are only "approximate".** This kernel has no CFS: `nice` only
places a task in one slot of the normal range, with no weights and no delayed wakeup;
`SCHED_IDLE` lands in the last slot, so every other ordinary task outranks it, but it is not
"runs only when the system is idle" - there is no idle-yield rule in the kernel. Group scheduling
and cgroup bandwidth are not implemented either, nor are `SCHED_RESET_ON_FORK` /
`SCHED_FLAG_RECLAIM` / `SCHED_FLAG_DL_OVERRUN`: accepting a flag and not doing it turns the
caller's expectation into a lie, so a non-zero flags field always returns `-EINVAL`.

**Measured readouts (`[sched]` lines, QEMU LA64 fork tier, all 63 checks ok; the nine anchors
the gate pins are shown)**

```
[sched] getscheduler default -> 0 ok
[sched] getparam FIFO priority is 88 -> 88 ok
[sched] rr interval nsec is 5000000 -> 5000000 ok
[sched] setattr DEADLINE 100% bandwidth -> EBUSY -> -16 ok
[sched] setattr DEADLINE 1ms/2ms/100ms is admitted -> 0 ok
[sched] getattr policy is DEADLINE -> 6 ok
[sched] getattr DEADLINE deadline round-trips -> 2000000 ok
[sched] setaffinity short len -> EINVAL -> -22 ok
[sched] sched_yield -> 0 ok
```

**Host readouts (`test_deadline_class` in `test_task_host.c`, 52 checks).** Assertions, one by
one: the three hard constraints (`runtime == 0`, `runtime > deadline`, `deadline > period` each
return `-1` PARAM); parameters booked (1 ms / 2 ms / 100 ms = 2 / 4 / 200 ticks,
`dl_abs_tick = now + 4`, bandwidth 10,000,000 ns/s); 100% bandwidth gives `-EBUSY` and **not a
single field of the slot is half-written**; a second task at 1% is admitted (bandwidth totals
20,000,000); EDF picks the earlier deadline (**the later-set** task wins, so it is not "by task
id"); at the period boundary the order reverses after advancing 200 ticks and refilling; with two
present, running out of budget throttles (**the boundary refill is followed by another 1-tick
charge in the same tick, so the assertion is `budget == 1`, not 2**); a throttled task is not
selected and leaves the ready chain; the next boundary refills and re-adds it; `dl_clear` clears
the fields, releases the bandwidth, stays in the ready set and silently ignores an out-of-range
id; a single deadline task is not throttled but is still charged; falling more than one period
behind re-aligns (`abs == tick + 200`, `budget == 1`); `kill` releases the bandwidth (slot
INVALID, `dl_active = 0`, leaves the ready chain, bandwidth back to 0).

**Coverage (no re-anchor).** The `linuxabisched` tier clears the sched section of
`arkrtos_linux_abi` from **24 uncovered lines to zero**: the host stub's copy budgets
(`g_from_budget` / `g_to_budget`) already express "first block succeeds, second fails", so every
`-EFAULT` branch of `copy_from_user` / `copy_to_user` is reachable - none of them is written off
as "unreachable by construction". With seven tiers merged, `arkrtos_linux_abi` goes from 96.55%
to **97.25%** (the 97.00% baseline is untouched); `test_linux_abi_host.c` grows from 606 to
**633 checks** (27 new error-face checks).

**Gate.** `tools/fork_gate.py` has `SCHED_CHECKS = 63`: the `[sched]` prefix is counted
separately, so none of the ten `EXPECT_*_CHECKS` has to change (those are the `[fork]` prefix).
The nine anchors above are verified one by one, and the bad-log self-test adds three sched
mutations (drop one line, insert a FAIL, change a number). In the fixture the six deadline checks
are split on `ARKRTOS_USE_SCHED_DEADLINE` - with the class on they expect `-EBUSY` and the
triplet, with it off they expect `-EINVAL` and zeros; both branches have the same count, so
`SCHED_CHECKS` does not vary by tier. `tools/linux_abi_gate.py`'s `EXPECT_SYS` compares all 12
numbers against asm-generic (118-127, 274/275); in the default tier those twelve answer
`-ENOSYS` truthfully.

**Boundaries.** (1) No CFS (see above), `nice` only places the task; (2) `SCHED_IDLE` is not real
idleness; (3) no group scheduling and no cgroup bandwidth; (4) `SCHED_RESET_ON_FORK` / `RECLAIM`
/ `DL_OVERRUN` all return `-EINVAL`, never a pretence; (5) affinity is CPU0 only (single core);
(6) deadline parameters are stored as `uint32` ns, so anything above 4.29 s returns `-EINVAL`;
(7) the throttling granularity is one tick (500 us), not an `hrtimer`-level refill as on Linux;
(8) there is no `SCHED_FLAG_DL_OVERRUN` overrun detection - overrun is only contained by
throttling.

### 8.18 Asynchronous I/O (U-29): `F_SETOWN` / `F_GETOWN` / `O_ASYNC` + SIGIO (2026-11-27, 24 QEMU LA64 checks + 44 host checks)

**What this is, and what it is not.** Linux signal-driven I/O has three layers:
`fcntl(F_SETOWN)` records who to notify, `F_SETFL(O_ASYNC)` arms the fd, and the kernel
delivers one `SIGIO` when the fd changes state. This section delivers **one SIGIO when a pipe
fd becomes readable (including EOF after every writer closed)** and nothing else:

* the writable direction (a `POLLOUT`-style notification) is not done - the write path has no
  "space became available" event (a blocking write sleeps and replays, it is not notified);
* no edge-triggered de-duplication: every `lx_pipe_wake(pi, 0)` delivers once, the same tier
  as Linux `SIGIO` (Linux does not promise one per byte either);
* no real-time signal queueing (`sigqueue` / `si_fd` / `si_band` from `SA_SIGINFO`); `SIGIO`
  is plain signal 29 and goes through the standard `rt_sigaction` handler;
* no process groups: `F_SETOWN` with a negative value is `-EINVAL` and bumps
  `s_sigio_pgrp_refused` - **it is never recorded as if it had worked**.

**Why scan the fd table instead of keeping a registry.** The truth is the fd slot itself: does
this pipe read end carry `O_ASYNC` and an owner? A registry would need synchronisation in
`close` / `dup` / `execve` / process exit / fork, and one missed spot would signal an fd that
no longer exists. A scan is `LX_MAX_FD x LX_PROC_MAX` = 8 x 16 integer comparisons; that is
the price of "it is impossible to fall out of sync".

**`dup` sharing (a real defect fixed along the way).** On Linux `dup` copies the **file
description**, so `F_SETFL` / `F_SETOWN` change that object and every copy sees it at once.
This kernel has no separate `struct file`: an fd slot stores a registration snapshot and
`dup` merely copies it. U-29 calls `lx_fd_sync_peer()` at the end of `lx_fd_set_fl()` and
`lx_fd_owner_cmd()`, propagating the change to every other fd **in the same process with the
same kind and the same pipe object** (a pipe is the only kind here where one object is
referenced by several fds). The fixture check "clear `O_ASYNC` on the duplicate -> the
original stops delivering" and the five two-way host assertions are what pin this down; an
implementation that only copies a snapshot fails them.

**With the switch off, `O_ASYNC` really is off.** With `ARKRTOS_USE_LINUX_SIGIO = 0`,
`F_SETOWN` / `F_GETOWN` return `-EINVAL` (bit-identical to pre-U-29), `F_SETFL` ignores
`O_ASYNC`, `F_GETFL` cannot report it, and the `owner` field of `lx_fd_t` does not even exist.

**Delivery point.** Same as every other signal: `lx_finish()` is the only delivery point
(`LX_RET_PTR_DONE` / `EXEC_DONE` / `RETRY` do not deliver). So the handler runs on the return
path of the very syscall that wrote the byte, and after `rt_sigreturn` `a0` is still that
call's return value (the byte count) - the signal swallows neither the return value nor the
byte in the pipe.

**Measured readouts (`[sio]` lines, QEMU LA64 fork tier, 24 checks)**

```
[sio] getown on console is 0 -> 0 ok
[sio] setown on console -> EINVAL -> -22 ok
[sio] pipe2 -> 0 ok
[sio] setown to self -> 0 ok
[sio] getown reads back the task id -> 11 ok
[sio] setown process group -> EINVAL -> -22 ok
[sio] setfl O_ASYNC -> 0 ok
[sio] getfl reports O_ASYNC -> 1 ok
[sio] dup read end ok -> 1 ok
[sio] dup shares the owner -> 11 ok
[sio] dup shares O_ASYNC -> 1 ok
[sio] rt_sigaction(SIGIO) rc -> 0 ok
[sio] write after O_ASYNC returns the byte -> 1 ok
[sio] SIGIO handler ran -> 1 ok
[sio] SIGIO signo is 29 -> 29 ok
[sio] read still returns the byte -> 1 ok
[sio] the byte survived -> 105 ok
[sio] clear O_ASYNC -> 0 ok
[sio] getfl no longer reports O_ASYNC -> 1 ok
[sio] write after clearing O_ASYNC returns the byte -> 1 ok
[sio] clearing O_ASYNC stops delivery -> 0 ok
[sio] re-open O_ASYNC -> 0 ok
[sio] close writer returns 0 -> 0 ok
[sio] close writer (EOF) sends SIGIO -> 1 ok
```

`105` is the byte `'i'` that was written; `11` is this task's id (it can differ per tier, so
the gate matches it by shape). Three anti-false-green checks are frozen here: (1) the handler
hit count and the signal number are asserted together - looking only at "write returned 1"
would pass a kernel that never delivers; (2) the cleared-`O_ASYNC` section asserts the hit
count **did not change**, not the write return value; (3) `read` must return the byte -
"delivered but ate the data" is just as wrong.

**The default action (no handler = terminate) is asserted only on the host.** Doing it in the
QEMU fixture would kill the task mid-run and lose every later readout; host section 7 of
`t_async_io` asserts "the return value is written first, then `g_exit_code == 128 + 29`",
matching Linux's default action.

**The 44 host checks** (`t_async_io` in `test_linux_abi_host.c`, seven sections) cover what
the QEMU fixture cannot build: console object `-EINVAL`/0, a second pipe with `O_ASYNC` armed
but no owner (the scan must skip `owner == 0`), a write-end fd (`kind != PIPE_R`) not
delivering, two-way `dup` sharing (5 checks), no delivery after clearing `O_ASYNC` with
`reg` untouched, no delivery without an owner, EOF delivering once, and the default action
terminating the task. **One host trap:** after delivery `s_sig_blocked` holds that signal's
bit, and the host never runs user instructions, so `rt_sigreturn` never puts the mask back -
therefore a single `fresh()` section may assert only one delivery; asserting a second one
requires a new `fresh()`.

**Gate.** `tools/fork_gate.py` has `SIGIO_CHECKS = 24`: the `[sio]` prefix is counted
separately (the ten `[fork]` constants are untouched), and all ten tiers carry
`-DARKRTOS_USE_LINUX_SIGIO=1` (otherwise a whole section vanishing on a tier switch would look
normal). Ten must-appear readouts are checked one by one (the two carrying a task id are
matched by shape), and the bad-log self-check adds three `sio` mutations (drop one line,
insert a FAIL, change a number).

**Coverage.** The host `linuxabisigio` profile runs the 44 assertions of `t_async_io` at
92.83% on its own; the line union across the eight profiles (`linuxabi`, `linuxabifutex`,
`linuxabifilemap`, `linuxabiselect`, `linuxabisocket`, `linuxabisched`, `linuxabisigio` and
the off profile) puts `arkrtos_linux_abi` at 97.27% (baseline 97.00% unchanged), and the full
`COVERAGE-GATE OK` (all 71 modules at or above baseline).

**Boundaries.** (1) Only "fd became readable (including EOF)"; the writable direction,
edge-triggered de-duplication, `si_fd`/`si_band` and real-time queueing are all out (above);
(2) a process-group registration returns `-EINVAL` and is counted, never pretended;
(3) `dup` sharing holds **within one process only**: after fork the parent and child each hold
their own copies, and `F_SETFL`/`F_SETOWN` do not cross processes (doing that right requires a
real file object with cross-process refcounts - see `docs/已知限制.md`); (4) the owner is a
single task id; after that task exits `F_GETOWN` still reads the number back (no notification
is generated and nothing is cleaned up - the registration is a value).

## 9 Switch and zero footprint

`ARKRTOS_USE_LINUX_SIGIO` (section 8.18), default **0**, `depends on ARKRTOS_USE_LINUX_ABI`
with an `#error` guard; with it off `F_SETOWN` / `F_GETOWN` fall back to `-EINVAL`
(bit-identical to pre-U-29), `F_SETFL` ignores `O_ASYNC`, and the `owner` slot plus
`lx_async_notify_pipe` are not compiled; with it on the evidence is the 24 `[sio]` readouts
shared by all ten tiers plus the 44 assertions of the host `linuxabisigio` profile.

`ARKRTOS_USE_LINUX_THREAD` (section 8.10) is a third Linux ABI switch, default **0**,
`depends on ARKRTOS_USE_LINUX_FORK` (with an `#error` guard); with it off the two loader/port
functions and the four `lx_thread_*` symbols are not emitted.

`ARKRTOS_USE_LINUX_ABI` (features header + Kconfig), default **0**, and
`depends on ARKRTOS_USE_ELF`. `ARKRTOS_USE_LINUX_FORK` (section 8) is a second,
narrower switch, default **0**, `depends on ARKRTOS_USE_LINUX_ABI &&
ARKRTOS_USE_EXIT_CODE` and on both paging tiers; with it off none of the four
fork symbols exist. With the switch off the whole
`arkrtos_linux_abi.c` file compiles to nothing, the port has no
`arkrtos_port_linux_syscall_num`, and `arkrtos_autoconf.h` records the 0. The
gate measures this in both directions (the off-profile `.o` must have an empty
symbol table).

`ARKRTOS_USE_PAGING_COW` (section 8.2) is the third switch, default **0**, with
`#error` guards on both of its prerequisites (fork and the demand tier). With it
off the refcount table and every `la_pg_cow_*` helper are not compiled and
`LA_PTE_APP_RO` is not emitted, so the default footprint and behaviour are
unchanged - measured by the multi-round gate, not merely declared.

`ARKRTOS_USE_PAGING_SHM` (section 8.3) is the fourth switch, default **0**,
`depends on ARKRTOS_USE_PAGING_COW` with the same `#error` guard (the shared bit
table is the COW tier's refcount bookkeeping; a non-COW clone copies every pair and
the sharing evaporates). With it off the shared-bit table and
`arkrtos_port_paging_map_shared` are not emitted and `mmap(MAP_SHARED)` returns
`-ENOSYS` - measured by three tiers (zero symbols when off, 34 readouts when on,
and the `-ENOSYS` control with SHM off).

`ARKRTOS_USE_LINUX_FUTEX` (section 8.4) is the fifth switch, default **0**,
`depends on ARKRTOS_USE_LINUX_ABI` with the same `#error` guard. With it off the
wait slots, the key resolver and `lx_futex` are not compiled and number 98 falls
back to `-ENOSYS`; with it on the measured evidence is the 147 readouts of the
fork+COW+SHM+futex tier (73 of them futex: 9 basic pairing, 7 multi-slice, 20
for bitsets/timeout/requeue and 37 for PI/robust) plus the 434 host checks of the futex profile.

`ARKRTOS_USE_PAGING_FILEMAP` (section 8.5) is the sixth switch, default **0**,
`depends on ARKRTOS_USE_LINUX_ABI` + `ARKRTOS_USE_PAGING` +
`ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND` (content reaches the
mapping through demand-paged frames, so without DEMAND it cannot be filled), with
four `#error` guards. With it off the file-mapping branch, the write-back
bookkeeping and `msync` are not compiled and `mmap(fd)` returns `-ENOSYS`; with it
on the evidence is the 167 readouts of the fifth tier (20 of them filemap) plus the
358 host checks of the filemap profile.
`ARKRTOS_USE_PAGING_MPROTECT` (section 8.6), default **0**, `depends on
ARKRTOS_USE_LINUX_ABI` + `PAGING` + `PAGING_PAGES` + `PAGING_DEMAND` (a pair's
entries can only be rewritten once demand paging can hand out the frame), with
four `#error` guards in features.h; with it off the `s_pg_page_ro` ledger,
`la_pg_page_ro` / `la_pg_mprotect_hit`, the port's `paging_protect` and
`mprotect_stat` produce nothing and `mprotect` falls back to "validate, return 0,
count separately"; with it on the evidence is the **178 readouts of the sixth
tier** (11 of them mprotect) plus the host-side compile gate.

`ARKRTOS_USE_LINUX_SELECT` (section 8.8), default **0**, `depends on
ARKRTOS_USE_LINUX_ABI` with the same `#error` guard; with it off the readiness
criteria, the `fd_set` scan and `s_sel_armed/s_sel_deadline` are not compiled and
number 72 falls back to `-ENOSYS`; with it on the evidence is the **194 readouts
of the seventh tier** (15 of them select) plus the 327 host checks of the select
profile.

`ARKRTOS_USE_LINUX_SOCKET` (section 8.9) is a further switch, default **0**,
`depends on ARKRTOS_USE_LINUX_ABI` with an `#error` guard; with it off all `lx_sock_*`
code and the socket dispatch in the fd table are not compiled and `198`-`210`/`242` fall
back to `-ENOSYS`; with it on the measured readout is the host socket tier's **768 checks**
(98.51% on that tier, 98.84% union) plus the **232 readouts of the QEMU eighth tier**
(39 of them the socket section: 38 semantic + 1 round-trip timing).

`MAP_FIXED` (section 8.7) is **not a switch of its own**: it exists when
`ARKRTOS_USE_PAGING_DEMAND=1` (per-pair entry installation is what makes
"discard this range" meaningful). With `DEMAND=0`, `mmap(MAP_FIXED)` honestly
returns `-ENOSYS` and the six fixture readouts never appear, which is why
`example/qemu_loongarch64/Makefile` must mirror DEMAND into `LXFORK_DEFS`.

## 10 Gate

```
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py [--no-build]
```

14 items; prints `LINUX-ABI-GATE OK` when all pass and is invoked from
`tools/ci_gate.py` as `linux-abi`: default-off, kconfig-wired, features-guard,
board-switch, off-zero-symbols, port-wiring (two directions), compile-on (9 TUs
under the real LA64 board), syscall-table (30 numbers vs asm-generic),
abi-split-la64, abi-split-rv32, build, qemu-readout (14 Linux checks + PASS + exit 7,
15 pipe checks + exit 55, 31 signal checks + PASS + exit 138, 53 dup checks
including 17 fcntl checks + exit 56, 16 rseq checks + PASS + exit 57, the unrelated
demos still intact), and parser-selfcheck (sixteen deliberately broken logs must
each be rejected).

The socket layer has readouts on **both tiers**: on the host the sixth Linux ABI coverage
tier `linuxabisocket` in `tools/coverage_gate.py` (with
`-DARKRTOS_USE_LINUX_SOCKET=1 -DARKRTOS_USE_LWIP=1` and
`-Wl,--wrap=arkrtos_port_svc_exec_frame`) runs `test_linux_abi_host.c` with 768 checks green,
98.51% on the tier and 98.84% union (baseline 98.82%), and `tools/ci_gate.py`'s
`linux-abi-host` item already includes it; on the board the **eighth tier** of
`tools/fork_gate.py` (`-DARKRTOS_USE_LINUX_SELECT=1 -DARKRTOS_USE_LINUX_SOCKET=1
-DARKRTOS_USE_LWIP=1`) gives 232 checks all `ok` + `exit code 0` on QEMU LA64.

The fork tier has its own gate, `tools/fork_gate.py` (invoked as `fork` from
`ci_gate`, **71 items**; it runs the fork, fork+COW, fork+COW+SHM,
fork+COW+SHM+futex, fork+COW+SHM+futex+filemap,
fork+COW+SHM+futex+filemap+mprotect,
fork+COW+SHM+futex+filemap+mprotect+select,
fork+COW+SHM+futex+filemap+mprotect+select+socket and
fork+COW+SHM+futex+filemap+mprotect+select+socket+thread and
fork+COW+SHM+futex+filemap+mprotect+select+socket+thread+appq tiers one after the
other, so build / qemu-readout / parser-selfcheck / compile-on each appear ten
times; plus the five tier-independent items (`default-off`, `off-zero-symbols`,
`off-zero-port`, `board-switch`, `restore-default`) and the 27 per-tier switch
items (`kconfig-wired-<tier>` / `features-guard-<tier>` / `default-off-<tier>`
for nine tiers), 71 in total):
default-off (fork and SHM), kconfig-wired (all four `depends on`, plus the SHM
item depending on COW), the four
`#error` guards in features.h, off-zero-symbols (RV32 `linux_abi.o` has no
`lx_clone`/`lx_wait4`/`s_child`; LA64 `elf`/`port`/`paging` have no fork symbols),
compile-on (four TUs plus the fixture), build, qemu-readout (47 `[fork]` checks, 20 of them the blocking-pipe group of section 8.15
all `ok`, plus `MAP_FIXED`, different parent/child pids, PASS line, `exit code 0`, hello/FS and the
other Linux fixtures still intact; six `MAP_FIXED` readouts (section 8.7: content
zeroed after the discard, the same address returned, misaligned and shared forms
refused); the three new readouts - blocking `wait4`
really waited >= 40 ms, the parent read end still read 3 bytes after the child
closed its own copy, the SIGCHLD handler really ran - are checked one by one),
parser-selfcheck (ten broken logs rejected; the COW tier adds four more: a lost
bit in the child mask, a lost parent write, all copy lines removed, and bytes
moved at fork time; the SHM tier adds five: the `pg shm map` line removed, the
child's shared write not seen by the parent, the child not seeing the parent's
write, a non-zero start value, and the successful-mapping readout removed; the
futex tier adds eleven (eight for the second batch plus three for PI/robust): the WAIT was never woken, the waker only saw half
(`bits=1`), a waiterless `WAKE` returned 1, a multi-slice WAIT came back as
`-EFAULT`, a multi-slice WAIT did not cross a slice, the bitset was ignored
(`bits=7` became 3), a requeue left the waiter on the old word, and the timeout
never expired; the filemap tier adds three:
the write-back never happened, the write-back truncated the file, and a private
write reached the file) and
restore-default (the build directory is returned to the default tier so a later
`make` does not reuse fork-tier objects). The COW round additionally requires 32
checks, `shared>=1 / stack>=1 / copied=0`, at least one `pg cow copy ... ref=2`,
and no `ref=1` copy line. The SHM round requires 60 checks, the six SHM readouts
one by one (two refused forms returning `-38`, mapping success, start value 0,
parent sees the child's write, child sees the parent's post-fork write) and at
least one `[LA64U] pg shm map va=0x<n> pairs>=1`. The SHM switch has its own
`default-off-shm` / `kconfig-wired-shm` / `features-guard-shm` items, and
`off-zero-port` also requires that a SHM-off build has no
`arkrtos_port_paging_map_shared`. The futex round requires 147 checks and validates
the 73 futex readouts one by one (the nine basic pairing readouts, the seven
multi-slice ones - a WAIT spanning >= 150 ms still returns 0, the parent sees the
waker's `2`, the multi-slice waker is reaped, `bits == 3` - and the twenty
second-batch ones: empty bitsets `-22`, both timeout forms `-110`, the 50 ms wall
clock, `CMP_REQUEUE` mismatch `-11`, missing `uaddr2` `-22`, `bits == 7`, the old
address returning 0 after a requeue, and the thirty-seven PI/robust ones - owner
handoff by base priority rather than wake-then-race, `OWNER_DIED` on owner death,
robust-list cleanup on task exit, the four-position bit evidence, and the 5 ms
fixture yielding), with the SHM and COW assertions still in
place; the futex
switch has its own `default-off-futex` / `kconfig-wired-futex` /
`features-guard-futex` items. The filemap round requires 167 checks and validates
the 20 filemap readouts one by one; its switch has its own
`default-off-filemap` / `kconfig-wired-filemap` / `features-guard-filemap` items
(autoconf 0, four `depends on` entries, four `#error` guards). The mprotect
round requires 178 checks and validates the 11 mprotect readouts one by one
(mapping success, read-only and read-write both accepted, a read-only page still
readable, the sibling 4 KB of the same pair still writable, a write going through
after revocation, the violating child reaped rather than restarted, the status
word equal to SIGSEGV, the parent page untouched), plus one serial
`[LA64U] mprotect violation badv=0x... era=0x... tid=... -> kill task` line as
cross evidence; its parser-selfcheck adds seven more bad logs (a read-only page
that cannot be read back, granularity degraded to 8 KB, the violating store not
blocked, the violating store reaching the parent page, the status word used as an
exit code, the violation log line removed, the success readout removed); its
switch has its own `default-off-mprotect` / `kconfig-wired-mprotect` /
`features-guard-mprotect` items (autoconf 0, four `depends on` entries, four
`#error` guards). The select round requires 194 checks and validates the 15 select
readouts one by one (an empty pipe read end not ready, the user set cleared when
nothing is ready, a 50 ms timed wait returning 0 with a wall clock >= 40 ms, the
read end ready after data with the ready bit kept, a write end with space and
stdout writable, the write end not ready once the pipe is full, EOF readiness once
every writer closed, `exceptfds` never reported and cleared, a bad fd returning
`-9`), with every mprotect / filemap / SHM / COW / futex assertion still in place;
its parser-selfcheck adds eight more bad logs (an empty pipe reported ready, EOF
not reported, a full pipe still reported writable, the `fd_set` not cleared, the
timeout restarted on every replay, `exceptfds` reported, a bad fd let through, and
the select readout removed); its switch has its own `default-off-select` /
`kconfig-wired-select` / `features-guard-select` items. The socket round requires
232 checks and validates the 38 socket readouts one by one (bind / listen /
accept / connect / send / recv over a real lwIP `127.0.0.1` loopback, name
queries, four `SOL_SOCKET` options, non-blocking `-EAGAIN`, plus one
round-trip-time regression gate), with every select / mprotect / filemap / SHM /
COW / futex assertion still in place; its parser-selfcheck adds six more bad logs
and its switch has its own `default-off-socket` / `kconfig-wired-socket` /
`features-guard-socket` items (autoconf 0, two `depends on` entries, two
`#error` guards). The thread round requires 265 checks and validates the 33
thread readouts one by one (`clone(CLONE_THREAD)` positive and negative, both
SETTIDs, `getpid` equal to the tgid and cross-checked against the parent pid the
fixture prints, `gettid` different from the parent, `CLONE_SETTLS` landing in
`$tp`, two stacks at different addresses, a parent local surviving, `CHILD_CLEARTID`
cleared to 0 with the `FUTEX_WAIT` woken, the four argument gates, a `fork` still
working after threads, and `exit_group` from a thread versus from the leader),
with every socket-and-below assertion still in place; its parser-selfcheck adds
seven more bad logs (CLEARTID not cleared, the join WAIT never woken, `SETTLS` not
landing in `$tp`, a thread `getpid` not equal to the tgid, both stacks at one
address, the thread-initiated `exit_group` code replaced by the leader's, and the
`clone(CLONE_THREAD)` readout removed), and its switch has its own
`default-off-thread` / `kconfig-wired-thread` / `features-guard-thread` items
(autoconf 0, `depends on ARKRTOS_USE_LINUX_FORK`, the corresponding `#error`).
The appq round requires 277 checks and validates the 12 quota readouts one by one
(fork and thread under the cap, over-limit fork and thread returning `-11`, a fork
refused while a space is free, the third thread, both threads joined through
`CHILD_CLEARTID` + `FUTEX_WAIT`, an immediate fork after the quota is refilled, the
blocked child really reaped by `wait4`, exit code 3 passed through), plus three
`[appq]` cross-evidence assertions (exactly three lines, one owner, cap reading 3);
its parser-selfcheck adds five more bad logs (an over-limit thread let through, a
fork let through while a space is free, a task that cannot start again after the
quota was refilled, a child never reaped, and the `[appq]` line removed) and its
switch has its own `default-off-appq` / `kconfig-wired-appq` / `features-guard-appq`
items;
`off-zero-symbols` / `off-zero-port` also require that a thread-off build produces
none of `arkrtos_elf_thread`, `arkrtos_port_paging_thread_frame` and the four
`lx_thread_*` symbols.

## 11 Real bugs and readout traps found on the way

1. **Every user pointer returned `-EFAULT`**: `arkrtos_elf.c` wrote the task's
   rom/ram window fields inside `#if (ARKRTOS_USE_MPU == 1)`, but QEMU LA64 is
   MPU=0 / PAGING=1, so all four fields stayed 0 and `copy_from_user` rejected
   every buffer. The fix is to write the window fields unconditionally and keep
   only `arkrtos_mpu_build_task()` inside the MPU profile.
2. **`openat` looked up a namespace path in a volume API**: `/mnt/ram/...` is a
   namespace path, `arkrtos_fs_stat_path()` wants a volume-relative one, hence a
   permanent `-ENOENT`. Switched to `arkrtos_vfs_app_stat()` /
   `arkrtos_vfs_read_at()`.
3. **The signal frame was placed from the user sp and overwrote the kernel's own
   return address** (`INE` / `ecode=0x0d`, not a single check printed). Root
   cause and fix: section 7. It only shows up on a port whose SVC chain runs
   below the user stack, so neither the compiler nor a static check can see it -
   only a board readout can.
4. **Readout trap**: the `[linux] exec rc=0` line sometimes appeared as `[l`,
   with the rest showing up only after the program finished. That was not lost
   bytes but **a line split by preemption** once the user task started running;
   the demo now prints whole lines under the scheduler lock. Two reader-side
   traps were fixed too: killing on the marker prefix truncates `exit code 7`,
   and `-serial file:` drops the last sub-4 KB block on kill. The SHM tier hit
   the same class twice more - a user-mode spin waiting 4M times and a child that
   started its 60 ms sleep before the parent took its baseline - both read as
   "the implementation is broken" while the implementation was fine; both are
   now ordered with a release pipe plus polling (section 8.3). The gate also dumps the whole serial
   capture to `%TEMP%\ark_fork_<tier>.log` on failure and prints the line count
   and path, because "a certain line is missing" used to be indistinguishable
   from "the serial only got half of it".

5. **`la_pg_install_pair` lived inside the COW block, so the DEMAND-only tier
   had never been linked.** Adding `MAP_FIXED` made
   `arkrtos_port_paging_discard()` call `la_pg_install_pair()` unconditionally,
   but the function was defined inside `#if (ARKRTOS_USE_PAGING_COW == 1)`, so the
   first `[fork]` tier (`LINUX_FORK=1 + DEMAND=1`, COW off) failed to link with
   `undefined reference to 'la_pg_install_pair'`. **This was not new code's
   fault**: the combination "DEMAND on, COW off" had never been linked by any
   gate (`compile-on` only runs `-c`, no linking). The fix moves the function
   outside the COW block. Lesson: **`-c` passing does not mean the combination
   exists**; linkability of switch combinations can only be proven by a real link
   (or a real run).
6. **The fixture is built without `EXTRA_CFLAGS`, so a missing switch mirror
   shows up as "readouts never appear".** `$(LXFORK_ELF)` uses
   `$(ARCH_FLAGS) $(LXFORK_DEFS)` only. The `MAP_FIXED` block only compiles with
   `ARKRTOS_USE_PAGING_DEMAND=1` and DEMAND was never mirrored, so all six tiers
   reported "26 != 32 checks" (and 34/40, 43/49, 63/69, 74/80) while the code was
   correct (`compile-on` passed in all six tiers because the gate itself builds
   the fixture with FORK_FLAGS). The fix is the `ARK_DEMAND_ON` mirror. Lesson:
   **separate "not measured" from "not compiled in"** - the two look alike in a
   log, but one needs a fixture change and the other a Makefile change.
7. **The coverage gate went red over a provably unreachable guard.** The
   `MAP_FIXED` branch originally re-checked `end == 0 || end > limit` after
   computing `end = align_up(want + len)`. Since `want` and `limit` are both page
   aligned and the previous line already guarantees `len <= limit - want`, that
   branch is unreachable, so neither arm gets covered and the four-tier union
   dropped from 97.96% to 97.91%. This is not a case for inventing a test that
   reaches it - a fake entrance is worse than no coverage. The right fix is to
   delete the redundant check and pin the invariant (both endpoints page aligned)
   in a comment; the union went back to 98.18% and `COVERAGE-GATE OK` returned.
8. **The futex replay read the word at the clobbered `uaddr` (0), so a normal long
   wait died with `-EFAULT`.** On the LA64 ABI the syscall result is written to
   `a0`, which is also where `futex(WAIT)` reads `uaddr`; when the blocking path
   returns 0 the replayed call reads `uaddr == 0`. The first version recognised
   the replay but still read the word at that 0, so `copy_from_user` always
   failed with `-EFAULT` and the slot was dropped - a WAIT not woken within
   100 ms, the most normal futex use there is, terminated with an error code.
   Worse, the host case asserted `-EFAULT` as the expected value, so the test was
   vouching for the bug. The fix adds `s_futex_uaddr` / `s_futex_val` per slot:
   store the real address and expected value when registering, take them back on
   replay. The host case now asserts "keeps sleeping / value changed counts as
   woken / a genuinely unreadable registered address is `-EFAULT`", and the board
   adds a multi-slice group (the child busy-waits 250 ms across two slices before
   writing and calling `WAKE`, with an elapsed-time check and `bits == 3`).
   Lessons: **when the return register is the first argument register, replay
   must be identified by external bookkeeping, not by the arguments**; and
   **never write a "plausible errno the old code returned" down as the expected
   value** - the shape works before and after and returns a plausible `errno`
   either way.

## 12 Boundaries

This is the minimal syscall set that lets a static program run, not a Linux
compatibility layer: no ptrace, no Linux `/proc`. Pipes are **blocking by default**
(section 6 / 8.15, with an `O_NONBLOCK` mode), and cross-task blocking reads and
writes are measured in the fork tier. The real-time behaviour is unchanged — every
Linux-ABI path completes synchronously inside the SVC with no extra thread and no
lock. Three paths "sleep and come back": blocking `wait4` (section 8.1), blocking
pipe `read`/`write` (U-26, sections 6 / 8.15) and a futex wait (section 8.4); all
are built as restartable syscalls: the task
is removed from the ready set, `ERA` is backed up onto the `syscall`, and the
call is replayed after the wakeup - it is not an interrupts-off spin. Dynamic linking has its
own switch (`ARKRTOS_USE_DYNLOAD`) and a minimal in-tree loader, but running glibc
binaries is still out of reach. Fork exists (section 8): per-process fd tables
with inheritance, cross-task `SIGCHLD` and a blocking `wait4` all have QEMU LA64
readouts, copy-on-write now exists too (`ARKRTOS_USE_PAGING_COW`, section
8.2, default off), as does an eleven-op futex tier
(WAIT/WAKE/REQUEUE/CMP_REQUEUE/BITSET plus the PI trio, robust-list cleanup and the
condition-variable PI variants, plus timeout, `ARKRTOS_USE_LINUX_FUTEX`, sections
8.4 / 8.4b / 8.4c, default off), file mapping (`ARKRTOS_USE_PAGING_FILEMAP`, section
8.5, default off), real page-level write protection
(`ARKRTOS_USE_PAGING_MPROTECT`, section 8.6, default off) and real `mmap`
semantics `MAP_FIXED` (demand tier, section 8.7, no extra switch), a thread tier
(`CLONE_THREAD`, section 8.10, default off - with it off `CLONE_VM`/`CLONE_THREAD` are
still refused) and a per-App task quota (section 8.11, default off); preemptive signal
delivery is still absent. `fcntl` fd flags and status flags exist (section 8.13,
2026-11-27), record locks exist (section 8.16, 2026-11-27; an 8-slot table, paths as
identity, no deadlock detection) while async I/O does not. `rseq`/`getcpu`
landed the same day (section 8.14, 2026-11-27), and so did scheduling policies
(U-28, section 8.17: the twelve `sched_*` numbers plus the kernel deadline class).

`execve` gives "replace this task's image" and `fork` gives "a second task with
its own address-space copy". Both exist, but do not read this as a full process
model: scheduling and priorities are still kernel-global, the fd table is
per-process only in the fork tier (kernel-global by default), copy-on-write
exists only in the `ARKRTOS_USE_PAGING_COW` tier (with private stack pages), and
threads exist only in the `ARKRTOS_USE_LINUX_THREAD` tier (section 8.10, default off),
and there is no job control.