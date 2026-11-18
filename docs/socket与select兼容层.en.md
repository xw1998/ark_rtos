# socket / select compatibility layer (App side)

> Condensed English edition. The Chinese page (socket与select兼容层.md) is normative: every number below is copied from it as of 2026-11-18, but if the two disagree, the Chinese page wins. This page adds no new claim and no new measurement.

## One line

Apps can write Linux-style socket programs directly: `socket/bind/listen/accept/connect/send/recv/sendto/recvfrom/select/getsockname/getpeername/shutdown/getsockopt/setsockopt/fcntl` are provided by `kernelsrc/sdk/posix`, which reaches the kernel socket service (`kernelsrc/src/arkrtos_net.c`) through **SVC `0x1E`**, and that service forwards to lwIP inside the kernel.

**Verification tier (read carefully)**: the current status is **verified on board** — kernel side F427/F401 both 0 Error, App side `SOCKET_DEMO` 0 Error / 0 Warning with the socket code really linked into the image; on F427, installing `SOCKET_DEMO` over fixed slot 0 and running the socket semantics self-test gave `result : pass=36 fail=0` (per item in §7). The stack has only the lwIP loopback interface (127.0.0.1/8); packets never leave the chip; **real Ethernet transmit/receive is unverified** (see lwIP协议栈接入.md).

## 1. Headers and names

| Header | Provides |
|---|---|
| `sys/socket.h` | `struct sockaddr` (Linux layout), `AF_INET` / `SOCK_STREAM` / `SOCK_DGRAM`, `SOL_SOCKET` / `SO_*`, `MSG_*`, `SHUT_*`, `fd_set` + `FD_ZERO/SET/CLR/ISSET`, 15 function prototypes and POSIX alias macros |
| `netinet/in.h` | `struct in_addr` / `struct sockaddr_in` (Linux layout), `INADDR_*`, `IPPROTO_*` |
| `arpa/inet.h` | `htons/ntohs/htonl/ntohl`, `inet_addr`, `inet_ntoa`, `inet_pton`, `inet_ntop` |
| `sys/time.h` | `struct timeval` (the `select` timeout; define `ARKRTOS_POSIX_NO_TIMEVAL` to avoid toolchain-version clashes) |
| `fcntl.h` | `F_GETFL` / `F_SETFL` / `O_NONBLOCK` |
| `errno.h` | socket-related errno (`EWOULDBLOCK` / `ECONNREFUSED` / `EAFNOSUPPORT` ...) |
| `sys/stat.h` | `S_IFSOCK` / `S_ISSOCK` |

Structs always use the **Linux layout** (no BSD `sa_len`): the address crosses the boundary as only two scalars (host-order ip + host-order port), so the layout is free, and choosing Linux lets source written for Linux/Windows compile unchanged. `SOL_SOCKET` and `SO_*` also take the Linux values.

## 2. Boundaries (only this much for now)

* **`AF_INET` only**: `socket(AF_INET6, ...)` returns an explicit error, never silently accepted;
* no DNS (`gethostbyname` / `getaddrinfo` are outside the compatibility surface); write addresses with `inet_addr` or `inet_pton`;
* no `poll()`, only `select()`;
* the ceiling for simultaneous sockets is **4** (kernel lwIP `MEMP_NUM_NETCONN=4`).

## 3. The kernel only registers; user mode polls

The kernel SVC handler **must not block** (see 同步原语与令牌守恒.md), so the socket surface keeps the same line:

* the App puts one request (a complete parameter block) into the kernel request slot and polls with **the same parameter block** until there is a result;
* the kernel request slot matches on **the complete parameter block**, not on owner + subcommand: otherwise a caller that abandoned a request and asks something else (or another socket sending the same `send`) would receive the previous reply — that is an "authoritative-looking wrong answer";
* a dead caller no longer holds the request slot forever (`arkrtos_net_release_task()` sets the owner to -1, and the next caller discards that orphaned reply).

**Blocking semantics are delivered on the App side**: the "wait" in `send` / `recv` / `accept` / `connect` is built from `select` (kernel POLL) + `arkrtos_task_wait(1)`; `SO_RCVTIMEO` / `SO_SNDTIMEO` are stored by the App and take effect at this layer (the kernel socket call itself never blocks), and **0/0 means wait forever** (POSIX semantics). Unrecognised `setsockopt` / `getsockopt` always return `ENOPROTOOPT`, and options the kernel does not know return `ENOTSUP` — refused, never silently accepted.

`O_NONBLOCK` lives in the App-side fd table, set with `fcntl(fd, F_SETFL, ...)`; `select()` reports **non-socket** fds (paths / devices / bare handles) as always ready — they have no "not ready yet" state, matching the POSIX answer for ordinary files.

## 4. SVC `0x1E` wire contract

The contract header is `kernelsrc/include/arkrtos_net_abi.h` (LF, pure ASCII). Key points:

* parameter block `ARKRTOS_NET_ARG_WORDS = 8` words;
* 17 subcommands `0..16`: QUERY / SOCKET / BIND / LISTEN / ACCEPT / CONNECT / SEND / RECV / SENDTO / RECVFROM / CLOSE / SETSOCKOPT / GETSOCKOPT / GETSOCKNAME / GETPEERNAME / SHUTDOWN / POLL;
* error codes `-200 ~ -215` (`EPENDING` / `EBUSY` / `ENOTSUP` / `EINVAL` / `EWOULDBLOCK` / `EINPROGRESS` / `EALREADY` / `EISCONN` / `ENOTCONN` / `ECONNREFUSED` / `ECONNRESET` / `ECONNABORTED` / `EHOSTUNREACH` / `EMSGSIZE` / `ENOBUFS` / `EIO`);
* `ARKRTOS_NET_MAX_SOCKETS = 4`; `ARKRTOS_NET_IP(a,b,c,d)` composes an address.

With `ARKRTOS_USE_LWIP != 1`, `arkrtos_net_svc` is a degraded stub: everything returns `ARKRTOS_NET_ENOTSUP`, mapped to `EOPNOTSUPP` on the App side. That means **when the kernel was built without a stack, `socket()` fails explicitly** instead of handing out a fake handle.

## 5. Limits and known issues

* **Overlapping fd number spaces**: App fds returned by `arkrtos_posix_open()` (from 3 up) overlap numerically with bare kernel handles returned by `arkrtos_dev_open()`; when a number is used as an App fd, the table's verdict wins, so mixing the two handle kinds misjudges. **Never hand a bare `arkrtos_dev_open` handle to any POSIX interface other than `read`/`write`/`close`.**
* **A path `open()` with `O_NONBLOCK` is accepted and remembered**: POSIX says it has no effect on ordinary files, so the kernel strips the bit before passing it to the path layer while recording it in the App's own fd table (readable via `fcntl(F_GETFL)`, changeable via `F_SETFL`), so portable code does not fail because of it. See POSIX与Windows兼容层说明.md §4.7.
* Multiple tasks share lwIP's global `errno` (a known limitation of lwIP's sockets layer): to decide whether a call failed **look at the return value**, and never depend on `errno` across tasks.

## 6. SOCKET_DEMO: an unmodified socket program

`example/stm32f427/app_sdk/SOCKET_DEMO/Src/main.c` is the experiment piece for face B (face A is `FS_DEMO`, the process/thread face is `POSIX_DEMO`): an ordinary POSIX sockets program whose same source compiles on Linux with `cc main.c`, and here gets its names answered by the compatibility layer.

What it covers: `htons/ntohs` round-trip, `inet_addr` / `inet_ntoa`; TCP echo (socket / `SO_REUSEADDR` / bind / listen / getsockname / connect / accept / getpeername / bidirectional send-recv / `select` readable / `select` timeout / `shutdown(SHUT_WR)` peer reads 0 / `SO_ERROR`); UDP (bind / `sendto` / `recvfrom` including the source port); `socket(AF_INET6)` must fail. Each line prints `ok` / `FAIL`, and the tail prints `result : pass=N fail=M`.

## 7. Verified / unverified

| Item | Conclusion | Evidence |
|---|---|---|
| kernel socket service links (F427) | verified on board | `Code=170958 RO-data=10162 RW-data=708 ZI-data=95552`, 0 Error / 1 Warning (pre-existing `arkrtos_context.S A1581W`) |
| kernel regression (F401) | compiles only | `Code=99742`, 0 Error / 1 Warning; `ARKRTOS_USE_LWIP` off in F401, `arkrtos_net.c` takes the stub branch |
| App-side socket surface links | verified on board | `SOCKET_DEMO` 0 Error / 0 Warning, `Code=10726` (about 3.6 KB more than `POSIX_DEMO`'s 7126, showing the socket block really is linked into the image) |
| routing gate | verified | `tools/ci_gate.py` 7/7 |
| **board run, socket semantics self-test** | **verified on board** | F427 + `SOCKET_DEMO` (fixed slot 0, overwrite install): `result : pass=36 fail=0`. Order was `[LWIP:45] tcpip up, loopif 127.0.0.1` → `[NET:1028] socket service up (SVC 0x1E)` → App self-test; TCP echo (including `select` readable and timeout, `shutdown(SHUT_WR)`, `SO_ERROR`), UDP (`sendto` / `recvfrom` including source port), and `socket(AF_INET6)` must fail — all three groups pass |
| real Ethernet transmit/receive | **unverified** | this board has no usable PHY evidence; what runs today is lwIP's own loopback interface |

## 8. Three bugs that only appeared on the first board run

The common shape of all three is **"success/failure read backwards" or "sentinel value versus zero-initialisation mismatch"**: everything compiles, the mock passes too, and only real hardware running `SOCKET_DEMO` reveals them. They are recorded here because they are all "looks like a conclusion" errors rather than crashes.

| Location | Symptom | Real cause |
|---|---|---|
| `arkrtos_net.c` `arkrtos_net_start()` | log says `[E][NET] service task register failed`, `socket()` returns `EOPNOTSUPP(95)` | `arkrtos_task_register()` **returns the task number (>= 1) on success** and only returns negative on failure; the code wrote `if(rc != 0)`, so every success was treated as failure — the freshly created request queue was deleted by itself, `g_net_mq` was reset to -1, while the task was in fact already running. Criterion changed to a sign test (`rc < 0`) |
| `arkrtos_net.c` `g_net_sock[]` | the very first `socket()` returns `ENOBUFS(105)` | the row table uses "`fd < 0` = this row is free", but the table is BSS zero-initialised, so at boot every row has `fd == 0` and **looks occupied**, making `arkrtos_net_claim()` return `ENOBUFS` immediately. Changed to explicit per-row initialisation to `-1`, plus a compile-time assertion "row count must match initialiser count" |
| `arkrtos_net.c` / `arkrtos_mq.c` | `socket()` returns `EBUSY(16)`, `g_net_mq` points at a queue slot with `used == 0` | the request queue was created by "the current task", so its `creator_id` was the lwIP port task, and any resource reclaim of that task carried away the kernel's own service queue; meanwhile `arkrtos_net_start()` was idempotently short-circuited by `g_net_mq >= 0` and `arkrtos_net_ready()` only checked that the handle was non-negative, so it still reported "started". Changed to kernel ownership (`arkrtos_mq_create_kernel_internal()`, `creator_id = 0`) plus a new `arkrtos_mq_is_alive()`: `ready` tells the truth, `start` can rebuild when the handle is invalid, and the service task waits periodically instead of spinning hot when the queue is invalid |

The lwIP port task priority was also raised to **9** (level with `DRIVER_TASK_PRIORITY`, above `APP_TASK_PRIORITY (10)`): it is responsible for bringing lwIP up, and an App that calls `socket()` in its first statement must already be able to get the service. Before raising it, a boot race let the App run first and get `EOPNOTSUPP` — which was **true** (the service really was not up yet), but a plain POSIX program has no way to retry that, so the ordering must be guaranteed by the kernel rather than by luck. The task only works in its first round and then idles on a 1 s period, so it costs no CPU.

**This lesson deserves its own line**: the return value of `arkrtos_task_register()` is a task number, not a `0/negative` status code. Any call site that does more work after a successful registration must test the sign.
