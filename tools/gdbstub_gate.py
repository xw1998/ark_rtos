#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GDB 远程串行协议（RSP）代理的宿主门禁。

被测对象是 kernelsrc/src/arkrtos_gdbstub.c：一个逐字节吃串口、只做现场勘验
（读内存、读写寄存器、监视命令）的目标侧代理。它默认关
（ARKRTOS_USE_GDBSTUB=0），关档时模块编成空 TU —— 只跑默认构建等于什么都没验。
这里分五组判据：

  A 默认档与配置：features.h 的 #ifndef 默认值必须是 0，Kconfig 一项
    default n，defconfig 与 autoconf 同口径。默认值被改错时「默认镜像逐字节
    不变」这句话就不成立，而常规构建看不出来。
  B 关档零符号：编 .o 后 nm 不得看到任何已定义的全局符号。
  C 开档编得过：模块本体 + 用例用真 include 做 -fsyntax-only。
  D 宿主用例两档全过：关档打 GDBSTUB-HOST OFF OK；开档打 GDBSTUB-HOST OK。
  E 判据自检（变异）：往源码里种五个已知错误，用例必须变红 ——
    ① hex 编码原地展开（源就是 s_out，先写后读把没读的字节覆盖掉）；
    ② 不支持的包回 `OK`（把"不支持"伪装成"成功"）；
    ③ QStartNoAckMode 先关 ack 再回 OK（那一包自己的 '+' 会丢）；
    ④ 校验和错回 '+' 而不是 '-'（把坏包当收下）；
    ⑤ p 按大端拼（寄存器字节序反了）。
    任何一处改完仍然全绿，说明对应的断言只是装饰。

用法
----
    python tools/gdbstub_gate.py
    python tools/gdbstub_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe --keep

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

MOD_C = "kernelsrc/src/arkrtos_gdbstub.c"
MOD_H = "kernelsrc/include/arkrtos_gdbstub.h"
TEST_C = "kernelsrc/tests/test_gdbstub_host.c"

INCLUDES = [
    "kernelsrc/include",
    "kernelsrc/tests",
    "config",
]

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

MARK_ON = "GDBSTUB-HOST OK"
MARK_OFF = "GDBSTUB-HOST OFF OK"


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


def check_config(errs):
    """A 组：默认档与三处配置必须同口径。"""
    feat = read_text("kernelsrc/include/arkrtos_features.h")
    if "#ifndef ARKRTOS_USE_GDBSTUB" not in feat:
        errs.append("features.h: ARKRTOS_USE_GDBSTUB has no #ifndef default")
    if "#define ARKRTOS_USE_GDBSTUB             0" not in feat:
        errs.append("features.h: ARKRTOS_USE_GDBSTUB default is not 0")

    kc = read_text("config/kconfig/Kconfig.features")
    blk = block_of(kc, "config ARKRTOS_USE_GDBSTUB\n")
    if blk is None:
        errs.append("Kconfig: ARKRTOS_USE_GDBSTUB is gone")
    elif "default n" not in blk:
        errs.append("Kconfig: ARKRTOS_USE_GDBSTUB default is not n")

    dfc = read_text("config/arkrtos_defconfig")
    if "ARKRTOS_USE_GDBSTUB=n" not in dfc:
        errs.append("defconfig: ARKRTOS_USE_GDBSTUB=n missing")

    auto = read_text("kernelsrc/include/arkrtos_autoconf.h")
    if "#define ARKRTOS_USE_GDBSTUB 0" not in auto:
        errs.append("autoconf: ARKRTOS_USE_GDBSTUB is not 0 (regenerate it)")

    hdr = read_text(MOD_H)
    if "ARKRTOS_GDBSTUB_PKT_MAX" not in hdr:
        errs.append("gdbstub.h: ARKRTOS_GDBSTUB_PKT_MAX is gone")
    if "arkrtos_gdbstub_ops_t" not in hdr:
        errs.append("gdbstub.h: the ops table is gone")

    # 源码 pin：这三条无法（或不值得）用变异表达，但一旦回退就会重新出现
    # 已经踩过的坑，所以直接钉在源码形状上。
    mod = read_text(MOD_C)
    if 'gs_send_cstr("OK");\n        s_ack = 0u;' not in mod:
        errs.append("gdbstub.c: QStartNoAckMode must answer OK before clearing ack")
    if "gs_send(s_enc, n * 2u)" not in mod:
        errs.append("gdbstub.c: the hex encoder must send its own s_enc buffer")
    for sig in ["static void gs_m(", "static void gs_M(", "static void gs_X(",
                "static void gs_p(", "static void gs_P(", "static void gs_qrcmd("]:
        if sig not in mod:
            errs.append("gdbstub.c: %s is gone" % sig)


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
    ap = argparse.ArgumentParser(description="Host gate for the GDB RSP agent")
    ap.add_argument("--gcc", help="gcc 可执行文件（默认自动找）")
    ap.add_argument("--keep", action="store_true", help="保留中间产物便于手工复跑")
    args = ap.parse_args()

    for rel in [MOD_C, MOD_H, TEST_C]:
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

    errs = []

    # E 组先跑：判据本身不成立时，后面的「通过」全部不可信。
    if judge("", MARK_ON, 1):
        errs.append("self-check: the judge accepts a non-zero exit")
    if judge("everything looks fine", MARK_ON, 0):
        errs.append("self-check: the judge accepts text without the marker")
    if judge(MARK_ON, MARK_OFF, 0):
        errs.append("self-check: the judge confuses the ON and OFF markers")
    if not judge("x\nGDBSTUB-HOST OK 193 checks\n", MARK_ON, 0):
        errs.append("self-check: the judge rejects a real ON line")

    check_config(errs)

    td = tempfile.mkdtemp(prefix="arkrtos-gdbstub-gate-")
    try:
        # D 组之一：默认档（不给任何 -D）必须落到 OFF 档用例，
        # 而且不能顺手把模块的 ON 标记打出来。
        ok, why = build_and_run(cc, pre, env, td, "gdbstub_off_default",
                                [TEST_C, MOD_C], [], MARK_OFF)
        if not ok:
            errs.append("default build is not OFF: %s" % why)
        else:
            exe = os.path.join(td, "gdbstub_off_default"
                               + ("." + "exe" if os.name == "nt" else ""))
            rc, (out, err) = run([exe], env)
            if MARK_ON in (out + err):
                errs.append("default build reports the ON marker (switch not defaulting off)")

        # B 组：关档对象里不许有任何已定义的全局符号。
        obj = os.path.join(td, "gdbstub_off.o")
        rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-O0",
                                           "-DARKRTOS_USE_GDBSTUB=0"] + inc
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

        # C 组：开档时模块本体与用例必须编得过（-fsyntax-only）。
        on_defs = ["-DARKRTOS_USE_GDBSTUB=1"]
        for rel in [MOD_C, TEST_C]:
            rc, (out, err) = run([cc] + pre + ["-std=gnu11", "-fsyntax-only"]
                                 + inc + on_defs + [rel], env)
            if rc != 0:
                tail = (err.strip().splitlines() or [""])[-1]
                errs.append("ON syntax failed in %s: %s" % (rel, tail[:200]))

        # D 组之二：开档用例全过。
        ok, why = build_and_run(cc, pre, env, td, "gdbstub_on",
                                [TEST_C, MOD_C], on_defs, MARK_ON)
        if not ok:
            errs.append("ON case failed: %s" % why)

        # E 组：变异自检。每个变异都必须让对应用例变红。
        muts = [
            # ① hex 编码原地展开：源正是 s_out 时把还没读的字节覆盖掉。
            (MOD_C,
             "static uint8  s_enc[GS_OUT_MAX];\n",
             "static uint8  s_enc[GS_OUT_MAX];\n"
             "#define s_enc s_out\n",
             "enc_inplace", [TEST_C, "%%MUT%%"], on_defs, MARK_ON,
             "hex encoding expands in place over its own source"),
            # ② 不支持的包回 OK：把"不支持"伪装成"成功"。
            (MOD_C,
             "    s_stat.unsupported++;\n"
             "    gs_send_empty();\n"
             "}\n"
             "\n"
             "static void gs_handle(const uint8 *p, uint32 n)",
             "    s_stat.unsupported++;\n"
             '    gs_send_cstr("OK");\n'
             "}\n"
             "\n"
             "static void gs_handle(const uint8 *p, uint32 n)",
             "q_as_ok", [TEST_C, "%%MUT%%"], on_defs, MARK_ON,
             "an unsupported Q packet is answered OK"),
            # ③ no-ack 切换失效：之后的包仍会带 '+'。
            (MOD_C,
             "        s_ack = 0u;\n"
             "        s_stat.ack = 0u;\n",
             "        s_ack = 1u;\n"
             "        s_stat.ack = 1u;\n",
             "ack_stuck", [TEST_C, "%%MUT%%"], on_defs, MARK_ON,
             "no-ack mode never takes effect"),
            # ④ 坏校验和回 '+'：把坏包当收下。
            (MOD_C,
             "        if (s_ack != 0u)\n"
             "        {\n"
             "            gs_put((uint8)'-');\n"
             "        }\n"
             "        return;\n",
             "        if (s_ack != 0u)\n"
             "        {\n"
             "            gs_put((uint8)'+');\n"
             "        }\n"
             "        return;\n",
             "badcs_ok", [TEST_C, "%%MUT%%"], on_defs, MARK_ON,
             "a bad checksum is acked instead of nacked"),
            # ⑤ p 按大端拼：寄存器字节序反了。
            (MOD_C,
             "        b[i] = (uint8)(val >> (i * 8u));",
             "        b[i] = (uint8)(val >> ((3u - i) * 8u));",
             "be_reg", [TEST_C, "%%MUT%%"], on_defs, MARK_ON,
             "a register is returned big-endian"),
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
        print("GDBSTUB-GATE FAILED (%d)" % len(errs))
        return 1

    print("GDBSTUB-GATE OK  default=OFF, OFF object has no symbols, "
          "module + host case compile with the switch ON, "
          "host cases OFF/ON pass, 5 mutations (in-place hex encode / "
          "unsupported-as-OK / ack-stuck / bad-csum-as-OK / big-endian reg) "
          "all turn red")
    return 0


if __name__ == "__main__":
    sys.exit(main())