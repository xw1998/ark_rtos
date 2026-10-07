# ark_softbus（同步副本，勿手改）

本目录由 `tools/sync_ark_softbus.py` 从 ark_softbus 源仓库复制而来。
**不要在这里直接改代码**：改动会在下次同步时被覆盖。改源仓库，再跑同步。

| 项 | 值 |
|---|---|
| 来源 | `D:/工作/git_project/ark_softbus` |
| 源 commit | `915c4ac1a184c99144866fa29914f307f71005df` |
| 工作区 | 有未提交改动 |
| 同步日期 | 2026-10-07 |
| 文件数 | 32 |

复制范围：`include/` → `include/`、`src/` → `src/`、
`ports/arkrtos/` → `port/`。
**不含** `tests/`：其中的 mock 头与真内核头同名，进构建路径会把真头挡掉；
`tools/`、`sim/`、`mcp/`、`examples/`、`platform/` 是主机侧与多平台构建，
不属于内核组件。`src/transport/transport_sock.c` 同步进来但内核工程不编译它
（需要 POSIX socket）。

## 内核侧依赖

端口三个钩子由 `board/stm32f427/arkrtos_bus_port.c` 提供：
`ark_bus_port_tick_ms()` / `ark_bus_port_lock()` / `ark_bus_port_unlock()`，
分别打到 `arkrtos_kernel_get_tick()` 与 `arkrtos_sched_lock/unlock()`。
大小档位由内核工程的 `-DARK_BUS_*` 决定，见 `kernelsrc/include/arkrtos_softbus.h`。
