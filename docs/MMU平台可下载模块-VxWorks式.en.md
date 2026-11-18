# Downloadable Modules on MMU Platforms (VxWorks style) and the Two Application Models

> Condensed English edition. The Chinese page (MMU平台可下载模块-VxWorks式.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

This page records M2: a **load-at-runtime, link-against-kernel-symbols-at-load-time** module mechanism (the VxWorks `ld` / `lkup` / `modules` / `unld` family), and how it relates to M1's Linux-style `exec` (see MMU进程模型与ELF装载.md).

In one line: **they are two side-by-side application models, each selected by Kconfig, each can be on alone or both on; M2 does not replace M1.**

## 1. The two models side by side

| | Linux style (`ARKRTOS_USE_ELF`) | VxWorks style (`ARKRTOS_USE_MODLOAD`) |
|---|---|---|
| Input | `ET_EXEC` executable (`.elf`) | `ET_REL` object file (`.o`) |
| Address | fixed at link time (`-Ttext=0x02000000`) | chosen by the loader at load time (a pool on the board) |
| External references | self-contained (statically linked or none) | looked up **by name** in the kernel export table |
| Privilege | PLV3 (isolated from the kernel) | PLV0 (kernel mode, no isolation) |
| Who writes it | end users writing programs | kernel/driver plugin developers |
| Lifetime | runs and exits | resident after load, visible via `modules`, removable via `unld` |
| Cost of a bug | App dies, kernel lives | a module writes a bad pointer and the system goes down with it |

This is the classic VxWorks 5.x trade-off: **modules can call kernel APIs directly, therefore they can also modify kernel memory directly.** Want a wall? Do not enable this switch; use `ARKRTOS_USE_ELF`.

All four combinations of the 2×2 matrix compile (`elf_gate` / `modld_gate` each compile the "off" direction too):

| `USE_ELF` | `USE_MODLOAD` | Result |
|---|---|---|
| 0 | 0 | both loaders are empty TUs; zero size and zero behavior change |
| 1 | 0 | `exec` only |
| 0 | 1 | `ld` / `lkup` / `modules` / `unld` only |
| 1 | 1 | both paths (the QEMU example's default board config) |

## 2. Choosing it in Kconfig

```text
config ARKRTOS_USE_MODLOAD
    bool "VxWorks-style downloadable modules (ld / lkup / modules / unld)"
    depends on ARKRTOS_USE_VFS && ARKRTOS_USE_PRIV
```

* depends on `VFS`: modules are read from the filesystem (`/mnt/ram/modhello.o`);
* depends on `PRIV`: modules run at PLV0, so the kernel-mode path itself must work;
* the board only has to give one number: `ARKRTOS_MOD_AREA_SIZE` (how big the module pool is). **The window address is not a board input** — the linker places the pool (`s_mod_pool`), so there is no "switch enabled but window macro forgotten ⇒ silent jump to address 0".

`board/qemu_loongarch64/arkrtos_board_config.h` sets both to 1 so the QEMU example exercises both paths at once; other targets decide via their own Kconfig / board config.

## 3. What the loader does

The input is a `.o`; the loader's four steps correspond to four things it must prove:

1. **Place sections**: put `SHF_ALLOC` sections, respecting alignment, into a free slot of the module pool (`ARKRTOS_MOD_SLOTS` slots, each `ARKRTOS_MOD_SLOT_SIZE`);
2. **Resolve symbols**: look up every undefined symbol **literally by name** in the `s_exports` whitelist of `kernelsrc/src/arkrtos_exports.c`, and write the address into the GOT;
3. **Relocate**: per the LoongArch64 SOP stack-machine relocation, patch the offset of every `pcalau12i` / `ld.d`;
4. **Start a task**: `module_main(argc, argv)` runs as a task entry at PLV0.

The export table has exactly 6 symbols (`lkup` shows them):

```text
arkrtos_mod_printf           the loader's own printf (%s %c %d %i %u %x %p %%; no width/precision/float)
arkrtos_console_write        write bytes to the console
arkrtos_kernel_get_tick      read the tick
arkrtos_task_delay_internal  delay (microseconds)
arkrtos_task_get_current     get the current task
arkrtos_thread_exit_internal end the current task
```

Criterion for adding a line: **a module must be able to do one legitimate thing with it, and it must not let the module bypass kernel management** (anything that touches TCBs, deletes other tasks, or writes MPU/page-table registers never enters the table). The platform-side `snprintf` is not exported either: that symbol does not exist on another board, and exporting it would be a promise that only holds in a demo.

### 3.1 Supported relocation types

The GCC 8.3 LoongArch backend emits only SOP stack-machine relocations (`-mexplicit-relocs` / `-mno-relax` are both unrecognized on this toolchain, so do not expect to dodge them with flags):

| Number | Type |
|---|---|
| 22 | `R_LARCH_SOP_PUSH_PCREL` |
| 23 | `R_LARCH_SOP_PUSH_ABSOLUTE` |
| 25 | `R_LARCH_SOP_PUSH_GPREL` |
| 29 | `R_LARCH_SOP_PUSH_PLT_PCREL` |
| 30 / 31 / 32 / 33 / 34 / 35 | `ASSERT` / `NOT` / `SUB` / `SL` / `SR` / `ADD` |
| 40 / 42 / 43 / 45 / 46 | `POP_32_S_10_12` / `_S_10_16_S2` / `_S_5_20` / `_S_0_10_10_16_S2` / `_U` |
| 1 / 2 | `R_LARCH_32` / `R_LARCH_64` |

The expression for external data access is `PCREL(_GLOBAL_OFFSET_TABLE_ + 0x800) + GPREL(sym)`, hence `gp = got_base + 0x800`. **An unrecognized relocation type is a hard error that carries the type number out** (`arkrtos_mod_bad_reloc()`); the loader never guesses an encoding.

## 4. Usage

```text
mark@arkrtos:~$ ld /mnt/ram/modhello.o a b
mark@arkrtos:~$ lkup                      list all exported symbols
mark@arkrtos:~$ lkup arkrtos_mod_printf   look up one
mark@arkrtos:~$ modules                   list resident modules (base/size/entry/relocs/imports/task/state)
mark@arkrtos:~$ unld 0                    unload (returns BUSY while the module task still runs; no hard rip-out)
```

Return codes (`arkrtos_mod_error_name()` gives names):

```text
0 OK / -1 PARAM / -2 FS / -3 FORMAT / -4 ARCH / -5 LAYOUT
-6 SYMBOL / -7 RELOC / -8 NOENTRY / -9 NOMEM / -10 BUSY
```

On `-6`, `arkrtos_mod_missing_symbol()` gives the missing name; on `-7`, `arkrtos_mod_bad_reloc()` gives the type number — **an error must point straight at the cause**, never a generic failure.

### 4.1 Interactive run in QEMU (reproducible steps)

Before going to hardware, walk `ld / lkup / modules / unld` in QEMU. The difficulty is not the commands but **how to get keyboard input in** — these two routes were measured and do not work, do not waste time:

* `-serial file:xxx.log` only captures output and **cannot feed input**;
* `-nographic` (= `-serial mon:stdio`) with piped stdin: characters echo, but **Enter has no effect** — not one command ever ran (a monitor multiplexing issue).

The only working route is **TCP chardev**: QEMU listens on a local port, the script connects and sends bytes, Enter is `\r`.

**① Build the ELF** (the module pool must contain `modhello.o`; `mod_demo.c` writes it at startup):

```bash
make -f example/qemu_loongarch64/Makefile -j
# output: build/qemu_loongarch64/ark_qemu_la64.elf
```

**② Start QEMU** (mind the `cwd`): on Windows QEMU cannot open paths containing non-ASCII (`-kernel "D:/工作/git_project/..."` yields `could not load kernel: Failed to load ELF`), so **change into the ELF directory and give only the file name**:

```bash
cd build/qemu_loongarch64
"$QEMU" -machine virt -kernel ark_qemu_la64.elf \
        -display none -monitor none \
        -serial tcp:127.0.0.1:45553,server=on,wait=off
```

Two conventions: `server=on,wait=off` starts first and waits for the client, at the cost of **losing boot output (including the first prompt) before the connection**, so the client should **send a bare Enter first to probe**; `wait=on` was measured to leave commands unable to reach the shell — do not use it.

**③ Connect and type** (`nc` or any socket client; end every line with `\r`):

```text
lkup                          ^ Enter is CR ('\r'); LF also works: ark_shell.c lines 411/472 accept both
lkup arkrtos_mod_printf
ld /mnt/ram/modhello.o alpha 42
modules
unld 0
modules
```

Expected (excerpt; kernel log `[A]/[B]` lines interleave, which is normal):

```text
mark@arkrtos:~$ lkup
kernel exports (6):
  arkrtos_mod_printf ...
mark@arkrtos:~$ ld /mnt/ram/modhello.o alpha 42
[modhello] hello from a relocatable module
[modhello] argc=3
[modhello] argv[2]=42
[MOD] modhello.o: base 0x00312320, 544 B, entry 0x00312320, 165 reloc(s), 8 import(s), task 8
mark@arkrtos:~$ unld 0
unld 0: slot released
```

**The prompt is `mark@arkrtos:~$ ` with ANSI colour** (`\x1b[1;32m...\x1b[0m`). So when using it as evidence, **strip ANSI escapes first and then look for `@arkrtos`**; a plain `grep "Ark>"` will never match.

**④ One-shot run (recommended)**:

```bash
python tools/qemu_shell_drive.py --modld-check
```

It starts QEMU itself, connects over the TCP serial port, sends Enter and waits for the prompt, then runs 7 commands in order and checks each expected substring (6 exports / single `lkup` / `modules` slot table / `ld`'s argv and `165 reloc(s), 8 import(s)` / `unld 0: slot released` / `modules` showing only the other slot after unload). On full success the last line is `SHELL-DRIVE OK: 7 条命令全部符合预期` with exit code 0.

Other switches: `--cmd "..."` (send only, no checking), `--script cmds.txt` (one per line, `#` skipped), `--timeout 3` (seconds to wait per command — **closing on time**, because the once-per-second kernel log interleaves after the prompt, so "ends with `$`" is unreliable), `--port` / `--elf` / `--qemu`.

**This is not part of `ci_gate`**: it depends on QEMU and a really running kernel, which a "one-click static check" cannot carry; it is a manual/special-purpose rerun. `tools/modld_gate.py` covers only the static half (§7).

## 5. QEMU readings (LoongArch64, 2026-11-17)

`example/qemu_loongarch64/mod_demo.c` and `elf_demo.c` are registered side by side; both paths run once at startup:

```text
[MOD] write /mnt/ram/modhello.o rc=0 size=6224
[modhello] hello from a relocatable module
[modhello] argc=3
[modhello] argv[0]=modhello.o
[modhello] argv[1]=alpha
[modhello] argv[2]=42
[modhello] tick 153 -> 168, calls=1
[modhello] bye
[MOD] ld rc=0 [base=3203872 size=544 entry=3203872 relocs=165 imports=8 task=8]
[MOD] module finished after 0 ms; try: ld /mnt/ram/modhello.o a b
```

`base == entry` (the first slot of the module area), `size=544`, `relocs=165`, `imports=8` all agree with `modhello.o`'s static readings (6224 B / `.text` 304 B / 165 relocations / 8 undefined symbols).

In the same run `[ELF] exec rc=0` / `[ELF] demo program finished` also appear as usual — **both models on at once, neither disturbing the other**, which is one of this round's acceptance criteria.

### 5.1 Signature gate (`ARKRTOS_USE_EXEC_SIGN`)

The module path is "heavier" than `exec`: it runs at **PLV0**, so a bad `.o` is not "a bad program in a sandbox" but the kernel itself. So the same switch also governs `ld`:

* after reading the file but before parsing the ELF header, verify a 32-byte tag at the end covering `[0, len-32)`; on success **shrink `len` back to the body length** before parsing further — without shrinking, a bad object that pushes `shoff + shnum*64` to the end of file would read the tag as the section header table.
* failure code `ARKRTOS_MOD_ERR_SIGN (-11)`, and `arkrtos_mod_error_name()` gives `missing or wrong signature tag (see tools/sign_payload.py)`.
* the failure happens before a slot is claimed, so a rejected object takes no module slot (no extra line in `modules`).

Measured (QEMU LA64, 2026-11-17, same boot as §5):

```text
[MOD] write /mnt/ram/modhello.o rc=0 size=6256        <- 6224 B body + 32 B tag
[MOD] ld rc=0 [base=... relocs=165 imports=8 task=8]  <- the signed version loads as usual
[MOD] tamper check PASS (bad tag refused, rc=-11)     <- last tag byte flipped, refused
```

`tools/sign_payload.py tamper` generates the counter-example alongside the positive case, both compiled into the image, and `mod_demo.c` runs it once per boot — refusal is driven by the load path itself, not verified by hand.

## 6. Three real bugs this round (all only visible on a real machine)

1. **The module pool address must not trust a macro.** The first version computed slot addresses from `ARKRTOS_MOD_AREA_BASE`, while the linker actually placed `s_mod_pool` elsewhere ⇒ `ecode=0xD (INE) era=0x00600000` (jumped into all-zero instructions). Fixed by `c.base = (uint32)c.img` and dropping the BASE macro requirement. **Whatever the linker decides, do not let a macro decide again.**
2. **`argv` must be a real pointer vector.** Casting an array of inline strings to `char**` and handing it to the module means `argv[i]` reads the string contents themselves ⇒ `ADEF badv=0x68646f6d` (`"modh"`). Fixed with a separate `s_mod_argp[slot][i]` pointer vector.
3. **The loader is heavy work on the task stack — about 4 KB per load.** The demo task stack was 512 words (2 KB) at first, while one `arkrtos_mod_load()` peaked at a measured **3080 B** (reading the ELF, parsing sections, copying segments, 165 relocations, resolving symbols one by one). Stack overflow smashed the saved frame of the task **immediately below** it, so about 1 second after the module finished there was a deterministic FATAL (`estat=0xD era=0x0032f868`, exactly the corrupted frame's `ERA` landing inside lfs data structures). Diagnostics and criteria:
   * a "passive frame watchdog" in the kernel validates the saved frames of all non-current tasks each tick, catching `[WATCH] task 4 frame became bad at tick=129 cur=5` — the damage happened **before** module code began executing, pinning responsibility on the loader rather than the module;
   * fix: demo stack raised to 2048 words (8 KB); "≥ 4 KB per load" is written into the header and this section. The shell's `ld` is the same — **give the shell task enough stack**, do not let two task stacks sit adjacent.

## 7. Gate and what is not covered

`tools/modld_gate.py` (`tools/ci_gate.py` item 38):

* A default profile `ARKRTOS_USE_MODLOAD == 0`;
* B LA64 with the switch on compiles, and **`arkrtos_mod_load` really appears in the preprocessed output** (guards against "on but empty TU");
* C **symbol reconciliation** (the core anti-false-green check): compile `mod/modhello.c` into a real `.o`, take undefined symbols with `nm -u`, and every one must be in `s_exports`;
* D guard pins: `ET_REL` detection, bounded `mod_read_word` / `mod_write_word`, default branch reporting with a type number, `#error` when `ARKRTOS_MOD_AREA_SIZE` is missing;
* E Kconfig and board wiring (`depends` on VFS and PRIV, help text saying "not a replacement");
* F **32-bit negative**: RV32 (`riscv64-unknown-elf-gcc -march=rv32imac`) and LA32 (`loongarch32-newlib-elf-gcc -march=loongarch32`) one each, force `-DARKRTOS_USE_MODLOAD=1` and it **must fail to compile**, with the error being "64-bit port only". Note both must be **fed `ARKRTOS_MOD_AREA_SIZE=65536` first** — otherwise the "missing window" `#error` fires first and this negative passes for the wrong reason.

The loader reads ELF64 RELA / symbol tables (64-bit fields, symbol index in the high 32 bits of `r_info`), which cannot be expressed with 32-bit `long`, so **it is LoongArch64-only**: on a 32-bit port the switch is stopped at compile time at the top of `arkrtos_modld.c`, not at runtime.

**Not covered**: runtime behavior (a real load, real relocation, real kernel API calls, load after `unld`, `BUSY` refusal). Those are QEMU readings, see §5. The gate only guarantees "it loads, it compiles, the export table is complete, the guards are not dismantled".
