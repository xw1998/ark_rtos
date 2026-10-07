# Ark RTOS 硬件安全态（"遗言"）机制验证镜像（HWSAFE_DEMO）

**这不是示例工程，是硬件安全态机制的上板验证镜像。**

它和普通 App 一样以**非特权态**任务运行，流程是：

1. 打开 `led0`（PC0）并**点亮**；
2. 用 `arkrtos_dev_set_will("led0", 0, 0)` 留一句**遗言**——
   "我死后请对 led0 执行 `drv_ctrl(cmd=0)`（关灯）"；
3. 故意越权写内核私有 RAM（`0x20003000`）→ MemManage → 任务被故障恢复杀掉；
4. **内核**在收尸路径执行该遗言 → LED 被强制关掉。

关键点：第 4 步是**内核**做的，不需要崩溃的 App 再跑一行代码——这正是
[`docs/App崩溃硬件安全与遗言机制.md`](../../../../docs/App崩溃硬件安全与遗言机制.md) 要解决的问题。

## 目录结构

```
HWSAFE_DEMO/
├── MDK-ARM/
│   └── hwsafe_demo.uvprojx   # Keil MDK 工程（scatter 由 BeforeMake 钩子生成）
└── Src/
    └── hwsafe_demo.c         # 探针主体，含 AppMain() 入口
```

## 用法

```bash
py -3 tools/pack_app.py --project example/stm32f427/app_sdk/HWSAFE_DEMO/MDK-ARM/hwsafe_demo.uvprojx \
      --type app --name HWSAFE_DEMO --out build/HWSAFE_DEMO/HWSAFE_DEMO.svcapp
py -3 tools/fs_put.py build/HWSAFE_DEMO/HWSAFE_DEMO.svcapp --port COM3 --path /HWSAFE_DEMO.svcapp

# 设备 shell：
#   fs mount
#   app install /HWSAFE_DEMO.svcapp
#   hwsafe         <- 期望 neutralized>=1、last_owner=<本槽任务号>
#   fault          <- 期望看到 MEMFAULT + RECOVER，task 指向本槽
#   app list       <- 期望 crash 计数 +1，槽仍 RUNNING（连续 3 次才 INVALID + held）
```

**目视**：探针点亮 led0 后崩溃，随后应看到 led0 **灭**——这是"遗言"被内核执行的直接证据。

## 前置条件

- 内核必须 `ARKRTOS_HW_SAFE=1`（F427 内核工程已在 `<Define>` 里打开）；
  否则 `arkrtos_dev_set_will()` 返回 `-1`，探针会打印 `will REFUSED`。
- 板级已为 led0/led1/led2 登记安全态回调（`arkrtos_board.c`），
  且 `led_drv_ctrl` 实现了 `0=off / 1=on / 2=toggle`。

## 已知边界

- 探针**每次被启动**都会崩溃一次。内核不会自动重启崩溃的 App，需要复验时手动
  `app start <slot>`；同一槽连续 3 次故障会被终局策略 held（`app start` 可再放行）。
- `led0/1/2` 都共享同一个 `led_drv`，三者的安全态回调是同一个函数；`led_drv_safe`
  在 owner 死亡时把灯关掉。
- 本镜像不看门狗心跳，整机是否健康请在 shell 侧看。
