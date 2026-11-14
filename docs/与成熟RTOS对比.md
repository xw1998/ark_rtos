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
| 3 | 事件 / 事件组 / 信号量 / 互斥锁 | ✓ / ✓（32 位）/ ✓ / ✓（优先级继承） | ✓ / ✓（`rt_event` 单标志）/ ✓ / ✓ | ✓ / ✓（`k_event` 32 位）/ ✓ / ✓ | **✓ / △ / ✓ / ✓**（`arkrtos_event` 是**单标志**广播复位；32 位事件集 `arkrtos_eset` 已实现但默认关（`ARKRTOS_USE_ESET`），OR/AND 掩码等待 + 可选 CLEAR，见 [同步原语与令牌守恒](同步原语与令牌守恒.md) §5.4；互斥锁带优先级继承，且沿等待链**多级传播**（§5.7）；可选**递归锁**创建入口（`ARKRTOS_USE_MTX_RECURSIVE`，默认关，§5.6）；另有**多对象同时就绪探测**（SVC 0x22，`ARKRTOS_USE_WAITMULTI`，非破坏性探测，§5.8）） |
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
| ⑥ | 任务通知 | **定位差** | 不做，两条理由都是结构性的：①**语义是别家三种原语的合并体**——FreeRTOS 的 32 位通知同时充当二值/计数信号量、事件组和长度为 1 的队列，而这三样在 Ark 里分别是 `event`（单标志）与 `eset`（32 位位集，可选开关，可从 ISR set）、`sem`、`mq`，再加一套就是第 4 种语义重叠的 IPC；②**省不掉开销**。通知之所以在 FreeRTOS 上"轻"，是因为它把等待者直接挂在任务控制块上；Ark 的用户态等待是"调用方登记 → 派发方 peek(only_kernel) → 汇聚点派发"这一条路径（App 侧不能碰内核结构），通知仍要走同一条登记路径，只多出一份每任务 32 位状态字与一套 API，换不来任何指令数 |
| ⑦ | SMP / 多核 | **仅骨架（不是多核支持）** | 调度器、就绪位图、延迟队列、对象表仍是**全局单核状态**，打开 `ARKRTOS_USE_SMP` 不会让两个核同时跑任务。骨架只补多核地基：每核状态表（`online`/`started`/`current_task_id`/`tick`/`ipi_count`/`idle_count`）、在线掩码、次核入口 `arkrtos_smp_secondary_main()`（未上线核只 WFI 空转）、IPI 通路（`arkrtos_smp_send_ipi()` + 弱符号收端）、`ARKRTOS_CPU_NUM` 1..8。`started`（启动请求被接受）与 `online`（核真在跑）分开记；单核 port 的 `arkrtos_port_cpu_start()` / `arkrtos_port_send_ipi()` **返回 -1 而不是假装成功**。默认关时体积与行为零变化（`Code=220290` 未变）。**验证档位=未验证（无多核板）**：宿主 44 条断言 + 5 文件 ON 档语法，无任何板级读数。真 SMP 要拆 per-CPU 就绪队列与逐核 MPU 上下文，属于另一件事，未做。见 [同步原语与令牌守恒](同步原语与令牌守恒.md) §5.10 |
| ⑨ | 同步原语细节（递归锁 / 多级 PI / 多对象探测 / 等待者容量） | **已补（未上板）** | 递归互斥锁 `arkrtos_mtx_create_recursive()`（SVC 0x15 子命令 17，`nest` 为 `uint8`，超 255 回绕）；优先级继承沿等待链多级传播（`A(3)→B(5)→C(7)`，宿主 5 条链式断言）；多对象同时就绪探测 `arkrtos_wm_probe_internal()`（SVC 0x22，非破坏性、返回 32 位掩码，带超时等待是 **1 ms 轮询**且「探测到→取用前被人拿走」的窗口不可消除）；等待者容量 `ARKRTOS_WQ_SLOTS` 可从 4 放大到任务表容量（`ARKRTOS_USE_WAIT_UNBOUNDED`，**不是链表式无上限**，真要解耦得改 46 处）。四项全部默认关零变化；宿主门禁 24 / 5 / 76 条断言。**递归锁与多对象探测已上板**（2026-11-14，F427，`utest` 11/11、110 条断言）；多级 PI 与等待者容量仍只有宿主/编译证据，后者开档在 F427 测试镜像塞不下（`L6406E`，108 error）。见 [同步原语与令牌守恒](同步原语与令牌守恒.md) §5.6–§5.9 |
| ⑩ | POSIX 读写锁 / 屏障 | **已补（已上板）** | 用户态组合实现（复用内核互斥锁 + 条件变量），零 SVC 改动；写者优先（读优先会饿死写者，故 `PTHREAD_PREFER_READER_NP` 返回 `ENOTSUP` 而不是接受后偷偷改语义）；屏障用 `gen` 支持复用。代价是占 `ARKRTOS_MTX_NUM`/`ARKRTOS_COND_NUM` 名额，表满报 `ENOMEM`。**上板验证过**（2026-11-14，F427，`POSIX_DEMO` 自检 `rwlock : PASS (0 violations)` / `barrier : PASS (arrived=2 serial=1 bad=0)`）；覆盖单任务锁状态机与 3 参与者屏障，多读者叠加重叠与写者饿死未测。该镜像声明 16 KB RAM（RW/ZI 4348 + 栈 4096 + 堆 2048 = 10492），固定槽窗口需 ≥ 16 KB 才装得下。见 [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) |
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
