#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ark make 的门禁：把开关按下去，真的编一遍四个文件。

为什么需要这一道
----------------
`ARKRTOS_USE_ARKMAKE` 默认 0，而且只有 H743 的 GCC 工程在
`USE_ARKMAKE=1` 时才会把组件编进去。`tools/ci_gate.py --build` 只跑 F427 与
F401 两个 Keil 工程，所以 **这两个文件和这条命令平时一个编译器都不经过**：
改了 `arkmake_shell_cmd.c` 之后门禁仍然全绿，直到有人拿 H743 打开开关才会
发现少了个分号。这就是"闸门假绿"的标准形态。

这里用 armclang -fsyntax-only，以与 H743 工程同一套 `-D` 和 `-I` 编四个文件：

    src/arkmake_parse.c         上游：文本 -> 工程描述（纯函数）
    src/arkmake_build.c         上游：工程描述 -> 一次构建
    arkmake_port_arkrtos.c      接线：读 / 日志 / 打包
    arkmake_shell_cmd.c         接线：make 命令（参数解析 + clean）

不覆盖什么：链接、板上行为、clean 是否真的删掉了哪个文件。语法全绿只说明
"打开了这个开关之后它能编过"。

用法
----
    python tools/arkmake_syntax_gate.py
    python tools/arkmake_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 有文件编不过或找不到编译器。
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

# 与 H743 GCC 工程同一套（去掉只给 main.c 用的 Core/Drivers 三层：
# 这四个文件不读 HAL 头）。
INCLUDES = [
    "kernelsrc/include",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/components/mdk_trace",
    "kernelsrc/components/arkc",
    "kernelsrc/components/arkc/include",
    "kernelsrc/components/arkmake",
    "kernelsrc/components/arkmake/include",
    "config",
]

# 三个开关必须一起给：arkrtos_features.h 里 arkmake 依赖 apppack 与 arkc，
# 缺一个就是 #error（那正是设计要的），但那样会把"接线写错"和"开关没给全"
# 混成同一条失败。
DEFS = [
    "ARKRTOS_USE_ARKMAKE=1",
    "ARKRTOS_USE_ARKC=1",
    "ARKRTOS_USE_APPPACK=1",
]

SOURCES = [
    "kernelsrc/components/arkmake/src/arkmake_parse.c",
    "kernelsrc/components/arkmake/src/arkmake_build.c",
    "kernelsrc/components/arkmake/arkmake_port_arkrtos.c",
    "kernelsrc/components/arkmake/arkmake_shell_cmd.c",
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
        print("ARKMAKE-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    base = [cc, "--target=arm-arm-none-eabi", "-mcpu=cortex-m4",
            "-std=c99", "-fsyntax-only"]
    base += ["-D" + d for d in DEFS]
    base += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel in SOURCES:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(base + [src], capture_output=True, encoding="utf-8",
                           errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("ARKMAKE-GATE FAIL: %s" % rel)
            print(out[-800:])
        else:
            print("ok: %s" % rel)

    if bad:
        return 1
    print("ARKMAKE-GATE OK (%d files, USE_ARKMAKE=1)" % len(SOURCES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
