# arkc 板上直跑 M4：NOR 上的标准 C hello world，`cd` 进目录 `make`，再 `mini run`

**这份文档记录演示路径本身**：一个**标准 C** 的 hello world 工程（`#include
<stdio.h>` + `printf` + `int main(void)`，一句 Ark RTOS 的东西都没有），源码
常驻在外置 NOR flash 的 `/mnt/nor/hello/` 里，板上敲一条 `make` 编出小程序
镜像，`mini run` 装进去跑，打印 `Hello, World!` 后自己退出、把两块 RAM 还回池里。

```
ark@arkrtos:~$ cd /mnt/nor/hello
ark@arkrtos:/mnt/nor/hello$ make
make: hello.svcm <- /mnt/nor/start.c (+more)
make:   src /mnt/nor/rt.c
make:   src hello.c
...
cc: hello.svcm: payload 10760 + table 26 x 4 = 11120 B
make: ok
ark@arkrtos:/mnt/nor/hello$ mini run hello.svcm
MINI:579 /mnt/nor/hello/hello.svcm: 10760 B payload -> code 0x24070000 (16384 B block), RW/stack 0x24074000 (16384 B), task 4
Hello, World!
TASK:2584 task 4 exits (self kill)
MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
```

## 0. 为什么不再有一条 `hello` 命令

2026-11-07 之前板上有一个 `hello` 命令：它把演示源铺到 `/demo` 再打印一串
命令行让人照着敲。那不是 Linux 的体验——工程文件本该和程序一起待在盘上，
构建本该是 `make`。那条命令已经**整段删除**（命令、源码、`USE_HELLO` 开关、
Makefile 宏、开关指纹），回归用例 `mini-sdk-std-hello` 里有一条
`hello → command not found` 的反假绿断言，防止有人把它加回来。

## 1. 一次性准备

### 1.1 固件

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j8 CROSS="<arm-none-eabi->" \
     USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
```

`USE_SDRAM=1` 在这里不只是"多给点内存"：它把 arkc 的编译池（4 MB）、ramfs
文件缓冲、以及 `cc --pack` 的四份 16 KB 负载 + 48 KB 对象/镜像区全部搬到
外置 SDRAM（`ARKRTOS_PACK_IN_EXTRAM` 跟着 `USE_SDRAM` 走）。片内 AXI SRAM
只留内核与栈。

实测体积（2026-11-07，含 `printf`）：FLASH 462088 B (88.14%)、RAM 285900 B
(63.45%)、SDRAMBUF 10744 KB / 12 MB、SDRAMFS 5882656 B / 6 MB (93.50%)。

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

它做两件事：

1. 在卷上建出 **`/mnt/nor/hello/`** 目录（走 littlefs 的 `mkdir`），把工程目录
   （`example/mini_hello/` 的 `hello.c` + `makefile`）放进去；
2. 把小程序 SDK 的 5 个文件放到**卷根**（`start.c` / `rt.c` / `arkrtos.h` /
   `unistd.h` / `stdio.h`），并清掉老形态留下的 `/mini_main.c`。

为什么这么分：SDK 是"这套编译器运行时"，一份；工程是"你要编的东西"，可以有
很多份并排放在同一块卷上（`hello/`、`blink/`…），各自的 makefile 用绝对路径
指回根上的 SDK 就能编。`--remote-dir` 可以改工程落点（默认 `/hello`）。

## 2. 板上：`cd` + `make` + `mini run`

上电后 `/mnt/nor` 已经挂好（`ARKRTOS_AUTOMOUNT_NOR`，见 §4），不需要先敲
`fs mount` / `mount`：

```
ark@arkrtos:~$ ls -l /mnt/nor
  -rw-r--r--   1 0     0         2980  start.c
  -rw-r--r--   1 0     0        13164  rt.c
  -rw-r--r--   1 0     0         2787  arkrtos.h
  -rw-r--r--   1 0     0         2459  unistd.h
  -rw-r--r--   1 0     0         3418  stdio.h
  drwxr-xr-x   1 0     0            0  hello
ark@arkrtos:~$ ls -l /mnt/nor/hello
  -rw-r--r--   1 0     0           84  hello.c
  -rw-r--r--   1 0     0         1533  makefile
ark@arkrtos:~$ cd /mnt/nor/hello
ark@arkrtos:/mnt/nor/hello$ make
make: hello.svcm <- /mnt/nor/start.c (+more)
make:   src /mnt/nor/rt.c
make:   src hello.c
make:   -I/mnt/nor
cc: pack 3 sources -> hello.svcm (4 passes, -O1)
cc:   A 10760 B
cc:   B 10760 B
cc:   C 10760 B
cc:   D 10760 B
cc:   A +0 is 0xB5E0 (flat image, no vector table; rom base 0x8000000, window 16384 B)
cc:   entry mode: flat miniapp, entry at +0 (no Reset_Handler)
cc: hello.svcm: payload 10760 + table 26 x 4 = 11120 B
cc:   reloc rom 0 / ram 0 / rom_movw 3 / ram_movw 23, entry +0x0
cc: packed in 10837 ms
make: ok
ark@arkrtos:/mnt/nor/hello$ mini run hello.svcm
MINI:579 /mnt/nor/hello/hello.svcm: 10760 B payload -> code 0x24070000 (16384 B block), RW/stack 0x24074000 (16384 B), task 4

mini running: task 4, code 10760 B in 16384 B @ 0x24070000, RW/stack 16384 B @ 0x24074000
  running 1 of 2 allowed
Hello, World!
TASK:2584 task 4 exits (self kill)
MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
ark@arkrtos:/mnt/nor/hello$ mini
mini: none running (usage: mini run <path>)
```

四遍差分的每一次约 2.7 s，整条 `make` 10.8 s（3 份源码 × 4 遍编译 + 4 次
链接）。`mini run` 用的是**相对名**：它与 `make` 读 makefile 走同一套路径解析
（按 shell 的 cwd），cwd 在 `/mnt/nor/hello` 就都能用。

## 3. 这个工程里有什么

### 3.1 `hello.c` —— 84 字节，标准 C

```c
#include <stdio.h>

int main(void) {
    printf("Hello, World!\n");
    return 0;
}
```

它在 Linux 上就是 `cc hello.c && ./a.out`，连输出都一样。**没有 `MiniMain`、
没有 `write`、没有一句 Ark RTOS 的头文件**。

`main()` 就是入口：SDK 的启动桩（`start.c`）调它，它 `return` 之后由桩调
`arkrtos_thread_exit()` 把两块 RAM 交还内核——"进程收尾"由运行时负责，和桌面
上由 C 库替你 `exit()` 是一回事。

### 3.2 SDK 的两个源 + 三个头

| 文件 | 作用 |
|------|------|
| `start.c`（源 `arkrtos_mini_start_arkc.c`） | `MINISTART`：调 `__arkcc_init_globals()`（arkc 把全局初值编成一个函数），再调 `main()`，它 return 后调 `arkrtos_thread_exit()`。**必须排在 `SRCS` 第一位** |
| `rt.c`（源 `sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c`） | 运行时：POSIX `write()` / `_exit()` 两条 SVC（`0x1A` 子命令 3 打印、`0x1B` 子命令 3 退出，与 `app_sdk/arkrtos_oslib.c` 逐位相同），加上 `printf`/`puts`/`putchar` 的实现 |
| `arkrtos.h` | `arkrtos_thread_exit()`、`ARKRTOS_WEAK`，以及入口约定（工程写 `int main(void)`） |
| `unistd.h` | `STDOUT_FILENO`/`STDERR_FILENO`、`write`/`_exit` |
| `stdio.h` | `printf`/`puts`/`putchar`，以及**支持的转换与硬上限** |

**为什么不用完整的 `arkrtos_posix.c`**：那份 3600 行，拖着 socket / VFS 两条链，
而小程序的**整份负载要搬进 RAM 执行**（16 KB 代码块），装不下。所以这里只提供
控制台子集，并把没有的东西如实写在头文件里。

### 3.3 `printf` 怎么在"ARM 后端没有 va 区"的编译器上变成真变参

`arkc` 的 `stdlib/include/stdarg.h` 是给 **C 后端**写的：`va_start` 展开成读一个
叫 `_ark_va_ptr` 的 extern，而只有 C 后端与 x86_64 后端会把它定义成指向 va 区的
局部变量（`irgen` 为每个变参函数请求把 a0..a7 镜像到 `frame+8000..8063`）。

**ARM 后端从没实现这块**。在补上之前，一个 C 变参函数在 ARM 上曾被翻成

```
        MOV r0, #0
        LDR r0, [r0]        ; 从地址 0 读 va 指针
```

——不报错，然后 `va_arg` 顺着地址 0 上的向量表（也就是一个栈地址）往下走：错答案，
无诊断（`examples/va_test.c` 实测）。现在 ARM / RV32I 后端**明确拒绝** C 变参函数
定义（`variadic function 'x' is not supported by the ARM backend: ...`），并把
`examples/run_tests.ps1` 的 `$expectArmFail` 表当成闸门：哪天有人实现了 va 区，
那几条用例会转红。细节见 arkc 仓库 README 的「后端」一节。

所以这里不假装有 `va_list`，也不用宏按参数个数分派（那样 `printf` 就不是函数了）。
做法是一条 `__asm` **取参桩**（`__asm` 整函数体没有 C 体，不引用 `_ark_va_ptr`，
不受上面那条拒绝影响）：

```c
__asm int printf(const char *fmt, ...)
{
    PUSH {r4, lr}
    SUB  sp, sp, #48          /* 12 个槽：格式串 + 11 个参数 */
    STR  r0, [sp, #0]         /* r0-r3 = 前 4 个参数 */
    ...
    LDR  r0, [sp, #56]        /* 调用方压栈的第 5-12 个参数 */
    STR  r0, [sp, #16]
    ...
    MOV  r1, sp
    LDR  r0, [sp, #0]
    BL   arkrtos_mini_printf_words
    ADD  sp, sp, #48
    POP  {r4, pc}
}
```

为什么这样就是对的：AAPCS 下前 4 个参数在 `r0-r3`，第 5 个起由调用方压在自己
的栈上，与 `r0-r3` **一起构成一串连续的字**。桩只做一件事——把这串字落到自己
栈上的 12 个槽里，再把这块内存交给普通 C 函数 `arkrtos_mini_printf_words()`
去按格式串取值。对调用方来说 `printf` 就是普通变参函数，一个宏都不用。

支持的转换：`%d %i %u %x %X %c %s %p %%`，标志与宽度 `-` / `0` / 1-2 位十进制。
**没有**：精度（`%.3s`）、长度修饰（`%ld`）、浮点（`%f`）、宽度写 `*`。

### 3.4 `makefile`（1533 B，在 `/mnt/nor/hello/`）

```
TARGET = hello
OUT    = hello.svcm
SRCS   = /mnt/nor/start.c /mnt/nor/rt.c hello.c
INCS   = /mnt/nor
```

- 三份源共 44 字节，写**绝对路径**刚好在 `ark make` 的一行 96 字节内，所以不用
  `..` 这种要靠路径解析去猜的写法。
- `SRCS` 的第一份必须是启动桩：镜像的入口偏移是相对负载起点算的，`MINISTART`
  必须落在偏移 0。
- **没有** `mini_main.c`：arkc 档的入口是工程自己的 `int main(void)`，那份
  「main() 调 MiniMain()」的骨架是 Keil / GCC 档的写法，arkc 档塞进来会直接
  `#error`（见 §5 第 7 条）。

## 4. 上电就挂好 `/mnt/nor`（`ARKRTOS_AUTOMOUNT_NOR`）

板上的 "fstab"：boot 时 VFS 起来之后挂 NOR 卷到 `/mnt/nor`，控制台一起就能
`cd` 过去。开关默认 **0**（不挂、不打印、不碰 littlefs 与 VFS 的挂载路径，
控制台第一屏与命令表逐字节不变），板级构建把它绑在 `USE_QSPI` 上——有 NOR
才有卷。挂不上不拦启动（空片、没焊芯片都要能进控制台自己问），只打一行：

```
FAIL BOOT: cannot mount nor0 at /mnt/nor: -5 (...); the console still comes up, run 'fs mount' / 'nor info'
```

## 5. 已知限制（都不是"再调调就好"）

1. **一个 `make` 最多 4 个源文件**（`ARKMAKE_MAX_SRCS`），对象槽按源数均分
   `CC_PACK_OBJ_ARENA`。SDRAM 档这块是 48 KB：4 个源每槽 12288 B、3 个源
   每槽 16384 B。**槽是按"对象文件"而不是"代码长度"定的**——带 printf 的
   `rt.c` 代码约 2.5 KB，对象文件却是 8660 B（ARKOBJ2 还带符号表与重定位表）。
   按代码长度算会差三倍，这一轮就是在这上面栽过一次（报 `compile rc=-1
   (File too large)`，现在会多打一行槽大小）。
2. **`SRCS` 的第一份必须是启动桩**：小程序没有复位向量（被内核当任务起来，
   栈由 `mini run` 从 RAM 块给），入口就是负载 `+0`。打包时会校验 `+0` 是
   THUMB `PUSH`（`0xB5Ex`），不是就拒绝出镜像——不猜入口。
3. **多个对象里只能有一个拥有 `.data/.bss`**：`-link` 不搬数据段，
   两个对象都放 RW 会撞同一个地址并**报错**（实测）：`SRAM overlap between
   objects #2 and #3`。所以 `write()` 与 `printf` 的缓冲都放在 `rt.c` 里，
   工程自己别再声明带初值的全局。
4. **平铺镜像没有向量表**：入口偏移恒为 0，`mini run` 的栈来自 RAM 块，
   镜像头里的 SP/入口字段与它无关。`cc --pack` 对**定义了 `Reset_Handler`**
   的镜像仍走"留 0x400 洞 + `-link` 填表"那条老路（M2 用例就是它），两条路
   由对象自己的符号表判定，不靠声明。
5. **控制台输出是 NUL 结尾字符串**：`write()` 分块拷贝补 `\0`，所以往控制台
   打二进制不在此列（`unistd.h` 里写明了）。
6. **`printf` 一次调用最多 12 个参数（格式串 + 11 个）**：取参桩只搬 12 个字，
   第 13 个起取不到（读到 0）。编译器本身允许更多参数，所以这条上限是 SDK 的，
   写在 `stdio.h` 与这里，不是"编译器不行"。
7. **工程里不能自己写 C 变参函数**：ARM 后端没有实现 va 区，这类定义会被明确
   拒绝（不是静默出错）。要写就得用 `__asm` 自己取参，或者先把编译器那块补上
   ——补法不是"加个后端分支"，ARM 的 `va_list` 必须按 AAPCS 的寄存器/栈混合
   布局重新设计，现有 `stdarg.h`（8 字节槽位 + `long long *`）是 C 后端的约定。
8. **工程必须自己定义 `int main(void)`**：SDK 在 arkc 档不提供任何默认 `main`，
   漏定义就是链接期 `unresolved symbol "main"`，不会给你一个"能跑但什么都不
   打印"的空程序。历史原因值得记一笔：以前的骨架是 SDK 提供 `main()` + 工程
   写 `MiniMain()`，而 SDK 同时提供一个弱定义的空 `MiniMain`——arkc 在同一个
   翻译单元内把它绑死，工程自己那份强定义永远进不来，表现正是"创建当拍就
   退出、什么都不打印"，既不报错也没有诊断。

## 6. 回归用例

`tools/final_app.py` 里两条：

| 用例 | 判什么 |
|------|--------|
| `mini-sdk-std-hello` | `ls -l /mnt/nor` 能看到 SDK 五件 + `hello` 目录（证明上电自动挂载）→ `hello` **command not found**（反假绿）→ `cd /mnt/nor/hello` → `cat hello.c` 里就是 `#include <stdio.h>` 与 `printf("Hello, World!\n");` → `make`（3 份源、四遍、`entry mode: flat miniapp`、`payload + table`）→ `mini run hello.svcm` 打出 `Hello, World!` 且两块 RAM 归还 |
| `mini-sdk-missing-main` | 工程没定义 `main` 时必须 `unresolved symbol "main"` + `cc: pass A failed`；**不许**"报错但照样出文件"（链接器以前只报诊断、仍写出镜像，四遍照做，最后在打包阶段报一个不相干的 -33） |

## 7. `printf` 的宿主门禁

`printf` 的错法基本都是"打出来是另一句话"，眼睛看不出来。所以格式化主体在宿主
上也跑一遍：

```sh
py -3 tools/mini_sdk_printf_gate.py
```

它用 `gcc -m32` 编 `kernelsrc/sdk/mini_sdk/tests/test_printf_host.c`（**必须 32 位**：
槽位是 32 位的，64 位宿主上指针会被截断），24 个固定用例逐字比对：负数、
`0x80000000`、`%u` 上界、`%x/%X`、`%08x`、宽度、左对齐、`%s` 空指针、`%c`、
`%%`、`%p`、11 个参数、未知转换、末尾孤零零的 `%`、超过 96 字节缓冲的分段
flush。桩本身是 ARM 汇编，宿主上跑不了，由上面的板端用例覆盖——两者合起来
这条链路才算完整。

## 8. 其它相关文档

- M1/M2（固定窗口直跑、四遍差分打包原理）：[arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)
- QSPI NOR 与 littlefs 卷：[arkc板上直跑-M3b-QSPI持久化.md](arkc板上直跑-M3b-QSPI持久化.md)
- `ark make` 本身：[ark make-板上构建工具.md](ark make-板上构建工具.md)
- 开关总表：[功能开关.md](功能开关.md)
