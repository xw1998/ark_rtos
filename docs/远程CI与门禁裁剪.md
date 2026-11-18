# 远程 CI 与门禁裁剪

这页只说一件事：把 `tools/ci_gate.py` 放到 Gitee 的 runner 上跑之后，**哪些检查
真的跑了、哪些没跑、为什么**，以及怎么在不骗人的前提下看这份日志。

一句话：远程档跑的是**裁剪过的项集**，`skip` 不等于 `pass`；日志里有一行
`NOT VERIFIED on this run: ...` 把所有没跑的检查列出来。

## 1. 为什么需要裁剪

`ci_gate` 的四十几项里有 17 项要驱动**只存在于开发机上的工具链**：

| 工具链 | 谁在要它 | 用在哪 |
|---|---|---|
| Keil `armclang` | `tickless` `rtstat` `eset-syntax` `waitmulti-syntax` `waitq` `smp` `seccheck` `swap` | 用 `-fsyntax-only` 编「开关打开档」的 TU |
| `arm-none-eabi-gcc` | `arkmake` | 板上 make 的语法档 |
| LoongArch64 / RISC-V 交叉 gcc | `paging` `elf` `modld` `payload-sign` `stack-guard` | `paging_gate.py` / `elf_gate.py` / `modld_gate.py` 等真编两套 TU、跑负向用例 |
| RV32 交叉 gcc（要 GCC 10+，`-fanalyzer`） | `analyzer` | 逐 TU 跑路径敏感静态分析（48 个 TU + 两条喂毒自检） |

Gitee Go 的 runner 上这些一个都没有。三种糊弄法都不接受：

* 让它们在 runner 上**照旧去跑** ⇒ 每次构建都是一片假红（缺工具链被报成「代码坏了」）；
* 干脆**从表里删掉** ⇒ 本机也不再跑，真回归没人看得见；
* 把**缺工具链说成通过** ⇒ 这是最坏的一种，等于把门禁变成装饰。

所以做的是第四种：`--remote` 模式下这些检查**显式跳过并单独报数**，末尾打印
清单，退出码只看真失败。

## 2. 远程档的读法

`.workflow/ci.yml` 在 master 上每次 push 触发一次：

```
python3 tools/ci_gate.py --remote
```

日志末尾长这样（下面是本机 2026-11-18 用 `--remote` 跑出来的实际读数，runner 上
应当一致）：

```
skip tickless                   needs armclang (Keil MDK) (not on the runner)
...
skip stack-guard                needs LA64 / RV32 cross gcc (not on the runner)
skip analyzer                   needs RV32 cross gcc with -fanalyzer (GCC 10+) (not on the runner)
ok   remote-ci                  workflow calls the gate with --remote; 17 skipped check(s) all named in the table

30 check(s) passed, 0 failed, 17 skipped (remote profile)
NOT VERIFIED on this run: analyzer, tickless, arkmake, rtstat, eset-syntax,
waitmulti-syntax, waitq, smp, seccheck, swap, paging, elf, shell-auth, modld,
payload-sign, stack-guard, sec-crc
```

三条口径：

1. **`skip` 行不是 `ok` 行**：它们不进 `PASSED`，也不改退出码；一条都没跑过的
   检查不可能因为「没跑」而被读成「通过」。
2. **`remote-ci` 是一项真检查**：它确认 `.workflow/ci.yml` 存在、真的指向
   `tools/ci_gate.py` 且带了 `--remote`，并确认 `LOCAL_TOOLCHAIN` 里每个名字都
   在 `CHECK_TABLE` 里（名字写错会让本机检查在 runner 上照旧去要工具链）。
3. 本机跑的仍然是**全量档**（`python tools/ci_gate.py`），远程档只是补充，
   不替代本机读数。

## 3. 远程档覆盖到什么（以及不覆盖什么）

**在 runner 上真跑的**：结构类（分区孪生、单一地址源、编码/行尾、文档索引、
API 新鲜度、布局、无分区头、Kconfig 生成物、DTS 生成物、发行一致性、软总线
同步头）+ 宿主编译与运行类（utest/sdk-stdio/sbuf/mempool/mq/sync/eset/waitmulti/
loader/fifo/fault/coverage/warn-clean/sign-host）+ `remote-ci` 自身。

注意 `warn-clean` 用的是宿主 gcc（`-Wall -Wextra -Wshadow`），runner 上有；
`analyzer` 要的是带 `-fanalyzer` 的 **GCC 10+**，本机只有交叉编译器那一份，
所以远程档跳过它——这和 `warn-clean` 是两件事，别混读。

**没跑的四类，各自的替代取证在哪**：

| 没跑 | 替代取证 |
|---|---|
| Keil 语法档（tickless/rtstat/eset/waitmulti/waitq/smp/seccheck/swap/sec-crc） | 开发机上 `python tools/ci_gate.py` 全量跑；判据是那一行的 `ok`/`FAIL` |
| `arkmake` | 同上 |
| 分页 / ELF / shell-auth / modld / 栈守卫 / 尾部签名（LA64 + RV32） | 同上；另有 QEMU 实跑读数（见 [QEMU模拟器移植与验证](QEMU模拟器移植与验证.md)） |
| 静态分析（`analyzer`，RV32 + `-fanalyzer`） | 同上；判据是 `tools/analyzer_gate.py` 的 `ANALYZER-GATE OK (48 TU, 0 diagnostic, 2 self-check probes fired)` |
| Keil 工程构建（`--build`，本来就不在远程档） | 开发机 `--build` + 真机回归 |

## 4. 本文件的状态（未验证部分写死）

* **工作流文件本身未验证**：本机没有 Gitee Go runner，`.workflow/ci.yml` 的
  schema 是按 Gitee Go 帮助中心（「触发事件」「任务编排」）的字段写的，第一次在
  网页上启用流水线时以平台校验结果为准。字段用到的只有：`version` / `name` /
  `displayName` / `triggers.push.branches.precise` / `stages[].stage.name` /
  `displayName` / `strategy` / `trigger` / `steps[].step: build@python` /
  `pythonVersion` / `commands`。
* **runner 上的读数未验证**：`--remote` 的**本机**行为已有读数（上一篇引用的
  30/0/17），但「runner 上 Python 3.9 + 系统 gcc 能不能让 30 项全过」没跑过。
  已知的两处风险：`coverage-host` 需要 `gcov`；`api-doc` 需要
  `git diff` 在工作区可读（Gitee Go 检出是普通 clone，应当满足）。
* 因此**不得把「远程档存在」读成「门禁已经离开本机」**：它只是多了一条在别处
  跑的证据链，真失败的发现仍然主要靠本机全量档。

## 5. 接进来之后还剩什么

1. runner 上第一次真跑，把 `NOT VERIFIED` 清单与失败项记进这页（这一步只能看到
   结果才能写，不预填）。
2. 想让 Keil 那半边也上云，需要自建 runner（装 MDK）或者把语法档改写成
   `arm-none-eabi-gcc` 能编的形态——后者会改动「用哪个编译器代表板上答案」这个
   口径，得先定。
3. **真静态分析已接**（2026-11-18）：`analyzer` 项用本机交叉工具链的 GCC 10
   `-fanalyzer` 跑 48 个 TU，判据与边界写在 [欠账盘点](欠账盘点.md) 的静态分析行。
   它和远程 CI 在这一点上分道：分析只在有工具链的机器上跑，runner 上显式 skip。
