#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""内核静态分析门禁：用 GCC 的 -fanalyzer 逐 TU 走一遍路径敏感的过程间分析。

为什么是这一条而不是 cppcheck / clang-tidy
-------------------------------------------
2026-11-17 登记的那条欠账写的是「本机没有 cppcheck / clang-tidy / MISRA，
GCC 4.8 没有 -fanalyzer（GCC 10 才有）」。2026-11-18 复查发现本机其实带着
riscv64-unknown-elf-gcc-10.2.0（Luban-Lite 工具链），GCC 10 就是 -fanalyzer
首次落地的那一版 —— 于是这条欠账不必等外部工具，用现成的交叉编译器就能补。

-fanalyzer 是真静态分析，和 warn-clean 那一档有本质区别：
  * warn-clean 只开编译器自己的语义告警（-Wall -Wextra -Wshadow），看单条
    TU 的局部问题；
  * -fanalyzer 建控制流图做路径敏感的过程间推导，抓的是
    double-free / use-after-free / null-dereference / 资源泄漏 / 未初始化
    这类「编译通过、上板才炸」的缺陷。
两者结论不可互相替代，ci_gate 里分成两项。

目标架构为什么用 RV32 而不是 RV64
---------------------------------
工具链默认是 riscv64-unknown-elf，指针 64 位，而内核到处是把 32 位地址
(uint32) 转成指针。用 RV64 跑，loader 一个 TU 就报 21 条
-Wint-to-pointer-cast、task 报 152 条 —— 这些是「目标架构选错了」的噪声，
不是缺陷。用 -march=rv32imac -mabi=ilp32 让指针宽度与 MCU 目标一致后，
同一批 TU 全部归零。所以本门禁固定 RV32 口径，且这一点写在判据里。

覆盖范围（诚实边界）
--------------------
跑 kernelsrc/src 下所有能独立编译的 TU、kernelsrc/sdk/app_sdk 与
driver_sdk 的 .c（driver_sdk 带 -DARKRTOS_DRV_USER_MODE，与
example/stm32f427/driver_sdk/DRV_DEMO/GCC/Makefile 的 DEFS 一致）。
少数 TU 在仓库里编不起来，逐条给出理由并列在 SKIP 表里（缺 vendored 头 /
缺档位宏）；被跳过的 TU 名字会打印在输出里，不静默吞掉。判定只认自有树的
诊断（kernelsrc/src、kernelsrc/sdk、kernelsrc/include、board、config、
example），vendored 树（lwip 的 cc.h 之类）的诊断单独计数、不进判据。

判据
----
* 覆盖的 TU 数不得低于 MIN_COVERED_TU（钉住覆盖不缩水）。
* 自有树诊断数必须为 0；任何一条 warning/error 都红。
* 自检：两个故意写坏的探针（double-free / null-dereference）必须各自被
  抓出来，抓不到就判「眼睛是瞎的」，直接失败 —— 防止这一项退化成永远通过。

用法
----
    py -3 tools/analyzer_gate.py
    py -3 tools/analyzer_gate.py --list
退出码 0 = 无诊断；1 = 有诊断或自检失败；3 = 找不到可用的 GCC 10+。
"""

import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 交叉编译器候选：先看环境变量，再看本机工具链，最后看 PATH。
CC_CANDIDATES = [
    os.path.join("D:/Artchip/luban-lite-master/toolchain/bin",
                 "riscv64-unknown-elf-gcc-10.2.0.exe"),
    os.path.join("D:/Artchip/luban-lite-master/toolchain/bin",
                 "riscv64-unknown-elf-gcc.exe"),
    "riscv64-unknown-elf-gcc-10.2.0",
    "riscv64-unknown-elf-gcc-10",
    "riscv64-unknown-elf-gcc",
]

# 固定 RV32 口径：指针宽度与 MCU 目标一致，否则 int-to-pointer-cast 噪声满天飞。
TARGET_FLAGS = ["-march=rv32imac", "-mabi=ilp32"]

BASE_INC = [
    "-Ikernelsrc/include",
    "-Ikernelsrc/components/mdk_trace",
    "-Iboard/stm32f427",
    "-Iconfig",
    "-Ikernelsrc/sdk/app_sdk",
    "-Ikernelsrc/sdk/driver_sdk",
    "-Ikernelsrc/components/lwip/src/include",
    "-Ikernelsrc/components/lwip/port",
]

# 判据只认这些前缀下的诊断；其余（vendored）只计数。
OWN_PREFIXES = ("kernelsrc/src/", "kernelsrc/sdk/", "kernelsrc/include/",
                "board/", "config/", "example/")

# 编不起来的 TU：名字 -> 理由。理由必须非空，否则门禁自己报错。
SKIP = {
    "arkrtos_fs.c": "needs lfs.h (littlefs tree is not vendored in this repo)",
    "arkrtos_softbus.c": "needs -DARK_BUS_* tier macros plus ark_bus_port_* headers",
    "arkrtos_trace.c": "needs ark_shell.h",
    "arkrtos_vfs.c": "needs ark_vfs.h (ark_vfs component source is not in this tree)",
    "arkrtos_vfs_lfs.c": "needs ark_vfs.h (ark_vfs component source is not in this tree)",
}

# 逐 TU 额外宏：driver_sdk 的用户态文件与 example 里的 DRV_DEMO 构建保持一致。
PER_TU_DEFS = {
    "arkrtos_drv_main.c": ["-DARKRTOS_DRV_USER_MODE"],
    "arkrtos_drv_oslib.c": ["-DARKRTOS_DRV_USER_MODE"],
}

# 覆盖下限：2026-11-18 实测值。加内核文件要同步抬这个数，删文件要先说明。
MIN_COVERED_TU = 48

SELF_CHECK_CASES = [
    ("double-free", """#include <stdlib.h>
int probe_double_free(void)
{
    char *p = (char *)malloc(8);
    if (p == 0) return 0;
    free(p);
    free(p);
    return 1;
}
""", "-Wanalyzer-double-free"),
    ("null-dereference", """int probe_null_deref(void)
{
    int *p = 0;
    *p = 1;
    return *p;
}
""", "-Wanalyzer-null-dereference"),
]


def find_cc():
    env = os.environ.get("ARK_ANALYZER_CC")
    if env:
        return env
    for c in CC_CANDIDATES:
        p = shutil.which(c) if not os.path.isabs(c) else (c if os.path.exists(c) else None)
        if p:
            return p
    return None


def cc_major(cc):
    r = subprocess.run([cc, "-dumpfullversion"], stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    txt = (r.stdout or b"").decode("utf-8", "replace").strip()
    if not txt:
        r = subprocess.run([cc, "-dumpversion"], stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        txt = (r.stdout or b"").decode("utf-8", "replace").strip()
    try:
        return int(txt.split(".")[0])
    except (ValueError, IndexError):
        return 0


def compile_one(cc, src, objdir, extra_defs):
    obj = os.path.join(objdir, os.path.basename(src) + ".o")
    cmd = [cc, "-c", "-o", obj, "-fanalyzer"] + TARGET_FLAGS + BASE_INC \
          + list(extra_defs) + [src]
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT)
    return r.returncode, (r.stdout or b"").decode("utf-8", "replace")


def parse_diags(txt):
    """Return [(path, kind, message)] for warning/error lines."""
    out = []
    for line in txt.splitlines():
        parts = line.split(":", 3)
        if len(parts) < 4:
            continue
        path, _ln, _col, rest = parts
        rest = rest.strip()
        for kind in ("warning:", "error:", "fatal error:"):
            if rest.startswith(kind):
                out.append((path.replace("\\", "/"), kind.rstrip(":"), rest))
                break
    return out


def is_own(path):
    return any(path.startswith(p) for p in OWN_PREFIXES)


def collect_tus():
    tus = sorted(glob.glob(os.path.join(ROOT, "kernelsrc", "src", "*.c")))
    tus += sorted(glob.glob(os.path.join(ROOT, "kernelsrc", "sdk", "app_sdk", "*.c")))
    tus += sorted(glob.glob(os.path.join(ROOT, "kernelsrc", "sdk", "driver_sdk", "*.c")))
    covered, skipped = [], []
    for t in tus:
        name = os.path.basename(t)
        rel = os.path.relpath(t, ROOT).replace("\\", "/")
        if name in SKIP:
            skipped.append((rel, SKIP[name]))
        else:
            covered.append(rel)
    return covered, skipped


def run_self_check(cc, objdir):
    """A gate that never fires is worse than no gate."""
    bad = []
    for name, body, need in SELF_CHECK_CASES:
        path = os.path.join(objdir, "probe_%s.c" % name.replace("-", "_"))
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(body)
        _rc, txt = compile_one(cc, path, objdir, [])
        if need not in txt:
            bad.append("%s: analyzer did not report %s" % (name, need))
    return bad


def main(argv):
    ap = argparse.ArgumentParser(description="GCC -fanalyzer gate for Ark RTOS")
    ap.add_argument("--list", action="store_true", help="print the TU coverage and exit")
    args = ap.parse_args(argv)

    covered, skipped = collect_tus()
    for rel, reason in skipped:
        assert reason, "skip reason missing for %s" % rel

    if args.list:
        print("covered (%d):" % len(covered))
        for c in covered:
            print("  " + c)
        print("skipped (%d):" % len(skipped))
        for rel, reason in skipped:
            print("  %s  -- %s" % (rel, reason))
        return 0

    cc = find_cc()
    if not cc:
        print("ANALYZER-GATE FAIL: no riscv64 GCC found (set ARK_ANALYZER_CC)")
        return 3
    major = cc_major(cc)
    if major < 10:
        print("ANALYZER-GATE FAIL: %s is GCC %d, -fanalyzer needs 10+"
              % (cc, major))
        return 3

    objdir = tempfile.mkdtemp(prefix="arkrtos_analyzer_")
    try:
        blind = run_self_check(cc, objdir)
        if blind:
            print("ANALYZER-GATE FAIL: self-check says the analyzer is blind")
            for b in blind:
                print("  " + b)
            return 1

        own, vendored = [], 0
        for rel in covered:
            defs = PER_TU_DEFS.get(os.path.basename(rel), [])
            _rc, txt = compile_one(cc, rel, objdir, defs)
            for path, kind, msg in parse_diags(txt):
                if is_own(path):
                    own.append("%s: %s %s" % (path, kind, msg))
                else:
                    vendored += 1

        if len(covered) < MIN_COVERED_TU:
            print("ANALYZER-GATE FAIL: only %d TU covered, floor is %d "
                  "(coverage shrank silently)" % (len(covered), MIN_COVERED_TU))
            return 1

        if own:
            print("ANALYZER-GATE FAIL: %d diagnostic(s) in own sources" % len(own))
            for d in own:
                print("  " + d)
            return 1

        print("analyzer: %d TU analysed, %d skipped (reasons printed by --list), "
              "%d vendored diagnostic(s) not judged" % (len(covered), len(skipped), vendored))
        print("ANALYZER-GATE OK (%d TU, 0 diagnostic, %d self-check probes fired)"
              % (len(covered), len(SELF_CHECK_CASES)))
        return 0
    finally:
        shutil.rmtree(objdir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
