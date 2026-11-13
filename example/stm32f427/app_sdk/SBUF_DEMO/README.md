# SBUF_DEMO：把流缓冲 / 消息缓冲开给 App 用（SVC 0x1F）

这份例程是 [流缓冲与消息缓冲](../../../../docs/流缓冲与消息缓冲.md) 里「非特权通道」
那条判据的实现：**一个 `.svcapp` 经 SVC 0x1F 收发字节流与消息，并打印每一条判据**。

它跑在 App 自己的非特权任务里，收发缓冲都是 App 自己的静态数组。所有
`arkrtos_sbuf_*` / `arkrtos_mbuf_*` 调用都过 SVC 边界。

| 段落 | 覆盖 |
|------|------|
| 创建与初始状态 | `create` 拿到句柄；`free()` 报出的容量是 256、`used()` 是 0 |
| 流语义 | 写 5 字节返还 5；读 2 字节拿走前两个、再读 8 字节拿到剩下 3 个且顺序是 `l..o` |
| 空读 | `timeout_ms=0` 的空读返回 **-2**（TIMEOUT），不是 0 |
| 写满 | 一次正好写满 256；`free()` 归零；再写 1 字节被拒；`reset` 后容量全还 |
| 回绕 | 写满 → 读掉前 128 → 再写 128 → 整段读回，字节序仍是 0..255 连续 |
| 消息语义 | `mbuf_send` 成功返回 0；整条读回；接收缓冲只有 4 字节时返回 **-6**（TOO_SMALL）且**不串**下一条 |
| 参数校验 | 长度 0 / 长度负值 / 源指针为空 三条一律被拒，且 `used()` 一点没变 |
| 阻塞超时 | App 侧没有"让出"通道，封装是 1 ms 轮询重试：空读带 100 ms 超时实测**正好 100 ms** 返回 `-2`；写满后再带 50 ms 超时写，实测**正好 50 ms** 返回 `-2` |
| 生命周期 | `delete` 返 0；删除后 `used()`/`send` 都失败 |

最后两行是 `SBUF: N pass, M fail` 与 `SBUF: ALL PASS` / `SBUF: FAILED`，
`main` 的返回值跟着 `fail` 走。

## 怎么跑

内核必须先编进这个功能（**源码头默认 0**，与 tickless / sbuf 那两次上板一致）：

1. 在 `example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx`
   的 Define 框里临时加 `ARKRTOS_USE_SBUF=1`，编译并烧进板子；
2. 本工程自带 `ARKRTOS_USE_SBUF=1`（App 头 `arkrtos.h` 按功能开关裁剪声明，
   不开就看不见 `arkrtos_sbuf_*`）；
3. `pack_app.py --type app --dev-slot 0` → `.svcapp` → `fs_put.py` → 串口
   `app stop 0` / `app install /sbuf_demo.svcapp 0` / `app start 0`。

内核**没有**编进 SBUF 时，SVC 0x1F 一律如实返回 -1（`create` 直接失败），
不会假装成功。

## 实测（STM32F427 + DAPLink，2026-11-13）

```
--- sbuf / mbuf demo (SVC 0x1F) ---
create -> 25165824
  PASS  create returns a handle
capacity=256 used=0
  PASS  a fresh buffer reports free space
  PASS  a fresh buffer is empty
send("hello",5) -> 5
  PASS  a short write takes the whole 5 bytes
  PASS  used() follows the write
  PASS  a partial read takes the first two bytes
  PASS  the rest comes out in order
  PASS  the buffer is drained
recv(empty,4,0) -> -2
  PASS  reading an empty buffer fails instead of returning 0
send(cap=256) -> 256
  PASS  one write can fill the buffer exactly
  PASS  free() reports zero when full
  PASS  writing to a full buffer fails instead of dropping bytes
  PASS  reset returns 0
  PASS  reset gives the whole buffer back
  PASS  writing again after a partial read succeeds
  PASS  the wrapped buffer still holds a full buffer worth of bytes
  PASS  bytes stay in order across a wrap
mbuf_send("msg-1",5) -> 0
  PASS  a message send returns 0 on success
  PASS  the message comes back whole
mbuf_recv(4) -> -6
  PASS  a message longer than the receive buffer is refused
  PASS  the refused message did not smear into the next one
send(len=0) -> -1
  PASS  a zero length send is refused
  PASS  a negative length send is refused
  PASS  a null source buffer is refused
  PASS  refused sends left the buffer alone
send(app flash ptr) -> 4 (App 自己的常量区是合法源)
  PASS  the app's own flash is a legal source buffer
recv(empty, 100ms) -> -2 in 100 ms
  PASS  a blocking read on an empty buffer times out
  PASS  the read waited its full 100 ms
send(full buffer, 50ms) -> -2 in 50 ms
  PASS  a blocking write on a full buffer times out
  PASS  the write waited its full 50 ms
delete -> 0
  PASS  delete returns 0
  PASS  used() on a deleted handle fails
  PASS  send on a deleted handle fails
SBUF: 33 pass, 0 fail
SBUF: ALL PASS
```

## 五条口径（都是这次上板才看清的）

1. **句柄不是小整数**：`create` 返回 `25165824` = `0x01800000`（句柄里编了对象类
   与序号）。按十进制打印看着离谱，但它 `>= 0` 就是句柄，别当错误码。
2. **App 侧看不到错误码枚举**：`arkrtos.h` 只给函数声明，`ARKRTOS_SBUF_ERR_*`
   是内核头里的东西。所以 App 只能按"负值=失败"分档；上面打印的 `-2`（TIMEOUT）
   与 `-6`（TOO_SMALL）是实测值，不是从枚举里读出来的。要精确到码，得看内核侧
   `utest` 的三个 sbuf 用例。
3. **"递一个 Flash 地址就该被拒"是错的**：一开始按"内核 Flash 不可读写"写了一条
   反向用例，结果 `send(main 的地址)` **返回 4**。原因很简单——App 的镜像本来就在
   Flash 里，内核的边界检查要求的是"落在调用者可见的 RAM/ROM 内"，App 自己的常量区
   正好合法。用例因此改成保留这条正向读数，并把真正的拒绝面写成长度/空指针。
4. **固定槽位安装后 `app list` 的 `size` 是整槽跨度**：`app install <path> 0` 走的是
   固定槽位路径，分区表里登记的是配置里那个槽的尺寸（槽 0 = 131072），
   不是镜像字节数（本镜像文件 7452 B）。要看镜像长度，装的时候日志里那句
   文件大小那一行才是。这一列的 shell 注释写的是"真实占用字节数"，对固定槽位
   不成立——按"宁可报错不给错答案"，这里以实测为准记下来。

5. **App 侧的"阻塞"是 1 ms 轮询，不是真让出**：App 任务没有创建/让出任务的 SVC
   通道，所以 oslib 的封装在内核回 `WOULDBLOCK` 后按 1 ms 重试直到超时。上板量
   了两档：空读带 100 ms 超时、写满后再带 50 ms 超时，`arkrtos_get_time_ms()` 量
   出来的都是**正好 100 / 50 ms**（既不早醒也不挂住）。这也说明 1 ms 粒度对毫秒级
   超时是够的，但**不是**"1 ms 内就能醒"——醒来靠的是下一次轮询。
