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

三、四段（LA64 / QEMU）：
  3. pin U：start.S 的次核停靠分支（真汇编 + 反汇编断言 + 改坏副本自检）；
     以及 port 层真核号（SMP 档读 CSR.TID，关档仍是常量 0）。
  4. pin V：arkrtos_context.S 的 arkrtos_smp_secondary_entry（次核启动入口）
     与 port 层三条 IOCSR 地址（MAIL_SEND / IPI_SEND / IPI_CLEAR）。SMP=1
     真汇编后反汇编必须有 iocsrwr / ertn / 该符号；同一文件 SMP=0 汇编时
     三者必须都不在（否则说明标记钉在了无条件代码上）。

不覆盖：**对称多核调度**（全局时钟同步、次核可进内核、TLB shootdown）与
**真实硬件**上的次核启动 / IPI 延迟（本仓库没有任何多核板）。QEMU 上已实跑的
四件事各有 pin：LA64 的 ②③（pin U/V，文档 §9.6）、LA64 的 ④ AMP 调度分核
（pin W，§9.8）、RV32 的 ②③ ACLINT msip（pin X，§9.7）、LA64 的 ⑤ AMP 自动
负载均衡（pin Y，§9.9）。LA32 侧与 RV32 的 ④ 仍未做，判词不许外推。所以这一道
的结论是"骨架自洽 + 四条实跑路径都被钉住"，不是"通用多核"。

用法
----
    python tools/smp_gate.py
    python tools/smp_gate.py --gcc D:/mingw/MinGW/bin/gcc.exe ^
                             --armclang D:/Keil_v5/ARM/ARMCLANG/bin/armclang.exe

退出码 0 = 全过；1 = 编不过或用例失败（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import re
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

RV32_GCC_FALLBACKS = [
    r"D:\Artchip\luban-lite-master\toolchain\bin\riscv64-unknown-elf-gcc.exe",
]

ARMCLANG_FALLBACKS = [
    r"D:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
    r"C:\Keil_v5\ARM\ARMCLANG\bin\armclang.exe",
]

# LA64 那一档（pin U）：start.S 的次核停靠分支 + port 层真核号。
LA64_ASM  = "example/qemu_loongarch64/start.S"
LA64_PORT = "kernelsrc/port/loongarch/la64/arkrtos_port.c"
LA64_CTX  = "kernelsrc/port/loongarch/la64/arkrtos_context.S"

# pin V：次核启动入口与 IPI 通路（②③）。这些指令只在 ARKRTOS_USE_SMP=1 下
# 才编出来，默认档一个编译器都不经过 —— 与上面 pin U 同源。
CTX_MARKERS = ["arkrtos_smp_secondary_entry", "iocsrwr", "ertn"]
PORT_MARKERS = ["LA_IOCSR_MAIL_SEND", "LA_IOCSR_IPI_SEND", "LA_IOCSR_IPI_CLEAR"]

# pin R1（2026-11-23）：主核 IPI 接收路径。四处在，缺一条这条路就不存在：
#   1) 中断分派里有 LA_ESTAT_IS_IPI 分支（只有 IS_TIMER 时，IPI 一律进致命分支）；
#   2) 该分支里先清挂起（la_ipi_clear_pending）再调处理函数；
#   3) irq_init 打开本核 CORE_EN 与 ECFG.LIE[12]（不打开则永远收不到）；
#   4) send_ipi 不再拒绝核 0（越界仍然拒绝）。
R1_MARKERS = [
    "else if((estat & LA_ESTAT_IS_IPI) != 0u)",   # 分派分支本体，不是 #define
    "la_ipi_clear_pending();\n            arkrtos_smp_ipi_handler();",
    "la_iocsr_write_w(LA_IOCSR_IPI_EN, 0xfffu);",
    "if(cpu >= (uint32)ARKRTOS_CPU_NUM)",
]

MAIN_C = "example/qemu_loongarch64/main.c"

# pin R2（2026-11-23）：次核开分页。五处在，缺一条这条路就不成立：
#   1) 快照记录点（la_tlb_fill / la_tlb_write 末尾各一次，共两条）；
#   2) 快照溢出**不许静默丢条目**，要置标志；
#   3) 铺 256 条窗口条目那段把快照关掉（否则上限立刻被冲爆）；
#   4) 次核换掉 TLBRENTRY，指向自己的重填入口（每核一份故障栈）；
#   5) 打开后读回自证（-3 与「没打开」必须可分辨）。
R2_PORT_MARKERS = [
    "static void la_pg_snap_add(unsigned long va, uint32 ps, uint32 index,",
    "s_pg_snap_overflow = 1u;",
    "s_pg_snap_on = 0u;\n#endif\n    la_pg_boot_map();",
    "LA_CSRW(LA_CSR_TLBRENTRY, (unsigned long)&tlb_refill_entry_secondary);",
    "static uint32 la_pg_identity_ok(unsigned long va, uint32 ppn, uint32 ps)",
]
R2_SNAP_CALL = "la_pg_snap_add(vaddr, ps, index, lo0, lo1);"
R2_CTX_SEC_STACK = "la_refill_stack_sec_top"

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

def find_rv32(explicit):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for cand in RV32_GCC_FALLBACKS:
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

def run_la64_pin_r1(cc):
    """pin R1：主核 IPI 接收路径（文本标记 + 关档符号自检）。

    三条断言，每条都带自检，缺任何一条这条路就只是「写了注释」：
      1) 四处标记都在 port.c 里（见 R1_MARKERS）；
      2) 逐块抠掉其中一条，标记检查必须报缺失 —— 否则钉的是别处同名字符串；
      3) SMP=1 的目标文件里有 la_ipi_clear_pending，SMP=0 的目标文件里没有
         —— 主核那条分支真的只在开关档里编出来。
    """
    od = cc.replace("gcc", "objdump")
    nm = cc.replace("gcc", "nm")
    if not os.path.exists(od) or not os.path.exists(nm):
        print("SMP-GATE FAIL: no LA64 objdump/nm next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-r1-")
    bad = 0

    with open(os.path.join(ROOT, LA64_PORT), encoding="utf-8", errors="replace") as f:
        port = f.read().replace("\r\n", "\n")   # 行尾无关：标记按 LF 写，CRLF 文件同样命中

    def r1_markers_present(text):
        return all(m in text for m in R1_MARKERS)

    # 自检：每一条抠掉后都必须被抓住
    for m in R1_MARKERS:
        mut = port.replace(m, "/* removed */", 1)
        if r1_markers_present(mut):
            print("SMP-GATE FAIL: pin R1 self-check missed a removed marker: %r" % m[:40])
            bad += 1
        else:
            print("ok: pin R1 self-check catches a removed marker (%s...)" % m[:28])

    if not r1_markers_present(port):
        print("SMP-GATE FAIL: %s lost a main-core IPI marker" % LA64_PORT)
        bad += 1
    else:
        print("ok: main-core IPI path present in %s (dispatch + clear + enable + send)" % LA64_PORT)

    # 演示侧：自检必须真调起来，否则读数不会出现
    with open(os.path.join(ROOT, MAIN_C), encoding="utf-8", errors="replace") as f:
        main_c = f.read()
    if "arkrtos_smp_ping_self_test();" not in main_c:
        print("SMP-GATE FAIL: the SMP fixture never runs the main-core IPI self test")
        bad += 1
    else:
        print("ok: the SMP fixture runs the main-core IPI self test")

    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    inc = ["-I" + os.path.join(ROOT, x) for x in LA64_INCLUDES]
    inc.insert(0, "-I" + td)
    base = ["-march=loongarch64", "-mabi=lp64d", "-std=c99", "-c",
            os.path.join(ROOT, LA64_PORT), "-include", "arkrtos_board_config.h",
            "-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\""]

    def build(tag, smp):
        obj = os.path.join(td, "port_%s.o" % tag)
        cmd = [cc] + base + ["-DARKRTOS_USE_SMP=%d" % smp,
                             "-DARKRTOS_CPU_NUM=2", "-o", obj] + inc
        rr = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                            encoding="utf-8", errors="replace")
        if rr.returncode != 0:
            print("SMP-GATE FAIL: port.c does not build with SMP=%d" % smp)
            print((((rr.stdout or "") + (rr.stderr or "")).strip())[-900:])
            return None
        return obj

    on_obj = build("on", 1)
    off_obj = build("off", 0)
    if (on_obj is None) or (off_obj is None):
        bad += 1
    else:
        def symbols(obj):
            rr = subprocess.run([nm, obj], capture_output=True,
                                encoding="utf-8", errors="replace")
            return (rr.stdout or "") + (rr.stderr or "")

        on_syms = symbols(on_obj)
        off_syms = symbols(off_obj)
        if "la_ipi_clear_pending" not in on_syms:
            print("SMP-GATE FAIL: SMP=1 object has no la_ipi_clear_pending "
                  "(the IPI dispatch got compiled out)")
            bad += 1
        elif "la_ipi_clear_pending" in off_syms:
            print("SMP-GATE FAIL: SMP=0 object still has la_ipi_clear_pending "
                  "(the default build must not pay for this)")
            bad += 1
        else:
            print("ok: la_ipi_clear_pending exists only in the SMP=1 object")

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0

def run_la64_pin_v(cc):
    """pin V：② 次核启动入口 + ③ IPI 通路（真汇编 + 文本标记 + 关档自检）。

    pin U 钉的是「次核停靠分支存在」，但那条分支在 QEMU -kernel 直启下根本
    不可达（次核 PC 是 pflash 的 slave_boot_code）。真正把次核拉起来的是
    arkrtos_context.S 里的 arkrtos_smp_secondary_entry（由 firmware 经
    mailbox 跳进来）和 port 层的两个 IOCSR 写。这里把这两处钉住：

      1) context.S 以 SMP=1 真汇编，反汇编里必须出现 iocsrwr / ertn 与
         secondary_entry 符号；
      2) 自检：同一份文件以 SMP=0 汇编，上述标记必须**消失** —— 否则说明
         标记来自无条件代码，钉错了地方；
      3) port.c 文本里三条 IOCSR 宏必须都在；自检：抠掉一条必须判缺失。
    """
    od = cc.replace("gcc", "objdump")
    if not os.path.exists(od):
        print("SMP-GATE FAIL: no LA64 objdump next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-la64v-")
    bad = 0
    src = os.path.join(ROOT, LA64_CTX)

    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    base_defs = ["-DARKRTOS_SIGN_KEY_LOCAL=1",
                 "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\""]
    inc = ["-I" + td]
    inc += ["-I" + os.path.join(ROOT, x) for x in LA64_INCLUDES]

    def assemble(tag, smp, path=None):
        obj = os.path.join(td, tag + ".o")
        cmd = [cc, "-march=loongarch64", "-mabi=lp64d", "-c",
               path or src, "-o", obj,
               "-include", "arkrtos_board_config.h"] + base_defs + inc
        cmd += ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2"] if smp else \
               ["-DARKRTOS_USE_SMP=0"]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                           encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            print("SMP-GATE FAIL: LA64 %s does not assemble (SMP=%d)" % (tag, int(smp)))
            print(out[-900:])
            return None
        d = subprocess.run([od, "-d", obj], capture_output=True,
                           encoding="utf-8", errors="replace")
        return (d.stdout or "") + (d.stderr or "")

    on = assemble("smpon", True)
    if on is None:
        bad += 1
    else:
        missing = [m for m in CTX_MARKERS if m not in on]
        if missing:
            print("SMP-GATE FAIL: %s missing from the SMP=1 object" %
                  ", ".join(missing))
            bad += 1
        else:
            print("ok: LA64 context.S SMP=1 has secondary entry + iocsr + ertn")

    # EUEN pin：FPU 档的次核入口必须自己置 EUEN.FPE（2026-11-21 回归，见
    # docs/QEMU模拟器移植与验证.md 9.6.10）。主核的 EUEN 是在 start.S 里设的，
    # 次核走的是硬件复位后的独立 CSR 状态、不继承；而 FPU 档的 eentry 会
    # 无条件 fst.d 保存 FPR，缺这一位时次核一进陷阱就落 Ecode=15（FPD），
    # 异常入口自己再故障 —— 表现是 IPI 永远收不到、次核原地打转。
    # 变异自检：把置位那条 ori 抠掉重汇编，CSR 0x2 的写必须跟着消失。
    euen_re = re.compile(r"csrwr\s+\$\w+,0x2\b")

    def sec_entry(text):
        i = text.find("<arkrtos_smp_secondary_entry>:")
        if i < 0:
            return ""
        j = text.find("\n\n", i)
        return text[i:j if j >= 0 else len(text)]

    if on is not None:
        body = sec_entry(on)
        if not euen_re.search(body):
            print("SMP-GATE FAIL: secondary entry never writes EUEN (CSR 0x2); "
                  "an FPU build dies on the first trap with Ecode=15 FPD")
            bad += 1
        else:
            mut_src = os.path.join(td, "ctx_mut.S")
            raw = open(src, "rb").read().replace(
                b"    csrwr   $t1, 0x2\r\n", b"    nop\r\n")
            with open(mut_src, "wb") as f:
                f.write(raw)
            mut = assemble("smpmut", True, mut_src)
            if mut is None:
                bad += 1
            elif euen_re.search(sec_entry(mut)):
                print("SMP-GATE FAIL: dropping the EUEN write did not remove "
                      "the CSR 0x2 write (pin does not point at it)")
                bad += 1
            else:
                print("ok: LA64 EUEN pin (CSR 0x2 written in secondary entry, "
                      "gone when the setter is removed)")

    off = assemble("smpoff", False)
    if off is None:
        bad += 1
    else:
        leaked = [m for m in ("iocsrwr", "arkrtos_smp_secondary_entry")
                  if m in off]
        if leaked:
            print("SMP-GATE FAIL: %s still in the SMP=0 object (pin V is in the "
                  "wrong place)" % ", ".join(leaked))
            bad += 1
        else:
            print("ok: LA64 self-check (SMP=0 object has no SMP markers)")

    def missing_markers(text):
        return [m for m in PORT_MARKERS if m not in text]

    try:
        port = open(os.path.join(ROOT, LA64_PORT), encoding="utf-8",
                    errors="replace").read()
    except OSError:
        port = ""
    missing = missing_markers(port)
    if missing:
        print("SMP-GATE FAIL: %s missing from %s" % (", ".join(missing), LA64_PORT))
        bad += 1
    else:
        print("ok: LA64 port keeps the three IOCSR addresses")

    # 自检：抠掉一条宏名，同一个判据必须报出来（否则这道 pin 是假绿）
    if missing_markers(port.replace(PORT_MARKERS[0], "")):
        print("ok: LA64 port self-check (a removed IOCSR name is detected)")
    else:
        print("SMP-GATE FAIL: port marker self-check missed a removed name")
        bad += 1

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0


# ---------------------------------------------------------------- pin W / X
# pin W：④ 调度分核（AMP）。这一档的代码全部在 ARKRTOS_USE_SMP_SCHED 下，
# 默认档一个编译器都不经过 —— 与 pin U/V 同源的假绿形态。
SCHED_H = "kernelsrc/include/arkrtos_smp_sched.h"
SCHED_C = "kernelsrc/src/arkrtos_smp_sched.c"
TASK_C  = "kernelsrc/src/arkrtos_task.c"

SCHED_API = [
    "arkrtos_smp_sched_enter",
    "arkrtos_smp_sched_task_to",
    "arkrtos_smp_sched_task_back",
    "arkrtos_smp_sched_switch",
    "arkrtos_smp_sched_poll",
    "arkrtos_smp_sched_owned_by_other",
]

# 这三处跳过必须各自在自己的 #if (ARKRTOS_USE_SMP_SCHED == 1) 里：漏一处，
# 迁到次核的任务就会被 CPU0 当成普通 READY 任务重新挂回就绪链，等于两个核
# 同时动同一份栈帧。
SKIP_GUARD = "#if (ARKRTOS_USE_SMP_SCHED == 1)"
SKIP_CALL  = "arkrtos_smp_sched_owned_by_other"


def every_skip_is_guarded(text):
    """每一处 owned_by_other 调用前 400 字符内必须有自己的开关 #if。"""
    pos = 0
    seen = 0
    while True:
        i = text.find(SKIP_CALL, pos)
        if i < 0:
            break
        seen += 1
        if SKIP_GUARD not in text[max(0, i - 400):i]:
            return seen, i
        pos = i + 1
    return seen, -1


def run_la64_pin_r2(cc):
    """pin R2：次核开分页（快照重放 + 每核故障栈 + 读回自证）。

    四层断言，每层都带自检，缺一层这条路就可能只是「写了注释」：
      1) port.c 里四处标记都在；抠掉任意一处，同一判据必须报出来；
      2) 快照记录点必须**恰好两处**（la_tlb_fill / la_tlb_write 各一次）；
         抠掉一处后计数变 1，判据不许仍然说「够」；
      3) SMP=1 的目标文件里有 arkrtos_port_paging_enable_secondary 与
         arkrtos_port_paging_enable_secondary 之外的快照符号，SMP=0 的目标
         文件里一个都没有 —— 默认档零变化不是靠嘴说；
      4) context.S 以 SMP=1 汇编后，次核重填入口的 la.abs 必须指向
         la_refill_stack_sec_top（每核一份栈）；把那个符号换成引导核的
         la_refill_stack_top 重汇编，这条引用必须消失 —— 否则钉的是
         「某个栈」，不是「本核自己的栈」。
    """
    od = cc.replace("gcc", "objdump")
    nm = cc.replace("gcc", "nm")
    if not os.path.exists(od) or not os.path.exists(nm):
        print("SMP-GATE FAIL: no LA64 objdump/nm next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-r2-")
    bad = 0

    with open(os.path.join(ROOT, LA64_PORT), encoding="utf-8",
              errors="replace") as f:
        port = f.read().replace("\r\n", "\n")

    def r2_markers_present(text):
        return all(m in text for m in R2_PORT_MARKERS)

    for m in R2_PORT_MARKERS:
        mut = port.replace(m, "/* removed */", 1)
        if r2_markers_present(mut):
            print("SMP-GATE FAIL: pin R2 self-check missed a removed marker: %r"
                  % m[:44])
            bad += 1
        else:
            print("ok: pin R2 self-check catches a removed marker (%s...)" % m[:30])

    if not r2_markers_present(port):
        print("SMP-GATE FAIL: %s lost a secondary-paging marker" % LA64_PORT)
        bad += 1
    else:
        print("ok: secondary paging path present in %s (snapshot + replay + "
              "refill entry + readback)" % LA64_PORT)

    calls = port.count(R2_SNAP_CALL)
    if calls != 2:
        print("SMP-GATE FAIL: %s records the static snapshot %d time(s), "
              "expected 2 (la_tlb_fill + la_tlb_write)" % (LA64_PORT, calls))
        bad += 1
    elif (port.replace(R2_SNAP_CALL, "/* removed */", 1)).count(R2_SNAP_CALL) != 1:
        print("SMP-GATE FAIL: pin R2 snapshot-call count is not specific")
        bad += 1
    else:
        print("ok: static snapshot recorded at both TLB writers (2 call sites)")

    with open(os.path.join(ROOT, MAIN_C), encoding="utf-8",
              errors="replace") as f:
        main_c = f.read()
    if "arkrtos_smp_paging_self_test();" not in main_c:
        print("SMP-GATE FAIL: the SMP fixture never runs the secondary-paging "
              "self test")
        bad += 1
    elif "[smp] pg cpu" not in main_c:
        print("SMP-GATE FAIL: the SMP fixture never prints the pg reading")
        bad += 1
    else:
        print("ok: the SMP fixture runs the secondary-paging self test and "
              "prints its readings")

    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    inc = ["-I" + td]
    inc += ["-I" + os.path.join(ROOT, x) for x in LA64_INCLUDES]
    base = ["-march=loongarch64", "-mabi=lp64d", "-std=c99", "-c",
            os.path.join(ROOT, LA64_PORT), "-include", "arkrtos_board_config.h",
            "-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\""]

    def build(tag, smp):
        obj = os.path.join(td, "port_%s.o" % tag)
        cmd = [cc] + base + ["-DARKRTOS_USE_SMP=%d" % smp,
                             "-DARKRTOS_CPU_NUM=2", "-o", obj] + inc
        rr = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                            encoding="utf-8", errors="replace")
        if rr.returncode != 0:
            print("SMP-GATE FAIL: port.c does not build with SMP=%d" % smp)
            print((((rr.stdout or "") + (rr.stderr or "")).strip())[-900:])
            return None
        return obj

    on_obj = build("on", 1)
    off_obj = build("off", 0)
    if (on_obj is None) or (off_obj is None):
        bad += 1
    else:
        def symbols(obj):
            rr = subprocess.run([nm, obj], capture_output=True,
                               encoding="utf-8", errors="replace")
            return (rr.stdout or "") + (rr.stderr or "")

        on_syms = symbols(on_obj)
        off_syms = symbols(off_obj)
        sym = "arkrtos_port_paging_enable_secondary"
        if sym not in on_syms:
            print("SMP-GATE FAIL: SMP=1 object has no %s (the whole R2 path got "
                  "compiled out)" % sym)
            bad += 1
        elif sym in off_syms:
            print("SMP-GATE FAIL: SMP=0 object still has %s (the default build "
                  "must not pay for this)" % sym)
            bad += 1
        else:
            print("ok: %s exists only in the SMP=1 object" % sym)

    # context.S：次核重填入口必须用本核自己的栈
    src = os.path.join(ROOT, LA64_CTX)

    def assemble(tag, smp, path=None, with_reloc=False):
        obj = os.path.join(td, tag + ".o")
        cmd = [cc, "-march=loongarch64", "-mabi=lp64d", "-c",
               path or src, "-o", obj,
               "-include", "arkrtos_board_config.h",
               "-DARKRTOS_SIGN_KEY_LOCAL=1",
               "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\""] + inc
        cmd += ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2"] if smp else \
               ["-DARKRTOS_USE_SMP=0"]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True,
                           encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + (r.stderr or "")).strip()
        if r.returncode != 0:
            print("SMP-GATE FAIL: LA64 %s does not assemble (SMP=%d)" % (tag, int(smp)))
            print(out[-900:])
            return None
        flags = ["-dr"] if with_reloc else ["-d"]
        d = subprocess.run([od] + flags + [obj], capture_output=True,
                           encoding="utf-8", errors="replace")
        return (d.stdout or "") + (d.stderr or "")

    on_ctx = assemble("ctxon", True, None, True)
    if on_ctx is None:
        bad += 1
    else:
        if "<tlb_refill_entry_secondary>:" not in on_ctx:
            print("SMP-GATE FAIL: SMP=1 context.S has no secondary refill entry")
            bad += 1
        elif R2_CTX_SEC_STACK not in on_ctx:
            print("SMP-GATE FAIL: the secondary refill entry does not use its "
                  "own static stack (%s)" % R2_CTX_SEC_STACK)
            bad += 1
        else:
            # 自检：把本核栈换成引导核栈，同一个判据必须报出来
            #        注意要把三处一起换：$r21 的加载、$sp 的加载、
            #        以及 .globl/标签定义（否则符号仍在对象里，自检会假 FAIL）。
            mut_src = os.path.join(td, "ctx_mut_r2.S")
            raw = open(src, "rb").read()
            raw = raw.replace(
                b"la.abs  $r21, la_refill_stack_sec_top",
                b"la.abs  $r21, la_refill_stack_top")
            raw = raw.replace(
                b"la.abs  $sp, la_refill_stack_sec_top",
                b"la.abs  $sp, la_refill_stack_top")
            raw = raw.replace(b"la_refill_stack_sec_", b"la_refill_stack_mut_")
            with open(mut_src, "wb") as f:
                f.write(raw)
            mut = assemble("ctxmut", True, mut_src, True)
            if mut is None:
                bad += 1
            elif R2_CTX_SEC_STACK in mut:
                print("SMP-GATE FAIL: swapping the per-core stack did not remove "
                      "the reference (pin does not point at it)")
                bad += 1
            else:
                print("ok: LA64 R2 pin (secondary refill entry uses its own "
                      "stack, gone when swapped)")

    off_ctx = assemble("ctxoff", False)
    if off_ctx is None:
        bad += 1
    else:
        leaked = [m for m in ("tlb_refill_entry_secondary", R2_CTX_SEC_STACK)
                  if m in off_ctx]
        if leaked:
            print("SMP-GATE FAIL: %s still in the SMP=0 object (pin R2 is in the "
                  "wrong place)" % ", ".join(leaked))
            bad += 1
        else:
            print("ok: LA64 R2 self-check (SMP=0 object has no secondary refill "
                  "entry and no per-core stack)")

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0


def run_la64_smp_sched(cc):
    """pin W：④ AMP 调度分核（真编译两档 + 关档零内容 + 三处跳过各有开关）。"""
    od = cc.replace("gcc", "objdump")
    if not os.path.exists(od):
        print("SMP-GATE FAIL: no LA64 objdump next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-w-")
    bad = 0

    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    base = ["-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h"]
    inc = ["-I" + td] + ["-I" + os.path.join(ROOT, x) for x in LA64_INCLUDES]

    try:
        sched_h = open(os.path.join(ROOT, SCHED_H), encoding="utf-8",
                       errors="replace").read()
        sched_c = open(os.path.join(ROOT, SCHED_C), encoding="utf-8",
                       errors="replace").read()
        task_c = open(os.path.join(ROOT, TASK_C), encoding="utf-8",
                      errors="replace").read()
    except OSError as exc:
        print("SMP-GATE FAIL: cannot read the AMP module (%s)" % exc)
        shutil.rmtree(td, ignore_errors=True)
        return False

    missing = [a for a in SCHED_API if a not in sched_h or a not in sched_c]
    if missing:
        print("SMP-GATE FAIL: %s missing from the AMP header/source" %
              ", ".join(missing))
        bad += 1
    else:
        print("ok: AMP module exposes all six contract symbols")

    # 自检：抠掉一个符号名，同一个判据必须报出来
    if [a for a in SCHED_API if a not in sched_h.replace(SCHED_API[0], "")]:
        print("ok: AMP self-check (a removed symbol name is detected)")
    else:
        print("SMP-GATE FAIL: AMP symbol self-check missed a removed name")
        bad += 1

    # 真编译：ON 档（-fsyntax-only 能抓符号/宏/类型错误）
    on_defs = base + ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2",
                      "-DARKRTOS_USE_SMP_SCHED=1"]
    r = subprocess.run([cc, "-march=loongarch64", "-mabi=lp64d", "-std=c99",
                        "-fsyntax-only", os.path.join(ROOT, SCHED_C)] +
                       on_defs + inc,
                       cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode != 0:
        print("SMP-GATE FAIL: AMP module does not compile with SMP_SCHED=1")
        print(out[-900:])
        bad += 1
    else:
        print("ok: AMP module compiles with SMP_SCHED=1")

    # 关档零内容：默认档必须编成一个没有 .text 的空目标
    off_obj = os.path.join(td, "sched_off.o")
    r = subprocess.run([cc, "-march=loongarch64", "-mabi=lp64d", "-std=c99",
                        "-c", os.path.join(ROOT, SCHED_C), "-o", off_obj] +
                       base + inc,
                       cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("SMP-GATE FAIL: AMP module does not compile with the switch off")
        print((((r.stdout or "") + (r.stderr or "")).strip())[-900:])
        bad += 1
    else:
        d = subprocess.run([od, "-h", off_obj], capture_output=True,
                           encoding="utf-8", errors="replace")
        dis = (d.stdout or "") + (d.stderr or "")
        text_size = ""
        for ln in dis.splitlines():
            if ".text " in ln or ln.strip().startswith("0 .text"):
                text_size = ln.split()[2] if len(ln.split()) > 2 else ""
        syms = subprocess.run([od, "-t", off_obj], capture_output=True,
                              encoding="utf-8", errors="replace")
        # 只数真正的符号行：objdump 会把 "xxx.o:     file format ..." 也打进来，
        # 拿整段去找名字会把文件名当成符号（那样这条判据恒红）。
        # *ABS* 那几行是文件名与段名，不是定义出来的符号；只留真符号。
        sym_text = "\n".join(ln for ln in
                             ((syms.stdout or "") + (syms.stderr or "")).splitlines()
                             if "file format" not in ln and "*ABS*" not in ln
                             and " .text" not in ln and " .data" not in ln
                             and " .bss" not in ln and " .comment" not in ln)
        if text_size.strip("0") != "" or "arkrtos_smp_sched" in sym_text:
            print("SMP-GATE FAIL: AMP module is not empty when the switch is off "
                  "(.text=%s)" % text_size)
            bad += 1
        else:
            print("ok: AMP module is an empty object when the switch is off "
                  "(.text=0, no symbols)")

    # 三处跳过必须各自带开关
    seen, bad_at = every_skip_is_guarded(task_c)
    if seen < 3:
        print("SMP-GATE FAIL: only %d guarded skip site(s) in %s (expect >= 3)" %
              (seen, TASK_C))
        bad += 1
    elif bad_at >= 0:
        print("SMP-GATE FAIL: a skip site at offset %d is outside its own "
              "#if (ARKRTOS_USE_SMP_SCHED == 1)" % bad_at)
        bad += 1
    else:
        print("ok: %d guarded skip site(s) in arkrtos_task.c" % seen)

    # 自检：把第一处跳过头上那道开关抠掉，判据必须报出来（必须抠「跳过前面
    # 那一道」，抠文件里第一道只会抠到 include 的开关，等于什么都没验）
    first = task_c.find(SKIP_CALL)
    at = task_c.rfind(SKIP_GUARD, 0, first)
    assert at >= 0
    mut = task_c[:at] + task_c[at + len(SKIP_GUARD):]
    if every_skip_is_guarded(mut)[1] >= 0:
        print("ok: skip-guard self-check (an unguarded site is detected)")
    else:
        print("SMP-GATE FAIL: skip-guard self-check missed an unguarded site")
        bad += 1

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0


RV32_ASM  = "example/qemu_riscv32/start.S"
RV32_PORT = "kernelsrc/port/riscv/rv32imac/arkrtos_port.c"
RV32_CTX  = "kernelsrc/port/riscv/rv32imac/arkrtos_context.S"

RV32_ASM_MARKERS = ["csrr    t0, mhartid", "msip", "arkrtos_smp_secondary_main"]
RV32_PORT_MARKERS = ["rv_clint_msip_write", "arkrtos_port_cpu_start",
                     "arkrtos_port_send_ipi"]


def run_rv32_smp(cc):
    """pin X：RV32 侧 ②③（ACLINT msip）。真汇编 + 关档自检 + 文本标记。"""
    od = cc.replace("gcc", "objdump")
    if not os.path.exists(od):
        print("SMP-GATE FAIL: no RV32 objdump next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-x-")
    bad = 0

    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    defs = ["-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h"]
    inc = ["-I" + td]
    inc += ["-I" + os.path.join(ROOT, x) for x in (
        "kernelsrc/include", "config", "board/qemu_riscv32",
        "kernelsrc/port/riscv/rv32imac", "example/qemu_riscv32",
        "kernelsrc/components/mdk_trace", "kernelsrc/shell",
        "kernelsrc/shell/ark_shell", "kernelsrc/components/ark_vfs/include",
        "kernelsrc/components/ark_vfs/port", "kernelsrc/components/ark_vfs/src",
        "kernelsrc/fs", "kernelsrc/fs/littlefs",
        "kernelsrc/components/ark_softbus/include",
        "kernelsrc/components/ark_softbus/src",
        "kernelsrc/components/ark_softbus/port")]

    src = os.path.join(ROOT, RV32_ASM)
    try:
        text = open(src, encoding="utf-8", errors="replace").read()
    except OSError as exc:
        print("SMP-GATE FAIL: cannot read %s (%s)" % (RV32_ASM, exc))
        shutil.rmtree(td, ignore_errors=True)
        return False

    missing = [m for m in RV32_ASM_MARKERS if m not in text]
    if missing:
        print("SMP-GATE FAIL: %s missing from %s" % (", ".join(missing), RV32_ASM))
        bad += 1
    else:
        print("ok: RV32 start.S has the mhartid branch + msip park loop")

    # 自检：改坏副本必须被抓住
    mut = os.path.join(td, "start_mut.S")
    with open(mut, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("csrr    t0, mhartid", "nop"))
    mut_text = open(mut, encoding="utf-8", errors="replace").read()
    if [m for m in RV32_ASM_MARKERS if m not in mut_text]:
        print("ok: RV32 self-check (a removed mhartid read is caught)")
    else:
        print("SMP-GATE FAIL: RV32 self-check missed a removed mhartid read")
        bad += 1

    # 真汇编（SMP=1）
    obj = os.path.join(td, "start.o")
    r = subprocess.run([cc, "-march=rv32imac", "-mabi=ilp32",
                        "-mcmodel=medany", "-mno-relax", "-c", src, "-o", obj] +
                       defs + inc + ["-DARKRTOS_USE_SMP=1",
                                     "-DARKRTOS_CPU_NUM=2"],
                       cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("SMP-GATE FAIL: RV32 start.S does not assemble with SMP=1")
        print((((r.stdout or "") + (r.stderr or "")).strip())[-900:])
        bad += 1
    else:
        # -r 是必须的：call 的目标是重定位，不加 -r 反汇编里看不到符号名，
        # 这条判据就会恒红（假失败比假绿好，但假失败也会让人关掉门禁）。
        d = subprocess.run([od, "-dr", obj], capture_output=True,
                           encoding="utf-8", errors="replace")
        dis = (d.stdout or "") + (d.stderr or "")
        if "mhartid" not in dis or "arkrtos_smp_secondary_main" not in dis:
            print("SMP-GATE FAIL: mhartid / secondary entry missing from the "
                  "SMP=1 RV32 object (branch got compiled out)")
            bad += 1
        else:
            print("ok: RV32 start.S assembles, mhartid + secondary entry present")

    try:
        port = open(os.path.join(ROOT, RV32_PORT), encoding="utf-8",
                    errors="replace").read()
    except OSError:
        port = ""
    missing = [m for m in RV32_PORT_MARKERS if m not in port]
    if missing:
        print("SMP-GATE FAIL: %s missing from %s" % (", ".join(missing), RV32_PORT))
        bad += 1
    else:
        print("ok: RV32 port keeps the msip writer and the two bring-up hooks")

    # 真语法检查（SMP=1 的 port.c）
    r = subprocess.run([cc, "-march=rv32imac", "-mabi=ilp32",
                        "-mcmodel=medany", "-mno-relax", "-std=c99",
                        "-fsyntax-only", os.path.join(ROOT, RV32_PORT)] +
                       defs + inc + ["-DARKRTOS_USE_SMP=1",
                                     "-DARKRTOS_CPU_NUM=2"],
                       cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        print("SMP-GATE FAIL: RV32 port does not compile with SMP=1")
        print((((r.stdout or "") + (r.stderr or "")).strip())[-900:])
        bad += 1
    else:
        print("ok: RV32 %s compiles with SMP=1" % RV32_PORT)

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0

# ---------------------------------------------------------------- pin Y
# pin Y：⑤ 自动负载均衡（ARKRTOS_USE_SMP_BALANCE）。这一档的代码被两个开关
# 夹着（SMP_SCHED 是前置，BALANCE 自己再一道），默认档一个编译器都不经过 ——
# 与 pin U/V/W 同源的假绿形态。钉四件事：
#   1) 四个契约符号在头/源里都在，且**只在 BALANCE=1 档**才落到目标文件里
#      （关档零符号，靠 objdump -t 对照，不靠"看着像关了"）；
#   2) 两条 #error 依赖是真编译出来的（BALANCE=1 缺 SMP_SCHED / CPU_NUM=1
#      必须编不过），同时好档必须编得过 —— 否则"永远失败"也能骗过判据；
#   3) 策略必须从 CPU0 的 poll 里被调起，而且那道调用在自己的 #if 里；
#   4) 两处「不许迁正在跑的任务」的保护各在自己的位置上（-6 返回码 +
#      候选筛选里的 current 判定）。
BAL_H   = "kernelsrc/include/arkrtos_smp_sched.h"
BAL_C   = SCHED_C
BAL_MAIN = "example/qemu_loongarch64/main.c"
BAL_API = ["arkrtos_smp_sched_auto_add",
           "arkrtos_smp_sched_auto_del",
           "arkrtos_smp_sched_auto_count",
           "arkrtos_smp_sched_balance_stat"]
BAL_GUARD = "#if (ARKRTOS_USE_SMP_BALANCE == 1)"

def bal_poll_ok(text):
    """poll 里那一句 (void)sms_balance_step(); 必须在 BALANCE 开关之内。"""
    i = text.find("void arkrtos_smp_sched_poll(void)")
    if i < 0:
        return False
    seg = text[i:]
    j = seg.find("(void)sms_balance_step();")
    if j < 0:
        return False
    return BAL_GUARD in seg[:j]

def run_la64_smp_balance(cc):
    """pin Y：⑤ AMP 自动负载均衡（两档真编译 + 关档零符号 + 依赖 #error + 策略 pin）。"""
    od = cc.replace("gcc", "objdump")
    if not os.path.exists(od):
        print("SMP-GATE FAIL: no LA64 objdump next to %s" % cc)
        return False

    td = tempfile.mkdtemp(prefix="arkrtos-smp-y-")
    bad = 0

    key = os.path.join(td, "arkrtos_sign_key_local.h")
    with open(key, "w", encoding="utf-8") as f:
        f.write("#ifndef ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_SIGN_KEY_LOCAL_H\n"
                "#define ARKRTOS_IMAGE_SIGN_KEY { " +
                ", ".join("0x%02X" % i for i in range(32)) + " }\n"
                "#endif\n")

    base = ["-DARKRTOS_SIGN_KEY_LOCAL=1",
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h"]
    inc = ["-I" + td] + ["-I" + os.path.join(ROOT, x) for x in LA64_INCLUDES]

    try:
        bal_h = open(os.path.join(ROOT, BAL_H), encoding="utf-8",
                     errors="replace").read()
        bal_c = open(os.path.join(ROOT, BAL_C), encoding="utf-8",
                     errors="replace").read()
        main_c = open(os.path.join(ROOT, BAL_MAIN), encoding="utf-8",
                      errors="replace").read()
    except OSError as exc:
        print("SMP-GATE FAIL: cannot read the balance module (%s)" % exc)
        shutil.rmtree(td, ignore_errors=True)
        return False

    missing = [a for a in BAL_API if a not in bal_h or a not in bal_c]
    if missing:
        print("SMP-GATE FAIL: %s missing from the balance header/source" %
              ", ".join(missing))
        bad += 1
    else:
        print("ok: balance module exposes all four contract symbols")

    # 自检：抠掉一个符号名，同一判据必须报出来
    if [a for a in BAL_API if a not in bal_h.replace(BAL_API[0], "")]:
        print("ok: balance self-check (a removed symbol name is detected)")
    else:
        print("SMP-GATE FAIL: balance symbol self-check missed a removed name")
        bad += 1

    on_defs = base + ["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2",
                      "-DARKRTOS_USE_SMP_SCHED=1",
                      "-DARKRTOS_USE_SMP_BALANCE=1"]

    # 真编译：ON 档（-fsyntax-only 抓符号/宏/类型错误）
    r = subprocess.run([cc, "-march=loongarch64", "-mabi=lp64d", "-std=c99",
                        "-fsyntax-only", os.path.join(ROOT, BAL_C)] +
                       on_defs + inc,
                       cwd=ROOT, capture_output=True,
                       encoding="utf-8", errors="replace")
    out = ((r.stdout or "") + (r.stderr or "")).strip()
    if r.returncode != 0:
        print("SMP-GATE FAIL: balance module does not compile with "
              "SMP_BALANCE=1")
        print(out[-900:])
        bad += 1
    else:
        print("ok: balance module compiles with SMP_BALANCE=1")

    # 两档各落一次目标文件：ON 档必须有四个符号，OFF 档一个都不许有。
    def bal_symbols(extra, tag):
        obj = os.path.join(td, "bal_%s.o" % tag)
        rr = subprocess.run([cc, "-march=loongarch64", "-mabi=lp64d", "-std=c99",
                             "-c", os.path.join(ROOT, BAL_C), "-o", obj] +
                            base + extra + inc,
                            cwd=ROOT, capture_output=True,
                            encoding="utf-8", errors="replace")
        if rr.returncode != 0:
            print("SMP-GATE FAIL: balance module does not compile (%s)" % tag)
            print((((rr.stdout or "") + (rr.stderr or "")).strip())[-900:])
            return None
        t = subprocess.run([od, "-t", obj], capture_output=True,
                           encoding="utf-8", errors="replace")
        return (t.stdout or "") + (t.stderr or "")

    on_syms = bal_symbols(["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2",
                           "-DARKRTOS_USE_SMP_SCHED=1",
                           "-DARKRTOS_USE_SMP_BALANCE=1"], "on")
    off_syms = bal_symbols(["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2",
                            "-DARKRTOS_USE_SMP_SCHED=1"], "off")
    if (on_syms is None) or (off_syms is None):
        bad += 1
    else:
        on_hit = [a for a in BAL_API if a in on_syms]
        off_hit = [a for a in BAL_API if a in off_syms]
        if len(on_hit) != len(BAL_API):
            print("SMP-GATE FAIL: BALANCE=1 object is missing %s" %
                  ", ".join(a for a in BAL_API if a not in on_hit))
            bad += 1
        elif off_hit:
            print("SMP-GATE FAIL: BALANCE=0 object still defines %s" %
                  ", ".join(off_hit))
            bad += 1
        else:
            print("ok: BALANCE=1 object defines all four symbols, BALANCE=0 "
                  "object defines none")

    # 两条 #error 依赖：坏档必须编不过，好档必须编得过。
    def bal_compiles(defs):
        rr = subprocess.run([cc, "-march=loongarch64", "-mabi=lp64d", "-std=c99",
                             "-fsyntax-only", os.path.join(ROOT, BAL_C)] +
                            base + defs + inc,
                            cwd=ROOT, capture_output=True,
                            encoding="utf-8", errors="replace")
        return rr.returncode, ((rr.stdout or "") + (rr.stderr or "")).strip()

    rc_bad1, out_bad1 = bal_compiles(["-DARKRTOS_USE_SMP=1",
                                      "-DARKRTOS_CPU_NUM=2",
                                      "-DARKRTOS_USE_SMP_SCHED=0",
                                      "-DARKRTOS_USE_SMP_BALANCE=1"])
    rc_bad2, out_bad2 = bal_compiles(["-DARKRTOS_USE_SMP=1",
                                      "-DARKRTOS_CPU_NUM=1",
                                      "-DARKRTOS_USE_SMP_SCHED=1",
                                      "-DARKRTOS_USE_SMP_BALANCE=1"])
    rc_ok, out_ok = bal_compiles(["-DARKRTOS_USE_SMP=1", "-DARKRTOS_CPU_NUM=2",
                                  "-DARKRTOS_USE_SMP_SCHED=1",
                                  "-DARKRTOS_USE_SMP_BALANCE=1"])
    if rc_bad1 == 0:
        print("SMP-GATE FAIL: SMP_BALANCE=1 without SMP_SCHED compiles "
              "(the dependency is not enforced)")
        bad += 1
    elif "ARKRTOS_USE_SMP_SCHED" not in out_bad1:
        print("SMP-GATE FAIL: BALANCE-without-SCHED failed for an unexpected "
              "reason (no #error text)")
        bad += 1
    elif rc_bad2 == 0:
        print("SMP-GATE FAIL: SMP_BALANCE=1 with CPU_NUM=1 compiles "
              "(the dependency is not enforced)")
        bad += 1
    elif "CPU_NUM" not in out_bad2:
        print("SMP-GATE FAIL: BALANCE-with-CPU_NUM=1 failed for an unexpected "
              "reason (no #error text)")
        bad += 1
    elif rc_ok != 0:
        print("SMP-GATE FAIL: the good BALANCE configuration does not compile "
              "(the two failures above prove nothing)")
        print(out_ok[-600:])
        bad += 1
    else:
        print("ok: both #error dependencies fire, the good configuration compiles")

    # 策略 pin：poll 里的调用必须带开关；-6 保护与候选筛选里的 current 判定都在。
    if bal_poll_ok(bal_c):
        print("ok: sms_balance_step() is called from poll inside its own #if")
    else:
        print("SMP-GATE FAIL: sms_balance_step() is not wired into "
              "arkrtos_smp_sched_poll under ARKRTOS_USE_SMP_BALANCE")
        bad += 1

    mut = bal_c.replace("(void)sms_balance_step();", "/* removed */")
    if bal_poll_ok(mut):
        print("SMP-GATE FAIL: poll self-check missed a removed balance call")
        bad += 1
    else:
        print("ok: poll self-check (a removed balance call is detected)")

    if "return -6;" not in bal_c:
        print("SMP-GATE FAIL: the running-task guard (-6) is gone from "
              "arkrtos_smp_sched_task_to")
        bad += 1
    elif bal_c.count("(idx == (arkrtos_current_task_id - 1))") < 2:
        print("SMP-GATE FAIL: the current-task filter is missing from either "
              "task_to or sms_balance_step")
        bad += 1
    else:
        print("ok: both running-task guards are in place (-6 + candidate filter)")

    # 演示侧：均衡档下手工 SMP 演示必须让位（两者抢同一个次核），且判据齐。
    if main_c.count("(ARKRTOS_USE_SMP_BALANCE != 1)") < 3:
        print("SMP-GATE FAIL: the manual SMP_SCHED demo is not gated off in "
              "BALANCE builds (both fight over the same secondary core)")
        bad += 1
    elif "[bal] park" not in main_c:
        print("SMP-GATE FAIL: the balance demo has no park step "
              "(workers would keep the CPU forever)")
        bad += 1
    else:
        print("ok: balance demo is self-contained (legacy demo gated off, "
              "park step present)")

    shutil.rmtree(td, ignore_errors=True)
    return bad == 0

def main():
    ap = argparse.ArgumentParser(description="Host + syntax gate for the SMP scaffolding")
    ap.add_argument("--gcc", default=None)
    ap.add_argument("--armclang", default=None)
    ap.add_argument("--la64", default=None,
                    help="LoongArch64 gcc (default: D:\\LoongIDE\\la64-tool\\bin\\loongarch64-newlib-elf-gcc.exe)")
    ap.add_argument("--rv32", default=None,
                    help="RISC-V gcc (default: D:\\Artchip\\luban-lite-master\\toolchain\\bin\\riscv64-unknown-elf-gcc.exe)")
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
    if not run_la64_pin_v(la):
        return 1
    if not run_la64_pin_r1(la):
        return 1
    if not run_la64_pin_r2(la):
        return 1
    if not run_la64_smp_sched(la):
        return 1
    if not run_la64_smp_balance(la):
        return 1

    rv = find_rv32(args.rv32)
    if not rv:
        print("SMP-GATE FAIL: RV32 gcc not found (use --rv32); a skip would be a false green")
        return 1
    if not run_rv32_smp(rv):
        return 1

    print("SMP-GATE OK (host accounting + %d ARM ON-path files + LA64 pin "
          "U/V/R1/R2/W/Y + RV32 pin X, CPU_NUM=4/2)" % len(TARGETS))
    return 0

if __name__ == "__main__":
    sys.exit(main())
