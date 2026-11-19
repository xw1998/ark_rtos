#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""MPS2-AN386（Cortex-M4）真上板门禁：编出来、跑起来、读数对得上。

为什么需要这一道
----------------
`board/qemu_mps2_an386/` 与 `example/qemu_mps2_an386/` 是本仓库第三个 ISA
家族在模拟器上的落点。它平时不参与任何 Keil 工程：F427/F401 的 uvprojx 里
没有这些文件，ci_gate 的 `--build` 也编不到它。于是这里的任何一处改动
（向量表被挤到 .text 中间、栈顶常量写错、UART 波特率算错、桩返回 0 而不是
-1）都能在「别处全绿」的情况下静默退化，直到有人真的去烧板。

所以这道门禁不看「代码长得像不像」，只看两件可证伪的事：
  1. 镜像的头 8 个字节是 Cortex-M 复位向量（栈顶 + 复位 PC 且 Thumb 位为 1），
     也就是向量表真的物理落在地址 0；
  2. QEMU 里跑出来的串口读数自洽——节拍增量、延时增量、SVC 时间与节拍换算
     四条互相独立的口径能对上，且零失配。

第三段是自检：把合成出来的坏日志喂给同一个解析函数，每一种坏法都必须被判
FAIL。没有这一段，解析器写松了（比如把 `>=5` 写成 `>=0`）也照样全绿。

边界（不许读过头）
------------------
- 本档 FPU / MPU / PRIV 全关，理由在 `board/qemu_mps2_an386/arkrtos_board_config.h`
  的文件头里：soft-float 下开 FPU 只多一份没人用的上下文，MPU 保护机制需要
  单独的越权访问判据，不在这一档的题面里。**这一档没有验证 MPU 保护**。
- 验的是「节拍 + 任务切换 + SVC 读数 + 信号量同步」四条链路，不是全功能档。
- QEMU 不是真硅：时序、UART FIFO 行为、复位后的外设默认值都只代表 QEMU 的
  模型。这一档的结论只能写成「在 QEMU 的 mps2-an386 模型上跑通」。

用法
----
    py -3 tools/qemu_m4_gate.py
    py -3 tools/qemu_m4_gate.py --qemu "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-arm.exe"
    py -3 tools/qemu_m4_gate.py --no-build        # 只跑已有的 ELF

退出码 0 = 全过；1 = 编译失败 / 镜像头不对 / 读数不自洽 / 自检没抓住坏日志。
缺工具链或 QEMU 一律 FAIL，不跳过——跳过会把「没验」写成「通过」。
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

MAKEFILE = "example/qemu_mps2_an386/Makefile"
BUILD_DIR = os.path.join(ROOT, "build", "qemu_mps2_an386")
ELF = os.path.join(BUILD_DIR, "ark_qemu_m4.elf")

DEFAULT_ARMGCC = "D:/env/tools/gnu_gcc/arm_gcc/bin"
QEMU_CANDIDATES = (
    "D:/env/tools/qemu/qemu9/mingw64/bin/qemu-system-arm.exe",
    "C:/Program Files/qemu/qemu-system-arm.exe",
)

# 期望的向量表头：栈顶 = RAM 尾（0x20000000 + 4 MB），复位 PC 落在镜像区且
# Thumb 位为 1。这两个数是板级配置的推论，不是从 ELF 里读回来再自证。
EXPECT_SP = 0x20400000
IMAGE_TOP = 0x00400000

FAILS = []
PASSES = []


def ok(name, detail):
    PASSES.append(name)
    print("PASS  %-22s %s" % (name, detail))


def bad(name, detail):
    FAILS.append(name)
    print("FAIL  %-22s %s" % (name, detail))


def find_qemu(explicit):
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.isfile(c):
            return c
    return None


def find_tool(armgcc_dir, name):
    p = os.path.join(armgcc_dir, name + ".exe")
    if os.path.isfile(p):
        return p
    return None


def read_text(path):
    with open(path, "rb") as f:
        return f.read().decode("utf-8", "replace")


# ---------------------------------------------------------------- 静态断言

def check_vector_table_source():
    """向量表必须由 KEEP 钉在 .text 的最前，否则 --gc-sections 会把它删掉。"""
    ld = read_text(os.path.join(ROOT, "example/qemu_mps2_an386/link.ld.in"))
    # 输出段的收尾可以是 "}" 单独一行，也可以带 "> REGION"；两种都算。
    m = re.search(r"^[ \t]*\.text\b[^{]*\{(.*?)^[ \t]*\}[^\n]*$",
                  ld, re.S | re.M)
    if not m:
        bad("ld-vector-table", "link.ld.in 里找不到 .text 输出段")
        return
    body = m.group(1)
    first = None
    for ln in body.splitlines():
        s = ln.strip()
        if not s or s.startswith("/*"):
            continue
        # 行尾注释是给人看的，不参与相等比较
        s = s.split("/*")[0].strip()
        first = s
        break
    if first != "KEEP(*(.text.start))":
        bad("ld-vector-table",
            ".text 的第一条不是 KEEP(*(.text.start))，实际是 %r" % (first,))
        return
    if "*(.text .text.*" not in body and ".text.*" not in body:
        bad("ld-vector-table", ".text 段里没有普通代码收集规则")
        return
    ok("ld-vector-table", ".text 首条 KEEP(.text.start)，普通代码在其后")


def check_startup_source():
    """start.S 要有 .text.start 段名与向量表第 0/1 项。"""
    src = read_text(os.path.join(ROOT, "example/qemu_mps2_an386/start.S"))
    problems = []
    if not re.search(r"\.section\s+\.text\.start", src):
        problems.append("没有 .section .text.start")
    if "_vectors:" not in src:
        problems.append("没有 _vectors 标签")
    if "__bss_start" not in src or "__bss_end" not in src:
        problems.append("没有 .bss 清零")
    if problems:
        bad("startup-source", "; ".join(problems))
        return
    ok("startup-source", ".text.start + _vectors + .bss 清零均在")


def check_board_switches():
    """本档的开关必须是「明确关」，而不是漏写后取到架构默认值。

    M4 的 ARKRTOS_ARCH_HAS_FPU/MPU 都是 1，所以 FPU/MPU 若没被显式关掉，
    就会在不该开的地方打开——这是「默认值恰好等于想法」的假绿形态。
    """
    cfg = read_text(os.path.join(ROOT, "board/qemu_mps2_an386/arkrtos_board_config.h"))
    need = {
        "ARKRTOS_USE_FPU": "0",
        "ARKRTOS_USE_MPU": "0",
        "ARKRTOS_USE_PRIV": "0",
        "ARKRTOS_USE_FS": "0",
        "ARKRTOS_USE_SHELL": "0",
        "ARKRTOS_USE_SMP": "0",
    }
    problems = []
    for k, v in need.items():
        m = re.search(r"#define\s+%s\s+(\S+)" % k, cfg)
        if not m:
            problems.append("%s 未显式定义" % k)
        elif m.group(1) != v:
            problems.append("%s=%s（期望 %s）" % (k, m.group(1), v))
    m = re.search(r"#define\s+ARKRTOS_ARCH_CORE\s+(\S+)", cfg)
    if not m or m.group(1) != "0x0102u":
        problems.append("ARKRTOS_ARCH_CORE 不是 0x0102u（Cortex-M4）")
    if problems:
        bad("board-switches", "; ".join(problems))
        return
    ok("board-switches", "FPU/MPU/PRIV/FS/SHELL/SMP 全显式 0，ARCH_CORE=M4")


def check_stub_guards():
    """桩文件必须自带「开关一开就编不过」的护栏，且桩不许返回成功。"""
    src = read_text(os.path.join(ROOT, "example/qemu_mps2_an386/kernel_stubs.c"))
    problems = []
    if "#error" not in src:
        problems.append("没有编译期护栏")
    if "return -1;" not in src:
        problems.append("桩里没有 return -1")
    for fn in ("arkrtos_port_flash_erase", "arkrtos_port_flash_write",
               "arkrtos_shell_ext_register", "arkrtos_shell_print_n"):
        if fn not in src:
            problems.append("缺 %s 桩" % fn)
    if problems:
        bad("stub-guards", "; ".join(problems))
        return
    ok("stub-guards", "护栏 + 失败返回 + 4 条必备桩")


# ---------------------------------------------------------------- 构建与镜像

def build(armgcc_dir):
    env = dict(os.environ)
    env["PATH"] = armgcc_dir + os.pathsep + env.get("PATH", "")
    log = os.path.join(tempfile.gettempdir(), "qemu_m4_gate_build.log")
    with open(log, "wb") as f:
        r = subprocess.call(
            ["make", "-f", MAKEFILE, "TOOLCHAIN=" + armgcc_dir],
            cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, env=env, shell=False)
    tail = read_text(log)[-600:]
    if r != 0:
        bad("build", "make 退出码 %d；日志尾部：\n%s" % (r, tail))
        return False
    if not os.path.isfile(ELF):
        bad("build", "make 成功但没有 %s" % ELF)
        return False
    ok("build", "make 通过，产物 %d KB" % (os.path.getsize(ELF) // 1024))
    return True


def check_image_head(objcopy):
    """镜像头 8 字节 = 栈顶 + 复位 PC，且复位 PC 是 Thumb 地址。

    这是 M-profile 的启动契约：硬件不读 ENTRY()，只从 0x0/0x4 取两个字。
    所以「向量表在 .text 中间」这类错误只有在这一步才暴露——链接器不会报。
    """
    binp = os.path.join(tempfile.gettempdir(), "qemu_m4_gate.bin")
    r = subprocess.call([objcopy, "-O", "binary", ELF, binp])
    if r != 0 or not os.path.isfile(binp):
        bad("image-head", "objcopy 失败")
        return
    with open(binp, "rb") as f:
        head = f.read(8)
    if len(head) < 8:
        bad("image-head", "镜像不足 8 字节")
        return
    sp = int.from_bytes(head[0:4], "little")
    pc = int.from_bytes(head[4:8], "little")
    problems = []
    if sp != EXPECT_SP:
        problems.append("初始 SP=0x%08x（期望 0x%08x）" % (sp, EXPECT_SP))
    if (pc & 1) != 1:
        problems.append("复位 PC=0x%08x 的 Thumb 位是 0" % pc)
    if not (0 < (pc & ~1) < IMAGE_TOP):
        problems.append("复位 PC=0x%08x 不在镜像区" % pc)
    if problems:
        bad("image-head", "; ".join(problems))
        return
    ok("image-head", "SP=0x%08x Reset=0x%08x（Thumb）" % (sp, pc))


# ---------------------------------------------------------------- 运行与判读

ROUND_RE = re.compile(
    r"^\[A\] round=(\d+) tick=(\d+) d_tick=(\d+) ms=(\d+) d_ms=(\d+) "
    r"ms_svc=(\d+) (svc_ok|SVC_MISMATCH)$")
WAKE_RE = re.compile(r"^\[B\] woke, round=(\d+) tick=(\d+)$")
BOOT_RE = re.compile(r"^\[ark\] mps2-an386 cortex-m4 bring-up$")
PARAM_RE = re.compile(
    r"^\[ark\] task_a=(\d+) task_b=(\d+) sem=(\d+) sysclk=(\d+) tick_us=(\d+)$")

TICK_US = 500          # ARKRTOS_TICK_PERIOD_US 的板级取值
TICKS_PER_500MS = 1000  # 500 ms / 500 us


def judge(text):
    """把一份串口日志判成 (passed, [(name, detail), ...])。

    抽成纯函数是为了让自检能拿合成日志喂它；判据写在函数里，不自证。
    """
    lines = [ln.strip() for ln in text.replace("\r\n", "\n").split("\n")]
    lines = [ln for ln in lines if ln]
    res = []

    boots = [i for i, ln in enumerate(lines) if BOOT_RE.match(ln)]
    if len(boots) != 1:
        return False, [("boot-banner", "banner 出现 %d 次（期望 1 次）" % len(boots))]
    res.append(("boot-banner", "banner 唯一"))

    params = None
    if boots[0] + 1 < len(lines):
        params = PARAM_RE.match(lines[boots[0] + 1])
    if not params:
        return False, res + [("boot-params", "banner 之后不是参数行")]
    task_a, task_b, sem, sysclk, tick_us = [int(x) for x in params.groups()]
    if (task_a, task_b) != (1, 2):
        return False, res + [("boot-params", "任务号 %d/%d（期望 1/2）"
                              % (task_a, task_b))]
    if sem != 0x01300000:
        return False, res + [("boot-params",
                              "sem 句柄 0x%08x（期望 0x01300000 = 槽 0 + 类型位）"
                              % sem)]
    if sysclk != 25000000 or tick_us != TICK_US:
        return False, res + [("boot-params", "sysclk=%d tick_us=%d 与板级不符"
                              % (sysclk, tick_us))]
    res.append(("boot-params", "task 1/2，sem 句柄、25 MHz、500 us 均对"))

    rounds = []
    wakes = []
    mismatch = 0
    noise = []
    for ln in lines:
        m = ROUND_RE.match(ln)
        if m:
            r = [int(x) for x in m.groups()[:6]]
            rounds.append(r + [m.group(7)])
            if m.group(7) != "svc_ok":
                mismatch += 1
        elif WAKE_RE.match(ln):
            wakes.append(int(WAKE_RE.match(ln).group(1)))
        elif ln.startswith("[") and not BOOT_RE.match(ln) and not PARAM_RE.match(ln):
            noise.append(ln)

    if len(rounds) < 6:
        return False, res + [("rounds", "只跑出 %d 轮（需要 >= 6）" % len(rounds))]
    if mismatch != 0:
        return False, res + [("svc", "%d 轮 SVC 与节拍换算失配" % mismatch)]
    res.append(("svc", "%d 轮全 svc_ok，零失配" % len(rounds)))

    # 轮号必须从 0 连续递增（重入 / 乱序都不许有）
    if [r[0] for r in rounds] != list(range(len(rounds))):
        return False, res + [("rounds", "轮号不连续：%r"
                              % [r[0] for r in rounds])]
    res.append(("rounds", "%d 轮连续递增" % len(rounds)))

    # 节拍与延时的互换：稳态 d_tick≈1000，d_ms≈500。前两轮含启动抖动，跳过。
    steady = rounds[2:]
    bad_ticks = [r for r in steady if not (990 <= r[2] <= 1010)]
    if bad_ticks:
        return False, res + [("tick-delta", "d_tick 越界：%r"
                              % [(r[0], r[2]) for r in bad_ticks])]
    bad_ms = [r for r in steady if not (490 <= r[4] <= 515)]
    if bad_ms:
        return False, res + [("ms-delta", "d_ms 越界：%r"
                              % [(r[0], r[4]) for r in bad_ms])]
    # ms 必须等于 tick × tick_us / 1000（同一条换算被两处独立算出）
    bad_scale = [r for r in rounds if r[3] != (r[1] * TICK_US) // 1000]
    if bad_scale:
        return False, res + [("ms-scale", "ms != tick*500/1000：%r"
                              % [(r[0], r[1], r[3]) for r in bad_scale])]
    res.append(("tick-delta", "稳态 d_tick=%d..%d、d_ms=%d..%d，ms 与 tick 换算一致"
                % (min(r[2] for r in steady), max(r[2] for r in steady),
                   min(r[4] for r in steady), max(r[4] for r in steady))))

    # B 必须被 A 的 post 唤醒，且每次唤醒都能对上一个 A 的轮次
    if len(wakes) < len(rounds) - 2:
        return False, res + [("sem-wake", "A 跑了 %d 轮，B 只醒了 %d 次"
                              % (len(rounds), len(wakes)))]
    if wakes != sorted(wakes) or len(set(wakes)) != len(wakes):
        return False, res + [("sem-wake", "B 的唤醒轮次乱序或重复：%r" % wakes)]
    res.append(("sem-wake", "B 唤醒 %d 次，轮次单调且无重复" % len(wakes)))

    if noise:
        return False, res + [("noise", "有 %d 行不认识：%r"
                              % (len(noise), noise[:3]))]
    return True, res


def run_qemu(qemu, seconds):
    outp = os.path.join(tempfile.gettempdir(), "qemu_m4_gate_serial.txt")
    if os.path.exists(outp):
        os.remove(outp)
    # -kernel 只吃相对名：本机仓库路径含非 ASCII 字符，QEMU 在该参数上会用
    # 窄字符 API 打开，绝对路径直接失败（同一坑在 RV32/LA64 的 run 目标里
    # 已经写进文档）。所以 cwd 切到 BUILD_DIR，只传文件名。
    cmd = [qemu, "-M", "mps2-an386", "-cpu", "cortex-m4", "-nographic",
           "-kernel", os.path.basename(ELF),
           "-serial", "file:" + outp.replace("\\", "/")]
    log = open(os.path.join(tempfile.gettempdir(), "qemu_m4_gate_qemu.log"), "wb")
    p = subprocess.Popen(cmd, cwd=BUILD_DIR, stdout=log, stderr=subprocess.STDOUT)
    try:
        p.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
    log.close()
    if not os.path.isfile(outp):
        return None
    return read_text(outp)


def self_check():
    """把合成日志喂给 judge，四种坏法都必须被判 FAIL。

    这道自检存在的理由：解析器写松了（阈值写成 >=0、正则不带行首锚点）也
    会全绿，而绿是没有信息量的。坏样本必须来自「真实可能发生的错」：
    极端值、字节错位、句柄错、以及少一轮。
    """
    good = read_text(os.path.join(HERE, "qemu_m4_gate_sample.txt"))
    passed, _ = judge(good)
    if not passed:
        return False, "基准样本自身没通过"

    # 换算错那一档不能改 d_ms：d_ms 是相邻两轮的差，判据里只查区间；真正
    # 被判的是 ms 与 tick 的同步换算，所以要把某轮的 ms 字段挪开。
    scale_bad = re.sub(r"(\[A\] round=2 tick=)(\d+)( d_tick=\d+ ms=)(\d+)",
                       lambda m: m.group(1) + m.group(2) + m.group(3)
                       + str(int(m.group(4)) + 7), good, count=1)
    cases = [
        ("d_tick 变 2 倍", good.replace("d_tick=1000", "d_tick=2000", 3)),
        ("svc 失配", good.replace("svc_ok", "SVC_MISMATCH", 1)),
        ("sem 句柄错", good.replace("sem=19922944", "sem=0", 1)),
        ("轮号断档", good.replace("[A] round=3 ", "[A] round=5 ", 1)),
        ("ms 与 tick 换算错", scale_bad),
        ("B 一次都不醒", re.sub(r"^\[B\] woke.*$", "", good, flags=re.M)),
        ("混进不认识的行", good + "[A] round=99 tick=x\n"),
    ]
    for name, text in cases:
        if text == good:
            return False, "自检样本替换没生效（%s）" % name
        pass2, _ = judge(text)
        if pass2:
            return False, "坏日志没被判出来：%s" % name
    return True, "%d 种坏法全部判 FAIL，基准样本判 PASS" % len(cases)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--armgcc", default=DEFAULT_ARMGCC)
    ap.add_argument("--no-build", action="store_true")
    ap.add_argument("--seconds", type=int, default=20)
    args = ap.parse_args(argv)

    print("=== MPS2-AN386 (Cortex-M4) gate ===")
    check_vector_table_source()
    check_startup_source()
    check_board_switches()
    check_stub_guards()

    qemu = find_qemu(args.qemu)
    objcopy = find_tool(args.armgcc, "arm-none-eabi-objcopy")
    if not objcopy:
        bad("toolchain", "找不到 arm-none-eabi-objcopy（%s）" % args.armgcc)
    if not qemu:
        bad("qemu", "找不到 qemu-system-arm.exe")
    if not objcopy or not qemu:
        print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
        return 1

    if args.no_build:
        if not os.path.isfile(ELF):
            bad("build", "--no-build 但没有 %s" % ELF)
            print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
            return 1
        ok("build", "--no-build，复用已有 ELF")
    elif not build(args.armgcc):
        print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
        return 1

    check_image_head(objcopy)

    text = run_qemu(qemu, args.seconds)
    if text is None:
        bad("qemu-run", "QEMU 没有产出串口文件")
    else:
        passed, detail = judge(text)
        for name, msg in detail:
            (ok if passed else bad)(name, msg)
        if not passed and text.strip():
            print("---- 串口日志 ----")
            print(text[:2000])
            print("------------------")

    sc_ok, sc_msg = self_check()
    (ok if sc_ok else bad)("parser-self-check", sc_msg)

    print("\n%d passed, %d failed" % (len(PASSES), len(FAILS)))
    if FAILS:
        return 1
    print("QEMU-M4-GATE OK (linker pin + image head + %d s QEMU run + parser self-check)"
          % args.seconds)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
