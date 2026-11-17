#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SMP 骨架（ARKRTOS_USE_SMP）的门禁：把开关按下去，编一遍并跑账目用例。

为什么需要这一道
----------------
`ARKRTOS_USE_SMP` 默认 0，F427/F401/H743 的 Keil 工程都按默认档编。于是
下面这些代码平时一个编译器都不经过：

    kernelsrc/include/arkrtos_smp.h            每核状态块与六个接口
    kernelsrc/src/arkrtos_smp.c                引导 / 上线 / IPI / 空转
    kernelsrc/src/arkrtos_init.c               SMP 引导调用点
    kernelsrc/include/arkrtos_hal.h            两个新 port 钩子的声明
    kernelsrc/port/arm/cortex-m{3,4,7}/*.c     两个钩子的单核桩

改了任何一处而把开关留在 0，门禁仍然全绿，直到有人真的打开它才炸。这就是
"默认关 = 没人编译"的假绿形态，跟 eset / waitmulti / waitq 那几道同源。

两段验证：
  1. 宿主：编 kernelsrc/src/arkrtos_smp.c + kernelsrc/tests/test_smp_host.c，
     用可编程的 port 桩（-DARKRTOS_CPU_NUM=4）逐条对账——上线掩码、越界
     核号、启动请求被拒时不许虚报上线、IPI 只发在线核、当前核读错时的收敛、
     重复初始化清零。
  2. armclang -fsyntax-only 以 -DARKRTOS_USE_SMP=1 -DARKRTOS_CPU_NUM=2 编
     arkrtos_smp.c / arkrtos_init.c / 三个 Cortex-M 移植层，确认 ON 档与
     两个新 port 钩子在三份 port 里都编得过。

不覆盖（这个骨架本身也没有）：多核调度、每核运行队列、核间任务迁移、
真实 IPI 投递与延迟、多核板上的次核启动。本仓库当前没有任何多核板，
所以这一道的结论只能是"骨架自洽且编得过"，不是"支持多核"。

用法
----
    python tools/smp_gate.py
    python tools/smp_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe ^
                             --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 编不过或用例失败（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

from arkrtos_f427_env import EXTRA_INCLUDES, SOFTBUS_TIER_DEFS

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEST_C = os.path.join(ROOT, "kernelsrc", "tests", "test_smp_host.c")
SMP_C  = os.path.join(ROOT, "kernelsrc", "src", "arkrtos_smp.c")

GCC_FALLBACKS = [
    "gcc",
    r"D:\mingw\MinGW\bin\gcc.exe",
    r"C:\mingw64\bin\gcc.exe",
]

LA64_GCC_FALLBACKS = [
    r"D:\LoongIDE\la64-tool\bin\loongarch64-newlib-elf-gcc.exe",
]

ARMCLANG_FALLBACKS = [
    r"D:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
    r"C:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
]

# LA64 那一档（pin U）：start.S 的次核停靠分支 + port 层真核号。
LA64_ASM  = "example/qemu_loongarch64/start.S"
LA64_PORT = "kernelsrc/port/loongarch/la64/arkrtos_port.c"

LA64_INCLUDES = [
    "kernelsrc/include",
    "config",
    "board/qemu_loongarch64",
    "kernelsrc/port/loongarch/la64",
    "example/qemu_loongarch64",
    "kernelsrc/components/mdk_trace",
    "kernelsrc/shell",
    "kernelsrc/shell/ark_shell",
    "kernelsrc/components/ark_vfs/include",
    "kernelsrc/components/ark_vfs/port",
    "kernelsrc/components/ark_vfs/src",
    "kernelsrc/fs",
    "kernelsrc/fs/littlefs",
    "kernelsrc/components/ark_softbus/include",
    "kernelsrc/components/ark_softbus/src",
    "kernelsrc/components/ark_softbus/port",
]

# 次核停靠分支的三条硬标记：读 CSR.TID、核 0 跳到清 .bss 之前、其它核死等。
# 少任何一条，SMP 档要么两个核一起清 .bss，要么次核进 main —— 都是静默错答案。
PARK_MARKERS = [
    "csrrd   $t0, 0x40",
    "beqz    $t0, 7f",
    "idle    0",
]

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

INCLUDES += EXTRA_INCLUDES

DEFS = [
    "ARKRTOS_USE_SMP=1",
    'ARKRTOS_BOARD_CONFIG="arkrtos_board_config.h"',
    "STM32F427xx",
    "USE_HAL_DRIVER",
]

DEFS += SOFTBUS_TIER_DEFS

TARGETS = [
    "kernelsrc/src/arkrtos_smp.c",
    "kernelsrc/src/arkrtos_init.c",
    "kernelsrc/port/arm/cortex-m3/arkrtos_port.c",
    "kernelsrc/port/arm/cortex-m4/arkrtos_port.c",
    "kernelsrc/port/arm/cortex-m7/arkrtos_port.c",
]

def find_gcc(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in GCC_FALLBACKS:
        if os.path.sep in cand:
            if os.path.exists(cand):
                return cand
        else:
            found = shutil.which(cand)
            if found:
                return found
    return None

def find_armclang(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in ARMCLANG_FALLBACKS:
        if os.path.exists(cand):
            return cand
    return shutil.which("armclang")

def run_host(cc):
    td = tempfile.mkdtemp(prefix="arkrtos-smp-gate-")
    exe = os.path.join(td, "smp_host.exe" if os.name == "nt" else "smp_host")
    cmd = [cc]
    if "mingw" in cc.replace("\\", "/").lower():
        cmd.append("-B" + os.path.dirname(cc))
    cmd += ["-std=gnu11"]
    cmd += ["-D" + d for d in DEFS]
    cmd += ["-DARKRTOS_CPU_NUM=4"]
    cmd += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]
    cmd += ["-o", exe, TEST_C, SMP_C]

    r = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("SMP-GATE FAIL: host build")
        print(((r.stdout or "") + (r.stderr or ""))[-900:])
        shutil.rmtree(td, ignore_errors=True)
        return False

    r = subprocess.run([exe], cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    print(out)
    shutil.rmtree(td, ignore_errors=True)
    return r.returncode == 0 and "SMP-HOST OK" in out

def run_syntax(cc):
    common = [cc, "--target=arm-arm-none-eabi", "-std=c99", "-fsyntax-only",
              "-mcpu=cortex-m4"]
    common += ["-D" + d for d in DEFS]
    common += ["-DARKRTOS_CPU_NUM=2"]
    common += ["-I" + os.path.join(ROOT, p) for p in INCLUDES]

    bad = 0
    for rel in TARGETS:
        src = os.path.join(ROOT, rel)
        r = subprocess.run(common + [src], capture_output=True,
                           encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            bad += 1
            print("SMP-GATE FAIL: %s" % rel)
            print(out[-900:])
        else:
            print("ok: %s" % rel)
    return bad == 0

def find_la64(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in LA64_GCC_FALLBACKS:
        if os.path.exists(cand):
            return cand
    return None


def park_markers_present(path):
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return False
    return all(m in text for m in PARK_MARKERS)


def run_la64(cc):
    """pin U：LA64 次核停靠（真汇编 + 反汇编断言）与真核号（port.c 真编译）。

    三条自检，任一条不过就是假绿：
      1) 把 start.S 的 csrrd 换掉，标记检查必须判缺失；
      2) 汇编产物里必须真出现 csrrd 与 idle（不是被 #if 吃掉）；
      3) 关档文本里 cpu_id 必须仍是 return 0（默认档不许读 CSR）。
    """
    od = cc.replace("gcc", "objdump")
    if not os.path.exists(od):
        print("SMP-GATE FAIL: no LA64 objdump next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-la64-")
    bad = 0
    src = os.path.join(ROOT, LA64_ASM)

    # 自检 1：改坏副本必须被抓住
    mut = os.path.join(td, "start_mut.S")
    with open(src, encoding="utf-8") as f:
        text = f.read()
    with open(mut, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("csrrd   $t0, 0x40", "nop"))
    if park_markers_present(mut):
        print("SMP-GATE FAIL: LA64 self-check missed a removed TID read")
        bad += 1
    else:
        print("ok: LA64 self-check (removed TID read is caught)")

    if not park_markers_present(src):
        print("SMP-GATE FAIL: %s lost the secondary-core park branch" % LA64_ASM)
        bad += 1
    else:
        print("ok: LA64 park branch present in %s" % LA64_ASM)

    # 签名键：features 里有硬闸门，不给 32 字节初值就编不过（不是可有可无）
    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    defs = ["-D" + d for d in DEFS]
    defs += ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2",
             "-DARKRTOS_SIGN_KEY_LOCAL=1",
             "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\""]

    inc = ["-I" + os.path.join(ROOT, x) for x in LA64_INCLUDES]
    inc.insert(0, "-I" + td)

    # 真汇编
    obj = os.path.join(td, "start.o")
    cmd = [cc, "-march=loongarch64", "-mabi=lp64d", "-c", src, "-o", obj,
           "-include", "arkrtos_board_config.h"] + defs + inc
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode != 0:
        print("SMP-GATE FAIL: LA64 start.S does not assemble with SMP=1")
        print(out[-900:])
        bad += 1
    else:
        d = subprocess.run([od, "-d", obj], capture_output=True,
                           encoding="utf-8", errors="replace")
        dis = (d.stdout or "") + (d.stderr or "")
        missing = [m for m in ("csrrd", "idle") if m not in dis]
        if missing:
            print("SMP-GATE FAIL: %s not in the object (branch got compiled out)" %
                  ", ".join(missing))
            bad += 1
        else:
            print("ok: LA64 start.S assembles, csrrd/idle both in the object")

    # port.c 真编译（-fsyntax-only 能抓到符号/宏错误，抓不到链接期）
    cmd = [cc, "-march=loongarch64", "-mabi=lp64d", "-std=c99", "-fsyntax-only",
           os.path.join(ROOT, LA64_PORT),
           "-include", "arkrtos_board_config.h"] + defs + inc
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode != 0:
        print("SMP-GATE FAIL: LA64 port does not compile with SMP=1")
        print(out[-900:])
        bad += 1
    else:
        print("ok: LA64 %s compiles with SMP=1" % LA64_PORT)

    # 自检 3：关档的 cpu_id 必须还是常量 0
    with open(os.path.join(ROOT, LA64_PORT), encoding="utf-8", errors="replace") as f:
        port = f.read()
    if "return 0u;" not in port:
        print("SMP-GATE FAIL: LA64 port lost the SMP-off constant return")
        bad += 1
    else:
        print("ok: LA64 port keeps the SMP-off constant return")

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description="Host + syntax gate for the SMP scaffolding")
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--armclang", default=None)
    ap.add_argument("--la64", default=None,
                    help="LoongArch64 gcc (default: D:\\LoongIDE\\la64-tool\\bin\\loongarch64-newlib-elf-gcc.exe)")
    args = ap.parse_args()

    cc = find_gcc(args.gcc)
    if not cc:
        print("SMP-GATE FAIL: no host C compiler found (use --gcc)")
        return 1
    ac = find_armclang(args.armclang)
    if not ac:
        print("SMP-GATE FAIL: armclang not found (a skip would be a false green)")
        return 1
    la = find_la64(args.la64)
    if not la:
        print("SMP-GATE FAIL: LA64 gcc not found (use --la64); a skip would be a false green")
        return 1

    print("gcc: %s" % cc)
    if not run_host(cc):
        return 1
    if not run_syntax(ac):
        return 1
    if not run_la64(la):
        return 1

    print("SMP-GATE OK (host accounting + %d ARM ON-path files + LA64 pin U, CPU_NUM=4/2)" %
          len(TARGETS))
    return 0

if __name__ == "__main__":
    sys.exit(main())
