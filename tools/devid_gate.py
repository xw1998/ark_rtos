#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""设备身份（SEC-G13）的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_devid.c：开机时读一次芯片唯一号与每机密钥，
派生出公开 ID（只命名），并在需要时用 HMAC 回答一个挑战（才证明）。它默认关
（ARKRTOS_USE_DEVICE_ID=0），关档时模块编成空 TU，init / sec / shell 三处接线
也不参与编译 —— 只跑默认构建等于什么都没验。这里分五组判据：

  A 默认档与配置：features.h 的默认值必须是 0，ID 长度必须带 8..32 的 #error；
    Kconfig 一项 default n + 一项 range 8 32；defconfig 与 autoconf 同口径；
    三处接线（init 的开机调用、sec 的第 15 项、shell 的 devid 命令）都必须在。
    默认值被改错时「默认镜像逐字节不变」这句话就不成立，而常规构建看不出来。
  B 关档零符号：编成 .o 后 nm 不得看到任何已定义的全局符号。
  C 开档编得过：模块本体 + 三处接线用真 include 做 -fsyntax-only。
  D 宿主用例四档全过：关档打 DEVID-HOST OFF OK；开档打 DEVID-HOST OK；
    内建密钥档打 DEVID-HOST BUILTIN OK；再把 sec 集成两档（PASS/FINDING/
    SKIP 三态）各跑一遍。
  E 判据自检（变异）：往源码里种四个已知错误，用例必须变红 ——
    ① 没有密钥却回一个全零 MAC（把"没有身份"伪装成"身份是 0"）；
    ② 编进固件的密钥被报成 OTP（把"人人都有"说成"这台独有"）；
    ③ 短于下限的密钥仍然置 ready（挡住穷举的下限形同虚设）；
    ④ sec 把 BUILTIN 报成 PASS（报告层面把能应答读成身份成立）。
    任何一处改完仍然全绿，说明对应的断言只是装饰。

用法
----
    python tools/devid_gate.py
    python tools/devid_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

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

MOD_C = "kernelsrc/src/arkrtos_devid.c"
TEST_C = "kernelsrc/tests/test_devid_host.c"
SEC_TEST_C = "kernelsrc/tests/test_sec_host.c"
SEC_C = "kernelsrc/src/arkrtos_sec.c"

# 开档要编的接线：每一处都只在 #if ARKRTOS_USE_DEVICE_ID == 1 里出现。
WIRED = [
    "kernelsrc/src/arkrtos_devid.c",
    "kernelsrc/src/arkrtos_init.c",
    "kernelsrc/src/arkrtos_sec.c",
    "kernelsrc/shell/arkrtos_shell_misc.c",
    "kernelsrc/shell/arkrtos_shell.c",
]

# 与 coverage_gate.py 同一张 32 字节内建密钥表（0x01..0x20）。
DEVID_SECRET_DEF = "-DARKRTOS_DEVICE_SECRET={" + ",".join(
    "0x%02X" % b for b in range(1, 33)) + "}"

# 与 coverage_gate.py / test_sign_host.c 同一张 32 字节签名密钥表。
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

MARK_ON = "DEVID-HOST OK"
MARK_OFF = "DEVID-HOST OFF OK"
MARK_BUILTIN = "DEVID-HOST BUILTIN OK"
MARK_SEC = "SEC-HOST OK"

SEC_BASE_DEFS = [
    "-DARKRTOS_USE_SECCHECK=1",
    "-DARKRTOS_USE_IMAGE_CRC_RECHECK=1",
    "-DARKRTOS_USE_IMAGE_SIGN=1",
    "-DARKRTOS_HW_SAFE=1",
    "-DARKRTOS_WDG_ENABLE=1",
    "-DARKRTOS_USE_RTSTAT=1",
    SIGN_KEY_DEF,
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
    """A 组：默认档与三处配置必须同口径，三处接线必须在。"""
    feat = read_text("kernelsrc/include/arkrtos_features.h")
    if "#ifndef ARKRTOS_USE_DEVICE_ID" not in feat:
        errs.append("features.h: ARKRTOS_USE_DEVICE_ID has no #ifndef default")
    if "#define ARKRTOS_USE_DEVICE_ID           0" not in feat:
        errs.append("features.h: ARKRTOS_USE_DEVICE_ID default is not 0")
    if "#ifndef ARKRTOS_DEVICE_ID_LEN" not in feat:
        errs.append("features.h: ARKRTOS_DEVICE_ID_LEN has no #ifndef default")
    # 长度越界必须在编译期挡住：8 字节以下撞车概率不再可以忽略。
    if ('#error "ARKRTOS_DEVICE_ID_LEN must be 8..32"' not in feat):
        errs.append("features.h: the 8..32 range #error is gone")

    kc = read_text("config/kconfig/Kconfig.features")
    blk = block_of(kc, "config ARKRTOS_USE_DEVICE_ID\n")
    if blk is None:
        errs.append("Kconfig: ARKRTOS_USE_DEVICE_ID is gone")
    elif "default n" not in blk:
        errs.append("Kconfig: ARKRTOS_USE_DEVICE_ID default is not n")
    blk2 = block_of(kc, "config ARKRTOS_DEVICE_ID_LEN\n")
    if blk2 is None:
        errs.append("Kconfig: ARKRTOS_DEVICE_ID_LEN is gone")
    else:
        if "range 8 32" not in blk2:
            errs.append("Kconfig: ARKRTOS_DEVICE_ID_LEN lost its range")
        if "default 16" not in blk2:
            errs.append("Kconfig: ARKRTOS_DEVICE_ID_LEN default is not 16")

    dfc = read_text("config/arkrtos_defconfig")
    if "ARKRTOS_USE_DEVICE_ID=n" not in dfc:
        errs.append("defconfig: ARKRTOS_USE_DEVICE_ID=n missing")
    if "ARKRTOS_DEVICE_ID_LEN=16" not in dfc:
        errs.append("defconfig: ARKRTOS_DEVICE_ID_LEN=16 missing")

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_DEVICE_ID 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_DEVICE_ID is not 0 (regenerate it)")
    if "#define ARKRTOS_DEVICE_ID_LEN (16)" not in auto:
        errs.append("autoconf: ARKRTOS_DEVICE_ID_LEN is not (16) (regenerate it)")

    # 三处接线必须真的在（删掉一处，"身份能被读出来 / 被报告 / 被现场查看"
    # 这条链就断了，而默认档编译仍然全绿）。
    init_c = read_text("kernelsrc/src/arkrtos_init.c")
    if "arkrtos_devid_boot()" not in init_c:
        errs.append("init.c: the boot-time identity load is not wired")
    sec_c = read_text(SEC_C)
    if "sec_check_devid" not in sec_c:
        errs.append("sec.c: the device.identity report item is gone")
    sh_c = read_text("kernelsrc/shell/arkrtos_shell.c")
    if '"devid"' not in sh_c:
        errs.append("shell.c: the devid command is not registered")
    shm_c = read_text("kernelsrc/shell/arkrtos_shell_misc.c")
    if "int cmd_devid(" not in shm_c:
        errs.append("shell_misc.c: cmd_devid() is gone")


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
        return None, "mutation anchor not unique in %s: %s (hits=%d)" % (rel, tag, s.count(old))
    path = os.path.join(td, "mut_%s_%s" % (tag, os.path.basename(rel)))
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(s.replace(old, new, 1))
    return path, ""


def main():
    ap = argparse.ArgumentParser(description="Host gate for the SEC-G13 device identity")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留中间产物便于手工复跑")
    args = ap.parse_args()

    needed = [MOD_C, TEST_C, SEC_TEST_C, SEC_C] + WIRED
    for rel in needed:
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
    if not judge("x\nDEVID-HOST OK 48 checks\n", MARK_ON, 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_defaults(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-devid-gate-")
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例，
        # 而且不能顺手把模块编进去（ON 标记不该出现）。
        ok, why = build_and_run(cc, pre, env, td, "devid_off_default",
                                [TEST_C, MOD_C], [], MARK_OFF)
        if not ok:
            errs.append("default build is not OFF: %s" % why)
        else:
            exe = os.path.join(td, "devid_off_default"
                               + ("." + "exe" if os.name == "nt" else ""))
            rc, (out, err) = run([exe], env)
            if MARK_ON in (out + err):
                errs.append("default build reports the ON marker (switch not defaulting off)")

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "devid_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_DEVICE_ID=0"] + inc
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
        on_defs = ["-DARKRTOS_USE_DEVICE_ID=1"] + lfs + bus
        for rel in WIRED:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # D 组之二：模块用例开档全过（无内建密钥）。
        ok, why = build_and_run(cc, pre, env, td, "devid_on",
                                [TEST_C, MOD_C], ["-DARKRTOS_USE_DEVICE_ID=1"],
                                MARK_ON)
        if not ok:
            errs.append("ON case failed: %s" % why)

        # D 组之三：内建密钥档（来源必须是 BUILTIN，且板级密钥会盖过它）。
        ok, why = build_and_run(cc, pre, env, td, "devid_builtin",
                                [TEST_C, MOD_C],
                                ["-DARKRTOS_USE_DEVICE_ID=1", DEVID_SECRET_DEF],
                                MARK_BUILTIN)
        if not ok:
            errs.append("BUILTIN case failed: %s" % why)

        # D 组之四：sec 集成两档（PASS / FINDING / SKIP 三态都要走到）。
        sec_defs = SEC_BASE_DEFS + ["-DARKRTOS_USE_DEVICE_ID=1"]
        ok, why = build_and_run(cc, pre, env, td, "sec_devid",
                                [SEC_TEST_C, SEC_C, MOD_C], sec_defs, MARK_SEC)
        if not ok:
            errs.append("sec integration case failed: %s" % why)
        ok, why = build_and_run(cc, pre, env, td, "sec_devid_builtin",
                                [SEC_TEST_C, SEC_C, MOD_C],
                                sec_defs + [DEVID_SECRET_DEF], MARK_SEC)
        if not ok:
            errs.append("sec integration (builtin) case failed: %s" % why)

        # E 组：变异自检。每个变异都必须让对应用例变红。
        muts = [
            # ① 没有密钥却回一个全零 MAC：把"没有身份"伪装成"身份是 0"。
            (MOD_C,
             "    if(g_status.ready == 0u)\n"
             "    {\n"
             "        return ARKRTOS_DEVID_ERR_NOKEY;\n"
             "    }\n",
             "    if(g_status.ready == 0u)\n"
             "    {\n"
             "        devid_zero(mac, (uint32)ARKRTOS_DEVID_MAC_LEN);\n"
             "        return ARKRTOS_DEVID_OK;\n"
             "    }\n",
             "zeromac", [TEST_C, "%%MUT%%"], ["-DARKRTOS_USE_DEVICE_ID=1"], MARK_ON,
             "a keyless board returns an all-zero MAC instead of refusing"),
            # ② 编进固件的密钥被报成 OTP：把"人人都有"说成"这台独有"。
            (MOD_C,
             "            g_status.source = (uint32)ARKRTOS_DEVID_SRC_BUILTIN;",
             "            g_status.source = (uint32)ARKRTOS_DEVID_SRC_OTP;",
             "builtin", [TEST_C, "%%MUT%%"],
             ["-DARKRTOS_USE_DEVICE_ID=1", DEVID_SECRET_DEF], MARK_BUILTIN,
             "a firmware-wide secret is reported as a per-device OTP secret"),
            # ③ 短于下限的密钥仍然置 ready：挡住穷举的下限形同虚设。
            (MOD_C,
             "        g_status.ready = ((uint32)rc >= DEVID_SECRET_MIN) ? 1u : 0u;",
             "        g_status.ready = 1u;",
             "shortkey", [TEST_C, "%%MUT%%"], ["-DARKRTOS_USE_DEVICE_ID=1"], MARK_ON,
             "a sub-minimum secret is accepted as ready"),
            # ④ sec 把 BUILTIN 报成 PASS：报告层面把"能应答"读成"身份成立"。
            (SEC_C,
             '        sec_add(p_rep, "device.identity", (uint32)ARKRTOS_SEC_FINDING,',
             '        sec_add(p_rep, "device.identity", (uint32)ARKRTOS_SEC_PASS,',
             "secpass", [SEC_TEST_C, "%%MUT%%", MOD_C],
             sec_defs + [DEVID_SECRET_DEF], MARK_SEC,
             "sec reports a firmware-wide secret as PASS"),
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
        print("DEVID-GATE FAILED (%d)" % len(errs))
        return 1

    print("DEVID-GATE OK  default=OFF, OFF object has no symbols, "
          "module + 3 wirings (init/sec/shell) compile with the switch ON, "
          "host cases OFF/ON/BUILTIN and the sec 3-state report pass, "
          "4 mutations (zero-mac / builtin-as-otp / short-key / finding-as-pass) "
          "all turn red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
