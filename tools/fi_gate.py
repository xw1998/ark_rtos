#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""故障注入与篡改监视（SEC-G16 软件半边）的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_fi.c 与它的三处注入点。它默认关
（ARKRTOS_USE_FAULT_INJECT=0）：关档时模块编成空 TU，三处接线块被整段
预编译掉，init 的清账调用与 shell 命令也不参与编译 —— 只跑默认构建等于
什么都没验。这里分五组判据：

  A 默认档：features.h 的 #ifndef/#define 默认值、Kconfig 的 default n、
    defconfig 行、autoconf 生成值必须同口径。默认值被改错时「默认不占
    代码空间、不改变任何行为」的说法就不成立，而常规构建看不出来。
  B 关档零符号：编成 .o 后 nm 不得看到任何已定义的全局符号。
  C 开档编得过：模块本体 + 五处接线（init 清账、task 两处、mempool 一处、
    shell 命令表、shell_diag 命令、shell_internal 声明）用真 include 做
    -fsyntax-only，并带上软总线档位宏（否则同一道门禁会死在 enforce 档）。
  D 宿主用例三档全过：默认档打 FI OFF OK；只开模块打 FI HOST OK；模块 +
    持久审计同开也打 FI HOST OK（那一档验「篡改留痕」的断言）。
  E 判据自检：坏输出必须判红，ON/OFF 两个标记不许混。
  F 接线判据：注入必须真的挂在三个点上，且每个接线文件都有开关闸门。
    只看 grep 计数并不等于验证语义，但它能挡住「改了模块忘了接线」这类
    静默退化 —— 那种情况下 D 组照样全过。

用法
----
    python tools/fi_gate.py
    python tools/fi_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

退出码 0 = 全过；1 = 有一组没通过。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

sys.path.insert(0, HERE)
from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS  # noqa: E402

MOD_C = "kernelsrc/src/arkrtos_fi.c"
MOD_H = "kernelsrc/include/arkrtos_fi.h"
TEST_C = "kernelsrc/tests/test_fi_host.c"

# 开档要编的接线文件：每一处都只在 #if ARKRTOS_USE_FAULT_INJECT == 1 里出现。
WIRED = [
    "kernelsrc/src/arkrtos_fi.c",
    "kernelsrc/src/arkrtos_init.c",
    "kernelsrc/src/arkrtos_task.c",
    "kernelsrc/src/arkrtos_mempool.c",
    "kernelsrc/shell/arkrtos_shell.c",
    "kernelsrc/shell/arkrtos_shell_diag.c",
]

# 注入点接线：文件 -> (arkrtos_fi_inject( 出现次数, 期望的 kind)。
INJECT_SITES = {
    "kernelsrc/src/arkrtos_mempool.c": (1, "ARKRTOS_FI_KIND_ALLOC"),
    "kernelsrc/src/arkrtos_task.c": (2, "ARKRTOS_FI_KIND_TICK"),
}

INCLUDES = [
    "kernelsrc/include",
    "config",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/components/mdk_trace",
    "board/stm32f427",
] + EXTRA_INCLUDES

LFS_DEFS = ["LFS_NO_MALLOC", "LFS_NO_ASSERT", "LFS_NO_DEBUG",
            "LFS_NO_WARN", "LFS_NO_ERROR"]

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]


def find_cc(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in GCC_FALLBACKS:
        if os.sep in cand:
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None


def mingw_prefix(cc):
    if "mingw" in cc.replace("\\", "/").lower():
        return ["-B" + os.path.dirname(cc)]
    return []


def run(cmd, env):
    r = subprocess.run(cmd, cwd=ROOT, env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, ((r.stdout or b"").decode("utf-8", "replace"),
                          (r.stderr or b"").decode("utf-8", "replace"))


def judge(txt, marker, rc):
    """门禁的唯一判据：退出码为 0 且输出里出现定长标记。

    单独抽出来是为了让 E 组能用假输出证明它真的会判红 —— 一个永远返回 True
    的判据会让整道门禁变成装饰。
    """
    return rc == 0 and marker in txt


def read_text(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def check_defaults(errs):
    """A 组：默认档与三处配置必须同口径。"""
    feat = read_text("kernelsrc/include/arkrtos_features.h")
    if "#ifndef ARKRTOS_USE_FAULT_INJECT" not in feat:
        errs.append("features.h: ARKRTOS_USE_FAULT_INJECT has no #ifndef default")
    if "#define ARKRTOS_USE_FAULT_INJECT        0" not in feat:
        errs.append("features.h: ARKRTOS_USE_FAULT_INJECT default is not 0")

    kc = read_text("config/kconfig/Kconfig.features")
    i = kc.find("config ARKRTOS_USE_FAULT_INJECT\n")
    if i < 0:
        errs.append("Kconfig: ARKRTOS_USE_FAULT_INJECT is gone")
    else:
        block = kc[i:i + 2000]
        j = block.find("\nconfig ")
        if j > 0:
            block = block[:j]
        if "default n" not in block:
            errs.append("Kconfig: ARKRTOS_USE_FAULT_INJECT default is not n")
        if "bool" not in block:
            errs.append("Kconfig: ARKRTOS_USE_FAULT_INJECT is not a bool")

    dfc = read_text("config/arkrtos_defconfig")
    if "ARKRTOS_USE_FAULT_INJECT=n" not in dfc:
        errs.append("defconfig: ARKRTOS_USE_FAULT_INJECT=n missing")

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_FAULT_INJECT 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_FAULT_INJECT is not 0 (regenerate it)")


def check_wiring(errs):
    """F 组：注入点必须真的挂上，且每个接线文件都有开关闸门。"""
    for rel, (count, kind) in INJECT_SITES.items():
        src = read_text(rel)
        got = src.count("arkrtos_fi_inject(")
        if got != count:
            errs.append("%s: arkrtos_fi_inject( appears %d time(s), want %d"
                        % (rel, got, count))
        if kind not in src:
            errs.append("%s: no %s at the injection site" % (rel, kind))
        if "#if (ARKRTOS_USE_FAULT_INJECT == 1)" not in src:
            errs.append("%s: injection is not behind the switch" % rel)

    for rel in WIRED:
        if "#if (ARKRTOS_USE_FAULT_INJECT == 1)" not in read_text(rel):
            errs.append("%s: no #if (ARKRTOS_USE_FAULT_INJECT == 1) guard" % rel)

    hdr = read_text(MOD_H)
    if "ARKRTOS_FI_SELFTEST_ALL" not in hdr:
        errs.append("%s: ARKRTOS_FI_SELFTEST_ALL is gone" % MOD_H)

    audit_h = read_text("kernelsrc/include/arkrtos_audit_log.h")
    if "ARKRTOS_AUDIT_EV_TAMPER" not in audit_h:
        errs.append("arkrtos_audit_log.h: ARKRTOS_AUDIT_EV_TAMPER is gone")


def main():
    ap = argparse.ArgumentParser(description="Host gate for the SEC-G16 fault injection module")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留中间产物便于手工复跑")
    args = ap.parse_args()

    for rel in [MOD_C, MOD_H, TEST_C] + WIRED:
        if not os.path.exists(os.path.join(ROOT, rel)):
            print("source not found: %s" % rel, file=sys.stderr)
            return 1

    cc = find_cc(args.gcc)
    if not cc:
        print("no host C compiler found (use --gcc)", file=sys.stderr)
        return 1

    pre = mingw_prefix(cc)
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(cc) + os.pathsep + env.get("PATH", "")
    inc = ["-I" + p for p in INCLUDES]
    bus = ["-D" + d for d in SOFTBUS_TIER_DEFS]
    lfs = ["-D" + d for d in LFS_DEFS]

    errs = []

    # E 组先跑：判据本身不成立时，后面的「通过」全部不可信。
    if judge("", "FI HOST OK", 1):
        errs.append("self-check: the judge accepts a non-zero exit")
    if judge("everything looks fine", "FI HOST OK", 0):
        errs.append("self-check: the judge accepts text without the marker")
    if judge("FI HOST OK", "FI OFF OK", 0):
        errs.append("self-check: the judge confuses the ON and OFF markers")
    if not judge("x\nFI HOST OK  78 checks\n", "FI HOST OK", 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_defaults(errs)
    check_wiring(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-fi-gate-")
    exe_suffix = ".exe" if os.name == "nt" else ""
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例。
        exe = os.path.join(td, "fi_off_default" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("default build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            if not judge(out + err, "FI OFF OK", rc):
                errs.append("default build is not OFF: %s" % (out + err).strip()[:200])

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "fi_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_FAULT_INJECT=0"] + inc
                             + ["-c", "-o", obj, MOD_C], env)
        if rc != 0:
            errs.append("OFF object failed to build: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            nm = os.path.join(os.path.dirname(cc), "nm.exe" if os.name == "nt" else "nm")
            if not os.path.isfile(nm):
                nm = shutil.which("nm") or "nm"
            rc, (out, err) = run([nm, obj], env)
            defined = []
            for ln in out.splitlines():
                parts = ln.split()
                if len(parts) >= 3 and parts[-2].isupper():
                    defined.append(parts[-1])
            if rc != 0:
                errs.append("nm failed on the OFF object: %s" % err.strip()[:200])
            elif defined:
                errs.append("OFF object still defines symbols: %s"
                            % ", ".join(defined[:8]))

        # C 组：开档时模块本体与五处接线必须编得过（-fsyntax-only）。
        on_defs = ["-DARKRTOS_USE_FAULT_INJECT=1"] + lfs + bus
        for rel in WIRED:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # D 组之二：只开模块（审计关）用例全过。
        exe = os.path.join(td, "fi_on" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_FAULT_INJECT=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "FI HOST OK", rc):
                errs.append("ON case failed: %s" % txt.strip()[:240])

        # D 组之三：模块 + 持久审计同开（篡改要落台账）。
        exe = os.path.join(td, "fi_on_audit" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_FAULT_INJECT=1",
                                "-DARKRTOS_USE_AUDIT_LOG=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON+audit build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "FI HOST OK", rc):
                errs.append("ON+audit case failed: %s" % txt.strip()[:240])
    finally:
        if not args.keep:
            shutil.rmtree(td, ignore_errors=True)
        else:
            print("kept: %s" % td)

    if errs:
        for e in errs:
            print("FAIL: %s" % e, file=sys.stderr)
        print("FI-GATE FAILED (%d)" % len(errs))
        return 1

    print("FI-GATE OK  default=OFF, OFF object has no symbols, "
          "%d wired files compile with the switch ON, host cases OFF/ON/ON+audit pass, "
          "injection sites wired" % len(WIRED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
