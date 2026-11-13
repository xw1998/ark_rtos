# MINI_HANDOVER：小程序正常退出 = 计划内交接

这份例程是 `docs/欠账盘点.md` §一那条欠账的判据实现：

> 小程序正常退出后 `hwsafe` 报 `handover=1` 且设备未被中和

它和「崩溃遗言」是**相反**的两个动作：

| 退出方式 | 内核动作 | `hwsafe` 读数 |
|---|---|---|
| fault / 被强杀 | 把死者名下的硬件驱动到安全态 | `neutralized` +1 |
| 正常 `return`（`ARKRTOS_MINI_EXIT_IS_HANDOVER`，默认 1） | 只摘台账，**不碰硬件** | `handover` +1，`neutralized` 不动 |

理由在 `docs/小程序设计.md` §9.4：无缝升级里马上接手的新版本会重新 `open` 同一台设备，
此刻把它打到安全态是抖动而不是安全。

## 程序做了什么

`Src/mini_handover.c`：

1. `arkrtos_dev_open("led2", 0)` —— led0/led1 被内核任务 1/2 占着，led2 平时无所有者；
2. `arkrtos_dev_ctrl(led, 1u, 0u)` 点亮（`ARKRTOS_LED_CTRL_ON = 1`，led2 = PC2，低电平点亮）；
3. 原地待 3 秒（`arkrtos_task_delay`，参数是微秒）——留出**目视**窗口；
4. **故意不 `close`**，打印后从 `main` 正常 `return 0`。

第 4 步是判据的前提：LED 驱动的 `close()` 会把灯关掉，退出那一刻台账就空了；
只有「不 close 直接 return」才构造出「退出时仍持有设备」的状态。
若先 close，退出时无台账可摘，`handover` 不会动，用例等于白跑。

## 实测（STM32F427，2026-11-13）

| 项 | 手段 | 读数 |
|---|---|---|
| 基线 | `hwsafe` | `enabled, devs=3 owners=3 neutralized=0 (will=0 cb=0 none=0 fail=0 handover=0)`；own = led0(task1) / led1(task2) / ttyS0(task6) |
| 运行 | `mini run /mh.svcm` | 见下方串口原文 |
| 交接计数 | 再发 `hwsafe` | `... neutralized=0 (will=0 cb=0 none=0 fail=0 handover=1)`；led2 的 own 条目消失，无新条目 |
| 两块回收 | `mini stat` | `mini: none running` |

串口原文：

```
MINI_HANDOVER: mini starts (planned hand-over probe)
MINI_HANDOVER: led2 handle=18874370 ctrl(ON)=0
MINI_HANDOVER: holding, round 1
MINI_HANDOVER: holding, round 2
MINI_HANDOVER: holding, round 3
MINI_HANDOVER: returning from main, device still held
mini run: /mh.svcm loaded but no longer running (a program that exits on its own looks the same; 'dmesg' shows which)
```

体积：`Code=2408 RO-data=272 RW-data=0 ZI-data=4096`；打包后
`build/mini_handover.svcapp` 2980 B（负载 2680 B），重定位表 11 项，
代码块 4096 B + RAM 块 8192 B，镜像**不自启**。

> **口径**：判据必须成对读 —— `handover` +1 **且** `neutralized` 不变。
> 单独看 `neutralized=0` 不算过（没有台账可摘时它同样不动）。

## 为什么没有用 GPIO / LED 当证据

试过用 `pyocd` 读 `GPIOC_ODR` 证明"灯还亮着"，这条路在本机**不能用**：

- 目标**运行时**读外设返回常量假值：`RCC_CR` / `FLASH_OPTCR` / `GPIOC_ODR` 三个不同地址
  返回同一个 `0x03000000`；`halt` 之后才是真值（`OPTCR=0x0FFFAAED`、`ODR=0x00000006`）。
- `halt` 本身会干扰目标：几次 `halt → read → go` 之后 `hwsafe` 的 `handover` 回到 0、
  `tick.isr` 从头计数 —— 目标在调试期间被复位过（看门狗）。

所以瞬时状态只认内核自己记的 `handover` / `neutralized` 与台账条目；LED 目视只能当人的辅助，
不能进判据。详见 [已知限制](../../../../docs/已知限制.md) §五。

## 编译、打包、上板

```bash
# 1. 编译
env -u PYTHONHOME -u PYTHONPATH "D:/Keil_v5/UV4/UV4.exe" -b \
    example/stm32f427/app_sdk/MINI_HANDOVER/MDK-ARM/mini_handover.uvprojx -j0 -o C:/tmp/mh.log

# 2. 打包（miniapp 类型：不自启，失败也不计入 App 槽位）
env -u PYTHONHOME -u PYTHONPATH py -3 tools/pack_app.py \
    --project example/stm32f427/app_sdk/MINI_HANDOVER/MDK-ARM/mini_handover.uvprojx \
    --type miniapp --no-autostart --name MINI_HANDOVER --version 1.0.0 \
    --out build/mini_handover.svcapp

# 3. 送进 NOR 卷（注意：小程序走 fs 卷，不是 VFS 的 /）
MSYS_NO_PATHCONV=1 env -u PYTHONHOME -u PYTHONPATH py -3 tools/fs_put.py \
    --port COM21 --path /mh.svcm build/mini_handover.svcapp

# 4. 取证：先记基线，再跑，再读
#    hwsafe
#    mini run /mh.svcm
#    hwsafe
#    mini stat
```

## 未验证

- `ARKRTOS_MINI_EXIT_IS_HANDOVER` 置 **0**（正常退出也回安全态）这一档没上板 ——
  需要另编一版内核，本轮板上的内核是默认档（该宏 = 1）。
