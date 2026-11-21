# SVC 服务号与子命令表

**[简体中文](SVC服务号与子命令表.md) | English([SVC服务号与子命令表.en.md](SVC服务号与子命令表.en.md))**

App / 驱动以非特权态运行时，每一个内核服务都经一条 `SVC` 指令陷入内核。
一次调用分两层编号：

| 层 | 放在哪 | 定义在 | 说明 |
|---|---|---|---|
| 服务号 | `SVC` 指令的立即数 | `kernelsrc/include/arkrtos_def.h` 的 `ARKRTOS_SVC_*` | 8 位，决定进哪个处理函数 |
| 子命令号 | 参数块的 `p[0]` | `kernelsrc/include/arkrtos_svc_abi.h`（网络服务在 `arkrtos_net_abi.h`） | 决定走哪个操作 |

两张表都由内核分派（`kernelsrc/src/arkrtos_task.c` 的 `svc_*()`）与 SDK 封装
（`arkrtos_oslib.c`、`arkrtos_driver_bridge.c`）**共用同一套宏**。改一个子命令
只改头文件里那一行，两侧同时跟上；两边各写一遍裸数字的时代已经结束（改了一
处忘了另一处，症状是静默走错分支，不是编译报错）。

## 服务号

| 号 | 宏 | 处理函数 | 子命令数 |
|---|---|---|---|
| 0x10 | `ARKRTOS_SVC_DEV_IO` | `svc_dev_io` | 6 |
| 0x11 | `ARKRTOS_SVC_TASK_CTRL` | `svc_task_ctrl` | 10 |
| 0x12 | `ARKRTOS_SVC_SYS_INFO` | `svc_sys_info` | 5 |
| 0x13 | `ARKRTOS_SVC_EVENT_CTRL` | `svc_event_ctrl` | 3 |
| 0x14 | `ARKRTOS_SVC_DRV_MGR` | `svc_drv_mgr` | 6 |
| 0x15 | `ARKRTOS_SVC_SYNC_CTRL` | `svc_sync_ctrl` | 17 |
| 0x16 | `ARKRTOS_SVC_MQ_CTRL` | `svc_mq_ctrl` | 4 |
| 0x17 | `ARKRTOS_SVC_TIMER_CTRL` | `svc_timer_ctrl` | 4 |
| 0x18 | `ARKRTOS_SVC_APP_MGR` | `svc_app_mgr` | 7（1 号已退役） |
| 0x19 | `ARKRTOS_SVC_LOG` | `svc_log` | 不用 `p[0]`（r0/r1/r2 是 level/tag/msg） |
| 0x1A | `ARKRTOS_SVC_SHELL` | `svc_shell` | 3 |
| 0x1B | `ARKRTOS_SVC_THREAD_CTRL` | `svc_thread_ctrl` | 3 |
| 0x1C | `ARKRTOS_SVC_FILE_SYS` | `svc_file_sys` | 23（文件 1..10 + 路径 11..23） |
| 0x1D | `ARKRTOS_SVC_HEARTBEAT` | `svc_heartbeat` | 1 |
| 0x1E | `ARKRTOS_SVC_NET` | `svc_net` | 17，见 `arkrtos_net_abi.h` |
| 0x1F | `ARKRTOS_SVC_SBUF_CTRL` | `svc_sbuf_ctrl` | 12（流缓冲 1..8 + 消息缓冲 9..12） |
| 0x20 | `ARKRTOS_SVC_MEMPOOL_CTRL` | `svc_mempool_ctrl` | 5 |
| 0x21 | `ARKRTOS_SVC_ESET_CTRL` | `svc_eset_ctrl` | 6（`ARKRTOS_USE_ESET` 打开时才编进来） |
| 0x22 | `ARKRTOS_SVC_WAITMULTI_CTRL` | `svc_waitmulti_ctrl`（`arkrtos_task.c`） | 1（`ARKRTOS_USE_WAITMULTI` 打开时才编进来） |

## 子命令号

逐个号码见 `kernelsrc/include/arkrtos_svc_abi.h`：每个服务一段，宏名形态
`ARKRTOS_<模块>_SUB_<操作>`，值写成 `(Nu)` 与网络服务表一致。两处特殊：

- `ARKRTOS_SVC_FILE_SYS` 的 1..10 是**卷相对**操作（`arkrtos_fs_*`，路径相对挂载点），
  11..23 是**命名空间绝对**操作（`arkrtos_vfs_*`，路径以 `/` 起）。两段在同一张
  表里但前缀不同（`ARKRTOS_FILE_SUB_*` / `ARKRTOS_PATH_SUB_*`），因为它们是两套
  路径语义，只是共用一条 SVC。
- `ARKRTOS_SVC_APP_MGR` 从 2 起。1 号是早期的「按槽位装镜像」，已被 2 号取代并
  退役；号码不复用。

## 约定

1. **号码是 ABI**：只增不改。App 镜像烧在板子上，用旧 SDK 编的镜像会按旧号码发
   请求；把号码复用给别的操作，症状是调错操作而不是调不通。
2. **名字可以改，值不能改**。改名动两侧源码；改值动 ABI。
3. **每个子命令的 `p[]` 布局不在头文件里重复写**：写它的地方是 SDK 封装
   （`arkrtos_oslib.c` 填 `p[1..]`），校验它的地方是内核处理函数里的
   `arkrtos_kernel_*_ok()`。头文件只固定号码，避免同一份布局出现两份。
4. **新加一个子命令要动三处**：`arkrtos_svc_abi.h` 加一行、内核 `svc_*()` 加一个
   `case ARKRTOS_<模块>_SUB_<操作>:`、SDK 封装里 `p[0]` 写同一个宏。三处名字相同，
   grep 一个宏名就能看全。

## 参数块字宽（宽参 ABI）

参数块 `p[]` 的每个槽在 64 位目标上默认仍是 32 位（`uint32`）。宽 VA 下用户地址
可以超过 4 GB，`uint32` 槽放不下，因此有一条编译开关把参数块升级成原生字宽：

| 项 | 窄档（默认） | 宽档 |
|---|---|---|
| 开关 | `ARKRTOS_USE_WIDE_SVC_ARGS=0` | `ARKRTOS_USE_WIDE_SVC_ARGS=1` |
| 槽类型 | `typedef unsigned int arkrtos_svc_arg_t` | `typedef unsigned long arkrtos_svc_arg_t` |
| 子命令标记 | 无（`p[0]` 就是子命令号） | `p[0]` 的高位置 `ARKRTOS_SVC_SUB_WORD`（`0x80000000UL`） |
| 块字节数 | `n*4` | `n*8`（`ARKRTOS_SVC_ARG_BYTES(n)`） |
| 生效条件 | 永远为空档 | 同时要求 `__SIZEOF_LONG__ > 4`（`ARKRTOS_SVC_WIDE_ARGS`） |

相关宏都在 `arkrtos_svc_abi.h`：`ARKRTOS_SVC_SUB(sub)` 填 `p[0]`（宽档置
bit31）、`ARKRTOS_SVC_SUB_OF(w)` 取出子命令号、`ARKRTOS_SVC_ARG_BYTES(n)` 算
校验长度。内核侧 30 个 `svc_*()` 处理函数在取到块后用 `SVC_WIDE_GUARD()` 校验
「块在用户 RAM 且 bit31 置位」，网络服务另有 `g_net_req_arg[]` 的宽化。

三条口径：

1. **宽参是编译开关，不是自适应**。内核与 App 必须用同一个 `ARKRTOS_USE_WIDE_SVC_ARGS`
   编译。宽内核 + 窄 App：内核打一行一次性日志
   `wide-args kernel got a 32-bit param block; kernel and app must be built with the same ARKRTOS_USE_WIDE_SVC_ARGS`
   并**终止调用任务**（`arkrtos_kernel_svc_wide_reject()` 走 `arkrtos_task_kill_internal`，
   与 `THREAD_SUB_KILL` 同一条路）：字宽不符是整档 ABI 不匹配，后面每一条 SVC 都会
   读错，只回 -1 会让 App 在用户态空转，串口上跟「内核没收到调用」长得一样；终止之后
   `exec` / `wait` 能看到它结束、装载器台账能记一笔，失败是干净的；窄内核 + 宽 App：
   调用被拒，不误成功。
2. **32 位目标（ARM / RV32 / LA32）上宽档是空档**：`ARKRTOS_SVC_WIDE_ARGS` 要求
   `__SIZEOF_LONG__ > 4`，开关打开也还是 32 位槽，体积与行为不变。默认档（开关
   为 0）在 64 位目标上同样零变化。
3. **SVC 返回寄存器只有 32 位**：宽档下若 mempool 分配到的块地址高于 4 GB，内核
   返回 0 并打一行一次性日志，**不返回截断地址**。

门禁 `tools/svc_wide_gate.py`（`ci_gate` 的 `svc-wide` 一项）钉住三件事：守卫真的调了
reject、reject 真的杀了任务（少任何一环就退回静默空转）；窄档里 `SVC_WIDE_GUARD`
展开成空语句；Linux 兼容层状态（fd 表、堆游标、信号表）的清空必须在新任务挂进
就绪集之前（`elf_start_task()` 之前），位置错了直接红——实测签名是夹具 stage1 的
`brk(0)` 比窗口堆基点高 8192，且随代码布局时红时绿。变异自检 6 条（guard-reject /
guard-ret / narrow-empty / kill / subword / reset-order）每条都要能把门禁弄红。

未覆盖：`kernelsrc/sdk/mini_sdk/arkc/` 仍是 `u32` 基（小程序运行时按 arkc 的
部署清单独立编译，拿不到这套头文件），宽 VA 下不在本机制范围内。

## 与小程序 SDK 的关系

`kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c` 是这两个服务（0x1A 控制台、
0x1B 线程退出）在 arkc + 小程序场景下的最小副本，它**不 include** 上面两张表：
那个文件要被 arkc（只吃部署清单里的文件）与宿主 gcc 门禁（不带 `-I`）各编一遍，
两边都拿不到头文件搜索路径。副本以同名宏在文件内落一份，注释里写明权威来源。
