# 低功耗与 tickless idle

> 一句话：`ARKRTOS_USE_TICKLESS=1` 之后，空闲循环不再一拍一次 `WFI`，而是先算
> "下一次真正要干活还有几拍"，把 SysTick 重新编程成一段更长的周期，睡完一次、
> 醒来把跳过的节拍一次补齐。
>
> **验证档位：宿主算账 + ON 档语法编译已过；未上板验证。** 真硬件的
> COUNTFLAG/VAL 读序、SysTick 中断优先级、WFI 的真实功耗收益都还没有板端读数。

## 1. 它解决什么

没有 tickless 时，即使系统里一个任务都没在跑，SysTick 也按 `ARKRTOS_TICK_PERIOD_US`
（默认 500 µs，2 kHz）一直进中断，MCU 每 500 µs 被叫醒一次。这就是"空闲也要耗电"
的主要来源，也是把 tick 频率提到 2 kHz 之后最实际的代价。

开了 tickless 之后，空闲时只要"下一个事件"还在若干拍之后，就一次性睡那么久，
期间 CPU 不进中断。

## 2. 开关与参数

| 符号 | 默认 | 含义 |
|---|---|---|
| `ARKRTOS_USE_TICKLESS` | `n`（关） | 打开 tickless idle。**默认关时体积与行为零变化**：整套代码在 `#if` 里，空闲循环走原来的 `ARKRTOS_WFI()` |
| `ARKRTOS_TICKLESS_MAX_TICKS` | `8` | 一次最多睡几拍（8 拍 @2 kHz = 4 ms）。上限越小，补齐的拍数越少、唤醒延迟越可控；越大越省电 |

两者都在 `config/kconfig/Kconfig.features` 里，由 `config/arkrtos_defconfig` 决定取值，
生成到 `kernelsrc/include/arkrtos_autoconf.h`，`arkrtos_features.h` 提供 `#ifndef` 兜底。

## 3. 怎么算"能睡多久"

只看两个 tick 驱动的唤醒源，因为空闲时没有任务在跑，不会有别人来 post 信号量：

* **延时链** `arkrtos_task_next_delay_ticks()`：链按 `delay_tick` 有序，只看链头；
  已经到期（`<= 0`，通常是唤醒路径留下的僵尸节点）按 1 拍报。
* **软定时器** `arkrtos_timer_next_ticks()`：扫所有 `used && active` 的定时器，
  取 `remain_ticks` 最小。

取两者较小值，再夹到 `[1, ARKRTOS_TICKLESS_MAX_TICKS]`。结果是 1 就不跳，
老老实实 `WFI` 到下一拍——跳一拍省不下任何一次中断。

有别的中断在睡梦中把 CPU 叫醒，属于正常情况：按"提前唤醒"结账，不走特例路径。

## 4. 相位：为什么不会越睡越慢

这是这个实现最容易写错的一处，写在这里当验收口径。

被提前叫醒时，走过的计数往往不是整拍（比如走了 1500 个计数，而一拍是 1000 个）。
如果把余数直接丢掉，每次空闲都会少算最多一拍——单次看不出来，跑一天就是分钟级的慢。

所以结算始终按**软件记住的相位**做：

```text
total = phase + elapsed          /* phase = 已经朝下一拍走过的计数，< cpt */
ticks = total / cpt              /* 这一次该补几拍 */
phase = total - ticks * cpt      /* 剩下的留给下一拍 */
```

相位不为 0 时，硬件被编程成"补满这一拍"的**短周期**（`cpt - phase`），
并在那一次中断里自己恢复成整拍（内部标记 `s_tl_short`）。短周期如果不被收回，
之后每一拍都会跟着变短，节拍就永久性地偏快了。

对应到代码：`arkrtos_tickless_split()` 是纯函数（整拍 + 余数），
`arkrtos_tl_settle()` 是带硬件读数的版本，两者共用同一份折算。

## 5. 补齐：为什么在中断里补，为什么走 tick 体

睡满一条长节拍时，SysTick 中断进来，`arkrtos_tickless_wake_ticks()` 结账出
"这次一共 n 拍"，`arkrtos_kernel_tick_handler()` 立刻把这 n 拍补齐。

* **必须在中断里补**：中断一退出就可能切到别的任务，那时再补就会把"过去"算到"现在"上。
* **必须走节拍体**（`arkrtos_kernel_tick_ticks()`），不能循环调 `arkrtos_kernel_tick_handler()`：
  后者会重新结账，把刚摆好的短周期当成"已经到点"，凭空多补一拍。这是实现时踩到的坑，
  也是 `arkrtos_kernel_tick_ticks()` 存在的理由。

补齐的拍数被 `ARKRTOS_TICKLESS_MAX_TICKS` 夹住，所以最坏情况是可预期的。

## 6. 接进哪些地方

| 位置 | 改动 |
|---|---|
| `kernelsrc/include/arkrtos_hal.h` | 新增 `arkrtos_port_start_timer_counts(counts)`、`arkrtos_port_timer_countflag_take()` |
| `kernelsrc/port/arm/cortex-m{3,4,7}/arkrtos_port.c` | 两个实现的移植层落地：编程值夹 `1..0x00FFFFFF`；COUNTFLAG 读一次即清 |
| `kernelsrc/src/arkrtos_tickless.c/.h` | plan / split / idle / wake_ticks 与相位结算 |
| `kernelsrc/src/arkrtos_task.c` | tick handler 拆成"包装（结账）+ `arkrtos_kernel_tick_body()`"；新增 `arkrtos_kernel_tick_ticks()`、`arkrtos_task_next_delay_ticks()` |
| `kernelsrc/src/arkrtos_timer.c` | 新增 `arkrtos_timer_next_ticks()` |
| 三个例程的 idle 循环（F401 / F427 / H743 `main.c`） | `ARKRTOS_WFI()` → `#if TICKLESS arkrtos_tickless_idle() #else ARKRTOS_WFI() #endif` |
| 构建 | `arkrtos_tickless.c` 进 F401/F427 的 uvprojx 与 H743 的 `sources.mk` |

关闭开关时，`arkrtos_kernel_tick_handler()` 就是原来那个函数本身
（`#if` 结构是"包装 + else 原名"），没有多一层调用。

## 7. 边界：它**不**处理什么

* **不做无限长睡**。那需要第二个定时器或 RTC 做唤醒源，还需要知道"下一个外部事件"
  在哪——本仓库当前没有这个信息源，也没有板子可以验证。所以只做"按最近事件夹住、
  上限 N 拍"。
* **不改变唤醒语义**。跳过的节拍是"一次性补发"，被延时的任务在补发那一下醒来，
  延时时长仍然按拍计，不按真实微秒重算。
* **不覆盖非 tick 唤醒源的省电**。外设中断来了就醒，醒来照常按提前唤醒结账。
* **只认单次回卷**。上限（默认 8 拍）远小于 24 位计数器能表达的跨度，加上 tick 中断
  本身就是唤醒源，醒来必然在回卷后不久。

## 8. 验证

```bash
python tools/tickless_host_gate.py     # 已并入 tools/ci_gate.py 的 tickless 一项
```

两段，缺一不可：

1. **宿主算账**（`kernelsrc/tests/test_tickless_host.c` + 真实 `arkrtos_tickless.c`）
   * `plan` / `split` 的边界：无事件、上限、`cap < 2`、计数为 0、空指针；
   * 睡满一条长节拍：报告的拍数、补发的节拍体次数、醒来后硬件回到整拍；
   * 连续 40 次"非整拍提前唤醒"之后 **累计补发拍数 == 总计数 / 每拍计数**——
     这就是"跑一天不会慢"的机器可验形式。
2. **ON 档真编译**：`ARKRTOS_USE_TICKLESS` 默认是 0，普通构建里这些代码全是死代码，
   语法错误不会被发现（闸门假绿的典型形态）。所以用 `armclang -fsyntax-only`
   加 `-DARKRTOS_USE_TICKLESS=1`，把三个真正的接入点都按下去编一遍：
   `arkrtos_tickless.c`、`arkrtos_task.c`、F427 例程的 `main.c`。

**未覆盖**：真板上的 SysTick 行为与功耗。宿主 152 条断言全过、ON 档 0 error，
都不等于板端已验证——那需要一台能测电流的板子。

## 9. 与主流 RTOS 的对应

FreeRTOS 的 tickless idle（`configUSE_TICKLESS_IDLE` + `eTaskConfirmSleepModeStatus`）、
RT-Thread 的 tickless、Zephyr 的电源管理做的都是同一件事：把"空闲时的节拍"变成
"下一次事件之前的一段睡眠"。差别在边界：

| | 唤醒源 | 睡眠上限 | 补齐方式 |
|---|---|---|---|
| 本实现 | 延时链 + 软定时器 | `ARKRTOS_TICKLESS_MAX_TICKS`（默认 8 拍） | 中断里一次补发，走节拍体 |
| FreeRTOS | 同上（可扩展） | 由调用者 `configEXPECTED_IDLE_TIME_BEFORE_SLEEP` 与定时器宽度决定 | 累计进 `xPendedTicks`，按拍补 |
| Zephyr | 全套 PM 框架（device/runtime/system） | 由 PM policy 决定 | 由 PM 子系统接管 |

本实现刻意只做最小闭环：不引入第二个定时器、不接管外设电源、不改唤醒语义。
