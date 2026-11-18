# Comparison with Mature RTOSes: FreeRTOS / RT-Thread / Zephyr

> Condensed English edition. The Chinese page (与成熟RTOS对比.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

> **Conventions**
> * The **Ark RTOS** column comes from this repository's current state (source + `docs/`) and is checkable item by item.
> * The **other three** columns come from their official documentation (links at the end) and state only stable facts.
> * "Missing" is graded in three tiers: **positional gap** (not planned) / **debt** (should be done, not done) / **different form** (done, but shaped differently).
> * The comparison with Linux is a separate account, see 与Linux差距盘点.md; this page only covers "does another MCU RTOS have what I have".

## 1. Position first: four RTOSes are not on the same track

| | FreeRTOS | RT-Thread | Zephyr | **Ark RTOS** |
|---|---|---|---|---|
| Origin | Real Time Engineers (now AWS) | Chinese community/commercial dual track | Linux Foundation | independent project |
| Pitch | **smallest kernel + widest port surface** | **componentisation + package ecosystem** | **vendor neutral + board coverage** (450+ boards) | **let an MCU install/upgrade/remove apps and drivers like a phone** |
| Kernel shape | a pile of `.c`, macro-configured | object-oriented kernel + device framework | kernel + subsystems | kernel + privilege isolation + installer |
| Abstraction | `FreeRTOSConfig.h` | Kconfig + SCons | **devicetree + Kconfig + west** | **Kconfig + DTS subset (dtc_lite)** |

In one line: the first three aim to "be the base of someone else's product", Ark RTOS aims to "let the same MCU **install later** apps and drivers, and a crashed installation cannot hurt the others". Every difference below grows from that.

## 2. Dimension-by-dimension

Legend: **✓** present · **△** present but differently shaped · **✗** absent · **—** not a concern of that RTOS

| # | Dimension | FreeRTOS | RT-Thread | Zephyr | Ark RTOS |
|---|---|---|---|---|---|
| 1 | Priority preemptive scheduling | ✓ | ✓ | ✓ | **✓** (ready set is a 256-bit two-level bitmap + per-priority lists, O(1)) |
| 2 | Round-robin time slicing | ✓ (optional) | ✓ | ✓ | **✓** |
| 3 | Event / event group / semaphore / mutex | ✓ / ✓ (32-bit) / ✓ / ✓ (priority inheritance) | ✓ / ✓ (`rt_event`, single flag) / ✓ / ✓ | ✓ / ✓ (`k_event`, 32-bit) / ✓ / ✓ | **✓ / △ / ✓ / ✓** — `arkrtos_event` is a **single-flag** broadcast reset; the 32-bit event set `arkrtos_eset` is implemented but off by default (`ARKRTOS_USE_ESET`), with OR/AND mask waits plus optional CLEAR, and its **App-side SVC 0x21 channel, kernel-side ISR entry (`set` inside a real EXTI0 handler) and real SVC-in-ISR (EXTI0 lowered to IP `0x40`, then `SVC 0x21` in the handler) are all verified on F427** (`ESET_DEMO` 37 assertions, `eset_from_isr_contract` 61 assertions, 0 failures), see 同步原语与令牌守恒.md §5.4; the mutex has priority inheritance with **multi-level propagation** along the wait chain (§5.7); optional **recursive-lock** creation entry (`ARKRTOS_USE_MTX_RECURSIVE`, default off, §5.6); plus **multi-object readiness probing** (SVC 0x22, `ARKRTOS_USE_WAITMULTI`, non-destructive, §5.8) |
| 4 | Message queue | ✓ | ✓ | ✓ | **✓** (`arkrtos_mq`) |
| 5 | Software timers | ✓ | ✓ | ✓ | **✓** (`arkrtos_timer`) |
| 6 | Stream / message buffer | ✓ (V10) | ✓ (pipe/ringbuffer) | ✓ (k_pipe / k_fifo) | **✓** (`arkrtos_sbuf` byte stream + `arkrtos_mbuf` length-header messages; off by default, see 流缓冲与消息缓冲.md; trigger watermark not provided yet) |
| 7 | Task notifications | ✓ (lightweight sync, V8.2+) | — | — | **✗ positional gap** (event + semaphore already cover it; no plan to build a third overlapping primitive to save a few bytes) |
| 8 | **Fully static allocation** | ✓ (V9+, `configSUPPORT_STATIC_ALLOCATION`) | ✓ | ✓ | **✓ (more thorough: the kernel owns no heap, objects are all static; the heap exists only in the App-side static arena)** |
| 9 | Dynamic heap | heap_1…heap_5 | memheap / mempool / small-memory | k_heap / k_mem_slab | **△** no kernel heap; the App-side POSIX layer has a static arena |
| 10 | Fixed-block memory pool | ✓ (heap_4 coalescing + heap_5) | ✓ (mem pool) | ✓ (k_mem_slab) | **✓** `arkrtos_mempool` (kernel side + App/driver via SVC 0x20; storage supplied by the caller, zero extra RAM, see 固定块内存池.md) |
| 11 | Low power / tickless | ✓ (tickless idle) | ✓ | ✓ (device/runtime/system PM, most complete) | **✓** (`ARKRTOS_USE_TICKLESS`, default off; sleep length clamped by the delay chain and software timers, phase settlement prevents lost ticks, see 低功耗与tickless.md. No second timer/RTC, only merging inside the upper bound; F427 measured interrupts down to 1/3.6, 20 s with no lost tick, **power not measured**) |
| 12 | Device driver model | ✗ (demos only) | ✓ (`rt_device`) | ✓ (devicetree driver model) | **✓** (unified device framework + `arkrtos_dt` looking devices up by `compatible`) |
| 13 | Configuration system | `FreeRTOSConfig.h` | Kconfig + menuconfig | Kconfig (+ west) | **✓** (Kconfig subset, symbol name = macro name) |
| 14 | Filesystem / VFS | external (+FAT) | DFS (FAT/littlefs/…) | LittleFS/FAT (subsystems) | **✓** (littlefs built in + VFS + procfs) |
| 15 | Networking | external (+TCP) | lwIP / SAL / AT | multi-stack + BLE | **△** (lwIP integration ready, `ARKRTOS_USE_LWIP` default 0; POSIX socket/select compatibility layer verified on board) |
| 16 | Shell / logging | ✗ | ✓ (FinSH + ulog) | ✓ (shell + log subsystem) | **✓** (kernel shell console + levelled logging + ring log) |
| 17 | MPU / privilege isolation | ✓ (FreeRTOS-MPU variant) | ✓ | ✓ (ARM MPU/TrustZone) | **✓** (SVC privilege isolation + per-task MPU windows, App/driver as separate firmware) |
| 18 | Crash recovery / watchdog guard | ✗ | ✗ | △ (wire it yourself) | **✓** (cross-reset crash journal + consecutive-crash disable + per-task heartbeat contract `guard`) |
| 19 | Hardware safe state (drive hardware to safety after an App crash) | ✗ | ✗ | ✗ | **✓ (unique)** (`hwsafe` ledger + registerable channel + last will `set_will`) |
| 20 | **Runtime install / uninstall of apps and drivers** | ✗ | △ (`dlmodule` dynamic modules) | ✗ | **✓ (mainline capability)** (image pool + installer + version gating + seamless upgrade) |
| 21 | Running mini programs from the filesystem | ✗ | ✗ | ✗ | **✓ (unique)** (`arkrtos_mini`, no slot, multi-instance) |
| 22 | Device discovery / RPC / streams (softbus) | ✗ (TCP aside) | △ (packages) | △ (networking subsystem) | **✓** (ark_softbus component, MCU profile verified on board) |
| 23 | Unit test framework | ✗ | ✓ (utest + CI) | ✓ (ztest + Twister) | **✓** (on-board `utest`: automatic case registration + assertion accounting, `utest run` 12/12 with 190 assertions on board; `utest-host` gate verifies the framework's own accounting) |
| 24 | Supported architectures | very wide (40+) | wide | ARM/RISC-V/x86/ARC/… | Cortex-M3/M4/M7 (RISC-V / LoongArch abstraction layers ready, port implementations pending) |
| 25 | Primary toolchain | any (Keil/GCC/IAR) | SCons / MDK | CMake + west | **Keil MDK (AC5/AC6) + GNU Make / arm-none-eabi-gcc (F427 example, see §3-⑤ and GCC构建指南.md)** |
| 26 | **Trimmed-down floor (measured)** | about 6–10 KB Flash (minimal config, community figure) | kernel about 3 KB + components as needed | about 8–20 KB (depending on devicetree and subsystems) | **37.8 KB Flash / 28.3 KB RAM** (`config/minimal_noshell_defconfig`, measured on the F427 kernel project; includes board HAL 7.4 KB and demo app 1.1 KB, kernel proper RAM ≈ 18.9 KB). Default profile 234.6 KB / 118.9 KB. See 功能开关.md §"measured minimal profile" |

## 3. Gap list and disposition

| # | Gap | Tier | Disposition |
|---|---|---|---|
| ① | Stream / message buffer | **filled (except trigger watermark)** | `arkrtos_sbuf` (byte stream, send may partially succeed) + `arkrtos_mbuf` (4-byte length header, whole message atomic; a receive buffer that is too small drops the whole message rather than keeping a fragment). Blocking/timeout, ISR send, handle lifetime are the same set as mq. 11 host assertion groups + `ci_gate` gate pass; H743 GCC on-profile measured +7792 B FLASH / +1248 B RAM, while the **F427 demo project overflows when turned on** (only 4.8 KB left there). The blocking/wakeup path is **not verified on board**. See 流缓冲与消息缓冲.md |
| ② | Fixed-block memory pool | **filled** | `arkrtos_mempool`: fixed blocks, zero fragmentation, no heap dependency; error convention is "reject an illegal free without touching pool state"; shell `mpool` selftest 9/9 on board. **App/driver channel open** (SVC 0x20, pool memory checked at the boundary to lie in caller-visible RAM). 47 host assertions + `ci_gate` gate pass; real link +536 B FLASH (H743) / +404 B (F427, 98.32%) in the default profile. **SVC path verified on board (F427, 2026-11-13)**: `MPOOL_DEMO.svcapp` 33 cases all pass, block addresses inside the App's own RAM, six classes of illegal creation all rejected; shell `mpool test` 9/9. See 固定块内存池.md §7 |
| ③ | Low power / tickless | **filled (F427 measured)** | `ARKRTOS_USE_TICKLESS` (default off): at idle take the smaller of "delay chain next expiry" and "software timer next expiry", clamp to `ARKRTOS_TICKLESS_MAX_TICKS` (default 8 ticks), program SysTick to a long period, and settle skipped ticks on wake. Phase is remembered in software with the remainder carried into the next tick — after 40 consecutive non-integral early wakeups the accumulated tick count still equals "total count / per-tick count", and this is an equality in the host gate. Default profile zero size and behavior change (the tick-count branch is dead code). The `tickless` gate = 152 host assertions + a real armclang compile of the ON profile (tickless.c / task.c / main.c). F427 measured (2026-11-13): `rt` reports `isr=18605 sleeps=18682 skipped=48728 of 67333`, interrupts down to **27.6%** (about 1/3.6), and 20 s idle advanced 40422 ticks = 20.2 s (no lost tick); on-profile +648 B Code / +68 RO / +24 RW / +8 ZI. **Current not measured**. See 低功耗与tickless.md §8 |
| ④ | Unit test framework | **filled** | on-board `utest`: automatic case registration, unified assertion macros, on-board `utest run` summary (12 cases in the default profile, F427 on board 12/12 with 190 assertions); plus `utest-host` gate verifying the framework's accounting with host gcc. See 单元测试框架.md |
| ⑤ | GCC / Make example project | **filled** | the F427 kernel project ships a GNU Make build (`example/stm32f427/kernel/ARKRTOS_TEST/GCC`), with `sources.mk` generated from the uvprojx by `tools/gen_gcc_sources.py`; toolchain, usage and three pitfalls in GCC构建指南.md |
| ⑥ | Task notifications | **positional gap** | not done, for two structural reasons: ① **its semantics are a merger of three other primitives** — FreeRTOS's 32-bit notification doubles as binary/counting semaphore, event group and a length-1 queue, while in Ark these are separately `event` (single flag) and `eset` (32-bit bitset, optional switch, settable from ISR), `sem`, `mq`; adding another would be a fourth overlapping IPC; ② **it saves nothing**. Notifications are "light" on FreeRTOS because the waiter hangs directly off the task control block; Ark's user-mode wait is "caller registers → dispatcher peeks (only_kernel) → rendezvous dispatches" (the App side cannot touch kernel structures), so notifications would still take the same registration path, adding only a 32-bit per-task state word and another API, buying zero instructions |
| ⑦ | SMP / multicore | **skeleton only (not multicore support)** | scheduler, ready bitmap, delay queue and object tables remain **global single-core state**; enabling `ARKRTOS_USE_SMP` does not make two cores run tasks simultaneously. The skeleton only lays multicore groundwork: per-core state table (`online`/`started`/`current_task_id`/`tick`/`ipi_count`/`idle_count`), online mask, secondary entry `arkrtos_smp_secondary_main()` (offline cores just spin in WFI), IPI path (`arkrtos_smp_send_ipi()` + weak receiver), `ARKRTOS_CPU_NUM` 1..8. `started` (start request accepted) and `online` (core really running) are recorded separately; a single-core port's `arkrtos_port_cpu_start()` / `arkrtos_port_send_ipi()` **return -1 instead of pretending success**. Off by default with zero size and behavior change (`Code=220290` unchanged). **Verification tier = unverified (no multicore board)**: 44 host assertions + 5 files of ON-profile syntax, no board reading at all. Real SMP needs per-CPU ready queues and per-core MPU contexts — a different job, not done. See 同步原语与令牌守恒.md §5.10 |
| ⑨ | Sync primitive details (recursive lock / multi-level PI / multi-object probe / waiter capacity) | **filled (all but waiter capacity verified on board)** | recursive mutex `arkrtos_mtx_create_recursive()` (SVC 0x15 sub 17, `nest` is `uint8`, wraps beyond 255); priority inheritance propagates multi-level along the wait chain (`A(3)→B(5)→C(7)`, 5 chain assertions on host); multi-object readiness probe `arkrtos_wm_probe_internal()` (SVC 0x22, non-destructive, returns a 32-bit mask; a timed wait is **1 ms polling** and the "probed → taken by someone else before use" window is irreducible); waiter capacity `ARKRTOS_WQ_SLOTS` can be raised from 4 to the task-table capacity (`ARKRTOS_USE_WAIT_UNBOUNDED`, **not a linked-list unbounded mode**; truly decoupling it would require changing 46 places). All four are default-off with zero change; host gates 24 / 5 / 76 assertions. **Recursive lock, multi-object probe and multi-level PI are all verified on board**: recursive lock and probe in the 2026-11-14 F427 `utest` (switch-on profile 11/11, 110 assertions), multi-level PI in the 2026-11-15 F427 case `mtx_pi_chain_contract` (real tasks, three-level chain, console effective priority raised to the HIGH band = two hops propagated, 20 assertions 0 failures; changing the propagation condition to base priority only makes the same case report `got 10 / want 6`, so the negative control holds). Waiter capacity still has only host/compile evidence; the on-profile F427 test image does not fit (`L6406E`, 108 errors). See 同步原语与令牌守恒.md §5.6–§5.9 |
| ⑩ | POSIX rwlock / barrier | **filled (verified on board)** | user-mode composition (kernel mutex + condition variable reused), zero SVC changes; writer priority (reader priority would starve writers, so `PTHREAD_PREFER_READER_NP` returns `ENOTSUP` rather than accepting and silently changing semantics); the barrier uses `gen` to support reuse. The cost is consuming `ARKRTOS_MTX_NUM`/`ARKRTOS_COND_NUM` slots; a full table returns `ENOMEM`. **Verified on board** (2026-11-14, F427, `POSIX_DEMO` selftest `rwlock : PASS (0 violations)` / `barrier : PASS (arrived=2 serial=1 bad=0)`); covers the single-task lock state machine and a 3-participant barrier; overlapping multi-reader and writer starvation are not measured. That image declares 16 KB RAM (RW/ZI 4348 + stack 4096 + heap 2048 = 10492), so the fixed slot window must be ≥ 16 KB to hold it. See POSIX与Windows兼容层说明.md |
| ⑧ | Bluetooth / full network stack | **positional gap** | no first-party implementation; there are already an lwIP integration point and a socket/select compatibility layer, attach externally when needed |

> "Positional gap" does not mean "impossible", it means **doing it would drift off the mainline and overlap existing capability**, so it is explicitly not done.

## 4. References

* FreeRTOS official documentation: <https://www.freertos.org/Documentation>
  (V9 introduced `configSUPPORT_STATIC_ALLOCATION`; V10 introduced stream buffer / message buffer; tickless idle is in the kernel configuration chapter; this repository's counterpart is in 低功耗与tickless.md)
* RT-Thread documentation centre: <https://www.rt-thread.io/document/>
  (utest framework, FinSH/msh, device framework, packages)
* Zephyr Project: <https://docs.zephyrproject.org/>
  (devicetree, Kconfig, power management, ztest/Twister, number of supported boards)

> If the table above disagrees with upstream documentation, upstream wins; when you find a discrepancy, come back and fix this page.
