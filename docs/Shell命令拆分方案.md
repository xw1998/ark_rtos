# Shell 命令拆分方案（只读盘点，未实施）

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

## 7. 为什么现在不做

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
