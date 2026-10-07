# 与 Linux 的差距盘点

**这份文档回答一个问题：把 Ark RTOS 和 Linux 摆在一起，App 侧与内核侧还差什么？**

先说清口径，免得把两种东西的差异当成"缺陷"：

- Ark RTOS 的目标**不是**做一个 Linux，而是让 MCU "像手机一样安装、升级、卸载应用与驱动"。
  Linux 的目标是通用分时操作系统（进程模型 + 虚拟内存 + 多用户 + 完整生态）。
  两者**共享的只是"给用户态程序用的那套名字"**（POSIX：文件、socket、pthread、信号量……），
  名字对齐了、语义能落到真实现上，迁移成本才低。
- 因此下面把差距分成三类，**只有第 2 类才是"欠账"**：
  1. **本质设计取舍**——Linux 有、但 Ark RTOS 的定位决定了不打算做（做了就不是它了）；
  2. **口径内的欠账**——已经承诺"像 Linux 一样用"，但目前没做到或明确报错的；
  3. **生态维度**——Linux 靠几十年积累的库/协议/认证，本项目不声称在这一维度上更强。
- 所有结论都指向仓库里可核验的文件，不写"大概""应该"。
  凡"不支持"，仓库的做法一律是**返回错误并置 `errno`**，而不是返回一个看着合理的结果
  （见 [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) §1）。

---

## 一、已经"像 Linux"的部分（先把账算平）

不看生态，只看"用户态程序能用到什么"，这几样是真的按 Linux 习惯做的：

| 维度 | 已经有的东西 | 出处 |
|------|------------|------|
| 路径命名空间 | `/` 是易失 ramfs、`/dev` 是设备注册表镜像、`/mnt/<名字>` 是持久卷、`/proc` 是只读扁平虚拟 fs；`ls /` 会合并子挂载点列出 `dev` 与 `proc` | [VFS路径命名空间](VFS路径命名空间.md) |
| POSIX 名字 | `pthread` / `semaphore` / `mqueue` / `unistd` / `dirent` / `sys/stat` / `fcntl` / `errno`；一份普通 C 程序改 include 列表即可编译上板 | [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) §4、§10 |
| 文件类 POSIX | `open/read/write/lseek/stat/fstat/unlink/opendir/readdir/closedir` 直接打到 VFS；`FS_DEMO` 上板全过 | 同上 §4.7 |
| socket / select | lwIP 2.1.3 + `socket/select` 兼容面，`SOCKET_DEMO` `pass=36 fail=0`（**回环**网卡） | [socket与select兼容层](socket与select兼容层.md) |
| `/proc` | `version/uptime/meminfo/cpuinfo/tasks`，内容每次读取现生成 | `kernelsrc/include/arkrtos_features.h`（`ARKRTOS_USE_PROCFS`） |
| 设备模型 | 设备树（DTS 子集）+ compatible 匹配；`dt` 命令打印 20 节点 | [设备树](设备树.md) |
| 内核控制台 | `console` 命令族 + `mount/ls/cat/fs` 等，操作习惯向 Linux 靠 | [内核Shell控制台使用说明](内核Shell控制台使用说明.md)、[Shell与日志Linux化](Shell与日志Linux化.md) |
| 线程服务 | 内核新增 SVC 0x1B（`arkrtos_thread_create/self/exit`），POSIX `pthread` 建在其上 | [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) §5 |
| 权限模型 | 任务跑非特权态、SVC 陷入内核、MPU 逐任务隔离，越权当场硬件拦下（`pass=78 fail=0`） | [内存保护与App越权验证](内存保护与App越权验证.md) |

一句话：**"用法像 Linux"这一层，主干是通的**；下面列的是没做到的部分。

---

## 二、逐维度差距表

### 1. 进程与执行模型（差距最大的一块）

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| 进程（独立地址空间）+ `fork` / `exec` / `waitpid` | 无进程模型：**一个 App = 一个地址空间 + 若干线程**；`fork` / `exec*` 明确不提供 | 取舍 |
| `getpid` / `getppid` / PID 命名空间 | 只有内核任务号（任务表索引），`getpid` 只有桩 | 取舍 |
| 动态链接、`.so` 共享库、ELF 加载器 | 镜像是自有 `.svcapp` 容器，装载即整体复制到 RAM；**无 ELF、无动态链接、无共享库** | 取舍 |
| `argv` / `argc` / 环境变量 | App 入口是 `main(void)`，无 argv、无 `environ` | 取舍 |
| 信号（`signal` / `sigaction` / `kill`） | **无 Unix 信号**；进程间/线程间同步用事件组、信号量、互斥量、消息队列、条件变量 | 取舍 |
| `pipe` / `FIFO` / Unix domain socket | 无；等价物是消息队列 + socket（回环） | 取舍 |
| System V / POSIX 共享内存 `shm` | 无 `shmget`/`shm_open`；只有 MPU 的共享窗口（用于小程序交接，不对外当通用 IPC） | 取舍 |
| `clone` / 命名空间 / 容器 | 无 | 取舍 |

### 2. 内存管理

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| MMU 虚拟内存、地址翻译、按需分页、swap | **只有 MPU 分段保护，没有地址翻译**（Cortex-M 无 MMU）；无虚拟内存 | 取舍 |
| `mmap` / `munmap` / `brk` / `sbrk` | 无；App 内存来自槽位 RAM 窗口 + App 侧 arena（`arkrtos_posix_malloc`，默认 1 KB） | 取舍 |
| 内核统一管理物理内存 | 内核不是分配器：地址在 `config/arkrtos_partition.h` 定义一次，槽位窗口在安装时绑定 | 取舍 |
| 内存超售 / OOM killer | 无；栈溢出/越界由 MPU + 栈哨兵当场拦 | 取舍 |

> 这一块是架构性的：没有 MMU 就没有虚拟内存，也就没有 `fork` 的 COW 基础。
> 这不是"没做完"，是 Cortex-M 的硬件前提决定的。

### 3. 文件系统 / VFS（口径内欠账较集中的一块）

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| ext4 / xfs / btrfs / tmpfs / sysfs / debugfs / FUSE… | 只有四种：**ramfs（易失 `/`）、littlefs（持久卷）、devfs 镜像、procfs（只读扁平）** | 取舍（小系统） |
| 符号链接 `symlink` / 硬链接 `link` | VFS **无 symlink/link**；POSIX 层也没有 | 欠账 |
| `truncate` / `ftruncate` | VFS 无 `truncate` | 欠账 |
| `mkdir` / `rename` / `rmdir` | VFS **有** `mkdir` / `rename` / `unlink`，但 **POSIX 层只封了 `unlink`**，`rmdir`/`mkdir`/`rename` 没有 POSIX 名字 | 欠账（接口没接线） |
| `inode` 语义（`st_ino` / `st_nlink`） | `struct stat` 故意只放 VFS 答得出的字段：`st_mode` 类型位、`st_size`、`st_mtime`；**无 `st_ino`/`st_nlink`** | 取舍 |
| 文件权限 `rwx`、属主/属组 | **无权限位、无 `chmod` 语义的 rwx**；模型是"这个 App 能不能走到这个路径" | 取舍 |
| `ioctl` / `fcntl` 锁 / `flock` | 无文件锁；设备侧 `ctrl` 是私有控制接口，不是 POSIX `ioctl` | 取舍 |
| `inotify` / `epoll` | 无（有 `select`/`poll` 兼容面，底在 lwIP）；无 `epoll`、无 `inotify` | 取舍 |
| 挂载选项 / loop 设备 / 绑定挂载 | `mount` 只按名字挂卷，无选项、无 loop、无 bind mount | 取舍 |
| 磁盘配额 / 审计 | 无 | 取舍 |
| 挂载点自动发现 / `fstab` | 无；`/mnt/<名字>` 由内核按块设备注册表生成 | 取舍 |

> 注：VFS 的 `ark_vfs` 上游 API 面**确实**有 `mkdir` / `rename` / `resolve` / `normalize` 等，
> 缺的是 **POSIX 层的名字没有对应封装**（见 `kernelsrc/sdk/posix/`）。这一条属于"接口没接线"，不是"内核没有"。
> 详见 [VFS路径命名空间](VFS路径命名空间.md) 与 `kernelsrc/include/arkrtos_vfs.h`。

### 4. 权限与安全

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| 多用户、uid/gid、`setuid` | **无多用户、无 uid/gid**；POSIX 的 `chmod`/`chown` 有名字但无强制语义 | 取舍 |
| capabilities、LSM（SELinux/AppArmor）、seccomp | 无 | 取舍 |
| Secure Boot 信任链、签名验证 | 有**可选**的镜像签名（HMAC-SHA256，对称，默认关），是"防篡改 + 认来源"，**不是完整信任链**；明确不做 A/B 回滚 | 取舍（已划边界） |
| 内核地址空间隔离 / KASLR | 非特权任务与内核靠硬件特权级 + MPU 隔离；无 ASLR | 取舍 |

### 5. 调度与并发

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| SMP 多核、CPU 亲和性、`cgroup` | **单核**；`arkrtos_spin.h` 为 SMP 预留了抽象，但当前实现是单核 | 取舍 |
| 完全公平调度 / 多种调度类 | 优先级抢占 + 同优先级时间片；就绪集是 256 位两级位图（O(1)） | 取舍 |
| 实时补丁 PREEMPT_RT | 本身就是 RTOS：确定性优于 Linux 主线 | —— |
| `nice` / `sched_setscheduler` | 无对应 POSIX 名字（优先级在建线程时给） | 欠账（小） |

### 6. 网络

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| 完整 TCP/IP、netfilter、netlink、raw socket、多网卡、无线 | lwIP **回环**（`127.0.0.1/8`），无真实 PHY 收发；`socket`/`select` 兼容面可用 | 欠账（板级缺 PHY） |
| DNS / DHCP / NTP 客户端 | 无 | 生态 |
| TLS / OpenSSL | 无 | 生态 |
| `getaddrinfo` / `if_nametoindex` 等一整套 | 只有兼容面覆盖到的那部分 | 生态 |

> 网络这块是"能验语义、不能验收发"：见 [lwIP协议栈接入](lwIP协议栈接入.md) §1 与 readme「仍未上板验证」清单。

### 7. 时间

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| RTC、墙钟、时区、`localtime` / `strftime` | **无 RTC、无日历**；`CLOCK_REALTIME` 被别名到单调时钟（不报错答案）；`st_mtime` 是**开机以来秒数**；`localtime` 不提供 | 取舍 |
| `timerfd` / POSIX 定时器 | 有软件定时器（SVC 0x17）；无 `timerfd` | 取舍 |

### 8. 系统管理 / 用户空间

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| init / systemd / service manager | 无 init 系统；有**开机自启清单**（镜像头 `auto-start`） | 取舍 |
| 完整 coreutils、包管理（apt/opkg） | 无包管理器；内核控制台给了一套精简命令（`info/app/drv/task/sched/fault/install/log/pool/trace/cfg` + `mount/ls/cat/fs`） | 取舍 |
| 内核模块 `.ko` | 无内核模块；**但驱动/App 走同一条动态安装通道**（`.svcapp` → `install`），这条主线本身就是对"装模块"的替代 | —— |
| 用户 / 登录 / 会话 / `su` / `sudo` | 无 | 取舍 |
| 电源管理（DVFS / suspend / resume） | 无 | 取舍 |
| `/sys` / `sysctl` / `udev` 热插拔 | 无 `/sys`、无热插拔；设备在启动时按设备树枚举 | 取舍 |

### 9. 开发 / 调试 / 工具链

| Linux 有 | Ark RTOS | 归类 |
|---------|---------|------|
| GCC/Clang + make/cmake + GDB | 主工程是 Keil（AC5/AC6，`uvprojx`）；有 Kconfig 工具链与 `tools/` 脚本；调试走 `mdkdebug`（UVSOCK）而非 GDB | 取舍 |
| `ptrace` / `perf` / `ftrace` / `strace` | 无；等价物是内核 trace/scope、`dmesg`、崩溃日记 | 取舍 |
| 动态分析桩 / eBPF | 无 | 取舍 |

---

## 三、如果把"欠账"排个序

只列**口径内**（"已经承诺像 Linux、但还没做到或明确报错"）的项，按"补起来值不值"排：

| 优先级 | 欠账 | 现状 | 备注 |
|--------|------|------|------|
| 高 | POSIX 层缺 `mkdir`/`rmdir`/`rename` 封装 | VFS 内核面已有，只是没接线 | 成本最低、收益最直接；补上后文件目录操作与 Linux 一致 |
| 高 | 真实以太网收发 | 只有回环网卡 | 需板级 PHY 驱动 + RMII 引脚；是"能不能说支持网络"的分水岭 |
| 中 | `truncate` / `ftruncate`、软/硬链接 | VFS 无 | 需要 littlefs 后端配合 |
| 中 | API 缺口：`symlink`、文件锁、`epoll` | 无 | 按需补，不是迁移硬门槛 |
| 低 | `nice` / `sched_setscheduler` 等调度 POSIX 名字 | 无 | 建线程时已能定优先级，够用 |
| 低 | `/sys` 风格可写内核属性面 | 无（`/proc` 只读） | 有需要再做 |

**不打算做的**（第一类取舍，这里明确写下以免反复被当成"还没做"）：
进程模型（fork/exec）、虚拟内存/MMU、动态链接/ELF、多用户/uid-gid、SMP、电源管理、
ASLR、内核模块、包管理器、信号（Unix signal）、共享内存 IPC、`/sys`+热插拔。

---

## 四、一句话结论

- **"用法像 Linux"的主干是通的**（路径命名空间、POSIX/Windows 兼容层、文件类 POSIX、socket/select、pthread、`/proc`、设备树、控制台）。
- **真正算欠账的不多**：最值得补的是 **POSIX 层缺的目录操作封装**（内核已具备，纯接线）与 **真实以太网收发**（板级缺 PHY）。
- **其余大部分差距是定位差**，不是完成度差：没有 MMU 就没有虚拟内存，没有进程模型就没有 `fork`——
  这些是 Cortex-M 的硬件前提 + 本项目"动态安装应用/驱动"的定位共同决定的。
  把 Linux 的通用 OS 清单拿来逐条打分，会把这些"本就不打算做"的项误判成缺失。

---

## 相关文档

- [POSIX与Windows兼容层说明](POSIX与Windows兼容层说明.md) —— 兼容层能力表与边界
- [socket与select兼容层](socket与select兼容层.md)
- [VFS路径命名空间](VFS路径命名空间.md)
- [小程序设计](小程序设计.md) —— 与 ELF 的对照
- [崩溃恢复与镜像版本把关](崩溃恢复与镜像版本把关.md) —— 签名/回滚边界
- [硬件耦合审计](硬件耦合审计.md) —— 能力/选配/契约/布局四层
