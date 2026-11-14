#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多对象同时就绪探测（arkrtos_waitmulti）的宿主门禁。

编译并运行 kernelsrc/tests/test_waitmulti_host.c 与
kernelsrc/src/arkrtos_waitmulti.c。测试文件自带四个 *_ready_internal 桩
和空实现的关/开中断，所以只需要一份宿主 C 编译器，不需要板子。

为什么值得跑：这个模块的全部判断都在「句柄分派 + 掩码 + ANY/ALL」里，
出错方式是静默的——掩码位错一位，调用方就会去取错的对象；某个句柄失效
却返回了一半答案，调用方按位取用时会撞到无效句柄。宿主上可以逐条断言，
板子上只表现为「偶尔取不到」。

编译宏：
  -DARKRTOS_USE_WAITMULTI=1   把模块真编进来（默认关，源文件为空对象）

已知边界：真实对象的并发时序（探测到就绪、取用前被别人拿走这个窗口）、
用户态 SVC 轮询包装 arkrtos_wait_multi() 的超时与 1ms 唤醒粒度、
真调度器下的关中断长度都未被覆盖。不要把这条门禁读成「整个模块已验证」。

用法
----
    python tools/waitmulti_host_gate.py
    python tools/waitmulti_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_waitmulti_host.c")
WM_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_waitmulti.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

# 头文件搜索路径：和 ARM 侧编译同一套（板级配置决定各表容量）
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
    ap = argparse.ArgumentParser(description="Host gate for wait-multi probe")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, WM_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-wm-gate-")
    exe = os.path.join(td, "wm_host.exe" if os.name == "nt" else "wm_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11"]
    cmd += ["-DARKRTOS_USE_WAITMULTI=1"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, WM_C]

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
