# 像 Linux 一样跑可执行文件：MMU 平台的 ELF64 装载（M1）

## 0 一句话

带 MMU 的后端（页表后端，LA64 已实测、LA32 同源）现在可以

```
mark@arkrtos:~$ exec /mnt/ram/hello one two
mark@arkrtos:~$ /mnt/ram/hello direct          # 也可以直接敲路径
```

——跑的是一个**原生 ELF64 可执行文件**，不是先转成某种裸镜像再跑。这正是 Linux
的可执行文件格式本身（Linux 的 `./prog` 也是 `PT_LOAD` + 跳 `e_entry`）。

M1 交付的是「**文件即程序**」+ Linux 入口约定（`sp→argc/argv`）+ 单实例；**没有**
交付「进程」：每进程地址空间、同 VA 多实例、按需缺页、`copy_from_user`、退出码通道
都还没有，见 §6 欠账表。**不得把 M1 说成"Ark 有进程了"。**

适用范围写死：

* 已验证：`qemu_loongarch64`（LA64，页表后端 + PLV3 窗口），QEMU 实跑，见 §5。
* 同源未跑：`la32_generic`（LA32，帧 128 B、装载器算术相同，但 `arkrtos_elf.c`
  要求 64 位 `long`，因此 LA32 只能编进空目标——见 §6 欠账 U-4）。
* 与本机制无关：无 MMU 后端（MPU/PMP）不做 ELF，用的是 `app run` 那条固定窗口路径。

---

## 1 「跑 ELF」和「直接跑可执行文件」不矛盾

用户在评审时问的是「带 MMU 的不应该跑 elf 应该直接就是可执行文件吧」。两层要分开：

1. **文件格式层**：Linux 的可执行文件就是 ELF。内核 `execve()` 做的事是读 ELF 头、
   按 `PT_LOAD` 把段映射进新地址空间、把 `argc/argv/envp` 摆到栈顶、跳 `e_entry`。
   所以「直接跑可执行文件」= 「装载 ELF」。我们没有中间格式：`/mnt/ram/hello`
   就是 `loongarch64-newlib-elf-gcc` 直接链出来的 ELF64，磁盘上也没有转换产物。
2. **进程模型层**：Linux 体验的内核不是文件格式，而是**每进程独立地址空间**。
   M1 把段装进**唯一一块 2 MB 的 PLV3 静态窗口**（`0x02000000`）、单实例、
   程序必须按 `-Ttext=0x02000000` 链到这个固定基址。所以 M1 是「文件即程序」，
   **不是**「有进程」。这条差距全部列在 §6。

一句话对答：**格式上我们跑的就是可执行文件（ELF）；差距在地址空间，不在格式。**

---

## 2 开关、窗口与互斥

| 项 | 值 | 位置 |
|---|---|---|
| 开关 | `ARKRTOS_USE_ELF`（Kconfig 符号，默认 `n`） | `config/kconfig/Kconfig.features` |
| 依赖 | `ARKRTOS_USE_VFS=1` 且 `ARKRTOS_USE_PRIV=1`，否则 `#error` | `arkrtos_elf.c` |
| 窗口 | `ARKRTOS_ELF_WINDOW_BASE` / `_SIZE`，缺失任一 `#error` | 板级配置文件 |
| LA64 QEMU 实测取值 | `0x02000000` / 2 MB（= `ARKRTOS_QEMU_APP_BASE` / `APP_SIZE`） | `board/qemu_loongarch64/arkrtos_board_config.h` |

关档时：`arkrtos_elf.c` 整体编成空目标（`#if (ARKRTOS_USE_ELF == 1)` 包整文件），
`arkrtos_elf.h` 的每个接口返回 `-1`/`0`，shell 的 `exec` 命令与
`/path/prog` 回落都不注册。**体积与行为零变化**。

### 2.1 与 PLV3 探针任务互斥（明写的取舍）

装载窗口和「PLV3 特权隔离探针」用的是**同一块 2 MB**。两者不能同时跑：

* 打开 ELF 时，例程不注册探针任务，并且**在启动日志里明说原因**：
  `[PRIV] PLV3 probe task skipped: ARKRTOS_USE_ELF owns the app window`；
* 反过来，`ARKRTOS_USE_ELF=0` 时探针照旧（`main.c` 里由 `ARK_EXAMPLE_PLV3_PROBE`
  这一个宏统一改判，不让两处判断各自漂移）。

这是一条**可辩论的取舍**：探针证明的是"页表 + PLV 真的拦得住"，ELF 演示证的是
"文件能变成程序"。M2 把每进程页表做出来之后，两者才能同时在场（探针占一个进程、
ELF 程序占另一个）。

---

## 3 装载流程（两遍法，不分配任何新内存）

`arkrtos_elf_exec(path, argc, argv)`：

1. **读头**：`vfs_read_at` 读 64 B `Ehdr`，`elf_check_header()` 校验
   magic / `EI_CLASS=2`(64 位) / `EI_DATA=1`(小端) / `e_machine=258`(LoongArch) /
   `e_type=2`(`ET_EXEC`) / `e_phentsize=56` / `1<=e_phnum<=16`。
   出现 `PT_INTERP`（动态链接）直接 `ERR_FORMAT`——M1 不支持加载器链。
2. **第一遍：只校验不写**。逐个 `PT_LOAD` 检查 `p_memsz>=p_filesz`、
   `[p_vaddr, p_vaddr+p_memsz)` 完整落在窗口内、段数不超过 16。
   这一步的意义是**要么全装、要么一个字节都不写**（窗口里可能还留着上一次的残段，
   半装进去的程序比装不上更危险）。
3. **第二遍：真写**。`vfs_read_at` 逐段拷 `p_filesz`，再把 `p_memsz-p_filesz`
   这段（`.bss`）清零。
4. **建栈与 argv**（全部在窗口顶部，不碰内核堆）：
   `win_top` 往下写参数字符串 → 记 `addrs[]` → 构造 `argc / argv[argc] / envp[0]=0`
   数组 → `sp = argv_start`。字符串预算 `ARKRTOS_ELF_ARGV_SIZE = 16 KB`，
   参数上限 `MAX_ARGS=16`、单条 `MAX_ARG_LEN=256`。
5. **起任务**：`stack_bottom = argv_start - 32 KB`，
   `arkrtos_task_register(entry, stack_bottom, 32 KB, 优先级 12, 0)`，
   然后把 `is_priv` 置 0（非特权）——**ELF 程序永远以 PLV3 跑**，
   它在窗口里写自己的 `.data`/栈，出窗口就撞 TLB。
6. **单实例**：`s_elf_task != 0` 时返回 `ERR_BUSY`；结束时（任务死亡检测）
   清 `s_elf_task`。

### 3.1 入口约定：`sp` 指向 `argc`

与 Linux 同构：

```
sp -> [ argc ][ argv[0] ][ argv[1] ]..[ 0 ][ envp[0]=0 ]
```

**坑（必须写进写程序的人脑子里）**：`_start` 里不能是普通 C 函数直接读 `sp`，
因为编译器序言会先 `addi.d $sp,$sp,-128`。`hello.c` 用的垫片是：

```c
__asm__(".section .text._start,\"ax\"\n.globl _start\n.type _start, @function\n"
        "_start:\n  move $a0, $sp\n  bl hello_main\n1:\n  b 1b\n"
        ".size _start, .-_start\n.previous\n");
```

不这样写，用户程序拿到的是 `top-0x80`，`argc` 会读成 0。

### 3.2 与内核对话：SVC 参数块是 32 位字

内核的 SVC 协议是 **32 位字**（`ARKRTOS_SVC_ARG/RET/NUM` 全 `uint32`），
64 位程序必须自己用 `uint32 params[N]` 并**把指针截断到 32 位**（窗口与栈都在低 4 GB，
所以截断是安全的，但不是"可以随便传 64 位指针"）。踩过的坑：用
`unsigned long params[4]` 传设备名，内核按 `uint32*` 解，`p[1]` 读到高半字 0，
`open("ttyS0")` 返回 `-1`。改成 `uint32` 后 `dev_open rc=18874368 (0x01200000)`。

用到的服务：`ARKRTOS_SVC_DEV_IO(0x10)` 的 `OPEN/READ/WRITE/CTRL/CLOSE`，
`ARKRTOS_SVC_THREAD_CTRL(0x1B)` 的 `CREATE/SELF/EXIT`（`EXIT` 不带 code）。

---

## 4 shell 上的两个入口

| 形态 | 行为 |
|---|---|
| `exec <path> [args...]` | 装载并**等它跑完**（上限 `SHELL_EXEC_WAIT_MS=30 s`，每 20 ms 轮询一次），不是 Linux 的"替换自身" |
| 直接敲 `/path/prog [args]` | 命令查表未命中时由 `ark_shell_eval.c` 回落到 `exec`；只接 `/x`、`./x`、`../x` 三种形态，别的一律按"未知命令"报错 |

两者打印同一行装载摘要：

```
[ELF] /mnt/ram/hello: 1732 B, 2 segment(s), entry 0x02000000, sp 0x021FC000, argc 3, task 7
[ELF] /mnt/ram/hello: finished after 0 ms
```

`1732 B` 是**各 `p_filesz` 之和**，不是文件长度（文件 6344 B 里含符号前的对齐、
节头表、`.comment` 等不装载内容）。这个口径差被误判过一次"装载器少装了 4 KB"，
在此写死：**要判文件长度看 `ls`，要判装载量看这一行。**

---

## 5 实测记录（QEMU la64，2026-11-16）

### 5.1 三条入口全部跑通

```
[ELF] demo task id=4 (writes /mnt/ram/hello and runs it)
[ELF] write /mnt/ram/hello rc=0 size=6344
...
mark@arkrtos:~$ [ELF] exec rc=0
[hello] entry sp=35635200 argc=3          <- 35635200 = 0x021FC000
[hello] hello from an ELF64 program
[hello] argc=3
[hello] argv[0]=hello
[hello] argv[1]=world
[hello] argv[2]=42
[hello] bye
TASK:4599 task 7 exits (self kill)
[ELF] demo program finished after 20 ms; try: exec /mnt/ram/hello a b c
```

`exec /mnt/ram/hello one two` 与直接敲 `/mnt/ram/hello direct` 各跑一遍，输出同上、
argv 分别是 `one/two` 与 `direct`。另跑一轮连续 10 次 `exec`：11 次"任务退出"
（含演示任务自跑那一次）、11 个 `bye`、零 `FATAL`。

### 5.2 排障记录：第 2 次 `exec` 必崩 `KERNEL refill`——根因是软定时器栈

这条必须留在文档里，因为它看起来像装载器的 bug，实际上和装载器一点关系都没有。

**现象**（确定性复现）：

* 空跑 45 s 不崩；
* 走 shell 跑 `exec /mnt/ram/hello`，**第 2 次必崩**（第 1 次是演示任务自跑）：

```
TASK:4599 task 7 exits (self kill)
[LA64U] KERNEL refill (plv0) badv=0x01000000 era=0x0020b9d8 -> fatal
[Ark RTOS] FATAL: estat=0x000b0800 ecode=0x0000000b era=0x02000558 ...
```

**定位过程**（按决策力排序，前两条是否定证据）：

1. `ecode=0x0b/era=0x02000558` 是**上一次 `syscall` 留下的陈旧 ESTAT**（本移植把
   0xB 当 SYS，与 QEMU 的 `EXCCODE_SYS` 一致），不是崩溃现场；TLB refill 只更新
   `TLBRERA/TLBRBADV/TLBRPRMD`。真正的现场是 `badv=0x01000000 era=0x0020b9d8`。
2. 把 `era` 落到符号：`0x0020b9d8` 在 `hwsafe_find_fn` 的查表循环里，循环上界是
   `g_hwsafe_dev_n`。读出来 `dev_n = 2210696 (0x0021bca8)`——**一个代码地址**，
   而正常值是 0（`hwsafe: devs=0`）。于是循环一路走到未映射的 `0x01000000`。
3. `0x0021bca8` 在 `mdk_trace_event` 内。谁把**代码地址**写进了一个 `int32` 计数器？
   看 `.bss` 布局：`g_hwsafe_dev_n` 在 `0x304BA8`，正好是
   `g_hwsafe_dev[-1].fn` 的地址，也正好在 `arkrtos_timer_task_stack`（`0x304C30`，
   96 字 = 384 B）**下方 136 B**。
4. `task` 命令报任务 5（`prio=200` = 软定时器服务任务）`peak/low = 0x304C60`，
   即休眠前已用掉 336 B、只剩 48 B。它一旦被调度回来，第一个 tick 中断把
   **256 B 的陷阱帧写到 `sp-256 = 0x304B60`**，帧内第 18 个字正好落在 `0x304BA8`。

**根因**：LA64 的陷阱帧直接压在被中断任务的栈上，一帧 256 B；软定时器服务任务的
栈只有 384 B，峰值已用 336 B，一帧就吃穿并向下溢出 208 B，把紧邻的
`g_hwsafe_dev_n` 写成了它自己保存的一个返回地址。之后**下一次**有人调
`hwsafe_find_fn`（ELF 程序 `open("ttyS0")` 就会）才炸 —— 所以崩点看起来在第 2 次 exec。

**验证与修法**：

* 反证：把该栈临时放大 8 倍（768 字），连续 6 次 `exec` 无崩溃（96 字必崩）；
* 修法：`board/qemu_loongarch64/arkrtos_board_config.h` 与
  `board/la32_generic/arkrtos_board_config.h` 板级覆盖
  `ARKRTOS_TIMER_TASK_STACK_WORDS`（LA64 = 512 字 = 2 KB，LA32 = 256 字 = 1 KB），
  判据是「**峰值 + 一整个陷阱帧 + 四层处理链（4×128 B）**」还留余量；
* 修后：连续 10 次 `exec` + 直敲路径全部通过，`hwsafe: devs=0 owners=2`
  不再被写坏（此前 N 次运行里它一直是 `devs=2210696`）。

**可复用结论**：在这个移植里，**任何"会跑用户回调的小栈"都必须按
「峰值 + 陷阱帧 + 处理链」给足**，而不是按"这个任务自己只调两三个函数"给。
`arkrtos_config.h` 的 96 字默认是给 32 位端口（帧 128 B）留的，64 位端口一律要更大。

---

## 6 与 Linux 的差距（M2 欠账表）

| # | 差距 | 现状（M1） | 影响 | 归属 |
|---|---|---|---|---|
| U-1 | 每进程地址空间 | 只有一块 2 MB 静态窗口，固定基址 `0x02000000` | 程序必须按该基址链接；两个程序不能同时在场 | M2 |
| U-2 | 同 VA 多实例 | 不支持；第二个 `exec` 返回 `ERR_BUSY` | 不能"同时开两个 hello" | M2 |
| U-3 | 页帧分配器 / 动态映射 | 无：段直接写进窗口 | 程序镜像必须能整块装进 2 MB | M2 |
| U-4 | `arkrtos_elf.c` 只支持 64 位 `long` | LA32（32 位）编出空目标（`#error` 保护） | LA32 后端暂时没有 ELF | M2 |
| U-5 | 缺页处理（按需换入） | 无：装载时一次性写满 | 镜像必须整块驻留 RAM | M2 |
| U-6 | `copy_from_user` 式的受控内核访问 | 无：内核直接 `memcpy` 到 PLV3 窗口（`LA_PTE_PLV(3)` 让它可行） | 用户指针的合法性靠"窗口内 + SVC 参数块 32 位"两条约定，不是逐次校验 | M2 |
| U-7 | ASID / TLB 批量失效 | ASID 恒 0 | 换进程要刷整表；SMP 下要 shootdown | M2 |
| U-8 | 退出码通道 | `THREAD_EXIT` 不带 code，TCB 无字段；只打印 `finished after N ms` | 脚本没法判成败 | M2 |
| U-9 | `fork` / 真正的 `exec` 替换自身 | 没有：`exec` 是"起一个任务并等它" | 与 POSIX 语义不同（已在 shell 帮助里写明） | M3 |
| U-10 | 动态链接（`PT_INTERP`、`ld.so`） | 明确拒绝（`ERR_FORMAT`） | 只能跑静态链接程序 | M3 |
| U-11 | 文件描述符继承 / 管道 / 信号 | 无 | 只能单进程自娱自乐 | M3 |
| U-12 | 软定时器栈的 32 位侧复核 | RV32 仍是 96 字：实测峰值 184 B 已用、剩 200 B，RV32 帧 ≈128 B ⇒ 处理链只剩 ~72 B | 与本次 LA64 同型的风险，尚未出事 | **本轮登记，单独一轮测完再定值**（改它会动 `2012924 B` 镜像基线） |
| U-13 | 栈溢出 / 陷阱帧越界的检测 | **没有**：`arkrtos_task_stack_init` 会把 `ARKRTOS_STACK_END_FLAG (0xed01)` 写进 `stack_bottom[0]`，但**从来没有人检查过它** | 就是本次事故能潜伏整轮的原因 | 建议做成 `ARKRTOS_USE_STACK_GUARD`（默认关=零变化），在切换/异常入口校验 |

---

## 7 复现步骤

```bash
cd "/d/工作/git_project/ark rtos"

# 1) 构建 LA64（Makefile 会把 hello.c 编成 ELF64 再转成 C 数组编进镜像）
make -f example/qemu_loongarch64/Makefile -j8
ls -l build/qemu_loongarch64/ark_qemu_la64.elf

# 2) 跑 QEMU，用管道喂命令（QEMU 没有宿主文件注入通道，
#    hello.elf 是靠 elf_blob.py 变成数组编进镜像、由演示任务写进 ramfs 的）
python - <<'PY'
import subprocess, time
qemu = r"D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe"
p = subprocess.Popen([qemu, "-machine", "virt", "-kernel",
                      "build/qemu_loongarch64/ark_qemu_la64.elf", "-nographic"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
time.sleep(9)
for c in ["exec /mnt/ram/hello one two\n",
          "exec /mnt/ram/hello a0 b\n",
          "exec /mnt/ram/hello a1 b\n",
          "/mnt/ram/hello direct\n",
          "hwsafe\n"]:
    p.stdin.write(c); p.stdin.flush(); time.sleep(2.5)
p.stdin.write("\x01x"); p.stdin.flush()
print(p.communicate(timeout=10)[0])
PY

# 3) 判据（缺一不可）
#    - 每个 exec 都有 [hello] ... bye，且 [ELF] ... finished
#    - 全程没有 "[LA64U] KERNEL refill" / "FATAL"
#    - 最后 hwsafe 报 "devs=0 owners=N"，dev_n 不是天文数字
```

装载器门禁与整体门禁：

```bash
python tools/elf_gate.py      # 默认档零变化 / LA64 正向编得过 / 机器码/类型/窗口缺失都要被拒
python tools/ci_gate.py       # 37 项，含 paging 与 elf 两组
```

---

## 8 写一个能跑的程序（最小清单）

1. 链到窗口基址：`-Ttext=0x02000000 -e _start -Wl,-z,max-page-size=4096 --strip-all`；
2. `_start` 用汇编垫片把 `sp` 传进 C（§3.1），别让编译器序言先动 `sp`；
3. 用 `uint32` 参数块调 SVC（§3.2），指针截断到 32 位；
4. 退出用 `ARKRTOS_SVC_THREAD_CTRL` 的 `EXIT`（无 code）；
5. 程序里**不要**假设自己是 PLV0：它在 PLV3，碰窗口外的地址会立刻被 TLB 拦住。

`example/qemu_loongarch64/elf/hello.c` 是一份可编译的模板（含 `elf_blob.py` 打包步骤）。
