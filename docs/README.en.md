# Ark RTOS documentation index (English)

This is the English index of the Ark RTOS documentation set. It mirrors
[README.md](README.md), which is the index of record.

**Reading rule that applies to every English page here:** the English pages are
condensed editions. The Chinese page for the same topic is normative and is the
one that carries the numbers, the transcripts and the judgements. An English page
adds no new claims and no new measurements; where it is silent, read the Chinese
page. Each row below links both.

## Which page to read

### Start here

| Topic | What it covers | English page | Chinese (normative) |
|---|---|---|---|
| Ark RTOS应用安装与调试指南 | The operating manual: build and debug an App/driver, install it over serial, boot flow, crash handling, troubleshooting. | [Ark RTOS应用安装与调试指南.en.md](Ark RTOS应用安装与调试指南.en.md) | [Ark RTOS应用安装与调试指南.md](Ark RTOS应用安装与调试指南.md) |
| 内核Shell控制台使用说明 | Console commands on the target: info / app / drv / task / sched / fault / install / log / pool / trace / cfg. | [内核Shell控制台使用说明.en.md](内核Shell控制台使用说明.en.md) | [内核Shell控制台使用说明.md](内核Shell控制台使用说明.md) |
| GCC构建指南 | Building the F427 example without Keil: GNU Make + arm-none-eabi-gcc, usage, the Keil mapping, three GCC-side pitfalls, verification status. | [GCC构建指南.en.md](GCC构建指南.en.md) | [GCC构建指南.md](GCC构建指南.md) |
| QEMU模拟器移植与验证 | Running the port layer on a PC with no board: QEMU 9.2.3 environment and DLL fix, RV32 single-M-mode port, LA64 single-core port, LA32 compile/link only, the SMP conclusion, trap frame and service-number capture, serial transcript, monitor telnet debugging. | [QEMU模拟器移植与验证.en.md](QEMU模拟器移植与验证.en.md) | [QEMU模拟器移植与验证.md](QEMU模拟器移植与验证.md) |

### Design and internals

| Topic | What it covers | English page | Chinese (normative) |
|---|---|---|---|
| 配置区与安装策略 | Partition and pool granularity, slots and RAM windows, the config-record format, and the two install strategies. | [配置区与安装策略.en.md](配置区与安装策略.en.md) | [配置区与安装策略.md](配置区与安装策略.md) |
| 调度器说明 | Scheduling model: bitmap + list O(1) ready set, delay chain, time slice, the three scheduling trigger points, measured cost, open items. | [调度器说明.en.md](调度器说明.en.md) | [调度器说明.md](调度器说明.md) |
| 同步原语与令牌守恒 | Why user-mode waits are register-and-poll, peek(only_kernel), the three contracts, the four failure root causes, how to read syncinfo. | [同步原语与令牌守恒.en.md](同步原语与令牌守恒.en.md) | [同步原语与令牌守恒.md](同步原语与令牌守恒.md) |
| 流缓冲与消息缓冲 | Stream vs message buffering: two semantics that must not be mixed, why the return codes avoid -4, ring area plus table-before-sleep, host and on-board evidence, SVC 0x1F. | [流缓冲与消息缓冲.en.md](流缓冲与消息缓冲.en.md) | [流缓冲与消息缓冲.md](流缓冲与消息缓冲.md) |
| 固定块内存池 | Fixed-block allocation with no heap and no fragmentation: caller-provided storage, refusal rules, SVC 0x20 subcommands, size when linked. | [固定块内存池.en.md](固定块内存池.en.md) | [固定块内存池.md](固定块内存池.md) |
| 内存保护与App越权验证 | How MPU regions isolate an App task, and how a privilege violation is actually demonstrated (APP_BAD). | [内存保护与App越权验证.en.md](内存保护与App越权验证.en.md) | [内存保护与App越权验证.md](内存保护与App越权验证.md) |
| MMU进程模型与ELF装载 | Executing an ELF file directly on an MMU platform: ELF64/ELF32 two-pass loading, the sp-to-argc entry convention, the trade-off with the PLV3 probe page, why M1 is file-is-a-program and not a process, the soft-timer stack root cause, RV32 PMP window loading, the M2 debt table. | [MMU进程模型与ELF装载.en.md](MMU进程模型与ELF装载.en.md) | [MMU进程模型与ELF装载.md](MMU进程模型与ELF装载.md) |
| Linux可执行文件兼容-系统调用ABI | Running an unmodified Linux static executable on the MMU profile: 27 of the 29 asm-generic syscalls with measured QEMU readouts, how the Ark and Linux ABIs are told apart per architecture, the switches for fork, signals, copy-on-write, shared pages, a minimal futex tier and file-backed mmap with write-back, the explicit non-support list (socket, MAP_FIXED, threads, the rest of futex), the deliberate fd-table and window-heap degradations, the 13-item syscall gate plus the 36-item fork gate, and the real bugs found on the way. | [Linux可执行文件兼容-系统调用ABI.en.md](Linux可执行文件兼容-系统调用ABI.en.md) | [Linux可执行文件兼容-系统调用ABI.md](Linux可执行文件兼容-系统调用ABI.md) |
| 动态链接与PT_INTERP | Linux dynamic linking on the MMU profile: the `ARKRTOS_USE_DYNLOAD` switch, how the kernel loads the `PT_INTERP` interpreter and lays down nine auxv entries, the in-tree `ld-ark.so` with three relocation kinds, why the fixtures are not compiled `.so` files, the QEMU readouts (two PASS runs + exit 105, broken fixture 127), the 12-item gate, the +2392 B default-off delta, and the eight things still unsupported. | [动态链接与PT_INTERP.en.md](动态链接与PT_INTERP.en.md) | [动态链接与PT_INTERP.md](动态链接与PT_INTERP.md) |
| 真实Linux二进制-需求清单与差距 | What a stock distribution LA64 binary actually asks for, measured on two real samples: the probe's exit-code semantics, the six gates (address space, interpreter, TLS, dynamic tags, relocations, dependency chain + storage), the four tiers with their cost, why writing our own `ld.so` for glibc is not a work item, the `lxstat` per-number histogram and its synthetic-traffic boundary, and the 7-item gate. The conclusion is that such binaries do not run today; what ships is the ledger and the instrument. | [真实Linux二进制-需求清单与差距.en.md](真实Linux二进制-需求清单与差距.en.md) | [真实Linux二进制-需求清单与差距.md](真实Linux二进制-需求清单与差距.md) |
| 跨架构机制矩阵 | How far last words, seamless hand-off and real-time have actually got on each class of hardware: three states per cell (reading / compile surface / refused at compile time), M4 on-board readings, RV32 and LA64 QEMU ledgers, non-ARM hand-off now implemented with QEMU readings (RV32 5/5 and LA64 3/3 swap: PASS), and why LA32 / RV64GC are still refused at compile time rather than untested. | [跨架构机制矩阵.en.md](跨架构机制矩阵.en.md) | [跨架构机制矩阵.md](跨架构机制矩阵.md) |
| SMP对称调度与TLB-shootdown-设计裁定 | Why symmetric scheduling and TLB shootdown are not implemented: the three rungs and the four boundaries the kernel actually stands on, the five costs of symmetric scheduling, the measured `invtlb` op table on this QEMU, the three prerequisites none of which holds, the R1-R5 reopening conditions and the minimal first step. | [SMP对称调度与TLB-shootdown-设计裁定.en.md](SMP对称调度与TLB-shootdown-设计裁定.en.md) | [SMP对称调度与TLB-shootdown-设计裁定.md](SMP对称调度与TLB-shootdown-设计裁定.md) |
| 验证载体分级 | Telling "compiles", "ran on QEMU" and "ran on silicon" apart: the L0/L1/L2/L3 scheme, the ledger `tools/evidence_levels.tsv` as the single source of truth, three measured reasons QEMU can never be upgraded to L3, and the minimum work to move a row onto real hardware. | [验证载体分级.en.md](验证载体分级.en.md) | [验证载体分级.md](验证载体分级.md) |
| MMU平台可下载模块-VxWorks式 | Downloadable modules the VxWorks way: on-the-fly ET_REL linking, the exported-symbol whitelist, supported relocation types, the PLV0 trade-off, why this is parallel to ELF rather than a replacement, and the 4 KB loader stack bug. | [MMU平台可下载模块-VxWorks式.en.md](MMU平台可下载模块-VxWorks式.en.md) | [MMU平台可下载模块-VxWorks式.md](MMU平台可下载模块-VxWorks式.md) |
| VFS路径命名空间 | Mount points, path resolution, root conventions, where /dev and /mnt come from. | [VFS路径命名空间.en.md](VFS路径命名空间.en.md) | [VFS路径命名空间.md](VFS路径命名空间.md) |
| 设备树 | The DTS subset and dtc_lite, the arkrtos_dt query API, generation and checking, the dt command, runtime parsing of the dtb in the image, fdt load tree swap, overlay (target-path / target=<&label> / property and node deletion), verification status. | [设备树.en.md](设备树.en.md) | [设备树.md](设备树.md) |
| 功能开关 | The three layers of compile-time switches, per-item semantics when switched off, dependency constraints, and how to prove a switch really turns the code off. | [功能开关.en.md](功能开关.en.md) | [功能开关.md](功能开关.md) |
| 低功耗与tickless | How the idle sleep length is computed, why phase settlement does not lose ticks, catching up in the tick body, limits and acceptance criteria. | [低功耗与tickless.en.md](低功耗与tickless.en.md) | [低功耗与tickless.md](低功耗与tickless.md) |
| 实时性与最坏延迟 | The DWT cycle counter, the four latency metrics, on-board readings, three boundaries, and the debugger-clears-TRCENA pitfall. | [实时性与最坏延迟.en.md](实时性与最坏延迟.en.md) | [实时性与最坏延迟.md](实时性与最坏延迟.md) |
| Kconfig配置系统 | Kconfig menus and the pure-Python toolchain, symbol name equals macro name, injection points, dependency constraints. | [Kconfig配置系统.en.md](Kconfig配置系统.en.md) | [Kconfig配置系统.md](Kconfig配置系统.md) |
| 配置头导航 | Which of the seven configuration headers to change for a given question, the include order (itself a contract), five rules, five gates. | [配置头导航.en.md](配置头导航.en.md) | [配置头导航.md](配置头导航.md) |
| Shell与日志Linux化 | Bash-shaped prompt and path normalisation, 13 Linux-style commands and 5 aliases, honesty rules (print '-' or n/a rather than inventing), the dmesg line format and the 4096 B log ring, /dev renames. | [Shell与日志Linux化.en.md](Shell与日志Linux化.en.md) | [Shell与日志Linux化.md](Shell与日志Linux化.md) |
| 硬件耦合审计 | Every point where the kernel is coupled to hardware and how each is handled, the eight-direction dependency matrix, the four-layer coupling principle, what a port has to touch. | [硬件耦合审计.en.md](硬件耦合审计.en.md) | [硬件耦合审计.md](硬件耦合审计.md) |
| 软总线接入 | ark_softbus as a vendored copy, the three board hooks, the MCU tier and measured RAM, the bus command and on-board self-check. | [软总线接入.en.md](软总线接入.en.md) | [软总线接入.md](软总线接入.md) |

### Compatibility layer, ABI and network

| Topic | What it covers | English page | Chinese (normative) |
|---|---|---|---|
| POSIX与Windows兼容层说明 | Porting C programs written for Linux/Windows: the capability table, the unsupported items and why, the startup latch, the heap, the on-board self-test | [POSIX与Windows兼容层说明.en.md](POSIX与Windows兼容层说明.en.md) | [POSIX与Windows兼容层说明.md](POSIX与Windows兼容层说明.md) |
| socket与select兼容层 | App-side sockets: the headers and their boundaries, the SVC 0x1E wire contract, blocking and timeout delivered on the App side, SOCKET_DEMO, the fd-space overlap limit | [socket与select兼容层.en.md](socket与select兼容层.en.md) | [socket与select兼容层.md](socket与select兼容层.md) |
| lwIP协议栈接入 | Why loopback came first (no PHY evidence on that board), the port layer's three rules, the kernel/pool resize that came with it, and what is verified versus not | [lwIP协议栈接入.en.md](lwIP协议栈接入.en.md) | [lwIP协议栈接入.md](lwIP协议栈接入.md) |
| SVC服务号与子命令表 | The two numbering layers: 19 service numbers (SVC immediate) and 133 subcommand numbers, the four ABI conventions, the three places to touch when adding a subcommand | [SVC服务号与子命令表.en.md](SVC服务号与子命令表.en.md) | [SVC服务号与子命令表.md](SVC服务号与子命令表.md) |
| 返回码与错误码约定 | What a negative kernel return means: 11 modules and 62 named codes, values frozen in place, the sentinels, and the existing contracts that must not be unified | [返回码与错误码约定.en.md](返回码与错误码约定.en.md) | [返回码与错误码约定.md](返回码与错误码约定.md) |

### Applications and images

| Topic | What it covers | English page | Chinese (normative) |
|---|---|---|---|
| 小程序设计 | The third user-mode application form: two-block (code/RAM) memory model, six load steps, size cap, multi-instance / crash-disable / autostart list, comparison with ELF. | [小程序设计.en.md](小程序设计.en.md) | [小程序设计.md](小程序设计.md) |
| 无缝升级例程 | SEAMLESS_V1/V2 handing control between two mini programs: two record files, three hard rules, measured figures. | [无缝升级例程.en.md](无缝升级例程.en.md) | [无缝升级例程.md](无缝升级例程.md) |
| 无缝升级-双缓冲热切换 | A/B hot swap: rewriting the exception stack frame in four steps, the arm/take two-phase API, three refusal boundaries, why the data segment is not reset. | [无缝升级-双缓冲热切换.en.md](无缝升级-双缓冲热切换.en.md) | [无缝升级-双缓冲热切换.md](无缝升级-双缓冲热切换.md) |
| 崩溃恢复与镜像版本把关 | What happens when one image keeps crashing: a cross-reset crash journal, the three-state version decision, and the explicit decision not to do signing or rollback here. | [崩溃恢复与镜像版本把关.en.md](崩溃恢复与镜像版本把关.en.md) | [崩溃恢复与镜像版本把关.md](崩溃恢复与镜像版本把关.md) |
| App崩溃硬件安全与遗言机制 | Driving hardware to a safe state when an App dies: last-words channels A/B, two-phase neutralisation, planned handover, the hwsafe command. | [App崩溃硬件安全与遗言机制.en.md](App崩溃硬件安全与遗言机制.en.md) | [App崩溃硬件安全与遗言机制.md](App崩溃硬件安全与遗言机制.md) |
| arkc板上直跑-M1实机记录 | Compiling on the board and running immediately: the entry contract (vector table), app run, four error codes, why its priority sits below the console, four anti-false-green rules. | [arkc板上直跑-M1实机记录.en.md](arkc板上直跑-M1实机记录.en.md) | [arkc板上直跑-M1实机记录.md](arkc板上直跑-M1实机记录.md) |
| arkc编译产物上板运行-实施计划 | The implementation plan behind M1: what changed, in which order, with which criteria. | [arkc编译产物上板运行-实施计划.en.md](arkc编译产物上板运行-实施计划.en.md) | [arkc编译产物上板运行-实施计划.md](arkc编译产物上板运行-实施计划.md) |
| arkc板上直跑-M2可重定位镜像 | Relocatable images that can be placed in any free pool block: the four-pass diff, the five silent-wrong-answer traps, host cross-checking, cc --pack constants, the size ledger, and the two unfinished criteria stated as unfinished. | [arkc板上直跑-M2可重定位镜像.en.md](arkc板上直跑-M2可重定位镜像.en.md) | [arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md) |
| arkc板上直跑-M3b-QSPI持久化 | Persistence on external QSPI NOR with littlefs: the AF table and per-pin mapping, the /WP and /HOLD single-line pitfalls, reading by bit-banging, the kernel defect that only shows up on littlefs, the fs put upload channel, the 24.8 s / 33.7 s speed ledger, cross-reset autostart not yet achieved. | [arkc板上直跑-M3b-QSPI持久化.en.md](arkc板上直跑-M3b-QSPI持久化.en.md) | [arkc板上直跑-M3b-QSPI持久化.md](arkc板上直跑-M3b-QSPI持久化.md) |
| arkc板上直跑-M4-hello演示 | A standard-C hello world (stdio.h + printf + int main) built with make on the board and run with mini run: the SDK's two sources and three headers, the printf argument stub, seven hard limits, two regression cases plus a printf host gate. | [arkc板上直跑-M4-hello演示.en.md](arkc板上直跑-M4-hello演示.en.md) | [arkc板上直跑-M4-hello演示.md](arkc板上直跑-M4-hello演示.md) |
| ark make-板上构建工具 | The on-board build tool: why it is not GNU make, its five keys, the three hard constraints of the four-pass packaging, how a failure stage turns into an error code, zero size when switched off. | [ark make-板上构建工具.en.md](ark make-板上构建工具.en.md) | [ark make-板上构建工具.md](ark make-板上构建工具.md) |
| arkc产物上板-错误码表与回归账本 | Which error code to read when a program will not run: the tables for the three load paths and the two numbering overlaps, five typical failure points, a six-line size ledger, the four cases in final_app.py. | [arkc产物上板-错误码表与回归账本.en.md](arkc产物上板-错误码表与回归账本.en.md) | [arkc产物上板-错误码表与回归账本.md](arkc产物上板-错误码表与回归账本.md) |
| arkvim编辑器显示约定 | On-screen layout conventions for the editor (why ':q' used to render as '::q'), five conventions with criteria, three host gates, how to read on-board rawcap evidence, and why this component may only be changed upstream. | [arkvim编辑器显示约定.en.md](arkvim编辑器显示约定.en.md) | [arkvim编辑器显示约定.md](arkvim编辑器显示约定.md) |
| 单元测试框架 | utest on the board: automatic case registration, assertion macros, the utest command, the host self-test gates, the case list. | [单元测试框架.en.md](单元测试框架.en.md) | [单元测试框架.md](单元测试框架.md) |
| Shell命令拆分方案 | How the 6244-line shell monolith was split, and how the split was verified (the read-only inventory behind it). | [Shell命令拆分方案.en.md](Shell命令拆分方案.en.md) | [Shell命令拆分方案.md](Shell命令拆分方案.md) |

### Verification, process and reference

| Topic | What it covers | English page | Chinese (normative) |
|---|---|---|---|
| 安全基线自检 | The twelve read-only checks behind sec: four verdict classes and why FINDING and SKIP must stay separate, per-check criteria, verbatim F427 readings, size when switched on. | [安全基线自检.en.md](安全基线自检.en.md) | [安全基线自检.md](安全基线自检.md) |
| 安全认证准备 | Preparing for certification: 11 asset classes and five trust boundaries, STRIDE, gap tables for IEC 62443-4-2 / 303 645 / PSA / 等保2.0 / IEC 61508, 17 gap registers, the local case matrix, the P0-P3 roadmap. | [安全认证准备.en.md](安全认证准备.en.md) | [安全认证准备.md](安全认证准备.md) |
| 发行版与版本策略 | Release and version policy: where the three version places must agree, the deliverables list, the three ABI numbers and when they move, preview/formal/1.0 criteria, the six release steps, the four things explicitly not done. | [发行版与版本策略.en.md](发行版与版本策略.en.md) | [发行版与版本策略.md](发行版与版本策略.md) |
| Release-0.0.3-preview.1 | Release notes for the 0.0.3 preview: what changed, the size ledger, reproduce commands, compatibility notes, the list of what is not verified. | [Release-0.0.3-preview.1.en.md](Release-0.0.3-preview.1.en.md) | [Release-0.0.3-preview.1.md](Release-0.0.3-preview.1.md) |
| 格式基线 | The format gate: why only four pure-byte rules are checked (no tabs, no trailing whitespace, ends with a newline, no BOM), the vendored exemptions, poisoned self-checks, and the \r\r\n case that shifted __LINE__ by 1143. | [格式基线.en.md](格式基线.en.md) | [格式基线.md](格式基线.md) |
| 远程CI与门禁裁剪 | The Gitee Go remote profile: which 17 checks are trimmed and why, why skip is not pass, the NOT VERIFIED list, and the fact that the workflow itself and its runner readings are still unverified. | [远程CI与门禁裁剪.en.md](远程CI与门禁裁剪.en.md) | [远程CI与门禁裁剪.md](远程CI与门禁裁剪.md) |
| 与Linux差距盘点 | A dimension-by-dimension gap table against Linux, with each entry classified as trade-off, debt or ecosystem, plus the debt ordering. | [与Linux差距盘点.en.md](与Linux差距盘点.en.md) | [与Linux差距盘点.md](与Linux差距盘点.md) |
| 与成熟RTOS对比 | The same comparison against FreeRTOS / RT-Thread / Zephyr: a per-dimension matrix and gap tiers. | [与成熟RTOS对比.en.md](与成熟RTOS对比.en.md) | [与成熟RTOS对比.md](与成熟RTOS对比.md) |
| 已知限制 | The single limits page: execution model, application forms, system services, reliability and security, toolchain, each item marked as trade-off or debt with evidence. | [已知限制.en.md](已知限制.en.md) | [已知限制.md](已知限制.md) |
| stm32h743-external-memory | External SDRAM/FMC on the H743: the FMCEN master gate, 32 MB read/write verification, the compiler pool and ramfs living in SDRAM. | [stm32h743-external-memory.en.md](stm32h743-external-memory.en.md) | [stm32h743-external-memory.md](stm32h743-external-memory.md) |

## Pages that exist only in Chinese

| Page | Why there is no English edition |
|---|---|
| [欠账盘点.md](欠账盘点.md) | The debt ledger is a working document: it is rewritten every time an item
is closed, and each cell carries the exact readings taken that day. Keeping a second copy in
English would double the places that can drift out of date, so the ledger stays Chinese-only |
| [api/Ark RTOS_API参考.md](api/Ark RTOS_API参考.md) | Generated from the source comments by
tools/gen_api_doc.py; the comments themselves are in Chinese |
| [SEC-G08-隔离执行-设计.md](SEC-G08-隔离执行-设计.md) | A pre-registration design note for one open item (isolated execution of App shell
callbacks), written against the code as it stands today; it is a plan, not a reading, and it will
be rewritten as the milestones land. A second English copy would drift from that churn |

## Directory layout

```
docs/
├── README.md                             # the index of record (Chinese)
├── README.en.md                          # this file (English index of the same 57 topics)
├── <topic>.md  /  <topic>.en.md          # 57 topics in both editions; 欠账盘点.md is Chinese only
├── 欠账盘点.md                            # debt ledger (Chinese only)
├── img/                                  # measured figures referenced by the root README
└── api/
    ├── Ark RTOS_API参考.md                # Markdown API reference (generated, committed)
    └── html/                            # Doxygen HTML (needs a local Doxygen install)
```

Page naming: a page keeps its Chinese file name in both editions, so a Chinese
and an English edition of the same page sit next to each other and share a prefix.
Links are by file name, and `ci_gate` fails if a page exists but is absent from
[README.md](README.md).

## Generating the API reference

```bash
# default: HTML if Doxygen is found, otherwise Markdown
python tools/gen_api_doc.py

# force Markdown (no Doxygen needed)
python tools/gen_api_doc.py --md

# both
python tools/gen_api_doc.py --both
```

The generator exists because the kernel sources mix GBK and UTF-8 comments (older
files are GBK, newer modules are UTF-8) while the Markdown is UTF-8, and Doxygen's
INPUT_ENCODING is a single global setting. It decodes every source line as UTF-8 and
falls back to GBK, copies the tree to a temporary UTF-8 copy (the repository is not
modified), generates a temporary Doxyfile from the root one, and then either calls
Doxygen or runs the built-in Markdown generator. `ci_gate` regenerates the Markdown
reference and fails if `git diff` is not empty, so the committed copy cannot go stale.

## Comment conventions for new interfaces

```c
/**
* @brief one line saying what the interface is for
* @param x what the parameter means
* @return what the return value means
* @note anything the caller must know (optional)
*/
```

Public APIs in `kernelsrc/include/arkrtos.h` are grouped with `@defgroup` so the
generated HTML is organised by module.
