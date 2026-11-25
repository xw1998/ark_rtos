#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""信任根（SEC-G12）的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_trust.c：开机时决定「这次用来验镜像的公钥，
与上次钉在板上的那把是不是同一把」，不一致就锁存，认领路径据此拒绝一切镜像。
它默认关（ARKRTOS_USE_TRUST_ROOT=0），关档时模块编成空 TU，init / loader /
sec 三处接线也不参与编译 —— 只跑默认构建等于什么都没验。这里分五组判据：

  A 默认档与配置：features.h 的默认值必须是 0 且带「依赖非对称档」的 #error；
    Kconfig 一项 default n + depends on ARKRTOS_USE_IMAGE_SIGN_ED25519；
    defconfig 与 autoconf 同口径。默认值被改错时「默认镜像逐字节不变」这句话
    就不成立，而常规构建看不出来。
  B 关档零符号：编成 .o 后 nm 不得看到任何已定义的全局符号。
  C 开档编得过：模块本体 + 三处接线（init 的开机调用、loader 的锁存拒绝、
    sec 的报告项）用真 include 做 -fsyntax-only。
  D 宿主用例两档全过：默认档打 TRUST-HOST OFF OK；开档打 TRUST-HOST OK
    （模块用例），再把 sec 集成那一档也跑一遍（PASS/FINDING/FAIL/SKIP 四态）。
  E 判据自检（变异）：往源码里种三个已知错误，用例必须变红 ——
    ① 锁存不置位；② 「读不到钉存槽」被报成 MATCH；③ sec 把「写不进去」
    报成 PASS。任何一处改完仍然全绿，说明对应的断言只是装饰。

用法
----
    python tools/trust_gate.py
    python tools/trust_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

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

MOD_C = "kernelsrc/src/arkrtos_trust.c"
TEST_C = "kernelsrc/tests/test_trust_host.c"
SEC_TEST_C = "kernelsrc/tests/test_sec_host.c"
SEC_C = "kernelsrc/src/arkrtos_sec.c"

# 开档要编的三处接线：每一处都只在 #if ARKRTOS_USE_TRUST_ROOT == 1 里出现。
WIRED = [
    "kernelsrc/src/arkrtos_trust.c",
    "kernelsrc/src/arkrtos_init.c",
    "kernelsrc/src/arkrtos_loader.c",
    "kernelsrc/src/arkrtos_sec.c",
]

# 与 coverage_gate.py / test_sign_host.c 同一张 32 字节密钥表与同一把公钥。
SIGN_KEY_DEF = "-DARKRTOS_IMAGE_SIGN_KEY={" + ",".join(
    "0x%02X" % b for b in range(32)) + "}"
ED25519_PUBKEY_DEF = "-DARKRTOS_IMAGE_SIGN_PUBKEY={" + ",".join(
    "0x%02X" % b for b in bytes.fromhex(
        "03a107bff3ce10be1d70dd18e74bc09967e4d6309ba50d5f1ddc8664125531b8")) + "}"

# 信任根本身依赖非对称档（features.h 有 #error 守着），所以开档三件套固定。
TRUST_ON_DEFS = [
    "-DARKRTOS_USE_TRUST_ROOT=1",
    "-DARKRTOS_USE_IMAGE_SIGN=1",
    "-DARKRTOS_USE_IMAGE_SIGN_ED25519=1",
    "-DARKRTOS_USE_ED25519=1",
    ED25519_PUBKEY_DEF,
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

MARK_ON = "TRUST-HOST OK"
MARK_OFF = "TRUST-HOST OFF OK"
MARK_SEC = "SEC-HOST OK"


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

    单独抽出来是为了让 E 组能用变异版证明它真的会判红 —— 一个永远返回 True
    的判据会让整道门禁变成装饰。
    """
    return rc == 0 and marker in txt


def read_text(rel):
    with open(os.path.join(ROOT, rel), "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def block_of(text, header):
    """取一个 Kconfig 条目（到下一个 config 为止）。"""
    i = text.find(header)
    if i < 0:
        return None
    blk = text[i:i + 3000]
    j = blk.find("\nconfig ", len(header))
    if j > 0:
        blk = blk[:j]
    return blk


def check_defaults(errs):
    """A 组：默认档与三处配置必须同口径。"""
    feat = read_text("kernelsrc/include/arkrtos_features.h")
    if "#ifndef ARKRTOS_USE_TRUST_ROOT" not in feat:
        errs.append("features.h: ARKRTOS_USE_TRUST_ROOT has no #ifndef default")
    if "#define ARKRTOS_USE_TRUST_ROOT          0" not in feat:
        errs.append("features.h: ARKRTOS_USE_TRUST_ROOT default is not 0")
    # 依赖必须写在编译期：钉一把"公钥"在 HMAC 档上没有意义。
    if ("(ARKRTOS_USE_TRUST_ROOT == 1) && (ARKRTOS_USE_IMAGE_SIGN_ED25519 != 1)"
            not in feat):
        errs.append("features.h: the asymmetric-profile dependency #error is gone")

    kc = read_text("config/kconfig/Kconfig.features")
    blk = block_of(kc, "config ARKRTOS_USE_TRUST_ROOT\n")
    if blk is None:
        errs.append("Kconfig: ARKRTOS_USE_TRUST_ROOT is gone")
    else:
        if "default n" not in blk:
            errs.append("Kconfig: ARKRTOS_USE_TRUST_ROOT default is not n")
        if "depends on ARKRTOS_USE_IMAGE_SIGN_ED25519" not in blk:
            errs.append("Kconfig: ARKRTOS_USE_TRUST_ROOT lost its dependency")

    dfc = read_text("config/arkrtos_defconfig")
    if "ARKRTOS_USE_TRUST_ROOT=n" not in dfc:
        errs.append("defconfig: ARKRTOS_USE_TRUST_ROOT=n missing")

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_TRUST_ROOT 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_TRUST_ROOT is not 0 (regenerate it)")

    # 三处接线必须真的在（删掉一处，"锁存即拒绝认领"这条链就断了，而默认档
    # 编译仍然全绿）。
    init_c = read_text("kernelsrc/src/arkrtos_init.c")
    if "arkrtos_trust_boot()" not in init_c:
        errs.append("init.c: the boot-time trust check is not wired")
    loader_c = read_text("kernelsrc/src/arkrtos_loader.c")
    if "arkrtos_trust_locked()" not in loader_c:
        errs.append("loader.c: accept() does not consult the latch")
    sec_c = read_text(SEC_C)
    if "sec_check_trust" not in sec_c:
        errs.append("sec.c: the trust.root report item is gone")


def build_and_run(cc, pre, env, td, name, srcs, defs, marker, expect_ok=True):
    """编 + 跑一档，返回 (是否按预期, 说明)。"""
    exe = os.path.join(td, name + (".exe" if os.name == "nt" else ""))
    cmd = ([cc] + pre + ["-std=gnu11", "-O0", "-ffunction-sections"]
           + defs + ["-I" + p for p in INCLUDES] + ["-o", exe] + list(srcs))
    rc, (out, err) = run(cmd, env)
    if rc != 0:
        tail = (err.strip().splitlines() or [""])[-1]
        return False, "build failed (%s): %s" % (name, tail[:200])
    rc, (out, err) = run([exe], env)
    txt = out + err
    good = judge(txt, marker, rc)
    if expect_ok:
        return good, ("case failed (%s): %s" % (name, txt.strip()[:240]) if not good else "")
    return (not good), ""


def mutate(rel, old, new, td, tag):
    """把模块的一个已知正确行为改坏，写到临时目录，返回新路径。"""
    s = read_text(rel)
    if s.count(old) != 1:
        return None, "mutation anchor not unique in %s: %s" % (rel, tag)
    path = os.path.join(td, "mut_%s_%s" % (tag, os.path.basename(rel)))
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(s.replace(old, new, 1))
    return path, ""


def main():
    ap = argparse.ArgumentParser(description="Host gate for the SEC-G12 trust root")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留中间产物便于手工复跑")
    args = ap.parse_args()

    for rel in [MOD_C, TEST_C, SEC_TEST_C, SEC_C] + WIRED:
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
    if judge("", MARK_ON, 1):
        errs.append("self-check: the judge accepts a non-zero exit")
    if judge("everything looks fine", MARK_ON, 0):
        errs.append("self-check: the judge accepts text without the marker")
    if judge(MARK_ON, MARK_OFF, 0):
        errs.append("self-check: the judge confuses the ON and OFF markers")
    if not judge("x\nTRUST-HOST OK 56 checks\n", MARK_ON, 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_defaults(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-trust-gate-")
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例。
        ok, why = build_and_run(cc, pre, env, td, "trust_off_default",
                                [TEST_C, MOD_C], [], MARK_OFF)
        if not ok:
            errs.append("default build is not OFF: %s" % why)
        else:
            # 默认档还必须没有把模块编进去：判 ON 标记不该出现。
            exe = os.path.join(td, "trust_off_default"
                               + (".exe" if os.name == "nt" else ""))
            rc, (out, err) = run([exe], env)
            if MARK_ON in (out + err):
                errs.append("default build reports the ON marker (switch not defaulting off)")

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "trust_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_TRUST_ROOT=0"] + inc
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

        # C 组：开档时模块本体与三处接线必须编得过（-fsyntax-only）。
        on_defs = TRUST_ON_DEFS + lfs + bus + [SIGN_KEY_DEF]
        for rel in WIRED:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # D 组之二：模块用例开档全过。
        ok, why = build_and_run(cc, pre, env, td, "trust_on",
                                [TEST_C, MOD_C], TRUST_ON_DEFS, MARK_ON)
        if not ok:
            errs.append("ON case failed: %s" % why)

        # D 组之三：sec 集成那一档（四态报告：PASS / FINDING / FAIL / SKIP）。
        sec_defs = ["-DARKRTOS_USE_SECCHECK=1",
                    "-DARKRTOS_USE_IMAGE_CRC_RECHECK=1",
                    "-DARKRTOS_USE_IMAGE_SIGN=1",
                    "-DARKRTOS_HW_SAFE=1", "-DARKRTOS_WDG_ENABLE=1",
                    "-DARKRTOS_USE_RTSTAT=1", SIGN_KEY_DEF] + TRUST_ON_DEFS
        ok, why = build_and_run(cc, pre, env, td, "sec_trust",
                                [SEC_TEST_C, SEC_C, MOD_C], sec_defs, MARK_SEC)
        if not ok:
            errs.append("sec integration case failed: %s" % why)

        # E 组：变异自检。每个变异都必须让对应用例变红。
        muts = [
            # ① 锁存不置位：换钥之后仍然放行 = 信任根形同虚设。
            (MOD_C, "            g_locked = 1u;\n", "            g_locked = 0u;\n",
             "latch", [TEST_C, "%%MUT%%"], TRUST_ON_DEFS, MARK_ON,
             "the latch is not asserted on a key mismatch"),
            # ② 读不到钉存槽却报 MATCH：把"没得看"读成"看过了没问题"。
            (MOD_C, "        g_status.pin = ARKRTOS_TRUST_PIN_UNAVAILABLE;\n",
             "        g_status.pin = ARKRTOS_TRUST_PIN_MATCH;\n",
             "unavailable", [TEST_C, "%%MUT%%"], TRUST_ON_DEFS, MARK_ON,
             "an unreadable pin store is reported as MATCH"),
            # ③ sec 把"写不进去"报成 PASS：报告层面把没建立的根说成已建立。
            (SEC_C,
             "        else if(st.pin == (uint32)ARKRTOS_TRUST_PIN_WRITEFAIL)\n"
             "        {\n"
             "            sec_add(p_rep, \"trust.root\", (uint32)ARKRTOS_SEC_SKIP,",
             "        else if(st.pin == (uint32)ARKRTOS_TRUST_PIN_WRITEFAIL)\n"
             "        {\n"
             "            sec_add(p_rep, \"trust.root\", (uint32)ARKRTOS_SEC_PASS,",
             "secpass", [SEC_TEST_C, "%%MUT%%", MOD_C], sec_defs, MARK_SEC,
             "sec reports a failed pin write as PASS"),
        ]
        for rel, old, new, tag, srcs, defs, marker, desc in muts:
            path, why = mutate(rel, old, new, td, tag)
            if path is None:
                errs.append("mutation not applied: %s" % why)
                continue
            real = [path if s == "%%MUT%%" else s for s in srcs]
            ok, why2 = build_and_run(cc, pre, env, td, "mut_" + tag,
                                     real, defs, marker, expect_ok=False)
            if not ok:
                errs.append("mutation survived (%s): %s" % (desc, why2))
    finally:
        if not args.keep:
            shutil.rmtree(td, ignore_errors=True)
        else:
            print("kept: %s" % td)

    if errs:
        for e in errs:
            print("FAIL: %s" % e, file=sys.stderr)
        print("TRUST-GATE FAILED (%d)" % len(errs))
        return 1

    print("TRUST-GATE OK  default=OFF, OFF object has no symbols, "
          "module + 3 wirings (init/loader/sec) compile with the switch ON, "
          "host cases OFF/ON and the sec 4-state report pass, "
          "3 mutations (latch / unavailable / skip-as-pass) all turn red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
