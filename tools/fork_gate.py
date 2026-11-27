#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Linux fork/clone + wait4（ARKRTOS_USE_LINUX_FORK）的门禁。

为什么需要这一道
----------------
`ARKRTOS_USE_LINUX_FORK` 是本内核第一条「一个进程变成两个」的能力：它要在
已降权的 PLV3 任务上复制整份地址空间、再造一个任务、并且让退出码经 wait4
回到父进程。这条链上有四类假绿，每一类都能编得过、看起来像是对的：

  1. 开关没接线：Kconfig 有一项、features.h 有 `#error`，但源文件没被 `#if`
     包住 —— 默认档悄悄多出一份代码；
  2. 只改了 trap 帧、没有复制窗口：父进程能看到子的写（同一份物理页），
     子进程的 `exit(42)` 也可能"看起来"收到了 —— 这不是 fork，是共享内存；
  3. 退出码是编出来的：wait4 对着一个还没上报的任务回 0 或 SIGKILL 那种
     "看起来权威"的值；
  4. 记账不消耗：同一个号能被反复 reap，或者认不出的号也回一个数。

所以判据全部落在可证伪的读数上：默认档的符号表、开档的真编译、以及 QEMU
上真跑一遍的分父/子读数。

判据（全过才 0）
----------------
  A. 默认档：`arkrtos_autoconf.h` 里 `ARKRTOS_USE_LINUX_FORK` 必须是 0；
     Kconfig 里它是 `depends on ARKRTOS_USE_LINUX_ABI && ARKRTOS_USE_EXIT_CODE`
     且 `depends on ARKRTOS_USE_PAGING_PAGES && ARKRTOS_USE_PAGING_DEMAND`；
     features.h 有四条 `#error`（ABI / EXIT_CODE / 两个分页依赖）。
  B. 关档零符号：用 RV32 工具链把 `arkrtos_linux_abi.c` 编成 .o，不许有
     `lx_clone` / `lx_wait4` / `s_child`；用 LA64 真板级（fork 未开）把
     `arkrtos_elf.c` / `arkrtos_port.c` / `arkrtos_paging.c` 编成 .o，不许有
     `arkrtos_elf_fork` / `arkrtos_port_paging_space_clone` /
     `arkrtos_paging_space_clone`。
  C. 开档编得过：LA64 真板级 + `-DARKRTOS_USE_LINUX_FORK=1
     -DARKRTOS_USE_PAGING_DEMAND=1`，五个内核 TU 与夹具本身全编得过；
     再加一档 `-DARKRTOS_USE_PAGING_COW=1` 同样要全编得过。
  D. QEMU 真读数（六档各跑一遍）：默认 fork 档 `[fork]` 的 27 条 check、
     fork+COW 档 32 条、fork+COW+SHM 档 40 条、fork+COW+SHM+futex 档 56 条、
     fork+…+filemap 档 76 条、fork+…+mprotect 档 87 条
     check 全 ` ok`（一条 FAIL 都不许）、
     `PASS: child ran in its own space, exit 42 reaped`、`exit code 0`；
     `[fork] parent pid=` 与 `[fork] child pid=` 两个数必须不同；并且同一次
     启动里 hello / FS / 其余 Linux 夹具的读数仍在（新开关不许把别人的演示
     搞坏）。三条新读数必须**逐条**出现（只比总条数会在换夹具时假绿）：
       - `blocking wait4 blocked >= 40 ms -> 1 ok`（父真的等过，不是立即返回）
       - `parent read end survived child close -> 3 ok`（fd 表是每进程一份）
       - `SIGCHLD handler ran -> 1 ok`（跨任务信号真的投到了父进程）
     COW 档另加四类内核日志断言：至少一条 `[LA64U] pg cow fork shared=<n>
     stack=<n> copied=0 frames=<n>`（共享了、栈页已私有化、一个字节都没搬）、至少一条
     `[LA64U] pg cow copy idx=<n> ref=2`（写故障时真复制了）、不许有
     `ref=1` 的 copy 行（没共享也去复制就是白搬）、以及
     `cow child saw pre-fork values (bits=3) -> 3 ok`（子进程看到的仍是
     fork 前的值，父改的是它自己那份）。
     FUTEX 档另加：九条 futex 专属读数逐条出现 —— 共享页映射成功、无等待者
     时 WAKE 回 0、值不等时 WAIT 回 -EAGAIN、不认识的 op 回 -ENOSYS、fork 出
     唤醒者、父进程在 WAIT 里真睡着且被叫醒后回 0、父进程看到唤醒者写进去的
     值、唤醒者同时看到「父等的还是 1」与「WAKE 回 1」（位掩码 3）、子进程被
     reap。这九条里最要紧的是「父 WAIT 回 0」：它在 LA64 上暴露过一个只有上板
     才看得见的坑 —— 阻塞返回时 a0（返回值寄存器）被写成 0，而 a0 同时是
     futex 的第一个参数，重放的那一次读到的 uaddr 就是 0。
     再另加六条「多时间片等待」读数（子进程先忙等 250 ms 跨过两个 100 ms
     时间片再写值 + WAKE）：共享页映射、clone>0、跨时间片 WAIT 回 0、父看到
     新值、WAIT 墙钟 >= 150 ms（证明真跨了时间片而不是一次命中）、子进程被
     reap、唤醒者位掩码 3。这组把「重放时 a0 被清成 0」这条真缺陷钉死在调度
     器上：修复前父进程第一个时间片醒来就拿 0 去读字 ⇒ -EFAULT。futex 档合计
     9 + 7 = 16 条，再叠进 filemap / mprotect 两档后是 69 + 7 = 76 与
     80 + 7 = 87 条。
     第五档（条件变量的 PI 变体 WAIT_REQUEUE_PI 11 / CMP_REQUEUE_PI 12）另加
     14 条：单进程五条参数面（值不符 -EAGAIN、目标锁空 -EINVAL、缺 uaddr2
     -EINVAL、交接前父进程 LOCK_PI 成功、CMP 期望值不符 -EAGAIN），跨进程九条
     （clone>0、子进程排队置 WAITERS、CMP 回 1、字里换成子进程 tid、不留
     WAITERS、子进程被 reap、子进程位掩码 7 = 被搬运唤醒 / 字里是自己 / 接手
     来的锁能放掉、解锁后字清 0）。反假绿全落在读数字上：搬运没做时 CMP 回 0、
     字里还是父进程 tid；退化成「叫醒再抢」时子进程 bit1 不成立。
     FILEMAP 档另加：八条文件映射读数逐条出现 —— 首字节是文件首字节、坏 fd
     回 -EBADF、只读 fd 上的可写共享回 -EACCES、offset=4096 读出的是文件
     第二页、EOF 之后是零、可写共享的 msync 回 0、私有映射的 msync 也回 0 但
     不落盘、以及重新映射读回时「共享写生效 + 私有写未生效」的位掩码 31。
     最要紧的是最后一条：只判「共享写生效」的话，把整段直接写回文件也能过。
     SHM 档另加：至少一条 `[LA64U] pg shm map va=<hex> pairs=<n>` 且 pairs>=1
     （共享页必须在 mmap 时刻就有实体；拖到 fork 才让父子各自缺页拿各自的帧，
     共享就退化成两次私有分配 —— 那种实现父子都不报错，只是各改各的），
     以及六条 SHM 专属读数逐条出现：两条非法形态必须回 -ENOSYS（关档/组合
     反假绿）、映射成功、起始清零、父看到子写、子看到父 fork 之后的写。
  D2. LOCK 段（U-27 记录锁跨进程，32 条，十档都编）：三条必现读数
     ——`lock child GETLK pid is not self -> 1 ok`（F_GETLK 真把别人的锁报
     回来）、`lock waiter blocked >= 40 ms -> 1 ok`（F_SETLKW 真挂起了，
     不是失败就立刻返回）、`lock free after child close (conflict range)
     -> 0 ok`（close 按文件释放，不是按进程一刀切）。另外六条读数把
     SEEK_END 换算、不相交区间、l_whence 归一、l_start 绝对化分别钉死。
  D3. SCHED 段（U-28 调度策略十二个号，63 条，十档都编；[sched] 前缀单独
     计数，不进上面十档的 [fork] 常量）：这一段只证「号、参数、错误码真的
     过了内核又回来了」，不证选择次序 —— EDF 谁先跑依赖墙钟与调度抖动，不是
     可证伪读数；选择次序、预算节流、周期补回、过载重新对齐由宿主用例
     test_task_host.c 的 test_deadline_class 用可控 tick 断言，两处读数合起来
     才是完整证据。必现读数九条：`getscheduler default -> 0 ok`（策略镜像从 0
     起）、`getparam FIFO priority is 88 -> 88 ok`（优先级原样回读）、
     `rr interval nsec is 5000000 -> 5000000 ok`（RR 轮转量 = 时间片 10 拍 x
     500 us）、`setattr DEADLINE 100% bandwidth -> EBUSY -> -16 ok`（准入）、
     `setattr DEADLINE 1ms/2ms/100ms is admitted -> 0 ok`、`getattr policy is
     DEADLINE -> 6 ok`、`getattr DEADLINE deadline round-trips -> 2000000 ok`
     （三元组原样回读）、`setaffinity short len -> EINVAL -> -22 ok`、
     `sched_yield -> 0 ok`。另外 54 条把三种策略映射、两处错误码分档
     （-EINVAL / -ESRCH / -EFAULT / -EBUSY）、VER0 48 与 VER1 56 两种 size、
     sched_flags 与系统调用 flags 的非 0 拒绝、DEADLINE 走 setscheduler 回
     -EINVAL、亲和性掩码只认 bit0 逐个钉死。夹具里 deadline 那六条读数按
     ARKRTOS_USE_SCHED_DEADLINE 分两支（类开着期望 EBUSY/三元组，类没开期望
     -EINVAL/零值），两支条数相同，所以 SCHED_CHECKS=63 不随档变化。
  D4. SIGIO 段（U-29 异步 I/O：F_SETOWN / F_GETOWN / O_ASYNC，24 条，十档
     都编；[sio] 前缀单独计数，不进上面十档的 [fork] 常量）：照 D3 的做法只证
     「登记、开关、投递真的过了内核」，不证投递时机（那由宿主用例用可控 tick
     断言）。必现读数十条：`getown on console is 0 -> 0 ok`（控制台本体没有
     可登记的主人）、`setown on console -> EINVAL -> -22 ok`、`getown reads
     back the task id -> <pid> ok`（F_SETOWN 0 = 认给自己）、`setown process
     group -> EINVAL -> -22 ok`（本层不做进程组，必须如实拒）、`getfl reports
     O_ASYNC -> 1 ok`、`dup shares the owner -> <pid> ok` 与 `dup shares
     O_ASYNC -> 1 ok`（同一份 file description 双向共享）、`SIGIO handler ran
     -> 1 ok` 与 `SIGIO signo is 29 -> 29 ok`（投递与信号号）、`clearing
     O_ASYNC stops delivery -> 0 ok`（关掉开关后命中数不变）、`close writer
     (EOF) sends SIGIO -> 1 ok`（EOF 也算「变可读」）。默认动作（不装 handler
     = 结束任务）不在这里做——会把任务中途杀掉，由宿主用例断言。
  E. 解析器自检：把合成坏日志喂给同一个判据（去掉 PASS、改退出码、插 FAIL、
     少一条 check、父子 pid 改成相同、去掉 exec rc=0），每种坏法都必须判红。

覆盖：fork 的返回值分流与地址空间复制、退出码经 wait4 回到父、记账是消耗品、
越界参数拒绝、**阻塞 wait4**（options=0，父进程真等到子进程退出）、
**fd 表按进程一份**（子 close 不影响父的读端）、**管道对象共享且引用计数正确**
（子写的数据父读得到）、**跨任务 SIGCHLD**（子退出后父的 handler 在下一次系统
调用返回前被调用）。

覆盖（记录锁部分）：跨进程冲突（F_SETLK 回 -EAGAIN）、F_GETLK 报别人的锁
（类型 / 进程号 / 区间 / whence）、F_SETLKW 的挂起与唤醒、SEEK_END 换算、
不相交区间各自成锁、close 按文件释放全部锁。

不覆盖：vfork / CLONE_VM 线程（无共享地址空间，一律 -EINVAL）、CLONE_FILES
（每进程 fd 表是 fork 语义的一部分，共享 fd 表这种形态本门禁不为它背书）。
MAP_FIXED 已覆盖（按需分页档的「同址丢弃旧内容」；非对齐回 -EINVAL、
MAP_FIXED|MAP_SHARED 回 -ENOSYS 两条反假绿也在）。文件映射已由第五档覆盖，但**不是** Linux 的页缓存
语义：每个进程各拿各的帧，写回（msync / munmap / 退出）之后才经文件路径互相
可见；没有 shm_open、没有超过 8 KB 的文件（写回要整文件进内核缓冲）。

用法
----
    py -3 tools/fork_gate.py
    py -3 tools/fork_gate.py --no-build --keep
    py -3 tools/fork_gate.py --qemu <path> --la64-gcc <path> --rv32-gcc <path>

退出码 0 = 全过；1 = 任一判据不成立 / 缺工具链与 QEMU（缺工具是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = "example/qemu_loongarch64/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"

# U-28 的 sched 十二号（118-127、274/275）不另开一档：它与 fork 档正交，
# 十档都带上 —— 否则「换档时 [sched] 读数整段消失」会被当成正常。
SCHED_FLAGS = ["-DARKRTOS_USE_LINUX_SCHED=1", "-DARKRTOS_USE_SCHED_DEADLINE=1"]
# U-29 的异步 I/O（F_SETOWN / F_GETOWN / O_ASYNC / SIGIO）同样与 fork 档正交，
# 十档都带上：否则换档时 [sio] 读数整段消失也会被当成正常。
SIGIO_FLAGS = ["-DARKRTOS_USE_LINUX_SIGIO=1"]
FORK_FLAGS = ["-DARKRTOS_USE_LINUX_FORK=1", "-DARKRTOS_USE_PAGING_DEMAND=1"] + \
    SCHED_FLAGS + SIGIO_FLAGS
# COW 档 = fork 档 + 写时复制开关。本门禁跑两遍：先默认 fork 档（27 条），
# 再 fork+COW 档（32 条 + 内核 COW 日志）。绝不把 FORK_FLAGS 直接改成带
# COW —— 那样默认 fork 路径（整块克隆）就再也没被测过了。
COW_FLAGS = FORK_FLAGS + ["-DARKRTOS_USE_PAGING_COW=1"]
# SHM 档 = COW 档 + MAP_SHARED 匿名共享页，且必须叠在 COW 档上：共享位的账本
# 就是 COW 档的引用计数表，非 COW 克隆会把每一对实体复制，共享自然消失。
SHM_FLAGS = COW_FLAGS + ["-DARKRTOS_USE_PAGING_SHM=1"]
# FUTEX 档 = SHM 档 + futex WAIT/WAKE。叠在 SHM 档上而不是独立档：futex 的键
# 是「物理帧 + 页内偏移」，没有共享页就没有跨进程的同一个字可等。
FUTEX_FLAGS = SHM_FLAGS + ["-DARKRTOS_USE_LINUX_FUTEX=1"]
# FILEMAP 档 = FUTEX 档 + 文件映射与写回（U-21）。叠在分页链上而不是独立档：
# 映射的内容要靠按需缺页拿帧装进去，没有 DEMAND 就没有帧。夹具多 20 条 check
# （其中 SHM 段的一条反假绿在开档时换成 -EBADF，见夹具里那段注释）。
FILEMAP_FLAGS = FUTEX_FLAGS + ["-DARKRTOS_USE_PAGING_FILEMAP=1"]
# MPROTECT 档 = FILEMAP 档 + 页级写保护（U-22）。叠在最上面而不是独立档：保护位
# 写在 PTE 上，而 PTE 只在分页链里存在；夹具这一段还要用 COW 把「子进程写只读页」
# 造出来，所以下面三档必须同时开。夹具多 11 条 check（63 -> 74）。
MPROTECT_FLAGS = FILEMAP_FLAGS + ["-DARKRTOS_USE_PAGING_MPROTECT=1"]
# SELECT 档 = MPROTECT 档 + pselect6 跨 fd 就绪等待（U-22）。叠在最上面是因为
# 夹具这一段要用管道对象与普通文件描述符一起读就绪位；管道虽然不依赖分页，
# 但夹具的后半段整段都在分页链上，单独造一档会让「就绪判据」和「fd 表」
# 各自只在一半场景里被测过。夹具多 16 条 check。
SELECT_FLAGS = MPROTECT_FLAGS + ["-DARKRTOS_USE_LINUX_SELECT=1"]
# SOCKET 档 = SELECT 档 + socket 翻译层 + lwIP 回环（U-23）。叠在 select 档上
# 是因为夹具的「等协议栈起来」用 pselect6 的空超时当睡眠，而且 fd 表、重放
# 槽、任务身份这些前置正好都是 select 档已经在跑的同一套对象。lwIP 开着但
# 不接 virtio 网卡：socket 走 127.0.0.1 回环，包不出芯片。夹具多 37 条 check。
SOCKET_FLAGS = SELECT_FLAGS + ["-DARKRTOS_USE_LINUX_SOCKET=1",
                               "-DARKRTOS_USE_LWIP=1"]
# THREAD 档 = SOCKET 档 + CLONE_THREAD（U-25）。叠在 socket 档上而不是独立档：
# 线程要验的「共享地址空间 + 同一张 fd 表 + 共享进程槽」正好是 socket 档已经
# 在用的同一套对象；而且线程退出路径里「不是最后一个就别拆 fd 表」只有在
# fd 表里真有东西（socket）时才看得见差别。夹具多 25 条 check。
THREAD_FLAGS = SOCKET_FLAGS + ["-DARKRTOS_USE_LINUX_THREAD=1"]
# QUOTA 档 = THREAD 档 + 每 App 任务配额（SEC-G07）。叠在线程档上而不是独立
# 档：配额的三条干净读数全靠「线程不占新地址空间」把「配额用完」与「空间用完」
# 分开，没有线程档就分不开（ERR_QUOTA 与 ERR_BUSY 都映射成 -EAGAIN）。
# 上限取 3：父 + 1 子 + 1 线程刚好压满，同时高于线程档自己的峰值 2（父 + 1
# 线程），两段互不干扰。板级 PAGING_APPS_MAX 是 2，并发进程最多两个。
QUOTA_FLAGS = THREAD_FLAGS + ["-DARKRTOS_USE_APP_QUOTA=1",
                              "-DARKRTOS_APP_MAX_TASKS=3"]

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
RV32_FALLBACKS = [
    "D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe",
]
QEMU_CANDIDATES = (
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
RV32_ARCH = ["-march=rv32imac", "-mabi=ilp32", "-mcmodel=medany", "-mno-relax"]

COMMON = [
    "-O2",
    "-Wall",
    "-ffreestanding",
    "-fno-common",
    "-nostdlib",
    "-nostartfiles",
    "-fno-builtin",
    "-fsyntax-only",
    "-DARKRTOS_SIGN_KEY_LOCAL=1",
]

SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

# 夹具（elf/linux_fork.c）里的 check() 条数：
#   第一段（继承的 fork 语义）10 条：两条 -EINVAL、child pid > 0、
#   child pid != parent、wait4 返回子号、WEXITSTATUS==42、WIFEXITED、
#   父 marker 未被改、reap 后 -ECHILD、认不出的号 -ECHILD；
#   第二段（阻塞 wait4 / fd 继承）7 条：pipe2、clone>0、阻塞 wait4 得子号、
#   子退出码 7、阻塞时长 >= 40 ms、父读端存活、子写的数据到达；
#   第三段（跨任务 SIGCHLD）4 条：装 handler、handler 被调到、信号号 17、
#   回收退出码 9；
#   第四段（真 mmap 语义 MAP_FIXED）6 条：先 mmap、同址 MAP_FIXED 返回同址、
#   旧内容被丢掉（两个标记字节都变 0）、之后仍可写、非对齐回 -EINVAL、
#   MAP_SHARED 形态回 -ENOSYS；
#   第五段（阻塞管道跨执行流，U-26）20 条：A 组（父的阻塞读等到子进程写）
#   pipe2 / clone>0 / 读回 3 / 内容 / 等满 40 ms / 子退出后读回 EOF /
#   EOF 不多睡一个时间片 / 子被 reap / 子退出码 11；B 组（父的阻塞写等到
#   子进程排空）pipe2 / 状态管道 pipe2 / 填满 256 / clone>0 / 写全 300 /
#   等满 40 ms / 子被 reap / 子退出码 13 / 子回传 5 字节 / 子共读 556 /
#   逐字节图案一致 —— 合计 47 条。
# LOCK 段（U-27 记录锁跨进程，32 条，不随档变化）：十档跑的是同一个夹具
# 二进制，这一段全都在，所以上面每个期望值都是「原段落合计 + LOCK_CHECKS」。
# 逐条是：两根管子的 pipe2、探测文件打开、父 F_SETLK / 自己的锁不算冲突、
# fork、子 SETLK 撞父的锁回 -EAGAIN、子不相交 SETLK、子 SEEK_END SETLK、
# 父用 SEEK_SET 问不相交段（rc / l_pid 是子进程 / l_start=200）、父用
# SEEK_SET 问文件尾段（rc / l_pid 是子进程，这条同时钉住 SEEK_END 换算）、
# 子 F_GETLK 读父的锁（rc / 类型 / l_pid 不是自己 / 绝对 start / len /
# whence 归一成 SEEK_SET）、父解锁、子 F_SETLKW 回 0 且墙钟 >= 40 ms、
# 父看到锁的主人是子进程（rc / l_pid）、子 close、父把三把锁逐个拿回来
# （冲突段 / 不相交段 / 文件尾段）、子被 reap、子退出码 0。
LOCK_CHECKS = 32
# SCHED 段（U-28，十档都在，与 LOCK 段同型）：63 条读数。夹具里 deadline 的
# 六条读数按 ARKRTOS_USE_SCHED_DEADLINE 分两支（类开着期望 EBUSY/三元组，
# 类没开期望 -EINVAL/零值），两支条数相同，所以这个常数不随档变化。
SCHED_CHECKS = 63
# SIGIO 段（U-29，十档都在，与 LOCK/SCHED 段同型）：23 条读数。夹具里没有
# 默认动作（不装 handler = 结束任务）那一条 —— 它会中途杀掉任务，读数全丢；
# 那一条由宿主用例 test_linux_abi_host.c 的 t_async_io 第 7 节断言。
SIGIO_CHECKS = 24
EXPECT_FORK_CHECKS = 47 + LOCK_CHECKS
# COW 段多 5 条：pipe2、clone>0、子被 reap、子看到 fork 前的值（位掩码 3）、
# 父自己的写还在。合计 52 条。
EXPECT_COW_CHECKS = 52 + LOCK_CHECKS
# SHM 段多 8 条：两条非法形态回 -ENOSYS（反假绿）、匿名共享映射成功、起始清零、
# clone>0、父看到子写、子被 reap、子看到父 fork 之后的写。合计 60 条。
EXPECT_SHM_CHECKS = 60 + LOCK_CHECKS
# FUTEX 段多 9 条：共享页映射、无等待者 WAKE==0、值不等 WAIT==-EAGAIN、
# 未知 op==-ENOSYS、clone>0、被叫醒的 WAIT==0、父看到新值、子被 reap、
# 唤醒者位掩码 3（看到 1 且 WAKE 回 1）。
# 多时间片段再 7 条：共享页映射、clone>0、跨时间片 WAIT==0、父看到新值、
# WAIT 墙钟 >= 150 ms、子被 reap、唤醒者位掩码 3。合计 49 + 7 = 56 条。
# 第二档（位集 / 超时 / 改挂）再 20 条：单进程 9 条参数与语义（位集 0 两条
# -EINVAL、无等待者 WAKE_BITSET==0、相对 0 超时 -ETIMEDOUT、绝对时刻已过
# -ETIMEDOUT、无等待者 REQUEUE==0、CMP_REQUEUE 期望值不符 -EAGAIN、缺
# uaddr2 -EINVAL、mmap）、相对 50 ms 超时 2 条（返回值 + 墙钟 >= 50 ms）、
# 位集跨进程 4 条、改挂跨进程 5 条。合计 56 + 20 = 76 条。
# 第四档（PI 三档 LOCK_PI/UNLOCK_PI/TRYLOCK_PI + robust list 99/100 与退出
# 清理）再 37 条：单进程 PI 语义 15 条 + robust 参数/回读 8 条、跨进程 PI 与
# PI robust 死亡交接 7 条、非 PI robust 7 条。76 + 37 = 113 条。
# 阻塞管道跨执行流段（不随档变化）多 20 条：113 + 20 = 133 条... 实测
# 127 + 20 = 147 条（基线 127 已含此前的全部段）。
EXPECT_FUTEX_CHECKS = 147 + LOCK_CHECKS
# FILEMAP 段多 20 条：只读私有映射的内容三处读数（首字节 / 第 25 / 第 4095）、
# 三条参数校验（未对齐 offset -EINVAL、坏 fd -EBADF、只读 fd 上的可写共享
# -EACCES）、offset=4096 的映射与 EOF 补零、O_RDWR 打开、可写共享映射与
# msync/munmap 两次写回、私有可写映射与它不落盘、以及最后把「共享写落盘 +
# 私有写不落盘」合成一个位掩码读数（20 % 26 那对字节）。合计 69 条；
# 再叠 futex 档的 15 条（含多时间片 7 条）= 76 条。
# 再加 futex 第四档的 37 条：96 + 37 = 133 条；再加第五档（条件变量 PI 变体）
# 的 14 条：133 + 14 = 147 条；再加阻塞管道段 20 条 = 167 条。
EXPECT_FILEMAP_CHECKS = 167 + LOCK_CHECKS
# MPROTECT 段多 10 条：8 KB 映射、mprotect(RO)==0、只读页仍可读、**同一对里的
# 另一半 4 KB 仍可写**（4 KB 粒度与 8 KB 粒度在此分家）、mprotect(RW)==0、
# 恢复保护后写回得去、再设 RO、为违规写 fork 出子进程、子进程被摘掉（不是重启）、
# 父进程那一页一个字节没变、状态字是「被信号终止」而不是 exit 码。
# 合计 80 条；再叠 futex 档的 15 条（含多时间片 7 条）= 87 条。
# 再加 futex 第四档的 37 条：107 + 37 = 144 条；再加第五档的 14 条：158 条；
# 再加阻塞管道段 20 条 = 178 条。
EXPECT_MPROTECT_CHECKS = 178 + LOCK_CHECKS
# SELECT 段多 15 条：pipe2、空管道读端不就绪（返回值 0）、非就绪时用户集被清位、
# 50 ms 定时等待回 0、定时等待墙钟 >= 40 ms、写进 3 字节、读端就绪（1）、
# 就绪位在用户集里保留、写端有空间就绪、stdout 恒可写、写满后写端不就绪、
# 写端全关后读端按 EOF 就绪、exceptfds 永不上报（0）、exceptfds 被清位、
# 坏 fd 回 -EBADF。合计 107 + 15 = 122 条。
# 再加 1 条反假绿：socket 开关没开时 198 号必须如实回 -ENOSYS（这一条只在
# select 档里编，socket 档自己不编，所以 socket 档仍然从 122 起算）。
# 再加 futex 第四档的 37 条：123 + 37 = 160 条；再加第五档的 14 条：174 条；
# 再加阻塞管道段 20 条 = 194 条。
EXPECT_SELECT_CHECKS = 194 + LOCK_CHECKS
# SOCKET 段多 37 条：等协议栈、stream/dgram 两个 fd、坏 domain 与坏 type
# 两个 errno、bind/listen/getsockname（addrlen 16、family、端口 8081）、
# connect、accept（fd + 写回 family）、TCP 收发与载荷逐字节（前 4 字节打包
# + 末字节）、getpeername 的 127.0.0.1、SO_REUSEADDR、SO_TYPE 与 SO_ERROR
# 各自的 rc 与本地值、未知选项 -ENOPROTOOPT、写 SO_ERROR -EINVAL、UDP
# bind/sendto/recvfrom/载荷/对端端口一致、shutdown 后对端 EOF、关闭后
# read/write 回 -EBADF。合计 122 + 38 + 1 = 161 条（最后一条是「一次 socket
# 往返不按一个 100 ms 等待时间片结算」的性能回归门）。
# 再加 futex 第四档的 37 条：161 + 37 = 198 条；再加第五档的 14 条：212 条；
# 再加阻塞管道段 20 条 = 232 条。
# 再加 socket 的 dup（U-30）10 条：dup 给新号、两个号不同、关副本、关副本之后
# 源仍然问得出 8081（不通知协议栈）、第二次 dup、关源、副本接管句柄仍然问得出
# 8081、最后一份关掉才真关（原来那条裸 close 变成带判据的一条）。232 + 10 = 242。
EXPECT_SOCKET_CHECKS = 242 + LOCK_CHECKS
# THREAD 段多 25 条：clone(CLONE_THREAD)>0、PARENT_SETTID 写回、CLEARTID 上的
# futex WAIT 回 0、CLEARTID 被清 0、线程真跑了、CHILD_SETTID 看到自己的 tid、
# tid != 父 tid、线程 getpid == tgid、父 getpid 未变、SETTLS 落到 $tp、两栈
# 不同址、父的局部变量还在、同组第二个线程 clone>0、它退出后 CLEARTID 也清 0、
# 两个线程都退之后进程仍活、四条参数门槛（缺 CLONE_VM / child_stack=0 /
# 未对齐 / CLONE_VFORK 各 -EINVAL）、线程退出之后还能 fork（父进程 fd 表没被
# 提前拆）与被 reap 的退出码 5、线程发起 exit_group 整组被 reap 且码 6、
# 组长发起 exit_group 整组被 reap 且码 7。合计 161 + 25 = 186 条。
# 再加 futex 第四档的 37 条（实测 231，含 thread 段自己多出的 8 条）；
# 再加第五档的 14 条：245 条；再加阻塞管道段 20 条 = 265 条；再加 U-30 的
# 10 条 = 275 条。
EXPECT_THREAD_CHECKS = 275 + LOCK_CHECKS
# QUOTA 段多 11 条（上限 3 = 父 + 1 子 + 1 线程）：建管子、上限内 fork 成功、
# 线程把最后一格占掉、超限 fork==-EAGAIN、超限线程==-EAGAIN、被阻塞的子进程
# 真被 reap、回收后第二个线程成功、**空间空闲时 fork 仍被拒**（全段最干净的
# 一条：不可能是 ERR_BUSY）、第三个线程被拒、两个线程按 CHILD_CLEARTID 归队、
# 额度回来后立刻能再起且退出码照传。合计 186 + 12 = 198 条。
# 再加 futex 第四档的 37 条（实测 243）；再加第五档的 14 条：257 条；
# 再加阻塞管道段 20 条 = 277 条；再加 U-30 的 10 条 = 287 条。
EXPECT_QUOTA_CHECKS = 287 + LOCK_CHECKS
# 上面这一档的每一段都对应一类「看起来对」的实现：另开地址空间（共享全局读
# 不到）、每线程一份 fd 表（fork 段留不住）、线程退出就把进程拆了（进程活不
# 下来）、把线程当进程挂到 wait4 上（exit_group 码不对）。

FAILS = []
PASSES = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-24s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-24s %s" % (name, detail))


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def read_text_bytes(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _pick(cands, given, what):
    if given:
        if os.path.isfile(given):
            return given
        bad("toolchain", "%s 不存在：%s" % (what, given))
        return None
    for c in cands:
        if os.path.isfile(c):
            return c
    bad("toolchain", "找不到 %s（缺工具链是 FAIL，不是跳过）" % what)
    return None


def nm_of(cc):
    d = os.path.dirname(cc)
    base = os.path.basename(cc)
    for suffix in ("-gcc.exe", "-gcc"):
        if base.endswith(suffix):
            for tail in ("-nm.exe", "-nm"):
                cand = os.path.join(d, base[: -len(suffix)] + tail)
                if os.path.isfile(cand):
                    return cand
    for name in os.listdir(d):
        if name.endswith("-nm.exe") and "newlib" in name:
            return os.path.join(d, name)
    return None


def run(cc, args):
    r = subprocess.run([cc] + args, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    out = (r.stdout or b"") + (r.stderr or b"")
    return r.returncode, read_text_bytes(out)


def includes(board, port):
    return [
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/" + board,
        "-Ikernelsrc/port/" + port,
    ]


def board_flags(board, extra=None):
    f = [
        "-DARKRTOS_SIGN_KEY_LOCAL=1",
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
    ]
    if extra:
        f += extra
    return f


# ---------------------------------------------------------------- A 默认档

def check_default_off():
    ac = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_FORK\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_FORK（开关没进 Kconfig 生成物）")
    elif m.group(1) != "0":
        bad("default-off", "默认档 ARKRTOS_USE_LINUX_FORK=%s（必须是 0）" % m.group(1))
    else:
        ok("default-off", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_FORK 0")

    kc = read_text(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    i = kc.find("config ARKRTOS_USE_LINUX_FORK")
    if i < 0:
        bad("kconfig-wired", "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_FORK")
    else:
        # 只截这一段：到下一个 config/menuconfig 为止，避免把后面的项算进来。
        tail = kc[i + len("config ARKRTOS_USE_LINUX_FORK"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        depends = " ".join(ln for ln in blk.splitlines()
                           if ln.strip().startswith("depends on"))
        needs = [
            "ARKRTOS_USE_LINUX_ABI",
            "ARKRTOS_USE_EXIT_CODE",
            "ARKRTOS_USE_PAGING_PAGES",
            "ARKRTOS_USE_PAGING_DEMAND",
        ]
        miss = [n for n in needs if n not in depends]
        if miss:
            bad("kconfig-wired", "Kconfig 项的 depends 里缺 %s" % "、".join(miss))
        else:
            ok("kconfig-wired", "Kconfig 项存在，四条依赖都在 depends 里")

    fh = read_text(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    guards = [
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_LINUX_ABI=1",
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_EXIT_CODE=1",
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_PAGING_PAGES=1",
        "ARKRTOS_USE_LINUX_FORK=1 requires ARKRTOS_USE_PAGING_PAGES=1 and ARKRTOS_USE_PAGING_DEMAND=1",
    ]
    miss = [g for g in guards if g not in fh]
    if miss:
        bad("features-guard", "features.h 缺 %d 条 #error（第一缺：%s）" % (len(miss), miss[0]))
    else:
        ok("features-guard", "features.h 四条 #error 都在")

    m = re.search(r"^#define\s+ARKRTOS_USE_PAGING_SHM\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-shm", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_PAGING_SHM")
    elif m.group(1) != "0":
        bad("default-off-shm", "默认档 ARKRTOS_USE_PAGING_SHM=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-shm", "arkrtos_autoconf.h: ARKRTOS_USE_PAGING_SHM 0")

    i = kc.find("config ARKRTOS_USE_PAGING_SHM")
    if i < 0:
        bad("kconfig-wired-shm", "Kconfig.features 里没有 config ARKRTOS_USE_PAGING_SHM")
    else:
        tail = kc[i + len("config ARKRTOS_USE_PAGING_SHM"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_PAGING_COW" not in blk:
            bad("kconfig-wired-shm",
                "Kconfig 的 SHM 项没有 depends on ARKRTOS_USE_PAGING_COW")
        else:
            ok("kconfig-wired-shm", "Kconfig 项存在，depends on ARKRTOS_USE_PAGING_COW")

    g = "ARKRTOS_USE_PAGING_SHM=1 requires ARKRTOS_USE_PAGING_COW=1"
    if g not in fh:
        bad("features-guard-shm", "features.h 缺 `#error \"%s\"`" % g)
    else:
        ok("features-guard-shm", "features.h 的 SHM 依赖 #error 在")

    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_FUTEX\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-futex", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_FUTEX")
    elif m.group(1) != "0":
        bad("default-off-futex", "默认档 ARKRTOS_USE_LINUX_FUTEX=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-futex", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_FUTEX 0")

    i = kc.find("config ARKRTOS_USE_LINUX_FUTEX")
    if i < 0:
        bad("kconfig-wired-futex", "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_FUTEX")
    else:
        tail = kc[i + len("config ARKRTOS_USE_LINUX_FUTEX"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_LINUX_ABI" not in blk:
            bad("kconfig-wired-futex",
                "Kconfig 的 futex 项没有 depends on ARKRTOS_USE_LINUX_ABI")
        else:
            ok("kconfig-wired-futex", "Kconfig 项存在，depends on ARKRTOS_USE_LINUX_ABI")

    g = "requires ARKRTOS_USE_LINUX_ABI=1"
    if re.search(r'#error\s+"ARKRTOS_USE_LINUX_FUTEX=1\s+requires ARKRTOS_USE_LINUX_ABI=1"',
                 fh) is None:
        bad("features-guard-futex", "features.h 缺 futex 对 LINUX_ABI 的 #error")
    else:
        ok("features-guard-futex", "features.h 的 futex 依赖 #error 在")

    m = re.search(r"^#define\s+ARKRTOS_USE_PAGING_FILEMAP\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-filemap", "arkrtos_autoconf.h 里没有 ARKRTOS_USE_PAGING_FILEMAP")
    elif m.group(1) != "0":
        bad("default-off-filemap", "默认档 ARKRTOS_USE_PAGING_FILEMAP=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-filemap", "arkrtos_autoconf.h: ARKRTOS_USE_PAGING_FILEMAP 0")

    i = kc.find("config ARKRTOS_USE_PAGING_FILEMAP")
    if i < 0:
        bad("kconfig-wired-filemap",
            "Kconfig.features 里没有 config ARKRTOS_USE_PAGING_FILEMAP")
    else:
        tail = kc[i + len("config ARKRTOS_USE_PAGING_FILEMAP"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        needs = ["ARKRTOS_USE_LINUX_ABI", "ARKRTOS_USE_PAGING",
                 "ARKRTOS_USE_PAGING_PAGES", "ARKRTOS_USE_PAGING_DEMAND"]
        miss = [n for n in needs if n not in blk]
        if miss:
            bad("kconfig-wired-filemap",
                "Kconfig 的 filemap 项 depends 里缺 %s" % "、".join(miss))
        else:
            ok("kconfig-wired-filemap",
               "Kconfig 项存在，四条依赖（ABI / PAGING / PAGES / DEMAND）都在")

    guards = [
        "ARKRTOS_USE_PAGING_FILEMAP=1 requires ARKRTOS_USE_LINUX_ABI=1",
        "ARKRTOS_USE_PAGING_FILEMAP=1 requires ARKRTOS_USE_PAGING=1",
        "ARKRTOS_USE_PAGING_FILEMAP=1 requires ARKRTOS_USE_PAGING_PAGES=1",
        "ARKRTOS_USE_PAGING_FILEMAP=1 requires ARKRTOS_USE_PAGING_DEMAND=1",
    ]
    miss = [g for g in guards if g not in fh]
    if miss:
        bad("features-guard-filemap",
            "features.h 缺 %d 条 filemap #error（第一缺：%s）" % (len(miss), miss[0]))
    else:
        ok("features-guard-filemap", "features.h 四条 filemap #error 都在")

    m = re.search(r"^#define\s+ARKRTOS_USE_PAGING_MPROTECT\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-mprotect",
            "arkrtos_autoconf.h 里没有 ARKRTOS_USE_PAGING_MPROTECT")
    elif m.group(1) != "0":
        bad("default-off-mprotect", "默认档 ARKRTOS_USE_PAGING_MPROTECT=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-mprotect", "arkrtos_autoconf.h: ARKRTOS_USE_PAGING_MPROTECT 0")

    i = kc.find("config ARKRTOS_USE_PAGING_MPROTECT")
    if i < 0:
        bad("kconfig-wired-mprotect",
            "Kconfig.features 里没有 config ARKRTOS_USE_PAGING_MPROTECT")
    else:
        tail = kc[i + len("config ARKRTOS_USE_PAGING_MPROTECT"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        needs = ["ARKRTOS_USE_LINUX_ABI", "ARKRTOS_USE_PAGING",
                 "ARKRTOS_USE_PAGING_PAGES", "ARKRTOS_USE_PAGING_DEMAND"]
        miss = [n for n in needs if n not in blk]
        if miss:
            bad("kconfig-wired-mprotect",
                "Kconfig 的 mprotect 项 depends 里缺 %s" % "、".join(miss))
        else:
            ok("kconfig-wired-mprotect",
               "Kconfig 项存在，四条依赖（ABI / PAGING / PAGES / DEMAND）都在")

    guards = [
        "ARKRTOS_USE_PAGING_MPROTECT=1 requires ARKRTOS_USE_LINUX_ABI=1",
        "ARKRTOS_USE_PAGING_MPROTECT=1 requires ARKRTOS_USE_PAGING=1",
        "ARKRTOS_USE_PAGING_MPROTECT=1 requires ARKRTOS_USE_PAGING_PAGES=1",
        "ARKRTOS_USE_PAGING_MPROTECT=1 requires ARKRTOS_USE_PAGING_DEMAND=1",
    ]
    miss = [g for g in guards if g not in fh]
    if miss:
        bad("features-guard-mprotect",
            "features.h 缺 %d 条 mprotect #error（第一缺：%s）"
            % (len(miss), miss[0]))
    else:
        ok("features-guard-mprotect", "features.h 四条 mprotect #error 都在")

    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_SELECT\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-select",
            "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_SELECT")
    elif m.group(1) != "0":
        bad("default-off-select", "默认档 ARKRTOS_USE_LINUX_SELECT=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-select", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_SELECT 0")

    i = kc.find("config ARKRTOS_USE_LINUX_SELECT")
    if i < 0:
        bad("kconfig-wired-select",
            "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_SELECT")
    else:
        tail = kc[i + len("config ARKRTOS_USE_LINUX_SELECT"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_LINUX_ABI" not in blk:
            bad("kconfig-wired-select",
                "Kconfig 的 select 项没有 depends on ARKRTOS_USE_LINUX_ABI")
        else:
            ok("kconfig-wired-select",
               "Kconfig 项存在，depends on ARKRTOS_USE_LINUX_ABI")

    if re.search(r'#error\s+"ARKRTOS_USE_LINUX_SELECT=1\s+requires '
                 r'ARKRTOS_USE_LINUX_ABI=1"', fh) is None:
        bad("features-guard-select", "features.h 缺 select 对 LINUX_ABI 的 #error")
    else:
        ok("features-guard-select", "features.h 的 select 依赖 #error 在")

    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_SOCKET\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-socket",
            "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_SOCKET")
    elif m.group(1) != "0":
        bad("default-off-socket", "默认档 ARKRTOS_USE_LINUX_SOCKET=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-socket", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_SOCKET 0")

    i = kc.find("config ARKRTOS_USE_LINUX_SOCKET")
    if i < 0:
        bad("kconfig-wired-socket",
            "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_SOCKET")
    else:
        tail = kc[i + len("config ARKRTOS_USE_LINUX_SOCKET"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        miss = [n for n in ("ARKRTOS_USE_LINUX_ABI", "ARKRTOS_USE_LWIP")
                if n not in blk]
        if miss:
            bad("kconfig-wired-socket",
                "Kconfig 的 socket 项 depends 里缺 %s" % "、".join(miss))
        else:
            ok("kconfig-wired-socket",
               "Kconfig 项存在，depends on ABI 与 LWIP 都在")

    guards = [
        "ARKRTOS_USE_LINUX_SOCKET=1 requires ARKRTOS_USE_LINUX_ABI=1",
        "ARKRTOS_USE_LINUX_SOCKET=1 requires ARKRTOS_USE_LWIP=1",
    ]
    miss = [g for g in guards if g not in fh]
    if miss:
        bad("features-guard-socket",
            "features.h 缺 %d 条 socket #error（第一缺：%s）"
            % (len(miss), miss[0]))
    else:
        ok("features-guard-socket", "features.h 两条 socket #error 都在")

    m = re.search(r"^#define\s+ARKRTOS_USE_LINUX_THREAD\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-thread",
            "arkrtos_autoconf.h 里没有 ARKRTOS_USE_LINUX_THREAD")
    elif m.group(1) != "0":
        bad("default-off-thread", "默认档 ARKRTOS_USE_LINUX_THREAD=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-thread", "arkrtos_autoconf.h: ARKRTOS_USE_LINUX_THREAD 0")

    i = kc.find("config ARKRTOS_USE_LINUX_THREAD")
    if i < 0:
        bad("kconfig-wired-thread",
            "Kconfig.features 里没有 config ARKRTOS_USE_LINUX_THREAD")
    else:
        tail = kc[i + len("config ARKRTOS_USE_LINUX_THREAD"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_LINUX_FORK" not in blk:
            bad("kconfig-wired-thread",
                "Kconfig 的 thread 项没有 depends on ARKRTOS_USE_LINUX_FORK")
        else:
            ok("kconfig-wired-thread",
               "Kconfig 项存在，depends on ARKRTOS_USE_LINUX_FORK")

    if "ARKRTOS_USE_LINUX_THREAD=1 requires ARKRTOS_USE_LINUX_FORK=1" not in fh:
        bad("features-guard-thread",
            "features.h 缺 thread 对 LINUX_FORK 的 #error")
    else:
        ok("features-guard-thread", "features.h 的 thread 依赖 #error 在")

    m = re.search(r"^#define\s+ARKRTOS_USE_APP_QUOTA\s+(\d+)\s*$", ac, re.M)
    if not m:
        bad("default-off-appq",
            "arkrtos_autoconf.h 里没有 ARKRTOS_USE_APP_QUOTA")
    elif m.group(1) != "0":
        bad("default-off-appq", "默认档 ARKRTOS_USE_APP_QUOTA=%s（必须是 0）"
            % m.group(1))
    else:
        ok("default-off-appq", "arkrtos_autoconf.h: ARKRTOS_USE_APP_QUOTA 0")

    i = kc.find("config ARKRTOS_USE_APP_QUOTA")
    if i < 0:
        bad("kconfig-wired-appq",
            "Kconfig.features 里没有 config ARKRTOS_USE_APP_QUOTA")
    else:
        tail = kc[i + len("config ARKRTOS_USE_APP_QUOTA"):]
        m_end = re.search(r"^(config|menuconfig)\s", tail, re.M)
        blk = tail[:m_end.start()] if m_end else tail
        if "ARKRTOS_USE_ELF" not in blk:
            bad("kconfig-wired-appq",
                "Kconfig 的配额项没有 depends on ARKRTOS_USE_ELF")
        else:
            ok("kconfig-wired-appq",
               "Kconfig 项存在，depends on ARKRTOS_USE_ELF")

    if "ARKRTOS_USE_APP_QUOTA=1 requires ARKRTOS_USE_ELF=1" not in fh:
        bad("features-guard-appq",
            "features.h 缺配额对 ELF 的 #error")
    else:
        ok("features-guard-appq", "features.h 的配额依赖 #error 在")

    bc = read_text(os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h"))
    if re.search(r"^#define\s+ARKRTOS_USE_LINUX_FORK\s+1\s*$", bc, re.M):
        bad("board-switch", "QEMU LA64 板级不该缺省打开 fork（应靠 EXTRA_CFLAGS 出镜像）")
    else:
        ok("board-switch", "QEMU LA64 板级没有缺省打开 fork（默认档零变化）")


# ---------------------------------------------------------------- B 关档零符号

def _sym_defs(nm, obj):
    r = subprocess.run([nm, obj], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    text = read_text_bytes((r.stdout or b"") + (r.stderr or b""))
    return text


def check_off_zero_symbols(rv32, la64, nm, key_dir):
    tmp = tempfile.mkdtemp(prefix="ark_fork_off_")

    obj = os.path.join(tmp, "linux_abi_off.o")
    flags = RV32_ARCH + [
        "-c", "-o", obj,
    ] + board_flags("qemu_riscv32") + ["-I" + key_dir] + \
        includes("qemu_riscv32", "riscv/rv32imac")
    rc, out = run(rv32, flags + ["kernelsrc/src/arkrtos_linux_abi.c"])
    if rc != 0:
        bad("off-zero-symbols", "关档把 linux_abi.c 编成 .o 失败：\n%s" % out[-400:])
    else:
        text = _sym_defs(nm, obj)
        hits = [t for t in ("lx_clone", "lx_wait4", "s_child",
                            "lx_sock_submit", "s_sockctx", "lx_socket",
                            "lx_thread_group_exit", "lx_clone_thread",
                            "lx_thread_bind_hook", "lx_thread_clear_tid",
                            "lx_appq_log")
                if t in text]
        if hits:
            bad("off-zero-symbols", "关档 linux_abi.o 里仍有 %s" % "、".join(hits))
        else:
            ok("off-zero-symbols",
               "关档 linux_abi.o 没有 fork 记账、两个入口与 socket 翻译层")

    pairs = [
        ("kernelsrc/src/arkrtos_elf.c", "arkrtos_elf_fork"),
        ("kernelsrc/port/loongarch/la64/arkrtos_port.c",
         "arkrtos_port_paging_space_clone"),
        ("kernelsrc/src/arkrtos_paging.c", "arkrtos_paging_space_clone"),
        ("kernelsrc/port/loongarch/la64/arkrtos_port.c",
         "arkrtos_port_paging_map_shared"),
    ]
    # 线程档的两个新符号：关档时不许留在 .o 里。
    pairs = pairs + [
        ("kernelsrc/src/arkrtos_elf.c", "arkrtos_elf_thread"),
        # SEC-G07：配额计数与发号在关档时不许留在 .o 里。
        ("kernelsrc/src/arkrtos_elf.c", "elf_app_task_count"),
        ("kernelsrc/src/arkrtos_elf.c", "elf_app_owner_new"),
        ("kernelsrc/port/loongarch/la64/arkrtos_port.c",
         "arkrtos_port_paging_thread_frame"),
    ]
    leaking = []
    for rel, sym in pairs:
        obj2 = os.path.join(tmp, os.path.basename(rel) + ".o")
        flags2 = LA64_ARCH + ["-c", "-o", obj2] + board_flags("qemu_loongarch64") + \
            ["-I" + key_dir] + includes("qemu_loongarch64", "loongarch/la64") + \
            ["-Ikernelsrc/components/ark_vfs/include",
             "-Ikernelsrc/components/ark_vfs/port",
             "-Ikernelsrc/components/ark_vfs/src",
             "-Ikernelsrc/fs", "-Ikernelsrc/fs/littlefs",
             "-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell",
             "-Ikernelsrc/components/ark_softbus/include",
             "-Ikernelsrc/components/ark_softbus/src",
             "-Ikernelsrc/components/ark_softbus/port",
             "-Ikernelsrc/components/mdk_trace"]
        rc2, out2 = run(la64, flags2 + [rel])
        if rc2 != 0:
            leaking.append("%s 编不过：%s" % (rel, out2[-200:]))
            continue
        text2 = _sym_defs(nm, obj2)
        if re.search(r"\s[Tt]\s+%s\b" % re.escape(sym), text2):
            leaking.append("%s 里仍有 %s" % (rel, sym))
    if leaking:
        bad("off-zero-port", "；".join(leaking))
    else:
        ok("off-zero-port", "关档 elf/port/paging 三处都没有 fork 的三个新符号")


# ---------------------------------------------------------------- C 开档编得过

def check_compile_on(la64, key_dir, extra=None, tag="fork"):
    defs = FORK_FLAGS + list(extra or [])
    flags = LA64_ARCH + COMMON + board_flags("qemu_loongarch64", defs) + [
        "-I" + key_dir,
        "-Ikernelsrc/components/mdk_trace",
        "-Ikernelsrc/shell",
        "-Ikernelsrc/shell/ark_shell",
        "-Ikernelsrc/components/ark_vfs/include",
        "-Ikernelsrc/components/ark_vfs/port",
        "-Ikernelsrc/components/ark_vfs/src",
        "-Ikernelsrc/fs",
        "-Ikernelsrc/fs/littlefs",
        "-Ikernelsrc/components/ark_softbus/include",
        "-Ikernelsrc/components/ark_softbus/src",
        "-Ikernelsrc/components/ark_softbus/port",
    ] + includes("qemu_loongarch64", "loongarch/la64")
    files = [
        "kernelsrc/src/arkrtos_linux_abi.c",
        "kernelsrc/src/arkrtos_elf.c",
        "kernelsrc/src/arkrtos_paging.c",
        "kernelsrc/src/arkrtos_task.c",
        "kernelsrc/port/loongarch/la64/arkrtos_port.c",
    ]
    bads = []
    for rel in files:
        rc, out = run(la64, flags + [rel])
        if rc != 0:
            bads.append("%s：\n%s" % (rel, out[-400:]))
    prog = run(la64, LA64_ARCH + list(extra or []) + [
        "-O2", "-Wall", "-ffreestanding", "-fno-common",
        "-nostdlib", "-nostartfiles", "-fno-builtin", "-fsyntax-only",
        "example/qemu_loongarch64/elf/linux_fork.c",
    ])
    if prog[0] != 0:
        bads.append("elf/linux_fork.c：\n%s" % prog[1][-400:])
    if bads:
        bad("compile-on", "开档编不过（%s）—— " % tag + bads[0])
    else:
        ok("compile-on", "6 个 TU 在 LA64 真板级 + %s 档下全编过（含夹具本身）" % tag)


# ---------------------------------------------------------------- D 真读数

def build_image(flags=None):
    flags = FORK_FLAGS if flags is None else flags
    log = os.path.join(tempfile.gettempdir(), "ark_fork_build.log")
    env = dict(os.environ)
    env["EXTRA_CFLAGS"] = " ".join(flags)
    with open(log, "wb") as f:
        # 先 clean：make 不跟踪 EXTRA_CFLAGS，上一档留下的 .o 会让这一次只做
        # 链接，于是镜像里根本没有 fork —— 那是本门禁最想抓的假绿。
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-1200:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT, env=env,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


def restore_default_build():
    """把构建目录恢复成默认档。

    本门禁为了读 fork 的现场证据会 clean + 带 EXTRA_CFLAGS 重编；如果就这么
    收尾，后面任何一次 `make` 都会复用这些 .o（make 不跟踪 EXTRA_CFLAGS），
    把默认档悄悄变成 fork 档 —— 下一个人看到的是"默认档也有 [fork] 读数"
    这种假绿。所以收尾必须重编一次默认档。
    """
    log = os.path.join(tempfile.gettempdir(), "ark_fork_restore.log")
    with open(log, "wb") as f:
        r = subprocess.run(["make", "-f", MAKEFILE, "clean"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
        if r.returncode != 0:
            return False, read_text(log)[-600:]
        r = subprocess.run(["make", "-f", MAKEFILE, "-j8"], cwd=ROOT,
                           stdout=f, stderr=subprocess.STDOUT)
    if r.returncode != 0:
        return False, read_text(log)[-1200:]
    return True, ""


def run_qemu(qemu, seconds):
    """跑 QEMU，串口走管道（file 后端在 kill 时会丢最后一个不满 4 KB 的块）。"""
    import threading
    import time

    cmd = [qemu, "-machine", "virt", "-kernel", ELF_NAME,
           "-nographic", "-monitor", "none"]
    p = subprocess.Popen(cmd, cwd=BUILD_DIR, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    chunks = []

    def drain():
        while True:
            b = p.stdout.read(1024)
            if not b:
                break
            chunks.append(b)

    th = threading.Thread(target=drain)
    th.daemon = True
    th.start()

    marker = b"[fork] finished after"
    deadline = time.time() + seconds
    while time.time() < deadline:
        if marker in b"".join(chunks):
            break
        time.sleep(0.05)

    # 看到 marker 只说明这一行开始上串口；立刻 kill 会把 "exit code 0" 截掉。
    time.sleep(0.5)

    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


def judge(text, expect_checks=EXPECT_FORK_CHECKS, cow=False, shm=False,
          futex=False, filemap=False, mprotect=False, select=False,
          socket=False, thread=False, quota=False):
    lines = text.replace("\r\n", "\n").splitlines()

    def count(pred):
        return sum(1 for ln in lines if pred(ln))

    fk_ok = count(lambda s: s.startswith("[fork] ") and s.rstrip().endswith(" ok"))
    # 汇总行 `[fork] FAIL: N checks failed` 不是一条 check，别把它算进去
    fk_fail = count(lambda s: s.startswith("[fork] ") and "FAIL" in s
                    and not s.startswith("[fork] FAIL:"))
    passline = count(lambda s: s.startswith(
        "[fork] PASS: child ran in its own space, exit 42 reaped"))
    exitline = count(lambda s: re.search(
        r"^\[fork\] finished after \d+ ms, exit code 0$", s) is not None)
    werr = count(lambda s: s.startswith("[fork] write /mnt/ram/linux_fork rc=0"))
    xerr = count(lambda s: s.startswith("[fork] exec rc=0"))
    # [sched] 段单独计数：前缀不同，所以上面十档的 EXPECT_* 一个都不用改。
    sc_ok = count(lambda s: s.startswith("[sched] ") and s.rstrip().endswith(" ok"))
    sc_fail = count(lambda s: s.startswith("[sched] ") and "FAIL" in s)
    # [sio] 段（U-29）同样单独计数：前缀不同，十档的 EXPECT_* 仍然不用改。
    sio_ok = count(lambda s: s.startswith("[sio] ") and s.rstrip().endswith(" ok"))
    sio_fail = count(lambda s: s.startswith("[sio] ") and "FAIL" in s)

    m_par = re.search(r"^\[fork\] parent pid=(\d+)$", text.replace("\r\n", "\n"), re.M)
    m_chi = re.search(r"^\[fork\] child pid=(\d+)$", text.replace("\r\n", "\n"), re.M)

    problems = []
    if werr != 1:
        problems.append("缺 `[fork] write /mnt/ram/linux_fork rc=0`")
    if xerr != 1:
        problems.append("缺 `[fork] exec rc=0`（装载器没接受夹具）")
    if fk_fail != 0:
        problems.append("有 %d 条 check 打了 FAIL" % fk_fail)
    if fk_ok != expect_checks:
        problems.append("check 通过条数 %d != %d" % (fk_ok, expect_checks))
    if sc_fail != 0:
        problems.append("有 %d 条 [sched] check 打了 FAIL" % sc_fail)
    if sc_ok != SCHED_CHECKS:
        problems.append("[sched] check 通过条数 %d != %d" % (sc_ok, SCHED_CHECKS))
    if sio_fail != 0:
        problems.append("有 %d 条 [sio] check 打了 FAIL" % sio_fail)
    if sio_ok != SIGIO_CHECKS:
        problems.append("[sio] check 通过条数 %d != %d" % (sio_ok, SIGIO_CHECKS))
    # 三条新读数必须逐条出现：只比总条数时，换一个夹具（少写一条、多写一条）
    # 仍可能凑出同一个数，那是假绿。
    req = {
        "blocking wait4 blocked >= 40 ms -> 1 ok": "阻塞 wait4 没读出等待时长",
        "parent read end survived child close -> 3 ok": "fd 表不是每进程一份（父读端被子关掉）",
        "blocking pipe read waited >= 40 ms -> 1 ok":
            "父的阻塞读没等到子进程写（空管道读没有挂起）",
        "blocking pipe write wrote all 300 -> 300 ok":
            "缓冲满时的阻塞写没有写全（不是等排空后写全）",
        "child saw the exact byte pattern -> 0 ok":
            "跨进程阻塞写的数据内容不对（写进去的和排出来的不是同一份）",
        "SIGCHLD handler ran -> 1 ok": "跨任务 SIGCHLD 没投递到父进程",
        "mmap for MAP_FIXED > 0 -> 1 ok": "MAP_FIXED 段没拿到一块可丢弃的映射",
        "MAP_FIXED returned the same address -> 1 ok":
            "MAP_FIXED 返回的地址与请求地址不同（不是真 mmap 语义）",
        "MAP_FIXED discarded the old bytes -> 1 ok":
            "同一地址上的旧内容没被丢掉（只是把旧页原地留着）",
        "MAP_FIXED page writable afterwards -> 1 ok":
            "丢弃之后那一页写不进去",
        "MAP_FIXED misaligned -> -EINVAL -> -22 ok":
            "非页对齐的 MAP_FIXED 没被拒绝",
        "MAP_FIXED shared -> -ENOSYS -> -38 ok":
            "MAP_FIXED|MAP_SHARED 被悄悄接受（这种形态本内核不支持）",
    }
    req["lock child GETLK pid is not self -> 1 ok"] = \
        "子进程没读到父进程的锁（F_GETLK 没把别人的锁报回来）"
    req["lock waiter blocked >= 40 ms -> 1 ok"] = \
        "F_SETLKW 没真的挂起（墙钟里量不到 40 ms）"
    req["lock free after child close (conflict range) -> 0 ok"] = \
        "close 没有按文件释放锁（锁被进程一刀切带走了，或者根本没放）"
    if cow:
        req["cow child saw pre-fork values (bits=3) -> 3 ok"] = \
            "COW 没生效（子看到了父 fork 之后的写）"
        req["parent cow write kept -> 48879 ok"] = \
            "父进程自己的写没落到自己那份页"
    if shm:
        if filemap:
            # 开了文件映射档之后「没带 MAP_ANONYMOUS」是文件映射请求，
            # fd=-1 必须回 -EBADF（而不是被当成匿名映射放过）。
            req["mmap(shared without anon) == -EBADF -> -9 ok"] = \
                "文件映射档下没带 MAP_ANONYMOUS 的请求没按文件映射处理（fd=-1 应回 -EBADF）"
        else:
            req["mmap(shared without anon) == -ENOSYS -> -38 ok"] = \
                "没带 MAP_ANONYMOUS 的 MAP_SHARED 居然被接受"
        req["mmap(private|shared) == -ENOSYS -> -38 ok"] = \
            "PRIVATE|SHARED 同时给居然被接受"
        req["mmap(MAP_SHARED|ANON) > 0 -> 1 ok"] = "匿名共享映射失败"
        req["shared page starts zeroed -> 0 ok"] = "共享页不是从 0 开始"
        # 这一条读的是子写进去的值本身（等值比较），所以行尾是 300 而不是 1：
        # 只判「非 0」会把「写到了别的值」也放过。
        req["parent saw child shared write -> 300 ok"] = \
            "父进程看不到子进程对共享页的写（这根本不是共享）或者看到的不是 300"
        req["shm child saw parent post-fork write (bits=1) -> 1 ok"] = \
            "子进程看不到父进程 fork 之后对共享页的写（共享对被当私有复制了）"
    if futex:
        req["futex mmap shared page > 0 -> 1 ok"] = "futex 用的共享页没建起来"
        req["futex(WAKE) with no waiter == 0 -> 0 ok"] = \
            "没有等待者时 WAKE 居然回非 0"
        req["futex(WAIT) on a different value == -EAGAIN -> -11 ok"] = \
            "值不等时 WAIT 没有立即回 -EAGAIN（说明它去睡了）"
        # 第四档：PI 三档（6/7/8）与 robust list（99/100）。旧判据是
        # 「LOCK_PI 必须回 -ENOSYS」，PI 落地之后那条必须删掉，否则门禁
        # 会把「已经实现」判成红。这里逐条钉死新读数：字布局（低 30 位
        # tid / bit31 WAITERS / bit30 OWNER_DIED）、-EDEADLK / -EPERM /
        # -EAGAIN 三档、接手死者时保留 OWNER_DIED、robust 参数面、
        # 跨进程「持锁死亡 -> 交接」、非 PI robust 直接写字。带 pid/tid
        # 或地址的那 7 条走下面的正则形态（档位不同数值就不同）。
        req["futex4 mmap shared page > 0 -> 1 ok"] = \
            "第四档的共享页没建起来"
        req["futex LOCK_PI on a free word == 0 -> 0 ok"] = \
            "空闲字上的 LOCK_PI 失败"
        req["futex LOCK_PI twice == -EDEADLK -> -35 ok"] = \
            "同一任务重复 LOCK_PI 没回 -EDEADLK（会自己等自己）"
        req["futex TRYLOCK_PI while held == -EDEADLK -> -35 ok"] = \
            "自己持有时 TRYLOCK_PI 没回 -EDEADLK"
        req["futex UNLOCK_PI as owner == 0 -> 0 ok"] = \
            "持有者 UNLOCK_PI 失败"
        req["futex UNLOCK_PI cleared the word -> 0 ok"] = \
            "UNLOCK_PI 没把字清干净（残留 tid 或 WAITERS）"
        req["futex UNLOCK_PI when not the owner == -EPERM -> -1 ok"] = \
            "非持有者 UNLOCK_PI 被接受了（Linux 回 -EPERM）"
        req["futex TRYLOCK_PI on an owned word == -EAGAIN -> -11 ok"] = \
            "被别人持有时 TRYLOCK_PI 没回 -EAGAIN"
        req["futex TRYLOCK_PI set the WAITERS bit while failing -> 2147483648 ok"] = \
            "TRYLOCK_PI 失败没置 WAITERS 位（持有者永远不知道有人排队）"
        req["futex TRYLOCK_PI left the owner tid alone -> 4242 ok"] = \
            "TRYLOCK_PI 失败时动了持有者 tid"
        req["futex LOCK_PI on a dead owner == 0 -> 0 ok"] = \
            "OWNER_DIED 字上的 LOCK_PI 失败（死者不能被接手）"
        req["futex LOCK_PI kept OWNER_DIED for the taker -> 1073741824 ok"] = \
            "接手者看不到 OWNER_DIED（用户态没机会做一致性修复）"
        req["futex UNLOCK_PI after consistent == 0 -> 0 ok"] = \
            "用户态清掉 OWNER_DIED 之后 UNLOCK_PI 失败"
        req["set_robust_list with a short length == -EINVAL -> -22 ok"] = \
            "长度不足的 robust head 被接受了"
        req["set_robust_list with an unaligned head == -EINVAL -> -22 ok"] = \
            "未对齐的 robust head 被接受了"
        req["set_robust_list with a null head == -EINVAL -> -22 ok"] = \
            "空 robust head 被接受了"
        req["set_robust_list on a well formed head == 0 -> 0 ok"] = \
            "合法 robust head 登记失败"
        req["get_robust_list(self) == 0 -> 0 ok"] = \
            "读自己的 robust head 失败"
        req["get_robust_list reports the length we registered -> 24 ok"] = \
            "读回的 robust 长度不是登记的那个"
        req["get_robust_list on a foreign pid == -ESRCH -> -3 ok"] = \
            "读别人的 robust head 被允许了（Linux 回 -ESRCH）"
        req["clone for PI robust child > 0 -> 1 ok"] = \
            "PI robust 子进程没 fork 出来"
        req["PI robust: parent observed the child holding the lock -> 1 ok"] = \
            "父进程没看到子进程持锁（握手的字没被写）"
        req["PI robust: parent acquired after the holder died -> 0 ok"] = \
            "持有者带锁死亡后父进程没拿到锁（robust 清理没跑，或退化成叫醒再抢）"
        req["PI robust: word carries OWNER_DIED for the taker -> 1073741824 ok"] = \
            "交接后字里没有 OWNER_DIED"
        req["PI robust child saw register+lock+tid+waiter (bits=15) -> 15 ok"] = \
            "PI robust 子进程四件事没全置位（登记/拿锁/字里是自己/看到父排队）"
        req["clone for non-PI robust child > 0 -> 1 ok"] = \
            "非 PI robust 子进程没 fork 出来"
        req["non-PI robust: parent observed the child owning the word -> 1 ok"] = \
            "父进程没看到子进程直接写字持有"
        req["non-PI robust: parent took over the dead owner's word -> 0 ok"] = \
            "非 PI robust 的死者字没被清理（父进程拿不到）"
        req["non-PI robust: word carries OWNER_DIED after the death -> 1073741824 ok"] = \
            "非 PI robust 清理没留下 OWNER_DIED"
        req["non-PI robust child registered and owned (bits=3) -> 3 ok"] = \
            "非 PI robust 子进程的登记与持有没全成立"
        # 第五档：条件变量的 PI 变体（WAIT_REQUEUE_PI 11 / CMP_REQUEUE_PI 12）。
        # 单进程判参数面，跨进程判「真的把排头搬进互斥并交接」：CMP 的回值必须
        # 是 1、字里必须换成子进程 tid 且不留 WAITERS、子进程三条位全置（被搬运
        # 唤醒 / 字里是自己 / 接手来的锁能放掉）。带 tid / 子号的读数走下面的正则。
        req["futex5 mmap shared page > 0 -> 1 ok"] = \
            "第五档的共享页没建起来"
        req["futex(WAIT_REQUEUE_PI) on a different value == -EAGAIN -> -11 ok"] = \
            "值不符时 WAIT_REQUEUE_PI 没立即回 -EAGAIN（说明它去睡了）"
        req["futex(WAIT_REQUEUE_PI) with a free mutex == -EINVAL -> -22 ok"] = \
            "目标锁没人持有时 WAIT_REQUEUE_PI 被接受了（Linux 回 -EINVAL）"
        req["futex LOCK_PI before the condvar handoff == 0 -> 0 ok"] = \
            "交接前父进程自己拿不住那把 PI 互斥"
        req["futex(CMP_REQUEUE_PI) with a mismatched word == -EAGAIN -> -11 ok"] = \
            "CMP_REQUEUE_PI 没核对期望值"
        req["futex(CMP_REQUEUE_PI) without uaddr2 == -EINVAL -> -22 ok"] = \
            "缺 uaddr2 的 CMP_REQUEUE_PI 被接受了"
        req["clone for condvar rqpi child > 0 -> 1 ok"] = \
            "条件变量 PI 用例的子进程没 fork 出来"
        req["futex rqpi: child queued on the mutex (WAITERS) -> 1 ok"] = \
            "WAIT_REQUEUE_PI 没在目标锁上置 WAITERS（持有者不会来交接）"
        req["futex(CMP_REQUEUE_PI) handed the mutex over == 1 -> 1 ok"] = \
            "CMP_REQUEUE_PI 没把排头搬过去（回值不是 1）"
        req["futex(CMP_REQUEUE_PI) left no stray WAITERS -> 0 ok"] = \
            "交接后字里残留 WAITERS（下一个持有者会去交一把空队列）"
        req["futex rqpi child saw the handoff (bits=7) -> 7 ok"] = \
            "被搬运的子进程三条位没全置（被唤醒 / 字里是自己 / 能解锁）"
        req["futex rqpi mutex cleared after the inherited unlock -> 0 ok"] = \
            "接手来的互斥被放掉之后字不干净"
        req["clone for futex child > 0 -> 1 ok"] = "futex 唤醒者没 fork 出来"
        # 最要紧的一条：LA64 上阻塞返回时 a0 被写成 0，而 a0 就是 uaddr。
        req["futex(WAIT) returned 0 after being woken -> 0 ok"] = \
            "父进程没被 WAKE 叫醒（或重放时把 uaddr 读成了返回值 0）"
        req["parent saw the value the waker wrote -> 2 ok"] = \
            "父进程醒来后没看到唤醒者写的 2（等的不是同一个字）"
        req["futex waker saw 1 and woke 1 (bits=3) -> 3 ok"] = \
            "唤醒者看到的值不是父进程等的那个 1，或 WAKE 没真叫醒 1 个等待者"
        # 多时间片：真调度器才是「时间片到期」的来源，这一组只有上板才证明得了。
        req["futex multislice mmap shared page > 0 -> 1 ok"] = \
            "多时间片 futex 的共享页没建起来"
        req["clone for futex multislice child > 0 -> 1 ok"] = \
            "多时间片 futex 的唤醒者没 fork 出来"
        req["futex multislice WAIT returned 0 after being woken -> 0 ok"] = \
            "跨时间片的 WAIT 没回 0（重放时把 uaddr 读成返回值 0 的老毛病）"
        req["futex multislice parent saw the value the waker wrote -> 2 ok"] = \
            "跨时间片醒来后没看到唤醒者写的 2"
        req["futex multislice WAIT spanned >= 150 ms (crossed a slice) -> 1 ok"] = \
            "这次 WAIT 没真的跨过时间片（不到 150 ms 就返回，等于没测到重放）"
        req["futex multislice waker saw 1 and woke 1 (bits=3) -> 3 ok"] = \
            "多时间片唤醒者没同时看到「父等的还是 1」与「WAKE 回 1」"
        # 第二档：位集 / 超时 / 改挂（REQUEUE、CMP_REQUEUE、WAIT_BITSET、
        # WAKE_BITSET、相对 50 ms 超时）。这些是 glibc 的 condvar 真正走的路。
        req["futex2 mmap shared page > 0 -> 1 ok"] = \
            "第二档 futex 的共享页没建起来"
        req["futex(WAIT_BITSET) with bitset 0 == -EINVAL -> -22 ok"] = \
            "位集为 0 的 WAIT_BITSET 被接受了（Linux 回 -EINVAL）"
        req["futex(WAKE_BITSET) with bitset 0 == -EINVAL -> -22 ok"] = \
            "位集为 0 的 WAKE_BITSET 被接受了（Linux 回 -EINVAL）"
        req["futex(WAKE_BITSET) with no waiter == 0 -> 0 ok"] = \
            "没有等待者时 WAKE_BITSET 回非 0"
        req["futex(WAIT) with zero relative timeout == -ETIMEDOUT -> -110 ok"] = \
            "相对超时为 0 的 WAIT 没有立刻回 -ETIMEDOUT（去睡了一个时间片）"
        req["futex(WAIT_BITSET) with absolute past == -ETIMEDOUT -> -110 ok"] = \
            "绝对时刻已经过去的 WAIT_BITSET 没有立刻回 -ETIMEDOUT" \
            "（说明它把绝对时刻当成了相对时长）"
        req["futex(REQUEUE) with no waiter == 0 -> 0 ok"] = \
            "没有等待者时 REQUEUE 回非 0"
        req["futex(CMP_REQUEUE) with mismatched value == -EAGAIN -> -11 ok"] = \
            "CMP_REQUEUE 没核对期望值（Linux 在不等时回 -EAGAIN）"
        req["futex(REQUEUE) without uaddr2 == -EINVAL -> -22 ok"] = \
            "缺 uaddr2 的 REQUEUE 被接受了"
        req["futex(WAIT) with 50 ms timeout == -ETIMEDOUT -> -110 ok"] = \
            "相对 50 ms 超时没到期（说明超时被每次时间片重放重新计时了）"
        req["futex(WAIT) timeout spanned >= 50 ms -> 1 ok"] = \
            "这次带超时的 WAIT 不到 50 ms 就返回了，等于没测到超时"
        req["clone for futex2 bitset child > 0 -> 1 ok"] = \
            "位集用例的唤醒者没 fork 出来"
        req["futex(WAIT_BITSET) returned 0 after being woken -> 0 ok"] = \
            "按位集等待的父进程没被叫醒"
        req["futex2 bitset waker saw 1, woke only matching (bits=7) -> 7 ok"] = \
            "位集要么被忽略了（不相交的掩码也叫醒了人）、要么相交的掩码叫不醒"
        req["clone for futex2 requeue child > 0 -> 1 ok"] = \
            "改挂用例的唤醒者没 fork 出来"
        req["futex(WAIT) on requeued word returned 0 -> 0 ok"] = \
            "被改挂的父进程没能在新地址上被叫醒"
        req["futex2 requeue: old word has no waiter left -> 0 ok"] = \
            "改挂之后旧地址上还有等待者（改挂没把等待者搬走）"
        req["futex2 requeue child saw 1 and new-word WAKE woke 1 (bits=3) -> 3 ok"] = \
            "改挂用例的读数不全（没看到父等的 1，或新地址的 WAKE 没真叫醒人）"
    if filemap:
        req["filemap first byte is file first byte -> 65 ok"] = \
            "文件内容没装进映射区（首字节不是 'A'）"
        req["filemap bad fd == -EBADF -> -9 ok"] = \
            "坏 fd 被当成了有效映射"
        req["filemap writable shared on read-only fd == -EACCES -> -13 ok"] = \
            "只读 fd 上的可写共享映射居然被接受（Linux 回 -EACCES）"
        req["filemap offset content starts at 4096 -> 79 ok"] = \
            "offset 没生效（映射区里还是文件首页的内容）"
        req["filemap past EOF reads zero -> 0 ok"] = \
            "EOF 之后不是零（把内存里的垃圾当成了文件内容）"
        req["filemap msync rc -> 0 ok"] = "msync 写回没成功"
        req["filemap private msync rc (no writeback) -> 0 ok"] = \
            "私有映射的 msync 没回 0"
        req["filemap shared write persisted, private write not (bits=31) -> 31 ok"] = \
            "写回要么没落盘（共享写丢了）、要么把私有映射的改动也写回去了（两边都不算对）"
    if mprotect:
        # 这一段的判据不是「mprotect 返回 0」——那在关档时也成立（关档只记账）。
        # 要的是「硬件真的挡住了」，所以下面每一条都必须逐字出现。
        req["mprotect mmap 8 KB > 0 -> 1 ok"] = "mprotect 段没拿到 8 KB 映射"
        req["mprotect RO == 0 -> 0 ok"] = "mprotect(RO) 没成功"
        req["read of a read-only page still works -> 4369 ok"] = \
            "只读页读不回来了（把只读做成了不可访问）"
        req["sibling 4 KB half of the same pair stays writable -> 13107 ok"] = \
            "同一对里的另一半 4 KB 也不能写了——保护粒度是 8 KB 而不是 4 KB"
        req["mprotect RW == 0 -> 0 ok"] = "mprotect(RW) 没成功（保护不可撤）"
        req["write after mprotect(RW) took effect -> 17476 ok"] = \
            "撤销保护后写不进去（保护是单向的）"
        req["mprotect RO again == 0 -> 0 ok"] = "第二次设 RO 失败"
        req["clone for mprotect child > 0 -> 1 ok"] = "违规写的子进程没 fork 出来"
        req["mprotect child reaped (killed, not restarted) -> 1 ok"] = \
            "违规写没被拦住，或拦住后把任务重启了（重启会回到同一条指令）"
        req["mprotect violation reports SIGSEGV status -> 11 ok"] = \
            "状态字不是 SIGSEGV（被信号终止与 exit(11) 在 Linux 里是两件事）"
        req["parent page unchanged after the violation -> 17476 ok"] = \
            "违规写居然改到了父进程那一页"
    if select:
        # 这一段不看「返回了个数」：每条读数都必须逐字出现。
        req["pselect6 pipe2 == 0 -> 0 ok"] = "pselect6 段没建起管道"
        req["pselect6 empty pipe read end not ready -> 0 ok"] = \
            "空管道的读端被报成了就绪（就绪判据没看缓冲区）"
        req["pselect6 clears the user set when nothing is ready -> 0 ok"] = \
            "没有就绪位时用户 fd_set 没被清位（原样退回来了）"
        req["pselect6 timed wait returns 0 at the deadline -> 0 ok"] = \
            "定时等待没在截止时刻回 0"
        req["pselect6 timed wait really waited >= 40 ms -> 1 ok"] = \
            "50 ms 的定时等待不到 40 ms 就返回了（超时被每次重放重新计时）"
        req["pselect6 pipe wrote 3 bytes -> 3 ok"] = "pselect6 段的写入不完整"
        req["pselect6 read end ready after 3 bytes -> 1 ok"] = \
            "管道里有数据之后读端没报就绪"
        req["pselect6 keeps the ready bit in the user set -> 1 ok"] = \
            "就绪位没写回用户集（把「就绪」和「用户要求的那几位」弄混了）"
        req["pselect6 write end ready while the pipe has space -> 1 ok"] = \
            "有空间的管道写端没报就绪"
        req["pselect6 stdout is always writable -> 1 ok"] = \
            "stdout 没被当成恒可写"
        req["pselect6 write end not ready when the pipe is full -> 0 ok"] = \
            "管道写满之后写端还被报成就绪（判的是「能调 write」而不是「有容量」）"
        req["pselect6 read end ready on EOF after the writer closed -> 1 ok"] = \
            "写端全关之后读端没按 EOF 报就绪（Linux 在 EOF 上必须报可读）"
        req["pselect6 never reports exceptfds -> 0 ok"] = \
            "exceptfds 被上报了（本层没有带外数据通路）"
        req["pselect6 clears exceptfds -> 0 ok"] = "exceptfds 没被清位"
        req["pselect6 bad fd == -EBADF -> -9 ok"] = \
            "没有对象支撑的 fd 被静默忽略了（Linux 回 -EBADF）"
    if socket:
        # socket 段同样不看「返回了个数」：连接必须真的把 5 个字节送到对端，
        # 端口必须按网络序读回来，半关必须让对端看到 EOF。
        req["socket stream fd >= 3 -> 1 ok"] = "协议栈 3 秒内都没起来（socket 建不出来）"
        req["bind loopback:8081 == 0 -> 0 ok"] = "回环地址 bind 失败"
        req["getsockname port == 8081 -> 8081 ok"] = \
            "getsockname 读回的端口不是 8081（端口没按网络序写回）"
        req["socket unsupported type == -ESOCKTNOSUPPORT -> -94 ok"] = \
            "范围内但不支持的 type 没回 -ESOCKTNOSUPPORT（跟 type 越界混成一档了）"
        req["connect loopback:8081 == 0 -> 0 ok"] = "回环 connect 失败"
        req["accept fd >= 3 -> 1 ok"] = "accept 没拿到新 fd"
        req["tcp send 5 == 5 -> 5 ok"] = "TCP 发送不完整"
        req["tcp recv 5 == 5 -> 5 ok"] = "TCP 接收不完整"
        req["tcp payload hiyo -> 1751742831 ok"] = \
            "收到的载荷不是发出去的那 5 个字节（只是长度对）"
        req["tcp payload last byte -> 117 ok"] = "第 5 个字节不是 u"
        req["getpeername ip == 127.0.0.1 -> 2130706433 ok"] = \
            "getpeername 读回的对端地址不是 127.0.0.1"
        req["getsockopt SO_TYPE locally answered 1 -> 1 ok"] = \
            "SO_TYPE 没被本层答成 SOCK_STREAM"
        req["getsockopt unknown opt == -ENOPROTOOPT -> -92 ok"] = \
            "未知选项被转给协议栈去猜了（应回 -ENOPROTOOPT）"
        req["setsockopt SO_ERROR == -EINVAL -> -22 ok"] = \
            "写 SO_ERROR 被接受了（Linux 回 -EINVAL）"
        req["udp sendto 3 == 3 -> 3 ok"] = "UDP 发送失败"
        req["udp recvfrom 3 == 3 -> 3 ok"] = "UDP 接收失败"
        req["udp payload udp -> 7693424 ok"] = "UDP 载荷不是发出去的那 3 个字节"
        req["udp recvfrom peer port == client port -> 0 ok"] = \
            "recvfrom 读回的对端端口不是客户端本地端口（地址不是这根连接的）"
        req["shutdown SHUT_WR == 0 -> 0 ok"] = "shutdown(SHUT_WR) 失败"
        req["recv after peer shutdown == 0 (EOF) -> 0 ok"] = \
            "半关之后对端没看到 EOF（FIN 没发出去，或 recv 在空队列上永久阻塞）"
        req["write on closed socket == -EBADF -> -9 ok"] = \
            "关掉的 fd 还能写（fd 槽没释放）"
        req["read on closed socket == -EBADF -> -9 ok"] = \
            "关掉的 fd 还能读（fd 槽没释放）"
        # 串口侧交叉证据：lwIP 任务必须在这次启动里起过（回环网卡就靠它）。
        if count(lambda s2: s2.startswith("[boot] lwip task=")) != 1:
            problems.append("缺 `[boot] lwip task=<n>`（协议栈没起，socket 读数不算）")
    for line, why in req.items():
        if count(lambda s2, t=line: s2.strip() == "[fork] " + t) != 1:
            problems.append("缺 `[fork] %s` —— %s" % (line, why))

    # [sched] 段：条数对得上还不够，关键读数必须逐条出现。只比总条数时，
    # 「少一条 A、多一条 B」仍能凑出同一个数 —— 那是假绿。
    sch_req = {
        "getscheduler default -> 0 ok": "初值不是 OTHER（翻译层没把 sched_policy 从 0 起）",
        "getparam FIFO priority is 88 -> 88 ok":
            "setparam/setscheduler 的优先级没原样回读",
        "rr interval nsec is 5000000 -> 5000000 ok":
            "RR 的轮转量没按时间片算成 5 ms",
        "setattr DEADLINE 100% bandwidth -> EBUSY -> -16 ok":
            "100% 带宽没被准入拒掉（EBUSY 是唯一正确的拒绝码）",
        "setattr DEADLINE 1ms/2ms/100ms is admitted -> 0 ok":
            "合法的 deadline 三元组被拒（类没接线，或准入算错）",
        "getattr policy is DEADLINE -> 6 ok":
            "setattr 之后策略没落到 DEADLINE",
        "getattr DEADLINE deadline round-trips -> 2000000 ok":
            "deadline 三元组没有原样回读",
        "setaffinity short len -> EINVAL -> -22 ok":
            "长度不足的亲和性掩码被接受了",
        "sched_yield -> 0 ok": "sched_yield 没回 0",
    }
    for line, why in sch_req.items():
        if count(lambda s2, t=line: s2.strip() == "[sched] " + t) != 1:
            problems.append("缺 `[sched] %s` —— %s" % (line, why))
    # SIGIO 段（U-29）：逐条必现。只比总条数会被「少一条 A、多一条 B」凑成
    # 同一个数骗过。带任务号的读数走下面的形态正则（pid 每档不同）。
    sio_req = {
        "getown on console is 0 -> 0 ok":
            "控制台本体没有 F_GETOWN 通路（回了非 0）",
        "setown on console -> EINVAL -> -22 ok":
            "控制台本体居然能登记主人（Linux 回 -EINVAL）",
        "setown process group -> EINVAL -> -22 ok":
            "进程组（负值）登记被接受了（本层不支持进程组，必须如实拒）",
        "setfl O_ASYNC -> 0 ok": "F_SETFL 打开 O_ASYNC 失败",
        "getfl reports O_ASYNC -> 1 ok":
            "F_GETFL 没读回 O_ASYNC（开关只写在参数里，没进 fd 状态）",
        "dup shares O_ASYNC -> 1 ok":
            "dup 副本没共享 O_ASYNC（dup 建了第二份 file description）",
        "rt_sigaction(SIGIO) rc -> 0 ok": "SIGIO 处理器装不上",
        "write after O_ASYNC returns the byte -> 1 ok":
            "async 写的返回值不是字节数（信号把返回值吞了）",
        "SIGIO handler ran -> 1 ok":
            "O_ASYNC 置位后写数据没有把 SIGIO 投进 handler",
        "SIGIO signo is 29 -> 29 ok": "handler 拿到的信号号不是 29",
        "read still returns the byte -> 1 ok": "SIGIO 把管道里的字节吞掉了",
        "the byte survived -> 105 ok": "读回来的字节不是写进去的那个",
        "clear O_ASYNC -> 0 ok": "清 O_ASYNC 失败",
        "getfl no longer reports O_ASYNC -> 1 ok":
            "清掉 O_ASYNC 之后 F_GETFL 仍报 O_ASYNC",
        "write after clearing O_ASYNC returns the byte -> 1 ok":
            "清掉 O_ASYNC 之后写失败了",
        "clearing O_ASYNC stops delivery -> 0 ok":
            "清掉 O_ASYNC 之后仍在投递",
        "re-open O_ASYNC -> 0 ok": "EOF 段之前重开 O_ASYNC 失败",
        "close writer returns 0 -> 0 ok": "关写端失败",
        "close writer (EOF) sends SIGIO -> 1 ok":
            "写端全关（EOF）没有被当成「变可读」投递",
    }
    for line, why in sio_req.items():
        if count(lambda s2, t=line: s2.strip() == "[sio] " + t) != 1:
            problems.append("缺 `[sio] %s` —— %s" % (line, why))
    # 带任务号的两条读数：主人必须是本任务号（getown 与 dup 副本读回同一个数）。
    m_own = re.search(r"^\[sio\] getown reads back the task id -> (\d+) ok$",
                      text.replace("\r\n", "\n"), re.M)
    if not m_own:
        problems.append("缺 `[sio] getown reads back the task id -> <pid> ok`"
                        "（F_GETOWN 没读回任务号）")
    else:
        own = m_own.group(1)
        if count(lambda s2, t=own: s2.strip() ==
                 "[sio] dup shares the owner -> " + t + " ok") != 1:
            problems.append("缺 `[sio] dup shares the owner -> %s ok`"
                            "（dup 副本的主人登记没共享）" % own)
    if futex:
        # 子进程号会随前面的用例复用（6 / 12 / 15 都出现过，档位不同 pid
        # 就不一样），所以这几条只判「形态」：确实是某一次 reap 读数且 ok。
        for pat, why in (
            (r"^\[fork\] futex child reaped -> \d+ ok$",
             "futex 子进程没被 reap"),
            (r"^\[fork\] futex multislice child reaped -> \d+ ok$",
             "多时间片的 futex 子进程没被 reap"),
            (r"^\[fork\] futex2 bitset child reaped -> \d+ ok$",
             "位集用例的子进程没被 reap"),
            (r"^\[fork\] futex2 requeue child reaped -> \d+ ok$",
             "改挂用例的子进程没被 reap"),
            # 第四档里带 pid/tid 或地址的读数：数值随档位变，只判形态。
            (r"^\[fork\] futex LOCK_PI wrote our tid into the word -> \d+ ok$",
             "LOCK_PI 没把本任务 tid 写进字里"),
            (r"^\[fork\] futex LOCK_PI took over from the dead owner -> \d+ ok$",
             "接手死者的字里不是本任务 tid"),
            (r"^\[fork\] get_robust_list reports the head we registered -> \d+ ok$",
             "读回的 robust head 与登记的不是同一个地址"),
            (r"^\[fork\] PI robust: word holds the parent tid -> \d+ ok$",
             "PI 交接后字里不是父进程 tid"),
            (r"^\[fork\] PI robust child reaped -> \d+ ok$",
             "PI robust 子进程没被 reap"),
            (r"^\[fork\] non-PI robust: word now holds the parent tid -> \d+ ok$",
             "非 PI 接手后字里不是父进程 tid"),
            (r"^\[fork\] non-PI robust child reaped -> \d+ ok$",
             "非 PI robust 子进程没被 reap"),
            (r"^\[fork\] futex\(CMP_REQUEUE_PI\) wrote the child tid into the mutex -> \d+ ok$",
             "CMP_REQUEUE_PI 没把新持有者 tid 写进字里"),
            (r"^\[fork\] futex rqpi child reaped -> \d+ ok$",
             "条件变量 PI 用例的子进程没被 reap"),
        ):
            if re.search(pat, text.replace("\r\n", "\n"), re.M) is None:
                problems.append("缺 `%s` 形态的读数 —— %s"
                                % (pat.replace("^", "").replace("$", ""), why))
    if passline != 1:
        problems.append("缺 `[fork] PASS: child ran in its own space, exit 42 reaped`")
    if exitline != 1:
        problems.append("缺 `[fork] finished after <ms>, exit code 0`")
    if not m_par:
        problems.append("缺 `[fork] parent pid=<n>`")
    if not m_chi:
        problems.append("缺 `[fork] child pid=<n>`")
    if m_par and m_chi and m_par.group(1) == m_chi.group(1):
        problems.append("父子 pid 相同（%s）—— 这不是 fork" % m_par.group(1))

    if cow:
        # 内核日志证明 fork 只挂引用、栈页已私有化、一个字节都没搬，
        # 且复制只在写故障时发生。注意：上面 text 可能带 \r\n，$ 匹配不到
        # 行尾的 \r —— 先归一化再跑正则（否则有日志也报「缺日志」）。
        nl = text.replace("\r\n", "\n")
        shares = re.findall(
            r"^\[LA64U\] pg cow fork shared=(\d+) stack=(\d+) copied=(\d+) frames=(\d+)$",
            nl, re.M)
        if not shares:
            problems.append("缺 `[LA64U] pg cow fork shared=<n> stack=<n> copied=0 frames=<n>`"
                            "（fork 没走 COW 路径）")
        else:
            broke = [t for t in shares if t[2] != "0" or int(t[0]) < 1]
            if broke:
                problems.append("COW fork 日志不对（shared=%s stacked=%s copied=%s）："
                                "应为 shared>=1 且 copied=0"
                                % (broke[0][0], broke[0][1], broke[0][2]))
            nopriv = [t for t in shares if int(t[1]) < 1]
            if nopriv:
                problems.append("栈页没有私有化（stack=%s）—— trap 帧压在栈上，"
                                "栈页必须留可写" % nopriv[0][1])
        copies = re.findall(r"^\[LA64U\] pg cow copy idx=(\d+) ref=(\d+)$", nl, re.M)
        if not copies:
            problems.append("缺 `[LA64U] pg cow copy idx=<n> ref=2`"
                            "（一次写时复制都没发生，COW 等于没接线）")
        elif any(ref == "1" for _i, ref in copies):
            problems.append("出现 ref=1 的 copy 行（没共享也去复制，白搬字节）")

    if shm:
        nl = text.replace("\r\n", "\n")
        maps = re.findall(
            r"^\[LA64U\] pg shm map va=0x[0-9a-f]+ pairs=(\d+)$", nl, re.M)
        if not maps:
            problems.append("缺 `[LA64U] pg shm map va=<hex> pairs=<n>`"
                            "（共享页没在 mmap 时刻建实体：fork 时才各自缺页拿各自的帧，"
                            "共享退化成两次私有分配）")
        elif any(int(n) < 1 for n in maps):
            problems.append("pg shm map 的 pairs=0（什么都没建）")

    if mprotect:
        nl = text.replace("\r\n", "\n")
        # 串口侧必须有一行「按页判违规并摘任务」。没有这行时，上面那条
        # 「子进程被 reap」仍可能成立（子进程自己退出），所以这行是必需的交叉证据。
        hits = re.findall(
            r"^\[LA64U\] mprotect violation badv=0x[0-9a-f]+ era=0x[0-9a-f]+"
            r" tid=\d+ -> kill task$", nl, re.M)
        if not hits:
            problems.append("缺 `[LA64U] mprotect violation badv=<hex> era=<hex>"
                            " -> kill task`（违规写没走 mprotect 判定路径）")

    if thread:
        nl = text.replace("\r\n", "\n")

        def one(pat, why):
            hits = re.findall(pat, nl, re.M)
            if len(hits) != 1:
                problems.append("线程读数 %s 出现 %d 次（必须恰好 1 次）：%s"
                                % (pat, len(hits), why))

        one(r"^\[fork\] clone\(CLONE_THREAD\) > 0 -> 1 ok$",
            "线程根本没造出来")
        one(r"^\[fork\] CHILD_CLEARTID cleared to 0 -> 0 ok$",
            "CHILD_CLEARTID 没被清（pthread_join 会永远等下去）")
        one(r"^\[fork\] futex WAIT on CHILD_CLEARTID woke with 0 -> 0 ok$",
            "join 用的 futex WAIT 没被唤醒（线程退出时的 WAKE 丢了）")
        one(r"^\[fork\] CLONE_SETTLS landed in \$tp -> 1515847680 ok$",
            "CLONE_SETTLS 没写进 $tp（0x5a5a0000 没到位）")
        one(r"^\[fork\] two stacks are distinct -> 1 ok$",
            "线程用的栈和父线程是同一处（帧被踩）")
        one(r"^\[fork\] exit_group-from-thread code 6 -> 6 ok$",
            "线程发起的 exit_group 没把整组带走（或码被改成组长的）")
        one(r"^\[fork\] exit_group-from-leader code 7 -> 7 ok$",
            "组长发起的 exit_group 读数不对")
        # 交叉证据：线程的 getpid 必须真的等于父进程报出来的那个数，而不是
        # 夹具里另找的一个常量。
        m_tg = re.search(r"^\[fork\] thread getpid == tgid \(parent pid\) -> (\d+) ok$",
                         nl, re.M)
        if not m_tg:
            problems.append("缺 `[fork] thread getpid == tgid (parent pid) -> <n> ok`"
                            "（线程的 getpid 没被读到）")
        elif m_par and (m_tg.group(1) != m_par.group(1)):
            problems.append("线程 getpid=%s 与父进程 pid=%s 不同（不是同一个线程组）"
                            % (m_tg.group(1), m_par.group(1)))

    if quota:
        nl = text.replace("\r\n", "\n")

        def one_q(pat, why):
            hits = re.findall(pat, nl, re.M)
            if len(hits) != 1:
                problems.append("配额读数 %s 出现 %d 次（必须恰好 1 次）：%s"
                                % (pat, len(hits), why))

        one_q(r"^\[fork\] quota fork under the limit > 0 -> 1 ok$",
              "上限内的 fork 就没成")
        one_q(r"^\[fork\] quota thread fills the last slot > 0 -> 1 ok$",
              "线程没把最后一格占掉（后面两条拒绝就没有前提）")
        one_q(r"^\[fork\] quota thread over the limit == -EAGAIN -> -11 -> -11 ok$",
              "线程绕过了配额（这条是唯一不可能是 ERR_BUSY 的拒绝）")
        one_q(r"^\[fork\] quota fork blocked while a space is free == -EAGAIN -> -11 ok$",
              "空间空闲时 fork 仍没被拒（配额没生效，只是空间挡住了）")
        one_q(r"^\[fork\] quota blocked child reaped -> 1 -> 1 ok$",
              "子进程没真的被阻塞回收（配额数的是活任务，前提塌了）")
        one_q(r"^\[fork\] quota third thread over the limit == -EAGAIN -> -11 ok$",
              "第三个线程没被拒（配额漏算）")
        one_q(r"^\[fork\] quota threads joined \(CHILD_CLEARTID cleared -> 0\)"
             r" -> 1 ok$", "两个线程没按 CLEARTID 归队")
        one_q(r"^\[fork\] quota fork after the quota was refilled > 0 -> 1 -> 1 ok$",
              "额度回收后没能立刻再起（计数漏回）")
        one_q(r"^\[fork\] quota refilled child exit code 3 -> 3 -> 3 ok$",
              "回收后那个子进程退出码没传回来")
        # 交叉证据：三次拒绝必须各留一行 [appq] 日志，且 owner 号与上限
        # 与本档设置一致。没有这一行，调用者只看到 EAGAIN，分不出
        # 「空间用完」与「额度用完」。
        appq = re.findall(
            r"^\[appq\] refused (fork|thread): owner (\d+) quota (\d+) tasks"
            r" reached$", nl, re.M)
        if len(appq) != 4:
            problems.append("[appq] 拒绝日志 %d 行（应有 4 行：超限 fork、超限线程、"
                            "空间空闲时 fork、第三个线程）" % len(appq))
        else:
            kinds = [k for k, _o, _q in appq]
            if kinds.count("fork") != 2 or kinds.count("thread") != 2:
                problems.append("[appq] 日志种类不对（%s）：应为 2 次 fork + 2 次 thread"
                                % "、".join(kinds))
            owners = set(o for _k, o, _q in appq)
            quotas = set(q for _k, _o, q in appq)
            if len(owners) != 1:
                problems.append("[appq] 三行日志的 owner 不是同一个（%s）"
                                % "、".join(sorted(owners)))
            if quotas != {"3"}:
                problems.append("[appq] 日志里的上限是 %s（门禁设的是 3）"
                                % "、".join(sorted(quotas)))

    # 新开关不许把别人的演示搞坏。
    for tag, name in (("[linux] PASS", "Linux ABI 夹具"),
                      ("[FS] PASS", "ramfs 自测"),
                      ("[hello] bye, exit code = argc", "Ark ABI 的 hello")):
        if count(lambda s, t=tag: s.startswith(t)) != 1:
            problems.append("同一次启动里丢掉了 %s 的读数" % name)

    if problems:
        return False, problems
    return True, ["%d 条 check 全 ok、PASS 行与 exit code 0 都在，父子 pid 不同（%s != %s），旁路演示未受影响"
                  % (expect_checks, m_par.group(1), m_chi.group(1))]


def self_check(good_text, cow=False, shm=False, futex=False, filemap=False,
               mprotect=False, select=False, socket=False, thread=False,
               quota=False):
    """把好日志的九种坏法喂回判据，每一种都必须判红，否则解析器是空转。"""
    g = good_text.replace("\r\n", "\n")
    mutations = {
        "去掉 PASS 行": lambda s: s.replace(
            "[fork] PASS: child ran in its own space, exit 42 reaped\n", ""),
        "退出码改成 3": lambda s: re.sub(r"exit code 0$", "exit code 3", s, flags=re.M),
        "插一条 FAIL": lambda s: s.replace(
            "[fork] child pid > 0 -> 1 ok",
            "[fork] child pid > 0 -> 1 ok\n[fork] bogus -> 0 FAIL (want 1)"),
        "少一条 check": lambda s: s.replace(
            "[fork] parent marker intact -> 1 ok\n", ""),
        "父子 pid 改成相同": lambda s: re.sub(
            r"^\[fork\] child pid=\d+$", lambda m: m.group(0), s, flags=re.M
        ).replace(_pid_line(s, "child"), _pid_line(s, "parent")),
        "去掉 exec rc=0": lambda s: s.replace("[fork] exec rc=0\n", ""),
        "阻塞读数改成没等": lambda s: s.replace(
            "[fork] blocking wait4 blocked >= 40 ms -> 1 ok",
            "[fork] blocking wait4 blocked >= 40 ms -> 0 FAIL (want 1)"),
        "父读端改成 -EBADF": lambda s: s.replace(
            "[fork] parent read end survived child close -> 3 ok",
            "[fork] parent read end survived child close -> -9 FAIL (want 3)"),
        "SIGCHLD 没被调到": lambda s: s.replace(
            "[fork] SIGCHLD handler ran -> 1 ok",
            "[fork] SIGCHLD handler ran -> 0 FAIL (want 1)"),
        "MAP_FIXED 没丢旧内容": lambda s: s.replace(
            "[fork] MAP_FIXED discarded the old bytes -> 1 ok\n",
            "[fork] MAP_FIXED discarded the old bytes -> 0 ok\n"),
        # [sched] 段（U-28，十档都在）：少一条、插一条 FAIL、改一个数字。
        "sched 少一条读数": lambda s: s.replace(
            "[sched] sched_yield -> 0 ok\n", ""),
        "sched 插一条 FAIL": lambda s: s.replace(
            "[sched] getscheduler default -> 0 ok",
            "[sched] getscheduler default -> 0 ok\n"
            "[sched] bogus -> 1 FAIL (want 0)"),
        "sched 数字被改": lambda s: s.replace(
            "[sched] getattr DEADLINE deadline round-trips -> 2000000 ok",
            "[sched] getattr DEADLINE deadline round-trips -> 3000000 ok"),
        # [sio] 段（U-29，十档都在）：少一条、插一条 FAIL、改一个数字。
        "sio 少一条读数": lambda s: s.replace(
            "[sio] clear O_ASYNC -> 0 ok\n", ""),
        "sio 插一条 FAIL": lambda s: s.replace(
            "[sio] SIGIO signo is 29 -> 29 ok",
            "[sio] SIGIO signo is 29 -> 29 ok\n"
            "[sio] bogus -> 1 FAIL (want 0)"),
        "sio 数字被改": lambda s: s.replace(
            "[sio] getown on console is 0 -> 0 ok",
            "[sio] getown on console is 0 -> 7 ok"),
    }
    if cow:
        mutations.update({
            "COW 位掩码丢一位": lambda s: s.replace(
                "[fork] cow child saw pre-fork values (bits=3) -> 3 ok",
                "[fork] cow child saw pre-fork values (bits=3) -> 1 FAIL (want 3)"),
            "COW 父写丢失": lambda s: s.replace(
                "[fork] parent cow write kept -> 48879 ok",
                "[fork] parent cow write kept -> 0 FAIL (want 48879)"),
            # 真日志里有多条 copy 行：只删一条仍能通过判据，那不是坏样本。
            # 整类删光才是「一次写时复制都没发生」的形态。
            "缺 cow copy 行": lambda s: re.sub(
                r"^\[LA64U\] pg cow copy idx=\d+ ref=\d+$\n", "", s, flags=re.M),
            "fork 时搬了字节": lambda s: re.sub(
                r"^(\[LA64U\] pg cow fork shared=\d+ stack=\d+) copied=0",
                r"\1 copied=2",
                s, count=1, flags=re.M),
        })
    if shm:
        mutations.update({
            "缺 pg shm map 日志": lambda s: re.sub(
                r"^\[LA64U\] pg shm map va=0x[0-9a-f]+ pairs=\d+$\n", "",
                s, flags=re.M),
            "共享写没被父看到": lambda s: s.replace(
                "[fork] parent saw child shared write -> 300 ok",
                "[fork] parent saw child shared write -> 200 FAIL (want 300)"),
            "子看不到父的写": lambda s: s.replace(
                "[fork] shm child saw parent post-fork write (bits=1) -> 1 ok",
                "[fork] shm child saw parent post-fork write (bits=1) -> 0 FAIL (want 1)"),
            "共享页起始非 0": lambda s: s.replace(
                "[fork] shared page starts zeroed -> 0 ok",
                "[fork] shared page starts zeroed -> 7 FAIL (want 0)"),
            "缺共享映射成功读数": lambda s: s.replace(
                "[fork] mmap(MAP_SHARED|ANON) > 0 -> 1 ok\n", ""),
        })
    if futex:
        mutations.update({
            "futex WAIT 没被叫醒": lambda s: s.replace(
                "[fork] futex(WAIT) returned 0 after being woken -> 0 ok",
                "[fork] futex(WAIT) returned 0 after being woken -> -38 FAIL (want 0)"),
            "futex 唤醒者只看到一半": lambda s: s.replace(
                "[fork] futex waker saw 1 and woke 1 (bits=3) -> 3 ok",
                "[fork] futex waker saw 1 and woke 1 (bits=3) -> 1 FAIL (want 3)"),
            "futex 无等待者 WAKE 回 1": lambda s: s.replace(
                "[fork] futex(WAKE) with no waiter == 0 -> 0 ok",
                "[fork] futex(WAKE) with no waiter == 0 -> 1 FAIL (want 0)"),
            "futex 多时间片 WAIT 被打成 -EFAULT": lambda s: s.replace(
                "[fork] futex multislice WAIT returned 0 after being woken -> 0 ok",
                "[fork] futex multislice WAIT returned 0 after being woken -> -14 FAIL (want 0)"),
            "futex 多时间片没跨过时间片": lambda s: s.replace(
                "[fork] futex multislice WAIT spanned >= 150 ms (crossed a slice) -> 1 ok",
                "[fork] futex multislice WAIT spanned >= 150 ms (crossed a slice) -> 0 FAIL (want 1)"),
            # 位集被忽略的形态：不相交的掩码也叫醒了人 ⇒ 位集那条读数从 7 变 3。
            "futex 位集被忽略": lambda s: s.replace(
                "[fork] futex2 bitset waker saw 1, woke only matching (bits=7) -> 7 ok",
                "[fork] futex2 bitset waker saw 1, woke only matching (bits=7) -> 3 FAIL (want 7)"),
            # 改挂没生效的形态：旧地址上还有等待者。
            "futex 改挂没搬走等待者": lambda s: s.replace(
                "[fork] futex2 requeue: old word has no waiter left -> 0 ok",
                "[fork] futex2 requeue: old word has no waiter left -> 1 FAIL (want 0)"),
            # 超时被重新计时的形态：那次 WAIT 根本没到期。
            "futex 超时没到期": lambda s: s.replace(
                "[fork] futex(WAIT) with 50 ms timeout == -ETIMEDOUT -> -110 ok",
                "[fork] futex(WAIT) with 50 ms timeout == -ETIMEDOUT -> 0 FAIL (want -110)"),
            # 第四档（PI 三档与 robust list）也要有反假绿：交接没发生、
            # OWNER_DIED 被内核吞掉、robust 参数不校验，三种都必须判红。
            "PI 交接没发生": lambda s: s.replace(
                "[fork] PI robust: parent acquired after the holder died -> 0 ok",
                "[fork] PI robust: parent acquired after the holder died -> -11 FAIL (want 0)"),
            "交接后 OWNER_DIED 被吞": lambda s: s.replace(
                "[fork] PI robust: word carries OWNER_DIED for the taker -> 1073741824 ok",
                "[fork] PI robust: word carries OWNER_DIED for the taker -> 0 FAIL (want 1073741824)"),
            "robust 参数不校验": lambda s: s.replace(
                "[fork] set_robust_list with a short length == -EINVAL -> -22 ok",
                "[fork] set_robust_list with a short length == -EINVAL -> 0 ok"),
        })
    if mprotect:
        mutations.update({
            "只读页读不回来": lambda s: s.replace(
                "[fork] read of a read-only page still works -> 4369 ok",
                "[fork] read of a read-only page still works -> -13 FAIL (want 4369)"),
            # 这一条正是「按 8 KB 对记台账」的形态：另一半页也一起只读了。
            "粒度退化成 8 KB": lambda s: s.replace(
                "[fork] sibling 4 KB half of the same pair stays writable -> 13107 ok",
                "[fork] sibling 4 KB half of the same pair stays writable -> 0 FAIL (want 13107)"),
            "违规写没被拦住": lambda s: s.replace(
                "[fork] mprotect child reaped (killed, not restarted) -> 1 ok",
                "[fork] mprotect child reaped (killed, not restarted) -> 0 FAIL (want 1)"),
            "违规写改了父页": lambda s: s.replace(
                "[fork] parent page unchanged after the violation -> 17476 ok",
                "[fork] parent page unchanged after the violation -> 57005 FAIL (want 17476)"),
            "状态字当成退出码": lambda s: s.replace(
                "[fork] mprotect violation reports SIGSEGV status -> 11 ok",
                "[fork] mprotect violation reports SIGSEGV status -> 2816 FAIL (want 11)"),
            "缺 mprotect 违规日志": lambda s: re.sub(
                r"^\[LA64U\] mprotect violation badv=0x[0-9a-f]+ era=0x[0-9a-f]+"
                r" tid=\d+ -> kill task$\n", "", s, flags=re.M),
            "缺 mprotect 成功读数": lambda s: s.replace(
                "[fork] mprotect RO == 0 -> 0 ok\n", ""),
        })
    if socket:
        mutations.update({
            "socket 载荷只对长度": lambda s: s.replace(
                "[fork] tcp payload hiyo -> 1751742831 ok",
                "[fork] tcp payload hiyo -> 0 ok"),
            "getsockname 端口没按网络序": lambda s: s.replace(
                "[fork] getsockname port == 8081 -> 8081 ok",
                "[fork] getsockname port == 8081 -> 129 ok"),
            "半关没让对端看到 EOF": lambda s: s.replace(
                "[fork] recv after peer shutdown == 0 (EOF) -> 0 ok",
                "[fork] recv after peer shutdown == 0 (EOF) -> -11 FAIL (want 0)"),
            "关掉的 fd 仍可写": lambda s: s.replace(
                "[fork] write on closed socket == -EBADF -> -9 ok",
                "[fork] write on closed socket == -EBADF -> 2 ok"),
            "recvfrom 对端端口不对": lambda s: s.replace(
                "[fork] udp recvfrom peer port == client port -> 0 ok",
                "[fork] udp recvfrom peer port == client port -> 1 ok"),
            "lwip 任务没起": lambda s: s.replace("[boot] lwip task=", "[boot] lwip off="),
        })
    if select:
        mutations.update({
            "空管道被报就绪": lambda s: s.replace(
                "[fork] pselect6 empty pipe read end not ready -> 0 ok",
                "[fork] pselect6 empty pipe read end not ready -> 1 FAIL (want 0)"),
            "EOF 不报就绪": lambda s: s.replace(
                "[fork] pselect6 read end ready on EOF after the writer closed -> 1 ok",
                "[fork] pselect6 read end ready on EOF after the writer closed -> 0 FAIL (want 1)"),
            "满管道仍报可写": lambda s: s.replace(
                "[fork] pselect6 write end not ready when the pipe is full -> 0 ok",
                "[fork] pselect6 write end not ready when the pipe is full -> 1 FAIL (want 0)"),
            "fd_set 没清位": lambda s: s.replace(
                "[fork] pselect6 clears the user set when nothing is ready -> 0 ok",
                "[fork] pselect6 clears the user set when nothing is ready -> 1 FAIL (want 0)"),
            "超时被重新计时": lambda s: s.replace(
                "[fork] pselect6 timed wait really waited >= 40 ms -> 1 ok",
                "[fork] pselect6 timed wait really waited >= 40 ms -> 0 FAIL (want 1)"),
            "exceptfds 被上报": lambda s: s.replace(
                "[fork] pselect6 never reports exceptfds -> 0 ok",
                "[fork] pselect6 never reports exceptfds -> 1 FAIL (want 0)"),
            "坏 fd 被放过": lambda s: s.replace(
                "[fork] pselect6 bad fd == -EBADF -> -9 ok",
                "[fork] pselect6 bad fd == -EBADF -> 0 FAIL (want -9)"),
            "缺 select 读数": lambda s: s.replace(
                "[fork] pselect6 stdout is always writable -> 1 ok\n", ""),
        })
    if thread:
        mutations.update({
            "线程 CLEARTID 没清": lambda s: s.replace(
                "[fork] CHILD_CLEARTID cleared to 0 -> 0 ok",
                "[fork] CHILD_CLEARTID cleared to 0 -> 1711149807 FAIL (want 0)"),
            "join 的 WAIT 没被唤醒": lambda s: s.replace(
                "[fork] futex WAIT on CHILD_CLEARTID woke with 0 -> 0 ok",
                "[fork] futex WAIT on CHILD_CLEARTID woke with 0 -> -110 FAIL (want 0)"),
            "SETTLS 没落 $tp": lambda s: s.replace(
                "[fork] CLONE_SETTLS landed in $tp -> 1515847680 ok",
                "[fork] CLONE_SETTLS landed in $tp -> 0 FAIL (want 1515847680)"),
            "线程 getpid 不等于 tgid": lambda s: re.sub(
                r"^\[fork\] thread getpid == tgid \(parent pid\) -> \d+ ok$",
                "[fork] thread getpid == tgid (parent pid) -> 999999 ok",
                s, flags=re.M),
            "两栈同址": lambda s: s.replace(
                "[fork] two stacks are distinct -> 1 ok",
                "[fork] two stacks are distinct -> 0 FAIL (want 1)"),
            "线程 exit_group 码被改成组长的": lambda s: s.replace(
                "[fork] exit_group-from-thread code 6 -> 6 ok",
                "[fork] exit_group-from-thread code 6 -> 7 FAIL (want 6)"),
            "缺 clone 线程读数": lambda s: s.replace(
                "[fork] clone(CLONE_THREAD) > 0 -> 1 ok\n", ""),
        })
    if quota:
        mutations.update({
            "配额被绕过（线程放行）": lambda s: s.replace(
                "[fork] quota thread over the limit == -EAGAIN -> -11 -> -11 ok",
                "[fork] quota thread over the limit == -EAGAIN -> -11 -> 15 FAIL (want -11)"),
            "配额没生效（空间空闲时 fork 放行）": lambda s: s.replace(
                "[fork] quota fork blocked while a space is free == -EAGAIN -> -11 ok",
                "[fork] quota fork blocked while a space is free == -EAGAIN -> 12"),
            "计数漏回（回收后起不来）": lambda s: s.replace(
                "[fork] quota fork after the quota was refilled > 0 -> 1 -> 1 ok",
                "[fork] quota fork after the quota was refilled > 0 -> 1 -> 0 FAIL (want 1)"),
            "配额数成死任务（子进程没回收）": lambda s: s.replace(
                "[fork] quota blocked child reaped -> 1 -> 1 ok",
                "[fork] quota blocked child reaped -> 1 -> 0 FAIL (want 1)"),
            "缺 [appq] 日志": lambda s: re.sub(
                r"^\[appq\] refused \w+: owner \d+ quota \d+ tasks reached$\n",
                "", s, flags=re.M),
        })

    if filemap:
        mutations.update({
            "filemap 共享写没落盘": lambda s: s.replace(
                "[fork] filemap shared write persisted, private write not (bits=31) -> 31 ok",
                "[fork] filemap shared write persisted, private write not (bits=31) -> 23 FAIL (want 31)"),
            "filemap 私有写也落盘": lambda s: s.replace(
                "[fork] filemap private msync rc (no writeback) -> 0 ok",
                "[fork] filemap private msync rc (no writeback) -> -5 FAIL (want 0)"),
            "filemap offset 没生效": lambda s: s.replace(
                "[fork] filemap offset content starts at 4096 -> 79 ok",
                "[fork] filemap offset content starts at 4096 -> 65 FAIL (want 79)"),
            "缺 filemap msync 读数": lambda s: s.replace(
                "[fork] filemap msync rc -> 0 ok\n", ""),
        })
    missed = []
    nchk = (EXPECT_QUOTA_CHECKS if quota else
            (EXPECT_THREAD_CHECKS if thread else
            (EXPECT_SELECT_CHECKS if select else
            (EXPECT_MPROTECT_CHECKS if mprotect else
            (EXPECT_FILEMAP_CHECKS if filemap else
            (EXPECT_FUTEX_CHECKS if futex else
            (EXPECT_SHM_CHECKS if shm else
             (EXPECT_COW_CHECKS if cow else EXPECT_FORK_CHECKS))))))))
    for name, fn in mutations.items():
        mutated = fn(g)
        if mutated == g:
            missed.append("%s（坏样本没改到东西）" % name)
            continue
        passed, _ = judge(mutated, nchk, cow, shm, futex, filemap, mprotect,
                          select, socket, thread, quota)
        if passed:
            missed.append(name)
    return missed


def _pid_line(text, who):
    m = re.search(r"^\[fork\] %s pid=\d+$" % who, text, re.M)
    return m.group(0) if m else "\x00"


# ---------------------------------------------------------------- main

def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    ap.add_argument("--rv32-gcc", default="")
    ap.add_argument("--qemu", default="")
    ap.add_argument("--no-build", action="store_true", help="复用已有 ELF")
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--keep", action="store_true", help="打印串口日志尾部")
    a = ap.parse_args(argv)

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")
    qemu = _pick([q for q in QEMU_CANDIDATES], a.qemu, "qemu-system-loongarch64")
    nm = nm_of(la64) if la64 else None
    if la64 and nm is None:
        bad("toolchain", "找不到与 %s 配套的 nm（符号检查不能跳过）" % os.path.basename(la64))

    key_dir = tempfile.mkdtemp(prefix="ark_fork_key_")
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    check_default_off()
    if rv32 and la64 and nm:
        check_off_zero_symbols(rv32, la64, nm, key_dir)
    if la64:
        check_compile_on(la64, key_dir)
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1"], "fork+cow")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1"],
                         "fork+cow+shm")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1"],
                         "fork+cow+shm+futex")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1",
                                         "-DARKRTOS_USE_PAGING_FILEMAP=1"],
                         "fork+cow+shm+futex+filemap")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1",
                                         "-DARKRTOS_USE_PAGING_FILEMAP=1",
                                         "-DARKRTOS_USE_PAGING_MPROTECT=1"],
                         "fork+cow+shm+futex+filemap+mprotect")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1",
                                         "-DARKRTOS_USE_PAGING_FILEMAP=1",
                                         "-DARKRTOS_USE_PAGING_MPROTECT=1",
                                         "-DARKRTOS_USE_LINUX_SELECT=1"],
                         "fork+cow+shm+futex+filemap+mprotect+select")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1",
                                         "-DARKRTOS_USE_PAGING_FILEMAP=1",
                                         "-DARKRTOS_USE_PAGING_MPROTECT=1",
                                         "-DARKRTOS_USE_LINUX_SELECT=1",
                                         "-DARKRTOS_USE_LINUX_SOCKET=1",
                                         "-DARKRTOS_USE_LWIP=1"],
                         "fork+cow+shm+futex+filemap+mprotect+select+socket")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1",
                                         "-DARKRTOS_USE_PAGING_FILEMAP=1",
                                         "-DARKRTOS_USE_PAGING_MPROTECT=1",
                                         "-DARKRTOS_USE_LINUX_SELECT=1",
                                         "-DARKRTOS_USE_LINUX_SOCKET=1",
                                         "-DARKRTOS_USE_LWIP=1",
                                         "-DARKRTOS_USE_LINUX_THREAD=1"],
                         "fork+cow+shm+futex+filemap+mprotect+select+socket+thread")
        check_compile_on(la64, key_dir, ["-DARKRTOS_USE_PAGING_COW=1",
                                         "-DARKRTOS_USE_PAGING_SHM=1",
                                         "-DARKRTOS_USE_LINUX_FUTEX=1",
                                         "-DARKRTOS_USE_PAGING_FILEMAP=1",
                                         "-DARKRTOS_USE_PAGING_MPROTECT=1",
                                         "-DARKRTOS_USE_LINUX_SELECT=1",
                                         "-DARKRTOS_USE_LINUX_SOCKET=1",
                                         "-DARKRTOS_USE_LWIP=1",
                                         "-DARKRTOS_USE_LINUX_THREAD=1",
                                         "-DARKRTOS_USE_APP_QUOTA=1",
                                         "-DARKRTOS_APP_MAX_TASKS=3"],
                         "fork+…+thread+appq")

    if qemu:
        # 十档各跑一遍：默认 fork（47 条）、fork+COW（52 条 + 内核 COW 日志）、
        # fork+COW+SHM（60 条 + 内核共享映射日志）、fork+COW+SHM+futex
        # （147 条 + 跨进程 WAIT/WAKE 配对 + 多时间片重放）、再叠 filemap（167 条）与
        # mprotect（178 条）、select（194 条）、socket（232 条）、thread（265 条）
        # 与配额（277 条）。别把 SHM 并进 COW 档、也别把
        # futex 并进 SHM 档 —— 那样「关档时 mmap(MAP_SHARED) / futex 仍回
        # -ENOSYS」这两条反假绿就永远测不到了。
        # 每次都要先 clean（make 不跟踪 EXTRA_CFLAGS），否则上一次的 .o 会让
        # 这一次只做链接，镜像里根本没有这一档的代码 —— 本门禁最想抓的假绿。
        rounds = [
            (FORK_FLAGS, EXPECT_FORK_CHECKS, False, False, "fork"),
            (COW_FLAGS, EXPECT_COW_CHECKS, True, False, "fork+cow"),
            (SHM_FLAGS, EXPECT_SHM_CHECKS, True, True, "fork+cow+shm"),
            (FUTEX_FLAGS, EXPECT_FUTEX_CHECKS, True, True, "fork+cow+shm+futex"),
            (FILEMAP_FLAGS, EXPECT_FILEMAP_CHECKS, True, True,
             "fork+cow+shm+futex+filemap"),
            (MPROTECT_FLAGS, EXPECT_MPROTECT_CHECKS, True, True,
             "fork+cow+shm+futex+filemap+mprotect"),
            (SELECT_FLAGS, EXPECT_SELECT_CHECKS, True, True,
             "fork+cow+shm+futex+filemap+mprotect+select"),
            (SOCKET_FLAGS, EXPECT_SOCKET_CHECKS, True, True,
             "fork+cow+shm+futex+filemap+mprotect+select+socket"),
            (THREAD_FLAGS, EXPECT_THREAD_CHECKS, True, True,
             "fork+cow+shm+futex+filemap+mprotect+select+socket+thread"),
            (QUOTA_FLAGS, EXPECT_QUOTA_CHECKS, True, True,
             "fork+cow+shm+futex+filemap+mprotect+select+socket+thread+appq"),
        ]
        if a.no_build:
            rounds = rounds[-1:]
        for flags, nchk, cow, shm, tag in rounds:
            if not a.no_build:
                built, why = build_image(flags)
                if not built:
                    bad("build", "[%s] make 失败：\n%s" % (tag, why))
                    continue
                ok("build", "[%s] make -f %s EXTRA_CFLAGS=\"%s\" 通过"
                   % (tag, MAKEFILE, " ".join(flags)))
            if not os.path.isfile(os.path.join(BUILD_DIR, ELF_NAME)):
                bad("qemu-run", "[%s] 找不到 %s（先编一次，或去掉 --no-build）"
                    % (tag, ELF_NAME))
                continue
            text = run_qemu(qemu, a.seconds)
            if text is None:
                bad("qemu-run", "[%s] QEMU 没写出串口（%s 秒内）" % (tag, a.seconds))
                continue
            # 串口整段留档：以前读数不对时只剩一句「缺 X 行」，到底是夹具
            # 没打、还是串口只收到一半，分不出来 —— 那会把一次真缺陷读成
            # 一次抖动。落盘一次，失败时可回看原文。
            dump = os.path.join(tempfile.gettempdir(),
                                "ark_fork_%s.log" % tag.replace("+", "_"))
            try:
                with open(dump, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
            except OSError:
                dump = ""

            futex = "-DARKRTOS_USE_LINUX_FUTEX=1" in flags
            filemap = "-DARKRTOS_USE_PAGING_FILEMAP=1" in flags
            mprotect = "-DARKRTOS_USE_PAGING_MPROTECT=1" in flags
            select = "-DARKRTOS_USE_LINUX_SELECT=1" in flags
            socket = "-DARKRTOS_USE_LINUX_SOCKET=1" in flags
            thread = "-DARKRTOS_USE_LINUX_THREAD=1" in flags
            quota = "-DARKRTOS_USE_APP_QUOTA=1" in flags
            passed, notes = judge(text, nchk, cow, shm, futex, filemap, mprotect,
                                  select, socket, thread, quota)
            if passed:
                ok("qemu-readout", "[%s] %s" % (tag, notes[0]))
                missed = self_check(text, cow, shm, futex, filemap, mprotect,
                                    select, socket, thread, quota)
                if missed:
                    bad("parser-selfcheck",
                        "[%s] 这些坏样本没被判红：%s" % (tag, "、".join(missed)))
                else:
                    ok("parser-selfcheck",
                       "[%s] 坏法（含 COW 专属）全被判红" % tag)
            else:
                bad("qemu-readout", "[%s] %s（串口 %d 行，原文见 %s）"
                    % (tag, "；".join(notes),
                       len(text.replace("\r\n", "\n").splitlines()),
                       dump or "（落盘失败）"))
                bad("parser-selfcheck", "[%s] 读数没过，自检无从谈起" % tag)
            if a.keep:
                tail = "\n".join(text.replace("\r\n", "\n").splitlines()[-40:])
                print("\n---- 串口末尾（%s） ----\n%s\n------------------" % (tag, tail))

        good, why = restore_default_build()
        if not good:
            bad("restore-default", "恢复默认档失败：\n%s" % why)
        else:
            ok("restore-default", "构建目录已恢复默认档（避免下一次 make 复用 fork 档 .o）")

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("FORK-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))