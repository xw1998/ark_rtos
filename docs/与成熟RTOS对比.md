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
| 6 | 流缓冲 / 消息缓冲 | ✓（V10 引入） | ✓（管道/ringbuffer） | ✓（k_pipe / k_fifo） | **✗ 欠账**（见 §三-①） |
| 7 | 任务通知 | ✓（轻量同步，V8.2+） | — | — | **✗ 定位差**（事件 + 信号量已覆盖，不打算为省几字节再造一套） |
| 8 | **完全静态分配** | ✓（V9+，`configSUPPORT_STATIC_ALLOCATION`） | ✓ | ✓ | **✓（更彻底：内核不拥有堆，对象全静态；堆只存在于 App 侧静态 arena）** |
| 9 | 动态堆 | heap_1…heap_5 | memheap / mempool / 小内存管理 | k_heap / k_mem_slab | **△** 内核无堆；App 侧 POSIX 层有静态 arena |
| 10 | 固定块内存池 | ✓（heap_4 的合并策略 + heap_5） | ✓（mem pool） | ✓（k_mem_slab） | **✓** `arkrtos_mempool`（内核侧，shell `mpool` 自检已上板；App/驱动经 SVC 的通道待补） |
| 11 | 低功耗 / tickless | ✓（tickless idle） | ✓ | ✓（device/runtime/system PM，最完整） | **✗ 欠账**（idle 只 `WFI`，SysTick 常开；见 §三-③） |
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
| 23 | 单元测试框架 | ✗ | ✓（utest + CI） | ✓（ztest + Twister） | **✗ 欠账**（见 §三-④；已有 `tools/ci_gate.py` 门禁，但不是用例框架） |
| 24 | 支持的架构 | 极广（40+） | 广 | ARM/RISC-V/x86/ARC/… | Cortex-M3/M4/M7（RISC-V / LoongArch 抽象层已就绪，移植实现待补） |
| 25 | 主构建工具链 | 任意（Keil/GCC/IAR） | SCons / MDK | CMake + west | **Keil MDK（AC5/AC6）；GCC/Make 工程待补（见 §三-⑤）** |

---

## 三、缺口清单与处置

| # | 缺口 | 档位 | 处置 |
|---|---|---|---|
| ① | 流缓冲 / 消息缓冲 | **欠账** | 计划补：一个单读单写的字节流缓冲（带阻塞/超时与触发水位），面向 ISR→任务场景 |
| ② | 固定块内存池 | **已补（内核侧）** | `arkrtos_mempool`：固定块、零碎片、不依赖堆；错误口径为非法释放拒绝且不动池状态；shell `mpool` 自检 9/9 上板通过。App/驱动经 SVC 使用的通道尚未开放 |
| ③ | 低功耗 / tickless | **欠账** | 计划补：idle 时若下一次唤醒 > 1 tick，则推迟 SysTick 并进入低功耗，唤醒后补偿 |
| ④ | 单元测试框架 | **欠账** | 归入"按软件标准测试"专项：在门禁之外加一套板端用例框架（对标 utest 的形态） |
| ⑤ | GCC / Make 样例工程 | **欠账** | 独立专项：F427 的 GCC+Make 工程（内核 + App + 驱动 + 小程序） |
| ⑥ | 任务通知 | **定位差** | 不做：事件 + 信号量的组合已能覆盖，且新增一套 IPC 会稀释"装应用"这条主线 |
| ⑦ | SMP / 多核 | **定位差** | 不做：目标是单核 MCU 上的应用安装模型 |
| ⑧ | 蓝牙 / 完整网络栈 | **定位差** | 不做第一方实现；已有 lwIP 接入点与 socket/select 兼容层，需要时外挂 |

> "定位差"不是"做不到"，而是**做了会偏离主线、且与现有能力重叠**，所以明确不做。

---

## 四、参考

- FreeRTOS 官方文档：<https://www.freertos.org/Documentation>
  （V9 引入 `configSUPPORT_STATIC_ALLOCATION`；V10 引入 stream buffer / message buffer；
  tickless idle 见内核配置章节）
- RT-Thread 文档中心：<https://www.rt-thread.io/document/>
  （utest 单元测试框架、FinSH/msh、设备框架、软件包）
- Zephyr Project：<https://docs.zephyrproject.org/>
  （devicetree、Kconfig、电源管理、ztest/Twister、支持的板卡数）

> 上表如与上游文档有出入，以上游为准；发现后请回来改这一页。
