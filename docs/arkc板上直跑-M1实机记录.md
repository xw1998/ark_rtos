# arkc 板上直跑 — M1 实机记录（固定窗口）

本文是 [arkc编译产物上板运行-实施计划.md](arkc编译产物上板运行-实施计划.md) 里 **M1（L1 固定窗口执行）** 的落地记录：
在 STM32H743 板上，用板上跑的 arkc（shell 里的 `cc`）编出裸镜像，再由内核的 `app run` 装进固定 RAM 窗口、以非特权任务执行。

日期：2026-11-05。所有读数都来自真机串口（COM3 / 115200），没有一条是推断。

---

## 1. 一句话结论

`cc --scatter <sct> --vector-table -o /tmp/p.bin` → `app run /tmp/p.bin` → 串口出现**源码里独有的两行**：

```
BRD-M1-R1 alive
BRD-M1-R1 tag alpha
```

程序自己 `svc 0x1B` 退出后窗口归还（`task` 从 5 项回到 4 项、`fault` 为空、`free` 里池占用回到 0），**再跑一次仍正常**。

---

## 2. 入口契约（M1 定稿，与计划 §4.1 的早期写法不同）

计划 §4.1 当时设想「crt0 是第一个链接输入 ⇒ 入口偏移 0」。实测后改为 **向量表契约**，理由和证据都在下面。

- arkc 的 `-o` 单文件产物在同一份源码里会把 `.rodata`（字符串）**排在所有函数之前**，所以「文件首字节 = 第一条指令」这个前提不成立。
- 定义 `Reset_Handler` 时 `arm_link()` 会自动生成 256 项向量表（0x400 B）：`+0 = 初始 SP`、`+4 = Reset_Handler | Thumb`。这正是 Cortex-M 的装载约定。
- 早先的实现是「整段 code 右移 0x400 再调符号偏移」，这会让汇编期已烙进去的绝对地址**偏低 0x400**（带字符串的程序必乱码）。已改为 **预留式**：汇编器直接从 `ARM_VT_SIZE` 起发射、buffer 前 0x400 预清零，链接阶段**完全不搬移**。CLI 开关 `--vector-table`；默认路径逐字节不变（宿主实测同源码同 `.sct` 带/不带都是 3114 B，`BRD-M1` 在 `0x400`）。

所以内核侧校验的是**镜像自带的向量表**，不是「入口在文件第几个字节」：

| 检查 | 不满足时的错误码 |
|---|---|
| 文件 ≥ 8 B，`+0` 正好是窗口顶（`APPRUN_RAM_BASE + APPRUN_RAM_SIZE`） | `-23 APPRUN_ERR_LAYOUT` |
| `+4` 的入口落在代码窗口内 | 同上 |
| 入口那一条指令是 arkc 的序言 `0xB5E0`（`PUSH {r5,r6,r7,lr}`） | `-22 APPRUN_ERR_PROLOGUE` |

对不上就**拒绝**：跑一份为别的布局链出来的镜像，等于执行任意字节。

---

## 3. 窗口与借块

- `APPRUN_BASE = SLOT_RAM_BASE = 0x24070000`，代码窗口 8 KB，数据/栈窗口 8 KB（`0x24072000`），窗口顶 `0x24074000`（= 初始 SP），栈取数据窗口末尾 4 KB。
- 借块走**小程序那套 RAM 池首适配**（`arkrtos_ptable_mini_alloc`），**但借完必须逐字段核对**：拿到的那一块要与窗口常量完全相同，否则当场归还并报 `-21 APPRUN_ERR_WINDOW`。首适配一旦给出别的地方，就说明池里在这个尺寸上有更靠前的空块——那样跑就等于让它跳进虚空，所以只拒绝、不迁就。
- **不能用 `arkrtos_ptable_alloc_raw()`**：它核的是 **Flash** 的镜像池（`CONFIG_BASE + CONFIG_SIZE`），不是这块 RAM 窗口，用它登记 RAM 区间必然失败（实测 `rc=-21`）。

---

## 4. 两个必须记下来的坑

### 4.1 镜像在 VFS 的 ramfs 上，不在 littlefs 卷上

Ark RTOS 的文件系统分两层：`arkrtos_fs_*`（littlefs，卷要 `fs mount`）与 `arkrtos_vfs_*`（`/` 易失 ramfs、`/dev`、`/mnt/<name>`）。
板上 `cc` 的产物落在 `/tmp/p.bin`，那是 **VFS 的 ramfs**。用 `arkrtos_fs_*` 去读只会得到「没有文件系统挂载」这个**错答案**。

- 读文件改 `arkrtos_vfs_open_read/read/close` 流式读；「缓冲区装不下」用「读满后再探一个字节」判，**超窗口不截断**。
- 前置条件从「littlefs 已挂」改为 **`arkrtos_vfs_ready()`**；shell 的提示文案同步改（不再把人引去 `fs mount`）。
- 另一条硬约束：ramfs 只有 **3 个文件槽**（`ARK_RAMFS_MAX_FILES=3`，每文件 16 KB 静态缓冲）。`/tmp/p.c` + `/tmp/p.sct` + `/tmp/p.bin` 正好三个，测试脚本必须先 `rm` 再写。

### 4.2 `--scatter` 只进了 `opts`、没进 `g_config`（会静默给出错答案）

板的 `cc` 把 `--scatter` 填进 `ArkOptions::scatter_file`，而 ARM 后端只读 `g_config.scatter_file`。结果：**编译成功、`.sct` 被忽略、镜像按内置的 `0x08000000 / 0x20020000` 链**，串口抓回板上产物看到的就是 `SP=0x20020000 / entry=0x08000429`（字符串在 `0x400`，说明 `--vector-table` 那次是对的）。

修法（在编译器内部，不在每个嵌入方各修一遍）：

1. `ark_compile()` 里把 `opts.scatter_file` 桥接进 `g_config`（仅当 `g_config` 为空）；
2. 后端在 codegen 阶段解析 `.sct` 失败时**不再回落**（原实现是 `if (...) { 用 scatter } else { 用默认 }`，静默）；
3. `ark_compile()` 在 `emit_asm` 前后比 `diag_errors()`：**codegen 阶段报过错就拒绝出镜像**，并补一条
   `the requested layout in <sct> could not be applied; refusing to fall back to the built-in layout 0x08000000 / 0x20020000`。

宿主三条实测（同一份源码）：

| 命令 | 退出码 | 产物 |
|---|---|---|
| `--scatter b1.sct --vector-table` | 0 | 3114 B，VT `24074000 / 24070429` |
| `--scatter nope.sct --vector-table` | 1 | **不出镜像**，两条报错 |
| 不带 `--scatter` | 0 | 与改动前的 `arkcc.exe` 逐字节相同（md5 `59167791…`） |

---

## 5. 优先级：直跑程序放在控制台**之下**

`ARKRTOS_APPRUN_PRIORITY = SHELL_TASK_PRIORITY + 6`（20），`ARKRTOS_APPRUN_PERIOD_MS = 0`。

这不是调参，是实测教训：最初沿用 `APP_TASK_PRIORITY`（10，高于 shell 的 14），一个 `for(;;){}` 的程序**把 shell 永久饿死**——提示符再也没有回来，连 `task`/`fault` 都查不了。放到 20 之后：

- shell 空闲（等串口）时 app 独占 CPU；
- 人一敲键盘，shell（14）立刻拿到自己那一段，`task`/`fault` 照常可用；
- 周期填 0：直跑程序没有周期契约，表里显示 1000 会让人误以为它是周期任务。

---

## 6. 上板读数

### 6.1 正常路径（3006 B 的程序，跑两次）

```
cc: scatter /tmp/p.sct
cc: vector table in the first 0x400 B
[scatter] parsed /tmp/p.sct
cc: done in 42 ms
...
APPRUN: /tmp/p.bin: 3006 B -> code 0x24070000 (8192 B), data/stack 0x24072000 (8192 B), task 4
BRD-M1-R1 alive
BRD-M1-R1 tag alpha
TASK: task 4 exits (self kill)
APPRUN: task 4 exited, window returned to the pool
app run: /tmp/p.bin started (task 4)
task: 4 used / 48 max，第 4 项 prio 20 / period 0 / INVALID / entry 0x24070429
fault: no fault recorded
free: pool(image) 1310720 total / 0 used
```

第二次 `app run` 同样的输出（task 号仍是 4，窗口回收干净）。

> 「started (task 4)」这句里任务号是**起跑那一刻**带出来的：app 可能比 shell 的打印更早退出，回读 `arkrtos_apprun_task_id()` 只能读到 0（曾出现「started (task 0)」这种自相矛盾的话），所以走 `arkrtos_apprun_run_ex(path, &task)`。

### 6.2 反假绿

| # | 做法 | 期望 | 实测 |
|---|---|---|---|
| ① | 换源码字符串（`BRD-M2 hot loop`）重编重跑 | 串口只有新串 | 只出现 `BRD-M2 hot loop` |
| ② | 程序 `for (;;) { }` 不退出 | 真在跑：`task` 里 `READY`；第二次 `app run` 报 BUSY | `4 20 READY 0`；`rc=-20 - a program is already running` |
| ③ | `.sct` 改成 `0x24040000 / 0x24043000` 重新链 | 明确报窗口不符，**不能**恰好跑通 | `rc=-23 - image was not linked for this window` + VT 约束说明；`task` 回到 3 项 |
| ④ | `USE_APPRUN=0` 的镜像 | `app` 与 `help` 里**没有** `run` | usage 与 help 都只有 `list/install/start/stop/uninstall` |

---

## 7. 体积账（开关纪律）

| 档 | FLASH | 说明 |
|---|---|---|
| HEAD（干净副本，`USE_APPRUN=0`） | 381512 B | 基线 |
| 本轮 `USE_APPRUN=0` | 381808 B | **+296 B** |
| 本轮 `USE_APPRUN=1` | 385760 B | 多 `arkrtos_apprun.o` 与 shell 的 `run` 分支 |

+296 B 已按 `nm -S` 逐符号定性，**全部落在 vendored 的 arkc 里**：`ark_compile +104`、`arm_assemble +48`、`arm_link +20`、`pass2_a.constprop.0 → pass2_a`（12900，同尺寸）、`CSWTCH +20`、新增 `g_arm_vt_reserve`（4）。
**内核与应用的符号尺寸零变化**；编译器默认路径的产物与改动前逐字节相同（§4.2 表）。所以「0 档行为不变」这条仍然成立，但**不能再写成「与 HEAD 逐字节相同」**——体积确实涨了 296 B，涨在该编译器自身。

`USE_APPRUN=0` 时 `arkrtos_apprun.c` 不进编译（`sources.mk` 按 `USE_APPRUN` 过滤），shell 的 `run` 分支在 `#if` 里。

---

## 8. M1 的已知边界（不修，属于下一阶段）

| 边界 | 现状 | 归谁 |
|---|---|---|
| 同一时刻只允许一个直跑程序 | 第二次 `app run` 报 `-20 BUSY`，不排队 | 设计如此（M1 窄入口） |
| 不做重定位 | 镜像必须按窗口地址链（`.sct` 写死 `0x24070000`） | M2（L2 可重定位，走既有 mini） |
| 无持久化 | 镜像在 VFS 的 ramfs 上，重启即失效 | M3 |
| 只有 3 个文件槽 | 先 `rm` 再写；`/tmp/x.c` + `/tmp/x.sct` + `/tmp/x.bin` 正好三个 | M3 之后 |
| 不支持多链接输入 | `cc` 单文件即可（`-link` 已接线但 M1 不用） | M2 |

---

## 9. 相关代码位置

| 文件 | 内容 |
|---|---|
| `kernelsrc/src/arkrtos_apprun.c` / `include/arkrtos_apprun.h` | 借窗、读文件、向量表校验、起任务、退出回收；错误码 `-20/-21/-22/-23` |
| `kernelsrc/shell/arkrtos_shell.c` | `app run <path>` 子命令与各错误码对应的说明 |
| `kernelsrc/src/arkrtos_task.c` | 任务退出钩子调 `arkrtos_apprun_on_task_exit()`（开关内） |
| `kernelsrc/components/arkc/` | vendored arkc（`sync_arkc.py` 同步）；`arkc_shell_cmd.c` 解析 `--scatter` / `--vector-table` / `-link` |
| `config/stm32h743/arkrtos_partition.h` | 十二节：窗口常量 + 编译期自检；优先级/周期的理由 |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | `USE_APPRUN ?= 0` |
