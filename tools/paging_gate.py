#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""隔离后端切换（ARKRTOS_USE_PAGING）的门禁：把两个后端都真的编一遍。

为什么需要这一道
----------------
`ARKRTOS_USE_PAGING` 默认 0，而且是「二选一」的开关：默认档编的是
`arkrtos_mpu.c`（窗口后端），`arkrtos_paging.c` 整个文件是空的 TU，一个编译器
都不经过它；反过来打开它以后 `arkrtos_mpu.c` 变成空的。于是有三种假绿：

  1. 分页后端里写错了（拼写、漏原型、忘包含），默认档全绿，直到有人在
     LoongArch 上打开开关才发现编不过；
  2. 开关其实没接线——打开也还是窗口后端在跑，读数上却写着「分页」；
  3. `arkrtos_config.h` 那三条 #error 只是注释：无页表的核也让它编过去，
     两个后端同时开也不报，最后得到一份「看着有隔离、其实没有」的镜像。

这里用真编译器（LA64 / RV32 两套交叉工具链）把上面三条都变成读数：
  A. 默认档：`kernelsrc/include/arkrtos_autoconf.h` 里符号必须是 0；
  B. LA64 正向：板级把开关置 1，`arkrtos_paging.c` 与 la64 port 必须编得过；
  C. RV32 负向：同一个 TU 强开 `-DARKRTOS_USE_PAGING=1` 必须**编不过**，
     且错误信息必须点名 `ARKRTOS_USE_PAGING` 与 `HAS_PAGING_HW`；
  D. 互斥负向：把 LA64 板级改成 `USE_MPU=1` 与 `USE_PAGING=1` 同时成立，
     必须**编不过**，且错误信息必须点名「互斥」。
  E. 动态映射接线（源码 pin）：`arkrtos_paging.c` 必须真的定义
     `arkrtos_paging_map_span()` 并调用 `arkrtos_port_paging_alias()`。
  F. port 侧不再占位：`arkrtos_port_paging_alias()` 的函数体必须走
     `la_tlb_write()`（tlbwr 确定性写条目），且不能残留在 `(void)va;`。
  G. 关档回退还在：`arkrtos_hal.h` / `arkrtos_mpu.h` 里的 `(-1)` 宏不能被
     改成 0 —— 否则默认档会把「没有分页」读成「登记成功」。
  H. shell 入口受开关约束：`pmap` 命令必须注册在 `ARKRTOS_USE_PAGING`
     条件里，且命令表容量预算里含这一条。
  I. 切换前按 VA 失效旧翻译，并重写 App 窗口那条 PS=20 条目。
  J. `arkrtos_mpu_set_app()` 必须走 `arkrtos_port_paging_switch_to()`，
     不许再直接 `activate()` —— 它跑在被换下任务自己的栈上（U-2）。
  K. port 侧「延迟换窗口」本体：switch_to 必须按 sp 是否在窗口里分岔，
     la_switch_prepare 必须清 pending、认出 new==old、补写 PRMD。
  L. 汇编两条恢复路径（eentry / switch_inline）都必须先换到恒等映射的
     静态栈，再 bl la_switch_prepare；那块栈必须落在 .bss。
  M. ELF 装载器按空间记台账（elf_slot_live / elf_copy_to_window /
     exec_space 装完恢复视图），shell 里 `pmap u2` 的判据还在。
  P. 受控访问用户地址（U-6）：`arkrtos_paging_user_copy()` 的四道闸门
     （参数/回绕、无用户窗口、空间不符、整段不在窗口内）与两个方向的
     字节循环，加 `pmap cu` 逐条判据。
  Q. invtlb 不放过窗口：QEMU 的 `invtlb 3` 实测会连带打掉 App 窗口那条
     PS=20 条目，所以 unalias 末尾必须重写它，activate 的「已是该空间」
     短路必须先查条目还在。
  R. 4 KB 页帧池（U-3）：port 有位图与连续段搜索、free 的两条拒绝码
     （-1 池外 / -3 本来没分配），la_mmu_init 给帧池补一条 PS=22 恒等映射
     （否则帧在内核里不可触及），hal / mpu.h 有 (-1) 回退，paging.c 转发到
     port，shell 有 `pmap frame` 与 `pmap ftest` 及其 PASS 串。
  S. 页级 App 窗口（U-3 第二阶）：开关三处（features.h 缺省 0 / Kconfig 两条
     depends / 板级置 1），port 里窗口几何 LA_PG_WIN_ENTRIES、组合守卫 #error、
     从帧池整块要帧（-3）、铺 256 条 PS=12 条目、逐页 invtlb、首末页抽查（-2，
     末页期望是偶页基址）、UART 改走 way 7/6、la_pg_boot_map 在 la_mmu_init 里
     真被调用，以及 PS=20 老路径仍在（两种档都在编）；hal / mpu.h / paging.c
     的 window_va 转发链与 shell 三处改用。
  T. 按需缺页（U-3 第三阶 / U-5）：开关三处（features.h 缺省 0 / Kconfig 两条
     depends / 板级 0），port 的两条 #error、按对记账 s_pg_pair_pa、缺页处理里
     「一对 8 KB = 两帧」与 tlbfill 重装、refill 专用 C 入口读 TLBRBADV、
     两条 MMIO 页改走 MTLB +12/+13（避开 tlbfill 的随机 way 与动态 alias 的
     +3..+10），以及汇编侧用 $r21 当帧基址保存全部 GPR（**不许**用 $t0 先毁
     再存）、ertn 回原指令；老路径（PS=20 窗口 + STLB way 7/6 + 不保存寄存器
     的 refill）必须仍在。另加两条**真编译**：C 侧带真编译器过一遍，
     .S 侧真汇编一遍（-fsyntax-only 不管汇编）。

E–M、P–T 每条都自带**防假绿自检**：把源串故意改坏一次，对应 pin 必须报错；
报不出错就说明这条 pin 是空转的。

不覆盖：分页后端在板上给出的实际隔离效果（哪条地址被拦、PLV 是否真降）、
跨度审计计数是否正确。那些要跑 QEMU 或上板，见
docs/QEMU模拟器移植与验证.md 的 §5.16 与 docs/内存保护与App越权验证.md。

用法
----
    python tools/paging_gate.py
    python tools/paging_gate.py --la64-gcc <path> --rv32-gcc <path>

退出码 0 = 全过；1 = 有一步不成立或找不到编译器（缺编译器是 FAIL，不是跳过）。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

LA64_FALLBACKS = [
    "D:/LoongIDE/la64-tool/bin/loongarch64-newlib-elf-gcc.exe",
]
RV32_FALLBACKS = [
    "D:/Artchip/luban-lite-master/toolchain/bin/riscv64-unknown-elf-gcc.exe",
]

LA64_ARCH = ["-march=loongarch64", "-mabi=lp64d", "-mstrict-align"]
RV32_ARCH = ["-march=rv32imac", "-mabi=ilp32", "-mcmodel=medany", "-mno-relax"]

COMMON = [
    "-O0",
    "-Wall",
    "-ffreestanding",
    "-fno-common",
    "-nostdlib",
    "-nostartfiles",
    "-fno-builtin",
    "-fsyntax-only",
    "-DARKRTOS_SIGN_KEY_LOCAL=1",
]

# 与 example/*/Makefile 里的测试密钥同值；只为了让 arkrtos_features.h 的
# 「有签名开关就必须有密钥」那条 #error 过关，不参与任何判定。
SIGN_KEY_HEADER = """#ifndef ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_SIGN_KEY_LOCAL_H
#define ARKRTOS_IMAGE_SIGN_KEY { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F }
#endif
"""


def _pick(cands, given, what):
    if given:
        return given
    for c in cands:
        if os.path.isfile(c):
            return c
    print("FAIL: 找不到 %s（缺编译器是 FAIL，不是跳过）" % what)
    sys.exit(1)


def _run(cc, args):
    cmd = [cc] + args
    r = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode, _text(r.stdout) + _text(r.stderr)


def _text(raw):
    """内核头文件是 GBK，gcc 把源里的中文原样回显，所以两种编码都要试一遍。"""
    if not raw:
        return ""
    for enc in ("utf-8", "gbk"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", "replace")


def _includes(board, port):
    return [
        "-Ikernelsrc/include",
        "-Iconfig",
        "-Iboard/" + board,
        "-Ikernelsrc/port/" + port,
    ]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--la64-gcc", default="")
    ap.add_argument("--rv32-gcc", default="")
    a = ap.parse_args()

    la64 = _pick(LA64_FALLBACKS, a.la64_gcc, "LoongArch64 交叉编译器")
    rv32 = _pick(RV32_FALLBACKS, a.rv32_gcc, "RISCV32 交叉编译器")

    tmp = tempfile.mkdtemp(prefix="ark_paging_")
    key_dir = os.path.join(tmp, "key")
    os.makedirs(key_dir)
    with open(os.path.join(key_dir, "arkrtos_sign_key_local.h"), "w",
              encoding="utf-8", newline="\n") as f:
        f.write(SIGN_KEY_HEADER)

    def board_flags(board):
        return [
            "-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
            "-include", "arkrtos_board_config.h",
            "-I" + key_dir,
        ] + _includes(board, "loongarch/la64" if board == "qemu_loongarch64"
                      else "riscv/rv32imac")

    failures = []

    # --- A. 默认档必须是 0 ------------------------------------------------
    ac = os.path.join(ROOT, "kernelsrc/include/arkrtos_autoconf.h")
    with open(ac, encoding="utf-8", errors="replace") as f:
        text = f.read()
    if "ARKRTOS_USE_PAGING 0" not in text and "ARKRTOS_USE_PAGING 0\n" not in text:
        failures.append("A 默认档：arkrtos_autoconf.h 里 ARKRTOS_USE_PAGING 不是 0")

    # --- B. LA64 正向 -----------------------------------------------------
    for rel in ("kernelsrc/src/arkrtos_paging.c",
                "kernelsrc/port/loongarch/la64/arkrtos_port.c"):
        rc, out = _run(la64, LA64_ARCH + COMMON + board_flags("qemu_loongarch64") + [rel])
        if rc != 0:
            failures.append("B LA64 编不过 %s：\n%s" % (rel, out[-500:]))

    # --- C. RV32 负向：无页表的核强开分页必须失败 -------------------------
    rv32_flags = RV32_ARCH + COMMON + \
        ["-DARKRTOS_BOARD_CONFIG=\"arkrtos_board_config.h\"",
         "-include", "arkrtos_board_config.h",
         "-I" + key_dir] + _includes("qemu_riscv32", "riscv/rv32imac")
    rc, out = _run(rv32, rv32_flags + ["-DARKRTOS_USE_PAGING=1",
                                      "kernelsrc/src/arkrtos_paging.c"])
    if rc == 0:
        failures.append("C RV32 负向：无页表的核强开 USE_PAGING 竟然编过了（守卫没生效）")
    elif ("ARKRTOS_USE_PAGING" not in out) or ("HAS_PAGING_HW" not in out):
        failures.append("C RV32 负向：编不过但不是那条守卫，报的是别的错：\n%s"
                        % out[-400:])

    # --- D. 互斥负向：两个后端同时开必须失败 -------------------------------
    src_board = os.path.join(ROOT, "board/qemu_loongarch64/arkrtos_board_config.h")
    with open(src_board, encoding="utf-8") as f:
        board_text = f.read()
    need = "#undef  ARKRTOS_USE_MPU\n#define ARKRTOS_USE_MPU              0"
    if need not in board_text:
        failures.append("D 互斥负向：板级里找不到 MPU 开关那两行，判据已过期")
    else:
        probe_dir = os.path.join(tmp, "probe")
        os.makedirs(probe_dir)
        probe = board_text.replace(
            need, "#undef  ARKRTOS_USE_MPU\n#define ARKRTOS_USE_MPU              1", 1)
        with open(os.path.join(probe_dir, "paging_probe_board.h"), "w",
                  encoding="utf-8", newline="\n") as f:
            f.write(probe)
        rc, out = _run(la64, LA64_ARCH + COMMON +
                       ["-DARKRTOS_BOARD_CONFIG=\"paging_probe_board.h\"",
                        "-include", "paging_probe_board.h",
                        "-I" + key_dir,
                        "-I" + probe_dir] +
                       _includes("qemu_loongarch64", "loongarch/la64") +
                       ["kernelsrc/src/arkrtos_paging.c"])
        if rc == 0:
            failures.append("D 互斥负向：USE_MPU=1 与 USE_PAGING=1 同时成立竟然编过了")
        elif "ARKRTOS_USE_MPU=1" not in out:
            failures.append("D 互斥负向：编不过但不是互斥那条 #error：\n%s" % out[-400:])

    # --- T. 按需缺页档必须真编得过（C 侧语法 + .S 侧真汇编） --------------
    demand_flags = board_flags("qemu_loongarch64") + \
        ["-DARKRTOS_USE_PAGING_DEMAND=1"]
    rc, out = _run(la64, LA64_ARCH + COMMON + demand_flags +
                   ["kernelsrc/port/loongarch/la64/arkrtos_port.c"])
    if rc != 0:
        failures.append("T 按需缺页档：port.c 带 DEMAND=1 编不过：\n%s" % out[-600:])
    asm_obj = os.path.join(tmp, "demand_context.o")
    rc, out = _run(la64, LA64_ARCH +
                   [f for f in COMMON if f != "-fsyntax-only"] + demand_flags +
                   ["-c", "-o", asm_obj,
                    "kernelsrc/port/loongarch/la64/arkrtos_context.S"])
    if rc != 0:
        failures.append("T 按需缺页档：arkrtos_context.S 带 DEMAND=1 汇编不过：\n%s"
                        % out[-600:])

    # --- E..H 源码 pin（每条都做一次「改坏必须报错」的自检） ---------------
    def _src(rel):
        with open(os.path.join(ROOT, rel), "rb") as f:
            return f.read().decode("utf-8", "replace")

    def _body(text, sig):
        """从签名的第一个 '{' 起按大括号配平截出函数体；找不到返回 ''。"""
        i = text.find(sig)
        if i < 0:
            return ""
        j = text.find("{", i)
        if j < 0:
            return ""
        depth = 0
        for k in range(j, len(text)):
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    return text[j:k + 1]
        return ""

    def pin_e(pg):
        bad = []
        if "int32 arkrtos_paging_map_span(" not in pg:
            bad.append("E: arkrtos_paging.c 没有定义 map_span")
        if "arkrtos_port_paging_alias(" not in _body(pg, "int32 arkrtos_paging_map_span("):
            bad.append("E: map_span 的函数体没有调用 port 的 alias")
        return bad

    def pin_f(port):
        bad = []
        if "static void la_tlb_write(" not in port:
            bad.append("F: port 里没有 la_tlb_write（确定性 tlbwr 写条目）")
        body = _body(port, "int32 arkrtos_port_paging_alias(")
        if not body:
            bad.append("F: 找不到 arkrtos_port_paging_alias 的定义")
        else:
            if "la_tlb_write(" not in body:
                bad.append("F: alias 函数体没有走 la_tlb_write")
            if "(void)va;" in body:
                bad.append("F: alias 仍是恒 -1 的占位实现")
        return bad

    def pin_g(hal, mpu):
        bad = []
        if "#define arkrtos_port_paging_alias(va, pa, mem)     (-1)" not in hal:
            bad.append("G: arkrtos_hal.h 的关档回退不再返回 -1")
        if "#define arkrtos_paging_map_span(va, pa, size, mem)           (-1)" not in mpu:
            bad.append("G: arkrtos_mpu.h 的关档回退不再返回 -1")
        return bad

    def pin_h(shell):
        bad = []
        guard = "#if (ARKRTOS_USE_PAGING == 1)"
        reg = 'ARK_SHELL_CMD("pmap", cmd_pmap'
        if reg not in shell:
            bad.append("H: shell 没有注册 pmap 命令")
        else:
            if shell.find(guard) > shell.find(reg) or shell.find(guard) < 0:
                bad.append("H: pmap 注册不在 ARKRTOS_USE_PAGING 条件里")
        if "(ARKRTOS_USE_PAGING ? 1 : 0)" not in shell:
            bad.append("H: 命令表容量预算没有算上 pmap")
        return bad

    def pin_i(port):
        bad = []
        if "static void la_tlb_inval_page(" not in port:
            bad.append("I: port 里没有 la_tlb_inval_page（切换前按 VA 失效）")
        inv = _body(port, "static void la_tlb_inval_page(")
        if "invtlb 3" not in inv:
            bad.append("I: la_tlb_inval_page 没有走 invtlb op=3")
        body = _body(port, "int32 arkrtos_port_paging_activate(")
        if not body:
            bad.append("I: 找不到 arkrtos_port_paging_activate 的定义")
        else:
            if "la_tlb_inval_page(" not in body:
                bad.append("I: activate 没有先失效旧翻译（切换会留在 softmmu 缓存里）")
            if "la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, LA_MTLB_INDEX + 2u" not in body:
                bad.append("I: activate 没有重写 App 窗口那条 PS=20 条目")
        return bad

    def pin_j(pg, hal, mpu):
        bad = []
        body = _body(pg, "void arkrtos_mpu_set_app(")
        if "arkrtos_port_paging_switch_to(" not in body:
            bad.append("J: set_app 没有切地址空间（走 switch_to）")
        # U-2：切窗口时会用的那一帧可能就压在窗口里（App 栈在窗口 VA），
        # 所以 set_app 里再出现 activate() 就是退回旧的崩法。
        if "arkrtos_port_paging_activate(" in body:
            bad.append("J: set_app 绕过 switch_to 直接 activate（U-2 会崩）")
        if "#define arkrtos_port_paging_lookup(va, index, ppn, ps) (0u)" not in hal:
            bad.append("J: arkrtos_hal.h 关档回退没有 lookup（恒 0 = 未命中）")
        if "arkrtos_paging_space_activate(" not in mpu:
            bad.append("J: arkrtos_mpu.h 没有 space_activate 接口")
        return bad

    def pin_k(port):
        bad = []
        body = _body(port, "int32 arkrtos_port_paging_switch_to(")
        if not body:
            bad.append("K: port 里没有 arkrtos_port_paging_switch_to 的定义")
        else:
            if "la_sp_in_app_window() != 0u" not in body:
                bad.append("K: switch_to 没有按 sp 是否在窗口里分岔")
            if "s_pg_pending_space = space;" not in body:
                bad.append("K: switch_to 在窗口里没有记下待装空间")
            if "arkrtos_port_paging_activate(space)" not in body:
                bad.append("K: switch_to 不在窗口里时没有立即生效")
        prep = _body(port, "void la_switch_prepare(")
        if not prep:
            bad.append("K: port 里没有 la_switch_prepare 的定义")
        else:
            if "s_pg_pending_space = LA_PG_PENDING_NONE;" not in prep:
                bad.append("K: la_switch_prepare 没有清 pending")
            if "new_frame == old_frame" not in prep:
                bad.append("K: la_switch_prepare 没认出「调度器最终没切走」")
            if "LA_FR_PRMD" not in prep:
                bad.append("K: la_switch_prepare 没补写 PRMD（C 侧写在旧窗口里）")
            if "arkrtos_port_paging_activate(space)" not in prep:
                bad.append("K: la_switch_prepare 没有真正换窗口")
        return bad

    def pin_l(asm):
        bad = []
        if asm.count("bl      la_switch_prepare") != 2:
            bad.append("L: 两条恢复路径（eentry / switch_inline）没有都调 la_switch_prepare")
        if asm.count("la.abs  $sp, la_switch_stack_top") != 2:
            bad.append("L: 恢复路径没有先换到恒等映射静态栈")
        if "la_switch_stack_top:" not in asm or ".space  2048" not in asm:
            bad.append("L: .S 里没有 la_switch_stack 那块静态栈")
        # 换栈必须发生在进 C 之前：第一条 bl la_switch_prepare 前面要有换栈。
        first = asm.find("bl      la_switch_prepare")
        if first >= 0 and asm.rfind("la.abs  $sp, la_switch_stack_top", 0, first) < 0:
            bad.append("L: 第一条恢复路径没有先换栈就进 la_switch_prepare")
        return bad

    def pin_m(elf, shell):
        bad = []
        if "static int32 elf_slot_live(" not in elf:
            bad.append("M: elf.c 里没有 elf_slot_live（按空间记台账）")
        if "static int32 elf_copy_to_window(" not in elf:
            bad.append("M: elf.c 里没有 elf_copy_to_window（先读内核暂存再写窗口）")
        body = _body(elf, "int32 arkrtos_elf_exec_space(")
        if not body:
            bad.append("M: elf.c 里没有 arkrtos_elf_exec_space 的定义")
        else:
            if "arkrtos_paging_space_activate(space)" not in body:
                bad.append("M: exec_space 装载前没有把窗口切到目标空间")
            if "arkrtos_paging_space_activate(prev)" not in body:
                bad.append("M: exec_space 装完没有把内核任务原来的视图装回去")
            if "elf_slot_live(space)" not in body:
                bad.append("M: exec_space 挑空间没有看台账")
        if "pmap u2: PASS - two instances, two spaces, neither frame overwritten" not in shell:
            bad.append("M: shell 里没有 pmap u2 的通过判据")
        return bad

    def pin_n(port):
        bad = []
        if "int32 arkrtos_port_paging_unalias(" not in port:
            bad.append("N: port 里没有 arkrtos_port_paging_unalias（U-15）")
            return bad
        body = _body(port, "int32 arkrtos_port_paging_unalias(")
        if not body:
            bad.append("N: 找不到 arkrtos_port_paging_unalias 的定义")
            return bad
        if "la_pg_static_hit(" not in body:
            bad.append("N: unalias 不拒静态跨度（撤掉内核条目 = 下一次取指 refill）")
        if "la_tlb_inval_page(" not in body:
            bad.append("N: unalias 没有失效 STLB/softmmu 里的旧翻译"
                       "（只重写 MTLB 时经该 VA 仍读到旧物理页）")
        if "la_tlb_write(" not in body:
            bad.append("N: unalias 没有把 V=0 的条目写回 machine TLB")
        if (("lo[0] == 0UL" not in body) or ("lo[1] == 0UL" not in body)):
            bad.append("N: unalias 不判两半是否都空了（会顺手回收还占着的槽位）")
        if "used = 0u" not in body:
            bad.append("N: unalias 从不回收槽位（撤销后仍报满）")
        du = _body(port, "uint32 arkrtos_port_paging_dyn_used(")
        if not du:
            bad.append("N: port 里没有 arkrtos_port_paging_dyn_used（槽位读数）")
        elif "used != 0u" not in du:
            bad.append("N: dyn_used 没有数 used 标志")
        return bad

    def pin_o(pg, shell, hal, mpu):
        bad = []
        body = _body(pg, "int32 arkrtos_paging_unmap_span(")
        if not body:
            bad.append("O: arkrtos_paging.c 里没有 arkrtos_paging_unmap_span")
        else:
            if "arkrtos_port_paging_unalias(" not in body:
                bad.append("O: unmap_span 没有调用 port 的 unalias")
            if "s_pg_map_pages -= done" not in body:
                bad.append("O: unmap_span 不结账（pages 读数会虚高）")
        du = _body(pg, "uint32 arkrtos_paging_dyn_used(")
        if (not du) or ("arkrtos_port_paging_dyn_used(" not in du):
            bad.append("O: arkrtos_paging_dyn_used 没有委托 port")
        if "#define arkrtos_port_paging_unalias(va)            (-1)" not in hal:
            bad.append("O: hal 的关档回退里没有 arkrtos_port_paging_unalias")
        if "#define arkrtos_paging_unmap_span(va, size)                  (-1)" not in mpu:
            bad.append("O: mpu.h 的关档回退里没有 arkrtos_paging_unmap_span")
        if 'strcmp(argv[1], "unmap")' not in shell:
            bad.append("O: shell 没有 pmap unmap 分支")
        elif "arkrtos_paging_unmap_span(" not in shell:
            bad.append("O: shell 的 pmap unmap 没有调 unmap_span")
        if 'strcmp(argv[1], "dyn")' not in shell:
            bad.append("O: shell 没有 pmap dyn 读数")
        elif "arkrtos_paging_dyn_used()" not in shell:
            bad.append("O: shell 的 pmap dyn 没有读 dyn_used")
        if "pmap test: PASS (map / unmap / rebind / unmap)" not in shell:
            bad.append("O: pmap test 没有把 unmap/rebind 串进判据")
        return bad

    def pin_p(pg, shell, hal, mpu):
        bad = []
        body = _body(pg, "int32 arkrtos_paging_user_copy(")
        if not body:
            bad.append("P: arkrtos_paging.c 里没有 arkrtos_paging_user_copy")
        else:
            if "(0xFFFFFFFFu - uva) < len" not in body:
                bad.append("P: user_copy 不判地址回绕（跨 4 GB 的区间会算成合法）")
            if "((rom_start == 0u) && (ram_start == 0u))" not in body:
                bad.append("P: user_copy 不拒「没有用户窗口」的任务（内核任务）")
            if "space != installed" not in body:
                bad.append("P: user_copy 不比「装着的空间」，会从别的 App 的帧里拷")
            if "p[i] = ((const uint8 *)kern)[i];" not in body:
                bad.append("P: user_copy 没有 copy_to_user 方向的字节循环")
            if "((uint8 *)kern)[i] = p[i];" not in body:
                bad.append("P: user_copy 没有 copy_from_user 方向的字节循环")
            if "if((write != 0u) ||" not in body:
                bad.append("P: user_copy 不拒「往代码窗口写」")
        fu = _body(pg, "int32 arkrtos_copy_from_user_task(")
        if not fu:
            bad.append("P: arkrtos_paging.c 里没有 arkrtos_copy_from_user_task")
        else:
            if "p_task->is_priv != 0u" not in fu:
                bad.append("P: copy_*_user_task 不拒内核任务（它没有用户窗口）")
            if "arkrtos_paging_space_of(p_task)" not in fu:
                bad.append("P: copy_*_user_task 不拿任务的绑定空间去比对")
            if "arkrtos_port_paging_space()" not in fu:
                bad.append("P: copy_*_user_task 不读当前装着的空间")
        cur = _body(pg, "int32 arkrtos_copy_from_user(void *dst")
        if not cur:
            bad.append("P: 没有 arkrtos_copy_from_user（当前任务入口）")
        elif "arkrtos_task_get_current()" not in cur:
            bad.append("P: arkrtos_copy_from_user 没有取当前任务")
        if 'strcmp(argv[1], "cu")' not in shell:
            bad.append("P: shell 没有 pmap cu 自测")
        else:
            for probe in ("range wrap", "no user window", "outside span",
                          "wrong space", "kernel caller"):
                if probe not in shell:
                    bad.append("P: pmap cu 缺 %s 这一条判据" % probe)
            if "pmap cu: PASS - from/to user, refusals -1/-2/-3/-4, kernel survived" not in shell:
                bad.append("P: shell 里没有 pmap cu 的通过判据")
        if "#define arkrtos_copy_from_user(dst, usrc, len)               (-1)" not in mpu:
            bad.append("P: mpu.h 的关档回退里没有 arkrtos_copy_from_user")
        if "#define arkrtos_copy_to_user_task(p_task, udst, src, len)    (-1)" not in mpu:
            bad.append("P: mpu.h 的关档回退里没有 arkrtos_copy_to_user_task")
        return bad

    def pin_q(port):
        bad = []
        if "static void la_pg_assert_window(void)" not in port:
            bad.append("Q: port 里没有 la_pg_assert_window（invtlb 之后重写窗口条目）")
        un = _body(port, "int32 arkrtos_port_paging_unalias(")
        if not un:
            bad.append("Q: 找不到 unalias 的定义")
        elif "la_pg_assert_window();" not in un:
            bad.append("Q: unalias 之后不重写窗口条目（QEMU 的 invtlb 3 会连带打掉 PS=20）")
        ac = _body(port, "int32 arkrtos_port_paging_activate(")
        if not ac:
            bad.append("Q: 找不到 activate 的定义")
        else:
            if "arkrtos_port_paging_lookup(ARKRTOS_QEMU_APP_BASE" not in ac:
                bad.append("Q: activate 的「已是该空间」短路不查条目还在")
            if "probed == la_pg_app_frame(space)" not in ac:
                bad.append("Q: activate 的短路不核对条目指向的帧")
        return bad

    def pin_r(pg, port, sh, hal, mpu):
        bad = []
        if "static uint32 s_fr_bits[" not in port:
            bad.append("R: port 里没有页帧位图 s_fr_bits")
        al = _body(port, "int32 arkrtos_port_paging_frame_alloc(")
        if not al:
            bad.append("R: 找不到 frame_alloc 的定义")
        elif ("la_fr_test(i) == 0u" not in al) or ("run == count" not in al):
            bad.append("R: frame_alloc 不做连续空闲段搜索（run == count 判据不在）")
        fr = _body(port, "int32 arkrtos_port_paging_frame_free(")
        if not fr:
            bad.append("R: 找不到 frame_free 的定义")
        else:
            if "return -3;" not in fr:
                bad.append("R: frame_free 对「本来没分配」不返回 -3")
            if "return -1;" not in fr:
                bad.append("R: frame_free 对「池外/未对齐」不返回 -1")
        if "la_tlb_fill(ARKRTOS_QEMU_FRAME_BASE, 22u, LA_MTLB_INDEX + 11u" not in port:
            bad.append("R: la_mmu_init 没给帧池补 PS=22 恒等映射（帧在内核里不可触及）")
        if "#define arkrtos_port_paging_frame_alloc(count, p_base)  (-1)" not in hal:
            bad.append("R: hal 的关档回退里没有 arkrtos_port_paging_frame_alloc")
        if "int32 arkrtos_paging_frame_alloc(uint32 count, uint32 *p_base);" not in mpu:
            bad.append("R: mpu.h 里没有 arkrtos_paging_frame_alloc 的声明")
        if "#define arkrtos_paging_frame_alloc(count, p_base)            (-1)" not in mpu:
            bad.append("R: mpu.h 的关档回退里没有 arkrtos_paging_frame_alloc")
        fw = _body(pg, "int32 arkrtos_paging_frame_alloc(")
        if not fw:
            bad.append("R: paging.c 里没有 frame_alloc 的包装")
        elif "arkrtos_port_paging_frame_alloc(count, p_base)" not in fw:
            bad.append("R: paging.c 的 frame_alloc 没转发到 port")
        if '"frame"' not in sh:
            bad.append("R: shell 里没有 pmap frame")
        if '"ftest"' not in sh:
            bad.append("R: shell 里没有 pmap ftest")
        if "pmap ftest: PASS (alloc/free/double-free/pattern" not in sh:
            bad.append("R: pmap ftest 的 PASS 串不在")
        return bad

    def pin_s(port, hal, mpu, pg, sh, feats, kconf, board):
        bad = []
        # 开关三处：缺省 0 / Kconfig 两条依赖 / 板级打开
        if "#define ARKRTOS_USE_PAGING_PAGES        0" not in feats:
            bad.append("S: arkrtos_features.h 的缺省值不是 0")
        if "config ARKRTOS_USE_PAGING_PAGES" not in kconf:
            bad.append("S: Kconfig.features 里没有 ARKRTOS_USE_PAGING_PAGES")
        elif kconf.count("depends on ARKRTOS_USE_PAGING") < 2:
            bad.append("S: Kconfig 的页级窗口没有同时依赖 PAGING 与 FRAMES")
        if "#define ARKRTOS_USE_PAGING_PAGES     1" not in board:
            bad.append("S: LA64 板级没有把页级窗口打开")
        # port：几何 + 组合守卫
        if "#define LA_PG_WIN_ENTRIES" not in port:
            bad.append("S: port 里没有窗口条目数几何 LA_PG_WIN_ENTRIES")
        if ('#error "ARKRTOS_USE_PAGING_PAGES=1 requires '
                'ARKRTOS_USE_PAGING_FRAMES=1"') not in port:
            bad.append("S: port 里没有「页级窗口必须配帧池」的 #error")
        body = _body(port, "static int32 la_pg_space_ensure(")
        if not body:
            bad.append("S: port 里没有 la_pg_space_ensure")
        else:
            if "arkrtos_port_paging_frame_alloc(LA_PG_WIN_PAGES, &base) != 0" not in body:
                bad.append("S: space_ensure 没有从帧池整块要帧")
            if "return -3;" not in body:
                bad.append("S: space_ensure 帧不够时不返回 -3（会留下半张窗口）")
        body = _body(port, "static void la_pg_install_space(")
        if not body:
            bad.append("S: port 里没有 la_pg_install_space")
        elif "la_tlb_write(va, 12u, LA_STLB_INDEX(va, LA_PG_WIN_WAY)," not in body:
            bad.append("S: install_space 没有按 PS=12 写窗口条目")
        body = _body(port, "static void la_pg_drop_window(")
        if not body:
            bad.append("S: port 里没有 la_pg_drop_window")
        elif "la_tlb_inval_page(" not in body:
            bad.append("S: drop_window 不逐页 invtlb（QEMU 会留旧翻译）")
        body = _body(port, "int32 arkrtos_port_paging_activate(")
        if not body:
            bad.append("S: 找不到 activate 的定义")
        else:
            if "return -2;" not in body:
                bad.append("S: activate 装完不抽查首末页（-2 不在）")
            if "(s_pg_space_base[space] + ARKRTOS_QEMU_APP_SIZE - 0x2000u)" not in body:
                bad.append("S: activate 的末页期望值不是偶页基址（TLBELO0 口径）")
            if "(probed_ps != 12u)" not in body:
                bad.append("S: activate 抽查不核对 ps=12")
        if "la_pg_boot_map();" not in port:
            bad.append("S: la_mmu_init 没有调 la_pg_boot_map（第一条 App 取指就 refill）")
        if "LA_STLB_INDEX(ARKRTOS_QEMU_UART_BASE, LA_PG_KUART_WAY)" not in port:
            bad.append("S: 内核 UART 没有固定 way（会顶掉窗口条目）")
        if "LA_STLB_INDEX(ARKRTOS_QEMU_APP_UART, LA_PG_AUART_WAY)" not in port:
            bad.append("S: App UART 没有固定 way（会顶掉窗口条目）")
        # 两种映射方式都要在源码里：默认关时仍编 PS=20 那条
        if port.count("la_tlb_write(ARKRTOS_QEMU_APP_BASE, 20u, "
                      "LA_MTLB_INDEX + 2u") < 2:
            bad.append("S: PS=20 老路径不见了（默认档会没有窗口映射）")
        # window_va 转发链
        if "uint32 arkrtos_port_paging_window_va(void);" not in hal:
            bad.append("S: hal 里没有 window_va 的声明")
        if "#define arkrtos_port_paging_window_va()                 (0u)" not in hal:
            bad.append("S: hal 的关档回退里没有 window_va")
        if "uint32 arkrtos_paging_window_va(void);" not in mpu:
            bad.append("S: mpu.h 里没有 window_va 的声明")
        if "#define arkrtos_paging_window_va()                           (0u)" not in mpu:
            bad.append("S: mpu.h 的关档回退里没有 window_va")
        wv = _body(pg, "uint32 arkrtos_paging_window_va(")
        if (not wv) or ("arkrtos_port_paging_window_va()" not in wv):
            bad.append("S: paging.c 的 window_va 没有转发到 port")
        if sh.count("arkrtos_paging_window_va()") < 3:
            bad.append("S: shell 里少于三处改用 window_va（u1/u2/cu 有硬写窗口址的）")
        return bad

    for rel in ("kernelsrc/src/arkrtos_paging.c",
                "kernelsrc/port/loongarch/la64/arkrtos_port.c",
                "kernelsrc/port/loongarch/la64/arkrtos_context.S",
                "kernelsrc/include/arkrtos_hal.h",
                "kernelsrc/include/arkrtos_mpu.h",
                "kernelsrc/include/arkrtos_elf.h",
                "kernelsrc/src/arkrtos_elf.c",
                "kernelsrc/shell/arkrtos_shell.c"):
        if not os.path.isfile(os.path.join(ROOT, rel)):
            failures.append("E-M: 缺文件 %s" % rel)

    if not failures:
        src_pg = _src("kernelsrc/src/arkrtos_paging.c")
        src_port = _src("kernelsrc/port/loongarch/la64/arkrtos_port.c")
        src_asm = _src("kernelsrc/port/loongarch/la64/arkrtos_context.S")
        src_hal = _src("kernelsrc/include/arkrtos_hal.h")
        src_mpu = _src("kernelsrc/include/arkrtos_mpu.h")
        src_elf = _src("kernelsrc/src/arkrtos_elf.c")
        src_sh = _src("kernelsrc/shell/arkrtos_shell.c")
        src_feats = _src("kernelsrc/include/arkrtos_features.h")
        src_kconf = _src("config/kconfig/Kconfig.features")
        src_board = _src("board/qemu_loongarch64/arkrtos_board_config.h")

    def pin_t(port, asm, feats, kconf, board):
        bad = []
        # 开关三处
        if "#define ARKRTOS_USE_PAGING_DEMAND       0" not in feats:
            bad.append("T: arkrtos_features.h 里按需缺页的缺省值不是 0")
        if "config ARKRTOS_USE_PAGING_DEMAND" not in kconf:
            bad.append("T: Kconfig.features 里没有 ARKRTOS_USE_PAGING_DEMAND")
        else:
            k = kconf.index("config ARKRTOS_USE_PAGING_DEMAND")
            nk = kconf.find("\nconfig ", k + 1)
            blk = kconf[k:nk] if nk > 0 else kconf[k:]
            if ("depends on ARKRTOS_USE_PAGING\n" not in blk) or \
               ("depends on ARKRTOS_USE_PAGING_PAGES" not in blk):
                bad.append("T: Kconfig 的按需缺页没有同时依赖 PAGING 与 PAGES")
        if "#define ARKRTOS_USE_PAGING_DEMAND    0" not in board:
            bad.append("T: LA64 板级不该缺省打开按需缺页（应为 0，靠 EXTRA_CFLAGS 出镜像）")
        # port：两条组合守卫
        if ('#error "ARKRTOS_USE_PAGING_DEMAND=1 requires '
                'ARKRTOS_USE_PAGING_PAGES=1"') not in port:
            bad.append("T: port 里没有「按需缺页必须配页级窗口」的 #error")
        if ('#error "ARKRTOS_USE_PAGING_DEMAND=1 requires '
                'ARKRTOS_USE_PRIV=1"') not in port:
            bad.append("T: port 里没有「按需缺页必须配特权级」的 #error")
        # 按对记账
        if "static uint32 s_pg_pair_pa[" not in port:
            bad.append("T: port 里没有按对记账 s_pg_pair_pa（真相不在 space_base）")
        # 缺页处理
        fault = _body(port, "static int32 la_pg_demand_fault(")
        if not fault:
            bad.append("T: port 里没有 la_pg_demand_fault")
        else:
            if "arkrtos_port_paging_frame_alloc(2u, &pa) != 0" not in fault:
                bad.append("T: 缺页不给「一对 = 两帧」要帧（一条 PS=12 覆盖两页）")
            if "s_pg_pair_pa[space][idx] = pa;" not in fault:
                bad.append("T: 缺页拿到帧之后不记账")
            if "la_tlb_fill_refill(va," not in fault:
                bad.append("T: 缺页没用重填上下文的装条目函数（tlbwr 在这里会装出 V=0）")
        fill = _body(port, "static void la_tlb_fill_refill(")
        if not fill:
            bad.append("T: port 里没有 la_tlb_fill_refill")
        else:
            if "LA_CSRW(LA_CSR_TLBREHI" not in fill:
                bad.append("T: 重填装条目不写 TLBREHI（ISTLBR=1 时只认 TLBR* 镜像 CSR）")
            if '"tlbfill"' not in fill:
                bad.append("T: 重填装条目没用 tlbfill")
        rc = _body(port, "int32 la_refill_demand_c(")
        if not rc:
            bad.append("T: port 里没有 la_refill_demand_c")
        elif "LA_CSRR(LA_CSR_TLBRBADV" not in rc:
            bad.append("T: 按需 refill 不读 TLBRBADV（读 TLBEHI 在重填上下文里是错的）")
        # MMIO 页必须让开 tlbfill 的随机 way
        if "LA_MTLB_INDEX + 12u" not in port:
            bad.append("T: 内核 UART 没有改走 MTLB +12（按需档 way 随机，STLB 会撞窗口）")
        if "LA_MTLB_INDEX + 13u" not in port:
            bad.append("T: App UART 别名没有改走 MTLB +13")
        # 老路径必须还在（两种档都要在编）
        if "LA_STLB_INDEX(ARKRTOS_QEMU_UART_BASE, LA_PG_KUART_WAY)" not in port:
            bad.append("T: 页级窗口老档的 UART way 7 分支不见了")
        # 汇编侧
        if "la.abs  $r21, la_refill_stack_top" not in asm:
            bad.append("T: refill 入口不用 $r21 当帧基址")
        if "la.abs  $t0, la_refill_stack_top" in asm:
            bad.append("T: refill 入口又拿 $t0 当帧基址（先毁再存，被打断的 $t0 回不来）")
        if "st.d    $t0, $r21, 88" not in asm:
            bad.append("T: refill 入口不保存 $t0（ertn 回来源/目的指针会整段偏掉）")
        if "bl      la_refill_demand_c" not in asm:
            bad.append("T: refill 入口没有调 la_refill_demand_c")
        if "ld.d    $sp, $sp, 16" not in asm:
            bad.append("T: refill 入口没有最后恢复原 sp（先算地址再写）")
        if "ertn" not in asm:
            bad.append("T: refill 入口没有 ertn（按需缺页必须回出错的那条指令重试）")
        if "la.abs  $sp, la_refill_stack_top\n    bl      la_refill_c" not in asm:
            bad.append("T: 老档的 refill 入口（换静态栈 + la_refill_c）不见了")
        return bad

    def pin_u(port, pg, sh, hal, mpu, elf):
        bad = []
        body = _body(port, "int32 arkrtos_port_paging_space_release(")
        if not body:
            bad.append("U: port 里没有 arkrtos_port_paging_space_release（空间回收）")
        else:
            if "space == s_pg_app_space" not in body:
                bad.append("U: 回收不看「这个空间是不是正装着」")
            if "la_pg_drop_window();" not in body:
                bad.append("U: 回收前不撤窗口条目（帧回池后旧条目仍指着它 = 两空间共享物理页）")
            if "la_sp_in_app_window() != 0u" not in body:
                bad.append("U: 回收不拒「调用者就站在窗口里」（撤条目后 ret 直接 refill）")
            if "return -3;" not in body:
                bad.append("U: 站在窗口里时不返回 -3")
            if "arkrtos_port_paging_frame_free(s_pg_space_base[space]," not in body:
                bad.append("U: 页级档不把整块空间帧还回池子")
            if "LA_PG_WIN_PAGES" not in body:
                bad.append("U: 归还帧数不是窗口那一整块（LA_PG_WIN_PAGES）")
            if "arkrtos_port_paging_frame_free(pa, 2u)" not in body:
                bad.append("U: 按需档不按「一对 = 两帧」归还")
            if "s_pg_pair_pa[space][i] = 0u;" not in body:
                bad.append("U: 按需档还了帧不销账（下一次缺页会拿到已回收的帧）")
            if "return -2;" not in body:
                bad.append("U: 没有帧池的档不返回 -2（「本来就没有」与「已归还」混成一个答案）")
        if "int32 arkrtos_port_paging_space_release(uint32 space);" not in hal:
            bad.append("U: hal 里没有 space_release 的声明")
        if "#define arkrtos_port_paging_space_release(space)   (-1)" not in hal:
            bad.append("U: hal 的关档回退里没有 space_release")
        if "int32 arkrtos_paging_space_release(uint32 space);" not in mpu:
            bad.append("U: mpu.h 里没有 space_release 的声明")
        if "#define arkrtos_paging_space_release(space)                  (-1)" not in mpu:
            bad.append("U: mpu.h 的关档回退里没有 space_release")
        w = _body(pg, "int32 arkrtos_paging_space_release(")
        if (not w) or ("arkrtos_port_paging_space_release(space)" not in w):
            bad.append("U: paging.c 的 space_release 没转发到 port")
        eb = _body(elf, "int32 arkrtos_elf_exec_space(")
        if not eb:
            bad.append("U: 找不到 arkrtos_elf_exec_space 的定义")
        else:
            i_rel = eb.find("arkrtos_paging_space_release(space)")
            i_act = eb.find("arkrtos_paging_space_activate(space)")
            if i_rel < 0:
                bad.append("U: exec_space 装载前不回收该空间（反复 exec 会吃干帧池）")
            elif (i_act >= 0) and (i_rel > i_act):
                bad.append("U: exec_space 先 activate 后回收（回收会把刚铺好的窗口撤掉）")
        if 'strcmp(argv[1], "rel")' not in sh:
            bad.append("U: shell 没有 pmap rel 分支")
        elif "arkrtos_paging_space_release(want)" not in sh:
            bad.append("U: shell 的 pmap rel 没有调 space_release")
        if "pmap rel: space %u rc=%d frames %u -> %u" not in sh:
            bad.append("U: pmap rel 不打印回收前后的帧数（没有读数就不是判据）")
        return bad

        for msg in (pin_e(src_pg) + pin_f(src_port) +
                    pin_g(src_hal, src_mpu) + pin_h(src_sh) +
                    pin_i(src_port) + pin_j(src_pg, src_hal, src_mpu) +
                    pin_k(src_port) + pin_l(src_asm) +
                    pin_m(src_elf, src_sh) +
                    pin_n(src_port) + pin_o(src_pg, src_sh, src_hal, src_mpu) +
                    pin_p(src_pg, src_sh, src_hal, src_mpu) +
                    pin_q(src_port) + pin_r(src_pg, src_port, src_sh, src_hal, src_mpu) +
                    pin_s(src_port, src_hal, src_mpu, src_pg, src_sh,
                          src_feats, src_kconf, src_board) +
                    pin_t(src_port, src_asm, src_feats, src_kconf, src_board) +
                    pin_u(src_port, src_pg, src_sh, src_hal, src_mpu, src_elf)):
            failures.append("源码 pin 不成立 " + msg)

        # 自检：把每条 pin 依赖的那一句改坏，对应 pin 必须报错
        selfchecks = [
            ("E", pin_e(src_pg.replace("arkrtos_port_paging_alias(",
                                       "arkrtos_port_paging_alias_X(", 1))),
            ("F", pin_f(src_port.replace("la_tlb_write(pair, 12u",
                                         "la_tlb_write_X(pair, 12u"))),
            ("G", pin_g(src_hal.replace(
                "#define arkrtos_port_paging_alias(va, pa, mem)     (-1)",
                "#define arkrtos_port_paging_alias(va, pa, mem)     (0)", 1), src_mpu)),
            ("H", pin_h(src_sh.replace('ARK_SHELL_CMD("pmap", cmd_pmap',
                                       'ARK_SHELL_CMD("pmapX", cmd_pmap', 1))),
            ("I", pin_i(src_port.replace("la_tlb_inval_page(",
                                         "la_tlb_inval_page_X("))),
            ("J", pin_j(src_pg.replace("arkrtos_port_paging_switch_to(",
                                       "arkrtos_port_paging_switch_to_X(", 1),
                         src_hal, src_mpu)),
            # J 的反向断言：把 set_app 里的 switch_to 改回 activate，必须报错
            ("J-", pin_j(src_pg.replace("(void)arkrtos_port_paging_switch_to(",
                                        "(void)arkrtos_port_paging_activate(", 1),
                          src_hal, src_mpu)),
            ("K", pin_k(src_port.replace("if(la_sp_in_app_window() != 0u)",
                                         "if(la_sp_in_app_window_X() != 0u)", 1))),
            ("L", pin_l(src_asm.replace("bl      la_switch_prepare",
                                         "bl      la_switchXprepare", 1))),
            ("M", pin_m(src_elf.replace("arkrtos_paging_space_activate(prev)",
                                         "arkrtos_paging_space_activate_X(prev)", 1),
                         src_sh)),
            ("N", pin_n(src_port.replace("int32 arkrtos_port_paging_unalias(",
                                         "int32 arkrtos_port_paging_unalias_X(", 1))),
            ("O", pin_o(src_pg.replace("arkrtos_port_paging_unalias(va + off)",
                                       "arkrtos_port_paging_unalias_X(va + off)", 1),
                         src_sh, src_hal, src_mpu)),
            ("P", pin_p(src_pg.replace("(0xFFFFFFFFu - uva)",
                                       "(0xFFFFFFFFu - uvaMut)", 1),
                         src_sh, src_hal, src_mpu)),
            ("P-", pin_p(src_pg.replace("((uint8 *)kern)[i] = p[i];",
                                        "((uint8 *)kern)[i] = p[i + 1];", 1),
                          src_sh, src_hal, src_mpu)),
            ("Q", pin_q(src_port.replace("    la_pg_assert_window();",
                                          "    la_pg_assert_windowX();", 1))),
            ("R", pin_r(src_pg,
                        src_port.replace("run == count", "run =X= count", 1),
                        src_sh, src_hal, src_mpu)),
            ("R-", pin_r(src_pg, src_port,
                         src_sh.replace('pmap ftest: PASS (alloc/free/double-free/pattern',
                                        'pmap ftest: PASSX (alloc/free/double-free/pattern', 1),
                         src_hal, src_mpu)),
            ("S", pin_s(src_port.replace("#define LA_PG_WIN_ENTRIES",
                                          "#define LA_PG_WIN_ENTRXIES", 1),
                         src_hal, src_mpu, src_pg, src_sh,
                         src_feats, src_kconf, src_board)),
            ("S-", pin_s(src_port.replace(
                "arkrtos_port_paging_frame_alloc(LA_PG_WIN_PAGES, &base) != 0",
                "arkrtos_port_paging_frame_alloc(LA_PG_WIN_PAGEZ, &base) != 0", 1),
                src_hal, src_mpu, src_pg, src_sh,
                src_feats, src_kconf, src_board)),
            ("T", pin_t(src_port.replace("static uint32 s_pg_pair_pa[",
                                          "static uint32 s_pg_pair_pX[", 1),
                        src_asm, src_feats, src_kconf, src_board)),
            ("U", pin_u(src_port.replace(
                "arkrtos_port_paging_frame_free(s_pg_space_base[space],",
                "arkrtos_port_paging_frame_free(s_pg_space_base[space]X,", 1),
                src_pg, src_sh, src_hal, src_mpu, src_elf)),
            ("U-", pin_u(src_port, src_pg, src_sh, src_hal, src_mpu,
                         src_elf.replace("(void)arkrtos_paging_space_release(space);",
                                         "", 1))),
            ("T-", pin_t(src_port,
                         src_asm.replace("la.abs  $r21, la_refill_stack_top",
                                          "la.abs  $t0, la_refill_stack_top", 1),
                         src_feats, src_kconf, src_board)),
        ]
        for tag, got in selfchecks:
            if not got:
                failures.append("自检 %s：把源改坏后这条 pin 仍然通过（pin 是空转的）" % tag)

    shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1

    print("PAGING-GATE OK: 默认档=0 / LA64 打开编得过(2 TU) / RV32 负向被拒 / "
          "两后端互斥被拒 / 动态映射接线 E-H pin 成立且自检有效 / "
          "每 App 地址空间 I-J pin 成立且自检有效 / "
          "U-2 延迟换窗口 K-M pin 成立且自检有效 / "
          "U-15 映射撤销 N-O pin 成立且自检有效 / "
          "U-6 受控用户拷贝 P pin 与 invtlb 后重写窗口 Q pin 成立且自检有效 / "
          "U-3 页帧池 R pin 成立且自检有效 / "
          "U-3 第二阶页级窗口 S pin 成立且自检有效 / "
          "U-3 第三阶按需缺页 T pin 成立且自检有效（C 侧真编、.S 侧真汇编） / "
          "U-3 空间回收 U pin 成立且自检有效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
