# arkc（同步副本，勿手改）

本目录由 `tools/sync_arkc.py` 从 arkc 源仓库复制而来。
**不要改 `src/` 与 `include/` 里的代码**：改动会在下次同步时被覆盖。
改源仓库，再跑同步。组件根目录的接线文件属于 Ark RTOS 侧，同步不碰，可以直接改。

| 项 | 值 |
|---|---|
| 来源 | `D:/工作/git_project/ark c compiler` |
| 源 commit | `9fcb3e6958cd74b80450d5d63a722cf170fc49ec` |
| 工作区 | 有未提交改动 |
| 同步日期 | 2026-11-08 |
| 文件数 | 45 |

复制范围：`src/` → `src/`、`include/` → `include/`（只取 .c/.h）。

**不含** `src/main.c`：主机驱动器自己带 `main()`，板上入口是 shell 的 `cc` 命令。
**不含** `src/backend/{c,x86_64,riscv32}`：本板只有 arm 后端（`src/backend/arm` + `src/backend/registry.c`）。
**不含** `port/`：`port/stm32/platform_stm32.c` 自带 `malloc/free/putchar/__user_initial_stackheap`，与 newlib 撞符号；`port/host` 是主机端。两者的职责由组件根目录的接线文件接过来。

另外同步一份 `stdlib/include/stdarg.h` → `kernelsrc/sdk/mini_sdk/arkc/stdarg.h`：板上市小程序的头文件集里有它（变参规则只能有一份实现，手抄一定漂）。

## 四个接线文件（Ark RTOS 侧，不在同步范围）

| 文件 | 职责 |
|---|---|
| `arkc_port_arkrtos.c` | 端口层：`ark_log`（LF→CRLF，按 200 字节分块走控制台上锁）、`ark_abort`（打印后 longjmp，不挂死 shell）、`ark_get_date_time`（固定 1970-01-01）、5 个 `fs_*`（走 `arkrtos_vfs_*`） |
| `arkc_port.h` | 上面那层给命令层用的四个函数（abort 落点 / 错误文本 / arena 清零） |
| `arkc_shell_cmd.c` | `cc` 命令：解析 `-o -S -c -E -D -I -O -target`，`setjmp` 保护、每次运行前 `config_init()` + `diag_init()`、结果按返回码回报 |
| `arkc_shell_cmd.h` | `arkc_shell_register()`，与 arkvim 同一种注册钩子 |

## 构建接线（Makefile，不要改上游源码）

arkc 的 20 个源必须用独立的 `CFLAGS_ARKC` 编译，原因只有一条：
`include/platform.h` 与 `kernelsrc/shell/platform.h` **同名**。
把 arkc 的 include 放进全局 `-I`，会让 shell/arkvim 的 `#include "platform.h"` 拿到编译器那份（或反之）；所以 arkc 的源文件单独带 `-I components/arkc/include`，接线文件则用相对路径（`#include "include/platform.h"`、`#include "../../shell/platform.h"`）把两份分清。

尺寸由 `ARKC_DEFS` 收（`-DARK_POOL_SIZE=…` 等，见 Makefile），默认值在 `include/arkcc_config.h` 里都是 `#ifndef`，不需要也不应该改上游。
工作区共约 178 KB，全部落在链接脚本的 `.ark_buf (NOLOAD)` 段：
不占镜像体积、不花上电清零时间，代价是内容不确定，所以 `cc` 每次运行前调 `arkc_port_zero_arena()` 整段清零。

## 板端已知限制

- **`#include "x.h"` 不按包含者目录解析**：arkc 的 `read_include()` 只用文件名拼 `-I` 目录，因此板上头文件要么用 `-I` 指到目录，要么放在当前 cwd。主机上靠 `#include "sub/x.h"` 的相对路径写法在板上会失败——这是上游行为，不是移植缺陷，改它要改 `src/preproc.c`。
- 根文件系统是 ramfs：单文件上限 512 B、最多 8 文件 / 16 目录。源码能放，但要编译真程序（连同头文件）必须换 littlefs 卷。
- 输出默认写 `<file>.bin`；`-o -` 打到控制台，文本原样、二进制先提示再 hex，不会把二进制糊在终端上。
- `ark_abort` 已改为 longjmp 回到 `cc`，所以池耗尽（`-DARK_POOL_SIZE` 太小）表现为 `cc: aborted: <msg>` 后回到提示符，而不是板子卡死。

## 内核侧依赖

`arkrtos_vfs_app_stat/open_read/read/close/write_file/list`（`kernelsrc/include/arkrtos_vfs.h`）、`arkrtos_shell_resolve_path`（`kernelsrc/include/arkrtos_shell.h`）、`platform_uart_send_buf/console_begin/console_end/tick_ms`（`kernelsrc/shell/platform.h`）。
少了它们会在链接期报未定义，而不是在运行时出错。
