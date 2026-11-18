# arkc On-Board Direct Run M4: a standard C hello world on NOR — `cd` into the directory, `make`, then `mini run`

> Condensed English edition. The Chinese page (arkc板上直跑-M4-hello演示.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

**This page records the demo path itself**: a **standard C** hello world project (`#include
<stdio.h>` + `printf` + `int main(void)`, not one line of Ark RTOS code) whose sources live
permanently in the external NOR flash under `/mnt/nor/hello/`; on the board one types `make`,
the small-program image is produced, `mini run` loads and runs it, it prints `Hello, World!`
and then exits by itself, returning both RAM blocks to the pool.

```
ark@arkrtos:~$ cd /mnt/nor/hello
ark@arkrtos:/mnt/nor/hello$ make
make: hello.svcm <- /mnt/nor/start.c (+more)
make:   src /mnt/nor/rt.c
make:   src main.c
...
cc: hello.svcm: payload 10760 + table 26 x 4 = 11120 B
make: ok
ark@arkrtos:/mnt/nor/hello$ mini run hello.svcm
MINI:579 /mnt/nor/hello/hello.svcm: 10760 B payload -> code 0x24070000 (16384 B block), RW/stack 0x24074000 (16384 B), task 4
Hello, World!
TASK:2584 task 4 exits (self kill)
MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
```

## 0. Why there is no longer a `hello` command

Before 2026-11-07 the board had a `hello` command: it scattered demo sources into `/demo`
and printed a list of command lines for the user to retype. That is not the Linux
experience — project files belong on the disk next to the program, and building belongs to
`make`. That command was **deleted entirely** (command, sources, `USE_HELLO` switch,
Makefile macro, switch fingerprint), and the regression case `mini-sdk-std-hello` carries a
false-green assertion `hello → command not found` so nobody adds it back.

## 1. One-time preparation

### 1.1 Firmware

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j8 CROSS="<arm-none-eabi->" \
     USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
```

`USE_SDRAM=1` here is not just "a bit more memory": it moves arkc's compile pool (4 MB), the
ramfs file buffers, and the four 16 KB payloads plus the 48 KB object/image area of
`cc --pack` into external SDRAM (`ARKRTOS_PACK_IN_EXTRAM` follows `USE_SDRAM`). On-chip AXI
SRAM keeps only the kernel and stacks.

Measured sizes (2026-11-07, including `printf`): FLASH 462088 B (88.14%), RAM 285900 B
(63.45%), SDRAMBUF 10744 KB / 12 MB, SDRAMFS 5882656 B / 6 MB (93.50%).

Flashing (OpenOCD + DAPLink; there is a ready wrapper in `tools/final_app.py`):

```sh
py -3 tools/final_app.py            # flash first, then run 6 cases by default
py -3 tools/final_app.py --noflash  # already flashed, cases only
```

### 1.2 Project files into NOR (only when changing projects)

Things on NOR survive power loss, so this step is **not needed for every demo**:

```sh
py -3 tools/mini_deploy.py --port COM3
```

It does two things:

1. creates the directory **`/mnt/nor/hello/`** on the volume (via littlefs `mkdir`) and puts
   the project directory (`main.c` + `makefile` from `example/mini_hello/`) inside it;
2. puts the small-program SDK's 6 files at the **volume root** (`start.c` / `rt.c` /
   `arkrtos.h` / `unistd.h` / `stdio.h` / `stdarg.h`) and removes the leftover `/mini_main.c`
   of the old shape.

Why split that way: the SDK is "the runtime of this compiler", one copy; the project is
"what you want to build", and many can sit side by side on the same volume (`hello/`,
`blink/`…), each makefile pointing back at the SDK at the root by absolute path.
`--remote-dir` changes the project landing point (default `/hello`).

## 2. On the board: `cd` + `make` + `mini run`

After power-up `/mnt/nor` is already mounted (`ARKRTOS_AUTOMOUNT_NOR`, see §4); no need to
type `fs mount` / `mount` first:

```
ark@arkrtos:~$ ls -l /mnt/nor
  -rw-r--r--   1 0     0         2980  start.c
  -rw-r--r--   1 0     0        13164  rt.c
  -rw-r--r--   1 0     0         2787  arkrtos.h
  -rw-r--r--   1 0     0         2459  unistd.h
  -rw-r--r--   1 0     0         3418  stdio.h
  drwxr-xr-x   1 0     0            0  hello
ark@arkrtos:~$ ls -l /mnt/nor/hello
  -rw-r--r--   1 0     0           84  main.c
  -rw-r--r--   1 0     0         1533  makefile
ark@arkrtos:~$ cd /mnt/nor/hello
ark@arkrtos:/mnt/nor/hello$ make
make: hello.svcm <- /mnt/nor/start.c (+more)
make:   src /mnt/nor/rt.c
make:   src main.c
make:   -I/mnt/nor
cc: pack 3 sources -> hello.svcm (4 passes, -O1)
cc:   A 10760 B
cc:   B 10760 B
cc:   C 10760 B
cc:   D 10760 B
cc:   A +0 is 0xB5E0 (flat image, no vector table; rom base 0x8000000, window 16384 B)
cc:   entry mode: flat miniapp, entry at +0 (no Reset_Handler)
cc: hello.svcm: payload 10760 + table 26 x 4 = 11120 B
cc:   reloc rom 0 / ram 0 / rom_movw 3 / ram_movw 23, entry +0x0
cc: packed in 10837 ms
make: ok
ark@arkrtos:/mnt/nor/hello$ mini run hello.svcm
MINI:579 /mnt/nor/hello/hello.svcm: 10760 B payload -> code 0x24070000 (16384 B block), RW/stack 0x24074000 (16384 B), task 4

mini running: task 4, code 10760 B in 16384 B @ 0x24070000, RW/stack 16384 B @ 0x24074000
  running 1 of 2 allowed
Hello, World!
TASK:2584 task 4 exits (self kill)
MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
ark@arkrtos:/mnt/nor/hello$ mini
mini: none running (usage: mini run <path>)
```

Each of the four differential passes takes about 2.7 s; the whole `make` is 10.8 s (3
sources × 4 passes plus 4 links). `mini run` takes a **relative name**: it uses the same path
resolution as `make` reading a makefile (based on the shell cwd), so with cwd at
`/mnt/nor/hello` both work.

## 3. What is in this project

### 3.1 `main.c` — 84 bytes, standard C

```c
#include <stdio.h>

int main(void) {
    printf("Hello, World!\n");
    return 0;
}
```

On Linux this is `cc main.c && ./a.out`, with identical output. **No `MiniMain` / `AppMain`,
no `write`, not a single Ark RTOS header.**

`main()` is the entry: the SDK's startup stub (`start.c`) calls it, and after it returns the
stub calls `arkrtos_thread_exit()` to hand both RAM blocks back to the kernel — "process
teardown" is the runtime's job, just as on a desktop where the C library does `exit()` for
you.

### 3.2 The SDK's two sources and four headers

| File | Role |
|------|------|
| `start.c` (source `arkrtos_mini_start_arkc.c`) | `MINISTART`: calls `__arkcc_init_globals()` (arkc compiles global initial values into one function), then `main()`, and after it returns calls `arkrtos_thread_exit()`. **Must be first in `SRCS`** |
| `rt.c` (source `sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c`) | runtime: two SVCs for POSIX `write()` / `_exit()` (`0x1A` subcommand 3 prints, `0x1B` subcommand 3 exits, bit-identical to `app_sdk/arkrtos_oslib.c`), plus implementations of `printf`/`puts`/`putchar` |
| `arkrtos.h` | `arkrtos_thread_exit()`, `ARKRTOS_WEAK`, and the entry contract (the project writes `int main(void)`) |
| `unistd.h` | `STDOUT_FILENO`/`STDERR_FILENO`, `write`/`_exit` |
| `stdio.h` | `printf`/`puts`/`putchar`, plus the **supported conversions and hard limits** |
| `stdarg.h` | `va_list`/`va_start`/`va_arg`/`va_end` (from `ark c compiler/stdlib/include/stdarg.h`, copied along by `sync_arkc.py`) |

**Why not the full `arkrtos_posix.c`**: that file is 3600 lines and drags the socket and VFS
chains, while a small program's **whole payload is copied into RAM to execute** (16 KB code
block) and does not fit. So only the console subset is provided here, and what is missing is
stated honestly in the headers.

### 3.3 Why `printf` is not written with `va_list` (although arkc now has `va_list`)

The history first, because this section's title used to be "the ARM backend has no va area":

**That used to be true.** `irgen` asked for a0..a7 to be mirrored at `frame+8000` for every
variadic function, but only the C and x86_64 backends defined `_ark_va_ptr` as a local
pointing at that area; the ARM backend never implemented it, and a C variadic function
compiled on ARM became

```
        MOV r0, #0
        LDR r0, [r0]        ; read the va pointer from address 0
```

— no error, and then `va_arg` walked down from the vector table at address 0 (which is a
stack address): wrong answer, no diagnostic (measured with `examples/va_test.c`). It was then
changed to "refuse explicitly", which only stopped giving wrong answers; the feature was
still absent.

**Both backends implement it now** (arkc `e75c2ca`). Two shapes:

| Backend | Area | Cost |
|---------|------|------|
| ARM Thumb-2 | prologue `PUSH {r0-r3}`, those four words land right below the caller's stack argument block: words 0..3 = r0..r3, word 4+k = k-th stack argument word, contiguous, no copying | 16 bytes of stack per call + one `ADD sp, sp, #16` before return; stack arguments have **no count limit** |
| RV32I | the 32 bytes at the frame bottom (`s0 - sp_size + 4*k`), using slack `sp_size` already reserved | no extra stack; only the eight words a0-a7, no stack arguments |
| C / x86_64 | `frame+8000+8k`, one 8-byte slot per actual argument | the old way, unchanged line for line |

`stdarg.h` picks the shape from `__ARK_VA_WORD_SLOTS` / `__ARK_VA_PACKED_WORDS` injected by
`compiler.c`. Execution semantics are pinned by `examples/run_va_gate.py`: the same source
goes to gcc and to arkcc→as→unicorn, comparing **run results**, 27 items (two groups need no
external toolchain), and **every case asserts frame ≤ 1024 bytes** — not decoration: the first
implementation used an 8 KB mirror area, was all green on the host, and the small-program task
stack on the board is only 4 KB (`APP_TASK_STACK_SIZE`), so it would necessarily overflow
there.

**Two silent wrong answers that only the board exposed** (arkc `2b500a6`, host gates green at
the time):

1. **arkc's own Thumb assembler dropped `lr` from `POP {.., lr}`.** The 16-bit POP has a
   single bit at position 8, and in POP that bit means **PC** (only PUSH can push LR). The
   variadic epilogue must be `POP {r5,r6,r7,lr}` + `ADD sp, sp, #16` + `BX lr` (skip the r0-r3
   mirror before returning); it assembled as `bce0` = `POP {r5,r6,r7}`: every return from a
   variadic function leaked 4 bytes of SP and never restored `lr`. The register-allocation
   post-pass could also rewrite that line as `POP {..,pc}`, which is worse — it jumped back
   while the mirror was still on the stack. It is now encoded as T2 `ldmia.w sp!, {..,lr}`;
   `POP` naming both lr and pc, and `PUSH` naming pc, are now direct errors instead of silently
   dropping a register.
2. **A callee that is only declared, never defined, was always assumed to "return one word".**
   All three backends looked up the return shape in `prog->funcs` (functions **defined** in
   this translation unit), while `irgen` decides how many words to pop from the call
   expression's type. So `extern void h(void); h();` left 4 bytes of stack per call, and
   `extern long long e(void); return e();` never pushed the high word at all. Now `irgen`
   encodes the return shape into `IR_CALL`'s `val` (bit32 = two words, bit33 = void) and the
   backend uses it when the name is not found.

**How these two relate to variadics**: item 2 is exactly the root cause of the on-board
`va_arg(ap, long long)` misread. `__arkcc_init_globals()` in `MINISTART` is such a
"declared-only" void call; it lowered `main`'s frame top by 4 bytes, so the ARM argument mirror
was no longer 8-byte aligned — `stdarg.h`'s "round up to 8" became a no-op, 64-bit actual
arguments were read one word early, and the low word `0x55` was taken as the high word
(measured 85, correct is 17). After the fix all six on-board readings are right:

```
va sum6      = 21 (want 21)
va sum20     = 210 (want 210)
va ll_hi     = 17 (want 17)
va tail      = 21 (want 21)
va dbl_tag   = 255 (want 255)
```

Neither class is catchable on the host: the gate uses GNU as (a different encoder), and the
callees are all in the same translation unit (`prog->funcs` finds them). So the gate gained two
**toolchain-free** assertion groups: the byte-level encoding of the return sequence, and the
result shape of the two "declared only" call forms above.

**So why is `printf` still an argument-fetch stub?** Two reasons:

1. The stub is already verified on the board (the `mini-sdk-std-hello` case and the `printf`
   host gate); there is no reason to swap a verified implementation for looks.
2. It is part of the SDK and does not depend on the compiler's variadic implementation: if the
   compiler's va area changes later, `printf` is unaffected. If you really want your own,
   `#include <stdarg.h>` and write it (see limitation 7 below).

So there is no macro dispatching on argument count (that would stop `printf` being a
function); it stays one `__asm` **argument-fetch stub**:

```c
__asm int printf(const char *fmt, ...)
{
    PUSH {r4, lr}
    SUB  sp, sp, #48          /* 12 slots: format string + 11 arguments */
    STR  r0, [sp, #0]         /* r0-r3 = first 4 arguments */
    ...
    LDR  r0, [sp, #56]        /* 5th-12th arguments pushed by the caller */
    STR  r0, [sp, #16]
    ...
    MOV  r1, sp
    LDR  r0, [sp, #0]
    BL   arkrtos_mini_printf_words
    ADD  sp, sp, #48
    POP  {r4, pc}
}
```

Why this is correct: under AAPCS the first 4 arguments are in `r0-r3`, and from the 5th on the
caller pushes them onto its own stack, forming **one contiguous run of words** together with
`r0-r3`. The stub does one thing — spill that run into 12 slots on its own stack, then hand
that memory to the ordinary C function `arkrtos_mini_printf_words()` to fetch by format string.
To the caller `printf` is an ordinary variadic function; not one macro is needed.

Supported conversions: `%d %i %u %x %X %c %s %p %%`, flags and width `-` / `0` / 1-2 decimal
digits. **Not supported**: precision (`%.3s`), length modifiers (`%ld`), floating point
(`%f`), width written as `*`.

### 3.4 `makefile` (1533 B, in `/mnt/nor/hello/`)

```
TARGET = hello
OUT    = hello.svcm
SRCS   = /mnt/nor/start.c /mnt/nor/rt.c main.c
INCS   = /mnt/nor
```

- The three sources take 44 bytes; with **absolute paths** the line still fits in
  `ark make`'s 96-byte line, so there is no `..` form that depends on guessing path resolution.
- The first entry of `SRCS` must be the startup stub: the image's entry offset is relative to
  the payload start, so `MINISTART` must sit at offset 0.
- **No** `mini_main.c`: the entry is the project's own `int main(void)`. That old skeleton
  ("main() calls MiniMain()") is gone everywhere (Keil / GCC tiers too), and on the arkc tier
  pushing it in gives a direct `#error` (see §5 item 8).

## 4. `/mnt/nor` mounted at power-up (`ARKRTOS_AUTOMOUNT_NOR`)

The board's "fstab": at boot, after VFS comes up, mount the NOR volume at `/mnt/nor`, so the
console can `cd` there immediately. The switch defaults to **0** (no mount, no print, does not
touch littlefs or the VFS mount path; the console's first screen and the command table stay
byte-identical), and the board build ties it to `USE_QSPI` — without NOR there is no volume. A
failed mount does not block boot (blank flash or an unpopulated chip must still reach the
console); it prints one line:

```
FAIL BOOT: cannot mount nor0 at /mnt/nor: -5 (...); the console still comes up, run 'fs mount' / 'nor info'
```

## 5. Known limitations (none of them is "just tune it a bit")

1. **One `make` takes at most 4 source files** (`ARKMAKE_MAX_SRCS`), and object slots are split
   evenly across sources from `CC_PACK_OBJ_ARENA`. In the SDRAM tier that block is 48 KB: 4
   sources give 12288 B per slot, 3 sources 16384 B. **Slots are sized by object files, not by
   code length** — `rt.c` with printf is about 2.5 KB of code but its object file is 8660 B
   (ARKOBJ2 also carries a symbol table and relocations). Sizing by code length is off by 3x,
   and this round failed exactly there (it reported `compile rc=-1 (File too large)`; the slot
   size is now printed too).
2. **The first entry of `SRCS` must be the startup stub**: a small program has no reset vector
   (the kernel starts it as a task and `mini run` gives it a RAM block), so the entry is
   payload `+0`. Packing verifies that `+0` is a THUMB `PUSH` (`0xB5Ex`) and refuses to emit an
   image otherwise — the entry is not guessed.
3. **Only one object may own `.data/.bss`**: `-link` does not move data sections, and two
   objects carrying RW collide at the same address and **error out** (measured): `SRAM overlap
   between objects #2 and #3`. So the buffers of `write()` and `printf` live in `rt.c`; the
   project must not declare initialised globals of its own.
4. **A flat image has no vector table**: the entry offset is always 0, `mini run`'s stack comes
   from a RAM block, and the SP/entry fields in the image header are irrelevant. For images
   that **define `Reset_Handler`**, `cc --pack` still takes the old path ("reserve a 0x400 hole
   + `-link` fills the table"; the M2 case is that one); the two paths are decided from the
   object's own symbol table, not from a declaration.
5. **Console output is NUL-terminated strings**: `write()` copies in chunks and appends `\0`, so
   binary to the console is out of scope (stated in `unistd.h`).
6. **One `printf` call takes at most 12 arguments (format string + 11)**: the stub moves 12
   words, and from the 13th on nothing is fetched (0 is read). The compiler itself allows more,
   so this limit belongs to the SDK and is written in `stdio.h` and here — it is not "the
   compiler can't".
7. **A project may define its own C variadic functions, costing only 16 bytes of stack**: after
   `#include <stdarg.h>`, `va_start`/`va_arg`/`va_end` all work (the ARM backend does
   `PUSH {r0-r3}` in the prologue, stack arguments stay where the caller pushed them, and
   `va_arg` walks up from there). Stack arguments have no count limit; it fits in the 4 KB
   small-program stack too — this limitation used to say "frames grow to 8 KB, basically do not
   use it", which is no longer true.
8. **The project must define `int main(void)` itself**: on the arkc tier the SDK provides no
   default `main`; missing it is a link-time `unresolved symbol "main"`, not a "runs but prints
   nothing" empty program. The history is worth recording: the old skeleton had the SDK provide
   `main()` plus the project's `MiniMain()`, and the SDK also provided a weak empty `MiniMain`
   — arkc bound it inside the same translation unit, so the project's strong definition never
   got in, and the symptom was exactly "exits on the very tick it is created, printing nothing",
   with neither error nor diagnostic.

## 6. Regression cases

Two in `tools/final_app.py`:

| Case | What it judges |
|------|----------------|
| `mini-sdk-std-hello` | `ls -l /mnt/nor` shows the six SDK files + the `hello` directory (proving auto-mount at power-up) → `hello` is **command not found** (false-green check) → `cd /mnt/nor/hello` → `cat main.c` contains `#include <stdio.h>` and `printf("Hello, World!\n");` → `make` (3 sources, four passes, `entry mode: flat miniapp`, `payload + table`) → `mini run hello.svcm` prints `Hello, World!` and both RAM blocks are returned |
| `mini-sdk-missing-main` | with no `main` defined the build must report `unresolved symbol "main"` + `cc: pass A failed`; it must **not** "report an error but still emit a file" (the linker used to report a diagnostic and still write the image, do all four passes, and then fail at packing with an unrelated -33) |

## 7. Host gate for `printf`

Almost every `printf` bug is "it printed a different sentence", invisible to the eye. So the
formatting core also runs on the host:

```sh
py -3 tools/mini_sdk_printf_gate.py
```

It compiles `kernelsrc/sdk/mini_sdk/tests/test_printf_host.c` with `gcc -m32` (**32-bit is
mandatory**: slots are 32-bit and pointers are truncated on a 64-bit host) and compares 24
fixed cases character by character: negative numbers, `0x80000000`, the `%u` upper bound,
`%x/%X`, `%08x`, width, left alignment, `%s` null pointer, `%c`, `%%`, `%p`, 11 arguments,
unknown conversion, a lone `%` at the end, and segmented flush past the 96-byte buffer. The
stub itself is ARM assembly and cannot run on the host; it is covered by the board cases above
— together the two sides complete this chain.

## 8. Related documents

- M1/M2 (fixed-window direct run, four-pass differential packing): [arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)
- QSPI NOR and the littlefs volume: [arkc板上直跑-M3b-QSPI持久化.md](arkc板上直跑-M3b-QSPI持久化.md)
- `ark make` itself: [ark make-板上构建工具.md](ark make-板上构建工具.md)
- Switch table: [功能开关.md](功能开关.md)
