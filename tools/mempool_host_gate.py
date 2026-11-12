#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""固定块内存池的宿主门禁。

编并跑 kernelsrc/tests/test_mempool_host.c 与 kernelsrc/src/arkrtos_mempool.c。
测试文件里自带内核桩（临界区、任务号、trace 全部空实现），所以这里只需要
一份宿主 C 编译器，不需要板子、不需要 QEMU、也不需要 arm-none-eabi。

为什么值得跑：池子的缺陷几乎都在指针算术里——块边界算错、外来指针被当成
池内、双释放把同一个块挂两次、区大小回绕让越界判成合法。判据里最容易写错
的是"拒绝时池状态必须一动不动"，这条只有跑起来才查得出来。

已知边界：SVC 0x20 的参数校验与用户可见性检查需要内核地址空间与 SVC 边界，
宿主上看不到；App 侧 arkrtos_mpool_* 封装同理。那部分只有上板用例能证明，
不要把这门禁读成"整个通道已验证"。

用法
----
    python tools/mempool_host_gate.py
    python tools/mempool_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_mempool_host.c")
MP_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_mempool.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

# 头文件搜索路径：和 ARM 侧编译同一套（板级配置决定 MEMPOOL_NUM 等）
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
    ap = argparse.ArgumentParser(description="Host gate for the fixed-block memory pool")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, MP_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-mempool-gate-")
    exe = os.path.join(td, "mempool_host.exe" if os.name == "nt" else "mempool_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11", "-DARKRTOS_USE_MEMPOOL=1"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, MP_C]

    # 有些 MinGW 装配把 as.exe / collect2.exe 只放在自己 bin 下，
    # 而 gcc 记的 exec prefix 是编译时的路径，找不到就在 CreateProcess 上炸。
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(cc) + os.pathsep + env.get("PATH", "")

    print("gcc: %s" % cc)
    r = subprocess.run(cmd, cwd=ROOT, env=env)
    if r.returncode != 0:
        print("build failed", file=sys.stderr)
        return 1

    r = subprocess.run([exe], cwd=ROOT, env=env)
    ok = (r.returncode == 0)

    if not args.keep:
        shutil.rmtree(td, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
