# Kconfig 配置系统

Ark RTOS 把"编什么、编多大"从散落在头文件里的 `#define` 收拢成一套和 Linux 同形的
Kconfig 菜单。它不引入任何第三方依赖：解析器、生成器、菜单编辑器都是本仓库
`tools/kconfig/` 下的纯 Python，随便一台装了 Python 3 的机器就能跑。

## 1. 设计基线

| 选择 | 做法 | 理由 |
|---|---|---|
| 语法 | Linux Kconfig 子集 | 用过内核的人不用学新东西 |
| 解析器 | 自研纯 Python（`kconfig.py`） | 无 pip 依赖，离线可构建 |
| 符号名 | **与 C 宏同名**，不加 `CONFIG_` 前缀 | 菜单里看到的名字就是代码里用的名字，没有翻译层 |
| 产物 | `kernelsrc/include/arkrtos_autoconf.h` | 一个头文件，内核和 SDK 都 include 它 |
| 覆盖 | 每个宏都包 `#ifndef` | 板级头（能力/资源）仍能覆盖；**工程宏已清零**，见 §8 |

符号 `ARKRTOS_USE_LWIP` 生成的就是：

```c
#ifndef ARKRTOS_USE_LWIP
#define ARKRTOS_USE_LWIP 1
#endif
```

## 2. 目录布局

```
Kconfig                        # 根菜单：mainmenu + 4 条 source
config/kconfig/
    Kconfig.features           # 功能门控（内核核心 / 同步 / 服务 / 可靠性）
    Kconfig.capacity           # 各类静态容量上限
    Kconfig.posix              # POSIX 层容量
    Kconfig.net                # lwIP 端口容量
config/arkrtos_defconfig       # 共享头的来源（= F427 例程配置，Keil 工程用它）
config/stm32h743_defconfig     # H743 GCC 工程的 per-build 基座
config/stm32f427_gcc_defconfig # F427 GCC 工程的 per-build 基座（只差 RTSTAT/SECCHECK=n）
config/minimal_defconfig       # 实测最小档（每个选配都关；体积读数见 [功能开关](功能开关.md)）
config/minimal_noshell_defconfig # 最小档 + 关掉 Shell（≈37.8 KB 那一行）
kernelsrc/include/arkrtos_autoconf.h   # 生成产物（入库）
tools/kconfig/
    kconfig.py                 # tokenizer + 递归下降求值 + 定点求解
    genconfig.py               # 生成 autoconf.h，兼 --check
    menuconfig.py              # 行式配置编辑器（--script 可测）
```

> 注意：Windows 文件系统大小写不敏感，仓库根不能同时存在名为 `Kconfig` 的文件和
> 名为 `kconfig/` 的目录，所以四个菜单片段放在 `config/kconfig/` 而不是根目录。

## 3. 注入点

`kernelsrc/include/arkrtos_types.h` 是内核和 SDK 每个翻译单元都会到达的最底层头，
生成文件在那里被 include：

```c
#include <arkrtos_autoconf.h>
```

放在这一层，是为了让 App 侧（POSIX / socket 的容量宏）和内核侧看到同一套值。

**三处 include 都用尖括号**（`arkrtos_types.h:15`、`arkrtos_features.h:31`、
`ark_shell/ark_shell_config.h:12`）。这不是风格问题：GCC 工程要在 `-I` 最前面插一份
per-build 头去遮住共享头（§8），而 `#include "…"` 会**先搜包含者自己的目录**，
再走 `-I`，`-I` 排在前面也遮不住。实测症状是「改了 defconfig 没有任何变化」。

## 4. 用法

```bash
# 列出全部符号及其当前值
py -3 tools/kconfig/genconfig.py --list

# 从默认值生成头文件（--kconfig 默认取 cwd 下的 Kconfig：在子目录里跑必须显式给）
py -3 tools/kconfig/genconfig.py --kconfig Kconfig --defconfig config/arkrtos_defconfig \
      --out kernelsrc/include/arkrtos_autoconf.h

# 校验已入库的头文件是否与 Kconfig + defconfig 一致（CI 用）
py -3 tools/kconfig/genconfig.py --kconfig Kconfig --defconfig config/arkrtos_defconfig \
      --out kernelsrc/include/arkrtos_autoconf.h --check

# 交互式改配置（也可 --script 喂脚本，便于自动化）
py -3 tools/kconfig/menuconfig.py --defconfig config/arkrtos_defconfig
```

`--savedefconfig` 可把当前配置回写成 defconfig；`--olddefconfig` 用旧配置补齐新符号。

## 5. 依赖关系

功能之间的硬约束写在 Kconfig 里（`depends on`），和 C 侧 `arkrtos_features.h` 的
`#error` 是同一套规则的两种表达，谁先触发都能挡住错误组合：

| 打开 | 必须已有 |
|---|---|
| `ARKRTOS_USE_FS` | `ARKRTOS_USE_BLK` |
| `ARKRTOS_USE_VFS` | `ARKRTOS_USE_BLK` |
| `ARKRTOS_USE_VFS_LFS` | `ARKRTOS_USE_VFS` && `ARKRTOS_USE_FS` |
| `ARKRTOS_USE_MINIAPP` | `ARKRTOS_USE_FS` |
| `ARKRTOS_USE_LWIP` | `ARKRTOS_USE_MQ` |

在 menuconfig 里把依赖不满足的项置 `y`，会被明确拒绝（`REFUSED`），而不是悄悄
生成一个编不过的配置。

## 6. 数量限制

原先散在头文件里写死的上限（任务表、同步对象表、队列、定时器、设备表、socket、
文件描述符、pthread key、目录项名长……）现在都成了 Kconfig 符号。

迁移原则是**零行为变化**：Kconfig 只提供默认值，C 侧保留同一份默认值并加
`#ifndef` 守卫。也就是说——

* 不跑 Kconfig 时，行为和过去完全一致；
* 想改上限时，改 defconfig 或命令行 `-D`，两边都能生效。

F427 内核在接入前后 `Code=172734` 一模一样，就是这个原则的实测证据。

## 7. 与 `arkrtos_features.h` 的关系

`arkrtos_features.h` 仍是代码里 `#if ARKRTOS_USE_XXX` 的落脚点；Kconfig 负责
**决定**这些宏取什么值，并保证取值之间处于合法组合。两者职责不重叠。

这条分工要求「features.h 里的每个开关都必须在 Kconfig 里有一条 `config`」——否则
它在 `menuconfig` 里看不到、也没法从 defconfig 改。早期有几个开关漏在 features.h
单独存在（`ARKRTOS_USE_LWIP / _MEMPOOL / _PROCFS / _SOFTBUS / _UTEST`、
`ARKRTOS_HW_SAFE` 与两个 MiniApp 旋钮），现已全部补入 Kconfig，与 features.h 的
`#ifndef` 默认值逐一对齐；`tools/ci_gate.py` 的 `features-switches` 检查会盯住这条
约束，漏一个就红。

补齐后 F427 内核重新编译，`Code=211002 RO-data=17378 RW-data=888 ZI-data=116316`
与补齐前逐字节相同——宏值本来就一样，Kconfig 只是让它们**可被菜单配置**。

## 8. 工程宏清零：Kconfig 是唯一取值来源（2026-11-14）

在此之前，`arkrtos_autoconf.h` 的 `#ifndef` 意味着**命令行 `-D` 永远赢**：
Keil 的 `Options → C/C++ → Define` 里写死的 `ARKRTOS_USE_LWIP=1` 等，Kconfig 里
改成 `n` 也关不掉（实测裁最小档时死在
`#error "ARKRTOS_USE_LWIP=1 requires ARKRTOS_USE_MQ=1"`）。这一轮把工程宏清零，
并把「每次构建的旋钮」也接到 Kconfig 上：

| 工程 | 以前的工程宏 | 现在 |
|---|---|---|
| F427 Keil | `ARKRTOS_USE_LWIP=1` `_SOFTBUS=1` `_HW_SAFE=1` `_RTSTAT=1` `_SECCHECK=1` `ARKRTOS_USE_FPU=1` | `<Define>` 只剩 `USE_HAL_DRIVER,STM32F427xx,ARKRTOS_BOARD_CONFIG="…"`；那 5 个功能开关写进 `config/arkrtos_defconfig` |
| F401 Keil | 同上 + `ARKRTOS_USE_FPU=1` | `<Define>` 只剩三项；共享头（= F427 配置）里多开的 5 项由 `board/stm32f401/arkrtos_board_config.h` 的 `#undef` + `#define 0` 关掉 |
| H743 GCC | `-DARKRTOS_USE_ARKVIM=1` `-DARKRTOS_USE_SDRAM=1` 等一串 | `DEFS` 里不再有 `-DARKRTOS_USE_*`；`make USE_ARKVIM=0` 变成**旋钮**，写进 per-build defconfig |
| F427 GCC | `-DARKRTOS_USE_LWIP=1` `-DARKRTOS_USE_SOFTBUS=1` `-DARKRTOS_USE_HW_SAFE=1` | 同上，只留 `-DUSE_HAL_DRIVER -DSTM32F427xx -DARKRTOS_USE_FPU=1 -DARKRTOS_BOARD_CONFIG=…` |

仍然保留为 `-D` 的四类，**都不是 Ark 内核功能开关**，不要往 Kconfig 里搬：

* `ARKRTOS_USE_FPU` / `ARKRTOS_USE_MPU` / `ARKRTOS_USE_PRIV`：板卡能力，
  根 `Kconfig` 明写「Board capability is NOT here」；且上下文切换 `.s` 走
  gcc/armasm 预处理，根本不经过板级头；
* `ARKRTOS_BOARD_CONFIG`：它本身就是「用哪个板级头」的选择器，鸡生蛋；
* `MDK_TRACE_*` / `LFS_NO_*` / `ARK_BUS_*` / `ARKVIM_*` / `ARKC_*` / `ARK_RAMFS_*`：
  上游组件自己的编译期预算，不属于内核菜单；
* `USE_HAL_DRIVER` / `STM32F4xx`：芯片厂 SDK 的约定。

### 8.1 GCC 侧：per-build 头遮蔽共享头

GCC 工程要保住 `make USE_ARKVIM=0` 这种「每次调用一个值」的用法，又不能让
`-D` 压过 Kconfig，做法是**每次构建生成一份头，并用 `-I` 放在最前面**：

```
make
  ├─ 生成 build/kconfig/<board>_defconfig  = config/<board>_defconfig + 本次旋钮
  ├─ 生成 build/kconfig/arkrtos_autoconf.h （genconfig.py，--kconfig $(ROOT)/Kconfig）
  └─ INCS 首位 -Ibuild/kconfig  ⇒  #include <arkrtos_autoconf.h> 命中它
```

三个配套细节，缺一个就会「看起来接上了、其实没生效」：

1. **`.DEFAULT_GOAL := all` 必须显式写**。Makefile 里一旦出现「生成 defconfig」这种
   规则，make 会把第一个目标当默认目标，`make` 就只生成 defconfig 然后退出：
   产物时间戳不动、`exit 0`、什么也没编。
2. **`cmp` 保护 mtime**：defconfig 的配方每次都跑（靠 `FORCE`），但内容没变时不
   覆盖文件，否则每次 `make` 都触发全量重编。
3. **`--kconfig $(ROOT)/Kconfig` 必须显式给**：默认取 cwd 下的 `Kconfig`，在
   `example/.../GCC` 里跑会报 `Kconfig not found: Kconfig`。

副作用是好的：改旋钮 ⇒ 生成头内容变 ⇒ `-MMD` 依赖失配 ⇒ 自动重编，
不再需要「切开关必须 `make clean`」。

### 8.2 五个 defconfig 的定位（不要写错地方）

| 文件 | 谁在用 | 语义 |
|---|---|---|
| `config/arkrtos_defconfig` | **入库共享头** `kernelsrc/include/arkrtos_autoconf.h` 的来源；F427 / F401 Keil 工程 | 默认档 = F427 例程配置（LWIP / SOFTBUS / HW_SAFE / RTSTAT / SECCHECK / SHELL / UTEST 全开） |
| `config/stm32h743_defconfig` | H743 GCC（首行 `KCFG_BASE`） | H743 例程实证配置：关 LWIP / SOFTBUS / RTSTAT / SECCHECK 等 |
| `config/stm32f427_gcc_defconfig` | F427 GCC（首行 `KCFG_BASE`） | 与共享 defconfig 只差 `RTSTAT=n` `SECCHECK=n` 两行——历史事实 + Flash 99.1% |
| `config/minimal_defconfig` | 手工 `--defconfig` | 每个选配都关（含 `ARKRTOS_USE_UTEST=n`） |
| `config/minimal_noshell_defconfig` | 手工 `--defconfig` | 上一档 + `ARKRTOS_USE_SHELL=n` |

**板级差异不写进这些文件**：板卡用 `board/<chip>/arkrtos_board_config.h` 的
`#undef` + `#define 0` 表达，经 `arkrtos_config.h:21` 的
`#include ARKRTOS_BOARD_CONFIG` 先于 autoconf 生效 ⇒ 板级值赢。

### 8.3 门禁侧要跟着改（否则门禁假红/假绿）

共享头按 F427 的真值打开了 lwIP 与软总线之后，**只拿 `kernelsrc/include`
去 armclang `-fsyntax-only` 的门禁会立刻编不过**：

```
kernelsrc/include/arkrtos_softbus.h:55: fatal error: 'ark_bus.h' file not found
example/stm32f427/.../main.c:49: fatal error: 'lwip_port.h' file not found
```

这不是被测代码坏了，是门禁没有镜像工程环境。F427 工程还靠两样东西才能把
这两个选配编起来：

1. 头文件搜索路径：`kernelsrc/components/lwip/port`、
   `kernelsrc/components/lwip/src/include`、
   `kernelsrc/components/ark_softbus/include`；
2. 软总线 MCU 档位宏 `-DARK_BUS_*`（写在 uvprojx 的 `MiscControls` 里，
   `arkrtos_softbus.h` 的 `#error` 就是拿它们当闸门）。

`tools/arkrtos_f427_env.py` 是这份环境的**唯一副本**，四道 ON 档门禁
（`tickless_host_gate` / `rtstat_syntax_gate` / `seccheck_syntax_gate` /
`swap_syntax_gate`）都从它取。改 uvprojx 的 `IncludePath` / `MiscControls`
或改软总线档位时，只改这一个文件，四道门禁一起跟上——别只改门禁里的
`INCLUDES`，那样下次换档位又会红。

养这条约束的收益是门禁变真了：改共享头里任何一个开关的默认值，先在这里
撞一次，而不是等上板才发现「工程里还挂着一条 `-D` 把值压回去了」。
