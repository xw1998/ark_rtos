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
| 222 | `mmap` | 只认 `MAP_PRIVATE|MAP_ANONYMOUS`；拒 `MAP_FIXED` | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |
| 260 | `wait4` | 只认 `options 0/WNOHANG`；不阻塞、不编造状态，reap 后记账清零（§8） | `wait4 returned child pid -> 12 ok` / `WEXITSTATUS == 42 -> 42 ok` / `wait4(child) again == -ECHILD -> -10 ok` |
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
| 信号只做到「最小一档」 | 同一任务自我投递 + 系统调用返回前投递；无跨任务、无抢占、无实时、无 `siginfo`（§7） | 投递对象仍是「自己」；跨任务投递要先有第二个可投递实体的判定与抢占式入口，本轮先不做 |
| `fd` 继承、重定向 | 无 | fd 表是内核全局（§4），shell 的 `exec` 起新任务并清零；`execve` 保留 fd，但没有父子可继承（管道已做，见 §6） |
| `socket` / `select` | 无（Ark 自己有一套 socket 兼容层，但**不在** Linux ABI 号表里） | 两套编号与阻塞语义不同，混在一个号表里只会给出错答案 |
| `futex` | 无 | 没有共享内存与等待队列语义 |
| 真 `mmap` 语义（文件映射/共享/`MAP_FIXED`/权限位） | 只做匿名私有分配，`RWX` 权限不区分 | 窗口式分配器（§4），兑现真语义需要页级权限与文件后端 |
| 多线程 | 无 | `clone` 没有；`exit_group` 与 `exit` 等价 |

## 4 降级实现，以及为什么这么降

- **fd 表**：内核全局 `lx_fd_t s_fd[8]`（路径 + 偏移），shell 的 `exec`（起新任务）
  成功时 `reset`；`execve`（同任务换镜像）**保留** `execve` 之前的 fd，与 Linux
  一致。没有父子，所以仍然谈不上「继承」。
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
| 读空缓冲、写端还在 | `-EAGAIN`（11） | 阻塞 | 没有 `fork`，fd 表全局单份，管道两端通常落在**同一个任务**里；阻塞就是自己等自己 |
| 读空缓冲、写端全关 | 返回 `0`（EOF） | 返回 0 | 与 Linux 一致；**绝不用 0 冒充「暂时没数据」** |
| 写满缓冲 | 短写（返回实际字节数） | 阻塞到能写 | 同上，不阻塞；短写本来就是 Linux 允许的返回形状 |
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
- 没有 `fork` / `clone`，所以**没有 fd 继承**：管道能在同一任务里用、能跨 `execve`
  存活，但没法「父进程建管道、子进程继承一端」。
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

- **无跨任务投递**：只能自己发给自己。
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
| `wait4` 返回 | 子活着 + `WNOHANG` → `0`；子活着 + 无 `WNOHANG` → `-EAGAIN`（本内核不能在系统调用里睡眠等事件）；不是自己的子 → `-ECHILD`；子已死但退出码没上报 → `-EIO`（绝不编造 0 或 SIGKILL） |
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

fork 那一档单独一道 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项，11 项）：

| 项 | 钉住什么 |
|---|---|
| `default-off` | `arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_FORK` 必须是 0 |
| `kconfig-wired` | 四条 `depends on` 都在（ABI / EXIT_CODE / PAGES / DEMAND） |
| `features-guard` | `features.h` 四道 `#error` |
| `off-zero-symbols` | 关档 RV32 编 `linux_abi.o` 零 `lx_clone`/`lx_wait4`/`s_child`；LA64 板级编 `elf`/`port`/`paging` 零 fork 符号 |
| `compile-on` | LA64 + 两个 flag，四 TU + 夹具全编过 |
| `build` | `make` 出 ELF |
| `qemu-readout` | `[fork]` 十条 check 全 `ok`、父子 pid 不同、PASS 行、`exit code 0`，且 hello / FS / 其余 Linux 夹具读数仍在 |
| `parser-selfcheck` | 6 种坏日志必须判红 |
| `restore-default` | 收尾把构建目录恢复默认档（避免下一次 `make` 复用 fork 档 `.o`） |

`parser-selfcheck` 是防「绿得没有信息量」的：Linux 块去掉 PASS 行、退出码改 3、
插一条 FAIL、少一条 check，管道块去掉 PASS 行、退出码改成 1，信号块去掉 PASS 行、
退出码改成 1，八种都必须被判红。

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
后端在进程被 kill 时丢不满 4 KB 的尾块（改管道）。

## 12 边界

- 这一层的定位是「**能跑通静态程序的最小系统调用集合**」，不是 Linux 兼容层
  的完整实现。没有 ptrace、没有 `/proc`（Ark 自己的 procfs 另算）。ELF interpreter
  另有一档：`ARKRTOS_USE_DYNLOAD` 打开后走自研 `ld-ark.so`（见
  [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但那是「能跑自研夹具」，不是
  「能跑 glibc 产物」。
- fork 这一档的门禁是独立的 `tools/fork_gate.py`（`ci_gate` 的 `fork` 项）：默认档
  autoconf=0 + 四道 `#error` + Kconfig 两条 `depends on`；关档 RV32 编 `linux_abi.o`
  零 `lx_clone`/`lx_wait4`/`s_child`、LA64 编 `elf`/`port`/`paging` 零 fork 符号；
  开档四 TU + 夹具编过；QEMU 上 `[fork]` 十条 check 全 `ok`、父子 pid 必须不同、
  `exit code 0`，且同一次启动里 hello / FS / 其余 Linux 夹具的读数仍在；
  解析器另有六种坏日志自检。
- Real-time 语义不因兼容层改变：Linux ABI 的路径全部在 SVC 里同步完成，
  不额外开线程、不加锁、不引入阻塞点（`read`/`write` 到 ramfs 或管道都是同步
  搬运；管道还刻意不做阻塞，见 §6）。
- 下一步（若要用真正的 Linux 工具链产物）：动态链接的内核侧与自研装载器已落地
  （2026-11-20，见 [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但**离 glibc
  还差整条 libc 兼容面**——futex、rseq、TLS 初始化、文件映射、`mprotect` 真落实。
  信号已做最小一档（§7），「进程」这个对象的头也开了（`fork`/`clone` + `wait4`，
  §8，默认关）；但那一档只做到「一次分叉、一次回收」，没有线程、没有 fd 继承、
  没有跨任务信号、没有阻塞等待。
- `execve` 是「同任务换镜像」；`fork` 才是「多一个任务 + 一份独立地址空间」。
  两者都在，但**不要**因此把这一层读成「有完整的进程模型」：调度、优先级、
  fd 表仍是内核全局单份，`fork` 不复制 fd 表（§8）。