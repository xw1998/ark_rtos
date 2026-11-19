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

2026-11-18 起把「接口暴露」这一类也收进来了（-Wmissing-prototypes
-Wstrict-prototypes -Wold-style-definition）。此前 21 个 TU 上这三条共 25 条，
全部是 -Wmissing-prototypes，分两族：
  * 内核 5 条 —— arkrtos_mq_send_from_isr / arkrtos_mq_recv_from_isr 与
    arkrtos_sem_post_from_isr 在 arkrtos.h 里有声明，但定义它们的 TU 只
    include 模块头，模块头里没有原型 ⇒ 定义点看不见原型；外加宿主档
    （ARKRTOS_SYNC_HOST_TEST）里不是 static 的 arkrtos_mtx_propagate /
    arkrtos_mtx_recalc_priority。修法是把原型补进各自的模块头（mq.h / sync.h）
    与宿主档分支，不制造新的公共 API。
  * 测试桩 20 条 —— 测试文件就地替掉内核函数（trace 钩子 12、端口开关 2、
    四个 ready 查询 4、arkrtos_fault_record 与 arkrtos_shell_print 各 1），
    桩定义前没有原型。修法是「先声明后定义」。
-Wstrict-prototypes / -Wold-style-definition 这次一条都没报（全仓签名都带原型），
一并钉住是为防止以后退化成 K&R 风格。
-Wconversion（含它隐含的 -Wsign-conversion）2026-11-19 起打开。此前 21 个 TU 上共
33 条（6 条窄化 + 27 条符号），当时的判断是"逐条改等于改数据通路，风险大于收益，
要动必须先定口径"。这次定的口径只有两条：

  1. 只加**显式转换**，不改变量类型、不改比较语义、不改算法。窄化处写 (uint8) /
     (uint16)，把原来隐式发生的截断写明；句柄掩码比较写成 ((uint32)handle & MASK)，
     位模式与原来逐位相同（int32 -> uint32 是保值转换）。
  2. 判据不是"编译过了"，而是**默认档 LA64 镜像逐字节不变**：转换只该改变编译器
     看到的意图，不该改变生成的指令与数据。

仍未开：-Wcast-qual（14 条，全在 test_eset_host.c，把字符串字面量的 const 丢掉）、
-Wwrite-strings（会额外暴露 20 条同型问题：create(name) 形参是 char* 而不是
const char*，修它要动 API 签名，属于另一件事）、-Wpointer-arith / -Wundef /
-Wbad-function-cast 各 0 条。数字登记在 docs/欠账盘点.md。

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
* 选项集固定为 WARN_FLAGS（-Wall -Wextra -Wshadow + 三条接口暴露告警）；
  加选项要先在宿主 21 个 TU 上跑成零告警，并在 docstring 里写清口径。
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

# 固定选项集：前三条问「编译器能确定是错的吗」，后三条问「接口有没有露在暗处」。
# 加选项必须先跑成零告警再改这里，否则门禁会先红一次。
WARN_FLAGS = ["-Wall", "-Wextra", "-Wshadow",
              "-Wmissing-prototypes", "-Wstrict-prototypes", "-Wold-style-definition",
              "-Wconversion"]

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
           ["-std=gnu11", "-O0"] + WARN_FLAGS + ["-c", "-o", os.devnull,
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

            # 第二条探针专测接口暴露那三条：没有原型的非 static 函数必须被报出
            # -Wmissing-prototypes。-Wall/-Wextra 对它是沉默的，所以这一条同时
            # 证明「新加的三个选项真的在命令行上」。
            miss_c = os.path.join(td, "deliberate_missing_proto.c")
            with open(miss_c, "w", encoding="utf-8") as f:
                f.write("int probe_no_proto(int v)\n{\n    return v;\n}\n")
            _rc2, warn2, _err2 = compile_one(cc, miss_c, [])
            hit = [l for l in warn2 if "missing-prototypes" in l]
            if not hit:
                print("WARN-GATE selfcheck FAIL: 没有原型的函数没被 -Wmissing-prototypes 报出")
                return 1
            print("selfcheck ok: 无原型探针被报出 %d 条" % len(hit))
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
    print("WARN-GATE OK  (%d 个宿主可编 TU，%s 零告警；"
          "这不是静态分析，内核其余文件由各自工具链编)"
          % (len(tus), " ".join(WARN_FLAGS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
