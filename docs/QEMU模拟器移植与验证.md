# QEMU 模拟器移植与验证

**[简体中文](QEMU模拟器移植与验证.md) | English([QEMU模拟器移植与验证.en.md](QEMU模拟器移植与验证.en.md))**

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
- **LA32 不在这个环境里**：公版 QEMU 9.2.3 与上游都没有
  `qemu-system-loongarch32`；龙芯教育的 LA32 分支只有 Linux 发行包，
  本机无法执行（§6.1）。

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
- LoongArch32 用另一套工具链：`D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-gcc.exe`
  （同为 GCC 8.3.0），必须用 `-march=loongarch32 -mabi=ilp32d`。
  它**没有可运行的 QEMU 目标**（见 §6），只用于编译/链接验证。

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
  46544      48   22128   68720  10c70  ark_qemu_rv32.elf
```

（含 §3.10 的 U 模式 + PMP + 独立故障栈；开 U 模式之前的单 M 模式版本是
`42816 / 48 / 15968`。）

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

### 3.10 U 模式 + PMP 内存保护（实跑取证）

与 §3.9 同一次构建：`USE_MPU=1`、`USE_PRIV=1`，内核注册一个 `is_priv = 0`
的任务 C，它运行在 **U 模式**。四条负对照各验一条，每条都必须在 U 模式
报错、并由内核干净重启 —— 这是"保护机制真的生效"唯一的现场证据。

| # | 探针 | 预期 trap | 实测 |
|---|---|---|---|
| 1 | 读 CLINT `0x02000000`（不在任何 PMP 窗口） | `mcause=5` load fault，`mtval`=目标地址 | `mcause=5 mtval=0x02000000 mepc=0x8000a248 from=U task=3` |
| 2 | 打印（UART 4 KB 窗口，`ARKRTOS_MPU_PERIPH_BASE/SIZE`） | 必须**成功** —— 否则"什么都没给所以什么都没跑" | `[U]` 每行都打出来了 |
| 3 | `ecall` 取毫秒（`arkrtos_get_time_ms()`） | 必须**成功**，`ecall_u` 递增、`ecall_m` 不动 | `ecall_u` 1→5，`ecall_m` 恒 1 |
| 4 | 写 CSR `csrsi mstatus, 8` | `mcause=2` 非法指令，`mtval`=指令编码 | `mcause=2 mtval=0x30046073 mepc=0x8000a21e from=U task=3` |

串口原始输出（U 任务部分，30 s 宿主时间，未删行）：

```
[boot] sem=19922944 taskA=1 taskB=2 taskU=3 mpu=1 priv=1 tick_period_us=500
[U] round=1 t=4 ms ecall_u=1 ecall_m=1
[U] reading CLINT (outside every PMP window) ...

[PMP] fault mcause=5 mtval=0x02000000 mepc=0x8000a248 from=U task=3
[U] round=2 t=5 ms ecall_u=2 ecall_m=1
[PMP] fault mcause=5 mtval=0x02000000 mepc=0x8000a248 from=U task=3
[U] round=3 t=6 ms ecall_u=3 ecall_m=1
[PMP] fault mcause=5 mtval=0x02000000 mepc=0x8000a248 from=U task=3
[U] round=4 t=7 ms ecall_u=4 ecall_m=1
[U] writing mstatus (privileged CSR) ...
[PMP] fault mcause=2 mtval=0x30046073 mepc=0x8000a21e from=U task=3
[U] round=5 t=8 ms ecall_u=5 ecall_m=1
[A] round=1 tick=1004 d_tick=1002 ms=502 d_ms=501
```

四件事同时成立：4 次 `from=U` 的 trap、4 次干净重启（round 1→5 连续）、
`ecall_u` 单调增且 `ecall_m` 不动（= 任务确实在 U 模式）、
`UNEXPECTED` / `FATAL` / `0xcdcdcdcc` 三者计数均为 0。

#### 3.10.1 实现要点

- **特权传递走 trap 帧的 MPP 位，不是一个全局变量**：`riscv_trap_c()` 开头
  `g_rv_next_mpp = (frame[RV_FR_MSTATUS] >> 11) & 3`，任务切换时由
  `arkrtos_port_set_thread_priv()` 覆盖；`trap_entry` 与
  `arkrtos_port_resume_task` 两条恢复路径都在 `csrw mstatus` 前把 MPP 位
  换成 `g_rv_next_mpp`。不这么做，一次不切换的 tick 中断会把 U 模式任务
  抬回 M 模式。
- **PMP 用 8 个 entry 的 pmpcfg0/pmpcfg1**：attr `R=1 W=2 X=4 A=[4:3]`，
  `pmpaddr = (base>>2) | ((pow2>>3)-1)`（NAPOT）。写序必须是
  「先写 cfg=0 关掉 → 写 pmpaddr → 一次性写 cfg」，否则会出现半个窗口
  的中间态。M 模式的 entry 一律 `L=0`，所以内核自己不受限。
- **故障走独立栈**，见下一条根因。

#### 3.10.2 三个实测踩出来的缺陷（都不是风格问题）

**(a) trap 内真的开了 MIE ⇒ 嵌套 trap ⇒ 任务静默跑成 M 模式**

内核里有几十处 `ARKRTOS_ENABLE_IRQ()` 位于可能从 trap 里被调到的函数。
在 Cortex-M 上 `PendSV` 与 `SVC` 同优先级不会互相打断，所以这没毛病；
RISC-V 上 `csrsi mstatus, 8` 在 trap 里是真的打开全局中断，于是：
定时器在 SVC 处理程序内部再次进 trap → 嵌套的 `riscv_trap_c` 把
`g_rv_next_mpp` 改写成"来源特权 M" → 外层 mret 后任务跑成 M 模式。

症状非常隐蔽：**不崩、不报错，只是 ecall 从 `mcause=8` 变成 `mcause=11`**，
并且 U 任务突然能读 CLINT 了。定位靠的是把"来源"变成可读数字：
`g_rv_ecall_u` / `g_rv_ecall_m` 两个计数器，前者涨、后者也涨就是它。
QEMU `-d int` 的 trace 直接指认 `m_timer epc=0x800096b0`（正是
`arkrtos_port_enable_irq` 的 `ret`）。

修法：`arkrtos_port_enable_irq()` 在 `s_in_trap != 0` 时**直接忽略请求**
（改内核几十个调用点不可能，也违反移植层职责）；`riscv_trap_c()` 再加一道
`nested` 保护，嵌套时既不切任务也不改写 `g_rv_next_mpp`。

**(b) 故障重启会重填任务栈，而处理程序就跑在同一个栈上 ⇒ `0xcdcdcdcc` 递归**

内核的故障恢复 `arkrtos_task_recover_pending()` 会调
`arkrtos_task_stack_init()` 把那个任务的整段栈重填成
`ARKRTOS_STACK_FILL_PATTERN = 0xcdcdcdcd` 再重建入口帧 —— 而本移植没有
独立 handler 栈，处理程序自己的活动帧正在那段区间里。填完一返回，从栈上
取回的 `ra` 就是 `0xcdcdcdcd`，`jalr` 清 bit0 后 PC 跳进 `0xcdcdcdcc`，
再取指又 fault，递归下移。QEMU 上实测每轮 **8 层
`fault_fetch epc=0xcdcdcdcc`**，每层下移 0xF0 = 240 字节（正好是一个
36 字的 trap 帧），8 层后叠到栈外去。

修法：故障分支在任何内核调用之前先 `rv_fault_enter()` 换到独立故障栈
（`rv_fault_stack`，默认 256 字 = 1 KB，板级可用
`ARKRTOS_RV32_FAULT_STACK_WORDS` 覆盖），再 tail 进
`arkrtos_port_fault_dispatch()`。任务栈顶部 144 字节的重建本身是安全的，
要搬走的只是"执行处理程序的那段栈"。修完 `fault_fetch` 计数为 0。

**(c) U 模式执行特权指令是 `mcause=2`，不是 5/7**

U 模式碰 CSR（U 任务误调内核内部函数，比如 `arkrtos_port_disable_irq()` 的
`csrrc mstatus`）会得到非法指令异常 `mcause=2`。最初只把 1/5/7 归入越权，
结果是 `[Ark RTOS] FATAL: mcause=0x00000002` 直接停机 —— **保护机制自己把
内核搞死了**。现在 `mcause=2` 只在来源是 U 时按越权处理（与 PMP 拒绝同一个
出口），来源是 M 时仍然是致命错误：M 模式吃到非法指令是移植层/内核的真
bug，不能拿"重启任务"掩掉。

#### 3.10.3 示例任务的两条写法约束（也是实测的）

U 任务尾部不能这么写：

```c
for(;;) { arkrtos_task_delay(200000u); }        /* 错：关中断忙等 */
for(;;) { arkrtos_task_wait_internal(1000u); }  /* 错：U 模式碰 CSR */
```

- `arkrtos_task_delay()` 是 SVC → `arkrtos_port_delay_us()` 忙等。SVC 在
  trap 里执行，此时 `mstatus.MIE = 0`，忙等全程关中断。实测 200 ms 忙等
  死循环让节拍只按 ~5 Hz 前进、A/B 永远排不上（8 s 宿主只跑出 18 个 tick，
  看起来像"QEMU 时间不走"）。
- `arkrtos_task_wait_internal()` 是内核内部函数，开头 `ARKRTOS_DISABLE_IRQ()`
  就是 `csrrc`，U 模式一执行就是 (c) 那条。

正确写法是 App 侧 SVC 接口：`arkrtos_task_wait(1000u)` —— 真正阻塞
（挂延时链 + 触发切换），中断开着。

#### 3.10.4 隔离的是什么，不是什么

这一版隔离的是**特权级**（U 碰不到窗口外地址、改不了 CSR），**不是任务间
隔离**。任务 C 由内核注册（`rom_start = 0`），拿到的是内核代码窗 + 整片
内核 RAM，所以它写内核 RAM 是合法的 —— 负对照必须挑 CLINT 这种不在任何
窗口里的地址才成立。任务间隔离要等 App 镜像走 loader 拿到自己的 slot。

### 3.11 中断驱动串口（PLIC，RV32）

§3.9/§3.10 的串口是轮询的：`arkrtos_board_console_putc()` 先等 THRE
再写。这一节把接收侧改成**中断驱动**，并让中断处理程序去唤醒一个任务——
这是「中断 → 内核调度」这条链在 RISC-V 上的第一条现场证据。

#### 3.11.1 PLIC 接线（port 层）

QEMU `virt` 的 PLIC 在 `0x0C000000`，取号方式与 16550 的 IRQ 号（10）无关，
是板级 `ARKRTOS_QEMU_UART_IRQ` 告诉 port 的。寄存器布局：

| 偏移 | 用途 |
|---|---|
| `+4*id` | 该中断源优先级 |
| `+0x2000` | context 0 使能位图（bit `id`） |
| `+0x200000` | context 0 阈值 |
| `+0x200004` | context 0 claim / complete（读写同一个地址） |

port 只暴露两个函数：`arkrtos_port_plic_enable(irq, prio)` /
`arkrtos_port_plic_disable(irq)`。**没有通用外部中断注册 API** ——
`arkrtos_hal.h` 里本来就只有 `disable_irq/enable_irq/irq_init`，硬加一层会把
port 的边界搞糊。板级用弱钩子接：port 声明并弱实现
`void arkrtos_port_external_isr(uint32 irq)`，板级覆盖它，进来自己按 `irq` 分发。

`arkrtos_port_irq_init()` 里 `csrs mie` 从 `MSIE` 扩到 `MSIE|MEIE`，
`riscv_trap_c()` 的 IRQ 分支新增 `cause == 11 → rv_plic_dispatch()`。

**claim 上限 8**：`rv_plic_dispatch()` 最多 claim 8 次。电平触发的中断如果
ISR 没把源摘干净（比如 IIR 里还有待读字节），claim 会一直返回同一个 id，
一个中断风暴就能把 tick 饿死。宁可晚一拍，也不丢节拍。

**中断里不做切换**：ISR 只调 `*_from_isr()` 置软件中断，真正的换栈由
`mret` 之后的 `m_software` 完成——和 Cortex-M 上「ISR 里只投 PendSV」的时序
是同一个道理。trap 内直接切会把外层帧丢掉（§3.10.2(b) 同一个坑）。

#### 3.11.2 板级：16550 接收 + 256 B 环形缓冲

- `arkrtos_board_uart_irq_init()`：清状态、读空 DR、`IER = RDA|RLS`、
  再 `arkrtos_port_plic_enable(10, 1)`。顺序必须是「先配 UART 再放开 PLIC」，
  反了会先吃一次中断再进去配中断源。
- `uart_isr()` 按 IIR 的 ID 分支（RDA `0x04` / RTO `0x0C` / LSR `0x06`），
  读空 DR，环形缓冲满则 `s_rx_dropped++`，每读到一字节调
  `arkrtos_board_uart_rx_hook(byte)`。`guard < 64` 防 IIR 读不干净时的死循环。
- 弱钩子 `arkrtos_board_uart_rx_hook()` 默认空实现。示例里它只做
  `arkrtos_sem_post_from_isr(s_sem_uart)`。
- 读数接口：`arkrtos_board_uart_rx_read(buf, max)`、
  `..._isr_count()` / `..._rx_bytes()` / `..._rx_dropped()` / `..._unknown_irq()`。
  把这几个数打进一行日志，等于把「中断真的发生了」变成可读数字，
  而不是靠「好像有输出」。

#### 3.11.3 实测读数

```
[boot] sem=19922944 taskA=1 taskB=2 taskU=3 mpu=1 priv=1 taskRX=4 tick_period_us=500
[boot] uart irq on, send bytes to see [R] echo
[R] wake=1 n=6 isr=2 rx=6 drop=0 -> hello.
```

`isr=2 rx=6`：两次 PLIC 中断搬完 6 个字节（第一次拿到 5 字节 `hello`，
第二次拿到 `\n`）；`wake=1` 说明信号量 post 把它从阻塞里叫醒了；
`drop=0` 说明缓冲够。回显 `-> hello.` 里的 `.` 是 `\n` 按不可打印字符的
写法。

#### 3.11.4 端到端自动检查（`example/qemu_riscv32/uart_irq_check.py`）

`-serial file:` 是**单向**的，验证不了接收。脚本改用 TCP chardev：

```
-chardev socket,id=c0,host=127.0.0.1,port=<动态空闲端口>,server=on,wait=on
-serial chardev:c0
```

判据四条：① 出现 `[boot] uart irq on`；② 发 `hello\n` 后出现 `[R] wake=`
且含 `-> hello`；③ 该行 `isr>0` 且 `rx>0`；④ 全程无 `UNEXPECTED` / `FATAL`。
实测 `PASS: 中断驱动串口端到端通`。

三个坑：

1. **`wait=off` 会漏掉启动横幅**：Python 连上时 guest 早已跑完那两行。
   必须 `wait=on`，让 QEMU 等 socket 连上再启动。
2. **`-kernel` 不能给含中文的绝对路径**：QEMU 在 Windows 上打不开且
   **不报错**（表现为起不来、socket 连不上）。必须 `cwd=build目录` + 传相对文件名。
3. **回显行不按行首出现**：`[R]` 那行会插在任务 A 正在打印的行中间
   （实测 `[A] [R] wake=1 ... -> hello.`），按行首匹配永远找不到。
   改成 `log.find("[R] ")` 子串切到行尾。

### 3.12 软件功能全开（RV32 全功能档：已实跑通过）

目标是把 RV32 从「port 层最小验证」推到与 ARM 板级同档的**全功能**：块设备 + FS + VFS
+ procfs + 日志 + 内核 Shell + 设备树。配置（`board/qemu_riscv32/arkrtos_board_config.h`）：
`USE_BLK/USE_FS/USE_VFS/USE_VFS_LFS/USE_PROCFS=1`、`ARKRTOS_USE_SHELL=1`、
`ARKRTOS_USE_LOG=1`、`ARKRTOS_USE_LOG_RING=1`、`ARKRTOS_PLATFORM_REBOOT_PORT=1`。

板级新增：`drvblk.c`（RAM-NOR 512 KB，擦除粒度 4 KB，**写前必须为 0xFF 否则拒写**并计
`rejected`）、`drvuart.c`（`SHELL_ENABLE` 前后两套设备 ops + 桩）、`arkrtos_dt.c` +
`dts/qemu_riscv32.dts`（dtc_lite 生成的 9 节点）。

**实跑通过（QEMU 真跑，`[FS]` 段逐行读数）**：`ram0` format/mount/写/读逐字节比对一致
（`write rc=0 read rc=0 len=17 -> match`）、`ls` 出 `hello.txt`、`/proc/version` 可读、
不擦除覆写被拒（`rejected=1`）、计数器段 `written=1024 rejected=1 erases=2`、`[FS] PASS`；
D-2 的 U 模式 PMP 负对照仍在（3× `mcause=5 mtval=0x02000000`、1× `mcause=2
mtval=0x30046073`，`from=U task=3`）。
尺寸：text 171296 / data 112 / bss 582672 / dec 754080。

#### 3.12.1 随机 M 模式故障：根因与本轮修复

**现象**：同一镜像约 40%~60% 的启动里 `[FS] format rc=-1 last_error=-1004`，故障点是
`mcause=5 mtval=0x00000004 from=M task=5`，`mepc` 落在 `lfs_dir_commitattr` 的
from-disk 分支（`const struct lfs_diskoff *disk = buffer;` 解引用 `NULL`）。

**根因（已闭环）**：`g_fs.stream.cfg` 悬空。

1. `arkrtos_fs_open_write()` → `fs_stream_open()` 里 `struct lfs_file_config fcfg;` 是
   **栈局部变量**，`fcfg.attr_count = 0` 写得好好的；
2. `lfs_file_opencfg()` 不复制 config，只存指针：`file->cfg = &fcfg`（`g_fs.stream` 是
   **跨调用存活**的对象，config 是**不跨调用存活**的栈变量）；
3. `fs_stream_open()` 返回后该指针指向已弹出的栈帧；`write_next` / `close_write` 的更深
   调用把同一段栈区覆盖成自己的局部量与返回值；
4. `close_write → lfs_file_close → lfs_file_sync_` 里
   `LFS_MKATTRS({...}, {LFS_MKTAG(LFS_FROM_USERATTRS, file->id, file->cfg->attr_count),
   file->cfg->attrs})` 读到的 `attr_count` 已是别人的残留值（本轮实测 **`0xffffffac` =
   `-84` = `LFS_ERR_CORRUPT`**，即某次调用的返回值）；
5. `LFS_MKTAG` 只做 `type<<20 | id<<10 | size` 的移位或、**不掩码**，垃圾高位把 tag 的
   bit31 带上来；bit31 是 traverse 内部给 from-disk 用的标记，于是这条「用户属性」被当成
   from-disk 项，`buffer`（`cfg->attrs == NULL`）被当 `lfs_diskoff*` 解引用 ⇒ `mtval=0x4`。
6. 「随机」的来源：`attr_count` 是别的调用的栈残留，其最高位是否被置上取决于当时栈上写了
   什么（错误码 `-84`、`0xFFFF...` 之类很常见），所以表现为时好时坏；`attr_count` 恰为 0
   的启动就正常。同理，**加任何打印/探针都会改变这段栈的历史**，故障率随之漂移——这不是
   「Heisenbug 不能诊断」，而是「症状取决于栈残留」。

**取证链（全部为 RAM-only 探针 + 故障路径事后打印，不改变非故障路径时序）**：

| 观测 | 读数 | 结论 |
|---|---|---|
| `lfs_dir_commitattr` 入口 | `tag=0xffffffac buf=0x00000000 ptag=0x20100411` | 坏 tag 已带 bit31，buffer 为 NULL |
| 同一 `attrs` 指针逐层扫描位标记 | `bm=0x3f`（commit/orphaning/relocating/…全置位） | 坏值在 `lfs_dir_commit` **入口**就存在，不是提交过程中被踩 |
| `lfs_dir_commit` 入口快照 | `c1=0xffffffac catt=0x80190310 cn=2` 且 `cat=10 == commitattr_n` | 同一数组、同一次提交，**构造时就错** |
| `lfs_file_sync_` 入口快照 | `ffile=0x8010be60 fac=0xffffffac fcfg=0x801901b4 fid=1` | `ffile` 是 `&g_fs.stream`；`cfg->attr_count` 在 sync 入口已是垃圾 |
| 地址比对 | `fcfg=0x801901b4` 落在 `s_stack_fs`(0x8018f40c–0x8019040c) 之内 | `file->cfg` 指向 **FS 任务栈**上的一个已失效帧 |
| 反汇编 `lfs_file_sync_` | `lw a2,80(s0)`（`file->cfg`）→ `lw a4,8(a2)`（`attr_count`）→ `or a4,a4,0x10200...` | 垃圾高位经无掩码的 `LFS_MKTAG` 直接进 tag |

**修复**（`kernelsrc/src/arkrtos_fs.c`）：把 stream 的 `struct lfs_file_config` 放进
`g_fs`（新成员 `stream_cfg`，紧邻 `stream` 声明），`fs_stream_open()` 用
`struct lfs_file_config *fcfg = &g_fs.stream_cfg;` 并传 `fcfg`。这样 config 与 stream
同寿命（两者都在 `g_fs` 里），`lfs_file_close → lfs_file_sync_` 读到的 `attr_count` 恒为 0，
构造出的 tag 为 `0x10200400`（无 bit31）⇒ 走 from-memory 分支、写 0 字节、不解引用 NULL。

注意：`write_file` / `read_file` / `truncate` / `read_at` 等处的 `fcfg` 是栈局部变量，
但**开与关在同一帧内**（`opencfg` → … → `lfs_file_close` 都在函数返回前完成），指针不会
跨越返回，因此不构成同类缺陷；只有 stream（`open_*` 与 `close_*` 分属不同调用）会悬空。

**验证**（同一台机、同一二进制）：

| 构建 | 运行次数 | 结果 |
|---|---|---|
| 修复前（SHELL=1，无探针） | 8 | 5 FAIL |
| 修复前（各档探针） | 14~20 | 3~7 FAIL（故障率随探针漂移） |
| 修复后 + 探针仍在（用于确认 `fac`） | 20 | **20/20 PASS**，`hit` 恒 0 |
| 修复后（探针已全部清理，终版） | 20 | **20/20 PASS**，`[FS] format rc=0` 全数 |

**三条可复用教训**：

1. **`lfs_file_opencfg` 的 `cfg` 是「借指针」不是「拷贝」**。任何 `file` 对象的寿命长于
   当次函数调用，`cfg` 就必须是 static/成员，不能是栈局部。
2. **故障率随探针漂移 ≠ 探针不可用**。正确的用法是「探针只写 RAM，故障路径事后 dump」，
   并把「同一个指针 / 同一个序号」作为事件对齐手段（本轮用 `cat == commitattr_n` 把两次
   观测锁成同一次提交，才没有被随机性带偏）。
3. **`LFS_MKTAG` 不做掩码**：它把 `size` 直接或进 tag，而 tag 的 bit31 另有语义（from-disk）。
   所以任何进 `LFS_MKTAG` 的 `size/attr_count` 一旦是垃圾，症状是「解引用 NULL」而不是
   「数值不对」——排错时要先怀疑「tag 的输入量」，再怀疑「数组被踩」。

### 3.13 栈用量实测与本轮修掉的一个真缺陷（U-12 / U-14，2026-11-17）

U-13 把栈底守卫接上之后，`stack raw` 能同时给两路读数：`fill`（扫描未被
覆盖的填充图样）与 `psp`（历史最低栈指针）。**两路取大者**这条口径不是保
守；本轮它救了一次。

**旧读数错在哪**：U-12 登记时写的是「RV32 软定时器栈峰值 184 B、剩 200 B」，
那是 `psp` 单路（47%）。同一次运行里 `fill` 是 **372 B / 384 B = 96%**——
只剩 12 B。本 port 的陷阱帧压在被中断任务的栈上（RV32 一帧 128 B），
失主 12 B 意味着「任何一次定时器中断正好切到它」就踩出去，而踩出去是
静默的（踩到相邻 `.bss`）。

**改前 → 改后（QEMU RV32 实跑，端口 45894）**：

| 项 | 改前 | 改后 |
|---|---|---|
| 软定时器服务任务栈 | 96 字 = 384 B，实测 372 B（**96%**） | 256 字 = 1 KB，实测 372 B（**36%**） |
| `utest` | 13 matched / 13 passed | 13 matched / 13 passed（不变） |
| 镜像（改后实测） | —— | text 235828 / data 128 / bss 628228；相对改前只多 `.bss`，增量为 160 字 × 4 = **640 B**（算术，未单独重建改前镜像去对） |

改法：`board/qemu_riscv32/arkrtos_board_config.h` 里覆盖
`ARKRTOS_TIMER_TASK_STACK_WORDS` 为 256（与 `board/la32_generic` 同值），
默认值（96）不动，所以其它板子体积与行为零变化。

**同口径的其它端口读数**（跑同一组 `utest run` / `sec all` / `audit` / `dmesg`）：

| 端口 | 软定时器栈 | 控制台（shell）栈 |
|---|---|---|
| RV32（改后） | 1024 B，372 B（36%） | 3072 B，2172 B（70%） |
| LA64 | 4096 B（512 字），2172 B（53%） | 8192 B（板级放大的），2700 B（33%） |

**U-14 在 RV32 上不能照 LA64 的判据读**：LA64 那个「3 KB 被 `ld` 用到
100%」的用例，在 RV32 上**不存在**——`help` 里没有 `ld` / `modules` /
`lkup`，`modld` 在 32 位档被 64 位守卫拒绝。所以 RV32 这一格只能给
「已测命令集内的峰值」，不能读成「3 KB 在 RV32 上够用」。LA32（无载体）
与 STM32（需上板）仍未测。

复现手法：RV32 镜像不能用 `tools/qemu_shell_drive.py` 直接驱（它硬编码
`-machine virt` 却不给 `-cpu rv32` / `-bios none`，RV32 镜像下去 rc=1
立即退出）。本轮用的是仓库内临时脚本 `build/_rv32_probe.py`（与驱动脚本
同构：TCP chardev + `\r` + 按时间 pump，只多这两个参数），
命令序列与读数原文落在 `build/u14_rv32_after.log`。

## 4. 已知限制（明确不覆盖的部分）

| 限制 | 影响 | 原因 |
|---|---|---|
| 只有故障路径有独立栈，普通 trap 仍与任务共用栈 | 普通中断的栈溢出行为与真实机不同 | 独立 handler 栈要改通用调度路径，见 §3.10.2(b) |
| trap 不可嵌套（强制） | 中断处理程序里不能开 `MIE` | 放开会让 U 任务静默跑成 M 模式，见 §3.10.2(a) |
| 任务间隔离未做（只做了特权级） | 一个 App 碰不到另一个 App 的 RAM 这点**未验证** | 示例任务由内核注册，拿到内核窗口；需 App 镜像走 loader 才有单独 slot |
| `mcause=2` 的 M 模式来源直接停机 | 内核自己写错 CSR 会停机而不是重启任务 | 故意：M 模式非法指令是真 bug，不能拿重启掩掉 |
| `ecall` 后 `mepc+=4` 由 port 做 | 换核/换编译器时须复核 | 不依赖硬件行为 |
| 无 FPU 上下文 | 浮点任务不可用 | `USE_FPU=0` |
| 单 hart | SMP 路径未验证 | `USE_SMP=0` |
| QEMU 不模拟真实外设 | 驱动层仍需上板 | 只有 16550 UART + CLINT（LoongArch 侧另有 ns16550a @0x1FE001E0） |
| 无 IPI（LoongArch） | 不能在任意时刻异步触发切换 | QEMU loongarch `virt` 自写 `ESTAT.IS` 实测无投递 ⇒ 改就地切换 |
| PLV3 隔离靠**静态 TLB 条目**（无页表遍历、无按需换页） | 一个 App 只能落在启动时写死的一页 2 MB，不能动态分配、不能换出 | 见 §5.12；这一档是为了把 PLV/TLB 这条路验穿，不是多 App 内存管理 |
| App 页是**整页 RWX**、App 的 sp 由 App 自己决定 | 没有 W^X；App 把 sp 指到内核页时，trap 帧会按它给的位置写 | 见 §5.12.4 的已知边界 |
| 内核低 16 MB 映射只在 `virt` 的地址布局下成立 | 换真实芯片必须重做 `.text` 起点、`.data` LMA 与 TLB 条目 | 同 LA32，见 §6.7 |
| 串口接收只在 RV32 上是中断驱动 | LoongArch 侧仍是轮询收（发送均为轮询） | RV32 已接 PLIC，见 §3.11；LA64/LA32 的 PLIC 等价物未接 |
| 无通用外部中断注册 API | App 侧不能自己装 ISR | `arkrtos_hal.h` 只有 `disable/enable/irq_init`；外部中断目前靠 port 弱钩子 + 板级覆盖 |
| **LA32 无任何执行载体** | LoongArch32 只有「编译 + 链接」证据，**没有任何运行证据** | QEMU 9.2.3 只有 `qemu-system-loongarch64`（上游无 loongarch32 系统模拟器，已 `ls` + `find` 双证），机器上也没有 LA32 板子 |

## 5. LoongArch64 移植（已实现，QEMU 实跑通过）

### 5.1 目录落点

```
kernelsrc/port/loongarch/la64/arkrtos_port.c        # HAL 实现 + trap 分派
kernelsrc/port/loongarch/la64/arkrtos_context.S     # 首次进入 / 切换 / 系统调用入口
board/qemu_loongarch64/arkrtos_board.{c,h}          # 板级初始化、UART、节拍中断入口
board/qemu_loongarch64/arkrtos_board_config.h       # 架构核选择与容量配置
example/qemu_loongarch64/start.S                    # 复位入口（设 sp + 清 .bss）
example/qemu_loongarch64/link.ld.in                 # 链接脚本模板
example/qemu_loongarch64/gen_ldconsts.py            # 从分区头生成链接期常量
example/qemu_loongarch64/libc_shim.c                # memset/memcpy/str* 等垫片
example/qemu_loongarch64/Makefile
```

### 5.2 运行模式：PLV0 内核 + PLV3 用户态、单核、没有 IPI

三条都是实测结论，不是选择：

1. **IPI 无效**。向 `ESTAT.IS` 自写中断位后没有任何异常被投递。因此
   不能照搬 Cortex-M 的 PendSV（「置位 pending → 等异常出口再切」），
   改为**就地切换**：`arkrtos_port_switch_task()` 在 syscall / 节拍异常的
   上下文里直接调 `arkrtos_port_switch_task_inline`（保存当前帧、恢复下一个
   帧），返回后由 trap 出口的 `ertn` 结算。切换点因此只有两处：trap 出口
   结算、以及 idle 首次进入。
2. **内核在 PLV0、示例 App 在 PLV3**（`ARKRTOS_USE_PRIV=1`）。隔离来自
   五条静态 TLB 条目的 PLV 位，没有软件页表遍历、没有按需换页 —— 实现与
   实测见 §5.12。`USE_PRIV=0` 时 port 退化为空操作，默认档字节不变。
3. **单核**（`USE_SMP=0`）；`virt` 支持 `-smp`，但 SMP 路径本轮未验证。

### 5.3 trap 帧：256 字节、8 字节槽

帧从高地址往低地址排，槽宽 **8 字节**（LA64 的 GPR 与 `ERA`/`PRMD` 都是 64 位）：

| 偏移 | 内容 |
|---|---|
| 0 | `$ra` (r1) |
| 8 | `$tp` (r2) |
| 16 | 原 `$sp`（入帧前的值，供调试/回溯） |
| 24–80 | `$a0`–`$a7` (r4–r11) |
| 88–152 | `$t0`–`$t8` (r12–r20) |
| 160 | `$fp` (r22) |
| 168 | `$s0` (r23) |
| 176–232 | `$s1`–`$s8` (r24–r31) |
| 240 | `ERA` |
| 248 | `PRMD` |

`$r21` 不进帧（psABI 保留寄存器，本 port 不用它，`start.S` 也不设 gp）。
切换时 `$s0` 当帧指针使用，因此 `$s0` 自身在 168 处显式保存。

### 5.4 PRMD 的位序（本轮最贵的一个坑）

**PRMD 的位序与 CRMD 的低 4 位相同：`PPLV=[1:0]`、`PIE=bit2`、`PWE=bit3`。**

我最初按「`PIE=bit0`、`PWE=bit1`、`PPLV=[3:2]`」处理，写成了
`LA_PRMD_INIT = 1u`，并在 inline 切换里用
`andi 1 / slli 2` 把 IE 往 `[3:2]` 放、把 `[3:2]` 往 bit0 放——位序正好反了。

后果链（全部在 QEMU 上观察到，不是推测）：

1. 首次 `idle` 在 PLV0 + IE1 下被节拍中断打断，硬件压的 PRMD 实测是 **0x4**；
2. 我按 0x1 语义把它折回帧里，`ertn` 于是把上下文恢复到 **PLV=1 且 IE=0**；
3. PLV1 下定时器中断不再投递（`ESTAT=0x800` 常驻、`CRMD=0x09`），
   `idle 0` 永久挂死，PC 停在 `0x20b824`；
4. 后续被 resume 出来的 sp/PC 是 32 位符号扩展残值（`0xfffffffeed07100` /
   `0x20d000`），一执行就 `Ecode=0x48`（ADEM），嵌套异常自锁。

修法两处（都按硬件语义改，而不是在 `ertn` 前强写 CRMD 绕过）：

```c
/* arkrtos_port.c */
#define LA_PRMD_INIT (0x4u)   /* PPLV=0 且 PIE=1：回到 PLV0 并开中断 */
```

```asm
/* arkrtos_context.S，inline 切换里折 PRMD */
csrrd   $t0, 0x0          /* CRMD */
andi    $t0, $t0, 7       /* bit3 是 CRMD 的 DA，不能带进 PRMD 的 PWE */
st.d    $t0, $s0, 248
```

因为位序相同，「CRMD 低 3 位」就是 PRMD 需要的 `PPLV|PIE`，掩到 7 即可；
多带 bit3 会把 `CRMD.DA` 写进 `PRMD.PWE`。

定位手段见 §5.9：不要靠推理，去读硬件真实压进帧里的那个值。

### 5.5 地址、UART 与定时器（实测标定）

| 项 | 值 | 备注 |
|---|---|---|
| RAM base / size | **0x0** / 128 MB | 低 1 MB 被 boot_info 占用 |
| 链接基址 | **0x200000** | 低于此会被 boot_info 覆盖 |
| UART | ns16550a @ **0x1FE001E0** | 不是 0x10000000；仅轮询收发 |
| `rdtime` 频率 | **100 MHz** | `arkrtos_port_get_system_clock()` 返回此值 |
| TCFG 计数频率 | **25 MHz** | `start_timer(us)` 里 `initval = us * 25` |
| CSRs | CRMD=0x0 / PRMD=0x1 / ECFG=0x4 / ESTAT=0x5 / ERA=0x6 / EENTRY=0xC / TCFG=0x41 / TVAL=0x42 / TICLR=0x44 | 编号即 `csrrd/csrwr` 的立即数 |

节拍定时器配置：`TCFG = ((initval & 0x3FFFFFFF) << 2) | 0x3`
（bit0 使能、bit1 周期模式，高位是计数值）。`TICLR` 写 1 清中断。
`arkrtos_port_enter_idle` 的 `wfi` 对应 `idle 0`。

`-bios none` **不能用**：LoongArch 的 `-kernel` 路径与 RISC-V 不同，
加上反而起不来；`-serial file:` **只接受相对路径**（绝对路径静默不落盘），
所以 `make run` 里先 `cd $(BUILD)`。

### 5.6 构建约束

```bash
cd "D:/工作/git_project/ark rtos"          # 必须在仓库根目录
make -f example/qemu_loongarch64/Makefile -j4
make -f example/qemu_loongarch64/Makefile run
```

- **必须在仓库根目录执行**。在 `example/qemu_loongarch64/` 里 `make` 会因为
  `build/` 相对路径解析而报 `No rule to make target ...`。
- 工具链只有一个 multilib（`lp64d`），因此全程 `-nostdlib -nostartfiles`，
  不链 newlib；用到的 libc 函数由 `libc_shim.c` 提供（与 RV32 同思路，
  但 LA64 下 `size_t` 是 64 位，签名要按 `__SIZE_TYPE__` 对齐）。
- Makefile 里通配全部 `kernelsrc/src/*.c` 后**剔除** `arkrtos_flashapp.c`、
  `arkrtos_vfs_lfs.c`（文件头无条件包含 `drvflash.h` / `lfs.h`，本目标没有
  Flash 控制器与 littlefs）。`arkrtos_fs.c` 保留（ramfs 需要）。
- `gen_ldconsts.py` 的调用**末尾必须带分号**：MinGW 版 make 3.82 对「不含
  shell 元字符」的命令走 `CreateProcess` 快路径，本机 python 装在含非 ASCII
  字符的目录里，快路径定位不到；写个 `;` 才会交给 `sh` 按 PATH 解析。
- 增量构建要跟踪 `-include` 的板级配置头，否则只改板级宏时旧 `.o` 会带着
  上一套开关被复用（`-MMD -MP` + `-include $(OBJS:.o=.d)`）。

### 5.7 LP64 带来的两个静默错答案

1. **`uint32` 变成了 64 位**。老内核里不少地方把 `uint32` 当 32 位用
   （地址字段、位宽假设），而它在 `unsigned long` 定义下 LP64 就是 64 位。
   已用条件 typedef 修好：

   ```c
   #if defined(__LP64__)
   typedef unsigned int  uint32;
   #else
   typedef unsigned long uint32;
   #endif
   ```

2. **trap 帧槽必须 8 字节**。若 C 侧用 `uint32*`（4 字节步长）索引 8 字节槽，
   `stack_init`、`ERA += 4`、`get_arg/set_ret`、epilogue 会**全部错位**，
   表现为 ADEM 嵌套异常死循环。修法是新增

   ```c
   typedef unsigned long la_frame_word;   /* 帧槽宽 = 8 字节 */
   ```

   并把六处访问函数签名改型。**`.S` 侧不需要改**——汇编本来就是 8 字节步长，
   错的只是 C 侧的指针类型。

### 5.8 实测结果

尺寸（`loongarch64-newlib-elf-size`）：

```
   text    data     bss     dec     hex  filename
  54536      80   18032   72648   11bc8  ark_qemu_la64.elf
```

运行命令：

```bash
cd build/qemu_loongarch64
"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -M virt -m 128M -nographic -display none \
  -serial file:outfinal.txt -kernel ark_qemu_la64.elf
```

串口原始输出（节选，未经修饰；删掉全部临时调试打印后的回归）：

```
[Ark RTOS] board: qemu_loongarch64 (virt, la64, 1 core)
[Ark RTOS] arch=loongarch uart=0x1fe001e0 timer=25000000Hz rdtime=100000000Hz
[boot] sem=19922944 taskA=1 taskB=2 tick_period_us=500
[A] round=0 tick=2 d_tick=2 ms=1 d_ms=1 ms_svc=1 svc_ok
[B] woke, round=1 tick=7
[A] round=1 tick=8 d_tick=6 ms=4 d_ms=3 ms_svc=4 svc_ok
[B] woke, round=2 tick=11
...
[A] round=17 tick=16020 d_tick=1000 ms=8010 d_ms=500 ms_svc=8010 svc_ok
[B] woke, round=18 tick=16021
[A] round=18 tick=17021 d_tick=1001 ms=8510 d_ms=500 ms_svc=8510 svc_ok
[B] woke, round=19 tick=17022
```

可读出的三件事：

1. **节拍换算正确**：`d_tick` 稳定在 1000/1001（2000 Hz 节拍 ↔ A 每次 500 ms），
   `d_ms` 500/501，`ARKRTOS_TICK_PERIOD_US=500` 生效。
2. **调度正确**：A 每轮 `task_wait(500 ms)` 后醒来、B 由信号量唤醒，
   tick 单调递增、无插队、无挂死（连续 19 轮）。
3. **SVC 通路正确**：`ms_svc` 与内核换算值逐轮一致，`svc_ok` 全绿，说明
   `syscall` 取号（从 `ERA-4` 解指令）、`$a0` 回填、`ERA += 4` 都工作正常。

### 5.9 调试手法：QEMU monitor over telnet

板子不在手上时，「寄存器到底是多少」只能靠这个。起 QEMU 时加

```
-monitor telnet:127.0.0.1:4567,server,nowait
```

然后用 python socket 发命令（`info registers` / `xp /32gx 0x3086d0`）：

```python
import re, socket
s = socket.create_connection(("127.0.0.1", 4567), timeout=2)
s.recv(65536)                      # 吞掉 banner
s.sendall(b"info registers\n")
buf = b""
while True:
    chunk = s.recv(65536)
    if not chunk: break
    buf += chunk
    if b"(qemu)" in chunk: break
txt = re.sub(rb"\x1b\[[0-9;]*[A-Za-z]|.\x08", b"", buf).decode("ascii", "replace")
print("\n".join(l for l in txt.splitlines() if re.match(r"^\s*[0-9a-f]+:", l)))
```

两个细节：

- **回显带 ANSI 与退格**，必须先用 `re.sub(r'\x1b\[[0-9;]*[A-Za-z]|.\x08', '', txt)`
  清掉，否则数字会夹着控制字节；
- `xp` 的输出行用 `^\s*[0-9a-f]+:` 过滤，否则混着提示符。

本轮就是靠「在 `.S` 里记一条内存轨迹环 + monitor `xp` 取回」量出
**首次 trap 的 PRMD 是 0x4**，从而推翻自己原先 0x1 的假设。

### 5.10 软件全栈实跑（blk → littlefs → VFS → procfs）

> **档位：QEMU 实跑通过。** 读数全部来自串口原文，没有修饰。

#### 5.10.1 打开了哪些开关

`board/qemu_loongarch64/arkrtos_board_config.h`：

| 开关 | 值 | 说明 |
| --- | --- | --- |
| `ARKRTOS_USE_BLK` | 1 | 块设备层 + 4 KB 擦除单位 |
| `ARKRTOS_USE_FS` | 1 | 文件系统抽象层 |
| `ARKRTOS_USE_VFS` | 1 | VFS 挂载表（实测 mounts=4） |
| `ARKRTOS_USE_VFS_LFS` | 1 | littlefs 后端 |
| `ARKRTOS_USE_PROCFS` | 1 | `/proc/version` 可读 |
| `ARKRTOS_USE_MPU` / `ARKRTOS_USE_PRIV` | 0 | 见 5.10.5：LA64 的隔离要页表，尚未做 |

为跑通这条链补的四个工程件：

1. `board/qemu_loongarch64/drvblk.c|h`：512 KB RAM 介质（`ERASE=4096`），
   首次 `arkrtos_board_blk_init()` 把整卷填 **0xFF**（等价于擦除态），
   写只允许 1→0，否则整笔返回 -1 并计数 `rejected`——这是 NOR 语义，
   也是 littlefs 判断「有没有按擦除单位擦过」的依据。
2. `example/qemu_loongarch64/Makefile`：补 `arkrtos_vfs_lfs.c`、
   `ark_vfs_port_arkrtos.c`、`ark_vfs_devfs.c`、`ark_vfs_procfs.c`、
   `lfs.c`、`lfs_util.c`、`drvblk.c` 七个源，并加 `-mstrict-align`（见 5.10.3 Bug B）。
3. `example/qemu_loongarch64/main.c`：新增第 3 个任务 `task_fs`（栈 1024 word = 4 KB），
   启动顺序 `irq_init → kernel_module_init → arkrtos_board_blk_init() → 注册 A/B/FS`。
4. `example/qemu_loongarch64/libc_shim.c`：补 `strspn/strcspn/strrchr/strpbrk` 与
   `__bswapsi2/__bswapdi2/__popcountsi2/__popcountdi2/__clzsi2/__ctzsi2`
   ——littlefs 用到了它们，而全程 `-nostdlib` 不链 newlib。

#### 5.10.2 实测读数（串口原文）

```
[boot] sem=19922944 taskA=1 taskB=2 taskFS=3 blk=1 fs=1 vfs=1 tick_period_us=500
[FS] blk ram0 found=1 init=1 size=524288 erase=4096
[FS] format rc=0
[FS] vfs_init rc=0 mount rc=0 mounts=4
[FS] write rc=0 read rc=0 len=17 -> match
[FS] ls /mnt/ram:
  hello.txt size=17
[FS] /proc/version: Ark RTOS arkrtos 0.0.3 (build Nov 16 202
[FS] overwrite-without-erase rc=4294967295 rejected=1
[FS] counters written=1024 rejected=1 erases=2
[FS] PASS
```

五行读数各对应一件事，缺一条都不算通过：

1. `blk ... found=1 init=1 size=524288`：设备枚举与初始化真的走通了，
   容量与擦除单位是从设备属性读出来的，不是打印常量；
2. `format rc=0` + `mounts=4`：littlefs 在 RAM 介质上建卷并挂进 VFS，
   四个挂载点（ram/、dev/、proc/、以及默认根）都注册成功；
3. `write/read len=17 -> match`：写入后读回的字节数一致；
4. `/proc/version` 有内容：procfs 不是空壳；
5. `overwrite-without-erase rc=-1 (4294967295) rejected=1` +
   `counters written=1024 rejected=1 erases=2`：**不擦除直接覆盖被拒**
   ——这条是「NOR 语义真的生效」的负对照，不是成功路径的复读。

镜像尺寸（`loongarch64-newlib-elf-size`）：

```
   text    data     bss     dec     hex  filename
 131656     160  558096  689912   a86f8  ark_qemu_la64.elf
```

`bss` 里 512 KB 就是那块 RAM 介质，不是内核开销。

栈是按实测留的，不是拍脑袋（用 `0xCDCDCDCD` 图案量高水位）：
FS 任务峰值 **2040 / 4096 B = 49.8%**，任务 A 568 / 2048，任务 B 552 / 2048，
idle 1020 / 1024。FS 给 4 KB 是因为 littlefs + VFS 是这条链里最深的调用栈。

#### 5.10.3 两个「能编译、能链接、但语义错」的缺陷

**Bug A：`$t0` 存进了 `sp+256`，不是 `sp+88`。**
`eentry` 与 `arkrtos_port_switch_task_inline` 的入口先把 `$t0` 当临时寄存器用，
却把它的保存槽算成了帧尾（`sp+256`），而 LA64 的 `t0` 槽是 `sp+88`。
后果不是立刻崩，而是**中断返回后 `$r12` 变成 `sp+256`**——一个看起来像地址的值，
在下一次用到 `$t0` 时才炸到别处。修法：入口先 `st.d $t0,$sp,88` 再算 sp 槽。

**Bug B：LP64 下相邻 `u8` 字段被合并成非对齐 `st.w`。**
`ark_vfs_ramfs.c` 里两个相邻 `u8` 结构字段，GCC 在 `-O2` 下用一条 4 字节 store 一起写，
地址没对齐 → ADEM。这不是代码写错，是编译产物在宽松对齐假设下调优。
修法是 `-mstrict-align`，并且**必须全量重建**：

```bash
rm -rf build/qemu_loongarch64
make -f example/qemu_loongarch64/Makefile -j8
```

因为本工程的 Makefile **不跟踪编译参数**——只改 `ARCH_FLAGS`/`CFLAGS` 而增量编，
会得到"旧的 .o + 新的命令行"，症状是改了半天毫无变化。

#### 5.10.4 偶发 ADEM：从「卡在奇怪的地方」到「255 被当成任务号」

现象：约每 3~10 次启动有 1 次，在 `[FS] PASS` 之后（FS 任务调
`arkrtos_task_wait(1000)` 那一刻）打 FATAL：

```
[Ark RTOS] FATAL: estat=0x00480000 ecode=0x00000008 era=0x0021e534 badv=0x0021c258 badi=0x28c3c2ec crmd=0x00000008
```

`Ecode=8` 是 ADEM（访存非对齐），`EsubCode=1`；`era` 落在
`arkrtos_port_switch_task_inline` 的第一条恢复指令上（`ld.d $r12,$r23,240`），
`crmd=0x8` 说明已在内核态、中断已关。

**先排除三条错判据**：ADEM 下 `BADV` 是 UNPREDICTABLE（实测读到 0x21c258，
与 `era` 完全无关，不能当证据）；`estat` 只说明「什么错」，不说明「为什么」；
打印探针会改时序、把偶发 bug 藏起来。所以取证只靠三样：
帧内寄存器、内存里的环形日志、QEMU monitor 在 `for(;;) idle` 停机态直接读内存。

取证结果（各字段都是现场读出来的）：

1. `la_switch_inline()` 返回 **0xffffffff**，汇编 `move $r23,$r4` 之后
   第一条 `ld.d $r12,$r23,240` 立刻非对齐 → ADEM；
2. 切换环形日志（`next / 旧帧 / 新栈指针 / 切换后任务号`）最后一条是
   `0x00000100  0x00389310  0xffffffff  0x00000100`
   ——**任务号 256、新栈指针 0xffffffff**；
3. `arkrtos_current_task_id` 现场被写成 `0x100`；
4. `next = arkrtos_sched_is_switching()` = `arkrtos_sched_next() + 1`；
   `sched_next()` 直接返回 `arkrtos_ready_top()`；
   而 `ready_top()` 在「位图置位、但该优先级的桶头还是 `ARKRTOS_TASK_NIL`(0xFF)」时
   **把 0xFF 当任务号返回** ⇒ 255 + 1 = **256**；
5. `sched_activate(256, …)` ⇒ `tid = 255` ⇒ 读 `arkrtos_task_table[255]`
   （越界，正好落在 0xFF 填充的 512 KB 介质数组里）⇒ `stack_ptr = 0xFFFFFFFF`
   ⇒ 返回它 ⇒ ADEM；
6. **附带损害比崩溃更严重**：`current_task_id=256` 之后每次切换都会把
   `old_psp`/`status`/`touch_tick` 写进介质数组，静默破坏文件系统内容
   ——即使没崩，卷也已经坏了。

封堵是三层，跨架构的在前，架构层的在后：

| 层 | 位置 | 做法 |
| --- | --- | --- |
| 内核 | `arkrtos_ready_top()` | 位图只是索引、`status` 才是真相：遇「组有位、组内无位」或「位有、桶头为空/越界」就**就地清位重取**（最多 8 轮），**绝不返回 255**；现场存进 `arkrtos_ready_heal_*` |
| 内核 | `arkrtos_sched_is_switching()` / `arkrtos_sched_activate()` | 任务号范围检查（`0..task_count`），越界一律不切、不写任何状态 |
| 架构 | `la_switch_inline()` | 栈帧闸门：`next_sp` 不在 `[0x300000,0xB00000)` 或未按 16 字节对齐 ⇒ 拒绝切换并计数（`s_swp_bogus`） |

顺手修掉的取证缺陷：`la_fatal_dump()` 打印 switch-log 时只打 4 条、且从写指针
`pos` 处起算，读到的全是空槽（这就是早前误判「pos=4 但记录全 0」的原因）。
现在打印全部有效条目、按**最旧→最新**排列，并补打 `heal/bogus` 计数。

**结果，以及没证明的部分**（这两句必须一起读）：

- 修复后 **90 次连跑**：`[FS] PASS` 90/90、FATAL **0**；
- 但三层守卫的计数在 90 次里**全是 0**（`heal=0`、`bogus=0`、任务号越界分支 0），
  也就是说修复之后**再没有观测到那次「位图与桶脱钩」**；
- 因此只能说：**下游链条已闭环取证**（255 → 256 → 越界 TCB → 0xFFFFFFFF → ADEM，
  每一环都有现场），**上游「脱钩」的触发源没有拿到单次现场，不能声称根因已定位**；
- 已排除：FS 栈溢出（实测峰值 49.8%）、`la_switch_inline` 未返回
  （`enter == exit`，25 次早退都是「无就绪任务」的正常路径）；
  「tick 打断恢复序列」这条在加了「保存完 PRMD 后关中断」之后仍崩，
  所以它不是单独成因（该保护仍然保留）；
- 崩溃率基线本身是时序敏感的（12 连跑 4 败 / 单次 2.5 s 内炸 / 18 连跑才炸一次），
  所以 90 次全过是「不再复现」的证据，**不是**「数学上不可能再发生」；
  三层守卫的价值在于：即使再发生，也不会变成 ADEM + 静默破坏介质，
  而是被计数拦下，且 `s_swp_log` / `arkrtos_sched_probe` / `arkrtos_ready_heal_*`
  三组现场足够定位。

#### 5.10.5 这一节明确**没有**做的事

1. **§5.10 当时没有内存隔离**：`ARKRTOS_USE_MPU`/`PRIV` 那时仍为 0。
   这一项已在 **§5.12** 补齐：走的是 **PLV3 + 静态 TLB 条目 + 重填向量**，
   而**不是**三级页表遍历（PGDL/PGDH + PWCL/PWCH 这套没实现，见 §5.12.4）；
2. **SHELL / LOG / UTEST 在 §5.10 时还没开**：本节的第五个任务是存储链路，
   控制台与单元测试的接线（`kernelsrc/shell/platform_arkrtos.c` + `ark_shell/*.c`）
   那时尚未接到 LA64 的 Makefile 里 —— 这一项已在 **§5.11** 补齐并实跑通过；
3. **LA32 只到编译**：`csrxchg` 与 `$t0` 槽的修复在 LA32 侧同步改了且能链接
   （见第 6 节），但 QEMU 9.2.3 没有 loongarch32 系统模拟器，没有运行证据。

### 5.11 控制台 Shell / 日志 / 板端单元测试（QEMU 实跑通过）

> **档位：QEMU 实跑通过。** 判据是一条**双向链路**的端到端自动检查
> （`example/qemu_loongarch64/shell_check.py`），不是「横幅打出来了」。

#### 5.11.1 接了什么

| 落点 | 内容 |
| --- | --- |
| `board/qemu_loongarch64/drvuart.c` | 新增：把 ns16550a 注册成 `ttyS0`（`arkrtos_dev_register(SHELL_DEV_NAME, ...)`），**收发都是轮询** |
| `dts/qemu_loongarch64.dts` → `board/qemu_loongarch64/arkrtos_dt.c` | 新增：`cmd_dt` 依赖的设备树（7 个节点），由 `tools/dts/dtc_lite.py` 生成，不手写 |
| `board/qemu_loongarch64/arkrtos_board.c` / `.h` | `arkrtos_dev_board_init()` 由空实现改为注册 `ttyS0`（失败会打印）；新增 `arkrtos_board_reboot()` |
| `board/qemu_loongarch64/arkrtos_board_config.h` | `SHELL` / `LOG` / `LOG_RING` / `UTEST` = 1，新增 `ARKRTOS_PLATFORM_REBOOT_PORT 1` |
| `example/qemu_loongarch64/Makefile` | 补 `platform_arkrtos.c`、`ark_shell/*.c`(6 个)、`drvuart.c`、`arkrtos_dt.c` |
| `example/qemu_loongarch64/main.c` | `id_shell = arkrtos_shell_init()`，成为第 5 个任务；启动行打印 `shell=/log=/utest=` |

三处诚实边界：

1. **串口是轮询，不是中断**：这个移植没有接外部中断控制器（本版只有
   TCFG 定时器一个中断源），所以 `uart_drv_ctrl` 对「开中断收」返回
   `ARKRTOS_DEV_ERR_NOIMPL`，而不是伪造一条收中断的路径；
2. **没有复位**：QEMU virt 没有复位设备，`reboot` 明确打印「做不到」后停机，
   不假装成功；
3. **`LOG` 走同一条 16550 轮询通路**，不做时间戳精度承诺。

#### 5.11.2 实测读数

`python example/qemu_loongarch64/shell_check.py`（TCP chardev，双向收发）连续 4 次：

```
PASS: LA64 内核 Shell 端到端通（横幅/help/uname/dt/ls/cat/stack/utest 全部通过）
```

脚本的 10 条判据：横幅 + 提示符、`[FS] PASS`、`help` 含 blk/mount/reboot、
`uname`、`dt` 含 `ns16550a` 且地址 `0x1fe001e0`、`ls /mnt/ram` 列出 `hello.txt`、
`cat /proc/version`、`stack` 的任务数 ≥ 5、与 `ps` 的 PID 行数相等且 `0 unmeasured`/
`0 at/over 90%`、`utest` 汇总行 `, 0 failed,`、全程无 `UNEXPECTED`/`FATAL`/`FAILED`。
其中 `stack` 与 `utest` 两条是「注册了但没跑」挡不住的：前者要求栈计量
真的在算（`0 unmeasured`），后者要求用例全量跑完且 0 失败。

> **勘误（2026-11-18）**：`stack` 这条判据原本写死 `5 task(s)`
> （A/B/FS/shell/idle）。示例后来为 ELF/MOD 演示又注册了任务，总数变 9，
> 判据就一直 FAIL——而 FAIL 跟被测对象无关，脚本因此长期是红的。
> 现在改成三条不随示例成长的判据：任务数 ≥ 5（少一个说明有任务没起来）、
> `stack` 的任务数与 `ps` 的 PID 行数**相等**（两个独立视图对不上就是有一边
> 在编数字，比一个常数可靠）、`0 unmeasured` 与 `0 at/over 90%` 同时出现。
> 当前读数是 `9 task(s), 0 unmeasured, 0 at/over 90%`，脚本重新 PASS。

> **勘误（2026-11-16）**：上面这条 `utest` 判据在本节写作时是**假绿**。
> 当时汇总行确实打印 `, 0 failed,`，但它是
> `0 matched, 0 passed, 0 failed, 0 skipped` —— **一条用例都没跑**。
> 判据只看「0 failed」，所以通过了。根因与修法见 **§5.13.3**；修好之后
> 现在的读数是 `9 matched, 9 passed, 0 failed, 0 skipped`，
> `assertions 90 run / 0 failed`。`shell_check.py` 的 `utest` 判据已随之
> 从「含 `, 0 failed,`」收紧为「含 `9 matched, 9 passed, 0 failed`」。

#### 5.11.3 第三个静默错答案：异常内部开中断 ⇒ `s_in_trap` 永久漏 1

**症状**（先看到的不是原因）：任务 A 每轮 `wait(500)` 之后读一次节拍差，
偶发 `d_tick=0`；同一轮里 SVC 读回的毫秒比本地读数大 **500 ms**。
两件事同源：A 的「睡眠」根本没发生 —— 它被标成 WAIT、挂进延时链，
然后**立刻返回**，真正切走要等下一拍节拍在异常出口结算。于是 A 打印的
是自己停住的旧节拍，而 SVC 读到的是错开 1000 拍之后的时间。

**取证**：在 `wait` 返回处读自己的 TCB，得到 `status=2 (WAIT)`、
`wait_time=1000`、`delay_queued=1`（「已经睡下但还在跑」）；再读
`arkrtos_port_switch_task()` 入口的快照，拿到 `ready=1`、**`in_trap=1`**。
任务上下文里 `s_in_trap` 是 1 —— 这就是根因。

**机制**：`arkrtos_port_switch_task()` 用 `s_in_trap` 区分「任务上下文」与
「异常上下文」，异常里只置 `s_switch_pending`、等 `la_trap_c()` 出口结算。
而 `arkrtos_port_enable_irq()` 当时**无条件**写 `CRMD.IE`，于是内核里那些
「临界区末尾开中断」的代码（阻塞类 SVC 的结尾就有一处）会在**异常处理程序
内部**把中断打开，节拍一进来就是嵌套异常。LoongArch 的 ERA/PRMD 只有一份、
没有嵌套栈：嵌套异常的出口一旦发生任务切换（pending 可能是外层刚置的），
外层处理程序的 C 栈帧就再也回不去，那句 `s_in_trap--` 永远执行不到 ——
`s_in_trap` 从此永久为 1，之后**所有**任务上下文的切换请求都被当成
「在中断里」，只置 pending。

**修法**（`kernelsrc/port/loongarch/la64/arkrtos_port.c`）：`s_in_trap != 0`
时 `arkrtos_port_enable_irq()` 直接返回。异常返回时 `PRMD.PIE` 会按被打断
时的状态恢复，该开的中断由 `ertn` 负责开 —— 这里什么都不做才是对的。

> 这不是 LA64 的「风格选择」：RV32 移植早已因为同类问题（嵌套 trap 把
> `g_rv_next_mpp` 改写成 M 模式，任务在一次故障重启后被静默提权到 M）
> 在 `enable_irq` 里装了同一道闸门，并把「trap 不可嵌套」写成边界。
> LA64 侧漏掉的正是这条**已经成立**的约束。

**修复前后**（同一份代码、同一批连跑，只差这一处）：

| | `[W!]`（wait 未阻塞） | `SVC_MISMATCH` |
| --- | --- | --- |
| 修复前，14 次 × 14~15 s | 6 次出现，每次 8~11 处 | 2/3 次出现 |
| 修复后，18 次（8 短跑 + 10 长跑）+ 4 次 `shell_check` | 0 | 0 |

修复后每轮 `d_tick` 分布：1000 占 34、1001 占 13、1002 占 1
（首轮冷启动的 6 个 2 拍不计），即 500 ms 周期真的按 1000 拍走。
本文原先「90 连跑 0 失败」的口径只统计了 `FATAL` 与 `[FS] PASS`，
把这条一直存在的 `MISMATCH` 漏掉了 —— 那个结论应读作「不再崩溃」，
**不是**「连等待时长都对」。

### 5.12 App 保护机制：PLV3 用户态 + 静态 TLB 隔离（QEMU 实跑通过）

> **档位：QEMU 实跑验证过**（`qemu-system-loongarch64 -machine virt`）。
> 这一节回答的是「App 能不能被内核挡住」，不是「多进程内存管理做好了」。
> 边界写在 §5.12.10，读之前先看那一节。

#### 5.12.1 为什么是静态条目而不是三级页表

LoongArch 的 MMU 是**软填**式的：TLB 不由硬件遍历页表得到，而是由软件
写 CSR 或执行 `tlbfill` 填进去。也就是说，**「隔离」这件事的全部来源
就是 TLB 条目的 PLV 位与地址范围**，页表遍历只是「怎么把条目填进去」
的一种实现方式，不是隔离的必要条件。

于是本轮选的是代价最小、也最容易取证的那条：启动时一次性写死五条条目，
运行期只在对 App 故障恢复时重填，**不做页表、不做按需换页、不做 ASID
切换**。这样「App 读内核页」失败时的现场（`ecode=PPI`、`badv` 正好是
内核地址、`plv=3`）能直接证明是条目权限挡的，而不是别的东西挡住。

#### 5.12.2 五条条目（QEMU virt 上的实际布局）

| VA 范围 | PS | 索引 | PLV | 缓存 | 用途 |
| --- | --- | --- | --- | --- | --- |
| `0x00000000`..`0x00800000` → PA 同址 | 22（4 MB ×2） | MTLB 2048 | 0 | 可缓存 | FLASH `0x00200000` + RAM 头 |
| `0x00800000`..`0x01000000` → PA 同址 | 22 | MTLB 2049 | 0 | 可缓存 | RAM 尾（`__ram_end=0x00B00000`） |
| `0x1FE00000`..`0x1FE01000` → PA 同址 | 12（4 KB） | STLB 组 `0x00`（`(VA>>13)&0xFF`） | 0 | **不缓存** | 内核 UART（MMIO） |
| `0x02000000`..`0x02200000` → PA `0x00300000` | 20（1 MB ×2） | MTLB 2050 | **3** | 可缓存 | App 代码/数据/栈 |
| `0x02202000`..`0x02203000` → PA `0x1FE00000` | 12 | STLB 组 `0x01` | **3** | **不缓存** | App 的 UART 别名 |

两条内核大页各覆盖 8 MB（PS=22，偶页 + 奇页各 4 MB），加起来正好盖住
`0x0`..`0x1000000`，即 `__ram_end` 以内的全部物理内存；PLV0 意味着 App
（PLV3）**一条都碰不到**。App 那两条 PLV3 条目是它唯一能走的路。

`MTLB 2048` 这个数不是我拍的，是 QEMU 源码的常量：`LOONGARCH_STLB 2048`、
`LOONGARCH_MTLB 64`、`LOONGARCH_TLB_MAX 2112` ⇒ MTLB 的绝对索引就是
`2048..2111`。真实硬件上 MTLB 的下标是 `0..63`、由 `CSR_TLBIDX` 的
index 字段选，换板子要改这一处。

#### 5.12.3 大页必须 `tlbwr`，4 KB 必须 `tlbfill`，大页塞不进 STLB

`helper_tlbfill`（QEMU `target/loongarch/tlb_helper.c`）的行为是二选一：

- `pagesize == CSR_STLBPS` ⇒ 写 STLB，路号 `get_random_tlb(0, 7)`（**随机**）；
- 否则 ⇒ 写 MTLB，下标 `get_random_tlb(2048, 2111)`（**随机覆盖**）。

`tlbfill` 的 index 是随机的，所以只要 TLB 里已经有别人的条目，用 `tlbfill`
写大页就是在赌命。这就是为什么：

1. **大页走 `tlbwr` + 显式 `CSR_TLBIDX`**（`index = 2048 + 槽号`、
   `PS` 写进 TLBIDX.PS、`E=1`），落到确定的位置；
2. **4 KB 页走 `tlbfill`**，让硬件按 `(VA>>13)&0xFF` 选组；
3. 反过来的想法（把 1 MB 页也塞进 STLB 免掉 MTLB）**行不通**：STLB 的
   查找是按 `PS` 字段和 `STLBPS` 比对页大小的，塞进去的大页永远匹配不上。

#### 5.12.4 坑：QEMU 上 `STLBPS` 复位值是 0

`target/loongarch/cpu.c:562` 是 `env->CSR_STLBPS = 0;`（复位值 0），而
`tlbfill` 正是拿 `pagesize == stlb_ps` 判断「写 STLB 还是写 MTLB」。不写
`STLBPS` 就直接 `tlbfill` 一个 4 KB 页 ⇒ 硬件认为「这不是 STLB 的页大小」
⇒ **随机覆盖 MTLB 的某一条**，命中的是内核大页就当场炸：

```
[LA64U] KERNEL refill (plv0) badv=0x00afffb8 era=0x00206b64 -> fatal
```

现场是「跑一会儿就报一次、`badv`/`era` 每次都不一样」—— 典型的随机覆盖。
修法是在 `la_mmu_init()` 里、填条目之前显式写一次：

```c
LA_CSRW(LA_CSR_STLBPS, 12UL);   /* PS 字段在 [4:0]，12 = 4 KB */
```

真实硬件上 `STLBPS` 复位即为 `0xC`，这一句是**幂等的**；写它只是为了在
QEMU 上复现真实硬件的初值，不是硬件要求。

#### 5.12.5 App 的 UART 别名为什么要换组

App 要打印，但 `0x1FE001E0` 那条条目是 PLV0 的，App 用不了。给它一个
PLV3 的别名是标准做法，可「随便挑一个地址」会踩另一个坑：**STLB 是组相联
的**，组号是 `(VA>>13)&0xFF`。内核 UART 与 App 别名若落在同一组，两页
不同 PLV 的条目会互相顶掉（而且 `tlbfill` 的路号是随机的，顶掉哪一路也不定）。

所以 App 别名的页基选 `0x02202000`：`(0x1FE00000>>13)&0xFF = 0x00`、
`(0x02202000>>13)&0xFF = 0x01`，**不同组**。App 侧看到的完整地址是
`0x022021E0`（`0x1E0` 偏移映射到 PA `0x1FE001E0`）。

> 这条坑是踩过的：`arkrtos_port.c` 里给的默认值是 `0x022001E0`（组 `0x00`，
> **与内核 UART 同组**），板级配置 `board/qemu_loongarch64/arkrtos_board_config.h`
> 才把它改成 `0x022021E0`。同组时两条 4 KB 条目会在同一组的 8 路里抢位置，
> 而 `tlbfill` 的路号是随机的 ⇒ 控制台那一页被顶掉，表现为「App 一打印，
> 内核就报重填」。

#### 5.12.6 两条故障入口，两条都必须换静态栈

| 入口 | CSR | 触发者 | 静态栈 | 为什么必须换 |
| --- | --- | --- | --- | --- |
| `eentry` | `CSR_EENTRY` | App 执行特权指令（`IPE`）、或访问**已映射但 PLV 不够**的页（`PPI`） | `la_fault_stack`（4 KB） | 故障处理最终会走 `arkrtos_task_recover_pending()` → `arkrtos_task_stack_init()`，**把整个任务栈重填**。处理函数若还在 App 栈上跑，它自己的 C 帧就被抹掉，返回时 `ra=0xcdcdcdcd`、PC 跳到 `0xcdcdcdcc`（RV32 移植踩过同一个坑） |
| `TLBRENTRY` | `CSR_TLBRENTRY` | App 访问**未映射**地址 | `la_refill_stack`（8 KB） | 触发者的 `sp` 本身可能就是非法页，压栈会再次 refill，形成无限重填 |

两条入口的共同纪律：

- **第一件事是换栈**，换完才允许 C 代码运行；
- 恢复上下文时必须显式写 `CSR_TLBRERA` / `CSR_TLBRPRMD`：`ertn` 看到
  `TLBRERA.ISTLBR=1` 就从 `TLBRERA`/`TLBRPRMD` 取 PC 与特权级，写回时
  **必须把 `ISTLBR` 位再置 1**，否则 `ertn` 会去用旧的 `ERA`/`PRMD`；
- 内核（`PPLV=0`）自己触发 refill 一律**直接停机**并打印 —— 那是条目配错了，
  不能让内核带着坏 TLB 继续跑。

#### 5.12.7 `.appprobe`：让「测了」和「看着像测了」分开

App 的代码、数据、字符串、栈全部落在 `.appprobe` 段，链接脚本把它放在
`ARKRTOS_QEMU_APP_BASE`（`0x02000000`），并有 `ASSERT` 保证不越过 App 页：

```ld
.appprobe ARKRTOS_QEMU_APP_BASE :
{
    __appprobe_start = .;
    KEEP(*(.appprobe))
    KEEP(*(.appprobe.*))
    __appprobe_end = .;
}
ASSERT(__appprobe_end <= (ARKRTOS_QEMU_APP_BASE + ARKRTOS_QEMU_APP_SIZE), "appprobe overflow")
```

四条配套纪律，缺一条就会变成「看起来测了、其实没测」：

1. **三个节名分开**（`.appprobe` / `.appprobe.text` / `.appprobe.rodata`），
   否则 `-ffunction-sections` 一开，函数被拎到 `.text.xxx` 里，PLV3 页上
   根本没有那段代码；
2. **所有 App 函数 `noinline`**，否则编译器把 App 函数内联进内核 caller，
   实测的执行者是内核代码（PLV0），隔离自然「通过」；
3. **App 不得调用任何内核封装**（`arkrtos_board_puts` 之类），必须自己写
   裸 MMIO 循环 —— 调内核函数就又回到 PLV0 了；
4. **装载自证**：`la_app_magic = 0x4C413634` 放在 `.appprobe.rodata`，内核
   启动后读 `0x02000000` 页上的这个值并与定值比对。对不上就说明段没被装进
   App 页（比如 `KEEP` 漏了、或者 `ASSERT` 没拦住），此时所有「App 隔离
   通过」的读数都必须作废。

启动日志里的这一行就是第 4 条在读：

```
[boot] paging on: kernel read 0x00300000=0x00000000 app_magic=0x4c413634 (probe image loaded)
```

#### 5.12.8 实跑读数

三条**负对照**（App 试三种越权方式，全部被挡）：

```
[U] round=1
[U] probe: read kernel page 0x00300000 ...
[LA64U] app fault ecode=7(PPI:privilege) badv=0x00300000 era=0x02001284 plv=3
[U] round=2
[U] probe: read unmapped 0x05000000 ...
[LA64U] tlb refill badv=0x05000000 era=0x020012ec plv=3
[U] round=3
[U] probe: csrrd CRMD (privileged) ...
[LA64U] app fault ecode=14(IPE:privileged-insn) badv=0x020012b0 era=0x020012b0 plv=3
```

三条读数的要点：

- `ecode=7` 是 `PPI`（page privilege illegal）⇒ 条目在、但 PLV 不够，**正是
  静态条目这一档要证明的东西**；
- `ecode=14` 是 `IPE`（privileged instruction）⇒ 特权指令在 PLV3 下非法；
- 三次全部 `plv=3` ⇒ 出事时确实在用户态，不是内核自己踩的；
- 每次故障后 App **回到 round 下一轮继续跑**（`round=4 ms=7 svc_ok`），
  说明故障恢复路径把上下文重建了，而不是崩在那里。

三条**正对照**（App 走自己的页与自己的 UART）：

```
[U] round=4 ms=7 svc_ok
...
[U] round=16 ms=6013 svc_ok
```

`svc_ok` 表示 App 通过 `syscall 0x11`（`TASK_CTRL`，`a0=SUB_WAIT`）拿到
内核服务并按时被唤醒；`ms` 单调递增且与内核侧 `[A] round=N ms=…` 对得上。

`[A]`（内核任务，500 ms 周期读节拍）、`[B]`（信号量唤醒）、`[U]`（PLV3 App）
三条线交替出现，互不阻塞 —— 这是「PLV3 不是把系统停住」的证据。

同一份固件上 `utest` 全量：

```
utest: 9 matched, 9 passed, 0 failed, 0 skipped; assertions 90 run / 0 failed
```

#### 5.12.9 为什么 App 的延时用 `SUB_WAIT` 而不是 `SUB_DELAY`

`SUB_DELAY` 在 trap 内部忙等（此时 `CRMD.IE=0`），节拍被吃掉：App 忙等
500 ms 时，主机 20 s 只前进了 37 ms。`SUB_WAIT` 走的是真阻塞（挂进延时链、
切走、到点唤醒），所以 App 的 `round` 推进与内核的 `ms` 才能对上。

> 这也顺带说明了 §5.11.3 那条约束的方向：**trap 内不开中断是对的**，
> 所以任何「在 trap 里等待」的设计都得改成「挂起 + 由节拍结算」。

#### 5.12.10 明确没做的部分（不要读成「做到了」）

| 没做 | 后果 |
| --- | --- |
| **没有页表遍历**（PGDL/PGDH/PWCL/PWCH 全未实现） | 一个 App 只能落在启动时写死的那一页；不能动态分配、不能换出、不能按需映射 |
| **默认档没有 ASID 区分**（`ASID` 恒 0） | 换 App 必须整 TLB 刷。U-7 的 ASID 档已做并实测（§5.28），缺省关 |
| **App 页整页 RWX** | 没有 W^X，App 可以改写自己的代码段 |
| **App 的 sp 由 App 自己给** | App 把 sp 指到内核页时，trap 帧会按它给的位置写 —— 这是「信任 App 传进来的栈」这一档的固有缺口，不是 TLB 能挡的 |
| **只有一个 App 页** | 证明的是「App 与内核之间」的隔离，**不证明 App 与 App 之间**的隔离 |
| **只在 QEMU virt 上验证** | 换真机要重做：MTLB 索引 `2048..2111` 是 QEMU 的常量，硬件上是 `0..63`；`STLBPS` 复位值也要按手册核 |

### 5.13 本轮修掉的三处真缺陷与一处未解

这一节记的都是「已经存在、并且被读数抓到过」的问题。前三条已修并复验，
第四条**没修**，只把现场钉住。

#### 5.13.1 `delay_us` 的阈值少写了 1000 倍

`delay_us` 用 `loops = us * factor` 估算循环数，`factor` 是从「1 µs 基准」
反推出来的常数，但代码里按 `us * factor / 1000` 算 ⇒ **500 ms 的延时变成
42.9 s**。修法是别再拍常数，直接按上限动态算：

```c
uint32 loops = (us > (0xFFFFFFFFu / factor)) ? (0xFFFFFFFFu / factor) : (us * factor);
```

判据是 Delay 在 QEMU 上实测的毫秒数与请求值同量级（见 §5.10 的 `d_ms` 读数）。

#### 5.13.2 PI 用例把相邻任务的内存写花了

`mtx_pi_chain_contract` 用例在 LA64 上偶发 `FATAL ecode=0x0d`、`era=0x300`
（`0x300` 是**相邻任务的参数**，不是代码）。根因是用例自己的栈太小：
优先级继承链上三层任务叠加的 C 帧超过了 `PI_CHAIN_STACK_WORDS`。修法是
**只改用例栈大小**（`512` words），不动上下文帧布局 —— 改帧布局会把
「栈溢出」变成一个看不见的偏移错误。

> 这一条修掉的是「`era=0x300`」那一版现场（帧被写花）。§5.13.4 记录的
> `era=0` 是**另一回事**：那是入口函数真的返回了，不是栈被写花。两件事
> 都发生在同一个用例附近，不要混读。

#### 5.13.3 `utest` 的 `0 matched, 0 passed, 0 failed` 假绿

三个目标的链接脚本都没有 `KEEP(*(.ctors))`，启动代码也不遍历
`.ctors/.init_array` ⇒ 构造函数表是空的 ⇒ **一条用例都没注册**，而汇总行
照样打印 `0 failed`。`shell_check.py` 当时只查「含 `, 0 failed,`」，于是
判过 —— 这是本项目里最典型的一次假绿。

两处一起修：

1. 三个目标（`qemu_loongarch64` / `qemu_riscv32` / `la32_generic`）的链接
   脚本都 `KEEP` 构造段，启动代码按 `__init_array_start..__init_array_end`
   遍历并调用（LA64 还要注意 `-mstrict-align` 下的取指对齐）；
2. **判据收紧**：`shell_check.py` 改为要求 `N matched, N passed, 0 failed`
   且 `N >= 1`，并对 `matched != passed` 单独报错。

修后读数是 `9 matched, 9 passed, 0 failed, 0 skipped`，三目标一致。
（后续又进了一例 `mtx_pi_chain_contract`，现在 RV32/LA64 实测都是
`10 matched, 10 passed, 0 failed, 0 skipped; assertions 93 run / 0 failed`。）
教训写在脚本头部：**「0 failed」只有在「matched ≥ 1」时才是通过**。

#### 5.13.4 未解：约 1/12 概率的间歇致命异常（`ecode=0x0d`, `era=0`, `ra=0`）

**现象**：LA64 上连跑 `utest`（连着跑两次命中率更高）时，隔十几次出现一次：

```
TASK:4582 task 8 exits (self kill)
TASK:4582 task 7 exits (self kill)
[Ark RTOS] FATAL: estat=0x000d0000 ecode=0x0000000d era=0x00000000 badv=0x0 badi=0x0 crmd=0x00000010
 frame@0x003071d0
  [0x00] ra=0x0        tp=0x0        sp=0x003072d0
  [0x30] ERA=0x0       PRMD=0x00000004
```

**第一层：栈归属（已确定，不是猜）**

`s_pi_chain_stack_mid` 在 `nm` 里是 `0x00306ad8`，`PI_CHAIN_STACK_WORDS=512`
⇒ 栈顶 `0x00306ad8 + 0x800 = 0x003072d8`，按 16 对齐后 `0x003072d0`。
故障帧在 `0x003071d0 = 栈顶 - 0x100`，**正是 `arkrtos_port_stack_init()` 放
初始帧的那个槽位**，帧里的 `sp` 字段也正好是栈顶。

也就是说：出事时**栈已经完全退到底**。这不是「野指针」，而是
**mid 任务从入口函数返回了** —— 返回时 `sp` 回到栈顶、`ra` 是入口被调用时
的值（这里是 0），于是 PC 跳到 0，取指得到 `INE`，`era=0`。

**第二层：为什么入口会返回**

`mtx_pi_chain_contract` 的两个辅助任务（`pi_chain_mid_entry` /
`pi_chain_high_entry`）都以 `arkrtos_task_kill_internal()` 收尾：

```c
static void pi_chain_mid_entry(void)
{
    ...
    arkrtos_task_kill_internal();
}
```

而 `arkrtos_task_kill_internal()` 的末尾是「置 INVALID → `ARKRTOS_SWITCH_TASK()`
→ `ARKRTOS_WFE()` → **返回**」。只要那一次切换没有真的发生（调度器返回
`NONE`、或切到自己的帧），函数就返回，`pi_chain_mid_entry()` 随即也返回，
`ra=0` ⇒ 上面那一幕。

**第三层：切换日志说了什么（推翻先前读法）**

本轮把切换日志扩到 6 个字段（`next / 旧帧 / 新帧 / 切后 cur_id /
目标切换前 status / 目标切换前 ready_next`），命中一次完整现场：

```
  next     old_frame  new_sp     cur  st  rd
  0x07  0x00309d80  0x003071d0  0x07  st=0x1 rd=0x6
  0x00  0x00307140  0x00390310  0x00  st=0x0 rd=0x0     <- mid 切到 idle
  0x08  0x00309d80  0x003069d0  0x08  st=0x1 rd=0x7
  0x00  0x00306950  0x00390310  0x00
  0x07  0x00309d50  0x00307140  0x07  st=0x1 rd=0x6
  0x00  0x0030a3b0  0x00390310  0x00
  0x06  0x00307140  0x00309bd0  0x06  st=0x1 rd=0x5     <- 从 mid 的帧切走
  0x00  0x0030a3b0  0x00390310  0x00
heal=0x00000006 heal_g=0x00000040 heal_word=0x0 ...
task table:
  6: status=0x1 14/14 ready_next=0x05  stack_ptr=0x00309bd0
  7: status=0x3 10/10 ready_next=0xff  stack_ptr=0x00307140
  8: status=0x0  6/6  ready_next=0xff  stack_ptr=0x00306940
```

两条读数：

1. **所有非 idle 的目标，`st` 都是 `0x1`（READY）** ⇒ 「已退出（INVALID）的
   任务被重新调度」这个读法**不成立**。被选中的任务在选中那一刻都是合法的
   就绪任务，问题不在就绪集与 status 脱钩。
2. `heal=6` 说明就绪位图自愈路径确实被走过 6 次，但它只是**结果**：位图会脱钩，
   是因为有任务在「位图上、桶里没有」这种中间态被摘掉。方向还要往前找。

**当前状态：没修。** 只在自杀路径上加了一条**不改行为**的诊断：切回来后
若 `arkrtos_current_task_id` 仍是自己，就累加 `arkrtos_kill_ret_cnt` 并打印
`kill: switch did not take (id=N) -> entry returns`，同时把这个计数打进
fatal 转储（`kill_ret=`）。这条计数非 0，就等于坐实上面第二层的推断；
为 0 则说明切换确实发生过、崩溃来自别处（下一步就该查「谁会 ertn 到一个
已死任务的初始帧」）。

**复现率与边界**：LA64 单 `utest` 15 轮 1 败、双 `utest` 8 会话 1 败 ⇒
约 1/12~1/15。加上 `kill_ret` 诊断后又连跑 **48 会话（每会话两次 `utest`）0 复现**
—— 这既不能读成「诊断代码把它治好了」（诊断不改行为），也不能读成「已经不会
再出」；只能读成「命中率在这个构建上偏低，还需要更长的连跑才抓得到」。
RV32 连跑 12 轮全过、带诊断代码的 RV32 单跑仍 `9 matched, 9 passed`。
**未复现不等于已修。**

**后续（本轮收口）：这条诊断本身是误报，已加闸门。** `arkrtos_task_kill_internal()`
在**中断上下文**里被调用时，那次 `ARKRTOS_SWITCH_TASK()` 是**推迟**执行的：
LA64 的 port 在 `CRMD.IE=0`（trap 内）只置软切换请求，真正的 `ertn` 要等
回到任务上下文。所以「切回来后 `arkrtos_current_task_id` 仍是自己」在
`in_isr` 下是**正确行为**，不是缺陷。修法是把诊断本身收窄：

```c
if (arkrtos_current_task_id == id && arkrtos_port_in_isr() == 0u) { ... }
```

加闸门后 LA64 连跑 10 次 `exec`（11 次任务自杀）**零** `kill: switch did not take`。
代价是 `text +12 B`（RV32 镜像 `2012876 -> 2012924 B` 的唯一来源），与 ELF 无关。
**注意这只关掉了「误报」这条路，不等于 §5.13.4 那个 `era=0` 的间歇异常被解释了**
——那是入口函数真的返回，根因仍未定，欠账照旧。

#### 5.13.4 追加（2026-11-19）：命中一族现场，根因收窄到「启动/idle 栈溢出，写坏正下方相邻任务的初始帧」

**结论分档**：机制已由**两向因果实验**证实（QEMU 实跑）；**修法尚未实施**，待裁定；
§5.13.4 最初那例（`s_pi_chain_stack_mid`）本轮没有再取样核对，只见「同形状」，见文末。

**本轮新增的诊断（默认档，只在崩溃转储里体现）**

- switch-log 由 6 字扩到 8 字：新增「目标帧的 ERA 槽 / SP 槽」，在 `LA_PRIV_APPLY` 之前采集；
- fatal 转储新增帧归属判定
  `frame owner=task N bottom=.. top=.. is_init_slot=? ra0=? sp_is_top=?`
  （帧不属于任何任务栈时打印 `frame owner=none (outside every task stack)`）；
- 任务表每行追加 `guard=<stack_bottom[0]>`（正常值 `0xed01`）。

体积影响（LA64 默认档，`SOURCE_DATE_EPOCH=1600000000`）：
`text 367512 -> 368024`（+512 B），`data 440 / bss 767508` 不变，
ELF `2585608 -> 2586816 B`，sha256 `1ff448e6... -> 7b7929f6...`。
RV32 / LA32 本轮没有重编复核。

**现场（3 次命中，形状完全一致）**

```
[Ark RTOS] FATAL: estat=0x000d0000 ecode=0x0000000d era=0x00000041 badv=0x00000041
                   badi=0x00000000 crmd=0x00000010
 frame@0x003b5710
  [0x00000000] 0x00000000 0x00000000 0x003b5810 0x00000000
  [0x000000e0] 0x00323fe8 0x003240f0 0x00000041 0x00000040
 frame owner=task 4 bottom=0x003b5010 top=0x003b5810
        is_init_slot=0x00000001 ra0=0x00000001 sp_is_top=0x00000001
 ...
 switch-log 最后一条（next frame new_sp cur st rd target-era target-sp）：
  0x00000004 0x003ba890 0x003b5710 0x00000004 st=0x1 rd=0x3
             target-era=0x00000041 target-sp=0x003b5810
```

三条读数：

1. `is_init_slot=1` —— 这一帧就是 `arkrtos_port_stack_init()` 写下的那一格
   （`栈顶 - 0x100`），**不是一次现场保存**。
2. `target-era=0x41`、`target-sp=0x3b5810`（= 栈顶）—— **目标帧在切换之前就已经是坏的**，
   所以不是 resume 时算错，也不是调度器挑错了任务。
3. `ertn` 恢复出 `PC=0x41`（另两例是 `0x8` / `0x0`）、`PRMD` 也被写成 `0x40`
   ⇒ 取指非法 ⇒ `ecode=0x0d (INE)`。

**修订既有读法：`ra=0` + `sp==栈顶` 不能读成「入口函数返回了」**

这两个值**正是 `arkrtos_port_stack_init()` 写出来的形状**（写 `ra=0 / sp=栈顶 /
ERA=entry / PRMD=4`）。在本例里恰好相反：这一帧**一次都没被用过**，是被外来写覆盖的。
§5.13.4 正文里「入口函数真的返回了」的第二层推断，对那一例是否有独立证据，需要单独复核。

**谁写坏的：正下方的 idle / 启动栈溢出**

- `s_stack_guard_demo` 占 `0x3b5010..0x3b5810`（512 字），初始帧槽在 `0x3b5710`；
  **紧贴它上方**就是 `s_idle_stack`（`0x3b5820`，`IDLE_STACK_WORDS=256u` ⇒ 1 KB）。
- `arkrtos_port_enter_idle()` 只是 `move sp, a0`（`arkrtos_context.S`），`main()`
  之后只剩 WFI 循环，所以**启动上下文此后就住在 idle 栈上**。
- `eentry` 在 `bl la_trap_c` 时**仍然用被打断者的 sp**（换到 `la_switch_stack_top`
  发生在 `la_trap_c` 返回之后）⇒ **在启动/idle 上下文里被 tick 打断时，整条内核陷阱链
  （tick handler + guard + timer + 调度扫描）都跑在 idle 栈上**。与 §5.14 的软定时器栈
  是同一个形状。
- 溢出顺序正好对得上：`0x3b5818(s_a_loops) → 0x3b5814(s_b_loops) → 0x3b5810(s_fs_ok)
  → 0x3b5808(frame[31] = PRMD) → 0x3b5800(frame[30] = ERA)`，**只改最后两格、
  其余 30 字仍是初始帧的 0**，与现场一致。

**水位实测（HMP `xp` 直接读物理内存）**

未写过的字是 `0`（`s_idle_stack` 是 `main.c` 的 `static` 数组，在 `.bss`，
不走 `0xcdcdcdcd` 预填）。用 4 KB 的对照镜像（`IDLE_STACK_WORDS=1024u`）跑完一次
`utest` 后读 `0x3b5820..0x3b6820`：最低被写字 = `0x3b5840` ⇒ **峰值 960 B / 4096 B**。
换回默认 1 KB 栈，同一峰值 ⇒ **只剩 64 B 兜底**，而踩到 `frame[31]` / `frame[30]`
只需再深 24 B / 32 B。这就解释了「偶发」：绝大多数 tick 的链路深度一样（960 B），
偶尔多一层就越界，且越界写进去的总是同一段深链路的同一批局部量 ——
与「每次两个坏值都相同」一致。

**两向因果实验**（同一份源码，只改 `example/qemu_loongarch64/main.c` 的 `IDLE_STACK_WORDS`）

| `IDLE_STACK_WORDS` | 栈大小 | 本轮实跑 | 结果 |
| --- | --- | --- | --- |
| `128u` | 512 B | 5 次 boot | **5/5 崩**。3 例是上面那条完全一致的形状；另 2 例溢出更深、踩到了别的 `.bss`（一例把 `sp` 变成 `&s_next_pplv`、PC 落在 `arkrtos_dev_handles`）⇒ 溢出深度决定踩坏什么 |
| `256u`（默认） | 1 KB | 8 次 boot × 3 轮 `utest` | 0 崩（与既有的约 1/15 相符；**0 复现不等于已修**） |
| `1024u` | 4 KB | 12 次 boot × 3 轮 `utest` | **12/12 干净，13/13/0 `utest`** |

两向都成立 ⇒「idle 栈溢出写坏相邻任务的初始帧」这条因果链成立，不是相关。

**修法（当时三候选；复核后落地为 1 + 2，见本节末尾「复核」）**

1. **加高 idle 栈**：`IDLE_STACK_WORDS 256u -> 1024u`（示例层一行，本轮已实测足够）。
   代价：`.bss +3 KB`；只是把余量从 64 B 拉到 3.1 KB，**没有把真实需求（960 B）表达出来**。
2. **栈底隔离带 + 自检**：`arkrtos_port_stack_init()` 不清零底部若干字，或每块栈底预留
   16~32 B 隔离区，并在 tick 路径里比对。能把「已经越界」当场变成可读的数，而不是等崩。
3. **架构层**：让 idle/启动上下文不进陷阱链 —— 即 `eentry` 在 `bl la_trap_c` 之前就换到
   静态陷阱栈。但「帧存在任务栈上」正是 resume 的载体，改动面最大。

推荐 1 + 2：先按实测给足（1 KB → 4 KB），再用隔离带/自检把余量变成读数。

**仍未解释**：§5.13.4 最初那例（`frame@0x003071d0`、`s_pi_chain_stack_mid`）
本轮没有再取样核对，只确认它与本例**共享同一个形状**（初始帧槽 + 垃圾 ERA）。
它是否同一根因，需要单独复跑判定；在那之前，欠账不能整体销账。

**复核（2026-11-19 晚）：撤回「idle 栈只用了 108 B」，① 与 ② 同时落地**

上一轮实施 ①+② 时顺带加了一条在线水位读数 `stack ctx:`，据此报出「idle 栈实测
只用 108 B、从未溢出」并请求撤回 ①。**该读数是错的，错在水位扫描算法本身**；
修正后 ① 重新成立。三处一并更正，免得以后拿错数做决定：

1. **扫描停在第一个图案字**。旧实现从 `top` 向下逐字探，碰到第一个
   `0xcdcdcdcd` 就停。实测 rv32 的陷阱帧顶部恰好留 8 字节没写过
   （`0x801993f8/fc` 仍是 `0xcdcdcdcd`），于是 `low` 停在 `top`、`used` 报 0——
   既低报用量，也会让真正触底时**一声不响**。新实现扫完 `[bottom, low)` 全段、
   取最低的非图案字（每拍至多 `stack_words` 次 32 位读，1 KB 栈即 256 次）。
2. **布防要求 16 字节对齐**，而 rv32 的 `s_idle_stack` 落在 `0x8019900c`
   （4 字节对齐），`watch_context` 于是**静默拒绝布防**，shell 里连一行读数都没有。
   现在放宽为字对齐（填图案与扫描本就按字做），且布防被拒时 shell 显式打
   `stack ctx: not armed (watch_context refused: ...)`——静默失败比没有守卫更坏。
3. 因此上一轮那张「108 B」读数表、以及基于它的「① 失去依据」结论**全部作废**。

**修正后的实测**（`stack ctx:` 内核读数 + QEMU monitor `xp` 直读双通道，逐字节一致）

| 目标 | `IDLE_STACK_WORDS` | 栈容量 | used | free |
| --- | --- | --- | --- | --- |
| LA64 qemu | `256u` | 1012 B | 824 B | 200 B |
| LA64 qemu | `1024u`（**采用**） | 4088 B | **904 / 984 B** | 3192 / 3112 B |
| RV32 qemu | `256u` | 1012 B | 504 B | 508 B |
| RV32 qemu | `1024u`（**采用**） | 4088 B | 504 B | 3580 B |
| LA64 qemu | `128u` | 512 B | **溢出** | 0 |

- LA64 真深度在 **904~984 B**（两次不同 boot），与本段上一版「4 KB 对照镜像 HMP
  读 960 B」互相印证；`256u` 那个 824 B 是**被 1 KB 截断的读数**，不是真深度。
- `128u` 一档在修正后的扫描下由守卫当场报出
  `[STK] context stack overflow low=0x003b5820 margin=0B` ⇒ ② 会响，不是纸面开关。
- RV32 两档读数完全一致（504 B），说明它与栈大小无关，不是截断值。

**② 的实现**（`ARKRTOS_USE_STACK_GUARD == 1` 才编入；默认档仍是 0，逐字节不变）

- `arkrtos_stack_guard_watch_context(bottom, size, top)`：在 `enter_idle()` **之前**
  调用（那一刻这块栈还没被当栈用），把 `[bottom, top)` 整段填
  `ARKRTOS_STACK_FILL_PATTERN`；参数不成立（尺寸 < 256 B / 非字对齐 /
  `top <= bottom` / 跨度大于 size）就不布防。
- `arkrtos_stack_guard_check_context()`：节拍体里每拍扫一次；余量低于
  `ARKRTOS_STACK_GUARD_CTX_MARGIN`（默认 64 B，可板级覆盖）或触底时**只报一次**
  （`arkrtos_fault_record(ARKRTOS_FAULT_STACKOVF, 0)` + 串口
  `[STK] context stack low margin|overflow low=0x... margin=NB`）。
- `stack` 命令新增一行汇总：`stack ctx: idle/boot 0x<b>..0x<t> low=0x<l>
  used=N free=M`。
- 三档示例（la64 / la32 / rv32）都在 `enter_idle()` 前布防；la32 与 rv32 的板级
  配置本轮把 `ARKRTOS_USE_STACK_GUARD` 置 1，否则拿不到读数。

**① 的落地**：三档 `IDLE_STACK_WORDS` 统一 `1024u`（4 KB）。依据是实测需求
904~984 B：1 KB 只剩 28 B，512 B 已实测溢出。LA32 没有可用的系统模拟器
（QEMU 9.2.3 不提供 la32），取与 la64 同档，如实标注**未实测**。

**体积记账（LA64，`SOURCE_DATE_EPOCH=1600000000`）**

| 配置 | text | data | bss |
| --- | --- | --- | --- |
| `ARKRTOS_USE_STACK_GUARD=0`（板级关档） | 367320 | 440 | 768532 |
| `=1`（本板现状，含 ② 与 `stack ctx`） | 369216 | 440 | 770580 |

差额 `text +1896 / bss +2048`，其中 2 KB 是板级栈守卫演示任务
（`s_stack_guard_demo`，512 字）——② 自身新增的静态量只有 5 个字
（`s_ctx_guard_{armed,reported,bottom,top,low}`，14 B，按对齐取整）。
关档镜像里 `s_ctx_guard` 与 `context stack` 一个字都不出现（门禁 C 段钉死），
`stack` 命令输出与改动前一致（实跑核对：8 个任务、无 `stack ctx` 行）。

**判词（更正后）**

- 本节原判词「启动/idle 栈溢出写坏紧邻任务的初始帧」**成立**：两向因果实验与
  904~984 B 的实测深度互相支持。「只用了 108 B」的撤回**撤销**，该读数定性为
  「水位扫描缺陷造成的低报」，不再引用。
- 但**另一族现场仍在**：本次复核在 4 KB 档上也复现了一次
  `[LA64U] KERNEL refill (plv0) badv=0xffffff00 era=0x00244d84`（`eentry` 建帧时
  `sp=0`，`addi.d sp, sp, -256` 得 `0xffffff00`），形状与「初始帧被写坏」不同。
  因此 §5.13.4 的欠账**只销掉一族**，「切换路径上的 sp 有效性」继续挂着。
  **2026-11-19 晚补记**：这另一族的真根因已定位并修好（恢复序列的关中断 `csrxchg` 掩码写成 1，清的是 `CRMD.PLV` 而不是 bit2 的 `CRMD.IE`），QEMU 实跑 0/48 fatal；详见本章末「[5.13.4 收口（2026-11-19 晚）](#5134-收口2026-11-19-晚kernel-refill-plv0-badv0xffffff00-的真根因是关中断掩码写错)」。

### 5.14 软定时器栈装不下陷阱帧：KERNEL refill 的真根因（已修）

**现象**：接上 ELF 装载器后，第 2 次 `exec /mnt/ram/hello` 必崩：

```
[LA64U] KERNEL refill (plv0) badv=0x01000000 era=0x0020b9d8
```

而串口最后打出来的那一行 `FATAL: ecode=0x0b era=0x02000558` **是陈旧的**：
本移植把 `0xB` 当 SYS，`ESTAT` 是上一次 syscall 留下的读数；`TLB refill` 只更新
`TLBRERA/TLBRBADV/TLBRPRMD`，不碰 `ESTAT`。按 FATAL 行的 `ecode` 去查会查错方向。

**四步证据链**（每步都是读数，不是推断）：

1. `nm` + `task` 命令：软定时器服务任务栈 `arkrtos_timer_task_stack` 在
   `0x00304C30`，`ARKRTOS_TIMER_TASK_STACK_WORDS=96` ⇒ 384 B；
   `task` 报 `peak/low = 0x00304C60` ⇒ 已用 336 B，**只剩 48 B**。
2. 它一回场，第一个 tick 中断把 **256 B 陷阱帧写到 `sp - 256 = 0x00304B60`**。
3. 帧内第 18 个字正好落在 `0x00304BA8` = `g_hwsafe_dev_n`，被写成
   `0x0021BCA8`（某个返回地址）。
4. 随后 ELF 程序 `open("ttyS0")` 走 `hwsafe_find_fn`，拿 `dev_n = 2210696`
   当循环上界，一路走到**未映射**的 `0x01000000` ⇒ `KERNEL refill`。

**反证**：把栈临时放大 8 倍（768 字）后连续 6 次 `exec` 不崩；96 字必崩。

**修法**：不改共享 defconfig（那会动 RV32 镜像基线），按项目既有风格在**板级头**
覆盖：

```c
#undef  ARKRTOS_TIMER_TASK_STACK_WORDS
#define ARKRTOS_TIMER_TASK_STACK_WORDS (512)   /* 2 KB，LA64 帧 256 B */
```

`board/qemu_loongarch64/arkrtos_board_config.h` 取 `512`，
`board/la32_generic/arkrtos_board_config.h` 取 `256`（LA32 帧 128 B）。
`nm` 复核 `arkrtos_timer_task_stack` 变为 `0x800`。

**可复用的口径**：**这个移植的陷阱帧落在任务栈上**，所以任何「会跑用户回调的
小栈」必须按「峰值 + 一帧上下文 + 处理链」给足，而不是按「函数调用深度」给。
`arkrtos_config.h` 里的 96 字是按 32 位端口（帧 128 B）定的，照搬到 64 位端口
就正好差一帧。

**修后读数**：连续 10 次 `exec`（11 次任务自杀、11 个 `bye`）**零 FATAL**；
再跑一轮 `exec one two` + 5 次 `exec a<i> b` + 直接敲 `/mnt/ram/hello direct`，
末态 `hwsafe: enabled, devs=0 owners=2 neutralized=8 ... last_owner=7`
（修前 `devs` 是天文数字）。RV32 仍是 96 字，实测 `peak/low=0x80104A80`
⇒ 已用 184 B / 剩 200 B，帧 128 B，处理链只剩 ~72 B —— **同型风险，本轮只登记
不修**（改它会动 `2012924 B` 基线），见 [MMU进程模型与ELF装载](MMU进程模型与ELF装载.md)
§6 欠账 U-12。

### 5.15 栈底守卫：把「写了不读的标志字」接上（U-13，2026-11-17）

**问题**：每任务栈底的标志字 `ARKRTOS_STACK_END_FLAG_VAL`（`0xed01`）一直写，
但**没有任何代码读它**。已有的 `ARKRTOS_USE_STACK_CHECK` 判的是「保存的 PSP 是否
低于栈底」，这只在「栈指针一路压下去」时成立。§5.14 现场的破坏方式是**陷阱帧
直接写到 `sp - 256`**（栈指针本身还没越界）、或野指针 / DMA 直接改写了栈底字 ——
这两种 PSP 判据全都看不见。

**开关**：`ARKRTOS_USE_STACK_GUARD`（`Kconfig.features`，default `n`）。
与 `ARKRTOS_USE_STACK_CHECK` 不是同一件事，两者可独立开关。

**判据**：读 `stack_bottom[0]` 是否仍等于 `0xed01`。不等即命中。

**两个落点**（都是「已经来不及但还救得回来」的位置）：

1. 调度切换路径（`arkrtos_task.c` 的 `arkrtos_sched_activate`，所有 port 共用）：
   命中后打一行 `[STK] task N: stack bottom flag overwritten`、记一次
   `arkrtos_fault_record(ARKRTOS_FAULT_STACKOVF, slot+1)`，并把该任务从就绪集
   摘除、置 `ARKRTOS_TASK_INVALID`（只打日志等于没有保护）。
2. LA64 / RV32 的 C trap 入口（`s_in_trap++` 之后）：这两个 port 的陷阱帧压在
   被中断任务的栈上，正是 §5.14 那种「一帧吃掉栈底」的现场。trap 里只记录与打
   日志、**不改调度状态**（还在异常上下文里换栈不安全），摘除留给下一次切换。

每任务两个标志位 `stack_guard_armed`（布防，注册时置 1）与
`stack_guard_reported`（只报一次，防每拍刷故障环）。关档时字段、原型、调用、
字符串全在 `#if` 内 ⇒ **零符号、零体积变化**。

**QEMU LA64 实测**（编入档 `board/qemu_loongarch64/arkrtos_board_config.h` 置 1，
示例里挂一个「延时 200 tick 后把栈底字写成 0 再让出 CPU」的任务）：

```
[boot] stack guard demo: bottom word clobbered
[STK] task 4: stack bottom flag overwritten
```

shell `fault` 命令读到故障环：

```
$ fault
#   tick      task  type
 0   165       4     STACKOVF
```

交互驱动已固化为可复跑的一步：

```
python tools/qemu_shell_drive.py --stack-guard-check --boot 45 --port 45571
SHELL-DRIVE OK: 1 条命令全部符合预期
```

**一个踩过的坑**：演示任务栈最初给 `128` 字（512 B）。LA64 一帧陷阱 256 B 加上
调用链，这个任务**还没踩到自己的栈底就把自己的调用帧写飞了**（`KERNEL refill
badv=0x13d648d8 -> fatal`）。改成 `512` 字后 `[STK] task 4` 正常出现 —— 测守卫
的任务栈本身必须够深，否则测的是它自己越界。

**门禁**：`tools/stack_guard_gate.py`（ci_gate 第 40 项），A 默认档 autoconf 为 0；
B LA64 开档预处理后 `arkrtos_task.c` 里必须有检查函数与故障码；C LA64 关档预处理
结果里 `stack_guard` 一个字都不许有；D RV32 端口开档必须编得过；E 逐条 pin 住
判据、切换调用、置 INVALID、注册布防、TCB 字段、features 回退与两块板级接线；
F 板级置 1 且示例有踩栈底任务。


#### 5.15.1 守卫抓到的第一个真问题：shell 的 3 KB 栈装不下 `ld`

把守卫编进档之后，`--modld-check` 反而挂了：`ld` 之后 shell 的输出整段消失，
`modules` 只剩一个 `[`，串口里在模块输出之前多出一行 `[STK] task 8`。

按「先量后改」查下来，这不是守卫误报：

```text
$ stack                                      # 空载
 8   3072     2452     620       79%   0x0021F060     ← 控制台任务（SHELL_TASK_STACK_SIZE）
$ ld /mnt/ram/modhello.o a b
$ stack                                      # ld 之后
 8?  3072     3072     0        100%   ...          ← 整段栈被用满，护底字已被写穿
```

- 任务号会随槽位复用变（这里出现过 7 和 8），判据是 `total=3072` 那一行；
- `used=3072` 是**扫描法能给出的上限值**，换句话说是「至少 3 KB，真实需求测不出来」；
- 不带守卫时这层溢出**静默**写进 shell 栈下方的静态区。

**连带暴露的一处假读数**：`--modld-check` 里原有的期望
`[modhello] argv[0]=modhello.o` 一直"通过"，但内核从来没做过 basename——
`cmd_ld` 把 `argv[1]` 原样交给装载器（VxWorks 的 `ld` 也是这样）。当时打印出
basename，是溢出把装载器静态区里的 argv 指针写坏了、恰好指到路径的第 9 个字符。
把栈修好之后同一行变成 `argv[0]=/mnt/ram/modhello.o`，即用户敲进去的那个路径；
驱动里的期望已改成完整路径，并在注释里写清这条旧读数是被破坏内存凑出来的。

**修法**（只动这块板，默认档零变化）：`arkrtos_shell.c` 给栈大小加一个板级钩子

```c
#ifndef ARKRTOS_SHELL_STACK_SIZE
#define ARKRTOS_SHELL_STACK_SIZE    SHELL_TASK_STACK_SIZE
#endif
static uint32 g_shell_stack[ARKRTOS_SHELL_STACK_SIZE / 4u];
```

`board/qemu_loongarch64/arkrtos_board_config.h` 取 `1024 * 8`。
修后读数：`ld` 跑完 shell 任务 `total=8192 used=3956 (48%)`——真实需求约 3.9 KB，
比原来的 3 KB 多出一次 `ld` 的链接链，原来的 79% 空载占用就已经很贴边了。

**结论口径**：这条不是「守卫太敏感」，而是「守卫把一段潜伏的静默破坏变成了可见的
任务摘除」。同型的 32 位端口（3 KB、帧 128 B）本轮**没测**，用 `stack` 在 RV32
QEMU 上跑一次 `ld` 即可判定，登记为待复核项。

### 5.16 运行动态映射：驱动登记 MMIO 跨度（欠账 #5，2026-11-17）

到本轮之前，`arkrtos_port_paging_alias()` 是一个**恒返回 -1 的占位**：注释写"运行期映射
未做"，于是「驱动在 MMU 档下访问一个静态映射表里没有的 MMIO 窗口」这件事在架构上无解。
本轮把它变成真实现，并给出 QEMU 上的实测读数。

#### 5.16.1 接口与契约

```c
/* kernelsrc/include/arkrtos_mpu.h */
int32  arkrtos_paging_map_span(uint32 va, uint32 pa, uint32 size, arkrtos_mpu_mem_t mem);
uint32 arkrtos_paging_map_pages(void);
```

- `map_span()` 把 `[va, va+size)` 逐 4 KB 登记到 `[pa, pa+size)`；
  **返回 0 表示每一页都进了 TLB 且都进了审计跨度表**，`<0` 是拒绝。
- 只支持 **4 KB 粒度**：`va`/`pa`/`size` 任一不是 4 KB 对齐、`size==0`、
  `size > 16 页`（`ARKRTOS_PAGING_MAP_MAX_PAGES`）一律 -1。
- **只做内核可见（PLV0）映射**。App 可见的动态映射需要 ASID 或每进程表 —— 那是 U-1，
  在这里近似会给出「看似权威的错答案」，所以明确拒绝而不是假装支持。
- **失败不回滚**：`map_span()` 中途失败时已登记的页会留在表里（没有 re-map 就没有回滚）；要收回就调 `unmap_span()`（U-15，§5.18）。
  调用方要按「要么整段成、要么当作部分成功」处理，返回值只说明断在哪一页。
- 拒绝码由 port 原样透出，不压成 -1：

  | 码 | 含义 |
  |---|---|
  | -1 | 参数/属性不支持（非 4 KB 对齐、size 非法、`mem` 不是 RAM/ROM/PERIPH_RW/PERIPH_RO） |
  | -2 | 与静态映射跨度重叠（`s_pg_spans[]` 里已有） |
  | -3 | 动态表满（`LA_PG_DYN_PAIRS` = 8 对） |
  | -5 | 同一 VA 已登记成**别的** PA 或别的属性；一字不差的重复登记是幂等返回 0 |

- `arkrtos_paging_map_pages()` 返回累计登记页数（只增不减，因为没有撤销）。

#### 5.16.2 根因：`tlbfill` 在 STLB 里随机选路，会顶掉内核 UART 条目

第一版实现直接沿用 `la_tlb_fill()`——它走 `tlbfill`。LoongArch 的 `tlbfill` 在
`PS == STLBPS(12)` 时**从 STLB 的 8 路里随机挑一路**写。内核 UART 的静态条目
（VA 0x1FE00000，组号 `(VA >> 13) & 0xFF = 0`）与测试 VA 0x03000000 **同组**，
于是登记完成、控制台下一次打印就死：

```text
KERNEL refill (plv0) badv=0x1fe001e5 -> fatal
```

（`0x1fe001e5` 正是 UART LSR 的地址，读它触发了重填，而 UART 条目已被顶掉。）

这不是"偶发"，是选路的性质：运行期反复写 4 KB 条目、且无法指定落哪一路，
**必然**会与同组的静态条目互相顶掉。

#### 5.16.3 修法：确定性 `tlbwr` + 落 MTLB

新增 `la_tlb_write(vaddr, ps, index, lo0, lo1)`——全部走 `tlbwr`（索引由调用方给，
行为确定）。动态 4 KB 条目**刻意写进 MTLB**（绝对索引 `LA_MTLB_INDEX + 3 + slot`，
即 2048 起）而不是 STLB：

- MTLB 的索引是确定的，`tlbwr` 写的就是那一条；
- MTLB 查找按条目自己的 PS 比 VPPN，不与内核静态 4 KB 条目抢 STLB 的组预算；
- 静态大页已占了 2048..2050，所以动态从 `2048 + 3` 起。

代价是 MTLB 总共只有 64 条，动态表上限被它钉死：`LA_PG_DYN_PAIRS` = **8 对**
（一对 = 偶页 + 奇页，因为一条 TLB 条目管 LO0 和 `LO0 + 2^PS`）。

**未登记的那一半必须写 0（V=0）**：绝不"顺手放开"邻居页。

修后同一段自测：`pmap: compare 16 words, mismatch=0` / `pmap test: PASS`，控制台不再死。

#### 5.16.4 审计侧：`check()` 也认动态条目

`arkrtos_port_paging_check(va, size, mem)` 末尾加了动态扫描：整段必须落在**同一页内**，
且该页的权限掩码必须覆盖 `mem`。**RW 覆盖 RO**（`rw` 登记过的页按 `ro` 查是 0，
按 `ram` 查是 -1）。

#### 5.16.5 `pmap` 子命令与实测 transcript

```text
pmap                                     读数（init_rc / checked / bad / bad_base / pages）
pmap map  <va> <pa> <size> <ram|rom|ro|rw>
pmap check <va> <size> <ram|rom|ro|rw>   审计探针（跨页/未映射都返回 -1）
pmap rd   <va> [count]                   经映射读（每行一个字，最多 8 个）
pmap rb   <va>                           经映射读一个字节
pmap wrb  <va> <byte>                    经映射写一个字节（MMIO 才用）
pmap test                                自测：内核 ROM 页映射后逐字比对 16 个字
```

`rd/rb/wrb` **不做保护性检查**——地址没映射就是一次重填致命。这是刻意的：
用它们验证"通不通"，不是验证"该不该"。

QEMU LA64 实跑（`tools/qemu_shell_drive.py --port <p>`）：

```text
$ pmap
pmap: init_rc=0 checked=0 bad=0 bad_base=0x00000000 pages=0

$ pmap test
pmap: map ROM 0x03000000 <- 0x00200000 rc=0
pmap: compare 16 words, mismatch=0
pmap test: PASS

$ pmap map 0x03000000 0x00300000 0x1000 ram      # 同一 VA 换 PA
pmap: map 0x03000000 <- 0x00300000 size=4096 rc=-5 pages=1

$ pmap map 0x02000000 0x00300000 0x1000 ram      # 落在 App 静态跨度内
pmap: map 0x02000000 <- 0x00300000 size=4096 rc=-2 pages=1

$ pmap map 0x03002000 0x1FE00000 0x1000 rw       # 真设备页：串口
pmap: map 0x03002000 <- 0x1FE00000 size=4096 rc=0 pages=2

$ pmap rb 0x030021E5      -> pmap: [0x030021E5] = 0x60     # UART LSR，经新 VA
$ pmap rb 0x1FE001E5      -> pmap: [0x1FE001E5] = 0x60     # 直连，同值
$ pmap wrb 0x030021E0 0x5A -> 0x5A 写进 UART THR，串口上真的打出 'Z'

$ pmap check 0x03002000 0x1000 ro  -> rc=0      # RW 覆盖 RO
$ pmap check 0x03002000 0x1000 ram -> rc=-1     # 属性不符
$ pmap check 0x03003000 0x1000 rw  -> rc=-1     # 未映射

$ pmap map 0x03004000 0x00300000 0x2000 ram -> rc=0 pages=3   # 两页一次登记
$ pmap test（第二次）-> 仍 PASS（幂等），pages 累计递增
```

判读要点：

1. **`-5` 是被实测出来的真缺陷**——第一版没有「同一 VA 换 PA」检查，
   `pmap map 0x03002000 0x00300000 ...` 返回 rc=0 并**真的改掉翻译**，
   随后 `wrb` 打到的是内核 RAM 而不是 UART。加上 `pa[2]` 比对后才有 -5。
2. **UART 的两条读路径同值**（`0x60`）说明动态条目真的在起翻译作用，
   不是"碰巧映射到同一块内存"。
3. **`wrb` 打出 'Z'** 说明写方向也通（不是只读副本）。

回归（同一次改动）：

```text
$ python tools/qemu_shell_drive.py --port <p> --modld-check
SHELL-DRIVE OK: 7 条命令全部符合预期

$ python tools/qemu_shell_drive.py --port <p> --stack-guard-check
SHELL-DRIVE OK: 1 条（0 173 4 STACKOVF）

$ python tools/paging_gate.py
PAGING-GATE OK: 默认档=0 / LA64 打开编得过(2 TU) / RV32 负向被拒 /
两后端互斥被拒 / 动态映射接线 E-H 四条 pin 成立且自检有效
```

#### 5.16.6 门禁：E~H 四条 pin 与自检

`tools/paging_gate.py` 新增四条**源码 pin**（每条都把源串改坏一次，验证 pin 真的会报错，
不是空转）：

| pin | 断言 |
|---|---|
| E | `arkrtos_paging.c` 定义 `arkrtos_paging_map_span(`，且函数体内调 `arkrtos_port_paging_alias(` |
| F | port 有 `static void la_tlb_write(`；alias 函数体含 `la_tlb_write(`、不含 `(void)va;`（占位实现残留） |
| G | `#else` 回退：`arkrtos_hal.h` 的 alias 恒 -1、`arkrtos_mpu.h` 的 `map_span` 恒 -1 |
| H | shell 注册了 `pmap`，且 `#if (ARKRTOS_USE_PAGING == 1)` 出现在注册点之前，且 `need` 里含 `(ARKRTOS_USE_PAGING ? 1 : 0)` |

pin F 的自检第一版是**空转**的：变异串 `la_tlb_write(pair, 12u` 在 alias 函数体里出现两次，
`replace(..., 1)` 只改掉幂等分支那一处，pin 仍通过。改成替换全部出现才有效。

#### 5.16.7 明确没做的部分

- **撤销 / 回收映射**：本轮做完了（U-15，见 §5.18）。原先担心「按 VA 发 `invtlb` 会把
  内核条目一起打掉」这条风险是实打实的，所以实现走的是「先按 VA 失效、再按 MTLB 绝对
  索引写回 V=0 条目」这两步的组合，且静态跨度一律拒绝（-2）。`arkrtos_port_paging_flush()`
  （整表 flush）仍是空操作。
- **App 可见的动态映射**：属 U-1（每进程页表），接口一律拒绝。

### 5.17 每 App 地址空间：同一 VA、两块物理帧（U-1，2026-11-17）

#### 5.17.1 本 port 没有硬件页表遍历，所以「每进程页表」= 每进程一组条目

QEMU 的 LA64 板（`cpu->ptw = OFF`）不会走内存里的页表，翻译只来自 TLB。
所以 U-1 在这里只能落成：**每个 App 地址空间占一条 App 窗口条目，切换时重写
那一条**，而不是「每个进程一张可遍历的页表」。

- 空间号即帧号：`space n → PA = ARKRTOS_QEMU_APP_BASE + n * ARKRTOS_QEMU_APP_SIZE`
  （2 MB 步进，space 0 = `0x02000000`、space 1 = `0x02200000`）。
- VA 在每个空间里完全一样（`0x02000000`），所以一个 App **连别的 App 的帧都叫不出来**
  —— 不是"越权被拒"，是那个地址根本不存在。
- 帧是板级固定数组（`ARKRTOS_PAGING_APPS_MAX`），**缺省 1**：此时 `activate()`
  在 `space == s_pg_app_space` 处直接返回，调度路径与改动前零差别。
- 内核任务（`is_priv != 0`）不切空间：它们跑在内核恒等映射上，要写某个帧就显式
  `arkrtos_paging_space_activate()`。切换发生在 `arkrtos_mpu_set_app()` 里，而
  App 任务的 `is_priv == 0`，所以只有 App 会带走空间。
- ELF 装载器落在**当前装着的那个空间**：写完段之后把新任务绑到同一个空间号
  （`arkrtos_paging_bind_task()`），否则会出现「写的是 frame A、跑的是 frame B」。

| 层 | 接口 | 说明 |
|---|---|---|
| port | `arkrtos_port_paging_activate(space)` | 0 = 成功/已在；-1 越界；**-2 = 写完读回对不上** |
| port | `arkrtos_port_paging_space()` / `space_frame(n)` / `space_count()` | 当前空间 / 帧基址 / 板级给的个数 |
| port | `arkrtos_port_paging_lookup(va, &idx, &ppn, &ps)` | 只读诊断：这个 VA 由哪条条目翻译 |
| paging | `arkrtos_paging_space_count/space_frame/space_current/space_activate` | 后端与 port 解耦，窗口档恒 1 |
| paging | `arkrtos_paging_bind_task(task, space)` / `space_of(task)` | 绑定只存号，切换发生在下次调度 |
| shell | `pmap space [n]` / `pmap look <va>` / `pmap u1` | 读数、切空间、看条目、跑自测 |

#### 5.17.2 根因：QEMU 的 `tlbwr` 不刷新大页的**奇页**

第一版按「重写索引 2050 那条 PS=20 条目」实现，`pmap u1` 直接给 FAIL：

```
pmap u1: va=0x02100000 space0=0xA5A50002 space1=0xA5A50002
pmap u1: FAIL (1 of 2 reads wrong)
```

`space 1` 写入的值在 `space 0` 里读得到，但 `pmap space 1` 又报 `rc=0`
—— 状态说切了，翻译没换。根因不在我们这边，在 QEMU：

`target/loongarch/tcg/tlb_helper.c` 的 `invalidate_tlb_entry()` 算失效地址时，
**偶页**用 `& ~mask`、**奇页**却写成 `& pagesize`（应为 `| pagesize`）：

```c
pagesize = MAKE_64BIT_MASK(tlb_ps, 1);            /* ps=20 -> 0x1FFFFE */
mask = MAKE_64BIT_MASK(0, tlb_ps + 1);            /*      -> 0x1FFFFF */
if (tlb_v0) { addr = (tlb_vppn << 13) & ~mask; ... }   /* 0x02000000：对 */
if (tlb_v1) { addr = (tlb_vppn << 13) & pagesize; ...} /* 0x02000000 & 0x1FFFFE = 0：错 */
```

于是 PS=20 条目**上半 1 MB**（奇页）的 softmmu 翻译从不失效：`tlbwr` 把 TLB 条目
改成 frame 1 了，可 CPU 取指/读写走的是 QEMU 的翻译缓存，仍然指向 frame 0。
`pmap u1` 正好把探针放在 `base + 1 MB`（奇页中段），所以每读必错。

修法：切换前按 VA 发一次 `invtlb` op=3（PAGEASID）：

```c
static void la_tlb_inval_page(uint32 va)
{
    __asm__ volatile("invtlb 3, $zero, %0" :: "r"((unsigned long)va) : "memory");
}
```

`helper_invtlb_page_asid()` 结尾是**整表** `tlb_flush()`，所以缓存一起清干净。
顺序必须是「先失效、再 `tlbwr`」：`invtlb` 是按**旧条目**的 PS 匹配 VA 的。
代价只有切换那一瞬（`APPS_MAX == 1` 时根本走不到这里）。

#### 5.17.3 `pmap look`：读回诊断，以及 `tlbsrch` 不写 `TLBELO` 的坑

`arkrtos_port_paging_lookup()` 用 `tlbsrch` + `tlbrd` 读回一条 VA 命中的条目。
第一版只做了 `tlbsrch`，读 `CSR_TLBELO0` 当 PPN —— 那是**上一次 `tlbwr` 留下的
陈旧值**：`tlbsrch` 只写 `TLBIDX`（INDEX/NE），条目的 VPPN/PS/LO0 要用 `tlbrd`
按 `TLBIDX.INDEX` 读回。少了这一步，`activate()` 里的读回校验会"永远对上"，
是标准的假绿。现在的形态：

```
pmap look: va=0x02000000 entry=2050 ps=20 ppn=0x02000000
pmap: space 1 rc=0 (current=1)
pmap look: va=0x02100000 entry=2050 ps=20 ppn=0x02200000
```

（条目号 2050 = MTLB 绝对号 `LA_MTLB_INDEX + 2`；`tlbrd` 会顺带把 `CSR_ASID`
改成条目里的 ASID，本 port ASID 恒 0，所以无害。）

#### 5.17.4 读数

```
$ python tools/qemu_shell_drive.py --boot 50 --port <p> --cmd \
    "pmap space;pmap look 0x02000000;pmap u1;pmap test;exec /mnt/ram/hello a b c"

pmap: spaces=2 current=0
  space 0 -> frame 0x02000000
  space 1 -> frame 0x02200000
pmap look: va=0x02000000 entry=2050 ps=20 ppn=0x02000000
pmap u1: va=0x02100000 space0=0xA5A50001 space1=0xA5A50002
pmap u1: frame0=0x02000000 frame1=0x02200000
pmap u1: rc0=0 rc1=0
pmap u1: look0 entry=2050 ps=20 ppn=0x02000000
pmap u1: look1 entry=2050 ps=20 ppn=0x02200000
pmap u1: PASS - same VA, two frames
pmap: compare 16 words, mismatch=0
pmap test: PASS
[hello] argc=4
[hello] argv[1]=a
[ELF] /mnt/ram/hello: finished after 0 ms
```

另外验过「在 space 1 里 `exec`」：程序在 frame 1 里装载并跑完（`argv[1]=z` 照常），
说明装载点与运行空间被绑在一起，而不是各说各话。

#### 5.17.5 门禁：I~M 五条 pin

`tools/paging_gate.py` 在 E~H 之后新增 I~M 五条源码 pin（与其余几条一样，每条都把源串
改坏一次验证 pin 会报错；J 另有反向断言的自检）：

| pin | 断言 |
|---|---|
| I | port 有 `la_tlb_inval_page()` 且函数体含 `invtlb 3`；`activate()` 体内**先** `la_tlb_inval_page(` 再 `la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u` |
| J | `set_app()` 体内调 `arkrtos_port_paging_switch_to(` **且不出现** `arkrtos_port_paging_activate(`（后者就是 U-2 的崩法）；`arkrtos_hal.h` 关档回退 `lookup` 恒 0；`arkrtos_mpu.h` 有 `arkrtos_paging_space_activate(` |
| K | port 有 `arkrtos_port_paging_switch_to()` 定义且体内按 `la_sp_in_app_window()` 分岔、窗口内只记 `s_pg_pending_space`；`la_switch_prepare()` 体内清 pending、认 `new_frame == old_frame`、补 `LA_FR_PRMD`、真调 `activate(space)` |
| L | `.S` 里 `bl      la_switch_prepare` 与 `la.abs  $sp, la_switch_stack_top` 各两次（`eentry` / `switch_inline` 两条恢复路径），且第一次换栈在第一次 `bl` 之前；`.bss` 里有 `la_switch_stack_top` 与 `.space 2048` |
| M | `arkrtos_elf.c` 有 `elf_slot_live()` / `elf_copy_to_window()`，`exec_space()` 体内有 `arkrtos_paging_space_activate(space)`、`arkrtos_paging_space_activate(prev)`、`elf_slot_live(space)`；shell 里有 `pmap u2: PASS - two instances, two spaces, neither frame overwritten` |

pin I 就是这次那个坑的护栏：少了 `invtlb`，切换在 QEMU 上静默失效，而所有
状态读数都还是"成功"。

#### 5.17.6 同一个 VA 上并发跑两个程序（U-2，2026-11-17 已做）

上一版这里写的是「装载器仍是单实例，第二次 `exec` 返回 `ERR_BUSY`」。U-2 把它换掉了：

装载器（`kernelsrc/src/arkrtos_elf.c`）：

- `s_elf_task[ARKRTOS_PAGING_APPS_MAX]` / `s_elf_info[]` / `elf_slot_live()` ——
  台账从「一个实例」变成「每个空间一个」；`exec_space()` 挑第一个空空间，
  全占着才返回 `ERR_BUSY`（语义与之前「窗口被占」一致）。
- 段不再直接 `memcpy` 到窗口指针：分页档下先读进内核暂存 `s_elf_stage[4096]`，
  再 `arkrtos_paging_space_activate(space)` + 每 4 KB 拷过去（`elf_copy_to_window()`）。
  理由：装载期间内核跑在恒等映射上，而窗口 VA 属于目标 App，
  直接写依赖「窗口此刻恰好指着它」这个隐含前提。
- 装载前后记住并恢复 `arkrtos_paging_space_current()`：失败也要装回去，
  否则 shell 之后每一条内存命令读到的都是别人的帧。

调度路径（本次真正的坑）：

```
arkrtos_sched_activate() -> arkrtos_mpu_set_app() -> 重写 App 窗口 MTLB 条目
```

而 LA64 的 trap 帧（`eentry`）与内联切换帧（`arkrtos_task_switch` 那条）都压在
**被换下任务自己的栈**上，App 任务的栈就在 App 的 2 MB 窗口 VA 里。条目一换，
这段 C 代码的返回地址、`$s0` 帧指针槽、局部量在下一句 `ld.d` / 函数返回时
就从**另一个 App 的帧**里取值。实测：

```
FATAL estat=0x00080000 ecode=0x00000008 era=0xcdcdcdcd badv=0xcdcdcdcd
      badi=0x002b0010 crmd=0x00000010
```

`0xcdcdcdcd` 是该 port 的栈填充图样起手，`ertn` 于是跳到那里，PLV3 里 ADEF 停机。

修法（延迟换窗口）：

1. `arkrtos_port_paging_switch_to(space)`（`arkrtos_hal.h` 有契约，关档宏恒 -1）：
   越界 -1；`la_sp_in_app_window()` 为真（当前 `sp` 落在
   `[APP_BASE, APP_BASE+APP_SIZE)`）就只记 `s_pg_pending_space` 并返回 0；
   否则等价于 `activate()`。判据用 `sp` 而不是 `current->is_priv`：危险的
   精确条件是「恢复现场要用的那一帧压在同一个 VA 上」。
2. `arkrtos_context.S` 的两条恢复路径（`eentry`、`arkrtos_port_switch_task_inline`）
   在 `la_trap_c` / `la_switch_inline` 返回、拿到目标帧之后，先
   `la.abs $sp, la_switch_stack_top`（恒等映射里的 2 KB 静态栈，`.bss`），
   再 `bl la_switch_prepare(new_frame, old_frame)`。换完之后不再碰任何栈，
   只按 `$s0` 恢复寄存器。
3. `la_switch_prepare()`（`arkrtos_port.c`）：无 pending 直接返回；
   `new_frame == old_frame`（调度器最后没切走）则清 pending **不切**——否则
   这一帧的恢复会从另一个 App 的帧里取值；否则 `activate(space)` 之后补写
   `new_frame[LA_FR_PRMD]`（C 侧 `LA_PRIV_APPLY` 是在窗口还没换时执行的，
   它写进去的是旧窗口里那块内存，不是目标帧）。

错误/重填路径不用改：它们进 C 之前已经把 `sp` 换到 `la_fault_stack` / `la_refill_stack`，
`la_sp_in_app_window()` 自然为假，行为退化成「立即生效」。

实测读数（`build/u2run2.log`，端口 45841，`grep -c FATAL` = 0）：

```
$ pmap u2
pmap u2: /mnt/ram/hello -> space 0 task 4, space 1 task 9
pmap u2: frame0=0x02000000 frame1=0x02200000
pmap u2: same VA 0x02000000 reads 0x00150064 / 0x00150064, other slot before/after 0xA5A50202 / 0x00150064
[hello] entry sp=35635200 argc=1
[hello] hello from an ELF64 program
[hello] argc=1
[hello] argv[0]=/mnt/ram/hello
[hello] bye
[hello] entry sp=35635200 argc=1
[hello] hello from an ELF64 program
[hello] argc=1
[hello] argv[0]=/mnt/ram/hello
[hello] bye
pmap u2: PASS - two instances, two spaces, neither frame overwritten
$ pmap elf
pmap elf: spaces=2 last=1
  space 0 frame=0x02000000 task=4 live=0 bytes=1732 entry=0x02000000
  space 1 frame=0x02200000 task=9 live=0 bytes=1732 entry=0x02000000
pmap elf: 0 live instance(s)
```

注意判据：同一 VA 两个空间读出**相同**字（`0x00150064` = hello 的第一条指令，
不是 ELF 魔数——入口是 `0x02000000`），这是对的；区分身份靠 `frame0/frame1`、
`task 4/9` 与「另一槽位装载前是 `0xA5A50202` 哨兵、装载后被覆盖」。
两次 `bye` 才是「两个实例各自跑完」的证据——只有 `PASS` 而没看到两条 `bye`
不算并发。

回归（`build/regress_u2.log`，端口 45851）：`--modld-check` 7 条 +
`--stack-guard-check` 1 条 + 单实例 `exec /mnt/ram/hello a b`（仍报
`space 0`、`finished after 0 ms`）+ `pmap elf`，`SHELL-DRIVE OK: 10 条命令全部符合预期`。

#### 5.17.7 明确没做的部分

- **页帧分配器（U-3）**：帧是板级固定数组，只按 `ARKRTOS_PAGING_APPS_MAX` 开。
- **ASID / TLB shootdown（U-7）**：这一节的镜像 ASID 恒 0、切换靠重写条目；
  U-7 的 ASID 档已于同日做完并实测（§5.28），但 shootdown 仍没有。
- **`copy_from_user`（U-6）**、按需缺页（U-5）、App 反向探测（PLV3 读内核页/别的帧）
  都不在本轮：本轮的隔离证据是「同一 VA → 两块物理内存」，越权证据仍来自
  PLV/`PLV3` 探针那一套（[内存保护与App越权验证](内存保护与App越权验证.md)）。

### 5.18 撤销动态映射：unmap 与槽位回收（U-15，2026-11-17）

#### 5.18.1 接口与拒绝码

| 层 | 接口 | 语义 |
|---|---|---|
| port | `int32 arkrtos_port_paging_unalias(uint32 va)` | 0 = 已撤销；-1 未 4 KB 对齐；-2 落在静态跨度；-4 没登记过 |
| port | `uint32 arkrtos_port_paging_dyn_used(void)` | 当前占用的动态槽位数（只数 `used` 标志，不读 TLB） |
| paging | `int32 arkrtos_paging_unmap_span(uint32 va, uint32 size)` | 逐页撤，第一个失败即返回那个码；`s_pg_map_pages` 只减已撤的页数 |
| paging | `uint32 arkrtos_paging_dyn_used(void)` | 委托 port |
| shell | `pmap unmap <va> <size>` / `pmap dyn` | 命令行入口与读数 |

三条设计取舍，都是为了「宁可报错，也不给看似权威的错答案」：

1. **没登记过就报 -4**，不静默成功。一个调用方撤自己没登记过的跨度，听到 "done" 比
   听到 "没这回事" 危险得多。
2. **静态跨度一律 -2**。内核镜像 / App 窗口 / 两根 UART 条目撤掉任意一条，下一次取指就是
   KERNEL refill —— 那是故障，不是"释放"。
3. **span 中途失败不回滚**（与 `map_span()` 同一条规则，理由也一样：没有 re-map 就没有
   回滚），但已撤的页会从 `s_pg_map_pages` 里如实扣掉。

槽位只在**一对 8 KB 的两半都空**时才回收：一条 MTLB 条目覆盖一对页，只撤一半时另半页
仍然有效，`used` 不能归零，否则下一个 `map_span()` 会把槽位发给别的 VA 对。

#### 5.18.2 本轮实测出来的一个真缺陷：只重写 MTLB 不够

原设计（以及本页 §5.16.7 的旧说法）是「动态条目落在 MTLB 的绝对索引上，`tlbwr` 覆盖同
一条索引后旧翻译就不存在了，所以撤销不需要 `invtlb`」。**前半句对，结论错。**

实测（第一版实现，只有 `tlbwr`）：

```text
pmap: map ROM 0x03000000 <- 0x00200000 rc=0
pmap: compare 16 words, mismatch=0
pmap: unmap 0x03000000 rc=0 left_entry=0 dyn=0
pmap: rebind 0x03000000 <- 0x00201000 rc=0 dyn=1
pmap: rebind compare 16 words, mismatch=16      <-- 16 个字全错
pmap test: FAIL (rebind translation)
```

根因：这个 VA 一旦被**用过**，翻译就同时存在于 STLB 和 QEMU 的 softmmu 缓存里。`tlbwr`
只改 machine TLB，不会回头看 STLB；QEMU 的 softmmu 更是对已翻译过的页做缓存。于是出现了
最坏的一种组合：

- `pmap look` 报 `no entry`（它查的是 MTLB，账面确实空了）——**看起来成功了**；
- 经同一个 VA 读出来的仍是旧物理页——**实际上没撤掉**。

修法是两步，顺序不能颠倒（`invtlb` 是按旧条目的 PS 匹配的，写在后面就查不到旧条目了）：

```c
la_tlb_inval_page(pair);            /* invtlb 3：按 VA 失效 STLB + softmmu */
la_tlb_inval_page(pair + 0x1000u);  /* 奇半页单独再来一次（QEMU 的奇页计算有偏差） */
la_tlb_write(pair, 12u, ...);       /* 再把 V=0 的条目写回 machine TLB */
```

改后同一序列：`rebind compare 16 words, mismatch=0`，`pmap test: PASS`。

这条与本页 §5.17.2（大页切换时 `tlbwr` 不刷新**奇页**）是同一类坑：**写进去 ≠ 生效，
读回来才算**。

#### 5.18.3 读数（QEMU LA64 实跑）

正向（`pmap test` 已把 map / unmap / rebind / unmap 串成一条判据）：

```text
pmap: map ROM 0x03000000 <- 0x00200000 rc=0
pmap: compare 16 words, mismatch=0
pmap: unmap 0x03000000 rc=0 left_entry=0 dyn=0
pmap: rebind 0x03000000 <- 0x00201000 rc=0 dyn=1
pmap: rebind compare 16 words, mismatch=0
pmap: final unmap rc=0 left_entry=0 dyn=0
pmap test: PASS (map / unmap / rebind / unmap)
```

其中 `rebind` 那一步是「槽位真的回收了」的判据：如果只清了账面没回收槽位，
同一个 VA 改绑另一块 PA 会撞上 `-5`。

拒绝码（逐条实跑）：

```text
pmap: unmap 0x00200000 size=4096 rc=-2 pages=0 dyn=0    # 内核镜像静态跨度
pmap: unmap 0x03000123 size=4096 rc=-1 pages=0 dyn=0    # 未 4 KB 对齐
pmap: unmap 0x03000000 size=4096 rc=-4 pages=0 dyn=0    # 没登记过
pmap: unmap 0x03000000 size=6144 rc=-1 pages=0 dyn=0    # 不是 4 KB 的整数倍
```

负向证据（撤掉之后**真的**访问一次，这才是「撤掉了」而不是「账面清零」）：

```text
pmap: map 0x03000000 <- 0x00200000 size=4096 rc=0 pages=1
pmap: [0x03000000] = 0x14016003
pmap: [0x03000004] = 0x03800063
pmap: unmap 0x03000000 size=4096 rc=0 pages=0 dyn=0
pmap look: va=0x03000000 no entry
pmap rd 0x03000000 1
[Ark RTOS] FATAL: estat=0x00010000 ecode=0x00000001 era=0x00225b20 badv=0x03000000 ...
```

`ecode=0x1` 是 TLB refill，`badv` 正是被撤的那个 VA。

回归：`utest` 13 matched / 13 passed / 0 failed（133 assertions），`pmap u1` 与
`pmap u2` 仍 PASS（同一 VA 两块帧的切换路径没被这条改动影响）。RV32 镜像体积与改动前
逐字节同尺寸：`text 235828 / data 128 / bss 628228`（paging 关档时 `unalias`/`dyn_used`
走宏回退，一行代码也不进镜像）。

#### 5.18.4 门禁：N / O 两条 pin 与自检

`tools/paging_gate.py` 新增两条源码 pin，各自带变异自检（把源串改坏一次，pin 必须报错）：

| pin | 断言 |
|---|---|
| N | port 有 `arkrtos_port_paging_unalias(`，其函数体含 `la_pg_static_hit(`、`la_tlb_inval_page(`、`la_tlb_write(`、两半空判据与 `used = 0u`；`arkrtos_port_paging_dyn_used(` 数 `used` 标志 |
| O | `arkrtos_paging_unmap_span(` 存在且调 `arkrtos_port_paging_unalias(` 并结账 `s_pg_map_pages -= done`；`arkrtos_paging_dyn_used` 委托 port；hal / mpu.h 两条关档回退；shell 有 `unmap` / `dyn` 分支且 `pmap test` 判据串了 unmap/rebind |

pin N 特意把 `la_tlb_inval_page(` 列为必需：把这一句删掉，功能仍然"看起来对"
（`look` 报 no entry），只有经该 VA 真读一次才暴露 —— 这类检查必须写进门禁，
不能只写在文档里。

#### 5.18.5 明确没做的部分

- **ASID 与批量失效**：这一档 ASID 仍恒 0（ASID 档见 §5.28）；没有
  `flush_tlb_range` / 整表 flush
  （`arkrtos_port_paging_flush()` 仍空操作）。
- **App 可见动态映射的撤销**：App 窗口走空间切换（U-1），那条 PS=20 条目由
  `activate()` 重写，不经过 `unalias`。
- **撤销后的页面回收（U-3）**：槽位回收了，物理页本身仍由板级固定数组持有，
  没有页帧分配器可还。
- **按 VA 失效会打掉窗口条目（U-6 时才发现）**：`invtlb 3` 连 PS=20 那条一起打掉，
  修法与取证见 §5.19.3。

### 5.19 受控访问用户地址：copy_from_user / copy_to_user（U-6，2026-11-17）

U-5 之前，内核路径读用户缓冲靠两条口头约定：「调用者一定是用户任务」「指针一定落在
窗口里」。任一条不成立，内核就按当前翻译去读 —— 读到的可能是另一个 App 的帧，
更常见的是未映射页让内核自己吃一个 TLB refill。U-6 把这件事做成可被拒绝的原语。

#### 5.19.1 接口与四条拒绝码

```c
int32 arkrtos_copy_from_user(void *dst, const void *usrc, uint32 len);
int32 arkrtos_copy_to_user(void *udst, const void *src, uint32 len);
int32 arkrtos_copy_from_user_task(arkrtos_task_t *p_task, void *dst, const void *usrc, uint32 len);
int32 arkrtos_copy_to_user_task(arkrtos_task_t *p_task, void *udst, const void *src, uint32 len);
int32 arkrtos_paging_user_copy(uint32 rom_start, uint32 rom_size,
                               uint32 ram_start, uint32 ram_size,
                               uint32 space, uint32 installed,
                               void *kern, const void *user, uint32 len, uint8 write);
```

分三层：`arkrtos_paging_user_copy` 是纯算术 + 字节拷贝的本体（不打任务表），
`*_task` 从 TCB 取四个跨度与绑定空间，`copy_*_user` 取当前任务。自测走的是
本体那一层，跑的是生产同一段闸门代码，不必在栈上造假的 TCB
（`arkrtos_paging_space_of()` 对不在表内的指针做相减属未定义行为）。

返回 0 表示整段拷完，否则返回负值：

| 码 | 含义 |
|---|---|
| -1 | 参数错或 `uva + len` 跨 4 GB 回绕 |
| -2 | 这个任务没有用户窗口（内核任务 / `is_priv` 非 0） |
| -3 | 整段不在允许的窗口内（写方向只认 RAM 窗口） |
| -4 | 当前装着的是别的 App 的地址空间（拷贝会走错翻译，属于静默错答案） |

判据是**整段**而不是首地址：只查首字节时，跨出窗口末尾的指针会让循环走进未映射页，
内核照样 refill —— 那正是本接口要消灭的形态。写方向只允许 RAM 窗口，代码窗口是
任务自己的镜像，内核路径没有理由改它，直接 -3 而不是「劝一劝」。

本体用 `volatile uint8` 字节循环：不引入可能跨页的字宽拷贝，性能不是当前目的。
非分页档（ARM / RV32）四条入口一律宏回退为 `(-1)` —— 那些后端没有「每任务虚拟空间」
这个概念，假装支持比返回 -1 更糟，也保证了默认档体积逐字节不变。

#### 5.19.2 实测（QEMU LA64 实跑）

`pmap cu` 在 `pmap test` 之后运行，11 条判据全过：

```
pmap cu: PASS - from/to user, refusals -1/-2/-3/-4, kernel survived
```

覆盖：`from_user` RAM 窗口（返回 0 + 字节校验）、`from_user` ROM 窗口、整段在窗口外
(-3)、内核镜像指针 0x00200000 (-3)、未映射指针 0x80000000 (-3)、区间回绕
0xFFFFF000+0x2000 (-1)、`to_user` 写 ROM 窗口 (-3)、`to_user` 写 RAM 窗口
(0 + 落字节校验)、没有用户窗口 (-2)、内核任务走 `arkrtos_copy_from_user` (-2)、
空间不匹配 (-4，需 `space_count() >= 2`)。「kernel survived」的意思是：这些非法指针
之后内核照常打印结论，没有 fault。

同一轮回归：`utest` 13 matched / 13 passed / 0 failed（133 assertions）、
`pmap u1` / `pmap u2` / `pmap test` 仍 PASS、`pmap dyn` 0 槽 0 页。RV32 镜像
`text 235828 / data 128 / bss 628228` 与 U-15 后逐字节同尺寸。

#### 5.19.3 顺手挖出的真缺陷：`invtlb 3` 会连带打掉 App 窗口那条 PS=20 条目

第一次把 `pmap cu` 排在 `pmap test` 后面跑，内核直接死：

```
pmap look: va=0x02000000 entry=2050 ps=20 ppn=0x02000000
pmap test: PASS (map / unmap / rebind / unmap)
pmap look: va=0x02000000 no entry
[LA64U] KERNEL refill (plv0) badv=0x02100000 era=0x00225c38 -> ...
[Ark RTOS] FATAL: ... ecode=0x0
```

根因：QEMU 9.2.3 的 `invtlb 3, $zero, <va>`（按 VA 失效）会把该 VA 上**所有**页尺寸的
条目一起打掉，包括 App 窗口那条 PS=20、走 MTLB+2 的条目；而 `pmap test` 末尾恰好
撤销它自己的动态映射并做了一次按 VA 失效。更贵的是 `activate()` 的短路：
`s_pg_app_space == space` 就直接返回 0 报「已装上」，条目已经没了也照报 —— 这是
典型的静默错答案。

两步修法（均落在 `kernelsrc/port/loongarch/la64/arkrtos_port.c`）：

1. 新增 `la_pg_assert_window()`：按 `s_pg_app_space` → `la_pg_app_frame()` 把窗口条目
   原样重写一遍（`la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u, ...)`），
   在 `unalias()` 的两处按 VA 失效之后各调一次。内容不变，代价一次 `tlbwr`。
2. `activate()` 的「已是该空间」短路改成先 `arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE, 0, &probed, 0)`
   核对条目还在、且 PPN 与 `la_pg_app_frame(space)` 一致，才返回 0；否则照常重写。

修后同一串命令（`pmap test` → `look` → `cu` → `dyn`）全绿，`look` 恢复
`entry=2050 ps=20 ppn=0x02000000`。这条也解释了为什么它只在「先跑过会 invtlb 的自测」
时出现：单纯 boot 起来直接用窗口是看不见的。

#### 5.19.4 门禁：P / Q 两条 pin 与自检

`tools/paging_gate.py` 新增两条源码 pin，各自带变异自检（把源串改坏一次，pin 必须报错）：

| pin | 断言 |
|---|---|
| P | `arkrtos_paging_user_copy` 体内有回绕判据、`((rom_start == 0u) && (ram_start == 0u))`、`space != installed`、两个方向的字节循环、`if((write != 0u) ||`；`*_task` 体内有 `p_task->is_priv != 0u`、`arkrtos_paging_space_of(p_task)`、`arkrtos_port_paging_space()`；`copy_from_user` 体内有 `arkrtos_task_get_current()`；shell 有 `pmap cu` 分支、五条探针串与 PASS 串；mpu.h 有两条关档回退宏 |
| Q | port 有 `static void la_pg_assert_window(void)`、`unalias` 体内调它、`activate` 体内有 `lookup(ARKRTOS_QEMU_APP_BASE` 与 `probed == la_pg_app_frame(space)` |

自检这一条有个真实的坑：第一版把变异串写成 `... < len` → `... < lenX`，pin 判据是
子串包含，`lenX` 仍包含 `len`，变异没落地、自检却报「pin 是空转的」。改成
`(0xFFFFFFFFu - uva)` → `(0xFFFFFFFFu - uvaMut)` 才真正破坏判据。**变异串必须
破坏被断言的子串本身，而不是它的前缀或后缀。**

#### 5.19.5 明确没做的部分

- **SVC 处理器没有改造**：`arkrtos_kernel_check.h` 里的布尔校验仍是原样，
  `copy_*_user` 只交付原语 + 自测，没把整层改成走受控拷贝。
- **没有 fixup 表**：拷贝中途失败就返回负值，不做「已经拷了几个字节」的部分成功语义。
- **没有 copy-on-write / 用户页换出**：窗口内全部是板级固定帧，恒常驻。
- **性能未优化**：逐字节 `volatile` 拷贝，不是 `memcpy` 的那种吞吐。


### 5.20 任务退出码通道（U-8，2026-11-17）

#### 5.20.1 欠的是什么

U-6 之后 shell 已经能受控地把用户地址搬进内核，但 `exec` 仍然只回报
「跑完了、用了 N ms、在哪个空间」。脚本要判一个程序的成败，只能去 grep 它
自己打印的字符串——那是程序**说的话**，不是它的**结论**。Linux 的答案是
`exit_group(status)` 加 `wait()`。本档先做内核侧前半截（退出码与台账），
shell 侧的 `wait` 于 2026-11-17 补上（§5.20.7）——它是**轮询任务表**，不是 POSIX 语义：
没有父子关系、没有僵尸队列，也不能「等任意一个子进程」。内核侧这部分是：

- `THREAD_CTRL`（SVC `0x1B`）新增子命令 **4 = `ARKRTOS_THREAD_SUB_EXIT_CODE`**，
  `p[1]` 是退出码（`int32`，负数原样）。行为与子 3（`EXIT`）相同——都结束调用任务，
  差别只在「有没有一个可读的码」。
- 退出码先落进 **TCB**，再由**装载器**在「任务变 `INVALID` 的那一次观察」把它取走
  存进实例台账；shell 的 `exec` 在打印结束行时读台账。

#### 5.20.2 开关与体积

| 项 | 内容 |
|---|---|
| 开关 | `ARKRTOS_USE_EXIT_CODE`（`config/kconfig/Kconfig.features`，`kernelsrc/include/arkrtos_features.h` 里缺省 **0**） |
| 开档 | 只有 `board/qemu_loongarch64/arkrtos_board_config.h` 打开（`#undef` + `#define ... 1`） |
| 关档代价 | 子命令宏、TCB 两个字段、`*_internal` 实现、SVC 分支、shell 的带码打印、ELF 台账两个字段全部被 `#if` 摘掉 ⇒ **RV32 镜像 `text 235828 / data 128 / bss 628228` 与 U-6 之后逐字节相同** |
| 开档代价 | 每个 TCB 多 8 字节（4 字节码 + 1 字节 flag + 3 字节对齐填充），镜像约 +240 B（首版没门控时 RV32 实测 `text 236068`，即 +240） |

`arkrtos_thread_exit_code()` **两档都声明、都实现**：关档退化成
`(void)code; arkrtos_thread_exit();`，所以 App 侧调用点不需要 `#if`
（一个宏开关不该逼用户改源）。

#### 5.20.3 为什么不在退出路径里回调

任务自杀发生在**调度器的切换路径里**（`arkrtos_thread_exit_internal()` 之后
不再回来）。在那里回调装载器意味着：切换路径要知道 ELF 台账、要持有它的锁、
要保证被换下的栈还能用——U-2 已经踩过一次「切换路径里碰窗口就吃到别人的帧」。
所以退出口只写两个 TCB 字段，**不通知任何人**；装载器侧改成懒清理：

```
elf_slot_live(space)               # 装载/查询/exec 等待循环都会走到这里
  └ id = s_elf_task[space]
    └ status == INVALID ?          # 就是"任务死了但槽还没被复用"那一刻
        arkrtos_task_exit_code(id, &code)   # 有码才写台账
        s_elf_task[space] = 0
```

`exit_code_set` 存在的理由只有一个：`exit code 0` 是一个**有效结论**，
不能和「没上报过」混为一谈。没上报时 shell 仍打原来的无码行
（`finished after N ms (space N)`），**不编一个看起来像退出码的 0**。

#### 5.20.4 实测读数（QEMU LA64 实跑）

`exec` 等程序结束的循环是 20 ms 一轮、每轮都调 `arkrtos_elf_running_in(space)`
⇒ 触发 `elf_slot_live`，所以码通常在任务死后的第一次轮询就被取走。
`hello.c` 故意把 **argc 当退出码**，让「程序自己打印的 argc」与「exec 打印的
exit code」互为对证：

```
$ exec /mnt/ram/hello one two
[hello] entry sp=35635200 argc=3
[hello] argc=3
[hello] bye, exit code = argc
[ELF] /mnt/ram/hello: finished after 0 ms (space 0, exit code 3)

$ exec /mnt/ram/hello
[hello] argc=1
[ELF] /mnt/ram/hello: finished after 20 ms (space 0, exit code 1)
```

同一次运行的其余判据：`utest: 13 matched, 13 passed`、`pmap u1` / `u2` /
`test` / `cu` 仍 PASS、`dyn slot(s) in use=0`、`SHELL-DRIVE OK: 3 条命令全部符合预期`。

> 记一条**测量环境**的坑（不是内核缺陷）：这轮 `qemu_shell_drive.py` 连跑三次
> 都是「连上了 TCP 但 90 s 一个字节都没收到」。同一份镜像改用 `-serial file:`
> 立刻能看到完整 banner + `mark@arkrtos:~$` 提示符 ⇒ 内核正常，是 host 侧
> TCP chardev 的偶发竞态。**判据：`buf` 全空 ≠ 内核死了**，先用 file 串口
> 单独确认一次再决定是重试还是查代码。

#### 5.20.5 门禁

`tools/elf_gate.py` 新增 **F pin**（读 8 个文件、逐环断言）：`svc_abi.h` 有子 4 宏 →
TCB 有两个字段且在 `is_priv` 之前 → `arkrtos_task.c` 有实现、有 `case`、有
`arkrtos_thread_exit_code_internal((int32)p[1])` → `arkrtos_elf.h` 台账两字段 →
`arkrtos_elf.c` 有 `arkrtos_task_exit_code(id, &code)` → shell 有
`exit code %d` → 用户 API 有声明 → `hello.c` 用 `ARKRTOS_THREAD_SUB_EXIT_CODE`
且 `params[1] = (ark_u32)argc;`。F pin 自带变异自检（`"exit code %d"` →
`"exit code X%d"`），自检不过就直接报「pin 空转」。

> 同一条教训第二次出现：第一版变异串写成 `"exit code %d"` → `"exit code %dX"`，
> 判据是子串包含，`%dX` 仍包含 `%d`，**变异没落地**。变异串必须破坏被断言
> 子串本身（同 §5.19.4）。

**F2 pin（`wait` 与 `main` 返回值转码）**：同一个门禁里再加一处，读 shell 与 app_main
两个文件、共 8 条断言——`arkrtos_shell.c` 里 `cmd_wait` 定义在
`#if (ARKRTOS_USE_EXIT_CODE == 1)` 内、注册表有
`ARK_SHELL_CMD("wait", cmd_wait` 且 `min_args` 为 3、拒绝自己
（`arkrtos_current_task_id`）、有 `no such task`、按 `ARKRTOS_TASK_INVALID` 判结束、
调 `arkrtos_task_exit_code`、两条打印串 `exited (exit code %d)` 与
`exited (no exit code reported)`；`arkrtos_app_main.c` 的 `arkrtos_app_exit`
开档走 `arkrtos_thread_exit_code(code)`、关档 `#else` 仍调 `arkrtos_thread_exit()`。
F2 同样带两条变异自检。

**F2b pin（另外两条退出路径）**：小程序 SDK（`kernelsrc/sdk/mini_sdk/arkrtos_mini_main.c`）
的 `arkrtos_mini_exit(code)` 与可下载模块（`kernelsrc/src/arkrtos_modld.c` 的
`mod_task_tramp`）——前者要求 `exit(code)` 在开关闸门里调
`arkrtos_thread_exit_code(code)`，后者要求入口返回值先被**接住**
（`rc = (int32)p_fn(...)`）再交出去（`arkrtos_thread_exit_code_internal(rc)`）。
只查「有没有 `#if`」抓不到「码到手却丢掉」，所以断言落在接住与交出两个动作上，
并各配一条变异自检。

同时把 F pin 的打印串断言从 `"exit code %d"` **收窄**成
`"finished after %u ms (space %u, exit code %d)"`：`cmd_wait` 里也有
`"exit code %d"`，不收窄的话「把 exec 那处改坏」的变异不再使 F pin 失败，
自检会静默空转（本轮实际踩到过一次）。

#### 5.20.6 明确没做的部分

- **`wait` 不是 POSIX 的 `wait()`**：没有父子关系、没有僵尸队列，码也不是由内核
  主动投递。`wait <task id> [timeout ms]` 只是 shell 侧 20 ms 轮询任务表，等
  `status` 变成 `ARKRTOS_TASK_INVALID` 后**只读**地取一次 TCB 里的码。所以它不等于
  「回收子进程」，也做不了「等任意一个子进程」（没有这个概念）；槽被复用后读到的
  码属于新任务。
- **driver_sdk 没接线**（这是取舍，不是缺口）：驱动任务不是「跑完就退」的程序，
  没有 `exec` / `wait` 那样的观察者，码写进 TCB 也没有人读。`app_sdk` 与
  `mini_sdk` 都接了（C 库 `exit()` 收到的码转成 `arkrtos_thread_exit_code(code)`），
  可下载模块由装载器在 `module_main` 返回后转发（`arkrtos_thread_exit_code_internal(rc)`）。
- **码只在 TCB 槽被复用前有效**：若任务死后立刻有大量建/删任务把该槽复用，
  码就丢了（实测 exec 的 20 ms 轮询使这窗口实际不可达，但机制上没有保护）。
- **板级只开了 LA64**：`board/qemu_loongarch64/arkrtos_board_config.h` 里
  `ARKRTOS_USE_EXIT_CODE` 被 `#undef` 后重定义为 1，命令行关不掉；RV32 板配置没开，
  因此 32 位侧只有「关档零变化」这一条读数，**RV32 开档没有任何 QEMU 读数**。
- **ARM 侧只编译、没上板**：`example/stm32f427/app_sdk/HELLO_APP/GCC` 关档与
  `DEFS="-DARKRTOS_USE_EXIT_CODE=1"` 开档都能编过并链接成 `build/hello_app.elf`；
  小程序侧 `HELLO_MINI/GCC` 两档也都编过（关档 484/8/28，开档 480/8/28——开档
  更小是因为少了「忽略码再纯退出」那层包装），都没有实机读数。

#### 5.20.7 读数：`wait` 与 main 返回值转码（2026-11-17）

从本轮起 `tools/qemu_shell_drive.py --script` 支持**竖线期望**（`命令 | 期望子串 |
期望子串2`），没有竖线就等于只发不判。这一点要诚实记下来：**此前用 `--script`
得到的 `SHELL-DRIVE OK` 期望集是空的，只证明命令发进去了、回显里有那几个字**。

LA64 默认档（`build/qemu_la64_zero`：`wait` 那一笔 text 367744，再把小程序与模块的
退出码转发接上后 **367824** / data 416 / bss 767508），五条判据一次跑过：

| 命令 | 期望子串 | 实测回显 |
| --- | --- | --- |
| `exec /mnt/ram/hello one two` | `exit code 3` | `finished after 0 ms (space 0, exit code 3)`，hello 落在 task 4 |
| `wait 4` | `exited (exit code 3)` | `task 4 exited (exit code 3)` |
| `wait 5 100` | `still running after 100 ms` | `task 5 still running after 100 ms (status WAIT)` |
| `wait 47` | `no such task 47` | `wait: no such task 47` |
| `wait 0` | `usage` | 用法串 |

模块路径（`ld /mnt/ram/modhello.o alpha 42`）：示例模块的 `module_main` 现在
`return argc;`，装载器的退出路径把它转发成任务退出码，于是
`wait 4` → `task 4 exited (exit code 3)`——**非零**读数，0 与「没上报」在读数上
分得开（没上报那条是 `exited (no exit code reported)`，本轮在模块接线之前实测过）。

小程序（`mini_sdk`）这条路只到编译：它的宿主是 ARM 工程，本机没有板级读数；
所以在表里它是「已接线、未上板」，不要读成「小程序退出码已验证」。

**关档零变化**由 RV32 逐字节证明：当前树（LA64 侧的 `wait` 与 app_main 改动都在，
但 RV32 板 `ARKRTOS_USE_EXIT_CODE` 为 0）构建出 text 235828 / data 128 / bss 628228，
`objcopy -O binary` 的 sha256 是
`2251eb0996a0485778da8e24e82a54596e4de35024d807b3fd0a4cd2a1b807bd`；
用 `git archive HEAD` 单独摊出的树（`/d/tmp_ark_head2`）同口径构建得到**同一个 sha256**。
LA64 之所以不能用来证这条，是因为该板把开关写死成 1（见 §5.20.6）。

### 5.21 4 KB 页帧池（U-3 第一阶，2026-11-17）

#### 5.21.1 欠的是什么

U-1 让每个 App 有自己的地址空间，但**帧从哪来**一直是写死的：
`space n -> APP_BASE + n * APP_SIZE`（2 MB 步进）。于是三件事做不到：

- 空间号与帧号绑死，删掉一个 App 也不会把帧还回来（空间不能回收）；
- 2 MB 的粒度对「按需缺页」没有意义——断层只可能落在 2 MB 边界，
  一条 `PS=20` 条目要么整体有效、要么整体无效；
- 内核没有任何「借一页内存」的接口，将来驱动、共享内存、页缓存都得
  自己另起一套。

这一阶只补第一层：**4 KB 页帧池 + 分配/归还/计数**。它刻意**不动**
App 窗口的映射（那条仍是每空间一条 `PS=20`），也**不动**任何既有行为：
打开开关后 App 窗口还是原来那条条目，`pmap u1/u2` 的读数逐字不变。

#### 5.21.2 开关与几何

| 项 | 内容 |
|---|---|
| 开关 | `ARKRTOS_USE_PAGING_FRAMES`（`arkrtos_features.h` 缺省 **0**；`config/kconfig/Kconfig.features` 里 `depends on ARKRTOS_USE_PAGING`；板级 `#undef/#define 1`） |
| 关档 | port 侧只有四个返回 `-1`/`0` 的桩，**不占一个字节的 .bss**（RV32 关档体积 `text 235828 / data 128 / bss 628228` 逐字节不变） |
| 帧池 | `ARKRTOS_QEMU_FRAME_BASE = 0x04000000`（64 MB）、`ARKRTOS_QEMU_FRAME_COUNT = 2048`（8 MB） |
| 位图 | `static uint32 s_fr_bits[(COUNT + 31)/32]` = 64 个字 = **256 B**，加一个 `s_fr_used` 计数 |
| 映射 | `la_mmu_init()` 给帧池补一条 `PS=22` 恒等映射，落在 `MTLB[2059]`（`LA_MTLB_INDEX + 11`），避开动态映射那 8 个槽位（2051..2058） |
| 编译期守卫 | 基址必须 8 MB 对齐、`1 <= COUNT <= 2048`，任一条不满足就 `#error` |

为什么基址要 8 MB 对齐：`PS=22` 条目覆盖 2^23 字节，VA/PA 都必须按
2^(22+1) 对齐。为什么必须给帧池一条恒等映射：内核在分页模式下按**物理
地址**访问帧，没有这一条映射它就只能在 `DA=1` 的窗口期用——那窗口期在
启动早期就结束了，之后分配出去的帧是一块内核也碰不到的黑洞，自测写图案
就会当场重填致命。

为什么是 `0x04000000` 而**不是** `0x03000000`：后者是 `pmap test` 动态
映射的靶子 VA（`PMAP_TEST_VA`）。同一个 VA 上同时存在一条 `PS=22` 静态
条目和一条 4 KB 动态条目，命中的是谁取决于条目顺序——那是随机的错答案。
第一版就是把帧池放在 `0x03000000`，`pmap test` 仍报 PASS（它恰好命中
动态条目），但两套机制共用同一个 VA 这件事本身就是缺陷；换基址是这个
缺陷的修法，不是「测试碰巧过了」。

#### 5.21.3 接口与返回码

port 层四个接口（`arkrtos_hal.h` 声明，`arkrtos_paging.c` 转发成
`arkrtos_paging_frame_*`，关档时是 `(-1)`/`(0u)` 宏）：

| 接口 | 返回 |
|---|---|
| `arkrtos_paging_frame_alloc(count, &base)` | `0` 成功（`base` = 首帧物理地址）；`-1` 参数非法（`count == 0` / 超总数 / `p_base` 空）；`-2` 没有这么长的**连续**空闲段 |
| `arkrtos_paging_frame_free(base, count)` | `0` 已归还；`-1` 池外 / 未 4 KB 对齐 / `count` 越界；`-3` 这段里**本来就有没分配过的帧**（此时一帧都不动） |
| `arkrtos_paging_frame_total()` | 池内帧总数（关档 0） |
| `arkrtos_paging_frame_used()` | 已分配帧数（关档 0） |

三条刻意的「不猜」：

1. 分配必须给出一段**连续**的帧（将来一次装 2 MB 的窗口要一整块），
   没有就报 `-2`，不做「只给一部分」；
2. first-fit，从低帧号往高扫，不做**碎片整理**——整理会移动别人正在
   用的帧，那不是分配器该悄悄做的事；
3. 归还走「先全部验证再改」：只要有一帧本来空闲，整段拒绝（`-3`）。
   否则同一个帧会被两个持有者同时以为归自己，那个 bug 只在很久以后
   显形。

#### 5.21.4 QEMU 实测（2026-11-17）

```
$ pmap frame
pmap: frames total=2048 used=0 free=2048

$ pmap ftest
pmap: frame alloc 1 rc=0 base=0x04000000
pmap: frame alloc 3 rc=0 base=0x04001000
pmap: frame free 1 rc=0
pmap: frame double-free rc=-3 (want -3)
pmap: frame free outside rc=-1 (want -1)
pmap: frame free 3 rc=0
pmap ftest: PASS (alloc/free/double-free/pattern, used back to 0)
```

`pmap ftest` 的判据按顺序是：1 帧 + 3 帧两段连续分配都成功；四帧各写
一段 64 字的图案（`0xA5A50000 ^ (frame<<8) ^ k`）**再逐字读回**；重复
释放返回 `-3`；池外释放返回 `-1`；两段都归还后 `used` 回到初值。
写图案再读回这一条是关键——它同时验证了「分配器记账对」与「帧池那条
`PS=22` 恒等映射真的生效」；少了它，一个只记对账但帧在内核里不可达的
实现也会 PASS。

同一次启动里的回归（逐字不变）：

```
$ pmap test      -> pmap test: PASS (map / unmap / rebind / unmap)
$ pmap u1        -> pmap u1: PASS - same VA, two frames   (entry=2050 ps=20)
$ pmap u2        -> pmap u2: PASS - two instances, two spaces, neither frame overwritten
$ utest          -> utest: 13 matched, 13 passed, 0 failed, 0 skipped
$ exec /mnt/ram/hello one two
                 -> [ELF] /mnt/ram/hello: finished after 0 ms (space 0, exit code 3)
```

#### 5.21.5 门禁

`tools/paging_gate.py` 新增 **pin R**（源码 pin，带两条防假绿自检）：

- port 里有 `s_fr_bits` 位图与 `run == count` 连续段判据；
- `frame_free` 的 `-1` / `-3` 两条拒绝码都在；
- `la_mmu_init()` 里有 `la_tlb_fill(ARKRTOS_QEMU_FRAME_BASE, 22u, LA_MTLB_INDEX + 11u, ...)`；
- `arkrtos_hal.h` / `arkrtos_mpu.h` 的关档回退是 `(-1)`，`arkrtos_mpu.h` 有声明；
- `arkrtos_paging.c` 的包装真的转发到 port；
- shell 有 `pmap frame` / `pmap ftest` 与 PASS 串。

自检两条：把 `run == count` 改成 `run =X= count`（pin 必须报「不做连续段
搜索」）；把 PASS 串改成 `PASSX`（pin 必须报「PASS 串不在」）。

**自检本身踩到的坑**：第一版把 `run == count` 改成 `run == countX`，
pin **仍然通过**——因为 `"run == count"` 还是 `"run == countX"` 的前缀。
变异串必须破坏被判定的子串本身，不能只是加后缀。

`ARKRTOS_USE_PAGING_FRAMES` 的门禁挂在 `paging_gate.py` 的 pin R 上，
**不新增 ci_gate 项**（与 U-8 同一条约定）。

#### 5.21.6 明确没做的部分

- **App 窗口仍是每空间一条 `PS=20`**：这一阶只是「帧从哪来」，不是
  「窗口怎么按页装」。把窗口切成 4 KB 页需要每页一条 `PS=12` 条目
  （STLB 2048 条足够装 2 MB = 512 页，每 set 2 页），`activate` /
  `la_pg_assert_window` / `pmap u1/u2` 的判据全都要跟着改——单独一阶。
- **没有按需缺页**：`la_refill_c()` 里那条「按需换入」分支还没写。
- **分配出去的帧没有主人**：这一阶不接管 App 窗口，所以没有任何代码
  会调 `frame_alloc`。真正的使用方是下一阶的窗口装载 + 退出释放。
- **没有回收调用方的检查**：谁分配谁负责归还，池子不记账「哪个空间
  用了哪些帧」。
- **帧池只在 LA64 板级打开**：RV32 板配置没开，因此 32 位侧只有
  「关档零变化」这一条读数。

### 5.22 页级 App 窗口（U-3 第二阶，2026-11-17）

#### 5.22.1 欠的是什么

§5.21 给了帧池，但池子没有使用方：App 窗口仍是每空间**一条** `PS=20`
（2 MB）条目，空间号即帧号（`space n -> APP_BASE + n * 2 MB`）。
于是两件事做不到：

- 帧从池里出；空间与物理地址仍绑死，`pmap frame` 的 `used` 永远是 0；
- 页粒度。2 MB 条目只有「整块有效 / 整块无效」两种状态，按需缺页
  （U-5）在它上面没有落点。

这一阶把窗口改成 **512 条页映射**：每个地址空间从帧池整块取 512 帧
（2 MB），把窗口铺成 256 条 `PS=12` 条目。**仍然 eager、仍然不还帧**
——它是「窗口怎么装」的改变，不是「什么时候装」「装完怎么回收」。

#### 5.22.2 几何：一条 PS=12 条目管相邻两页

LoongArch 的页表项是成对的：`TLBELO0` 管 `vaddr`，`TLBELO1` 管
`vaddr + 4 KB`。所以

```
2 MB 窗口 = 512 页 = 256 条 PS=12 条目
组号 set = (VA >> 13) & 0xFF，窗口基址 0x02000000 的组号 = 0
条目号 = way * 256 + set   ⇒ 256 条正好铺满 STLB 的一个路（256 组）
```

窗口独占 **way 0**。这带来一个必须一起解决的问题：两条 4 KB 的 UART
页（内核 UART 与 App UART，都在组 0）以前走 `la_tlb_fill()`（`tlbfill`
的路号是**随机**的），撞一次 way 0 就顶掉窗口的第一条映射，症状是 App
第一次取指就 refill 到一个「映射明明在」的地址。因此打开这一档时两条
UART 页改走固定路：内核 UART **way 7**、App UART **way 6**。

关档（`ARKRTOS_USE_PAGING_PAGES = 0`）时这两个 `#define` 不参与编译，
`la_tlb_fill()` 原样保留。

#### 5.22.3 切换流程与三条「不猜」

`arkrtos_port_paging_activate(space)`（页级档）：

1. `la_pg_space_ensure(space)`：该空间第一次被装时从池里整块要
   `LA_PG_WIN_PAGES`（512）帧；要不到就 `return -3`，**窗口一条都不动**
   ——半张窗口（一半有效一半 refill）是最难查的失败形态；
2. 同一空间且首条目 PPN == 该空间帧基址、`ps == 12`：直接返回 0
   （不能只看 space 号，别的路径的一次 `invtlb` 就可能把条目打掉，见 §5.18）；
3. `la_pg_drop_window()`：逐 4 KB 对整段窗口 VA 发 `invtlb 3`。QEMU 的
   softmmu 按 VA 记翻译，`tlbwr` 只清自己那 8 KB 且**奇页地址算错**
   （`addr & pagesize`），所以「换窗口」必须按源码页显式失效 512 次；
4. `la_pg_install_space(space)`：256 次 `la_tlb_write(va, 12u, ...)`；
5. 抽查**首页与末页**：任一条 PPN / ps 对不上就 `return -2`，且
   `s_pg_app_space` 不被改（报错留在原地，比「切换了」这句谎话好）。

第 5 步踩过一个坑：`lookup` 读回的是条目 **TLBELO0（偶页）** 的 PPN，
所以末页 `0x02000000 + 2MB - 4KB` 的期望值是这一对 8 KB 的**偶页基址**
`base + 2MB - 8KB`，写成末页本身会得到一次假失败（首轮 QEMU 实测
`pmap u1: rc1=-2`，修正后 `rc0=0 rc1=0`）。代码里留了注释。

帧的来源与归属：`static uint32 s_pg_space_base[ARKRTOS_PAGING_APPS_MAX]`，
`0` = 还没分配；~~拿过不还~~（当时如实登记为欠账；**已于 §5.24 补上回收**，不是「偷偷还了又可能被别人拿走」的那种实现——回收严格「先撤窗口条目再还帧」）。

#### 5.22.4 窗口 VA 挪出 shell

页级档下「空间号 → VA」这条等式没了（VA 恒为窗口基址，帧基址只在物理
侧），所以新增 port 接口 `arkrtos_port_paging_window_va()`（→
`arkrtos_paging_window_va()`，关档回退 `(0u)`）。shell 里 `pmap u1` /
`pmap u2` / `pmap cu` 三处原本硬写 `0x02000000`，现在全部改为读这个接口。
窗口 VA 是 port 属性，不是 shell 该知道的事。

#### 5.22.5 QEMU 实测（2026-11-17）

启动后 `pmap frame` 的 `used` 从 0 变成 **512** —— 帧池的第一个真实使用方
出现了。原文（`build/s2_run2.log`，`--boot 25 --timeout 4`，7 条命令）：

```
$ pmap frame
pmap: frames total=2048 used=512 free=1536

$ pmap look 0x02000000
pmap look: va=0x02000000 entry=0 ps=12 ppn=0x04000000

$ pmap u1
pmap u1: va=0x02100000 space0=0xA5A50001 space1=0xA5A50002
pmap u1: frame0=0x04000000 frame1=0x04200000
pmap u1: rc0=0 rc1=0
pmap u1: look0 entry=128 ps=12 ppn=0x04100000
pmap u1: look1 entry=128 ps=12 ppn=0x04300000
pmap u1: PASS - same VA, two frames

$ pmap u2      -> PASS - two instances, two spaces, neither frame overwritten
$ pmap cu      -> PASS - from/to user, refusals -1/-2/-3/-4, kernel survived
$ pmap test    -> PASS (map / unmap / rebind / unmap)
$ utest run    -> 13 matched, 13 passed, 0 failed; assertions 133 run / 0 failed
```

三条读数的意思要分开读：

- `pmap look 0x02000000` 报 `ps=12`（不是 20）——窗口真的换了映射方式；
- `used=512` 且 `pmap frame` 的 `free=1536`——每个空间整块 2 MB，
  space 0 在 `la_pg_boot_map()` 里就拿了；
- `pmap u1` 的 `look0 entry=128`：组 0 way 0 的第 0 条 = `0*256+0`（首条目），
  `look1 entry=128` 同一原因是两个空间的**同一个 VA** 都在首条 —— 编号相同
  而 PPN 不同（`0x04100000` vs `0x04300000`），这正是「同一 VA 两块物理内存」。

`pmap u1` 的 VA 是 `0x02100000`（窗口 + 1 MB），落在第几对上由 port 的
页级计算，跟「末页是 TLBELO1」那个坑无关；这里两条读的都是偶页。

首轮日志（`build/s2_run1.log`）另有 `pmap ftest` 在页级档下的读数：
`used` 基线是 512（不是 0），`pmap: frame alloc 1 rc=0 base=0x04200000`，
判尾仍是 `PASS (alloc/free/double-free/pattern, used back to 512)` ——
帧池自己的契约不因窗口用了它而变。

RV32 关档：`text 235828 / data 128 / bss 628228`，**逐字节不变**。

#### 5.22.6 门禁

`tools/paging_gate.py` 新增 **pin S**（带两条防假绿自检），断言链覆盖八处：

- 开关三处：`arkrtos_features.h` 缺省 0 / `Kconfig.features` 里
  `config ARKRTOS_USE_PAGING_PAGES` 且两条 `depends on`（PAGING 与
  FRAMES）/ 板级 `#define ARKRTOS_USE_PAGING_PAGES     1`；
- port：`LA_PG_WIN_ENTRIES` 几何、组合守卫
  `#error "ARKRTOS_USE_PAGING_PAGES=1 requires ARKRTOS_USE_PAGING_FRAMES=1"`、
  `la_pg_space_ensure` 里 `frame_alloc(LA_PG_WIN_PAGES, &base)` 与 `return -3;`、
  `la_pg_install_space` 里 `la_tlb_write(va, 12u, LA_STLB_INDEX(va, LA_PG_WIN_WAY),`、
  `la_pg_drop_window` 里 `la_tlb_inval_page(`、`activate` 里 `return -2;` 与
  末页期望 `(s_pg_space_base[space] + ARKRTOS_QEMU_APP_SIZE - 0x2000u)` 与
  `(probed_ps != 12u)`、`la_pg_boot_map();` 真被调用、两条 UART 固定 way；
- **PS=20 老路径仍在**：`la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u`
  在源码里出现 ≥ 2 次（`la_mmu_init` 与 `la_pg_assert_window` 各一条，
  默认档没有它就没有窗口映射）；
- 转发链：hal 的声明与 `(0u)` 回退、mpu.h 的声明与回退、`paging.c` 的
  `window_va` 真转发到 port、shell 里 ≥ 3 处改用 `arkrtos_paging_window_va()`。

自检两条：把 `#define LA_PG_WIN_ENTRIES` 改成 `LA_PG_WIN_ENTRXIES`；
把 `frame_alloc(LA_PG_WIN_PAGES, &base) != 0` 里的 `LA_PG_WIN_PAGES` 改成
`LA_PG_WIN_PAGEZ`（破坏被判定的子串本身，不是加后缀）。

`ARKRTOS_USE_PAGING_PAGES` 的门禁同样挂在 `paging_gate.py` 上，**不新增
ci_gate 项**（与 U-8 / pin R 同一条约定）。

#### 5.22.7 明确没做的部分

- **没有按需缺页（U-5）**：装载仍然是先把 512 帧全给出去再整段铺满；
  `tlb_refill_entry` 还没有「BADV 在窗口内 ⇒ 换入一页」的分支，而且那个
  入口现在**不保存通用寄存器**，要加分支得先给它补齐保存/恢复——单独一阶。
- ~~**没有空间回收**~~：这一阶时帧拿过不还，空间跑满后再要空间就是 `-3`。
  **已于 §5.24 补上**（`arkrtos_paging_space_release`，装载器在 activate 之前
  回收该空间的旧窗口）；「页替换」**已由 §5.26 补上**（开 `ARKRTOS_USE_PAGING_SWAP` 的档；缺省仍落回原致命路径）。
- **默认档没有 ASID**：切换空间仍靠整段 `invtlb`（512 次）。U-7 的 ASID 档
  已做并实测（§5.28），但跨核 TLB shootdown 仍无从谈起。
- **窗口独占 way 0**：内核 UART / App UART 已经改走 way 7 / 6 让开，但
  其余经 `la_tlb_fill()` 进 STLB 的条目（如果有）仍可能与窗口抢路。
- **只在 LA64 板级打开**：RV32 侧只有「关档零变化」这一条读数。

### 5.23 按需缺页（U-3 第三阶 / U-5，2026-11-17）

#### 5.23.1 欠的是什么

§5.22 之后窗口有了页粒度，但仍然是 **eager**：`activate()` 一被调用就把 512 帧
全要下来、256 条条目一次铺满。于是「App 用了多少内存」与「App 有多大」仍然
是两个无关的数 —— 2 MB 窗口哪怕只摸了 4 KB，也照付 512 帧。

缺页处理这一层缺的不只是代码，还有三处**前提**：

- `tlb_refill_entry` 从来**不保存通用寄存器**。它调的 `la_refill_c()` 只有两个
  出口：换到另一个任务的帧、或者停机。按需缺页不一样 —— 装好条目之后要
  `ertn` 回到**出错的那条指令**重试，被打断的寄存器必须原样还回去；
- 重填上下文（`TLBRERA.ISTLBR = 1`）里能用的 CSR 与平时不同（见 5.23.2）；
- 装配方式：窗口的 way 在 §5.22 是「独占 way 0、写死 way 号」，而按需缺页要靠
  硬件 `tlbfill` 自己选组选路（见 5.23.4）。

这一阶仍然**不还帧**：它改的是「什么时候装」，不是「装完怎么回收」。
回收在下一节 §5.24 单独补齐（页级档整块还、按需档逐对还）。

#### 5.23.2 三个静默错答案（都在 QEMU 上实测踩过）

这一阶的难点不在逻辑，而在三条**会跑起来但答案错**的形态。三条都只在本轮
一次实跑里暴露过一次，值得逐条写死：

1. **重填上下文里不能用 `TLBEHI` + `tlbwr` 装条目。**
   `helper_tlbwr` / `helper_tlbfill` 在 `ISTLBR = 1` 时只认
   `CSR_TLBREHI / TLBRELO0 / TLBRELO1` 那一组镜像 CSR，忽略 `TLBEHI / TLBELO0/1`。
   实测形态：在重填处理里写 `TLBEHI` 再 `tlbwr`，条目按硬件填的
   `TLBREHI`（= 出错地址的 VPPN）落进去、`TLBELO` 取 `TLBRELO0/1` 的**旧值**
   ⇒ 条目 **V = 0**。于是下一次访问命中一条 V=0 的条目，QEMU 报
   `TLBRET_INVALID`（**不是** `TLBRET_NOMATCH`，不置 `ISTLBR`）⇒ 走 `eentry`
   ⇒ `la_trap_c` 判 PIS 致命异常、停机：`FATAL ecode=0x2 (PIS) era=elf_memcpy`。
   正确形态就是硬件重填处理惯用的那一套：写 `TLBREHI / TLBRELO0 / TLBRELO1`
   再 `tlbfill`（`TLBREHI.PS` 在 bit[5:0]，VPPN 从 bit13 起 ⇒ 页基址直接或
   `12` 就是完整值）。
2. **`CSR.SAVE0`（0x30）在 QEMU 9.2.3 上不可用。** 规范里它是可选实现，
   QEMU 写进去读回 **0**。第一版拿它暂存 `sp`，症状是 `la_refill_c` 里
   `sp = 0`，第二次致命：`KERNEL refill (plv0) badv=0x15591570
   era=arkrtos_sched_activate`。改静态槽之后才往下走。
3. **不能先用要保存的寄存器当帧基址。** 第二版拿 `$t0` 做帧指针：`la.abs $t0,
   <帧>` 这一步就把**被打断的 `$t0` 原值毁了**，之后才 `st.d $t0, ...` 存进去的
   是帧地址。表现极隐蔽 —— 启动、装载、第一个 App 都正常，直到 `elf_memcpy`
   （GCC 拿 `$t0`/`$t1` 当源/目的指针的循环）第一次缺页后才崩：源指针与目的指针
   整段偏掉、循环尾判据不再等于 `n`，**2 MB 窗口被逐 8 KB 往外写到窗口外**，
   停在 `badv=0x02200000 era=elf_memcpy`。定位靠临时在装载器里插桩 + 反汇编
   （`21f4e0`–`21f504` 显示循环用 `$t0` 当源指针）。定案是改用 **`$r21`**：
   LoongArch psABI 把它定为**保留寄存器**（内核与按 psABI 编译的 App 都不用，
   `example/*/start.S` 里的注释也这么写），所以可以先拿它当指针、后面不必还原。
   帧 256 字节、16 字节对齐，只放 GPR（`ERA` / `PRMD` 仍在 CSR 里）。

#### 5.23.3 实现：开关、粒度、账本、流程

开关三处：`ARKRTOS_USE_PAGING_DEMAND`（`arkrtos_features.h` 缺省 0 /
`Kconfig.features` 的 `config ARKRTOS_USE_PAGING_DEMAND`，`depends on` PAGING 与
PAGES 两条 / 板级同样缺省 0）。port 里配两条组合守卫：
`=1 requires ARKRTOS_USE_PAGING_PAGES=1` 与 `=1 requires ARKRTOS_USE_PRIV=1`。

粒度是**一对 8 KB**，不是一页：一条 `PS=12` 条目同时给出两页（`TLBELO0` 管
`vaddr`、`TLBELO1` 管 `vaddr + 4 KB`），「装半条」表达不出来 ⇒ 要么两帧一起拿、
要么一条都不装。

账本是 `s_pg_pair_pa[space][256]`（0 = 这一对还没被摸过）。按需档里
`s_pg_space_base` 恒 0 —— 空间不再有「整块 2 MB 的基址」，物理页是按对零散拿的。
这也是 shell 读数的口径变化：`pmap u1` / `pmap u2` 的 `frame0` / `frame1`
在这一档显示 `0x00000000`（`look` 报 `no entry ps=0`），它们的 PASS 判据靠
「同一 VA 读到两份不同的内容、且对方槽位没被覆盖」，不靠帧号。

流程：

- `activate(space)`：`invtlb` 清掉旧空间的翻译 + 只改 `s_pg_app_space`，
  **一条条目都不铺**，也不做「已是该空间」的短路（这一档的真相在
  `s_pg_pair_pa`，短路省不下什么却多一条会骗人的路）；
- 首次访问某一对 ⇒ `TLBRENTRY` ⇒ `la_refill_demand_c()`：读 `TLBRBADV`，
  在窗口内就 `frame_alloc(2u, &pa)` 拿两帧、记账、`tlbfill` 装一条，
  `ertn` 回原指令重试；不在窗口内 / 帧池空了就 `return 0`，汇编侧落回
  `la_refill_c()`（原来的报错 / 杀任务路径，不返回）；
- `la_pg_assert_window()`（撤销动态映射之后的重铺）：只补**已经付过费**的那几对。

这一档**刻意不区分 PLV**：内核经窗口拷贝（ELF 装载、`pmap` 自测）与 App 自己
访问走同一条路，否则装载器第一笔写就死在「KERNEL refill → fatal」上。

#### 5.23.4 `tlbfill` 的 way 是随机的 ⇒ 两条 MMIO 页必须搬进 MTLB

§5.22 靠「窗口独占 way 0、UART 写 way 7 / 6」把冲突躲开。按需档不成立：
`tlbfill` 在 `pagesize == CSR.STLBPS`（12）时走
`set = get_random_tlb(0, 7)` —— **way 由硬件随机挑**（`index = way*256 + set`，
`set = (VA>>13) & 0xFF`）。窗口基址 `0x02000000`，2 MB 正好一个组（0..255）
一条，所以窗口自己的 256 条不会自撞；但**组 0 与内核 UART `0x1fe00000` 同组**、
组 1 与 App UART 别名 `0x02202000` 同组 ⇒ 窗口第 0 对（或第 1 对）随机落到那一
路时就把控制台页顶掉。

实测形态（两次同一形态）：`pmap u2` / `pmap test` 跑到一半
`badv=0x1fe001e5` 致命。修法是把两条 MMIO 页搬进 **MTLB**（`tlbfill` 只碰
STLB 0..2047，永不与 MTLB 撞），槽位分工写死在 `la_mmu_init()` 的注释里：

```
+0/+1     内核两条 PS=22 大页
+2        App 整块（非页级档，PS=20）
+3..+10   动态 alias（LA_MTLB_DYN_FIRST，8 对）
+11       页帧池（PS=22）
+12/+13   内核 UART / App UART 别名（按需档专用）
```

第一版把它们放 `+3/+4`，正好和 `arkrtos_port_paging_alias()` 抢槽 ——
`pmap test` 一登记动态映射就把控制台页顶掉（同一形态第二次踩到），于是挪到
`+12/+13`。

#### 5.23.5 构建与实跑读数

按需档单独出一个 `BUILD` 目录，基线目录不动：

```bash
make -f example/qemu_loongarch64/Makefile -j8                      # 基线
make -f example/qemu_loongarch64/Makefile BUILD=build/qemu_la64_demand \
     EXTRA_CFLAGS=-DARKRTOS_USE_PAGING_DEMAND=1 -j8                # 按需档
```

`EXTRA_CFLAGS` 同时进 `CFLAGS` 与 `ASFLAGS` —— `.S` 也要按开关决定
「refill 入口是否保存通用寄存器」，所以汇编侧必须看得见板级宏
（`ASFLAGS` 里加了 `-include arkrtos_board_config.h`；这条本身不改默认档
任何一个字节，见 5.23.6）。

体积：`size` 报 text 366120 → **366152**（+32 B）、data 416 / bss 767508 不变
（基线 `build/sde_final`，按需 `build/qemu_la64_demand`）。

实跑（`--boot 25 --timeout 6`，`build/s3_run1.log`）：

```
$ pmap frame
pmap: frames total=2048 used=14 free=2034

$ exec /mnt/ram/hello one two
[ELF] /mnt/ram/hello: 1748 B, 2 segment(s), entry 0x02000000, sp 0x021FC000, argc 3, task 4, space 0
[hello] argv[0]=/mnt/ram/hello / [1]=one / [2]=two
[ELF] /mnt/ram/hello: finished after 0 ms (space 0, exit code 3)

$ pmap look 0x02000000
pmap look: va=0x02000000 no entry

$ pmap u1
pmap u1: va=0x02100000 space0=0xA5A50001 space1=0xA5A50002
pmap u1: frame0=0x00000000 frame1=0x00000000
pmap u1: rc0=0 rc1=0
pmap u1: look0 entry=0 ps=0 ppn=0x00000000
pmap u1: PASS - same VA, two frames

$ pmap u2
pmap u2: same VA 0x02000000 reads 0x00150064 / 0x00150064, other slot before/after 0xA5A50202 / 0x00150064
pmap u2: PASS - two instances, two spaces, neither frame overwritten

$ pmap cu      -> PASS - from/to user, refusals -1/-2/-3/-4, kernel survived
$ pmap test    -> PASS (map / unmap / rebind / unmap)
$ utest run    -> 13 matched, 13 passed, 0 failed; assertions 133 run / 0 failed
```

三组读数要分开读：

- `used=14` 而不是 §5.22 的 512 —— 窗口按摸到的对付款。一个 `hello`（两个
  PT_LOAD 段 + 栈 + argv 区）摸到的对很少，加上自测的临时空间就是 14；
- **`pmap look 0x02000000` 报 `no entry`**：这是按需档的正确形态，不是缺陷 ——
  `activate` 之后窗口是空的，没被摸过的对没有条目。`pmap u1` 那两条
  `look0/look1 entry=0 ps=0` 同理（它读的是切换后的空间视图）；
- `pmap u1` / `pmap u2` 的 `frame0/frame1 = 0x0` 是账本口径（`s_pg_space_base`
  在这一档不存在），不是「没有帧」——同一个 VA 在两空间读到两份不同的内容、
  且退出后对方槽位没被覆盖，才是这条判据的实质。

关档回归（基线目录）全部照旧：`used=512`、`pmap look 0x02000000 entry=0 ps=12
ppn=0x04000000`、`pmap u1/u2/cu/test` 全 PASS、`utest 13/13`。

#### 5.23.6 证明「关档零变化」的正确做法

这一条踩过一次方法论错误，值得记下来。第一次比对是「带改动的基线镜像
sha256 ≠ `git stash` 回 HEAD 重建的镜像 sha256」，于是怀疑是
`ASFLAGS` 新加的 `-include` 让 `.S` 看到了板级宏。**那个结论是错的** ——
两个镜像本来就不同，因为它们各含一个 `__DATE__` / `__TIME__` 生成的构建时间串
（`arkrtos_shell.o` 里的 `build : Nov 17 2026 17:12:43` 一行）。

正确做法两步：

```bash
# 1) 固定构建时间戳（GCC 认 SOURCE_DATE_EPOCH）
SOURCE_DATE_EPOCH=1600000000 make -f example/qemu_loongarch64/Makefile BUILD=<dir> -j8
# 2) 剔掉调试信息再比（DWARF 行表会随源文件改动自然变化，不是行为/体积变化）
loongarch64-newlib-elf-objcopy --strip-debug <dir>/ark_qemu_la64.elf <dir>/stripped.elf
sha256sum <head>/stripped.elf <now>/stripped.elf
```

实测结果：HEAD 与带改动的工作区各出一份，`stripped.elf` **逐字节相同**
（`027dcf53...`，460264 B）。顺带把 `-include` 的嫌疑也证伪了：把
`EXTRA_CFLAGS` 显式给成 `-DARKRTOS_USE_PAGING_DEMAND=0`（等价于「无条件让 `.S`
看到板级宏」）再出一份，`stripped.elf` 仍然是同一串哈希。

剩下的差异只有两类，都不是行为或体积：`__DATE__/__TIME__` 的时间串（不固定
时间戳时必然不同，HEAD 自己重建两次也不同）与 DWARF（源文件改了，行表跟着改）。

#### 5.23.7 门禁

`tools/paging_gate.py` 新增 **pin T**（两条防假绿自检），并且这一条**真的编译**：

- 源码断言：开关三处、port 的两条 `#error`、`s_pg_pair_pa`、
  `la_pg_demand_fault` 里 `frame_alloc(2u, &pa) != 0` 与 `la_tlb_fill_refill(va,`、
  `la_tlb_fill_refill` 里 `LA_CSRW(LA_CSR_TLBREHI` 与 `"tlbfill"`、
  `la_refill_demand_c` 里 `LA_CSRR(LA_CSR_TLBRBADV`、MMIO 页的
  `LA_MTLB_INDEX + 12u` / `+ 13u`、**老路径仍在**（`LA_STLB_INDEX(... KUART_WAY)`）、
  汇编侧 `la.abs  $r21, la_refill_stack_top`、`st.d    $t0, $r21, 88`、
  `bl      la_refill_demand_c`、`ld.d    $sp, $sp, 16`、`ertn`，以及
  **`la.abs  $t0, la_refill_stack_top` 必须不存在**（把 5.23.2 的第 3 条钉死）、
  老档的 `la.abs  $sp, la_refill_stack_top` + `bl la_refill_c` 仍在；
- 两条自检：把 `s_pg_pair_pa` 改坏、把 `$r21` 换回 `$t0`，pin T 都必须报错；
- **真编译**：`arkrtos_port.c` 带 `-DARKRTOS_USE_PAGING_DEMAND=1` 过一遍
  `-fsyntax-only`；`arkrtos_context.S` 带同一组宏真汇编成 `.o`
  （`-fsyntax-only` 不管汇编，这道必须用 `-c`，否则「汇编里写错了」这一整类
  假绿都盖不到）。

与 pin R / pin S 同约定：**不新增 `ci_gate.py` 项**，门禁挂 `paging_gate.py`。

#### 5.23.8 明确没做的部分

- ~~**帧不还**~~：这一阶按需档只是「按对付款」，退出/切空间都不回收。
  **已由 §5.24 补上**（按需档逐对 `frame_free(pa, 2u)` 并销账）；页替换
  **已由 §5.26 补上**（开关 `ARKRTOS_USE_PAGING_SWAP`，缺省 0）。
- **默认档没有 ASID**：切空间仍是整段 `invtlb`；ASID 档见 §5.28（U-7）。
- **refill 不区分 PLV**：内核与 App 走同一条路（5.23.3 里说明了为什么必须这样）。
- ~~**没有页替换 / 淘汰**~~：缺省档仍是「帧池空了就 `return 0` → 原致命路径」；
  开 `ARKRTOS_USE_PAGING_SWAP=1` 后改为换出一对 8 KB 再重试（§5.26）。
- **只在 LA64 板级实测**：RV32 侧只有「关档零变化」这一条读数。
- **`pmap u1/u2` 的帧号显示 0x0**：这一档的账本在 `s_pg_pair_pa`，shell 没有
  按需档专用的读数（要看清每一对，得加新命令，登记为可选）。
  **已由 §5.33 补上**（`pmap pairs [space]`，逐对只读打印 VA/PA）。

### 5.24 空间回收（U-3 遗留，2026-11-17）

#### 5.24.0 为什么是这一格

U-3 三阶做完后，`docs/欠账盘点.md` 里剩下的最实一格是「帧拿过不还」：每次
`exec` 挑一个空空间、装窗口要 512 帧（页级档）或按对付款（按需档），退出后
一个都不还。`FRAME_COUNT = 2048`，于是**第四次 exec 就会把池吃干**，第五次
落回原来的致命路径。这条不需要页替换（换出要挑牺牲页 + 写回，是另一条机制），
只需要「把这一格还回去」，所以先做它。

#### 5.24.1 接口与返回码

一个接口，两层声明，一层转发：

- `arkrtos_hal.h` / `arkrtos_mpu.h`：`int32 ...paging_space_release(uint32 space);`
  关档恒等宏为 `(-1)`（与同族 `paging_*` 一致，关档调用点不会链接进任何东西）；
- `arkrtos_paging.c`：越界先报 -1，其余原样转发到 port；
- port：`arkrtos_port_paging_space_release()`。

返回码写死（`0` 成功，其余一律负数，且**区分「没有」与「没还」**）：

| 返回 | 含义 |
| --- | --- |
| `0` | 已归还。**幂等**：本来就没拿过（基址 0 / 账本全 0）也返回 0 |
| `-1` | 空间号越界，或这一档根本没有 TLB 通路（`ARKRTOS_USE_PRIV == 0`） |
| `-2` | 这一档没有帧池可还（`PAGES != 1`，即 PS=20 静态窗口） |
| `-3` | 调用者的 `sp` 正站在这个空间的窗口 VA 里 —— **一个字节都不动** |
| `-4` | 帧池拒绝（port 那边 `frame_free` 的 -1/-3 原样透出） |

`-2` 与 `0` 必须分开：把「这一档本来就没从池里拿过」读成「已经还了」，会让
上层在静态窗口档上误以为回收生效。

#### 5.24.2 两档实现

```c
if (space >= APPS_MAX)                    return -1;
if (space == s_pg_app_space && la_sp_in_app_window() != 0u) return -3;
if (space == s_pg_app_space)              la_pg_drop_window();   /* 先撤条目 */
```

- **页级档**（`PAGING_PAGES=1, DEMAND=0`）：整块还。
  `arkrtos_port_paging_frame_free(s_pg_space_base[space], LA_PG_WIN_PAGES)`，
  成功才把 `s_pg_space_base[space] = 0u`。池子那边是 all-or-nothing（一次
  验证全部帧再改位图），所以拒绝时账本一个字不改 —— 下次 `activate` 仍指向
  同一块帧，比「以为还了其实没还」安全。
- **按需档**（`DEMAND=1`）：truth 在 `s_pg_pair_pa[space][256]`，逐对
  `frame_free(pa, 2u)`（一条 PS=12 条目管两页），成功才把该格清 0。第一处
  拒绝记 `-4` 但**继续结账**（其余对还能还的就还掉），最后如实返回 -4。

两档都**不动 `s_pg_app_space`**：空间仍然存在，只是空着（基址 0 / 账本全 0），
下一次 `activate()` 的 `ensure()` 看到基址 0 会重新要帧。把「空间空了」压进
「空间不存在」会让 `activate` 的短路判据骗人。

#### 5.24.3 为什么必须「先撤条目，再还帧」

反序是这一条最容易写错的地方：帧先回池、旧条目还指着它，中间只要有别人拿走
这块帧，就会变成**两个空间共享同一块物理页**——而且没有任何报错，只在很久
以后以「A 写了 B 也变了」的形式显形。所以顺序是死的：

1. `la_pg_drop_window()`：页级档逐 4 KB `invtlb 3`（QEMU 的 softmmu 缓存按
   VA 记翻译）；
2. 再 `frame_free` 把帧交回池子。

装载器 `arkrtos_elf_exec_space()` 里同理，`(void)arkrtos_paging_space_release(space);`
必须落在 `arkrtos_paging_space_current()` / `activate(space)` **之前**：先回收
要用的那个空间（它上一次留下的窗口），再激活它铺新的。反序会把刚铺好的窗口
当场撤掉。这一条**由门禁按源码位置钉死**（`find` 位置比较），不靠注释。

#### 5.24.4 QEMU 实跑读数（11 条命令全符合预期）

镜像 `build/qemu_la64_rel/`（LA64 页级档，`PAGES=1, DEMAND=0`），
`tools/qemu_shell_drive.py` 驱动，日志 `build/pmap_rel.log`：

1. 开机 `pmap frame` → `used=512 free=1536`（space 0 启动时铺了一块）；
2. `pmap rel 1` → `space 1 rc=0 frames 512 -> 512 (frame=0x00000000 current=0)`
   —— 从未分配过的空间，**幂等 no-op**，帧数一分不动；
3. `pmap rel 0` → `space 0 rc=0 frames 512 -> 0`；随后 `pmap frame` →
   `used=0 free=2048`（整池干净）；
4. `pmap space` → `spaces=2 current=0`，两个空间都 `frame 0x00000000`
   —— **空间号还在，只是空着**，与 5.24.2 的口径一致；
5. `pmap space 1` → `rc=0 (current=1)` 重新要帧 → `pmap frame` → `used=512`；
   再 `pmap rel 1`（`current=1`、调用者在核心栈上）→ `rc=0 frames 512 -> 0`，
   `current` 仍 1，`pmap frame` → `used=0`；
6. `exec /mnt/ram/hello a b` → `finished after 0 ms (space 0, exit code 3)`，
   随后 `pmap frame` → `used=1024 free=1024` = 程序自己那块 512 + 装载器装回
   `prev`（space 1）又 512。**这就是回收生效的形态**：以前反复 exec 是单向
   累加，现在同一空间复用时先还被再重拿，`used` 不随 exec 次数单调上涨。

#### 5.24.5 没实跑的分支（必须如实写）

`-3`（调用者 `sp` 就在窗口 VA 里）**只有源码判据，没有实跑**。现有调用点
只有两处：`pmap rel`（在 shell 上下文，内核栈）与装载器（同样在内核栈），
都不在窗口里。要造出这个调用者需要一个「App 在自己窗口里调内核回收自己」
的用例，当前没有 —— 登记为可选，不写成已验证。

`-2`（静态 PS=20 窗口档）也没有专门跑一次 shell 命令，它是编译期分支
（`PAGES != 1`），由门禁的源码断言覆盖。

#### 5.24.6 门禁：`paging_gate.py` 的 pin U + 两条自检

沿用 R/S/T 的约定（**不新增 `ci_gate.py` 项**），新增 `pin_u()`：

- port 侧：函数存在、越界 -1、`space == s_pg_app_space` 与
  `la_sp_in_app_window() != 0u` 加 `return -3;`、先 `la_pg_drop_window();`、
  页级档 `frame_free(s_pg_space_base[space],` 与 `LA_PG_WIN_PAGES`、按需档
  `frame_free(pa, 2u)` 与销账 `s_pg_pair_pa[space][i] = 0u;`、静态档 `return -2;`；
- hal / mpu：声明与关档宏；
- `arkrtos_paging.c`：转发；
- `arkrtos_elf.c`：**release 必须在 activate 之前**（比较 `find` 位置）；
- shell：`pmap rel` 用法与读数行。

两条防假绿自检：把 `frame_free` 调用改坏、把 elf 里那句 release 删掉，
pin U 都必须报错。`PAGING-GATE OK`，末段含
`U-3 空间回收 U pin 成立且自检有效`。

#### 5.24.7 关档零变化（RV32，固定时间戳比 sha256）

RV32 档 `PAGING=0`，新增代码全在 `#if` 内。证据不是「体积没变」而是逐字节：

```bash
SOURCE_DATE_EPOCH=1600000000 make -f example/qemu_riscv32/Makefile BUILD=<dir> -j8
riscv64-unknown-elf-objcopy --strip-debug <dir>/ark_qemu_rv32.elf <dir>/stripped.elf
sha256sum <head>/stripped.elf <now>/stripped.elf
```

`git worktree add ../ark_rtos_head 4d12e10` 出基线，两侧 `stripped.elf`
**逐字节相同**（`7cf766ae...`）；`size` 同为 `text 235828 / data 128 /
bss 628228`。

#### 5.24.8 仍然没做的

- ~~**页替换**~~：**已由 §5.26 补上**（缺省关；开档后池空改为换出一对 8 KB）；
- **ASID / TLB shootdown**（U-7）：这一档切空间仍是整段 `invtlb`，回收也没有按
  ASID 定向；ASID 档见 §5.28（仍无 shootdown）；
- ~~**按需档的细粒度读数**~~：`pmap rel` 只报整池 `used` 前后值，没有「这个空间
  现在挂着哪几对」的命令；**已由 §5.33 补上**（`pmap pairs [space]`）；
- **App 侧自回收**：`-3` 分支就是为它留的门，但当前没有这个调用者。

### 5.33 按需档细粒度读数（`pmap pairs`，2026-11-21）

§5.23 的按需档把真账放在 `s_pg_pair_pa[space][256]`：空间激活后窗口是空的，
第一次摸到某一对 8 KB 才在 refill 里拿两帧。但 shell 只能通过 `pmap rel` 看
整池 `used` 的前后差值，回答不了「这个空间现在挂着哪几对、各在哪个物理页」。
这一格不需要新硬件、不涉及 TLB 语义，属于 U-3 的收尾，本轮补上。

#### 5.33.1 三条只读接口（port / 门面 / 头文件）

| 接口 | 语义 |
| --- | --- |
| `arkrtos_port_paging_pair_total()` | 这一档共有多少对。`PAGING_PAGES == 1` 时恒 `LA_PG_WIN_ENTRIES`（256），否则 0 |
| `arkrtos_port_paging_pair_count(space)` | 已挂对数。越界返回 0；按需档扫 `s_pg_pair_pa[space][i] != 0u`；页级档为 `(s_pg_space_base[space] != 0u) ? 256 : 0` |
| `arkrtos_port_paging_pair_pa(space, idx)` | 第 `idx` 对的物理基址。越界返回 0；按需档直读 `s_pg_pair_pa[space][idx]`；页级档 `s_pg_space_base[space] + idx * 0x2000`（基址 0 时回 0） |

三条都是**纯只读**：绝不申请帧、绝不触发缺页（按需档下读一个还没被摸过的
空间只会得到 0 对，而不是把帧拉进来）。门面 `arkrtos_paging.c` 只做转发，
故意不加任何算术（越界判据留在 port，两档语义差也只在那一层）。

一个必须记住的落点：这三条**放在 `#if (ARKRTOS_USE_PAGING_SWAP == 1)` 块之外**。
最初它们被写进 SWAP 条件块里，默认档（`PAGING=1, SWAP=0`）链接直接报
`undefined reference to arkrtos_port_paging_pair_count/pair_pa` —— 细粒度读数
是架构读数，不该被页替换档绑死。

#### 5.33.2 shell：`pmap pairs [space]`

不给空间号时用 `arkrtos_paging_space_current()`。三种输出刻意分开：

```
pmap pairs: space 1 mapped=1/256 pair(s) of 8 KB (8 KB) va=0x02000000
  pair 128: va=0x02100000 pa=0x04010000
```

- 逐对最多打 8 条，余下报 `... N more pair(s) not shown`；
- `total == 0`（这一档没有页级窗口）报
  `pmap pairs: this build has no paged window (total=0 spaces=N)`；
- 空间号越界报 `pmap pairs: space N out of range (spaces=M)`。

「问错空间号」与「这一档没这个读数」是两件不同的事，混成一句会被读成后者，
所以分成两条文案（宁可报错也不给看似权威的错答案）。

#### 5.33.3 QEMU LA64 实跑读数（两档都跑）

按需档（`EXTRA_CFLAGS=-DARKRTOS_USE_PAGING_DEMAND=1`，`qemu_shell_drive.py
--script`，`--boot 40 --timeout 6`）：

1. `pmap pairs 1`（开工前）→ `space 1 mapped=0/256 pair(s) of 8 KB (0 KB) va=0x02000000`；
2. `pmap u1` 之后 `pmap pairs 1` → `mapped=1/256 ... pair 128: va=0x02100000 pa=0x04010000`
   —— 只摸了一对就只报一对，PA 是 refill 里实际拿到的帧；
3. `pmap pairs 0` → `mapped=8/256 pair(s) of 8 KB (64 KB)`，逐对
   `pair 0 pa=0x04000000`、`pair 128 pa=0x0400E000`、`pair 250..255`
   各指向不同的 8 KB 帧（启动期已按需摸过 8 对）；
4. `pmap pairs 9` → `space 9 out of range (spaces=2)`；
5. `pmap frame` → `frames total=2048 used=18 free=2030`；
6. 同一次驱动 `SHELL-DRIVE OK: 7 条命令全部符合预期`。

默认档（页级窗口，`PAGES=1, DEMAND=0`）：

1. `pmap pairs 0` → `space 0 mapped=256/256 pair(s) of 8 KB (2048 KB)`，
   `pair 0 pa=0x04000000`、`pair 1 pa=0x04002000` …… 连续铺满（与 §5.22 的
   512 帧整块、256 条 PS=12 一致）；
2. `pmap pairs 1` → `mapped=0/256`（这个空间没装过）；
3. `pmap pairs`（无参）= 当前空间 0 ⇒ 256/256；
4. `pmap pairs 7` → `space 7 out of range (spaces=2)`；`pmap pairs x` →
   `usage: pmap pairs [space]`；
5. `pmap frame` → `frames total=2048 used=512 free=1536`。

#### 5.33.4 门禁：`paging_gate.py` 的 pin X

沿用 R/S/T/U/V 的约定（不新增 `ci_gate.py` 项），新增 `pin_x()`：钉住 port
三条落在真实账本（`PAGES` 判 256 / 扫 `s_pg_pair_pa` / 页级档按 `0x2000` 步进）、
hal 与 mpu 的声明与关档回退宏（`(0u)`）、`arkrtos_paging.c` 的三条转发、
shell 的 `pmap pairs` 三条文案（`mapped=%u/%u pair(s)`、`has no paged window`、
`out of range`）。

两条防假绿自检：把 `pair_count` 的签名改坏、把 shell 的
`arkrtos_paging_pair_pa(space, i)` 改成 `(space, 0u)`，pin X 都必须报错。
`PAGING-GATE OK` 末段含
`按需档细粒度读数 X pin 成立且自检有效（pair_total/count/pa 与 pmap pairs）`。

**本轮同时修掉的门禁假绿**：`paging_gate.py` 自提交 09e8036 起把 pin 汇总循环
吞进了 `pin_v` 的 `return bad` 之后（AST 可查：`pin_v` 里 `return` 之后还有
100+ 行可达代码），13 条源码 pin（E–W）与其自检从未执行，门禁却报「成立」。
把 `arkrtos_hal.h` 的 `paging_alias` 关档回退从 `(-1)` 改成 `(0)`（正是自检 G
的变异体）门禁仍 `exit=0`，即为复现证据；修好后同一处破坏立刻 `exit=1`。
同时修掉 8 类 pin 漂移（`_src()` 不归一化 CRLF、`_body()` 命中前向声明、
64 位回绕判据、`activate` 多 `#if` 分支、per-space way、`unsigned long` VA、
shell 拆文件后 ASID 块判定、三条空转自检）。`ci_gate.py` 新增结构性判据
`gate-live`（AST 扫 `tools/*.py`，报「`return`/`raise`/`break`/`continue`
之后同块仍有语句」），当前全仓 0 处死代码。

### 5.25 RV32 上的 ELF32 装载（U-4，2026-11-17）

RV32 是没有 MMU 的那一档，所以这里不是「换一套页表」，而是**PMP 窗口 + U 模式**：
板级划一块 1 MB 可执行 RAM 窗口（`0x80A00000`，2 的幂且按自身对齐），装载器把
`PT_LOAD` 段写进去，把任务 TC 的 `rom_*/ram_*` 都指到它，`is_priv=0`，再
`arkrtos_mpu_build_task()` 重建区域集。

#### 5.25.1 装载器：32/64 位同一份 TU

字段宽度与偏移按 `__SIZEOF_LONG__` 选表（ELF32：EHDR 52 / PHDR 32 /
`e_phoff`@28 / `e_phentsize`@42 / `p_filesz`@16 …），机器号 32 位默认 RISC-V(243)。
`tools/elf_gate.py` 的 **C 项判据在 U-4 翻转**：RV32 强开 `-DARKRTOS_USE_ELF=1`
必须**编得过**（此前要求必须编不过，因为那时只支持 64 位 `long`）。G pin 把 11 个
ELF32 偏移逐个钉住，并把 Makefile 的 `ELF_WIN` 与板级 `ARKRTOS_ELF_WINDOW_BASE`
对账。

#### 5.25.2 MPU 侧：ELF 窗口必须走独立分支

`arkrtos_mpu_rom_window()` 原来只有两个分支：MiniApp（校验槽位池
`SLOT_RAM_BASE..+TOTAL`）与内核 Flash。ELF 窗口在池外 ⇒ 走 MiniApp 分支校验必然
失败 ⇒ 区域退化成 `size=0` ⇒ **U 模式一取指就 fault**（`mcause=1`
`mtval=0x02000000`，而且看起来像"窗口宏给错了"）。新增分支按窗口几何自校验，
形状不对仍失败关闭（size=0），不向上取整。

#### 5.25.3 三个只在实跑里现形的缺陷

1. `-Ttext` 会让第一个 LOAD 段回退一页（`0x809FF000` 而不是 `0x80A00000`），
   装载器报「段在窗口外」。改用 `-Wl,-Ttext-segment=$(ELF_WIN)`。
2. `hello32` 发的退出码子命令（子 4）在 RV32 默认档**没编进去**
   （`ARKRTOS_USE_EXIT_CODE=0`）⇒ 返回 -1，程序掉进 `for(;;)`，ELF 任务优先级
   不低于 shell ⇒ **shell 永不返回**（现象：`exec` 之后 shell 一条输出都没有）。
   修法是示例程序退回子 3 `ARKRTOS_THREAD_SUB_EXIT` 兜底。
3. 装配期竞态：`arkrtos_task_register()` 内部已把任务挂上就绪集，而降权与 MPU
   写回在它之后 ⇒ 中途被切走时任务半配置起跑，表现为**间歇失败**（5 连跑 2 败，
   签名 `mcause=1 mtval=0x02000000 from=M`，帧内 `a0..a3` 全 0）。修法是把
   注册 → 降权 → MPU 写回 包进 `ARKRTOS_ENTER_CRITICAL()/EXIT_CRITICAL()`。

#### 5.25.4 读数（5 连跑全一致）

```
exec /mnt/ram/hello32 a b c
[hello32] entry sp=-2135965696 argc=4          # 0x80AFC000
[hello32] hello from an ELF32 program
[hello32] argc=4
[hello32] argv[0]=/mnt/ram/hello32  argv[1]=a  argv[2]=b  argv[3]=c
[hello32] bye, exit code = argc
TASK:4717 task 8 exits (self kill)
[ELF] /mnt/ram/hello32: 1081 B, 2 segment(s), entry 0x80A00074, sp 0x80AFC000, argc 4, task 8, space 0
[ELF] /mnt/ram/hello32: finished after 0 ms (space 0)
```

同一日志的**预期负对照**仍在：`mcause=5 mtval=0x02000000 from=U task=3`（读
CLINT）、`mcause=2 mtval=0x30046073`（写 mstatus）——U 模式越权照旧被拒。

#### 5.25.5 默认档零变化

三处会让默认档变样的地方都已挡住：板级 `ARKRTOS_MPU_ELF_WINDOW_*` 只在
`ARKRTOS_USE_ELF==1` 时定义（否则 `mpu.c` 的窗口分支编进默认镜像，实测 +564 B）；
`arkrtos_mpu_code_in_ram()` 在无窗口宏时保留原三元表达式写法（等价改写也掉 16 B）；
Makefile 的 `hello32` blob 是条件依赖。判据：与同一 `SOURCE_DATE_EPOCH` 下用
`git archive HEAD` 出的树各自构建、`--strip-debug` 后 **sha256 逐字节相同**
（`7cf766ae…`，text/data/bss = 235828/128/628228）。

#### 5.25.6 仍然没做的

- 所有 RV32 实例共用同一块窗口 ⇒ **没有地址空间隔离**，不并发；这条不是 U-4 的
  目标，但别把「跑起来了」读成「RV32 也有进程模型」；
- ELF 任务出陷阱时 `[FRAME]` 不打（打印范围仍限内核 RAM，见 §9.6）；
- LA32 侧仍未实跑（无执行载体）。

### 5.26 页替换（U-16，2026-11-17）

#### 5.26.0 为什么是这一格

§5.24 补完空间回收后，按需档还剩一个洞：**帧池空了就是死**。`la_pg_demand_fault()`
拿不到两帧时 `return 0`，回到原来的致命路径。U-16 把这条改掉：池空时**换出一对
已有的 8 KB**，再重试分配。

先说清这一档**不是**什么：交换区是内核 `.bss` 里的一块数组，**不是文件、不是磁盘、
不省内存**（实测 bss 反而 +73 728 B）。它验的是**账本与搬运的正确性**——槽号不会
串、换出的内容回来逐字节相同、帧的进出对得上。真省内存要等「交换区落在持久介质上」，
那是另一格欠账。

#### 5.26.1 三条不猜的规矩

1. **帧没拿到就返回 0，绝不装半成品条目**。缺页重填是原子语义：要么这条 PS=12
   条目装上、原指令重试成功，要么什么都没发生回到致命路径。装一半会让 App 在
   「一半是零、一半是旧内容」的窗口里继续跑。
2. **换出先 `invtlb`，再 `frame_free`**。反序会让 TLB 里还留着指向刚被别人拿走的
   帧的条目——那一刻不报错，等下一对映射覆盖过来时已经是别人的数据。
3. **换入先抄中继缓冲 → 放自己的槽 → 再要帧**。这条最反直觉，见 5.26.2。

#### 5.26.2 账本：槽号用 `k+1` 编码 + 8 KB 中继缓冲

槽账本是 `s_pg_pair_slot[ARKRTOS_PAGING_APPS_MAX][256]`（`uint8`），**0 表示没有槽、
k 表示槽 k−1**。静态对象只能零初始化，用 0 当「无」就不需要运行期初始化，也避免
「忘了初始化 ⇒ 一堆对同时指向 0 号槽」这种只在压力下才显形的 bug。

换入为什么必须过中继缓冲：如果「先要帧再拷」，要帧这一步可能触发换出，而换出需要
一个空槽——此刻唯一的空槽正是自己占着的那个（已经清掉、内容在手上）。先要帧就死锁。
正确顺序是：读槽 → 抄进 8 KB 的 `s_pg_bounce` → 清槽（槽空出来）→ 要帧（此时换出
有槽可用）→ 抄回。失败时把 bounce 的内容**放回一个槽**再返回 0，不吃数据。

#### 5.26.3 换出的两轮挑选：先换别的空间

`la_pg_swap_out_one(skip_space, skip_idx)` 先扫**别的空间**的条目，扫不到才回到本
空间、并避开 `skip_idx`（正在用的那一对）。理由是别的空间的条目此刻都没装着，换掉
不产生额外缺页；本空间的对可能正被当前取指路径用着。轮转指针 `s_pg_swap_hand`
保证确定性，同样的输入出同样的结果，便于复跑比对。

`la_pg_take_pair()` 在 `frame_alloc(2u)` 失败后最多做 8 轮「换出 + 再分配」，
8 轮还拿不到就认输返回 0（不无限自旋）。

#### 5.26.4 shell 读数与自测

两个命令（`dyn` 分支之后，`frame` 之前）：

```
pmap swap      -> pmap: swap out=%u in=%u fail=%u slots=%u/%u resident=%u frames %u/%u
pmap swaptest  -> 自测：占满帧池、逐对写图案、再逐字比对
```

`pmap swaptest` 的判据只有三条：**mismatch=0**、**out 与 in 都增长了**、收尾账本回到
`used0 - 2*res0` 且槽数归 0。不拿计数当唯一证据——计数增长但内容错了照样是失败。

自测为什么要先吃光空闲帧：窗口只有 256 对（512 帧），池子上千帧，光在窗口里摸
永远摸不满，那种「全绿」什么都没验到。所以先按 2 帧一次占空闲帧直到只剩 8 帧，
再摸 `resident + 4 + slots` 对（上限 256），必然触发换出与换回。

#### 5.26.5 QEMU 实跑读数（LA64，swap 档）

```
pmap frame     -> frames total=2048 used=0 free=2048
pmap swap      -> swap out=0 in=0 fail=0 slots=0/8 resident=0 frames 0/2048
pmap swaptest  -> space=0 held=1020 pair(s) free=8 resident=0 slots=8 touch=12 pair(s)
                  swap out 0 -> 20, in 0 -> 12, mismatch=0
                  after release frames=0 expect=0 slots=0
                  pmap swaptest: PASS (swap out/in round trip, content byte-identical, accounting clean)
pmap swap      -> swap out=20 in=12 fail=0 slots=0/8 resident=0 frames 0/2048
```

12 次摸页换推出 20 次、换回 12 次，是 5.26.3 的两轮挑选与 skip 规则决定的（本空间
的对被避开 ⇒ 一次「换出 + 换回」在连续摸页里会重复发生），不是抖动。全部内容逐字
节相同，收尾帧账与槽账都回到起点。

#### 5.26.6 门禁：`paging_gate.py` 的 pin V

pin V 钉的是「这一档必须真的是这样实现的」，不是「编得过」：

- 开关三处（`arkrtos_features.h` 缺省 0 / `Kconfig.features` 的两条 `depends on`）；
- 组合守卫（不进 DEMAND 或没帧池直接 `#error`）、槽数 1..64 校验；
- 槽号 `k+1` 编码、8 KB bounce、两轮挑选、`take_pair` 的 8 轮上限；
- **换出「先 invtlb 再 frame_free」的顺序**（按源码先后位置判，不按有没有这两个词）；
- 缺页分支三段顺序（读槽→清账→要帧）、失败不吃数据、成功的显式清零；
- `space_release()` 清槽、读数五字段、三处接口（hal / mpu / paging）、
  shell 两个分支与 PASS 串、驱动 `--swap-check` 的期望序列。

另有三条**变异自检**（把实现改坏，pin 必须报错）：把 `s_pg_bounce` 改名、
去掉 `la_tlb_inval_page(va)`、把 PASS 串改坏。三条都能被抓到，pin 才算有效。
最后是**真编译**：对 `arkrtos_port.c` 与 `arkrtos_paging.c` 用
`-DARKRTOS_USE_PAGING_DEMAND=1 -DARKRTOS_USE_PAGING_SWAP=1` 跑 `-fsyntax-only`
（「开关打开后就编不过」是这一档最容易漏的假绿）。

#### 5.26.7 体积与零变化

| 档 | text | data | bss |
|---|---|---|---|
| 基线（默认） | 366744 | 416 | 767508 |
| 按需档 | 366824 | 416 | 767508 |
| 按需 + SWAP 档 | 367784 | 416 | **841236** |

bss 增量 73 728 = 8 槽 × 8192 + bounce 8192，与实现逐项对得上。符号侧：swap 档有
`s_pg_swap_area` / `s_pg_bounce` / `s_pg_pair_slot` / `s_pg_swap_bits|hand|out|in|fail`
与 shell 的 `s_pmap_swap_hold`，**基线里这些符号一个都没有**。

零变化：默认档与 `git archive HEAD` 出的树各自以 `SOURCE_DATE_EPOCH=1600000000`
构建、`--strip-debug` 后 **sha256 逐字节相同**。这道读数取自引入 `KERNEL_SRCS`
排序（`$(sort)`）之前；排序之后同一比对改了口径，见 §5.27.6 的新基线
（可加载镜像 `a510d493…`）。

#### 5.26.8 仍然没做的

- **交换区不在持久介质上**：`.bss` 数组，掉电即失，也**不省物理内存**；
- **没有脏页判定**：每一对换出都整块 8 KB 拷贝，没有「没改过就不写」；
- **没有 ASID / TLB shootdown**（U-7）：换出仍是按 VA 单条 `invtlb`；ASID 档
  见 §5.28，但换出/坏页失效仍没有按 ASID 定向的手段；
- **`fail` 路径只有源码判据**：8 轮全败（池里所有对都在被用）在自测里构造不出来，
  没实跑过；
- **只在 LA64 板级实测**：RV32 侧依旧只有「关档零变化」这一条读数。

### 5.27 控制台登录（SEC-G01，2026-11-17）

#### 5.27.0 这一格要回答什么

QEMU 与真板上的串口是**唯一的运维入口**：谁把线插上，谁就能敲 `poweroff`、
卸掉还没跑起来的程序、或者把日志刷屏。SEC-G01 要的是「进控制台之前先证明
自己是运维」，而不是「设备认识你是谁」。

同一句话必须连着它的反面一起读，否则很容易被读成「有认证了、安全了」：

- 它**不是设备身份**（那是 SEC-G13，需要密钥/证书）；
- 它**不阻止**整片刷写 Flash（能接 SWD 的人不需要串口）；
- 口令**明文过串口**，抓线就能看到；
- 镜像泄露后，摘要可以**离线爆破**（这正是为什么它要求口令不能太弱）。

#### 5.27.1 三条设计规矩

1. **设备不存口令**：存 16 字节盐与 `sha256(salt || password)` 的 32 字节摘要，
   都在编译期注入。口令本身一个字节都不落设备。
2. **比对长度不随内容变**：`auth_digest_equal()` 固定 32 次 XOR 累加、一次
   `diff == 0`，没有「第一个字节不同就 return」——否则错误口令的耗时泄漏了
   匹配到第几位。
3. **开机即锁、错误到上限就硬锁**：`ARKRTOS_SHELL_AUTH_TRIES`（默认 5）次连续
   错误之后，控制台锁到**下一次复位**为止，此后即使口令正确也拒。不做指数
   退避：串口是低速链，退避只会让单位时间内的猜测次数变少、每次猜测更便宜。

放行名单只有 `login` / `logout` / `help`：没有 `login` 就永远解不开锁，`help`
用来解释为什么被拒。

#### 5.27.2 门钩放在哪里（这一格最容易做错）

门钩是 `ark_shell_t` 上的一个函数指针（`ark_shell_gate_fn`），调用点在
`ark_shell_eval.c` 的 `run_stage()` 里、**`ark_shell_find_command()` 之前**：

```c
#if ARK_SHELL_ENABLE_GATE
    if ((shell->gate_fn != NULL) && (shell->gate_fn(xargv[0]) != 0)) {
        eval_set_status(shell, 126);
        return 126;
    }
#endif

    cmd = ark_shell_find_command(xargv[0]);
```

放错位置（挂在查找之后）会留一条**旁路**：未知词会掉进 exec 回退，`./prog`
照样能跑，门钩等于没拦。`tools/shell_auth_gate.py` 的 F pin 把这条顺序钉死，
并且带一条「把门钩整体搬到查找之后必须报错」的自检。

`ARK_SHELL_ENABLE_GATE` 由 `ark_shell_config.h` 从 `ARKRTOS_USE_SHELL_AUTH`
派生；关档时这段调用点整段不编（预处理后的源码里 `gate_fn(xargv[0])` 一个
都不剩，门禁按预处理结果判）。

#### 5.27.3 凭据怎么进镜像

`tools/shell_auth_cred.py` 是唯一的入口：

```bash
py -3 tools/shell_auth_cred.py --salt 61726b2d72746f732d636f6e736f6c65 \
    --password <口令>                      # 打印凭据头正文
py -3 tools/shell_auth_cred.py --salt <hex> --password <口令> \
    --check <32 字节 hex>                  # 返回 0 = 匹配
```

生成的头叫 `arkrtos_shell_auth_local.h`，由 `.gitignore` 挡住
（`*_shell_auth_local.h`），构建时靠 `-DARKRTOS_SHELL_AUTH_LOCAL=1` 引进来。
`arkrtos_features.h` 在开档时逐项 `#error`：没有 shell、没有盐、没有摘要，
全部编不过并**点名缺哪一个**——不给「开了但没人能登录」或「有个通用默认
口令」两种假绿。

`example/qemu_loongarch64/Makefile` 里那份是**演示凭据**（盐
`ark-rtos-console`、口令 `arkrtos-demo`），只在 `EXTRA_CFLAGS` 里显式带
`-DARKRTOS_USE_SHELL_AUTH=1` 时才生成、才加 `-D`。真板子用脚本自己生成
一对，别抄这一份。

#### 5.27.4 QEMU 实跑读数（LA64，登录档）

构建：

```bash
make -f example/qemu_loongarch64/Makefile -j8 BUILD=build/qemu_la64_auth \
     EXTRA_CFLAGS="-DARKRTOS_USE_SHELL_AUTH=1"
```

开机横幅之后（直接抓串口，`-serial file:…`）：

```
console: locked - type 'login <password>'
```

三组命令序列（`tools/qemu_shell_drive.py` 驱动，三组都打印
`SHELL-DRIVE OK`）：

| 序列 | 期望 | 实测 |
|---|---|---|
| `info` → `login wrongpw` → `login arkrtos-demo` → `info` → `logout` → `info` | 先拒、错口令计次、正确口令解锁、登出后又锁 | `login: console is locked, type 'login <password>'` / `login: wrong password (1/5)` / `login: ok, console unlocked` / `logout: console locked` / 再 `console is locked` |
| `help` → `ls` → `w1`…`w5` → 正确口令 → `logout` → `ls` | help 放行、其余被拒 6 次、解锁后可执行、登出后又拒 | 与期望一致 |
| `login a`…`login e` → 正确口令 → `help` → `logout` | 第 5 次 `(5/5)` 硬锁；之后**正确口令也拒** | `login: wrong password, console locked until reset (5/5)` → `login: locked until reset (5 wrong passwords); reboot to try again` → `logout: console already locked until reset` |

`info` 在锁定状态下**不执行**，只打印拒绝理由——这是「报错而不是给错答案」：
它不会假装跑完了再把结果藏起来。

#### 5.27.5 体积

| 档 | text | data | bss |
|---|---|---|---|
| 默认档（`KERNEL_SRCS` 已排序） | 366744 | 416 | 767508 |
| 登录档（`ARKRTOS_USE_SHELL_AUTH=1`） | **370320** | **440** | 767508 |

开档 **+3576 B text / +24 B data**，与实现规模（SHA-256 头内静态实现被两个
TU 各内联一份、门钩、两条命令、若干提示串）对得上。

#### 5.27.6 零变化与「为什么改成 `objcopy -O binary` 比对」

这一轮的默认档**体积与 HEAD 逐项相同**（366744 / 416 / 767508），但它带进来
一行与安全无关的改动：`KERNEL_SRCS` 加了 `$(sort)`。

理由是实测出来的：`$(wildcard kernelsrc/src/*.c)` 的顺序来自 NTFS 的目录枚举，
往 `kernelsrc/src/` 里**增删一个 `.c`** 就会重排——这一轮新增
`arkrtos_shell_auth.c` 之后，`arkrtos_task.c` 的链接位置就换了。链接顺序一变，
默认档镜像就不再逐字节可复现，「零变化」也就没法比对。排序把链接顺序钉在
文件名字典序上，代价是**旧的可复现基线作废**（§5.26 里那个 `7e71813c…` 是
排序之前的读数，本机已不能复现）。

新的比对口径：

1. 两棵树都加同一行 `$(sort)`，各自以 `SOURCE_DATE_EPOCH=1600000000` 构建；
2. 用 `objcopy -O binary` 取**可加载镜像**（真正上板跑的字节），比 sha256。

实测：当前树与「`git archive HEAD` 出的树 + 同一行 `$(sort)`」两枚镜像
**逐字节相同**，`a510d493843a107911603930b41ca6787bc9165e5e22e70f2da555b85a8f1c3f`
（31 462 512 字节）。

为什么不再比 (stripped) ELF：`arkrtos_sha256.h` 是 header-only，默认档里这些
`static` 函数仍会被内联掉、不产生代码，但**gcc 给本地静态符号的自动编号**会
往前挪（`big.3660` → `big.3662`、`rwx.6503` → `rwx.6505` 等 32 项，地址与大小
全部相同），ELF 头里的符号表因此抖动。真正"上板跑的字节"是 loadable image，
用它比对更诚实，也更稳定。

#### 5.27.7 门禁：`tools/shell_auth_gate.py`

`py -3 tools/shell_auth_gate.py` 全过，`ci_gate.py` 随之从 43 项变 **44 项**。
它钉的是：

- **A** 开关三处：`features.h` 缺省 0 / Kconfig 两项（`depends on ARKRTOS_USE_SHELL`、
  `range 1 100`）/ Makefile 只在 `EXTRA_CFLAGS` 显式开档时生成凭据头并加 `-D`；
- **B** 关档零符号（真编 `.o` 后 `nm`）：关档 `arkrtos_shell_auth.c` 编出空
  TU、`ark_shell.c` 里没有 `ark_shell_set_gate`；开档三样都在，且
  `ark_shell_eval.c` 预处理后必须有门钩调用点、关档必须一个不剩；
- **C** 三个 TU 用真 LA64 交叉编译器带开关编得过；
- **D** 三条 `#error` 真会触发且各自点名（无盐 / 有盐无摘要 / 没有 shell）；
- **E** 常量时间比对（32 字节整段 XOR 累加、循环里没有提前 return）；
- **F** 门钩位置（在 `ark_shell_find_command` 之前）+ 放行名单只有
  `login` / `logout` / `help`；
- **G** 失败上限与硬锁（置 `s_auth_locked_hard`，且硬锁判断在口令比对之前）；
- **H** `login` / `logout` 在开档条件里注册、命令表预算 `+2`；
- **I** 开机即锁（`init` 置 1）+ shell 任务里真调 `init`/`set_gate` 并打印提示；
- **J** 演示凭据自洽（`Makefile` 里的盐/摘要必须与 `shell_auth_cred.py` 现算的一致，
  且脚本对错口令必须报 mismatch）；
- **K** 凭据头不入库（`.gitignore` 覆盖 + `git ls-files` 干净）。

E–K 每条都带「把源改坏必须报错」的自检，报不出错就算 pin 空转。

#### 5.27.8 仍然没做的 / 诚实边界

- **口令明文过串口**：板子没有可信显示，也没有非对称密钥，只能这样。抓线者
  能看到口令；
- **离线爆破**：摘要就在镜像里，弱口令挡不住离线字典（这一条在 Kconfig help
  与 `docs/安全认证准备.md` 里都写着）；
- **硬锁不跨复位**：复位就清（这正是设计意图——爆破者每次要重启板子），
  但也意味着**能按复位键的人可以无限重试**；
- **没有运行期改口令 / 多用户**：凭据在编译期固定；
- **不是设备身份**：不参与 SEC-G13，不替代签名与安全启动。

### 5.28 每空间 ASID（U-7，2026-11-17）

前几节留下的最大一条是「切空间 = 逐页 `invtlb 512` 次 + 重写 256 条」。这一节把
它换成**每空间一个 ASID + 一条独立 STLB way**，切换只写一次 `CSR.ASID`。

#### 5.28.1 开关与依赖

| 项 | 值 |
|----|----|
| 开关 | `ARKRTOS_USE_PAGING_ASID`（缺省 0；`EXTRA_CFLAGS=-DARKRTOS_USE_PAGING_ASID=1` 出独立镜像） |
| 依赖 | `ARKRTOS_USE_PAGING` + `ARKRTOS_USE_PAGING_PAGES=1` + `ARKRTOS_USE_PRIV=1`（不满足直接 `#error`） |
| 约束 | `ARKRTOS_PAGING_APPS_MAX <= 8`（每个空间占一条 way） |
| 关档 | 宏展开回原值（`LA_PG_WIN_WAY_OF(s) → LA_PG_WIN_WAY`、`LA_PG_ASID_OF(s) → 0`、`LA_PG_DYN_G → 0`），`la_asid_*` 整段不编 |

映射关系：空间 `s` → `ASID s+1`（ASID 0 留给内核默认视图）、`way s % 8`。

#### 5.28.2 实现要点

- **装空间**（`la_pg_install_space`）：先 `la_asid_swap(LA_PG_ASID_OF(space))` 再写
  256 条 PS=12 条目 —— `tlbwr` 把当时的 `CSR.ASID` 抄进条目，不切就装到别人名下；
  装完还原调用者的 ASID。
- **切换**（`arkrtos_port_paging_activate`）：`la_pg_space_ensure` → 写 `CSR.ASID`
  → **读回校验**首末两对（`la_pg_pair_check` 走 `tlbsrch` + `tlbrd`），缺了就补铺。
  这一步不赌 `invtlb` 是按 VA 还是按 (ASID,VA)，两种语义下结果都正确。
- **回收**（`space_release`）：先切到该空间的 ASID 再撤条目，然后切回；若撤的不是
  当前空间，再把当前空间补铺一遍。
- **启动**：`la_pg_boot_map` 装完 space 0 之后把 `CSR.ASID` 设成 space 0 的号，
  于是「内核默认视图」= space 0，shell 开机看到的窗口与关档一致。
- **动态 MMIO 别名**：ASID 档下带 `LA_PTE_G`（全局），否则换空间后控制台/驱动
  读不动 —— 它是内核 PLV0 通路，不属于任何 App 空间。

#### 5.28.3 QEMU 实测读数（`--elf build/qemu_la64_asid/ark_qemu_la64.elf`）

```
pmap asid: mode=on current=1 spaces=2
  space 0 -> asid 1 way 0 frame 0x04000000
  space 1 -> asid 2 way 1 frame 0x00000000
pmap asid: switch 16x 104910 ticks, 65568 ns/switch (clock 100000000 Hz)
```

最关键的一条是**同一 VA 在两个空间里命中不同的条目**（这是「条目并存」的直接
证据，关档下它会命中同一条）：

```
pmap look 0x02000000        -> entry=0   ps=12 ppn=0x04000000      (space 0)
pmap space 1                -> rc=0 (current=1)
pmap look 0x02000000        -> entry=256 ps=12 ppn=0x04200000      (space 1)
```

`pmap u1` 也换了读法：两次 `look` 的 entry 号是 `128` / `384`（不同 way 的不同条目），
不再是同一条被重写：

```
pmap u1: look0 entry=128 ps=12 ppn=0x04100000
pmap u1: look1 entry=384 ps=12 ppn=0x04300000
pmap u1: PASS - same VA, two frames
pmap u2: PASS - two instances, two spaces, neither frame overwritten
utest: 13 matched, 13 passed, 0 failed
```

#### 5.28.4 切换代价：三次读数与它们的分解

同一镜像连续三次 `pmap asid`：

| 次 | ticks / 16 次 | ns/次 | 说明 |
|----|----|----|----|
| 1 | 104910 | 65568 | 含 space 1 的**一次性**建窗（取 512 帧 + 装 256 条） |
| 2 | 97170 | 60731 | 瞬态 |
| 3 | 54960 | 34350 | 稳态 |

稳态 34.35 µs/次。这个数**不是 `csrwr` 本身的代价**：每次 `activate` 还做两次
`tlbsrch` 读回校验（QEMU 的 TLB 查找在 512 条量级上是线性扫描），当前的仪表
分不开「写 CSR」与「两次校验」。诚实写法是：**端到端 34 µs/次，其中包含两次
读回校验**；CSR 写本身的单次成本本轮没有独立读数。

#### 5.28.5 默认档逐字节不变

同一 `SOURCE_DATE_EPOCH=1600000000`、同 `--strip-debug` 口径，对 `git archive HEAD`
导出的树做同构编译：

```
6859ec5387dd4f4d6697dbf8d394d0dc2724e03ff7cf9c605a1f7d9d0b1ad8d8  HEAD 树
6859ec5387dd4f4d6697dbf8d394d0dc2724e03ff7cf9c605a1f7d9d0b1ad8d8  本轮工作区
```

三个读接口（`arkrtos_port_paging_asid_current/of/window_way`）在关档下是**宏**
（`hal.h` 的 `#else` 分支），不出符号；shell 的 `pmap asid` 分支整段在
`#if (ARKRTOS_USE_PAGING_ASID == 1)` 里 —— 默认档连一个字节的新代码都没有。

#### 5.28.6 门禁

`tools/paging_gate.py` 新增 pin W（三条变异自检 + 真编 ASID 档）：开关三处、
三条 `#error`、空间→ASID/way 的映射、装空间时切 ASID 且用该空间自己的 way、
装完还原、activate 写 ASID 并做读回校验、校验走 `tlbsrch`、回收先切 ASID、
启动设 space 0、`la_pg_dyn_lo` 四条路径都带全局位、`hal.h` 条件声明与关档宏、
shell 的 `pmap asid` 分支与 `ns/switch` 读数。另加一条**真负向**：把板级
`PAGES` 改成 0 再开 ASID=1，必须被那条 `#error` 拦住（编过 = 守卫失效）。

#### 5.28.7 仍然没做的 / 诚实边界

- **没有 TLB shootdown**：ASID 只解决「换空间要重写」。一个空间**改自己的映射**
  仍要按 VA 失效，且多核下要让别的核也失效 —— 那要 IPI，SMP 侧还没到那一步；
- **没有真实硬件读数**：本机只有 QEMU。`tlbwr` 抄 `CSR.ASID`、`tlbsrch` 按当前
  ASID 匹配这两条**是 QEMU 的行为**（LA64 手册语义一致，但本机没有第二块板可对拍）；
- **`invtlb` 的 ASID 语义**（U-7b 已取证，见 §5.29）：当时正因为不确定，`activate` 用读回校验兜住，
  `space_release` 先切到目标 ASID 再撤 —— 两种语义下都正确，但「QEMU 到底是哪一种」
  本轮没有测出来；
- **ASID 只有 10 位**：`APPS_MAX <= 8` 的限制来自 way 数（8 条 STLB way），不是 ASID 位宽；
- **窗口仍固定 2 MB / 512 帧**：这一节只换切换机制，不改变窗口大小与帧账本。

### 5.29 U-7b：按 ASID 定向失效在本载体上不可用（逐 op 取证）

U-7 的 `space_release` 与 `activate` 都把「`invtlb` 到底按 VA 还是按
(ASID, VA) 失效」当成一个没测过的前提，用读回校验兜着。这一节把它测掉：
结论是**本载体的 `invtlb` 没有可用的「只清某一个 ASID」的形态**，所以
U-7b 不引入定向失效，只把取证口留在 shell 里。

#### 5.29.1 逐 op 实测（QEMU 9.2.3，`pmap invop <op> [asid]`）

| op | 结果 | 现场 |
|----|------|------|
| 0 / 1 / 2 / 6 | 一次调用把**整张表**打掉（含内核恒等映射） | 下一条取指即 `[LA64U] KERNEL refill (plv0) badv=0x0023da58 era=0x0023da58 -> fatal`，`estat=0x00000800` |
| 4 / 5 | 活下来，窗口条目一个没掉 | 本载体上等价空操作 |
| 7 | 非法指令 | `estat ecode=0xd`，`badi=0x0649b587` |
| 3 | 按 VA 失效，只清这个 VA | 内核不受影响；**不看 ASID**（见 5.29.2） |

要害是 op=6：它在手册叙事里最像「按 (ASID, VA) 清一条」，实测是把整表
清空 —— 谁把它写进 `space_release`，谁就在第一次回收空间时打死机器
（本轮第一版 `pmap asidinval` 就是这么死的）。`pmap invop` 就是为这一条
留的取证口：它**先打印 op / asid 再动 TLB**，死了至少留下线索。

#### 5.29.2 对称两轮：op=3 不看 ASID

`pmap asidinval` 用 `asid_raw_swap`（只 `csrwr`、不补铺）逐个读条目，
A..D 四步、两轮对称：

```
pmap asidinval: A baseline space0=1 space1=1
pmap asidinval: B op3(va) space0=0 space1=0
pmap asidinval: C reinstall space0=1 space1=1, D op3 under space0 space0=0 space1=0
pmap asidinval: OP3 IGNORES ASID - clearing one space also drops every space entry of that VA
```

当前 ASID 是 space 1 时按 VA 撤一次，space 0 的那条也掉了；换到 space 0
的 ASID 再撤一次，两条又都掉了。判词三态（`OP3 IGNORES ASID` /
`OP3 SCOPED TO CURRENT ASID` / `MIXED`），本轮落在第一态；`MIXED` 存在就是
为了不让「两轮读数不吻合」被凑成某个形态。

#### 5.29.3 于是代码只多了两个诊断口

- `arkrtos_port_paging_asid_raw_swap(asid)`：只写一次 `CSR.ASID` 并返回
  旧值，不记账、不补铺。`activate` 会补铺，所以拿它测「条目还在不在」
  永远测不出来；
- `arkrtos_port_paging_inval_va(va)`：`invtlb 3`。单独看它没有意义，必须和
  op=6 的「整表清空」摆在一起才是证据。

两条都只在 `ARKRTOS_USE_PAGING_ASID == 1` 下存在，默认档连声明都没有。
曾经写过 `arkrtos_port_paging_inval_va_asid()`（`invtlb 6`）并已**整块删除**：
它一调用就停机，留在源码里就是一个等着被调用的坑。

#### 5.29.4 默认档仍然逐字节不变（这次是同参对拍）

同 `SOURCE_DATE_EPOCH=1600000000`、同 `-B`、同 `--strip-debug`，三棵树各
自全新编一遍：

```
3a6c4ba7d84d1b0be333e3e2dd54e0f228a5f00e5ea544bc31af535c4da1c3a0  git archive 1c14a36（U-7 之前的树）
3a6c4ba7d84d1b0be333e3e2dd54e0f228a5f00e5ea544bc31af535c4da1c3a0  git archive d5ff09f（U-7 提交）
3a6c4ba7d84d1b0be333e3e2dd54e0f228a5f00e5ea544bc31af535c4da1c3a0  本轮工作区（含 U-7b）
```

`size`：默认档 `text 367264 / data 440 / bss 767508`；ASID 档 `text 369848`
（U-7b 的两条诊断口与 `pmap invop` / `pmap asidinval` 都在这 +2584 字节里）。

**一条必须写下来的口径问题**：§5.28.5 记的 `6859ec53…` 今天复现不出来 ——
上面三棵树同参编出来都是 `3a6c4ba7…`。三棵树给同一个数，说明这个差异不是
源码带来的（只可能来自那次构建的现场，例如复用了没清理的对象文件）。那次
读数**作废**，以本节这组同参对拍为准；结论不变：默认档里没有 U-7 / U-7b
一个字节的新代码。

#### 5.29.5 边界

- **不得读成「Ark RTOS 不支持 shootdown」**：这里判的是本机 QEMU 9.2.3 的
  `invtlb` 没有可用的「按 ASID 定向失效」，真机（ISA 层面）未验；
- 多核 shootdown 还要 IPI，SMP 侧没到那一步；
- `space_release` 的「切 ASID → 按 VA 撤 → 切回 → 补铺」不能省：op=3 连带
  清掉别的空间同 VA 条目已由 5.29.2 实测确认。

### 5.30 镜像池的 Flash 后端与 SEC-G17 开机 CRC 复核（2026-11-18）

#### 5.30.1 先修载体：QEMU 的镜像池原来写不进去

QEMU 的 Flash 区就是普通 DRAM，上电全 `0x00`，而真 Flash 擦除后是 `0xFF`。
`arkrtos_loader` 找空位（`find_clean`）判的是「这段是不是全 `0xFF`」，于是：

```
pool free : 0 B
[LOADER] install returned -14 (ERR_NOSPACE)
```

⇒ 在这个模拟器上，「把镜像装进池再开机复核」这条路**一次都没真跑过**，唯一的读数是
`sec` 那行 SKIP。修法只在板级、只覆盖池：

| 位置 | 内容 |
|---|---|
| `board/qemu_loongarch64/arkrtos_board.c` | `QEMU_POOL_FLASH_BEGIN/END` = `IMAGE_POOL_BASE..IMAGE_POOL_END`；`arkrtos_qemu_pool_range()` 判范围（不回绕、不越界） |
| `arkrtos_port_flash_erase(addr,size)` | 范围内写 `0xFF`，否则 `-1` |
| `arkrtos_port_flash_write(addr,data,len)` | 范围内按字节覆盖，否则 `-1` |
| `arkrtos_port_flash_sector_size(addr)` | 池内返回 `IMAGE_POOL_SECTOR`，池外 `0` |
| `arkrtos_port_board_init()` | 在 `arkrtos_trace_init()` 之前把整池擦成 `0xFF` |

池以外（代码区 `0x00200000..0x00280000`、配置区 `0x00280000..0x002A0000`，内核正在那儿跑）
仍然明确失败，不做「假装能写」。当时代码区还是 256 KB，改完后 `pool free : 524288 B (512K)`（正好 usable）；
2026-11-20 内核区扩到 512 KB（LA64 的 RTSTAT 镜像放不下原布局，见 §5.30 末的布局表），镜像池随之变成
`0x002A0000..0x00400000`（1408 K / usable 1280 K，11 个 128 K 扇区），实测 `pool free : 1310720 B (1280K)`。

**边界**：RAM 后端不模拟位翻转、不要求写前先擦、复位即重擦（内容不跨复位/掉电）。
这条只解决「能不能装」，不等于「Flash 行为像真片」。见 [已知限制](已知限制.md)。

#### 5.30.2 装得进去之后立刻抓到一条真缺陷

`utest run loader_crc_recheck_pool_e2e` 第一次真装就报 `-5`。逐步定位（临时
`ARKRTOS_LOGE` 取证，取证后已撤干净）：

```
DIAG readback crc mismatch
DIAG got=f2a5e2fa want=56821d7f base=260000 total=148
```

根因：`arkrtos_loader_load_buffer()` 把重定位表写进池之后，**没有把表那一段折进
`crc_written`**；而回读复核比的是 `arkrtos_loader_image_crc()`（头 + 表 + 负载）⇒ 任何
`reloc_count > 0` 的镜像经这条路径装完**恒报 CRC 错**，还在池里留一份 UNCOMMITTED 残片。
流式路径 `arkrtos_loader_stream_image()` 一直是折进去的，所以只有 buffer 路径中招。

修法（`kernelsrc/src/arkrtos_loader.c`）：

```c
crc_written = arkrtos_loader_crc_header(&hdr);
{ uint32 rt_len = p_src->reloc_count * ARKRTOS_APP_RELOC_SIZE;
  if(rt_len != 0u) {
      crc_written = arkrtos_crc32(image + ARKRTOS_APP_RELOC_OFFSET, rt_len, crc_written);
      if(arkrtos_port_flash_write(base + ARKRTOS_APP_RELOC_OFFSET,
                                  image + ARKRTOS_APP_RELOC_OFFSET, rt_len) != 0) { /* ERR_FLASH */ }
  } }
```

**这条修复在默认档（开关关着）也生效**，所以默认档镜像是真实行为变化：sha256 由
`6bbcda8b…` 变为 `fbd1094e…`，`text` `367264 → 367536`（+272）。不得声称「逐字节不变」。

#### 5.30.3 SEC-G17 的读数

开关档（`EXTRA_CFLAGS="-DARKRTOS_USE_IMAGE_CRC_RECHECK=1"`）：

```
PASS  loader_crc_recheck_relocated
PASS  loader_crc_recheck_zero_delta
PASS  loader_crc_recheck_unknown_ram_base
PASS  loader_crc_recheck_pool_e2e
utest: 17 matched, 17 passed, 0 failed, 0 skipped; assertions 155 run / 0 failed
```

只跑池内那一条、并临时保留镜像让 `sec` 看得见（取证后已恢复 `uninstall`）：

```
RUN   loader_crc_recheck_pool_e2e
PASS  loader_crc_recheck_pool_e2e
utest: 1 matched, 1 passed, 0 failed, 0 skipped; assertions 5 run / 0 failed
  [PASS] images.crc_recheck checked=1 bad=0 unknown=0 first=0
pool free : 523960 B (511K)
```

池空着的时候（同一镜像直接 `sec`）：

```
  [SKIP] images.crc_recheck no pool image to recheck
```

默认档（不带 `EXTRA_CFLAGS`）：

```
utest: 13 matched, 13 passed, 0 failed, 0 skipped; assertions 133 run / 0 failed
sec: 12 checks - pass 7, fail 0, finding 2, skip 3
  [SKIP] images.crc_recheck flash holds relocated bytes; header crc32 covers the nominal form
```

最后一行文案与改动前**逐字相同**（默认档 SKIP 口径不变），`tools/sec_crc_gate.py` 按源码钉住
并带变异自检。

#### 5.30.4 复现命令

```sh
# 默认档
SOURCE_DATE_EPOCH=1600000000 make -f example/qemu_loongarch64/Makefile BUILD=build/crc_zero -j8
env -u PYTHONHOME -u PYTHONPATH py -3 tools/qemu_shell_drive.py \
    --elf "$(pwd -W)/build/crc_zero/ark_qemu_la64.elf" --boot 90 --timeout 45 --port 45795 \
    --cmd "utest run"      # 期望 13/13

# 开关档
SOURCE_DATE_EPOCH=1600000000 make -f example/qemu_loongarch64/Makefile BUILD=build/crcrc \
    EXTRA_CFLAGS="-DARKRTOS_USE_IMAGE_CRC_RECHECK=1" -j8
env -u PYTHONHOME -u PYTHONPATH py -3 tools/qemu_shell_drive.py \
    --elf "$(pwd -W)/build/crcrc/ark_qemu_la64.elf" --boot 90 --timeout 45 --port 45793 \
    --cmd "utest run"      # 期望 17/17、assertions 155

# 门禁（真 armclang，两档各编两个 TU + 8 条 pin 的变异自检）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/sec_crc_gate.py   # SEC-CRC-GATE OK (8 pin + 4 TU 两档)
```

端口每次换新；`qemu_shell_drive.py` 偶发「90 s 内没等到 shell 提示符」，同参数换端口重试即可
（已确认与代码无关）。

#### 5.30.5 后续体积变化（2026-11-18，与本节无关）

同一个默认档镜像在**后一次纯代码整理**之后不再是 `fbd1094e…`。整理内容是
`kernelsrc/src/arkrtos_task.c` 的 `svc_file_sys()`（458 行）按注释里写明的路径模型边界
拆成三个函数——`svc_fs_volume()`（子命令 1..10，卷相对）、`svc_fs_namespace()`
（子命令 11..23，命名空间绝对）与只剩参数检查 + 一次分支的 `svc_file_sys()`；
case 体逐行搬运、只整体 dedent 4 个空格，返回值与错误码一处未改。

同口径（`SOURCE_DATE_EPOCH=1600000000`、`objcopy -O binary`）读数：

```
before  text 367536 / data 440 / bss 767508   sha256 fbd1094e…   31,462,512 B
after   text 367584 / data 440 / bss 767508   sha256 66a09c13…   31,462,512 B
```

即 `text` +48 B（多一层函数调用与两次 `-O2` 下不再共享的常量装载），`data`/`bss` 与
镜像总长不变。**不得声称逐字节不变**——二进制确实变了，只是行为不变。行为侧的判据是
`python example/qemu_loongarch64/shell_check.py` 仍 `PASS`（横幅/help/uname/dt/ls/cat/
stack/utest 九组全部通过），`stack` 报 `9 task(s)`、`0 unmeasured`、`0 at/over 90%`。

#### 5.30.6 再整理一次：`elf_load()` 拆成三个阶段（2026-11-18）

紧接着上一节，`kernelsrc/src/arkrtos_elf.c` 的 `elf_load()`（278 行）按它自己注释里已经
写明的三段——**第一遍只校验 / 第二遍真写 / 注册任务并降权**——拆成三个 `static` 函数：

| 函数 | 位置 | 职责 |
|---|---|---|
| `elf_check_segments()` | `arkrtos_elf.c:558` | 只遍历程序头做校验（拒绝 `PT_INTERP`、拒绝 `p_filesz > p_memsz`、用减法写的窗口范围判据），一个字节都不写，返回可见 `PT_LOAD` 段数 |
| `elf_write_segments()` | `arkrtos_elf.c:621` | 真写：`filesz` 段拷贝 + `memsz - filesz` 清零（BSS）；分页档走 `elf_copy_to_window()` / `elf_zero_window()`，非分页档走 `elf_read_exact()` / `elf_memzero()` |
| `elf_start_task()` | `arkrtos_elf.c:694` | `注册 → 降权 → MPU 窗口写回 → 绑帧`整段（`ARKRTOS_ENTER_CRITICAL()` 到 `ARKRTOS_EXIT_CRITICAL()`），返回任务 id |

`elf_load()` 本身从 278 行降到 **143 行**，只剩「参数检查 + 读 ELF 头 + 入口判据 +
三次阶段调用 + 填 `p_info`」。等价性仍用归一化行多重集比对：差异只允许是新增的函数骨架、
`*p_segs` / `*p_bytes` / `return id` 三处收尾语句、三次调用，以及把 `win_base` 在
`elf_start_task()` 里换成 `(uint32)ARKRTOS_ELF_WINDOW_BASE`（该函数不再收这个参数）。

同口径读数（`SOURCE_DATE_EPOCH=1600000000`、`objcopy -O binary`）：

```
before  text 367584 / data 440 / bss 767508   sha256 66a09c13…   31,462,512 B
after   text 367600 / data 440 / bss 767508   sha256 2968c36e…   31,462,512 B
```

`text` +16 B，`data` / `bss` 与镜像总长不变；**同样不得声称逐字节不变**。
行为判据是 `example/qemu_loongarch64/shell_check.py` 仍 `PASS`（横幅/help/uname/dt/ls/cat/
stack/utest 九组全部通过，`[ELF]`/`[MOD]` 两条 tamper check 也在其中）。

拆完之后 `doc_audit.py funcs` 的最长函数换成 `arkrtos_modld.c` 的 `mod_one_rela()`
（252 行），`elf_load` 退出最前列。

#### 5.30.7 又拆一笔：`mod_one_rela()` 按四类机器分开（2026-11-18）

`kernelsrc/src/arkrtos_modld.c` 的 `mod_one_rela()`（252 行）是一个大 `switch`，本来就把
重定位分成四类互不相干的机器。现在按这四类拆成四个 `static` 函数，各自只保留自己那段
`switch`，`default` 统一返回一个局部哨兵 `MOD_RELA_PASS`（= 1，意思是"这个类型不归我"）：

| 函数 | 管哪些类型 | 需要什么 |
|---|---|---|
| `mod_rela_push()` | `R_LA_SOP_PUSH_ABSOLUTE` / `PUSH_PCREL` / `PUSH_PLT_PCREL` / `PUSH_GPREL` | 符号值 `S`、地址 `P`、栈；`GPREL` 还会去查 GOT 槽 |
| `mod_rela_alu()` | `R_LA_SOP_ADD` / `SUB` / `SL` / `SR` / `NOT` / `ASSERT` | 只动 SOP 栈 |
| `mod_rela_pop()` | `POP_32_S_10_12` / `S_5_20` / `S_10_16_S2` / `S_0_10_10_16_S2` / `POP_32_U` | 栈顶值 + `P`（要读/写指令字） |
| `mod_rela_write()` | `R_LA_32` / `R_LA_64` | 符号值 `S` + `addend` + `P` |

`mod_one_rela()` 自己只剩「算 `P` 与 `S` → 依次问这四个 → 四类都不认就
`c->last_reloc = type` 并返回 `ARKRTOS_MOD_ERR_RELOC`」，约 40 行。case 体是**逐行搬运**，
等价性用归一化行多重集比对：差异只允许是四个函数骨架、`MOD_RELA_PASS` 的 `default`、
四条 dispatch 与签名。`switch` 的分派顺序不影响语义——四类的类型集合互不相交。

同口径读数：

```
before  text 367600 / data 440 / bss 767508   sha256 2968c36e…   31,462,512 B
after   text 367392 / data 440 / bss 767508   sha256 1d94199f…   31,462,512 B
```

即 `text` **−208 B**（每个 helper 只看自己那几条 `case`，`-O2` 下的跳转表与常量装载都变窄了），
`data`/`bss` 与镜像总长不变。行为判据 `shell_check.py`：三次里两次 `PASS`，第三次在
`help` 一行上判 FAIL，原始转录末端是 §5.13.4 那个既有的
`FATAL estat=0x000d0000 ecode=0x0000000d era=0x00000008` 间歇异常——按上一节的结论，
这一类间歇与代码改动无关，两次 `PASS` 的转录里 `[ELF]`/`[MOD]` 两条 tamper check 都在。

拆完之后 `doc_audit.py funcs` 里超过 120 行的函数从 32 个降到 **31 个**，最长换成
`arkrtos_apppack.c` 的 `arkrtos_apppack_build()`（244 行）。

#### 5.30.8 第三笔：`arkrtos_apppack_build()` 按五段分开（2026-11-18）

`kernelsrc/src/arkrtos_apppack.c` 的 `arkrtos_apppack_build()`（244 行）本来就是按注释分好的
五段流水，现在按这五段拆成五个 `static` 函数，主函数只剩「串起来并回填 `stats`」：

| 函数 | 行数 | 管什么 | 进出什么 |
|---|---|---|---|
| `apppack_resolve_entry()` | 约 30 | 入口的两种合法形态（`e_entry` 直接落在负载内 / 落在头里的 `entry_off` 字段） | 出参 `*p_entry_off` |
| `apppack_count_pass()` | 约 31 | 第一遍：逐条扫出补丁条数，不写任何输出 | 出参 `*p_count` / `*p_n` |
| `apppack_write_table()` | 约 29 | 第二遍：写补丁表 + 确定性对账 + 偏移从「相对负载」换算成「相对镜像」 | 入 `n`/`payload_off`，出 `table[]` |
| `apppack_verify_payload()` | 约 61 | 拷 A → 按表打补丁 → 与 D 逐字节比 → 再还原成 A（不让校验顺手改数据） | 入 `table`/`n`，出 `payload` |
| `apppack_fill_header()` | 约 51 | 头清零 + 逐字段填 + `image_id` + 三段 CRC | 入 `payload`/`table`/`n` |
| `arkrtos_apppack_build()` 自己 | **101**（原 244） | 校验参数 → 算 `payload_off`/`total` 与越界判据 → 依次调五个 → `*out_len = total` → 回填 `stats` | — |

`payload_off` / `total` / `table` / `payload` 四个量**留在主函数**：它们要等第一遍数完 `n`
才算得出来，搬进任何 helper 都只会多一层出参。case 体是逐行搬运，等价性同样用归一化行
多重集比对：only-OLD 只剩被有意改写的几类语句（`entry_off =` → `*p_entry_off =`、
`count.n_* = 0u` 的清零、`apppack_scan(…, &count)` → `(…, p_count)`、
`n = count.n_rom + …` → `p_count->…`）与被搬到函数外的长注释首行；only-NEW 全是函数骨架、
五条调用与 `if (rc != 0) { return rc; }`。**没有语句丢失**。

**这一笔在默认档下体积逐字节不变**，与本文件前两笔的口径不同，原因必须写清：

```
before  text 367392 / data 440 / bss 767508   sha256 1d94199f…   31,462,512 B
after   text 367392 / data 440 / bss 767508   sha256 1d94199f…   31,462,512 B
```

默认档 `ARKRTOS_USE_APPPACK=0`，整个 TU 被 `#if` 包成空目标，所以拆它不可能改变默认档的
任何一个字节。这不是「拆分不留痕」的证据，只是「这条路径默认关着」。

**那 `APPPACK=1` 的路径怎么验？** 所有板级头都是 `#undef` 后 `#define … 0`，命令行 `-D`
打不过去；而 `tools/arkmake_syntax_gate.py` 与 `ci_gate.py` 都不编这个 TU。做法是造一个只
用于校验的板级头 shim（放 `build/` 下，不入库）：

```c
/* build/_apppack_chk/arkrtos_board_config.h */
#include "../../board/qemu_loongarch64/arkrtos_board_config.h"
#undef  ARKRTOS_USE_APPPACK
#define ARKRTOS_USE_APPPACK 1
```

把它放在 `-I` 列表第一位、配合 `-include arkrtos_board_config.h`，用与 make 相同的 LA64 flags
`-c kernelsrc/src/arkrtos_apppack.c`：`exit=0`、零 warning，`nm` 见到
`T arkrtos_apppack_build` 与 `T arkrtos_apppack_error_name` 两个真符号。**这是目前唯一能证明
该 TU 在 `APPPACK=1` 下类型正确的办法**；它不是行为验证，`APPPACK=1` 的端到端仍属未验证。

读数与判据：

- 构建：`make -f example/qemu_loongarch64/Makefile` `exit=0`，`arkrtos_apppack.c` 零 warning；
- 行为：`shell_check.py` **一次 `PASS`**（横幅/help/uname/dt/ls/cat/stack/utest 九组全过，
  转录里 `[ELF]`/`[MOD]` 两条 tamper check 都在）；
- `format_gate.py`：`361 own C/asm file(s) clean on F1-F4`；
- `doc_audit.py funcs`：超过 120 行的函数 **31 → 30**，`arkrtos_apppack_build` 退出清单，
  `apppack.c` 内最长换成 `apppack_scan()`（185 行，本笔未动）。

结论档位：**仅编译通过 + 默认档 QEMU 端到端跑过**；`APPPACK=1` 只到「类型正确」，没有运行证据。

#### 5.30.9 第四笔：`svc_fs_namespace()` 按四段子命令区间分开（2026-11-18）

`kernelsrc/src/arkrtos_task.c` 的 `svc_fs_namespace()`（232 行）处理 SVC 0x1C 的 11..23 号子命令。
这些编号本来就分成连续的四段，正好对应四种对象，于是按段拆成四个 `static`：

| 函数 | 子命令 | 管什么 | 行数 |
|---|---|---|---|
| `svc_fs_ns_io()` | 11..14 | open / read / write / close（句柄上的字节流） | 74 |
| `svc_fs_ns_meta()` | 15..17 | stat / seek / readdir | 73 |
| `svc_fs_ns_tree()` | 18..21 | unlink / mkdir / rmdir / rename | 76 |
| `svc_fs_ns_size()` | 22..23 | truncate(path) / ftruncate(handle) | 36 |
| `svc_fs_namespace()` 自己 | — | 四条边界判断 + 未识别号回 `EINVAL` | **24**（原 232） |

case 体逐行搬运，各族的 `switch` 与 `default` 沿用原样式；等价性用归一化行多重集比对：
**only-OLD 为空**（一条语句都没丢），only-NEW 只有四个函数骨架、三条额外的 `default` 与四条
边界判断。**调度锁一处未增未删**：原函数里「不自己加锁，ark_vfs 的 port lock 就是
`arkrtos_sched_lock`」那句说明留在第一族注释里，后三族沿用同一规则。子命令编号连续，所以
入口只按边界选族；24 及以上仍落到最后的 `EINVAL`，与原 `default` 同义。

一个在这笔里真实踩到的坑，记下来备查：用脚本拼装新函数时，插在 `switch` 前面的那行
分节注释漏了收尾 `*/`，C 注释直接吃到第一个 `case` 行尾的 `*/` 为止，于是整个
`switch`/`{`/第一个 `case` 一起被当成注释 —— 归一化比对立刻显示 only-OLD 少了
`case ARKRTOS_PATH_SUB_OPEN:`、only-NEW 多了两条 `switch`，`format_gate` 也不会报
（它只管字节规则）。**拆分脚本必须自己比对，不能指望编译或格式门禁兜住这一类。**

读数：

```
before  text 367392 / data 440 / bss 767508   sha256 1d94199f…   31,462,512 B
after   text 367336 / data 440 / bss 767508   sha256 b0ad6a91…   31,462,512 B
```

`text` **−56 B**，`data`/`bss` 与镜像总长不变。行为判据 `shell_check.py` 一次 `PASS`；
`arkrtos_task.c` 的编译告警与改动前**同为 147 条**（全部是既有的指针宽度转换告警，
新增代码零新增告警）。`doc_audit.py funcs`：超过 120 行的函数 **30 → 29**，
最长换成 `arkrtos_mini_load_image()`（229 行）。

#### 5.30.10 第五笔：`arkrtos_mini_load_image()` 按「阶段 / 流 / 收尾」分开（2026-11-18）

`kernelsrc/src/arkrtos_mini.c` 的 `arkrtos_mini_load_image()`（229 行）实际做三件事：读镜像头与
重定位表、按头里声明的尺寸借内存、把负载从文件流式搬进 RAM 并逐段累 CRC，最后定入口。按这三件事
拆成两段 `static` + 一个收尾很短的入口：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mini_load_stage()` | 读头 → `arkrtos_loader_check_header(p_hdr, 0, MASK_MINI)` → 读重定位表 → 再校一次 → 算 `code_size` / `code_span` / `ram_size_hdr` / `code_want`、回绕自证、`max_block` 与栈下限判据 → `arkrtos_ptable_mini_alloc()`，末尾回填 `pt_index` / `code_base` / `code_block` / `code_size` / `ram_base` / `ram_block` / `ram_size` | 92 |
| `mini_load_stream()` | `while(cur < total)` 逐段读文件、就地搬进 `p_img->code_base` / `p_img->ram_base`、累 CRC；`yield_credits` 仍在 `#if (ARKRTOS_MINI_YIELD_CHUNKS > 0)` 里，只在出参上多交一个 `*p_crc` | 93 |
| `arkrtos_mini_load_image()` 自己 | 环境判据（`ARKRTOS_MINI_READ_VFS`）→ 加调度锁 → 调两段 → CRC 比对 → 算入口（ARM 置 Thumb 位）→ 解锁 → 回填 `entry` / `crc`；失败按阶段分别回滚 | **61**（原 229） |

`payload_off` / `total` / `delta_rom` / `delta_ram` 这类「数出来才算得出」的量没有做成出参，而是在
`mini_load_stream()` 里就地从 `p_hdr` / `p_img` 重算 —— 否则第二段要接七八个入参。`free_out` /
`unlock_out` 只留在入口函数，两段内部一律 `return <错误码>` 上交。等价性仍用归一化行多重集比对：
only-OLD 只剩被有意改写的语句（`ret = X; goto unlock_out;` / `goto free_out;` 共 9 条，以及挪到
声明处的局部量），**没有语句丢失**。

**这一笔抓到两个默认档根本看不见的真错**，如实记下。`ARKRTOS_USE_MINIAPP` 在所有板级头里默认是
0，该 TU 在默认档被 `#if` 整段吃掉（`size -A build/mini_split2/kernelsrc/src/arkrtos_mini.o` 的
`.text` 为 0），所以普通 `make` 通过什么也证明不了。用 `build/_mini_chk/arkrtos_board_config.h`
（`#include` 板级头后 `#undef` + `#define ARKRTOS_USE_MINIAPP 1`，`-I` 排第一、配
`-include arkrtos_board_config.h`）强制打开开关做 `-c` 编译，第一次就报 `code_base` 未声明
（3 处）：它原来是主函数的局部量，搬进 `mini_load_stream()` 后不存在，改成 `p_img->code_base`
（拷贝目标、CRC 段、终点判断三处）；入口里 `entry = code_base + ...` 同样改成
`p_img->code_base + ...`。修后 `MINIAPP=1` 编译 `exit=0`、**5 条告警，与 HEAD 基线同为 5 条**、
`nm` 见 18 个 `T` 符号（`arkrtos_mini_run` / `_info` / `_stop` / `_running` / `_forget` /
`_on_fault` / `_on_task_exit` / `_autostart_*` 等）。

读数：

```
before  text 367336 / data 440 / bss 767508   sha256 b0ad6a91…   31,462,512 B
after   text 367336 / data 440 / bss 767508   sha256 b0ad6a91…   31,462,512 B
```

**默认档逐字节不变**，但原因不是「拆得干净」，而是 `ARKRTOS_USE_MINIAPP=0` 时这个 TU 是空目标，
改它不可能改变默认档一个字节 —— 与 §5.30.8 的 `apppack` 同型，**不能套用 −N B 的句式**。
`shell_check.py` 仍 `PASS`，但它不经过这条路径（默认档没有 MINIAPP 可跑），所以只能读成
「默认档没被破坏」，**不能读成这笔拆分的行为证据**。结论档位：编译面通过（含强制开开关的 shim
编译），**行为未验证**。`doc_audit.py funcs`：超过 120 行的函数 **29 → 28**，最长换成
`arkrtos_mod_load()`（220 行）。

#### 5.30.11 第六笔：`arkrtos_mod_load()` 按「读 / 解析 / 链接 / 记槽」分开（2026-11-18）

`kernelsrc/src/arkrtos_modld.c` 的 `arkrtos_mod_load()`（220 行）是模块装载的总入口：自检参数、
读整个 `.o`（含可选签名 tag）、解析 ELF 头与节表、布局 / GOT / 搬运 / 重定位 / 找入口、最后记槽
并起任务。按这五段拆成五个 `static`：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mod_args_valid()` | 每个 `argv[i]` 非空且短于 `MOD_ARG_LEN`；「缺省成一个参数」不在这里补（那要一个返回后仍然有效的数组，只能由调用点自己持有） | 17 |
| `mod_read_image()` | 读整个 `.o` 进 `s_mod_file`；`ARKRTOS_USE_EXEC_SIGN` 下校验尾部 32 字节 tag 并把 `len` 缩回正文长度（不缩的话坏对象会拿 tag 当节头表读） | 35 |
| `mod_parse_elf()` | 清零链接上下文、按本槽落点填 `img`/`base`/`size`、校 ELF 头（magic / class / machine / `ET_REL`）、校节表边界、找 `.symtab` / `.strtab` | 67 |
| `mod_link_image()` | `mod_place` → `mod_got_alloc` → `mod_copy` → `mod_got_fill` → `mod_reloc_all`（失败时把缺的符号名 / 坏的重定位类型留在诊断变量里）→ `mod_find_entry` | 46 |
| `mod_commit_slot()` | 取 basename、写槽的名字与几何、把 `argv` 拷进槽自己的静态区并补 `argv[argc] = NULL` | 35 |
| `arkrtos_mod_load()` 自己 | 参数形态与缺省、清诊断、选空槽、串起五段、`arkrtos_task_register`（失败时把 `used` 清回 0） | **69**（原 220） |

`mod_parse_elf()` 与其余几段都收 `mod_link_t *`，`field` 一律从 `.` 改成 `->`；`mod_commit_slot()`
只读上下文（`const mod_link_t *`）。等价性仍用归一化行多重集比对：only-OLD 只剩这些 `.` → `->`、
`&c` → `c` 的改写（33 条），**没有语句丢失**。

**这一笔是本轮第一次拿到真运行读数**（前四笔里有三笔落在默认档关着的路径上）。LA64 默认档
`ARKRTOS_USE_MODLOAD=1`，`example/qemu_loongarch64/mod_demo.c` 在开机后会把编进镜像的
`modhello.o` 写进 ramfs 再现场链接执行；QEMU 串口抓到：

```
[MOD] write /mnt/ram/modhello.o rc=0 size=6256
[MOD] ld[modhello] hello from a relocatable module
[MOD] module finished after 0 ms; try: ld /mnt/ram/modhello.o a b
[MOD] tamper check PASS (bad tag refused, rc=4294967285)
```

即装载、重定位、入口查找、参数传递、跑完退出、以及签名 tag 拒绝这六件事全部照旧。`shell_check.py`
也 `PASS`（但它本身不跑 `mod` 命令，模块这条链的证据来自上面的串口读数，不是来自它）。

读数：

```
before  text 367336 / data 440 / bss 767508   sha256 b0ad6a91…   31,462,512 B
after   text 367400 / data 440 / bss 767508   sha256 a5403823…   31,462,512 B
```

`text` **+64 B**（多出五组函数进出与一次 `slot` 传参，`-O2` 没能完全内联掉），`data`/`bss` 与镜像
总长不变；`arkrtos_modld.c` 在默认档 **0 条告警**。`doc_audit.py funcs`：超过 120 行的函数
**28 → 27**，最长换成 `arkrtos_loader_stream_image()`（212 行）。

#### 5.30.12 第七笔：`arkrtos_loader_stream_image()` 按「表 / 负载 / 收尾」分开（2026-11-18）

`kernelsrc/src/arkrtos_loader.c` 的 `arkrtos_loader_stream_image()`（212 行）是串口安装的落盘段：
先整段接收重定位表并校验，再按 ACK 节奏收负载、边收边打重定位补丁、边算两个 CRC，最后比 CRC 与
签名。拆成两段 `static` + 收尾：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `loader_stream_reloc_table()` | 按 `ARKRTOS_LOADER_CHUNK_SIZE` 收完整张重定位表、原样落盘（同时滚标称 CRC 与落盘 CRC、喂签名）、落盘后调 `arkrtos_loader_check_header()` 校验表体，失败先打日志再 NAK（NAK 只带 0x15，日志是主机唯一能拿到的原因），通过才 ACK 放行负载 | 66 |
| `loader_stream_payload()` | `while(written < image_size)` 逐块读、`carry` 处理被表项切开的尾部、先算标称 CRC 再调 `arkrtos_loader_reloc_apply()` 打补丁、写 Flash、滚落盘 CRC、每块 ACK | 106 |
| `arkrtos_loader_stream_image()` 自己 | 两个 CRC 的初值、`arkrtos_sign_stream_begin()`、串起两段、比 `crc_recv` 与头里的 `crc32`、`arkrtos_sign_stream_end()`、交回 `*p_crc_written` | **61**（原 212） |

两个 CRC 都没有做成「拷回去再传进来」：`loader_stream_*` 收 `uint32 *p_crc_recv` /
`uint32 *p_crc_written`，在调用点的值上继续滚。等价性用归一化行多重集比对：only-OLD 只有
`crc_recv = arkrtos_crc32(..., crc_recv)` 这类改写成 `*p_crc_recv = ...` 的 4 条、`int32 chk =`
拆成声明 + 赋值的 1 条，以及外层那句 `if(rt_len != 0u)`（`while(got < 0)` 天然不进循环，语义相同），
**没有语句丢失**。

这一笔的行为证据分两段，都是 QEMU 上的真读数（用一个 socket 版发送器驱动 `install` 窗口，
按设备 ACK 逐块发，脚本本身在 `build/` 里、不入库）：

1. **未签名镜像**（`build/APP_DEMO/APP_DEMO.svcapp`，28204 B = 256 头 + 1612 表 + 26336 负载）：
   表段 ACK、52 个负载块全部 ACK，设备随后报
   `LOADER:1242 signature mismatch: image rejected` / `INSTALL:266 rejected: err -18`。
   报的是签名而不是 CRC ⇒ **传输 CRC 已经比对通过**，也就是「收表 → 校验表 → 收负载 → 打补丁 →
   落盘 → 滚 CRC」这条链上每个字节都对。
2. **用本地开发密钥签过的同一镜像**：同上两段都 ACK，然后设备**静默挂住**（60 s 内串口零输出、
   周期任务的秒级打印也停）。把这笔改动 `git checkout` 回 HEAD 用同样的脚本重跑，**基线以完全相同
   的方式挂住** ⇒ 挂起与本次拆分无关，位置在签名门之后（回读复核 / 提交点那条路）。

**新欠账（本轮只登记，不明白根因）**：QEMU 上签名通过之后的那段路会挂死，HEAD 与拆分版一致；
真板上同一路径已装成过（见 [Ark RTOS应用安装与调试指南](Ark RTOS应用安装与调试指南.md)），所以更像是这台 QEMU 的 Flash 后端在「写满一整片 + 回读
+ 再写状态字」这一串上的问题，不能读成内核缺陷，也不能读成这次的拆分有问题。

读数：

```
before  text 367400 / data 440 / bss 767508   sha256 a5403823…   31,462,512 B
after   text 367384 / data 440 / bss 767508   sha256 660b94c9…   31,462,512 B
```

`text` **−16 B**，`data`/`bss` 与镜像总长不变；`arkrtos_loader.c` 的告警改动前后**同为 21 条**
（既有的指针宽度转换告警，新增代码零新增告警）。`shell_check.py` `PASS`。`doc_audit.py funcs`：
超过 120 行的函数 **27 → 26**，最长换成 `arkrtos_flashapp_write()`（211 行）。

#### 5.30.13 第八笔：`arkrtos_flashapp_write()` 按「闸门 / 擦 / 写 / 回读 / 元数据」分开（2026-11-18）

`kernelsrc/src/arkrtos_flashapp.c` 的 `arkrtos_flashapp_write()`（211 行）是「产物烧进片内 Flash
应用区」的写路径，原本五步全挤在一个函数里、共用 9 处提前返回。这一笔把每一步抽成一个 `static`，
主函数只剩闸门、调度锁与错误码收口：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `flashapp_busy_claim()` | 取单实例闸门：读状态字，有人占用就 `ERR_BUSY`，没有就写 `0xFFFFFFFF` 占位。**故意不走 `arkrtos_flashapp_task_id()`**——那支内部取同一把锁，在持锁上下文里自死锁 | 15 |
| `flashapp_busy_release()` | 清状态字。任何返回路径都要走到 | 8 |
| `flashapp_program()` | 第三步：开文件、按 `FLASHAPP_IO_CHUNK` 分块读、写 Flash 应用区，块内偏移单调推进（H7 flash word 是读-改-写，同一 32 字节块只能编程一次） | 44 |
| `flashapp_verify()` | 第四步：重开文件、回读 Flash 逐字节比对。写接口返回 0 只说明「下发成功」 | 46 |
| `flashapp_write_meta()` | 第五步：区尾 32 字节元数据（魔数/长度/CRC/版本）写入 + 回读。放最后是掉电语义：元数据缺失 ⇒ 判「没有产物」 | 33 |
| `arkrtos_flashapp_write()` 自己 | 参数与 VFS 就绪检查、占闸门、跑 scan + check_shape、持调度锁、串四步、`out_unlock` 统一放锁、`out_busy` 统一放闸门 | **93**（原 211） |

等价性用归一化行多重集比对（剥注释、逐行去空白、比 Counter）：only-OLD 是 9 条 `goto out_busy`、
9 条内联的 `arkrtos_sched_unlock_internal()`、以及被搬进 helper 后变成 `return` 的那些
`ret = ARKRTOS_FLASHAPP_ERR_FLASH / ARKRTOS_MINI_ERR_FS / ARKRTOS_FLASHAPP_ERR_VERIFY`；
only-NEW 是 5 个新函数签名、`goto out_unlock`、`out_unlock:` 标签、`ret = flashapp_*(...)` 调用与
`return` 语句。**没有语句丢失**：每一步的分支条件、返回码、错误码一条未改；`*p_len` 与成功日志
仍在放锁之后、且只在 `ret == 0` 时执行。

编译口径（这一笔没有可跑的行为证据，如实写清）：

* **LA64 QEMU 默认档根本不编这个 TU**——`example/qemu_loongarch64/Makefile` 用 `filter-out`
  把它剔掉了（理由是这套目标没有 Flash 控制器）。所以「默认档逐字节不变」在这里是**因为没编**，
  不是因为编了以后一样：`text 367384 / sha256 660b94c9… / 31,462,512 B` 与上一笔逐字节相同。
* **STM32H743 侧默认 `ARKRTOS_USE_FLASHAPP=0`**，整个 TU 被 `#if` 包成空目标：armclang
  `-c` 出来的目标 `Code/RO/RW/ZI` 全为 0（`fromelf -z` 实测）。
* 强制打开（`-DARKRTOS_USE_FLASHAPP=1 -DARKRTOS_USE_APPRUN=1 -DARKRTOS_USE_VFS=1`，H743 配置）
  后 `armclang -fsyntax-only` **exit=0**，告警 **2 条**（`code_base` / `code_size` set but not used，
  第 558 行的 `arkrtos_flashapp_run()` 里，改动前后同数同位置）。目标体积 `Code 3822 → 3818`
  （−4 B）、`RO Data 423 → 447`（+24 B）、`RW/ZI` 不变。

**结论分档：仅编译通过，未上板。** 这台 QEMU 目标没有 Flash 控制器、`flashapp` 也不被 LA64
Makefile 编译；H743 真板本轮没有跑这条路径。不得把本笔读成「烧写路径已验证」。

读数：

```
默认档（LA64）  text 367384 / data 440 / bss 767508   sha256 660b94c9…   31,462,512 B   （与上一笔逐字节相同）
H743 ON 目标    Code 3818 / RO 447 / RW 12 / ZI 1064   （基线 Code 3822 / RO 423）
H743 OFF 目标   Code 0 / RO 0 / RW 0 / ZI 0            （空 TU）
```

`shell_check.py` `PASS`（LA64 默认档不受影响）。`doc_audit.py funcs`：超过 120 行的函数
**26 → 25**，最长换成 `arkrtos_layout_validate()`（199 行）。

#### 5.30.14 第九笔：`arkrtos_layout_validate()` 按「头部 / 槽位 / 重叠 / 参数」分开（2026-11-18）

`kernelsrc/src/arkrtos_layout.c` 的 `arkrtos_layout_validate()`（199 行）是配置记录唯一的总校验：
一条记录有 7 条整体属性、每个槽位 6~9 条字段要求、槽位之间的两两不重叠、以及尾部的运行期参数与
「本内核还没接的项」。这一笔按这四类拆成 `static`，主函数只剩遍历与模式一致性：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `layout_check_header()` | 整条记录的属性：指针非空、魔数、版本、CRC（`arkrtos_layout_crc()`）、硬件兼容号、模式合法、`slot_count` 上限 | 33 |
| `layout_check_slot()` | 单个槽的字段：类型、`flags`/`reserved` 必须为 0、Flash 区间落在镜像池内、最小尺寸、分配粒度对齐；FIXED 模式再要求整扇区对齐与 RAM 窗口钉死；RAM 块要求 2 的幂、上下限、基址对齐与窗口内 | 66 |
| `layout_check_overlap()` | 与下标更小的已检查槽两两比：Flash 区间不重叠，RAM 窗口（若都固定）不重叠 | 29 |
| `layout_check_knobs()` | 尾部参数：回收力度、日志级别、崩溃重启上限、`flags` 只认 RAW_ALLOW 一位、`APP_ALLOW_RAW_IMAGE != 1` 时拒绝 RAW_ALLOW、挂接窗口上限、未接项（看门狗/堆/默认栈）必须为 0、保留区必须全 0 | 59 |
| `arkrtos_layout_validate()` 自己 | 头部校验、逐槽遍历（跳过未用槽并数 `used`）、模式与槽表一致性（FIXED 至少一个槽、AUTO 不许带槽表）、最后交给 `layout_check_knobs()` | **53**（原 199） |

等价性用归一化行多重集比对：**only-OLD 为空**，only-NEW 只有 5 个函数签名、`{`/`}`、`rc` 变量与
`rc = layout_check_*(...)` 调用、`if(rc != ARKRTOS_CFG_OK)` / `return rc` 这三对，以及 3 个
`return ARKRTOS_CFG_OK`。**一条语句都没被改写或丢失**：每个判断的条件、返回的错误码、注释原位
未动，只是被搬进对应阶段。

这一笔是**默认档真实生效的代码**（LA64 构建会编这个 TU），所以体积变化必须如实报，不能声称
「逐字节不变」：

```
before  text 367384 / data 440 / bss 767508   sha256 660b94c9…   31,462,512 B
after   text 367480 / data 440 / bss 767508   sha256 666ef74f…   31,462,512 B
```

`text` **+96 B**（4 个新函数的调用开销与入口），`data`/`bss` 与镜像总长不变；`arkrtos_layout.c`
的告警改动前后**同为 3 条**（既有的指针宽度转换告警）。`shell_check.py` `PASS`（这次是真跑到了
这条校验：默认档的布局走的就是它）。`doc_audit.py funcs`：超过 120 行的函数 **25 → 24**，最长换成
`arkrtos_apprun_run_ex()`（195 行）。

#### 5.30.15 第十笔：`arkrtos_apprun_run_ex()` 按「闸门 / 借窗 / 核向量 / 起任务」分开（2026-11-18）

`kernelsrc/src/arkrtos_apprun.c` 的 `arkrtos_apprun_run_ex()`（195 行）是裸镜像运行的唯一入口：
单实例闸门、VFS 前置、借窗口、读文件、核向量表、起任务、发布全局状态。这一笔把其中四段搬成
`static`，主函数只剩「按顺序调用 + 失败清理」：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `apprun_gate_claim()` | 单实例闸门：`g_apprun_task != 0` 报 `ERR_BUSY`，否则占成 `0xFFFFFFFF`（装载中）。成功时调用方必须配一次 `apprun_gate_release()` | 15 |
| `apprun_gate_release()` | 把 `g_apprun_task` 清回 0；所有失败路径都要走到这里 | 8 |
| `apprun_borrow_window()` | 走 `arkrtos_ptable_mini_alloc()` 借 16 KB 代码 + 数据窗口，**逐字段核对**四个基址/尺寸常量是否与 `ARKRTOS_APPRUN_*` 完全相等（裸镜像是按固定窗口链出来的，换地址等于让它跳进虚空）；对不上当场 `mini_free` 并报 `ERR_WINDOW`。出参四个 + `pt_index` | 42 |
| `apprun_check_vectors()` | 读镜像自带向量表：`+0` 初始 SP 必须正好等于数据窗口顶端、`+4` 入口必须落在代码窗口内（且 `+2` 不越界），再核入口首半字是 arkc 序言 `0xB5E0`；分别报 `ERR_LAYOUT` / `ERR_PROLOGUE`，成功时出参 `p_entry` | 28 |
| `apprun_start_task()` | 注册任务（栈取数据窗口末尾 4 KB）、填 TCB 两个窗口、`arkrtos_mpu_build_task()`、`arkrtos_ptable_set_slot()` 补入口；失败返回 `ARKRTOS_LOADER_ERR_TASK`（由调用方还窗） | 38 |
| `arkrtos_apprun_run_ex()` 自己 | 参数检查 → 闸门 → VFS 就绪 → 调度锁 → 借窗/读文件/核向量三段调用与逐段清理 → 放锁 → 起任务 → 发布 `g_apprun_pt/entry/task` 与 `*task_out` → 日志 → `sched_activate_higher()` → `release_slot:` 统一放闸门 | **92**（原 195） |

等价性用归一化行多重集比对，only-OLD 15 行 12 类，逐条都是拆分内的形态改写：

- `uint32 entry;` → `uint32 entry = 0u;`（声明与初始化合并）；
- 2× `goto release_slot;`（借窗块内两条失败路径）→ 改成 helper 直接 `return`，主函数统一一处 `if(ret != 0)` 清理；
- 2× `arkrtos_sched_unlock_internal();`（借窗块内的 slot<0 与字段不符两条）→ 主函数在借窗失败分支解锁一次；
- 2× `ret = ARKRTOS_APPRUN_ERR_WINDOW;` / 1× `ERR_LAYOUT` / 1× `ERR_PROLOGUE` → helper 内 `return ARKRTOS_APPRUN_ERR_*`；
- 1× `(void)arkrtos_ptable_mini_free(pt_index);` —— 基线的**序言校验失败**分支自己还一次窗，现在它与 `ERR_LAYOUT` 共用主函数同一处 `if(ret != 0)` 清理，**仍是一次失败一次归还，没有漏还也没有双还**；
- `code_base = c_base;` / `code_size` / `ram_base` / `ram_size` 四行 → helper 出参 `*p_code_base` 等；`entry = vec;` → `*p_entry = vec;`。

only-NEW 53 行全是新签名（含续行）、`{`/`}`、出参赋值、`return X;`、`if(ret != 0)`、四处
`ret = apprun_*(...)` 调用与 `apprun_gate_release();`。**没有判断条件被改写、没有返回码被换、
没有注释被删。**

这一笔是默认档真实生效的代码（LA64 构建会编 `arkrtos_apprun.c`），体积读数：

```
before（第九笔之后） text 367480 / data 440 / bss 767508   sha256 666ef74f…   31,462,512 B
after               text 367480 / data 440 / bss 767508   sha256 666ef74f…   31,462,512 B
```

**逐字节相同**——五个新函数都是 `static` 且只有一个调用点，`-O2` 把它们整个内联回主函数，
最终机器码与拆分前一模一样。这是「拆分不改行为」比体积数字更硬的证据（`arkrtos_apprun.c`
告警前后同为 0 条；把拆分前的副本按同一组 flags 编译，`-c` 出来的目标文件也相同）。
`shell_check.py` `PASS`（`build/shell_check_apprun.log`）。`doc_audit.py funcs`：超过 120 行的
函数 **24 → 23**，最长换成 `svc_fs_volume()`（191 行）。

**结论分档：QEMU 实跑验证过（默认档逐字节不变 + 端到端 PASS），未上板。**

#### 5.30.16 第十一笔：`svc_fs_volume()` 按「内容读写 / 元数据 / 命名空间编辑 / 挂载对」分开（2026-11-18）

`kernelsrc/src/arkrtos_task.c` 的 `svc_fs_volume()`（191 行）是 `ARKRTOS_SVC_FILE_SYS` 子命令
1..10 的服务端实现：全是「参数再核一遍 → 加调度锁 → 调 `arkrtos_fs_*` → 放锁 → 回一个值」的
同一副骨架，只是载体不同。这一笔按参数形状把它分成四组，主函数只剩分发：

| 函数 | 管哪些子命令 | 行数 |
|---|---|---|
| `svc_fs_vol_io()` | 1 WRITE / 2 READ / 7 READ_AT——都是「路径 + 调用方缓冲区」的内容读写，读类在 `fs_ret == 0` 时把 `got` 当返回值 | 77 |
| `svc_fs_vol_meta()` | 4 STAT / 10 INFO——调用方交进来的是 4 字节出参，服务端不读用户内存 | 41 |
| `svc_fs_vol_tree()` | 3 REMOVE / 8 RENAME / 9 LIST——命名空间上的改动与枚举 | 61 |
| `svc_fs_vol_mount()` | 5 MOUNT / 6 UNMOUNT——默认卷这一对（MOUNT 故意幂等，UNMOUNT 严格） | 30 |
| `svc_fs_volume()` 自己 | 十个 case 归成四组 dispatch，末尾 `default` 回 `ARKRTOS_VFS_EINVAL` | **31**（原 191） |

等价性用归一化行多重集比对：**only-OLD 为空**——一个 case 体、一条参数检查、一次加锁/放锁、
一个返回值都没被改写或丢失，十个 case 体是整段原样搬进各自的 helper（缩进都没变，因为 helper
里的 `switch` 与原函数同一层）。only-NEW 45 行全是结构性新增：4 个函数签名、4 个 `switch(p[0])`、
8 个 `{`/`}`、3 个 `int32 fs_ret;`、10 个 case 标号（入口那 10 个标号的注释被去掉后与原标号
逐字相同，计入抵消）、4 处 dispatch 调用与 4 个 `break;`。

这一笔是默认档真实生效的代码，且**真的被 `shell_check.py` 走到**（`ls` / `cat` 走的就是这套
VFS SVC）。体积：

```
before（第十笔之后） text 367480 / data 440 / bss 767508   sha256 666ef74f…   31,462,512 B
after              text 367528 / data 440 / bss 767508   sha256 f33290de…   31,462,512 B
```

`text` **+48 B**（四次额外调用与四个新函数入口的常数开销；内层 case 体字节不变所以远小于
拆出的源码行数），`data`/`bss` 与镜像总长不变；`arkrtos_task.c` 告警前后**同为 147 条**，
全部是该文件既有的 `-Wpointer-to-int-cast` / `-Wint-to-pointer-cast`（这个文件在搬用户指针，
与本次拆分无关）。`shell_check.py` `PASS`（`build/shell_check_fsvol.log`）。`doc_audit.py funcs`：
超过 120 行的函数 **23 → 22**，最长换成 `apppack_scan()`（185 行）。

**结论分档：QEMU 实跑验证过（默认档 + 端到端 PASS 且真走这条 SVC），未上板。**

#### 5.30.17 第十二笔：`apppack_scan()` 按「MOVW 对 / 整字 / 同指令 / 改写对」分出四步（2026-11-18）

`kernelsrc/src/arkrtos_apppack.c` 的 `apppack_scan()`（185 行）是四遍差分打包里「扫一遍」的核心：
它按 Thumb 指令长度走字节，判断每个位置是 ROM/RAM 整字表项、还是被链接期改写的 MOVW/MOVT 立即数对。
四个判断按顺序有硬性先后（MOVW 对必须排在整字判之前，否则 MOVT 的高半字会被当成 `+delta` 的 ROM
数据字），所以这一笔**不改顺序**，只把每一步整体搬成一个「试一次」的 helper：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `apppack_scan_movw_first()` | 第 0 步：先按 32 位 MOVW/MOVT 立即数对判，三份里真的变了才认，还要求 `pos` 是 2 的倍数；命中就写表项、加计数、前进 4 字节 | 66 |
| `apppack_scan_word()` | 第 1 步：4 字节纯数据字（常量池指针/常量表），命中后还要求 4 字节对齐（装载器的表检查会拒非对齐项），对齐不了就交回给下一步 | 62 |
| `apppack_scan_same_insn()` | 第 2 步：三份的半字相同 ⇒ 按指令长度（`apppack_is32()` 判 2/4 字节）跳过 | 26 |
| `apppack_scan_movw_diff()` | 第 3 步：指令本身不同 ⇒ 只能是 MOVW/MOVT 被改写；没有「不归它管」的出口，要么消费一步要么报 `ERR_DIFF` | 59 |
| `apppack_scan()` 自己 | `while(pos < len)` 里依次调四步，`<0` 上抛错误、`!=0` 表示已消费一步即 `continue` | **29**（原 185） |

四个 helper 的返回约定统一成三态：`1` = 消费了一步（出参 `p_n` / `p_pos` 已推进，表项与计数已写）、
`0` = 不属于这一类、负值 = 差分失败。为了让 case 体逐字不动，helper 里用 `pos` / `n` 的局部副本干活，
命中路径末尾写回 `*p_n` / `*p_pos`。

等价性归一化比对：only-OLD 7 行 4 类 —— 2 处 `pos += ARKRTOS_APP_RELOC_MOV_SIZE;`、1 处 `pos += 4u;`、
1 处 `pos += adv;`、3 处 `continue;`；它们在 only-NEW 里逐条对应成 4 处 `*p_pos = pos + ...;` 与
4 处 `return 1;`。**没有判断条件被改写、没有表项编码被改、没有计数被改、没有错误码被换**（四步的
顺序在主干里一字未动）。

体积：默认档 `ARKRTOS_USE_APPPACK=0` 时这个 TU 被包成空目标，所以默认档**逐字节不变** ——
text 367528 / sha256 `f33290de…` / 31,462,512 B。开关强制打开（`build/_apppack_chk` 的板级头
shim，`-DARKRTOS_USE_APPPACK=1`）后 `-c` 编译 exit=0、**告警 0 条与基线同数**，`text` 3851 → 3947
（**+96 B**，四个 helper 全被内联，`nm` 里看不到它们的符号）。`shell_check.py` `PASS`
（`build/shell_check_apppack.log`，默认档不走这条路径，所以它证明的是「没被弄坏」，不是「这段新
代码被跑到」）。`doc_audit.py funcs`：超过 120 行的函数 **22 → 21**，最长换成
`arkrtos_loader_load_buffer()`（176 行）。

**结论分档：编译通过（开关打开档，类型与内联都验过）+ 默认档逐字节不变 + 端到端 PASS；这条扫描
路径本身没有在板上或 QEMU 上真跑过（默认档不打包），未上板。**

#### 5.30.18 第十三笔：`arkrtos_loader_load_buffer()` 按「验镜像 / 落槽 / 提交」分三段（2026-11-18）

`kernelsrc/src/arkrtos_loader.c` 的 `arkrtos_loader_load_buffer()`（176 行）是「从内存缓冲区装一个应用」
的入口：校验 → 落槽（预留 + 写头 + 写重定位表 + 分块写 Flash）→ 回读复核并把状态字改成 `VALID`。
三段的**前置契约**本来就不同（第一段只读、第二段持有一槽、第三段把槽提交成可用），所以按这三段抽三个
`static` helper，而不是按语句切碎：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `loader_verify_buffer()` | 参数检查、给出 `p_src`、`arkrtos_loader_check_header(... TYPE_MASK_POOL)`、算总长、整镜像 CRC、签名门；成功时用出参把 `p_src` / `total` 交给后两段 | 49 |
| `loader_program_slot()` | `reserve_fixed` 拿槽 → 算 `delta_rom` / `delta_ram` → 写 UNCOMMITTED 头 → 头 CRC → 写重定位表并入 CRC → 负载分块搬运、按表打补丁、写 Flash；**每条失败路径自己 `ptable_free` 再返回**；成功写回 `slot` / `base` / `hdr` / `crc_written` | 119 |
| `loader_commit_slot()` | 回读复核、`p_hdr->state = VALID` 落盘、`slot_crash_cnt[slot] = 0`、`arkrtos_crash_forget`、`ptable_set_slot` | 29 |
| `arkrtos_loader_load_buffer()` 自己 | 三次调用 + 逐段短路；失败就地 `return`，没有跨段 `goto` | **33**（原 176） |

第二段仍然偏长（119 行），这是刻意的：它本来就是「一槽之内的一条直线」，每一步失败都要先还槽再返回；
再切碎会把「谁欠谁一次 free」的账拆到两个函数里，那才是真正的漏洞来源。**按段切，不按语句切。**

等价性归一化比对：only-OLD 只剩 **4 行**，全是同一次改名 —— `hdr.state = ARKRTOS_APP_STATE_VALID;`、
`(const uint8 *)&hdr.state, 4u) != 0)`、`arkrtos_loader_entry_addr(base, hdr.payload_offset, hdr.entry_offset),`、
`if(arkrtos_loader_image_crc((const uint8 *)base, &hdr) != crc_written)`：原函数用栈上 `hdr`，拆开后三段
必须共用同一份头，于是头由入口持有、后两段收 `arkrtos_app_header_t *p_hdr`。only-NEW 51 行全是签名、
花括号、四个出参写回、三处 `ret = loader_*(...); if(ret != 0){ return ret; }`。**没有判断条件、错误码、
CRC 口径、槽位序号被改。**

体积：`arkrtos_loader.c` 在 LA64 默认档**真的被编进去**，所以这一笔**不是**「逐字节不变」——
text `367528 → 367576`（**+48 B**），`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 不变；
`BUILD=build/loadbuf_split` 与重建后的默认档目录两份 `.bin` 在 `SOURCE_DATE_EPOCH=1600000000` 下
**sha256 逐字节相同**（`eb5abe42…`）。`arkrtos_loader.c` 单文件告警 **21 条与基线同数**（同一组 flag 对
拆分前副本与现文件各 `-c` 一次对照；该文件既有的 `-Wint-to-pointer-cast` 与本次拆分无关）。

行为证据比前几笔强，因为它有真运行读数：`utest` 用例 `sign_hmac_and_loader_gate` 里
`arkrtos_loader_load_buffer()` 被调两次 —— 第一次签名错 ⇒ 串口见
`LOADER:452 signature mismatch: image rejected`（第一段的分支）；第二次把签名改回来，而用例只断言
「不是 `ERR_SIGN`」，停在哪一段从汇总行看不出来。于是在本文件里临时插一行探针（`loader_commit_slot`
成功之后打印槽号与 base），单独构建 `build/_probe` 跑一次 `utest`，得到
`LOADER:2022 probe: load_buffer committed slot 0 base 0x00260000` ⇒ **三段全部走通、真装成了一槽**
（这条读数取自 2026-11-17 的布局；2026-11-20 镜像池基址改为 `0x002A0000`，槽 0 随之移动，机制不变）。
探针随后整文件回滚（回滚后 `probe:` 计数 0、行尾仍是 `crlf 3483 / lf 3483`），提交里不含探针。
`shell_check.py` `PASS`（`build/shell_check_loadbuf.log`，`13 matched, 13 passed, 133 assertions`）。

顺带记一条本轮踩到的坑：`shell_check.py` 的 ELF 路径写死 `build/qemu_loongarch64/`，在别的 `BUILD=`
目录构建完直接跑，它会测到**上一次的旧镜像**（本轮第一次跑就撞上：日志 PASS，可那个 ELF 比源码老一小时）。
所以体积读数取 `BUILD=build/loadbuf_split`，行为读数前先把新源码增量重编进默认目录再跑。

这一笔把两道门禁碰红了，两条都是真问题，按「先补证据、再改判据」处理，没有一条是靠调低门槛过去的：

1. `coverage-host`：拆出来的入口与参数检查行没有被宿主用例执行，`arkrtos_loader` 从 15.89% 掉到
   15.55%。**没有下调基线**，而是给 `kernelsrc/tests/test_loader_host.c` 加了一条
   `test_load_buffer_e2e()`：用一块 RAM 假 Flash（擦除 = 填 0xFF、写 = `memcpy`、回读 = 直接读那块
   内存）+ 一份固定槽位配置，把 verify → program → commit 三段真跑一遍。正向断言槽位被提交成
   `VALID`、`runtime_ram_base` 被固化、ROM 表项那个字变成「标称值 + delta」、其余负载逐字节落盘；
   反向三条分别断言：长度不足 ⇒ `PARAM` 且**一个槽都没占**、负载改坏一个字节 ⇒ `CRC` 且一个槽都没占、
   Flash 写失败 ⇒ `FLASH` 且**占一次还一次**。改完 `arkrtos_loader` 覆盖率 15.55% → **29.38%**，
   基线同步抬到 29.38（把这次的有效覆盖冻住，以后加代码不再白掉）；该用例在 `loader-host` 的同一组
   flag 下是 `75 passed, 0 failed`。
2. `sec-crc`：pin「load_buffer 把表体折进 crc_written」取的是 `load_buffer` 的函数体，表体折叠搬进
   `loader_program_slot()` 之后它变红。**约束本身没变**（先折表、再搬负载），所以把判据改成「同一
   文件内，折叠那一行出现在负载循环之前」，pin 改名 `装载路径把表体折进 crc_written`；`drop_table_crc`
   这个改坏方式仍能让它变红（`SEC-CRC-GATE OK`，8 pin 自检全过）。

`ci_gate.py` 复跑：**47 check(s) passed, 0 failed, 0 skipped**（`build/ci_gate_loadbuf2.log`）。

`doc_audit.py funcs`：超过 120 行的函数 **21 → 20**；最长 **并列 167 行** —— `arkrtos_mtx_lock_internal()`
与 `arkrtos_loader_move_slot()`。

**结论分档：QEMU 实跑验证过（端到端 PASS + utest 用例真调这个函数且实测提交成功），未上板。**

#### 5.30.19 第十四笔：`arkrtos_mtx_lock_internal()` 按「快路径 / 登记等待 / 阻塞判定」分三段（2026-11-18）

`kernelsrc/src/arkrtos_sync.c` 的 `arkrtos_mtx_lock_internal()`（167 行）是互斥锁加锁的唯一入口，
它把四种完全不同的处境写在同一个函数里：锁空着（可以就地拿）、自己已经持有（递归或报错）、
`timeout==0` 只试一次（不登记等待者）、真要排队等（继承优先级 + 入队 + 阻塞 + 判唤醒真假）。
这四段的前置状态不同，临界区（`ARKRTOS_DISABLE_IRQ()` 之后）却只有一份，所以按「拿到结论就开中断返回」
切成三段：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mtx_lock_try_immediate()` | owner 为空（含「队列头就是自己」则摘掉自己的登记再拿锁）、自己已是 owner 的递归加深 / 非递归报错（两个 `#if` 分支各一份）、`timeout==0` 的只试一次；返回 1 表示还要继续等，返回 0 表示 `*p_ret` 已是最终码 | 68 |
| `mtx_lock_register()` | 优先级继承（只有「持有者优先级低于等待者」才提）→ `waiters_add`（队列满则 `recalc_priority` 回滚）→ 用户态等待者只登记（`appq_set(p_tsk, 1u)` + `WOULDBLOCK`）；同样用 1/0 区分「继续」与「已是最终码」 | 31 |
| `mtx_lock_block_and_claim()` | `propagate()` → `trace_wait_obj()` → `task_block_in_critical()`，然后按 `reason` 分类：`<0` 参数错、`OBJ_DELETED` 删锁、`1` 超时与转交的竞态、以及「`owner` 不是自己或没带 HANDOFF 位」的假醒自检（`owner` 恰是自己时 `arkrtos_diag_sync_ghost++`）；恒为最终结论 | 65 |
| `arkrtos_mtx_lock_internal()` 自己 | 句柄校验 + 临界区入口 + `arkrtos_task_get_current()` 判上下文 + 三次调用 | **24**（原 167） |

口径就一条：**每个 helper 内自己开中断再 return**，入口不再补开 —— 一次失败一次开中断，不重不漏。
三个 helper 一律返回「是否已给出结论」而不是错误码，因为 `ARKRTOS_SYNC_OK` 本身就是 0，用返回值兼作
「继续」会和成功码撞车；所以结论走 `int32 *p_ret` 出参。

等价性归一化比对（剥注释、逐行去空白、取多重集差）：only-OLD 只剩 **6 行**，就是被搬进 helper 的六个
`return ARKRTOS_SYNC_ERR_* / OK;`；only-NEW 全是 `*p_ret = ...; return 0;` 六处、`return 1;` 两处、
`int32 ret;` 与入口那三次调用。**没有判断条件、错误码、优先级继承口径、等待者摘除时机被改。**

这一笔的对象同时是宿主用例的链接对象（`kernelsrc/tests/test_sync_host.c`，`-DARKRTOS_USE_MTX_RECURSIVE=1
-DARKRTOS_SYNC_HOST_TEST`），所以证据里必须带上它：`SYNC-HOST OK (85 checks)`，另用
`-DARKRTOS_USE_MTX_RECURSIVE=0` 单独编一次确认非递归那个 `#else` 分支也过编译。

体积：`arkrtos_sync.c` 在 LA64 默认档**真的被编进去**，所以这一笔**不是**「逐字节不变」 ——
text `367576 → 367672`（**+96 B**，多出来的部分是三个 helper 的栈帧与调用点），`data` 440 /
`bss` 767508 / 镜像总长 31,462,512 B 不变；`BUILD=build/mtx_split` 与重建后的默认档目录两份 `.bin`
在 `SOURCE_DATE_EPOCH=1600000000` 下 **sha256 逐字节相同**（`d3c34f6e…`）。单文件告警 **0 条**
（同一组 flag 对拆分前副本与现文件各 `-c` 一次对照）。

门禁：`coverage-host` 里 `arkrtos_sync` **49.05%**（基线 43.79，未下调）；`sec-crc` 8 pin 全过（这一笔
不动 CRC 路径）；`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py` **PASS**
（`build/shell_check_mtx.log`）。

`doc_audit.py funcs`：超过 120 行的函数 **20 → 19**；最长换成 `arkrtos_loader_move_slot()`（167）。

**结论分档：宿主用例实跑验证过（85 checks）+ QEMU 端到端 PASS，未上板。**

#### 5.30.20 第十五笔：`arkrtos_loader_move_slot()` 按「前置把关 / 真搬 / 收尾提交」分三段（2026-11-18）

`kernelsrc/src/arkrtos_loader.c` 的 `arkrtos_loader_move_slot()`（167 行）是池内搬移的唯一实现，
它同时做三件互不相干的事：先证明这个槽真的可以动（状态、只向下搬、头合法、目标干净），
再把字节真搬过去（写 UNCOMMITTED 头 → 表原样搬 → 负载读出旧补丁再叠加落点差 → 每段滚 CRC），
最后整段复核并单字提交 VALID、把槽位改指新落点。三段的前置不同、失败语义也不同，按段切成：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `loader_move_precheck()` | 槽状态必须是 LOADED、`new_base` 非 0 且严格小于旧落点、`check_header(TYPE_MASK_POOL)`、总长、池边界、目标 `span_erased()`；成功时交出旧落点、总长与一份**已置 UNCOMMITTED 的头副本** | 51 |
| `loader_move_program()` | 写 UNCOMMITTED 头 → `crc_header` → 重定位表原样搬（分块 + 滚 CRC + 落盘）→ 负载读出旧补丁再 `reloc_apply(..., delta, 0u)` → 每段滚 CRC；`*p_crc` 交出「本次写下去的落盘形态」CRC | 96 |
| `loader_move_finalize()` | `image_crc` 整段复核 → 单字写 `state = VALID` 提交 → `ptable_move` 改指新落点 → `ptable_set_slot` 重算入口 | 32 |
| `arkrtos_loader_move_slot()` 自己 | 三次调用 + 一次 `delta = new_base - old_base` | **25**（原 167） |

口径三条：① 旧落点 / 总长 / 头副本走**出参**传递，三段都不再重读 Flash 里的头 —— 头在本函数里已经被改写过，
重读只会读到「自己刚写坏的那份」；② 「决定搬」与「已经把 state 改成 UNCOMMITTED」合在 precheck 里完成，
不再是函数中部的一个副作用；③ `delta` 算一次传下去，避免两段各算一遍。

等价性归一化比对（剥注释、逐行去空白、取多重集差）：only-OLD **19 行全部是 `hdr.` → `p_hdr->` 改名**
（含 `hdr = *p_old;` / `hdr.state = UNCOMMITTED;` 两行搬进 precheck），only-NEW 全是签名、出参赋值、花括号与
三次调用。**没有判断条件、错误码、CRC 口径、提交顺序被改。**

体积：text `367672 → 367656`（**−16 B**），`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 不变；
`SOURCE_DATE_EPOCH=1600000000` 下 `BUILD=build/move_split` 与重建后的默认档两份 `.bin` **sha256 逐字节相同**
（`f7658a81…`）。单文件告警 **21 条 = 拆分前 21 条**（这个文件本来就有 21 条告警，这里只保证严格同数）。

宿主用例是这一笔真正的收获：`find_clean()` / `move_slot()` / `pool_free()` 都写死用 `IMAGE_POOL_BASE`
扫绝对地址，所以搬移路径在宿主上原本只能读到 0 行覆盖。新增的 `test_move_slot_e2e()` 先用 `VirtualAlloc`
（POSIX 分支用 `MAP_FIXED`）把 `0x08060000` 真映射出来，再用 RAM 假 Flash 跑一次真实回收：装载在
「池底 + 一个扇区」的镜像被压实到池底、头重新提交成 VALID、ROM 表项的整字补丁再叠一次
`(新落点 − 旧落点)`，旧位置整段回到擦除态，再回收一次不再移动。`loader-host: 88 passed, 0 failed`（原 75）。

两个坑都只在宿主上才现形：

1. **映射长度必须用 `IMAGE_POOL_SIZE`，不能用 `IMAGE_POOL_USABLE_SIZE`**。回收的逐扇区循环跑的是
   `pool_units`（= `IMAGE_POOL_SIZE` / 扇区）个扇区，末尾那个保留扇区在 USABLE 之外，但它照样会被
   `span_erased()` 读一遍 ⇒ 只映射 USABLE 就是读未映射地址，直接段错误 —— 不是"少测一点"。
2. **`arkrtos_loader_reclaim()` 返回的是「实际擦掉的扇区数」，不是状态码**。搬完之后旧副本所在的扇区
   已经没人占，逐扇区那一轮会把它擦掉，所以首次回收返回 **1**；第二次才是 0。

门禁：`coverage-host` 里 `arkrtos_loader` **45.25%**（原基线 29.38，**未下调**，按惯例用 `--update`
把基线抬到实测值；同一轮 `arkrtos_sync` 也由 43.79 抬到实测 49.05）；`sec-crc` 8 pin 全过；
`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py` **PASS**。

`doc_audit.py funcs`：超过 120 行的函数 **19 → 18**；最长换成 `arkrtos_guard_tick()`（166）。

**结论分档：宿主用例实跑验证过（88 checks）+ QEMU 端到端 PASS，未上板。**
搬移路径的运行时证据来自**宿主**（`VirtualAlloc` 把池映射到真实绝对地址 + RAM 假 Flash），不是 QEMU/真机；
映射失败时用例打印 `SKIP` 并返回，不假装跑过。

#### 5.30.21 第十六笔：`arkrtos_guard_tick()` 按「免费证据 / 契约判定 / 审计 / 记账」分四段（2026-11-18）

`kernelsrc/src/arkrtos_guard.c` 的 `arkrtos_guard_tick()`（166 行）每拍都在做，但每段节拍不同：
「谁跑过」每拍记、「契约违约」每服务拍算、「结构审计」每 `ARKRTOS_GUARD_AUDIT_DIV` 个服务拍跑一次、
「首次判坏时的崩溃记账」每次开机只跑一次。四段的触发条件不同，混在一个函数里读不出哪段在什么时候跑：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `guard_mark_running()` | 免费证据：本拍在 CPU 上的任务记一笔 `cpu_tick` / `task_id`（**不参与契约判定**，头文件写明） | 8 |
| `guard_count_violations()` | 逐个声明过周期的任务判违约：任务消失则契约随之注销（`declared--`），否则只看 beat 年龄，返回本拍违约个数 | 40 |
| `guard_structure_audit()` | `g_audit_div++` 后按 `ARKRTOS_GUARD_AUDIT_DIV` 取模跑一次 `arkrtos_audit_run()`，写回 `audit_mask` / `audit_runs` | 13 |
| `guard_starve_charge()` | 首次判坏时给违约镜像记一次崩溃（锁是 `g_status.starves`）：遍历、`ptable_find_task`、`crash_fault`、持有则打 `CRASH` 日志，再按 verdict 位记 `AUDITFAIL` / `GUARDHB` 两条 fault 并打 `GUARD` 日志 | 68 |
| `arkrtos_guard_tick()` 自己 | 取 tick → 免费证据 → 服务拍闸门 → 违约计数 → 审计 → 组装 `verdict` → feed 或 starve（starve 侧只在 `starves == 0` 时调 `guard_starve_charge()`） | **59**（原 166） |

口径三条：① 只有 `guard_count_violations()` 会写 `declared` / `misses`，也就只有它能让「契约集合」变小；
② 审计与契约互不依赖，谁先谁后不影响读数 —— 但顺序保持原样（契约先），因为 `verdict` 是两段结果的**合取**；
③ `guard_starve_charge()` 里那条 `GUARD` 日志原本打的是局部量 `violating`，拆段后局部量不在作用域内，
改成读 `g_status.violating`（入口在调用它之前已经写过），**数值相同、语义相同**。

等价性归一化比对（剥注释、逐行去空白、取多重集差）：only-OLD **只剩 1 行** —— `(uint32)violating);`，
正是上面第 ③ 条那处改名的对象；only-NEW 20 行全是函数签名、`int32 id;` / `uint32 violating;` 两个局部声明、
`return violating;` 与入口的四处调用。**判断条件、错误码、日志文案、记账时机一处未改。**

体积：text `367656 → 367752`（**+96 B**，四个 helper 的栈帧与调用点），`data` 440 / `bss` 767508 /
镜像总长 31,462,512 B 不变；`SOURCE_DATE_EPOCH=1600000000` 下 `BUILD=build/guard_split` 与重建后的默认档
两份 `.bin` **sha256 逐字节相同**（`783c394d…`）。`arkrtos_guard.c` 告警 **0 条**。

门禁：`coverage-host` 10 个模块全部不低于基线（本文件不在覆盖对象里，未改动基线）；`sec-crc` 8 pin 全过；
`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py` **PASS**（`build/shell_check_guard.log`）。

`doc_audit.py funcs`：超过 120 行的函数 **18 → 17**；最长换成 `arkrtos_mbuf_recv_internal()`（153）。

**结论分档：编译通过 + 告警同数 + 两份可复现构建 sha256 相同 + QEMU 端到端 PASS，未上板。**
这一笔不新增用例：`arkrtos_guard_tick()` 的每条分支都要真任务表 / 真看门狗 / 真崩溃计数，
宿主上只能靠大量桩，读数说明不了「看门狗该不该喂」；`utest` 与 `shell` 覆盖的是它的调用点而不是它的分支，
所以这里不把「跑过一遍没崩」写成「分支已验」。

#### 5.30.22 第十七笔：`arkrtos_mbuf_recv_internal()` 把「取整条 / 等整条」拆开，去掉第二份逐字重复的取走代码（2026-11-18）

`kernelsrc/src/arkrtos_sbuf.c` 的 `arkrtos_mbuf_recv_internal()`（153 行）里，「一条消息到齐了没有、
到齐就整条取走」这段代码**逐字出现了两次**：一次在进阻塞之前的即时尝试，一次在被 `ARKRTOS_WAKE_HANDOFF`
唤醒之后的重试。两份一模一样，改错一份不会有任何提示：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mbuf_recv_try_take()` | 一次「到齐了吗」判定：没到齐 `return 0`（中断仍关，交回调用方）；到齐就整条处理完——`buf` 放不下则整条丢弃并交出 `TOO_SMALL`，否则跳头取载荷、`wake_one`、trace、开中断、切任务、交出长度 | 46 |
| `mbuf_recv_wait()` | 已确认没到齐且调用方愿意等：每轮 `waiter_add` → 中断上下文检测 → 剩余时间 → `trace_wait_obj` → `task_block_in_critical`；没带 `WAKE_HANDOFF` 按 `reason` 返回参数错 / 超时，带了就调 `try_take()` 重试，取不到继续下一轮 | 52 |
| `arkrtos_mbuf_recv_internal()` 自己 | 参数与句柄校验 → 关中断 → `try_take()` 即时尝试 → `timeout_ms == 0` 立刻超时 → `get_current` 判上下文 → 算 `start_tick` / `deadline` → `mbuf_recv_wait()` | **47**（原 153） |

口径三条：① `mlen` 判定顺序与阈值一字未改 —— helper 里写的 `(mlen <= 0) || (p_sb->count < …)` 就是原来
`(mlen > 0) && (p_sb->count >= …)` 的取反，没有新条件；② 「放不下就整条丢掉」留在 helper 里而不是把错误码
抛给调用方去丢 —— 丢掉是「取走」的一部分，必须与 `wake_one` / 开中断 / 切任务原子地一起做完；
③ 每条返回路径仍自己 `ARKRTOS_ENABLE_IRQ()`，唯一不开中断的出口是 `try_take()` 返回 0，因为调用方还在
临界区里继续等。

等价性归一化比对（剥注释、逐行去空白、取多重集差）：only-OLD **26 行全部可归到两件事**——
第二份逐字重复的取整条代码（丢弃路径与正常路径各一份，共 19 行），以及 `return ret_len;` /
`return ARKRTOS_SBUF_ERR_TOO_SMALL;` 改成「写 `*p_ret` + `return 1;`」的形态改写；
only-NEW 20 行全是两个签名、`return 0;`、`*p_ret = …; return 1;`、`mbuf_recv_wait()` 的循环与入口三处调用。
**判断条件、错误码、trace 调用、开中断与切任务的时机一处未改。**

体积：text `367752 → 367752`（**±0 B**），`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 不变；
`SOURCE_DATE_EPOCH=1600000000` 下 `BUILD=build/sbuf_split` 与重建后的默认档两份 `.bin` **sha256 逐字节相同**
（`783c394d0acfa98e694bcec1effecffb19fe34a61aa3a738c75ccce24330a0cd`），且与拆分前那一笔的镜像**也是同一个 sha256**
—— 编译器本来就把第二份重复代码合并了，所以这一笔的收益在可读性而不是体积。`arkrtos_sbuf.c` 告警 **0 条**。

门禁：`coverage-host` 里本文件 **44.85% → 46.69%**（重复的第二份代码在宿主流上走不到，去掉它等于把分母里
永远不执行的整段删掉，比值自然上升；按惯例 `--update` 把基线抬到实测值 **46.69**，**未下调**），其余 9 个模块
均不低于基线；`sec-crc` 8 pin 全过；`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py`
**PASS**（`mbuf` 收发在 shell 的 `utest` 里被真跑过）。

`doc_audit.py funcs`：超过 120 行的函数 **17 → 16**；最长换成 `audit_partition()`（152）。

**结论分档：编译通过 + 告警同数 + 两份可复现构建 sha256 相同且与拆分前逐字节相同 + QEMU 端到端 PASS，未上板。**
这一笔不新增用例：`mbuf_recv_wait()` 的每条分支都要真任务表、真阻塞唤醒与真超时推进，宿主上只能堆桩；
`utest` 与 `shell` 跑到的是它的调用点，不是分支。

#### 5.30.23 第十八笔：`audit_partition()` 按「身份+Flash / 池几何 / 配置区 / RAM / 崩溃日志 / 镜像池」分六组（2026-11-18）

`kernelsrc/src/arkrtos_audit.c` 的 `audit_partition()`（152 行）是一条**平铺的断言清单**：每一组
都在查同一张分区表的另一种不变量，全部用同一个 `audit_fail(p, ARKRTOS_AUDIT_PARTITION, INDEX_NONE)`。
平铺看不出「哪些组互不相关」也看不出「哪一组之后必须停」：

| 函数 | 查什么 | 行数 |
|---|---|---|
| `audit_part_basic()` | `p->checks++`；身份三件（magic / version / `hw_compat_id`）不符就**返回 0** 让入口收工（表都不是这张表，后面的字段读了没意义）；Flash 里 kernel 与 pool 各自存在且在片内、两者不重叠。返回 1 表示可以继续查 | 31 |
| `audit_part_pool()` | 池几何：扇区对齐、`pool_units` 与 `size/sector` 自洽、`reserve <= units`、`usable_size` 与 reserve 恰好扣一次、`alloc_unit` 是 2 的幂且落在 `[4, sector]` | 26 |
| `audit_part_config()` | 配置区：要么整段缺席（两个字段都是 0），要么是片内的真区间且不与 kernel / pool 重叠 | 20 |
| `audit_part_ram()` | RAM 三区（share / kernel / image）各自非空、在片内、两两不重叠 | 29 |
| `audit_part_crash()` | 崩溃日志几何（落在 share RAM 尾部且 4 字节对齐）；`ARKRTOS_USE_CRASH_LOG` 打开时再比一次「宏 vs `arkrtos_crash_area_base()/size()` 与 journal 大小」 | 29 |
| `audit_part_image()` | 镜像池形状：min/max 块都是 2 的幂、min ≤ max、总长与基址都按 min 块对齐 | 12 |
| `audit_partition()` 自己 | 取表 → 空表即报 → 算 `flash_end`/`ram_end` → `basic()` 不通就收工 → 其余五组按原顺序调用 | **25**（原 152） |

口径三条：① **唯一会改变控制流的是身份组**（原代码在这里 `return`，其余组失败只是累积 `mask`），
所以只有 `basic()` 有返回值，`0` = 别再读了；② 六组之间**没有共享可变状态**，都只读表、只写
`p->mask/index/checks`，顺序除了「身份先于一切」之外与原代码逐条一致；③ `p->checks++` 仍只在
`basic()` 与 `crash()` 的 `#if` 内各一次 —— 原代码的两处自增搬到了各自的新位置，**没有多也没有少**
（`checks` 是「真的比对过几条」的分子，位置不能随手挪）。

等价性归一化比对：**only-OLD 为空**（0 行）—— 原函数里每一条语句都原样活在新位置，连缩进都没动；
only-NEW 37 行全是六个签名、七对花括号、`return 0;` / `return 1;` 与入口的六次调用。
这是本轮拆到现在第一笔**一个语句都没丢、也没有改名**的。

体积：text `367752 → 367752`（**±0 B**，六个 helper 都单调用点，`-O2` 全部内联回去），`data` 440 /
`bss` 767508 / 镜像总长 31,462,512 B 不变；`SOURCE_DATE_EPOCH=1600000000` 下 `BUILD=build/audit_split`
与重建后的默认档两份 `.bin` **sha256 逐字节相同**（`ae8aee71…`）。`arkrtos_audit.c` 告警 **0 条**。

门禁：`coverage-host` 10 个模块全部不低于基线（本文件不在覆盖对象里，未动基线）；`sec-crc` 8 pin 全过；
`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py` **PASS**。

`doc_audit.py funcs`：超过 120 行的函数 **16 → 15**；最长换成 `arkrtos_loader_reserve_fixed()`（148）。

**结论分档：编译通过 + 告警同数 + 两份可复现构建 sha256 相同 + only-OLD 为空 + QEMU 端到端 PASS，未上板。**
这一笔不新增用例：六组判据全部吃真实分区表（`arkrtos_ptable_get()` 的全局表 + 板级宏），宿主上要伪造
整张表才能让某一条断言变红，而伪造出来的「红」只证明断言写对了、不证明真实布局对。

#### 5.30.24 第十九笔：`arkrtos_loader_reserve_fixed()` 按「窗口 / 冲突 / 摘旧 / 登记」分四段（2026-11-18）

`kernelsrc/src/arkrtos_loader.c` 的 `arkrtos_loader_reserve_fixed()`（148 行）在循环里连做四件事：
算 RAM 窗口、判这个槽能不能装、判槽里有没有正在跑的镜像、摘旧记录再登记并清落点。四种失败各有各的
返回码，四种理由混在一个 for 体里：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `loader_fixed_ram_window()` | 把镜像头声明的 `ram_size` 向上取整到 2 的幂（与开机扫描同一口径，配置登记的是上界不是窗口），超过 `SLOT_RAM_MAX_BLOCK` 返回 `ERR_SIZE` | 17 |
| `loader_fixed_slot_busy()` | 遍历分区表，与本槽有交集且状态是 `RUNNING` 的镜像 ⇒ `ERR_BUSY`（擦写会让它当场跑飞）；其它状态都可以覆盖 | 23 |
| `loader_fixed_slot_detach()` | 覆盖安装：把落在本槽内的旧记录逐条 `ptable_free`，保证表里的区间等于配置里的槽，不多不少 | 18 |
| `loader_fixed_claim()` | `ptable_alloc` 整槽 → `ram_bind` → `clear_span` 清落点；清失败打 `LOADER` 日志、把槽还回去、返回 `ERR_FLASH`；成功用出参交出 `base` / `ram_base`，返回值是槽号 | 24 |
| `arkrtos_loader_reserve_fixed()` 自己 | 取表 / 取配置槽表 → `want` → 算窗口（失败即 `ERR_SIZE`）→ 消费一次性 hint → 逐槽三步筛（类型、hint、装得下）→ 冲突 → 摘旧 → 登记 | **64**（原 148） |

口径三条：① **窗口先算一次**，循环里只比较 —— 原代码在 for 体之前算，语义不变，但「算不出来就整段不成立」
这一点现在由一个函数的返回值表达；② `ERR_BUSY` 与 `ERR_FLASH` 仍各自对应「有镜像在跑」与「清不干净」，
没合并成通用失败；③ 一次性 hint 仍在**任何取槽动作之前**消费掉（即使窗口算失败也已经消费）——
这是原代码的顺序，未调整。

等价性归一化比对：only-OLD **4 行** —— `int32 slot;` 声明与 `slot = ptable_alloc(...)` 合并成
`int32 slot = arkrtos_ptable_alloc(...)`、`ram_bind` 与 `*p_ram_base` 的实参由 `slots[i].ram_base`
改名成形参 `ram_base`；only-NEW 35 行全是四个签名、`uint32 j;` / `uint32 ram_size;`、`return 0;` /
`return ERR_*`、花括号与入口的四处调用。**判断条件、返回码、日志文案一处未改。**

体积：text `367752 → 367752`（**±0 B**，四个 helper 全内联），`data` 440 / `bss` 767508 /
镜像总长 31,462,512 B 不变；`SOURCE_DATE_EPOCH=1600000000` 下 `BUILD=build/reserve_split` 与重建后的
默认档两份 `.bin` **sha256 逐字节相同**（`63796b55…`）。`arkrtos_loader.c` 告警 **21 条 = 拆分前 21 条**
（全是既有的指针宽度转换）。

门禁：`loader-host: 88 passed, 0 failed`（本文件同时是宿主用例的链接对象）；`coverage-host` 里
`arkrtos_loader` **45.25% → 45.77%**（基线按惯例抬到实测，未下调）；`sec-crc` 8 pin 全过；
`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py` **PASS**。

`doc_audit.py funcs`：超过 120 行的函数 **15 → 14**；最长换成 `arkrtos_net_validate()`（144）。

**结论分档：宿主用例实跑（88 checks）+ 编译通过 + 告警同数 + 两份可复现构建 sha256 相同 + QEMU 端到端 PASS，未上板。**
固定槽位这条路径本身没有新增运行读数：它要一份板级槽表（`arkrtos_layout_slots()` 由 `config/` 生成）
加上真 Flash 后端，宿主上 `test_load_buffer_e2e` 走的是池内 AUTO 路径，不经过本函数。

#### 5.30.25 第二十笔：`arkrtos_net_validate()` 按「子命令的参数形状」分九段（2026-11-18）

`kernelsrc/src/arkrtos_net.c` 的 `arkrtos_net_validate()`（144 行）是一个十六个 `case` 的平铺 switch：
每个 case 体自己做「句柄是不是我的」加本子命令特有的缓冲区校验。三类检查在十六个 case 里被抄了十几遍，
想核一条规则得把整段扫一遍：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `net_validate_own()` | 只要求「句柄属于 caller」的子命令统一出口：`arkrtos_net_own(...) != 0` 才是 0 | 5 |
| `net_validate_listen()` | `own` + backlog 上限 255 | 8 |
| `net_validate_send()` | `own` + 长度上限 `0xFFFF` + 非空时 `user_ro_ok`（**SEND 与 SENDTO 的 case 体原本逐字相同**，共用这一份） | 13 |
| `net_validate_recv()` | `own` + 长度非 0 且不超 `0xFFFF` + `dev_buf_ok` | 13 |
| `net_validate_recvfrom()` | 调 `net_validate_recv()`，再校验两个可选的 4 字节出参 | 15 |
| `net_validate_getsockopt()` | `own` + 一个可选的 4 字节出参 | 13 |
| `net_validate_name()` | `own` + 两个可选的 4 字节出参（GETSOCKNAME / GETPEERNAME 共用） | 15 |
| `net_validate_shutdown()` | `own` + `how` 不超过 `SHUT_RDWR` | 8 |
| `net_validate_poll()` | `own` + 事件位非 0 且不含 IN/OUT/ERR 之外的位 | 14 |
| `arkrtos_net_validate()` 自己 | `SOCKET` 的类型校验仍内联（它是唯一不看句柄的子命令），其余各 case 只 `return net_validate_*(p);`，`default` 仍 `EINVAL` | **49**（原 144） |

口径三条：① **只按「参数形状」归并**，不按子命令逐个拆 —— BIND/CONNECT/ACCEPT/CLOSE/SETSOCKOPT 五个
原本写了两遍的「只看归属」合成一个 `net_validate_own()`；② `recvfrom` 是 `recv` 的超集，改成调用
`net_validate_recv()`，顺序仍是「先长度与出参、后两个可选地址出参」，与原文一致；③ 入口不补任何检查，
每个 helper 自己返回结论 —— 十六个 case 的返回码集合与先后顺序一处未改。

等价性归一化比对：only-OLD **24 行**，可逐件对齐 —— `SENDTO` 那一段**逐字重复的 case 体 24 行中的
19 行**（`own` 判、长度判、`user_ro_ok` 判、两个 `return`）因为被合并成 `net_validate_send()` 而少了一份，
剩下的是 `if(net_validate_own(p) != 0) return ARKRTOS_NET_EINVAL;` 改成 `if((net_validate_own(p) != 0) || …)`
的两处合写与 `return 0;` 变形；only-NEW 36 行全是九个签名、`return net_validate_*(p);`、花括号与缩进。
**判断条件、返回码、`case` 的归属关系一处未改**；`SEND`/`SENDTO` 这一处是**设计内去重**（两份逐字相同的
代码合成一份），不是语义改动。

体积：text `367752 → 367752`（**±0 B**，九个 helper 全内联，且编译器本来就把 SEND/SENDTO 两份重复体合并了），
`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 不变；`SOURCE_DATE_EPOCH=1600000000` 下
`BUILD=build/net_split` 与重建后的默认档两份 `.bin` **sha256 逐字节相同**（`63796b55…`），
**且与拆分前那一笔是同一个 sha256** —— 这一笔与第十七笔同型：买的是可读性，不是体积。
`arkrtos_net.c` 告警 **0 条**（该文件本来就没有告警）。

门禁：`coverage-host` 10 个模块全部不低于基线（`arkrtos_net` 不在覆盖对象内，未动基线）；
`sec-crc` 8 pin 全过；`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py` **PASS**
（横幅 / help / uname / dt / ls / cat / stack / utest 全过）。

`doc_audit.py funcs`：超过 120 行的函数 **14 → 13**；最长换成 `arkrtos_mini_run()`（144）。

**结论分档：编译通过 + 告警同数 + 两份可复现构建 sha256 相同且与拆分前逐字节相同 + only-OLD 逐行可归因 + QEMU 端到端 PASS，未上板。**
这一笔不新增用例：九条分支要真套接字句柄、真 `net_own` 台账与真 App 窗口边界才判得出来，
宿主上只能堆桩，堆出来的「红/绿」只证明判据写对了，不证明真路径对。

#### 5.30.26 第二十一笔：`arkrtos_mini_run()` 按「路径 / 占位 / 起任务 / 提交 / 回滚」分五段（2026-11-18）

`kernelsrc/src/arkrtos_mini.c` 的 `arkrtos_mini_run()`（144 行）从头到尾是五件事连做：量路径长度、
抢一个实例项、读镜像借块、起任务填 TCB、把项从 `LOAD` 改成 `RUN`；中间任意一步失败都落到同一个
`out:` 标签回滚。每件事的失败码不同，混在一个函数体里读不出「错在哪一步」：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mini_run_check_path()` | 量长度：`ARKRTOS_MINI_PATH_MAX` 以内且非空，超了不截断（截断会让两个不同路径撞成同一个身份） | 19 |
| `mini_run_slot_claim()` | 算崩溃哈希 → 关中断 → 判「被禁用」/「装载忙」→ 扫空项与在跑计数（到并发上限也是 `BUSY`）→ 清项、置 `LOAD`、写路径、定下本次的上限、置 `g_mini_busy`，成功交出项指针与 `max_block` | 57 |
| `mini_run_start_task()` | `arkrtos_task_register` 起任务；失败先 `ptable_mini_free` 把两块还回去再返回 `ERR_TASK`；成功填 TCB 的 `rom_*` / `ram_*` / `is_priv` 并 `mpu_build_task` | 26 |
| `mini_run_commit()` | 关中断把项从 `LOAD` 改成 `RUN`，写 `pt_index` / `ram_block` / 七个 `info.*`，放开 `g_mini_busy` | 20 |
| `mini_run_abort()` | 回滚：清占位项 + 放开 `g_mini_busy`（原 `out:` 标签里那段） | 12 |
| `arkrtos_mini_run()` 自己 | 五步顺序调用 + 成功后一条 `MINI` 日志 + `sched_activate_higher` | **47**（原 144） |

口径三条：① **占位的原子性没被拆散** —— 「判禁用 / 判忙 / 扫空项 / 置 LOAD / 置 busy」仍在同一次
关中断里，`mini_run_slot_claim()` 出锁之后实例项已经带着路径站在那里；② 回滚仍只有一处入口
（`out:` → `mini_run_abort()`），`load_image` 与 `start_task` 两条失败路径共用它，一次失败一次清、
不漏不双；③ `max_block` 仍然在**出锁之前**取值（原代码就是「本次装载用哪条上限，此刻定下来」）。

等价性归一化比对：only-OLD **21 行**，全部可归因 —— 18 行是 `img.` → `p_img->` 的改名（镜像结构由
入口持有后两段共用）、1 行 `max_block = g_mini_max_block;` 变成 `*p_max_block = …`、1 行 `uint32 irq2;`
与它的 `lock/unlock` 两行改写成 `mini_run_abort()` 里的 `irq`（回滚段独立成函数，不再借用入口的
`irq`）；only-NEW 60 行全是五个签名、局部声明、`return 0;` / `return ret;`、花括号与入口的五次调用。
**判断条件、返回码、日志文案、关中断的边界一处未改。**

体积：这一笔在 QEMU LA64 默认档下**该 TU 是空目标** —— `board/qemu_loongarch64/arkrtos_board_config.h`
把 `ARKRTOS_USE_MINIAPP` 定成 0，所以 text `367752 → 367752`、`data` 440 / `bss` 767508 /
镜像总长 31,462,512 B，两份 `.bin` 在 `SOURCE_DATE_EPOCH=1600000000` 下 sha256 相同（`63796b55…`），
**与拆分前是同一个 sha256**。为了不让「空目标等于没改」混过去，另用 shim 头（`build/_mini_chk/
arkrtos_board_config.h`：先吃真板级配置再 `#undef` / `#define ARKRTOS_USE_MINIAPP 1`）把该 TU 单独
`-c` 编了一遍：**exit=0**，告警 **5 条 = 基线 5 条**（全是既有的 `-Wint-to-pointer-cast`，唯一差异是
告警里打印的函数名从 `arkrtos_mini_run` 变成 `mini_run_start_task`）。

门禁：`coverage-host` 10 个模块不低于基线（`arkrtos_mini` 不在覆盖对象内，未动基线）；
`sec-crc` 8 pin 全过；`format_gate` 361 个自有文件干净；`example/qemu_loongarch64/shell_check.py`
**PASS**；`ci_gate` **47 passed / 0 failed**。

`doc_audit.py funcs`：超过 120 行的函数 **13 → 12**；最长换成 `arkrtos_mq_send_internal()`（143）。

**结论分档：编译通过（含 MINIAPP=1 强制档）+ 告警同数 + 默认档 sha256 与拆分前相同 + QEMU 端到端 PASS，未上板。**
这一笔**没有运行读数**：小程序装载要 FS + 实例表 + 真 RAM 池，QEMU LA64 板档把 `ARKRTOS_USE_MINIAPP`
关着，开口令就要改板级配置、那是另一件事（会增加默认档体积），故不在这里做。

#### 5.30.27 第二十二笔：`arkrtos_mq_send_internal()` 按「投递 / 不等待 / 登记 / 阻塞后分类」分四段（2026-11-18）

`kernelsrc/src/arkrtos_mq.c` 的 `arkrtos_mq_send_internal()`（143 行）是一个「快路径 + 睡下去 + 醒来
再判」的三段式，但三段之间还夹着四件互不相干的事：投递并叫醒接收者、满了且不等时撤掉过期登记、
登记自己入等待表、阻塞返回后的五种分类。混在一个函数里，`return` 有十几处，每次都要回头看
「此刻中断关着没有、我的登记撤了没有」：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mq_send_deliver()` | `arkrtos_mq_put` → `wake_one` → trace → 开中断 → 切任务；**快路径与「等到空间之后」的收尾是同一段** | 8 |
| `mq_send_no_wait()` | 有当前任务就撤掉自己可能留下的登记（用户态轮询用 `timeout==0` 收尾，一条过期登记会永久占住发送等待表一位），开中断报 `FULL` | 11 |
| `mq_send_waiter_push()` | `waiter_add` 登记自己；`-2`（已登记）视作成功照旧往下走，真满返回 `NOSPACE` | 10 |
| `mq_send_after_block()` | 阻塞返回后的五种分类：`sync_in_handler` → `WOULDBLOCK`；`ret<0` → 撤登记 + `CONTEXT`；`OBJ_DELETED` → **不撤登记** + `DELETED`；`WAKE_TIMEOUT` → 撤登记 + `TIMEOUT`；其余返回 1 表示「被唤醒，可以再判一次空间」 | 34 |
| `arkrtos_mq_send_internal()` 自己 | 参数/句柄/状态校验 → 关中断 → 未满投递 → `timeout==0` 走不等待 → 取当前任务 → 记起始节拍与上限 → 登记 → 阻塞 → 分类 → `while(count>=DEPTH)` 按剩余时间重排队 → 投递 | **55**（原 143） |

口径三条：① **投递段只有一份** —— 原来「进阻塞前的即时投递」与「醒来后终于拿到空间的收尾」是两段
逐字相同的代码（`put` / `wake_one` / trace / 开中断 / 切任务），改错一份没有任何提示，现在共用
`mq_send_deliver()`，**这是设计内去重，不是语义改动**；② 每个 helper 自己开中断再返回（⑰ 起的口径），
入口不补开；但「登记 + `block_in_critical` 必须在同一临界区内」这条**没被拆散**，`waiter_push()` 之后
入口仍不放开中断，落刀处就在这两行之间；③ `OBJ_DELETED` 那条**故意不撤登记**（队列已被删除方清空，
再去别人的表里摘自己是越界操作），这一条在 helper 里原样保留并加了注释。

等价性归一化比对：only-OLD **11 行**，全部可归因 —— 5 行是第二份重复的投递代码（两份并一份）、
4 行是 `return ARKRTOS_MQ_ERR_*;` 改成 `*p_ret=…; return 0;`、2 行是 `int32 added = …` 与
`if((added < 0) && (added != -2))`；only-NEW 32 行全是签名、局部声明、`return 0;` / `return 1;`、
花括号与入口的四处调用。**判断条件、错误码、日志文案、关中断的边界一处未改。**

体积：这一笔**不是**逐字节不变 —— 去重真的省下代码：text `367752 → 367608`（**−144 B**），
`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 不变，`SOURCE_DATE_EPOCH=1600000000` 下两份 `.bin`
sha256 相同（`c6f984f31f23080b8ab0d611ea56c37768916b635bae64bac34891813b402276`）；`arkrtos_mq.c`
告警 **0 条 = 拆分前 0 条**（构建日志里那 3 条是 `arkrtos_shell_image.c` / `arkrtos_shell_misc.c`
既有的 LA64 指针宽度转换，与本文件无关）。

**两道门禁被碰红，都是「先补证据」修好，没有下调基线：**

- `coverage-host` 一开始 FAIL `arkrtos_mq 43.24% < 基线 43.42%`。手写 gcov 复现（`build/_mqcov` 与
  `build/_mqcov_base` 两档，同一 host harness）判定这不是覆盖率掉了：拆分前 **357 个可执行行里执行
  155 行**，拆分后 **370 行里执行 160 行** —— **执行行数 +5，分母 +13**（新增 helper 的签名行与
  `*p_ret=…` 行），纯粹是「分母变大」。处置是给 `kernelsrc/tests/test_mq_host.c` 补**真用例**，
  不是改阈值。
- 补的用例先把两个内核桩改成**可编程**：`arkrtos_task_get_current()` 从「恒返回 0」改成返回
  `g_cur_task`（默认 0），`arkrtos_task_block_in_critical()` 从「恒返回 -1」改成按 `g_block_mode`
  给出五种返回值（默认 0 档仍是 -1）。**默认值与拆分前逐字一致，前面八段用例的路径一行没动**；
  只有新加的「10. 阻塞发送」一段才临时打开。六条路径逐条断言：A) 有任务但进不了阻塞 → 连做 5 次
  都必须是同一个拒绝（若登记没被撤销，第二次起会撞上重复登记）；B) 被唤醒时空间又被占走 → 重新
  排队一次、最终超时；C) 被唤醒时空间已腾出 → 投递成功且先进先出；D) 剩余时间已用尽 → 不再排队、
  直接超时；E) 等待期间队列被删除；F) 用户态轮询 → 报 `WOULDBLOCK`（**这是唯一能按数值区分的结论：
  `-4` 而不是 `-1`**）。用例从 **61 条涨到 88 条**（`MQ-HOST OK (88 checks)`）。
  随后 `coverage_gate.py --update` 把 `arkrtos_mq` 由 **43.42% 抬到实测 63.51%**，其余 9 个模块
  不低于基线（`arkrtos_mq.c` 里原来走不到的 `after_block` 五个分类、`waiter_push` 的满表分支与
  `deliver` 收尾现在都有真执行）。

门禁：`coverage-host` 10 个模块不低于基线（基线**上调**到实测）；`sec-crc` 8 pin 全过；
`format_gate` 361 个自有文件干净；`doc_audit.py funcs` 超过 120 行的函数 **12 → 11**（最长换回
`elf_load()` 143）；`example/qemu_loongarch64/shell_check.py` **PASS**；`ci_gate` **47 passed /
0 failed**。

**结论分档：宿主用例实跑（88 checks，含 6 条阻塞发送路径）+ 编译通过 + 告警同数 + 两份可复现构建
sha256 相同 + QEMU 端到端 PASS，未上板。** 阻塞路径的运行证据来自宿主（可编程桩驱动，判据是返回值
与队列状态，不是真调度器），真睡下/真被 ISR 唤醒/真超时推进仍只有上板用例能证明。

#### 5.30.28 第二十三笔：`elf_load()` 按「参数关 / 窗口切分 / 入口 / 段 / 栈」分五段（2026-11-18）

`kernelsrc/src/arkrtos_elf.c` 的 `elf_load()`（143 行）是这条流水线本身：参数与签名关、窗口切分、
读头、入口检查、段校验+写入、铺栈、起任务、填读数。前六关每一关都是「不通过就原样返回它的错误码」，
八种错误码混在一具函数体里，而中间还夹着两段必须整段原子的注释（栈尺寸口径、装配期竞态）：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `elf_prepare_args()` | 路径非空 → **签名门** → `argc` 上下界 → `argv` 缺省补成 `path` → 逐项非空；成功用出参交出最终 `argc`/`argv` | 53 |
| `elf_window_layout()` | 窗口三段：`win_top` / `argv_start`（16 字节对齐）/ `stack_bottom`，任一段为空即 `ERR_LAYOUT` | 20 |
| `elf_check_entry()` | 入口落在可执行区里且 4 字节对齐 | 12 |
| `elf_load_segments()` | 先 `elf_check_segments`（一个字节都不写）再 `elf_write_segments`，成功交出 `segs`/`bytes` | 27 |
| `elf_stack_build()` | 无分页档不需要；有分页档先 `paging_space_activate` 再 `elf_build_stack`，成功交出 `sp` | 22 |
| `elf_load()` 自己 | 读 ELF 头、五次顺序调用、算 `stack_bytes`、起任务、填 `p_info` 七项 | **88**（原 143） |

口径三条：① **签名门仍在「路径非空之后、读 ELF 头之前」** —— 这个顺序是有来历的（既不让「格式错误」
把「文件没签名」盖掉，也不对空路径白算一遍 HMAC），拆段时跟着 `elf_prepare_args()` 一起搬，位置没动；
② `#if (ARKRTOS_USE_EXEC_SIGN == 1)` 与 `#if (ARKRTOS_USE_PAGING == 1)` 两个条件编译块各自留在它所属的
helper 里，条件不成立时那一段连符号都不生成（无分页档 `elf_stack_build()` 用 `(void)space;` 消化形参，
不产生未使用告警）；③ 两段「必须整段原子」的注释留在入口，紧贴 `elf_start_task()` —— 它们解释的是
「注册 → 降权 → MPU 写回」这一步为什么不能被打断，属入口的语义，不属任何 helper。

等价性归一化比对：only-OLD **4 行**，全部是「声明与初始化合并」——1 行 `rc = elf_verify_signature(path);`
（改成 `int32 rc = elf_verify_signature(path);`，作用域收进 helper 的 `#if` 块）与 3 行
`win_top = …` / `argv_start = …` / `stack_bottom = …`（改成带初值的声明，因为它们在 helper 里第一次赋值
就是取值）；only-NEW 75 行全是签名、局部声明、`return`/`*p_…=`、花括号与入口五次调用。
**判断条件、错误码、地址计算、条件编译的边界一处未改。**

体积：这一笔**不是**逐字节不变 —— text `367608 → 367640`（**+32 B**），`data` 440 / `bss` 767508 /
镜像总长 31,462,512 B 不变，`SOURCE_DATE_EPOCH=1600000000` 下两次独立构建（`build/qemu_loongarch64` 与
`build/_elf_repro`）的 `.bin` sha256 相同（`1b6982cceaf26da927284bfaf3ff7fdefabbe53ba06727d3d9774f871b21b7a3`），
且与拆分前（`c6f984f3…`）不同；`arkrtos_elf.c` 告警 **0 条 = 拆分前 0 条**（构建日志里 3 条仍是
`arkrtos_shell_image.c` / `arkrtos_shell_misc.c` 既有的 LA64 指针宽度转换）。

门禁：`coverage-host` 10 个模块不低于基线（本文件不在覆盖对象内，未动基线）、`sec-crc` 8 pin 全过、
`format_gate` 361 个自有文件干净、`doc_audit.py funcs` 超过 120 行的函数 **11 → 10**（最长换成
`arkrtos_sync_release_task()` 141）、`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate`
**47 passed / 0 failed**。

**第一次跑端到端失败，记在这里不粉饰**：`FAIL: help 输出里没有命令 'blk'`。查串口原始输出，`blk` 确实
注册着（`[boot] … blk=1`），真正的原因是既有的 `ecode=0x000d era=0x00000008` 间歇异常——它在
`[MOD] tamper check PASS` 之后直接打死内核，shell 根本没走到 `help`。原样重跑一次即 **PASS**。
这笔改动不含任何异常/特权/上下文切换路径（只搬了参数与布局计算），异常落在 mod 演示之后也与
本笔无关；该间歇异常的根因仍挂在 §5.13.4 的欠账里，未在本轮处理。

**结论分档：编译通过 + 告警同数 + 两次独立可复现构建 sha256 相同 + only-OLD 4 行逐条可归因 +
QEMU 端到端 PASS（第二次），未上板。**

#### 5.30.29 第二十四笔：`arkrtos_sync_release_task()` 按「摘等待 / 移交锁 / 收自己的对象」分五段（2026-11-18）

`kernelsrc/src/arkrtos_sync.c` 的 `arkrtos_sync_release_task()`（141 行）是任务下线时的收尸：先把自己的
appq 标记清掉，再从条件变量 / 信号量 / 互斥锁三类对象的等待表里摘掉自己，然后处理「我正持有的锁」——
有等待者就移交给优先级最高者并唤醒，没有就释放，不持有则重算该锁主人的有效优先级；最后把自己建出来的
对象一起带走。三段摘等待的循环**逐字相同**，中间夹着一段锁移交，读起来很容易把「摘」和「交」看成一件事：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `sync_wipe_conds()` | 从所有 `used` 的条件变量等待表里清掉自己 | 19 |
| `sync_wipe_sems()` | 信号量同样 | 19 |
| `sync_wipe_mtxs()` | 先摘自己，再看自己是不是 owner：是则 `waiters_pop_highest` 移交（带 `HANDOFF` 语义、递归档把 `nest` 置 1）或直接释放（`nest` 清 0）；不是则 `mtx_recalc_priority` | 49 |
| `sync_drop_own_sems()` | `creator_id == task_id` 的信号量整条作废（等待表清空、`used`/`name`/`count`/`creator_id` 归零） | 19 |
| `sync_drop_own_mtxs()` | `creator_id == task_id` 的互斥锁整条作废；**优先级继承的回滚故意不做**（主人通常就是正在被拆的这个任务，它已被摘出就绪环，再去动环会把调度器搞坏） | 29 |
| `arkrtos_sync_release_task()` 自己 | 清 appq 标记 → 取本任务槽 → 三段摘等待 → 两段收对象 | **26**（原 141） |

口径三条：① **本函数仍不开关中断，调用方负责临界区**（函数头那条注释原样保留在入口）；② 三段摘等待
仍是「清空所有命中槽」而不是「摘第一个就返回」——虽然 `waiters_add` 保证一个任务只占一个槽，但这里
保留原来的防御性写法不动，语义零变化；③ 锁移交与释放在 `sync_wipe_mtxs()` 内部完成，`HANDOFF` 的
`wake_reason`、递归档的 `nest`、`ready_add` 的顺序全部原样。

等价性归一化比对：**only-OLD 为 0 行**（一条语句都没丢、没改名、没改缩进），only-NEW 24 行全是五个签名、
花括号与入口的两次注释 + 五次调用。

体积：这一笔**逐字节不变** —— text `367640 → 367640`（±0 B，五个 helper 全被 `-O2` 内联回去），
`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 不变，`SOURCE_DATE_EPOCH=1600000000` 下的 `.bin`
sha256 仍是 `1b6982cceaf26da927284bfaf3ff7fdefabbe53ba06727d3d9774f871b21b7a3`（**与拆分前同一个
sha256**）；`arkrtos_sync.c` 告警 0 条与拆分前同数。

门禁：`coverage-host` 里 `arkrtos_sync` **49.05% → 50.07%**（执行行数增加、分母基本不变，按惯例
`--update` 抬到实测，未下调），其余 9 个模块不低于基线；`sec-crc` 8 pin 全过；`format_gate` 361 个
自有文件干净；`doc_audit.py funcs` 超过 120 行的函数 **10 → 9**（最长换成 `arkrtos_crash_scan_apply()`
141）；`example/qemu_loongarch64/shell_check.py` **PASS**；`ci_gate` **47 passed / 0 failed**。

**结论分档：编译通过 + 告警同数 + 可复现构建 sha256 与拆分前相同 + only-OLD 为空 + QEMU 端到端 PASS，
未上板。** 收尸路径本身没有新增运行读数：要走通它需要真任务退出 + 真等待者队列（`sync-host` 那套
宿主用例只覆盖加锁/等信号量，不驱动任务下线收尸），所以「谁被移交到锁」这类结论仍只有上板或专门的
宿主夹具才能证明，本轮不声称。

#### 5.30.30 第二十五笔：`arkrtos_crash_scan_apply()` 按「认领槽 / 清扫无主 / 发布并扣留」分三段（2026-11-18）

`kernelsrc/src/arkrtos_crash.c` 的 `arkrtos_crash_scan_apply()`（141 行）是开机时对跨复位崩溃日志做的一次性
对账：分区表每次复位都会按池地址重排下标，日志却按上一次的下标记着账，所以要先「把每条账认领到名下地址真
的属于它的槽」，再「把没有任何槽认领的账扫掉」，最后「把计数发布到分区表并按上限扣留」。三段各扫一次整表，
中间隔着注释，混在一起读很容易把「认领」与「清扫」当成同一趟：

| 函数 | 管什么 | 行数 |
|---|---|---|
| `arkrtos_crash_scan_rebind()` | Pass 1：分区表里非空、且日志下标与该槽基址对不上的项，去全表找「基址相同」的另一条日志整条搬过来并把源条目清空（防止同一地址在同一趟里被记两次）；找不到就清掉（换了镜像 / 槽被复用，不能让新来者继承别人的崩溃） | 52 |
| `arkrtos_crash_scan_drop_unclaimed()` | Pass 2：日志里任何「没有任何非空槽认领的基址」整条清掉（卸载的镜像 / 地址已离开分区表），否则后一次恰好落到同地址的安装会继承旧计数 | 33 |
| `arkrtos_crash_scan_publish()` | Pass 3：把计数写进 `slot_crash_cnt`；对「已带禁用原因」或「计数达上限」的非空槽补禁用原因与 tick、清自动启动、`arkrtos_ptable_set_slot(..., INVALID, ...)` 扣留并打一条 `ARKRTOS_LOGE` | 51 |
| `arkrtos_crash_scan_apply()` 自己 | 取分区表 / 日志 / 上限 → 三段调用 → 有任何改动就提交 | **16**（原 141） |

口径三条：① 三段共用同一个「改过没有」的累计量，改成出参 `uint32 *p_changed`，入口不再在段内判断提交；
② 上限 `limit` 只有 Pass 3 用得到，所以只有 `publish()` 多带一个形参，另两段不带 —— 不为「签名统一」把
用不到的东西塞进去，那只会让三段离自己的语义更远；③ 循环边界 `(i < p_pt->slot_max) && (i <
(uint32)ARKRTOS_SLOT_ARRAY_MAX)`、两个局部标志 `taken` / `claimed`、`break` 的位置全部原样保留。

等价性归一化比对：**only-OLD 4 行**，全是同一件事——4 处 `changed = 1u;` 改成 `*p_changed = 1u;`
（Pass 1 里搬成一条、找不到来源清掉一条，Pass 2 一条，Pass 3 一条）；only-NEW 26 行全是三个签名（9 行）、
三组花括号、提到 helper 顶上的 `uint32 i;` 两处与 `uint32 k;` 一处、`uint32 limit,` 形参、入口的三次调用。

体积：text `367640 → 367640`（**±0 B**，三个 helper 都被 `-O2` 内联回 `scan_apply`），`data` 440 /
`bss` 767508 / 镜像总长 31,462,512 B 不变。**但 sha256 变了**：`1b6982cc…` →
`3d6a3c84a305a68f76417977c192306fba750cebef6d399475e3d282564c476b`。这不是语义变化，是「一大段」变成
「三段 + 内联」之后 `-O2` 换了 `scan_apply` 的栈槽分配顺序：把两个构建的 `arkrtos_crash_scan_apply`
反汇编出来逐条比，都是 **0x354 字节 / 213 条指令**；把寄存器号与立即数抹成占位符后**指令流逐条完全相同**
（`identical opcode stream: True`），只有 5 处 `st.d` 的栈偏移换了位次（`0xc8/0xb0/0xa0/0xe0/0xd8`）。
三次独立构建（`build/_crash_repro`、`build/_crash_repro2`、重编的 `build/qemu_loongarch64`）的 `.bin`
在 `SOURCE_DATE_EPOCH=1600000000` 下 sha256 全等于上面那一串。`arkrtos_crash.c` 告警 **0 条 = 拆分前 0 条**。

门禁：`coverage-host` 10 个模块均不低于基线（`arkrtos_crash` 不在覆盖对象内，基线未动）、`sec-crc` 8 pin
全过、`format_gate` 361 个自有文件干净、`doc_audit.py funcs` 超过 120 行的函数 **9 → 8**（最长换成
`arkrtos_mq_recv_internal()` 139）、`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate`
**47 passed / 0 failed**。

**结论分档：编译通过 + 告警同数 + 三次构建 sha256 一致 + only-OLD 4 行逐条可归因 + 指令流逐条等价 +
QEMU 端到端 PASS，未上板。** 这一笔**没有新增运行读数**：`scan_apply` 真正干事要有「上一轮留下的日志 +
这一轮分区表下标漂移」，QEMU 上正常启动时日志全空、三段都空转，所以「认领 / 清扫 / 扣留各命中几条」这类
结论本轮不声称；要造出来得先让镜像崩一次再改池布局，属另一件事。

#### 5.30.31 第二十六笔：`arkrtos_mq_recv_internal()` 按「取走 / 不等待 / 登记 / 阻塞后分类」分四段，并补上宿主阻塞接收用例（2026-11-18）

`kernelsrc/src/arkrtos_mq.c` 的 `arkrtos_mq_recv_internal()`（139 行）与上一笔的 `mq_send_internal()` 是镜像：
校验 → 关中断 → 有消息就取走 → 空队列且不等待就报空 → 登记等待者 → 用户态轮询检查 → 阻塞 → 按唤醒原因分类
→ 消息被别的接收者抢走就按剩余时间重排队。整段读下来，「取走」在两处各有一份**逐字相同**的拷贝，改错一份没有任何提示。

| 函数 | 管什么 | 行数 |
|---|---|---|
| `mq_recv_take()` | `get` → 叫醒一个发送者 → trace → 开中断 → 切任务 → 返回取到的字数（**快路径与「等到消息」的收尾共用这一份，属设计内去重**） | 10 |
| `mq_recv_no_wait()` | 空队列且不等：顺手撤掉自己可能留下的过期登记（用户态轮询用 `timeout 0` 收尾，一条过期登记会把接收等待表的一格永久占住）→ 开中断 → `EMPTY` | 9 |
| `mq_recv_waiter_push()` | 登记接收等待者；`waiter_add` 返回 `-2`（已登记过）照旧往下，真满才 `NOSPACE` | 10 |
| `mq_recv_after_block()` | 第一次阻塞之后的分类：`reason<0` → 撤登记 + `CONTEXT`；`OBJ_DELETED` → **不撤**（对象已删，等待表随它作废）+ `DELETED`；`WAKE_TIMEOUT` → 撤 + `TIMEOUT`；其余返回 1 = 「被唤醒，可以再判一次队列」 | 28 |
| `arkrtos_mq_recv_internal()` 自己 | 校验 → 关中断 → 取走 / 不等待 / 取当前任务 → 记起始节拍与超时上限 → 登记 → `in_handler` 检查 → 阻塞 → 分类 → `while (count <= 0)` 重排队 → 取走 | **97**（原 139） |

口径四条：① 取走段只有一份（原来「进阻塞前的即时取走」与「被 `WAKE_HANDOFF` 唤醒后的取走」是两段逐字相同的代码）；
② 每个 helper 自己开中断再返回、入口不补开 —— 但「登记 + `block_in_critical` 必须同处一个临界区」这条没被拆散，
`arkrtos_trace_wait_obj()` 与 `block_in_critical()` 仍在同一段临界区里；③ `OBJ_DELETED` 那条故意不撤登记；
④ `in_handler` 检查留在入口而不是搬进 helper —— 它判的是「这段话该不该往下走」，不是「阻塞之后怎么了」，
搬走会让「登记了却不睡」这件事看不出落在哪。

等价性归一化比对：**only-OLD 13 行**，逐条可归因 —— `int32 got = 0;`（1）、`got = arkrtos_mq_get(...)`
（2 行里 1 行是第二份重复取走）、`wake_one` / `mdk_trace` / `ENABLE_IRQ` / `SWITCH_TASK` / `return got`
各 1（第二份重复取走）、`int32 added = ...` 与 `if((added < 0) && (added != -2))` 各 1（第二份重复登记）、
`return CONTEXT` / `return DELETED` / `return SYNC_ERR_TIMEOUT` 各 1（改写成出参 `*p_ret`）。only-NEW 31 行
全是四个签名、三组花括号、`return 0|1`、`*p_ret = ...`、两处 `return mq_recv_take(...)`、
`return mq_recv_no_wait(...)`、`if(reason != 0) return reason;` 与入口的调用。

体积：这一笔**去重真的省了代码**。text `367640 → 367432`（**−208 B**），`data` 440 / `bss` 767508 / 镜像总长
31,462,512 B 不变；sha256 `3d6a3c84…` → `9caa8220238bda251737efeae12c51e8e61a78f0d280a72a1e256adb50d1d9b3`，
四个独立构建（`build/_mqrecv_repro`、`build/_mqrecv_doc`、`build/_mqrecv_new2` 与重编的
`build/qemu_loongarch64`）在 `SOURCE_DATE_EPOCH=1600000000` 下 sha256 全等于这一串。为把「−208 B 是哪来的」
说清楚，另用 `git show HEAD:kernelsrc/src/arkrtos_mq.c` 把上一版源码换进树里单独编了一次
（`build/_mqrecv_base`，编完立刻换回并核 sha256）：那一次 text = 367640 / sha256 = `3d6a3c84…`，与本笔之前
一致 ⇒ 差额全部来自这一笔。全量构建告警 **270 条 = 拆分前 270 条**（168 条 `int-to-pointer` + 99 条
`pointer-to-int`，全在 `arkrtos_shell_image.c` / `arkrtos_shell_misc.c` 的既有 LA64 指针宽度转换，另 3 条
既有未使用变量/函数），`arkrtos_mq.c` 本身 **0 条**。

**`coverage-host` 先红后绿，也是靠补真用例不是改阈值。** 拆完第一次跑：FAIL `arkrtos_mq 62.80% < 基线
63.51%`。手工 gcov 复现（`build/_mqcov2/`，口径同 `tools/coverage_gate.py` 头注释：只量被宿主用例链接的那一个
`.c`）：新 379 行 / 执行 238，旧 370 行 / 执行 235 ⇒ **执行行 +3、分母 +9**，纯口径效应。核查未覆盖行时发现
真正的问题不在百分比：**整个阻塞接收路径（`arkrtos_mq.c:581` 起）在宿主上是 0 覆盖**，
`mq_recv_waiter_push()` 与 `mq_recv_after_block()` 一行都没跑过 —— 上一笔给 `test_mq_host.c` 装的可编程桩只被
「阻塞发送」用上，「阻塞接收」还是空白。

于是给 `kernelsrc/tests/test_mq_host.c` 补第 11 段「阻塞接收」，复用已有桩（新增 `mode 7`：阻塞期间往
`g_block_free_handle` 投一条消息再返回 `WAKE_HANDOFF`），八个场景：空队列不等待 / `timeout>0` 但没有当前任务 /
有任务却进不了阻塞（连做 5 次同结论，登记确实每次都撤掉了）/ 醒来仍无消息 → 重排队一次后超时 / 醒来消息已到
→ 取走且只睡一次 / 剩余时间用尽 → 不再排队 / 等待期间队列被删 / 用户态轮询 → `WOULDBLOCK`。用例 **88 → 108**
（`MQ-HOST OK (108 checks)`），随后 `--update` 把 `arkrtos_mq` 由基线 63.51% 抬到实测 **74.67%**（其余 9 个模块
不降，diff 只动这一行）。

门禁：`coverage-host` 10 个模块均不低于基线、`sec-crc` 8 pin 全过、`format_gate` 361 个自有文件干净、
`doc_audit.py funcs` 超过 120 行的函数 **8 → 7**（最长换成 `arkrtos_mini_swap_at()` 139）、
`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate` **47 passed / 0 failed**。

**结论分档：宿主用例实跑（108 checks，含 8 条阻塞接收场景）+ 编译通过 + 告警同数 + 四次构建 sha256 一致
+ 与拆分前的 −208 B 用 HEAD 源码构建对账 + QEMU 端到端 PASS，未上板。** 阻塞接收的运行证据来自宿主（可编程桩
+ 返回值 + 队列状态判据），真睡下 / 真被 ISR 唤醒 / 真超时推进仍只有上板用例能证明。

**顺带修掉上一笔留下的一处文档结构缺陷**：`## 6. LoongArch32 移植` 与 `#### 5.30.30 …` 被写在了同一行，且
§5.30.30 的正文落在了第六节标题与它自己的引言之间；中英文两份文档同样症状。本轮把标题拆回两行、把
§5.30.30 与 §5.30.31 放回第五节尾部，第六节恢复「标题 → 6.1 …」的原结构。

#### 5.30.32 第二十七笔：`arkrtos_mini_swap_at()` 按「查路径 / 进入在途 / 装载挂切换 / 三类收尾」拆成六个 helper（2026-11-18）

`kernelsrc/src/arkrtos_mini.c` 的 `arkrtos_mini_swap_at()`（139 行）是「小程序 A/B 无缝升级」的外壳：
查路径 → 关中断做三条互斥与崩溃禁用判定 → 拍下旧版本特征并置在途 → 开中断、锁外装载新镜像 →
核对布局 → 挂切换 → 三条出口（成功 / 装载失败 / 装载后失败）。三条出口各自要开中断、各自要动不同的账，
原来靠 `goto load_fail` 与 `goto refuse` 两个标签兜住，读的时候必须来回跳着看谁清了什么。

| 函数 | 管什么 | 行数 |
|---|---|---|
| `arkrtos_mini_swap_check_path()` | `path==0` 与长度（`0` 或 `>= ARKRTOS_MINI_PATH_MAX`）两条 `PARAM`，否则 0 | 20 |
| `arkrtos_mini_swap_begin()` | 持锁做完全部前置判定：崩溃禁用（`hash` 由 `arkrtos_crash_hash_path()` 算）→ 三条互斥（`g_mini_busy` / `g_mini_swap.active` / `arkrtos_task_swap_pending()`）→ 目标任务存在 → 拍下 `task_id/ram_block/ram_size/code_block/max_block` → 置在途、记 `slot_i`、清 `img.pt_index`、拷路径 | 60 |
| `arkrtos_mini_swap_stage()` | 锁外装载（`max_block` 用 `begin` 拍下的那个）→ **`*p_loaded=1`** → 三条布局核对 → `arkrtos_task_swap_arm()` | 30 |
| `arkrtos_mini_swap_commit()` | 成功路径：放开装载闸门（`g_mini_busy=0`）→ 打 `armed` 日志 | 15 |
| `arkrtos_mini_swap_abort_load()` | 装载本身失败：**只清账**（`active=0`、`busy=0`），不 `drop`、不 `free` —— 镜像没成功就没借出块 | 9 |
| `arkrtos_mini_swap_abort_loaded()` | 装载成功但后续失败：`arkrtos_mini_swap_drop()` 拿借据 → 清 `busy` → 锁外 `arkrtos_ptable_mini_free()` | 14 |
| `arkrtos_mini_swap_at()` 自己 | 查路径 → 进入在途 → 装载挂切换 → 按 `loaded` 分派两条失败出口 / 成功收尾 | **35**（原 139） |

口径四条：① **两条失败出口不合并**。`load_fail`（装载失败，镜像自己不留借据）与 `refuse`（装载后失败，
必须把借出的一对块还池）语义不同，合并成一个 abort 就得对「`img.pt_index` 是否已被 load 置位」做推理，
那是把一个不变量换成一个隐含假设；这里的做法是让 `arkrtos_mini_swap_stage()` 用 `*p_loaded` 出参把
「失败发生在装载之前还是之后」显式带回来。② **每条非零返回前都还锁，且都不动 `g_mini_busy` /
`g_mini_swap.active`** —— 与拆分前逐条一致（前置判定失败时账本一个字段都不该变）。③ `max_block` 与旧版本
四样特征一起在锁内拍下：装载本身会放开锁（读文件），回读时 `g_mini_max_block` 与 `slot->info` 都不该再被读。
④ 路径拷贝的 `for` 循环原样保留（`(k+1) < MAX` 的边界与末尾补 `'\0'` 都没动）。

等价性归一化比对（剥注释 → 逐行去空白 → 多重集差）：**only-OLD 20 行**，逐条可归因 —— 局部变量声明与
改名 10 行（`drop_pt` / `old_ram_block` / `old_ram_size` / `old_code_block` 的声明，`task_id = slot->info.task_id`
与三处 `old_* = ...` 的赋值，`max_block = g_mini_max_block`），`goto load_fail` 1 行、`goto refuse` 2 行、
两个标签 `refuse:` / `load_fail:` 各 1 行（改成两个 helper 调用），`ret = arkrtos_mini_load_image(...)`、
`ret = ARKRTOS_SWAP_ERR_LAYOUT`、`ret = arkrtos_task_swap_arm(` 各 1 行（改写为 `return` / 直接返回 `arm` 的结果），
三条布局比对里的 `old_*` 3 行（改成 `p_ver->*`）。only-NEW 65 行（44 个不同文本）全是六个签名、
九组花括号、`return 0` / `return ret`、`p_ver->*`、`*p_loaded = 0|1` 与入口的三次调用与分派。

体积：默认 QEMU LA64 档 `ARKRTOS_USE_MINIAPP=0`（`TASK_SWAP` 也默认 0），整个 `.c` 被包成空目标，
**这一笔在默认档下体积与 sha256 逐字节不变** —— text **367432** / data 440 / bss 767508 / 镜像总长
31,462,512 B，sha256 `9caa8220238bda251737efeae12c51e8e61a78f0d280a72a1e256adb50d1d9b3`（与上一笔读数同值）。

**编入档（防假绿）**：默认档空目标意味着「编译通过」什么都不证明，所以另建 `build/_miniswap_chk/` 板级头，
先 `#include` 真实板级配置再 `#undef/#define ARKRTOS_USE_MINIAPP 1` 与 `ARKRTOS_USE_TASK_SWAP 1`，
单独 `-c` 编译 `arkrtos_mini.c`。**这一步本轮第一次是假绿的**：`-include arkrtos_board_config.h` 会先命中
`-Iboard/qemu_loongarch64` 里那份把 `MINIAPP` 写成 0 的真实头，于是 `-dM -E` 打出 `MINIAPP 0`、`nm` 里连
`arkrtos_mini_swap_at` 都没有 —— 把 `-Ibuild/_miniswap_chk` 放到**所有 `-I` 之前**才真正编进来
（`-dM -E` 同时打出 `MINIAPP 1` / `TASK_SWAP 1` 与一个哨兵宏）。真正编入后：`exit=0`、告警 **5 条**且与基线
同类同数（全是既有 `-Wint-to-pointer-cast`，位置在 `mini_load_stream` / `arkrtos_mini_run` 里，与本次改动无关），
`nm` 见 `T arkrtos_mini_swap_at` / `T arkrtos_mini_swap_take`。编入档对象体积对账（同一套 flags 各编一次
`build/arkrtos_mini_swap.base.c` 与新文件）：text **16392 → 16440（+48 B）**，其中
`arkrtos_mini_swap_at` 自身 `0x6c0 → 0x6e4`（**+36 B**）、`arkrtos_mini_swap_take` `0x310` 不变、
`g_mini_swap` 的 bss `0x6c` 不变 —— 即「切小了但它总代码变多」，这是拆分的正常代价，**不能当成去重收益**。

门禁：`doc_audit.py funcs` **7 → 6**（最长变 `arkrtos_flashapp_run` 139）、`format_gate` 361 个自有文件、
`sec_crc_gate` 8 pin + 4 TU 两档、`coverage_gate` 十模块不降（mini 不在覆盖对象内）、`mq_host_gate` 108 checks、
`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate` **47 passed / 0 failed**。

**结论分档：默认档逐字节不变（三次独立构建同 sha256）+ 编入档编译通过 + 告警同数 + QEMU 端到端 PASS，未上板。**
这一笔**没有新增运行读数**：mini swap 的真行为是「目标任务的下一次系统调用里换镜像」，QEMU 档 `MINIAPP=0`
根本不编这段代码，要出读数得先有能跑起来的小程序 + 一次真实的 SVC 落地，属另一件事。

#### 5.30.33 第二十八笔：`arkrtos_flashapp_run()` 按「闸门 / 核入口 / 借窗 / 起任务 / 发布」拆成六段（2026-11-18）

`kernelsrc/src/arkrtos_flashapp.c` 的 `arkrtos_flashapp_run()`（139 行）是「跑 Flash 里那个应用」的外壳：
占单实例闸门 → 从元数据取长度与入口 → 复核入口落在区内且首半字是序言 → 借窗并逐字段核对四个窗口常量 →
注册任务、把两块窗口写进 TCB、建 MPU 视图、置槽位为 RUNNING → 发布三样账、打日志、激活。
六条失败路径原来都靠 `goto release_slot` 兜住，而其中「借窗失败要归还」与「注册失败要归还」的归还对象并不相同。

| 函数 | 管什么 | 行数 |
|---|---|---|
| `flashapp_run_claim()` | 单实例闸门：持锁看 `g_flashapp_task`，非 0 报 `ERR_BUSY`，否则写 `0xFFFFFFFF` 占位 | 15 |
| `flashapp_run_release()` | 失败出口：只把 `g_flashapp_task` 清回 0（借窗由各自的失败点自己还） | 8 |
| `flashapp_run_check_entry()` | `entry = vec & ~1u` 后在区内且 `+2` 不越界，再核首半字等于 `ARKRTOS_FLASHAPP_PROLOGUE` | 18 |
| `flashapp_run_borrow()` | 调度器锁内 `arkrtos_ptable_mini_alloc(APPRUN_CODE_SIZE, FLASHAPP_RAM_SIZE)`，再逐字段核对四个窗口常量，两步失败都自己解锁/归还 | 39 |
| `flashapp_run_start()` | 注册任务（栈取窗口末尾 4 KB）、写 TCB 的两窗口与 `is_priv=0`、建 MPU、置槽位 RUNNING；**注册失败时自己把借来的窗还掉** | 37 |
| `flashapp_run_publish()` | 持锁写 `g_flashapp_pt/entry/task`、`task_out`、打日志、`arkrtos_sched_activate_higher()` | 23 |
| `arkrtos_flashapp_run()` 自己 | 依次调用五段，失败统一落到 `release_slot` | **48**（原 139） |

口径四条：① **借窗的所有权随成功转移**：`flashapp_run_borrow()` 成功即表示窗已属于本路径，`flashapp_run_start()`
注册失败时必须自己 `arkrtos_ptable_mini_free()`（与原来一致），调用方的失败出口一律只清闸门 —— 这样
「谁还窗」在代码里是唯一的，不靠 `pt_index` 是否非零去推断。② 借窗段自己开/关调度器锁，两条失败路径都
在返回前解锁，入口不再管调度器锁。③ 闸门的占位（`0xFFFFFFFF`）与释放（`0`）仍是同一把 `g_flashapp_lock`，
只是分别落在 claim / release 两个 helper，成对关系比原来隔着 139 行更容易看。④ `task_out` 必须仍在
`arkrtos_sched_activate_higher()` 之前带出去（新任务可能立刻跑完），所以发布段把「写账 + 带出」与「激活」
放在同一个函数里，顺序没动。

等价性归一化比对：**only-OLD 18 行**，逐条可归因 —— `code_base` / `code_size` 两个局部量消失（窗的几何只在
借用段内部核对，不再搬回入口）2 行、`uint32 entry;` 改 `uint32 entry = 0u;` 1 行、借用段两处
`goto release_slot` 改成 `return` 2 行、`entry = vec & ~1u` 搬进核入口段 1 行、`ret = ERR_LAYOUT` /
`ret = ERR_PROLOGUE` 各 1 行、`c_base/c_size/r_base/r_size` 的合并声明拆成四行 1 行、`ret = ERR_WINDOW` 2 行、
四个 `code_base = c_base` 一族的搬移 4 行、`ret = LOADER_ERR_TASK` 1 行、`g_flashapp_task = (uint32)task_id`
与 `*task_out = (uint32)task_id` 各 1 行（类型已统一成 `uint32`，不再需要强转）。only-NEW 65 行（41 个不同文本）
全是六个签名、九组花括号、`return 0|ret|ERR_*`、`*p_* = ...` 与入口的四次调用。

体积：LA64 默认档 Makefile 根本不编这个 TU（该目标没有 Flash 控制器），所以**默认档镜像与 sha256 逐字节不变**
—— text 367432 / data 440 / bss 767508 / 总长 31,462,512 B / sha256 `9caa8220…`（实测一次构建）。

**编入档（防假绿）**：用 armclang 对 H743 板级做真编译。命令是
`armclang --target=arm-arm-none-eabi -mcpu=cortex-m7 -O2 -c`，头路径 `kernelsrc/include` +
`config/stm32h743`（FLASHAPP 的一组地址宏只在 `config/stm32h743/arkrtos_partition.h` 里定义）+
`board/stm32h743` + `kernelsrc/port/arm/cortex-m7` + CMSIS（`example/stm32h743/kernel/ARKRTOS_TEST/Drivers/CMSIS`
的 `Include` 与 `Device/ST/STM32H7xx/Include`），并强制打开
`ARKRTOS_USE_FLASHAPP=1` / `ARKRTOS_USE_APPRUN=1` / `ARKRTOS_USE_VFS=1`、关掉 `ARKRTOS_USE_IMAGE_SIGN=0`、
补 `-DSTM32H743xx`。**这一步踩了两个坑**：上一笔留在 `build/_flashapp_chk` 的 shim 指向 STM32F427（`stm32f4xx.h`
在本机不存在，那条路根本编不起来），而 H743 的 CMSIS 头必须显式给 `-DSTM32H743xx` 否则报
"Please select first the target STM32H7xx device"。同一套 flags 各编一次 base 与新文件：**告警 0 条 = 0 条**，
`fromelf -z` 得 Code **3818 → 3888（+70 B）**、RO Data 447 不变、RW Data 12 / ZI Data 1064 不变 ——
其中 3818 与上一笔（`flashapp_write` 拆分）留下的读数逐字一致，可当作同一口径的交叉验证。
**+70 B 是切分的真实代价，不是去重收益。**

门禁：`doc_audit.py funcs` **6 → 5**（最长变 `arkrtos_sem_wait_internal` 138）、`format_gate` 361 个自有文件、
`sec_crc_gate` 8 pin + 4 TU 两档、`coverage_gate` 十模块不降、`mq_host_gate` 108 checks、
`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate` **47 passed / 0 failed**。

**结论分档：默认档逐字节不变 + 编入档（armclang H743）编译通过且告警 0 条 + QEMU 端到端 PASS，未上板。**
这一笔**没有新增运行读数**：跑 Flash 应用要有真 Flash 里的产物与一次真的调度切换，QEMU 档既无 Flash
控制器也不编这段代码。

#### 5.30.34 第二十九笔：`arkrtos_sem_wait_internal()` 按「句柄 / 快路径 / 不等待 / 登记 / 唤醒分类」拆成五段（2026-11-18）

`kernelsrc/src/arkrtos_sync.c` 的 `arkrtos_sem_wait_internal()`（138 行）是信号量 P 操作的全部逻辑：
句柄三查 → 计数大于 0 时按「队首优先、队首是自己才能取」的规矩取走 → `timeout==0` 只试一次 →
登记等待者 → 用户态等待者只登记不阻塞（`WOULDBLOCK`）→ 内核态在同一临界区里入队并睡下 →
醒来后按 `reason` 分类（参数错 / 对象被删 / 超时 / 假醒 / 真拿到）。十条返回路径各自带一次开中断。

| 函数 | 管什么 | 行数 |
|---|---|---|
| `sem_wait_check()` | 类型位 + 下标范围 + 槽在用三条，成功把下标带出；全在关中断之前，不碰任何队列 | 12 |
| `sem_wait_take()` | 快路径：计数 `<=0` 让路；有人排队且队首不是自己也让路；否则 `count--`、撤自己登记、`appq_set(0)`、记 `dbg_sem_takes`、开中断、返回 1 | 25 |
| `sem_wait_try_once()` | `timeout==0`：顺手撤掉自己可能留下的过期登记（否则 poster 会把 token 停在一个永远没人取的计数上），开中断、报 `ERR_TIMEOUT` | 10 |
| `sem_wait_enqueue()` | `arkrtos_waiters_add`；真满（`<0 && != -2`）报 `ERR_NOSPACE` 且**不开中断**（交回调用方）；新增登记成功记两个 `dbg_sem_*` | 15 |
| `sem_wait_classify()` | 阻塞归来后五路分类：`reason<0` 撤登记报 `PARAM`；`OBJ_DELETED` **不撤**队列（删除方已清空）报 `DELETED`；`1` 撤登记报 `TIMEOUT`；无 `WAKE_HANDOFF` 记 ghost（`arkrtos_diag_sync_ghost++` 与三个 `dbg_sem_ghost_*`）后撤登记报 `TIMEOUT`；其余撤登记报 `OK` | 54 |
| `arkrtos_sem_wait_internal()` 自己 | 句柄 → 取当前任务 → 关中断 → 快路径 → 不等待 → 上下文 → 登记 → 用户态 → trace + `block_in_critical` → 分类 | **56**（原 138） |

口径四条：① 快路径的两种结局靠返回值区分 —— `sem_wait_take()` 返回 1 表示「已取到、中断已开」，入口直接
`return 0`；返回 0 表示「这次不取、中断仍关着」，控制权回入口继续往下走。原来是
`if(count>0){ peek; if(top==0||top==p_tsk){ …; return 0; } }` 的嵌套，改成「不够就让路」的早退等形变体，
判断条件取反，语义与优先级规则一处未动。② **每条出口自己开中断**，入口不再补开 —— 与第十五笔
`arkrtos_mtx_lock_internal` 同一口径，一次失败一次开中断，不重不漏。③ 三处「撤掉自己的登记」不合并
（`try_once` / `classify` 的 `1` 与假醒）：它们是三种不同的「这个等待者不算数」的原因，合并会把
「对象已删就不要动队列」这条前提藏进共享代码里。④ 登记与 `block_in_critical` 仍落在入口的同一临界区内
（中间没有开中断），这是「唤醒不会投给一个还没睡下的任务」的前提，没有被拆段破坏。

等价性归一化比对：**only-OLD 3 行**，逐条可归因 —— `if(arkrtos_sems[idx].count > 0)` 一行（改成
`<= 0` 早退）、`arkrtos_task_t *p_top = arkrtos_waiters_peek(…)` 一行（声明与初始化拆开，早退分支用不到
`p_top`）、`if((p_top == 0) || (p_top == p_tsk))` 一行（改成 `(p_top != 0) && (p_top != p_tsk)` 让路）。
其余语句（含四段长注释、十个错误码、四处 ghost 记账）原样落在新位置。only-NEW 38 行（23 个不同文本）
是五个签名、六组花括号、`return 0|1|ret` 与入口的六处调用。

体积：这个 TU 在 LA64 默认档**真的被编进去**（QEMU 的 Shell 与 utest 都走信号量），所以这一笔
**不是**逐字节不变：text **367432 → 367512（+80 B）**，`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B
不变，`SOURCE_DATE_EPOCH=1600000000` 下 sha256 由 `9caa8220…` 变成 **`204d38d1…`**（一次构建，
`build/_semwait_repro`）。`arkrtos_sync.c` 本身告警 **0 条**，整档 `warning:` 计数 **270 = 270**。
**+80 B 是切分的真实代价，不是去重收益。**

门禁：`doc_audit.py funcs` **5 → 4**（最长变 `arkrtos_loader_scan_pool` 134）、`format_gate` 361 个自有文件、
`sec_crc_gate` 8 pin + 4 TU 两档、`coverage_gate` 十模块不降（`arkrtos_sync` **50.07% → 50.39%**，按惯例
只抬不降，本轮未改基线文件）、`mq_host_gate` 108 checks、`example/qemu_loongarch64/shell_check.py` **PASS**、
`ci_gate` **47 passed / 0 failed**。

**结论分档：编译通过 + 告警同数 + 可复现构建 + QEMU 端到端 PASS，未上板。** 上板这一档要说清：
QEMU 的 `utest` 里 `sem_token_flow` / `sem_reject_bad_handle` 真跑了「快路径 / 不等待 / 句柄拒绝」三条，
`sem_pi_*` 里 `sem_wait_internal(…, 500)` 也真调用了带超时的形态 —— 但那条用例通常当场拿到令牌，
所以 `sem_wait_enqueue()` 与 `sem_wait_classify()` 的「登不上」「真超时」「假醒」分支在本轮**没有运行证据**，
只有编译期正确性。

#### 5.30.35 第三十笔：`arkrtos_loader_scan_pool()` 按「扫描前置 / 跳过裸镜像 / 重建 RAM 块 / 认领镜像头」拆成四段，并补上池扫描的宿主端到端用例（2026-11-18）

`kernelsrc/src/arkrtos_loader.c` 的 `arkrtos_loader_scan_pool()`（134 行）是开机重建槽位表的唯一入口：
`scan_done` 闸门 → 清空槽位表 → 按 1 个分配单位遍历池的可用区间 → 命中裸镜像登记区间就整段跳过 →
命中镜像头就交给 `arkrtos_loader_accept()` 认领、登记槽位、写 autostart、按镜像头重建 RAM 绑定 →
扫完若运行期允许裸镜像，再让开发槽位里的裸镜像认领自己的区间。它原来是本文件里最长的一段，
里面还套着「固定 RAM 要不到就报错、绝不等价换址」这一条硬约束。

| 函数 | 管什么 | 行数 |
|---|---|---|
| `loader_scan_begin()` | `scan_done` 闸门：已扫过就返回 0；否则置位、`ptable_clear_slots()`、`ptable_get()` 带出、两个计数器清零、返回 1。只清槽位表，**不重种整张分区表** | 15 |
| `loader_scan_skip_raw()` | 这个地址是否落在已登记的裸镜像区间里：是则把 `addr` 抬到区间尾（`r_base > addr` 时先取 `r_base`），带出下一站地址并返回 1；否则返回 0 | 17 |
| `loader_scan_rebind_ram()` | 按镜像头重建 RAM 绑定。`runtime_ram_base != 0`：`ram_size` 向上取最小 2 的幂 → `ptable_ram_reserve` 成功才 `ram_bind`，失败打 `"fixed RAM 0x%08X is not available any more - reinstall the image"`（**不改址**）；否则 `ptable_ram_alloc` + `ram_bind` | 36 |
| `loader_scan_accept()` | `magic` 不对返回 0；`arkrtos_loader_accept()` 通过则 `ptable_alloc` → `set_slot(LOADED)` → 写 `pt->slot_autostart[slot]` → `rebind_ram` → 按 type 计 app/driver；末尾 `*p_step = (total != 0u) ? total : ARKRTOS_POOL_ALLOC_UNIT;` 返回 1 | 38 |
| `arkrtos_loader_scan_pool()` 自己 | 前置 → `while(addr < BASE+USABLE)`：跳过裸镜像 / 认领镜像头 / 步进 → 扫完按需 `claim_dev_slots()` | **38**（原 134） |

口径四条：① 「跳过」与「认领」都用返回值表达，入口只负责步进 —— `skip_raw` 返回 1 表示
「这个地址不管了、`next` 已算好」，`accept` 返回 0 表示「不是镜像头、按 1 个单位步进」，
两处原来分散在循环体里的 `continue` 与三元步长都被收进 helper，循环里只剩一条主线。②
`rebind_ram` 里那段「取不小于 `ram_size` 的最小 2 的幂」的空循环连同它的三段注释原样搬走，
一个字没改 —— 它表达的是「固定 RAM 要按装载时同一个块要回来」，属于语义而非优化。③
`accept` 里 `magic` 判断放在最前面并且**不推进计数器**，与原来 `if(p_hdr->magic == ARKRTOS_APP_MAGIC) { … }`
等价；差异只是从「包住整段」改成「早退」，缩进少一层。④ 末尾的裸镜像认领仍留在入口，
因为它不属于「一个地址」的处理，而是整轮扫描之后的第二道工序。

等价性归一化比对：**only-OLD 10 行 / 9 个不同文本**，逐条可归因 —— `addr += (total != 0u) ? total : ARKRTOS_POOL_ALLOC_UNIT;`
与 `addr += ARKRTOS_POOL_ALLOC_UNIT;`、`addr += total;` 三行（三种步长各自搬进 helper 或落成 `*p_step`）、
`addr = IMAGE_POOL_BASE;`（改成声明处初始化）、两个 `continue;`（改成 `next` / 返回值控制）、
`if(ptable_find_addr(...) >= 0)` 与 `if(p_hdr->magic == ARKRTOS_APP_MAGIC)`（各自变成 helper 的早退条件）、
`pt = ptable_get();`（搬进 `loader_scan_begin`）、`uint32 addr;`（改成声明处初始化）。
其余语句（含 `ARKRTOS_LOGE` 的两行拼接字符串、幂次循环、三个计数器）原样落在新位置。
only-NEW 35 行（23 个不同文本）是四个签名、六组花括号、四段注释归位与入口的四处调用。

体积：这一笔在 LA64 默认档 **text 与上一笔完全相同** —— `367512 → 367512`（±0 B），
`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 全部不变；但 sha256 由 `204d38d1…` 变成
**`dc72606e826c338e3f21506c6021cd36ca2f263ed207e6e91b5130c4b856f3ce`** ——
**总长相同不等于字节相同**，切分改变了指令排布，这里如实报「代码字节不同、总长相同」。
`arkrtos_loader.c` 告警 **21 = 21**，整档 `warning:` 计数 **270 = 270**。

门禁：`doc_audit.py funcs` **4 → 3**（`arkrtos_loader.c` 里两笔都消失，最长变
`arkrtos_loader_crc_recheck` 133）、`format_gate` 361 个自有文件、`sec_crc_gate` 8 pin + 4 TU 两档、
`mq_host_gate` 108 checks、`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate` **47 passed / 0 failed**。

覆盖率这一项本轮**先是红的**：`coverage_gate` 报 `arkrtos_loader 45.15% < 基线 45.77%`。
诊断（`cg.find_cc` + `cg.measure` 对拆分前后各编一次）显示 **执行行数 498 → 498 一行没少，分母 1088 → 1103
多了 15 行** —— 新增的正是四个 helper 的签名与参数检查行。这与第㉒笔（`arkrtos_mq_send_internal`）
同型，处置也同型：**不下调基线**，补真用例。`kernelsrc/tests/test_loader_host.c` 新增
`test_scan_pool_e2e()`：把镜像池真正映射到 `IMAGE_POOL_BASE`（`map_pool_at`，Windows `VirtualAlloc` /
POSIX `mmap MAP_FIXED`，**长度用 `IMAGE_POOL_SIZE` 而不是 `USABLE_SIZE`**），在池里摆四个位置 ——
裸镜像区（必须不被登记）、固定 RAM 的 app、走分配的 app、固定 RAM 已被别人占用的 driver ——
然后真调 `arkrtos_loader_scan()` 与 `arkrtos_loader_scan_driver()`。为了让这条用例能观察内部动作，
测试桩加了四个**默认全关**的可编程开关（`g_scan_on` / `g_raw_base,size` / `g_ram_alloc_ok` /
`g_ram_reserve_ok` / `g_ram_reserve_fail_base`），关掉时既有 88 条断言口径一字未动
（已用 `git show HEAD:kernelsrc/tests/test_loader_host.c` 配新 loader 跑出 **88 passed / 0 failed** 交叉验证）。
补完之后 `loader-host` 由 **88 → 107 passed，0 failed，0 SKIP**，`arkrtos_loader` 覆盖率
**45.77% → 52.31%**（分母 1103），用 `--update` 把基线抬到实测 52.31（只抬不降；同一次
`--update` 也把上一笔遗留的 `arkrtos_sync` 50.07 → 50.39 记进基线），十模块 `COVERAGE-GATE OK`。

**结论分档：宿主用例实跑 + 编译通过 + 告警同数 + 可复现构建 + QEMU 端到端 PASS，未上板。**
这一档要说清依据：`arkrtos_loader_scan_pool()` 的**运行证据来自宿主，不是 QEMU 也不是板子** ——
宿主的池是内存里的假 Flash，由 `map_pool_at()` 映射到真实绝对地址后直接 `memcpy` 假镜像头，
镜像头不带 CRC（`accept` 不比 CRC），所以「QEMU 开机扫池」这条真路径本轮**没有**新读数；
映射不上时用例打 SKIP 而不是假装跑过。另外补用例过程中修掉两处真实缺陷：一是桩的 `ptable_alloc`
原来无条件记槽位、导致前一个安装用例的槽位表残留让 `loader_fixed_slot_detach` 多 free 一次
（`g_free_n` 由 1 变 2），二是同一进程对同一地址第二次 `VirtualAlloc` 必失败导致
`test_move_slot_e2e` 被误判 SKIP —— 前者改成只在 `g_scan_on` 打开时才记账并返回递增槽号，
后者给 `map_pool_at()` 加了「已映射就直接返回」的短路。这两条都是**用例的问题，不是被测代码的问题**，
但把它们留在里面会让门禁时红时绿。

#### 5.30.36 第三十一笔：`arkrtos_loader_crc_recheck()` 按「前置校验 / 单表项回算 / 串联与比对」拆成三段（2026-11-18）

`arkrtos_loader_crc_recheck()`（133 行，`kernelsrc/src/arkrtos_loader.c`）是 SEC-G17 的核心：
把**落盘形态**的镜像（负载里那些绝对地址已按装载时的增量打过补丁）逐表项减回**标称形态**，
再按「头 + 重定位表 + 负载」重算一遍 CRC，与 `hdr.crc32` 比。它一次性把参数校验、头部校验、
两个增量的推导、表项遍历、尾段与比较全塞在一个函数里，是本文件当时第二长的函数。

| 单元 | 管什么 | 行数 |
|---|---|---|
| `loader_crc_ctx_t` | 中间状态：`hdr` / `p_rel` / `total` / `delta_rom` / `delta_ram` / `crc` / `pos`。只在下面两个 static 与入口之间用，**不对外、不进任何持久结构** | 11（typedef） |
| `loader_crc_begin()` | 前半段：`p_img` 空判 → 按字节拷头（不对齐指针不解引用）→ `check_header`（池口径）→ `total` 与 `cap` → `runtime_ram_base == 0` 报「算不出来」→ 两个增量公式 → 头部 CRC → 表体折进 CRC。返回 0 或一个可直接上抛的错误码 | 60 |
| `loader_crc_entry()` | 一个表项：拷 4 字节表项 → 算 `kind`/`off`/`need` → **`off < pos` 报 `RELOC`**（往回退会让同一段被重复计入）→ `off > pos` 的间隙先折进 CRC → 按 `kind` 选 `delta_ram`/`delta_rom` → 拷进对齐缓冲再减（`MOVW` 走 `arkrtos_loader_reloc_movw(word, 0u - delta)`）→ 折进 CRC 并推进 `pos` | 54 |
| `arkrtos_loader_crc_recheck()` 自己 | `p_got` 空判 → 清零 → `begin` → 逐表项 `entry` → 尾段 `pos < total` → `*p_got = crc` → 与 `hdr.crc32` 比 | **36**（原 133） |

口径四条：① **校验只留一份**：头部校验、`cap` 判断、`runtime_ram_base` 判断、两个增量公式
全在 `begin` 里，入口一行校验都不重复 —— 这条函数的意义就是「宁可报算不出来，也不拿错的
delta 硬算出一个不匹配」，把这句话摊在两处就等于给两种口径。② `p_got` 的空判与清零留在
入口（它是调用契约，不是校验），`p_img` 的空判在 `begin`（它属于校验）。③ 每个表项的
`need`/`delta` 选择、对齐缓冲、「减 delta = 加 `0 - delta`」全部原样搬走，一个字没改。④
`off < pos` 仍然**立刻返回**而不是记账继续 —— 这是「表项往回退」唯一的挡口。

等价性归一化比对：**only-OLD 29 行 / 29 个不同文本**，全部落在三条机械映射上，逐条可归因：
① 变量成为 `ctx` 成员：`hdr` → `c->hdr`、`p_rel` → `c->p_rel`、`total`/`delta_rom`/`delta_ram`/
`crc`/`pos` → `c->…`（共 17 行，如 `crc = arkrtos_crc32(p_img + pos, off - pos, crc);` →
`c->crc = arkrtos_crc32(p_img + c->pos, off - c->pos, c->crc);`）；② 声明搬家：`uint32 rt_len;`
从函数顶部挪进 `if` 块内（1 行），`if(rt_len != 0u)` → `if(c->hdr.reloc_count != 0u)`（1 行）；
③ 结构形变：`if(p_got != 0) { *p_got = 0u; }` 与 `if((p_img == 0) || (p_got == 0))` 两条合并判断
拆成「入口判 `p_got`、`begin` 判 `p_img`」（2 行）、`*p_got = crc` → `*p_got = c.crc`（1 行）、
`return (crc == hdr.crc32) ? 0 : ERR_CRC` → 同样比较但取自 `c.crc`（1 行）、`for(i = 0u; i < hdr.reloc_count; i++)`
→ `i < c.hdr.reloc_count`（1 行）。其余语句（含六段长注释、`MOVW` 与 `uint32` 两条分支、
四处 `arkrtos_crc32` 调用）语义与文本未变。only-NEW 53 行（43 个不同文本）是 typedef、两个
签名、六组花括号、`return 0|ret|ERR_*` 与入口的三处调用。

体积：**默认档 text 仍是 367512（±0 B）**，`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B
全部不变，但 sha256 由 `dc72606e…` 变成 **`d8a3afb0d1cdf75775bae29ca9890029843491e86921973406cf6e0df8f4eca0`**。
这次**把「为什么变了」查清楚了**才写结论：同一构建目录、同一 `SOURCE_DATE_EPOCH=1600000000`，
基线与新版各构建一次，`cmp -l` 只有 **21 个字节、7 处**不同，逐处都是立即数：`addi.w $r6,$r0,1528`
→ `1566`、`1458` → `1496`、`ori $r6,$r0,0x973` → `0x999`、`1121` → `1159` …… **全部恰好 +38**，
而替换块比原函数长 38 行。所以这 7 处是日志/断言里的 `__LINE__` 常量，**不是行为变化**：
指令条数、段布局、总长一个都没动。换句话说，这一笔的「逐字节不变」只对**代码**成立，对
**带行号的常量**不成立 —— 两种说法都不能省。

**编入档（防假绿）**：这一整段实现被 `#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)` 包着，默认档
一个编译器都不经过它，所以必须单独编。用 armclang 对 F427 板级做 `-O2 -c` 真编译，
`-DARKRTOS_USE_IMAGE_CRC_RECHECK=1`，同一套头路径与宏编基线/新版各一次：**告警 0 条 = 0 条**，
`fromelf -z` 得 Code **15852 → 16196（+344 B）**、RO Data 721 / RW Data 4 / ZI Data 521 不变。
**+344 B 是切分的真实代价**（多了一层调用与一个 28 字节的栈上 ctx，`-O2` 也没有把它内联回去），
不是去重收益。`tools/sec_crc_gate.py`（8 条 pin 各带变异自检 + 两个 TU 两档语法编译）仍
`SEC-CRC-GATE OK (8 pin + 4 TU 两档)`。

门禁：`doc_audit.py funcs` **3 → 2**（最长变 `arkrtos_loader_check_header` 125）、
`format_gate` 361 个自有文件、`coverage_gate` 十模块 OK（`arkrtos_loader` **52.31% → 52.78%**，
`--update` 把基线抬到 52.78，只抬不降）、`mq_host_gate` 108 checks、
`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate` **47 passed / 0 failed**。

**结论分档：宿主用例实跑 + 默认档体积不变（行号常量除外）+ 编入档（armclang F427）编译通过且零告警
+ QEMU 端到端 PASS，未上板。** `arkrtos_loader_crc_recheck()` 的运行证据来自宿主：
`test_loader_host.c` 的 `test_crc_recheck()` 真造了一份「落点不等于标称基址」的镜像，覆盖
①正向（反算回标称形态后与打包工具有同一个 `crc32`）②负载改一个字节报 `ERR_CRC`
③`p_img`/`p_got` 为空各报 `ERR_PARAM` ④`cap` 少 4 字节报 `ERR_SIZE`（不是「不一致」）
⑤`runtime_ram_base == 0` 报 `ERR_PARAM`（「算不出来」不是「对」）⑥表项往回退报 `ERR_RELOC`，
`loader-host` **107 passed / 0 failed / 0 SKIP**。**没有的读数**：QEMU 开机后在真池镜像上跑这条
复核（默认档开关是 `n`，QEMU 档不开），以及真 Flash 的电离位翻转 —— 这两条仍然只有
`utest run loader_crc_recheck_pool_e2e` 那条需要开档的手工路径，不在本笔证据范围内。

#### 5.30.37 第三十二笔：`arkrtos_loader_check_header()` 按「身份三查 / 形状七查 / 表体」拆成三段（2026-11-18）

`arkrtos_loader_check_header()`（125 行，`kernelsrc/src/arkrtos_loader.c`）是装载链路上最热的一道闸门：
安装、扫描、搬移、流式安装、CRC 复核、小程序装载等六类路径共九处调用点都要过它。它把三类判据塞在一个函数里 ——
「这张头是不是我们的」（魔数 / 兼容号 / 类型位）、「这张头描述的镜像有多大、入口在哪、表在哪」、
以及「重定位表每一个表项本身合不合法」。三类判据之间没有任何共享状态，只有一份 `p_hdr`（和 `p_rel`），
所以这一笔是**纯搬运**：原函数每一行语句原样活在新的位置上，连缩进都没动。

| 单元 | 管什么 | 行数 |
|---|---|---|
| `loader_hdr_check_id()` | 身份三查：`magic` → `ERR_MAGIC`；`hw_compat_id` → `ERR_COMPAT`；`(type >= 32u) || ((types_mask >> type) & 1u) == 0u` → `ERR_PARAM`。**越界的 `type` 必须先挡住**，因为 `1u << 33` 是未定义行为，不能把它当掩码判断用 | 19 |
| `loader_hdr_check_layout()` | 形状七查：`image_size == 0` / `image_size > 0xFFFFFFFF - ARKRTOS_APP_HEADER_SIZE`（回绕）→ `ERR_SIZE`；`entry_offset >= image_size` → `ERR_ENTRY`；`reloc_offset` / `reloc_count` 上限 / `reloc_kind` / `payload_offset` 四条 → `ERR_RELOC` | 45 |
| `loader_hdr_check_reloc()` | 表体：逐表项算 `kind`/`off`/`need`，按种类定粒度（`MOVW` 半字对齐且覆盖 8 字节，数据类整字 4 字节对齐）→ 必须落在 `[payload_offset, TOTAL_LEN - need]` → 必须升序（`off < prev` 报 `RELOC`） | 50 |
| `arkrtos_loader_check_header()` 自己 | `check_id` → `check_layout` → `if(p_rel != 0) check_reloc` → **`ram_size` 两条检查 → `return 0`** | **36**（原 125） |

口径三条：① **顺序即优先级**：三段严格按「是不是我们的 → 描述得对不对 → 表体对不对」排，
每一段不进就直接上抛，不再往下判 —— 一张魔数都不对的头报 `ERR_MAGIC`，不会因为 `image_size` 也是 0
就报 `ERR_SIZE`。② **`ram_size` 检查刻意留在最后**（原代码就在表体之后）：先验「这张头描述的是什么」，
再验「这台设备装不装得下」，所以表体错误优先于 RAM 大小错误，这条错误优先级一处未改，注释里写明了原因。
③ `p_rel` 的空判留在入口 —— 它决定「要不要校验表体」，不是校验本身（流式安装只先拿到 256 字节的头，
表体还在串口上，此时 `p_rel == 0` 是合法状态，调用方必须在表体落盘后补一次带地址的调用）。

等价性归一化比对：**only-OLD 0 行 / 0 个文本** —— 剥掉注释与空白后，原函数的每一行都逐字出现在
新位置，一条语句都没丢、没改名、没合并。only-NEW **28 行 / 12 个文本**，全部是新增的骨架：
三个 `static int32 …(…)` 签名、三组花括号（各 6 行的 `{` 与 `}`）、三个 `return 0;`、`int32 ret;`、
三处 `ret = loader_hdr_check_…(...);`、三处 `if(ret != 0) { return ret; }`（第三个在 `if` 块内）。
这是拆到现在**第二笔 only-OLD 为空的**（第一笔是 `audit_partition`），也与本笔选择「纯搬运」一致：
三个 helper 之间不共享任何参数，没有任何理由做变量改名或状态外提。

体积：**默认档 text 仍是 367512（±0 B）**，`data` 440 / `bss` 767508 / 镜像总长 31,462,512 B 全部不变，
但 sha256 由 `d8a3afb0…` 变成 **`9e59d69377d3e52d0c7ac478a1a19ddeab0bd4a7bea31ad07494e5e5a5804683`**。
同目录同 `SOURCE_DATE_EPOCH=1600000000`、基线与新版各构建一次、`cmp -l` 得 **292 个字节 / 74 处**不同，
逐个归因成三件事：① `__LINE__` 常量 —— 日志里的行号立即数全部 **+40**（`addi.w $r6,$r0,1566` → `1606`、
`1159` → `1199`、`1107` → `1147`、`1778` → `1818` …），因为插入块让插入点之后的行整体后移 40 行
（文件 3633 → 3673 行）；② `arkrtos_loader_check_header` 自己缩了 **8 字节**（`nm -S` 0x150 → 0x148），
`-O2` 把三个 helper 全内联回来了（`nm` 里一个 `loader_hdr_check_*` 符号都没有），只是分支编码变了；
③ `arkrtos_loader_load_dev_hdr` 里一个共用的错误返回基本块换了落点（`0x84ac` → `0x8400`），
连带整段分支位移重算 —— 函数本身的尺寸、地址一处未变（0x8D8）。**instruction 条数与段布局没动，
`text` 读数一分没变；但「逐字节不变」这句在这一笔不成立**，不能省。

**编入档（防假绿）**：`check_header` 没有 `#if` 包着（六条路径都要过它），默认档真的编它 ——
LA64 那份读数就是它的证据。另用 armclang 对 F427 板级做 `-O2 -c` 单 TU 编译，同一套头路径与宏
编基线/新版各一次：**告警 0 条 = 0 条**，`fromelf -z` 得 Code **14620 → 14616（−4 B）**、
RO Data 713 / RW Data 4 / ZI Data 521 不变（这条路径上 `-O2` 也把三个 helper 内联了，
减掉的 4 字节是分支编码差异）。`tools/sec_crc_gate.py` 仍 `SEC-CRC-GATE OK (8 pin + 4 TU 两档)`。

门禁：`doc_audit.py funcs` **2 → 1**（唯一剩下的超过 120 行的函数是 `arkrtos_sbuf_send_internal` 124，
`kernelsrc/src/arkrtos_sbuf.c:428`）、`format_gate` 361 个自有文件、
`coverage_gate` 十模块 OK（`arkrtos_loader` **52.78% → 53.41%**，`--update` 把基线抬到 53.41，只抬不降）、
`sec_crc_gate` 8 pin + 4 TU、`mq_host_gate` 108 checks、
`example/qemu_loongarch64/shell_check.py` **PASS**、`ci_gate` **47 passed / 0 failed**。

**结论分档：宿主用例实跑 + 默认档体积不变（行号常量与基本块落点除外）+ armclang F427 编译通过零告警
+ QEMU 端到端 PASS，未上板。** `check_header` 的运行证据来自宿主：`test_loader_host.c` 里大量
「头部该被拒」的用例按错误码逐条判（`ERR_MAGIC` / `ERR_COMPAT` / `ERR_PARAM`（类型位越界与不接受两种）/
`ERR_SIZE`（`image_size` 为 0、回绕、`ram_size` 越界）/ `ERR_ENTRY` / `ERR_RELOC`（四条头字段与
表体越界、粒度、乱序各一条）），`loader-host` **107 passed / 0 failed / 0 SKIP**；QEMU 默认档开机后
的 `exec` / `ld` 走的就是同一条函数。**没有的读数**：九处调用点各自的拒绝计数没有独立探针
（现有用例是「返回码对不对」，不是「哪条路径报的」）。

#### 5.30.38 第三十三笔：`arkrtos_sbuf_send_internal()` 把「放得下就写掉」那段去重（2026-11-18）

`arkrtos_sbuf_send_internal()`（124 行，`kernelsrc/src/arkrtos_sbuf.c`）是长函数清单上的最后一个：
它把参数校验、快路径投递、不等待、登记等待者、阻塞与唤醒分类全塞在一个函数里。拆开看，它只有两处
真正的重复 —— 「把放得下的那部分写进去」这段代码在函数里出现了**两次，逐字相同**：

第一次是进阻塞之前的快路径（`if(room > 0u) { … }`），第二次是被 `WAKE_HANDOFF` 唤醒之后的重试
（`if(arkrtos_sbuf_room(p_sb) > 0u) { … }`）。两份副本的字数、缩进层次（8 空格 / 12 空格）不同，
所以肉眼看不出来；改错一份没有任何提示 —— 这正是 `arkrtos_mbuf_recv_internal`（第十八笔）遇到过的同型问题。

| 单元 | 管什么 | 行数 |
|---|---|---|
| `sbuf_send_fit()` | 算 `room` → 0 就 `return 0` → `n = min(len, room)` → `put_bytes` → `wake_one` → 一条 `mdk_trace_arkrtos_obj_signal` → `return n`。**只写不睡**：开中断与切任务留给调用方，因为「写完就返回」与「写完继续判断」只有调用方知道 | 18 |
| `arkrtos_sbuf_send_internal()` 自己 | 参数三道（`buf`/`len`、句柄、`len` 不得超过整块缓冲）→ 关中断 → `fit` 一次、写成就开中断切任务返回 → `timeout_ms == 0` 不等待（顺手撤旧登记）→ 取当前任务 → 算 `deadline` → `for(;;)`：没空间就登记、用户态只登记、剩余时间为 0 撤登记报超时、`block_in_critical`、没有 `HANDOFF` 位按 `reason` 分「超时」与「对象被删 / 未能阻塞」、有则 `fit` 一次 | **110**（原 124） |

口径三条：① **去重不是改写**：`fit` 里的每一句都是原快路径那段的原文，只是把 `room > 0u` 的提前返回
换成了 `return 0`；重试那条路径原来靠 `if(room > 0u)` 判「有没有写」，现在靠 `n > 0` 判同一个事实。
② `fit` **不碰中断**：原文两次调用都在同一个临界区里、写完立刻 `ENABLE_IRQ + SWITCH_TASK`，
把开关中断塞进 helper 就得把「谁来切任务」也一起编码进去，那是把顺序藏起来而不是说清楚。
③ 「两处相同代码」之外一个字都没动：参数校验、`NOSPC` 的注释、等待表满 `-2` 的例外、
`WOULDBLOCK` 那条用户态分支、`HANDOFF` 位分类、末尾那条「被唤醒但空间又被别人占走：按剩余超时重睡，
不返回假成功」的注释全部原样保留。

等价性归一化比对：**only-OLD 10 行 / 9 个文本** —— 就是第二份副本本身（`uint32 room = …`、
`uint32 n = ((uint32)len < room) ? …`、`put_bytes`、`wake_one`、`obj_signal` 两行、`return (int32)n;`）
加上两个「有没有空间」的守卫（`if(room > 0u)` 与 `if(arkrtos_sbuf_room(p_sb) > 0u)`）；
only-NEW **15 行 / 12 个文本**（两处 `n = sbuf_send_fit(…)`、两处 `if(n > 0)`、两处 `return n;`、
`return 0;`、一个 `{` 与一个 `}`、helper 的两行签名、`uint32 n;`、`if(room == 0u)`、
`n = ((uint32)len < room) ? …`、入口的 `int32 n;`）。逐条可归因，没有「顺手改了点别的」。

体积：**默认档逐字节不变** —— text **367512（±0 B）**、`data` 440 / `bss` 767508 / 总长
31,462,512 B 全不变，sha256 **仍是 `9e59d693…`（`cmp -l` 0 处不同）**，且文件从 1134 行变成 1147 行
（+13）也没让任何 `__LINE__` 常量变 —— 这条函数之后 `arkrtos_sbuf.c` 里没有带行号的日志。
原因与第十八笔同型：**LA64 上的 GCC 本来就把两份相同代码合并了**，去重买到的是可读性不是体积。

**同一份代码在 ARM 上不是这样**：armclang F427 `-O2 -c` 单 TU（必须带 `-DARKRTOS_USE_SBUF=1`，
默认档该 TU 是空目标，`fromelf -z` 全 0 —— 这一点本身也是证据：不开档时 `-c` 出来的是 0 字节 Code）
基线与新版各一次：**告警 0 条 = 0 条**，Code **9150 → 9214（+64 B）**、RO Data 176 → 184（+8）、
ZI 1264 不变。**+64 B 是这一笔在 ARM 上的真实代价**（多了一层调用，armclang 没把 helper 内联回去，
也不再各自展开两份相同代码），与 LA64 的 ±0 B 并不矛盾：两个编译器对「重复代码」的处理本来就不同，
两个读数都得报，不能只挑好看的那个。

门禁：`doc_audit.py funcs` **1 → 0** —— 这是这条长函数清单**第一次清零**（下界口径：
只认「函数名与括号同行」的函数）；`format_gate` 361 个自有文件、`sec_crc_gate` 8 pin + 4 TU、
`coverage_gate` 十模块 OK（`arkrtos_sbuf` **46.69% → 47.57%**，`--update` 抬到 47.57，只抬不降 ——
涨的原因与第十八笔一样：那份重复副本在宿主流上走不到，删掉它等于删掉分母里永不执行的整段）、
`mq_host_gate` 108 checks、`example/qemu_loongarch64/shell_check.py` **PASS**、
`ci_gate` **47 passed / 0 failed**。

**结论分档：宿主用例实跑（`SBUF-HOST OK`，覆盖率实测有升）+ 默认档逐字节不变（`cmp -l` 0 处）
+ armclang F427 开档编译通过零告警（+64 B / +8 B RO）+ QEMU 端到端 PASS，未上板。**
`send_internal` 的运行证据来自宿主：`test_sbuf_host.c` 覆盖环形缓冲与消息分帧的收发，
但**阻塞发送这条路径在宿主上没有专门的形参化桩**（`test_mq_host.c` 那种「六条阻塞路径」的用例
`sbuf` 侧还没有）—— 这一条是**已知缺口**，登记而不假装有；`shell_check.py` 走的 `utest` 里也没有
`sbuf` 的阻塞发送用例。

> **2026-11-19 补记（上面这段是登记时的状态，不改写）**：缺口已补。`test_sbuf_host.c` 换成
> 可编程阻塞桩，`sbuf` 的发送/接收与 `mbuf` 的发送/接收四条路径各走六类收尾（无当前任务 /
> 睡不下去 / 超时 / 交接成功 / 等待期间删对象 / 用户态轮询），外加「mbuf 半条消息不许放走读者」，
> 共 **169 条断言**；`coverage_gate` 的 `arkrtos_sbuf` 47.57% → 80.50%。这组用例同时抓出两处
> 真实缺陷（`-1` 被当成令牌导致死循环、`ERR_DELETED` 不可达）并已修复，细节见
> [单元测试框架](单元测试框架.md) 的「`sbuf` 阻塞路径：可编程桩」一节。真睡、真 ISR 交接、
> 真节拍推进下的超时仍只有上板能证明。

#### 5.30.39 第三十四笔：`elf_load()` 拆成「交接块 + 两档分支 + 收尾」，并暴露默认档的死代码盲区（2026-11-21）

第三十三笔之后 `doc_audit funcs` 的清单**并没有清零**（当时的「已清零」是拿另一份构建量出来的读数，
见 [欠账盘点](欠账盘点.md) 的更订）：2026-11-21 实测 `kernelsrc/src/arkrtos_elf.c` 的 `elf_load()`
**338 行**，是全仓最长的一个。它把两档装载的全部流水线（判参数 → 切窗口 → 读头 → 查 PT_INTERP →
量镜像 → 查范围 → 写段 → 装解释器 → 铺 auxv → 铺栈 → 起任务 → 填 `p_info`）写在一个函数里，
**两个分支各写了一遍「量 → 查范围 → 写镜像 → 记 tp/segs/bytes」**，读的时候没法确认两档用的是不是同一套判据。

| 单元 | 管什么 | 行数 |
|---|---|---|
| `elf_load_state_t` | 交接块（24 个字段）：`path`/`space`/`phoff`/`phnum`/`phentsize`/`entry`/`etype`/`win_base`/`stack_bottom`/`img`/`bytes`/`segs`/`tp`/`task_entry`/`aux`/`auxn`/`p_info`/`win_top`/`argv_start`/`argc`/`argv`/`start_task`/`p_tp`。两档往同一块里写结果，收尾只有一份 | — |
| `elf_load_dynamic()` | 动态档：ET_DYN 主程序按窗口基址 `bias` 装载、量、判入口、查范围（不许越进解释器区）、写段、记 `tp`/`segs`，再调 `elf_load_interp()`，最后铺 `AT_PHDR`/`AT_BASE`/`AT_ENTRY`… 十七项 auxv 并填 `p_info->dynamic`/`interp_*` | **94**（原 142） |
| `elf_load_interp()` | 解释器单独一份：读头 → `ibias`（ET_DYN 用板级基址、ET_EXEC 必须本来就在那儿）→ 量 → 判入口 → 查范围 → 写段（`bytes` 累加）→ 与主程序镜像不许重叠。**失败一律折成 `ERR_INTERP`** | 68 |
| `elf_load_static()` | 静态档：按 `e_vaddr` 落位，入口就是 `e_entry`，铺含 `AT_RANDOM`/`AT_CLKTCK` 的 auxv；行为与引入 DYNLOAD 之前逐位一致 | 66 |
| `elf_load_finish()` | 收尾：`elf_stack_build()` 铺栈 + 填 `p_info` 七项 + `arkrtos_linux_abi_reset()` + `elf_start_task()`（整段原子，理由见函数内注释） | 65 |
| `elf_load()` 自己 | 参数 → 窗口切分 → 读头 → 查 PT_INTERP（默认档把「有 PT_INTERP」直接判 `ERR_FORMAT`，不半跑）→ 填交接块 → 按 `has_interp` 派发 → `elf_load_finish()` | **110**（原 338） |

口径三条：① **两档共用的判据只有 `elf_measure_image` / `elf_check_range` / `elf_check_entry`**，
拒绝码不同（主程序越界给 `ERR_LAYOUT`、解释器任何失败给 `ERR_INTERP`），所以解释器那一整段没有并进
`elf_load_dynamic()` 的尾巴，而是独立成 `elf_load_interp()`。② `has_interp` 仍只在 `#if (ARKRTOS_USE_DYNLOAD == 1)`
里存在，默认档与开档的派发结构靠 `#if / #else / #endif` 包住同一个 `{ rc = elf_load_static(&st); }` 块，
两种档编出来都只有一条路径。③ 原来写在函数中段的两段「必须整段原子」的注释跟着各自代码进了
`elf_load_finish()` / `elf_start_task()`，没有留在入口处当装饰。

**这一笔真正的收获不是行数，是抓到了默认档看不见的缺陷。** `ARKRTOS_USE_DYNLOAD` 默认是 0，
所以 `elf_load_dynamic` / `elf_load_interp` 在默认档**整段被预处理掉**：拆分后我第一次只跑了默认档
`make`，`exit=0`、告警只剩 5 条未用变量（`bytes`/`segs`/`tp`/`task_entry`/`auxn` 留在入口没删）。
带着 `-DARKRTOS_USE_DYNLOAD=1` 编译（`tools/dynlink_gate.py` 做的就是这件事）才现形两处**默认档
永远看不到**的错：`elf_measure_image(...)` 调用后面**多留了一行 `&st->img);`**（抽取函数体时重复了一段），
以及 `elf_write_image(..., &st->bytes, &tls_placed)` 用到的 `tls_placed` **在动态档里没有声明**
（它只声明在静态档的局部）。两处都是「默认档编译通过、开档编不过」——结论：**拆默认关的分支时，
「默认档 make 过」不能算验证**，必须拿开档门禁编一遍；这与本仓 `paging_gate` 那次「门禁死代码假绿」
（见本文 §5.33）是同一类错误，只是发生在被测物而不是门禁上。

等价性：`default` 档两遍**同配置**构建（`make clean` 之后各跑一次，`make` 不跟踪 `-D`，不清就量到别人的 `.o`）——
text **448680 → 448664（−16 B）**、`data` 448 / `bss` 5,128,740 / 镜像总长 **31,462,512 B** 全不变，
两份 `.bin` 的 sha256 分别是 `c2630b80…` / `0d02542b…`（**不是逐字节不变**，代码真的短了 16 B）；
`arkrtos_elf.c` 在默认档全量构建里 **0 条告警**。

门禁：`tools/dynlink_gate.py` **13 passed / 0 failed**（其中 `compile-on` 开档编 5 个 TU、`build` 开档全链、
`qemu-readout` 两条 PASS + `exit 105`、坏夹具 `exit 127`、`fixture-rebuild` 逐字节相同、`restore-default` 收尾），
`tools/format_gate.py` **OK（392 个自研文件）**，`tools/ci_gate.py` **56 passed / 1 failed**——唯一红的是
`api-doc`（新增 4 个函数条目后参考文档过期），已用 `tools/gen_api_doc.py --md` 重生成，**因为该检查比对的是
git 里的那一份，所以它必须在提交之后才会转绿**（提交后复跑为 0 failed）；`doc_audit funcs` **15 → 14**。

**结论分档：默认档 + 开档（`-DARKRTOS_USE_DYNLOAD=1`）都编译通过、告警 0、QEMU 端到端 PASS
（动态链接两条 PASS + `exit 105`、坏夹具 `exit 127`），未上板。**
`pmap`/页表相关的默认档读数不受影响（这一笔只动 `elf_load` 一个函数）。

#### 5.30.40 第三十五笔：`ovl_emit_node()` 按「路径 / 属性 / 子节点 / 收尾」拆成四段（2026-11-21）

`kernelsrc/src/arkrtos_fdt_overlay.c` 的 `ovl_emit_node()`（**189 行**，拆分后全仓最长的一个）
是 overlay 重拼的单节点主循环：拼路径 → 找命中的 fragment → 写 `BEGIN_NODE` → 逐 token 走基树
的属性和子节点、期间把 fragment 的覆盖插进去 → 写 `END_NODE`。四件事的边界在原代码里就是清楚的
（每一段都有自己的注释），所以这一笔是纯分段。

| 单元 | 管什么 | 行数 |
|---|---|---|
| `ovl_path_push()` | 把节点名接到父路径后面（就地写，父路径字节不动）：根的一级子节点写成 `/name`，其余写成 `父/name`；越界只报 `ERR_PATH` 不截断 —— 截断出来的路径会和别的节点撞名，撞名的后果是「删错了人」 | 33 |
| `ovl_emit_base_prop()` | 一条基树属性：被 fragment 覆盖过（含「删属性」那条长度 0）就不输出；`*pos` 进出都指在属性负载上，出函数时已越过 `ovl_pad4(len)` 并做越界检查 | 38 |
| `ovl_emit_base_child()` | 一条基树子节点：命中「删子节点」标记就 `ovl_skip_node()` 整段跳过，否则递归回调 `ovl_emit_node()`；两种情形调用方都只是「继续读下一条 token」，所以成功统一返回 `OK` | 18 |
| `ovl_emit_node_end()` | 收尾：先把 fragment 自己的子节点补上（必须排在基树子节点之后、`END_NODE` 之前），再写 `END_NODE` 并把游标交回调用方 | 19 |
| `ovl_emit_node()` 自己 | 深度闸门 → 拼路径 → 扫 fragment 取 `mi[]` → 写 `BEGIN_NODE`+名字 → 有 fragment 就先 `ovl_check_deletions()`（**先验后写**：写了一半再报错会留下半个 blob）→ 逐 token 分派（`NOP` / `PROP` / `BEGIN_NODE` / `END_NODE` / 其它报 `ERR_FORMAT`） | **40**（原 189） |

口径三条：① 「删子节点」和「递归重拼」都保留在同一个 helper 里，因为它们只差一个分支、
返回的又都是「继续循环」；把跳过那条单独拆出去会多一层、而判据是同一个 `ovl_is_delete_marker()`。
② 四个 helper 都是 `static` 且单调用点，`-O2` 全部内联回去，拆分不改变调用深度。
③ `ovl_emit_base_child()` 会递归回调 `ovl_emit_node()`，后者定义在它之后 ⇒ 加了一条前向声明
（第一次编译就是在这里报 `implicit declaration` + `static declaration follows non-static`，
不是「编译通过后才发现」）。

等价性：`default` 档两遍**同配置** clean 构建 —— text **448664 → 448632（−32 B）**、
`data` 448 / `bss` 5,128,740 / 镜像总长 31,462,512 B 全不变，两份 `.bin` 的 sha256 分别是
`06612070…`（拆前）/ `a36f19ad…`（拆后），**不是逐字节不变**；`arkrtos_fdt_overlay.c` 在该档
**0 条告警**（全量构建的 242 条来自其它既有文件）。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**，其中 `fdt` 项把 overlay 真跑了一遍
（`overlay absent-before → nodes 8→9 + model/props read back`、6 个板的 dtb 与 C 表逐行比、
`save+load nodes unchanged`、5 组自检），`devicetree-dtb` 6 个板全过；`tools/format_gate.py` OK 392。
`doc_audit funcs` **14 → 13**。

**结论分档：编译通过 + 告警 0 + QEMU 端到端 PASS（overlay 真拼出 9 个节点并读回），未上板。**

#### 5.30.41 第三十六笔：`arkrtos_fdt_overlay()` 按「前置 / 保留区 / 头 / 两遍」拆成五段（2026-11-21）

`kernelsrc/src/arkrtos_fdt_overlay.c` 的 `arkrtos_fdt_overlay()`（**174 行**）是 overlay 对外入口：
两块 blob 的解析与扫描 → 收 fragment → 量保留区 → 第一遍只算长度 → 写头 → 第二遍落笔并对账。
拆成五个 `static` 单调用点 helper，主函数只剩 71 行。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `ovl_prepare()` | 两块的 `ovl_blob_parse` + `ovl_blob_scan`；`tag` 只从 fragment 侧取，`__symbols__` / `__local_fixups__` 在这里就拒（`ERR_OVERLAY_TAG`）。放一起是因为「两次 parse 两次 scan、只有第二块要 tag」这条顺序本身就是契约 | 36 |
| `ovl_rsvmap_len()` | 基树 memory reserve map 的长度扫描（16 字节/项到全零结束项，越界 `ERR_RANGE`）；注释随函数搬走 | 23 |
| `ovl_write_head()` | 前导区清零 + 九个头字段逐项写（`totalsize` / `size_strings` 留 0 尾部回填）+ 搬 reserve map | 30 |
| `ovl_measure()` | 第一遍：`buf=0`、`write=0`，只把 `sp` / `str_used` 读出来做成 `struct_bytes` / `str_upper` | 22 |
| `ovl_write()` | 第二遍：落笔 + `overflow` 判红 + 两遍一致性对账（`sp` 必须等于 `off_struct + struct_bytes`、`str_used` 不得超过上界），不一致报 `ERR_OVERLAY` 而**不是**交一块长度对不上的 blob | 29 |
| `arkrtos_fdt_overlay()` 自己 | 参数/对齐检查 → `cap` 下限 → `ovl_prepare` → `ovl_collect_frags` → `nf==0` 判红 → 量保留区 → 算 `off_struct` → 填 `ovl_ctx_t` 的固定字段 → 两次 pass → 长度溢出/越 `cap` 两条 `ERR_OVERLAY_SIZE` → 回填 `totalsize` 与 `size_strings` → `*out_len` | **71**（原 174） |

三条口径：

Ⓐ 两次 pass 的「进 pass 前把 ctx 摆成哪一档」全在 helper 里，主函数不再直接碰 `buf` / `write` / `sp` / `str_base` / `str_used` / `overflow` —— 也就是说「两遍必须同构」这件事从「调用处小心翼翼」变成「只能通过 `ovl_measure` / `ovl_write` 进」。
Ⓑ 对账逻辑跟着第二遍走：`overflow` 与「两遍不一致」都是写档才可能发生的事，放回主函数会让人以为第一遍也可能触发。
Ⓒ 五个 helper 全是 `static` + 单调用点，`-O2` 全内联，拆分不改调用深度；这次 **text 反而涨了 288 B**（见下），不做「零成本」的假设。

等价性：`default` 档两遍同配置 clean 构建 text **448632 → 448920（+288 B）**、`data` 448 / `bss` 5,128,740 / 总量 31,462,512 B 不变；**`.bin` 的 sha256 在这里不能当等价证据**：镜像里 `kernelsrc/components/arkc/arkc_port_arkrtos.c` 把 C99 `__DATE__`/`__TIME__` 编进了数据（板级没有 RTC，它就当构建时间用），同一份源码连做两次 clean 构建 就有 8 字节不同（`cmp -l` 看下来差异全是 ASCII 数字）。所以本轮只报 `text`/`data`/`bss` 与 QEMU 读回，**text 涨了 288 B**，不报 sha；该文件 0 告警（全量 clean 构建的 242 条告警逐条比对前后完全一致，全部来自既有代码，与本次改动无关）。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；其中 `fdt` 项真跑 overlay（`tools/fdt_gate.py` 14 passed，overlay 一条读到「应用前无 overlay-demo-node / victim-child 在 / victim-prop=before / 控制台 115200 → 应用后 nodes 10→10、model=fdt-overlay-demo、current-speed 115200→9600、victim-child 查不到、victim-prop 已删」，另有 7 种坏法自检全判红）。`tools/format_gate.py` OK 392；`doc_audit funcs` **13 → 12**。

结论分档：**编译通过 + 告警 0 + QEMU 端到端 PASS（overlay 真被应用并逐项读回），未上板**。

#### 5.30.42 第三十七笔：`ovl_collect_one()` 拆成「目标 / __overlay__ / 落表」三段，并记一次被门禁抓到的真回归（2026-11-21）

`kernelsrc/src/arkrtos_fdt_overlay.c` 的 `ovl_collect_one()`（**169 行**）读一个 fragment 节点的
`target` / `target-path` 与 `__overlay__` 区间，再抄进 `frags[]`。拆成三段，主函数只剩 24 行。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `ovl_frag_target()` | fragment 自己的属性：`target = <&label>`（占位值必须 `0xffffffff`，标签经 `__fixups__` + 基树 `__symbols__` 换成路径）与 `target-path = "/abs/path"`；同时把游标交给调用方 | 84 |
| `ovl_frag_overlay()` | 子节点必须**恰好一个** `__overlay__`，其余一律拒绝；返回它的属性区起点与 `END_NODE` 令牌偏移 | 63 |
| `ovl_frag_store()` | 目标必须在基树里真的存在（不在不是「以后再补」而是错），再把路径与两处偏移抄进 `out[]` | 34 |
| `ovl_collect_one()` 自己 | 三段依次调用 | **24**（原 169） |

**这一笔的重点不是行数，是一次真回归。** 第一版把 `ovl_symbol_path(bb, ...)` 写成了
`ovl_symbol_path(fb, ...)` —— `bb` 是基树、`fb` 是 fragment，标签表在 fragment 侧，
但「标签 → 路径」要查的是基树的 `__symbols__`。编译零告警、`default` 档体积逐字节相同，
**只有 QEMU 里真去应用 overlay 才现形**：`fdt overlay` 报 `rc=-12 (target-path missing or not found)`，
后面十条读数全红（`tools/fdt_gate.py` 12 passed / 2 failed）。改成把 `bb` 一起传进
`ovl_frag_target()` 之后，`fdt overlay` 恢复 `overlay ok bytes=1176 nodes=10 (was 10) model=fdt-overlay-demo`。

口径：**拆「参数搬运」类函数时，编译器抓不到少传一个参数**（两个参数同类型 `const ovl_blob_t *`，
类型系统完全看不出区别），体积读数也抓不到（改前改后 text 一模一样）。这类拆分唯一的验收手段是
「带真实输入的端到端读数」，行数与体积都不是证据。

等价性：`default` 档两遍同配置 clean 构建 text **448920 → 448920（0 B）**、`data` 448 /
`bss` 5,128,740 / 总量 31,462,512 B 全不变；该文件 0 告警（全量 clean 构建 242 条告警前后逐条一致）。
**体积不变不能当等价证据**（见上），只说明没引入新代码。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/fdt_gate.py` **14 passed**
（overlay 一条：应用前无 `overlay-demo-node` / `victim-child` 在 / `victim-prop=before` / 控制台 115200
→ 应用后 nodes 10→10、`model=fdt-overlay-demo`、`current-speed` 115200→9600、`victim-child` 查不到、
`victim-prop` 已删；7 种坏法自检全红）。`tools/format_gate.py` OK 392；`doc_audit funcs` **12 → 11**。

结论分档：**编译通过 + 告警 0 + QEMU 端到端 PASS（其中一次真回归就是它抓出来的），未上板**。

#### 5.30.43 第三十八笔：`elf_measure_image()` 拆出 TLS 分支与 PT_LOAD 分支，并记一次 API 文档生成器的口径（2026-11-21）

`kernelsrc/src/arkrtos_elf.c` 的 `elf_measure_image()`（**169 行**）逐条读程序头，把 TLS 尺寸、
装载区间 `[lo, hi)`、`segs`、`AT_PHDR` 地址一次算出来。两个分支各自成函数，主函数只剩遍历与装配。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `elf_tls_t` | 量到的静态 TLS 模板四字段（`va` / `filesz` / `memsz` / `align`），只在 `ARKRTOS_USE_ELF_TLS=1` 时存在 | typedef |
| `elf_measure_tls()` | PT_TLS 分支：`filesz > memsz` 拒、`vaddr + bias` 回绕拒、`p_align` 必须是 0/1 或 2 的幂且 `vaddr` 真对齐、**第二个 PT_TLS 直接拒**（"只允许一个"是格式约束，不能挑一个用），并把 `hi` 推到 TLS 块末尾 | 56 |
| `elf_measure_load()` | 一个 PT_LOAD：算出 `[a0, a1)`、收窄 `lo/hi`、判 phdr 是否落在这段的 file range 里（这才让 `AT_PHDR` 有对应的内存）、`segs++`；回绕全用减法写 | 47 |
| `elf_measure_image()` 自己 | 参数检查 → 循环读 phdr → 按 `p_type` 分派 → `segs==0 || hi<=lo` 判红 → 装配 `p_img` | **97**（原 169） |

两条口径：

Ⓐ TLS 的「只允许一个」判据靠 `elf_tls_t` 的 `memsz` 是否已非 0 —— 调用方进来前把四个字段清零，
所以读到非 0 就是"已经见过一个"，不需要额外传 `seen` 标志。
Ⓑ TLS 分支整体在 `#if (ARKRTOS_USE_ELF_TLS == 1)` 里，默认档编不到它；验收只能靠 `tools/elf_gate.py`
的 H1 两项（LA64 两 TU 带 `-DARKRTOS_USE_ELF_TLS=1` 编得过、RV32 带着必须被 `#error` 拒）。
这与第三十四笔的教训同源：**默认档 `make` 过不等于这段代码编过。**

顺带记一条 API 文档生成器的口径：`tools/gen_api_doc.py --md` 是把**紧挨着函数定义的那段文档注释**
认成该函数的说明。第一版把 `/** @brief 量一遍段表… */` 留在了 `elf_tls_t` 的 `#if` 上方，
生成出来的标题就变成了 `#if (ARKRTOS_USE_ELF_TLS == 1)`，而 `elf_measure_image` 的说明整段丢失
（`ci_gate` 立刻报 `FAIL api-doc reference is stale`）。正确位置是**文档注释紧贴函数签名**，
类型与 helper 都插在这段注释之前。

等价性：`default` 档两遍同配置 clean 构建 text **448920 → 448920（0 B）**、`data` 448 /
`bss` 5,128,740 / 总量 31,462,512 B 全不变；该文件 0 告警（全量 clean 构建 242 条告警前后逐条一致）。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**（含 `elf` 项的默认档零符号 +
LA64 开档 + TLS 两项、`dynlink` 项的 QEMU 两条 PASS 与坏夹具 `exit 127`、`realbin` 项的探针语义）；
`tools/elf_gate.py` **ELF-GATE OK**（TLS H1 两 TU 编得过 / H2 RV32 被拒 / H3 接线与自检）；
`tools/format_gate.py` OK 392；`doc_audit funcs` **11 → 10**。

结论分档：**编译通过（含 TLS 开档）+ 告警 0 + QEMU 端到端 PASS，未上板**。

#### 5.30.44 第三十九笔：`arkrtos_smp_sched_switch()` 按「交还 / 保存 / 选择 / 落地」拆成四段（2026-11-21）

`kernelsrc/src/arkrtos_smp_sched.c` 的 `arkrtos_smp_sched_switch()`（**160 行**）是次核每拍从汇编
进来的调度入口：核号合法性 → 处理 CPU0 的交还请求 → 存当前帧 → 选下一个 → 落地并返回要恢复的栈指针。
四段各自成函数，主函数只剩「顺序 + 两条短路」。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `sms_take_handoff()` | 处理 CPU0 登记的「把这个任务还我」：任务正在跑（`h == cur`）就先存帧、交还所有权、本核置空闲并置 `handed`；还在本核队列里就直接摘出来交还；**既不在跑也不在队列里就保留登记、下一拍再看**（不吞请求）；坏下标清掉。`*p_cur` / `*p_handed` 反映处理后的状态 | 45 |
| `sms_save_cur()` | `handed != 0` 时直接返回（交还那一路已经存过帧，**再存一次会把空闲帧写进任务控制块**）；否则存 `stack_ptr`（含栈水位），并把被切下的任务放回队尾保证同优先级轮转；`cur < 0` 时存 `s_idle_sp` | 27 |
| `sms_pick_next()` | `sms_pick()` + 三条短路，短路时**自己解锁**并置 `*p_stay = 1`：①空闲→空闲原地返回（少计一次假切换，`s_switch_cnt` 不在纯空闲时每拍自增；只对 `handed == 0` 成立）；②当前任务仍最优且时间片未尽（`s_slice--`）；③时间片用尽后挪队尾重选仍是自己 | 52 |
| `sms_apply_next()` | 落地：上 CPU 的从队列摘掉、重置时间片、`s_switch_cnt++`、写 `s_cur`；取不到任务则 `s_cur = NIL` 并返回 `s_idle_sp`；**统一在这里解锁** | 28 |
| `arkrtos_smp_sched_switch()` 自己 | 核号非法直接返回 → 懒初始化 → 加锁 → 四段顺序调用 → `ret == 0` 防御 | **39**（原 160） |

两条口径：

Ⓐ 锁的所有权在拆分后必须**每条路径各解锁恰好一次**。原函数在 5 处 `arkrtos_spin_unlock` 后返回，
拆分后其中 3 处（三条短路）搬进 `sms_pick_next()`，由 `*p_stay` 告诉调用方「已经解锁，别再动」；
剩下两处（正常落地与 `ret == 0` 防御）分属 `sms_apply_next()` 与主函数。`stay` 标志不是为了省一次
判断，而是为了**让"谁解的锁"在代码里可读**——否则漏解或双解都不会被编译器抓到，只会表现为某一拍
死锁或计数器跳变。
Ⓑ `handed` 这条状态跨了第 1、2、3 三段：它同时关掉「再存一次帧」和「空闲→空闲短路」两件事。
拆函数时必须把它显式传递，不能只在某一段里读 `s_cur == NIL` 来替代——交还那一路 `cur` 刚被置成 NIL，
但 `frame` 属于被交还的任务，必须走 `else` 分支把 `s_idle_sp` 取回来，语义完全不同。

等价性：`default` 档两遍同配置 clean 构建 text **448920 → 448920（0 B）**、`data` 448 /
`bss` 5,128,740 / 总量 31,462,512 B 全不变；该文件 0 告警（全量 clean 构建 242 条告警前后逐条一致）。
`.bin` 的 sha256 不作为等价证据（`arkc_port_arkrtos.c` 内嵌 `__DATE__` / `__TIME__`，两次 clean 构建必差几字节）。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**（含 `smp` 项的 host 计数 + 5 个 ARM
ON-path 文件 + LA64 pin U/V/W + RV32 pin X）；`tools/smp_gate.py` **SMP-GATE OK**（LA64 自检会
抓掉 TID 读、SMP=0 对象里不许有 SMP 标记、AMP 开关关掉时 `.text=0` 且无符号）；
`tools/format_gate.py` OK 392；`doc_audit funcs` **10 → 9**。

端到端：`make -f example/qemu_loongarch64/Makefile run`，串口 `out.txt` 跑到 `round=58`
（被 90 s 窗口 `timeout` 杀掉，`exit=124` 属正常停法），全程 `svc_ok`、A/B 双任务按
`d_tick≈1000 / d_ms≈500` 稳定轮转，`grep -icE "fail|panic|error|assert"` = **0**。

结论分档：**编译通过 + 告警 0 + QEMU 端到端 PASS，未上板**（QEMU 算上板，不是真机）。

#### 5.30.45 第四十笔：`arkrtos_linux_abi_try()` 拆成号表 / 分派 / 收尾三段（2026-11-21）

`kernelsrc/src/arkrtos_linux_abi.c` 的 `arkrtos_linux_abi_try()`（**158 行**）是 Linux ABI 的
总入口：判号 → 记号表直方图 → 一个 25 条 `case` 的分派表 → 返回值分类 → 写回并投递信号。
三段各自成函数，主函数只剩「顺序 + 一条提前返回」。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `lx_note_nr()` | 号表直方图：命中就 `hits++`，没命中就占一个槽（`LX_NR_SLOTS` 满了记 `s_nr_overflow`）。整段在 `ARKRTOS_LINUX_ABI_SYSSTAT == 1` 门内 | 26 |
| `lx_dispatch()` | 25 条 `case` 的分派表，逐条调处理函数写 `*p_rc`；**只有 exit 族与 default 会返回 1**（这两种「返回码已写、调用到此为止」，原函数里是 `return 1`） | 95 |
| `lx_finish()` | 返回值分类（`PTR_DONE` / `EXEC_DONE` 直接计 `HANDLED` 并返回，不写回；`-EFAULT` / 其它负数 / 正数分三档计数）→ `lx_ret()` → `lx_signal_deliver()` 投递点 | 32 |
| `arkrtos_linux_abi_try()` 自己 | 取号 → 非 Linux 号返回 0 → 记直方图 → `lx_dispatch` 判「已结束」→ `lx_finish` → 返回 1 | **22**（原 158） |

三条口径：

Ⓐ 「这次调用已经结束」在拆分后只能靠**返回值**带出来。原函数用 `return 1` 从 `case` 里直接冲出
整个函数；`lx_dispatch` 是独立函数，`return 1` 只回到它自己的调用点，于是改成「返回 1 = 调用方也
直接 return 1；返回 0 = 走收尾」。`rc` 通过 `*p_rc` 带出，因为 exit 与 default 两条路根本不产生
「返回值分类」这件事。
Ⓑ 号表直方图**必须留在分派之前**，不能挪进 `lx_dispatch`。走到 default 的 `-ENOSYS` 与 exit 提前
返回的调用也要进直方图，否则「程序卡在哪一号」恰好在最关键的那几号上看不见——这条在原注释里写着，
拆函数时按位置保留，不只是搬代码。
Ⓒ `lx_finish()` 的两处提前返回（`PTR_DONE` / `EXEC_DONE`）都是「处理函数已经写完，这里不许再覆盖」，
所以它对调用方没有返回值可给，返回 `void`；把这两条写成「返回一个码让主函数再判断」会把"不许覆盖"
变成一次可被写错的判断。

等价性：`default` 档两遍同配置 clean 构建 text **448920 → 449128（+208 B，唯一一处涨）**、
`data` 448 / `bss` 5,128,740 / 总量 31,462,512 B 全不变；该文件 0 告警（全量 clean 构建 242 条告警
前后逐条一致）。+208 B 与第 36 笔的 +288 B 同源：`-O2` 下多了一层不内联的调用边界与 `*p_rc` 的
栈中转，**这是拆分的真实成本，不假装零成本**。`.bin` 的 sha256 仍不可作等价证据（`__DATE__` /
`__TIME__`）。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/linux_abi_gate.py`
**13 passed / 0 failed**（含 `off-zero-symbols` 关档零符号、`compile-on` 8 个 TU 在 LA64 真板级下
全编过、`syscall-table` 25 条号与 asm-generic 逐条相等、`abi-split-la64/rv32` 两个方向的号段分家、
`qemu-readout` 的 14 条 check / PASS 行 / `exit code 7` / 管道 14 条与信号 31 条 check（退出码 55 / 138）
全在、`parser-selfcheck` 8 种坏法全被判红）；`tools/format_gate.py` OK 392；
`doc_audit funcs` **9 → 8**。

结论分档：**编译通过 + 告警 0 + QEMU 端到端 PASS，未上板**（QEMU 算上板，不是真机）。

#### 5.30.46 第四十一笔：`fdt_walk_path()` 拆出 BEGIN_NODE 分支，并复用既有的 `fdt_build_path()`（2026-11-21）

`kernelsrc/src/arkrtos_fdt.c` 的 `fdt_walk_path()`（**148 行**）沿 structure block 走一遍，
按 `BEGIN_NODE` 累栈、`END_NODE` 退栈，到「深度正好是路径组件数 + 1」那一层逐段比名字，
对上就写 `*out` 与绝对路径。循环体里最长的一支是 `BEGIN_NODE`（近 70 行），抽成函数后
主循环只剩「读 tok → 分派」。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `fdt_walk_begin()` | 判深度上限（`> MAX_DEPTH + 1` 报 `ERR_DEPTH`）→ 名字入栈、`*p_depth` 回写 → 深度不等于 `n + 1` 就返回继续 → 逐段 `fdt_streq` 比名字 → 对上则填 `*out`（`name` / `depth - 1` / `props_off` / `n_props`）与路径，`*p_hit = 1` | 49 |
| `fdt_walk_path()` 自己 | 初始化 + `fdt_split_path` + 主循环（5 个 tok 分支，`BEGIN_NODE` 只剩 9 行调用）+ 5 处 `return ERR_FORMAT` | **97**（原 148） |

三条口径：

Ⓐ 路径拼装**不再复制一份**，改为调用文件里早就有、语义相同的 `fdt_build_path()`（原本只被
`fdt_walk_match` 用）。两段逻辑逐条对得上：`k` 从 2 起跳过根节点（名字为空）、每次先写 `/`、
名字逐字符搬、容量不够返回 `ERR_ARG`。差别只有一处：`fdt_build_path` 入口有
`if((pathbuf == 0) || (pathcap < 2u)) return ARKRTOS_FDT_ERR_ARG;`，而原来的内联版没有这条
——`pathcap == 0` 时 `pathcap - 1u` 会回绕成 `0xFFFFFFFF`，判断永不成立，然后往 `pathbuf[0]`
写。所以这次复用是**收紧**（不再有那条越界路径），不是放宽；现有调用点一个是
`arkrtos_fdt_node_by_path` 传 `(0, 0u)`（走不到这段），另一个传的是真实缓冲与容量。
`fdt_build_path` 定义在 `fdt_walk_path` 之后，因此加了一行前置声明，函数体未动。
Ⓑ `*p_hit` 而不是「返回 1 表示找到」：`ARKRTOS_FDT_ERR_*` 全是负数，用返回值区分
「找到了」和「错误」也能写，但那会把 `OK(0)` 同时当成「没找到、继续」——`fdt_walk_begin`
的绝大多数返回都是这个 0。用独立的出参让「没出错」和「对上了」在调用点各是一个判断。
Ⓒ 顺带修掉一处缩进：原内联块里 `    for(k = 2u; ...)` 少了 4 格缩进（`format_gate` 的
F1–F4 不管缩进，所以它一直没红）。这段整体搬进 `fdt_build_path` 后自然消失。

等价性：`default` 档两遍同配置 clean 构建 text **449128 → 449112（−16 B，唯一一处降）**、
`data` 448 / `bss` 5,128,740 / 总量 31,462,512 B 全不变；该文件 0 告警（全量 clean 构建 242 条
告警前后逐条一致）。`-16 B` 来自删掉一份重复的路径拼装，但仍然保留了一层不内联的调用。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/fdt_gate.py`
**14 passed / 0 failed**（含 `qemu-overlay` 真跑：应用前无 `overlay-demo-node`、`victim-child`
在、`victim-prop=before`、控制台 115200；应用后 nodes 10→10、`model=fdt-overlay-demo`、
`current-speed` 115200→9600、`victim-child` 查不到、`victim-prop` 已删；另含 7 种坏法自检全红）；
`tools/format_gate.py` OK 392；`doc_audit funcs` **8 → 7**。

结论分档：**编译通过 + 告警 0 + QEMU 端到端 PASS，未上板**（QEMU 算上板，不是真机）。

#### 5.30.47 第四十二笔：`arkrtos_fdt_stdout_resolve()` 拆成设备名 / 路径 / 选项三段（2026-11-21）

`/chosen/stdout-path` 有两种形态：句柄（`<&uart0>`，4 字节）与字符串
（`serial0:115200n8`）。`arkrtos_fdt_stdout_resolve()`（**142 行**）把字符串这一路
「取设备名 → 设备名变路径（别名或绝对路径）→ 解析冒号后的选项」全写在了一起。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `fdt_stdout_dev()` | 取 `:` 之前那段设备名，写满 `cap` 报 `ERR_PATH`、空串也报 `ERR_PATH`，长度经 `*p_used` 带出 | 24 |
| `fdt_stdout_dev_path()` | 以 `/` 开头就当路径直接 `node_by_path` 校验后复制；否则先 `arkrtos_fdt_alias` 再校验再复制，并置 `from_alias`。**别名查不到时把 `NOT_FOUND` 原样报出**，不混成路径错误 | 36 |
| `fdt_stdout_opts()` | 冒号之后：数字 `<= 8` 当数据位、`> 8` 当波特率，各只认第一个（第二个报 `ERR_PATH`）；`n` / `o` / `e` 当校验位，同样只认第一个 | 56 |
| `arkrtos_fdt_stdout_resolve()` 自己 | 六字段清零 → 取原始属性 → 句柄形式（`len == 4 && v[3] != 0`）→ 字符串形式（末字节必须 `\0`）→ 三段顺序调用 | **54**（原 142） |

两条口径：

Ⓐ 三段都返回 `ARKRTOS_FDT_ERR_*` / `OK`，主函数只做「`rc != OK` 就返回」的串联，
没有任何一段靠「返回 1 表示成功」之类的二值约定——这条路径上错误码本来就已经是
「0 = 成功、负 = 具体原因」，改写会让调用点丢信息。
Ⓑ `fdt_stdout_dev_path()` 的别名分支要**保住 `NOT_FOUND` 的语义**：`arkrtos_fdt_alias`
查不到不是路径写错，而是设备名不存在；原代码用三元把两者分开，抽函数时按原样保留
（这一条在 `tools/fdt_gate.py` 的坏法自检里能看出后果：设备名不存在与路径不存在
报的 reason 不同）。

等价性：`default` 档两遍同配置 clean 构建 text **449112 → 449064（−48 B）**、
`data` 448 / `bss` 5,128,740 / 总量 31,462,512 B 全不变；该文件 0 告警（全量 clean
构建 242 条告警前后逐条一致）。

端到端：这一条**有真实覆盖**——`dts/qemu_loongarch64.dts` 里
`stdout-path = "serial0:115200n8"`，正好是「别名 + 冒号选项」这条路。
`tools/fdt_gate.py` 的 `qemu-console` 读开机串口得到
`path=/soc/serial@1fe001e0 baud=115200 clk=25000000 reason=ok match=1`：
别名 `serial0` 解到了节点、`115200` 进了 baud、`n` 与 `8` 被吃下、`from_alias` 置位；
同一门禁的 `console-selfcheck` 用 4 种坏样本（改路径 / 改 reason / 改 match / 缺行）
证明这条检查会红。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/fdt_gate.py`
**14 passed / 0 failed**；`tools/format_gate.py` OK 392；`doc_audit funcs` **7 → 6**。

结论分档：**编译通过 + 告警 0 + QEMU 端到端 PASS，未上板**（QEMU 算上板，不是真机）。

#### 5.30.48 第四十三笔：`arkrtos_fdt_res_check()` 拆成 timebase / 找定时器 / 核对定时器三段（2026-11-21）

`kernelsrc/src/arkrtos_fdt_res.c` 的 `arkrtos_fdt_res_check()`（**130 行**）做三件事：
读 `/cpus/timebase-frequency` 并与期望比、按 `compatible` 在全树里找定时器节点、
把找到的节点抄进 `out` 再核对 `reg` / `clock-frequency`。三段各自成函数。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `res_check_timebase()` | 取 `/cpus` → 读 `timebase-frequency` 填 `out->timebase` / `has_timebase` → 只在调用方期望非 0 时核对，缺属性报 `NO_TIMEBASE`、不等报 `TIMEBASE_DIFF` | 29 |
| `res_find_timer()` | `iter_init` → 逐节点跳过 `disabled` → 读 `compatible` 比串 → 命中置 `found` 跳出；返回 `OK` / `NO_TIMER` | 33 |
| `res_check_timer()` | 填 `has_timer` / `timer_node` / `timer_compat` / `timer_reg` / `timer_hz`，再按期望核对 `reg` 与 `clock-frequency`（各分「缺属性」与「值不等」两种原因码） | 48 |
| `arkrtos_fdt_res_check()` 自己 | 空指针检查 → 九个字段清零 → `fdt_ready` → 三段顺序调用，`timer_compat == 0` 时在第 2 段之前就返回 | **44**（原 130） |

两条口径：

Ⓐ 三段之间**不是**「返回 0/1」的链路，而是 `ARKRTOS_RES_*` 原因码的直传：主函数只做
`rc != ARKRTOS_RES_OK` 就返回。这一点比拆法本身重要——`arkrtos_fdt_res_reason()` 就是把这些码
翻成串口上的 `timebase_mismatch` / `timer_reg_absent` 之类，中间任何一段把码合成布尔值，串口上
就只剩「有一项不对」。
Ⓑ `expect->timer_compat == 0` 的提前返回**必须在第 1 段之后、第 2 段之前**：调用方只要
「核 timebase、不核定时器」时（本板就是这种），第 2、3 段一次都不能跑。抽函数时这条位置不变，
否则 `has_timer` 会被错误置位。

等价性（**这一条要特别说明**）：

- 该文件默认档不编入：`ARKRTOS_USE_FDT_RES` 默认 **0**（`board/qemu_loongarch64/arkrtos_board_config.h`），
  所以 `default` 档两遍同配置 clean 构建 text **449064 → 449064（0 B）**、`data` 448 /
  `bss` 5,128,740 全等——**0 B 不代表代码对**，它只说明这段没进镜像。
- 因此另做**开档**读数：`make ... EXTRA_CFLAGS=-DARKRTOS_USE_FDT_RES=1`（换档必须 `make clean`，
  构建系统不跟踪 `EXTRA_CFLAGS`），两遍 clean 构建 text **450344 → 450328（−16 B）**，
  该文件 0 告警、全量告警 242 条前后一致。
- `.bin` sha256 仍不可作等价证据（`__DATE__` / `__TIME__`）。

端到端（**开档**）：

- 默认调用形态（`timer_compat = 0`）：串口 `[fdt] res timebase=100000000 expect=100000000
  timer=(none described) rc=ok` —— 第 1 段走通并核对成功。
- 为了覆盖第 2、3 段，临时把 `example/qemu_loongarch64/main.c` 的 `rexp.timer_compat` 从 `0`
  改成 `"ns16550a"`（树里 `uart0` 的 compatible）跑一次，得
  `[fdt] res timebase=100000000 expect=100000000 timer=serial@1fe001e0 rc=ok`：
  遍历、跳过 disabled、比串、填 `timer_node` 全通。**该临时改动已还原（字节级写回，CRLF 1707 不变），
  未进本次提交**。
- 未覆盖：`expect_timer_reg` / `expect_timer_hz` 非 0 的两条「值不等」分支，以及 `NO_TIMER` 与
  `TIMER_NO_REG` / `TIMER_NO_HZ` 的报错路径（要造缺属性或缺节点的树才触发）。这一段是简单比较，
  风险低，但**如实登记为未验证**。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/format_gate.py` OK 392；
`doc_audit funcs` **6 → 5**。

结论分档：**编译通过（含开档）+ 告警 0 + QEMU 端到端 PASS（timer 分支用临时改配置覆盖后已还原），
未上板**（QEMU 算上板，不是真机）。

#### 5.30.49 第四十四笔：`arkrtos_mbuf_send_internal()` 拆出整条写入与阻塞重试（2026-11-21）

`kernelsrc/src/arkrtos_sbuf.c` 的 `arkrtos_mbuf_send_internal()`（**125 行**）是消息缓冲的发送口：
先做参数与容量上限检查，再走「空间够就直接写 / 不够就阻塞重试」两条路。两条路里**同一段写入代码
出现了两次**（写长度头 → 写正文 → 唤醒一个读者 → 打 trace），阻塞重试那半边还塞着一个带
`for(;;)` 的多出口循环，每条出口各自恢复中断。这次把这两块各抽出来。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `mbuf_write()` | 一次写完整条：`put_bytes(hdr)` → `put_bytes(buf)` → `wake_one(recv_waiters)` → `mdk_trace_arkrtos_obj_signal` | 12 |
| `mbuf_wait_put()` | 阻塞重试循环：空间不足则 `waiter_add` → 核处理器上下文（报 `WOULDBLOCK`）→ 按剩余超时算 `remain`（0 则摘等待者报 `TIMEOUT`）→ `trace_wait_obj` + `task_block_in_critical` 睡下 → 醒来判令牌位与错误码 → 复核空间，够就 `mbuf_write` 后 `SWITCH_TASK` 返回 `OK`，不够就整条重来 | 65 |
| `arkrtos_mbuf_send_internal()` 自己 | 空指针 / `len <= 0` 报 `PARAM` → `sbuf_get` → `need > ARKRTOS_SBUF_SIZE` 立刻报 `NOSPC`（不是重试）→ `put_hdr` → `DISABLE_IRQ` → 快速路径 `mbuf_write` 后 `ENABLE_IRQ` + `SWITCH_TASK` → `timeout_ms == 0` 报 `TIMEOUT` → 无当前任务报 `PARAM` → 算 `start_tick` / `deadline` → 交给 `mbuf_wait_put` | **62**（原 125） |

三条口径：

Ⓐ **`mbuf_write` 抽出来是为了去掉重复，不是为了减行数**：两处写入逐字相同（顺序也是语义的一部分——
先头后体，读者看到头就一定能读到完整正文）。把「先写头再写体」这两句挤到一个函数里，改动后
再有人动其中一处也不会出现一边写了头一边没写头的分叉。
Ⓑ **`mbuf_wait_put` 的每条返回路径自己恢复中断**，这是原代码的约定：调用进来时已 `DISABLE_IRQ`
且已持临界区，所以这个函数里**没有一条 `return` 可以不 `ARKRTOS_ENABLE_IRQ()`**。抽函数时这条
最容易漏——因为编译器不会报「少恢复一次中断」，它只会变成某个偶发的死机。本函数 6 条出口全部
带 `ARKRTOS_ENABLE_IRQ()`，逐条对照过。
Ⓒ **`ret < 0` 必须排在「令牌位检查」之前**：`arkrtos_task_block_in_critical` 的 −1（睡不下）
在二进制上每一位都是 1，单看 `ARKRTOS_WAKE_HANDOFF` 位会被判成「拿到了交接」，于是原地重睡到死。
原代码这里就是 `if((ret < 0) || ((ret & ARKRTOS_WAKE_HANDOFF) == 0))`，拆函数时**顺序原样搬**，
并补了一句注释说清为什么不能调换。

体积（`default` 档，`loongarch64-newlib-elf-size`，两遍同配置 `make clean` 构建）：

- 改前 `text 449064 / data 448 / bss 5,128,740`
- 改后 `text 449064 / data 448 / bss 5,128,740`
- **0 B 差异**。这次是纯拆分 + 复用合并（把两处重复写入合成一处），编译器把内联后的结果优化成
  同样的形状，符合预期；**0 B 不能反过来当作「代码等价」的证明**（见下一条）。
- 全量告警 242 条前后一致，`arkrtos_sbuf.c` 0 告警。
- `.bin` sha256 **不可**作等价证据（`arkc_port_arkrtos.c` 里内嵌 `__DATE__` / `__TIME__`）。

端到端：**本笔没有任何功能级覆盖**，这一点必须写死。

- `example/qemu_loongarch64/main.c` **不调用 mbuf API**，QEMU 启动链路走不到这段代码；
- `tools/sbuf_host_gate.py`（169 checks）**完全不含 mbuf 字样**（`grep mbuf` 命中 0 条），它覆盖的是
  流式 `put` / `get` / 等待者与超时，不覆盖「整条消息 + 长度头」这条路径；
- `tools/mq_host_gate.py`（108 checks）是消息队列的宿主测试，与 mbuf 无关；
- 唯二调用点是 SVC 分派（`arkrtos_task.c`）与用户态 API。

所以本笔结论分档为：**编译通过（0 告警）+ 静态门禁（`sbuf_host_gate` 169 / `mq_host_gate` 108 /
`ci_gate` 57 / `format_gate` 392 全绿），行为未验证**。不得写成「行为等价」或「端到端通过」。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/format_gate.py` OK 392；
`doc_audit funcs` **5 → 4**（`arkrtos_mbuf_send_internal` 125 从清单消失）。

#### 5.30.50 第四十五笔：`arkrtos_sched_check()` 拆成任务表 / 位图 / 扫描三段（2026-11-21）

`kernelsrc/src/arkrtos_task.c` 的 `arkrtos_sched_check()`（**123 行**）是调度器的一致性自检：
把「就绪位图 / 就绪桶链表」这份第二副本与任务表真实状态比一遍，再由一次全表线性扫描选出最优
任务，与位图选出的最优比一次；返回不一致条目数（0 = 一致）。三段各自成函数。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `sc_task_list()` | 逐任务比「就绪链标记 `ready_next != NIL`」与「状态 READY/RUNNING」，顺带查「声称在延时链上却不在链头」，并把就绪任务数经 `*p_cnt` 带出 | 40 |
| `sc_bitmap()` | `ready_map[8]` / `ready_group` / `ready_head[]` 三方对照（字位图非空但组位未标、组位说有但字位图为空、位图说有但桶空、桶头任务优先级对不上） | 39 |
| `sc_scan_best()` | 全表线性扫描选最优（优先级号小者胜、同优先级按 `touch_tick` 早者胜），返回任务号，无就绪返回 −1 | 39 |
| `arkrtos_sched_check()` 自己 | `sc_task_list` → `sc_bitmap` → `cnt != ready_total` 加一 → `sc_scan_best` 与 `ready_top()` 比对（一侧为空 / 优先级不同） | **30**（原 123） |

三条口径：

Ⓐ **SMP 跳过逻辑在两段里都必须原样保留**：`arkrtos_smp_sched_owned_by_other()` 的 `continue`
同时出现在 `sc_task_list` 与 `sc_scan_best` 里，而且**两处语义不同**——前者是「次核任务不在
CPU0 就绪链上，这是设计不是不一致」，后者是「次核任务的优先级不参与本核决策」。少任一处都会
在迁出一个任务后立刻报 bad。注释也随代码一起搬，没有合并成一句。
Ⓑ **返回值是「不一致条目数」不是布尔**：三段各自累加，主函数用 `bad = …; bad += …;` 相加。
shell 的 `sched` 命令把它当计数打印（`%d mismatch(es)`），`audit` 只判 `!= 0`。拆函数时如果
把任一段改成返回 0/1，计数信息就丢了，shell 上的「几处不一致」会退化成「有不一致」。
Ⓒ **`cnt` 用出参而不是全局**：`sc_task_list(&cnt)` 把就绪任务数带出来，主函数再与
`arkrtos_ready_total` 比。这里不引入任何新的静态变量——`arkrtos_sched_check` 本身是可以被
并发调用的自检（`arkrtos_audit` 会调），一旦有全局中间态就多一条竞态。

体积（`default` 档，`loongarch64-newlib-elf-size`，两遍同配置 `make clean` 构建）：

- 改前 `text 449064 / data 448 / bss 5,128,740`
- 改后 `text 449064 / data 448 / bss 5,128,740`
- **0 B 差异**，全量告警 242 条前后一致。纯拆分、编译器内联后形状相同，符合预期——但 **0 B
  不代表行为等价**，真正的等价证据在下一段。

端到端（QEMU，`tools/qemu_shell_drive.py` 走 TCP 串口喂命令）：

```
sched | sched: consistent
audit | audit: all structures self-consistent
```

实跑输出：

- `sched` → `ready=1 top=9` + `sched: consistent (bitmap/links == task table)`
- `audit` → 32 项检查里 `tasks: ok` / `sched: ok`，`checks=32 bad_groups=0 mask=0x00000000
  first_offender=4294967295`，`audit: all structures self-consistent`
- 驱动脚本收尾 `SHELL-DRIVE OK: 2 条命令全部符合预期`（退出码 0）

也就是说这条自检在真跑起来的调度器上确实返回 0，且两处 SMP 跳过在单核 QEMU 上空转不误报。
**未覆盖**：`sc_bitmap` 的四种 bad 分支与 `sc_scan_best` 的次优先级分支都只在「结构真被破坏」
时才走到，本次没有造破坏去触发它们（要故意改位图/桶头才触发），如实登记为未验证。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/format_gate.py` OK 392；
`doc_audit funcs` **4 → 3**。

结论分档：**编译通过（0 告警新增）+ QEMU 端到端 PASS（`sched` / `audit` 双命令）+ 静态门禁全绿，
未上板**（QEMU 算上板，不是真机）。

> 门禁偶发（如实记录）：本笔有一次 `ci_gate` 在 `fdt` 组报了 `overlay-selfcheck` 的「坏样本没改到东西」（13 passed / 1 failed）；单跑 `tools/fdt_gate.py` 为 **14 passed / 0 failed / FDT-GATE OK**，紧接复跑 `ci_gate` 恢复 **57 / 0 / 0**。该组是 QEMU 串口时序相关的既有偶发，与本笔改动（`arkrtos_task.c`）无因果关系，未做任何代码改动。

#### 5.30.51 第四十六笔：`lx_signal_deliver()` 拆成选信号 / 铺帧 / 装上下文三段（2026-11-21）

`kernelsrc/src/arkrtos_linux_abi.c` 的 `lx_signal_deliver()`（**122 行**）在每次系统调用返回前
把待投递信号落成 Linux 风格的信号帧。一条链是三件事：**选哪个信号并按 action 处理「忽略 /
默认动作」**、**把整帧铺到用户栈**、**把 trap 帧的 era/sp/ra 改成进 handler**。中间每一步都有
「这次不投递、pending 位放回去、下次再试」的出口，所以原函数有十几条 `return`。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `lx_sig_pick()` | `pending & ~blocked` → 选最低号 → 清 pending 位 → `SIG_IGN` / `SIG_DFL`+默认忽略 / 默认停止 直接记数返回；其余默认动作结束任务（`128 + signo`），返回 1 | 55 |
| `lx_sig_build_frame()` | 端口帧长非法 → 拒（`refused`）；handler 取不到指令 → 拒；端口给不出帧址 → 拒；否则填头（magic / signo / blocked / fbytes）→ **先导出被打断点快照** → `copy_to_user` 整帧。任一处失败都还回 pending 位 | 60 |
| `lx_sig_install()` | 屏蔽字 |= 本信号 + `sa->mask`（再剔掉 KILL/STOP）→ 三次 `set_user_reg`（ERA / SP / RA）→ `SVC_RET(sig)` + `delivered++`；`set_user_reg` 任一失败回滚屏蔽字并还 pending 位 | 22 |
| `lx_signal_deliver()` 自己 | `lx_sig_pick` → `sig <= 1` 原样返给调用方 → `lx_sig_build_frame` 失败返 0 → `lx_sig_install`（返回值丢弃，失败已自回滚）→ 返 0 | **22**（原 122） |

三条口径：

Ⓐ **`sig <= 1u` 的两种语义必须原样交回调用方**：0 = 无事可做（含被忽略、默认忽略、默认停止），
1 = **任务已被默认动作结束**。返回 1 时调用方（`arkrtos_linux_abi_try`）不许再碰上下文——它已经
退掉了。所以 `lx_sig_pick` 用「0 / 1 / 信号号(>1)」三态返回值，**不能压成布尔**。
Ⓑ **铺帧顺序不能调换**：必须先 `frame_export`（把被打断点的现场存进内核缓冲）**再**写
`era/sp/ra`。反过来存下来的是处理函数的现场，`sigreturn` 就回不到被打断点。这条注释从原函数
整段搬进 `lx_sig_build_frame`，一个字没删。
Ⓒ **三个 helper 全部「失败就自己回滚」**：pending 位与屏蔽字都由出错的那一层还原，主函数不再
判断——所以主函数里 `(void)lx_sig_install(...)` 丢弃返回值是安全的，不是漏判。原函数里也有
同样三条「还 pending 位」的路径，只是散在一个函数里。

体积（`default` 档，`loongarch64-newlib-elf-size`，两遍同配置 `make clean` 构建）：

- 改前 `text 449064 / data 448 / bss 5,128,740`
- 改后 `text 449048 / data 448 / bss 5,128,740`
- **−16 B**，全量告警 242 条前后一致、本文件 0 告警。

端到端（**这一笔的覆盖是这一串里最强的**）：`tools/linux_abi_gate.py` 的 `qemu-readout` 项会在
QEMU 里跑信号夹具 `example/qemu_loongarch64/elf/linux_signal.c`，判 `[sig]` 块 **31 条 check
全部 ok**、`[sig] PASS: sigaction installed` 在、退出码 **138**（= 128 + SIGUSR1，正是
`lx_sig_pick` 里「其余默认动作结束任务」那条路径）。31 条覆盖：动作与旧动作、kill 返回值跨处理
函数、hits/signo、嵌套 hits/depth/rc、block/query/unblock、sigsetsize、SIGKILL、无 restorer、
被拒动作未装上、别的 pid、sig=99、tgkill，以及 `rt_sigreturn` 之后恢复。读数 `14 条 pipe +
31 条 sig` 全 ok，旁路演示未受影响，门禁 **13 passed / 0 failed / LINUX-ABI-GATE OK**。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/format_gate.py` OK 392；
`doc_audit funcs` **3 → 2**。

结论分档：**编译通过（0 告警）+ QEMU 端到端 PASS（信号夹具 31 条 check + 退出码 138）+ 静态门禁
全绿，未上板**（QEMU 算上板，不是真机）。

#### 5.30.52 第四十七笔：`ovl_find_path()` 拆成定位根 / 找子节点两段（2026-11-21）

`kernelsrc/src/arkrtos_fdt_overlay.c` 的 `ovl_find_path()`（**121 行**）按绝对路径在基树里找
节点：校验开头、定位根、然后逐段「在本层找一个名字全等的子节点、把当前位置推进到它的内容起点」。
拆成三段：定位根、找直接子节点、逐段推进。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `ovl_root_begin()` | `path` 为空或不以 `/` 开头 → `PATH`；`off_struct + 4 > end` 或该处不是 `BEGIN_NODE` → `FORMAT`；读根节点名；`p++` 跳过前导 `/`。`/` 就是根节点时 `*pp` 指向 `'\0'`，由调用方收尾 | 31 |
| `ovl_locate_child()` | 在 `[pos, end)` 里跨过 NOP / 属性 / 非目标子节点，按 `seg/seglen` 全等匹配；命中把子节点内容起点经出参带回并返回 `OK`；走到 `END_NODE` 或区耗尽返回 `NOT_FOUND` | 63 |
| `ovl_find_path()` 自己 | 定位根 → `/` 直接收 → 逐段切（`//` 与尾随 `/` 都报 `PATH`）→ 调 `ovl_locate_child` 推进 → 收尾写 `*body` | **49**（原 121） |

三条口径：

Ⓐ **和同文件的 `ovl_find_child()` 是两件事，不能合并**：既有那个只回答「有没有」，返回 1/0、
不给出子节点位置；`ovl_find_path` 要走多层，**必须拿到子节点内容起点**才能继续深入，所以新函数
叫 `ovl_locate_child` 并在注释里写明分工。（第一版直接叫 `ovl_find_child` 撞了既有名字，
编译期 `Wint-conversion` + `defined but not used` 报出来才发现——这正是「拆分参数搬运类函数编译器
抓不到少传参数」那条旧教训的镜像：**命名冲突编译器抓得到，语义串味它抓不到**，所以名字与注释都
必须自己写清。）
Ⓑ **错误码语义原样保留**：`//` 与尾随 `/` 报 `ARKRTOS_FDT_ERR_PATH`（「不认，不猜」），
区耗尽报 `NOT_FOUND` 而不是 `FORMAT`——这一点与既有 `ovl_find_child`（耗尽报 `FORMAT`）不同，
**没有统一**，因为两者用途不同：这里「路径不存在」是正常结果，那里「内容区坏」才是异常。
Ⓒ **`end` 只算一次**：`ovl_struct_end(bl)` 在主函数算一次，作为参数传进两段，避免两段各算一次
（那是同一个值，但会让「树被改过」的时序问题多一个观测点）。

体积（`default` 档，`loongarch64-newlib-elf-size`，两遍同配置 `make clean` 构建）：

- 改前 `text 449048 / data 448 / bss 5,128,740`
- 改后 `text 449096 / data 448 / bss 5,128,740`
- **+48 B**（多出的两层函数调用没被完全内联），全量告警 242 条前后一致、本文件 0 告警。

端到端：`tools/fdt_gate.py` 的 `qemu-overlay` 项在 QEMU 里应用一份 overlay，路径解析走的正是
`ovl_find_path` 逐段下钻——应用前无 `overlay-demo-node`、`victim-child` 在、`victim-prop=before`、
控制台 115200；应用后 `nodes 10→10`、`model=fdt-overlay-demo`、`current-speed 115200→9600`、
`victim-child` 查不到、`victim-prop` 已删。7 种坏法（错 nodes / 错 model / 错属性值 / 应用前就有
节点 / uart 没改 / 子节点没删 / 属性没删）全部被判红。门禁 **14 passed / 0 failed / FDT-GATE OK**。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/format_gate.py` OK 392；
`doc_audit funcs` **2 → 1**。

结论分档：**编译通过（0 告警）+ QEMU 端到端 PASS（overlay 应用路径解析）+ 静态门禁全绿，未上板**
（QEMU 算上板，不是真机）。

#### 5.30.53 第四十八笔：`fdt_walk_match()` 拆出命中判定与结果交付（2026-11-21）

`kernelsrc/src/arkrtos_fdt.c` 的 `fdt_walk_match()`（**121 行**）是上一次遍历同时回答两个问题：
`ph != 0` 时按 phandle（含 `linux,phandle` 别名）找，`ph == 0` 时按（名字, 深度）找；找到就填
`*out` 并按需拼出绝对路径。函数主体是 structure block 的 token 遍历，命中处理占了近一半行。

| 单元 | 干什么 | 行数 |
|---|---|---|
| `fdt_node_hit()` | 刚进入节点时判命中：`ph != 0` 先试 `phandle` 再试 `linux,phandle`；`ph == 0` 比（`depth == want_depth` 且名字全等）。返回 1 = 命中 | 27 |
| `fdt_take_hit()` | 命中后交付：按需填 `out`（name / depth / props_off / `fdt_count_props`），按需调 `fdt_build_path` 拼绝对路径 | 17 |
| `fdt_walk_match()` 自己 | init 检查 → `off/end` → token 循环（NOP / END / END_NODE / PROP / BEGIN_NODE 各分支）→ 入栈、判命中、交付 | **88**（原 121） |

三条口径：

Ⓐ **命中判定要接「入栈后的深度」**：`depth` 在 `stack[depth] = name; depth++;` 之后才传给
`fdt_node_hit`，函数里填 `tmp.depth = depth - 1u`、比较 `depth == want_depth`——这套「深度是
1 起、name 里存的 index 是 0 起」的换算原样搬进新函数，没有在边界上改口径。
Ⓑ **交付路径要用整条祖先链**：`fdt_take_hit` 收到的是 `stack` 与入栈后的 `depth`，而不是单个
节点——因为 `fdt_build_path` 要的是从根到目标的整条链。传单个名字就拼不出绝对路径。
Ⓒ **遍历循环本身一个字没动**：NOP 跳过、END 要求 `off == end`、END_NODE 要求 `depth != 0`、
PROP 跳 `fdt_pad4(len)` 并查越界、BEGIN_NODE 查 `MAX_DEPTH + 1`——全部留在主函数里。抽的只是
「这一节点是不是目标」和「命中之后怎么交付」这两件事，遍历状态机不拆。

体积（`default` 档，`loongarch64-newlib-elf-size`，两遍同配置 `make clean` 构建）：

- 改前 `text 449096 / data 448 / bss 5,128,740`
- 改后 `text 449096 / data 448 / bss 5,128,740`
- **0 B 差异**，全量告警 242 条前后一致、本文件 0 告警。0 B 只能说明编译器把两层调用内联成了同样的
  形状，**不能当作行为等价**；等价性看下一段。

端到端：`tools/fdt_gate.py` 的 `qemu-fdt-api` 在 QEMU 里跑 `chosen` / `path` / `phandle` 三条查询
——分别打 `arkrtos_fdt_node_by_phandle`（走后者的 `ph != 0` 分支）与按路径查询（走 `ph == 0` 的
名字+深度分支），三者解析到同一节点、`save+load` 往返 `nodes=10` 不变、空文件被拒且旧树保留；
4 种坏法（alias / depth / nodes / 回滚读数）全被判红。门禁 **14 passed / 0 failed / FDT-GATE OK**。

门禁：`tools/ci_gate.py` **57 passed / 0 failed / 0 skipped**；`tools/format_gate.py` OK 392；
`doc_audit funcs` **1 → 0**（超过 120 行的函数清单清零）。

结论分档：**编译通过（0 告警）+ QEMU 端到端 PASS（phandle / 路径两条分支都跑到）+ 静态门禁全绿，
未上板**（QEMU 算上板，不是真机）。

### 5.31 LSX 向量寄存器上下文（U-19，2026-11-21）

#### 5.31.1 缺口：FPU 档只存了一半的浮点上下文

`ARKRTOS_USE_FPU=1` 的 trap 帧存 32 个 `$f`（每个 64 位）与 FCSR，而向量寄存器
`$vr0..$vr31` 是 **128** 位——每个 `$vr` 是 `$f`（低 64 位）加上后面 64 位。所以
「浮点上下文按任务保存」这句话此前只对其中一半成立：任何一条 LSX 指令
（`vld` / `vst` / `vadd.b` …）都会在两个任务之间串味。

真正的坑不在「忘了存」，而在 `EUEN`（CSR 0x2）的三位：

| 位 | 名字 | 管什么 | 本档取值 |
|---|---|---|---|
| 0 | FPE | 浮点单元 | 1（`start.S` 早就置了） |
| 1 | SXE | 128 位向量（LSX） | **1（本轮置；关档不置）** |
| 2 | ASXE | 256 位向量（LASX） | **故意 0** |

不置 SXE 时向量指令不落错答案，而是落显式故障（`Ecode=13 INE`）。置了 SXE 却不
保存，就是**跨任务静默算错**。这两条只能二选一，不能「开着好看」。

#### 5.31.2 帧布局

| 偏移 | 内容 | 何时存在 |
|---|---|---|
| 0..255 | 32 个 GPR（8 B 槽）+ ERA / PRMD | 总是 |
| 256..511 | `$f0..$f31`（`256 + 8*i`） | `USE_FPU=1` |
| 512..519 | FCSR | `USE_FPU=1` |
| **528..1039** | **`$vr0..$vr31`（`528 + 16*i`，16 字节对齐）** | **`USE_LSX=1`** |

帧长：默认 256 / FPU 档 528 / **LSX 档 1040**。1040 = 65 x 16，而帧基址本来就在
16 字节对齐的栈顶上，于是 `vst`/`vld` 的「16 字节对齐」与 `si12` 偏移上限（最大
偏移 1024 < 2047）同时满足，一个字节的对齐填充都不用加。C 侧只跟着改
`LA_FR_SIZE` / `LA_FR_WORDS`（1040 / 130）：`arkrtos_port_swap_frame_bytes()` 是
`return LA_FR_SIZE`，所以内核的栈预算检查自动跟随，没有第二处要改。

#### 5.31.3 自检与读数

自检写在内核例程 `example/qemu_loongarch64/main.c` 的 `[lsx]` 块里（照 `[fcsr]`
自检的样子）：两个任务各持一份 512 字节图案（`0xA5A50000 ^ i*0x01010101` 与
`0x5A5A0000 ^ i*0x01010101`），每轮 `vld` 全量装 32 个 `$vr` ->
`arkrtos_task_wait_internal(1)`（**必然被切走**）-> `vst` 全量存回 -> 与自己的图案
逐字比对，跑 50 轮。另有一条**前置对照**：装完立刻存、不经切换，它证明「指令与
开关本身是对的」，这样后面的失败才能归因到「没保存」这一件事上。

```bash
make -f example/qemu_loongarch64/Makefile clean
EXTRA_CFLAGS="-DARKRTOS_USE_LSX=1" make -f example/qemu_loongarch64/Makefile -j8
cd build/qemu_loongarch64
timeout 20 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
    -machine virt -kernel ark_qemu_la64.elf -nographic -monitor none -serial file:out.txt
```

读数（`out.txt`，节选）：

```
[lsx] pre bad=0 out0=2779054080 pat0=2779054080 out16=3048542224 pat16=3048542224
[lsx] pre bad=0 out0=1515847680 pat0=1515847680 out16=1246367760 pat16=1246367760
[fcsr] A rm=1 bad=0 rounds=50 | B rm=2 bad=0 rounds=50 -> PASS (FCSR per task across switches)
[lsx] A bad=0 rounds=50 | B bad=0 rounds=50 -> PASS (vr0..vr31 per task across switches)
```

`[fcsr]` 那一行**不是顺手贴的**：它证明这一档没有把邻居搞坏（FPR 与 FCSR 仍在），
否则「向量区存对了」很可能只是「整个 FPU 区都不再存」。

**反假绿**：把 `arkrtos_context.S` 里两处 `#if (ARKRTOS_USE_LSX == 1)` 临时改成
`#if 0 /* NEGCTRL-LSX */` 后重编重跑，`[lsx] pre bad=0` 两条**仍在**（指令与开关
没问题），但判决行立刻变：

```
[lsx] A bad=50 rounds=50 | B bad=49 rounds=50 -> FAIL (vector regs leaked between tasks)
```

改回后 `grep -c NEGCTRL-LSX` 为 0。这条对照说明判据真的能红，而不是「本来就红
不了」。

**默认档逐字节不变**：`git worktree add /d/tmp/ark_base HEAD` 拉一份基线，两边都
`make clean` 后以 `SOURCE_DATE_EPOCH=1700000000` 构建，再 `objcopy --strip-debug`，
新树与基线树的 sha256 前 24 位都是 `3e6858e8be2816dfafef1b10`。

**门禁**：`tools/lsx_gate.py` 六项（`switch-default` / `frame-size` / `euen` /
`parser-selfcheck` / `default-build` / `lsx-build`），实测 `6 passed, 0 failed` /
`LSX-GATE OK`；`--no-build` 只跑现成镜像，`parser-selfcheck` 用四种坏日志变异证明
解析器不是恒绿。

#### 5.31.4 边界（不许读成更多）

1. 读数来自 `qemu-system-loongarch64`，**不是真机**。16 字节对齐落 ALE 的语义、
   真片上的 icache/dcache 一致性都要真机能证；这一档自检里没有 `dbar`/`ibar`，
   因为切任务走的是 `ertn` 路径而不是自改代码。
2. **LASX 不保存**：`EUEN.ASXE` 故意保持 0，用到 `xvst`/`xvld` 落 `Ecode=13 INE`，
   是显式故障而不是静默算错。真要做得先改开帧路径（`xvst` 要 32 字节对齐，现在
   帧基址只保证 16 字节），属另一轮工作量。
3. 每个任务的栈预算 +512 字节。板级 `*_TASK_STACK_WORDS` 常量没有自动加，开档时
   由使用者按余量判断（QEMU 板的栈本来就宽裕）。
4. 只有 LA64 port 消费这个宏；LA32 / RV32 / ARM 档不产生任何指令，镜像不变。

### 5.32 ELF 演示任务的 4 KB 栈不够（2026-11-21）

**现象**：dyn 演示档偶发卡在第二轮动态链接演示之前，串口上先出现一条
`[STK] task 7: stack bottom flag overwritten`，紧接着 `task 7 exits (self kill)`。
任务 7 是 `elf_demo.c` 里的演示任务，它被守卫摘掉之后 `dynmain_exec` 与坏夹具
`dynmain_bad` 两轮都不再执行，于是 `tools/dynlink_gate.py` 的 `qemu-readout`
凑不齐「三条 `[dyn] finished` / 三条 `tls base=` / 坏夹具 127」而判红。默认档
不出现这条，所以它一直是隐性的。

**先排除「野指针单点写坏」**：

- 保护字被写入的值两次分别是 `0x00437810`（落在 `g_fs` 内部偏移 `0x18`）与
  `0x00000004` / `0x00000008`，不固定 —— 不像某个固定结构体被错写；
- `s_demo_stack[0]`（`0x8DE830`）正好等于 task A（`s_stack_a`，2048 B）的栈顶，
  但 task A 的图案扫描峰值只有 940 B，它没有写上去；
- 报错时 `sp=0x8DEF00`，距栈底 1744 B。

**取证**：在 `arkrtos_stack_guard_check()` 里临时插一段诊断（定位完已删），
打印保护字、`sp`，再用与 `arkrtos_task_stack_info_raw` 同一套图案扫描统计
「栈里还有多少个字仍是 `0xcdcdcdcd`、最低/最高被写过的下标」。一次复现读数：

```
 0x000003F9 0x00000001 0x000003FF ...   <- cnt=1017 lo=1 hi=1023
[STK2] val=0x00000000 bot=0x008DE830 sp=0x008DEF80
 4096 4096 4096 2224
```

`cnt=1017`（1023 个字里 1017 个被写过）、`lo=1 hi=1023` ⇒ **整段栈被连续压过**，
不是单点写坏。同时 `used_psp` 只有 2224 B —— 因为 `stack_peak_low` 只在切任务 /
进陷阱时采，**单次调用链内部的深压栈它看不见**；判「有没有超」必须用图案扫描，
这一点在 §5.25 之后又一次踩到。

**定量**：把演示栈临时放到 8 KB，用图案扫描读峰值（同一份代码，只改栈大小）：

| 任务 | 栈大小 | 峰值用量 |
|------|-------:|---------:|
| 1 / 2（A、B 循环） | 2048 | 940 / 972 |
| 3（文件系统） | 4096 | 2604 |
| 4 / 5 / 6 / 9 | 2048 | 540 / 540 / 532 / 540 |
| **7（ELF 演示）** | 8192 | **4172**（dyn 档；默认档 3980） |
| 8（模块演示） | 8192 | 532 |
| 10 | 8192 | 1164 |
| 11（execve 演示） | 32768 | 3956 |

也就是说 4 KB 在默认档只剩 116 B 余量（3980/4096），dyn 档多出来的那几个调用
直接不够，溢出量约 50~80 B —— 正好覆盖保护字与随后十几个字，与 `cnt=1017`、
`lo=1` 完全对得上。**不是野指针，是演示任务自己的栈开小了**：它自己就是跑装载器
的那个任务，exec 一次要把 ELF 头、段表、interp 路径和整段 23 KB 的夹具文件
写穿 VFS/littlefs。

**修法**：`example/qemu_loongarch64/elf_demo.c` 的 `DEMO_STACK_WORDS` 1024 → 2048
（8 KB，留一倍余量），注释里写清上面这组实测值与「改大小之前先量一遍」。

**验证**：dyn 演示档连跑 5 次，5/5 都是三条 `[dyn] finished` + 三条 `tls base=` +
坏夹具 `exit code 127`，不再出现 `[STK] task 7`；`tools/dynlink_gate.py`
`13 passed, 0 failed` / `DYNLINK-GATE OK`；`tools/ci_gate.py`
`55 check(s) passed, 0 failed`。

**边界**：读数来自 `qemu-system-loongarch64`，不是真机；峰值随工具链版本与优化档
变动，换工具链要重新量一遍，不要照抄 8 KB 这个数字。

## 6. LoongArch32 移植（仅编译通过；上游有 LA32 模拟器，本机不能执行）

### 6.1 载体现状：上游有 LA32 模拟器，本机不能执行

先查本机，再动手：

```bash
ls D:/env/tools/qemu/qemu9/mingw64/bin/ | grep loongarch
find D:/env/tools/qemu -name "qemu-system-loongarch32*"
```

两条都是空——QEMU 9.2.3 **只提供 `qemu-system-loongarch64`**，
上游 QEMU 也没有 loongarch32 系统模拟器；机器上同样没有 LA32 板子。
所以目录命名**故意不用 `qemu_` 前缀**：

```
kernelsrc/port/loongarch/la32/arkrtos_port.c        # HAL 实现（4 字节槽帧）
kernelsrc/port/loongarch/la32/arkrtos_context.S     # eentry / 就地切换 / resume / idle
board/la32_generic/arkrtos_board.{c,h}             # 「通用 LA32 参考目标」
board/la32_generic/arkrtos_board_config.h          # ARKRTOS_ARCH_CORE = 0x0301（LA32）
example/la32_generic/{main.c,start.S,link.ld.in,gen_ldconsts.py,libc_shim.c,Makefile}
```

叫 `qemu_loongarch32` 会让人以为有台机器能跑，那是在造证据。

**但「上游公版没有」不等于「没有」**：龙芯教育维护了一份支持
LoongArch32 精简指令集的 QEMU 分支 ——
`https://gitee.com/loongson-edu/la32r-QEMU.git`（GPL-2.0，默认分支
`la32_debug`，`VERSION` = `6.2.50`）。本轮把它的源码读了一遍，
**下面是源码口径，不是运行口径**：

| 项 | 事实（源码） | 与本目标的对应关系 |
|---|---|---|
| 机器 | `DEFINE_MACHINE("ls3a5k32", …)`，描述 `ls3a32 test platform`；默认 CPU `la32`；`mc->max_cpus = 32`；`TARGET_SUPPORTS_MTTCG=y` | 一台真能装 ELF 的 LA32 机器 |
| 内存 | 节点 0 = `-m` 指定的 RAM，映射在 **0x0**；节点 1 = 4 MB ROM @ `0x1c000000`（`LA_BIOS_BASE`） | 与本目标「DRAM 从 0x0 起、镜像 0x200000 起」一致 |
| 串口 | `serial_mm_init(…, 0x1fe001e0, 0, irq3, 115200)`，寄存器步长 1（ns16550a 口径） | 与 `ARKRTOS_QEMU_UART_BASE = 0x1FE001E0`、`UART_REG_SHIFT = 0` 完全对上 |
| 定时器 | 常量定时器 `CSR.TCFG / TVAL / TICLR`，计数 100 MHz（10 ns/tick） | 与 `la32` port 的 TCFG/TICLR 用法、100 MHz 标定一致（§6.4） |
| `rdtime.d` | 32 位构建下**写寄存器对**：低 32 位进 rd、高 32 位进 rj（`insn_trans/trans_extra.c.inc` 的 `TARGET_LOONGARCH32` 分支） | 与本 port 的 `la_rdtime()` 两寄存器写法一致 |
| 装载 | `-kernel` 走 `load_elf(…, EM_LOONGARCH, …)`，ELF32 可装；入口若落在 `0x8000_0000` 窗口则改写成有符号 32 位 | 本目标产物是 ELF32 / `EM_LOONGARCH` / entry `0x200000`，可被它装载 |
| 强制外设 | `drive_get(IF_SD)` 为空就 `exit(1)` | 启动命令行**必须**带 `-drive if=sd,…`，否则起不来 |

**跑不起来的唯一原因是宿主，不是目标**：官方发行包只有 Linux 二进制
（`v0.0.2`：`la32r-QEMU-x86_64-ubuntu-22.04.tar`、`…-aarch64-…`；
`v0.0.1-alpha`：debian / centos x86_64），**没有 Windows 版**。本机
`wsl -l -v` 显示没有已安装的发行版，`C:/msys64` 也不存在，而 QEMU 6.2
的 mingw64 构建要整套 MSYS2 工具链——本轮没有为它动工。

所以本节结论不变、只是措辞要精确：**LA32 仍然只有编译面证据**；
「没有载体」应读成「载体存在，但这台 Windows 机器执行不了它」。
真接上它时，第一件事不是改代码，而是按 §6.8.5 的六组读数逐条验；
验过之后 §6.7 那张「从 la64 搬来的」清单才能逐行划掉。

#### 6.1.1 还有哪些 LA32 的 QEMU 载体（都是查来的源码事实，没有一条是本机读数）

上面那份 la32r-QEMU 不是唯一的。下面四条是本轮逐个访问仓库/发布页核到的，
**分成三档写清楚，不要把「有项目」读成「能跑」**：

| 载体 | 是什么（已核事实） | 可执行性 |
|---|---|---|
| `gitee.com/loongson-edu/la32r-QEMU` | QEMU 派生，`VERSION` = `6.2.50`；分支 `la32_debug`（默认）/ `la32_release`；targets 有 `loongarch32-softmmu` 与 `loongarch32-linux-user`；机器 `ls3a5k32`（§6.1 那张表） | 有 Linux 预编译包（`v0.0.1-alpha` / `v0.0.2`），**没有 Windows 版** |
| `github.com/loongson-community/qemu` 分支 `la32-user-exp` | QEMU **9.2.94**；在 `target/loongarch/cpu.c` 里新增 LA32/LA32R CPU 模型 `la32r` / `max32r` / `max32`（`CPUCFG1.ARCH` 置 0 或 1，`dtb_compatible` 为 `loongarch,la32r` / `loongarch,la32`）；`configs/targets/` 下**没有** `loongarch32-*.mak`，即 LA32 是跑在 `loongarch64-softmmu` 目标下的 CPU 模型；`hw/loongarch/virt.c` 未为 LA32 改动 | 只有源码（configure + ninja，需 Linux 构建环境），**无预编译、无 Windows 版** |
| `github.com/sunhaiyong1978/Yongbao-Embedded` 发布页 | 0.15 提供 `qemu-x86_64-to-loongarch32` / `qemu-loongarch64-to-loongarch32`（各 4.8 MB，`qemu-user` 模式，binfmt + chroot）；0.16 另给 `…la32s_custom.qcow2` 与 `vmlinux-6.18.0_la32s.efi`（配 system 模式用）；0.18 无附件 | 二进制**只对 Linux x86_64 / LoongArch64**，**没有 Windows 版** |
| `gitee.com/loongson-edu/la32r-Linux` | 支持 la32r 的 Linux 5.14 内核，README 声明可跑在 la32r-QEMU、chiplab/loongson-soc、全流程参考 SOC 三个平台 | 是内核不是模拟器；说明 la32r-QEMU 是被官方内核支持的运行目标 |

旁证（说明这条路在上游是活的、不是孤例）：`binutils` 的 LA32 补丁
（2025-06，`mengqinggang`）正文直接引 `sunhaiyong1978/CLFS-for-LoongArch32`
的 `Qemu_For_LoongArch32-Simple.md` 作为「基于 qemu 的 LA32 Linux 系统」；
`glibc` 的 LA32 补丁（2026-03，v5）写明 **LA32 tested by qemu-user and
qemu-system**；内核侧 LA32 基本支持（Huacai Chen，`[PATCH V3 00/14]`，
2025-11）已进主线讨论。工具链仓库：`cloudspurs/{gcc,binutils-gdb,glibc}`
的 `la32` 分支。

一条容易看走眼的差异：`la32-user-exp` 那条路是
`qemu-system-loongarch64 -cpu max32 -m 256M -smp 1 -nographic`
（CLFS 文档原命令，跑 **virt** 机器、挂 `vmlinux.efi`），**不是**
`ls3a5k32`。本目标的地址常量（DRAM 从 `0x0`、镜像 `0x200000`、UART
`0x1FE001E0`、BIOS `0x1c000000`）是按 `ls3a5k32` 定的，virt 的内存基址
与装载路径不同；**要拿 `-cpu max32` 跑本目标，先按 §6.8.5 验地址，
不能当成同一个载体**（此为读源码的判断，无运行读数）。

再确认一次上游公版：QEMU master（`VERSION` = `11.1.90`）的
`configs/targets/` 里仍**只有** `loongarch64-softmmu.mak`、
`loongarch32-softmmu.mak` 是 404 —— 所以「LA32 系统模拟器」目前**只在
上面两份 fork 里**，公版没有。

结论（保持三档分法，不合并）：**有源码可编 2 份**（la32r-QEMU、
la32-user-exp）、**有 Linux 预编译 1 套**（Yongbao-Embedded 的 qemu-user
与 qcow2 镜像）、**本机可执行 0 个** —— 本机 `D:/env/tools/qemu/` 只有
`qemu32`、`qemu9` 两个目录，`find` 到 `loongarch32` 的命中全是
`D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-*` 工具链，没有任何
`qemu-system-loongarch32*`。要真跑，仍然得先有 Linux（WSL2 发行版或一台
Linux 机）。

#### 6.1.2 本轮本机核查（2026-11-17）：判 `carrier_unavailable_on_this_machine`

§6.1.1 回答的是「上游有没有」，这一节回答的是「这台机器上今天能不能跑」。
两者证据性质不同（读源码/发布页 vs 本机命令输出），所以判词分开写。

五条命令，原样输出（2026-11-17）：

```bash
$ ls D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-*.exe | wc -l
58                       # 含 loongarch64 / riscv32 / riscv64，无 loongarch32
$ cat D:/env/tools/qemu/qemu32/VERSION
3.0.0                    # 老版本，整目录 13 个 exe 全是 arm/mips/riscv，无 loongarch
$ find D:/env -name "qemu-system-loongarch32*"
                         # 空
$ ls C:/msys64 ; which meson
                         # ls: cannot access 'C:/msys64': No such file or directory
                         # PATH 上只有 ninja 1.13.2，没有 meson
$ wsl -l -v
                         # 没有已安装的分发版
```

判词三句，各自独立成立，**不要合并成一句**：

1. **`la32_execution_carrier` = `carrier_unavailable_on_this_machine`。**
   本机两份 QEMU 都没有 LA32 机器：9.2.3 只到 `loongarch64`，3.0.0 连
   `loongarch` 都没有（`find` 全盘为空）。
2. **载体本身存在，缺的是宿主。** 上游 `la32r-QEMU` 能跑的发行物只有
   Linux 二进制（§6.1.1 第三列），没有 Windows 版；自建它要 QEMU 6.2 的
   `configure` + `ninja`，本机既无 MSYS2 也无 WSL 发行版，且 `meson` 不在
   PATH 上 ⇒ 缺的是**整套宿主编译环境**，不是缺一个包。
3. **这个判词只否证「今天在这台机器上执行 LA32」，不否证 LA32 port 的
   代码，也不否证「上游没有 LA32 模拟器」。** 三件事分属三层，混着读会
   把环境取舍读成代码缺陷。

要把它变成可执行，只有两条环境级的路（都不是改代码）：

- **A：换到 Linux 宿主（或装一个 WSL 发行版）**按上游 README 编
  `la32r-QEMU`，然后跑 §6.8.5 的首跑命令，再按 §6.8.5 那六组读数逐条验；
- **B：装 MSYS2 全套**，在 Windows 上自建 QEMU 6.2 的 mingw64 构建。

两条都要**先装环境**，属于项目级取舍，不在「顺手补欠账」的尺度内；
因此本轮登记为待裁定项，未动工（`docs/欠账盘点.md` 第一节同名行）。

**用户裁定（2026-11-17）：先收档，判词 `carrier_unavailable_on_this_machine`
留存。** 不装 MSYS2、不换 WSL/Linux 宿主；LA32 port 保持「只到编译/链接」这一档，
哪天有了 LA32 载体再按 §6.8.5 首跑，判词与核对命令原样留在这里当入口。

#### 6.1.3 外部载体再核查（2026-11-19）：判词不变

§6.1.2 记的是「这台机器今天能不能跑」，本节记的是「上游这半年有没有多出载体」。
三条新事实全部是 2026-11-19 现查的，不是凭记忆：

1. **上游 QEMU master 仍然没有 LA32 目标。** GitLab API 列 `target/loongarch/`
   目录共 23 项，没有 `loongarch32` 相关文件；同一目录下的 `Kconfig` 全文只有

   ```
   config LOONGARCH64
       bool
   ```

   一条。核对命令：

   ```bash
   $ curl -s "https://gitlab.com/api/v4/projects/11167699/repository/tree?path=target/loongarch"
   # 23 项：kvm tcg Kconfig README arch_dump.c cpu-csr.h cpu-mmu.h cpu-param.h ... vec.h
   $ curl -s "https://gitlab.com/qemu-project/qemu/-/raw/master/target/loongarch/Kconfig"
   config LOONGARCH64
       bool
   ```

   推论：新版 QEMU 发行说明里「补全 LoongArch TCG 指令」补的是 **LA64 指令集**，
   不是新增 32 位目标。不要把发行说明读成「上游已经有 LA32 了」。

2. **`la32r-QEMU` 的最新发行物仍然只有 Linux/Mac。** gitee API 查 releases：

   | 发行 | 资产 |
   |---|---|
   | v0.0.2（2024-05-10） | `la32r-QEMU-x86_64-ubuntu-22.04.tar`、`la32r-QEMU-aarch64-ubuntu-22.04.tar` |
   | v0.0.1-alpha（2022-04-15） | `qemu-system-loongarch32_{MAC_arm64,debian_la64,centos_la64,centos_x86_64,debian_x86_64}`、`loongarch32r-linux-gnusf-gdb` |

   全是 ELF/Linux 可执行文件，没有一个 PE。仓库状态：默认分支 `la32_debug`，
   另有一支 `la32_release`，最后一次更新 2025-12-26，GPL-2.0，7 star。

3. **本机仍然只有一个 WSL 壳。** `wsl.exe -l -v` 返回的是「没有已安装的分发版」
   引导文案（提示 `wsl.exe --list --online` 与 `wsl.exe --install <Distro>`），
   不是发行版列表；`wsl.exe` 本身在 `C:/Windows/system32/` 里，但没有任何
   Linux 环境。这与 §6.1.2 第五行一致，今天只是再确认一次。

再补一条正面事实（避免把「跑不了」读成「什么都没有」）：**编译侧是齐的**。
`D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-gcc.exe`（GCC 8.3.0）在，
`example/la32_generic` 的产物能出；缺的只有执行载体这一件东西。

**判词不变：`la32_execution_carrier` = `carrier_unavailable_on_this_machine`
（2026-11-19 复核，无新载体）。** 三条不许读法照旧：不等于 LA32 port 的代码
有问题；不等于上游永远不会有 LA32 目标；不等于「这台机器上跑不了」这件事
可以用换 RTOS / 换编译器绕过。

### 6.2 工具链与产物

`D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-gcc.exe`（GCC 8.3.0）。

```bash
-flags: -march=loongarch32 -mabi=ilp32d
```

实测约束（`-print-multi-lib`）：

- 默认库是 `ilp32s`；另有 `base/ilp32f`、`base/ilp32s` 两档；
- `-march` 的合法值只有 `loongarch32`；**`-mabi=ilp32` 不被接受**，必须
  写全 `ilp32d`/`ilp32f`/`ilp32s` 之一（`lp64*` 是超集，用不了）。

产物核对（2026-11-15 最小档实测；**全功能档的读数见 §6.8**）：

```
file format elf32-loongarch
architecture: Loongarch32, flags 0x00000112
EXEC_P, HAS_SYMS, D_PAGED
start address 0x00200000

   text    data     bss     dec     hex  filename
  52280      56   15968   68304   10ad0  build/la32_generic/ark_la32_generic.elf
```

> 上面这一份是**只开调度/同步/阻塞唤醒**的最小档，已经不是当前默认。
> 现在 `board/la32_generic/arkrtos_board_config.h` 默认打开的是与 la64 对齐的
> 全功能档（Shell / 日志环 / 块设备 / littlefs / VFS / procfs / UTEST）。

全镜像反汇编中 `st.d|ld.d|addi.d|lu32i.d|lu52i.d|pcalau12i` 匹配数为 **0**。

### 6.3 trap 帧：128 字节、4 字节槽

与 la64 逐行同构，唯一变化是槽宽：

| 偏移 | 内容 | 偏移 | 内容 |
|---|---|---|---|
| 0 | ra (r1) | 72 | t7 (r19) |
| 4 | tp (r2) | 76 | t8 (r20) |
| 8 | sp 原值 | 80 | fp (r22) |
| 12–40 | a0–a7 | 84 | s0 (r23)（兼帧指针） |
| 44–76 | t0–t8 | 88–116 | s1–s8 |
| **120** | **ERA** | **124** | **PRMD** |

`LA_FR_WORDS=32`、`LA_FR_SIZE=128`、`LA_FR_A0=3`、`LA_FR_ERA=30`、
`LA_FR_PRMD=31`。r21 是 psABI 保留（TLS）寄存器，不进帧。

### 6.4 `rdtime` 的写法与一个真陷阱

`rdtime.w` / `rdtimeh.w` **本 binutils 不识别**（"no match insn"）；
`rdtime.d $rd, $rj` 被接受，而且是**寄存器对**语义：低 32 位进 `$rd`、
高 32 位进 `$rj`（反汇编确认编码 `rdtime.d $r12,$r13`）。所以 32 位取时
基数的写法是：

```c
uint32 lo, hi;
__asm__ volatile("rdtime.d %0, %1" : "=r"(lo), "=r"(hi) : : "memory");
return lo;
```

### 6.5 最贵的一个坑：工具链不拒绝 `.d` 指令

在 `-march=loongarch32` 下，`st.d` / `ld.d` / `addi.d` / `lu12i.w` /
`la.abs` / `iblar` 等**都能汇编通过**（实测）。也就是说：

> **「汇编通过」不能证明代码是 32 位合法的。**

据此的纪律是：本目标所有汇编**一律只用 `.w` 形式**，并且这条告诫已经
同时写在 `arkrtos_context.S` 文件头、`start.S`、`port.c` 与 `Makefile`
四处。若将来有人把 la64 的 `.d` 代码复制过来，只要漏掉槽宽换算，
汇编和链接都不会报错，只有真机运行才会错——而现在没有真机。

### 6.6 `run` 目标故意报错

`make -f example/la32_generic/Makefile run` 会打印一段说明并**失败**
（recipe 里 `exit 1`，make 自身退出码 2 并打 `make: *** [run] Error 1`）：

```
ERROR: no runnable LoongArch32 execution target on this host.
  - QEMU 9.2.3 ships only qemu-system-loongarch64; upstream QEMU has no
    loongarch32 system emulator either.
  - A LoongArch32 QEMU fork does exist (gitee.com/loongson-edu/la32r-QEMU,
    machine ls3a5k32), but its release binaries are Linux-only and this host
    has neither a WSL distro nor MSYS2 -- see docs section 6.1.
  - There is no LA32 board on this machine either.
  This target only verifies build + link: build/la32_generic/ark_la32_generic.elf
```

这是刻意的：宁可明确报错，也不给一个「看起来跑起来了」的错答案。
等某台机器上真的有可执行的 `qemu-system-loongarch32`，`run` 目标才应该
被补成真命令（命令行写法见 §6.8.5 末尾）。

### 6.7 哪些结论是从 la64 搬来的（未在 LA32 上验证）

| 项 | 来源 | LA32 证据 |
|---|---|---|
| PRMD 位序（`PPLV[1:0]` / `PIE=bit2` ⇒ 回 PLV0 且开中断 = 0x4） | la64 QEMU 实测 | 无。`start.S`/`port.c` 按同一 `LA_PRMD_INIT` 写 |
| 无软件自中断 ⇒ 就地切换 | la64 QEMU 实测（同代硬件语义） | 无 |
| TIMER_HZ / RDTIME_HZ（100 MHz 计数、25 MHz 定时器） | la64 QEMU 实测 | 无。板级常量照搬，注释已标明 |
| `rdtime.d` 在 32 位下写寄存器对、`TCFG/TICLR` 定时器语义 | la64 QEMU 实测 + la32r-QEMU 源码（§6.1 那张表） | 无运行读数。若上游 32 位语义与硬件一致，本 port 的写法成立 |
| 地址映射 0x200000 起、UART @0x1FE001E0 | la64 QEMU virt 约定 | 无。接真实芯片必须重写 `.text` 起点与 `.data` LMA |
| 构建告警 | — | 仅 14 条 `variably modified 'arkrtos_app_off_*_check'`（分区头既有写法，la64 同）+ 1 条 `'p_tsk' may be used uninitialized`（内核既有） |

接上任何 LA32 载体（模拟器或芯片）时，先跑一遍 `example/la32_generic/main.c`
的四组读数（500 ms 节拍、A/B 交替、`svc_ok`、无 `[dbg]`），才能把哪一条
从「搬来的」改成「实测的」。

### 6.8 全功能档：与 la64 对齐的编译面

> **档位：仅编译 + 链接。** 本节所有数字都是构建产物读数，**没有任何一条是
> 运行期读数**——LA32 没有执行载体（§6.1）。这一点在下面每一处都再标一次，
> 因为「la64 全绿」与「la32 全绿」是两件事。

#### 6.8.1 打开了什么

目标是把 la64 QEMU 目标**跑通过的那一组**原样搬到 LA32，让编译面与实跑面
是同一套代码路径，而不是「la32 编一个小内核」：

| 层 | la64 QEMU 目标 | LA32 参考目标 | 差异 |
|---|---|---|---|
| 日志 | `LOG` + `LOG_RING` | 同 | 无 |
| 块设备 | `BLK`，`ram0` = RAM 模拟 NOR（512 KB / 4 KB 擦除） | 同（`board/la32_generic/drvblk.c`） | 无（那一份本身与指针宽度无关） |
| 文件系统 | `FS` + `VFS` + `VFS_LFS` + `PROCFS` | 同 | 无 |
| 控制台 | `SHELL`，ns16550a 注册为 `ttyS0`，收发轮询 | 同（`board/la32_generic/drvuart.c`） | 无 |
| 自检 | `UTEST` | 同 | 无 |
| 设备树 | `dts/qemu_loongarch64.dts` → `arkrtos_dt.c` | `dts/la32_generic.dts` → `arkrtos_dt.c`（7 节点） | 树内容按参考映射重写，`dt` 命令可查 |
| 自检/取证 | 无（la64 实跑那一轮没开） | `AUDIT` + `CRASH_LOG` + `MDK_TRACE` + `SECCHECK` + `SOFTBUS` 全 1 | 这五项在 la64/rv32 上已实跑取证（§8.4）；LA32 这一列是**编译面**对齐，没有运行期读数 |
| 安全类取证 | 同上五项 | 同左 + `IMAGE_SIGN` / `HW_SAFE` / `KERNEL_GUARD` 全 1 | 这三项在 rv32/la64 上已实跑取证（§8.4.8 ~ §8.4.10）；LA32 这一列仍是编译面 |
| 仍关 | `MINIAPP` / `POSIX` / `LWIP` / `APPPACK` / `APPRUN` / `FLASHAPP` / `QSPI` / `SMP` | 同 | 无（`FLASHAPP` 两侧都剔除：它无条件 include `drvflash.h`，两个目标都没有 Flash 控制器） |

`Makefile` 相应从「剔除 `fs` / `vfs_lfs`」改成「只剔除 `flashapp`」，并把
shell（`platform_arkrtos.c` + `ark_shell/*.c` 六个）、VFS 的 4 个驱动、
littlefs 两个源、`drvblk.c` / `drvuart.c` / `arkrtos_dt.c` 全部编进来。

#### 6.8.2 构建读数（2026-11-16 实测）

```bash
rm -rf build/la32_generic
make -f example/la32_generic/Makefile -j8
make -f example/la32_generic/Makefile size
```

```
   text    data     bss     dec     hex  filename
 284336     216  615956  900508   dbd9c  build/la32_generic/ark_la32_generic.elf
```

> 这组数字是「再打开 `AUDIT`/`CRASH_LOG`/`MDK_TRACE`/`SECCHECK`/`SOFTBUS`
> 以及 `IMAGE_SIGN`/`HW_SAFE`/`KERNEL_GUARD` 之后」的读数（2026-11-16 第三次
> 实测）；只开内核+文件系统+SHELL 时是 `213480 / 180 / 578592`，只加前五项
> 时是 `267712 / 216 / 615956`。差额主要来自软总线的静态池（bss）、trace 的
> 环形缓冲，以及签名/遗言/心跳三张表（text +36 KB 量级）。

- 与最小档（text 52280 / bss 15968）相比，text +161 KB、bss +562 KB
  （其中 512 KB 是 `drvblk.c` 的 RAM 介质）。链接脚本的 ASSERT 没有被触发，
  即该镜像仍落在参考映射的 8 MB 内核 RAM 段内。
  > 这是 2026-11-16 的读数。2026-11-20 起 LA64 板的 RAM 介质已从 512 KiB
  > 扩到 4 MiB（为了让真实发行版二进制装得下），所以「512 KB 介质」这句只
  > 描述当时；现在的 `.bss` 是 5 112 340 B（4 MiB 介质 + 640 KiB 容量缓冲），
  > 仍在 8 MB 内核 RAM 段内。
- 干净重建全量编译告警 **1 条**：`arkrtos_sync.c:518 'p_tsk' may be used
  uninitialized` —— 内核既有告警，la64 上同样存在，与本轮改动无关。
  （最小档曾出现 14 条 `variably modified 'arkrtos_app_off_*_check'`，
  全功能档下不再出现，原因未追，登记为观测事实而非结论。）
- ELF 头核对：`Class: ELF32`、`Machine: LoongArch`、`Entry point 0x200000`。
- 全镜像反汇编 48,563 条指令中，`st.d` / `ld.d` / `addi.d` / `lu32i.d` /
  `lu52i.d` / `pcalau12i` / `ldx.d` / `stx.d` 匹配数**全为 0** —— 这条检查
  对应 §6.5 那条「工具链不拒绝 `.d` 指令」的纪律。

符号在场证明（`nm` 摘录，证明「开关打开了、代码真的链进去了」，不是只改了宏）：

```
T ark_shell_init                 T arkrtos_shell_init
T arkrtos_dev_register           T arkrtos_board_uart_dev_register
T arkrtos_board_blk_init         T arkrtos_fs_format
T arkrtos_vfs_mount_volume       T lfs_mount
T ark_vfs_arkrtos_procfs         t procfs_mount / procfs_read / procfs_readdir
T arkrtos_dt_count / _model / _node
T ark_utest_list / ark_utest_run
```

#### 6.8.3 本轮暴露的一个真缺口：`-nostdlib` 下 littlefs 的两个桩

第一次链接直接失败（这不是「配置没开」，是**代码真的缺**）：

```
lfs.o: undefined reference to `__popcountsi2'
lfs.o: undefined reference to `__bswapsi2'
lfs.o: undefined reference to `strspn'
lfs.o: undefined reference to `strcspn'
```

两类的来源完全不同，必须分开记：

1. **libgcc 内置函数**：`-nostdlib` 不链 libgcc，GCC 把
   `__builtin_popcount` / `__builtin_bswap*` 打桩成对这些符号的调用。
   最小档没编 littlefs，所以这个缺口一直没被触发；la64 的 `libc_shim.c`
   早就给了 `__popcountsi2` / `__bswapsi2` 等四个，LA32 这一份当时还停在
   「够跑调度器」的规模。
2. **字符串函数**：`lfs_dir_find()` 用 `strspn` / `strcspn` 按 `/` 切路径，
   VFS/Shell 还用 `strrchr` / `strpbrk`。同样是 la64 侧已有、LA32 侧缺。

修法是把 la64 `example/qemu_loongarch64/libc_shim.c` 末尾那两段
（字符串补件 + libgcc 补件，共 178 行）搬到 LA32 的同名文件，并把
「lp64 下按 64 位字做 popcount」改成 ilp32 口径的说明。**语义不变、实现逐行
相同**，所以这不是「为 LA32 另写一套」。

> 这条缺口值得单独写出来的原因：它只在「功能面真的打开」时才出现。
> 如果继续用最小档，编译器与链接器都不会提醒你 littlefs 在这个目标上
> 根本链不起来。

#### 6.8.4 这一节明确**没有**做的事

- **没有跑过任何一条读数的**：节拍、任务切换、SVC、阻塞唤醒、littlefs
  读写、Shell 命令、UTEST 汇总，全部**未经执行**。镜像能不能启动、能不能
  进 shell，目前没有任何证据。
- 没有接外部中断控制器：UART 仍是轮询（与 la64 同一条边界）。
- 没有复位机制：`shell reboot` 走 `arkrtos_board_reboot()`，本目标实现为
  打印一句说明后停机。
- `example/la32_generic/Makefile run` 仍然是一条明确的错误（§6.6）。
- 参考映射的地址/频率常量仍是从 la64 搬来的（§6.7 那张表一条都没变）。

接上任何 LA32 载体后的第一件事，仍然是照 `example/la32_generic/main.c`
的六组读数逐条看：500 ms 节拍、A/B 交替、`svc_ok`、`[FS] PASS`、
`/proc/version` 可读、`utest` 汇总 **`13 matched, 13 passed, 0 failed`**
（用例数 9 → 10 是 `mtx_pi_chain_contract`，10 → 13 是
`hwsafe_death_neutralizes` / `guard_heartbeat_contract` /
`sign_hmac_and_loader_gate` 三例进场后的事，rv32/la64 实测均为
`assertions 133 run / 0 failed`，各连跑 3 次）。

最后一条必须连 `matched` 一起看：只看 `, 0 failed,` 会把
「0 matched, 0 passed, 0 failed」（一条用例都没跑）判成通过 —— 这正是
LA64/RV32 上真实发生过的假绿（§5.13.3）。LA32 因为没有执行载体，这一条
现在**既不能判通过也不能判失败**，只能说「修法已同步进 `start.S` /
`link.ld.in`，符号 `__init_array_start/end` 在场（9×4 = 0x24 字节）」。

#### 6.8.5 计时源、载体候选与首跑命令

**计时源**：`MDK_TRACE` 的时间戳来自 `rdtime.d`（§6.4 的两寄存器写法）。
本目标能核对到的只有「反汇编里 `rdtime.d $r12,$r0` 的编码合法」：

```bash
D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-objdump.exe -d \
    build/la32_generic/ark_la32_generic.elf | grep -n "rdtime"
```

这条是**编码合法性**，不是「读到的计数是 100 MHz」——后者要执行才知道。

**载体候选**：§6.1 里的 `la32r-QEMU`（`ls3a5k32` 机器）。等有可执行的
`qemu-system-loongarch32` 之后，首跑命令按它的机器约定写（注意两条：
`-machine ls3a5k32`、必须给一块 SD 设备否则 `exit(1)`）：

```bash
qemu-system-loongarch32 -machine ls3a5k32 -m 128M -smp 1 \
    -kernel build/la32_generic/ark_la32_generic.elf \
    -drive if=sd,file=build/la32_generic/sd.img,format=raw -nographic
```

产物自身的装载面已经核对过：`ELF32` / `Machine: LoongArch` /
`Flags: 0x1, LP32` / `Entry point 0x200000`（§6.2 的 `readelf -h` 读数），
落在该机器的节点 0 RAM 里，不需要改链接脚本就能被 `-kernel` 装上。
**装载得上不等于跑得对**：时钟频率、`TCFG` 的 tick 语义、UART 的
`THRE` 行为仍要在执行后逐条确认，这些正是 §6.7 那张表的行。

## 7. 复现步骤

### 7.1 RISC-V RV32

```bash
cd "D:/工作/git_project/ark rtos/example/qemu_riscv32"
make -j4            # 产出 ../../build/qemu_riscv32/ark_qemu_rv32.elf
make size           # 打印 text/data/bss
make run            # 启动 QEMU，串口写入 build/qemu_riscv32/out.txt
```

### 7.2 LoongArch64

```bash
cd "D:/工作/git_project/ark rtos"          # 注意：必须在仓库根目录
make -f example/qemu_loongarch64/Makefile -j4
make -f example/qemu_loongarch64/Makefile size
make -f example/qemu_loongarch64/Makefile run   # 串口写入 build/qemu_loongarch64/out.txt
python example/qemu_loongarch64/shell_check.py  # 双向 shell 端到端检查（§5.11.2）
```

带 monitor 调试时不要用 `run` 目标，直接手敲：

```bash
cd build/qemu_loongarch64
"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -M virt -m 128M -nographic -display none \
  -serial file:out.txt -monitor telnet:127.0.0.1:4567,server,nowait \
  -kernel ark_qemu_la64.elf
```

RV32 与 LoongArch64 这两个目标都是无限循环，Ctrl-C 或终止进程结束，
然后 `cat out.txt` 看输出。

### 7.3 LoongArch32（只到链接）

```bash
cd "D:/工作/git_project/ark rtos"          # 必须在仓库根目录
make -f example/la32_generic/Makefile -j4
make -f example/la32_generic/Makefile size
make -f example/la32_generic/Makefile run   # **只会报错失败**（exit 1 → make 退出码 2），见 §6.6
```

期望体积（全功能档 + `.ctors` + libc 补件 + 五项取证开关 + 签名/遗言/心跳，
2026-11-16 实测）：`text 284336 / data 216 / bss 615956`。三个台阶分别是：
最小档 `52280/…/15968` → 内核+FS+SHELL 全功能档 `213480/180/578592` →
再开 `AUDIT`/`CRASH_LOG`/`MDK_TRACE`/`SECCHECK`/`SOFTBUS`
（`267712/216/615956`）→ 再开 `IMAGE_SIGN`/`HW_SAFE`/`KERNEL_GUARD` 后的
上面这组（差额主要在软总线静态池、trace 环形缓冲与三张安全表，§6.8）。
改了 `ARCH_FLAGS` 或板级开关后必须先 `rm -rf build/la32_generic` 全量重建
——Makefile 不跟踪编译参数，增量构建会复用上一套开关的 `.o`。

没有串口文件可看——这个目标产出 ELF 就结束了。要核对「没有混进 64 位指令」：

```bash
D:/LoongIDE/la32-tool/bin/loongarch32-newlib-elf-objdump.exe -d \
    build/la32_generic/ark_la32_generic.elf \
  | grep -cE "\b(st\.d|ld\.d|addi\.d|lu32i\.d|lu52i\.d|pcalau12i)\b"
# 期望输出 0
```

> 重链接前要确保没有 QEMU 进程占着 ELF，否则 `ld` 报
> `cannot open output file ...: Invalid argument`（Windows 文件锁，不是链接脚本问题）。

### 7.4 ARM Cortex-M4（MPS2-AN386）

```bash
cd "D:/工作/git_project/ark rtos"          # 必须在仓库根目录
make -f example/qemu_mps2_an386/Makefile -j4
make -f example/qemu_mps2_an386/Makefile size

cd build/qemu_mps2_an386
"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-arm.exe" \
  -M mps2-an386 -cpu cortex-m4 -nographic \
  -kernel ark_qemu_m4.elf -serial file:out.txt
```

`-kernel` 只能传**裸文件名**，且必须先 `cd build/qemu_mps2_an386`：本机仓库
路径含非 ASCII 字符，QEMU 在这个参数上用的是窄字符 API，绝对路径直接失败
（与 RV32/LA64 的 `run` 目标同一条坑）。

一道门禁把上面这些全做完：

```bash
python tools/qemu_m4_gate.py            # 静态 pin + 编译 + 20 s QEMU + 自检
python tools/qemu_m4_gate.py --no-build # 复用已有 ELF
```

判据不是「有输出就算过」：镜像头 8 字节必须是 Cortex-M 复位向量，串口读数
要四条口径互相印证（见 §12.4），外加自检——把 7 种合成坏日志喂给同一个
解析函数，每一种都必须判 FAIL。

## 8. 三目标功能矩阵（档位、开关与缺口）

> 这一节只做一件事：把「哪些功能在哪三个目标上**真的跑过**」摊开。
> 每个格子只有三种可能：**实跑**（QEMU 里跑出读数）、**编译**（工具链
> 认了、链接出 ELF，但没执行过）、**未做**。没有第四种。

### 8.1 三个目标各自打开的功能

三条 `[boot]` 行是各自固件启动时自己报的（不是文档抄的）：

| 目标 | 启动读数 | 体积 text/data/bss |
| --- | --- | --- |
| RV32 | `sem=19922944 taskA=1 taskB=2 taskU=3 mpu=1 priv=1 taskRX=4 taskFS=5 blk=1 fs=1 vfs=1 shell=7 log=1 crasharea=0x80101e00 boots=1 tick_period_us=500` | 235800 / 128 / 624132 |
| LA64 | `sem=19922944 taskA=1 taskB=2 taskFS=3 blk=1 fs=1 vfs=1 shell=6 log=1 taskU=4 app_base=0x02000000 crasharea=0x00301e00 boots=1 utest=1 tick_period_us=500` | 305024 / 416 / 636436 |
| LA32 | 无（没有执行载体） | 284336 / 216 / 615956 |

三份板级配置里 `LOG / LOG_RING / BLK / FS / VFS / VFS_LFS / PROCFS / SHELL /
UTEST / AUDIT / CRASH_LOG / MDK_TRACE / SECCHECK / SOFTBUS / IMAGE_SIGN /
HW_SAFE / KERNEL_GUARD` 都是 1；
`MINIAPP / POSIX / LWIP / APPPACK / APPRUN / FLASHAPP / QSPI / RTSTAT /
SCHED_STAT / CM_BACKTRACE / SMP` 都是 0。

后三项（`IMAGE_SIGN` / `HW_SAFE` / `KERNEL_GUARD`）是第二轮才拨到 1 的，
取证方式与边界见 §8.4.8 ~ §8.4.10；`IMAGE_SIGN` 需要一把 32 字节密钥，
所以三份 `Makefile` 都在构建目录里生成 `arkrtos_sign_key_local.h`
（值 `0x00..0x1F`，与 `tools/sign_host_gate.py` 的 `KEY` 一致，不进仓库）。
RV32 另有 `MPU=1 PRIV=1`（PMP + U 模式），LA64 是 `MPU=0 PRIV=1`（PLV3 +
分页），LA32 两者皆 0（无执行载体，只能编译）。

两处**刻意不开**的，理由写在 §8.4.6：`RTSTAT` / `SCHED_STAT` 在本 port 上
`cyccnt` 返的是 -1/0，打开只会得到假读数，不如关着。

### 8.2 矩阵

| 功能 | RV32 | LA64 | LA32 |
| --- | --- | --- | --- |
| 节拍 / 任务切换 / 抢占 | **实跑**（`taskA/B` 500 ms 交替） | **实跑** | 编译 |
| 信号量 / 互斥 + 优先级继承 / FIFO / 事件 / 定时器 / mempool / mq | **实跑**（`utest 13/13`） | **实跑**（`utest 13/13`） | 编译 |
| 阻塞与唤醒（`wait` / `sem_post`） | **实跑** | **实跑** | 编译 |
| SVC 入口（`syscall 0x11` TASK_CTRL / `0x12` SYS_INFO） | **实跑** | **实跑**（PLV3 App 走同一条） | 编译 |
| 内核日志（ring）/ 内核 Shell（help/ls/cat/stack/utest/dt） | **实跑**（`shell_check.py`） | **实跑**（`shell_check.py` 10 条判据） | 编译 |
| blk（RAM 块设备）/ littlefs / VFS / procfs | **实跑**（`[FS] PASS`） | **实跑**（`[FS] PASS`） | 编译 |
| 任务级内存隔离 | **实跑**：MPU（PMP）+ U 模式，`mpu=1 priv=1 taskRX` | **实跑**：PLV3 + 静态 TLB 条目（§5.12） | **未做**（`priv=0`，无载体） |
| 越权访问的负对照 | **实跑**（§3.10） | **实跑**（PPI / refill / IPE 三条，§5.12.8） | 未做 |
| UTEST 全量（13 例 / 133 断言） | **实跑** `13 matched, 13 passed`（连跑 3 次） | **实跑** `13 matched, 13 passed`（连跑 3 次） | 编译 |
| 结构自检 `ARKRTOS_USE_AUDIT` | **实跑** `structures.audit checks=28 mask=0x0` | **实跑** `checks=27 mask=0x0` | 编译 |
| 崩溃日志 `ARKRTOS_USE_CRASH_LOG` | **实跑** `crash.journal boots=1 seq=1`、`journal: 0x80101E00 +512 bytes` | **实跑** `boots=1 seq=1`、`journal: 0x00301E00 +512 bytes` | 编译 |
| 指令追踪 `ARKRTOS_USE_MDK_TRACE` | **实跑** `trace : ARMED, cap=2048 records total=14729 lost=0 wrapped=1` | **实跑** `total=14646 lost=0 wrapped=1` | 编译 |
| 安全自检 `ARKRTOS_USE_SECCHECK` | **实跑** `12 checks - pass 7, fail 0, finding 2, skip 3`、`gates.kernel bits=0xFF (need 0x3F), iso=1`（MPU）、`sign/hwsafe=0x3` | 同上但 `iso=2`（分页 + PLV） | 编译 |
| 软总线 `ARKRTOS_USE_SOFTBUS` | **实跑** `bus selftest: ALL PASS`（9 项 / 56 断言）、`bus info` peer online | **实跑** 同上 | 编译 |
| 网络（lwip） | 未做（无网络后端） | 未做（同左） | 未做 |
| `exec` 原生 ELF（`ARKRTOS_USE_ELF`） | **实跑 ELF32（2026-11-17，U-4）**：PMP 窗口 + U 模式，`[hello32] … bye` 8 行 + `[ELF] … finished`，5 连跑 5 成（§5.25） | **实跑**：`[ELF] ... rc=0` + 程序自报 `argc/argv`，连跑 10 次（[MMU进程模型与ELF装载](MMU进程模型与ELF装载.md) §5）；2026-11-17 起装载器在 `activate` 之前回收该空间的旧窗口（`used` 512→0→512，反复 `exec` 不再吃干帧池，§5.24 有 11 条读数） | 未做（`arkrtos_elf.c` 只支持 64 位 `long`，见 U-4） |
| `ld` 可下载模块（`ARKRTOS_USE_MODLOAD`，PLV0） | 未做（32 位被 `#error` 挡住，负向用例在 `tools/modld_gate.py` F 段） | **实跑**：`ld rc=0 [base=... relocs=165 imports=8]`，`lkup/modules/unld` 走通（[MMU平台可下载模块-VxWorks式](MMU平台可下载模块-VxWorks式.md) §5） | 未做（同左） |
| App 装载族（apppack / miniapp / apprun / flashapp） | 未做（缺 arkc RV32 后端） | 未做（缺 arkc loongarch 后端；板级 `APPRUN/FLASHAPP` 也置 0） | 未做 |
| 镜像签名 `ARKRTOS_USE_IMAGE_SIGN` | **实跑** `sign_hmac_and_loader_gate`（整块/流式同口径、篡改与 `-18` 装载门） | **实跑** 同左 | 编译 |
| 硬件安全态 `ARKRTOS_HW_SAFE`（遗言/中和） | **实跑** `hwsafe_death_neutralizes`、`sec: hwsafe.deadman` | **实跑** 同左 | 编译 |
| 内核守卫 `ARKRTOS_USE_KERNEL_GUARD`（心跳窗口） | **实跑** `guard_heartbeat_contract`、`sec: guard.watchdog`（FINDING：无硬件看门狗） | **实跑** 同左 | 编译 |
| SMP（多核） | 未做：机器能带多 hart，实测 `-smp 2` 直接崩（所有 hart 跑同一段启动代码，§9.2） | 未做：实测 `-smp 2` 不崩，但只有核 0 在跑；port 层 `cpu_start`/`send_ipi` 都返回 -1（§9.2） | 未做：`ls3a5k32` 支持 `-smp` 且建模 IPI，但本机无法执行（§6.1） |
| tickless / waitq / swap | 编译档核对（`ci_gate`），未实跑 | 同左 | 同左 |

### 8.3 四条明确缺口（不要读成「等价完成」）

1. **LA32 没有在本机跑过**：公版 QEMU 没有 `qemu-system-loongarch32`，
   本机也没有 LA32 板子；龙芯教育的 `la32r-QEMU` 是存在的载体，但只发
   Linux 二进制，本机既无 WSL 发行版也无 MSYS2（§6.1）。所以矩阵里 LA32
   那一列**全是「编译」**，它连 `MDK_TRACE` 的时间源都只到「反汇编确认
   `rdtime.d $r12,$r0` 编码合法」这一步（§6.8.5），没有一条运行期读数。
   接上载体后的第一件事写在 §6.8.5 末尾。
2. **SMP 三处都没有实跑证据**：`arkrtos_smp` 只有宿主侧计数；三份板级
   配置 `ARKRTOS_USE_SMP = 0`；三个 port 的 `cpu_start`/`send_ipi` 一律
   返回 -1。QEMU 侧确实能仿真多核（§9.1），但这是**硬件能仿真**，不是
   **我们能跑**——实测 `-smp 2` 在 RV32 上直接崩、在 LA64 上虽不崩但次核
   不参与（§9.2）。
3. **App 整链里"arkc 产出的镜像"那一路未在两个 QEMU 目标上跑，缺口在 arkc 侧**：
   注意这一条**不再覆盖** `exec`（原生 ELF，64/32 位）与 `ld`（可下载 `.o`）：这两个
   已经不需要 arkc —— ELF 由 `loongarch64-newlib-elf-gcc` 直接链出，模块由同一
   工具链编出 `ET_REL`，两者都在 LA64 上实跑通过（见 §8.2 上方两行）。
   仍欠的是 `apppack` / `miniapp` / `apprun` / `flashapp` 这一族"arkc 产出的
   固定 ABI 镜像"。卡点只有一处：arkc 还没有
   RV32 / LA64 后端，以及配套的打包与重定位口径（`cc --pack` 产出的镜像
   装不进 App 内存池、装进去也起不来）。**这条只与 arkc 有关，本项目
   暂时不推进**——不把它算成 QEMU port 这条线的未完成项，也不拿它当
   「移植没做完」的理由。
4. **签名/硬件安全/内核守卫三项只剩「硬件那一半」没取证**：算法与门本身
   已经在 QEMU 上跑出真读数（§8.4.8 ~ §8.4.10）——HMAC 覆盖面对得上、
   篡改会被 `-18` 挡在装载器上、遗言能按 owner 中和、心跳窗口能拒绝并
   定位到具体任务。没取证的是它们的**硬件侧**：真实 OTP/HSM 里那把密钥、
   真看门狗复位、真 MPU 之外的隔离。这些不是「缺 App 链」就能解释的，
   而是 QEMU 上没有对应外设。

另有一条**已修但需记住**的：`-nostdlib` 下 `libgcc` 的内置函数与
`strspn/strcspn` 这类字符串函数都要自己补（§6.8），只在功能面真的打开时
才暴露 —— 最小档跑得再绿，也证明不了全功能档能链起来。本轮把五项功能
真的开起来时，就又被这个坑咬了一次（软总线 `transport_uart.c` 的
`ark_bus_priv.h` 与 `transport_loopback.c` 的漏挂钩）。

### 8.4 逐开关打开实录（AUDIT / CRASH_LOG / MDK_TRACE / SECCHECK / SOFTBUS / IMAGE_SIGN / HW_SAFE / KERNEL_GUARD）

> 这一小节记录「把开关真的拨到 1 之后，为了让它既编得过、又跑得出**真读数**，
> 各自改了什么」。每条都分三段：机制 / 读数 / 边界。

#### 8.4.1 `ARKRTOS_USE_AUDIT=1`（结构自检）

**机制**：`arkrtos_audit.c` 遍历内核全局结构（任务表、信号量、mempool、
定时器……）做一致性核对。它原本假定崩溃日志一定在，`audit_partition()`
会拿「宏定义的地址」和「运行期 `area_base()`」比对；而 `CRASH_LOG=0` 时
`area_base()` 按契约返 0，于是这条比对必然 FAIL —— 这是个**只在该开关组合
下才现形的假失败**。

**修法**：把该比对包进 `#if ARKRTOS_USE_CRASH_LOG`，几何部分（段不重叠、
对齐、容量够）保持无条件检查。

**读数**：RV32 `structures.audit checks=30`、LA64 `checks=29`（差 1 项是
RV32 多了 PMP 相关的边界检查），两边都以 `audit: all structures
self-consistent` 收尾。

**边界**：它核对的是**静态结构一致性**，不是并发安全；跑通不代表没有竞态。

#### 8.4.2 `ARKRTOS_USE_CRASH_LOG=1`（崩溃日志 + 启动次数）

**机制**：日志区必须落在 **UNINIT（`.noinit`）** 段里，否则 C 启动代码会把
它清零，「重启后还在」这个前提就没了。ARM 侧由 `gen_scatter.py` 按
`CRASH_LOG_SIZE > 0` 生成 UNINIT 段；但三个 QEMU 目标用的是手写
`link.ld.in`，**原先根本没有 `.noinit` 输出段**，于是链接出来的日志区
落到了别的段里 —— 编译链接都成功，行为却是错的（清零）。

**修法**（三目标同款）：

1. `link.ld.in`：在 `__text_end` 之后加 `.noinit CRASH_LOG_BASE (NOLOAD)`
   段，并加 `ASSERT(__noinit_end <= (CRASH_LOG_BASE + CRASH_LOG_SIZE),
   "crash journal overlaps the rest of shared RAM")`；
2. `gen_ldconsts.py`：`WANTED` 里补 `CRASH_LOG_BASE` / `CRASH_LOG_SIZE`
   （它们由 `svcrt_partition` 式的地址常量表带进 C）；
3. `main.c`：`arkrtos_kernel_module_init()` 之后依次
   `arkrtos_ptable_init()` → `arkrtos_crash_init()` →
   `arkrtos_crash_scan_apply()`（顺序不能反：`scan_apply` 要读已有
   journal，必须在 `init` 之后、业务起来之前）；
4. `arkrtos_crash.c` 全程只用符号 `arkrtos_crash_journal` 访问，不再用宏
   地址 —— 这样 `CRASH_LOG_BASE` 改到哪里都跟着走。

**读数**：RV32 `crasharea=0x80101e00 boots=1`、`crash.journal boots=1 seq=1`、
`journal: 0x80101E00 +512 bytes`；LA64 `crasharea=0x00301e00`、`journal:
0x00301E00 +512 bytes`。`boots=1` 说明这是第一轮启动（不是 0，也不是更大的
残留值 —— 前者说明没扫到区，后者说明区被别的数据踩了）。

**边界**：只证明「记账面工作」，没有做「真的崩一次再重启看 seq 变 2」的
取证；要做得把 QEMU 停在异常里再 `-kernel` 重跑。

#### 8.4.3 `ARKRTOS_USE_MDK_TRACE=1`（指令/事件追踪）

**机制**：本仓库对 mdk_trace 是**部分 vendored**，原本缺 `mdk_trace_buff.c/h`
（上游在 `mdk_agent` 仓库），而后端在 `config/mdk_trace_config.h` 里被**无条件**
定义成 SWD —— 非 ARM 目标上就会引用到不存在的 `mdk_trace_swd_blob`。
另外内核侧门面 `kernelsrc/src/arkrtos_trace.c` 原先直连 SWD，换后端就要改内核。

**修法**：

1. 从上游复制 `mdk_trace_buff.c/h` 进 vendored 目录；
2. `mdk_trace_config.h` 按架构选后端：`__arm__` → SWD，其余 → BUFF；
3. `arkrtos_trace.c` 改成后端无关门面（`status_line` 三档、capacity/total/
   overrun 从后端取）；
4. **时间源**：RISC-V 原本用 `mcycle`，但**在模拟器里 `mcycle` 数的是被模拟
   的工作量，不是时间**，换算出来的时长是假的。新增 `MDK_TRACE_USE_RDTIME=1`
   走 `csrr %0, time`（CSR 0xC01），与内核节拍同源（QEMU virt timebase
   10 MHz）；LoongArch 用 `rdtime.d`，LA64 virt 定时器 100 MHz。默认值
   仍是 0，对 ARM 目标零变化。

**读数**：RV32 `trace : ARMED, cap=2048 records total=14729 lost=0 wrapped=1`；
LA64 `total=14646 lost=0 wrapped=1`。`lost=0` 且 `wrapped=1` 说明是**溢出后
覆盖**（环形缓冲的正常行为），不是丢记录。

**边界**：`wrapped=1` 意味着最早的一段记录已被覆盖；所以「total 有多大」
不等于「能回看多少」。另外 LA64 的 `MDK_TRACE_CPU_HZ=100000000u` 是按
QEMU virt 的定时器频率填的，换真机要按该核时钟树重填。

#### 8.4.4 `ARKRTOS_USE_SECCHECK=1`（安全自检闸门）—— 本轮唯一改了判据语义的地方

**机制**：`sec_check_gates()` 用一组 bit 表示「各项安全机制是否在岗」，
bit0 原本硬绑 `ARKRTOS_USE_MPU`。但 LoongArch 没有 MPU 这种部件，它靠
**TLB/页表 + PLV** 做隔离 —— 于是在 LA64 上必然报
`gates.kernel FAIL bits=0x3E (need 0x3F)`，而这不是「隔离没开」，是
「判据只认一种实现」。

**修法（两处）**：

1. 新增架构能力宏 `ARKRTOS_ARCH_HAS_PAGING`：
   ARM(M3/M4/M7)=0、RV32IMAC=0、RV64GC=0、**LA32=1、LA64=1**。
   它只表达 **ISA 的能力**，不代表板级真的开了隔离。
2. `sec_check_gates()` 的 bit0 改成双路，并把「是哪一路」作为 `iso` 报出来：

   ```
   #if (ARKRTOS_USE_MPU == 1)
       m |= 1u; iso = 1u;      /* MPU/PMP */
   #elif ((ARKRTOS_ARCH_HAS_PAGING == 1) && (ARKRTOS_USE_PRIV == 1))
       m |= 1u; iso = 2u;      /* 分页 + 特权级 */
   #endif
   ```

   报告串变成 `... bits=0x%X (need 0x%X), iso=%u sign/hwsafe=0x%X`。

**为什么不是「恒 PASS」**：`ARKRTOS_ARCH_HAS_PAGING` 单独为 1 不给 bit0，
必须同时 `ARKRTOS_USE_PRIV=1`。LA32 正是这个组合的反例 ——
`HAS_PAGING=1` 但 `USE_PRIV=0`，bit0 仍为 0，SECCHECK 仍会 FAIL。若只按
ISA 能力给绿，就会造出一个「有页表就等于隔离已开」的假绿。

**读数**：RV32 `12 checks - pass 5, fail 0, finding 4, skip 3`、
`gates.kernel ... bits=0x3F (need 0x3F), iso=1`；LA64 同款但 `iso=2`。
两边 `fail=0`。`finding=4` 是「如实登记但不当失败」的项（例如某硬件安全项
在 QEMU 上不可用），`skip=3` 是无载体项。

**边界**：`iso=2` 只能证明「分页+PLV 这条路在管隔离」，不能证明 TLB 条目
覆盖了所有该覆盖的页 —— 后者由 §5.12 的越权负对照（PPI/refill/IPE 三条）
单独取证。

#### 8.4.5 `ARKRTOS_USE_SOFTBUS=1`（软总线）

**机制**：软总线在 MCU 档位上有一组 `ARK_BUS_*` 宏必须与
`arkrtos_softbus.h` 顶部注释**同档**，否则组件 `#error`。它还需要三个
include 路径（`include` / `src` / `port`）—— `src` 那条容易漏，因为
`transport_uart.c` 直接 `#include "ark_bus_priv.h"`（内部头）。

**修法**：

1. Makefile 加 `ARK_BUS_FLAGS`（14 个 `-DARK_BUS_*`），**必须定义在
   `CFLAGS :=` 之前** —— `:=` 是立即展开，写在后面就是空串，然后组件
   `#error`（这个坑踩过一次）；
2. `SOFTBUS_SRCS` 除 `src/ark_bus_*.c` 还要显式加
   `src/transport/transport_loopback.c`、`src/transport/transport_uart.c`
   与 `port/ark_bus_hal_arkrtos.c`（通配符漏的，漏了就是
   `undefined reference to ark_bus_loopback_init`）；
3. 板级补三个钩子文件 `board/<板>/arkrtos_bus_port.c`（tick_ms / 调度器
   加锁 / 解锁），内容与 `board/stm32f427` 同一份，跨目标通用；
4. `ARKRTOS_TICK_PERIOD_US=500` ⇒ 2 ticks/ms，满足「能整除 1000us」的闸门。

**读数**：三份 `bus selftest` 全部 `9 项 / checks 56, failed 0 / ALL PASS`；
`bus info` 显示 `node id 0x00001001`、`peers 1 (peer 0x00001002 online)`、
`profile msg<=128 topic<=12 dev<=4 qos=2 rpc=4 stream=1`。走的是真实的
帧编解码 + 环缓冲（`transport_loopback`），不是桩。

**边界**：loopback 传输在同一进程内转圈，**没有**验证「跨 UART 物理链路」
的时序与丢帧（`transport_uart.c` 编进去了但没接真实对端）。所以
「软总线在 QEMU 上通」应读作「协议栈 + 多节点注册 + 收发路径通」，
不能读成「链路层已在硬件的波特率/中断下验证」。

#### 8.4.6 刻意留关的：`RTSTAT` / `SCHED_STAT`

这两个的读数是「每任务 CPU 占用 / 调度延迟」，来源是 `cyccnt`。本 port 的
`cyccnt` 返 -1/0（没有可用的周期计数器），打开后算出来的占用率是假的。
**关着不是「没做」，是「做了也是错的」** —— 按「宁可报错不给错答案」，
不如不报。若要开，先给这两个 port 补一个真周期源（RV32 可试
`mcycle`，但要先确认模拟器与真机的语义差；LA64 有 `rdtime`）。

#### 8.4.7 LA32 的反汇编核对（不是运行期读数）

LA32 没有执行载体，所以 `_la_cycles()` 只能到「这指令编码对不对」这一步。
`rdtime.d` 在 LoongArch 里写的是**偶/奇寄存器对**，32 位下必须写进偶号
寄存器（`rdtime.d` 的 rd 最低位保留）。核对方式：

```
loongarch32-newlib-elf-objdump -d build/la32_generic/ark_la32_generic.elf
```

实测出现 `21a050: 0000680c  rdtime.d  $r12,$r0`（rd=`$r12`，偶数），编码
合法。**但这仍然只是「反汇编长得对」，不是「时间戳对」** —— LA32 那一列
在矩阵里必须读作「编译」。

#### 8.4.8 `ARKRTOS_USE_IMAGE_SIGN=1`（镜像签名门）

**机制**：`arkrtos_sign.c` 用 HMAC-SHA256 + 32 字节预共享密钥对镜像算标签，
覆盖面是「header（crc32 / signature / state / runtime_ram_base 四处按 0 代入）
+ 重定位表 + 负载」；`arkrtos_loader_load_buffer` 在 CRC 之后、reserve 之前
调用 `arkrtos_loader_sign_gate()`，失败返回 `ARKRTOS_LOADER_ERR_SIGN(-18)`。

两处必须先说清，否则读数会被读错：

- **密钥不进仓库**。`-DARKRTOS_SIGN_KEY_LOCAL=1` 会 `#include
  "arkrtos_sign_key_local.h"`，三份 `Makefile` 在 `$(BUILD)` 里用 `printf`
  生成这个头（`.gitignore` 已忽略 `*_sign_key_local.h`）。不用
  `-DARKRTOS_IMAGE_SIGN_KEY={0x00,...}` 是因为 bash 会把花括号当展开语法。
- **CRC 覆盖 signature 本身**，所以「只有签名错」这个负对照必须**重算 CRC**；
  反过来「先算 CRC 再签名」会让 CRC 当场失效，安装永远过不去。

**读数**（RV32 与 LA64 各连跑 3 次，第 13 条用例 3/3 PASS）：

```
RUN   sign_hmac_and_loader_gate
PASS  sign_hmac_and_loader_gate
```

用例内部 7 条断言全部走通：整块校验 == 0、流式（begin/update/end）与整块
同口径 == 0、负载翻一字节 ⇒ 失败、签名自己翻一位 ⇒ 失败、重算 CRC 后
`arkrtos_loader_load_buffer()` 返回值**正好等于 `-18`**、签名改回后不再
等于 `-18`。`sec` 里对应的那条现在是
`[PASS] images.signature hmac-sha256, verified at install (symmetric pre-shared key)`。

**边界**：这是**对称**方案——能读到设备里那把密钥的人就能签新镜像。它证明
的是「门真的挂在装载器上、篡改真的过不去」，不是「设备只持公钥的信任链」。
真机还要求密钥来自 OTP/HSM，这一半 QEMU 上没有。

#### 8.4.9 `ARKRTOS_HW_SAFE=1`（设备遗言与死中和）

**机制**：App/驱动在 `claim(dev, owner, obj)` 之后可以 `set_will()` 留下一段
「我死了以后把设备安全化」的遗言。任务死亡时 `on_death(owner)` 只打标记，
`run_pending()` 才真正跑回调（两阶段），这样死亡路径本身不会长时间持锁。
`on_death_reason(owner, handover=1)` 是**计划内交接**：只摘台账，既不驱动
硬件也不计入 `neutralized`。

**读数**（3/3 PASS，`hwsafe_death_neutralizes`）：无 claim 时 `set_will` 返
`ARKRTOS_HWSAFE_ERR_OWNER(-1)`；claim 后 `on_death` 回调 **0 次**、
`run_pending()` 返回 1、回调参数 `(obj, owner)` 对得上、再来一次返回 0
（幂等）、`path_counts` 的 cb 恰好 +1；release 之后死亡不再触发；交接路径
`neutralized` 不涨且台账清空。

**两个只在真跑时才暴露的坑**：

1. **台账不是空的**。内核自己已经认领了控制台设备（`sec` / `hwsafe`
   命令都能看到 `own0/own1: dev=ttyS0 owner=5/7` 这类条目），所以用例里
   写 `owner_count() == 1` 第一次就 FAIL（实测 got 3）。正确写法是**以
   基线为参照**（`oc0 + 1` / `oc0`）。
2. **不能拿控制台任务号当 owner 试**。`on_death` 是按 owner 扫全表的，
   用真实任务号会把它的真设备台账一起标死；用例里的 owner 取
   `ARKRTOS_TASK_MAX_NUM + 1`（不存在的任务）。

**读数（命令行面）**：RV32 `hwsafe: enabled, devs=0 owners=2 neutralized=0`，
`sec` 里 `[PASS] hwsafe.deadman devs=0 owners=2 will/cb=0/0`；LA64 同款
（owner 号不同：3/6）。

**边界**：`devs=0` 是实情——这三个 QEMU 板上没有哪个设备登记了「安全态
回调」，所以 `run_pending()` 在整机层面无事可做；能证明的是机制本身按契约
工作（用例里自带回调）。真机上的「电机停转 / 继电器释放」这类动作没有被
验证过。

#### 8.4.10 `ARKRTOS_USE_KERNEL_GUARD=1`（心跳窗口）

**机制**：任务用 `arkrtos_guard_beat(id, period_ms)` 声明「我应该每
`period_ms` 上报一次」，`period_ms` 必须 ≥ `ARKRTOS_GUARD_MIN_PERIOD_MS`
（100 ms）；`arkrtos_guard_tick()` 由节拍调用（`arkrtos_task.c` 的
`arkrtos_kernel_tick_handler` 里、调度动作**之前**），每 100 ms 服务一次，
逾期打 `ARKRTOS_GUARD_BAD_BEAT` 并累加 `misses`。`arkrtos_guard_init()`
必须由板级 `main()` 显式调用（三份 `main.c` 已加）。

**读数**（3/3 PASS，`guard_heartbeat_contract`）：`beat(self,10)` 返
`ARKRTOS_GUARD_ERR_PARAM`；`beat(self,100)` 返 0；停报 300 ms 后
`verdict` 带 `BAD_BEAT` 且 `violating >= 1`；按 id 遍历 `arkrtos_guard_task_at()`
能找到自己且 `declared==1 / period_ms==100 / misses>0`。

**命令行面的读数**（RV32 与 LA64 一致）：

```
guard
watchdog: disabled by the board (ARKRTOS_WDG_ENABLE = 0)
feeds=0 starves=0 verdict=0x0 audit_mask=0x0 audit_runs=1
contracts: 0 declared, 0 violating now
```

`sec` 里这一项判 `[FIND] guard.watchdog: guard compiled but
ARKRTOS_WDG_ENABLE=0: a hung task is not reset` —— **这是刻意的如实报告**：
守卫的心跳判据在了，但 QEMU 没有硬件看门狗，所以「卡死的任务会被复位」
这件事在本目标上不成立。`contracts: 0 declared` 也是实情：整机没有镜像
声明心跳周期，只有用例临时声明后撤销。

**顺带抓到的一个真竞态（已修在用例里，不是修在内核里）**：打开
`KERNEL_GUARD` 后 LA64 第一次跑 `audit_structures_consistent` 出现
`mask=0x40`（`ARKRTOS_AUDIT_SCHED`），复跑即通过。根因是**自审是一次多步
遍历而不是快照**：任务上下文里跑自审时，节拍中断会插进来改任务表与就绪
位图，遍历就跨过了「一边已改、一边未改」的中间态。守模块本身把自审放在
tick 入口、调度动作之前就是为了避开这一点。用例要判的是「静止态自洽」，
所以改成整段 `ARKRTOS_ENTER_CRITICAL()` 内跑。**这条要读作「自审不是
ISR 安全的」，不是「内核状态坏了」**。

**边界**：QEMU 上没有 `WDG`（`ARKRTOS_WDG_ENABLE=0`），所以能判的是
「窗口会被判、判到具体任务、撤销后不再记账」，判不了「喂狗与否真的会
复位芯片」。

## 9. SMP 能不能「用 QEMU 仿真」

> 这一节回答一个具体问题，答案分两半：**QEMU 侧能不能仿真多核**，和
> **我们的内核在这三个目标上能不能跑多核**。两半的答案不一样。

### 9.1 QEMU 侧：三个机型都能多核

| 目标 | 多核建模 | 依据 |
|---|---|---|
| RV32（`qemu-system-riscv32 -machine virt`） | 有：每个 hart 一份 CLINT/ACLINT 的 `msip`，`-smp N` 起 N 个 hart，IPI 走 `msip` 写 | 实测 `-smp 2` 确实起了 2 个 hart（§9.2 的重复启动输出）；IPI 具体寄存器依 ACLINT 模型 |
| LA64（`qemu-system-loongarch64 -machine virt`） | 有：机器创建 `TYPE_LOONGARCH_IPI`，`num-cpu = smp.cpus`，IOCSR mailbox 挂进 `system_iocsr`，`ipi` 的 GPIO 接 `IRQ_IPI`（`hw/loongarch/virt.c`） | 读的是 QEMU 9.2.3 源码；更早的版本没有这个设备，所以「LA64 没有 IPI 模型」是老版本的结论，按本机这一版不成立 |
| LA32（`la32r-QEMU` 的 `ls3a5k32`） | 有：`mc->max_cpus = 32`、`TARGET_SUPPORTS_MTTCG=y`；`iocsr` 写 `0x1040` 即给目标核置 `ESTAT.IRQ_IPI`（`target/loongarch/iocsr_helper.c`） | 读的是该分支源码；本机不能执行（§6.1） |

### 9.2 我们这边：三个目标都还是单核，实测 `-smp` 不是「加上就能用」

现状（可核）：三份板级配置 `ARKRTOS_USE_SMP = 0`；`arkrtos_smp.c` 文件头
写明**不做**多核调度 / 每核运行队列 / 核间任务迁移 / 全局时钟同步；
三个 QEMU port 的 `arkrtos_port_cpu_start()` / `arkrtos_port_send_ipi()`
一律 `return -1`；两份 `start.S` 都没有「按核号分叉」（RV32 不读
`mhartid`，LA64 不读 `CSR.TID`）。

所以「给 QEMU 加个 `-smp 2`」在两个目标上的实测结果完全不同：

**RV32 `-smp 2`：崩。** 两个 hart 同时从复位向量执行同一段启动代码，
串口被两路打印撕成 `[A[rAkr kR TROTSO]S ]b obaoradr:d …`，随后：

```
[PMP] fault mcause=1 mtval=0x00000000 mepc=0x00000000 ra=0x8002b1a2 from=M task=4
[PMP] fault mcause=1 mtval=0xcdcdcdcc mepc=0xcdcdcdcc ra=0x8002b1a2 from=M task=4
[Ark RTOS] FATAL: mcause=0x1 mepc=0x0 …
[Ark RTOS] halted.
```

原因是 RISC-V 规范里所有 hart 复位后同时开始执行，谁先清完 `.bss`、
谁先建调度器都不确定；这不是 QEMU 的问题，是我们的启动代码只按单 hart
写的。

**LA64 `-smp 2`：不崩，但也不是多核在跑。** 同一份镜像照样
`[FS] PASS`、`utest: 13 matched, 13 passed, 0 failed, 0 skipped;
assertions 133 run / 0 failed`，节拍读数与单核一致（`[A] round=21
tick=21018 d_tick=1000` 等），日志里没有第二个核的任何痕迹。**这条只说明
「没被破坏」，不说明「次核起来了」**：本 port 没有任何启动次核的代码
（`cpu_start` 返 -1），也没有核间中断（`send_ipi` 返 -1）。次核本轮观测
始终不参与，QEMU 侧的保持机制没有逐行查明，不作结论。

带 `-smp 2` 的复现命令（与 §7.1 / §7.2 只差一个参数）：

```bash
cd build/qemu_riscv32 && (sleep 6; printf 'utest\n'; sleep 20) | timeout 34 \
  "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe" \
  -machine virt -cpu rv32 -smp 2 -bios none -kernel ark_qemu_rv32.elf -nographic
# 期望：两路启动打印交错 -> [Ark RTOS] FATAL -> halted（exit 124）

cd build/qemu_loongarch64 && (sleep 6; printf 'utest\n'; sleep 20) | timeout 34 \
  "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -machine virt -smp 2 -nographic -kernel ark_qemu_la64.elf
# 期望：与单核完全一致的 13/13 通过（次核未参与，不代表 SMP 可用）
```

### 9.3 结论

- **「QEMU 能不能仿真 SMP」：能。** 三个机型都有多核与 IPI 建模
  （§9.1），LA32 那台连 32 核都写了。
- **「我们的内核能不能在这三个目标上跑 SMP」：不能，第一步都还没做。**
  要真的看到双核参与（而不只是「没崩」），至少缺四件事，缺一不可：
  1. **次核停靠**：`start.S` 按核号分叉（RV32 `mhartid`、LA32/LA64
     `CSR.TID`），非 0 核先停在等待位，而不是直接跑 `main`；
  2. **真核号**：`arkrtos_port_cpu_id()` 返回硬件核号（现在三处都写死
     `return 0`）；
  3. **IPI 通路**：`arkrtos_port_cpu_start()` / `arkrtos_port_send_ipi()`
     的真实现（RV32 = ACLINT `msip`；LA64 / LA32 = IOCSR IPI mailbox）；
  4. **调度分核**：`arkrtos_smp.c` 明确不做的那一半（每核运行队列、
     核间任务迁移、全局时钟同步）。
- 因此矩阵里 SMP 那一行**三列都是「未做」**，而且它不是「加个 `-smp`
  就有的东西」——按上面四条估，它是独立的一件事，不该混在 QEMU port 这条
  线里顺手宣称。

### 9.4 本轮本机核查（2026-11-17）：判 `smp_not_implemented_four_gaps`

§9.1 / §9.2 / §9.3 是「上游能不能仿真」「我们跑到哪一步」「差哪四件」。
本轮的核查只补一件事：**把「跑过 `-smp 2`」和「支持 SMP」彻底分开记账**。

判词：`smp_not_implemented_four_gaps`（缺口 = §9.3 的四条，一件未做）。
配套三条不许读法，写死在这里：

- `-smp 2` 在 RV32 上**崩**，不能读成「SMP 有问题」——崩的是单 hart 启动代码
  被两个 hart 同时执行（§9.2 的串口撕裂与 `mepc=0x0`）；
- `-smp 2` 在 LA64 上**13/13 照旧通过**，不能读成「SMP 可用」——日志里没有
  第二个核的任何痕迹，`cpu_start` / `send_ipi` 都还返 -1；
- 两件事都**不改变**矩阵里 SMP 那行的「未做」。

若要在本机推进，最小可验证子步是 §9.3 的 ①②（次核停靠 + 真核号），
**不碰** ③IPI 与 ④调度分核：

- 开关仍是 `ARKRTOS_USE_SMP`，默认 0，默认档体积与行为零变化；
- 判据两条：`-smp 2` 时串口出现次核的 `parked` 读数（带硬件核号，
  LA64 走 `CSR.TID`）；`-smp 1` 的镜像与日志**逐字节不变**；
- 这一步结束时也不能说「多核在跑」——只能说「次核起来了并且停在该停的地方」。

前置条件（本轮未做，先登记）：它会改
`example/qemu_loongarch64/start.S` 与 `kernelsrc/src/arkrtos_smp.c`，
直接威胁当前单核 QEMU 全绿基线（`utest` 13/13、`pmap` 系列、
`ci_gate` 43 项）。按「默认档零变化 + 不破坏既有基线」的约定，
**须用户明确批准动启动路径后再开工**；批准前的状态就是
`smp_not_implemented_four_gaps`。

### 9.5 最小子步实测（2026-11-17）：判 `smp_park_unreachable_on_qemu_kernel_boot`

用户批准做 §9.3 的 ①②（次核停靠 + 真核号），不碰 ③IPI 与 ④调度分核。
代码写完了，但**第一条判据在本机不可达**——原因不是代码没写，是 QEMU 不给这条路。

#### 9.5.1 决定性事实：QEMU 直启时次核的 PC 不是我们的 entry

读的是 QEMU 9.2.3 源码（`hw/loongarch/boot.c`）：

- `loongarch_direct_kernel_boot()` 给每个 CPU 都设 `env.load_elf = true`，
  但**只有 `first_cpu` 拿到 `elf_address = kernel_entry`**，其余核拿到
  `elf_address = VIRT_FLASH0_BASE`（pflash 的 BIOS 区）；
- 那块 pflash 里放的是同一文件开头的 `slave_boot_code`（指令数组）：关中断、
  清 mailbox、开 IPI，然后 `idle` 等 mailbox 非零，最后 `jirl` 跳到 mailbox 里的地址。

所以 `-smp 2` 时**次核从来没执行过 `start.S`** —— 这也是 §9.2「LA64 不崩、日志
无痕」的真正解释（当时只有现象，没有原因）。要让次核进入我们的代码，唯一的路是
**往 IOCSR mailbox 写入口地址再发 IPI**，那属于 §9.3 第 ③ 条，本轮明确不做。

#### 9.5.2 本轮实际落地（全部在开关下，默认档不编译）

1. `example/qemu_loongarch64/start.S`：`_start` 开头读 `CSR.TID`（0x40），非 0 核
   不清 `.bss`、不跑构造函数表、不进 `main`，直接 `idle` 死等。注释写明本机不可达
   及原因。
2. `kernelsrc/port/loongarch/la64/arkrtos_port.c`：`arkrtos_port_cpu_id()` 在
   `ARKRTOS_USE_SMP == 1` 时读 `csrrd 0x40`（QEMU 在 cpu reset 里写
   `env->CSR_TID = cs->cpu_index`，`target/loongarch/cpu.c`），关档仍 `return 0u`。
3. `kernelsrc/src/arkrtos_smp.c`：`arkrtos_smp_init()` 改用
   `arkrtos_port_cpu_id()` 取「本核」核号，不再写死 0（写死会让在线掩码与每核
   状态表整体错一格）。
4. `board/qemu_loongarch64/arkrtos_board_config.h`：`ARKRTOS_USE_SMP` 的
   `#undef` + `#define 0` 改成 `#ifndef` 守卫。不改的话
   `EXTRA_CFLAGS=-DARKRTOS_USE_SMP=1` 会被板级无条件覆盖，症状是「编出来了，
   但和目标文件一字节没变」。

这是第二次踩「宏被后 include 的头覆盖」（第一次是 U-3 的
`ARKRTOS_USE_PAGING_DEMAND`），已写死为约定：**板级配置头里凡是允许命令行覆盖
的开关，一律用 `#ifndef` 守卫**。

#### 9.5.3 实跑读数

```bash
make -f example/qemu_loongarch64/Makefile BUILD=build/qemu_la64_smp2 \
     EXTRA_CFLAGS="-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2" -j8
```

`size`：SMP 档 text **366440** / data 416 / bss 767508；默认档 text 366120
（+320 B = `arkrtos_smp.c` 全部代码 + `cpu_id` 的 CSR 读）。

`-smp 1` 与 `-smp 2` 各跑一次 `utest`，两次都是：

```
utest: 13 matched, 13 passed, 0 failed, 0 skipped; assertions 133 run / 0 failed
```

`-smp 2` 的串口里**没有第二个核的任何输出**，与 §9.5.1 的源码事实一致。必须把
话说死：**13/13 通过只证明「SMP 档代码没破坏单核」，不证明次核起来了。**

#### 9.5.4 关档零变化（口径同 §5.23.6）

`SOURCE_DATE_EPOCH=1600000000` 各建一版，`objcopy --strip-debug` 后比 sha256：

| 版本 | stripped.elf |
|---|---|
| 改动前（`77f2384` 的构建） | `027dcf53…6114b`，460264 B |
| 本轮改动后（默认档） | `027dcf53…6114b`，460264 B |

逐字节相同 ⇒ 默认档体积与行为零变化。

#### 9.5.5 门禁 pin U（`tools/smp_gate.py`）

新增 LA64 段，五条断言 + 一条自检：

1. `start.S` 三条硬标记必须在：`csrrd   $t0, 0x40`、`beqz    $t0, 7f`、`idle    0`；
2. **自检**：把 `csrrd` 换成 `nop` 的临时副本必须被判缺失（否则这道门禁是假绿）；
3. `start.S` 用 LA64 gcc 以 `-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2` **真汇编**，
   产物反汇编里必须同时出现 `csrrd` 与 `idle`（防「被 `#if` 吃掉」）；
4. `arkrtos_port.c` 同档 `-fsyntax-only` 真编译（这条曾抓到 `LA_CSRR` 宏定义在
   函数之后导致的 `undefined reference to LA_CSRR`）；
5. 关档 `cpu_id` 仍必须有 `return 0u;`（默认档不许读 CSR）。

`py -3 tools/smp_gate.py` 全过；`ci_gate.py` 43 项里的 `smp` 项随之变成
「44 条宿主断言 + 5 个 ARM 文件 + LA64 pin U」。

#### 9.5.6 判词与三条不许读法

判词：**`smp_park_unreachable_on_qemu_kernel_boot`** —— ①② 的代码已落地，但 ①
在本机（QEMU `-kernel` 直启）没有可触发路径，② 只能以「读到 0、与写死 0 行为
一致」的形式验证，**区分性未验证**。§9.4 的 `smp_not_implemented_four_gaps` 里
③④ 仍然成立，所以矩阵里 SMP 那一行的结论**不变**。

不许读法：

- 不能把 `-smp 2` 的 13/13 读成「次核停靠成功」—— 次核在 QEMU 的
  `slave_boot_code` 里，从未进过我们的镜像；
- 不能把 `csrrd 0x40` 在 CPU0 上返回 0 读成「真核号已验证」—— 只有一个核在跑我们
  的代码，读到的值没有区分度；只能写「实现存在，区分性未验证」；
- 不能把本轮读成「SMP 有进展所以矩阵那一行可以改」—— ③④ 未做，改法不变。

下一步若要真看到次核进我们的代码，最小实验是：主核往 IOCSR mailbox
（`CORE_BUF_20`）写入一个 C 入口地址，再对 `CORE_EN_OFF` 发 IPI；次核从
`slave_boot_code` 跳出后进该入口，并自己建栈与 TLB 映射。那已经是 §9.3 第 ③ 条
的工作量，须另行批准。

### 9.6 ②③ 做实（2026-11-19，QEMU LA64 实跑）：判 `smp_bringup_ok_on_qemu`

> 用户原文：「**SMP用模拟的做掉啊**」。范围写死：§9.3 的 ② 次核启动与 ③ IPI 通路
> 做实并给出串口证据；**④ 调度分核明确不做**（设计边界，不是没做完）。

#### 9.6.1 决定性事实：QEMU `-kernel` 直启下把次核拉起来的唯一路径

§9.5.1 已经查明次核的 PC 是 pflash 里的 `slave_boot_code`，不是我们的 entry。
本轮把那段固件代码逐条读完（`hw/loongarch/boot.c`，QEMU 9.2.3），它做的事是：

1. `csrwr $t0, EENTRY`：把 EENTRY 写成 0（即 TLB refill 入口全指向 0）；
2. 清 `CRMD.IE`；
3. 清自己的 mailbox：`iocsrwr.d $zero, 0x1020`；
4. 开 IPI：`ECFG` 置 bit12（`LIE[12] = IPI`），并往 `0x1004 CORE_EN` 写 `0xfff`；
5. 循环：`idle 0` → 读 `0x1020` → 为 0 就继续 idle；
6. mailbox 非 0 时：读 `0x1000` status → 写 `0x100C` clear → 关 `ECFG` bit12 →
   从 `0x1020` 取入口地址 → `jirl $zero, $ra, 0` **直接跳进去**。

所以「拉起次核」在内核侧只有两步，**与 `start.S` 无关**：

```
往 0x1048（MAIL_SEND，64 位）写  (入口地址 << 32) | (核号 << 16) | 字节掩码
往 0x1040（IPI_SEND， 32 位）写  (核号 << 16) | 向量号
```

次核跳进我们的入口时 `CRMD.DA = 1`（地址翻译关）、`CRMD.IE = 0`，`sp` 是固件
留下的垃圾值、`EENTRY` 是 0、`.bss` 与 `.data` 已由 CPU0 初始化好。入口因此必须
**自己建栈、自己装 EENTRY、自己开 ECFG/CORE_EN/CRMD.IE**，且地址必须是物理地址。

#### 9.6.2 IOCSR 地址与两个写的语义（以 QEMU 头文件为准）

`include/hw/intc/loongson_ipi_common.h` / `hw/intc/loongson_ipi.c`：

| IOCSR 偏移 | 名字 | 宽度 | 语义 |
|---|---|---|---|
| `0x1000` | `CORE_STATUS` | 32 | 读：本核挂起位图（bit n = 向量 n 待处理） |
| `0x1004` | `CORE_EN` | 32 | 写：允许哪些向量拉中断（本核） |
| `0x1008` | `CORE_SET` | 32 | 写：软件置挂起位（不走中断） |
| `0x100C` | `CORE_CLEAR` | 32 | 写：清挂起位（EOI 等价物） |
| `0x1020` | `CORE_BUF_20` | 32/64 | 次核启动 mailbox（固件读它取入口） |
| `0x1040` | `IPI_SEND` | 32 | 写：给目标核置 `BIT(向量)`，该核 `CORE_EN` 命中则拉中断 |
| `0x1048` | `MAIL_SEND` | 64 | 写：`(数据 << 32) \| (核号 << 16) \| (bit27..30 字节掩码)` |

两条容易踩的语义：

- **`IPI_SEND` 只置位、不计数**：同一向量连发 N 次，目标核只看到一次
  （`status \|= BIT(vector)`，位已经为 1 就不再有新动作）。所以「发 5 次收到 1 次」
  是硬件语义，不是通路故障。要数往返，必须一发一等。
- **`MAIL_SEND` 的 bit27..30 是「保留旧值的字节掩码」**，全 0 才是四个字节全写新值。
  本 port 传 0，即整 32 位入口地址一次写进去。

`iocsrrd/rd.w|d`、`iocsrwr.w|d` 的寄存器约定是 **rd = 值、rj = 地址**（helper 分别
走 `address_space_ld*/st*`）。`iocsrwr.d` 在 `0x1048` 上落成一次 8 字节写。

#### 9.6.3 落地（五处，全部在 `ARKRTOS_USE_SMP == 1` 下）

1. **`kernelsrc/port/loongarch/la64/arkrtos_port.c`**
   - 宏 `LA_IOCSR_IPI_STATUS/CLEAR/SEND`（0x1000/0x100C/0x1040）与
     `LA_IOCSR_MAIL_SEND`（0x1048），加 `la_iocsr_write_w/d` 两个内联封装；
   - `arkrtos_port_cpu_start(cpu)`：先 `MAIL_SEND` 写入
     `(arkrtos_smp_secondary_entry << 32) | (cpu << 16)`，再 `IPI_SEND`
     写 `(cpu << 16) | 1`（向量 1 专用于「启动」）；
   - `arkrtos_port_send_ipi(cpu)`：`cpu == 0` 或越界返回 -1，否则同样写 `IPI_SEND`。
   两个函数的原 `return -1` 桩删除（SMP 档下）。
2. **`kernelsrc/port/loongarch/la64/arkrtos_context.S`**
   - `eentry:` 开头插入分流：存 `$t0` → `csrrd $t0, 0x40` → 非 0 跳
     `la_smp_trap_entry`，是 0 则走原有单核路径（这条分支对默认档不存在）；
   - `la_smp_trap_entry`：同样的 256 字节帧（全 GPR + ERA + PRMD），但**不**调
     `la_trap_c`——读 `0x1000` status → 写 `0x100C` clear → `bl
     arkrtos_smp_ipi_handler` → 全量恢复 `ertn`；
   - `arkrtos_smp_secondary_entry`：`csrrd 0x40` 取核号 → `sp = arkrtos_smp_stacks
     + (tid+1) * ARKRTOS_SMP_SEC_STACK_SIZE` → `csrwr eentry, 0xC`（EENTRY）→
     `csrxchg` 置 ECFG bit12 → `iocsrwr.w 0xfff, 0x1004`（CORE_EN）→
     `csrxchg` 置 CRMD.IE → `bl arkrtos_smp_secondary_main` → 死循环。
3. **`kernelsrc/src/arkrtos_smp.c`**：`arkrtos_smp_stacks[ARKRTOS_CPU_NUM]
   [ARKRTOS_SMP_SEC_STACK_SIZE]`（.bss，不占镜像体积）。
4. **`board/qemu_loongarch64/arkrtos_board_config.h`**：`ARKRTOS_SMP_SEC_STACK_SIZE`
   = 8192（C 与 `.S` 共用的唯一位置）；`arkrtos_smp.h` 里补一个同样的 `#ifndef`
   兜底，只为让不带板级头的宿主用例编得过（两边必须一致）。
5. **`example/qemu_loongarch64/main.c`**：`arkrtos_kernel_module_init()` 之后
   `#if (ARKRTOS_USE_SMP == 1)` 跑一次 `arkrtos_smp_self_test()`。

**为什么次核不走 `la_trap_c`、也不调 `arkrtos_port_irq_init()`**：`la_switch_stack_top`
与 `s_in_trap` 都是**全局唯一**（`port.c:172`），调度表和就绪位图也是单核一份。
次核一旦进 `la_trap_c`，就会在「共享的中断栈 + 共享的 in_trap 计数」上和 CPU0
打架；而 `arkrtos_port_irq_init()` 会开 `CRMD.IE` 且要过 `s_in_trap`，CPU0 恰好在
trap 里时次核就永远收不到 IPI。所以次核只做「清挂起 → 计数 → 回到 idle」。

#### 9.6.4 本轮的真缺陷：`atomic_cas` 汇编写错，CAS 返回成功但内存没变

现象（修好启动链路之后才暴露）：次核确实上线了，`arkrtos_smp_cpu[1].online == 1`、
`started == 1`、`idle_count` 在涨（**普通 store 次核写、主核读得到**），但
`arkrtos_smp_online_mask` 恒为 0，于是自检打印
`cores=2 mask=0x00000000 result=FAIL(secondary offline)`。

根因在 `arkrtos_port_atomic_cas()`：原实现

```c
__asm__ volatile("sc.w %0, %2, 0" : "=&r"(fail) : "r"(p_addr), "r"(new_value));
```

`sc.w rd, rj, si14` 的 **rd 既是待写值、也是成功标志**（成功写 0、失败写 1）。
这里 rd 填的是输出变量、rj 填的是 `new_value`，于是 **把 `new_value` 当地址**、
把 rd 里的垃圾当数据写出去；内存原封不动，函数却返回「成功」。失败分支更糟：
`sc.w $zero, %0, 0` 会真的把 0 写进 `*p_addr`。

修法（两处）：

```c
if (old != expect)
{
    return 0u;   /* 不写回；ll 的保留由本核下一次 ll/sc 或 ertn 自然失效 */
}
__asm__ volatile("or   %0, %2, $zero\n\t"
                 "sc.w %0, %1, 0"
                 : "=&r"(fail) : "r"(p_addr), "r"(new_value) : "memory");
```

新汇编逐条核过：`ll.w r12,r4,0` → `bne` → `move r12,r6` → `sc.w r12,r4,0` →
`bnez r12,retry`。

**代价必须写明**：这个修复落在 `kernelsrc/port/loongarch/la64/arkrtos_port.c` 的
公共代码上，**默认档镜像因此不再是逐字节不变**（见 §9.6.8）。这不是 SMP 开关
带来的差异，而是一个本来就坏的函数被修好了——`atomic_cas` 在默认档也会被
`mempool`/`sync` 一类路径用到，只是此前没人靠它的返回值决定正确性。

#### 9.6.5 自检脚本为什么是「一发一等」

`arkrtos_smp_self_test()` 对每个次核、每个向量 1..5：发一次 → 有界自旋等
`ipi_count` 涨到这一格 → 再发下一次。原因见 §9.6.2 第一条：同向量连发会被
合并成一次中断，「连发 5 次收到 1 次」不是故障。等待有上限
（`SMP_SELF_TEST_SPIN = 20000000` 次 `pause` 语义的 spin hint），超时即判
`FAIL(ipi not received)`，不会挂死。

打印一行：`[smp] cores=N mask=0x%08x sent=%u ipi1=%u result=ok|FAIL(...)`。

#### 9.6.6 实跑读数

构建（`SOURCE_DATE_EPOCH` 固定时间戳，便于对拍）：

```bash
make -f example/qemu_loongarch64/Makefile BUILD=build/qemu_la64_smp \
     EXTRA_CFLAGS="-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2" -j8
```

运行（必须 `-nographic`，否则 `-serial file:` 与 `-D` 会莫名失败，见 §9.6.9 附注）：

```bash
timeout 25 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -M virt -m 128M -smp 2 -nographic -display none \
  -serial file:build/_smp_run9.txt -kernel build/qemu_la64_smp/ark_qemu_la64.elf
```

串口原文（第 6 行）：

```
[smp] cores=2 mask=0x00000003 sent=1 ipi1=5 result=ok
```

同一份镜像带 `utest` 交互跑（`-smp 2`）：`[FS] PASS` 与
`utest: 13 matched, 13 passed, 0 failed, 0 skipped; assertions 133 run / 0 failed`
照旧（SMP 档没有破坏单核基线）。

**时效说明（2026-11-21）**：上面这条 `ipi1=5 result=ok` 是 11-19 的读数，在 11-21 的
HEAD 上**已不可复现** —— 那天之后加的 FPU/LSX 上下文保存让次核在第一次陷阱里落
Ecode=15，整机在 IPI 处挂死（见 §9.6.11）。修掉之后再跑同样命令可以复现本条读数，
所以这不是环境差异，是一次回归；在 §9.6.11 落地之前，本条读数只对 11-19 的镜像成立。

**次核到底有没有跑到我们的代码**：`-d in_asm -D build/_asm2.log` 抓 TB 记录，
日志里出现这一串（`-d in_asm` 不打印 CPU 号，靠符号名与地址区分）：

```
IN: 0x1c000000            <- pflash slave_boot_code 复位入口
IN: 0x1c000038            <- idle 0（pollloop）
IN: 0x1c000048            <- mailbox 非 0 后的分支（跳过 beqz 才算真收到）
IN: arkrtos_smp_secondary_main
IN: 0x00244f1c            <- arkrtos_smp_secondary_entry 之后
...
IN: smp_mask_set
```

镜像符号（`build/qemu_la64_smp/ark_qemu_la64.elf`，nm）：

```
0x00244c90 T eentry
0x00244df4 t la_smp_trap_entry
0x00244f1c T arkrtos_smp_secondary_entry
0x00210390 T arkrtos_smp_secondary_main
0x00210200 T arkrtos_smp_init
0x00320d60 B arkrtos_smp_stacks
0x00324d60 B arkrtos_smp_online_mask
0x0023d6d0 T arkrtos_port_atomic_cas
```

#### 9.6.7 门禁：`tools/smp_gate.py` 新增 pin V

pin U（§9.5.5）钉的是「次核停靠分支存在」，但那条分支在 `-kernel` 直启下不可达。
pin **V** 钉本轮真正起作用的两处：

1. `kernelsrc/port/loongarch/la64/arkrtos_context.S` 以
   `-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2` **真汇编**，反汇编里必须同时有
   `arkrtos_smp_secondary_entry`、`iocsrwr`、`ertn`；
2. **自检**：同一份文件以 `-DARKRTOS_USE_SMP=0` 汇编，上述标记必须**全都不在**
   ——否则说明标记来自无条件代码，钉错了地方；
3. `arkrtos_port.c` 文本里 `LA_IOCSR_MAIL_SEND` / `LA_IOCSR_IPI_SEND` /
   `LA_IOCSR_IPI_CLEAR` 三个宏名必须在；**自检**：同一个判据把其中一条抠掉后
   必须报缺失（第一次写这条自检时用 `replace(..., 1)` 只删一处，而
   `LA_IOCSR_MAIL_SEND` 在文件里出现两次，自检因此「没报」——当场被自己抓到）。

`py -3 tools/smp_gate.py` 全过；`tools/ci_gate.py` **47 项 0 失败 0 跳过**，
其中 `smp` 项现在是「44 条宿主断言 + 5 个 ARM 文件 + LA64 pin U/V」。

#### 9.6.8 默认档镜像：这次**不是**逐字节不变（如实记账）

| 版本 | `objcopy -O binary` | sha256 |
|---|---|---|
| 本轮之前（`980d3f9`） | 31,462,512 B | `9e59d693…` |
| 本轮之后（默认档，无 SMP 宏） | 31,462,512 B | `69919f39…fedc6c3` |

`cmp -l` 共 27 个字节不同，**全部落在 `0x23D495..0x23D4BB`**，即
`arkrtos_port_atomic_cas` 函数体内部（nm/objdump 核过地址）。结论：

- 「默认关时体积与行为零变化」这条**本轮不成立**，原因是修了 `atomic_cas` 的真
  缺陷，不是 SMP 开关引入的差异；
- 体积（31,462,512 B）与段布局都没变，变化只在那个函数自己的指令编码上；
- 想回到「零变化」只有一条路：让 `atomic_cas` 继续保持坏着——不做。

复现（两次连续构建得到同一个 sha256，可重复）：

```bash
SOURCE_DATE_EPOCH=1600000000 make -f example/qemu_loongarch64/Makefile \
  BUILD=build/qemu_loongarch64 -j8
D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-objcopy -O binary \
  build/qemu_loongarch64/ark_qemu_la64.elf build/_def_img.bin
sha256sum build/_def_img.bin
```

#### 9.6.9 判词与五条不许读法

判词：**`smp_bringup_ok_on_qemu`** —— LA64 / QEMU `-smp 2` 上，② 次核启动与
③ IPI 通路**实跑通过**（掩码 `0x3`、5 次 IPI 往返全收到）；④ 调度分核当时**未做**
（设计边界，2026-11-19 晚已由 §9.8 的 AMP 模块补上）；真实硬件**未验证**；RV32 / LA32 侧未动。

不许读法：

1. **不能读成「支持 SMP 多核」**：调度器仍是全局单核一份（当前任务号、就绪位图、
   延迟队列、`la_switch_stack_top`、`s_in_trap`），次核只做「上线 + 计数 + idle」，
   不碰对象表、不跑任务。上线掩码非 0 不等于两个核在并行干活。
2. **不能把 `mask=0x3` 读成「次核跑过 `start.S`」**：它没有。次核走的是固件的
   `slave_boot_code` → `arkrtos_smp_secondary_entry`，`start.S` 里的停靠分支在这条
   路径上依然不可达（pin U 只是防止它被删掉）。
3. **不能把 `ipi1=5` 读成「向量 1 发了 5 次中断」**：是「一发一等」的发 5 个往返；
   同向量连发会被硬件合并。
4. **不能读成真板可用**：IOCSR 的 `MAIL_SEND` 字节掩码、`IPI_SEND` 的置位语义、
   `slave_boot_code` 的行为都来自 QEMU 9.2.3 源码；真实固件是否同样写 mailbox、
   同一套偏移，未验证。
5. **不能把本轮读成「RV32 也能了」**：RISC-V 那套是 ACLINT/CLINT `msip`，与 IOCSR
   完全不同。RV32 侧的 ②③ 是另一轮做的（2026-11-19，见 §9.7），本节读数与本条判词
   都不覆盖它。

附注（环境坑，复现时会撞上）：`-monitor none -display none -serial null`
与 `-D`/`-serial file:` 组合会让 QEMU 直接 rc=1 且目标文件不生成（表现为
`mv: cannot stat`），同样参数换成 `-nographic` 就正常——所有实跑统一用
`-nographic -display none -serial file:...`。另外本机
`loongarch64-newlib-elf-gdb.exe` 缺 DLL 跑不起来（rc=127），HMP 的 `cpu N`
也切不动核，所以「次核有没有跑」只能靠 `-d in_asm` 的 TB 符号名 + 代码里打读数。


#### 9.6.10 未解决：`-smp 2` 下约 10% 的间歇停机（已登记，不是已修复）

这一节记的是**本轮没能修掉的东西**，以及证明它不属于 SMP 实现的证据。结论是
「与 SMP 代码无关，但也没有定论到 QEMU 侧 bug」，按未解决登记。

**失败签名（四类，全部有落盘日志）**

| # | 现场 | 读法 |
|---|---|---|
| 1 | `[LA64U] KERNEL refill (plv0) badv=0x1648cf48 era=0x00215920` | `era` 落在 `arkrtos_sched_activate+0x80` 的 `st.w table[tid].status` 附近 |
| 2 | `estat=0x000d0000 ecode=0x0d(INE) era=0x0021f080 badv=0x0021f080 badi=0x00000001 crmd=0x00000010` | `0x21f080 = mdk_trace_send.part.0+0x180`，指令是 `4c000020 jirl $r0,$r1,0`（函数返回） |
| 3 | `ecode=0x0d era=0x00000008 badv=0x00000008 badi=0x0 crmd=0x10`，`frame@0x003b5710` | 任务 4 的初始帧槽；帧里 PRMD 槽 = `0x00323fdc`（即 `arkrtos_ready_heal_g` 的地址）、ERA 槽 = `8` |
| 4 | `[LA64U] KERNEL refill (plv0) badv=0xffffff04 era=0x00244894`，随后 `ecode=0x0d era=0x00000100 badi=0x29c00061 crmd=0x08` | `era` 在 EENTRY `0x244890` 里 ⇒ 异常入口中二次异常；发生在 shell 提示符刚出、任何命令之前 |

第 3 类与 §5.13.4 那条 `ecode=0x0d era=0 badv=0 badi=0 crmd=0x10` 是同一家族
（帧在栈顶下 `0x100` 的初始帧、ERA 槽是垃圾值、返回目标无效）。§5.13.4 的根因
至今未定，本轮的多核读数只是给那一族多添了几个样本，**没有把它解释掉**。

**对照表（同一镜像、同一命令行，只换 `-smp` 与 `-accel`）**

| 构型 | 崩溃 | 备注 |
|---|---|---|
| 默认镜像 `-smp 1` | 0 / 6 | 基线 |
| SMP 镜像 `-smp 1` | 0 / 24 | 每轮 `mask=0x1 result=FAIL(secondary offline)`，无次核可用 |
| SMP 镜像 `-smp 2`（MTTCG） | **3 / 20** | 另 17 次 `result=ok` |
| SMP 镜像 `-smp 2` + `SMP_DBG_NO_IPI=1` | 1 / 20 | IPI 不是必要诱因 |
| **默认镜像 `-smp 2`（MTTCG）** | **2 / 20** | **SMP 代码根本没编进去** |
| 默认镜像 `-smp 2` + `-accel tcg,thread=single` | 0 / 20 | |
| SMP 镜像 `-smp 2` + `-accel tcg,thread=single` | **1 / 10** | 且 10 / 10 全是 `mask=0x1 result=FAIL(secondary offline)` |

两个决定性的读数：

1. **默认镜像（`ARKRTOS_USE_SMP=0`）在 `-smp 2` 下同样会崩（2/20）** ⇒ 崩溃不可能
   由本轮 SMP 代码引入。
2. **`-accel tcg,thread=single` 不是解药**：默认镜像上它 20 轮不崩，但换到 SMP
   镜像上仍然 1/10 崩，而且 **10 轮全部 `secondary offline`**——单线程 TCG 下
   CPU0 自旋等次核时 CPU1 得不到调度，次核永远上不了线。即：SMP 上线**依赖真并发**，
   而真并发（MTTCG）才带那 ~10% 的间歇停机，两者不可兼得。

**更能说明问题的一条：默认镜像在单核下也会崩。** 收尾阶段用默认档镜像
（`ARKRTOS_USE_SMP=0`，命令行**没有** `-smp`，即单 vCPU）跑 `utest`，三次里命中一次：

```
[Ark RTOS] FATAL: estat=0x000d0000 ecode=0x0000000d era=0x00000008 badv=0x00000008 badi=0x00000000 crmd=0x00000010
```

同一条命令另外两次是 `utest: 13 matched, 13 passed, 0 failed, 0 skipped; assertions 133 run / 0 failed`。这个签名（`ecode=0x0d`、`era=8`、`badv=8`）与
§5.13.4 那条 `era=0` 的家族同源，也与本节的第 3 类完全一致 ⇒ **间歇停机既不需要
两个 vCPU、也不需要 MTTCG、更不需要 SMP 代码**。它是一条独立的、早就存在的老缺陷，
本轮只是把它在多核场景下多撞了几次。凡把 `-smp 2` 的崩溃算到 SMP 头上、或反过来
拿单核「跑通一次」证明 SMP 稳定的说法，都被这条读数否掉。

（本条是对本轮中途一次短读数的修正：先跑的前几轮曾读出「SMP 镜像 + `thread=single`
不崩」，10 轮跑满后是 1/10，与 MTTCG 下的 3/20 同一量级，属抽样波动。）

**次核到底在跑什么——HMP 实测，不是推断**

默认镜像 + `-smp 2`，起到 `ark@arkrtos` 提示符后用 HMP `info registers -a` 连读三组：

```
CPU#0  PC=0x0023d464  CRMD=0x14(IE=1)  ESTAT=0x800(TI)  EENTRY=0x00244890  TICLR/TCFG=0xc353
CPU#1  PC=0x1C00003C  CRMD=0x8(PLV0,DA=1,IE=0)  EENTRY=0x0  ESTAT=0x0  TCFG=0x0  TVAL=0x0
       GPR12=0xffffffffffffffff  GPR13=0x0000000000001020
```

三组采样里 CPU#1 的 PC 与全部 32 个 GPR **逐位不变**。`0x1c000000` 是
`virt.flash0`（romd，16 MB，`info mtree -f` 核过），`xp /32wx 0x1c000000` 读出：

```
0x0400302c 0x0380100c 0x04000180 0x1400002d  0x038081ad 0x06481da0 0x1400002c 0x0400118c
0x02fffc0c 0x1400002d 0x038011ad 0x064819ac  0x1400002d 0x038081ad 0x06488000 0x03400000
0x064809ac 0x43fff59f ...
```

`0x06488000` 是 `idle 0`、`0x43fff59f` 是回跳 `beq`，`r13=0x1020` 正是
`loongson_ipi_iocsr` 的 mailbox 端口 ⇒ **CPU#1 停在 §9.6.1 抄录的那段
`slave_boot_code` 停放循环里**（`EENTRY=0`、`IE=0` 与固件做的事逐条吻合）。

结论：**默认镜像下 CPU#1 从未执行我们的内核代码，也不写客户机 RAM**（ROM 只读 +
读 IOCSR）。所以「崩溃来自次核执行内核代码」这条路被实测排除了；SMP 镜像下它才被
mailbox 拉进 `arkrtos_smp_secondary_entry`（§9.6.6 的 `-d in_asm` 符号序列）。

**复现**

```bash
qemu-system-loongarch64.exe -M virt -m 128M -smp 2 -display none \
  -serial tcp:127.0.0.1:5501,server=on,wait=on \
  -monitor tcp:127.0.0.1:6501,server=on,wait=off \
  -kernel build/qemu_loongarch64/ark_qemu_la64.elf
```

看提示符出来后连 HMP `6501`，敲 `info registers -a` 即可读到上表的 CPU#1 状态。
`-accel tcg,thread=single` 加在 `-M` 之前即可复现最后一行。

**新增一条不许读法**

6. **不能把 `-accel tcg,thread=single` 当推荐配方**：它并不修掉崩溃（SMP 镜像
   1/10 仍崩），并且会让次核**永远上不了线**（`mask=0x1`，10/10）。任何拿它当
   「稳定跑法」的说法都是错答案。

#### 9.6.11 回归与修复：次核没开 `EUEN.FPE`，整机在 IPI 处挂死（2026-11-21）

§9.6.4 / §9.6.8 那两处是**公共代码**的改动（`atomic_cas`、`eentry` 的 FPR 保存），
它们把「次核入口缺一条 CSR 设置」这件事从无害变成了致命。本节记这次回归的取证、
修法与判据。

**现象（确定性复现，不是偶发）**

SMP 镜像（`BUILD=build/la64_smp_only EXTRA_CFLAGS="-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2"`）
在 `-smp 2` 下每次都在第 8 行停住，串口最后一行是：

```
[smp] cores=2 mask=0x00000003 sent=1 ipi1=0 result=FAIL(ipi not received)
```

（`ipi1=0` 与后面的 ` result=FAIL` 贴在一起，容易被读成 `ipi10`。）11-19 的
`build/la64_sms_on/ark_qemu_la64.elf` 用同一命令行仍打到 122 行且 `ipi1=5 result=ok`。
即：**这是 HEAD 上的回归，不是 QEMU 或命令行差异**。

**取证：`-d int` 抓到次核在异常入口里自激**

```bash
timeout 20 qemu-system-loongarch64.exe -M virt -m 128M -smp 2 -nographic \
  -display none -serial file:D:/tmp/x.log -d int -D D:/tmp/int.log \
  -kernel build/la64_smp_only/ark_qemu_la64.elf
```

`int.log` 173 万行全是同一条（摘原文）：

```
loongarch_cpu_do_interrupt enter: pc 000000000024f0f4 ERA 0000000000246584 exception: 15 (Floating Point Disabled)
loongarch_cpu_do_interrupt: PC 000000000024ee00 ERA 000000000024f0f4 cause 15
, ESTAT 00000000000f1000 EXCFG 0000000000001000 BADVA 0000000000000000 BADI 000000002bc40060 SYS_NUM 0 cpu 1 asid 00000000000a0000
```

三条读数把方向定死：`ESTAT` 的 Ecode = **15（FPD）**；**cpu 1**；`PC = 0x24ee00`
正是 `eentry` 的入口地址（`nm` 可核），`ERA = 0x24f0f4` 是 `eentry` 里存 FPR 的
`fst.d`。也就是说：**异常入口自己在故障，永远走不出去**，于是 `ipi_count` 不会涨、
主核自旋超时后整机停摆。

**根因链**

1. `b104c11`（FCSR/FPU 上下文）与 `1477bdb`（LSX 上下文）让 `eentry` **无条件**保存
   FPR（`fst.d`）。
2. `example/qemu_loongarch64/start.S` 早就为这条路设了 `EUEN.FPE`，注释原句是
   「不开这一位，连 eentry 里存 FPR 的那条 `fst.d` 自己都会故障」——但那是**主核**
   的 `_start`。
3. 次核走的是 `kernelsrc/port/loongarch/la64/arkrtos_context.S` 的
   `arkrtos_smp_secondary_entry`，它按硬件复位后的 CSR 状态起跑，**从来不写 `EUEN`
   （CSR 0x2）**，全部设置只有 `EENTRY` / `ECFG` / `CORE_EN` / `CRMD.IE`。
4. 于是次核在第一条 IPI 陷阱上落 Ecode=15 ⇒ 见上。

这条因果链在 `start.S` 的注释里已经写死了，只是次核入口漏了对称的一条。

**修法（3 行汇编 + 注释，包在 `#if (ARKRTOS_USE_FPU == 1)` 里）**

在 `arkrtos_smp_secondary_entry` 装 `EENTRY` **之前**补：

```
#if (ARKRTOS_USE_FPU == 1)
    csrrd   $t1, 0x2
    ori     $t2, $zero, 0x1        /* EUEN.FPE */
    or      $t1, $t1, $t2
    csrwr   $t1, 0x2
#endif
#if (ARKRTOS_USE_LSX == 1)
    csrrd   $t1, 0x2
    ori     $t2, $zero, 0x2        /* EUEN.SXE（只开 SXE、不开 ASXE，与 start.S 同） */
    or      $t1, $t1, $t2
    csrwr   $t1, 0x2
#endif
```

选「次核入口补 EUEN」而不是「eentry 条件保存 FPR」的理由：与主核 `start.S` 完全对称、
改动面最小，且整段在 `#if ARKRTOS_USE_SMP == 1` 的函数里 ⇒ 默认档一行都不编。

**修后读数（QEMU，不是真机）**

| 档位 | 读数 |
|---|---|
| SMP（`-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2`） | `[smp] cores=2 mask=0x00000003 sent=1 ipi1=5 result=ok`，25 s 内 191 行、`[A]/[B]` 到 round=17 仍在跑 |
| SMP + `SMP_SCHED`（`-DARKRTOS_USE_SMP_SCHED=1`） | `[sched] to cpu1 rc=0` → `cur1=0x00000006 q=1 sw=1 owned=1 loops=+2951/+3426/+3024 t=2020/4016/6016` → `[sched] back rc=0` → 之后 `cur1=0xffffffff owned=0` |

**变异自检（证明这条指令就是因，不是「顺手加上就好了」）**

把 `ori $t2, $zero, 0x1`（置 FPE 位那条）换成注释重编，其余不动：

```
[smp] cores=2 mask=0x00000003 sent=1 ipi1=0 result=FAIL(ipi not received)
```

串口 8 行后挂死，与修复前逐字一致；还原后回到 `ipi1=5 result=ok`。

**门禁（`tools/smp_gate.py` pin V 扩展）**

pin V 原本只要求 SMP=1 反汇编里有 `secondary_entry` / `iocsrwr` / `ertn`。现在追加：

* 在 `arkrtos_smp_secondary_entry` 的函数体切片里必须出现 `csrwr $rN,0x2`；
* 变异自检：把该文件的 `csrwr $t1, 0x2` 换成 `nop` 重汇编，上面这条断言必须消失 ——
  否则说明断言不是钉在这条写上的（假绿）。

```
ok: LA64 EUEN pin (CSR 0x2 written in secondary entry, gone when the setter is removed)
```

**默认档零变化（如实记账，含一条限定）**

同一源码树下分别用「新文件」「`git checkout --` 回的 HEAD 文件」各建一次默认档
（SMP=0），两次镜像的 `.text/.data/.bss` 大小逐项相同（`449096 / 448 / 5128740`）；
`.text` 逐字节比对只有 **8 个字节**不同，全部落在 `__DATE__` / `__TIME__` 展开出来的
`Nov 21 2026 18:12:39` vs `18:12:53` 这类构建时间戳串里 ⇒ **代码字节零变化**。
（限定：这条比对只在「两次构建间隔几十秒」下成立；真正的逐字节对拍要走
`SOURCE_DATE_EPOCH`，见 §9.6.6。）

**边界与不许读法**

* 读数全部来自 `qemu-system-loongarch64`：**QEMU 算上板，不是真机**。本仓库没有
  LA64 多核板。
* 本节故障是**确定性**的（每次必挂、每次都停在同一行），与 §9.6.10 登记的
  「~10% 间歇停机」不是同一现象；本节**没有**解释 §9.6.10，那条登记保持原状。
* 不能把本节读成「次核的 CSR 初始化已经做全了」：本次只补了 `EUEN`。次核仍然不
  打开分页（`CRMD.PG` 恒 0、DA 直通）、不装 TLB，`arkrtos_port_send_ipi()` 仍然
  拒绝核 0 —— 这三条是 TLB shootdown 的前置，见 `docs/欠账盘点.md`。

### 9.7 RV32 侧 ②③：ACLINT `msip`（2026-11-19，QEMU RV32 `-smp 2` 实跑）

LA64 走的是 IOCSR mailbox，RISC-V 完全另一套：**每 hart 一个 `msip` 字**，
往它写 1 就是给那个 hart 挂一个机器软件中断。QEMU 9.2 的 `virt` 用 aclint 设备
（`hw/intc/riscv_aclint.c`），偏移与经典 CLINT 一致，实测 `msip[1]` 能拉起次核。

与 LA64 最关键的区别：**RV32 的次核会真的执行我们的 `start.S`**（LA64 走 pflash 固件），
所以启动协议写在汇编入口里：

1. 入口第一件事 `csrr t0, mhartid`，非 0 号 hart 直接分叉；
2. 次核**只用 CSR 轮询** `mip.MSIP` 停靠（`csrr` 循环，一个字节内存都不碰）——
   这样避开「CPU0 还在清 `.bss`，次核却已经在用 `.bss` 里的变量」的竞态；
3. 被 CPU0 写 `msip[cpu]=1` 唤醒后，才清自己的 `msip`、换到自己的静态栈
   （`arkrtos_smp_stacks` 里第 `hart` 份）、装 `mtvec`、开 `MIE`/`MSIE`，
   进 `arkrtos_smp_secondary_main(mhartid)`，永不返回。

内核侧落地点：

- `kernelsrc/port/riscv/rv32imac/arkrtos_port.c`：每 hart 的 `msip` 取址
  （`base + 4*cpu`，`cpu` 就是 `mhartid`）、`rv_clint_msip_write()`、
  `arkrtos_port_cpu_start()` / `arkrtos_port_send_ipi()` 的真实现；
  `arkrtos_port_cpu_id()` 真读 `mhartid`；
- `kernelsrc/port/riscv/rv32imac/arkrtos_context.S`：次核专用 `rv_smp_trap_entry` /
  `rv_smp_trap_c`，**只认机器软件中断**（清了挂起再计数），其余 `mcause` 一律致命打印停机；
- 板级：`ARKRTOS_USE_SMP` 从 `#undef` 改 `#ifndef`（原来的 `#undef` 会吃掉命令行 `-D`），
  补 `ARKRTOS_SMP_SEC_STACK_SIZE` 默认值，banner 按档位报 hart 数；
- `example/qemu_riscv32/Makefile`：`ASFLAGS` 也 `-include` 板级头（否则 `.S` 看不到 SMP 宏）。

**实跑读数**（`qemu-system-riscv32 -machine virt -smp 2`）：

```
[smp] cpus=2 mask=0x00000003 sent=1 ipi1=5 result=ok
```

`-smp 1` 对照：`result=FAIL(secondary offline)`——**这条比成功那条更重要**：
它证明成功读数不是「自检自己报自己」，而是真的有一个次 hart 上线并回了 5 次 IPI 往返。

**默认档零变化**：`port` / `context` / `start` / `main` / `board` 五个对象的可分配段
逐字节相同（`objcopy -O binary` 后 sha256 对比），默认档全量重建 0 warning
（顺带修掉 `guard` / `audit` 在 RV32 上 `uint32` 是 `unsigned long` 引起的 `-Wformat`）。

判词：**`smp_rv32_msip_ok_on_qemu`**。边界：④ 调度分核这一轮**只落在 LA64**
（见 §9.8），RV32 次核上线后仍是 `arkrtos_smp_idle_loop()` 空转；`msip` 的
置位/清位语义来自 QEMU 的 aclint 模型，真实硬件的 ACLINT 是否同样写、是否需要
额外的 fence，未验证。

### 9.8 ④ 调度分核 / 任务迁移：选 AMP，不选对称调度（2026-11-19，QEMU LA64 `-smp 2` 实跑）

#### 9.8.1 为什么是 AMP

把全局调度器拆成「每核一份」要动的是 `arkrtos_task.c` 的主体：当前任务号、
就绪位图、延时链、静态量，每一处都要改成每核数组、每一处都要加锁，任何一处漏了
就是静默的数据竞争。这个改动的爆炸半径远大于一次会话能验证的范围。

所以选 **AMP**：**全局调度器一行不改**，次核拿一份自己的运行队列，任务由调用方
**显式迁过去、显式要回来**。这不是「对称多核」，也不是「负载均衡」——它是一条
能被验证的、边界清楚的路。开关 `ARKRTOS_USE_SMP_SCHED`（默认 0），
两条 `#error` 要求 `ARKRTOS_USE_SMP=1` 且 `ARKRTOS_CPU_NUM>=2`（放在
`arkrtos_config.h`，因为只有那里看得到 `ARKRTOS_CPU_NUM`）。

#### 9.8.2 新模块：`arkrtos_smp_sched.c` / `.h`

每核状态全在一个模块里：当前任务（-1 = idle）、idle 帧指针、时间片余量、本核拍数、
切换计数、迁入/迁出计数、本核运行队列（`s_q[cpu][TASK_MAX]`，按最小 priority 选、
同优先级 FIFO 轮转）、每核自旋锁；CPU0 → 次核的交还请求与次核 → CPU0 的归还标记。

对外只有五件事：

| 接口 | 语义 |
|---|---|
| `arkrtos_smp_sched_enter(cpu)` | 次核进自己的调度域：起本核节拍，然后 WFI 等任务被迁过来（不返回） |
| `arkrtos_smp_sched_task_to(id, cpu)` | 把任务迁到某核。`-2` 目标核 0、`-3` 不是 READY、`-4` 已归别核、`-5` 队列满 |
| `arkrtos_smp_sched_task_back(id)` | 把任务要回 CPU0（请求 + 下一拍回收） |
| `arkrtos_smp_sched_cur(cpu)` / `_selftest(out[8])` | 读数与自检 |
| `_poll()` / `_switch()` / `_tick()` / `_owned_by_other()` | 内核与 port 用的内部钩子 |

四条边界（写死在头文件里，不是「暂未实现」）：

1. **次核不许发 SVC、不许调内核 API**：对象表、文件系统、设备表都没有跨核保护。
   `la_smp_trap_c` 遇到 `SYS` 一律致命打印 + 停机，**绝不静默放行**——放过去就是
   数据竞争，报错停机是唯一诚实的处理；
2. **只接受 READY 任务迁移**：`RUNNING` 迁移要在源核的切换点插入逻辑，`WAIT`/`DELAY`
   迁移会破坏等待语义，两样都直接拒绝（`-3`）；
3. **次核节拍不推进全局 tick**：`tick` 仍只由 CPU0 推进，否则延时链与时基会跑两遍；
4. **迁移是「请求 + 每拍轮询」**：延迟上限 1 拍（500 µs）。不在 IPI 里直接做调度，
   避免 IPI 与定时器两条路径重入。

归属用模块私有的 `s_owner[]` 而**不是** TCB 新字段：TCB 是布局敏感结构，加字段会让
默认关档的尺寸也变，与「关档逐位不变」直接冲突。

#### 9.8.3 落地点（五处）

| 文件 | 改动 |
|---|---|
| `kernelsrc/src/arkrtos_smp_sched.c` / `include/arkrtos_smp_sched.h` | 新模块（未开档时是空翻译单元） |
| `kernelsrc/src/arkrtos_smp.c` | `secondary_main()` 在开档时改调 `arkrtos_smp_sched_enter(cpu)` |
| `kernelsrc/src/arkrtos_task.c` | tick 体里加 `poll()`；`ready_rebuild` / `sched_check`（两处循环）/ 线性优选扫描都按 `owned_by_other()` 跳过次核任务 |
| `kernelsrc/port/loongarch/la64/arkrtos_port.c` | `la_smp_trap_c()`：定时器 → 清 TICLR + 计拍 + 换帧；IPI → 清挂起 + 默认处理；`SYS` 与其它 → 致命 |
| `kernelsrc/port/loongarch/la64/arkrtos_context.S` | `la_smp_trap_entry` 在开档时把帧与 `CSR.TID` 交给 `la_smp_trap_c`，并用它返回的帧继续恢复 |

第三条值得单独说：迁到次核的任务 `status` 仍是 `READY`（语义是「可运行」），
不跳过的话，只要迁出一个任务，`schedcheck` 就**必然**报不一致、`ready_rebuild`
会把它重新挂回 CPU0 就绪链——等于两个核同时动同一份帧。

#### 9.8.4 实跑读数

开启档：`EXTRA_CFLAGS="-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2 -DARKRTOS_USE_SMP_SCHED=1"`，
`qemu-system-loongarch64 -machine virt -smp 2 -kernel ark_qemu_la64.elf -nographic`：

```
[smp] cores=2 mask=0x00000003 sent=1 ipi1=5 result=ok
[sched] to cpu1 rc=0
[sched] r=1 cur1=0x00000004 q=1 sw=1 in=1 out=0 t=2042 owned=1 loops=+4452
[sched] r=2 cur1=0x00000004 q=1 sw=1 in=1 out=0 t=4054 owned=1 loops=+3693
[sched] r=3 cur1=0x00000004 q=1 sw=1 in=1 out=0 t=6058 owned=1 loops=+3184
[sched] back rc=0
[sched] r=4 cur1=0xffffffff q=0 sw=2 in=1 out=1 t=8062 owned=0 loops=+3395
[sched] r=5 cur1=0xffffffff q=0 sw=2 in=1 out=1 t=10066 owned=0 loops=+4760
[sched] r=6 cur1=0xffffffff q=0 sw=2 in=1 out=1 t=12092 owned=0 loops=+4056
```

读法：`cur1=0x4` 是任务 id（`s_id_w`），`q=1`（队列里还剩它自己，因为被切下时会排回队尾）、
`owned=1`（模块私有归属表说是次核的）；`loops` 每 2 s 涨三千多，说明**任务真的在次核上
执行**而不是「挂上了没人跑」；`back rc=0` 之后 `cur1=0xffffffff`、`q=0`、`out=1`
（交还计数 +1）、`owned=0`，次核回到空闲。既有功能全部照旧：`[A]/[B]` 节拍与
`svc_ok`、`[FS] PASS`、`exec rc=0`、`[MOD] ld rc=0`、两处 tamper check、
`[STK] task 4: stack bottom flag overwritten`（栈守卫演示，预期）、shell 正常。

#### 9.8.5 本轮抓到并修掉的一个读数缺陷：空闲→空闲也被计成一次切换

第一版实跑时 `sw` 在任务交还、次核只剩空闲后从 8 跳到 1010 → 2011 → 3011，
看着像「每拍都在疯狂切换」。真因是切换函数的末尾无条件 `s_switch_cnt[cpu]++`：
次核每一拍都从 idle 进 switch、又选回 idle，明明没换任务也记了一次。

修法是在「选下一个」之后加一条早返回：`handed == 0` 且 `next` 与 `cur` 都是 NIL 时，
帧已在前面存进 `s_idle_sp[cpu]`，直接原地返回、不计数。限定 `handed == 0` 是必须的：
交还那一路 `cur` 刚被置成 NIL，但 `frame` 属于被交还的任务，必须走 else 分支把
`s_idle_sp` 取回来。修后读数变成 `sw=1`（切入任务一次）→ `sw=2`（交还后切回空闲一次），
此后不再增长，其余读数一个没变。

#### 9.8.6 默认档零变化（两条独立证据）

1. 默认档 `arkrtos_smp_sched.o` 的 `text/data/bss` **全为 0**（`elf-size` 读数），
   即关档下这个新文件不产生任何可分配内容；
2. 与基线（`58ab9d6`）在独立 worktree 同口径构建，逐对象反汇编对拍（mnemonic +
   操作数 + 重定位名）：`arkrtos_task.o` 11072 行、`arkrtos_port.o` 5323 行、
   `arkrtos_context.o` 338 行、`main.o` 3487 行**只有两条不同**，都是
   `ori $r6,$r0,0x<line>` ——日志里的 `__LINE__` 常量（源文件因本轮插入共前移 34 行）。
   指令流、符号集合、段大小完全一致。整镜像 `objcopy -O binary` 后 31 462 512 B 中
   只有 3 个字节不同，全部落在同一函数里的这两条 `ori` 上。

**不许读成「默认档逐字节不变」**：那 2 条日志里的行号确实变了，如实记账。

#### 9.8.7 判词与四条不许读法

判词：**`smp_amp_sched_ok_on_qemu`** —— LA64 / QEMU `-smp 2` 上，任务能迁到次核、
在次核上真跑（`loops` 增长）、能被要回 CPU0，既有功能全部照旧；RV32 侧与真实硬件
**未验证**。

1. **不能读成「支持对称多核调度」**：全局调度器仍是单核一份，没有自动负载均衡，
   任务必须由调用方显式迁移；次核上的任务不能调内核 API。
2. **不能把 `loops=+4452` 读成「两个核在并行算」**：同一个任务在某一刻只在一个核上跑，
   迁移是「搬走」不是「复制」；读数里 `owned=1` 与 CPU0 侧 `owned_by_other()` 跳过
   是同一件事的两面。
3. **不能把 `sw=1/2` 读成「只切换了两次」的调度器性能读数**：这是次核的切换计数，
   起点是次核刚进调度域，不是整机切换次数。
4. **不能读成真板可用**：`la_smp_trap_c` 里定时器/IPI 的解析依赖 QEMU 的 `ESTAT`
   编码与 `TICLR` 语义，IOCSR mailbox 的字节掩码同样来自 QEMU 9.2.3 源码；真实
   固件与硬件未验证。

### 9.9 ⑤ AMP 自动负载均衡（2026-11-23，QEMU LA64 `-smp 2` 实跑）

#### 9.9.1 位置：在 ④ 之上加一层策略，不动调度器

④ 把「迁谁、什么时候迁」留给人。⑤ 只把**决策**自动化：次核空了就迁一个过去，
次核排了两个而 CPU0 一个就绪任务都没有就要一个回来。**全局调度器仍然一行不改**，
次核上的任务仍然不许调内核 API —— 省掉的不是锁，而是「同时有两条路径改同一份
状态」这件事本身。所以这不叫对称多核，叫 AMP 上的一个负载策略。

#### 9.9.2 开关与依赖

`ARKRTOS_USE_SMP_BALANCE`（默认 0），两条 `#error`（放在 `arkrtos_config.h`）：
必须 `ARKRTOS_USE_SMP_SCHED==1`，且 `ARKRTOS_CPU_NUM>=2`。Kconfig 里是
`config ARKRTOS_USE_SMP_BALANCE`（`depends on ARKRTOS_USE_SMP_SCHED`，默认 n）。

#### 9.9.3 两条规则与门槛为什么这样定

| 规则 | 触发条件 | 动作 |
|---|---|---|
| ① 迁出 | 该次核 `s_qn==0 && s_cur==NIL` **且** CPU0 就绪任务 ≥2 | 把候选表里第一个 READY 任务迁到该核 |
| ② 要回 | CPU0 就绪任务 ==0 **且** 该次核队列 ≥2 | 从该核队尾要回一个（`task_back`） |

`≥2` 与 `==0 / ≥2` 这两组门槛是防 ping-pong 的：迁出要求 CPU0 手上还有富余，要回
要求 CPU0 真空且次核真的排起来了 —— 不去定义「平均」，也就不会在两核各剩一个的
时候来回搬。搬运本身仍是 ④ 的「请求 + 每拍轮询」，延迟上限 1 拍。

**不迁「正在本核跑」的任务**：`task_to` 对 idx==当前任务返回 `-6`，策略的候选筛选
也把当前任务剔掉。原因同 ④ 第 2 条边界 —— RUNNING 迁移要往源核的切换点插逻辑，
这里不做，就明确拒绝而不是「试着搬搬看」。

#### 9.9.4 接口

| 接口 | 语义 |
|---|---|
| `arkrtos_smp_sched_auto_add(id)` | 登记进候选表（上限 `ARKRTOS_SMP_BALANCE_MAX`=4）。`-1` 表满 / `-3` 不是合法任务 |
| `arkrtos_smp_sched_auto_del(id)` | 摘出候选表（收尾必做，否则策略还在动它） |
| `arkrtos_smp_sched_auto_count()` | 候选表里的有效条数 |
| `arkrtos_smp_sched_balance_stat(out[4])` | `out[0..3]` = 自动迁出 / 自动要回 / 次核空闲但无可迁 / 策略节拍 |

候选表**必须由调用方显式登记** —— 不提供「自己猜该迁谁」的能力：猜错了就是把一个
任务搬到它不该在的核上，而且没有任何读数能证明它猜错了。

#### 9.9.5 实跑读数

`EXTRA_CFLAGS` 加 `-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2 -DARKRTOS_USE_SMP_SCHED=1 -DARKRTOS_USE_SMP_BALANCE=1`，
`qemu-system-loongarch64 -machine virt -smp 2 -kernel ark_qemu_la64.elf -nographic`，跑 25 s：

```
[bal] id1=7 id2=8
[bal] add b1 rc=0
[bal] add b2 rc=0 n=2
[bal] r=1 out=1 back=0 idle=0 tick=1003 cur1=6 q1=1 b1=+2925 c1=1 b2=+2919 c2=0
[bal] r=2 out=1 back=0 idle=0 tick=2004 cur1=6 q1=1 b1=+2894 c1=1 b2=+2841 c2=0
[bal] r=3 out=1 back=0 idle=0 tick=3004 cur1=6 q1=1 b1=+2975 c1=1 b2=+2966 c2=0
[bal] r=4 out=1 back=0 idle=0 tick=4004 cur1=6 q1=1 b1=+3029 c1=1 b2=+3022 c2=0
[bal] r=5 out=1 back=0 idle=0 tick=5004 cur1=6 q1=1 b1=+2956 c1=1 b2=+2952 c2=0
[bal] r=6 out=1 back=0 idle=0 tick=6004 cur1=6 q1=1 b1=+2923 c1=1 b2=+2917 c2=0
[bal] park b1=7 b2=8 done
```

四条判据逐条对上：

1. `out=1` 且 `tick` 从 1003 涨到 6004 —— **迁出是策略自己做的**；`back=0` 是因为
   CPU0 从未真空中断过，不是要回逻辑失效（这一点不要读反）；
2. `cur1=6`（= id1−1）与 `c1=1` 说明次核上跑的是 b1；`c2=0` 说明 b2 一直在 CPU0；
3. 两个计数**同时**涨约 2900–3030 / 200 拍 —— 两核在并行推进，且同优先级让 CPU0
   侧那一个还能分到时间片（异优先级会被忙等的同核任务饿死，本轮踩过，见 9.9.6）；
4. 收尾 `[bal] park b1=7 b2=8 done` 之后 worker 停在 `wfi`，镜像里原有的 `[A]/[B]`
   周期任务继续正常打印（`svc_ok`）。

#### 9.9.6 与 ④ 手工演示的互斥（本轮踩到的真问题）

第一版把两套演示同时编进去，读数是 `out=0`（从不迁移）、`cur1=6` 一直是手工演示的
`task_w`（id 7）—— **两套演示抢同一个次核**：手工那套先把次核占了，均衡策略看到
「次核不空」就永远不动。这不是策略错，是两个演示在争一个资源。

所以均衡档下手工 SMP 演示按 `(ARKRTOS_USE_SMP_BALANCE != 1)` 关掉三处（块头、
注册段、`task_to` 段，以及只为它服务的 `s_stack_chk` 声明）。手工那套的读数改在
仅 `SMP_SCHED=1` 的镜像里复跑，形态与 §9.8.4 一致（`to cpu1 rc=0` → 次核推进 →
`back rc=0` → `cur1=0xffffffff`）—— **关掉的是演示，不是能力**。

worker 收尾不能退出：任务退出要走内核调度器，而次核上不许走那条路径。所以 `park`
的做法是「先从候选表摘掉 → 降到优先级 200（与 idle 同级）→ 非他人所有时挂回就绪链」，
worker 本体在 `s_bal_stop` 置位后原地 `wfi`，永不返回。

#### 9.9.7 顺手收掉的一个既有缺陷：任务函数返回 = 跳 0

演示第一版跑出 `ecode=0x0d (INE) era=0`：`task_bal_chk` 打印完六轮就返回，而入口帧
的 `ra` 是 0，`ret` 直接跳到地址 0。**这不是均衡引入的** —— 仅 `SMP_SCHED=1` 的参考
镜像同样崩。

修法：新增 `arkrtos_task_entry_return()`（`arkrtos_task.c`），log 一行
`task <id> entry returned -> auto exit`，然后 `arkrtos_task_kill_internal()` 收尾，
再 `wfi` 死循环；LA64 与 RV32 的 `arkrtos_port_stack_init` 都把入口帧的 `RA` 指向它。
判据是实跑日志里出现 `task 9 entry returned -> auto exit` 且不再有 `FATAL`。
**LA64 已实跑；RV32 只有编译，未实跑。**

#### 9.9.8 默认档零变化

`ARKRTOS_USE_SMP_BALANCE` 默认 0：新增状态（候选表、四个计数器）全在
`arkrtos_smp_sched.c` 的 `#if` 内，演示全在 `main.c` 的 `#if` 内，默认档
`arkrtos_smp_sched.o` 仍是空翻译单元。`tools/smp_gate.py` 的 pin Y 用 `objdump -t`
逐符号钉住「BALANCE=1 的目标文件有四个契约符号 / BALANCE=0 的一个都没有」，并真编
两条坏档证明 `#error` 会触发、好档能过。`build/la64_def_chk` 默认档 20 s 实跑：
0 `FATAL`、无任何 `[bal]`/`[sched]` 输出。

#### 9.9.9 判词与三条不许读法

判词：**`smp_amp_balance_ok_on_qemu`** —— LA64 / QEMU `-smp 2` 上，策略能在次核空闲
时自己迁出一个任务、两核并行推进各自的计数、收尾能停下来把 CPU0 交还；默认档与
仅 ④ 档的行为都未变。RV32 侧与真实硬件**未验证**。

1. **不能读成「对称多核 / 通用负载均衡」**：没有全局时钟同步、次核不许进内核、
   没有 TLB shootdown；候选表靠人登记，策略只在「次核空」与「CPU0 真空」两个极端
   点动作，中间状态一概不动。跑两个任务是自检，不是能力上限声明。
2. **不能把 `b1=+2925 / b2=+2919` 读成「均衡带来的加速」**：这是「两个任务各在一个
   核上跑」的读数，没有同口径的单核对照，不构成加速比证据。
3. **不能读成真板可用**：次核启动、IPI、IOCSR mailbox 全部走 QEMU 9.2.3 的模型，
   真实固件与硬件未验证。

### 9.10 ⑥ 主核 IPI 接收路径（R1，2026-11-23，QEMU LA64 `-smp 2` 实跑）

#### 9.10.1 缺口在哪：不是「少写一个处理函数」，是分派表里没有这个向量

在此之前 `arkrtos_port_send_ipi(0)` 直接返回 -1，理由写得很清楚：**主核没有 IPI 处理路径**。
更具体地说，`la_trap_c()` 的 INT 分支只认 `ESTAT.IS` 的定时器位，其余中断一律落到
`arkrtos_port_fatal_report()` 停机。所以「给主核发一个 IPI」不是能不能收到的问题，而是
收进来就会把自己打成致命停机。

这条缺口是 [SMP对称调度与TLB-shootdown-设计裁定](SMP对称调度与TLB-shootdown-设计裁定.md)
§5.3 的 R1，也是 §5.4 写死的「最小第一步」：它不改分页、不改调度，是唯一能单独交付的一项。

#### 9.10.2 四处改动，缺一条这条路就不存在

| # | 改动 | 为什么必须 |
|---|---|---|
| 1 | 清挂起收敛成 `la_ipi_clear_pending()`（读 `STATUS 0x1000` → 写 `CLEAR 0x100C`） | 这段序列原来只有「调度分核」档的次核路径有；不清就 `ertn` 回去立刻再进一次 = 中断风暴 |
| 2 | `la_trap_c()` 在定时器分支之后加 `else if((estat & LA_ESTAT_IS_IPI) != 0u)`，**先清挂起再**调 `arkrtos_smp_ipi_handler()`，**不碰 `s_switch_pending`** | 顺序反了就是上面那个风暴；碰 `s_switch_pending` 会把「收到 IPI」误读成「该切任务」 |
| 3 | `arkrtos_port_irq_init()` 在 `#if SMP` 下写 `CORE_EN = 0xfff`（IOCSR `0x1004`）并置 `ECFG.LIE[12]` | QEMU 的 loongson-ipi **只在本核 `CORE_EN` 命中该向量时**才拉中断，光置挂起位不会进来；`ECFG.LIE` 不开同样收不到 |
| 4 | `arkrtos_port_send_ipi()` 不再拒绝核 0（越界仍 -1） | 原来的拒绝理由（主核没有处理路径）已经不存在 |

向量号沿用本仓既有约定 1（`IPI_SEND` 写 `(目标核 << 16) | 向量号`）。**同一个向量连发会被
QEMU 合并成一次**，所以调用侧是「一发一等」，不可能靠压计数当带宽测。

**回执机制**（不引入新开关，全在 `ARKRTOS_USE_SMP` 之下）：`arkrtos_smp_cpu_t` 增
`ipi_back` / `ipi_back_req` 两个字段，`arkrtos_smp_ping_main(cpu)` 先登记 req 再发 IPI，
默认 `arkrtos_smp_ipi_handler()` 在次核上看到 req 就回一个给 CPU0、`ipi_back++`、清 req。
登记与撤除的规则：目标核不在线或发不出去就把 req 撤掉（不留悬空请求）。**一次性回执，
不做心跳** —— 心跳会让 `ipi_count` 变成时间的函数，读数就不再是「这条路径通不通」的证据。

**RV32 这一档明确不动**：那里 CPU0 的 `msip` 是 PendSV 的挂起位，把 IPI 和处理函数混进去
会搅乱语义（`arkrtos_port_send_ipi(0)` 在 RV32 上仍然拒绝，注释里写明原因）。

#### 9.10.3 默认档零变化（实测，不是推断）

同一台机、同一工具链、默认档（不带 `EXTRA_CFLAGS`）编两次：一份是 `d7ab1c1` 的干净检出
（`git worktree`），一份带本轮改动。

| | text | data | bss |
|---|---|---|---|
| `d7ab1c1` 默认档 | 449888 | 456 | 5128740 |
| 带 R1 的默认档 | 449888 | 456 | 5128740 |

逐节对比：`.text` / `.data` / `.bss` 等 12 个装载节全等，只有 `.debug_info` / `.debug_line` /
`.debug_str` 三个调试节差 1–12 字节（两边的构建目录名长度不同，路径字符串进了 `-g` 信息）。
即：**代码与数据零变化**；ELF 文件大小差 24 字节全部在调试节里。

#### 9.10.4 实跑读数

仅 SMP 档（`-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2`），`-smp 2` 跑 25 s：

```
[smp] cores=2 mask=0x00000003 sent=1 ipi1=5 result=ok
[smp] ping ipi0=2 self=1 back1=1 result=ok
```

- `ipi0=2`：一次是 CPU0 自投（`self=1` 表示自投那一发确实回来了），一次是次核按登记回 ping；
- `back1=1`：次核的 handler 确实回了，且只回一次（同一镜像里第二次 IPI 不再回，见宿主用例）；
- 上一行的 `ipi1=5` 是既有的次核侧读数，未变。

SMP_SCHED + BALANCE 档（`+ -DARKRTOS_USE_SMP_SCHED=1 -DARKRTOS_USE_SMP_BALANCE=1`）同一镜像里
两条路径互不打架：

```
[smp] ping ipi0=2 self=1 back1=1 result=ok
[bal] r=1 out=2 back=1 idle=0 tick=1004 cur1=7 q1=1 b1=+2665 c1=0 b2=+2688 c2=1
...
[bal] park b1=7 b2=8 done
```

**命令行的坑（本轮踩到）**：`-nographic` 与 `-display none` 同时给会把 monitor 挂在 stdio，
QEMU 进程不会自己退出（`rc=124`），但 `-serial file:<log>` 照样正常写入；要么加
`-monitor none`，要么外面套 `timeout`。本节读数用的是后者。

#### 9.10.5 门禁

- `tools/smp_gate.py` 新增 **pin R1**：四条标记逐条检查、**每条都带变异自检**（把它从
  `port.c` 抠掉后标记检查必须报缺失）。第一条标记写的是
  `else if((estat & LA_ESTAT_IS_IPI) != 0u)` 而不是 `LA_ESTAT_IS_IPI` —— 第一版写宽了，
  自检当场抓住「抠掉分派分支后，`#define` 那一处还让检查通过」，于是把标记收窄到分支本体。
  另加：真编 SMP=1 / SMP=0 两个目标文件，`nm` 断言 `la_ipi_clear_pending` 只存在于 ON 档；
  夹具（`example/qemu_loongarch64/main.c`）必须真的调用 `arkrtos_smp_ping_self_test()`。
- `kernelsrc/tests/test_smp_host.c` 第 10 段约 20 条断言：`ping_main` 拒 0 / 拒越界 / 拒离线核、
  拒绝时不碰 port 层也不留悬空 req、在线核登记 1 条且只发 1 发到目标核、次核 handler 回一次
  给 CPU0 且消费掉 req、无 req 的第二次 IPI 不回、CPU0 上即使有 req 也不自投。
  跑出来的读数是 `SMP-HOST OK (66 checks)`。
- 覆盖率：`arkrtos_smp` 由 89.19% 升到 **90.11%**（基线未动）。

#### 9.10.6 判词与三条不许读法

判词：**`r1_main_core_ipi_ok_on_qemu`** —— LA64 / QEMU `-smp 2` 上，主核能收到 IPI 并如实认账
（自投与次核回执在同一镜像里都有读数），默认档的代码与数据节逐项不变，RV32 未改动。
**真实硬件未验证。**

1. **不能读成「TLB shootdown 已经具备」**：R1 只是 §5.3 的第一条。R2（次核开分页）、
   R3（跨核共享 VA 调用者）、R4（失效回执）、R5（真机）一件都没做，shootdown 仍然不实现。
2. **不能读成「对称多核 / 次核可以进内核」**：次核上遇到 SVC 依然一律致命停机，全局调度器
   依然只有一份。
3. **不能把 `ipi0=2` 读成「IPI 不丢」或「带宽」**：同向量连发在 QEMU 模型上会被合并成一次，
   代码是一发一等；这个计数只证明「这一发到了、并且回了」，不证明任何吞吐。

### 9.11 ⑦ 次核开分页（R2，2026-11-23，QEMU LA64 `-smp 2` 实跑）

#### 9.11.1 缺口在哪：次核跑的是直译地址，不是「次核不会用 TLB」

次核起来之后一直停在 `CRMD.DA=1` 的直译状态（`arkrtos_smp_secondary_main()` 只做 ping/回执与
AMP 运行队列），它看到的地址与主核的映射视图无关；一旦次核要碰用户窗口 VA、或者在自己核上执行到
一条会触发重填的取指，「次核那套 TLB 是空的」就立刻变成故障。

所以 R2 不是「在次核上调一次 `arkrtos_port_paging_enable()`」：那条路会从帧池给 space 0 再拿 2 MB、
把 256 条页级窗口条目铺一遍 —— 那是**引导核**的 App 地址空间，次核照抄等于把两台机器装成同一套房。
（跨核共享 VA 是 R3，不是这一步。）

#### 9.11.2 做法：重放「引导核铺静态条目」那一小段快照

| # | 改动 | 为什么必须 |
|---|---|---|
| 1 | `la_tlb_fill()` / `la_tlb_write()` 末尾各记一次快照（`la_pg_snap_add()`，**恰好 2 处**） | 静态条目只经这两个写入口；漏一处就有条目不上快照 |
| 2 | 门控 `s_pg_snap_on` 只在 `la_mmu_fill_static()` 的静态条目段打开，`la_pg_boot_map()` 那 256 条页级窗口**前后各关/开一次** | 不关就立刻冲爆上限（8 条），而且那 256 条是引导核的窗口，不该进快照 |
| 3 | 容量 8 条、溢出**置 `s_pg_snap_overflow` 而不是静默丢** | 静默丢 = 次核少一条映射却报成功，等于给假绿 |
| 4 | `arkrtos_port_paging_enable_secondary()`：写 `ASID=0` / `STLBPS=12` / `invtlb 0` → 按快照逐条 `la_tlb_write()` 重放 → 写 `TLBRENTRY` → DA→PG | 这四个 CSR 与 `CRMD` 都是**每核**状态，主核写过不代表次核有 |
| 5 | 打开后立即读回自证：`arkrtos_port_paging_lookup((unsigned long)&s_pg_snap_n, …)` + `la_pg_identity_ok()`，不符返回 -3 | 「开了分页」与「开了之后还查得到地址」是两件事，只有后者能证明能继续跑 |

快照内容就是 `la_mmu_fill_static()` 铺的那 5 条：内核 `0x00000000` PS=22 @MTLB+0、
内核 `0x00800000` PS=22 @MTLB+1、控制台 UART `0x1fe00000` PS=12 @STLB way7、
App UART 别名 PS=12 @STLB way6、帧池 `ARKRTOS_QEMU_FRAME_BASE` PS=22 @MTLB+11。
运行期铺的条目（动态 alias、换空间窗口）**不进快照**。

为了让快照只包住静态段，`la_mmu_init()` 拆成三段：`la_mmu_fill_static()`（原 body）、
`la_mmu_enable_paging()`（关 DA 开 PG + `ibar 0`）、`la_mmu_init()`（置快照状态 → 铺静态 →
关快照 → 写 `TLBRENTRY=&tlb_refill_entry` → 开分页）。行为与拆分前一致。

#### 9.11.3 每核一份 8 KB 重填栈

`la_refill_c` 的入口在 `.S` 里用一份**静态**栈保存 30 个 GPR。两核同时重填时共用一份 256 字节帧会
互相覆盖现场（返回时跳到别人算出来的地址）。所以次核有自己的入口 `tlb_refill_entry_secondary`
与自己的 8 KB 栈（`la_refill_stack_sec_bottom/top`），`TLBRENTRY` 每核各指各的。

DEMAND 档的回退分支原来写死 `la.abs $sp, la_refill_stack_top`，改成 `addi.d $sp, $r21, 0`（`$r21`
是本核帧基址）；不改的话，次核上走回退分支会把帧建到引导核的栈上。

#### 9.11.4 两处判据错误：都是「读数错」，不是「功能错」

第一刀（`on=0 rc=-3`）：读回比较写成 `va & ~0xFFF`。`arkrtos_port_paging_lookup()` 读回的 **PPN 是
页基址**，PS=22 时是 4 MB 对齐值，与 `va & ~0xFFF` 永远不等 ⇒ 假失败。

第二刀（`rc=-3` 仍在；把 `hit/ppn/ps` 打出来看到 `hit=1 ppn=0x00000000 ps=22` 后才定位）：lookup 只读
**TLBELO0（偶页）**，一条条目管一对页。`&arkrtos_smp_cpu[cpu]` 在内核 `.bss`（4.9 MB，跨过 4 MB 边界）
里，正好落在**奇页**，正确期望值要加一个页大小：

```
(ppn + (va & (1UL << ps))) == (va & ~((1UL << ps) - 1))
```

抽成 `la_pg_identity_ok()`，`port.c` 的自证与 `smp.c` 的独立读回用同一口径。这条也说明为什么
调用方（`smp.c`）要在 port 自证之外**再用内核 `.bss` 的地址独立查一次**并把 `hit/ppn/ps`
无条件落进状态块：`rc=-3` 与「没打开」必须可分辨，而分辨手段只能是读数。

#### 9.11.5 失败不停机

`arkrtos_port_paging_enable_secondary()` 的返回码：`0` 成功、`-1` 没有快照、`-2` 快照溢出、
`-3` 打开后读回不符。失败时次核仍在 DA=1 下空转，**不停机** —— 停机会把「次核没开分页」
伪装成「次核没起来」，那是两种完全不同的故障；失败由主核自检读 `pg_rc` 报 `result=FAIL(secondary paging)`。

#### 9.11.6 默认档零变化（实测，不是推断）

没有再引入新开关：整段包在既有的 `ARKRTOS_USE_SMP`（默认 0）之下，也不需要 `ARKRTOS_CPU_NUM>1`
以外的配置。默认档与 R1 基线（`4cbf8d4`）同口径编两次：

| | text | data | bss |
|---|---|---|---|
| `4cbf8d4` 默认档 | 449888 | 456 | 5128740 |
| 带 R2 的默认档 | 449888 | 456 | 5128740 |

`nm` 里 `pg_snap` / `enable_secondary` / `refill_entry_secondary` / `refill_stack_sec` **零符号**。

#### 9.11.7 实跑读数

SMP 档（`-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2`），`-smp 2`：

```
[smp] cores=2 mask=0x00000003 sent=1 ipi1=5 result=ok
[smp] ping ipi0=2 self=1 back1=1 result=ok
[smp] pg cpu1 on=1 rc=0 look=1
[smp] pg result=ok
```

`on=1` 是 port 层自证通过后才写的；`look=1` 是次核用 `&arkrtos_smp_cpu[1]` 这个内核 `.bss` 地址
自己查回一条恒等映射且与 `va` 对得上；`rc=0`，不是「没报」（未报是 `0xFFFFFFFF`）。

#### 9.11.8 门禁

`tools/smp_gate.py` 新增 **pin R2**：

- 5 条标记（`la_pg_snap_add` 定义、溢出置标志、`la_pg_boot_map()` 前后关/开、`TLBRENTRY` 换到次核入口、
  `la_pg_identity_ok` 定义）逐条检查，**每条都带变异自检**（抠掉后必须报缺失）；
- 快照记录点必须**恰好 2 处**（抠掉一处变 1 就报错）；
- 真编 SMP=1 / SMP=0 两个目标文件，`nm` 断言 `arkrtos_port_paging_enable_secondary` 只在 ON 档；
- `context.S` 两档汇编：ON 档必须有 `<tlb_refill_entry_secondary>:` 且引用 `la_refill_stack_sec_top`；
  把该符号的**三处**（`$r21` 的加载、`$sp` 的加载、`.globl`/标签定义）一起换掉后，符号必须从对象里消失；
  OFF 档两个符号都不许出现；
- 夹具（`example/qemu_loongarch64/main.c`）必须真的调用次核分页自检并打印 `[smp] pg cpu`。

**这里第一版自检是一次假失败**：变异只换了 `la.abs $r21, la_refill_stack_sec_top` 一处，而次核入口的
`la.abs $sp` 与 `.bss` 里的标签定义仍引用该符号 ⇒ 符号还在对象里，自检当场报「pin 没指在那个符号上」。
自检自己抓出了自己的不严谨，记在这里备查。

#### 9.11.9 判词与三条不许读法

判词：**`r2_secondary_paging_ok_on_qemu`** —— LA64 / QEMU `-smp 2` 上，次核自己写每核 CSR 开起了分页、
重放了引导核的静态映射快照、并用内核 `.bss` 里的地址自己查回一条恒等 TLB 条目；默认档 text/data/bss
逐项不变、零新符号。**真实硬件未验证。**

1. **不能读成「跨核共享 VA 已具备」**：两核现在共享的是同一份**静态**快照，不是同一套 App 窗口。
   R3 要求「至少一处非演示调用：用户态任务经 `task_to` 到次核，且两核都会访问同一窗口 VA」—— 一行都没做。
2. **不能读成「TLB shootdown 已具备」**：R4 的回执判定不存在、失效协议不存在。R2 只是让次核
   「有自己的条目」，而 shootdown 的前提是「有别人的条目需要被失效」。
3. **不能把 `look=1` 读成「映射一致」**：它只说明这一条恒等映射在本核查得到。快照是引导核那一刻的内容，
   运行期主核新铺的条目次核看不到 —— 那正是 R3/R4 要解决的问题。

### 9.12 ⑧ 跨核共享 VA（R3，2026-11-23，QEMU LA64 `-smp 2` 实跑）

#### 9.12.1 缺口在哪：窗口是「每核漂移状态」，而 R2 只共享一张静态快照

R2 之后次核有自己的 TLB、能跑内核代码，但它看到的用户窗口只有引导核在 `la_mmu_init()` 那一刻铺的
那几张静态条目。`0x02000000` 起 2 MB 的 App 窗口（页级窗口档）由 256 条页级条目组成，条目的物理
落点**由「此刻装着哪个空间」决定** —— 这是每核各一份、随任务切换不停漂移的状态。次核从没装过它，于是：

- 一个用户任务的栈/帧落在窗口 VA 上时，次核拿着空窗口去恢复，第一条 `ld.d` 就 refill 失败；
- 就算碰巧装上过一次，两个核各自漂到不同空间时，「同一段 VA」在两核上是两块不同的物理内存 ——
  那叫各说各话，不叫共享。

R3 要证明的只有一句：**同一个窗口 VA 在两核上翻译到同一块物理帧**。为此需要三件事：次核能在切到
用户任务前自己装窗口；帧的特权级要按帧自己的返回地址定；任务注册那一写必须落在它自己空间的物理页上。

#### 9.12.2 做法：port 四件 + sched 每核 prepare 栈 + paging 绑定重造帧

| # | 改动 | 为什么必须 |
|---|---|---|
| 1 | `arkrtos_port_paging_install_space_window(space)`：先 `la_pg_drop_window()` 再 `la_pg_install_space(space)`，ASID 档下写 `CSR.ASID` 且**不还原**，末尾首末两对条目按索引读回自证 | 上一个任务可能是别的空间，同一段 VA 上的残留条目会直接翻译到别人的帧；装完就 `ertn`，还原 ASID 等于让用户代码当场 refill |
| 2 | 该函数**不碰** `s_pg_app_space` | 那是引导核切换路径的记账；次核有自己的视图，把它改成次核眼下的空间会让引导核下一次切换判断错 |
| 3 | `arkrtos_port_paging_va_in_window(va,len)`：整段都在 `[LA_WIN_VA, +2 MB)` 内才返回 1 | 「要不要装窗口」的判据是**地址**不是特权级：栈在窗口里的特权任务，每次压栈都要经窗口翻译 |
| 4 | `la_frame_priv_auto(f, want_user)`：`want_user` 与「帧的 ERA 是否落在 App 窗口 VA」取交集，写 `PRMD.PPLV` | 用户任务卡在系统调用里被换下时，帧是从内核态存的，硬写 PLV3 ⇒ 内核身份在 PLV3 上取指 ⇒ PPI 死循环 |
| 5 | `arkrtos_smp_sched.c`：每核 `s_pend_space/s_pend_priv` + `sms_plan_prepare()`（切出时判决）+ `arkrtos_smp_sched_prepare()`（恢复前落地，三步顺序：装窗口 → 定级 → 清请求）+ `arkrtos_smp_trap_stacks[CPU_NUM][4096] aligned(16)` | 判决只能在**切出**时做（那时 `next` 已知），落地只能在**恢复前**做（那时才该换窗口）；帧本身可能就在新空间的页里，顺序不能换 |
| 6 | `arkrtos_context.S` 次核入口：切到本核静态栈之后再 `bl arkrtos_smp_sched_prepare` | 此刻 `sp` 还指向被打断任务的栈（可能就在旧窗口里），就地写栈会改到别人的帧 |
| 7 | `arkrtos_paging_bind_task()`：绑定成功后调 `arkrtos_port_paging_relocate_frame()` 在目标空间里重造初始帧，整段包在 `((SMP==1)&&(PRIV==1))` 里 | 注册时 `stack_init` 按 `stack_top` 直接写；若栈在窗口 VA 上而窗口装着别的空间，这一写进了别人的物理页，恢复出来 ERA/PRMD 是池里的零 |
| 8 | `relocate_frame` 按**帧池物理地址**直写（`pa = 空间帧基址 + (va - LA_WIN_VA)`），不装窗口、不切 ASID、不动 `s_pg_app_space` | 帧池有 PS=22 恒等映射，一次按字写就够；走「装窗口 → 按 VA 写」要多做 768 次 TLB 操作，只为写 264 字节 |

`sms_plan_prepare()` 的判决（按顺序，命中即返回）：

1. `next < 0`（idle）：`pending_priv=1`，不动窗口 —— idle 跑恒等映射；
2. `next` 是特权任务：`pending_priv=1`；**栈不在窗口里**就直接返回（内核任务照旧不动窗口），
   **栈在窗口里**（R3 的新情形）按它绑定的空间装；
3. `next` 是用户任务：`pending_priv=0`，按绑定空间装。

窗口已经是该空间时跳过（`sp != s_win_space[cpu]` 才记请求）—— 这一步是每核的，不需要跨核锁：
本核的切换路径只写本核字段。落地顺序（`arkrtos_smp_sched_prepare`）也是三步不换：先装窗口
（帧可能就在新空间的页里，不先装就对不上），再按请求补 `PRMD.PPLV`，最后清请求（一次性消费，
绝不残留给下一次恢复）。

#### 9.12.3 失败码：装没装、装上没对上，必须分开报

`arkrtos_port_paging_install_space_window()`：`0` 成功、`-1` 空间号越界、`-2` 本档没有页级窗口
（静态 PS=20 窗口是恒等映射，次核快照里已带着）、`-3` 帧池拒绝（该空间还没拿到那 2 MB）、
`-4` 按需档不支持（次核缺页路径未验证，不假装能跑）、`-5` 铺完首末两对条目读回对不上。

失败时 `arkrtos_smp_sched_prepare()` 把 `s_win_space[cpu]` **复位**成 `0xFFFFFFFF`（不是留着旧值），
`[shva] win inst/rc/space` 三槽把「装了几次 / 最近一次返回码 / 当前装着哪个空间」原样打出来。
下一个用户任务在次核上必然取指失败，读数会如实报 FAIL —— 这里不假装成功。

`arkrtos_smp_sched_win_stat(out)` 返回 `[0]=installs`、`[1]=rc`、`[2]=space`，且**第一次读数前先
`sms_lazy_init()`**：不初始化的话 `s_win_space` 还是 `.bss` 的 0，会被读成「窗口里装着空间 0、
装过 0 次」—— 一个看起来权威的错答案。

#### 9.12.4 实测教训（九条，都是踩出来的）

1. **窗口是每核漂移状态，内核访问必须关中断成对完成。** 观察者在 `wait_internal` 里一阻塞，别的
   用户任务就可能把窗口换成自己的空间；醒来再按窗口 VA 去读，轻则读到别人的帧、重则 PLV0 直接
   refill 致命（实测踩到：`badv=0x02000808`，正是信箱的 ACK 槽）。所以窗口 VA 的每一次访问都写成
   「`ARKRTOS_ENTER_CRITICAL` → 装 space 0 → 读写 → `ARKRTOS_EXIT_CRITICAL`」。
2. **`.appprobe` 里的代码在页级窗口下取不到。** 窗口 VA 的物理落点由当前空间决定，space 0 的窗口
   指向帧池里自己那 2 MB，不是 QEMU 把 `.appprobe` 装进去的那块。所以演示任务的代码放内核恒等映射里
   （PLV0），只把**栈与信箱**放窗口 VA —— 要量的是「栈与数据经窗口共享」，不是 PLV3 保护。
3. **初始帧要按帧池物理地址直写。** 注意页内偏移别丢：写下去的 264 字节正好是 `stack_init` 那四个槽
   （`ra/sp/era/prmd`），其余全零。
4. **同一个 2 MB 窗口不能同时住手工任务与 ELF 装载器。** 装载器的程序段在 `0x02000000` 起 6392 字节、
   用户栈在窗口顶端向下 32 KB，演示任务因此保留 `0x02100000` 起 1 MB 处的一段：
   `[0x02100000,0x02100800)` 任务栈 2 KB、`[0x02100800,0x02100810)` 信箱 4 字，并写进注释说明为什么。
5. **QEMU 9.x 的 `tlbsrch` 与 MTLB `tlbrd` 读回都不可用。** MTLB 条目按 VA 查找一律报 miss、
   `tlbrd` 读成 invalid；同一条窗口条目用**按绝对索引**的 `tlbrd` 读得出来且逐位正确。拿 `tlbsrch`
   当映射自证会得到稳定的假阴性（`activate` 曾因此恒 `-2`、`install_space_window` 恒 `-5`）。
   改成「按索引读回刚写下去的那一条」，验的正是 install 做的事；真机上两种读法都成立，所以这不是降级。
   顺带把 `activate` 的「已经在该空间就直接返回」也改成带首末两对条目自证的快速路径。
6. **`PRMD.PPLV` 不能按任务属性写。** 必须与「帧的 ERA 是否落在 App 窗口 VA」取交集
   （`la_frame_priv_auto` / `arkrtos_port_frame_apply_priv_auto`）。只看属性会让卡在系统调用里的用户
   任务以 PLV3 回到内核代码上取指，PPI 不写 ERA、`ertn` 又回同一条指令 —— 实测整机停住、串口一个字
   都不再出（`QEMU -d int` 抓到 `pc=0x002203C4 arkrtos_task_wait_internal+0xa4` 反复 exception 7）。
7. **所有恢复路径必须共用同一个定级函数。** 本核 trap 出口（`la_frame_apply_priv`）、切换路径
   （`la_switch_prepare`）、次核 prepare（`arkrtos_port_frame_apply_priv_auto`）现在都过
   `la_frame_priv_auto`；三处各写一遍就是三个各写各的漏洞。
8. **演示/测试代码禁止手写 CSR 临界区内联汇编。** 手写版（`csrrd CRMD` + `csrxchg` 清 bit2）在 `-O2`
   下把掩码常量放在一个 callee-saved 寄存器里跨调用复用，实测出现过掩码变成 `0x10`（`CRMD.PG`）——
   那一下关掉的不是中断而是**分页**，随后任何跨页取指都变成 PPI，整机自锁（`-d int` 抓到
   `pc=0x24e640` 反复 exception 7）。走内核的 `ARKRTOS_ENTER_CRITICAL` 宏：掩码在函数内是局部量、
   进入/退出成对，不依赖调用者的寄存器分配。

9. **`want_user` 与 `is_priv` 语义相反，翻错一次是静默的。** `la_frame_priv_auto(f, want_user)`
   的第二参数是「该不该是用户态」（1 = 用户），而 `s_next_pplv` 是 PRMD 原值（0 = 内核、3 = 用户），
   `s_pend_priv` 记的是 `is_priv`（1 = 内核）。R3 三处调用点都按 `is_priv` 直接传了下去，结果**所有
   用户任务都以 PLV0 恢复**：默认档全部用例、SMP 档 `[shva]` 读数、乃至 `-smp 2` 的八项 PASS 全绿，
   隔离已经形同虚设却没有任何一条读数报警。暴露它的是 `fork` 门禁里的 `mprotect` 档 —— 那条路要按
   `PRMD.PPLV` 决定「杀任务」还是「内核写只读页 = 致命」，于是 8/8 确定性停机（同档在 R2 版本上 2/2
   通过，改动点在 R3，定位是二分比对出来的）。修法：三处显式翻一次语义，并在 `tools/paging_gate.py`
   的 pin K 里钉死 `(s_next_pplv != 0u) ? 1u : 0u`（改回 `==` 必须报错）。教训本身比缺陷重要：
   **「权限放松」不会自己报错，只有需要区分权限的用例才能测出来。**

#### 9.12.5 默认档零足迹（附一处 +144 B，必须解释清楚）

R3 不引入新开关，整段包在既有的 `ARKRTOS_USE_SMP`（默认 0）+ `ARKRTOS_USE_PAGING` +
`ARKRTOS_USE_PRIV` 之下。默认档同口径编两次：

| | text | data | bss |
|---|---|---|---|
| R2 基线（`751b1cc`） | 449888 | 456 | 5128740 |
| 带 R3 的默认档 | 450032 | 456 | 5128740 |

`text +144 B`；`nm` 里 R3 的符号（`install_space_window` / `va_in_window` / `relocate_frame` /
`apply_priv_auto` / `smp_sched_prepare` / `win_stat` / `smp_trap_stacks`）全为 0。多出来的 144 B 只有
一处来源：`arkrtos_port_paging_activate()` 的自证从 `tlbsrch` 版换成「按索引 `tlbrd` 比首末两对条目」
（`la_pg_win_entry_ok`），这是一次**缺陷修复**（第 5 条教训），不是新功能 —— 默认档本来就每次都跑
activate 的自证。

`smp_gate` 在源头把这条钉住：`arkrtos_paging_bind_task()` 里 `relocate_frame` 的调用点之前必须是
`((SMP==1)&&(PRIV==1))` 守卫且中间没有 `#endif`；把守卫弱化成只判 `PAGING`，同一条判据必须报错。

#### 9.12.6 实跑读数

SMP 档（`-DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2 -DARKRTOS_USE_SMP_SCHED=1`，板级
`PAGING=1 PRIV=1`），`-smp 2`，跑 30 s：

```
[smp] pg cpu1 on=1 rc=0 look=1
[smp] pg result=ok
[sched] to cpu1 rc=0
[shva] id=9 to rc=0
[shva] win inst=1 rc=0 space=0
[shva] ack=1 core1=1 data=0x00001235 loops=46896894
[shva] back rc=0 owned=0 winrc=0
[shva] result=ok
```

逐条读：

- `win inst=1 rc=0 space=0` —— 次核在恢复这个任务前**自己装了一遍** space 0 的窗口，读回自证通过；
- `core1=1` —— 迁移期间次核的 `cur` 确实指向它（不是「发了迁移但没跑」）；
- `data=0x00001235` —— CPU0 往 `0x02100800` 写种子 `0x1234`，次核经**同一段 VA** 读出、加一、写回，
  CPU0 再在同段 VA 读回来。这是跨核共享 VA 的直接证据，不是「两边都写了」；
- `loops=46896894` —— 次核确实在这个任务里跑了几千万次自旋；
- `back rc=0 owned=0 winrc=0` —— 归属回到 CPU0、次核空闲、观察者自己的窗口访问一次都没出错；
- 同一次启动另外 8 项 PASS（FS / ELF tamper / linux / execve / pipe / sig / MOD tamper / fcsr），无 FAIL。

#### 9.12.7 门禁

`tools/smp_gate.py` 新增 **pin R3**，五层断言、每层都带自检：

1. port 5 个标记 + sched 5 个标记逐个存在；把任一处替换成 `/* removed */` 后判据必须报出来；
2. `arkrtos_port_paging_install_space_window` 必须**恰好 2 处定义**（页级窗口档 / 静态窗口档各一条）；
3. `paging.c` 里 `relocate_frame` 的调用点必须被 `((SMP==1)&&(PRIV==1))` 守卫覆盖；把守卫换成只判
   `PAGING`，判据必须报错（否则「默认档不编」只是巧合）；
4. 对象级：`smp_sched.c` 在 `SMP=1 + SMP_SCHED=1` 下编出的目标文件里必须有
   `arkrtos_smp_sched_prepare` / `arkrtos_smp_sched_win_stat` / `arkrtos_smp_trap_stacks`；两个开关都关
   时一个符号都没有；把 `prepare` 改名重编，判据必须随之报错（证明读的是真符号）；
5. 夹具（`example/qemu_loongarch64/main.c`）必须真的打印 `[shva] win inst=` / `[shva] ack=` /
   `[shva] result=` 并调用 `arkrtos_smp_sched_selftest`（有判据，不是只打印）。

宿主侧另加两档覆盖率用例（`tools/coverage_gate.py`）：`pagingsmp`（`arkrtos_paging.c` 在 `SMP=1` 下编，
覆盖 `bind_task` 的 relocate 与失败回滚）、`smpschedwin`（`arkrtos_smp_sched.c` 在
`SMP=1/PAGING=1/PRIV=1/CPU_NUM=3` 下编，覆盖 `sms_plan_prepare` 三条判决与 `prepare` 的三步顺序）。
另有一处判据是收尾时才补的：`tools/paging_gate.py` 的 pin K 新增「`want_user` 换算不许写反」
（第 9 条教训）。它不在 R3 的功能面上，而是这一轮**静默回归**的唯一防线 —— 没有 mprotect 档，三处
调用点写反也能全绿。

`kernelsrc/tests/test_smp_sched_win_host.c` 是新用例；`smp_sched` 的状态全是静态、没有 reset 接口，
所以用例一律「夹具归一 + 读数取增量」收尾，不写死绝对值。

两档实测：`arkrtos_paging 100.00%`（基线 100.0）、`arkrtos_smp_sched 98.67%`（基线 97.89）。

#### 9.12.8 判词与三条不许读法

判词：**`r3_cross_core_shared_va_ok_on_qemu`** —— LA64 / QEMU `-smp 2` 上，次核在切到用户任务前自己把
该空间的窗口铺进本核 TLB 并按索引读回自证，恢复前按帧自己的返回地址定特权级，两核经同一段窗口 VA
读写同一块物理帧；默认档零新符号（`text +144 B` 是 activate 自证改按索引读回的缺陷修复）。
**真实硬件未验证。**

1. **不能读成「TLB shootdown 已具备」**：R3 只是 §5.4 五条前置里的第三条。次核拿到的是「那一刻的
   条目」，引导核之后对这个空间做 COW / 换空间 / mprotect 重写条目，次核**看不到** —— R4（失效回执）
   一行都还没做，shootdown 仍然不实现。
2. **不能读成「对称多核」**：次核上的 SVC 依然一律致命停机，全局调度器依然只有一份；R3 只让
   「被显式迁移的用户任务」在次核上跑得起来。
3. **不能把 `[shva] result=ok` 读成「两核窗口一致」**：它只证明这一次、这一段保留 VA、在这个时刻是
   同一块帧。窗口是漂移状态，两个核各自跑到不同空间时它们的窗口本来就不该一致；一致性由调度器保证
   （谁跑在这个空间就装这个空间），不是由硬件保证。

## 10. `-Wconversion` 开档（2026-11-19）：默认档镜像逐字节不变

`tools/warn_gate.py` 的选项集从六条扩到七条，新加 `-Wconversion`（GCC 隐含同开
`-Wsign-conversion`）。此前 21 个宿主可编 TU 上共 33 条：6 条窄化 + 27 条符号，当时的判断是
「逐条改等于改数据通路，要动必须先定口径」。这次按两条口径改完：

1. **只加显式转换**，不改类型、不改比较语义、不改算法。窄化处写 `(uint8)` / `(uint16)`，
   把原本隐式发生的截断写明；句柄掩码比较写 `((uint32)handle & MASK)` —— int32→uint32
   是保值转换，位模式逐位相同。
2. **判据是默认档 LA64 镜像逐字节不变**，不是「编译过了」。

33 处分布：`arkrtos_sync.c` 15（含 `cond_handle` / `mtx_handle` 两族）、`arkrtos_mq.c` 7、
`arkrtos_eset.c` 5、`arkrtos_fifo.c` 3（`fifo->size` 与 `rd_idx`/`wt_idx` 取模后显式截到
`uint16`）、`arkrtos_sbuf.c` 2（默认档该 TU 编成空目标，由 armclang 开档那一次覆盖）、
`kernelsrc/tests/test_eset_host.c` 1。

同口径读数（`SOURCE_DATE_EPOCH=1600000000`、`objcopy -O binary`）：

    before  text 367512 / data 440 / bss 767508   sha256 9e59d693…   31,462,512 B
    after   text 367512 / data 440 / bss 767508   sha256 9e59d693…   31,462,512 B

`cmp -l` **0 处不同**。这不是「编了以后一样」的巧合：`arkrtos_mq.c` 等源文件的补丁时间
（09:03:00）早于目标文件与 ELF 的重建时间（09:03:20），新镜像确实是改后代码编出来的。
结论：**显式转换对 LA64 的 codegen 完全没有影响**——转换只改变编译器看到的意图，
不改变生成的指令与数据，这正是这条口径要证明的事。

**编入档（防假绿）**：默认档 `arkrtos_sbuf.c` 是空目标，所以另用 armclang F427 做
`-O2 -c` 单 TU 真编译，HEAD 与工作区各一次，五个 TU（`mq` / `sync` / `fifo` / `eset` /
`sbuf`，后者带 `-DARKRTOS_USE_SBUF=1`）**告警 0 条 = 0 条**，`fromelf -z` 的
Code / RO / RW / ZI 逐项相同：`mq` 3932/128/0/1568、`sync` 9726/288/4/2248、
`fifo` 300/48/0/12、`eset` 2504/72/0/432、`sbuf` 9290/184/0/1264。

门禁：`warn_gate.py` → `WARN-GATE OK (21 个宿主可编 TU … -Wconversion 零告警 …)`；
`ci_gate.py` 全量 **47 passed / 0 failed / 0 skipped**（`MQ-HOST` / `SBUF-HOST` 回归
照旧全过，证明显式转换没改行为）。

同日另开 `-Wcast-qual`：2026-11-17 记的 14 条（全在 `kernelsrc/tests/test_eset_host.c`）
在同一天的复核里已经是 **0 条**（中间几笔把测试桩里的 `const` 丢法改掉了），所以这次开档
**源码零改动**，镜像不可能变化；开档前重新数一遍而不是照旧数字写口径，是因为照旧数字开档
只会让门禁先红一次。

同日再开 `-Wwrite-strings`：20 条全部是 `create(name)` 传字面量给 `char*` 形参
（`sbuf` 6 / `sync` 10 / `mq` 4）。修法是把 `create`/`open` 的 `name` 形参 53 处
（公开头 23 / 内核定义 16 / SDK 14）加 4 处内部 `copy_name` 的 `src` 改成 `const char *` ——
改前 grep 过 `name = name` 一类赋值，只有 `arkrtos_rt.c` / `arkrtos_sec.c` 保存名字、
形参本来就是 `const char *`，其余全拷进定长数组，所以是纯签名放宽（调用方传 `char *`
照样编过，ABI 与指令不变），并删掉 `test_eset_host.c` 里 14 处多余转型。
**教训：`-Wcast-qual` 与 `-Wwrite-strings` 是耦合的** —— 单开 `-Wcast-qual` 时字面量还是
`char[N]`，那 14 条一条都不报；「单开 cast-qual 得 0 条」不能读成「转型已改掉」。
`-Wpointer-arith` / `-Wundef` / `-Wbad-function-cast` 同日也一并打开（复核各 0 条、源码零改动，纯钉住防退化），选项集至此十二条。

**结论分档：宿主 21 个 TU 十二条选项零告警 + 默认档 LA64 镜像逐字节不变（`cmp -l` 0 处）
+ 编入档（armclang F427 九个 TU，含 driver SDK 的 drv_oslib）零告警且尺寸逐项不变，未上板。** 这一笔只改类型标注，
没有新增可上板的行为，上板不会带来新的信息 —— 不假装有第四个档。

## 5.13.4 收口（2026-11-19 晚）：`KERNEL refill (plv0) badv=0xffffff00` 的真根因是「关中断掩码写错」

**这一节销掉的是上一节写「另一族现场仍在」的那一族**（`badv=0xffffff00 era=0x00244d84`，
`eentry` 建帧时 `sp=0`），并一并销掉同源的另两种现场：

| 形态 | 现场 | 说明 |
| --- | --- | --- |
| A' | `[LA64U] KERNEL refill (plv0) badv=0xffffff00 era=0x00244d84` | `eentry` 的 `st.d $ra,$sp,0` 用 `sp=0` 算出 `0xffffffffffffff00` |
| A  | `badv=0x154548f8 era=0x00215a20` | `arkrtos_sched_activate` 里 `st.w` 写野地址（`$r24` 已是别处的值） |
| B  | `ecode=0x0d era=0x9` | 从被写坏的上下文里取指/访存 |

### 一、取证手段：`-d int`，零代码改动

之前的诊断都是往热路径里塞快照（`g_r24_pre/in/post`、refill 入口寄存器快照），
这本身有海森堡效应（v4 快照 80 例 0 次形态 A，干净基线 2/16）。本轮改用 QEMU 自带的
异常跟踪，**一个字节都不改**：

```
cd build/qemu_loongarch64
qemu-system-loongarch64.exe -M virt -m 128M -display none \
  -serial file:ser.txt -kernel ark_qemu_la64.elf -d int -D int.log
```

命中一例后的关键序列（167 MB / 340 万行日志里读出来的）：

```
loongarch_cpu_do_interrupt: PC 0000000000244d80 ERA 000000000023e458 cause 0
helper_ertn: ERA 000000000023e458
loongarch_cpu_do_interrupt: PC 0000000000244d80 ERA 0000000000245000 cause 0   <-- tick 落在恢复序列里
helper_ertn: ERA 0000000000245000                                              <-- 反复 ertn 回 0x245000
...（约 250 次）
loongarch_cpu_do_interrupt enter: pc 0000000000244d84 ERA 0000000000245000 TLBRERA 1
                                    exception: 2 (Page invalid exception for store)
```

`0x245000` 位于 `arkrtos_port_switch_task_inline` 的 **GPR 恢复序列中段**（恢复 ERA/PRMD
之后、`ertn` 之前）。那里本来应该有两条「关中断」指令，写的是：

```asm
ori     $t1, $zero, 1
csrxchg $zero, $t1, 0x0
```

### 二、根因：`csrxchg` 的掩码 `1` 清的是 CRMD.PLV，不是 CRMD.IE

LoongArch 的 `CRMD` 位序是 `PLV[1:0]`（bit0/1）、**`IE`（bit2）**、`DA`（bit3）、`PG`（bit4）。
`csrxchg rd, rj, csr` 用 **`rj` 当掩码**，所以掩码写 `1` 只动 bit0（PLV），
**IE 一直开着** —— 注释写着「存档之后立刻关中断」，实际一条也没关掉。

旁证两条，互相独立：

1. C 侧 `arkrtos_port_enable_irq()` 用的是 `addi.w $r12,$r0,4` + `csrxchg $r12,$r12,0x0`
   （掩码 4 = bit2），它确实能让节拍进来；同一文件里 SMP 次核入口那句
   `ori $t1,$zero,0x4 /* CRMD.IE */` 也是 4。只有「关中断」那几处写成 1。
2. 修好掩码之后，`-d int` 采到的 **36 971 次中断里，落在三个恢复窗口的 ERA 为 0 次**
   （窗口：`0x244e44..0x244ed0`、`0x244fa4..0x24502c`、`0x245038..0x245068`）。

于是因果链是完整的：

1. tick 在 `0x245000` 把 CPU 拿走（IE 根本没关）；
2. 硬件的 `ERA = 0x245000` 存进新帧，恢复后 `ertn` 回到 `0x245000`；
3. `0x245000..0x24502c` 之间没有任何一条写 `CSR.ERA` 的指令 ⇒ **`ertn` 跳回自己**，
   每轮都用上一轮的 `$r23` 当帧基址，`sp` 槽被逐轮读成别的值，最终变 0；
4. `sp=0` 时再来一次 tick ⇒ `eentry` 的 `st.d $ra,$sp,0` 打在 `0xffffffffffffff00`
   ⇒ 就是 `badv=0xffffff00 era=0x00244d84`。

形态 A 是同一根因的另一半：中断落在恢复序列里 ⇒ 恢复出的 `$r24` 属于别的上下文
⇒ `arkrtos_sched_activate` 里 `mul.d` 放大成野地址 ⇒ `st.w` 崩溃。

> 口径提醒（本轮踩过）：`[Ark RTOS] FATAL:` 那一行的 `era/badv/crmd` 是**陈旧值**。
> LoongArch 的 TLB 重填只写 `TLBRERA/TLBRPRMD/TLBRBADV`，不写 `ERA/PRMD/BADV`。
> 只有 `[LA64U] KERNEL refill (plv0) badv=… era=…` 这一行（从重填镜像 CSR 取）在现场。

### 三、修法

`kernelsrc/port/loongarch/la64/arkrtos_context.S`，四处「关中断」的掩码 `1` → `0x4`：

- `arkrtos_port_switch_task_inline` 存档后的那一处（原有，一直是空操作）；
- `eentry`、`arkrtos_port_switch_task_inline`、`arkrtos_port_resume_task` 三处恢复序列
  起点前**新增**的同一句（`la_switch_inline` / `la_switch_prepare` 的临界区退出会把
  IE 恢复回进入时的值，恢复序列因此需要自己再关一次）。

`ertn` 按 `PRMD.PIE` 恢复 IE，所以关掉不改变语义。代价只有新增的 3×2 条指令：
链接后 `text 369216 -> 369240`（**+24 B**）、`data/bss` 不变（掩码那 4 处是等长替换）。

### 四、读数（判据都是「实跑」，不是「编译过」）

基线（`77c0470`，干净 worktree）：`_parboot.py` 16 例 × 12 s 曾 4/16、5/16 fatal。

修复后（同一台机、同一 QEMU、同一命令 `utest`）：

| 批 | 规模 | 结果 |
| --- | --- | --- |
| 1 | 16 例 × 15 s，`utest` | **0 / 16 fatal**，`PASS audit_structures_consistent` 16/16，`FAIL` 0 条 |
| 2 | 16 例 × 30 s，`utest` | **0 / 16 fatal**，`PASS` 15 条 / 例，`FAIL` 0 |
| 3 | 16 例 × 30 s，无命令（纯 boot + demo + 节拍） | **0 / 16 fatal** |
| 4 | `-d int` 单例 × 60 s（36 971 次中断） | 恢复窗口内 ERA **0 次**，无 `KERNEL refill`，无 `ertn` 自环 |
| 5 | `-smp 2` 8 例 × 30 s + `utest` | **0 / 8 fatal** |

`tools/ci_gate.py`：**47 passed / 0 failed / 0 skipped**（与修复前一致）。

**任务 B（`audit_structures_consistent` 报 `r.mask == 0x40`）同源收口**：修复后
`utest` 三批共 48 例全部 `PASS audit_structures_consistent`，不需要再改
`arkrtos_sched_check()` —— 那个 `0x40`（`ARKRTOS_AUDIT_SCHED`）是 `.bss` 被野写的结果，
不是调度判据本身的缺陷。这是「别把下游症状当独立缺陷修」的一次实例。

### 五、边界（不许读过头）

- 本轮只做了 **QEMU LA64**；真实硬件（loongson）上 `csrxchg` 语义相同，但**未上板**。
- `-smp 2` 的 8 例只是 0/8，**没有冻结判据**，不构成「SMP 间歇停机已证明修好」；
  §三 欠账盘点里的 SMP 行按读数更新，不当结论用。
- 原始 §5.13.4 那例（`frame@0x003071d0`、`s_pi_chain_stack_mid`、`ra=0`）是
  「中途任务从入口返回」，与本节两族**不同源**。**这一族已于 2026-11-23 收口**：
  任务函数返回时入口帧的 `ra` 是 0，`ret` 直接跳到地址 0（现场就是 `ecode=0x0d (INE)
  era=0`）。修法是新增 `arkrtos_task_entry_return()` trampoline，LA64 / RV32 的
  `arkrtos_port_stack_init` 都把入口帧的 `RA` 指向它（log 一行
  `task <id> entry returned -> auto exit`，然后 `arkrtos_task_kill_internal()`，
  再 `wfi` 死循环）。判据是实跑日志里出现该行且不再有 `FATAL`；LA64 已实跑，
  RV32 仅编译。细节见 §9.9.7。

## 11. 开源 CPU 模拟器选型：ARM / RISC-V / LoongArch，带 MMU 与带 MPU（2026-11-19）

本节要回答的问题不是「哪个模拟器最好」，而是两个更窄的问题：**这台 Windows
机器今天能开哪些机型**，以及**为了让我们再跑一个 ISA 家族，要付多少成本**。
证据按三档写，混着读会被当成同一种证据：

- **本机实测**：命令在今天这台机器上跑过，输出可复现；
- **仓库/文档事实**：读了上游仓库文件或发布页 API，没有在本机执行；
- **未核实**：只登记候选，不当证据用。

### 11.1 本机实测：QEMU 9.2.3 能开什么

```bash
$ ls D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-*.exe | wc -l
58
$ D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-arm.exe -M help
lm3s6965evb          Stellaris LM3S6965EVB (Cortex-M3)
lm3s811evb           Stellaris LM3S811EVB (Cortex-M3)
microbit             BBC micro:bit (Cortex-M0)
mps2-an385           ARM MPS2 with AN385 FPGA image for Cortex-M3
mps2-an386           ARM MPS2 with AN386 FPGA image for Cortex-M4
mps2-an500           ARM MPS2 with AN500 FPGA image for Cortex-M7
mps2-an505           ARM MPS2 with AN505 FPGA image for Cortex-M33
mps2-an511           ARM MPS2 with AN511 DesignStart FPGA image for Cortex-M3
mps2-an521           ARM MPS2 with AN521 FPGA image for dual Cortex-M33
musca-a / musca-b1   ARM Musca-A/B1 board (dual Cortex-M33)
netduino2            Netduino 2 Machine (Cortex-M3)
netduinoplus2        Netduino Plus 2 Machine (Cortex-M4)
olimex-stm32-h405    Olimex STM32-H405 (Cortex-M4)
stm32vldiscovery     ST STM32VLDISCOVERY (Cortex-M3)
versatileab / pb     ARM Versatile/AB, /PB (ARM926EJ-S)
vexpress-a9          ARM Versatile Express for Cortex-A9
vexpress-a15         ARM Versatile Express for Cortex-A15
virt-*               ARM Virtual Machine（含当前版本）
```

```bash
$ qemu-system-aarch64.exe -M help      # mps3-an524/an536/an547, sbsa-ref, virt, raspi0..4b, vexpress-a9/a15
$ qemu-system-riscv32.exe -M help      # none, opentitan, sifive_e, sifive_u, spike, virt
$ qemu-system-loongarch64.exe -M help  # none, virt
$ qemu-system-loongarch64.exe -cpu help# la132, la464, max
$ qemu-system-riscv32.exe -cpu help    # lowrisc-ibex, rv32, rv32e, rv32i, sifive-e31, sifive-e34, sifive-u34
$ qemu-system-arm.exe -accel help      # tcg
```

两个直接结论：

1. **Windows 官方构建只有 TCG**（`-accel help` 只列 `tcg`），所以全部机型都是
   解释执行，「哪个更快」不该成为选型主因。
2. **ARM 侧机型密度最高**：Cortex-M0/M3/M4/M7/M33 与 Cortex-A9/A15 都在，
   而我们 `kernelsrc/port/arm/` 下已经有 cortex-m3 / cortex-m4 / cortex-m7
   三套 port（含 SysTick、PendSV、SVC、MPU 分支），却**没有一个 QEMU 目标**
   —— `example/` 下只有 `qemu_riscv32` 与 `qemu_loongarch64`。

### 11.2 MMU 与 MPU 分别落在哪些机型上

| 机制 | 本机存在的机型 | 与我们代码的关系 |
|---|---|---|
| MPU（PMSAv7，8 区域） | `mps2-an385/an386/an500/an505`、`netduino2/plus2`、`stm32vldiscovery`、`lm3s6965evb/811evb`、`olimex-stm32-h405` | `port/arm/cortex-m3` 与 `cortex-m4/m7` 里已有 `ARKRTOS_USE_MPU` 分支（区域配置 + idle 上下文），但从未在模拟器上执行过 |
| MMU（VMSA，两级页表） | `vexpress-a9`(Cortex-A9)、`vexpress-a15`、`aarch64 virt`、`sbsa-ref` | 需要异常级切换、页表遍历、TLB 维护，工作量是 MPU 的数倍；本仓库 LA64 侧已有 SVC 0x18 取布局的 MMU 证据，ARM 侧没有 |
| RISC-V MMU（Sv32/Sv39/Sv48） | `riscv32 virt`、`riscv64 virt`、`sifive_u` | rv32 已实跑（无 MMU 路径）；rv64 需要 64 位 `long` 与 `-mcmodel` 调整 |
| RISC-V PMP | 全部 riscv 机型（PMP 在 CPU 模型里）；`sifive_e`(E31) 无 MMU | 可作为「无 MMU 但有物理保护」的对照 |
| LoongArch 软 TLB | `loongarch64 virt`（la464 / la132） | 已实跑，含进程模型与 ELF 装载 |

### 11.3 候选模拟器清单

| 项目 | 许可 | Windows 可跑 | MMU | MPU | 对本仓库的适配成本 | 证据等级 |
|---|---|---|---|---|---|---|
| QEMU 9.2.3（已装） | GPL-2.0 | 官方 w64 构建，已在用 | 有 | 有 | 0：只缺板级 | 本机实测 |
| 上游 QEMU master/11.x 自建 | GPL-2.0 | 需 MSYS2 或 Linux 宿主 | 有 | 有 | 高（整套宿主编译环境） | 仓库事实（`target/loongarch/Kconfig` 只有 LOONGARCH64） |
| `la32r-QEMU`（loongson-edu） | GPL-2.0 | 发行物只有 Linux/Mac | 无（LA32R 无 MMU） | 无 | 中（需 Linux/WSL 宿主） | 仓库事实（releases API） |
| Renode（Antmicro） | Apache-2.0 | 有 Windows 安装包 | 有（外部 MMU 模型） | 有（Cortex-M 模型） | 中：要写 `.repl` 平台描述与 UART 外设映射 | 上游文档，未在本机安装 |
| TinyEMU（Bellard） | MIT（仓库 `MIT-LICENSE.txt`） | 源码可 mingw 编译；官方发行物以 Linux + 在线 JS 为主 | 有（RV32/64） | 无 | 无 ARM；只覆盖 RISC-V | 未核实 Windows 二进制 |
| rv32emu（sysprog21） | MIT | 需自行 make（MSYS2/mingw），未核实 | 有（Sv32，系统模式） | 无 | 中：RV32IMAFC + virtio，但外设与我们不同 | 未核实 |
| mini-rv32ima（cnlohr） | 宽松（单头文件） | 单头文件，任何 C 编译器可编 | **无**（作者明确说明无 MMU） | 无 | 低，但撑不起 app 隔离 | 未核实 |
| Spike（riscv-isa-sim） | BSD | 以 Linux 为主 | 有 | 无 | 参考 ISS，无外设生态 | 未核实 |
| SkyEye | GPL-2.0 | 有 Windows 版 | 有（ARM 等） | 部分 | 代码老、接口陌生 | 未核实 |
| Unicorn | GPL-2.0 | Windows wheel 有 | 有（CPU 内） | 有（CPU 内） | **只有 CPU 核，无整机外设**，不能直接跑镜像 | 未核实 |

这张表的用法要写死一句：**「支持某架构」不等于「能跑我们的镜像」**。我们要的是
具体外设（CMSDK APB UART、CLINT/ACLINT、PLIC、16550），以及确定的加载地址与
启动约定；只实现指令集的模拟器（Spike、Unicorn、mini-rv32ima）能跑纯计算，
跑不了带串口输出的内核。

### 11.4 推荐顺序（按「已有 port × 成本」排，不是按「名气」排）

1. **`mps2-an385`（Cortex-M3 + MPU）** —— 最短路径：`port/arm/cortex-m3` 已把
   SysTick、PendSV、SVC、MPU 写完，缺的只有 board（CMSDK APB UART0 在
   `0x40004000`）、startup 与 linker script。做成以后，本仓库第一次有
   **第三个 ISA 家族**在模拟器上跑，也是 MPU 保护机制唯一能在模拟器里验证的地方。
2. **`riscv64 virt`（Sv39）** —— rv32imac 已实跑，rv64 主要是 `long`/指针宽度、
   `-mcmodel=medany` 与页表三处改动，工作量中等。
3. **`aarch64 virt`（EL1/EL0 MMU）** —— 收益是「MMU 进程隔离」能在两个 ISA 上
   对拍，但要写异常级、GIC、页表，成本最高，排在 LA64 已有 MMU 证据之后。
4. **LA32** —— 无载体（§6.1.3），不动。

### 11.5 判词与不许读法

- `sim_machine_matrix_local`（本机 58 个 system 模拟器的机型清单）= **实测**，
  表在 §11.1；
- `arm_sim_target_missing`（ARM 有 port、无 QEMU 目标）= **实测**：`example/` 下
  只有 `qemu_riscv32`、`qemu_loongarch64`、`la32_generic` 与三个 stm32 实机板；
- `next_sim_target_is_mps2_an385` = **建议**，不是结论，也不代表已完成。

三条不许读法：

1. 不许把「Renode/TinyEMU 支持 RISC-V」读成「我们的镜像能在上面跑」——外设模型
   对不上就是跑不起来；
2. 不许把「带 MMU 的模拟器很多」读成「我们的 MMU 代码已经验证过」——验证是
   在具体机型上跑出具体读数才成立的；
3. 本节除 QEMU 9.2.3 的机型清单外，**没有在 Renode / TinyEMU / rv32emu /
   mini-rv32ima / Spike / SkyEye / Unicorn 上执行过任何东西**，这些行只能当
   候选清单读。

## 12. ARM Cortex-M4 上板（MPS2-AN386，2026-11-19，QEMU 实跑）

这一节是 §11.4 推荐顺序的第 1 条落地。先写结论：**镜像在 QEMU 的
`mps2-an386` 模型上跑起来了，节拍、任务切换、SVC 读数、信号量同步四条链路
都有读数**；同时**这一档没有验证 MPU 保护机制**，因为 MPU 在本档是显式关闭
的（理由见 §12.3）。这两句必须一起读。

### 12.1 为什么是 an386 而不是 §11.4 写的 an385

§11.4 写的是 `mps2-an385`（Cortex-M3 + MPU），理由是「`port/arm/cortex-m3`
已把 SysTick、PendSV、SVC、MPU 写完」。实际动手时卡在一个更靠前的位置：

| 目标 | 上下文切换汇编 | 能否用 `arm-none-eabi-gcc` 编 |
|------|----------------|-------------------------------|
| `cortex-m3` | 只有 `arkrtos_context.S`（Keil `AREA`/`PROC` 语法） | 不能 |
| `cortex-m4` | `arkrtos_context.S` + **`arkrtos_context_gcc.s`**（GNU as 版） | 能 |
| `cortex-m7` | 同 M3，只有 Keil 版 | 不能 |

GPT 版 GCC 工具链不认 Keil 汇编语法，所以 M3 目标这一轮做不了「零改动」上板：
要么先写一份 M3 的 GNU as 上下文切换（等于新建一个 port 文件，属另一件事），
要么选 M4。M4 与 M3 在 SysTick / PendSV / SVC / NVIC 上完全同构，指令集是
M3 的超集，所以**这一档的读数对 M3 的启动链同样有效，只差 MPU 与 FPU 两处**。

### 12.2 机器事实（来自 QEMU 9.2.3 `hw/arm/mps2.c`）

| 项 | 值 | 说明 |
|----|----|------|
| SYSCLK | 25 MHz | `SYSCLK_FRQ 25000000`，同时是 UART 的 `pclk` |
| 镜像区 | `0x00000000..0x003FFFFF`（4 MB） | SSRAM1；`0x00400000` 是同一块的别名 |
| 数据区 | `0x20000000..0x203FFFFF`（4 MB） | SSRAM23 |
| 串口 | CMSDK APB UART0 @ `0x40004000` | `DATA 0x00` / `STATE 0x04`（bit0 TXFULL）/ `CTRL 0x08` / `BAUDDIV 0x10` |
| 定时器 | CMSDK timer0..3 @ `0x40000000 + i*0x1000` | IRQ 8..11；本档用 SysTick，不碰它们 |
| 装载上限 | `armv7m_load_kernel(cpu, kernel, 0, 0x400000)` | 4 MB 以内 |

**不要选 AN511 分支**：那个变体把 ssram1 放在 `0x00400000`、大小 8 MB，
与 an386 的布局不同，混用会让向量表落不到地址 0。

Cortex-M 的启动语义与 RV32/LA64 都不同：硬件不读 `ENTRY()`，而是复位后从
`0x0` 取初始 SP、从 `0x4` 取复位 PC。所以向量表必须**物理**落在镜像最前，
链接脚本的 `.text` 第一条因此写成 `KEEP(*(.text.start))`——少了这个 KEEP，
`--gc-sections` 会把整个向量表当没人引用的段删掉，而链接器不会报任何错。

### 12.3 板级档位：为什么 MPU / FPU 是关的

`board/qemu_mps2_an386/arkrtos_board_config.h` 里 FPU / MPU / PRIV 全部
**显式写 0**。这一步是必须的，不是省事：

- `ARKRTOS_USE_FPU` / `ARKRTOS_USE_MPU` 的默认值取自
  `ARKRTOS_ARCH_HAS_FPU` / `ARKRTOS_ARCH_HAS_MPU`，而 M4 的这两个宏都是 1。
  **不显式关就等于打开**——「默认值恰好等于想法」这种假绿形态在此前已经
  踩过一次（见 `docs/欠账盘点.md`）。
- 本档用 `-mfloat-abi=soft` 编，开 FPU 只多一份没人用的上下文保存；
- MPU 保护机制要单独的越权访问判据（造一次非法访问、看是否进 MemManage），
  那是独立的一轮工作，本次不做。**因此这一档的「跑通了」不包含 MPU。**

同时关掉的还有 FS / VFS / SHELL / LOG / BLK / MINIAPP / POSIX / SOFTBUS /
IMAGE_SIGN / SMP 等一整套，只留「节拍 + 任务 + SVC + 串口」。

### 12.4 实跑读数与四条判据

`build/qemu_mps2_an386` 下 20 秒实跑的串口（节选）：

```
[ark] mps2-an386 cortex-m4 bring-up
[ark] task_a=1 task_b=2 sem=19922944 sysclk=25000000 tick_us=500
[A] round=0 tick=7 d_tick=7 ms=3 d_ms=3 ms_svc=5 svc_ok
[B] woke, round=1 tick=18
[A] round=1 tick=1014 d_tick=1007 ms=507 d_ms=504 ms_svc=507 svc_ok
[B] woke, round=2 tick=1019
[A] round=2 tick=2017 d_tick=1003 ms=1008 d_ms=501 ms_svc=1009 svc_ok
...
[A] round=11 tick=11018 d_tick=1000 ms=5509 d_ms=500 ms_svc=5509 svc_ok
[B] woke, round=12 tick=11018
```

体积（`arm-none-eabi-size`）：`text 34304 / data 48 / bss 17072`。

四条判据（`tools/qemu_m4_gate.py` 逐条判，任一条不成立即 FAIL）：

1. **节拍**：稳态 `d_tick` 落在 990..1010。`tick_us=500` 且延时 500 ms，
   期望 1000 个节拍；实测 1000~1002（±2 个节拍 = ±1 ms 的 UART 打印抖动）。
2. **延时**：稳态 `d_ms` 落在 490..515，实测 500~504。
3. **SVC 读数**：`arkrtos_get_time_ms()`（走 `svc #imm` 与内核侧换算）与
   用户侧 `tick*500/1000` 在**同一时刻**取值，容差 ±1 ms。13 轮里零失配，
   且 `ms == tick*500/1000` 逐轮成立——两侧换算口径一致，不是「都对但都错」。
4. **信号量同步**：任务 B 每次唤醒都能对上一个 A 的轮次，轮次单调、无重复，
   说明 PendSV 上下文切换与等待队列都是真的在工作，不是打印骗人。

外加一条只有该家族才有的判据：**镜像头 8 字节**必须是
`SP=0x20400000`（RAM 尾）+ 复位 PC 落在镜像区且 Thumb 位为 1。这条判的是
「向量表真的在地址 0」，是 M-profile 独有的启动契约（RV32/LA64 由 ELF 入口
保证，不存在这个失效面）。

### 12.5 判词与边界

- `arm_cortex_m4_runs_on_qemu` = **QEMU 实跑**。证据是 §12.4 的四条读数
  + 镜像头 + `tools/qemu_m4_gate.py` 全绿（13 项）。
- 本档 **MPU / FPU / PRIV 全关**，所以这次不构成 MPU 或 FPU 的验证。
- QEMU 不是真硅：时序、UART FIFO、复位后外设默认值都只代表 QEMU 的模型。
  这一档的结论不能写成「在 M4 硬件上跑通」。
- 本档不含 FS / SHELL：`example/qemu_mps2_an386/kernel_stubs.c` 里的
  flash / shell 桩一律 `return -1`，文件系统用的是
  `kernelsrc/src/arkrtos_fs.c` 在 `ARKRTOS_USE_FS == 0` 分支里的 facade
  （同一份代码在所有没有存储的板子上共用）。这些桩带编译期护栏：一旦把
  对应开关打开，构建会 `#error`，不允许「开关开了但实现还是桩」的假绿。

三条不许读法：

1. 不许把「ARM 家族跑通了」读成「三个 ISA 家族的全功能档都验证过」——本档
   是**最小档**，FS/SHELL/LOG/SMP 全关，与 RV32/LA64 的全功能档不可比；
2. 不许把「Cortex-M4 跑通」读成「Cortex-M3 也验过」——M3 的上下文切换只有
   Keil 版汇编，本机没有 armclang 参与这条链路，所以 M3 侧未验证（见 §12.1）；
3. 不许把「QEMU 上跑通」读成「可以烧 STM32 了」——本档的板级头是给
   `mps2-an386` 写的，串口基址 `0x40004000`、`SystemCoreClock` 25 MHz 都是
   这台机型的值。

### 12.6 这一轮踩到的坑（按代价排）

| 坑 | 形态 | 修法 |
|----|------|------|
| `IRQn_Type` 未定义 | CMSIS `core_cm4.h` 假设设备头先给这个枚举，直接 include 会在 30 行内炸 6 处 | 自建设备头里先定义完整枚举，再 include |
| `SystemCoreClock` 未声明 | 该版 CMSIS 不声明它，port 层三处引用编译失败 | 设备头 `extern`，板级 C 里定义 |
| `__weak` 未定义 | 该版只给 `__WEAK`，`__weak` 原本由厂商 HAL 头提供 | 设备头里补 `#define __weak __attribute__((weak))` |
| `.s` 小写扩展名不走预处理 | `arkrtos_context_gcc.s` 里有 `#if`，直接汇编会把它当注释 | 汇编规则加 `-x assembler-with-cpp` |
| 链接缺 libc | `-nostdlib` 下没有 `snprintf`/`memcpy` | 自带最小 `libc_shim.c`（与 RV32/LA64 各一份同源） |
| 链接缺 `arkrtos_dev_board_init` | `arkrtos_kernel_module_init()` 无条件调它 | 板级 C 里明确空桩 + 注释说明「本档没有可注册设备」 |
| `-kernel` 吃不了绝对路径 | 仓库路径含中文，QEMU 用窄字符 API，直接失败 | `cd build/qemu_mps2_an386` 后只传文件名 |
| `ArkRTOS` 的 fs facade 被 `filter-out` 掉了 | Makefile 照抄 RV32 剔除了 `arkrtos_fs.c`，而 FS=0 时它才是符号来源 | 显式加回 `SRCS_C`，并补 `-DLFS_NO_*`（littlefs 配置头会断言的开关） |

