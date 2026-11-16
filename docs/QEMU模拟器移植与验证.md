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
约 1/12~1/15。加上 `kill_ret` 诊断后又连跑 **36 会话（每会话两次 `utest`）0 复现**
—— 这既不能读成「诊断代码把它治好了」（诊断不改行为），也不能读成「已经不会
再出」；只能读成「命中率在这个构建上偏低，还需要更长的连跑才抓得到」。
RV32 连跑 12 轮全过、带诊断代码的 RV32 单跑仍 `9 matched, 9 passed`。
**未复现不等于已修。**

## 6. LoongArch32 移植（仅编译通过，无运行载体）

> **档位：仅编译通过。** 本节的每一条都是「源码写出来了、工具链认了、
> 链接出了 ELF 并且反汇编核对过」，**不是「跑起来看过读数」**。
> 本节不存在任何串口输出、节拍读数或上下文切换的现场证据。

### 6.1 为什么是「参考目标」而不是 `qemu_loongarch32`

先查载体，再动手：

```bash
ls D:/env/tools/qemu/qemu9/mingw64/bin/ | grep loongarch
find D:/env/tools/qemu -name "qemu-system-loongarch32*"
```

两条都是空——QEMU 9.2.3 **只提供 `qemu-system-loongarch64`**，
上游也没有 loongarch32 系统模拟器；机器上同样没有 LA32 板子。
所以目录命名**故意不用 `qemu_` 前缀**：

```
kernelsrc/port/loongarch/la32/arkrtos_port.c        # HAL 实现（4 字节槽帧）
kernelsrc/port/loongarch/la32/arkrtos_context.S     # eentry / 就地切换 / resume / idle
board/la32_generic/arkrtos_board.{c,h}             # 「通用 LA32 参考目标」
board/la32_generic/arkrtos_board_config.h          # ARKRTOS_ARCH_CORE = 0x0301（LA32）
example/la32_generic/{main.c,start.S,link.ld.in,gen_ldconsts.py,libc_shim.c,Makefile}
```

叫 `qemu_loongarch32` 会让人以为有台机器能跑，那是在造证据。

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

`make -f example/la32_generic/Makefile run` 会打印一段说明并以退出码 1 结束：

```
ERROR: no LoongArch32 execution target is available.
  - QEMU 9.2.3 ships only qemu-system-loongarch64; upstream has no
    loongarch32 system emulator.
  - There is no LA32 board on this machine either.
  This target only verifies build + link: build/la32_generic/ark_la32_generic.elf
```

这是刻意的：宁可明确报错，也不给一个「看起来跑起来了」的错答案。

### 6.7 哪些结论是从 la64 搬来的（未在 LA32 上验证）

| 项 | 来源 | LA32 证据 |
|---|---|---|
| PRMD 位序（`PPLV[1:0]` / `PIE=bit2` ⇒ 回 PLV0 且开中断 = 0x4） | la64 QEMU 实测 | 无。`start.S`/`port.c` 按同一 `LA_PRMD_INIT` 写 |
| 无软件自中断 ⇒ 就地切换 | la64 QEMU 实测（同代硬件语义） | 无 |
| TIMER_HZ / RDTIME_HZ（100 MHz 计数、25 MHz 定时器） | la64 QEMU 实测 | 无。板级常量照搬，注释已标明 |
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
| 仍关 | `MINIAPP` / `POSIX` / `LWIP` / `SOFTBUS` / `APPPACK` / `APPRUN` / `FLASHAPP` / `QSPI` / `SMP` | 同 | 无（`FLASHAPP` 两侧都剔除：它无条件 include `drvflash.h`，两个目标都没有 Flash 控制器） |

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
 213480     180  578592  792252   c16bc  build/la32_generic/ark_la32_generic.elf
```

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
`/proc/version` 可读、`utest` 汇总 **`9 matched, 9 passed, 0 failed`**。

最后一条必须连 `matched` 一起看：只看 `, 0 failed,` 会把
「0 matched, 0 passed, 0 failed」（一条用例都没跑）判成通过 —— 这正是
LA64/RV32 上真实发生过的假绿（§5.13.3）。LA32 因为没有执行载体，这一条
现在**既不能判通过也不能判失败**，只能说「修法已同步进 `start.S` /
`link.ld.in`，符号 `__init_array_start/end` 在场（9×4 = 0x24 字节）」。

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
make -f example/la32_generic/Makefile run   # **只会报错退出 1**，见 §6.6
```

期望体积（全功能档 + `.ctors` 与 libc 补件，2026-11-16 实测）：
`text 227684 / data 196 / bss 582688`（此前的最小档是 213480/180/578592，
差额来自构造段遍历与字符串/libgcc 补件，§6.8）。
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
| RV32 | `sem=19922944 taskA=1 taskB=2 taskU=3 mpu=1 priv=1 taskRX=4 taskFS=5 blk=1 fs=1 vfs=1 shell=7 log=1` | 187544 / 128 / 590864 |
| LA64 | `sem=19922944 taskA=1 taskB=2 taskFS=3 blk=1 fs=1 vfs=1 shell=6 log=1 taskU=4 app_base=0x02000000 utest=1` | 247280 / 392 / 599072 |
| LA32 | 无（没有执行载体） | 227684 / 196 / 582688 |

三份板级配置里 `LOG / LOG_RING / BLK / FS / VFS / VFS_LFS / PROCFS / SHELL /
UTEST` 都是 1；`MINIAPP / POSIX / LWIP / SOFTBUS / APPPACK / APPRUN /
FLASHAPP / IMAGE_SIGN / QSPI / RTSTAT / SCHED_STAT / MDK_TRACE /
CM_BACKTRACE / CRASH_LOG / AUDIT / SECCHECK / SMP` 都是 0（本三目标的示例
配置，不代表这些功能不存在：它们由 `tools/ci_gate.py` 的「编入档」逐项
做 ON-path 编译核对）。

### 8.2 矩阵

| 功能 | RV32 | LA64 | LA32 |
| --- | --- | --- | --- |
| 节拍 / 任务切换 / 抢占 | **实跑**（`taskA/B` 500 ms 交替） | **实跑** | 编译 |
| 信号量 / 互斥 + 优先级继承 / FIFO / 事件 / 定时器 / mempool / mq | **实跑**（`utest 9/9`） | **实跑**（`utest 9/9`） | 编译 |
| 阻塞与唤醒（`wait` / `sem_post`） | **实跑** | **实跑** | 编译 |
| SVC 入口（`syscall 0x11` TASK_CTRL / `0x12` SYS_INFO） | **实跑** | **实跑**（PLV3 App 走同一条） | 编译 |
| 内核日志（ring）/ 内核 Shell（help/ls/cat/stack/utest/dt） | **实跑**（`shell_check.py`） | **实跑**（`shell_check.py` 10 条判据） | 编译 |
| blk（RAM 块设备）/ littlefs / VFS / procfs | **实跑**（`[FS] PASS`） | **实跑**（`[FS] PASS`） | 编译 |
| 任务级内存隔离 | **实跑**：MPU（PMP）+ U 模式，`mpu=1 priv=1 taskRX` | **实跑**：PLV3 + 静态 TLB 条目（§5.12） | **未做**（`priv=0`，无载体） |
| 越权访问的负对照 | **实跑**（`docs/` §3.10） | **实跑**（PPI / refill / IPE 三条，§5.12.8） | 未做 |
| UTEST 全量（9 例 / 90 断言） | **实跑** `9 matched, 9 passed` | **实跑** `9 matched, 9 passed` | 编译 |
| 调试/追踪族（mdk_trace / crash / rtstat / audit / sched_stat） | 未做（本示例关） | 未做（本示例关） | 未做 |
| 网络 / 软总线（lwip / softbus） | 未做（本示例关） | 未做（本示例关） | 未做 |
| App 装载族（apppack / miniapp / apprun / flashapp） | 未做（本示例关） | 未做（本示例关；且 LA64 缺 arkc 后端） | 未做 |
| SMP（多核） | 未做（无多核板） | 未做（QEMU `virt` 单核配置） | 未做 |
| tickless / waitq / swap / seccheck / sign | 编译档核对（`ci_gate`），未实跑 | 同左 | 同左 |

### 8.3 三条明确缺口（不要读成「等价完成」）

1. **LA32 没有任何执行载体**：`qemu-system-loongarch32` 不存在，现成板子
   也没有，所以矩阵里 LA32 那一列**全是「编译」**。接上载体后的第一件事
   写在 §6.8.4 末尾（六组读数逐条看）。
2. **SMP 无板**：`arkrtos_smp` 只有宿主侧计数与 ON-path 编译核对，没有
   任何双核实跑证据。RV32/LA64 的 QEMU 机器都是单核配置。
3. **App 整链（装载 + 执行）未在两个 QEMU 目标上跑**：现有实跑覆盖的是
   「内核 + 一个手工放置的 PLV3 探针」，不是「从文件系统读出镜像、校验、
   重定位、跳过去执行」。RV32 上这条链需要 arkc 的 RV32 后端同步，
   LA64 上连 arkc loongarch 后端都还不存在。

另有一条**已修但需记住**的：`-nostdlib` 下 `libgcc` 的内置函数与
`strspn/strcspn` 这类字符串函数都要自己补（§6.8），只在功能面真的打开时
才暴露 —— 最小档跑得再绿，也证明不了全功能档能链起来。
