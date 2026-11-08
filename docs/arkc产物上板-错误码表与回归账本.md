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

| 码 | `loader`（安装/App） | `mini`（文件里的负载） | `apprun`（固定窗口直跑） | `apppack`（打包期） |
|----|----|----|----|----|
| -1 | PARAM 参数非法 | 同 loader | 同 loader | PARAM 指针/窗口/长度非法 |
| -2 | MAGIC 镜像头魔数错 | 同 loader | 同 loader | — |
| -3 | COMPAT 硬件签名不符 | 同 loader | 同 loader | — |
| -4 | SIZE 长度非法/超空间 | 同 loader | 同 loader | — |
| -5 | CRC32 失败 | 同 loader | 同 loader | — |
| -6 | FLASH 擦写失败 | — | — | — |
| -7 | NO_SLOT 无空闲槽 | — | — | — |
| -8 | TASK 任务注册失败 | 同 loader | 同 loader | — |
| -9 | STATE 槽位状态不允许 | 同 loader | — | — |
| -10 | ADDR 落点非法 | 同 loader | — | — |
| -11 | ENTRY 入口偏移越界 | 同 loader | — | — |
| -12 | OCCUPIED 区间重叠 | 同 loader | — | — |
| -13 | RELOC 表非法/补丁后不一致 | 同 loader | — | — |
| -14 | NOSPACE 池内无足够空间 | 同 loader | — | — |
| -15 | BUSY 镜像在跑 | BUSY 实例数已满 / 有一次装载在进行 | — | — |
| -16 | VERSION 版本回灌 | — | — | — |
| -17 | DUP 重复副本 | — | — | — |
| -18 | SIGN 签名失败 | **FS：读镜像失败**（原因另见 `read: %d`） | — | — |
| -19 | — | DISABLED 崩溃到上限已禁用 | — | — |
| -20 | — | **SHORT_READ 读到文件尾** | **BUSY 已有一个在跑** | — |
| -21 | — | — | WINDOW 池里没拿到这块地 | — |
| -22 | — | — | PROLOGUE 首半字不是 arkc 序言 | — |
| -23 | — | — | LAYOUT 镜像不是给这个窗口链的 | — |
| -30..-34 | — | — | — | PARAM / DIFF / VERIFY / SIZE / ENTRY |

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
| M2 判据 C 的「跨文件调用」 | `-link` 已接线但没用过：`-link` 要「两份源 + 两份目标 + 散列文件 + 产物」六份，ramfs 只有 3 槽 | M3-b 之后（持久化 + 更多槽）再补 |
| 板上产物与宿主产物的 108 B 差异 | 同源码同 `-O1`：板上 3006 B、宿主 3114 B，两边各自自洽，**成因未查明** | 待查，不据此推任何等价性结论 |
| M3-b（QSPI W25Q64 + littlefs 持久化） | 未开始，风险最高，放在最后 | 独立阶段，M1/M2 结论不依赖它 |

---

## 6.1 板端没有 riscv32 后端（裁剪档）——RV 对象必须被拒，不能被当 ARM 补丁

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

判定：FLASH 82.17%、RAM/CCMRAM 未变，仍在预算内，**接受**；按档裁剪（让板端 profile
关掉复数支持以回收这 15.5 KB）需要给 irgen 与四个后端补闸门，登记为欠账而不是在这里
声称"可关"。

可复现判据：`git stash push -- kernelsrc/components/arkc` → 重建得 `text 412972`；
`git stash pop` → 重建得 `text 428532`。两次构建除 arkc 源码外的一切（CFLAGS、链接脚本、
其余目标文件）完全相同。
