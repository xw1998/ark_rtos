# Release 0.0.3-preview.1

> Condensed English edition. The Chinese page (Release-0.0.3-preview.1.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

- tag: `v0.0.3-preview.1`
- version (inside the firmware): `0.0.3` (`ARKRTOS_VERSION_STRING`, readable with the `version` command)
- release type: **preview**. Criteria and grading: [发行版与版本策略.en.md](发行版与版本策略.en.md) §5.
- previous capability record: `0.0.2` as written in readme "License and versions" (**never tagged**;
  this version is the repository's first tag). Nothing recorded for 0.0.2 was rolled back.

In one sentence: **a kernel preview that installs, runs, switches versions, recovers from a crash,
and builds cleanly on all four project sets**; what it lacks is the "externally promised stability"
boundaries, all listed in §5.

## 1. Changes in this version

### 1.1 Build and configuration: Kconfig becomes the single source of values (`d9be839`, `6c2e83d`)

| item | before | now |
|---|---|---|
| where feature switches live | Keil project's `<Define>` (five forced-on macros overriding Kconfig) | only `USE_HAL_DRIVER` / `STM32F4xx` / `ARKRTOS_BOARD_CONFIG` left in the project |
| shared header `arkrtos_autoconf.h` | hand-written, drifting from `arkrtos_defconfig` | generated from `config/arkrtos_defconfig`, byte-compared by `ci_gate`'s `config-generated` |
| board differences | via project macros | `#undef` + `#define 0` in `board/<chip>/arkrtos_board_config.h` |
| GCC projects | feature macros piled into `DEFS` | per-build header generated every build, `-I` first so it shadows the shared one; knobs like `make USE_SHELL=0` still work and changing a knob forces a rebuild |
| defconfig | 1 | 5: `arkrtos` (source of the shared header) / `stm32h743` / `stm32f427_gcc` / `minimal` / `minimal_noshell` |

Real defects fixed along the way (not refactors — these were already wrong):

1. **The softbus port glue could not be switched off**: `ark_bus_hal_arkrtos.c` (vendored) had no
   switch guard and still referenced three hooks with `ARKRTOS_USE_SOFTBUS=0` ⇒ link failure. Fixed by
   adding the guard in the ark_softbus **source repo** and re-syncing (`1c3c27c`), not by editing the
   vendored copy.
2. **`arkrtos_utest.h` sat inside `#if ARKRTOS_USE_MEMPOOL`** while the use site is inside
   `#if ARKRTOS_USE_UTEST` ⇒ differing switch values failed to compile (1 error in the minimal build).
3. **`Kconfig.features` was structurally broken**: two merged `endmenuconfig` lines, and the UTEST
   block missing `bool/default` so it parsed as `n` — the root cause of "F427 Keil size mysteriously
   6738 B smaller".
4. **Four ON-build gates had an incomplete input environment** (`lwip_port.h` / `ark_bus.h` not
   found): added `tools/arkrtos_f427_env.py` carrying the F427 project's real header paths and
   `-DARK_BUS_*` level macros.

### 1.2 Capabilities and real-hardware evidence

Every item in this release has an on-board reading; the criteria and anti-false-green forms are in the
respective documents:

| capability | evidence | commit |
|---|---|---|
| tickless idle | interrupts down to **27.6%** (1/3.6); 20 s idle gains 40422 ticks = 20.2 s with no lost tick; `rt` gained a line so "switch did not take effect" and "did not get slower" do not look alike. **Current not measured** | `908d56e` |
| mempool App/driver SVC channel | `MPOOL_DEMO` all 33 cases pass, block addresses inside App RAM; six classes of invalid input each rejected | `042e34f` |
| planned small-program handover | holding `led2`, `return` normally from `main`: `handover=1`, `neutralized=0`, `led2` own entry removed | `d2496b0` |
| A/B hot switch (default off) | four conclusions pass; the original design claim "the data segment is not reset" **was refuted by measurement** and the docs were changed | `0159417`, `82e6f42` |
| crash ledger across reset | 3 crashes → disabled, still held after `reboot`, and still taken over after the slot index drifts | `90f8d2d` |
| security baseline self-check `sec` | 12 read-only checks (pass 8 / fail 0 / finding 2 / skip 2), T1–T12 all have readings | `2d0c41f`, `89941f2`, `34abade` |
| option-byte truth | `arkrtos_port_flash_protection()` reads `rdp=0`, `wrp lo/hi=0x0000`, matching the debugger's direct `OPTCR` read | `9d5f7ae` |
| real-time metrics | DWT cycle counting for four latencies + `rt` command + gate | `26331f6` |
| stream buffer / message buffer | F427 on-build 11/11 cases, 104 assertions; `send_from_isr` in a real interrupt wakes a blocked reader (2 ticks = 1.0 ms) | `2356fcf`, `8a1941e`, `ccc668c`, `11ed851` |
| on-board compile and persistence (arkc / arkmake) | M1 direct run, M2 relocatable four-pass differential, M3b QSPI persistence, M3-d cross-reset autostart, M4 hello world | see `docs/arkc板上直跑-*.md` |
| softbus software layer | frame auth / payload encryption / anti-replay / handshake session keys / mesh routing / Raft / signed firmware, **all default off, zero size and zero behaviour change**; on-board loopback `checks 56 failed 0` | `docs/软总线接入.en.md` |

### 1.3 Readability and engineering books

| item | result | commit |
|---|---|---|
| `SVC_Server` giant dispatch | 1318 lines → 58-line table + 17 handlers; `Code` +408 B (+0.19%), behaviour re-verified on board | `cb3690f` |
| SVC service / subcommand numbers named | 17 service numbers + about 105 subcommand numbers into `arkrtos_svc_abi.h`; 108 sites in the kernel, 111 in the SDK, values unchanged everywhere | `4796ab1` |
| return codes named | 26 modules, 397 sites, values unchanged everywhere; only two deliberately raw `-1` remain repo-wide | `06c4a83`, `8ee8371` |
| unified example entry points | 16 app_sdk examples + 4 mini examples unified to `Src/main.c` | `ef8054c` |
| checkable debt | added [已知限制.en.md](已知限制.en.md) + [欠账盘点.en.md](欠账盘点.en.md) (each item with a check command and criterion), added the `doc-index` gate to `ci_gate` | `aaec450` |
| release mechanism takes shape (new here) | version 0.0.2 → `0.0.3`; added [发行版与版本策略.en.md](发行版与版本策略.en.md) (version rules, release artifact list, ABI-bump criteria, preview/stable/1.0 criteria, six release steps); added `tools/release_check.py` as `release-consistency` in `ci_gate` (three version sites agree + release note exists with an "unverified" section + latest tag aligns with the version) | this release |

## 2. Size ledger (measured 2026-11-14, re-runnable)

F427 kernel project, Flash = Code + RO + RW, RAM = RW + ZI:

| build | Code | RO-data | RW-data | ZI-data | Flash | RAM |
|---|---|---|---|---|---|---|
| default | 220094 | 19198 | 896 | 120852 | 240188 B ≈ 234.6 KB | 121748 B ≈ 118.9 KB |
| minimal (Shell still on) | 63110 | 8350 | 388 | 43172 | 71848 B ≈ 70.2 KB | 43560 B ≈ 42.5 KB |
| minimal + Shell off | 36886 | 1446 | 332 | 28608 | 38664 B ≈ 37.8 KB | 28940 B ≈ 28.3 KB |

The other three project sets (default configuration):

| project | result |
|---|---|
| F401 Keil | `116578 / 13034 / 476 / 53472` |
| F427 GCC | `text 259412 / data 368 / bss 113536`, FLASH 99.10% |
| H743 GCC | FLASH 448604 B (85.56%) / RAM 443300 B (98.39%) / CCMRAM 22732 B |

> The three sizes only mean "compiled and linked"; none was run on board, and functional completeness
> is only "links" level evidence (see the same passage in [功能开关.en.md](功能开关.en.md)).

## 3. Reproduction

```bash
# gates (29 items: encoding/line endings, config consistency, switch declarations, 9 host tests,
# 4 ON-build syntax gates, release-consistency)
env -u PYTHONHOME -u PYTHONPATH py -3 tools/ci_gate.py

# version consistency (version header / Chinese and English README / release note / tag)
env -u PYTHONHOME -u PYTHONPATH py -3 tools/release_check.py

# Keil: F427 / F401 kernel projects (-r full rebuild)
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -r \
  example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx -j0 -o C:/tmp/f427.log

# GCC: F427 / H743 (CROSS is mandatory, otherwise make (e=2))
cd example/stm32f427/kernel/ARKRTOS_TEST/GCC && make -j4

# trim to the minimal build
env -u PYTHONHOME -u PYTHONPATH py -3 tools/kconfig/genconfig.py \
  --kconfig Kconfig --defconfig config/minimal_noshell_defconfig \
  --out <target project's arkrtos_autoconf.h>
```

## 4. Compatibility and upgrade notes

| item | this version | vs the 0.0.2 record |
|---|---|---|
| `ARKRTOS_PARTITION_VERSION` | `10` | unchanged |
| `ARKRTOS_HW_COMPAT_ID` | `0x42700005` (compat generation 5) | unchanged |
| SVC service / subcommand numbers | `arkrtos_svc_abi.h` | **renamed only, values unchanged**; old App binaries still install |

- **Old `.svcapp` / `.svcm` images do not need rebuilding**: this version changed no image header,
  no shared partition-table structure and no SVC number value.
- **What must be regenerated is the config header**: if you ever copied `arkrtos_autoconf.h` into your
  own project, regenerate it from this tag's `config/arkrtos_defconfig` (`ci_gate` compares that file).
- After project macros are zeroed, **do not add feature macros back into Keil `<Define>`** — they
  override Kconfig and the gate will not notice.

## 5. Unverified (the preview's boundary, must not be omitted)

Functional boundaries:

- **Real Ethernet send/receive**: there is no board-level PHY driver, MDIO, `eth_` interface or RMII
  pin assignment; lwIP runs on a loopback netif; **not one ping or capture on a real link**.
- **Two softbus nodes over a physical line**: only in-board loopback (`checks 56 failed 0`); the board
  has a single USART1 taken by the console, so two boards / board + PC over a real serial line is
  **unverified**.
- **shell authentication** and the **RDP/WRP deployment path**: `sec` reads real RDP/WRP values, but
  setting them (irreversible) and shell authentication are not done; production signing needs the real
  key written into the build on the production line.
- **tickless current**: the interrupt rate is down (27.6%), but there is **no ammeter reading**, so
  the "how much power is saved" cell is empty.
- **softbus auth / encryption / routing / Raft / firmware signing**: all default off, with host and
  loopback level evidence only, no multi-node on-wire measurement.

Engineering boundaries (these do not block "it works", they block "someone can take over"):

- No remote CI (gates run locally only), no static analysis (no cppcheck / MISRA / clang-tidy).
- 22 kernel files are still GBK (77 UTF-8), no `.clang-format` / `.editorconfig`.
- Test coverage: 39 kernel files / 828 functions; 9 host gate tests, 9 board utest cases (12 cases /
  115 assertions with `SBUF` on). No coverage tooling wired.
- `kernelsrc/shell/arkrtos_shell.c` is still a 6244-line single file (the split plan exists, the code
  is untouched).

Per-item check commands: [欠账盘点.en.md](欠账盘点.en.md) and [已知限制.en.md](已知限制.en.md).

## 6. What this version does not promise

- **No ABI stability**: during 0.x the next `MINOR` may change the SVC surface, and at that point
  "old images need rebuilding" will be written down.
- **Nothing beyond the verified capabilities**: the table in §1.2 is the complete set; do not infer
  "it probably also works" about anything not listed.
- **No long-term support branch**: there is no LTS definition yet; `0.0.x` keeps tags only, no
  parallel maintained branches.
