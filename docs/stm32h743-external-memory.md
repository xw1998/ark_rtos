# STM32H743 板上外置存储器（SDRAM / QSPI NOR）：接起来了，但这块板子的窗口不应答

**这份文档回答一个问题：板子上明明焊着 32MB 的 SDRAM 和 8MB 的 NOR Flash，
为什么当初"空间不够"？现在控制器配好了，为什么还是用不上？**

答案分两半，两半都有板上读数支撑：

1. 当初的"空间不够"**跟外部器件无关**，全部来自片内预算与几个软件常量。
   这一段已经解决（大块工作区搬出片内 RAM 的机制已经落地）。
2. 但搬过去以后发现：**本板 FMC 的存储窗口对任何访问都不应答**，
   而它的寄存器、命令引擎、引脚驱动全部正常。这是硬件层面的负读数，
   软件改不动，本文把它钉死在证据上。

最后给出"要继续查该量哪几个点"的清单，以及本次踩到的四个坑。

---

## 1. 板上事实（原理图核实，不是推测）

原理图：`broad_example/example/FK743M5-XIH6原理图.pdf`。用 `pdftotext -raw` 取文字层读网络标号
（图像 OCR 会把整行认错位，见第 6 节第 4 条）。

| 器件 | 型号 | 容量 | 挂在哪 |
|---|---|---|---|
| SDRAM | W9825G6KH-6（IC3） | 32 MB = 4 bank × 8M × 16bit | FMC SDRAM Bank1（SDNE0/SDCKE0）⇒ `0xC0000000` |
| QSPI NOR | W25Q64 | 8 MB | QUADSPI ⇒ `0x90000000` |

MCU：STM32H743XIH6，2 MB Flash、512 KB AXI SRAM（`0x24000000`）、128 KB DTCM（`0x20000000`）。

SDRAM 引脚（全部 AF12）：D0 PD14、D1 PD15、D2 PD0、D3 PD1、D4 PE7…D12 PE15、
D13 PD8、D14 PD9、D15 PD10；A0 PF0…A5 PF5、A6 PF12、A7 PF13、A8 PF14、A9 PF15、
A10 PG0、A11 PG1、A12 PG2；BA0 PG4、BA1 PG5；NBL0 PE0、NBL1 PE1；
SDNWE PC0、SDNRAS PF11、SDNCAS PG15、SDCLK PG8、**SDNE0 PH3、SDCKE0 PH2**。

这些事实只在 `config/stm32h743/arkrtos_partition.h` 里写一次（`CHIP_SDRAM_*`），
`tools/gen_ld.py` 与驱动都从那里派生——地址不允许在第二个地方出现。

---

## 2. 当初"空间不够"是四件事，没有一件是外部器件

| 现象 | 真实来源 |
|---|---|
| `RAM 439284 B (97.50%)` | 片内 AXI SRAM 里装着 arkc 的 `.ark_buf`（178 KB）、ramfs 的定长文件缓冲、内核 .bss |
| `KERNEL_SIZE 512K` | arkc 的 text 约 139 KB，内核区只能按 2 的幂跳到 512 KB |
| `ARK_RAMFS_FILE_CAP / MAX_FILES` 卡住大文件 | 两个都是**软件常量**（每文件一段定长缓冲） |
| `.bss` 里还有 `g_ld_syms/g_ld_relocs[512]` 各 0x11000 | 链接器符号表与重定位表，写死在 .bss |

所以"板子有 SDRAM 却报空间不够"是成立的：那时 SDRAM 一个驱动都没有，
编译器当然只能挤片内。

---

## 3. 已经落地的东西（默认关闭＝行为与体积零变化）

| 开关 | 位置 | 默认 | 作用 |
|---|---|---|---|
| `ARKRTOS_USE_SDRAM` | `kernelsrc/include/arkrtos_features.h` | 0 | 编进 FMC/SDRAM 驱动、把 `.ark_buf` 与 ramfs 文件缓冲放到外置分区 |
| `ARKRTOS_SHELL_BOARD_CMDS` | 同上 | 0 | 编进板级 shell 命令（`sdram`）；由 `USE_SDRAM` 决定 |
| `USE_SDRAM`（make 变量） | `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | **0** | 一次决定四个东西：上面的 `-D`、编译器的缓冲预算、ramfs 槽数与段归属、`gen_ld.py --ext-ram` |
| `ARKRTOS_SDRAM_VERIFY_AT_BOOT` | `example/stm32h743/kernel/ARKRTOS_TEST/Core/Src/main.c` | 0 | 上电是否真的去摸一次窗口（摸了就可能触发总线故障，所以生产档必须关） |

`make USE_SDRAM=1` 时：`.ark_buf` → `SDRAMBUF`（`0xC0000000` 起 12 MB）、
ramfs 文件缓冲（`.extram`）→ `SDRAMFS`（`0xC0C00000` 起 6 MB），
编译预算抬到池 4 MB / 源码 512 KB / 汇编 2 MB / 代码 1 MB，ramfs 8 槽 × 512 KB。

`make`（默认档，`USE_SDRAM=0`）时：`gen_ld.py --ext-ram 0` 把两个分区归零，
`.ark_buf` 与 `.extram` 回到片内 RAM，**链接脚本与没有外置 RAM 的板逐字节同构**，
RAM 占用回到 439284 B / 97.50%。

> **为什么要新加 `--ext-ram` 这个参数**：分区头描述的是"板上有这两片器件"这个
> 物理事实，它是无条件存在的；但"这次构建用不用"是板级决定。最初版本直接按
> 分区头生成 `SDRAMBUF/SDRAMFS`，于是 `USE_SDRAM=0` 时 `.ark_buf` 仍被放进
> `0xC0000000`——片内 RAM 空出 232 KB、编译器一开池就取总线故障。这个坑是
> 本轮把默认档改回 0 时才暴露的，记在第 6 节。

### 3.1 三态语义（`sdram` 的状态机）

```
DOWN ──init──> CONFIGURED ──verify──> VERIFIED
```

- `CONFIGURED`：寄存器写完了，但**没有一个字节被证明能存取**。
- `VERIFIED`：一个 word 写进去又读回来了，才允许把数据放在那里。
- 板级 init 只做纯寄存器动作（不碰窗口），所有"摸窗口"的动作放在 shell 里；
  失败以 `PANIC BUS` + 地址暴露，而不是静默返回错误值。
- VFS 的门控：`arkrtos_sdram_ready() != 1` 时跳过挂载，因为 ramfs 的文件缓冲
  就住在那块内存里——宁可没有 VFS，也不要把文件写进不存在的内存。

### 3.2 `sdram` 命令

```
sdram info        几何/容量/时钟/是否 verified
sdram init        重跑整条上电序列（只碰寄存器，不会故障）
sdram verify      写一个 word 再读回（会碰窗口，失败就是总线故障）
sdram probe       只摸窗口最末一个 word
sdram test        整片图案自检（32 MB 全覆盖）
sdram fsm         命令引擎仪器：SELF_REFRESH → NORMAL，看 MODES1 是否跟着走
sdram pins        引脚电平 / MODER / SDCLK 是否在翻转
sdram clk <0..3>  FMCSEL：0=HCLK3 1=PLL1Q 2=PLL2R 3=PER_CK
sdram regs        SDCR1/SDTR1/SDRCMR/SDRTR/SDSR 原始值
```

---

## 4. 本轮读数：窗口不应答（负读数）

### 4.1 配好了一半，而且是可以自证的那一半

调试器（CPU 停在断点，读的是寄存器不是窗口）：

```
SDCR1 = 0x00002DD9   NC=9 列, NR=13 行, MWID=16bit, NB=4bank, CAS=3, SDCLK=/3, RPIPE=1
SDTR1 = 0x01104351   TMRD/TXSR/TRAS/TRC/TWR/TRP/TRCD 由 W9825G6KH 的 ns 值与实际 SDCLK 算出
SDRTR = 0x000004B8   refresh count 按 64 ms / 8192 行 算
SDSR  = 0
AHB3ENR = 0x1000     FMCEN 已置
D1CFGR  = 0x48 / D1CCIPR = 0   HCLK3 = 240 MHz ⇒ SDCLK = 80 MHz
```

板上 `sdram info` 打印同口径：`configured: 32 MB @0xC0000000, HCLK3 240 MHz, SDCLK 80 MHz`。

三条独立证据说明 FMC 的这一半是活的：

1. **寄存器可写可回读**：SDCR1 写进去读回来一致。
2. **命令引擎在跑**：`sdram fsm` 打出 `MODES1 0 -> 1 -> 0`，即 SELF_REFRESH 与
   NORMAL 两条命令都被执行、SDSR 的 MODES1 跟着变。
3. **引脚在被驱动**：`sdram pins` 实测 SDCKE0 PH2、SDNE0 PH3、SDNRAS PF11、
   SDNCAS PG15、SDNWE PC0 都在 AF 且电平为 1；SDCLK PG8 在 512 次采样里变过。

### 4.2 但窗口一个字节都不应答

CPU 侧（`sdram verify`，probe 改成"先读一次"之后）：

```
*** PANIC BUS ***
CFSR   = 0x00008200   BFARVALID | PRECISERR
BFAR   = 0xC1FFFFFC   正是 32MB 窗口的最后一个 word
MPUCTRL= 0x00000005   （ENABLE|PRIVDEFENA；MMARVALID=0、DACCVIOL=0 ⇒ 不是 MPU 犯规）
```

`BFARVALID|PRECISERR` 是"总线给出了错误应答，并且地址是精确的那一个"。
这条读数只能这样产生：CPU 发出一次 AXI 读，被拒绝。

调试器侧（AHB-AP，绕开 CPU）：`0x60000000`、`0xC0000000`、`0xC1FFFFFC`
在 8/16/32 位读、32 位写下**全部** `Failed to read memory`。

### 4.3 排除项（每一条都有一次实验）

| 假设 | 实验 | 结果 |
|---|---|---|
| SDRAM 时序/几何算错 | 逐字段核对 SDCR1/SDTR1，与 HAL 的 `FMC_SDRAM_Init` 编码一致 | 排除（且时序错不可能产生总线错误，只会读回乱数据） |
| SDCLK 频率不对 | `sdram clk 0/1/2/3` 换 FMCSEL，四种时钟源都试 | 排除 |
| MPU/缓存属性 | MPU_CTRL、CFSR 的 MMARVALID/DACCVIOL 位；调试器路径不经 MPU | 排除 |
| 我们的时钟树/固件 | **复位态裸测**：不跑固件，HSI 64 MHz，只置 FMCEN 后配 bank1 | 仍然报错 ⇒ 排除 |
| bank 没开（MBKEN/SDNE） | H7 的 SDCR **没有 SDNE 位**；把 BCR1 的 MBKEN 置 1（异步 SRAM 口）后读 `0x60000000` | 仍然报错 ⇒ 排除 |
| 数据宽度 / bank 选择 | 8/16/32 位读 × 写 × bank1/bank2 矩阵 | 全部失败 |
| 需要外部器件应答 | bank1 配成**纯异步 16-bit SRAM**（MBKEN=1, MTYP=00, MWID=01, FACCEN=1, BTR1=0xFFFFFFFF），这种 bank 不需要任何外部器件参与就能读 | 仍然报错 ⇒ 与 SDRAM 器件、几何、时序彻底无关 |
| 是"FMC 拒绝了事务"还是"没人应答" | 对照未实现的外设地址 `0x52005000`/`0x5200A000`（返回 0，不报错）与 `0x60000000`（报错） | 报错形态与"AXI 上根本没挂 slave"同型（DECERR），不是 FMC 的 SLVERR |

两条主设备（CM7 的 AXI 口、调试器的 MEM-AP）、两种状态（复位态、固件运行态）、
四种位宽、两个 bank、两种 bank 类型——全部同一结果。

### 4.4 因此：剩下的解释类别只有一种

寄存器与命令引擎在同一颗硅片上、同一个时钟域里都正常，唯独它的**外部存储
窗口**对任何访问都不应答。这不是软件能改的层次：

- 器件/封装级缺陷（FMC 的 AXI 从端口没响应），或
- 非原厂 die（IDCODE 可被仿冒），或
- 这块板的 MCU 这一路功能本身坏了。

芯片身份读数（调试器，复位态）：

```
DBGMCU_IDCODE = 0x20036450   DEV_ID=0x450（STM32H74x/75x）、REV_ID=0x2003（rev V）
FLASHSIZE     = 0x00000800   2048 KB
UID           = 0x0031003C 0x30335105 0x34393535
```

`DEV_ID/REV_ID/FLASHSIZE` 与正品 H743XI rev V 一致，时钟也确实跑在 rev V 才有
的 480 MHz 系统时钟上——**所以不能据此断定是仿冒片，只能说读数不矛盾**。

> **本节不写的结论**：不写"SDRAM 不可用是软件问题"，也不写"已经修好"。
> 现在能负责的说法只有一句：**软件侧已经把能证明的都证明了，窗口不应答是
> 硬件层面的负读数。**

---

## 5. 要继续查，该量哪几个点（给动手的人）

按"便宜的排前面"：

1. 换一块同型号板，同一份 `make USE_SDRAM=1` 固件跑 `sdram verify`。
   过了就是这块板的 MCU 坏了；不过就是这批片子/这份工程的问题。
2. 万用表量 IC3 的 VDD/VDDQ 是否有电、`SDNE0`(PH3)/`SDCKE0`(PH2) 是否到芯片引脚。
   （注意：**SDRAM 不应答不会产生 DECERR**，因为 FMC 不校验从器件的存在性，
   采样到的就是悬空电平。所以这两条只能解释"读回乱数据"，解释不了"总线错误"，
   但仍值得排除焊接/供电。）
3. 换一颗 MCU 试（如果是打样板）。
4. QUADSPI 那条路（`0x90000000`，`QSPIEN` 已可置位）同样报错，可作为
   "外置存储窗口这一类是否整体失效"的旁证；顺带这也是 M2 的 NOR 驱动入口。

---

## 6. 本次踩到的四个坑（都可复用）

1. **`0x52004040` 不是 SDCR1**。SDRAM 寄存器在 `0x52004140`
   （`FMC_Bank5_6_R` 偏移 0x140），`0x52004040` 是 NOR 的 `BWTR1`。
   读它得到全 0，曾经被误判成"控制器没配"。地址来源应该是 CMSIS 头里
   `FMC_Bank5_6_R->SDCR[2]` 的注释，不是记忆。
2. **H7 的 `SDSR` 没有 BUSY 位**。用它做"命令有没有被执行"的仪器，永远采不到
   真值，于是"命令被丢弃"这个结论其实是仪器的盲区。可观测的量是
   `SDSR.MODES1`：SELF_REFRESH(01) ↔ NORMAL(00) 能证明命令引擎真的在跑。
   **仪器要选这颗芯片上真的存在的东西。**
3. **调试器的非对齐读会静默返回 0**。`read_memory <未按宽度对齐的地址> 32 1`
   在本机这份 OpenOCD + CMSIS-DAP 上，对**未映射**的地址也返回 `0x00000000`
   而不是报错。曾经因此以为"FMC 窗口在 +2 处应答了"。判定方法：拿一个确定
   没人应答的地址（`0xE0100002`、`0xA0000002`）做对照，它也返回 0 ⇒ 读数无效。
4. **读矢量 PDF 原理图要按文字层坐标配对**。用图像 OCR（view_image）读网络标号
   会把整行错位，本轮之前就把 PB12/PB13 认反过。`pdftotext -raw` 之后按坐标
   对齐再配对，才敢写进地址表。

另外两条与本轮直接相关的实现事实：

5. `SystemInit`（`Core/Src/system_stm32h7xx.c`）会主动把
   `FMC_Bank1_R->BTCR[0] = 0x000030D2`（MBKEN=0）写下去禁 bank1，
   并临时开关 FMCEN。所以"复位后 `BCR1=0x30D2`"不是谁配错了，是启动代码干的。
6. H7 的 `SDCR` 里没有 `SDNE` 位；bit15 写不进去（写 0xADD9 读回 0x2DD9），
   `RPIPE=2` 能写进。所以"用 SDNE 位打开 bank"这条路在这颗芯片上不存在。
7. **`make` 不跟踪 `-D`**。在同一个 build 目录里把 `USE_SDRAM=0` 换成 `1`
   只重链接不重编，会得到一个"半片内半片外"的镜像（实测：`USE_SDRAM=1`
   却仍报 RAM 44.77%、SDRAMBUF 232 KB）。改开关后必须 `make clean`。
   判断方法是看镜像大小与 RAM 百分比是否跟着变，不看命令行里有没有那个 `-D`。

---

## 7. 结论

- **"空间不够"与外部器件无关**，是片内预算 + 软件常量；这一点已解决。
- **把大块工作区搬到 SDRAM 的机制已经落地并默认关闭**：
  `ARKRTOS_USE_SDRAM` / `USE_SDRAM` / `--ext-ram` 三处对齐，默认档零变化。
- **本板 FMC 的外部存储窗口不应答**（DECERR 型），已被两条主设备、
  两种状态、四种位宽、两个 bank 反复确认；FMC 的寄存器、命令引擎、引脚驱动
  全部正常。**这是硬件层面的负读数，软件无法绕过。**
- 在换板或换片复测之前，本板的正确配置是 `USE_SDRAM=0`（默认档）：
  编译器回到片内预算档，ramfs 回到 4 槽 16 KB，VFS 正常挂载。
