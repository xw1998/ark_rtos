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

## 1 支持的系统调用（15 个，全部有实测读数）

编号取 `asm-generic`（LoongArch / RISC-V / ARM64 共用这一套）：

| 号 | 名字 | Ark 侧兑现方式 | 实测读数 |
|---:|---|---|---|
| 56 | `openat` | `arkrtos_vfs_app_stat()` 查命名空间；目录返 `-EISDIR` | `openat>0 -> 1 ok` |
| 57 | `close` | 释放 fd 槽，返 0 | `close -> 0 ok` |
| 63 | `read` | `arkrtos_vfs_read_at()` 按 fd 的偏移读 | `read 4 bytes -> 4 ok`（读到 ELF magic） |
| 64 | `write` | 逐 128 字节 `arkrtos_board_putc` | `hello from the Linux syscall ABI` 正文 |
| 93 | `exit` | `arkrtos_thread_exit_code_internal()` | `exit code 7` |
| 94 | `exit_group` | 同上（单线程程序等价） | 同上 |
| 96 | `set_tid_address` | 记住地址、返本任务 id（无 futex 语义） | `getpid==task id>0 -> 1 ok` 同路径 |
| 113 | `clock_gettime` | `MONOTONIC` 返内核节拍；`REALTIME` 返 `-ENOSYS` | `MONOTONIC -> 0 ok` / `REALTIME -> -38 ok` |
| 160 | `uname` | `sysname=ArkRTOS`、`machine=loongarch64` | `uname -> 0 ok` / `sysname=ArkRTOS machine=loongarch64` |
| 172 | `getpid` | 返内任务 id | `getpid==task id>0 -> 1 ok` |
| 174 | `getuid` | 恒 0 | 走同一返回路径 |
| 175 | `geteuid` | 恒 0 | 走同一返回路径 |
| 214 | `brk` | 窗口内单调分配（见 §4） | `brk(0)>0` / `brk(+4096) -> 34607104 ok` |
| 215 | `munmap` | 只返 0，**不归还地址** | 走同一返回路径 |
| 222 | `mmap` | 只认 `MAP_PRIVATE|MAP_ANONYMOUS`；拒 `MAP_FIXED` | `mmap>0 -> 1 ok` / `mmap write/read -> 1 ok` |

未知号一律 `-ENOSYS`（38），并计数；**不猜**。

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
| `fork` / `clone` / `execve` | 无 | 没有进程、没有父子、没有地址空间复制；`exec` 由内核装载路径提供（shell 的 `exec` 命令），不是 `execve` 语义 |
| 信号（`rt_sigaction`/`kill`/`sigreturn`） | 无 | 需要用户栈上的信号帧与返回路径，Ark 的陷阱帧模型是另一套 |
| `fd` 继承、管道、重定向 | 无 | fd 表是内核全局（§4），`exec` 时清零；没有父子可继承 |
| `socket` / `select` | 无（Ark 自己有一套 socket 兼容层，但**不在** Linux ABI 号表里） | 两套编号与阻塞语义不同，混在一个号表里只会给出错答案 |
| `futex` | 无 | 没有共享内存与等待队列语义 |
| 真 `mmap` 语义（文件映射/共享/`MAP_FIXED`/权限位） | 只做匿名私有分配，`RWX` 权限不区分 | 窗口式分配器（§4），兑现真语义需要页级权限与文件后端 |
| 多线程 | 无 | `clone` 没有；`exit_group` 与 `exit` 等价 |

## 4 降级实现，以及为什么这么降

- **fd 表**：内核全局 `lx_fd_t s_fd[8]`（路径 + 偏移），`exec` 成功时 `reset`。
  没有父子，所以不需要「每个进程一张表」；反过来，**不能**假装继承。
- **堆**：`brk` / `mmap` 共用 ELF 窗口内的一段（窗口 `0x02000000`，2 MB；堆从
  窗口一半起，末尾留 128 KB 给栈）。单调向上分配，不回收。
- **`munmap` 只返 0、不归还地址**：这是有意的。若「假装还了」，程序会认为那块
  内存仍可用而实际已被别的东西占用——那是**静默错答案**，比 `-ENOSYS` 坏得多。
- **用户指针纪律**：兼容层**不直接解引用用户指针**，一律走
  `arkrtos_copy_from_user()` / `arkrtos_copy_to_user()`（判据是任务 TCB 的
  `rom_start/rom_size/ram_start/ram_size` 四个窗口字段）。越界返 `-EFAULT`。
  这条纪律是 2026-11-19 那个 bug 的直接产物（§7）。

## 5 开关与零体积

- `ARKRTOS_USE_LINUX_ABI`（`kernelsrc/include/arkrtos_features.h`、
  `config/kconfig/Kconfig.features`），默认 **0**，且 `depends on ARKRTOS_USE_ELF`
  （没有装载器就没有兼容层，features 里有 `#error` 顶住）。
- 默认关时：`arkrtos_linux_abi.c` 整文件不产出任何符号，port 里没有
  `arkrtos_port_linux_syscall_num`，`arkrtos_hal.h` 只剩空宏。**体积与行为零变化**，
  这一条在门禁里是双向实测的（关档 `.o` 的符号表必须全空）。
- `arkrtos_autoconf.h` 由 `tools/kconfig/genconfig.py` 从 `config/arkrtos_defconfig`
  生成，新增 Kconfig 项后必须重跑一次，否则默认档读数里看不到这个开关。

## 6 门禁：`tools/linux_abi_gate.py`

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
| `compile-on` | LA64 真板级下 5 个 TU 全编过（含用户程序本身） |
| `syscall-table` | 15 个号与 `asm-generic` 逐条相等 |
| `abi-split-la64` | 立即数 == 0 是 Linux、!= 0 是 Ark |
| `abi-split-rv32` | `0x10..0x22` 归 Ark，Linux 最小号 56 不撞 |
| `build` | `make -f example/qemu_loongarch64/Makefile` 通过 |
| `qemu-readout` | 14 条 check 全 ok + PASS 行 + `exit code 7` + 旁路演示未受影响 |
| `parser-selfcheck` | 把 4 种坏日志喂回判据，必须全判红 |

`parser-selfcheck` 是防「绿得没有信息量」的：去掉 PASS 行、退出码改 3、插一条
FAIL、少一条 check，四种都必须被判红。

## 7 本轮踩到的两个真 bug 与一个读数坑

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

**③ 读数坑：半行被抢占，看起来像「少了一条读数」。**
加锁前实测到 `[linux] write ... rc=0` 之后直接是 `[l`，而剩下的
`inux] exec rc=0` 出现在**程序跑完之后**——不是丢字节，是**打印被任务切换打断**：
`exec` 之后用户任务一开跑就抢走 CPU，演示任务的一行被打成两截。修法是在
`elf_demo.c` 里把整行打印放进调度器锁（`arkrtos_sched_lock_internal()` /
`arkrtos_sched_unlock_internal()`），改完 3/3 连跑稳定。判据侧另有两处：
读到 marker 前缀就 kill 会把 `exit code 7` 截掉（加 0.5 s 宽限）；`-serial file:`
后端在进程被 kill 时丢不满 4 KB 的尾块（改管道）。

## 8 边界

- 这一层的定位是「**能跑通静态程序的最小系统调用集合**」，不是 Linux 兼容层
  的完整实现。没有 ptrace、没有 `/proc`（Ark 自己的 procfs 另算）。ELF interpreter
  另有一档：`ARKRTOS_USE_DYNLOAD` 打开后走自研 `ld-ark.so`（见
  [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但那是「能跑自研夹具」，不是
  「能跑 glibc 产物」。
- Real-time 语义不因兼容层改变：Linux ABI 的路径全部在 SVC 里同步完成，
  不额外开线程、不加锁、不引入阻塞点（`read`/`write` 都是到 ramfs 的同步搬运）。
- 下一步（若要用真正的 Linux 工具链产物）：动态链接的内核侧与自研装载器已落地
  （2026-11-20，见 [动态链接与PT_INTERP](动态链接与PT_INTERP.md)），但**离 glibc
  还差整条 libc 兼容面**——futex、rseq、TLS 初始化、文件映射、`mprotect` 真落实。
  剩下的大头是用户栈上的信号帧与「进程」这个对象；在那之前，静态程序 + 这 15 个
  号就是诚实上限。