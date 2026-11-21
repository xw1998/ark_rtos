#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""小程序 SDK 参数块字宽门禁：四个用例 + 一条源码断言

为什么要有它：`kernelsrc/sdk/mini_sdk/arkc/arkrtos_mini_rt_arkc.c` 的参数块在
32 位槽与 64 位槽两档下都要与内核 `ARKRTOS_SVC_ARG_BYTES(3)` 对得上。错法的形态
都很安静（块宽错、子命令字没带 bit31、指针被截），板上只表现为「控制台没输出」，
与「内核没收到调用」不可区分。宿主上把 SVC 拿桩接住就能直接看字节。

用例（都在 32 位宿主上编，见下面「为什么不用 64 位宿主」）：

  1. narrow     `-m32`：块 12 字节、`p[0]` 无标记；
  2. 32 位开档  `-m32 -DARKRTOS_USE_WIDE_SVC_ARGS=1`：必须与用例 1 **输出完全
                相同**——32 位目标上这个开关本来就是空档，块不能变宽；
  3. wide       `-m32 -D...=1 -D__SIZEOF_POINTER__=8`：把「指针 8 字节」这个
                前提显式喂进去，验证块 24 字节、`p[0]` 带 `0x80000000`、
                相邻字不被踩；
  4. guard      `-m32 -D...=1 -U__SIZEOF_POINTER__`：工具链不报指针宽度时必须
                **编不过**，且 stderr 里有那句 #error。这一条防的是「默默按
                4 字节走」——那才会给出看似权威的错答案（arkc 就属于这一类）。
  5. 源码断言   `_exit()` 的块用 `MINI_SVC_SUB(ARKRTOS_THREAD_SUB_EXIT)` 填
                `q[0]`。它最后是死循环（内核不会再调度这个任务），宿主上调用
                回不来，只能用源码钉。

为什么不用 64 位宿主：这台机器上唯一的宿主 gcc 是 i686 MinGW（`-dumpmachine`
= `i686-pc-mingw32`），没有 64 位宿主编译器。所以宽档不靠真实 64 位宿主，而是
把「指针宽度 = 8」作为编译期输入（`-D__SIZEOF_POINTER__=8`）喂给同一份源码——
被测的是「拿到 8 字节指针宽度之后块怎么排」，与宿主的真实指针宽度无关。

用法
----
    python tools/mini_svc_args_gate.py
    python tools/mini_svc_args_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

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
TEST_C = os.path.join(ROOT, "kernelsrc", "sdk", "mini_sdk", "tests",
                      "test_svc_args_host.c")
RT_C = os.path.join(ROOT, "kernelsrc", "sdk", "mini_sdk", "arkc",
                    "arkrtos_mini_rt_arkc.c")

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


def build(gcc, out_dir, name, extra, exe):
    cmd = [gcc, "-m32", "-std=c99", "-Wall", "-Wextra", "-Wno-unused-parameter"]
    if "mingw" in gcc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(gcc))
    cmd += extra + ["-o", exe, TEST_C]
    print("building %-9s: %s" % (name, " ".join(extra) or "(default)"))
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def run(exe):
    r = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, r.stdout.decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description="Host gate for the mini SDK SVC arg block width")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留生成的 exe")
    args = ap.parse_args()

    for p in (TEST_C, RT_C):
        if not os.path.exists(p):
            print("missing source: %s" % p, file=sys.stderr)
            return 1

    gcc = find_gcc(args.gcc)
    if gcc is None:
        print("gcc not found; pass --gcc <path>", file=sys.stderr)
        return 1

    exe_suffix = ".exe" if os.name == "nt" else ""
    out_dir = tempfile.mkdtemp(prefix="mini_svcargs_")
    bad = []

    def one(name, extra, want_mode, want_bytes):
        exe = os.path.join(out_dir, name + exe_suffix)
        r = build(gcc, out_dir, name, extra, exe)
        if r.returncode != 0:
            bad.append("%s build failed: %s" % (
                name, (r.stderr or b"").decode("utf-8", "replace").strip()[-200:]))
            return ""
        rc, out = run(exe)
        if rc != 0 or ("SVC-ARGS-HOST OK mode=%s" % want_mode) not in out:
            bad.append("%s run failed: %s" % (name, out.strip()[-300:]))
            return ""
        if ("block_bytes=%d" % want_bytes) not in out:
            bad.append("%s block is not %d bytes: %s"
                       % (name, want_bytes, out.strip().splitlines()[:1]))
        return out

    # 1) 默认档：12 字节块，p[0] 无标记
    narrow = one("narrow", [], "narrow", 12)

    # 2) 32 位目标上开档：必须是空档 —— 输出要与用例 1 逐字相同
    narrow_on = one("narrow-on",
                    ["-DARKRTOS_USE_WIDE_SVC_ARGS=1"], "narrow", 12)
    if narrow and narrow_on and (narrow != narrow_on):
        bad.append("switch on a 32-bit target changed the block: it must be a no-op")

    # 3) 宽档：把「指针 8 字节」喂进去，块 24 字节 + bit31 标记
    one("wide",
        ["-DARKRTOS_USE_WIDE_SVC_ARGS=1", "-D__SIZEOF_POINTER__=8", "-w"],
        "wide", 24)

    # 4) 守卫：开档但不给指针宽度，必须编不过且报出那句 #error
    r = build(gcc, out_dir, "guard",
              ["-DARKRTOS_USE_WIDE_SVC_ARGS=1", "-U__SIZEOF_POINTER__", "-w"],
              os.path.join(out_dir, "guard" + exe_suffix))
    guard_err = (r.stderr or b"").decode("utf-8", "replace")
    if r.returncode == 0:
        bad.append("guard case compiled: a toolchain that hides __SIZEOF_POINTER__ "
                   "must be rejected instead of silently using 32-bit slots")
    elif "does not define __SIZEOF_POINTER__" not in guard_err:
        bad.append("guard case failed for the wrong reason: %s"
                   % guard_err.strip()[-200:])

    # 5) 源码断言：_exit 的块必须带标记（死循环，宿主上调不回来）
    rt = open(RT_C, "rb").read().decode("utf-8", "replace")
    if "MINI_SVC_SUB(ARKRTOS_THREAD_SUB_EXIT)" not in rt:
        bad.append("_exit() no longer fills q[0] with MINI_SVC_SUB(THREAD_SUB_EXIT)")

    if bad:
        for b in bad:
            print("FAIL %s" % b, file=sys.stderr)
        return 1

    print("MINI-SVC-ARGS-GATE OK: narrow 12B / 32-bit switch-on is a no-op / "
          "wide 24B + bit31 + full pointer slot, 3 SVC calls for 200 bytes / "
          "unknown pointer width refused / _exit marker in source")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())