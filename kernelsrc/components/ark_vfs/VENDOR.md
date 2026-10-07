# ark_vfs（同步副本，勿手改）

本目录由 `tools/sync_ark_vfs.py` 从 ark_vfs 源仓库复制而来。
**不要在这里直接改代码**：改动会在下次同步时被覆盖。改源仓库，再跑同步。

| 项 | 值 |
|---|---|
| 来源 | `D:/工作/git_project/ark_vfs` |
| 源 commit | `11a143fc9a9f1123f20763e45e8332fe64fa15fd` |
| 工作区 | 干净 |
| 同步日期 | 2026-10-07 |
| 文件数 | 11 |

复制范围：`include/` → `include/`、`src/` → `src/`、
`ports/arkrtos/` → `port/`。
**不含** `tests/`：其中的 mock 头与真内核头同名，进构建路径会把真头挡掉。

## 内核侧依赖

端口用到 `arkrtos_dev_name_at()`（kernelsrc/include/arkrtos_dev.h）。
少了它会在链接期报未定义，而不是在运行时出错。
