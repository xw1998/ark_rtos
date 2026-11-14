#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多对象同时就绪探测（ARKRTOS_USE_WAITMULTI）的门禁：把开关按下去，真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_WAITMULTI` 默认 0，F427/F401/H743 的 Keil 工程都按默认档编。
于是所有 `#if (ARKRTOS_USE_WAITMULTI == 1)` 里的代码平时一个编译器都不经过：

    kernelsrc/src/arkrtos_waitmulti.c          探测本体（分派 + ANY/ALL）
    kernelsrc/src/arkrtos_sync.c               sem/mtx 的 ready 查询
    kernelsrc/src/arkrtos_mq.c                 mq 的 ready 查询（含关档桩）
    kernelsrc/src/arkrtos_event.c              event 的 ready 查询
    kernelsrc/src/arkrtos_task.c               SVC 0x22 处理与分派
    kernelsrc/sdk/app_sdk/arkrtos_oslib.c      用户态包装（SVC + 轮询等待）

改了其中任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。
这就是「默认关 = 没人编译」的假绿形态，跟 eset / rtstat 那两道同源。
宿主门禁（waitmulti_host_gate.py）只编探测本体，这条把其余五个文件一起编。

不覆盖：链接、真机上 SVC 的实际行为、轮询唤醒延迟——那些只有上板才知道。

用法
----
    python tools/waitmulti_syntax_gate.py
    python tools/waitmulti_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 有文件编不过或找不到编译器（缺编译器是 FAIL，不是跳过）。
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
    "ARKRTOS_USE_WAITMULTI=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

DEFS += SOFTBUS_TIER_DEFS

TARGETS = [
    ("kernelsrc/src/arkrtos_waitmulti.c", "cortex-m4", []),
    ("kernelsrc/src/arkrtos_sync.c", "cortex-m4", []),
    ("kernelsrc/src/arkrtos_mq.c", "cortex-m4", []),
    ("kernelsrc/src/arkrtos_event.c", "cortex-m4", []),
    ("kernelsrc/src/arkrtos_task.c", "cortex-m4", []),
    ("kernelsrc/sdk/app_sdk/arkrtos_oslib.c", "cortex-m4", []),
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
        print("WM-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    common = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only"]
    common += ["-D" + d for d in DEFS]
    common += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel, cpu, extra in TARGETS:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + ["-D" + d for d in extra] + ["-mcpu=" + cpu, src],
                           capture_output=True, encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("WM-GATE FAIL: %s (-mcpu=%s)" % (rel, cpu))
            print(out[-900:])
        else:
            print("ok: %s (-mcpu=%s)" % (rel, cpu))

    if bad:
        return 1
    print("WM-GATE OK (%d files, USE_WAITMULTI=1)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
