#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""等待者容量（ARKRTOS_USE_WAIT_UNBOUNDED）的门禁。

这个开关把每个对象的等待槽位从固定 4 放大到任务表容量。它默认关，所以
打开后的那套代码在默认构建里一个编译器都不过：
    kernelsrc/include/arkrtos_sync.h        sem / mtx / cond 的排队数组
    kernelsrc/include/arkrtos_mq.h          mq 的收 / 发两条排队数组
    kernelsrc/include/arkrtos_sbuf.h        sbuf 的收 / 发两条排队数组
    kernelsrc/include/arkrtos_event.h       event 的排队数组
    kernelsrc/include/arkrtos_config.h      派生 ARKRTOS_WQ_SLOTS
如果只改了数组尺寸而漏了某处的循环上界，默认档一切正常、打开后静默越界
或永远填不满 —— 这正是「默认关 = 没人编译」的假绿形态。

两段验证：
  1. armclang -fsyntax-only 以 ARKRTOS_USE_WAIT_UNBOUNDED=1 编 sync / mq /
     sbuf / event / waitmulti，确认五个模块在新尺寸下都编得过；
  2. 用同一开关跑 sync 宿主用例（76 条断言），确认槽位账目与令牌守恒在
     48 槽下仍然成立，并打印实际槽位数。

不覆盖：真实调度器下的排队与唤醒时序、板上 RAM 余量是否够（打开后所有
对象表一起线性变大，本板 ARKRTOS_TASK_MAX_NUM 个槽 × 各表排队数）。

用法
----
    python tools/waitq_gate.py
    python tools/waitq_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 编不过或用例失败（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys

from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ARMCLANG_FALLBACKS = [
    r"D:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
    r"C:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
]

INCLUDES = [
    "kernelsrc/include",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/components/mdk_trace",
    "kernelsrc/components/arkc/include",
    "kernelsrc/components/arkmake/include",
    "kernelsrc/sdk",
    "config",
    "config/stm32f427",
    "board/stm32f427",
    "example/stm32f427/kernel/ARKRTOS_TEST/Core/Inc",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/STM32F4xx_HAL_Driver/Inc",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/STM32F4xx/Include",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Core/Include",
]

INCLUDES += EXTRA_INCLUDES

DEFS = [
    "ARKRTOS_USE_WAIT_UNBOUNDED=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

DEFS += SOFTBUS_TIER_DEFS

TARGETS = [
    "kernelsrc/src/arkrtos_sync.c",
    "kernelsrc/src/arkrtos_mq.c",
    "kernelsrc/src/arkrtos_sbuf.c",
    "kernelsrc/src/arkrtos_event.c",
    "kernelsrc/src/arkrtos_waitmulti.c",
]


def find_armclang(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in ARMCLANG_FALLBACKS:
        if os.path.exists(cand):
            return cand
    return shutil.which("armclang")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--armclang", default=None)
    args = ap.parse_args()

    cc = find_armclang(args.armclang)
    if not cc:
        print("WAITQ-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    common = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only",
              "-mcpu=cortex-m4"]
    common += ["-D" + d for d in DEFS]
    common += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel in TARGETS:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + [src], capture_output=True,
                           encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("WAITQ-GATE FAIL: %s" % rel)
            print(out[-900:])
        else:
            print("ok: %s" % rel)

    if bad:
        return 1

    r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "sync_host_gate.py"),
                        "--unbounded"],
                       cwd=ROOT, capture_output=True, encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode != 0 or "SYNC-HOST OK" not in out:
        print("WAITQ-GATE FAIL: host run with the switch ON")
        print(out[-900:])
        return 1
    for line in out.splitlines():
        if line.startswith("WQ-SLOTS="):
            print(line.strip())

    print("WAITQ-GATE OK (%d files, waiters = task table size)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
