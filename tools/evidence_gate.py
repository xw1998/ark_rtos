#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把「这条读数是在什么载体上拿到的」变成机器可查的一列。

为什么需要这一道
----------------
这个仓库的读数有三种来源，它们的可信度差一个量级，但在散文里长得一模一样：

  L1 编译面      —— 只证明编得过，没有运行；
  L2 QEMU 运行   —— 真跑过，但跑在模拟器上（时序、TLB/ASID 语义、外设模型
                    与硅片都不同；本仓库已实测过若干"QEMU 上成立、硅片上未证"
                    的点，见 docs/验证载体分级.md §3）；
  L3 真机        —— 板子上的串口日志，是唯一能叫"上板"的一档。

含糊的地方正是"QEMU 算上板"：一句话里既没有载体也没有级别，读的人会默认它是
L3。所以台账 tools/evidence_levels.tsv 把每条主张压成一行，本级机械校验它：

  1. 列固定 5 列，level 只能是 L0/L1/L2/L3；
  2. L2 的载体必须是存在的 board/qemu_* 目录（写错机器名就红）；
  3. L3 的载体必须是存在的真实板级目录，且不能是 qemu_*（QEMU 永远升不成 L3）；
  4. L2 / L3 的证据里必须有一个**本仓库历史里真实存在**的提交号——
     git cat-file 查不到就红，防止"我们测过"这种无出处的主张；
  5. L1 的证据必须写成 gate:<脚本> 或提交号（编译面的出处是门禁，不是日志）；
  6. L0 的载体必须是 none（没有载体就老实写没有）；
  7. L3 的行数是冻结的常量 L3_FROZEN：硅片读数只能靠人把板子上的日志抄回来，
     不能靠改代码"长"出来，所以增加一行 L3 必须同时改这个常量——
     那是一个刻意动作，不是顺手。

不覆盖：不判断证据内容是否与主张相符（那要靠人读日志），也不判断一张
文档里的散文有没有偷偷把 L2 说成 L3。它管的是台账本身不许自欺。

用法
----
    py -3 tools/evidence_gate.py
    py -3 tools/evidence_gate.py --verbose

退出码 0 = 全过；1 = 任一规则不成立。
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LEDGER = os.path.join(HERE, "evidence_levels.tsv")
BOARD_DIR = os.path.join(ROOT, "board")

COLS = ["claim", "arch", "level", "carrier", "evidence"]
LEVELS = ("L0", "L1", "L2", "L3")

# 冻结值：当前仓库里真正有硅片读数的行数（全部是 STM32F427）。
L3_FROZEN = 4

COMMIT_RE = re.compile(r"\b[0-9a-f]{7,40}\b")


def load_rows(path):
    rows = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        for n, ln in enumerate(f, 1):
            ln = ln.rstrip("\r\n")
            if not ln.strip() or ln.lstrip().startswith("#"):
                continue
            cells = ln.split("\t")
            if [c.strip() for c in cells] == COLS:
                continue
            rows.append((n, cells))
    return rows


def commit_exists(sha):
    r = subprocess.run(["git", "cat-file", "-e", sha + "^{commit}"],
                       cwd=ROOT, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    return r.returncode == 0


def board_exists(name):
    return os.path.isdir(os.path.join(BOARD_DIR, name))


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    if not os.path.isfile(LEDGER):
        print("FAIL  evidence-levels       台账不存在：%s" % LEDGER)
        return 1

    problems = []
    rows = load_rows(LEDGER)
    if not rows:
        print("FAIL  evidence-levels       台账里没有数据行")
        return 1

    counts = {lv: 0 for lv in LEVELS}
    for n, cells in rows:
        where = "第 %d 行" % n
        if len(cells) != len(COLS):
            problems.append("%s 有 %d 列（应为 %d 列，制表符分隔）"
                            % (where, len(cells), len(COLS)))
            continue
        claim, arch, level, carrier, ev = [c.strip() for c in cells]
        for name, val in (("claim", claim), ("arch", arch), ("level", level),
                          ("carrier", carrier), ("evidence", ev)):
            if not val:
                problems.append("%s 的 %s 为空" % (where, name))
        if level not in LEVELS:
            problems.append("%s 的 level=%r 不在 %s 里" % (where, level, "/".join(LEVELS)))
            continue
        counts[level] += 1

        if level == "L2":
            if not carrier.startswith("qemu_"):
                problems.append("%s 是 L2，载体却是 %r（QEMU 读数必须写 qemu_* 板级目录）"
                                % (where, carrier))
            elif not board_exists(carrier):
                problems.append("%s 的载体 board/%s 不存在" % (where, carrier))
        elif level == "L3":
            if carrier.startswith("qemu_"):
                problems.append("%s 是 L3，载体却是 %r（QEMU 永远升不成真机）"
                                % (where, carrier))
            elif not board_exists(carrier):
                problems.append("%s 的载体 board/%s 不存在" % (where, carrier))
        elif level == "L0":
            if carrier != "none":
                problems.append("%s 是 L0，载体必须是 none，实际 %r" % (where, carrier))
        else:  # L1
            if carrier != "none" and not board_exists(carrier):
                problems.append("%s 是 L1，载体 board/%s 不存在（没有运行载体就写 none）"
                                % (where, carrier))

        if level in ("L2", "L3"):
            shas = COMMIT_RE.findall(ev)
            if not shas:
                problems.append("%s 是 %s，证据里没有提交号" % (where, level))
            else:
                bad = [s for s in shas if not commit_exists(s)]
                if bad:
                    problems.append("%s 的证据提交号在仓库历史里不存在：%s"
                                    % (where, ", ".join(bad)))
        elif level == "L1":
            if "gate:" not in ev and not any(commit_exists(s) for s in COMMIT_RE.findall(ev)):
                problems.append("%s 是 L1，证据既没有 gate:<脚本> 也没有能查到的提交号"
                                % where)

    if counts["L3"] != L3_FROZEN:
        problems.append("L3 行数是 %d，冻结值是 %d（增加真机读数必须同时改 "
                        "tools/evidence_gate.py 的 L3_FROZEN）"
                        % (counts["L3"], L3_FROZEN))
    for lv in LEVELS:
        if counts[lv] == 0:
            problems.append("台账里没有 %s 行（四档必须都在，缺档本身就是信息）" % lv)

    if a.verbose:
        for n, cells in rows:
            print("  %4d  %s" % (n, " | ".join(cells[:4])))

    if problems:
        for p in problems:
            print("FAIL  evidence-levels       %s" % p)
        return 1

    print("PASS  evidence-levels       %d 行台账自洽：L0=%d L1=%d L2=%d L3=%d"
          "（L3 冻结 %d），L2 载体都是存在的 qemu_* 板级、L3 没有 qemu_*，"
          "证据提交号都能在 git 历史里查到"
          % (len(rows), counts["L0"], counts["L1"], counts["L2"], counts["L3"],
             L3_FROZEN))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
