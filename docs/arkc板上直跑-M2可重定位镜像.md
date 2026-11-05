# arkc 板上直跑 — M2 可重定位镜像（四遍差分）

本文是 [arkc编译产物上板运行-实施计划.md](arkc编译产物上板运行-实施计划.md) 里 **M2（L2 可重定位，走既有 mini）** 的落地记录：
在 STM32H743 板上，用板上跑的 arkc 把同一份源码**链接四遍**，差分出重定位表，打成标准 `.svcm`，
再交给**没有为这件事改过一个字节**的 `mini run` 装进 RAM 池的任意空块执行。

日期：2026-11-05。读数全部来自真机串口（COM3 / 115200）与宿主对拍程序，没有推断出来的数字。

---

## 1. 一句话结论

`cc --pack /tmp/p.c -o /tmp/p.svcm` → `mini run /tmp/p.svcm` → 串口出现源码里独有的两行：

```
BRD-M2-R1 alive
BRD-M2-R1 tag alpha
```

程序自己退出，code 块与 RAM 块**都回到池里**。镜像 3288 B（负载 3008 + 表 6×4），
四遍链接共 267 ms。重定位表四类（整字 ROM / 整字 RAM / MOVW-ROM / MOVW-RAM）**各自非零**，
没有哪一类是「0 次命中所以等于没验证」。

---

## 2. 四遍差分到底在做什么

镜像被链接在**标称基址**上，运行期却会被装到 RAM 池里任意一个 2 的幂对齐的空块。
代码里所有绝对地址（函数指针、字符串字面量、指向自身 RAM 的指针、`MOVW/MOVT` 立即数里的地址）
都必须随落点平移。差分链接是**不依赖编译器配合**的唯一办法（不改上游 codegen）：

| 遍 | 链接基址 |
|----|----------|
| A | 标称（ROM = `rom_base`，RAM = `ram_base`） |
| B | ROM = `rom_base + delta`，RAM 不变 |
| C | RAM = `ram_base + delta`，ROM 不变 |
| D | ROM = `rom_base + 2*delta`，RAM = `ram_base + delta`（**只用于验证**，不会成为镜像） |

判据：

- A→B 恰好平移 `delta` 且 A→C 不变 ⇒ **ROM 类**
- A→C 恰好平移 `delta` 且 A→B 不变 ⇒ **RAM 类**
- A、B、C 三者完全相同 ⇒ 不是绝对地址
- 其余 ⇒ **拒绝**（`-31`），因为该位置依赖基址的方式超出模型

打包最后一步：把表应用回 A，必须**逐字节等于 D**，不等就拒绝出镜像（`-32`）。
于是「这份镜像能不能搬到别的 2 的幂对齐落点」在打包阶段就被证明，而不是留到板上随机崩。

`delta` 必须 ≥ 两个窗口长度：Delta 小于窗口时 B/C 的地址段会与 A 重叠，
「该位置的值恰好差 delta」就不再是「它是个绝对地址」的证据——重叠段里一个普通常量也可能满足。
这不是保守，是判据成立的前提（`-30`）。

---

## 3. 五个坑（每一条都是先出现错答案，再定位）

前三条是「**给出能跑的镜像但语义错**」这一类，第四条是「**给出正确的表但静默丢项**」，
第五条是「**验证通过但验证的是错的东西**」。它们都不可能靠「跑起来了吗」发现。

### 3.1 MOVW/MOVT 立即数对必须先判，再判整字

`MOVT` 的前半字（`0xF2C0 | (imm4<<8) | (i<<12) | rd`）在 A/B/C 三份里**完全一致**，
只有后半字 `MOVW` 的 `imm8` 随基址变。于是**按 4 字节整字看**，这一对恰好等于 `+delta`，
被误判成「整字 ROM 表项」。它的起点常落在 `2 mod 4` 上——装载器本来就会拒收非对齐的整字项，
但更麻烦的是：`MOVW` 的后半字被当成「表项已经处理过了」，于是**真正的 MOVW 对再也没被扫到**。

修法（两侧同构）：扫描循环里插入**步骤 0**——先尝试把 `pos` 解释为 `MOVW/MOVT` 立即数对
（`pos+8<=len`，三份都能解出对，且三个值**不全相同**），命中则记一条 MOVW 表项并 `pos += 8`。
整字判降为步骤 1，半字一致判为步骤 2/3。

### 3.2 整字表项必须 `pos % 4 == 0`

与 C 侧一致：装载器 `arkrtos_loader.c` 拒收非 4 字节对齐的整字项。
Python 侧原本没有这个闸门，于是能在 `0x482` 这种位置产出「能通过 `verify` 的表项」——
**`verify` 通过只证明「表应用后等于 D」，不证明表项落在合法位置**。修法是给 Python 补上
`pos % RELOC_SIZE == 0`，把 Python 对齐到 C 的严格性，而不是放宽 C。

### 3.3 `kind != 0` 不能当命中判据（本轮最隐蔽的一条）

`ARKRTOS_APP_RELOC_KIND_ROM == 0`。C 侧原来的写法是：

```c
if (kind) { 记表项; }
```

于是**所有整字 ROM 表项被静默丢掉**——差分确实算出来「这里是 ROM 绝对地址」，
但判断「有没有命中」用的是「种类非零」，而 ROM 类的种类值本来就是 0。
症状是表比 Python 少项、`verify` 反而照样过（少一项时 D 遍位置可能恰好没被覆盖到）。

修法：引入独立的 `uint32 hit = 0u;`，由**是否算出可用差分**设置 `hit`，判命中用 `hit` 而不是 `kind`。

### 3.4 负载必须是 A，不能是「被打了补丁的副本」

`verify` 的做法是把表应用到 A 的副本上，和 D 比较。原来那份副本**被直接当成了镜像负载**，
于是镜像是「标称地址 + delta」的产物——头部 CRC 正确、`mini run` 装载成功、然后跑飞。
修法：`verify` 之后 `for (i...) payload[i] = a[i];` 还原成标称形态。

### 3.5 `image_id` = 负载 CRC，且必须写在头 CRC **之前**

- `image_id` 的语义是**负载 CRC32**（不是镜像 CRC）。
- 它落在被 CRC 覆盖的头部区间内。原来在算完头 CRC **之后**回填 ⇒ 头 CRC 必然对不上，
  板上表现为 `mini run` 返回 `rc=-5`（CRC 校验失败）。

修法：`p_hdr->image_id = arkrtos_crc32(payload, len, 0u);` 放在算头 CRC 之前，删掉末尾的回填。

镜像 CRC 的覆盖顺序（`arkrtos_loader_crc_header`）：头（`crc32`/`state`/`runtime_ram_base`
三字段**按 0 代入**，用真实的 4 个 0 字节）→ 重定位表 → 负载。

---

## 4. 宿主对拍：先用 1/100 的成本把缺陷找出来

差分与打包是纯计算。把它搬到宿主上，用 MinGW gcc 直接编 `kernelsrc/src/arkrtos_apppack.c`：

```sh
cc -std=c99 -DARKRTOS_USE_APPPACK=1 -Ikernelsrc/include -Iconfig/stm32h743 \
   kernelsrc/src/arkrtos_apppack.c <harness.c> -o harness.exe
```

（`variably modified` 是既有噪声，不是本轮引入的。）

harness 里**自己写真实的 `arkrtos_crc32`（与 zlib 逐位一致，poly `0xEDB88320`，前后取反）
与 `arkrtos_loader_crc_header`**——否则 CRC 对不上时看不出是算错还是字段错。

```
rc=0 payload_equals_A=1 image_id_ok=1 header_crc_ok=1
n_rom=1 n_ram=1 n_rom_movw=2 n_ram_movw=2
out_len=3396 reloc_count=6
0x0118 kind=1 / 0x011C kind=0 / 0x0596 kind=2 / 0x05D4 kind=3 / 0x065A kind=2 / 0x0748 kind=3
```

Python `diff_reloc` 对同一输入产出
`0x0000 kind=1 / 0x0004 kind=0 / 0x047E kind=2 / 0x04BC kind=3 / 0x0542 kind=2 / 0x0630 kind=3`，
`verify OK`。偏移差 `0x118` 正是 `payload_prefix` ⇒ **两侧逐项一致**。

### 4.1 反假绿（宿主，成本几乎为零）

| # | 注入 | 期望 | 实测 |
|---|------|------|------|
| ① | `D_FILE = pB.bin`（验证遍不是真 D） | 拒绝，报 verify | `-32` |
| ② | `C_FILE = pD.bin`（窗口遍串了） | 拒绝，报 diff | `-31` |
| ③ | `DELTA = 0x1000`（小于窗口） | 拒绝，报 param | `-30` |
| ④ | `DELTA = 0x90000`（与 D 用的不一致） | 拒绝 | `-31` |
| ⑤ | Python 逐项删表后重放 | 全表 ⇒ 等于 D；**删任意 1 项**（6/6）⇒ 都不等于 D | 6/6 全部不等 |

第 ⑤ 条是「判据 A 的『故意删表项必须出问题』」在宿主的等价实现：
如果删掉某一项后仍然等于 D，说明那一项本来就没被 D 遍覆盖到，判据没有测到那个地址。

---

## 5. 板上读数

```
cc: pack /tmp/p.c -> /tmp/p.svcm (4 passes, -O1)
cc:   A 3006 B / B 3006 B / C 3006 B / D 3006 B
cc: /tmp/p.svcm: payload 3008 + table 6 x 4 = 3288 B
cc:   reloc rom 1 / ram 1 / rom_movw 2 / ram_movw 2, entry +0x428
cc: packed in 267 ms
mini run /tmp/p.svcm
[...] MINI:568 /tmp/p.svcm: 3008 B payload -> code 0x24070000 (4096 B block),
        RW/stack 0x24072000 (8192 B), task 4
BRD-M2-R1 alive
BRD-M2-R1 tag alpha
[...] MINI:862 task 4 returned: MiniApp finished, code and RAM blocks back in the pool
mini run: /tmp/p.svcm loaded but no longer running (…)
mini: none running (usage: mini run <path>)
```

- 负载补零到 4 的倍数：3006 → 3008（`arkrtos_apppack_build` 要求长度为 4 的倍数）。
- 四类表项计数非零，且与宿主对拍的同名计数**同值**（1 / 1 / 2 / 2）。
- 跑完 code 块与 RAM 块**都归还**；`mini: none running` 是正常收尾（程序已经自己退出）。

### 5.1 `cc --pack` 的常量（板上）

| 常量 | 值 |
|------|-----|
| `CC_PACK_WIN_SIZE` | 8192 |
| `CC_PACK_OUT_CAP` | 16384 |
| `CC_PACK_ARENA` | 32768（**每遍都从池里重借，不是四份并存**） |
| `CC_PACK_ROM_BASE` | `0x08000000` |
| `CC_PACK_RAM_BASE` | `0x20000000` |
| `CC_PACK_DELTA` | `0x00080000` |
| `CC_PACK_SCT` | `/tmp/pp.sct`（四遍共用一个，结束时删掉） |

`cc_reset_state()`（`config_init` + `g_config.enable_fpu_hw=1` + `arkc_port_zero_arena()` +
`diag_init`）**每一遍都调**，所以四遍各有干净 arena——这是「同一进程里链四遍」能成立的前提。

---

## 6. 体积账

| 档 | FLASH | RAM |
|----|-------|-----|
| 0 档（`make clean && make -j8`） | **381808 B** | 与 M1 相同 |
| 开档（`USE_APPPACK=1 USE_MINI_VFS=1`） | 388704 B (74.14%) | 439300 B (97.50%) |

0 档与 M1 基准**字节数一致**（md5 差异只来自 `__DATE__/__TIME__`）。
`arkrtos_apppack.c` 按 `USE_APPPACK` 过滤进 `sources.mk`，默认不编。

`ARK_RAMFS_MAX_FILES = 3`（每文件 16 KB 静态缓冲）：`/tmp/p.c` + `/tmp/pp.sct` + `/tmp/p.svcm`
正好三个，测试脚本必须先 `rm` 再写，第四份一律失败。

---

## 7. M2 的判据达成情况

| 判据 | 状态 |
|------|------|
| A `cc --pack` → `mini run` 出字、退出、块归还 | **通过**（§5） |
| B 同一镜像在两个不同落点各跑一次 | **未做**：`mini` 的 code 块是首适配，需要先占住 4 KB 块才能逼它换落点；本轮先用「表项四类非零 + D 遍逐字节相等」作为可重定位性的证据，落点切换登记为下一步 |
| C 程序含带初值全局 / 字符串字面量 / 函数指针 / 跨文件调用 | **部分**：`p.c` 覆盖了字符串字面量与函数指针（MOVW 对 ×4），**跨文件调用未覆盖**——ramfs 只有 3 个槽，`-link` 多输入要 4 份以上文件 |
| 反假绿 ① 删表项必须出问题 | **通过**（宿主 6/6，§4.1 第 ⑤ 条） |
| 反假绿 ② 打印四类分布 | **通过**（1 / 1 / 2 / 2） |
| 反假绿 ③ `verify` 的 D 遍必须真跑且不匹配就拒绝 | **通过**（注入 D 错 ⇒ `-32`；表应用回 A 后与 D 逐字节比） |

**弱项如实记账**：判据 B 与 C 的「跨文件调用」没有做完，原因分别是「需要先占块」与
「ramfs 槽位不够」。它们不改变「板上编出的可重定位镜像能在板上跑起来」这条结论，
但**也不能说 M2 的判据全绿**。

---

## 8. 已知边界

| 边界 | 现状 | 归谁 |
|------|------|------|
| 单文件源码 | `cc` 一次编一个 `.c`；`-link` 已接线但 ramfs 槽位不够 | M2 之后（槽位或持久化） |
| 镜像在 VFS 的 ramfs 上 | 掉电即失 | M3-b（QSPI W25Q64 + littlefs） |
| 板上产物比宿主小 **108 B**（3006 vs 3114） | 同一份 `p.c`、同 `-O1`，板上 3006 B、宿主 3114 B；两边都在各自基址上自洽（向量表、入口、表项差分都成立），**成因未查明** | 登记为未查明，不猜。若继续推进，下一步应先比对两边 `.s` 的段落尺寸 |
| 池 80 KB | 长源码 `-O1` 完整流水线会耗尽池 ⇒ 测试源必须短 | 已知，不修 |

---

## 9. 相关代码位置

| 文件 | 内容 |
|------|------|
| `kernelsrc/src/arkrtos_apppack.c` / `include/arkrtos_apppack.h` | 四遍差分、表组装、D 遍验证；错误码 `-30/-31/-32/-33/-34` |
| `tools/pack_app.py` | 同算法的 Python 实现（PC 对拍的另一半）；本轮把扫描顺序与对齐闸门对齐到 C |
| `kernelsrc/include/arkrtos_app_image.h` | 镜像头、表项编码（`(off/2)<<2 | kind`）、四类 kind 的值 |
| `kernelsrc/components/arkc/arkc_shell_cmd.c` | `--pack` 的接线（四遍链接 + 调 apppack + 落盘） |
| `kernelsrc/components/arkc/arkc_port_arkrtos.c` | 四遍各自的 arena 重置与捕获钩子 |
| `kernelsrc/src/arkrtos_mini.c` | `ARKRTOS_MINI_READ_VFS=1` 时从 ramfs 读镜像（M3-a） |
| `kernelsrc/src/arkrtos_vfs.c` | `arkrtos_vfs_read_at(path, buf, len, off, &got)` |
| `example/stm32h743/kernel/ARKRTOS_TEST/GCC/Makefile` | `USE_APPPACK ?= 0` |
