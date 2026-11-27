# 在 Ark RTOS 上跑 Linux 可执行文件：系统调用 ABI（MMU 档，LA64 实跑）

**[简体中文](Linux可执行文件兼容-系统调用ABI.md) | English([Linux可执行文件兼容-系统调用ABI.en.md](Linux可执行文件兼容-系统调用ABI.en.md))**

## 0 一句话

MMU 档（页表后端）上，装载器已经能把一个**原生 Linux 静态 ELF 可执行文件**装进
PLV3 空间并跳 `e_entry`（见 [MMU进程模型与ELF装载.md](MMU进程模型与ELF装载.md)）。
这一层补的是**它进去以后怎么跟内核说话**：程序发的是 Linux 的系统调用号
（`syscall 0` + `$a7`），内核按 Linux 的 ABI 把它接住、返回 Linux 的返回值语义。

```
[linux] write /mnt/ram/linux_hello rc=0
[linux] exec rc=0
[linux] hello from the Linux syscall ABI (a7=64 write)
[linux] uname -> 0 ok
[linux] sysname=ArkRTOS machine=loongarch64
[linux] clock_gettime(MONOTONIC) -> 0 ok
[linux] clock_gettime(REALTIME) -> -38 ok
[linux] getpid==task id>0 -> 1 ok
[linux] brk(0)>0 -> 1 ok
[linux] brk(+4096) -> 34607104 ok
[linux] mmap>0 -> 1 ok
[linux] mmap write/read -> 1 ok
[linux] openat>0 -> 1 ok
[linux] read 4 bytes -> 4 ok
[linux] ELF magic -> 1 ok
[linux] close -> 0 ok
[linux] openat(missing) -> -2 ok
[linux] PASS: every syscall answered as specified
[linux] finished after 0 ms, exit code 7
```

上面是 `qemu-system-loongarch64 -machine virt` 上的一次真实串口读数（
`example/qemu_loongarch64/elf/linux_hello.c`，纯 Linux ABI 的静态程序：自己写
`_start`、用 `syscall 0`，一行 Ark 头文件都不包含）。

**「静态程序能跑」≠「支持 Linux」。** 第 3 节列的是明确不支持项，一条不含糊。

## 1 支持的系统调用（61 个；60 个有 QEMU 实跑读数，1 个（`pread64`）只有号表与参数校验）

编号取 `asm-generic`（LoongArch / RISC-V / ARM64 共用这一套）：

| 号 | 名字 | Ark 侧兑现方式 | 实测读数 |
|---:|---|---|---|
| 23 | `dup` | 复制一个 fd 槽（管道/文件/标准流/socket），返最小空号（§8.12） | `dup(writer) is a new fd -> 1 ok` / `read through the copy -> 3 ok` / `dup(999) -> -EBADF = -9` / `exit code 56` |
| 24 | `dup3` | 复制到指定号（源是 socket 也照抄句柄；目标槽或 `0..2` 别名指向 socket 仍拒）；`fd 0..2` 走别名（不共享槽）；`O_CLOEXEC` **接受并记账**，`old==new` 明确拒（§8.12） | `dup3(writer, 1) returns 1 -> 1 ok` / `dup3(1, 6, O_CLOEXEC) returns 6 -> 6 ok` / `dup3(reader, writer) overwrites -> 4 ok` |
| 25 | `fcntl` | 六条 cmd：`F_DUPFD` / `F_GETFD` / `F_SETFD` / `F_GETFL` / `F_SETFL` / `F_DUPFD_CLOEXEC`（最后一条就是 `F_DUPFD` 顺带置 `FD_CLOEXEC`）；其余 cmd 一律 `-EINVAL`（§8.13） | `F_GETFD reads FD_CLOEXEC back -> 1 ok` / `F_GETFL(reader) = O_RDONLY\|O_NONBLOCK = 2048 ok` / `F_DUPFD_CLOEXEC(9) -> 9 = 9 ok` |
| 56 | `openat` | `arkrtos_vfs_app_stat()` 查命名空间；目录返 `-EISDIR` | `openat>0 -> 1 ok` |
| 57 | `close` | 释放 fd 槽，返 0；管道端会减引用计数 | `close -> 0 ok` |
| 59 | `pipe2` | 环形缓冲管道 + 两端引用计数；**默认阻塞**、`O_NONBLOCK` 档并存（见 §6） | `read empty -> -EAGAIN = -11`（非阻塞档）/ `blocking pipe read waited >= 40 ms -> 1 ok`（fork 档，见 §8.15）/ `exit code 55` |
| 63 | `read` | `arkrtos_vfs_read_at()` 按 fd 的偏移读 | `read 4 bytes -> 4 ok`（读到 ELF magic） |
| 64 | `write` | 逐 128 字节 `arkrtos_board_putc` | `hello from the Linux syscall ABI` 正文 |
| 67 | `pread64` | 按 `pos` 直读、**不改 fd 读游标**（`ld.so` 按段表随机访问同一个文件要用） | 无夹具读数（§1.1） |
| 93 | `exit` | `arkrtos_thread_exit_code_internal()` | `exit code 7` |
| 94 | `exit_group` | 同上（单线程程序等价） | 同上 |
| 96 | `set_tid_address` | 记住地址、返本任务 id（不参与 futex；futex 见 §8.4） | `getpid==task id>0 -> 1 ok` 同路径 |
| 98 | `futex` | 十一档 op：`WAIT`/`WAKE`/`REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`/`WAKE_BITSET`（含超时与位集）+ PI 三档 `LOCK_PI`/`UNLOCK_PI`/`TRYLOCK_PI` + 条件变量的 PI 变体 `WAIT_REQUEUE_PI`/`CMP_REQUEUE_PI`，键 = 物理帧 + 页内偏移（`ARKRTOS_USE_LINUX_FUTEX`，§8.4 / §8.4b / §8.4c） | `futex(WAIT) on a different value == -EAGAIN -> -11 ok` / `futex(WAIT) returned 0 after being woken -> 0 ok` / `futex2 bitset waker saw 1, woke only matching (bits=7) -> 7 ok` / `futex(WAIT) with 50 ms timeout == -ETIMEDOUT -> -110 ok` / `futex LOCK_PI on a free word == 0 -> 0 ok` / `futex LOCK_PI twice == -EDEADLK -> -35 ok` / `futex TRYLOCK_PI on an owned word == -EAGAIN -> -11 ok` |
| 99 | `set_robust_list` | 登记「持有者死掉时要替我交代的锁链」：链头非空、8 字节对齐、长度装得下 `next/futex_offset/pending` 三格，否则 `-EINVAL`（§8.4b） | `set_robust_list with a short length == -EINVAL -> -22 ok` / `set_robust_list on a well formed head == 0 -> 0 ok` |
| 100 | `get_robust_list` | 只认自己（本内核没有可查询的第二个进程身份），别人的 pid 如实 `-ESRCH`；出参指针为空 `-EFAULT`（§8.4b） | `get_robust_list(self) == 0 -> 0 ok` / `get_robust_list reports the length we registered -> 24 ok` / `get_robust_list on a foreign pid == -ESRCH -> -3 ok` |
| 113 | `clock_gettime` | `MONOTONIC` 返内核节拍；`REALTIME` 返 `-ENOSYS` | `MONOTONIC -> 0 ok` / `REALTIME -> -38 ok` |
| 118 | `sched_setparam` | 只改优先级、策略不变；`OTHER`/`BATCH`/`IDLE` 上 `priority != 0` 与 `DEADLINE` 上都回 `-EINVAL`（同 Linux：后者改用 `sched_setattr`）（§8.17） | `[sched]` 段 63 条，其中 `getparam FIFO priority is 88 -> 88 ok`、`setparam` 错误面见宿主 633 条 |
| 119 | `sched_setscheduler` | `pid`/`policy`/`sched_param*`；FIFO/RR 落 `100-priority`，OTHER/BATCH 落 `128+nice`，IDLE 落 147；`DEADLINE` 无字段承载三元组，回 `-EINVAL`（§8.17） | `getscheduler default -> 0 ok` / `getparam FIFO priority is 88 -> 88 ok` |
| 120 | `sched_getscheduler` | 读 TCB 里的 `sched_policy`（策略跟随任务槽，不另开私表）（§8.17） | `getattr policy is DEADLINE -> 6 ok`（同一字段回读） |
| 121 | `sched_getparam` | 由内核优先级反推：FIFO/RR 是 `100-priority`，其余策略一律 0（§8.17） | `getparam FIFO priority is 88 -> 88 ok` |
| 122 | `sched_setaffinity` | 单核：`len >= sizeof(unsigned long)`、掩码只认 bit0 且必须置位，否则 `-EINVAL`（§8.17） | `setaffinity short len -> EINVAL -> -22 ok` |
| 123 | `sched_getaffinity` | 同上校验后回写 1（只有 CPU0）（§8.17） | `[sched]` 段回读掩码位为 1 |
| 124 | `sched_yield` | 把自己挪到同优先级桶尾再挂一次切换请求（桶里只有自己时是空操作）（§8.17） | `sched_yield -> 0 ok` |
| 125 | `sched_get_priority_max` | FIFO/RR 是 99；OTHER/BATCH/IDLE/DEADLINE 是 0；未知策略 `-EINVAL`（§8.17） | 宿主 633 条里的上下界断言 |
| 126 | `sched_get_priority_min` | FIFO/RR 是 1；其余同上（§8.17） | 同上 |
| 127 | `sched_rr_get_interval` | 轮转量 = 10 拍 × 500 us = 5 ms，按 `struct timespec`（两个 8 字节 long）回（§8.17） | `rr interval nsec is 5000000 -> 5000000 ok` |
| 129 | `kill` | 只能投给自己（`pid != 本任务` → `-ESRCH`），置 pending（§7） | `kill rc survives handler -> 0 ok` / `kill other pid -> -ESRCH = -3 ok` |
| 130 | `tkill` | 同上（单线程下与 `kill` 同实体） | 同 `kill` 路径 |
| 131 | `tgkill` | 要求 `tgid == tid`，否则 `-ESRCH` | `tgkill tgid!=tid -> -ESRCH = -3 ok` |
| 134 | `rt_sigaction` | 装/取动作；`sigsetsize` 必须 8；`SIGKILL`/`SIGSTOP` 与非 0 缺失的 restorer 一律 `-EINVAL`（§7） | `rt_sigaction rc = 0 ok` / `old handler was SIG_DFL = 0 ok` / `no restorer -> -EINVAL = -22 ok` |
| 135 | `rt_sigprocmask` | `BLOCK`/`UNBLOCK`/`SETMASK`；`KILL`/`STOP` 永不可屏蔽（§7） | `query sees blocked bit = 1 ok` / `delivered on unblock = 2 ok` |
| 139 | `rt_sigreturn` | 校验帧 `magic` 与帧长后整帧装回；特权级不认用户帧（§7） | 由 restorer 触发，处理函数返回后读数仍正确 |
| 160 | `uname` | `sysname=ArkRTOS`、`machine=loongarch64` | `uname -> 0 ok` / `sysname=ArkRTOS machine=loongarch64` |
| 168 | `getcpu` | 两个 4 字节出参（任一可为 NULL，越界 `-EFAULT`）+ 一个 cache 参数（不看）；单核，CPU 号与 NUMA 节点号都写 0——必须与 `rseq` 写回的 `cpu_id` 同值（§8.14） | `getcpu rc = 0 ok` / `getcpu cpu = 0 ok` / `getcpu node = 0 ok` |
| 172 | `getpid` | 返内任务 id | `getpid==task id>0 -> 1 ok` |
| 174 | `getuid` | 恒 0 | 走同一返回路径 |
| 175 | `geteuid` | 恒 0 | 走同一返回路径 |
| 214 | `brk` | 窗口内单调分配（见 §4） | `brk(0)>0` / `brk(+4096) -> 34607104 ok` |
| 215 | `munmap` | 只返 0，**不归还地址** | 走同一返回路径 |
| 220 | `clone` | 只做 fork：`flags` 必须 0 或 `SIGCHLD`、`child_stack` 必须 0；复制整份地址空间 + 造新任务，子返 0 父返子号（§8，`ARKRTOS_USE_LINUX_FORK`） | `clone(child_stack!=0) == -EINVAL -> -22 ok` / `parent child=12` / `child pid != parent pid -> 1 ok` / `exit code 0` |
| 221 | `execve` | 同任务换镜像：铺段进调用者自己的空间、把入口/`sp`/`tp` 写回 trap 帧；成功不返回 | `stage2 pid==stage1 pid -> 11 ok` / `image reloaded (.data marker) -> 1 ok` / `fd 3 survived -> 1 ok` / `heap cursor reset -> 34668544 ok` / `exit code 42`（§5） |
| 222 | `mmap` | 匿名私有 / 匿名共享（后者要 `ARKRTOS_USE_PAGING_SHM`，§8.3）；**`MAP_FIXED` 已有**（按需分页档，只认匿名私有，§8.7）；同时给 `PRIVATE|SHARED` 回 `-ENOSYS` | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` / `MAP_FIXED discarded the old bytes -> 1 ok`（§8.7，6 条） |
| 260 | `wait4` | 只认 `options 0/WNOHANG`；**无 `WNOHANG` 时真阻塞**（可重启系统调用，§8.1），不编造状态，reap 后记账清零（§8） | `wait4 returned child pid -> 12 ok` / `WEXITSTATUS == 42 -> 42 ok` / `wait4(child) again == -ECHILD -> -10 ok` / `blocking wait4 blocked >= 40 ms -> 1 ok` |
| 274 | `sched_setattr` | `struct sched_attr` 逐字段按字节读（VER0 48 字节，`size < 48` 回 `-EINVAL`）；系统调用 `flags` 或 `sched_flags` 非 0 一律 `-EINVAL`；`DEADLINE` 走 `arkrtos_task_dl_set()`，准入失败 `-EBUSY`；非 deadline 策略上三个时间参数必须全 0（§8.17） | `setattr DEADLINE 100% bandwidth -> EBUSY -> -16 ok` / `setattr DEADLINE 1ms/2ms/100ms is admitted -> 0 ok` |
| 275 | `sched_getattr` | 回写同一布局；`size >= 56` 时两个 util 字段写 0（不做利用率跟踪，但字段必须给定值）（§8.17） | `getattr policy is DEADLINE -> 6 ok` / `getattr DEADLINE deadline round-trips -> 2000000 ok` |
| 226 | `mprotect` | **有页级写保护**（`ARKRTOS_USE_PAGING_MPROTECT`，§8.6）：4 KB 粒度改 `WE` 位、违规写按 SIGSEGV 杀任务；`PROT_EXEC` / `PROT_NONE` 只记账不强制；关档回「只校验参数、返 0」 | `mprotect RO == 0 -> 0 ok` / `sibling 4 KB half of the same pair stays writable -> 13107 ok` / `mprotect violation reports SIGSEGV status -> 11 ok`（§8.6，11 条） |
| 227 | `msync` | 只写回本进程记账里的「可写共享文件映射」；窗口内未命中回 0、窗口外 `-ENOMEM`（`ARKRTOS_USE_PAGING_FILEMAP`，§8.5） | `filemap msync rc -> 0 ok` / `filemap private msync rc (no writeback) -> 0 ok` / `filemap shared write persisted, private write not (bits=31) -> 31 ok` |
| 293 | `rseq` | 注册契约（`len` 必须 32、`flags` 必须 0、地址 32 字节对齐、`NULL` 注销）+ `cpu_id_start`/`cpu_id` 写回 0；`rseq_cs` 的两处中止点（信号投递前、tick 抢占前，§8.14） | `rseq register rc = 0 ok` / `cpu_id readback 0 = 0 ok` / `signal aborted the critical section = 1 ok` / `rseq_cs cleared by the abort = 0 ok` / `exit code 57` |

socket 家族（`198-210`、`242`，共 13 个）见 §8.9：它们在翻译层里落地，宿主 socket 档
**964 条断言**、QEMU 第八档 **242 条 check**（2026-11-27 复测；U-23 时是 768 / 232），
所以这 13 个号**都**计入上面「有 QEMU 实跑读数」。

未知号一律 `-ENOSYS`（38），并计数；**不猜**。

### 1.1 只有号表、没有夹具读数的那个（诚实标注）

`pread64`(67) 是给 `ld.so` 铺段用的，本轮**没有实跑夹具**：只有「号表与
`asm-generic` 相等 + LA64 板级编译通过」。

`mprotect`(226) 原先也在这张名单里（只校验参数、返 0、**不改任何页权限**）。批C-2
把它改成了真改硬件条目（§8.6），于是它有了 11 条串口读数，不再是「只有号表」的号。

表格里其余 47 个号都有 `[linux]` / `[pipe]` / `[sig]` / `[dup]` / `[fork]` / `[rseq]` / `[sched]` 夹具的串口读数。

调度策略那十二个号（118-127、274/275）读数在 `[sched]` 前缀下：它们是 §8.17 的翻译层证据，
与内核 deadline 类的「选择次序/预算」证据（宿主 `test_deadline_class` 52 条）分工不同，不能互相替代。

## 2 两个 ABI 怎么分界

同一个 SVC 入口要同时服务 Ark 自己的 ABI 和 Linux 的 ABI，分界必须在 port 层判，
不能靠内核猜：

| 架构 | Ark ABI | Linux ABI | 判据 |
|---|---|---|---|
| LoongArch64 | `syscall` 的 15 位立即数 | `syscall 0` 且 `$a7` 里是 asm-generic 号 | 读 `ERA-4` 的指令字：立即数 == 0 ⇒ Linux；!= 0 ⇒ Ark 号 |
| RV32IMAC | `a7` 取 0x10..0x22 | `a7` 取 asm-generic 号（最小 56） | 号段不重叠：`0x10..0x22` 归 Ark，其余按 Linux 号查 |
| Cortex-M3/M4/M7 | SVC 立即数 | — | 恒返 `0xFFFFFFFF`（没有 Linux 用户态这回事） |
| LoongArch32 | SVC 立即数 | — | 同上 |

内核侧只有一条钩子：`SVC_Server()` 在 `arkrtos_guard_svc()` 之后、`switch(svc_num)`
之前调 `arkrtos_linux_abi_try(ctx)`；它返回非 0 就直接返回（这一路已经处理完）。

返回值符号扩展单独一个 `arkrtos_port_svc_set_ret_signed()`——Linux 拿负数当
`-errno`，用无符号写回的话 `-2` 会变成 `4294967294`，程序读到的是天文数字。

## 3 明确不支持（不是欠账，是不支持）

| 项 | 现状 | 为什么 |
|---|---|---|
| 动态链接（`PT_INTERP` / `.so`） | **默认档仍拒**；打开 `ARKRTOS_USE_DYNLOAD` 后已支持（2026-11-20，QEMU LA64 实跑） | 内核按 Linux 约定装载解释器、铺 auxv、入口跳解释器，配一套自研 `ld-ark.so`；见 [动态链接与PT_INTERP](动态链接与PT_INTERP.md)。范围是「自研装载器 + 一级依赖 + 立即重定位」：现成发行版二进制仍跑不了（`ld.so` 是 glibc 的，仓里没有） |
| `fork` / `clone` | **fork 与线程都有**（`ARKRTOS_USE_LINUX_FORK` / `ARKRTOS_USE_LINUX_THREAD`，默认关；2026-11-23 / 11-24，QEMU LA64 实跑） | `flags==0`/`SIGCHLD` 走 fork（复制整份地址空间）；`CLONE_VM`/`CLONE_THREAD` 走线程（共用一份地址空间，见 §8.10）；其余组合一律 `-EINVAL` |
| 信号 | **已有跨任务投递**（`kill`/`tkill`/`tgkill` 按 pid 定位目标进程，2026-11-23，QEMU LA64 实跑）；`SA_SIGINFO` 三参派发与 `siginfo_t` / `ucontext_t` 已有（U-31，§8.19，2026-11-27）；仍无抢占式投递、无实时信号、无 `sigqueue` 排队（§7） | 跨任务靠「置目标 `sig_pending` + 目标卡在阻塞 `wait4` 时唤醒」，铺帧仍在目标自己下一次系统调用返回时做；抢占式入口还没有 |
| `fd` 继承 | **已有**（fork 档每进程 fd 表 + 管道对象引用计数，2026-11-23）；`dup`(23) / `dup3`(24) 已有（U-30，§8.12，2026-11-27）；64 位号表里没有 `dup2`，glibc 用 `dup3` 实现 | 见 §4 与 §8 |
| `socket` / `select` | **已有**（`ARKRTOS_USE_LINUX_SOCKET` / `ARKRTOS_USE_LINUX_SELECT`，默认关；2026-11-24，QEMU LA64 第八/七档实跑） | 13 个 socket 号 + `pselect6`(242) 进号表，底层接 lwIP 与 fd 表的 `kind` 分派；见 §8.9 / §8.8 |
| `futex` | **已有十一档 op**（`ARKRTOS_USE_LINUX_FUTEX`，默认关；WAIT/WAKE 2026-11-23、`REQUEUE`/`CMP_REQUEUE`/`WAIT_BITSET`/`WAKE_BITSET` 与相对/绝对超时 2026-11-24、PI 三档 `LOCK_PI`/`UNLOCK_PI`/`TRYLOCK_PI` 与 robust list 清理 2026-11-26、条件变量 PI 变体 `WAIT_REQUEUE_PI`/`CMP_REQUEUE_PI` 2026-11-27，QEMU LA64 实跑） | 值没变就睡、变了就叫；位集、改挂、`-ETIMEDOUT`、优先级继承、持有者死亡交接、条件变量整队搬运与直接交接都按 Linux 语义；键 = 物理帧 + 页内偏移；仍无 `WAKE_OP`/FD 类 op（如实 `-ENOSYS`）、无墙钟（`FUTEX_CLOCK_REALTIME` 带超时回 `-ENOSYS`）；见 §8.4 / §8.4b / §8.4c |
| 真 `mmap` 语义（文件映射/`MAP_FIXED`/权限位） | **文件映射已有**（`ARKRTOS_USE_PAGING_FILEMAP`，§8.5）、**`MAP_FIXED` 已有**（按需分页档，§8.7，只认匿名私有）、**页级写保护已有**（`ARKRTOS_USE_PAGING_MPROTECT`，§8.6）；匿名共享 `MAP_SHARED` 只随 `fork` 继承、无 `shm_open`（§8.3）；`PROT_EXEC` / `PROT_NONE` 仍只记账不强制 | 窗口式分配器（§4）；LA64 这条通路没有 NR/NX，做不出硬件不可执行 |
| 多线程 | **已有**（`ARKRTOS_USE_LINUX_THREAD`，默认关；2026-11-24，QEMU LA64 第九档实跑） | `clone(CLONE_THREAD)` 造一组共用地址空间的任务；`exit_group` 收整组、`getpid` 回 tgid；见 §8.10 |

## 4 降级实现，以及为什么这么降

- **fd 表**：分两档。默认档（`ARKRTOS_USE_LINUX_FORK=0`）仍是内核全局
  `lx_fd_t s_fd[8]`（路径 + 偏移），shell 的 `exec`（起新任务）成功时 `reset`；
  `execve`（同任务换镜像）**保留** `execve` 之前的 fd，与 Linux 一致。fork 档改成
  **每进程一份**（`lx_proc_t { fd[8]; sig[NSIG]; sig_pending; sig_blocked; }`，
  `static lx_proc_t s_proc[LX_PROC_MAX]`）：`clone` 时整表复制，管道两端 `readers` /
  `writers` 各 +1，子进程 `close` 只减自己那份引用（管道对象在父那边继续存活）。
  实现手法是宏别名（`#define s_fd (lx_proc_curp()->fd)`），既有 500+ 个调用点不动。
- **堆**：`brk` / `mmap` 共用 ELF 窗口内的一段（窗口 `0x02000000`，2 MB；堆从
  窗口一半起，末尾留 128 KB 给栈）。单调向上分配，不回收；`execve` 成功时游标
  归零（旧游标的地址语义属于旧镜像）。
- **`munmap` 只返 0、不归还地址**：这是有意的。若「假装还了」，程序会认为那块
  内存仍可用而实际已被别的东西占用——那是**静默错答案**，比 `-ENOSYS` 坏得多。
- **用户指针纪律**：兼容层**不直接解引用用户指针**，一律走
  `arkrtos_copy_from_user()` / `arkrtos_copy_to_user()`（判据是任务 TCB 的
  `rom_start/rom_size/ram_start/ram_size` 四个窗口字段）。越界返 `-EFAULT`。
  这条纪律是 2026-11-19 那个 bug 的直接产物（§11）。

## 5 execve（221）：同任务换镜像（2026-11-21，QEMU LA64 实跑）

Linux 的 `execve` 是「把当前进程的镜像换掉，成功后不返回」。Ark 的窗口模型
（一个任务一块静态窗口、trap 帧就压在用户栈上）恰好让它能做到**真语义**，而不是
「起一个新任务假装是 exec」：

- 装载器 `elf_load()` 多了一个 `start_task` 开关：为 0 时只把段铺进**调用者
  自己的空间**、铺好用户栈、把入口 / `sp` / `tp` 交回来，一个任务都不起；
- port 层 `arkrtos_port_svc_exec_frame()` 把这三个值写回**当前 trap 帧**
  （`ERA` / `SP` / `TP`、清 `a0..a7` 与 `ra`、`PRMD` 只留 PLV 位），末尾补
  `dbar 0` + `ibar 0`：就地覆盖同一段 VA，不做 icache 同步就是「跑旧程序半截」，
  QEMU 看不出差别，真机会。
- 为什么能就地覆盖：段写入被 `elf_check_range()` 限制在 `stack_bottom` 以下，
  而活栈/trap 帧/内核 C 栈都在 `[stack_bottom, argv_start)` 里，新 argc/argv 写在
  `argv_start` 以上 ⇒ 覆盖的只是旧镜像的代码与数据，不碰马上要用的栈。

四处必须写在标题上的不一致：

1. `envp` 不消费：这台机器没有环境变量这一层，新镜像拿到同一张空表；
2. **fd 表保留**（与 Linux 一致，但与 shell 的 `exec` 不同——后者起新任务、会 reset）；
3. 旧镜像的堆游标作废，已分配的地址不做任何回收声明（旧镜像已经没了）；
4. **带 `FD_CLOEXEC` 的 fd 在这里被还掉**（2026-11-27 起）：`lx_execve()` 在
   `s_heap_cur = 0` 之后调 `lx_fd_close_cloexec()`，逐个 `lx_fd_release()` 标了位的槽。
   位置刻意放在「段已铺好、trap 帧已改、堆游标已重置」之后——那是整条路径上唯一
   不可能失败的收尾点，放在前面一旦后续失败就得回滚别人的 fd 表。其余 fd 照旧存活
   （§8.12 的 `fd 3 survived` 读数仍然成立，它没带 `O_CLOEXEC`）。

参数纪律：`path` 与 `argv` 都在用户内存里，且装载一开始就被覆盖，所以先全部拷进
内核（16 × 256 字节静态暂存，越界回 `-E2BIG`）。

### 实测读数（`[execve]` 行，QEMU LA64）

```
[execve] stage1 pid=11
[execve] stage1 fd=3
[execve] stage1 brk(0)=34668544
[execve] stage1 brk(+8192) -> 34676736 ok
[execve] execve(missing) == -ENOENT -> -2 ok
[execve] stage1 calling execve(/mnt/ram/linux_execve)
[execve] stage2 pid=11
[execve] stage2 pid==stage1 pid -> 11 ok
[execve] image reloaded (.data marker) -> 1 ok
[execve] fd 3 survived (ELF magic) -> 1 ok
[execve] stage2 brk(0)=34668544
[execve] heap cursor reset -> 34668544 ok
[execve] PASS: same task, image reloaded, fd kept, heap reset
[execve] finished after 0 ms, exit code 42
```

四条判据各自排掉一种「看起来对」的实现：`pid` 相同排掉「起了新任务」；
`.data` 标记回到文件初值排掉「只改 trap 帧、不重写 PT_LOAD」；`fd 3` 还能读出
ELF 魔数排掉「顺手 reset 了 fd 表」；`brk(0)` 回到基点排掉「沿用旧堆游标」。

**反假绿（实测）**：把 `arkrtos_port_svc_exec_frame()` 改成「校验完直接
`return 0`、不改帧」，重编再跑，读数立刻变成
`[execve] FAIL: execve returned 35651560 (success must not return)` + `exit code 1`。
夹具内部还有一条独立的反假绿：先 `execve("/mnt/ram/nope")` 期望 `-ENOENT`，程序
必须还能继续往下跑——失败是可返回的那条路径。

### 边界

- 没有 `fork` / `clone`：需要地址空间复制；也没有父子，所以 `execve` 之后没有
  「谁等谁」。
- 没有 `execveat`，没有 `FEAT_EXECFD`；参数上限是 Ark 自己的 16 × 256，不是
  Linux 的 `MAX_ARG_STRLEN` 语义。
- 其它 port（RV32 / Cortex-M / LA32）没有 `arkrtos_port_svc_exec_frame`，`hal.h`
  里的弱定义让它们照样链接，运行期 `execve` 明确回 `-ENOSYS`——不把「没做」
  说成「成功」。

## 6 pipe2（59）：阻塞管道 + O_NONBLOCK 档（2026-11-27，U-26，QEMU LA64 实跑）

程序侧看到的就是 Linux 的 `int pipe2(int fds[2], int flags)`：分配两个 fd（读端在先）、
把号写回用户给的 `int[2]`。Ark 的实现是「两个 fd 各记一个引用 + 一条环形缓冲」：

- `lx_pipe_t` 静态表 `s_pipe[2]`（`LX_PIPE_MAX 2`），每条缓冲 256 字节（`LX_PIPE_BUF`），
  `readers` / `writers` 两个引用计数；
- `lx_fd_t` 加 `kind` 字段（普通文件 / 读端 / 写端）与 `pipe` 下标，`read` / `write` /
  `close` 全部分派到位；`execve` 换镜像时 fd 表照旧保留 ⇒ **管道跨 `execve` 存活**
  （与 Linux 一致）；
- 读写都是内核内同步搬运环形缓冲，数据不经 ramfs，也不开线程。

### 语义（默认阻塞，与 Linux 一致）

`pipe2(..., 0)` 拿到的是**阻塞**管道（Linux 的默认），带 `O_NONBLOCK` 才是「能读/写
多少算多少」。阻塞的落地方式是「登记 + 重放」：等待挂在**任务**维度（`s_pipe_wait[]`），
睡一个 `LX_WAIT_SLICE_MS`（100 ms）时间片，醒来**重放整条系统调用**（参数寄存器原样
留着，`done` 记在等待格里跨重放累计，不会重复写同一段）。返回 `LX_RET_RETRY` 与
socket / pselect6 是同一条约定。

| 事件 | Ark（默认阻塞） | Ark（`O_NONBLOCK`） | Linux | 备注 |
|---|---|---|---|---|
| 读空缓冲、写端还在 | 挂起，等到有人写或信号 | `-EAGAIN`（11） | 同 Ark 两列 | 对侧写数据时主动唤醒；100 ms 时间片是「丢唤醒」的兜底 |
| 读空缓冲、写端全关 | 返回 `0`（EOF），**不睡** | 返回 `0` | 返回 0 | EOF 是返回条件，不是等待条件 |
| 写满缓冲 | 挂起，等到读端腾出空间，**写全** | 短写（返回实际字节数） | 同 Ark 两列 | 阻塞写是写全的：超过 256 字节的块分段等空间 |
| 读端全关还写 | `-EPIPE`（32）+ 置 SIGPIPE | 同左 | `SIGPIPE` / `-EPIPE` | 先判再拷：缓冲还有空间也照样 `-EPIPE` |
| 有未屏蔽、且真会打断的信号 | 一字节没写/读就回 `-EINTR`；已有进度回短写 | 同左 | 同左 | pending 里**默认动作是「忽略」**的信号（SIGCHLD 等）不算 |
| `flags` 带 `O_CLOEXEC` | 接受，两端都记 `FD_CLOEXEC` | 同左 | 接受 | `execve` 换帧后逐个还槽（§8.13） |
| 清 / 置 `O_NONBLOCK`（`F_SETFL`） | 允许，管道两端可自由切换 | 同左 | 允许 | U-26 起；此前清位回 `-EINVAL` |

`SIGPIPE` 走的是既有的「置 pending → 系统调用返回用户态之前投递」这条路：没装处理
函数时默认动作结束任务（退出码 141），装了就在返回前跑一次 handler（`a0` = 13），
handler `rt_sigreturn` 之后 `a0` 仍是 `-EPIPE`。

上限：`LX_MAX_FD 8` 个 fd 槽 ⇒ 最多两条管道（4 槽）再加 4 个普通文件；管道或 fd 槽
不够时 `-ENFILE`。用户地址写不进 `fds[2]`（`copy_to_user` 失败）时整条管道回滚并返
`-EFAULT`——不留半条泄漏的管道。

### 实测读数（`[pipe]` 行，QEMU LA64，默认档）

```
[pipe] write /mnt/ram/linux_pipe rc=0 size=8832
[pipe] pipe2 rc = 0 ok                     <- 本组全部显式 O_NONBLOCK
[pipe] fds = 3 4
[pipe] fds are two increasing numbers = 1 ok
[pipe] write 5 bytes = 5 ok
[pipe] read 5 bytes = 5 ok
[pipe] content matches = 1 ok
[pipe] read empty -> -EAGAIN = -11 ok
[pipe] close write end = 0 ok
[pipe] read after writer closed -> EOF = 0 ok
[pipe] pipe2(O_CLOEXEC) rc = 0 ok
[pipe] pipe2(O_CLOEXEC) hands out two fds = 1 ok
[pipe] second pipe2 rc = 0 ok
[pipe] write 400 into 256 buffer -> short write = 256 ok
[pipe] read back what fits = 256 ok
[pipe] write again after drain = 256 ok
[pipe] sigaction(SIGPIPE, SIG_IGN) before EPIPE = 0 ok
[pipe] write with reader closed -> -EPIPE = -32 ok
[pipe] blocking pipe2 rc = 0 ok            <- 以下 flags=0，默认阻塞档
[pipe] blocking write with data present = 5 ok
[pipe] blocking read with data present = 5 ok
[pipe] blocking read content matches = 1 ok
[pipe] blocking close write end = 0 ok
[pipe] blocking read after writer closed -> EOF = 0 ok
[pipe] sigaction(SIGPIPE, SIG_IGN) = 0 ok
[pipe] blocking pipe2 (EPIPE) rc = 0 ok
[pipe] blocking write, reader closed, SIG_IGN -> -EPIPE = -32 ok
[pipe] sigaction(SIGPIPE, handler) = 0 ok
[pipe] blocking write, reader closed, handler -> -EPIPE = -32 ok
[pipe] SIGPIPE handler ran once = 1 ok
[pipe] SIGPIPE handler got signo = 13 ok
[pipe] blocking close write end (EPIPE pipe) = 0 ok
[pipe] PASS: pipe2 two fds, data round trip, EAGAIN on empty, EOF on writer close, O_CLOEXEC accepted, short write and EPIPE, blocking defaults and SIGPIPE delivery
[pipe] finished after 0 ms, exit code 55
```

30 条 `check()`（夹具 `example/qemu_loongarch64/elf/linux_pipe.c`，纯 Linux ABI 静态
程序，自带 `_start`）。前 16 条是 `O_NONBLOCK` 档的既有读数（两个 fd 递增、数据往返
逐字节、空读 `-EAGAIN`、关写端 EOF、`O_CLOEXEC` 被接受、满时短写、读端关掉 `-EPIPE`
——这一条现在要先 `SIG_IGN`，否则默认动作 141 会把任务直接结束），后 14 条是默认阻塞
档**在单执行流上能证的部分**：有数据时阻塞读立刻回、写端全关时阻塞读回 EOF、
`SIG_IGN` 下 `-EPIPE` 不挂死、装 handler 后 `SIGPIPE` 在系统调用返回前真投递一次。

「空读挂起、等到别人写」「满写挂起、等到别人排空」这两条**单执行流证不了**（一挂起
就回不来），读数在 fork 档的 `[fork]` 块（§8.15）。

### 边界

- 缓冲 256 字节、`LX_PIPE_MAX 2` 条、fd 槽 8 个。
- 等待粒度是 100 ms 时间片：对侧一定会主动唤醒，时间片只是「登记与睡下之间被抢占、
  唤醒丢了」的兜底，所以最坏多等一个时间片，不会永久卡住。
- 阻塞需要 port 的 `arkrtos_port_svc_restart()`（参数寄存器重放）。端口没有这个能力
  时读写不假装成功：读回 `-ENOSYS`，写回**已写入的字节数**（短写），绝不把已经进了
  环形缓冲的数据说成没写。
- `-EINTR` 是防御路径：本层在系统调用返回用户态之前就会把可投递信号投掉，实际很难
  命中；默认动作是「忽略」的信号（SIGCHLD/SIGCONT/SIGURG/SIGWINCH）**不会**打断。
- 单核验证：跨执行流的阻塞读写只在 QEMU LA64 单核上跑过；SMP 真机的「两个核同时
  睡在同一根管道上」未验。
- `SIGPIPE` 覆盖 write / 写端；`SIGPIPE` 的 `siginfo`、`SA_RESTART` 语义没有（信号层
  仍是最小一档，见 §7）。

## 7 信号（`rt_sigaction` 134 / `rt_sigprocmask` 135 / `kill` 129 / `tkill` 130 / `tgkill` 131 / `rt_sigreturn` 139）

2026-11-21 落地，QEMU LA64 实跑（`example/qemu_loongarch64/elf/linux_signal.c`，
`[sig]` 块 31 条 check + PASS 行 + `exit code 138`）。

```
[sig] write /mnt/ram/linux_signal rc=0 size=8328
[sig] exec rc=0
[sig] getpid = 1 ok
[sig] rt_sigaction rc = 0 ok
[sig] old handler was SIG_DFL = 0 ok
[sig] kill rc survives handler = 0 ok
[sig] handler ran once = 1 ok
[sig] nested handler ran = 1 ok
[sig] max handler depth = 2 ok
[sig] blocked signal not delivered = 1 ok
[sig] delivered on unblock = 2 ok
[sig] no restorer -> -EINVAL = -22 ok
[sig] kill other pid -> -ESRCH = -3 ok
[sig] ignored signal not delivered = 3 ok
[sig] PASS: sigaction installed, nested delivery, blocking, defaults and refusals all ok
[sig] finished after 0 ms, exit code 138
```

### 语义（做到哪一档，写死）

| 项 | 现状 |
|---|---|
| 投递对象 | **只有「同一任务给自己发」**。没有 `fork`/`clone` ⇒ 没有第二个可投递实体；`pid != 本任务 id` 一律 `-ESRCH`（`kill`/`tkill` 同实体，`tgkill` 另要求 `tgid == tid`） |
| 号段 | 1..31。0 是存在性探测（返 0），≥32 返 `-EINVAL`；没有实时信号 |
| 投递时机 | **系统调用返回用户态之前**（分发器唯一的出口处）。没有抢占式投递：信号只在程序自己下一次进内核时才送到 |
| 动作地址 | `sa_handler` 必须是一个能取指的用户地址（内核先 `copy_from_user(..., 4)` 探一次） |
| restorer | **必须有**且非 0，否则 `rt_sigaction` 返 `-EINVAL`、旧动作不装（没有「处理完回哪」的路径，绝不猜地址） |
| 信号帧 | 头 32 B（`magic` / `signo` / 保存的 `blocked` / 帧长）+ `siginfo_t` 128 B + `ucontext_t` 432 B + **整份 trap 帧快照**（256 / 528 / 1040 B，帧长由端口报，本板 FPU 档 528）；`SA_SIGINFO` 时 `a1`/`a2` 分别指向后两块（U-31，§8.19） |
| 存放位置 | 端口算：`fsp = (当前内核 sp - 帧长 - 128) & ~15`，再查任务栈底（留 64 B） |
| `sigreturn` | 校验 `magic` 与帧长后整帧装回；**特权级不认用户帧里那份**（不许抬权） |
| 默认动作 | 忽略 {17,18,23,28}；挂起类 {19,20,21,22} 按忽略并单独计数（没有可挂起的对象）；其余 `exit(128 + signo)` |
| `SIG_IGN` | 真丢弃并计数 |
| `SIGKILL` / `SIGSTOP` | 装不上（`-EINVAL`）、也屏蔽不掉（`rt_sigprocmask` 里强行清位） |

### 为什么帧址要由端口算，且基准是「当前内核 sp」

这是本轮唯一一个会**静默毁现场**的坑，单列一条。本 port 的 `eentry` 是
`addi.d $sp,$sp,-528` 之后直接在**被打断任务的栈上**建帧，没有独立异常处理栈。
所以 SVC 处理链（C 代码）本身就长在用户栈下面。第一版按「帧里的用户 sp − 帧长」
算信号帧地址，等于把地址指到内核 C 调用链正在用的那段栈上：`copy_to_user` 把
自己的返回地址一起盖掉，复制一返回 PC 就变成 0，报 `INE`（`ecode=0x0d`），
一条 check 都没打出来。

修法：以**当前内核 sp**（`move %0, $sp`）为基准往下让出 `帧长 + 128`，再与任务
栈底留 64 B 余量。返回链只会弹栈不会压栈，这个位置对内核是安全的。

读数：改前 `FATAL estat=0x000d0800 ecode=0x0d(INE) era=0`；改后 29/29 ok、
`exit code 138`。

### 实测读数（`[sig]` 行，QEMU LA64）

31 条 check：旧动作先回写后安装、`kill` 的返回值在处理函数跑完之后仍是 0、
handler 跑过 1 次且 `signo=10`、handler 里再 kill 自己造成嵌套（`depth=2`、
`nested signo=12`、嵌套那次 `kill` 返回 0）、`block` 之后不投递、
`unblock` 时才送到（`delivered on unblock = 2`）、`sigsetsize != 8` / `SIGKILL` /
无 restorer 三种拒绝都返 `-EINVAL` 且被拒的动作确实没装上、别的 pid → `-ESRCH`、
`sig=99` → `-EINVAL`、`tgkill` 的 `tgid != tid` → `-ESRCH`、`SIG_IGN` 静音、
最后还原 `SIG_DFL` 靠默认动作退出（`exit code 138` = 128 + 10），再加
**浮点寄存器区**两条：处理函数里读到的 `$f0` 仍是主程序装的图案、`sigreturn`
之后 `$f0` 也被整帧恢复成那个图案。全过才打 PASS 行。

LSX 档（`EXTRA_CFLAGS=-DARKRTOS_USE_LSX=1`）同样是 31 条，只是最后两条换成
**向量区**：处理函数里读到的 `$vr0` 是主程序装的 128 位图案、`sigreturn` 之后
`$vr0` 也被恢复。两条判据**不能同时存在**——`$f0` 就是 `$vr0` 的低 64 位，
handler 里「把 `$f0` 改成图案 B」会顺手改掉 `$vr0` 的低半，第一版就是这么
四条全红、而内核一处没错（同内核换回不带 `-mlsx` 的夹具立刻全绿）。所以
LSX 档只验 `$vr0`：它的 128 位图案已经把低 64 位那一份（`$f0`）一起覆盖。

**反假绿（四处变异都实测判红）**：

1. 把分发器尾部的投递点注释掉 ⇒ 门禁 exit=1：`[sig] check 通过条数 21 != 31`
   （当时门禁常量还是 29，报的是 21 != 29）、缺 PASS 行、缺 `exit code 138`。
2. 把 `frame_import` 改成只写回 `ERA`/`SP`（丢掉快照其余部分）⇒ 门禁 exit=1：
   `old handler was SIG_DFL = 33554800 FAIL`、`rt_sigprocmask unblock rc = 12 FAIL`、
   `[sig] FAIL: 2 checks failed`、`exit code 1`。这条证明「整帧快照」不是装饰：
   少存寄存器，处理函数一返回现场就是坏的。
3. （LSX 档）把 `arkrtos_port_svc_frame_bytes()` 改成恒返 528 ⇒ 交付的快照
   不含向量区：`lsx vr0 seen in handler = 1 ok` 而
   `lsx vr0 restored after sigreturn = 0 FAIL`、`[sig] FAIL: 1 checks failed`、
   `exit code 1`。这说明「sigreturn 之后恢复」那条判据对**向量区那一半字节**
   有分辨力，不是靠前半段顺带过的。
4. （LSX 档）把 `LA_FP_RESTORE` 里的 `vld $vr0, \base, 528` 改成读 `512`
   （落在 FCSR 槽上）⇒ 两条一起红（`seen = 0`、`restored = 0`），并且连带
   `[lsx] A bad=50 | B bad=50 -> FAIL (vector regs leaked between tasks)`——
   同一条恢复路径在任务切换上也坏了，佐证红的是内核那条指令而不是夹具。

还原后复验：默认档 31/31 ok、LSX 档 31/31 ok，两边 `exit code 138`，
门禁 13/13 全绿，格式门禁 392 个文件干净。

### 边界

- **跨任务投递已支持**（2026-11-23）：`kill`/`tkill`/`tgkill` 按 pid 找到目标进程槽与
  任务，置目标的 `sig_pending`；目标若正卡在阻塞 `wait4` 上就
  `arkrtos_task_wakeup_internal()` 唤醒它，信号在它下一次系统调用返回时送达。
  同任务自我投递仍走同一条路（pid == 自己）。
- **无抢占式投递**：只在系统调用返回点送。程序若一直不调系统调用，信号就一直
  pending（`kill` 返 0 但 handler 不跑）。
- 无实时信号（1..31 之外一律 `-EINVAL`）、无 `sigaltstack`、无 `SA_RESTART`
  语义（被信号打断的系统调用不会自动重试）。`siginfo` 与三参派发已有（U-31，
  §8.19）：`SA_SIGINFO` 时 handler 收 `(signo, siginfo_t *, ucontext_t *)`，
  `si_code` / `si_pid` / `si_band` / `si_fd` 按来源填；同一信号**不排队**（只留
  最后一次来源），`si_uid` 恒 0。
- 作业控制类（`SIGSTOP`/`SIGTSTP`/`SIGTTIN`/`SIGTTOU`）的默认动作按忽略处理：
  没有「另一个进程」可以停。
- 帧里带 FPU 区（528 B 档实跑验过）；LSX 档整块 1040 B 也一起拷，且**向量区已单独
  实测**（两条判据见上）。LASX（`$xr0..$xr31`）未保存。

## 8 fork（220 `clone`）+ wait4（260）：父子进程（2026-11-23，QEMU LA64 实跑）

`ARKRTOS_USE_LINUX_FORK`（默认关）接上「一个进程变成两个」：`clone` 复制整份
地址空间、造一个新任务，子进程从返回处读到 0，父进程读到子任务号；`wait4`
把退出码交回父进程。它依赖三层，缺一层 `features.h` 直接 `#error`：
`ARKRTOS_USE_LINUX_ABI`（号表）、`ARKRTOS_USE_EXIT_CODE`（退出码上报）、
`ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND`（可复制的页窗与
换窗口延迟机制 U-2）。

| 项 | 现状 |
|---|---|
| `clone` 认的参数 | `flags == 0` 或 `SIGCHLD(17)`，且 `child_stack == 0`；其余一律 `-EINVAL`（没有 `CLONE_VM`，所以没有线程） |
| 地址空间 | `arkrtos_paging_space_clone()` 逐 8 KB 对拷贝父空间；子进程拿到**独立**的帧池空间，父子的写互不可见 |
| 子进程的起点 | 复制父进程此刻的 trap 帧、把 `a0`（返回值槽）清 0：子进程从 `clone` 的返回处继续，读到 0 |
| 任务号 | 子进程是新 TCB（`is_priv = 0`），`generation` 与同号旧实例区分；`getpid` 读 `current_task_id` |
| 复制先后 | 先复制空间与帧、再进临界区注册任务并打补丁（与 Linux `copy_process` 把 `wake_up_new_task` 放最后同理：不让半配好的任务进就绪集） |
| `wait4` 认的参数 | `options == 0` 或 `WNOHANG(1)`；`pid` 必须是自己登记过的子 |
| `wait4` 返回 | 子活着 + `WNOHANG` → `0`；子活着 + 无 `WNOHANG` → **睡下等**（可重启系统调用，见 §8.1）；不是自己的子 → `-ECHILD`；子已死但退出码没上报 → `-EIO`（绝不编造 0 或 SIGKILL）；100 ms 超时唤醒后重扫仍无退出码且子任务已死 → `-EIO` |
| `status` | `(code & 0xFF) << 8`，`copy_to_user` 写 4 字节；写失败回 `-EFAULT` 且**不清**记账（可以重试） |
| 记账 | `s_child[4]`（`LX_MAX_CHILD`）；reap 成功后 `used = 0`；同号新实例靠 `generation` 认出来 |
| 实际上限 | 记账表 4 条，但真正的闸门是地址空间数（本板 `ARKRTOS_PAGING_APPS_MAX 2`）：父子占满 2 个空间，再 `clone` 回 `-EAGAIN`（`ARKRTOS_ELF_ERR_BUSY` → `-EAGAIN`，是「并发上限」不是「内存不够」） |

关档时 `arkrtos_elf_fork` / `arkrtos_port_paging_space_clone` /
`arkrtos_port_paging_write_space` / `arkrtos_port_paging_clone_frame` 全部不编译，
`nm` 上零符号（门禁 B 项实测）。

### 为什么延迟换窗口（U-2）在这里必须成立

`clone` 与 `wait4` 都发生在父进程**正站在自己的窗口里**跑系统调用的时候：
此刻它恢复现场要用的那一帧（以及返回地址）正压在同一个 VA 上。所以
`arkrtos_mpu_set_app()` 只登记 `s_pg_pending_space`，真正的 `activate()` 由
汇编先把 `sp` 换到内核静态栈、再调 `la_switch_prepare()` 补做。

### 实测到的一个真缺陷：不能用「帧地址相等」判断「没切」

第一版 `la_switch_prepare()` 用 `new_frame == old_frame` 判「调度器把帧原样
交回来了」，在 fork 上翻车：**父子共享同一套虚拟地址**，父进程在 `wait4` 上的
trap 帧与子进程保存的帧地址可能完全相同（实测都是 `0x21fbdc0`），于是被判成
「没切」、窗口留在父空间——子进程却在父窗口里继续跑，它的每一次 `write` 都回
`-14`（`-EFAULT`：`space_of(child) != 当前窗口 space`），随后沿父进程的代码
继续，父进程看起来「卡死」。表现是约 1/3 概率失败，且失败run 里
`TASK: task 12 exits` 一行都不出现（子进程根本没走完自己的路径）。

修法：加 `s_pg_commit` 显式标志——真正做出切换决定的两处（`la_trap_c()` 出口、
`la_switch_inline()` 过栈帧闸门之后）置 1，`la_switch_prepare()` 消费并清零，
判据改成 `commit == 0` 就当没切。改完连跑 6/6 + 3/3 全过、`rc=-14` 归零。
教训与 Linux / RT-Thread 的调度器一致：**「切没切」要由做出决定的那个点说话，
不要事后从一个可能恰好相等的外部读数反推**。

### 8.1 阻塞 `wait4`：可重启系统调用（Linux `ERESTARTSYS` 同型）

第一版想的是「睡一会儿再重扫」，实测**整机静默死锁**：本内核在 SVC 处理程序内部
没有真正睡眠的通路——`arkrtos_task_block_in_critical()` 只是「置 WAIT + 从就绪集摘掉
+ 武装延迟唤醒 + 置 `s_switch_pending`」，真正切走发生在 `la_trap_c()` 末尾；从 SVC
里调它只是把父自己摘出就绪集、然后**继续在关中断的热自旋里跑**，子进程一次都没被
调度。症状是日志停在 `dbg parent entering blocking wait4`，`TASK: ... exits` 一行都
不出现。

改法照 Linux 的做法：**睡下之后返回，让那次系统调用被重放**。

- 端口层新增 `arkrtos_port_svc_restart(void *p_exc_ctx)`：LA64 实现把帧里的 `ERA`
  减 4（退回 `syscall` 指令），下次该任务被调度回来时重新执行同一条 `syscall`；
  `hal.h` 里的弱定义返 `-1`（不支持），ABI 层收到 `-1` 就如实回 `-EAGAIN`，
  **绝不假装阻塞过**。
- 参数寄存器在 trap 帧里没被改写 ⇒ 重放读到的是同一组 `pid`/`status`/`options`。
- 唤醒路径两条：① 子进程退出时 `arkrtos_linux_abi_on_task_exit()` 给父置
  `SIGCHLD` pending 并 `arkrtos_task_wakeup_internal(父)`；② 100 ms 兜底超时
  （`LX_WAIT_SLICE_MS`）——兜住「子进程死在故障路径、不走退出钩子」的情形，
  醒来重扫发现「死了但没有退出码」就判 `-EIO`。
- `s_lx_wait[task]` 标记「卡在 wait4」，跨任务 `kill` 据此决定要不要唤醒。

代价写在明面上：这是「重放整个系统调用」而不是「从睡眠点续跑」，语义上等价
（`wait4` 是幂等的扫描），但**每次唤醒都要重扫一遍子表**；`wait4` 之外的阻塞系统
调用（`read` 空管道）**没有**接这条通路；`futex` 的 WAIT 用的是同一条重放机制（§8.4）。

### 8.2 写时复制（`ARKRTOS_USE_PAGING_COW`，2026-11-23，QEMU LA64 实跑）

fork 默认仍是「整块克隆」（逐 8 KB 对搬字节）。打开 `ARKRTOS_USE_PAGING_COW`
后改成**共享 + 第一次写才复制**：`space_clone()` 一个字节都不搬，把父的每一对
直接指给子（引用计数 1→2）、把父窗口里共享的条目降成只读（WE=0）；哪一方先写，
就命中一条只读条目，`la_pg_cow_fault()` 把那一对复制成自己的、装回可写、`ertn`
回到出错的那条指令。实现边界写死：

| 项 | 现状 |
|---|---|
| 粒度 | **8 KB 对**：一条 PS=12 条目同时给出 lo0/lo1，WE 位整条共用，这套映射表达不出「一页可写一页只读」；多抄 4 KB，不多看见一个字节 |
| 引用计数 | 按 4 KB 帧号（帧池 first-fit 不保证 8 KB 对齐），一对的偶数帧号就是它的句柄；**`ref > 1` 即共享**，不另存布尔位 —— 多一个位就多一处能和计数对不上的地方 |
| 只读条目 | `LA_PTE_APP_RO` = 去掉 WE；故障码两种都认：V=0 是 PIS(2)，**V=1 且 WE=0 是 PME(4)**（QEMU LA64 实测报 4），PLV0 与 PLV3 一视同仁 |
| 栈页 | **强制私有**：fork 时立刻给子复制一份（日志里 `stack=<n>` 是这一步的对数）。理由见下；`copied=0` 仍然成立（`copied` 只表示 swap 槽的实体复制） |
| 缺页回装 | 按需档每次切空间都把窗口翻译整段丢掉，重填时**共享对必须按只读装** —— 否则 COW 只剩 fork 那一刻的一层只读，进程切回来第一次写就绕过它 |
| 上限 | 没有 swap 时 COW 只是「延迟复制」：两边都写过之后各有一份实体帧，2 个地址空间 × 2 MB 仍受帧池（2048 帧）约束 |

**为什么栈页必须私有。** 本内核的异常入口 `eentry` 第一件事就是
`addi.d $sp,$sp,-256` 然后往帧里写通用寄存器 —— **trap 帧压在 App 自己的栈上**
（与 Linux「进异常先换内核栈」的做法不同）。如果栈页被降成「共享只读」，这一次
PLV0 写就是 PME，而且**帧还没落地**：C 处理器（`la_trap_c`）根本没机会跑，嵌套
异常直接把机器停掉（实测 `FATAL ecode=0x4 era` 落在 `eentry` 里、随后
`[STK] ... stack bottom flag overwritten`）。所以 fork 时把与任务栈相交的对立刻
复制给子进程、父保持可写，两边都不留「共享只读」的时间窗。这条边界在打开 COW 时
**必然存在**，不是可调参数；要改只有先给 trap 换独立内核栈（那是另一级改动）。

读数（`tools/fork_gate.py` 的 fork+COW 档）：52 条 check 全 `ok`，其中 COW 段 5 条
—— `cow child saw pre-fork values (bits=3) -> 3 ok`（子进程看到的还是 fork 前的值）、
`parent cow write kept -> 48879 ok`（父的写只落在自己那份）。内核日志断言四条：至少
一条 `[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>`（要求 `shared>=1`、
`stack>=1`、`copied==0`）、至少一条 `[LA64U] pg cow copy idx=<n> ref=2`、不许出现
`ref=1` 的 copy 行（没共享也复制就是白搬字节）、旁路演示读数仍在。门禁跑两遍
（先 fork 档 47 条、再 fork+COW 档 52 条），**不把默认的整块克隆路径从测试里换掉**。

修这一档时抓到两个真根因，都是「编译期与静态检查看不出来」的那种：① 按需档切空间
会把窗口翻译整段丢掉，重填若一律装可写，COW 只剩 fork 那一刻的一层（表现为父进程
的全局被子进程写脏、内核一条 `pg cow fault` 都不打）；② 写只读页是 **PME(4)** 不是
PIS(2)，只认 2 会把它当越权打进致命路径。

### 8.3 `MAP_SHARED` 匿名共享页（`ARKRTOS_USE_PAGING_SHM`，2026-11-23，QEMU LA64 实跑）

COW 只管「fork 之后各写各的」这一半；要让父子看见彼此的字，得有另一条显式的共享
通路。打开 `ARKRTOS_USE_PAGING_SHM`（必须叠在 COW 档上，`#error` 顶住）后，
`mmap(MAP_SHARED|MAP_ANONYMOUS)` 在**映射时刻**就把每一对 8 KB 建成实体、清零并
标成共享：

| 项 | 现状 |
|---|---|
| 粒度 | 仍是 **8 KB 对**：4 KB 的共享映射独占一对，同一对里再落私有映射会被整段拒绝（`-ENOMEM`） |
| 建页时机 | **mmap 时刻**。拖到 fork 才让父子各自缺页，两边各拿一帧，「共享」就退化成两次私有分配 —— 那种实现父子都不报错，只是各改各的 |
| 账本 | 共享位单独一张表（`s_pg_pair_shr[space][idx]`）。`ref > 1` 只说明「有人在共享」，分不出「COW 复制出来的共享」与「本来就要共享」 |
| fork | 共享对只 `ref++`：不降只读、不栈私有化、不复制；重填与装空间时按可写装条目；写故障与 `write_space` 直接跳过这一对 |
| 继承 | 只随 `fork` 继承。没有 `shm_open`，共享映射一律匿名；关档时 `MAP_SHARED` 一律 `-ENOSYS` |
| 非法形态 | 不带 `MAP_ANONYMOUS`、同时给 `PRIVATE\|SHARED`、`MAP_SHARED` 配 `MAP_FIXED` 都是 `-ENOSYS`（本内核的统一口径：不支持是 `ENOSYS`，不是 `EINVAL`） |

读数（`tools/fork_gate.py` 的 fork+COW+SHM 档，共 60 条 check）：SHM 段 8 条 ——
两条非法形态回 `-38`、`mmap(MAP_SHARED|ANON) > 0`、`shared page starts zeroed -> 0`、
`clone for shm child > 0`、`parent saw child shared write -> 300`、`shm child exit reaped -> 12`、
`shm child saw parent post-fork write (bits=1) -> 1`。内核日志断言另加一条
`[LA64U] pg shm map va=0x<hex> pairs=<n>`（要求 `pairs >= 1`）。同一轮里
`[LA64U] pg cow fork shared=<n> ...` 的 `shared` 比纯 COW 档多 1 对（共享对也算共享）。

**为什么共享位要单独存。** 「`ref > 1` 就是共享」这句话只对 COW 成立：一个
`MAP_SHARED` 的对在只有父子两个空间时 `ref` 也是 2，但它第一次写**不能**复制
（复制就等于把共享拆了）。所以不能从引用计数反推，必须另存一位。

**顺手修掉的一条真缺陷：`write(1/2)` 没有独占控制台。** 加 SHM 后子进程的 COW
日志能插在父进程一行 `[fork] ... -> 1` 与 ` ok` 之间，门禁看起来像「少了一条读数」
——读数没少，是行被劈开了。现在 `lx_write` 整次调用持控制台锁
（`arkrtos_console_lock()` / `arkrtos_console_unlock()`），内核日志拿同一把锁，
二者不再互相切行。

**判据侧的顺序是构造出来的，不是碰出来的。** 「父 fork 之后写、子在之后读到」这一条
原先让子进程在纯用户循环里自旋等 4M 次，赌「父进程能在子自旋期间拿到 CPU」。
一次 clone 之后子进程常常先跑，而用户态的自旋循环里没有任何让出点 ⇒ 这一赌会输：
偶发读到 `bits=0`，看着像「共享对被 private 复制」，其实是夹具在赌调度顺序。
现在子进程在一条**放行管道**上**阻塞读**——U-26 之后管道默认阻塞（空管道
`read` 挂起等到有人写，见 §6），所以那一次 read 会一直等到父进程写下放行字节才返回
（循环仍保留上限并接受 `-EINTR`，那是防御路径，不是轮询）——
父进程写完 200 再写一个字节放行；「父看到子的 300」也改成在 `wait4` 返回之后读（子那时已经退出，
没有竞态）。同一手法用在阻塞 `wait4` 那一段：父先读起点时刻，再放子进程去睡 60 ms
—— 否则「子已经跑掉一部分」会让读数偶尔变成 `blocking wait4 waited 2 ms`，
看起来像 wait4 没阻塞。改完 fork+cow+shm 档 3/3 连跑稳定。

### 8.4 `futex`：十一档 op（`ARKRTOS_USE_LINUX_FUTEX`，2026-11-23 起，2026-11-24 扩到第二档，2026-11-26 扩到 PI 三档，2026-11-27 扩到条件变量的 PI 变体，QEMU LA64 实跑）

`clone` 造出了第二条执行流，`MAP_SHARED` 让两边看见同一块内存，但**「等一个值变了再继续」**还缺一块：
没有它，pthread 的 mutex / condvar / sem 一个都起不来。futex 就是这块砖——它不是「内核替你同步」，
而是「值没变我就睡，变了就叫我」：绝大部分同步操作在用户态完成，只在真的要让出 CPU 时才进内核。

打开 `ARKRTOS_USE_LINUX_FUTEX`（`depends on ARKRTOS_USE_LINUX_ABI`）后，98 号落四批共十一档：
第一批是 `FUTEX_WAIT` / `FUTEX_WAKE`（2026-11-23）；第二批是把 glibc 的 condvar 真正走的那条路
补齐——`FUTEX_REQUEUE` / `FUTEX_CMP_REQUEUE` / `FUTEX_WAIT_BITSET` / `FUTEX_WAKE_BITSET`
（2026-11-24），外加 `timeout` 与 `-ETIMEDOUT`；第三批是 glibc 默认互斥走的 PI 三档
`FUTEX_LOCK_PI` / `FUTEX_UNLOCK_PI` / `FUTEX_TRYLOCK_PI`（2026-11-26，细节见 §8.4b）；第四批是
**PI 互斥配条件变量**那条路——`FUTEX_WAIT_REQUEUE_PI` / `FUTEX_CMP_REQUEUE_PI`（2026-11-27，
细节见 §8.4c）。
缺 `WAIT_BITSET|CLOCK_REALTIME` + `CMP_REQUEUE` 里的任何一个，pthread 的 condvar 就仍然
跑不起来；缺 PI 三档，`pthread_mutex` 的默认配置一碰就停摆——所以这几批都是按「glibc 走哪条路」
一次做全的。

| 项 | 现状 |
|---|---|
| op | `WAIT`(0) / `WAKE`(1) / `REQUEUE`(3) / `CMP_REQUEUE`(4) / `WAIT_BITSET`(9) / `WAKE_BITSET`(10)。`FUTEX_PRIVATE_FLAG`(128) **接受但剥掉**——本实现的键本来就是物理的，私有与进程间是同一件事；`FUTEX_CLOCK_REALTIME`(256) 只允许配 `WAIT_BITSET`，且因本机没有墙钟，**带超时时如实回 `-ENOSYS`**（不拿开机时长冒充绝对时刻）；PI 三档 `LOCK_PI`(6)/`UNLOCK_PI`(7)/`TRYLOCK_PI`(8) 见 §8.4b，条件变量的 PI 变体 `WAIT_REQUEUE_PI`(11)/`CMP_REQUEUE_PI`(12) 见 §8.4c；仍如实 `-ENOSYS` 的是 `FUTEX_FD`(2)/`WAKE_OP`(5) 与 13/14——不猜 |
| 键 | **物理帧号 + 页内偏移**，不是 VA。每个进程有自己的 8 KB 对窗口，同一个 VA 在父子身上指不同帧；拿 VA 当键会让两个无关进程在同一个 VA 上互相叫醒——那种错法不报错，只偶发多醒。窗口之外（栈、内核直映射区）查不到帧 ⇒ 如实 `-ENOSYS`，不拿 VA 冒充 |
| 睡眠 | 本内核没有无限睡眠：登记后 `arkrtos_task_block_in_critical(LX_WAIT_SLICE_MS=100 ms)` + `arkrtos_port_svc_restart()`，与 §8.1 同一条可重启系统调用通路。醒来先看 `s_futex_woke`（有人叫过就回 0），再看登记下来的截止时刻（到了就回 `-ETIMEDOUT`），否则重读一次字：值变了就当被叫醒，没变继续睡一个时间片；**不传 timeout 时永不超时**（与 Linux 一致）。超时存的是**截止时刻**、不是时长：相对超时只从第一次进入算起，每次时间片重放重新加一遍的话永远也到不了期。`WAIT` 的 `timeout` 是**相对**的，`WAIT_BITSET` 的 `timeout` 是**绝对**时刻（与 Linux 一致，本内核时钟是单调毫秒）。端口不支持重放时回 `-ENOSYS` 并清槽，**绝不假装睡过**。重放的那一次 a0 已被返回值清成 0（见坑 ②），所以「读哪个字、期望值是多少」都用登记下来的那一份，而不是重放时读到的参数 |
| `-EAGAIN` / `-EINTR` | 入口读到的字 != `val` ⇒ 立即 `-EAGAIN`（不睡）；有未屏蔽待投递信号 ⇒ 立即 `-EINTR`。重放那一次也再查一遍信号，所以信号能把已经睡着的等待者叫回来 |
| 等待槽 | 每任务一槽（`s_futex_key / s_futex_used / s_futex_woke / s_futex_uaddr / s_futex_val / s_futex_bitset / s_futex_deadline / s_futex_timed[ARKRTOS_TASK_MAX_NUM]`）。`uaddr`/`val` 记的是「上一次真正睡下时的地址与期望值」——重放时 a0 已经被返回值清成 0，不存下来就只能拿 0 去读字；`bitset` 记位集（普通 WAIT 登记全 1，所以普通 WAKE 能叫醒它），`deadline` 记截止时刻，`timed` 是「有没有超时」的独立标志（**不拿 0 当哨兵**：单调时钟从 0 开始，0 是合法截止时刻）。一个任务睡着时不可能再发起一次 WAIT，一槽够用；任务退出时 `on_task_exit` 清槽（八格一起清），否则任务号复用后新任务会继承一条「已经在等」的旧记录 |
| `WAKE` / `WAKE_BITSET` | 扫所有任务，同键且登记中的逐个置 `s_futex_woke=1` + `arkrtos_task_wakeup_internal()`，最多 `val` 个，返回实际唤醒数；没有等待者返 0（与 Linux 同）。`WAKE_BITSET` 另按 `val3` 掩码筛：位集为 0 回 `-EINVAL`，只有 `bitset & mask` 非 0 的等待者会被叫醒；普通 `WAKE` 不看位集，所以它能叫醒按位集睡着的人（与 Linux 一致） |
| `REQUEUE` / `CMP_REQUEUE` | Linux 把 `nr_requeue` **塞在 `timeout` 那一格**（第 4 个参数）、`uaddr2` 在第 5 格，这里照抄这个复用：先唤醒 `val` 个，再把剩下的最多 `nr_requeue` 个改挂到 `uaddr2`，返回值是**唤醒数加改挂数**（`man futex` 的措辞是 "woken up or requeued"）：Linux 的调用方靠这个数判断「挂过去了没有」，`nr_wake=0` 的 `CMP_REQUEUE` 若只回 0，glibc 的 `pthread_cond_broadcast` 就无从判断。`CMP_REQUEUE` 动手前先核对 `uaddr` 上的字等于 `val3`，不等回 `-EAGAIN`；缺 `uaddr2` 回 `-EINVAL`。被改挂的等待者把**期望值更新成 `uaddr2` 上的当前字**：Linux 里 requeue 不叫醒人，而本内核靠时间片重放回到用户态，若不更新期望值，重放时会因「两个地址的字不同」被误判成「已唤醒」——这是模型内的必要偏差，写在代码注释里。同理，「这次是不是重放」按**自己那一槽还在用**判定，不按地址比对：改挂已经把键搬到 `uaddr2`，按地址会比不出自己（详见本节②的另一半） |
| 检查顺序 | 一次 WAIT 醒来依次判：有人 `WAKE` 过 → 超时到期 → 值变了（当叫醒） → 有未屏蔽信号（`-EINTR`） → 继续睡。顺序即优先级：`WAKE` 优先于超时（同一刻既被叫醒又到期时，Linux 回 0） |

读数（`tools/fork_gate.py` 的 fork+COW+SHM+futex 档，**147 条 check**，其中 futex 段 87 条 —— 9 条基本配对 + 7 条多时间片 + 20 条第二档（位集 / 超时 / 改挂）+ 37 条第四档（PI 三档与 robust list，见 §8.4b）+ 14 条第五档（PI 条件变量，见 §8.4c））：

| 读数 | 钉住什么 |
|---|---|
| `futex mmap shared page > 0 -> 1` | 等的那块字确实来自一段共享映射（不是栈、不是私有页） |
| `futex(WAKE) with no waiter == 0 -> 0` | 没有等待者时不虚报唤醒数 |
| `futex(WAIT) on a different value == -EAGAIN -> -11` | 值已变就不睡（Linux 的 fast path） |
| `futex LOCK_PI on a free word == 0 -> 0` | 空闲字上 `LOCK_PI` 立即拿到并把本任务 tid 写进字里（PI 三档与 robust list 的 37 条读数见 §8.4b；原先这一格判的是「`LOCK_PI` 必须回 `-ENOSYS`」，PI 落地后那条判据反过来会判红，已删除）|
| `clone for futex child > 0 -> 1` | 唤醒者是另一个真实进程，不是同一任务的自我唤醒 |
| `futex(WAIT) returned 0 after being woken -> 0` | 父进程真睡下并被叫醒。第一次进入时期望值 1；被子进程抢先改值时按 Linux 语义回 `-EAGAIN`，夹具拿当前值重睡（最多 4 轮）——所以这一条判的是「真睡下 + 真被叫醒」，不是「父进程一定先走到 WAIT」。失败时子进程用 SIGCHLD 打断，读数是 `-EINTR`，不会挂死 QEMU |
| `parent saw the value the waker wrote -> 2` | 醒来后看到的是唤醒者写的字（共享页真生效，不是私有副本） |
| `futex child reaped -> 12` | 唤醒者正常退出并被 reap |
| `futex waker saw 1 and woke 1 (bits=3) -> 3` | bit0 = 子进程读共享页时看到的还是父进程要等的那个 1（证明父子读写的是同一个字，共享帧生效）；bit1 = `WAKE` 真的叫醒了 1 个 |
| `futex multislice mmap shared page > 0 -> 1` | 多时间片那组用的共享页确实建起来了 |
| `clone for futex multislice child > 0 -> 1` | 对应的唤醒者是另一个真实进程 |
| `futex multislice WAIT returned 0 after being woken -> 0` | 跨过至少一个 100 ms 时间片之后 WAIT 仍回 0 —— 修复前这里会变成 `-EFAULT`（重放拿 0 去读字），所以这一条是把「一次正常的长时间等」钉死 |
| `futex multislice parent saw the value the waker wrote -> 2` | 跨时间片醒来后看到唤醒者写的 2（读的是登记地址，不是 0） |
| `futex multislice WAIT spanned >= 150 ms (crossed a slice) -> 1` | 从进入 WAIT 到返回的墙钟 >= 150 ms —— 证明真的跨了时间片，而不是一次命中（否则这一档等于没测到重放） |
| `futex multislice child reaped -> 12` | 多时间片的唤醒者被正常 reap |
| `futex multislice waker saw 1 and woke 1 (bits=3) -> 3` | 同一组证据位在跨时间片场景下仍成立 |
| `futex2 mmap shared page > 0 -> 1` | 第二档那组用的共享页确实建起来了 |
| `futex(WAIT_BITSET) with bitset 0 == -EINVAL -> -22` | 位集为 0 被拒（Linux 同）——不把空集当成「匹配所有」 |
| `futex(WAKE_BITSET) with bitset 0 == -EINVAL -> -22` | 同上，唤醒侧也不接受空集 |
| `futex(WAKE_BITSET) with no waiter == 0 -> 0` | 带位集的唤醒同样不虚报唤醒数 |
| `futex(WAIT) with zero relative timeout == -ETIMEDOUT -> -110` | 相对超时 0 必须当场到期（如果只把超时当「睡多久」，这里会白睡一个时间片） |
| `futex(WAIT_BITSET) with absolute past == -ETIMEDOUT -> -110` | 绝对时刻已过必须当场到期——把绝对时刻当相对时长读的实现会在这里睡下去 |
| `futex(REQUEUE) with no waiter == 0 -> 0` | 没有等待者时改挂回 0 |
| `futex(CMP_REQUEUE) with mismatched value == -EAGAIN -> -11` | `CMP_REQUEUE` 真的核对期望值（这是 glibc condvar 防丢唤醒的那一环）|
| `futex(REQUEUE) without uaddr2 == -EINVAL -> -22` | 缺 `uaddr2` 被拒，不当成「原地改挂」 |
| `futex(WAIT) with 50 ms timeout == -ETIMEDOUT -> -110` | 相对超时真的到期（跨过一个 100 ms 时间片后回 `-ETIMEDOUT`，不是继续睡）|
| `futex(WAIT) timeout spanned >= 50 ms -> 1` | 这次等待的墙钟 >= 50 ms——证明不是一次参数校验就返回 |
| `clone for futex2 bitset child > 0 -> 1` | 位集用例的唤醒者是另一个真实进程 |
| `futex(WAIT_BITSET) returned 0 after being woken -> 0` | 按位集睡着的父进程真的被相交掩码叫醒 |
| `futex2 bitset waker saw 1, woke only matching (bits=7) -> 7` | bit0 = 子看到父等的还是 1；bit1 = **不相交掩码（1 vs 2）没有叫醒它**；bit2 = 相交掩码叫醒了 1 个。位集被忽略时 bit1 就没了（子第一次 `WAKE_BITSET(1)` 会回 1），所以这一条是位集的真判据 |
| `clone for futex2 requeue child > 0 -> 1` | 改挂用例的唤醒者是另一个真实进程 |
| `futex(WAIT) on requeued word returned 0 -> 0` | 被改挂的父进程在新地址上被叫醒并回 0（重放时从槽里取回的是**改挂后**的地址与期望值）|
| `futex2 requeue: old word has no waiter left -> 0` | 改挂之后旧地址上 `WAKE` 回 0——等待者真的搬走了，不是「两个地址都能叫醒」 |
| `futex2 requeue child saw 1 and new-word WAKE woke 1 (bits=3) -> 3` | bit0 父等的还是 1；bit1 新地址上的 `WAKE` 真的叫醒了 1 个。改挂没生效时新地址永远叫不醒，子进程 200000 次后改投 SIGCHLD，读数是 `-EINTR` |
| `futex2 bitset child reaped -> <pid>` / `futex2 requeue child reaped -> <pid>` | 两组子进程都被正常 reap（任务号会复用，这两条按「形态」判，不钉具体数字）|

这一档的定序是**重试到收敛**，不拿调度当判据：用户态可被时间片抢占，子进程可能在父进程走到 `WAIT`
之前就跑完（实测踩到过一次：父 `WAIT` 回 `-EAGAIN`、子 `WAKE` 回 0，看起来像 futex 不配对）。现在的写法是
子进程循环「写 2 + `WAKE`」直到回 1，父进程在被抢值时拿当前值重睡；futex 真的不配对时两边都收敛不了，
读数仍是红的。

**三条只有上板才看得见的坑。**

① **共享映射的 8 KB 对粒度会吃掉地址**：一段 4 KB 的 `MAP_SHARED` 也独占整对，所以共享映射的
起址必须落在对边界、堆游标整对前进。第一版让游标只走 4 KB，第二个共享映射就叠进第一对的另一半，
被端口按「这一段已经有内容」拒绝，形式上回 `-ENOMEM`——读起来像内存不够，实际是对齐。现在
`mmap` 的共享分支先 `lx_pair_up(start)`、写完 `s_heap_cur = lx_pair_up(end)`（留下的半个 4 KB 洞
不还给分配器：给出去就会与共享帧重叠）。

② **返回值寄存器就是第一个参数寄存器**：LA64 的 Linux ABI 里系统调用返回值写回 `a0`，而 `futex`
的第一个参数（`uaddr`）也从 `a0` 读。阻塞路径返回 0 时，分派器把 0 写进 `a0`，于是**重放的那一次
读到的 `uaddr` 是 0**。不能靠地址认重放，只能靠「我这一槽还登记着」：`uaddr == 0` 且本槽 `used`
为 1，就取回本任务记下的键。这不是理论洁癖——`wait4` 的重放同样把 `pid` 读成 0，只是 0 对 `wait4`
本来就是合法输入（等任意儿子），所以此前没有暴露；`futex` 是第一个必须把这件事说清楚的地方。
宿主用例里有一条专门喂这个形态：先登记一次 WAIT，再用 `uaddr=0` 重入 —— 必须被认成重放、用登记下来的
地址继续睡，而不是 `-EFAULT`。

**这一步最初是错的，而且被当成了期望值。** 最早的实现认出重放之后，仍用重放时读到的 `uaddr`（=0）
去 `copy_from_user` 读字 ⇒ 必然 `-EFAULT`，然后清槽返回。于是「一个超过 100 ms 还没被唤醒的 WAIT」
会以 `-EFAULT` 死掉 —— 恰恰是 futex 最正常的用法（等得久一点）。它没在上板夹具里暴露，是因为
基本配对那组子进程几乎立刻写值 + `WAKE`，父进程通常在一个时间片内就被叫醒；而宿主用例当时把
`-EFAULT` 写成了「期望值」，等于用测试把这个缺陷固化了下来。修法就是第 3 张表里的 `s_futex_uaddr /
s_futex_val`：登记时存 `uaddr`、`val`，重放时取回它们再读字。改完宿主用例的那三条断言（继续睡 /
值变了当被叫醒 / 登记地址真读不出来才 `-EFAULT`）与 QEMU 的多时间片段一起把这个行为钉住。
这个缺陷值得单独记一笔，因为它的形态是「**修复前后都能跑、都返回一个像样的错误码**」——只有把
『等得久』当成一条正常路径去测，才会发现正常路径被当成了错误。

**② 的另一半：认重放的判据不能是「地址对得上」。** 修好「重放读哪个字」之后还剩下一个同源的坑：
`REQUEUE` 会把等待者那一槽的**键搬走**（`s_futex_key` 改到 `uaddr2`），而重放的那一次算出来的键仍来自
用户原样写下的旧地址 `uaddr`。早先 `resumed` 判的是「槽里的键 == 这次算出的键」，于是**被改挂过的
等待者醒来后认不出自己**：它掉进「第一次进入」分支，读到旧地址上的字仍等于期望值，就又在旧地址上睡下，
新地址上的 `WAKE` 再也叫不醒它。`fork` 门禁的 `fork+cow+shm+futex` 档就是这么判红的——读数 72 != 76，
串口停在 `clone for futex2 requeue child > 0` 之后、父进程在 `-ETIMEDOUT` 重试里转到运行器超时。
判据改成「**自己那一槽还在用**」：一个任务睡着的时候不可能再发起一次 `WAIT`，槽在用就只能是重放；
要比对的值也取槽里记的那一份（改挂时它已经更新成 `uaddr2` 上的当前字）。宿主用例原来用 `uaddr=0`
模拟重放，只走得到「a0 被返回值清 0」那条兜底分支，照不出这个形态——现在改成**按用户原样写下的
地址**重入，并追加一条「重放之后旧地址上 `WAKE` 仍回 0」，把「没有重新睡回旧地址」钉住。

③ **「一次 write」不等于「一行」。** §8.3 让 `lx_write` 持控制台锁，解决的是
「内核日志插进用户一行中间」；但夹具自己把一行拆成三次 `write`（前缀、数字、换行）时，
**同一把锁保护不了三次调用之间的空隙**——另一个进程的整行会插进来。全量门禁里就撞到了：
`[fork] parent child=` 与数字之间插进了子进程的 `[fork] child pid=12`，于是
`[fork] parent child=[fork] child pid=12`，按行读的门禁判定「缺了 child pid」，而每条读数
其实都打了。修法在夹具侧：带数字的行先拼进栈上缓冲，**一次 `write` 写出**（`out_line3` /
`out_line5`）。Linux 上的 `printf` 行缓冲做的是同一件事；RT-Thread 的 `rt_kprintf` 之所以
看起来没这个问题，是因为它整行在关中断下拼好再一次输出。判据侧不再为「谁先调度到」留余地。

宿主侧口径：`linuxabi` / `linuxabioff` / `linuxabifutex` / `linuxabifilemap` / `linuxabiselect`
五档并集行覆盖率 **98.22%**（基线 `tools/coverage_baseline.json` 钉的是 97.96%，本轮只往上走、没有调低也没有
`--update`），`linuxabi` 档 **270 checks**、futex 档 **434 checks**（2026-11-26 起，PI 三档与 robust
list 再 63 条）、filemap 档 **358 checks**、select 档 **327 checks**，都打印
`LINUX-ABI HOST OK`；`ARKRTOS_USE_PAGING_SHM=1` 的共享分支**不在宿主账内**
（宿主用例从未在 `LINUX_FORK=1` 下编过），那一半的口径只有上板 40 条 SHM 读数、36 条 futex
读数与 20 条 filemap 读数。剩余 24 行未覆盖经逐行核对全是死/防御分支（两处 `-EBUSY`
重入、`-ENOMEM` 的溢出与对齐变体、`-E2BIG`、嵌入式弱符号兜底等；列表见覆盖率门禁的
说明与本周的 `covmiss` 记录），不是「没测」。

### 8.4b futex 第四档：PI 三档与 robust list（2026-11-26，宿主 63 条 + QEMU LA64 37 条）

§8.4 解决的是「等一个值变了再继续」，但 glibc 的 `pthread_mutex` 默认配置是 **PI（优先级继承）**
互斥，走的是另一条路：拿锁 `FUTEX_LOCK_PI`、放锁 `FUTEX_UNLOCK_PI`、试锁 `FUTEX_TRYLOCK_PI`；
另外 `pthread_mutex_lock` 还要防「持有者死在临界区里，整条链永远停住」，那靠 99/100 号
`set_robust_list` / `get_robust_list` 与任务退出时的 robust 清理。两者的共同点是**内核必须知道
「谁持有、谁在排队」**——普通 futex 谁都能叫醒谁，PI 必须按优先级把锁**交给**最该拿到它的那个
等待者，否则高优先级任务会被低优先级持有者后面的中等优先级任务无限推后（优先级反转）。

| 项 | 现状 |
|---|---|
| op | `LOCK_PI`(6) / `UNLOCK_PI`(7) / `TRYLOCK_PI`(8)，与 §8.4 的六档并集九档。`FUTEX_PRIVATE_FLAG`(128) 同样接受并剥掉；`FUTEX_CLOCK_REALTIME`(256) 带超时时如实 `-ENOSYS`（本机没有墙钟） |
| 字布局 | 低 30 位 = 持有者 tid（`FUTEX_TID_MASK = 0x3FFFFFFF`）、bit31 `FUTEX_WAITERS = 0x80000000`、bit30 `FUTEX_OWNER_DIED = 0x40000000`。与 Linux 逐位相同——用户态（glibc）直接读这个字判断状态，差一位就是另一套 ABI |
| 优先级继承 | 登记/排队时把等待者的 `base_priority` 抬到持有者头上，放锁或交接时算回原值，统一走 `arkrtos_ready_reprio()` 这一个入口重排就绪集。**有效优先级的唯一真源是任务的 `base_priority` 加上「谁在等我」这份记录**，不另存一份会被自己中间值污染的快照——否则「抬两次算回一次」这种错误只在多等待者时偶发 |
| 交接而非叫醒再抢 | `UNLOCK_PI` 与 robust 清理都**直接**把字写成「接手者 tid | 保留位」，所有权当场转移，不「叫醒所有等待者让他们再抢一次」。后者正是优先级反转的成因，也是 PI 与普通 futex 的语义分界：反假绿要卡的正是这一点（退化成叫醒再抢时，QEMU 第 2 段的父进程会读到 `-EAGAIN` 或字里还是死者 tid，是红的读数而不是挂死） |
| 谁接手 | `lx_pi_top_waiter()`：先比 `base_priority`（数值小者优先），再比任务号（稳定序）。没有等待者就直接清字 |
| 等待与重放 | 与 §8.4 同一条可重启系统调用通路（100 ms 时间片 + `arkrtos_port_svc_restart()`）；重放同样从等待槽里取回 `uaddr` 与期望值，不看返回值寄存器。`LOCK_PI` 的 `timeout` 是相对时长（与 Linux 一致） |
| 错误码 | 同一任务重复 `LOCK_PI`、或自己持有时 `TRYLOCK_PI` ⇒ `-EDEADLK`(35)；非持有者 `UNLOCK_PI` ⇒ `-EPERM`；被别人持有且 `TRYLOCK_PI` ⇒ `-EAGAIN` **并置 `WAITERS`**（持有者靠这一位知道有人排队，否则永远不来交接）；睡着后被未屏蔽信号打断 ⇒ `-EINTR` |
| 死者接手 | 字里带 `OWNER_DIED` 时 `LOCK_PI` 直接接手，并把 `OWNER_DIED` **原样留给接手者**：用户态看到这一位才知道「前一个持有者死在临界区里」，要自己做一致性修复。内核替它清掉就不合规 |
| 登记时不读别人的地址空间 | PI 登记时把持有者 tid **抄进**内核自己的 `s_futex_pi_owner[]`，不跨进程读对方的字——跨进程读字随时可能读到半写状态，用来判「谁是持有者」不可信 |
| robust 链 | `set_robust_list(head, len)`：`head` 非空、8 字节对齐、`len >= 12`（`next` / `futex_offset` / `list_op_pending` 三格），否则 `-EINVAL`。链上每一项是 `{ next | PI位 }`，`next` 的最低位标记「这一项是 PI 互斥」；`futex_offset` 是**从节点自身算起的字节偏移**（`offsetof` 口径），不是元素数——坑 ① |
| 退出清理 | `on_task_exit` 里先 `lx_robust_exit()`（改写用户内存：逐条把链上的字写成 `OWNER_DIED`，PI 条目按 PI 语义交接给 `lx_pi_top_waiter()`），再 `lx_futex_forget()`（撤内核登记）。顺序不能反——先撤登记就没有等待者信息可交了。链按 `ROBUST_LIST_LIMIT = 2048` 条截断：死在退出钩子里等于整个系统停摆，比漏清一条严重 |
| 跨进程边界 | `get_robust_list(pid, ...)` 只认 `pid == 自己`，别人的 pid 如实 `-ESRCH`（本内核没有可查询的第二个进程身份，不假装能读别人的地址空间）；出参指针为空 ⇒ `-EFAULT` |
| 与 Linux 的差异 | 条件变量的 PI 变体 `WAIT_REQUEUE_PI`(11) / `CMP_REQUEUE_PI`(12) 不在本节，它们在独立实现里（§8.4c）；`FUTEX_CLOCK_REALTIME` 带超时 `-ENOSYS`；等待者选择只看 `base_priority`（deadline 类只作用于就绪选择，futex 等待队列不按截止时刻排序；调度策略见 §8.17） |

读数（`tools/fork_gate.py` 的 fork+COW+SHM+futex 档 113 条之内的 37 条；宿主侧 `t_futex_pi()` 另 63 条）：

| 读数 | 钉住什么 |
|---|---|
| `futex4 mmap shared page > 0 -> 1` | 第四档的共享页建起来了（长度按整页 8 KB 申请） |
| `futex LOCK_PI on a free word == 0 -> 0` / `futex LOCK_PI wrote our tid into the word -> <tid>` | 空闲字上拿到锁，且字里是本任务 tid（不是随意一个标记） |
| `futex LOCK_PI twice == -EDEADLK -> -35` / `futex TRYLOCK_PI while held == -EDEADLK -> -35` | 自己等自己必须当场回 `-EDEADLK`，不能睡下去 |
| `futex UNLOCK_PI as owner == 0 -> 0` / `futex UNLOCK_PI cleared the word -> 0` | 放锁成功且字被清干净（残留 tid 或 `WAITERS` 会让下一个持有者判错状态） |
| `futex UNLOCK_PI when not the owner == -EPERM -> -1` | 非持有者放锁被拒（Linux 同） |
| `futex TRYLOCK_PI on an owned word == -EAGAIN -> -11` | 被别人持有时试锁不睡、当场失败 |
| `futex TRYLOCK_PI set the WAITERS bit while failing -> 2147483648` | 失败时置 `WAITERS`——持有者靠这一位知道有人排队 |
| `futex TRYLOCK_PI left the owner tid alone -> 4242` | 失败时不动持有者 tid（字布局没被踩） |
| `futex LOCK_PI on a dead owner == 0 -> 0` / `... took over from the dead owner -> <tid>` | `OWNER_DIED` 字上直接接手，字里换成新持有者 |
| `futex LOCK_PI kept OWNER_DIED for the taker -> 1073741824` | 接手者看得见 `OWNER_DIED`（用户态才有机会修数据） |
| `futex UNLOCK_PI after consistent == 0 -> 0` | 用户态清掉 `OWNER_DIED` 后放锁成功 |
| `set_robust_list with a short length / unaligned head / null head == -EINVAL -> -22` | 三种坏 head 全被拒（长度不足、未对齐、空指针） |
| `set_robust_list on a well formed head == 0 -> 0` | 合法 head 登记成功 |
| `get_robust_list(self) == 0 -> 0` / `... reports the head we registered -> <addr>` / `... reports the length we registered -> 24` | 读回的就是登记的那一份（不是另存一份副本） |
| `get_robust_list on a foreign pid == -ESRCH -> -3` | 别人的 pid 如实拒绝，不假装能读 |
| `clone for PI robust child > 0 -> 1` | 跨进程 PI 的持有者是另一个真实进程 |
| `PI robust: parent observed the child holding the lock -> 1` | 握手字被写（子进程确实拿到了锁） |
| `PI robust: parent acquired after the holder died -> 0` | **本档最要紧的一条**：子进程带锁退出后父进程拿到锁——robust 清理跑了、且所有权是交接来的 |
| `PI robust: word holds the parent tid -> <tid>` / `... carries OWNER_DIED for the taker -> 1073741824` | 交接后字里是父进程 tid 且保留 `OWNER_DIED` |
| `PI robust child saw register+lock+tid+waiter (bits=15) -> 15` | bit0 登记 / bit1 拿锁 / bit2 字里是自己 / bit3 看到父进程排队（`WAITERS`） |
| `clone for non-PI robust child > 0 -> 1` | glibc 普通（非 PI）robust 互斥的持有者也是另一个真实进程 |
| `non-PI robust: parent observed the child owning the word -> 1` | 子进程**直接写字**持有（不经内核）这件事成立 |
| `non-PI robust: parent took over the dead owner's word -> 0 -> 0` / `... word carries OWNER_DIED after the death -> 1073741824` / `... word now holds the parent tid -> <tid>` | 非 PI 条目同样被退清：死者字变成 `OWNER_DIED`，父进程接手后字里换成自己 |
| `non-PI robust child registered and owned (bits=3) -> 3` | 子进程的登记与持有两条都成立 |

**三条只有上板才看得见的坑（夹具侧的契约错误，读起来都像内核 bug）。**

① **`futex_offset` 是字节偏移，不是元素数。** 夹具里变量都是 `unsigned long *`，第一版写
`rh[1] = 0x40ul` 想表达「往后 0x40 个元素」，内核按 Linux 的 `offsetof` 口径当 0x40 个**字节**
去读 ⇒ 打到 0x108940 而不是锁字 0x108B00 ⇒ 读到 0 ≠ 死者 tid ⇒ 清理静默跳过 ⇒ 父进程永久
等待。改成 `rh[1] = (const char *)lock - (const char *)nod` 之后整段变绿。这条错法的可怕之处
是**不报错**：算错偏移只是「读到别的字」，而「读到 0」在 robust 语义里恰好是合法中间态。

② **握手用的 futex，期望值必须写「未就绪态」，不能读当前值再传。** 两处「等子进程放行」的
`WAIT` 原来传 `v = *go`，而子进程可能已经放行过（字变成 1）——传 1 时内核判定「值没变、该睡」，
把已经发生的唤醒永远丢掉，父进程再也不可能被叫醒。改成固定传 0：字已经变成 1 就直接回
`-EAGAIN`，循环当场收尾。Linux 里用 futex 做握手都必须守这一条，差别只在于真 Linux 上它表现为
偶发挂死而不是必挂。

③ **共享页是 8 KB（0x2000）一对，`0x400` 个元素就跨出整页。** 非 PI robust 段原先把
`go2` 放在 `pw + 0x400`（= 0x2000 字节），恰好落在下一页；`mmap` 又只申请 4096 ⇒ 握手字
根本不在映射里，父进程在 0x0210a000 上以 `resumed=1 sval=0` 无限重放。改成 `mmap` 8192、
`go2 = pw + 0x380` 之后正常。同一段子里子进程原来是 4,000,000 次纯自旋等 `WAITERS`——单核
上父进程根本跑不到 `LOCK_PI`，读数 `bits=7` 缺 bit3；改成「`FUTEX_WAIT` 带 0.5 ms 超时」
（本机没有 `sched_yield`，这是唯一能主动交棒的调用）循环 2000 轮，读数变 `bits=15`。
教训：**等另一条执行流时必须真的让出 CPU**，纯自旋在单核上等于把对方饿死。

宿主侧：`t_futex_pi()` **63 条**断言钉机制（登记 / 阻塞 / 交接 / 唤醒语义 / 优先级抬升与算回 /
robust 参数面 / 退出清理的记账），跨进程面留给 QEMU 那 37 条——两者不重复：宿主能精确控制
「谁是持有者、谁在等」，QEMU 证的是「跨进程真的共用同一块物理帧、退出钩子真的跑到用户内存上」。

### 8.4c futex 第五档：条件变量的 PI 变体（2026-11-27，宿主 68 条 + QEMU LA64 14 条）

§8.4b 把 PI 互斥做全了，但 **PI 互斥配条件变量**还有一条专路：glibc 的 `pthread_cond_wait`
在互斥是 PI 型时走 `FUTEX_WAIT_REQUEUE_PI`(11)，`pthread_cond_signal` 走
`FUTEX_CMP_REQUEUE_PI`(12)。语义与前几档都不同：等待者**已经持有**互斥（`cond_wait` 先放锁、
再睡在条件变量字上），醒来时必须**直接拿到**互斥——所以不是「叫醒它让它自己再 `LOCK_PI`」
（那等于把 PI 语义丢掉、退回优先级反转），而是内核把互斥的字**当场改成它的 tid**、再叫醒它。

| 项 | 现状 |
|---|---|
| op | `WAIT_REQUEUE_PI`(11) / `CMP_REQUEUE_PI`(12)，与前面九档并集十一档。两者共用 §8.4b 的字布局（低 30 位持有者 tid / bit31 `WAITERS` / bit30 `OWNER_DIED`）；`FUTEX_PRIVATE_FLAG` 同样接受并剥掉 |
| 等待者登记 | `uaddr2` 必须**有人持有**，否则当场 `-EINVAL`——`requeue` 的目标是一把有人攥着的 PI 锁，没人持有就没有「接手」的位置（Linux 同判）；登记前把 `uaddr2` 的 `WAITERS` 位置 1，否则持有者的 `UNLOCK_PI` 永远不知道要交接。等待槽里多记一位 `s_futex_rqpi`：rqpi 也是 PI 类，普通 `WAKE` / `REQUEUE` **不碰它**（判据统一用 `s_futex_pi`），只有 `CMP_REQUEUE_PI` 按 `s_futex_rqpi` 整队搬 |
| 搬运 | `CMP_REQUEUE_PI` 依次：核对 `uaddr` 上的字等于 `val3`（不等 `-EAGAIN`）→ 核对 `uaddr2` 有人持有（否则 `-EINVAL`）→ **先**按 `nr_wake` 叫醒 `uaddr` 上的普通等待者（与 Linux 同序：先唤醒再搬，否则被叫醒的会抢先改字）→ 在 rqpi 等待者里挑基准优先级最高的当排头，把 `uaddr2` 的字写成 `排头 tid`（`OWNER_DIED` 原样带过，队里还有 rqpi 等待者且本次允许搬才留 `WAITERS`）→ 叫醒排头 → 其余在 `nr_requeue` 限额内改挂到 `uaddr2`（键、地址、期望值、持有者全部更新，持有者记成新排头，优先级继承算在真正攥锁的人头上） |
| 返回值 | **唤醒数加搬运数**（含排头），与 `man futex` 的 "woken up or requeued" 同口径：`nr_wake=0` 时调用方也能从返回值看出有没有搬成功 |
| 重放 | 与 §8.4 同一条可重启通路。排头醒来时 `s_futex_woke` 已置：直接销账并回 0，**不再读字**——所有权是内核刚写进去的，用户态可能已经把它改成 0 或别的位，再读只会误判 |
| 错误码 | 值不符 `-EAGAIN`；`uaddr2` 为空或没人持有 `-EINVAL`；两个地址任一在窗口外 `-ENOSYS`；读写用户内存失败 `-EFAULT`（写 `uaddr2` 失败时把叫醒撤销，不让排头拿着一个没落到内存里的「所有权」跑起来）；睡着后被信号打断 `-EINTR`；端口不支持重放 `-ENOSYS` |
| 与 Linux 的差异 | ① **没有 FIFO 等待队列**：排头按 `base_priority`（同级按任务号）选，Linux 按 rtmutex 的排队顺序。对 PI 语义这是更强的一致性（最该跑的先接手），但「谁先到谁先拿」这件事本内核不保证；② 睡眠按 100 ms 时间片重放（本内核没有无限睡眠），对外语义等价；③ `timeout` 只按**相对**时长解释，带 `FUTEX_CLOCK_REALTIME` 如实 `-ENOSYS`（无墙钟）；④ 混合队列按 `s_futex_rqpi` 标记分流——混在同一个字上用普通 `WAIT` 睡下的等待者只会被 `nr_wake` 叫醒、不会被搬走，Linux 对混合队列另有判定；⑤ 重新调度只看 `base_priority`（deadline 类不参与 futex 等待队列排序，见 §8.17） |

读数（QEMU LA64 这一档 14 条，全部在 `tools/fork_gate.py` 的 futex 档里）：

| 读数 | 钉住什么 |
|---|---|
| `futex5 mmap shared page > 0 -> 1` | 第五档的共享页建起来了 |
| `futex(WAIT_REQUEUE_PI) on a different value == -EAGAIN -> -11` | 值不符就不睡（与 `WAIT` 同口径，fast path 不落到搬运逻辑） |
| `futex(WAIT_REQUEUE_PI) with a free mutex == -EINVAL -> -22` | 目标锁没人持有时当场拒绝——不把「没人持有的锁」当成可接手的位置 |
| `futex LOCK_PI before the condvar handoff == 0 -> 0` | 交接前父进程自己是一把 PI 锁的真实持有者（后面 `WAITERS` / 交接都以它为前提） |
| `futex(CMP_REQUEUE_PI) with a mismatched word == -EAGAIN -> -11` | 搬运侧也核对期望值 |
| `futex(CMP_REQUEUE_PI) without uaddr2 == -EINVAL -> -22` | 缺 `uaddr2` 被拒 |
| `clone for condvar rqpi child > 0 -> 1` | 等待者是另一个真实进程 |
| `futex rqpi: child queued on the mutex (WAITERS) -> 1` | 子进程真的睡下并让内核替父进程置了 `WAITERS`（持有者靠这一位知道有人排队） |
| `futex(CMP_REQUEUE_PI) handed the mutex over == 1 -> 1` | 搬运计数回 1（含排头）——退化成「只叫醒不搬运」时这里是 0 |
| `futex(CMP_REQUEUE_PI) wrote the child tid into the mutex -> <tid>` | 所有权**当场转移**：字里是子进程 tid 而不是父进程的 |
| `futex(CMP_REQUEUE_PI) left no stray WAITERS -> 0` | 只有一个 rqpi 等待者时不留 `WAITERS`，否则新持有者的 `UNLOCK_PI` 会去交一把空队列 |
| `futex rqpi child saw the handoff (bits=7) -> 7` | 子进程三条位全置：被搬运唤醒（bit0）/ 字里是自己（bit1）/ 接手来的锁能正常放掉（bit2） |
| `futex rqpi child reaped -> <n>` / `futex rqpi mutex cleared after the inherited unlock -> 0` | 子进程正常退出，且它放掉的是**自己接手的那把锁**（字最终清 0，没有残留 tid） |

宿主侧：`t_futex_requeue_pi()` **68 条**断言（43 条 `ck_ret` + 25 条 `ck` / `ck_u`），覆盖 QEMU
上做不到的精确控制：重放继续睡 / CMP 期望值不符 / 双等待者的优先级选择（base 3 胜 base 9）/
改挂者重放与最终清 0 / `nr_requeue=0` 只搬排头（字 `6` 不带 `WAITERS`）/ 搬运写字失败 `-EFAULT`
且**不叫醒** / CMP 两处 `-EFAULT` / 无等待者回 0 且字不变 / WAIT 超时 0 与端口不可重放 /
读超时结构失败 / 信号 `-EINTR` / 重放三出口（`-ENOSYS`、`-ETIMEDOUT`、`-EINTR`）。

两个夹具侧的口径（不写下来下次还会踩）：

① **排头必须先重放销账、再解锁**。测试里让被搬运的排头直接 `UNLOCK_PI`，`lx_pi_top_waiter()`
会把「还在队列里的自己」选成接手者，字里于是留着 `tid | WAITERS`（读数 `0x80000007`）而不是清 0。
真 Linux 上这由内核在一次系统调用里原子完成（交接 + 唤醒），本内核用「登记 + 时间片重放」模拟，
所以用户侧的解锁必须发生在重放返回之后——这与 glibc 的顺序一致（`cond_wait` 返回后才继续用锁）。

② **只搬一个时，文字的 `WAITERS` 位取决于「还有没有别人」**。夹具里给两个等待者分别设
`base_priority` 4 / 5（tid 6 / 7）才能复现「tid 7 胜出」；不设优先级时默认值会让选择随机化，
判据就时红时绿——优先级继承的测试必须显式写 `base_priority`，不能靠默认值。

### 8.5 文件映射与写回（`ARKRTOS_USE_PAGING_FILEMAP`，2026-11-23，QEMU LA64 实跑）

`clone` 造出了第二条执行流、`MAP_SHARED` 让两条流看见同一块内存，但**「把文件当内存用」**还缺一块：
没有它，`ld.so` 装依赖库只能整文件读进匿名内存、改了文件内容没有回去的通路、`msync` 无从谈起。
打开 `ARKRTOS_USE_PAGING_FILEMAP`（`depends on ARKRTOS_USE_LINUX_ABI` + `PAGING` +
`PAGING_PAGES` + `PAGING_DEMAND`）后，222 号 `mmap` 在没有 `MAP_ANONYMOUS` 时走文件映射，
并新增 227 号 `msync`。

| 项 | 现状 |
|---|---|
| 认哪几种 | 没有 `MAP_ANONYMOUS` 就是文件映射；`MAP_FIXED` + 文件映射仍 `-ENOSYS`（同址丢弃再重读要写回语义），`MAP_PRIVATE` / `MAP_SHARED` 必须且只能给一个；`prot` 必须含 `PROT_READ`，`offset` 必须 4 KB 对齐 |
| 装内容 | `mmap` 当场把文件内容按 128 字节分块读进堆窗口（`arkrtos_vfs_read_at` + `arkrtos_copy_to_user`）；`offset` 生效，**EOF 之后清零**（不伪造 SIGBUS）。映射建立之后文件再变，**映射看不见**——这不是按需回读文件页，是一次性装快照 |
| 地址 | 与匿名 `mmap` 共用同一套堆游标；失败**不回收**游标（回收会让两段映射共用一页，那是静默错答案），这一段地址就地作废 |
| 写回认哪几种 | 只有「`MAP_SHARED` + `PROT_WRITE` + `O_RDWR` 打开」的映射记账，每进程 **2 条**（`LX_FILEMAP_MAX`）；私有映射的改动不落盘 |
| 写回时机 | `msync`（227）、`munmap`、任务退出（`on_task_exit` 开头、临界区之前——写回要调 VFS，不能在关中断里做） |
| 写回方式 | VFS 没有 `write_at` ⇒ 「读整个文件 → 覆盖映射段 → 整文件写回」。文件在映射期间**长度变了** ⇒ `-EIO`（绝不把别人写长的文件截断成读到的长度）；映射段整段落在 EOF 之后 ⇒ 空操作（文件不因此变长） |
| 缓冲 | 内核单份 8 KB 写回缓冲 + 临界区忙标志：第二笔并发写回如实 `-EBUSY`，不让两份数据在同一个缓冲里互相覆盖（那会写回一个查不出来的错文件） |
| 上限 | 文件 > 8 KB 时「可写共享映射」当场 `-ENOMEM`（早报，而不是以后只写得回一半）；私有映射不受这条限，但要的 `len` 仍受堆窗口限 |
| `msync` | 地址须 4 KB 对齐、`len` 非 0、flags 只认 `MS_ASYNC` / `MS_INVALIDATE` / `MS_SYNC`；命中记账就写回并计数；窗口内未命中回 0（私有/匿名本来没有可写回的东西，Linux 同）；窗口外 `-ENOMEM` |
| 普通文件 `write` | 仍 `-ENOSYS`：写内容只走映射写回这一条路，不假装写进去了 |
| `mprotect` | 不受影响：仍是只校验不生效（§1），PROT_EXEC 无法硬件强制（LA64 这条通路没有 NR/NX 位） |

读数（`tools/fork_gate.py` 的第五档 fork+COW+SHM+futex+filemap，**167 条 check**，其中 filemap 段 20 条）：

| 读数 | 钉住什么 |
|---|---|
| `filemap open read-only fd > 2 -> 1` | 文件真在 ramfs 里、真能按命名空间路径打开 |
| `filemap private map > 0` / `first byte is file first byte` / `byte 25 is Z` | 内容真的按文件装进去了，不是一块零页 |
| `filemap last byte of the page -> 'N'` | 5000 字节文件在 4 KB 页尾仍是文件内容（不是提前清零） |
| `filemap misaligned offset == -EINVAL -> -22` | `offset` 4 KB 对齐是硬校验 |
| `filemap bad fd == -EBADF -> -9` | 坏 fd 不落到匿名路径上去 |
| `filemap writable shared on read-only fd == -EACCES -> -13` | 可写共享映射要求 `O_RDWR`（Linux 语义） |
| `filemap offset=4096 map > 0` / `offset content starts at 4096 -> 'O'` | `offset` 真的生效，不是被忽略 |
| `filemap past EOF reads zero -> 0` | EOF 之后读零（不伪造 SIGBUS） |
| `filemap O_RDWR open on existing file > 0` | FILEMAP 档放开的那一个例外真能用 |
| `filemap writable shared map > 0` / `msync rc -> 0` / `munmap rc -> 0` | 可写共享映射建得起来、两个写回时机都回 0 |
| `filemap private writable map > 0` / `private msync rc (no writeback) -> 0` | 私有映射的 `msync` 回 0 且**不**落盘 |
| `filemap remap after writeback > 0` | 写回之后重新映射能读回同一批字节 |
| `filemap shared write persisted, private write not (bits=31) -> 31` | 五个证据位同时成立：共享映射的写进了文件、私有映射的写没进文件 |

**只有上板才看得见的坑：SVC 取参上限是 4。** 第一版在 QEMU 上 7 条 FAIL、全部文件映射都回 `-9`：
`ARKRTOS_ARCH_SVC_ARG_MAX = 4`，端口的 `arkrtos_port_svc_get_arg()` 在 `idx >= 4` 时直接返 0，
于是 `mmap` 的 `fd`（`a4`）与 `offset`（`a5`）**恒为 0** ⇒ `lx_fd_find(0)` 失败 ⇒ `-EBADF`。
宿主用例全过，因为宿主那份 `svc_get_arg` 桩允许取到 6 参——**这种缺陷只有上板才看得见**。
修法是 LA64 与 RV32 的端口在 `ARKRTOS_USE_LINUX_ABI == 1` 时把取参上界放宽到 6：trap 帧里
`$a0..$a7` 本来就在（`LA_FR_A0 + 0..7`），不改帧布局；原生 SVC 一侧仍只用 0..3，两侧互不影响。
RV32 一并放宽，是为了不留下同一个「读成 0」的坑。

### 8.6 `mprotect` 页级写保护（`ARKRTOS_USE_PAGING_MPROTECT`，2026-11-23，QEMU LA64 实跑）

到 §8.5 为止，226 号 `mprotect` 仍然只校验参数、返 0 并单独计数，**一页的权限都没改**：
`ld.so` 把 `.text`、`.data.rel.ro` 设成只读（RELRO）之后本该有的保护全都不存在，
一次野指针写能把重定位表改掉而毫无声响。打开 `ARKRTOS_USE_PAGING_MPROTECT`
（`depends on ARKRTOS_USE_LINUX_ABI` + `PAGING` + `PAGING_PAGES` + `PAGING_DEMAND`）后，
`mprotect` 改成真改 TLB 条目。

| 项 | 现状 |
|---|---|
| 强制哪一位 | 只有**写位**：`PROT_WRITE` 决定这一页的 `WE`。`PROT_EXEC` / `PROT_NONE` 只记账不强制——LA64 这条通路没有 NR/NX，做不出硬件不可执行；`PROT_NONE` 也没做成不可访问（读仍然可读）。写在边界里，不假装 |
| 粒度 | **4 KB**，不是 8 KB「对」：台账每位页一位（bit0 = 低 4 KB / bit1 = 高 4 KB），端口两遍走。`mprotect(addr, 4096, RO)` 只让低 4 KB 只读，同一对里的高 4 KB 仍可写 |
| 对台账的约束 | 一对的条目一次写两条（`tlbfill` 按对装），所以被保护的那一页必须**整对驻留**：对还没有实体时先按需缺页拿帧，再改条目 |
| 违规写怎么判 | trap 里**先判 mprotect 命中、再判 COW**：命中 ⇒ `arkrtos_cpu_fault_kill()` **杀任务不重启**（重启会回到同一条 store 上无限重启）；PLV0 违规 ⇒ 明确停机（不能表现成「用户程序被杀死」） |
| 状态字 | 被信号终止必须与 `exit(11)` 分开：TCB 多一个字节 `exit_by_signal`，`wait4` 状态字低 7 位是信号号（SIGSEGV = 11），正常退出才是 `code << 8`。否则一次越界写在父进程眼里就是「正常 exit(11)」，`WIFEXITED` / `WIFSIGNALED` 同时判错 |
| 从异常里切走 | kill 后必须显式置 `s_pg_commit`：fork 出的父子共享同一套 VA，目标帧地址与故障帧地址**可以完全相同**（实测都是 `0x021fbcb0`），`la_switch_prepare` 会把「地址相等」当成「没切」，于是窗口没换、恢复出来的还是故障那一帧 |
| 台账生命周期 | 按页随 fork 继承（`la_pg_space_clone` 逐页拷），随空间释放清零 |
| 撤销 | 再 `mprotect` 一次 `PROT_READ|PROT_WRITE` 即撤。COW 共享对本来只读，撤 mprotect 不会把它放开（放开就是绕过写时复制，两个进程从此各写各的帧） |
| 日志 | 违规写打一行 `[LA64U] mprotect violation badv=0x… era=0x… tid=… -> kill task`（带 `tid` 是为了运维知道摘的是哪个任务，门禁按这行做交叉证据） |

读数（第五档 + `-DARKRTOS_USE_PAGING_MPROTECT=1`，**178 条 check**，其中 mprotect 段 11 条）：

| 读数 | 钉住什么 |
|---|---|
| `mprotect mmap 8 KB > 0 -> 1` | 段里真拿到 8 KB 匿名映射（下面每一条都靠它） |
| `mprotect RO == 0 -> 0` | `mprotect(4096, PROT_READ)` 被接受 |
| `read of a read-only page still works -> 4369` | 只读页**读得回来**（没把只读做成不可访问） |
| `sibling 4 KB half of the same pair stays writable -> 13107` | 同一对里的另一半 4 KB **还能写** ⇒ 粒度是 4 KB 不是 8 KB |
| `mprotect RW == 0 -> 0` / `write after mprotect(RW) took effect -> 17476` | 保护**可撤**，不是单向的 |
| `mprotect RO again == 0 -> 0` | 可以第二次设只读（幂等） |
| `clone for mprotect child > 0 -> 1` | 违规写的子进程真 fork 出来了 |
| `mprotect child reaped (killed, not restarted) -> 1` | 违规写**真被拦住**，而且拦住后是杀任务、不是重启（重启会回到同一条指令） |
| `mprotect violation reports SIGSEGV status -> 11` | 状态字按 Linux「被信号终止」编码（低 7 位 = 信号号），不是 `exit(11)` 的高字节 |
| `parent page unchanged after the violation -> 17476` | 子进程那次违规写**一个字节都没落到父进程那一页** |

再加一条串口侧交叉证据：`[LA64U] mprotect violation badv=0x… era=0x… tid=… -> kill task`
——没有它，「子进程被 reap」也可能只是子进程自己退出了，这条说明它走的是
mprotect 判定路径而不是别的死法。

**只有上板才看得见的坑：父子帧地址相同，`s_pg_commit` 不会自己置上。**
第一版上板时串口出现**两行完全相同**的违规日志、第二条 `tid` 是父任务，随后整轮
演示提前结束。根因不在 mprotect 台账，而在「杀完任务从异常里切走」这一步：
fork 出的父子共享同一套 VA，返回的帧地址与故障帧地址一模一样，`la_switch_prepare`
判「地址相等 ⇒ 没切」，于是窗口仍是子进程的，从故障帧恢复出来的还是子进程那一帧，
同一条 store 再执行一次，把父进程也杀了。修法是在 kill 分支里 `LA_PRIV_APPLY()`
之后显式 `s_pg_commit = 1u;`。**「地址相等」在这里什么都不说明**，这条也适用于
以后任何「从 trap 里换任务」的路径。

**第二个坑：读数行的文案就是判据。** 夹具里那一行原本写成
`check("…SIGSEGV status -> 11", …)`，于是打印出来是 `… -> 11 -> 11 ok`，
而门禁要的是逐字 `… -> 11 ok`，于是六档门禁在最后一档判红——代码是对的、读数是对的，
**只是文案多了一层**。修法是把期望值从标签里拿掉（值由 `check` 自己打）。

### 8.7 真 `mmap` 语义：`MAP_FIXED`（按需分页档，2026-11-24，QEMU LA64 实跑）

到 §8.6 为止 222 号 `mmap` 只有一种形态：从堆窗口游标往上**新**拿一块地址。
`MAP_FIXED` 要的正好相反 ——「**就用这个地址**，上面的旧东西全部作废」。这两件事
差的不是一个返回值：前者保证不动别人的映射，后者是一句「这块地址归我，你把旧内容
丢掉」。`ld.so` 先预留再原地铺、`brk` 收缩后再长，以及任何「先 mmap 占位、再原地
铺」的写法，靠的都是后者。

**只认匿名私有。** `MAP_SHARED|MAP_FIXED` 与 `MAP_FIXED` + 文件映射一律 `-ENOSYS`：
共享对只有**整对**落在丢弃区间里才敢丢（见下），文件映射的「同址丢弃再重读」要写回
语义，本内核不做。**不支持就明确拒绝**，绝不「在别处分配一个再把同一个地址还回去」
—— 那样程序会以为自己手里就是那块内存，是最难查的假绿。

| 项 | 现状 |
|---|---|
| 语义 | 同址丢弃：把 `[want, want+len)` 覆盖到的每一页**就地清零**，条目按可写重装。成功后返回 `want` 本身 |
| 帧不还池 | 已驻留的私有对**原地清零**，不还回帧池。理由有两条：空间帧本来就「只借不还」，还帧会让下一次无关分配在别处失败；原地清零又保证地址稳定、`MAP_FIXED` 之后第一次写不再缺页 |
| 从未碰过的页 | `pa == 0` 直接跳过 —— 按需缺页下它本来就是零，没有帧要清、也没有条目要装 |
| 换出过的页 | 连 swap 槽一起作废：那几页的字节马上变得不可达，槽留着就是泄漏 |
| COW 共享对 | **先复制**：对面保留原字节，我们拿到新的一对再清零。这条保证 `MAP_FIXED` 不会把别人的页改掉 |
| `MAP_SHARED` 对 | 只有**整对**落在区间里才允许丢；部分覆盖直接回 `-1` —— 把这半对私有化、留另一半共享，是这套页表格式表达不出来的状态，宁可整段拒绝也不半做 |
| `mprotect` 台账 | 丢掉的那一页的只读位清掉（新映射是可写的），条目按可写入装 |
| 范围 | 必须 4 KB 对齐且整段落在堆窗口 `[lx_heap_base(), lx_heap_end())` 里；非对齐或 `want < base` 回 `-EINVAL`，`len > limit - want` 回 `-ENOMEM` |
| 失败不半做 | 共享对拆分检查在**动任何东西之前**做；帧池空回 `-2`（ABI 层翻译成 `-ENOMEM`），台账保持原样 |
| 游标 | 成功后 `s_heap_cur` 推到这一段之后（只增不减），下一次「地址随便」的 `mmap` 不会又发一次同一段 |
| 关档行为 | `ARKRTOS_USE_PAGING_DEMAND=0` 时 `mmap(MAP_FIXED)` 如实回 `-ENOSYS`（没有按对装条目的能力，也就谈不上「丢掉」），不降级也不假装 |

读数（`tools/fork_gate.py` 六档都带 `-DARKRTOS_USE_PAGING_DEMAND=1`，所以这一段在每档都跑，共 6 条）：

| 读数 | 钉住什么 |
|---|---|
| `mmap for MAP_FIXED > 0 -> 1` | 段里先拿到一块可被丢弃的映射（下面每条都靠它） |
| `MAP_FIXED returned the same address -> 1` | 返回的是**请求的那个地址**，不是别处 |
| `MAP_FIXED discarded the old bytes -> 1` | 先往首尾各写一个标记字节，`MAP_FIXED` 之后两个都读回 0 —— 判据落在**内容**上，不是返回值 |
| `MAP_FIXED page writable afterwards -> 1` | 丢弃之后那一页写得进、读得回 |
| `MAP_FIXED misaligned -> -EINVAL -> -22` | `want + 1` 被拒 |
| `MAP_FIXED shared -> -ENOSYS -> -38` | `MAP_SHARED|MAP_ANONYMOUS|MAP_FIXED` 被如实拒绝，没被悄悄接受 |

第三行是全段最要紧的一条：只判「返回同一个地址」的实现可以把旧页原地留着，
程序读回来的是自己的旧数据 —— 那是最难查的假绿。所以判据必须落在字节上。

**夹具侧的坑：开关必须逐个镜像。** `example/qemu_loongarch64/Makefile` 里
`$(LXFORK_ELF)` 只用 `$(ARCH_FLAGS) $(LXFORK_DEFS)` 编译，**不带 CFLAGS /
EXTRA_CFLAGS**，所以夹具看不到内核的编译开关。这一段只在 `ARKRTOS_USE_PAGING_DEMAND=1`
时编，而 DEMAND 原先没进 `LXFORK_DEFS` —— 表现是六档门禁齐声报「check 条数差 6」，
代码本身完全正确。修法是在 Makefile 里补一条 `ARK_DEMAND_ON` 镜像，并在那里写清
「每加一个夹具要看的开关，就得显式镜像一条；漏镜像的表现就是那几行读数整段不出现」。

**另一个真缺陷：`la_pg_install_pair` 原本躺在 COW 段里。** 它定义在
`#if (ARKRTOS_USE_PAGING_COW == 1)` 之内，而 `arkrtos_port_paging_discard()` 无条件
调它 —— 于是「按需分页开、COW 关」这一档链接直接
`undefined reference to 'la_pg_install_pair'`。这个组合正是 `[fork]` 第一档，
换句话说：**只开 DEMAND 从未被链接过**。修法是把函数（连同注释）搬到 COW 段之外、
`la_pg_pair_pte()` 之后；它依赖的 `la_tlb_write` / `la_tlb_inval_page` /
`LA_STLB_INDEX` / `LA_PG_WIN_WAY_OF` / `la_asid_swap` 全是无条件定义，搬出来不引入
任何新的条件依赖。

### 8.8 `pselect6`：跨 fd 的就绪等待（`ARKRTOS_USE_LINUX_SELECT`，2026-11-24，QEMU LA64 实跑）

到 §8.7 为止，兼容层的「等」有两条：`wait4` 等子进程退出（§8.1）、`futex` 等一个字被改
（§8.4）。缺的第三条是 shell、`initscripts`、任何 select 循环都靠的那一条 ——
**「这一堆 fd 里有一个能读了/能写了就叫我」**。它在 asm-generic 上是 72 号 `pselect6`
（`select` 在 la64 上只剩这一条路，libc 的 `select` 也翻译成它）。

| 项 | 现状 |
|---|---|
| 号与参数 | 72 号 `pselect6`：0=`nfds`、1=`readfds`、2=`writefds`、3=`exceptfds`、4=`timespec*`、5=临时信号掩码指针 |
| 超时语义 | 与 Linux 一致：`timeout == NULL` 是无限等待（本内核按时间片重放）、`timeout=0` 是纯轮询立刻回 0、否则是**相对时长**且**不写回**用户结构（这是 `pselect` 与 `select` 的区别） |
| 返回值 | 就绪描述符个数；就绪位写回用户表、非就绪位清零。三张表都不给（全 NULL）且 `nfds>0` 时按 0 个对象处理，只做超时等待 |
| 读就绪 | fd 0 恒可读（读到 EOF）；普通文件恒就绪（同步读）；管道读端 `count > 0 || writers == 0`（**写端全关就是 EOF，必须报可读**）；管道写端 / 未打开 fd 不可读 |
| 写就绪 | fd 1 / 2 恒可写（控制台）；普通文件恒就绪；管道写端 `readers == 0 || count < 256`（**满就没空间**，判的是容量不是「能调 write」）；管道读端 / 未打开 fd 不可写 |
| `exceptfds` | 接受但**永不上报**：本层没有带外数据、没有异步错误队列。写回时同样被清位 —— 不是「原样留着」 |
| fd 有效性 | 被选中的 fd 必须**有对象支撑**（fd ≤ 2，或在本进程 fd 表里）；否则 `-EBADF`。静默忽略是最难查的假绿：调用方会一直等到超时 |
| 参数校验 | `nfds > 1024`（`FD_SETSIZE`）回 `-EINVAL`；第 6 个参数（每次调用临时信号掩码）**非空回 `-ENOSYS`**，不装作忽略 |
| fd_set 读写 | 按 **32 位字**逐个 `copy_from_user` / `copy_to_user`，不把 1024 位（128 字节）整张搬上内核栈；`nfds` 决定要读几个字 |
| 阻塞形式 | 与 futex 同型：`arkrtos_port_svc_restart` + `arkrtos_task_block_in_critical(100 ms)`，截止时刻存 per-task（**存截止时刻**，不是剩余时长 —— 否则每次重放都会重新计时）；没有可重放端口时如实回 `-EAGAIN` |
| 任务号复用 | 任务退出与 `reset` 都清掉 `s_sel_armed/s_sel_deadline`：任务号会被复用，不清就是「新任务继承一条已经在等的旧记录」 |

读数（`tools/fork_gate.py` 第七档，夹具 178 → **194 条**，其中 select 15 条）：

| 读数 | 钉住什么 |
|---|---|
| `pselect6 pipe2 == 0 -> 0` | 段里先把管道建起来（下面每条都靠它） |
| `pselect6 empty pipe read end not ready -> 0` | 空管道读端**不**就绪（`timeout=0` 即刻回 0，不是「有管道就报读」） |
| `pselect6 clears the user set when nothing is ready -> 0` | 没有就绪位时用户 `fd_set` 被清位，不是原样退回来 |
| `pselect6 timed wait returns 0 at the deadline -> 0` | 50 ms 的相对超时到点回 0 |
| `pselect6 timed wait really waited >= 40 ms -> 1` | 这次等待**真的等够了**（墙钟读数；「每次重放重新计时」的实现在这里必红） |
| `pselect6 pipe wrote 3 bytes -> 3` | 夹具侧写入完整（不是半截） |
| `pselect6 read end ready after 3 bytes -> 1` | 有数据之后读端就绪 |
| `pselect6 keeps the ready bit in the user set -> 1` | 就绪位写回用户集（把「用户要求的那几位」当输出漏掉，会在这里红） |
| `pselect6 write end ready while the pipe has space -> 1` | 有空间的写端就绪 |
| `pselect6 stdout is always writable -> 1` | fd 1 恒可写 |
| `pselect6 write end not ready when the pipe is full -> 0` | 管道写满（256 字节）之后写端**不**就绪 —— 判的是容量，不是「能调 write」 |
| `pselect6 read end ready on EOF after the writer closed -> 1` | 写端全关之后读端按 EOF 就绪（Linux 在 EOF 上必须报可读，否则 `read` 循环永远等不到） |
| `pselect6 never reports exceptfds -> 0` | 只给了 `exceptfds` 且 fd 可读时回 0 |
| `pselect6 clears exceptfds -> 0` | 同上那张表被清位 |
| `pselect6 bad fd == -EBADF -> -9` | 没有对象支撑的 fd 被选中如实拒绝 |

**只有上板才看得见的坑：夹具的位宽。** 第一版把「stdout 恒可写」写成了
`ws[1] = 2u`，本意是「fd 1 的位」——但 `fd_set` 是按 32 位字排的数组，
`ws[1]` 第 1 位是 **fd 33**，于是 `nfds=2` 的调用里根本没有 fd 1，读数如实回 0。
代码是对的、判断是错的，这条只有真跑才暴露（宿主用例用 `u_put32` 写的是同一个字，
没踩到）。修法是先清零整张表，再把 `ws[0] = 2u` 当 fd 1 的位。

**为什么单独开一个开关。** 就绪判据依赖 fd 表与管道对象的具体形状（容量 256、
`writers/readers` 计数），换 socket 之后这套判据要重写。把 select 与未来的 socket
放在同一个开关下，会让「关档零体积」这条承诺变成「关档少了一半功能」。
`ARKRTOS_USE_LINUX_SELECT` 只依赖 `ARKRTOS_USE_LINUX_ABI`。

### 8.9 socket（U-23）：翻译层与 QEMU 第八档（2026-11-24，宿主 964 checks + QEMU 242 条）

13 个号落地：`socket`(198)、`bind`(200)、`listen`(201)、`accept`(202)、`connect`(203)、
`getsockname`(204)、`getpeername`(205)、`sendto`(206)、`recvfrom`(207)、`setsockopt`(208)、
`getsockopt`(209)、`shutdown`(210)、`accept4`(242)。`read`/`write`/`close` 三个老号按 fd
的种类分派到 socket 实现（同一张 fd 表，`kind` 字段区分文件/管道/socket）。

| 语义 | 实现 |
|---|---|
| 句柄 | fd 槽里的 `kind` 标 socket，`net` 字段存内核 net 句柄；socket 与文件/管道共用同一张表 |
| `bind` / `connect` | 地址从用户 `sockaddr_in` 拷进来（family 必须 `AF_INET`，否则 `-EAFNOSUPPORT`；`alen < 8` → `-EINVAL`；拷不动 → `-EFAULT`） |
| `accept` / `accept4` | 两步式：`step=1` 先拿结果与 aux 句柄，`step=2` 再往用户地址写；地址写失败时**释放刚拿到的 fd** 再返回错误。`accept4` 认 `SOCK_NONBLOCK` / `SOCK_CLOEXEC` |
| `getsockname` / `getpeername` | 名字查询返回后按需写回 16 字节 `sockaddr_in` 与 `addrlen`；`addrlen==0`（`r[1]==0`）时**提前返回、不碰用户 `addrlen`** |
| `sendto` / `recvfrom` | `r[4]!=0` 才走带地址的形态，否则退化成 `SEND`/`RECV`；`recvfrom` 长度 `>0xFFFF` → `-EMSGSIZE`，`-ECONNRESET` 直回 |
| `setsockopt` / `getsockopt` | `SOL_SOCKET` 下翻译 `SO_REUSEADDR`(2)、`SO_SNDBUF`(7)、`SO_RCVBUF`(8)、`SO_ERROR`(4)；`SO_TYPE`(3)、`SO_DOMAIN`(39)、`SO_ERROR` 由本层本地作答；`SO_ERROR` 写回 → `-EINVAL`；非 `SOL_SOCKET` 或未知 opt → `-ENOPROTOOPT`；值/长度拷不动 → `-EFAULT`/`-EINVAL` |
| 阻塞与非阻塞 | 内核回 pending 时按 fd 的 `O_NONBLOCK` 与 `MSG_DONTWAIT` 决定是「直接回 `-EAGAIN`」还是「重放」。重放走 `arkrtos_port_svc_restart()`：不可重放时回 `-EAGAIN` 且**不睡** |
| 身份 | `lx_sock_me_ok` 先查任务身份再查 fd：`task_id==0` 的调用一律 `-ENOSYS`，且**不发任何 net 请求** |
| 关闭 | `close` 走 socket 分支释放 net 句柄与 fd 槽；对管道读端 `write` 如实 `-EBADF` |

**为什么先查身份再查 fd。** 这是宿主档第五百多条断言逼出来的顺序问题：如果先判 fd，
空任务身份下会给出 `-EBADF`，把「这个上下文根本没有 socket 能力」误报成「fd 用错」。
反过来也守住一条：非法身份下 13 个入口的 net 请求计数必须**一个都不涨**。

**只开 lwIP、不开 virtio 网卡的构建档。** `example/qemu_loongarch64/Makefile` 新增
`ARK_LWIP_ON := $(findstring ARKRTOS_USE_LWIP=1,$(EXTRA_CFLAGS))`：`ARKRTOS_USE_LWIP=1`
而 `ARKRTOS_USE_NET_VIRTIO` 未开时只编 `$(LWIP_SRCS)`，不编 `drvnet.c`/`net_demo.c`，
再把 `ARKRTOS_USE_LINUX_SOCKET=1` 与 lwIP 一起塞进 `LXFORK_DEFS`。这条档是为了让
「有 lwIP 协议栈、没有网卡」的宿主/仿真构建也能出 ELF。

**宿主读数。** 第六个 Linux ABI 覆盖率档 `linuxabisocket`（`tools/coverage_gate.py`）
在宿主上跑 `test_linux_abi_host.c` 的 socket 段：单档 **98.51%**，六档 union **98.84%**
（基线 98.82%；2026-11-27 随 `rseq`/`getcpu` 的宿主断言一起重钉），整档 **964 checks** 全绿
（U-30 之前是 949；socket 段本身 322 条，其中 U-30 的 dup-on-socket 新增 15 条）。

**QEMU 第八档：242 条 check 全 `ok` + `exit code 0`。** `tools/fork_gate.py` 的第八档
（`SELECT+SOCKET+LWIP`）在 LA64 上真跑 `example/qemu_loongarch64/elf/linux_fork.c` 的
socket 段（38 条）加一条性能回归门（1 条），叠在第七档 194 条之上：

读取对象是**真 lwIP 回环**（`127.0.0.1`），不是桩：`socket` 建 stream/dgram 两个句柄；
坏 domain / 坏 type / 不支持的 type 各自的 errno；`bind` + `listen` + `getsockname` 端口按
网络序读回 8081；`connect` 回环、`accept` 拿到新 fd 且写回的对端 family 是 `AF_INET`；
`send` 5 字节 / `recv` 5 字节载荷逐字节比对（`hiyo` + `u`）；`getpeername` 的 IP 是
`127.0.0.1`；`setsockopt` 认 REUSEADDR，`getsockopt` 的 `SO_TYPE` / `SO_ERROR` 由本层本地
作答、未知 opt 回 `-ENOPROTOOPT`、写 `SO_ERROR` 回 `-EINVAL`；`shutdown(SHUT_WR)` 之后
对端 `recv` 回 0（真 EOF）；关掉之后 `read`/`write` 回 `-EBADF`；UDP 走 `sendto`/`recvfrom`，
`recvfrom` 的对端端口必须等于客户端的 `getsockname` 端口。

**三个只有真跑才暴露的坑（都已修）。**

1. **`sendto`/`recvfrom`/`setsockopt` 必须用 6 参数入口。** 第一版用 4 参数的 `lx4`，
   0 号参数没传，`send` 恒回 `-22`；改用 `lx6` 后正常。
2. **`getsockopt` 的 4 字节 `int` 不能用 `lx_long` 接。** 内核写 4 字节，`lx_long` 的高
   32 位保留旧值，读回来是 `-4294967295` 这种「看起来很深」的假数字；改成
   `unsigned int pack32` 并把地址传进去才是对的。
3. **netconn 池只有 4 个。** `MEMP_NUM_NETCONN = 4`、`ARKRTOS_NET_MAX_SOCKETS = 4`、
   `LX_MAX_FD = 8`（fd 3..10）三者同量级，夹具同时开三只 TCP + 一只 UDP 就会把池子占满。
   修法是**夹具用完就关**（真实程序也是这么写的），而不是把池子调大 —— 调大要改默认档
   BSS，破坏「默认档零变化」。同理，shm 段留下的管道 fd 也在这里显式关掉。

**lwIP 服务任务栈 384 → 1024 字，lwIP 任务栈 1 KB → 4 KB。** 第一版 `tcp recv 5` 永久挂住：
net 服务任务用 384 字（1.5 KB）栈跑 `netconn` 一轮就把它打穿，STK 守卫报
`stack bottom flag overwritten`，被踩的是相邻 `.bss`，症状看起来像协议栈内部断言。
`ARKRTOS_NET_TASK_STACK_WORDS` 三处同步改成 1024（`Kconfig.net` 默认值、
`arkrtos_autoconf.h`、`arkrtos_net.h` 兜底），`lwip_port.c` 里非 virtio + socket 档的
lwIP 任务栈同步提到 1024 字（与 virtio 档同值，理由写在注释里：这一轮还要装 lo 网卡）。

**「一次 socket 往返 = 一个 100 ms 时间片」这条性能缺陷（已修）。** 第八档第一版跑得过，
但整段 socket 读数要 3.0 s（`[fork] finished after 3000 ms` 临界）。分段计时把账量出来：
`t_setup 507 ms / t_conn 403 / t_acc 202 / t_recv 202`，每一步都恰好是一个
`LX_WAIT_SLICE_MS = 100 ms` 的整数倍 —— 请求已经完成，等的人还在睡满一整个时间片。
根因是请求/回复槽只有一个「状态字」而没有任何就绪通知：`arkrtos_net_submit` 回
`EPENDING`，翻译层就盲睡一个时间片再重放。

修法照 Linux / RT-Thread 的做法，把「等」变成事件驱动（`kernelsrc/src/arkrtos_net.c`）：

- 服务任务发布 `REQ_DONE` 之后，若这条请求是 Linux ABI 层发的（`g_net_req_wake`），
  直接 `arkrtos_task_wakeup_internal(owner)` 叫醒等待者。裸 Ark SVC 调用者没有这个
  约定（它可能在等别的东西），所以唤醒权跟着请求一起登记，而不是「谁在等就叫醒谁」。
- 那条唤醒有竞态：服务任务可能在上面的 `arkrtos_net_call()` 返回**之前**就把回复写好了，
  那时标记还没置 1、唤醒发不出去。所以等待方在睡下之前先问一句
  `arkrtos_net_reply_ready(me)`，两种顺序都不会白等一个时间片。
- 时间片保留为丢唤醒时的兜底 —— 一次没叫醒不会永久卡住。
- `connect` 那种「协议栈还在推进」的两段式等待改按 `LX_SOCK_POLL_MS = 10 ms` 重放：
  那类状态变化通常几毫秒内就发生，用 100 ms 去等就是 121 ms 一次本地 connect。

实测：`t_tcp 2023 → 168 ms`，整段 3.0 s → **0.58 s**（`[fork] finished after 580 ms,
exit code 0`）。这条也进了判据：
`socket round trips do not cost a wait slice (< 800 ms) -> 1 ok` —— 唤醒要是丢了，
十几次往返会立刻回到 2 s 级，判据必红。

**夹具的两个构造约束（与内核无关，但改了夹具才稳）。**

- **子进程等待不用用户态读时钟。** 多时间片的 futex WAIT 原来靠「反复 `clock_gettime`
  忙等 250 ms」，QEMU 实测这条路径偶发让某一次 `clock_gettime` 回一个来路不明的正值
  （内核侧既没有 `rc=3` 记录、也没有 Linux 分发落空记录），忙等被提前打断，跨时间片那条
  判据假红。现在子进程改成连睡三个内核记账的 100 ms 定时 `futex(WAIT)`，一个字都不读
  时钟；父进程的墙钟只读两次，且带 8 次重试。异常本身登记在
  [已知限制](已知限制.md)，属待定位项。
- **两条 `child reaped -> N ok` 的 `N` 是子进程 pid**，随档位变化（6 / 12 / 15 都出现过），
  所以判据只判形态（`^\[fork\] futex child reaped -> \d+ ok$`），不写死具体 pid。

### 8.10 线程（`CLONE_THREAD`，U-25）：一组任务共用一份地址空间（2026-11-24，QEMU LA64 第九档 275 条）

`clone(220)` 的线程分支落地，开关 `ARKRTOS_USE_LINUX_THREAD`（默认关，
`depends on ARKRTOS_USE_LINUX_FORK`）。与 fork 分支的分流只看 flags：同时带
`CLONE_VM` 与 `CLONE_THREAD` 走线程，`flags==0`/`SIGCHLD` 且 `child_stack==0`
走 fork。除此之外一律 `-EINVAL`（含 `CLONE_VFORK`、只给 `CLONE_VM` 不给
`CLONE_THREAD`、`child_stack==0`、栈不是 16 字节对齐）。

| 语义 | 实现 |
|---|---|
| 地址空间 | **不复制**：子任务直接绑父的那个 `space`，同一份 `s_pg_pair_pa` 账本、同一批物理帧。父线程的 .bss 与子线程的 .bss 是同一个字 |
| 帧 | 子线程的初始 trap 帧落在 `child_stack - LA_FR_SIZE`（1040 B），帧里 `SP = child_stack`、`A0 = 0`，其余寄存器（含 `ERA`）**原样继承父线程那次系统调用的帧** —— 于是子线程第一次被恢复时回到 `syscall` 的**下一条指令**、返回值 0，这正是旧式 `clone(2)` 的约定（glibc 的 `pthread_create` 也是靠这条约定分流的） |
| 帧所在页 | 与 fork 相反：线程栈是**新页**，内核路径不做按需缺页，所以这里自己去帧池要那几对、**无条件清零**（池里的对可能装着别人刚还回来的内容；换出/换入一旦发生"刚起来是干净的"就不成立），失败就把这次拿到的还回去 |
| 进程状态槽 | **共用**父的槽，靠引用计数。绑定发生在装载器的临界区里（`lx_thread_bind_hook`），而不是等子线程第一次进 ABI —— 晚一步子线程会给自己惰性分配新槽，症状是"线程里关不掉主线程的 fd"，且不报任何错 |
| fd / 信号 | 同一张 fd 表、同一份信号动作与屏蔽字（都由共用槽承载） |
| `CLONE_SETTLS` | `a2` 写进子线程帧的 `$tp`；`tls==0` 按"没给"处理（`$tp` 沿父线程）——**"把 `$tp` 设成 0"这个意图表达不出来**，登记在 [已知限制](已知限制.md) |
| `PARENT_SETTID` / `CHILD_SETTID` | 都写 4 字节 `int`（内核侧先探一次可写再创建：创建之后才发现坏指针，只能留下一个跑着的线程和一个说不清的返回码） |
| `CHILD_CLEARTID` | 退出时把那个字清零并 `FUTEX_WAKE` 一次 —— `pthread_join` 的等待就落在这条路上 |
| `getpid` / `gettid` | `getpid(172)` 回**线程组号**（组长任务号），`gettid(178)` 回本线程号。关档时 `gettid` 仍回 `-ENOSYS` |

**退出语义（这一节只有真跑才看得出来）。** `exit(93)` 只结束调用它的那个线程；
`exit_group(94)` 结束同组全部线程。收尾路径按"组里还有没有别人活着"分岔：

- **还有兄弟活着**：只清自己的 `CLEARTID`、唤醒等待者，**绝不**走 `sock_close_all`、
  fd 释放与槽回收 —— 否则先退的那个线程会把整组的 fd 表与 socket 句柄一起带走。
- **最后一个走（含组长）**：才做完整的资源回收（关 socket、释放每个 fd、清槽、
  解绑），退出码记在组长那份 TCB 上，`wait4` 读的就是它。

线程**不登记** `s_child[]`：Linux 的 `wait4` 看不见线程（它们不是子进程），
所以整组只剩组长一个"对外的进程"。

**QEMU 第九档：275 条 check 全 `ok` + `exit code 0`（600 ms）。** `tools/fork_gate.py`
的第九档 = 第八档（242 条）+ 线程段 33 条。读数里最要紧的六条：

- `clone(CLONE_THREAD) > 0 -> 1 ok`、`thread PARENT_SETTID wrote the tid -> 6 ok`；
- `futex WAIT on CHILD_CLEARTID woke with 0 -> 0 ok` —— 用**真实的 join 路径**
  （`CHILD_CLEARTID` + `FUTEX_WAIT`）等线程退出，一次验完 CLEARTID、WAKE、WAIT 三件事；
- `CLONE_SETTLS landed in $tp -> 1515847680 ok`（`0x5a5a0000`）；
- `thread getpid == tgid (parent pid) -> 14 ok` —— 门禁另有一条**交叉证据**：这个 14
  必须等于夹具打出来的 `[fork] parent pid=`，否则"tgid 恒等于某个常量"也能过；
- `exit_group from a thread reaped the group -> 6 ok` / `exit_group-from-leader code 7 -> 7 ok`
  —— 两条都在**子进程**里做，不杀测试进程本身；中间夹一条 `fork after threads reaped -> 6 ok`
  证明线程退出没有把进程的 fd 表 / 进程槽提前拆掉（否则后面 fork 会立刻失败）。

反假绿：四条参数门槛（缺 `CLONE_VM`、`child_stack=0`、栈未对齐、带 `CLONE_VFORK`
各 `-EINVAL`）在开档时逐条必须拒绝；`judge()` 对七条线程读数逐条要求"恰好出现一次"，
`self_check()` 有七种坏样本（CLEARTID 没清、WAIT 没唤醒、SETTLS 没落、`getpid` 不等
tgid、两栈同址、`exit_group` 码错、缺 clone 读数）必须被判红。

**关档零变化。** `arkrtos_elf_thread` / `arkrtos_port_paging_thread_frame` 两个函数
与 `lx_thread_group_exit` / `lx_clone_thread` / `lx_thread_bind_hook` / `lx_thread_clear_tid`
四个符号在关档时**一个都不产出**，门禁 `off-zero-symbols` 用 `nm` 逐条核对。

**四个只有真跑才暴露的坑（都已修）。**

1. **`gettid` 的号抄成了 186。** 186 是 x86_64 的 `gettid`；LA64 走 asm-generic 表，
   是 **178**。号写错的后果不是报错，而是"真实 glibc 程序调 `gettid` 拿到 `-ENOSYS`"——
   夹具自己用的是同一个常量，所以自测全绿也发现不了。这一条是复核系统调用表时抓出来的。
2. **子线程不是"跳到线程体"，而是从 `syscall` 的下一条指令继续跑。** 第一版夹具直接
   `thr = clone(...);` 之后就当作父线程继续，结果子线程把父线程后面的检查**整段又跑了
   一遍**（日志里同一句出现两次、第二次读数是 0），并且线程体一次都没执行。真实写法是
   `if (thr == 0) { 线程体(); }`，而且这个分支必须挤在 `syscall` 与分支之间**不许编译器
   插入任何读栈的指令**（子线程此时已经换到自己的栈上，父栈的内容它读不到）：夹具按
   musl `__clone` 的写法把 `syscall` / `bnez` / `jirl` 放进同一个 asm 块。
3. **`PARENT_SETTID` 是 4 字节 `int`。** 夹具拿 `lx_long` 接、初值 `-1`，内核只写低
   4 字节，读回来是 `-4294967290`（低 32 位是 tid、高 32 位还是 `0xFFFFFFFF`）。这和
   §8.9 的 `getsockopt` 坑是同一类，修法也一样：用 `unsigned int`。
4. **`FUTEX_WAIT` 会被同进程子进程退出的 `SIGCHLD` 打断（`-EINTR`）。** requeue 用例
   第一版只把 `-EAGAIN` 当"重试"，第九档里偶发一次合法打断就被读成假红（`rc10 = -4`）。
   真实调用方对 `-EINTR` 也是重试的，夹具改成 `-EAGAIN` / `-EINTR` / `-ETIMEDOUT` 三种
   都重试，并给它 100 ms 一轮、最多 60 轮的上限（既不会挂死，也不会把"没等到"读成"等到"）。

**写死的边界（不假装修好了）。**

- **SMP 未验证。** `exit_group` 强杀的是"可能正在另一核上跑"的任务，本内核没有核间
  停核协议；线程档只在**单核** QEMU 配置下跑过。
- **`execve` 在非组长线程上的行为未定义。** `execve` 换的是**当前任务**的映像，同组
  其它线程仍指着旧空间；Linux 的语义是"`execve` 结束整组、只留调用者"，本内核没有这么做，
  也**没有**在非组长线程上拒绝（登记在 [已知限制](已知限制.md)）。
- **没有线程本地存储模块分配。** `SETTLS` 只写 `$tp`，TLS 布局是 libc 的事；
  `SET_TID_ADDRESS` 只回报自己的 tid、不保存指针。robust list（99/100）与退出清理已经落地，
  见 §8.4b；它按任务记账，线程组里的任意一条退出都只清自己那条链。
- **没有优先级 / 调度策略继承的额外语义**：线程就是一个普通任务，优先级、时间片与
  其它任务同档。

### 8.11 每 App 任务配额（`ARKRTOS_USE_APP_QUOTA`，2026-11-24，QEMU LA64 第十档 287 条）

第十档 = 第九档（275 条）+ 配额段。它回答的是「一个 App 能不能把任务表吃光」，
不是一个新系统调用：装载器给每份 ELF 发一个非零 owner 号（`elf_app_owner_new()`），
fork 与 `clone(CLONE_THREAD)` 都**继承**同一个 owner，同一 owner 的**活任务**数到
`ARKRTOS_APP_MAX_TASKS` 时创建失败——`ARKRTOS_ELF_ERR_QUOTA` 在 `lx_fork_errno()`
里映射成 `-EAGAIN`，并各打一行
`[appq] refused <fork|thread>: owner N quota M tasks reached`。

**为什么上限取 3。** 本板 `ARKRTOS_PAGING_APPS_MAX = 2`（并发进程最多父 + 1 子），
而 `ERR_QUOTA` 与 `ERR_BUSY` 都映射 `-EAGAIN` ⇒ 返回值上分不出是谁拦的。只有
「线程不占新地址空间」这条能把两者分开，所以夹具用「1 进程 + 2 线程」把 3 个名额
压满，再读两条**无歧义**的拒绝：线程超限（不可能 BUSY）、以及**空间空闲时** fork
超限。3 同时高于第三批线程段的峰值 2，两段互不干扰；配额计数逻辑与常量取值无关，
默认档的 8 走同一条代码。

**配额段的读数（`[fork] quota …`）**：① 上限内 fork 成；② 线程把最后一格占掉；
③ 超限 fork 回 `-11`；④ 超限线程回 `-11`；⑤ 被阻塞的子进程按 `wait4` 真收走
（配额数的是活任务，前提是它真的活着）；⑥ 上限内第二个线程成；⑦ **空间空闲时**
fork 仍回 `-EAGAIN`（全段最干净的一条）；⑧ 第三个线程回 `-EAGAIN`；⑨ 两个线程按
真实的 join 路径（`CHILD_CLEARTID` 清零 + `FUTEX_WAIT`）归队；⑩ 额度回收后立刻能
再 fork；⑪ 那个子进程退出码照传 3。另加三行 `[appq]` **交叉证据**：必须恰好 3 行
（2 次 fork + 1 次 thread）、owner 是同一个、上限读数是 3——没有这一行，调用者只
看到 `EAGAIN`，分不出「空间用完」与「额度用完」。

**两个只有真跑才暴露的坑（都已修）。**

1. **管道读不阻塞 ⇒ 子进程半路自杀（U-26 之前的读数坑，现已根治）。** 当时
   `lx_pipe_read()` 在「空且还有写端」时回 `-EAGAIN`。夹具第一版写
   `while (read(...) > 0) {}`，负值直接跳出 ⇒ 子进程立刻 `exit`、配额压根没被压满，
   症状是「线程被放行」与「没东西可回收」两条同时红，`[DBG] cnt` 打出来的活列表里
   已经找不到那个 tid。当时的修法是子进程每 100 ms 用一次 `futex(FUTEX_WAIT)` 超时
   醒来重试，只在读到 EOF(0) 时才退出。**U-26 之后管道默认阻塞（§6），这一次 `read`
   会直接挂起**，那段 futex 轮询退化成防御路径（只有 `O_NONBLOCK` 档才可能回
   `-EAGAIN`），没有删掉——它保证夹具在任何一档下都不会空转。
   **休眠字必须是 App 窗口内的全局变量**：`lx_futex_key_task()` 对 `uaddr < LA_WIN_VA`
   直接回无效，栈上的字等不到唤醒。
2. **线程栈在 COW 下的分类需要提示窗口。** 线程帧落在 `child_stack - 1040`，而
   `clone` 不给 `stack_size`，`la_pg_stack_pair()` 原先只认 `[hi-size, hi)` ⇒ 把线程
   栈判成「不是栈」，fork 时给它装了只读页。第一次 `st.d` 就 PME（`ecode=4`），而
   eentry 第一件事是 `addi.d $sp,$sp,-528` ⇒ 异常栈踩同一页，无限嵌套，C 处理器
   没机会跑。修法是：线程且 `stack_size == 0` 时按栈顶下方 32 KB
   （`LA_PG_THREAD_STACK_HINT`）判类。**这是分类提示，不是栈底保证**——栈守卫、
   越界检查、栈溢出判定一律不得依赖它。

**写死的边界。** 配额只管**任务表**：fd 表本来每进程只有 `LX_MAX_FD` 个槽、页帧
本来被 App 自己的 2 MB 地址窗口卡住，一个 App 都耗不尽；**内存池的按 App 配额仍未
做**（要给分配器加 per-owner 账本）。并发进程数受 `ARKRTOS_PAGING_APPS_MAX` 限制；
`ERR_QUOTA` 与 `ERR_BUSY` 同为 `-EAGAIN`，调用者只能靠 `[appq]` 日志区分。见
[已知限制](已知限制.md) §四与 [安全认证准备](安全认证准备.md) §7 SEC-G07。

**关档零变化。** `elf_app_owner_new` / `elf_app_task_count` / `lx_appq_log` 三个符号
与 TCB 里的 `app_owner` 字段在 `ARKRTOS_USE_APP_QUOTA=0` 时一个都不产出，门禁
`off-zero-symbols` 用 `nm` 逐条核对；`features.h` 里有
`ARKRTOS_USE_APP_QUOTA=1 requires ARKRTOS_USE_ELF=1` 的 `#error` 顶住。

### 8.12 `dup`(23) / `dup3`(24)：fd 槽复制、标准流别名与控制台引用（2026-11-27，QEMU LA64 56 条）

原来这一对只回 `-ENOSYS` 的欠账在本轮清掉。它们是**默认档即生效**的：没有新开关，
没有体积档位，`ARKRTOS_USE_LINUX_ABI=1` 就有——因为「把一个 fd 复制成两个」不需要
任何新的内核机制，只需要把 fd 表里那一格再抄一份。

**语义（逐条与 Linux 对齐，偏差写在明面上）**

| 形态 | 回答 |
|---|---|
| `dup(oldfd)` 成功 | 返**最小空号**（>= 3）；源与副本是两个独立描述符，关掉任一个另一个仍然有效 |
| `dup3(oldfd, newfd)`，`newfd >= 3` 且空闲 | 返 `newfd` |
| `dup3(oldfd, newfd)`，`newfd >= 3` 且已占用 | 先释放旧目标（管道端减一次引用计数），再装副本；返 `newfd` |
| `dup3(oldfd, 1)` / `(?, 2)` / `(?, 0)` | **占一格真槽**并记成该标准流的别名（`lx_proc_t.std_slot[]`）。标准流原来就是「没有别名的 0/1/2」，所以 `int saved = dup(1); dup2(fd, 1); ...; dup2(saved, 1);` 这套守护进程写法在语义上成立 |
| `dup(0)` / `dup(1)` / `dup(2)`（源没有别名） | 复制的是**控制台对象**（`LX_FD_CONSOLE`）：写就走控制台，读回 EOF，`select` 判可写 |
| 源不存在 / `oldfd` 越界 | `-EBADF`（9） |
| `dup3` 的 `oldfd == newfd` | `-EINVAL`（22）——这是 `dup3` 与 `dup2` 唯一的差别，Linux 如此 |
| `dup3` 的 `flags` 含 `O_CLOEXEC` | **接受**，新槽记上 `FD_CLOEXEC`；`execve` 成功换帧后由 `lx_fd_close_cloexec()` 还槽（§5 第 4 条、§8.13）。2026-11-27 之前这里回 `-EINVAL`，理由曾是「本层没有 exec 关闭语义」——那条理由现在不成立了 |
| `flags` 含除 `O_CLOEXEC` 之外的任何位 | `-EINVAL` |
| `newfd >= LX_MAX_FD + 3` | `-EBADF` |
| fd 表满（`dup` 要新槽） | `-ENFILE`（23）。Linux 在「进程 fd 表满」时给 `EMFILE`；本层只有一张固定大小的表（不是「内核文件表 + 进程 fd 表」两层），`pipe2` / `openat` 在同一条件下都用 `ENFILE`，这里跟同一口径，不单独造第二种回答 |
| 源是 socket | 照抄 `net` / `stype` / `sflags` / `pok` / `pbits`，两个槽指向**同一个 net 句柄**；最后一份关闭时才向 net 发一次 `CLOSE`（U-30，见下） |
| 目标是 socket 槽（含 `0..2` 别名指向 socket） | `-EINVAL`（22）。关掉那一格必须过 net 服务（有重放语义），本层不能在这里阻塞；硬退槽会漏句柄，硬留着则旧句柄永远没人关——如实拒绝 |

**复制的是「槽」，不是「打开文件表项」——唯一可见偏差。** 一次 `dup` 复制
`kind / pipe / size / wr / path`，两个槽各自持有自己的 `off`。管道与控制台没有游标，
语义与 Linux 完全一致；`openat` 打开的普通文件在两个副本上**各自从当前偏移读**
（Linux 是共享偏移）。这条偏差是刻意取舍：引入打开文件表项要给每一次 `read`/`write`
加一层间接与一套引用计数，而本层的文件读只有 `openat` + `read` + `pread64` 三个号
（§4），收益不抵复杂度。`pread64` 本来就不动游标，两条路都不受影响。

**标准流别名（`std_slot[3]`）为什么占一格真槽、而不共享源槽。** 共享槽做不到
`close(源)` 之后别名仍然有效——Linux 里 `dup3(w, 1)` 之后 `close(w)` 不会让 fd 1 失效。
所以 `fd 0..2` 命中别名时，`read` / `write` / `close` / `select` 一律先把它路由到
那一格；同一次 `dup3` 到同一个标准流时**复用原来那一格**（先摘别名再释放），
这样即使表满也能成功。fork 的子进程 1:1 照抄 `std_slot`（父的别名在子里仍然有效）。

**一个只有回头看才发现的坑。** `lx_fd_release()` 原来按「`kind != LX_FD_FILE` 就是
管道」判要不要减引用计数。新加的 `LX_FD_CONSOLE` 的 `pipe` 字段是 0，会被这条判据
当成「管道 0 的读者」去减——把一条真实管道悄悄减到 0 是在别处才炸的错。改成显式判
`kind == LX_FD_PIPE_R || kind == LX_FD_PIPE_W`，这条判据以后不会因为再添一种 fd 种类
而变形。

**实测读数（`[dup]` 行，QEMU LA64，56 条 check 全 `ok`）**

```
[dup] write /mnt/ram/linux_dup rc=0 size=9792
[dup] pipe2 rc = 0 ok
[dup] dup(writer) is a new fd = 1 ok
[dup] write through the copy = 5 ok
[dup] read from the original reader = 5 ok
[dup] content matches = 1 ok
[dup] close the original writer = 0 ok
[dup] copy still writable after close = 3 ok
...
[dup] dup(999) -> -EBADF = -9 ok
[dup] dup3(1, 1, 0) -> -EINVAL = -22 ok
[dup] dup3(1, 6, O_CLOEXEC) returns 6 = 6 ok
[dup] F_GETFD(6) sees FD_CLOEXEC = 1 ok
[dup] dup3(999, 1, 0) -> -EBADF = -9 ok
[dup] dup3(writer, 1) returns 1 = 1 ok
[dup] write(1) lands in the pipe = 3 ok
[dup] close(1) drops the alias = 0 ok
[dup] dup3(reader, writer) overwrites = 4 ok
[dup] overwritten fd is no longer a writer = -9 ok
[dup] dup(0) is a console reference = 1 ok
[dup] read the console reference -> EOF = 0 ok
[dup] fcntl: pipe2 rc = 0 ok
[dup] F_GETFD on a fresh fd -> 0 = 0 ok
[dup] F_SETFD(FD_CLOEXEC) -> 0 = 0 ok
[dup] F_GETFD reads FD_CLOEXEC back = 1 ok
[dup] F_GETFL(reader) = O_RDONLY|O_NONBLOCK = 2048 ok
[dup] F_GETFL(writer) = O_WRONLY|O_NONBLOCK = 2049 ok
[dup] F_SETFL clearing O_NONBLOCK on a pipe -> 0 = 0 ok
[dup] F_GETFL after clearing shows O_RDONLY = 0 ok
[dup] F_SETFL setting O_NONBLOCK back -> 0 = 0 ok
[dup] F_GETFL after setting shows O_NONBLOCK again = 2048 ok
[dup] F_GETFL(console) = O_RDWR|O_NONBLOCK = 2050 ok
[dup] F_DUPFD(0) allocates a new fd = 1 ok
[dup] F_DUPFD_CLOEXEC(9) -> 9 = 9 ok
[dup] the F_DUPFD_CLOEXEC copy carries FD_CLOEXEC = 1 ok
[dup] unknown cmd 99 -> -EINVAL = -22 ok
[dup] F_GETFD on a bad fd -> -EBADF = -9 ok
[dup] PASS: dup new fd, data through the copy, alias survives close, dup3 redirects stdout,
             dup3 overwrites, console reference, O_CLOEXEC accepted with FD_CLOEXEC readback,
             fcntl FD/flags/dupfd, EBADF/EINVAL on the failure surfaces
[dup] finished after 0 ms, exit code 56
```

**怎么读这 56 条。** ⓪ 前 36 条是 dup/dup3（第 1–5 组），后 20 条是 fcntl(25)
（第 6 组，§8.13 另有一节逐条讲）；①「复制」不是复制名字：`close the original writer` 之后
`copy still writable after close = 3` 证明两个描述符各自持有一次管道写端计数；
② 两端都关之后 `read -> EOF = 0` 证明计数真的减到了 0（没有泄漏，也没有提前归零）；
③ `dup3(writer, 1)` 之后 `write(1)` 的字节从**管道**里读回来，证明 `write` 真的按
别名路由，而不是往控制台打；④ `close(1)` 只摘别名、不动源；⑤ `dup3(reader, writer)`
把已经是写端的号覆盖成读端，随后 `overwritten fd is no longer a writer = -9` 与
`overwrite dropped the only writer -> EOF = 0` 两条一起证明「先释放再装」的顺序对；
⑥ 失败面（`EBADF` / `old==new` / 源无效）逐条读到负 errno 的具体值，`O_CLOEXEC`
这一格现在是**成功读数**（`returns 6 = 6 ok` + `F_GETFD(6) sees FD_CLOEXEC = 1 ok`）；
⑦ 夹具**输出走 fd 2**——第 3 组用例把 fd 1 重定向进管道，用 fd 1 打印会被自己吃掉，
这是本轮踩到的一个读数坑。

**U-30：socket 也能 `dup` 了（2026-11-27）。** 原来「源是 socket → `-EINVAL`」那一格
本轮清掉。实现刻意**没有**引入「打开文件表项 + 引用计数」那层结构——fd 表只有
`LX_MAX_FD` 个槽，扫一遍比维护一张必须在 `dup` / `close` / `close_all` / 槽重置 / fork
五处同步的计数器更难出错：

| 行为 | 实现 |
|---|---|
| `dup(sock)` | 副本槽照抄 `net` / `stype` / `sflags` / `pok` / `pbits`，两个槽指向**同一个 net 句柄**；**不发任何 net 请求**（宿主读数 `g_net_calls == 0`） |
| 谁真的关句柄 | `lx_sock_close` 先用 `lx_sock_refs()` 数「本进程内 `kind==LX_FD_SOCKET` 且 `net==该句柄` 的槽」：大于 1 就只退槽、回 0；**等于 1 才**向 net 发一次 `ARKRTOS_NET_SUB_CLOSE` |
| 任务退出收尸（`lx_sock_close_all`） | 用 `seen[LX_MAX_FD]` 按句柄去重，一个句柄只关一次 |
| 槽重置（`lx_fd_release`） | socket 五格（`net`/`stype`/`sflags`/`pok`/`pbits`）一并清零，避免下一个 `dup` 抄到脏值 |
| `F_SETFL(O_NONBLOCK)` | 同时写进**同句柄的每一格**（`sflags` 与 `fl` 双向同步）：Linux 里 `dup` 出来的描述符共享文件状态旗标，只改自己那格就是「`fcntl` 说改了、收发没变」 |
| `fd 0..2` 别名 | `lx_sock_idx()` 认别名：`dup3(sock, 1)` 之后 `write(1)` / `getsockopt(1, …)` 都按 socket 走；**没有别名**时对 `0..2` 回 `-ENOTSOCK`（原来一律 `-EBADF`，会把「这个 fd 不是 socket」误报成「fd 用错」） |
| `dup3(?, 目标槽是 socket)` | 仍 `-EINVAL`（含 `0..2` 别名指向 socket 的情形），理由见表里那一行 |
| fork | socket 槽**不继承**（子进程 fd 表里那一格是空的）；句柄引用计数只在本进程内成立，跨进程共享**没有**支持——这条写进[已知限制](已知限制.md) |
| 顺带修的真泄漏 | `lx_accept` 两条地址失败回滚路径原来只退 fd 槽、把刚拿到的 net 句柄留给「任务退出收尸」——槽退掉之后 `close_all` 再也扫不到它，句柄永远不关。现在回滚前先 best-effort 发一次 `ARKRTOS_NET_SUB_CLOSE` |

**实测读数。** 宿主 `linuxabisocket` 档整档 **964 checks**（U-30 新增 15 条：`dup(sock)`
拿到新号且 0 条 net 请求、副本 `write` 走 `SEND` 且句柄不变、关源 fd 不发 `CLOSE`、
副本仍可写、两份都活着时 `F_SETFL` 双向可见、副本置 `O_NONBLOCK` 后 `read` 立刻
`-EAGAIN`、最后一份 `close` 恰好发一次 `CLOSE`、`dup3(sock, 7)` 覆盖不存在的号、
`dup3(pipe, 3)` 与 `dup3(sock, 1)` 两种 `-EINVAL`、`getsockopt(1)` 走别名、
`getsockopt(0)` 无别名回 `-ENOTSOCK`、邻居槽不被带偏的 `F_SETFL` 同步）。
QEMU 夹具 socket 段加 10 条（`[sock] socket dup returns fd >= 3` 到
`[sock] dup copy still bound to 8081`），第八档从 232 条涨到 **242 条**；
`tools/fork_gate.py` 的 `EXPECT_SOCKET_CHECKS` / `EXPECT_THREAD_CHECKS` /
`EXPECT_QUOTA_CHECKS` 三处期望值同步 +10（另加 `LOCK_CHECKS`）。

**边界。** ① socket 副本共享同一句柄、最后一份关闭才真关（见上）；跨进程不共享；
② 文件副本的偏移各自独立（唯一可见偏差）；
③ `O_CLOEXEC` 已有真语义（exec 换帧后还槽），但只有 `openat` / `dup3` / `pipe2` /
`socket` 四条造 fd 的路径会记这个位——`fcntl(F_SETFD)` 也能记（§8.13）；④ `dup3` 的
`newfd` 上限是 `LX_MAX_FD + 3`，超过回 `-EBADF`（Linux 在号超过 `RLIMIT_NOFILE` 时
也是 `-EBADF`，只是它用 rlimit 而不是固定表）；⑤ 宿主档另有 94 条断言
（`test_linux_abi_host.c` 的 `t_dup` 40 条 + `t_fdflags` 54 条），逐条覆盖表满 `-ENFILE`、表满时
复用已有别名那一格、同一号关两次 `-EBADF`、`dup3(pw, 1, O_CLOEXEC)` 的别名槽也带
`FD_CLOEXEC` 且 exec 后 fd 1 真的回控制台等 QEMU 夹具里不好造的状态。

### 8.13 `fcntl`(25)：FD 旗标与状态旗标（2026-11-27，QEMU LA64 20 条 + 宿主 54 条）

`fcntl` 是 glibc 起运行时就要用的那几个号之一（`isatty` 会问 `F_GETFL`、
`__libc_fcntl64` 会给 fd 打 `FD_CLOEXEC`）。本节是 **fd 表这一层**能兑现的 cmd
（记录锁三把见 §8.16；`F_SETOWN` / `F_GETOWN` 与 `O_ASYNC` 见 §8.18），其余一律
`-EINVAL`：回 0 等于谎报「事情办成了」。

| cmd | 回答 |
|---|---|
| `F_DUPFD`(0) | 从 `max(arg, 3)` 起找最小空号；`arg < 0` 或越过 `LX_MAX_FD + 3` → `-EINVAL`；表满 → `-ENFILE` |
| `F_GETFD`(1) | 源存在回 `cloexec ? FD_CLOEXEC : 0`；不存在 `-EBADF` |
| `F_SETFD`(2) | 只认 `FD_CLOEXEC` 这一位（`arg` 里其它位被忽略，与 Linux 同）；返 0 |
| `F_GETFL`(3) | 回 `O_ACCMODE` + `O_APPEND` + `O_NONBLOCK`（U-29 之后再加 `O_ASYNC`）四位。管道如实报它被造出来时的 `O_NONBLOCK` 位（`pipe2` 带 `O_NONBLOCK` 才有；默认 0 = 阻塞，见 §6）；控制台回 `O_RDWR\|O_NONBLOCK` |
| `F_SETFL`(4) | 改 `O_APPEND` / `O_NONBLOCK` 两位，返 0；**管道上置/清 `O_NONBLOCK` 都接受**（U-26 之后管道本身可阻塞，清位不再是假承诺，与 Linux 同）。U-29 起 `O_ASYNC` 位也认（`ARKRTOS_USE_LINUX_SIGIO` 打开时），见 §8.18 |
| `F_DUPFD_CLOEXEC`(1030) | 同 `F_DUPFD`，新槽带 `FD_CLOEXEC` |
| `F_GETLK`(5) / `F_SETLK`(6) / `F_SETLKW`(7) | 见 §8.16（记录锁：所有者是进程，身份是文件路径，单位是字节区间） |
| `F_SETOWN`(8) / `F_GETOWN`(9) | 见 §8.18。`ARKRTOS_USE_LINUX_SIGIO` 关档时落 `-EINVAL`（与 U-29 之前逐位相同） |
| 其它任何 cmd | `-EINVAL`（不回 `-ENOSYS`；Linux 对不认识的 cmd 也是 `EINVAL`） |

**`F_SETFL` 对管道的语义在 U-26 变了。** 旧契约（管道恒非阻塞）下，为了让「清
`O_NONBLOCK`」不变成假承诺，`F_SETFL` 当场回 `-EINVAL`。U-26 把管道改成**默认阻塞**
（§6）之后，清位就是真的阻塞、置位就是真的非阻塞，于是这两位按 Linux 语义如实接受、
返 0；读数里四条（清 → 0 / 查回 `O_RDONLY` / 置回 → 0 / 再查是 `O_NONBLOCK`）就是
这个转变的证据。

**`FD_CLOEXEC` 的兑现点在 `execve`。** `lx_execve()` 铺完段、改完 trap 帧、重置完
堆游标之后调 `lx_fd_close_cloexec()` 遍历当前进程的 fd 槽，`used && cloexec` 的槽
逐个 `lx_fd_release()`。位置是刻意的：那是整条路径上唯一不可能失败的收尾，放在前面
任何一步失败都得回滚别人的 fd 表。撤回一条还要注意 `dup3(old, 1, O_CLOEXEC)` 这种
**标准流别名**——别名槽不在 `lx_fd_find()` 的视野里（它对 fd<3 恒回 -1），所以
`lx_dup3` 里用 `(rc <= 2) ? lx_std_slot(rc) : lx_fd_find(rc)` 定位。这个漏洞是宿主用例
「exec 之后管道读端读到 EOF」抓出来的：漏标记时 fd 1 仍指向管道，PASS 之外的那条
断言变成 `got 3 want 0`。

**实测读数（`[dup]` 段的第 6 组，QEMU LA64）**

```
[dup] fcntl: pipe2 rc = 0 ok
[dup] F_GETFD on a fresh fd -> 0 = 0 ok
[dup] F_SETFD(FD_CLOEXEC) -> 0 = 0 ok
[dup] F_GETFD reads FD_CLOEXEC back = 1 ok
[dup] F_GETFL(reader) = O_RDONLY|O_NONBLOCK = 2048 ok
[dup] F_GETFL(writer) = O_WRONLY|O_NONBLOCK = 2049 ok
[dup] F_SETFL clearing O_NONBLOCK on a pipe -> 0 = 0 ok
[dup] F_GETFL after clearing shows O_RDONLY = 0 ok
[dup] F_SETFL setting O_NONBLOCK back -> 0 = 0 ok
[dup] F_GETFL after setting shows O_NONBLOCK again = 2048 ok
[dup] F_GETFL(console) = O_RDWR|O_NONBLOCK = 2050 ok
[dup] F_DUPFD(0) allocates a new fd = 1 ok
[dup] F_DUPFD_CLOEXEC(9) -> 9 = 9 ok
[dup] the F_DUPFD_CLOEXEC copy carries FD_CLOEXEC = 1 ok
[dup] unknown cmd 99 -> -EINVAL = -22 ok
[dup] F_GETFD on a bad fd -> -EBADF = -9 ok
```

读数里 `2048` 是 `O_NONBLOCK`、`2049` 是 `O_WRONLY|O_NONBLOCK`、`2050` 是
`O_RDWR|O_NONBLOCK`——写成十进制是故意的：这三个数直接就是 `asm-generic` 的位值，
换成 `0x800` 反而看不出「`F_GETFL` 回的是原样拼出来的位、没有偷偷加料」这条契约。
（`pipe2` 这一步显式带了 `O_NONBLOCK`，否则 U-26 之后 `F_GETFL` 会回 `0` / `1`。）

**宿主档的 54 条**（`test_linux_abi_host.c` 的 `t_fdflags`）把 QEMU 夹具造不出来的
状态补齐：`dup3(pw, 1, O_CLOEXEC)` 的别名槽继承 CLOEXEC 且 exec 后 fd 1 回控制台
（关掉原写端后管道读端读到 EOF）、`openat(O_CLOEXEC)` 记位并在 exec 后被还、
`F_DUPFD(11)` 越界 `-EINVAL`、`arg < 0` 的 `-EINVAL`、`F_GETFL` 副本跟源、
`openat(O_APPEND/O_NONBLOCK)` 的两位在 `F_GETFL` 里读得回、文件 fd 上 `F_SETFL`
置/清这两位恒回 0 等。socket 档另有 12 条钉住「`fcntl` 改 fd 旗标要同时写回
`sflags`」——socket 层读的是 `sflags`，不同步就是「`fcntl` 说改了、收发没变」的
假承诺（置上 `O_NONBLOCK` 后 `recvfrom` 收不到数据当场 `-EAGAIN` 且不睡时间片）。

**边界。** ① 记录锁已于 2026-11-27 落地（§8.16），但只按**路径**认文件、全表 8 格、无死锁检测；
② `F_SETOWN` / `F_GETOWN` / `O_ASYNC` 已于 2026-11-27 落地（§8.18），但只支持「fd 变可读
（含 EOF）时报一次 SIGIO」，不做可写方向、实时排队、`si_fd`、进程组，且主人只认单任务号；
③ 没有 `F_GETPIPE_SZ` / `F_SETPIPE_SZ`——管道缓冲固定 256 字节（§6）。

### 8.14 `rseq`(293) 与 `getcpu`(168)：可重启序列与 CPU 号（2026-11-27，QEMU LA64 16 条 + 宿主 40 条）

`rseq` 是 glibc 与 Rust 标准库都在用的「无锁临界区」原语：程序把一段临界区的
`[start_ip, post_commit_offset)` 与 `abort_ip` 登记在一张 `struct rseq_cs` 里，内核
在**打断用户执行**的那一刻查这张表——PC 落在区间里就把它改到 `abort_ip`，程序于是
从「重来」的路径继续，而不是带着被撕开的假设返回区间中间。不发这个信号，临界区里的
读写顺序就成了假的。

**注册契约（`rseq(293)`）。** 四个参数：`struct rseq *`、`rseq_len`、`flags`、`sig`。
- `flags != 0` → `-EINVAL`；`rseq_len != 32` → `-EINVAL`（`struct rseq` 在 Linux UAPI
  里就是 32 字节，`sizeof` 不符的调用方本来也读不对字段）；
- 地址必须 32 字节对齐 → 否则 `-EINVAL`；
- 指针为 `NULL` → **注销**（Linux 同义），返 0；
- 注册时把 `cpu_id_start`（偏移 0）与 `cpu_id`（偏移 4）都写成 0；**写不进去回
  `-EFAULT` 且不留下登记**——注册成功却让程序读到一个假 CPU 号，比当场拒绝坏得多；
- 第 4 个参数 `sig`（中止指令流里的签名）本层不看：内核不在内核态执行用户的中止指令，
  签名只被 abort 路径解释，不看它不影响语义。

`struct rseq` 的其余字段（`flags`/`node_id`/`mm_cid` 等）本层不写，保持程序原本的内容。

**`getcpu(168)`。** 两个 4 字节出参（任一可为 `NULL`，越界 `-EFAULT`）+ 一个 `cache`
参数（不看）。单核机器上 CPU 号与 NUMA 节点号都是 0——**必须与 `rseq` 写回的 `cpu_id`
同值**：两处给出不同的号，程序就会在快路径（读 `rseq.cpu_id`）与慢路径（`getcpu`）
上看到两个世界，进而把「迁移过 CPU」当成事实去做内存序推断。

**两处中止点。** 本层只在「确实打断了用户执行」的两个位置查 `rseq_cs`：

| 时机 | 调用点 | 为什么在这里 |
|---|---|---|
| 信号投递 | `lx_signal_deliver()` 里、`lx_sig_build_frame()` **之前** | Linux 的 `rseq_ip_fixup` 也在信号处理之前跑。先修 PC 再铺帧，信号帧里存的自然就是 `abort_ip`，`rt_sigreturn` 之后从 abort 路径继续，而不是回到临界区中间 |
| tick 抢占 | LA64 `la_trap_c()` 的切换点（真正切栈之前），且要求被打断的是用户态（`PRMD & 3 != 0`） | 时间片被打断和被信号打断对程序是同一件事：`rseq_cs` 还挂着就说明它在临界区里 |

判据逐条对齐 Linux：`rseq` 已注册 / `rseq_cs` 非 0 / 该块可读 / `version` 与 `flags`
都是 0 / PC 落在 `[start_ip, start_ip + post_commit_offset)` 里。任何一条不成立都
**不动 PC**。改错一个不在这块数据上的现场，比什么都不做危险得多。

**顺序是硬的：先清 `rseq_cs`，再改 PC。** 反过来的话，PC 一改而清指针失败，程序回到
abort 路径后会看到「cs 还在」而再跑一遍临界区，同一段代码被执行两次。

**与 Linux 差一处（写在明面上）。** `rseq_cs` 的 `version`/`flags` 非 0 时，Linux 投
`SIGSEGV` 杀进程；本层在切换点里没有可投递信号的位置，于是**只清指针、不动 PC**——
保守方向：宁可丢掉一次中止（退化成「像没有 rseq 一样」），也不把一个非法内容的块当
真去跳。

**实测读数（`[rseq]` 行，QEMU LA64）**

```
  [rseq] write /mnt/ram/linux_rseq rc=0 size=8232
  [rseq] exec rc=0
  [rseq] getcpu rc = 0 ok
  [rseq] getcpu cpu = 0 ok
  [rseq] getcpu node = 0 ok
  [rseq] rseq register rc = 0 ok
  [rseq] cpu_id_start written 0 = 0 ok
  [rseq] cpu_id readback 0 = 0 ok
  [rseq] signal aborted the critical section = 1 ok
  [rseq] post-commit did not run = 0 ok
  [rseq] rseq_cs cleared by the abort = 0 ok
  [rseq] handler ran once = 1 ok
  [rseq] normal critical section completed = 1 ok
  [rseq] normal section not falsely aborted = 1 ok
  [rseq] rseq_cs cleared by the commit = 0 ok
  [rseq] rseq len != 32 -> -EINVAL = -22 ok
  [rseq] rseq flags != 0 -> -EINVAL = -22 ok
  [rseq] rseq(NULL) deregisters = 0 ok
  [rseq] PASS: getcpu both outputs zero, rseq registered with cpu_id written, signal delivery aborted the critical section at abort_ip with rseq_cs cleared, commit path completed, invalid contract refused, NULL deregisters
  [rseq] finished after 0 ms, exit code 57
```

`exit code 57` 是夹具自己约定的成功码；`post-commit did not run = 0` 是「提交段一次也
没执行」的正向证据，`normal section not falsely aborted = 1` 是「没有信号时不许误中止」
的反向对照——两条一起才说明中止是按区间判的，不是「见到 `rseq_cs` 就跳」。

**宿主档的 40 条**（`test_linux_abi_host.c` 的 `t_rseq`）把 QEMU 夹具造不出来的状态补齐：
`getcpu` 三正两负（`NULL` 出参、越界 `-EFAULT`）；注册契约的每一条拒绝面（`len`
/ `flags` / 对齐 / `NULL` 注销 / `-ESRCH`）；`g_to_budget=1` 造「`cpu_id` 写不进去」
→ `-EFAULT` 且不留登记；抢占点上 PC 在区间内 → 返回 1、`ERA` 等于 `abort_ip`、
`rseq_cs` 被清 0，PC 在区间外 / `cs == 0` / `version != 0` / `cs` 指针不可读 → 一律
返回 0 且不动 PC；注销后与 `reset` 后作废；信号投递点用 `g_export_era` 抓「导出那一刻
的用户 PC」，把「信号帧里存的确实是 `abort_ip`」变成真断言而不是推断。

**夹具为什么要 `-O0`（真发现的工具链缺陷）。** 这个夹具靠 `&&label` 界定可重启序列的
三个端点。实测本工具链（LoongIDE GCC 8.3 的 LoongArch 端口）在 `-O1`/`-O2` 下对标签
取地址的代码生成不可靠：
- 中止块在 CFG 里不可达时会被**整块删掉**（`s_aborted = 1` 那条 store 消失），`&&label`
  随即落到邻近块的首址（实测 `start`/`abort` 双双变成 `linux_main` 入口、`post` 变 0，
  于是 `[start, start+post)` 退化成空区间，中止永远不发生）；
- 同一页内多个标签并存时立即数会算错（最小复现：两个标签相距 8 字节，编译出的差值是 -4）。

修法是两条：夹具用 `-O0`（该档下地址正确），并加一个**恒 0 的 `volatile` 分支**
`if (s_zero_guard) goto cs_abort;`——给中止块一条编译器看得见的边，块留下来，标签地址
也就对了；运行时这条分支一次也不走。`example/qemu_loongarch64/Makefile` 里那条规则
的 `-O0` 与注释就是为这件事写的。

**门禁。** `tools/linux_abi_gate.py` 的 `syscall-table` 从 27 条涨到 **30 条**（`RSEQ`
293、`GETCPU` 168 进号表），`qemu-readout` 新增「rseq 块 16 条 check 全 ok + PASS 行 +
`exit code 57`」，`parser-selfcheck` 从 12 种坏样本涨到 **16 种**（rseq 块：去 PASS 行、
退出码改 1、插一条 FAIL、少一条 check）。

**边界。** ① 单核：`cpu_id`/`node_id` 恒 0，没有迁移，也没有 `mm_cid`；② tick 抢占那条
路径本机没有夹具（夹具只验信号投递点），宿主档两条抢占断言覆盖的是同一个 `lx_rseq_fixup`；
③ `rseq_cs` 非法内容只清指针不杀任务（见上「与 Linux 差一处」）；④ 不实现
`RSEQ_FLAG_UNREGISTER`（`flags != 0` 一律 `-EINVAL`）。

### 8.15 阻塞管道的跨执行流语义（U-26，2026-11-27，QEMU LA64 fork 档 20 条）

§6 给的是**单执行流**能证明的部分（有数据、EOF、`-EPIPE` + SIGPIPE）。「空读挂起、等到
别人写」和「满写挂起、等到别人排空」必须有两个执行流才证得了——单执行流一挂起就再
也回不来，那不是可证伪的读数。这一组因此放在 `example/qemu_loongarch64/elf/linux_fork.c`
里（只在 `ARKRTOS_USE_LINUX_FORK=1` 时才构建，被 `tools/fork_gate.py` 的**十档**各跑
一遍）。

**A 组：阻塞读等到别人写**

1. 父进程建一条 `pipe2(..., 0)`（默认阻塞），`clone` 一个子进程；
2. 父进程**先关掉自己那份写端**——此刻唯一的写者是子进程，所以父的阻塞读必须真的挂起；
3. 子进程忙等 60 ms（`clock_gettime` 轮询，没有 `nanosleep`），写 `"abc"`，关写端，`exit 11`；
4. 父进程的读回到 `3`、内容 `"abc"`，**墙钟 ≥ 40 ms**；
5. 子进程已退出且写端全关，父的下一次阻塞读**立刻**回 `0`（EOF 不是等待条件，不许
   白睡一个 100 ms 时间片）；
6. `wait4` 回收子进程、退出码 `11`。

**B 组：阻塞写等到别人排空**

1. 父进程建数据管道 + 状态管道各一条；
2. 父先写 256 个 `'A'` 把环形缓冲**填满**，`clone` 子进程，然后写 300 字节图案
   （`(uint8)(i & 0xFF)`）——一个字节都写不进去，必须挂起；
3. 子进程忙等 60 ms，先排空 256 个 `'A'`，再把 300 字节排空，**逐字节对图案**；
4. 父的写回 `300`（阻塞写是**写全**的，不是短写），**墙钟 ≥ 40 ms**；
5. `wait4` 回收子进程、退出码 `13`；
6. 子进程把「共读到 556 字节」和「图案全对（0）」各写 5 字节回状态管道，父读它——
   跨进程的读数不能只靠退出码，退出码只有 8 位。

**实测读数（`[fork]` 行，QEMU LA64 fork 档）**

```
[fork] cross-task blocking read pipe2 == 0 -> 0 ok
[fork] clone for blocking reader > 0 -> 1 ok
[fork] blocking pipe read waited 61 ms
[fork] blocking pipe read returned 3 -> 3 ok
[fork] blocking pipe read content -> 1 ok
[fork] blocking pipe read waited >= 40 ms -> 1 ok
[fork] blocking pipe read after writer exit -> EOF -> 0 ok
[fork] EOF read did not sleep a slice -> 1 ok
[fork] blocking reader child reaped -> 12 ok
[fork] blocking reader child exit 11 -> 11 ok
[fork] cross-task blocking write pipe2 == 0 -> 0 ok
[fork] status pipe2 == 0 -> 0 ok
[fork] fill the 256-byte pipe buffer -> 256 ok
[fork] clone for blocking writer > 0 -> 1 ok
[fork] blocking pipe write waited 161 ms
[fork] blocking pipe write wrote all 300 -> 300 ok
[fork] blocking pipe write waited >= 40 ms -> 1 ok
[fork] blocking writer child reaped -> 12 ok
[fork] blocking writer child exit 13 -> 13 ok
[fork] child report came back -> 5 ok
[fork] child read 556 bytes in total -> 556 ok
[fork] child saw the exact byte pattern -> 0 ok
```

（`waited 161 ms` 而不是 60 ms：父进程的阻塞写要等子进程排空两轮，中间必然跨过至少
一个 100 ms 时间片。读数只断言「≥ 40 ms」，不拿具体毫秒数当判据。）

**这一组同时暴露并修掉的一个真缺陷：`SIGCHLD` 不该打断阻塞调用。** 第一版实现里
`lx_sig_pending_unblocked()` 只判「pending & ~blocked」是否非空，于是 `wait4` 回收
子进程之后（本层**不清** SIGCHLD 的 pending 位，Linux 也不清），父进程的下一次阻塞
`read`/`write` 立刻回 `-EINTR`。Linux 的 `get_signal()` 对**默认动作是「忽略」**的
待投递信号（SIGCHLD/SIGCONT/SIGURG/SIGWINCH 在 `SIG_DFL` 下，以及显式 `SIG_IGN`）
直接销掉返回 0：既不铺帧，也不打断系统调用。判据已按这个口径改成逐位看 disposition，
`t_pipe_block` 之外宿主 futex 的 `-EINTR` 用例仍用装了 handler 的 SIGUSR1 取证，语义不变。

**门禁。** `tools/fork_gate.py` 十档的 `EXPECT_*_CHECKS` 全部 +20（47 / 52 / 60 / 147 /
167 / 178 / 194 / 232 / 265 / 277；U-27 之后又各自 +32，见 §8.16），并把三条新锚点写进 `judge()` 的必现清单：
`blocking pipe read waited >= 40 ms -> 1 ok`、`blocking pipe write wrote all 300 -> 300 ok`、
`child saw the exact byte pattern -> 0 ok`——只比总条数时，「少写一条、多写一条」仍可能
凑出同一个数，那是假绿。

**边界。** ① 单核：两个进程同时睡在同一根管道上未验；② 等待粒度 100 ms 时间片；
③ 缓冲 256 字节、最多两条管道；④ 阻塞读写要求 port 支持 `arkrtos_port_svc_restart()`
（无重放能力时读回 `-ENOSYS`、写回已写入的字节数）；⑤ `SA_RESTART` 未实现，
`-EINTR` 属防御路径。

### 8.16 `fcntl` 记录锁（5 / 6 / 7）：跨进程的字节区间锁（2026-11-27，QEMU LA64 fork 档 32 条 + 宿主 56 条）

**为什么单独一节。** POSIX 记录锁的所有者是**进程**，不是 fd、不是任务，也不是文件描述。
三条语义由此推出，本层全部照办：

* 同一进程重复加锁不和**自己**冲突，只把自己那一段的旧锁换成新锁（切/劈）；
* `close` 掉这个文件的**任意**一个 fd，本进程在这个文件上的锁**全部**释放——Linux 就是
  这么定的（锁属于进程，`close` 无法知道还有没有别的 fd 指着同一个文件）；
* 锁**不随 `fork` 继承**（子进程是新的进程槽），但**随 `execve` 保留**（`execve` 不动进程槽）。

单执行流证不了这两条里的任何一条：同一个 owner 永远不冲突，`close` 释放也分不清「按 fd
放」与「按进程放」。所以可证伪的读数在 `ARKRTOS_USE_LINUX_FORK=1` 的夹具里（与 §8.15 同
一支 `linux_fork.c`，`tools/fork_gate.py` 十档各跑一遍）；宿主侧只负责**容量**与参数校验。

**数据结构与身份。** `s_rslk[LX_RSLK_MAX]`（8 格），每条是
`{used, type, owner, pid, start, len, path[LX_PATH_MAX]}`，`owner = 进程槽 + 1`（0 = 空条目）。
文件身份**只有路径**——本层没有 inode，硬链接 / 绑定挂载那种「不同路径、同一个文件」分不
出来，写进已知限制。锁表满回 `-ENOLCK`，**不淘汰别人的锁**：静默丢掉一把别人正靠着的锁，
比如实拒绝糟得多。

**布局。** 按 asm-generic 的**64 位** `struct flock` 解析：`l_type@0`、`l_whence@2`、
`l_start@8`、`l_len@16`、`l_pid@24`，共 32 字节。字段一律按小端逐字节读写（不 `memcpy` 到
结构体：宿主与目标字宽不同）。`off_t` 的**高 32 位必须是低 32 位的符号扩展**，给别的值回
`-EINVAL`——把一个越界偏移截断成「看起来合法」的假区间，比拒绝糟。`l_len == 0` 是「到
EOF」，等价上界 `0xFFFFFFFF`；`start + len` 加出溢出也按到 EOF 收口。

**三把 cmd 的答法。**

| cmd | 答法 |
|---|---|
| `F_SETLK`(6) | 无冲突就装入；有冲突回 `-EAGAIN`（Linux 对 `F_SETLK` 也是 `EAGAIN`，只有 `F_SETLKW` 才可能等） |
| `F_GETLK`(5) | 有冲突回**起始偏移最小**的那把（Linux 也挑 f_pos 最小者），五个字段全填；无冲突只把 `l_type` 改成 `F_UNLCK`，其余四格**原样不动**（Linux 也不动）。拿 `l_type = F_UNLCK` 来问回 `-EINVAL` |
| `F_SETLKW`(7) | 有冲突就登记「谁在等哪个 fd」（`s_rslk_wait[任务号]`，4 字节），睡一个 `LX_WAIT_SLICE_MS = 100` 时间片后 `LX_RET_RETRY` 重放；有未阻塞信号回 `-EINTR`；端口不能重放则 `-ENOSYS` |

冲突判定：同一路径 + 区间有交集 + 「我要写或它要写」（两把读锁不冲突）。`F_UNLCK` 走切
锁：整段覆盖就整条删、砍头 / 砍尾就缩短、中间开洞就劈成两条——劈裂要空位，所以**先数清
楚再改**，不足 `-ENOLCK`。

唤醒只在**解锁**与**释放**两处发生，且只负责叫人：判定留在重放那一轮（条件可能在叫醒与
重放之间被别人改掉，伪唤醒无害）。`lx_fd_release()` 见到文件 fd 就调
`lx_rslk_release_path(slot, path)`；进程槽被重置（子进程退出 / 回收）就
`lx_rslk_release_owner(slot)` 并无差别全唤醒。

**实测读数（`[fork]` 行，QEMU LA64 fork 档；lock 段 32 条全 ok）**

```
[fork] lock parent F_SETLK [0,100) -> 0 ok
[fork] lock own lock is not a conflict -> 2 ok
[fork] lock child SETLK on parent lock == -EAGAIN -> -11 ok
[fork] lock child disjoint SETLK -> 0 ok
[fork] lock child SEEK_END SETLK -> 0 ok
[fork] lock parent GETLK disjoint start is 200 -> 200 ok
[fork] lock parent GETLK eof pid is child -> 6 ok
[fork] lock child GETLK type is WRLCK -> 1 ok
[fork] lock child GETLK pid is not self -> 1 ok
[fork] lock child GETLK start is absolute 0 -> 0 ok
[fork] lock child GETLK whence is SEEK_SET -> 0 ok
[fork] lock waiter blocked >= 40 ms -> 1 ok
[fork] lock free after child close (conflict range) -> 0 ok
[fork] lock child reaped -> 12 ok
```

`F_GETLK` 回的 `pid` 是**子进程**的任务号 6，不是自己的；`whence` 被改写成 `SEEK_SET`、
`start` 是绝对偏移——这三条合起来才能排除「原样回声」的假实现。阻塞那一条不比绝对时刻，
而是「子进程先报 `'P'`、父进程再拖 100 ms 才解锁、子进程量自己的墙钟 ≥ 40 ms」。

**门禁。** `tools/fork_gate.py` 的 `LOCK_CHECKS = 32`，十档 `EXPECT_*_CHECKS` 统一 +32
（79 / 84 / 92 / 179 / 199 / 210 / 226 / 264 / 297 / 309）；`judge()` 另钉三条必现读数：
`lock child GETLK pid is not self -> 1 ok`、`lock waiter blocked >= 40 ms -> 1 ok`、
`lock free after child close (conflict range) -> 0 ok`。只比总条数时，「少写一条、多写一条」
仍可能凑出同一个数，那是假绿。

**覆盖率口径（本笔把 `arkrtos_linux_abi` 基线从 98.82% 重锚到 97.00%）。** 记录锁整节新增
约 730 行，其中 **61 行在宿主上原理不可达**：宿主档 `ARKRTOS_USE_LINUX_FORK=0` 时
`lx_proc_cur()` 恒回槽 0，于是表里每把锁的 owner 都相同，`lx_rslk_conflict()` 的
`owner == owner` 分支把每条已用条目都跳过——冲突扫描主体、切锁劈裂、等待表唤醒、阻塞睡
下、`F_GETLK` 命中回填这五处全走不到。宿主唯一可观测的是**容量**（8 格，第 9 把
`-ENOLCK`）与参数校验（宿主 56 条）。这 61 行由 `fork_gate` 十档在目标上覆盖（上面的读数
即证据）；不为了凑数字把它们改成死代码或塞 `#if` 假分支。

**边界。** ① 无死锁检测：`F_SETLKW` 的环回等待会一直睡到别的锁释放或被信号打断，
`EDEADLK` 不回；② 无 OFD 锁（`F_OFD_SETLK`）、无租约（`F_SETLEASE`）；③ 全表 8 格
（`-ENOLCK`），不按 inode 分表；④ 无 `/proc/locks`；⑤ 只认 64 位 `struct flock`（LA64 是
本层唯一的 Linux ABI 载体，32 位的 16 字节布局没有载体）；⑥ 单核：两个进程同时睡在同一
把锁上未验（唤醒是全叫、判定在重放，原理上不会丢）。

### 8.17 调度策略（U-28）：内核 deadline 类与 `sched_*` 十二个号（2026-11-27，QEMU LA64 63 条 + 宿主 52 + 633 条）

**为什么这一节要分两处证。** 调度策略有两类读数，混在一处就只能证一半：

* **号、参数、错误码**（翻译层）——「`sched_setscheduler(0, SCHED_FIFO, {88})` 回 0，之后
  `sched_getscheduler(0)` 回 1、`sched_getparam(0)` 回 88」这类往返读数，在 QEMU 上可证伪；
* **选择次序与预算**（内核 EDF 类）——「哪条先跑」在 QEMU 上依赖墙钟与调度抖动，不是可
  证伪读数。这一半只能在宿主 `test_task_host.c` 里，用可控的 `arkrtos_kernel_tick` 与直接
  摆好的 TCB 断言（`test_deadline_class`，52 条）。

两处合起来才是完整证据。只做前者时，「EDF 其实是按优先级挑的」也能全绿；只做后者时，
「十二个号其实翻译错了」也能全绿。

**内核侧：一个 deadline 类，不是一套 CFS。** 本内核是固定优先级抢占式调度器。U-28 没有把
它改造成按权重分配的调度器，而是在固定优先级之前插一个 deadline 类（Linux 也是这么分层的：
`stop > dl > rt > fair > idle`）：

* `arkrtos_dl_top()` 在**所有**固定优先级任务之前选：绝对截止时刻（`dl_abs_tick`）最早者胜，
  同刻按任务号小者；节流中、非 `READY`/`RUNNING`、次核任务一律跳过。`arkrtos_ready_top()`
  与 `sc_scan_best()` 都先问它一次，所以「deadline 类高于一切固定优先级」在两条选择路径上
  是同一个事实，不是两处各写一份。
* 每条 deadline 任务在 TCB 里带六个数：`dl_runtime_ns/dl_deadline_ns/dl_period_ns`（原参数，
  给人看数与算带宽用）与 `dl_runtime_tick/dl_deadline_tick/dl_period_tick`（换算成拍，
  给调度器用），再加 `dl_budget_tick`、`dl_abs_tick`、`dl_throttled`、`dl_active`。
* 时间参数一律**向上取整**换算成拍：1 ns 的预算不能被算成 0 拍（那等于设了没设）；向上取整
  是单调的，所以 `runtime <= deadline <= period` 换算后仍成立。

**准入。** `Σ(runtime/period) <= 95%`（Linux 的默认 RT 带宽），超出回 `-EBUSY`（不是
`-EINVAL`：参数合法，是带宽不够）。算这一笔时**先把自己旧的那一份扣掉**——否则同一任务
原地改参数会被自己的旧带宽顶出去。用 ns 原参数算而不用拍反推：拍是向上取整的，反推会把
比值算偏。

**每拍怎么结算（`arkrtos_dl_tick()`，在 tick body 里、`sched_tick_sweep` 之前）。**

1. 先看**周期边界**：`dl_abs_tick` 到了或过了，就顺延一个周期、补满预算、解除节流并把任务
   挂回就绪集；顺延之后仍然落后（系统已过载）就直接重新对齐到 `now + period`，**不补一串
   历史预算**——那会把一次过载变成雪崩。
2. 再扣**当前任务一拍**：`arkrtos_current_task_id` 指的就是被这一拍打断的那个任务，扣它
   就是扣「真正占着 CPU 的拍」。空闲任务（`<= 0`）不扣任何人的预算。
3. `dl_budget_tick` 归零就挂牌节流：`dl_throttled = 1` 并**从就绪集摘下**。不能改成「选中
   时跳过」——那样它会一直占着桶头，把同优先级的普通任务挡死。
4. 只有**一条** deadline 任务时不节流（Linux 同型）：没有同类的竞争者就没有需要它让出的
   对象，节流只会白扔预算；但预算照记，免得后来加进第二条时它凭空多出一整轮。

**Linux 侧：十二个号的映射。**

| 号 | 名字 | 本层答法 |
|---|---|---|
| 118 | `sched_setparam` | 只改优先级，策略不变；`OTHER`/`BATCH`/`IDLE` 上 `priority != 0` 回 `-EINVAL`，`DEADLINE` 回 `-EINVAL`（同 Linux：改用 `sched_setattr`） |
| 119 | `sched_setscheduler` | `pid`/`policy`/`sched_param*`；`DEADLINE` 没有承载三元组的字段，回 `-EINVAL` |
| 120 | `sched_getscheduler` | 读 TCB 里的 `sched_policy` |
| 121 | `sched_getparam` | 由内核优先级反推：FIFO/RR 是 `100 - priority`，其余策略一律 0 |
| 122 / 123 | `sched_set/getaffinity` | 单核：`len >= sizeof(unsigned long)`、掩码只认 bit0 且必须置位，否则 `-EINVAL`；读回恒为 1 |
| 124 | `sched_yield` | 把自己挪到同优先级桶尾（桶里只有自己时是空操作），再挂一次切换请求 |
| 125 / 126 | `sched_get_priority_max/min` | FIFO/RR 是 99 / 1；`OTHER`/`BATCH`/`IDLE`/`DEADLINE` 都是 0；认不出的策略 `-EINVAL` |
| 127 | `sched_rr_get_interval` | 轮转量 = 内核时间片 10 拍 × 500 us = 5 ms（`sec=0 / nsec=5000000`） |
| 274 | `sched_setattr` | `struct sched_attr` 逐字段按字节读（VER0 48 字节；`size < 48` 回 `-EINVAL`）；系统调用 `flags` 或 `sched_flags` 非 0 一律 `-EINVAL`；`DEADLINE` 走 `arkrtos_task_dl_set()`，准入失败 `-EBUSY`；非 deadline 策略上三个时间参数必须全 0 |
| 275 | `sched_getattr` | 回写同一布局；`size >= 56` 时把两个 util 字段写成 0（本内核不做利用率跟踪，但字段存在就必须给定值，不能留垃圾） |

策略落格（固定优先级是「数小者高」）：

| policy | Linux 参数 | 内核优先级 |
|---|---|---|
| `SCHED_FIFO`(1) / `SCHED_RR`(2) | `priority` 1..99 | `100 - priority`（99 -> 1，1 -> 99） |
| `SCHED_OTHER`(0) / `SCHED_BATCH`(3) | `nice` -20..19 | `128 + nice`（108..147） |
| `SCHED_IDLE`(5) | `priority` 必须 0 | 147（普通档最末格，**不是**「只在空闲时跑」） |
| `SCHED_DEADLINE`(6) | 只能经 `sched_setattr` 的三元组 | 内核 deadline 类 |

**`policy` 存哪里。** 存进 TCB（`sched_policy`），随 `arkrtos_task_register()` 清零。不另开
一张 ABI 层私表：任务槽会被复用，私表与槽位迟早脱节（复用的槽会带着上一个任务的策略）。

**为什么 `SCHED_IDLE` / `SCHED_BATCH` 只算「近似」。** 本内核没有 CFS：`nice` 只是把任务放
到普通档的某一格，没有权重、没有延迟唤醒；`SCHED_IDLE` 落在最末一格，别的普通任务都优先于
它，但它不是「系统空闲才跑」——内核里没有空闲让路判定。不实现组调度、cgroup 带宽，也不实现
`SCHED_RESET_ON_FORK` / `SCHED_FLAG_RECLAIM` / `SCHED_FLAG_DL_OVERRUN`：认了却不做，就是把
调用方的预期变成谎言，所以 flags 非 0 一律回 `-EINVAL`。

**实测读数（`[sched]` 行，QEMU LA64 fork 档，63 条全 ok）**

```
[sched] getscheduler default -> 0 ok
[sched] getparam FIFO priority is 88 -> 88 ok
[sched] rr interval nsec is 5000000 -> 5000000 ok
[sched] setattr DEADLINE 100% bandwidth -> EBUSY -> -16 ok
[sched] setattr DEADLINE 1ms/2ms/100ms is admitted -> 0 ok
[sched] getattr policy is DEADLINE -> 6 ok
[sched] getattr DEADLINE deadline round-trips -> 2000000 ok
[sched] setaffinity short len -> EINVAL -> -22 ok
[sched] sched_yield -> 0 ok
```

**宿主侧的读数（`test_task_host.c` 的 `test_deadline_class`，52 条）。** 逐条断言：三条硬
约束（`runtime == 0`、`runtime > deadline`、`deadline > period` 各回 `-1` PARAM）；参数落账
（1 ms / 2 ms / 100 ms = 2 / 4 / 200 拍，`dl_abs_tick = now + 4`，带宽 10,000,000 ns/s）；100%
带宽 `-EBUSY` 且**槽位一个字段都没被半写**；第二条 1% 通过（带宽累加到 20,000,000）；EDF 选
更早截止者（**后设**的任务胜，所以不是「按任务号」）；到周期边界顺延 200 拍并补满预算后次序
反转；两条在场时扣光即节流（**边界补满后同一次 tick 还会再扣 1，所以断言 `budget == 1` 而
不是 2**）；节流者不入选且离开就绪链；下一次边界补满并挂回；`dl_clear` 清字段、释放带宽、
仍在就绪集、越界号静默；单条 deadline 任务不节流但照扣预算；落后超过一个周期重新对齐
（`abs == tick + 200`、`budget == 1`）；`kill` 释放带宽（槽位 INVALID、`dl_active = 0`、
离开就绪链、带宽归 0）。

**覆盖率（不重锚基线）。** `linuxabisched` 这一档把 `arkrtos_linux_abi` 的 sched 段从
**24 行未覆盖清零**：宿主桩的拷贝预算（`g_from_budget` / `g_to_budget`）本来就能造「第一块
成功、第二块失败」，所以 `copy_from_user` / `copy_to_user` 的每一条 `-EFAULT` 分支都走得到，
不是靠「原理不可达」写过去的。合并七档后 `arkrtos_linux_abi` 由 96.55% 升到 **97.25%**
（基线 97.00% 不动）；`test_linux_abi_host.c` 由 606 条断言增到 **633 条**（新增 27 条错误面）。

**门禁。** `tools/fork_gate.py` 的 `SCHED_CHECKS = 63`：`[sched]` 前缀单独计数，十档的
`EXPECT_*_CHECKS` 一个都不用改（那些是 `[fork]` 前缀）。九条必现读数逐条校验（上面那段），
坏日志自检另加三种 sched 形态（少一条、插一条 FAIL、改一个数字）。夹具里 deadline 那六条按
`ARKRTOS_USE_SCHED_DEADLINE` 分两支——类开着期望 `-EBUSY` 与三元组，类没开期望 `-EINVAL`
与零值，两支条数相同，所以 `SCHED_CHECKS` 不随档变化。`tools/linux_abi_gate.py` 的
`EXPECT_SYS` 逐条对 asm-generic 号（118-127、274/275 共 12 条）；默认档那里这十二个号如实
回 `-ENOSYS`。

**边界。** ① 没有 CFS（见上），`nice` 只是落格；② `SCHED_IDLE` 不是真空闲；③ 无组调度、
无 cgroup 带宽；④ `SCHED_RESET_ON_FORK` / `RECLAIM` / `DL_OVERRUN` 一律 `-EINVAL`，不假装；
⑤ affinity 只有 CPU0（单核）；⑥ deadline 参数按 `uint32` ns 存，超过 4.29 s 回 `-EINVAL`；
⑦ 节流粒度是拍（500 us），不是 Linux 那种 `hrtimer` 级的补充；⑧ 无 `sched_setattr` 的
`SCHED_FLAG_DL_OVERRUN` 检测，超额运行只能靠节流兜住。

### 8.18 异步 I/O（U-29）：`F_SETOWN` / `F_GETOWN` / `O_ASYNC` + SIGIO（2026-11-27；U-31 起 QEMU LA64 38 条 + 宿主 72 条）

**是什么、不是什么。** Linux 的异步 I/O 通知有三个层次：`fcntl(F_SETOWN)` 登记「出了事
发给谁」、`F_SETFL(O_ASYNC)` 打开这个 fd 的通知开关、内核对「fd 状态变化」投一次 `SIGIO`。
本节只做**管道 fd 变可读（含写端全关的 EOF）时投一次 SIGIO**，其余一律如实不做：

* 可写方向（`POLLOUT` 型的通知）不做——本层的写路径没有「空间变多」这个事件（阻塞写是
  自己睡醒重放，不是事件通知）；
* 边缘触发去重不做：每次 `lx_pipe_wake(pi, 0)` 都投一次，与 Linux 的 `SIGIO` 语义同一
  档（Linux 也不保证「每个字节一次」）；
* 实时信号排队（`sigqueue`）不做；`SA_SIGINFO` 的三参派发与 `si_band` / `si_fd`
  在 U-31 已做（§8.19），`SIGIO` 仍是普通信号 29；
* 进程组（`F_SETOWN` 传负值）不做：Linux 允许把通知发给进程组，本层没有进程组模型，
  负值一律 `-EINVAL` 并计数 `s_sigio_pgrp_refused`，**不假装登记上了**。

**为什么是「扫描 fd 表」而不是「注册表」。** 通知的判据是「这个管道读端现在有没有
`O_ASYNC` 和主人」，真源就是 fd 槽本身。另建一张注册表就必须在 `close` / `dup` /
`execve` / 进程退出 / fork 五处同步，漏一处就会给一个已经不存在的 fd 发信号；扫描是
`LX_MAX_FD × LX_PROC_MAX` = 8 × 16 次整数比较，代价可以忽略，换来的是「不可能不同步」。

**`dup` 的共享语义（U-29 顺手补的真缺陷）。** Linux 上 `dup` 复制的是 **file
description**，所以 `F_SETFL` / `F_SETOWN` 改的是那个对象，所有副本立刻看得见。本内核
没有独立的 `struct file`，fd 槽里存的是登记快照，`dup` 只复制。U-29 在 `lx_fd_set_fl()`
与 `lx_fd_owner_cmd()` 收尾调 `lx_fd_sync_peer()`：把改动同步到**同一进程内、同一
kind、同一 pipe 对象**的其它 fd（管道是唯一「一个对象被多个 fd 引用」的 kind）。
夹具「在 dup 副本上清 `O_ASYNC` → 原 fd 立刻不再投递」与宿主用例的双向 5 条断言就是
这条实现被钉死的地方——只有快照复制的实现在这里必红。

**`O_ASYNC` 在关档时是真关。** `ARKRTOS_USE_LINUX_SIGIO = 0` 时 `F_SETOWN` /
`F_GETOWN` 落 `-EINVAL`（与 U-29 之前逐位相同），`F_SETFL` 忽略 `O_ASYNC` 位、
`F_GETFL` 读不回它，`lx_fd_t` 里连 `owner` 这一格都不存在。

**投递点。** 与其它信号一致：`lx_finish()` 是唯一的投递点（`LX_RET_PTR_DONE` /
`EXEC_DONE` / `RETRY` 三条早退不投递）。所以「写一个字节」这一次系统调用的返回路径上
handler 被调到，`rt_sigreturn` 之后 `a0` 仍是被打断的那次调用的返回值（写入字节数）——
信号不吞返回值，也不吞管道里的字节。

**实测读数（`[sio]` 行，QEMU LA64 fork 档，38 条）**

```
[sio] getown on console is 0 -> 0 ok
[sio] setown on console -> EINVAL -> -22 ok
[sio] pipe2 -> 0 ok
[sio] setown to self -> 0 ok
[sio] getown reads back the task id -> 11 ok
[sio] setown process group -> EINVAL -> -22 ok
[sio] setfl O_ASYNC -> 0 ok
[sio] getfl reports O_ASYNC -> 1 ok
[sio] dup read end ok -> 1 ok
[sio] dup shares the owner -> 11 ok
[sio] dup shares O_ASYNC -> 1 ok
[sio] rt_sigaction(SIGIO) rc -> 0 ok
[sio] write after O_ASYNC returns the byte -> 1 ok
[sio] SIGIO handler ran -> 1 ok
[sio] SIGIO signo is 29 -> 29 ok
[sio] SIGIO siginfo pointer is set -> 1 ok
[sio] SIGIO si_signo -> 29 ok
[sio] SIGIO si_code is POLL_IN -> 1 ok
[sio] SIGIO si_band -> 65 ok
[sio] SIGIO si_fd -> 3 ok
[sio] SIGIO ucontext pc is non-zero -> 1 ok
[sio] read still returns the byte -> 1 ok
[sio] the byte survived -> 105 ok
[sio] clear O_ASYNC -> 0 ok
[sio] getfl no longer reports O_ASYNC -> 1 ok
[sio] write after clearing O_ASYNC returns the byte -> 1 ok
[sio] clearing O_ASYNC stops delivery -> 0 ok
[sio] re-open O_ASYNC -> 0 ok
[sio] close writer returns 0 -> 0 ok
[sio] close writer (EOF) sends SIGIO -> 1 ok
[sio] SIGPIPE pipe2 -> 0 ok
[sio] rt_sigaction(SIGPIPE, SA_SIGINFO) -> 0 ok
[sio] close SIGPIPE pipe read end -> 0 ok
[sio] write with no reader returns -EPIPE -> -32 ok
[sio] SIGPIPE handler ran -> 1 ok
[sio] SIGPIPE si_signo -> 13 ok
[sio] SIGPIPE si_code is SI_USER -> 0 ok
[sio] SIGPIPE si_pid is self -> 14 ok
```

`105` 是写进去的那个字节 `'i'`；`11` 是本任务号（每档可能不同，门禁用形态判）。三条
反假绿写死在这里：① handler 命中数与信号号一起判——只看「write 返回 1」会被「根本没投递」
骗过；② 清掉 `O_ASYNC` 那节看的是**命中数不变**，不是 write 返回值；③ `read` 必须拿回
那个字节，「投递了但把数据吃了」同样是错的。

**默认动作（不装 handler = 结束任务）只在宿主断言。** 在 QEMU 夹具里做会把本任务中途
杀掉，后面的读数全丢；宿主用例 `t_async_io` 第 7 节断言「返回值先写回、随后
`g_exit_code == 128 + 29`」，与 Linux 的默认动作一致。

**宿主的 72 条**（`test_linux_abi_host.c`：`t_async_io` 44 条七节 + U-31 的
`t_siginfo` 28 条五节）把 QEMU 夹具造不出来的
状态补齐：控制台对象的 `-EINVAL`/0、另一条管道只开 `O_ASYNC` 不设主人（扫描必须跳过
`owner == 0`）、写边 fd（`kind != PIPE_R`）不投递、`dup` 双向共享（5 条）、清 `O_ASYNC`
后不投递且 `reg` 没被动过、无主人不投递、EOF 也投一次、默认动作结束任务。**宿主用例的
一个坑**：投递之后 `s_sig_blocked` 会含该信号位，宿主不跑用户指令就等不到 `rt_sigreturn`
把它装回去——所以同一节里只能断言一次投递，要再断言必须重新 `fresh()`。

**门禁。** `tools/fork_gate.py` 的 `SIGIO_CHECKS = 38`：`[sio]` 前缀单独计数（与 `[fork]`
的十档常量互不干扰），十档都带 `-DARKRTOS_USE_LINUX_SIGIO=1`（否则「换档整段消失」会被
当成正常）。十条必现读数逐条校验（含带任务号的两条走形态正则），坏日志自检另加三种
`sio` 形态（少一条、插一条 FAIL、改一个数字）。

**覆盖率。** 宿主 `linuxabisigio` 档单跑 `test_linux_abi_host.c` 的 72 条断言、单档 93.11%；
八档（`linuxabi` / `linuxabifutex` / `linuxabifilemap` / `linuxabiselect` / `linuxabisocket` /
`linuxabisched` / `linuxabisigio` 与关档）求行并集后 `arkrtos_linux_abi` 97.34%（基线 97.00% 不动），
全量 `COVERAGE-GATE OK`（71 个模块均不低于基线）。

**边界。** ① 只报「fd 变可读（含 EOF）」，可写方向、边缘触发去重、`si_fd`/`si_band`、
实时排队一律不做（见上）；② 进程组登记回 `-EINVAL` 并计数，不假装支持；③ `dup` 的
共享只在**同一进程内**成立：fork 之后父子各自的槽是两份拷贝，`F_SETFL`/`F_SETOWN`
不会跨进程同步（要正确必须引入真正的 file 对象与跨进程引用计数，见
`docs/已知限制.md`）；④ 主人只认单个任务号，任务退出后 `F_GETOWN` 仍读回那个号
（不发呆信号也不清理，与「登记是数值」的语义一致）。

### 8.19 `siginfo_t` 与 `ucontext_t`（U-31）：`SA_SIGINFO` 三参派发（2026-11-27，QEMU LA64 38 条 + 宿主 72 条）

**是什么。** Linux 的信号处理函数有两种调用约定：`void (*)(int)` 与
`SA_SIGINFO` 的 `void (*)(int, siginfo_t *, void *)`。后者让处理函数能读到「谁发的、
为什么发」，以及被打断时的上下文（`ucontext_t`）。U-31 把这一档做进来：

| 项 | 现状 |
|---|---|
| 派发 | `sa_flags & SA_SIGINFO` 时 `a1 = fsp + 头长`（siginfo）、`a2 = a1 + 128`（ucontext）；没装则 `a1`/`a2` 原样不动 |
| 帧布局 | 头（`magic`/`signo`/`blocked`/帧长，4 字，LA64 32 B）→ `siginfo_t` 128 B → `ucontext_t` 432 B → trap 帧快照；头的字节数按 `sizeof(unsigned long)` 派生，32 位宿主上是 16 B |
| siginfo 字段 | `si_signo` / `si_errno`(恒 0) / `si_code`；`_kill` 族填 `si_pid` / `si_uid`(恒 0)；`_sigpoll`（SIGIO）填 `si_band` / `si_fd` |
| `si_code` 来源 | `kill` → `SI_USER`(0)；`tkill` / `tgkill` → `SI_TKILL`(-6)；SIGPIPE（内核代发）→ `SI_USER`；SIGIO → `POLL_IN`(1) |
| `si_pid` | 发送者任务号；线程档取组长号（Linux `task_tgid_vnr` 语义） |
| ucontext | `uc_sigmask`（handler 期间生效的屏蔽字）、`uc_mcontext.pc` / `ra` / `tp` / `sp`；`uc_stack.ss_flags = SS_DISABLE`（没有 altstack）、`uc_link = 0` |
| 不填的 | 浮点 / 向量现场、其余 GPR、altstack、`si_uid` 的真实 uid（本层没有 uid 概念，恒 0） |

**为什么不排队。** pending 每个信号只有一位，来源也只有一格（8 字节：`code` / `pid` /
`fd`，32 个信号共 256 B/进程）。同一信号连发两次，处理函数读到的是**最后一次**的来源。
这与「pending 位只表达有没有」是同一个取舍，写进已知限制；真排队要引入每信号一个链表，
把一个只有一位的机制变成内存分配点。

**SIGIO 走 `_sigpoll`。** Linux 的 `send_sigio()` 用 `SIGPOLL` 的 siginfo：
`si_code = POLL_IN`、`si_band = POLLIN|POLLRDNORM (0x41)`、`si_fd` 是那个 fd 号。本层
照做。一个管道对象只投一次：`dup` 出来的副本与原 fd 共享 `O_ASYNC` 与主人，扫到**最小
fd 号**就停（Linux 上 `dup` 共享同一个 `struct file`，fasync 也只有一份）。

**SIGPIPE 走 `SI_USER`。** Linux 的 `send_sig(SIGPIPE, current, 0)` 走 `SEND_SIG_NOINFO`：
`si_code = SI_USER`、`si_pid` = 自己的进程号、`si_uid` = 自己的 uid（本层 0）。

**ucontext 是只读视图。** handler 改了 `ucontext` 里的寄存器**不会**影响 `rt_sigreturn`
之后的现场：`rt_sigreturn` 只认帧里那份 trap 帧快照（改上下文是 `setcontext` 一类接口
才做的事，本层没有）。这一点写进已知限制，不做「改了也不生效」的静默。

**实测读数（`[sio]` 段新增 14 条，QEMU LA64）**

```
[sio] SIGIO siginfo pointer is set -> 1 ok
[sio] SIGIO si_signo -> 29 ok
[sio] SIGIO si_code is POLL_IN -> 1 ok
[sio] SIGIO si_band -> 65 ok
[sio] SIGIO si_fd -> 3 ok
[sio] SIGIO ucontext pc is non-zero -> 1 ok
[sio] SIGPIPE pipe2 -> 0 ok
[sio] rt_sigaction(SIGPIPE, SA_SIGINFO) -> 0 ok
[sio] close SIGPIPE pipe read end -> 0 ok
[sio] write with no reader returns -EPIPE -> -32 ok
[sio] SIGPIPE handler ran -> 1 ok
[sio] SIGPIPE si_signo -> 13 ok
[sio] SIGPIPE si_code is SI_USER -> 0 ok
[sio] SIGPIPE si_pid is self -> 14 ok
```

`65` 是 `POLLIN|POLLRDNORM`；`3` 是那个管道的读端号（`dup` 出来的副本号不报——最小
fd 号才是登记点）；`14` 是本任务号（每档可能不同，门禁用形态判）。

**宿主的 28 条**（`test_linux_abi_host.c` 的 `t_siginfo`，五节）把 QEMU 夹具造不出来的
现场补齐：① `kill` 自己 → `si_code = SI_USER`、`si_pid` = 自身任务号、`uc_sigmask` 含本
信号位、`mcontext.pc` / `regs[3]` 就是被打断的 pc / sp、`uc_stack.ss_flags =
SS_DISABLE`；② 没装 `SA_SIGINFO` 时 `a1` / `a2` 原样不动，但帧里的 siginfo 仍然铺好；
③ `tkill` → `si_code = SI_TKILL`；④ SIGPIPE 走 `SI_USER`；⑤ SIGIO 走 `_sigpoll`
（`si_code = 1`、`si_band = 0x41`、`si_fd` = 读端号）。

**边界。** ① 同一信号不排队、只留最后一次来源；② `si_uid` 恒 0（本层没有 uid）；
③ `ucontext` 只填 `uc_sigmask` 与 pc / ra / tp / sp，没有浮点 / 向量现场与 altstack，
也没有 `uc_link`；④ 改 `ucontext` 不生效（要改上下文得走别的接口）；⑤ 帧头的字节数
按字宽派生、缓冲按**字节**算——这一条是 U-31 踩出来的真 bug，见 §11 ⑪。

## 9 开关与零体积

- `ARKRTOS_USE_LINUX_ABI`（`kernelsrc/include/arkrtos_features.h`、
  `config/kconfig/Kconfig.features`），默认 **0**，且 `depends on ARKRTOS_USE_ELF`
  （没有装载器就没有兼容层，features 里有 `#error` 顶住）。
- 默认关时：`arkrtos_linux_abi.c` 整文件不产出任何符号，port 里没有
  `arkrtos_port_linux_syscall_num`，`arkrtos_hal.h` 只剩空宏。**体积与行为零变化**，
  这一条在门禁里是双向实测的（关档 `.o` 的符号表必须全空）。
- `ARKRTOS_USE_LINUX_THREAD`（§8.10），默认 **0**，`depends on ARKRTOS_USE_LINUX_FORK`
  （features.h 有一条 `#error` 顶住）。关档时 `arkrtos_elf_thread` /
  `arkrtos_port_paging_thread_frame` 两个函数与四个 `lx_thread_*` 符号全部不产出。
- `ARKRTOS_USE_LINUX_FORK`（§8），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI &&
  ARKRTOS_USE_EXIT_CODE` 且 `depends on ARKRTOS_USE_PAGING_PAGES &&
  ARKRTOS_USE_PAGING_DEMAND`；关档时 fork 的四个符号（`arkrtos_elf_fork` 与端口层
  三个搬运函数）一个都不产生。
- `ARKRTOS_USE_PAGING_COW`（§8.2），默认 **0**，`#error` 顶住两条前提
  （必须有 `ARKRTOS_USE_LINUX_FORK` 与 `ARKRTOS_USE_PAGING_DEMAND`）；关档时引用
  计数表与 `la_pg_cow_*` 全不编译，`LA_PTE_APP_RO` 也不产出。门禁的两档实测
  （关档零符号、开档 52 条读数）保证「默认档体积与行为零变化」不是一句声明。
- `ARKRTOS_USE_PAGING_SHM`（§8.3），默认 **0**，`depends on ARKRTOS_USE_PAGING_COW`
  且 features.h 用 `#error` 顶住这条依赖（共享位的账本就是 COW 档的引用计数表，
  非 COW 克隆会把每一对实体复制，共享自然消失）；关档时共享位表与
  `arkrtos_port_paging_map_shared` 全不产出、`mmap(MAP_SHARED)` 回 `-ENOSYS`。
  三档门禁实测（关档零符号、开档 60 条读数、SHM 关档的 `-ENOSYS` 对照）。
- `ARKRTOS_USE_LINUX_FUTEX`（§8.4），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` 且
  `features.h` 用 `#error` 顶住这条依赖；关档时不编译等待槽、键解析与 `lx_futex`，98 号落回
  `-ENOSYS`；开档实测是 fork+COW+SHM+futex 档的 147 条读数（其中 futex 87 条：基本配对 9 + 多时间片 7 +
  位集/超时/改挂 20 + PI 与 robust 37）与宿主 futex 档的 434 checks。
- `ARKRTOS_USE_PAGING_FILEMAP`（§8.5），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` +
  `ARKRTOS_USE_PAGING` + `ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND`（内容靠按需缺页
  拿帧，没有 DEMAND 就装不进映射区），`features.h` 用四条 `#error` 顶住；关档时文件映射分支、
  写回记账与 `msync` 全不编译，`mmap(fd)` 回 `-ENOSYS`；开档实测是第五档的 167 条读数（其中
  filemap 20 条）与宿主 filemap 档的 358 checks。
- `ARKRTOS_USE_PAGING_MPROTECT`（§8.6），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` +
  `ARKRTOS_USE_PAGING` + `ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND`
  （要按需缺页拿帧才能改一对的条目），`features.h` 用四条 `#error` 顶住；关档时台账
  `s_pg_page_ro`、`la_pg_page_ro` / `la_pg_mprotect_hit`、端口 `paging_protect` 与
  `mprotect_stat` 全不产出，`mprotect` 回到「只校验参数、返 0、单独计数」；开档实测是
  **第六档 178 条读数**（其中 mprotect 11 条）与宿主侧编译门禁。
- `ARKRTOS_USE_LINUX_SELECT`（§8.8），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` 且
  `features.h` 用 `#error` 顶住这条依赖；关档时不编译就绪判据、`fd_set` 扫描与
  `s_sel_armed/s_sel_deadline`，72 号落回 `-ENOSYS`；开档实测是**第七档 194 条读数**
  （其中 select 15 条）与宿主 select 档的 327 checks。
- `MAP_FIXED`（§8.7）**不是独立开关**：它在 `ARKRTOS_USE_PAGING_DEMAND=1` 时才有
  （要有按对装条目的能力，才谈得上「把这一段原来的东西丢掉」）；`DEMAND=0` 时
  `mmap(MAP_FIXED)` 如实回 `-ENOSYS`，夹具里那 6 条读数也整段不出现 —— 所以
  `example/qemu_loongarch64/Makefile` 必须把 DEMAND 镜像进 `LXFORK_DEFS`。
- `ARKRTOS_USE_LINUX_SOCKET`（§8.9），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` 且
  `features.h` 用 `#error` 顶住这条依赖；关档时不编译 `lx_sock_*` 全部实现与 fd 表的
  socket 分派，`198`–`210` 与 `242` 落回 `-ENOSYS`；开档实测是宿主 socket 档的
  **964 checks**（覆盖率单档 98.51%、六档 union 98.84%，基线 98.82%）与 **QEMU 第八档 242 条读数**
  （其中 socket 段 39 条：语义 38 + 往返耗时 1；U-30 的 10 条 dup-on-socket 读数另计）。
- `ARKRTOS_USE_LINUX_SIGIO`（§8.18），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI`
  且 `features.h` 用 `#error` 顶住这条依赖；关档时 `F_SETOWN` / `F_GETOWN` 落回
  `-EINVAL`（与 U-29 之前逐位相同）、`F_SETFL` 忽略 `O_ASYNC`、`owner` 槽与
  `lx_async_notify_pipe` 全不编译；开档实测是**十档共用的 `[sio]` 24 条读数**与宿主
  `linuxabisigio` 档的 44 条断言。
- `arkrtos_autoconf.h` 由 `tools/kconfig/genconfig.py` 从 `config/arkrtos_defconfig`
  生成，新增 Kconfig 项后必须重跑一次，否则默认档读数里看不到这个开关。

## 10 门禁：`tools/linux_abi_gate.py` 与 `tools/fork_gate.py`

```
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py        # 含 make
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py --no-build
```

14 项，全 PASS 时打印 `LINUX-ABI-GATE OK`，并由 `tools/ci_gate.py` 的
`linux-abi` 项调用：

| 项 | 钉住什么 |
|---|---|
| `default-off` | `arkrtos_autoconf.h` 里必须是 0 |
| `kconfig-wired` | Kconfig 项存在且 `depends on ARKRTOS_USE_ELF` |
| `features-guard` | 开 ABI 必须开 ELF 的 `#error` |
| `board-switch` | QEMU LA64 板级显式置 1 |
| `off-zero-symbols` | 关档 `.o` 符号表**全空**（不是「没测」） |
| `port-wiring` | 关档无 `arkrtos_port_linux_syscall_num` / 开档有（两向） |
| `compile-on` | LA64 真板级下 9 个 TU 全编过（含五个用户程序本身） |
| `syscall-table` | 30 个号与 `asm-generic` 逐条相等 |
| `abi-split-la64` | 立即数 == 0 是 Linux、!= 0 是 Ark |
| `abi-split-rv32` | `0x10..0x23` 归 Ark；`dup`/`dup3`（23/24）落在这一段里，所以另核「RV32 板级不开 ELF/ABI」——开 ABI 必须开 ELF 是 `features.h` 的 `#error`，歧义由构造不可达（不是靠推断） |
| `build` | `make -f example/qemu_loongarch64/Makefile` 通过 |
| `qemu-readout` | Linux 块 14 条 check 全 ok + PASS 行 + `exit code 7`；管道块 30 条 + `exit code 55`；信号块 31 条 + PASS 行 + `exit code 138`；dup 块 56 条（含 fcntl 20 条）+ `exit code 56`；rseq 块 16 条 + PASS 行 + `exit code 57`；旁路演示未受影响 |
| `parser-selfcheck` | 把 16 种坏日志喂回判据，必须全判红 |

socket 这一层是**两侧都有读数**的：宿主侧是 `tools/coverage_gate.py` 的第六个 Linux ABI 档
`linuxabisocket`（`-DARKRTOS_USE_LINUX_SOCKET=1 -DARKRTOS_USE_LWIP=1`，`-Wl,--wrap=arkrtos_port_svc_exec_frame`）
跑 `test_linux_abi_host.c`，992 checks 全绿、单档覆盖率 96.10%、八档 union 97.34%（基线 97.00%），
`tools/ci_gate.py` 的 `linux-abi-host` 项已带上这一档；板级侧是 `tools/fork_gate.py` 的
**第八档**（`-DARKRTOS_USE_LINUX_SELECT=1 -DARKRTOS_USE_LINUX_SOCKET=1 -DARKRTOS_USE_LWIP=1`），
QEMU LA64 上 242 条 check 全 `ok` + `exit code 0`。

fork 那一档单独一道 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项，**71 项**；fork / fork+COW / fork+COW+SHM / fork+COW+SHM+futex / fork+COW+SHM+futex+filemap / fork+COW+SHM+futex+filemap+mprotect / fork+COW+SHM+futex+filemap+mprotect+select / fork+COW+SHM+futex+filemap+mprotect+select+socket / fork+COW+SHM+futex+filemap+mprotect+select+socket+thread / fork+COW+SHM+futex+filemap+mprotect+select+socket+thread+appq **十档各跑一遍**，所以 build / qemu-readout / parser-selfcheck 各出现十次、compile-on 十次；再加十档共用的 `default-off` / `off-zero-symbols` / `off-zero-port` / `board-switch` / `restore-default` 五道，与九档各自的 `kconfig-wired-<档>` / `features-guard-<档>` / `default-off-<档>` 三件套二十七道，合计 71 项）：

上面这些 `[fork]` 常量都不含 `[sched]`（U-28，63 条）与 `[sio]`（U-29 + U-31，38 条）——
两个前缀各自单独计数，十档都跑同一份夹具，所以「换档时整段消失」不会被当成正常。

| 项 | 钉住什么 |
|---|---|
| `default-off` | `arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_FORK` 必须是 0 |
| `kconfig-wired` | 四条 `depends on` 都在（ABI / EXIT_CODE / PAGES / DEMAND） |
| `features-guard` | `features.h` 四道 `#error` |
| `off-zero-symbols` | 关档 RV32 编 `linux_abi.o` 零 `lx_clone`/`lx_wait4`/`s_child`；LA64 板级编 `elf`/`port`/`paging` 零 fork 符号 |
| `compile-on` | LA64 + 两个 flag，四 TU + 夹具全编过 |
| `build` | `make` 出 ELF |
| `qemu-readout` | `[fork]` **47 条** check 全 `ok`、父子 pid 不同、PASS 行、`exit code 0`，且 hello / FS / 其余 Linux 夹具读数仍在；三条新读数（阻塞 `wait4` 真的等了 >= 40 ms / 父读端在子关掉自己那份之后仍读到 3 字节 / SIGCHLD handler 真被调到）逐条校验；另有 `MAP_FIXED` 段 6 条（§8.7，丢弃后内容为 0、地址不变、非对齐与共享形态被拒）与阻塞管道段 20 条（§8.15，阻塞读等到写 / 阻塞写等到排空 / 跨进程内容逐字节比对）逐条校验 |
| `qemu-readout`（COW 档） | fork+COW：`[fork]` **52 条** check 全 `ok`；内核日志 `shared>=1 / stack>=1 / copied=0`、至少一条 `pg cow copy ... ref=2`、无 `ref=1` 的 copy 行；两条 COW 读数（子看到 fork 前的值、父的写只落自己那份）逐条校验 |
| `qemu-readout`（SHM 档） | fork+COW+SHM：`[fork]` **60 条** check 全 `ok`；六条 SHM 读数逐条校验（两条非法形态回 `-38`、映射成功、起始清零、父看到子写、子看到父 fork 之后的写）；内核日志另加至少一条 `pg shm map va=0x<n> pairs>=1` |
| `qemu-readout`（futex 档） | fork+COW+SHM+futex：`[fork]` **147 条** check 全 `ok`；八十七条 futex 读数逐条校验（九条基本配对 + 七条多时间片 + 二十条第二档 + 三十七条 PI 与 robust + 十四条 PI 条件变量：位集 0 回 `-22`、相对/绝对超时各一条 `-110`、50 ms 墙钟、`CMP_REQUEUE` 期望值不符回 `-11`、缺 `uaddr2` 回 `-22`、位集 `bits=7`、改挂后旧地址 `WAKE` 回 0 与 `bits=3` 等）；SHM 与 COW 的全部判据同时仍在 |
| `qemu-readout`（filemap 档） | fork+COW+SHM+futex+filemap：`[fork]` **167 条** check 全 `ok`；filemap 段 20 条读数逐条校验（私有只读内容三读数、页尾仍是文件内容、offset 非对齐回 `-22`、坏 fd 回 `-9`、只读 fd 上可写共享映射回 `-13`、offset=4096 生效且 EOF 后读零、O_RDWR 打开成功、可写共享映射 + `msync` + `munmap`、私有映射 `msync` 不落盘、重映射读回、`bits=31` 的五位证据）；SHM / COW / futex 的全部判据同时仍在 |
| `qemu-readout`（mprotect 档） | fork+COW+SHM+futex+filemap+mprotect：`[fork]` **178 条** check 全 `ok`；mprotect 段 11 条逐条校验（映射成功、`mprotect` 双向可设、只读页仍可读、同对另一半仍可写、撤保护后写得进、违规子进程被 reap 而非重启、状态字 = SIGSEGV、父页未被改）；串口另须有一行 `[LA64U] mprotect violation badv=0x… era=0x… tid=… -> kill task`；filemap / SHM / COW / futex 的全部判据同时仍在 |
| `qemu-readout`（select 档） | fork+COW+SHM+futex+filemap+mprotect+select：`[fork]` **194 条** check 全 `ok`；select 段 15 条逐条校验（空管道读端不就绪、非就绪时用户集被清位、50 ms 定时等待回 0 且墙钟 >= 40 ms、有数据后读端就绪且就绪位保留、有空间的写端与 stdout 可写、写满后写端不就绪、写端全关后按 EOF 就绪、`exceptfds` 永不上报且被清位、坏 fd 回 `-9`）；mprotect / filemap / SHM / COW / futex 的全部判据同时仍在 |
| `qemu-readout`（socket 档） | fork+COW+SHM+futex+filemap+mprotect+select+socket：`[fork]` **242 条** check 全 `ok`；socket 段 48 条（语义 38 + U-30 的 dup 10）逐条校验（`127.0.0.1` 真 lwIP 回环上的 bind/listen/accept/connect/send/recv、名字查询、`SOL_SOCKET` 四条 option、非阻塞 `-EAGAIN`，含一条往返耗时回归门）；select / mprotect / filemap / SHM / COW / futex 的全部判据同时仍在 |
| `qemu-readout`（thread 档） | fork+COW+SHM+futex+filemap+mprotect+select+socket+thread：`[fork]` **275 条** check 全 `ok`；线程段 33 条逐条校验（`clone(CLONE_THREAD)` 正负、两个 SETTID、`getpid`=tgid 且与夹具打出的父 pid 交叉相等、`gettid` 与父不同、`CLONE_SETTLS` 落 `$tp`、两栈不同址、父局部存活、`CHILD_CLEARTID` 清 0 + `FUTEX_WAIT` 被唤醒、四条参数门槛、线程后仍能 `fork`、线程发起与组长发起的 `exit_group` 各一条）；socket 及以下各档判据同时仍在 |
| `qemu-readout`（appq 档） | fork+…+thread+appq：`[fork]` **287 条** check 全 `ok`；配额段 12 条读数逐条校验（上限内 fork 与线程成、超限 fork 与超限线程回 `-11`、空间空闲时 fork 仍回 `-EAGAIN`、第三个线程回 `-11`、两个线程按 `CHILD_CLEARTID` + `FUTEX_WAIT` 归队、回收后立刻能再 fork、被阻塞的子进程被 `wait4` 真收走、退出码 3 照传），外加三行 `[appq]` 交叉证据（条数恰好 3、owner 同一个、上限读数 3）；thread 及以下各档判据同时仍在 |
| `parser-selfcheck` | 10 种坏日志必须判红（COW 档另加 4 种：位掩码丢一位、父写丢失、整类删光 copy 行、fork 时搬了字节；SHM 档再加 5 种：删光 `pg shm map` 行、共享写没被父看到、子看不到父的写、共享页起始非 0、缺映射成功读数；futex 档再加 11 种（第二档 8 种 + PI 与 robust 3 种）：WAIT 没被叫醒、唤醒者只看到一半（`bits=1`）、无等待者 WAKE 回 1、多时间片 WAIT 被打成 `-EFAULT`、多时间片没跨过时间片、位集被忽略（`bits=7` 变 3）、改挂没搬走等待者、超时没到期；filemap 档再加 3 种：写回没发生、写回把文件截短、私有映射的改动被落盘；mprotect 档再加 7 种：只读页读不回来、粒度退化成 8 KB、违规写没被拦住、违规写改了父页、状态字当成退出码、缺违规日志行、缺成功读数；select 档再加 8 种：空管道被报就绪、EOF 不报就绪、满管道仍报可写、`fd_set` 没清位、超时被重新计时、`exceptfds` 被上报、坏 fd 被放过、缺 select 读数；socket 档再加 6 种；线程档再加 7 种：CLEARTID 没清 0、join 的 WAIT 没被唤醒、`SETTLS` 没落 `$tp`、线程 `getpid` 不等于 tgid、两栈同址、线程发起的 `exit_group` 码被改成组长的、缺 `clone(CLONE_THREAD)` 读数；配额档再加 5 种：超限线程被放行、空间空闲时 fork 被放行、回收后起不来、子进程没被回收、缺 `[appq]` 日志。累计 fork 10 / cow 14 / shm 19 / futex 30 / filemap 34 / mprotect 41 / select 49 / socket 55 / thread 62 / appq 67 种） |
| `default-off-shm` / `kconfig-wired-shm` / `features-guard-shm` | SHM 开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_PAGING_COW`、features.h 有对应 `#error`；`off-zero-port` 另核关档没有 `arkrtos_port_paging_map_shared` |
| `default-off-futex` / `kconfig-wired-futex` / `features-guard-futex` | futex 开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_LINUX_ABI`、features.h 有对应 `#error` |
| `default-off-filemap` / `kconfig-wired-filemap` / `features-guard-filemap` | 文件映射开关的三件套：autoconf 为 0、Kconfig 四条 `depends on`（ABI / PAGING / PAGES / DEMAND）、features.h 四条 `#error` |
| `default-off-mprotect` / `kconfig-wired-mprotect` / `features-guard-mprotect` | mprotect 开关的三件套：autoconf 为 0、Kconfig 四条 `depends on`（ABI / PAGING / PAGES / DEMAND）、features.h 四条 `#error` |
| `default-off-select` / `kconfig-wired-select` / `features-guard-select` | select 开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_LINUX_ABI`、features.h 有对应 `#error`；`off-zero-port` 同档核关档没有 select 的新符号 |
| `default-off-socket` / `kconfig-wired-socket` / `features-guard-socket` | socket 开关的三件套：autoconf 为 0、Kconfig 两条 `depends on`（ABI / LWIP）、features.h 两条 `#error`；`off-zero-port` 同档核关档没有 socket 的新符号 |
| `default-off-thread` / `kconfig-wired-thread` / `features-guard-thread` | 线程开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_LINUX_FORK`、features.h 有对应 `#error`；`off-zero-symbols` / `off-zero-port` 同档核关档 `arkrtos_elf_thread` / `arkrtos_port_paging_thread_frame` 与四个 `lx_thread_*` 全不产出 |
| `restore-default` | 收尾把构建目录恢复默认档（避免下一次 `make` 复用 fork 档 `.o`） |

`parser-selfcheck` 是防「绿得没有信息量」的：Linux 块去掉 PASS 行、退出码改 3、
插一条 FAIL、少一条 check，管道块去掉 PASS 行、退出码改成 1，信号块去掉 PASS 行、
退出码改成 1，dup 块去掉 PASS 行、退出码改成 1、插一条 FAIL、少一条 check，
fork 块把「阻塞读数改成没等」/「父读端改成 `-EBADF`」/「SIGCHLD 没被
调到」，九种都必须被判红；U-29 起 `[sio]` 段另加三种（少一条、插一条 FAIL、改一个数字），
同样必须被判红。

## 11 本轮踩到的真 bug 与读数坑

**① 用户指针全部 `-EFAULT`：ELF 任务的窗口没写回。**
现象是程序几乎不打印，`exec rc=0` 之后没有正文。根因：`arkrtos_elf.c` 里把
`p_task->rom_start/rom_size/ram_start/ram_size` 的写回包在 `#if
(ARKRTOS_USE_MPU == 1)` 里，而 QEMU LA64 是 MPU=0 / PAGING=1 ⇒ 四个字段是 0，
`copy_from_user` 判定「不在任何窗口内」返 `-2`。修法：窗口写回**无条件做**，只把
`arkrtos_mpu_build_task()` 留在 MPU 档内。副产品解释了一个怪现象：写栈成功、读
rodata 失败，因为栈窗口（32 KB）被单独设过，而代码窗口没有。

**② `openat` 用卷内路径查 VFS 命名空间。**
`/mnt/ram/linux_hello` 是命名空间路径，而 `arkrtos_fs_stat_path()` 要的是卷内路径
⇒ 恒 `-ENOENT`。改成 `arkrtos_vfs_app_stat()` / `arkrtos_vfs_read_at()`。

**③ 信号帧地址按用户 sp 算，盖掉了内核自己的返回地址**（`INE` / `ecode=0x0d`，
一条 check 都没打出）。根因与修法见 §7：「为什么帧址要由端口算」一段。
这条的价值在于它只在「SVC 处理链跑在用户栈下面」这种 port 上才出现，
编译期与静态检查都看不出来，只能靠上板读数定位。

**④ 读数坑：半行被抢占，看起来像「少了一条读数」。**
加锁前实测到 `[linux] write ... rc=0` 之后直接是 `[l`，而剩下的
`inux] exec rc=0` 出现在**程序跑完之后**——不是丢字节，是**打印被任务切换打断**：
`exec` 之后用户任务一开跑就抢走 CPU，演示任务的一行被打成两截。修法是在
`elf_demo.c` 里把整行打印放进调度器锁（`arkrtos_sched_lock_internal()` /
`arkrtos_sched_unlock_internal()`），改完 3/3 连跑稳定。判据侧另有两处：
读到 marker 前缀就 kill 会把 `exit code 7` 截掉（加 0.5 s 宽限）；`-serial file:`
后端在进程被 kill 时丢不满 4 KB 的尾块（改管道）。加 SHM 档时同一个坑换了张脸
再来一次：用户 `write(1/2)` 逐字节直写 `board_putc` 且不拿控制台锁，子进程的
`[LA64U] pg cow copy ...` 能插在 `[fork] ... -> 1` 与 ` ok` 之间，门禁报「少了一条
读数」（33 != 34），实际上一条都没少。修法是 `lx_write` 整次调用持
`arkrtos_console_lock()`——内核日志拿的是同一把锁，二者不再互相切行（§8.3）。
再往下一层还有一次同型翻车：锁只管一次 `write`，夹具把一行拆成三次 `write` 时，
**三次调用之间仍会让位**（§8.4 坑 ③）；现在带数字的行都先拼成整行再写。

**⑤ 读数坑续：把「夹具赌调度」读成「实现坏了」。** 同一档里还有两处顺序问题，
它们都不改实现就消失：SHM 段原来用纯用户自旋等 4M 次，阻塞 `wait4` 段原来让子
进程一 clone 就开始睡 60 ms。前者在某次全量门禁里读出 `bits=0`（像共享失效），
后者读出 `waited 2 ms`（像 wait4 没阻塞），单跑又都正常 —— 典型的「判据里藏着
调度假设」。两处都改成「管道放行 + 轮询」定序（§8.3），3/3 与 5/5 连跑稳定。门禁同时补了一条
可诊断性：QEMU 读数不过时把整段串口写到 `%TEMP%\ark_fork_<tier>.log` 并在失败
行里给出行数与路径 —— 否则「缺某一行」分不出是夹具没打、还是串口只收到一半。

**⑥ `la_pg_install_pair` 躺在 COW 段里：只开 DEMAND 的老档从未被链接过。**
加 `MAP_FIXED` 时 `arkrtos_port_paging_discard()` 无条件调 `la_pg_install_pair()`，
而那个函数定义在 `#if (ARKRTOS_USE_PAGING_COW == 1)` 之内 ⇒ `[fork]` 第一档
（`LINUX_FORK=1 + DEMAND=1`、COW 未开）链接直接
`undefined reference to 'la_pg_install_pair'`。**这不是新代码的错**：组合
「DEMAND 开、COW 关」在门禁里从来没被链接过一次（`compile-on` 只做 `-c`，
不带链接）。修法是把函数搬到 COW 段之外、`la_pg_pair_pte()` 之后，并写清依赖
（`la_tlb_write` / `la_tlb_inval_page` / `LA_STLB_INDEX` / `LA_PG_WIN_WAY_OF` /
`la_asid_swap` 全无条件）。教训：**`-c` 通过不等于这个组合存在**，开关组合的
可链接性只能靠真链接（或真跑）来证。

**⑦ 夹具编译不带 `EXTRA_CFLAGS`：开关漏镜像，表现是「读数整段不出现」。**
`example/qemu_loongarch64/Makefile` 的 `$(LXFORK_ELF)` 只用
`$(ARCH_FLAGS) $(LXFORK_DEFS)` 编译，`CFLAGS` / `EXTRA_CFLAGS` /
`-include arkrtos_board_config.h` 一概不参与。所以夹具里能看到的内核开关**只有
`LXFORK_DEFS` 里手工镜像的那几条**。`MAP_FIXED` 段只在
`ARKRTOS_USE_PAGING_DEMAND=1` 时编，而 DEMAND 从未镜像 ⇒ 六档齐声报
「check 通过条数 26 != 32」（以及 34/40、43/49、63/69、74/80），
`[fork] mmap for MAP_FIXED > 0` 一行都看不到，而**代码完全正确**
（`compile-on` 六档全 PASS，因为 gate 自己编夹具时带着 FORK_FLAGS）。
修法是 Makefile 加 `ARK_DEMAND_ON := $(findstring ...)` 与 `ifneq` →
`LXFORK_DEFS += -DARKRTOS_USE_PAGING_DEMAND=1`。教训：**「缺读数」要先分
「没测到」与「没编进去」**——两者在日志上长得很像，但一个改夹具、一个改 Makefile。

**⑧ 覆盖率门禁被一条可证明不可达的守卫拉红。**
`MAP_FIXED` 分支最初在算完 `end = align_up(want + len)` 之后又判了一次
`end == 0 || end > limit`。但 `want` 与 `limit` 都页对齐、上一句已保证
`len <= limit - want`，这条判据**永远走不到** ⇒ 两个分支都测不到，四档并集从
97.96% 掉到 97.91%，`coverage` 项判红。这不是「造一个用例把它盖住」的场合：
硬造一个进不去的入口等于造假入口。正确做法是删掉那条冗余判断、用注释把
不变量（两个端点都页对齐）写死。删完并集回到 98.18%，`COVERAGE-GATE OK`。

**⑨ futex 重放用被清成 0 的 `uaddr` 去读字：正常的长等待被打成 `-EFAULT`。**
LA64 上系统调用返回值写回 `a0`，而 `futex(WAIT)` 的第一个参数（`uaddr`）也从 `a0` 读；阻塞返回 0
时重放的那一次读到的 `uaddr` 就是 0。最早的实现认出重放后仍拿这个 0 去 `copy_from_user`，必然
`-EFAULT` —— 也就是说**一次超过 100 ms 没被唤醒的 WAIT 会以错误码终止**，而这是 futex 最正常的
用法。更糟的是宿主用例把 `-EFAULT` 写成了期望值，等于测试在替缺陷做证。修法是每槽增加
`s_futex_uaddr` / `s_futex_val`，登记时存下真正的地址与期望值，重放时取回再用；宿主用例改成
「继续睡 / 值变了当被叫醒 / 登记地址真读不出来才 `-EFAULT`」三条，QEMU 侧另加一组「子进程先忙等
250 ms 跨过两个时间片再写值 + `WAKE`」的读数（含墙钟 >= 150 ms 与 `bits=3`）。教训两条：**返回
值是第一个参数寄存器时，「重放」必须靠外部记账而不是靠参数**；以及**别把「修复前能跑通的错误码」
当成期望值**——它的形态是前后都跑得动、都返回一个像样的 `errno`。

**⑩ 两个参数的语义随 op 改变：`timeout` 那一格在 `REQUEUE` 里是 `nr_requeue`。**
Linux 的 futex 号表把「同一格参数」在不同 op 下复用：`REQUEUE`/`CMP_REQUEUE` 的第 4 个参数是
`nr_requeue`（不是超时指针）、第 5 个是 `uaddr2`、第 6 个 `val3`；`WAIT` 的 `timeout` 是**相对**
时长，而 `WAIT_BITSET` 的 `timeout` 是**绝对**时刻。这三处只要有任意一处按「WAIT 那一套」想当然
地写，症状都是静默的：`REQUEUE` 会把一个指针当计数（或把计数当指针读 ⇒ `-EFAULT`），
`WAIT_BITSET` 会把绝对时刻当相对时长、于是**每次调用都重新睡满一个时长**，永远等不到期。
写法上因此把 timeout 解析抽成 `lx_futex_timeout(p_ctx, abs_flag, &dl)`、由调用方说明「这一档是
绝对还是相对」，并把结果存成**截止时刻**而不是时长——后者在时间片重放模型里根本无法表达「已经
等了多久」。教训：**移植 op 号表时，先按参数位置画一张表，再写代码**；同一个位置在不同 op 下的
含义不同，是 Linux 一类老接口的常态。

**⑪ 信号帧缓冲按「字」算，32 位宿主上 `copy_to_user` 读越界。**
U-31 给帧加了 `siginfo` + `ucontext` 两块固定区（560 B）之后，`s_sigframe` 仍按
`unsigned long[...]` 声明，而帧长 `total` 是**按字节**算的（592 + 快照）。LA64 上
`sizeof(unsigned long) == 8`，两者差 8 倍也够用，所以 QEMU 全绿；32 位宿主上
`sizeof(unsigned long) == 4`，缓冲只有字节数的一半 ⇒ `copy_to_user` 从缓冲后面读
出去，行为未定义（这一轮宿主用例抓到的是「帧长 / payload 偏移」两条断言先红）。
修法：缓冲改用 `LX_SIGFRAME_BUF_BYTES`（头 + 128 + 432 + 快照，全按字节）向上取整
到字；头长用 `sizeof(unsigned long)` 派生；固定区按**字节**清零、`export` / `import`
都从 `(uint8 *)s_sigframe + 固定区偏移` 起算。教训：**只要有一处按字节、一处按字，
就必须把「字」彻底换成「字节」**——混用在一个 ABI 宽度上能跑，在另一个上直接越界，
而越界读在宿主上通常表现为「读数莫名其妙」，不是段错误。

## 12 边界

- 这一层的定位是「**能跑通静态程序的最小系统调用集合**」，不是 Linux 兼容层
  的完整实现。没有 ptrace、没有 `/proc`（Ark 自己的 procfs 另算）。ELF interpreter
  另有一档：`ARKRTOS_USE_DYNLOAD` 打开后走自研 `ld-ark.so`（见
  [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但那是「能跑自研夹具」，不是
  「能跑 glibc 产物」。
- fork 这一档的门禁是独立的 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项）：默认档
  autoconf=0 + 四道 `#error` + Kconfig 两条 `depends on`；关档 RV32 编 `linux_abi.o`
  零 `lx_clone`/`lx_wait4`/`s_child`、LA64 编 `elf`/`port`/`paging` 零 fork 符号；
  开档四 TU + 夹具编过；QEMU 上 `[fork]` 47 条 check 全 `ok`、父子 pid 必须不同、
  `exit code 0`，且同一次启动里 hello / FS / 其余 Linux 夹具的读数仍在；其中
  阻塞 `wait4`、fd 继承与管道引用计数、跨任务 SIGCHLD 三条读数逐条校验；阻塞管道段
  20 条（§8.15）逐条校验（含「阻塞读等了 >= 40 ms」「阻塞写写全 300」「跨进程字节图案
  全对」三条必现锚点，防止「少写一条、多写一条」凑出假绿）；
  解析器另有九种坏日志自检。fork+COW 档（`ARKRTOS_USE_PAGING_COW=1`）另跑一遍：
  52 条 check + 四条内核日志断言（共享对数、栈页私有化对数、`copied=0`、真发生过
  写时复制），坏日志自检再加四种 COW 专属形态；fork+COW+SHM 档（`ARKRTOS_USE_PAGING_SHM=1`）
  再跑一遍：60 条 check + 共享映射内核日志断言（`pg shm map ... pairs>=1`），坏日志
  自检再加五种 SHM 专属形态；futex 档（`ARKRTOS_USE_LINUX_FUTEX=1`）跑 147 条（futex 段
  87 条：基本配对 9 + 多时间片 7 + 位集/超时/改挂 20 + PI 三档与 robust list 37 + PI 条件变量 14），坏日志
  自检再加八种 futex 专属形态（含「位集被忽略」「改挂没搬走等待者」「超时没到期」三条第二档的）；
  PI 档的判据不是「返回 0 就算过」，而是「持有者带锁死亡后接手者拿到的是父进程 tid | OWNER_DIED」，
  退化成叫醒再抢时这一条一定是红的。
- Real-time 语义不因兼容层改变：Linux ABI 的路径全部在 SVC 里同步完成，
  不额外开线程、不加锁（`read`/`write` 到 ramfs 或管道都是同步搬运）。会「睡下
  再回来」的只有三处：阻塞 `wait4`（§8.1）、阻塞管道 `read`/`write`（U-26，§6 /
  §8.15）、以及 futex 等待（§8.4），它们都走可重启系统调用：睡下时把任务从就绪集
  摘掉、把 `ERA` 退回 `syscall`，唤醒后重放；不是关中断自旋。管道等待的粒度是
  调度器时间片（`LX_WAIT_SLICE_MS = 100`），也就是「最多睡 100 ms 再看一次」，
  不是 Linux 那种精确的 waitqueue 唤醒——这一点在 §6 的边界里写明了。
- 下一步（若要用真正的 Linux 工具链产物）：动态链接的内核侧与自研装载器已落地
  （2026-11-20，见 [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但**离 glibc
  还差整条 libc 兼容面**——TLS 初始化
  （调度策略 `sched_*` 十二个号与内核 deadline 类已于 2026-11-27 落地，见 §8.17）
  （条件变量的 PI 变体 `WAIT_REQUEUE_PI` / `CMP_REQUEUE_PI` 已于 2026-11-27 落地，见 §8.4c；
  `rseq`/`getcpu` 也于同日落地，见 §8.14）。
  futex 的 PI 三档与 robust list 清理已经落地（§8.4b）。
  信号已有跨任务投递（§7），「进程」这个对象也开了（`fork`/`clone` + `wait4`，§8，
  默认关）：每进程 fd 表与继承、跨任务 `SIGCHLD`、阻塞 `wait4` 都已在 QEMU LA64 上
  有读数；写时复制也有了（`ARKRTOS_USE_PAGING_COW`，§8.2，默认关）、匿名
  `MAP_SHARED` 共享页也有了（`ARKRTOS_USE_PAGING_SHM`，§8.3，默认关）、futex 九档 op
  （WAIT/WAKE/REQUEUE/CMP_REQUEUE/BITSET + PI 三档与 robust list + 超时）也有了
  （`ARKRTOS_USE_LINUX_FUTEX`，§8.4 / §8.4b，默认关）、文件映射也有了
  （`ARKRTOS_USE_PAGING_FILEMAP`，§8.5，默认关）、`mprotect` 页级写保护真落实了
  （`ARKRTOS_USE_PAGING_MPROTECT`，§8.6，默认关）、真 `mmap` 语义 `MAP_FIXED` 也有了
  （按需分页档，§8.7，不需另开开关）；线程档也有（`CLONE_THREAD`，§8.10，默认关；
  关档时 `CLONE_VM`/`CLONE_THREAD` 仍拒）、每 App 任务配额也有（§8.11，默认关）；
  仍没有抢占式信号投递；`fcntl` 的 fd 旗标与状态旗标已有（§8.13，2026-11-27），
  记录锁已有（§8.16，2026-11-27；8 格表、按路径认文件、无死锁检测），调度策略已有
  （U-28，§8.17，2026-11-27；十二个 `sched_*` 号 + 内核 deadline 类），异步 I/O 仍没有；
  `rseq`/`getcpu` 已有（§8.14，2026-11-27）；
  管道默认阻塞（U-26，§6 / §8.15，`O_NONBLOCK` 档并存），跨执行流的阻塞读写
  已在 fork 档实测。
- `execve` 是「同任务换镜像」；`fork` 才是「多一个任务 + 一份独立地址空间」。
  两者都在，但**不要**因此把这一层读成「有完整的进程模型」：调度与优先级仍是
  内核全局单份，`fd` 表在 fork 档才分进程（默认档全局单份），写时复制只在
  `ARKRTOS_USE_PAGING_COW` 档（且栈页强制私有），线程只在
  `ARKRTOS_USE_LINUX_THREAD` 档（§8.10，默认关），没有作业控制。