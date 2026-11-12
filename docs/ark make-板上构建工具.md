# ark make：板上的构建工具

## 一句话

板子上已经有 `cc`、`vim`、`shell`、`vfs`，把**几份 `.c` 打成一个能跑的镜像**却
仍然要人手敲一条长命令行：`cc --pack a.c b.c c.c -o app.svcm`。少写一份源码是
看不见的错——镜像照样链接成功，只是少了那个编译单元。`ark make` 把这件事固化
成一份工程描述文件 + 一条命令：

```
ark@arkrtos:/mnt/nor$ make
make: /mnt/nor/app.svcm <- /mnt/nor/a.c (+more)
make:   src /mnt/nor/b.c
cc: pack 2 sources -> /mnt/nor/app.svcm (4 passes, -O1)
...
cc: /mnt/nor/app.svcm: payload 4504 + table 4 x 4 = 4776 B
cc: packed in 38011 ms
make: ok
ark@arkrtos:/mnt/nor$ mini run /mnt/nor/app.svcm
ARKMAKE: link ok
ARKMAKE: cross-file add3=6
```

## 用法

```
make [<makefile>] [-f <makefile>] [-o <out>] [-v] [-j]
make clean [<makefile>] [-f <makefile>] [-o <out>]
```

| 参数 | 含义 |
|---|---|
| `<makefile>` / `-f <f>` | 工程描述文件，缺省 `makefile`（相对当前目录解析） |
| `-o <out>` | 覆盖工程里的 `OUT` |
| `-v` | 把 `cc --pack` 的逐对象读数打出来（每一遍、每一份源的 code 长度与落点） |
| `-j` / `-jN` | **认得出来，但不生效**：单核 + 单个编译池，构建本来就是串行的 |
| `clean` | 删掉这份工程构建出的**成品镜像**（没有中间产物可删，见下） |

选项写错会**立刻**报出来，不会先跑三十秒再失败：一次构建是几十秒的操作，
把参数错拖到最后发现是最浪费时间的一种失败。

`-j` 的处理是**说清楚而不是装没看见**，也不是报错退出：从桌面工程照抄
`make -j8` 的人应该看到一句人话，而不是以为自己拿到了并行加速。

```
ark@arkrtos:/mnt/nor$ make -j8
make: -j has no effect: one core and one compile pool, the build is serial by construction
make: /mnt/nor/app.svcm <- /mnt/nor/a.c (+more)
...
```

`make clean` 删的是**成品**，因为对象文件过完这四遍就不存在了——它们活在
`cc_pack` 的编译池（或静态 arena）里，从来不落到卷上。所以它只做一件事：

```
ark@arkrtos:/mnt/nor$ make clean
make: clean: removed /mnt/nor/app.svcm (4776 bytes)
make: clean: no object files to remove (they live in the compile pool)
ark@arkrtos:/mnt/nor$ make clean
make: clean: /mnt/nor/app.svcm: no such image
```

第二条返回成功（GNU make 的 clean 在没东西可删时也是成功），但必须把
"没找到"说出来，不能安静地什么都不做。clean 也走完整的 makefile 解析：
`OUT`/`TARGET` 认不出来就不知道要删什么，那一步会直接失败而不是猜一个名字。

## 为什么它刻意不是 GNU make

三条都不是"没做完"，而是板上不成立：

| GNU make 的东西 | 板上为什么不成立 |
|---|---|
| 时间戳依赖（`mtime`） | littlefs / ramfs 不保证可靠的 `mtime`。用"比较时间"决定要不要重编，得到的结论是随机的——而且错了以后镜像看起来完全正常 |
| 隐含规则 / 后缀规则 | 需要一套模式匹配与搜索路径语义；它的失败方式是"没有规则可用"和"用了你没想过的规则"两种，很难在板上解释清楚 |
| recipe（每行一个 shell） | 板上没有 shell 子进程，也没有 `sh -c`。留一个假 recipe 只会让人以为能写 `mkdir -p` |
| 并行构建（`-j`） | 一个核、一个编译池（对象槽是静态 arena），没有可并的东西。参数认下来，但明说它不生效 |
| `clean` | 只保留"删成品"这一层语义：板上没有 `.o` 可删，也没有时间戳依赖需要清理 |

**其余任何写法一律报错，不静默忽略**。读不懂却照样构建出一份东西，是这个工具
最该避免的失败方式。实测：

```
ark@arkrtos:/mnt/nor$ make -f bad.mk
make: bad.mk: line 4:  expected `=` after the variable name (rules and recipes are not supported)
ark@arkrtos:/mnt/nor$ make -f nope.mk
make: /mnt/nor/nope.mk: line 0:  cannot read the makefile
```

## 语法

只有三样：

1. `NAME = value` —— 覆盖赋值
2. `NAME += value` —— 追加（未定义过的变量追加报错，不隐式创建）
3. `#` 到行尾 —— 注释

值里的 `$(NAME)` 递归展开（深度上限 8，自引用报错）。工程字段只有五个：

| 键 | 必填 | 含义 |
|---|---|---|
| `TARGET` | 是 | 产物名，缺省产物名 `<TARGET>.svcm` |
| `OUT` | 否 | 完整产物路径；空则用 `TARGET` 推 |
| `SRCS` | 是 | 源文件列表，空格分隔，**上限 4**（与 `CC_PACK_MAX_SRCS` 对齐） |
| `DEFS` | 否 | 宏，空格分隔，上限 16，形如 `LOG_LEVEL=2` |
| `INCS` | 否 | 头文件目录，空格分隔，上限 8 |

最小可用：

```
TARGET = app
OUT    = /mnt/nor/app.svcm
SRCS   = /mnt/nor/a.c /mnt/nor/b.c
```

`SRCS` 里写绝对路径最稳：路径按**当前工作目录**解析，而 `make` 是在哪个目录下
敲的就取决于你。

## 板上实测（2026-11-05，STM32H743）

> **2026-11-07 订正**：下表是**片内 RAM / 80 KB 编译池**档的读数。切到
> `USE_SDRAM=1`（池 4 MB）之后，同一形态的两源工程实测 **1522 ms**（见
> [arkc板上直跑-M4-hello演示.md](arkc板上直跑-M4-hello演示.md) §3），差别不在
> 算法而在池：小池每遍都要在池里腾挪，四遍 × 两源就是四倍开销。
> 另外**计时口径**要小心：同一命令曾实测到 11124 ms，原因是一个 `for(;;)`
> 的小程序以 `APP_TASK_PRIORITY`(10) 在跑、把控制台(14)饿住，`make` 只抢到
> 零碎 CPU。凡是耗时不合理的读数，先确认没有更高的任务在占 CPU
> （演示工程默认 `MINI_PRIORITY=20`，见 docs/功能开关.md）。

| 用例 | 源 | 产物 | 耗时 |
|---|---|---|---|
| `make`（a.c 52 B + b.c 452 B） | 2 份，跨 TU 调用 `add3` | `app.svcm` 4776 B（payload 4504 + 16 B 表） | 38.0 s / 45.2 s（`-v`） |
| `make -f mk2`（DEFS/INCS 探针） | p.c 134 B + q.c 482 B | `probe.svcm` 4800 B | 19.5 s |
| `make -f mk3b`（缺源文件） | 1 份不存在的 | —— | 立即失败 |

跑起来：

```
mini run /mnt/nor/app.svcm   ->  ARKMAKE: link ok
                                 ARKMAKE: cross-file add3=6
mini run /mnt/nor/probe.svcm ->  ARKMAKE: defs+incs ok, probe=42
```

第二条探针是**反假绿**用的：`probe()` 只有在 `DEFS=ARKMAKE_DEFS_PROBE` 真的
传给了编译器、且 `#include "probe.h"` 真的通过 `INCS=/mnt/nor` 找到了，才返回
42。那次运行的工作目录是 `/`，所以头文件除了 `-I` 没有第二条路。

## 三条硬约束

**一、对象必须按真实落点编基址，所以对象不能跨遍复用。**
arkc 在 codegen 期就把绝对地址烤进指令（`movw/movt` 成对、literal slot），
对象里只有外部符号是重定位。四遍打包的差别就在每遍的 `.sct` 上，因此
`-c` 产物只在**同一份 `.sct`** 下有意义的。`make` 因此永远全量重编，
不复用任何中间产物——这不是懒，而是不复用才不会有"搬过位置的对象"。

**二、`make` 只是驱动，不是第二条打包路。**
`ops->pack` 直接调 `arkc_cc_pack()`，也就是 `cc --pack` 的本体。两条独立的
打包实现迟早在板上打出不一样的镜像，而只有一条被跑过。代价写在编译期：

```
#if ARKRTOS_USE_ARKMAKE && !ARKRTOS_USE_APPPACK
#error "ARKRTOS_USE_ARKMAKE=1 requires ARKRTOS_USE_APPPACK=1 ..."
#endif
```

**三、错误码按"失败发生在哪一段"翻译，不合并成一句"失败了"。**
一次 `--pack` 分五段（编译 / 链接 / 布局校验 / 差分打包 / 写盘），同一句
"失败了"落在不同段上意味着完全不同的下一步。所以 `arkc_cc_pack()` 多带一个
出参阶段码，`ark make` 把它翻成自己的错误词汇：

| arkc 阶段 | arkmake 错误码 | 屏幕上的字样 |
|---|---|---|
| `SETUP`（借工作区/注册对象槽/写 .sct） | `ARKMAKE_ERR_SETUP` | `cannot start the build` |
| `COMPILE`（含池耗尽） | `ARKMAKE_ERR_COMPILE` | `compile failed` |
| `LINK` | `ARKMAKE_ERR_LINK` | `link failed` |
| `LAYOUT`（四遍尺寸不一致 / 装不进窗口） | `ARKMAKE_ERR_SIZE` | `image too large` |
| `PACK`（差分不出表、没有向量表） | `ARKMAKE_ERR_PACK` | `cannot pack the image` |
| `WRITE` | `ARKMAKE_ERR_WRITE` | `cannot write the image` |

实测（缺源文件）：

```
cc: pass A: compiling /mnt/nor/nosuch.c failed
make: compile failed (-11)
```

在加阶段码之前这里是 `make: link failed (-12)`——一个错答案：它把人送去查
链接器，而真正的原因在编译段。

## 工程接线

```
kernelsrc/components/arkmake/
├── include/arkmake.h          <- 上游（工具头文件）
├── src/arkmake_parse.c        <- 上游（文本 -> 工程描述，纯函数）
├── src/arkmake_build.c        <- 上游（工程描述 -> 一次构建）
├── arkmake_port.h             <- 本仓库：ctx 与两个入口
├── arkmake_port_arkrtos.c     <- 本仓库：读 / 日志 / 打包三个回调
├── arkmake_shell_cmd.c        <- 本仓库：make 命令
├── arkmake_shell_cmd.h        <- 本仓库：注册钩子
└── VENDOR.md                  <- 同步来源与边界
```

上游树（gitee `xw19981010/ark_make`）由 `tools/sync_arkmake.py` 同步；
组件根目录的四个接线文件属于 Ark RTOS 侧，同步不碰。**同步不删任何文件**：
目标目录里多出来的只报告——一次同步顺手删掉一个还用着的文件，是这套复制式
同步最容易犯的错。

构建接线（`example/stm32h743/kernel/ARKRTOS_TEST/GCC/`）：

```
USE_ARKMAKE ?= 0                       # Makefile 顶部
-DARKRTOS_USE_ARKMAKE=$(USE_ARKMAKE)   # DEFS
-I$(ROOT)/kernelsrc/components/arkmake           # 接线头
-I$(ROOT)/kernelsrc/components/arkmake/include   # 上游头
ifeq ($(USE_ARKMAKE),1)                # sources.mk
SRCS += src/arkmake_parse.c src/arkmake_build.c ...
endif
```

内核开关在 `kernelsrc/include/arkrtos_features.h`（`ARKRTOS_USE_ARKMAKE`，
默认 0），依赖写成两条 `#error`：缺 `ARKRTOS_USE_APPPACK` 或
`ARKRTOS_USE_ARKC` 都编不过。

## 关掉时零变化（实测对照）

同一份工作树，只改 `USE_ARKMAKE`：

| 构建 | text | data | bss | 符号 | `"project file"` 字符串 |
|---|---|---|---|---|---|
| `USE_ARKMAKE=0` | 407484 | 2248 | 437756 | 无 `arkmake_*` / `cmd_make` | 0 处 |
| `USE_ARKMAKE=1` | 414516 | 2248 | 441976 | 9 个 | 2 处 |

关掉那一版的三围与引入本组件之前的固件**逐字节一致**（407484 / 2248 /
437756）；打开那版多 7032 B 代码与 4220 B 静态（一份 2 KB 的 makefile 文本
缓冲 + 一份 2 KB 的 `arkmake_project_t`，都放 `.bss` 而不是任务栈）。

## 这一轮抓到的两个真缺陷

**1. `arkmake_build()` 的总述行漏打第一份源码。**
原实现 `const char *parts[] = { "make: ", out, " <- ", NULL, NULL }` 再用
`while (parts[k] != NULL)` 拼接，第三项永远是 `NULL`，于是循环在补第一份源码
之前就停了，屏幕上打出 `make: out <-  (+more)`。而这一行正是唯一能看出
"少了一份源码"的地方。已在源仓库修正，并在 `tests/test_build.c` 加断言
（总述行必须含 `a.c`），宿主 `make test` 全绿。

**2. `arkc` 的 `-link` 分支没有重置包池（宿主看不见）。**
`ark_compile()` 的 C 路径与汇编路径都在入口 `mem_pool_reset()`，但 `-link`
分支在那一行之前 `return`。宿主机上每次运行都是新进程，池天然从零开始，
所以这条缺陷在宿主复现不了；板上 `cc --pack` 是**一个进程里连做 4 遍**，
链接期继承了上一次编译的峰值（`q.c` 峰值 64720 B，链接本身只要 8488 B），
于是 80 KB 池在链接段被用穿：

```
cc: pool 77400/81920 B
cc: pass A aborted: memory pool exhausted
make: link failed (-12)          <- 这条又被阶段码问题掩盖了一层
```

修法是在 `-link` 分支入口 `mem_pool_reset()`（链接器要的东西它自己从对象文件
重读，编译期的分配一件也不需要）。修完同一份工程从"第 A 遍就 abort"变成
四遍跑完 19.5 s。

## 代价与欠账

- **永远全量构建**：两份源码 19–45 s（四遍 × 每份源码 + 四遍链接；80 KB 池档
  的读数，SDRAM 档实测 1.5 s）。没有增量，也不打算有——见上文"没有时间戳"。
- **单份源码的池上限**：80 KB 池、-O1 下，实测 482 B 的 `q.c` 峰值 64720 B，
  两份 ~450 B 的源码就能把池贴到 77 KB。更大的源码要在
  `ARKC_DEFS` 里调 `ARK_POOL_SIZE`（当前 RAM 只剩约 6.8 KB 余量，调大要先在
  别处腾出来）。
- **SRCS 上限 4**：与 `cc --pack` 的对象槽一致；超了在解析层就报
  `too many entries`，不是等四遍跑完。这个数**不是随手拍的**，抬它要同时动
  四处：`arkmake.h` 的 `ARKMAKE_MAX_SRCS`（上游源仓库）、`arkc_shell_cmd.c`
  的 `CC_PACK_MAX_SRCS`、对象槽预算（槽容量 = arena / 源数），以及上板复验。
  具体卡点：SDRAM 档 arena 48 KB，8 源均分只剩 6 KB 一槽，而实测一个带 printf
  的 10 函数对象是 8660 B——当场 `File too large`；片内档只有 16 KB arena，
  连 4 源都得 12 KB/槽才刚好。所以上限留在 4，超了就说清楚，不静默截断。
- **`-j` 只有"认得出来"这一层**：单核 + 单编译池，构建是串行的。参数不报错，
  但每次都会打出"has no effect"那一行，不假装并行。
- **`clean` 只删成品**：板上没有中间产物（对象活在编译池里，构建结束即消失），
  也没有时间戳依赖需要清理。`clean` 不碰源码、不碰 makefile。

## 复跑步骤

```bash
# 1. 编固件（make 不跟踪 -D，换开关先 make clean）
make -j8 USE_QSPI=1 USE_MINI_VFS=1 USE_APPPACK=1 USE_ARKMAKE=1

# 2. 烧录后挂载 NOR（t≈3 s 后 mount 才稳定）
mount /mnt/nor

# 3. 上传工程描述。fs put 的路径是 littlefs 里的绝对路径（/makefile），
#    不是挂载点路径（/mnt/nor/makefile）——它直接调 arkrtos_fs_open_write，
#    不做 VFS 前缀解析，写挂载点路径会拿到 LFS_ERR_NOENT
fs put /makefile <字节数>

# 4. 构建并运行
cd /mnt/nor
make -v
mini run /mnt/nor/app.svcm    # mini run 要绝对路径
```
