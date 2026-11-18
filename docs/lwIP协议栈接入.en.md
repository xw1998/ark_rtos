# lwIP Integration (the base under the socket / select surface)

> Condensed English edition. The Chinese page (lwIP协议栈接入.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

## One line

The kernel has lwIP 2.1.3 compiled in (`kernelsrc/components/lwip`), gated by `ARKRTOS_USE_LWIP`; for now only a **loopback** interface is up (127.0.0.1/8), with no real Ethernet transmit/receive yet.

**Verification tier (read carefully)**: the current status is **verified on board** — F427 kernel project 0 Error, F401 kernel project regression 0 Error; on F427, `SOCKET_DEMO` (overwrite install into fixed slot 0) ran the socket semantics self-test with `result : pass=36 fail=0` (per-item evidence in socket与select兼容层.md §7).

## 1. Why lwIP, and why loopback first

* Face B of the App compatibility surface (socket / select) cannot exist without a TCP/IP stack, and the repository had none;
* full-functionality OS mode is the only path with a sockets API (`NO_SYS=1` gives only the raw API);
* this F427 board **has no evidence of an Ethernet PHY**: the repository only contains ETH/PHY macros left by CubeMX, no ETH initialisation at all, and the kernel project does not compile `stm32f4xx_hal_eth.c`. So what gets verified first is **the stack and the socket semantics** (connect/send/receive/close/timeout all go through the same TCP state machine); real transmit/receive waits for hardware that has a NIC.
* The board side was re-checked separately: under `board/stm32f427/` there is no PHY (LAN8720-class) driver, no MDIO read/write, no `eth_`-prefixed interface, and no trace of RMII / MDIO in the pin assignment. In other words what is missing here is **hardware plus a PHY driver**, not a software TODO; before a board with a PHY appears there is no physical evidence for real link traffic, so this page records it as **unverified**.

## 2. Layout and provenance

| Path | Content |
|---|---|
| `kernelsrc/components/lwip/src/` | subset of the official lwIP source (192 files, unmodified, see `VENDOR.md` in the same directory) |
| `kernelsrc/components/lwip/port/` | board port layer: `lwipopts.h`, `arch/cc.h`, `arch/sys_arch.h`, `sys_arch.c`, `lwip_port.[ch]` |

Third-party source contains **no `#if ARKRTOS_USE_LWIP`** (editing third-party code costs more than it buys); everything configurable for the build sits in `port/`, and with the switch off the port layer is simply not compiled.

## 3. Build integration (F427 kernel project)

* `ARKRTOS/lwIP`: 33 `.c` files under `src/**`; `ARKRTOS/lwIP Port`: 2 `.c` files under `port/**`;
* include paths: `kernelsrc\components\lwip\src\include`, `kernelsrc\components\lwip\port`;
* switch: the Kconfig symbol `ARKRTOS_USE_LWIP` (default `n`). The F427 default profile turns it on in `config/arkrtos_defconfig`; the F401 / H743 board headers and defconfigs keep it off. **Do not write `ARKRTOS_USE_LWIP=1` in the project `<Define>` any more** — project macros were zeroed on 2026-11-14 and values come only from Kconfig (see Kconfig配置系统.md §8).

## 4. Three conventions of the port layer

1. **Timeout semantics must be translated**: lwIP says `timeout==0` means "wait forever"; Ark RTOS says 0 means "do not wait" and only negative values mean forever. Every entry point goes through `lwip_to_arkrtos_timeout()`; nobody hard-codes the parameter.
2. **Failure is never dressed up as success**: kernel object creation failure, invalid handle, and any error other than timeout all return the failure value lwIP recognises, and leave a kernel log record with the error code. A mailbox request deeper than `ARKRTOS_MQ_DEPTH` (8) returns `ERR_MEM` directly, **never silently truncated**; an unavailable thread stack returns `SYS_THREAD_NULL`.
3. **Inside the kernel use the `*_internal` names only**: bare names like `arkrtos_sem_create()` are **App SDK SVC wrappers** (`kernelsrc/sdk/app_sdk/arkrtos_oslib.c`) and do not link inside the kernel. The kernel calls `arkrtos_sem_create_internal()` / `arkrtos_mtx_*_internal()` / `arkrtos_mq_*_internal()` / `arkrtos_task_wait_period_internal()` / `arkrtos_kernel_get_time()`.

Two more facts that must be known:

* `sys_now()` goes through `arkrtos_kernel_get_time()` (milliseconds derived from the tick inside the kernel);
* `errno` is **one global variable** (defined by the port when `LWIP_PROVIDE_ERRNO=1`). Sharing `errno` across tasks is a known limitation of lwIP's own sockets layer; no per-task copy is made — to decide whether a socket call failed **look at the return value**, and never depend on `errno` across tasks.

## 5. Loopback is "multithreading mode", no external polling needed

`LWIP_NETIF_LOOPBACK_MULTITHREADING` is **hard-wired to 1** in `lwipopts.h`:

* the lo interface is created automatically by `lwip_init()` → `netif_init()` (address 127.0.0.1/8, input hooked to `tcpip_input`);
* loopback send calls `netif_loop_output()`, which queues pbufs on the loop list and then uses `tcpip_try_callback(netif_poll)` to **hand them back to the stack from the tcpip thread itself**;
* therefore **no one** needs to call `netif_poll_all()` periodically from a main loop.

> This is a correction record: in multithreading mode `netif_poll_all()` is not even declared (in `netif.h` it sits inside `#if !LWIP_NETIF_LOOPBACK_MULTITHREADING`), so copying the folklore "loopback must poll itself" simply does not compile. Only polling mode (`=0`) requires calling it yourself.

`port/lwip_port.c` therefore does exactly one thing: call `tcpip_init()` once in the first round of one kernel task. **It must not be called from `main()`** — it blocks until the tcpip thread finishes initialisation (internal semaphore), and `main()` runs before the scheduler starts, so it would wait forever. The task then idles on a 1 s period (the kernel has no "end yourself" semantics); when a real NIC arrives, packet moving lands in that loop.

## 6. The cost: kernel region 128K → 256K

lwIP grows the kernel code from `Code=113214` to `Code=145222` (about +32 KB). A 128 KB kernel region does not fit and the link fails:

```
Error: L6407E: Sections of aggregate size 0x5de0 bytes could not fit into .ANY selector(s).
```

MPU regions must be powers of two (`arkrtos_mpu_window_check` in `arkrtos_partition.h`), so the next step above 128K can only be 256K:

| Partition | Before | After |
|---|---|---|
| KERNEL | 128 KB (`0x08000000~0x0801FFFF`) | **256 KB** (`0x08000000~0x0803FFFF`) |
| CONFIG | 128 KB (`0x08020000`) | 128 KB (`0x08040000`) |
| IMAGE_POOL | 768 KB (`0x08040000`, 6 sectors) | **640 KB** (`0x08060000`, 5 sectors) |
| pool allocatable (minus 1 sector compaction margin) | 640 KB | 512 KB |

**Knock-on effect**: the device-side config region and the pool base both move up, so the layout config already written on the board and any installed images **no longer match** — the config must be rewritten and images reinstalled. To avoid that cost, drop the project macro `ARKRTOS_USE_LWIP` and override `KERNEL_SIZE` back to 128K with `-D`.

## 7. Verified / unverified

| Item | Conclusion | Evidence |
|---|---|---|
| F427 kernel project links | **compiles only** | `Code=171350 RO-data=10254 RW-data=716 ZI-data=95544`, 0 Error / 1 Warning (`arkrtos_context.S A1581W`, pre-existing); this is the current build including the socket service (SVC 0x1E) |
| `.sct` recomputed from the config header | verified | `build/kernel.sct` generates `LR_KERNEL 0x08000000 0x00040000` |
| F401 kernel project regression | compiles only | `Code=100130`, 0 Error / 1 Warning. `ARKRTOS_USE_LWIP` is off in the F401 config, `arkrtos_net.c` takes the stub branch |
| routing gate | verified | `tools/ci_gate.py` 7/7 |
| board run, socket semantics self-test | **verified on board** | F427 + `SOCKET_DEMO` (overwrite install into fixed slot 0): `[LWIP:45] tcpip up, loopif 127.0.0.1` → `[NET:1028] socket service up (SVC 0x1E)` → App self-test, `result : pass=36 fail=0`; per item see socket与select兼容层.md §7 |
| real Ethernet transmit/receive | **unverified** | no PHY evidence on the board side (no PHY driver / MDIO / `eth_` interface / RMII pin assignment), see §1 |

## 8. Capacity and limits (remember these, they bite when the App surface arrives)

* threads have only **2 slots** (tcpip thread + one reserved for a future NIC driver thread), each with a **2 KB static stack** — the kernel only accepts the caller's own stack, so the stacks are static arrays in `sys_arch.c`;
* mailbox depth defaults to 8, ceiling `ARKRTOS_MQ_DEPTH` (8);
* `MEM_SIZE=8 KB`, `PBUF_POOL_SIZE=8`, `MEMP_NUM_TCP_SEG=16`, `MEMP_NUM_TCP_PCB=4`, `MEMP_NUM_NETCONN=2` — values chosen under the current kernel RAM headroom and to be recalculated when a real NIC is connected.
