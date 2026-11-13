#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""故障记录环形缓冲（arkrtos_fault）的宿主门禁。

编并跑 kernelsrc/tests/test_fault_host.c 与 kernelsrc/src/arkrtos_fault.c。
只有三个桩（临界区进入/退出、arkrtos_kernel_tick），不需要调度器或板子。

为什么值得跑：这张表是事后诊断的唯一线索。环形索引一旦错一位，串口上
看到的就是「上一次的故障」——一个看起来完全合理、但与实际不符的答案。
这里钉住的是索引语义本身：未写满时 0 = 最旧；写满后 wr 就是最旧槽位，
count 封顶；溢出多圈后仍满足「0 = 总记录数 − N」；越界 index 返回 NULL；
重新 init 连游标一起归零；每条记录都在成对的临界区之间。

明确没验证什么：
  - 真实中断嵌套下的原子性（桩把临界区做成空操作，配平只说明进入与退出
    一一对应，不说明它真的关了中断）；
  - HardFault 路径上调用它的可行性（要板子）；
  - SVC 读取接口 arkrtos_fault_record_read 的打包格式。

用法
----
    python tools/fault_host_gate.py
    python tools/fault_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_fault_host.c")
FAULT_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_fault.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

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
    ap = argparse.ArgumentParser(description="Host gate for the fault record ring")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, FAULT_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-fault-gate-")
    exe = os.path.join(td, "fault_host.exe" if os.name == "nt" else "fault_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, FAULT_C]

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
