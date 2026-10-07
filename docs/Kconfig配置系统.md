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
| 覆盖 | 每个宏都包 `#ifndef` | `-D` 命令行 / uvprojx 的 `<Define>` 仍能覆盖 Kconfig |

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
config/arkrtos_defconfig       # 默认配置（--defconfig 的输入）
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
#include "arkrtos_autoconf.h"
```

放在这一层，是为了让 App 侧（POSIX / socket 的容量宏）和内核侧看到同一套值。

## 4. 用法

```bash
# 列出全部符号及其当前值
py -3 tools/kconfig/genconfig.py --list

# 从默认值生成头文件
py -3 tools/kconfig/genconfig.py --defconfig config/arkrtos_defconfig \
      --out kernelsrc/include/arkrtos_autoconf.h

# 校验已入库的头文件是否与 Kconfig + defconfig 一致（CI 用）
py -3 tools/kconfig/genconfig.py --defconfig config/arkrtos_defconfig \
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
