# Release 0.0.3-preview.1

- tag：`v0.0.3-preview.1`
- 版本号（固件内）：`0.0.3`（`ARKRTOS_VERSION_STRING`，`version` 命令可读）
- 发布类型：**预览版**。判据与分档见 [发行版与版本策略](发行版与版本策略.md) §5。
- 上一份能力记录：readme「许可与版本」里写的 `0.0.2`（**从未打过 tag**，本版是仓库
  第一个 tag）。本版没有回退 0.0.2 记录里的任何能力。

一句话：**这是一个能装、能跑、能换版本、能崩了自救、且在四套工程上都编得干净的内核
预览版**；缺的是「对外承诺稳定」的那几条边界，全部列在 §5。

## 1. 本版变更

### 1.1 构建与配置：Kconfig 成为唯一取值来源（`d9be839`、`6c2e83d`）

| 项 | 之前 | 现在 |
|---|---|---|
| 功能开关写在哪 | Keil 工程的 `<Define>`（5 个强开宏压过 Kconfig） | 工程里只留 `USE_HAL_DRIVER` / `STM32F4xx` / `ARKRTOS_BOARD_CONFIG` |
| 共享头 `arkrtos_autoconf.h` | 手写，与 `arkrtos_defconfig` 漂移 | 由 `config/arkrtos_defconfig` 生成，`ci_gate` 的 `config-generated` 逐字节比对 |
| 板级差异 | 靠工程宏 | `board/<chip>/arkrtos_board_config.h` 的 `#undef` + `#define 0` |
| GCC 工程 | `DEFS` 里堆功能宏 | 每次构建生成 per-build 头，`-I` 放最前遮蔽共享头；`make USE_SHELL=0` 这类旋钮照旧可用，改旋钮自动重编 |
| defconfig | 1 个 | 5 个：`arkrtos`（共享头来源）/ `stm32h743` / `stm32f427_gcc` / `minimal` / `minimal_noshell` |

顺手修掉的真缺陷（都不是重构，是原来就错）：

1. **软总线端口胶合层关不掉**：`ark_bus_hal_arkrtos.c`（vendored）没有开关守卫，
   `ARKRTOS_USE_SOFTBUS=0` 时仍引用三个钩子 ⇒ 链接失败。修法是在 ark_softbus
   **源仓库**加守卫再同步（`1c3c27c`），不直接改 vendored 副本。
2. **`arkrtos_utest.h` 被塞在 `#if ARKRTOS_USE_MEMPOOL` 里**，而使用点在
   `#if ARKRTOS_USE_UTEST` 里 ⇒ 两个开关取值不同就编不过（最小档 1 Error）。
3. **`Kconfig.features` 结构损坏**：两处 `endmenuconfig` 合并行、UTEST 块缺
   `bool/default` 被解析成 `n`——这是「F427 Keil 体积莫名少 6738 B」的根因。
4. **四道 ON 档门禁的输入环境不全**（`lwip_port.h` / `ark_bus.h` 找不到）：新增
   `tools/arkrtos_f427_env.py` 承载 F427 工程真正的头路径与 `-DARK_BUS_*` 档位宏。

### 1.2 能力与真机证据

本版收录的每一条都有上板读数，逐条的判据和反假绿写法在对应文档里：

| 能力 | 证据 | 提交 |
|---|---|---|
| tickless idle | 中断降到 **27.6%**（1/3.6），静置 20 s 涨 40422 拍 = 20.2 s 不丢拍；`rt` 新增一行读数，避免「开关没生效」和「没变慢」长得一样。**电流未测** | `908d56e` |
| mempool 的 App/驱动 SVC 通道 | `MPOOL_DEMO` 33 条用例全过，块地址落在 App RAM 内；六类非法输入逐条被拒 | `042e34f` |
| 小程序计划内交接 | 持 `led2` 从 `main` 正常 `return`：`handover=1`、`neutralized=0`、`led2` own 条目摘除 | `d2496b0` |
| A/B 热切换（默认关） | 四条结论通过；「数据段不重置」这条原设计说法**被实测推翻**并已改文档 | `0159417`、`82e6f42` |
| 崩溃台账跨复位 | 崩 3 次禁用 + `reboot` 后仍 held + 槽位下标漂移后仍能接管 | `90f8d2d` |
| 安全基线自检 `sec` | 12 项只读检查（pass 8 / fail 0 / finding 2 / skip 2），T1–T12 全部有读数 | `2d0c41f`、`89941f2`、`34abade` |
| 选项字节真值 | `arkrtos_port_flash_protection()` 读出 `rdp=0`、`wrp lo/hi=0x0000`，与调试器直读 `OPTCR` 一致 | `9d5f7ae` |
| 实时性度量 | DWT 周期计数测四项延迟 + `rt` 命令 + 门禁 | `26331f6` |
| 流缓冲 / 消息缓冲 | F427 开档 11/11 用例、104 条断言；真中断里 `send_from_isr` 唤醒阻塞读者（2 拍 = 1.0 ms） | `2356fcf`、`8a1941e`、`ccc668c`、`11ed851` |
| 板上编译与持久化（arkc / arkmake） | M1 直跑、M2 可重定位四遍差分、M3b QSPI 持久化、M3-d 跨复位自启、M4 hello world | 见 `docs/arkc板上直跑-*.md` |
| 软总线软件层 | 帧认证 / 载荷加密 / 防重放 / 握手会话密钥 / 网状路由 / Raft / 签名固件，**全部默认关、零体积零行为变化**；板上 loopback `checks 56 failed 0` | `docs/软总线接入.md` |

### 1.3 可读性与工程账

| 项 | 结果 | 提交 |
|---|---|---|
| `SVC_Server` 巨型 dispatch | 1318 行 → 58 行表 + 17 个 handler；`Code` +408 B（+0.19%），行为已上板复验 | `cb3690f` |
| SVC 服务号 / 子命令号具名化 | 17 个服务号 + 约 105 个子命令号进 `arkrtos_svc_abi.h`；内核 108 处、SDK 111 处替换，值逐处不变 | `4796ab1` |
| 返回码具名化 | 26 个模块、397 处替换，值逐处不变；物理上全仓只剩两处刻意保留的裸 `-1` | `06c4a83`、`8ee8371` |
| 例程入口统一 | 16 个 app_sdk 例程 + 4 个 mini 示例统一为 `Src/main.c` | `ef8054c` |
| 欠账可核对化 | 新增 [已知限制](已知限制.md) + [欠账盘点](欠账盘点.md)（每条带核对命令与判据），`ci_gate` 加 `doc-index` 门禁 | `aaec450` |
| 发布机制成形（本版新增） | 版本号 0.0.2 → `0.0.3`；新增 [发行版与版本策略](发行版与版本策略.md)（版本号规则、发布物清单、ABI 升版判据、预览版/正式版/1.0 判据、发版六步）；新增 `tools/release_check.py` 并在 `ci_gate` 里成为 `release-consistency`（三处版本一致 + 发布说明存在且带「未验证」节 + 最新 tag 与版本号对齐） | 本版提交 |

## 2. 体积账本（2026-11-14 实测，可复跑）

F427 内核工程，Flash = Code + RO + RW，RAM = RW + ZI：

| 档位 | Code | RO-data | RW-data | ZI-data | Flash | RAM |
|---|---|---|---|---|---|---|
| 默认档 | 220094 | 19198 | 896 | 120852 | 240188 B ≈ 234.6 KB | 121748 B ≈ 118.9 KB |
| 最小档（Shell 仍开） | 63110 | 8350 | 388 | 43172 | 71848 B ≈ 70.2 KB | 43560 B ≈ 42.5 KB |
| 最小档 + Shell 关 | 36886 | 1446 | 332 | 28608 | 38664 B ≈ 37.8 KB | 28940 B ≈ 28.3 KB |

其余三套工程（默认配置）：

| 工程 | 结果 |
|---|---|
| F401 Keil | `116578 / 13034 / 476 / 53472` |
| F427 GCC | `text 259412 / data 368 / bss 113536`，FLASH 99.10% |
| H743 GCC | FLASH 448604 B（85.56%）/ RAM 443300 B（98.39%）/ CCMRAM 22732 B |

> 三档体积只代表「编译 + 链接通过」，没上板跑过；功能是否完整只是「能链接」级别的证据
> （见 [功能开关](功能开关.md) 同一段落的说明）。

## 3. 复现

```bash
# 门禁（29 项，含编码/行尾、config 一致性、开关声明、9 条宿主测试、4 道 ON 档语法门禁、release-consistency）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/ci_gate.py

# 版本一致性（版本头 / 中英 README / 发布说明 / tag）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/release_check.py

# Keil：F427 / F401 内核工程（-r 全量重建）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -r \
  example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx -j0 -o C:/tmp/f427.log

# GCC：F427 / H743（CROSS 必须给，否则 make (e=2)）
cd example/stm32f427/kernel/ARKRTOS_TEST/GCC && make -j4
cd example/stm32h743/kernel/ARKRTOS_TEST/GCC && make -j4 CROSS=/d/ST/STM32CubeIDE_1.16.1/STM32CubeIDE/plugins/com.st.stm32cube.ide.mcu.externaltools.gnu-tools-for-stm32.12.3.rel1.win32_1.0.200.202406191623/tools/bin/arm-none-eabi-

# 裁到最小档
env -u PYTHONHOME -u PYTHONPATH py -3 tools/kconfig/genconfig.py \
  --kconfig Kconfig --defconfig config/minimal_noshell_defconfig \
  --out <目标工程的 arkrtos_autoconf.h>
```

## 4. 兼容与升级注意

| 编号 | 本版值 | 与 0.0.2 记录相比 |
|---|---|---|
| `ARKRTOS_PARTITION_VERSION` | `10` | 未变 |
| `ARKRTOS_HW_COMPAT_ID` | `0x42700005`（兼容代次 5） | 未变 |
| SVC 服务号 / 子命令号 | `arkrtos_svc_abi.h` | **只改名不改值**，旧 App 二进制仍可装 |

- **旧 `.svcapp` / `.svcm` 镜像不需要重编**：本版没有动镜像头、共享分区表结构或
  SVC 编号的数值。
- **需要重新生成的是配置头**：如果曾把 `arkrtos_autoconf.h` 拷进过自己的工程，
  以本 tag 的 `config/arkrtos_defconfig` 重新生成一次（`ci_gate` 会比对该文件）。
- 工程宏清零后，**不要再往 Keil `<Define>` 里加功能宏**——会压过 Kconfig 且不被门禁发现。

## 5. 未验证（预览版的边界，不许省略）

功能边界：

- **真实以太网收发**：板级没有 PHY 驱动、MDIO、`eth_` 接口与 RMII 引脚分配，
  lwIP 跑的是回环 netif；**没有一条真实链路的 ping / 抓包证据**。
- **软总线物理线两节点**：只有板内 loopback（`checks 56 failed 0`）；板上仅一路
  USART1 且被控制台独占，两块板 / 板 + PC 的真实串口线**未验证**。
- **shell 认证** 与 **RDP/WRP 部署通路**：`sec` 能读出真实的 RDP/WRP 值，但设置
  （不可逆）与 shell 认证仍未做；正式签名需要产线把真实密钥写进构建。
- **tickless 的电流**：中断频率降下来了（27.6%），但**没有电流表读数**，「省电多少」
  这一格是空的。
- **Softbus 的认证/加密/路由/Raft/固件签名**：全部默认关，只有宿主与 loopback 级证据，
  没有多节点线上的实测。

工程边界（这些不阻塞「能用」，阻塞「能接手」）：

- 无远程 CI（门禁只在本机跑）、无静态分析（无 cppcheck / MISRA / clang-tidy）。
- 内核 22 个文件仍是 GBK（UTF-8 77 个），无 `.clang-format` / `.editorconfig`。
- 测试覆盖率：内核 39 个文件 / 828 个函数；宿主测试 9 条门禁、板端 utest 9 例
  （开 `SBUF` 后 12 例 / 115 条断言）。覆盖率工具未接。
- `kernelsrc/shell/arkrtos_shell.c` 仍是 6244 行单文件（拆分方案已成文、代码未动）。

逐条核对命令见 [欠账盘点](欠账盘点.md) 与 [已知限制](已知限制.md)。

## 6. 这一版不承诺什么

- **不承诺 ABI 稳定**：0.x 期间下一个 `MINOR` 可以动 SVC 面，届时会写「旧镜像需重编」。
- **不承诺「验证过的能力」以外的东西**：上面 §1.2 的表就是全集，没列的能力不要按
  「应该也能」推断。
- **不承诺长期支持分支**：还没有 LTS 的定义，`0.0.x` 只留 tag，不维护并行分支。
