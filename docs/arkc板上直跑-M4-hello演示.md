# arkc 板上直跑 M4：工程文件在 NOR 上，`cd /mnt/nor && make`，然后 `mini run`

**这份文档记录演示路径本身**：一个标准 POSIX 的 hello world 工程，源码常驻在
外置 NOR flash 上，板上敲一条 `make` 编出小程序镜像，`mini run` 装进去跑，
打印 `hello, world` 后自己退出、把两块 RAM 还回池里。

```
ark@arkrtos:~$ cd /mnt/nor
ark@arkrtos:/mnt/nor$ make
make: hello.svcm <- start.c (+more)
...
cc: hello.svcm: payload 7688 + table 6 x 4 = 7968 B
make: ok
ark@arkrtos:/mnt/nor$ mini run hello.svcm
MINI:579 /mnt/nor/hello.svcm: 7688 B payload -> code 0x24070000 (8192 B block), RW/stack 0x24074000 (16384 B), task 4
hello, world
MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
```

## 0. 为什么不再有一条 `hello` 命令

2026-11-07 之前板上有一个 `hello` 命令：它把演示源铺到 `/demo` 再打印一串
命令行让人照着敲。那不是 Linux 的体验——工程文件本该和程序一起待在盘上，
构建本该是 `make`。那条命令已经**整段删除**（命令、源码、`USE_HELLO` 开关、
Makefile 宏、开关指纹），回归用例 `mini-sdk-nor-make` 里有一条
`hello → command not found` 的反假绿断言，防止有人把它加回来。

现在演示用的四个源文件常驻 NOR 卷（断电不丢），上电就挂好在 `/mnt/nor`。

## 1. 一次性准备

### 1.1 固件

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j8 CROSS="<arm-none-eabi->" \
     USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
```

`USE_SDRAM=1` 在这里不只是"多给点内存"：它把 arkc 的编译池（4 MB）、ramfs
文件缓冲、以及 `cc --pack` 的四份 16 KB 负载 + 16 KB 对象/镜像缓冲全部搬到
外置 SDRAM（`ARKRTOS_PACK_IN_EXTRAM` 跟着 `USE_SDRAM` 走）。片内 AXI SRAM
只留内核与栈。

实测体积（2026-11-07）：FLASH 460848 B (87.90%)、RAM 285628 B (63.39%)、
SDRAMBUF 10744 KB / 12 MB、SDRAMFS 5849888 B / 6 MB (92.98%)。

烧写（OpenOCD + DAPLink，本仓库的 `tools/final_app.py` 里有现成封装）：

```sh
py -3 tools/final_app.py            # 默认会先烧再跑 6 条用例
py -3 tools/final_app.py --noflash  # 已烧过，只跑用例
```

### 1.2 工程文件进 NOR（只在换工程时做一次）

NOR 上的东西掉电不丢，所以这一步**不是每次演示都要做**：

```sh
py -3 tools/mini_deploy.py --port COM3
```

它把小程序 SDK 的 5 个文件（`start.c`/`mini_main.c`/`rt.c`/`arkrtos.h`/
`unistd.h`）和工程目录（`example/mini_hello/` 的 `hello.c` + `makefile`）
一起推进卷根，共 7 个文件、约 17 KB。卷内用短名不是偷懒：`ark make` 的一行
值上限是 96 字节，四条长路径拼进 `SRCS` 直接超限（板上实测
`value is longer than the field that holds it`）。

## 2. 板上：`cd` + `make` + `mini run`

上电后 `/mnt/nor` 已经挂好（`ARKRTOS_AUTOMOUNT_NOR`，见 §4），不需要先敲
`fs mount` / `mount`：

```
ark@arkrtos:~$ ls /mnt/nor
  hello.c                  1732
  makefile                  921
  start.c                  2763
  mini_main.c              2197
  rt.c                     4131
  arkrtos.h                2827
  unistd.h                 2331
ark@arkrtos:~$ cd /mnt/nor
ark@arkrtos:/mnt/nor$ make
make: hello.svcm <- start.c (+more)
make:   src mini_main.c
make:   src rt.c
make:   src hello.c
make:   -I/mnt/nor
cc: pack 4 sources -> hello.svcm (4 passes, -O1)
cc:   A 7686 B
cc:   B 7686 B
cc:   C 7686 B
cc:   D 7686 B
cc:   A +0 is 0xB5E0 (flat image, no vector table; rom base 0x8000000, window 16384 B)
cc:   entry mode: flat miniapp, entry at +0 (no Reset_Handler)
cc: hello.svcm: payload 7688 + table 6 x 4 = 7968 B
cc:   reloc rom 0 / ram 0 / rom_movw 1 / ram_movw 5, entry +0x0
cc: packed in 8404 ms
make: ok
ark@arkrtos:/mnt/nor$ mini run hello.svcm
MINI:579 /mnt/nor/hello.svcm: 7688 B payload -> code 0x24070000 (8192 B block), RW/stack 0x24074000 (16384 B), task 4

mini running: task 4, code 7688 B in 8192 B @ 0x24070000, RW/stack 16384 B @ 0x24074000
  running 1 of 2 allowed
ark@arkrtos:/mnt/nor$ hello, world
TASK:2584 task 4 exits (self kill)
MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
ark@arkrtos:/mnt/nor$ mini
mini: none running (usage: mini run <path>)
```

四遍差分的每一次耗时约 2.1 s，整条 `make` 8.4 s（四遍 × 4 份源码编译 + 4 次
链接）。`mini run` 用的是**相对名**：它与 `make` 读 makefile 走同一套路径
解析（按 shell 的 cwd），cwd 在 `/mnt/nor` 就都能用。

## 3. 这个工程里有什么

### 3.1 `hello.c` —— 标准 POSIX，一句 Ark RTOS 的话都没有

```c
#include <unistd.h>

#define GREETING "hello, world\n"

void MiniMain(void)
{
    (void)write(STDOUT_FILENO, GREETING, sizeof(GREETING) - 1);
}
```

它在 Linux 上就是 `cc hello.c && ./hello`。入口叫 `MiniMain()` 而不是
`main()`：SDK 的 `main()` 调 `MiniMain()`，返回后 `arkrtos_thread_exit()`
把两块 RAM 交还内核——"进程收尾"由 SDK 负责，和桌面上由 C 运行时 `exit()`
是一回事。

### 3.2 SDK 的两个源文件 + 一个运行时

| 文件 | 作用 |
|------|------|
| `start.c`（源 `arkrtos_mini_start_arkc.c`） | `MINISTART`：调 `__arkcc_init_globals()`（arkc 把全局初值编成一个函数），再调 `main()`，**必须排在 `SRCS` 第一位** |
| `mini_main.c`（源 `arkrtos_mini_main.c`） | `main()`：`MiniMain()` → `arkrtos_thread_exit()` |
| `rt.c`（源 `sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c`） | POSIX 控制台子集：`write()` / `_exit()`，两条 SVC（`0x1A` 子命令 3 打印、`0x1B` 子命令 3 退出），SVC 号与 `app_sdk/arkrtos_oslib.c` 逐位相同 |
| `arkrtos.h` / `unistd.h` | arkc 版最小头：`MiniMain()` 声明、`arkrtos_thread_exit()`、`STDOUT_FILENO`、`write`/`_exit` |

**为什么不用完整的 `arkrtos_posix.c`**：那份 3600 行，需要 `stdarg`（arkc 没有
varargs），还拖着 socket / VFS 两条链，而小程序的**整份负载要搬进 RAM 执行**，
8 KB 代码块装不下。所以这里只提供 POSIX 的控制台子集，并把没有的东西如实
写在 `unistd.h` 里（没有 stdio、没有 open/read、没有 fork）。

### 3.3 `makefile`（921 B，就在 NOR 卷根）

```
TARGET = hello
OUT    = hello.svcm
SRCS   = start.c mini_main.c rt.c hello.c
INCS   = /mnt/nor
```

`ark make` 只认 `NAME = value` / `NAME += value` / `#` 三种行，参数上限是
`ARKMAKE_MAX_SRCS=4`、`ARKMAKE_MAX_INCS=8`、一行值 96 字节。

## 4. 上电就挂好 `/mnt/nor`（`ARKRTOS_AUTOMOUNT_NOR`）

板上的 "fstab"：boot 时 VFS 起来之后挂 NOR 卷到 `/mnt/nor`，控制台一起就能
`cd` 过去。开关默认 **0**（不挂、不打印、不碰 littlefs 与 VFS 的挂载路径，
控制台第一屏与命令表逐字节不变），板级构建把它绑在 `USE_QSPI` 上——有 NOR
才有卷。挂不上不拦启动（空片、没焊芯片都要能进控制台自己问），只打一行：

```
FAIL BOOT: cannot mount nor0 at /mnt/nor: -5 (...); the console still comes up, run 'fs mount' / 'nor info'
```

## 5. 已知限制（都不是"再调调就好"）

1. **一个 `make` 最多 4 个源文件**（`ARKMAKE_MAX_SRCS`）。要更多得改 `cc_pack`
   的对象槽分配——那 16 KB 对象区要按源数均分。
2. **`SRCS` 的第一个必须是启动桩**：小程序没有复位向量（被内核当任务起来，
   栈由 `mini run` 从 RAM 块给），入口就是负载 `+0`。打包时会校验 `+0` 是
   THUMB `PUSH`（`0xB5Ex`），不是就拒绝出镜像——不猜入口。
3. **多个对象里只能有一个拥有 `.data/.bss`**：`-link` 不搬数据段，
   两个对象都放 RW 会撞同一个地址并**报错**（实测）：`SRAM overlap between
   objects #2 and #3`。所以 `write()` 的 96 字节缓冲放在 `rt.c` 里，工程自己
   别再声明带初值的全局。
4. **平铺镜像没有向量表**：入口偏移恒为 0，`mini run` 的栈来自 RAM 块，
   镜像头里的 SP/入口字段与它无关。`cc --pack` 对**定义了 `Reset_Handler`**
   的镜像仍走"留 0x400 洞 + `-link` 填表"那条老路（M2 用例就是它），两条
   路由对象自己的符号表判定，不靠声明。
5. **控制台输出是 NUL 结尾字符串**：`write()` 分块拷贝补 `\0`，所以往控制台
   打二进制不在此列（`unistd.h` 里写明了）。
6. **`printf` 不可用**：arkc 没有 varargs，SDK 的格式化器
   （`arkrtos_ulog.h`）在 `__ARK_CC__` 下整段排除。要输出就用 `write()`。

## 6. 这条演示的回归用例

`tools/final_app.py` 里两条：

| 用例 | 判什么 |
|------|--------|
| `mini-sdk-nor-make` | `ls /mnt/nor` 能看到工程文件（证明上电自动挂载）→ `hello` **command not found**（反假绿）→ `cd /mnt/nor && make`（四遍、`entry mode: flat miniapp`、`payload + table`）→ `mini run hello.svcm` 打出 `hello, world` 且两块 RAM 归还 |
| `mini-sdk-missing-minimain` | 工程没定义 `MiniMain` 时必须 `unresolved symbol "MiniMain"` + `cc: pass A failed`；**不许**"报错但照样出文件"（链接器以前只报诊断、仍写出镜像，四遍照做，最后在打包阶段报一个不相干的 -33） |

## 7. 其它相关文档

- M1/M2（固定窗口直跑、四遍差分打包原理）：[arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)
- QSPI NOR 与 littlefs 卷：[arkc板上直跑-M3b-QSPI持久化.md](arkc板上直跑-M3b-QSPI持久化.md)
- `ark make` 本身：[ark make-板上构建工具.md](ark make-板上构建工具.md)
- 开关总表：[功能开关.md](功能开关.md)
