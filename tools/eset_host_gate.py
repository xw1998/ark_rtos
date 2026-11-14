#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""32 位事件集（arkrtos_eset）的宿主门禁。

编并跑 kernelsrc/tests/test_eset_host.c 与 kernelsrc/src/arkrtos_eset.c。
测试文件里自带内核桩（调度器、临界区、trace、port 全部空实现），所以这里
只需要一份宿主 C 编译器，不需要板子。

为什么值得跑：事件集的缺陷几乎都在「判定与交付」里——OR/AND 判定用错位、
交给等待者的值在醒来后被重算、带 CLEAR 的位被两个等待者各拿一份、超时
路径留下幽灵登记、删除不把等待者放走。宿主上可以逐条断言，板子上只能靠
串口看现象。

编译宏：
  -DARKRTOS_USE_ESET=1       把模块真编进来（默认是关的，源文件为空对象）
  -DARKRTOS_ESET_HOST_TEST   让对象表从 static 变外部可见，测试才能摆等待槽

已知边界：真实调度器下的优先级与唤醒时序（ready_add / PendSV 全为空实现）、
用户态 SVC 轮询路径（sync_in_handler 恒 0）、板上中断里 set 的实际延迟
都未被覆盖。不要把这条门禁读成「整个模块已验证」。

用法
----
    python tools/eset_host_gate.py
    python tools/eset_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_eset_host.c")
ESET_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_eset.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

# 头文件搜索路径：和 ARM 侧编译同一套（板级配置决定 TASK_MAX_NUM 等）
INCLUDES = [
    os.path.join(ROOT, "kernelsrc", "include"),
    os.path.join(ROOT, "kernelsrc", "components", "mdk_trace"),
    os.path.join(ROOT, "board", "stm32f427"),
    os.path.join(ROOT, "config"),
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
    ap = argparse.ArgumentParser(description="Host gate for the 32-bit event set")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, ESET_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-eset-gate-")
    exe = os.path.join(td, "eset_host.exe" if os.name == "nt" else "eset_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11"]
    cmd += ["-DARKRTOS_USE_ESET=1", "-DARKRTOS_ESET_HOST_TEST"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, ESET_C]

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
