#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""32 位事件集（ARKRTOS_USE_ESET）的门禁：把开关按下去，真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_ESET` 默认 0，而 F427/F401/H743 的 Keil 工程都按默认档编。
于是所有 `#if (ARKRTOS_USE_ESET == 1)` 里的代码平时一个编译器都不经过：

    kernelsrc/src/arkrtos_eset.c          事件集本体
    kernelsrc/src/arkrtos_task.c          SVC 0x21 处理与分派、任务下线收尸
    kernelsrc/src/arkrtos_init.c          开机初始化
    kernelsrc/sdk/app_sdk/arkrtos_oslib.c      用户态包装（SVC + 轮询等待）
    kernelsrc/sdk/driver_sdk/arkrtos_drv_oslib.c  驱动侧包装

改了其中任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。
这就是「默认关 = 没人编译」的假绿形态，跟 rtstat 那道门禁同源。

这里用 armclang `-fsyntax-only` 以 `-DARKRTOS_USE_ESET=1` 编上面这些文件。
不覆盖：链接、真机上 SVC 与 ISR 的实际行为——那些只有上板才知道。

用法
----
    python tools/eset_syntax_gate.py
    python tools/eset_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

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

# F427 工程真值打开的选配（lwIP / 软总线）需要额外头路径，否则报的是
# 「文件没找到」而不是被测开关的问题。
INCLUDES += EXTRA_INCLUDES

DEFS = [
    "ARKRTOS_USE_ESET=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

# 软总线档位宏：与 F427 工程 MiscControls 一致。
DEFS += SOFTBUS_TIER_DEFS

# (源文件, -mcpu, 额外宏)。F427 是 cortex-m4。
# 驱动侧 oslib 整份文件就是用户态封装，头文件里的错误码与 SVC 声明都在
# `#ifdef ARKRTOS_DRV_USER_MODE` 里，所以这个宏必须给。
TARGETS = [
    ("kernelsrc/src/arkrtos_eset.c", "cortex-m4", []),
    ("kernelsrc/src/arkrtos_task.c", "cortex-m4", []),
    ("kernelsrc/src/arkrtos_init.c", "cortex-m4", []),
    ("kernelsrc/sdk/app_sdk/arkrtos_oslib.c", "cortex-m4", []),
    ("kernelsrc/sdk/driver_sdk/arkrtos_drv_oslib.c", "cortex-m4",
     ["ARKRTOS_DRV_USER_MODE=1"]),
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
        print("ESET-GATE FAIL: armclang not found (a skip would be a false green)")
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
            print("ESET-GATE FAIL: %s (-mcpu=%s)" % (rel, cpu))
            print(out[-900:])
        else:
            print("ok: %s (-mcpu=%s)" % (rel, cpu))

    if bad:
        return 1
    print("ESET-GATE OK (%d files, USE_ESET=1)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
