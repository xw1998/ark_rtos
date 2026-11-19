#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""栈底守卫（ARKRTOS_USE_STACK_GUARD）的门禁：把两个方向都变成读数。

为什么需要这一道
----------------
U-13 的原状是「标志字写了，但从来没有人读过」——`arkrtos_task_register()`
往 `stack_bottom[0]` 写 `ARKRTOS_STACK_END_FLAG_VAL`，此后没有任何代码读它。
这种「写了不读」的代码不会报错、不会变慢，只会让人以为有保护。

守卫本身也有同型的四种假绿，所以逐条钉死：

  1. 开关「开着但还是空的」：`#if` 写反、宏拼错，编出来的内核里没有检查；
  2. 只在某一个 port 接上：LA64 的 trap 帧压在任务栈上，RV32 也是；
     少一个 port，少的就是那台板子的整条保护；
  3. 命中之后不摘任务：记录写了、控制台打了，任务继续跑坏栈 ——
     只要下一拍不把它从就绪集里拿掉，它就会在下一个随机指针上崩；
  4. 关档不干净：默认档里还留着字段/字符串/调用，体积与行为就不是
     「逐字节不变」。

判据
----
  A. 默认档：`arkrtos_autoconf.h` 里 `ARKRTOS_USE_STACK_GUARD` 必须是 0；
  B. LA64 ON（板级已置 1）：`arkrtos_task.c` 预处理结果里必须有
     `arkrtos_stack_guard_check` 的定义与调用；
  C. LA64 OFF（探针头把开关改回 0）：预处理结果里 `stack_guard` 一个字都不许出现
     —— 字段、原型、调用、字符串全在 `#if` 里；
  D. RV32 ON：`arkrtos_port.c` 必须编得过（32 位端口也接上了，且没有
     64 位假设）；
  E. 守卫 pin：保护字判据、命中后的摘除、注册时布防、Kconfig 声明；
  F. 板级：qemu_loongarch64 把开关置 1，示例里真有踩栈底的任务，并且
     控制台栈大小有一个板级钩子、本板确实放大过（守卫的第一个真阳性：
     shell 的 3 KB 栈在 `ld` 期间被用到 100%，见 §5.15.1）。

另一种假绿（②，启动/idle 上下文）：
  任务栈有 `stack_guard_armed` 旗标，但**启动阶段的 idle 栈不是任务槽位**，
  `current_task_id > 0` 的前置条件让它从来没被检查过。现在的做法是布防整段
  [bottom, top) 填图案、每拍逐字找最低的非图案字，并把读数打到 `stack ctx:`。

不覆盖：运行期读数。QEMU 实测见 docs/QEMU模拟器移植与验证.md 的栈守卫一节
（`[STK] task 4` + `fault` 里的 STACKOVF，以及 `stack ctx:` 的 used/free）。

用法
----
    python tools/stack_guard_gate.py
    python tools/stack_guard_gate.py --la64-gcc <path> --rv32-gcc <path>

退出码 0 = 全过；1 = 有一步不成立或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LA64_FALLBACKS = ["D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe"]
RV32_FALLBACKS = [
    "D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe"]

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
RV32_ARCH = ["-march=rv32imac", "-mabi=ilp32", "-mcmodel=medany", "-mno-relax"]

COMMON = [
    "-O0", "-Wall", "-ffreestanding", "-fno-common", "-nostdlib",
    "-nostartfiles", "-fno-builtin", "-fsyntax-only", "-DARKRTOS_SIGN_KEY_LOCAL=1",
]

SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

# task.c 的包含面比 port 宽：把 Makefile 里那串 -I 抄全，否则预处理会
# 死在第一个找不到的头文件上（那是仪器错，不是被测对象错）。
TASK_INCLUDES = [
    "-Ikernelsrc/include", "-Iconfig",
    "-Iexample/qemu_loongarch64",
    "-Ikernelsrc/components/mdk_trace",
    "-Ikernelsrc/shell", "-Ikernelsrc/shell/ark_shell",
    "-Ikernelsrc/components/ark_vfs/include",
    "-Ikernelsrc/components/ark_vfs/port",
    "-Ikernelsrc/components/ark_vfs/src",
    "-Ikernelsrc/fs", "-Ikernelsrc/fs/littlefs",
    "-Ikernelsrc/components/ark_softbus/include",
    "-Ikernelsrc/components/ark_softbus/src",
    "-Ikernelsrc/components/ark_softbus/port",
    "-Ibuild/qemu_loongarch64",
]

PROBE_OFF = """#undef ARKRTOS_USE_STACK_GUARD
#define ARKRTOS_USE_STACK_GUARD 0
"""

PROBE_ON = """#undef ARKRTOS_USE_STACK_GUARD
#define ARKRTOS_USE_STACK_GUARD 1
"""


def _pick(cands, given, what):
    if given:
        return given
    for c in cands:
        if os.path.isfile(c):
            return c
    print("FAIL: 找不到 %s（缺编译器是 FAIL，不是跳过）" % what)
    sys.exit(1)


def _text(raw):
    if not raw:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _run(cc, args):
    r = subprocess.run([cc] + args, cwd=ROOT,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, _text(r.stdout) + _text(r.stderr)


def _read(path):
    with open(path, "rb") as f:
        return _text(f.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    ap.add_argument("--rv32-gcc", default="")
    a = ap.parse_args()

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")

    tmp = tempfile.mkdtemp(prefix="ark_stkguard_")
    key_dir = os.path.join(tmp, "key")
    os.makedirs(key_dir)
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)
    probe_off = os.path.join(tmp, "guard_off.h")
    probe_on = os.path.join(tmp, "guard_on.h")
    with open(probe_off, "w", encoding="utf-8", newline="\n") as f:
        f.write(PROBE_OFF)
    with open(probe_on, "w", encoding="utf-8", newline="\n") as f:
        f.write(PROBE_ON)

    base = COMMON + [
        "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
        "-include", "arkrtos_board_config.h",
        "-I" + key_dir,
    ]

    la64_task = LA64_ARCH + base + [
        "-Iboard/qemu_loongarch64", "-Ikernelsrc/port/loongarch/la64",
    ] + TASK_INCLUDES + ["kernelsrc/src/arkrtos_task.c"]

    failures = []

    # --- A. 默认档必须是 0 ------------------------------------------------
    autoconf = _read(os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h"))
    if not re.search(r"^#define\s+ARKRTOS_USE_STACK_GUARD\s+0$", autoconf, re.M):
        failures.append("A 默认档：arkrtos_autoconf.h 里 ARKRTOS_USE_STACK_GUARD 不是 0")

    # --- B. LA64 ON：预处理结果里必须有检查函数 --------------------------
    flags_on = [x for x in la64_task if x != "-fsyntax-only"] + ["-E"]
    rc, out = _run(la64, flags_on)
    if rc != 0:
        failures.append("B LA64 ON 预处理失败：\n%s" % out[-600:])
    else:
        if "arkrtos_stack_guard_check" not in out:
            failures.append("B LA64 ON：预处理结果里没有 arkrtos_stack_guard_check"
                            "（开关开着却是空实现）")
        if "ARKRTOS_FAULT_STACKOVF" not in out:
            failures.append("B LA64 ON：命中后没有记 ARKRTOS_FAULT_STACKOVF")
        if "stack bottom flag overwritten" not in out:
            failures.append("B LA64 ON：没有那行控制台读数（任务号只在故障环里，"
                            "串口上看不到）")

    # --- C. LA64 OFF：一个字都不许留 -------------------------------------
    flags_off = [x for x in la64_task if x != "-fsyntax-only"] + [
        "-E", "-include", probe_off]
    rc, out = _run(la64, flags_off)
    if rc != 0:
        failures.append("C LA64 OFF 预处理失败：\n%s" % out[-600:])
    else:
        if "stack_guard" in out:
            failures.append("C LA64 OFF：预处理结果里仍有 stack_guard"
                            "（字段/原型/调用没被 #if 包住，关档体积会变）")
        if "s_ctx_guard" in out:
            failures.append("C LA64 OFF：预处理结果里仍有 s_ctx_guard"
                            "（② 的静态量与扫描没被 #if 包住，关档体积会变）")
        if "context stack" in out:
            failures.append("C LA64 OFF：预处理结果里仍有 context stack 那行读数")

    # --- D. RV32 ON：32 位端口也接上，且编得过 ---------------------------
    rv32_port = RV32_ARCH + base + [
        "-include", probe_on,
        "-Ikernelsrc/include", "-Iconfig",
        "-Iboard/qemu_riscv32", "-Ikernelsrc/port/riscv/rv32imac",
        "kernelsrc/port/riscv/rv32imac/arkrtos_port.c",
    ]
    rc, out = _run(rv32, rv32_port)
    if rc != 0:
        failures.append("D RV32 ON 编不过 arkrtos_port.c：\n%s" % out[-600:])

    # --- E. 守卫 pin -------------------------------------------------------
    task_c = _read(os.path.join(ROOT, "kernelsrc/src/arkrtos_task.c"))
    cfg_c = _read(os.path.join(ROOT, "kernelsrc/src/arkrtos_cfg.c"))
    task_h = _read(os.path.join(ROOT, "kernelsrc/include/arkrtos_task.h"))
    feats = _read(os.path.join(ROOT, "kernelsrc/include/arkrtos_features.h"))
    kcfg = _read(os.path.join(ROOT, "config/kconfig/Kconfig.features"))
    cfg_c2 = _read(os.path.join(ROOT, "kernelsrc/include/arkrtos_config.h"))
    diag_c = _read(os.path.join(ROOT, "kernelsrc/shell/arkrtos_shell_diag.c"))

    pins = [
        (task_c, "p_task->stack_bottom[0] == (uint32)ARKRTOS_STACK_END_FLAG_VAL",
         "判据必须是「栈底那一个字还是不是保护字」"),
        (task_c, "arkrtos_stack_guard_check(tid) != 0",
         "切换路径必须真的调用检查，且命中要走进摘除分支"),
        (task_c, "arkrtos_task_table[tid].status = ARKRTOS_TASK_INVALID",
         "命中后必须把任务摘掉，不能只打一行日志"),
        (cfg_c, "p_task->stack_guard_armed    = 1u;",
         "注册时必须布防，否则检查永远返回 0"),
        (task_h, "uint8  stack_guard_armed;",
         "TCB 必须有布防位（关档时整段不编入）"),
        (feats, "#define ARKRTOS_USE_STACK_GUARD         0",
         "features.h 的回退默认必须是 0"),
        # --- ② 启动/idle 上下文水位 ---------------------------------------
        (task_h, "arkrtos_stack_guard_watch_context",
         "② 必须有布防入口（否则 idle 栈永远不在检查面内）"),
        (task_h, "arkrtos_stack_guard_context_info",
         "② 必须有读数出口（否则 shell 里看不到 used/free）"),
        (task_c, "void arkrtos_stack_guard_watch_context(uint32 stack_bottom",
         "② 布防实现必须在 task.c（而不是某个 port 里）"),
        (task_c, "s_ctx_guard_low    = stack_top;",
         "② 水位初值必须是 top（布防后尚未写过任何字）"),
        (task_c, "(uint32)ARKRTOS_STACK_FILL_PATTERN)",
         "② 判据必须是「这个字还是不是填充图案」"),
        (task_c, "arkrtos_stack_guard_check_context();",
         "② 节拍体里必须真的调扫描，否则读数永远停在初值"),
        (cfg_c2, "ARKRTOS_STACK_GUARD_CTX_MARGIN",
         "② 告警门限必须有宏（默认 64 B，可板级覆盖）"),
        (diag_c, "stack ctx: idle/boot",
         "② shell 必须有一行汇总读数"),
        (diag_c, "stack ctx: not armed",
         "② 布防被拒时必须说出来（静默失败比没有守卫更坏）"),
    ]
    for src, need, why in pins:
        if need not in src:
            failures.append("E 守卫 pin：找不到 %r（%s）" % (need, why))

    if "config ARKRTOS_USE_STACK_GUARD" not in kcfg:
        failures.append("E Kconfig：没有 config ARKRTOS_USE_STACK_GUARD")

    for rel, tag in (("kernelsrc/port/loongarch/la64/arkrtos_port.c", "LA64"),
                     ("kernelsrc/port/riscv/rv32imac/arkrtos_port.c", "RV32")):
        src = _read(os.path.join(ROOT, rel))
        if "arkrtos_stack_guard_check_current();" not in src:
            failures.append("E port：%s 的 trap 入口没有调检查（%s）" % (rel, tag))

    # --- F. 板级 + 示例 ----------------------------------------------------
    board = _read(os.path.join(ROOT,
                  "board/qemu_loongarch64/arkrtos_board_config.h"))
    if not re.search(r"^#define\s+ARKRTOS_USE_STACK_GUARD\s+1", board, re.M):
        failures.append("F 板级：qemu_loongarch64 没有把 ARKRTOS_USE_STACK_GUARD 置 1")
    main_c = _read(os.path.join(ROOT, "example/qemu_loongarch64/main.c"))
    if "task_stack_guard_demo" not in main_c:
        failures.append("F 示例：main.c 里没有踩栈底的演示任务"
                        "（上板时就没有可观察的命中）")

    # ② 的布防必须在 enter_idle 之前（之后这块栈已经在用了，填图案等于自毁）
    for rel in ("example/qemu_loongarch64/main.c",
                "example/la32_generic/main.c",
                "example/qemu_riscv32/main.c"):
        src = _read(os.path.join(ROOT, rel))
        i_arm = src.find("arkrtos_stack_guard_watch_context")
        i_ent = src.find("arkrtos_port_enter_idle(sp")
        if i_arm < 0:
            failures.append("F 示例：%s 没有布防 idle 栈（② 空转）" % rel)
        elif i_ent < 0 or i_arm > i_ent:
            failures.append("F 示例：%s 的布防没在 enter_idle 之前" % rel)
        if not re.search(r"^#define\s+IDLE_STACK_WORDS\s+1024u", src, re.M):
            failures.append("F 示例：%s 的 IDLE_STACK_WORDS 不是 1024u"
                            "（实测深度 984 B，1 KB 只剩 28 B）" % rel)

    # 守卫的第一个真阳性：控制台任务栈 3 KB 在 `ld` 期间被用到 100%，
    # 溢出静默写坏装载器静态区。修法是给栈大小留一个板级钩子并在本板放大。
    shell_c = _read(os.path.join(ROOT, "kernelsrc/shell/arkrtos_shell.c"))
    if "#ifndef ARKRTOS_SHELL_STACK_SIZE" not in shell_c:
        failures.append("F 控制台栈：arkrtos_shell.c 没有 ARKRTOS_SHELL_STACK_SIZE "
                        "板级覆盖钩子")
    if not re.search(r"^#define\s+ARKRTOS_SHELL_STACK_SIZE\s+\(1024 \* 8\)",
                     board, re.M):
        failures.append("F 控制台栈：qemu_loongarch64 没有把 ARKRTOS_SHELL_STACK_SIZE "
                        "放大（默认 3 KB 实测在 ld 期间被用到 100%）")

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("STACK-GUARD-GATE OK: 默认档=0 / LA64 开档有检查(记录+任务号) / "
          "关档零符号 / RV32 端口开档编得过 / 守卫与摘除未拆 / "
          "Kconfig 与两块板级接线在 / 示例有踩栈底任务 / 控制台栈钩子与放大在 / "
          "② idle 上下文布防在读(三档都在 enter_idle 前、idle 栈 1024u)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
