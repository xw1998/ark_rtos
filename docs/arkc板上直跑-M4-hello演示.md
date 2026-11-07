# arkc 板上直跑 M4：用真正的 `cc` 和 `make` 编一个 POSIX hello world

**这份文档记录演示路径本身**，对象是工具链，不是某个便捷命令：

- **A 段（arkcc）**：在串口上手敲 `cc` —— 两个翻译单元各编一个对象，`-link`
  把两个对象接成一个镜像，`app run` 把它装进运行窗口跑起来。
- **B 段（make）**：`cd /demo` + `make` —— 板上构建工具读工程文件，驱动同一个
  编译器做四遍基址差分，打出一份可重定位镜像，`mini run` 装进去跑。

两段用的是**同一份源码**，输出必须是同样两行。`hello` 命令在这条流程里的角色
只是**备料**：把四个文件铺到 `/demo` 并打印上面这些真实命令行——它不编译、
不链接、不装载。这一点是刻意的：一条命令跑完的东西证明不了工具链，而演示要
展示的正是"编译器是怎么被用起来的"。

---

## 1. 一次性准备

固件要打开四个开关（`arkrtos_features.h` 里有 `#error` 兜底，关错了编不过，
而不是静默少功能）：

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j8 CROSS="<arm-none-eabi->" \
     USE_SDRAM=1 USE_APPRUN=1 USE_HELLO=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1
```

实测体积（2026-11-07）：FLASH 446944 B (85.25%)、RAM 284560 B (63.16%)、
SDRAMBUF 10744 KB / 12 MB、SDRAMFS 5767968 B / 6 MB。

烧进去，串口 USART1 / COM3 / 115200 8N1：

```
ark@arkrtos:~$ hello
```

`hello` 把 `/demo/hello.c`、`/demo/util.c`、`/demo/hello.sct`、`/demo/makefile`
写好（覆盖写，先删再写，不会追加），再把 A/B 两段的命令原样打在屏幕上。

## 2. A 段实录：`cc` 手敲（2026-11-07，FK743M5-XIH6）

```
$ cc /demo/hello.c -c -o /demo/hello.obj --scatter /demo/hello.sct --vector-table
cc: /demo/hello.c -> /demo/hello.obj (target arm, -O1), -c
cc: scatter /demo/hello.sct
cc: vector table in the first 0x400 B
[scatter] parsed /demo/hello.sct
cc: pool 617152/4194304 B
cc: done in 93 ms

$ cc /demo/util.c -c -o /demo/util.obj --scatter /demo/hello.sct
cc: /demo/util.c -> /demo/util.obj (target arm, -O1), -c
cc: scatter /demo/hello.sct
[scatter] parsed /demo/hello.sct
cc: pool 559616/4194304 B
cc: done in 66 ms

$ cc -link /demo/hello.obj /demo/util.obj --scatter /demo/hello.sct --vector-table -o /demo/hello.bin
cc: link 2 obj -> /demo/hello.bin
cc: scatter /demo/hello.sct
cc: vector table in the first 0x400 B
cc: reloc-text on
[link] note: 41 duplicate compiler-runtime symbols merged (first definition kept)
[scatter] parsed /demo/hello.sct
cc: pool 524568/4194304 B
cc: done in 11 ms

$ app run /demo/hello.bin
[   43.839] <6>APPRUN:305 /demo/hello.bin: 5452 B -> code 0x24070000 (8192 B),
                                data/stack 0x24072000 (8192 B), task 4
app run: /demo/hello.bin started (task 4)
ark@arkrtos:~$ hello, world
built by arkc on the STM32H743, linked from two objects
[   43.840] <6>TASK:2584 task 4 exits (self kill)
[   43.840] <6>APPRUN:361 task 4 exited, window returned to the pool
```

三件事同时被证明：**分离编译**真的产出了两个对象（93 / 66 ms）、**链接器**真的
把第二个对象搬进了第一个对象的基址（`reloc-text on`，41 个运行时符号去重）、
`app run` 的窗口校验（SP=窗口顶、entry 在代码窗内、entry 处是序言）全部通过。

「两行都对」才是 `util.c` 真被执行了的证据：`write()` 按 `my_strlen()` 返回的
长度打字节，长度错一个字节就看得到。两行文字本身是 `hello.c` 里的字面量，
所以光看文字证明不了第二个翻译单元参与了。

## 3. B 段实录：`make`

```
$ cd /demo
ark@arkrtos:/demo$ make
make: /demo/hello.svcm <- /demo/hello.c (+more)
make:   src /demo/util.c
cc: pack 2 sources -> /demo/hello.svcm (4 passes, -O1)
cc:   A 5452 B
cc:   B 5452 B
cc:   C 5452 B
cc:   D 5452 B
cc:   A vector: SP 0x20002000 entry 0x800070E (rom base 0x8000000, window 8192 B)
cc: /demo/hello.svcm: payload 5452 + table 11 x 4 = 5752 B
cc:   reloc rom 1 / ram 1 / rom_movw 4 / ram_movw 5, entry +0x70E
cc: packed in 1522 ms
make: ok

$ mini run hello.svcm
[   99.991] <6>MINI:579 /demo/hello.svcm: 5452 B payload -> code 0x24070000 (8192 B block),
                                    RW/stack 0x24072000 (8192 B), task 4
mini running: task 4, code 5452 B in 8192 B @ 0x24070000, RW/stack 8192 B @ 0x24072000
  running 1 of 2 allowed
ark@arkrtos:/demo$ hello, world
built by arkc on the STM32H743, linked from two objects
[   99.992] <6>MINI:873 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
```

`make` 只做三件事：读工程文件、把 `arkmake_project_t` 摊成 `arkc_cc_pack()` 的
参数、把失败阶段翻成自己的错误码。**它没有第二条编译路径**——四遍差分打包的
本体就是 `cc --pack`，所以 `make` 与 A 段走的是同一个编译器、同一批源。

`makefile` 全文如下（`ark make` 只认这几个键）：

```
# /demo/makefile -- everything 'ark make' reads, nothing else.
# The build is 'cc --pack': four passes over these sources with four
# different link bases, diffed into one relocatable image.
TARGET = hello
OUT    = /demo/hello.svcm
SRCS   = /demo/hello.c /demo/util.c
```

**计时口径**：`packed in 1522 ms` 是两源 × 四遍的真空闲读数。此前同一操作实测过
`11124 ms`，那不是打包慢——当时有一个 `for(;;)` 的小程序以 `APP_TASK_PRIORITY`
（10，在控制台 14 之上）在跑，控制台只抢到零碎 CPU。修掉优先级之后同一命令是
1.5 s。凡是 `make` 的耗时读数，都要先确认没有更高的任务在占 CPU。

## 4. 为什么 `--scatter` 必须出现在 `-c` 上

这是本演示最容易被跳过、也最容易错得安静的一步。

`arkc` 在 **codegen 阶段**就把 `.text` / `.data` / `.bss` 的绝对地址烤进指令
（MOVW/MOVT 对），链接器不能像 ELF 工具链那样事后搬动。所以布局必须在**编译**
那一刻就知道，`-link` 只负责把"它搬动了哪个对象"造成的位移补回去（`reloc-text`
默认对 `-link` 打开）。

不带 `--scatter` 时：两个对象都按默认基址编（`0x08000000` / `0x20000000`），
链接**照样成功**，镜像也装得进去，然后第一次写全局变量就死在板子上：

```
*** PANIC MEM ***
R0=0x20000000 R1=0x20000040
CFSR=0x00000082 MMFAR=0x20000000 BFAR=0x20000000
```

`MMFAR=0x20000000` 就是"这块内存不在我这个 app 的窗口里"的直白读数。
判据因此写死：**两个 `-c` 都必须带 `--scatter <同一个 .sct>`**，
`final_app.py` 的 `hello-demo` 用例里三条命令逐字与屏幕上打印的一致。

## 5. 三个硬约束（每一个都踩过）

### 5.1 分散文件必须有 `*.o (RESET, +First)`

`arkc` 0.8.0 的散射校验（`ARKCC_FEATURE_SCATTER_CHECK`）在给了 `--vector-table`
时要求布局里存在装载 RESET 的执行区，且它落在镜像基址上。缺了直接拒绝出图：

```
arkcc: Error: the layout in use defines no RESET section, but --vector-table was
given: the vector table would not be the first object in the image
```

**这条不只影响演示**：`tools/final_app.py` 的 `SCT_OK`/`SCT_BAD` 与 `cc --pack`
自己写的 `/tmp/pp.sct` 也都要带这一行，否则 M1/M2 的板级用例第一步就失败。
三处已同步。

### 5.2 单执行区，不要 `ER_IROM` + `ER_IRAM` 两个段

`app run` 的装载器**只拷文件字节**，所以每一个"有初值"的字节都必须落在 8 KB
装载区里。写成两个执行区时 RW 段落在装载区外，镜像里根本没有那部分数据：链接
成功、装载成功、数据是零。演示版 `.sct` 因此是一个执行区、装载区 `0x4000`
（把初始 SP 顶到 `0x24074000`，正好是 `app run` 要求的窗口顶）。

### 5.3 ARM 后端目前不产全局/静态变量的初始化数据

`static const char msg[] = "..."` 里的字节在 ARM 目标上**不会**进镜像（对象里连
`ABC` 都搜不到），同样源在 `-target x86` 下有 `.data`。所以两份源码都用**内联
字面量**（`write(1, "hello, world\r\n", my_strlen("hello, world\r\n"))`），实测
落在镜像 `+0x400` 之后。这是编译器的真缺陷，不是写法偏好，记录在案待修。

## 6. 池与槽位预算（SDRAM 档）

| 读数 | 出处 |
|---|---|
| 编译 hello.c：`pool 617152/4194304 B`，93 ms | A 段实录 |
| 编译 util.c：`pool 559616/4194304 B`，66 ms | A 段实录 |
| 链接两对象：`pool 524568/4194304 B`，11 ms | A 段实录 |
| 片上 RAM 档（`USE_SDRAM=0`）池只有 80 KB，这套源报 `memory pool exhausted`，`pool 77704/81920 B` | 上一版实录 |
| 宿主用同一份预算复现同一失败：`[POOL] FAIL req=32768 used=77696 cap=81920` | `arkcc_min.exe` |

"池不够"不是猜的：宿主能逐字复现板上的失败点。改 `USE_SDRAM=1` 之后池从 80 KB
变成 4 MB，同一份源一次编过。SDRAM 的通路问题见
`docs/stm32h743-external-memory.md` 顶部的订正段（缺的是 `FMC_BCR1.FMCEN`）。

**ramfs 槽位**：`/demo` 一次要同时站着 4 个源/脚本 + 2 个对象 + 1 个 `.bin`
（A 段），再跑一次 `make` 再加 1 个 `.svcm`（B 段），正好 8 个。零余量的上限在
演示现场等于"随便多一个文件就全挂"，所以 SDRAM 档的槽数从 8 提到 11
（11 × 512 KB = 5.5 MB + 节点开销 < 6 MB 的 `SDRAMFS` 分区；12 槽正好 6 MB，
装不下结构体开销）。数字的由来写在 `GCC/Makefile` 的 `RAMFS_DEFS` 注释里。

## 7. 本轮修掉的四个真缺陷

| 缺陷 | 症状 | 修法 |
|---|---|---|
| `rd_le32` 在 16 位总线的 SDRAM 上被 gcc 合成非对齐宽读，返回 0 | `cc --pack` 卡在 `no usable vector table (-34)`：符号表读成 0 条 ⇒ `ld_find_global("Reset_Handler") < 0` ⇒ 向量表整段不写。更早一次直接 BUS FAULT（`BFAR=0x21744c9a`，PC 落在 `rd_le32`） | `src/compiler.c` 里改成 `const volatile unsigned char *` 逐字节读，禁止合成宽读 |
| 小程序任务优先级在控制台之上 | `mini run` 一个 `for(;;)` 的程序之后提示符再也不回来，只能复位；连带把 `make` 的耗时读数抬到 11 s | 演示工程默认 `MINI_PRIORITY=20`（= `ARKRTOS_APPRUN_PRIORITY`，分区头里有同一条实测注释）。内核默认不变，`make MINI_PRIORITY=` 复现旧行为 |
| `mini run` / `mini forget` 不认相对路径 | `cd /demo` 之后 `make` 能读到相对名 `makefile`，`mini run hello.svcm` 却报 `rc=-18`（`no such file or mount`） | `cmd_mini` 里先按 shell 的 cwd 解开路径（`shell_vfs_path`），绝对路径解出来仍是原样；故障计数也改用解开后的路径取键 |
| `-D` 变化不触发重编 | 改 `RAMFS_DEFS` 里的槽数后旧 `.o` 被当成最新，新上限编不进去 | ramfs 的两个数字进 `build/.ldcfg` 指纹（`ramfs=$(ARK_RAMFS_SIG)`），与 `USE_*` 同等对待 |

前两条是"读数会骗人"的典型：一条让编译器把自己的符号表读丢了，一条让**计时**
变成别的任务 CPU 占比的函数。

## 8. 门禁

`tools/final_app.py` 两条用例，走的都是上面这两段用户路径本身（不是测试专用
入口），逐条匹配进度行、两行输出、块回收，并禁止
`FATAL / app run failed / *FAULT`：

```sh
python tools/final_app.py --noflash --port COM3 --only hello-demo   # A 段
python tools/final_app.py --noflash --port COM3 --only make-demo    # B 段
python tools/final_app.py --noflash --port COM3                     # 全部 6 条
```

`hello-demo` 里屏幕打印的三条命令与三个 `Step` 逐字同形——否则"照屏幕敲"和
"这里跑的"就不是同一条命令，闸门证明的东西也就变了。

## 9. 还没解决的

- **ARM 后端不产全局/静态初始化数据**（§5.3）。演示源已经绕开（内联字面量），
  真实工程想要 `static const char *name = "x";` 就会拿到全零。修在编译器仓库，
  需要单独一轮。
- **`cc --pack` 的对象槽只有 16 KB**（与镜像缓冲共用）。当前两源 × 5452 B 的
  负载够用；再大的源要在 `SLOT_RAM_TOTAL=64 KB` 与 `app run` 常占的 16 KB 之间
  重新分配，不能简单调大。
