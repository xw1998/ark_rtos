#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""安全基线自检（ARKRTOS_USE_SECCHECK）的门禁：把开关按下去，真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_SECCHECK` 默认 0。`arkrtos_sec.c` 的全部实体、`arkrtos_sec.h` 里
三个接口的声明、以及 shell 里的 `cmd_sec` 与命令表那一行，平时一个编译器都不经过。
于是"改了却没编过"这种错误会一直全绿，直到认证那天打开开关才发现编不过——
和 rtstat / arkmake 两道门禁同一个假绿形态（默认关 = 没人编译）。

这里用 armclang `-fsyntax-only` 以 `-DARKRTOS_USE_SECCHECK=1` 编两个文件：
自检本体与 shell 接线。不覆盖：链接、12 项判据在板上是否给出正确结论、
`/` 命令表预算是否真的不溢出——那些要上板（F427 上已实测，见
docs/安全基线自检.md §4）。

用法
----
    python tools/seccheck_syntax_gate.py
    python tools/seccheck_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

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
    "kernelsrc/components/ark_softbus/include",
    "kernelsrc/components/lwip/src/include",
    "kernelsrc/components/lwip/port",
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
    "ARKRTOS_USE_SECCHECK=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

# shell 那一档把可选组件都打开：`cmd_sec` 的位置在 rt 之后、ps 之前，
# 关着的组件会让命令表分支少一半，编过去也说明不了问题。
DEFS_SHELL = DEFS + [
    "ARKRTOS_USE_RTSTAT=1",
    "ARKRTOS_HW_SAFE=1",
    "ARKRTOS_USE_UTEST=1",
]

TARGETS = [
    ("kernelsrc/src/arkrtos_sec.c", "cortex-m4", DEFS),
    ("kernelsrc/shell/arkrtos_shell.c", "cortex-m4", DEFS_SHELL),
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
        print("SECCHECK-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    base = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only"]
    base += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel, cpu, defs in TARGETS:
        src = os.path.join(ROOT, rel)
        cmd = base + ["-D" + d for d in defs] + ["-mcpu=" + cpu, src]
        r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("SECCHECK-GATE FAIL: %s (-mcpu=%s)" % (rel, cpu))
            print(out[-900:])
        else:
            print("ok: %s (-mcpu=%s)" % (rel, cpu))

    if bad:
        return 1
    print("SECCHECK-GATE OK (%d files, USE_SECCHECK=1)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
