#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""消息队列（arkrtos_mq）的宿主门禁。

编并跑 kernelsrc/tests/test_mq_host.c 与 kernelsrc/src/arkrtos_mq.c。
测试文件里自带内核桩（调度器、临界区、trace 全部空实现），所以这里只需要
一份宿主 C 编译器，不需要板子、不需要 QEMU、也不需要 arm-none-eabi。

为什么值得跑：这个模块的缺陷几乎都在"槽位账目"里——头尾索引回绕、msglen
与实际拷贝长度错位、满了还写、截断时把下一条串进来。宿主上可以逐字断言，
板子上只能靠串口看现象。

已知边界：阻塞 / 唤醒路径需要真实调度器与节拍，测试里的 task_get_current()
返回 0，所以只有非阻塞路径被覆盖（含 send_from_isr 的满/不满两态）。
那部分只有上板用例能证明，不要把这条门禁读成"整个模块已验证"。

用法
----
    python tools/mq_host_gate.py
    python tools/mq_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_mq_host.c")
MQ_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_mq.c")

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
    ap = argparse.ArgumentParser(description="Host gate for the message queue")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, MQ_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-mq-gate-")
    exe = os.path.join(td, "mq_host.exe" if os.name == "nt" else "mq_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11", "-DARKRTOS_USE_MQ=1"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, MQ_C]

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
