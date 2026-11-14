# 与成熟 RTOS 的对比：FreeRTOS / RT-Thread / Zephyr

> **口径**
> - **Ark RTOS 一列**来自本仓库现状（源码 + `docs/`），可逐条核对。
> - **其余三列**来自各自官方文档（页面见文末），只写稳定事实。
> - "缺"分三档：**定位差**（不打算做）/ **欠账**（该做、未做）/ **口径不同**（做了，但形态不一样）。
> - 与 Linux 的对比是另一份账，见 [与Linux差距盘点.md](与Linux差距盘点.md)；本页只管"另一个 MCU RTOS 有我有没有"。

---

## 一、先说定位：四个 RTOS 不在同一条赛道

| | FreeRTOS | RT-Thread | Zephyr | **Ark RTOS** |
|---|---|---|---|---|
| 出身 | Real Time Engineers（现 AWS） | 国内社区/商业双轨 | Linux 基金会 | 独立项目 |
| 主打 | **最小内核 + 移植面最广** | **组件化 + 软件包生态** | **厂商中立 + 板级覆盖**（450+ 板） | **让 MCU 像手机一样装/升/卸应用与驱动** |
| 内核形态 | 一堆 `.c`，宏配置 | 对象化内核 + 设备框架 | 内核 + 子系统 | 内核 + 特权隔离 + 安装器 |
| 抽象机制 | `FreeRTOSConfig.h` | Kconfig + SCons | **devicetree + Kconfig + west** | **Kconfig + DTS 子集（dtc_lite）** |

一句话：前三者的目标是"成为别人产品的底座"，Ark RTOS 的目标是"让同一块 MCU 上能
**后装**应用与驱动、并且装进来的东西崩了也伤不到别人"。后面所有差异都从这儿长出来。

---

## 二、逐维度对照

图例：**✓** 有 · **△** 有但口径不同 · **✗** 无 · **—** 不属于该 RTOS 的关注点

| # | 维度 | FreeRTOS | RT-Thread | Zephyr | Ark RTOS |
|---|---|---|---|---|---|
| 1 | 优先级抢占调度 | ✓ | ✓ | ✓ | **✓**（就绪集为 256 位两级位图 + 每优先级链，O(1)） |
| 2 | 时间片轮转 | ✓（可选） | ✓ | ✓ | **✓** |
| 3 | 事件 / 信号量 / 互斥锁 | ✓ / ✓ / ✓（优先级继承） | ✓ | ✓ | **✓**（互斥锁同样带优先级继承） |
| 4 | 消息队列 | ✓ | ✓ | ✓ | **✓**（`arkrtos_mq`） |
| 5 | 软定时器 | ✓ | ✓ | ✓ | **✓**（`arkrtos_timer`） |
| 6 | 流缓冲 / 消息缓冲 | ✓（V10 引入） | ✓（管道/ringbuffer） | ✓（k_pipe / k_fifo） | **✓**（`arkrtos_sbuf` 字节流 + `arkrtos_mbuf` 长度头消息；默认关，见 [流缓冲与消息缓冲](流缓冲与消息缓冲.md)。触发水位暂未提供） |
| 7 | 任务通知 | ✓（轻量同步，V8.2+） | — | — | **✗ 定位差**（事件 + 信号量已覆盖，不打算为省几字节再造一套） |
| 8 | **完全静态分配** | ✓（V9+，`configSUPPORT_STATIC_ALLOCATION`） | ✓ | ✓ | **✓（更彻底：内核不拥有堆，对象全静态；堆只存在于 App 侧静态 arena）** |
| 9 | 动态堆 | heap_1…heap_5 | memheap / mempool / 小内存管理 | k_heap / k_mem_slab | **△** 内核无堆；App 侧 POSIX 层有静态 arena |
| 10 | 固定块内存池 | ✓（heap_4 的合并策略 + heap_5） | ✓（mem pool） | ✓（k_mem_slab） | **✓** `arkrtos_mempool`（内核侧 + App/驱动经 SVC 0x20；存储区由调用者提供、额外 RAM 为零，见 [固定块内存池](固定块内存池.md)） |
| 11 | 低功耗 / tickless | ✓（tickless idle） | ✓ | ✓（device/runtime/system PM，最完整） | **✓**（`ARKRTOS_USE_TICKLESS`，默认关；按延时链与软定时器夹住睡眠长度、相位结算防丢拍，见 [低功耗与 tickless](低功耗与tickless.md)。没有二次定时器/RTC，只做上限内合并；F427 上板实测中断降到 1/3.6、20 s 不丢拍，**功耗未测**） |
| 12 | 设备驱动模型 | ✗（只有 demo） | ✓（`rt_device` 框架） | ✓（devicetree 驱动模型） | **✓**（统一设备框架 + `arkrtos_dt` 按 `compatible` 找设备） |
| 13 | 配置系统 | `FreeRTOSConfig.h` | Kconfig + menuconfig | Kconfig（+ west） | **✓**（Kconfig 子集，符号名=宏名） |
| 14 | 文件系统 / VFS | 外挂（+FAT） | DFS（FAT/littlefs/…） | LittleFS/FAT（子系统） | **✓**（littlefs 内置 + VFS + procfs） |
| 15 | 网络 | 外挂（+TCP） | lwIP / SAL / AT | 多协议栈 + BLE | **△**（lwIP 接入已就绪，`ARKRTOS_USE_LWIP` 默认 0；POSIX socket/select 兼容层已上板） |
| 16 | Shell / 日志 | ✗ | ✓（FinSH + ulog） | ✓（shell + log 子系统） | **✓**（内核 shell 控制台 + 分级日志 + 环形日志） |
| 17 | MPU / 特权隔离 | ✓（FreeRTOS-MPU 版本） | ✓ | ✓（ARM MPU/TrustZone） | **✓**（SVC 特权隔离 + 逐任务 MPU 窗口，App/驱动独立固件） |
| 18 | 崩溃恢复 / 看门狗守卫 | ✗ | ✗ | △（需自己接） | **✓**（跨复位崩溃日记 + 连续崩溃禁用 + 每任务心跳合同 `guard`） |
| 19 | 硬件安全态（App 崩溃后驱硬件到安全态） | ✗ | ✗ | ✗ | **✓（独有）**（`hwsafe` 台账 + 可注册通道 + 遗言 `set_will`） |
| 20 | **运行期安装 / 卸载应用与驱动** | ✗ | △（`dlmodule` 动态模块） | ✗ | **✓（主线能力）**（镜像池 + 安装器 + 版本把关 + 无缝升级） |
| 21 | 文件系统里跑小程序 | ✗ | ✗ | ✗ | **✓（独有）**（`arkrtos_mini`，不装槽位、多实例） |
| 22 | 设备间互发现 / RPC / 流（软总线） | ✗（+TCP 另说） | △（软件包） | △（网络子系统） | **✓**（ark_softbus 组件，MCU 档已上板） |
| 23 | 单元测试框架 | ✗ | ✓（utest + CI） | ✓（ztest + Twister） | **✓**（板端 `utest`：用例自动注册 + 断言记账，`utest run` 上板 9/9；`utest-host` 门禁验框架本身的记账） |
| 24 | 支持的架构 | 极广（40+） | 广 | ARM/RISC-V/x86/ARC/… | Cortex-M3/M4/M7（RISC-V / LoongArch 抽象层已就绪，移植实现待补） |
| 25 | 主构建工具链 | 任意（Keil/GCC/IAR） | SCons / MDK | CMake + west | **Keil MDK（AC5/AC6）+ GNU Make / arm-none-eabi-gcc（F427 例程，见 §三-⑤ 与 [GCC构建指南](GCC构建指南.md)）** |
| 26 | **可裁剪下限（实测）** | 约 6–10 KB Flash（最小配置，社区口径） | 内核约 3 KB + 组件按需 | 约 8–20 KB（视设备树与子系统） | **37.8 KB Flash / 28.3 KB RAM**（`config/minimal_noshell_defconfig`，F427 内核工程实测；含板级 HAL 7.4 KB、演示应用 1.1 KB，内核本体 RAM ≈ 18.9 KB）。默认档 234.6 KB / 118.9 KB。见 [功能开关](功能开关.md) §"量到的最小档" |

---

## 三、缺口清单与处置

| # | 缺口 | 档位 | 处置 |
|---|---|---|---|
| ① | 流缓冲 / 消息缓冲 | **已补（触发水位除外）** | `arkrtos_sbuf`（字节流，send 可部分成功）+ `arkrtos_mbuf`（4 字节长度头，整条原子；接收缓冲不足整条丢弃而不是留半条）。阻塞/超时、ISR 发送、句柄生命周期与 mq 同一套。宿主 11 组断言 + `ci_gate` 门禁已过；H743 GCC 开档实测 +7792 B FLASH / +1248 B RAM，**F427 演示工程开档链接溢出**（那个工程只剩 4.8 KB）。阻塞/唤醒路径**未上板验证**。见 [流缓冲与消息缓冲](流缓冲与消息缓冲.md) |
| ② | 固定块内存池 | **已补** | `arkrtos_mempool`：固定块、零碎片、不依赖堆；错误口径为非法释放拒绝且不动池状态；shell `mpool` 自检 9/9 上板通过。**App/驱动侧通道已开**（SVC 0x20，池内存区在边界上校验必须落在调用者可见 RAM）。宿主 47 条断言 + `ci_gate` 门禁已过；默认档真实链接 +536 B FLASH（H743）/ +404 B（F427，98.32%）。**SVC 路径已上板（F427，2026-11-13）**：`MPOOL_DEMO.svcapp` 33 条用例全过，块地址落在 App 自己 RAM 内、六类非法创建全被拒；shell `mpool test` 9/9。见 [固定块内存池](固定块内存池.md) §七 |
| ③ | 低功耗 / tickless | **已补（F427 上板实测）** | `ARKRTOS_USE_TICKLESS`（默认关）：idle 时取「延时链最近到期」与「软定时器最近到期」的较小值，夹到 `ARKRTOS_TICKLESS_MAX_TICKS`（默认 8 拍）后把 SysTick 编成长周期，醒来一次补齐跳过的节拍。相位由软件记住，余数进下一拍——连续 40 次非整拍提前唤醒后累计拍数仍等于「总计数/每拍计数」，这一条在宿主门禁里是等式。默认档体积与行为零变化（拍一档是死代码）。`tickless` 门禁 = 宿主 152 条断言 + ON 档 armclang 真编译（tickless.c / task.c / main.c）。F427 上板实测（2026-11-13）：`rt` 报 `isr=18605 sleeps=18682 skipped=48728 of 67333`，中断降到 **27.6%**（约 1/3.6），静置 20 s 节拍涨 40422 拍 = 20.2 s（不丢拍）；开档 +648 B Code / +68 RO / +24 RW / +8 ZI。**电流未测**。见 [低功耗与 tickless](低功耗与tickless.md) §8 |
| ④ | 单元测试框架 | **已补** | 板端 `utest` 框架：用例自动注册、统一断言宏、上板 `utest run` 汇总（9 用例，F427 上板 9/9）；另有 `utest-host` 门禁用宿主 gcc 验框架记账。见 [单元测试框架](单元测试框架.md) |
| ⑤ | GCC / Make 样例工程 | **已补** | F427 的内核工程已带 GNU Make 构建（`example/stm32f427/kernel/ARKRTOS_TEST/GCC`），`sources.mk` 由 `tools/gen_gcc_sources.py` 从 uvprojx 生成；工具链、用法与三个坑见 [GCC构建指南](GCC构建指南.md) |
| ⑥ | 任务通知 | **定位差** | 不做，两条理由都是结构性的：①**语义是别家三种原语的合并体**——FreeRTOS 的 32 位通知同时充当二值/计数信号量、事件组和长度为 1 的队列，而这三样在 Ark 里分别是 `event`（位集，可从 ISR set）、`sem`、`mq`，再加一套就是第 4 种语义重叠的 IPC；②**省不掉开销**。通知之所以在 FreeRTOS 上"轻"，是因为它把等待者直接挂在任务控制块上；Ark 的用户态等待是"调用方登记 → 派发方 peek(only_kernel) → 汇聚点派发"这一条路径（App 侧不能碰内核结构），通知仍要走同一条登记路径，只多出一份每任务 32 位状态字与一套 API，换不来任何指令数 |
| ⑦ | SMP / 多核 | **定位差** | 不做，理由是"做了会拆掉主线"：Ark 的全部机制建立在**单核单一地址空间**上——全局 256 位两级就绪位图 + 每优先级链、一个 SVC 陷入口、逐任务 MPU 窗口、单一镜像池与单一安装器台账。SMP 要引入 per-CPU 就绪队列、IPI、跨核 spinlock、逐核 MPU 上下文，**每一条内核快路径都要加原子操作**，与"默认关时体积与行为零变化"直接冲突；而这些工作换不来"装应用/崩溃隔离"上的任何收益。另外 Cortex-M 的双核（如 H7 的 M7+M4）是 AMP 不是 SMP，真要支持那块板，需要的是核间通信与资源仲裁（可在 ark_softbus 上做），不是 SMP 内核 |
| ⑧ | 蓝牙 / 完整网络栈 | **定位差** | 不做第一方实现；已有 lwIP 接入点与 socket/select 兼容层，需要时外挂 |

> "定位差"不是"做不到"，而是**做了会偏离主线、且与现有能力重叠**，所以明确不做。

---

## 四、参考

- FreeRTOS 官方文档：<https://www.freertos.org/Documentation>
  （V9 引入 `configSUPPORT_STATIC_ALLOCATION`；V10 引入 stream buffer / message buffer；
  tickless idle 见内核配置章节；本仓库的对应实现见 [低功耗与 tickless](低功耗与tickless.md)）
- RT-Thread 文档中心：<https://www.rt-thread.io/document/>
  （utest 单元测试框架、FinSH/msh、设备框架、软件包）
- Zephyr Project：<https://docs.zephyrproject.org/>
  （devicetree、Kconfig、电源管理、ztest/Twister、支持的板卡数）

> 上表如与上游文档有出入，以上游为准；发现后请回来改这一页。
