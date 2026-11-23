# 真实发行版 LA64 二进制：需求清单与差距

> 载体：QEMU LoongArch64（`board/qemu_loongarch64`）。**QEMU 算上板，不是真机。**
> 仪器：`tools/elf/realbin_probe.py`（探针）+ `tools/realbin_gate.py`（门禁 7 项）。
> 读数：`ci_gate` 里对应 `realbin` 一项（进 `--remote` 时按本地工具链跳过）。

---

## 1. 一句话结论

**静态链接的发行版二进制已经能跑完（2026-11-20，QEMU 实跑）；动态链接的还不行。**
后者不是「补一个缺口」的差距。这一页现在是**账本 + 仪器 + 第一份真读数**：清单与
探针（怎么逐项算账、门禁怎么盯住探针自己）保留原样，新增的 §5.3 是第一次让
**不是在 Ark 上编的**二进制从装载到退出整段跑完。

跑通的是龙芯 glibc 工具链产出的**静态** `hello`（623 728 B，`printf` + `fflush` +
`write(1,...)` + `return 3`），读数见 §5.3。**动态链接仍然不可运行**：`PT_INTERP`
指向的 glibc `ld.so` 需要一整套用户态动态装载器与 libc 兼容面，属项目级工作量
（§7 说明为什么这条路不立项）。

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
| 真实发行版二进制**能跑起来** | **静态档跑通了，动态档没有** | §5.3：glibc 静态 hello 装载→运行→退出整段跑完；动态链接仍无 `ld.so` 兼容面（§7） |
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
| 1 | 地址空间 | `[0x120000000, 0x120008080)`（动态）/ `[0x120000000, 0x12008bb38)`（静态） | **地址宽度已升到 64 位（2026-11-20）**：判据里不再有 `p_vaddr > 0xFFFFFFFF`，窗口 VA 与物理帧解耦（`make ... WIN_VA=0x120000000` 把窗口基址、内核链接基址与夹具基址一起抬）。宽窗口档 QEMU 实跑 `linux_hello` `PASS` + `exit 7`，无 fatal（见 §11 与 [MMU进程模型与ELF装载](MMU进程模型与ELF装载.md) §11） | 每进程独立 VA 空间 + 运行期建页表（**真实静态 hello 已经真装进宽窗口跑完**，2026-11-20，见 §5.3）；native SVC 参数块仍是 `uint32[]`，宽 VA 下 App 必须走 Linux ABI 传指针 | `[address]` |
| 2 | 解释器 | `PT_INTERP = /lib64/ld.so.1` | 默认档直接拒（`ERR_FORMAT`）；开 `ARKRTOS_USE_DYNLOAD` 后能装**本仓** `ld-ark.so`（`/mnt/ram/ld-ark.so`） | 真 glibc `ld.so` 的能力面（见 §7） | `[interp-path]` |
| 3 | TLS | `PT_TLS`（动态继承自 libc：16/144；静态：32/96）+ 17 条 `R_LARCH_TLS_TPREL64` + `set_tid_address` | 默认档 `arkrtos_elf.c` **完全不读 `PT_TLS`**；开着 `ARKRTOS_USE_ELF_TLS` 时读段、铺 `tdata`、清 `tbss`、把 `$r2` 指过去（2026-11-20，静态 local-exec，见 §5.2）；**自研 `ld-ark.so` 侧已经把 `PT_TLS` + `R_LARCH_TLS_TPREL64` 做成**（2026-11-21，见 §5.6） | 真 libc 的 TCB 布局（`tcbhead_t`/`dtv`）、`DTPMOD`/`DTPREL`、`set_tid_address` 语义 | `[tls]` |
| 4 | 动态段标签 | `INIT_ARRAY`/`FINI_ARRAY`、`GNU_HASH`、`VERSYM`/`VERNEED`/`VERDEF`、`RELACOUNT`、`FLAGS` | `ld-ark.so` 只处理 `NEEDED/STRTAB/SYMTAB/RELA/RELASZ/RELAENT/STRSZ/SYMENT/JMPREL/HASH` 这几项 | 构造函数执行、符号版本匹配、GNU 哈希、`DF_1_NOW` 语义 | `[dynamic-tags]` |
| 5 | 重定位 | `RELATIVE`、`64`、`JUMP_SLOT`、`IRELATIVE`(IFUNC)、`TLS_TPREL64` | 内核静态装载器只做 `R_LARCH_64 / RELATIVE / JUMP_SLOT`（`ARK_SUPPORTED_RELOC`，门禁与 `ldso.c` 双向对齐）；**自研 `ld-ark.so` 另做 `R_LARCH_TLS_TPREL64`**（2026-11-21，§5.6） | IFUNC 解析器（装载期跑用户代码）+ `DTPMOD`/`DTPREL` | `[relocations]` |
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

**2026-11-20 补的一条（有读数，见 §5.4）**：上表「装载器动作」那行写的
「把 `filesz` 字节搬进任务空间；`memsz-filesz` 清零」在真实 glibc 静态二进制上是
**错的**——`PT_TLS` 的 `.tbss` 是 `NOBITS`，它与后面的 `.init_array` **同地址**
（实测两段都在 `0x120083638`）。照 `p_memsz` 清零等于把构造表抹成 0。现在的规则是
「只补缺口」：被某个 `PT_LOAD` 的 `p_filesz` 区间背书过的字节**一个都不动**，
只有落在背书范围之外的部分才补数据/清零；而且**只有装载器真铺过字节时才预置
`TP`**（全程被镜像覆盖时那个地址上躺的是别人的数据，TLS 块由程序自己的 C 运行时建）。

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
   装不了。这条写的时候第 1 道 `[address]` 还拦着真实静态 hello；到 §5.3 就通了。

### 5.3 真实静态 glibc 二进制装上板跑完（2026-11-20）

这是第一次让**不是在 Ark 上编的**二进制从装载到退出整段跑完。样本是龙芯 glibc
工具链产出的 `hello_static.elf`：623 728 B，`e_entry=0x12000008E0`，无 `PT_INTERP`、
无重定位、无 `DT_NEEDED`，`PT_TLS` 32/96。源码在 `D:/tmp/realbin/hello.c`
（`printf` → `fflush` → `write(1, ...)` → `return 3`）。

读数（QEMU，**不是真机**；上传走 `fs put`）：

```
$ exec /mnt/ram/realhello
hello from real loongarch64 linux binary, argc=1
direct write ok
[  115.113] <6>TASK:4009 task 4 exits (self kill)
[ELF] /mnt/ram/realhello: 543511 B, 2 segment(s), entry 0x00000001200008E0,
      sp 0x00000001201FC000, argc 1, task 4, space 0
[ELF] /mnt/ram/realhello: finished after 0 ms (space 0, exit code 3)
```

四件可对账的事，缺一件都不算「跑完」：

| 项 | 为什么它能证明东西 |
| --- | --- |
| `hello from ... argc=1` | glibc 的 `printf` 走完了 `stdio` 缓冲 + `write` 到 fd 1；`argc=1` 说明栈上 `argc/argv` 布局与 Linux 约定一致 |
| `direct write ok` | 直接 `write(1, ...)` 也通，不是「只有 printf 那条路碰巧对」 |
| `task 4 exits (self kill)` | `exit_group` 走进了内核的 `arkrtos_task_kill_internal()`（`kernelsrc/src/arkrtos_task.c:4008` 那行），是程序自己退出，不是被杀 |
| `entry 0x00000001200008E0` / `exit code 3` | 前者与 ELF 头 `e_entry` 逐位相同；后者就是源码里的 `return 3` |

`543511 B` 是真正写进窗口的 `p_filesz` 之和，不等于文件大小 623 728 B（后者含
调试节），两个数不是同一口径。

**为这一刻改掉的三处**（每处都有独立判据，不是「一起改完就好了」）：

| # | 改前现象 | 根因 | 改法 | 判据 |
| --- | --- | --- | --- | --- |
| 1 | 任务一降权就 `FPD` 死循环，连 eentry 自己都进不去 | LA64 用户态 FPU 没开（`EUEN.FPE` 是 **bit 0**，不是 bit 1），而内核按 `-mabi=lp64d` 编，任何 C 函数都可能被塞 FP 指令 | `example/qemu_loongarch64/start.S` 的 `_start` 里置 `EUEN.FPE`（必须早于任何 C 代码；板级 `board_init` 那条弱实现被本板 `arkrtos_board.c` 覆盖，是死代码） | QEMU `-d int`：`exception: 15 (Floating Point Disabled)`、`enter: pc 0x24cd98 ERA 0`，随后 `0x24cb44` 反复自陷；反汇编两处都是 `fst.d $f0,$r23,256`。改 bit0 后启动完整 |
| 2 | 开了 FPU 之后任务切换会串浮点值 | 陷阱帧里没有 FPR（帧只有 32 个 GPR 槽） | 帧 256 → **528**：`LA_FP_SAVE/RESTORE` 各 32 条 `fst.d/fld.d $fN, base, 256+8*N`，eentry / SMP 分流 / 就地切换 / resume 六处；关档定义成空宏，零指令零体积 | 反汇编计数 `fst.d` 64 条（eentry + 就地切换各 32）、`fld.d` 96 条（+resume 32）；`[A]/[B]` 秒表 15 s 全绿 |
| 3 | 程序在 `PT_TLS` 之前就崩在空指针上 | auxv 只有 `AT_PAGESZ/AT_ENTRY/AT_PHDR/...`，glibc 启动会立刻解引用 `AT_RANDOM` | 补 `AT_RANDOM`（16 B 弱随机区）+ `AT_SECURE/UID/EUID/GID/EGID/CLKTCK/HWCAP/EXECFN`，`ARKRTOS_ELF_AUX_MAX` 20 | 补前 `badv=0x1`；补后消失，程序走到自己的 `main` |

开关是既有的能力层 `ARKRTOS_USE_FPU`（`config/kconfig/board/Kconfig.qemu_loongarch64`
里 help 重写，`qemu_loongarch64_defconfig` 改 `=y`，板级头**由 `genboard.py` 重生成**，
不手改——CI 有 `board-config-generated` 一项在盯）。两档体积（同一份源码，只切开关）：

| 档 | 镜像 text | `arkrtos_context.o` | `arkrtos_port.o` |
| --- | --- | --- | --- |
| `ARKRTOS_USE_FPU=n` | 415552 | 896 | 11989 |
| `ARKRTOS_USE_FPU=y` | 416280（+728） | 1536（+640，64 条 `fst.d/fld.d`） | 11989（不变） |

关档时 `LA_FP_SAVE/RESTORE` 展开成空宏，帧回到 256 字节——**多出来的 640 B 全是
开档才有的指令**，不是「先塞后删」。

**一处必须单列的「错读数」**：`[ELF]` 那行原来用 `0x%08X` 打 `unsigned long`，
而 `libc_shim.c` 的 `vsnprintf` 把长度修饰 `l` 丢掉、数值分支一律
`__builtin_va_arg(ap, unsigned int)` ⇒ 真值 `0x12000008E0` 被打成 `0x200008E0`，
**看上去像装载器把镜像放到了 `0x20000000`**。两处都修了（shell 侧换 `%016lX`
并按 `unsigned long` 传参；`vsnprintf` 按长度修饰真取宽），同一批夹具随即打出
`0x0000000120000000`，真实 hello 打出 `0x00000001200008E0`。
这是「读数本身可以是错的」的现成例子：**先证明仪器，再相信仪器**。

**这一节仍然没做的（写死，不许读成别的东西）**：

1. 只覆盖**静态**档。`PT_INTERP` + glibc `ld.so` 那条路一字未动（§7）。
2. 没有 TCB（`tcbhead_t` / `dtv` / 自指指针），没有 `R_LARCH_TLS_TPREL64` 重定位，
   没有 `set_tid_address`。静态 hello 不碰这些，所以它跑通**不证明**这些面能用。
3. `exit code 3` 是程序自己 `return 3`，不是「退出码通路有 bug」。
4. FCSR（舍入模式与异常使能位）**已经按任务保存**（§5.5：QEMU 实跑 PASS，加上
   「去掉存取两行」的对照跑 FAIL）；**LSX（`$vr0..$vr31`）已于 2026-11-21 按任务保存**
   （§5.7，开关 `ARKRTOS_USE_LSX` 缺省 0，帧 528 -> 1040）；LASX（`$xr0..$xr31`）
   仍**不保存**，真用到会落 `Ecode=13 INE` 显式故障。
5. 上传通道实测 2 151–2 240 B/s，623 760 B 要 278–339 s。走串口而不是「编进镜像」，
   是因为 2 MB Flash 窗口 / 512 KB 内核池的几何硬墙装不下这个二进制（§3 与
   `tools/realbin_probe.py` 文件头）。

---

### 5.4 真实 glibc 静态程序的 `__thread` 变量（2026-11-20）

判据就是 §5.2 那条「一个用到 `__thread` 的真实 glibc 静态程序能读到自己的线程
变量」。样本 `D:/tmp/realbin/tls_real.c` → `tls_real.elf`（623 800 B，
`ta=5 / tb=7 / tc[0]='x'`，全对才 `return 3`）。`readelf -l` 的关键两行：

```
LOAD  0x07f575 0x0000000120083575  filesz 0x6fdb  memsz 0x85c3  RW  align 0x4000
TLS   0x07f610 0x0000000120083610  filesz 0x0028  memsz 0x0078  R   align 8
```

读数（`WIN_VA=0x120000000`，QEMU，**不是真机**）：

| 档 | 读数 |
| --- | --- |
| `ARKRTOS_USE_ELF_TLS=0`（默认） | `tls-real a=5 b=7 c=x` + `[ELF] ... entry 0x00000001200009A0 ... exit code 3` |
| 开档，**修之前** | **全静默**：`exec` 之后一字节输出都没有，连 `[A]/[B]` 心跳都停 |
| 开档，**修之后** | `tls-real a=5 b=7 c=x` + `exit code 3`，与默认档逐字相同 |

**默认档这一行要诚实读**：这一档 `PT_TLS` 代码整段不编译，初始帧 `$tp=0`，
TP 是 glibc 自己的 `__libc_setup_tls()` 按 `AT_PHDR/AT_PHNUM` 立的
⇒ **它不证明我们的 TCB 路径在工作**，只证明「不碰 TLS 时 glibc 自己能起来」。

**开档修之前的死循环（现场）**：`-d int -D` 落 252 MB 异常流。崩溃前程序已正常走过
若干 `Syscall`（`SYS_NUM 214` = brk），随后在 `ERA 0x12001e7cc` 处反复
`exception: 7 (Page Privilege error)`、`enter: pc 0x0000000000000000`
——PLV3 取指跑到 `0x0` 的 PLV0 页，无限 PPI。`0x12001e7cc` 在 `__init_misc` 里，
前一条是 `bl → 0x120000610`（`strrchr` 一类 IFUNC 入口），即**从空指针 call 出去**。

**根因不是 TP，是清零**。`.tbss` 是 `NOBITS`，运行地址与 `.init_array` 重叠：

```
[ 9] .tdata       PROGBITS    0x0000000120083610  off 0x7f610
[10] .tbss        NOBITS      0x0000000120083638  off 0x7f638
[11] .init_array  INIT_ARRAY  0x0000000120083638  off 0x7f638
```

旧实现按 `p_memsz - p_filesz` 把 `0x120083638..0x120083688` 清零，正好把
`.init_array` 整张表抹成 0 ⇒ `__libc_csu_init` 从表里 call 空指针。
**先前那条「字节同一性」的纸面论证就错在这里**：它只比了 `.tdata` 那 0x28 字节
（确实逐字节相同），没注意要清零的 0x50 字节压在别的节上。

**两次对照实验把「是 TP 还是清零」分开**（同一份二进制，只切一个开关）：

1. `if(tp != 0u)` 改成恒不成立（不写 TP，其余一字不动）→ 仍然静默死循环
   ⇒ **TP 不是原因**。
2. 连内置 `tls_main` 夹具也关掉（它不写 TP 就会自己 fault，把 shell 挡住），
   再跑真实二进制 → 仍然静默 ⇒ 与夹具无关。

**修法与判据**（`kernelsrc/src/arkrtos_elf.c`）：

| 规则 | 判据 |
| --- | --- |
| 先收集所有 `PT_LOAD` 的 `p_filesz` 区间（`elf_load_file_spans()`） | 段表读不出或条数 > 8 时**一个字节都不写**（宁可 TLS 不生效，也不抹镜像） |
| `.tdata` 抄写与 `.tbss` 清零都按「被背书就跳过」逐段切（`elf_write_tls_piece()`） | 真实 glibc 二进制整块被覆盖 ⇒ 写 0 字节；夹具的 `.tbss` 在最后一个 LOAD 之外 ⇒ 只清那一段 |
| `TP` 只在真铺过字节时预置（`tls_placed`） | glibc 二进制 `tp=0`（TCB 由 glibc 自建）；夹具照旧拿到 TP，退出码仍是 3 |
| 门禁 H3 加三条 pin + 一条变异自检 | 把 `elf_write_tls_piece(` 改名后 pin 必须变红（实测红过一次） |

**修后同一条路径的读数**（开档，`WIN_VA=0x120000000`）：

```
$ exec /mnt/ram/tlsreal
tls-real a=5 b=7 c=x
TASK:4009 task 4 exits (self kill)
[ELF] /mnt/ram/tlsreal: 543527 B, 2 segment(s), entry 0x00000001200009A0,
      sp 0x00000001201FC000, argc 1, task 4, space 0
[ELF] /mnt/ram/tlsreal: finished after 0 ms (space 0, exit code 3)
```

同一档的内置夹具仍然过：`[tls] finished after 0 ms, exit code 3`。

**这一节仍然没做的（写死，不许读成别的）**：

1. 跑通的是**静态**档的 `__thread`，且 TP 由 glibc 自建。`ARKRTOS_USE_ELF_TLS`
   给的是「没有 libc 初始化的小程序」用的 variant I 约定（`TP = TLS 块起点`），
   **不是** glibc 的 `tcbhead_t`/`dtv` 布局。
2. 内核静态装载器仍不做 `R_LARCH_TLS_TPREL64`（静态 local-exec 不需要它）；
   自研 `ld-ark.so` 侧已经在做（2026-11-21，§5.6）。`set_tid_address`（96 号）**已实现**：
   `arkrtos_linux_abi.c` 只回报本任务的 tid，不往用户内存写一个字（本内核的 futex
   只有 WAIT/WAKE 最小一档，没有 robust list，登记的指针没有消费者；替程序清 tid
   变量是副作用）。
3. `PT_TLS` 块被镜像背书时我们**不**替程序建 TLS 块——那是它自己的 C 运行时的事。

---

### 5.5 FCSR 作为线程上下文（2026-11-20）

判据：两个任务各锁一种舍入模式，每轮「设 RM → 实算一次 `2^24+1` → 睡 1 ms（一定被
切走）→ 读回 RM 再算一次」，跑满 50 轮。`2^24+1` 在单精度里不可表示，RZ 舍成
16777216、RP 舍成 16777218，所以「读回的 RM 对」与「实算结果对」是两条独立证据。

读数（`qemu_loongarch64`，默认档 `ARKRTOS_USE_FPU=1`，**QEMU 不是真机**）：

```
[fcsr] A rm=1 bad=0 rounds=50 | B rm=2 bad=0 rounds=50 -> PASS (FCSR per task across switches)
```

单开关对照：把 `LA_FP_SAVE/LA_FP_RESTORE` 里存/取 FCSR 的那两行去掉，其余一字不动，
同一条路径立刻变成

```
[fcsr] A rm=1 bad=98 rounds=50 | B rm=2 bad=0 rounds=50 -> FAIL (FCSR leaked between tasks)
```

⇒ 这个 PASS 不是「本来就没切换」的假绿（否则对照跑也不会 FAIL）。

**为什么不写助记符**：本工具链 binutils 2.31 的 `as` 不认识 `movfcsr2gr`/`movgr2fcsr`
（`Fatal error: no match insn`；`$fcsr0`/`$fcsr`/`$FCSR0`/`$0`/`0`/反序全试过，
`strings as.exe` 里两个助记符都在，属寄存器类没接线）。所以 `.S` 里写的是**字面编码**，
出处不是猜的：

1. 用真 glibc 工具链编一份 `fe.elf`（`fesetround`/`fegetround`，静态 623 744 B），
   里面确实含这两条指令；
2. 用**同一套** `loongarch64-newlib-elf-objdump -d` 反汇编，读出生产使用的编码：
   `0114c804 movfcsr2gr $r4,$r0`、`0114c080 movgr2fcsr $r0,$r4`、
   `0114c80c movfcsr2gr $r12,$r0`、`0114c180 movgr2fcsr $r0,$r12`；
3. 反过来把 `.word 0x0114c804 / 0x0114c080 / 0x0114c80c / 0x0114c180` 交给 `as.exe`
   汇编、再 `objdump -d`，逐条解回原助记符与寄存器（往返一致）。

形状：`movfcsr2gr rd,fcsr = 0x0114C800 | (fcsr<<5) | rd`，
`movgr2fcsr fcsr,rj = 0x0114C000 | (rj<<5) | fcsr`。

**落点**（`kernelsrc/port/loongarch/la64/arkrtos_context.S`）：

| 位置 | 动作 |
| --- | --- |
| `LA_FP_SAVE` | `.word 0x0114C80C`（`movfcsr2gr $t0,$fcsr0`）+ `st.d $t0, base, 512` |
| `LA_FP_RESTORE` | `ld.d $t0, base, 512` + `.word 0x0114C180`（`movgr2fcsr $fcsr0,$t0`），放在最后，避免「异常使能已开、FPR 还没装完」的中间态 |
| `arkrtos_port_fcsr_get/set` | `$a0` 入参/返回的原语，给 C 侧读改写（自检用）；函数名进 `.text.arkrtos_port_fcsr` |

**帧长不变**：帧偏移 512 那个槽在 528 字节帧定下来时就预留着（原注释写「预留，当前
不存」），所以这一步**不动任何栈预算**。新建任务的帧由 `arkrtos_port_stack_init()`
整体清零，FCSR 槽初值是 0（RN、无异常使能）；`$t0` 在存/取点都先把原值存进 88 槽
才借用，不伤活值。开档时的三处存点（eentry / 就地切换 / 次核入口）与一处只恢复
（`arkrtos_port_resume_task`，它从不由自己保存）在反汇编里逐条核过。

**LSX 已于 2026-11-21 补上**（§5.7）：`$vr0..$vr31` 按任务保存，开关
`ARKRTOS_USE_LSX` 缺省 0，帧 528 -> 1040。**仍然没做的**是 LASX
（`$xr0..$xr31`）：`EUEN.ASXE` 故意保持 0，用到会落 `Ecode=13 INE`——显式故障，
不是静默算错。

---

### 5.6 动态 TLS：装载器建线程块 + `R_LARCH_TLS_TPREL64`（2026-11-21）

判据：走 `PT_INTERP` 动态路径的主程序用 `__thread`，它自己的线程变量与**库里**的
线程变量都能读对；库那条必须经库自己的重定位槽拿到偏移，不能拿主程序的偏移凑。

| 项 | 读数 / 规则 |
| --- | --- |
| 开关 | `ARKRTOS_USE_DYNLOAD`（默认 0）；夹具的 `PT_TLS` 由 `tools/elf/dynfixture.py` 从静态链接产物**原样搬出**，不手写模板 |
| ld.so 动作 | 扫每个对象的 `PT_TLS`（`p_type=7`）；按装载顺序（主程序在前）16 B 对齐分配块内偏移；`mmap` 一块 RW 匿名内存并清零；主程序从**已装载的内存**抄模板，库用 `read_file` 从文件抄（输出布局里 `file offset == vaddr - base`）；跳回主程序前 `move $tp, <块起点>` |
| 重定位 | `R_LARCH_TLS_TPREL64 = 11`，右值 = `S + A - TP`，即**相对 `$tp` 的带符号偏移**；槽里存的不是绝对地址，用户代码自己 `tp + off` 去解引用 |
| 符号 | `STT_TLS = 6`。`.dynsym` 里 `st_value` 是**块内偏移**（实测 `m_tls=8`、`lib_tls=0`），所以 `lookup()` 必须跳过 `STT_TLS`——否则它会返回 `base + 偏移`，一个不是变量的地址，静默指错 |
| QEMU 读数 | `[ldso] tls base=0x0000000002116000 size=32 objs=2 offs=0,16,`、`reloc libanswer.so: RELATIVE=1 64=1 JUMP_SLOT=0 TPREL64=1`、`reloc dynmain: ... TPREL64=1`；14 条 `check` 全 `ok`；`[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT + TLS all applied`；`exit code 105`，`dynmain`（ET_DYN）与 `dynmain_exec`（ET_EXEC，基址 `0x02000000`）各一次 |
| 单开关对照 | 把 `apply_rela()` 里的 type 11 分支改成直接 `return 0`（槽保持 0），同一条路径立刻 `TPREL64=0`，并打出 `TPREL64 m_tls offset -> 0 FAIL (want 8)`、`TPREL64 offset nonzero -> 0 FAIL (want 1)`、`TLS lib value -> 4369 FAIL (want 4242)`（4369 = 0x1111，正是相邻的 `__thread` pad 值），`exit code 1` |
| 门禁 | `tools/dynlink_gate.py`：新增 `fixture-tls`（PT_TLS 尺寸、`.rela` 里 type 11 条数、`.dynsym` 的 `STT_TLS` 值必须 `m_tls=8` / `lib_tls=0`）与报文 pin（`[ldso] tls base=` ×3、`TPREL64=1` 库 3 / 主 2、`m_tls offset -> 8` ×2、`TLS lib value -> 4242` ×2、`after write -> 4243` ×2）；自检坏样本从 4 种加到 7 种 |

**反假绿的三个设计**（缺一个这条 PASS 就不可信）：

1. `m_tls` 的块内偏移**故意非 0**：`m_tls_pad` 与它一起声明，按本仓 gcc 的
   `.tdata` 排布调过顺序，实测 `st_value=8`。否则「重定位没做、槽还是 0、正好读到
   偏移 0」会读到一个"看起来对"的值。
2. 库那条**强制走库自己的 `got_libtls`**：`lib_tls_addr()` 返回 `$tp + 槽值`，主
   程序拿它与 `lib_tls_off()`（库把槽值报回来）比，相等才说明符号解析是跨对象找到
   库的 `tls_off`，不是拿主程序的偏移凑出来的。
3. 再加一条隔离检查：写库的 TLS 之后，主程序的 `m_tls` 必须还是 77。

**边界（写死，不许读成别的）**：

1. 只做**静态块 + `TPREL64`**（initial-exec 形态）。`DTPMOD`/`DTPREL`（dlopen
   之后才绑定）没做。
2. **库里的 `local-exec` 代码不能直接用**：静态链接时它的 `$tp + 0` 是按「块起点
   = 0」算死在机器码里的，装载器不修补指令立即数。只有排在线程块偏移 0 的主程序
   成立；库必须经 `TPREL64` 槽访问（本仓夹具就是这么写的）。这是一条能力边界，
   不是实现取巧。
3. 这不是 glibc 的 TCB：`tcbhead_t`/`dtv`/自指指针仍然没有，真 `ld.so` 起不来
   （§7）。

---

### 5.7 LSX 向量寄存器上下文（2026-11-21）

判据：两个任务各持一份 32 x 16 B 的图案，每轮 `vld` 全量装 `$vr0..$vr31`、被切走、
再 `vst` 存回并与自己的图案逐字比对，50 轮后两边 `bad` 都是 0。

| 项 | 读数 / 规则 |
| --- | --- |
| 开关 | `ARKRTOS_USE_LSX`（默认 0，依赖 `ARKRTOS_USE_FPU=1`，不满足是编译期 `#error`） |
| 帧布局 | GPR/ERA/PRMD `0..255`；FPR `256..511` + FCSR@512；向量区 **`528 + 16*i` -> `$vr{i}`**，帧长 **1040** |
| `EUEN` | bit0 FPE（已有）+ **bit1 SXE = 1**；bit2 **ASXE 故意 0** |
| QEMU 读数 | `[lsx] pre bad=0 ...` 两条（前置对照：装完立刻存、不经切换）+ `[lsx] A bad=0 rounds=50 \| B bad=0 rounds=50 -> PASS (vr0..vr31 per task across switches)` |
| 反假绿 | 两处 `#if (ARKRTOS_USE_LSX == 1)` 改 `#if 0` => `bad=50` / `bad=49` FAIL，而 `pre` 仍 `bad=0`（失败只出在跨切换那一步）；已还原 |
| 默认档 | 固定 `SOURCE_DATE_EPOCH` + `--strip-debug` 后与打开前同 sha256（`3e6858e8…`），即**逐字节不变** |
| 门禁 | `tools/lsx_gate.py` 六项（`switch-default` / `frame-size` / `euen` / `parser-selfcheck` / `default-build` / `lsx-build`） |

**边界**：LASX 仍不保存（`ASXE = 0`，用到落 `Ecode=13 INE` 显式故障；`xvst` 要
32 字节对齐而帧基址只保证 16 字节）；每个任务栈 +512 字节由使用者按余量判断；
读数来自 QEMU，不是真机。细节见
[QEMU模拟器移植与验证](QEMU模拟器移植与验证.md) §5.31。

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

本仓的 `ld-ark.so` 是**配自研库**的极小装载器（四种重定位、无符号版本），
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

1. **动态链接的**真实发行版二进制仍跑不起来（`PT_INTERP` + glibc `ld.so` 的能力面
   属项目级工作量）。静态档已经跑通，见 §5.3。
2. 存储、地址宽度、FPU 上下文这三道都已经通了：4 MiB 卷（§5.1）、64 位地址宽度
   （2026-11-20，宽窗口档 QEMU 实跑通过）、`EUEN.FPE` + 528 字节帧（§5.3）。
   **真实静态 hello 已在宽窗口里跑完**。仍没做的是：每进程独立 VA 空间与运行期
   建页表、native SVC 参数块（还是 `uint32[]`，宽 VA 下 App 必须走 Linux ABI 传指针）、
   glibc 形态的 TCB（`tcbhead_t`/`dtv`）、`R_LARCH_TLS_TPREL64` 重定位。
   静态 TLS 落位（§5.4）、FCSR（§5.5）与 LSX 向量寄存器（§5.7）都已有读数；
   只剩 LASX（`$xr0..$xr31`）不保存，用到落 `Ecode=13 INE`。
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

**下一步的最小一步**：原来这条写的是「给静态档补 TCB，判据是一个用到 `__thread` 的
真实 glibc 静态程序能读到自己的线程变量」——**这个判据 2026-11-20 已经达成**
（§5.4：`tls-real a=5 b=7 c=x` + `exit code 3`），但达成方式是「glibc 自己立 TCB，
装载器不碰 TP」，所以它不构成本仓 TCB 的支持证据。下一步因此分成两条，按代价排：

1. **补 `R_LARCH_TLS_TPREL64` 重定位**（中小）：让「程序自己立 TCB」这条路径上的
   重定位收齐，判据仍是既有夹具 + 真实二进制不回归。（`set_tid_address` 原来列在
   这里，现已实现，只剩「不写用户内存」这条明确语义；robust futex 清理除外，本内核的
   futex 只有 WAIT/WAKE 最小一档。）
2. **`PT_INTERP` + `ld.so` 动态档**（项目级，需单独立项）：带一套用户态动态装载器
   与 libc 兼容面，不是补一个缺口（§7 说明为什么不自研 `ld.so` 去配 glibc）。

**顺序说明（与早先版本不同，是有意的）**：这一段原来写的是「先 64 位地址、
再 TLS」。本轮反了过来，先做 TLS 的静态部分，理由是：两道互不依赖，而 TLS
不碰地址模型（只读一个段、铺一次模板、写一个寄存器），一次 QEMU 读数就能闭环；
地址宽度要同时动判据、窗口切分与 port 三层，半途动风险高得多。所以现在的实际
状态是「TLS 静态部分已完成并有读数（§5.2），剩下 TCB/动态 TLS 恰好依赖
地址 + 重定位，自然排在地址宽度之后」。