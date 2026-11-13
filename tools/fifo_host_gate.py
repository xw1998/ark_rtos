#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""环形缓冲（arkrtos_fifo）的宿主门禁。

编并跑 kernelsrc/tests/test_fifo_host.c 与 kernelsrc/src/arkrtos_fifo.c。
这个模块不依赖调度器、临界区或任何板级资源，所以测试文件里没有内核桩，
只需要一份宿主 C 编译器。

为什么值得跑：FIFO 丢字节在板子上表现为「串口偶尔串字符 / 少一行」，
看起来永远像传输问题。这里把三件事钉成可断言的事实：

  1. 容量账目 create(size) 的有效容量是 size - 9（头部 8 字节 + 一格用于
     区分空/满）；size <= 8 直接返回 0 且不改写调用方缓冲；size == 9 的
     FIFO 一个字节也写不进。
  2. 搬运语义 写满拒绝剩余且不覆盖未读数据；环形回绕后读出顺序仍是发送
     顺序；请求多于存量时返回存量；len <= 0 是空操作。
  3. 计数器 arkrtos_fifo_write_refused() 是所有 FIFO 累计的「因满被拒字节」；
     arkrtos_fifo_bad_magic() / bad_magic_bytes() 是「头部被踩坏」的调用次数
     与因此丢掉的字节（read 只记次数，不记字节——读没有丢数据）。

明确没验证什么：
  - 单生产者-单消费者之外的并发（本模块按契约不内建临界区，混用要调用方加锁）；
  - size > 65543（size 字段是 uint16，会静默截断，属已知限制）；
  - 手工构造 size == 0 且魔数正确时 write 会除零（调用方违约，不制造 UB）。

用法
----
    python tools/fifo_host_gate.py
    python tools/fifo_host_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_fifo_host.c")
FIFO_C = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_fifo.c")

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
    ap = argparse.ArgumentParser(description="Host gate for the byte FIFO")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe 便于手工复跑")
    args = ap.parse_args()

    for path in (TEST_C, FIFO_C):
        if not os.path.exists(path):
            print("test source not found: %s" % path, file=sys.stderr)
            return 1

    cc = find_gcc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    td = tempfile.mkdtemp(prefix="arkrtos-fifo-gate-")
    exe = os.path.join(td, "fifo_host.exe" if os.name == "nt" else "fifo_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11", "-fno-asynchronous-unwind-tables"]
    cmd += ["-I" + p for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, FIFO_C]

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
