# arkc 产物上板 — 错误码表、尺寸账本与板级回归

本文是 [arkc编译产物上板运行-实施计划.md](arkc编译产物上板运行-实施计划.md) 里 **M4（收口）** 的落地记录，
也是 M1（[固定窗口](arkc板上直跑-M1实机记录.md)）与 M2（[可重定位](arkc板上直跑-M2可重定位镜像.md)）
两份记录的索引与对账页：

- 一次失败到底该看哪个码（§2、§3）；
- 编进去多少（§4）；
- 怎么在一条命令里复跑这些结论（§5）。

日期：2026-11-05。读数全部来自真机（COM3 / 115200）与干净构建。

---

## 1. 为什么要有这一页

M1/M2 通路上同一个「跑不起来」有五种完全不同的原因：镜像不是给这个窗口链的、重定位表有一条
位置解释不了、CRC 对不上、池里那块地拿不到、任务表满了。它们的**修法互不相同**，
所以码必须分开——一条退化成 `-1` 的错误码等于把「查什么地方」这件事也一起丢了。

同样地，「跑起来了」不能当作结论：串口上出现一行像样的输出，也可能是上一份镜像的残留、
也可能是程序在错误的落点上碰巧读了不越界的地址。所以 M1/M2 的判据必须是
**源码独有字符串 + 程序自己量出的落点号 + 退出后块归还**，并且这三条要能自动比对（§5）。

---

## 2. 三条装载路径的错误码（同一张表，按路径分开读）

各路径用同一套 `ARKRTOS_LOADER_ERR_x`（`kernelsrc/include/arkrtos_loader.h`），
只有本路径独有的情况才另开码。**跨路径有两处编号重叠**，这是故意的（同一个数字在不同
路径里语义不同），代价是报错文案必须把路径名带上，否则人会看错。

| 码 | `loader`（安装/App） | `mini`（文件里的负载） | `apprun`（固定窗口直跑） | `flashapp`（烧在 Flash 里跑） | `apppack`（打包期） |
|----|----|----|----|----|----|
| -1 | PARAM 参数非法 | 同 loader | — | 同 loader | PARAM 指针/窗口/长度非法 |
| -2 | MAGIC 镜像头魔数错 | 同 loader | — | 同 loader | — |
| -3 | COMPAT 硬件签名不符 | 同 loader | — | 同 loader | — |
| -4 | SIZE 长度非法/超空间 | 同 loader | — | 同 loader | — |
| -5 | CRC32 失败 | 同 loader | — | 同 loader | — |
| -6 | FLASH 擦写失败 | — | — | — | — |
| -7 | NO_SLOT 无空闲槽 | — | — | — | — |
| -8 | TASK 任务注册失败 | 同 loader | — | 同 loader | — |
| -9 | STATE 槽位状态不允许 | 同 loader | — | — | — |
| -10 | ADDR 落点非法 | 同 loader | — | — | — |
| -11 | ENTRY 入口偏移越界 | 同 loader | — | — | — |
| -12 | OCCUPIED 区间重叠 | 同 loader | — | — | — |
| -13 | RELOC 表非法/补丁后不一致 | 同 loader | — | — | — |
| -14 | NOSPACE 池内无足够空间 | 同 loader | — | — | — |
| -15 | BUSY 镜像在跑 | BUSY 实例数已满 / 有一次装载在进行 | — | — | — |
| -16 | VERSION 版本回灌 | — | — | — | — |
| -17 | DUP 重复副本 | — | — | — | — |
| -18 | SIGN 签名失败 | **FS：读镜像失败**（原因另见 `read: %d`） | — | — | — |
| -19 | — | DISABLED 崩溃到上限已禁用 | — | — | — |
| -20 | — | **SHORT_READ 读到文件尾** | — | **BUSY 已有一个在跑** | — |
| -21 | — | — | — | WINDOW 池里没拿到这块地 | — |
| -22 | — | — | — | PROLOGUE 首半字不是 arkc 序言 | — |
| -23 | — | — | — | LAYOUT 镜像不是给这个窗口链的 | — |
| -30..-34 | — | — | — | — | PARAM / DIFF / VERIFY / SIZE / ENTRY |
| -40 | — | — | — | **BUSY 已有一个直跑/写入在进行** | — |
| -41 | — | — | — | WINDOW 数据窗口拿不到（被 app run 占 / 池表满） | — |
| -42 | — | — | — | LAYOUT 产物不是给这个 Flash 区链的（SP/入口越界） | — |
| -43 | — | — | — | PROLOGUE 入口首半字不是 arkc 序言 | — |
| -44 | — | — | — | META 区内没有有效产物（未烧 / 已擦 / 元数据坏） | — |
| -45 | — | — | — | VERIFY 回读比对不符（写进去的和文件不一样） | — |
| -46 | — | — | — | FLASH 底层擦/写失败 | — |

两处重叠：**-18**（loader 的签名失败 vs mini 的读失败）、**-20**（mini 的短读 vs apprun 的
已在跑）。所以 shell 的文案一律写成 `mini run: <path> failed, rc=-18` + `read: -1 (...)`，
不写裸数字。

---

## 3. 五种典型失败分别落在哪（计划 §M4 要求逐条对上）

| 现象 | 码 | 出处 | 实测证据 |
|------|----|------|----------|
| **链接表溢出**（符号/重定位超 arkc 的 `LD_MAX_*`） | arkc 自己的 `ld_table_overflow`，不走上面的码 | `kernelsrc/components/arkc/src/compiler.c` | M0 量过：单文件程序远低于 64 个符号；打包四遍时它会表现为某一遍 `cc: pass X aborted: ...`，而不是出一个坏镜像 |
| **打包四遍不一致**（某位置不是一个基址平移能解释的） | `-31 DIFF` | `arkrtos_apppack.c` | 宿主注入 `C_FILE=pD.bin` ⇒ `-31`；`DELTA` 与 D 用的不一致 ⇒ `-31` |
| **入口签名不符** | 打包期 `-34 ENTRY`；运行期 `-22 PROLOGUE` | 同上 / `arkrtos_apprun.c` | `-34` 的文案直接说「compile with --vector-table」；M1 用非 arkc 产物喂 `app run` ⇒ `-22` |
| **窗口被占** | M1 `-21 WINDOW`；mini `-15 BUSY` | `arkrtos_apprun.c` / `arkrtos_mini.c` | 本轮实测：mini 实例还占着 `0x24070000` 时，`app run` 报 `rc=-21 - the run window could not be taken`；`mini` 实例满两个时 `mini run` 报 `rc=-15 running 2 of 2 allowed` |
| **镜像超 cap** | `-33 SIZE`（表或输出缓冲装不下／表项超 `ARKRTOS_APP_RELOC_MAX`） | `arkrtos_apppack.c` | `cc --pack` 在打包前先查 `CC_PACK_OUT_CAP` 与窗口，超了直接拒绝，不截断 |

补充两条只在真机上才出现的：

- `cc --pack` 拿不到 32 KB + 16 KB 的池块时**拒绝借**而不是缩水：
  `cc: --pack needs 32 KB + 16 KB from the RAM pool; nothing free (the lend is refused, not shrunk)`。
  本轮实测触发条件是小程序实例把池切碎（两个实例各占 4 KB + 8 KB），**不是内存总量不够**。
- `arkc` 的池耗尽报 `[arkc] FATAL: memory pool exhausted` + `cc: pass A aborted: memory pool exhausted`，
  同时打印 `cc: pool <used>/81920 B`。本轮实测：809 B 的源在 pass A 就撞上，压到 569 B 四遍全过。

---

## 4. 尺寸账本（干净构建，`make clean` 后逐个量）

| `USE_ARKC` | `USE_APPRUN` | `USE_APPPACK` | `USE_MINI_VFS` | FLASH | RAM |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 0 | **190600 B** (36.35%) | 147036 B (32.63%) |
| 0 | 1 | 0 | 0 | 193292 B (36.87%) | 147068 B (32.64%) |
| 1 | 0 | 0 | 0 | **381808 B** (72.82%) | 439284 B (97.50%) |
| 1 | 1 | 0 | 0 | 385768 B (73.58%) | 439316 B (97.50%) |
| 1 | 0 | 1 | 1 | 388704 B (74.14%) | 439300 B (97.50%) |
| 1 | 1 | 1 | 1 | 391408 B (74.66%) | 439332 B (97.51%) |

读法：

- **arkc 是这一版真正的大户**：`USE_ARKC` 从 0 到 1，FLASH +191 KB、**RAM +292 KB**（编译池 + 工作区
  是静态的）。App 在板上编译这件事的代价基本就等于这一行，M1/M2 的开关都只是零头
  （apprun +3.9 KB、apppack +6.9 KB、mini VFS +2.9 KB FLASH）。
- RAM 已到 97.5%，这是本板（440 KB 内核窗）的上限附近：再加功能要动分区表，不是加个开关的事。
- 所有 M1/M2 开关默认关，且 0 档与各自的基线**字节数相同**（381808 B），这是「零变化」的判据。

---

## 5. 板级回归：`tools/final_app.py`

把 M1/M2 的判据做成一条命令跑完的用例。**每条只有 PASS / FAIL**，没有「看起来没问题」。

```sh
py -3 tools/final_app.py            # 烧 build/ARKRTOS_TEST.bin 再跑全部用例
py -3 tools/final_app.py --noflash  # 板子已有固件，直接跑
py -3 tools/final_app.py --list     # 列用例
py -3 tools/final_app.py --only m2-pack
```

前置：板子在 `--port`（默认 COM3）；固件按 `USE_ARKC=1 USE_APPRUN=1 USE_APPPACK=1 USE_MINI_VFS=1`
构建；`m2-two-landings` 还要求 `MINI_PRIORITY` 低于控制台（否则死循环的小程序把控制台饿死，
第二次 `mini run` 发不出去）。

| 用例 | 在证明什么 |
|------|------------|
| `board-clean`（前导） | 上一轮的残留小程序已停、池与窗口都空 |
| `m1-fixed-window` | 板上编出的 `.bin` 按固定窗口跑、打印源码独有字符串、退出后窗口归还、`fault` 里没有真正的异常 |
| `m1-wrong-window` | 把 `.sct` 换成别的基址 ⇒ 必须 `rc=-23`，且**不许**出现 `M1 alive`（反假绿） |
| `m2-pack-run` | 四遍差分打包成功（A/B/C/D 等长、四类表项计数都非零）、`mini run` 出字、`mini stop` 后块归还 |
| `m2-two-landings` | 同一份 `.svcm` 连跑两次，落点 `0x24070000` / `0x24071000`，程序自己量出的块号 `0` / `1` |

本轮实测：`all 4 cases PASS`。

### 5.1 脚本自己踩过的四个坑（都留成了注释，别改回去）

| 坑 | 症状 | 结论 |
|----|------|------|
| **锚行首** | 四条用例全 FAIL，日志里输出其实都在 | 内核日志带 `[  15.837] <6>` 前缀、程序输出常紧跟在提示符 `ark@arkrtos:~$ ` 之后 ⇒ 「源码独有字符串」只能锚**行尾**，靠行尾锚保证它不是别人的前缀 |
| **不先停小程序** | `app run` 报 `-21 窗口拿不到`；`cc --pack` 报 `32 KB + 16 KB 拿不到` | 小程序占的正是 `0x24070000`，还把 64 KB 池切碎 ⇒ 每个用例前先 `mini stop`（前导用例 `board-clean`） |
| **不清 `/tmp`** | `cc --pack` 报 `cannot write ...: I/O error` | ramfs 只有 3 个文件槽 ⇒ 每个用例开头统一 `rm` 掉全部已知文件名（`CLEAN`） |
| **断言 `fault` 为空** | `fault` 里出现 `TASKKILL` 条目 | **`mini stop` 会把它杀掉的每个任务记成一条 TASKKILL**（实测：清场两次后 `fault` 有 4 条）⇒ 判据改成「四种 CPU 异常名一个都不许出现」 |

这四个坑的共同点：**失败信息指向的地方，往往不是真正的原因**。所以脚本里凡是「为了跑通而
加的动作」（清场、停实例），都要能自己报 FAIL，而不是静默把状态糊过去。

---

## 6. 仍然没做的（如实记账）

| 项 | 现状 | 归谁 |
|----|------|------|
| M2 判据 C 的「跨文件调用」 | **仍未做，但阻塞已解除**：`-link` 已接线但没用过（要「两份源 + 两份目标 + 散列文件 + 产物」六份，ramfs 只有 3 槽）。M3-b 的 NOR 持久卷已经能用（见 `arkc板上直跑-M3b-QSPI持久化.md`），把六份文件摆在 `/mnt/nor` 上即可做 —— 现在只是**没做**，不是做不了 | 下一轮补，用例进 `tools/final_app.py` |
| 板上产物与宿主产物的 108 B 差异 | **已查明（2026-11-09）**：对照的不是同一优化档——板端 `cc` 默认 `-O1`，宿主 `arkcc.exe` 默认 `-O0`。把两边都钉在同一档后**逐字节相同**（§6.1） | 结论已落地，撤销「待查」 |
| M3-b（QSPI W25Q64 + littlefs 持久化） | **已完成（2026-11-05 实机）**：产物写进 NOR、复位后仍在、能被 `mini run` 装载执行；读侧改 QUADSPI 内存映射后 `ls /mnt/nor` 258 s → 0.249 s。**但**写/擦仍走位敲（3 KB 约 24.8 s）、开机仍要手动 `fs mount` + `mount /mnt/nor`、跨复位自启不成立 —— 见该文 §10 的七条限制 | 见 M3-b 文档；本轮只修账 |

## 6.1 「108 B 差异」的结论：不是编译器分歧，是对照档位不对

**现象**：同一份 `m1.c` + `m1.sct`，板上 `cc` 出 1444 B，宿主 `arkcc.exe` 出 1528 B，差 −84 B；
更早 M2 那次是 3006 vs 3114（−108 B）。两次都登记为「成因未查明」。

**取证路径**（前几轮走过弯路，这里写下来避免重犯）：

1. 不再猜 `.bin` 字节，改为抓 `cc -S` 的**汇编文本**（`cat` 对可打印 ASCII 无损），
   拿板端汇编与宿主各档汇编逐行对账。
2. 一对比就看见：「板上产物 = 宿主 `-O2` 产物去掉 DWARF 指令」——板上把 `i*4` 全常量折叠了，
   宿主旧对照里还留着 `MOV/PUSH/POP/MUL`。
3. 于是把宿主 `-O0/-O1/-O2` 三档全部重生成：**`-O0` 是 257 行 1528 B、`-O1` 与 `-O2` 都是 215 行 1444 B**。
   也就是说，之前那份被当成「宿主 -O1」的汇编其实是 **-O0**，`MUL` 消失的真正原因是档位差一档，
   不是板端偷偷抬档。
4. 直接读代码也没找到能抬档的地方：`config_init()` 把 `g_config.opt_level` 置 0；
   板端 `arkc_shell_cmd.c` 的 `cc_reset_state()`/`cc_pack_fill()` 都写 `opt_level`（`-O` 解析，缺省 1）；
   `compiler.c:2229` 取 `max(命令行档, g_config.opt_level)`；`config.c:237` 的 `-O` 解析只有 `>=2` 才顺带开
   inline/vectorize，没有任何一处把 1 改成 2。

**板上实测（回归档固件，`cc` 与 `arkcc.exe` 同源 `ca969c6`）**：

| 档位 | 宿主 `arkcc.exe` | 板上 `cc` | 结果 |
|------|-----------------|----------|------|
| `-O0` | 1528 B | 1528 B | 逐字节相同 |
| `-O1` | 1444 B | 1444 B | 逐字节相同 |
| `-O2` | 1444 B | 1444 B | 逐字节相同 |

`-O1`/`-O2` 在这份源上同尺寸是同一条事实：该文件只有一个函数、无可内联对象，
`-O2` 额外打开的 `opt_inline`/`opt_vectorize` 无事可做。

**复跑**：`python <工作区>/board_cmp_lvl.py`（三档全对账）、`board_cmp_o1.py`（单档）。
两者都用 `cat` 抓回字节、按 shell 转义规则解码，再与宿主产物逐字节比；
解码长度必须与 `stat` 报的 Size 相等，否则脚本自己判「通路丢字节、结论不成立」。

**仍然记着的一条限制**：`cat` 通路在数据里出现字面 `0x5C` 时会有歧义（脚本会把歧义位置打出来），
本次两档都是 `0` 处歧义；超出 3 个 ramfs 槽位的更大工程没有走这条对账路径。

---

## 6.2 板端没有 riscv32 后端（裁剪档）——RV 对象必须被拒，不能被当 ARM 补丁

STM32 profile（`ARKCC_PROFILE=STM32_MIN` / `CROSS_COMPILE`）把 `ARKCC_BACKEND_RISCV32`
置 0，所以板上这份 arkc **不编 `backend_riscv32.c`**：`ark_backend_find("riscv32")` 返回
NULL，`-target riscv32` 在板上根本不可达。

上游 0.9 给 riscv32 补了 `-c`/`-link`（对象体装机器码、跨对象 `auipc` 对与 `.word`
绝对地址两种重定位），`src/compiler.c` 因此多了一批只在宿主构建里存在的引用
（`rv_get_*` 九个 getter、`g_rv_obj_mode`、`rv_patch_reloc`）。这些**全部包在
`#if ARKCC_BACKEND_RISCV32` 里**——不包的话板上链接阶段会直接
`undefined reference to 'rv_get_symbol_count'` 一串，整个固件编不出来。

那条 `#else` 里写的不是空语句，而是明确报错：

```
this build has no riscv32 backend, so the riscv32 relocation at offset <n>
for '<sym>' cannot be applied; no image was written
```

因为「函数不存在」的正确后果是**拒绝这个对象**，而不是把它交给 ARM 的
`ld_patch_ref`——那会把 Thumb 分支编码写进 RISC-V 对象，然后 exit 0。同一个逻辑在
宿主侧的表现是 `__arkcc_isa_riscv32` 描述符的双向校验（见上游 README 的「RV32I 的对象
模式」）。

体积代价：这一轮（含上游 0.9 的 RV 对象模式与 ISA 校验）H743 干净构建
`text 412972 / data 2256 / bss 463532`，比上一版 `text 412180` 多 792 B，FLASH 占用
79.20%。板端能编出的目标只是 ARM，这些字节属于"永远走不到但必须存在的守卫"，不做
进一步裁剪（裁到只剩 `#error` 会让板端拿到 RV 对象时的行为与宿主不一致）。

**2026-11-08 追加（上游 `f1995fa`：RV 后端补齐内联汇编 / `__svc` / `__smc` / 打包车道运算，
并修掉 `opt_vectorize` 的静默错答案）**：本轮的板端体积**一位没变**——
`text 412972 / data 2256 / bss 463532`，这不是"没编进去"，是三条代码各自正好落在编译
边界外面，逐条核过：

| 上游改动 | 板端为什么不变 |
|---------|--------------|
| `backend_riscv32.c` 的五个新 case 与 `ecall`/`ebreak` | 整个文件不在 STM32 profile 里（`ARKCC_BACKEND_RISCV32=0`），一行都不编 |
| `opt.c` 的向量化窄化修复与 `volatile`/`__packed` 拒绝 | `opt_vectorize` 的唯一调用点在 `#if ARKCC_FEATURE_OPT_VECTORIZE` 里，板端该宏为 0 ⇒ 函数无引用，被 `--gc-sections` 整段丢弃（map 里 `.text.opt_vectorize 0x0 0x3c4`） |
| `compiler.c` 的 RV getter 裁剪 | 与 0.9 那一轮完全相同，本轮未改 |

留一条可复现的判据：单独拿同一套 `CFLAGS_ARKC` 编 `opt.c`，新旧目标文件差 **+96 B**
（3116 → 3212），但链接后差值归零——**目标文件涨了、固件没涨**，原因就是上面第二行。
把这条记下来是因为"体积不变"很容易被读成"改动没生效"，而这次两者恰好都对。

## 7. 相关文件

| 文件 | 内容 |
|------|------|
| `tools/final_app.py` | 板级回归（M1/M2 四条用例 + 前导清场；`--only` / `--noflash` / `--list`） |
| `docs/arkc板上直跑-M1实机记录.md` | M1 的入口契约、窗口校验、优先级与反假绿 |
| `docs/arkc板上直跑-M2可重定位镜像.md` | M2 的四遍差分、五个静默错答案的坑、两落点判据 |
| `kernelsrc/include/arkrtos_loader.h` | `ARKRTOS_LOADER_ERR_x` 的唯一定义处 |
| `kernelsrc/include/arkrtos_apprun.h` / `arkrtos_mini.h` / `arkrtos_apppack.h` | 三条路径各自的独有码 |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | 全部开关（`USE_ARKC` / `USE_APPRUN` / `USE_APPPACK` / `USE_MINI_VFS` / `MINI_PRIORITY`） |

**2026-11-08 追加（上游 `d412208` + `2f3da38`：ARM 的 16 字节 `_Complex` 返回值改走 `r0-r3`、
RV32I 的 `_Complex float` 返回丢值修复、`(*fp)(params)` 保留参数表）**：这一轮的板端体积
**真的变了**，不是上一轮那种"编边界外、一位没动"：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 412 972 | 428 532 | **+15 560** |
| `data` | 2 256 | 2 260 | +4 |
| `bss` | 463 532 | 463 528 | −4 |
| FLASH 占用 | 79.20% | **82.17%** | +2.97 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（同一套 `CFLAGS_ARKC`，`arm-none-eabi-size` 的 `text` 列）：

| 目标文件 | 旧 | 新 | 差 |
|---|---|---|---|
| `backend_arm.o` | 78 572 | 90 110 | +11 538 |
| `irgen.o` | 24 386 | 27 797 | +3 411 |
| `sema.o` | 5 495 | 6 063 | +568 |
| `parser.o` | 22 051 | 22 075 | +24 |
| `ir.o` | 1 706 | 1 710 | +4 |
| `compiler.o` | 18 826 | 18 834 | +8 |

原因：`ARKCC_FEATURE_COMPLEX` 目前只闸住 parser 的两处语法，机器后端的复数四字值通路
（16 字节 `IR_STORE`/`IR_LOAD`、取实部/虚部、`float ↔ double` 加宽、返回值寄存器展开）
**无条件编入**；RV32I 后端整体不在 STM32 profile 里，因此 RV 侧的新代码一行都没进固件。
`bss` 少 4 字节是符号布局变化，不是删了对象；`data` 多的 4 字节是新增的复数初值常量。

判定：FLASH 82.17%、RAM/CCMRAM 未变，仍在预算内，**接受**。

> **2026-11-09 更新**：当时写的"按档裁剪需要给 irgen 与四个后端补闸门，登记为欠账，
> 不声称可关"这件事**已经做完了**。同一套 CFLAGS 下加 `-DARKCC_FEATURE_COMPLEX=0`
> 即可回收 **13 824 B**（`text 438812 → 424988`，FLASH 84.13% → 81.49%）。
> 机制与实测见本文件末尾"欠账 ④"一节；这里保留原判定不动，因为它是当时的事实记录。
> 与 15 560 的差额约 1.7 KB 是共用双精度通路与常量序列化，闸门关不掉，不再追。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得 `text 412972`；
`git stash pop` → 重建得 `text 428532`。两次构建除 arkc 源码外的一切（CFLAGS、链接脚本、
其余目标文件）完全相同。

**2026-11-08 追加（上游 `060b008`：RV32I 的大结构体返回值按 psABI 实现 sret、ARM 的 sret 判定
改由 IR 位承载、`ARK_MAX_SRET_FUNCS` 语义改为"每个函数的大结构体返回调用站点上限"）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 428 532 | 428 740 | **+208** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 528 | 463 528 | 0 |
| FLASH 占用 | 82.17% | 82.21% | +0.04 pp |

逐目标文件（`arm-none-eabi-size` 的 `text` 列）：

| 目标文件 | 旧 | 新 | 差 |
|---|---|---|---|
| `irgen.o` | 27 797 | 28 013 | +216 |
| `backend_arm.o` | 90 110 | 90 110 | 0 |

**为什么只有 irgen 变**：RV32I 后端整体不在 STM32 profile 里，psABI 的 sret 实现一行都没进
固件；ARM 侧是"删掉按名字查表的 `sret_lookup`/`sret_get_size` + 删掉每个翻译单元建表的那段"
换"读 `IR_CALL` 的 bit36/载荷字段 + 每块缓冲按 8 字节对齐 + 超限报错"，两边体量正好抵平
（编译产物逐字节不同，但 `.text` 大小相同）。`bss` 不变是因为 `sret_call_buf_off` 是栈上数组，
且板端档位本来就是 64 项。

判定：FLASH 82.21%、RAM/CCMRAM/NOINIT 未变，仍在预算内，**接受**。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得 `text 428532`；
`git stash pop` → 重建得 `text 428740`。两次构建除 arkc 源码外的一切（CFLAGS、链接脚本、
其余目标文件）完全相同。

**2026-11-08 追加（上游 `80f98b4`：支持函数类型 typedef `typedef Ret F(args);`）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 428 740 | 430 188 | **+1 448** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 528 | 463 528 | 0 |
| FLASH 占用 | 82.21% | 82.49% | +0.28 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（`arm-none-eabi-size` 的 `text` 列，122 个目标文件全表比对）：

| 目标文件 | 旧 | 新 | 差 |
|---|---|---|---|
| `parser.o` | 22 075 | 23 482 | +1 407 |
| 其余 121 个 | — | — | 0 |

输出 ELF 的段级差：`.text` 357 620 → 358 796（+1 176）、`.rodata` 70 232 → 70 504（+272），
合计 +1 448；`parser.o` 自身 text 列 +1 407，两者相差 41 字节来自段对齐填充（`.rodata` 扩容
后对齐边界移动），不是另有代码变动。

原因：RV32I 后端整体不在 STM32 profile 里，`rv_*` 测试一行都没进固件；ARM 侧新增的是纯前端
（`parse_func_type_tail` / `params_from_func_type` / 两处 typedef 注册 / `sizeof`、`_Alignof`
的函数类型拒绝 / `parse_declspec` 的 `if (base) goto done;`），`irgen.o`、`backend_arm.o` 逐字节
不变即可佐证 —— 这是"只动解析层、不动代码生成"的正面证据。

判定：FLASH 82.49%、RAM/CCMRAM/NOINIT 未变，仍在预算内，**接受**。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得 `text 428740`；
`git stash pop` → 重建得 `text 430188`。两次构建除 arkc 源码外的一切（CFLAGS、链接脚本、
其余目标文件）完全相同。

**2026-11-08 追加（上游 `30502d0`：支持全局地址常量初值，C11 6.6p9）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 430 188 | 432 628 | **+2 440** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 528 | 463 532 | **+4** |
| FLASH 占用 | 82.49% | 82.95% | +0.46 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（`arm-none-eabi-size` 的 `text`/`data`/`bss` 三列，122 个目标文件全表比对，
只有 3 个变化，其余 119 个逐列相同）：

| 目标文件 | text 旧 -> 新 | 差 | bss 旧 -> 新 |
|---|---|---|---|
| `irgen.o` | 28 013 -> 29 810 | +1 797 | 12 -> 16 |
| `backend_arm.o` | 90 110 -> 90 742 | +632 | 9 428 -> 9 428 |
| `ir.o` | 1 710 -> 1 718 | +8 | 4 -> 4 |
| 其余 119 个 | — | 0 | — |
| 合计（122 个 .o 的 text 列） | 492 081 -> 494 518 | +2 437 | — |

输出 ELF 的段级差：ELF `text` +2 440 比 `.o` 合计 +2 437 多 3 字节，来自段对齐填充，不是
另有代码变动。

原因与逐文件落点：`irgen.o` 是主体——新增地址常量走查器（`addr_elem_size` / `addr_value` /
`addr_lvalue` / `looks_like_address` / `irgen_try_addr_init`）+ `serialize_const_expr` 的
`AST_ARRAY_ACCESS`/`AST_MEMBER`/`TOK_BAND` 三个分支 + 5 处调用点与 5 处 `g_ser_unknown` 收尾；
`bss +4` 就是新增的 `static int g_ser_unknown`。`backend_arm.o` +632 是 `init_sym` 的生成分支
（`LDR r0,=name` 转发 / `emit_mov_addr32(基址+偏移)`）与 3 处零标量守卫。`ir.o` +8 是
`IrGlobal` 三字段的初始化语句。**RV32I 后端整个不在 STM32 profile 里**，因此它的 `.word sym`
分支一行都没进固件——这也是"改的是新路径、没动既有代码生成"的正面证据（其余 119 个目标
文件逐列相同）。

判定：FLASH 82.95%、RAM/CCMRAM/NOINIT 未变，仍在 512 KB 预算内（余 89.05 KB），**接受**。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得
`text 430188 / data 2260 / bss 463528`；`git stash pop` → 重建得
`text 432628 / data 2260 / bss 463532`。两次构建除 arkc 源码外的一切（CFLAGS、链接脚本、
其余目标文件）完全相同。

**2026-11-08 追加（上游 `ce98151`：聚合初始化器里的地址元素 + 函数指针数组声明符）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 432 628 | 435 012 | **+2 384** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 532 | 463 532 | 0 |
| FLASH 占用 | 82.95% | 83.41% | +0.46 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（只有 arkc 侧的 5 个目标变化）：

| 目标文件 | text 旧 -> 新 | 差 |
|---|---|---|
| `irgen.o` | 29 810 -> 31 242 | +1 432 |
| `backend_arm.o` | 90 742 -> 91 242 | +500 |
| `parser.o` | 23 482 -> 23 802 | +320 |
| `agginit.o` | 2 728 -> 2 868 | +140 |
| `ir.o` | 1 718 -> 1 722 | +4 |
| 五者合计 | — | +2 396 |

ELF `text` +2 384 比逐目标合计 +2 396 少 12 字节，是段对齐/填充差，不是另有代码变动。

落点：`irgen.o` 是新增的第二次 AST 走查（`irgen_add_ref` / `irgen_agg_addr_scalar` /
`irgen_agg_addr_walk` / `irgen_init_has_addr` / `irgen_try_agg_addr_init`）+ 9 处调用点；
`parser.o` 是 `(*name)` 分支支持括号内数组维度；`agginit.o` 是 `ark_agg_fill_ex()` 的
`addr_hook` 与"命中后推进文本游标"；`backend_arm.o` 是 `arm_agg_addr_hook()` 与聚合初始化
后按 `init_refs` 覆盖写地址的循环。`bss` 未变——本轮没有新增文件作用域变量。
**RV32I 后端仍不在 STM32 profile 里**，它的 `.word sym` 分支一行都没进固件。

判定：FLASH 83.41%、RAM/CCMRAM/NOINIT 未变，仍在 512 KB 预算内（余 84.59 KB），**接受**。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得
`text 432628 / data 2260 / bss 463532`（FLASH 82.95%）；`git stash pop` → 重建得
`text 435012 / data 2260 / bss 463532`（FLASH 83.41%）。两次构建除 arkc 源码外的一切
（CFLAGS、链接脚本、其余目标文件）完全相同。

**2026-11-08 追加（上游 `b8378f4`：聚合初始化器里的字符串字面量元素）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 435 012 | 435 652 | **+640** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 532 | 463 532 | 0 |
| FLASH 占用 | 83.41% | 83.53% | +0.12 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（只有 arkc 侧的 2 个目标变化，其余逐列相同）：

| 目标文件 | text 旧 -> 新 | 差 |
|---|---|---|
| `irgen.o` | 31 242 -> 31 700 | +458 |
| `backend_arm.o` | 91 242 -> 91 423 | +181 |
| `parser.o` / `agginit.o` / `ir.o` | 23 802 / 2 868 / 1 722 | 0 |
| 二者合计 | — | +639 |

ELF `text` +640 比逐目标合计 +639 多 1 字节，是段对齐填充，不是另有代码变动。

落点：`irgen.o` 是 `irgen_add_ref_ex()`（末位 `str` 参数）、`irgen_agg_addr_scalar()` 的
`AST_STR` 分支（含宽字面量报错）与新增的类型感知扫描 `irgen_init_has_str_addr()`；
`backend_arm.o` 是 `.rodata` 里按元素偏移发 `.L_gstr_<gi>_<off>` 标签的循环，加 refs 写入
循环里的 `if (r->str)` 分派。`bss` 未变——本轮没有新增文件作用域变量。
**RV32I 后端仍不在 STM32 profile 里**，它的 `.Lgstr_<name>_<off>` 与 `.word` 分支一行都没进固件
（这也是"改的是新路径、没动既有代码生成"的正面证据）。

判定：FLASH 83.53%、RAM/CCMRAM/NOINIT 未变，仍在 512 KB 预算内（余 84.34 KB），**接受**。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得
`text 435012 / data 2260 / bss 463532`（FLASH 83.41%）；`git stash pop` → 重建得
`text 435652 / data 2260 / bss 463532`（FLASH 83.53%）。两次构建除 arkc 源码外的一切
（CFLAGS、链接脚本、其余目标文件）完全相同。

**2026-11-08 追加（上游 `6fce783`：聚合初始化器里的指派器与 union 地址元素 + 设计器下标折常量）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧 | 新 | 差 |
|---|---|---|---|
| `text` | 435 652 | 436 108 | **+456** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 532 | 463 532 | 0 |
| FLASH 占用 | 83.53% | 83.61% | +0.08 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（只有 arkc 侧的 2 个目标变化，其余逐列相同）：

| 目标文件 | text 旧 -> 新 | 差 |
|---|---|---|
| `irgen.o` | 31 700 -> 31 833 | +133 |
| `parser.o` | 23 802 -> 24 126 | +324 |
| `backend_arm.o` / `agginit.o` / `ir.o` / `lexer.o` | 91 423 / 2 868 / 1 722 / 7 945 | 0 |
| 三者合计 | — | +457 |

ELF `text` +456 比逐目标合计 +457 少 1 字节，是段对齐填充，不是另有代码变动。

落点：`irgen.o` 是走查改写（新增 `irgen_agg_addr_place()`、数组分支的 `cur` 游标、
结构/`union` 分支的 `f` 游标与 `.name=` 重定位、`union` 单活跃成员限制）；`parser.o` 是新增
`parse_designator_index()`（`parse_cond_expr` + `eval_const_expr` 折常量，失败报到 `]` 为止）。
`backend_arm.o` 为 0 —— 本轮没有改任何代码生成路径，只改了"前端怎么定地址元素的偏移"和
"解析器怎么读设计器下标"，与上一轮 `.rodata` 标签那条通路无关。

判定：FLASH 83.61%、RAM/CCMRAM/NOINIT 未变，仍在 512 KB 预算内（余 83.86 KB），**接受**。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得
`text 435652 / data 2260 / bss 463532`（FLASH 83.53%）；`git stash pop` → 重建得
`text 436108 / data 2260 / bss 463532`（FLASH 83.61%）。两次构建除 arkc 源码外的一切
（CFLAGS、链接脚本、其余目标文件）完全相同。

**2026-11-08 追加（上游 `c6ebdc6`：花括号省略 brace elision —— 全局数据 / 地址元素 / 块作用域三路 + 维度推导 + 标量花括号）**：

板端（STM32H743，`USE_ARKC=1`，同一套 CFLAGS / 链接脚本）：

| 项 | 旧（`6fce783`） | 新（`c6ebdc6`） | 差 |
|---|---|---|---|
| `text` | 436 108 | 438 724 | **+2 616** |
| `data` | 2 260 | 2 260 | 0 |
| `bss` | 463 532 | 463 532 | 0 |
| FLASH 占用 | 83.61% | 84.11% | +0.50 pp |
| RAM / CCMRAM / NOINIT | 98.22% / 17.33% / 97.66% | 同左 | 0 |

逐目标文件（只有 arkc 侧的 3 个目标变化，其余逐列相同）：

| 目标文件 | text 旧 -> 新 | 差 |
|---|---|---|
| `irgen.o` | 31 833 -> 33 436 | +1 603 |
| `parser.o` | 24 126 -> 24 430 | +304 |
| `agginit.o` | 2 868 -> 3 588 | +720 |
| `backend_arm.o` / `ir.o` / `lexer.o` | 91 423 / 1 722 / 7 945 | 0 |
| 三者合计 | — | +2 627 |

段级拆分（`arm-none-eabi-size -A`，与 ELF 总数吻合）：`.text` 362 972 -> 365 076（**+2 104**）、
`.rodata` 72 248 -> 72 760（**+512**），合计正好 +2 616。逐目标合计 +2 627 与 ELF +2 616 差 11 字节，
差在链接期的段合并 / 对齐，不是另有一处代码变动（段级两栏相加与 ELF 完全相等）。

落点：`irgen.o` 是三条路径里的两条 AST 走查（`AaggWalk` 的 `aw_item_value/aw_object/aw_fill/aw_braced`
与 `LocalAggWalk` 的 `lw_value/lw_designator/lw_object/lw_field/lw_fill`）整体重写 + 新增
`irgen_unwrap_scalar_braces()`（标量花括号解包）与 `_Complex` 边界诊断；`agginit.o` 是全局序列化文本
填充 `ark_agg_flat()` / `ark_agg_object()` 的游标化（数组分支判满 `return off`、逗号跳过挪到判满之后、
字符串短路只在 `TYPE_ARRAY` 生效）；`parser.o` 是导出 `ark_infer_array_size_from_init()` 与
`infer_leaf_slots()`（按「每元素值数 cap」推维度）。`backend_arm.o` 为 0 —— 两后端代码生成一行未改，
花括号省略全部收在前端「把初值摊平成 (偏移, 值) 序列」，与上一轮的 `.rodata` 标签通路无关。

判定：FLASH 84.11%、RAM/CCMRAM/NOINIT 未变，仍在 512 KB 预算内（余 81.24 KB），**接受**。
（block 作用域字符串初始化会展开成 `memcpy` 调用，所以本轮的块作用域用例真的会进固件链 `memcpy`；
上板体积已含它。）

可复现判据：`git stash push -- kernelsrc/components/arkc` → `make clean` 后重建得
`text 436 108 / data 2 260 / bss 463 532`（FLASH 83.61%），逐目标 `irgen.o` 31 833 / `parser.o` 24 126 /
`agginit.o` 2 868；`git stash pop` → 重建得 `text 438 724 / data 2 260 / bss 463 532`（FLASH 84.11%），
逐目标 33 436 / 24 430 / 3 588。两次都是 `make clean` 全量重建（不是增量链接），除 arkc 源码外的一切
（CFLAGS、链接脚本、其余目标文件）完全相同。

回归账（本轮全量重跑，宿主侧，`c6ebdc6`）：

| 门禁 | 结果 |
|---|---|
| `tests/run_agg_addr_tests.py --arm-as --arm-libgcc --rv-as` | **83 passed / 0 failed**（CASES 32 × 2 后端 + MUST_FAIL 8 × 2 + RV asm 3；其中 9 例是花括号省略） |
| `tests/run_func_ptr_global_tests.py` | 39 / 0 |
| `tests/run_complex_tests.py` | 76 / 0 |
| `tests/run_arm_int64_tests.py`（不吃 `--rv-as`） | 12 / 0 |
| `tests/run_c11_keywords_tests.py`（不吃 `--arm-libgcc`） | 30 / 0 |
| `examples/run_va_gate.py --arm-as --arm-libgcc --rv-as` | 59 / 0 / 2 targets，`--opt-suite` 5/5 green |
| `run_rv_image` / `alloca` / `sret` / `misc_ir` / `float` / `packed`（`--rv-as`） | 20 / 5 / 13 / 12 / 11 / 7，全 0 failed |
| `tests/run_rv_link_tests.py` | 10 / 0 |
| `tests/run_func_typedef_tests.py` | 24 / 0 |
| `run_capacity_tests.py` / `run_diag_tests.py` / `run_diag_boundary.py` / `run_regalloc_tests.py` | 4 / 0；0 failure；all boundary held；all claims hold |
| `tests/run_reloc_text_tests.py ./arkcc.exe` | all checks passed |
| `/tmp/runps2.py` | done rc=0 |

口径两条（本轮踩到，写死在这里）：
1. `run_agg_addr_tests.py` 的 `--arm-as` / `--rv-as` 收的是**目录**（内部按 `arm-none-eabi-` /
   `riscv64-unknown-elf-` 前缀找工具），传可执行文件路径会 `TypeError: 'NoneType' object is not callable`。
2. 门禁 runner 取的是**进程退出码**，即程序 `main` 的返回值 **mod 256**；写用例期望值时
   必须保证值 < 256，否则"oracle 不符"全部是模运算造成的假 FAIL。

---

## 欠账 ④：`ARKCC_FEATURE_COMPLEX` 变成真的可裁开关（arkc 本轮）

背景：`ARKCC_FEATURE_COMPLEX` 过去只闸住 parser 的两处语法，机器后端的四字值通路
（物化/取实虚部/加宽/返回寄存器展开）**无条件编入**，所以板端固件为复数特性长了
15.5 KB，而翻开关一点用没有。本轮把它做成真开关。

做法（三件常量折叠 + 四处手写 `#if`）：

- `include/ir.h`：`IR_AUX_COMPLEX` 双档定义 —— 闸门开 `0x800`，闸门关 **`0`**。
  `arkcc_config.h` 则由无条件 `#define` 改成 `#ifndef`，否则 `-D` 会被头文件覆盖
  （本轮踩到过：两档体积逐字节相同，就是这条）。
- `include/type.h`：新增 `ARK_IS_CX(t)` —— 闸门开 `((t)->kind == TYPE_COMPLEX)`，
  闸门关 **`0`**。`irgen.c` / `sema.c` / 三个机器后端里 47 处 `kind == TYPE_COMPLEX`
  比较全部改用它。
- `src/parser.c`：`case TOK_COMPLEX:` 双档报错。**顺带修掉一个静默错答案** ——
  运行期 `-fno-complex` 以前是*忽略*关键字，`double _Complex z` 静默变成 `double z`；
  现在报 `` `_Complex` is switched off by the run-time configuration (complex=0) ``。
- 值驱动、折不掉的部分手写 `#if`：`sema.c` 的 `eval_const_cexpr`、
  `irgen.c` 的 `irgen_try_complex_init`（五个调用点改成
  `#if ARKCC_FEATURE_FCONST_FOLD && ARKCC_FEATURE_COMPLEX` 只包那一句 `if`，
  `else` 留在外面保住 `else if` 链）、ARM 与 RV32I 后端的 `cplx_ret`
  （关档改成 `const int cplx_ret = 0`，下面两个分支自己折掉）。

体积（H743 固件 `example/stm32h743/kernel/ARKRTOS_TEST/GCC`，两档都是
`make clean` 全量重建，唯一差别是 `OPTFLAGS` 末尾多一个 `-DARKCC_FEATURE_COMPLEX=0`）：

| 口径 | 闸门开 | 闸门关 | 差 |
|---|---|---|---|
| `text` | 438 812 | 424 988 | **−13 824 B** |
| FLASH 占用 | 84.13% | 81.49% | −2.64 pp |
| `data` / `bss` | 2 260 / 463 532 | 2 260 / 463 532 | 不变 |

宿主 x86-64（四后端全编）：`text 556 740 → 535 484`（−21 256 B）。

ARM 目标集（与固件同一套 `STM32_CFLAGS`，逐文件 `-c` 后 `size -t` 求和，**不是**固件链接值）：
`246 554 → 232 926`（−13 628 B）；逐目标 `backend_arm.o −8 664`、`irgen.o −4 264`、
`sema.o −608`、`parser.o −92`。

口径两条（必须一起读）：
1. 闸门开时的固件比上一轮账本（`text 438 724`）多 **88 B**，那是上面那条新诊断
   （parser 的运行期拒绝分支）。闸门开时其余输出逐字节不变，host 侧 `text` 仍是 556 740
   （与改动前一致）。
2. 与历史 `+15 560 B` 差约 **1.7 KB 不追**：那份增量按"整个复数特性的增长"算，
   其中有一部分是**双精度通路与常量序列化**，它们同时服务 `double`，闸门关不掉；
   外加 `init_is_complex` 字段接线与那 88 B。报 −13 824（可复算），不把未回收写成已收回。

新增门禁 `tests/run_complex_gate.py`（宿主，需两个二进制同时在场，**不自己构建**）：

| 检查 | 判据 |
|---|---|
| `full/accepts-complex` | 全量档编 `_Complex` 必须成功（正控制） |
| `trimmed/rejects-complex` | 裁剪档必须失败 |
| `trimmed/reason-is-configured-out` | 失败原因必须是 `is not available in this build` |
| `trimmed/not-a-crash` | 不得是信号/段错误 |
| `trimmed/accepts-plain-C` | 裁剪档必须照收普通 C |
| `both/plain-asm-identical` | 不含 `_Complex` 的源在两档下汇编文本**逐字节相同** |
| `full/-fno-complex-is-an-error` | 运行期开关必须报错而非静默忽略 |
| `selfcheck/plain-build-has-no-config-msg` | 自检的自检：普通源不得出现裁剪档那句文案 |

实测（`--arkcc ./arkcc.exe --arkcc-off ./arkcc_off.exe --arm-as … --arm-libgcc … --rv-as …`）：
**9 checks, all green**（含 `full/complex-semantic-suite` 76 passed / 0 failed）。

回归账（闸门开档全量重跑，见下表；与上一轮逐项相同，无新增失败）：

| 门禁 | 结果 |
|---|---|
| `tests/run_agg_addr_tests.py --arm-as --arm-libgcc --rv-as` | 83 / 0 |
| `tests/run_func_ptr_global_tests.py` | 39 / 0 |
| `tests/run_complex_tests.py` | 76 / 0 |
| `tests/run_arm_int64_tests.py`（不吃 `--rv-as`） | 12 / 0 |
| `tests/run_c11_keywords_tests.py`（不吃 `--arm-libgcc`） | 30 / 0 |
| `examples/run_va_gate.py --arm-as --arm-libgcc --rv-as` | 59 / 0 / 2 targets，`--opt-suite` 5/5 green |
| `run_rv_image` / `alloca` / `sret` / `misc_ir` / `float` / `packed`（`--rv-as`） | 20 / 5 / 13 / 12 / 11 / 7，全 0 failed |
| `tests/run_rv_link_tests.py` | 10 / 0 |
| `tests/run_func_typedef_tests.py` | 24 / 0 |
| `run_capacity_tests.py` / `run_diag_tests.py` / `run_diag_boundary.py` / `run_regalloc_tests.py` | 4 / 0；0 failure；all boundary held；all claims hold |
| `tests/run_reloc_text_tests.py ./arkcc.exe` | all checks passed |
| `tests/run_complex_gate.py`（新） | 9 / 9 |

---

## 2026-11-09：块作用域的多余初值从「静默丢弃」改成 GCC 风格警告

**背景（这是清单里的一条已知项，不是新功能）**：`int b[2] = {1, 2, 3};` 写在函数里时，
C99 6.7.9p2 说这是约束违反、必须给诊断。arkcc 以前在**块作用域**把多余元素直接丢掉、
一句提示都没有——结论本身不假，但用户以为自己写了三个元素。这正是「宁可报错也不给错
答案」不能容忍的形态（静默吞掉用户写的东西）。文件作用域一直有诊断（`cannot turn the
initialiser of the global 'a' into data`），文案不贴切但至少不静默，**本轮不动**。

**做法**（`src/irgen.c`，只加诊断，写入的字节不变）：

- `LocalAggWalk` 加 `int *excess` 共享标志；`lw_object` 为子 aggregate 新建的 `sub`
  把指针复制下去 ⇒ `int b[2][2] = {1,2,3,4,5}` 这种「里层越界」也只警告一次，不是每行一次。
- 数组分支在 `cur >= len`、结构分支在「所有成员都填满」处调用
  `lw_warn_excess(w, cap, is_array)`。结构分支要先数一遍成员链表拿到成员总数
  （那里的 `f` 只是「还有没有成员可填」的游标）。
- 花括号省略（`int b[2][2] = {1,2,3,4}`）走 `!braced` 早退分支，**不误报**。
- `diag_tool` 签名是 `(sev, object, msg, a0..a5)`——六个字符串；数字必须先用
  `diag_num(buf, v)` 渲染，直接传 `int` 会段错误（本轮踩过，`rc=139`）。

**实测（板端 `make clean` 全量重建，默认档，唯一差别是源码同步）**：

| 口径 | 改前 | 改后 | 差 |
|---|---|---|---|
| `text` | 438 812 | 439 060 | +248 B |
| FLASH 占用 | 84.13% | 84.18% | +0.05 pp |
| `data` / `bss` | 2 260 / 463 532 | 2 260 / 463 532 | 不变 |

宿主 x86-64（四后端全编）：`text 556 740 → 557 100`（+360 B）。
差额全部是新增诊断文本本身，**不改变任何初始化写入**（`-S` 产物除诊断外的路径不变）。

行为实测：
- `int b[2]={1,2,3};` → `arkcc: Warning: excess elements in the initialiser are ignored: the array holds only 2`，`rc=0`（与 GCC 一致：警告而非错误）。
- `struct S s={1,2,3};`（两个成员）→ `... the object holds only 2`。
- `int b[2][2]={1,2,3,4};` → **无输出**（花括号省略，合法）。

**回归**：闸门开档全量重跑与上一轮逐项相同，无新增失败——
`agg 83/0`、`func_ptr_global 39/0`、`complex 76/0`、`arm_int64 12/0`、`c11 30/0`、
`va_gate 59/0/2 targets + opt 5/5`、`run_rv_{image,alloca,sret,misc_ir,float,packed}`
20/5/13/12/11/7、`rv_link 10/0`、`func_typedef 24/0`、`capacity 4/0`、`diag 0 failure`、
`diag_boundary all held`、`regalloc all claims hold`、`reloc_text all passed`、
`complex_gate 9/9`。

---

## 2026-11-09：`-mcpu Cortex-M7` 的双精度开关被清零（板端 `__aeabi_f2d` 的根因）

**现象**：`mini-sdk-va` 用例（板上 `cc` 编标准 C 的 `va_start/va_arg`，取 `double`）
链接期报 `unresolved symbol "__aeabi_f2d"`。

**根因分三层，缺一层都修不好**：

1. 上一轮把 `float` 穿 `...` 改成按 C 标准**提升为 `double`**（正确），于是需要一次
   `float -> double` 的转换指令。
2. 板端 port 从没告诉编译器目标 CPU，后端走保守路径发 `BL __aeabi_f2d`；
   而小程序侧的 SDK 运行时（`kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c`）
   **没有任何 `__aeabi_f2d`/`__aeabi_d2f`**——这个符号在板端不存在。
3. 更深的一层在上游：`src/compiler.c` 里 `-mcpu` 块已经按 CPU 表算出
   `g_arm_fpu_dp = cpu->has_fpu_dp`，但**下游又有一句 `g_arm_fpu_dp = 0;`**
   把它清回 0 ⇒ `-mcpu Cortex-M7` 看着被接受、实际仍发 `BL __aeabi_f2d`。
   这不是板端问题，是编译器缺陷。

**修法**：

- 上游 `src/compiler.c`：把 `g_arm_fpu_dp = 0` 从下游挪到 `-mcpu` 块**之前**。
  挪之前还必须是「复位」语义——板端同一个进程里反复调 `ark_compile()`，
  上一次带 `-mcpu Cortex-M7` 留下的 1 不能泄漏进这一次没有 `-mcpu` 的编译。
- 板端 port `kernelsrc/components/arkc/arkc_shell_cmd.c`：`cc_pack_fill()` 与单编路径
  都显式 `cpu = "cortex-m7"`（板子就是 H743，VFPv5-D16）。**不去手写软浮 helper**——
  用板上真有的硬件。

**实测**：`--cpu cortex-m7` 时 `float->double` 发 `VCVT.F64.F32 d0, s0`；
不带 `-mcpu` 时仍发 `BL __aeabi_f2d`（保守路径不变，宿主既有产物一个字节都没动）。
板端回归 `mini-sdk-va` 由 FAIL 转 PASS。

---

## 2026-11-09：M2 判据 B 改成「按真实落点现算」，不再写死块大小

判据 B 要验的是「程序读自己的绝对地址读对了」，所以期望值必须由 `mini run` 自报的基址
现算：`want = (base - 0x24070000) >> 12`。上一版把期望写成「4096 B 块、落点 0x24070000/0x24071000、
打印 0/1」，而伙伴池的块是「≥ 负载的最小 2 的幂」——`m2.svcm` 负载 1652 B ⇒ 2048 B 块，
两次落点 `0x24070000` / `0x24070800`，`>>12` 后都是 0。写死块大小等于把判据变成
「这块板今天几 B 一块」，不是被测对象。

同时修掉判据里的一个真 bug：取程序自报数字的正则原来写了 `^M2 alive\n...`，
而串口回显里 `M2 alive` 前面还挂着提示符（`ark@arkrtos:~$ M2 alive`）⇒ 永远匹配不上、
用例假 FAIL，并且因为程序是死循环，失败后残留两个实例把池占满，
**后面三条 `mini-sdk-*` 全部级联 FAIL**。去掉 `^`（保留 `re.M`）后 9/9 全绿。

---

## 2026-11-09：板级回归 9/9 PASS（固件 `FLASH 488988 B / 93.27%`）

构建档（**缺一项就会假 FAIL**，前一版只给了 `USE_ARKC/USE_APPRUN/USE_APPPACK/USE_MINI_VFS`
⇒ `/mnt/nor` 根本没挂载、`make: command not found`，5 条用例红）：

```sh
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
```

实测体积：`FLASH 488988 B (93.27%)`、`RAM 308240 B (68.41%)`、
`SDRAMBUF 10744 KB / 12 MB`、`SDRAMFS 5882656 B / 6 MB`；
`text 486692 / data 2284 / bss 17191012`。

结果（`python tools/final_app.py --noflash`）：`m1-fixed-window`、`m1-wrong-window`、
`m2-pack-run`、`m2-two-landings`、`mini-sdk-std-hello`、`mini-sdk-va`、
`mini-sdk-missing-main`、`stack-usage`、`vfs-create` —— **all 9 cases PASS**。

---

## 2026-11-09：M3-d 跨复位自启 + 回归 12/12 PASS + 结构门禁 12/12 绿

### 三条真缺陷（都只在真机上才现形）

**① 自启跑到 VFS 挂点建好之前 ⇒ 每条清单项都 -18。**
自启原来在 `arkrtos_fs_mount()` 里触发，而这个函数是被
`arkrtos_vfs_mount_volume()` 调进来的——此刻 `ark_vfs_mount()` 还没执行，
`/mnt/nor` 这个挂点根本不存在。清单里写的是 `mini run` 那条写法
（`/mnt/nor/xxx.svcm`），靠挂载点解析，于是每一条都以 `-18` 失败；
更糟的是 `g_mini_auto_boot_done` 已经置上，**整次开机不再重试**。
改法：VFS 构建把自启搬到挂载成功之后（`arkrtos_vfs.c` 里新增调用点），
`arkrtos_fs.c` 那边加 `&& (ARKRTOS_MINI_READ_VFS == 0)` 只留给非 VFS 构建，
保证「默认关闭时行为零变化」。

**② 清单路径的存在性被问到了错的那一层。**
`mini autostart` 用 `arkrtos_fs_stat_path()`（卷根坐标）去查
`/mnt/nor/c3auto.svcm`，而内核读镜像走的是 VFS 命名空间 ⇒ 于是出现
「`mini run` 刚跑成功、紧接着 `mini autostart` 报 `not in the volume (fs err -2)`」。
改法：VFS 构建改问 `arkrtos_vfs_app_stat()`，并且错误文案也走
`arkrtos_vfs_error_name()`——问哪一层，就说哪一层的话。

**③ Kconfig 的 string 符号被生成成 `""`，把头文件默认值吃掉。**
`ARKRTOS_AUTOMOUNT_NOR_POINT` 的 Kconfig 默认是 `"/mnt/nor"`，但它的依赖
（`ARKRTOS_AUTOMOUNT_NOR`）在 defconfig 里是 `n`，板子是在 **Makefile 里用
`-DARKRTOS_AUTOMOUNT_NOR=$(USE_QSPI)` 打开**的。于是 genconfig 解析出空值、
原样写成 `#define ARKRTOS_AUTOMOUNT_NOR_POINT ""`——`#ifndef` 兜底再也轮不到，
开机挂载点变成空串，`mount nor0 at : -2`，**整块 NOR 没挂上，6 条用例连带红**。
改法：`tools/kconfig/genconfig.py` 对空 string **不生成**（bool 仍然恒生成 0，
那条注释里说的理由对 string 不成立：`""` 是合法值，头文件默认值分不出
「配成空」与「没配」）。这一条是「宁可报错不给错答案」在生成器上的落点——
空路径不该被当成一个可用的挂载点。

另外 `main.c` 调 `arkrtos_fs_last_error()` / `arkrtos_fs_err_is_unformatted()`
却没包含 `arkrtos_fs.h`（隐式声明，d32fb84 带进来的），一并补上。

### 打包器的标称基址改成走分区头

`arkc_shell_cmd.c` 里 `CC_PACK_ROM_BASE/CC_PACK_RAM_BASE` 原来写死
`0x08000000/0x20000000`，改成 `ARKRTOS_APP_NOMINAL_ROM_BASE` /
`ARKRTOS_APP_NOMINAL_RAM_BASE`（`config/arkrtos_partition.h` 的唯一来源）。
语义不变：这两个值只进镜像头，装载器运行时按「落点 − 标称」补差；
变的是 delta 从「大偏移」变成「池内小偏移」，`m2-pack-run` /
`m2-two-landings` / `m2-cross-tu-link` 三条用例已实测不退化。
帮助文本里那个具体地址也改成「the image ROM base」，让
`ci_gate` 的 `single-address-source` 真正只剩一个来源。

### 门禁

```sh
env -u PYTHONPATH -u PYTHONHOME <py310> -X utf8 tools/ci_gate.py
# 12 check(s) passed, 0 failed
```

补的四个 Kconfig 符号（`ARKRTOS_USE_QSPI` / `ARKRTOS_AUTOMOUNT_NOR` /
`ARKRTOS_AUTOMOUNT_NOR_POINT` / `ARKRTOS_USE_ARKMAKE`）让 `features-switches`
转绿；`arkrtos_autoconf.h` 重新生成后 `config-generated` 转绿。

### 板级回归：all 12 cases PASS

```sh
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
# FLASH 492900 B (94.01%) / RAM 308240 B (68.41%)
# text 490596 / data 2284 / bss 17191012
python tools/final_app.py     # 会重烧，因为固件变了
```

新增用例 **`mini-autostart-reboot`** 是这条链的正向证据 + 反假绿：
先 `cc --pack` 出一个 svcm、`mini run` 手工跑一次确认它能跑，
再 `mini autostart add` 进清单，然后**复位**（`openocd reset run`，不重烧——
重烧会改 flash，而这条要证明的正是「同一固件、同一块 NOR，换个开机就自己跑」），
断言三行：`autostart: /mnt/nor/c3auto.svcm started`、
`boot autostart: 1 started from /mini.autostart`、程序自己打的 `C3 autostart alive`。
反假绿：从清单里删掉再复位，这两类行**必须不再出现**（若还出现，说明那行输出
来自别的路径，例如 loader 的 slot 自启）。

### 新登记两条（可复用的口径）

**⑤ 「开机那一刻做的事」必须在它依赖的对象存在之后。** 讲顺序的缺陷不会在
单测里现形（函数都返回成功），只在真机冷启动的那一次才现形；判据要能跨复位
（`tools/final_app.py` 的 `Reset` 步），且反假绿必须是「删掉触发条件再复位」。
**⑥ 同一个路径在不同层有不同的坐标。** 问存在性时先确认「内核读它时走的是哪一层」
（fs 卷根坐标 vs VFS 命名空间坐标，错配就会给出「刚跑成功却报不存在」）。
**⑦ 生成器的空值策略也是判据的一部分。** `""` 与「未配置」必须分开表示；
能兜底的 `#ifndef` 只在符号**没被生成**时才有效。

## 2026-11-09：setjmp/longjmp 变成真函数 + `cc --pack` 多对象数据段三处真缺陷 + 回归 14/14 PASS

这一轮补的是「一个完整可用的 C 编译器」里最后两处语言级欠账：`<setjmp.h>`
的一对函数，和 `__thread`。两件事都**只在真机上**才给得出结论，取数方式
一样：板上 `make` + `mini run`，期望串写死，闸门自带防假绿。

### c6-1 `setjmp`/`longjmp`：必须是汇编函数，不能是 C 包装

SDK 新增 `kernelsrc/sdk/mini_sdk/arkc/setjmp.h`（`typedef int jmp_buf[20]`，
用前 40 B：`[0..7]=r4-r11`、`[8]=sp`、`[9]=lr`）与 `setjmp.s`。
工程模板 `example/mini_setjmp/`（`sj.c` 是纯标准 C，`SRCS` 里带上 `setjmp.s`）。

**为什么不用 C 包装 + 机器码 blob**：`setjmp` 要存**调用者**的 `sp`/`lr`，
而 C 包装拿到的永远是包装自己那一帧；包装 `return` 之后那帧会被下一次调用
复用，`longjmp` 恢复 `sp` 再 `bx` 回去，就顺着正常返回路径一路 unwind。
本项目实测过这个错答案：期望 `longjmp(env,7)` 之后 main 拿到 7，实际拿到
**99**（`deep(5)` 正常返回、main 走了自己的 `return 99`）。改成 `.s` 之后
宿主 unicorn 立刻是 `status=returned r0=0`，五条断言全过。

汇编选型上还有一处坑：`it eq; moveq` 里的 `MOVS` 在 ARMv7-M 上
UNPREDICTABLE，所以本实现用 `cmp r0,#0 / bne .L_nonzero / movs r1,#1`。
内部汇编器不支持数字局部标号（`1:` / `1f`，会报 `unresolved symbol "1f"`），
标号必须写成 `.L_xxx`。镜像里逐字节核对过：`2900 f040 8001 2101` 就是
`cmp r0,#0; bne.w; movs r1,#1`。

真机读数（`mini run sj.svcm`）：

```
sj: first pass r=0
sj: jumped back r=7 (want 7)
sj: frames abandoned = 6 (want 6)
sj: longjmp(env, 0) -> 1 (want 1)
sj: ok
```

四条边界写在头文件里，不许当成「已实现完整 C 标准」：不保存 VFP 状态、
没有 signal mask、`jmp_buf` 的布局是本实现私有的、同一份字节在 arkc 仓库
镜像为 `stdlib/src/arm/setjmp.s`（两边必须一致）。

### c6-2 三处真缺陷（都只在板上多对象打包时才现形）

**缺陷 ⑧ `cc --pack` 不打开 `--reloc-data`，两个带全局量的对象撞 SRAM。**
现象：`/tmp/ppA.bin: Error: SRAM overlap between objects #1 and #2: both place
.data/.bss in 0x24070000..0x240700C8` → `make: link failed (-12)`。
根因：`config_init()` 里 `enable_reloc_data` 默认 0，只有 `cc` 命令行在
`-link`/`--scatter` 时才置 1；`cc --pack` 走的是另一条路（它永远在链接，
且同时有多个对象在飞）。修法：接线文件 `arkc_shell_cmd.c` 的
`cc_pack_obj()` 与 `cc_pack_link()` 在各自 `cc_reset_state()` **之后**各加
`g_config.enable_reloc_data = 1;`（必须在其后，`config_init()` 会把它清 0）。
此前只有 hello/va 工程（只有一个对象带全局量）所以从未暴露。

**缺陷 ⑨ 汇编对象谎报前一个编译单元的 RAM 窗口。**
打开 ⑧ 之后现象变了：`object #2 declares data but carries no relocatable
address (compiled without --reloc-data)`，而 #2 正是 `setjmp.s`。
根因：`g_data_sram_start/end`、`g_bss_sram_start/end` 只有 `codegen_arm()`
会写，而汇编输入（`cc -c x.s`）根本不进 codegen——四个边界还是**进程里
上一个** C 文件（rt.c/start.c）留下的值，于是这个对象凭空「声明」了一块
RAM，链接器去搬它、一个补丁站点都找不到，就拒绝了整张镜像。
修法在 arkc 上游（`src/backend/arm/backend_arm.c` 新增
`arm_reset_ram_window()`，`src/compiler.c` 的汇编输入分支在 `mem_pool_reset()`
之后调用它），再 `tools/sync_arkc.py --apply` 同步到 vendored 副本。
口径：**「这个对象没有 RAM 窗口」必须显式说，不能靠上一个对象的状态继承。**

**缺陷 ⑩（口径登记，不是缺陷）`__thread` 在 ARM 上等于普通全局量。**
`__thread int counter = 5;` 与 `int counter = 5;` 生成完全相同的东西：ARM
后端根本不输出数据段，全局初值一律编进 `__arkcc_init_globals()` 逐条写
RAM，所以 `.tbss/.tdata` 的分段选择在这条路上没有任何差别，也没有 TP 基址。
单线程不给错答案，但把它当成「每线程一份」写会在多线程场景静默出错。
真机读数（`mini run tls.svcm`）：

```
tls: init=5 (want 5)
tls: after write=9 (want 9)
tls: zeroed=0 (want 0)
tls: ok (single instance, no TP base -- see the header)
```

**没有**给这条降级加 warning（会打破 `tests/arm_exec` 与 `run_full_test.ps1`
里对 `0 warnings` 的断言，且当前板上没有多线程小程序）；边界写在这里与
`example/mini_tls/tls.c` 的文件头，并用一条板上用例把它钉成可重复读数。

### 板级回归：all 14 cases PASS

```sh
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1
# FLASH 492940 B (94.02%) / RAM 308240 B (68.41%)
python tools/final_app.py            # 会重烧；--noflash 复用已烧固件
```

新增两条用例：

* `mini-setjmp` —— `ls` 确认 `/mnt/nor/{setjmp.h,setjmp.s,sj}`、`cat sj.c`
  看到标准 `<setjmp.h>`、`make` 断言 `pack 4 sources` 且
  `ram_movw [1-9]\d*`（多对象数据段真的被搬过的读数）、`mini run` 五条读数
  全对；`forbid` 里钉着本轮两条错误的原文
  （`SRAM overlap between objects` / `carries no relocatable address`）。
* `mini-tls` —— 三条读数 + 「单实例、无 TP 基址」这条边界。

两条用例各踩了一个**断言形态**坑（不是设备问题，记在这里免得下次再犯）：
① 小程序的第一行输出会和 shell 提示符挤在同一行
（`ark@arkrtos:/mnt/nor/sj$ sj: first pass r=0`），所以**首行不能锚 `^`**，
其余各行照旧锚整行——`mini-sdk-va` 早就是这么写的；
② 板上 `cat` 把非 ASCII 打成 `\xE5...` 转义，所以 `cat tls.c` 的行尾可能是
注释，只能锚行首不能断言行尾。

### 新登记三条（可复用的口径）

**⑧ `--pack` 天生是「多对象 + 链接」**：凡是「只有一个对象时看不出来」的
数据段语义（`reloc_data`、`reloc_text`），默认值都不能沿用单对象那套；
判定方法是问「这条路径上会不会同时有第二个对象带全局量」。

**⑨ 每个对象必须显式声明自己有什么、没有什么。** 进程内反复编译时，
「上一个单元留下的静态状态」会被下一个单元继承，而继承出来的东西在单对象
场景下永远看不见；这类缺陷的修法是在入口处清零，不是在出口处判断。

**⑩ `__thread` 的可移植性要按目标后端问，不按语言标准问。** 「编译器支持
`__thread` 关键字」与「目标上每线程一份」是两件事；这条链路上只有前者。

---

## 2026-11-09：阶段 3c —— 产物烧进片内 Flash 并从那里执行（`app flash`）+ 回归 16/16 PASS

前面所有直跑路径的产物都活在 RAM 里，掉电即失。这一轮补最后一段：
镜像写进 `0x081E0000 + 128K`（bank2 尾部）并从 Flash 取指执行，复位后仍在。

### 实测读数

```sh
make -j8 USE_SDRAM=1 USE_APPRUN=1 USE_APPPACK=1 USE_ARKMAKE=1 USE_MINI_VFS=1 USE_QSPI=1 USE_FLASHAPP=1
# FLASH 497700 B (94.93%) / RAM 309360 B (68.66%)
python tools/final_app.py            # all 16 cases PASS
python tools/ci_gate.py              # 12 check(s) passed, 0 failed
```

```
cc: done in 58 ms                                   -> 1452 B
app flash write /tmp/m5.bin
    FLASHAPP:410 /tmp/m5.bin: 1452 B, crc 0x612B4A1E -> 0x081E0000..0x081E05AC (verified)
app flash info
    flash app: 1452 B, crc 0x612B4A1E, entry 0x081E0424, area 0x081E0000 +128K
app flash run
    FLASHAPP:412 1452 B from flash 0x081E0000, entry 0x081E0424, data 0x24072000 (8192 B), task 4
    M5 alive in flash
    M5 tag bravo
    FLASHAPP:413 task 4 exited, data window returned to the pool
（复位；ramfs 清空，ls /tmp 里没有 m5.bin）
app flash info / run                                -> 仍成功
app flash erase;  app flash run                     -> rc=-44
```

### 新增两条用例

* `m5-flashapp` —— 空区 `info` 报 `-44` → `cc --scatter --vector-table` → `write`
  （带长度 / CRC / 目标地址 / `(verified)`）→ `info` → `run` → **Reset** →
  `ls /tmp` 无 `m5.bin` → `info`/`run` 仍成功 → `erase` → `run` 报 `-44` →
  `app flash bogus` 打 usage → 全程无异常。
* `m5-wrong-window` —— 先用好镜像写成功，再用 **RAM 窗口挪 4 KB**（初始 SP 变成
  `0x24075000`）的坏镜像写，必须报 `-42`，**且区里那份仍能 `info`/`run`**。

### 三条新口径

**⑪ 「写进去了」不等于「写对了」。** `arkrtos_port_flash_write()` 是读-改-写逐
32 B flash word（同一块只能写一次），所以只有**回读逐字节比对**能证明内容正确；
长度与 CRC 只是"我算过"，不是"Flash 里现在是这个"。`write` 的 `(verified)` 就是
这条比对的结果，不是推断。

**⑫ 破坏性动作之前必须先验完形状。** 一个明显不对的镜像不该擦掉区里那份还能跑的。
把 SP / 入口 / 序言三项校验全部放在 `erase` 之前，换来的是"坏输入不损失已有产物"
这条性质——`m5-wrong-window` 就是它的判据（只看错误码不够，必须看旧的那份还在）。

**⑬ 校验"从 Flash 取指"必须同时看两样。** 只看源码独有字符串，把镜像拷进 RAM 里跑
也会通过；判据必须同时要求**日志里的入口点落在 Flash 区**。这条和 `m2-two-landings`
"让程序自己算落点"是同一个思路：让设备报出那个只有真这样跑才会出现的数。

### 顺带修掉的一处会静默 fallback 的隐患

`ARKRTOS_MPU_IMAGE_POOL_SIZE` 原来只覆盖 RAM 池。FlashApp 任务的 `rom_start` 落在
窗口外时，`arkrtos_mpu_rom_window()` 会**静默 fallback 到内核 Flash 窗口**，第一次
取指就 fault —— 现象是"程序一启动就死"，而所有校验都通过。现在这个宏改成
「池 + FlashApp 两段之和」，并加了编译期校验（区必须落在 bank2、区尾必须等于
Flash 末尾、`MAX_IMAGE` 至少要容下 头+256 B）。

### 回归用例的断言形态坑

`Reset` 之后原本断言 `^ark@arkrtos:~/`，但提示符经 ANSI 清理后是
`ark@arkrtos:~$ `（**尾部带一个空格**），`$` 前不匹配 ⇒ 假 FAIL。
`Reset` 只留 `expect=[r"Ark RTOS kernel shell"]`，别处不再重复断言提示符。

### 关闭档的实测与一处真缺陷（2026-11-09 补测）

`USE_FLASHAPP=0` 单独编一次并烧上板：

```
FLASH 492956 B (94.02%) / RAM 308240 B (68.41%)      # 开档是 497700 B / 309360 B
app bogus  -> usage: app [... | run <path>]           # 不含 flash
app flash info -> 落到同一条 usage                     # 不是假装成功，也不是静默忽略
```

**顺手抓到的真缺陷**：`app` 的 usage 行原本在 `ARKRTOS_USE_APPRUN == 1` 分支里
无条件写上了 `| flash <write ...>`。关闭档下这条命令根本不存在，却出现在帮助里——
用户会以为自己参数写错了，而不是"这个构建没编这个功能"。修法是再套一层
`#if (ARKRTOS_USE_FLASHAPP == 1)`。**帮助文本也是断言**：它声明的东西必须真的存在。

### ⑭ 分区是静态决定，不跟着功能开关走

`ARKRTOS_FLASHAPP_SIZE` 默认就是 `IMAGE_POOL_SECTOR`，**开关关着也照样预留**
（代价：池少一个扇区，固件 +16 B）。这不是疏忽：`.sct` / `.ld` 的生成器与 C
读的是同一个头文件，如果这个值跟着开关变，同一个头文件就会对不同的工具说不同的
话——那正是本仓库"地址只在一个地方定义一次"那条规矩要防的事。真的不需要这块地，
就在编译命令行改，但必须同时把生成器指到同一份配置。

同理，关闭档的 16 B 差值是池单元数 10 → 9 带来的常数变化，不是代码残留：
`USE_FLASHAPP=0` 时 `arkrtos_flashapp.c` 根本不进编译单元（`sources.mk` 里有源文件，
但 `#if` 与命令表都在 `arkrtos_features.h` 的闸门后面）。
