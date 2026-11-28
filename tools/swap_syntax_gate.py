#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""任务热切换（ARKRTOS_USE_TASK_SWAP）的门禁：把开关按下去，真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_TASK_SWAP` 默认 0，而 F427/F401 两个 Keil 工程都按默认档编。
于是这几处 `#if (ARKRTOS_USE_TASK_SWAP == 1)` 里的代码平时一个编译器都不经过：

    kernelsrc/src/arkrtos_task.c    热切换本体 + SVC_Server 里的落地钩子
    kernelsrc/src/arkrtos_mini.c    `mini swap`（第二对 RAM 块 + 切换后归还旧块）
    kernelsrc/src/arkrtos_loader.c  `app swap`（第二个 Flash 槽 + 切换后改槽位状态）
    kernelsrc/shell/arkrtos_shell.c 两条命令与命令行解析
    kernelsrc/port/arm/cortex-m{3,4,7}/   PSP/MPU 写入口（只用到既有接口）

改了其中任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。
这就是"默认关 = 没人编译"的假绿形态，跟 rtstat / arkmake 那两道门禁同源。

这里用 armclang `-fsyntax-only` 以 `-DARKRTOS_USE_TASK_SWAP=1` 编上面这些文件，
三个移植层各用自己那一档 `-mcpu`。不覆盖：链接、栈帧改写之后在板上的实际行为、
`mini swap` / `app swap` 的读数——这些只有上板才知道。

跨架构那一向原本要求「非 ARM 打开就 #error」。RV32IMAC 与 LA64 现在各自在
端口层改写自己的 trap 帧（mepc / ERA），所以契约改成两向三档：

    LA64 / RV32IMAC   开档必须编得过（并且不许再出现拒绝文案），关档也要编得过
    LA32              开档必须仍被 #error 拒绝（帧布局没实现，静默改写就是错答案）

第二条是防假绿的负对照：如果哪天有人把 #error 整段删掉，正向两档照样全绿，
只有 LA32 这一条会把「没实现却编过了」抓出来。

用法
----
    python tools/swap_syntax_gate.py
    python tools/swap_syntax_gate.py --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 有文件编不过或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import shell_src

from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ARMCLANG_FALLBACKS = [
    r"D:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
    r"C:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
]

# 跨架构那一向：非 ARM 打开这个开关必须是编译期报错，不是静默改写 a0。
LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
RV32_FALLBACKS = [
    "D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe",
]
# 拒绝文案的稳定片段：RV32IMAC / LA64 现在是「实现了」，只有 RV64GC / LA32
# 还留在「帧没实现」那一侧。
CROSS_ARCH_REFUSAL_MARKER = "frames are not implemented"

CROSS_ARCH_INCLUDES = [
    "kernelsrc/include",
    "config",
    "kernelsrc/components/mdk_trace",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
]

# 只为让 features.h 的「有签名开关就必须有密钥」那条 #error 过关。
CROSS_SIGN_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""

INCLUDES = [
    "kernelsrc/include",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/components/mdk_trace",
    "kernelsrc/components/arkc/include",
    "kernelsrc/components/arkmake/include",
    "kernelsrc/sdk",
    "config",
    "config/stm32f427",
    "board/stm32f427",
    "example/stm32f427/kernel/ARKRTOS_TEST/Core/Inc",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/STM32F4xx_HAL_Driver/Inc",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Device/ST/STM32F4xx/Include",
    "example/stm32f427/kernel/ARKRTOS_TEST/Drivers/CMSIS/Core/Include",
]

# 共享头按 F427 的真值打开了 lwIP 与软总线，这两个组件的头必须能搜到
# （见 arkrtos_f427_env.py）。
INCLUDES += EXTRA_INCLUDES

DEFS = [
    "ARKRTOS_USE_TASK_SWAP=1",
    "ARKRTOS_USE_SWAP_ROLLBACK=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

# 软总线档位宏：与 F427 工程 MiscControls 一致。
DEFS += SOFTBUS_TIER_DEFS

# (源文件, -mcpu)。内核文件用 m4（F427 就是 m4），移植层各编各的。
TARGETS = [
    ("kernelsrc/src/arkrtos_task.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_mini.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_loader.c", "cortex-m4"),
    ("kernelsrc/src/arkrtos_swap_rollback.c", "cortex-m4"),
    # Shell 已按命令族拆分：每一个 shell 源文件都编进档（防止命令搬家后空转）。
] + [(rel, "cortex-m4") for rel in shell_src.rel_list(ROOT)] + [
    ("kernelsrc/port/arm/cortex-m3/arkrtos_port.c", "cortex-m3"),
    ("kernelsrc/port/arm/cortex-m4/arkrtos_port.c", "cortex-m4"),
    ("kernelsrc/port/arm/cortex-m7/arkrtos_port.c", "cortex-m7"),
]


def find_armclang(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in ARMCLANG_FALLBACKS:
        if os.path.exists(cand):
            return cand
    return shutil.which("armclang")


def _find(paths):
    for cand in paths:
        if os.path.exists(cand):
            return cand
    return None


def _cross_compile(cc, arch, board, port, key_dir, on):
    flags = [cc] + arch + [
        "-O2", "-ffreestanding", "-fno-common", "-nostdlib", "-nostartfiles",
        "-fno-builtin", "-fsyntax-only", "-DARKRTOS_SIGN_KEY_LOCAL=1",
        '-DARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
        "-include", "arkrtos_board_config.h", "-I" + key_dir,
    ]
    flags += ["-I" + os.path.join(ROOT, sub) for sub in CROSS_ARCH_INCLUDES]
    flags += [
        "-I" + os.path.join(ROOT, "board", board),
        "-I" + os.path.join(ROOT, "kernelsrc", "port", port),
    ]
    if on:
        flags.append("-DARKRTOS_USE_TASK_SWAP=1")
    r = subprocess.run(flags + [os.path.join(ROOT, "kernelsrc/src/arkrtos_task.c")],
                       capture_output=True, encoding="utf-8", errors="replace")
    return r.returncode, ((r.stdout or "") + (r.stderr or ""))


def check_cross_arch():
    """非 ARM 的三档：两档实现（必须编得过）、一档仍未实现（必须被 #error 拒）。

    正向（LA64 / RV32IMAC）：swap 的帧改写已经下沉到端口层，内核里没有按架构
    写的分支，所以这两个板档在 ON 与 OFF 下都必须编得过；再额外断言 ON 的输出里
    不出现拒绝文案（防止「靠一条 #error 编过了」这种假读数）。
    负向（LA32）：帧布局没有实现，ON 必须非零退出且命中拒绝文案——这是负对照，
    保证那条 #error 还在。

    这里只做 `-fsyntax-only`：链接与真机行为由上板读数负责（RV32 / LA64 各一次
    `swap` 自检，见 docs/无缝升级-双缓冲热切换.md §5）。
    """
    la64 = _find(LA64_FALLBACKS) or shutil.which("loongarch64-newlib-elf-gcc")
    rv32 = _find(RV32_FALLBACKS) or shutil.which("riscv64-unknown-elf-gcc")
    if not la64:
        print("SWAP-GATE FAIL: LA64 cross gcc not found (a skip would be a false green)")
        return 1
    if not rv32:
        print("SWAP-GATE FAIL: RV32 cross gcc not found (a skip would be a false green)")
        return 1

    key_dir = tempfile.mkdtemp(prefix="ark_swap_key_")
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w", encoding="utf-8") as f:
        f.write(CROSS_SIGN_HEADER)

    rv32_arch = ["-march=rv32imac", "-mabi=ilp32", "-mcmodel=medany", "-mno-relax"]
    la_arch = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]

    # (名字, 编译器, 架构参数, 板级目录, 端口目录, 开档期望)
    # 期望 "compile" = ON 必须编过；"refuse" = ON 必须被 #error 拒。
    cases = [
        ("LA64", la64, la_arch, "qemu_loongarch64", "loongarch/la64", "compile"),
        ("RV32", rv32, rv32_arch, "qemu_riscv32", "riscv/rv32imac", "compile"),
        ("LA32", la64, la_arch, "la32_generic", "loongarch/la64", "refuse"),
    ]
    bad = 0
    for name, cc, arch, board, port, want in cases:
        rc_on, out_on = _cross_compile(cc, arch, board, port, key_dir, True)
        rc_off, out_off = _cross_compile(cc, arch, board, port, key_dir, False)

        if want == "compile":
            if rc_on != 0:
                bad += 1
                print("SWAP-GATE FAIL: %s ON does not compile" % name)
                print(out_on[-1200:])
            elif CROSS_ARCH_REFUSAL_MARKER in out_on:
                bad += 1
                print("SWAP-GATE FAIL: %s ON still refuses (message survived)" % name)
        else:
            if (rc_on == 0) or (CROSS_ARCH_REFUSAL_MARKER not in out_on):
                bad += 1
                print("SWAP-GATE FAIL: %s ON was not refused (rc=%d)" % (name, rc_on))
                print(out_on[-800:])

        if rc_off != 0:
            bad += 1
            print("SWAP-GATE FAIL: %s OFF does not compile" % name)
            print(out_off[-800:])
        if bad == 0:
            print("ok: %s %s / OFF compiles" % (name, "ON compiles" if want == "compile" else "ON refused"))
    return bad

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--armclang", default=None)
    args = ap.parse_args()

    cc = find_armclang(args.armclang)
    if not cc:
        print("SWAP-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1

    common = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only"]
    common += ["-D" + d for d in DEFS]
    common += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel, cpu in TARGETS:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + ["-mcpu=" + cpu, src],
                           capture_output=True, encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("SWAP-GATE FAIL: %s (-mcpu=%s)" % (rel, cpu))
            print(out[-1200:])
        else:
            print("ok: %s (-mcpu=%s)" % (rel, cpu))

    # FPU 扩展帧那一半只有在 ARKRTOS_USE_FPU=1 时才编进来，而默认是 0：
    # 不加这一遍，M4/M7 的「104 字节扩展帧」路径平时一个编译器都不经过，
    # 与「默认关 = 没人编译」是同一个坑。M3 没有 FPU，不列。
    for rel, cpu in (("kernelsrc/port/arm/cortex-m4/arkrtos_port.c", "cortex-m4"),
                     ("kernelsrc/port/arm/cortex-m7/arkrtos_port.c", "cortex-m7")):
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + ["-mcpu=" + cpu, "-DARKRTOS_USE_FPU=1", src],
                           capture_output=True, encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("SWAP-GATE FAIL: %s (FPU=1)" % rel)
            print(out[-1200:])
        else:
            print("ok: %s (FPU=1 extended frame path)" % rel)

    bad += check_cross_arch()

    if bad:
        return 1
    print("SWAP-GATE OK (%d files, USE_TASK_SWAP=1; LA64/RV32 compile both ways, "
          "LA32 still refused when ON)" % len(TARGETS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
