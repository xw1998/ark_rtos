#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""宿主可编 TU 的编译器告警门禁：-Wall -Wextra 必须零告警。

为什么需要这一项
----------------
这台机器上没有 cppcheck / MISRA / clang-tidy，宿主那份 GCC 4.8 也没有 -fanalyzer
（GCC 10 才有）。真正能跑路径敏感分析的是本机交叉工具链自带的
riscv64-unknown-elf-gcc-10.2.0，那条线是独立的 tools/analyzer_gate.py（ci_gate 的
analyzer 项，2026-11-18 接）。本脚本只做更小的那一半：把编译器自己的语义分析开到满，
-Wall -Wextra -Wshadow 覆盖未初始化、隐式声明、有符号/无符号混用、返回值未用、
数组越界初判、内外层变量同名遮蔽等。它**不等于静态分析**，只是比默认告警多看一眼。

更重的告警集为什么不一起开（2026-11-17 实测计数，21 个 TU）：
-Wconversion 33 条、-Wsign-conversion 27 条、-Wmissing-prototypes 一类 25 条。
这些是类型收窄/接口暴露的风格问题，逐条改等于改内核 API 与数据通路，风险
远大于收益，要动必须先单独定口径。所以本门禁只钉住「编译器能确定是错的」
那一档，并把上面这三个数字登记在 docs/欠账盘点.md 里。

它抓到过真东西：2026-11-17 它在 arkrtos_sync.c 上翻出 arkrtos_sync_release_task()
里的 p_tsk 声明了却从未赋值 —— 死任务的锁永远不会移交、等待队列永远摘不干净。
那种缺陷编译通过、上板也不报错，只有读代码或看告警能发现。

这条路为什么只覆盖这 21 个 TU
-----------------------------
内核绝大多数文件依赖架构头（`__svc`、CMSIS、LA64 的 CSR 宏、RISC-V 的 PMP 常量），
宿主的 x86 gcc 编不了，硬编也只会得到一堆"这个架构没有"的噪声。所以本门禁只跑
**宿主用例已经链接到的那批 TU**：10 个测试文件 + 10 个被测模块 + SDK stdio。
剩下的文件要用各自工具链（armcc/armclang、loongarch64-newlib-elf-gcc、
riscv64 工具链）编，那是另一件事，本门禁不声称覆盖它们。

判据与边界
----------
* 判据 = 每一条 TU 编译成功且输出里没有 "warning:"。有任何一条告警就红。
* 选项集固定为 -Wall -Wextra -Wshadow；加选项要先在宿主 21 个 TU 上跑成零告警。
* 这里只证明"编译器在这个配置下没话说"，不证明没有逻辑缺陷；也不是 MISRA /
  认证意义上的静态分析。别拿它当"静态分析已完成"。
* 宏组合与 tools/ci_gate.py 里对应的 *-host 检查逐条一致；改那边要同步这里。

用法
----
    python tools/warn_gate.py
    python tools/warn_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

退出码 0 = 零告警；1 = 有告警或编不过。
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from coverage_gate import CASES, COMMON_INC, find_cc, mingw_prefix  # noqa: E402

# 与 CASES 同源的补一条：SDK stdio 的宿主用例（对应 ci_gate 的 sdk-stdio-host）。
EXTRA = [("sdk-stdio", "kernelsrc/sdk/app_sdk/tests/test_stdio_host.c", [], [])]


def _dec(b):
    for enc in ("utf-8", "gbk"):
        try:
            return b.decode(enc)
        except Exception:
            pass
    return b.decode("latin-1")


def compile_one(cc, src, defs):
    cmd = ([cc] + mingw_prefix(cc) +
           ["-std=gnu11", "-O0", "-Wall", "-Wextra", "-Wshadow", "-c", "-o", os.devnull,
            src] + list(defs) + COMMON_INC)
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True)
    out = _dec(r.stdout) + _dec(r.stderr)
    warn = [l for l in out.splitlines() if "warning:" in l]
    err = [l for l in out.splitlines() if "error:" in l]
    return r.returncode, warn, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--selfcheck", action="store_true",
                    help="先证明这条门禁真能看见告警（防假绿）")
    args = ap.parse_args()

    cc = find_cc(args.gcc)
    if not cc:
        print("WARN-GATE FAIL: 没有宿主 C 编译器（设置 ARKRTOS_HOST_CC）")
        return 1

    if args.selfcheck:
        # 一条必然告警的 TU（未用参数 + 未初始化读）：如果连它都报 0 条，
        # 说明是门禁瞎了而不是代码干净。
        import tempfile
        with tempfile.TemporaryDirectory() as td:
            bad_c = os.path.join(td, "deliberate_warning.c")
            with open(bad_c, "w", encoding="utf-8") as f:
                f.write("int probe(int unused_arg)\n{\n    int x;\n    return x;\n}\n")
            _rc, warn, _err = compile_one(cc, bad_c, [])
            if not warn:
                print("WARN-GATE selfcheck FAIL: 故意写坏的 TU 一条告警都没报")
                return 1
            print("selfcheck ok: 故意写坏的 TU 被报出 %d 条告警" % len(warn))
        return 0

    tus = []
    for name, test, mods, defs, _marker in CASES:
        tus.append((name, test, defs))
        for m in mods:
            tus.append((name, m, defs))
    for name, test, mods, defs in EXTRA:
        tus.append((name, test, defs))
        for m in mods:
            tus.append((name, m, defs))

    bad = 0
    total = 0
    for name, src, defs in tus:
        rc, warn, err = compile_one(cc, src, defs)
        total += len(warn)
        if rc != 0 or err:
            bad += 1
            print("FAIL %-10s %-46s 编不过" % (name, src))
            for l in err[:3]:
                print("       " + l[:150])
        elif warn:
            bad += 1
            print("FAIL %-10s %-46s %d 条告警" % (name, src, len(warn)))
            for l in warn[:5]:
                print("       " + l.replace(ROOT + os.sep, "")[:150])
        else:
            print("ok   %-10s %-46s 0 告警" % (name, src))

    if bad:
        print("WARN-GATE FAIL: %d 个 TU 有问题（共 %d 条告警）" % (bad, total))
        return 1
    print("WARN-GATE OK  (%d 个宿主可编 TU，-Wall -Wextra -Wshadow 零告警；"
          "这不是静态分析，内核其余文件由各自工具链编)" % len(tus))
    return 0


if __name__ == "__main__":
    sys.exit(main())
