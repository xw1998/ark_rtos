# QEMU 模拟器移植与验证

> 结论档位说明（全文档通用）：
> - **仅编译通过**：只验证到链接产出 ELF，未在目标上运行过。
> - **QEMU 实跑验证过**：在 QEMU 目标机上真正执行，有串口原始输出为证。
> - **未验证**：只做了设计，没有编译也没有运行。
>
> 本文所有「已通过」均指 **QEMU 实跑验证过**，不是「应该能跑」。

## 1. 为什么有这条线

主线的验证载体是 STM32F427 / STM32H743 板子。板子不在手上的时候，
架构相关的改动（trap 入口、上下文切换、系统调用 ABI、定时器）就没有
落地证据，只能靠读代码「推断正确」——这正是最难自查、也最容易出静默
错答案的部分。QEMU 提供的是一个**真实执行指令**的软件目标机：它不
验证外设驱动，但能验证 port 层最危险的那部分（入栈出栈顺序、mepc
回填、寄存器保存完整性、定时器重装、SVC 参数与返回通路）。

因此这条线的定位是 **port 层的执行级验算**，不是替代上板。

## 2. 环境

### 2.1 QEMU

- 版本：**QEMU 9.2.3**（MSYS2 mingw-w64 构建）
- 路径：`D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe`
  （同目录另有 `qemu-system-loongarch64.exe`）
- 安装来源：清华 MSYS2 镜像的 `mingw-w64-x86_64-qemu-9.2.3-2-any.pkg.tar.zst`，
  解压后补齐约 163 个依赖包，最终 `bin/` 下 136+ 个 DLL。

补齐依赖时踩到的两个坑（换机器复现时会再遇到）：

1. **`libnfs-14.dll` 必须来自 libnfs 5.0.3**。其它版本的 `libnfs-14.dll`
   导出符号不全，QEMU 启动即报 `0xC0000139`（入口点未找到）。
2. **`vulkan-1.dll` 不在 MSYS2 的 qemu 依赖树里**，需要单独补。

Git Bash 下缺 DLL 的表现是 **exit 127 且零输出**，看起来像「命令不存在」，
很容易误判成路径写错。定位方法：用 python `subprocess` 跑，读真实返回码：

| 返回码（十进制） | 十六进制 | 含义 |
|---|---|---|
| 3221225785 | 0xC0000139 | 找不到入口点（DLL 版本不对） |
| 3221225595 | 0xC000007B | 位宽不匹配（32/64 位 DLL 混用） |

进一步定位缺哪个依赖：用 `pefile` 把 QEMU 主程序的 import 表与 `bin/`
下所有 DLL 的 export 表对比，取差集。

### 2.2 RISC-V 工具链

- 路径：`D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe`
- 版本：**Xuantie-900 V2.6.1 / GCC 10.2.0**
- **关键约束：该工具链只有三个 multilib**

  ```
  rv32imafc   / ilp32f
  rv32imafdc_xtheade / ilp32d
  rv64imafdc_zfh_xtheadc / lp64d
  ```

  没有 `rv32imac` / `ilp32` 的 multilib，因此：
  - **不能链接 libgcc**（会去找不存在的 multilib）——链接命令里显式不加 `-lgcc`；
  - **不能链接 libc**——所有用到的 libc 函数必须自己提供（见 §3.6 `libc_shim.c`）；
  - **禁止 64 位乘除**——`__udivdi3` / `__divdi3` 来自 libgcc，一旦生成就是
    链接期 `undefined reference`。时间换算因此全部写成 32 位整数式。

  编译用 `-march=rv32imac -mabi=ilp32` 是允许的（只影响编译，不影响 multilib
  选择），只要不碰链接库。

### 2.3 LoongArch 工具链

- 路径：`D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe`（GCC 8.3.0）
- 这两个工具链的可执行文件是 **32 位 PE**，因此 i686 运行库（`libgcc_s_dw2-1.dll`
  等）必须复制到 **`bin/` 与 `libexec/gcc/<target>/8.3.0/` 两处**。
  只放 `bin/` 时，`cc1` 是子进程、走的是 libexec 目录，表现为 **exit 127 /
  0xC000007B / cc1 静默失败**（编译器主程序能打印版本，实际编译却没有任何输出）。

## 3. RISC-V RV32 移植（已实现，QEMU 实跑通过）

### 3.1 目录落点

```
kernelsrc/port/riscv/rv32imac/
    arkrtos_port.c          端口实现（中断、定时器、原子、栈初始化）
    arkrtos_context.S       上下文切换与 trap 入口
board/qemu_riscv32/
    arkrtos_board.c         16550 UART 控制台 + 设备初始化钩子
    arkrtos_board_config.h  板级开关（含 CHIP_* 物理参数覆盖）
example/qemu_riscv32/
    main.c                  自检例程（A/B 任务 + SVC 通路校验）
    start.S                 复位入口、BSS 清零、跳 main
    link.ld.in              链接脚本模板
    gen_ldconsts.py         从分区头求值出纯数值头
    libc_shim.c             最小 libc（printf 家族 + 内存函数）
    Makefile
```

### 3.2 运行模式

| 项 | 取值 | 说明 |
|---|---|---|
| 特权级 | 单 **M 模式** | `USE_MPU=0`、`USE_PRIV=0` |
| hart | 1 | `USE_SMP=0` |
| FPU | 关 | `USE_FPU=0` |
| 系统调用号 | **a7** | `ARKRTOS_ARCH_SVC_NUM_BITS = -1`（`ecall` 无立即数字段） |
| 参数 / 返回 | a0~a2 / a0 | 与 Linux RISC-V ABI 同约定 |

### 3.3 trap 帧

trap 帧 **144 字节（16 对齐）**，由 `tp` 指向。布局：

| 偏移 | 内容 | 偏移 | 内容 |
|---|---|---|---|
| 0 | ra | 68 | s2 |
| 4 | sp | … | s3~s11、t3~t6 |
| 8 | gp | 120 | t6 |
| 12 | tp | 124 | mepc |
| 16 | t0 | 128 | mstatus |
| 20~64 | t1,t2,s0,s1,a0~a7 | 132~140 | 填充 |

索引常量：`RV_FR_A0=9`、`RV_FR_A7=16`、`RV_FR_MEPC=31`、`RV_FR_MSTATUS=32`、
`RV_FR_WORDS=36`。

trap 入口的保存顺序是 **t0 先存，恢复时 sp → t0 → tp 最后**——因为
`tp` 同时是帧指针，`sp` 是栈指针，任何在保存它们之前就使用它们的指令
都会破坏现场。

`ecall` 后 **`mepc += 4` 由 port 负责**（不依赖硬件行为），否则返回后
会重复执行 `ecall` 形成死循环。

### 3.4 任务切换与定时器

- 切换机制照 PendSV：`switch_task` 只置 `CLINT_MSIP = 1`；若 trap 内
  `MIE=0` 则挂起，`mret` 后立刻进软中断完成切换。
- `mstatus` 初值 `0x1880`。
- `start_timer(us)` = `us * 10` counts（10 MHz timebase，约分整数式，无 64 位除法）。
- `atomic_cas` 用 `lr.w / sc.w`（`-cpu rv32` 带 A 扩展，实测是真原子）。

**中断使能顺序（本轮修掉的关键 bug）**：

```
irq_init:  csrw mtvec → mtimecmp = 0xFFFFFFFF（清挂起）
           → 只 csrs mie, MSIE
           → csrsi mstatus, 8
start_timer / start_timer_counts: 末尾 csrs mie, MTIE
```

原因：CLINT 的 `mtimecmp` **复位值是 0**，只要 `MTIE` 与 `MIE` 同时为 1，
在打开 `MIE` 的那一瞬间就会立刻进 timer trap。现象是「打印完横幅后一步
都不输出，像死机」。因此把「节拍何时生效」与「中断何时能进」都绑定到
调度启动点。

### 3.5 地址与链接

QEMU `virt`（rv32）实测参数：

| 项 | 值 |
|---|---|
| RAM base | **0x80000000** |
| RAM size | 128 MB |
| timebase | 10 MHz |
| CLINT | 0x02000000（msip +0x0 / mtimecmp +0x4000 / mtime +0xBFF8） |
| UART | 16550 @ 0x10000000 |

地址仍只在 `config/arkrtos_partition.h` 定义一次。QEMU 属于「把 RAM 当
Flash 用」的平台，因此 `CHIP_*` 六个宏都用 `#ifndef` 包住，由
`board/qemu_riscv32/arkrtos_board_config.h` 覆盖：

```
CHIP_FLASH_BASE 0x80000000   CHIP_FLASH_SIZE 0x00100000
CHIP_RAM_BASE   0x80100000   CHIP_RAM_SIZE   0x00800000
SHARE_RAM_BASE  0x80100000   SHARE_RAM_SIZE  0x00002000
KERNEL_RAM_BASE 0x80102000   KERNEL_RAM_SIZE 0x007EE000
```

（`gen_scatter.py` 只收集 `#define`、忽略 `#ifndef`，所以 STM32 各工程的
`.sct` 逐字节不变。）

**链接脚本不能直接包含分区头**：`arkrtos_partition.h` 含 C 编译期断言
（`typedef char ...`），喂给 `ld` 会报 `syntax error`。做法是用
`example/qemu_riscv32/gen_ldconsts.py` 复用 `tools/gen_scatter.py` 的
`parse_defines()` / `MacroEval` 求出数值，生成只含 `#define NAME 0x...`
的 `build/qemu_riscv32/ldconsts.h`，链接脚本 `#include "ldconsts.h"`。

链接命令：

```
gcc -march=rv32imac -mabi=ilp32 -mcmodel=medany -mno-relax \
    -T link.ld -Wl,--gc-sections -nostdlib -nostartfiles -o app.elf $(OBJS)
```

注意 **没有 `-lgcc`、没有 libc**。

### 3.6 构建期三个坑

1. **板级宏必须 `-include arkrtos_board_config.h`**。
   `#include ARKRTOS_BOARD_CONFIG` 只覆盖「先含 config.h 再含 features.h」
   的 TU；任何直接 `#include "arkrtos_features.h"` 的 TU 会拿到 autoconf
   的默认值（1），实测导致 VFS/LOG/SHELL/MDK_TRACE 全变成 1 并连锁出一堆
   `undefined reference`。
2. **`arkrtos_trace.h` 无条件 `#include "mdk_trace.h"`**，因此必须加
   `-Ikernelsrc/components/mdk_trace`。
3. **`arkrtos_fs.c` 需要 LittleFS 裁剪宏**：
   `-DLFS_NO_MALLOC -DLFS_NO_ASSERT -DLFS_NO_DEBUG -DLFS_NO_WARN -DLFS_NO_ERROR`，
   否则 `lfs_arkrtos_config.h` 里的 `#error` 会触发。
4. **`-MMD -MP` + `-include $(OBJS:.o=.d)`**：不加时改板级宏不会触发重编，
   会出现「头文件里关掉了某功能，.o 里还引用着对应符号」的 `undefined reference`。

另外两条 MinGW make 的脾气：

- `make 3.82`（`/d/mingw/MinGW/bin/make`）**不认 `/d/...` 路径**，Makefile
  里必须用 `D:/...` + `.exe` 后缀。
- 对「不含 shell 元字符」的命令它走 CreateProcess 快路径，而本机 python 装在
  含非 ASCII 的目录下会解析失败。生成 `ldconsts.h` 的 recipe **末尾加 `;`**
  强制走 shell。

### 3.7 libc 垫片

`example/qemu_riscv32/libc_shim.c` 提供：

- `memcpy/memset/memcmp/memmove/strlen/strcmp/strncmp/strcpy` 等内存与字符串函数；
- `vsnprintf` / `vsnprintf`（支持 `-0+ #` 标志、width、precision、`l/h/z/t`
  长度修饰、`%d %i %u %x %X %o %c %s %p %%`）；
- 浮点格式化一律返回 -1 并清空缓冲（明确不支持，不假装支持）；
- 返回值遵循 C99「本应写入的长度」（截断时返回需求长度，不是实际写入长度）。

### 3.8 启动顺序与 SVC 通路

启动顺序与 `kernelsrc/include/arkrtos_init.h` 一致：

```
board_init → cfg_load → task_register(A/B) → irq_init
           → kernel_module_init → sem_create_internal → start_idle
```

**内核任务必须用 `*_internal` API**（`arkrtos_sem_create_internal` /
`arkrtos_sem_wait_internal` / `arkrtos_task_wait_internal`）。`arkrtos_sem_create`、
`arkrtos_task_wait`、`arkrtos_get_time_ms` 是 **App 侧 SVC 包装**（定义在
`kernelsrc/sdk/app_sdk/arkrtos_oslib.c`），调度器启动前调用 `sem_create`
实测返回失败（`[boot] sem_create FAILED`）。

SVC 通路另用 `arkrtos_get_time_ms()` 每轮自检：把它返回的毫秒与内核侧
`tick * ARKRTOS_TICK_PERIOD_US / 1000` 比对，差 >2 ms 就打 `SVC_MISMATCH`。
这条自检覆盖的是「App 包装 → ecall → trap 帧 a7 → 内核分发 → a0 回填 →
用户态继续执行」的完整链路。

### 3.9 实测结果

尺寸（`riscv64-unknown-elf-size`）：

```
   text    data     bss     dec     hex  filename
  42816      48   15968   58832    e5d0  ark_qemu_rv32.elf
```

运行命令：

```bash
cd build/qemu_riscv32
"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe" \
  -machine virt -cpu rv32 -bios none -kernel ark_qemu_rv32.elf \
  -nographic -serial file:out.txt
```

（`-serial file:` 在 riscv32 上可以给绝对路径，loongarch64 上不行，见 §5。）

串口原始输出（节选，未经修饰）：

```
[Ark RTOS] board: qemu_riscv32 (virt, rv32imac, 1 hart)
[Ark RTOS] arch=riscv clint=0x02000000 timebase=10000000Hz
[boot] sem=19922944 taskA=1 taskB=2 tick_period_us=500
[A] round=0 tick=3 d_tick=3 ms=1 d_ms=1
 ms_svc=2 svc_ok
[B] woke, round=1 tick=8
[A] round=1 tick=1007 d_tick=1004 ms=503 d_ms=502
 ms_svc=503 svc_ok
[B] woke, round=2 tick=1009
[A] round=2 tick=2008 d_tick=1001 ms=1004 d_ms=501
 ms_svc=1004 svc_ok
[B] woke, round=3 tick=2008
[A] round=3 tick=3008 d_tick=1000 ms=1504 d_ms=500
 ms_svc=1504 svc_ok
[B] woke, round=4 tick=3009
[A] round=4 tick=4008 d_tick=1000 ms=2004 d_ms=500
 ms_svc=2004 svc_ok
```

可读出的三件事：

1. **节拍换算正确**：1000 tick ↔ 500 ms（`d_ms` 恒 500/501/502），
   `ARKRTOS_TICK_PERIOD_US=500` 生效。
2. **调度正确**：A 每轮 `task_wait(500ms)` 后醒来，B 由信号量唤醒，
   A/B 交替且 tick 单调。
3. **SVC 通路正确**：`ms_svc` 与内核换算值一致（`svc_ok` 全绿），说明
   `ecall` 的 a7 取号、a0 回填、`mepc+4` 都工作正常。

## 4. 已知限制（明确不覆盖的部分）

| 限制 | 影响 | 原因 |
|---|---|---|
| 无 U 模式 / 无 PMP | 测不出特权级隔离、App 越权 | 单 M 模式 port，`USE_PRIV=0` |
| handler 与任务共用栈 | 栈溢出行为与真实中断不同 | 未做中断独立栈 |
| trap 不可嵌套 | 不在中断里再开 `MIE` | 简化实现 |
| `ecall` 后 `mepc+=4` 由 port 做 | 换核/换编译器时须复核 | 不依赖硬件行为 |
| 无 FPU 上下文 | 浮点任务不可用 | `USE_FPU=0` |
| 单 hart | SMP 路径未验证 | `USE_SMP=0` |
| QEMU 不模拟真实外设 | 驱动层仍需上板 | 只有 16550 UART + CLINT |

## 5. LoongArch（未开始）

已实测的 QEMU loongarch64 `virt` 参数（来自环境探测，**port 尚未实现**）：

| 项 | 值 | 备注 |
|---|---|---|
| RAM base | **0x0** | size 128 MB |
| 低 1 MB | boot_info 占用 | **链接基址必须 ≥ 0x200000** |
| UART | ns16550a @ **0x1FE001E0** | 不是 0x10000000 |
| `-bios none` | **不接受** | 会报 `Could not find ROM image 'none'` |
| `-serial file:` | 只接受相对路径 | 绝对路径静默失败 |

移植计划：

```
kernelsrc/port/loongarch/la64/{arkrtos_port.c, arkrtos_context.S}
board/qemu_loongarch64/{arkrtos_board.c, arkrtos_board_config.h}
example/qemu_loongarch64/{main.c, start.S, link.ld.in, gen_ldconsts.py, libc_shim.c, Makefile}
```

要点：

- 系统调用走 `syscall` 指令，CSR 用 `ERA`（返回地址）、`PRMD`（特权/中断
  状态）、`ESTAT`（异常状态）、`ECFG`（中断配置）；
- 定时器走 `TCFG`/`TVAL` 或稳定计数器；
- **LA64 指针是 64 位，而内核大量用 `uint32` 存地址**——port 里必须显式
  截断/回填，并在文档里写清楚哪些位置做了这件事，否则会出现「高 32 位被
  悄悄丢掉、访问到别名地址」这类静默错答案；
- Makefile / `gen_ldconsts.py` 完全复用本轮的套路。

## 6. 复现步骤

```bash
cd "D:/工作/git_project/ark rtos/example/qemu_riscv32"
make -j4            # 产出 ../../build/qemu_riscv32/ark_qemu_rv32.elf
make size           # 打印 text/data/bss
make run            # 启动 QEMU，串口写入 build/qemu_riscv32/out.txt
```

`make run` 会一直跑（例程是无限循环），Ctrl-C 结束，然后 `cat out.txt` 看输出。
