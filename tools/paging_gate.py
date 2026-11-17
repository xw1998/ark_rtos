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

E–M 每条都自带**防假绿自检**：把源串故意改坏一次，对应 pin 必须报错；
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

        for msg in (pin_e(src_pg) + pin_f(src_port) +
                    pin_g(src_hal, src_mpu) + pin_h(src_sh) +
                    pin_i(src_port) + pin_j(src_pg, src_hal, src_mpu) +
                    pin_k(src_port) + pin_l(src_asm) +
                    pin_m(src_elf, src_sh) +
                    pin_n(src_port) + pin_o(src_pg, src_sh, src_hal, src_mpu)):
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
          "U-15 映射撤销 N-O pin 成立且自检有效")
    return 0


if __name__ == "__main__":
    sys.exit(main())
