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
`cat /proc/version`、`stack` 为 `5 task(s)` 且 `0 unmeasured`、
`utest` 汇总行 `, 0 failed,`、全程无 `UNEXPECTED`/`FATAL`/`FAILED`。
其中 `stack` 与 `utest` 两条是「注册了但没跑」挡不住的：前者要求栈计量
真的在算（`0 unmeasured`），后者要求用例全量跑完且 0 失败。

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
| **没有 ASID 区分**（`ASID` 恒 0） | 换 App 必须整 TLB 刷；多 App 无法共存 |
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
- **ASID / TLB shootdown（U-7）**：ASID 恒 0，切换靠重写条目。
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

- **ASID 与批量失效**：ASID 仍恒 0；没有 `flush_tlb_range` / 整表 flush
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
`exit_group(status)` 加 `wait()`；本档只做前半截：

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

#### 5.20.6 明确没做的部分

- **没有 `wait()`**：码只能由装载器台账**事后**读（`arkrtos_elf_info_space()`），
  父任务不能阻塞等一个指定子任务的码；也没做「一个进程多个子进程」的建模。
- **没有 `arkrtos_thread_exit()` 带默认码**：子 3 仍是纯退出，TCB 里 `exit_code_set`
  保持 0。C 的 `main` 返回值不会自动变成退出码——要让退出码成立，程序必须
  显式调 `arkrtos_thread_exit_code(code)`。
- **mini_sdk / driver_sdk 没接线**：只做了 `app_sdk` 的 `oslib`。
- **码只在 TCB 槽被复用前有效**：若任务死后立刻有大量建/删任务把该槽复用，
  码就丢了（实测 exec 的 20 ms 轮询使这窗口实际不可达，但机制上没有保护）。
- **板级只开了 LA64**：RV32 板配置没开，因此 32 位侧只有「关档零变化」这一条读数。

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
`0` = 还没分配；**拿过不还**（空间回收仍是欠账，如实登记，不做「偷偷还了
又可能被别人拿走」的那种实现）。

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
- **没有空间回收**：帧拿过不还，`ARKRTOS_PAGING_APPS_MAX` 个空间跑满之后
  再要空间就是 `-3`。谁负责还、什么时候还（任务退出？最后一个实例退出？）
  没有设计，登记为欠账。
- **没有 ASID**：切换空间仍靠整段 `invtlb`（512 次），跨核 TLB shootdown
  仍无从谈起（U-7）。
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

这一阶仍然**不还帧**（空间回收仍是欠账）：它改的是「什么时候装」，不是
「装完怎么回收」。

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

- **帧不还**：按需档只是「按对付款」，退出/切空间都不回收（与 §5.22 同一条欠账）。
- **没有 ASID**：切空间仍是整段 `invtlb`（U-7）。
- **refill 不区分 PLV**：内核与 App 走同一条路（5.23.3 里说明了为什么必须这样）。
- **没有页替换 / 淘汰**：帧池空了就是 `return 0` → 原来的致命路径，没有换出。
- **只在 LA64 板级实测**：RV32 侧只有「关档零变化」这一条读数。
- **`pmap u1/u2` 的帧号显示 0x0**：这一档的账本在 `s_pg_pair_pa`，shell 没有
  按需档专用的读数（要看清每一对，得加新命令，登记为可选）。

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
| `exec` 原生 ELF64（`ARKRTOS_USE_ELF`，PLV3） | 未做（`arkrtos_elf.c` 只支持 64 位 `long`，见 U-4） | **实跑**：`[ELF] ... rc=0` + 程序自报 `argc/argv`，连跑 10 次（[MMU进程模型与ELF装载](MMU进程模型与ELF装载.md) §5） | 未做（同左，编成空目标） |
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
   注意这一条**不再覆盖** `exec`（原生 ELF64）与 `ld`（可下载 `.o`）：这两个
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
