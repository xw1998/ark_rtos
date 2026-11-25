#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""回滚白名单（SEC-G11）的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_rollback.c：给「版本不许降」这条安装规则开
一个显式、有界、留痕的例外。它默认关（ARKRTOS_USE_ROLLBACK=0），关档时模块编成
空 TU，安装路径里的那几行也不参与编译——所以只跑默认构建等于什么都没验。
这里分五组判据：

  A 默认档：features.h 的三条默认值、Kconfig 的三条约束（default n / range
    1 8）、defconfig 与 autoconf 必须同口径。默认值一旦被改错，整套「默认没有
    降级口子」的说法就不成立了，而这件事在常规构建里看不出来。
  B 关档零符号：编成 .o 后 nm 不得看到任何已定义的全局符号。留着桩函数的关档
    比没有更坏——它会照常回答 PASS 形状的问题。
  C 开档编得过：模块本体加三处接线（loader 的 check_install、shell_image 的
    命令、shell 的命令表）用 F427 工程那套 include 做 -fsyntax-only。接线全在
    #if 里，常规构建碰不到它；这一组是它唯一的编译门禁。
  D 宿主用例三档全过：默认档要打 ROLLBACK OFF OK；只开模块要打
    ROLLBACK HOST OK；模块 + 持久审计同时开也要打 ROLLBACK HOST OK（这一档
    验的是「放行/武装都落到审计台账」那几条断言）。
  E 判据自检：把坏输出喂给判据函数必须判红。门禁自己撒谎比被测代码撒谎更难查。

用法
----
    python tools/rollback_gate.py
    python tools/rollback_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

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

MOD_C = "kernelsrc/src/arkrtos_rollback.c"
TEST_C = "kernelsrc/tests/test_rollback_host.c"

# 开档要编的接线文件：每一处都只在 #if ARKRTOS_USE_ROLLBACK == 1 里出现。
WIRED = [
    "kernelsrc/src/arkrtos_rollback.c",
    "kernelsrc/src/arkrtos_loader.c",
    "kernelsrc/shell/arkrtos_shell_image.c",
    "kernelsrc/shell/arkrtos_shell.c",
]

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

    单独抽出来是为了让 E 组能用一个假输出证明它真的会判红——一个永远返回
    True 的判据会让整道门禁变成装饰。
    """
    return rc == 0 and marker in txt


def read_text(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def check_defaults(errs):
    """A 组：默认档与三处配置必须同口径。"""
    feat = read_text("kernelsrc/include/arkrtos_features.h")
    if "#ifndef ARKRTOS_USE_ROLLBACK" not in feat:
        errs.append("features.h: ARKRTOS_USE_ROLLBACK has no #ifndef default")
    if "#define ARKRTOS_USE_ROLLBACK            0" not in feat:
        errs.append("features.h: ARKRTOS_USE_ROLLBACK default is not 0")
    if "#ifndef ARKRTOS_ROLLBACK_ENTRIES" not in feat:
        errs.append("features.h: ARKRTOS_ROLLBACK_ENTRIES has no #ifndef default")
    if "#ifndef ARKRTOS_ROLLBACK_USES" not in feat:
        errs.append("features.h: ARKRTOS_ROLLBACK_USES has no #ifndef default")

    kc = read_text("config/kconfig/Kconfig.features")
    i = kc.find("config ARKRTOS_USE_ROLLBACK")
    if i < 0:
        errs.append("Kconfig: ARKRTOS_USE_ROLLBACK is gone")
    else:
        block = kc[i:i + 4000]
        j = block.find("\nconfig ")
        if j > 0:
            block = block[:j]
        if "default n" not in block:
            errs.append("Kconfig: ARKRTOS_USE_ROLLBACK default is not n")
    for name, dep in (("ARKRTOS_ROLLBACK_ENTRIES", "ARKRTOS_USE_ROLLBACK"),
                      ("ARKRTOS_ROLLBACK_USES", "ARKRTOS_USE_ROLLBACK")):
        j = kc.find("config %s" % name)
        if j < 0:
            errs.append("Kconfig: %s is gone" % name)
            continue
        blk = kc[j:j + 1200]
        k = blk.find("\nconfig ")
        if k > 0:
            blk = blk[:k]
        if "range 1 8" not in blk:
            errs.append("Kconfig: %s lost its range 1 8" % name)
        if ("depends on %s" % dep) not in blk:
            errs.append("Kconfig: %s no longer depends on %s" % (name, dep))

    dfc = read_text("config/arkrtos_defconfig")
    if "ARKRTOS_USE_ROLLBACK=n" not in dfc:
        errs.append("defconfig: ARKRTOS_USE_ROLLBACK=n missing")

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_ROLLBACK 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_ROLLBACK is not 0 (regenerate it)")


def main():
    ap = argparse.ArgumentParser(description="Host gate for the SEC-G11 rollback whitelist")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留中间产物便于手工复跑")
    args = ap.parse_args()

    for rel in [MOD_C, TEST_C] + WIRED:
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
    if judge("", "ROLLBACK HOST OK", 1):
        errs.append("self-check: the judge accepts a non-zero exit")
    if judge("everything looks fine", "ROLLBACK HOST OK", 0):
        errs.append("self-check: the judge accepts text without the marker")
    if judge("ROLLBACK HOST OK", "ROLLBACK OFF OK", 0):
        errs.append("self-check: the judge confuses the ON and OFF markers")
    if not judge("x\nROLLBACK HOST OK  41 checks\n", "ROLLBACK HOST OK", 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_defaults(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-rollback-gate-")
    exe_suffix = ".exe" if os.name == "nt" else ""
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例。
        exe = os.path.join(td, "rollback_off_default" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("default build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            if not judge(out + err, "ROLLBACK OFF OK", rc):
                errs.append("default build is not OFF: %s" % (out + err).strip()[:200])

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "rollback_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_ROLLBACK=0"] + inc
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

        # C 组：开档时四处接线必须编得过（-fsyntax-only，不链接）。
        on_defs = ["-DARKRTOS_USE_ROLLBACK=1"] + lfs + bus
        for rel in WIRED:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # D 组之二：只开模块（审计关）用例全过。
        exe = os.path.join(td, "rollback_on" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_ROLLBACK=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "ROLLBACK HOST OK", rc):
                errs.append("ON case failed: %s" % txt.strip()[:240])

        # D 组之三：模块 + 持久审计同开（放行/武装都落台账）。
        exe = os.path.join(td, "rollback_on_audit" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_ROLLBACK=1",
                                "-DARKRTOS_USE_AUDIT_LOG=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON+audit build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "ROLLBACK HOST OK", rc):
                errs.append("ON+audit case failed: %s" % txt.strip()[:240])
    finally:
        if not args.keep:
            shutil.rmtree(td, ignore_errors=True)
        else:
            print("kept: %s" % td)

    if errs:
        for e in errs:
            print("FAIL: %s" % e, file=sys.stderr)
        print("ROLLBACK-GATE FAILED (%d)" % len(errs))
        return 1

    print("ROLLBACK-GATE OK  default=OFF, OFF object has no symbols, "
          "4 wired files compile with the switch ON, host cases OFF/ON/ON+audit pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
