# Hardware Safety State on App Crash (the "last will" mechanism)

> Condensed English edition. The Chinese page (App崩溃硬件安全与遗言机制.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

This document answers one question: **when an App that drives hardware (motors, machine tools, robot arms, relays …) crashes, who guarantees the hardware does not keep running on the last command it received?**

MPU / privilege isolation solves "an App accessing someone else's memory" (see 内存保护与App越权验证.md). It does **not** solve "hardware the App legitimately touched still sits at its last setting after the App dies". A spinning motor does not stop because the App controlling it faulted — unless someone stops it **after the App is dead**. This mechanism is that someone: **the kernel**, not the already-crashed App.

## 1. Design premises (boundaries first)

1. **The actor must be the kernel.** A crashed App cannot run a line of code; the fault-recovery path may even run with interrupts masked. So the neutralization is performed by the **kernel** during task reaping, and the code it calls can only live in the **resident image** (a driver's `ctrl`, or a safety callback registered by a driver/App). The App's own thread code is **not** usable after a crash.
2. **Do not pretend to be safe.** When nothing ever declared a safe state for a device, the kernel can only drop it from the ledger and **count it** (`hwsafe` shows `dropped without callback`); it never writes "neutralized" in the log.
3. **Off by default.** `ARKRTOS_HW_SAFE` (`kernelsrc/include/arkrtos_features.h`) defaults to `0`; when off the module **is not in the binary at all**, and device `open/close` plus the task-termination path are **exactly as before** — zero size and zero behavior change.
4. **Complementary, not a replacement.** Three different lines of defense from MPU and the watchdog guard (`ARKRTOS_USE_KERNEL_GUARD`):
   * MPU: blocks unauthorized access;
   * watchdog guard: blocks "the task stopped feeding the dog" (whole-machine level);
   * this mechanism: blocks "the owner died, the hardware is still running" (device level, deterministic neutralization).

## 2. Three roles and two channels

### Roles

| Role | Who | What |
|---|---|---|
| Register a safety callback | **driver** (resident) | `arkrtos_hwsafe_register(dev_name, neutralizer)` declares what this device's safe state is |
| Declare a "last will" | **App** (the crashable side) | `arkrtos_dev_set_will(dev_name, ctrl_cmd, arg)` declares "after I die, run this ctrl on that device" |
| Keep the ownership ledger | **kernel** | on device open, record "device name + owner task id"; on task death, neutralize accordingly |

Where the name comes from: while **still alive**, the App declares with "open the device" that "it is mine while I live, take it back when I die", and adds one sentence "how to take it back" — that is its last will. The kernel honors that will, not a dead thread that could run one more snippet.

### Two channels

**Channel A — driver-registered callback (device level, generic)**

```c
/* driver side (user-mode driver uses SVC 0x14 sub 4; kernel-mode driver links directly) */
int32 my_motor_safe(arkrtos_dev_hdr_t *obj, uint32 owner)
{
    my_motor_t *m = (my_motor_t *)obj;
    m->pwm = 0;                 /* non-blocking, idempotent, register/GPIO writes only */
    HAL_GPIO_WritePin(...);
    return 0;
}
arkrtos_drv_hwsafe_register("motor0", my_motor_safe);
```

Callback contract (**mandatory**, see the `arkrtos_hwsafe_fn` comment in `arkrtos_hwsafe.h`): it runs on the task-exit / fault-recovery path where interrupts may be masked; therefore it must be **non-blocking** (bounded register/GPIO writes; no waiting, no delay, no SVC, no filesystem or Flash) and should be **idempotent**.

**Channel B — App-declared last will (owner level, no callback needed)**

```c
/* App side: the device must be opened first (the kernel uses this to confirm you really own it) */
h = arkrtos_dev_open("motor0", 0);
...
arkrtos_dev_set_will("motor0", MOTOR_CTRL_STOP, 0);   /* stop the motor when I die */
```

On the owner's death the kernel runs `drv->drv_ctrl(obj, cmd, val)` on that device. For devices without a `ctrl` entry this channel does nothing (the kernel returns an error rather than pretending success).

### Priority

When both exist for the same device, **the owner's will runs first (channel B); only without a will is the driver callback used (channel A)**. With neither, the kernel just drops the ledger entry and counts it:

```
owner died → device has a will? → drv_ctrl(obj, will_cmd, will_val)
                              └no→ device has a registered callback? → fn(obj, owner)
                                                          └no→ drop entry + count (do not pretend safety)
```

Reason: the will is the **concrete closing action for this device this time**, more precise than the device's default safe state; the driver callback is the factory-defined fallback. Both are **kernel-verifiable resident code**.

### Death reason: neutralization vs planned handover

The same reaping path is reached by two classes of death: **unplanned** (crash, forced kill, fault recovery) and **planned handover** (during seamless upgrade the old mini-program hands the control loop to the next version and returns normally). The former must bring hardware back to a safe state; the latter is the opposite — **someone takes over immediately**, and forcing the device to its safe state at that instant is jitter, not safety.

So the reaping entry carries the death reason:

| Entry | Death reason | Behavior |
|---|---|---|
| `arkrtos_task_release_resources(tid)` | unplanned (reason=0) | **exactly as before**: will / callback / count only |
| `arkrtos_task_release_resources_handover(tid)` | planned handover (reason=1) | **drop the ledger entry only**; run neither will nor callback; counted separately |

The handover path is **not counted in `neutralized`** (it neutralized nothing) but in a separate `handover` counter.

**What counts as planned handover**: only "the **mini-program main task exits normally**" (`ARKRTOS_MINI_EXIT_IS_HANDOVER`, default 1) — that is exactly the seamless-upgrade contract (the old version writes out its state and returns). A crash goes through the loader's `halt_image` path, and a forced kill goes through the non-mini branch of `arkrtos_task_kill_internal`; neither is a handover and both fully neutralize.

> Setting the switch to 0 makes normal mini-program exit return to the safe state too; the cost is one hardware jitter during a seamless upgrade.

## 3. Implementation (kernel side)

| File | Content |
|---|---|
| `kernelsrc/include/arkrtos_hwsafe.h` / `src/arkrtos_hwsafe.c` | callback table + ownership ledger + two-phase neutralization |
| `kernelsrc/src/arkrtos_dev.c` | `arkrtos_hwsafe_claim()` after a successful open; `release()` on close; unregister the device's callback on `unregister`; provides `arkrtos_dev_ctrl_obj()` for the will |
| `kernelsrc/src/arkrtos_task.c` | `arkrtos_task_release_resources{,_handover}()` share one core and pass the death reason into `arkrtos_hwsafe_on_death_reason()`; two phases: mark inside the critical section, run the real callbacks outside it via `arkrtos_hwsafe_run_pending()` |
| `kernelsrc/include/arkrtos_mini.h` / `src/arkrtos_mini.c` | `arkrtos_mini_task_is_main()` decides whether a task is "the mini-program main task currently running", so the exit path can choose handover |
| `board/stm32f427/` | `drvled.c` provides `led_drv_safe()`; `arkrtos_board.c` registers led0/led1/led2 |
| `kernelsrc/shell/arkrtos_shell.c` | `hwsafe` command: ledger, registered devices, cumulative neutralizations, last neutralized task id |

**Why two phases?** `arkrtos_task_release_resources()` is also called from the **fault path**, where interrupts are masked. Running a driver callback inside the critical section would deadlock if the callback waits or delays at all. So the critical section only marks "pending neutralization" (pure memory writes), and the callbacks run after it (interrupts enabled).

**Capacity**: `ARKRTOS_HWSAFE_MAX_DEV` (callback table) and `ARKRTOS_HWSAFE_MAX_OWN` (ledger) both default to 8, fixed at compile time. When a table is full the kernel **drops rather than overwrites someone else** — overwriting would lose another device's safe state; configure the tables large enough.

## 4. Public interfaces

### Kernel (`arkrtos_hwsafe.h`, kernel/driver internal only)

| Function | Note |
|---|---|
| `arkrtos_hwsafe_reset()` | module init, clears both tables |
| `arkrtos_hwsafe_register(name, fn)` | register a device safety callback; a duplicate name is an **error** (two safe states is a config bug) |
| `arkrtos_hwsafe_unregister(name)` | unregister |
| `arkrtos_hwsafe_claim/release(name, owner, obj)` | ledger add/remove (called by the device framework) |
| `arkrtos_hwsafe_set_will/clear_will(name, owner, cmd, val)` | owner's will |
| `arkrtos_hwsafe_on_death(task_id)` | **phase 1** of two-phase neutralization (unplanned death; equals `reason(tid, 0)`) |
| `arkrtos_hwsafe_on_death_reason(task_id, handover)` | phase 1 with death reason (`handover!=0` → planned handover) |
| `arkrtos_hwsafe_run_pending()` | **phase 2** (runs outside the critical section; handover entries only drop the ledger) |
| `arkrtos_hwsafe_owner_count/dev_count/neutralized_count/last_owner/registered/info/will` | observation |

### Driver SDK (`arkrtos_driver_sdk.h`)

```c
typedef int32 (*arkrtos_hwsafe_fn)(void *obj, uint32 owner);
int32 arkrtos_drv_hwsafe_register(const char *dev_name, arkrtos_hwsafe_fn fn);
```

### App SDK (`arkrtos.h`)

```c
int32 arkrtos_dev_set_will(const char *dev_name, int32 cmd, int32 val);
```

### SVC surface

| SVC | sub | Function | Args |
|---|---|---|---|
| 0x14 `DRV_MGR` | 4 | register a device safety callback | `p[1]=name`, `p[2]=fn` (must fall inside the caller's own image window) |
| 0x14 `DRV_MGR` | 5 | query whether a callback is registered | `p[1]=name` → `1/0` |
| 0x14 `DRV_MGR` | 6 | unregister a device safety callback | `p[1]=name` |
| 0x10 `DEV_IO` | 6 | declare the current task's will for a device | `p[1]=name`, `p[2]=cmd`, `p[3]=val` |

**Security checks**: a registered callback pointer must fall inside the **caller's own image window** (`arkrtos_kernel_cb_own_region_ok`), the same constraint as driver callback pointers — **you cannot borrow someone else's privilege**. A will may only be declared for a device **the current task has already opened**, so one App cannot leave a will for another's device.

## 5. Shell observation

```
ark> hwsafe
hwsafe: enabled, devs=3 owners=6 neutralized=6 (will=3 cb=0 none=3 fail=0 handover=0) last_owner=12
  dev0: led0  (callback registered)
  dev1: led1  (callback registered)
  dev2: led2  (callback registered)
  own0: dev=led0 owner=5 dead=0 will=none
```

* `neutralized` increments after a **non-handover** owner death (the `will` / `cb` / `none` channels all increment it); handover entries are **not** counted here.
* `(will= cb= none= fail= handover=)` are per-channel counters: `will` = will executed, `cb` = driver callback executed, `none` = neither (entry dropped only), **`fail` = executed but the driver returned non-zero**, i.e. "we tried but the device was **not** brought back to a safe state", `handover` = planned handover skipped (ledger dropped, hardware untouched).
* **Counters follow the driver's return value**: `will`/`cb` only when the driver returns 0, otherwise `fail`. "Tried" is never written as "neutralized".
* Whether a device really has a callback is a separate question from counting: look for `callback registered` on the device line, so "no callback" is not read as "neutralized".

## 6. On-board verification

`example/stm32f427/app_sdk/HWSAFE_DEMO/`:

1. the App opens `led0`, turns it on, and declares a will (`ARKRTOS_DEV_LED_CTRL_OFF`);
2. the App deliberately performs an unauthorized access → stopped by the MPU → fault recovery kills the task;
3. the kernel runs the will while reaping → the lit state is forced off;
4. `hwsafe` in the shell shows `neutralized=1`, `last_owner=<that App's task id>`; `ls /dev` and the LED visually confirm it is off.

**Measured acceptance** (commit `c021168`, reproduced twice on independent fresh installs): `hwsafe` reports `neutralized=6 last_owner=12`, `fault` shows `MEMFAULT` → `RECOVER`×3 → `TASKKILL` → `APPDISABLED`, and the LED visibly goes from on to off.

**Verdict tiers**: this page uses only "compiles / verified on hardware / not verified", with no embellishment.

| Item | Status |
|---|---|
| Kernel compiles (F427, `ARKRTOS_HW_SAFE=1`) | **0 Error(s)** |
| Gate `tools/ci_gate.py` | **all pass** |
| On board: reaping–neutralization path (hardware taken out of its dangerous state after an App crash) | **pass** (`c021168`, `neutralized=6 last_owner=12`, two independent fresh installs) |
| On board: **per-channel counters** (`will` / `cb` / `none`) | **pass** (`HWSAFE_DEMO 1.0.1` two phases: `will=1 cb=2 none=3 fail=0 handover=0`; after three crashes `app list` shows that slot `crash=3 held=crash-limit`) |
| On board: driver-callback channel alone (channel A, App leaves no will) | **pass** (same image phase 2/2: `led1` lit, crashes without a `will`; `cb` counter reached 2 entirely from this channel, LED on→off; every `own` line shows `will=none`) |
| On board: crash-limit three-time disable + survives reset | **pass** (`CRASH:3006 slot N disabled: crash-limit, 3 consecutive faults`; after `reboot` the first line is `CRASH:xxx slot N held: crash-limit, 3 faults, last at tick <original tick>`, `app list` shows `auto=no`/`INVALID` and it no longer autostarts; readings unchanged after a second reboot on the same disk) |
| On board: planned handover (mini-program normal exit) | **not done** (handover semantics were introduced this round; the same change set was landed and verified on board on the svcrtos_nano side, see nano's docs/App崩溃硬件安全与遗言机制.md) |
