# 真实发行版 LA64 二进制：需求清单与差距

> 载体：QEMU LoongArch64（`board/qemu_loongarch64`）。**QEMU 算上板，不是真机。**
> 仪器：`tools/elf/realbin_probe.py`（探针）+ `tools/realbin_gate.py`（门禁 7 项）。
> 读数：`ci_gate` 里对应 `realbin` 一项（进 `--remote` 时按本地工具链跳过）。

---

## 1. 一句话结论

**现成发行版二进制现在跑不起来，而且不是「补一个缺口」的差距。** 本轮交付的是
**清单 + 仪器**：拿 Ark 当前装载器真正支持的东西，去对真实二进制**逐项**算账，
把「要做什么」按依赖顺序排出来，并且让这张账本可以随时重算、并且有门禁盯着
（探针自己出错会被判红）。

差别不在体积，在于二进制**向内核要什么**：落在哪个地址、有没有 `PT_TLS`、
动态段里有没有 `GNU_HASH` / 符号版本 / `INIT_ARRAY`、重定位里有没有 TLS 与
`IRELATIVE`、依赖链有多深。这些东西 `readelf` 都能打，但 `readelf` **不会**
告诉你哪几条 Ark 现在做不到。

---

## 2. 这一页交付什么、不交付什么

| | 状态 | 证据 |
| --- | --- | --- |
| 「真实二进制要什么」的可复算清单 | **已交付** | 探针两份真样本实测，见 §4 |
| 探针自身不会把坏输入判绿、不会和能力表各说各话 | **已交付** | 门禁 7 项全过，见 §9 |
| 内核侧「哪些号真的被发出来」的按号直方图 | **已交付（开关默认关）** | QEMU 读数 11 个号，见 §8 |
| 真实发行版二进制**能跑起来** | **没有** | 六道门槛一道未过，见 §5 |
| 某个具体发行版镜像的完整 syscall 分布 | **没有** | 二进制跑不起来就采不到，见 §8 边界 |

分档口径照旧：**仅编译通过 / QEMU 实跑验证过 / 未验证**。本页所有数字都是
第二档（QEMU 实跑）或第三档（未验证，会明写）。

---

## 3. 探针怎么用

```bash
# 只看结构需求（不解析依赖）
py -3 tools/elf/realbin_probe.py hello_dyn.elf hello_static.elf

# 连依赖链一起算：sysroot 里找 libc.so.6 等 DT_NEEDED
py -3 tools/elf/realbin_probe.py hello_dyn.elf --sysroot <sysroot 目录>

# 机器可读
py -3 tools/elf/realbin_probe.py hello_dyn.elf --sysroot <dir> --json
```

**退出码语义（这是这条仪器最容易读错的地方）**：

- `0` = 这个二进制只用 Ark 已经支持的东西，且落在当前窗口里；
- `1` = 有 blocker，`blockers` 列表就是「要做什么」，顺序即依赖顺序；
- **`1` 是探针的正常结果，不是脚本出错。**

探针是**纯 `struct` 解析**，不 shell 出去调 `readelf`，所以门禁在任何机器上都能跑；
但重定位名字表必须与 binutils 对账——这件事由门禁的 `reloc-name-audit` 做
（`loongarch64-linux-gnu-readelf` 不在就 **SKIP 并打印原因**，不静默通过）。

`--sysroot` 是递归的（深度 3，循环依赖标 `cyclic`）：主程序的 `DT_NEEDED`
先在 `usr/lib64` / `lib64` / `usr/lib` / `lib` / `usr/lib64/lp64d` /
`lib64/lp64d` / `usr/lib/loongarch64-linux-gnu` 里找，找到再递归算它自己的
blocker。**「依赖存在」本身不是 blocker；「依赖自身也有 blocker」才是。**
sysroot 给错（不存在 / 是空目录）时探针明说，不静默按「没有依赖」处理。

---

## 4. 实测读数：两份真样本

样本用本地龙芯工具链编（`loongson-gnu-toolchain-8.3-i686-mingw-loongarch64-linux-gnu-rc1.4`，
glibc rc1.4 / gcc 8.3.0，与发行版同源），**不下载 Alpine/Debian 镜像**：
路径可控、可复现、离线可用。

### 4.1 动态链接（`hello_dyn.elf`，20 464 B）

```
== hello_dyn.elf
  type=ET_EXEC machine=258 entry=0x120000570 size=20464 B
  PT_LOAD x2  span=32896 B  [0x120000000, 0x120008080)  max_align=0x4000
  PT_INTERP: /lib64/ld.so.1
  PT_TLS: no   RELRO=True   exec-stack=False
  DT_NEEDED: libc.so.6
  hash: gnu=True sysv=True | versym=True verneed=True | init_array=True | relr=False | bind_now=False
  relocs: R_LARCH_64 x5, R_LARCH_JUMP_SLOT x3
  symbol versions needed: libc.so.6:GLIBC_2.27
  dep libc.so.6 -> <sysroot>/usr/lib64/libc.so.6  1571208 B  INTERP=/lib64/ld.so.1
      自身 blocker: [interp-path] PT_INTERP 指向 /lib64/ld.so.1，VFS 里没有这个路径;
      [tls] 有 PT_TLS（filesz=16 memsz=144 align=8）;
      [dynamic-tags] 动态段里有 Ark 没实现的标签：INIT, SONAME, INIT_ARRAY, INIT_ARRAYSZ,
      FLAGS, GNU_HASH, VERSYM, RELACOUNT, VERDEF, VERDEFNUM, VERNEED, VERNEEDNUM;
      [relocations] 有 Ark 不处理的重定位：R_LARCH_IRELATIVE x20, R_LARCH_TLS_TPREL64 x17
  blockers (4, 按依赖顺序):
    1. [address] PT_LOAD 覆盖 [0x120000000, 0x120008080)，当前窗口是 [0x2000000, 0x2200000)
    2. [interp-path] PT_INTERP 指向 /lib64/ld.so.1，VFS 里没有这个路径
    3. [dynamic-tags] 动态段里有 Ark 没实现的标签：
       INIT_ARRAY, FINI_ARRAY, INIT_ARRAYSZ, FINI_ARRAYSZ, GNU_HASH, VERSYM, VERNEED, VERNEEDNUM
    4. [needed] 被依赖对象自身也有 blocker：libc.so.6(interp-path/tls/dynamic-tags/relocations)
```

注意三件容易被忽略的事：

- **主程序自己很干净**（5 条 `R_LARCH_64` + 3 条 `R_LARCH_JUMP_SLOT`），
  真正难的是 `libc.so.6`——1 571 208 B、1 353 条重定位
  （`R_LARCH_RELATIVE x1239, R_LARCH_64 x68, R_LARCH_IRELATIVE x20,
  R_LARCH_TLS_TPREL64 x17, R_LARCH_JUMP_SLOT x9`，与 `readelf -rW` 逐项一致）。
- `R_LARCH_IRELATIVE` 是 IFUNC：glibc 用它给 `memcpy`/`strlen` 选实现，
  **不是「多一种重定位」而是「装载期要跑用户代码」**。
- `symbol versions needed: libc.so.6:GLIBC_2.27` ⇒ 就算把所有符号都导出成同名，
  没有 `VERSYM/VERNEED` 匹配也会绑错版本。

### 4.2 静态链接（`hello_static.elf`，623 728 B）

```
== hello_static.elf
  type=ET_EXEC machine=258 entry=0x1200008e0 size=623728 B
  PT_LOAD x2  span=572216 B  [0x120000000, 0x12008bb38)  max_align=0x4000
  PT_INTERP: (none, static)
  PT_TLS: yes   RELRO=True   exec-stack=False
  DT_NEEDED: (none)
  hash: gnu=False sysv=False | versym=False verneed=False | init_array=False | relr=False | bind_now=False
  relocs: (none)
  blockers (2, 按依赖顺序):
    1. [address] PT_LOAD 覆盖 [0x120000000, 0x12008bb38)，当前窗口是 [0x2000000, 0x2200000)
    2. [tls] 有 PT_TLS（filesz=32 memsz=96 align=8）
```

静态是**离得最近的一档**：没有动态标签、没有重定位、没有依赖链，只剩
「地址」与「TLS」两道。它的 623 728 B 从 2026-11-20 起已经装得进
`/mnt/ram`（卷从 512 KiB 扩到 4 MiB，见 §5 第 6 条），但探针仍然报
`[address]` 与 `[tls]` 两条 blocker（`[tls]` 是静态描述：装载器在开着
`ARKRTOS_USE_ELF_TLS` 时已经能把 TLS 段铺进任务空间，见 §5.2，真正拦住它的
是 `[address]`）。

---

## 5. 六道门槛

| # | 门槛 | 真实二进制要什么（实测） | Ark 现在 | 还差什么 | 判据（探针标签） |
| --- | --- | --- | --- | --- | --- |
| 1 | 地址空间 | `[0x120000000, 0x120008080)`（动态）/ `[0x120000000, 0x12008bb38)`（静态） | **地址宽度已升到 64 位（2026-11-20）**：判据里不再有 `p_vaddr > 0xFFFFFFFF`，窗口 VA 与物理帧解耦（`make ... WIN_VA=0x120000000` 把窗口基址、内核链接基址与夹具基址一起抬）。宽窗口档 QEMU 实跑 `linux_hello` `PASS` + `exit 7`，无 fatal（见 §11 与 [MMU进程模型与ELF装载](MMU进程模型与ELF装载.md) §11） | 每进程独立 VA 空间 + 运行期建页表；以及**把真实静态 hello 真装进宽窗口跑一遍**（本轮还没有这个读数）；native SVC 参数块仍是 `uint32[]`，宽 VA 下 App 必须走 Linux ABI 传指针 | `[address]` |
| 2 | 解释器 | `PT_INTERP = /lib64/ld.so.1` | 默认档直接拒（`ERR_FORMAT`）；开 `ARKRTOS_USE_DYNLOAD` 后能装**本仓** `ld-ark.so`（`/mnt/ram/ld-ark.so`） | 真 glibc `ld.so` 的能力面（见 §7） | `[interp-path]` |
| 3 | TLS | `PT_TLS`（动态继承自 libc：16/144；静态：32/96）+ 17 条 `R_LARCH_TLS_TPREL64` + `set_tid_address` | 默认档 `arkrtos_elf.c` **完全不读 `PT_TLS`**；开着 `ARKRTOS_USE_ELF_TLS` 时读段、铺 `tdata`、清 `tbss`、把 `$r2` 指过去（2026-11-20，静态 local-exec，见 §5.2） | 真 libc 的 TCB 布局、`R_LARCH_TLS_TPREL64` 重定位、`set_tid_address` 语义 | `[tls]` |
| 4 | 动态段标签 | `INIT_ARRAY`/`FINI_ARRAY`、`GNU_HASH`、`VERSYM`/`VERNEED`/`VERDEF`、`RELACOUNT`、`FLAGS` | `ld-ark.so` 只处理 `NEEDED/STRTAB/SYMTAB/RELA/RELASZ/RELAENT/STRSZ/SYMENT/JMPREL/HASH` 这几项 | 构造函数执行、符号版本匹配、GNU 哈希、`DF_1_NOW` 语义 | `[dynamic-tags]` |
| 5 | 重定位 | `RELATIVE`、`64`、`JUMP_SLOT`、`IRELATIVE`(IFUNC)、`TLS_TPREL64` | 只做 `R_LARCH_64 / RELATIVE / JUMP_SLOT`（`ARK_SUPPORTED_RELOC`，门禁与 `ldso.c` 双向对齐） | IFUNC 解析器（装载期跑用户代码）+ TLS 重定位 | `[relocations]` |
| 6 | 依赖链与存储 | `libc.so.6` 1 571 208 B；静态 hello 623 728 B | `/mnt/ram` 是 **4 MiB** 的 RAM 块设备上的 littlefs（`ARKRTOS_QEMU_BLK_SIZE = 4 * 1024 * 1024`，2026-11-20 由 512 KiB 扩上来）；`CHIP_FLASH_SIZE` 2 MB、`CHIP_RAM_SIZE` 8 MB。**容量这道已实测通过**：开机写 640 KiB 文件再读回逐字节一致 | 只剩下依赖递归（`DT_NEEDED` 链的解析与装载） | `[needed]` |

两条要写死的读法：

- 第 6 条是**先决条件**，不是「顺便优化」。放不下就没得测，谈能力没意义。
  它在 2026-11-20 已经过一次实测（见下）。
- 第 1 条不解决，后面五条都采不到真实分布——这也是 §8 那句边界的根因。

### 5.1 存储这一道的实测读数（2026-11-20）

改的是板级一个常量与一处自检，不是「把窗口搬了」：

| 项 | 改前 | 改后 |
| --- | --- | --- |
| `ARKRTOS_QEMU_BLK_SIZE` | 512 KiB | **4 MiB** |
| 容量读数 | 无（只有 `size=524288` 一行） | 开机写 `/mnt/ram/cap.bin` 655 360 B 再读回，逐字节比对 |
| QEMU 输出 | — | `[FS] cap write rc=0 bytes=655360 read len=655360 -> match` |
| 镜像 `.bss` | 786 964 B | 5 112 340 B（+4 MiB 介质 +640 KiB 缓冲） |

为什么取 4 MiB：本机龙芯 glibc 工具链的静态 hello 623 728 B 与它依赖的
`libc.so.6` 1 571 208 B 加起来 2.19 MB，旧卷（512 KiB）连一份都放不下。
640 KiB 的容量文件比旧**整卷**还大，所以这条读数证明的是「卷真的能装下
比过去整卷更大的东西」，而不只是念了一遍容量宏。

**没有因此变绿的东西**：这一轮（存储）没有让任何一道门槛变绿——地址这道原样
未动，静态 hello 仍然跑不起来；存储只从「连门都进不去」变成「装得进去、装载器
明确拒绝并给理由」。TLS 是下一轮单独做的（§5.2）。

### 5.2 TLS 这一道的实测读数（2026-11-20）

这一轮把第 3 道从「完全不读 `PT_TLS`」推进到「静态 TLS 通路跑通、有 QEMU 读数」，
但**没有**做 TCB，所以它仍然不等于「发行版二进制能跑」。

| 项 | 读数 |
| --- | --- |
| 开关 | `ARKRTOS_USE_ELF_TLS`（默认 0，`depends on ARKRTOS_USE_ELF`；非 LoongArch 端口开档是编译期 `#error`） |
| 约定 | **静态 local-exec**：`TP = PT_TLS.p_vaddr + bias`，TPREL 偏移非负 |
| 约定怎么来的 | 两份互相独立的工具链（本仓 newlib 交叉 gcc、龙芯 8.3 glibc `loongarch64-linux-gnu-gcc`）各编同一份「两变量 TLS」夹具，反汇编都是 `lu12i/ori/lu32i/lu52i + add.d $r12,$r12,$r2`，偏移 **0**（`.tdata`）与 **4**（`.tbss`） |
| 装载器动作 | 读 `p_type=7`；校验（`filesz<=memsz`、对齐、出现第二个 `PT_TLS` 直接拒）；把 `filesz` 字节搬进任务空间；`memsz-filesz` 清零；`hi` 扩到 TLS 末尾；`tls_va` 作为 `tp` 传给 `elf_start_task`，在**降权之后**写进任务帧 `LA_FR_TP`（帧偏移 8 字节 = `$r2`） |
| 夹具 | `example/qemu_loongarch64/elf/tls_main.c` → 5008 B 带签名 ELF，退出码自带语义：9 = `.tdata` 模板/TP 不对，10 = `.tbss` 没清零，11 = `write` 返回值不对，**3 = 两格都过** |
| QEMU 读数 | `[tls] write /mnt/ram/tls_main rc=0 size=5008` / `[tls] exec rc=0` / 控制台 `TLSOK` / `[tls] finished after 0 ms, exit code 3` |
| 体积（默认 → 开档） | 镜像 `414896/448/5112340` → `420944/448/5112340`（+6048 B text）；其中 `arkrtos_elf.o` `5003` → `5531`（+528）、`arkrtos_port.o` `11872` → `11920`（+48），余下是 5008 B 夹具 blob 与演示打印串。默认档比最早的 414208 基线多 688 B，来自 64 位地址运算与宽 VA 诊断 |
| 门禁 | `tools/elf_gate.py` 新增 H：LA64 开档两个 TU 必须编得过；RV32 开档必须编不过且错误点名 `ARKRTOS_USE_ELF_TLS`（实测命中）；`ELF_PT_TLS=7` 与「读段 → 搬模板 → 清 tbss → 写帧」四段 pin，配一次变异自检 |

**为什么夹具里两个 `__thread` 变量都带 `volatile`**：第一版没带，`-O1` 把 TLS
访问折成了常量，链接产物**连 `PT_TLS` 都不剩**——那是「测了但什么也没测」的假夹具。
这条与「闸门必须自带防假绿自检」是同一条纪律。

**这一道仍然没做的（写死，不许读成别的东西）**：

1. **没有 TCB**。实现的是「TP 指向 TLS 块」，不是 glibc 的 TCB 布局
   （`tcbhead_t`、`dtv`、自指指针、静态 TLS 的模块偏移都在 glibc 那边定义）。
   真 libc 仍然起不来。
2. **没有 TLS 重定位**。真实样本里的 17 条 `R_LARCH_TLS_TPREL64` 仍然不处理。
3. **没有 `set_tid_address` 语义**（线程指针与 TCB 的自指关系不在这一层）。
4. **只解决静态 local-exec**。`initial-exec` / `local-dynamic` 与动态 TLS 一字未动。
5. 探针的 `[tls]` 标签仍是**静态描述**（「这个文件有 `PT_TLS`」），不代表装载器
   装不了；真正拦住真实静态 hello 的是第 1 道 `[address]`。

---

## 6. 四档目标与代价

由近到远。**「过关」指探针 0 blocker 且 QEMU 实跑有读数**，不是「编得过」。

| 档 | 目标样本 | 现在过哪几道 | 还差 | 量级 |
| --- | --- | --- | --- | --- |
| 0 | **探针与清单**（本轮） | — | — | **已交付**，7 项门禁全过 |
| 1 | 静态 musl hello（`loongarch64-linux-musl-gcc -static`） | 静态档本来就只剩「地址 + TLS」 | 地址按需放置、TLS/TCB、外加 musl 起步要的号（`set_tid_address`、`rt_sigaction`、`ioctl` 等） | 中：装载器改 VA 模型 + 补 TLS，比补 syscall 表难 |
| 2 | 动态 musl（`ld-musl-loongarch64.so.1` + `libc.so`） | 同上 | musl 不用 IFUNC（省掉最大一块），但要解析 `PT_INTERP` 路径、`NEEDED` 递归、`RELATIVE`/`JUMP_SLOT`/TLS 重定位 | 中大：ld.so 从「配自研库」升到「配一份真 libc」 |
| 3 | glibc 动态（题面那一档） | — | 全部六道 + IFUNC + 符号版本 + `rseq`/`futex` + 更宽的 syscall 面 | 项目级：不是补缺口，是换一套用户态契约 |
| — | 静态 glibc（介于 1 与 3） | 静态两道 + `INIT_ARRAY` | glibc 的启动链自己在 `__libc_start_main` 之前就要一堆号 | 大 |

**这张表的用法**：想「让发行版二进制跑起来」这句话落到工程上，最小的一步是
第 1 档——它把「地址 + TLS」这两道从「没做」变成「做了并且有读数」，
而这两道对后面每一档都是必需的。

---

## 7. 为什么不做「自研 ld.so 配 glibc」

现成的思路是「`PT_INTERP` 我们也能装，那就自己写个 `ld.so` 去配 glibc」。这条
路走不通，理由不是工程量，是**契约**：

1. glibc 的 `ld.so` 与 `libc.so.6` 是**一对**：`libc` 直接引用 `ld.so` 里的
   `_dl_*`（`_dl_find_object`、`_dl_audit_objopen`、`_dl_catch_exception`…），
   换个 `ld.so` 这些符号全缺。
2. glibc 启动链在 `__libc_start_main` **之前**就要 `TLS`（`__libc_setup_tls` 的
   前提是 TP 已经能用）、`INIT_ARRAY`、`IRELATIVE`（IFUNC）、`rseq` 注册、
   `futex`。少一个不是「退化」而是**起不来**。
3. 符号版本（`GLIBC_2.27` 这类）要求 `VERSYM`/`VERNEED` 匹配；只按名字绑定
   会绑到老版本实现，那种错误不报错、只是算错——正是本项目明令禁止的
   「看似权威的错答案」。

本仓的 `ld-ark.so` 是**配自研库**的极小装载器（三种重定位、无符号版本），
它证明的是「内核侧的 Linux 约定 + 重定位路径通了」（见
[动态链接与PT_INTERP](动态链接与PT_INTERP.md)），**不是**「能配 glibc」。
两件事不能混着读。

---

## 8. 按号直方图（`ARKRTOS_USE_LINUX_ABI_SYSSTAT`）

`docs/Linux可执行文件兼容-系统调用ABI.md` 里的总数（handled / enosys / efault）
回答不了「这个二进制到底发了哪些号」。开关打开后内核维护一张 **16 槽稀疏表**
（按首次出现顺序，满槽只计 `overflow`，不静默丢），并多出一个 shell 命令。

打开方式（`make` 不跟踪 `EXTRA_CFLAGS`，**切档必须 `make clean`**）：

```bash
make clean
make -f example/qemu_loongarch64/Makefile -j8 \
     EXTRA_CFLAGS="-DARKRTOS_USE_DYNLOAD=1 -DARKRTOS_LINUX_ABI_SYSSTAT=1"
```

QEMU 实测（`elf_demo.c` 在 linux demo 之后直接打印，因为这块板没有交互式输入，
不落串口就等于没人验过）：

```
[lxstat] slots=11 overflow=0
[lxstat] nr 64 x102    (write)
[lxstat] nr 160 x1     (uname)
[lxstat] nr 113 x2     (clock_gettime)
[lxstat] nr 278 x1     (getrandom：未实现号，回 -38)
[lxstat] nr 172 x1     (getpid)
[lxstat] nr 214 x2     (brk)
[lxstat] nr 222 x1     (mmap)
[lxstat] nr 56 x2      (openat)
[lxstat] nr 63 x1      (read)
[lxstat] nr 57 x1      (close)
[lxstat] nr 93 x1      (exit)
```

同一次启动里 `[linux] getrandom(unknown nr) -> -38 ok`、`PASS: every syscall
answered as specified`、`finished after 0 ms, exit code 7` 都在，
`[ELF]`/`[FS]` 旁路读数也在——**兼容层不许把别人的演示搞坏**。

交互式使用时：

```
ark> lxstat
lxstat: handled=.. enosys=.. efault=.. other=.. mprotect=..
nr  64 write           x102
...
```

**边界（必须写死）**：这 11 个号来自 `example/qemu_loongarch64/elf/linux_hello.c`
的**合成触发**，不是真实发行版二进制的分布。真实分布要等 §5 第 1 条（地址空间）
放开、二进制真能跑到 `main` 之后才采得到。`lxstat` 的 shell 命令本身只有
「编译 + 注册」级证据（QEMU 没有交互式输入），串口证据来自上面的直方图打印。

另外，`linux_hello.c` 本轮新增了一条负例 `getrandom(278) -> -ENOSYS`：
原来的 13 条 check **没有一条**打到「未实现的号」，没有它就证明不了「未知号
被计数」。这也是直方图存在的理由——总数会算 `-ENOSYS`，直方图才回答
「是哪个号」。

---

## 9. 门禁

```bash
py -3 tools/realbin_gate.py        # 7 项，全过打印 REALBIN-GATE OK
```

| 检查 | 钉住什么 |
| --- | --- |
| `fixture-near` | 窗口内静态夹具 0 blocker（退出码 0） |
| `fixture-far` | 远基址 `0x120000000` **恰好 1 条** `address` blocker（退出码 1） |
| `fixture-dyn` | 动态夹具 0 blocker（`interp=/mnt/ram/ld-ark.so` 不误判成 `interp-path`） |
| `probe-negatives` | 4 种坏输入（非 ELF / ELF32 / `e_phentsize=40` / `e_phoff` 越界）全部判红且原因到位 |
| `table-alignment` | `ARK_SUPPORTED_DT`（12 项）与重定位表（3 项）与 `ldso.c` 的 `#define`/`case` **双向**一致 |
| `reloc-name-audit` | 5 种重定位名字与条数同 `readelf -rW` 逐项一致（缺工具则 SKIP 并说明） |
| `sysroot-semantics` | 不存在的根目录与空根目录都被明说，不静默当成「无依赖」 |

`probe-negatives` 不是形式主义：它在实现阶段**真的抓出了探针的缺陷**——坏 ELF 上
探针先访问 `e.e_type` 直接抛 `AttributeError`。这正是「负例测的是自己的仪器」的意思。

两条实现细节值得留档：

- **重定位去重**：glibc 的 `DT_RELASZ` 把 `.rela.plt` 也包进去了
  （`RELASZ 32472 = .rela.dyn 31776 + .rela.plt 696`），`DT_JMPREL` 指向同一段中间
  ⇒ 两次扫描会把 `JUMP_SLOT` / `IRELATIVE` 数两遍（实测 9→18、20→40）。
  探针按文件偏移去重后与 `readelf` 逐项一致，反过来也证实了
  `R_LARCH_IRELATIVE=12`、`R_LARCH_TLS_TPREL64=11` 这两条编号没写错。
- **`DT_RELAENT` 不走 `case`**：`ldso.c` 把重定位项长写死 24，探针因此单独判
  `DT_RELAENT != 24`（`relaent` blocker），门禁的「未接线标签」把它们区分开。

### 默认档零变化

`ARKRTOS_LINUX_ABI_SYSSTAT` 默认 0。零变化分两层**实测**，不是声明：

**(a) 内核代码：目标码逐项相同。** 四个改动过的内核 TU 用同一组默认档
选项分别编 `HEAD` 与工作区版本，`size` 的 text/data/bss 逐项相等：

| 文件 | HEAD | 工作区 |
| --- | --- | --- |
| `kernelsrc/src/arkrtos_linux_abi.c` | `4236 / 0 / 632` | `4236 / 0 / 632` |
| `kernelsrc/shell/arkrtos_shell.c` | `10341 / 0 / 9752` | 同 |
| `kernelsrc/shell/arkrtos_shell_misc.c` | `3706 / 0 / 0` | 同 |
| `example/qemu_loongarch64/elf_demo.c` | `22278 / 0 / 4096` | 同 |

这条曾经不成立：三个读数函数最早写成了「关档返回 0」的空壳，默认档里
`arkrtos_linux_abi.c` 因此多出 128 B 目标码。修法是把三个函数定义整体
放进 `#if (ARKRTOS_LINUX_ABI_SYSSTAT == 1)`，头文件在关档时也不声明它们
（调用点本来就在同一个开关里）——**空壳也算体积**。

**(b) 镜像：多出来的 56 B 是演示程序，不是内核。** 干净重建后
`.text / .data / .bss = 413808 / 448 / 786964`；改前基线是
`413752 / 448 / 786964`。差的 56 B 全部来自**编进镜像的 `linux_hello` blob**：
同一个 `.c` 去掉本轮新加的那条 `getrandom` check 后单独编，text 从 2792 B
降到 2736 B，正好 56 B。也就是说内核侧 0 B、`.data/.bss` 0 B。

**(c) 默认档没有新符号**：`nm` 里 `lxstat` / `nr_slot` / `nr_overflow`
计数为 0。

---

## 10. 边界与未做

**本轮明确没有做的：**

1. 真实发行版**跑不起来**（六道门槛一道未过）——本轮交付的是账本与仪器。
2. 存储已经不是拦路的那一道（4 MiB 卷 + 640 KiB 文件往返核对）；**地址宽度**
   也已从 32 位升到 64 位（2026-11-20，宽窗口档 QEMU 实跑通过，见下表与 §11），
   所以「把窗口挪到 `0x120000000`」现在表达得出来了。但**真实静态 hello 还没有
   装进宽窗口跑过**，每进程 VA 空间与运行期页表也仍没做；native SVC 参数块还是
   `uint32[]`。TLS 的**静态通路**已在 2026-11-20 打通（§5.2），但 TCB、TLS 重定位
   与 `set_tid_address` 未做。
3. `[lxstat]` 的读数是**合成触发**的 11 个号，不是真实分布（见 §8）。
4. 探针不解析 `PT_NOTE`、不读 `.gnu.attributes`（ABI 标记）、不判 `RELRO`
   到「能否强制」这一层——`RELRO=True` 只是描述，不构成 blocker。
5. 探针的 sysroot 搜索只覆盖 7 个常见子目录，深度 3；发行版镜像里那种
   `RUNPATH=$ORIGIN/../lib` 不解析。
6. 没有真实 `ld.so` 的兼容面测试，也没有任何「按 glibc 契约跑起来」的实验
   （§7 说明为什么这条路不立项）。

**已完成的一步（2026-11-20）**：装载链的**地址宽度升到了 64 位**，窗口 VA 与物理帧
解耦（`WIN_VA=` 构建开关）。宽窗口档 QEMU 读数：`linux_hello`（Linux ABI）
`[linux] PASS` + `brk(+4096) -> 4832890880` + `exit code 7`，`[MOD]` / `[FS]` 全 PASS，
无 fatal。路径上真踩到两处 32 位截断——`elf_build_stack()` 的 `~0x7u` 掩码把 64 位
地址截成 `0x0201FFFF8`，以及 port 里 `uint32 pc = frame[LA_FR_ERA]` 把 ERA 截断后按错
地址取指令——两处根因与反汇编证据见
[MMU进程模型与ELF装载](MMU进程模型与ELF装载.md) §11。

**下一步的最小一步**：把 `D:/tmp/realbin/hello_static.elf`（623 728 B，链在
`0x120000000`）嵌进镜像，在 `WIN_VA=0x120000000` 档真装真跑，抓 `[elf]` /
`[linux]` 读数与退出码；预期还会撞 `set_tid_address` / `brk` / TLS(TCB) 这些 ABI 面。

**顺序说明（与早先版本不同，是有意的）**：这一段原来写的是「先 64 位地址、
再 TLS」。本轮反了过来，先做 TLS 的静态部分，理由是：两道互不依赖，而 TLS
不碰地址模型（只读一个段、铺一次模板、写一个寄存器），一次 QEMU 读数就能闭环；
地址宽度要同时动判据、窗口切分与 port 三层，半途动风险高得多。所以现在的实际
状态是「TLS 静态部分已完成并有读数（§5.2），剩下 TCB/动态 TLS 恰好依赖
地址 + 重定位，自然排在地址宽度之后」。