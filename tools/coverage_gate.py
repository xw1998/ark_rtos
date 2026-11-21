#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""宿主用例的行覆盖率门禁：把"没测过的行"变成可数的数字。

为什么需要这一项
----------------
ci_gate 里那一排 *-host 检查只回答"用例过了没有"。用例过了但只走到模块的
三成代码，与走了八成代码，在这套门禁里长得一模一样。覆盖率把这件事变成一行
可比的数字，并且可以设一条"不许低于当前值"的下限——回归时掉下去就红。

这台机器上没有 cppcheck / gcov 的板端工具链也没关系：宿主用例本来就是用
MinGW gcc 编的，gcc 自带 --coverage + gcov，不需要额外装东西。

口径（写死，避免误读）
----------------------
* 只量 kernelsrc/src 里**被宿主用例链接的那一个 .c**，不量测试文件本身，也不量
  整仓。所以本门禁的数字**不代表内核整体覆盖率**——内核绝大多数的行（调度、
  SVC、驱动、shell）在宿主上根本没有被链接。
* 行覆盖率 = gcov 报的 Lines executed 那一行，分子分母都由 gcov 给，本文不重算。
* 门槛是"不低于基线"，不是"达到某个行业标准"。基线由 --update 从实测写入
  （见 tools/coverage_baseline.json），加代码导致覆盖率下降时会红，需要人工
  判断是"该补用例"还是"该改基线"，不许默默调低。
* 阻塞 / 上板路径在宿主上走不到，所以覆盖率天然有天花板。这是设计事实，不是
  缺陷——别把它读成"这模块测好了"。

用法
----
    python tools/coverage_gate.py                # 量一遍并和基线比
    python tools/coverage_gate.py --update       # 用实测值重写基线
    python tools/coverage_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe

退出码 0 = 全过；1 = 有用例失败 / 编不出来 / 覆盖率低于基线。
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASELINE = os.path.join(HERE, "coverage_baseline.json")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
    os.environ.get("ARKRTOS_HOST_CC", ""),
]

COMMON_INC = [
    "-Ikernelsrc/include",
    "-Ikernelsrc/components/mdk_trace",
    "-Iboard/stm32f427",
    "-Iconfig",
]

# F427 工程那一套补充环境（lwIP / 软总线的头与档位宏）。原样借
# tools/arkrtos_f427_env.py，不在这里抄第二份：那份表改了，四道门禁一起跟上。
from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS

F427_INCS = [
    "-Ikernelsrc/shell",
    "-Ikernelsrc/shell/ark_shell",
    "-Ikernelsrc/fs",
    "-Ikernelsrc/fs/littlefs",
    "-Ikernelsrc/components/ark_vfs/include",
    "-Ikernelsrc/components/arkc/include",
    "-Ikernelsrc/components/arkmake/include",
    "-Ikernelsrc/sdk",
    "-Iconfig/stm32f427",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Core/Inc",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Drivers/STM32F4xx_HAL_Driver/Inc",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/STM32F4xx/Include",
    "-Iexample/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Core/Include",
] + ["-I" + p for p in EXTRA_INCLUDES]

F427_DEFS = [
    '-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "-DSTM32F427xx",
    "-DUSE_HAL_DRIVER",
] + ["-D" + d for d in SOFTBUS_TIER_DEFS]

# 签名链的 32 字节密钥（0x00..0x1F）。与 test_sign_host.c /
# test_payload_sign_host.c 里的 static 表一致，改一处要三处一起改。
SIGN_KEY_DEF = "-DARKRTOS_IMAGE_SIGN_KEY={" + ",".join(
    "0x%02X" % b for b in range(32)) + "}"

# 关掉块层时它上层的三个消费者也必须一起关（features.h 有 #error 守着，
# 这不是本门禁的额外要求，而是仓库自己的依赖规则）。
BLK_OFF_DEFS = [
    "-DARKRTOS_USE_BLK=0",
    "-DARKRTOS_USE_FS=0",
    "-DARKRTOS_USE_VFS=0",
    "-DARKRTOS_USE_VFS_LFS=0",
    "-DARKRTOS_USE_MINIAPP=0",
    "-DARKRTOS_USE_PROCFS=0",
]

# (名字, 测试源, 被量的内核源, 额外宏, 成功标记[, 可选 dict])
# 宏与标记逐条对齐 tools/ci_gate.py 里对应的 *-host 检查，改那边要同步这里。
#
# 可选 dict 的两个键：
#   incs —— 这个用例额外要的头文件搜索路径（F427 工程那套）
#   argv —— 传给用例的实参（例如签名用例把镜像写到哪里）
#   markers —— 覆盖掉第 5 项，要求多条输出同时出现才算过
#
# 同一个 .c 出现在多个用例里（blk 的开档 / 关档）时，百分比按**行号并集**算：
# 两档编出来的是不同的代码（#else 那一半只在关档时存在），而覆盖率问的是
# "这行代码有没有被执行过"。所以每档交出自己的"执行行集 / 编入行集"，
# main() 按模块求并集再除。单档百分比会以 case 行打印出来，只作参考不参与判。
#
# 为什么 sign 用的是 test_payload_sign_host.c 而不是 test_sign_host.c：
# 后者把 MAC 打到 stdout 交给 tools/sign_host_gate.py 与打包器比对，自己
# 不做判定；前者自带手算 tag 对拍 + 篡改必拒 + 流式/一次性一致，宿主上
# 失败会自己返回非 0。覆盖面也更宽（多跑了流式入口）。
CASES = [
    ("utest", "tools/utest_host_selftest.c",
     ["kernelsrc/src/arkrtos_utest.c"], [], "HOST-SELFTEST OK"),
    ("sbuf", "kernelsrc/tests/test_sbuf_host.c",
     ["kernelsrc/src/arkrtos_sbuf.c"], ["-DARKRTOS_USE_SBUF=1"], "SBUF-HOST OK"),
    ("mempool", "kernelsrc/tests/test_mempool_host.c",
     ["kernelsrc/src/arkrtos_mempool.c"], ["-DARKRTOS_USE_MEMPOOL=1"],
     "MEMPOOL-HOST OK"),
    ("mq", "kernelsrc/tests/test_mq_host.c",
     ["kernelsrc/src/arkrtos_mq.c"],
     ["-DARKRTOS_USE_MQ=1", "-DARKRTOS_MQ_HOST_TEST"], "MQ-HOST OK"),
    ("sync", "kernelsrc/tests/test_sync_host.c",
     ["kernelsrc/src/arkrtos_sync.c"],
     ["-DARKRTOS_USE_MTX_RECURSIVE=1", "-DARKRTOS_SYNC_HOST_TEST"], "SYNC-HOST OK"),
    ("loader", "kernelsrc/tests/test_loader_host.c",
     ["kernelsrc/src/arkrtos_loader.c"],
     ["-fno-asynchronous-unwind-tables",
      "-DARKRTOS_USE_IMAGE_CRC_RECHECK=1"], "LOADER-HOST OK"),
    ("fifo", "kernelsrc/tests/test_fifo_host.c",
     ["kernelsrc/src/arkrtos_fifo.c"],
     ["-fno-asynchronous-unwind-tables"], "FIFO-HOST OK"),
    ("fault", "kernelsrc/tests/test_fault_host.c",
     ["kernelsrc/src/arkrtos_fault.c"],
     ["-fno-asynchronous-unwind-tables"], "FAULT-HOST OK"),
    ("eset", "kernelsrc/tests/test_eset_host.c",
     ["kernelsrc/src/arkrtos_eset.c"],
     ["-DARKRTOS_USE_ESET=1", "-DARKRTOS_ESET_HOST_TEST"], "ESET-HOST OK"),
    ("waitmulti", "kernelsrc/tests/test_waitmulti_host.c",
     ["kernelsrc/src/arkrtos_waitmulti.c"],
     ["-DARKRTOS_USE_WAITMULTI=1"], "WM-HOST OK"),
    # --- 2026-11-21：把已有宿主用例但没进覆盖率账的模块补上 -------------
    ("smp", "kernelsrc/tests/test_smp_host.c",
     ["kernelsrc/src/arkrtos_smp.c"],
     ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=4"] + F427_DEFS,
     "SMP-HOST OK", {"incs": F427_INCS}),
    ("sign", "kernelsrc/tests/test_payload_sign_host.c",
     ["kernelsrc/src/arkrtos_sign.c"],
     ["-DARKRTOS_USE_IMAGE_SIGN=1", "-DARKRTOS_USE_EXEC_SIGN=1",
      "-Wno-variably-modified", SIGN_KEY_DEF],
     ["STREAM_MATCH 1", "WRONG_TAG_REFUSED 1"]),
    # 镜像侧入口（sign_image / stream_* / verify）由 test_sign_host.c 走：
    # 它把镜像写到 argv[1]，所以这里给一个相对文件名（工作目录就是临时目录）。
    # 与上面的 sign 用例按行号并集合并到同一个模块（arkrtos_sign）。
    ("sign-image", "kernelsrc/tests/test_sign_host.c",
     ["kernelsrc/src/arkrtos_sign.c"],
     ["-DARKRTOS_USE_IMAGE_SIGN=1", "-Wno-variably-modified", SIGN_KEY_DEF],
     ["MAC ", "IMAGE "], {"argv": ["image.bin"]}),
    ("tickless", "kernelsrc/tests/test_tickless_host.c",
     ["kernelsrc/src/arkrtos_tickless.c"],
     ["-DARKRTOS_USE_TICKLESS=1", "-DARKRTOS_USE_TIMER=1"],
     "TICKLESS-HOST OK", {"incs": F427_INCS}),
    # --- 2026-11-21 新增：块设备注册表（此前没有任何宿主用例）---------
    ("blk", "kernelsrc/tests/test_blk_host.c",
     ["kernelsrc/src/arkrtos_blk.c"],
     ["-DARKRTOS_USE_BLK=1"], "BLK-HOST OK"),
    ("blk-off", "kernelsrc/tests/test_blk_host.c",
     ["kernelsrc/src/arkrtos_blk.c"],
     BLK_OFF_DEFS, "BLK-HOST OFF OK"),
    # --- 2026-11-21 新增：设备框架（此前没有任何宿主用例）---------------
    # HW_SAFE 开/关是两段代码（所有权台账 claim/release），按行号并集合并。
    ("dev", "kernelsrc/tests/test_dev_host.c",
     ["kernelsrc/src/arkrtos_dev.c"],
     ["-DARKRTOS_HW_SAFE=1"], "DEV-HOST OK"),
    ("dev-nosafe", "kernelsrc/tests/test_dev_host.c",
     ["kernelsrc/src/arkrtos_dev.c"],
     ["-DARKRTOS_HW_SAFE=0"], "DEV-HOST NOSAFE OK"),
    # --- 2026-11-21 新增：事件对象（WAITMULTI 开/关是两段代码）-------
    ("event", "kernelsrc/tests/test_event_host.c",
     ["kernelsrc/src/arkrtos_event.c"],
     ["-DARKRTOS_USE_WAITMULTI=0"], "EVENT-HOST OK"),
    ("event-multi", "kernelsrc/tests/test_event_host.c",
     ["kernelsrc/src/arkrtos_event.c"],
     ["-DARKRTOS_USE_WAITMULTI=1"], "EVENT-HOST MULTI OK"),
    # --- 2026-11-21 新增：软定时器（USE_TIMER 开/关 + TICKLESS 是第三段代码）--
    ("timer", "kernelsrc/tests/test_timer_host.c",
     ["kernelsrc/src/arkrtos_timer.c"],
     ["-DARKRTOS_USE_TIMER=1", "-DARKRTOS_TIMER_HOST_TEST"], "TIMER-HOST OK"),
    ("timer-tickless", "kernelsrc/tests/test_timer_host.c",
     ["kernelsrc/src/arkrtos_timer.c"],
     ["-DARKRTOS_USE_TIMER=1", "-DARKRTOS_USE_TICKLESS=1",
      "-DARKRTOS_TIMER_HOST_TEST"], "TIMER-HOST TL OK"),
    ("timer-off", "kernelsrc/tests/test_timer_host.c",
     ["kernelsrc/src/arkrtos_timer.c"],
     ["-DARKRTOS_USE_TIMER=0"], "TIMER-HOST OFF OK"),
    # --- 2026-11-21 新增：任务配置与任务表（STACK_USAGE 开/关 +
    # STACK_GUARD 是第二段代码，按行号并集合并）-----------------------
    # 槽位复用 / recover_pending 跳过 / 表满 / 三道参数闸门是同一个函数里的
    # 不同分支，成功路径必须走一遍才看得见 TCB 填得对不对。
    ("cfg", "kernelsrc/tests/test_cfg_host.c",
     ["kernelsrc/src/arkrtos_cfg.c"], [], "CFG-HOST OK"),
    ("cfg-guard", "kernelsrc/tests/test_cfg_host.c",
     ["kernelsrc/src/arkrtos_cfg.c"], ["-DARKRTOS_USE_STACK_GUARD=1"],
     "CFG-HOST GUARD OK"),
    ("cfg-nousage", "kernelsrc/tests/test_cfg_host.c",
     ["kernelsrc/src/arkrtos_cfg.c"], ["-DARKRTOS_USE_STACK_USAGE=0"],
     "CFG-HOST NOUSAGE OK"),
]


def find_cc(explicit):
    for cand in ([explicit] if explicit else []) + GCC_FALLBACKS:
        if not cand:
            continue
        if os.path.sep in cand or "/" in cand:
            if os.path.isfile(cand):
                return cand
        else:
            hit = shutil.which(cand)
            if hit:
                return hit
    return None


def mingw_prefix(cc):
    """MinGW 的 gcc/gcov 要 -B<dir> 才找得到自己的 cc1 / 运行库。"""
    norm = cc.replace("\\", "/").lower()
    if "mingw" in norm:
        return ["-B" + os.path.dirname(cc)]
    return []


def _run(cmd, cwd=None):
    """跑一条命令并把输出解出来。MinGW 的 gcc/gcov 用本地代码页（GBK）输出，
    直接按 UTF-8 解码会在中文诊断上抛异常，所以这里按顺序试。"""
    r = subprocess.run(cmd, cwd=cwd, capture_output=True)

    def dec(b):
        for enc in ("utf-8", "gbk"):
            try:
                return b.decode(enc)
            except Exception:
                pass
        return b.decode("latin-1")

    return r.returncode, dec(r.stdout), dec(r.stderr)


def parse_lines_executed(txt, src_base):
    """从 gcov 文本里取该源文件的 Lines executed 百分比。"""
    want = "File '" + src_base + "'"
    cur = None
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("File '"):
            cur = line
        elif cur and cur.startswith(want) and line.startswith("Lines executed:"):
            val = line.split("Lines executed:")[1].split("%")[0]
            return float(val)
    # gcov 只处理一个源文件时，标题行可能被省略
    for line in txt.splitlines():
        line = line.strip()
        if line.startswith("Lines executed:"):
            return float(line.split("Lines executed:")[1].split("%")[0])
    return None


def parse_gcov_sets(path):
    """从 .gcov 里取两套行号：被执行过的，与该档编进去的。

    gcov 的文本格式：<count>:<行号>:<源行>，count 为数字表示执行过，
    "#####" 表示没执行，"-" 表示这一行不生成代码。branch / function
    行不是这种格式，会被两个 try/int 自然过滤掉。

    为什么要行号而不直接拿百分比：同一个 .c 在不同宏档下编出来的是不同的
    代码（#if 的一半只在某一档存在，blk 就是如此）。单个用例的百分比只回答
    "这一档走了多少"，而问题问的是"这行代码有没有被跑过"。所以这里把行号
    集交出去，由 main() 按模块求并集再算百分比。
    """
    ex   = set()
    inst = set()
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split(":", 2)
            if len(parts) < 3:
                continue
            head = parts[0].strip()
            try:
                n = int(parts[1])
            except ValueError:
                continue
            if head == "-":
                continue
            if head == "#####":
                inst.add(n)
            elif head.isdigit() or head.endswith("*"):
                inst.add(n)
                ex.add(n)
    return ex, inst

def pct_of(ex, inst):
    """行覆盖率 = 跑过的行 / 编进去的行。分母为 0 时返回 0（不造假）。"""
    return (100.0 * len(ex) / len(inst)) if inst else 0.0

def _case(case):
    """把 CASES 里的一行摊平成 (name, test, mods, defs, markers, incs, argv)。"""
    name, test, mods, defs, marker = case[:5]
    extra = case[5] if len(case) > 5 else {}
    markers = [marker] if isinstance(marker, str) else list(marker)
    return (name, test, mods, defs, markers,
            list(extra.get("incs", [])), list(extra.get("argv", [])))

def measure(cc, name, test, mods, defs, markers, incs=(), argv=()):
    """编 + 跑 + gcov，返回 {模块基名: 行覆盖率}；失败抛 RuntimeError。"""
    base_cmd = ([cc] + mingw_prefix(cc) + ["-std=gnu11", "-O0", "--coverage"]
                + defs + incs + COMMON_INC)
    with tempfile.TemporaryDirectory() as td:
        objs = []
        for src in [test] + mods:
            obj = os.path.join(td, os.path.basename(src)[:-2] + ".o")
            cmd = base_cmd + ["-c", "-o", obj, os.path.abspath(src)]
            rc, _out, err = _run(cmd, cwd=ROOT)
            if rc != 0:
                raise RuntimeError("编译失败 %s: %s" % (src, err.strip()[-300:]))
            objs.append(obj)
        exe = os.path.join(td, "cov_" + name + (".exe" if os.name == "nt" else ""))
        cmd = [cc] + mingw_prefix(cc) + ["--coverage", "-o", exe] + objs
        rc, _out, err = _run(cmd, cwd=ROOT)
        if rc != 0:
            raise RuntimeError("链接失败 %s: %s" % (name, err.strip()[-300:]))
        # 在临时目录里跑，.gcda 才会落在能读到的地方
        rc, out, err = _run([exe] + [str(a) for a in argv], cwd=td)
        out = out + err
        missing = [m for m in markers if m not in out]
        if rc != 0 or missing:
            raise RuntimeError("用例没通过 %s（缺 %s）: %s"
                               % (name, ",".join(missing), out.strip()[-300:]))

        gcov = os.path.join(os.path.dirname(cc),
                            "gcov.exe" if os.name == "nt" else "gcov")
        if not os.path.isfile(gcov):
            hit = shutil.which("gcov")
            if not hit:
                raise RuntimeError("找不到 gcov（应与 gcc 同目录）")
            gcov = hit
        res = {}
        for m in mods:
            base = os.path.basename(m)[:-2]
            # 注意：gcov 不认 gcc 的 -B，传了会 "invalid option -- B"
            rc, out, err = _run([gcov, "-o", td, os.path.abspath(m)], cwd=td)
            txt = out + err
            pct = parse_lines_executed(txt, os.path.basename(m))
            if pct is None:
                raise RuntimeError("gcov 没给出覆盖率 %s: %s" % (base, txt.strip()[-300:]))
            gcovfile = os.path.join(td, os.path.basename(m) + ".gcov")
            if os.path.isfile(gcovfile):
                ex, inst = parse_gcov_sets(gcovfile)
            else:
                ex, inst = set(), set()
            res[base] = (pct, ex, inst)
        return res


def load_baseline():
    if not os.path.isfile(BASELINE):
        return {}
    with open(BASELINE, "r", encoding="utf-8") as f:
        return json.load(f)


def save_baseline(data):
    # 钉两位小数：门禁判据带 0.005 的容差，多留的全精度会让"基线文件里的值"
    # 看起来比之前低一点点（例如 92.02 -> 92.01680），读起来像偷偷降了门槛。
    pinned = {k: round(v + 1e-9, 2) for k, v in data.items()}
    with open(BASELINE, "w", encoding="utf-8", newline="\n") as f:
        json.dump(pinned, f, ensure_ascii=False, indent=2, sort_keys=True)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--update", action="store_true",
                    help="用实测值重写基线（只在明确理由下用）")
    args = ap.parse_args()

    cc = find_cc(args.gcc)
    if not cc:
        print("COVERAGE-GATE FAIL: 没有宿主 C 编译器（设置 ARKRTOS_HOST_CC）")
        return 1

    baseline = load_baseline()
    merged = {}         # 模块基名 -> [被执行行集, 编进去的行集]
    bad = 0
    for case in CASES:
        name, test, mods, defs, markers, incs, argv = _case(case)
        try:
            res = measure(cc, name, test, mods, defs, markers, incs, argv)
        except RuntimeError as exc:
            print("FAIL %-10s %s" % (name, exc))
            bad += 1
            continue
        for base, (pct, ex, inst) in sorted(res.items()):
            slot = merged.setdefault(base, [set(), set()])
            slot[0] |= ex
            slot[1] |= inst
            # 单档读数只作参考行，不参与判（判的是下面的并集）
            print("case %-24s %6.2f%%  [%s]" % (base, pct, name))

    measured = {}
    for base, (ex, inst) in sorted(merged.items()):
        pct = pct_of(ex, inst)
        measured[base] = pct
        floor = baseline.get(base)
        if args.update or floor is None:
            print("new  %-24s %6.2f%%  (无基线，本次记下)" % (base, pct))
        elif pct + 0.005 < floor:
            print("FAIL %-24s %6.2f%%  < 基线 %.2f%%" % (base, pct, floor))
            bad += 1
        else:
            print("ok   %-24s %6.2f%%  (基线 %.2f%%)" % (base, pct, floor))

    if args.update:
        save_baseline(measured)
        print("基线已写入 %s（%d 个模块）" % (os.path.relpath(BASELINE, ROOT), len(measured)))

    if bad:
        print("COVERAGE-GATE FAIL: %d 处不达标" % bad)
        return 1
    print("COVERAGE-GATE OK  (%d 个模块，行覆盖率均不低于基线)" % len(measured))
    return 0


if __name__ == "__main__":
    sys.exit(main())
