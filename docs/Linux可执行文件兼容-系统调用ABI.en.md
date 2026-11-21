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

## 1 Supported syscalls (25; 23 measured on hardware, 2 table-and-argument only)

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
| 221 | `execve` | replace the image of the **calling task**: load segments into its own window, write entry/`sp`/`tp` back into the trap frame; does not return on success | `stage2 pid==stage1 pid -> 11 ok` / `image reloaded (.data marker) -> 1 ok` / `fd 3 survived -> 1 ok` / `heap cursor reset -> 34668544 ok` / `exit code 42` (section 5) |
| 222 | `mmap` | only `MAP_PRIVATE|MAP_ANONYMOUS`; `MAP_FIXED` refused | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |
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

The other 23 numbers all have serial readouts from the `[linux]` / `[pipe]` /
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

`fork`/`clone`, fd inheritance / redirection,
`socket`/`select`, `futex`, real `mmap` semantics (file-backed, shared,
`MAP_FIXED`, permission bits), and threads. Signals exist only in a minimal
form: same-task self-delivery, delivered on the way out of a syscall, no
cross-task delivery, no preemptive delivery, no real-time signals, no `siginfo`
(section 7). Each of these needs an object (a
process, a user-side signal frame, a shared-memory wait queue) that Ark RTOS
does not have; faking them would produce silently wrong answers. See the Chinese
page for the reasoning per row.

`execve` is no longer unsupported either: it replaces the image of the current
task in place (section 5). What is still missing is the *process* object - there
is no `fork`/`clone`, no address-space copy, and therefore no parent/child
relation to inherit anything through.

Dynamic linking no longer belongs in this list: with `ARKRTOS_USE_DYNLOAD` on,
the kernel loads a `PT_INTERP` interpreter and ships a minimal in-tree
`ld-ark.so` (2026-11-20, QEMU LA64). That is not glibc support - see
[动态链接与PT_INTERP](动态链接与PT_INTERP.en.md).

## 4 Deliberate degradations

- **fd table** is kernel-global (`lx_fd_t s_fd[8]`, path + offset) and is reset
  by the shell's `exec` (which starts a *new* task). `execve` keeps the fds
  opened before the call, as Linux does. There are no parents, so inheritance
  still cannot be faked.
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

Why no blocking: there is no `fork` and the fd table is kernel-global, so both
ends usually live in the **same task** - blocking would be waiting on itself.

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
256-byte buffer, at most 2 pipes, 8 fd slots, no fd inheritance (no `fork`), no
`SIGPIPE`.

## 7 Signals (`rt_sigaction` 134 / `rt_sigprocmask` 135 / `kill` 129 / `tkill` 130 / `tgkill` 131 / `rt_sigreturn` 139)

Landed 2026-11-21, measured on QEMU LA64 (`example/qemu_loongarch64/elf/linux_signal.c`,
31 `[sig]` checks plus a PASS line and `exit code 138`).

### Semantics (how far it goes, stated plainly)

| item | status |
|---|---|
| delivery target | **only "the task signals itself"**. There is no `fork`/`clone`, so there is no second entity; `pid != self` gives `-ESRCH` (`kill`/`tkill` are the same entity, `tgkill` additionally wants `tgid == tid`) |
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

- **No cross-task delivery**: a task can only signal itself.
- **No preemptive delivery**: only at the syscall return point. A program that
  never enters the kernel keeps the signal pending (`kill` returns 0, no handler).
- No real-time signals, no `sigaltstack`, no `siginfo`, no `SA_RESTART`
  semantics (an interrupted syscall is not retried).
- Stop-family defaults are treated as ignored: there is no other process to stop.
- The frame includes the FPU area (measured on the 528 B profile); with the LSX
  profile the whole 1040 B block is copied too and the **vector area is measured
  separately** (two checks above). LASX (`$xr0..$xr31`) is not saved.

## 8 Switch and zero footprint

`ARKRTOS_USE_LINUX_ABI` (features header + Kconfig), default **0**, and
`depends on ARKRTOS_USE_ELF`. With the switch off the whole
`arkrtos_linux_abi.c` file compiles to nothing, the port has no
`arkrtos_port_linux_syscall_num`, and `arkrtos_autoconf.h` records the 0. The
gate measures this in both directions (the off-profile `.o` must have an empty
symbol table).

## 9 Gate

```
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py [--no-build]
```

13 items; prints `LINUX-ABI-GATE OK` when all pass and is invoked from
`tools/ci_gate.py` as `linux-abi`: default-off, kconfig-wired, features-guard,
board-switch, off-zero-symbols, port-wiring (two directions), compile-on (8 TUs
under the real LA64 board), syscall-table (25 numbers vs asm-generic),
abi-split-la64, abi-split-rv32, build, qemu-readout (14 Linux checks + PASS + exit 7,
14 pipe checks + exit 55, 31 signal checks + PASS + exit 138, the unrelated demos
still intact), and parser-selfcheck (eight deliberately broken logs must each be
rejected).

## 10 Three real bugs and one readout trap found on the way

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

## 11 Boundaries

This is the minimal syscall set that lets a static program run, not a Linux
compatibility layer: no ptrace, no Linux `/proc`. Pipes exist but are
non-blocking (section 6), which is a different contract from Linux. The
real-time behaviour is unchanged — every Linux-ABI path completes synchronously
inside the SVC with no extra thread, no blocking point. Going further requires a
process object (for a real libc surface) before anything else; signals are
already at their minimal form (section 7). Dynamic linking itself has its own switch (`ARKRTOS_USE_DYNLOAD`) and a
minimal in-tree loader, but running glibc binaries is still out of reach.

`execve` gives "replace this task's image", not "this system has processes":
without `fork` there is no second task, and without an address-space copy there
is no parent/child isolation. Do not read it as "Ark has processes now".