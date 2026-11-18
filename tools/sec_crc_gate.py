#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SEC-G17（镜像 CRC 开机复核）门禁：开关默认关、开着能编、关着不留痕、行为不退化。

为什么需要这一道
----------------
`ARKRTOS_USE_IMAGE_CRC_RECHECK` 默认 n，`arkrtos_loader_crc_recheck()` 的实现和
`arkrtos_sec.c` 里调它的那一支平时一个编译器都不经过——即使它编不过、即使它
被改成永远返回 PASS，默认构建也照样全绿。这是和 rtstat / seccheck / paging
同一个假绿形态（默认关 = 没人编译）。

本门禁用真实的 armclang（F427 板级头）编两个 TU，两档都编：
  - `-DARKRTOS_USE_IMAGE_CRC_RECHECK=1`（开档：实现与 sec 的调用点都在）
  - 不给（默认档：新代码整段不参与，必须照样编得过）
缺 armclang 是 FAIL 不是跳过。

另外钉四条**只靠编译发现不了**的约定，每条都带变异自检（把源码复制一份故意改坏，
检查必须变红，否则说明这条 pin 是空转的）：

  1. 开关在 defconfig 里是 `n`，Kconfig 里 `default n` 且依赖 SECCHECK
     —— 开关不能自己打开（"新功能默认关"）。
  2. `load_buffer` 必须把**重定位表**也折进 `crc_written`。
     漏掉它会怎样：回读复核比的是 `arkrtos_loader_image_crc()`（头 + 表 + 负载），
     而 `crc_written` 只算头 + 负载 ⇒ 任何 reloc_count > 0 的镜像安装后恒报
     ERR_CRC，还在池里留一份 UNCOMMITTED 残片。这个缺陷在 QEMU 能装镜像之后
     第一次真装就暴露了（2026-11-18，`loader_crc_recheck_pool_e2e` 报 -5）。
  3. `arkrtos_sec.c` 里「一个都没复核成」必须是 SKIP 而不是 PASS。
     全 unknown 却报 PASS，正是本项要消灭的"看着权威的错答案"。
  4. 开关关着时那一行 SKIP 文案必须与改动前**逐字相同**。
     它决定默认档的二进制/行为是否真的零变化；后来人"顺手改个文案"就会破坏
     这条口径而没人发现。

不覆盖（写清楚，免得被读成"已验证"）：QEMU/板上的真实安装与复位后复核。
那是 `utest run loader_crc_recheck_pool_e2e` 的读数（见
docs/安全认证准备.md 的 SEC-G17 一节），默认档镜像与真 Flash 的电离位翻转
都不在本门禁的能力范围内。

用法
----
    python tools/sec_crc_gate.py
    python tools/sec_crc_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 有 pin 变红或有文件编不过。
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from seccheck_syntax_gate import DEFS, INCLUDES, find_armclang

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DEFCONFIG = "config/arkrtos_defconfig"
KCONFIG = "config/kconfig/Kconfig.features"
LOADER_H = "kernelsrc/include/arkrtos_loader.h"
LOADER_C = "kernelsrc/src/arkrtos_loader.c"
SEC_C = "kernelsrc/src/arkrtos_sec.c"
AUTOCONF = "kernelsrc/include/arkrtos_autoconf.h"

GUARD = "#if (ARKRTOS_USE_IMAGE_CRC_RECHECK == 1)"
# 默认档那一行 SKIP 文案（逐字）：开关关着时它必须原封不动。
LEGACY_SKIP = '"flash holds relocated bytes; header crc32 covers the nominal form"'


def read(rel):
    with open(os.path.join(ROOT, rel), "rb") as f:
        return f.read().decode("utf-8")


# ---------------------------------------------------------------------------
# 源码 pin：每个函数都只看"一份文本"，这样才能拿改坏的副本做自检
# ---------------------------------------------------------------------------

def pin_defconfig(txt):
    return ("ARKRTOS_USE_IMAGE_CRC_RECHECK=n" in txt)


def pin_kconfig(txt):
    for line in txt.splitlines():
        if line.strip() == "config ARKRTOS_USE_IMAGE_CRC_RECHECK":
            blk = txt.split(line, 1)[1].split("\nconfig ", 1)[0]
            return ("default n" in blk) and ("depends on ARKRTOS_USE_SECCHECK" in blk)
    return False


def pin_loader_h_guard(txt):
    if GUARD not in txt:
        return False
    tail = txt.split(GUARD, 1)[1]
    return "int32 arkrtos_loader_crc_recheck(" in tail


def pin_loader_c_guard(txt):
    if GUARD not in txt:
        return False
    tail = txt.split(GUARD, 1)[1]
    return "int32 arkrtos_loader_crc_recheck(" in tail


def pin_load_buffer_table_crc(txt):
    """装载路径里：表体必须进 crc_written，且要发生在负载循环之前。

    这一条原先取的是 `arkrtos_loader_load_buffer()` 的函数体；该函数拆成
    verify / program / commit 三段之后，表体折叠发生在 `loader_program_slot()`
    里，而「先折表、再搬负载」这个约束没变。所以判据改成「同一文件内，折叠
    那一行出现在负载循环之前」——顺序是实质，函数边界不是。"""
    crc = txt.find("crc_written = arkrtos_crc32(image + ARKRTOS_APP_RELOC_OFFSET")
    loop = txt.find("while(written < p_src->image_size)")
    if (crc < 0) or (loop < 0):
        return False
    return crc < loop


SEC_CRC_LINE = '"checked=%u bad=%u unknown=%u first=%u"'

def pin_sec_unknown_is_skip(txt):
    """checked==0 的两个分支都必须报 SKIP：空池是"没有对象"，全 unknown 是"一份
    都没算出来"，两种都不是通过。窗口取到 PASS/FAIL 那一行之前，所以只要有人把
    其中任一分支改成 PASS，窗口里就会冒出 ARKRTOS_SEC_PASS 而变红。"""
    i = txt.find("if(checked == 0u)")
    j = txt.find(SEC_CRC_LINE, i)
    k = txt.rfind('sec_add(p_rep, "images.crc_recheck"', i, j)
    if i < 0 or j < 0 or k < 0:
        return False
    window = txt[i:k]
    return (window.count("ARKRTOS_SEC_SKIP") >= 2) and ("ARKRTOS_SEC_PASS" not in window)


def pin_sec_legacy_text(txt):
    return LEGACY_SKIP in txt


def pin_autoconf(txt):
    for line in txt.splitlines():
        if "ARKRTOS_USE_IMAGE_CRC_RECHECK" in line and line.strip().startswith("#define"):
            return line.strip().endswith("0")
    return False


PINS = [
    ("defconfig 默认 n", DEFCONFIG, pin_defconfig),
    ("Kconfig default n + 依赖 SECCHECK", KCONFIG, pin_kconfig),
    ("loader.h 声明在开关内", LOADER_H, pin_loader_h_guard),
    ("loader.c 实现在开关内", LOADER_C, pin_loader_c_guard),
    ("装载路径把表体折进 crc_written", LOADER_C, pin_load_buffer_table_crc),
    ("sec.c 全 unknown 走 SKIP", SEC_C, pin_sec_unknown_is_skip),
    ("sec.c 默认档 SKIP 文案逐字未变", SEC_C, pin_sec_legacy_text),
    ("autoconf 默认 0", AUTOCONF, pin_autoconf),
]


def mutate(txt, mode):
    """把一份文本故意改坏：mode 决定改哪种形状（调用方按 pin 选）。"""
    if mode == "drop_guard":
        return txt.replace(GUARD, "#if 1", 1)
    if mode == "drop_table_crc":
        return txt.replace("crc_written = arkrtos_crc32(image + ARKRTOS_APP_RELOC_OFFSET",
                           "crc_written = crc_written + 0u * arkrtos_crc32(image + ARKRTOS_APP_RELOC_OFFSET", 1)
    if mode == "drop_unknown_skip":
        i = txt.find("if(checked == 0u)")
        if i < 0:
            return txt
        k = txt.find("ARKRTOS_SEC_SKIP", i)
        if k < 0:
            return txt
        return txt[:k] + "ARKRTOS_SEC_PASS" + txt[k + len("ARKRTOS_SEC_SKIP"):]
    if mode == "kconfig_default_y":
        i = txt.find("config ARKRTOS_USE_IMAGE_CRC_RECHECK")
        if i < 0:
            return txt
        head, tail = txt[:i], txt[i:]
        return head + tail.replace("default n", "default y", 1)
    if mode == "edit_legacy_text":
        return txt.replace(LEGACY_SKIP, '"flash holds relocated bytes"', 1)
    if mode == "enable_switch":
        return txt.replace("ARKRTOS_USE_IMAGE_CRC_RECHECK=n",
                           "ARKRTOS_USE_IMAGE_CRC_RECHECK=y", 1)
    if mode == "autoconf_one":
        return txt.replace("#define ARKRTOS_USE_IMAGE_CRC_RECHECK 0",
                           "#define ARKRTOS_USE_IMAGE_CRC_RECHECK 1", 1)
    return txt


# 每条 pin 对应的"改坏方式"：自检必须因此变红
MUTATORS = {
    "defconfig 默认 n": "enable_switch",
    "Kconfig default n + 依赖 SECCHECK": "kconfig_default_y",
    "loader.h 声明在开关内": "drop_guard",
    "loader.c 实现在开关内": "drop_guard",
    "装载路径把表体折进 crc_written": "drop_table_crc",
    "sec.c 全 unknown 走 SKIP": "drop_unknown_skip",
    "sec.c 默认档 SKIP 文案逐字未变": "edit_legacy_text",
    "autoconf 默认 0": "autoconf_one",
}


def compile_tu(cc, rel, extra_defs):
    base = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only"]
    base += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]
    cmd = base + ["-D" + d for d in (DEFS + extra_defs)] + ["-mcpu=cortex-m4",
                                                            os.path.join(ROOT, rel)]
    r = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    return (r.returncode == 0), out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--armclang", default=None)
    args = ap.parse_args()

    bad = []

    # --- 1) 源码 pin（每条都配变异自检） ---
    cache = {}
    for label, rel, fn in PINS:
        if rel not in cache:
            cache[rel] = read(rel)
        txt = cache[rel]
        if not fn(txt):
            bad.append("pin 变红: %s (%s)" % (label, rel))
            continue
        mut = MUTATORS.get(label)
        if mut:
            broken = mutate(txt, mut)
            if broken == txt:
                bad.append("自检失效: %s 的改坏方式没改到任何字节" % label)
            elif fn(broken):
                bad.append("自检失效: %s 改坏后仍然通过（这条 pin 是空转的）" % label)

    # --- 2) 两档语法编译 ---
    cc = find_armclang(args.armclang)
    if not cc:
        print("SEC-CRC-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    tuples = [
        ("开关档", ["ARKRTOS_USE_IMAGE_CRC_RECHECK=1"], ["kernelsrc/src/arkrtos_loader.c",
                                                       "kernelsrc/src/arkrtos_sec.c"]),
        ("默认档", [], ["kernelsrc/src/arkrtos_loader.c",
                        "kernelsrc/src/arkrtos_sec.c"]),
    ]
    n_ok = 0
    for label, extra, files in tuples:
        for rel in files:
            good, out = compile_tu(cc, rel, extra)
            if good:
                n_ok += 1
            else:
                bad.append("编译失败（%s）: %s\n%s" % (label, rel, out[-500:]))

    if bad:
        for b in bad:
            print("SEC-CRC-GATE FAIL: %s" % b)
        return 1

    print("SEC-CRC-GATE OK (%d pin + %d TU 两档)" % (len(PINS), n_ok))
    return 0


if __name__ == "__main__":
    sys.exit(main())
