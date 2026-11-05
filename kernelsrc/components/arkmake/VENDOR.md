# arkmake（同步副本，勿手改）

本目录由 `tools/sync_arkmake.py` 从 arkmake 源仓库复制而来。
**不要改 `src/` 与 `include/` 里的代码**：改动会在下次同步时被覆盖。
改源仓库，再跑同步。组件根目录的接线文件属于 Ark RTOS 侧，同步不碰，可以直接改。

| 项 | 值 |
|---|---|
| 来源 | `D:/工作/git_project/arkmake` |
| 源 commit | `f3b2b9a901b4fc20190bbfbfd2336c26994d1cf7` |
| 工作区 | 干净 |
| 同步日期 | 2026-11-05 |
| 文件数 | 3 |

复制范围：`src/` → `src/`、`include/` → `include/`（只取 .c/.h）。
**不含** `tests/`：板上不要测试驱动；判据在源仓库那一侧跑。

## 四个接线文件（Ark RTOS 侧，不在同步范围）

| 文件 | 职责 |
|---|---|
| `arkmake_port_arkrtos.c` | 三个回调：读（`fs_read_file`）、日志（`ark_shell_printf`）、打包（`arkc_cc_pack`） |
| `arkmake_port.h` | `arkmake_ctx_t{verbose}` 与 `arkmake_port_ops()` / `arkmake_port_read()` |
| `arkmake_shell_cmd.c` | `make [<makefile>] [-f f] [-o out] [-v]`：先解析命令行（写错立刻报），再读+解析工程，最后交给 `arkmake_build()` |
| `arkmake_shell_cmd.h` | `arkmake_shell_register()`，与 arkvim/arkc 同一种注册钩子 |

## 为什么没有第五条打包通路

`arkmake` 不自己打镜像：`ops->pack` 直接调 `arkc_cc_pack()`，也就是 `cc --pack` 的本体。两条路迟早会打出不一样的镜像，而只有一条被上板跑过。
代价是 `ARKRTOS_USE_ARKMAKE=1` 必须同时开 `ARKRTOS_USE_ARKC` 与 `ARKRTOS_USE_APPPACK`——这条依赖在 `arkrtos_features.h` 里写成 `#error`，不是运行期的一句"失败了"。

## 板端已知限制

- **永远全量构建**：vfs 卷不保证可靠的 mtime，用时间戳判断要不要重编会得出随机结论。代价是两源一次约 33 s（四遍 × 两份源 + 四次链接）。
- **对象不可复用**：每一遍的 `.sct` 不同，arkc 在 codegen 期就把基址烤进指令，所以 `-c` 产物只能在同一份 `.sct` 下重编，不能跨遍复用。
- **SRCS 上限 4**（`ARKMAKE_MAX_SRCS`，与 `CC_PACK_MAX_SRCS` 对齐）：超了在解析层就报 `too many entries`，不是等四遍跑完才失败。
- `-o` 覆盖工程里的 `OUT`；两者都空时产物名是 `<TARGET>.svcm`（当前目录）。
- 优化档固定 -O1，与 `cc --pack` 缺省一致：四遍必须同档，否则差分会把优化差异当成地址位移补掉。

## 内核侧依赖

`fs_read_file` / `fs_last_error`（`components/arkc/include/flash_fs.h`）、`arkc_cc_pack`（`components/arkc/arkc_shell_cmd.h`）、`ark_shell_printf`（`kernelsrc/shell/ark_shell/ark_shell.h`）。
少了它们会在链接期报未定义，而不是在运行时出错。
