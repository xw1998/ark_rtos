# arkvim（同步副本，勿手改）

本目录由 `tools/sync_arkvim.py` 从 arkvim 源仓库复制而来。
**不要改 `src/` 里的代码**：改动会在下次同步时被覆盖。改源仓库，再跑同步。
组件根目录的接线文件属于 Ark RTOS 侧，同步不碰，可以直接改。

| 项 | 值 |
|---|---|
| 来源 | `D:/工作/git_project/arkvim` |
| 源 commit | `e0058b6177bf67ffd789ad6207774bf07f550dd6` |
| 工作区 | 干净 |
| 同步日期 | 2026-11-04 |
| 文件数 | 21 |

复制范围：`src/` → `src/`（只取 .c/.h）。
**不含** `arkvim_fileio_posix.c`：主机端 POSIX 后端，和
`arkvim_fileio_vfs.c` 实现同一组后端钩子，同进构建路径会撞符号。

## 三个接线文件（Ark RTOS 侧，不在同步范围）

| 文件 | 职责 |
|---|---|
| `arkvim_port_arkrtos.c` | 端口薄包：字节收发/延时打到 `ark_shell` 的 `platform.h`，控制台锁与 `>` 重定向捕获都在那一层，绕过去会在重定向下丢字符 |
| `arkvim_fileio_vfs.c/.h` | 文件后端：装载/保存/`cat`/`ls` 走 `arkrtos_vfs_*`，路径解析统一用 `arkrtos_shell_resolve_path()`（一个 shell 只该有一个 cwd 语义） |
| `arkvim_shell_cmd.c/.h` | `vim <file>` 命令与命令表注册钩子 |

## 尺寸裁剪（不要改上游源码）

上游每个上限都是 `#ifndef`，尺寸由内核工程 Makefile 的 `ARKVIM_DEFS` 决定：
`ARKVIM_GB_MAX_SIZE=8192` / `ARKVIM_LT_MAX_LINES=512` / `ARKVIM_UNDO_MAX_DEPTH=32`。
`sizeof(arkvim_editor_t)` 在默认尺寸下是 31148 B（16K 文本 + 4100 行表 + 9736 撤销栈），8K 文本后仍占十几 KB，RAM 只有 440KB，所以不能按上游默认开。
`ARKVIM_GB_MAX_SIZE` 同时是文件后端拒绝超大文件的判据，二者永不脱节。

## 板端已知限制

根文件系统是 ramfs：单文件上限 512 B、最多 8 个文件 / 16 个目录（`ark_vfs_ramfs.c`）。正式配置 8K 缓冲比这大得多，所以 "file too big" 分支在板端几乎打不到，要用 `-DARKVIM_GB_MAX_SIZE=256` 的临时构建才能验证。

## 内核侧依赖

`arkrtos_shell_resolve_path()`（kernelsrc/include/arkrtos_shell.h）、
`arkrtos_vfs_app_stat/open_read/read/close/write_file/list`（kernelsrc/include/arkrtos_vfs.h）。
少了它们会在链接期报未定义，而不是在运行时出错。
