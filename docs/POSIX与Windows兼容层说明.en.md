# POSIX and Windows Compatibility Layer

**[简体中文](POSIX与Windows兼容层说明.md) | English**

> Condensed English edition. The Chinese page (POSIX与Windows兼容层说明.md) is normative:
> every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese
> page wins. This page adds no new claim and no new measurement.

Location: `kernelsrc/sdk/posix/` — an App-side layer, not part of the kernel.

The goal is one sentence: **to move a C program written for Linux / Windows onto Ark RTOS, what
you change is the include list, not the program structure.**

Everything in the layer lives in the App's own mapping. The kernel only provides capabilities it
already had (devices, semaphores, message queues, mutexes, the clock, and one newly added thread
service). There is therefore **no address, no partition number and no slot number anywhere in the
compatibility layer** — the same source file compiles on any board.

## 1. What this is not

| Not | Explanation |
|---|---|
| Not part of the kernel | Everything is in `kernelsrc/sdk/posix/`, added to an App project as a source file. The kernel changed no interface for it (except the new thread service, SVC 0x1B). |
| Not a libc | No `printf` family, no `string.h`, no `stdlib.h`. The App uses the SDK's `arkrtos_*` or brings its own. |
| Not an emulator | Every call really lands on a kernel service; there are no "pretend it worked" stubs. Anything that cannot be done returns an error and sets `errno` instead of returning a plausible-looking value. |
| Not a process model | One App is one address space plus several threads. There is no `fork` / `exec` / `waitpid` and no process-level fd table: all `fd`s are numbered by the compatibility layer on the App side. Files do exist, through the VFS path namespace (§4.7). |

## 2. File inventory

```
kernelsrc/sdk/posix/
├── arkrtos_posix.h          umbrella header: whole layer + heap API + tunables in one include
├── arkrtos_posix.c          implementation (~3700 lines; adding this one .c is enough)
├── arkrtos_posix_types.h    base types (ssize_t / off_t / time_t / struct timespec)
├── errno.h                  error codes + per-thread errno
├── fcntl.h                  open flags (shared by devices and VFS paths)
├── unistd.h                 read / write / close / lseek / unlink / rmdir / rename / truncate /
│                            ftruncate / link / symlink / readlink / sleep / usleep / nanosleep / time
├── dirent.h                 opendir / readdir / closedir
├── sys/stat.h               stat / fstat
├── time.h                   clock_gettime / sched_yield
├── pthread.h                pthread threads and mutexes
├── semaphore.h              unnamed semaphores
├── mqueue.h                 message queues
├── arkrtos_win_compat.h     Windows-style names (CreateThread / Sleep / strcpy_s ...)
└── arkrtos_libc_glue.c      C-library glue (no semihosting + printf outlet + AC5/AC6 syscall hooks)
```

`arkrtos_libc_glue.c` is for the **toolchain C library**, not for the compatibility layer: it
routes `printf` output to the console device and turns semihosting off. An App that adds only
`arkrtos_posix.c` also runs, but then `printf` has no outlet — add this file if you need printing
(§10).

Three ways to include, pick as needed:

```c
/* A. Everything: a plain POSIX program, one include line changed */
#include "arkrtos_posix.h"

/* B. One thing only: single headers also work standalone */
#include "semaphore.h"      /* e.g. semaphores only */

/* C. Windows-style names too */
#include "arkrtos_win_compat.h"   /* already includes arkrtos_posix.h internally */
```

Add `arkrtos_posix.c` to the project and there is no further build step. It consumes the App's own
RAM, not the kernel's.

## 3. Tunables

All are defined before including the headers, or as project preprocessor macros.

| Macro | Default | Meaning |
|---|---|---|
| `ARKRTOS_POSIX_THREAD_MAX` | `2` | Upper bound on simultaneously existing pthreads. Each extra thread costs one static stack plus two semaphores. |
| `ARKRTOS_POSIX_STACK_WORDS` | `256` | Stack size per pthread in words (= 1 KB). |
| `ARKRTOS_POSIX_HEAP_SIZE` | `1024` | App-side heap arena in bytes. |
| `ARKRTOS_POSIX_DEFAULT_PRIO` | `10` | Default kernel priority for threads. |
| `ARKRTOS_POSIX_NO_TIMESPEC` | undefined | Define it when the platform headers already define `struct timespec`, to avoid a redefinition. |
| `ARKRTOS_POSIX_DIR_MAX` | `2` | Upper bound on simultaneously open directory streams (`DIR`). When the pool is empty `opendir` returns `NULL` + `ENOMEM` and never points at somebody else's stream. |
| `ARKRTOS_POSIX_WRAP_STDLIB` | undefined | Off by default. When defined, `malloc/free/calloc/realloc` map onto this layer's arena. See §6. |

### Default profile and the RAM window

The development slot gives an App an **8 KB** RAM window (`ARKRTOS_DEV_RAM_WINDOW`, same value on
F427 / F401, see 配置区与安装策略.md). The defaults are sized for that window:

| Item | Bytes |
|---|---|
| 2 thread stacks (2 × 1 KB) | 2048 |
| heap arena | 1024 |
| errno table (64 × int32) | 256 |
| thread slot table + completion semaphores and other static data | tens |
| **layer subtotal** | **~3.4 KB** |
| left for the App's own data + main thread stack | ~4.6 KB |

The arena and the thread stacks are **static**: they are occupied even when unused. If the App
slot RAM is larger, raise the profile per project; if the App uses no threads, lower
`ARKRTOS_POSIX_THREAD_MAX` (at least 1; if `pthread_create` is used, at least the number of threads
needed).

## 4. Capability and mapping overview

### 4.1 Devices, paths and time (`unistd.h` / `time.h` / `fcntl.h`)

`open()` splits into two kinds of target **by name**, with the caller writing the same line:

| POSIX | Lands on | Notes |
|---|---|---|
| `open(name, flags)` | name starting with `/` → `arkrtos_path_open` (VFS path); otherwise → `arkrtos_dev_open` (device) | Both paths share the same `open()` and the same `fd` numbering space; the `fd` is numbered on the App side (`3..10`, 8 slots in total) and is no longer a bare device handle |
| `read` / `write` | path fd → `arkrtos_path_read/write`; device fd → `arkrtos_dev_read/write` | failure sets `EIO`, bad fd sets `EBADF` |
| `close(fd)` | the close of the corresponding side | both the handle and the fd slot are given back |
| `lseek(fd,off,whence)` | only valid on a **path fd** (`arkrtos_path_seek`) | devices have no offset state; a device fd returns `-1` with `ESPIPE` rather than inventing a 0 |
| `sleep(s)` | `arkrtos_task_wait(s*1000)` | whole-second scheduled wait |
| `usleep(us)` | scheduled wait | the kernel waits in milliseconds and rounds **up**: `usleep` never returns early |
| `nanosleep(req,rem)` | scheduled wait | absolute-time wait; `rem` is filled only when it timed out short |
| `time(t)` / `clock_gettime()` | millisecond ticks since boot | **no RTC, no calendar**. `CLOCK_REALTIME` is aliased to the monotonic clock instead of reporting a wrong wall time |
| `sched_yield()` | a 0 ms wait | the kernel has no separate "yield but stay ready" primitive, so this is an ordinary wait |

Not provided: `fork` / `exec*` / `getpid` (stub only) / `localtime` / `strftime` — there is no
process model and no RTC.

`O_CREAT` / `O_TRUNC` / `O_APPEND` / `O_DIRECTORY` **only make sense for paths**. Asking a
**device name** for them is **rejected** rather than silently ignored: `O_CREAT`/`O_TRUNC`/
`O_APPEND` set `EINVAL`, `O_DIRECTORY` sets `ENOTDIR`, `O_RDWR` sets `EOPNOTSUPP` (devices have
only unidirectional handles). A caller that wrote `O_CREAT` must not believe it got a file.

File names and device names share one entry point, separated by the single hard rule "is there a
`/` prefix". A missing device name sets `ENODEV`; a missing path sets `ENOENT` (from the VFS error
mapping of §4.7). The two do not fall back to each other.

### 4.2 errno (`errno.h`)

`errno` is **per thread**, held in the App's own RAM: a 64-entry `int32` array indexed by the
kernel task number (from SVC 0x1B `self`).

- Reading `errno` costs **one SVC**. That is a deliberate trade — correctness first on a small
  RTOS. Cache it in a local variable inside hot loops.
- The table size 64 has the same origin as the kernel task-table limit.
- When the kernel has no exact POSIX code for a condition, the layer uses the closest one (e.g.
  timeout → `ETIMEDOUT=110`).

### 4.3 pthread (`pthread.h`)

| POSIX | Lands on |
|---|---|
| `pthread_create` | thread service SVC 0x1B (see §5) |
| `pthread_join` | one completion semaphore per thread |
| `pthread_exit` / `pthread_self` | `arkrtos_thread_exit` / `arkrtos_thread_self` |
| `pthread_mutex_*` | kernel mutex (`arkrtos_mutex_*`) |
| `pthread_cond_*` | kernel condition variable (`arkrtos_cond_*`, three-step form: see 同步原语与令牌守恒.md §5.3) |
| `pthread_rwlock_*` | **user-space composition**: kernel mutex + two condition variables (read condition, write condition), zero SVC changes. **Writer-preferring** (pure reader preference starves writers) — hence `pthread_rwlockattr_setkind_np(PTHREAD_PREFER_READER_NP)` returns `ENOTSUP` instead of being accepted and quietly running writer-preferring. Kernel objects are created lazily, rolled back with `ENOMEM` on failure; `destroy` returns `EBUSY` when there are holders or waiters |
| `pthread_barrier_*` | **user-space composition**: mutex + condition variable + a generation counter `gen`, so the barrier is reusable (the next round does not wake the previous round's waiters). `pthread_barrier_wait` lets one thread return `PTHREAD_BARRIER_SERIAL_THREAD(-1)`; `pshared != 0` returns `ENOTSUP` |

`pthread_attr_t` is Ark RTOS's own struct (`stackaddr` / `stacksize` / `priority` / `period_ms`);
**all-zero is the default**:

```c
pthread_t th;
pthread_create(&th, 0, worker, arg);   /* zero attributes, stack taken from the SDK static pool */
```

**Deliberately unsupported**:

- **No `pthread_attr_set*` family.** Fill struct fields directly; the POSIX setters are only an
  extra layer of indirection on a small 32-bit system.
- Mutexes **expose no priority-inheritance API**. The kernel mutex does perform priority
  inheritance, but a POSIX program can neither observe nor configure it.
- **Read-write locks and barriers are user-space compositions, not kernel objects**: they consume
  entries from `ARKRTOS_MTX_NUM` / `ARKRTOS_COND_NUM`, and a full table shows up as `ENOMEM` (see
  the capacity budget in §4.4). Writer preference is a deliberate choice and
  `PTHREAD_PREFER_READER_NP` is explicitly unsupported. **Verified on board** (2026-11-14, F427):
  `POSIX_DEMO` carries two self-checks — the rwlock is a single-task state machine (two
  consecutive reads → a writer request must be refused → unlock one by one → the writer gets in →
  a reader request must be refused), and the barrier has three participants (2 workers + main,
  `count=3`, recording arrivals and a `serial` count). Measured on board:
  `rwlock  : PASS (0 violations)`, `barrier : PASS (arrived=2 serial=1 bad=0)`. **This covers the
  state machine and one-shot barrier semantics**; it does not cover "several readers truly
  overlapping" or "no writer starvation under writer preference", which need long contention —
  those two still have no board-level reading.
- `thread == 0` means "no such thread", the same width as `pthread_t` (`uint32`).

Stack ownership: either the caller supplies it (`stackaddr`) or it comes from the layer's static
pool. **The kernel never allocates a stack for you** and will not accept a stack pointer that lies
in kernel RAM or in another App's RAM.

### 4.4 Semaphores (`semaphore.h`)

`sem_t` holds a single kernel handle, so `sem_init` / `sem_destroy` are the whole lifecycle and no
static storage is needed.

| POSIX | Notes |
|---|---|
| `sem_init(s, pshared, v)` | `pshared` is ignored (one address space, no "between processes" notion) |
| `sem_wait` / `sem_trywait` / `sem_post` | direct mapping |
| `sem_timedwait(s, ms)` | **timeout is in milliseconds**, not `struct timespec` (the kernel contract is ms) |
| `sem_getvalue` | **unsupported**, returns -1 with `ENOSYS` |

Why `sem_getvalue` is unsupported: the kernel semaphore service has no "current count" query, and
guessing a number here would race with other tasks. **An error beats a possibly wrong number.**

**Capacity budget (read this when several Apps run concurrently)**: the kernel semaphore table is
**globally fixed-size** (`ARKRTOS_SEM_NUM`, in `kernelsrc/include/arkrtos_config.h`).
**Each POSIX thread consumes 2 slots, each `sem_t` consumes 1 slot**, and when the table is full
`arkrtos_sem_create_internal()` only returns -1 → the POSIX layer maps it to `ENOMEM` → it shows up
as a `pthread_create` failure (**looks like out of memory, actually a full table**). Size it by the
sum over all concurrent users, not by one App's peak. Measured case (boot self-check 109/6 →
116/0) in 同步原语与令牌守恒.md §9.2.

### 4.5 Message queues (`mqueue.h`)

| POSIX | Notes |
|---|---|
| `mq_open(name, oflag)` | create only, never lookup. The kernel has no namespace, `oflag` is ignored |
| `mq_send(q, msg, len_words)` | length unit is **words**, not bytes |
| `mq_receive(q, msg, len_words, timeout_ms)` | timeout is a millisecond argument, not `struct timespec` |
| `mq_close(q)` | `arkrtos_mq_delete` — **the queue is destroyed**, see below |
| `mq_unlink(name)` | **unsupported**, returns -1 with `ENOSYS` (no namespace to unlink from) |

**`mq_close` differs from POSIX**: POSIX `mq_close` only closes a descriptor and leaves the queue
alive; there is no reference count here, so `mq_close` destroys the queue. The queue's life is
therefore decided by the single thread that creates and closes it — do not expect A to close it and
B to keep using it. Likewise `mq_open` does no lookup: two threads each calling `mq_open` with the
same name get **two independent queues**, not the same one — the name is only a label for the
kernel, not a shared key. The correct way to share a queue across threads is for the creator to
pass the returned `mqd_t` to the others.

**The length unit is a word, not a byte**, copied from the kernel contract on purpose: the kernel
copies by word. Compute `len_words` as "how many 32-bit words do I want to send" — sending a
`uint32[2]` means `2`. A message is 1..`ARKRTOS_MQ_MSG_WORDS` (= 4) words long, and the receive
side returns **the number of words actually received**.

### 4.6 Windows-style names (`arkrtos_win_compat.h`)

Layered on top of `arkrtos_posix.h`, providing only calls that **have a real counterpart**.

| Windows | Lands on |
|---|---|
| `Sleep(ms)` | `arkrtos_task_wait(ms)` |
| `GetTickCount()` | `arkrtos_get_time_ms()` |
| `CreateThread(...)` | `pthread_create` |
| `WaitForSingleObject(h, INFINITE)` | `pthread_join` |
| `InitializeCriticalSection` / `Enter` / `Leave` / `Delete` | kernel mutex |
| `strcpy_s` / `strcat_s` / `sprintf_s` | the layer's own bounds-checked implementations |
| `ZeroMemory` | `arkrtos_posix_memset` |

Two mapping notes:

- **`CreateThread`'s start routine is `DWORD(*)(LPVOID)` while the thread service wants
  `void*(*)(void*)`.** `WINAPI` is empty on ARM, so both arguments and return value live in `r0`;
  a direct function-pointer cast and call is correct.
- **`WaitForSingleObject` accepts only "thread handle + INFINITE".** Any other handle or a finite
  timeout returns `WAIT_TIMEOUT`. Reason: a finite wait that times out would have to return
  **without reaping** the thread, and the caller could not distinguish "still running, handle still
  valid" from "already finished" — so it is better to say plainly that it is not supported.

**Explicitly unsupported**: `CloseHandle` returns `FALSE` with `EOPNOTSUPP`. A thread handle is a
thread slot, and the slot is released by `pthread_join`; "closing" without waiting would leave the
thread occupying the slot forever.

### 4.7 File-class POSIX (onto the VFS namespace)

As soon as a name starts with `/`, `open()` goes to the kernel VFS (SVC 0x1C) and the following set
becomes available. The shape of the namespace (`/` is a volatile ramfs, `/dev` is the device image,
`/mnt/<name>` is a persistent volume) is in VFS路径命名空间.md.

| POSIX | Lands on | Notes |
|---|---|---|
| `open(path, flags)` | `arkrtos_path_open` | `O_RDONLY/O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND/O_DIRECTORY` all meaningful |
| `read` / `write` | `arkrtos_path_read/write` | |
| `lseek` | `arkrtos_path_seek` | returns the **new absolute offset** |
| `unlink(path)` | `arkrtos_path_unlink` | files only; directories need `rmdir` |
| `rmdir` / `rename` | `arkrtos_path_rmdir/rename` | rename within one mount; non-empty directories refused |
| `mkdir(path, mode)` | `arkrtos_path_mkdir` | single level; `mode` accepted only for source compatibility and **has no effect** (no permission model) |
| `truncate` / `ftruncate` | `arkrtos_path_truncate/ftruncate` | the file must exist (never created); growth pads with zeros; **refused while a write stream is open** (`ENOSYS`/`EBUSY`, no approximation); `ftruncate` on a read-only descriptor returns `EINVAL` |
| `link` / `symlink` | none | **always -1 + `EPERM`**: the VFS has no inode and no second name — this is the error POSIX prescribes for "this file system does not support hard links" |
| `readlink` | none | missing → `ENOENT`, present but not a link → `EINVAL`; it **never** reports success with an empty string |
| `stat` / `fstat` | `arkrtos_path_stat` | answers only fields the VFS really has |
| `opendir` / `readdir` / `closedir` | `arkrtos_path_readdir` | `readdir` returns `NULL` at end of stream and **sets `errno` to 0**; on error it also returns `NULL` but `errno` is non-zero |

`struct stat` **deliberately holds only what the VFS can answer** (`st_mode` type bits, `st_size`,
`st_mtime`): no `st_ino` / `st_nlink` / permission bits. `st_mtime` is **seconds since boot**, not
wall time (no RTC). Filling in a 0 would be treated by callers as fact, so the field is better left
out. `st_mode` carries only the `S_IFMT` type bits (pinned to the kernel's `ARKRTOS_PATH_S_IF*` by
static assertion), no rwx: the kernel's model is "may this App reach this path", not "owner
read-write, group read-only".

**Capacity is finite and stated plainly**: path handles live in an 8-slot fd table on the App side
(`ARKRTOS_POSIX_FD_MAX`), and directory streams have a separate table of `ARKRTOS_POSIX_DIR_MAX`
(default 2). An empty table makes `open` return `-1` + `EMFILE`, `opendir` return `NULL` +
`ENOMEM`, and **never** lends you somebody else's handle.

**Handles are reclaimed by the kernel when a task exits**: `task_exit` releases the VFS handle held
by that task, so a thread that exits with a file open does not leak the handle outside the App's
8-slot table.

## 5. Why a new kernel thread service

POSIX `pthread_create` needs to create a thread, and **user state has no way to create one**: a
thread needs a TCB and a scheduler slot, and only the kernel holds those. So the kernel added thread
service **SVC 0x1B**, exposing three calls (`arkrtos.h`):

```c
int32 arkrtos_thread_create(void (*entry)(void), void *stack, uint32 stack_size,
                          uint32 priority, uint32 period_ms);
int32 arkrtos_thread_self(void);
void  arkrtos_thread_exit(void);
```

The layer builds POSIX semantics on top of these three. One design detail is worth writing down:
the **startup gate**.

The kernel thread service takes a bare `void (*)(void)`, while POSIX hands over "a start routine +
one argument". Bridging uses a trampoline plus one slot per thread:

1. `pthread_create` claims an entry in the slot table and writes `start_routine`, `arg`, the stack
   pointer and everything else into it;
2. calls `arkrtos_thread_create`, the kernel returns a task number;
3. writes the task number into the slot too;
4. **only then does it `sem_post` the startup gate.**

The new thread's first action is `sem_wait(gate)`; once the gate opens it finds its own slot by
task number and takes out the routine and argument.

Why not do something cheaper? Because if the thread were released right after being created, it
could be scheduled onto the CPU before its slot was fully written and read a half-built slot. The
gate makes the ordering **explicit** instead of gambling on preemption timing. The cost is one
semaphore per thread, buying off a class of extremely hard-to-reproduce intermittent bugs.

When the thread finishes it posts a second semaphore (`done`); `pthread_join` waits on it and then
releases the slot. Both semaphore names must fit in 8 bytes (`pth0g` / `pth0d`, last digit is the
thread index).

## 6. App-side heap

The layer carries a small **first fit + adjacent-block coalescing** allocator inside the App's own
RAM, 1 KB by default.

- API: `arkrtos_posix_malloc` / `free` / `calloc` / `realloc`, plus `arkrtos_posix_heap_total()`
  and `arkrtos_posix_heap_free_bytes()`.
- A pointer outside the arena is **refused** rather than written through.
- Deliberately **not a kernel service**: the kernel has no heap, the MPU already confines the App to
  its own window, and the allocator sits on the same side of the privilege boundary as its caller —
  one fewer failure point.

**The toolchain C library's own heap is separate.** The arena above belongs to
`arkrtos_posix_malloc`; an **unmodified** ordinary program calls the toolchain libc `malloc`, which
lands in the library heap region of the scatter file. App scatter files do not define a heap by
default, so a program using `malloc` fails at **link time** with
`L6915E: Heap was used, but no heap region was defined` — on purpose (link failure beats a bad
pointer that explodes at run time). To enable it, give a size when generating the scatter file
(both `tools/gen_app_sct.py` and `tools/gen_scatter.py` support it):

```bash
py -3 tools/gen_app_sct.py --project posix_demo.uvprojx --type app --dev-slot 3 \
    --ram-size 8192 --heap-size 2048
```

The generated three-region layout is fixed as `RW → ARM_LIB_HEAP → ARM_LIB_STACK` and the order
cannot be changed: a region's `+0` is the **upper bound** of the preceding region, RW's upper bound
is the stack bottom, and a heap placed after the stack necessarily overlaps the stack-protected
area. Without `--heap-size` the behaviour is exactly as before (default 0).

**`ARKRTOS_POSIX_WRAP_STDLIB` is off by default.** When on, `malloc/free/calloc/realloc` are
macro-replaced onto this arena. Why off: a program written for the toolchain libc heap, silently
moved onto a 1 KB arena, would fail at run time with `ENOMEM` for no visible reason; leaving
`malloc` alone forces the user to state the intent by turning this macro on.

## 7. On-board self-test coverage

Section 9 of the self-test in `example/stm32f427/app_sdk/APP_DEMO` calls the layer's **public
names** (`pthread_create`, `sem_wait`, `mq_receive`, `CreateThread`, `strcpy_s` ...), so the build
proves "the name mapping works" and the run proves "the semantics match".

On-board output (F427 + DAPLink, 115200) is all `OK`:

```
posix.malloc / calloc_zero / realloc / heap_reclaim      OK
posix.pthread_create / pthread_join / pthread_ran / pthread_retval   OK
posix.mutex / sem_trywait / sem_empty / sem_wait / usleep            OK
win.createthread / wait_infinite / thread_ran / wait_timeout         OK
win.strcpy_s / strcpy_s_bounds / sprintf_s                           OK
```

Some of these criteria are worth noting — they judge by behaviour, not by "the return value is
non-negative":

- `pthread_retval`: the thread does `return (void*)0x5A5A`, and after join `retval == 0x5A5A` is
  checked — proving the return value really came back through the trampoline.
- `pthread_ran`: the thread increments every round, and after join the counter delta equals the
  expected number — proving the thread really ran, the right number of times.
- `heap_reclaim`: free bytes after `free` return close to the pre-`malloc` value — proving
  coalescing really happened.
- `sem_empty`: with a count of 0, `sem_trywait` must fail — proving "empty means failure" rather
  than a false success.
- `win.strcpy_s_bounds`: a deliberately small buffer must return a **negative** value — proving the
  bounds check really blocks.

## 8. Usage advice

- **Just run an existing POSIX program**: `#include "arkrtos_posix.h"`, add `arkrtos_posix.c` to
  the project, deal with `main`'s return value and arguments, leave the rest alone.
- **An App template for customer secondary development**: use `arkrtos_win_compat.h`. The names
  customers know are all there, and behind them are real implementations, never "compiles but
  behaves wrong".
- **RAM-tight driver Apps**: do not include the umbrella header. Include only what you need and
  lower `ARKRTOS_POSIX_THREAD_MAX` to the real requirement — thread stacks are static, so do not
  reserve slots for threads you never create.
- **Data that needs queueing**: prefer `mqueue` (backed by the kernel message queue) over pairing a
  semaphore with a global array — that gives no length semantics and you end up writing the race
  handling yourself.
- **Do not expect `errno` to be free in a hot loop**: one SVC. Store it locally if needed.

## 9. Related files

| Path | Contents |
|---|---|
| `kernelsrc/sdk/posix/` | all headers and the implementation of the layer |
| `kernelsrc/include/arkrtos.h` | thread service (`arkrtos_thread_*`) and all kernel service contracts |
| `kernelsrc/src/arkrtos_loader.c` | dispatch of thread service SVC 0x1B and VFS path service SVC 0x1C |
| `kernelsrc/include/arkrtos_vfs.h` / `kernelsrc/src/arkrtos_vfs.c` | kernel side of file-class POSIX (§4.7) |
| `example/stm32f427/app_sdk/APP_DEMO/Src/main.c` | section 9 self-test (on-board verification of the layer) |
| `example/stm32f427/app_sdk/POSIX_DEMO/Src/main.c` | section 10 test piece (an unmodified POSIX program) |
| `example/stm32f427/app_sdk/FS_DEMO/Src/main.c` | section 11 test piece (file-class POSIX self-check) |
| `docs/配置区与安装策略.md` | slot RAM window and install strategy |
| `docs/Ark RTOS应用安装与调试指南.md` | packaging, install, and on-board workflow |
| `docs/api/Ark RTOS_API参考.md` | SDK interface reference |

## 10. An unmodified POSIX program ran (on-board evidence)

The sections above list capabilities; this section reports how far the sentence "what you change is
the include list, not the program structure" has actually been verified.

Test piece: `example/stm32f427/app_sdk/POSIX_DEMO/Src/main.c`. It is an ordinary console program —
`<stdio.h>` / `<string.h>` / `<stdlib.h>` / `<ctype.h>` plus `<unistd.h>` / `<pthread.h>` /
`<semaphore.h>`, with no `arkrtos.h`, no device name, no address, no partition number. The same
source compiles on Linux with `cc main.c -pthread`.

### 10.1 What the layer had to add for it to compile

| Gap | Fix |
|---|---|
| POSIX names such as `sem_t` / `mqd_t` | `semaphore.h` adds `typedef arkrtos_sem_t sem_t;`, `mqueue.h` adds `typedef arkrtos_mqd_t mqd_t;` (kernel-side type names unchanged) |
| `ssize_t` / `off_t` / `pid_t` / `mode_t` / `time_t` / `useconds_t` | `arkrtos_posix_types.h` gives them uniformly; `ARKRTOS_POSIX_NO_STD_TYPES` turns it off when the toolchain already provides them |
| The App side pulling in the partition header | `arkrtos_posix.c` used to include `arkrtos_config.h` (which drags in `arkrtos_partition.h`, forbidden in Apps); it now includes the self-contained `arkrtos_features.h` |

### 10.2 The printf outlet: C-library glue

`arkrtos_libc_glue.c` does three things:

1. declares `__use_no_semihosting` (otherwise semihosting wires `_sys_write` to the debugger and
   not one byte reaches the serial port);
2. routes `fputc` / `_ttywrch` straight to the console device (`arkrtos_dev_write`), so `printf`
   takes the ordinary device path;
3. **AC5 (ARMCC 5) only**: defines the full `_sys_open/_sys_close/_sys_write/_sys_read/_sys_istty/
   _sys_seek/_sys_ensure/_sys_flen` set **and** also defines `__stdin_name` / `__stdout_name` /
   `__stderr_name` (value `":tt"`).

Why item 3 must also take over the names: in AC5's `armlib/c_4f.l`, `sys_io.o` (source
`../clib/angel/sysapp.c`) defines **the whole set** of those 8 `_sys_*` functions, and the entry
that pulls it into the image is exactly the three `__std*_name` symbols. Without taking over the
names, `sys_io.o` collides with the whole custom `_sys_*` family
(`L6200E: Symbol _sys_write multiply defined`); with the names taken over, `sys_io.o` is kept out of
the image and the remaining references are satisfied by the custom hooks. `_sys_open` recognizes
only `":tt"` (the fixed stdin/stdout/stderr name) and returns `-1` for anything else — **it must
not return a plausible-looking handle**, or `fopen()` would take it and read empty content.

### 10.3 How the App project is assembled

- **Do not add `arkrtos_app_main.c` to the project**: it carries a strong `int main(void)`, the user
  program also has `main`, and AC5 produces two `__ARM_use_no_argv` definitions → `L6200E` at link
  time. The entry chain `arkrtos_app_start.s` → `__main` → user `main()` is enough.
- The project's BeforeMake hook passes `--heap-size` to scatter generation (§6); otherwise programs
  using `malloc` will not link.
- **When copying another App project as a template, the `<TargetName>` in `.uvoptx` must be changed
  too.** It is Keil's download/debug entry index: if it still names the old target, `UV4 -f` fails
  with `Internal DLL Error` + `Flash Download failed - Target DLL has been cancelled`, while other
  projects flash fine at the same moment — it looks like broken hardware but is only this one field.

### 10.4 On-board result (F427 + DAPLink, COM3 / 115200)

Flashed into development slot 3 (partition id 5, base `0x080A0000`, auto-start); after reset the
console shows:

```
POSIX demo: an unmodified POSIX C program as an Ark RTOS App
string  : HELLO ARKRTOS (13 chars)
strcmp  : as expected
sorted  : app 5 / vfs 12 / loader 21 / kernel 30
heap    : malloc works
sleep   : 1 s
gate    : releasing both threads
[A] released, running / [B] released, running
[A] round 1..3 / [B] round 1..3
threads : joined
done    : entering heartbeat
heartbeat                       ← once every 5 s, forever
```

`app list` confirms at the same time that this is the running instance:

```
id  type  state    auto  task  crash  held  base        size     ram         entry
 5   app   RUNNING  yes    6     0     none  0x080A0000  131072   0x2001C000  0x080A0001
```

Conclusion, graded: **verified on board** — `printf` really comes out of the console,
`pthread_create/join`, `sem_wait`, `qsort`, `malloc/free`, `sleep` all behave per POSIX semantics on
the board, and the program source contains not one Ark RTOS-specific name.

## 11. File-class POSIX ran too (verified on board)

Section 10 verified the "devices and threads" half (`pthread` / `semaphore` / `printf`). The
file-class half has its own test piece:
`example/stm32f427/app_sdk/FS_DEMO/Src/main.c`, including only `<stdio.h>` / `<string.h>` /
`<fcntl.h>` / `<unistd.h>` / `<sys/stat.h>` / `<dirent.h>`, running a self-check on `/` (ramfs).

What it covers (see that directory's `README.md`): create/write/read-back,
`lseek(SEEK_SET/SEEK_END)`, `stat`/`fstat` type bits and length, `opendir("/")` + `readdir` finding
the same name, opening `/dev/ttyS0` as a path, writing a line and distinguishing it from a regular
file through `fstat`'s `S_ISCHR`, a missing path must be `-1`/`NULL` with `errno==ENOENT`, and the
same path must fail after `unlink`.

### 11.1 An include trap only the toolchain exposes

`<errno.h>` **must be written as `"errno.h"`** (quoted form). Reason: `errno.h` is an ISO C header
and the AC5/AC6 toolchain ships its own; the angle-bracket form finds that one first — and it does
not have POSIX codes such as `ENOENT`, so the build fails:

```
..\Src\main.c(226): error:  #20: identifier "ENOENT" is undefined
```

`<unistd.h>` / `<dirent.h>` / `<sys/stat.h>` are POSIX headers the toolchain does not provide, so
the angle-bracket form does find the SDK's and needs no change. This is the case of "an ISO C header
being shadowed by the toolchain while SDK coverage is incomplete"; the fix is the quoted form (on
Linux the quoted form falls back to the system header, so both ends compile).

### 11.2 Measured on board

**Verified on board** (F427, `UV4 -f` into development slot 3, serial captured after reset):

```
  ok   open /dev/ttyS0 as path      
  ok   fstat is char device        
  ok   single write is not an error
[fs_demo] device opened as a path
  ok   write to device path        
fs_demo: pass=32 fail=0
```

The device name is `/dev/ttyS0`, not the assumed `/dev/uart0`: the names listed under `/dev` come
straight from the kernel device registry (on this board `led0` / `led1` / `led2` / `ttyS0` / `BLED`),
and "it compiles" proves nothing about a name existing — the first version failed on board with
`FAIL open /dev/uart0 as path (saw -1)`. The fix: `opendir("/dev")` to enumerate first, then open
by registered name.

Flashing is the same as `POSIX_DEMO`: `UV4 -f example/stm32f427/app_sdk/FS_DEMO/MDK-ARM/fs_demo.uvprojx`;
after reset the console prints `fs_demo: pass=32 fail=0` and then enters the heartbeat.

### 11.3 Two real problems the board exposed

**① A character device opened by path made `fstat` return -1.** The first version failed with
`FAIL fstat is char device (saw -1)`. `/dev/ttyS0` opened by path is a VFS handle, and `fstat` on a
VFS handle always computes the size with `lseek(SEEK_END)` — a character device has no position to
seek to, hence -1, contradicting `sys/stat.h`'s own statement that "a device reports `S_IFCHR`,
size 0". Fix: on `open`, call `arkrtos_path_stat` first and cache the `S_IFMT` type bits in the fd
slot; `fstat` answers from the cache, and only regular files compute length by seeking.

**② Writing once to a device returned 0 (not a single byte went in).** The second version failed
with `FAIL write to device path (saw 0)`. Root cause in the VFS devfs port: it stored the kernel
device handle in an `int16_t`, while a handle is `(slot | ARKRTOS_DEV_HANDLE_FLAG 0x01200000)`; with
the high flag truncated the kernel consistently treated the handle as invalid and returned -1 (that
was the earlier `saw -1`). Changing the type to `int32_t` stopped the error. **But returning 0 is
not itself an error**: the driver's transmit pipeline is a fixed-length ring, and a write that lands
exactly when it is full can accept no byte at all; POSIX allows short writes on character devices.
(`opened==0` and a bad magic were ruled out on the device side — the same pipeline was printing
`printf` output normally before and after.) So the correct client form is the one the kernel console
itself uses: retry byte by byte until done, with an upper bound. After the fix all 33 bytes arrive,
`[fs_demo] device opened as a path` appears on the console, and the self-check reports
`pass=32 fail=0`.
