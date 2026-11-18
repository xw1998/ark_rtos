# Low power and tickless idle

**[简体中文](低功耗与tickless.md) | English**

> Condensed English edition. The Chinese page (低功耗与tickless.md) is normative: every number below is
> copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no
> new claim and no new measurement.

> In one line: with `ARKRTOS_USE_TICKLESS=1` the idle loop no longer does `WFI` once per tick. It first
> computes how many ticks remain before the next real work, reprograms SysTick to that longer period,
> sleeps once, and catches up the skipped ticks on wake.
>
> **Verification tier: host accounting + ON-path syntax compile + F427 on-board measurement
> (2026-11-13) all passed.** The board readings (§8) prove three things: long sleeps really happen,
> SysTick interrupts are really cut to about 1/3.6, and time is not lost (20 s of wall clock against
> 40422 ticks = 20.2 s). **What was not measured is current**: there is no ammeter on this device, so
> what is saved is "number of interrupts", not "watts".

## 1. What it solves

Without tickless, even with no task running, SysTick interrupts every `ARKRTOS_TICK_PERIOD_US`
(default 500 µs, 2 kHz), so the MCU is woken every 500 µs. That is the main source of "idle still
burns power" and the most tangible price of raising the tick rate to 2 kHz.

With tickless on, when idle and the next event is several ticks away, it sleeps that long in one go
and the CPU takes no interrupt in between.

## 2. Switches and parameters

| Symbol | Default | Meaning |
|---|---|---|
| `ARKRTOS_USE_TICKLESS` | `n` (off) | Enable tickless idle. **With the default off, size and behaviour are unchanged**: the whole implementation sits inside `#if`, and the idle loop takes the original `ARKRTOS_WFI()` |
| `ARKRTOS_TICKLESS_MAX_TICKS` | `8` | Maximum ticks per sleep (8 ticks @2 kHz = 4 ms). A smaller cap means fewer catch-up ticks and a more controllable wake latency; a larger one saves more power |

Both live in `config/kconfig/Kconfig.features`, are decided by `config/arkrtos_defconfig`, generated
into `kernelsrc/include/arkrtos_autoconf.h`, with `#ifndef` fallbacks in `arkrtos_features.h`.

## 3. How "how long can we sleep" is computed

Only the two tick-driven wake sources are considered, because while idle no task is running and
nobody else will post a semaphore:

* **delay chain** `arkrtos_task_next_delay_ticks()`: the chain is ordered by `delay_tick`, only the
  head is inspected; an already-expired node (`<= 0`, usually a zombie left by the wake path) reports
  1 tick.
* **software timers** `arkrtos_timer_next_ticks()`: scan every `used && active` timer and take the
  smallest `remain_ticks`.

Take the smaller of the two and clamp to `[1, ARKRTOS_TICKLESS_MAX_TICKS]`. A result of 1 means no
skip: just `WFI` to the next tick — skipping one tick saves no interrupt at all.

Another interrupt waking the CPU during a sleep is normal: it is settled as an "early wake", no
special path.

## 4. Phase: why it does not drift slower and slower

This is the easiest place in such an implementation to get wrong, so it is written down as an
acceptance basis.

When woken early, the elapsed count is usually not a whole tick (say 1500 counts elapsed while one
tick is 1000). Dropping the remainder loses up to one tick per idle period — invisible once, minutes
of drift in a day.

So settlement always uses the **phase remembered in software**:

```text
total = phase + elapsed          /* phase = counts already walked toward the next tick, < cpt */
ticks = total / cpt              /* how many ticks to catch up this time */
phase = total - ticks * cpt      /* the rest is carried to the next tick */
```

When the phase is non-zero the hardware is programmed with a **short period** (`cpt - phase`) that
fills the current tick, and that interrupt restores the full period itself (internal flag
`s_tl_short`). If the short period were not taken back, every later tick would be short too and the
tick would be permanently fast.

In code: `arkrtos_tickless_split()` is a pure function (whole ticks + remainder), and
`arkrtos_tl_settle()` is the variant with hardware readings; both share one conversion.

## 5. Catch-up: why inside the interrupt, and why through the tick body

After a full long tick, the SysTick interrupt runs, `arkrtos_tickless_wake_ticks()` settles "n ticks
this time", and `arkrtos_kernel_tick_handler()` catches those n ticks up immediately.

* **It must be inside the interrupt**: as soon as the interrupt exits, another task may be switched
  in, and catching up then would attribute "the past" to "the present".
* **It must go through the tick body** (`arkrtos_kernel_tick_ticks()`), not loop calling
  `arkrtos_kernel_tick_handler()`: the latter would settle again, treat the freshly armed short period
  as "already expired", and invent an extra tick. This is a trap hit during implementation and the
  reason `arkrtos_kernel_tick_ticks()` exists.

The catch-up count is clamped by `ARKRTOS_TICKLESS_MAX_TICKS`, so the worst case is predictable.

## 6. Where it plugs in

| Location | Change |
|---|---|
| `kernelsrc/include/arkrtos_hal.h` | New `arkrtos_port_start_timer_counts(counts)`, `arkrtos_port_timer_countflag_take()` |
| `kernelsrc/port/arm/cortex-m{3,4,7}/arkrtos_port.c` | Both port implementations: programmed value clamped to `1..0x00FFFFFF`; COUNTFLAG cleared by reading once |
| `kernelsrc/src/arkrtos_tickless.c/.h` | plan / split / idle / wake_ticks and phase settlement |
| `kernelsrc/src/arkrtos_task.c` | Tick handler split into "wrapper (settle) + `arkrtos_kernel_tick_body()`"; new `arkrtos_kernel_tick_ticks()`, `arkrtos_task_next_delay_ticks()` |
| `kernelsrc/src/arkrtos_timer.c` | New `arkrtos_timer_next_ticks()` |
| Idle loop of the three examples (F401 / F427 / H743 `main.c`) | `ARKRTOS_WFI()` → `#if TICKLESS arkrtos_tickless_idle() #else ARKRTOS_WFI() #endif` |
| Build | `arkrtos_tickless.c` added to the F401/F427 uvprojx and to H743 `sources.mk` |

With the switch off, `arkrtos_kernel_tick_handler()` is the original function itself (the `#if`
structure is "wrapper + else original name"), no extra call layer.

## 7. Boundaries: what it does **not** do

* **No infinite sleep.** That needs a second timer or an RTC as wake source and knowledge of "the next
  external event" — this repo has no such information source and no board to verify it on. So it only
  does "clamp to the nearest event, cap at N ticks".
* **No change in wake semantics.** Skipped ticks are delivered in one burst; a delayed task wakes on
  that burst and its delay is still counted in ticks, never recomputed in real microseconds.
* **No power saving for non-tick wake sources.** A peripheral interrupt wakes it and is settled as an
  early wake as usual.
* **Only single wrap-around is assumed.** The cap (default 8 ticks) is far below what a 24-bit counter
  can express, and the tick interrupt is itself a wake source, so a wake necessarily happens shortly
  after the wrap.

## 8. Verification

```bash
python tools/tickless_host_gate.py     # folded into the tickless check of tools/ci_gate.py
```

Two parts, both required:

1. **host accounting** (`kernelsrc/tests/test_tickless_host.c` + the real `arkrtos_tickless.c`)
   * `plan` / `split` boundaries: no event, the cap, `cap < 2`, zero counts, null pointers;
   * a full long tick: reported tick count, number of catch-up tick-body calls, hardware back on the
     full period after wake;
   * after 40 consecutive "early wakes on a partial tick", **cumulative catch-up ticks == total
     counts / counts per tick** — that is the machine-checkable form of "it will not drift in a day".
2. **real ON-path compile**: `ARKRTOS_USE_TICKLESS` defaults to 0, so in an ordinary build all this
   code is dead and syntax errors would never surface (the classic false-green gate). So
   `armclang -fsyntax-only` is run with `-DARKRTOS_USE_TICKLESS=1` over all three real insertion
   points: `arkrtos_tickless.c`, `arkrtos_task.c`, and the F427 example's `main.c`.

3. **on-board measurement** (F427 + DAPLink, 2026-11-13, `ARKRTOS_USE_TICKLESS=1`, see below)

**Not covered**: current / power in watts. There is no ammeter on this device; all that can be
measured is "how many interrupts were avoided".

### 8.1 On-board measurement (2026-11-13)

Environment: STM32F427 (`pyocd -t stm32f427zi`), serial COM21, the macro added only in the Keil
project (the source headers still default to 0). To make "the switch took effect" visible on the
serial port, `rt show` prints one extra cumulative line (`arkrtos_tickless_stats()`, inside
`#if TICKLESS`; not compiled in the default profile):

```text
tickless  isr=18605  sleeps=18682  skipped=48728 of 67333 tick(s)  cap=8
```

The four numbers are: SysTick interrupt count, count of times it really programmed the long period and
entered WFI, ticks delivered through the catch-up path, and the cumulative tick count
(`arkrtos_kernel_get_tick()`). `skipped` is not counted separately; it is `total ticks - isr` (every
interrupt contributes at least one tick, the rest must be catch-up), so `isr + skipped == total ticks`
is a definition rather than a measurement; what carries information is the ratio.

| Acceptance item | Result | Reading |
|---|---|---|
| Long sleeps really happen (the switch is not inert) | Pass | `sleeps=18682`, almost equal to `isr=18605`: every interrupt is backed by a real reprogram + WFI |
| SysTick interrupts really cut down | Pass | `isr 18605 / 67333 ticks = 27.6%`, i.e. about **1/3.6** of the interrupts; the `tick.isr` line of `rt` simultaneously reports `N=67331` (≈ total ticks), the two sources agree |
| Time not lost (phase settlement) | Pass | 20 s idle: ticks go from 26920 to 67333, i.e. 40422 ticks = 20.2 s (the extra 0.2 s is the serial round trip of the two commands) |
| With the switch off, not one line is printed | Pass | default profile `rt show` has no `tickless` line, size back to 219914 B (§8.2) |
| Current / power down | **Not measured** | No ammeter; "3.6× fewer interrupts" cannot be converted to watts |

**One basis reminder**: the `tick.isr` line of `rt` counts tick-body calls (including catch-up), not
interrupts; to know whether interrupts really dropped, look at the `tickless isr=` column.

### 8.2 Size (F427 `ARKRTOS_TEST` / Keil AC6, same project, only this macro changed)

| | Code | RO-data | RW-data | ZI-data |
|---|---|---|---|---|
| default (off) | 219914 | 19198 | 896 | 120852 |
| on | 220562 | 19266 | 920 | 120860 |
| delta | **+648** | **+68** | **+24** | **+8** |

The on-profile size includes the `rt` reading line and everything in `arkrtos_tickless.c`; the
default profile contains not a single byte. Compiles `0 Error(s), 0 Warning(s)` (incremental) and
`0 Error(s), 4 Warning(s)` (full; all 4 in the untouched vendored `arkrtos_vfs_procfs.c`).

## 9. Correspondence with mainstream RTOSes

FreeRTOS tickless idle (`configUSE_TICKLESS_IDLE` + `eTaskConfirmSleepModeStatus`), RT-Thread's
tickless and Zephyr's power management all do the same thing: turn "ticks while idle" into "one sleep
until the next event". The difference is in the boundary:

| | Wake sources | Sleep cap | Catch-up |
|---|---|---|---|
| this implementation | delay chain + software timers | `ARKRTOS_TICKLESS_MAX_TICKS` (default 8 ticks) | one burst inside the interrupt, through the tick body |
| FreeRTOS | same (extensible) | decided by the caller's `configEXPECTED_IDLE_TIME_BEFORE_SLEEP` and timer width | accumulated in `xPendedTicks`, replayed per tick |
| Zephyr | the whole PM framework (device/runtime/system) | decided by PM policy | taken over by the PM subsystem |

This implementation deliberately does only the minimal closed loop: no second timer, no peripheral
power management, no change in wake semantics.
