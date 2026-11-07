# arkc 板上直跑 M4：一条 `hello` 把 POSIX hello world 编出来跑起来

**这份文档记录演示路径本身**：串口上敲一个词，板子自己把源码写进文件系统、
用自己的编译器编成镜像、再装进运行窗口跑起来，打印两行字然后退出。

它不是"编译器能编 hello world"的又一次声明，而是**这条路径上每一个前置条件**
和它们各自为什么必需——以及实测读数。

---

## 1. 怎么跑

固件必须同时打开三个开关（`USE_HELLO` 依赖另外两个，`arkrtos_features.h`
里有 `#error` 兜底，关错了编不过而不是静默少功能）：

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j6 CROSS="<arm-none-eabi->" USE_SDRAM=1 USE_APPRUN=1 USE_HELLO=1
```

烧进去，然后在串口（USART1 / COM3 / 115200 8N1）上：

```
ark@arkrtos:~$ hello
```

## 2. 板上实录（2026-11-07，FK743M5-XIH6，build Nov  7 2026）

```
hello

[hello] 1/3 sources -> /tmp/hello.c, /tmp/hello.sct
[hello] 2/3 cc: compile + link for the app-run window
cc: /tmp/hello.c -> /tmp/hello.bin (target arm, -O1)
cc: scatter /tmp/hello.sct
cc: vector table in the first 0x400 B
[scatter] parsed /tmp/hello.sct
cc: pool 615792/4194304 B
cc: done in 134 ms
[hello] 3/3 app run /tmp/hello.bin
[   14.719] <6>APPRUN:305 /tmp/hello.bin: 3544 B -> code 0x24070000 (8192 B),
                                    data/stack 0x24072000 (8192 B), task 4
[hello] started as task 4; the program prints and exits on its own
hello, world
written by arkc running on the STM32H743 itself
[   14.719] <6>TASK:2584 task 4 exits (self kill)
[   14.720] <6>APPRUN:361 task 4 exited, window returned to the pool
```

三件事同时被证明：源文件真的写进了 VFS（`/tmp/hello.c` 1897 B）、编译器真的
在板上把 1897 B 的 C 编成了 3544 B 的镜像（134 ms）、`app run` 的窗口校验
（SP=窗口顶、entry 在代码窗内、entry 处是 arkc 序言）全部通过。

## 3. 三个硬约束（每一个都踩过）

### 3.1 分散文件必须有 `*.o (RESET, +First)`

arkc 0.8.0 的散射校验（`ARKCC_FEATURE_SCATTER_CHECK`）在给了 `--vector-table`
时要求布局里存在装载 RESET 的执行区，且它就落在镜像基址上。缺了就直接拒绝出图：

```
arkcc: Error: the layout in use defines no RESET section, but --vector-table was
given: the vector table would not be the first object in the image
```

**这条不只影响演示**：`tools/final_app.py` 的 `SCT_OK`/`SCT_BAD` 与 `cc --pack`
自己写的 `/tmp/pp.sct` 也都要带上这一行，否则 M1/M2 的板级回归用例会在第一步失败。
两处已同步修好。

### 3.2 单执行区，不要 ER_IROM + ER_IRAM 两个段

app run 的装载器**只拷文件字节**，所以每一个"有初值"的字节都必须落在 8 KB 装载区
里。写成两个执行区时 RW 段落在装载区外，镜像里根本没有那部分数据：链接成功、
装载成功、数据是零。演示版 `.sct` 因此是一个执行区、装载区 0x4000（把初始 SP 顶到
`0x24074000`，正好是 app run 要求的窗口顶）。

### 3.3 ARM 后端目前不产全局/静态变量的初始化数据

`static const char msg[] = "..."` 里的字节在 ARM 目标上**不会**进镜像（对象里
连 `ABC` 都搜不到），而同样的源在 `-target x86` 下有 `.data`。所以演示程序用
**内联字面量**（`write(1, "hello, world\r\n", sizeof(...) - 1)`），实测落在镜像
`+0x400`。这是编译器的真缺陷，不是本演示的写法偏好；记录在案待修。

## 4. 为什么必须先把 SDRAM 弄起来

片上 RAM 档（`USE_SDRAM=0`）给编译器的池是 80 KB，而这套 hello world 要 100 KB 以上：

| 读数 | 出处 |
|---|---|
| 板上报 `memory pool exhausted`，`pool 77704/81920 B` | 串口实录 |
| 宿主用同一份预算（`-DARKCC_PROFILE=STM32_MIN` + 板上那套 `-DARK_*`）复现同一失败，失败请求 32768 B | `arkcc_min.exe`，`[POOL] FAIL req=32768 used=77696 cap=81920` |
| SDRAM 档：`pool 615792/4194304 B`，编完 134 ms | 串口实录 |

所以"池不够"不是猜的：**宿主能逐字复现板上的失败点**，改 `USE_SDRAM=1` 之后
池从 80 KB 变成 4 MB，同一份源一次编过。SDRAM 的通路问题见
`docs/stm32h743-external-memory.md` 顶部的订正段（缺的是 `FMC_BCR1.FMCEN`）。

## 5. 门禁

`tools/final_app.py` 里新增用例 `hello-demo`，它走的就是上面这条用户路径
（不是测试专用入口），逐条匹配三步进度行、两行输出、任务退出与窗口回收，
并禁止 `FATAL / compile-lost / app run failed / *FAULT`：

```sh
python tools/final_app.py --noflash --port COM3 --only hello   # 1/1 PASS
```

## 6. 顺带修掉的两处"静默"

- **Makefile 不跟踪 `-D`**：`USE_SDRAM`/`USE_APPPACK` 这类开关只改 `-D`，
  make 会把旧 `.o` 当成最新的。实测两次后果：`.ark_buf` 仍留在片内 RAM
  （池还是 80 KB）、`USE_APPPACK=1` 却报 `option '--pack' is not supported`。
  现在开关值写进 `build/.ldcfg` 指纹文件，它参与 `.o` 与 `kernel.ld` 的依赖：
  切换开关一定重编、一定重生成链接脚本，值不变时 mtime 不动（不会无故全量重建）。
- **启动时验证 SDRAM**：`ARKRTOS_SDRAM_VERIFY_AT_BOOT` 默认由 0 改 1。原来
  "没验证就跳过 VFS"的后果是 `ls: no mount`——从控制台看不出根因在 SDRAM；
  现在启动就验证，失败会带着地址报出来。

## 7. 未解项（不在本演示路径上）

`cc --pack`（M2 四遍差分打包）在 arkc 0.8.0 同步之后失败在最后一步：

```
cc:   A 3134 B      (四个 pass 都编出来了)
cc:   A vector: SP 0x0 entry 0x0 (rom base 0x8000000, window 8192 B)
cc: pack failed: no usable vector table at payload +0/+4 (compile with --vector-table) (-34)
```

定位到的读数是：板上 `cc -link` 对同一个对象报 **0 syms**（`-v` 输出
`linked 1 objects -> ... (3134 bytes, 0 syms, 0 relocs)`），而宿主版本对同一个
源报 **43 syms** 并正常写入向量表。即链接期没有读到对象的符号表，于是
`ld_find_global("Reset_Handler") < 0`，向量表整段被跳过。一次更早的运行里同一处
直接 BUS FAULT（`BFAR=0x21744c9a`，`PC` 落在 `rd_le32`、调用方是符号查找）。
这条与 `hello`/M1 无关（两者都不走 `--pack`），需要单独查对象符号表在 32 位板上的
解析路径。
