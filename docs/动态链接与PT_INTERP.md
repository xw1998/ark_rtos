# 动态链接与 PT_INTERP（LA64 + 自研 ld.so）

> 开关：`ARKRTOS_USE_DYNLOAD`（Kconfig：`depends on ARKRTOS_USE_ELF`，**默认 n**）。
> 载体：QEMU LoongArch64（`board/qemu_loongarch64`）。**QEMU 算上板，不是真机。**
> 读数：`tools/dynlink_gate.py` 12 项全过；`ci_gate` 里对应 `dynlink` 一项。

---

## 1. 这一页回答什么

打开开关后，内核接受带 `PT_INTERP` 的可执行文件：**按 Linux 的约定**把解释器
装进一段板级给定的地址、在用户栈上铺 `auxv`、把入口换成解释器；解释器是本仓
自带的一个极小 `ld.so`（`example/qemu_loongarch64/elf/ldso.c`），它自己
`mmap` + `pread64` 装依赖库、做三种重定位，再跳回主程序。

**它不回答**「现成 Linux 发行版的二进制能不能直接跑」——**不能**，原因在第 8 节
逐条写死。这一页的判据全部落在「内核侧 Linux 约定 + 本仓 ld.so + 真实重定位
路径」这一层。

关档（默认）时：`PT_INTERP` 仍按格式错误拒绝（`ARKRTOS_ELF_ERR_FORMAT = -9`），
`elf_demo.c` 里的四个夹具 blob 一个字节都不编进镜像。

---

## 2. 内核侧做了什么

### 2.1 板级布局（`board/qemu_loongarch64/arkrtos_board_config.h`）

| 宏 | 值 | 含义 |
| --- | --- | --- |
| `ARKRTOS_ELF_WINDOW_BASE` / `_SIZE` | `0x02000000` / `0x00200000` | ELF 用户窗口（原有） |
| `ARKRTOS_ELF_INTERP_BASE` | `0x02100000` | 解释器装载基址（窗口内，主程序区之上） |
| `ARKRTOS_ELF_INTERP_SIZE` | `64 KB` | 解释器可用区间 |

两条硬规则：解释器区间必须落在 ELF 窗口内；主程序区与解释器区**不许重叠**
（重叠即 `ERR_LAYOUT`，不挑一个「能跑就行」的解法）。`lx_heap_base()` 在开档
时从 `INTERP_BASE + INTERP_SIZE` 起算，堆不会盖到解释器。

### 2.2 装载流程（`kernelsrc/src/arkrtos_elf.c`）

1. 读 `Ehdr`，`elf_check_header()` 现在接受 `ET_EXEC` 与 `ET_DYN`，并回传类型；
2. `elf_find_interp()` 扫程序头找 `PT_INTERP`，把路径拷进 64 B 缓冲。路径长度
   按 `p_filesz` 判，末尾强制补 NUL——**不信文件里的 NUL**（截断路径被当成
   另一个文件装进去是最难查的静默失败）；
3. 主程序按原有两遍法（先只校验、再真写）落位。`ET_DYN` 主程序的
   `bias = 窗口基址`，并要求它的最低 vaddr 就是 0（否则「基址」和「vaddr」
   两个说法对不上，直接拒绝）；
4. 解释器单独测量 + 单独写：`ET_EXEC` 解释器的 `p_vaddr` 必须是绝对地址
   （`interp.lo == INTERP_BASE`），`ET_DYN` 解释器则铺到 `INTERP_BASE`；
5. 主程序与解释器的 `[lo,hi)` 不许相交；
6. `AT_PHDR` 必须非 0（主程序的程序头表确实在某个 `PT_LOAD` 里）；
7. 铺 `auxv`：`AT_PHDR / AT_PHENT / AT_PHNUM / AT_PAGESZ(=4096) / AT_BASE
   (=解释器装入地址) / AT_ENTRY / AT_UID / AT_EUID / AT_FLAGS`，末尾 `AT_NULL`；
8. `task_entry = interp.entry`，`p_info->interp_base / interp_entry` 一并回填，
   供演示与 shell 读取。

### 2.3 Linux ABI 侧（`kernelsrc/src/arkrtos_linux_abi.c`）

ld.so 要用的三个新系统调用：

| 号 | 名字 | 实现口径 |
| --- | --- | --- |
| 67 | `pread64` | 带偏移读，不动 fd 位置 |
| 226 | `mprotect` | **接受但不强制**（整窗单条 `PS=20` 条目，RWX）；返回值与 `stat` 里如实标 `accepted, not enforced` |
| — | `stat` 第 5 个变体 | `LX_STAT_MPROTECT` 承接上一条的「成功」 |

`mmap`（222）沿用窗口内的单调分配；`MAP_FIXED` **不支持也不假装支持**——所以
ld.so 的库地址 = `mmap` 返回值 + `p_vaddr`，不是由 ELF 指定的。

---

## 3. 自研 ld.so（`example/qemu_loongarch64/elf/ldso.c`）

`ET_EXEC`，固定链接在 `0x02100000` = `ARKRTOS_ELF_INTERP_BASE`，因此不需要
自举重定位。入口 `_start` 把 `sp` 传进 `$a0`（这一句曾经漏掉，症状是
`ecode=7 (PPI) badv=0x0` 无限刷屏）。

流程：

1. 从 `sp` 读 `argc / argv / envp / auxv`；
2. `main_base = AT_PHDR - 64`，并**核对** `e_phoff == 64`；不等就
   `[ldso] FAIL: main e_phoff != 64; refusing to guess its base`——所有 ld.so
   都依赖这条约定，坏了就该失败而不是猜；
3. 解析主程序 `PT_DYNAMIC`：`DT_NEEDED / DT_STRTAB / DT_SYMTAB / DT_HASH /
   DT_STRSZ / DT_SYMENT / DT_RELA / DT_RELASZ / DT_JMPREL / DT_PLTRELSZ`；
4. 每个 `DT_NEEDED` 依次在 `/mnt/ram/`、`/lib/` 找；`mmap` 一段匿名 RW，
   逐 `PT_LOAD` `pread64` 铺进去，`.bss` 手工清零（**不要求 `MAP_FIXED`**）；
5. 符号解析：按名字线性扫 `.dynsym`（`nchain` 取 `DT_HASH[1]`），不是 ELF 哈希；
6. 重定位，**先库后主程序**：`R_LARCH_RELATIVE(3)` / `R_LARCH_64(2)` /
   `R_LARCH_JUMP_SLOT(5)`；其它类型
   `[ldso] FAIL: unsupported relocation type N`；
7. 打 `[ldso]` 行，恢复启动时的 `sp`，`jr AT_ENTRY`。

失败一律 `[ldso] FAIL: ...` + `exit(127)`。**装载器不允许「看起来成功」**。

边界（写在源码与门禁里）：只支持一级依赖（库的库明确报错）、最多 4 个对象、
最多 16 个程序头、无懒绑定（全部立即重定位）、无 TLS、不调用 `.init_array`。

---

## 4. 夹具：为什么不是「编译一个 .so」

本机工具链 `loongarch64-newlib-elf` 的 `ld 2.31.1` **不支持 `-shared` / `-pie`**，
`gcc -shared` 会静默产出静态 `ET_EXEC`（没有 `.dynamic`、没有 `PT_INTERP`）。
所以夹具走 `tools/elf/dynfixture.py`：先按静态 ELF 链接（要用 `.symtab` 里的
链接期地址），再按 manifest 重写成动态镜像——加 `PT_INTERP`、`PT_DYNAMIC`、
`.dynsym/.dynstr/DT_HASH/.rela.dyn/.rela.plt`，原有 `PT_LOAD` 逐字节保留，另加
一个只读 `PT_LOAD` 覆盖 ELF 头与程序头表（这样 `AT_PHDR = base + 64` 才成立）。

**重定位是 manifest 写明的，不是链接器算出来的**：这样才能让每条路径单独被
门禁打断，而不是「整套一起动、坏了看不出来是谁」。

| 文件 | 类型 | 关键内容 |
| --- | --- | --- |
| `libanswer.so` | `ET_DYN` | 导出 `answer_add / lib_g_addr / g_base / g_ptr / abs_ptr / got_answer`；自身 1 条 RELATIVE、1 条 `R_LARCH_64` |
| `dynmain` | `ET_DYN` | 需要 `libanswer.so`；1 RELATIVE + 2 `R_LARCH_64` + 2 JUMP_SLOT |
| `dynmain_exec` | `ET_EXEC` | 同一份源，链接基址 `0x02001000`，走另一条 bias 分支 |
| `dynmain_bad` | `ET_DYN` | **反假绿**：JUMP_SLOT 指向库里不存在的 `no_such_symbol` |

构建产物（本轮实测字节数）：`ld-ark.link.elf` 25184 B、`libanswer.so` 21163 B、
`dynmain` 22811 B（签名后 22843 B）、`dynmain_bad` 22515 B（签名后 22547 B）。

---

## 5. QEMU 实跑读数（2026-11-20，`-DARKRTOS_USE_DYNLOAD=1`）

```
[dyn] write /mnt/ram/ld-ark.so rc=0 size=25184
[dyn] write /mnt/ram/libanswer.so rc=0 size=21163
[dyn] write /mnt/ram/dynmain rc=0 size=22843
[dyn] write /mnt/ram/dynmain_exec rc=0 size=22843
[dyn] exec dynmain rc=0
[ldso] ark ld.so start sp=0x00000000021fc000 argc=1
[ldso] main base=0x0000000002000000 type=3 entry=0x00000000020013a8 interp_base=0x0000000002100000
[ldso] need libanswer.so
[ldso] lib libanswer.so base=0x0000000002110000 size=24576
[ldso] reloc libanswer.so: RELATIVE=1 64=1 JUMP_SLOT=0
[ldso] reloc dynmain: RELATIVE=1 64=2 JUMP_SLOT=2
[ldso] jump entry=0x00000000020013a8
[dyn] main entered through ld.so
[dyn] JUMP_SLOT answer_add(5) -> 105 ok
[dyn] RELATIVE *m_rel -> 33576384 ok
[dyn] RELATIVE value -> 7 ok
[dyn] R_LARCH_64 g_ptr -> 34689144 ok
[dyn] R_LARCH_64 abs_ptr -> 34689144 ok
[dyn] g_ptr data -> 100 ok
[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT all applied
[dyn] info dynamic=1 interp_base=34603008 interp_entry=34608608
[dyn] finished after 0 ms, exit code 105
[dyn] exec dynmain_exec rc=0
[ldso] main base=0x0000000002000000 type=2 ...
[dyn] PASS: RELATIVE + R_LARCH_64 + JUMP_SLOT all applied
[dyn] finished after 0 ms, exit code 105
[dyn] write /mnt/ram/dynmain_bad rc=0 size=22547
[dyn] exec dynmain_bad rc=0
[ldso] need libanswer.so
[ldso] lib libanswer.so base=0x0000000002110000 size=24576
[ldso] reloc libanswer.so: RELATIVE=1 64=1 JUMP_SLOT=0
[ldso] FAIL: unresolved PLT symbol no_such_symbol
[ldso] FAIL: relocation failed
[dyn] finished after 0 ms, exit code 127
```

四条判据同时成立才算过：

1. 两个正常变体（`ET_DYN` / `ET_EXEC`）都 6/6 ` ok`，打 `[dyn] PASS`，退出码 **105**；
2. `[dyn] info` 里 `interp_base=34603008` = `0x02100000`，`dynamic=1`；
3. 坏夹具打 `[ldso] FAIL: unresolved PLT symbol no_such_symbol`，退出码 **127**，
   且**没有多出一次 PASS**；
4. 同一次启动里 `[linux] PASS`、`[FS] PASS`、`[ELF] tamper check PASS` 仍在。

`[ldso] reloc` 的计数不是摆设：`libanswer.so` 是 1 RELATIVE + 1 `R_LARCH_64` + 0
JUMP_SLOT，主程序是 1 + 2 + 2，与 manifest 完全一致。`libanswer.so` 的基址两轮
分别在 `0x02110000` / `0x02112000`（堆分配位置随运行变化，正常）。

---

## 6. 门禁（`tools/dynlink_gate.py`，`ci_gate` 的 `dynlink` 项）

12 项，全过才 `DYNLINK-GATE OK`：

| 项 | 判据 |
| --- | --- |
| `default-off` | `arkrtos_autoconf.h` 里 `ARKRTOS_USE_DYNLOAD` 是 0 |
| `kconfig-wired` | Kconfig 项存在、`depends on ARKRTOS_USE_ELF`、不是 `default y` |
| `elf-guards` | `elf.c` 对两个板级宏有 `#error` 护栏 |
| `board-layout` | 让编译器把四个板级宏展开成整数（不自己解析宏引用链），验区间在窗口内且不重叠 |
| `off-no-blob` | 关档编 `elf_demo.c` 无夹具符号 / 开档有三个（**两向**） |
| `compile-on` | 2 个内核 TU + 3 个用户态 TU 在 LA64 真板级下编过 |
| `build` | `make ... EXTRA_CFLAGS=-DARKRTOS_USE_DYNLOAD=1` 通过（**先 clean**：make 不跟踪 `EXTRA_CFLAGS`，复用旧 `.o` 会造出「没有动态演示」的假绿） |
| `fixture-shape` | 逐字节读产物：`e_phoff==64`、有从文件头开始且覆盖 phdr 表的 `PT_LOAD`、`PT_INTERP` 字符串、`PT_DYNAMIC`、`DT_NEEDED` 条数、坏夹具里真有 `no_such_symbol`、ld.so 首个 `PT_LOAD` 等于解释器基址 |
| `fixture-rebuild` | 同一 manifest 再跑一次 `dynfixture.py`，与构建产物**逐字节相同** |
| `qemu-readout` | 第 5 节的四条判据，按行首锚点计数 |
| `parser-selfcheck` | 好日志的四种坏法（删一条 PASS / 把 127 改成 105 / 删 ldso 的 FAIL / 删一条 write）必须全被判红 |
| `restore-default` | 收尾重编默认档（否则下一次 `make` 会复用开档 `.o`，把默认档悄悄变成开档） |

---

## 7. 关档的体积与行为

| 项 | 默认档 41cb2f5（基线） | 本轮默认档 |
| --- | --- | --- |
| `.text` | 411360 B | 413752 B（+2392 B，+0.58%） |
| `.data` / `.bss` | 448 / 786964 | 448 / 786964 |

严格说这**不是**「体积零变化」，差的是 +2.4 KB，来源是把装载逻辑重构成共享的
`elf_measure_image / elf_check_range / elf_write_image` 与 `auxv` 铺栈那一段
（动态分支与静态分支共用），不是动态链接本身的代码——夹具 blob 一个字节也没进
默认镜像（`off-no-blob` 两向实测）。

行为面：默认档 QEMU 实跑与基线逐行比对，只有时间戳、tick 计数与一个模块基址
（因镜像大了 32 字节而平移）不同，全部 `PASS` 行一致、`[dyn]` 一次都没出现；
`PT_INTERP` 仍返回 `ARKRTOS_ELF_ERR_FORMAT`。唯一的口径差异是错误检测顺序：
旧实现是在扫 `PT_LOAD` 的循环里遇到 `PT_INTERP` 才拒，现在在扫段之前就先判
`PT_INTERP`——同一个错误码，只是更早。若一个文件同时有坏 `PT_LOAD` 和
`PT_INTERP`，报出来的是后者。

---

## 8. 明确不支持（不要读成「能跑 Linux 程序了」）

1. **现成发行版的 LA64 二进制跑不了**：它们的 `PT_INTERP` 指向 glibc 的
   `ld-linux-loongarch-lp64d.so.1`，仓里没有这个文件；本机也没有 LA64 Linux
   交叉工具链能产出这类二进制（本仓夹具是靠 manifest 重写出来的）。
2. **glibc 的 ld.so 要的系统调用面远超现在**：`futex`、`rseq`、TLS
   （`arch_prctl` / `set_thread_area`）、文件映射（`mmap` 带 fd）、
   `openat` 之外的整套 `*at`、`rt_sigaction` 等。内核侧一个都没有。
3. **`mprotect` accepted-not-enforced**：整窗一条 `PS=20` 条目，权限位不生效。
4. **没有懒绑定**：`JUMP_SLOT` 全部在跳转前做完；`DT_PLTGOT` 的第二项没被
   用来做 `dl_runtime_resolve`。
5. **没有 `.init_array` / `.fini_array` / TLS 初始化**：夹具不需要，真实 `.so`
   需要。
6. **只支持一级依赖**、最多 4 个对象、最多 16 个程序头；符号解析是线性扫。
7. **ld.so 自身不经过签名闸门**：本板 `ARKRTOS_USE_EXEC_SIGN` 未开；
   若将来开签名，解释器镜像也必须签名并由内核校验（现在的实现没有这一步）。
8. **QEMU 算上板，不是真机**：所有读数来自 `qemu-system-loongarch64`。

---

## 9. 复现

```bash
cd "/d/工作/git_project/ark rtos"
# 1) 编开档镜像
make -f example/qemu_loongarch64/Makefile \
     EXTRA_CFLAGS="-DARKRTOS_USE_DYNLOAD=1" -j8
# 2) 跑（-kernel 只吃相对名：仓库路径含非 ASCII，绝对路径 QEMU 打不开）
cd build/qemu_loongarch64
timeout 90 "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe" \
        -m 512M -kernel ark_qemu_la64.elf -serial stdio -display none
# 3) 一条命令做完上面所有判据（含关档符号、夹具形状、日志判读与自检）
cd "/d/工作/git_project/ark rtos"
py -3 tools/dynlink_gate.py
```

`dynlink_gate.py` 收尾会把构建目录恢复成**默认档**。

---

## 10. 相关文档

- 系统调用面与两个 ABI 的分界：[Linux可执行文件兼容-系统调用ABI.md](Linux可执行文件兼容-系统调用ABI.md)
- 装载器的两遍法与窗口布局：[MMU进程模型与ELF装载.md](MMU进程模型与ELF装载.md)
- 载体分级（L1/L2/L3）与「QEMU 算上板」的口径：[验证载体分级.md](验证载体分级.md)