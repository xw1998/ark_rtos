# ESET_DEMO：把 32 位事件集开给 App 用（SVC 0x21）

这份例程是 [同步原语与令牌守恒](../../../../docs/同步原语与令牌守恒.md) §5.4 里
「32 位事件集」那条**非特权通道**的判据实现：**一个 `.svcapp` 经 SVC 0x21
创建/置位/等待/清除/销毁事件集，并把每一步的返回值与位图打到控制台**。

宿主用例（`tests/test_eset_host.c`）证明的是内核态语义；这份例程证明的是
**同一个对象从 App 侧穿 SVC 边界**那一层：句柄编码、参数校验、位图回传、
销毁后的拒绝，全部在板上的非特权任务里跑。

| 段落 | 覆盖 |
|------|------|
| 创建 | `create` 返回非负句柄；新对象 32 位全 0 |
| 累加 | `set(bit0|bit2)` 后 `get` 得 `bit0|bit2`；再 `set(bit1)` 是**或上去**不是覆盖 |
| OR 等待 | `wait(bit1|bit3, OR)` 立刻满足；写回的 `got` **只有当时成立的那些位** |
| 不清就不丢 | 没有 `CLEAR` 选项时，交出去的位仍留在对象里 |
| AND 等待 | `wait(bit0|bit3, AND)` 缺一位 → `-2`（TIMEOUT）且 `got == 0`；凑齐 → `0` 且 `got == mask` |
| `CLEAR` 的粒度 | `wait(bit0|bit3, OR|CLEAR)` 之后，交出的那两位被清，**没交出的位（bit1|bit2）原样保留** |
| 精确清除 | `clear(bit1)` 只清 bit1；`clear(~bit2)` 只留 bit2 |
| 参数拒绝 | 既不给 OR 也不给 AND / 两个都给 / `mask == 0` / `p_got == NULL` / `set(mask 0)` / `get(NULL)` —— 六类全部 `-1`，**且对象状态一位不动** |
| 对象隔离 | 第二个 `create` 拿到**不同句柄**，对它 `set(bit3)` 不影响第一个对象 |
| 生命周期 | `delete` 返回 0；之后 `get`/`set`/`wait`/`delete` 全 `-1` |

最后两行是 `ESET: N pass, M fail` 与 `ESET: ALL PASS` / `ESET: FAILED`，
`main` 的返回值跟着 `fail` 走。

> **错误码口径**：`kernelsrc/include/arkrtos.h` 只导出 `arkrtos_eset_*` 函数与
> `ARKRTOS_ESET_OPT_*`，**没有**导出 `ARKRTOS_ESET_ERR_*` 这组名字；App 侧能用的
> 只有裸值。本文件的断言因此写成 `== -1 /* ERR_PARAM | ERR_OPT | ERR_STATE */`
> 与 `== -2 /* ARKRTOS_SYNC_ERR_TIMEOUT */`，并以「拒绝后状态不变」这类
> **副作用断言**兜住「返回码对但对象被改了」的退化。

## 实测（STM32F427，2026-11-15）

```
app install /eset_demo.svcapp 2
app install: fixed slot 2 -> 0x080A0000

app install: /eset_demo.svcapp
--- eset demo (SVC 0x21) ---
  PASS  create -> handle
handle = 26214400
  PASS  get -> 0
  PASS  fresh set has no bits
  PASS  set bit0|bit2 -> 0
  PASS  get -> bit0|bit2
  PASS  set bit1 -> 0 (accumulate)
  PASS  get -> bit0|bit1|bit2
  PASS  wait OR(bit1|bit3) -> 0
  PASS  wait OR writes back only the bits that were set
  PASS  OR without CLEAR leaves the bits in the object
  PASS  wait AND(bit0|bit3) -> TIMEOUT(-2)
  PASS  AND miss writes back 0
  PASS  wait AND(bit0|bit2) -> 0, got == mask
  PASS  set bit3 -> 0
  PASS  wait OR|CLEAR(bit0|bit3) -> got both
  PASS  CLEAR removed exactly the handed-over bits
  PASS  clear bit1 -> 0
  PASS  get -> bit2
  PASS  clear everything but bit2 -> 0
  PASS  bit2 survived
  PASS  wait with neither OR nor AND -> -1
  PASS  wait with both OR and AND -> -1
  PASS  wait mask 0 -> -1
  PASS  wait p_got NULL -> -1
  PASS  set mask 0 -> -1
  PASS  get NULL -> -1
  PASS  rejected calls did not touch the object
  PASS  second create -> distinct handle
  PASS  set bit3 on the second object
  PASS  second object -> bit3
  PASS  first object untouched by the second
  PASS  delete -> 0
  PASS  get after delete -> -1
  PASS  set after delete -> -1
  PASS  wait after delete -> -1
  PASS  delete twice -> -1
  PASS  delete the second object -> 0
ESET: 37 pass, 0 fail
ESET: ALL PASS
app install: ok, slot 1
```

`handle = 26214400` = `0x01900000`，是 `arkrtos_eset_create()` 的句柄编码，
App 侧只当不透明值用。

体积：`Code=4368 RO-data=1188 RW-data=0 ZI-data=4104`（2 条 warning 来自 AC6
`<stdio.h>` 的 `nonnull` 属性，与 `MPOOL_DEMO` 同一条）；打包后
`build/eset_demo.svcapp` 6060 B（负载 5556 B），重定位表 62 项，
RAM 声明 8192 B（RW/ZI 8 + 栈 4096 = 4104）。

同一次烧录的内核里，全量单测仍是 `11 matched, 11 passed, 0 failed`、
`assertions 129 run / 0 failed`——开 `ARKRTOS_USE_ESET` 没有改变任何既有用例。

## 编译、打包、上板

```bash
# 0. 内核侧先开开关（本板已在 board/stm32f427/arkrtos_board_config.h 里写好）
#    #define ARKRTOS_USE_ESET 1 ；#define ARKRTOS_ESET_NUM 4（原因见下面的口径提醒）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -b \
    example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx -j0 -o C:/tmp/kernel.log
env -u PYTHONHOME -u PYTHONPATH py -3 -m pyocd flash -t stm32f427igtx \
    example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST/ARKRTOS_TEST.axf

# 1. 编译 App（0 err）
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -b \
    example/stm32f427/app_sdk/ESET_DEMO/MDK-ARM/eset_demo.uvprojx -j0 -o C:/tmp/eset_app.log

# 2. 打包（四遍差分链接，打包期就证明镜像可重定位）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/pack_app.py \
    --project example/stm32f427/app_sdk/ESET_DEMO/MDK-ARM/eset_demo.uvprojx \
    --type app --name ESET_DEMO --version 1.0.0 --out build/eset_demo.svcapp

# 3. 挂载 NOR 卷并送进镜像（先 fs mount，否则设备回 not mounted）
#    控制台：fs mount
MSYS_NO_PATHCONV=1 env -u PYTHONHOME -u PYTHONPATH py -3 tools/fs_put.py \
    --port COM21 --path /eset_demo.svcapp build/eset_demo.svcapp

# 4. 装到固定槽位 2（auto=0，装完立刻自跑一轮并退场）
#    控制台：app install /eset_demo.svcapp 2
```

镜像带自启标志，`app install` 成功后立刻跑完一轮并把读数打到控制台；
任务结束即退场，不存在常驻循环。`app list` 里的 `id` 列是**分区表的记录号**
（从 0 连续分配），不是布局槽号——上板时是 `app install ... 2`，回报
`ok, slot 1`，`pool` 的 `fixed slots` 表里 `2 app 0x080A0000 RUNNING`。
要停/卸它请用 `app list` 给出的那个 `id`。

## 口径提醒

1. **`ARKRTOS_ESET_NUM = 4` 是本板为了塞进 120 KB 内核 RAM 做的降配**，
   不是内核默认的 8。F427 测试镜像开 eset 前几乎顶满（`RW 944 + ZI 121924`），
   默认 8 个对象（864 B）直接 `L6406E` ×48 链不上；降到 4 个（432 B）后
   `0 Error`，余量约 84 B。**降的只是"同时存在几个事件集"，
   每个对象的等待槽（`ARKRTOS_MAX_ESET_WAITERS`）仍是 4。**
2. **`eset` 的 ISR 入口（`arkrtos_eset_set_from_isr()`）仍无板级读数**：
   oslib 侧它就是转发到 `set`，本工程没有像 `mq_from_isr_contract` 那样
   用 EXTI 真 ISR 去测它。档位就是"编译 + 宿主证据"。
3. **用户态等待不登记等待者**（见文档 §5.4 的分界说明）：本工程在
   `timeout = 0` 下测的是"判条件 + 消费"这一层，没有测超时轮询的
   实际延时分布；`-2` 是 oslib 在首轮 `WOULDBLOCK` 后直接翻译出来的。
