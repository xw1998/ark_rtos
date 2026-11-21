# SVC Service Numbers and Subcommand Table

> Condensed English edition. The Chinese page (SVC服务号与子命令表.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

When an App / driver runs unprivileged, every kernel service enters the kernel through one `SVC` instruction. A call is numbered on two levels:

| Level | Where it lives | Defined in | Meaning |
|---|---|---|---|
| Service number | immediate of the `SVC` instruction | `ARKRTOS_SVC_*` in `kernelsrc/include/arkrtos_def.h` | 8 bits, selects the handler |
| Subcommand number | `p[0]` of the parameter block | `kernelsrc/include/arkrtos_svc_abi.h` (net services in `arkrtos_net_abi.h`) | selects the operation |

Both tables are shared through the same macros by the kernel dispatcher (`svc_*()` in `kernelsrc/src/arkrtos_task.c`) and the SDK wrappers (`arkrtos_oslib.c`, `arkrtos_driver_bridge.c`). Changing a subcommand means changing one line in the header and both sides follow; the era of writing the raw number twice is over (changing one and forgetting the other silently took the wrong branch instead of failing to compile).

## Service numbers

| No. | Macro | Handler | Subcommands |
|---|---|---|---|
| 0x10 | `ARKRTOS_SVC_DEV_IO` | `svc_dev_io` | 6 |
| 0x11 | `ARKRTOS_SVC_TASK_CTRL` | `svc_task_ctrl` | 10 |
| 0x12 | `ARKRTOS_SVC_SYS_INFO` | `svc_sys_info` | 5 |
| 0x13 | `ARKRTOS_SVC_EVENT_CTRL` | `svc_event_ctrl` | 3 |
| 0x14 | `ARKRTOS_SVC_DRV_MGR` | `svc_drv_mgr` | 6 |
| 0x15 | `ARKRTOS_SVC_SYNC_CTRL` | `svc_sync_ctrl` | 17 |
| 0x16 | `ARKRTOS_SVC_MQ_CTRL` | `svc_mq_ctrl` | 4 |
| 0x17 | `ARKRTOS_SVC_TIMER_CTRL` | `svc_timer_ctrl` | 4 |
| 0x18 | `ARKRTOS_SVC_APP_MGR` | `svc_app_mgr` | 7 (No. 1 retired) |
| 0x19 | `ARKRTOS_SVC_LOG` | `svc_log` | no `p[0]` (r0/r1/r2 are level/tag/msg) |
| 0x1A | `ARKRTOS_SVC_SHELL` | `svc_shell` | 3 |
| 0x1B | `ARKRTOS_SVC_THREAD_CTRL` | `svc_thread_ctrl` | 3 |
| 0x1C | `ARKRTOS_SVC_FILE_SYS` | `svc_file_sys` | 23 (files 1..10 + paths 11..23) |
| 0x1D | `ARKRTOS_SVC_HEARTBEAT` | `svc_heartbeat` | 1 |
| 0x1E | `ARKRTOS_SVC_NET` | `svc_net` | 17, see `arkrtos_net_abi.h` |
| 0x1F | `ARKRTOS_SVC_SBUF_CTRL` | `svc_sbuf_ctrl` | 12 (stream 1..8 + message 9..12) |
| 0x20 | `ARKRTOS_SVC_MEMPOOL_CTRL` | `svc_mempool_ctrl` | 5 |
| 0x21 | `ARKRTOS_SVC_ESET_CTRL` | `svc_eset_ctrl` | 6 (compiled in only when `ARKRTOS_USE_ESET` is on) |
| 0x22 | `ARKRTOS_SVC_WAITMULTI_CTRL` | `svc_waitmulti_ctrl` (`arkrtos_task.c`) | 1 (compiled in only when `ARKRTOS_USE_WAITMULTI` is on) |

## Subcommand numbers

Per-number detail lives in `kernelsrc/include/arkrtos_svc_abi.h`: one block per service, macro form `ARKRTOS_<module>_SUB_<operation>`, written as `(Nu)` like the net service table. Two special cases:

- `ARKRTOS_SVC_FILE_SYS` 1..10 are **volume-relative** operations (`arkrtos_fs_*`, path relative to the mount point); 11..23 are **namespace-absolute** operations (`arkrtos_vfs_*`, path starting with `/`). Both segments sit in one table but carry different prefixes (`ARKRTOS_FILE_SUB_*` / `ARKRTOS_PATH_SUB_*`), because they are two path semantics that merely share one SVC.
- `ARKRTOS_SVC_APP_MGR` starts at 2. No. 1 was the early "install image by slot", superseded by 2 and retired; numbers are never reused.

## Conventions

1. **Numbers are ABI**: add-only, never change. An App image is flashed on the board and an image built with the old SDK sends requests by the old numbers; reusing a number for another operation shows up as calling the wrong operation, not as a failed call.
2. **Names may change, values may not.** Renaming touches both sides' source; changing a value touches the ABI.
3. **The `p[]` layout of each subcommand is not duplicated in the header**: it is written by the SDK wrapper (`arkrtos_oslib.c` fills `p[1..]`) and validated by `arkrtos_kernel_*_ok()` in the kernel handler. The header fixes numbers only, so one layout never exists in two places.
4. **A new subcommand touches three places**: one line in `arkrtos_svc_abi.h`, one `case ARKRTOS_<module>_SUB_<operation>:` in the kernel `svc_*()`, and the same macro written to `p[0]` in the SDK wrapper. Same name in all three, so grepping the macro shows everything.

## Parameter block word width (wide-argument ABI)

Each slot of the parameter block `p[]` is still 32-bit (`uint32`) by default, even on 64-bit targets. With wide VA a user address may exceed 4 GB and does not fit in a `uint32` slot, so one compile-time switch upgrades the parameter block to native word width:

| Item | Narrow (default) | Wide |
|---|---|---|
| Switch | `ARKRTOS_USE_WIDE_SVC_ARGS=0` | `ARKRTOS_USE_WIDE_SVC_ARGS=1` |
| Slot type | `typedef unsigned int arkrtos_svc_arg_t` | `typedef unsigned long arkrtos_svc_arg_t` |
| Subcommand marker | none (`p[0]` is the subcommand number) | high bit of `p[0]` set to `ARKRTOS_SVC_SUB_WORD` (`0x80000000UL`) |
| Block bytes | `n*4` | `n*8` (`ARKRTOS_SVC_ARG_BYTES(n)`) |
| Effective when | always narrow | also requires `__SIZEOF_LONG__ > 4` (`ARKRTOS_SVC_WIDE_ARGS`) |

All macros live in `arkrtos_svc_abi.h`: `ARKRTOS_SVC_SUB(sub)` fills `p[0]` (setting bit31 in the wide build), `ARKRTOS_SVC_SUB_OF(w)` extracts the subcommand number, `ARKRTOS_SVC_ARG_BYTES(n)` computes the length to validate. On the kernel side all 30 `svc_*()` handlers validate "block lies in user RAM and bit31 is set" through `SVC_WIDE_GUARD()`; the net service additionally widens `g_net_req_arg[]`.

Three ground rules:

1. **Wide arguments are a compile-time switch, not adaptive.** Kernel and App must be built with the same `ARKRTOS_USE_WIDE_SVC_ARGS`. Wide kernel + narrow App: the kernel prints a one-shot log line `wide-args kernel got a 32-bit param block; kernel and app must be built with the same ARKRTOS_USE_WIDE_SVC_ARGS` and **terminates the calling task** (`arkrtos_kernel_svc_wide_reject()` goes through `arkrtos_task_kill_internal`, the same path as `THREAD_SUB_KILL`): a width mismatch is a whole-ABI mismatch and every later SVC of that App would read wrong arguments, so returning -1 alone leaves the App spinning in user mode — which on the console looks exactly like "the kernel never got the call". After termination `exec` / `wait` observe the task end and the loader ledger records it; narrow kernel + wide App: the call is rejected, never silently succeeding.
2. **On 32-bit targets (ARM / RV32 / LA32) the wide build is a no-op**: `ARKRTOS_SVC_WIDE_ARGS` requires `__SIZEOF_LONG__ > 4`, so even with the switch on the slot stays 32-bit and neither size nor behaviour changes. The default build (switch 0) likewise changes nothing on 64-bit targets.
3. **The SVC return register is only 32 bits**: in the wide build, if a mempool block lands above 4 GB the kernel returns 0 and prints a one-shot log line rather than returning a truncated address.

The gate `tools/svc_wide_gate.py` (the `svc-wide` item of `ci_gate`) pins three things: the guard really calls reject and reject really kills the caller (missing either link brings back the silent spin); `SVC_WIDE_GUARD` expands to an empty statement in the narrow build; the Linux compatibility layer state (fd table, heap cursor, signal table) is cleared before the new task joins the ready set (before `elf_start_task()`), and a wrong position fails the gate — the measured signature is fixture stage1 `brk(0)` sitting 8192 above the window heap base, flipping red/green with code layout. Six mutation self-checks (guard-reject / guard-ret / narrow-empty / kill / subword / reset-order) each have to turn the gate red.

Not covered: `kernelsrc/sdk/mini_sdk/arkc/` remains `u32`-based (the mini-program runtime is compiled separately from arkc's deployment manifest and does not see these headers), so it is outside this mechanism under wide VA.

## Relation to the mini-program SDK

`kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c` is the minimal copy of these two services (0x1A console, 0x1B thread exit) for the arkc + mini-program case, and it **does not include** the two tables above: that file is compiled once by arkc (which only eats files listed in the deployment manifest) and once by the host gcc gate (with no `-I`), so neither build gets a header search path. The copy inlines the same macro names and the comment names the authoritative source.
