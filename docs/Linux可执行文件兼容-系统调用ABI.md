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

## 1 支持的系统调用（27 个；25 个有实跑读数，2 个只有号表与参数校验）

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
| 96 | `set_tid_address` | 记住地址、返本任务 id（无 futex 语义） | `getpid==task id>0 -> 1 ok` 同路径 |
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
| 222 | `mmap` | 只认匿名映射：`MAP_PRIVATE|MAP_ANONYMOUS` 或 `MAP_SHARED|MAP_ANONYMOUS`（后者要 `ARKRTOS_USE_PAGING_SHM`，§8.3）；拒 `MAP_FIXED` 与同时给 `PRIVATE|SHARED` | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |
| 260 | `wait4` | 只认 `options 0/WNOHANG`；**无 `WNOHANG` 时真阻塞**（可重启系统调用，§8.1），不编造状态，reap 后记账清零（§8） | `wait4 returned child pid -> 12 ok` / `WEXITSTATUS == 42 -> 42 ok` / `wait4(child) again == -ECHILD -> -10 ok` / `blocking wait4 blocked >= 40 ms -> 1 ok` |
| 226 | `mprotect` | 只校验页对齐与窗口范围、返 0 并单独计数；**保护不生效**（整窗一块 PS=20 的 RWX 映射） | 无夹具读数（§1.1） |

未知号一律 `-ENOSYS`（38），并计数；**不猜**。

### 1.1 两个只有号表、没有夹具读数的（诚实标注）

`pread64`(67) 与 `mprotect`(226) 是给 `ld.so` 铺段用的，本轮**没有实跑夹具**：
前者只有「号表与 `asm-generic` 相等 + LA64 板级编译通过」，后者只有参数校验
（对齐、窗口范围）与一条明确的降级说明——它返 0 但**不改任何页权限**，单独计数
就是为了让「接受」与「强制」在读数时能分开，不冒充有页级保护。

表格里其余 25 个号都有 `[linux]` / `[pipe]` / `[sig]` 三个夹具的串口读数。

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
| `futex` | 无 | 没有共享内存与等待队列语义 |
| 真 `mmap` 语义（文件映射/`MAP_FIXED`/权限位） | 匿名私有 + 匿名共享（`MAP_SHARED` 只随 `fork` 继承，无文件后端、无 `shm_open`，§8.3）；`RWX` 权限不区分 | 窗口式分配器（§4）；兑现文件映射需要文件后端，权限位需要页级保护 |
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
调用（`read` 空管道、`futex`）**没有**接这条通路。

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

读数（`tools/fork_gate.py` 的 fork+COW 档）：26 条 check 全 `ok`，其中 COW 段 5 条
—— `cow child saw pre-fork values (bits=3) -> 3 ok`（子进程看到的还是 fork 前的值）、
`parent cow write kept -> 48879 ok`（父的写只落在自己那份）。内核日志断言四条：至少
一条 `[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>`（要求 `shared>=1`、
`stack>=1`、`copied==0`）、至少一条 `[LA64U] pg cow copy idx=<n> ref=2`、不许出现
`ref=1` 的 copy 行（没共享也复制就是白搬字节）、旁路演示读数仍在。门禁跑两遍
（先 fork 档 21 条、再 fork+COW 档 26 条），**不把默认的整块克隆路径从测试里换掉**。

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
| 继承 | 只随 `fork` 继承。没有 `shm_open`、没有 `mmap` 文件后端、没有 `MAP_FIXED`；关档时 `MAP_SHARED` 一律 `-ENOSYS` |
| 非法形态 | 不带 `MAP_ANONYMOUS`、同时给 `PRIVATE\|SHARED`、带 `MAP_FIXED` 都是 `-ENOSYS`（本内核的统一口径：不支持是 `ENOSYS`，不是 `EINVAL`） |

读数（`tools/fork_gate.py` 的 fork+COW+SHM 档，共 34 条 check）：SHM 段 8 条 ——
两条非法形态回 `-38`、`mmap(MAP_SHARED|ANON) > 0`、`shared page starts zeroed -> 0`、
`clone for shm child > 0`、`parent saw child shared write -> 1`、`shm child exit reaped -> 12`、
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
  （关档零符号、开档 26 条读数）保证「默认档体积与行为零变化」不是一句声明。
- `ARKRTOS_USE_PAGING_SHM`（§8.3），默认 **0**，`depends on ARKRTOS_USE_PAGING_COW`
  且 features.h 用 `#error` 顶住这条依赖（共享位的账本就是 COW 档的引用计数表，
  非 COW 克隆会把每一对实体复制，共享自然消失）；关档时共享位表与
  `arkrtos_port_paging_map_shared` 全不产出、`mmap(MAP_SHARED)` 回 `-ENOSYS`。
  三档门禁实测（关档零符号、开档 34 条读数、SHM 关档的 `-ENOSYS` 对照）。
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
| `syscall-table` | 27 个号与 `asm-generic` 逐条相等 |
| `abi-split-la64` | 立即数 == 0 是 Linux、!= 0 是 Ark |
| `abi-split-rv32` | `0x10..0x22` 归 Ark，Linux 最小号 56 不撞 |
| `build` | `make -f example/qemu_loongarch64/Makefile` 通过 |
| `qemu-readout` | Linux 块 14 条 check 全 ok + PASS 行 + `exit code 7`；管道块 14 条 + `exit code 55`；信号块 31 条 + PASS 行 + `exit code 138`；旁路演示未受影响 |
| `parser-selfcheck` | 把 8 种坏日志喂回判据，必须全判红 |

fork 那一档单独一道 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项，22 项；fork / fork+COW / fork+COW+SHM **三档各跑一遍**，所以 build / qemu-readout / parser-selfcheck 各出现三次）：

| 项 | 钉住什么 |
|---|---|
| `default-off` | `arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_FORK` 必须是 0 |
| `kconfig-wired` | 四条 `depends on` 都在（ABI / EXIT_CODE / PAGES / DEMAND） |
| `features-guard` | `features.h` 四道 `#error` |
| `off-zero-symbols` | 关档 RV32 编 `linux_abi.o` 零 `lx_clone`/`lx_wait4`/`s_child`；LA64 板级编 `elf`/`port`/`paging` 零 fork 符号 |
| `compile-on` | LA64 + 两个 flag，四 TU + 夹具全编过 |
| `build` | `make` 出 ELF |
| `qemu-readout` | `[fork]` **21 条** check 全 `ok`、父子 pid 不同、PASS 行、`exit code 0`，且 hello / FS / 其余 Linux 夹具读数仍在；三条新读数（阻塞 `wait4` 真的等了 >= 40 ms / 父读端在子关掉自己那份之后仍读到 3 字节 / SIGCHLD handler 真被调到）逐条校验 |
| `qemu-readout`（COW 档） | fork+COW：`[fork]` **26 条** check 全 `ok`；内核日志 `shared>=1 / stack>=1 / copied=0`、至少一条 `pg cow copy ... ref=2`、无 `ref=1` 的 copy 行；两条 COW 读数（子看到 fork 前的值、父的写只落自己那份）逐条校验 |
| `qemu-readout`（SHM 档） | fork+COW+SHM：`[fork]` **34 条** check 全 `ok`；六条 SHM 读数逐条校验（两条非法形态回 `-38`、映射成功、起始清零、父看到子写、子看到父 fork 之后的写）；内核日志另加至少一条 `pg shm map va=0x<n> pairs>=1` |
| `parser-selfcheck` | 9 种坏日志必须判红（COW 档另加 4 种：位掩码丢一位、父写丢失、整类删光 copy 行、fork 时搬了字节；SHM 档再加 5 种：删光 `pg shm map` 行、共享写没被父看到、子看不到父的写、共享页起始非 0、缺映射成功读数） |
| `default-off-shm` / `kconfig-wired-shm` / `features-guard-shm` | SHM 开关的三件套：autoconf 为 0、Kconfig 有 `depends on ARKRTOS_USE_PAGING_COW`、features.h 有对应 `#error`；`off-zero-port` 另核关档没有 `arkrtos_port_paging_map_shared` |
| `restore-default` | 收尾把构建目录恢复默认档（避免下一次 `make` 复用 fork 档 `.o`） |

`parser-selfcheck` 是防「绿得没有信息量」的：Linux 块去掉 PASS 行、退出码改 3、
插一条 FAIL、少一条 check，管道块去掉 PASS 行、退出码改成 1，信号块去掉 PASS 行、
退出码改成 1，fork 块把「阻塞读数改成没等」/「父读端改成 `-EBADF`」/「SIGCHLD 没被
调到」，九种都必须被判红。

## 11 本轮踩到的三个真 bug 与一个读数坑

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

## 12 边界

- 这一层的定位是「**能跑通静态程序的最小系统调用集合**」，不是 Linux 兼容层
  的完整实现。没有 ptrace、没有 `/proc`（Ark 自己的 procfs 另算）。ELF interpreter
  另有一档：`ARKRTOS_USE_DYNLOAD` 打开后走自研 `ld-ark.so`（见
  [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但那是「能跑自研夹具」，不是
  「能跑 glibc 产物」。
- fork 这一档的门禁是独立的 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项）：默认档
  autoconf=0 + 四道 `#error` + Kconfig 两条 `depends on`；关档 RV32 编 `linux_abi.o`
  零 `lx_clone`/`lx_wait4`/`s_child`、LA64 编 `elf`/`port`/`paging` 零 fork 符号；
  开档四 TU + 夹具编过；QEMU 上 `[fork]` 21 条 check 全 `ok`、父子 pid 必须不同、
  `exit code 0`，且同一次启动里 hello / FS / 其余 Linux 夹具的读数仍在；其中
  阻塞 `wait4`、fd 继承与管道引用计数、跨任务 SIGCHLD 三条读数逐条校验；
  解析器另有九种坏日志自检。fork+COW 档（`ARKRTOS_USE_PAGING_COW=1`）另跑一遍：
  26 条 check + 四条内核日志断言（共享对数、栈页私有化对数、`copied=0`、真发生过
  写时复制），坏日志自检再加四种 COW 专属形态；fork+COW+SHM 档（`ARKRTOS_USE_PAGING_SHM=1`）
  再跑一遍：34 条 check + 共享映射内核日志断言（`pg shm map ... pairs>=1`），坏日志
  自检再加五种 SHM 专属形态。
- Real-time 语义不因兼容层改变：Linux ABI 的路径全部在 SVC 里同步完成，
  不额外开线程、不加锁（`read`/`write` 到 ramfs 或管道都是同步搬运；管道刻意
  不做阻塞，见 §6）。唯一的「睡下再回来」是阻塞 `wait4`，它用可重启系统调用实现
  （§8.1）：睡下时把任务从就绪集摘掉、把 `ERA` 退回 `syscall`，唤醒后重放；
  不是关中断自旋。
- 下一步（若要用真正的 Linux 工具链产物）：动态链接的内核侧与自研装载器已落地
  （2026-11-20，见 [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但**离 glibc
  还差整条 libc 兼容面**——futex、rseq、TLS 初始化、文件映射、`mprotect` 真落实。
  信号已有跨任务投递（§7），「进程」这个对象也开了（`fork`/`clone` + `wait4`，§8，
  默认关）：每进程 fd 表与继承、跨任务 `SIGCHLD`、阻塞 `wait4` 都已在 QEMU LA64 上
  有读数；写时复制也有了（`ARKRTOS_USE_PAGING_COW`，§8.2，默认关）、匿名
  `MAP_SHARED` 共享页也有了（`ARKRTOS_USE_PAGING_SHM`，§8.3，默认关）；仍没有线程
  （`CLONE_VM`/`CLONE_THREAD` 拒）、没有抢占式信号投递、没有 `dup2` 与 `flash`
  类重定向、没有文件映射。
- `execve` 是「同任务换镜像」；`fork` 才是「多一个任务 + 一份独立地址空间」。
  两者都在，但**不要**因此把这一层读成「有完整的进程模型」：调度与优先级仍是
  内核全局单份，`fd` 表在 fork 档才分进程（默认档全局单份），写时复制只在
  `ARKRTOS_USE_PAGING_COW` 档（且栈页强制私有），没有线程、没有作业控制。