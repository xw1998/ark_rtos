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

## 1 支持的系统调用（29 个；28 个有实跑读数，1 个只有号表与参数校验）

编号取 `asm-generic`（LoongArch / RISC-V / ARM64 共用这一套）：

| 号 | 名字 | Ark 侧兑现方式 | 实测读数 |
|---:|---|---|---|
| 56 | `openat` | `arkrtos_vfs_app_stat()` 查命名空间；目录返 `-EISDIR` | `openat>0 -> 1 ok` |
| 57 | `close` | 释放 fd 槽，返 0；管道端会减引用计数 | `close -> 0 ok` |
| 59 | `pipe2` | 环形缓冲管道 + 两端引用计数；**非阻塞**（见 §6） | `pipe2 rc = 0` / `read empty -> -EAGAIN = -11` / `write with reader closed -> -EPIPE = -32` / `exit code 55` |
| 63 | `read` | `arkrtos_vfs_read_at()` 按 fd 的偏移读 | `read 4 bytes -> 4 ok`（读到 ELF magic） |
| 64 | `write` | 逐 128 字节 `arkrtos_board_putc` | `hello from the Linux syscall ABI` 正文 |
| 67 | `pread64` | 按 `pos` 直读、**不改 fd 读游标**（`ld.so` 按段表随机访问同一个文件要用） | 无夹具读数（§1.1） |
| 93 | `exit` | `arkrtos_thread_exit_code_internal()` | `exit code 7` |
| 94 | `exit_group` | 同上（单线程程序等价） | 同上 |
| 96 | `set_tid_address` | 记住地址、返本任务 id（不参与 futex；futex 见 §8.4） | `getpid==task id>0 -> 1 ok` 同路径 |
| 98 | `futex` | 只 `WAIT`/`WAKE`，键 = 物理帧 + 页内偏移（`ARKRTOS_USE_LINUX_FUTEX`，§8.4） | `futex(WAKE) with no waiter == 0 -> 0 ok` / `futex(WAIT) on a different value == -EAGAIN -> -11 ok` / `futex(WAIT) returned 0 after being woken -> 0 ok` / `futex waker saw 1 and woke 1 (bits=3) -> 3 ok` |
| 113 | `clock_gettime` | `MONOTONIC` 返内核节拍；`REALTIME` 返 `-ENOSYS` | `MONOTONIC -> 0 ok` / `REALTIME -> -38 ok` |
| 129 | `kill` | 只能投给自己（`pid != 本任务` → `-ESRCH`），置 pending（§7） | `kill rc survives handler -> 0 ok` / `kill other pid -> -ESRCH = -3 ok` |
| 130 | `tkill` | 同上（单线程下与 `kill` 同实体） | 同 `kill` 路径 |
| 131 | `tgkill` | 要求 `tgid == tid`，否则 `-ESRCH` | `tgkill tgid!=tid -> -ESRCH = -3 ok` |
| 134 | `rt_sigaction` | 装/取动作；`sigsetsize` 必须 8；`SIGKILL`/`SIGSTOP` 与非 0 缺失的 restorer 一律 `-EINVAL`（§7） | `rt_sigaction rc = 0 ok` / `old handler was SIG_DFL = 0 ok` / `no restorer -> -EINVAL = -22 ok` |
| 135 | `rt_sigprocmask` | `BLOCK`/`UNBLOCK`/`SETMASK`；`KILL`/`STOP` 永不可屏蔽（§7） | `query sees blocked bit = 1 ok` / `delivered on unblock = 2 ok` |
| 139 | `rt_sigreturn` | 校验帧 `magic` 与帧长后整帧装回；特权级不认用户帧（§7） | 由 restorer 触发，处理函数返回后读数仍正确 |
| 160 | `uname` | `sysname=ArkRTOS`、`machine=loongarch64` | `uname -> 0 ok` / `sysname=ArkRTOS machine=loongarch64` |
| 172 | `getpid` | 返内任务 id | `getpid==task id>0 -> 1 ok` |
| 174 | `getuid` | 恒 0 | 走同一返回路径 |
| 175 | `geteuid` | 恒 0 | 走同一返回路径 |
| 214 | `brk` | 窗口内单调分配（见 §4） | `brk(0)>0` / `brk(+4096) -> 34607104 ok` |
| 215 | `munmap` | 只返 0，**不归还地址** | 走同一返回路径 |
| 220 | `clone` | 只做 fork：`flags` 必须 0 或 `SIGCHLD`、`child_stack` 必须 0；复制整份地址空间 + 造新任务，子返 0 父返子号（§8，`ARKRTOS_USE_LINUX_FORK`） | `clone(child_stack!=0) == -EINVAL -> -22 ok` / `parent child=12` / `child pid != parent pid -> 1 ok` / `exit code 0` |
| 221 | `execve` | 同任务换镜像：铺段进调用者自己的空间、把入口/`sp`/`tp` 写回 trap 帧；成功不返回 | `stage2 pid==stage1 pid -> 11 ok` / `image reloaded (.data marker) -> 1 ok` / `fd 3 survived -> 1 ok` / `heap cursor reset -> 34668544 ok` / `exit code 42`（§5） |
| 222 | `mmap` | 匿名私有 / 匿名共享（后者要 `ARKRTOS_USE_PAGING_SHM`，§8.3）；**`MAP_FIXED` 已有**（按需分页档，只认匿名私有，§8.7）；同时给 `PRIVATE|SHARED` 回 `-ENOSYS` | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` / `MAP_FIXED discarded the old bytes -> 1 ok`（§8.7，6 条） |
| 260 | `wait4` | 只认 `options 0/WNOHANG`；**无 `WNOHANG` 时真阻塞**（可重启系统调用，§8.1），不编造状态，reap 后记账清零（§8） | `wait4 returned child pid -> 12 ok` / `WEXITSTATUS == 42 -> 42 ok` / `wait4(child) again == -ECHILD -> -10 ok` / `blocking wait4 blocked >= 40 ms -> 1 ok` |
| 226 | `mprotect` | **有页级写保护**（`ARKRTOS_USE_PAGING_MPROTECT`，§8.6）：4 KB 粒度改 `WE` 位、违规写按 SIGSEGV 杀任务；`PROT_EXEC` / `PROT_NONE` 只记账不强制；关档回「只校验参数、返 0」 | `mprotect RO == 0 -> 0 ok` / `sibling 4 KB half of the same pair stays writable -> 13107 ok` / `mprotect violation reports SIGSEGV status -> 11 ok`（§8.6，11 条） |
| 227 | `msync` | 只写回本进程记账里的「可写共享文件映射」；窗口内未命中回 0、窗口外 `-ENOMEM`（`ARKRTOS_USE_PAGING_FILEMAP`，§8.5） | `filemap msync rc -> 0 ok` / `filemap private msync rc (no writeback) -> 0 ok` / `filemap shared write persisted, private write not (bits=31) -> 31 ok` |

未知号一律 `-ENOSYS`（38），并计数；**不猜**。

### 1.1 只有号表、没有夹具读数的那个（诚实标注）

`pread64`(67) 是给 `ld.so` 铺段用的，本轮**没有实跑夹具**：只有「号表与
`asm-generic` 相等 + LA64 板级编译通过」。

`mprotect`(226) 原先也在这张名单里（只校验参数、返 0、**不改任何页权限**）。批C-2
把它改成了真改硬件条目（§8.6），于是它有了 11 条串口读数，不再是「只有号表」的号。

表格里其余 28 个号都有 `[linux]` / `[pipe]` / `[sig]` / `[fork]` 夹具的串口读数。

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
| `fork` / `clone` | **已有**（`ARKRTOS_USE_LINUX_FORK`，默认关；2026-11-23，QEMU LA64 实跑） | 只做 fork，不做线程：`CLONE_VM` / `CLONE_THREAD` 一律 `-EINVAL`；见 §8 |
| 信号 | **已有跨任务投递**（`kill`/`tkill`/`tgkill` 按 pid 定位目标进程，2026-11-23，QEMU LA64 实跑）；仍无抢占式投递、无实时信号、无 `siginfo`（§7） | 跨任务靠「置目标 `sig_pending` + 目标卡在阻塞 `wait4` 时唤醒」，铺帧仍在目标自己下一次系统调用返回时做；抢占式入口还没有 |
| `fd` 继承 | **已有**（fork 档每进程 fd 表 + 管道对象引用计数，2026-11-23） | 见 §4 与 §8；`dup`/`dup2` 重定向仍无 |
| `socket` / `select` | 无（Ark 自己有一套 socket 兼容层，但**不在** Linux ABI 号表里） | 两套编号与阻塞语义不同，混在一个号表里只会给出错答案 |
| `futex` | **已有 WAIT/WAKE 最小一档**（`ARKRTOS_USE_LINUX_FUTEX`，默认关；2026-11-23，QEMU LA64 实跑） | 只做「值没变就睡、变了就叫」；键 = 物理帧 + 页内偏移；没有 `REQUEUE`/`PI`/`WAIT_BITSET`、没有 robust list 清理、没有超时（不编 `-ETIMEDOUT`）；见 §8.4 |
| 真 `mmap` 语义（文件映射/`MAP_FIXED`/权限位） | **文件映射已有**（`ARKRTOS_USE_PAGING_FILEMAP`，§8.5）、**`MAP_FIXED` 已有**（按需分页档，§8.7，只认匿名私有）、**页级写保护已有**（`ARKRTOS_USE_PAGING_MPROTECT`，§8.6）；匿名共享 `MAP_SHARED` 只随 `fork` 继承、无 `shm_open`（§8.3）；`PROT_EXEC` / `PROT_NONE` 仍只记账不强制 | 窗口式分配器（§4）；LA64 这条通路没有 NR/NX，做不出硬件不可执行 |
| 多线程 | 无 | `clone` 没有；`exit_group` 与 `exit` 等价 |

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

三处必须写在标题上的不一致：

1. `envp` 不消费：这台机器没有环境变量这一层，新镜像拿到同一张空表；
2. **fd 表保留**（与 Linux 一致，但与 shell 的 `exec` 不同——后者起新任务、会 reset）；
3. 旧镜像的堆游标作废，已分配的地址不做任何回收声明（旧镜像已经没了）。

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

## 6 pipe2（59）：非阻塞管道（2026-11-21，QEMU LA64 实跑）

程序侧看到的就是 Linux 的 `int pipe2(int fds[2], int flags)`：分配两个 fd（读端在先）、
把号写回用户给的 `int[2]`。Ark 的实现是「两个 fd 各记一个引用 + 一条环形缓冲」：

- `lx_pipe_t` 静态表 `s_pipe[2]`（`LX_PIPE_MAX 2`），每条缓冲 256 字节（`LX_PIPE_BUF`），
  `readers` / `writers` 两个引用计数；
- `lx_fd_t` 加 `kind` 字段（普通文件 / 读端 / 写端）与 `pipe` 下标，`read` / `write` /
  `close` 全部分派到位；`execve` 换镜像时 fd 表照旧保留 ⇒ **管道跨 `execve` 存活**
  （与 Linux 一致）；
- 读写都是内核内同步搬运环形缓冲，数据不经 ramfs，也不开线程。

### 语义（与原版 Linux 的差别写在明面上）

| 事件 | Ark | Linux | 为什么不一样 |
|---|---|---|---|
| 读空缓冲、写端还在 | `-EAGAIN`（11） | 阻塞 | 管道读写仍是内核内同步搬运、刻意不做阻塞；fork 档下两端可以真分属父子（fd 已继承），但「读空就睡」这类等待没有接 |
| 读空缓冲、写端全关 | 返回 `0`（EOF） | 返回 0 | 与 Linux 一致；**绝不用 0 冒充「暂时没数据」** |
| 写满缓冲 | 短写（返回实际字节数） | 阻塞到能写 | 同上，不阻塞；短写本来就是 Linux 允许的返回形状，程序按返回值循环即可 |
| 缓冲全满、一个字节都写不进 | `-EAGAIN` | 阻塞 | 同上 |
| 读端全关还写 | `-EPIPE`（32） | `SIGPIPE` / `-EPIPE` | 没有信号，只能返回 `-EPIPE`；`SIGPIPE` 不会来 |
| `flags` 带 `O_CLOEXEC` | `-EINVAL` | 接受 | 本内核没有 `exec` 时的关闭语义，接受该位等于给一个不成立的承诺 |
| `flags` 带 `O_NONBLOCK` | 接受（本实现本来就非阻塞） | 置非阻塞位 | 语义相同 |

上限：`LX_MAX_FD 8` 个 fd 槽 ⇒ 最多两条管道（4 槽）再加 4 个普通文件；管道或 fd 槽
不够时 `-ENFILE`。用户地址写不进 `fds[2]`（`copy_to_user` 失败）时整条管道回滚并返
`-EFAULT`——不留半条泄漏的管道。

### 实测读数（`[pipe]` 行，QEMU LA64）

```
[pipe] write /mnt/ram/linux_pipe rc=0 size=7200
[pipe] pipe2 rc = 0 ok
[pipe] fds = 3 4
[pipe] fds are two increasing numbers = 1 ok
[pipe] write 5 bytes = 5 ok
[pipe] read 5 bytes = 5 ok
[pipe] content matches = 1 ok
[pipe] read empty -> -EAGAIN = -11 ok
[pipe] close write end = 0 ok
[pipe] read after writer closed -> EOF = 0 ok
[pipe] pipe2(O_CLOEXEC) -> -EINVAL = -22 ok
[pipe] second pipe2 rc = 0 ok
[pipe] write 400 into 256 buffer -> short write = 256 ok
[pipe] read back what fits = 256 ok
[pipe] write again after drain = 256 ok
[pipe] write with reader closed -> -EPIPE = -32 ok
[pipe] PASS: pipe2 two fds, data round trip, EAGAIN on empty, EOF on writer close, EINVAL on O_CLOEXEC, short write and EPIPE
[pipe] exec rc=0
[pipe] finished after 0 ms, exit code 55
```

14 条 `check()`（夹具 `example/qemu_loongarch64/elf/linux_pipe.c`，纯 Linux ABI 静态
程序，自带 `_start`）覆盖：两个 fd 都合法且递增、数据往返逐字节、空读 `-EAGAIN`、
关写端后读回 EOF、`O_CLOEXEC` 被拒、第二条管道能开、400 字节写进 256 缓冲得到 256
短写、排空后还能再写、读端关掉后写得到 `-EPIPE`。全过才打 PASS 行、`exit code 55`。

**反假绿（实测）**：对内核做两处变异——① 空读改成 `return 0`；② 去掉写侧的读端检查。
重编再跑，读数立刻变成 `read empty -> -EAGAIN = 0 FAIL` 与
`write with reader closed -> -EPIPE = -11 FAIL`，`exit code 1`；还原后复验 14/14 ok、
`exit code 55`。门禁的 `parser-selfcheck` 另有两条管道专用坏样本（去掉管道 PASS 行、
把退出码改成 1），两者都必须被判红。

### 边界

- **非阻塞语义与 Linux 不同**：任何「写满/读空就自旋等」的程序在这套管道上会拿到
  `-EAGAIN` 而死循环。这是有意选择，不是没做完——理由见上表。
- 缓冲 256 字节、`LX_PIPE_MAX 2` 条、fd 槽 8 个。
- **fd 继承已做**（fork 档）：父进程建管道、`clone` 之后子进程继承两端，父子各持一份
  引用；父关掉自己的写端、子仍持有时读端不会拿到 EOF。
- 阻塞仍不做：`read`/`write` 遇到空/满返 `-EAGAIN` 或短写，不睡眠。
- 没有 `SIGPIPE`：读端关掉后的写只是返回 `-EPIPE`。

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
| 信号帧 | 头 32 B（`magic` / `signo` / 保存的 `blocked` / 帧长）+ **整份 trap 帧快照**（256 / 528 / 1040 B，帧长由端口报，本板 FPU 档 528） |
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
- 无实时信号（1..31 之外一律 `-EINVAL`）、无 `sigaltstack`、无 `siginfo`、
  无 `SA_RESTART` 语义（被信号打断的系统调用不会自动重试）。
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

读数（`tools/fork_gate.py` 的 fork+COW 档）：32 条 check 全 `ok`，其中 COW 段 5 条
—— `cow child saw pre-fork values (bits=3) -> 3 ok`（子进程看到的还是 fork 前的值）、
`parent cow write kept -> 48879 ok`（父的写只落在自己那份）。内核日志断言四条：至少
一条 `[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>`（要求 `shared>=1`、
`stack>=1`、`copied==0`）、至少一条 `[LA64U] pg cow copy idx=<n> ref=2`、不许出现
`ref=1` 的 copy 行（没共享也复制就是白搬字节）、旁路演示读数仍在。门禁跑两遍
（先 fork 档 27 条、再 fork+COW 档 32 条），**不把默认的整块克隆路径从测试里换掉**。

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

读数（`tools/fork_gate.py` 的 fork+COW+SHM 档，共 40 条 check）：SHM 段 8 条 ——
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
现在子进程在一条**放行管道**上**轮询**——本内核的管道是非阻塞的（空管道
`read` 当场 `-EAGAIN`，见 §6），所以要一直 read 到父进程那个字节真的取到——
父进程写完 200 再写一个字节放行；「父看到子的 300」也改成在 `wait4` 返回之后读（子那时已经退出，
没有竞态）。同一手法用在阻塞 `wait4` 那一段：父先读起点时刻，再放子进程去睡 60 ms
—— 否则「子已经跑掉一部分」会让读数偶尔变成 `blocking wait4 waited 2 ms`，
看起来像 wait4 没阻塞。改完 fork+cow+shm 档 3/3 连跑稳定。

### 8.4 `futex`：WAIT / WAKE 最小一档（`ARKRTOS_USE_LINUX_FUTEX`，2026-11-23，QEMU LA64 实跑）

`clone` 造出了第二条执行流，`MAP_SHARED` 让两边看见同一块内存，但**「等一个值变了再继续」**还缺一块：
没有它，pthread 的 mutex / condvar / sem 一个都起不来。futex 就是这块砖——它不是「内核替你同步」，
而是「值没变我就睡，变了就叫我」：绝大部分同步操作在用户态完成，只在真的要让出 CPU 时才进内核。

打开 `ARKRTOS_USE_LINUX_FUTEX`（`depends on ARKRTOS_USE_LINUX_ABI`）后，98 号落最小一档：
`FUTEX_WAIT` / `FUTEX_WAKE`。

| 项 | 现状 |
|---|---|
| op | 只有 `WAIT`(0) / `WAKE`(1)。`FUTEX_PRIVATE_FLAG`(128) **接受但剥掉**——本实现的键本来就是物理的，私有与进程间是同一件事；其余（`REQUEUE` / `WAIT_BITSET` / `PI` / `WAKE_OP` …）一律 `-ENOSYS`，不猜 |
| 键 | **物理帧号 + 页内偏移**，不是 VA。每个进程有自己的 8 KB 对窗口，同一个 VA 在父子身上指不同帧；拿 VA 当键会让两个无关进程在同一个 VA 上互相叫醒——那种错法不报错，只偶发多醒。窗口之外（栈、内核直映射区）查不到帧 ⇒ 如实 `-ENOSYS`，不拿 VA 冒充 |
| 睡眠 | 本内核没有无限睡眠：登记后 `arkrtos_task_block_in_critical(LX_WAIT_SLICE_MS=100 ms)` + `arkrtos_port_svc_restart()`，与 §8.1 同一条可重启系统调用通路。醒来先看 `s_futex_woke`（有人叫过就回 0），否则重读一次字：值变了就当被叫醒，没变继续睡一个时间片；**不编 `-ETIMEDOUT`**（Linux 不传 timeout 时同样永不超时，语义一致）。端口不支持重放时回 `-ENOSYS` 并清槽，**绝不假装睡过**。重放的那一次 a0 已被返回值清成 0（见坑 ②），所以「读哪个字、期望值是多少」都用登记下来的那一份，而不是重放时读到的参数 |
| `-EAGAIN` / `-EINTR` | 入口读到的字 != `val` ⇒ 立即 `-EAGAIN`（不睡）；有未屏蔽待投递信号 ⇒ 立即 `-EINTR`。重放那一次也再查一遍信号，所以信号能把已经睡着的等待者叫回来 |
| 等待槽 | 每任务一槽（`s_futex_key / s_futex_used / s_futex_woke / s_futex_uaddr / s_futex_val[ARKRTOS_TASK_MAX_NUM]`）。后两格记的是「上一次真正睡下时的 `uaddr` 与 `val`」——重放时 a0 已经被返回值清成 0，不存下来就只能拿 0 去读字。一个任务睡着时不可能再发起一次 WAIT，一槽够用；任务退出时 `on_task_exit` 清槽（五格一起清），否则任务号复用后新任务会继承一条「已经在等」的旧记录 |
| `WAKE` | 扫所有任务，同键且登记中的逐个置 `s_futex_woke=1` + `arkrtos_task_wakeup_internal()`，最多 `val` 个，返回实际唤醒数；没有等待者返 0（与 Linux 同） |

读数（`tools/fork_gate.py` 的 fork+COW+SHM+futex 档，56 条 check，其中 futex 段 16 条 —— 9 条基本配对 + 7 条多时间片）：

| 读数 | 钉住什么 |
|---|---|
| `futex mmap shared page > 0 -> 1` | 等的那块字确实来自一段共享映射（不是栈、不是私有页） |
| `futex(WAKE) with no waiter == 0 -> 0` | 没有等待者时不虚报唤醒数 |
| `futex(WAIT) on a different value == -EAGAIN -> -11` | 值已变就不睡（Linux 的 fast path） |
| `futex op other than WAIT/WAKE == -ENOSYS -> -38` | 未实现的 op 如实拒绝，不静默当成 WAIT |
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

③ **「一次 write」不等于「一行」。** §8.3 让 `lx_write` 持控制台锁，解决的是
「内核日志插进用户一行中间」；但夹具自己把一行拆成三次 `write`（前缀、数字、换行）时，
**同一把锁保护不了三次调用之间的空隙**——另一个进程的整行会插进来。全量门禁里就撞到了：
`[fork] parent child=` 与数字之间插进了子进程的 `[fork] child pid=12`，于是
`[fork] parent child=[fork] child pid=12`，按行读的门禁判定「缺了 child pid」，而每条读数
其实都打了。修法在夹具侧：带数字的行先拼进栈上缓冲，**一次 `write` 写出**（`out_line3` /
`out_line5`）。Linux 上的 `printf` 行缓冲做的是同一件事；RT-Thread 的 `rt_kprintf` 之所以
看起来没这个问题，是因为它整行在关中断下拼好再一次输出。判据侧不再为「谁先调度到」留余地。

宿主侧口径：`linuxabi` / `linuxabioff` / `linuxabifutex` / `linuxabifilemap` 四档并集行覆盖率
**97.96%（1055/1077）**（基线钉的是 97.20%，本轮新增 §8.5 之后分母涨了、读数没降），
`linuxabi` 档 **270 checks**、futex 档 **317 checks**、filemap 档 **358 checks**，都打印
`LINUX-ABI HOST OK`；`ARKRTOS_USE_PAGING_SHM=1` 的共享分支**不在宿主账内**
（宿主用例从未在 `LINUX_FORK=1` 下编过），那一半的口径只有上板 40 条 SHM 读数、16 条 futex
读数与 20 条 filemap 读数。剩余 22 行未覆盖经逐行核对全是死/防御分支（两处 `-EBUSY`
重入、`-ENOMEM` 的溢出与对齐变体、`-E2BIG`、嵌入式弱符号兜底等；列表见覆盖率门禁的
说明与本周的 `covmiss` 记录），不是「没测」。

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

读数（`tools/fork_gate.py` 的第五档 fork+COW+SHM+futex+filemap，**69 条 check**，其中 filemap 段 20 条）：

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

读数（第五档 + `-DARKRTOS_USE_PAGING_MPROTECT=1`，**80 条 check**，其中 mprotect 段 11 条）：

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

## 9 开关与零体积

- `ARKRTOS_USE_LINUX_ABI`（`kernelsrc/include/arkrtos_features.h`、
  `config/kconfig/Kconfig.features`），默认 **0**，且 `depends on ARKRTOS_USE_ELF`
  （没有装载器就没有兼容层，features 里有 `#error` 顶住）。
- 默认关时：`arkrtos_linux_abi.c` 整文件不产出任何符号，port 里没有
  `arkrtos_port_linux_syscall_num`，`arkrtos_hal.h` 只剩空宏。**体积与行为零变化**，
  这一条在门禁里是双向实测的（关档 `.o` 的符号表必须全空）。
- `ARKRTOS_USE_LINUX_FORK`（§8），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI &&
  ARKRTOS_USE_EXIT_CODE` 且 `depends on ARKRTOS_USE_PAGING_PAGES &&
  ARKRTOS_USE_PAGING_DEMAND`；关档时 fork 的四个符号（`arkrtos_elf_fork` 与端口层
  三个搬运函数）一个都不产生。
- `ARKRTOS_USE_PAGING_COW`（§8.2），默认 **0**，`#error` 顶住两条前提
  （必须有 `ARKRTOS_USE_LINUX_FORK` 与 `ARKRTOS_USE_PAGING_DEMAND`）；关档时引用
  计数表与 `la_pg_cow_*` 全不编译，`LA_PTE_APP_RO` 也不产出。门禁的两档实测
  （关档零符号、开档 32 条读数）保证「默认档体积与行为零变化」不是一句声明。
- `ARKRTOS_USE_PAGING_SHM`（§8.3），默认 **0**，`depends on ARKRTOS_USE_PAGING_COW`
  且 features.h 用 `#error` 顶住这条依赖（共享位的账本就是 COW 档的引用计数表，
  非 COW 克隆会把每一对实体复制，共享自然消失）；关档时共享位表与
  `arkrtos_port_paging_map_shared` 全不产出、`mmap(MAP_SHARED)` 回 `-ENOSYS`。
  三档门禁实测（关档零符号、开档 40 条读数、SHM 关档的 `-ENOSYS` 对照）。
- `ARKRTOS_USE_LINUX_FUTEX`（§8.4），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` 且
  `features.h` 用 `#error` 顶住这条依赖；关档时不编译等待槽、键解析与 `lx_futex`，98 号落回
  `-ENOSYS`；开档实测是 fork+COW+SHM+futex 档的 56 条读数（其中 futex 16 条，含多时间片 7 条）与
  宿主 futex 档的 317 checks。
- `ARKRTOS_USE_PAGING_FILEMAP`（§8.5），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` +
  `ARKRTOS_USE_PAGING` + `ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND`（内容靠按需缺页
  拿帧，没有 DEMAND 就装不进映射区），`features.h` 用四条 `#error` 顶住；关档时文件映射分支、
  写回记账与 `msync` 全不编译，`mmap(fd)` 回 `-ENOSYS`；开档实测是第五档的 76 条读数（其中
  filemap 20 条）与宿主 filemap 档的 337 checks。
- `ARKRTOS_USE_PAGING_MPROTECT`（§8.6），默认 **0**，`depends on ARKRTOS_USE_LINUX_ABI` +
  `ARKRTOS_USE_PAGING` + `ARKRTOS_USE_PAGING_PAGES` + `ARKRTOS_USE_PAGING_DEMAND`
  （要按需缺页拿帧才能改一对的条目），`features.h` 用四条 `#error` 顶住；关档时台账
  `s_pg_page_ro`、`la_pg_page_ro` / `la_pg_mprotect_hit`、端口 `paging_protect` 与
  `mprotect_stat` 全不产出，`mprotect` 回到「只校验参数、返 0、单独计数」；开档实测是
  **第六档 80 条读数**（其中 mprotect 11 条）与宿主侧编译门禁。
- `MAP_FIXED`（§8.7）**不是独立开关**：它在 `ARKRTOS_USE_PAGING_DEMAND=1` 时才有
  （要有按对装条目的能力，才谈得上「把这一段原来的东西丢掉」）；`DEMAND=0` 时
  `mmap(MAP_FIXED)` 如实回 `-ENOSYS`，夹具里那 6 条读数也整段不出现 —— 所以
  `example/qemu_loongarch64/Makefile` 必须把 DEMAND 镜像进 `LXFORK_DEFS`。
- `arkrtos_autoconf.h` 由 `tools/kconfig/genconfig.py` 从 `config/arkrtos_defconfig`
  生成，新增 Kconfig 项后必须重跑一次，否则默认档读数里看不到这个开关。

## 10 门禁：`tools/linux_abi_gate.py` 与 `tools/fork_gate.py`

```
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py        # 含 make
env -u PYTHONHOME -u PYTHONPATH py -3 tools/linux_abi_gate.py --no-build
```

13 项，全 PASS 时打印 `LINUX-ABI-GATE OK`，并由 `tools/ci_gate.py` 的
`linux-abi` 项调用：

| 项 | 钉住什么 |
|---|---|
| `default-off` | `arkrtos_autoconf.h` 里必须是 0 |
| `kconfig-wired` | Kconfig 项存在且 `depends on ARKRTOS_USE_ELF` |
| `features-guard` | 开 ABI 必须开 ELF 的 `#error` |
| `board-switch` | QEMU LA64 板级显式置 1 |
| `off-zero-symbols` | 关档 `.o` 符号表**全空**（不是「没测」） |
| `port-wiring` | 关档无 `arkrtos_port_linux_syscall_num` / 开档有（两向） |
| `compile-on` | LA64 真板级下 8 个 TU 全编过（含四个用户程序本身） |
| `syscall-table` | 29 个号与 `asm-generic` 逐条相等 |
| `abi-split-la64` | 立即数 == 0 是 Linux、!= 0 是 Ark |
| `abi-split-rv32` | `0x10..0x22` 归 Ark，Linux 最小号 56 不撞 |
| `build` | `make -f example/qemu_loongarch64/Makefile` 通过 |
| `qemu-readout` | Linux 块 14 条 check 全 ok + PASS 行 + `exit code 7`；管道块 14 条 + `exit code 55`；信号块 31 条 + PASS 行 + `exit code 138`；旁路演示未受影响 |
| `parser-selfcheck` | 把 8 种坏日志喂回判据，必须全判红 |

fork 那一档单独一道 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项，**43 项**；fork / fork+COW / fork+COW+SHM / fork+COW+SHM+futex / fork+COW+SHM+futex+filemap / fork+COW+SHM+futex+filemap+mprotect **六档各跑一遍**，所以 build / qemu-readout / parser-selfcheck 各出现六次）：

| 项 | 钉住什么 |
|---|---|
| `default-off` | `arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_FORK` 必须是 0 |
| `kconfig-wired` | 四条 `depends on` 都在（ABI / EXIT_CODE / PAGES / DEMAND） |
| `features-guard` | `features.h` 四道 `#error` |
| `off-zero-symbols` | 关档 RV32 编 `linux_abi.o` 零 `lx_clone`/`lx_wait4`/`s_child`；LA64 板级编 `elf`/`port`/`paging` 零 fork 符号 |
| `compile-on` | LA64 + 两个 flag，四 TU + 夹具全编过 |
| `build` | `make` 出 ELF |
| `qemu-readout` | `[fork]` **27 条** check 全 `ok`、父子 pid 不同、PASS 行、`exit code 0`，且 hello / FS / 其余 Linux 夹具读数仍在；三条新读数（阻塞 `wait4` 真的等了 >= 40 ms / 父读端在子关掉自己那份之后仍读到 3 字节 / SIGCHLD handler 真被调到）逐条校验；另有 `MAP_FIXED` 段 6 条（§8.7，丢弃后内容为 0、地址不变、非对齐与共享形态被拒）逐条校验 |
| `qemu-readout`（COW 档） | fork+COW：`[fork]` **32 条** check 全 `ok`；内核日志 `shared>=1 / stack>=1 / copied=0`、至少一条 `pg cow copy ... ref=2`、无 `ref=1` 的 copy 行；两条 COW 读数（子看到 fork 前的值、父的写只落自己那份）逐条校验 |
| `qemu-readout`（SHM 档） | fork+COW+SHM：`[fork]` **40 条** check 全 `ok`；六条 SHM 读数逐条校验（两条非法形态回 `-38`、映射成功、起始清零、父看到子写、子看到父 fork 之后的写）；内核日志另加至少一条 `pg shm map va=0x<n> pairs>=1` |
| `qemu-readout`（futex 档） | fork+COW+SHM+futex：`[fork]` **56 条** check 全 `ok`；十六条 futex 读数逐条校验（九条基本配对 + 七条多时间片：跨时间片 WAIT 回 0、父看到新值、WAIT 墙钟 >= 150 ms、多时间片唤醒者被 reap、`bits=3` 等）；SHM 与 COW 的全部判据同时仍在 |
| `qemu-readout`（filemap 档） | fork+COW+SHM+futex+filemap：`[fork]` **76 条** check 全 `ok`；filemap 段 20 条读数逐条校验（私有只读内容三读数、页尾仍是文件内容、offset 非对齐回 `-22`、坏 fd 回 `-9`、只读 fd 上可写共享映射回 `-13`、offset=4096 生效且 EOF 后读零、O_RDWR 打开成功、可写共享映射 + `msync` + `munmap`、私有映射 `msync` 不落盘、重映射读回、`bits=31` 的五位证据）；SHM / COW / futex 的全部判据同时仍在 |
| `qemu-readout`（mprotect 档） | fork+COW+SHM+futex+filemap+mprotect：`[fork]` **87 条** check 全 `ok`；mprotect 段 11 条逐条校验（映射成功、`mprotect` 双向可设、只读页仍可读、同对另一半仍可写、撤保护后写得进、违规子进程被 reap 而非重启、状态字 = SIGSEGV、父页未被改）；串口另须有一行 `[LA64U] mprotect violation badv=0x… era=0x… tid=… -> kill task`；filemap / SHM / COW / futex 的全部判据同时仍在 |
| `parser-selfcheck` | 9 种坏日志必须判红（COW 档另加 4 种：位掩码丢一位、父写丢失、整类删光 copy 行、fork 时搬了字节；SHM 档再加 5 种：删光 `pg shm map` 行、共享写没被父看到、子看不到父的写、共享页起始非 0、缺映射成功读数；futex 档再加 5 种：WAIT 没被叫醒、唤醒者只看到一半（`bits=1`）、无等待者 WAKE 回 1、多时间片 WAIT 被打成 `-EFAULT`、多时间片没跨过时间片；filemap 档再加 3 种：写回没发生、写回把文件截短、私有映射的改动被落盘；mprotect 档再加 7 种：只读页读不回来、粒度退化成 8 KB、违规写没被拦住、违规写改了父页、状态字当成退出码、缺违规日志行、缺成功读数） |
| `default-off-shm` / `kconfig-wired-shm` / `features-guard-shm` | SHM 开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_PAGING_COW`、features.h 有对应 `#error`；`off-zero-port` 另核关档没有 `arkrtos_port_paging_map_shared` |
| `default-off-futex` / `kconfig-wired-futex` / `features-guard-futex` | futex 开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_LINUX_ABI`、features.h 有对应 `#error` |
| `default-off-filemap` / `kconfig-wired-filemap` / `features-guard-filemap` | 文件映射开关的三件套：autoconf 为 0、Kconfig 四条 `depends on`（ABI / PAGING / PAGES / DEMAND）、features.h 四条 `#error` |
| `default-off-mprotect` / `kconfig-wired-mprotect` / `features-guard-mprotect` | mprotect 开关的三件套：autoconf 为 0、Kconfig 四条 `depends on`（ABI / PAGING / PAGES / DEMAND）、features.h 四条 `#error` |
| `restore-default` | 收尾把构建目录恢复默认档（避免下一次 `make` 复用 fork 档 `.o`） |

`parser-selfcheck` 是防「绿得没有信息量」的：Linux 块去掉 PASS 行、退出码改 3、
插一条 FAIL、少一条 check，管道块去掉 PASS 行、退出码改成 1，信号块去掉 PASS 行、
退出码改成 1，fork 块把「阻塞读数改成没等」/「父读端改成 `-EBADF`」/「SIGCHLD 没被
调到」，九种都必须被判红。

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

## 12 边界

- 这一层的定位是「**能跑通静态程序的最小系统调用集合**」，不是 Linux 兼容层
  的完整实现。没有 ptrace、没有 `/proc`（Ark 自己的 procfs 另算）。ELF interpreter
  另有一档：`ARKRTOS_USE_DYNLOAD` 打开后走自研 `ld-ark.so`（见
  [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但那是「能跑自研夹具」，不是
  「能跑 glibc 产物」。
- fork 这一档的门禁是独立的 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项）：默认档
  autoconf=0 + 四道 `#error` + Kconfig 两条 `depends on`；关档 RV32 编 `linux_abi.o`
  零 `lx_clone`/`lx_wait4`/`s_child`、LA64 编 `elf`/`port`/`paging` 零 fork 符号；
  开档四 TU + 夹具编过；QEMU 上 `[fork]` 27 条 check 全 `ok`、父子 pid 必须不同、
  `exit code 0`，且同一次启动里 hello / FS / 其余 Linux 夹具的读数仍在；其中
  阻塞 `wait4`、fd 继承与管道引用计数、跨任务 SIGCHLD 三条读数逐条校验；
  解析器另有九种坏日志自检。fork+COW 档（`ARKRTOS_USE_PAGING_COW=1`）另跑一遍：
  32 条 check + 四条内核日志断言（共享对数、栈页私有化对数、`copied=0`、真发生过
  写时复制），坏日志自检再加四种 COW 专属形态；fork+COW+SHM 档（`ARKRTOS_USE_PAGING_SHM=1`）
  再跑一遍：40 条 check + 共享映射内核日志断言（`pg shm map ... pairs>=1`），坏日志
  自检再加五种 SHM 专属形态。
- Real-time 语义不因兼容层改变：Linux ABI 的路径全部在 SVC 里同步完成，
  不额外开线程、不加锁（`read`/`write` 到 ramfs 或管道都是同步搬运；管道刻意
  不做阻塞，见 §6）。唯一的「睡下再回来」是阻塞 `wait4`，它用可重启系统调用实现
  （§8.1）：睡下时把任务从就绪集摘掉、把 `ERA` 退回 `syscall`，唤醒后重放；
  不是关中断自旋。
- 下一步（若要用真正的 Linux 工具链产物）：动态链接的内核侧与自研装载器已落地
  （2026-11-20，见 [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但**离 glibc
  还差整条 libc 兼容面**——`rseq`、TLS 初始化，以及
  futex 的其余 op（`REQUEUE`/`PI`/`WAIT_BITSET` 与 robust list 清理，§8.4）。
  信号已有跨任务投递（§7），「进程」这个对象也开了（`fork`/`clone` + `wait4`，§8，
  默认关）：每进程 fd 表与继承、跨任务 `SIGCHLD`、阻塞 `wait4` 都已在 QEMU LA64 上
  有读数；写时复制也有了（`ARKRTOS_USE_PAGING_COW`，§8.2，默认关）、匿名
  `MAP_SHARED` 共享页也有了（`ARKRTOS_USE_PAGING_SHM`，§8.3，默认关）、futex 最小一档
  （WAIT/WAKE）也有了（`ARKRTOS_USE_LINUX_FUTEX`，§8.4，默认关）、文件映射也有了
  （`ARKRTOS_USE_PAGING_FILEMAP`，§8.5，默认关）、`mprotect` 页级写保护真落实了
  （`ARKRTOS_USE_PAGING_MPROTECT`，§8.6，默认关）、真 `mmap` 语义 `MAP_FIXED` 也有了
  （按需分页档，§8.7，不需另开开关）；仍没有线程（`CLONE_VM`/`CLONE_THREAD` 拒）、
  没有抢占式信号投递、没有 `dup2` 与 `flash` 类重定向。
- `execve` 是「同任务换镜像」；`fork` 才是「多一个任务 + 一份独立地址空间」。
  两者都在，但**不要**因此把这一层读成「有完整的进程模型」：调度与优先级仍是
  内核全局单份，`fd` 表在 fork 档才分进程（默认档全局单份），写时复制只在
  `ARKRTOS_USE_PAGING_COW` 档（且栈页强制私有），没有线程、没有作业控制。