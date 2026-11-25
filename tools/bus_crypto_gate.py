#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""软总线侧信道加密（ARKRTOS_USE_BUS_CRYPTO/ENCRYPT/REPLAY）的门禁。

为什么需要这一道
----------------
串口与网络是这条总线上唯一"别人也能读"的一段线。组件里本来就有三档能力
（HMAC 标签 / ChaCha20 载荷加密 / 16 位滑动窗口重放保护），它们编得进、跑
得动，也正因为如此，最容易被接成"看起来开了、其实没生效"：

  1. 开关没接线：Kconfig 拨到 1，组件宏还是 0（或反过来）—— 两层的 struct
     布局不一致，密钥字段错位，链路时好时坏，报错还落在别处；
  2. 默认档多出代码：关档的目标里仍留着 crypto 符号与 "bus key" 文案 ——
     "关着"只是没调用，ROM 与攻击面都还在；
  3. 自检在"没装钥"的配置下跑：crypto 段全程打印"跳过"，于是"自检通过"
     证明的是"没开也能过"；
  4. 篡改检测被 CRC 冒充：CRC 不含密钥、攻击者随手就能重算，只翻一个线上
     字节再喂回去，先被 CRC 挡下，看起来"篡改被拒"，其实标签一次都没验过。

所以判据全部落在可证伪的读数上：默认档的宏与符号表、两层宏不一致时必须编
不过、以及在 three 档（full / auth-only / off）下真编真跑的两组宿主用例。

判据（全过才 0）
----------------
  A. 默认关：`arkrtos_autoconf.h` 三项必须是 0；`config/arkrtos_defconfig`
     三项 `=n`；`Kconfig.features` 里三项都存在且 `default n`，ENCRYPT /
     REPLAY `depends on ARKRTOS_USE_BUS_CRYPTO`；`arkrtos_softbus.h` 有把
     `ARK_BUS_*_ENABLED` 与 `ARKRTOS_USE_BUS_*` 绑死的 `#error`。
  B. 两层一致（两个方向都测）：`-DARK_BUS_CRYPTO_ENABLED=1` 而
     `ARKRTOS_USE_BUS_CRYPTO=0` 时 `arkrtos_softbus.c` **必须编不过**（并报
     出那条 #error）；两边都给 1 时必须编得过。
  C. 关档零符号：默认档编 `arkrtos_softbus.c` 成 .o，符号表里不许出现
     `ark_bus_crypto_*` / `ark_bus_replay_*` / `ark_bus_chacha20_*` /
     `ark_bus_sha256` / `ark_bus_hmac_sha256`，目标文件里也不许有 "bus key"
     这个子命令文案；同一份源开档编时必须出现 `ark_bus_crypto_set_key`
     （只测一向的话，"开关写成 1 但代码没包住"和"根本没编"长得一样）。
  D. 构建侧接线：三个 shared-autoconf 的 Makefile（la32_generic /
     qemu_loongarch64 / qemu_riscv32）必须从 autoconf 取三个值再 -D 下去；
     F427 GCC Makefile 与 Keil 工程必须给出 `ARK_BUS_*_ENABLED`；
     `tools/arkrtos_f427_env.py` 的 Tier 定义里必须三项都为 0。
  E. 宿主矩阵（2 组用例 × 3 档，全过才算过）：
       - `test_bus_crypto_host.c`：full 95 checks / auth-only 71 / off 走
         "off" 分支；向量（SHA-256、RFC 4231 HMAC、RFC 8439 ChaCha20）、
         无钥明文不变、只装一端 → E_AUTH、两端同钥还原、错钥拒绝后换钥通过、
         重放拒绝。
       - `test_softbus_host.c`：full 127 / auth-only 127 / off 84 checks。
         成功行必须自带档位（`crypto=1 encrypt=1 replay=1` 这种），否则"跑
         的是哪一档"又只能靠猜。full 档还要求内核自检里的 crypto 段真跑
         （不是"keyed checks skipped"），这一条由用例内部的 CK 保证；门禁
         在这里再核一次"档位标签与期望逐字相等"，防止把 -D 写错档还判绿。
  F. 判据自检：把四条合成的坏输出（少 OK / 档位标签错 / 计数错 / off 档
     却有计数）喂给同一个判定函数，四种都必须判红。

覆盖：三档组合的接线、默认档零成本、两层宏一致性、以及加密/认证/重放的
行为面（密文、标签、错钥、重放、篡改）。明确不覆盖：物理串口上的两个节点、
真节拍下的密钥轮换、以及"密钥从哪来"（那是部署问题，不是本开关的问题）。

用法
----
    python tools/bus_crypto_gate.py
    python tools/bus_crypto_gate.py --cc D:/mingw/MinGW/bin/gcc.exe
    python tools/bus_crypto_gate.py --keep      # 保留中间产物

退出码 0 = 全过；1 = 有一项红。
"""

import argparse
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from tempdir_util import TempDir  # noqa: E402

CC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
    os.environ.get("ARKRTOS_HOST_CC", ""),
]

AUTOCONF = os.path.join(ROOT, "kernelsrc", "include", "arkrtos_autoconf.h")
DEFCONFIG = os.path.join(ROOT, "config", "arkrtos_defconfig")
KCONFIG = os.path.join(ROOT, "config", "kconfig", "Kconfig.features")
HEADER = os.path.join(ROOT, "kernelsrc", "include", "arkrtos_softbus.h")
SRC = os.path.join("kernelsrc", "src", "arkrtos_softbus.c")

SWITCHES = ("ARKRTOS_USE_BUS_CRYPTO", "ARKRTOS_USE_BUS_ENCRYPT",
            "ARKRTOS_USE_BUS_REPLAY")
COMPONENT_MACROS = ("ARK_BUS_CRYPTO_ENABLED", "ARK_BUS_ENCRYPT_ENABLED",
                    "ARK_BUS_REPLAY_ENABLED")

# 用例跑的是这一个"小板"档，与 ci_gate / coverage_gate 里 softbus 用例同参。
TIER_DEFS = [
    "-DARK_BUS_MAX_TOPICS=12",
    "-DARK_BUS_MAX_DEVICES=4",
    "-DARK_BUS_MSG_MAX_SIZE=128",
    "-DARK_BUS_TOPIC_NAME_MAX=24",
    "-DARK_BUS_QOS_MAX_SESSIONS=2",
    "-DARK_BUS_RPC_MAX_SLOTS=4",
    "-DARK_BUS_RPC_MAX_METHODS=4",
    "-DARK_BUS_TASK_ENABLED=0",
    "-DARK_BUS_COLL_ENABLED=0",
    "-DARK_BUS_INFER_ENABLED=0",
    "-DARK_BUS_STREAM_TX_SLOTS=1",
    "-DARK_BUS_STREAM_RX_SLOTS=1",
    "-DARK_BUS_STREAM_RX_MAX=256",
    "-DARK_BUS_STREAM_MAX_BYTES=256",
]

SOFTBUS_INCS = [
    "-Ikernelsrc/include",
    "-Ikernelsrc/components/ark_softbus/include",
    "-Ikernelsrc/components/ark_softbus/src",
    "-Ikernelsrc/components/ark_softbus/port",
    "-Ikernelsrc/shell",
    "-Ikernelsrc/shell/ark_shell",
    "-Ikernelsrc/components/mdk_trace",
    "-Iboard/stm32f427",
    "-Iconfig",
]

COMPONENT_INCS = [
    "-Ikernelsrc/components/ark_softbus/include",
    "-Ikernelsrc/components/ark_softbus/src",
    "-Ikernelsrc/components/ark_softbus/port",
]

COMPONENT_LINK = [
    "kernelsrc/components/ark_softbus/src/transport/transport_loopback.c",
]

FAILS = []
PASSES = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-26s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-26s %s" % (name, detail))


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


def find_cc(explicit):
    for cand in ([explicit] if explicit else []) + CC_FALLBACKS:
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
    """MinGW 的 gcc 要 -B<dir> 才找得到自己的 cc1 / 运行库。"""
    if "mingw" in cc.replace("\\", "/").lower():
        return ["-B" + os.path.dirname(cc)]
    return []


def tool_next_to(cc, name):
    d = os.path.dirname(cc)
    for cand in (os.path.join(d, name + ".exe"), os.path.join(d, name)):
        if os.path.isfile(cand):
            return cand
    return shutil.which(name)


def run(cmd, timeout=600):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, timeout=timeout)

    def dec(b):
        for enc in ("utf-8", "gbk"):
            try:
                return b.decode(enc)
            except Exception:
                pass
        return b.decode("latin-1")

    return r.returncode, dec(r.stdout), dec(r.stderr)


# ------------------------------------------------------------------ A
def check_default_off():
    text = read_text(AUTOCONF)
    miss = [s for s in SWITCHES if ("#define %s 0" % s) not in text]
    if miss:
        bad("default-off", "autoconf 里不是 0：%s" % ", ".join(miss))
    else:
        ok("default-off", "autoconf 三项都是 0（%d symbols 里查的）" %
           text.count("#define "))

    text = read_text(DEFCONFIG)
    miss = [s for s in SWITCHES if ("%s=n" % s) not in text]
    if miss:
        bad("defconfig", "缺少或不是 n：%s" % ", ".join(miss))
    else:
        ok("defconfig", "三项均为 =n")

    text = read_text(KCONFIG)
    problems = []
    for s in SWITCHES:
        if ("config %s" % s) not in text:
            problems.append("%s 不存在" % s)
    # ENCRYPT / REPLAY 必须在 CRYPTO 之下；退化成各开各的会造出
    # "加密开、认证关" 这种组件里没有的档位。
    for s in SWITCHES[1:]:
        head = text.find("config %s" % s)
        if head < 0:
            continue
        nxt = text.find("\nconfig ", head)
        block = text[head:nxt if nxt > 0 else len(text)]
        if "depends on ARKRTOS_USE_BUS_CRYPTO" not in block:
            problems.append("%s 没 depends on ARKRTOS_USE_BUS_CRYPTO" % s)
        if "default n" not in block:
            problems.append("%s 不是 default n" % s)
    if problems:
        bad("kconfig", "；".join(problems))
    else:
        ok("kconfig", "三项都在、ENCRYPT/REPLAY 挂在 CRYPTO 下、默认 n")

    text = read_text(HEADER)
    problems = []
    for m, s in zip(COMPONENT_MACROS, SWITCHES):
        if m not in text or s not in text:
            problems.append("%s/%s" % (m, s))
    if "#error" not in text:
        problems.append("没有 #error")
    if problems:
        bad("header-guard", "一致性闸门缺：%s" % ", ".join(problems))
    else:
        ok("header-guard", "三个组件宏与三个 Kconfig 符号被 #error 绑死")


# ------------------------------------------------------------------ B / C
def compile_obj(cc, out, extra_defs, extra_incs=None):
    cmd = [cc] + mingw_prefix(cc) + [
        "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
    ] + (extra_incs or SOFTBUS_INCS) + [
        "-DARKRTOS_USE_SOFTBUS=1",
    ] + TIER_DEFS + extra_defs + ["-c", SRC, "-o", out]
    return run(cmd)


def check_consistency(cc, tmp):
    """两层宏不一致时必须编不过 —— 这是"静默内存破坏"的唯一闸门。"""
    obj = os.path.join(tmp, "mismatch.o")
    rc, _, err = compile_obj(cc, obj,
                             ["-DARK_BUS_CRYPTO_ENABLED=1",
                              "-DARK_BUS_ENCRYPT_ENABLED=0",
                              "-DARK_BUS_REPLAY_ENABLED=0"])
    if rc == 0:
        bad("consistency-guard", "-DARK_BUS_CRYPTO_ENABLED=1 而 "
            "ARKRTOS_USE_BUS_CRYPTO=0 竟然编过了")
    elif "#error" not in err:
        bad("consistency-guard", "编不过但原因不是 #error：%s" %
            err.strip().splitlines()[-1:] or "?")
    else:
        ok("consistency-guard", "两层宏不一致时 #error 拦下（不是靠链接期）")

    obj = os.path.join(tmp, "agree.o")
    rc, _, err = compile_obj(cc, obj,
                             ["-DARKRTOS_USE_BUS_CRYPTO=1",
                              "-DARKRTOS_USE_BUS_ENCRYPT=1",
                              "-DARKRTOS_USE_BUS_REPLAY=1",
                              "-DARK_BUS_CRYPTO_ENABLED=1",
                              "-DARK_BUS_ENCRYPT_ENABLED=1",
                              "-DARK_BUS_REPLAY_ENABLED=1"])
    if rc != 0:
        bad("consistency-agree", "两边都给 1 却编不过：%s" % err.strip()[-300:])
    else:
        ok("consistency-agree", "两层宏一致时编得过（full 档）")


FORBIDDEN = ("ark_bus_crypto_", "ark_bus_replay_", "ark_bus_chacha20_",
             "ark_bus_sha256", "ark_bus_hmac_sha256")
REQUIRED_ON = ("ark_bus_crypto_set_key",)


def check_symbols(cc, nm, tmp):
    obj = os.path.join(tmp, "off.o")
    rc, _, err = compile_obj(cc, obj, [
        "-DARK_BUS_CRYPTO_ENABLED=0",
        "-DARK_BUS_ENCRYPT_ENABLED=0",
        "-DARK_BUS_REPLAY_ENABLED=0",
    ])
    if rc != 0:
        bad("off-symbols", "关档编不出来：%s" % err.strip()[-300:])
        return
    rc, out, _ = run([nm, "-u", obj])
    hit = sorted(set(s.strip().lstrip("_") for s in out.splitlines()
                     if s.strip() and any(f in s for f in FORBIDDEN)))
    if hit:
        bad("off-symbols", "关档仍有加密符号：%s" % ", ".join(hit[:6]))
    else:
        ok("off-symbols", "关档 .o 没有 crypto / replay / chacha / sha256 引用")

    raw = b""
    with open(obj, "rb") as f:
        raw = f.read()
    if b"bus key" in raw:
        bad("off-strings", "关档 .o 里还有 `bus key` 子命令文案")
    else:
        ok("off-strings", "关档 .o 里没有 `bus key`（子命令确实没编进去）")

    obj = os.path.join(tmp, "on.o")
    rc, _, err = compile_obj(cc, obj, [
        "-DARKRTOS_USE_BUS_CRYPTO=1",
        "-DARKRTOS_USE_BUS_ENCRYPT=1",
        "-DARKRTOS_USE_BUS_REPLAY=1",
        "-DARK_BUS_CRYPTO_ENABLED=1",
        "-DARK_BUS_ENCRYPT_ENABLED=1",
        "-DARK_BUS_REPLAY_ENABLED=1",
    ])
    if rc != 0:
        bad("on-symbols", "开档编不出来：%s" % err.strip()[-300:])
        return
    rc, out, _ = run([nm, "-u", obj])
    miss = [s for s in REQUIRED_ON if s not in out]
    if miss:
        bad("on-symbols", "开档却引用不到：%s" % ", ".join(miss))
    else:
        ok("on-symbols", "开档 .o 引用了 %s" % ", ".join(REQUIRED_ON))


# ------------------------------------------------------------------ D
MAKEFILES = [
    "example/la32_generic/Makefile",
    "example/qemu_loongarch64/Makefile",
    "example/qemu_riscv32/Makefile",
]


def check_wiring():
    problems = []
    for rel in MAKEFILES:
        text = read_text(os.path.join(ROOT, rel))
        for s in SWITCHES:
            if ("arkrtos_autoconf.h),0)" not in text and
                    ("/^#define %s /" % s) not in text):
                problems.append("%s 没从 autoconf 取 %s" % (rel, s))
        for m in COMPONENT_MACROS:
            if ("-D%s=$(" % m) not in text:
                problems.append("%s 没把 %s 传给 -D" % (rel, m))
    if problems:
        bad("build-wiring", "；".join(problems[:4]))
    else:
        ok("build-wiring", "三个 Makefile 都从 autoconf 取三项并 -D 下去")

    problems = []
    text = read_text(os.path.join(
        ROOT, "example/stm32f427/kernel/ARKRTOS_TEST/GCC/Makefile"))
    for m in COMPONENT_MACROS:
        if ("-D%s=" % m) not in text:
            problems.append("GCC Makefile 缺 %s" % m)
    for s in SWITCHES:
        if ("%s=$(" % s) not in text:
            problems.append("GCC Makefile 没把 %s 交给 autoconf" % s)
    text = read_text(os.path.join(
        ROOT, "example/stm32f427/kernel/ARKRTOS_TEST/MDK-ARM/ARKRTOS_TEST.uvprojx"))
    for m in COMPONENT_MACROS:
        if ("-D%s=0" % m) not in text:
            problems.append("uvprojx 缺 -D%s=0" % m)
    if problems:
        bad("build-wiring-f427", "；".join(problems[:4]))
    else:
        ok("build-wiring-f427", "F427 的 GCC Makefile 与 Keil 工程都给了三个宏（默认 0）")

    text = read_text(os.path.join(ROOT, "tools", "arkrtos_f427_env.py"))
    miss = [m for m in COMPONENT_MACROS if ('"%s=0"' % m) not in text]
    if miss:
        bad("build-wiring-env", "f427_env.py 的 Tier 定义缺：%s" % ", ".join(miss))
    else:
        ok("build-wiring-env", "f427_env.py 默认 Tier 三项都是 0")


def check_makefile_hash():
    """Makefile 里 $(shell ...) 内的 # 会被当成注释起点，整行被截断成
    "unterminated call to function" ——而 build-wiring 只 grep 文本，看不出来。
    这条把「不写 #」变成可判的：三项都必须从 autoconf 取，且取值那行不能
    带 #（否则 make 解析不了，所有用到这个 Makefile 的门禁都会红）。"""
    problems = []
    for rel in MAKEFILES:
        for i, ln in enumerate(read_text(os.path.join(ROOT, rel)).splitlines(), 1):
            if "$(shell awk" not in ln:
                continue
            if "#" in ln:
                problems.append("%s:%d $(shell ...) 里有未转义的 #" % (rel, i))
            if "arkrtos_autoconf.h" not in ln:
                problems.append("%s:%d 没读 autoconf.h" % (rel, i))
    if problems:
        bad("makefile-hash", "；".join(problems[:4]))
    else:
        ok("makefile-hash",
           "三个 Makefile 的 autoconf 取值行都无未转义 #（make 能解析）")

# ------------------------------------------------------------------ E
def judge_softbus(out, tier):
    """返回 (是否通过, 说明)。契约写死在这里，坏样本自检也用同一个。"""
    want = {
        "full": "SOFTBUS-HOST OK  127 checks (crypto=1 encrypt=1 replay=1)",
        "auth": "SOFTBUS-HOST OK  127 checks (crypto=1 encrypt=0 replay=0)",
        "off": "SOFTBUS-HOST OK  84 checks (crypto=0 encrypt=0 replay=0)",
    }[tier]
    if "SOFTBUS-HOST FAILED" in out:
        return False, "有用例失败"
    if want not in out:
        return False, "成功行不是 %r" % want
    return True, want


def judge_crypto(out, tier):
    want = {
        "full": "BUS-CRYPTO-HOST OK  95 checks (crypto=1 encrypt=1 replay=1)",
        "auth": "BUS-CRYPTO-HOST OK  71 checks (crypto=1 encrypt=0 replay=0)",
        "off": "BUS-CRYPTO-HOST OK  off",
    }[tier]
    if "BUS-CRYPTO-HOST FAILED" in out or "checks failed" in out:
        return False, "有用例失败"
    if want not in out:
        return False, "成功行不是 %r" % want
    return True, want


TIER_DEFS_MATRIX = {
    "full": ["-DARKRTOS_USE_BUS_CRYPTO=1",
             "-DARKRTOS_USE_BUS_ENCRYPT=1",
             "-DARKRTOS_USE_BUS_REPLAY=1",
             "-DARK_BUS_CRYPTO_ENABLED=1",
             "-DARK_BUS_ENCRYPT_ENABLED=1",
             "-DARK_BUS_REPLAY_ENABLED=1"],
    "auth": ["-DARKRTOS_USE_BUS_CRYPTO=1",
             "-DARK_BUS_CRYPTO_ENABLED=1"],
    "off": [],
}


def build_and_run(cc, exe, srcs, incs, defs, cwd=ROOT):
    cmd = [cc] + mingw_prefix(cc) + [
        "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2",
    ] + incs + defs + srcs + ["-o", exe]
    rc, out, err = run(cmd)
    if rc != 0:
        return None, (err.strip() or out.strip())[-800:]
    rc, out, err = run([exe])
    return out + err, None


def component_sources(with_port=False):
    """组件源集合。

    with_port 要分开：组件级用例自带 HAL 钩子（它要一个确定性时钟），接上
    port 胶水反而会缺 ark_bus_port_tick_ms 这些板级符号；内核级用例反过来，
    它自己的 arkrtos_softbus.c 就是靠 port 胶水接组件的。两份不能混。"""
    d = os.path.join(ROOT, "kernelsrc/components/ark_softbus/src")
    names = sorted(n for n in os.listdir(d)
                   if n.startswith("ark_bus_") and n.endswith(".c"))
    srcs = ["kernelsrc/components/ark_softbus/src/" + n for n in names] + COMPONENT_LINK
    if with_port:
        srcs.append("kernelsrc/components/ark_softbus/port/ark_bus_hal_arkrtos.c")
    return srcs


def check_host_matrix(cc, tmp):
    comp = component_sources()
    for tier in ("full", "auth", "off"):
        exe = os.path.join(tmp, "bus_crypto_%s.exe" % tier)
        out, why = build_and_run(
            cc, exe,
            ["kernelsrc/tests/test_bus_crypto_host.c"] + comp,
            COMPONENT_INCS, TIER_DEFS_MATRIX[tier])
        if out is None:
            bad("crypto-host:%s" % tier, "编译失败：%s" % why)
            continue
        good, note = judge_crypto(out, tier)
        if good:
            ok("crypto-host:%s" % tier, note)
        else:
            bad("crypto-host:%s" % tier, "%s\n%s" % (note, out[-900:]))

    link = component_sources(with_port=True)
    for tier in ("full", "auth", "off"):
        exe = os.path.join(tmp, "softbus_%s.exe" % tier)
        defs = ["-DARKRTOS_USE_SOFTBUS=1"] + TIER_DEFS_MATRIX[tier]
        out, why = build_and_run(
            cc, exe,
            ["kernelsrc/tests/test_softbus_host.c",
             "kernelsrc/src/arkrtos_softbus.c"] + link,
            SOFTBUS_INCS, defs + TIER_DEFS)
        if out is None:
            bad("softbus-host:%s" % tier, "编译失败：%s" % why)
            continue
        good, note = judge_softbus(out, tier)
        if good:
            ok("softbus-host:%s" % tier, note)
        else:
            bad("softbus-host:%s" % tier, "%s\n%s" % (note, out[-900:]))


# ------------------------------------------------------------------ F
def self_check():
    """判定函数自己也要能被证伪：四种坏输出都必须判红。"""
    good = ("SOFTBUS-HOST OK  127 checks (crypto=1 encrypt=1 replay=1)\n")
    cases = [
        ("少 OK 行", ""),
        ("档位标签错", "SOFTBUS-HOST OK  127 checks (crypto=1 encrypt=0 replay=0)"),
        ("计数错", "SOFTBUS-HOST OK  125 checks (crypto=1 encrypt=1 replay=1)"),
        ("off 档却有计数", "SOFTBUS-HOST OK  84 checks (crypto=0 encrypt=0 replay=1)"),
        ("有 FAILED", "SOFTBUS-HOST FAILED  1/127 checks failed"),
    ]
    missed = [name for name, txt in cases if judge_softbus(txt, "full")[0]]
    if judge_softbus(good, "full")[0] is False:
        missed.append("好输出被判红")
    if missed:
        bad("parser-selfcheck", "这些坏样本没被判红：%s" % ", ".join(missed))
    else:
        ok("parser-selfcheck", "%d 种坏输出全被判红、好输出判绿" % len(cases))


def _checks(cc, nm, tmp):
    check_consistency(cc, tmp)
    if nm:
        check_symbols(cc, nm, tmp)
    else:
        bad("off-symbols", "找不到 nm（应与 gcc 同目录，或 --nm 指定）")
    check_host_matrix(cc, tmp)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default=None)
    ap.add_argument("--nm", default=None)
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args(argv)

    cc = find_cc(a.cc)
    if not cc:
        print("FAIL  compiler                 找不到宿主 gcc（可用 --cc 指定）")
        print("\n0 passed, 1 failed")
        return 1
    nm = a.nm or tool_next_to(cc, "nm")

    print("cc = %s" % cc)
    check_default_off()
    check_wiring()
    check_makefile_hash()

    if a.keep:
        import tempfile

        tmp = tempfile.mkdtemp(prefix="bus_crypto_gate_")
        print("tmp = %s（--keep：跑完不删）" % tmp)
        _checks(cc, nm, tmp)
    else:
        with TempDir() as tmp:
            _checks(cc, nm, tmp)

    self_check()

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if not FAILS:
        print("BUS-CRYPTO-GATE OK")
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
