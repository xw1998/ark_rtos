#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""实时性度量（ARKRTOS_USE_RTSTAT）的门禁：把开关按下去，真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_RTSTAT` 默认 0，而 F427/F401 两个 Keil 工程都按默认档编。
于是这五处 `#if (ARKRTOS_USE_RTSTAT == 1)` 里的代码平时一个编译器都不经过：

    kernelsrc/src/arkrtos_rt.c            测量本体（DWT 周期计数）
    kernelsrc/src/arkrtos_task.c          节拍入口/出口两个时间戳
    kernelsrc/src/arkrtos_init.c          开机武装计数器
    kernelsrc/shell/arkrtos_shell.c       `rt` 命令
    kernelsrc/port/arm/cortex-m{3,4,7}/   临界区钩子 + DWT 读写

改了其中任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。
这就是"默认关 = 没人编译"的假绿形态，跟 arkmake 那道门禁同源。

这里用 armclang `-fsyntax-only` 以 `-DARKRTOS_USE_RTSTAT=1` 编上面这些文件，
三个移植层各用自己那一档 `-mcpu`。不覆盖：链接、DWT 在板上的实际行为、
`rt` 命令的读法是否正确——这些只有上板才知道。

用法
----
    python tools/rtstat_syntax_gate.py
    python tools/rtstat_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

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

# 板级配置必须一起给：port 层用到的 SCB / __set_PRIMASK 来自 CMSIS core 头，
# 而那条链是 arkrtos_config.h -> ARKRTOS_BOARD_CONFIG -> stm32f4xx.h -> core_cm4.h。
# 少这个 -D，编出来就是"未声明的标识符"，而真机工程里它一直是被定义的。
DEFS = [
    "ARKRTOS_USE_RTSTAT=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    # 芯片选择与 HAL 驱动开关：不给这两个，stm32f4xx.h 会在选型处 #error，
    # 后面的 core_cm4.h 根本不会被包含，于是报出来的是"SCB 未声明"而不是真正原因。
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

# (源文件, -mcpu)。内核三个文件用 m4（F427 就是 m4），移植层各编各的。
TARGETS = [
    ("kernelsrc/src/arkrtos_rt.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_task.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_init.c", "cortex-m4"),
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
        print("RTSTAT-GATE FAIL: armclang not found (a skip would be a false green)")
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
            print("RTSTAT-GATE FAIL: %s (-mcpu=%s)" % (rel, cpu))
            print(out[-900:])
        else:
            print("ok: %s (-mcpu=%s)" % (rel, cpu))

    if bad:
        return 1
    print("RTSTAT-GATE OK (%d files, USE_RTSTAT=1)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
