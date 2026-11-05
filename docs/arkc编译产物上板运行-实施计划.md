# arkc 编译产物上板运行 — 实施计划

- 状态：**计划，未开工**（本文只描述要做什么、怎么判通过，不含任何已实施改动）
- 日期：2026-11-05
- 目标平台：STM32H743（`example/stm32h743/kernel/ARKRTOS_TEST/GCC`）
- 前置成果：板上 `cc`/`vim` 已能编译（28 KB 控制台栈定稿；见 `kernelsrc/components/arkc/VENDOR.md`）

---

## 0. 一句话目标

让**板上编译器刚刚编出来的那段机器码，在板上真的执行起来**，并且有判据证明「跑的就是刚编的那份」——不是别的东西、不是残留、不是恰好没崩。

---

## 1. 把「跑起来」拆成三层，分别验收

| 层 | 含义 | 关键属性 | 本计划阶段 |
|----|------|----------|-----------|
| **L1** | 固定窗口直接执行 | 无重定位、单实例、落点写死 | M1 |
| **L2** | 可重定位，落点任意 | 复用内核既有 `mini` 通路与重定位表 | M2 |
| **L3** | 常驻文件系统、可开机自启 | 掉电不丢、`mini autostart` | M3 |

三层必须**各自独立可验收**。L1 通过不推 L2，L2 通过不推 L3 —— 反过来也一样，L3 的持久化能力不能拿来替 L1 的执行证据。

---

## 2. 现在已经有的东西（全部已核实到代码行）

这一节是计划的成本基线：**容器、装载器、重定位、退出回收、SDK 骨架全都已经存在**，本轮不重造。

| 能力 | 位置 | 核实结论 |
|------|------|----------|
| 镜像格式 256 B 头 + 重定位表 + 负载 | `kernelsrc/include/arkrtos_app_image.h` | 字段顺序/CRC 口径/`runtime_ram_base` 明确；`TYPE_MINIAPP=3` 已定义 |
| 重定位打补丁（整字 + MOVW/MOVT 对） | `kernelsrc/src/arkrtos_loader.c:622` `reloc_movw()` / `:661` `reloc_apply()` | 只支持 4 种 kind：ROM/RAM 整字、ROM/RAM_MOVW |
| 装载六步（读头→读表→校验→借块→搬运+补丁→起任务） | `kernelsrc/src/arkrtos_mini.c:440..540` | 入口 `code_base + entry_offset`，`|= 1` 置 Thumb；`is_priv=0`；MPU 两窗口 |
| 伙伴池 64 KB @ `0x24070000` | `config/stm32h743/arkrtos_partition.h:125-132` | 块 1 KB…32 KB，2 的幂；`ARKRTOS_APP_NOMINAL_RAM_BASE = SLOT_RAM_BASE` |
| 退出自动归还两块 | `kernelsrc/src/arkrtos_task.c:2618` → `arkrtos_mini.c:777` | 应用调 `arkrtos_thread_exit()` 即释放，不会漏 |
| mini SDK（crt0 + main 骨架 + 说明） | `kernelsrc/sdk/mini_sdk/{arkrtos_mini_start.s, _gcc.s, arkrtos_mini_main.c, README.md}` | MDK 版跳 `__main`；GCC 版自己搬 `.data`、清 `.bss` |
| 宿主打包工具（四遍差分 + 表 + 头 + CRC） | `tools/pack_app.py`（`diff_reloc:414`/`verify_reloc:481`/`assemble:583`） | 支持离线输入 `--bin-a..d`，对工具链无依赖 |
| 板上 `cc` 输出 flat bin / ARKOBJ2 / 汇编 | `kernelsrc/components/arkc/arkc_shell_cmd.c:74` | 已有 `-S/-c/-E/-o/-O0..2/-D/-I`，带 `done in N ms` 计时 |
| 链接器多目标与 scatter/linker-script | 上游 `include/compiler.h:19-35`（`link_mode` + `link_inputs[32]` + `scatter_file` + `linker_script`） | **上游早就支持**，只是板上 shell 没暴露 |
| arkc 绝对地址全走 MOVW/MOVT | 上游 `src/backend/arm/backend_arm.c:34` `emit_mov_imm()`、`:4334`（`LDR =label` 也降 MOVW/MOVT，无 literal pool） | ⇒ 恰好只命中内核已有的 `reloc_movw`，**不需要新增 reloc kind** |
| `__CC_ARM` 预定义 | 上游 `src/preproc.c`（预定义宏表） | ⇒ `arkrtos_oslib.c` 走 `__svc` 分支，**一行不改**即可用 arkc 编译 |
| 全局初值/清零写成代码 | 上游 `backend_arm.c:2331` 生成 `__arkcc_init_globals()`；`:820` 只在 `Reset_Handler` 里自动 `BL` | 应用侧必须自己调（见 G4） |

---

## 3. 缺什么（每一条都有证据，不是猜测）

| 编号 | 缺口 | 证据 | 影响 |
|------|------|------|------|
| **G1** | 板上 `cc` 无法改链接基址 | `arkc_shell_cmd.c` 的 `cc_usage()` 与 argv 解析里**没有** `--scatter`/`-T`（宿主 `src/main.c:433/437` 有） | 产物只能链接在 `0x08000000`，而 mini 要在 RAM 执行。**M0 追加**：宿主侧的 `--scatter` 当时也是空转（见 §3.2），所以这不是「只补选项解析」那么小 |
| **G2** | 板上 `cc` 拒收多个输入文件 | `arkc_shell_cmd.c`：遇到第二个非选项参数直接报 `more than one input file` | 无法把 crt0 + main + oslib 三个目标链接起来 |
| **G3** | 板上 `cc` 不会打包 | 只说 `-o x.bin`；无头、无重定位表、无 CRC | 产物不能被 `mini run` 接受 |
| **G4** | 应用侧没有 crt0 | arkc 只在 `Reset_Handler` 里自动 `BL __arkcc_init_globals`，且**没有 .data/.bss 时该函数根本不生成** | crt0 直接调会链接失败；不调则全局变量是垃圾 |
| **G5** | `mini` 只从 `arkrtos_fs` 卷读 | `arkrtos_mini.c:192` `arkrtos_mini_read_exact()` → `arkrtos_fs_read_at()`（唯一读点） | H743 没有卷（ramfs 在 `ark_vfs` 层，是另一套接口） |
| **G6** | 板上链接表上限 64/64 | `GCC/Makefile`：`-DARK_LINK_MAX_SYMS=64 -DARK_LINK_MAX_RELOCS=64`；`LdSym` = 136 B（`name[128]`） | 抬到 256 要 +34.8 KB RAM，而内核 RAM 已 97.50%（剩 ~11 KB）⇒ **不能靠抬表解决** |
| **G7** | 关键数字全未实测 | — | **M0 已量**：板上编译 46–48 ms、宿主/板上产物逐字节相同、单文件直编不走链接符号表（§3.2） |
| **G8** | 板上 ramfs 单文件上限 16 KB | `GCC/Makefile`：`-DARK_RAMFS_FILE_CAP=16384`；实测一份 2.2 KB 源程序的 `-S` 清单 18 801 B，`cc -S -o /tmp/x.s` 直接 `cannot write output file` | `-S` 清单只能走控制台；中间产物不能同时留多份 |
| **G9** | 板上 ramfs 只有 3 个槽 | `-DARK_RAMFS_MAX_FILES=3`；实测 `/tmp` 目录本身占一个（目录 + 2 文件即满） | M2 的「源码 + 临时 + 产物」正好卡满，第四份一律失败 |

### 3.2 M0 实测结论（2026-11-05，全部现场读数，不改任何结论性判词）

M0 做的第一件事就是量，量完推翻了自己计划里的三条假设。三条都记在这里，
因为它们决定 M1/M2 怎么写：

| # | 原假设 | 实测 | 影响 |
|---|--------|------|------|
| 1 | 「不改上游 arkc 一行」 | **推翻**。① `arkrtos_oslib.c` 有 9 处 `ARKRTOS_SVC_DECL_*(…);`（宏自带 `;` + 调用点 `;`）⇒ 顶层空声明，arkc 报 `expected declaration at top level`，armcc/GCC 都接受；② `--scatter` 对 codegen **完全无效**（两份只差 `LR_IROM1` 的 `.sct` 编同一源，产物逐字节 0 差异） | 上游改两处，已 `sync_arkc.py` 同步（见下） |
| 2 | 「crt0 序言是 `PUSH {r7,lr}`」 | **推翻**：实测首条指令是 `0xB5E0` = `PUSH {r5,r6,r7,lr}` | 打包校验按 0xB5E0 写，不按记忆写 |
| 3 | 「多文件链接可行，只要符号数 < 64」 | **部分推翻、部分修正**。四个 obj（含 `arkrtos_oslib.c`）链接成功但 payload **33 384 B**；把 oslib 拿掉后两个 obj 只有 **3 148 B**（含字符串/全局/svc 的真实用例 **3 880 B**）。⇒ 体积爆炸的真凶是 **oslib**：它本身在板上编就 `FATAL: memory pool exhausted`，链接进来又拖一份 runtime | 程序形状**默认自包含单文件**（2 272 B，最省也最稳）；多 obj 作为备选可行（crt0 + app，< 4 KB），但**禁止用 oslib**，SDK 侧只给「瘦头文件」（纯 `__svc` 声明，无实现） |

上游两处修复（改在 `D:\工作\git_project\ark c compiler`，已同步进
`kernelsrc/components/arkc`）：

- `src/parser.c`：顶层循环跳过孤立 `TOK_SEMICOLON`（C11 6.8.2 允许空声明，
  armcc/GCC 接受，arkc 不接受就是兼容性缺陷）。
- `src/backend/arm/backend_arm.c`：`.sct` 分支只置了 `g_scatter_loaded = 1`
  却**从不推导** `g_flash_base/g_data_base/g_bss_base`（只有 `-T` 分支推导），
  于是 `--scatter` 解析成功、地址全用默认值 —— 静默空转。现抽出
  `arm_scatter_derive_bases()`，两个分支共用。

修好之后的实测（`m0_single.c`，含函数指针 + 带初值全局 + 字符串 + `__svc`）：

| 基址组合 | 产物大小 | 与基线差异 | 读到什么 |
|---|---|---|---|
| 默认（无 `--scatter`） | 2134 B | — | 代码 `0x08000000`、数据 `0x20000000` |
| `LR_IROM1 0x08004000` + `RW_IRAM1 0x24070000` | 2134 B | 57 字节 | 函数指针 `=bump` → `MOVW #0x4000 / MOVT #0x0800`；全局 → `MOVT #0x2407` |
| 同上但 `LR_IROM1` 改 `0x08014000` | 2134 B | **1 字节**（高半字 0x0800→0x0801） | 正是「整字 +delta」该有的形态 |

**`-link` 的一个静默坑（实测）**：`-link` 只收集**紧随其后**的非选项参数。
写成 `arkcc a.obj b.obj -link -o x.bin` 时两个 obj 被当普通输入，产出 **0 字节**
且不报错；正确写法是 `arkcc -link a.obj b.obj -o x.bin`。打包工具内部拼命令行
必须按后者，并且**校验输出非空**——0 字节不能当成「链接成功」。

**一条必须写死的口径**：差分不能用 **0** 当基址。基址为 0 时 arkc 会把
`MOVW+MOVT` 折叠成单条 `MOV`（高半字为零），产物从 2272 B 变 1918 B，
差分出来不是地址而是整个代码布局 —— 差分会得出错误的表。基址必须
**非零且按 4 字节对齐**，delta 取 0x10000 这一级。

板上读数（`m0_board.c`，653 B 源）：

| 项 | 读数 |
|---|---|
| 宿主 vs 板上 bin | **逐字节相同**：2260 B 全等（板上 `cat` 的 `\xNN` 转义解码后比对，最长公共前缀 = 全文长度） |
| `cc -o bin` 耗时 | 46 / 47 / 46 ms（-O1 三次），-O0 48 ms，-O2 46 ms |
| `cc -c` 耗时 | 47 ms（-O1）/ 46 ms（-O2） |
| 池占用 | 66 248 / 81 920 B |
| `cc -S -o -` | 1549 ms —— **不是编译时间**，是 18.8 KB 清单走 115200 UART 的时间 |
| `cc -S -o <file>` | 不可用：清单 18 801 B > `ARK_RAMFS_FILE_CAP` 16 384 B（G8） |

### 3.1 由 G6 推导出的硬约束

mini 程序的符号数必须压在 64 以内。**推论**：应用必须**单文件或少文件**，且不能携带一套完整的 C 库。这条约束会直接决定 M1/M2 的程序形状，因此**M0 第一件事就是量它**，量出来再决定形状，而不是先写代码再发现链接不了。

---

## 4. 阶段计划

每个阶段都给：改点、开关、判据、反假绿、账本。**任何阶段不通过就不进下一阶段。**

### M0 — 量化前置（**已完成，读数见 §3.2**）

不改板上代码，只把后面所有决定的输入量出来。下表保留原始判据，右列是实测结果。

| 项 | 做法 | 判据 |
|----|------|------|
| M0-1 符号数/重定位数 | 宿主 arkc 编「单文件 mini 程序」（含 1 处串口写 + 1 个带初值全局 + 1 个字符串字面量 + 1 处 `__svc` 调用），看链接是否 `ld_table_overflow` | **结论反转**：单文件直编根本不走链接符号表 —— 把 `ARK_LINK_MAX_SYMS` 降到 1 仍然成功且产物不变。⇒ 64/64 只约束「多 obj 链接」，而多 obj 链接体积爆炸（33 384 B），所以程序形状定为自包含单文件，符号上限对它无约束 |
| M0-2 板上编译耗时 | 板上 `cc` 同一文件，读 `done in N ms` | **T = 46–48 ms**（编 + 汇编 + 落盘）。M2 四遍代价上限 ≈ 4T ≈ 190 ms，板上完全可以接受 |
| M0-3 宿主/板上编译等价 | 同一源文件分别在宿主与板上 `cc` 出 bin，逐字节比对 | **必须完全相同**；不同就是「板上编译器不可信」，本计划整体停手（先修编译器） |
| M0-4 crt0 形状定稿 | 写 `arkrtos_mini_start_arkc.c`（C 语言 crt0，见 §4.1） | 已产出；首条指令实测 `0xB5E0` = `PUSH {r5,r6,r7,lr}`（**不是** `PUSH {r7,lr}`），打包校验按此写 |
| M0-5 基址与 delta 定值 | 选出 nominal 链接基址（代码块基址）与差分 delta | **已完成**：`--scatter` 现在真的改 codegen；换基址 +0x10000 只动 MOVT 高半字（整字 +delta 形态）。口径：基址**不得取 0**（会折叠成单条 `MOV`，产物 2272→1918 B，差分失真），须非零且 4 字节对齐 |

**M0 的反假绿**：M0-3 必须真的逐字节比，不能只比大小、不能只比 CRC ——CRC 相同但字节不同在理论上可能，而这里要的是「编译器等价」这个强结论。

### 4.1 crt0 的设计（不靠改上游 codegen）

写成 C（arkc 不接受汇编输入），三件事：

```
void MINISTART(void)
{
    __arkcc_init_globals();      /* extern 声明；由 arkc 在存在 .data/.bss 时生成 */
    MiniMain();                  /* SDK 骨架：返回后调 arkrtos_thread_exit() */
    arkrtos_thread_exit();
}
```

**G4 的解法（关键）**：arkc 在「没有 .data/.bss」时不生成 `__arkcc_init_globals`，此时链接会明确报未定义符号——这符合「宁可报错」。而 SDK 骨架里放一个**非零初值的锚点全局**（`volatile unsigned g_mini_sdk_anchor = 0xA5A5A5A5u;`）即保证 `.data` 非空、该函数必生成。**因此 crt0 不需要上游配合**。注意：这与 §3.2 记的两处上游修复不矛盾 —— 那两处是「arkc 与 armcc 的兼容性缺陷」（空声明、`.sct` 空转），与本节的 `__arkcc_init_globals` 生成条件无关。

入口偏移的取得：约定 crt0 是**第一个链接输入**的第一段 ⇒ `entry_offset = 0`；打包时**校验** payload 前两条指令等于 crt0 的固定序言，不等于就拒绝出镜像（不做「猜一个入口」）。

### M1 — L1 固定窗口执行（预计 1 天）

最小闭环：板上编出来的 bin，被读进固定 RAM 窗口直接执行。

| 项 | 内容 |
|----|------|
| 改点 1 | `arkc_shell_cmd.c`：`cc` 增加 `--scatter <file>`（写入 `opts.scatter_file`）与 `-link <obj>...`（写入 `opts.link_mode/link_inputs`）。**只碰 ArkRTOS 侧接线文件，不碰上游** |
| 改点 2 | 新组件 `kernelsrc/src/arkrtos_apprun.c` + 开关 `ARKRTOS_USE_APPRUN`（默认 **0**）：`app run <path>` 读文件到固定窗口 `SLOT_RAM_BASE` 起 `APPRUN_WINDOW_SIZE`，建非特权任务 + MPU 两窗口，退出回收 |
| 改点 3 | mini SDK 增 `arkrtos_mini_start_arkc.c`（§4.1）与一小段 README |
| 开关纪律 | `USE_APPRUN=0` 时**不编该源文件**，FLASH/RAM 与当前逐字节相同（make 里按 `USE_ARKC` 同样的写法控制） |
| 判据 A | `cc /tmp/hello.c --scatter /tmp/app.sct -o /tmp/hello.bin` → `app run /tmp/hello.bin` → 串口出**源码里独有的字符串**（不是残留日志） |
| 判据 B | 程序自己调 `arkrtos_thread_exit()`，退出后 `fault` 为空、`task` 里无残留、**再跑一次仍正常**（证明窗口回收干净） |
| 判据 C | 故意把 `--scatter` 里的基址改成另一个值但不重新链接 → 应崩或明确报错，**不能**恰好跑通（证明判据对地址敏感） |
| 反假绿 | ① 改一行字符串重新编、重跑，串口内容必须变；② 跑一个 `while(1)` 程序确认它是真在跑（不返回）而不是立刻返回；③ 关掉 `use apprun` 的构建里 `app` 命令不存在（开关真的生效） |
| 账本 | FLASH/RAM 前后对比（必须只在 `USE_APPRUN=1` 时变化）；`SLOT_RAM` 窗口大小与 `APPRUN_WINDOW_SIZE` 的关系要写进注释 |

**M1 明确不做**：不做重定位、不共享伙伴池（同一时刻只允许一个固定窗口实例，命令里要拒绝第二个）。理由：L1 的价值是**最早拿到「板上编译器产出的机器码在板上执行」的证据**，把它和重定位混在一起，出问题时分不清是入口错、基址错、还是补丁错。

### M2 — L2 可重定位，走既有 mini（预计 2–3 天）

把 `pack_app.py` 的四遍差分搬到板上（C 实现），产出标准 `.svcm`，由**未修改**的 `mini run` 装载。

| 项 | 内容 |
|----|------|
| 改点 1 | 新组件 `kernelsrc/src/arkrtos_apppack.c`：四遍链接（A 标称 / B ROM+delta / C RAM+delta / D 双 delta 验证）→ 差分出表 → 组装 256 B 头 + 表 + 负载 + CRC → 写文件 |
| 改点 2 | `cc` 增加 `--pack -o x.svcm`（内部调 apppack）；`--pack` 与 `-S/-c/-E` 互斥，冲突即报错 |
| 内存纪律 | payload 上限受 `ARK_CODE_BUF_SIZE=16384` 与 `ramfs` 单文件 16 KB 双重约束；A 落 `/tmp`，B/C/D 复用同一个临时文件，**不引入第二份 16 KB 静态缓冲**（RAM 只剩 ~11 KB，这是硬约束不是偏好） |
| 内核侧 | **零改动**：走 `arkrtos_loader_reloc_apply()` |
| 判据 A | `cc /tmp/hello.c --pack -o /tmp/hello.svcm` → `mini run /tmp/hello.svcm` → 出字、退出、`mini stat` 显示块已归还 |
| 判据 B | 同一镜像在两个不同落点各跑一次（先跑一个占用块、再跑它）→ 两次都正常，输出一致 |
| 判据 C | 程序里含：1 个带初值全局、1 个字符串字面量、1 个函数指针、1 处跨文件调用 —— 四类绝对地址各至少命中一次 |
| 反假绿 | ① 打包后**故意删掉一条重定位表项**，重新装载 → 必须崩或行为异常（若仍正常，说明判据没测到那个地址）；② 打印表项数与类别分布，ROM/RAM/整字/MOVW 四类若某类恒为 0，要解释清楚（否则等于没验证那一类）；③ `verify_reloc` 的 D 遍必须真的跑并且**不匹配就拒绝出镜像** |
| 账本 | 表项数 × 4 B 对镜像大小的影响；4 遍总耗时（M0-2 的 T 的倍数）；ramfs 槽位占用（源码 + 临时 + 产物 = 3 槽，正好卡在 `ARK_RAMFS_MAX_FILES=3`，要写明「源码与产物不能同时留第四份」） |

### M3 — L3 镜像有地方常驻（预计 1–2 天）

| 子步 | 内容 | 判据 |
|------|------|------|
| M3-a | `mini` 读取后端可切换：新增 `arkrtos_mini_read_exact` 的 VFS 分支（开关 `ARKRTOS_MINI_READ_VFS`，默认 **0** 保持走 `arkrtos_fs`）；VFS 侧补一个门面 `arkrtos_vfs_read_at(path, buf, len, off, &got)` = `open_read` + `ark_vfs_lseek` + `read` + `close` | 默认关时 mini 行为与现在逐字节相同；开启后能从 ramfs 读 `.svcm` 并 `mini run` 成功。**注意 ramfs 掉电即失，此步只解决「能不能读」，不解决持久化** |
| M3-b | QSPI W25Q64 + littlefs：dts 里 `&qspi` 现为 `status="disabled"`（`NCS=PG6`/`CLK=PF10`/`IO0..3=PF8/PF9/PF7/PF6`）；挂成 `/mnt/nor` | 断电重启后 `.svcm` 仍在；`mini autostart` 能起来 |
| M3-c | `arkrtos_vfs_mount_volume()` 的格式化路径要显式（不自动格盘） | 首次挂载未格式化卷要**明确报错**，不能静默格盘 |

M3-b 的风险最高（NOR 驱动 + 文件系统 + 未验证的引脚），所以排在最后，且 M1/M2 的结论不依赖它。

### M4 — 收口（0.5 天）

- `docs/` 补一节「arkc 产物上板运行」，`VENDOR.md` 交叉引用本文；
- 错误码表：链接表溢出、打包四遍不一致、入口签名不符、窗口被占、镜像超 cap —— 每条都要有**不同**的错误码，不能都退化成 `-1`；
- 回归脚本：把 M1-A/B/C 与 M2-A/B/C 做成 `final_app.py`（与既有 `final_acc.py` 同风格）；
- 尺寸账本：`USE_APPRUN=0/1`、`USE_ARKC=0/1` 四种组合的 FLASH/RAM 实测表；
- 记忆：板级坑（G1–G6）+ 三个用例写进 wiki。

---

## 5. 明确不做的事（及理由）

| 不做 | 理由 |
|------|------|
| 不新增 reloc kind | arkc 的绝对地址 100% 是 MOVW/MOVT 对（含 `LDR =label`），内核已有的 `reloc_movw` 正好覆盖。新增 kind 只会让两套打补丁逻辑并存 |
| 不为了「省事」改上游 arkc | 原写的「一行不改」被 M0 推翻（§3.2）：空声明与 `--scatter` 空转是**兼容性缺陷**，必须修，已修已同步。除此之外不再动上游：多目标链接、linker-script 上游都已有，缺的只是板上 shell 的选项解析 —— 那部分是我们自己的接线文件。动手前先跑 `sync_arkc.py --check`，它必须报「一致」 |
| 不把 `__arkcc_init_globals` 改成「总是生成」 | 用 SDK 里一个带初值的锚点全局即可让该函数必然生成，比改上游更省、更符合「不猜」 |
| 不把 `.data/.bss` 塞进代码块（RWX 单块） | 与已上板验证过的 MDK 通路不同形，会让「RAM 类重定位」这一类永远得不到验证；留作后续压缩块数的优化 |
| 不做 RAM 块设备给文件系统用 | 内核 RAM 97.50%，SDRAM 未接，做出来也是易失的假持久化 |
| 不动 `SHELL_TASK_STACK_SIZE` | 28 KB 已定稿实测 22 000/28 672 B，与本文无关，不要顺手改 |

---

## 6. 主要风险与停手条件

| 风险 | 触发信号 | 处置 |
|------|----------|------|
| R1 mini 程序符号数超 64 | M0-1 报 `ld_table_overflow` | 先缩程序形状（单文件、少库）；仍超则**停**，因为抬表要 34.8 KB RAM，与 97.5% 冲突，必须另立方案（例如让打包只输出 obj 段的**精简符号引用**）——不在本计划内 |
| R2 板上编译器与宿主不等价 | M0-3 字节不同 | **整体停手**，先修编译器。这之前的一切「跑起来了」都不算证据 |
| R3 arkc 产物里有非 MOVW/MOVT 的绝对地址 | M0 逐条扫 `-S`，发现 literal pool 或 `ADR` 型绝对引用 | 停，重新评估：可能需要新 kind 或改 codegen，成本级别完全不同 |
| R4 固定窗口与伙伴池冲突 | M1 判据 B（跑两次）失败 | 说明窗口没真正回收；不允许用「重启一次就好了」掩盖 |
| R5 时间预算 | M2 超过 3 天仍未通过判据 C | 缩范围：M2 只保证 M1 那一个程序可重定位，附上限制说明，不假装通用 |

---

## 7. 顺序与依赖

```
M0 ──→ M1 ──→ M2 ──→ M4
        └────→ M3-a（可与 M2 并行，因为只动读取后端）
                     M3-b（独立，最后）
```

- M0 是唯一的总闸门：R2 触发则全部作废，所以必须最先做、且结论要写死。
- M1 与 M2 的判据不能互相替代。
- M3-b 的完成度不参与 L1/L2 的结论。

---

## 8. 决定状态

**已决定（用户 2026-11-05 批复）**

1. 按本文顺序开工：M0 → 出结论后停下汇报 → M1（不一次做完）；
2. 上一轮 arkc 上板编译那批成果**不单独提交**，与本计划的成果合并提交（push 仍需单独获同意）。

**M0 之后仍待用户决定**

1. **M1 的 `app run` 是否接受**「单实例固定窗口」这个中间态 —— 它是把「编译→执行」这一段先钉死而引入的窄入口，M2 之后由 `mini` 取代；不接受的话，M1 就只能等 M2 一起做，首个体证会推迟 2–3 天。
