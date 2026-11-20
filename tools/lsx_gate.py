#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""LoongArch LSX 向量寄存器上下文（ARKRTOS_USE_LSX）的门禁。

为什么需要这一道
----------------
LSX 的 32 个向量寄存器（$vr0..$vr31，每个 128 位）在 FPU 档里**只存了低
64 位**（那部分是 $f0..$f31）。打开 EUEN.SXE 之后，用户态一用向量指令就
会在物理寄存器上留下活值；如果 trap 帧不保存高 64 位，另一个任务用同一组
寄存器，被打断的那个任务回来读到的就是**别人的数据**——这类错误不抛异常、
不改控制流，只让结果安静地错。所以这一类功能有三处典型假绿：

  1. 开关没接线：EUEN.SXE 没打开，vld/vst 直接落 Ecode=13 INE，看上去
     像"没实现"而不是"没保存"；
  2. 存了但没恢复：帧里有数据、恢复序列忘了写回，读数与"没存"一模一样；
  3. 夹具自己就是错的：vld/vst 的基址寻址写错（si12 偏移用了没装载的
     寄存器），或者两个任务用同一份图案——"都相同"什么也证明不了。
     本门禁因此要求**同一条路径上的前置对照**（装完立刻存、不经切换，
     必须 bad=0）与**两份不同图案**（任一任务被对方覆盖就必然不符）。

判据（全过才 0）
----------------
  A. `switch-default`：`arkrtos_config.h` 里 `ARKRTOS_USE_LSX` 默认 0，
     且有「LSX=1 需要 FPU=1」的 `#error`（没有 FPR 区就没有地方挂向量区）。
  B. `frame-size`：`arkrtos_context.S` 在开档时 `LA_FRAME_BYTES 1040`、
     `arkrtos_port.c` 对应 `LA_FR_SIZE (1040u)` / `LA_FR_WORDS (130u)`；
     两个文件的向量区偏移都必须是 `528 + 16*i`（i=0..31）各 32 条，
     即「保存」与「恢复」覆盖同一组槽位（槽位对不上就是第 2 类假绿）。
  C. `euen`：`start.S` 在开关下置 EUEN 的 bit1（SXE），且**不置 bit2**
     （ASXE）——帧里没有 $xr，开 ASXE 会变成静默算错。
  D. `default-build`：默认档全量重建 + QEMU 实跑，日志里**一条 `[lsx]`
     都不许有**（关档夹具必须没编进去），同时 `[fcsr]` 的 PASS 行仍在。
  E. `lsx-build`：开档全量重建 + QEMU 实跑，读数为
     两条 `[lsx] pre bad=0`（A/B 各一，前置对照）、
     一条 `[lsx] A bad=0 rounds=50 | B bad=0 rounds=50 -> PASS`，
     且 `[fcsr]` 的 PASS 行仍在（新功能不许搞坏邻居）。
  F. 解析器自检：把好日志按四种坏法改动（删 PASS 行、rounds 50->49、
     bad=0->bad=1、删掉一条 pre 行），每种都必须被判红。

不覆盖：LASX（$xr0..$xr31）。帧里没有 256 位向量区，EUEN.ASXE 故意不开，
用到 LASX 的代码落 Ecode=13 INE（显式故障，不是静默算错）。真机也没跑过：
本机只有 QEMU，`vld/vst` 的 16 字节对齐与 dbar/ibar 语义只有真机能证。

用法
----
    py -3 tools/lsx_gate.py
    py -3 tools/lsx_gate.py --no-build      # 用构建目录里现成的镜像跑 QEMU
    py -3 tools/lsx_gate.py --seconds 30

退出码 0 = 全过；1 = 任一判据不成立 / 缺工具链与 QEMU（缺工具是 FAIL）。
"""

import argparse
import os
import re
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD_DIR = os.path.join(ROOT, "build", "qemu_loongarch64")
ELF_NAME = "ark_qemu_la64.elf"
MK = "example/qemu_loongarch64/Makefile"
QEMU_CANDIDATES = (
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-loongarch64.exe",
)
ROUNDS = 50
WINDOW = "528 + 16*i"

FAILS = []
PASSES = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-24s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-24s %s" % (name, detail))


def read_text(path):
    with open(path, "rb") as f:
        raw = f.read()
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def run(cmd, cwd=ROOT, env=None):
    e = dict(os.environ)
    e.pop("PYTHONHOME", None)
    e.pop("PYTHONPATH", None)
    if env:
        e.update(env)
    r = subprocess.run(cmd, cwd=cwd, env=e,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return r.returncode, read_text_bytes(r.stdout or b"")


def read_text_bytes(raw):
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


# ---------------------------------------------------------------- A 开关默认

def check_switch_default():
    path = "kernelsrc/include/arkrtos_config.h"
    text = read_text(path)
    m = re.search(r"#ifndef ARKRTOS_USE_LSX\s*\n#define ARKRTOS_USE_LSX\s+(\d+)", text)
    if not m:
        bad("switch-default", "config 头里找不到 ARKRTOS_USE_LSX 的默认值定义")
        return
    default = m.group(1)
    guard = ("ARKRTOS_USE_LSX == 1" in text) and ("ARKRTOS_USE_FPU == 0" in text) \
        and ("#error" in text)
    if default != "0":
        bad("switch-default", "默认值是 %s（应为 0，默认档必须零变化）" % default)
    elif not guard:
        bad("switch-default", "缺「LSX=1 需要 FPU=1」的 #error 依赖检查")
    else:
        ok("switch-default", "默认 0 + 依赖 FPU 的 #error 都在")


# ---------------------------------------------------------------- B 帧布局

def check_frame_layout():
    asm = read_text("kernelsrc/port/loongarch/la64/arkrtos_context.S")
    port = read_text("kernelsrc/port/loongarch/la64/arkrtos_port.c")

    if not re.search(r"#if \(ARKRTOS_USE_LSX == 1\)\s*\n#define LA_FRAME_BYTES\s+1040", asm):
        bad("frame-size", "context.S 里没有 LSX 档的 LA_FRAME_BYTES 1040")
        return
    if not re.search(r"#define LA_FR_SIZE\s+\(1040u\)", port):
        bad("frame-size", "port.c 里没有 LSX 档的 LA_FR_SIZE (1040u)")
        return
    if not re.search(r"#define LA_FR_WORDS\s+\(130u\)", port):
        bad("frame-size", "port.c 里没有 LSX 档的 LA_FR_WORDS (130u)")
        return

    miss = []
    for mn in ("vst", "vld"):
        for i in range(32):
            insn = r"%s\s+\$vr%d,\s*\\base,\s*%d" % (mn, i, 528 + 16 * i)
            if not re.search(insn, asm):
                miss.append("%s $vr%d@%d" % (mn, i, 528 + 16 * i))
    if miss:
        bad("frame-size", "向量槽位缺 %d 条（示例 %s）" % (len(miss), miss[0]))
    else:
        ok("frame-size", "帧长 1040（528 + 512），vst/vld 各 32 条覆盖 %s" % WINDOW)


# ---------------------------------------------------------------- C EUEN

def check_euen():
    text = read_text("example/qemu_loongarch64/start.S")
    m = re.search(r"#if \(ARKRTOS_USE_LSX == 1\)(.*?)#endif", text, re.S)
    if not m:
        bad("euen", "start.S 里没有 ARKRTOS_USE_LSX 的 EUEN 段")
        return
    blk = m.group(1)
    sxe = re.search(r"ori\s+\$t1,\s*\$zero,\s*0x2", blk) is not None
    asxe = re.search(r"ori\s+\$t1,\s*\$zero,\s*0x4", blk) is not None
    if not sxe:
        bad("euen", "开通段没有置 EUEN.SXE（bit1，ori 0x2）")
    elif asxe:
        bad("euen", "开通段置了 ASXE（bit2）—— 帧里没有 $xr，会变成跨任务静默算错")
    else:
        ok("euen", "开机置 SXE，未置 ASXE（LASX 保持 INE 显式故障）")


# ---------------------------------------------------------------- D/E 构建与实跑

def find_qemu(given):
    if given:
        return given if os.path.isfile(given) else None
    for c in QEMU_CANDIDATES:
        if os.path.isfile(c):
            return c
    return None


def build(flags):
    rc, out = run(["make", "-f", MK, "clean"])
    if rc != 0:
        return rc, out
    env = {"EXTRA_CFLAGS": flags} if flags else {}
    return run(["make", "-f", MK, "-j8"], env=env)


def run_qemu(qemu, seconds):
    """串口走管道而不是 -serial file:：file 后端块缓冲，进程被 kill 时最后
    一个不满 4 KB 的块会丢，读数会变成抽签。"""
    cmd = [qemu, "-machine", "virt", "-kernel", ELF_NAME,
           "-nographic", "-monitor", "none"]
    p = subprocess.Popen(cmd, cwd=BUILD_DIR, stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    chunks = []

    def drain():
        while True:
            b = p.stdout.read(1024)
            if not b:
                break
            chunks.append(b)

    th = threading.Thread(target=drain)
    th.daemon = True
    th.start()

    deadline = time.time() + seconds
    while time.time() < deadline:
        joined = b"".join(chunks)
        if joined.count(b"[lsx]") >= 1 and joined.count(b"[fcsr]") >= 1:
            break
        time.sleep(0.05)
    time.sleep(0.6)
    p.kill()
    p.wait()
    th.join(timeout=5)
    data = b"".join(chunks)
    if not data:
        return None
    return read_text_bytes(data)


def judge_default(text):
    lines = text.splitlines()
    lsx = [ln for ln in lines if ln.startswith("[lsx]")]
    fcsr = [ln for ln in lines if ln.startswith("[fcsr]")]
    problems = []
    if lsx:
        problems.append("关档日志里有 %d 条 [lsx]（夹具没被开关包住）" % len(lsx))
    if not any("-> PASS" in ln for ln in fcsr):
        problems.append("关档 [fcsr] 没有 PASS 行（默认档本身就不工作了）")
    return problems


def judge_lsx(text):
    lines = text.splitlines()

    def count(pred):
        return sum(1 for ln in lines if pred(ln))

    pre = count(lambda s: re.match(r"^\[lsx\] pre bad=0 ", s) is not None)
    verdict = count(lambda s: re.match(
        r"^\[lsx\] A bad=0 rounds=%d \| B bad=0 rounds=%d -> PASS"
        % (ROUNDS, ROUNDS), s) is not None)
    any_lsx = count(lambda s: s.startswith("[lsx]"))
    fcsr = count(lambda s: s.startswith("[fcsr]") and "-> PASS" in s)

    problems = []
    if pre != 2:
        problems.append("[lsx] pre 行 = %d（要 2 条：A/B 各一条 bad=0 的前置对照）" % pre)
    if verdict != 1:
        problems.append("[lsx] 跨切换 PASS 行 = %d（要 1 条 bad=0 rounds=%d 两边）"
                        % (verdict, ROUNDS))
    if any_lsx != pre + 1:
        problems.append("[lsx] 行数 = %d，除 pre 与 A/B 判决外还有多余行" % any_lsx)
    if fcsr < 1:
        problems.append("[fcsr] 的 PASS 行没了（新功能把邻居搞坏了）")
    return problems


def check_builds(qemu, seconds, no_build):
    # D. 默认档（--no-build 时跳过：构建目录里很可能正是开档镜像）
    if not no_build:
        rc, out = build("")
        if rc != 0:
            bad("default-build", "默认档重建失败：\n%s" % out[-400:])
            return
        if not os.path.isfile(os.path.join(BUILD_DIR, ELF_NAME)):
            bad("default-build", "构建目录里没有 %s" % ELF_NAME)
            return
        text = run_qemu(qemu, seconds)
        if text is None:
            bad("default-build", "QEMU 没写出串口（%d 秒内）" % seconds)
            return
        problems = judge_default(text)
        if problems:
            bad("default-build", problems[0])
        else:
            ok("default-build", "默认档 QEMU 实跑：0 条 [lsx]、[fcsr] PASS 仍在")

    # E. 开档
    if not no_build:
        rc, out = build("-DARKRTOS_USE_LSX=1")
        if rc != 0:
            bad("lsx-build", "开档重建失败：\n%s" % out[-400:])
            return
    text = run_qemu(qemu, seconds)
    if text is None:
        bad("lsx-build", "QEMU 没写出串口（%d 秒内）" % seconds)
        return
    problems = judge_lsx(text)
    if problems:
        bad("lsx-build", problems[0])
        return
    for ln in text.splitlines():
        if ln.startswith("[lsx]"):
            print("      | %s" % ln)
    ok("lsx-build", "开档 QEMU 实跑：前置 bad=0 两条 + 跨切换 PASS 一条")


# ---------------------------------------------------------------- F 解析器自检

def check_parser_selfcheck():
    good = "\n".join([
        "[lsx] pre bad=0 out0=1 pat0=1 out16=2 pat16=2",
        "[lsx] pre bad=0 out0=3 pat0=3 out16=4 pat16=4",
        "[fcsr] A rm=1 bad=0 rounds=50 | B rm=2 bad=0 rounds=50 -> PASS (FCSR per task)",
        "[lsx] A bad=0 rounds=50 | B bad=0 rounds=50 -> PASS (vr0..vr31 per task)",
        "",
    ])
    mutations = [
        ("删掉跨切换 PASS 行", good.replace(
            "[lsx] A bad=0 rounds=50 | B bad=0 rounds=50 -> PASS (vr0..vr31 per task)\n", "")),
        ("rounds 50 -> 49", good.replace("rounds=50 | B bad=0 rounds=50", "rounds=49 | B bad=0 rounds=49")),
        ("bad=0 -> bad=1", good.replace("[lsx] A bad=0", "[lsx] A bad=1")),
        ("删掉一条 pre 行", good.replace(
            "[lsx] pre bad=0 out0=3 pat0=3 out16=4 pat16=4\n", "")),
    ]
    miss = [name for name, text in mutations if not judge_lsx(text)]
    if judge_lsx(good):
        bad("parser-selfcheck", "好日志被判红了：%s" % judge_lsx(good)[0])
    elif miss:
        bad("parser-selfcheck", "这几种坏法没被判红：%s" % ", ".join(miss))
    else:
        ok("parser-selfcheck", "好日志判绿；4 种坏法（缺 PASS / rounds / bad / 缺 pre）全判红")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--seconds", type=int, default=25)
    a = ap.parse_args()

    print("== LSX 上下文门禁（ARKRTOS_USE_LSX）==")
    check_switch_default()
    check_frame_layout()
    check_euen()
    check_parser_selfcheck()

    qemu = find_qemu(a.qemu)
    if qemu is None:
        bad("toolchain", "找不到 qemu-system-loongarch64（用 --qemu 指定）")
    else:
        check_builds(qemu, a.seconds, a.no_build)

    print("-" * 60)
    print("%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if FAILS:
        print("LSX-GATE FAIL: " + ", ".join(FAILS))
        return 1
    print("LSX-GATE OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
