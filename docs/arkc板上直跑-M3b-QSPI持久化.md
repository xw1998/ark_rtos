# arkc 板上直跑 M3-b：QSPI NOR 持久化

**状态：闭环成立（2026-11-05 实机）。** 板上 `cc --pack` 的产物落在外部 NOR 上，
**复位后文件仍在，且能被 `mini run` 装载执行**；代价是慢（打包 24.8 s、装载 33.7 s），
原因与提速方向写在 §8。

本文只写这一轮真机上量到的读数与踩到的坑，不重复 M1/M2 的装载口径
（见 [arkc板上直跑-M1实机记录.md](arkc板上直跑-M1实机记录.md)、
[arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)）。

---

## 1. 这一轮回答的问题

| 问题 | 读数 | 结论 |
|---|---|---|
| 外部 NOR 认不认得出、容量多少 | JEDEC `0xEF4017`（W25Q64）、SR1 `0x00`、8 MiB 映射到 `0x90000000` | 认得出 |
| 能不能读、写、擦 | `nor verify` 四步：擦除 → 全 `0xFF` → 写 256 B 斜坡 → 逐字节比对 256/256 | 能 |
| 卷能不能建起来并跨挂载保持内容 | `fs format` → `fs mount` → `fs test: PASS`（写 512 B → 读回 → unmount → remount → 读回 → 删） | 能 |
| 卷能不能出现在 VFS 树里 | `mount /mnt/nor` → `vfs:451 volume nor0 mounted at /mnt/nor` | 能 |
| 板上的 `cc --pack` 能不能把产物写进 NOR | `cc --pack /mnt/nor/k.c -o /mnt/nor/k.svcm` → `payload 2940 + table 6 x 4 = 3220 B`，`ls` 见 3220 B | 能（24811 ms） |
| **复位后产物还在不在、还能不能跑** | 复位 → `fs mount` → `mount /mnt/nor` → `ls -l` 见 `k.c 447` + `k.svcm 3220` → `mini run /mnt/nor/k.svcm` → 出 `NOR: app alive` / `NOR: ctor ok`，任务自行退出、两块归还 | **成立** |

串口原文（这一轮的验收日志）：

```
[    0.000] <6>NOR:1885 W25Q64 answered: JEDEC 0xEF4017, SR1 0x00, 8 MiB at 0x90000000 (mm)
...
>>> fs mount
mounted nor0 at 0x0, 4194304 bytes
>>> mount /mnt/nor
[    6.913] <6>vfs:451 volume nor0 mounted at /mnt/nor
>>> ls -l /mnt/nor
  -rw-r--r--   1 0     0          447  k.c
  -rw-r--r--   1 0     0         3220  k.svcm
  2 entries
>>> mini run /mnt/nor/k.svcm
---- load+run took 33.7 s ----
[   47.038] <6>MINI:579 /mnt/nor/k.svcm: 2940 B payload -> code 0x24070000 (4096 B block),
           RW/stack 0x24072000 (8192 B), task 4
NOR: app alive
NOR: ctor ok
[   47.039] <6>TASK:2584 task 4 exits (self kill)
[   47.039] <6>MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
```

**判据怎么算成立**：`k.c` 是宿主侧写好后用 `fs put` 传进去、`k.svcm` 是**板上**编译器
写出来的；两者都在复位后仍然存在；`.svcm` 跑出来的两行字只有那份源码里有
（`NOR: app alive` / `NOR: ctor ok`），不是装载器自己打印的。

---

## 2. 引脚与 AF：权威表在 CubeMX 器件库里

原理图给的是网络标号（`QSPI_IO0..3` / `CLK` / `NCS`），**不含 AF 编号**。这一轮
不用猜：本机 STM32CubeIDE 的器件库里就有逐脚的可能复用表。

- 来源：`D:\ST\STM32CubeIDE_1.16.1\STM32CubeIDE\plugins\com.st.stm32cube.common.mx_6.12.1.202409122256\db\mcu\IP\GPIO-STM32H747_gpio_v1_0_Modes.xml`
  （H743 与 H747 共用同一份 GPIO IP）
- 解析方式：按 `<GPIO_Pin PortName="PF" Name="PF6">` 分节，取 `<PinSignal Name="...">`
  之后的 `<PossibleValue>GPIO_AFn_xxx</PossibleValue>`

实测映射（QSPI1 / W25Q64）：

| 引脚 | 信号 | AF |
|---|---|---|
| PF6 | QUADSPI_BK1_IO3 | **9** |
| PF7 | QUADSPI_BK1_IO2 | **9** |
| PF8 | QUADSPI_BK1_IO0 | **10** |
| PF9 | QUADSPI_BK1_IO1 | **10** |
| PF10 | QUADSPI_CLK | **9** |
| PG6 | QUADSPI_BK1_NCS | **10** |

> 这一组里的 8/9 两个脚是 **AF10**，不是 AF9。当初「整组按 AF9 配」时读回
> 全 `0x00`；同一份器件库还给出 PF8 AF9 = TIM13_CH1、PF9 AF9 = TIM14_CH1，
> 也就是说那两根本来就没接到 QUADSPI 上。这是本轮唯一一处「配置错了但现象
> 很像硬件坏」的地方。

---

## 3. 数据面的取舍：读也走位敲，QUADSPI 只留作取证

QUADSPI 的间接读在这块板上表现为：

1. 整块复位后的**第一条带地址命令会被吞**（`fifo empty SR=0`，命令根本没发出去）；
2. 一旦某次写路径失败，外设会被留在 FIFO 有残留字节的状态（实测 `SR=0x124`，
   `FLEVEL=1`），此后 `nor id` / `nor read` 连续返回 `0xFF`，SR1 读成 `0xFF`
   （MISO 等于悬空），只有整块复位才能恢复；
3. 写路径的完成位落在 `SR` 的 bit2，而 CMSIS 头里的 `QUADSPI_SR_TCF` 是 bit1
   （RM 的命名与头文件不一致），照头文件等会超时报 `wr timeout instr=20 SR=0x4`。

于是这一轮把**数据面整体换成位敲（bit-bang）**：读 `0x03`、页编程 `0x02`、
扇区擦除 `0x20`、状态 `0x05`、`WREN 0x06`，每一步都能单独回读验证。
QUADSPI 只保留为取证入口（`nor id|sr|read|mm|regs|pins|af|raw|clk`）。

代价写在明处：位敲每个半时钟 `nor_delay_us(1)` ⇒ 约 **500 kHz / 62 KB/s**。
3 KB 级的 `.svcm` 读一次几十毫秒，但 littlefs 每次 open 都会重读元数据，
装载总耗时因此是秒级（见 §8）。

---

## 4. 只在真机上才现形的坑

| 坑 | 现象 | 根因 | 修法 |
|---|---|---|---|
| **单线模式下 /WP 与 /HOLD 必须拉高** | `WREN` 之后 `SR1` 的 WEL 永远不置位，擦除一字不动 | 单线（非 quad）模式下 IO2 = **/WP**、IO3 = **/HOLD**；由 QUADSPI 驱动时它们被拉低，芯片进入写保护 | 这两个脚在写/擦期间配成 **GPIO 推挽输出高**，写/擦走位敲 |
| **`ADSIZE` 曾写成 3** | 地址按 4 字节发，命令整体错位 | `ADSIZE` 编码是 0=8 位 / 1=16 位 / 2=24 位 / 3=32 位 | 固定 `2`（24 位） |
| **新建文件被判成 I/O 错误** | `cc --pack ... -o /mnt/nor/k.svcm` 报 `cannot write /mnt/nor/k.svcm: I/O error`，日志 `ark_vfs:101 open/stat failed on /k.svcm: last_error=-2 (LFS_ERR_NOENT)`；而同一份产物写到 `/tmp`（ramfs）一切正常 | VFS 的 littlefs 桥在 `open` 前先 `stat`，把「条目不存在」也当成真错：判据写的是 `arkrtos_fs_last_error() != 0`，而门面在「没有这个条目」时恰恰会把 `LFS_ERR_NOENT` 记进 `last_error` | 门面新增 `arkrtos_fs_last_is_noent()`（1 = 上一次失败只是因为条目不存在），桥上改成 **只有它不是 NOENT 才按 IO 报**；NOENT 继续走「新建」分支 |

第三条是本轮唯一一处**内核缺陷**（不是配置问题）：它只在「往 littlefs 卷上**新建**
文件」时才现形，ramfs 上永远看不见，所以 M2 那批用例全绿也照样漏过去。

对应改动（三个文件，都是必要面）：

```
kernelsrc/include/arkrtos_fs.h      + uint8 arkrtos_fs_last_is_noent(void);   （声明 + 契约）
kernelsrc/src/arkrtos_fs.c          + 实现（真档 == LFS_ERR_NOENT；ARKRTOS_USE_FS=0 的桩返回 0）
kernelsrc/src/arkrtos_vfs_lfs.c     判据由 last_error != 0 改为 last_is_noent() == 0
```

`arkrtos_fs.c` 是 GBK 时代留下的 CRLF 文件，这次是**字节级补丁**（CRLF 计数
1269/1269 前后一致）；`arkrtos_fs.h` 与 `arkrtos_vfs_lfs.c` 本来就是 LF/UTF-8。

---

## 5. 块设备与卷

- 驱动：`board/stm32h743/drvqspi.c`（新），注册块设备 **`nor0`**，`erase_unit` 4096、容量 8 MiB；
- **只在 JEDEC 应答后才注册**（`blk_nor_bringup` 读 ID 失败就返回错误），避免
  「声称有 8 MiB，然后每次访问都失败」；
- 分区头 `config/stm32h743/arkrtos_partition.h` 里 `ARKRTOS_NOR_CHIP_SIZE` 由
  `16 MiB` 改成 **`8 MiB`**（原值是 F427 板抄来的，与实物 W25Q64 不符）；
- 卷：`ARKRTOS_FS_DEV_NAME "nor0"`、`ARKRTOS_FS_BASE 0`、`ARKRTOS_FS_SIZE 4 MiB`。

挂载两步走（这两步是有顺序的，反了会得到 `-7: already exists`）：

```
fs mount            -> mounted nor0 at 0x0, 4194304 bytes
mount /mnt/nor      -> vfs:451 volume nor0 mounted at /mnt/nor
```

`fs` 命令族操作的是 littlefs 层（卷根），`ls /mnt/nor` 走的是 VFS 命名空间；
两者指向同一个卷，但**卷根 ≠ VFS 根**，写文件时路径要带挂载点。

---

## 6. 宿主→板上的文件通道：`fs put`

板上 shell 没有 `echo`，用 vim 灌多行源码在真机上很脆（本轮就踩到过：ESC 没生效，
后续命令行被当正文写进缓冲区，底部 `31L`）。**不要用 vim 做自动化上传。**

`fs put` 是现成的二进制上传通道（`kernelsrc/shell/arkrtos_shell.c`）：

```
fs put <path> <len>        # 板子先报 "len bytes in 256-byte chunks"
                           # 然后每 256 B 一组收数据，落盘成功后回一个 0x06（0x15 = 停）
```

宿主侧脚本 `fsput.py`（本轮新写，放在临时工作目录）：

```
python fsput.py <本地文件> <卷内路径> [--mount]
# 例：fsput.py norapp.c /k.c --mount   ->  fs put: ok, 447 bytes in /k.c
```

要点两条：
- **MSYS/Git-Bash 会把 `/k.c` 改写成 Windows 路径**，必须 `MSYS_NO_PATHCONV=1`，
  否则板子收到的是 `fs put C:/Users/.../PortableGit/k.c`（本轮第一次就撞上）；
- 每次 `0x06` 是「**已经落盘**」才回的，不是「收到」就回 —— 位敲写慢，这个语义
  正好防止宿主跑过头把 128 B 控制台 FIFO 冲爆。

---

## 7. 全链路实录

```
# 1) 宿主：上传源码
fsput.py norapp.c /k.c --mount                  -> fs put: ok, 447 bytes in /k.c

# 2) 板上：编译到 NOR（四遍差分 + 打包）
cc --pack /mnt/nor/k.c -o /mnt/nor/k.svcm
  cc: pack /mnt/nor/k.c -> /mnt/nor/k.svcm (4 passes, -O1)
  cc:   A 2940 B / B 2940 B / C 2940 B / D 2940 B
  cc: /mnt/nor/k.svcm: payload 2940 + table 6 x 4 = 3220 B
  cc:   reloc rom 1 / ram 1 / rom_movw 2 / ram_movw 2, entry +0x422
  cc: packed in 24811 ms

# 3) 复位，重新挂载，确认文件还在
ls -l /mnt/nor      -> k.c 447 / k.svcm 3220

# 4) 跑
mini run /mnt/nor/k.svcm   -> NOR: app alive / NOR: ctor ok / 任务退出、块归还
```

编译出来的固件需要三个开关（见 §9 的尺寸账）：

```
make USE_QSPI=1 USE_APPPACK=1 USE_MINI_VFS=1
```

- `USE_QSPI=1` —— 编入 `drvqspi.c` 与 `nor` 命令；
- `USE_APPPACK=1` —— `cc --pack` 的四遍差分打包（M2 的能力，默认关）；
- `USE_MINI_VFS=1` —— `mini run` 从 VFS 命名空间读（要读 `/mnt/nor/...` 就必须开）。

`make` 不跟踪 `-D` 与头文件：**换开关前先 `make clean`**。

---

## 8. 速度：为什么是 24.8 s / 33.7 s

| 动作 | 目标卷 | 耗时 |
|---|---|---|
| `cc --pack` 产出 3220 B | ramfs `/tmp` | **168 ms** |
| `cc --pack` 产出 3220 B | littlefs `/mnt/nor` | **24811 ms** |
| `mini run` 装载 2940 B 负载 | ramfs `/tmp` | 与上面同批，秒内完成 |
| `mini run` 装载 2940 B 负载 | littlefs `/mnt/nor` | **33.7 s**（到第一行输出） |

两个数量级的差距不是「NOR 慢」这么简单，它叠了三层：

1. **位敲本身**（§3）：62 KB/s，是 QUADSPI 的百分之一量级；
2. **littlefs 的写放大**：3220 B 的写入要反复改元数据块，每次改动都可能触发一次
   4 KB 扇区擦除（实测量级：24.8 s ÷ 单扇区擦除 ≈ 数百次），这是写侧的主要成本；
3. **装载路径每次 open 都重读元数据**：`arkrtos_mini_read_exact` 每块调用一次
   `arkrtos_vfs_read_at`，而它内部是 open + seek + read + close，`g_fs` 只有一份
   文件缓存，于是每块都要重新把元数据从 NOR 读一遍。

提速方向（按收益排序，都还没做）：

- **位敲去掉 1 µs 级延时**（改用 DWT 计数或直接空转，先实测最小可靠周期）；
- **读改回 QUADSPI 内存映射**（先解决 §3 的「首命令被吞」与 FIFO 残留，否则会把
  错误数据当镜像读进来）；
- **装载侧减少 open 次数**（给 VFS 桥加一个「整文件读」入口，或在 `mini` 里把
  chunk 放大到一次读完）。

在没解决之前，**M3-b 的可用于是「能持久化、能跑」，不是「能当产线用」**。

---

## 9. 尺寸账本

| 构建 | FLASH | RAM |
|---|---|---|
| 0 档（全默认，`arkrtos_fs_last_is_noent` 之前） | 381808 | 439308 |
| 0 档（全默认，**含**本轮 noent 修复） | **381832**（+24） | 439284（−24） |
| `USE_QSPI=1`（仅探针，AF 修正前） | 392824 | — |
| `USE_QSPI=1`（AF 修正 + 写擦 + 位敲通道，noent 之前） | 398320 | 439308 |
| `USE_QSPI=1 USE_APPPACK=1 USE_MINI_VFS=1`（**本轮板上固件**） | **405240** | **439324**（97.51%） |

- 0 档的 **+24 B** 是 noent 修复的固有代价（一个谓词函数 + 一个调用点），
  它不是开关功能，不能指望「默认关就零变化」——修缺陷会改体积，如实记账。
- RAM 只剩 **97.51%**，再往上加功能要先看池与栈，别默认还有余量。
- 固件**不逐字节可复现**：`BOOT` 横幅里的 `build <__DATE__> <__TIME__>` 会让
  每次编译的 `.bin` 哈希不同（同一份源码两次编译实测 md5 不同），所以
  「同一份源码 → 同一个 bin」这条口径在这条链上不成立，验证要以串口读数为准。

---

## 10. 已知限制与欠账

1. **开机仍要手动挂载**：`fs mount` + `mount /mnt/nor` 两条命令。`arkrtos_mini_boot_autostart()`
   只在「本次开机第一次成功挂载」时读 `/mini.autostart`，而那份清单在 **ramfs 根**
   上，复位即失；所以**跨复位自启目前不成立**（M3-b 判据里的「`mini autostart` 能
   起来」这一条**尚未达成**）。要达成它需要有「开机自动挂 nor0」的板级动作 + 清单
   落在持久卷上，属新功能，须先定开关与零变化口径。
2. **`mini autostart` 的清单路径与卷绑定**：清单写的是 VFS 路径（如
   `/mnt/nor/k.svcm`），而清单文件是 ramfs 的 `/mini.autostart` —— 两者不在同一个
   卷上，掉电后清单先没，路径再对也没用。
3. **M3-c 未做**：首次挂载未格式化的卷要**明确报错**而不静默格盘，现在还没有专门
   的判据与用例。
4. **速度**：见 §8，未优化。
5. **QUADSPI 的写路径没修**：`nor_write_enable/nor_page_program/nor_sector_erase`
   的 QUADSPI 版本留在代码里但没有进数据通路，`SR` 完成位语义也没定论。

---

## 11. 复跑方法

宿主脚本（临时工作目录，未入库）：

| 脚本 | 用途 |
|---|---|
| `fsput.py <本地> <卷内路径> [--mount]` | 走 `fs put` 把文件传进卷；`--mount` 会先 `fs unmount/mount/mount /mnt/nor` |
| `m1_drive.py <命令文件> [--noflash] [--log x]` | 烧录 + 逐行发命令（`@秒` 前缀控制该行的等待） |
| `m3b_final.py [--log x]` | 一键验收：复位 → 挂载 → `ls` → `stat` → `mini run`（150 s 窗口）→ `mini list` |
| `reset.sh` | 只复位不烧录（OpenOCD `reset run`） |

命令行的坑：**不要把长任务的输出管道接给 `head`** —— `head` 提前关管道会让 Python
抛 `BrokenPipeError` 直接死掉，看起来像「板子挂了」（本轮踩过一次，日志停在
`>>> mini run` 且退出码 1）。

## 12. 本轮改动清单

| 文件 | 性质 |
|---|---|
| `board/stm32h743/drvqspi.c` / `.h` | 新增：QSPI NOR 驱动（位敲数据面 + QUADSPI 取证 + `nor` 命令） |
| `board/stm32h743/drvblk.c` / `.h` | 新增 `nor0` 块设备（bringup 读 JEDEC） |
| `board/stm32h743/arkrtos_board.c` | 板级接线 |
| `config/stm32h743/arkrtos_partition.h` | `ARKRTOS_NOR_CHIP_SIZE` 16 MiB → 8 MiB、注释与实物对齐 |
| `kernelsrc/include/arkrtos_fs.h`、`kernelsrc/src/arkrtos_fs.c` | 新增 `arkrtos_fs_last_is_noent()` |
| `kernelsrc/src/arkrtos_vfs_lfs.c` | 修：新建文件被当成 I/O 错误 |
| `kernelsrc/include/arkrtos_features.h`、`example/.../sources.mk`、`Makefile` | 开关接线（`USE_QSPI` / `USE_APPPACK` / `USE_MINI_VFS`） |
