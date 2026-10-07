# ark_vfs（同步副本，勿手改）

本目录由 `tools/sync_ark_vfs.py` 从 ark_vfs 源仓库复制而来。
**不要在这里直接改代码**：改动会在下次同步时被覆盖。改源仓库，再跑同步。

| 项 | 值 |
|---|---|
| 来源 | `D:/工作/git_project/ark_vfs` |
| 源 commit | `ed8da8dfaa5d34db122fad7b8bdec48f49857552` |
| 工作区 | 干净 |
| 同步日期 | 2026-09-23 |
| 文件数 | 10 |

复制范围：`include/` → `include/`、`src/` → `src/`、
`ports/arkrtos/` → `port/`。
**不含** `tests/`：其中的 mock 头与真内核头同名，进构建路径会把真头挡掉。

## 内核侧依赖

端口用到 `arkrtos_dev_name_at()`（kernelsrc/include/arkrtos_dev.h）。
少了它会在链接期报未定义，而不是在运行时出错。

## 本地适配说明（2026-10-07）

Ark RTOS 的 `port/` 当前**不是**从上游 `ports/arkrtos/` 同步来的：上游 ark_vfs
仓库此刻只有 `ports/svcrtos/`，没有 `ports/arkrtos/`。本目录的
`ark_vfs_port_arkrtos.{c,h}` 是把 `ports/svcrtos/` 的端口改前缀（`svcrt_` →
`arkrtos_`）后落盘的，因此相对上游已经偏离。

后果：在上游补齐 `ports/arkrtos/` 之前，`tools/sync_ark_vfs.py` 的 port 部分
会因源目录不存在而失败（`include/`、`src/` 两段不受影响）。
补齐上游目录后再跑同步即可恢复一致。

