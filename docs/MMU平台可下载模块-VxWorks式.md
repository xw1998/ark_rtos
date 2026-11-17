# MMU 平台上的可下载模块（VxWorks 式）与两种应用模型

本文记录 M2：**运行时装载、对内核符号表现场链接**的模块（VxWorks 的
`ld` / `lkup` / `modules` / `unld` 那一套），以及它与 M1 的 Linux 式
`exec`（[MMU进程模型与ELF装载.md](MMU进程模型与ELF装载.md)）的关系。

一句话：**两者是并列的两种应用模型，由 Kconfig 各自选择，可以单开也可以同开；
M2 不取代 M1。**

## 1. 两种模型对照

| | Linux 式（`ARKRTOS_USE_ELF`） | VxWorks 式（`ARKRTOS_USE_MODLOAD`） |
|---|---|---|
| 输入 | `ET_EXEC` 可执行文件（`.elf`） | `ET_REL` 目标文件（`.o`） |
| 地址 | 链接期定死（`-Ttext=0x02000000`） | 装载时由装载器决定（板上的池子） |
| 外部引用 | 自包含（要么静态链好，要么没有） | 按**名字**在内核导出表里查 |
| 特权级 | PLV3（与内核隔离） | PLV0（内核态，无隔离） |
| 谁来做 | 终端用户写程序 | 内核/驱动插件开发者 |
| 生命周期 | 跑完退出 | 装载后驻留，可 `modules` 查、`unld` 卸 |
| 出错代价 | App 崩，内核活 | 模块写坏指针，系统一起下去 |

这是 VxWorks 5.x 的经典取舍：**模块能直接调内核 API，所以也就能直接改内核内存。**
要墙就别开这个开关，用 `ARKRTOS_USE_ELF`。

2×2 矩阵四种组合都编得过（`elf_gate` / `modld_gate` 各自把「关」的方向也编一遍）：

| `USE_ELF` | `USE_MODLOAD` | 结果 |
|---|---|---|
| 0 | 0 | 两个装载器都是空 TU，体积与行为零变化 |
| 1 | 0 | 只有 `exec` |
| 0 | 1 | 只有 `ld` / `lkup` / `modules` / `unld` |
| 1 | 1 | 两条路都在（QEMU 例程的默认板级配置） |

## 2. Kconfig 怎么选

```text
config ARKRTOS_USE_MODLOAD
    bool "VxWorks-style downloadable modules (ld / lkup / modules / unld)"
    depends on ARKRTOS_USE_VFS && ARKRTOS_USE_PRIV
```

* 依赖 `VFS`：模块从文件系统里读（`/mnt/ram/modhello.o`）；
* 依赖 `PRIV`：模块跑 PLV0，需要内核态这条路本身是通的；
* 板级只要求给一个数：`ARKRTOS_MOD_AREA_SIZE`（模块池多大）。
  **窗口地址不要求板级给**——它由链接器给池子（`s_mod_pool`）定，
  这样就不会出现「开了开关却忘了改窗口宏 ⇒ 静默跳进地址 0」。

`board/qemu_loongarch64/arkrtos_board_config.h` 把两者都置 1，让 QEMU 例程一次
跑通两条路；别的目标由各自的 Kconfig / 板级决定。

## 3. 装载器做了什么

输入是一个 `.o`，装载器的四步对应它要证明的四件事：

1. **摆段**：把 `SHF_ALLOC` 的节按对齐摆进模块池的一个空槽（`ARKRTOS_MOD_SLOTS`
   个槽，每槽 `ARKRTOS_MOD_SLOT_SIZE`）；
2. **查符号**：每个未定义符号在 `kernelsrc/src/arkrtos_exports.c` 的 `s_exports`
   白名单里按**名字逐字**查地址，写进 GOT；
3. **重定位**：按 LoongArch64 的 SOP 栈机重定位改每条 `pcalau12i` / `ld.d` 的偏移；
4. **挂任务**：`module_main(argc, argv)` 作为任务入口起来，跑 PLV0。

导出表只有 6 个符号（`lkup` 可以自己看）：

```text
arkrtos_mod_printf           装载器自实现的 printf（%s %c %d %i %u %x %p %%，不做宽度/精度/浮点）
arkrtos_console_write        往控制台写字节
arkrtos_kernel_get_tick      取节拍
arkrtos_task_delay_internal  延时（微秒）
arkrtos_task_get_current     取当前任务
arkrtos_thread_exit_internal 结束当前任务
```

加一行的判据：**模块必须能靠它完成一件正当的事，且它不能让模块绕过内核的管理**
（改 TCB、删别的任务、写 MPU/页表寄存器的一律不进表）。也不导出平台侧的 `snprintf`：
那种符号换一块板就不存在，导出它等于给模块一个只在演示里成立的承诺。

### 3.1 支持的重定位类型

GCC 8.3 的 LoongArch 后端只发 SOP 栈机重定位（`-mexplicit-relocs` / `-mno-relax`
在这版工具链上都是 unrecognized，所以也别指望换选项绕开）：

| 编号 | 类型 |
|---|---|
| 22 | `R_LARCH_SOP_PUSH_PCREL` |
| 23 | `R_LARCH_SOP_PUSH_ABSOLUTE` |
| 25 | `R_LARCH_SOP_PUSH_GPREL` |
| 29 | `R_LARCH_SOP_PUSH_PLT_PCREL` |
| 30 / 31 / 32 / 33 / 34 / 35 | `ASSERT` / `NOT` / `SUB` / `SL` / `SR` / `ADD` |
| 40 / 42 / 43 / 45 / 46 | `POP_32_S_10_12` / `_S_10_16_S2` / `_S_5_20` / `_S_0_10_10_16_S2` / `_U` |
| 1 / 2 | `R_LARCH_32` / `R_LARCH_64` |

外部数据访问的表达式是 `PCREL(_GLOBAL_OFFSET_TABLE_ + 0x800) + GPREL(sym)`，
所以 `gp = got_base + 0x800`。**不认识的重定位类型直接报错并把类型号带出去**
（`arkrtos_mod_bad_reloc()`），不猜编码。

## 4. 用法

```text
mark@arkrtos:~$ ld /mnt/ram/modhello.o a b
mark@arkrtos:~$ lkup                      列出全部导出符号
mark@arkrtos:~$ lkup arkrtos_mod_printf   查一个
mark@arkrtos:~$ modules                   列出常驻模块（base/size/entry/relocs/imports/task/state）
mark@arkrtos:~$ unld 0                    卸载（模块任务还在跑时返回 BUSY，不硬拔）
```

返回码（`arkrtos_mod_error_name()` 给名字）：

```text
0 OK / -1 PARAM / -2 FS / -3 FORMAT / -4 ARCH / -5 LAYOUT
-6 SYMBOL / -7 RELOC / -8 NOENTRY / -9 NOMEM / -10 BUSY
```

`-6` 时 `arkrtos_mod_missing_symbol()` 给出缺的那个名字，`-7` 时
`arkrtos_mod_bad_reloc()` 给出类型号——**报错要能直接指到原因**，不给一个笼统的失败。

### 4.1 在 QEMU 里交互式实测（可复跑的操作步骤）

上板前先在 QEMU 上把 `ld / lkup / modules / unld` 走一遍。难点不在命令，而在
**怎么把键盘输入送进去**——这两条路实测都不通，别浪费时间：

* `-serial file:xxx.log` 只能收输出，**喂不进输入**；
* `-nographic`（= `-serial mon:stdio`）配管道 stdin：字符有回显，但**回车不生效**，
  命令一次都没执行过（monitor 复用层的问题）。

能用的只有 **TCP chardev**：QEMU 监听本地端口，脚本连上去按字节发，回车发 `\r`。

**① 先有 ELF**（模块池里要有 `modhello.o`，`mod_demo.c` 启动时会写进去）：

```bash
make -f example/qemu_loongarch64/Makefile -j
# 产物：build/qemu_loongarch64/ark_qemu_la64.elf
```

**② 起 QEMU**（注意 `cwd`）：QEMU 在 Windows 上打不开含非 ASCII 的路径
（`-kernel "D:/工作/git_project/..."` 会得到 `could not load kernel: Failed to load ELF`），
所以**切到 elf 所在目录、`-kernel` 只给文件名**：

```bash
cd build/qemu_loongarch64
"$QEMU" -machine virt -kernel ark_qemu_la64.elf \
        -display none -monitor none \
        -serial tcp:127.0.0.1:45553,server=on,wait=off
```

两点口径：
`server=on,wait=off` —— 先起跑再等客户端，代价是**连上之前的开机输出（含第一条提示符）
会丢**，所以客户端要**先敲回车探活**；`wait=on` 实测接上后命令进不了 shell，不要用。

**③ 连上去敲**（`nc` / 任意 socket 客户端；每行结尾发 `\r`）：

```text
lkup                          ↑ 回车用 CR（'\r'），LF 也行：ark_shell.c 411/472 行两个都收
lkup arkrtos_mod_printf
ld /mnt/ram/modhello.o alpha 42
modules
unld 0
modules
```

期望看到（节选，内核日志 `[A]/[B]` 会插进来，属正常）：

```text
mark@arkrtos:~$ lkup
kernel exports (6):
  arkrtos_mod_printf ...
mark@arkrtos:~$ ld /mnt/ram/modhello.o alpha 42
[modhello] hello from a relocatable module
[modhello] argc=3
[modhello] argv[2]=42
[MOD] modhello.o: base 0x00312320, 544 B, entry 0x00312320, 165 reloc(s), 8 import(s), task 8
mark@arkrtos:~$ unld 0
unld 0: slot released
```

**提示符是 `mark@arkrtos:~$ `，带 ANSI 颜色**（`\x1b[1;32m...\x1b[0m`）。
所以拿它当判据时必须**先去 ANSI 转义再找 `@arkrtos`**，直接 `grep "Ark>"` 一定落空。

**④ 一键跑完（推荐）**：

```bash
python tools/qemu_shell_drive.py --modld-check
```

它自己起 QEMU、用 TCP 串口连上、发回车等提示符，然后按顺序执行 7 条命令并逐条
校验期望子串（导出表 6 个符号 / `lkup` 单查 / `modules` 槽表 / `ld` 的 argv 与
`165 reloc(s), 8 import(s)` / `unld 0: slot released` / 卸载后 `modules` 只剩另一槽）。
全过时末行是 `SHELL-DRIVE OK: 7 条命令全部符合预期`，退出码 0。

其它开关：`--cmd "..."`（只发不判，手工看）、`--script cmds.txt`（一行一条，`#` 跳过）、
`--timeout 3`（每条命令等输出的秒数——**按时间收口**，因为每秒一行的内核日志
会插在提示符后面，靠「行尾是 `$`」判结束不可靠）、`--port` / `--elf` / `--qemu`。

**这一项不进 `ci_gate`**：它依赖 QEMU 与真跑内核，不是「一键静态检查」能承担的东西，
属人工/专项复跑。`tools/modld_gate.py` 只做静态那半边（见 §7）。

## 5. QEMU 实测读数（LoongArch64，2026-11-17）

`example/qemu_loongarch64/mod_demo.c` 与 `elf_demo.c` 并列注册，启动后两条路各跑一次：

```text
[MOD] write /mnt/ram/modhello.o rc=0 size=6224
[modhello] hello from a relocatable module
[modhello] argc=3
[modhello] argv[0]=modhello.o
[modhello] argv[1]=alpha
[modhello] argv[2]=42
[modhello] tick 153 -> 168, calls=1
[modhello] bye
[MOD] ld rc=0 [base=3203872 size=544 entry=3203872 relocs=165 imports=8 task=8]
[MOD] module finished after 0 ms; try: ld /mnt/ram/modhello.o a b
```

`base == entry`（模块区第一个槽）、`size=544`、`relocs=165`、`imports=8` 与
`modhello.o` 的静态读数（6224 B / `.text` 304 B / 165 条重定位 / 8 个未定义符号）对得上。

同一次运行里 `[ELF] exec rc=0` / `[ELF] demo program finished` 也照常出现 ——
**两种模型同时开、互不影响**，这是本轮的验收判据之一。

## 6. 本轮的三个真 bug（都只在真机上现形）

1. **模块池地址不能信宏**。第一版用 `ARKRTOS_MOD_AREA_BASE` 算槽地址，
   而链接器实际把 `s_mod_pool` 放在别处 ⇒ `ecode=0xD (INE) era=0x00600000`
   （跳进全 0 指令）。改成 `c.base = (uint32)c.img` 并删掉 BASE 宏要求后消失。
   **能被链接器决定的东西，就不要让宏再决定一次。**
2. **`argv` 必须是独立的指针向量**。把 inline 字符串数组强转 `char**` 交给模块，
   模块读 `argv[i]` 会读到字符串内容本身 ⇒ `ADEF badv=0x68646f6d`（`"modh"`）。
   修法是单独的 `s_mod_argp[slot][i]` 指针向量。
3. **装载器在任务栈上是重活，装一次要 4 KB 量级**。最初演示任务栈只有 512 字
   （2 KB），而一次 `arkrtos_mod_load()` 实测峰值 **3080 B**（读 ELF、解析节、
   拷段、165 条重定位、逐条查符号）。栈向下溢出后打穿了**紧贴其下方**任务的保存帧，
   于是模块跑完约 1 秒后确定性 FATAL（`estat=0xD era=0x0032f868`，正是被写坏的
   帧里 `ERA` 落进了 lfs 的数据结构）。诊断手法与判据：
   * 内核里挂「被动帧看门狗」在每个节拍校验所有非当前任务的保存帧，抓到
     `[WATCH] task 4 frame became bad at tick=129 cur=5` —— 坏在模块代码开始执行**之前**，
     责任由此锁定在装载器而不是模块；
   * 修法：示例栈抬到 2048 字（8 KB）；装一次要 ≥ 4 KB 这条写进头文件与本节。
   `shell` 里的 `ld` 也一样——**给 shell 任务留够栈**，别让两个任务的栈贴在一起。

## 7. 门禁与不覆盖的部分

`tools/modld_gate.py`（`tools/ci_gate.py` 第 38 项）：

* A 默认档 `ARKRTOS_USE_MODLOAD == 0`；
* B LA64 打开编得过，且**预处理结果里真有 `arkrtos_mod_load`**（防「开着却空 TU」）；
* C **符号对账**（防假绿的核心）：把 `mod/modhello.c` 真编成 `.o`，`nm -u` 取未定义符号，
  逐个必须在 `s_exports` 里；
* D 守卫 pin：`ET_REL` 判定、`mod_read_word` / `mod_write_word` 有界、
  默认分支报错带类型号、缺 `ARKRTOS_MOD_AREA_SIZE` 必须 `#error`；
* E Kconfig 与板级接线（`depends` 有 VFS 与 PRIV、help 写明「不是替代」）；
* F **32 位负向**：RV32（`riscv64-unknown-elf-gcc -march=rv32imac`）与
  LA32（`loongarch32-newlib-elf-gcc -march=loongarch32`）各一条，强开
  `-DARKRTOS_USE_MODLOAD=1` 必须**编不过**，且错误必须是「只支持 64 位 port」。
  注意两条都要**先把 `ARKRTOS_MOD_AREA_SIZE=65536` 喂饱**——否则先炸的是
  「缺窗口」那条 `#error`，这道负向就被别的原因蒙对了。

装载器读写的是 ELF64 的 RELA / 符号表（64 位字段、`r_info` 高 32 位取符号下标），
32 位 `long` 上表达不了，所以**它是 LoongArch64 专用**：32 位 port 打开开关
会在 `arkrtos_modld.c` 顶部被编译期挡住，不是运行期才出错。

**不覆盖**：运行期行为（真装载、真重定位、真调内核 API、`unld` 后再装、`BUSY` 拒绝）。
那些是 QEMU 读数，见 §5。门禁只保证「装得上、能编、导出表没漏、守卫没被拆」。
