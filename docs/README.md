# Ark RTOS 文档目录

## 先读哪一份

| 我想…… | 看这份 |
|---|---|
| 把 App/驱动调试起来、装到板子上、处理崩溃 | **[Ark RTOS应用安装与调试指南.md](Ark RTOS应用安装与调试指南.md)**（操作手册，先读这份） |
| Read the same page in English | [Ark RTOS应用安装与调试指南.en.md](Ark RTOS应用安装与调试指南.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 搞懂安装策略、设备端配置区、槽位与 RAM 窗口 | [配置区与安装策略.md](配置区与安装策略.md)（配置记录格式 + 两种安装策略） |
| 用英文读同一份配置区与安装策略 | [配置区与安装策略.en.md](配置区与安装策略.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 把为 Linux/Windows 写的 C 程序搬到 Ark RTOS 上跑 | [POSIX与Windows兼容层说明.md](POSIX与Windows兼容层说明.md)（能力表 + 不支持项及原因） |
| 用英文读同一份 POSIX/Windows 兼容层说明 | [POSIX与Windows兼容层说明.en.md](POSIX与Windows兼容层说明.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想知道 App 调内核时那两层编号（服务号 / 子命令号）怎么定、怎么加一个新子命令 | [SVC服务号与子命令表.md](SVC服务号与子命令表.md)（19 个服务号、133 个子命令号（含网络 17，按两个 abi 头文件里的 `SUB_` 宏实测）、ABI 规矩、新加一处要动哪三处） |
| Read the SVC service/subcommand table in English | [SVC服务号与子命令表.en.md](SVC服务号与子命令表.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 内核函数返回的那个负数是什么意思、能不能改成别的值 | [返回码与错误码约定.md](返回码与错误码约定.md)（11 个模块 62 个 `ARKRTOS_*_ERR_*`、值逐处保持历史原样、三个哨兵、三条不得统一的既有契约） |
| 用英文读同一份返回码约定 | [返回码与错误码约定.en.md](返回码与错误码约定.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 在 App 里写 Linux 风格的 socket / select 程序 | [socket与select兼容层.md](socket与select兼容层.md)（名字与边界、SVC 0x1E 契约、阻塞在 App 侧兑现、SOCKET_DEMO、限制） |
| Read the socket/select compatibility page in English | [socket与select兼容层.en.md](socket与select兼容层.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想知道网络协议栈怎么接进来、现在能验证到什么 | [lwIP协议栈接入.md](lwIP协议栈接入.md)（lwIP 2.1.3 端口层、回环网卡、socket 底座、未接真实以太网的边界） |
| Read the lwIP integration page in English | [lwIP协议栈接入.en.md](lwIP协议栈接入.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 搞懂 MPU 怎么隔离 App、越权会怎样 | [内存保护与App越权验证.md](内存保护与App越权验证.md)（逐任务 region 配置 + APP_BAD 越权验证） |
| 用英文读同一份内存保护说明 | [内存保护与App越权验证.en.md](内存保护与App越权验证.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 带 MMU 的平台上想**像 Linux 那样直接执行可执行文件**（`exec /mnt/ram/hello a b` 或直接敲路径） | [MMU进程模型与ELF装载.md](MMU进程模型与ELF装载.md)（ELF64/ELF32 两遍法装载、`sp→argc` 入口约定、与 PLV3 探针抢同一页的取舍、**M1 只到「文件即程序」不是「进程」**、软定时器栈装不下陷阱帧的完整根因、RV32 无 MMU 档的 PMP 窗口装载（§9）、M2 欠账表） |
| 用英文读同一份 MMU/ELF 装载说明 | [MMU进程模型与ELF装载.en.md](MMU进程模型与ELF装载.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想**像 VxWorks 那样把可下载模块装进已跑着的系统**（`ld /mnt/ram/modhello.o a b`、`lkup`、`modules`、`unld`） | [MMU平台可下载模块-VxWorks式.md](MMU平台可下载模块-VxWorks式.md)（`ET_REL` 现场链接、内核符号导出表、支持的重定位类型、PLV0 取舍、**与 ELF 是并列的两种模型不是替代**、装载器要 4 KB 栈这个真 bug） |
| Read the same page in English | [MMU平台可下载模块-VxWorks式.en.md](MMU平台可下载模块-VxWorks式.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 搞懂文件路径怎么解析、挂载点怎么设、`/dev` 与 `/mnt` 从哪来 | [VFS路径命名空间.md](VFS路径命名空间.md)（挂载点、路径解析、根目录约定、设备与持久卷的位置） |
| 用英文读同一份 VFS 命名空间说明 | [VFS路径命名空间.en.md](VFS路径命名空间.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 查调度策略、这一轮调度器改了什么、还剩什么 | [调度器说明.md](调度器说明.md)（位图+链的 O(1) 就绪集、三种调度触发点、实测代价账与未做项） |
| 用英文读同一份调度器设计 | [调度器说明.en.md](调度器说明.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 看调度器在真机上长什么样（图） | 根目录 [`README.md`](../README.md) §调度表现（4.97 s trace 实测图，图存 `docs/img/`） |
| 读英文项目说明 | 根目录 [`readme_en.md`](../readme_en.md)（与中文 [`readme.md`](../readme.md) 对应，同一份内容） |
| 用串口控制台看状态、启停 App、改配置 | [内核Shell控制台使用说明.md](内核Shell控制台使用说明.md) |
| 用英文读同一份 Shell 控制台说明 | [内核Shell控制台使用说明.en.md](内核Shell控制台使用说明.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 搞清 Shell / 日志为什么长这样、和 Linux 差在哪 | [Shell与日志Linux化.md](Shell与日志Linux化.md)（命令风格、日志分级与出口、dmesg 环） |
| Read the Shell-and-logging page in English | [Shell与日志Linux化.en.md](Shell与日志Linux化.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 不想敲命令行，要图形界面 | `python tools/arkrtos_host_gui.py`（连接 / 控制台 / 安装 / 布局配置四个页签） |
| 用脚本 / 自动化流程驱动这块板子 | [`../skills/arkrtos/SKILL.md`](../skills/arkrtos/SKILL.md) 与《Ark RTOS应用安装与调试指南.md》§11 |
| 想在线把配置记录写进设备（不拆板、不用烧录器） | [配置区与安装策略.md](配置区与安装策略.md) §6.3（`tools/arkrtos_cfg.py`）与 §6.5（真机验收结论） |
| 想把板子上跑的过程录成 trace 并画成网页 | [`../skills/arkrtos/SKILL.md`](../skills/arkrtos/SKILL.md) §4.1（武装 / 采集 / 切段 / 渲染） |
| 搞懂同步原语怎么写、令牌为什么不会丢、`syncinfo` 怎么读 | [同步原语与令牌守恒.md](同步原语与令牌守恒.md)（三条契约 + 返回值/唤醒原因 + 排障口径） |
| 用英文读同一份同步原语说明 | [同步原语与令牌守恒.en.md](同步原语与令牌守恒.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 在板上用 arkc（shell 的 `cc`）编程序并**直接跑**（不安装、不占槽位、固定 RAM 窗口） | [arkc板上直跑-M1实机记录.md](arkc板上直跑-M1实机记录.md)（入口契约=向量表 + `--vector-table`、`app run`、四个错误码、优先级为什么在控制台之下、反假绿四条）与 [arkc编译产物上板运行-实施计划.md](arkc编译产物上板运行-实施计划.md) |
| Read the same page in English | [arkc编译产物上板运行-实施计划.en.md](arkc编译产物上板运行-实施计划.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| Read the same page in English | [arkc板上直跑-M1实机记录.en.md](arkc板上直跑-M1实机记录.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 让板上编出来的程序**装到池里任意空块**去跑（可重定位，`.svcm` 走 `mini run`） | [arkc板上直跑-M2可重定位镜像.md](arkc板上直跑-M2可重定位镜像.md)（四遍差分口径、五个静默错答案的坑、宿主对拍与反假绿五条、`cc --pack` 常量、体积账、**未做完的两条判据如实记账**） |
| Read the same page in English | [arkc板上直跑-M2可重定位镜像.en.md](arkc板上直跑-M2可重定位镜像.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 让板上编译出来的产物**掉电后还在**、复位后还能跑（外部 QSPI NOR + littlefs） | [arkc板上直跑-M3b-QSPI持久化.md](arkc板上直跑-M3b-QSPI持久化.md)（AF 权威表来源与逐脚映射、/WP /HOLD 单线坑、为什么读也走位敲、「新建文件被判 I/O 错误」这条只在 littlefs 上现形的内核缺陷、`fs put` 上传通道、24.8 s / 33.7 s 的速度账、跨复位自启尚未达成） |
| Read the same page in English | [arkc板上直跑-M3b-QSPI持久化.en.md](arkc板上直跑-M3b-QSPI持久化.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 板上一声「跑不起来」到底看哪个错误码、编进去多少、怎么一条命令复跑 M1/M2 | [arkc产物上板-错误码表与回归账本.md](arkc产物上板-错误码表与回归账本.md)（三条装载路径的错误码表 + 两处编号重叠、五类典型失败落点、六行尺寸账本、`tools/final_app.py` 四条用例与它自己踩过的四个坑） |
| Read the same page in English | [arkc产物上板-错误码表与回归账本.en.md](arkc产物上板-错误码表与回归账本.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 板上一份工程描述把几份 `.c` 打成一个能跑的镜像（`make`） | [ark make-板上构建工具.md](ark make-板上构建工具.md)（为什么不是 GNU make、五个键、四遍打包的三条硬约束、失败阶段怎么翻成错误码、关掉时零体积、这一轮抓到的两个真缺陷） |
| Read the same page in English | [ark make-板上构建工具.en.md](ark make-板上构建工具.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 从 NOR 上的工程目录 `make` 出一个**标准 C** 的 hello world（`<stdio.h>` + `printf` + `int main`），再 `mini run` 跑起来 | [arkc板上直跑-M4-hello演示.md](arkc板上直跑-M4-hello演示.md)（SDK 两源三头、`cd /mnt/nor/hello && make && mini run` 实录、`printf` 的 `__asm` 取参桩怎么绕开"没有 va_list"、七条硬限制、两条回归用例与 printf 宿主门禁） |
| Read the same page in English | [arkc板上直跑-M4-hello演示.en.md](arkc板上直跑-M4-hello演示.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 编辑器屏幕上的排版不对（`:q` 显示成 `::q`、光标狂闪、行被撕开） | [arkvim编辑器显示约定.md](arkvim编辑器显示约定.md)（五条显示约定的判据、host 三道门禁、板上 rawcap 取证的读法、改这个组件只能走上游） |
| Read the arkvim display-convention page in English | [arkvim编辑器显示约定.en.md](arkvim编辑器显示约定.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 加一个"放在文件系统里、想跑就跑"的小程序（不安装、不占槽位） | [小程序设计.md](小程序设计.md)（两块内存模型 + 装载六步 + 体积上限 + 多实例/崩溃禁用/开机自启清单 + 已知限制） |
| 用英文读同一份小程序设计 | [小程序设计.en.md](小程序设计.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 让两个小程序交接控制权、不停机换版本 | [无缝升级例程.md](无缝升级例程.md)（状态记录 + 请求记录两个文件、三条硬规则、SEAMLESS_V1/V2 实测） |
| Read the seamless-upgrade walkthrough in English | [无缝升级例程.en.md](无缝升级例程.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 让正在跑的任务原地换成另一个镜像（小程序两块 RAM / App 两块 Flash 槽，不加第三级缓冲） | [无缝升级-双缓冲热切换.md](无缝升级-双缓冲热切换.md)（异常栈帧改写四步、arm/take 两段式 API、三条拒绝边界、`mini swap` / `app swap`，以及 F427 上板实测：数据段"不重置"必须重新表述） |
| Read the same page in English | [无缝升级-双缓冲热切换.en.md](无缝升级-双缓冲热切换.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 一个镜像反复崩溃会怎样、装镜像前内核怎么把关 | [崩溃恢复与镜像版本把关.md](崩溃恢复与镜像版本把关.md)（跨复位崩溃日记 + 版本三态判定 + 明确不做签名/回滚） |
| 用英文读同一份崩溃恢复与版本把关说明 | [崩溃恢复与镜像版本把关.en.md](崩溃恢复与镜像版本把关.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| App 崩溃了，它控制过的硬件（电机/继电器）怎么被带离危险态 | [App崩溃硬件安全与遗言机制.md](App崩溃硬件安全与遗言机制.md)（遗言通道 A/B、两阶段中和、计划内交接 handover、`hwsafe` 命令） |
| Read the same page in English | [App崩溃硬件安全与遗言机制.en.md](App崩溃硬件安全与遗言机制.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 决定编什么功能、把上限改大改小 | [Kconfig配置系统.md](Kconfig配置系统.md)（符号名=宏名、依赖约束、零行为变化迁移） |
| Read the same page in English | [Kconfig配置系统.en.md](Kconfig配置系统.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 改一个上限 / 开一个功能 / 挪一段地址，到底该动哪个头 | [配置头导航.md](配置头导航.md)（七个配置头的职责边界、包含顺序、五条规则、四条门禁怎么盯住） |
| Read the configuration-header map in English | [配置头导航.en.md](配置头导航.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 6244 行的 shell 单文件该怎么拆、拆完怎么验 | [Shell命令拆分方案.md](Shell命令拆分方案.md)（只读盘点：命令族行段、跨族共享符号清单、三条硬约束、五步迁移、不做的理由） |
| Read the same page in English | [Shell命令拆分方案.en.md](Shell命令拆分方案.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 查有哪些编译期开关、关掉后调用方看到什么 | [功能开关.md](功能开关.md)（三层开关、逐项"关掉后"语义、真能关的验证方法） |
| 用英文读同一份开关清单 | [功能开关.en.md](功能开关.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 用字节流缓冲或定界消息缓冲传数据 | [流缓冲与消息缓冲.md](流缓冲与消息缓冲.md)（两种语义不可混用、返回码为什么避开 -4、开档体积实测） |
| Read the stream/message buffer page in English | [流缓冲与消息缓冲.en.md](流缓冲与消息缓冲.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 要在固定块上分配（零碎片、O(1)、不依赖堆） | [固定块内存池.md](固定块内存池.md)（存储区由调用者提供、拒绝口径、SVC 0x20 子命令） |
| Read the fixed-block pool in English | [固定块内存池.en.md](固定块内存池.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想让空闲时别每 500 µs 被叫醒一次（省电） | [低功耗与tickless.md](低功耗与tickless.md)（按最近事件夹住睡眠、相位结算为什么不会越睡越慢、上限与边界、验证档位） |
| Read the tickless-idle design in English | [低功耗与tickless.en.md](低功耗与tickless.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 要看实时性到底多少、最坏延迟多大 | [实时性与最坏延迟.md](实时性与最坏延迟.md)（DWT 周期计数器、四项读数口径、上板实测、三条边界、调试器清 TRCENA 的坑） |
| 用英文读同一份实时性说明 | [实时性与最坏延迟.en.md](实时性与最坏延迟.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想知道这套构建的安全基线还在不在（12 项只读自检） | [安全基线自检.md](安全基线自检.md)（PASS/FAIL/FINDING/SKIP 四档、12 条检查逐条判据、F427 逐字读数、三条边界与开关体积） |
| Read the security baseline self-check in English | [安全基线自检.en.md](安全基线自检.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 要过安全认证：资产、威胁模型、标准差距、缺口清单 | [安全认证准备.md](安全认证准备.md)（11 类资产与五条信任边界、STRIDE、62443-4-2/303 645/PSA/等保2.0/61508 差距表、17 条缺口寄存器、12 个本地用例、P0–P3 路线） |
| Read the same page in English | [安全认证准备.en.md](安全认证准备.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 说清板子上有什么硬件、驱动按 compatible 找设备 | [设备树.md](设备树.md)（DTS 子集、dtc_lite、arkrtos_dt API、`dt` 命令） |
| Read the device tree page in English | [设备树.en.md](设备树.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想发一个版本，要说清「这版能当正式版吗」 | [发行版与版本策略.md](发行版与版本策略.md)（版本号规则与三处一致、发布物清单与变更控制、ABI 三编号与升版判据、预览版/正式版/1.0 判据、发版六步、明确不做的四条） |
| Read the release and version-policy page in English | [发行版与版本策略.en.md](发行版与版本策略.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 查某一版到底改了什么、哪些还没验证 | [Release-0.0.3-preview.1.md](Release-0.0.3-preview.1.md)（构建与配置变更、能力与真机证据、体积账本、复现命令、兼容注意、未验证项清单） |
| 查 API 签名与参数 | [api/Ark RTOS_API参考.md](api/Ark RTOS_API参考.md) |
| 想知道它和 Linux 比还差什么、哪些是欠账哪些是定位差 | [与Linux差距盘点.md](与Linux差距盘点.md) |
| Read the same Linux gap inventory in English | [与Linux差距盘点.en.md](与Linux差距盘点.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想知道它和 FreeRTOS / RT-Thread / Zephyr 比还缺什么 | [与成熟RTOS对比.md](与成熟RTOS对比.md)（逐维度矩阵 + 缺口分档 + 明确不做的） |
| Read the same page in English | [与成熟RTOS对比.en.md](与成熟RTOS对比.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 让设备之间互相发现、发事件、远程调用、传数据（软总线） | [软总线接入.md](软总线接入.md)（ark_softbus 同步副本、三钩子端口、MCU 档与 RAM 实测、`bus` 命令与上板自检） |
| Read the softbus integration page in English | [软总线接入.en.md](软总线接入.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 不用 Keil，想用 GNU Make + arm-none-eabi-gcc 编 F427 例程 | [GCC构建指南.md](GCC构建指南.md)（工程位置、工具链、make 用法、与 Keil 的对应、GCC 侧三个坑、验证状态） |
| 在板上跑单元测试、加自己的用例 | [单元测试框架.md](单元测试框架.md)（用例自动注册、断言宏、`utest` 命令、宿主自测门禁、用例清单） |
| Read the same page in English | [单元测试框架.en.md](单元测试框架.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 换芯片前想知道内核耦合了哪些硬件 | [硬件耦合审计.md](硬件耦合审计.md)（耦合点清单与处置、移植要动的范围） |
| Read the hardware-coupling audit in English | [硬件耦合审计.en.md](硬件耦合审计.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想让门禁在 Gitee 上自动跑、并看清哪些项没跑 | [远程CI与门禁裁剪.md](远程CI与门禁裁剪.md)（`ci_gate --remote` 远程档、裁掉的 17 项及其工具链、skip 不等于 pass、未验证部分写死） |
| Read the remote-CI page in English | [远程CI与门禁裁剪.en.md](远程CI与门禁裁剪.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 想让空白/制表符/行尾这类"风格问题"被机器判而不是靠人 | [格式基线.md](格式基线.md)（只判四条纯字节规则：无制表符、无行尾空白、文件以换行结尾、无 BOM；豁免 vendored 树；`
\r\n` 让 `__LINE__` 偏移 1143 的实证） |
| Read the format-baseline page in English | [格式基线.en.md](格式基线.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |
| 手上没有板子，想在 PC 上真跑一遍 port 层（trap 入口/上下文切换/SVC 通路/定时器） | [QEMU模拟器移植与验证.md](QEMU模拟器移植与验证.md)（QEMU 9.2.3 环境与依赖 DLL 补丁、RV32 单 M 模式 port、LA64 单核 port、**LA32 只到编译/链接（载体 `la32r-QEMU` / `loongson-community/qemu:la32-user-exp` 都存在，但只有 Linux 发行包或源码，本机不能执行，见 §6.1.1）**、**SMP 可仿真但内核侧一步未做（§9）**、trap 帧与取号、无 libgcc/libc 约束、**PRMD 位序坑**、串口实录、已知限制、monitor telnet 调试手法、**三目标功能矩阵与逐开关实录（audit / crash_log / mdk_trace / seccheck / softbus / image_sign / hw_safe / kernel_guard）**） |
| 用英文读同一份限制清单（哪些是取舍、哪些是欠账） | [已知限制.en.md](已知限制.en.md)（condensed English edition；中文页 normative，不新增主张与读数） |

## 目录结构

```
docs/
├── README.md                              # 本文件（文档索引 + API 文档生成说明）
├── Ark RTOS应用安装与调试指南.md             # 【操作手册】开发调试/串口安装/启动流程/故障处理/排错
├── Ark RTOS应用安装与调试指南.en.md         # 【English】Ark RTOS应用安装与调试指南页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 配置区与安装策略.md                      # 【设计说明】分区/池粒度/槽位与 RAM 窗口/配置记录格式/安装策略
├── 配置区与安装策略.en.md                   # 【English】配置区页的浓缩英文版（中文页 normative；不新增主张与读数）
├── POSIX与Windows兼容层说明.md              # 【设计说明】兼容层能力与边界、启动门闩、堆、真机自测
├── POSIX与Windows兼容层说明.en.md           # 【English】兼容层页的浓缩英文版（中文页 normative；不新增主张与读数）
├── SVC服务号与子命令表.md                   # 【协议】服务号（SVC 立即数）+ 子命令号（p[0]）两层的唯一真源、ABI 规矩、加新子命令的三处改动
├── SVC服务号与子命令表.en.md                # 【English】浓缩英文版：19 个服务号总表与子命令数、FILE_SYS 两段路径语义、APP_MGR 从 2 起、四条 ABI 约定、新加子命令三处、小程序 SDK 副本为何不 include；中文页 normative
├── 返回码与错误码约定.md                    # 【协议】11 个模块的 ARKRTOS_*_ERR_* 具名码、值逐处不变、哨兵与不得统一的既有契约
├── 返回码与错误码约定.en.md                 # 【English】浓缩英文版：五条约定、26 个模块 397 处码值账、六个哨兵、八条不得统一、littlefs 两处裸 return -1；中文页 normative

├── socket与select兼容层.md                  # 【设计说明】App 侧 socket/select：头文件与边界、SVC 0x1E wire 契约、阻塞/超时在 App 侧兑现、SOCKET_DEMO、fd 空间重叠等限制
├── socket与select兼容层.en.md               # 【English】浓缩英文版：七个头文件与 Linux 布局、四条边界（只 AF_INET / 无 DNS / 只有 select / 4 个 socket）、内核只登记用户态轮询且按完整参数块匹配、SVC 0x1E 的 8 字参数块与 17 子命令与 -200..-215、未编协议栈时 socket 明着失败、三条限制、SOCKET_DEMO 覆盖项、上板 pass=36 fail=0 与首次上板三处读反导致静默错答案的取证；中文页 normative
├── 调度器说明.md                            # 【设计说明】调度模型、O(1) 就绪集/延时链/时间片、三种触发点、实测与未做项
├── 调度器说明.en.md                         # 【English】调度器页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 同步原语与令牌守恒.md                    # 【设计说明】为何用户态等待只登记+轮询、派发方 peek(only_kernel)、三条契约、四个失败根因
├── 同步原语与令牌守恒.en.md                   # 【English】同步原语页的浓缩英文版（中文页 normative；不新增主张与读数）
├── Shell与日志Linux化.md                    # 【设计说明】Shell 命令风格与日志分级/出口/dmesg 环的 Linux 化取舍
├── Shell与日志Linux化.en.md                 # 【English】浓缩英文版：bash 形态提示符与路径规范化、13 条 Linux 风格命令与 5 个别名、诚实口径（没有的字段打 - 或 n/a）、dmesg 行格式与日志环 4096 B、/dev 改名表与根命名空间、F401 环尺寸覆盖到 512 与链接余量；中文页 normative
├── 内存保护与App越权验证.md                  # 【设计说明】逐任务 MPU region 配置 + APP_BAD 越权验证
├── 内存保护与App越权验证.en.md               # 【English】内存保护页的浓缩英文版（中文页 normative；不新增主张与读数）
├── MMU进程模型与ELF装载.md                  # 【实机记录+设计说明】ELF 装载：页表后端 ELF64（LA64）与无 MMU 档 ELF32（RV32/PMP）；开关/窗口/互斥、两遍法、`sp→argc`、QEMU 实跑读数、`KERNEL refill` 根因、M2 欠账 U-1..U-13
├── MMU进程模型与ELF装载.en.md               # 【English】MMU/ELF 装载页的浓缩英文版（中文页 normative；不新增主张与读数）
├── MMU平台可下载模块-VxWorks式.md          # 【实机记录+设计说明】VxWorks 式可重定位模块：ET_REL 现场链接、导出表白名单、SOP 重定位、ld/lkup/modules/unld、PLV0 取舍、与 ELF 的 2x2 并存、QEMU 实测读数、三个真 bug
├── MMU平台可下载模块-VxWorks式.en.md        # 【English】MMU平台可下载模块-VxWorks式页的浓缩英文版（中文页 normative；不新增主张与读数）
├── VFS路径命名空间.md                       # 【设计说明】VFS 挂载点、路径解析、根目录约定、/dev 与 /mnt
├── VFS路径命名空间.en.md                    # 【English】VFS 页的浓缩英文版（中文页 normative；不新增主张与读数）
├── lwIP协议栈接入.md                        # 【设计说明】lwIP 2.1.3 端口层、回环网卡、socket 底座、未接真实以太网的边界
├── lwIP协议栈接入.en.md                     # 【English】浓缩英文版：为什么先做回环（板级无 PHY 物证）、目录与 192 文件未改动、33+2 个 .c 与开关只来自 Kconfig、端口层三条口径（超时换算 / 失败不装成功 / 只用 _internal）、多线程回环不需 netif_poll_all 的纠错、内核区 128K 到 256K 与池 768 到 640 KB 连带重装、已验证与未验证分档、2 槽 2 KB 栈与五个内存参数；中文页 normative
├── 小程序设计.md                           # 【设计说明】文件系统里的第三种用户态应用：两块(代码/ RAM)内存模型、装载六步、体积上限、多实例/跨复位崩溃禁用/开机自启清单、与 ELF 的对照
├── 小程序设计.en.md                         # 【English】小程序页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 崩溃恢复与镜像版本把关.md                 # 【设计说明】B 项崩溃终局与跨复位计数、C 项版本三态判定；明确不做签名与回滚
├── 崩溃恢复与镜像版本把关.en.md             # 【English】崩溃恢复页的浓缩英文版（中文页 normative；不新增主张与读数）
├── App崩溃硬件安全与遗言机制.md              # 【设计说明】App 崩溃后的硬件安全态（遗言）机制：通道 A/B、两阶段中和、计划内交接、`hwsafe`
├── App崩溃硬件安全与遗言机制.en.md          # 【English】App崩溃硬件安全与遗言机制页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 无缝升级例程.md                          # 【例程说明】SEAMLESS_V1/V2 两个小程序交接控制权、两个记录文件与三条硬规则、实测数据
├── 无缝升级例程.en.md                       # 【English】浓缩英文版：为什么是小程序加外部 NOR、两个记录文件字段表、交接时序、装载分块让出与交接空洞实测、两处上板修掉的缺陷、未做项；中文页 normative

├── 无缝升级-双缓冲热切换.md                  # 【设计说明】A/B 双缓冲热切换：异常栈帧改写四步、arm/take、三条拒绝边界、数据段不重置、默认关与 +48 B
├── 无缝升级-双缓冲热切换.en.md              # 【English】无缝升级-双缓冲热切换页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 单元测试框架.md                          # 【设计说明】板端 utest：用例自动注册、断言宏、`utest` 命令、宿主自测门禁与用例清单
├── 单元测试框架.en.md                       # 【English】单元测试框架页的浓缩英文版（中文页 normative；不新增主张与读数）
├── arkc板上直跑-M3b-QSPI持久化.md       # 【实机记录】QSPI NOR 持久化：引脚/AF 取证、位敲数据面取舍、noent 内核缺陷、全链路实录、速度账与欠账
├── arkc板上直跑-M3b-QSPI持久化.en.md        # 【English】arkc板上直跑-M3b-QSPI持久化页的浓缩英文版（中文页 normative；不新增主张与读数）
├── arkc板上直跑-M4-hello演示.md      # 【实机记录】标准 C 的 printf/main 小程序：SDK 与工程分目录放 NOR、`make` + `mini run`、printf 取参桩、七条硬限制、回归与宿主门禁
├── arkc板上直跑-M4-hello演示.en.md          # 【English】arkc板上直跑-M4-hello演示页的浓缩英文版（中文页 normative；不新增主张与读数）
├── arkvim编辑器显示约定.md            # 【约定】屏幕布局、五条显示约定与判据、已知限制、host 门禁与板上取证、vendored 同步流程
├── arkvim编辑器显示约定.en.md               # 【English】arkvim 编辑器显示约定的浓缩英文版（中文页 normative；不新增主张与读数）
├── stm32h743-external-memory.md             # 【实机记录】H743 外置 SDRAM/FMC：FMCEN 总闸门、32 MB 读写验证、编译器池与 ramfs 落 SDRAM
├── stm32h743-external-memory.en.md          # 【English】stm32h743-external-memory页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 功能开关.md                              # 【设计说明】三层开关、逐项"关掉后"语义、依赖约束、真能关的验证方法
├── 功能开关.en.md                           # 【English】功能开关页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 流缓冲与消息缓冲.md                      # 【用法+证据】sbuf/mbuf 两种语义、返回码、配置与 H743/F427 开档体积
├── 流缓冲与消息缓冲.en.md                   # 【English】浓缩英文版：两种语义不可混用、返回码为何避开 -4、环形区与先挂表后睡、宿主 11 组与 F427/H743 体积账、上板三个用例与真 ISR 探针、SVC 0x1F 33 条；中文页 normative
├── 固定块内存池.md                          # 【用法+证据】固定块池、SVC 0x20、拒绝口径与默认档链接实测
├── 固定块内存池.en.md                       # 【English】浓缩英文版：为何不用堆、调用者提供存储区、接口与 SVC 0x20 子命令、拒绝口径、宿主 47 条与板端 9/9 与 MPOOL_DEMO 33 条证据；中文页 normative

├── 低功耗与tickless.md                      # 【设计说明+证据】睡眠长度怎么算、相位结算防丢拍、补齐走节拍体、边界与验收口径
├── 低功耗与tickless.en.md                   # 【English】浓缩英文版：睡眠长度怎么算、相位结算防丢拍、补齐走节拍体、F427 板端四个读数与体积账、功耗未测；中文页 normative

├── 实时性与最坏延迟.md                      # 【设计说明+证据】DWT 周期计数器、tick.isr/wake.lo/wake.hi/crit.max 四项口径、上板读数、三条边界、TRCENA 坑
├── 实时性与最坏延迟.en.md                    # 【English】实时性页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 安全基线自检.md                          # 【设计说明+证据】`sec` 12 项只读自检：四档结论、逐条判据、F427 逐字读数、开关与体积
├── 安全基线自检.en.md                       # 【English】浓缩英文版：四档结论与 FINDING/SKIP 为何必须分开、12 条检查逐条判据、iso 三值（0 无后端 / 1 窗口 / 2 页表）与不用 MPU 当唯一判据的理由、F427 上板逐字读数与 76 us 交叉核对、只读与 static 报告结构、三条边界、开关零变化与 +2056/+668 B 体积账、复现三条命令；中文页 normative
├── 安全认证准备.md                          # 【设计说明】资产与信任边界、STRIDE 威胁模型、五个标准差距表、17 条缺口寄存器、本地用例矩阵、P0–P3 路线
├── 安全认证准备.en.md                       # 【English】安全认证准备页的浓缩英文版（中文页 normative；不新增主张与读数）
├── Kconfig配置系统.md                       # 【设计说明】Kconfig 菜单、纯 Python 工具链、符号=宏名、注入点、依赖关系、数量限制迁移
├── Kconfig配置系统.en.md                    # 【English】Kconfig配置系统页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 配置头导航.md                            # 【导航】七个配置头各管什么、包含顺序（顺序本身是契约）、五条规则、门禁与典型症状落点
├── 配置头导航.en.md                         # 【English】浓缩英文版：改什么动哪个头、包含顺序、七个头的边界、五条规则、五道门禁、三种改法命令；中文页 normative

├── Shell命令拆分方案.md                     # 【方案·未实施】shell 单文件 6244 行的只读盘点、目标文件划分、共享符号清单、迁移顺序与判据
├── Shell命令拆分方案.en.md                  # 【English】Shell命令拆分方案页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 设备树.md                                # 【设计说明】DTS 子集与 dtc_lite、arkrtos_dt 查询 API、生成与校验、`dt` 命令、验证状态
├── 设备树.en.md                             # 【English】浓缩英文版：DTS 子集与拒绝未知指令、查询 API 只返回 enabled、生成与 --check、F427 dt 命令 20 节点原文、验证分档；中文页 normative
├── 内核Shell控制台使用说明.md               # 【操作手册】console 命令（info/app/drv/task/sched/fault/install/log/pool/trace/cfg）
├── 内核Shell控制台使用说明.en.md              # 【English】Shell 控制台页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 已知限制.md                              # 【设计说明】唯一一页限制清单：执行模型/应用形态/系统服务/可靠性安全/工具链，逐条标注取舍或欠账
├── 已知限制.en.md                           # 【English】浓缩英文版：五张限制表（执行模型/应用形态/系统服务/可靠性安全/工具链），逐条标取舍或欠账并给证据；中文页 normative

├── 欠账盘点.md                              # 【清单】功能/工程能力/易读性/文档四类欠账，每条带核对命令与判据、优先级与维护规则
├── 格式基线.md                              # 【设计说明】`.editorconfig` + `tools/format_gate.py`：四条纯字节规则（无制表符/无行尾空白/文件以换行结尾/无 BOM）、vendored 豁免表、喂毒自检、`\r\r\n` 致 `__LINE__` 偏移 1143 的取证
├── 格式基线.en.md                           # 【English】浓缩英文版：为什么只判四条纯字节规则（本机无 clang-format、GBK 与 UTF-8 并存）、F1-F4 判据、积极豁免的 vendored 树、喂毒自检是门禁一半、1143 行多余 CR 让 __LINE__ 偏移 4779 对 3636 的取证与 58 字节镜像差、.gitattributes 两个 -text 例外、加新规则的顺序、四条不许读成什么；中文页 normative
├── 远程CI与门禁裁剪.md                      # 【设计说明】Gitee Go 远程档：为什么裁 17 项（缺 Keil/交叉编译器）、skip 不等于 pass、NOT VERIFIED 清单、工作流本身与 runner 读数均未验证的口径
├── 远程CI与门禁裁剪.en.md                   # 【English】浓缩英文版：17 项要开发机工具链所以远程档裁掉、三种糊弄法为何都不接受、skip 不是 pass 的三条口径、30/0/17 读数与 NOT VERIFIED 清单、哪些真跑哪些靠本机替代取证、工作流与 runner 读数均未验证、之后还剩三步；中文页 normative
├── 与Linux差距盘点.md                      # 【设计说明】与 Linux 逐维度差距表 + 欠账排序 + 明确不做的取舍
├── 与Linux差距盘点.en.md                    # 【English】浓缩英文版：三类口径（取舍/欠账/生态）、九维度差距表、欠账排序与不做的清单；中文页 normative

├── 与成熟RTOS对比.md                       # 【设计说明】对 FreeRTOS/RT-Thread/Zephyr 的逐维度矩阵 + 缺口分档
├── 与成熟RTOS对比.en.md                     # 【English】与成熟RTOS对比页的浓缩英文版（中文页 normative；不新增主张与读数）
├── 硬件耦合审计.md                          # 【设计说明】内核硬件耦合点清单与处置、移植要动的范围
├── 硬件耦合审计.en.md                       # 【English】浓缩英文版：达标/待修/结构性解耦三档结论、八方向依赖矩阵、内核里 HAL 令牌扫描命中表、三条待修（guard 反向 include、死宏 168 MHz 对实测 96 MHz、F401 借用 F427 HAL 目录）、四层耦合原则（capability/feature/contract/layout）、ark_vfs 切分形态与 12 个 include 分区头的文件里唯一例外；中文页 normative
├── 软总线接入.md                            # 【设计说明】ark_softbus 组件同步副本、板级三钩子、MCU 档位与 RAM 实测、`bus` 命令与上板自检
├── 软总线接入.en.md                         # 【English】软总线接入页的浓缩英文版（中文页 normative；不新增主张与读数）
├── GCC构建指南.md                           # 【操作手册】GNU Make + arm-none-eabi-gcc 的 F427 例程：工具链、用法、与 Keil 的对应、GCC 侧三个坑、验证状态
├── QEMU模拟器移植与验证.md                  # 【实机记录】QEMU 上的 port 层验证：环境与依赖 DLL 坑、RV32 单 M 模式 port（trap 帧/a7/CLINT）、LA64 单核端口（256 B 帧/就地切换/PRMD 位序/TCFG-RDTIME 标定）、LA32 参考目标（128 B 帧，仅编译/链接；载体现状见 §6.1）、SMP 结论（§9）、构建与链接约束、串口实录、已知限制、monitor telnet 调试
├── 发行版与版本策略.md                       # 【设计说明】版本号规则与三处一致、发布物清单、ABI 升版判据、预览版/正式版/1.0 判据、发版六步、明确不做的
├── 发行版与版本策略.en.md                   # 【English】浓缩英文版：发布=一个 tag + 一份发布说明、三段版本号何时动、-preview 只在 tag 上、四处一致与 release_check 六项、发布物清单、三个 ABI 编号、预览/正式/1.0 五条判据、发版六步顺序、四条不做；中文页 normative
├── Release-0.0.3-preview.1.md               # 【发布说明】0.0.3 预览版：变更、体积账本、复现命令、兼容注意、未验证项
├── img/                                   # 【实测图】根 README §调度表现 引用（必须入库，勿被忽略规则吞掉）
│   ├── sched-overview.png                 # 整体调度时间线 + 切换间隔（4.97 s 窗口）
│   ├── sched-switch-detail.png            # 单次切换细节 + App 任务被调度进来跑 72.7 µs
│   └── sched-stats.png                    # 间隔/PendSV 分布 + 优化 A/B（插桩关口径）
└── api/
    ├── Ark RTOS_API参考.md                 # 内置生成器输出的 Markdown API 参考（已生成）
    └── html/                            # Doxygen 输出的 HTML（需本机安装 Doxygen）
```

下面几节介绍 API 文档的生成方式。

## 生成方式

```bash
# 默认：检测到 Doxygen 就生成 HTML，否则生成 Markdown
python tools/gen_api_doc.py

# 强制生成 Markdown（无需 Doxygen）
python tools/gen_api_doc.py --md

# 两者都要
python tools/gen_api_doc.py --both
```

## 为什么需要脚本而不是直接跑 Doxygen

内核源码（`kernelsrc/`）中的注释存在 **GBK 与 UTF-8 混用**（历史文件为 GBK，
新增模块为 UTF-8），而 `*.md` 文档为 UTF-8。Doxygen 的 `INPUT_ENCODING` 是全局
设置，直接扫描源码会让其中一部分中文注释变成乱码。

`tools/gen_api_doc.py` 的做法是：

1. 逐行尝试 UTF-8 → GBK 解码，把源码复制成一份 UTF-8 临时树（系统临时目录，不改动仓库）；
2. 基于根目录 `Doxyfile` 生成临时配置（输入指向 UTF-8 树、编码设为 UTF-8）；
3. 调用 Doxygen，或在内置 Markdown 生成器中解析注释并输出 API 参考。

## 安装 Doxygen（可选）

- Windows：`choco install doxygen.install` 或从 <https://www.doxygen.nl/download.html> 下载安装包
- 安装后确保 `doxygen` 在 PATH 中，重新运行 `python tools/gen_api_doc.py` 即可

## 注释规范

新增接口请沿用现有 Doxygen 风格，至少包含：

```c
/**
* @brief 一句话说明接口用途
* @param x 参数说明
* @return 返回值说明
* @note 注意事项（可选）
*/
```

公开 API（`kernelsrc/include/arkrtos.h`）建议同时使用 `@defgroup` 归类，
以便生成的 HTML 按模块组织。
