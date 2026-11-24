#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""持久审计日志环（SEC-G05）的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_audit_log.c：把一个环形文件当台账，记录
「谁在什么时候做了什么、被拒绝了没有」，并保证下次开机还读得回来。它默认关
（ARKRTOS_USE_AUDIT_LOG=0），而关档的代码在任何常规构建里都不会被编到，所以
只跑默认构建等于什么都没验。这里分五组判据：

  A 默认档：什么都不定义时，features.h 出来的开关必须是 0；Kconfig 的三条约束
    （depends on ARKRTOS_USE_FS、range 8 512）与 defconfig / autoconf 必须同口径。
    开关默认值一旦被改错，整套「默认零成本」的说法就不成立了，而这件事在
    常规构建里看不出来。
  B 关档零符号：编成 .o 后 nm 不得看到任何已定义的全局符号。留着桩函数的关档
    比没有更坏——它会照常回答 PASS 形状的问题。
  C 开档编得过：模块本体加六处接线（fs / installer / shell_diag / shell_mod /
    shell_auth / linux_abi）用 F427 工程那套 include 与档位宏做 -fsyntax-only。
    接线代码全在 #if 里，常规构建碰不到它；这一组是它唯一的编译门禁。
  D 宿主用例两档全过：ON 档要打 AUDIT-LOG HOST OK（163 条检查），OFF 档要打
    AUDIT-LOG OFF OK（4 条）。
  E 判据自检：把坏输出喂给判据函数必须判红。门禁自己撒谎比被测代码撒谎更难查。

用法
----
    python tools/audit_log_gate.py
    python tools/audit_log_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

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

# 与 tools/arkrtos_f427_env.py 同一份环境：F427 工程的 include 与软总线档位。
sys.path.insert(0, HERE)
from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS  # noqa: E402

MOD_C = "kernelsrc/src/arkrtos_audit_log.c"
TEST_C = "kernelsrc/tests/test_audit_log_host.c"

# 开档要编的接线文件：每一处都只在 #if ARKRTOS_USE_AUDIT_LOG == 1 里出现，
# 常规构建（默认关）永远看不到它们。
WIRED = [
    "kernelsrc/src/arkrtos_audit_log.c",
    "kernelsrc/src/arkrtos_fs.c",
    "kernelsrc/src/arkrtos_installer.c",
    "kernelsrc/shell/arkrtos_shell_diag.c",
    "kernelsrc/shell/arkrtos_shell_mod.c",
    "kernelsrc/src/arkrtos_shell_auth.c",
    "kernelsrc/src/arkrtos_linux_abi.c",
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
        if os.path.sep in cand:
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
    """门禁的唯一判据：退出码为 0 且标准输出/错误里出现定长标记。

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
    for name, val in (("ARKRTOS_USE_AUDIT_LOG", "0"),
                      ("ARKRTOS_AUDIT_LOG_RECORDS", "64"),
                      ("ARKRTOS_AUDIT_LOG_PATH", '"/audit.bin"'),
                      ):
        if ("#ifndef %s" % name) not in feat:
            errs.append("features.h: %s has no #ifndef default" % name)
    if "#define ARKRTOS_USE_AUDIT_LOG           0" not in feat:
        errs.append("features.h: ARKRTOS_USE_AUDIT_LOG default is not 0")

    kc = read_text("config/kconfig/Kconfig.features")
    i = kc.find("config ARKRTOS_USE_AUDIT_LOG")
    if i < 0:
        errs.append("Kconfig: the symbol is gone")
    else:
        block = kc[i:i + 4000]
        j = block.find("\nconfig ")
        if j > 0:
            block = block[:j]
        if "depends on ARKRTOS_USE_FS" not in block:
            errs.append("Kconfig: the ring no longer depends on ARKRTOS_USE_FS")
        if "default n" not in block:
            errs.append("Kconfig: default is not n")
    j = kc.find("config ARKRTOS_AUDIT_LOG_RECORDS")
    if j < 0:
        errs.append("Kconfig: ARKRTOS_AUDIT_LOG_RECORDS is gone")
    else:
        rblock = kc[j:j + 1200]
        k = rblock.find("\nconfig ")
        if k > 0:
            rblock = rblock[:k]
        if "range 8 512" not in rblock:
            errs.append("Kconfig: ARKRTOS_AUDIT_LOG_RECORDS lost its range 8 512")
        if "depends on ARKRTOS_USE_AUDIT_LOG" not in rblock:
            errs.append("Kconfig: RECORDS no longer depends on the switch")

    dfc = read_text("config/arkrtos_defconfig")
    if "ARKRTOS_USE_AUDIT_LOG=n" not in dfc:
        errs.append("defconfig: ARKRTOS_USE_AUDIT_LOG=n missing")

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_AUDIT_LOG 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_AUDIT_LOG is not 0 (regenerate it)")


def main():
    ap = argparse.ArgumentParser(description="Host gate for the persistent audit log ring")
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
    if judge("", "AUDIT-LOG HOST OK", 1):
        errs.append("self-check: the judge accepts a non-zero exit")
    if judge("everything looks fine", "AUDIT-LOG HOST OK", 0):
        errs.append("self-check: the judge accepts text without the marker")
    if judge("AUDIT-LOG HOST OK", "AUDIT-LOG OFF OK", 0):
        errs.append("self-check: the judge confuses the ON and OFF markers")
    if not judge("x\nAUDIT-LOG HOST OK  163 checks\n", "AUDIT-LOG HOST OK", 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_defaults(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-auditlog-gate-")
    exe_suffix = ".exe" if os.name == "nt" else ""
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例。
        exe = os.path.join(td, "auditlog_off_default" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("default build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            if not judge(out + err, "AUDIT-LOG OFF OK", rc):
                errs.append("default build is not OFF: %s" % (out + err).strip()[:200])

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "audit_log_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_AUDIT_LOG=0"] + inc
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

        # C 组：开档时七处接线必须编得过（-fsyntax-only，不链接）。
        on_defs = ["-DARKRTOS_USE_AUDIT_LOG=1"] + lfs + bus
        for rel in WIRED:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # D 组之二：开档用例全过。
        exe = os.path.join(td, "auditlog_on" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_AUDIT_LOG=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "AUDIT-LOG HOST OK", rc):
                errs.append("ON case failed: %s" % txt.strip()[:240])
    finally:
        if not args.keep:
            shutil.rmtree(td, ignore_errors=True)
        else:
            print("kept: %s" % td)

    if errs:
        for e in errs:
            print("FAIL: %s" % e, file=sys.stderr)
        print("AUDIT-LOG-GATE FAILED (%d)" % len(errs))
        return 1

    print("AUDIT-LOG-GATE OK  default=OFF, OFF object has no symbols, "
          "7 wired files compile with the switch ON, host cases 163/4 pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
