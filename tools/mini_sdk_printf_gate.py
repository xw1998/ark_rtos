#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""小程序 SDK 的 printf 宿主门禁：编 + 跑 kernelsrc/sdk/mini_sdk/tests/test_printf_host.c

为什么要有它：printf 的错法基本都是「打出来是另一句话」——宽度差一格、负数
少个号、宽度算错把数字吃掉。这类错在板上只能靠眼睛看，而眼睛看不出来。所以
把格式化主体拿到宿主上，用一批固定用例逐字比对。

**必须用 -m32 编**：printf 的槽位是 32 位的（板上就是这么走的），64 位宿主上
指针会被截断，测出来的东西和板上不是一回事。

用法
----
    python tools/mini_sdk_printf_gate.py
    python tools/mini_sdk_printf_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "sdk", "mini_sdk", "tests", "test_printf_host.c")

# MinGW 不总在 PATH 里：这台机器上它在 D:/mingw/MinGW/bin。只在这里补，不改调用方的环境。
GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
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


def main():
    ap = argparse.ArgumentParser(description="Host gate for the mini SDK printf")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    if not os.path.exists(TEST_C):
        print("test source not found: %s" % TEST_C, file=sys.stderr)
        return 1

    gcc = find_gcc(args.gcc)
    if gcc is None:
        print("gcc not found; pass --gcc <path>", file=sys.stderr)
        return 1

    out_dir = tempfile.mkdtemp(prefix="mini_pf_")
    exe = os.path.join(out_dir, "printf_gate.exe")
    cmd = [gcc, "-m32", "-std=c99", "-Wall", "-Wextra",
           "-Wno-unused-parameter", "-o", exe, TEST_C]
    print("building: %s" % " ".join(cmd))
    rc = subprocess.call(cmd)
    if rc != 0:
        print("build failed (%d)" % rc, file=sys.stderr)
        return 1

    print("running: %s" % exe)
    rc = subprocess.call([exe])
    if rc != 0:
        print("printf gate FAILED (%d)" % rc, file=sys.stderr)
        return 1
    print("printf gate: all cases PASS")
    if not args.keep:
        try:
            os.remove(exe)
            os.rmdir(out_dir)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
