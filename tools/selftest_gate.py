#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""元件自检（SEC-G14）的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_selftest.c：开机与运行期回答「这颗片子还活着
吗」。它默认关（ARKRTOS_USE_SELFTEST=0），关档时模块编成空 TU，init 里的开机
调用与 shell 命令也不参与编译 —— 只跑默认构建等于什么都没验。这里分五组判据：

  A 默认档：features.h 的四条默认值、Kconfig 的四条约束（default n / range 64
    8192 / 1 1000000 / 0 3600000）、defconfig 与 autoconf 必须同口径。默认值被
    改错时整套「默认不占开机时间」的说法就不成立，而常规构建看不出来。
  B 关档零符号：编成 .o 后 nm 不得看到任何已定义的全局符号。
  C 开档编得过：模块本体 + 四处接线（init、shell 命令表、shell_diag 命令、
    LA64 板级两个钩子）用真 include 做 -fsyntax-only。
  D 宿主用例四档全过：默认档打 SELFTEST OFF OK；只开模块打 SELFTEST HOST OK；
    模块 + 持久审计同开也打 SELFTEST HOST OK（那一档验「失败留痕」的断言）；
    再一档把板级钩子摘掉（SELFTEST_NO_HOOK），必须打 SELFTEST-NOHOOK HOST OK
    —— 弱兜底真的被链接进来时，FLASH/CLOCK 只能是 SKIP，不许是 PASS 或崩溃。
  E 判据自检：坏输出必须判红，ON/OFF 两个标记不许混。

用法
----
    python tools/selftest_gate.py
    python tools/selftest_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

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

MOD_C = "kernelsrc/src/arkrtos_selftest.c"
TEST_C = "kernelsrc/tests/test_selftest_host.c"

# 开档要编的接线文件：每一处都只在 #if ARKRTOS_USE_SELFTEST == 1 里出现。
WIRED = [
    "kernelsrc/src/arkrtos_selftest.c",
    "kernelsrc/src/arkrtos_init.c",
    "kernelsrc/shell/arkrtos_shell.c",
    "kernelsrc/shell/arkrtos_shell_diag.c",
]

# 板级钩子单独一组 include：它按 LA64 板子的真实地址与 port 层接口编。
BOARD_C = "board/qemu_loongarch64/arkrtos_board.c"
BOARD_INCS = ["board/qemu_loongarch64", "kernelsrc/port/loongarch/la64"]
BOARD_DEFS = ['-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
              "-include", "arkrtos_board_config.h"]

# 与 coverage_gate.py / test_sign_host.c 同一张 32 字节密钥表。
SIGN_KEY_DEF = "-DARKRTOS_IMAGE_SIGN_KEY={" + ",".join(
    "0x%02X" % b for b in range(32)) + "}"

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
    for name in ("ARKRTOS_USE_SELFTEST", "ARKRTOS_SELFTEST_RAM_BYTES",
                 "ARKRTOS_SELFTEST_CLOCK_PPM", "ARKRTOS_SELFTEST_PATROL_MS"):
        if ("#ifndef %s" % name) not in feat:
            errs.append("features.h: %s has no #ifndef default" % name)
    if "#define ARKRTOS_USE_SELFTEST            0" not in feat:
        errs.append("features.h: ARKRTOS_USE_SELFTEST default is not 0")
    if "#define ARKRTOS_SELFTEST_RAM_BYTES      512" not in feat:
        errs.append("features.h: ARKRTOS_SELFTEST_RAM_BYTES default is not 512")

    kc = read_text("config/kconfig/Kconfig.features")
    i = kc.find("config ARKRTOS_USE_SELFTEST\n")
    if i < 0:
        errs.append("Kconfig: ARKRTOS_USE_SELFTEST is gone")
    else:
        block = kc[i:i + 2000]
        j = block.find("\nconfig ")
        if j > 0:
            block = block[:j]
        if "default n" not in block:
            errs.append("Kconfig: ARKRTOS_USE_SELFTEST default is not n")

    for name, rng in (("ARKRTOS_SELFTEST_RAM_BYTES", "range 64 8192"),
                      ("ARKRTOS_SELFTEST_CLOCK_PPM", "range 1 1000000"),
                      ("ARKRTOS_SELFTEST_PATROL_MS", "range 0 3600000")):
        j = kc.find("config %s\n" % name)
        if j < 0:
            errs.append("Kconfig: %s is gone" % name)
            continue
        blk = kc[j:j + 800]
        k = blk.find("\nconfig ")
        if k > 0:
            blk = blk[:k]
        if rng not in blk:
            errs.append("Kconfig: %s lost its %s" % (name, rng))
        if "depends on ARKRTOS_USE_SELFTEST" not in blk:
            errs.append("Kconfig: %s no longer depends on ARKRTOS_USE_SELFTEST" % name)

    dfc = read_text("config/arkrtos_defconfig")
    for line in ("ARKRTOS_USE_SELFTEST=n", "ARKRTOS_SELFTEST_RAM_BYTES=512",
                 "ARKRTOS_SELFTEST_CLOCK_PPM=5000",
                 "ARKRTOS_SELFTEST_PATROL_MS=0"):
        if line not in dfc:
            errs.append("defconfig: %s missing" % line)

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_SELFTEST 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_SELFTEST is not 0 (regenerate it)")


def main():
    ap = argparse.ArgumentParser(description="Host gate for the SEC-G14 component self-test")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留中间产物便于手工复跑")
    args = ap.parse_args()

    for rel in [MOD_C, TEST_C, BOARD_C] + WIRED:
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
    if judge("", "SELFTEST HOST OK", 1):
        errs.append("self-check: the judge accepts a non-zero exit")
    if judge("everything looks fine", "SELFTEST HOST OK", 0):
        errs.append("self-check: the judge accepts text without the marker")
    if judge("SELFTEST HOST OK", "SELFTEST OFF OK", 0):
        errs.append("self-check: the judge confuses the ON and OFF markers")
    if not judge("x\nSELFTEST HOST OK  44 checks\n", "SELFTEST HOST OK", 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_defaults(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-selftest-gate-")
    exe_suffix = ".exe" if os.name == "nt" else ""
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例。
        exe = os.path.join(td, "selftest_off_default" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("default build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            if not judge(out + err, "SELFTEST OFF OK", rc):
                errs.append("default build is not OFF: %s" % (out + err).strip()[:200])

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "selftest_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_SELFTEST=0"] + inc
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

        # C 组：开档时模块本体与四处接线必须编得过（-fsyntax-only）。
        on_defs = ["-DARKRTOS_USE_SELFTEST=1"] + lfs + bus + [SIGN_KEY_DEF]
        for rel in WIRED:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # C 组之二：LA64 板级两个钩子（按板子的真 address / port 接口编）。
        # 板级 include 必须排在 f427 前面：-include 找的是第一份
        # arkrtos_board_config.h，顺序反了会拿到 F427 那个。
        binc = ["-I" + p for p in BOARD_INCS] + inc
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                             + binc + BOARD_DEFS + on_defs + [BOARD_C], env)
        if rc != 0:
            tail = (err.strip().splitlines() or [""])[-1]
            errs.append("ON syntax failed in %s: %s" % (BOARD_C, tail[:200]))

        # D 组之二：只开模块（审计关）用例全过。
        # 宿主档一律带 -DARKRTOS_SELFTEST_HOST_TEST=1：模块用它开一个注入缝
        # （写完图案、还没校验之间），让用例证明 RAM 失败路径报得出「第几条
        # 图案」。生产构建里这个符号与那条 if 都不存在。
        exe = os.path.join(td, "selftest_on" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_SELFTEST=1",
                                "-DARKRTOS_SELFTEST_HOST_TEST=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "SELFTEST HOST OK", rc):
                errs.append("ON case failed: %s" % txt.strip()[:240])

        # D 组之三：模块 + 持久审计同开（失败要落台账）。
        exe = os.path.join(td, "selftest_on_audit" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_SELFTEST=1",
                                "-DARKRTOS_SELFTEST_HOST_TEST=1",
                                "-DARKRTOS_USE_AUDIT_LOG=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("ON+audit build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "SELFTEST HOST OK", rc):
                errs.append("ON+audit case failed: %s" % txt.strip()[:240])
        # D 组之四：一个板级钩子都不给。用例这一档不定义两个强符号，模块
        # 自己的弱兜底才会真的被链接进来 —— 测的就是「板子没给读数时报
        # SKIP，既不是 PASS 也不是崩溃」。
        exe = os.path.join(td, "selftest_nohook" + exe_suffix)
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0"]
                             + ["-DARKRTOS_USE_SELFTEST=1",
                                "-DARKRTOS_SELFTEST_HOST_TEST=1",
                                "-DSELFTEST_NO_HOOK=1"] + inc
                             + ["-o", exe, TEST_C, MOD_C], env)
        if rc != 0:
            errs.append("no-hook build failed: %s"
                        % (err.strip().splitlines() or [""])[-1][:200])
        else:
            rc, (out, err) = run([exe], env)
            txt = out + err
            if not judge(txt, "SELFTEST-NOHOOK HOST OK", rc):
                errs.append("no-hook case failed: %s" % txt.strip()[:240])

    finally:
        if not args.keep:
            shutil.rmtree(td, ignore_errors=True)
        else:
            print("kept: %s" % td)

    if errs:
        for e in errs:
            print("FAIL: %s" % e, file=sys.stderr)
        print("SELFTEST-GATE FAILED (%d)" % len(errs))
        return 1

    print("SELFTEST-GATE OK  default=OFF, OFF object has no symbols, "
          "5 wired files (incl. LA64 board hooks) compile with the switch ON, "
          "host cases OFF/ON/ON+audit/ON-no-hook pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
