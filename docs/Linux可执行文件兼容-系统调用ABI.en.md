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

## 1 Supported syscalls (27; 25 measured on hardware, 2 table-and-argument only)

Numbers follow `asm-generic` (shared by LoongArch / RISC-V / ARM64):

| nr | name | how Ark fulfils it | measured |
|---:|---|---|---|
| 56 | `openat` | `arkrtos_vfs_app_stat()` on the VFS namespace; directories return `-EISDIR` | `openat>0 -> 1 ok` |
| 57 | `close` | release the fd slot, return 0 (a pipe end drops its reference count) | `close -> 0 ok` |
| 59 | `pipe2` | ring-buffer pipe with per-end reference counts; **non-blocking** (section 6) | `pipe2 rc = 0` / `read empty -> -EAGAIN = -11` / `write with reader closed -> -EPIPE = -32` / `exit code 55` |
| 63 | `read` | `arkrtos_vfs_read_at()` at the fd offset | `read 4 bytes -> 4 ok` |
| 64 | `write` | 128-byte chunks through `arkrtos_board_putc` | the program's own text |
| 67 | `pread64` | read at `pos` **without moving the fd cursor** (what `ld.so` needs to walk a segment table) | no fixture readout (1.1) |
| 93 | `exit` | `arkrtos_thread_exit_code_internal()` | `exit code 7` |
| 94 | `exit_group` | same (single-threaded) | same |
| 96 | `set_tid_address` | remember the address, return the task id (no futex) | same path as getpid |
| 113 | `clock_gettime` | `MONOTONIC` from kernel ticks; `REALTIME` returns `-ENOSYS` | `MONOTONIC -> 0 ok` / `REALTIME -> -38 ok` |
| 129 | `kill` | only towards the calling task (`pid != self` gives `-ESRCH`); sets pending (section 7) | `kill rc survives handler -> 0 ok` / `kill other pid -> -ESRCH = -3 ok` |
| 130 | `tkill` | same entity as `kill` while single-threaded | same path as `kill` |
| 131 | `tgkill` | requires `tgid == tid`, else `-ESRCH` | `tgkill tgid!=tid -> -ESRCH = -3 ok` |
| 134 | `rt_sigaction` | install/query; `sigsetsize` must be 8; `SIGKILL`/`SIGSTOP` and a missing non-zero restorer give `-EINVAL` (section 7) | `rt_sigaction rc = 0 ok` / `old handler was SIG_DFL = 0 ok` / `no restorer -> -EINVAL = -22 ok` |
| 135 | `rt_sigprocmask` | `BLOCK`/`UNBLOCK`/`SETMASK`; `KILL`/`STOP` can never be masked (section 7) | `query sees blocked bit = 1 ok` / `delivered on unblock = 2 ok` |
| 139 | `rt_sigreturn` | validates frame `magic` and length, then restores the whole frame; privilege level is not taken from the user frame (section 7) | triggered by the restorer; readouts stay correct after the handler returns |
| 160 | `uname` | `sysname=ArkRTOS`, `machine=loongarch64` | `uname -> 0 ok` |
| 172 | `getpid` | internal task id | `getpid==task id>0 -> 1 ok` |
| 174 | `getuid` | always 0 | same path |
| 175 | `geteuid` | always 0 | same path |
| 214 | `brk` | monotonic allocation inside the ELF window | `brk(0)>0` / `brk(+4096) -> 34607104 ok` |
| 215 | `munmap` | returns 0 only, **does not give the address back** | same path |
| 220 | `clone` | fork only: `flags` must be 0 or `SIGCHLD`, `child_stack` must be 0; copies the whole address space and makes a new task, the child returns 0 and the parent the child id (section 8, `ARKRTOS_USE_LINUX_FORK`) | `clone(child_stack!=0) == -EINVAL -> -22 ok` / `parent child=12` / `child pid != parent pid -> 1 ok` / `exit code 0` |
| 221 | `execve` | replace the image of the **calling task**: load segments into its own window, write entry/`sp`/`tp` back into the trap frame; does not return on success | `stage2 pid==stage1 pid -> 11 ok` / `image reloaded (.data marker) -> 1 ok` / `fd 3 survived -> 1 ok` / `heap cursor reset -> 34668544 ok` / `exit code 42` (section 5) |
| 222 | `mmap` | anonymous only: `MAP_PRIVATE|MAP_ANONYMOUS` or `MAP_SHARED|MAP_ANONYMOUS` (the latter needs `ARKRTOS_USE_PAGING_SHM`, section 8.3); `MAP_FIXED` and `PRIVATE|SHARED` together are refused | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |
| 260 | `wait4` | only `options 0/WNOHANG`; **without `WNOHANG` it really blocks** (restartable syscall, section 8.1), never invents a status, the bookkeeping entry is consumed on reap (section 8) | `wait4 returned child pid -> 12 ok` / `WEXITSTATUS == 42 -> 42 ok` / `wait4(child) again == -ECHILD -> -10 ok` / `blocking wait4 blocked >= 40 ms -> 1 ok` |
| 226 | `mprotect` | checks page alignment and window range only, returns 0 and counts separately; **protection is not enforced** (the window is one PS=20 RWX mapping) | no fixture readout (1.1) |

Unknown numbers return `-ENOSYS` (38) and are counted. Nothing is guessed.

### 1.1 The two that are table-and-argument only

`pread64`(67) and `mprotect`(226) exist for `ld.so`, and this round has **no
runtime fixture** for either: the first only has "the number matches
`asm-generic` plus it compiles under the LA64 board profile", the second only
has argument validation and an explicit degradation note - it returns 0 but
**changes no page permission**, and the separate counter exists so that
"accepted" and "enforced" can be told apart in the readout instead of passing
off one as the other.

The other 25 numbers all have serial readouts from the `[linux]` / `[pipe]` /
`[sig]` fixtures.

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

`dup`/`dup2` redirection, `socket`/`select`, `futex`, file-backed `mmap`,
`MAP_FIXED` and page-level permission bits, and threads. Anonymous `MAP_SHARED`
exists in the SHM tier (section 8.3, default off) and is inherited through `fork`
only. `fork`/`clone` is no longer in this
list: with `ARKRTOS_USE_LINUX_FORK` on (default off, 2026-11-23, QEMU LA64) a
process can split once and be reaped via `wait4` - fork only, no threads
(`CLONE_VM`/`CLONE_THREAD` are `-EINVAL`), see section 8. Signal delivery now
crosses tasks (`kill`/`tkill`/`tgkill` resolve the target by pid, 2026-11-23,
QEMU LA64), but there is still no preemptive delivery, no real-time signal and no
`siginfo` (section 7). fd inheritance exists in the fork tier: each process owns
its fd table and a pipe end holds its own reference count (section 4/8). `futex`
still needs a shared-memory wait queue that Ark RTOS does not have; faking it
would produce silently wrong answers. See the Chinese page for the reasoning per
row.

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

Three differences that stay in the title:

1. `envp` is not consumed (this machine has no environment layer; the new image
   receives the same empty table);
2. the **fd table is preserved** (same as Linux, unlike the shell's `exec`,
   which starts a new task and resets it);
3. the old image's heap cursor is void; already-allocated addresses are not
   reclaimed in any way (the old image is gone).

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

## 6 pipe2 (59): non-blocking pipes (2026-11-21, QEMU LA64)

Programs see Linux's `int pipe2(int fds[2], int flags)`: two fds (read end first)
written back into the user `int[2]`. Ark keeps one reference count per end plus a
ring buffer: a static table `s_pipe[2]` (`LX_PIPE_MAX 2`), 256 bytes each
(`LX_PIPE_BUF`), with `readers` / `writers` counters. `lx_fd_t` gained a `kind`
field (file / read end / write end) and a `pipe` index, so `read` / `write` /
`close` dispatch correctly; `execve` keeps the fd table, so **pipes survive
`execve`** as on Linux. All transfers are synchronous inside the SVC - the data
never touches ramfs and no thread is created.

Semantics differ from Linux on one axis, deliberately (the Chinese page has the
full table): reading an empty buffer with a live writer returns `-EAGAIN` (11)
instead of blocking; reading an empty buffer with all writers closed returns 0
(EOF, same as Linux); writing into a full buffer returns a **short write**;
writing with every reader closed returns `-EPIPE` (32) because there is no
`SIGPIPE`; `O_CLOEXEC` is refused with `-EINVAL` because this kernel has no
close-on-exec semantics to promise; `O_NONBLOCK` is accepted. Exhausting the 8
fd slots or the 2 pipes returns `-ENFILE`; a failed `copy_to_user` of `fds[2]`
rolls the whole pipe back (`-EFAULT`).

Why no blocking: pipe I/O is still a synchronous in-kernel copy and deliberately
never sleeps. In the fork tier the two ends can really belong to parent and
child (fds are inherited), but "sleep until readable" was not wired up.

Measured readout (14 `check()` calls in
`example/qemu_loongarch64/elf/linux_pipe.c`, `exit code 55`):

```
[pipe] pipe2 rc = 0 ok
[pipe] fds = 3 4
[pipe] write 5 bytes = 5 ok
[pipe] read 5 bytes = 5 ok
[pipe] read empty -> -EAGAIN = -11 ok
[pipe] read after writer closed -> EOF = 0 ok
[pipe] pipe2(O_CLOEXEC) -> -EINVAL = -22 ok
[pipe] write 400 into 256 buffer -> short write = 256 ok
[pipe] write with reader closed -> -EPIPE = -32 ok
[pipe] finished after 0 ms, exit code 55
```

Anti-false-green (measured): making an empty read return 0 and dropping the
reader check on the write side flips the readout to
`read empty -> -EAGAIN = 0 FAIL` plus `write with reader closed -> -EPIPE =
-11 FAIL` and `exit code 1`. The gate's `parser-selfcheck` feeds two more
pipe-specific broken logs back into the judge; both must be rejected.

Boundaries: non-blocking semantics (a spinning reader/writer gets `-EAGAIN`),
256-byte buffer, at most 2 pipes, 8 fd slots, no `SIGPIPE`. fd inheritance does
exist in the fork tier: the parent creates a pipe, `clone` hands both ends to the
child, and closing the parent's write end does not give the reader EOF while the
child still holds it.

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
`read`, `futex`) is wired to this path.

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

Readouts (`tools/fork_gate.py`, fork+COW tier): 26 checks all `ok`, 5 of them in
the COW section - `cow child saw pre-fork values (bits=3) -> 3 ok` (the child
still sees the pre-fork values) and `parent cow write kept -> 48879 ok` (the
parent's write stays in its own copy). Four kernel-log assertions: at least one
`[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>` with `shared>=1`,
`stack>=1`, `copied==0`; at least one `[LA64U] pg cow copy idx=<n> ref=2`; no
copy line with `ref=1` (copying an unshared pair is moving bytes for nothing);
and the unrelated demos still intact. The gate runs three rounds (fork tier 21
checks, fork+COW tier 26, fork+COW+SHM tier 34) and **never removes the default
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
| Inheritance | inherited through `fork` only. No `shm_open`, no file-backed `mmap`, no `MAP_FIXED`; with the switch off `MAP_SHARED` is always `-ENOSYS` |
| Refused forms | missing `MAP_ANONYMOUS`, `PRIVATE\|SHARED` together, and `MAP_FIXED` are all `-ENOSYS` (this kernel's rule: unsupported means `ENOSYS`, not `EINVAL`) |

Readouts (`tools/fork_gate.py`, fork+COW+SHM tier, 34 checks): the SHM section has
8 - two refused forms returning `-38`, `mmap(MAP_SHARED|ANON) > 0`,
`shared page starts zeroed -> 0`, `clone for shm child > 0`,
`parent saw child shared write -> 1`, `shm child exit reaped -> 12` and
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

## 9 Switch and zero footprint

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

## 10 Gate

```
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py [--no-build]
```

13 items; prints `LINUX-ABI-GATE OK` when all pass and is invoked from
`tools/ci_gate.py` as `linux-abi`: default-off, kconfig-wired, features-guard,
board-switch, off-zero-symbols, port-wiring (two directions), compile-on (8 TUs
under the real LA64 board), syscall-table (27 numbers vs asm-generic),
abi-split-la64, abi-split-rv32, build, qemu-readout (14 Linux checks + PASS + exit 7,
14 pipe checks + exit 55, 31 signal checks + PASS + exit 138, the unrelated demos
still intact), and parser-selfcheck (eight deliberately broken logs must each be
rejected).

The fork tier has its own gate, `tools/fork_gate.py` (invoked as `fork` from
`ci_gate`, 22 items; it runs the fork, fork+COW and fork+COW+SHM tiers one after
the other, so build / qemu-readout / parser-selfcheck each appear three times):
default-off (fork and SHM), kconfig-wired (all four `depends on`, plus the SHM
item depending on COW), the four
`#error` guards in features.h, off-zero-symbols (RV32 `linux_abi.o` has no
`lx_clone`/`lx_wait4`/`s_child`; LA64 `elf`/`port`/`paging` have no fork symbols),
compile-on (four TUs plus the fixture), build, qemu-readout (21 `[fork]` checks
all `ok`, different parent/child pids, PASS line, `exit code 0`, hello/FS and the
other Linux fixtures still intact; the three new readouts - blocking `wait4`
really waited >= 40 ms, the parent read end still read 3 bytes after the child
closed its own copy, the SIGCHLD handler really ran - are checked one by one),
parser-selfcheck (nine broken logs rejected; the COW tier adds four more: a lost
bit in the child mask, a lost parent write, all copy lines removed, and bytes
moved at fork time; the SHM tier adds five: the `pg shm map` line removed, the
child's shared write not seen by the parent, the child not seeing the parent's
write, a non-zero start value, and the successful-mapping readout removed) and
restore-default (the build directory is returned to the default tier so a later
`make` does not reuse fork-tier objects). The COW round additionally requires 26
checks, `shared>=1 / stack>=1 / copied=0`, at least one `pg cow copy ... ref=2`,
and no `ref=1` copy line. The SHM round requires 34 checks, the six SHM readouts
one by one (two refused forms returning `-38`, mapping success, start value 0,
parent sees the child's write, child sees the parent's post-fork write) and at
least one `[LA64U] pg shm map va=0x<n> pairs>=1`. The SHM switch has its own
`default-off-shm` / `kconfig-wired-shm` / `features-guard-shm` items, and
`off-zero-port` also requires that a SHM-off build has no
`arkrtos_port_paging_map_shared`.

## 11 Three real bugs and one readout trap found on the way

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
   and `-serial file:` drops the last sub-4 KB block on kill.

## 12 Boundaries

This is the minimal syscall set that lets a static program run, not a Linux
compatibility layer: no ptrace, no Linux `/proc`. Pipes exist but are
non-blocking (section 6), which is a different contract from Linux. The
real-time behaviour is unchanged — every Linux-ABI path completes synchronously
inside the SVC with no extra thread and no lock. The single "sleep and come back"
path is blocking `wait4`, built as a restartable syscall (section 8.1): the task
is removed from the ready set, `ERA` is backed up onto the `syscall`, and the
call is replayed after the wakeup - it is not an interrupts-off spin. Dynamic linking has its
own switch (`ARKRTOS_USE_DYNLOAD`) and a minimal in-tree loader, but running glibc
binaries is still out of reach. Fork exists (section 8): per-process fd tables
with inheritance, cross-task `SIGCHLD` and a blocking `wait4` all have QEMU LA64
readouts, and copy-on-write now exists too (`ARKRTOS_USE_PAGING_COW`, section
8.2, default off); still absent are threads (`CLONE_VM`/`CLONE_THREAD` are
refused), preemptive signal delivery and `dup2`-style redirection.

`execve` gives "replace this task's image" and `fork` gives "a second task with
its own address-space copy". Both exist, but do not read this as a full process
model: scheduling and priorities are still kernel-global, the fd table is
per-process only in the fork tier (kernel-global by default), copy-on-write
exists only in the `ARKRTOS_USE_PAGING_COW` tier (with private stack pages), and
there are no threads and no job control.