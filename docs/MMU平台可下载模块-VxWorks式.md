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
Ark> ld /mnt/ram/modhello.o a b
Ark> lkup                      列出全部导出符号
Ark> lkup arkrtos_mod_printf   查一个
Ark> modules                   列出常驻模块（base/size/entry/relocs/imports/task/state）
Ark> unld 0                    卸载（模块任务还在跑时返回 BUSY，不硬拔）
```

返回码（`arkrtos_mod_error_name()` 给名字）：

```text
0 OK / -1 PARAM / -2 FS / -3 FORMAT / -4 ARCH / -5 LAYOUT
-6 SYMBOL / -7 RELOC / -8 NOENTRY / -9 NOMEM / -10 BUSY
```

`-6` 时 `arkrtos_mod_missing_symbol()` 给出缺的那个名字，`-7` 时
`arkrtos_mod_bad_reloc()` 给出类型号——**报错要能直接指到原因**，不给一个笼统的失败。

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
* E Kconfig 与板级接线（`depends` 有 VFS 与 PRIV、help 写明「不是替代」）。

**不覆盖**：运行期行为（真装载、真重定位、真调内核 API、`unld` 后再装、`BUSY` 拒绝）。
那些是 QEMU 读数，见 §5。门禁只保证「装得上、能编、导出表没漏、守卫没被拆」。
