# 内存保护与外设权限（App 越权验证）

> 结论分三档：**仅编译通过** / **上板验证过** / **未验证**。
> §1–§6 是 **上板验证过**（STM32F427VGTx，DAPLink / SWD，内核 F427 工程，App 走 `app_sdk/APP_DEMO`）。
> §7 的 MMU 后端是 **QEMU 实跑**（LA64 跑真内核、真 `utest`、真 `sec`），不是上板；
> 两者分档标注，不混用，也不把 QEMU 的读数借给上板结论。

## 1. 这份文档回答什么

「让 MCU 像手机一样安装应用」的前提是：**装进来的第三方 App 不能碰内核和别的 App**。
本文说明这条路现在走到了哪一步：机制是什么、怎么验证的、哪几条边界**没有**解决（写清楚，
免得被当成已经解决）。

## 2. 机制：三层缺一不可

| 层 | 做什么 | 位置 |
|---|---|---|
| 编译期 | App 按标称布局链接，安装时由内核把 RAM/ROM 绝对地址重定位到**实际落点** | `tools/gen_scatter.py`、`arkrtos_loader_reloc_apply()` |
| 任务建立期 | 每个任务的 TCB 带 `is_priv`；App 任务 = 非特权（CONTROL.nPRIV = 1），内核任务 = 特权 | `arkrtos_task.h:is_priv`、`arkrtos_cfg.c`、`arkrtos_task.c:arkrtos_sched_activate()` |
| 运行期 | MPU 为每个任务建 region：本任务的 RAM 窗口（RW，XN）、内核 RAM / 外设（按需只读或禁入） | `arkrtos_mpu.c:arkrtos_mpu_build_task()` |

App 的 RAM 窗口来自 `config/arkrtos_partition.h` 的 `SLOT_RAM_*`：安装时内核从镜像池里分配
一个 2 的幂大小的块，把基址交给 MPU 建 region，同时把**这个基址**用于 RAM 重定位。

## 3. 本轮修复：RAM 基址必须固化（否则 App 一启动就复位）

### 症状

`app start <slot>` 后设备**复位**（`RCC_CSR` 读出 IWDGRSTF），槽位被 crash-limit 禁用。
用 `DEMCR.VC_MMERR` 向量捕获抓到出错现场：

```
PC  = 0x080650AA   (__scatterload_copy: stmhs r1!,{r3,r4,r5,r6})
R1  = 0x20014000   (写入目标 = App 自己 RAM 窗口的排他上界)
CFSR = 0x82 (MMARVALID | DACCVIOL)   MMFAR = 0x20014000
```

### 根因

App 启动时 `__scatterload` 按镜像里**已固化的** scatter 表把 RW 初始化数据搬到 RAM，
表里的 `dest` 就是安装那一刻的 RAM 基址。而 **RAM 分配结果不落盘**：上电扫描会按
「池内地址升序」重新分配一次。只要两次分配的结果不同（**重复安装**时旧绑定仍占着池，
就会不同），镜像里所有 RAM 绝对地址立刻作废 —— 于是写到 `0x20014000`，正好越出当前
8 KB 窗口，撞 MPU。

板上两组读数（同一份镜像，同一块板）：

| | RAM 基址 | 来源 |
|---|---|---|
| 安装当时（固化进 Flash） | `0x20014000` | 设备 Flash 里 scatter 表 `dest` 字段 |
| 上电重建（原来的行为） | `0x20012000` | 板上分区表 `slot_ram_base[1]` |

原代码注释里写着「分配顺序 = 池内地址升序，重建结果与装载时一致」——这个假设在重复安装
下不成立。

### 修法

**把安装时分配到的 RAM 基址写进镜像头，上电扫描优先复用它，不再重新分配。**

| 改动 | 文件 |
|---|---|
| 镜像头末尾新增 `runtime_ram_base`（头长与其它字段偏移**不变**；老镜像该处本来就是 0 → 天然向后兼容） | `kernelsrc/include/arkrtos_app_image.h` |
| 新增 `arkrtos_ptable_ram_reserve(base, size)`：按指定基址占用，校验越界 / 对齐 / 重叠 | `kernelsrc/src/arkrtos_ptable.c` |
| 安装时写入分配到的基址；扫描时**优先**按它预留，**要不到就明确报错、让该镜像暂时没有 RAM 块**（`start` 会拒启并要求重装），而不是换一个地址把它跑起来 | `kernelsrc/src/arkrtos_loader.c` |
| CRC：`runtime_ram_base` 与 `crc32`/`state` 一样**按 0 代入**（它不是"传输来的内容"，是安装方落盘前才填的本地信息；算进去会让每次安装都误报 CRC 不符） | `arkrtos_loader.c` 两处 CRC |
| 打包侧：`verify` 时若该字段非 0 直接报错（那说明拿板上拆下来的镜像当打包产物了） | `tools/pack_app.py` |

**为什么要"宁可报错"**：RAM 基址一变，镜像里成百上千个绝对地址同时失效。换一个地址把它
启动起来，只会得到一次看不懂的 MemManage；报错说清楚"要重装"，是唯一诚实的答案。

## 4. 验证

### 4.1 正向：App 在非特权态跑完整自检

```
app install /APP_DEMO.svcapp      ->  app install: ok, slot 2
APP_TEST summary: pass=78 fail=0
APP_ALIVE t=... cpu=0% ...        （持续 110 s 无复位）
```

覆盖设备 IO、任务/延时、信号量/互斥量/事件、消息队列、软件定时器、时间基、CPU 占用、
故障计数、槽位状态、日志、shell 注册、**FPU**、POSIX/Win 兼容层、文件系统。
全部通过，说明非特权态下 SVC 通道、共享窗口、FPU 使用都没有被 MPU 误伤。

### 4.2 反向：两个故意越权的镜像

`example/stm32f427/app_sdk/APP_BAD`（非示例，是探针）：

| 探针 | 行为 | 期望 |
|---|---|---|
| probe1 | 写内核私有 RAM 的字（`0x20003000`，不在本 App 窗口内） | MemManage |
| probe2 | 非特权态写 `SCB->SHCSR`（`0xE000ED24`） | BusFault / HardFault |

设备 `fault` 读数：

```
#   tick      task  type
 0   134746    7     MEMFAULT    <- probe1
 1   134746    7     RECOVER
 2   190824    8     BUSFAULT    <- probe2
 3   190824    8     RECOVER
```

同时：

- 探针后面的 `"NOT blocked"` 分支**一次都没打印** → 两次越权都被拦下了
- 两个探针槽（4、5）`crash=1` 且仍然 `RUNNING` → 内核按终局策略**恢复任务**而不是整机崩
- 另有一个把探针一路撞下去的实例（槽 3）`crash=3`、`INVALID`、`held=crash-limit` → **连续越权会被禁用**，正是想要的终局
- 期间 shell 一直可敲（`app list` / `fault` 正常应答），槽 2 的 APP_DEMO 继续心跳 → **整机不受影响**

## 5. 已知边界（**没有**解决的，别当成已解决）

1. **只约束非特权任务。** 内核任务（含驱动框架里以特权态跑的代码）不受 MPU 限制；这是设计，不是遗漏。
2. **共享窗口对 App 仍可写。** 共享分区表与崩溃日志同处一个窗口，把该窗口改成只读会挡住内核自己的写路径，所以保持可写。App 理论上能改分区表字段（当前不作为安全边界）。
3. **App 注册的 shell 回调在特权上下文执行 App 代码。** shell 在内核任务里跑，回调里的 App 代码因此拿到特权；要修需要一个 SVC 蹦床 + 非特权栈，成本不低，暂未做。
4. **`arkrtos_loader_accept` 不校验负载 CRC（只校验头）**，`signature[64]` 也没有验签 —— 按既定裁决留给后续（当前只做完整性：版本单调 + image_id + CRC）。
5. **故障环在复位后清空**（在 RAM）。要看"刚发生那一刻"的记录，必须在复位前读；跨复位的崩溃计数由 `arkrtos_crash_journal`（`.noinit` 区）承担。

## 6. 复现步骤

```
# 1. 编译并烧录内核（F427）
UV4 -r example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx
UV4 -f example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx

# 2. 打包并上传 App
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/APP_DEMO/MDK-ARM/app_demo.uvprojx \
      --type app --name APP_DEMO --out build/APP_DEMO/APP_DEMO.svcapp
py -3 tools/fs_put.py build/APP_DEMO/APP_DEMO.svcapp --port COM3 --path /APP_DEMO.svcapp

# 3. 在 shell 里
fs mount
app install /APP_DEMO.svcapp     # 期望 pass=78 fail=0

# 4. 反向验证（可选）
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/APP_BAD/MDK-ARM/app_bad.uvprojx \
      --type app --name APP_BAD --out build/APP_BAD/APP_BAD.svcapp
py -3 tools/fs_put.py build/APP_BAD/APP_BAD.svcapp --port COM3 --path /APP_BAD.svcapp
# app install /APP_BAD.svcapp ; fault ; app list
```

> 取证提示：长时内存转储前先写 `DBGMCU_APB1_FZ` bit12（`DBG_IWDG_STOP`）冻结独立看门狗，
> 否则转储本身就会把现场复位掉；抓"出错指令"用 `DEMCR` bit4（`VC_MMERR`）做向量捕获，
> 故障处理是微秒级的，halt/read/resume 轮询必然错过。

## 7. 两种隔离后端：窗口 vs 页表（2026-11-16）

> 分档：本节是 **QEMU 实跑**（LA64 跑真内核 / 真 `utest` / 真 `sec`，RV32 跑回归），
> **不是上板**。§1–§6 的上板结论一个字都没有被本节改写或覆盖。

### 7.1 为什么要分成两个后端

「让 App 进不了内核」这件事，在不同芯片上落到**完全不同的硬件部件**上：

| 后端 | 开关 | 靠什么隔离 | 适用核 | 实现文件 |
|---|---|---|---|---|
| 窗口 | `ARKRTOS_USE_MPU=1` | ARM MPU / RISC-V PMP 的**物理窗口寄存器**：按基址+大小划几段，越出即 fault | Cortex-M3/M4/M7、RV32IMAC（PMP） | `kernelsrc/src/arkrtos_mpu.c` |
| 页表 | `ARKRTOS_USE_PAGING=1` | **TLB 映射 + 特权级**（LoongArch PLV0 内核 / PLV3 用户）：地址先过映射，再过权限 | LoongArch LA32/LA64；将来 RV64 port | `kernelsrc/src/arkrtos_paging.c` |

前者是 MCU 的典型形态，后者是带 MMU 的芯片的典型形态。**它们不是两套 API**，而是
同一套契约的两个实现。

### 7.2 一套契约，两个实现（名字不改）

`kernelsrc/include/arkrtos_mpu.h` 里的五个入口就是这套契约：

```c
arkrtos_mpu_module_init();      /* 初始化隔离硬件 */
arkrtos_mpu_build_task(tcb);    /* 给这个任务套上隔离上下文 */
arkrtos_mpu_set_app();          /* 切到 App 侧 */
arkrtos_mpu_set_idle();         /* 切回内核侧 */
arkrtos_mpu_reset();            /* 复位 */
```

名字里的 `mpu` 是历史写法。全仓有 **8 个调用点**，其中一个落在 GBK 编码的
`arkrtos_task.c` 里——改名字的代价是把一个 GBK 文件拖进转码风险，收益只是好看。
所以名字保留，语义按「给这个任务套上它的隔离上下文」读。两个 TU 由互斥宏保证
符号不冲突：`USE_PAGING=1` 时 `arkrtos_mpu.c` 编出空目标，反之 `arkrtos_paging.c`
整个文件在 `#if` 内、编出空 TU。

### 7.3 三条守卫，全是 `#error`，没有静默降级

写在 `kernelsrc/include/arkrtos_config.h`：

| 条件 | 报错原文（节选） |
|---|---|
| `USE_PAGING=1` 但本核心 `ARKRTOS_ARCH_HAS_PAGING_HW != 1` | `ARKRTOS_USE_PAGING=1 但本核心没有页表/TLB（见 arkrtos_arch.h 的 HAS_PAGING_HW）` |
| 两个后端同时为 1 | `ARKRTOS_USE_PAGING=1 与 ARKRTOS_USE_MPU=1 互斥：同一时刻只能有一个隔离后端` |
| `USE_PAGING=1` 但 `USE_PRIV != 1` | `ARKRTOS_USE_PAGING=1 需要 ARKRTOS_USE_PRIV=1：页表隔离靠特权级（PLV/U 态）落地` |

为什么不用「自动挑一个」：这三条里任何一条被违反，得到的都不是「次优配置」而是
**一个自称有隔离、实际没有的二进制**。报错是唯一诚实的答案。

顺便把两个容易混的符号分开：

| 符号 | 是什么 | 谁读 |
|---|---|---|
| `ARKRTOS_ARCH_HAS_PAGING_HW` | **硬件事实**：这颗核有没有 TLB/页表。per-core，在 `arkrtos_arch.h` 里按核心定义（M3/M4/M7/RV32IMAC=0，RV64GC/LA32/LA64=1） | 只被 `arkrtos_config.h` 的守卫读 |
| `ARKRTOS_USE_PAGING` | **本构建用不用**分页后端。Kconfig 符号，默认 0 | `arkrtos_mpu.h`、`arkrtos_paging.c`、port 层、`arkrtos_sec.c` |

RV64GC 的硬件位是 1，但本仓还没有 RV64 port、也没有页表初始化，`USE_PAGING` 仍是 0。
`sec` 的 `iso=` 只读后者；读硬件位会让一个根本没打开分页的构建报 `iso=2`——假绿。

### 7.4 LA64 上真做到的事：静态映射 + 装载期跨度审计

**映射表是静态的**，在 `arkrtos_port_irq_init()` 里由 `la_mmu_init()` 一次建好，
之后不再变（`kernelsrc/port/loongarch/la64/arkrtos_port.c`）：

| 跨度 | 大小 / 页大小 | 权限 |
|---|---|---|
| 内核低 16 MB | 16 MB，PS=22 ×2 | PLV0，可读写执行 |
| 内核 UART | 4 KB | PLV0 |
| App 区 `0x02000000` | 2 MB，PS=20 | **PLV3**，RWX |
| App UART 别名 `0x02202000` | 4 KB | **PLV3**，读写 |

`ASID` 固定 0；**任务切换不换页表**，只改 `s_next_pplv`（`arkrtos_port_set_thread_priv`），
特权级由异常帧在返回用户态时落地。所以 `set_app` / `set_idle` / `reset` 在 paging
后端里是空实现——它们要做的「换地址空间」这件事，本后端没有做。

**那 paging 后端的真实增量是什么？**是**装载期跨度审计**：
`arkrtos_mpu_build_task()` 里对每个任务的 ROM 区间与 RAM 区间调
`arkrtos_port_paging_check(base, size, mem)`，要求整段落在**单条**已映射跨度内
且属性位对得上；不满足就记首个 `bad_base` 并把 `bad` 计数加一。

```c
int32 arkrtos_paging_stats(uint32 *p_checked, uint32 *p_bad, uint32 *p_bad_base);
```

语义写死三条，都是为了不给出「看似权威的错答案」：

1. **只审计，不修复**：发现越界只计数，不猜一个更宽的跨度把断言放过去；
2. **`size == 0` 跳过**：空区间不是越界；
3. **`arkrtos_port_paging_alias()` 恒返回 -1**：不假装支持运行期动态映射。
   `flush()` 是空操作：真发 `invtlb` 会把内核自己的条目也打掉。

### 7.5 QEMU 实测读数

| 项 | LA64（`USE_PAGING=1`） | RV32（默认档，`USE_PAGING=0`） |
|---|---|---|
| 构建 | `make -f example/qemu_loongarch64/Makefile -j8` | `make -f example/qemu_riscv32/Makefile -j8` |
| 产物 | `ark_qemu_la64.elf` = **2335944 B** | `ark_qemu_rv32.elf` = **2012860 B（与改动前逐字节等量）** |
| `utest` | **13 matched, 13 passed, 0 failed** | **13 matched / 133 assertions / 0 failed** |
| `sec` 的 `gates.kernel` | `[PASS] … iso=2 sign/hwsafe=0x3` | `[PASS] … iso=1` |

读法：

- LA64 的 `iso=2` 说明**页表后端真的在跑**，不是「开关打开了、代码没接上」。
  这一位在改动前是 0（LA64 没有 MPU 实体，`USE_MPU=0`），bit0 一直是 FAIL——
  也就是说 LA64 在此之前**根本没有地址隔离后端**。
- RV32 的逐字节等量，是「默认档零变化」这条承诺的实测证据。**读数更新
  （2026-11-16）**：这份文档原先记的 `2012860 B` 是当时那一次构建的值；同一天在
  `127ce6d` 上重新构建得到 `2012876 B`（工具链/构建日期差异，与该提交的行为无关），
  本轮加入 `ARKRTOS_USE_ELF`（默认关）后为 `2012924 B`，`text` 只涨 12 B，来源是
  `arkrtos_task_kill_internal` 里那条诊断闸门 `arkrtos_port_in_isr()==0u`
  （修掉"推迟切换被误报成 switch did not take"），与 ELF 装载器无关
  （ELF 关档时那一整个 TU 是空目标，`exec` 命令也不注册）。判据仍是**实测到字节**。
  这个读数在本轮中途曾经涨过 144 B，根因是一个没人调用的 `arkrtos_iso_backend()`
  探测接口——删掉它就回来了。**零变化的判据必须实测到字节，不能靠读代码。**
- LA64 在 `USE_PAGING=0` 档的体积本轮**没有单独测量**（那一档在 LA64 上没有可用
  隔离后端，不是可发布配置），别把这个空位读成「已经验证过零变化」。

### 7.6 真做到的 vs 欠账（别把「能切换」读成「MMU 版做完了」）

| 能力 | 状态 |
|---|---|
| 两个后端可由 Kconfig 二选一，矛盾配置编译期拒绝 | **已做**（三条 `#error` + `tools/paging_gate.py` 门禁） |
| 静态映射（内核 / App / UART）与 PLV3 用户态 | **已做**（LA64 port，QEMU 实跑） |
| 装载期跨度审计 + 计数读数（`arkrtos_paging_stats`） | **已做** |
| `sec` 报 `iso=2`，与窗口后端 `iso=1` 区分 | **已做** |
| App 在非特权态跑完整自检 | **已做**（LA64 QEMU `utest` 13/13） |
| 把**可执行文件**装进用户窗口直接跑（`exec` / 敲路径，Linux 入口约定） | **已做 M1**（`ARKRTOS_USE_ELF=1`，单窗口 + 单实例；见 [MMU进程模型与ELF装载](MMU进程模型与ELF装载.md)）。它是"文件即程序"，**不是**进程 |
| 每 App 一份页表 / 同 VA 多实例 | **未做**（当前单页表、ASID=0） |
| 页帧分配器 | **未做** |
| 缺页处理 | **未做** |
| `copy_from_user` / `copy_to_user`（内核安全访问用户地址） | **未做** |
| 运行期动态映射（`arkrtos_port_paging_alias`） | **未做**，接口恒返回 -1 |
| SMP（多核 TLB shootdown） | **未做** |

验收判据（做这些欠账时按这条验收，不按「能跑起来」验收）：

1. 两个 App 各自一份页表，**同一 VA** 在两个 App 里指向不同物理页，互不干扰；
2. 反向三探针（写内核 RAM / 写别的 App RAM / 跳到未映射地址）**全拦**，且各有独立 fault 读数；
3. 内核保持静态直映射，不被用户态页表影响；
4. `copy_from_user` 对非法用户指针返回错误码，**不 fault 内核**。

### 7.7 复现

```bash
# LA64（分页后端，iso=2）
make -f example/qemu_loongarch64/Makefile -j8
cd build/qemu_loongarch64 && (sleep 6; printf 'utest\n'; sleep 22) | \
  timeout 36 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
  -machine virt -kernel ark_qemu_la64.elf -nographic

# RV32（默认档，MPU 后端，iso=1）
make -f example/qemu_riscv32/Makefile -j8
cd build/qemu_riscv32 && (sleep 6; printf 'utest\n'; sleep 14) | \
  timeout 24 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-riscv32.exe" \
  -machine virt -cpu rv32 -bios none -kernel ark_qemu_rv32.elf -nographic

# 只跑不放命令（看开机日志）：直接用 Makefile 的 run，把串口落到 out.txt
make -f example/qemu_riscv32/Makefile run

# 门禁：默认档=0 / LA64 打开编得过 / RV32 打开必须被拒 / 两后端互斥必须被拒
python tools/paging_gate.py
```

> QEMU 参数别自己编：`-cpu loongarch64` 不存在（会报 `unable to find CPU model`），
> RV32 少了 `-cpu rv32 -bios none` 会撞 opensbi 区域重叠。上面的 `-machine` / `-cpu` /
> `-bios` / `-kernel` 与各自 `example/*/Makefile` 的 `run:` 目标同参数，差别只有一处：
> `run:` 用 `-serial file:out.txt`，上面为了把 shell 命令喂进去换成了 stdio 管道。
> `sleep` 的时机只是经验值，以 `utest` 提示符真出现为准——喂早了命令会被丢掉。
