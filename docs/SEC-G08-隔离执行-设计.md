# SEC-G08 剩余半边：App shell 回调的隔离执行（设计与实施计划）

> 状态：**五个阶段全部落地（2026-11-26）**：M-08-1（窗口切分）、M-08-2（upcall 核心）、M-08-3（LA64 桩字节 + 初始帧 + `SVC 0x23` 分派）、M-08-4（跳板改调 `call()` + 注册三值放行 + 参数搬家 + QEMU 端到端门禁）、M-08-5（文档收口 + SDK 边界四条写进公共头）。
> 当前落地的是 fail-closed 半边（`2080a9b`、`903abee`）：特权级分离 + 分页的构建上，
> `arkrtos_shell_ext_register()` 见到 `is_priv == 0` 的调用者直接回
> `ARKRTOS_USHELL_ERR_PRIV`（-6），注册表一个槽都不动。
> 本文回答的是另一半：**在同一个构建上，把功能安全地还回来**。
>
> 读法纪律：本文写的是**计划**，不是读数。任何一行在代码落地并经判据验证之前，
> 不得在 `欠账盘点.md` / `已知限制.md` 里被当成「已实现」。

## 1. 缺陷现状（缺陷本身没有变）

```
App 任务（PLV3，自己的 2 MB 窗口）
   └─ SVC 0x1A 注册命令 → 内核存下 func 指针（VA 在 App 窗口里）
控制台任务（PLV0，内核栈）
   └─ 键入命令 → 跳板 → func(argc, argv)   ← 同一份 App 代码，跑在 PLV0
```

`func` 的**指令与数据都在 App 的窗口里**，但执行时的特权级与控制台任务相同：
它读得到内核 RAM，写得到任务表。这不是「App 的命令能做啥」的问题，是「内核替它做了」。
fail-closed 把这条路径关掉了，代价是功能消失。

## 2. 目标与非目标

**目标（必须同时成立）**

1. `func` 只在 **PLV3** 执行，且只在 **它自己的地址空间**里执行；
2. 它看到的栈、参数、返回通道**都在 App 窗口内**，内核 RAM 一个字节都碰不到；
3. 返回路径只有一条：`syscall`（Ark 服务号 0x23，本文新增），由内核记账后终止该执行体；
4. 任何一步失败都必须**可见**（有返回码或日志行），不许静默降级成「在 PLV0 跑一下算了」；
5. 关档/其它架构（RV32 PMP 档、Cortex-M MPU 档）行为**逐字不变**。

**非目标（写死，避免越做越大）**

- 不是沙箱：不限制 App 回调能调哪些 SVC、不限制它能吃多少 CPU；它和 App 本体同权。
- 不做「在 App 自己的线程上异步插入执行」：那需要打断 App 正在进行的系统调用，是另一件事
  （Linux 侧有信号的框架，Ark 侧不做）。
- 不做回调的并发保护：同一 App 的两个回调同时跑由调用者自己负责（见 §6 边界 3）。

## 3. 三个候选机制与取舍

| 机制 | 做法 | 否掉/选中的理由 |
|---|---|---|
| A. 控制台任务自己降权 | 在跳板里就地装 App 空间、建 PLV3 帧、`ertn`，返回时靠 0x23 回到内核 | 控制台任务是 `is_priv=1`，**调度器刻意不给特权任务装空间**（`arkrtos_mpu_set_app()` 里那条 early return）；要么给这个任务开一个例外（等于给「特权任务可临时进用户空间」开一条通用通道），要么在切换路径里加第二种语义。为一件 shell 命令改调度器的语义边界，代价大于收益 |
| B. 复用 App 自己的线程 | 让 App 主线程自己执行回调 | App 多半正卡在某个系统调用里（`read` 等 stdin / `wait4`），没有「插入执行」的入口；信号那套在 Linux ABI 档里才有，Ark 档没有 |
| **C. App 空间里的临时 upcall 线程（选中）** | 内核在本 App 的地址空间里**注册一个临时任务**（`is_priv = 0`，栈在 App 窗口的专用区），入口是内核写进窗口里的一小段桩；控制台任务等它结束 | 全部复用既有机制：任务注册 / 降权 / `arkrtos_paging_bind_task()` / 调度器装空间 / SVC 分派 / 任务退出回收。新代码只在「谁写桩、谁等、怎么把 rc 拿回来」三处。窗口布局加一段专用区，不影响已有段 |

## 4. 选定机制：细节

### 4.1 窗口切分（唯一一处布局改动）

现状（`arkrtos_elf.c: elf_window_layout()`）：

```
[ win_base .. stack_bottom )        可执行区（PT_LOAD 落点）
[ stack_bottom .. argv_start )      用户栈 32 KB
[ argv_start .. win_top )           argv/aux 区 16 KB（16 字节对齐）
```

新布局，在可执行区与用户栈之间插入 **upcall 区 8 KB**（4 KB 桩页 + 4 KB 栈，按 4 KB 对齐）：

```
[ win_base .. upcall_base )         可执行区
[ upcall_base .. upcall_base+4K )   桩页：内核写、PLV3 可执行；`rk up` 之后清空
[ upcall_base+4K .. stack_bottom )  upcall 栈（默认 4 KB，`ARKRTOS_ELF_UPCALL_STACK`）
[ stack_bottom .. argv_start )      用户栈 32 KB（不变）
[ argv_start .. win_top )           argv/aux 16 KB（不变）
```

- 常量：`ARKRTOS_ELF_UPCALL_SIZE`（默认 8 KB），窗口因此多要 8 KB；QEMU LA64 窗口 2 MB，
  可执行区余量充足。窗口小到放不下时报 **`ARKRTOS_ELF_ERR_LAYOUT`**（与现有三段判据同一条出口，
  不新增错误码）。
- 装载时把 upcall 区清零（`elf_zero_window()`），并把 `upcall_base / upcall_sp / stub_va`
  记进 `arkrtos_elf_info_t`（该结构已经按空间 `s_elf_info[space]` 存一份）。
- **不参与装载的程序照样有这块区**：它不是 App 的，是内核借窗口用的；不给 App 任何句柄。

### 4.2 桩（stub）：内核写的 24 字节 LA64 指令

固定写法，全在 `arkrtos_port_ushell_stub()` 里（LA64 专属；其它 port 只返回 `-1`）：

```
入口约定（内核注册任务时写进初始帧）：
    $a0 = argc        $a1 = argv
    $a2 = func        （App 窗口内的 VA）
    $sp = upcall_sp   （upcall 栈顶，16 字节对齐）
    $ra = upcall_sp   （桩不返回；留一个可辨认的值便于诊断）

桩体：
    move  $t0, $a2
    jirl  $ra, $t0, 0        # rc = func(argc, argv)，返回地址指向下一句
    li.w  $zero, 0           # 屏障：让 jirl 的落点可读
    syscall 0x23             # a0 = rc；内核在此收回执行权（不再返回用户态）
```

- 桩页由内核通过 `arkrtos_port_paging_write_space()` 写入（该接口已有，`CLONE_THREAD` 的帧就是它写的）。
- **桩里没有绝对地址、没有全局变量、不读任何内核内存**：三个参数全在寄存器里。
- 若 App 自己的指令集档不同（RV32 的桩将来要做 U 模式版本），桩字节按 port 提供；
  本档只做 LA64，其它架构 `arkrtos_port_ushell_stub()` 返回 -1 ⇒ 注册仍 fail-closed（见 §4.5）。

### 4.3 执行流程（一次命令 9 步）

1. **跳板**（控制台任务，PLV0）找到 `g_ushell[i]`，取 `owner_task` → `arkrtos_task_get_current()` 同款查表拿 TCB；
2. **门禁**（全部失败都回错误码 + 一行日志，不降级）：
   - owner 任务不存在 / 已退出 ⇒ `-1`「handler no longer available」；
   - `is_priv != 0`（内核侧注册的行）⇒ 走原来的同栈直调（**不走隔离**，它本来就是内核代码）；
   - 构建没有 `ARKRTOS_USE_PRIV && ARKRTOS_USE_PAGING && ARKRTOS_USE_ELF` ⇒ 注册阶段就已经被拒，这里不会出现；
   - `func` 必须落在该空间的 ELF 段内（**新的** `ushell_func_alive_space(space, func)`，按 `s_elf_info` 的 `PT_LOAD` 区间判，替代现在只看分区表槽位的写法）；
   - 该空间已有 upcall 在跑（`s_up_busy[space]`）⇒ 回 `-1`「busy」——同一空间不叠两层；
3. **登记**：置 `s_up_busy[space] = 1`、把 `rc` 槽、等待用信号量准备好；
4. **建任务**：`arkrtos_task_register(stub_va, upcall_stack + size, size, prio, 0)`，
   随后 `is_priv = 0`、写回 `rom_start/rom_size/ram_start/ram_size`（= 该 App 的窗口）、
   `arkrtos_paging_bind_task(&tcb, space)`，最后用 `arkrtos_port_task_set_regs()` 一类的入口把
   `a0/a1/a2/sp` 写进初始帧。**这一段在临界区里做完**（与 `elf_start_task()` 同一个理由：
   半配置状态被 tick 切走就是间歇故障）；
5. **等**：控制台任务在信号量上阻塞（超时 `ARKRTOS_USHELL_UP_TIMEOUT_MS`，默认 10 s）；
6. **桩执行**：调度器把 upcall 线程切上 CPU，装的是 App 的空间，PLV3 起跑；
7. **返回**：`syscall 0x23` → `la_trap_c` → `SVC_Server` → 新处理器 `svc_upcall_ret()`：
   - 校验调用者是 upcall 线程（任务表里那个 id + 空间都对），否则回 `-1`（不许别人冒充返回）；
   - `rc = a0`；写进 `s_up_rc[space]`；`s_up_busy[space] = 0`；
   - 把该任务按「自己退出」摘掉（复用 `exit` 内部路径），并 `post` 信号量;
8. **跳板取 rc**：信号量返回后读 `s_up_rc[space]`，作为命令返回值；
9. **清理**：任务槽由退出路径回收；桩页在下次 `rk up` 不清也行（无秘密），但 `s_up_busy` 必须归零——
   异常路径（超时 / 任务被信号杀）都不能把它留成 1，否则该 App 的 shell 命令永久「busy」。

### 4.4 注册路径（把 fail-closed 收窄成「有条件放行」）

`arkrtos_shell_ext_register()` 的策略从二值改成三值：

```
#if (ARKRTOS_USE_USHELL_APP_CB == 0)              /* 特权级分离 + 分页 */
    if (调用者 is_priv == 0) {
        if (arkrtos_ushell_upcall_ready(space_of_caller)  != 0)  → 放行（记下 space 与 owner）
        else                                                     → ARKRTOS_USHELL_ERR_PRIV（-6）
    }
#endif
```

`arkrtos_ushell_upcall_ready()` = 「该空间有 upcall 区 + port 提供桩 + 没有并发上限打满」。
判据必须落在**每个 App 自己的装载结果**上，不是全局常量：没装过 ELF 的空间永远没有 upcall 区。

`g_ushell[]` 增两个字段：`space`（注册时调用者的空间号）与 `has_space`（1 = 需要隔离执行）。
内核侧注册的行 `has_space = 0`，走原来的直调路径，**不碰** `arkrtos_kernel_ushell_active`
（该标志的收窄就是这一条：隔离路径不再需要它；等 §4.6 的内核侧行也具备隔离条件后再整体删除）。

### 4.5 失败语义（每条都要能说清「谁看见了」）

| 失败 | 谁看见 | 表现 |
|---|---|---|
| 空间没有 upcall 区 / port 不给桩 | 注册方 | `ARKRTOS_USHELL_ERR_PRIV`（-6），注册表不动 |
| `func` 不在该空间的 ELF 段内 | 操作员 | `0x..: handler no longer available (image unloaded?)` |
| 同一空间已有 upcall 在跑 | 操作员 | `0x..: busy (another upcall in this space)` |
| 建任务失败（槽满 / 配额） | 操作员 | `0x..: cannot start isolated worker (-N)` |
| 桩里 `syscall 0x23` 之前异常（段错 / 访存越界） | 操作员 | 任务被现有故障路径杀掉；跳板超时后用 `s_up_busy` 归零收尾，报 `-1` |
| 超时（App 死循环） | 操作员 | `0x..: isolated worker timed out after 10000 ms`；**任务不被强杀**（与现有「不擅自杀任务」的口径一致），只把 busy 位清掉并在下一次调用时判「上一次还没回来」 |
| 冒充 0x23（普通 App 直接 syscall 0x23） | 内核 | `-1`，且不做任何状态变更 |

### 4.6 与既有机制的交互

- **`arkrtos_kernel_ushell_active`**：隔离路径**不再置它**。App 回调的缓冲区在 App 窗口里，
  `arkrtos_kernel_ptr_ram_ok()` 本来就认 `ARKRTOS_ELF_WINDOW_*`。内核侧注册的命令仍是直调，保留现状。
- **信号**：App 回调被信号杀掉时走 `arkrtos_cpu_fault_kill` / `exit_by_signal`，upcall 线程随之消失，
  跳板拿到的是「任务没了」而不是 rc —— 按 §4.5 超时/异常路径处理，不回假 rc。
- **配额（SEC-G07）**：upcall 线程**继承** App 的 `app_owner`，占用 App 的任务配额——
  否则一个 App 可以起无限多回调线程把任务表吃光。顺手把这条写进用例。
- **审计（SEC-G05）**：开档时把「App 命令被隔离执行」记一条 kind 14（`code` = owner id），
  「被拒」也记一条。没有审计档时这段编掉。

## 5. 判据（写死在门禁里，不靠「看起来对了」）

**必须同时成立的读数**

1. **QEMU LA64 实跑**（新门禁 `tools/ushell_iso_gate.py`，脚本化 shell）：
   - App（`example/qemu_loongarch64/elf/hello.c` 新档）用 SVC 0x1A 注册命令 `hi`；
   - shell 里跑 `hi a b`，读数：App 打印自己的 `argc=3` + 两行参数 + 内核打的
     `[ushell] isolated rc=0 tid=N space=S plv=3`；
   - **特权级证据**：桩里第一条指令之前由内核把 `$a3` 设成哨兵值，回调里把它改成别的值；
     返回后内核断言「哨兵被改过」⇒ 证明确实执行过（不是跳过）；同时在桩里读 `CRMD.PLV`
     写进 `$a4`，返回后断言 = 3 ⇒ **在 PLV3 跑过**（不是内核代跑）；
   - **空间证据**：回调把 `func` 的地址与栈地址各打一行，内核断言两者都落在该 App 的
     `[win_base, win_top)` 内；
   - **反面**：同一 App 的注册在第 1 步之前被 `sec` 打印拒绝过一次（`-6`），证明门禁没被整体放开；
   - **反面**：老镜像（装载时没有 upcall 区，用 `ARKRTOS_ELF_UPCALL_SIZE=0` 编一档）上注册仍回 -6。
2. **宿主读数**（`test_shell_ushell_host.c` 新档 `ushell_iso`）：
   - 注册在 `upcall_ready == 0` 时回 -6、`== 1` 时放行，且 `g_ushell[i].space` 记对；
   - 跳板在 busy / 无空间 / func 不在段内 / 建任务失败四条路径上给出**不同**的返回与日志；
   - `s_up_busy` 在每条失败路径上都归零（逐个路径断言，防「一条路忘了清」）；
   - 内核侧行（`has_space == 0`）仍走直调，且**不再**置 `arkrtos_kernel_ushell_active`（新断言）。
3. **反假绿变异**（每次跑门禁都真做一遍，然后还原）：
   - 把桩里的 `syscall 0x23` 换成 `jirl $zero, $ra, 0`（回到内核？不成立）⇒ 门禁必须红（无 rc）；
   - 把建任务那步的 `is_priv = 0` 删掉 ⇒ PLV 读数为 0，门禁必须红；
   - 把 `s_up_busy` 的归零删掉 ⇒ 第二次调用必须「busy」，门禁必须红。

**不许当证据的东西**：QEMU 里「没崩」；`rc == 0` 而无 PLV 读数；只有内核侧打印没有 App 侧打印。

## 6. 诚实边界（落地后要同步写进 `已知限制.md`）

1. **只覆盖「Shell 命令」这个入口**。App 通过其它 SVC 让内核代跑代码的路径不存在，这一项不改变；
2. **不是沙箱**：回调与 App 本体同权（同空间、同 SVC 权限、同配额身份），它只是**不再拿到内核特权**；
3. **回调与 App 本体并发**：App 自己的全局变量 / 锁（含 libc 的 malloc 锁）在回调里同时访问会竞争。
   App 侧要按「多线程」写；不这么做就是 App 的缺陷，不是隔离执行的缺陷。**这一条必须写进 SDK 文档**；
4. **单核验证**：SMP 档下 upcall 与 App 本体可能在两个核上同时跑，没有额外同步；`-smp 2` 实跑不在本轮判据里；
5. **超时不强杀**：死循环的 App 回调只能等它自己退出或被杀；跳板只保证「不再叠加」；
6. **RV32 / Cortex-M 档不变**：它们的桩需要各自写（RV32 的 U 模式桩将来单独做），本轮 `arkrtos_port_ushell_stub()` 返回 -1 ⇒ 那些档继续 fail-closed。

## 7. 分阶段实施（每阶段独立可验证、可回退）

| 阶段 | 内容 | 判据 | 风险 |
|---|---|---|---|
| M-08-1 ✅ | 窗口布局 + upcall 区（`ARKRTOS_ELF_UPCALL_SIZE`，默认 8 KB）+ `s_elf_info` 三字段 + 清零 + `elf_gate` 更新（**已落地 2026-11-26**：`elf_gate` 全绿、宿主 `elf` 196 条 / `elfup0` 188 条全绿） | `tools/elf_gate.py` 全绿；`test_elf_host.c` 新增「upcall 区不与被加载段、用户栈、argv 重叠」与「`=0` 档退回旧布局」两档断言；`fork_gate` 十档仍全绿（布局改了，App 栈位置要重新对齐） | 低（纯加区，行为不变） |
| M-08-2 ✅ | `ready()` / `call()` / `ret()` 三个入口 + 宿主桩化的单测（**已落地 2026-11-26**：宿主 `USHELL-HOST OK 210 checks` / `REFUSED 17 checks`，`arkrtos_shell_ushell` 行覆盖率 99.58%） | `warn_gate` 零告警（238 TU）；`coverage_gate` 该模块 ≥ 基线；`ci_gate` 的 `ushell-host` 两臂绿 | 中（全部走宿主可测路径，行为未变） |
| M-08-3 ✅ | LA64 桩字节 + 初始帧写寄存器 + `SVC 0x23` 分派（`arkrtos_task.c`）（**已落地 2026-11-26**） | `tools/linux_abi_gate.py` 新增 `svc-0x23`（号定义 / 分派 case / 回调用通道三处同源）与 `abi-split-rv32` 号段 0x10..0x23，14 项全绿；QEMU 单测 `utest run ushell_upcall_refuse_stranger` 读数 `1 matched, 1 passed, 0 failed; assertions 6 run / 0 failed` | 中（port 改动，已取 QEMU 读数） |
| M-08-4 ✅ | 跳板改调 `call()`、注册策略收窄、`ushell_func_alive_space()`、`g_ushell` 两字段（**已落地 2026-11-26**：QEMU 端到端读数 `[ushell] isolated rc=0 tid=6 space=0 plv=3`，App 侧 `cb argc=3` / `argv[0..2]=hi,a,b` / `cb argv=0x21f3fd0 func=0x2000350 sp=0x21f3f68`；宿主两臂 `USHELL-HOST OK 278 checks` / `REFUSED 34 checks`；新门禁 `tools/ushell_iso_gate.py` PASS，3/3 反假绿变异被抓） | §5 全部读数；`ci_gate` 119 项 + `fork_gate` 71 项全绿 | 高（行为切换点） |
| M-08-5 ✅ | 文档收口：`已知限制.md` §四那一行改写、`安全认证准备.md` §5/§7、`欠账盘点.md` 那一行、SDK 文档补「回调按多线程写」（**已落地 2026-11-26**：`arkrtos_ushell.h` 增「回调的写法约束（隔离执行档，必须遵守）」四条——按多线程写 / argc·argv 是只读副本且 ≤ 512 字节 / 只许阻塞 `wait()` 不许忙等 / 不得依赖 `$tp` 与内核内存；四页文档同步改写） | 文档自洽 + `doc_audit` 绿 | 低 |

**M-08-2 的实现位置（取舍，与原计划不同）**：原计划新增独立 TU `arkrtos_ushell_upcall.c`，实际
落在 `kernelsrc/shell/arkrtos_shell_ushell.c` 的同一 TU（`#if ARKRTOS_USE_USHELL_ISO` 段），接口契约
单独放 `kernelsrc/include/arkrtos_ushell_upcall.h`。理由：除 QEMU LA64（`$(wildcard kernelsrc/src/*.c)`）
外，F427 GCC `sources.mk`、F427/F401 `.uvprojx`、H743 `sources.mk` 都是**显式文件清单**，新增独立
TU 必须同时改 4 处工程文件才编得进去；而 `arkrtos_shell_ushell.c` 已被全部工程包含。代价是单文件
变长；收益是「一处编译开关、五平台同源」，且宿主用例本来就在覆盖该 TU。port 契约拆成
`stub_supported` / `stub` / `start` 三个钩子：`ready()` 需要一个无副作用的「本档会不会写桩」查询，
先让 LA64 返回 `0 / -1 / 0` 占位，**本阶段可见行为一个字节都没变**（`ready()` 恒 0 ⇒ 注册继续 fail-closed），
M-08-3 只替换函数体。

**M-08-3 的实现与两处取舍（与预注册不同，均已验证）**：

1. **QEMU 侧的证据形态换成「冒充者被拒」**，不是原计划的「桩把参数打回去」。理由：后者需要一个能发出 `syscall 0x23` 的**真 App 镜像**作夹具（否则得先把跳板接到 `call()`，那是 M-08-4），而「非 upcall 线程发 `syscall 0x23` 必得 `ERR_CALLER`、且记账不动」恰好只依赖 M-08-3 自己落地的那一段（号表 + 分派 + `ret()` 的冒充判据），却能证到同一件事：**这条系统调用真的进了内核并走到了 `arkrtos_ushell_upcall_ret()`**。用例在 `arkrtos_utest_cases.c`，`$a0` 先塞哨兵（0x5a5a / 0x1234）：若号被别的东西吃掉、或者参数被原样打回，读到的就是哨兵本身，读数是假的。
2. **`arkrtos_port_paging_write_space()` 复用而非新写一份**：桩与初始帧要写的是「某个空间的窗口 VA」，与 fork 的 `space_clone` 写目标同形；差别只有「PA 从哪来」（按需档 `s_pg_pair_pa[][]`、铺满档 `s_pg_space_base[] + idx*0x2000`）。因此把该函数的守卫从 `LINUX_FORK` 拆成 `LINUX_FORK || USHELL_ISO`，函数内按 `ARKRTOS_USE_PAGING_DEMAND` 分叉。**这也解释了 `ARKRTOS_USE_USHELL_ISO` 为什么要多要一条 `ARKRTOS_USE_PAGING_PAGES == 1`**：静态 PS=20 窗口档（PAGES=0）只有一张共享窗口，没有「按空间写」的通路，编出实体只会得到一个写不进去的实现。
3. **`start()` 显式收 `space`**：不读 `s_pg_app_space`（「当前空间」随调用时机漂移，与仓库里「唯一真源：任务表」的口径相悖）。代价是头文件 + 宿主桩 + 1 处调用点各加一个参数。

**一条被实测确认的 ABI 口径**：Ark 自己的 SVC 返回值是 **32 位**的——`ARKRTOS_SVC_RET` 把值按 `uint32` 写进帧的 `$a0`，高位补零，所以负值读回来是 `0x00000000FFFFFFF9` 而不是符号扩展的 `-7`；要 64 位符号扩展的是 Linux ABI 那条路（`arkrtos_port_svc_set_ret_signed()`）。App 侧按 `int32` 取低 32 位即可，QEMU 用例按 `(uint32)` 比较（第一版拿 `long` 直接比，把正确的行为读成了失败——这是判据写错，不是实现错）。

**M-08-4 的实现与五处取舍/实测结论（均已取读数）**：

1. **注册策略从二值收窄成三值**：`arkrtos_shell_ext_register()` 只在「特权级分离 + 分页」档
   看调用者 `is_priv`——`is_priv != 0`（内核侧注册的行）**照旧直接放行**，`is_priv == 0` 时再问
   `arkrtos_ushell_upcall_ready(space)`：回 1 就放行并记下 `space` 与 `owner_task`，回 0 仍回 -6。
   这样「关掉功能」与「安全地还回来」只差一个判据，回退不需要动代码。
2. **`ushell_func_alive_space()` 按 `s_elf_info[space]` 的 `PT_LOAD` 区间判**，不再只看分区表槽位：
   分区表说「这块 Flash 里有个 App」，但说不出一段地址是不是**这个空间**当前那份映像的可执行段。
3. **PLV 证据取自 trap 帧的 PRMD 两位**（port 钩子 `arkrtos_port_svc_caller_plv()`），
   取代 §5.1 原计划的「`$a3` 哨兵 + `$a4` 读 CRMD.PLV」：PPLV 就在帧里，少一次读 CSR、
   也不需要在用户态多占一个寄存器。
4. **§5.2 的措辞订正**：「不再置 `arkrtos_kernel_ushell_active`」只对**隔离路径**成立；
   `is_priv != 0` 的同栈直调路径照旧置位（它本来就是内核代码，那个放行位就是给它用的）。
5. **参数必须搬家**：回调在 PLV3 读不到内核 RAM，`argv` 只能是起跑前写进 App 窗口的一份副本。
   落在 `[call_sp, up_sp)` 这 512 字节以内（`ARKRTOS_USHELL_UP_ARGS_MAX`），8 字节小端槽，
   末尾带一个 NULL 指针槽（否则回调里 `while(argv[i])` 会越过参数区）。
6. **实测坑 A：任务栈顶必须停在 `call_sp`，不能给到 `up_sp`**。`arkrtos_task_register()` 会按
   「栈底 + 栈长」造一份初始帧（`ARKRTOS_USE_STACK_USAGE=1` 时还会整段填图案），栈顶给到
   `up_sp` 时那份帧正好压在刚写好的参数区上。症状极有迷惑性：`argc` 对、`argv` 指针对、
   `argv[i]` 全 0（`(null)`），看起来像「写窗口没生效」。宿主用例现在把这条几何钉住
   （`g_up_reg_bytes == g_up_start_sp - 桩页底`）。
7. **实测坑 B：App 侧 `SVC 0x11` 是寄存器 ABI，且只能用子 1（阻塞 wait）**。`$a0` = 子命令、
   `$a1` = 参数，与 `0x10 DEV_IO` / `0x1A SHELL` 的「参数块指针」不是一套；子 1 = `WAIT(ms)`
   真阻塞，子 3 = `DELAY(us)` 忙等。夹具第一版两条都踩了：子命令被读成指针 ⇒ 落到 `default`
   空转；改用子 3 后，App 优先级 12 高于 shell 14，忙等直接饿死 shell，**症状是「控制台像死了」**
   而 A/B 任务照常打印。结论写进 SDK 与 `已知限制.md`：**回调里只许阻塞 wait，不许忙等**。
8. **审计 kind 14 未做**：隔离执行的每次回调目前只在 `ulog` 里留痕（`[ushell] isolated …`），
   没走审计通道。理由：审计项要的是「谁在什么特权级做了什么」，而本阶段的证据链是
   「trap 帧 PLV + 任务表 space」两处内核侧读数；再加一条审计记录只会多一份可能与真源
   漂移的副本。M-08-5 已把 SDK 边界定稿（`arkrtos_ushell.h` 四条写法约束），审计 kind 14
   仍决定不落：证据链两处内核侧读数已经够，加记录只多一份会漂移的副本。
9. **新门禁 `tools/ushell_iso_gate.py`**：QEMU 上跑「送 App → 注册 → 键入 `hi a b` → 断言
   App 侧参数/地址 + 内核侧 `rc=0 plv=3` → 再发一次（busy 归零）→ `hix` 必须 not found 且不误调」，
   另带 `--mutate` 三条反假绿变异（桩里换 syscall 号 / `is_priv` 改 1 / 不清 `s_up_busy`），
   读数 `PASS: 3/3 条变异都被抓到`。

**回退方式**：M-08-1 加的是常量（置 0 即退回旧布局）；M-08-4 的放行门是
`arkrtos_ushell_upcall_ready()`，把它改成恒 0 就回到今天这个 fail-closed 行为，
**不需要回滚代码**。

## 8. 为什么不先做「最省事的那条」

- **把 `ARKRTOS_USE_USHELL_APP_CB` 直接置 1**：等于把 PLV0 借给 App，缺陷原样回来。否。
- **只在 `func` 外面套一个地址区间检查**（「指针在 App 窗口内才允许执行」）：地址检查挡不住
  App 代码去读内核 RAM（那不需要参数），也挡不住它对任务表做任意写。否。
- **用信号机制把回调注入 App 本体**：Ark 档没有信号；Linux ABI 档有，但那是给 Linux 程序用的，
  与 shell 命令的注册语义对不上。否（列为将来可选）。