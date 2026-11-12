#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""App / 小程序 SDK 的 stdio 宿主门禁。

编并跑 kernelsrc/sdk/app_sdk/tests/test_stdio_host.c —— 它把
kernelsrc/sdk/app_sdk/arkrtos_stdio.c 与一份桩 `arkrtos_shell_print()` 链在一起，
逐字节核对 `printf` / `puts` / `putchar` 的输出。

为什么要有它：这三条是用户源码里最常见的一句，错了就是「打出来是另一句话」，
在板上只能靠眼睛看。宿主上可以逐字节比对。

用法
----
    python tools/sdk_stdio_host_gate.py
    python tools/sdk_stdio_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "sdk", "app_sdk", "tests", "test_stdio_host.c")
STDIO_C = os.path.join(ROOT, "kernelsrc", "sdk", "app_sdk", "arkrtos_stdio.c")

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
    ap = argparse.ArgumentParser(description="Host gate for the SDK stdio shim")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, STDIO_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-stdio-gate-")
    exe = os.path.join(td, "stdio_host.exe" if os.name == "nt" else "stdio_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11", "-I" + os.path.join(ROOT, "kernelsrc", "include"),
            "-o", exe, TEST_C, STDIO_C]

    print("gcc: %s" % cc)
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode != 0:
        print("build failed", file=sys.stderr)
        return 1

    r = subprocess.run([exe], cwd=ROOT)
    ok = (r.returncode == 0)

    if not args.keep:
        shutil.rmtree(td, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
