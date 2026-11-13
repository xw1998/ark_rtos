#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""任务热切换（ARKRTOS_USE_TASK_SWAP）的门禁：把开关按下去，真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_TASK_SWAP` 默认 0，而 F427/F401 两个 Keil 工程都按默认档编。
于是这几处 `#if (ARKRTOS_USE_TASK_SWAP == 1)` 里的代码平时一个编译器都不经过：

    kernelsrc/src/arkrtos_task.c    热切换本体 + SVC_Server 里的落地钩子
    kernelsrc/src/arkrtos_mini.c    `mini swap`（第二对 RAM 块 + 切换后归还旧块）
    kernelsrc/src/arkrtos_loader.c  `app swap`（第二个 Flash 槽 + 切换后改槽位状态）
    kernelsrc/shell/arkrtos_shell.c 两条命令与命令行解析
    kernelsrc/port/arm/cortex-m{3,4,7}/   PSP/MPU 写入口（只用到既有接口）

改了其中任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。
这就是"默认关 = 没人编译"的假绿形态，跟 rtstat / arkmake 那两道门禁同源。

这里用 armclang `-fsyntax-only` 以 `-DARKRTOS_USE_TASK_SWAP=1` 编上面这些文件，
三个移植层各用自己那一档 `-mcpu`。不覆盖：链接、栈帧改写之后在板上的实际行为、
`mini swap` / `app swap` 的读数——这些只有上板才知道。

用法
----
    python tools/swap_syntax_gate.py
    python tools/swap_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 有文件编不过或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys

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

DEFS = [
    "ARKRTOS_USE_TASK_SWAP=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

# (源文件, -mcpu)。内核文件用 m4（F427 就是 m4），移植层各编各的。
TARGETS = [
    ("kernelsrc/src/arkrtos_task.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_mini.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_loader.c", "cortex-m4"),
    ("kernelsrc/shell/arkrtos_shell.c", "cortex-m4"),
    ("kernelsrc/port/arm/cortex-m3/arkrtos_port.c", "cortex-m3"),
    ("kernelsrc/port/arm/cortex-m4/arkrtos_port.c", "cortex-m4"),
    ("kernelsrc/port/arm/cortex-m7/arkrtos_port.c", "cortex-m7"),
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
        print("SWAP-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    common = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only"]
    common += ["-D" + d for d in DEFS]
    common += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel, cpu in TARGETS:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + ["-mcpu=" + cpu, src],
                           capture_output=True, encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("SWAP-GATE FAIL: %s (-mcpu=%s)" % (rel, cpu))
            print(out[-1200:])
        else:
            print("ok: %s (-mcpu=%s)" % (rel, cpu))

    if bad:
        return 1
    print("SWAP-GATE OK (%d files, USE_TASK_SWAP=1)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
