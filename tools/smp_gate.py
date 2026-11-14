#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SMP 骨架（ARKRTOS_USE_SMP）的门禁：把开关按下去，编一遍并跑账目用例。

为什么需要这一道
----------------
`ARKRTOS_USE_SMP` 默认 0，F427/F401/H743 的 Keil 工程都按默认档编。于是
下面这些代码平时一个编译器都不经过：

    kernelsrc/include/arkrtos_smp.h            每核状态块与六个接口
    kernelsrc/src/arkrtos_smp.c                引导 / 上线 / IPI / 空转
    kernelsrc/src/arkrtos_init.c               SMP 引导调用点
    kernelsrc/include/arkrtos_hal.h            两个新 port 钩子的声明
    kernelsrc/port/arm/cortex-m{3,4,7}/*.c     两个钩子的单核桩

改了任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。这就是
"默认关 = 没人编译"的假绿形态，跟 eset / waitmulti / waitq 那几道同源。

两段验证：
  1. 宿主：编 kernelsrc/src/arkrtos_smp.c + kernelsrc/tests/test_smp_host.c，
     用可编程的 port 桩（-DARKRTOS_CPU_NUM=4）逐条对账——上线掩码、越界
     核号、启动请求被拒时不许虚报上线、IPI 只发在线核、当前核读错时的收敛、
     重复初始化清零。
  2. armclang -fsyntax-only 以 -DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2 编
     arkrtos_smp.c / arkrtos_init.c / 三个 Cortex-M 移植层，确认 ON 档与
     两个新 port 钩子在三份 port 里都编得过。

不覆盖（这个骨架本身也没有）：多核调度、每核运行队列、核间任务迁移、
真实 IPI 投递与延迟、多核板上的次核启动。本仓库当前没有任何多核板，
所以这一道的结论只能是"骨架自洽且编得过"，不是"支持多核"。

用法
----
    python tools/smp_gate.py
    python tools/smp_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe ^
                             --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 编不过或用例失败（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_smp_host.c")
SMP_C  = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_smp.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

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
    "ARKRTOS_USE_SMP=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

DEFS += SOFTBUS_TIER_DEFS

TARGETS = [
    "kernelsrc/src/arkrtos_smp.c",
    "kernelsrc/src/arkrtos_init.c",
    "kernelsrc/port/arm/cortex-m3/arkrtos_port.c",
    "kernelsrc/port/arm/cortex-m4/arkrtos_port.c",
    "kernelsrc/port/arm/cortex-m7/arkrtos_port.c",
]

def find_gcc(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in GCC_FALLBACKS:
        if os.path.sep in cand:
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None

def find_armclang(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in ARMCLANG_FALLBACKS:
        if os.path.exists(cand):
            return cand
    return shutil.which("armclang")

def run_host(cc):
    td = tempfile.mkdtemp(prefix="arkrtos-smp-gate-")
    exe = os.path.join(td, "smp_host.exe" if os.name == "nt" else "smp_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11"]
    cmd += ["-D" + d for d in DEFS]
    cmd += ["-DARKRTOS_CPU_NUM=4"]
    cmd += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, SMP_C]

    r = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("SMP-GATE FAIL: host build")
        print(((r.stdout or "") + (r.stderr or ""))[-900:])
        shutil.rmtree(td, ignore_errors=True)
        return False

    r = subprocess.run([exe], cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    print(out)
    shutil.rmtree(td, ignore_errors=True)
    return r.returncode == 0 and "SMP-HOST OK" in out

def run_syntax(cc):
    common = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only",
              "-mcpu=cortex-m4"]
    common += ["-D" + d for d in DEFS]
    common += ["-DARKRTOS_CPU_NUM=2"]
    common += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel in TARGETS:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + [src], capture_output=True,
                           encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("SMP-GATE FAIL: %s" % rel)
            print(out[-900:])
        else:
            print("ok: %s" % rel)
    return bad == 0

def main():
    ap = argparse.ArgumentParser(description="Host + syntax gate for the SMP scaffolding")
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--armclang", default=None)
    args = ap.parse_args()

    cc = find_gcc(args.gcc)
    if not cc:
        print("SMP-GATE FAIL: no host C compiler found (use --gcc)")
        return 1
    ac = find_armclang(args.armclang)
    if not ac:
        print("SMP-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    print("gcc: %s" % cc)
    if not run_host(cc):
        return 1
    if not run_syntax(ac):
        return 1

    print("SMP-GATE OK (host accounting + %d ON-path files, CPU_NUM=4/2)" % len(TARGETS))
    return 0

if __name__ == "__main__":
    sys.exit(main())
