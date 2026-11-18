# Shell 命令拆分方案（已实施，2026-11-17）

**[简体中文](Shell命令拆分方案.md) | English([Shell命令拆分方案.en.md](Shell命令拆分方案.en.md))**

## 一句话

`kernelsrc/shell/arkrtos_shell.c` 现在是 **6244 行 / 37 个分节 / 111 个 static 函数**，
其中 fs 一族独占 2190 行（35.1%）。实测跨族共享的符号只有 **10 个函数 + 8 个静态变量**，
所以拆分是「机械搬移 + 一个内部头」，**不需要改任何命令逻辑**。本文给出搬法、约束与判据；
**代码一行未动**——本机没有 DAP 探针，改坏无法上板。

## 1. 现状读数

| 指标 | 读数 |
|---|---|
| 文件 | `kernelsrc/shell/arkrtos_shell.c`，**6244 行**（UTF-8 + LF，无 NUL） |
| 分节 | 37 个 `/* ==== */` 分节 |
| 文件级静态函数 | 111 个 |
| 文件级静态变量 | 8 个（`g_shell`、`g_shell_stack`、`g_ushell`、`g_ushell_out`、`g_blk_scratch`、`g_fs_scratch`、`rt_unit_c`、`rt_unit_us`） |
| 最大命令族 | `fs`：3292–5481，**2190 行** |
| 次大 | `app` 438 / `blk` 399 / `crash+audit+sched+dt` 267 / `cfg` 246 / `stack` 192 |
| 命令注册 | **一处**：`register_kernel_commands()`（约 5909 行起），上界 `need = 49 + 4 个可选特性` |

## 2. 为什么值得拆

- 一个 6244 行的文件里，读 `fs` 也要把整个文件拉进上下文；新增一条命令的默认落点永远
  是「继续往这个文件堆」。
- 但**不要按「函数长度」拆**：本文件 111 个函数里绝大多数是几十行的小函数，问题不在函数粒度，
  而在**一个文件装了几十个互不相干的命令族**。拆分的目标是「一族一文件」，不是「一函数一文件」。
- 同时**不要顺手改逻辑**：上一轮 SVC 拆分（1318 → 58 行 + 17 handler）是纯搬运，仍带来
  `Code` +408 B。所以「体积不变」不能当判据，「行为等价 + 体积变化如实记账」才是。

## 3. 目标文件划分

| 目标文件 | 搬什么（源文件行段，快照值） | 约行数 | 门控 |
|---|---|---|---|
| `arkrtos_shell.c`（留存） | 文件头/包含、小工具 97–260、注册注释 1692–1695、`register_kernel_commands()`、控制台任务 6107–6245 | ~420 | 外层 `SHELL_ENABLE` |
| `arkrtos_shell_internal.h`（新增） | 共享函数的声明（见 §4），不含任何实现 | ~60 | — |
| `arkrtos_shell_image.c` | `info` 261–361、`app` 362–799、`drv` 800–908、`install` 1593–1691 | ~750 | 无族级门控 |
| `arkrtos_shell_diag.c` | `task` 909–1000、`stack` 1001–1192、`sched/audit/guard/crash/dt` 1193–1558、`fault` 1559–1592、`log/syncinfo` 2008–2151、`rt` 2315–2480、`sec` 2481–2560、`utest` 2561–2646 | ~1160 | 各自的 `ARKRTOS_USE_*`（`STACK_USAGE` / `LOG` / `RTSTAT` / `SECCHECK` / `UTEST`） |
| `arkrtos_shell_fs.c` | `blk` 2893–3291、`fs` 3292–5481、`pwd/cd` 5482–5537、`df` 5662–5712 | ~2700 | `ARKRTOS_USE_BLK` / `USE_FS` / `USE_VFS` |
| `arkrtos_shell_cfg.c` | `pool` 2152–2314、`cfg` 2647–2892 | ~410 | `ARKRTOS_USE_MEMPOOL`（pool 段有门控） |
| `arkrtos_shell_ushell.c` | 用户命令注册表 + 内核跳板 1696–2007 | ~312 | `ARKRTOS_SHELL_USE_USER_CMD` 一类外层门控 |
| `arkrtos_shell_misc.c` | `history`、`uname`、`free`、`ps`、`which`、`man`、`dmesg`、`hwsafe` 5538–5661、5713–6106 | ~420 | `HW_SAFE` 等 |

合计与 6244 行的差额来自「分节注释与 `#if/#endif` 边界」，不是漏搬。若 `fs` 一族仍嫌大，
第二阶段可在文件内按「读路径 / 写路径 / 取证自检」再分三块，但**先做文件级拆分**。

## 4. 跨族共享符号（实测清单）

以下符号在**两个以上分节**里被引用，必须进 `arkrtos_shell_internal.h`（声明）并在唯一一个 `.c`
里保留定义（不能每族各复制一份 `static`——那会变成 N 份行为可能漂移的副本）：

| 符号 | 定义行 | 被哪些族用 |
|---|---|---|
| `sh_out` | 119 | 几乎全部（app/blk/cd/cfg/crash/df/dmesg/drv/fault/free/fs/guard/install/log/man/utest/which/控制台/ushell） |
| `parse_u32` | 125 | app/blk/drv/fs/install/log/rt/task |
| `slot_state_name` | 148 | app/drv/info/ps/utest |
| `slot_type_name` | 162 | app/crash/drv/info/ps |
| `cfg_type_text` | 216 | cfg/utest |
| `task_state_name` | 230 | ps/task |
| `cfg_recv_exact` | 2672 | cfg/fs |
| `ushell_drop` | 1787 | ushell/控制台任务 |
| `cmd_stack` | 1026 / 1181 | **同名两定义**：`#if ARKRTOS_USE_STACK_USAGE` 的真实现与 `#else` 的告示桩；必须一起搬 |
| `cmd_pool` | 2596 | pool 注册点在一处、定义在另一段，搬动时不要只看分节 |

静态变量里只有 `g_shell`、`g_shell_stack`（控制台本体）与 `g_ushell`、`g_ushell_out`（用户命令
输出缓冲）需要跨文件可见，其余（`g_blk_scratch`、`g_fs_scratch`、`rt_unit_*`）可随族走。

## 5. 三条硬约束

1. **命令注册只有一处**。`g_cmd_table` 是固定数组 `ark_shell_cmd_t g_cmd_table[ARK_SHELL_MAX_COMMANDS + 1]`
   （`ARK_SHELL_MAX_COMMANDS = 56`），容量检查在 `register_kernel_commands()` 里一次性做
   （`need = 49 + 可选特性数`）。拆文件后若变成「每族各自注册」，必须共用同一个 `g_cmd_count`、
   并各自检查剩余容量：**溢出是静默的写越界**，不会报错、不会崩溃，只会改掉相邻变量。
2. **输出出口只能有一个**。所有打印走 `sh_out` / `ark_shell_printf`；用户命令的输出还共享
   `g_ushell_out` 这一块缓冲（SVC `0x1A` 的跳板按块拷回 App）。多开一个出口就会出现
   「同一条命令在控制台和 App 里输出不一样」。
3. **门控随族走、不跨文件对半分**。外层整文件门控是 `SHELL_ENABLE`（在分区头），内层各族
   有 `ARKRTOS_USE_BLK / _FS / _VFS / _LOG / _UTEST / _SECCHECK / _RTSTAT / HW_SAFE`。
   一个族的 `#if` 头在一份文件、`#else` 桩在另一份文件，是最容易把「关掉后仍有符号可链」这条
   契约弄坏的方式。

## 6. 迁移顺序（每一步都能停）

1. 只加 `arkrtos_shell_internal.h`：把 §4 的共享符号改成「头里声明、`.c` 里定义」，
   `static` 去掉。**这一步不改任何行为**，先让全部门禁通过（写作时是 28 项）。
2. 搬 `ushell`（312 行，边界最干净）：它有自己的注册表与跳板，跨族依赖只有 `sh_out`。
3. 搬 `fs + blk`（约 2700 行，最大一块，也最容易证明「搬完命令还在」）。
4. 搬 `image`、`cfg`、`diag`、`misc`，每搬一族跑一次门禁。
5. 最后才整理 `register_kernel_commands()` 的 `need` 上界与文件头注释。

每一小步的验收：`env -u PYTHONHOME -u PYTHONPATH py -3 tools/ci_gate.py` 全部门禁通过 +
Keil 工程 0 error + `help` 的命令集合与拆分前逐字相同 + `Code` 变化如实记账。

## 7. 当时为什么先不做（本节是 2026-11-13 的判断；2026-11-17 已实施，见 §10）

- 本机没有 DAP 探针（`pyocd list` 无设备），拆完只能证明「编译通过 + 门禁通过」，
  不能证明「每条命令在板上还一样」。这条限制与 SVC 拆分时相同，但 shell 的命令面更宽
  （44 条命令），盲拆的代价更高。
- 纯搬运也会动 `Code` 布局（函数顺序、ICF、内联决策），所以「体积不变」不是可用的判据；
  必须先有板上 `help` 输出与至少每族一条只读命令的基线，才谈得上等价性。
- 因此本轮只交付这张地图；真正动手前应先拿到探针，或在板子可用时先录一份 `help` 基线。

## 8. 风险

| 风险 | 说明 |
|---|---|
| 行号会漂 | 本文行段是 6244 行快照的读数；重组时按**符号名**找，不按行号找 |
| 同名双定义 | `cmd_stack` 有两份（真实现 / 告示桩），拆时一起搬，否则会出现重复定义或「关掉开关后链接不到」 |
| 静态变量被复制 | 把 `g_ushell_out` 之类的缓冲在两族各定义一份，编译能过但行为不同 |
| 编码 | 本文件是 UTF-8 + LF（不是 GBK），拆分不会踩转码；但新文件必须保持一致，否则 `mixed-eol` / `encoding` 门禁会红 |
| 门控组合 | 全关档与全开档都要编一次：44 条命令里有十几条在 `#if` 里，漏一个 `#else` 桩就是链接错误 |

## 9. 完成判据

- 每个新文件顶部写清「本文件负责哪几族命令、受哪个门控」；
- `docs/内核Shell控制台使用说明.md` 的命令表与拆分后的文件一一对应；
- `ci_gate` 全部门禁通过；Keil 全量 0 error；
- `help` 输出的命令集合与拆分前逐字相同（排序也相同）；
- `Code / RO-data / RW-data / ZI-data` 四个数都给出，变化写进本页或 `docs/欠账盘点.md`。

## 10. 实施记录（2026-11-17 完成）

动手时的主文件已经不是 §1 那个 6244 行快照——这一年里 SVC 拆分、`sec` / `utest` /
`rt` / `pool` 等命令都加了进来，实际是 **7952 行**。所以行段按 7952 行重新切，
但原则照旧：**按符号切，不照抄本文行号**。

拆分脚本是 `build/split_shell.py`（`build/` 被忽略，不入库）。它只做三件事：
按分节行段搬运；给**跨文件真的用到**的函数去掉 `static`（族内 helper 保持文件内
可见，不把整份 shell 的名字灌进全局符号表）；把共享声明写进内部头。命令体逐字未改。

| 文件 | 行数 | 负责 |
|---|---|---|
| `arkrtos_shell.c` | 569 | 控制台实例与栈、跨族小工具（`sh_out` / `parse_u32` / 名字表）、`register_kernel_commands()`、控制台任务 |
| `arkrtos_shell_internal.h` | 205 | 内部头：包含 + feature fallback 宏 + 共享声明（67 个函数原型 + `g_shell` / `g_ushell`） |
| `arkrtos_shell_ushell.c` | 313 | 用户命令注册表 + SVC 0x1A 跳板 |
| `arkrtos_shell_image.c` | 796 | `info` / `app` / `drv` / `install` |
| `arkrtos_shell_diag.c` | 1196 | `task` / `stack` / `sched` / `audit` / `guard` / `crash` / `fault` / `log` / `syncinfo` / `rt` / `sec` / `utest` |
| `arkrtos_shell_cfg.c` | 420 | `pool` / `cfg` |
| `arkrtos_shell_fs.c` | 2730 | `blk` / `fs` / `pwd` / `cd` / `df` / 整套 VFS 命令 |
| `arkrtos_shell_misc.c` | 318 | `history` / `uname` / `free` / `ps` / `which` / `man` / `dmesg` / `hwsafe` |
| `arkrtos_shell_mod.c` | 357 | `exec` / `wait` / `ld` / `lkup` / `modules` / `unld` |
| `arkrtos_shell_pmap.c` | 1249 | `pmap` 全族 |

### 10.1 行为等价怎么证明（不是"看起来一样"）

1. **先把噪声关掉**：示例 `example/qemu_loongarch64/main.c` 的 `task_a` / `task_b`
   周期打印包了一层 `#if !defined(ARKRTOS_QEMU_DEMO_QUIET) || (ARKRTOS_QEMU_DEMO_QUIET == 0)`
   （默认未定义 ⇒ 行为零变化）。带 `-DARKRTOS_QEMU_DEMO_QUIET=1` 另建
   `build/qemu_la64_quiet`，两次运行才可能逐行一致（已实测）。
2. **37 条只读命令**跑一遍，转录逐行比较（`build/shell_regress_cmds.txt` +
   `build/shell_cmp.py`）。比较器只做四件事：去 ANSI 转义、把 `[A]`/`[B]` 计时行
   **连同换行**一起删（否则会留下 `bus in` 这类残片）、去提示符、丢空行。
3. **只掩"天然会变"的部分**：hex 一律掩掉（镜像布局一变，任务表里的入口地址就整体
   平移）；`info` / `task` / `stack` / `fault` / `syncinfo` / `trace dump` / `dmesg` /
   `guard` 的数字也掩掉。
4. **先证明判据本身有分辨力**：同一个镜像连跑两次也必须逐行相同。这一步抓到两条
   真·噪声——`task` 的状态列会在 `WAIT` / `READY` 之间翻；`stack` 的水位一次一个数
   （同一镜像两跑：task 3 报 2324 / 2180、task 5 报 3044 / 2708）。两条都按证据降级为
   "只比结构"，**不是为了让结果变绿而放宽**。

读数（拆分后 vs 拆分前基线，同一静音档）：

- `SHELL-CMP OK: 37 条命令输出逐行相同`
- `help` 的 59 条命令集合与顺序逐字相同（基线存 `build/help_baseline_cmds.txt`）

### 10.2 体积（同配置前后对照）

| | text | data | bss |
|---|---|---|---|
| 拆分前 | 367304 | 416 | 767508 |
| 拆分后 | 366744 | 440 | 767508 |

text **−560 B**、data **+24 B**、bss 不变。负号不是"优化"：跨 TU 之后内联决策与字符串
合并的边界都变了（§2 已写过，纯搬运也会动布局），所以体积只记账，不作判据。

### 10.3 门禁怎么跟着改（这一段比拆分本身重要）

有 5 处门禁原本直接在 `kernelsrc/shell/arkrtos_shell.c` 里 grep 字符串做 pin。命令一搬家，
这些 pin 就**一条也匹配不上、却仍然通过**——那是空转，不是覆盖。所以：

- 新增 `tools/shell_src.py`（`rel_list()` 给出 `kernelsrc/shell/arkrtos_shell*.c` 全清单），
  `elf_gate` / `shell_auth_gate` / `paging_gate` 的 pin 改成读**并集**；
- `rtstat` / `seccheck` / `swap` 三道"编入档"改成把**每一个** shell 源文件都编一遍，
  免得这一档编的只是个空壳；
- `elf_gate` 的 F2 pin 从 `static int cmd_wait(...)` 放宽到 `int cmd_wait(...)`
  （函数已跨文件可见，`static` 不可能还在）。判据本身没放松：仍在查 wait 按 INVALID
  判结束、从 TCB 取码、区分"没上报过码"与"码是 0"，三条变异自检照跑。

全量门禁：`44 check(s) passed, 0 failed, 0 skipped`。

### 10.4 板端读数（如实）

- 本机没有 DAP 探针：**上板验证未做**。本轮只到"QEMU 跑得起来 + 37 条命令输出逐行
  相同 + 门禁全绿"。
- `stack` 的 `watch it` 告警行不可作判据（见 §10.1 第 4 条）：三次读数里 task 5 的水位
  在 2708 / 3044 / 3092 之间浮动，最差 75%，距 90% 警戒仍有 1004 B 余量。
