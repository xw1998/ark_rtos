#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tickless idle 的门禁：宿主算账 + ON 档真编译。

两段，缺一不可：

1) 宿主算账
   编 kernelsrc/tests/test_tickless_host.c 与真实的 kernelsrc/src/arkrtos_tickless.c，
   把 SysTick 与两个"最近事件"查询换成假实现，跑 plan/split 的边界、睡满
   一条长节拍、以及连续 40 次"非整拍提前唤醒"之后的累计拍数。
   最后这条是关键：它把"跑一天不会慢"变成可复算的等式
   累计拍数 == 总计数 / 每拍计数。

2) ON 档真编译
   ARKRTOS_USE_TICKLESS 默认是 0，所以普通构建里这一整套代码全是死代码，
   语法错误、类型错误、漏声明都不会被发现——这就是"闸门假绿"的典型形态。
   这里用 armclang -fsyntax-only 以 -DARKRTOS_USE_TICKLESS=1 把开关按下去，
   再编译三个真正的接入点：tickless.c（本模块）、task.c（tick 包装与
   延时链查询）、F427 的 main.c（idle 循环）。任一编不过就是 FAIL。

不覆盖什么：真硬件行为。SysTick 的 COUNTFLAG/VAL 真实读序、SysTick 中断
优先级、WFI 的实际功耗都要上板；宿主与语法全绿不等于板端已验证。

用法
----
    python tools/tickless_host_gate.py
    python tools/tickless_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe
    python tools/tickless_host_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 有用例失败或编不出来。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_tickless_host.c")
TL_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_tickless.c")
TASK_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_task.c")
MAIN_C = os.path.join(ROOT, "example", "stm32f427", "kernel", "ARKRTOS_TEST",
                      "Core", "Src", "main.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]
ARMCLANG_FALLBACKS = [
    r"D:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
    r"C:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
]

# 与 F427 内核工程同一套头文件搜索路径（只取内核/板级/配置这几层）
INCLUDES = [
    "kernelsrc/include",
    "kernelsrc/components/mdk_trace",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/components/ark_softbus/include",
    "board/stm32f427",
    "config",
]
MAIN_INCLUDES = INCLUDES + [
    "example/stm32f427/kernel/ARKRTOS_TEST/Core/Inc",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Include",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/STM32F4xx/Include",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/STM32F4xx_HAL_Driver/Inc",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/STM32F4xx_HAL_Driver/Inc/Legacy",
]

def find_tool(explicit, candidates):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in candidates:
        if os.path.sep in cand:
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None

def run(cmd, env=None):
    r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                       encoding="utf-8", errors="replace")
    return r

def host_stage(gcc):
    td = tempfile.mkdtemp(prefix="arkrtos-tickless-gate-")
    exe = os.path.join(td, "tickless_host.exe" if os.name == "nt" else "tickless_host")
    cmd = [gcc]
    if "mingw" in gcc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(gcc))
    cmd += ["-std=gnu11", "-DARKRTOS_USE_TICKLESS=1", "-DARKRTOS_USE_TIMER=1"]
    cmd += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, TL_C]

    env = dict(os.environ)
    env["PATH"] = os.path.dirname(gcc) + os.pathsep + env.get("PATH", "")

    r = run(cmd, env)
    if r.returncode != 0:
        print("host build failed:\n" + (r.stderr or "")[-1500:])
        return False

    r = run([exe], env)
    txt = (r.stdout or "") + (r.stderr or "")
    print(txt.strip())
    shutil.rmtree(td, ignore_errors=True)
    return (r.returncode == 0) and ("TICKLESS-HOST OK" in txt)

def on_path_stage(armclang):
    """把开关按下去，真编译三个接入点（只做语法/类型检查，不链接）。"""
    ok = True
    jobs = [
        ("tickless-c", TL_C, INCLUDES, []),
        ("task-c", TASK_C, INCLUDES, []),
        ("main-c", MAIN_C, MAIN_INCLUDES, ["-DSTM32F427xx", "-DUSE_HAL_DRIVER"]),
    ]
    for name, src, incs, extra in jobs:
        if not os.path.exists(src):
            print("on-path %-12s source missing: %s" % (name, src))
            ok = False
            continue
        cmd = [armclang, "--target=arm-arm-none-eabi", "-mcpu=cortex-m4",
               "-std=c99", "-fsyntax-only", "-DARKRTOS_USE_TICKLESS=1"]
        cmd += ["-I" + os.path.join(ROOT, p) for p in incs]
        cmd += extra + [src]
        r = run(cmd)
        if r.returncode != 0:
            print("on-path %-12s FAIL\n%s" % (name, (r.stderr or "")[-1200:]))
            ok = False
        else:
            print("on-path %-12s ok" % name)
    return ok

def main():
    ap = argparse.ArgumentParser(description="Host + on-path gate for tickless idle")
    ap.add_argument("--gcc", help="host gcc 可执行文件（默认自动找）")
    ap.add_argument("--armclang", help="armclang 可执行文件（默认找 Keil 安装）")
    args = ap.parse_args()

    for path in (TEST_C, TL_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    gcc = find_tool(args.gcc, GCC_FALLBACKS)
    if not gcc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1
    print("gcc: %s" % gcc)
    if not host_stage(gcc):
        print("TICKLESS-GATE FAIL (host)")
        return 1

    armclang = find_tool(args.armclang, ARMCLANG_FALLBACKS)
    if not armclang:
        print("no armclang found (use --armclang)", file=sys.stderr)
        print("TICKLESS-GATE FAIL (on-path: compiler missing)")
        return 1
    print("armclang: %s" % armclang)
    if not on_path_stage(armclang):
        print("TICKLESS-GATE FAIL (on-path)")
        return 1

    print("TICKLESS-GATE OK")
    return 0

if __name__ == "__main__":
    sys.exit(main())
